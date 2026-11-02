// Leitor de CD do PC-FX visto pelo barramento SCSI do KING (HuC6272): sinais BSY, REQ, ACK, SEL, ATN, RST,
// C/D, I/O, MSG e o barramento de dados, com as fases livre, comando, dados (entrada e saída), status,
// mensagem (entrada e saída). Comandos: TEST UNIT READY, REZERO, REQUEST SENSE, READ(6/10/12), SEEK(6/10),
// INQUIRY, MODE SELECT/SENSE(6) (mínimos), START/STOP, PREVENT/ALLOW, READ CD-ROM CAPACITY, PREFETCH, READ
// SUBCHANNEL, READ TOC, READ HEADER, PLAY AUDIO(10/12/MSF/TRACK INDEX), PAUSE/RESUME e os da NEC (D8 a DE:
// início/fim do áudio, pausa, subcanal Q, informações do disco). Leitura a 300 KB/s (velocidade dupla) e CD-DA
// a 44,1 kHz. O comportamento segue o que se conhece do aparelho (documentação do Mednafen sobre o PC-FX).
#pragma once
#include <cstdint>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/state.h"

namespace indigo::pcfx {

// estado salvo: todo instante gravado tem de estar perto do relógio da CPU (±3 s) ou ser "nunca";
// um valor absurdo faria os laços de tempo girarem para sempre
constexpr int64_t JANELA_ESTADO = int64_t(1) << 26;
inline bool tempoOk(int64_t t, int64_t agora, bool podeNunca = false) {
  return (podeNunca && t == INT64_MAX) || (t >= agora - JANELA_ESTADO && t <= agora + JANELA_ESTADO);
}

class ScsiCd {
public:
  static constexpr int64_t NONE = INT64_MAX;
  enum Sig : uint16_t { BSY = 1, REQ = 2, CD = 4, IO = 8, MSG = 16, ACK = 32, SEL = 64, ATN = 128, RST = 256 };

  explicit ScsiCd(cdrom::Disc* disc);
  void power(int64_t now);

  // sinais que o KING (iniciador) controla
  void setDB(uint8_t v) { db_ = v; }
  void setSignal(uint16_t s, bool on) { sig_ = on ? (sig_ | s) : (sig_ & ~s); }
  uint8_t db() const { return db_; }
  bool sig(uint16_t s) const { return (sig_ & s) != 0; }

  // avança o leitor até o instante t (relógio mestre); devolve o próximo instante em que algo acontece
  int64_t run(int64_t t);
  // REQ acabou de subir (o KING usa para a interrupção de "mudança de fase")
  bool reqRose = false;

  // CD-DA: amostras de 44,1 kHz produzidas desde a última chamada (o som tira daqui)
  std::vector<int16_t> cdda;  // L, R, L, R...
  uint8_t subchannel = 0;     // último byte de subcanal (P-W) para o KING
  bool subValid = false;

  int status() const { return cddaStatus_; }
  void sync(StateIO& io);
  bool temposOk(int64_t agora) const {
    return tempoOk(last_, agora) && tempoOk(readTimer_, agora, true) && tempoOk(cddaNext_, agora, true);
  }

private:
  enum Phase : uint8_t { BUS_FREE, COMMAND, DATA_IN, DATA_OUT, STATUS, MESSAGE_IN, MESSAGE_OUT };
  enum : int8_t { CD_PAUSED = -1, CD_STOPPED = 0, CD_PLAYING = 1 };
  enum : uint8_t { PM_SILENT = 0, PM_NORMAL, PM_INTERRUPT, PM_LOOP };

  cdrom::Disc* disc_;
  int64_t last_ = 0;
  uint16_t sig_ = 0;
  uint8_t db_ = 0;
  uint8_t phase_ = BUS_FREE;
  bool lastRst_ = false;
  // comando
  uint8_t cmd_[16] = {};
  uint8_t cmdPos_ = 0;
  uint8_t dataOut_[256] = {};
  uint8_t dataOutPos_ = 0, dataOutWant_ = 0;
  bool statusSent_ = false, msgSent_ = false;
  uint8_t msgPending_ = 0;
  uint8_t senseKey_ = 0, senseAsc_ = 0;
  // fila de dados para o iniciador
  std::vector<uint8_t> fifo_;
  uint32_t fifoPos_ = 0;
  bool transferDone_ = false;
  // leitura de setores
  int64_t readTimer_ = NONE;
  uint32_t sectorAddr_ = 0, sectorCount_ = 0;
  // CD-DA
  int8_t cddaStatus_ = CD_STOPPED;
  uint8_t playMode_ = PM_SILENT;
  uint32_t readSec_ = 0, readSecStart_ = 0, readSecEnd_ = 0;
  uint16_t cddaPos_ = 588;
  int16_t cddaBuf_[588 * 2] = {};
  int64_t cddaNext_ = NONE;
  uint32_t cddaFrac_ = 0;
  uint8_t subQ_[10] = {};
  uint8_t modeData_[16] = {};

  void setReq(bool on);
  void changePhase(uint8_t p);
  void statusMessage(uint8_t status, uint8_t msg);
  void ccError(uint8_t key, uint8_t asc = 0);
  void dataIn(const uint8_t* p, size_t n);
  void virtualReset();
  void execute();
  void readBase(uint32_t sa, uint32_t sc, int64_t now);
  void playBase(uint32_t lba, uint32_t len, int8_t status, uint8_t mode);
  void runRead(int64_t t);
  void runCdda(int64_t t);
  void makeSubQ(uint32_t lba);
  uint32_t leadout() const;
  int trackOf(uint32_t lba) const;  // número da faixa (1..), 0 fora
  bool isData(int track) const;
  uint32_t trackStart(int track) const;
};

}  // namespace indigo::pcfx
