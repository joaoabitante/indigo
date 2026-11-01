// Unidade de CD do Neo Geo CD: o leitor (controle da unidade pelo protocolo de 4 bits da BIOS), o decodificador
// LC8951 (CDC) e o CD-DA.
//
// Protocolo com a unidade (como no aparelho e no NeoCD, que serve de régua): a cada interrupção de comunicação
// (vetor $58, ~64,6 Hz parado ou 75 Hz lendo) a BIOS troca um pacote de 5 bytes em 10 meios-bytes:
//   $FF0161 (leitura)  meio-byte da resposta, com o bit 4 = "strobe"
//   $FF0163 (escrita)  meio-byte do comando
//   $FF0165 (escrita)  1 = avança o comando (no 10º meio-byte o comando é executado); 2 = avança a resposta e
//                      baixa o strobe; 3 = sobe o strobe
//   $FF0181            0 = comunicação desligada (sem interrupção), 1 = ligada (zera os ponteiros)
// O último meio-byte é a soma de verificação (~(soma dos 9 meios-bytes + 5) & 15). Comandos: 00 estado,
// 10 parar, 20 consultas (posição, faixa, fim do disco, início das faixas...), 30 tocar a partir de um MSF,
// 40 posicionar, 60 pausa, 70 continuar, 80/90 avançar/voltar, B0 ir para a faixa.
//
// LC8951: registradores pelo par $FF0101 (número do registrador, que avança sozinho) / $FF0103 (dado). A cada
// setor lido com o decodificador ligado (CTRL0.DECEN) os registradores HEAD trazem o MSF do setor, o setor de
// dados vai para o buffer e a interrupção do decodificador (vetor $54) é pedida; a BIOS escreve DBC/DAC/DTRG e o
// DMA do console copia o buffer (2048 bytes) para a memória.
//
// CD-DA: as faixas de áudio tocam a 44 100 Hz (588 amostras por setor, 75 setores por segundo); as amostras de
// cada setor tocado entram numa fila e são misturadas ao som do YM2610 na taxa de saída.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/state.h"

namespace indigo::neogeocd {

class CdUnit {
public:
  explicit CdUnit(cdrom::Disc& d) : disc_(d) { reset(); }
  void reset();

  // estado da unidade (resposta ao comando 00)
  enum Status : uint8_t {
    IDLE = 0x00, PLAYING = 0x10, SEEKING = 0x20, SCANNING = 0x30, PAUSED = 0x40, STOPPED = 0x90, END_OF_DISC = 0xC0
  };

  // comunicação
  uint8_t readResponse() const;
  void writeCommand(uint8_t v);
  void pointerStep(uint8_t v);
  void resetPointers() { cmdPtr_ = 0; respPtr_ = 9; strobe_ = 1; }
  void driveReset();  // $FF0000: para a unidade

  // LC8951
  void setRegister(uint8_t v) { regPtr_ = v; }
  uint8_t readRegister();
  void writeRegister(uint8_t v);
  bool decoderEnabled() const { return ctrl0_ & DECEN; }
  bool decoderIrqEnabled() const { return ifctrl_ & DECIEN; }
  bool decoderIrqPending() const { return !(ifstat_ & DECI); }
  bool transferStarted() const { return !(ifstat_ & DTBSY); }
  const uint8_t* buffer() const { return buffer_; }
  void endTransfer();

  // um "passo" do leitor (chamado pelo temporizador do CD): se está tocando, lê o setor atual (dados ou áudio) e
  // avança; devolve true se o decodificador deve pedir a interrupção
  bool tick();
  // ciclos do 68000 até o próximo passo (150 Hz lendo dados no CDZ)
  int delay(bool cdz) const;
  bool playing() const { return playing_; }

  // CD-DA: amostras de 44 100 Hz na fila; produce() reamostra n amostras para a taxa de saída
  void produce(float* l, float* r, size_t n, double outRate);
  int16_t audioSample(int ch) const;  // amostra atual ($FF0188/$FF018A)

  void sync(StateIO& io);

private:
  enum { DECEN = 0x80, AUTORQ = 0x10, DECIEN = 0x20, DOUTEN = 0x02 };  // CTRL0 / IFCTRL
  enum { DECI = 0x20, DTEI = 0x40, DTBSY = 0x08 };                      // IFSTAT (ativos em 0)
  enum { MODRQ = 0x08, FORMRQ = 0x04, SHDREN = 0x01 };                  // CTRL1

  void process();  // executa o comando recebido
  int trackIdx() const { return disc_.trackAt(pos_); }
  bool isData() const;
  bool isAudio() const;
  int trackNumberAt() const;
  bool pregap() const;
  uint32_t trackStart(int number) const;
  bool trackIsData(int number) const;
  void seek(uint32_t lba) { pos_ = lba; }
  void updateHead(uint32_t lba);
  static uint8_t checksum(const uint8_t* p);
  static void setChecksum(uint8_t* p) { p[4] = static_cast<uint8_t>((p[4] & 0xF0) | checksum(p)); }

  cdrom::Disc& disc_;

  // unidade
  uint8_t status_ = IDLE;
  uint32_t pos_ = 0;
  bool playing_ = false;
  uint8_t cmd_[5] = {}, resp_[5] = {};
  uint32_t cmdPtr_ = 0, respPtr_ = 9, strobe_ = 1;

  // LC8951
  uint8_t regPtr_ = 0;
  uint8_t sbout_ = 0, ifctrl_ = 0, dbcl_ = 0, dbch_ = 0, dacl_ = 0, dach_ = 0, dtrg_ = 0, dtack_ = 0;
  uint8_t wal_ = 0, wah_ = 0, ctrl0_ = 0, ctrl1_ = 0, ptl_ = 0, pth_ = 0;
  uint8_t comin_ = 0, ifstat_ = 0xFF, head_[4] = {}, stat_[4] = {};
  uint8_t buffer_[2048] = {};

  // CD-DA (amostras intercaladas esquerda/direita, 16 bits)
  std::deque<int16_t> fifo_;
  double frac_ = 0;
};

}  // namespace indigo::neogeocd
