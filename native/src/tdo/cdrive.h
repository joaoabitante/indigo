// Unidade de CD do 3DO (a unidade Panasonic/MKE ligada ao XBUS, dispositivo 0). A CPU manda pacotes de 7 bytes
// de comando pela porta de comando/estado e lê a resposta (o byte do comando ecoado, os dados e o byte de estado
// da unidade). Os setores lidos (2048 bytes, Mode 1) vão para a fila de dados, lida pela CPU byte a byte ou pelo
// DMA do XBUS. Velocidade dupla: 150 setores por segundo.
//
// Comandos (byte 0): 0x01 busca (MSF), 0x02 girar, 0x03 parar, 0x04 diagnóstico, 0x06 abrir a gaveta,
// 0x07 fechar, 0x08 abortar, 0x09 modo (byte 1 bit 7 = velocidade dupla), 0x0A reiniciar, 0x0B esvaziar a fila,
// 0x10 ler (MSF em binário, bytes 1-3; quantidade de setores nos bytes 5-6), 0x83 identificação,
// 0x8B sessão, 0x8E informações do disco, 0x8F TOC de uma faixa (byte 1).
// Byte de estado: 0x80 gaveta fechada, 0x40 disco presente, 0x20 girando, 0x10 erro, 0x02 velocidade dupla,
// 0x01 pronta.
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
  enum : uint8_t { ST_DOOR = 0x80, ST_DISC = 0x40, ST_SPIN = 0x20, ST_ERR = 0x10, ST_2X = 0x02, ST_READY = 0x01 };
  static constexpr int CYCLES_PER_SECTOR_2X = 12500000 / 150;
  static constexpr int REPLY_DELAY = 12500;  // ~1 ms para responder a um comando
  static constexpr size_t FIFO_MAX = 2048 * 8;

  void setDisc(cdrom::Disc* d) { disc_ = d; }
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
  bool doorOpen() const { return door_; }
  void sync(StateIO& io);

private:
  void execute();
  uint8_t stat() const;
  void reply(std::initializer_list<uint8_t> bytes);

  cdrom::Disc* disc_ = nullptr;
  uint8_t cmd_[7] = {};
  int cmdLen_ = 0;
  std::deque<uint8_t> status_, pendingStatus_;
  std::deque<uint8_t> data_;
  int32_t delay_ = 0;      // ciclos até a resposta pendente aparecer
  bool spinning_ = false, door_ = false, doubleSpeed_ = true, error_ = false;
  uint32_t readLba_ = 0, readLeft_ = 0;
  int32_t readTimer_ = 0;
};

}  // namespace indigo::tdo
