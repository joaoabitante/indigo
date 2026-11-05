// Núcleo Atari 2600 (VCS): CPU 6507 (o 6502 do NES, nes/cpu.h, com o modo decimal ligado e 13 linhas de
// endereço), TIA ponto a ponto (cada ciclo de cor: playfield, 2 jogadores, 2 mísseis, bola, HMOVE com as barras
// pretas, colisões, VDEL, NUSIZ, RESPx no meio da linha, WSYNC/RSYNC, som com os 2 canais e seus polinômios),
// RIOT 6532 (RAM de 128 bytes, temporizador e entradas) e os bancos de cartucho mais usados.
//
// Régua de comportamento: o Stella (emulador de referência do 2600) e as notas de hardware do TIA de Andrew
// Towers; o código é próprio. As telas são conferidas contra o Stella pela ferramenta tools/t7_a2600.cpp.
//
// Tela: 320 x 228 (NTSC) ou 320 x 274 (PAL), cada ponto do TIA (160 por linha) vira 2 pixels na largura, para
// a proporção ficar parecida com a da TV. A janela começa 23 (NTSC) / 32 (PAL) linhas depois do fim do VSYNC,
// como no Stella, então jogos que mudam o número de linhas por quadro não fazem a imagem pular. O formato
// (NTSC/PAL) é descoberto ao abrir o jogo rodando 60 quadros e contando as linhas de cada quadro; a paleta PAL
// é usada nos jogos PAL. A tela é desenhada só quando o quadro termina (quadro inteiro, sem rasgos).
//
// Teclas: direcional = joystick; A = botão; SELECT = Game Select; START = Game Reset;
//         L = alterna a dificuldade do jogador 1 (esquerda) entre A e B; R = a do jogador 2;
//         X = alterna Cor / Preto e branco. Começa com as duas dificuldades em B e em cores.
//
// Cartuchos (detecção pelo tamanho e por assinaturas no código, como o Stella): 2K, 4K, 4K com Superchip,
// F8, F6, F4 (com e sem Superchip "SC"), FE (Activision), E0 (Parker Bros.), E7 (M-Network, 8K/12K/16K),
// 3F (Tigervision), FA (CBS RAM Plus), F0 (Megaboy), UA (UA Ltd.) e DPC (Pitfall II).
// Sem firmware. Sem save de bateria (o 2600 não tem).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../nes/cpu.h"

namespace indigo::a2600 {

// .a26 sempre; .bin só como último recurso (o .bin também é usado por outros consoles): tamanho de cartucho
// de 2600 (até 2K, 4K, 8K, 10K do Pitfall II, 12K, 16K, 32K ou 64K) e vetor de reset apontando para o
// cartucho ($1000-$1FFF em algum espelho). O coordenador decide a ordem em que os .bin são testados.
bool isA2600Rom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

// ------------------------------------------------------------------ cartucho
enum class Bank : uint8_t { K2, K4, K4SC, F8, F8SC, F6, F6SC, F4, F4SC, FE, E0, E7, F3F, FA, F0, UA, DPC };
const char* bankName(Bank b);
Bank detectBank(const std::vector<uint8_t>& rom);  // lança runtime_error se o tamanho não serve

class Cart {
public:
  explicit Cart(const std::vector<uint8_t>& rom);
  Bank type;
  void reset();
  // a: endereço de 13 bits com A12 = 1; bus: valor que estava no barramento (leitura da porta de escrita da RAM)
  uint8_t read(uint16_t a, uint8_t bus);
  void write(uint16_t a, uint8_t v);
  // acessos fora do cartucho que mexem nos bancos (3F: escrita em $00-$3F; UA: $0220/$0240; FE: pilha)
  bool watchesLow() const { return type == Bank::F3F || type == Bank::UA || type == Bank::FE; }
  void lowAccess(uint16_t a, uint8_t v, bool isWrite);
  uint8_t peek(uint16_t a) const;  // leitura sem efeito colateral (vetor de reset, testes)
  void sync(StateIO& io);
  // DPC: ciclos de CPU por segundo (para o relógio das vozes de música)
  double clockRate = 1193191.66666667;
  uint64_t* cycleCounter = nullptr;

private:
  void remap();                    // recalcula as fatias de 1 KB a partir dos bancos escolhidos
  void hotspot(uint16_t a);        // a: deslocamento 0-$FFF
  void feWatch(uint16_t a, uint8_t v);
  uint8_t dpcRead(uint16_t a);
  void dpcWrite(uint16_t a, uint8_t v);
  void dpcMusic();
  void dpcRandom();
  uint8_t romAt(uint16_t a) const { return rom_[off_[(a >> 10) & 3] + (a & 0x3FF)]; }

  std::vector<uint8_t> rom_;       // programa (múltiplo de 1 KB)
  std::vector<uint8_t> display_;   // DPC: 2 KB de gráficos
  std::vector<uint8_t> ram_;       // Superchip (128), FA (256), E7 (2048)
  uint32_t off_[4] = {};           // deslocamento na ROM de cada fatia de 1 KB
  int banks4k_ = 1;                // número de bancos de 4 KB (ou de 2 KB no 3F/E7, de 1 KB no E0)
  int cur_ = 0;                    // banco atual (esquemas de 4 KB e F0)
  int e0_[3] = {4, 5, 6};          // E0: banco de cada fatia
  int e7rom_ = 0, e7ram_ = 0, e7ramBank_ = 7;  // E7: banco de ROM em $1000, banco de RAM de 256 bytes
  int f3f_ = 0;                    // 3F: banco em $1000-$17FF
  int ramSize_ = 0;
  bool lastFE_ = false;
  // DPC
  uint8_t tops_[8] = {}, bottoms_[8] = {}, flags_[8] = {};
  uint16_t counters_[8] = {};
  bool music_[3] = {};
  uint8_t random_ = 1;
  uint64_t audioCycles_ = 0;
  double fracClocks_ = 0;
};

// ------------------------------------------------------------------ TIA
class TIA {
public:
  TIA();
  void reset();
  void setPal(bool pal);
  // um ciclo de cor
  void clock();
  // n ciclos de cor (o som anda junto)
  void run(int n);
  uint8_t read(uint8_t a, uint8_t bus);
  void write(uint8_t a, uint8_t v);
  int hctr() const { return hctr_; }
  uint32_t lineCount() const { return lines_; }
  void sync(StateIO& io);

  // entradas
  bool fire[2] = {};

  // quadro
  bool frameDone = false;
  int frameHeight = 228, yStart = 23, frameLines = 262;
  uint32_t lastFrameLines = 0;     // linhas do último quadro completo
  // detecção NTSC/PAL (como o detector do Stella): votos pelas linhas entre os fins de VSYNC
  double palVotes = 0, ntscVotes = 0;
  uint32_t vsyncCount = 0, lastVsyncOffLine = 0;
  uint32_t totalFrames = 0;
  uint8_t screen[160 * 274] = {};  // índices de cor (valor do TIA >> 1), uma linha de 160 pontos por linha

  // som: amostras nativas (2 por linha, ~31,4 kHz), volume 0..30 dos dois canais somados
  std::vector<uint8_t> audioOut;
  bool wsyncRequested = false;

private:
  // objetos (a lógica de cada um segue os contadores e decodificadores do chip)
  struct Player {
    uint16_t collision = 0, maskOff = 0;
    uint8_t color = 0, hmm = 0, counter = 0, copy = 1, divider = 1, dividerPending = 1, sample = 0;
    uint8_t decodesOffset = 0, patOld = 0, patNew = 0, pattern = 0;
    int8_t renderCounter = 0, tripPoint = 0, dividerChange = -1;
    bool moving = false, rendering = false, reflected = false, delaying = false;
    void reset(uint16_t mask);
    void updatePattern();
    void setDivider(uint8_t d) { divider = d; tripPoint = d == 1 ? 0 : 1; }
    void latch() {
      collision = (!rendering || renderCounter < tripPoint) ? maskOff
                : ((pattern & (1u << sample)) ? 0xFFFF : maskOff);
    }
    void tick();
    void nusiz(uint8_t v, bool hblank);
    void resp(uint8_t c);
    uint8_t respClock() const;
    bool mainCopyAt4() const { return rendering && sample == 4 && copy == 1; }
  };
  struct Missile {
    uint16_t collision = 0, maskOff = 0;
    uint8_t color = 0, hmm = 0, counter = 0, copy = 1, width = 1, effWidth = 1, decodesOffset = 0;
    int8_t renderCounter = 0;
    bool moving = false, rendering = false, visible = false, enabled = false, enam = false, resmp = false;
    void reset(uint16_t mask);
    void updateEnabled() { enabled = enam && !resmp; latchOnly(); }
    void latchOnly() { collision = (visible && enabled) ? 0xFFFF : maskOff; }
    void tick(int hclock, bool regular);
    void resm(uint8_t c, bool hblank);
    void nusiz(uint8_t v);
  };
  struct Ball {
    uint16_t collision = 0, maskOff = 0;
    uint8_t color = 0, hmm = 0, counter = 0, width = 1, effWidth = 1, lastMoveTick = 0;
    int8_t renderCounter = 0;
    bool moving = false, rendering = false, signal = false, enabled = false, enOld = false, enNew = false,
         delaying = false;
    void reset(uint16_t mask);
    void updateEnabled() { enabled = delaying ? enOld : enNew; latchOnly(); }
    void latchOnly() { collision = (signal && enabled) ? 0xFFFF : maskOff; }
    void tick(bool regular);
  };

  void delayedWrite(uint8_t reg, uint8_t v);
  void push(uint8_t reg, uint8_t v, int delay);
  void tickMovement();
  void tickHblank();
  void tickHframe();
  void pfTick(uint32_t x);
  void renderPixel(uint32_t x);
  void nextLine();
  void applyRsync();
  uint8_t resxCounter() const;
  // gerente de quadro (VSYNC/VBLANK)
  void fmNextLine();
  void fmSetVsync(bool on);
  void fmSetState(int s);
  void frameComplete();
  // som
  void audioPhase0(int ch);
  void audioPhase1(int ch);
  void audioRun(int n);

  Player p0_, p1_;
  Missile m0_, m1_;
  Ball bl_;
  // playfield
  uint32_t pfPattern_ = 0;
  uint16_t pfCollision_ = 0, pfMaskOff_ = 0;
  uint8_t pf0_ = 0, pf1_ = 0, pf2_ = 0, pfColor_ = 0, pfX_ = 0;
  bool pfReflected_ = false, pfRefp_ = false, pfScore_ = false;
  uint8_t bkColor_ = 0, priority_ = 0;  // 0 normal, 1 placar, 2 playfield na frente
  uint8_t col0_ = 0, col1_ = 0;         // COLUP0/COLUP1 (índices de cor)
  // linha
  int hctr_ = 0, hctrDelta_ = 0;
  bool hblank_ = true, extendedHblank_ = false, movementInProgress_ = false;
  uint8_t movementClock_ = 0;
  uint16_t collisionMask_ = 0;
  uint32_t lines_ = 0;
  // fila de escritas atrasadas (os registradores do chip só mudam alguns ciclos de cor depois da escrita)
  struct Pending { uint8_t reg, value, left; };
  Pending queue_[24] = {};
  int queued_ = 0;
  // entradas
  bool latched_ = false;
  uint8_t latchValue_[2] = {0x80, 0x80};
  bool dumped_ = false;
  // registradores de quadro
  bool vsync_ = false, vblank_ = false;
  // gerente de quadro
  int fmState_ = 0;  // 0 espera VSYNC, 1 espera o fim do VSYNC, 2 espera o começo da imagem, 3 imagem
  uint32_t lineInState_ = 0, vsyncLineCount_ = 0, curFrameLines_ = 0, y_ = 0, lastY_ = 0;
  bool vsyncPending_ = false, rendering_ = false;
  uint32_t vsyncPendingLines_ = 0;
  // som
  struct Channel {
    uint8_t audc = 0, audf = 0, audv = 0, div = 0, pulse = 0, noise = 0;
    bool clockEnable = false, noiseFeedback = false, noiseBit4 = false, pulseHold = false;
  };
  Channel ch_[2];
  int audioCounter_ = 0;
  uint32_t sum0_ = 0, sum1_ = 0, sumCt_ = 0;
};

// ------------------------------------------------------------------ máquina
class Machine : public nes::CpuBus {
public:
  explicit Machine(const std::vector<uint8_t>& rom);
  Cart cart;
  nes::CPU cpu;
  TIA tia;
  bool pal = false;
  uint64_t cycles = 0;

  void powerOn();
  void runFrame();
  void setKeys(uint32_t mask);
  void setKeys2(uint32_t mask);  // joystick da porta 2 (só direções e botão)
  uint8_t read(uint16_t a) override;
  void write(uint16_t a, uint8_t v) override;
  uint8_t peek(uint16_t a);  // sem efeito colateral (testes)
  void sync(StateIO& io);
  uint8_t ram[128] = {};

private:
  void cycle();
  void waitWsync();
  uint8_t riotRead(uint16_t a);
  void riotWrite(uint16_t a, uint8_t v);
  void riotTick();

  uint8_t dataBus_ = 0;
  // RIOT
  uint8_t timer_ = 0, intFlag_ = 0, ddra_ = 0, ddrb_ = 0, outa_ = 0, outb_ = 0;
  uint16_t divider_ = 1024, sub_ = 0;
  bool wrapped_ = false, edgePositive_ = false;
  // controles
  uint8_t joy_ = 0xFF;       // SWCHA (jogador 1 nos 4 bits altos, jogador 2 nos baixos; 1 = solto)
  uint8_t switches_ = 0x3F;  // SWCHB: reset, select, cor, dificuldades
  uint32_t prevKeys_ = 0;
};

class A2600Core : public Core {
public:
  A2600Core(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  Machine& machine() { return *m_; }

  std::string system() const override { return "a2600"; }
  std::string title() const override { return ""; }
  void runFrame() override;
  void reset() override;
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  void setKeys2(uint32_t mask) override { m_->setKeys2(mask); }
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override { return fb_.data(); }
  int width() const override { return 320; }
  int height() const override { return m_->tia.frameHeight; }
  double fps() const override { return m_->pal ? 3546894.0 / 228.0 / 312.0 : 3579545.0 / 228.0 / 262.0; }
  void setSampleRate(int rate) override;
  size_t audioAvailable() const override { return (wpos_ + kBuf - rpos_) % kBuf; }
  void pullAudio(float* left, float* right, size_t n) override;
  bool hasSave() const override { return false; }
  bool saveDirty() const override { return false; }
  std::vector<uint8_t> exportSave() override { return {}; }
  void importSave(const std::vector<uint8_t>&) override {}
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& s) override;

  // índices de cor da última tela (160 por linha), para os testes
  const uint8_t* screenIndices() const { return m_->tia.screen; }
  uint32_t paletteRgb(uint8_t idx) const;  // 0xRRGGBB

private:
  void convert();
  void mixAudio();
  std::unique_ptr<Machine> m_;
  std::vector<uint32_t> fb_;
  static constexpr size_t kBuf = 16384;
  std::vector<float> buf_ = std::vector<float>(kBuf);
  size_t wpos_ = 0, rpos_ = 0;
  double step_ = 0, phase_ = 0, prev_ = 0, hp_ = 0, hpIn_ = 0;
  int rate_ = 48000;
};

}  // namespace indigo::a2600
