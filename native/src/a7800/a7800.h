// Núcleo Atari 7800 (ProSystem): CPU "Sally" (6502 com modo decimal, a mesma de nes/cpu.h), MARIA (listas de
// exibição DLL/DL, modos 160A/160B/320A/320B/320C/320D, DMA com buracos, modo canguru, modo de escrita, NMI de DLI
// e a conta de ciclos de DMA de cada linha, que tira tempo da CPU), TIA só com o som (2 canais), RIOT 6532
// (controles, chaves do console e temporizador) e POKEY no cartucho quando o cabeçalho .a78 pede.
//
// Relógio: o mestre é 7,159 MHz (NTSC; 7,094 MHz no PAL). A linha tem 454 ciclos do mestre e o quadro 263 linhas
// (313 no PAL). A CPU usa 4 ciclos do mestre por acesso (1,79 MHz), ou 6 quando o endereço é da TIA ou do RIOT
// (1,19 MHz). O MARIA para a CPU no ciclo 28 da linha enquanto faz o DMA.
//
// Sem BIOS: o jogo começa direto no vetor de reset do cartucho (como os emuladores fazem). Se o arquivo de
// firmware "a7800" (BIOS de 4 KB ou 16 KB) for fornecido, ela roda antes, como no aparelho.
//
// Cartuchos: plano (até 52 KB), SuperGame (bancos de 16 KB; RAM de 16 KB ou 2x16 KB em $4000, banco 0 ou 6
// em $4000, 144 KB), Absolute (F-18 Hornet), Activision (Double Dragon, Rampage), Souper (Rikki & Vikki, sem o
// chip de música BupChip), banksets (o MARIA lê a segunda metade da ROM), POKEY em $4000, $0450 ou $0800,
// SaveKey (EEPROM 24LC256 na porta 2, vira o "save" do jogo).
//
// Teclas: direcional = joystick ProLine; A = botão esquerdo (o principal, INPT1); B = botão direito (INPT0);
// START = Reset do console; SELECT = Select; X = Pause; L e R trocam a posição das chaves de dificuldade
// esquerda e direita (cada toque alterna entre B e A; começam em B).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../nes/cpu.h"

namespace indigo::a7800 {

// cabeçalho "ATARI7800" ou extensão .a78
bool isA7800Rom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

// ------------------------------------------------------------------ cartucho
struct Cart {
  enum Kind : uint8_t { FLAT, SUPERGAME, ABSOLUTE, ACTIVISION, SOUPER };
  Kind kind = FLAT;
  std::vector<uint8_t> rom;   // ROM que a CPU vê (múltiplo de 16 KB)
  std::vector<uint8_t> mrom;  // banksets: ROM que o MARIA vê (vazio = a mesma)
  std::vector<uint8_t> ram;   // RAM do cartucho em $4000 (0, 16 KB ou 32 KB)
  size_t romSize = 0;         // tamanho real (antes de completar o múltiplo de 16 KB)
  bool pokey4000 = false, pokey450 = false, pokey800 = false;
  bool exrom = false, bank6 = false, bankedRam = false, mirrorRam = false, haltRam = false;
  bool pal = false, saveKey = false;
  std::string title;
  int banks = 1;  // bancos de 16 KB da ROM

  // estado
  uint8_t bank = 0, ramBank = 0;
  uint8_t souperChr[2] = {}, souperMode = 0, souperRam[2] = {};

  // tabelas de páginas de 4 KB ($0000–$FFFF): nullptr = sem ROM/RAM ali (registrador, POKEY ou barramento aberto)
  const uint8_t* rpage[16] = {};
  uint8_t* wpage[16] = {};
  const uint8_t* mpage[16] = {};

  bool load(const std::vector<uint8_t>& file, const std::string& nome, std::string& erro);
  void reset();
  void rebuild();  // refaz as tabelas a partir do banco atual
  void write(uint16_t a, uint8_t v);  // escrita em $4000–$FFFF que não caiu em RAM (troca de banco)
  uint8_t mariaSpecial(uint16_t a) const;  // leitura do MARIA nas áreas especiais (Souper)
  bool mariaHasSpecial() const { return kind == SOUPER && (souperMode & 3) == 3; }
  void sync(StateIO& io);

private:
  const uint8_t* romAt(size_t off) const;
  const uint8_t* mromAt(size_t off) const;
};

// ------------------------------------------------------------------ som
class TiaSound {
public:
  void reset();
  void write(int r, uint8_t v);  // r = 0x15..0x1A
  void tick();                   // um passo do relógio de som (31,4 kHz)
  int level() const { return (bit[0] ? audv[0] : 0) + (bit[1] ? audv[1] : 0); }
  void sync(StateIO& io);

private:
  void channel(int c);
  uint8_t audc[2] = {}, audf[2] = {}, audv[2] = {};
  uint16_t div[2] = {}, p4[2] = {}, p5[2] = {}, p9[2] = {};
  uint8_t bit[2] = {};
};

class Pokey {
public:
  void reset();
  void write(int r, uint8_t v, uint64_t now);
  uint8_t read(int r, uint64_t now);
  uint64_t nextEvent() const;           // ciclo (1,79 MHz) do próximo pulso de canal
  void fire(uint64_t now);              // processa os pulsos que vencem em "now"
  int level() const;
  void sanitize(uint64_t now);  // depois de ler um estado
  bool irqEnabled() const { return irqen & 7; }
  bool irq() const { return (~irqst & irqen & 0x07) != 0; }
  void sync(StateIO& io);

private:
  void recalc(uint64_t now);
  uint8_t audf[4] = {}, audc[4] = {}, audctl = 0, skctl = 0, irqen = 0, irqst = 0xFF;
  uint32_t period[4] = {1, 1, 1, 1};
  uint64_t next[4] = {};
  uint8_t out[4] = {}, hp[2] = {};
  bool active[4] = {};
};

// ------------------------------------------------------------------ máquina
class A7800 : public nes::CpuBus {
public:
  explicit A7800(Cart c);
  Cart cart;
  nes::CPU cpu;
  bool pal = false;
  int lines = 263;            // linhas por quadro
  int outHeight = 240;        // linhas da imagem de saída
  static constexpr int kFirstOut = 17;  // primeira linha mostrada
  std::vector<uint32_t> fb;   // 320 x outHeight
  bool dirty = false;         // EEPROM do SaveKey mudou

  void reset(bool power);
  void runFrame();
  void setKeys(uint32_t mask);
  void setSampleRate(int rate);
  size_t audioAvailable() const { return (wpos + kBuf - rpos) % kBuf; }
  void pullAudio(float* l, float* r, size_t n);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  std::vector<uint8_t> eeprom;  // SaveKey (32 KB) quando o cabeçalho pede

  uint8_t read(uint16_t a) override;
  void write(uint16_t a, uint8_t v) override;
  uint8_t peek(uint16_t a) const;  // sem efeito colateral (ferramentas de teste)

  // números para os testes
  uint64_t frameCount = 0;
  uint64_t cpuCycles = 0;  // acessos da CPU ao barramento
  int lastDmaCycles = 0;   // ciclos do MARIA no último DMA de linha

private:
  // relógio
  void beginCycle(uint16_t a);
  void endCycle();
  void advance(uint64_t clocks);
  void idleTo(uint64_t m);
  void endLine();
  // memória
  uint8_t memRead(uint16_t a);
  void memWrite(uint16_t a, uint8_t v);
  uint8_t mariaRead(uint16_t a) const;
  // MARIA
  void mariaWrite(int r, uint8_t v);
  void runDma();
  int drawLine();
  int lineRamWrite(uint16_t addr, int hpos, int pal);
  void loadDll();
  void renderTo(int pos);
  // RIOT
  void riotSync();
  uint8_t riotRead(uint16_t a);
  void riotWrite(uint16_t a, uint8_t v);
  uint8_t swcha() const;
  uint8_t tiaRead(int r) const;
  // SaveKey (I2C)
  void saveKeyPins(bool scl, bool sda);
  bool saveKeySda() const;
  // som
  void audioSync();
  void integrate(int level, uint64_t dt);
  void emit(double v);
  void sync(StateIO& io);

  // memória interna
  uint8_t ram1[0x800] = {}, ram2[0x800] = {}, riotRam[0x80] = {};
  std::vector<uint8_t> bios;
  bool biosOn = false, ctrlLock = false, biosFailed = false;
  uint8_t openBus = 0;

  // tempo
  uint64_t mclk = 0, lineStart = 0, haltUntil = 0, phi2 = 0;
  int line = 0;
  bool dmaPending = false, frameDone = false, nmiLine = false, wsync = false;

  // MARIA
  uint8_t col[32] = {};  // cores ($20–$3F; as de índice múltiplo de 4 são o fundo)
  uint8_t ctrl = 0x7F, dpph = 0, dppl = 0, charbase = 0;
  bool dmaOn = false, vblank = true;
  uint16_t dll = 0, dl = 0;
  uint8_t offset = 0, holey = 0, writeMode = 0;
  bool nmiPending = false;
  uint8_t lineRam[2][160] = {};
  int front = 0;      // buffer mostrado nesta linha
  int rendered = 0;   // pontos (de 320) já desenhados na linha atual

  // RIOT
  uint8_t ddra = 0, ora = 0, ddrb = 0, orb = 0;
  uint8_t intim = 0, riotFlags = 0;
  uint16_t interval = 1024;
  uint32_t counter = 1024;
  bool expired = false;
  uint64_t riotLast = 0;
  // controles
  uint8_t joy = 0xFF;     // SWCHA (direções do jogador 1 em 7–4; 1 = solto)
  uint8_t buttons = 0;    // bit 0 = botão esquerdo, bit 1 = direito
  uint8_t sw = 0;         // bit 0 reset, 1 select, 3 pause (1 = apertado)
  uint8_t diff = 0;       // bit 0 = dificuldade esquerda em A, bit 1 = direita em A
  uint32_t prevKeys = 0;
  bool oneButton = true;

  // SaveKey
  uint8_t i2cState = 0, i2cBits = 0, i2cByte = 0, i2cOut = 1;
  bool i2cScl = true, i2cSda = true, i2cRead = false, i2cAck = false;
  uint16_t i2cAddr = 0;
  int i2cCount = 0;

  // som
  TiaSound tia;
  Pokey pokey;
  bool hasPokey = false;
  uint64_t audioM = 0, tiaNext = 0;
  int audioLevel = 0;
  double samplePeriod = 1, sampleT = 0, sampleV = 0, capV = 0;
  static constexpr size_t kBuf = 16384;
  std::vector<float> buf = std::vector<float>(kBuf);
  size_t wpos = 0, rpos = 0;
  double masterHz = 7159090.9;

  const uint32_t* palette = nullptr;
};

class A7800Core : public Core {
public:
  A7800Core(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  A7800& m() { return *m_; }

  std::string system() const override { return "a7800"; }
  std::string title() const override { return m_->cart.title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(false); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->fb.data(); }
  int width() const override { return 320; }
  int height() const override { return m_->outHeight; }
  double fps() const override;
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return m_->cart.saveKey; }
  bool saveDirty() const override { return m_->dirty; }
  std::vector<uint8_t> exportSave() override { m_->dirty = false; return m_->eeprom; }
  void importSave(const std::vector<uint8_t>& b) override;
  std::string note() const override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<A7800> m_;
};

}  // namespace indigo::a7800
