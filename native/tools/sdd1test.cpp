// Testes do S-DD1 do SNES (src/snes/sdd1.*). Não há ROMs de teste públicas para o chip. O teste confere:
//   1. o descompressor sozinho: 64 fluxos pseudoaleatórios (os 4 arranjos de planos de bits × os 4 modelos de
//      contexto, com dados de vários graus de compressão) dão a mesma saída que o descompressor do bsnes. Os
//      valores esperados (impressão digital FNV-1a de 1.024 bytes de cada fluxo) foram tirados uma vez do bsnes,
//      fora do repositório; em cada sessão a comparação completa roda contra o próprio bsnes (milhões de bytes);
//   2. o chip no barramento do SNES, num cartucho de mentira com o cabeçalho de S-DD1: detecção, LoROM em
//      $00-$3F, janelas de 1 MB em $C0-$FF ($4804-$4807) e o espelho do bit 7, DMA com descompressão (e sem ela),
//      fim da transferência desligando o canal em $4801, RAM do cartucho e estado salvo no meio de um fluxo.
#include <cstdio>
#include <cstring>
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

// ------------------------------------------------------------------ fluxos de teste
namespace gen {
static uint32_t x;
static uint32_t rnd() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
constexpr int STREAMS = 64, OUT = 1024;
constexpr uint32_t BASE = 0x123450;  // onde os fluxos ficam na ROM (janela 0 = 1º MB)

// monta o fluxo k na ROM a partir de BASE
static void fill(std::vector<uint8_t>& rom, int k) {
  x = 0x9E3779B9u ^ static_cast<uint32_t>(k * 7919);
  for (int i = 0; i < 0x1800; i++) {
    const int p = (i / 48 + k) % 6;  // 0: aleatório; 1-5: cada vez mais zeros (sequências longas)
    uint8_t v = static_cast<uint8_t>(rnd());
    for (int j = 0; j < p; j++) v &= static_cast<uint8_t>(rnd());
    if (k & 16) v = static_cast<uint8_t>(~v);
    rom[BASE + i] = v;
  }
  rom[BASE] = static_cast<uint8_t>(((k & 15) << 4) | (rom[BASE] & 0x0F));  // planos (bits 7-6) e contexto (5-4)
}
static uint32_t fnv(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}
}  // namespace gen

#ifndef SDD1_GOLDEN
// tirados do descompressor do bsnes (bsnes-emu/bsnes 78474e5, sfc/coprocessor/sdd1/decompressor.cpp)
static const uint32_t GOLDEN[gen::STREAMS] = {
#include "sdd1test-golden.inc"
};

static void decompressorTests() {
  std::vector<uint8_t> rom(0x200000);
  int same = 0;
  for (int k = 0; k < gen::STREAMS; k++) {
    gen::fill(rom, k);
    indigo::snes::SDD1 chip(rom);  // janelas no começo: $C0 = 1º MB
    chip.ioWrite(0x4805, 0x01);
    chip.startStream(0xC00000 | gen::BASE);
    uint8_t out[gen::OUT];
    for (auto& b : out) b = chip.streamByte();
    const uint32_t h = gen::fnv(out, sizeof out);
    char what[80];
    std::snprintf(what, sizeof what, "fluxo %d (planos %02X, contexto %02X) igual ao bsnes", k, rom[gen::BASE] & 0xC0, rom[gen::BASE] & 0x30);
    check(h == GOLDEN[k], what, h, GOLDEN[k]);
    if (h == GOLDEN[k]) same++;
  }
  std::printf("  descompressor: %d de %d fluxos iguais ao bsnes\n", same, gen::STREAMS);
}

// ------------------------------------------------------------------ chip no barramento
static std::vector<uint8_t> fakeRom(uint8_t chip, uint8_t ramShift) {
  std::vector<uint8_t> r(0x600000);  // 6 MB, como o Star Ocean
  for (size_t i = 0; i < r.size(); i++) r[i] = static_cast<uint8_t>((i >> 20) * 0x11 ^ (i * 13) ^ (i >> 9));
  const size_t h = 0x7FC0;
  const char* title = "INDIGO SDD1 TESTE    ";
  std::memcpy(&r[h], title, 21);
  r[h + 0x15] = 0x32;  // LoROM rápida com S-DD1
  r[h + 0x16] = chip;
  r[h + 0x17] = 0x0D;
  r[h + 0x18] = ramShift;
  r[h + 0x1C] = 0xFF; r[h + 0x1D] = 0xFF; r[h + 0x1E] = 0x00; r[h + 0x1F] = 0x00;
  r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
  r[0] = 0x78;  // SEI
  return r;
}

static void dmaToWram(indigo::snes::SNES& s, uint32_t src, uint16_t size) {
  s.write(0x002181, 0x00); s.write(0x002182, 0x00); s.write(0x002183, 0x00);
  s.write(0x004300, 0x08);  // A → B, endereço fixo
  s.write(0x004301, 0x80);  // $2180 (WRAM)
  s.write(0x004302, static_cast<uint8_t>(src)); s.write(0x004303, static_cast<uint8_t>(src >> 8));
  s.write(0x004304, static_cast<uint8_t>(src >> 16));
  s.write(0x004305, static_cast<uint8_t>(size)); s.write(0x004306, static_cast<uint8_t>(size >> 8));
  s.write(0x00420B, 0x01);
}

static void busTests() {
  using indigo::snes::Cart;
  Cart cart;
  std::string err;
  std::vector<uint8_t> rom = fakeRom(0x45, 0x03);
  // fluxo 5 do teste anterior em $4A:3450 (janela de $C0 apontando para o 5º MB → $C2:3450 no SNES)
  std::vector<uint8_t> scratch(0x200000);
  gen::fill(scratch, 5);
  std::memcpy(&rom[0x400000 + 0x23450], &scratch[gen::BASE], 0x1800);
  check(indigo::snes::parseRom(rom, cart, err), "cabeçalho de S-DD1 aceito");
  check(cart.sdd1 && cart.map == Cart::LOROM, "detectado como S-DD1 em LoROM");
  check(cart.sram.size() == 0x2000 && cart.battery, "8 KB de RAM com bateria (como o Star Ocean)", static_cast<long>(cart.sram.size()), 0x2000);
  indigo::snes::SNES s(cart);

  // LoROM em $00-$3F e $80-$BF
  check(s.read(0x018000) == rom[0x8000], "$01:8000 = ROM $8000");
  check(s.read(0x9F8123) == rom[0x1F * 0x8000 + 0x123], "$9F:8123 = ROM $F8123");
  check(s.read(0x208000) == rom[0x100000], "$20:8000 = ROM $100000 (2º MB)");
  s.write(0x004805, 0x81);
  check(s.read(0x208000) == rom[0x000000 + 0x8000 - 0x8000], "bit 7 de $4805: $20:8000 espelha $00:8000");
  s.write(0x004805, 0x01);
  check(s.read(0x004805) == 0x01 && s.read(0x004804) == 0x00 && s.read(0x004807) == 0x03, "registradores do MMC com os valores iniciais");
  // janelas de 1 MB
  check(s.read(0xC00000) == rom[0], "$C0:0000 = 1º MB");
  check(s.read(0xF12345) == rom[0x312345], "$F1:2345 = 4º MB");
  s.write(0x004806, 0x05);
  check(s.read(0xE01234) == rom[0x501234], "$4806 = 5: $E0:1234 = 6º MB", s.read(0xE01234), rom[0x501234]);
  s.write(0x004806, 0x0E);
  check(s.read(0xE01234) == rom[0x401234], "janela além da ROM espelha (6 MB: o 15º MB cai no 5º)", s.read(0xE01234), rom[0x401234]);
  s.write(0x004806, 0x02);

  // descompressão: o que o DMA entrega é o mesmo que o descompressor sozinho
  s.write(0x004804, 0x04);  // $C0-$CF = 5º MB
  indigo::snes::SDD1 ref(rom);
  ref.ioWrite(0x4804, 0x04);
  ref.startStream(0xC23450);
  std::vector<uint8_t> want(3000);
  for (auto& b : want) b = ref.streamByte();
  s.write(0x004800, 0x01); s.write(0x004801, 0x01);
  dmaToWram(s, 0xC23450, 3000);
  bool same = true;
  for (int i = 0; i < 3000; i++) same &= s.peek(0x7E0000 + i) == want[i];
  check(same, "DMA com descompressão: 3.000 bytes iguais ao descompressor");
  check(s.read(0x004801) == 0x00, "fim da transferência desliga o canal em $4801", s.read(0x004801), 0);
  check(s.read(0xC23450) == rom[0x423450], "depois do DMA, $C2:3450 volta a ser a ROM");
  // sem $4801 o DMA copia a ROM comprimida
  dmaToWram(s, 0xC23450, 16);
  same = true;
  for (int i = 0; i < 16; i++) same &= s.peek(0x7E0000 + i) == rom[0x423450];
  check(same, "sem $4801: DMA de endereço fixo copia o mesmo byte da ROM");
  // canal ligado só em $4801 (sem $4800) não descomprime
  s.write(0x004800, 0x00); s.write(0x004801, 0x01);
  dmaToWram(s, 0xC23450, 4);
  check(s.peek(0x7E0000) == rom[0x423450] && s.read(0x004801) == 0x01, "sem $4800 o canal não descomprime");
  // dois fluxos seguidos: o segundo recomeça do cabeçalho
  s.write(0x004800, 0x01); s.write(0x004801, 0x01);
  dmaToWram(s, 0xC23450, 100);
  s.write(0x004801, 0x01);
  dmaToWram(s, 0xC23450, 100);
  same = true;
  for (int i = 0; i < 100; i++) same &= s.peek(0x7E0000 + i) == want[i];
  check(same, "segundo DMA recomeça o fluxo do início");

  // estado salvo no meio de um fluxo (leituras da CPU no endereço do DMA também tiram bytes descomprimidos)
  s.write(0x004302, 0x50); s.write(0x004303, 0x34); s.write(0x004304, 0xC2);
  s.write(0x004305, 0xD0); s.write(0x004306, 0x07);  // 2.000 bytes
  s.write(0x004801, 0x01);
  same = true;
  for (int i = 0; i < 700; i++) same &= s.read(0xC23450) == want[i];
  check(same, "leitura da CPU no endereço do DMA tira os bytes do fluxo");
  const std::vector<uint8_t> st = s.saveState();
  std::vector<uint8_t> a(1300), b(1300);
  for (auto& v : a) v = s.read(0xC23450);
  s.loadState(st);
  for (auto& v : b) v = s.read(0xC23450);
  check(a == b, "estado salvo no meio do fluxo continua igual");
  check(std::equal(a.begin(), a.end(), want.begin() + 700), "resto do fluxo depois do estado igual ao descompressor");
  check(s.read(0x004801) == 0x00, "fim do fluxo pela CPU desliga o canal");

  // RAM do cartucho
  s.write(0x700010, 0x5A);
  check(s.read(0x006010) == 0x5A && s.read(0x806010) == 0x5A, "RAM em $70:0000 e na janela $6000");
  check(s.cart.sram[0x10] == 0x5A, "RAM do cartucho gravada (save de bateria)");
  s.write(0x007FFF, 0xA5);
  check(s.read(0x711FFF) == 0xA5 && s.cart.sram[0x1FFF] == 0xA5, "janela $6000-$7FFF e $71:xxxx na mesma RAM de 8 KB");

  // Street Fighter Alpha 2: S-DD1 sem RAM
  Cart c2;
  check(indigo::snes::parseRom(fakeRom(0x43, 0x00), c2, err) && c2.sdd1 && c2.sram.empty(), "S-DD1 sem RAM (como o Street Fighter Alpha 2)");
  indigo::snes::SNESCore core(fakeRom(0x43, 0x00), nullptr);
  check(!core.hasSave(), "sem RAM: sem save de bateria");
}

int main() {
  std::printf("== S-DD1: descompressor\n");
  decompressorTests();
  std::printf("== S-DD1: chip no barramento\n");
  busTests();
  std::printf("S-DD1: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
#endif
