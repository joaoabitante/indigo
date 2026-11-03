// Partes adaptadas do Mednafen (Beetle PC-FX, Mednafen Team, GPL-2.0 ou posterior); veja native/TERCEIROS.md.
#include "pcfx.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::pcfx {

namespace {
constexpr int64_t NEVER = INT64_MAX;
constexpr int SRC_TIMER = 1, SRC_INPUT = 3, SRC_VDCA = 4, SRC_KING = 5, SRC_VDCB = 6;
// linhas do VDC (contagem do PC Engine) que viram as linhas 22 a 261 do HuC6261: linha = (vdc + 3) % total
constexpr int VDC_FIRST = 19, VDC_END = 259;

// YUV -> RGB (coeficientes da norma, como no Mednafen)
struct Yuv {
  int16_t r[65536], g[65536], b[65536];
  Yuv() {
    for (int ur = 0; ur < 256; ur++)
      for (int vr = 0; vr < 256; vr++) {
        const int u = ur - 128, v = vr - 128, i = vr + ur * 256;
        r[i] = int16_t(int(0 - 0.000039457070707 * u + 1.139827967171717 * v));
        g[i] = int16_t(int(0 - 0.394610164141414 * u - 0.580500315656566 * v));
        b[i] = int16_t(int(0 + 2.031999684343434 * u - 0.000481376262626 * v));
      }
  }
};
const Yuv& yuvLut() { static const Yuv t; return t; }
inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
inline uint32_t toRgb(uint32_t yuv) {
  const Yuv& t = yuvLut();
  const int y = (yuv >> 16) & 0xFF;
  const int i = yuv & 0xFFFF;
  return 0xFF000000u | uint32_t(clamp8(y + t.b[i])) << 16 | uint32_t(clamp8(y + t.g[i])) << 8 | clamp8(y + t.r[i]);
}

// ordem de 3 camadas (VDC, fundos do KING, RAINBOW) pelas prioridades já remapeadas (0 = desligada):
// [p0][p1][p2][camada] = 0 (atrás), 1, 2 (frente) ou 3 (não aparece)
struct PrioMap {
  uint8_t m[8][8][8][3];
  PrioMap() {
    for (int a = 0; a < 8; a++)
      for (int b = 0; b < 8; b++)
        for (int c = 0; c < 8; c++) {
          const int p[3] = {a ? a : 0x10, b ? b : 0x10, c ? c : 0x10};
          for (int k = 0; k < 3; k++) {
            const int o1 = p[(k + 1) % 3], o2 = p[(k + 2) % 3];
            if (p[k] >= 8) m[a][b][c][k] = 3;
            else if (p[k] < o1 && p[k] < o2) m[a][b][c][k] = 0;
            else if (p[k] > o1 && p[k] > o2) m[a][b][c][k] = 2;
            else m[a][b][c][k] = 1;
          }
        }
  }
};
const PrioMap& prioMap() { static const PrioMap t; return t; }

std::string lowerStr(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

bool isPcfxBios(const std::vector<uint8_t>& b, const std::string& n) {
  if (b.size() != 0x100000) return false;
  const std::string l = lowerStr(n);
  // só .rom/.bin: um jogo de 1 MB de outro console com "pcfx" no nome não vira BIOS
  const auto termina = [&](const char* e) { const size_t k = std::strlen(e); return l.size() >= k && l.compare(l.size() - k, k, e) == 0; };
  if (!termina(".rom") && !termina(".bin")) return false;
  return l.find("pcfx") != std::string::npos || l.find("pc-fx") != std::string::npos || l.find("pc_fx") != std::string::npos;
}

bool isPcfxDisc(cdrom::Disc& d) {
  for (const auto& t : d.tracks()) {
    if (t.type == cdrom::TrackType::Audio) continue;
    uint8_t buf[cdrom::DATA_SECTOR];
    return d.readData(t.start, buf) && std::memcmp(buf, "PC-FX:Hu_CD-ROM", 15) == 0;
  }
  return false;
}

Pcfx::Pcfx(const std::vector<uint8_t>& bios, cdrom::Disc* disc)
    : cpu(*this), cd(disc), king(cd, vce), sound(king, cd), bios_(bios) {
  if (bios_.size() != 0x100000) throw std::runtime_error("a BIOS do PC-FX tem de ter 1 MB (pcfx.rom).");
  for (int i = 0; i < 512; i++) vdcVce.pal[i] = uint16_t(i);  // paleta identidade: o VDC entrega os índices
  vdcVce.div = 4;
  vdcVce.lines = 263;
  for (int i = 0; i < 2; i++) {
    vdc[i].vce = &vdcVce;
    vdc[i].sys = this;
    vdc[i].index = i;
    vdc[i].vramWords = 0x10000;
    vdc[i].drawFirst = VDC_FIRST;
    vdc[i].drawEnd = VDC_END;
  }
  sound.setSampleRate(48000);
  // memória de save formatada (cabeçalho do sistema de arquivos da BIOS), como sai de fábrica
  static const uint8_t init00[] = {0x24, 0x8A, 0xDF, 0x50, 0x43, 0x46, 0x58, 0x53, 0x72, 0x61, 0x6D, 0x80, 0x00, 0x01, 0x01, 0x00,
                                   0x01, 0x40, 0x00, 0x00, 0x01, 0xF9, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t ex00[] = {0x24, 0x8A, 0xDF, 0x50, 0x43, 0x46, 0x58, 0x43, 0x61, 0x72, 0x64, 0x80, 0x00, 0x01, 0x01, 0x00,
                                 0x01, 0x40, 0x00, 0x00, 0x01, 0xF9, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t init80[] = {0xF9, 0xFF, 0xFF};
  std::memcpy(&bram[0], init00, sizeof init00);
  std::memcpy(&bram[0x80], init80, sizeof init80);
  std::memcpy(&bram[0x8000], ex00, sizeof ex00);
  std::memcpy(&bram[0x8080], init80, sizeof init80);
  reset();
}

void Pcfx::reset() {
  std::fill(ram.begin(), ram.end(), 0);
  irqAsserted_ = 0; irqMask_ = 0xFFFF; irqPrio_[0] = irqPrio_[1] = 0;
  tmrCtrl_ = tmrPeriod_ = 0; tmrCounter_ = 0; tmrLast_ = ts;
  for (int i = 0; i < 2; i++) {
    padCtrl_[i] = 0; padLatched_[i] = false; padLatchAt_[i] = NEVER; padData_[i] = 0;
    mouseX_[i] = mouseY_[i] = 0; mouseBtn_[i] = 0;
  }
  backupCtrl_ = 0; lastVdcAr_[0] = lastVdcAr_[1] = 0; exBusReset_ = 0; ramPage_ = 0;
  // o HuC6261 e os VDCs começam juntos: a linha 0 do VDC é a linha 3 do HuC6261
  for (int i = 0; i < 2; i++) { vdc[i].power(); vdc[i].fixTime(ts); }
  vdcVce.div = 4;
  vdcVce.lines = 263;
  lines_ = 263;
  vce.raster = 2;
  vce.inHblank = true;
  lineBase_ = ts;
  lineEvt_ = 0;
  nextLineEvt_ = ts + 48;
  vdcIrq_[0] = vdcIrq_[1] = false;
  rowDone_[0] = rowDone_[1] = -1;
  king.reset(ts);
  sound.reset(ts);
  cpu.reset();
  irqUpdate();
}

// ------------------------------------------------------------------ interrupções
void Pcfx::irqAssert(int src, bool on) {
  const uint16_t bit = uint16_t(1 << (7 - src));
  const uint16_t old = irqAsserted_;
  irqAsserted_ = on ? (irqAsserted_ | bit) : (irqAsserted_ & ~bit);
  if (old != irqAsserted_) irqUpdate();
}

void Pcfx::irqUpdate() {
  const uint32_t pend = irqAsserted_ & ~irqMask_;
  int level = -1, last = -1;
  for (int lv = 8; lv < 16; lv++)
    if (pend & (1u << (15 - lv))) {
      const int p = lv >= 12 ? (irqPrio_[0] >> ((15 - lv) * 3)) & 7 : (irqPrio_[1] >> ((11 - lv) * 3)) & 7;
      if (p >= last) { level = 8 + p; last = p; }
    }
  cpu.irqLevel = int8_t(level);
}

uint16_t Pcfx::irqRead16(uint32_t a) {
  switch (a & 0xC0) {
    case 0x00: return irqAsserted_;
    case 0x40: return irqMask_;
    case 0x80: return irqPrio_[0];
    default: return irqPrio_[1];
  }
}

void Pcfx::irqWrite16(uint32_t a, uint16_t v) {
  switch (a & 0xC0) {
    case 0x40: irqMask_ = v & 0x7F; break;
    case 0x80: if (irqMask_ == 0x7F) irqPrio_[0] = v & 0xFFF; break;  // prioridades só mudam com tudo mascarado
    case 0xC0: if (irqMask_ == 0x7F) irqPrio_[1] = v & 0x1FF; break;
    default: break;
  }
  irqUpdate();
}

// ------------------------------------------------------------------ temporizador (passo de 15 ciclos)
void Pcfx::timerUpdate(int64_t t) {
  if (tmrCtrl_ & 2) {
    const int64_t eff = int64_t(tmrPeriod_ ? tmrPeriod_ : 0x10000) * 15;
    int64_t c = tmrCounter_ - (t - tmrLast_);
    if (c <= 0) {
      c += ((-c) / eff + 1) * eff;  // sem laço: vários estouros de uma vez
      if (tmrCtrl_ & 1) { tmrCtrl_ |= 4; irqAssert(SRC_TIMER, true); }
    }
    tmrCounter_ = int32_t(c);
  }
  tmrLast_ = t;
}

int64_t Pcfx::timerNext() const { return (tmrCtrl_ & 2) ? tmrLast_ + tmrCounter_ : NEVER; }

// ------------------------------------------------------------------ controle
void Pcfx::padUpdate(int64_t t) {
  for (int i = 0; i < 2; i++)
    if (padLatchAt_[i] <= t) {
      padLatchAt_[i] = NEVER;
      if (mouseOn_[i]) {
        // mouse (identificação 0xD): botões nos bits 16-17 e o movimento desde a última leitura, X nos bits 8-15 e
        // Y nos 0-7, cada um de -127 a 127 (o que passar fica para a próxima)
        const int32_t rx = std::clamp(mouseX_[i], -127, 127), ry = std::clamp(mouseY_[i], -127, 127);
        mouseX_[i] -= rx;
        mouseY_[i] -= ry;
        padData_[i] = (0xDu << 28) | uint32_t(mouseBtn_[i] & 3) << 16 | uint32_t(rx & 0xFF) << 8 | uint32_t(ry & 0xFF);
      } else {
        padData_[i] = pad_[i] | (0xFu << 28);  // 0xF = controle comum
      }
      padLatched_[i] = true;
      padCtrl_[i] &= ~1;
      irqAssert(SRC_INPUT, true);
    }
}

uint16_t Pcfx::padRead16(uint32_t a) {
  padUpdate(ts);
  uint16_t r;
  a &= 0xC2;
  if (a == 0x00 || a == 0x80) {
    r = padLatched_[(a & 0x80) >> 7] ? 8 : 0;
  } else {
    const int w = (a >> 7) & 1;
    r = uint16_t(padData_[w] >> ((a & 2) ? 16 : 0));
    if (!(a & 2)) padLatched_[w] = false;
  }
  if (!padLatched_[0] && !padLatched_[1]) irqAssert(SRC_INPUT, false);
  return r;
}

void Pcfx::padWrite16(uint32_t a, uint16_t v) {
  padUpdate(ts);
  if ((a & 0xC0) == 0x00 || (a & 0xC0) == 0x80) {
    const int w = (a & 0x80) >> 7;
    if ((v & 1) && !(padCtrl_[w] & 1)) padLatchAt_[w] = ts + 1536;
    padCtrl_[w] = v & 7;
  }
}

void Pcfx::setMouse(int port, int dx, int dy, uint8_t buttons) {
  port &= 1;
  if (!mouseOn_[port] && !dx && !dy && !buttons) return;  // o mouse só entra na porta quando é usado
  mouseOn_[port] = true;
  mouseX_[port] = std::clamp(mouseX_[port] + dx, -32768, 32767);
  mouseY_[port] = std::clamp(mouseY_[port] + dy, -32768, 32767);
  mouseBtn_[port] = buttons & 3;
}

// ------------------------------------------------------------------ VDCs
void Pcfx::runVdcs(int64_t t) {
  vdc[0].runTo(t);
  vdc[1].runTo(t);
  for (int i = 0; i < 2; i++)
    if (vdc[i].irq() != vdcIrq_[i]) { vdcIrq_[i] = vdc[i].irq(); irqAssert(i ? SRC_VDCB : SRC_VDCA, vdcIrq_[i]); }
}

void Pcfx::stall() {
  ts += 3;
  runVdcs(ts);
}

uint16_t Pcfx::vdcRead16(int chip, uint32_t a) {
  runVdcs(ts);
  uint16_t r;
  if (!(a & 4)) r = vdc[chip].read(0);
  else { r = vdc[chip].read(2); r = uint16_t(r | vdc[chip].read(3) << 8); }
  runVdcs(ts);
  return r;
}

void Pcfx::vdcWrite16(int chip, uint32_t a, uint16_t v) {
  runVdcs(ts);
  if (!(a & 4)) {
    lastVdcAr_[chip] = v;
    vdc[chip].write(0, uint8_t(v));
  } else {
    vdc[chip].write(2, uint8_t(v));
    vdc[chip].write(3, uint8_t(v >> 8));
  }
  runVdcs(ts);
}

// ------------------------------------------------------------------ linhas do HuC6261
void Pcfx::runLine(int64_t t) {
  while (nextLineEvt_ <= t) {
    if (lineEvt_ == 0) {  // 48 ciclos depois do hsync: o contador de linha anda
      vce.raster = uint16_t((vce.raster + 1) % lines_);
      if (vce.raster == 0) {
        lines_ = (vce.pictureMode & 1) ? 262 : 263;
        vdcVce.lines = lines_;
        const bool inter = vce.pictureMode & 2;
        vce.oddField = inter ? !vce.oddField : false;
      }
      king.rasterLine(vce.raster);
      irqAssert(SRC_KING, king.irq());
      lineEvt_ = 1;
      nextLineEvt_ = lineBase_ + 341;
    } else if (lineEvt_ == 1) {  // começo da imagem
      vce.inHblank = false;
      activeStart();
      lineEvt_ = 2;
      nextLineEvt_ = lineBase_ + LINE;
    } else {  // hsync: começa o apagamento horizontal da próxima linha
      vce.inHblank = true;
      lineBase_ += LINE;
      lineEvt_ = 0;
      nextLineEvt_ = lineBase_ + 48;
    }
  }
}

// guarda o que o HuC6261 usa na linha e desenha os fundos do KING
void Pcfx::activeStart() {
  vdcVce.div = (vce.pictureMode & 8) ? 3 : 4;
  const int r = vce.raster;
  if (r >= 262) return;
  // o RAINBOW anda em todas as linhas (também no apagamento vertical): troca de faixa e descompressão
  rbType_ = int8_t(king.rainbowLine(r, rbLine_));
  if (r < 22) return;
  if (rbType_ == 1 && !(vce.pictureMode & 8)) chromaKey();
  lc_.pictureMode = vce.pictureMode;
  lc_.priority[0] = vce.priority[0]; lc_.priority[1] = vce.priority[1];
  std::memcpy(lc_.palOffset, vce.palOffset, sizeof lc_.palOffset);
  lc_.ble = vce.ble; lc_.spbl = vce.spbl; lc_.ccr = vce.ccr;
  std::memcpy(lc_.coef, vce.coef, sizeof lc_.coef);
  // prioridade de cada camada (1..8, 0 = desligada), depois renumerada em ordem 1..7 sem repetição
  uint8_t p[8] = {};
  for (int n = 0; n < 4; n++)
    if ((vce.pictureMode >> (10 + n)) & 1) { const int v = ((vce.priority[1] >> (n * 4)) & 0xF) + 1; p[L_BG0 + n] = uint8_t(v > 8 ? 0 : v); }
  if (vce.pictureMode & 0x0100) { const int v = (vce.priority[0] & 0xF) + 1; p[L_VDC_BG] = uint8_t(v > 8 ? 0 : v); }
  if (vce.pictureMode & 0x0200) { const int v = ((vce.priority[0] >> 4) & 0xF) + 1; p[L_VDC_SPR] = uint8_t(v > 8 ? 0 : v); }
  if (vce.pictureMode & 0x4000) { const int v = ((vce.priority[0] >> 8) & 0xF) + 1; p[L_RAINBOW] = uint8_t(v > 8 ? 0 : v); }
  int remap = 1;
  bool done[8] = {};
  for (int i = 1; i <= 8; i++)
    for (int l = int(L_BG0); l <= int(L_RAINBOW); l++)
      if (p[l] == i && !done[l]) { p[l] = uint8_t(remap++); done[l] = true; }
  std::memcpy(lc_.layerPrio, p, sizeof p);
  king.drawBgLine(r, bgLine_);
}

void Pcfx::lineDone(int idx, int s) {
  std::memcpy(vdcRow_[idx], vdc[idx].row, sizeof vdcRow_[idx]);
  rowDone_[idx] = int16_t(s);
  if (rowDone_[0] == rowDone_[1]) {
    const int r = (s + 3) % lines_;
    if (r >= 22 && r < 262) mixLine(r);
    rowDone_[0] = rowDone_[1] = -1;
  }
}

void Pcfx::frameDone(int idx) {
  if (idx != 0) return;
  buildFrame();
  frames++;
  frameReady_ = true;
}

// chroma key do HuC6261 sobre o plano YUV do RAINBOW (só a 5,37 MHz): pontos dentro das faixas Y/U/V (mínimo no
// byte de baixo, máximo no de cima) ficam transparentes; com as três chaves em zero, só o preto absoluto (0,0,0)
void Pcfx::chromaKey() {
  const uint32_t ky = vce.chromaY, ku = vce.chromaU, kv = vce.chromaV;
  const uint32_t ymin = ky & 0xFF, ymax = ky >> 8, umin = ku & 0xFF, umax = ku >> 8, vmin = kv & 0xFF, vmax = kv >> 8;
  if (!(ky | ku | kv)) {
    for (auto& p : rbLine_) if (!(p & 0xFFFFFF)) p = 0;
  } else if (ymin <= ymax && umin <= umax && vmin <= vmax) {
    for (auto& p : rbLine_) {
      const uint32_t y = (p >> 16) & 0xFF, u = (p >> 8) & 0xFF, v = p & 0xFF;
      if (y >= ymin && y <= ymax && u >= umin && u <= umax && v >= vmin && v <= vmax) p = 0;
    }
  }
}

// mistura uma linha: VDC (os dois chips), fundos do KING e RAINBOW, com prioridades e celofane
void Pcfx::mixLine(int raster) {
  const LineCache& c = lc_;
  const bool hi = c.pictureMode & 8;
  const int vw = hi ? 342 : 256;
  // primeiro ponto do VDC que cai no começo da imagem do HuC6261, contado do começo da linha do VDC. Pelo tempo do
  // HSYNC (fim aos 213 ciclos a 5,37 MHz e aos 203 a 7,16 MHz; imagem aos 341), o VDC aparece em
  // (HDS+1)*8 - 24 pontos a 5,37 MHz e em (HDS+1)*8 - 38 a 7,16 MHz (como no Mednafen)
  const int x0 = hi ? 70 : 48;
  uint32_t vdcRaw[342], vdcYuv[342];
  const uint32_t poff[2] = {uint32_t(c.palOffset[0] & 0xFF) << 1, uint32_t((c.palOffset[0] >> 8) & 0xFF) << 1};
  const bool anyVdc = c.layerPrio[L_VDC_BG] || c.layerPrio[L_VDC_SPR];
  for (int x = 0; x < vw; x++) {
    auto conv = [](uint16_t v) -> uint32_t {
      if (v & pce::Vdc::SPR_FLAG) return 0x100u | (v & 0xFF);
      if (v & pce::Vdc::TRANSP_FLAG) return 0;
      return v & 0xFF;
    };
    const uint32_t z0 = conv(vdcRow_[0][x + x0]), z1 = conv(vdcRow_[1][x + x0]);
    uint32_t t;
    if ((c.pictureMode & 0x80) && (z1 & 0x18F) > 0x180) t = (z1 & 0xF) | ((z0 & 0xF) << 4) | 0x100;  // combinação de sprites
    else if ((c.pictureMode & 0x40) && ((z1 ^ 0x100) & 0x18F) > 0x180) t = (z1 & 0xF) | ((z0 & 0xF) << 4);  // de fundos
    else t = (z1 & 0xF) ? z1 : z0;
    vdcRaw[x] = t;
    vdcYuv[x] = (anyVdc && (t & 0xF)) ? vce.yuv[((t & 0xFF) + poff[(t >> 8) & 1]) & 0x1FF] | (uint32_t((t & 0x100) ? L_VDC_SPR : L_VDC_BG) << 28) : 0;
  }
  // celofane: coeficientes de cada camada (0 = não mistura)
  uint8_t ble[8] = {};
  bool bleAny = false;
  for (int n = 0; n < 4; n++) ble[L_BG0 + n] = (c.ble >> (4 + n * 2)) & 3;
  ble[L_VDC_BG] = c.ble & 3;
  ble[L_VDC_SPR] = (c.ble >> 2) & 3;
  ble[L_RAINBOW] = (c.ble >> 12) & 3;
  for (int i = 1; i < 8; i++) bleAny |= ble[i] != 0;
  auto my = [](int coef, int v) { return int(uint8_t(v * coef / 8)); };
  auto muv = [](int coef, int v) { return int(int8_t((v - 128) * coef / 8)); };
  auto cello = [&](uint32_t back, uint32_t fore, int which) {
    const int fc = c.coef[which * 2], bc = c.coef[which * 2 + 1];
    const int y = my((bc >> 8) & 15, (back >> 16) & 0xFF) + my((fc >> 8) & 15, (fore >> 16) & 0xFF);
    const int u = muv((bc >> 4) & 15, (back >> 8) & 0xFF) + muv((fc >> 4) & 15, (fore >> 8) & 0xFF) + 128;
    const int v = muv(bc & 15, back & 0xFF) + muv(fc & 15, fore & 0xFF) + 128;
    return (fore & 0xF0000000u) | uint32_t(clamp8(y)) << 16 | uint32_t(clamp8(u)) << 8 | clamp8(v);
  };
  uint32_t bpc = (c.pictureMode & 0x7F00) ? vce.yuv[0] : 0x008080;  // cor de trás de tudo
  const int mode = hi ? 0 : (c.ble & 0xC000) == 0xC000 ? 1 : (c.ble & 0xC000) == 0x4000 ? 2 : bleAny ? 3 : 0;
  if (mode == 1) bpc = 0x008080;
  if (mode == 2) bpc = uint32_t(c.ccr & 0xFF00) << 8 | uint32_t(c.ccr & 0xF0) << 8 | uint32_t(c.ccr & 0x0F) << 4;
  const int w = hi ? 341 : 256;
  const auto& pm = prioMap();
  const bool rbOn = rbType_ >= 0;  // sem faixa do RAINBOW nesta linha, a camada fica desligada
  uint32_t* out = lineOut_[raster - 22];
  lineW_[raster - 22] = uint16_t(w);
  for (int x = 0; x < w; x++) {
    const int xb = hi ? x * 256 / 341 : x;
    const uint32_t lay[3] = {vdcYuv[x], bgLine_[8 + xb], rbLine_[xb]};
    const uint8_t pr[3] = {c.layerPrio[lay[0] >> 28], c.layerPrio[lay[1] >> 28], rbOn ? c.layerPrio[lay[2] >> 28] : uint8_t(0)};
    uint32_t px[4] = {0, 0, 0, 0};
    for (int k = 0; k < 3; k++) px[pm.m[pr[0]][pr[1]][pr[2]][k]] = lay[k];
    uint32_t z = bpc;
    for (int k = 0; k < 3; k++) {
      const uint32_t p = px[k];
      if (!p) continue;
      const int b = ble[p >> 28];
      const bool sprOk = (p >> 28) != L_VDC_SPR || ((c.spbl >> ((vdcRaw[x] & 0xF0) >> 4)) & 1);
      if (mode == 2 && b) z = sprOk ? cello(z, p, b - 1) : p;
      else if ((mode == 1 || mode == 3) && b && k > 0 && (z & 0xF0000000u)) z = sprOk ? cello(z, p, b - 1) : p;
      else z = p;
    }
    if (mode == 1) {  // celofane da frente: mistura com a cor fixa
      const int fc = c.coef[0], bc = c.coef[1];
      const int y = my((fc >> 8) & 15, (c.ccr >> 8) & 0xFF) + my((bc >> 8) & 15, (z >> 16) & 0xFF);
      const int u = muv((fc >> 4) & 15, c.ccr & 0xF0) + muv((bc >> 4) & 15, (z >> 8) & 0xFF) + 128;
      const int v = muv(fc & 15, (c.ccr << 4) & 0xF0) + muv(bc & 15, z & 0xFF) + 128;
      z = (z & 0xF0000000u) | uint32_t(clamp8(y)) << 16 | uint32_t(clamp8(u)) << 8 | clamp8(v);
    }
    out[x] = toRgb(z);
  }
}

void Pcfx::buildFrame() {
  int w = 256;
  for (int y = 0; y < H; y++) if (lineW_[y] > w) w = lineW_[y];
  fbWidth = w;
  for (int y = 0; y < H; y++) {
    const int lw = lineW_[y] ? lineW_[y] : 256;
    uint32_t* o = fb + y * w;
    if (lw == w) std::memcpy(o, lineOut_[y], size_t(w) * 4);
    else for (int x = 0; x < w; x++) o[x] = lineOut_[y][x * lw / w];
  }
}

// ------------------------------------------------------------------ barramento
uint8_t Pcfx::read8(uint32_t a) {
  if (a <= 0x1FFFFF) { ramTiming(a); return ram[a]; }
  if (a <= 0xFFFFFF) { ramTiming(a); return 0xFF; }
  if (a >= 0xF0000000) { ts += 2; return bios_[a & 0xFFFFF]; }
  if (a >= 0xE0000000 && a <= 0xE7FFFFFF) return (a & 1) ? 0xFF : bram[(a & 0xFFFF) >> 1];
  if (a >= 0xE8000000 && a <= 0xE9FFFFFF) return bram[0x8000 + ((a & 0xFFFF) >> 1)];
  if (a >= 0x80000000 && a <= 0x807FFFFF) return portRead8(a & 0x7FFFFF);
  return 0xFF;
}

uint16_t Pcfx::read16(uint32_t a) {
  if (a <= 0x1FFFFF) { ramTiming(a); return uint16_t(ram[a] | ram[a + 1] << 8); }
  if (a <= 0xFFFFFF) { ramTiming(a); return 0xFFFF; }
  if (a >= 0xF0000000) { ts += 2; return uint16_t(bios_[a & 0xFFFFF] | bios_[(a & 0xFFFFF) + 1] << 8); }
  // áreas de "cadeia de bits": leem os dados do VCE, dos VDCs e do KING
  if (a >= 0xA0000000 && a <= 0xA3FFFFFF) { ts += 4; runLine(ts); return vce.read16(4); }
  if (a >= 0xA4000000 && a <= 0xA7FFFFFF) { ts += 4; return vdcRead16(0, 4); }
  if (a >= 0xA8000000 && a <= 0xABFFFFFF) { ts += 4; return vdcRead16(1, 4); }
  if (a >= 0xAC000000 && a <= 0xAFFFFFFF) { ts += 4; const uint16_t r = king.read16(ts, 0x604); irqAssert(SRC_KING, king.irq()); return r; }
  if (a >= 0xB0000000 && a <= 0xBFFFFFFF) return 0;
  if (a >= 0xE0000000 && a <= 0xE7FFFFFF) return bram[(a & 0xFFFF) >> 1];
  if (a >= 0xE8000000 && a <= 0xE9FFFFFF) return bram[0x8000 + ((a & 0xFFFF) >> 1)];
  if (a >= 0x80000000 && a <= 0x807FFFFF) return portRead16(a & 0x7FFFFF);
  return 0xFFFF;
}

uint32_t Pcfx::read32(uint32_t a) {
  if (a <= 0x1FFFFF) { ramTiming(a); return uint32_t(ram[a]) | uint32_t(ram[a + 1]) << 8 | uint32_t(ram[a + 2]) << 16 | uint32_t(ram[a + 3]) << 24; }
  if (a <= 0xFFFFFF) { ramTiming(a); return 0xFFFFFFFF; }
  if (a >= 0xB0000000 && a <= 0xBFFFFFFF) return 0;
  return read16(a) | uint32_t(read16(a | 2)) << 16;
}

void Pcfx::write8(uint32_t a, uint8_t v) {
  if (a <= 0x1FFFFF) { ramTiming(a); ram[a] = v; return; }
  if (a <= 0xFFFFFF) { ramTiming(a); return; }
  if (a >= 0xE0000000 && a <= 0xE7FFFFFF) {
    if (!(a & 1) && (backupCtrl_ & 1)) { uint8_t& m = bram[(a & 0xFFFF) >> 1]; if (m != v) { m = v; bramDirty = true; } }
    return;
  }
  if (a >= 0xE8000000 && a <= 0xE9FFFFFF) {
    if (backupCtrl_ & 2) { uint8_t& m = bram[0x8000 + ((a & 0xFFFF) >> 1)]; if (m != v) { m = v; bramDirty = true; } }
    return;
  }
  if (a >= 0x80000000 && a <= 0x807FFFFF) portWrite8(a & 0x7FFFFF, v);
}

void Pcfx::write16(uint32_t a, uint16_t v) {
  if (a <= 0x1FFFFF) { ramTiming(a); ram[a] = uint8_t(v); ram[a + 1] = uint8_t(v >> 8); return; }
  if (a <= 0xFFFFFF) { ramTiming(a); return; }
  if (a >= 0xE0000000 && a <= 0xE7FFFFFF) {
    if (backupCtrl_ & 1) { uint8_t& m = bram[(a & 0xFFFF) >> 1]; if (m != uint8_t(v)) { m = uint8_t(v); bramDirty = true; } }
    return;
  }
  if (a >= 0xE8000000 && a <= 0xE9FFFFFF) {
    if (backupCtrl_ & 2) { uint8_t& m = bram[0x8000 + ((a & 0xFFFF) >> 1)]; if (m != uint8_t(v)) { m = uint8_t(v); bramDirty = true; } }
    return;
  }
  if (a >= 0xA0000000 && a <= 0xAFFFFFFF) return;
  if (a >= 0xB0000000 && a <= 0xB3FFFFFF) { ts += 2; runLine(ts); vce.write16(4, v); return; }
  if (a >= 0xB4000000 && a <= 0xB7FFFFFF) { ts += 2; vdcWrite16(0, 4, v); return; }
  if (a >= 0xB8000000 && a <= 0xBBFFFFFF) { ts += 2; vdcWrite16(1, 4, v); return; }
  if (a >= 0xBC000000 && a <= 0xBFFFFFFF) { ts += 2; king.write16(ts, 0x604, v); irqAssert(SRC_KING, king.irq()); return; }
  if (a >= 0x80000000 && a <= 0x807FFFFF) portWrite16(a & 0x7FFFFF, v);
}

void Pcfx::write32(uint32_t a, uint32_t v) {
  if (a <= 0x1FFFFF) {
    ramTiming(a);
    ram[a] = uint8_t(v); ram[a + 1] = uint8_t(v >> 8); ram[a + 2] = uint8_t(v >> 16); ram[a + 3] = uint8_t(v >> 24);
    return;
  }
  if (a <= 0xFFFFFF) { ramTiming(a); return; }
  if (a >= 0xA0000000 && a <= 0xAFFFFFFF) return;
  write16(a, uint16_t(v));
  write16(a | 2, uint16_t(v >> 16));
}

// espaço de E/S (IN/OUT, e também em 0x80000000 da memória)
uint8_t Pcfx::portRead8(uint32_t a) {
  if (a <= 0x0FF) return uint8_t(padRead16(a & ~1u) >> ((a & 1) * 8));
  if (a <= 0x2FF) { ts += 4; return 0; }
  if (a <= 0x3FF) { ts += 4; runLine(ts); return uint8_t(vce.read16(a)); }
  if (a <= 0x5FF) { ts += 4; return uint8_t(vdcRead16((a >> 8) & 1, a)); }
  if (a <= 0x6FF) {
    ts += 4;
    const uint16_t r = king.read16(ts, a & ~1u);
    irqAssert(SRC_KING, king.irq());
    return uint8_t(r >> ((a & 1) * 8));
  }
  if (a <= 0x7FF) return (a & 1) ? 0 : exBusReset_;
  if (a >= 0xC00 && a <= 0xCFF) {
    switch (a & 0xC0) {
      case 0x80: return uint8_t(backupCtrl_);
      case 0x00: return uint8_t(lastVdcAr_[0]);
      case 0x40: return uint8_t(lastVdcAr_[1]);
      default: return 0;
    }
  }
  if (a >= 0xE00 && a <= 0xEFF) return uint8_t(irqRead16(a & ~1u) >> ((a & 1) * 8));
  if (a >= 0xF00 && a <= 0xFFF) {
    timerUpdate(ts);
    uint16_t r = 0;
    switch (a & 0xFC0) {
      case 0xF00: r = tmrCtrl_; break;
      case 0xF80: r = tmrPeriod_; break;
      case 0xFC0: r = uint16_t((tmrCounter_ + 14) / 15); break;
      default: break;
    }
    return uint8_t(r >> ((a & 1) * 8));
  }
  return 0;
}

uint16_t Pcfx::portRead16(uint32_t a) {
  if (a <= 0x0FF) return padRead16(a);
  if (a <= 0x2FF) { ts += 4; return 0; }
  if (a <= 0x3FF) { ts += 4; runLine(ts); return vce.read16(a); }
  if (a <= 0x5FF) { ts += 4; return vdcRead16((a >> 8) & 1, a); }
  if (a <= 0x6FF) { ts += 4; const uint16_t r = king.read16(ts, a); irqAssert(SRC_KING, king.irq()); return r; }
  if (a <= 0x7FF) return exBusReset_;
  if (a >= 0xC00 && a <= 0xCFF) {
    switch (a & 0xC0) {
      case 0x80: return backupCtrl_;
      case 0x00: return lastVdcAr_[0];
      case 0x40: return lastVdcAr_[1];
      default: return 0;
    }
  }
  if (a >= 0xE00 && a <= 0xEFF) return irqRead16(a);
  if (a >= 0xF00 && a <= 0xFFF) {
    timerUpdate(ts);
    switch (a & 0xFC0) {
      case 0xF00: return tmrCtrl_;
      case 0xF80: return tmrPeriod_;
      case 0xFC0: return uint16_t((tmrCounter_ + 14) / 15);
      default: return 0;
    }
  }
  return 0;
}

void Pcfx::portWrite8(uint32_t a, uint8_t v) {
  if (a <= 0x0FF) { padWrite16(a, v); return; }
  if (a <= 0x1FF) { ts += 2; sound.write(ts, a, v); irqAssert(SRC_KING, king.irq()); return; }
  if (a <= 0x2FF) { ts += 2; king.rainbow.write8(a, v); return; }  // RAINBOW (HuC6271)
  if (a <= 0x3FF) { ts += 2; runLine(ts); vce.write16(a, v); return; }
  if (a <= 0x5FF) { ts += 2; vdcWrite16((a >> 8) & 1, a, v); return; }
  if (a <= 0x6FF) {
    ts += 2;
    king.write16(ts, a & 0x706, uint16_t(v << ((a & 1) ? 8 : 0)));
    irqAssert(SRC_KING, king.irq());
    return;
  }
  if (a <= 0x7FF) { if (!(a & 1)) exBusReset_ = v & 1; return; }
  if (a >= 0xC00 && a <= 0xCFF) { if ((a & 0xC1) == 0x80) backupCtrl_ = v & 3; return; }
  if (a >= 0xE00 && a <= 0xEFF) irqWrite16(a, v);
}

void Pcfx::portWrite16(uint32_t a, uint16_t v) {
  if (a <= 0x0FF) { padWrite16(a, v); return; }
  if (a <= 0x1FF) { ts += 2; sound.write(ts, a, v); irqAssert(SRC_KING, king.irq()); return; }
  if (a <= 0x2FF) { ts += 2; king.rainbow.write16(a, v); return; }
  if (a <= 0x3FF) { ts += 2; runLine(ts); vce.write16(a, v); return; }
  if (a <= 0x5FF) { ts += 2; vdcWrite16((a >> 8) & 1, a, v); return; }
  if (a <= 0x6FF) { ts += 2; king.write16(ts, a, v); irqAssert(SRC_KING, king.irq()); return; }
  if (a <= 0x7FF) { exBusReset_ = v & 1; return; }
  if (a >= 0xC00 && a <= 0xCFF) { if ((a & 0xC0) == 0x80) backupCtrl_ = v & 3; return; }
  if (a >= 0xE00 && a <= 0xEFF) { irqWrite16(a, v); return; }
  if (a >= 0xF00 && a <= 0xFFF) {
    timerUpdate(ts);
    switch (a & 0xFC0) {
      case 0xF00:
        if (!(tmrCtrl_ & 2) && (v & 2)) tmrCounter_ = int32_t(tmrPeriod_ ? tmrPeriod_ : 0x10000) * 15;
        tmrCtrl_ = v & 7;
        irqAssert(SRC_TIMER, tmrCtrl_ & 4);
        break;
      case 0xF80: tmrPeriod_ = v; break;
      default: break;
    }
  }
}

// ------------------------------------------------------------------ laço principal
void Pcfx::syncAll(int64_t t) {
  runVdcs(t);
  runLine(t);
  king.update(t);
  timerUpdate(t);
  padUpdate(t);
  if (king.adpcmControl() & 3) sound.run(t);
  irqAssert(SRC_KING, king.irq());
}

int64_t Pcfx::nextEvent() const {
  int64_t n = std::min(vdc[0].nextTime(), vdc[1].nextTime());
  n = std::min(n, nextLineEvt_);
  n = std::min(n, king.next());
  n = std::min(n, timerNext());
  n = std::min(n, std::min(padLatchAt_[0], padLatchAt_[1]));
  if (king.adpcmControl() & 3) n = std::min(n, sound.nextTick());
  return n;
}

void Pcfx::runFrame() {
  frameReady_ = false;
  // limite de segurança: um quadro e meio
  const int64_t limit = ts + int64_t(LINE) * 263 * 3 / 2;
  while (!frameReady_ && ts < limit) {
    runUntil = std::max(ts + 1, std::min(nextEvent(), ts + 2048));
    cpu.run();
    syncAll(ts);
  }
  sound.run(ts);
}

// ------------------------------------------------------------------ estado
void Pcfx::sync(StateIO& io) {
  io.sync(ts);
  cpu.sync(io);
  io.bytes(ram);
  io.bytes(bram);
  for (int i = 0; i < 2; i++) vdc[i].sync(io);
  io.sync(vdcVce.div); io.sync(vdcVce.lines);
  vce.sync(io);
  king.sync(io);
  sound.sync(io);
  io.sync(irqAsserted_); io.sync(irqMask_); io.raw(irqPrio_, sizeof irqPrio_);
  io.sync(tmrCtrl_); io.sync(tmrPeriod_); io.sync(tmrCounter_); io.sync(tmrLast_);
  io.raw(padCtrl_, sizeof padCtrl_); io.sync(padLatched_[0]); io.sync(padLatched_[1]);
  io.raw(padLatchAt_, sizeof padLatchAt_); io.raw(padData_, sizeof padData_);
  io.sync(mouseOn_[0]); io.sync(mouseOn_[1]); io.raw(mouseX_, sizeof mouseX_); io.raw(mouseY_, sizeof mouseY_);
  io.raw(mouseBtn_, sizeof mouseBtn_);
  io.sync(backupCtrl_); io.raw(lastVdcAr_, sizeof lastVdcAr_); io.sync(exBusReset_); io.sync(ramPage_);
  io.sync(lineBase_); io.sync(nextLineEvt_); io.sync(lineEvt_); io.sync(lines_);
  io.sync(vdcIrq_[0]); io.sync(vdcIrq_[1]);
  io.raw(&lc_, sizeof lc_);
  io.raw(bgLine_, sizeof bgLine_);
  io.raw(rbLine_, sizeof rbLine_); io.sync(rbType_);
  io.raw(vdcRow_, sizeof vdcRow_);
  io.raw(rowDone_, sizeof rowDone_);
  io.sync(frames);
  if (io.reading()) {
    // instantes fora do lugar fariam os laços de tempo girarem para sempre: o estado é recusado
    bool ok = ts >= 0 && ts <= (int64_t(1) << 52) && tempoOk(tmrLast_, ts) && tempoOk(lineBase_, ts) &&
              nextLineEvt_ >= lineBase_ && nextLineEvt_ <= lineBase_ + LINE &&
              tempoOk(padLatchAt_[0], ts, true) && tempoOk(padLatchAt_[1], ts, true) &&
              king.temposOk(ts) && sound.temposOk(ts);
    if (!ok) throw std::runtime_error("Estado salvo inválido (relógios fora do lugar).");
    if (tmrCounter_ < 0 || tmrCounter_ > 0x10000 * 15) tmrCounter_ = 0x10000 * 15;
    if (vdcVce.div < 2 || vdcVce.div > 4) vdcVce.div = 4;
    if (vdcVce.lines != 262 && vdcVce.lines != 263) vdcVce.lines = 263;
    if (lines_ != 262 && lines_ != 263) lines_ = 263;
    if (vce.raster >= lines_) vce.raster = 0;
    if (lineEvt_ > 2) lineEvt_ = 0;
    if (rbType_ < -1 || rbType_ > 1) rbType_ = -1;
    for (int i = 0; i < 2; i++) {
      mouseX_[i] = std::clamp(mouseX_[i], -32768, 32767);
      mouseY_[i] = std::clamp(mouseY_[i], -32768, 32767);
      mouseBtn_[i] &= 3;
    }
    for (auto& p : lc_.layerPrio) p &= 7;
    for (int i = 0; i < 2; i++) { if (rowDone_[i] < -1 || rowDone_[i] > 300) rowDone_[i] = -1; vdc[i].fixTime(ts); }
    irqUpdate();
  }
}

std::vector<uint8_t> Pcfx::saveState() {
  StateIO io;
  uint32_t magic = 0x58464350;  // "PCFX"
  io.sync(magic);
  sync(io);
  return io.data();
}

void Pcfx::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  uint32_t magic = 0;
  io.sync(magic);
  if (magic != 0x58464350) throw std::runtime_error("Este estado salvo não é de PC-FX.");
  // carrega numa cópia: um estado ruim não estraga o jogo em andamento
  const std::vector<uint8_t> backup = saveState();
  try {
    sync(io);
  } catch (...) {
    StateIO b(backup);
    uint32_t m = 0;
    b.sync(m);
    sync(b);
    throw;
  }
}

// ------------------------------------------------------------------ núcleo
PcfxCore::PcfxCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios)
    : disc_(std::move(disc)) {
  const std::vector<uint8_t> b = bios ? *bios : firmware("pcfx");
  if (b.empty())
    throw std::runtime_error("o PC-FX precisa da BIOS do console. Abra o arquivo da BIOS do seu PC-FX (pcfx.rom, 1 MB) uma vez no Índigo.");
  sys_ = std::make_unique<Pcfx>(b, disc_.get());
  if (save) importSave(*save);
  title_ = disc_ ? disc_->name() : "PC-FX";
  // título gravado no setor 1 do disco (só se for texto simples)
  if (disc_) {
    for (const auto& t : disc_->tracks()) {
      if (t.type == cdrom::TrackType::Audio) continue;
      uint8_t s[cdrom::DATA_SECTOR];
      if (disc_->readData(t.start + 1, s)) {
        std::string n;
        for (int i = 0; i < 32 && s[i]; i++) n += (s[i] >= 32 && s[i] < 127) ? char(s[i]) : '?';
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (!n.empty() && n.find('?') == std::string::npos) title_ = n;
      }
      break;
    }
  }
  const bool testBios = std::search(b.begin(), b.end(), "Indigo PC-FX", "Indigo PC-FX" + 12) != b.end();
  note_ = testBios ? " · BIOS de teste do Índigo" : " · PC-FX experimental";
}

void PcfxCore::setKeys(uint32_t k) {
  uint16_t p = 0;
  if (k & KEY_A) p |= 1 << 0;       // I
  if (k & KEY_B) p |= 1 << 1;       // II
  if (k & KEY_X) p |= 1 << 2;       // III
  if (k & KEY_Y) p |= 1 << 3;       // IV
  if (k & KEY_L) p |= 1 << 4;       // V
  if (k & KEY_R) p |= 1 << 5;       // VI
  if (k & KEY_SELECT) p |= 1 << 6;
  if (k & KEY_START) p |= 1 << 7;   // Run
  if (k & KEY_UP) p |= 1 << 8;
  if (k & KEY_RIGHT) p |= 1 << 9;
  if (k & KEY_DOWN) p |= 1 << 10;
  if (k & KEY_LEFT) p |= 1 << 11;
  sys_->setPad(0, p);
}

// mouse do PC: vai para a porta 2 (a 1 continua com o controle do teclado/joystick)
void PcfxCore::setMouse(int dx, int dy, uint32_t buttons) { sys_->setMouse(1, dx, dy, uint8_t(buttons & 3)); }

void PcfxCore::importSave(const std::vector<uint8_t>& b) {
  if (b.size() == 0x10000 || b.size() == 0x8000) {
    std::copy(b.begin(), b.end(), sys_->bram.begin());
    sys_->bramDirty = false;
  }
}

}  // namespace indigo::pcfx
