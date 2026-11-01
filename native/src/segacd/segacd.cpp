// Sega CD: construção, mapa de memória dos dois 68000, Word RAM, save, save state e o núcleo.
// Registradores, interrupções e DMA em gatearray.cpp; CDC em lc8951.cpp; drive em cddrive.cpp; gráficos em
// asic.cpp; PCM e som em pcm.cpp.
#include "segacd.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::segacd {

namespace {
const char STATE_MAGIC[4] = {'I', 'S', 'C', '2'};  // versão 2: estado reorganizado por peça (a 1 é recusada)

// Imagem de células da Word RAM 1M (principal, 220000-23FFFF; manual do Mega-CD, "cell image"): o banco do
// principal (128 KB) é visto como 5 faixas de uma imagem de 512 pontos (256 bytes) de largura, com 256, 128,
// 64, 32 e 32 linhas. Na imagem de células os mesmos bytes aparecem como padrões de 8x8 do VDP em colunas: cada
// coluna de células tem a altura da faixa e as colunas seguem da esquerda para a direita. Converte o
// deslocamento na imagem de células no deslocamento linear do banco.
uint32_t cellImageToLinear(uint32_t off) {
  static const uint32_t bandStart[6] = {0x00000, 0x10000, 0x18000, 0x1C000, 0x1E000, 0x20000};
  int b = 0;
  while (off >= bandStart[b + 1]) b++;
  const uint32_t rel = off - bandStart[b];
  const uint32_t cellsHigh = (bandStart[b + 1] - bandStart[b]) / 2048;  // 256 bytes x 8 linhas por célula
  const uint32_t cell = rel >> 5, row = (rel >> 2) & 7;
  const uint32_t cx = cell / cellsHigh, cy = cell % cellsHigh;
  return bandStart[b] + (cy * 8 + row) * 256 + cx * 4 + (rel & 3);
}

struct CellImageTable {
  uint32_t lin[0x8000];  // por palavra longa (4 bytes) da imagem de células
  CellImageTable() { for (uint32_t i = 0; i < 0x8000; i++) lin[i] = cellImageToLinear(i * 4); }
};
const CellImageTable& cellTable() { static const CellImageTable t; return t; }
}  // namespace

bool isSegaCdBios(const std::vector<uint8_t>& b) {
  if (b.size() != 0x20000) return false;
  if (std::memcmp(&b[0x100], "SEGA", 4) != 0) return false;
  if (b[0x180] == 'B' && b[0x181] == 'R') return true;  // número de série "BR 000006-..." das BIOS do CD
  const std::string nome(reinterpret_cast<const char*>(&b[0x120]), 0x60);
  return nome.find("BOOT") != std::string::npos;  // "MEGA-CD BOOT ROM", "WONDER-MEGA BOOT", "CDX BOOT ROM"...
}

namespace {
std::string regionOf(const uint8_t* h) {  // cabeçalho do Mega Drive (h = byte 0x1F0): "JUE" ou o código novo em hexa
  const char r0 = static_cast<char>(h[0]);
  if (((r0 >= '0' && r0 <= '9') || (r0 >= 'A' && r0 <= 'F' && r0 != 'E')) && (h[1] == ' ' || h[1] == 0)) {
    const int v = r0 <= '9' ? r0 - '0' : r0 - 'A' + 10;
    return (v & 4) ? "us" : (v & 8) ? "eu" : (v & 1) ? "jp" : "";
  }
  bool j = false, u = false, e = false;
  for (int i = 0; i < 3; i++) { j |= h[i] == 'J'; u |= h[i] == 'U'; e |= h[i] == 'E'; }
  return u ? "us" : e ? "eu" : j ? "jp" : "";
}
}  // namespace

std::string segaCdBiosRegion(const std::vector<uint8_t>& b) { return b.size() >= 0x200 ? regionOf(&b[0x1F0]) : ""; }

std::string segaCdDiscRegion(cdrom::Disc& d) {
  uint8_t s[2048];
  return d.readData(0, s) ? regionOf(&s[0x1F0]) : "";
}

std::string segaCdFirmwareName(const std::vector<uint8_t>& b) {
  const std::string r = segaCdBiosRegion(b);
  return r.empty() ? "segacd" : "segacd_" + r;
}

// ------------------------------------------------------------ construção
SegaCD::SegaCD(md::MD& m, std::vector<uint8_t> bios, std::unique_ptr<cdrom::Disc> disc, bool ramCart)
    : subBus_(std::make_unique<SubBus>(this)), sub(*subBus_), md_(m), bios_(std::move(bios)), prg_(0x80000),
      wram_(0x40000), bram_(0x2000), disc_(std::move(disc)) {
  if (bios_.size() != 0x20000) throw std::runtime_error("BIOS do Sega CD inválida (precisa ter 128 KB).");
  if (!disc_) throw std::runtime_error("Disco do Sega CD inválido.");
  if (ramCart) { cartId_ = 4; cart_.assign(size_t(1) << 17, 0); }  // cartucho de RAM de backup de 128 KB
  // o MD entrega ao CD 000000-7FFFFF (BIOS, PRG-RAM, Word RAM, porta do cartucho) e A12000-A120FF
  for (int p = 0; p < 0x80; p++) page[p] = true;
  page[0xA1] = true;
  ioLo = 0x2000; ioHi = 0x20FF;
  scdPerLine_ = md_.pal ? 3214 : 3184;
  drive_.attach(disc_.get());
  // título: nome do jogo no cabeçalho do disco (0x150, internacional; senão 0x120), senão o nome do arquivo
  uint8_t sec[2048];
  if (disc_->readData(0, sec)) {
    for (int base : {0x150, 0x120}) {
      std::string tt;
      for (int i = 0; i < 48; i++) { const char c = static_cast<char>(sec[base + i]); tt += (c >= 32 && c < 127) ? c : ' '; }
      std::string r;
      for (char c : tt) if (!(c == ' ' && (r.empty() || r.back() == ' '))) r += c;
      while (!r.empty() && r.back() == ' ') r.pop_back();
      if (!r.empty()) { discTitle = r; break; }
    }
  }
  if (discTitle.empty()) discTitle = disc_->name();
  formatBackup();
  reset();
}

SegaCD::~SegaCD() = default;

void SegaCD::reset() {
  resetBusreq_ = 2;  // sub-CPU em reset, SBRQ lido em 1
  ifl2_ = false;
  hint_ = 0xFFFF;
  wp_ = bank_ = pm_ = 0;
  mode1M_ = false; ret_ = true; dmna_ = false; swapAsked_ = false;  // 2M, Word RAM do principal
  commMain_ = commSub_ = 0;
  std::memset(commCmd_, 0, sizeof commCmd_);
  std::memset(commStatus_, 0, sizeof commStatus_);
  led_ = 0;
  std::fill(prg_.begin(), prg_.end(), 0);
  std::fill(wram_.begin(), wram_.end(), 0);
  subHalt_ = true; subWaiting_ = false;
  subClk_ = 0; frameScd_ = 0; pcmClk_ = 0;
  sub.reset();  // registradores num estado conhecido; os vetores são lidos de novo quando o principal soltar o reset
  sub.cycles = 0;
  subReset_ = true;
  cartProt_ = 1;
  peripheralReset();
  pcmBuf_.clear(); pcmLast_[0] = pcmLast_[1] = cddaLast_[0] = cddaLast_[1] = 0; cddaFrac_ = 0;
  hpX_[0] = hpX_[1] = hpY_[0] = hpY_[1] = 0;
  audL_.clear(); audR_.clear(); audPos_ = 0;
}

// ------------------------------------------------------------ Word RAM
// Modos de prioridade (PM) das escritas do sub-CPU na imagem de pontos e do ASIC, ponto a ponto (4 bits):
// 0 escreve sempre, 1 só onde o ponto antigo é 0 ("por baixo"), 2 só pontos novos diferentes de 0 ("por cima")
uint8_t SegaCD::blend(uint8_t old, uint8_t fresh) const {
  if (pm_ == 0) return fresh;
  if (pm_ == 3) return old;
  uint8_t out = 0;
  for (int sh = 0; sh <= 4; sh += 4) {
    const uint8_t o = (old >> sh) & 0xF, n = (fresh >> sh) & 0xF;
    const uint8_t pick = pm_ == 1 ? (o ? o : n) : (n ? n : o);
    out = static_cast<uint8_t>(out | (pick << sh));
  }
  return out;
}

// No 2M a memória é uma só; no 1M cada CPU vê um banco de 128 KB. Os dois bancos são as palavras alternadas da
// memória de 256 KB (os dois chips de 128K x 16): o banco 0 fica com as palavras pares e o 1 com as ímpares.
uint8_t SegaCD::wordRamMain(uint32_t a) {
  a &= 0x3FFFF;
  if (!mode1M_) return ret_ ? wram_[a] : static_cast<uint8_t>((a & 1) ? md_.cpu.irc : md_.cpu.irc >> 8);
  const uint32_t off = a < 0x20000 ? a : cellTable().lin[(a >> 2) & 0x7FFF] | (a & 3);
  return wram_[interleave(mainHalf(), off)];
}

void SegaCD::wordRamMainWrite(uint32_t a, uint8_t v) {
  a &= 0x3FFFF;
  if (!mode1M_) { if (ret_) wram_[a] = v; return; }
  const uint32_t off = a < 0x20000 ? a : cellTable().lin[(a >> 2) & 0x7FFF] | (a & 3);
  wram_[interleave(mainHalf(), off)] = v;
}

// ------------------------------------------------------------ 68000 principal
// 000000-01FFFF BIOS (o vetor da interrupção 4 em 000070 vira FFFF:HINT), 020000-03FFFF janela de 128 KB da
// PRG-RAM (banco BK; só com o sub parado pelo SBRQ), espelhados até 1FFFFF; 200000-23FFFF Word RAM;
// 400000-7FFFFF cartucho de RAM de backup; A12000 registradores.
uint8_t SegaCD::read8(uint32_t a) {
  if (a < 0x200000) {
    const uint32_t o = a & 0x3FFFF;
    if (o < 0x20000) {
      if ((o & 0x1FFFC) == 0x70) return (o & 2) ? static_cast<uint8_t>((o & 1) ? hint_ : hint_ >> 8) : 0xFF;
      return bios_[o];
    }
    if (!(resetBusreq_ & 2)) return static_cast<uint8_t>((a & 1) ? md_.cpu.irc : md_.cpu.irc >> 8);
    return prg_[(static_cast<uint32_t>(bank_) << 17) | (o & 0x1FFFF)];
  }
  if (a < 0x400000) return wordRamMain(a);
  if (a < 0x800000) {
    if (!cartId_ || !(a & 1)) return 0xFF;
    if (a < 0x600000) return cartId_;
    if (a < 0x700000) return cart_[(a >> 1) & (cart_.size() - 1)];
    return cartProt_;
  }
  return mainRegRead8(a);
}

uint16_t SegaCD::read16(uint32_t a) {
  if (a < 0x200000) {
    const uint32_t o = a & 0x3FFFF;
    if (o < 0x20000) {
      if ((o & 0x1FFFC) == 0x70) return (o & 2) ? hint_ : 0xFFFF;
      return static_cast<uint16_t>((bios_[o] << 8) | bios_[o + 1]);
    }
    if (!(resetBusreq_ & 2)) return md_.cpu.irc;
    const uint32_t p = (static_cast<uint32_t>(bank_) << 17) | (o & 0x1FFFF);
    return static_cast<uint16_t>((prg_[p] << 8) | prg_[p + 1]);
  }
  if (a < 0x400000) return static_cast<uint16_t>((wordRamMain(a) << 8) | wordRamMain(a + 1));
  if (a < 0x800000) return static_cast<uint16_t>(0xFF00 | read8(a | 1));
  return mainRegRead16(a);
}

void SegaCD::write8(uint32_t a, uint8_t v) {
  if (a < 0x200000) {
    const uint32_t o = a & 0x3FFFF;
    if (o >= 0x20000 && (resetBusreq_ & 2)) prg_[(static_cast<uint32_t>(bank_) << 17) | (o & 0x1FFFF)] = v;
    return;
  }
  if (a < 0x400000) { wordRamMainWrite(a, v); return; }
  if (a < 0x800000) {
    if (!cartId_ || !(a & 1)) return;
    if (a >= 0x700000) { cartProt_ = v; return; }
    if (a >= 0x600000 && (cartProt_ & 1)) {
      uint8_t& c = cart_[(a >> 1) & (cart_.size() - 1)];
      if (c != v) { c = v; dirty_ = true; }
    }
    return;
  }
  mainRegWrite8(a, v);
}

void SegaCD::write16(uint32_t a, uint16_t v) {
  if (a < 0x400000) { write8(a, static_cast<uint8_t>(v >> 8)); write8(a + 1, static_cast<uint8_t>(v)); return; }
  if (a < 0x800000) { write8(a | 1, static_cast<uint8_t>(v)); return; }
  mainRegWrite16(a, v);
}

// DMA do VDP: da Word RAM o dado chega um ciclo atrasado (a primeira palavra é a que estava no barramento e as
// seguintes vêm deslocadas de uma); os jogos somam 2 ao endereço de origem
uint16_t SegaCD::dmaRead(uint32_t a) {
  if (a >= 0x200000 && a < 0x400000) a -= 2;
  return read16(a);
}

// ------------------------------------------------------------ sub-CPU
// 000000-07FFFF PRG-RAM (os primeiros WP * 512 bytes protegidos); 080000-0BFFFF Word RAM 2M, ou no 1M a imagem
// de pontos (um ponto de 4 bits por endereço); 0C0000-0DFFFF banco 1M do sub; FE0000-FEFFFF RAM de backup (bytes
// ímpares); FF0000-FF7FFF PCM (bytes ímpares); FF8000 registradores.
namespace {
inline bool prgProtected(uint32_t a, uint8_t wp) { return a < 0x20000 && a < (static_cast<uint32_t>(wp) << 9); }
}  // namespace

uint8_t SegaCD::subRead8(uint32_t a) {
  a &= 0xFFFFF;
  if (a < 0x80000) return prg_[a];
  if (a < 0xC0000) {
    if (mode1M_) {
      const uint8_t d = wram_[interleave(subHalf(), (a >> 1) & 0x1FFFF)];
      return (a & 1) ? (d & 0x0F) : (d >> 4);
    }
    if (ret_) { subWaiting_ = true; throw SubWait{}; }
    return wram_[a & 0x3FFFF];
  }
  if (a < 0xE0000) {
    if (mode1M_) return wram_[interleave(subHalf(), a & 0x1FFFF)];
    return static_cast<uint8_t>((a & 1) ? sub.irc : sub.irc >> 8);
  }
  if (a < 0xF0000) return (a & 1) ? bram_[(a >> 1) & 0x1FFF] : 0xFF;
  if (!(a & 0x8000)) return (a & 1) ? pcmRead((a >> 1) & 0x1FFF) : static_cast<uint8_t>(sub.irc >> 8);
  return subRegRead8(a & 0x1FF);
}

uint16_t SegaCD::subRead16(uint32_t a) {
  a &= 0xFFFFE;
  if (a < 0x80000) return static_cast<uint16_t>((prg_[a] << 8) | prg_[a + 1]);
  if (a < 0xC0000) {
    if (mode1M_) {  // dois pontos, cada um nos 4 bits de baixo de um byte
      const uint8_t d = wram_[interleave(subHalf(), (a >> 1) & 0x1FFFF)];
      return static_cast<uint16_t>(((d >> 4) << 8) | (d & 0x0F));
    }
    if (ret_) { subWaiting_ = true; throw SubWait{}; }
    const uint32_t o = a & 0x3FFFF;
    return static_cast<uint16_t>((wram_[o] << 8) | wram_[o + 1]);
  }
  if (a < 0xE0000) {
    if (mode1M_) {
      const uint32_t o = interleave(subHalf(), a & 0x1FFFF);
      return static_cast<uint16_t>((wram_[o] << 8) | wram_[o + 1]);
    }
    return sub.irc;
  }
  if (a < 0xF0000) return static_cast<uint16_t>(0xFF00 | bram_[(a >> 1) & 0x1FFF]);
  if (!(a & 0x8000)) return pcmRead((a >> 1) & 0x1FFF);
  return subRegRead16(a & 0x1FE);
}

void SegaCD::subWrite8(uint32_t a, uint8_t v) {
  a &= 0xFFFFF;
  if (a < 0x80000) { if (!prgProtected(a, wp_)) prg_[a] = v; return; }
  if (a < 0xC0000) {
    if (mode1M_) {
      uint8_t& b = wram_[interleave(subHalf(), (a >> 1) & 0x1FFFF)];
      const uint8_t fresh = (a & 1) ? static_cast<uint8_t>((b & 0xF0) | (v & 0x0F)) : static_cast<uint8_t>((b & 0x0F) | (v << 4));
      b = blend(b, fresh);
      return;
    }
    if (ret_) { subWaiting_ = true; throw SubWait{}; }
    wram_[a & 0x3FFFF] = v;
    return;
  }
  if (a < 0xE0000) {
    if (mode1M_) wram_[interleave(subHalf(), a & 0x1FFFF)] = v;
    return;
  }
  if (a < 0xF0000) {
    if (a & 1) { uint8_t& b = bram_[(a >> 1) & 0x1FFF]; if (b != v) { b = v; dirty_ = true; } }
    return;
  }
  if (!(a & 0x8000)) { if (a & 1) pcmWrite((a >> 1) & 0x1FFF, v); return; }
  subRegWrite8(a & 0x1FF, v);
}

void SegaCD::subWrite16(uint32_t a, uint16_t v) {
  a &= 0xFFFFE;
  if (a < 0x80000) {
    if (!prgProtected(a, wp_)) { prg_[a] = static_cast<uint8_t>(v >> 8); prg_[a + 1] = static_cast<uint8_t>(v); }
    return;
  }
  if (a < 0xC0000) {
    if (mode1M_) {
      uint8_t& b = wram_[interleave(subHalf(), (a >> 1) & 0x1FFFF)];
      b = blend(b, static_cast<uint8_t>(((v >> 4) & 0xF0) | (v & 0x0F)));
      return;
    }
    if (ret_) { subWaiting_ = true; throw SubWait{}; }
    wram_[a & 0x3FFFF] = static_cast<uint8_t>(v >> 8); wram_[(a & 0x3FFFF) + 1] = static_cast<uint8_t>(v);
    return;
  }
  if (a < 0xE0000) {
    if (mode1M_) {
      const uint32_t o = interleave(subHalf(), a & 0x1FFFF);
      wram_[o] = static_cast<uint8_t>(v >> 8); wram_[o + 1] = static_cast<uint8_t>(v);
    }
    return;
  }
  if (a < 0xF0000) {
    uint8_t& b = bram_[(a >> 1) & 0x1FFF];
    if (b != static_cast<uint8_t>(v)) { b = static_cast<uint8_t>(v); dirty_ = true; }
    return;
  }
  if (!(a & 0x8000)) { pcmWrite((a >> 1) & 0x1FFF, static_cast<uint8_t>(v)); return; }
  subRegWrite16(a & 0x1FE, v);
}

// ------------------------------------------------------------ save
std::vector<uint8_t> SegaCD::exportSave() {
  dirty_ = false;
  std::vector<uint8_t> s = bram_;
  s.insert(s.end(), cart_.begin(), cart_.end());
  return s;
}

void SegaCD::importSave(const std::vector<uint8_t>& b) {
  if (!b.empty()) {
    std::copy(b.begin(), b.begin() + std::min(b.size(), bram_.size()), bram_.begin());
    if (b.size() > bram_.size() && !cart_.empty())
      std::copy(b.begin() + bram_.size(), b.begin() + std::min(b.size(), bram_.size() + cart_.size()), cart_.begin());
  }
  formatBackup();
}

// RAM de backup sem formatação (save novo ou apagado): grava o rodapé do formato de RAM de backup da Sega
// (diretório vazio, "SEGA_CD_ROM" e "RAM_CARTRIDGE", como a BIOS deixa depois de formatar) para o BIOS e os jogos
// acharem a RAM pronta para uso (sem isso alguns jogos recusam salvar)
void SegaCD::formatBackup() {
  static const uint8_t FMT[0x40] = {
      0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x00, 0x00, 0x00, 0x00, 0x40,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      'S', 'E', 'G', 'A', '_', 'C', 'D', '_', 'R', 'O', 'M', 0x00, 0x01, 0x00, 0x00, 0x00,
      'R', 'A', 'M', '_', 'C', 'A', 'R', 'T', 'R', 'I', 'D', 'G', 'E', '_', '_', '_'};
  for (auto* m : {&bram_, &cart_}) {
    if (m->size() < 0x40 || std::memcmp(m->data() + m->size() - 0x20, FMT + 0x20, 0x20) == 0) continue;
    std::fill(m->begin(), m->end() - 0x40, 0);
    uint8_t f[0x40];
    std::memcpy(f, FMT, sizeof f);
    const size_t blocks = m->size() / 64 - 3;
    f[0x10] = f[0x12] = f[0x14] = f[0x16] = static_cast<uint8_t>(blocks >> 8);
    f[0x11] = f[0x13] = f[0x15] = f[0x17] = static_cast<uint8_t>(blocks);
    std::memcpy(m->data() + m->size() - 0x40, f, sizeof f);
  }
}

// ------------------------------------------------------------ save state
void SegaCD::sync(StateIO& io) {
  sub.sync(io);
  // gate array
  io.sync(resetBusreq_); io.sync(ifl2_); io.sync(hint_); io.sync(wp_); io.sync(bank_); io.sync(pm_);
  io.sync(mode1M_); io.sync(ret_); io.sync(dmna_); io.sync(swapAsked_);
  io.sync(dd_); io.sync(edt_); io.sync(dsr_); io.sync(hostData_); io.sync(dmaAddr_);
  io.sync(commMain_); io.sync(commSub_); io.raw(commCmd_, sizeof commCmd_); io.raw(commStatus_, sizeof commStatus_);
  io.sync(led_); io.sync(timerReload_); io.sync(mask_); io.sync(fontColor_); io.sync(hock_); io.sync(fontBits_);
  io.raw(subcode_, sizeof subcode_);
  io.sync(pending_); io.sync(subHalt_); io.sync(subReset_); io.sync(subWaiting_);
  io.sync(subClk_); io.sync(timerAt_); io.sync(driveAt3_); io.sync(stopwatchZero_); io.sync(frameScd_);
  uint8_t x = xfer_, h = xferHeld_;
  io.sync(x); io.sync(h); io.sync(xferClk_);
  io.sync(cartProt_);
  io.bytes(prg_); io.bytes(wram_); io.bytes(bram_); io.bytes(cart_);
  cdc_.sync(io);
  drive_.sync(io);
  asic_.sync(io);
  pcm_.sync(io);
  io.sync(pcmClk_);
  io.raw(pcmLast_, sizeof pcmLast_); io.raw(cddaLast_, sizeof cddaLast_); io.sync(cddaFrac_);
  io.raw(hpX_, sizeof hpX_); io.raw(hpY_, sizeof hpY_);
  if (io.reading()) {
    // só limita o que poderia levar a acesso fora da memória ou a laço sem fim; valores válidos ficam iguais
    xfer_ = x <= TO_WRAM_2M ? static_cast<Dest>(x) : TO_NONE;
    xferHeld_ = h <= TO_WRAM_2M ? static_cast<Dest>(h) : TO_NONE;
    dd_ &= 7; edt_ &= 1; dsr_ &= 1; bank_ &= 3; pm_ &= 3; resetBusreq_ &= 3;
    refreshIpl();
    if (!(cddaFrac_ == cddaFrac_) || cddaFrac_ < 0 || cddaFrac_ > 2) cddaFrac_ = 0;
    for (float* f : {&hpX_[0], &hpX_[1], &hpY_[0], &hpY_[1]}) if (!(*f == *f) || *f > 100 || *f < -100) *f = 0;
    // relógios: todos perto da hora do principal
    const int64_t m = mainNow();
    const int64_t win = int64_t(scdPerLine_) * 400;
    clampTo<int64_t>(subClk_, m - win, m + win);
    clampTo<int64_t>(timerAt_, 0, subClk_ + 256 * 384 * 4);
    clampTo<int64_t>(driveAt3_, subClk_ * 3 - 6000000, subClk_ * 3 + 2000000);
    clampTo<int64_t>(stopwatchZero_, subClk_ - (int64_t(1) << 61), subClk_ + win);
    clampTo<int64_t>(frameScd_, subClk_ - win, subClk_ + win);
    clampTo<int64_t>(pcmClk_, subClk_ - win, subClk_ + win);
    // relógios de DMA e do ASIC só valem enquanto estão em uso (parados guardam a hora antiga, que é válida)
    if (xfer_ || xferHeld_) clampTo<int64_t>(xferClk_, subClk_ - win, subClk_ + win);
    if (asic_.busy) clampTo<int64_t>(asic_.clk, subClk_ - win, subClk_ + win);
    pcmBuf_.clear();
  }
}

// ------------------------------------------------------------ núcleo
SegaCdCore::SegaCdCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>& bios,
                       const std::vector<uint8_t>* save, bool ramCart) {
  if (!isSegaCdBios(bios)) throw std::runtime_error("BIOS do Sega CD inválida.");
  md_ = std::make_unique<md::MD>(bios, nullptr);  // o cabeçalho da BIOS dá a região do aparelho
  cd_ = std::make_unique<SegaCD>(*md_, bios, std::move(disc), ramCart);
  md_->attach(cd_.get());
  if (save) cd_->importSave(*save);
  md_->reset();
}

SegaCdCore::~SegaCdCore() { if (md_) md_->attach(nullptr); }

void SegaCdCore::reset() {
  md_->reset();
  cd_->reset();
  md_->reset();  // o 68000 principal relê os vetores (agora vindos da BIOS pelo Sega CD)
}

void SegaCdCore::runFrame() {
  const size_t before = md_->audioAvailable();
  md_->runFrame();
  const size_t after = md_->audioAvailable();
  cd_->endFrame();
  // o som do CD acompanha o do Mega Drive: o mesmo número de amostras por quadro
  cd_->produceAudio(after >= before ? after - before : 0);
}

void SegaCdCore::pullAudio(float* l, float* r, size_t n) {
  md_->pullAudio(l, r, n);
  cd_->mixAudio(l, r, n);
}

void SegaCdCore::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, STATE_MAGIC)) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  std::vector<uint8_t> ms;
  if (!io.reading()) ms = md_->saveState();
  uint32_t n = static_cast<uint32_t>(ms.size());
  io.sync(n);
  if (io.reading()) { if (n > (64u << 20)) throw std::runtime_error("Estado salvo danificado."); ms.resize(n); }
  if (n) io.raw(ms.data(), n);
  if (io.reading()) md_->loadState(ms);
  cd_->sync(io);
}

std::vector<uint8_t> SegaCdCore::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void SegaCdCore::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
}

std::unique_ptr<Core> createSegaCdCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save) {
  std::vector<uint8_t> bios;
  const std::string r = disc ? segaCdDiscRegion(*disc) : "";
  for (const std::string& n : {std::string("segacd_") + r, std::string("segacd"), std::string("segacd_us"),
                               std::string("segacd_eu"), std::string("segacd_jp")})
    if (bios.empty() && !firmware(n).empty() && isSegaCdBios(firmware(n))) bios = firmware(n);
  if (bios.empty())
    throw std::runtime_error("O Sega CD precisa da BIOS do aparelho (arquivo de 128 KB, por exemplo bios_CD_U.bin). "
                             "Abra o arquivo da BIOS no Índigo uma vez para instalá-la.");
  return std::make_unique<SegaCdCore>(std::move(disc), bios, save);
}

}  // namespace indigo::segacd
