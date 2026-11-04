// Mapa de memória do Dreamcast (endereços físicos de 29 bits), Holly (registradores SB, interrupções, DMA do canal
// 2, DMA do GD-ROM, do AICA e da PVR) e o Maple (controle na porta A com um VMU no encaixe 1).
//   0x00000000 BIOS (2 MB)            0x00200000 flash (128 KB)       0x005F6800 Holly SB (0x005F7000 GD-ROM)
//   0x005F8000 PowerVR2 (registradores, neblina, paleta)              0x00700000 AICA   0x00710000 relógio
//   0x00800000 RAM de som (2 MB)      0x04000000 VRAM (64 bits)       0x05000000 VRAM (32 bits)
//   0x0C000000 RAM (16 MB, 4 espelhos) 0x10000000 FIFO do TA (0x11000000 caminho direto de textura)
#include <algorithm>

#include "dc.h"

namespace indigo::dc {

namespace {
enum : uint32_t {
  C2DSTAT = 0x6800, C2DLEN = 0x6804, C2DST = 0x6808, SDST = 0x6820, LMMODE0 = 0x6884, LMMODE1 = 0x6888,
  SBREV = 0x689C, ISTNRM = 0x6900, ISTEXT = 0x6904, ISTERR = 0x6908, IML2NRM = 0x6910, IML6ERR = 0x6938,
  MDSTAR = 0x6C04, MDTSEL = 0x6C10, MDEN = 0x6C14, MDST = 0x6C18, GDSTAR = 0x7404, GDLEN = 0x7408,
  GDDIR = 0x740C, GDEN = 0x7414, GDST = 0x7418, GDSTARD = 0x74F4, GDLEND = 0x74F8, ADSTAG = 0x7800,
  PDSTAP = 0x7C00, PDSTAR = 0x7C04, PDLEN = 0x7C08, PDDIR = 0x7C0C, PDEN = 0x7C14, PDST = 0x7C18,
};
inline uint32_t& SB(System& s, uint32_t off) { return s.sb[(off - 0x6800) >> 2]; }
}  // namespace

void System::buildMap() {
  std::fill(std::begin(rmap), std::end(rmap), nullptr);
  std::fill(std::begin(wmap), std::end(wmap), nullptr);
  for (uint32_t p = 0; p < 0x20; p++) rmap[p] = boot.data() + (p << 16);
  for (uint32_t p = 0; p < 2; p++) rmap[0x20 + p] = flash.data() + (p << 16);
  for (uint32_t p = 0x80; p < 0x100; p++) rmap[p] = wmap[p] = aram.data() + ((p & 0x1F) << 16);
  for (uint32_t base : {0x0400u, 0x0600u})
    for (uint32_t p = 0; p < 0x100; p++) rmap[base + p] = wmap[base + p] = vram.data() + ((p & 0x7F) << 16);
  for (uint32_t p = 0x0C00; p < 0x1000; p++) rmap[p] = wmap[p] = ram.data() + ((p & 0xFF) << 16);
}

// ---------------------------------------------------------------- caminho lento
uint32_t System::read32(uint32_t pa) {
  pa &= 0x1FFFFFFC;
  if (uint8_t* p = rmap[pa >> 16]) { uint32_t v; std::memcpy(&v, p + (pa & 0xFFFF), 4); return v; }
  if (pa >= 0x005F7000 && pa < 0x005F7100) return gdrom.read(pa & 0xFF, 4);
  if (pa >= 0x005F6800 && pa < 0x005F8000) return sbRead(pa & 0xFFFF);
  if (pa >= 0x005F8000 && pa < 0x005FA000) return pvr.read(pa - 0x005F8000);
  if (pa >= 0x00700000 && pa < 0x00720000) return aica.regRead(pa & 0x1FFFF, 4);
  if ((pa >> 24) == 5 || (pa >> 24) == 7) return vramRead32(pa);
  return 0;
}
uint16_t System::read16(uint32_t pa) {
  pa &= 0x1FFFFFFE;
  if (uint8_t* p = rmap[pa >> 16]) { uint16_t v; std::memcpy(&v, p + (pa & 0xFFFF), 2); return v; }
  if (pa >= 0x005F7000 && pa < 0x005F7100) return uint16_t(gdrom.read(pa & 0xFF, 2));
  if (pa >= 0x00700000 && pa < 0x00720000) return uint16_t(aica.regRead(pa & 0x1FFFF, 2));
  if ((pa >> 24) == 5 || (pa >> 24) == 7) { uint16_t v; std::memcpy(&v, &vram[vram32(pa & 0x7FFFFE)], 2); return v; }
  return uint16_t(read32(pa & ~3u) >> ((pa & 2) * 8));
}
uint8_t System::read8(uint32_t pa) {
  pa &= 0x1FFFFFFF;
  if (uint8_t* p = rmap[pa >> 16]) return p[pa & 0xFFFF];
  if (pa >= 0x005F7000 && pa < 0x005F7100) return uint8_t(gdrom.read(pa & 0xFF, 1));
  if (pa >= 0x00700000 && pa < 0x00720000) return uint8_t(aica.regRead(pa & 0x1FFFF, 1));
  if ((pa >> 24) == 5 || (pa >> 24) == 7) return vram[vram32(pa & 0x7FFFFF)];
  return uint8_t(read32(pa & ~3u) >> ((pa & 3) * 8));
}

void System::write32(uint32_t pa, uint32_t v) {
  pa &= 0x1FFFFFFC;
  if (uint8_t* p = wmap[pa >> 16]) { std::memcpy(p + (pa & 0xFFFF), &v, 4); return; }
  if (pa >= 0x10000000 && pa < 0x14000000) {
    if ((pa & 0x01000000) == 0) { pvr.taWriteWords(pa, v); return; }
    // caminho direto de textura (0x11000000/0x13000000): 64 bits, ou 32 bits com o LMMODE ligado
    const uint32_t off = pa & 0x7FFFFF;
    const bool lin = (pa & 0x02000000) ? (SB(*this, LMMODE1) & 1) : (SB(*this, LMMODE0) & 1);
    if (lin) vramWrite32(off, v); else std::memcpy(&vram[off], &v, 4);
    return;
  }
  if (pa >= 0x005F7000 && pa < 0x005F7100) { gdrom.write(pa & 0xFF, v, 4); return; }
  if (pa >= 0x005F6800 && pa < 0x005F8000) { sbWrite(pa & 0xFFFF, v); return; }
  if (pa >= 0x005F8000 && pa < 0x005FA000) { pvr.write(pa - 0x005F8000, v); return; }
  if (pa >= 0x00700000 && pa < 0x00720000) { aica.regWrite(pa & 0x1FFFF, v, 4); return; }
  if ((pa >> 24) == 5 || (pa >> 24) == 7) { vramWrite32(pa, v); return; }
  if (pa >= 0x00200000 && pa < 0x00220000) { flashWrite(pa & 0x1FFFF, uint8_t(v)); return; }
}
void System::write16(uint32_t pa, uint16_t v) {
  pa &= 0x1FFFFFFE;
  if (uint8_t* p = wmap[pa >> 16]) { std::memcpy(p + (pa & 0xFFFF), &v, 2); return; }
  if (pa >= 0x005F7000 && pa < 0x005F7100) { gdrom.write(pa & 0xFF, v, 2); return; }
  if (pa >= 0x00700000 && pa < 0x00720000) { aica.regWrite(pa & 0x1FFFF, v, 2); return; }
  if ((pa >> 24) == 5 || (pa >> 24) == 7) { std::memcpy(&vram[vram32(pa & 0x7FFFFE)], &v, 2); return; }
  if (pa >= 0x00200000 && pa < 0x00220000) { flashWrite(pa & 0x1FFFF, uint8_t(v)); return; }
  if (pa >= 0x005F6800 && pa < 0x005FA000) write32(pa & ~3u, v);
}
void System::write8(uint32_t pa, uint8_t v) {
  pa &= 0x1FFFFFFF;
  if (uint8_t* p = wmap[pa >> 16]) { p[pa & 0xFFFF] = v; return; }
  if (pa >= 0x005F7000 && pa < 0x005F7100) { gdrom.write(pa & 0xFF, v, 1); return; }
  if (pa >= 0x00700000 && pa < 0x00720000) { aica.regWrite(pa & 0x1FFFF, v, 1); return; }
  if ((pa >> 24) == 5 || (pa >> 24) == 7) { vram[vram32(pa & 0x7FFFFF)] = v; return; }
  if (pa >= 0x00200000 && pa < 0x00220000) { flashWrite(pa & 0x1FFFF, v); return; }
  if (pa >= 0x005F6800 && pa < 0x005FA000) write32(pa & ~3u, v);
}

void System::write32B(uint32_t pa, const uint32_t* d) {
  pa &= 0x1FFFFFE0;
  if (pa >= 0x10000000 && pa < 0x14000000 && (pa & 0x01000000) == 0) {
    if (pa & 0x00800000) pvr.yuvWrite(d); else pvr.taWrite(d);
    return;
  }
  for (int i = 0; i < 8; i++) write32(pa + 4 * uint32_t(i), d[i]);
}

// flash AMD (MBM29LV001): AA/55 nos endereços 5555/2AAA, depois A0 = grava um byte, 80 ... 30 = apaga o setor
void System::flashWrite(uint32_t off, uint8_t v) {
  const uint32_t a = off & 0x7FFF;
  switch (flashState_) {
    case 0: case 3: flashState_ = (a == 0x5555 && v == 0xAA) ? flashState_ + 1 : 0; break;
    case 1: case 4: flashState_ = (a == 0x2AAA && v == 0x55) ? flashState_ + 1 : 0; break;
    case 2:
      flashState_ = 0;
      if (a == 0x5555 && v == 0xA0) flashState_ = 6;
      else if (a == 0x5555 && v == 0x80) flashState_ = 3;
      break;
    case 5:
      flashState_ = 0;
      if (v == 0x30) {  // apaga o setor de 16 KB
        std::fill_n(flash.begin() + (off & ~0x3FFFu), 0x4000, uint8_t(0xFF));
        flashDirty = true;
      } else if (v == 0x10) {
        std::fill(flash.begin(), flash.end(), uint8_t(0xFF));
        flashDirty = true;
      }
      break;
    case 6:
      flash[off] &= v;
      flashDirty = true;
      flashState_ = 0;
      break;
    default: flashState_ = 0;
  }
}

// ---------------------------------------------------------------- Holly
void System::updateIrq() {
  uint32_t nrm = istnrm & 0x3FFFFF;
  const uint32_t lv[3][3] = {{SB(*this, 0x6930), SB(*this, 0x6934), SB(*this, 0x6938)},
                             {SB(*this, 0x6920), SB(*this, 0x6924), SB(*this, 0x6928)},
                             {SB(*this, 0x6910), SB(*this, 0x6914), SB(*this, 0x6918)}};
  int irl = 15;
  static const int irlOf[3] = {9, 11, 13};  // nível 6, 4 e 2 do Holly
  for (int k = 0; k < 3; k++)
    if ((nrm & lv[k][0]) || (istext & lv[k][1]) || (isterr & lv[k][2])) { irl = irlOf[k]; break; }
  cpu.setIrl(irl);
}

uint32_t System::sbRead(uint32_t off) {
  switch (off) {
    case ISTNRM: return (istnrm & 0x3FFFFF) | (istext ? 0x40000000u : 0) | (isterr ? 0x80000000u : 0);
    case ISTEXT: return istext;
    case ISTERR: return isterr;
    case SBREV: return 0x0B;
    case 0x688C: return 0;  // SB_FFST: FIFOs vazias
    case 0x7480: return SB(*this, off);
    default: break;
  }
  if (off >= 0x6800 && off < 0x8000) return SB(*this, off);
  return 0;
}

void System::sbWrite(uint32_t off, uint32_t v) {
  switch (off) {
    case ISTNRM: istnrm &= ~(v & 0x3FFFFF); updateIrq(); return;
    case ISTEXT: return;  // apagado na origem
    case ISTERR: isterr &= ~v; updateIrq(); return;
    case C2DST:
      SB(*this, off) = v & 1;
      if (v & 1) runDma([&] { ch2Dma(); });
      return;
    case SDST:
      if (v & 1) raiseNrm(20);  // DMA de ordenação: só avisa que terminou
      return;
    case MDST:
      SB(*this, off) = v & 1;
      if ((v & 1) && (SB(*this, MDEN) & 1)) runDma([&] { maple.dma(SB(*this, MDSTAR) & 0x1FFFFFE0); schedule(EV_MAPLE, 20000); });
      return;
    case GDST:
      SB(*this, off) = v & 1;
      if ((v & 1) && (SB(*this, GDEN) & 1)) runDma([&] { gdrom.dmaTransfer(); });
      return;
    case PDST:
      SB(*this, off) = v & 1;
      if ((v & 1) && (SB(*this, PDEN) & 1)) runDma([&] { pvrDma(); });
      return;
    case 0x6890:  // SB_SFRES: 0x7611 reinicia o sistema
      if (v == 0x7611) cpu.reset();
      return;
    default: break;
  }
  if (off >= 0x7800 && off < 0x7880 && (off & 0x1F) == 0x18) {  // SB_ADST (canais 0-3 do G2)
    SB(*this, off) = v & 1;
    if (v & 1) runDma([&] { g2Dma(int((off - 0x7800) >> 5)); });
    return;
  }
  if (off >= 0x6910 && off <= IML6ERR) { SB(*this, off) = v; updateIrq(); return; }
  if (off >= 0x6800 && off < 0x8000) SB(*this, off) = v;
}

void System::ch2Dma() {
  uint32_t src = cpu.sar[2] & 0x1FFFFFE0, dst = SB(*this, C2DSTAT), len = SB(*this, C2DLEN);
  if (!len) len = cpu.dmatcr[2] * 32;
  len = std::min<uint32_t>(len, 16u << 20);
  uint32_t blk[8];
  if ((dst & 0x01000000) == 0 && (dst >> 26) == 4) {  // FIFO do TA (polígonos ou YUV)
    for (uint32_t i = 0; i < len; i += 32) {
      for (int k = 0; k < 8; k++) blk[k] = read32(src + i + 4 * uint32_t(k));
      if (dst & 0x00800000) pvr.yuvWrite(blk); else pvr.taWrite(blk);
    }
  } else {
    for (uint32_t i = 0; i < len; i += 4) write32((dst & 0x1FFFFFFF) + i, read32(src + i));
    SB(*this, C2DSTAT) = dst + len;
  }
  cpu.sar[2] = (cpu.sar[2] + len);
  cpu.dmatcr[2] = 0;
  cpu.chcr[2] |= 2;
  SB(*this, C2DLEN) = 0;
  schedule(EV_CH2, 2000);
}

void System::g2Dma(int ch) {
  const uint32_t b = 0x7800 + uint32_t(ch) * 0x20;
  if (!(SB(*this, b + 0x14) & 1)) { SB(*this, b + 0x18) = 0; return; }
  uint32_t aicaA = SB(*this, b) & 0x1FFFFFE0, sh = SB(*this, b + 4) & 0x1FFFFFE0;
  const uint32_t len = std::min<uint32_t>(SB(*this, b + 8) & 0x7FFFFFE0, 8u << 20);
  const bool toSh = SB(*this, b + 0xC) & 1;
  for (uint32_t i = 0; i < len; i += 4) {
    if (toSh) write32(sh + i, read32(aicaA + i)); else write32(aicaA + i, read32(sh + i));
  }
  SB(*this, b) = aicaA + len; SB(*this, b + 4) = sh + len;
  if (SB(*this, b + 8) & 0x80000000u) SB(*this, b + 0x14) = 0;
  SB(*this, b + 8) = 0;
  SB(*this, b + 0x18) = 0;
  g2Pending_ |= 1u << ch;
  schedule(EV_G2DMA, 4000);
}

void System::pvrDma() {
  const uint32_t vr = SB(*this, PDSTAP) & 0x1FFFFFE0, sh = SB(*this, PDSTAR) & 0x1FFFFFE0;
  const uint32_t len = std::min<uint32_t>(SB(*this, PDLEN), 8u << 20);
  const bool toSh = SB(*this, PDDIR) & 1;
  for (uint32_t i = 0; i < len; i += 4) {
    if (toSh) write32(sh + i, read32(vr + i)); else write32(vr + i, read32(sh + i));
  }
  SB(*this, PDST) = 0;
  schedule(EV_PVRDMA, 4000);
}

void System::doEvent(int e) {
  evAt[e] = INT64_MAX;
  switch (e) {
    case EV_RENDER: raiseNrm(2); raiseNrm(1); raiseNrm(0); break;
    case EV_MAPLE: SB(*this, MDST) = 0; raiseNrm(12); break;
    case EV_CH2: SB(*this, C2DST) = 0; raiseNrm(19); break;
    case EV_GDDMA: SB(*this, GDST) = 0; raiseNrm(14); gdrom.dmaDone(); break;
    case EV_GDROM: gdrom.event(); break;
    case EV_G2DMA:
      for (int ch = 0; ch < 4; ch++) if (g2Pending_ & (1u << ch)) raiseNrm(15 + ch);
      g2Pending_ = 0;
      break;
    case EV_PVRDMA: raiseNrm(11); break;
    default: break;
  }
}

// ---------------------------------------------------------------- Maple
Maple::Maple(System& s) : sys(s) { vmu = blankVmu(); }

namespace {
void putStr(uint8_t* d, const char* s, size_t n) {
  size_t i = 0;
  for (; s[i] && i < n; i++) d[i] = uint8_t(s[i]);
  for (; i < n; i++) d[i] = ' ';
}
const char* LICENSE = "Produced By or Under License From SEGA ENTERPRISES,LTD.";
}  // namespace

// um quadro para o aparelho (porta, unidade 0 = principal ou 1 = encaixe 1); devolve as palavras da resposta
uint32_t Maple::device(int port, int unit, uint32_t* req, int words, uint32_t* resp, uint32_t& hdr) {
  const uint32_t cmd = req[0] & 0xFF, sender = (req[0] >> 16) & 0xFF;
  const uint32_t me = uint32_t(port << 6) | (unit == 0 ? 0x21u : 0x01u);  // principal avisa do VMU no encaixe 1
  auto header = [&](uint32_t code, uint32_t len) { hdr = code | sender << 8 | me << 16 | len << 24; };
  const uint32_t fn = words > 1 ? req[1] : 0;
  if (unit == 0) {  // controle
    switch (cmd) {
      case 1: case 3: {
        std::memset(resp, 0, 112);
        resp[0] = 0x01000000; resp[1] = 0xFE060F00; resp[2] = 0; resp[3] = 0;
        uint8_t* b = reinterpret_cast<uint8_t*>(resp);
        b[16] = 0xFF; b[17] = 0;
        putStr(b + 18, "Dreamcast Controller", 30);
        putStr(b + 48, LICENSE, 60);
        b[108] = 0xAE; b[109] = 0x01; b[110] = 0xF4; b[111] = 0x01;
        header(5, 28);
        return 28;
      }
      case 9: {
        if (fn != 0x01000000) { header(0xFC, 0); return 0; }
        uint16_t bt = 0xFFFF;
        auto k = [&](uint32_t key, uint16_t bit) { if (keys & key) bt &= uint16_t(~bit); };
        k(KEY_A, 0x0004); k(KEY_B, 0x0002); k(KEY_X, 0x0400); k(KEY_Y, 0x0200); k(KEY_START, 0x0008);
        k(KEY_UP, 0x0010); k(KEY_DOWN, 0x0020); k(KEY_LEFT, 0x0040); k(KEY_RIGHT, 0x0080);
        k(KEY_SELECT, 0x0800);  // D (o Select do Índigo; quase nenhum jogo usa)
        const uint32_t rt = (keys & KEY_R) ? 0xFF : 0, lt = (keys & KEY_L) ? 0xFF : 0;
        resp[0] = 0x01000000;
        resp[1] = bt | rt << 16 | lt << 24;
        resp[2] = 0x80808080;
        header(8, 3);
        return 3;
      }
      case 0xE: header(7, 0); return 0;  // vibração, etc.
      default: header(0xFD, 0); return 0;
    }
  }
  // VMU: armazenamento (2), tela (4) e relógio (8)
  switch (cmd) {
    case 1: case 3: {
      std::memset(resp, 0, 112);
      resp[0] = 0x0E000000; resp[1] = 0x7E7E3F40; resp[2] = 0x00051000; resp[3] = 0x000F4100;
      uint8_t* b = reinterpret_cast<uint8_t*>(resp);
      b[16] = 0xFF; b[17] = 0;
      putStr(b + 18, "Visual Memory", 30);
      putStr(b + 48, LICENSE, 60);
      b[108] = 0x7C; b[109] = 0x00; b[110] = 0x82; b[111] = 0x00;
      header(5, 28);
      return 28;
    }
    case 0xA: {  // informações da memória
      if (fn != 0x02000000) { header(0xFC, 0); return 0; }
      static const uint16_t info[12] = {255, 0, 255, 254, 1, 253, 13, 0, 200, 31, 0, 0};
      resp[0] = fn;
      std::memcpy(&resp[1], info, sizeof info);
      header(8, 7);
      return 7;
    }
    case 0xB: {  // ler bloco
      if (fn != 0x02000000 || words < 3) { header(0xFC, 0); return 0; }
      const uint32_t loc = req[2], blk = (((loc >> 16) & 0xFF) << 8 | (loc >> 24)) & 0xFF;
      resp[0] = fn; resp[1] = loc;
      std::memcpy(&resp[2], &vmu[blk * 512], 512);
      header(8, 130);
      return 130;
    }
    case 0xC: {  // gravar bloco (128 bytes por fase)
      if (fn == 0x04000000) { header(7, 0); return 0; }  // tela do VMU: aceita e ignora
      if (fn != 0x02000000 || words < 3) { header(0xFC, 0); return 0; }
      const uint32_t loc = req[2], blk = (((loc >> 16) & 0xFF) << 8 | (loc >> 24)) & 0xFF, ph = (loc >> 8) & 3;
      const int n = std::min(words - 3, 32);
      if (n > 0) {
        std::memcpy(&vmu[blk * 512 + ph * 128], &req[3], size_t(n) * 4);
        vmuDirty = true;
      }
      header(7, 0);
      return 0;
    }
    case 0xD: header(7, 0); return 0;  // fim da gravação
    case 9: {
      if (fn == 0x08000000) {  // relógio: devolve o do AICA (sem conversão de data)
        resp[0] = fn; resp[1] = 0x07D00101; resp[2] = 0x00000000;
        header(8, 3);
        return 3;
      }
      header(0xFC, 0);
      return 0;
    }
    case 0xE: header(7, 0); return 0;
    default: header(0xFD, 0); return 0;
  }
}

void Maple::dma(uint32_t list) {
  dmas++;
  uint32_t req[256], resp[256];
  for (int guard = 0; guard < 64; guard++) {
    const uint32_t d0 = sys.read32(list), dest = sys.read32(list + 4) & 0x1FFFFFE0;
    const int len = int(d0 & 0xFF), pattern = int((d0 >> 8) & 7);
    list += 8;
    if (pattern == 0) {
      const int words = std::min(len + 1, 256);
      for (int i = 0; i < words; i++) req[i] = sys.read32(list + 4 * uint32_t(i));
      list += 4 * uint32_t(len + 1);
      const uint32_t to = (req[0] >> 8) & 0xFF;
      const int port = int(to >> 6);
      const int unit = (to & 0x20) ? 0 : (to & 0x01) ? 1 : -1;
      uint32_t hdr = 0;
      uint32_t n = 0;
      if (port == 0 && unit >= 0) n = device(port, unit, req, words, resp, hdr);
      else hdr = 0xFFFFFFFF;
      sys.write32(dest, hdr);
      for (uint32_t i = 0; i < n; i++) sys.write32(dest + 4 + 4 * i, resp[i]);
    }
    if (d0 & 0x80000000u) break;
  }
}

void Maple::sync(StateIO& io) {
  io.bytes(vmu);
  io.sync(vmuDirty);
}

// VMU formatado: bloco 255 = raiz, 254 = FAT, 253-241 = diretório, 0-199 livres para os saves
std::vector<uint8_t> blankVmu() {
  std::vector<uint8_t> v(VMU_SIZE, 0);
  auto w16 = [&](uint32_t off, uint16_t x) { v[off] = uint8_t(x); v[off + 1] = uint8_t(x >> 8); };
  uint8_t* root = &v[255 * 512];
  std::fill_n(root, 16, uint8_t(0x55));
  root[0x10] = 1;  // cor própria do VMU ligada
  root[0x11] = 0xFF; root[0x12] = 0xFF; root[0x13] = 0xFF; root[0x14] = 0xFF;
  // data da formatação (BCD): 2000-01-01 00:00:00, sábado
  root[0x30] = 0x20; root[0x31] = 0x00; root[0x32] = 0x01; root[0x33] = 0x01; root[0x37] = 0x06;
  w16(255 * 512 + 0x46, 254); w16(255 * 512 + 0x48, 1);    // FAT
  w16(255 * 512 + 0x4A, 253); w16(255 * 512 + 0x4C, 13);   // diretório
  w16(255 * 512 + 0x4E, 0);                                // ícone
  w16(255 * 512 + 0x50, 200);                              // blocos de usuário
  w16(255 * 512 + 0x52, 31);
  for (int b = 0; b < 256; b++) w16(254 * 512 + 2 * uint32_t(b), 0xFFFC);  // livre
  w16(254 * 512 + 2 * 255, 0xFFFA);
  w16(254 * 512 + 2 * 254, 0xFFFA);
  for (int b = 253; b > 241; b--) w16(254 * 512 + 2 * uint32_t(b), uint16_t(b - 1));
  w16(254 * 512 + 2 * 241, 0xFFFA);
  return v;
}

}  // namespace indigo::dc
