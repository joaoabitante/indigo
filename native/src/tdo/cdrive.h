// Unidade de CD do 3DO (a unidade Panasonic/MKE CR-560-B ligada ao XBUS, dispositivo 0). A CPU manda pacotes de
// 7 bytes de comando pela porta de comando/estado e lê a resposta (o byte do comando ecoado, os dados e o byte de
// estado da unidade). Os setores lidos vão para a fila de dados, lida pela CPU byte a byte ou pelo DMA do XBUS.
// Comandos conferidos com a documentação pública da interface MKE e com o modelo da CR-560-B no MAME
// (licença BSD-3-Clause):
//   0x01 busca  0x02 girar  0x03 parar  0x05 estado (apaga o "sucesso")  0x06/0x07 abrir/fechar a gaveta
//   0x08 abortar (um byte só, sem resposta)  0x09 modo (byte 1 = 0: bytes 3-4 tamanho do setor, 2048 ou 2352;
//   byte 1 = 3: bit 7 do byte 2 = velocidade dupla)  0x0E tocar MSF (bytes 1-3 começo, 4-6 fim; 0xFFFFFF = até o
//   fim do disco; tudo zero = parar)  0x0F tocar faixas (byte 1 a primeira, byte 3 a última, inclusive): o
//   áudio sai a 44,1 kHz misturado ao do DSP
//   0x10 ler (bytes 1-3 MSF, ou LBA com o bit 0 do byte 4; bytes 5-6 quantidade de setores): sem resposta
//     imediata; a resposta (0x10, estado com "sucesso") vem depois do último byte de dado
//   0x80 teste do caminho de dados (0x80 0xAA 0x55)  0x82 erro (8 bytes 0)  0x83 identificação (10 bytes)
//   0x85 capacidade (MSF do lead-out)  0x8B informações do disco (1ª e última faixa, lead-out)
//   0x8C TOC (byte 2 = faixa; 0 = lead-out)  0x8D sessão. Comando desconhecido: erro.
// Byte de estado: 0x80 gaveta fechada, 0x40 disco presente, 0x20 girando, 0x10 erro, 0x08 sucesso,
// 0x04 tocando, 0x02 velocidade dupla, 0x01 pronta. Na partida: velocidade simples, motor parado.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/state.h"

namespace indigo::tdo {

class CdDrive {
public:
  enum : uint8_t { ST_DOOR = 0x80, ST_DISC = 0x40, ST_SPIN = 0x20, ST_ERR = 0x10, ST_OK = 0x08, ST_PLAY = 0x04,
                   ST_2X = 0x02, ST_READY = 0x01 };
  static constexpr int CYCLES_PER_SECTOR_2X = 12500000 / 150;
  static constexpr int REPLY_DELAY = 3200;  // ~256 us para responder a um comando
  static constexpr size_t FIFO_MAX = 2448 * 8;

  void setDisc(cdrom::Disc* d) { disc_ = d; }
  bool hasDisc() const { return disc_ != nullptr; }
  void reset();

  // porta de comando/estado
  void writeCommand(uint8_t b);
  bool statusReady() const { return !status_.empty(); }
  uint8_t readStatus();
  bool dataReady() const { return !data_.empty(); }
  uint8_t readData();
  size_t dataSize() const { return data_.size(); }
  bool canCommand() const { return cmdLen_ < 7 && delay_ <= 0; }

  void tick(int cycles);  // avança o tempo da unidade
  // áudio de CD (comandos 0x0E/0x0F): a próxima amostra estéreo a 44,1 kHz (zero sem tocar)
  void audioSample(int16_t& l, int16_t& r);
  bool playing() const { return playing_; }
  bool doorOpen() const { return door_; }
  bool doubleSpeed() const { return doubleSpeed_; }
  int sectorSize() const { return sectorSize_; }
  void sync(StateIO& io);

private:
  void execute();
  uint8_t stat() const;
  void reply(std::initializer_list<uint8_t> bytes, uint8_t extra = 0);

  cdrom::Disc* disc_ = nullptr;
  uint8_t cmd_[7] = {};
  int cmdLen_ = 0;
  std::deque<uint8_t> status_, pendingStatus_;
  std::deque<uint8_t> data_;
  int32_t delay_ = 0;  // ciclos até a resposta pendente aparecer
  bool spinning_ = false, door_ = false, doubleSpeed_ = false, success_ = false, playing_ = false;
  bool readDone_ = false;  // falta mandar a resposta da leitura quando a fila esvaziar
  uint32_t playLba_ = 0, playEnd_ = 0, playPos_ = 0;  // faixa de áudio: setor atual, fim (exclusivo), amostra
  uint8_t pcm_[cdrom::RAW_SECTOR] = {};               // setor de áudio em uso (relido pelo LBA)
  uint32_t pcmLba_ = 0xFFFFFFFF;
  int32_t sectorSize_ = 2048;
  uint32_t readLba_ = 0, readLeft_ = 0;
  int32_t readTimer_ = 0;
};

}  // namespace indigo::tdo
