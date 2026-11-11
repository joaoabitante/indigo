#include "jaguar.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace indigo::jaguar {

namespace {
const char STATE_MAGIC[4] = {'I', 'J', 'G', '2'};
// limites dos relógios guardados no estado: à frente cabe qualquer fila de blits; atrás, só um pouco
constexpr int64_t MAX_AHEAD = int64_t(1) << 40, MAX_BEHIND = int64_t(1) << 20;
constexpr uint32_t CART_MARK = 0x04040404;
constexpr int LEFT_HC = 188;  // começo da janela visível (em ciclos do vídeo), como no Virtual Jaguar
constexpr int VIS_HC = 1304;  // largura da janela visível (326 pontos de 4 ciclos)

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
uint32_t at32(const std::vector<uint8_t>& b, size_t o) { return o + 4 <= b.size() ? be32(&b[o]) : 0; }

// tabelas de conversão CRY -> RGB (16 x 16 cores, cada uma com intensidade máxima), da documentação do Jaguar
// (as mesmas do Virtual Jaguar, GPL-3; a célula [8][12] do vermelho, 15 lá, aqui é 115 como na tabela do azul)
const uint8_t RED_CV[16][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 19, 0},
    {68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 68, 64, 43, 21, 0},
    {102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 95, 71, 47, 23, 0},
    {135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 130, 104, 78, 52, 26, 0},
    {169, 169, 169, 169, 169, 169, 169, 169, 169, 170, 141, 113, 85, 56, 28, 0},
    {203, 203, 203, 203, 203, 203, 203, 203, 203, 183, 153, 122, 91, 61, 30, 0},
    {237, 237, 237, 237, 237, 237, 237, 237, 230, 197, 164, 131, 98, 65, 32, 0},
    {255, 255, 255, 255, 255, 255, 255, 255, 247, 214, 181, 148, 115, 82, 49, 7},
    {255, 255, 255, 255, 255, 255, 255, 255, 255, 235, 204, 173, 143, 112, 81, 51},
    {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 227, 198, 170, 141, 113, 85},
    {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 249, 223, 197, 171, 145, 119},
    {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 248, 224, 200, 177, 153},
    {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 252, 230, 208, 187},
    {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 240, 221},
    {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}};
const uint8_t GREEN_CV[16][16] = {
    {0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255},
    {0, 19, 38, 57, 77, 96, 115, 134, 154, 173, 192, 211, 231, 250, 255, 255},
    {0, 21, 43, 64, 86, 107, 129, 150, 172, 193, 215, 236, 255, 255, 255, 255},
    {0, 23, 47, 71, 95, 119, 142, 166, 190, 214, 238, 255, 255, 255, 255, 255},
    {0, 26, 52, 78, 104, 130, 156, 182, 208, 234, 255, 255, 255, 255, 255, 255},
    {0, 28, 56, 85, 113, 141, 170, 198, 226, 255, 255, 255, 255, 255, 255, 255},
    {0, 30, 61, 91, 122, 153, 183, 214, 244, 255, 255, 255, 255, 255, 255, 255},
    {0, 32, 65, 98, 131, 164, 197, 230, 255, 255, 255, 255, 255, 255, 255, 255},
    {0, 32, 65, 98, 131, 164, 197, 230, 255, 255, 255, 255, 255, 255, 255, 255},
    {0, 30, 61, 91, 122, 153, 183, 214, 244, 255, 255, 255, 255, 255, 255, 255},
    {0, 28, 56, 85, 113, 141, 170, 198, 226, 255, 255, 255, 255, 255, 255, 255},
    {0, 26, 52, 78, 104, 130, 156, 182, 208, 234, 255, 255, 255, 255, 255, 255},
    {0, 23, 47, 71, 95, 119, 142, 166, 190, 214, 238, 255, 255, 255, 255, 255},
    {0, 21, 43, 64, 86, 107, 129, 150, 172, 193, 215, 236, 255, 255, 255, 255},
    {0, 19, 38, 57, 77, 96, 115, 134, 154, 173, 192, 211, 231, 250, 255, 255},
    {0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255}};

uint8_t blueCv(int c, int r) { return RED_CV[15 - c][r]; }  // a tabela do azul é a do vermelho de cabeça para baixo

// ROM de ondas do Jerry (0xF1D000): 8 tabelas de 128 valores de 16 bits com sinal (em palavras de 32 bits).
// Os valores do chip não estão documentados com exatidão: estas são as mesmas formas, geradas aqui.
constexpr double TAU = 6.283185307179586;
void makeWaveRom(uint8_t* w) {
  for (int t = 0; t < 8; t++)
    for (int i = 0; i < 128; i++) {
      const double ph = i / 128.0;
      double v = 0;
      switch (t) {
        case 0: v = ph < 0.25 ? ph * 4 : (ph < 0.75 ? 2 - ph * 4 : ph * 4 - 4); break;      // triângulo
        case 1: v = std::sin(TAU * ph); break;                                          // seno
        case 2: v = std::sin(TAU * ph) * (0.5 + 0.5 * std::sin(TAU * ph * 4)); break;  // seno modulado
        case 3: v = std::sin(TAU * ph * 12) * 0.9; break;                                // 12 ondas
        case 4: v = std::sin(TAU * ph * ph * 16); break;                                 // chirp
        case 5: v = (ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4) * 0.98; break;                       // triângulo com ruído
        case 6: v = i == 0 ? 1.0 : 0.0; break;                                                // impulso
        default: { uint32_t x = uint32_t(i) * 2654435761u; x ^= x >> 13; v = int16_t(x >> 8) / 32768.0; } // ruído
      }
      const int32_t s = int32_t(std::lround(std::clamp(v, -1.0, 1.0) * 32767));
      put32(w + (t * 128 + i) * 4, uint32_t(s));
    }
}
}  // namespace

// ---------------------------------------------------------------------------------- reconhecimento
bool isJaguarRom(const std::vector<uint8_t>& b, const std::string& n) {
  if (b.size() < 0x24) return false;
  if (endsWith(n, ".j64")) return true;
  const bool mark = at32(b, 0x400) == CART_MARK;
  if (endsWith(n, ".jag") || endsWith(n, ".abs")) {
    if ((b[0] == 0x60 && (b[1] == 0x1B || b[1] == 0x1A)) || (b[0] == 0x01 && b[1] == 0x50)) return true;
    return mark || b.size() % 0x100000 == 0;
  }
  // .rom também é do MSX: só com a marca do cartucho do Jaguar
  if (endsWith(n, ".rom")) return mark && b.size() >= 0x2000;
  return false;
}

// ---------------------------------------------------------------------------------- criação
Jaguar::Jaguar(const std::vector<uint8_t>& f, const std::string& nome, bool isPal)
    : pal(isPal), cpu(*this), gpu(*this, false), dsp(*this, true), dram(0x200000, 0), file_(f) {
  if (f.empty()) throw std::runtime_error("Arquivo do Jaguar vazio.");
  for (int i = 0; i < 0x10000; i++) {
    const int c = (i >> 12) & 15, r = (i >> 8) & 15, y = i & 0xFF;
    const uint32_t R = RED_CV[c][r] * y >> 8, G = GREEN_CV[c][r] * y >> 8, B = blueCv(c, r) * y >> 8;
    cry_[i] = 0xFF000000u | R | (G << 8) | (B << 16);
  }
  std::fill(std::begin(eeprom), std::end(eeprom), 0xFFFF);
  makeWaveRom(wave_);
  fb.assign(size_t(FB_W) * FB_H, 0xFF000000u);
  aud_.assign(1 << 15, 0.0f);
  // título: o nome do arquivo sem a extensão
  std::string t = nome;
  const size_t sl = t.find_last_of("/\\");
  if (sl != std::string::npos) t = t.substr(sl + 1);
  const size_t dot = t.find_last_of('.');
  if (dot != std::string::npos) t = t.substr(0, dot);
  title = t;
  std::string low = nome;
  for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
  j64_ = endsWith(low, ".j64");
  reset();
}

void Jaguar::reset() {
  std::fill(dram.begin(), dram.end(), 0);
  std::memset(tom, 0, sizeof tom);
  std::memset(blitRegs, 0, sizeof blitRegs);
  std::memset(jmisc_, 0, sizeof jmisc_);
  gpu.reset(); dsp.reset();
  now_ = m68k_ = hlStart_ = 0; vc_ = 0;
  tomEn_ = tomPend_ = jEn_ = jPend_ = 0;
  ob_ = 0; obf_ = false; opFlag_ = 0;
  tomPitNext_ = jPitNext_[0] = jPitNext_[1] = 0;
  std::memset(jPit_, 0, sizeof jPit_);
  i2sNext_ = 0; sclk_ = 19; smode_ = 0; ltxd_ = rtxd_ = 0;
  joySel_ = 0xFFFF;
  eeState_ = eeBits_ = eeOutCnt_ = 0; eeShift_ = 0; eeAddr_ = eeOp_ = 0; eeWen_ = false;
  audW_ = audR_ = 0; audNext_ = 0;
  busFree_ = blitEnd_ = 0; blitIrq_ = false; accessTime = 0; frame_ = 0;
  hleBoot();
  cpu.reset();
  updateIpl();
}

// o que a ROM de partida deixa pronto antes de entrar no cartucho (HLE)
void Jaguar::hleBoot() {
  const std::vector<uint8_t>& b = file_;
  cart.clear();
  ramProgram = true;
  uint32_t ssp = 0x200000;
  auto load = [&](uint32_t to, size_t from, size_t n) {
    for (size_t i = 0; i < n && from + i < b.size(); i++) dram[(to + i) & 0x1FFFFF] = b[from + i];
  };
  // .j64 é sempre cartucho; os outros formatos se reconhecem pelos primeiros bytes
  if (!j64_ && b[0] == 0x60 && b[1] == 0x1B) {  // ABS (DRI): texto+dados em 0x24, carga = partida
    const uint32_t ld = at32(b, 0x16), n = at32(b, 0x02) + at32(b, 0x06);
    load(ld, 0x24, n); entry = ld;
  } else if (!j64_ && b[0] == 0x01 && b[1] == 0x50) {  // ABS (COFF)
    const uint32_t ld = at32(b, 0x28), n = at32(b, 0x18) + at32(b, 0x1C);
    load(ld, 0xA8, n); entry = at32(b, 0x24);
  } else if (!j64_ && b[0] == 0x60 && b[1] == 0x1A && b.size() > 0x2E && b[0x1C] == 'J' && b[0x1D] == 'A' && b[0x1E] == 'G') {
    // Jaguar Server ("JAGR"): tipo 2 sem endereço de partida
    const uint32_t ld = at32(b, 0x22);
    if (be16(&b[0x20]) == 2) { load(ld, 0x2A, b.size() - 0x2A); entry = ld; }
    else { load(ld, 0x2E, b.size() - 0x2E); entry = at32(b, 0x2A); }
  } else {  // cartucho
    ramProgram = false;
    size_t sz = 0x2000;
    while (sz < b.size() && sz < 0x600000) sz <<= 1;
    cart.assign(sz, 0xFF);
    std::copy(b.begin(), b.begin() + std::min(b.size(), size_t(0x600000)), cart.begin());
    entry = at32(cart, 0x404);
    if (entry < 0x800000 || entry >= 0xE00000) entry = 0x802000;
    ssp = 0x4000;
  }
  entry &= 0xFFFFFE;
  // vetores: erros de barramento/endereço pulam o quadro longo; o resto volta direto (RTE)
  put16(&dram[0x400], 0x508F); put16(&dram[0x402], 0x4E73); put16(&dram[0x404], 0x4E73);
  for (int v = 2; v < 256; v++) put32(&dram[v * 4], v < 4 ? 0x400 : 0x404);
  put32(&dram[0], ssp);
  put32(&dram[4], entry);
  // lista de objetos vazia (um objeto de parada) em 0x1000
  put32(&dram[0x1000], 0); put32(&dram[0x1004], 4);
  if (!ramProgram) put32(&dram[0x804], 1);  // a ROM de partida marca o fim do seu trabalho aqui
  // vídeo como a ROM de partida deixa (NTSC: 262 linhas; PAL: 312 linhas, linha de 1702 tiques)
  setReg16(0x00, uint16_t(0x1861 | (cart.empty() ? 0 : (cart[0x400] & 0x1E))));
  setReg16(0x02, 0x35CC);
  const uint16_t v[][2] = {{0x2E, 844}, {0x30, 1713}, {0x32, 125}, {0x34, 1741}, {0x36, 651}, {0x38, 203}, {0x3A, 203},
                           {0x3C, 1665}, {0x3E, 523}, {0x40, 500}, {0x42, 24}, {0x44, 517}, {0x46, 38}, {0x48, 518},
                           {0x4A, 511}, {0x4C, 6}, {0x54, 784}, {0x58, 0}, {0x28, 0x6C1}, {0x4E, 519}};
  for (auto& p : v) setReg16(p[0], p[1]);
  if (pal) {
    const uint16_t vp[][2] = {{0x2E, 850}, {0x30, 1711}, {0x32, 158}, {0x3E, 623}, {0x40, 600}, {0x42, 34}, {0x44, 618}};
    for (auto& p : vp) setReg16(p[0], p[1]);
  }
  setReg16(0x20, 0x1000); setReg16(0x22, 0);  // OLP (as metades ficam trocadas)
  gpu.endian = dsp.endian = 0x00070007;
  gpu.ramWrite32(0xF03000, 0x03D0DEAD);  // marca deixada pela verificação do cartucho
  smode_ = 0x15; sclk_ = 0x13;
  i2sNext_ = now_ + 64 * (sclk_ + 1);
  put16(&jmisc_[0x12], 0xB5); put16(&jmisc_[0x14], 4);  // CLK2/CLK3
}

// ---------------------------------------------------------------------------------- barramento
uint16_t Jaguar::read16(uint32_t a) {
  a &= 0xFFFFFE;
  if (a < 0x800000) return be16(&dram[a & 0x1FFFFE]);
  if (a < 0xE00000) return cart.empty() ? 0xFFFF : be16(&cart[(a - 0x800000) % cart.size()]);
  if (a < 0xF00000) return (a < 0xE20000 && !bios.empty()) ? uint16_t((bios[(a - 0xE00000) % bios.size()] << 8) | bios[(a - 0xE00000 + 1) % bios.size()]) : 0xFFFF;
  if (a < 0xF10000) return tomRead16(a & 0x3FFE);
  if (a < 0xF20000) return jerryRead16(a & 0xFFFE);
  return 0xFFFF;
}

uint8_t Jaguar::read8(uint32_t a) {
  a &= 0xFFFFFF;
  if (a < 0x800000) return dram[a & 0x1FFFFF];
  const uint16_t w = read16(a & ~1u);
  return uint8_t((a & 1) ? w : w >> 8);
}

void Jaguar::write16(uint32_t a, uint16_t v) {
  a &= 0xFFFFFE;
  if (a < 0x800000) { put16(&dram[a & 0x1FFFFE], v); return; }
  if (a < 0xF00000) return;  // ROM
  if (a < 0xF10000) { tomWrite16(a & 0x3FFE, v); return; }
  if (a < 0xF20000) { jerryWrite16(a & 0xFFFE, v); return; }
}

void Jaguar::write8(uint32_t a, uint8_t v) {
  a &= 0xFFFFFF;
  if (a < 0x800000) { dram[a & 0x1FFFFF] = v; return; }
  if (a < 0xF00000) return;
  // registros: lê a palavra guardada (sem efeitos colaterais quando dá) e escreve a palavra inteira
  const uint32_t w = a & ~1u;
  uint16_t old = 0;
  if (w >= 0xF00000 && w < 0xF10000) {
    const uint32_t o = w & 0x3FFE;
    if (o >= 0x3000) old = uint16_t(gpu.ramRead32(0xF03000 + o - 0x3000) >> ((o & 2) ? 0 : 16));
    else if (o >= 0x2100 && o < 0x2120) { const uint32_t r = gpu.regRead(o & 0x1C); old = uint16_t((o & 2) ? r : r >> 16); }
    else if (o >= 0x2200 && o < 0x2300) old = be16(&blitRegs[o & 0xFE]);
    else if (o == 0xE0) old = tomEn_;
    else old = be16(&tom[(o >= 0x600 && o < 0x800) ? o - 0x200 : o]);
  } else if (w >= 0xF1B000 && w < 0xF1D000) {
    old = uint16_t(dsp.ramRead32(w) >> ((w & 2) ? 0 : 16));
  }
  write16(w, (a & 1) ? uint16_t((old & 0xFF00) | v) : uint16_t((old & 0x00FF) | (v << 8)));
}

uint32_t Jaguar::read32(uint32_t a) {
  a &= 0xFFFFFC;
  if (a < 0x800000) return be32(&dram[a & 0x1FFFFC]);
  if (a >= 0xF02100 && a < 0xF02120) return gpu.regRead(a & 0x1F);
  if (a >= 0xF03000 && a < 0xF04000) return gpu.ramRead32(a);
  if (a >= 0xF1A100 && a < 0xF1A124) return dsp.regRead(a - 0xF1A100);
  if (a >= 0xF1B000 && a < 0xF1D000) return dsp.ramRead32(a);
  if (a >= 0xF02200 && a < 0xF022A0) { const uint32_t o = a & 0xFC; return o == 0x38 ? (blitBusy() ? 0 : 1) : be32(&blitRegs[o]); }
  return (uint32_t(read16(a)) << 16) | read16(a + 2);
}

void Jaguar::write32(uint32_t a, uint32_t v) {
  a &= 0xFFFFFC;
  if (a < 0x800000) { put32(&dram[a & 0x1FFFFC], v); return; }
  if (a >= 0xF02100 && a < 0xF02120) { gpu.regWrite(a & 0x1F, v); return; }
  if (a >= 0xF03000 && a < 0xF04000) { gpu.ramWrite32(a, v); return; }
  if (a >= 0xF1A100 && a < 0xF1A124) { dsp.regWrite(a - 0xF1A100, v); return; }
  if (a >= 0xF1B000 && a < 0xF1D000) { dsp.ramWrite32(a, v); return; }
  if (a >= 0xF02200 && a < 0xF022A0) {
    const uint32_t o = a & 0xFC;
    put32(&blitRegs[o], v);
    blitAlias(o);
    if (o == 0x38) blit(v);
    return;
  }
  write16(a, uint16_t(v >> 16));
  write16(a + 2, uint16_t(v));
}

// ---------------------------------------------------------------------------------- Tom
uint16_t Jaguar::tomRead16(uint32_t o) {
  if (o >= 0x3000) return uint16_t(gpu.ramRead32(0xF03000 + o - 0x3000) >> ((o & 2) ? 0 : 16));
  if (o >= 0x2100 && o < 0x2120) { const uint32_t r = gpu.regRead(o & 0x1C); return uint16_t((o & 2) ? r : r >> 16); }
  if (o >= 0x2200 && o < 0x2300) {
    if ((o & 0xFC) == 0x38) return (o & 2) ? (blitBusy() ? 0 : 1) : 0;  // B_CMD lido = estado (bit 0: ocioso)
    return be16(&blitRegs[o & 0xFE]);
  }
  if (o >= 0x600 && o < 0x800) o -= 0x200;  // espelho da CLUT
  switch (o) {
    case 0x04: {  // HC: posição dentro da meia linha
      const int64_t d = std::max<int64_t>(0, accessTime - hlStart_);
      return uint16_t((d & 0x3FF) | ((vc_ & 1) ? 0x400 : 0));
    }
    case 0x06: return uint16_t(vc_ | ((interlaced() && (frame_ & 1)) ? 0x800 : 0));  // bit 11: segundo campo
    case 0x10: case 0x12: case 0x14: case 0x16: return uint16_t(ob_ >> (48 - (o - 0x10) * 8));
    case 0xE0: return tomPending();
    default: return be16(&tom[o]);
  }
}

void Jaguar::tomWrite16(uint32_t o, uint16_t v) {
  if (o >= 0x3000) {
    const uint32_t a = 0xF03000 + o - 0x3000, w = gpu.ramRead32(a);
    gpu.ramWrite32(a, (o & 2) ? ((w & 0xFFFF0000u) | v) : ((w & 0xFFFFu) | (uint32_t(v) << 16)));
    return;
  }
  if (o >= 0x2100 && o < 0x2120) { gpu.regWrite16(o & 0x1C, v, o & 2); return; }
  if (o >= 0x2200 && o < 0x2300) {
    put16(&blitRegs[o & 0xFE], v);
    if (o & 2) { blitAlias(o & 0xFC); if ((o & 0xFC) == 0x38) blit(be32(&blitRegs[0x38])); }
    return;
  }
  if (o >= 0x600 && o < 0x800) o -= 0x200;
  switch (o) {
    case 0x26: obf_ = true; opFlag_ = v; break;
    case 0x50: case 0x52: put16(&tom[o], v); resetTomPit(); return;
    case 0xE0:
      tomEn_ = v & 0x1F;
      tomPend_ &= uint8_t(~(v >> 8) & 0x1F);
      updateIpl();
      return;
    case 0xE2: return;
    default: break;
  }
  put16(&tom[o], v);
}

void Jaguar::tomIrq(int bit) {
  if (!(tomEn_ & (1 << bit))) return;
  tomPend_ |= uint8_t(1 << bit);
  updateIpl();
}

void Jaguar::jerryIrq(int bit) {
  if (!(jEn_ & (1 << bit))) return;
  jPend_ |= uint8_t(1 << bit);
  updateIpl();
}

// o Jerry chega ao 68000 pelo bit 4 do Tom enquanto tiver interrupção pendente
uint8_t Jaguar::tomPending() const { return uint8_t(tomPend_ | ((jPend_ & jEn_) ? 0x10 : 0)); }
void Jaguar::updateIpl() { cpu.ipl = (tomPending() & tomEn_) ? 2 : 0; }

void Jaguar::riscCpuInt(bool fromDsp) {
  if (fromDsp) jerryIrq(1);
  else tomIrq(1);
}

// ---------------------------------------------------------------------------------- Jerry
uint16_t Jaguar::joyRead(bool buttons) {
  const int sel = joySel_ & 0x0F;
  int row = -1;
  if (sel == 0xE) row = 0; else if (sel == 0xD) row = 1; else if (sel == 0xB) row = 2; else if (sel == 0x7) row = 3;
  if (!buttons) {
    uint16_t d = 0xFFFF;
    if (row >= 0) {
      static const uint32_t ROWS[4][4] = {{J_UP, J_DOWN, J_LEFT, J_RIGHT},
                                          {J_STAR, J_N0 << 7, J_N0 << 4, J_N0 << 1},
                                          {J_N0, J_N0 << 8, J_N0 << 5, J_N0 << 2},
                                          {J_HASH, J_N0 << 9, J_N0 << 6, J_N0 << 3}};
      for (int i = 0; i < 4; i++) if (pad & ROWS[row][i]) d &= uint16_t(~(0x100 << i));
      static const uint16_t M2[4] = {0xFFFF, 0xFFFD, 0xFFFB, 0xFFF7};
      d &= M2[row];
    }
    return d;
  }
  uint16_t d = pal ? 0xFF6F : 0xFF7F;  // bit 4: 1 = NTSC, 0 = PAL
  if (row >= 0) {
    static const uint32_t B[4] = {J_A, J_B, J_C, J_OPTION};
    if (pad & B[row]) d &= 0xFFFD;
    if (row == 0 && (pad & J_PAUSE)) d &= 0xFFFE;
  }
  return d;
}

uint16_t Jaguar::jerryRead16(uint32_t o) {
  if (o >= 0xA100 && o < 0xA124) { const uint32_t r = dsp.regRead(o - 0xA100); return uint16_t((o & 2) ? r : r >> 16); }
  if (o >= 0xB000 && o < 0xD000) return uint16_t(dsp.ramRead32(0xF10000 + o) >> ((o & 2) ? 0 : 16));
  if (o >= 0xD000 && o < 0xE000) return be16(&wave_[o - 0xD000]);
  switch (o) {
    case 0x0020: return jPend_;
    case 0x0036: case 0x0038: case 0x003A: case 0x003C: {  // contagem atual dos temporizadores
      const int n = (o - 0x36) / 4;
      if (!jPitNext_[n]) return 0;
      const int64_t per = int64_t(jPit_[n * 2]) + 1, left = std::max<int64_t>(0, jPitNext_[n] - std::max(now_, m68k_));
      return uint16_t(((o - 0x36) & 2) ? left / per : left % per);  // pré-escala, depois divisor
    }
    case 0x4000: return uint16_t((joyRead(false) & 0xFFFE) | eepromDo());
    case 0x4002: return joyRead(true);
    case 0x4800: eepromClock(); return 0;
    case 0x5000: eepromCs(); return 0;
    case 0xA148: case 0xA14C: return 0;
    case 0xA14A: return uint16_t(ltxd_);
    case 0xA14E: return uint16_t(rtxd_);
    case 0xA150: return 0;
    case 0xA152: return 0x0001;  // SSTAT
    default: return o < 0x40 ? be16(&jmisc_[o]) : 0;
  }
}

void Jaguar::jerryWrite16(uint32_t o, uint16_t v) {
  if (o >= 0xA100 && o < 0xA124) { dsp.regWrite16(o - 0xA100, v, o & 2); return; }
  if (o >= 0xB000 && o < 0xD000) {
    const uint32_t a = 0xF10000 + o, w = dsp.ramRead32(a);
    dsp.ramWrite32(a, (o & 2) ? ((w & 0xFFFF0000u) | v) : ((w & 0xFFFFu) | (uint32_t(v) << 16)));
    return;
  }
  switch (o) {
    case 0x0000: case 0x0002: case 0x0004: case 0x0006:
      jPit_[o / 2] = v;
      resetJerryPit(o < 4 ? 0 : 1);
      return;
    case 0x0020:
      jEn_ = v & 0x3F;
      jPend_ &= uint8_t(~(v >> 8));
      updateIpl();
      return;
    case 0x4000: joySel_ = v; return;
    case 0x4800: eepromDi(v & 1); return;
    case 0x5000: eepromCs(); return;
    case 0xA148: case 0xA14C: return;
    case 0xA14A: ltxd_ = v; return;
    case 0xA14E: rtxd_ = v; return;
    case 0xA150: return;
    case 0xA152:
      sclk_ = v & 0xFF;
      i2sNext_ = (smode_ & 1) ? now_ + 64 * (int64_t(sclk_) + 1) : 0;
      return;
    case 0xA154: return;
    case 0xA156:
      smode_ = v;
      i2sNext_ = (smode_ & 1) ? now_ + 64 * (int64_t(sclk_) + 1) : 0;
      return;
    default: if (o < 0x40) put16(&jmisc_[o], v); return;
  }
}

// ---------------------------------------------------------------------------------- EEPROM 93C46
void Jaguar::eepromCs() { eeState_ = 1; eeBits_ = 0; eeShift_ = 0; eeWen_ = true; }

uint16_t Jaguar::eepromDo() {
  switch (eeState_) {
    case 5: eeState_ = 1; return 0;  // ocupado depois de gravar: 0 uma vez, depois pronto
    case 4: return eeOutCnt_ >= 16 ? 0 : (eeprom[eeAddr_] >> eeOutCnt_) & 1;
    default: return 1;
  }
}

void Jaguar::eepromClock() {
  if (eeState_ != 4) return;
  if (eeOutCnt_) eeOutCnt_--;
  else eeState_ = 1;
}

void Jaguar::eepromDi(int bit) {
  switch (eeState_) {
    case 1: eeState_ = 2; eeBits_ = 0; eeShift_ = 0; break;  // bit de partida
    case 2:
      eeShift_ = (eeShift_ << 1) | uint32_t(bit);
      if (++eeBits_ < 8) break;
      eeOp_ = uint8_t(eeShift_ >> 6); eeAddr_ = uint8_t(eeShift_ & 0x3F);
      eeBits_ = 0; eeShift_ = 0;
      switch (eeOp_) {
        case 2: eeState_ = 4; eeOutCnt_ = 16; break;  // READ
        case 1: eeState_ = 3; break;                  // WRITE
        case 3:                                       // ERASE
          if (eeWen_) { eeprom[eeAddr_] = 0xFFFF; eepromDirty = true; }
          eeState_ = 5; break;
        default:
          switch (eeAddr_ >> 4) {
            case 0: eeWen_ = false; eeState_ = 1; break;  // EWDS
            case 1: eeState_ = 3; break;                  // WRAL
            case 2:                                       // ERAL
              if (eeWen_) { std::fill(std::begin(eeprom), std::end(eeprom), 0xFFFF); eepromDirty = true; }
              eeState_ = 5; break;
            default: eeWen_ = true; eeState_ = 1; break;  // EWEN
          }
      }
      break;
    case 3:
      eeShift_ = (eeShift_ << 1) | uint32_t(bit);
      if (++eeBits_ < 16) break;
      if (eeWen_) {
        if (eeOp_ == 1) eeprom[eeAddr_] = uint16_t(eeShift_);
        else std::fill(std::begin(eeprom), std::end(eeprom), uint16_t(eeShift_));
        eepromDirty = true;
      }
      eeState_ = 5;
      break;
    case 4: eepromClock(); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------- tempo
void Jaguar::resetTomPit() {
  const uint32_t pre = reg16(0x50), div = reg16(0x52);
  tomPitNext_ = pre ? now_ + std::max<int64_t>(64, int64_t(pre + 1) * (div + 1)) : 0;
}

void Jaguar::resetJerryPit(int n) {
  const uint32_t pre = jPit_[n * 2], div = jPit_[n * 2 + 1];
  jPitNext_[n] = (pre | div) ? now_ + std::max<int64_t>(64, int64_t(pre + 1) * (div + 1)) : 0;
}

void Jaguar::timers() {
  if (tomPitNext_ && now_ >= tomPitNext_) {
    tomIrq(3);
    gpu.setIrq(2);
    const uint32_t pre = reg16(0x50), div = reg16(0x52);
    tomPitNext_ = pre ? tomPitNext_ + std::max<int64_t>(64, int64_t(pre + 1) * (div + 1)) : 0;
  }
  for (int n = 0; n < 2; n++)
    if (jPitNext_[n] && now_ >= jPitNext_[n]) {
      dsp.setIrq(2 + n);
      jerryIrq(2 + n);
      const uint32_t pre = jPit_[n * 2], div = jPit_[n * 2 + 1];
      jPitNext_[n] = (pre | div) ? jPitNext_[n] + std::max<int64_t>(64, int64_t(pre + 1) * (div + 1)) : 0;
    }
  if (blitIrq_ && now_ >= blitEnd_) { blitIrq_ = false; gpu.setIrq(4); }  // fim do blit: interrupção 4 do GPU
  if (i2sNext_ && now_ >= i2sNext_) {
    dsp.setIrq(1);
    i2sNext_ += 64 * (int64_t(sclk_) + 1);
  }
}

void Jaguar::runTo(int64_t t) {
  while (now_ < t) {
    int64_t step = std::min(t, now_ + 128);
    for (int64_t e : {tomPitNext_, jPitNext_[0], jPitNext_[1], i2sNext_, blitIrq_ ? blitEnd_ : 0})
      if (e > now_ && e < step) step = e;
    while (m68k_ < step) {
      // o 68000 tem a menor prioridade: espera o Blitter e o Object Processor largarem o barramento
      if (m68k_ < busFree_) { m68k_ = std::min(busFree_, step); continue; }
      accessTime = m68k_;
      const int c = cpu.step();
      cpu.cycles = 0;
      m68k_ += int64_t(c > 0 ? c : 4) * 2;
    }
    gpu.run(step);
    dsp.run(step);
    now_ = step;
    timers();
    // som: o valor atual do DAC, amostrado na taxa da saída
    const int64_t pos = now_ * sampleRate_;
    if (audNext_ == 0 || audNext_ > pos + clock()) audNext_ = pos;
    while (audNext_ <= pos) {
      const size_t cap = aud_.size();
      if (audW_ - audR_ + 2 > cap) audR_ += 2;  // ninguém está lendo: descarta o mais velho
      aud_[audW_ % cap] = int16_t(ltxd_) / 32768.0f;
      aud_[(audW_ + 1) % cap] = int16_t(rtxd_) / 32768.0f;
      audW_ += 2;
      audNext_ += clock();
    }
  }
}

void Jaguar::pullAudio(float* l, float* r, size_t n) {
  const size_t cap = aud_.size();
  for (size_t i = 0; i < n; i++) {
    if (audR_ < audW_) { l[i] = aud_[audR_ % cap]; r[i] = aud_[(audR_ + 1) % cap]; audR_ += 2; }
    else { l[i] = r[i] = 0; }
  }
}

// ---------------------------------------------------------------------------------- quadro
void Jaguar::beginFrame() {
  const int vdb = reg16(0x46), vde = reg16(0x48), vp = reg16(0x3E) & 0x7FF;
  top_ = (vdb > 0 && vdb < vp) ? vdb : 31;
  const int h = (vde > vdb) ? (vde - vdb) / 2 : 0;
  rows_ = (h > 0 && h <= FB_H / 2 && vde <= vp + 1) ? h : 240;
  const int pw = ((reg16(0x28) >> 9) & 7) + 1;
  const bool il = interlaced();
  const int w = VIS_HC / pw, oh = il ? rows_ * 2 : rows_;
  // entrelaçado: cada campo só redesenha as suas linhas (as do outro campo ficam)
  // só a área que vai ser mostrada (w x oh pontos, linha a linha com passo w)
  if (!il || w != outW || oh != outH) std::fill_n(fb.begin(), std::min(fb.size(), size_t(w) * oh), 0xFF000000u);
  outW = w;
  outH = oh;
}

// meias linhas por campo e tiques por meia linha, como os registros VP e HP mandam (com limites de segurança)
static int halvesOf(int vp) { return (vp >= 400 && vp <= 700) ? vp + 1 : 524; }
static int hlenOf(int hp) { return (hp >= 600 && hp <= 1000) ? hp + 1 : 845; }

double Jaguar::frameRate() const {
  return double(clock()) / (double(hlenOf(reg16(0x2E) & 0x3FF)) * halvesOf(reg16(0x3E) & 0x7FF));
}

void Jaguar::runFrame() {
  beginFrame();
  const int vp = reg16(0x3E) & 0x7FF, hp = reg16(0x2E) & 0x3FF;
  const int halves = halvesOf(vp), hlen = hlenOf(hp);
  // entrelaçado: o número de meias linhas é ímpar, e o segundo campo começa no meio de uma linha
  const int field = interlaced() ? int(frame_ & 1) : 0;
  for (int hl = 0; hl < halves; hl++) {
    vc_ = hl;
    hlStart_ = now_;
    if (hl == (reg16(0x4E) & 0x7FF)) tomIrq(0);
    if ((hl & 1) == field) opLine(hl, field);
    runTo(now_ + hlen);
  }
  frame_++;
}

// ---------------------------------------------------------------------------------- Object Processor
void Jaguar::opLine(int hl, int field) {
  const int vdb = reg16(0x46), vde = reg16(0x48), vp = reg16(0x3E);
  int start = vdb, end = vde;
  if (vde > vp) { start = 0; end = 511; }  // VDE além do quadro: o OP roda em todas as linhas
  if (hl < start || hl >= end) return;
  const uint16_t vmode = reg16(0x28);
  if (vmode & 0x80) {  // BGEN: limpa o buffer de linha com a cor de fundo
    const uint16_t bg = reg16(0x58);
    for (int i = 0; i < 720; i++) put16(&tom[0x1800 + i * 2], bg);
  }
  uint32_t p = ((uint32_t(reg16(0x22)) << 16) | reg16(0x20)) & 0xFFFFF8;
  accessTime = now_;
  opPhrases_ = 0;
  for (int guard = 0; guard < 4096; guard++) {
    const uint64_t p0 = phrase(p);
    opPhrases_++;
    const uint32_t link = uint32_t(p0 >> 21) & 0x3FFFF8;
    switch (p0 & 7) {
      case 0: case 1: {  // bitmap (1 = com escala)
        const bool scaled = (p0 & 7) == 1;
        const uint32_t ypos = (p0 >> 3) & 0x7FF;
        uint32_t height = (p0 >> 14) & 0x3FF;
        if (uint32_t(hl) >= ypos && height > 0) {
          const uint64_t p1 = phrase(p + 8);
          opPhrases_ += scaled ? 2 : 1;
          uint64_t pp0 = p0;
          const uint32_t dwidth = uint32_t((p1 >> 18) & 0x3FF) * 8;
          uint32_t data = uint32_t(p0 >> 40) & 0xFFFFF8;
          if (!scaled) {
            drawBitmap(p0, p1, 0x20);
            height--;
            data += dwidth;
            pp0 = (pp0 & ~0xFFFFF80000FFC000ull) | (uint64_t(height) << 14) | (uint64_t(data) << 40);
            storePhrase(p, pp0);
          } else {
            uint64_t p2 = phrase(p + 16);
            const uint32_t hs = uint32_t(p2 & 0xFF);
            if (hs) drawBitmap(p0, p1, hs);
            uint32_t rem = uint32_t(p2 >> 16) & 0xFF, vs = uint32_t(p2 >> 8) & 0xFF;
            if (vs == 0) vs = 0x20;
            if (rem < 0x20) {
              while (rem < 0x20) { rem += vs; if (height) height--; data += dwidth; }
              pp0 = (pp0 & ~0xFFFFF80000FFC000ull) | (uint64_t(height) << 14) | (uint64_t(data & 0xFFFFF8) << 40);
              storePhrase(p, pp0);
            }
            rem -= 0x20;
            p2 = (p2 & ~0xFF0000ull) | (uint64_t(rem & 0xFF) << 16);
            storePhrase(p + 16, p2);
          }
        }
        p = link;
        break;
      }
      case 2: {  // objeto do GPU: interrompe o GPU e espera o OBF
        ob_ = p0;
        obf_ = false;
        if (gpu.clk < now_) gpu.clk = now_;  // o GPU atende já, no começo da linha
        gpu.setIrq(3);
        for (int n = 0; n < 200 && !obf_ && gpu.running() && (gpu.regRead(0) & (1u << 7)); n++) gpu.run(gpu.clk + 32);
        if (!obf_) { guard = 1 << 30; break; }  // o GPU não liberou: a lista para nesta linha
        p += 8;
        break;
      }
      case 3: {  // desvio
        const uint32_t ypos = (p0 >> 3) & 0x7FF;
        bool take = false;
        switch ((p0 >> 14) & 7) {
          case 0: take = uint32_t(hl) == ypos || ypos == 0x7FF; break;
          case 1: take = uint32_t(hl) < ypos; break;
          case 2: take = uint32_t(hl) > ypos; break;
          case 3: take = opFlag_ & 1; break;
          default: break;  // 4: segunda metade da linha (o OP roda no começo)
        }
        p = take ? link : p + 8;
        break;
      }
      case 4:  // parada
        ob_ = p0;
        if (p0 & 8) tomIrq(2);
        guard = 1 << 30;
        break;
      default: p += 8; break;
    }
  }
  // o OP segura o barramento enquanto lê a lista e os dados (~2 tiques por phrase, no máximo meia linha)
  busFree_ = std::max(busFree_, now_ + std::min<int64_t>(int64_t(opPhrases_) * 2, 400));
  const int row = (hl - field - top_) >> 1;
  if (hl - field >= top_ && row < rows_) renderRow(interlaced() ? row * 2 + field : row);
}

// desenha uma linha de um bitmap no buffer de linha; hscale em 3.5 (0x20 = 1,0)
void Jaguar::drawBitmap(uint64_t p0, uint64_t p1, uint32_t hscale) {
  const int depth = int(p1 >> 12) & 7;
  if (depth > 5) return;
  const int bits = 1 << depth;
  const int ppp = 64 / bits;
  int xpos = int(p1 & 0xFFF);
  if (xpos & 0x800) xpos -= 0x1000;
  uint32_t iwidth = uint32_t(p1 >> 28) & 0x3FF;
  if (!iwidth) iwidth = 1;
  uint32_t data = uint32_t(p0 >> 40) & 0xFFFFF8;
  const uint32_t pitch = uint32_t((p1 >> 15) & 7) * 8;
  const bool reflect = (p1 >> 45) & 1, rmw = (p1 >> 46) & 1, trans = (p1 >> 47) & 1;
  const uint32_t index = uint32_t(p1 >> 37) & 0xFE;
  static const uint32_t FP_MASK[6] = {0x3E, 0x3C, 0x38, 0x30, 0x30, 0x20};
  const int skip = int(((p1 >> 49) & 0x3F & FP_MASK[depth]) >> depth);
  const bool wide = depth == 5;
  const int lbMax = wide ? 360 : 720;
  const int total = int(iwidth) * ppp;
  const int dir = reflect ? -1 : 1;
  int x = xpos;
  uint32_t acc = 0;
  uint64_t ph = 0;
  int phIdx = -1;
  for (int s = skip; s < total;) {
    const int k = s / ppp;
    if (k != phIdx) { ph = phrase(data + uint32_t(k) * pitch); phIdx = k; opPhrases_++; }
    const int i = s % ppp;
    const uint32_t pix = uint32_t(ph >> (64 - bits * (i + 1))) & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1));
    if (x >= 0 && x < lbMax && !(trans && pix == 0)) {
      if (wide) put32(&tom[0x1800 + x * 4], pix);
      else {
        uint16_t c;
        if (bits == 16) c = uint16_t(pix);
        else if (bits == 8) c = be16(&tom[0x400 + pix * 2]);
        else c = be16(&tom[0x400 + (((index & ~uint32_t((1 << bits) - 1)) | pix) & 0xFF) * 2]);
        uint8_t* d = &tom[0x1800 + x * 2];
        if (!rmw) put16(d, c);
        else {  // soma com saturação: cor (dois nibbles com sinal) e intensidade (byte com sinal)
          int y = d[1] + int8_t(c & 0xFF);
          int c1 = (d[0] & 15) + (int8_t(c >> 4 & 0xF0) >> 4), c2 = (d[0] >> 4) + (int8_t(c >> 8 & 0xF0) >> 4);
          y = std::clamp(y, 0, 255); c1 = std::clamp(c1, 0, 15); c2 = std::clamp(c2, 0, 15);
          d[0] = uint8_t((c2 << 4) | c1); d[1] = uint8_t(y);
        }
      }
    }
    x += dir;
    if ((dir > 0 && x >= lbMax) || (dir < 0 && x < 0)) break;
    if (hscale == 0x20) s++;
    else { acc += 0x20; while (acc >= hscale) { acc -= hscale; s++; } }
  }
}

// converte o buffer de linha em pontos da tela
void Jaguar::renderRow(int row) {
  if (row < 0 || row >= outH) return;
  uint32_t* dst = &fb[size_t(row) * outW];
  const uint16_t vmode = reg16(0x28);
  const int pw = ((vmode >> 9) & 7) + 1;
  const uint32_t border = 0xFF000000u | tom[0x2B] | (uint32_t(tom[0x2A]) << 8) | (uint32_t(tom[0x2D]) << 16);
  if (!(vmode & 1)) { std::fill(dst, dst + outW, 0xFF000000u); return; }
  int start = (int(int16_t(reg16(0x38))) - LEFT_HC) / pw;
  int i = 0, src = 0;
  if (start > 0) { for (; i < std::min(start, outW); i++) dst[i] = border; }
  else src = -start;
  const int mode = (vmode >> 1) & 3;
  const bool varmod = (vmode & 0x100) && mode == 0;
  for (; i < outW; i++, src++) {
    if (mode == 1) {  // RGB24: bytes G, R, -, B
      if (src >= 360) { dst[i] = border; continue; }
      const uint8_t* p = &tom[0x1800 + src * 4];
      dst[i] = 0xFF000000u | p[1] | (uint32_t(p[0]) << 8) | (uint32_t(p[3]) << 16);
      continue;
    }
    if (src >= 720) { dst[i] = border; continue; }
    const uint16_t c = be16(&tom[0x1800 + src * 2]);
    if (mode == 0 && !(varmod && (c & 1))) { dst[i] = cry_[c]; continue; }
    // RGB16: RRRRR BBBBB GGGGGG
    const uint32_t r = c >> 11, b = (c >> 6) & 31, g = c & 63;
    dst[i] = 0xFF000000u | ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16);
  }
}

// ---------------------------------------------------------------------------------- save state
void Jaguar::sync(StateIO& io) {
  cpu.sync(io);
  gpu.sync(io);
  dsp.sync(io);
  io.bytes(dram);
  io.raw(tom, sizeof tom);
  io.raw(blitRegs, sizeof blitRegs);
  io.raw(jmisc_, sizeof jmisc_);
  io.raw(eeprom, sizeof eeprom);
  io.sync(now_); io.sync(m68k_); io.sync(hlStart_); io.sync(vc_); io.sync(frame_);
  io.sync(tomEn_); io.sync(tomPend_); io.sync(jEn_); io.sync(jPend_);
  io.sync(ob_); io.sync(obf_); io.sync(opFlag_);
  io.sync(tomPitNext_); io.sync(jPitNext_[0]); io.sync(jPitNext_[1]); io.raw(jPit_, sizeof jPit_);
  io.sync(i2sNext_); io.sync(sclk_); io.sync(smode_); io.sync(ltxd_); io.sync(rtxd_); io.sync(joySel_);
  io.sync(eeState_); io.sync(eeBits_); io.sync(eeOutCnt_); io.sync(eeShift_); io.sync(eeAddr_); io.sync(eeOp_);
  io.sync(eeWen_); io.sync(pad);
  // barramento e Blitter, guardados como distância ao relógio (64 bits: blits na fila podem passar de 2^24 tiques)
  int64_t bf = std::clamp<int64_t>(busFree_ - now_, 0, MAX_AHEAD), be = std::clamp<int64_t>(blitEnd_ - now_, 0, MAX_AHEAD);
  io.sync(bf); io.sync(be); io.sync(blitIrq_);
  if (io.reading()) {
    // relógios de um estado adulterado: tudo perto de now_ (atrás demais, o processador levaria horas para alcançar)
    now_ = std::clamp<int64_t>(now_, 0, int64_t(1) << 50);
    busFree_ = now_ + std::clamp<int64_t>(bf, 0, MAX_AHEAD); blitEnd_ = now_ + std::clamp<int64_t>(be, 0, MAX_AHEAD);
    auto perto = [&](int64_t& t) { t = std::clamp<int64_t>(t, now_ - MAX_BEHIND, now_ + MAX_AHEAD); };
    perto(m68k_);
    gpu.rebase(now_ - MAX_BEHIND, now_ + MAX_AHEAD);
    dsp.rebase(now_ - MAX_BEHIND, now_ + MAX_AHEAD);
    for (int64_t* t : {&tomPitNext_, &jPitNext_[0], &jPitNext_[1], &i2sNext_}) if (*t) { perto(*t); if (!*t) *t = 1; }
    hlStart_ = std::clamp<int64_t>(hlStart_, now_ - MAX_BEHIND, now_);
    clampTo(eeState_, 0, 5); clampTo(eeBits_, 0, 16); clampTo(eeOutCnt_, 0, 16);
    eeAddr_ &= 0x3F; sclk_ &= 0xFF; vc_ &= 0x7FF;
    audNext_ = 0;
    updateIpl();
  }
}

std::vector<uint8_t> Jaguar::saveState() {
  StateIO io;
  char m[4];
  std::memcpy(m, STATE_MAGIC, 4);
  io.raw(m, 4);
  sync(io);
  return io.data();
}

void Jaguar::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  char m[4];
  io.raw(m, 4);
  if (std::memcmp(m, STATE_MAGIC, 4) != 0) throw std::runtime_error("Estado salvo não é do Jaguar.");
  const std::vector<uint8_t> backup = saveState();
  try { sync(io); }
  catch (...) { StateIO b(backup); char k[4]; b.raw(k, 4); sync(b); throw; }
}

// ---------------------------------------------------------------------------------- núcleo
// PAL pelo nome do arquivo (o cartucho do Jaguar não diz a região)
static bool palName(const std::string& nome) {
  std::string n = nome;
  for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (const char* t : {"(europe", "(e)", "(pal", "[pal", "(germany", "(france", "(uk)", "(spain", "(italy", "(australia",
                        "(netherlands", "(sweden", "(scandinavia", "(nordic", "(denmark", "(finland", "(norway", "(portugal",
                        "(austria", "(switzerland", "(belgium", "(ger)", "(fr)", "(eu)", "(a)", "(g)", "(f)", "(s)", "(i)"})
    if (n.find(t) != std::string::npos) return true;
  return false;
}

JaguarCore::JaguarCore(const std::vector<uint8_t>& rom, const std::string& nome, const std::vector<uint8_t>* save)
    : m_(std::make_unique<Jaguar>(rom, nome, palName(nome))) {
  if (!firmware("jaguar").empty()) m_->bios = firmware("jaguar");
  if (save) importSave(*save);
}

void JaguarCore::setKeys(uint32_t k) {
  uint32_t p = 0;
  if (k & KEY_UP) p |= J_UP;
  if (k & KEY_DOWN) p |= J_DOWN;
  if (k & KEY_LEFT) p |= J_LEFT;
  if (k & KEY_RIGHT) p |= J_RIGHT;
  if (k & KEY_A) p |= J_A;
  if (k & KEY_B) p |= J_B;
  if (k & KEY_X) p |= J_C;
  if (k & KEY_Y) p |= J_C;
  if (k & KEY_START) p |= J_PAUSE;
  if (k & KEY_SELECT) p |= J_OPTION;
  if (k & KEY_L) p |= J_STAR;
  if (k & KEY_R) p |= J_HASH;
  m_->pad = (m_->pad & ~0x7FFu) | p;
}

std::vector<uint8_t> JaguarCore::exportSave() {
  m_->eepromDirty = false;
  std::vector<uint8_t> b(128);
  for (int i = 0; i < 64; i++) { b[i * 2] = uint8_t(m_->eeprom[i] >> 8); b[i * 2 + 1] = uint8_t(m_->eeprom[i]); }
  return b;
}

void JaguarCore::importSave(const std::vector<uint8_t>& b) {
  for (size_t i = 0; i < 64 && i * 2 + 1 < b.size(); i++) m_->eeprom[i] = uint16_t((b[i * 2] << 8) | b[i * 2 + 1]);
}

std::string JaguarCore::note() const {
  return std::string("Atari Jaguar") + (m_->pal ? " (PAL 50 Hz)" : "") + ": partida sem a ROM do console; teclas C = X/Y, Option = Select, Pause = Start, "
         "* e # = L e R.";
}

}  // namespace indigo::jaguar
