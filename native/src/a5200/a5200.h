// Núcleo Atari 5200 (SuperSystem, 1982): CPU 6502C "Sally" (a mesma de nes/cpu.h, com modo decimal), ANTIC
// (lista de exibição, modos 2–F, LMS, saltos, JVB, DLI, rolagem fina horizontal e vertical, DMA de jogadores e
// mísseis, ciclos roubados da CPU, WSYNC, VCOUNT, NMI de DLI e de VBI), GTIA (jogadores e mísseis com tamanhos,
// prioridades pelas equações do chip, quinto jogador, cores múltiplas, colisões, modos GTIA de 16 tons, 9 cores
// e 16 matizes, gatilhos) e o POKEY comum com o 7800 (src/atari/pokey.h: som, temporizadores, teclado do
// controle e potenciômetros do joystick analógico).
//
// Relógio: 1,79 MHz; linha de 114 ciclos (228 pontos de cor), quadro de 262 linhas (NTSC; não existe 5200 PAL).
// A imagem mostra os pontos de cor 32 a 223 (a largura "wide" inteira) das linhas 8 a 247: 384 x 240.
//
// BIOS: obrigatória (5200.rom, 2 KB, do aparelho do usuário: firmware "a5200"), em $F800.
// Cartuchos (.a52, .bin, .car): 4 KB, 8 KB, 16 KB de um chip ou de dois chips (escolhido pelo código), 32 KB,
// 40 KB (Bounty Bob, bancos em $4FF6 e $5FF6) e os "super cartuchos" de 64 a 512 KB (bancos em $BFC0–$BFFF).
//
// Teclas: direcional = joystick analógico (extremos e centro); A = botão de cima (gatilho, TRIG0);
// B = botão de baixo (Shift/Break do POKEY); START = Start; SELECT = Pause; Y = tecla 1; L = *; R = #.
// Teclado completo: no PC, o teclado numérico (0–9, * e /, . = #) chega pelos bits KEY_KP*; no controle e na
// tela de toque, X é o "shift" do teclado (sozinho não faz nada; enquanto está apertado, o joystick fica no
// centro e os gatilhos soltos): X + direcional escolhe a tecla pela posição no teclado 3×4 do controle
// (↖ 1, ↑ 2, ↗ 3, ← 4, → 6, ↙ 7, ↓ 8, ↘ 9), X + A = 5, X + B = 0, X + Y = 1, X + L = *, X + R = #,
// X + START = Reset.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../atari/pokey.h"
#include "../core/core.h"
#include "../core/state.h"
#include "../nes/cpu.h"

namespace indigo::a5200 {

// cartucho de 5200: .a52, .car com tipo de 5200, ou .bin com tamanho e vetor de partida de 5200
bool isA5200Rom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
// BIOS do usuário: 2 KB e "5200" no nome do arquivo
bool isA5200Bios(const std::vector<uint8_t>& data, const std::string& nomeMinusculo);

// teclas do controle (código KBCODE, como o POKEY recebe)
enum Keypad : int {
  KP_NONE = -1, KP_START = 0x39, KP_PAUSE = 0x31, KP_RESET = 0x29, KP_0 = 0x25, KP_1 = 0x3F, KP_2 = 0x3D,
  KP_3 = 0x3B, KP_4 = 0x37, KP_5 = 0x35, KP_6 = 0x33, KP_7 = 0x2F, KP_8 = 0x2D, KP_9 = 0x2B, KP_HASH = 0x23,
  KP_STAR = 0x27,
};

// ------------------------------------------------------------------ cartucho
struct Cart {
  enum Kind : uint8_t { K4, K8, NS16, EE16, K32, BB40, SUPER };
  Kind kind = K32;
  std::vector<uint8_t> rom;
  std::string title;
  uint8_t bb[2] = {};   // Bounty Bob: banco de $4000 e de $5000
  uint8_t super = 0;    // super cartucho: banco de 32 KB
  const uint8_t* page[8] = {};  // $4000–$BFFF em páginas de 4 KB (nullptr = vazio)

  bool load(const std::vector<uint8_t>& file, const std::string& nomeMinusculo, std::string& erro);
  void reset();
  void map();
  uint8_t read(uint16_t a) const { const uint8_t* p = page[(a >> 12) - 4]; return p ? p[a & 0xFFF] : 0xFF; }
  void access(uint16_t a);  // troca de banco por leitura ou escrita
  bool switches(uint16_t a) const {
    return (kind == BB40 && (a & 0xEFF0) == 0x4FF0 && (a & 0xF) >= 6 && (a & 0xF) <= 9) ||
           (kind == SUPER && a >= 0xBFC0);
  }
  void sync(StateIO& io);
};

// ------------------------------------------------------------------ máquina
class A5200 : public nes::CpuBus {
public:
  A5200(Cart c, const std::vector<uint8_t>& bios);
  Cart cart;
  nes::CPU cpu;
  static constexpr int W = 384, H = 240;
  static constexpr int LINES = 262, CYCLES = 114;
  std::vector<uint32_t> fb = std::vector<uint32_t>(size_t(W) * H, 0xFF000000u);

  void reset(bool power);
  void runFrame();
  void setKeys(uint32_t mask);
  // entrada direta (testes): potenciômetros do controle 1, gatilhos e teclado
  void setInput(uint8_t potX, uint8_t potY, bool trig, bool shift, int key);
  void setSampleRate(int rate);
  size_t audioAvailable() const { return (wpos + kBuf - rpos) % kBuf; }
  void pullAudio(float* l, float* r, size_t n);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  uint8_t read(uint16_t a) override;
  void write(uint16_t a, uint8_t v) override;
  uint8_t peek(uint16_t a) const;       // sem efeito colateral (testes)
  void poke(uint16_t a, uint8_t v);     // escreve como a CPU, sem gastar tempo (testes)

  // números para os testes
  uint64_t frameCount = 0;
  uint64_t clk = 0;            // ciclos desde que ligou
  uint64_t cpuCycles = 0;      // ciclos usados pela CPU
  int stolenLast = 0;          // ciclos roubados na última linha completa
  uint8_t stolenLine[262] = {};  // ciclos roubados em cada linha do último quadro
  static uint32_t color(uint8_t c);  // cor do GTIA na paleta (0xAABBGGRR)
  int line = 0, cyc = 0;

private:
  // tempo
  void tick();
  void beginCycle();
  void endCycle();
  void startLine();
  void endLine();
  // memória
  uint8_t memRead(uint16_t a);
  void memWrite(uint16_t a, uint8_t v);
  uint8_t dmaRead(uint16_t a) const;
  // ANTIC
  uint8_t anticRead(int r);
  void anticWrite(int r, uint8_t v);
  void anticLine();
  void buildLine(int fetchW, int x0);
  void stealRange(int from, int count, int step);
  // GTIA
  uint8_t gtiaRead(int r);
  void gtiaWrite(int r, uint8_t v);
  void renderTo(int cc);
  // som
  void audioSync();
  void integrate(int level, uint64_t dt);
  void sync(StateIO& io);

  uint8_t ram[0x4000] = {};
  std::vector<uint8_t> bios;
  uint8_t openBus = 0xFF;

  // linha atual
  uint8_t steal[CYCLES + 1] = {};  // 1 = ciclo tomado pelo ANTIC (uint8_t: o estado gravado não vira bool inválido)
  bool wsync = false, nmiLine = false, frameDone = false;
  int dliAt = -1, vbiAt = -1;  // ciclo em que o NMI acontece nesta linha (-1 = nenhum)

  // ANTIC
  uint8_t dmactl = 0, chactl = 0, hscrol = 0, vscrol = 0, pmbase = 0, chbase = 0, nmien = 0, nmist = 0x1F;
  uint16_t dlist = 0, memScan = 0;
  uint8_t ir = 0;          // instrução atual da lista
  uint8_t dctr = 0;        // linha dentro da linha de modo
  bool needIr = true, prevVs = false, jvb = false, firstRow = true;
  uint8_t blankLeft = 0;   // linhas em branco que faltam
  uint8_t names[64] = {};  // códigos de caractere (ou bytes de gráfico) da linha de modo
  // saída do ANTIC para a linha: por ponto de cor, 0 = fundo, 1–4 = PF0–PF3, 5 = alta resolução (PF2 com os
  // meios pontos de hr acesos em PF1)
  uint8_t pf[232] = {}, hr[232] = {};
  bool lineVisible = false;

  // GTIA
  uint8_t hposp[4] = {}, hposm[4] = {}, sizep[4] = {}, sizem = 0, grafp[4] = {}, grafm = 0;
  uint8_t colpm[4] = {}, colpf[4] = {}, colbk = 0, prior = 0, vdelay = 0, gractl = 0, consol = 0;
  uint8_t mpf[4] = {}, ppf[4] = {}, mpl[4] = {}, ppl[4] = {};
  uint8_t trigLatch = 0;   // bits 0–3: gatilho travado (GRACTL bit 2)
  int rendered = 0;        // pontos de cor já desenhados na linha atual

  // controles
  uint8_t trig = 0;        // bit 0..3 = gatilho apertado
  bool shiftKey = false;
  int keyCode = KP_NONE;

  // som
  atari::Pokey pokey;
  uint64_t audioM = 0;
  int audioLevel = 0;
  double samplePeriod = 1, sampleT = 0, sampleV = 0, capV = 0;
  static constexpr size_t kBuf = 16384;
  std::vector<float> buf = std::vector<float>(kBuf);
  size_t wpos = 0, rpos = 0;
};

class A5200Core : public Core {
public:
  A5200Core(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo, const std::vector<uint8_t>* save);
  A5200& m() { return *m_; }

  std::string system() const override { return "a5200"; }
  std::string title() const override { return m_->cart.title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(false); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  std::string note() const override {
    return " · teclado do controle: teclado numérico do PC, ou segure X + direcional (posição da tecla), A = 5, B = 0";
  }
  const uint32_t* framebuffer() const override { return m_->fb.data(); }
  int width() const override { return A5200::W; }
  int height() const override { return A5200::H; }
  double fps() const override { return 1789772.5 / (114.0 * 262); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return false; }
  bool saveDirty() const override { return false; }
  std::vector<uint8_t> exportSave() override { return {}; }
  void importSave(const std::vector<uint8_t>&) override {}
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<A5200> m_;
};

}  // namespace indigo::a5200
