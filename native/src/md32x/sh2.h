// Hitachi SH-2 (SH7604) do 32X: núcleo RISC de 32 bits com os periféricos internos que os jogos usam.
//  - instruções do SH-2 (com os "delay slots" dos desvios, a exceção de instrução ilegal e de ilegal no slot);
//  - cache de 4 KB, 4 vias, 64 linhas de 16 bytes, gravação direta (write-through), LRU do manual, modo de duas
//    vias (metade vira RAM em C0000000), área sem cache (2xxxxxxx), área de descarte (4xxxxxxx), matriz de
//    endereços (6xxxxxxx) e de dados (Cxxxxxxx);
//  - divisor (DIVU 32/32 e 64/32, com estouro), DMA interno de 2 canais (DMAC: automático ou pelo pedido DREQ
//    externo), temporizador livre (FRT) e cão de guarda (WDT, modo de intervalo), controlador de interrupções
//    (prioridades IPRA/IPRB e vetores VCRx) e a serial (SCI) ligada à do outro SH-2.
//
// Convenção: `pc` é o endereço da instrução que vai executar. Dentro da instrução, o "PC" do manual (base dos
// endereços relativos) é pc + 4; no slot de um desvio é o destino + 2, como manda o manual.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::md32x {

class Sh2;

// barramento externo (endereços físicos de 29 bits, já sem os bits de área de cache)
class Sh2Bus {
public:
  virtual ~Sh2Bus() = default;
  virtual uint8_t rd8(Sh2& c, uint32_t a) = 0;
  virtual uint16_t rd16(Sh2& c, uint32_t a) = 0;
  virtual uint32_t rd32(Sh2& c, uint32_t a) { return (uint32_t(rd16(c, a)) << 16) | rd16(c, a + 2); }
  virtual void wr8(Sh2& c, uint32_t a, uint8_t v) = 0;
  virtual void wr16(Sh2& c, uint32_t a, uint16_t v) = 0;
  virtual void wr32(Sh2& c, uint32_t a, uint32_t v) { wr16(c, a, uint16_t(v >> 16)); wr16(c, a + 2, uint16_t(v)); }
  // nível da interrupção externa (IRL) foi aceito: vetor automático 64 + nível/2
  virtual void irlAck(Sh2&, int) {}
  // vetor da interrupção externa dado pelo barramento (modo de vetor externo, como a SCU do Saturn); -1 = automático
  virtual int irlVector(Sh2&, int /*nível*/) { return -1; }
  // o SH-2 escreveu no canal de DMA (pode ter um pedido DREQ esperando)
  virtual void dmaKick(Sh2&, int /*canal*/) {}
};

class Sh2 {
public:
  Sh2(Sh2Bus& bus, int id);

  // registradores
  uint32_t r[16] = {};
  uint32_t pc = 0, pr = 0, sr = 0xF0, gbr = 0, vbr = 0, mach = 0, macl = 0;
  int id;                    // 0 = mestre, 1 = escravo
  int64_t clock = 0;         // ciclos do SH-2 desde o começo
  bool sleeping = false;     // SLEEP: parado até uma interrupção
  Sh2* other = nullptr;      // o outro SH-2 (serial)
  bool slaveMode = false;    // pino de modo escravo (BCR1 bit 15 lido como 1); o 32X não usa

  // mapa rápido de leitura/escrita por bloco de 16 MB do endereço físico (nulo = barramento)
  const uint8_t* rpage[32] = {};
  uint8_t* wpage[32] = {};
  uint32_t pmask[32] = {};
  uint8_t rwait[32] = {};   // ciclos de espera de cada leitura nesse bloco
  void remapped() { mapGen_++; }

  void reset();              // estado de ligar (lê PC e SP dos vetores 0 e 1)
  void powerOn();            // zera também cache e periféricos
  void run(int64_t until);   // executa até `clock >= until`
  int step();                // uma instrução (ou uma interrupção); devolve os ciclos

  // interrupção externa (IRL 0..15, já com o nível da prioridade)
  void setIrl(int level) { if (irl_ != level) { irl_ = level; irqCheck_ = true; } }
  int irl() const { return irl_; }
  int64_t timerNext() const { return timerNext_; }  // próximo evento dos temporizadores internos
  bool stop = false;         // o barramento pede para sair de run() depois desta instrução (laço de espera)
  // pedido DREQ externo no canal (devolve true se o canal aceitou e transferiu)
  bool dreq(int ch, int maxUnits);
  // pulso no pino FTI: captura do temporizador livre (o Saturn liga os dois SH-2 por aqui)
  void inputCapture();
  bool dmaActive(int ch) const;
  uint32_t dmaSource(int ch) const { return dma_[ch].sar; }
  uint32_t dmaDest(int ch) const { return dma_[ch].dar; }
  uint32_t dmaCount(int ch) const { return dma_[ch].tcr; }

  // memória vista pelo programa (com cache e áreas internas)
  uint8_t read8(uint32_t a);
  uint16_t read16(uint32_t a);
  uint32_t read32(uint32_t a);
  void write8(uint32_t a, uint8_t v);
  void write16(uint32_t a, uint16_t v);
  void write32(uint32_t a, uint32_t v);

  // usado pelo teste de instruções: desliga a cache e as esperas
  uint8_t cacheData[4096] = {};
  void sync(StateIO& io);

private:
  friend class Sh2Test;
  void exec(uint16_t op, uint32_t pcv);
  void delayed(uint32_t target);
  void exception(int vec, uint32_t savePc);
  void checkIrq();
  void illegal();
  bool isBranch(uint16_t op) const;
  uint16_t fetch(uint32_t a);

  // cache
  uint32_t cacheRead(uint32_t a, int size, bool code);
  void cacheWrite(uint32_t a, int size, uint32_t v);
  int cacheFind(uint32_t a) const;
  void lruTouch(int set, int way);
  int lruVictim(int set) const;
  uint32_t extRead(uint32_t a, int size);
  void extWrite(uint32_t a, int size, uint32_t v);

  // periféricos internos (FFFFFE00-FFFFFFFF)
  uint32_t ioRead(uint32_t a, int size);
  void ioWrite(uint32_t a, int size, uint32_t v);
  void ioWrite8(uint32_t a, uint8_t v);
  void divide64();
  void divide32();
  void dmaStart(int ch);
  void dmaUnit(int ch);
  void dmaDone(int ch);
  void frtRun();
  void wdtRun();
  void timersRun();
  void timersSchedule();
  void ioWrite8x(uint32_t a, uint8_t v);
  void sciSend();
  void updateInternalIrq();

  Sh2Bus& bus;
  uint32_t npc_ = 0;        // próximo PC (desvios mudam)
  bool inSlot_ = false;
  bool irqCheck_ = true;
  int irl_ = 0;
  int intLevel_ = 0, intVector_ = 0;  // interrupção interna pendente de maior prioridade
  int cyc_ = 0;             // ciclos da instrução atual
  uint32_t mapGen_ = 1;

  // atalho da busca de instruções: página de memória direta (sem cache)
  const uint8_t* fptr_ = nullptr;
  uint32_t fbase_ = 0, fend_ = 0, fgen_ = 0;
  int fset_ = -1;           // linha da cache do atalho (conjunto)

  // cache
  uint8_t ccr_ = 0;
  uint32_t tag_[64][4] = {};   // bits 28-10 = endereço, bit 0 = válido
  uint8_t lru_[64] = {};

  // DIVU
  uint32_t dvsr_ = 0, dvdnt_ = 0, dvcr_ = 0, vcrdiv_ = 0, dvdnth_ = 0, dvdntl_ = 0;
  // DMAC
  struct Dma { uint32_t sar, dar, tcr, chcr, vcr; uint8_t drcr; };
  Dma dma_[2] = {};
  uint32_t dmaor_ = 0;
  // FRT
  uint8_t tier_ = 1, ftcsr_ = 0, tcr_ = 0, tocr_ = 0xE0, temp_ = 0;
  uint16_t frc_ = 0, ocra_ = 0xFFFF, ocrb_ = 0xFFFF, ficr_ = 0;
  int64_t frtLast_ = 0;
  // WDT
  uint8_t wtcsr_ = 0x18, wtcnt_ = 0, rstcsr_ = 0x1F;
  int64_t wdtLast_ = 0;
  int64_t timerNext_ = INT64_MAX;  // próximo evento dos temporizadores (não vai no estado: recalculado)
  // INTC
  uint16_t ipra_ = 0, iprb_ = 0, vcra_ = 0, vcrb_ = 0, vcrc_ = 0, vcrd_ = 0, vcrwdt_ = 0, icr_ = 0;
  // SCI
  uint8_t smr_ = 0, brr_ = 0xFF, scr_ = 0, tdr_ = 0xFF, ssr_ = 0x84, rdr_ = 0;
  // demais registradores (BSC, SBYCR, UBC...): só guardados
  uint8_t misc_[512] = {};
};

}  // namespace indigo::md32x
