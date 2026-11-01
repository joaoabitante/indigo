// Teste dos mapeadores com chips próprios (MMC5, VRC6, VRC7, Namco 163): monta cartuchos sintéticos em que
// cada banco de 8 KB de PRG e cada banco de 1 KB de CHR está preenchido com o próprio número, grava os
// registradores como um jogo faria e confere o que a CPU e o PPU passam a enxergar, segundo a documentação
// pública de cada chip (wiki do NESdev).
//   indigo-nesmappertest        imprime cada verificação e "0 problemas" no fim
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/nes/nes.h"

using namespace indigo::nes;

static int bad = 0, total = 0;

static void check(const char* what, int got, int want) {
  total++;
  if (got != want) { bad++; std::printf("  ERRO %s: veio %d, esperado %d\n", what, got, want); }
}

static Cart makeCart(int mapper, int prgKb, int chrKb, bool battery = false) {
  Cart c;
  c.mapper = mapper;
  c.prg.resize(size_t(prgKb) * 1024);
  for (size_t i = 0; i < c.prg.size(); i++) c.prg[i] = static_cast<uint8_t>(i / 0x2000);
  c.chr.resize(size_t(chrKb) * 1024);
  for (size_t i = 0; i < c.chr.size(); i++) c.chr[i] = static_cast<uint8_t>(i / 0x400);
  c.prgRam.assign(0x2000, 0);
  c.battery = battery;
  c.vertical = true;
  return c;
}

static int prgAt(NES& n, uint16_t a) { return n.peek(a); }
static int chrAt(NES& n, uint16_t a) { return n.mapper->chrRead(a); }

static void mmc5() {
  std::printf("== MMC5 (5)\n");
  NES n(makeCart(5, 512, 512));
  check("RAM de 64 KB sem cabeçalho NES 2.0", static_cast<int>(n.cart.prgRam.size()), 0x10000);
  check("$E000 ao ligar = último banco", prgAt(n, 0xE000), 63);
  // modo 3: quatro bancos de 8 KB
  n.write(0x5100, 3);
  n.write(0x5114, 0x85); n.write(0x5115, 0x86); n.write(0x5116, 0x87); n.write(0x5117, 0x09);
  check("modo 3 $8000", prgAt(n, 0x8000), 5);
  check("modo 3 $A000", prgAt(n, 0xA000), 6);
  check("modo 3 $C000", prgAt(n, 0xC000), 7);
  check("modo 3 $E000", prgAt(n, 0xE000), 9);
  // modo 1: dois bancos de 16 KB
  n.write(0x5100, 1);
  n.write(0x5115, 0x85); n.write(0x5117, 0x0B);
  check("modo 1 $8000 (bit 0 ignorado)", prgAt(n, 0x8000), 4);
  check("modo 1 $A000", prgAt(n, 0xA000), 5);
  check("modo 1 $C000", prgAt(n, 0xC000), 10);
  check("modo 1 $E000", prgAt(n, 0xE000), 11);
  // modo 2: 16 KB + 8 KB + 8 KB
  n.write(0x5100, 2);
  n.write(0x5115, 0x82); n.write(0x5116, 0x8C); n.write(0x5117, 0x1F);
  check("modo 2 $8000", prgAt(n, 0x8000), 2);
  check("modo 2 $A000", prgAt(n, 0xA000), 3);
  check("modo 2 $C000", prgAt(n, 0xC000), 12);
  check("modo 2 $E000", prgAt(n, 0xE000), 31);
  // modo 0: 32 KB
  n.write(0x5100, 0);
  n.write(0x5117, 0x0E);
  check("modo 0 $8000", prgAt(n, 0x8000), 12);
  check("modo 0 $E000", prgAt(n, 0xE000), 15);
  // RAM: só grava com $5102 = 2 e $5103 = 1
  n.write(0x5113, 1);
  n.write(0x6000, 0x55);
  check("RAM protegida não grava", prgAt(n, 0x6000), 0);
  n.write(0x5102, 2); n.write(0x5103, 1);
  n.write(0x6000, 0x55);
  check("RAM liberada grava", prgAt(n, 0x6000), 0x55);
  n.write(0x5113, 2);
  check("outro banco de RAM", prgAt(n, 0x6000), 0);
  n.write(0x5113, 1);
  check("volta ao banco 1", prgAt(n, 0x6000), 0x55);
  n.write(0x5100, 3); n.write(0x5114, 0x01);
  check("RAM em $8000 (bit 7 = 0)", prgAt(n, 0x8000), 0x55);
  n.write(0x8001, 0x66);
  check("grava na RAM em $8000", prgAt(n, 0x8001), 0x66);
  n.write(0x5117, 0x01);
  check("$E000 é sempre ROM", prgAt(n, 0xE000), 1);
  // multiplicador
  n.write(0x5205, 200); n.write(0x5206, 3);
  check("multiplicador (baixo)", n.read(0x5205), 600 & 0xFF);
  check("multiplicador (alto)", n.read(0x5206), 600 >> 8);
  // ExRAM: modo 2 lê e grava; modo 3 só lê
  n.write(0x5104, 2); n.write(0x5C10, 0x77);
  check("ExRAM modo 2", n.read(0x5C10), 0x77);
  n.write(0x5104, 3); n.write(0x5C10, 0x11);
  check("ExRAM modo 3 só leitura", n.read(0x5C10), 0x77);
  // CHR em bancos de 1 KB: o último conjunto gravado vale com o desenho desligado
  n.write(0x5101, 3);
  for (int k = 0; k < 8; k++) n.write(static_cast<uint16_t>(0x5120 + k), static_cast<uint8_t>(10 + k));
  for (int k = 0; k < 8; k++) check("CHR A 1 KB", chrAt(n, static_cast<uint16_t>(k * 0x400)), 10 + k);
  for (int k = 0; k < 4; k++) n.write(static_cast<uint16_t>(0x5128 + k), static_cast<uint8_t>(20 + k));
  for (int k = 0; k < 8; k++) check("CHR B 1 KB (repete a cada 4 KB)", chrAt(n, static_cast<uint16_t>(k * 0x400)), 20 + (k & 3));
  n.write(0x5101, 0); n.write(0x5127, 3);
  check("CHR 8 KB", chrAt(n, 0x1C00), 3 * 8 + 7);
  n.write(0x5101, 1); n.write(0x5123, 2); n.write(0x5127, 5);
  check("CHR 4 KB baixo", chrAt(n, 0x0400), 2 * 4 + 1);
  check("CHR 4 KB alto", chrAt(n, 0x1000), 5 * 4);
  n.write(0x5101, 3); n.write(0x5130, 1); n.write(0x5120, 2);
  check("bits altos do CHR ($5130)", chrAt(n, 0x0000), (0x102) & 0xFF);
  // tabelas de nomes: preenchimento
  n.write(0x5105, 0xFF); n.write(0x5106, 0x42); n.write(0x5107, 2);
  check("preenchimento: telha", n.mapper->ntRead(0x2000), 0x42);
  check("preenchimento: atributo", n.mapper->ntRead(0x23C0), 0xAA);
  n.write(0x5105, 0x44);  // 0,1,0,1 → telas 0 e 2 na CIRAM A, 1 e 3 na B
  n.mapper->ntWrite(0x2400, 0x99);
  check("CIRAM B pela tela 1", n.mapper->ntRead(0x2C00), 0x99);
  check("CIRAM A separada", n.mapper->ntRead(0x2000), 0);
}

static void vrc6(int id) {
  std::printf("== VRC6 (%d)\n", id);
  NES n(makeCart(id, 256, 256));
  // no 26 as linhas A0 e A1 vêm trocadas
  auto reg = [&](uint16_t a) -> uint16_t { return id == 26 ? static_cast<uint16_t>((a & ~3) | ((a & 1) << 1) | ((a >> 1) & 1)) : a; };
  n.write(reg(0x8000), 3); n.write(reg(0xC000), 9);
  check("$8000 (16 KB)", prgAt(n, 0x8000), 6);
  check("$A000", prgAt(n, 0xA000), 7);
  check("$C000 (8 KB)", prgAt(n, 0xC000), 9);
  check("$E000 fixo", prgAt(n, 0xE000), 31);
  for (int k = 0; k < 8; k++) n.write(reg(static_cast<uint16_t>((k < 4 ? 0xD000 : 0xE000) + (k & 3))), static_cast<uint8_t>(30 + k));
  for (int k = 0; k < 8; k++) check("CHR 1 KB", chrAt(n, static_cast<uint16_t>(k * 0x400)), 30 + k);
  n.write(reg(0xB003), 0x84);
  check("espelhamento horizontal", n.mapper->nt[1] * 10 + n.mapper->nt[2], 1);
  n.write(0x6000, 0x5A);
  check("RAM com $B003 bit 7", prgAt(n, 0x6000), 0x5A);
  // IRQ em modo de ciclos: latch $FE conta 2 ciclos até estourar
  n.write(reg(0xF000), 0xFE); n.write(reg(0xF001), 0x06);
  n.read(0); n.read(0);
  check("IRQ do VRC6", n.mapper->irq ? 1 : 0, 1);
  n.write(reg(0xF002), 0);
  check("IRQ reconhecido", n.mapper->irq ? 1 : 0, 0);
}

static void vrc7() {
  std::printf("== VRC7 (85)\n");
  NES n(makeCart(85, 512, 256));
  n.write(0x8000, 4); n.write(0x8010, 5); n.write(0x9000, 6);
  check("$8000", prgAt(n, 0x8000), 4);
  check("$A000 (VRC7a, A4)", prgAt(n, 0xA000), 5);
  check("$C000", prgAt(n, 0xC000), 6);
  check("$E000 fixo", prgAt(n, 0xE000), 63);
  n.write(0x8008, 7);
  check("$A000 (VRC7b, A3)", prgAt(n, 0xA000), 7);
  const uint16_t regs[8] = {0xA000, 0xA010, 0xB000, 0xB010, 0xC000, 0xC010, 0xD000, 0xD010};
  for (int k = 0; k < 8; k++) n.write(regs[k], static_cast<uint8_t>(40 + k));
  for (int k = 0; k < 8; k++) check("CHR 1 KB", chrAt(n, static_cast<uint16_t>(k * 0x400)), 40 + k);
  n.write(0xE000, 0x81);
  check("espelhamento horizontal", n.mapper->nt[1] * 10 + n.mapper->nt[2], 1);
  n.write(0x6000, 0x3C);
  check("RAM com $E000 bit 7", prgAt(n, 0x6000), 0x3C);
  n.write(0xE010, 0xFF); n.write(0xF000, 0x06);
  n.read(0);
  check("IRQ do VRC7", n.mapper->irq ? 1 : 0, 1);
  n.write(0xF010, 0);
  check("IRQ reconhecido", n.mapper->irq ? 1 : 0, 0);
}

static void namco163() {
  std::printf("== Namco 163 (19)\n");
  NES n(makeCart(19, 256, 256, true));
  check("RAM de 8 KB + 128 bytes de som", static_cast<int>(n.cart.prgRam.size()), 0x2080);
  n.write(0xE000, 3); n.write(0xE800, 4); n.write(0xF000, 5);
  check("$8000", prgAt(n, 0x8000), 3);
  check("$A000", prgAt(n, 0xA000), 4);
  check("$C000", prgAt(n, 0xC000), 5);
  check("$E000 fixo", prgAt(n, 0xE000), 31);
  for (int k = 0; k < 8; k++) n.write(static_cast<uint16_t>(0x8000 + k * 0x800), static_cast<uint8_t>(10 + k));
  for (int k = 0; k < 8; k++) check("CHR 1 KB", chrAt(n, static_cast<uint16_t>(k * 0x400)), 10 + k);
  // tabela de nomes: $E0/$E1 = CIRAM; outro valor = banco de CHR ROM
  n.write(0xC000, 0xE1); n.write(0xC800, 0x05);
  n.mapper->ntWrite(0x2010, 0x4D);
  check("tela na CIRAM", n.mapper->ntRead(0x2010), 0x4D);
  check("tela na CHR ROM", n.mapper->ntRead(0x2400), 5);
  // CHR com valor $E0 usa a CIRAM, a menos que o bit de $E800 desligue
  n.write(0x8000, 0xE1);
  check("CHR na CIRAM", chrAt(n, 0x0010), 0x4D);
  n.write(0xE800, 0x44);
  check("CHR na CIRAM desligada", chrAt(n, 0x0010), 0xE1);
  // proteção da RAM pelo $F800
  n.write(0xF800, 0x40); n.write(0x6000, 0x12);
  check("RAM liberada", prgAt(n, 0x6000), 0x12);
  n.write(0xF800, 0x41); n.write(0x6000, 0x34);
  check("RAM protegida", prgAt(n, 0x6000), 0x12);
  check("save marcado", n.dirty ? 1 : 0, 1);
  // RAM de som com autoincremento
  n.write(0xF800, 0x80); n.write(0x4800, 0xAB); n.write(0x4800, 0xCD);
  n.write(0xF800, 0x01);
  check("RAM de som (endereço 1)", n.read(0x4800), 0xCD);
  check("sem autoincremento", n.read(0x4800), 0xCD);
  check("RAM de som no fim do save", n.cart.prgRam[0x2000], 0xAB);
  // IRQ: conta para cima e para em $7FFF
  n.write(0x5000, 0xFD); n.write(0x5800, 0xFF);
  check("contador lido", n.read(0x5800), 0xFF);
  n.read(0); n.read(0);
  check("IRQ do Namco 163", n.mapper->irq ? 1 : 0, 1);
  n.write(0x5800, 0x7F);
  check("IRQ reconhecido", n.mapper->irq ? 1 : 0, 0);
}

// ---------------------------------------------------------------- som: ROM mínima que liga um canal numa nota
// conhecida; a frequência medida (autocorrelação do som a 48 kHz, depois do primeiro segundo) tem de bater com a
// fórmula do chip
static std::vector<uint8_t> soundRom(int mapper, const std::vector<std::pair<uint16_t, uint8_t>>& writes) {
  std::vector<uint8_t> code;
  for (auto [a, v] : writes) { code.insert(code.end(), {0xA9, v, 0x8D, static_cast<uint8_t>(a), static_cast<uint8_t>(a >> 8)}); }
  const uint16_t loop = static_cast<uint16_t>(0xE000 + code.size());
  code.insert(code.end(), {0x4C, static_cast<uint8_t>(loop), static_cast<uint8_t>(loop >> 8)});
  const uint16_t rti = static_cast<uint16_t>(0xE000 + code.size());
  code.push_back(0x40);
  std::vector<uint8_t> rom(16 + 0x8000 + 0x2000, 0);
  const uint8_t hdr[8] = {'N', 'E', 'S', 0x1A, 2, 1, static_cast<uint8_t>((mapper & 15) << 4), static_cast<uint8_t>(mapper & 0xF0)};
  std::copy(hdr, hdr + 8, rom.begin());
  std::copy(code.begin(), code.end(), rom.begin() + 16 + 0x6000);
  const uint16_t vec[3] = {rti, 0xE000, rti};
  for (int k = 0; k < 3; k++) { rom[16 + 0x7FFA + 2 * k] = static_cast<uint8_t>(vec[k]); rom[16 + 0x7FFB + 2 * k] = static_cast<uint8_t>(vec[k] >> 8); }
  return rom;
}

static void soundCheck(const char* name, int mapper, const std::vector<std::pair<uint16_t, uint8_t>>& writes, double want) {
  Cart c;
  std::string err;
  parseRom(soundRom(mapper, writes), c, err);
  NES n(c);
  n.apu.setSampleRate(48000);
  std::vector<float> x;
  for (int f = 0; f < 120; f++) {
    n.runFrame();
    std::vector<float> L(n.apu.available()), R(L.size());
    n.apu.pull(L.data(), R.data(), L.size());
    if (f >= 60) x.insert(x.end(), L.begin(), L.end());
  }
  double mean = 0, peak = 0;
  for (float v : x) mean += v;
  mean /= static_cast<double>(x.size());
  for (float& v : x) { v -= static_cast<float>(mean); peak = std::max(peak, std::fabs(static_cast<double>(v))); }
  // autocorrelação normalizada; o primeiro pico perto do máximo dá o período (evita errar por uma oitava)
  const int minLag = 48000 / 3000, maxLag = 48000 / 60, N = static_cast<int>(x.size()) - maxLag;
  std::vector<double> r(maxLag + 2, 0);
  double best = 0;
  for (int lag = minLag; lag <= maxLag + 1; lag++) {
    double s = 0;
    for (int i = 0; i < N; i++) s += x[i] * x[i + lag];
    r[lag] = s;
    if (lag <= maxLag) best = std::max(best, s);
  }
  double got = 0;
  for (int lag = minLag + 1; lag <= maxLag; lag++) {
    if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
      const double d = (r[lag - 1] - r[lag + 1]) / (2 * (r[lag - 1] - 2 * r[lag] + r[lag + 1]));
      got = 48000.0 / (lag + d);
      break;
    }
  }
  total++;
  const double errPct = std::fabs(got - want) / want * 100;
  const bool ok = errPct < 1.0 && peak > 0.02 && peak < 0.6;
  if (!ok) bad++;
  std::printf("  %s %-26s esperado %7.2f Hz, medido %7.2f Hz, pico %.3f\n", ok ? "som" : "ERRO som", name, want, got, peak);
}

static void sound() {
  std::printf("== Som dos chips\n");
  const double CPU = 1789773.0;
  const int p = 0x1FC;
  const uint8_t lo = p & 0xFF, hi = p >> 8;
  soundCheck("VRC6 quadrada", 24, {{0x9003, 0}, {0x9000, 0x7F}, {0x9001, lo}, {0x9002, static_cast<uint8_t>(0x80 | hi)}}, CPU / (16 * (p + 1)));
  soundCheck("VRC6 (26) quadrada", 26, {{0x9003, 0}, {0x9000, 0x7F}, {0x9002, lo}, {0x9001, static_cast<uint8_t>(0x80 | hi)}}, CPU / (16 * (p + 1)));
  soundCheck("VRC6 dente de serra", 24, {{0x9003, 0}, {0xB000, 0x2A}, {0xB001, lo}, {0xB002, static_cast<uint8_t>(0x80 | hi)}}, CPU / (14 * (p + 1)));
  soundCheck("VRC6 frequência x16", 24, {{0x9003, 2}, {0x9000, 0x7F}, {0x9001, 0xC0}, {0x9002, 0x8F}}, CPU / (16 * ((0xFC0 >> 4) + 1)));
  soundCheck("MMC5 quadrada", 5, {{0x5015, 1}, {0x5000, 0xBF}, {0x5002, lo}, {0x5003, hi}}, CPU / (16 * (p + 1)));
  soundCheck("Sunsoft 5B tom", 69, {{0xC000, 7}, {0xE000, 0x3E}, {0xC000, 8}, {0xE000, 0x0F}, {0xC000, 0}, {0xE000, 0xFE}, {0xC000, 1}, {0xE000, 0}}, CPU / (32 * 0xFE));
  // Namco 163: dente de serra de 16 amostras no endereço 0, canal 7 sozinho
  std::vector<std::pair<uint16_t, uint8_t>> w = {{0xE000, 0x00}, {0xF800, 0x80}};
  for (int i = 0; i < 8; i++) w.push_back({0x4800, static_cast<uint8_t>(((2 * i) & 15) | (((2 * i + 1) & 15) << 4))});
  const int freq = 0x1000;
  w.push_back({0xF800, 0xF8});
  for (int v : {freq & 0xFF, 0, (freq >> 8) & 0xFF, 0, (256 - 16) | ((freq >> 16) & 3), 0, 0, 0x0F}) w.push_back({0x4800, static_cast<uint8_t>(v)});
  soundCheck("Namco 163 onda", 19, w, CPU * freq / (15.0 * 65536 * 1 * 16));
  // VRC7: instrumento 3 (multiplicador 1), fnum $100, bloco 4
  soundCheck("VRC7 FM", 85, {{0x9010, 0x30}, {0x9030, 0x30}, {0x9010, 0x10}, {0x9030, 0x00}, {0x9010, 0x20}, {0x9030, 0x10 | (4 << 1) | 1}},
             49716.0 * 0x100 * 8 / 262144.0);
}

int main() {
  sound();
  mmc5();
  vrc6(24);
  vrc6(26);
  vrc7();
  namco163();
  std::printf("%d verificações, %d problemas\n", total, bad);
  return bad ? 1 : 0;
}
