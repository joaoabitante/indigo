#include "dma.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "system.h"

namespace indigo::ps1 {

namespace {
constexpr uint32_t ADDR_MASK = 0x1FFFFC;
inline uint32_t ramRead(System& s, uint32_t a) { uint32_t v; std::memcpy(&v, s.ram + (a & ADDR_MASK), 4); return v; }
inline void ramWrite(System& s, uint32_t a, uint32_t v) { std::memcpy(s.ram + (a & ADDR_MASK), &v, 4); }
// a DMA lê e grava a RAM no modo "hyper page": 1 ciclo por palavra mais 1 para abrir cada linha de 16 palavras
inline uint64_t burst(uint64_t words) { return words + (words + 15) / 16; }
// tempos medidos no console pelo ps1-tests (dma/chopping): partida, intervalo entre blocos da GPU (SyncMode 1) e
// troca de dono do barramento entre uma janela do chopping e a da CPU
constexpr uint32_t START_COST = 8, BLOCK_GAP = 10, CHOP_GAP = 5;
}  // namespace

void Dma::reset() {
  std::memset(ch_, 0, sizeof ch_);
  dpcr_ = 0x07654321;
  dicr_ = 0;
  last_ = sys_.now;
  listActive_ = false;
  waiting_ = 0;
  std::memset(readyAt_, 0, sizeof readyAt_);
  listEndAt_ = 0;
  chopping_ = 0;
  std::memset(chopAt_, 0, sizeof chopAt_);
}

uint32_t Dma::read(uint32_t off) {
  if (off == 0xF0) return dpcr_;
  if (off == 0xF4) return dicr_;
  const int c = int((off - 0x80) >> 4) & 7;
  if (c > 6) return 0;
  switch (off & 0xC) {
    case 0x0: return ch_[c].madr;
    case 0x4: return ch_[c].bcr;
    case 0x8: return ch_[c].chcr;
    default: return 0;
  }
}

void Dma::write(uint32_t off, uint32_t v) {
  if (off == 0xF0) { dpcr_ = v; return; }
  if (off == 0xF4) {
    // bits 24..30: escrever 1 apaga a marca; 0..5 e 15..23 são de leitura e escrita
    dicr_ = (dicr_ & 0xFF000000u & ~(v & 0x7F000000u)) | (v & 0x00FF803Fu);
    updateIrq();
    return;
  }
  const int c = int((off - 0x80) >> 4) & 7;
  if (c > 6) return;
  Ch& ch = ch_[c];
  switch (off & 0xC) {
    case 0x0: ch.madr = v & 0xFFFFFF; break;
    case 0x4: ch.bcr = v; break;
    case 0x8:
      if (c == 6) ch.chcr = (v & 0x51000000u) | 0x2;  // OTC: só início/gatilho; sempre para trás
      else ch.chcr = v & 0x71770703u;
      start(c);
      break;
    default: break;
  }
}

void Dma::updateIrq() {
  const bool was = (dicr_ & 0x80000000u) != 0;
  const bool force = (dicr_ >> 15) & 1;
  const bool master = (dicr_ >> 23) & 1;
  const bool any = ((dicr_ >> 16) & (dicr_ >> 24) & 0x7F) != 0;
  const bool now = force || (master && any);
  if (now) dicr_ |= 0x80000000u; else dicr_ &= 0x7FFFFFFFu;
  if (now && !was) sys_.raiseIrq(IRQ_DMA);
}

void Dma::finish(int c) {
  ch_[c].chcr &= ~0x11000000u;
  if (dicr_ & (1u << (16 + c))) dicr_ |= 1u << (24 + c);
  updateIrq();
}

// pedido de DMA do aparelho do canal (DRQ); a GPU, a OTC e a PIO estão sempre prontas
bool Dma::request(int c) const {
  System& s = sys_;
  if (s.now < readyAt_[c]) return false;
  switch (c) {
    case 0: return s.mdec.dmaInRequest();
    case 1: return s.mdec.dmaOutRequest();
    case 4: return s.spu.dmaRequest();
    default: return true;
  }
}

// passa 'words' palavras a partir de a (no sentido do canal); devolve o endereço seguinte
uint32_t Dma::transfer(int c, uint32_t a, uint32_t words) {
  System& s = sys_;
  Ch& ch = ch_[c];
  const bool fromRam = ch.chcr & 1;
  const uint32_t step = (ch.chcr & 2) ? uint32_t(-4) : 4u;
  if (fromRam) {
    for (uint32_t i = 0; i < words; i++, a += step) {
      const uint32_t w = ramRead(s, a);
      switch (c) {
        case 0: s.mdec.dmaWrite(w); break;
        case 2: s.gpu.gp0(w); break;
        case 4: s.spu.dmaWrite(w); break;
        default: break;
      }
    }
  } else {
    for (uint32_t i = 0; i < words; i++, a += step) {
      uint32_t w;
      switch (c) {
        case 1: {
          // o MDEC diz onde cada palavra fica dentro do macrobloco (blocos de 8x8 gravados em linhas de 16 pixels)
          int32_t off;
          w = s.mdec.dmaRead(off);
          ramWrite(s, a + uint32_t(off) * step, w);
          continue;
        }
        case 2: w = s.gpu.readData(); break;
        case 3: w = s.cdrom.dmaRead(); break;
        case 4: w = s.spu.dmaRead(); break;
        default: w = 0xFFFFFFFF; break;
      }
      ramWrite(s, a, w);
    }
  }
  return a;
}

void Dma::start(int c) {
  Ch& ch = ch_[c];
  const uint32_t sync = (ch.chcr >> 9) & 3;
  waiting_ &= uint8_t(~(1u << c));
  chopping_ &= uint8_t(~(1u << c));
  if (!(ch.chcr & 0x01000000u)) return;
  if (!enabled(c)) return;
  System& s = sys_;
  const bool fromRam = ch.chcr & 1;
  const bool drq = c <= 4;  // canais com pedido do aparelho (MDEC, GPU, CD, SPU)
  if (sync == 0 && !(ch.chcr & 0x10000000u)) {
    // bloco único sem o gatilho manual: os canais com pedido começam quando o aparelho pede
    if (!drq) return;
    s.syncPeriph();
    if (!request(c)) { waiting_ |= uint8_t(1u << c); return; }
  }

  if (sync == 2) {
    if (c == 2 && fromRam) { listActive_ = true; listEndAt_ = 0; last_ = s.now; runList(0x8000, true); return; }
    finish(c);
    return;
  }

  if (sync == 1 && c != 2) {  // blocos sob pedido: um bloco por pedido do aparelho
    s.syncPeriph();
    pump(c);
    return;
  }

  if (sync == 0 && (ch.chcr & 0x100) && c != 6) {
    // bloco único com "chopping": janelas de 2^N palavras da DMA alternadas com 2^M ciclos da CPU
    if (drq) s.syncPeriph();
    s.now += START_COST;
    chopping_ |= uint8_t(1u << c);
    chop(c);
    return;
  }

  uint32_t words;
  if (sync == 0) words = (ch.bcr & 0xFFFF) ? (ch.bcr & 0xFFFF) : 0x10000;
  else words = (ch.bcr & 0xFFFF) * (ch.bcr >> 16);
  if (sync == 1 && (ch.bcr & 0xFFFF) == 0) words = 0x10000 * (ch.bcr >> 16);
  if (words > 0x100000) words = 0x100000;  // mais que 4 MB não cabe na RAM nem acaba a tempo
  const uint32_t blocks = sync == 1 ? ((ch.bcr >> 16) ? (ch.bcr >> 16) : 0x10000) : 0;
  uint32_t a = ch.madr;
  if (c == 6) {
    // limpa a tabela de ordenação: cada entrada aponta para a anterior, a última é o fim da lista
    for (uint32_t i = 0; i < words; i++) {
      ramWrite(s, a, i == words - 1 ? 0x00FFFFFF : ((a - 4) & 0x1FFFFF));
      a -= 4;
    }
  } else {
    if (drq && c != 2) s.syncPeriph();
    a = transfer(c, a, words);
  }
  if (sync == 1) { ch.madr = a & 0xFFFFFF; ch.bcr &= 0xFFFF; }
  // a CPU fica parada enquanto a DMA usa o barramento: 1 ciclo por palavra e a partida; nos blocos da GPU,
  // mais o intervalo entre um bloco e o pedido seguinte
  s.now += (c == 2 ? burst(words) : words) + START_COST;
  if (sync == 1) {
    // blocos maiores que a fila de 16 palavras da GPU: a DMA espera a GPU esvaziar a fila no meio do bloco
    const uint32_t bs = ch_[c].bcr & 0xFFFF ? ch_[c].bcr & 0xFFFF : 0x10000;
    s.now += uint64_t(BLOCK_GAP + (bs > 16 ? (bs - 16) / 16 + 1 : 0)) * blocks;
  }
  finish(c);
}

// uma janela do "chopping": a DMA passa até 2^N palavras com a CPU parada; depois a CPU anda 2^M ciclos
void Dma::chop(int c) {
  System& s = sys_;
  Ch& ch = ch_[c];
  const uint32_t left = (ch.bcr & 0xFFFF) ? (ch.bcr & 0xFFFF) : 0x10000;
  const uint32_t n = std::min(left, 1u << ((ch.chcr >> 16) & 7));
  ch.madr = transfer(c, ch.madr, n) & 0xFFFFFF;
  ch.bcr = (ch.bcr & 0xFFFF0000u) | ((left - n) & 0xFFFF);
  s.now += burst(n) + CHOP_GAP;
  if (left == n) {
    chopping_ &= uint8_t(~(1u << c));
    finish(c);
  } else {
    chopAt_[c] = s.now + (1u << ((ch.chcr >> 20) & 7));
  }
}

// blocos sob pedido: passa os blocos enquanto o aparelho pede; o resto fica para o Dma::sync()
void Dma::pump(int c) {
  System& s = sys_;
  Ch& ch = ch_[c];
  const uint32_t bs = (ch.bcr & 0xFFFF) ? (ch.bcr & 0xFFFF) : 0x10000;
  int guard = 0;
  while ((ch.bcr >> 16) && request(c) && guard++ < 4096) {
    const uint32_t a = transfer(c, ch.madr, bs);
    ch.madr = a & 0xFFFFFF;
    ch.bcr -= 0x10000;
    s.now += bs + 1;
    // a SPU grava (ou lê) a RAM de som a ~1 meia palavra a cada 5 ciclos: entre um bloco e outro a CPU anda
    if (c == 4) readyAt_[c] = s.now + uint64_t(bs) * 10;
    if (c <= 1) s.syncPeriph();  // o MDEC precisa do tempo do bloco para pedir o seguinte
  }
  if (!(ch.bcr >> 16)) {
    waiting_ &= uint8_t(~(1u << c));
    finish(c);
  } else {
    waiting_ |= uint8_t(1u << c);
  }
}

void Dma::runList(uint32_t budget, bool eager) {
  System& s = sys_;
  Ch& ch = ch_[2];
  uint32_t a = ch.madr & ADDR_MASK;
  uint64_t words = 0;
  while (budget) {
    const uint32_t hdr = ramRead(s, a);
    const uint32_t n = hdr >> 24;
    for (uint32_t i = 0; i < n; i++) s.gpu.gp0(ramRead(s, a + 4 + i * 4));
    const uint32_t cost = n + 1;
    budget = budget > cost ? budget - cost : 0;
    if (eager) words += cost; else s.now += cost;
    if (hdr & 0x800000) {
      ch.madr = 0x00FFFFFF;
      listActive_ = false;
      // a última palavra entra na fila da GPU perto do fim do desenho
      const uint64_t end = std::max(s.now + words, s.gpu.busyUntil());
      if (end > s.now) listEndAt_ = end;
      else finish(2);
      return;
    }
    a = hdr & ADDR_MASK;
    ch.madr = hdr & 0xFFFFFF;
  }
  if (eager) s.now += words;  // lista longa demais (ou sem fim): o que já passou ocupou o barramento
}

void Dma::sync() {
  const uint64_t el = sys_.now - last_;
  last_ = sys_.now;
  if (waiting_) {
    for (int c = 0; c < 7; c++) {
      if (!((waiting_ >> c) & 1)) continue;
      if (!(ch_[c].chcr & 0x01000000u) || !enabled(c)) { waiting_ &= uint8_t(~(1u << c)); continue; }  // parado pelo programa
      if (!request(c)) continue;
      if (((ch_[c].chcr >> 9) & 3) == 0) { waiting_ &= uint8_t(~(1u << c)); ch_[c].chcr |= 0x10000000u; start(c); }
      else pump(c);
    }
    last_ = sys_.now;
  }
  if (chopping_) {
    for (int c = 0; c < 7; c++) {
      if (!((chopping_ >> c) & 1)) continue;
      if (!(ch_[c].chcr & 0x01000000u) || !enabled(c)) { chopping_ &= uint8_t(~(1u << c)); continue; }  // parado pelo programa
      if (sys_.now >= chopAt_[c]) chop(c);
    }
    last_ = sys_.now;
  }
  if (listEndAt_) {
    if (!(ch_[2].chcr & 0x01000000u)) listEndAt_ = 0;  // parada pelo programa
    else if (sys_.now >= listEndAt_) { listEndAt_ = 0; finish(2); }
  }
  if (!listActive_) return;
  if (!(ch_[2].chcr & 0x01000000u)) { listActive_ = false; return; }  // parada pelo programa
  // a lista e a CPU dividem o barramento: a DMA anda no máximo o tempo que a CPU andou
  runList(uint32_t(el > 0x10000 ? 0x10000 : el + 1), false);
  last_ = sys_.now;
}

uint64_t Dma::nextEvent() const {
  uint64_t t = listEndAt_ ? listEndAt_ : ~0ull;
  if (waiting_) t = std::min(t, sys_.now + 128);  // olha de novo os pedidos dos aparelhos
  if (listActive_) t = std::min(t, sys_.now + 256);
  for (int c = 0; c < 7; c++)
    if ((chopping_ >> c) & 1) t = std::min(t, chopAt_[c]);  // próxima janela do chopping
  return t;
}

void Dma::sync(StateIO& io) {
  io.raw(ch_, sizeof ch_);
  io.sync(dpcr_); io.sync(dicr_); io.sync(last_); io.sync(listActive_); io.sync(waiting_);
  io.raw(readyAt_, sizeof readyAt_);
  io.sync(listEndAt_);
  io.sync(chopping_);
  io.raw(chopAt_, sizeof chopAt_);
  if (io.reading()) {
    chopping_ &= 0x3F;  // a OTC não faz chopping
    for (auto& t : chopAt_) if (t > sys_.now + 0x100) t = sys_.now;
    if (listEndAt_ > sys_.now + 0x4000000) listEndAt_ = sys_.now;
    for (auto& r : readyAt_) if (r > sys_.now + 0x100000) r = sys_.now;
    waiting_ &= 0x7F;
    for (auto& c : ch_) c.madr &= 0xFFFFFF;
    if (last_ > sys_.now) last_ = sys_.now;
  }
}

}  // namespace indigo::ps1
