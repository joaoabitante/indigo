// Núcleo Tiger Game.com (1997). O comportamento do hardware segue o driver gamecom do MAME (Wilbert Pol e
// Robbbert, BSD-3-Clause; ver TERCEIROS.md) e o formato do cabeçalho dos cartuchos, o rom-properties
// (GerbilSoft) e o GameComHDK (Tpot) — documentos lidos como referência; o código é do Índigo.
//
// Hardware emulado:
//   - CPU Sharp SM8521 (núcleo SM8500, sm8500.h) a 11,0592 MHz / 2 = 5,5296 MHz;
//   - memória: $0000-$03FF RAM interna com os registradores de E/S em $10-$7F; $1000-$1FFF ROM interna (4 KB);
//     $2000-$9FFF quatro janelas de 8 KB (MMU1-MMU4): valor < $20 = banco da ROM do sistema (256 KB), senão
//     banco do cartucho (2 MB, com espelho, como no MAME); $A000-$DFFF vídeo (16 KB, só escrita, lê 0);
//     $E000-$FFFF RAM estendida (8 KB);
//   - LCD 200x160 em 4 tons, duas páginas de 8 KB (LCDC bit 6), 40 bytes por coluna (4 pixels por byte);
//     paleta de contraste pelos bits 4-5 do LCDC; interrupção LCDC no começo do retraço;
//   - DMA de blocos (VRAM->VRAM, ROM->VRAM, RAM estendida<->VRAM) com paleta de 4 cores e transparência,
//     feito quando a CPU para em HALT (como no MAME) ou depois de um tempo se ela não parar;
//   - timers 0 e 1 (pré-divisor + contador), relógio de 1 s / 1 min (CLKT);
//   - som: dois canais de onda de 32 amostras de 4 bits (SG0/SG1), um de ruído (SG2, aproximado: o MAME não
//     emula) e o DAC de 8 bits (SGDA); saída mono;
//   - teclas e tela de toque pela multiplexação de P0/P1/P2 (grade de 13 x 10 áreas de 16x16 pixels).
// Não emulados: porta serial (UART), o segundo cartucho, watchdog, troca de relógio (STOP).
//
// BIOS: as duas ROMs do console da pessoa são obrigatórias: a interna do SM8521 (4 KB, "internal.bin",
// firmware "gamecom_int") e a do sistema (256 KB, "external.bin", firmware "gamecom_ext").
//
// Teclas (bits de Key em core.h): direcional = setas, A = KEY_A, B = KEY_B, C = KEY_X, D = KEY_Y,
// Menu = KEY_SELECT, Pause = KEY_START, Sound = KEY_R. KEY_L liga/desliga a caneta: com ela ligada, o
// direcional move o cursor e KEY_A toca na tela. setPointer() toca direto (mouse/toque do app).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "sm8500.h"

namespace indigo::gamecom {

constexpr int SCREEN_W = 200, SCREEN_H = 160;
constexpr int64_t CPU_HZ = 5529600;
constexpr int LINE_CYCLES = 445;              // 200 colunas + 8 linhas de retraço
constexpr int LINES = 208;
constexpr int FRAME_CYCLES = LINE_CYCLES * LINES;  // 92560 ciclos ≈ 59,74 Hz
constexpr size_t INT_ROM_SIZE = 0x1000, EXT_ROM_SIZE = 0x40000, CART_SPACE = 0x200000;

// cartucho: "TigerDMGC" no cabeçalho (em 0 ou em $40000) ou extensão .tgc
bool isGamecomRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
// ROM do console: "gamecom_int" (4 KB), "gamecom_ext" (256 KB) ou "" se não for. Reconhece os dumps conhecidos
// pelo CRC32; outro arquivo do tamanho certo vale se o nome tiver "gamecom"/"game.com"/"game_com".
std::string gamecomFirmwareKind(const std::vector<uint8_t>& b, const std::string& nomeMinusculo);
uint32_t crc32(const uint8_t* p, size_t n);

class GameCom : public Sm8500Bus {
public:
  GameCom(const std::vector<uint8_t>& cart, const std::vector<uint8_t>& intRom, const std::vector<uint8_t>& extRom);

  Sm8500 cpu{this};
  uint32_t fb[SCREEN_W * SCREEN_H] = {};

  void reset();
  void runFrame();
  void setKeys(uint32_t mask);
  void setPointer(int x, int y, bool down);
  std::string title() const;

  void setSampleRate(int rate);
  size_t audioAvailable() const { return (wpos + AUDIO_BUF - rpos) % AUDIO_BUF; }
  void pullAudio(float* l, float* r, size_t n);

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // Sm8500Bus
  uint8_t read(uint16_t a) override;
  void write(uint16_t a, uint8_t v) override;
  void tick(int cycles, bool halted) override;

  // para os testes
  uint64_t cycleCount() const { return cycles; }
  uint8_t peekRam(int a) const { return ram[a & 0x3FF]; }
  uint8_t peekVram(int a) const { return vram[a & 0x3FFF]; }
  uint8_t peekExt(int a) const { return ext[a & 0x1FFF]; }
  void pokeTimers(uint32_t pre, uint32_t preMax) { for (auto& t : tm) { t.on = true; t.pre = pre; t.preMax = preMax; } }
  void pokeSoundAcc(uint32_t v) { sg[0].acc = sg[1].acc = noiseAcc = v; }
  uint32_t timerPre(int i) const { return tm[i & 1].pre; }
  uint32_t soundAcc(int i) const { return i < 2 ? sg[i].acc : noiseAcc; }

  // RAM estendida de 8 KB (agenda e recordes): vira o save do jogo
  bool extDirty = false;
  std::vector<uint8_t> exportExt();
  void importExt(const std::vector<uint8_t>& b);
  int line() const { return lineNo; }
  uint64_t frames() const { return frameCount; }
  int dmaCount() const { return dmaDone; }
  float lastSample() const { return lastOut; }

private:
  // ---------------------------------------------------------------- memória
  std::vector<uint8_t> cart;   // 2 MB com espelho
  std::string cartTitle;
  uint8_t intRom[INT_ROM_SIZE] = {};
  std::vector<uint8_t> extRom;  // 256 KB
  uint8_t ram[0x400] = {};
  uint8_t vram[0x4000] = {};
  uint8_t ext[0x2000] = {};
  uint32_t bankBase[5] = {};    // janelas 1-4: deslocamento (bit 31 = cartucho)
  bool cartSelected = false;
  uint8_t muxLo = 0xFF, muxHi = 0xFF;  // o que a CPU escreveu em P1/P2 (seleção das teclas/colunas)

  // ---------------------------------------------------------------- tempo e periféricos
  uint64_t cycles = 0, frameCount = 0;
  int lineCycle = 0, lineNo = 0;
  bool frameDone = false;
  struct Timer { bool on = false; uint32_t pre = 0, preMax = 1; uint8_t max = 0; };
  Timer tm[2];
  bool clkOn = false;
  uint32_t clkPeriod = 0, clkCount = 0;
  bool dmaPending = false;
  uint32_t dmaWait = 0;
  int dmaDone = 0;

  // entrada
  uint32_t keys = 0, prevKeys = 0;
  bool stylusMode = false;
  int curX = 100, curY = 80, holdFrames = 0;
  int penX = 0, penY = 0;
  bool penDown = false, ptrDown = false;
  int ptrX = 0, ptrY = 0;

  // som
  struct Tone { uint32_t acc = 0; uint8_t pos = 0; };
  Tone sg[2];
  uint32_t noiseAcc = 0;
  uint16_t lfsr = 0x7FFF;
  uint8_t dacLevel = 0x80;
  static constexpr size_t AUDIO_BUF = 16384;
  std::vector<float> bufL, bufR;
  size_t wpos = 0, rpos = 0;
  double cyclesPerSample = double(CPU_HZ) / 48000.0;
  double sampleAcc = 0;
  double hpCap = 0;
  float lastOut = 0;

  // ---------------------------------------------------------------- funções
  void ioWrite(uint8_t r, uint8_t v);
  void setMmu(int n, uint8_t v);
  void inputMux();
  void stylusColumn(int col);
  void runDma();
  void drawLine(int l);
  void endFrame();
  void soundStep(int c);
  void emitSample();
  void syncState(StateIO& s);
};

class GameComCore : public Core {
public:
  GameComCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  GameCom& gc() { return *m_; }

  std::string system() const override { return "gamecom"; }
  std::string title() const override { return m_->title(); }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  void setPointer(int x, int y, bool down) override { m_->setPointer(x, y, down); }
  const uint32_t* framebuffer() const override { return m_->fb; }
  int width() const override { return SCREEN_W; }
  int height() const override { return SCREEN_H; }
  double fps() const override { return double(CPU_HZ) / FRAME_CYCLES; }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return m_->extDirty; }
  std::vector<uint8_t> exportSave() override { return m_->exportExt(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importExt(b); }
  std::string note() const override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<GameCom> m_;
};

}  // namespace indigo::gamecom
