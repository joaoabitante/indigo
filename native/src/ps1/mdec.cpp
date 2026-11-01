// MDEC do PlayStation 1 (veja mdec.h).
#include "mdec.h"

#include <algorithm>
#include <cstring>

namespace indigo::ps1 {

namespace {

// ordem zigue-zague: posição k da sequência -> índice linha*8+coluna do bloco (linha = frequência vertical)
struct ZigZag {
  uint8_t pos[64];
  ZigZag() {
    int k = 0;
    for (int s = 0; s < 15; s++) {
      const int lo = std::max(0, s - 7), hi = std::min(s, 7);
      if (s & 1) {
        for (int v = lo; v <= hi; v++) pos[k++] = uint8_t(v * 8 + (s - v));
      } else {
        for (int v = hi; v >= lo; v--) pos[k++] = uint8_t(v * 8 + (s - v));
      }
    }
  }
};
const ZigZag ZZ;

inline int sext10(int v) { return (v & 0x200) ? (v | ~0x3FF) : (v & 0x3FF); }

// o chip guarda os resultados em 9 bits com sinal e depois satura para 8 bits
inline int clamp9to8(int v) {
  v = (v & 0x100) ? (v | ~0x1FF) : (v & 0x1FF);
  return v < -128 ? -128 : (v > 127 ? 127 : v);
}

inline int clampCoef(int v) { return v < -0x4000 ? -0x4000 : (v > 0x3FFF ? 0x3FFF : v); }

}  // namespace

void Mdec::reset() {
  std::memset(in_, 0, sizeof(in_));
  inRd_ = inCount_ = 0;
  started_ = stalled_ = false;
  wLeft_ = 0;
  wStatus_ = 0xFFFF;
  cmd_ = control_ = 0;
  remaining_ = 0;
  inCommand_ = false;
  tableIndex_ = 0;
  busy_ = 0;
  std::memset(quant_, 0, sizeof(quant_));
  std::memset(scale_, 0, sizeof(scale_));
  std::memset(coef_, 0, sizeof(coef_));
  coefIndex_ = qscale_ = block_ = 0;
  std::memset(cr_, 0, sizeof(cr_));
  std::memset(cb_, 0, sizeof(cb_));
  std::memset(y_, 0, sizeof(y_));
  stage_ = -1;
  stageOut_ = false;
  stageWords_ = 0;
  std::memset(stageBuf_, 0, sizeof(stageBuf_));
  std::memset(stageOff_, 0, sizeof(stageOff_));
  std::memset(out_, 0, sizeof(out_));
  std::memset(outOff_, 0, sizeof(outOff_));
  outRd_ = outCount_ = 0;
}

int Mdec::blockWords() const {
  switch (depth()) {
    case 0: return 8;    // 8x8 de 4 bits
    case 1: return 16;   // 8x8 de 8 bits
    case 2: return 48;   // 8x8 de 24 bits
    default: return 32;  // 8x8 de 15 bits
  }
}

uint32_t Mdec::status() const {
  uint32_t s = 0;
  const bool f = full();
  if (!outCount_) s |= 1u << 31;
  if (f) s |= 1u << 30;
  if (wLeft_ || inCount_ || inCommand_ || stage_ >= 0 || outCount_) s |= 1u << 29;
  const bool inReq = (control_ & (1u << 30)) && !f && wLeft_;
  if (inReq) s |= 1u << 28;
  // o pedido de saída cai depois das primeiras palavras do último bloco que há (a menos que venha outro, ou
  // que não haja mais entrada) e na última palavra
  const int b = blockWords();
  if ((control_ & (1u << 29)) && outCount_ > 1 && (outCount_ >= b * 13 / 16 || stage_ >= 0 || !inReq)) s |= 1u << 27;
  s |= ((cmd_ >> 25) & 0xF) << 23;
  uint32_t blk = 4;  // mono: sempre 4
  if (stage_ >= 0) blk = uint32_t(stage_);
  else if (decoding() && (depth() & 2)) blk = block_ < 2 ? 4u + block_ : uint32_t(block_ - 2);
  s |= blk << 16;
  s |= wStatus_;
  return s;
}

// como o bit 28 do status (FIFO não cheia), mas com a decodificação andando o DMA0 espera a FIFO esvaziar: um
// bloco de 32 palavras não pode chegar com palavras ainda na fila (os blocos menores enchem a FIFO aos poucos)
bool Mdec::dmaInRequest() const { return (control_ & (1u << 30)) && wLeft_ && !full() && (!inCount_ || !started_); }

// o DMA1 lê um bloco de 32 palavras por pedido: há um inteiro na saída, ou o estágio completa o que falta
bool Mdec::dmaOutRequest() const {
  return (control_ & (1u << 29)) && outCount_ && (outCount_ >= 32 || stage_ >= 0 || !wLeft_);
}

void Mdec::pop() {
  outRd_ = uint8_t((outRd_ + 1) % OUT_MAX);
  outCount_--;
  process();
}

uint32_t Mdec::read(uint32_t offset, int size) {
  (void)size;
  if (offset & 4) return status();
  // porta de dados: na ordem do chip (blocos de 8x8)
  if (!outCount_) return 0;
  const uint32_t v = out_[outRd_];
  pop();
  return v;
}

uint32_t Mdec::dmaRead(int32_t& off) {
  off = 0;
  if (!outCount_) return 0;
  const uint32_t v = out_[outRd_];
  off = outOff_[outRd_];
  pop();
  return v;
}

void Mdec::write(uint32_t offset, uint32_t value, int size) {
  (void)size;
  if (offset & 4) {
    if (value & 0x80000000u) {
      // reinício: aborta o comando e esvazia as FIFOs (as tabelas ficam); status 80040000h
      inRd_ = inCount_ = 0;
      started_ = stalled_ = false;
      wLeft_ = 0;
      wStatus_ = 0;
      cmd_ = 0;
      remaining_ = 0;
      inCommand_ = false;
      tableIndex_ = 0;
      busy_ = 0;
      std::memset(coef_, 0, sizeof(coef_));
      coefIndex_ = qscale_ = block_ = 0;
      stage_ = -1;
      stageOut_ = false;
      stageWords_ = 0;
      outRd_ = outCount_ = 0;
    }
    control_ = value & 0x7FFFFFFFu;
    process();
    return;
  }
  dmaWrite(value);
}

void Mdec::dmaWrite(uint32_t v) {
  // quem escreve conta as palavras do comando (o campo 15-0 do status anda com a escrita)
  if (!wLeft_) {
    const uint32_t type = v >> 29;
    wLeft_ = type == 1 ? ((v & 0xFFFF) ? (v & 0xFFFF) : 0x10000) : type == 2 ? ((v & 1) ? 32 : 16) : type == 3 ? 32 : 0;
    wStatus_ = wLeft_ ? uint16_t(wLeft_ - 1) : uint16_t(v & 0xFFFF);
  } else {
    wLeft_--;
    wStatus_ = uint16_t(wLeft_ - 1);
  }
  if (inCount_ >= IN_FIFO) return;  // FIFO cheia: a palavra se perde, como no chip
  in_[(inRd_ + inCount_) % IN_FIFO] = v;
  inCount_++;
  process();
}

void Mdec::run(int cycles) {
  // cada bloco pronto ocupa o chip BLOCK_CYCLES; no tempo que sobra ele segue para os próximos
  for (int guard = 0; guard < 4096; guard++) {
    if (busy_ > 0) {
      const int d = std::min(busy_, cycles);
      busy_ -= d;
      cycles -= d;
      if (busy_ > 0) return;
    }
    process();
    if (busy_ <= 0 || cycles <= 0) return;
  }
}

void Mdec::startCommand(uint32_t cmd) {
  cmd_ = cmd;
  started_ = false;
  switch (cmd >> 29) {
    case 1:
      remaining_ = (cmd & 0xFFFF) ? (cmd & 0xFFFF) : 0x10000;
      inCommand_ = true;
      coefIndex_ = 0;
      block_ = (depth() & 2) ? 0 : 2;
      break;
    case 2:
      remaining_ = (cmd & 1) ? 32 : 16;
      inCommand_ = true;
      tableIndex_ = 0;
      break;
    case 3:
      remaining_ = 32;
      inCommand_ = true;
      tableIndex_ = 0;
      break;
    default:  // sem função: só reflete os bits
      remaining_ = 0;
      inCommand_ = false;
      break;
  }
}

void Mdec::process() {
  stalled_ = false;
  for (;;) {
    // estágio -> saída: um bloco de pixels entra quando a saída baixa de meio bloco; Cr/Cb liberam o estágio
    // quando ela baixa de 1 bloco e 1/4 (medido em 15 bits; os outros formatos seguem a mesma proporção)
    if (stage_ >= 0) {
      const int b = stageOut_ ? stageWords_ : blockWords(), half = b / 2 + 1;
      if (stageOut_ && outCount_ <= half && outCount_ + stageWords_ <= OUT_MAX) {
        for (int i = 0; i < stageWords_; i++) {
          const int p = (outRd_ + outCount_) % OUT_MAX;
          out_[p] = stageBuf_[i];
          outOff_[p] = stageOff_[i];
          outCount_++;
        }
        stage_ = -1;
      } else if (!stageOut_ && outCount_ <= half + b * 3 / 4) {
        stage_ = -1;
      }
    }
    if (!inCommand_) {
      if (!inCount_) return;
      const uint32_t c = in_[inRd_];
      inRd_ = (inRd_ + 1) % IN_FIFO;
      inCount_--;
      startCommand(c);
      continue;
    }
    const uint32_t type = cmd_ >> 29;
    if (!inCount_) {
      started_ = false;  // esvaziou: espera encher de novo
      return;
    }
    if (busy_ > 0) return;
    if (type == 1) {
      if (!started_) {
        if (inCount_ < IN_FIFO && wLeft_) return;
        started_ = true;
      }
      // o estágio ocupado segura a palavra que terminaria o bloco seguinte
      if (stage_ >= 0 && endsBlock(in_[inRd_])) { stalled_ = true; return; }
    }
    uint32_t w = in_[inRd_];
    inRd_ = (inRd_ + 1) % IN_FIFO;
    inCount_--;
    if (type == 1) {
      decodeHalf(uint16_t(w));
      decodeHalf(uint16_t(w >> 16));
    } else if (type == 2) {
      for (int i = 0; i < 4; i++, w >>= 8) {
        quant_[(tableIndex_ >> 6) & 1][tableIndex_ & 63] = uint8_t(w);
        tableIndex_ = (tableIndex_ + 1) & 0x7F;
      }
    } else {
      for (int i = 0; i < 2; i++, w >>= 16) {
        scale_[tableIndex_ & 63] = int16_t(int16_t(uint16_t(w)) >> 3);  // o chip usa só os 13 bits de cima
        tableIndex_ = (tableIndex_ + 1) & 63;
      }
    }
    if (--remaining_ == 0) inCommand_ = false;
  }
}

// a mesma conta do decodeHalf, sem mudar nada: algum dos dois meios da palavra fecha o bloco atual?
bool Mdec::endsBlock(uint32_t w) const {
  int k = coefIndex_;
  for (int i = 0; i < 2; i++, w >>= 16) {
    const uint16_t h = uint16_t(w);
    if (k == 0) {
      if (h != 0xFE00) k = 1;
      continue;
    }
    if (h == 0xFE00) return true;
    k = std::min(64, k + (h >> 10));
    if (k < 64) k++;
    if (k >= 64) return true;
  }
  return false;
}

bool Mdec::decodeHalf(uint16_t h) {
  const uint8_t* qt = quant_[block_ < 2 ? 1 : 0];
  if (coefIndex_ == 0) {
    if (h == 0xFE00) return false;  // enchimento antes do bloco
    qscale_ = uint8_t(h >> 10);
    std::memset(coef_, 0, sizeof(coef_));
    // o primeiro coeficiente (DC) usa só a tabela, sem a escala do bloco
    const int ci = sext10(h & 0x3FF), q = qt[0];
    int v = q ? ci * q * 16 + (ci ? (ci < 0 ? 8 : -8) : 0) : ci * 32;
    coef_[ZZ.pos[0]] = int16_t(clampCoef(v));
    coefIndex_ = 1;
  } else if (h == 0xFE00) {
    coefIndex_ = 64;  // fim do bloco: o resto fica em zero
  } else {
    coefIndex_ = uint8_t(std::min(64, coefIndex_ + (h >> 10)));  // pula zeros
    if (coefIndex_ < 64) {
      const int ci = sext10(h & 0x3FF), q = qscale_ * qt[coefIndex_];
      int v = q ? ((ci * q) >> 3) * 16 + (ci ? (ci < 0 ? 8 : -8) : 0) : ci * 32;
      coef_[ZZ.pos[coefIndex_]] = int16_t(clampCoef(v));
      coefIndex_++;
    }
  }
  if (coefIndex_ < 64) return false;
  coefIndex_ = 0;
  finishBlock();
  return true;
}

// IDCT do chip: primeiro a passada vertical, guardada em 16 bits, depois a horizontal, cada soma arredondada
// em 15 bits; a tabela de escala tem a frequência na linha e a amostra na coluna
void Mdec::idct(const int16_t* c, int8_t* out) const {
  int16_t t[64];
  for (int y = 0; y < 8; y++)
    for (int u = 0; u < 8; u++) {
      int32_t s = 0;
      for (int v = 0; v < 8; v++) s += c[v * 8 + u] * scale_[v * 8 + y];
      t[y * 8 + u] = int16_t((s + 0x4000) >> 15);
    }
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      int32_t s = 0;
      for (int u = 0; u < 8; u++) s += t[y * 8 + u] * scale_[u * 8 + x];
      out[y * 8 + x] = int8_t(clamp9to8((s + 0x4000) >> 15));
    }
}

void Mdec::finishBlock() {
  busy_ += BLOCK_CYCLES;
  const bool color = depth() & 2;
  if (!color) {
    idct(coef_, y_[0]);
    emitMono();
    stage_ = 4;
    return;
  }
  switch (block_) {
    case 0: idct(coef_, cr_); stage_ = 4; stageOut_ = false; break;
    case 1: idct(coef_, cb_); stage_ = 5; stageOut_ = false; break;
    default:
      idct(coef_, y_[block_ - 2]);
      emitColor(block_ - 2);  // cada bloco Y sai assim que fica pronto, como no chip
      stage_ = int8_t(block_ - 2);
      break;
  }
  block_ = block_ == 5 ? 0 : uint8_t(block_ + 1);
}

void Mdec::emitMono() {
  uint32_t* o = stageBuf_;
  stageOut_ = true;
  std::memset(stageOff_, 0, sizeof(stageOff_));
  const bool sgn = cmd_ & (1u << 26);
  const int8_t* y = y_[0];
  if (depth() == 0) {
    const uint8_t x = sgn ? 0x00 : 0x88;
    uint8_t b[32];
    for (int i = 0; i < 32; i++) {
      const int a = std::min(127, y[2 * i] + 8), c = std::min(127, y[2 * i + 1] + 8);
      b[i] = uint8_t(((uint8_t(a) >> 4) | (uint8_t(c) & 0xF0)) ^ x);
    }
    for (int i = 0; i < 8; i++) o[i] = b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | uint32_t(b[4 * i + 3]) << 24;
    stageWords_ = 8;
  } else {
    const uint8_t x = sgn ? 0x00 : 0x80;
    for (int i = 0; i < 16; i++)
      o[i] = uint8_t(y[4 * i] ^ x) | uint8_t(y[4 * i + 1] ^ x) << 8 | uint8_t(y[4 * i + 2] ^ x) << 16 |
             uint32_t(uint8_t(y[4 * i + 3] ^ x)) << 24;
    stageWords_ = 16;
  }
}

void Mdec::emitColor(int blk) {
  uint32_t* o = stageBuf_;
  const bool sgn = cmd_ & (1u << 26);
  const bool d24 = depth() == 2;
  uint8_t bytes[192];
  size_t nb = 0;
  {
    const int8_t* y = y_[blk];
    const int cy = (blk >> 1) * 4, cx = (blk & 1) * 4;
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++) {
        const int ci = (cy + (r >> 1)) * 8 + cx + (c >> 1);
        const int Y = y[r * 8 + c], cr = cr_[ci], cb = cb_[ci];
        const int R = clamp9to8(Y + ((359 * cr + 0x80) >> 8)) ^ 0x80;
        const int G = clamp9to8(Y + ((((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 0x80) >> 8)) ^ 0x80;
        const int B = clamp9to8(Y + ((454 * cb + 0x80) >> 8)) ^ 0x80;
        if (d24) {
          const uint8_t x = sgn ? 0x80 : 0x00;
          bytes[nb++] = uint8_t((R & 0xFF) ^ x);
          bytes[nb++] = uint8_t((G & 0xFF) ^ x);
          bytes[nb++] = uint8_t((B & 0xFF) ^ x);
        } else {
          const int r5 = std::min(31, ((R & 0xFF) + 4) >> 3), g5 = std::min(31, ((G & 0xFF) + 4) >> 3),
                    b5 = std::min(31, ((B & 0xFF) + 4) >> 3);
          uint16_t p = uint16_t(r5 | g5 << 5 | b5 << 10);
          if (cmd_ & (1u << 25)) p ^= 0x8000;
          if (sgn) p ^= 0x4210;
          bytes[nb++] = uint8_t(p);
          bytes[nb++] = uint8_t(p >> 8);
        }
      }
  }
  for (size_t i = 0; i < nb / 4; i++)
    o[i] = bytes[4 * i] | bytes[4 * i + 1] << 8 | bytes[4 * i + 2] << 16 | uint32_t(bytes[4 * i + 3]) << 24;
  stageWords_ = uint8_t(nb / 4);
  stageOut_ = true;
  // o DMA1 grava o macrobloco em linhas de 16 pixels: deslocamento de cada palavra do bloco dentro dele
  const int rowW = d24 ? 6 : 4;
  for (int j = 0; j < stageWords_; j++) {
    const int r = (j / rowW + 8 * (blk >> 1)) * 2 * rowW + (blk & 1) * rowW + j % rowW;
    stageOff_[j] = int16_t(r - (blk * stageWords_ + j));
  }
}

void Mdec::sync(StateIO& io) {
  io.raw(in_, sizeof(in_));
  io.sync(inRd_);
  io.sync(inCount_);
  io.sync(started_);
  io.sync(stalled_);
  io.sync(wLeft_);
  io.sync(wStatus_);
  io.sync(cmd_);
  io.sync(control_);
  io.sync(remaining_);
  io.sync(inCommand_);
  io.sync(tableIndex_);
  io.sync(busy_);
  io.raw(quant_, sizeof(quant_));
  io.raw(scale_, sizeof(scale_));
  io.raw(coef_, sizeof(coef_));
  io.sync(coefIndex_);
  io.sync(qscale_);
  io.sync(block_);
  io.raw(cr_, sizeof(cr_));
  io.raw(cb_, sizeof(cb_));
  io.raw(y_, sizeof(y_));
  io.sync(stage_);
  io.sync(stageOut_);
  io.sync(stageWords_);
  io.raw(stageBuf_, sizeof(stageBuf_));
  io.raw(stageOff_, sizeof(stageOff_));
  io.raw(out_, sizeof(out_));
  io.raw(outOff_, sizeof(outOff_));
  io.sync(outRd_);
  io.sync(outCount_);
  if (io.reading()) {
    inRd_ %= IN_FIFO;
    clampTo<uint8_t>(inCount_, 0, IN_FIFO);
    clampTo<uint32_t>(wLeft_, 0, 0x10000);
    clampTo<uint32_t>(remaining_, 0, 0x10000);
    if (!remaining_) inCommand_ = false;
    tableIndex_ &= 0x7F;
    clampTo<uint8_t>(coefIndex_, 0, 63);
    clampTo<uint8_t>(block_, 0, 5);
    clampTo<int8_t>(stage_, -1, 5);
    clampTo<uint8_t>(stageWords_, 0, 48);
    outRd_ %= OUT_MAX;
    clampTo<uint8_t>(outCount_, 0, OUT_MAX);
    if (busy_ < 0) busy_ = 0;
    // a tabela de escala guarda 13 bits e os coeficientes 15: fora disso as somas da IDCT estourariam
    for (auto& v : scale_) clampTo<int16_t>(v, -4096, 4095);
    for (auto& v : coef_) clampTo<int16_t>(v, -0x4000, 0x3FFF);
  }
}

}  // namespace indigo::ps1
