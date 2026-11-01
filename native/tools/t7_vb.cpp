// Testes do núcleo Virtual Boy.
//   indigo-t7_vb <pasta>                     roda todas as verificações (pasta = /root/indigo-env/roms/vb)
//   indigo-t7_vb rom arquivo.vb quadros saida.png [modo] [teclas]   grava a tela depois de N quadros
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../src/vb/vb.h"

using namespace indigo;
using namespace indigo::vb;

namespace {
std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

// PNG sem compressão (blocos "stored" do zlib)
void writePng(const std::string& path, const uint32_t* px, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const uint32_t c = px[y * w + x];
      raw.push_back(uint8_t(c)); raw.push_back(uint8_t(c >> 8)); raw.push_back(uint8_t(c >> 16));
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n == raw.size());
    z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
  }
  const uint32_t ad = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(ad >> s));
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    const uint32_t n = static_cast<uint32_t>(data.size());
    const uint8_t len[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
    std::fwrite(len, 1, 4, f);
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    std::fwrite(td.data(), 1, td.size(), f);
    const uint32_t c = crc32(td.data(), td.size());
    const uint8_t cc[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
    std::fwrite(cc, 1, 4, f);
  };
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  std::fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                               uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  std::fclose(f);
}

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
  g_checks++;
  if (!ok) { g_fails++; std::printf("  FALHOU: %s\n", what); }
}

// mesmo roteiro de teclas do frontend de referência (refvb.c): A e START alternados a cada segundo
uint32_t scriptKeys(int f) {
  const int ph = (f / 50) % 4;
  return ph == 1 ? uint32_t(KEY_A) : ph == 3 ? uint32_t(KEY_START) : 0u;
}

// compara com o Mednafen (beetle-vb): canal vermelho dos dois olhos lado a lado a cada "passo" quadros.
// Devolve o número de quadros diferentes (e imprime o primeiro).
int compareRef(const std::string& romPath, int frames, int step, const std::string& refPath, int* checked) {
  const auto rom = readAll(romPath), ref = readAll(refPath);
  *checked = 0;
  if (rom.empty() || ref.empty()) return -1;
  VBCore core(rom, nullptr);
  core.setStereoMode(2);
  int bad = 0;
  for (int f = 0; f < frames; f++) {
    core.setKeys(scriptKeys(f));
    core.runFrame();
    if ((f + 1) % step) continue;
    const size_t off = static_cast<size_t>((f + 1) / step - 1) * 768 * 224;
    if (off + 768 * 224 > ref.size()) break;
    (*checked)++;
    int diff = 0, first = -1;
    for (int i = 0; i < 768 * 224; i++)
      if ((core.framebuffer()[i] & 0xFF) != ref[off + i]) { if (first < 0) first = i; diff++; }
    if (diff) {
      if (!bad) std::printf("  quadro %d: %d pontos diferentes (primeiro x=%d y=%d: %d, ref %d)\n", f + 1, diff,
                            first % 768, first / 768, core.framebuffer()[first] & 0xFF, ref[off + first]);
      if (!bad && std::getenv("VB_DUMP")) {
        std::vector<uint32_t> img(768 * 224);
        writePng(std::string(std::getenv("VB_DUMP")) + "_nosso.png", core.framebuffer(), 768, 224);
        for (int i = 0; i < 768 * 224; i++) img[i] = 0xFF000000u | ref[off + i];
        writePng(std::string(std::getenv("VB_DUMP")) + "_ref.png", img.data(), 768, 224);
      }
      bad++;
    }
  }
  return bad;
}

// ------------------------------------------------------------------------------- instruções codificadas à mão
// Os valores esperados vêm das descrições do manual do V810 (NEC U10082EJ) e do comportamento documentado do
// Virtual Boy; não dependem do código do núcleo.
uint16_t I1(int op, int r1, int r2) { return static_cast<uint16_t>((op << 10) | (r2 << 5) | (r1 & 31)); }
enum : int { MOV = 0x00, ADD = 0x01, SUB = 0x02, CMP = 0x03, SHL = 0x04, SHR = 0x05, SAR = 0x07, MUL = 0x08,
             DIV = 0x09, MULU = 0x0A, DIVU = 0x0B, SETF = 0x12, TRAP = 0x18, RETI = 0x19, HALT = 0x1A, BSTR = 0x1F,
             CAXI = 0x3A, FPP = 0x3E };
const uint16_t HALT_OP = 0x1A << 10;

struct Cpu {
  std::unique_ptr<VB> vb;
  V810& c() { return vb->cpu; }
};

// monta uma ROM de 64 KB com o código em 0x07000000 e HALT em todos os vetores (0xFE00–0xFFFF)
Cpu run(const std::vector<uint16_t>& code, const std::function<void(VB&)>& setup, int64_t cycles = 100000) {
  std::vector<uint8_t> rom(0x10000, 0);
  for (size_t i = 0; i < code.size(); i++) { rom[i * 2] = uint8_t(code[i]); rom[i * 2 + 1] = uint8_t(code[i] >> 8); }
  const size_t end = code.size() * 2;
  rom[end] = uint8_t(HALT_OP); rom[end + 1] = uint8_t(HALT_OP >> 8);
  for (size_t a = 0xFE00; a < 0x10000; a += 2) { rom[a] = uint8_t(HALT_OP); rom[a + 1] = uint8_t(HALT_OP >> 8); }
  Cpu t{std::make_unique<VB>(rom)};
  VB& v = *t.vb;
  v.cpu.pc = 0x07000000;
  v.cpu.sr[V810::PSW] = 0;
  setup(v);
  v.runUntil = v.ts + cycles;
  v.cpu.run();
  return t;
}
uint32_t psw(Cpu& t) { return t.c().sr[V810::PSW]; }
uint32_t fl(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// modelo bit a bit (independente) das operações de cadeia de bits, para conferir MOVBSU & cia.
uint32_t memBit(const std::vector<uint32_t>& w, uint64_t bit) { return (w[bit >> 5] >> (bit & 31)) & 1; }

void cpuTests() {
  std::printf("CPU V810 (instruções codificadas à mão):\n");
  {  // ADD com estouro
    auto t = run({I1(ADD, 1, 2)}, [](VB& v) { v.cpu.r[1] = 0x7FFFFFFF; v.cpu.r[2] = 1; });
    check(t.c().r[2] == 0x80000000u && (psw(t) & 0xF) == (V810::F_OV | V810::F_S), "ADD 0x7FFFFFFF+1: OV e S");
  }
  {  // SUB com empréstimo
    auto t = run({I1(SUB, 1, 2)}, [](VB& v) { v.cpu.r[1] = 2; v.cpu.r[2] = 1; });
    check(t.c().r[2] == 0xFFFFFFFFu && (psw(t) & 0xF) == (V810::F_CY | V810::F_S), "SUB 1-2: CY e S");
  }
  {  // MUL: resultado de 64 bits, alto em r30
    auto t = run({I1(MUL, 1, 2)}, [](VB& v) { v.cpu.r[1] = 0x10000; v.cpu.r[2] = 0x10000; });
    check(t.c().r[2] == 0 && t.c().r[30] == 1 && (psw(t) & (V810::F_OV | V810::F_Z)) == (V810::F_OV | V810::F_Z),
          "MUL 0x10000*0x10000: baixo 0, r30=1, OV, Z");
    auto u = run({I1(MULU, 1, 2)}, [](VB& v) { v.cpu.r[1] = 0xFFFFFFFF; v.cpu.r[2] = 2; });
    check(u.c().r[2] == 0xFFFFFFFEu && u.c().r[30] == 1 && (psw(u) & V810::F_OV), "MULU 0xFFFFFFFF*2");
  }
  {  // DIV: quociente truncado, resto em r30; 0x80000000 / -1 estoura
    auto t = run({I1(DIV, 1, 2)}, [](VB& v) { v.cpu.r[1] = 2; v.cpu.r[2] = static_cast<uint32_t>(-7); });
    check(t.c().r[2] == static_cast<uint32_t>(-3) && t.c().r[30] == static_cast<uint32_t>(-1), "DIV -7/2 = -3 resto -1");
    auto u = run({I1(DIV, 1, 2)}, [](VB& v) { v.cpu.r[1] = 0xFFFFFFFF; v.cpu.r[2] = 0x80000000; });
    check(u.c().r[2] == 0x80000000u && u.c().r[30] == 0 && (psw(u) & V810::F_OV), "DIV 0x80000000/-1: OV");
    auto w = run({I1(DIVU, 1, 2)}, [](VB& v) { v.cpu.r[1] = 10; v.cpu.r[2] = 0xFFFFFFFF; });
    check(w.c().r[2] == 429496729u && w.c().r[30] == 5, "DIVU 0xFFFFFFFF/10");
  }
  {  // divisão por zero: exceção 0xFF80, volta para a própria instrução
    auto t = run({I1(MOV, 0, 0), I1(DIV, 1, 2)}, [](VB& v) { v.cpu.r[1] = 0; v.cpu.r[2] = 5; });
    check((t.c().sr[V810::ECR] & 0xFFFF) == 0xFF80 && t.c().sr[V810::EIPC] == 0x07000002 &&
              t.c().pc == 0xFFFFFF82 && (psw(t) & V810::F_EP) && t.c().r[2] == 5,
          "DIV por zero: ECR=FF80, EIPC na instrução, vetor FFFFFF80");
  }
  {  // deslocamentos: CY é o último bit que saiu; com 0 posições CY=0
    auto t = run({I1(SHR, 1, 2)}, [](VB& v) { v.cpu.r[1] = 4; v.cpu.r[2] = 0x18; });
    check(t.c().r[2] == 1 && (psw(t) & V810::F_CY), "SHR 0x18>>4: CY=1");
    auto u = run({I1(SHL, 1, 2)}, [](VB& v) { v.cpu.r[1] = 32; v.cpu.r[2] = 7; v.cpu.sr[V810::PSW] = V810::F_CY; });
    check(u.c().r[2] == 7 && !(psw(u) & V810::F_CY), "SHL por 32 (= 0 posições): CY=0");
    auto w = run({I1(SAR, 1, 2)}, [](VB& v) { v.cpu.r[1] = 1; v.cpu.r[2] = 0x80000001; });
    check(w.c().r[2] == 0xC0000000u && (psw(w) & (V810::F_CY | V810::F_S)) == (V810::F_CY | V810::F_S), "SAR");
  }
  {  // SETF com as condições depois de CMP
    auto t = run({I1(CMP, 1, 2), I1(SETF, 0x6, 3), I1(SETF, 0x2, 4), I1(SETF, 0xF, 5), I1(SETF, 0x1, 6)},
                 [](VB& v) { v.cpu.r[1] = 5; v.cpu.r[2] = static_cast<uint32_t>(-3); });
    check(t.c().r[3] == 1 && t.c().r[4] == 0 && t.c().r[5] == 0 && t.c().r[6] == 0, "SETF LT/Z/GT/C depois de CMP -3,5");
  }
  {  // CAXI: troca com r30 quando igual
    auto t = run({I1(CAXI, 1, 2), 0x0010}, [](VB& v) {
      v.cpu.r[1] = 0x05000000 - 0x10; v.cpu.r[2] = 5; v.cpu.r[30] = 9; v.write32(0x05000000, 5); });
    check(t.vb->read32(0x05000000) == 9 && t.c().r[2] == 5 && (psw(t) & V810::F_Z), "CAXI igual: grava r30");
    auto u = run({I1(CAXI, 1, 2), 0x0000}, [](VB& v) {
      v.cpu.r[1] = 0x05000000; v.cpu.r[2] = 4; v.cpu.r[30] = 9; v.write32(0x05000000, 5); });
    check(u.vb->read32(0x05000000) == 5 && u.c().r[2] == 5 && !(psw(u) & V810::F_Z), "CAXI diferente: reg2 = memória");
  }
  {  // instruções próprias do VB
    auto t = run({I1(FPP, 1, 2), 0x0C << 10, I1(FPP, 3, 4), 0x0A << 10, I1(FPP, 0, 5), 0x08 << 10, I1(FPP, 0, 6), 0x09 << 10},
                 [](VB& v) { v.cpu.r[1] = 0x1FFFF; v.cpu.r[2] = 3; v.cpu.r[3] = 1; v.cpu.r[5] = 0x12345678; v.cpu.r[6] = 0x12345678; });
    check(t.c().r[2] == static_cast<uint32_t>(-3), "MPYHW com -1 (17 bits)");
    check(t.c().r[4] == 0x80000000u, "REV de 1");
    check(t.c().r[5] == 0x12347856u && t.c().r[6] == 0x56781234u, "XB e XH");
  }
  {  // TRAP e RETI
    auto t = run({I1(TRAP, 0x15, 0)}, [](VB&) {});
    check((t.c().sr[V810::ECR] & 0xFFFF) == 0xFFB5 && t.c().sr[V810::EIPC] == 0x07000002 && t.c().pc == 0xFFFFFFB2,
          "TRAP 0x15: ECR=FFB5, retorno na instrução seguinte, vetor FFFFFFB0");
    auto u = run({I1(RETI, 0, 0)}, [](VB& v) {
      v.cpu.sr[V810::PSW] = V810::F_EP; v.cpu.sr[V810::EIPC] = 0x07000002; v.cpu.sr[V810::EIPSW] = V810::F_Z; });
    check(u.c().pc == 0x07000004 && psw(u) == V810::F_Z, "RETI volta para EIPC com EIPSW");
  }
  {  // código inválido
    auto t = run({I1(0x1B, 0, 0)}, [](VB&) {});
    check((t.c().sr[V810::ECR] & 0xFFFF) == 0xFF90 && t.c().sr[V810::EIPC] == 0x07000000, "código inválido: FF90");
  }
  {  // interrupção do VIP (nível 4) tira do HALT, ajusta o nível e mascara
    auto t = run({HALT_OP}, [](VB& v) { v.setIrq(4, true); v.cpu.sr[V810::PSW] = 4u << 16; });
    check(t.c().pc == 0xFFFFFE42 && (t.c().sr[V810::ECR] & 0xFFFF) == 0xFE40 && ((psw(t) >> 16) & 15) == 5 &&
              (psw(t) & V810::F_EP) && (psw(t) & V810::F_ID) && t.c().sr[V810::EIPC] == 0x07000000,
          "interrupção nível 4: vetor FFFFFE40, I=5, EP e ID");
    auto u = run({HALT_OP}, [](VB& v) { v.setIrq(4, true); v.cpu.sr[V810::PSW] = 5u << 16; });
    check(u.c().pc == 0x07000002, "interrupção abaixo do nível do PSW é ignorada");
  }
  // ---------------------------------------------------------------- ponto flutuante
  auto fpu = [](int sub, uint32_t a, uint32_t b) {  // reg2 (r2) = a, reg1 (r1) = b
    return run({I1(FPP, 1, 2), static_cast<uint16_t>(sub << 10)}, [=](VB& v) { v.cpu.r[2] = a; v.cpu.r[1] = b; });
  };
  {
    auto t = fpu(0x04, fl(1.5f), fl(2.25f));
    check(t.c().r[2] == fl(3.75f) && (psw(t) & 0x3F) == 0, "ADDF.S 1,5+2,25 exato");
    auto u = fpu(0x05, fl(2.0f), fl(2.0f));
    check(u.c().r[2] == 0 && (psw(u) & 0xF) == V810::F_Z, "SUBF.S igual: zero, Z=1 S=0 CY=0");
    auto w = fpu(0x05, fl(1.0f), fl(3.0f));
    check(w.c().r[2] == fl(-2.0f) && (psw(w) & 0xF) == (V810::F_S | V810::F_CY), "SUBF.S negativo: S e CY");
    auto d = fpu(0x07, fl(1.0f), fl(3.0f));
    check(d.c().r[2] == fl(1.0f / 3.0f) && (psw(d) & V810::F_FPR), "DIVF.S 1/3 arredondado, FPR");
    auto c = fpu(0x00, fl(1.0f), fl(2.0f));
    check((psw(c) & 0xF) == (V810::F_S | V810::F_CY) && c.c().r[2] == fl(1.0f), "CMPF.S 1<2");
    auto ov = fpu(0x06, fl(1e30f), fl(1e30f));
    check((ov.c().sr[V810::ECR] & 0xFFFF) == 0xFF64 && (psw(ov) & V810::F_FOV) && ov.c().pc == 0xFFFFFF62 &&
              ov.c().sr[V810::EIPC] == 0x07000000,
          "MULF.S estouro: exceção FOV (FF64)");
    auto un = fpu(0x06, fl(1e-30f), fl(1e-30f));
    check(un.c().r[2] == 0 && (psw(un) & (V810::F_FUD | V810::F_FPR)) == (V810::F_FUD | V810::F_FPR) && (psw(un) & V810::F_Z),
          "MULF.S muito pequeno: zero com FUD e FPR");
    auto dz = fpu(0x07, fl(1.0f), 0);
    check((dz.c().sr[V810::ECR] & 0xFFFF) == 0xFF68 && (psw(dz) & V810::F_FZD) && dz.c().r[2] == fl(1.0f),
          "DIVF.S por zero: FZD (FF68), registro intacto");
    auto iv = fpu(0x07, 0, 0);
    check((iv.c().sr[V810::ECR] & 0xFFFF) == 0xFF70 && (psw(iv) & V810::F_FIV), "DIVF.S 0/0: FIV (FF70)");
    auto ro = fpu(0x04, 0x7FC00000, fl(1.0f));
    check((ro.c().sr[V810::ECR] & 0xFFFF) == 0xFF60 && (psw(ro) & V810::F_FRO), "ADDF.S com NaN: operando reservado (FF60)");
    auto dn = fpu(0x04, 0x00000001, fl(1.0f));
    check((dn.c().sr[V810::ECR] & 0xFFFF) == 0xFF60, "ADDF.S com desnormalizado: operando reservado");
    auto cv = run({I1(FPP, 1, 2), 0x03 << 10, I1(FPP, 3, 4), 0x03 << 10, I1(FPP, 5, 6), 0x0B << 10, I1(FPP, 7, 8), 0x02 << 10},
                  [](VB& v) { v.cpu.r[1] = fl(2.5f); v.cpu.r[3] = fl(3.5f); v.cpu.r[5] = fl(-2.75f); v.cpu.r[7] = 16777217; });
    check(cv.c().r[2] == 2 && cv.c().r[4] == 4, "CVT.SW arredonda para o par (2,5→2, 3,5→4)");
    check(cv.c().r[6] == static_cast<uint32_t>(-2), "TRNC.SW -2,75 → -2");
    check(cv.c().r[8] == fl(16777216.0f) && (psw(cv) & V810::F_FPR), "CVT.WS 16777217 → 16777216 com FPR");
    auto big = run({I1(FPP, 1, 2), 0x03 << 10}, [](VB& v) { v.cpu.r[1] = fl(3e9f); });
    check((big.c().sr[V810::ECR] & 0xFFFF) == 0xFF70, "CVT.SW fora do alcance: FIV");
  }
  // ---------------------------------------------------------------- cadeias de bits
  {
    std::vector<uint32_t> src = {0xDEADBEEF, 0x12345678, 0xCAFEBABE}, dst = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    for (int sub = 8; sub < 16; sub++) {
      auto t = run({static_cast<uint16_t>((BSTR << 10) | sub)}, [&](VB& v) {
        for (int i = 0; i < 3; i++) v.write32(0x05000000 + i * 4, src[i]);
        for (int i = 0; i < 4; i++) v.write32(0x05000100 + i * 4, dst[i]);
        v.cpu.r[30] = 0x05000000; v.cpu.r[27] = 5; v.cpu.r[29] = 0x05000100; v.cpu.r[26] = 30; v.cpu.r[28] = 61;
      });
      std::vector<uint32_t> exp = dst;
      for (uint64_t k = 0; k < 61; k++) {
        const uint32_t s = memBit(src, 5 + k), db = 30 + k;
        uint32_t& w = exp[db >> 5];
        const uint32_t d = (w >> (db & 31)) & 1;
        uint32_t r = 0;
        switch (sub & 7) {
          case 0: r = d | s; break; case 1: r = d & s; break; case 2: r = d ^ s; break; case 3: r = s; break;
          case 4: r = d | !s; break; case 5: r = d & !s; break; case 6: r = d ^ !s; break; default: r = !s; break;
        }
        w = (w & ~(1u << (db & 31))) | (r << (db & 31));
      }
      bool ok = t.c().r[28] == 0 && t.c().r[27] == (5 + 61) % 32 && t.c().r[26] == (30 + 61) % 32 &&
                t.c().r[30] == 0x05000000 + 4 * ((5 + 61) / 32) && t.c().r[29] == 0x05000100 + 4 * ((30 + 61) / 32);
      for (int i = 0; i < 4; i++) ok = ok && t.vb->read32(0x05000100 + i * 4) == exp[i];
      static const char* N[] = {"ORBSU", "ANDBSU", "XORBSU", "MOVBSU", "ORNBSU", "ANDNBSU", "XORNBSU", "NOTBSU"};
      check(ok, (std::string(N[sub & 7]) + " de 61 bits atravessando palavras").c_str());
    }
    // busca de 1 para cima: palavra 0 = 0, palavra 1 com o bit 3 ligado; começando no bit 4 da palavra 0
    auto s = run({static_cast<uint16_t>((BSTR << 10) | 2)}, [](VB& v) {
      v.write32(0x05000000, 0); v.write32(0x05000004, 1u << 3);
      v.cpu.r[30] = 0x05000000; v.cpu.r[27] = 4; v.cpu.r[28] = 100; v.cpu.r[29] = 7; });
    check(!(psw(s) & V810::F_Z) && s.c().r[29] == 7 + 28 + 3 && s.c().r[28] == 100 - 31 && s.c().r[30] == 0x05000004 &&
              s.c().r[27] == 2,
          "SCH1BSU: encontra o bit 3 da palavra seguinte (registros no bit anterior)");
    // busca de 0 para baixo atravessando para a palavra anterior
    auto d = run({static_cast<uint16_t>((BSTR << 10) | 1)}, [](VB& v) {
      v.write32(0x05000000, 0x7FFFFFFF); v.write32(0x05000004, 0xFFFFFFFF);
      v.cpu.r[30] = 0x05000004; v.cpu.r[27] = 2; v.cpu.r[28] = 10; v.cpu.r[29] = 0; });
    check(!(psw(d) & V810::F_Z) && d.c().r[29] == 3 && d.c().r[28] == 7 && d.c().r[30] == 0x05000004 && d.c().r[27] == 0,
          "SCH0BSD: encontra o bit 31 da palavra anterior");
    auto n = run({static_cast<uint16_t>((BSTR << 10) | 0)}, [](VB& v) {
      v.write32(0x05000000, 0xFFFFFFFF); v.cpu.r[30] = 0x05000000; v.cpu.r[27] = 0; v.cpu.r[28] = 20; v.cpu.r[29] = 0; });
    check((psw(n) & V810::F_Z) && n.c().r[28] == 0 && n.c().r[29] == 20 && n.c().r[27] == 20, "SCH0BSU sem achar: Z=1");
  }
}

// ------------------------------------------------------------------------------- testes com as ROMs
void romTests(const std::string& dir) {
  struct Ref { const char* rom; const char* ref; int frames, step; };
  static const Ref REFS[] = {
      {"VBeatJukebox/vbeatjukebox.vb", "vbeatjukebox.ref", 3000, 25},
      {"vbeat/VBeat16k_PriorArt.vb", "priorart.ref", 3000, 25},
      {"doom/doom_vb.vb", "doom.ref", 3000, 25},
      {"cputeste.vb", "cputeste.ref", 50, 10},
      {"vipteste.vb", "vipteste.ref", 600, 3},
  };
  std::printf("Comparação quadro a quadro com o Mednafen (beetle-vb, dois olhos):\n");
  for (const Ref& r : REFS) {
    int n = 0;
    const int bad = compareRef(dir + "/" + r.rom, r.frames, r.step, dir + "/" + r.ref, &n);
    std::printf("  %s: %d quadros comparados, %d diferentes\n", r.rom, n, bad);
    check(bad == 0 && n == r.frames / r.step, r.rom);
  }

  std::printf("cputeste.vb contra o mesmo programa compilado no PC:\n");
  {
    VBCore core(readAll(dir + "/cputeste.vb"), nullptr);
    for (int i = 0; i < 50; i++) core.runFrame();
    const auto exp = readAll(dir + "/cputeste.esperado");
    VB& v = core.vb();
    check(v.read32(0x0500BFF8) == 0x454E4F44, "cputeste terminou");
    int bad = 0;
    for (size_t i = 0; i + 4 <= exp.size(); i += 4) {
      const uint32_t e = exp[i] | exp[i + 1] << 8 | exp[i + 2] << 16 | uint32_t(exp[i + 3]) << 24;
      if (v.read32(0x0500C000 + static_cast<uint32_t>(i)) != e) {
        if (!bad) std::printf("  resultado %zu: %08X, esperado %08X\n", i / 4, v.read32(0x0500C000 + static_cast<uint32_t>(i)), e);
        bad++;
      }
    }
    std::printf("  %zu resultados, %d diferentes\n", exp.size() / 4, bad);
    check(!exp.empty() && bad == 0, "resultados do cputeste");
  }

  const auto doom = readAll(dir + "/doom/doom_vb.vb");
  std::printf("Save state (Doom):\n");
  {
    VBCore core(doom, nullptr);
    std::vector<float> L(4096), R(4096);
    for (int i = 0; i < 400; i++) { core.setKeys(scriptKeys(i)); core.runFrame(); core.pullAudio(L.data(), R.data(), core.audioAvailable() < 4096 ? core.audioAvailable() : 4096); }
    const auto st = core.saveState();
    auto runN = [&](uint32_t& hv, uint32_t& ha) {
      hv = 0; ha = 0;
      for (int i = 0; i < 150; i++) {
        core.setKeys(scriptKeys(400 + i));
        core.runFrame();
        hv = crc32(reinterpret_cast<const uint8_t*>(core.framebuffer()), 384 * 224 * 4, hv);
        const size_t n = core.audioAvailable() < 4096 ? core.audioAvailable() : 4096;
        core.pullAudio(L.data(), R.data(), n);
        ha = crc32(reinterpret_cast<const uint8_t*>(L.data()), n * 4, ha);
      }
    };
    uint32_t v1, a1, v2, a2;
    runN(v1, a1);
    core.loadState(st);
    runN(v2, a2);
    check(v1 == v2 && a1 == a2, "salvar, rodar 150 quadros, carregar e rodar de novo: mesma tela e mesmo som");
    core.loadState(st);
    check(core.saveState() == st, "salvar → carregar → salvar dá os mesmos bytes");
    // estados danificados: truncados, bytes trocados, de outro jogo
    uint32_t seed = 99;
    int ok = 0, total = 0;
    for (int k = 0; k < 60; k++) {
      std::vector<uint8_t> bad = st;
      seed = seed * 1103515245 + 12345;
      if (k % 3 == 0) bad.resize(seed % st.size());
      else for (int j = 0; j < 40; j++) { seed = seed * 1103515245 + 12345; bad[(seed >> 8) % bad.size()] ^= uint8_t(seed >> 3); }
      total++;
      try { core.loadState(bad); } catch (const std::exception&) {}
      for (int i = 0; i < 3; i++) core.runFrame();
      ok++;
    }
    check(ok == total, "estados danificados não travam (exceção ou continua)");
    bool threw = false;
    try { VBCore other(readAll(dir + "/vipteste.vb"), nullptr); other.loadState(st); } catch (const std::exception&) { threw = true; }
    check(threw, "estado de outro jogo é recusado");
  }

  std::printf("Som, saída 3D e SRAM:\n");
  {
    VBCore core(readAll(dir + "/VBeatJukebox/vbeatjukebox.vb"), nullptr);
    core.setSampleRate(48000);
    std::vector<float> L(8192), R(8192);
    double sum = 0;
    size_t total = 0;
    for (int i = 0; i < 500; i++) {
      core.runFrame();
      const size_t n = core.audioAvailable();
      core.pullAudio(L.data(), R.data(), n < 8192 ? n : 8192);
      for (size_t k = 0; k < n && k < 8192; k++) sum += double(L[k]) * L[k] + double(R[k]) * R[k];
      total += n;
    }
    const double rms = std::sqrt(sum / (2.0 * total));
    std::printf("  jukebox: %zu amostras em 500 quadros (esperado %.0f), RMS %.3f\n", total, 500 * 48000 / core.fps(), rms);
    check(std::fabs(total - 500 * 48000 / core.fps()) < 200 && rms > 0.01 && rms < 1.0, "som do VSU tocando na taxa certa");
  }
  {
    VBCore core(readAll(dir + "/vipteste.vb"), nullptr);
    for (int i = 0; i < 30; i++) core.runFrame();
    std::vector<uint32_t> left(core.framebuffer(), core.framebuffer() + 384 * 224);
    core.setStereoMode(1);
    bool ok = core.width() == 384;
    for (int i = 0; i < 384 * 224 && ok; i++) {
      const uint32_t c = core.framebuffer()[i];
      ok = (c & 0xFF) == (left[i] & 0xFF) && ((c >> 8) & 0xFF) == ((c >> 16) & 0xFF) && (left[i] & 0xFFFF00) == 0;
    }
    core.setStereoMode(2);
    ok = ok && core.width() == 768;
    for (int y = 0; y < 224 && ok; y++)
      for (int x = 0; x < 384 && ok; x++) ok = core.framebuffer()[y * 768 + x] == left[y * 384 + x];
    check(ok, "modos de saída: olho esquerdo vermelho, anáglifo vermelho/ciano, lado a lado 768x224");
  }
  {
    VBCore core(readAll(dir + "/vipteste.vb"), nullptr);
    check(!core.hasSave(), "sem save antes de o jogo gravar na SRAM");
    core.vb().write16(0x06000010, 0xBEEF);
    check(core.hasSave() && core.saveDirty(), "gravação na SRAM liga o save");
    const auto s = core.exportSave();
    VBCore again(readAll(dir + "/vipteste.vb"), &s);
    check(!core.saveDirty() && s.size() == 0x10000 && again.vb().read16(0x06000010) == 0xBEEF, "save exportado e importado");
  }
  {  // velocidade: Doom em jogo
    VBCore core(doom, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 600; i++) { core.setKeys(scriptKeys(i)); core.runFrame(); }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  velocidade: Doom a %.0f quadros/s (o console roda a 50,27)\n", 600 / s);
  }
}

// ROMs de lixo (tamanhos variados) e estados aleatórios: nada pode travar nem sair da memória
void fuzzTests() {
  std::printf("Robustez (ROMs aleatórias e estados de lixo):\n");
  uint32_t x = 2024;
  auto rnd = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
  int ran = 0;
  for (int k = 0; k < 24; k++) {
    const size_t sizes[] = {1, 3, 100, 1024, 5000, 65536, 300000, 1 << 20};
    std::vector<uint8_t> rom(sizes[k % 8]);
    for (auto& b : rom) b = uint8_t(rnd() >> 24);
    VBCore core(rom, nullptr);
    for (int f = 0; f < 10; f++) { core.setKeys(rnd()); core.runFrame(); }
    std::vector<uint8_t> st = core.saveState();
    for (int j = 0; j < 4; j++) {
      std::vector<uint8_t> bad = st;
      for (auto& b : bad) if ((rnd() & 63) == 0) b = uint8_t(rnd());
      try { core.loadState(bad); } catch (const std::exception&) {}
      core.runFrame();
    }
    ran++;
  }
  check(ran == 24, "24 ROMs aleatórias rodaram sem travar");
  bool threw = false;
  try { VBCore e(std::vector<uint8_t>{}, nullptr); } catch (const std::runtime_error&) { threw = true; }
  check(threw, "ROM vazia é recusada com mensagem");
  check(isVbRom({1, 2}, "jogo.vb") && !isVbRom({1, 2}, "jogo.gba"), "detecção pela extensão .vb");
}

int romMode(int argc, char** argv) {
  const auto rom = readAll(argv[2]);
  VBCore core(rom, nullptr);
  const int frames = std::atoi(argv[3]);
  if (argc > 5) core.setStereoMode(std::atoi(argv[5]));
  for (int i = 0; i < frames; i++) {
    core.setKeys(scriptKeys(i));
    core.runFrame();
  }
  writePng(argv[4], core.framebuffer(), core.width(), core.height());
  std::printf("título \"%s\", tela %08X\n", core.title().c_str(),
              crc32(reinterpret_cast<const uint8_t*>(core.framebuffer()), core.width() * core.height() * 4));
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::string(argv[1]) == "rom") return romMode(argc, argv);
  if (argc >= 6 && std::string(argv[1]) == "cmp") {
    int n = 0;
    const int bad = compareRef(argv[2], std::atoi(argv[3]), std::atoi(argv[4]), argv[5], &n);
    std::printf("%d quadros comparados, %d diferentes\n", n, bad);
    return bad != 0;
  }
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/vb";
  cpuTests();
  fuzzTests();
  if (!std::ifstream(dir + "/cputeste.vb")) {
    std::printf("pasta %s sem os testes (rode native/tools/t7_vb.sh): pulei os testes com ROMs\n", dir.c_str());
  } else {
    romTests(dir);
  }
  std::printf("RESULTADO vb: %d verificações, %d falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
