#include "mars.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::md32x {

namespace {
const char STATE_MAGIC[4] = {'I', '3', '2', 'X'};
constexpr int64_t SLICE = 96;
constexpr float PWM_GAIN = 0.86f;          // PWM com o pulso máximo = 0,86 da escala (volume relativo ao FM do PicoDrive)              // fatia dos SH-2 (ciclos): latência entre os dois processadores
enum { P_PWM = 0, P_CMD = 1, P_HINT = 2, P_VINT = 3, P_VRES = 4 };

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// cor do 32X (BGR555) para 0xAABBGGRR
const uint32_t* colorLut() {
  static uint32_t lut[32768];
  static bool ready = false;
  if (!ready) {
    for (int c = 0; c < 32768; c++) {
      const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
      lut[c] = 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
    }
    ready = true;
  }
  return lut;
}

// BIOS própria do SH-2 (HLE): vetores para a armadilha em 200 e o programa de partida em 204
void makeSh2Bios(uint8_t* b, size_t size, bool slave) {
  std::memset(b, 0, size);
  for (int v = 0; v < 64; v++) put32(b + v * 4, 0x200);
  put32(b + 0, 0x204); put32(b + 8, 0x204);
  const uint32_t sp = slave ? 0x0603F800 : 0x06040000;
  put32(b + 4, sp); put32(b + 12, sp);
  static const uint16_t trap[] = {0xAFFE, 0x0009};  // bra $ / nop
  static const uint16_t master[] = {
      0xD104,  // 204 mov.l @(espera,pc),r1
      0x4110,  // 206 dt r1
      0x8BFD,  // 208 bf 206
      0xD004,  // 20A mov.l @("M_OK",pc),r0
      0xC208,  // 20C mov.l r0,@(20,gbr)        COMM0 = "M_OK"
      0xD204,  // 20E mov.l @(entrada,pc),r2
      0x6222,  // 210 mov.l @r2,r2              entrada do mestre (cabeçalho 3E0)
      0x422B,  // 212 jmp @r2
      0x0009,  // 214 nop
      0x0009,  // 216
  };
  static const uint16_t slaveCode[] = {
      0xD104,  // 204 mov.l @("M_OK",pc),r1
      0xC608,  // 206 mov.l @(20,gbr),r0
      0x3010,  // 208 cmp/eq r1,r0
      0x8BFC,  // 20A bf 206                    espera o mestre
      0xD003,  // 20C mov.l @("S_OK",pc),r0
      0xC209,  // 20E mov.l r0,@(24,gbr)        COMM4 = "S_OK"
      0xD203,  // 210 mov.l @(entrada,pc),r2
      0x6222,  // 212 mov.l @r2,r2              entrada do escravo (cabeçalho 3E4)
      0x422B,  // 214 jmp @r2
      0x0009,  // 216 nop
  };
  put16(b + 0x200, trap[0]); put16(b + 0x202, trap[1]);
  const uint16_t* code = slave ? slaveCode : master;
  for (int i = 0; i < 10; i++) put16(b + 0x204 + i * 2, code[i]);
  if (!slave) {
    put32(b + 0x218, 81920);        // espera: o 68000 termina de limpar a memória (~0,85 quadro)
    put32(b + 0x21C, 0x4D5F4F4B);   // "M_OK"
    put32(b + 0x220, 0x220003E0);
  } else {
    put32(b + 0x218, 0x4D5F4F4B);
    put32(b + 0x21C, 0x535F4F4B);   // "S_OK"
    put32(b + 0x220, 0x220003E4);
  }
}
}  // namespace

bool isMd32xRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (rom.size() < 0x400 || rom.size() > (8u << 20)) return false;
  if (endsWith(n, ".32x")) return true;
  return std::memcmp(&rom[0x3C0], "MARS CHECK MODE", 15) == 0 || std::memcmp(&rom[0x100], "SEGA 32X", 8) == 0;
}

// ------------------------------------------------------------ construção
Mars::Mars(md::MD& m, const std::vector<uint8_t>& rom) : md(m), msh2(*this, 0), ssh2(*this, 1) {
  rom_ = rom;
  if (rom_.size() < 0x400) rom_.resize(0x400, 0xFF);
  uint32_t p = 0x400;
  while (p < rom_.size()) p <<= 1;
  rom_.resize(p, 0xFF);
  romMask_ = p - 1;
  msh2.other = &ssh2;
  ssh2.other = &msh2;
  buildBios();
  md.ext = this;
  md.vdp.idxOut = idx_;
  power();
}

Mars::~Mars() {
  md.ext = nullptr;
  md.vdp.idxOut = nullptr;
}

void Mars::buildBios() {
  const auto& f68 = firmware("32x_m68k");
  const auto& fm = firmware("32x_master");
  const auto& fs = firmware("32x_slave");
  realBios_ = f68.size() >= 256 && fm.size() >= 2048 && fs.size() >= 1024;
  if (realBios_) {
    std::memcpy(bios68_, f68.data(), 256);
    std::memcpy(biosM_, fm.data(), 2048);
    std::memcpy(biosS_, fs.data(), 1024);
    return;
  }
  // 68000: vetores para a tabela de saltos do cartucho (880200 + 6·(n-1)); a HINT (70) começa em zero e é
  // gravável; em C0 e D4 as duas rotinas da BIOS que os jogos chamam (escrever com RV ligado e os bancos)
  std::memset(bios68_, 0, sizeof bios68_);
  std::memcpy(bios68_, rom_.data(), 4);
  for (int i = 1; i < 48; i++) put32(bios68_ + i * 4, 0x880200 + (i - 1) * 6);
  put32(bios68_ + 0x70, 0);
  static const uint16_t rot[] = {
      0x08F9, 0x0000, 0x00A1, 0x5107,          // C0 bset #0,$A15107 (RV)
      0x1280,                                  // C8 move.b d0,(a1)
      0x0239, 0x00FE, 0x00A1, 0x5107,          // CA and.b #$FE,$A15107
      0x4E75,                                  // D2 rts
      0x48E7, 0x8040,                          // D4 movem.l d0/a1,-(sp)
      0x227C, 0x00A1, 0x30F1,                  //    movea.l #$A130F1,a1
      0x7007,                                  //    moveq #7,d0
      0x12D8,                                  //    move.b (a0)+,(a1)+
      0x5289,                                  //    addq.l #1,a1
      0x51C8, 0xFFFA,                          //    dbra d0,*-4
      0x0239, 0x00FE, 0x00A1, 0x5107,          //    and.b #$FE,$A15107
      0x4CDF, 0x0201,                          //    movem.l (sp)+,d0/a1
      0x4E75,                                  //    rts
  };
  for (size_t i = 0; i < sizeof rot / 2; i++) put16(bios68_ + 0xC0 + i * 2, rot[i]);
  makeSh2Bios(biosM_, sizeof biosM_, false);
  makeSh2Bios(biosS_, sizeof biosS_, true);
}

void Mars::power() {
  aden_ = false; nres_ = true; fm_ = rv_ = s68_ = dmaBit_ = hen_ = false;
  intCmd_ = 0; bank_ = 0;
  mask_[0] = mask_[1] = 0; pend_[0] = pend_[1] = 0;
  hcount_ = 0; hcnt_ = 0;
  std::memset(comm_, 0, sizeof comm_);
  dreqSrcH_ = dreqSrcL_ = dreqDstH_ = dreqDstL_ = 0; dreqLen_ = 0;
  std::memset(fifo_, 0, sizeof fifo_); fifoN_ = 0;
  tv_ = 0;
  std::memcpy(hintVec_, bios68_ + 0x70, 4);
  vmode_ = 0; shift_ = 0; fillLen_ = 0; fillAddr_ = fillData_ = 0;
  fs_ = fsPending_ = 0; vblank_ = true; fillEnd_ = 0;
  std::memset(pal_, 0, sizeof pal_);
  std::fill(sdram_.begin(), sdram_.end(), 0);
  for (auto& f : fb_) std::fill(f.begin(), f.end(), 0);
  pwmCtl_ = pwmCycle_ = 0; std::memset(pwmFifo_, 0, sizeof pwmFifo_); pwmN_[0] = pwmN_[1] = 0;
  pwmTimer_ = 16; pwmNext_ = INT64_MAX; pwmPeriod_ = 0; pwmOut_[0] = pwmOut_[1] = 0; pwmLatch_ = 0;
  pwmLog_.clear();
  for (auto& p : poll_) p = Poll{};
  now_ = sh2Time(md.clock());
  lineStart_ = now_;
  audioT0_ = now_; held_[0] = held_[1] = 0;
  outW_ = outR0_ = 0;
  mapPages();
  msh2.clock = ssh2.clock = now_;
  msh2.powerOn(); ssh2.powerOn();
  msh2.clock = ssh2.clock = now_;
  updateIrl();
}

void Mars::mapPages() {
  for (Sh2* c : {&msh2, &ssh2}) {
    for (int i = 0; i < 32; i++) { c->rpage[i] = nullptr; c->wpage[i] = nullptr; c->pmask[i] = 0; c->rwait[i] = 0; }
    c->rpage[2] = rom_.data(); c->pmask[2] = std::min<uint32_t>(romMask_, 0x3FFFFF); c->rwait[2] = 4;
    c->rpage[4] = fb_[fs_ ^ 1].data(); c->pmask[4] = 0x1FFFF; c->rwait[4] = 2;
    c->rpage[6] = sdram_.data(); c->wpage[6] = sdram_.data(); c->pmask[6] = 0x3FFFF; c->rwait[6] = 0;
    c->remapped();
  }
}

void Mars::resetSh2s() {
  for (auto& p : poll_) p = Poll{};
  msh2.clock = ssh2.clock = now_;
  msh2.powerOn(); ssh2.powerOn();
  msh2.clock = ssh2.clock = now_;
  if (!realBios_) {
    // o que a BIOS do mestre faria: código inicial do cartucho na SDRAM, VBR, GBR e a soma em COMM8
    const uint32_t src = be32(&rom_[0x3D4]) & 0x0FFFFFFF, dst = be32(&rom_[0x3D8]) & 0x0FFFFFFF;
    const uint32_t size = std::min<uint32_t>(be32(&rom_[0x3DC]), 0x40000);
    for (uint32_t i = 0; i < size; i++) sdram_[(dst + i) & 0x3FFFF] = rom_[(src + i) & romMask_];
    msh2.gbr = ssh2.gbr = 0x20004000;
    msh2.vbr = be32(&rom_[0x3E8]);
    ssh2.vbr = be32(&rom_[0x3EC]);
    comm_[4] = be16(&rom_[0x18E]);
  }
  updateIrl();
}

// ------------------------------------------------------------ sincronia
void Mars::syncTo(int64_t t) {
  if (t <= now_) return;
  if (!running()) {
    while (pwmNext_ <= t) { now_ = pwmNext_; pwmTick(); }
    now_ = t;
    msh2.clock = std::max(msh2.clock, t);
    ssh2.clock = std::max(ssh2.clock, t);
    return;
  }
  while (now_ < t) {
    if (pwmNext_ <= now_) pwmTick();
    int64_t end = std::min(t, now_ + SLICE);
    // os dois esperando: pula direto até o próximo acontecimento
    if (poll_[0].on && poll_[1].on) end = std::min({t, poll_[0].wake, poll_[1].wake});
    if (pwmNext_ < end) end = pwmNext_;
    if (end <= now_) end = now_ + 1;
    for (int i = 0; i < 2; i++) {
      Sh2& c = i ? ssh2 : msh2;
      Poll& p = poll_[i];
      if (p.on) {
        if (p.wake > end) { if (c.clock < end) c.clock = end; continue; }
        p.on = false;
        p.cnt = 0;
        if (c.clock < p.wake) c.clock = p.wake;
      }
      c.run(end);
    }
    now_ = end;
  }
  if (pwmNext_ <= now_) pwmTick();
}

void Mars::updateIrl() {
  static const int LV[5] = {6, 8, 10, 12, 14};
  for (int i = 0; i < 2; i++) {
    int bits = pend_[i];
    if (((intCmd_ >> i) & 1) && (mask_[i] & 2)) bits |= 1 << P_CMD;
    int lv = 0;
    for (int b = 4; b >= 0; b--) if (bits & (1 << b)) { lv = LV[b]; break; }
    Sh2& c = i ? ssh2 : msh2;
    if (!running()) lv = 0;
    if (c.irl() != lv) { c.setIrl(lv); wake(i); }
  }
}

// laço de espera: o mesmo SH-2 lendo o mesmo registrador, na mesma instrução, com o mesmo valor, várias vezes
// seguidas. Ele para de rodar até alguém escrever nos registradores, chegar uma interrupção ou o tempo `until`
// (a próxima mudança de HBLANK/VBLANK, para o FBCTL; o temporizador interno dele).
void Mars::notePoll(Sh2& c, uint32_t a, uint16_t v, int64_t until) {
  // até 4 leituras diferentes no laço (por exemplo os dois meios de um COMM de 32 bits)
  Poll& p = poll_[c.id];
  for (int k = 0; k < 4; k++) {
    if (p.pc[k] == c.pc && p.addr[k] == a && p.val[k] == v) {
      if (++p.cnt >= 8) {
        p.on = true;
        p.wake = std::min(until, c.timerNext());
        c.stop = true;
      }
      return;
    }
  }
  p.next = (p.next + 1) & 3;
  p.pc[p.next] = c.pc; p.addr[p.next] = a; p.val[p.next] = v;
  p.cnt = 0;
}

void Mars::wake(int i) { poll_[i].on = false; poll_[i].cnt = 0; }

void Mars::raise(int bit) {
  for (int i = 0; i < 2; i++)
    if (bit == P_VRES || (mask_[i] & (1 << bit))) pend_[i] |= uint8_t(1 << bit);
  updateIrl();
}

// ------------------------------------------------------------ linhas e quadro
void Mars::busLine(int line, int64_t mclk) {
  syncTo(sh2Time(mclk));
  lineStart_ = now_;
  const int act = md.vdp.activeLines();
  if (line == act) vblankStart();
  if (line == 0) { vblank_ = false; wake(0); wake(1); }
  if (!aden_) return;
  if (line < act || hen_) {
    if (hcnt_ == 0) { hcnt_ = hcount_; raise(P_HINT); }
    else hcnt_--;
  } else {
    hcnt_ = hcount_;
  }
}

void Mars::vblankStart() {
  vblank_ = true;
  wake(0); wake(1);
  if (fsPending_ != fs_) swapFb();
  if (aden_) raise(P_VINT);
}

void Mars::swapFb() {
  fs_ = fsPending_ & 1;
  mapPages();
}

void Mars::busLineDrawn(int row, uint32_t* px, const uint8_t* idx, int w) {
  if (!aden_ || !(vmode_ & 3)) return;
  if (md.vdp.interlace2()) row >>= 1;
  if (row < 0 || row >= 240) return;
  const uint8_t* d = fb_[fs_].data();
  const uint32_t base = uint32_t(be16(d + row * 2)) * 2;
  uint16_t c32[320];
  switch (vmode_ & 3) {
    case 1:
      for (int x = 0; x < 320; x++) c32[x] = pal_[d[(base + shift_ + x) & 0x1FFFF]];
      break;
    case 2:
      for (int x = 0; x < 320; x++) c32[x] = be16(d + ((base + x * 2) & 0x1FFFE));
      break;
    default: {
      int x = 0;
      uint32_t p = base;
      while (x < 320) {
        const uint16_t wv = be16(d + (p & 0x1FFFE));
        p += 2;
        const uint16_t c = pal_[wv & 0xFF];
        for (int n = (wv >> 8) + 1; n > 0 && x < 320; n--) c32[x++] = c;
        if (p - base > 0x20000) break;
      }
      while (x < 320) c32[x++] = 0;
      break;
    }
  }
  const uint32_t* lut = colorLut();
  const int bgi = md.vdp.reg[7] & 0x3F;
  const uint16_t inv = (vmode_ & 0x80) ? 0x8000 : 0;
  if (w > 320) w = 320;
  for (int i = 0; i < w; i++) {
    const uint16_t c = c32[w == 320 ? i : i * 5 / 4];
    if (idx[i] == bgi || ((c ^ inv) & 0x8000)) px[i] = lut[c & 0x7FFF];
  }
}

void Mars::frameEnd(size_t count) {
  syncTo(sh2Time(md.clock()));
  // som: PWM amostrado em `count` pontos espalhados pelo quadro (o mesmo número de amostras que o Mega Drive
  // produziu, para as duas filas andarem juntas), segurando o último valor entre os pulsos
  const double span = double(now_ - audioT0_);
  size_t li = 0;
  for (size_t k = 0; k < count; k++) {
    const double t = double(audioT0_) + (double(k) + 1.0) * span / double(count);
    while (li < pwmLog_.size() && double(pwmLog_[li].t) <= t) { held_[0] = pwmLog_[li].l; held_[1] = pwmLog_[li].r; li++; }
    outL_[outW_] = held_[0];
    outR_[outW_] = held_[1];
    outW_ = (outW_ + 1) % OUTBUF;
    if (outW_ == outR0_) outR0_ = (outR0_ + 1) % OUTBUF;
  }
  while (li < pwmLog_.size()) { held_[0] = pwmLog_[li].l; held_[1] = pwmLog_[li].r; li++; }
  audioT0_ = now_;
  pwmLog_.clear();
}

void Mars::setSampleRate(int rate) { rate_ = rate > 0 ? rate : 48000; }

size_t Mars::audioAvailable() const { return (outW_ + OUTBUF - outR0_) % OUTBUF; }

void Mars::mixInto(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (outR0_ == outW_) break;
    l[k] = std::clamp(l[k] + outL_[outR0_] * PWM_GAIN, -1.0f, 1.0f);
    r[k] = std::clamp(r[k] + outR_[outR0_] * PWM_GAIN, -1.0f, 1.0f);
    outR0_ = (outR0_ + 1) % OUTBUF;
  }
}

// ------------------------------------------------------------ 68000
bool Mars::busRead(uint32_t a, int size, uint16_t& v) {
  if (bypass_ || a >= 0xE00000) return false;
  if (a >= 0xA10000) {
    if (a >= 0xA20000) return false;
    if ((a & 0xFFFFC0) == 0xA15100) { sync68k(); v = sysRead68k(a); }
    else if (aden_ && (a & 0xFFFFF0) == 0xA15180) { sync68k(); v = vdpRead(a); }
    else if (aden_ && (a & 0xFFFE00) == 0xA15200) v = pal_[(a & 0x1FF) >> 1];
    else if ((a & 0xFFFFFC) == 0xA130EC) v = (a & 2) ? 0x5253 : 0x4D41;  // "MARS"
    else return false;
    if (size == 1) v = (a & 1) ? (v & 0xFF) : (v >> 8);
    return true;
  }
  if (!aden_) return false;
  if (a < 0x100) {
    if (rv_) return false;
    auto byte = [&](uint32_t x) -> uint8_t { return (x & ~3u) == 0x70 ? hintVec_[x & 3] : bios68_[x & 0xFF]; };
    v = size == 1 ? byte(a) : uint16_t((byte(a) << 8) | byte(a + 1));
    return true;
  }
  if (a < 0x840000) return false;
  if (a < 0x880000) {
    sync68k();
    v = fbRead(a & 0x1FFFF, size);
    return true;
  }
  if (a < 0xA00000) {
    const uint32_t ca = a < 0x900000 ? a - 0x880000 : (uint32_t(bank_) << 20) | (a & 0xFFFFF);
    bypass_ = true;
    v = size == 1 ? md.read8(ca) : md.read16(ca);
    bypass_ = false;
    return true;
  }
  return false;
}

bool Mars::busWrite(uint32_t a, int size, uint16_t v) {
  if (bypass_ || a >= 0xE00000) return false;
  if (a >= 0xA10000) {
    if (a >= 0xA20000) return false;
    if ((a & 0xFFFFC0) == 0xA15100) { sync68k(); sysWrite68k(a, size, v); return true; }
    if (aden_ && (a & 0xFFFFF0) == 0xA15180) { if (!fm_) { sync68k(); vdpWrite(a, size, v); } return true; }
    if (aden_ && (a & 0xFFFE00) == 0xA15200) { if (!fm_) { sync68k(); palWrite(a, size, v); } return true; }
    return false;
  }
  if (!aden_) return false;
  if (a < 0x100) {
    if (rv_) return false;
    if ((a & ~3u) == 0x70) {
      if (size == 2) { hintVec_[a & 2] = uint8_t(v >> 8); hintVec_[(a & 2) + 1] = uint8_t(v); }
      else hintVec_[a & 3] = uint8_t(v);
    }
    return true;
  }
  if (a < 0x840000) return false;
  if (a < 0x880000) {
    sync68k();
    fbWrite(a & 0x3FFFF, size, v);
    return true;
  }
  if (a < 0xA00000) {
    const uint32_t ca = a < 0x900000 ? a - 0x880000 : (uint32_t(bank_) << 20) | (a & 0xFFFFF);
    bypass_ = true;
    if (size == 1) md.write8(ca, uint8_t(v)); else md.write16(ca, v);
    bypass_ = false;
    return true;
  }
  return false;
}

uint16_t Mars::sysRead68k(uint32_t a) {
  switch (a & 0x3E) {
    case 0x00: return uint16_t((fm_ ? 0x8000 : 0) | 0x80 | (nres_ ? 2 : 0) | (aden_ ? 1 : 0));
    case 0x02: return intCmd_;
    case 0x04: return bank_;
    case 0x06: return uint16_t((fifoN_ >= 8 ? 0x80 : 0) | (s68_ ? 4 : 0) | (dmaBit_ ? 2 : 0) | (rv_ ? 1 : 0));
    case 0x08: return dreqSrcH_;
    case 0x0A: return dreqSrcL_;
    case 0x0C: return dreqDstH_;
    case 0x0E: return dreqDstL_;
    case 0x10: return dreqLen_;
    case 0x1A: return tv_;
    default:
      if ((a & 0x30) == 0x20) return comm_[(a >> 1) & 7];
      if ((a & 0x30) == 0x30) return pwmRead(a);
      return 0;
  }
}

void Mars::sysWrite68k(uint32_t a, int size, uint16_t v) {
  const uint32_t r = a & 0x3E;
  wake(0); wake(1);
  // escrita de 8 bits: junta com a outra metade do registrador
  auto merge = [&](uint16_t old) -> uint16_t {
    if (size == 2) return v;
    return (a & 1) ? uint16_t((old & 0xFF00) | (v & 0xFF)) : uint16_t((old & 0x00FF) | ((v & 0xFF) << 8));
  };
  switch (r) {
    case 0x00: {
      const uint16_t cur = sysRead68k(0);
      const uint16_t nv = merge(cur);
      fm_ = nv & 0x8000;
      if (size == 1 && !(a & 1)) return;  // só o byte do FM
      const bool newAden = nv & 1, newRes = nv & 2;
      if (newAden && !aden_) {        // liga o adaptador: os SH-2 começam em reset, a não ser que nRES venha junto
        aden_ = true;
        nres_ = false;
        mapPages();
      } else if (!newAden && aden_) {
        aden_ = false;
      }
      if (newRes && !nres_ && aden_) { nres_ = true; resetSh2s(); }
      else nres_ = newRes;
      updateIrl();
      return;
    }
    case 0x02: intCmd_ = uint8_t(merge(intCmd_) & 3); updateIrl(); return;
    case 0x04: bank_ = uint8_t(merge(bank_) & 3); return;
    case 0x06: {
      const uint16_t nv = merge(sysRead68k(6));
      rv_ = nv & 1; dmaBit_ = nv & 2; s68_ = nv & 4;
      if (!s68_) fifoN_ = 0;
      return;
    }
    case 0x08: dreqSrcH_ = merge(dreqSrcH_) & 0xFF; return;
    case 0x0A: dreqSrcL_ = merge(dreqSrcL_) & 0xFFFE; return;
    case 0x0C: dreqDstH_ = merge(dreqDstH_) & 0xFF; return;
    case 0x0E: dreqDstL_ = merge(dreqDstL_); return;
    case 0x10: dreqLen_ = merge(dreqLen_) & 0xFFFC; return;
    case 0x12:
      if (size == 2) fifoPush(v);
      else if (!(a & 1)) fifoLatch_ = uint8_t(v);
      else { fifoPush(uint16_t((fifoLatch_ << 8) | (v & 0xFF))); fifoLatch_ = 0; }
      return;
    case 0x1A: tv_ = merge(tv_) & 0x0101; return;
    default:
      if ((r & 0x30) == 0x20) { comm_[(r >> 1) & 7] = merge(comm_[(r >> 1) & 7]); return; }
      if ((r & 0x30) == 0x30) {
        if (r == 0x30) { pwmCtl_ = uint16_t((pwmCtl_ & ~0xF) | (merge(pwmCtl_) & 0xF)); pwmCtlChanged(); return; }
        pwmByteWrite(a, size, v);
        return;
      }
      return;
  }
}

// ------------------------------------------------------------ FIFO do DREQ
void Mars::fifoPush(uint16_t v) {
  if (!s68_) return;
  if (fifoN_ < 8) {
    fifo_[fifoN_++] = v;
    dreqLen_ = uint16_t(dreqLen_ - 1);
    if (dreqLen_ == 0) s68_ = false;
  }
  if ((fifoN_ & 3) == 0 || !s68_) dreq0();
}

uint16_t Mars::fifoPop() {
  wake(0); wake(1);
  if (!fifoN_) return fifo_[0];
  const uint16_t v = fifo_[0];
  std::memmove(fifo_, fifo_ + 1, sizeof(uint16_t) * 7);
  fifoN_--;
  return v;
}

void Mars::dreq0() {
  wake(0); wake(1);
  for (Sh2* c : {&msh2, &ssh2}) {
    if (!fifoN_) return;
    if (c->dmaActive(0) && (c->dmaSource(0) & 0x1FFFFFFF) == 0x4012) c->dreq(0, fifoN_);
  }
}

void Mars::dmaKick(Sh2&, int ch) {
  if (ch == 0 && fifoN_ && ((fifoN_ & 3) == 0 || !s68_)) dreq0();
}

// ------------------------------------------------------------ SH-2
uint16_t Mars::sysReadSh2(Sh2& c, uint32_t a) {
  switch (a & 0x3E) {
    case 0x00: return uint16_t((fm_ ? 0x8000 : 0) | (aden_ ? 0x200 : 0) | (hen_ ? 0x80 : 0) | mask_[c.id]);
    case 0x04: return hcount_;
    case 0x06: return uint16_t((fifoN_ == 0 ? 0x4000 : 0) | (s68_ ? 4 : 0) | (dmaBit_ ? 2 : 0) | (rv_ ? 1 : 0));
    case 0x08: return dreqSrcH_;
    case 0x0A: return dreqSrcL_;
    case 0x0C: return dreqDstH_;
    case 0x0E: return dreqDstL_;
    case 0x10: return dreqLen_;
    case 0x12: return fifoPop();
    default:
      if ((a & 0x30) == 0x20) return comm_[(a >> 1) & 7];
      if ((a & 0x30) == 0x30) return pwmRead(a);
      return 0;
  }
}

void Mars::sysWriteSh2(Sh2& c, uint32_t a, int size, uint16_t v) {
  const uint32_t r = a & 0x3E;
  wake(c.id ^ 1);
  auto merge = [&](uint16_t old) -> uint16_t {
    if (size == 2) return v;
    return (a & 1) ? uint16_t((old & 0xFF00) | (v & 0xFF)) : uint16_t((old & 0x00FF) | ((v & 0xFF) << 8));
  };
  switch (r) {
    case 0x00: {
      const uint16_t nv = merge(sysReadSh2(c, 0));
      if (size == 2 || !(a & 1)) fm_ = nv & 0x8000;
      if (size == 2 || (a & 1)) { hen_ = nv & 0x80; mask_[c.id] = uint8_t(nv & 0xF); }
      updateIrl();
      return;
    }
    case 0x04: hcount_ = uint8_t(merge(hcount_)); return;
    case 0x14: pend_[c.id] &= ~(1 << P_VRES); updateIrl(); return;
    case 0x16: pend_[c.id] &= ~(1 << P_VINT); updateIrl(); return;
    case 0x18: pend_[c.id] &= ~(1 << P_HINT); updateIrl(); return;
    case 0x1A: intCmd_ &= ~(1 << c.id); updateIrl(); return;
    case 0x1C: pend_[c.id] &= ~(1 << P_PWM); updateIrl(); return;
    default:
      if ((r & 0x30) == 0x20) { comm_[(r >> 1) & 7] = merge(comm_[(r >> 1) & 7]); return; }
      if ((r & 0x30) == 0x30) {
        if (r == 0x30) { pwmCtl_ = merge(pwmCtl_) & 0x0F8F; pwmCtlChanged(); return; }
        pwmByteWrite(a, size, v);
      }
      return;
  }
}

uint8_t Mars::rd8(Sh2& c, uint32_t a) {
  const uint16_t v = rd16(c, a & ~1u);
  return uint8_t((a & 1) ? v : v >> 8);
}

uint16_t Mars::rd16(Sh2& c, uint32_t a) {
  a &= ~1u;
  switch (a >> 24) {
    case 0x00:
      if ((a & 0x3FFC0) == 0x4000) {
        const uint16_t v = sysReadSh2(c, a);
        if ((a & 0x3E) != 0x12) notePoll(c, a & 0x3FFFE, v, INT64_MAX);
        return v;
      }
      if ((a & 0x3FFF0) == 0x4100) {
        const uint16_t v = vdpReadAt(a, c.clock);
        if ((a & 0xE) == 0xA) {
          // FBCTL muda sozinho no começo e no fim da HBLANK (e da VBLANK, na troca de linha) e no fim do preenchimento
          const int64_t hb = lineStart_ + 2560 * 3 / 7;
          int64_t w = c.clock < hb ? hb : lineStart_ + 3420 * 3 / 7;
          if (c.clock < fillEnd_) w = std::min(w, fillEnd_);
          notePoll(c, a & 0x3FFFE, v, w);
        }
        return v;
      }
      if ((a & 0x3FE00) == 0x4200) return pal_[(a & 0x1FF) >> 1];
      if (c.id == 0) return a < sizeof biosM_ ? be16(biosM_ + a) : 0;
      return a < sizeof biosS_ ? be16(biosS_ + a) : 0;
    case 0x02: return be16(&rom_[a & romMask_]);
    case 0x04: return fbRead(a & 0x1FFFF, 2);
    case 0x06: return be16(&sdram_[a & 0x3FFFE]);
    default: return 0;
  }
}

void Mars::wr8(Sh2& c, uint32_t a, uint8_t v) {
  switch (a >> 24) {
    case 0x00:
      if ((a & 0x3FFC0) == 0x4000) { sysWriteSh2(c, a, 1, v); return; }
      if (fm_ && (a & 0x3FFF0) == 0x4100) { vdpWriteAt(a, 1, v, c.clock, true); return; }
      if (fm_ && (a & 0x3FE00) == 0x4200) { palWrite(a, 1, v); return; }
      return;
    case 0x04: fbWrite(a & 0x3FFFF, 1, v); return;
    case 0x06: sdram_[a & 0x3FFFF] = v; return;
    default: return;
  }
}

void Mars::wr16(Sh2& c, uint32_t a, uint16_t v) {
  a &= ~1u;
  switch (a >> 24) {
    case 0x00:
      if ((a & 0x3FFC0) == 0x4000) { sysWriteSh2(c, a, 2, v); return; }
      if (fm_ && (a & 0x3FFF0) == 0x4100) { vdpWriteAt(a, 2, v, c.clock, true); return; }
      if (fm_ && (a & 0x3FE00) == 0x4200) { palWrite(a, 2, v); return; }
      return;
    case 0x04: fbWrite(a & 0x3FFFF, 2, v); return;
    case 0x06: put16(&sdram_[a & 0x3FFFE], v); return;
    default: return;
  }
}

// ------------------------------------------------------------ VDP do 32X
uint16_t Mars::vdpRead(uint32_t a) { return vdpReadAt(a, now_); }

uint16_t Mars::vdpReadAt(uint32_t a, int64_t t) {
  switch (a & 0xE) {
    case 0x0: return uint16_t((md.pal ? 0 : 0x8000) | vmode_);
    case 0x2: return shift_;
    case 0x4: return fillLen_;
    case 0x6: return fillAddr_;
    case 0x8: return fillData_;
    case 0xA: {
      // HBLANK: parte final da linha (a imagem ocupa 2560 dos 3420 ciclos mestres)
      const bool hb = (t - lineStart_) * 7 >= 2560 * 3;
      const bool pen = vblank_ || hb || !(vmode_ & 3);
      return uint16_t((vblank_ ? 0x8000 : 0) | (hb ? 0x4000 : 0) | (pen ? 0x2000 : 0) | (t < fillEnd_ ? 2 : 0) | fs_);
    }
    default: return 0;
  }
}

void Mars::vdpWrite(uint32_t a, int size, uint16_t v) { vdpWriteAt(a, size, v, now_, false); }

void Mars::vdpWriteAt(uint32_t a, int size, uint16_t v, int64_t t, bool fromSh2) {
  const uint32_t r = a & 0xE;
  wake(0); wake(1);
  // escrita de 8 bits no byte de cima da palavra não muda nada (todos os campos ficam no byte de baixo), menos o
  // endereço e o dado do preenchimento
  if (size == 1) {
    if (r == 0x6 || r == 0x8) {
      const uint16_t old = r == 0x6 ? fillAddr_ : fillData_;
      v = (a & 1) ? uint16_t((old & 0xFF00) | (v & 0xFF)) : uint16_t((old & 0xFF) | ((v & 0xFF) << 8));
    } else if (!(a & 1)) {
      return;
    }
  }
  switch (r) {
    case 0x0: vmode_ = uint8_t(v & 0xC3); return;
    case 0x2: shift_ = v & 1; return;
    case 0x4: fillLen_ = uint8_t(v); return;
    case 0x6: fillAddr_ = v; return;
    case 0x8: {
      fillData_ = v;
      uint8_t* d = fb_[fs_ ^ 1].data();
      uint16_t ad = fillAddr_;
      for (int i = 0; i <= fillLen_; i++) {
        put16(d + uint32_t(ad) * 2, v);
        ad = uint16_t((ad & 0xFF00) | ((ad + 1) & 0xFF));
      }
      fillAddr_ = ad;
      // o preenchimento leva uns 3 ciclos por palavra; pelo 68000 a referência termina na hora
      fillEnd_ = fromSh2 ? t + 9 + (fillLen_ + 1) * 3 : 0;
      return;
    }
    case 0xA:
      fsPending_ = v & 1;
      if (vblank_ || !(vmode_ & 3)) { if (fsPending_ != fs_) swapFb(); }
      return;
    default: return;
  }
}

void Mars::palWrite(uint32_t a, int size, uint16_t v) {
  uint16_t& p = pal_[(a & 0x1FF) >> 1];
  if (size == 2) p = v;
  else if (a & 1) p = uint16_t((p & 0xFF00) | (v & 0xFF));
  else p = uint16_t((p & 0xFF) | ((v & 0xFF) << 8));
}

uint16_t Mars::fbRead(uint32_t off, int size) {
  const uint8_t* d = fb_[fs_ ^ 1].data();
  if (size == 1) return d[off & 0x1FFFF];
  return be16(d + (off & 0x1FFFE));
}

// quadro: 8 bits só gravam valores diferentes de zero; na área de sobreposição (bit 17) cada byte zero é
// ignorado também nas palavras
void Mars::fbWrite(uint32_t off, int size, uint16_t v) {
  uint8_t* d = fb_[fs_ ^ 1].data();
  const bool over = off & 0x20000;
  off &= 0x1FFFF;
  if (size == 1) { if (v & 0xFF) d[off] = uint8_t(v); return; }
  off &= ~1u;
  if (!over) { put16(d + off, v); return; }
  if (v & 0xFF00) d[off] = uint8_t(v >> 8);
  if (v & 0xFF) d[off + 1] = uint8_t(v);
}

// ------------------------------------------------------------ PWM
void Mars::pwmCtlChanged() {
  pwmPeriod_ = (pwmCycle_ - 1) & 0xFFF;
  pwmTimer_ = ((pwmCtl_ >> 8) & 0xF) ? (pwmCtl_ >> 8) & 0xF : 16;
  if (!pwmPeriod_) pwmNext_ = INT64_MAX;
  else if (pwmNext_ == INT64_MAX) pwmNext_ = now_ + pwmPeriod_;
}

void Mars::pwmTick() {
  if (!pwmPeriod_) { pwmNext_ = INT64_MAX; return; }
  for (int i = 0; i < 2; i++)
    if (poll_[i].on)
      for (uint32_t a : poll_[i].addr) if ((a & 0x3FFF0) == 0x4030 || (a & 0x3FFF8) == 0x4038) { wake(i); break; }
  const float scale = 2.0f / float(pwmPeriod_ + 1);
  for (int ch = 0; ch < 2; ch++) {
    if (!pwmN_[ch]) continue;
    const int v = std::min<int>(pwmFifo_[ch][0], pwmPeriod_);
    pwmOut_[ch] = float(v) * scale - 1.0f;
    pwmFifo_[ch][0] = pwmFifo_[ch][1]; pwmFifo_[ch][1] = pwmFifo_[ch][2];
    pwmN_[ch]--;
  }
  const int lmd = pwmCtl_ & 3, rmd = (pwmCtl_ >> 2) & 3;
  const float l = lmd == 1 ? pwmOut_[0] : lmd == 2 ? pwmOut_[1] : 0.0f;
  const float r = rmd == 1 ? pwmOut_[1] : rmd == 2 ? pwmOut_[0] : 0.0f;
  if (pwmLog_.size() < 65536) pwmLog_.push_back({pwmNext_, l, r});
  const int64_t t = pwmNext_;
  pwmNext_ += pwmPeriod_;
  if (--pwmTimer_ <= 0) {
    pwmTimer_ = ((pwmCtl_ >> 8) & 0xF) ? (pwmCtl_ >> 8) & 0xF : 16;
    if (aden_) raise(P_PWM);
    if ((pwmCtl_ & 0x80) && running()) {  // RTP: pedido DREQ1 a cada interrupção do temporizador
      const int64_t keep = now_;
      now_ = t;
      for (Sh2* c : {&msh2, &ssh2}) if (c->dmaActive(1)) c->dreq(1, 1);
      now_ = keep;
    }
  }
}

uint16_t Mars::pwmRead(uint32_t a) {
  switch (a & 0xE) {
    case 0x0: return pwmCtl_;
    case 0x2: return pwmCycle_;
    case 0x4: return uint16_t((pwmN_[0] == 3 ? 0x8000 : 0) | (pwmN_[0] == 0 ? 0x4000 : 0));
    case 0x6: case 0x8: return uint16_t((pwmN_[1] == 3 ? 0x8000 : 0) | (pwmN_[1] == 0 ? 0x4000 : 0));
    default: return 0;
  }
}

void Mars::pwmPush(int ch, uint16_t v) {
  const uint16_t s = uint16_t((v - 1) & 0xFFF);
  if (pwmN_[ch] < 3) pwmFifo_[ch][pwmN_[ch]++] = s;
  else { pwmFifo_[ch][0] = pwmFifo_[ch][1]; pwmFifo_[ch][1] = pwmFifo_[ch][2]; pwmFifo_[ch][2] = s; }  // cheia: perde a mais velha
}

void Mars::pwmWrite(uint32_t a, uint16_t v) {
  switch (a & 0xE) {
    case 0x2: pwmCycle_ = v & 0xFFF; pwmCtlChanged(); return;
    case 0x4: pwmPush(0, v); return;
    case 0x6: pwmPush(1, v); return;
    case 0x8: pwmPush(0, v); pwmPush(1, v); return;
    default: return;
  }
}

// escritas de 8 bits nos pulsos: o byte par fica guardado e o ímpar completa a palavra
void Mars::pwmByteWrite(uint32_t a, int size, uint16_t v) {
  const uint32_t r = a & 0xE;
  if (size == 2) { pwmWrite(a, v); return; }
  if (r == 0x2) {
    pwmCycle_ = (a & 1) ? uint16_t((pwmCycle_ & 0xF00) | (v & 0xFF)) : uint16_t((pwmCycle_ & 0xFF) | ((v & 0xF) << 8));
    pwmCtlChanged();
    return;
  }
  if (r < 0x4 || r > 0x8) return;
  if (!(a & 1)) { pwmLatch_ = uint8_t(v); return; }
  pwmWrite(a & ~1u, uint16_t((pwmLatch_ << 8) | (v & 0xFF)));
  pwmLatch_ = 0;
}

// ------------------------------------------------------------ estado
void Mars::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, STATE_MAGIC)) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  msh2.sync(io);
  ssh2.sync(io);
  io.bytes(sdram_); io.bytes(fb_[0]); io.bytes(fb_[1]);
  io.sync(aden_); io.sync(nres_); io.sync(fm_); io.sync(rv_); io.sync(s68_); io.sync(dmaBit_); io.sync(hen_);
  io.sync(intCmd_); io.sync(bank_); io.raw(mask_, sizeof mask_); io.raw(pend_, sizeof pend_);
  io.sync(hcount_); io.sync(hcnt_); io.raw(comm_, sizeof comm_);
  io.sync(dreqSrcH_); io.sync(dreqSrcL_); io.sync(dreqDstH_); io.sync(dreqDstL_); io.sync(dreqLen_);
  io.raw(fifo_, sizeof fifo_); io.sync(fifoN_); io.sync(fifoLatch_); io.sync(tv_); io.raw(hintVec_, sizeof hintVec_);
  io.sync(vmode_); io.sync(shift_); io.sync(fillLen_); io.sync(fillAddr_); io.sync(fillData_);
  io.sync(fs_); io.sync(fsPending_); io.sync(vblank_); io.sync(fillEnd_); io.raw(pal_, sizeof pal_); io.sync(lineStart_);
  io.sync(pwmCtl_); io.sync(pwmCycle_); io.raw(pwmFifo_, sizeof pwmFifo_); io.raw(pwmN_, sizeof pwmN_);
  io.sync(pwmTimer_); io.sync(pwmNext_); io.raw(pwmOut_, sizeof pwmOut_); io.sync(pwmLatch_);
  uint32_t nlog = static_cast<uint32_t>(pwmLog_.size());
  io.sync(nlog);
  if (io.reading()) { if (nlog > 65536) throw std::runtime_error("Estado salvo danificado."); pwmLog_.resize(nlog); }
  for (auto& s : pwmLog_) { io.sync(s.t); io.sync(s.l); io.sync(s.r); }
  io.sync(audioT0_); io.raw(held_, sizeof held_);
  io.sync(now_);
  for (auto& p : poll_) { io.raw(p.pc, sizeof p.pc); io.raw(p.addr, sizeof p.addr); io.raw(p.val, sizeof p.val); io.sync(p.next); io.sync(p.cnt); io.sync(p.on); io.sync(p.wake); }
  if (io.reading()) {
    for (auto& p : poll_) { clampTo(p.cnt, 0, 8); p.next &= 3; }
    clampTo(fifoN_, 0, 8);
    for (auto& n : pwmN_) clampTo(n, 0, 3);
    clampTo(pwmTimer_, 1, 16);
    bank_ &= 3; fs_ &= 1; fsPending_ &= 1; intCmd_ &= 3;
    for (auto& m : mask_) m &= 0xF;
    for (auto& p : pend_) p &= 0x1F;
    clampTo(hcnt_, 0, 255);
    pwmPeriod_ = (pwmCycle_ - 1) & 0xFFF;
    if (pwmNext_ != INT64_MAX) clampTo<int64_t>(pwmNext_, now_ - 0x1000, now_ + 0x1000);
    for (float* f : {&pwmOut_[0], &pwmOut_[1], &held_[0], &held_[1]}) if (!(*f == *f) || *f > 4 || *f < -4) *f = 0;
    for (auto& s : pwmLog_) {
      clampTo<int64_t>(s.t, now_ - (int64_t(1) << 24), now_ + (int64_t(1) << 24));
      if (!(s.l == s.l) || s.l > 4 || s.l < -4) s.l = 0;
      if (!(s.r == s.r) || s.r > 4 || s.r < -4) s.r = 0;
    }
    clampTo<int64_t>(audioT0_, now_ - (int64_t(1) << 24), now_);
    for (Sh2* c : {&msh2, &ssh2}) clampTo<int64_t>(c->clock, now_ - (int64_t(1) << 24), now_ + (int64_t(1) << 24));
    clampTo<int64_t>(lineStart_, now_ - (int64_t(1) << 24), now_);
    mapPages();
    updateIrl();
  }
}

// ------------------------------------------------------------ núcleo
Md32xCore::Md32xCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save)
    : md_(std::make_unique<md::MD>(rom, save)) {
  mars_ = std::make_unique<Mars>(*md_, rom);
  md_->reset();
  mars_->power();
}

void Md32xCore::reset() {
  mars_->power();
  md_->reset();
  mars_->power();
}

void Md32xCore::runFrame() {
  const size_t before = md_->audioAvailable();
  md_->runFrame();
  const size_t after = md_->audioAvailable();
  mars_->frameEnd(after >= before ? after - before : 0);
}

size_t Md32xCore::audioAvailable() const { return std::min(md_->audioAvailable(), mars_->audioAvailable()); }

void Md32xCore::pullAudio(float* l, float* r, size_t n) {
  md_->pullAudio(l, r, n);
  mars_->mixInto(l, r, n);
}

std::vector<uint8_t> Md32xCore::saveState() {
  StateIO io;
  std::vector<uint8_t> m = md_->saveState();
  io.bytes(m);
  mars_->sync(io);
  return io.data();
}

void Md32xCore::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  auto apply = [&](const std::vector<uint8_t>& src) {
    StateIO io(src);
    uint32_t n = 0;
    io.sync(n);
    if (n > src.size()) throw std::runtime_error("Estado salvo truncado.");
    std::vector<uint8_t> m(n);
    if (n) io.raw(m.data(), n);
    md_->loadState(m);
    mars_->sync(io);
  };
  try {
    apply(s);
  } catch (...) {
    apply(backup);
    throw;
  }
}

std::string Md32xCore::note() const {
  return mars_->usingBios() ? "" : "32X sem as BIOS originais: partida própria do Índigo (HLE).";
}

}  // namespace indigo::md32x
