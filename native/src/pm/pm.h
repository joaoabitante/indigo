// Núcleo Pokémon Mini (Nintendo, 2001). Próprio do Índigo nativo; a régua de comportamento é a documentação da
// comunidade (pokemon-mini.net / github.com/pokemon-mini/pm-dev-docs) e o emulador de referência PokeMini
// (JustBurn), usado só como guia — o código aqui é escrito do zero.
//
// Hardware emulado:
//   - CPU Epson S1C88 ("Minx") a 4 MHz (cpu.h), BIOS de 4 KB, 4 KB de RAM, registradores em $2000-$20FF e
//     cartucho de até 2 MB (espelhado a partir de $200000);
//   - PRC (processador de vídeo): mapa de blocos 8x8 (4 tamanhos, rolagem), 24 sprites 16x16 com máscara, cópia
//     do quadro de $1000-$12FF para o LCD; contador de 65 passos de 855 ciclos (quadro de 55575 ciclos ≈ 71,98
//     Hz) e divisor de quadros (/2 a /12); a CPU fica parada enquanto o PRC desenha/copia;
//   - controlador do LCD (estilo SED1565, 96x64 visíveis) com acesso direto pelas portas $20FE/$20FF;
//   - os tons de cinza: os jogos piscam pixels a cada quadro; a tela mostra a média dos dois últimos quadros
//     enviados ao LCD (3 tons: claro, médio, escuro), como a persistência do LCD de verdade;
//   - timers 1, 2 e 3 (8 ou 16 bits, oscilador de 4 MHz ou de 32768 Hz, pré-divisores), timer de 256 Hz com as
//     interrupções de 32/8/2/1 Hz, contador de segundos (relógio), interrupções com prioridade;
//   - som: o timer 3 gera um PWM (saída alta quando contador <= pivô), volume 0/50/100%, filtro passa-alta
//     (alto-falante piezo); o som é mono (sai igual nos dois lados);
//   - EEPROM interna de 8 KB (24C64, I2C nas portas $2060/$2061): é o "save" (os jogos gravam nela);
//   - motor de vibração e infravermelho: ignorados (o infravermelho nunca recebe nada).
//
// BIOS: indigo::firmware("pm") (o bios.min original, 4 KB) se fornecido; senão a FreeBIOS livre (freebios.cpp).
//
// Teclas (bits de Key em core.h):
//   A = KEY_A, B = KEY_B, C = KEY_R ou KEY_X, direcional = setas, Power = KEY_SELECT,
//   sensor de choque (sacudir o aparelho) = KEY_L.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "cpu.h"

namespace indigo::pm {

// .min pela extensão, ou o cabeçalho "MN" em $2100 + "NINTENDO" em $21A4
bool isPMRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

// BIOS livre embutida (freebios.cpp), 4096 bytes
extern const uint8_t FREEBIOS[4096];

constexpr int SCREEN_W = 96, SCREEN_H = 64;
constexpr int64_t CPU_HZ = 4000000;
constexpr int PRC_STEP = 855;               // ciclos por passo do contador do PRC
constexpr int FRAME_CYCLES = 65 * PRC_STEP;  // 55575 ciclos por quadro do PRC

class PM : public MinxBus {
public:
  PM(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& bios);

  Minx cpu;
  uint32_t fb[SCREEN_W * SCREEN_H] = {};
  bool eepromDirty = false;
  bool usingFreeBios = true;

  void reset(bool hard);
  void runFrame();
  void setKeys(uint32_t mask);
  std::string title() const;

  std::vector<uint8_t> eeprom;  // 8 KB
  void importEeprom(const std::vector<uint8_t>& b);

  // som
  void setSampleRate(int rate);
  size_t audioAvailable() const { return (wpos + AUDIO_BUF - rpos) % AUDIO_BUF; }
  void pullAudio(float* l, float* r, size_t n);

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // MinxBus
  uint8_t read(uint32_t addr) override;
  void write(uint32_t addr, uint8_t v) override;
  void irqCheck() override;

  // para os testes
  uint64_t cycleCount() const { return cycles; }
  const uint8_t* lcdImage() const { return lcdCur; }  // 0/1 por pixel, a última imagem do LCD
  uint8_t peekRam(int a) const { return ram[a & 0xFFF]; }

private:
  struct Timer {
    uint8_t scale = 0, osc = 0, ctrlL = 0, ctrlH = 0, preL = 0xFF, preH = 0xFF, pvtL = 0, pvtH = 0;
    uint8_t cntL = 0, cntH = 0;
  };

  // ---------------------------------------------------------------- memória
  std::vector<uint8_t> rom;
  uint32_t romMask = 0;
  uint8_t bios[4096] = {};
  uint8_t ram[4096] = {};
  uint8_t io[256] = {};  // registradores "simples" (lidos como gravados)

  // ---------------------------------------------------------------- tempo
  uint64_t cycles = 0;       // ciclos de 4 MHz desde que ligou
  uint64_t frameStart = 0;   // ciclo em que o contador do PRC valeu 1 neste quadro
  uint64_t stallUntil = 0;   // CPU parada (PRC usando o barramento) até este ciclo
  uint64_t timerSynced = 0;  // timers atualizados até este ciclo
  uint64_t miscSynced = 0;   // timer de 256 Hz e contador de segundos atualizados até este ciclo
  bool frameDone = false;
  bool resched = false;      // algo mudou: recalcular o próximo evento

  // ---------------------------------------------------------------- periféricos
  Timer tm[3];
  uint16_t pivot3 = 0;
  uint8_t t256 = 0;          // contador de 256 Hz
  uint32_t seconds = 0;      // contador de segundos (24 bits)
  uint32_t keys = 0;         // máscara atual (bits de Key)
  bool shockPrev = false;

  // PRC
  uint8_t prcState = 0;      // 0 nada feito, 1 desenhou, 2 copiou (no quadro atual)
  uint8_t prcRateMatch = 0x10;
  uint8_t mapPX = 0, mapPY = 0;

  // LCD (controlador)
  uint8_t gdram[9][132] = {};
  uint8_t lcdCol = 0, lcdPage = 0, lcdStart = 0, lcdContrast = 0x1F;
  bool lcdSetContrast = false, lcdSegRev = false, lcdMaxContrast = false, lcdAllOn = false, lcdInvert = false;
  bool lcdOn = false, lcdRowRev = false, lcdRmw = false, lcdDummy = true;
  uint8_t lcdRmwCol = 0, lcdReadLatch = 0x40;
  bool lcdWritten = false;   // a GDRAM mudou desde a última atualização da tela
  uint8_t lcdPrev[SCREEN_W * SCREEN_H] = {}, lcdCur[SCREEN_W * SCREEN_H] = {};

  // EEPROM (I2C)
  uint8_t i2cLast = 0;       // nível das linhas SDA ($04) e SCL ($08) vindas da CPU
  uint8_t i2cState = 0;      // 0 parado, 1 recebendo, 2 enviando, 3 esperando o "stop"
  uint8_t i2cBit = 0, i2cByte = 0, i2cPhase = 0, i2cOut = 1;
  uint16_t i2cAddr = 0;

  // som
  static constexpr size_t AUDIO_BUF = 16384;
  std::vector<float> bufL, bufR;
  size_t wpos = 0, rpos = 0;
  double cyclesPerSample = CPU_HZ / 48000.0;
  double nextSample = 0;     // ciclo da próxima amostra
  double accHigh = 0, accTotal = 0, hpCap = 0;

  // ---------------------------------------------------------------- funções
  uint8_t ioRead(uint8_t r);
  void ioWrite(uint8_t r, uint8_t v);
  void sync();                       // tudo em dia até "cycles"
  void syncTimers(uint64_t t);
  void syncAudio(uint64_t t);
  uint64_t nextEvent();
  void setIrq(int n);
  void timerCtrl(int t, bool hi, uint8_t v);
  bool halfRunning(int t, bool hi) const;
  uint32_t halfDivider(int t, bool hi, bool& osc2) const;
  uint64_t tickCount(uint64_t t, uint32_t div, bool osc2) const;
  uint64_t tickTime(uint64_t tick, uint32_t div, bool osc2) const;
  uint64_t eventTicks(int t, bool hi) const;
  int prcCount() const;
  void prcRender();
  void prcCopy();
  void frameEnd();
  void lcdCtrl(uint8_t v);
  void lcdData(uint8_t v);
  uint8_t lcdReadData();
  void lcdRefresh(bool prcFrame);
  void i2cUpdate();
  uint8_t ioPortRead();
  void writeRtcStamp();
  void emitSample();
  void syncState(StateIO& io);
};

class PMCore : public Core {
public:
  PMCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  PM& pm() { return *m_; }

  std::string system() const override { return "pm"; }
  std::string title() const override { return m_->title(); }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(false); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->fb; }
  int width() const override { return SCREEN_W; }
  int height() const override { return SCREEN_H; }
  double fps() const override { return double(CPU_HZ) / FRAME_CYCLES; }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return true; }  // a EEPROM é do aparelho: qualquer jogo pode gravar
  bool saveDirty() const override { return m_->eepromDirty; }
  std::vector<uint8_t> exportSave() override { m_->eepromDirty = false; return m_->eeprom; }
  void importSave(const std::vector<uint8_t>& b) override { m_->importEeprom(b); }
  std::string note() const override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<PM> m_;
};

}  // namespace indigo::pm
