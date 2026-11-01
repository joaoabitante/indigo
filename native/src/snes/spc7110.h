// Chip SPC7110 da Hudson (Far East of Eden Zero / Tengai Makyou Zero, Momotarou Dentetsu Happy, Super Power
// League 4) e o relógio RTC-4513 da Epson que vem junto no Far East of Eden Zero. O chip tem quatro partes:
//   - descompressão (registradores $4800-$480C): tabela de fluxos na ROM de dados, codificação aritmética
//     adaptativa com modelo de contexto por pixel (1, 2 ou 4 bits por pixel) e saída já em tiles do SNES;
//   - porta de dados ($4810-$481A): leitura da ROM de dados com deslocamento, passo e incremento automático;
//   - conta ($4820-$482F): multiplicação 16×16 e divisão 32÷16, com e sem sinal;
//   - mapa de memória ($4830-$4834): a ROM de programa (o 1º MB do arquivo) em $00-$0F/$C0-$CF e três janelas
//     de 1 MB da ROM de dados em $D0-$FF (e nos espelhos de $10-$3F:$8000-$FFFF); a RAM do cartucho em $6000-$7FFF
//     só funciona com o bit 7 de $4830 ligado.
// O algoritmo de descompressão foi decifrado por neviksti (e simplificado por talarubi); esta é uma implementação
// própria do Índigo, conferida com o bsnes. O relógio segue a hora do aparelho (decisão do João, 31/10): ao ligar o
// jogo, ao carregar um estado e quando o jogo troca entre 12 e 24 horas, a data e a hora do computador ou do
// celular são copiadas para o chip. Entre esses momentos ele conta sozinho, como o de verdade, e aceita a hora que
// o jogo gravar. O save continua só com a RAM de 8 KB (o mesmo arquivo dos outros emuladores).
#pragma once
#include <cstdint>
#include <ctime>
#include <vector>

#include "../core/state.h"

namespace indigo::snes {

class SPC7110 {
public:
  // rom: o arquivo inteiro (1 MB de programa + dados; 7 MB = tradução com 1 MB de expansão em $40-$4F)
  SPC7110(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram, bool rtc);
  void reset(bool power);
  void run(int clocks);  // relógio mestre (21,477 MHz): contas e descompressão em andamento, relógio

  bool ownsIo(uint16_t a) const { return a >= 0x4800 && a < 0x4843 && (a < 0x4840 || rtc_); }
  uint8_t ioRead(uint32_t addr, uint8_t openBus);  // $4800-$483F, $50:xxxx (= $4800), $58:xxxx (= $4808), relógio
  void ioWrite(uint32_t addr, uint8_t v);
  bool romRead(uint32_t addr, uint8_t& v) const;  // ROM em $00-$3F/$80-$BF:$8000-$FFFF, $C0-$FF e a expansão
  bool ramRead(uint32_t addr, uint8_t& v) const;  // $00-$3F/$80-$BF:$6000-$7FFF
  bool ramWrite(uint32_t addr, uint8_t v, bool& changed);
  void sync(StateIO& io);

  // ---------------------------------------------------------------- relógio RTC-4513
  struct Rtc {
    uint32_t ticks = 0;    // 2^21 por segundo (21 bits)
    uint32_t seconds = 0;  // para as interrupções de minuto e de hora
    uint8_t chipSelect = 0, state = 0, mdr = 0, offset = 0, ready = 0, holdTick = 0;
    uint32_t wait = 0;
    // registradores (cada campo com a largura que tem no chip; mask() corta)
    uint8_t secondLo = 0, secondHi = 0, batteryFail = 1, minuteLo = 0, minuteHi = 0, resync = 0;
    uint8_t hourLo = 0, hourHi = 0, meridian = 0, dayLo = 0, dayHi = 0, dayRam = 0;
    uint8_t monthLo = 0, monthHi = 0, monthRam = 0, yearLo = 0, yearHi = 0, weekday = 0;
    uint8_t hold = 0, calendar = 0, irqFlag = 0, roundSec = 0, irqMask = 0, irqDuty = 0, irqPeriod = 0;
    uint8_t pause = 0, stop = 0, atime = 0, test = 0;
    bool followDevice = false;  // segue a hora do aparelho (fora do estado salvo)

    void powerOn();
    void setFrom(std::time_t t);
    void step();
    uint8_t read(int a);
    void write(int a, uint8_t v);
    uint8_t regRead(int a);
    void regWrite(int a, uint8_t v);
    void mask();
    void irq(int period) { if (!stop && !pause && period == irqPeriod) irqFlag = 1; }
    void tick();
    void tickSecond();
    void tickMinute();
    void tickHour();
    void tickDay();
    void tickMonth();
    void tickYear();
    void sync(StateIO& io);
  };
  Rtc& rtcForTest() { return clock_; }  // para os testes
  static std::time_t (*deviceClock)();   // hora do aparelho (os testes trocam por uma fixa)
  // depois que o app carrega um estado: o relógio volta para a hora do aparelho. Fica fora do loadState para o
  // estado continuar exato (salvar, carregar e salvar de novo dá o mesmo arquivo).
  void followHostClock() { if (rtc_ && clock_.followDevice) clock_.setFrom(deviceClock()); }

private:
  uint8_t dataRom(uint32_t addr) const;
  uint32_t dataOffset() const { return r4811 | (r4812 << 8) | (uint32_t(r4813) << 16); }
  uint16_t dataAdjust() const { return static_cast<uint16_t>(r4814 | (r4815 << 8)); }
  uint16_t dataStride() const { return static_cast<uint16_t>(r4816 | (r4817 << 8)); }
  void setDataOffset(uint32_t v) { r4811 = uint8_t(v); r4812 = uint8_t(v >> 8); r4813 = uint8_t(v >> 16) & 0x7F; }
  void setDataAdjust(uint32_t v) { r4814 = uint8_t(v); r4815 = uint8_t(v >> 8); }
  void dataPortRead();
  void dataPortStep();               // depois de ler $4810
  void dataPortAdjust(int trigger);  // escrita em $4814 (1), $4815 (2) ou leitura de $481A (3)
  void dcuLoadAddress();
  void dcuBegin();
  uint8_t dcuRead();
  void multiply();
  void divide();

  // ---------------------------------------------------------------- descompressor
  struct Decoder {
    struct Ctx { uint8_t state = 0, invert = 0; };
    Ctx ctx[5][15];
    uint8_t bpp = 1;
    uint32_t offset = 0;
    uint8_t bitsLeft = 8;
    uint16_t range = 0x100, code = 0;
    uint8_t history = 0;
    uint64_t pixels = 0, recent = 0;  // últimos pixels e lista das cores mais recentes (um nibble por cor)
    uint32_t word = 0;                // 8 pixels decodificados (planos de bits já separados)

    void start(const SPC7110& c, int mode, uint32_t origin);
    void decode(const SPC7110& c);
    void sync(StateIO& io);
  };




  const std::vector<uint8_t>& rom_;
  std::vector<uint8_t>& ram_;
  size_t dromBase_ = 0, dromSize_ = 0, expBase_ = 0, expSize_ = 0;
  bool rtc_ = false;

  // descompressão
  uint8_t r4801 = 0, r4802 = 0, r4803 = 0, r4804 = 0, r4805 = 0, r4806 = 0, r4807 = 0;
  uint8_t r4809 = 0, r480a = 0, r480b = 0, r480c = 0;
  uint8_t dcuMode = 0;
  uint32_t dcuAddress = 0;
  uint8_t dcuOffset = 0, dcuTile[32] = {};
  int32_t dcuWait = 0;  // ciclos até a descompressão começar (0 = nada pendente)
  Decoder dec_;
  // porta de dados
  uint8_t r4810 = 0, r4811 = 0, r4812 = 0, r4813 = 0, r4814 = 0, r4815 = 0, r4816 = 0, r4817 = 0, r4818 = 0;
  // contas
  uint8_t alu[16] = {};  // $4820-$482F
  int32_t mulWait = 0, divWait = 0;
  // mapa
  uint8_t r4830 = 0, r4831 = 0, r4832 = 1, r4833 = 2, r4834 = 0;
  // relógio
  Rtc clock_;
  int64_t rtcDebt_ = 0;
};

}  // namespace indigo::snes
