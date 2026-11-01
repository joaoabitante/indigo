// Controlador de CD-ROM do PlayStation 1 (o decodificador CXD1199/CXD1815 com o microcontrolador HC05 e o
// leitor), em 0x1F801800..0x1F801803, com a interrupção IRQ2 e o canal 3 do DMA:
//  - registradores com o índice (0x1F801800), FIFO de parâmetros (16 bytes), FIFO de respostas (16 bytes,
//    completada com zeros e lida em círculo), buffer de dados do setor (2048, 2340 ou 2328 bytes), máscara e
//    flags de interrupção (INT1 dados/relatório, INT2 fim, INT3 aceite, INT4 fim de faixa/disco, INT5 erro);
//  - as respostas esperam a interrupção anterior ser reconhecida (com um pequeno atraso), como no HC05;
//  - comandos: Getstat, Setloc, Play, Forward, Backward, ReadN, MotorOn, Stop, Pause, Init, Mute, Demute,
//    Setfilter, Setmode, Getparam, GetlocL, GetlocP, SetSession, GetTN, GetTD, SeekL, SeekP, Test (versão,
//    chaves, região, chips, SCEx), GetID (licença "SCEA/SCEE/SCEI" pela região do disco), ReadS, Reset,
//    GetQ e ReadTOC; os inválidos dão INT5 com o código de erro do hardware;
//  - tempos aproximados do hardware: primeira resposta (~0xC4E1 ciclos com o motor ligado), segundas
//    respostas medidas (Pause, Stop, GetID), busca proporcional à distância, partida do motor, leitura de
//    75 ou 150 setores por segundo e a latência de dois setores do decodificador; o Init busca o começo pelo
//    subcanal (para no pregap) e a busca além da área final dá erro (INT5 stat+4, 04h) e para o motor;
//  - áudio: CD-DA e XA-ADPCM (Mode 2 Form 2: 4 ou 8 bits, mono ou estéreo, 37,8 ou 18,9 kHz, filtro do
//    setmode), reamostrado para 44,1 kHz pelo interpolador em zigue-zague de 7 fases com a tabela do
//    hardware, volume do CD (a matriz de 4 registradores) e Mute/Demute; sai por onAudio a cada 768 ciclos;
//  - disco: ligado por um cdrom::Disc* (nullptr = sem disco) e a tampa aberta/fechada.
// Ciclos são da CPU (33,8688 MHz).
#pragma once
#include <cstdint>
#include <functional>

#include "../cdrom/cdrom.h"
#include "../core/state.h"

namespace indigo::ps1 {

class CdRom {
public:
  static constexpr int32_t CPU_HZ = 33868800;

  explicit CdRom(cdrom::Disc* disc = nullptr);

  // troca o disco (nullptr = sem disco); com a tampa fechada é como abrir e fechar a tampa
  void setDisc(cdrom::Disc* disc);
  cdrom::Disc* disc() const { return disc_; }
  void setLidOpen(bool open);
  bool lidOpen() const { return lidOpen_; }
  // região do console para o Test 22h ('A', 'E' ou 'I'); 0 = a do disco
  void setConsoleRegion(char r) { consoleRegion_ = r; }
  // região do disco lida da licença no setor 4 ('A', 'E', 'I'; 0 se não houver)
  char discRegion() const { return discRegion_; }

  void reset();
  uint32_t read(uint32_t offset, int size);
  void write(uint32_t offset, uint32_t value, int size);
  void run(int cycles);
  uint32_t dmaRead();  // canal 3: quatro bytes do buffer de dados

  std::function<void()> onIrq;                   // borda de subida da IRQ2
  std::function<void(int16_t, int16_t)> onAudio;  // um par de amostras a 44,1 kHz
  std::function<void(int16_t, int16_t)> onXaSample;  // para os testes: cada amostra XA decodificada (antes de reamostrar)

  void save(StateIO& io) { sync(io); }
  void load(StateIO& io) { sync(io); }

  // arredondamento do filtro do XA-ADPCM: o padrão segue o psx-spx ((old*f0 + older*f1 + 32) >> 6); a outra
  // forma (cada termo >> 6, como o Beetle/Mednafen faz) existe só para comparar o resto do decodificador
  void setXaBeetleRounding(bool b) { xaBeetleRounding_ = b; }

  // estado interno para os testes
  bool irqLine() const { return (if_ & ie_ & 0x1F) != 0; }
  int32_t position() const { return cur_; }

private:
  enum Drive : uint8_t {
    STOPPED, SPINUP, SEEKING, SEEKING_LOGICAL, SEEK_VERIFY, READING, PLAYING, PAUSED, STANDBY
  };
  enum Phase : uint8_t { CMD_IDLE, CMD_RECEIVED, CMD_SECOND, CMD_SEEKWAIT };

  static constexpr int CDDA_CAP = 4096;   // pares de amostras de CD-DA
  static constexpr int XA_CAP = 16384;    // amostras de XA (37,8 kHz) por canal

  void sync(StateIO& io);
  void softReset();
  void updateIrq();
  void raise(uint8_t type, const uint8_t* bytes, int n);
  bool canRaise() const { return !(if_ & 0x0F) && readyDelay_ <= 0; }
  void setAsync(uint8_t type, const uint8_t* bytes, int n);
  uint8_t stat(bool error = false) const;
  void errorResponse(uint8_t code, uint8_t statOr = 0);
  bool needDisc();
  bool discReady() const { return disc_ && !lidOpen_ && drive_ != SPINUP; }
  int32_t period() const { return (mode_ & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75; }
  int32_t seekTime(int32_t from, int32_t to, bool resume = true) const;
  void startSeek(int32_t target, bool logical, Drive after, int32_t extra, bool resume = true);
  void stopReading();
  void execute();
  void second();
  void sectorEvent();
  void processSector(const uint8_t* raw, const uint8_t* q);
  void readSector(int32_t lba, uint8_t* raw, uint8_t* q);
  bool makeQ(int32_t lba, uint8_t* q) const;
  void deliverData(const uint8_t* raw);
  void decodeXa(const uint8_t* raw);
  void pushCdda(const uint8_t* raw);
  void clearAudio();
  void audioTick();
  void detectDisc();
  void lidEvent();

  cdrom::Disc* disc_ = nullptr;
  char discRegion_ = 0, consoleRegion_ = 0;
  bool discMode2_ = false, discHasData_ = false;

  // ---- interface com a CPU
  uint8_t index_ = 0;
  uint8_t params_[16] = {};
  uint8_t prmCount_ = 0;
  uint8_t result_[16] = {};
  uint8_t resCount_ = 0, resRd_ = 0;
  uint8_t ie_ = 0, if_ = 0;
  bool irqOut_ = false;
  int32_t readyDelay_ = 0;     // atraso entre o reconhecimento e a próxima interrupção
  uint8_t asyncType_ = 0;      // INT1/INT4/INT5 esperando para sair (o mais novo substitui)
  uint8_t asyncBytes_[8] = {};
  uint8_t asyncCount_ = 0;
  uint8_t request_ = 0;        // HCHPCTL

  // ---- comando em andamento
  uint8_t cmd_ = 0;
  uint8_t phase_ = CMD_IDLE;
  int32_t cmdTimer_ = 0;
  uint8_t args_[16] = {};
  uint8_t nargs_ = 0;

  // ---- leitor
  uint8_t drive_ = STOPPED, after_ = STOPPED;
  int32_t cur_ = 0, target_ = 0, setloc_ = 0;
  bool setlocDirty_ = false;
  int32_t sectorTimer_ = 0;    // ciclos até o próximo setor (0 = parado)
  int32_t spinTimer_ = 0;      // partida do motor depois de fechar a tampa / trocar o disco
  int32_t spinDown_ = 0;       // tampa aberta: o motor ainda gira (Getstat com o bit 1) até parar
  int8_t seekResult_ = 1;      // 0 em andamento, 1 ok, -1 falhou
  bool seekFail_ = false;      // o alvo da busca fica além da área final: a busca falha e o motor para
  uint8_t retry_ = 0;
  bool holdLogical_ = false;
  uint32_t sectorsRead_ = 0;
  int32_t reportDelay_ = 0;
  uint8_t reportLastF_ = 0xFF;
  int16_t playTrack_ = -1;
  int8_t ff_ = 0;              // +1 avanço rápido, -1 retrocesso
  uint8_t mode_ = 0x20, filterFile_ = 0, filterChan_ = 0;
  bool muted_ = false, adpMute_ = false;
  uint8_t volPending_[4] = {0x80, 0, 0x80, 0};  // L->L, L->R, R->R, R->L
  uint8_t vol_[4] = {0x80, 0, 0x80, 0};
  bool lidOpen_ = false, shellOpen_ = true;
  bool xaBeetleRounding_ = false;  // configuração (não vai no estado salvo)
  // latência do decodificador: os dois últimos setores lidos (bruto + subcanal Q)
  uint8_t pipe_[2][2352] = {};
  uint8_t pipeQ_[2][12] = {};
  uint8_t pipeCount_ = 0, pipePos_ = 0;
  uint8_t q_[12] = {};         // subcanal Q do último setor lido
  uint8_t header_[12] = {};    // cabeçalho + subcabeçalho do último setor de dados
  bool headerValid_ = false;

  // ---- dados
  uint8_t sector_[2340] = {};  // último setor entregue
  uint16_t sectorSize_ = 0;
  bool sectorNew_ = false;
  uint8_t fifo_[2340] = {};    // buffer lido pela CPU/DMA
  uint16_t fifoSize_ = 0, fifoPos_ = 0;

  // ---- áudio
  int32_t audioTimer_ = 768;
  int16_t cdda_[CDDA_CAP][2] = {};
  uint16_t cddaRd_ = 0, cddaCount_ = 0;
  uint8_t cddaStep_ = 1;
  int16_t xa_[XA_CAP][2] = {};
  uint16_t xaRd_ = 0, xaCount_ = 0;
  int16_t xaPrev_[2][2] = {};  // [canal][old, older]
  int16_t xaLast_[2] = {};     // última amostra (18,9 kHz)
  int16_t zig_[2][32] = {};
  uint8_t zigPos_ = 0, zigPhase_ = 0;
};

}  // namespace indigo::ps1
