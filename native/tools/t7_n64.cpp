// Testes do núcleo Nintendo 64.
//   indigo-t7_n64 pasta              roda as verificações (pasta = /root/indigo-env/roms/n64, preparada por
//                                    tools/t7_n64.sh: cartuchos de teste montados com o GNU as a partir de
//                                    tools/t7_n64_*.s, escritos para o Índigo)
//   indigo-t7_n64 --png rom quadros saida.png    grava a tela (para conferir a olho)
// Não há ROM, BIOS nem código de partida (IPL3) comercial aqui: a partida é HLE.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/n64/n64.h"

using namespace indigo;
using indigo::n64::N64;
using indigo::n64::N64Core;

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s %s\n", what.c_str(), detail.c_str()); }
}
static std::string hex64(uint64_t v) { char b[24]; std::snprintf(b, sizeof b, "%016llX", (unsigned long long)v); return b; }
static std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }

// ---------------------------------------------------------------- PNG sem compressão
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
static void writePng(const std::string& path, const uint32_t* px, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const uint32_t c = px[y * w + x];
      raw.push_back(c & 0xFF); raw.push_back((c >> 8) & 0xFF); raw.push_back((c >> 16) & 0xFF);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n == raw.size());
    z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + static_cast<long>(i), raw.begin() + static_cast<long>(i + n));
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  const uint32_t ad = (b << 16) | a;
  for (int k = 3; k >= 0; k--) z.push_back((ad >> (8 * k)) & 0xFF);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
    const uint32_t n = static_cast<uint32_t>(d.size());
    for (int k = 3; k >= 0; k--) out.push_back((n >> (8 * k)) & 0xFF);
    std::vector<uint8_t> td(t, t + 4);
    td.insert(td.end(), d.begin(), d.end());
    out.insert(out.end(), td.begin(), td.end());
    const uint32_t c = crc32(td.data(), td.size());
    for (int k = 3; k >= 0; k--) out.push_back((c >> (8 * k)) & 0xFF);
  };
  std::vector<uint8_t> ih;
  for (uint32_t v : {static_cast<uint32_t>(w), static_cast<uint32_t>(h)})
    for (int k = 3; k >= 0; k--) ih.push_back((v >> (8 * k)) & 0xFF);
  ih.insert(ih.end(), {8, 2, 0, 0, 0});
  chunk("IHDR", ih);
  chunk("IDAT", z);
  chunk("IEND", {});
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<long>(out.size()));
}

// ---------------------------------------------------------------- ajudantes
static uint32_t ram32(N64& s, uint32_t a) { uint32_t v; std::memcpy(&v, &s.rdram[a & 0x7FFFFC], 4); return v; }
static uint64_t ram64(N64& s, uint32_t a) { return (uint64_t(ram32(s, a)) << 32) | ram32(s, a + 4); }
static bool roda(N64Core& c, int quadrosMax) {
  for (int i = 0; i < quadrosMax; i++) {
    c.runFrame();
    if (ram32(c.sys(), 0x2FFFF0) == 0x600DF00Du) return true;
  }
  return false;
}
static std::unique_ptr<N64Core> abre(const std::string& p, const std::vector<uint8_t>* save = nullptr) {
  const auto rom = readAll(p);
  if (rom.empty()) { check(false, "abrir " + p); return nullptr; }
  auto core = createCore(rom, p, save);
  auto* n = dynamic_cast<N64Core*>(core.get());
  if (!n) { check(false, "detectar o N64 em " + p); return nullptr; }
  core.release();
  return std::unique_ptr<N64Core>(n);
}

// ---------------------------------------------------------------- CPU
static void testeCpu(const std::string& dir) {
  auto c = abre(dir + "/cpu.z64");
  if (!c) return;
  const bool fim = roda(*c, 30);
  check(fim, "cpu.z64 chega ao fim");
  N64& s = c->sys();
  const uint64_t M1 = ~0ull;
  struct { int n; uint64_t v; const char* o; } esperado[] = {
    {0, 0xFFFFFFFF80000000ull, "addiu estoura sem exceção e estende o sinal"},
    {1, 8, "addu usa só 32 bits"},
    {2, 0x2222222222222211ull, "daddu"},
    {3, 0xFFFFFFFFFFFFFFFEull, "dsubu"},
    {4, 2, "slt/sltu"},
    {5, 2, "sll"},
    {6, 0x08000000, "srl"},
    {7, 0xFFFFFFFFFFFFFFFCull, "sra"},
    {8, 0x0000001000000000ull, "dsll32"},
    {9, 0xFFFFFFFFF0000000ull, "dsra32"},
    {10, 0x0800000000000000ull, "dsrl"},
    {11, 0xFFFFFFFFD0000000ull, "mult lo"}, {12, M1, "mult hi"},
    {13, 1, "multu lo"}, {14, 0xFFFFFFFFFFFFFFFEull, "multu hi"},
    {15, uint64_t(-3), "div lo"}, {16, M1, "div hi"},
    {17, M1, "divu por zero lo"}, {18, 5, "divu por zero hi"},
    {19, 0x0000000300000000ull, "dmult lo"}, {20, 1, "dmult hi"},
    {21, 0xFFFFFFFFFFFFFFFEull, "dmultu lo"}, {22, 1, "dmultu hi"},
    {23, uint64_t(-14), "ddiv lo"}, {24, uint64_t(-2), "ddiv hi"},
    {25, 0x0FFFFFFFFFFFFFFFull, "ddivu lo"}, {26, 0xF, "ddivu hi"},
    {27, 0xFFFFFFFFFFFFFF89ull, "lb"}, {28, 0xAB, "lbu"}, {29, 0xFFFFFFFFFFFFCDEFull, "lh"}, {30, 0x89AB, "lhu"},
    {31, 0xFFFFFFFF89ABCDEFull, "lw"}, {32, 0x89ABCDEF, "lwu"},
    {33, 0x11223344, "lwl/lwr desalinhado"},
    {34, 0x33445566778899AAull, "ldl/ldr desalinhado"},
    {35, 0x0000AABBCCDD0000ull, "swl/swr desalinhado"},
    {36, 0x0001020304050607ull, "sdl/sdr (1ª palavra)"}, {37, 0x0800000000000000ull, "sdl/sdr (2ª palavra)"},
    {38, 2, "beql: atraso pulado quando não desvia"},
    {39, 1, "bltzal: endereço de retorno"}, {40, 1, "jalr: endereço de retorno"},
    {41, 8, "syscall"}, {42, 9, "break"},
    {43, 0x55, "add com estouro não grava"}, {44, 12, "add com estouro: exceção 12"},
    {45, 4, "lw desalinhado: AdEL"}, {46, 0xFFFFFFFF80100001ull, "BadVAddr"},
    {47, 13, "teq: trap"},
    {48, 0x5A5A1234, "TLB: leitura pela página mapeada no refill"}, {49, 1, "TLB: um refill"},
    {50, 0x77, "TLB: página ímpar do par"}, {51, 1, "TLB: sem refill novo"},
    {52, 2, "TLBP acha e não acha"},
    {53, 1, "interrupção do timer (Count = Compare)"},
    {54, 2, "interrupção do RCP (SP pelo MI)"}, {55, 0, "MI_INTR do SP limpo pelo tratador"},
    {56, 0x40700000, "add.s"}, {57, 0x3FD5555555555555ull, "div.d 1/3"},
    {58, 0x3FB504F3, "sqrt.s 2"}, {59, 0x3DCCCCCD, "cvt.s.d 0.1"},
    {60, 2, "cvt.w.s 2.5 (mais próximo)"}, {61, 3, "cvt.w.s 2.5 (para cima)"}, {62, 2, "cvt.w.s 2.5 (para baixo)"},
    {63, 2, "cvt.w.s 2.5 (para zero)"}, {64, uint64_t(-2), "cvt.w.s -2.5 (mais próximo)"},
    {65, uint64_t(-2), "cvt.w.s -2.5 (para cima)"}, {66, uint64_t(-3), "cvt.w.s -2.5 (para baixo)"},
    {67, uint64_t(-2), "cvt.w.s -2.5 (para zero)"},
    {68, 4, "round.w.d 3.5"}, {69, uint64_t(-1), "trunc.w.d -1.7"}, {70, uint64_t(-2), "floor.l.d -1.5"},
    {71, 2, "ceil.w.s 1.1"},
    {72, 0x7F800000, "1/0 = infinito"}, {73, 0x00008020, "FCR31: causa e flag de divisão por zero"},
    {74, 0x00001004, "FCR31: inexato"},
    {75, 1, "c.lt.d + bc1t"}, {76, 0, "c.eq.s falso"},
    {77, 15, "exceção do FPU (divisão por zero ligada)"}, {78, 0x12345678, "destino intacto na exceção"},
    {79, 0x2222222211111111ull, "FR=0: registro ímpar = metade alta"},
    {80, 0xC008000000000000ull, "cvt.d.w -3"}, {81, 0xC008000000000000ull, "mul.d"},
    {82, 0xFFFFFFFFBF800000ull, "neg.s (mfc1 estende o sinal)"}, {83, 0x0000010000000000ull, "cvt.l.d (arredonda para o par)"},
    {84, 1, "ll/sc"}, {85, 0x123456789ABCDEF0ull, "EPC de 64 bits (dmtc0/dmfc0)"},
    {86, 0xFFFFFFFFFFFFF000ull, "nor"}, {87, 1, "sltiu com imediato negativo"},
    {88, M1, "dsllv/dsrav 63"}, {89, 0x18000000, "sra usa os 64 bits (VR4300)"}, {90, 0xABC, "mthi/mfhi"},
  };
  int ok = 0;
  for (const auto& e : esperado) {
    const uint64_t v = ram64(s, 0x200000 + 8 * e.n);
    check(v == e.v, std::string("CPU ") + std::to_string(e.n) + " " + e.o, hex64(v) + " (esperado " + hex64(e.v) + ")");
    if (v == e.v) ok++;
  }
  std::printf("  CPU: %d de %zu resultados certos (%llu instruções)\n", ok, std::size(esperado),
              (unsigned long long)s.cpu.instrucoes);
}

// ---------------------------------------------------------------- RSP
static void testeRsp(const std::string& dir) {
  auto c = abre(dir + "/rsp.z64");
  if (!c) return;
  check(roda(*c, 30), "rsp.z64 chega ao fim");
  N64& s = c->sys();
  const uint64_t r[] = {ram64(s, 0x200000), ram64(s, 0x200008), ram64(s, 0x200010), ram64(s, 0x200018),
                        ram64(s, 0x200020), ram64(s, 0x200028), ram64(s, 0x200030)};
  check(r[0] == 0 && r[1] == 1 && r[2] == 0, "semáforo do SP: 0, 1 e solto", hex64(r[0]) + " " + hex64(r[1]) + " " + hex64(r[2]));
  check(r[3] == 3, "RSP parou no BREAK (HALT + BROKE)", hex64(r[3]));
  check(r[4] == 1, "interrupção do SP no BREAK", hex64(r[4]));
  check(r[5] == 1, "SIG0 ligado pelo COP0 do RSP", hex64(r[5]));
  // PC parado logo depois do BREAK
  uint32_t brk = 0;
  for (uint32_t a = 0; a < 0x1000; a += 4) { uint32_t w; std::memcpy(&w, &s.spmem[0x1000 + a], 4); if (w == 0x0000000D) { brk = a; break; } }
  check(brk && r[6] == brk + 4, "SP_PC depois do BREAK", hex64(r[6]) + " break em " + hex(brk));
  auto vet = [&](int k) {
    std::vector<uint16_t> v(8);
    for (int i = 0; i < 8; i++) {
      const uint32_t a = 0x210000 + uint32_t(k) * 16 + uint32_t(i) * 2;
      v[i] = uint16_t(ram32(s, a & ~3u) >> ((a & 2) ? 0 : 16));
    }
    return v;
  };
  struct { int k; uint16_t e[8]; const char* o; } esperado[] = {
    {0, {0x2000, 0x7FFF, 0x7FFE, 0xE000, 0x0001, 0x0000, 0x0024, 0x0000}, "VMULF (arredonda e satura)"},
    {1, {0x7FFF, 0x8000, 0x7FFF, 0x0000, 0x7FFF, 0x0001, 0x1334, 0x5555}, "VADD (satura)"},
    {2, {0x0000, 0x0000, 0x0000, 0x8000, 0x8002, 0xFFFD, 0x1134, 0xAAAB}, "VSUB"},
    {3, {0x8000, 0x0000, 0xFFFE, 0x0000, 0x8000, 0x0001, 0x1334, 0x5555}, "VADDC"},
    {4, {0x0000, 0x0000, 0x0000, 0x8000, 0x8002, 0xFFFD, 0x1134, 0xAAAB}, "VSUBC"},
    {5, {0x4000, 0x8000, 0x7FFF, 0xC000, 0x0001, 0xFFFF, 0x0100, 0x0000}, "VLT"},
    {6, {0x7FFF, 0x8000, 0x7FFF, 0x8000, 0x4000, 0xC000, 0x7FFF, 0x0000}, "VMUDH com elemento escolhido"},
    {7, {0x0000, 0x0000, 0xC000, 0x0000, 0x4000, 0xC000, 0x0000, 0x0000}, "VSAR (meio do acumulador)"},
    {8, {0x1000, 0xE000, 0x1FFF, 0xF000, 0x0000, 0xFFFF, 0x048D, 0x0000}, "VSAR (alto do acumulador)"},
    {9, {0xFFFF, 0x0001, 0, 0, 0, 0, 0, 0}, "VRCP/VRCPH de 0x4000"},
    {10, {0x0000, 0xABCD, 0, 0, 0, 0, 0, 0}, "MTC2 no elemento 1"},
    {11, {0x4000, 0x8000, 0x7FFF, 0xC000, 0x0001, 0xFFFF, 0x0100, 0x0000}, "VCH"},
  };
  int ok = 0;
  for (const auto& e : esperado) {
    const auto v = vet(e.k);
    std::string d;
    bool igual = true;
    for (int i = 0; i < 8; i++) { if (v[i] != e.e[i]) igual = false; char b[8]; std::snprintf(b, sizeof b, "%04X ", v[i]); d += b; }
    check(igual, std::string("RSP ") + e.o, d);
    ok += igual;
  }
  const uint32_t f[] = {ram32(s, 0x2100E0), ram32(s, 0x2100E4), ram32(s, 0x2100E8), ram32(s, 0x2100F0),
                        ram32(s, 0x2100F4), ram32(s, 0x2100F8), ram32(s, 0x2100FC)};
  check(f[0] == 0xFFFFF028 && f[1] == 0x470A, "VCO/VCC depois do VCH", hex(f[0]) + " " + hex(f[1]));
  check(f[2] == 55, "laço escalar com desvio (soma 1..10)", hex(f[2]));
  check(f[3] == 0x2A, "VCO depois do VADDC", hex(f[3]));
  check(f[4] == 0xFFFFF890, "VCO depois do VSUBC", hex(f[4]));
  check(f[5] == 0xB8, "VCC depois do VLT", hex(f[5]));
  check(f[6] == 0xFFFFABCD, "MFC2 estende o sinal", hex(f[6]));
  std::printf("  RSP: %d de %zu vetores certos (%llu instruções do RSP)\n", ok, std::size(esperado),
              (unsigned long long)s.rsp.instrucoes);
}

// ---------------------------------------------------------------- RDP e VI
static void testeGfx(const std::string& dir) {
  auto c = abre(dir + "/gfx.z64");
  if (!c) return;
  check(roda(*c, 30), "gfx.z64 chega ao fim");
  c->runFrame();
  N64& s = c->sys();
  check(ram64(s, 0x200000) == 0x20, "interrupção do DP no full sync (lista da RDRAM)", hex64(ram64(s, 0x200000)));
  check(ram64(s, 0x200008) == 0x20, "interrupção do DP na lista mandada pelo RSP (XBUS)", hex64(ram64(s, 0x200008)));
  check(ram64(s, 0x200010) == 3, "RSP terminou a lista do XBUS", hex64(ram64(s, 0x200010)));
  auto px = [&](int x, int y) {
    const uint32_t a = 0x100000 + uint32_t(y * 320 + x) * 2;
    return uint16_t(ram32(s, a & ~3u) >> ((a & 2) ? 0 : 16));
  };
  struct { int x, y; uint16_t v; const char* o; } esperado[] = {
    {5, 5, 0x0021, "fundo (preenchimento)"},
    {80, 100, 0xF801, "triângulo com shade"}, {119, 100, 0xF801, "borda direita do triângulo"},
    {120, 100, 0x0021, "fora da borda"}, {60, 41, 0xF801, "vértice de cima"}, {59, 100, 0x0021, "à esquerda do lado maior"},
    {240, 120, 0x07C1, "z: o triângulo perto fica na frente"}, {290, 120, 0x003F, "z: o de longe onde não há o perto"},
    {215, 100, 0x07C1, "z: só o perto"},
    {21, 161, 0xFFC1, "textura 4x: texel (0,0)"}, {25, 161, 0xF83F, "textura 4x: texel (1,0)"},
    {25, 165, 0xFFC1, "textura 4x: texel (1,1)"}, {51, 191, 0xFFC1, "textura 4x: texel (7,7)"},
    {52, 161, 0x0021, "textura 4x: fim do retângulo"},
    {100, 160, 0xFFC1, "cópia: texel (0,0)"}, {101, 160, 0xF83F, "cópia: texel (1,0)"},
    {101, 161, 0xFFC1, "cópia: texel (1,1)"}, {107, 167, 0xFFC1, "cópia: canto (7,7) incluído"},
    {108, 160, 0x0021, "cópia: fim"},
    {280, 200, 0xFFC1, "lista do XBUS: preenchimento"}, {299, 219, 0xFFC1, "lista do XBUS: canto incluído"},
    {300, 219, 0x0021, "lista do XBUS: fim"},
  };
  int ok = 0;
  for (const auto& e : esperado) {
    const uint16_t v = px(e.x, e.y);
    char b[64];
    std::snprintf(b, sizeof b, "(%d,%d) = %04X, esperado %04X", e.x, e.y, v, e.v);
    check(v == e.v, std::string("RDP ") + e.o, b);
    ok += v == e.v;
  }
  std::printf("  RDP: %d de %zu pixels certos (%llu pixels desenhados)\n", ok, std::size(esperado), (unsigned long long)s.rdp.pixels);
  // saída do VI: 320x240, mesmas cores em RGBA
  check(c->width() == 320 && c->height() == 240, "VI: 320x240", std::to_string(c->width()) + "x" + std::to_string(c->height()));
  const uint32_t* f = c->framebuffer();
  check(f[100 * 320 + 80] == 0xFF0000FFu && f[120 * 320 + 240] == 0xFF00FF00u && f[5 * 320 + 5] == 0xFF840000u,
        "VI: pixels convertidos de RGBA5551", hex(f[100 * 320 + 80]) + " " + hex(f[120 * 320 + 240]) + " " + hex(f[5 * 320 + 5]));
  // velocidade: a lista é desenhada de novo a cada quadro
  const auto t0 = std::chrono::steady_clock::now();
  const int Q = 300;
  for (int i = 0; i < Q; i++) c->runFrame();
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const uint64_t n = ram64(s, 0x200018);
  check(n >= 250, "a lista foi redesenhada a cada quadro", std::to_string(n));
  std::printf("  velocidade (lista do RDP a cada quadro, CPU esperando o VI): %.0f quadros/s (%.1fx)\n", Q / sec, Q / sec / 60.0);
}

// ---------------------------------------------------------------- controle, EEPROM, Controller Pak, PI, AI
static uint8_t ram8(N64& s, uint32_t a) { return s.rdram[(a & 0x7FFFFF) ^ 3]; }
static void testeIo(const std::string& dir) {
  auto c = abre(dir + "/io.z64");
  if (!c) return;
  c->setSampleRate(44100);
  c->setKeys(KEY_A | KEY_START | KEY_RIGHT);
  check(roda(*c, 30), "io.z64 chega ao fim");
  N64& s = c->sys();
  check(ram64(s, 0x200000) == 2, "interrupção do SI", hex64(ram64(s, 0x200000)));
  const uint8_t b0[] = {ram8(s, 0x220003), ram8(s, 0x220004), ram8(s, 0x220005), ram8(s, 0x220006)};
  char d[64];
  std::snprintf(d, sizeof d, "%02X %02X %02X %02X", b0[0], b0[1], b0[2], b0[3]);
  check(b0[0] == 0x90 && b0[1] == 0x00 && b0[2] == 0x50 && b0[3] == 0x00, "controle: A + Start + direita (analógico 80)", d);
  check(ram8(s, 0x220008) == 0x83, "canal 1 sem controle: bit de \"sem resposta\"", hex(ram8(s, 0x220008)));
  std::snprintf(d, sizeof d, "%02X %02X %02X", ram8(s, 0x220047), ram8(s, 0x220048), ram8(s, 0x220049));
  check(ram8(s, 0x220047) == 0x00 && ram8(s, 0x220048) == 0x80 && ram8(s, 0x220049) == 0x00, "EEPROM 4K responde no canal 4", d);
  std::string ee;
  for (int i = 0; i < 8; i++) ee += char(ram8(s, 0x2200C8 + uint32_t(i)));
  check(ee == "INDIGO64", "EEPROM: grava e lê o bloco 2", ee);
  bool pak = true;
  for (int i = 0; i < 32; i++) pak &= ram8(s, 0x220145 + uint32_t(i)) == i;
  check(pak, "Controller Pak: grava e lê 32 bytes");
  std::string pi;
  for (int i = 0; i < 16; i++) pi += char(ram8(s, 0x221000 + uint32_t(i)));
  check(pi == "CARTUCHO PI DMA!", "DMA do PI do cartucho", pi);
  check(ram64(s, 0x200008) == 8 && ram64(s, 0x200010) == 0x10 && ram64(s, 0x200018) == 0,
        "PI: interrupção no fim do DMA e limpa pelo PI_STATUS",
        hex64(ram64(s, 0x200008)) + " " + hex64(ram64(s, 0x200010)) + " " + hex64(ram64(s, 0x200018)));
  check(ram64(s, 0x200020) == 0x41100000, "AI_STATUS ocupado com um buffer", hex64(ram64(s, 0x200020)));
  const uint64_t len = ram64(s, 0x200028);
  check(len > 3800 && len <= 4000, "AI_LEN conta o que falta", std::to_string(len));
  check(ram64(s, 0x200030) == 4, "interrupção do AI quando o buffer começa", hex64(ram64(s, 0x200030)));
  // controle mudando quadro a quadro
  c->setKeys(KEY_B | KEY_LEFT | KEY_L);
  c->runFrame(); c->runFrame();
  std::snprintf(d, sizeof d, "%02X %02X %02X %02X", ram8(s, 0x228003), ram8(s, 0x228004), ram8(s, 0x228005), ram8(s, 0x228006));
  check(ram8(s, 0x228003) == 0x60 && ram8(s, 0x228004) == 0x00 && ram8(s, 0x228005) == 0xB0, "controle: B + Z + esquerda", d);
  // dois jogadores: com setKeys2 o canal 1 responde (controle sem Controller Pak) e o canal 0 não muda
  {
    auto c2 = abre(dir + "/io.z64");
    check(c2->players() == 2, "2 jogadores: players() == 2");
    c2->setKeys(KEY_A | KEY_START | KEY_RIGHT);
    c2->setKeys2(KEY_B);
    check(roda(*c2, 30), "2 jogadores: io.z64 chega ao fim");
    N64& s2 = c2->sys();
    std::snprintf(d, sizeof d, "%02X %02X %02X %02X", ram8(s2, 0x220008), ram8(s2, 0x22000A), ram8(s2, 0x22000B), ram8(s2, 0x22000C));
    check(ram8(s2, 0x220008) == 0x03 && ram8(s2, 0x22000A) == 0x05 && ram8(s2, 0x22000B) == 0x00 && ram8(s2, 0x22000C) == 0x02,
          "2 jogadores: canal 1 com controle, sem Controller Pak", d);
    check(ram8(s2, 0x220003) == 0x90 && ram8(s2, 0x220005) == 0x50, "2 jogadores: canal 0 continua com o controle 1");
  }
  // som: esquerda +-8000 (período de 100 amostras a 44100 Hz), direita 0x1000
  std::vector<float> l, r;
  for (int i = 0; i < 20; i++) {
    c->runFrame();
    std::vector<float> a(c->audioAvailable()), b(a.size());
    c->pullAudio(a.data(), b.data(), a.size());
    l.insert(l.end(), a.begin(), a.end());
    r.insert(r.end(), b.begin(), b.end());
  }
  int trocas = 0, direitaOk = 0;
  for (size_t i = 1; i < l.size(); i++) if ((l[i] > 0) != (l[i - 1] > 0)) trocas++;
  for (float v : r) if (std::fabs(v - 0x1000 / 32768.0f) < 1e-4f) direitaOk++;
  const double seg = l.size() / 44100.0;
  const double hz = trocas / 2.0 / (seg > 0 ? seg : 1);
  std::snprintf(d, sizeof d, "%zu amostras, %.0f Hz, direita certa em %d", l.size(), hz, direitaOk);
  check(l.size() > 13000 && hz > 400 && hz < 480, "som do AI: onda quadrada de ~441 Hz", d);
  check(direitaOk > int(r.size() * 9 / 10), "som do AI: canal direito", d);
  // save: EEPROM (512 bytes) + Controller Pak (32 KB)
  check(c->saveDirty(), "save sujo depois de gravar");
  const auto sv = c->exportSave();
  check(sv.size() == 512 + 32768, "save exportado = EEPROM 4K + Controller Pak", std::to_string(sv.size()));
  if (sv.size() == 512 + 32768) {
    check(std::string(sv.begin() + 16, sv.begin() + 24) == "INDIGO64", "EEPROM no save");
    bool p = true;
    for (int i = 0; i < 32; i++) p &= sv[512 + 0x60 + size_t(i)] == i;
    check(p, "Controller Pak no save");
    auto c2 = abre(dir + "/io.z64", &sv);
    if (c2) {
      check(std::string(c2->sys().eeprom.begin() + 16, c2->sys().eeprom.begin() + 24) == "INDIGO64" && c2->sys().mempak[0x61] == 1,
            "save importado de volta");
    }
  }
}

// ---------------------------------------------------------------- SRAM e FlashRAM
static void testeSaves(const std::string& dir) {
  {
    auto c = abre(dir + "/sram.z64");
    if (c) {
      check(roda(*c, 30), "sram.z64 chega ao fim");
      N64& s = c->sys();
      std::string t;
      for (int i = 0; i < 16; i++) t += char(ram8(s, 0x221000 + uint32_t(i)));
      check(ram64(s, 0x200000) == 3 && t == "SRAM DO INDIGO64", "SRAM: grava e lê por DMA do PI", t);
      check(ram64(s, 0x200008) == 0x20444F20, "SRAM: leitura direta da CPU", hex64(ram64(s, 0x200008)));
      const auto sv = c->exportSave();
      check(sv.size() == 32768 && std::string(sv.begin() + 0x100, sv.begin() + 0x110) == "SRAM DO INDIGO64",
            "save da SRAM (32 KB)", std::to_string(sv.size()));
    }
  }
  {
    auto c = abre(dir + "/flash.z64");
    if (c) {
      check(roda(*c, 30), "flash.z64 chega ao fim");
      N64& s = c->sys();
      std::string t;
      for (int i = 0; i < 32; i++) t += char(ram8(s, 0x221000 + uint32_t(i)));
      check(ram64(s, 0x200000) == 5 && t == "FLASHRAM DO INDIGO 64, PAGINA 2.", "FlashRAM: grava a página 2 e lê", t);
      check(ram32(s, 0x221100) == 0x11118001 && ram32(s, 0x221104) == 0x00C2001E, "FlashRAM: estado por DMA",
            hex(ram32(s, 0x221100)) + hex(ram32(s, 0x221104)));
      const auto sv = c->exportSave();
      check(sv.size() == 131072 && std::string(sv.begin() + 256, sv.begin() + 288) == t && sv[0] == 0xFF && sv[0x200] == 0xFF && sv[300] == 0x5A,
            "save da FlashRAM (128 KB, setor apagado)", std::to_string(sv.size()));
    }
  }
}

// ---------------------------------------------------------------- save state, ordem dos bytes, robustez
static uint32_t crcRam(N64& s) { return crc32(s.rdram.data(), s.rdram.size()); }
static void testeEstado(const std::string& dir) {
  auto c = abre(dir + "/gfx.z64");
  if (!c) return;
  for (int i = 0; i < 10; i++) c->runFrame();
  const auto st = c->saveState();
  for (int i = 0; i < 20; i++) c->runFrame();
  const uint32_t a = crcRam(c->sys()), fa = crc32(reinterpret_cast<const uint8_t*>(c->framebuffer()), 320 * 240 * 4);
  bool ok = true;
  try { c->loadState(st); } catch (...) { ok = false; }
  check(ok, "carrega o estado");
  for (int i = 0; i < 20; i++) c->runFrame();
  const uint32_t b = crcRam(c->sys()), fb = crc32(reinterpret_cast<const uint8_t*>(c->framebuffer()), 320 * 240 * 4);
  check(a == b && fa == fb, "save state determinístico (RDRAM e imagem iguais depois de 20 quadros)", hex(a) + " " + hex(b));
  // estado adulterado: recusado sem derrubar
  auto ruim = st;
  ruim[0] ^= 0xFF;
  bool lancou = false;
  try { c->loadState(ruim); } catch (const std::exception&) { lancou = true; }
  check(lancou, "estado de outro console é recusado");
  ruim = st;
  ruim.resize(ruim.size() / 2);
  lancou = false;
  try { c->loadState(ruim); } catch (const std::exception&) { lancou = true; }
  check(lancou, "estado truncado é recusado");
  c->runFrame();
  check(crcRam(c->sys()) != 0, "continua rodando depois do estado recusado");
  ruim = st;
  ruim.push_back(0);
  lancou = false;
  try { c->loadState(ruim); } catch (const std::exception&) { lancou = true; }
  check(lancou, "estado com sobra é recusado");
  // bytes trocados ao acaso (fora da RDRAM e dentro): carrega com limites ou é recusado, sem travar
  uint32_t semente = 12345;
  int sobreviveu = 0;
  for (int k = 0; k < 60; k++) {
    ruim = st;
    for (int j = 0; j < 8; j++) {
      semente = semente * 1103515245u + 12345u;
      const size_t pos = k < 30 ? 4 + (semente >> 8) % 4000 : (semente >> 4) % ruim.size();
      ruim[pos] ^= uint8_t(semente >> 24) | 1;
    }
    try { c->loadState(ruim); } catch (const std::exception&) {}
    c->runFrame();
    sobreviveu++;
  }
  check(sobreviveu == 60, "60 estados adulterados sem travar");
  // ordem dos bytes: .v64 (trocados de 2 em 2) e .n64 (palavras little-endian) rodam igual ao .z64
  const auto z = readAll(dir + "/cpu.z64");
  if (z.size() >= 0x1000) {
    std::vector<uint8_t> v(z.size()), n(z.size());
    for (size_t i = 0; i + 1 < z.size(); i += 2) { v[i] = z[i + 1]; v[i + 1] = z[i]; }
    for (size_t i = 0; i + 3 < z.size(); i += 4) { n[i] = z[i + 3]; n[i + 1] = z[i + 2]; n[i + 2] = z[i + 1]; n[i + 3] = z[i]; }
    for (auto* r : {&v, &n}) {
      auto core = createCore(*r, r == &v ? "x.v64" : "x.n64", nullptr);
      auto* nc = dynamic_cast<N64Core*>(core.get());
      check(nc != nullptr, r == &v ? "detecta .v64" : "detecta .n64");
      if (nc) {
        roda(*nc, 30);
        check(ram64(nc->sys(), 0x200010) == 0x2222222222222211ull && nc->title() == "INDIGO TESTE CPU",
              r == &v ? "cartucho .v64 roda igual" : "cartucho .n64 roda igual", nc->title());
      }
    }
  }
  // não rouba arquivos de outros consoles: lixo, cabeçalho curto
  std::vector<uint8_t> lixo(0x2000, 0x80);
  check(!indigo::n64::isN64Rom(lixo, "x.z64"), "lixo com extensão .z64 não é cartucho");
  std::vector<uint8_t> curto = {0x80, 0x37, 0x12, 0x40};
  check(!indigo::n64::isN64Rom(curto, "x.z64"), "cabeçalho sem cartucho não é aceito");
  // cartucho mínimo (cabeçalho e um laço) e cartucho com lixo: rodam sem travar
  std::vector<uint8_t> min(0x101000, 0);
  const uint8_t cab[] = {0x80, 0x37, 0x12, 0x40, 0, 0, 0, 0x0F, 0x80, 0, 0x04, 0};
  std::memcpy(min.data(), cab, sizeof cab);
  const uint8_t laco[] = {0x10, 0x00, 0xFF, 0xFF, 0, 0, 0, 0};  // b . ; nop
  std::memcpy(&min[0x1000], laco, sizeof laco);
  {
    auto core = createCore(min, "min.z64", nullptr);
    check(core != nullptr, "cartucho mínimo abre");
    if (core) { for (int i = 0; i < 5; i++) core->runFrame(); }
    for (size_t i = 0x1000; i < min.size(); i++) { semente = semente * 1103515245u + 12345u; min[i] = uint8_t(semente >> 24); }
    auto core2 = createCore(min, "lixo.z64", nullptr);
    if (core2) {
      for (int i = 0; i < 30; i++) core2->runFrame();
      check(true, "cartucho com código aleatório roda 30 quadros sem travar");
    }
  }
}

// ---------------------------------------------------------------- RDP direto (lista montada aqui)
struct Lista {
  N64& s;
  uint32_t base, pos;
  Lista(N64& sys, uint32_t b) : s(sys), base(b), pos(b) {}
  void w(uint64_t v) {
    const uint32_t hi = uint32_t(v >> 32), lo = uint32_t(v);
    std::memcpy(&s.rdram[pos], &hi, 4); std::memcpy(&s.rdram[pos + 4], &lo, 4);
    pos += 8;
  }
  void roda() {
    s.write32(0x0410000C, 1);  // sem XBUS
    s.write32(0x04100000, base);
    s.write32(0x04100004, pos);
  }
};
// triângulo de lado maior vertical em x0, de y0 a y1, até x1 (embaixo), com shade/textura/z constantes
static void triangulo(Lista& L, int tipo, int x0, int y0, int x1, int y1, uint32_t rgba, int32_t z, int32_t s0 = 0, int32_t dsdx = 0) {
  const uint64_t yh = uint64_t(y0 * 4), ym = uint64_t(y1 * 4), yl = uint64_t(y1 * 4);
  L.w((uint64_t(0x08 | tipo) << 56) | (1ull << 55) | (yl << 32) | (ym << 16) | yh);
  L.w(uint64_t(uint32_t(x1 << 16)) << 32);
  L.w(uint64_t(uint32_t(x0 << 16)) << 32);
  const int32_t slope = ((x1 - x0) << 16) / std::max(1, y1 - y0);
  L.w((uint64_t(uint32_t(x0 << 16)) << 32) | uint32_t(slope));
  if (tipo & 4) {
    const uint64_t r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    L.w((r << 48) | (g << 32) | (b << 16) | a);
    for (int i = 0; i < 7; i++) L.w(0);
  }
  if (tipo & 2) {
    L.w(uint64_t(uint16_t(s0)) << 48 | (uint64_t(0x7FFF) << 16));  // s, t = 0, w = 1.0
    L.w(uint64_t(uint16_t(dsdx)) << 48);
    for (int i = 0; i < 6; i++) L.w(0);
  }
  if (tipo & 1) { L.w(uint64_t(uint32_t(z)) << 32); L.w(0); }
}
[[maybe_unused]] static uint16_t px16(N64& s, uint32_t img, int x, int y) {
  const uint32_t a = img + uint32_t(y * 320 + x) * 2;
  return uint16_t(ram32(s, a & ~3u) >> ((a & 2) ? 0 : 16));
}
static void testeRdpDireto(const std::string& dir) {
  auto c = abre(dir + "/gfx.z64");
  if (!c) return;
  roda(*c, 30);
  N64& s = c->sys();
  const uint32_t IMG = 0x300000, ZB = 0x340000, TEX = 0x380000;
  // textura 32x32 RGBA16: gradiente
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      const uint16_t v = uint16_t(((x & 31) << 11) | ((y & 31) << 6) | (16 << 1) | 1);
      const uint32_t a = TEX + uint32_t(y * 32 + x) * 2;
      std::memcpy(&s.rdram[a ^ 2], &v, 2);
    }
  Lista L(s, 0x3F0000);
  L.w(0x2D000000005003C0ull);
  L.w(0x2F30000000000000ull);
  L.w(0x3F10013F00000000ull | ZB);
  L.w(0x37000000FFFCFFFCull);
  L.w(0x364FC3BC00000000ull);
  L.w(0x3F10013F00000000ull | IMG);
  L.w(0x3E00000000000000ull | ZB);
  L.w(0x3700000000010001ull);
  L.w(0x364FC3BC00000000ull);
  L.w(0x2700000000000000ull);
  L.w(0x3D10001F00000000ull | TEX);       // imagem 32 de largura
  L.w(0x3510100007000000ull);             // tile 7: linha de 8 palavras
  L.w(0x340000000707C07Cull);             // carrega 32x32
  L.w(0x3510100000000000ull);
  L.w(0x320000000007C07Cull);
  // 1 ciclo, filtro bilinear, z, combinador = texel0 * shade
  L.w(0x2F08200000000030ull);
  L.w(0x3C121824FF33FFFFull);
  const uint32_t ini = L.pos;
  for (int k = 0; k < 20; k++) triangulo(L, 7, 0, 0, 320, 240, 0xFFFFFFFF, (0x2000 - k * 100) << 16, 0, 0x60);
  L.w(0x2900000000000000ull);
  (void)ini;
  const uint64_t p0 = s.rdp.pixels;
  const auto t0 = std::chrono::steady_clock::now();
  const int N = std::getenv("N64_BENCH") ? std::atoi(std::getenv("N64_BENCH")) : 10;
  for (int i = 0; i < N; i++) L.roda();
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double mp = double(s.rdp.pixels - p0) / sec / 1e6;
  std::printf("  RDP: %.1f milhões de pixels/s (triângulos com textura bilinear, shade e z)\n", mp);
}

int main(int argc, char** argv) {
  if (argc >= 5 && std::string(argv[1]) == "--png") {
    auto c = abre(argv[2]);
    if (!c) return 1;
    for (int i = 0; i < std::atoi(argv[3]); i++) c->runFrame();
    writePng(argv[4], c->framebuffer(), c->width(), c->height());
    return 0;
  }
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/n64";
  if (std::getenv("N64_BENCH")) { testeRdpDireto(dir); return 0; }
  testeCpu(dir);
  testeRsp(dir);
  testeGfx(dir);
  testeIo(dir);
  testeSaves(dir);
  testeEstado(dir);
  testeRdpDireto(dir);
  std::printf("RESULTADO n64: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
