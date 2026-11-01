// Chip DSP-1 dos cartuchos de SNES (Super Mario Kart, Pilotwings, Ballz 3D, Suzuka 8 Hours...).
// O chip é um NEC µPD7725 com um programa gravado de fábrica. Dois jeitos de rodar:
//   - com o arquivo do chip (dsp1b.rom, 8 KB, extraído do cartucho do usuário): o µPD7725 é emulado instrução
//     por instrução e roda o programa original (resultado exato);
//   - sem o arquivo: os comandos do DSP-1 são calculados direto (multiplicação, inverso, seno/cosseno, projeção
//     para o Mode 7, matrizes de atitude...), com as mesmas contas em ponto fixo descritas para o chip.
// Do lado do SNES são só dois registradores: DR (dados) e SR (situação).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::snes {

// ------------------------------------------------------------ NEC µPD7725
class Upd7725 {
public:
  // programa: 2048 palavras de 24 bits; dados: 1024 palavras de 16 bits (arquivo de 8 KB, little-endian)
  bool load(const std::vector<uint8_t>& firmware);
  void reset();
  void exec();  // uma instrução
  uint8_t readDR();
  void writeDR(uint8_t v);
  uint8_t readSR() const { return static_cast<uint8_t>(sr >> 8); }
  void sync(StateIO& io);

  // acesso direto (testes)
  uint32_t program[2048] = {};
  uint16_t data[1024] = {};
  uint16_t ram[256] = {};
  uint16_t pc = 0, rp = 0, dp = 0, a = 0, b = 0, tr = 0, trb = 0, dr = 0, sr = 0, k = 0, l = 0, m = 0, n = 0, so = 0;
  uint16_t stack[4] = {};
  uint8_t sp = 0;
  struct Flags { bool ov0 = false, ov1 = false, z = false, c = false, s0 = false, s1 = false; } fa, fb;

private:
  enum : uint16_t { RQM = 0x8000, DRS = 0x1000, DRC = 0x0400 };
  void execOp(uint32_t op);
  void execJp(uint32_t op);
  void execLd(uint32_t op);
  void dst(int d, uint16_t idb);
};

// ------------------------------------------------------------ DSP-1 (com ou sem o arquivo do chip)
class Dsp1 {
public:
  // firmware vazio = imitação dos comandos
  explicit Dsp1(const std::vector<uint8_t>& firmware);
  bool exact() const { return lle_; }
  void reset();
  void run(int masterClocks);  // o µPD7725 anda junto com o relógio do SNES (7,6 MHz)
  uint8_t read(bool status);
  void write(bool status, uint8_t v);
  void sync(StateIO& io);

private:
  // imitação: protocolo de bytes do chip
  void command(uint8_t c);
  void execute();
  void raster();
  int16_t in(int i) const { return static_cast<int16_t>(param_[i * 2] | (param_[i * 2 + 1] << 8)); }
  void out(int i, int16_t v) { output_[i * 2] = static_cast<uint8_t>(v); output_[i * 2 + 1] = static_cast<uint8_t>(v >> 8); }

  // contas do DSP-1 (ponto fixo de 16 bits, como no chip)
  void inverse(int16_t c, int16_t e, int16_t& ic, int16_t& ie) const;
  void normalize(int16_t m, int16_t& c, int16_t& e) const;
  void normalizeDouble(int32_t p, int16_t& c, int16_t& e) const;
  int16_t truncate(int16_t c, int16_t e) const;
  void parameter(int16_t fx, int16_t fy, int16_t fz, int16_t lfe, int16_t les, int16_t aas, int16_t azs);
  void rasterLine(int16_t vs, int16_t& an, int16_t& bn, int16_t& cn, int16_t& dn) const;
  void project(int16_t x, int16_t y, int16_t z, int16_t& h, int16_t& v, int16_t& mm) const;
  void target(int16_t h, int16_t v, int16_t& x, int16_t& y) const;
  void attitude(int16_t (&mat)[3][3], int16_t m, int16_t zr, int16_t yr, int16_t xr);

  bool lle_ = false;
  Upd7725 chip_;
  int64_t debt_ = 0;

  // estado da imitação
  uint8_t cmd_ = 0;
  bool waiting_ = true, firstParam_ = true;
  int inCount_ = 0, inIndex_ = 0, outCount_ = 0, outIndex_ = 0;
  uint8_t param_[16] = {}, output_[16] = {};
  int16_t rasterVs_ = 0;
  int16_t matA_[3][3] = {}, matB_[3][3] = {}, matC_[3][3] = {};
  // projeção (comando Parameter)
  int16_t sinAas_ = 0, cosAas_ = 0, sinAzs_ = 0, cosAzs_ = 0, sinAZS_ = 0, cosAZS_ = 0;
  int16_t nx_ = 0, ny_ = 0, nz_ = 0, gx_ = 0, gy_ = 0, gz_ = 0, cLes_ = 0, eLes_ = 0, gLes_ = 0;
  int16_t centreX_ = 0, centreY_ = 0, vOffset_ = 0, vPlaneC_ = 0, vPlaneE_ = 0;
  int16_t secC1_ = 0, secE1_ = 0, secC2_ = 0, secE2_ = 0;
};

}  // namespace indigo::snes
