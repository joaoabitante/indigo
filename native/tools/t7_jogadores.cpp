// Teste dos dois jogadores nos consoles que não têm um teste próprio da etapa 7 (NES, SNES e Master System):
// o controle 2 (Core::setKeys2) chega à porta 2 do console e não mexe no controle 1. Monta cartuchos mínimos
// no próprio teste (sem arquivos).
//   indigo-t7_jogadores
#include <cstdio>
#include <string>
#include <vector>

#include "../src/nes/nes.h"
#include "../src/sms/sms.h"
#include "../src/snes/snes.h"

using namespace indigo;

namespace {

int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) fails++;
  std::printf("%s %s\n", ok ? "ok   " : "FALHA", what.c_str());
}

// NES: $4016 bit 0 = strobe; cada leitura de $4016/$4017 devolve o próximo botão (A B Select Start ↑ ↓ ← →)
int nesPort(nes::NES& n, uint16_t port) {
  n.write(0x4016, 1);
  n.write(0x4016, 0);
  int v = 0;
  for (int i = 0; i < 8; i++) v |= (n.read(port) & 1) << i;
  return v;
}

void testNes() {
  std::vector<uint8_t> rom = {'N', 'E', 'S', 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  rom.resize(16 + 0x4000 + 0x2000, 0xEA);
  rom[16 + 0x3FFC] = 0x00; rom[16 + 0x3FFD] = 0xC0;
  nes::NESCore c(rom, nullptr);
  check(c.players() == 2, "NES: players() == 2");
  c.setKeys(KEY_A | KEY_UP);
  c.setKeys2(KEY_B | KEY_START | KEY_RIGHT);
  check(nesPort(c.nes(), 0x4016) == 0x11, "NES: $4016 só com o controle 1 (" + std::to_string(nesPort(c.nes(), 0x4016)) + ")");
  check(nesPort(c.nes(), 0x4017) == 0x8A, "NES: $4017 com o controle 2 (" + std::to_string(nesPort(c.nes(), 0x4017)) + ")");
  c.setKeys2(0);
  check(nesPort(c.nes(), 0x4017) == 0 && nesPort(c.nes(), 0x4016) == 0x11, "NES: controle 2 solto");
}

// SNES: leitura manual de $4016/$4017 (16 bits: B Y Select Start ↑ ↓ ← → A X L R 0 0 0 0) e a automática
uint32_t snesManual(snes::SNES& s, uint32_t port) {
  s.write(0x4016, 1);
  s.write(0x4016, 0);
  uint32_t v = 0;
  for (int i = 0; i < 17; i++) v = (v << 1) | (s.read(port) & 1);
  return v;  // 16 bits do controle e o 17º (1 = controle ligado)
}

void testSnes() {
  std::vector<uint8_t> rom(0x8000, 0);
  rom[0] = 0x80; rom[1] = 0xFE;  // BRA para si mesmo, em $8000
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
  snes::SNESCore c(rom, nullptr);
  snes::SNES& s = c.snes();
  check(c.players() == 2, "SNES: players() == 2");
  c.setKeys(KEY_B | KEY_L);
  check(snesManual(s, 0x4017) == 0, "SNES: sem setKeys2 a porta 2 fica vazia");
  c.setKeys2(KEY_A | KEY_UP);
  check(snesManual(s, 0x4016) == ((0x8020u << 1) | 1), "SNES: $4016 só com o controle 1");
  check(snesManual(s, 0x4017) == ((0x0880u << 1) | 1), "SNES: $4017 com o controle 2");
  s.write(0x4200, 1);  // leitura automática no começo do vblank
  c.runFrame();
  auto joy = [&](uint32_t a) { return s.read(a) | (s.read(a + 1) << 8); };
  check(joy(0x4218) == 0x8020 && joy(0x421A) == 0x0880, "SNES: leitura automática ($4218 e $421A)");
}

// Master System: DCh = controle 1 (↑ ↓ ← → 1 2) + ↑ ↓ do 2; DDh = ← → 1 2 do controle 2 (0 = apertado)
void testSms() {
  std::vector<uint8_t> rom(0x8000, 0);
  sms::SMSCore c(rom, false, nullptr);
  sms::SMS& m = c.sms();
  check(c.players() == 2, "Master System: players() == 2");
  c.setKeys(KEY_LEFT | KEY_B);
  c.setKeys2(KEY_UP | KEY_RIGHT | KEY_A);
  check(m.in(0xDC) == 0xAB, "Master System: DCh com o controle 1 e o ↑ do 2 (" + std::to_string(m.in(0xDC)) + ")");
  check((m.in(0xDD) & 0x0F) == 0x05, "Master System: DDh com → e 2 do controle 2 (" + std::to_string(m.in(0xDD)) + ")");
  c.setKeys2(0);
  check(m.in(0xDC) == 0xEB && (m.in(0xDD) & 0x0F) == 0x0F, "Master System: controle 2 solto");
  sms::SMSCore gg(rom, true, nullptr);
  check(gg.players() == 1, "Game Gear: só 1 jogador");
}

}  // namespace

int main() {
  testNes();
  testSnes();
  testSms();
  std::printf("RESULTADO jogadores: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
