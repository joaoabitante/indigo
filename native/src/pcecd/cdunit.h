// Interface CD-ROM² do PC Engine (a unidade de interface + o leitor de CD), ligada ao PC Engine pelo ponto de
// extensão PceExpansion (src/pce/pce.h):
//  - porta $1800-$180F: barramento SCSI (seleção, fases de comando, dados, status e mensagem, ACK manual e
//    automático em $1808), comandos TEST UNIT READY, REQUEST SENSE, READ(6), AUDIO START/STOP POSITION
//    ($D8/$D9), PAUSE ($DA), READ SUBCODE Q ($DD) e READ TOC ($DE), com tempos de busca (modelo de
//    Dave Shadoff, PCECD_seek) e de leitura (75 setores de 2048 bytes por segundo) aproximados;
//  - ADPCM: MSM5205 com 64 KB de RAM, endereços de leitura/escrita, comprimento, flags de metade e de fim,
//    DMA do CD para a RAM do ADPCM, taxa de 32 kHz / (16 - n), acessos à RAM em janelas de tempo;
//  - CD-DA (44,1 kHz) com o fader ($180F, CD-DA ou ADPCM, 6 s ou 2,5 s) e a amostra lida em $1805/$1806;
//  - interrupções do CD na linha IRQ2 ($1802 habilita, $1803 mostra): metade e fim do ADPCM, subcódigo,
//    status/mensagem e dados prontos;
//  - memória: 64 KB de RAM do CD (bancos $80-$87), 192 KB do Super System Card 3.0 (bancos $68-$7F, com a
//    assinatura em $18C1-$18C3) e o Arcade Card (2 MB, portas $1A00-$1AFF e bancos $40-$43).
// A BRAM de 2 KB (banco $F7, $1803/$1807) continua no PC Engine (pce.h).
#pragma once
#include <cstdint>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/state.h"
#include "../pce/pce.h"

namespace indigo::pcecd {

// CD-ROM² (System Card 1.0/2.x: só os 64 KB), Super CD-ROM² (3.0: mais 192 KB) e Arcade CD-ROM² (3.0 com o
// Arcade Card)
enum class Model : uint8_t { CdRom2 = 0, SuperCdRom2 = 1, ArcadeCdRom2 = 2 };

class CdUnit : public pce::PceExpansion {
public:
  static constexpr int64_t NONE = INT64_MAX;
  static constexpr double MASTER_HZ = 236250000.0 / 11.0;  // 21,477 MHz

  CdUnit(cdrom::Disc& disc, Model model, pce::HuC6280& cpu);
  void reset();
  Model model() const { return model_; }

  // PceExpansion
  void mapBanks(pce::HuC6280& cpu) override;
  uint8_t ioRead(uint16_t off) override;
  void ioWrite(uint16_t off, uint8_t v) override;
  uint8_t bankRead(uint8_t bank, uint16_t off) override;
  void bankWrite(uint8_t bank, uint16_t off, uint8_t v) override;
  void runTo(int64_t clk) override;
  int64_t nextEvent() const override;

  // som do CD-DA e do ADPCM na taxa pedida (somado ao do PSG pelo núcleo)
  void setSampleRate(int rate);
  size_t available() const { return (aw_ + ABUF - ar_) % ABUF; }
  void pull(float* l, float* r, size_t n);  // copia (não soma)

  void sync(StateIO& io);

  // para os testes
  const std::vector<uint8_t>& adpcmRam() const { return adpcmRam_; }
  const std::vector<uint8_t>& cdRam() const { return cdRam_; }
  uint8_t irqActive() const { return irqActive_; }
  int cdAudioStatus() const { return cdStatus_; }
  uint32_t cdAudioSector() const { return cdSector_; }
  static double seekMs(uint32_t from, uint32_t to);

private:
  enum Sig : uint16_t { ACK = 1, BSY = 2, CD = 4, IO = 8, MSG = 16, REQ = 32, RST = 64, SEL = 128 };
  enum Phase : uint8_t { BUS_FREE, COMMAND, DATA_IN, MESSAGE_IN, STATUS, BUSY };
  enum Upd : uint8_t { U_CMD, U_REQ, U_DATA_IN, U_GOOD };
  enum Irq : uint8_t { IRQ_ADPCM_HALF = 0x04, IRQ_ADPCM_END = 0x08, IRQ_SUBCODE = 0x10, IRQ_STATUS = 0x20, IRQ_DATA = 0x40 };
  enum CdStatus : uint8_t { CD_PLAYING = 0, CD_INACTIVE = 1, CD_PAUSED = 2, CD_STOPPED = 3 };
  enum CdEnd : uint8_t { END_STOP = 0, END_LOOP = 1, END_IRQ = 2 };

  cdrom::Disc& disc_;
  Model model_;
  pce::HuC6280& cpu_;
  int64_t now_ = 0;

  // ---- interrupções e registradores simples
  uint8_t irqEnabled_ = 0, irqActive_ = 0, resetReg_ = 0;
  bool readRight_ = false;
  int16_t latch_ = 0;
  int64_t latchUntil_ = 0;

  // ---- SCSI
  uint16_t sig_ = 0;
  uint8_t phase_ = BUS_FREE;
  bool msgDone_ = false;
  uint8_t dataPort_ = 0, readPort_ = 0;
  uint32_t sector_ = 0;
  uint16_t sectorsLeft_ = 0;
  uint8_t cmd_[16] = {};
  uint8_t cmdLen_ = 0;
  uint8_t fifo_[2048] = {};
  uint16_t fifoPos_ = 0, fifoLen_ = 0;
  int64_t tUpdate_ = NONE, tSector_ = NONE, tAckClr_ = NONE;
  uint8_t updType_ = U_CMD;
  bool changed_ = false;  // transitório (sempre falso entre chamadas)

  // ---- CD-DA
  uint8_t cdStatus_ = CD_INACTIVE, cdEnd_ = END_STOP;
  uint32_t cdStart_ = 0, cdStop_ = 0, cdSector_ = 0;
  uint16_t cdSample_ = 0;
  int16_t cdL_ = 0, cdR_ = 0;
  int64_t tCdTick_ = 0, tPlayStart_ = NONE;
  uint8_t subTick_ = 0, subPos_ = 0;
  std::vector<uint8_t> cdBuf_ = std::vector<uint8_t>(cdrom::RAW_SECTOR);
  int64_t cdBufSector_ = -1;  // cache (não vai no estado)

  // ---- fader
  uint8_t faderReg_ = 0;
  int64_t faderStart_ = 0;

  // ---- ADPCM
  std::vector<uint8_t> adpcmRam_ = std::vector<uint8_t>(0x10000);
  uint16_t adAddrPort_ = 0, adReadAddr_ = 0, adWriteAddr_ = 0;
  uint32_t adLength_ = 0;  // 17 bits
  uint8_t adCtrl_ = 0, adDma_ = 0, adRate_ = 0, adReadBuf_ = 0, adWriteBuf_ = 0;
  bool adNibble_ = false, adPlaying_ = false, adPlayReq_ = false, adEnd_ = false, adHalf_ = false;
  int16_t adOut_ = 2048;  // 12 bits
  uint8_t adMag_ = 0;
  int64_t tAdSample_ = NONE, tAdRead_ = NONE, tAdWrite_ = NONE, tDma_ = NONE;
  uint32_t adFrac_ = 0;  // resto da divisão do período da amostra

  // ---- memórias
  std::vector<uint8_t> cdRam_ = std::vector<uint8_t>(0x10000);
  std::vector<uint8_t> superRam_;  // 192 KB (Super System Card)

  // ---- Arcade Card
  struct AcPort { uint32_t base = 0; uint16_t offset = 0, inc = 0; uint8_t ctrl = 0; };
  bool arcade_ = false, acUsed_ = false;
  std::vector<uint8_t> acRam_;  // 2 MB
  AcPort acPort_[4];
  uint32_t acValue_ = 0;
  uint8_t acShift_ = 0, acRotate_ = 0;

  // ---- som
  static constexpr size_t ABUF = 16384;
  std::vector<float> abL_ = std::vector<float>(ABUF), abR_ = std::vector<float>(ABUF);
  size_t aw_ = 0, ar_ = 0;
  int hostRate_ = 48000;
  double hostPeriod_ = MASTER_HZ / 48000.0;
  int64_t audioClk_ = 0;
  double accL_ = 0, accR_ = 0, accN_ = 0;

  // interrupções
  void irqSet(uint8_t src);
  void irqClear(uint8_t src);
  void irqLine();
  // SCSI
  void setSig(uint16_t s, bool on);
  void setPhase(uint8_t p);
  void scsiIrq();
  void scsiReset();
  void statusMessage(uint8_t status, uint8_t data, int len = 1);
  void queue(uint8_t type, int64_t delay);
  void update();
  void processCommand();
  void processDataIn();
  void processStatus();
  void processMessageIn();
  void execCommand();
  uint32_t audioLba() const;
  void ackAutoClear();
  uint8_t dataRead() const;
  uint32_t currentSector() const { return cdStatus_ == CD_INACTIVE ? sector_ : cdSector_; }
  int64_t seekClocks(uint32_t from, uint32_t to) const;
  void fifoSet(const uint8_t* p, int n);
  // CD-DA
  void cdPlay(uint32_t start, bool pause);
  void cdTick();
  void cdSampleOut();
  // ADPCM
  void adFlags();
  void adReset();
  void adSetHalf(bool v);
  void adSetEnd(bool v);
  void adControl(uint8_t v);
  void adSample();
  void adDmaCheck();
  void adTimer();
  int64_t adSlot(bool forRead) const;
  int64_t adPeriodNext();
  // Arcade Card
  uint32_t acAddr(const AcPort& p) const;
  void acAutoInc(AcPort& p);
  void acAddOffset(AcPort& p);
  uint8_t acPortRead(int n);
  void acPortWrite(int n, uint8_t v);
  uint8_t acRead(uint16_t off);
  void acWrite(uint16_t off, uint8_t v);
  // som
  double faderVol(bool adpcm, int64_t t) const;
  void advanceAudio(int64_t t);
  void emit(double l, double r);
  // eventos
  void runEvents(int64_t t);
};

}  // namespace indigo::pcecd
