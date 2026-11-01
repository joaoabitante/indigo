// Sega CD: chip PCM RF5C164 (8 canais, 64 KB de RAM de ondas, 32552 Hz = 50 MHz / 1536) e o som do CD (CD-DA a
// 44,1 kHz com o fader). As duas fontes são geradas no ritmo do sub-CPU e, no fim de cada quadro, reamostradas
// para o mesmo número de amostras que o Mega Drive produziu no quadro e somadas ao som dele.
#include <algorithm>
#include <cstring>

#include "segacd.h"

namespace indigo::segacd {

namespace {
constexpr int PCM_RATIO = 384 * 4;  // 12,5 MHz / 384 = 32552 Hz (em ciclos SCD)
}

// ------------------------------------------------------------ RF5C164
// Pelo comportamento descrito do chip (manual do Mega-CD, seção do PCM): 8 vozes lendo amostras de 8 bits em
// sinal-magnitude (bit 7 = 1 positivo) de uma RAM de 64 KB; o byte 0xFF marca o fim da onda e manda a voz para
// o endereço de laço (LS). Cada voz tem ENV (volume), PAN (nibble de baixo = esquerda, de cima = direita), FD
// (passo do contador de endereço, 5.11), LS e ST (byte alto do endereço inicial). Registrador 7: bit 7 liga o
// som, bit 6 = 1 escolhe a voz (bits 0-2), = 0 escolhe o banco de 4 KB da janela de ondas (bits 0-3).
// Registrador 8: um bit por voz, 0 = ligada; uma voz desligada fica parada no endereço inicial.
void Rf5c164::reset() {
  for (auto& v : voice_) v = Voice{};
  selected_ = waveBank_ = keyOn_ = 0;
  sounding_ = false;
  std::fill(ram.begin(), ram.end(), 0);
}

void Rf5c164::write(uint32_t a, uint8_t v) {
  if (a >= 0x1000) { ram[(static_cast<uint32_t>(waveBank_) << 12) | (a & 0xFFF)] = v; return; }
  Voice& c = voice_[selected_];
  switch (a) {
    case 0x0: c.env = v; break;
    case 0x1: c.pan = v; break;
    case 0x2: c.step = static_cast<uint16_t>((c.step & 0xFF00) | v); break;
    case 0x3: c.step = static_cast<uint16_t>((c.step & 0x00FF) | (v << 8)); break;
    case 0x4: c.loop = static_cast<uint16_t>((c.loop & 0xFF00) | v); break;
    case 0x5: c.loop = static_cast<uint16_t>((c.loop & 0x00FF) | (v << 8)); break;
    case 0x6:
      c.startHi = v;
      if (!(keyOn_ & (1 << selected_))) c.pos = c.start();
      break;
    case 0x7:
      sounding_ = (v & 0x80) != 0;
      if (v & 0x40) selected_ = v & 7;
      else waveBank_ = v & 0x0F;
      break;
    case 0x8:
      keyOn_ = static_cast<uint8_t>(~v);
      for (int i = 0; i < 8; i++)
        if (!(keyOn_ & (1 << i))) voice_[i].pos = voice_[i].start();
      break;
    default: break;
  }
}

uint8_t Rf5c164::read(uint32_t a) const {
  if (a >= 0x1000) return ram[(static_cast<uint32_t>(waveBank_) << 12) | (a & 0xFFF)];
  if (a >= 0x10 && a < 0x20) {  // contador de endereço da voz (a >> 1) & 7: byte baixo, depois o alto
    const uint32_t addr = voice_[(a >> 1) & 7].pos >> 11;
    return static_cast<uint8_t>((a & 1) ? addr >> 8 : addr);
  }
  return 0xFF;
}

void Rf5c164::generate(int32_t& l, int32_t& r) {
  l = r = 0;
  if (!sounding_) return;
  for (int i = 0; i < 8; i++) {
    if (!(keyOn_ & (1 << i))) continue;
    Voice& c = voice_[i];
    int s = ram[(c.pos >> 11) & 0xFFFF];
    if (s == 0xFF) {  // fim da onda: volta ao laço (e toca o byte de lá)
      c.pos = static_cast<uint32_t>(c.loop) << 11;
      s = ram[c.loop];
      if (s == 0xFF) continue;  // laço só com a marca: mudo
    } else {
      c.pos += c.step;
    }
    const int mag = s & 0x7F;
    const int value = (s & 0x80) ? mag : -mag;
    // amplitude: amostra (7 bits) x ENV (8 bits) x PAN (4 bits), escalada para 16 bits
    l += (value * c.env * (c.pan & 0x0F)) >> 5;
    r += (value * c.env * (c.pan >> 4)) >> 5;
  }
  l = std::clamp(l, -32768, 32767);
  r = std::clamp(r, -32768, 32767);
}

void Rf5c164::sync(StateIO& io) {
  for (auto& c : voice_) { io.sync(c.env); io.sync(c.pan); io.sync(c.startHi); io.sync(c.step); io.sync(c.loop); io.sync(c.pos); }
  io.sync(selected_); io.sync(waveBank_); io.sync(keyOn_); io.sync(sounding_);
  io.bytes(ram);
  if (io.reading()) { selected_ &= 7; waveBank_ &= 0x0F; }
}

// ------------------------------------------------------------ PCM no tempo do sub-CPU
void SegaCD::pcmRun(int64_t t) {
  if (t <= pcmClk_) return;
  int64_t n = (t - pcmClk_ + PCM_RATIO - 1) / PCM_RATIO;
  if (n > 65536) { pcmClk_ = t; return; }  // relógio danificado: não gera um mundo de amostras
  pcmClk_ += n * PCM_RATIO;
  const bool keep = pcmBuf_.size() < 200000;
  while (n--) {
    int32_t l, r;
    pcm_.generate(l, r);
    if (keep) { pcmBuf_.push_back(static_cast<int16_t>(l)); pcmBuf_.push_back(static_cast<int16_t>(r)); }
  }
}

void SegaCD::pcmWrite(uint32_t a, uint8_t v) { pcmRun(now()); pcm_.write(a, v); }
uint8_t SegaCD::pcmRead(uint32_t a) { pcmRun(now()); return pcm_.read(a); }

// ------------------------------------------------------------ fim do quadro
void SegaCD::endFrame() {
  const int64_t t = mainNow();
  advance(t);
  pcmRun(t);
}

namespace {
// reamostragem de m amostras estéreo para n, somando em dl/dr: cada amostra de entrada vale um degrau (como no
// chip) e cada saída é a média da entrada no seu intervalo (filtro de caixa)
void resampleAdd(const int16_t* src, size_t m, int16_t* prev, float* dl, float* dr, size_t n) {
  if (!n) return;
  if (!m) {
    for (size_t i = 0; i < n; i++) { dl[i] += prev[0] / 32768.0f; dr[i] += prev[1] / 32768.0f; }
    return;
  }
  const double step = static_cast<double>(m) / static_cast<double>(n);
  const double inv = 1.0 / (step * 32768.0);
  double t0 = 0;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const double t1 = (i + 1 == n) ? static_cast<double>(m) : static_cast<double>(i + 1) * step;
    double al = 0, ar = 0, t = t0;
    while (t < t1 - 1e-9) {
      const double e = std::min(t1, static_cast<double>(k + 1));
      const double w = e - t;
      al += w * src[k * 2]; ar += w * src[k * 2 + 1];
      t = e;
      if (t >= static_cast<double>(k + 1) - 1e-9 && k + 1 < m) k++;
      else if (t >= static_cast<double>(k + 1) - 1e-9) break;
    }
    dl[i] += static_cast<float>(al * inv);
    dr[i] += static_cast<float>(ar * inv);
    t0 = t1;
  }
  prev[0] = src[(m - 1) * 2]; prev[1] = src[(m - 1) * 2 + 1];
}
}  // namespace

void SegaCD::produceAudio(size_t n) {
  // CD-DA: 44100 amostras por segundo no tempo do CD
  const int64_t t = mainNow();
  const int64_t dt = t - frameScd_;
  frameScd_ = t;
  double want = cddaFrac_ + (dt > 0 ? static_cast<double>(dt) * 44100.0 / 50000000.0 : 0.0);
  if (want > 8192) want = 8192;
  const int nc = static_cast<int>(want);
  cddaFrac_ = want - nc;
  std::vector<int16_t> cd;
  drive_.readAudio(nc, cd);
  // descarta o que já foi lido e guarda o quadro novo
  if (audPos_ > 0) {
    audL_.erase(audL_.begin(), audL_.begin() + static_cast<long>(audPos_));
    audR_.erase(audR_.begin(), audR_.begin() + static_cast<long>(audPos_));
    audPos_ = 0;
  }
  const size_t o = audL_.size();
  audL_.resize(o + n, 0.0f);
  audR_.resize(o + n, 0.0f);
  resampleAdd(pcmBuf_.data(), pcmBuf_.size() / 2, pcmLast_, audL_.data() + o, audR_.data() + o, n);
  resampleAdd(cd.data(), cd.size() / 2, cddaLast_, audL_.data() + o, audR_.data() + o, n);
  pcmBuf_.clear();
  // saída acoplada por capacitor: passa-altas de 1ª ordem (~14 Hz)
  for (size_t i = o; i < o + n; i++) {
    float* v[2] = {&audL_[i], &audR_[i]};
    for (int c = 0; c < 2; c++) {
      const float x = *v[c];
      hpY_[c] = hpY_[c] * (1.0f - 1.0f / 512.0f) + (x - hpX_[c]);
      hpX_[c] = x;
      *v[c] = hpY_[c];
    }
  }
  if (audL_.size() > 16384) {  // ninguém está lendo o som: guarda só o fim (como o Mega Drive)
    const size_t drop = audL_.size() - 16384;
    audL_.erase(audL_.begin(), audL_.begin() + static_cast<long>(drop));
    audR_.erase(audR_.begin(), audR_.begin() + static_cast<long>(drop));
  }
}

void SegaCD::mixAudio(float* l, float* r, size_t n) {
  const size_t have = audL_.size() - audPos_;
  const size_t k = std::min(n, have);
  for (size_t i = 0; i < k; i++) {
    l[i] = std::clamp(l[i] + audL_[audPos_ + i], -1.0f, 1.0f);
    r[i] = std::clamp(r[i] + audR_[audPos_ + i], -1.0f, 1.0f);
  }
  audPos_ += k;
}

void SegaCD::dropAudio(size_t n) { audPos_ = std::min(audL_.size(), audPos_ + n); }

}  // namespace indigo::segacd
