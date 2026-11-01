// Núcleo do Vectrex (GCE/Milton Bradley, 1982): CPU 6809 a 1,5 MHz, VIA 6522 (timers, registrador de
// deslocamento, portas), PSG AY-3-8912, DAC de 8 bits ligado aos integradores X/Y e um tubo vetorial.
//
// Memória: 0000-7FFF cartucho (até 64 KB com troca de banco pela linha PB6 da VIA), C800-CFFF RAM de 1 KB
// (espelhada), D000-D7FF VIA (espelhada a cada 16 bytes; D800-DFFF seleciona RAM e VIA juntas),
// E000-FFFF BIOS de 8 KB (Mine Storm + rotinas do sistema). A BIOS é obrigatória: firmware("vectrex").
// Se o próprio arquivo aberto for a BIOS (8 KB, reset em F000, vetores de interrupção na RAM), ela é usada
// como BIOS e o console liga sem cartucho (Mine Storm).
//
// Feixe (modelo do circuito analógico, passo de 1 ciclo de CPU):
//   - porta A da VIA -> DAC (valor com sinal). O DAC vai sempre para o integrador X; pelo multiplexador
//     (PB0 = 0 liga, PB1-PB2 escolhem) vai para o "sample and hold" de Y (0), da referência/ZERO de X e Y (1),
//     do brilho Z (2) ou do som (3);
//   - RAMP (PB7, ou a saída PB7 do timer 1 quando ACR bit 7 = 1), ativo em 0: enquanto ativo, a cada ciclo
//     x += DAC - ref e y += Y - ref (unidades do integrador; a tela tem 33000 x 41000 unidades = 330 x 410
//     pixels, mesma calibração do vecx);
//   - ZERO (CA2 em 0, PCR = 110): volta o feixe ao centro; BLANK (CB2 em 0): apaga o feixe. Com o registrador
//     de deslocamento ligado (ACR modo 110), o CB2 vem dele (1 bit a cada 2 ciclos, 18 ciclos por byte), que é
//     como a BIOS desenha linhas tracejadas e pontos.
//   Cada trecho reto aceso vira uma linha suavizada (Xiaolin Wu) num acumulador de "fósforo" com brilho
//   proporcional a Z e ao tempo que o feixe passa em cada pixel; o acumulador decai a cada quadro
//   (persistência) e é convertido em branco levemente azulado.
//   Gancho para overlays coloridos: tintFor() (vectrex.cpp) decide a cor de cada pixel; hoje devolve sempre
//   o mesmo tom. Um overlay seria uma imagem 330x410 multiplicando essa cor.
//
// Quadro: o Vectrex não tem varredura fixa; a BIOS (Wait_Recal) programa o timer 2 com 30000 ciclos
// (0x7530) a cada quadro = 50 Hz. Aqui cada runFrame roda exatamente 30000 ciclos (1,5 MHz / 30000 = 50 Hz),
// e a tela mostrada é o que o feixe desenhou nesse intervalo somado ao resto do fósforo.
//
// Teclas: direções = joystick 1 analógico nos valores máximos (8 direções); A, B, X, Y = botões 1, 2, 3, 4
// do controle 1. O controle 2 fica parado.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "m6809.h"

namespace indigo::vectrex {

bool isVectrexRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
bool isVectrexBios(const std::vector<uint8_t>& rom);

// PSG AY-3-8912 a 1,5 MHz (o Vectrex usa a porta A de E/S para os botões)
class AY {
public:
  void reset();
  void write(int reg, uint8_t v);
  uint8_t read(int reg, uint8_t portIn) const;
  void tick8();  // avança 8 ciclos do relógio (um passo do divisor dos tons)
  float level() const { return out; }
  void sync(StateIO& io);

  uint8_t r[16] = {};

private:
  int toneCnt[3] = {}, toneOut[3] = {}, noiseCnt = 0, envCnt = 0, envStep = 0;
  uint32_t lfsr = 1;
  bool envHold = false, envAlt = false, half = false;
  float out = 0;
};

// VIA 6522 (só o que o Vectrex liga: portas, 2 timers, registrador de deslocamento, interrupções)
class VIA {
public:
  void reset();
  void tick();  // 1 ciclo
  uint8_t read(int reg, uint8_t pbIn, uint8_t paIn);
  void write(int reg, uint8_t v);
  bool irq() const { return (ifr & ier & 0x7F) != 0; }
  uint8_t pbPins() const;  // saídas da porta B (entradas ficam em 1), com PB7 do timer 1
  bool ca2Low() const { return (pcr & 0x0E) == 0x0C; }
  bool cb2High() const;    // false = feixe apagado
  void sync(StateIO& io);

  uint8_t orb = 0, ora = 0, ddrb = 0, ddra = 0, acr = 0, pcr = 0, ifr = 0, ier = 0;
  uint8_t t1ll = 0, t1lh = 0, t2ll = 0, sr = 0;
  uint16_t t1c = 0xFFFF, t2c = 0xFFFF;
  bool t1armed = false, t2armed = false, t1pb7 = true, srBit = false;
  int srLeft = 0;

private:
  void srStep();
};

class VectrexCore final : public M6809Bus, public Core {
public:
  static constexpr int W = 330, H = 410;
  static constexpr int CYCLES_PER_FRAME = 30000;

  VectrexCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);

  // Core
  std::string system() const override { return "vectrex"; }
  std::string title() const override { return title_; }
  void runFrame() override;
  void reset() override;
  void setKeys(uint32_t mask) override { keys_ = mask; }
  const uint32_t* framebuffer() const override { return fb_.data(); }
  int width() const override { return W; }
  int height() const override { return H; }
  double fps() const override { return 1500000.0 / CYCLES_PER_FRAME; }
  void setSampleRate(int rate) override;
  size_t audioAvailable() const override { return (wpos_ + BUF - rpos_) % BUF; }
  void pullAudio(float* left, float* right, size_t n) override;
  bool hasSave() const override { return false; }
  bool saveDirty() const override { return false; }
  std::vector<uint8_t> exportSave() override { return {}; }
  void importSave(const std::vector<uint8_t>&) override {}
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& state) override;

  // M6809Bus
  uint8_t read(uint16_t a) override;
  void write(uint16_t a, uint8_t v) override;

  // para os testes
  void stepInstr();
  bool accessAtStart = false;  // só para comparar com o vecx: E/S no início da instrução  // uma instrução da CPU (com os dispositivos acompanhando)
  M6809& cpu() { return cpu_; }
  VIA& via() { return via_; }
  uint64_t cycles() const { return clock_; }
  const std::vector<uint8_t>& ram() const { return ram_; }
  const uint8_t* ayRegs() const { return ay_.r; }

private:
  void catchUp(uint64_t t);
  uint64_t ioTime() const;
  void tick();
  void analogInputs();       // recalcula DAC/multiplexador/AY depois de mudar porta A/B
  void flushSegment();
  void drawLine(float x0, float y0, float x1, float y1, float inten);
  void plot(int x, int y, float v);
  void present();
  void sync(StateIO& io);
  uint8_t joyPot(int sel) const;

  M6809 cpu_;
  VIA via_;
  AY ay_;
  std::vector<uint8_t> bios_, cart_, ram_;
  std::string title_;
  int bank_ = 0;
  bool bankInvert_ = false;
  uint32_t keys_ = 0;

  uint64_t clock_ = 0, devClock_ = 0, instrStart_ = 0, frameEnd_ = 0;

  // analógico
  int8_t dac_ = 0, ySH_ = 0, ref_ = 0, zSH_ = 0, soundSH_ = 0, pot_ = 0;
  uint8_t ayLatch_ = 0, ayMode_ = 0;
  int32_t bx_ = 0, by_ = 0;  // posição do feixe (unidades do integrador; centro = 0, y para cima)
  // trecho em andamento
  bool segOn_ = false;
  int32_t segX_ = 0, segY_ = 0, segCycles_ = 0, segDX_ = 0, segDY_ = 0, segZ_ = 0;

  std::vector<uint16_t> glow_;  // fósforo: 8192 = brilho cheio
  std::vector<uint32_t> fb_;

  // som
  static constexpr size_t BUF = 16384;
  std::vector<float> buf_;
  size_t wpos_ = 0, rpos_ = 0;
  double ticksPerSample_ = 0, frac_ = 0, acc_ = 0, cap_ = 0;
  int accN_ = 0, sub8_ = 0;
};

}  // namespace indigo::vectrex
