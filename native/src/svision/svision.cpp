#include "svision.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::sv {

namespace {
const char STATE_MAGIC[4] = {'I', 'S', 'V', '1'};
// tons de cinza do LCD (0 = claro, 3 = escuro), 0xAABBGGRR
const uint32_t GRAYS[4] = {0xFFE0E0E0u, 0xFFB9B9B9u, 0xFF545454u, 0xFF121212u};
// registradores
enum : int {
  R_XSIZE = 0x00, R_XPOS = 0x02, R_YPOS = 0x03,
  R_VDMA_SRC = 0x08, R_VDMA_DST = 0x0A, R_VDMA_LEN = 0x0C, R_VDMA_GO = 0x0D,
  R_DMA_START = 0x18, R_DMA_SIZE = 0x1A, R_DMA_CTRL = 0x1B, R_DMA_ON = 0x1C,
  R_JOY = 0x20, R_LINK_IN = 0x21, R_LINK_OUT = 0x22, R_TIMER = 0x23, R_TIMER_ACK = 0x24, R_DMA_ACK = 0x25,
  R_SYS = 0x26, R_IRQ = 0x27, R_NOISE_VOL = 0x28, R_NOISE_LEN = 0x29, R_NOISE_CTRL = 0x2A,
};
// $2026: bit 0 NMI por quadro, 1 IRQ do timer, 2 IRQ do DMA de áudio, 3 LCD ligado, 4 timer lento, 5-7 banco
constexpr int SYS_NMI = 1, SYS_TIMER = 2, SYS_DMA = 4, SYS_LCD = 8, SYS_SLOW = 0x10;
// fração do período com a saída ligada, em 32 avos, para cada valor do ciclo de trabalho (bits 4-5)
const int DUTY[4] = {28, 24, 16, 9};
constexpr int STEP = 16;  // ciclos por passo do som
bool endsWith(const std::string& s, const char* x) {
  const size_t l = std::strlen(x);
  return s.size() >= l && s.compare(s.size() - l, l, x) == 0;
}
}  // namespace

bool isSvRom(const std::vector<uint8_t>& rom, const std::string& n) {
  return endsWith(n, ".sv") && rom.size() >= 0x100 && rom.size() <= (1u << 20);
}

Supervision::Supervision(const std::vector<uint8_t>& src) : cpu(*this) {
  // cartucho em bancos de 16 KB; arquivos menores são espelhados até completar um banco
  size_t size = 0x4000;
  while (size < src.size()) size += 0x4000;
  rom_.assign(size, 0xFF);
  for (size_t k = 0; k < size; k++) rom_[k] = src[k % src.size()];
  banks_ = static_cast<int>(size >> 14);
  std::copy(GRAYS, GRAYS + 4, pal_);
  reset();
}

void Supervision::reset() {
  std::memset(ram, 0, sizeof ram);
  std::memset(vram, 0, sizeof vram);
  std::memset(reg, 0, sizeof reg);
  reg[R_XSIZE] = 0xA0;
  timerShot_ = dmaDone_ = false;
  timerAt_ = 0;
  for (auto& t : tone_) t = Tone{};
  noise_ = Noise{};
  dma_ = Dma{};
  frame = 0;
  frameEnd_ = now + CYCLES_PER_FRAME;
  cpu.s = 0xFF;
  cpu.p = 0x24;
  cpu.reset();
  render();
}

void Supervision::setKeys(uint32_t m) { keys_ = m; }

void Supervision::setPalette(const uint32_t* p4) {
  std::copy(p4, p4 + 4, pal_);
  render();
}

// ---------------------------------------------------------------- barramento
uint8_t Supervision::peek(uint16_t a) const {
  if (a < 0x2000) return ram[a];
  if (a < 0x4000) return reg[a & 0x1FFF];
  if (a < 0x6000) return vram[a & 0x1FFF];
  if (a < 0x8000) return 0xFF;
  if (a < 0xC000) return romAt(reg[R_SYS] >> 5, a);
  return romAt(banks_ - 1, a);
}

uint8_t Supervision::read(uint16_t a) {
  tick();
  if (a < 0x2000 || a >= 0x4000) return peek(a);
  const int r = a & 0x1FFF;
  switch (r) {
    case R_JOY: {  // ativo em 0
      uint8_t v = 0;
      if (keys_ & KEY_RIGHT) v |= 0x01;
      if (keys_ & KEY_LEFT) v |= 0x02;
      if (keys_ & KEY_DOWN) v |= 0x04;
      if (keys_ & KEY_UP) v |= 0x08;
      if (keys_ & KEY_B) v |= 0x10;
      if (keys_ & KEY_A) v |= 0x20;
      if (keys_ & KEY_SELECT) v |= 0x40;
      if (keys_ & KEY_START) v |= 0x80;
      return static_cast<uint8_t>(~v);
    }
    case R_LINK_IN: return static_cast<uint8_t>((reg[r] & 0xF0) | (reg[R_LINK_OUT] & 0x0F));  // sem cabo: volta o que sai
    case R_IRQ: return static_cast<uint8_t>((reg[r] & ~3) | (timerShot_ ? 1 : 0) | (dmaDone_ ? 2 : 0));
    case R_TIMER_ACK: timerShot_ = false; break;
    case R_DMA_ACK: dmaDone_ = false; break;
  }
  return reg[r];
}

void Supervision::write(uint16_t a, uint8_t v) {
  tick();
  if (a < 0x2000) { ram[a] = v; return; }
  if (a >= 0x4000) { if (a < 0x6000) vram[a & 0x1FFF] = v; return; }
  const int r = a & 0x1FFF;
  reg[r] = v;
  switch (r) {
    case R_TIMER: {
      // dispara uma vez depois de valor x 256 ciclos (ou x 16384 no modo lento); 0 vale 256
      const uint64_t n = (v ? v : 256) * uint64_t((reg[R_SYS] & SYS_SLOW) ? 16384 : 256);
      timerAt_ = now + n;
      timerShot_ = false;
      break;
    }
    case R_VDMA_GO:
      // cópia de blocos de 16 bytes (normalmente da ROM para a VRAM), com a CPU parada
      if ((v & 0x80) && !vdmaBusy_) {
        vdmaBusy_ = true;  // a cópia não dispara outra cópia dentro dela
        uint16_t s = reg[R_VDMA_SRC] | (reg[R_VDMA_SRC + 1] << 8);
        uint16_t d = reg[R_VDMA_DST] | (reg[R_VDMA_DST + 1] << 8);
        const int len = (reg[R_VDMA_LEN] ? reg[R_VDMA_LEN] : 256) * 16;
        for (int k = 0; k < len; k++) write(d++, read(s++));
        vdmaBusy_ = false;
      }
      break;
    default:
      if ((r >= 0x10 && r <= 0x1C) || (r >= R_NOISE_VOL && r <= R_NOISE_CTRL)) soundWrite(r, v);
  }
}

bool Supervision::irqLine() const {
  return (timerShot_ && (reg[R_SYS] & SYS_TIMER)) || (dmaDone_ && (reg[R_SYS] & SYS_DMA));
}

// ---------------------------------------------------------------- som
void Supervision::soundWrite(int r, uint8_t v) {
  audioRun();  // o que veio antes toca com os valores antigos
  if (r >= 0x10 && r <= 0x17) {
    Tone& t = tone_[(r >> 2) & 1];
    const int base = r & ~3;
    switch (r & 3) {
      case 0: case 1:
        t.period = (reg[base] | ((reg[base + 1] & 7) << 8)) * 32;  // frequência = 125000 / valor
        t.phase = 0;
        break;
      case 3: t.count = v + 1; break;  // toca por (valor + 1) quadros quando o bit 6 do controle está em 0
    }
    return;
  }
  switch (r) {
    case R_DMA_ON:
      dma_.on = (v & 0x80) != 0;
      dma_.pos = 0;
      dma_.counter = 0;
      dma_.size = (reg[R_DMA_SIZE] ? reg[R_DMA_SIZE] : 256) * 32;  // em amostras de 4 bits
      break;
    case R_NOISE_LEN: noise_.count = v + 1; break;
    case R_NOISE_CTRL: noise_.lfsr = 1; noise_.counter = 0; break;
  }
}

void Supervision::audioStep() {
  int l = 0, rr = 0;
  for (int c = 0; c < 2; c++) {
    Tone& t = tone_[c];
    const int base = 0x10 + c * 4;
    const uint8_t ctl = reg[base + 2];
    if (t.period) {
      if ((ctl & 0x40) || t.count) {
        const bool on = t.phase * 32 <= DUTY[(ctl >> 4) & 3] * t.period;
        if (on) (c == 0 ? rr : l) += ctl & 0x0F;  // canal 1 à direita, canal 2 à esquerda
      }
      t.phase = (t.phase + STEP) % t.period;
    }
  }
  const uint8_t nc = reg[R_NOISE_CTRL];
  if (nc & 0x10) {
    if ((nc & 2) || noise_.count) {
      const int s = noise_.out ? (reg[R_NOISE_VOL] & 0x0F) : 0;
      if (nc & 8) l += s;
      if (nc & 4) rr += s;
    }
    noise_.counter += STEP;
    const int per = 256 * (1 + (reg[R_NOISE_VOL] >> 4));
    while (noise_.counter >= per) {
      noise_.counter -= per;
      // registrador de deslocamento de 7 ou 14 bits (bit 0 do controle)
      const int top = (nc & 1) ? 13 : 6;
      noise_.out = (noise_.lfsr >> top) & 1;
      const int fb = ((noise_.lfsr >> top) ^ (noise_.lfsr >> (top - 1))) & 1;
      noise_.lfsr = static_cast<uint16_t>(((noise_.lfsr << 1) | fb) & ((2u << top) - 1));
      if (!noise_.lfsr) noise_.lfsr = 1;
    }
  }
  if (dma_.on) {
    const uint8_t ctl = reg[R_DMA_CTRL];
    const uint16_t addr = static_cast<uint16_t>((reg[R_DMA_START] | (reg[R_DMA_START + 1] << 8)) + dma_.pos / 2);
    // na faixa do banco, o DMA lê o banco escolhido nos bits 4-6 do controle
    const uint8_t b = (addr >= 0x8000 && addr < 0xC000) ? romAt((ctl >> 4) & 7, addr) : peek(addr);
    const int s = (dma_.pos & 1) ? (b & 0x0F) : (b >> 4);
    if (ctl & 8) l += s;
    if (ctl & 4) rr += s;
    dma_.counter += STEP;
    const int per = 256 * (1 + (ctl & 3));
    while (dma_.counter >= per && dma_.on) {
      dma_.counter -= per;
      if (++dma_.pos >= dma_.size) { dma_.on = false; dmaDone_ = true; reg[R_DMA_ON] &= 0x7F; }
    }
  }
  sumL_ += l; sumR_ += rr; sumN_++;
  sampleAcc_ += STEP;
  if (sampleAcc_ >= cyclesPerSample_) {
    sampleAcc_ -= cyclesPerSample_;
    // nível médio (0 a 45 por lado), sem a componente contínua
    const double xl = sumL_ / sumN_ / 48.0, xr = sumR_ / sumN_ / 48.0;
    sumL_ = sumR_ = 0; sumN_ = 0;
    hpL_ = 0.996 * (hpL_ + xl - lastL_); lastL_ = xl;
    hpR_ = 0.996 * (hpR_ + xr - lastR_); lastR_ = xr;
    if (audioAvailable() < BUF - 1) {
      bufL[wpos] = static_cast<float>(hpL_);
      bufR[wpos] = static_cast<float>(hpR_);
      wpos = (wpos + 1) % BUF;
    }
  }
}

void Supervision::audioRun() {
  while (pendingCycles_ >= STEP) { pendingCycles_ -= STEP; audioStep(); }
}

void Supervision::setSampleRate(int rate) {
  if (rate > 0) cyclesPerSample_ = double(CPU_HZ) / rate;
}

void Supervision::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos != wpos) { l[k] = bufL[rpos]; r[k] = bufR[rpos]; rpos = (rpos + 1) % BUF; }
    else { l[k] = r[k] = 0; }
  }
}

// ---------------------------------------------------------------- vídeo
void Supervision::render() {
  if (!(reg[R_SYS] & SYS_LCD)) {
    std::fill(fb, fb + SCREEN * SCREEN, pal_[0]);
    return;
  }
  // linha de 48 bytes, 4 pixels por byte (bit 0-1 = pixel da esquerda); a rolagem horizontal anda em pixels e a
  // vertical em linhas, voltando ao início antes de $1FE0
  const int xpos = reg[R_XPOS], endX = std::min(SCREEN, (reg[R_XSIZE] | 3) - 3);
  int j = xpos / 4 + reg[R_YPOS] * 0x30;
  for (int y = 0; y < SCREEN; y++) {
    uint32_t* line = fb + y * SCREEN;
    for (int x = 0; x < SCREEN; x++) {
      if (x >= endX) { line[x] = pal_[0]; continue; }
      const int k = x + (xpos & 3);
      const uint8_t b = vram[(j + k / 4) & 0x1FFF];
      line[x] = pal_[(b >> ((k & 3) * 2)) & 3];
    }
    j += 0x30;
    if (j >= 0x1FE0) j = 0;
  }
}

void Supervision::endFrame() {
  audioRun();
  render();
  for (auto& t : tone_) if (t.count) t.count--;
  if (noise_.count) noise_.count--;
  frame++;
  if (reg[R_SYS] & SYS_NMI) cpu.nmi();
}

void Supervision::runFrame() {
  using C = lynx::Cpu65SC02<Supervision>;
  while (now < frameEnd_) {
    if (timerAt_ && now >= timerAt_) { timerAt_ = 0; timerShot_ = true; }
    if (irqLine() && !(cpu.p & C::I)) cpu.irq();
    else cpu.step();
    if (pendingCycles_ >= 1024) audioRun();
  }
  frameEnd_ += CYCLES_PER_FRAME;
  endFrame();
}

// ---------------------------------------------------------------- save state
void Supervision::sync(StateIO& io) {
  cpu.sync(io);
  io.raw(ram, sizeof ram); io.raw(vram, sizeof vram); io.raw(reg, sizeof reg);
  io.sync(now); io.sync(frameEnd_); io.sync(timerAt_); io.sync(timerShot_); io.sync(dmaDone_); io.sync(frame);
  for (auto& t : tone_) { io.sync(t.period); io.sync(t.phase); io.sync(t.count); }
  io.sync(noise_.counter); io.sync(noise_.count); io.sync(noise_.lfsr); io.sync(noise_.out);
  io.sync(dma_.on); io.sync(dma_.pos); io.sync(dma_.size); io.sync(dma_.counter);
  io.sync(pendingCycles_); io.sync(sampleAcc_); io.sync(sumL_); io.sync(sumR_); io.sync(sumN_);
  io.sync(hpL_); io.sync(hpR_); io.sync(lastL_); io.sync(lastR_);
  if (io.reading()) {
    // limites: um arquivo danificado não leva a divisão por zero nem a laços sem fim
    for (auto& t : tone_) { clampTo(t.period, 0, 2047 * 32); clampTo(t.phase, 0, std::max(0, t.period - 1)); clampTo(t.count, 0, 256); }
    clampTo(noise_.counter, 0, 256 * 16); clampTo(noise_.count, 0, 256);
    clampTo(dma_.size, 0, 256 * 32); clampTo(dma_.pos, 0, dma_.size); clampTo(dma_.counter, 0, 1024);
    clampTo(pendingCycles_, 0, CYCLES_PER_FRAME); finiteOr(sampleAcc_, 0); clampTo(sampleAcc_, 0.0, 1e6);
    clampTo(sumN_, 0, 1 << 20);
    finiteOr(sumL_, 0); finiteOr(sumR_, 0); finiteOr(hpL_, 0); finiteOr(hpR_, 0); finiteOr(lastL_, 0); finiteOr(lastR_, 0);
    if (frameEnd_ < now || frameEnd_ > now + CYCLES_PER_FRAME) frameEnd_ = now + CYCLES_PER_FRAME;
    if (timerAt_ > now + 256ull * 16384) timerAt_ = now + 256ull * 16384;
    render();
  }
}

std::vector<uint8_t> Supervision::saveState() {
  StateIO io;
  char magic[4];
  std::memcpy(magic, STATE_MAGIC, 4);
  io.raw(magic, 4);
  sync(io);
  return io.data();
}

void Supervision::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  char magic[4];
  io.raw(magic, 4);
  if (std::memcmp(magic, STATE_MAGIC, 4) != 0) throw std::runtime_error("Estado salvo não é do Supervision.");
  const std::vector<uint8_t> backup = saveState();
  try {
    sync(io);
  } catch (...) {
    StateIO back(backup);
    back.raw(magic, 4);
    sync(back);
    throw;
  }
}

// ---------------------------------------------------------------- adaptador
SupervisionCore::SupervisionCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>*)
    : m_(std::make_unique<Supervision>(rom)) {}

}  // namespace indigo::sv
