// Testes do SA-1 do SNES (src/snes/sa1.*). Não há ROMs de teste públicas para o chip, então este teste monta
// um cartucho de mentira com o cabeçalho de SA-1 ($FFD5 = $23, $FFD6 = $35) e um programa para o 65816 do
// SA-1, escrito aqui byte a byte. O lado do SNES conversa com o chip pelo barramento (como o 65816 do console
// faria) e confere o que o programa deixou na I-RAM e na BW-RAM:
//   proteção de escrita da I-RAM, reset e parada do SA-1 pelo SNES, multiplicação, divisão, soma de produtos,
//   BW-RAM pela janela, linear e como bitmap de 4 bits, DMA da ROM para a I-RAM, leitura de bits de tamanho
//   variável, conversão de bitmap tipos 1 e 2, mensagens e IRQ nos dois sentidos, troca do vetor de IRQ do
//   SNES, Super MMC (troca de bloco de 1 MB) e estado salvo.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/snes/snes.h"

static int checks = 0, fails = 0;
static void check(bool ok, const char* what, long got = -1, long want = -1) {
  checks++;
  if (ok) return;
  fails++;
  if (want >= 0) std::printf("  FALHA: %s (veio %lX, esperado %lX)\n", what, got, want);
  else std::printf("  FALHA: %s\n", what);
}

// ------------------------------------------------------------------ montador mínimo (modo emulação, 8 bits)
struct Asm {
  std::vector<uint8_t>& rom;
  size_t at;
  void b(std::initializer_list<int> v) { for (int x : v) rom[at++] = static_cast<uint8_t>(x); }
  void lda(int v) { b({0xA9, v}); }
  void ldaA(int a) { b({0xAD, a & 0xFF, a >> 8}); }
  void sta(int a) { b({0x8D, a & 0xFF, a >> 8}); }
  void stz(int a) { b({0x9C, a & 0xFF, a >> 8}); }
  void staL(int a) { b({0x8F, a & 0xFF, (a >> 8) & 0xFF, a >> 16}); }
  void put(int reg, int v) { if (v) { lda(v); sta(reg); } else stz(reg); }
  void copy(int from, int to) { ldaA(from); sta(to); }
};

static const uint8_t TABLE[16] = {0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB, 0xED, 0x0F, 1, 2, 3, 4, 5, 6, 7, 8};

static std::vector<uint8_t> fakeRom() {
  std::vector<uint8_t> r(0x200000, 0);  // 2 MB: o 2º MB serve para o teste do Super MMC
  const size_t h = 0x7FC0;
  const char* title = "INDIGO SA1 TESTE     ";
  for (int i = 0; i < 21; i++) r[h + i] = static_cast<uint8_t>(title[i]);
  r[h + 0x15] = 0x23;
  r[h + 0x16] = 0x35;  // ROM + SA-1 + RAM + bateria
  r[h + 0x17] = 0x0B;
  r[h + 0x18] = 0x03;  // 8 KB de BW-RAM
  r[h + 0x1C] = 0xFF; r[h + 0x1D] = 0xFF;
  r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
  r[0] = 0x78;  // SEI (programa do SNES, que o teste não roda)
  for (int i = 0; i < 16; i++) r[0x1000 + i] = TABLE[i];  // $00:9000
  r[0x100000] = 0x5B;                                      // primeiro byte do 2º MB
  r[0x1FFFFF] = 0xE7;

  Asm a{r, 0x100};  // programa do SA-1 em $00:8100
  a.put(0x222A, 0xFF);  // o SA-1 pode escrever em toda a I-RAM
  // multiplicação: -3 × 1000
  a.stz(0x2250);
  a.put(0x2251, 0xFD); a.put(0x2252, 0xFF); a.put(0x2253, 0xE8); a.put(0x2254, 0x03);
  for (int i = 0; i < 4; i++) a.copy(0x2306 + i, 0x3010 + i);
  // divisão: -1000 / 7 = -143 resto 1
  a.put(0x2250, 0x01);
  a.put(0x2251, 0x18); a.put(0x2252, 0xFC); a.put(0x2253, 0x07); a.put(0x2254, 0x00);
  for (int i = 0; i < 4; i++) a.copy(0x2306 + i, 0x3014 + i);
  // soma de produtos: 100 × 200 + (-50) × 30 = 18500
  a.put(0x2250, 0x02);
  a.put(0x2251, 100); a.put(0x2252, 0); a.put(0x2253, 200); a.put(0x2254, 0);
  a.put(0x2251, 0xCE); a.put(0x2252, 0xFF); a.put(0x2253, 30); a.put(0x2254, 0);
  for (int i = 0; i < 3; i++) a.copy(0x2306 + i, 0x3018 + i);
  a.copy(0x230B, 0x301B);
  // BW-RAM: pela janela $6000, linear ($40) e como bitmap de 4 bits ($60)
  a.stz(0x2225);
  a.lda(0x5A); a.sta(0x6000);
  a.lda(0xA5); a.staL(0x400001);
  a.stz(0x223F);
  a.lda(0x0C); a.staL(0x600002);
  a.lda(0x03); a.staL(0x600003);
  // DMA de 16 bytes da ROM ($00:9000) para a I-RAM ($3100)
  a.put(0x2230, 0x80);
  a.put(0x2232, 0x00); a.put(0x2233, 0x90); a.put(0x2234, 0x00);
  a.put(0x2238, 16); a.put(0x2239, 0);
  a.put(0x2235, 0x00); a.put(0x2236, 0x01);
  // bits de tamanho variável a partir de $00:9000: 16 bits, anda 4, mais 16
  a.put(0x2259, 0x00); a.put(0x225A, 0x90); a.put(0x225B, 0x00);
  a.copy(0x230C, 0x3020); a.copy(0x230D, 0x3021);
  a.put(0x2258, 0x04);
  a.copy(0x230C, 0x3022); a.copy(0x230D, 0x3023);
  // conversão tipo 2 (2 bits): duas linhas de 8 pontos para a I-RAM em $3200
  a.put(0x2230, 0xA0); a.put(0x2231, 0x02);
  a.put(0x2235, 0x00); a.put(0x2236, 0x02);
  const int line0[8] = {3, 0, 1, 2, 3, 0, 1, 2}, line1[8] = {0, 0, 0, 0, 0, 0, 0, 3};
  for (int i = 0; i < 8; i++) a.put(0x2240 + i, line0[i]);
  for (int i = 0; i < 8; i++) a.put(0x2248 + i, line1[i]);
  // conversão tipo 1 (2 bits, 1 bloco de largura): bitmap em BW-RAM $0100, blocos montados na I-RAM em $3300
  a.lda(0x93); a.staL(0x400100); a.staL(0x400101);
  a.put(0x2230, 0xB0); a.put(0x2231, 0x02);
  a.put(0x2232, 0x00); a.put(0x2233, 0x01); a.put(0x2234, 0x00);
  a.put(0x2235, 0x00); a.put(0x2236, 0x03);
  // Super MMC: o bloco D ($20-$3F:$8000) passa a mostrar o 2º MB
  a.put(0x2221, 0x81);
  a.lda(0); a.b({0xAF, 0x00, 0x80, 0x20}); a.sta(0x3030);  // LDA $208000
  a.b({0xAF, 0x00, 0x00, 0xD0}); a.sta(0x3031);            // LDA $D00000 ($D0-$DF seguem o mesmo $2221)
  // vetor de IRQ do SNES trocado por $1234, mensagem 5 e IRQ para o SNES
  a.put(0x220E, 0x34); a.put(0x220F, 0x12);
  a.put(0x2209, 0xC5);
  // espera IRQ do SNES: liga a interrupção do lado do SA-1, CLI e fica parado
  a.put(0x220A, 0x80);
  a.lda(0x77); a.sta(0x307F);
  a.b({0x58});        // CLI
  a.b({0x80, 0xFE});  // BRA *
  // tratador de IRQ do SA-1 em $00:8400
  a.at = 0x400;
  a.copy(0x2301, 0x3080);
  a.put(0x220B, 0x80);
  a.lda(0x99); a.sta(0x3081);
  a.b({0x40});  // RTI
  return r;
}

int main() {
  indigo::snes::Cart cart;
  std::string err;
  if (!indigo::snes::parseRom(fakeRom(), cart, err)) {
    std::printf("FALHA: %s\n", err.c_str());
    return 1;
  }
  check(cart.sa1, "reconhece o cabeçalho de SA-1");
  check(cart.sram.size() == 0x2000, "BW-RAM de 8 KB", long(cart.sram.size()), 0x2000);
  indigo::snes::SNES s(cart);
  check(s.sa1 != nullptr, "cria o SA-1");

  // I-RAM: protegida até o SNES liberar
  s.write(0x003001, 0x11);
  check(s.read(0x003001) == 0x00, "I-RAM protegida contra o SNES");
  s.write(0x002229, 0xFF);
  s.write(0x003000, 0xAB);
  check(s.read(0x003000) == 0xAB, "SNES escreve na I-RAM", s.read(0x003000), 0xAB);
  // ROM pelo lado do SNES (LoROM do SA-1)
  check(s.read(0x009000) == 0x21, "ROM em $00:9000");
  check(s.read(0xC00000) == 0x78, "ROM em $C0:0000");
  check(s.read(0x00FFEE) == 0x00, "vetor de IRQ do SNES ainda é o da ROM");

  // liga o SA-1: vetores de reset ($8100) e IRQ ($8400)
  s.write(0x002203, 0x00); s.write(0x002204, 0x81);
  s.write(0x002207, 0x00); s.write(0x002208, 0x84);
  s.write(0x002201, 0xA0);  // IRQ do SA-1 e da conversão para o SNES
  auto runUntil = [&](uint32_t addr, uint8_t v) {
    for (int i = 0; i < 200000; i++) { if (s.peek(addr) == v) return true; s.read(0x7E0000); }
    return false;
  };
  check(!runUntil(0x00307F, 0x77), "SA-1 parado não roda");
  s.write(0x002200, 0x00);
  check(runUntil(0x00307F, 0x77), "SA-1 roda o programa até o fim");

  auto iram = [&](int a) { return static_cast<long>(s.peek(0x003000 + a)); };
  auto word = [&](int a, int n) { long v = 0; for (int i = n - 1; i >= 0; i--) v = (v << 8) | iram(a + i); return v; };
  check(word(0x10, 4) == 0xFFFFF448, "multiplicação -3 × 1000", word(0x10, 4), 0xFFFFF448);
  check(word(0x14, 2) == 0xFF71, "quociente -1000 / 7", word(0x14, 2), 0xFF71);
  check(word(0x16, 2) == 0x0001, "resto -1000 / 7", word(0x16, 2), 1);
  check(word(0x18, 3) == 18500, "soma de produtos", word(0x18, 3), 18500);
  check(iram(0x1B) == 0, "sem estouro na soma");
  check(s.cart.sram[0] == 0x5A, "BW-RAM pela janela do SA-1", s.cart.sram[0], 0x5A);
  check(s.cart.sram[1] == 0x3C, "BW-RAM linear e como bitmap de 4 bits", s.cart.sram[1], 0x3C);
  bool dmaOk = true;
  for (int i = 0; i < 16; i++) dmaOk &= iram(0x100 + i) == TABLE[i];
  check(dmaOk, "DMA da ROM para a I-RAM");
  check(word(0x20, 2) == 0x4321, "bits de tamanho variável (16)", word(0x20, 2), 0x4321);
  check(word(0x22, 2) == 0x5432, "bits de tamanho variável (andou 4)", word(0x22, 2), 0x5432);
  check(iram(0x200) == 0xAA && iram(0x201) == 0x99, "conversão tipo 2, linha 0", word(0x200, 2), 0x99AA);
  check(iram(0x202) == 0x01 && iram(0x203) == 0x01, "conversão tipo 2, linha 1", word(0x202, 2), 0x0101);
  check(iram(0x30) == 0x5B, "Super MMC: bloco D no 2º MB", iram(0x30), 0x5B);
  check(iram(0x31) == 0x5B, "Super MMC: $D0 no bloco do $2221", iram(0x31), 0x5B);

  // conversão tipo 1: o SNES lê a BW-RAM e recebe o bloco já no formato dos planos
  check(s.read(0x400100) == 0xAA, "conversão tipo 1, plano 0", s.read(0x400100), 0xAA);
  check(s.read(0x400101) == 0x99, "conversão tipo 1, plano 1", s.read(0x400101), 0x99);
  check(s.read(0x400102) == 0xFF, "conversão tipo 1, linha 1 (BW-RAM apagada = $FF)", s.read(0x400102), 0xFF);

  // mensagem e IRQ do SA-1 para o SNES, com o vetor trocado
  check(s.read(0x002300) == 0xE5, "SFR: IRQ, troca de vetor, conversão, mensagem 5", s.read(0x002300), 0xE5);
  check(s.cpu.irqLine, "IRQ do SA-1 chega no SNES");
  check(s.read(0x00FFEE) == 0x34 && s.read(0x00FFEF) == 0x12, "vetor de IRQ do SNES trocado");
  s.write(0x002202, 0xA0);
  check(!s.cpu.irqLine, "SNES limpa o IRQ do SA-1");
  s.write(0x002231, 0x80);  // fim da conversão tipo 1
  check(s.read(0x400100) == 0x93, "BW-RAM normal depois da conversão", s.read(0x400100), 0x93);
  check(s.read(0x400000) == 0x5A, "BW-RAM do SNES em $40:0000", s.read(0x400000), 0x5A);
  check(s.read(0x006000) == 0x5A, "janela da BW-RAM do SNES em $6000", s.read(0x006000), 0x5A);
  s.write(0x002224, 0x01);
  s.write(0x006000, 0x66);
  check(s.cart.sram[0x2000 % s.cart.sram.size()] == 0x66, "janela do SNES no bloco 1 (espelho em 8 KB)");
  s.write(0x002224, 0x00);

  // mensagem e IRQ do SNES para o SA-1
  s.write(0x002200, 0x83);
  check(runUntil(0x003081, 0x99), "IRQ do SNES chega no SA-1");
  check(iram(0x80) == 0xA3, "CFR: IRQ, fim do DMA e mensagem 3", iram(0x80), 0xA3);

  // estado salvo
  const std::vector<uint8_t> st = s.saveState();
  s.write(0x003040, 0x42);
  s.loadState(st);
  check(iram(0x40) == 0x00 && iram(0x81) == 0x99, "estado salvo guarda a I-RAM");

  // reset do SA-1 pelo SNES: para, apaga o sinal e roda de novo desde o vetor
  s.write(0x002200, 0x20);
  s.write(0x00307F, 0x00);
  check(!runUntil(0x00307F, 0x77), "SA-1 em reset fica parado");
  s.write(0x002200, 0x00);
  check(runUntil(0x00307F, 0x77), "SA-1 recomeça depois do reset");
  check(s.dirty, "BW-RAM mudada marca o save");

  std::printf("SA-1: %d conferências, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
