// Testes do SPC7110 do SNES (src/snes/spc7110.*) e do relógio RTC-4513. Não há ROMs de teste públicas para o
// chip. O teste confere:
//   1. o chip sozinho contra o bsnes: 48 roteiros pseudoaleatórios de gravações e leituras nos registradores
//      (descompressão nos três modos com pulos e passos, porta de dados com todos os modos de $4818, contas com
//      e sem sinal, janelas da ROM de dados, RAM) numa ROM de 3 MB com uma tabela de 256 fluxos. Cada roteiro
//      vira uma impressão digital (FNV-1a) de tudo o que foi lido; os valores esperados foram tirados uma vez do
//      bsnes, fora do repositório (bsnes-emu/bsnes 78474e5, sfc/coprocessor/spc7110);
//   2. a contagem do relógio (BCD, inclusive valores inválidos, 12/24 horas, meses e anos bissextos) e o protocolo
//      das portas $4840-$4842, também contra o bsnes (sfc/coprocessor/epsonrtc);
//   3. o chip no barramento do SNES, num cartucho de mentira com o cabeçalho de SPC7110: detecção, mapa de memória,
//      portas em $50 e $58, DMA da porta de descompressão, RAM com e sem o bit 7 de $4830, tempo das contas,
//      estado salvo no meio de um fluxo e o relógio seguindo a hora do aparelho (ao ligar, na troca para 24 horas
//      e ao carregar um estado).
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifndef SPC7110_GOLDEN
#include "../src/snes/snes.h"
#endif

namespace gen {
static uint32_t x;
static uint32_t rnd() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
constexpr int SCRIPTS = 48, RTC_SCRIPTS = 32;

// 1 MB de programa + 2 MB de dados. Dados: tabela de 256 fluxos no começo (modo e endereço de 24 bits, grande
// endian), depois bytes com graus diferentes de "compressão" (mais zeros = sequências mais longas)
static std::vector<uint8_t> makeRom() {
  std::vector<uint8_t> r(0x300000);
  x = 0x7110F00Du;
  for (size_t i = 0; i < 0x100000; i++) r[i] = static_cast<uint8_t>(rnd());
  for (size_t i = 0x100000; i < r.size(); i++) {
    const int p = static_cast<int>((i >> 10) % 5);
    uint8_t v = static_cast<uint8_t>(rnd());
    for (int j = 0; j < p; j++) v &= static_cast<uint8_t>(rnd());
    if ((i >> 12) & 1) v = static_cast<uint8_t>(~v);
    r[i] = v;
  }
  for (int k = 0; k < 256; k++) {
    const size_t e = 0x100000 + k * 4;
    const uint32_t addr = 0x400 + (rnd() % 0x1FF000);
    r[e] = static_cast<uint8_t>(k % 7 == 6 ? 3 : k % 3);  // de vez em quando o modo 3 (inválido)
    r[e + 1] = static_cast<uint8_t>(addr >> 16); r[e + 2] = static_cast<uint8_t>(addr >> 8); r[e + 3] = static_cast<uint8_t>(addr);
  }
  return r;
}
static uint32_t h;
static void mix(uint8_t v) { h ^= v; h *= 16777619u; }

// roteiro k sobre um chip com w(endereço, valor), r(endereço), wait() (deixa as operações pendentes terminarem),
// rom(endereço) e ram(endereço)/ramw(endereço, valor)
template <class Chip>
static uint32_t script(Chip& c, int k) {
  x = 0xC0FFEEu ^ static_cast<uint32_t>(k * 104729);
  h = 2166136261u;
  for (int step = 0; step < 60; step++) {
    switch (rnd() % 6) {
      case 0: case 1: {  // descompressão
        if (rnd() % 4 == 0) { c.w(0x4801, rnd() & 0xFC); c.w(0x4802, rnd() & 3); }
        else { c.w(0x4801, 0); c.w(0x4802, 0); }
        c.w(0x4803, 0);
        c.w(0x4804, rnd());
        c.w(0x4805, rnd() % 24);
        c.w(0x4807, rnd() % 5);
        c.w(0x480B, rnd() & 3);
        c.w(0x4809, rnd()); c.w(0x480A, rnd());
        mix(c.r(0x480C));
        c.w(0x4806, 0);
        c.wait();
        mix(c.r(0x480C));
        const int n = 1 + static_cast<int>(rnd() % 600);
        for (int i = 0; i < n; i++) mix(c.r(0x4800));
        for (uint16_t a = 0x4801; a <= 0x480C; a++) mix(c.r(a));
        break;
      }
      case 2: {  // porta de dados
        c.w(0x4818, rnd() & 0x7F);
        c.w(0x4811, rnd()); c.w(0x4812, rnd()); c.w(0x4813, rnd() & 0x3F);
        c.w(0x4816, rnd()); c.w(0x4817, rnd() % 3 == 0 ? 0xFF : rnd() & 3);
        c.w(0x4814, rnd()); c.w(0x4815, rnd() % 3 == 0 ? 0xFF : rnd() & 7);
        for (int i = 0; i < 40; i++) {
          switch (rnd() % 6) {
            case 0: c.w(0x4814, rnd()); break;
            case 1: c.w(0x4815, rnd() & 0x83); break;
            case 2: mix(c.r(0x481A)); break;
            case 3: c.w(0x4818, rnd() & 0x7F); break;
            default: mix(c.r(0x4810)); break;
          }
        }
        for (uint16_t a = 0x4810; a <= 0x481A; a++) if (a != 0x481A) mix(c.r(a));
        break;
      }
      case 3: {  // contas
        for (uint16_t a = 0x4820; a <= 0x4824; a++) c.w(a, rnd());
        c.w(0x482E, rnd() & 1);
        if (rnd() & 1) { c.w(0x4825, rnd()); }
        else { c.w(0x4826, rnd() % 4 == 0 ? 0 : rnd()); c.w(0x4827, rnd() % 4 == 0 ? 0 : rnd()); }
        c.wait();
        for (uint16_t a = 0x4820; a <= 0x482F; a++) mix(c.r(a));
        break;
      }
      case 4: {  // mapa de memória e ROM
        for (uint16_t a = 0x4830; a <= 0x4834; a++) if (rnd() & 1) c.w(a, rnd());
        for (uint16_t a = 0x4830; a <= 0x4834; a++) mix(c.r(a));
        for (int i = 0; i < 64; i++) {
          uint32_t addr = rnd() & 0xFFFFFF;
          if (rnd() & 1) addr |= 0xC00000; else addr = (addr & 0xBF7FFF) | 0x8000;
          mix(c.rom(addr));
        }
        break;
      }
      default: {  // RAM do cartucho
        if (rnd() & 1) c.w(0x4830, rnd() & 0x87);
        for (int i = 0; i < 16; i++) {
          const uint32_t addr = ((rnd() & 0xBF) << 16) | 0x6000 | (rnd() & 0x1FFF);
          if (rnd() & 1) c.ramw(addr, static_cast<uint8_t>(rnd()));
          else mix(c.ram(addr));
        }
        break;
      }
    }
  }
  return h;
}

// relógio: um estado inicial aleatório (todos os campos com a largura do chip), depois gravações e leituras pelas
// portas e passos de contagem (segundo, minuto, hora, dia, mês, ano), e no fim todos os registradores lidos pelas
// portas. R tem w(porta, valor), r(porta), tick(qual) e set(campo, valor) com os campos na ordem do RTC-4513
template <class R>
static uint32_t rtcScript(R& c, int k) {
  x = 0x4513u ^ static_cast<uint32_t>(k * 7907);
  h = 2166136261u;
  for (int f = 0; f < 29; f++) c.set(f, static_cast<uint8_t>(rnd()));
  for (int step = 0; step < 400; step++) {
    const uint32_t op = rnd() % 10;
    if (op < 4) c.tick(static_cast<int>(rnd() % 6));
    else if (op < 6) {  // grava um registrador pelo protocolo
      c.w(0, 1); c.w(1, 3); c.wait(); c.w(1, rnd() & 15); c.wait();
      const int n = 1 + static_cast<int>(rnd() % 3);
      for (int i = 0; i < n; i++) { c.w(1, rnd() & 15); c.wait(); }
      c.w(0, 0);
    } else if (op < 8) {  // lê registradores pelo protocolo
      c.w(0, 1); c.w(1, 0x0C); c.wait(); c.w(1, rnd() & 15); c.wait();
      for (int i = 0; i < 4; i++) { mix(c.r(1)); mix(c.r(2)); c.wait(); }
      c.w(0, 0);
    } else if (op == 8) {  // portas sem o protocolo certo
      c.w(static_cast<int>(rnd() % 3), rnd() & 15);
      mix(c.r(static_cast<int>(rnd() % 3)));
    } else {
      mix(c.r(0)); mix(c.r(2));
    }
  }
  c.w(0, 1); c.w(1, 0x0C); c.wait(); c.w(1, 0); c.wait();
  for (int i = 0; i < 16; i++) { mix(c.r(1)); c.wait(); }
  c.w(0, 0);
  return h;
}
}  // namespace gen

#ifndef SPC7110_GOLDEN
using indigo::snes::SPC7110;

static int checks = 0, fails = 0;
static void check(bool ok, const char* what, long got = -1, long want = -1) {
  checks++;
  if (ok) return;
  fails++;
  if (want >= 0) std::printf("  FALHA: %s (veio %lX, esperado %lX)\n", what, got, want);
  else std::printf("  FALHA: %s\n", what);
}

// tirados do bsnes (bsnes-emu/bsnes 78474e5) fora do repositório
static const uint32_t GOLDEN[gen::SCRIPTS] = {
#include "spc7110test-golden.inc"
};
static const uint32_t RTC_GOLDEN[gen::RTC_SCRIPTS] = {
#include "spc7110test-rtc-golden.inc"
};

struct Mine {
  std::vector<uint8_t> data, sram = std::vector<uint8_t>(0x2000, 0);
  SPC7110 chip;
  explicit Mine(const std::vector<uint8_t>& r) : data(r), chip(data, sram, false) {}
  void w(uint16_t a, uint32_t v) { chip.ioWrite(a, static_cast<uint8_t>(v)); }
  uint8_t r(uint16_t a) { return chip.ioRead(a, 0xEE); }
  void wait() { chip.run(64); }
  uint8_t rom(uint32_t a) { uint8_t v = 0xEE; chip.romRead(a, v); return v; }
  uint8_t ram(uint32_t a) { uint8_t v = 0xEE; chip.ramRead(a, v); return v; }
  void ramw(uint32_t a, uint8_t v) { bool ch = false; chip.ramWrite(a, v, ch); }
};

struct MineRtc {
  std::vector<uint8_t> rom = std::vector<uint8_t>(0x100000, 0), sram;
  SPC7110 chip{rom, sram, true};
  SPC7110::Rtc& t = chip.rtcForTest();
  MineRtc() { t.followDevice = false; }  // contra o bsnes: só o chip, sem a hora do aparelho
  void w(int a, uint32_t v) { t.write(a, static_cast<uint8_t>(v)); }
  uint8_t r(int a) { return t.read(a); }
  void wait() { for (int i = 0; i < 8; i++) if (t.wait && --t.wait == 0) t.ready = 1; }
  void tick(int k) {
    switch (k) { case 0: t.tickSecond(); break; case 1: t.tickMinute(); break; case 2: t.tickHour(); break;
                 case 3: t.tickDay(); break; case 4: t.tickMonth(); break; default: t.tickYear(); break; }
  }
  void set(int f, uint8_t v) {
    uint8_t* fs[] = {&t.secondLo, &t.secondHi, &t.batteryFail, &t.minuteLo, &t.minuteHi, &t.resync, &t.hourLo, &t.hourHi,
                     &t.meridian, &t.dayLo, &t.dayHi, &t.dayRam, &t.monthLo, &t.monthHi, &t.monthRam, &t.yearLo, &t.yearHi,
                     &t.weekday, &t.hold, &t.calendar, &t.irqFlag, &t.roundSec, &t.irqMask, &t.irqDuty, &t.irqPeriod,
                     &t.pause, &t.stop, &t.atime, &t.test};
    *fs[f] = v;
    t.mask();
  }
};

static void chipTests() {
  const std::vector<uint8_t> rom = gen::makeRom();
  int same = 0;
  for (int k = 0; k < gen::SCRIPTS; k++) {
    Mine m(rom);
    const uint32_t got = gen::script(m, k);
    char what[64];
    std::snprintf(what, sizeof what, "roteiro %d igual ao bsnes", k);
    check(got == GOLDEN[k], what, got, GOLDEN[k]);
    if (got == GOLDEN[k]) same++;
  }
  std::printf("  chip: %d de %d roteiros iguais ao bsnes\n", same, gen::SCRIPTS);
  same = 0;
  for (int k = 0; k < gen::RTC_SCRIPTS; k++) {
    MineRtc m;
    const uint32_t got = gen::rtcScript(m, k);
    char what[64];
    std::snprintf(what, sizeof what, "relógio: roteiro %d igual ao bsnes", k);
    check(got == RTC_GOLDEN[k], what, got, RTC_GOLDEN[k]);
    if (got == RTC_GOLDEN[k]) same++;
  }
  std::printf("  relógio: %d de %d roteiros iguais ao bsnes\n", same, gen::RTC_SCRIPTS);
}

// ------------------------------------------------------------------ chip no barramento
static std::vector<uint8_t> fakeRom(uint8_t chip, size_t size) {
  std::vector<uint8_t> r = gen::makeRom();
  r.resize(size);
  const size_t h = 0xFFC0;
  std::memcpy(&r[h], "INDIGO SPC7110 TESTE ", 21);
  r[h + 0x15] = 0x3A;
  r[h + 0x16] = chip;
  r[h + 0x17] = 0x0C;
  r[h + 0x18] = 0x03;  // 8 KB de RAM
  r[h + 0x1C] = 0xFF; r[h + 0x1D] = 0xFF; r[h + 0x1E] = 0x00; r[h + 0x1F] = 0x00;
  r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
  r[0x8000] = 0x78;  // SEI
  r[0x8001] = 0x80; r[0x8002] = 0xFE;  // BRA para ela mesma
  return r;
}

static void busTests() {
  using indigo::snes::Cart;
  Cart cart;
  std::string err;
  const std::vector<uint8_t> rom = fakeRom(0xF5, 0x300000);
  check(indigo::snes::parseRom(rom, cart, err), "cabeçalho de SPC7110 aceito");
  check(cart.spc7110 && !cart.epsonRtc, "detectado como SPC7110 sem relógio ($F5)");
  check(cart.sram.size() == 0x2000 && cart.battery, "8 KB de RAM com bateria", static_cast<long>(cart.sram.size()), 0x2000);
  indigo::snes::SNES s(cart);

  // ROM de programa e janelas da ROM de dados
  check(s.read(0x008123) == rom[0x8123], "$00:8123 = ROM de programa $8123");
  check(s.read(0xC12345) == rom[0x012345], "$C1:2345 = ROM de programa $12345");
  check(s.read(0x8F8000) == rom[0x0F8000], "$8F:8000 = ROM de programa $F8000");
  check(s.read(0xD01234) == rom[0x100000 + 0x001234], "$D0:1234 = dados (janela 0 em $4831)");
  check(s.read(0xE01234) == rom[0x100000 + 0x001234], "1 MB de dados ($4834 = 0): $E0 espelha o 1º MB");
  s.write(0x004834, 0x01);  // 2 MB de dados
  check(s.read(0xE01234) == rom[0x100000 + 0x101234], "$4834 = 1 (2 MB): $E0:1234 = 2º MB de dados ($4832 = 1)");
  check(s.read(0xF01234) == rom[0x100000 + 0x001234], "$4833 = 2 com 2 MB: espelha o 1º MB", s.read(0xF01234), rom[0x101234]);
  s.write(0x004833, 0x01);
  check(s.read(0xF01234) == rom[0x100000 + 0x101234], "$4833 = 1: $F0 = 2º MB de dados");
  check(s.read(0x3F8000) == rom[0x100000 + 0x1F8000], "$3F:8000 espelha $FF:8000");
  check(s.read(0x004833) == 0x01 && s.read(0x004832) == 0x01 && s.read(0x004831) == 0x00, "registradores do mapa");

  // porta de dados em $4810 e em $50:xxxx
  s.write(0x004818, 0x00);
  s.write(0x004811, 0x34); s.write(0x004812, 0x12); s.write(0x004813, 0x00);
  check(s.read(0x004810) == rom[0x101234], "$4810 = dados[$1234]");
  check(s.read(0x004810) == rom[0x101235] && s.read(0x004811) == 0x36, "$4810 avança 1 a cada leitura");
  // $50:xxxx = $4800 (porta de descompressão); $58:xxxx = $4808 (sempre 0)
  check(s.read(0x585555) == 0x00, "$58:5555 = $4808 (0)");

  // descompressão: o DMA de $4800 entrega o mesmo que o chip sozinho
  std::vector<uint8_t> want(2048);
  {
    std::vector<uint8_t> romCopy = rom, ram(0x2000);
    SPC7110 ref(romCopy, ram, false);
    ref.ioWrite(0x4834, 0x01); ref.ioWrite(0x4804, 0x07); ref.ioWrite(0x480B, 0x00);
    ref.ioWrite(0x4806, 0x00); ref.run(64);
    for (auto& b : want) b = ref.ioRead(0x4800, 0);
  }
  s.write(0x004801, 0); s.write(0x004802, 0); s.write(0x004803, 0);
  s.write(0x004804, 0x07); s.write(0x00480B, 0x00);
  s.write(0x004806, 0x00);
  check(!(s.read(0x00480C) & 0x80), "logo depois de $4806, $480C ainda ocupado");
  for (int i = 0; i < 8; i++) s.read(0x000000);
  check(s.read(0x00480C) & 0x80, "pouco depois, $480C pronto (bit 7)");
  s.write(0x002181, 0x00); s.write(0x002182, 0x00); s.write(0x002183, 0x00);
  s.write(0x004300, 0x08); s.write(0x004301, 0x80);
  s.write(0x004302, 0x00); s.write(0x004303, 0x48); s.write(0x004304, 0x00);
  s.write(0x004305, 0x00); s.write(0x004306, 0x04);  // 1.024 bytes
  s.write(0x00420B, 0x01);
  bool same = true;
  for (int i = 0; i < 1024; i++) same &= s.peek(0x7E0000 + i) == want[i];
  check(same, "DMA de $4800: 1.024 bytes iguais ao chip sozinho");
  // estado salvo no meio do fluxo; e $50:xxxx continua o mesmo fluxo
  const std::vector<uint8_t> st = s.saveState();
  std::vector<uint8_t> a(512), b(512);
  for (auto& v : a) v = s.read(0x501234);
  s.loadState(st);
  for (auto& v : b) v = s.read(0x500000);
  check(a == b, "estado salvo no meio do fluxo continua igual");
  check(std::equal(a.begin(), a.end(), want.begin() + 1024), "$50:xxxx continua o fluxo do $4800");

  // contas: multiplicação com sinal fica pronta depois de uns 30 ciclos
  s.write(0x00482E, 0x01);
  s.write(0x004820, 0x10); s.write(0x004821, 0xFF);  // -240
  s.write(0x004824, 0x03); s.write(0x004825, 0x00);  // 3
  check(s.read(0x00482F) & 0x80, "multiplicação: ocupado logo depois de $4825");
  for (int i = 0; i < 8; i++) s.read(0x000000);
  check(!(s.read(0x00482F) & 0x80), "multiplicação pronta");
  const uint32_t p = s.read(0x004828) | (s.read(0x004829) << 8) | (s.read(0x00482A) << 16) | (uint32_t(s.read(0x00482B)) << 24);
  check(p == static_cast<uint32_t>(-720), "-240 × 3 = -720", p, static_cast<uint32_t>(-720));
  s.write(0x00482E, 0x00);
  s.write(0x004820, 0x40); s.write(0x004821, 0x42); s.write(0x004822, 0x0F); s.write(0x004823, 0x00);  // 1.000.000
  s.write(0x004826, 0xE8); s.write(0x004827, 0x03);  // 1.000
  for (int i = 0; i < 8; i++) s.read(0x000000);
  check((s.read(0x004828) | (s.read(0x004829) << 8)) == 1000 && s.read(0x00482C) == 0, "1.000.000 ÷ 1.000 = 1.000, resto 0");

  // RAM: só com o bit 7 de $4830
  s.write(0x004830, 0x00);
  s.write(0x006010, 0x5A);
  check(s.read(0x006010) == 0x00 && s.cart.sram[0x10] != 0x5A, "sem o bit 7 de $4830 a RAM fica desligada");
  s.write(0x004830, 0x80);
  s.write(0x006010, 0x5A);
  check(s.read(0x806010) == 0x5A && s.cart.sram[0x10] == 0x5A, "com o bit 7 a RAM grava (save de bateria)");
  s.write(0x3F7FFF, 0xA5);
  check(s.read(0x007FFF) == 0xA5 && s.cart.sram[0x1FFF] == 0xA5, "$6000-$7FFF de todos os bancos = a mesma RAM de 8 KB");

  // Far East of Eden Zero: com o relógio em $4840-$4842, seguindo a hora do aparelho (aqui uma hora fixa:
  // sábado, 31/10/2026, 15:04:05)
  static std::time_t fixed = 0;
  {
    std::tm tm{};
    tm.tm_year = 126; tm.tm_mon = 9; tm.tm_mday = 31; tm.tm_hour = 15; tm.tm_min = 4; tm.tm_sec = 5; tm.tm_isdst = -1;
    fixed = std::mktime(&tm);
  }
  SPC7110::deviceClock = [] { return fixed; };
  Cart c2;
  check(indigo::snes::parseRom(fakeRom(0xF9, 0x500000), c2, err) && c2.spc7110 && c2.epsonRtc, "$F9: SPC7110 com o relógio (5 MB)");
  indigo::snes::SNES s2(c2);
  auto wait = [&] { for (int i = 0; i < 20; i++) s2.read(0x000000); };
  s2.write(0x004840, 0x01);
  check(s2.read(0x004842) == 0x80, "relógio pronto depois de selecionado");
  s2.write(0x004840, 0x00);
  auto readAll = [&](uint8_t* t) {  // os 16 registradores pelo protocolo
    s2.write(0x004840, 0x01); s2.write(0x004841, 0x0C); wait();
    s2.write(0x004841, 0x00);
    for (int i = 0; i < 16; i++) { wait(); t[i] = s2.read(0x004841) & 15; }
    s2.write(0x004840, 0x00);
  };
  auto put = [&](int reg, const std::vector<uint8_t>& vals) {
    s2.write(0x004840, 0x01); s2.write(0x004841, 0x03); wait();
    s2.write(0x004841, static_cast<uint8_t>(reg));
    for (uint8_t v : vals) { wait(); s2.write(0x004841, v); }
    wait();
    s2.write(0x004840, 0x00);
  };
  uint8_t t[16];
  readAll(t);
  check(!(t[1] & 8), "hora do aparelho: sem o aviso de bateria acabada");
  check(t[0] == 5 && (t[1] & 7) == 0 && t[2] == 4 && (t[3] & 7) == 0 && t[4] == 3 && (t[5] & 3) == 0 && (t[5] & 4),
        "ao ligar: 03:04:05 da tarde (12 horas, PM)");
  check(t[6] == 1 && (t[7] & 3) == 3 && t[8] == 0 && (t[9] & 1) == 1 && t[10] == 6 && t[11] == 2 && (t[12] & 7) == 6,
        "ao ligar: sábado, 31/10/26");
  check(t[13] & 2, "calendário ligado (a data anda à meia-noite)");
  put(15, {0x4});  // o jogo passa para 24 horas
  readAll(t);
  check(t[4] == 5 && (t[5] & 3) == 1 && !(t[5] & 4) && (t[15] & 4), "trocou para 24 horas: 15:04 do aparelho");
  // a hora que o jogo grava vale até desligar: acerta 12:59:59 e espera um pouco mais de um segundo
  put(0, {9, 5, 9, 5, 2, 1});
  for (int f = 0; f < 70; f++) s2.runFrame();
  readAll(t);
  check(t[0] <= 1 && (t[1] & 7) == 0 && t[2] == 0 && (t[3] & 7) == 0 && t[4] == 3 && (t[5] & 3) == 1,
        "relógio anda: 12:59:59 vira 13:00:00 depois de um segundo de jogo");
  const std::vector<uint8_t> st2 = s2.saveState();
  s2.loadState(st2);
  s2.followHostClock();  // o app chama depois de carregar
  readAll(t);
  check(t[4] == 5 && (t[5] & 3) == 1 && t[2] == 4 && (t[15] & 4), "estado carregado: volta para a hora do aparelho (no formato de 24 horas do estado)");
  s2.loadState(st2);
  const std::vector<uint8_t> st3 = s2.saveState();
  s2.loadState(st3);
  check(s2.saveState() == st3, "estado com o relógio volta igual");

  indigo::snes::SNESCore core(fakeRom(0xF9, 0x500000), nullptr);
  check(core.hasSave(), "save de bateria (só a RAM de 8 KB)");
  check(core.exportSave().size() == 0x2000, "save com 8 KB", static_cast<long>(core.exportSave().size()), 0x2000);
}

int main() {
  std::printf("== SPC7110: chip e relógio contra o bsnes\n");
  chipTests();
  std::printf("== SPC7110: chip no barramento\n");
  busTests();
  std::printf("SPC7110: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
#endif
