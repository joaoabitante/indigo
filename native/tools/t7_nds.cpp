// Testes do núcleo do Nintendo DS.
//   indigo-t7_nds [pasta]   (pasta = /root/indigo-env/roms/nds, preparada pelo t7_nds.sh)
// - CPU no modo ARMv4T (ARM7) contra a SingleStepTests/ARM7TDMI (MIT): arquivos em pasta/arm7tdmi/v1 e, para as
//   instruções ARM, ../3do/arm7tdmi/v1 (baixados pelo t9b_3do.sh)
// - instruções do ARMv5TE (cpu5.nds, t7_nds_cpu5.s) com valores calculados à mão
// - homebrew de teste próprio (teste.nds): divisor/raiz, SWI em alto nível, timers, DMA, IPC, IRQ, cartucho,
//   save por SPI, toque, relógio, som, 2D e 3D (pixels conferidos pela geometria da cena)
// - save state (mesmos quadros depois de carregar; estados adulterados recusados), save do cartucho e velocidade
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/nds/nds.h"

using namespace indigo;
using namespace indigo::nds;

static int fails = 0, checks = 0, skipped = 0;
#define CHECK(cond, ...)                    \
  do {                                      \
    checks++;                               \
    if (!(cond)) {                          \
      fails++;                              \
      std::printf("  FALHA: " __VA_ARGS__); \
      std::printf("\n");                    \
    }                                       \
  } while (0)

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ================================================================================================ régua da CPU
namespace sst {
struct Tr { uint32_t kind, size, addr, data; };
struct State { uint32_t v[40]; };
struct Test { State ini, fin; std::vector<Tr> tr; uint32_t op, base; };

uint32_t rd32(const std::vector<uint8_t>& b, size_t p) {
  return p + 4 <= b.size() ? uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24 : 0;
}

class Bus : public ArmBus {
public:
  const Test* t = nullptr;
  size_t rdi = 0, wri = 0;
  bool fetched = false, bad = false;
  std::vector<Tr> reads, writes;
  void start(const Test& x) {
    t = &x; rdi = wri = 0; fetched = false; bad = false;
    reads.clear(); writes.clear(); mine.clear();
    for (const auto& r : x.tr) { if (r.kind == 1) reads.push_back(r); else if (r.kind == 2) writes.push_back(r); }
  }
  uint32_t rd() {
    if (!fetched) { fetched = true; return t->op; }
    if (rdi < reads.size()) return reads[rdi++].data;
    bad = true;
    return 0;
  }
  // leitura menor que a da régua (LDRSH desalinhado vira leitura de byte): pega a faixa certa do dado
  uint32_t lane(uint32_t a, uint32_t size) {
    const size_t i = rdi;
    const uint32_t v = rd();
    if (!fetched || i >= reads.size()) return v;
    const uint32_t ts = reads[i].size;
    if (ts > size) return v >> ((a & (ts - 1) & ~(size - 1)) * 8);
    return v;
  }
  uint32_t read32(uint32_t) override { return rd(); }
  uint32_t read16(uint32_t a) override { return (fetched ? lane(a, 2) : rd()) & 0xFFFF; }
  uint32_t read8(uint32_t a) override { return (fetched ? lane(a, 1) : rd()) & 0xFF; }
  std::vector<Tr> mine;
  void wr(uint32_t a, uint32_t v, uint32_t size) {
    mine.push_back({2, size, a, v});
    if (wri >= writes.size()) { bad = true; return; }
    const Tr& w = writes[wri++];
    const uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFF : 0xFF;
    const uint32_t am = size == 4 ? ~3u : size == 2 ? ~1u : ~0u;
    if ((w.addr & am) != a || (w.data & mask) != (v & mask) || w.size != size) bad = true;
  }
  void write32(uint32_t a, uint32_t v) override { wr(a, v, 4); }
  void write16(uint32_t a, uint32_t v) override { wr(a, v, 2); }
  void write8(uint32_t a, uint32_t v) override { wr(a, v, 1); }
  const uint8_t* codePage(uint32_t, uint32_t&, uint32_t&, uint32_t&) override { return nullptr; }
  bool swi(int, uint32_t) override { return false; }
};

bool validMode(uint32_t m) { return m == 0x10 || m == 0x11 || m == 0x12 || m == 0x13 || m == 0x17 || m == 0x1B || m == 0x1F; }
int spsrIdx(uint32_t m) { switch (m) { case 0x11: return 0; case 0x13: return 1; case 0x17: return 2; case 0x12: return 3; default: return 4; } }
int bankBase(uint32_t m) { switch (m) { case 0x11: return 21; case 0x13: return 23; case 0x17: return 25; case 0x12: return 27; default: return 29; } }

// formas imprevisíveis no manual (resultado depende do modelo do chip): ficam de fora
bool unpredictable(uint32_t op, bool thumb, uint32_t mode) {
  if (thumb) return false;
  const uint32_t cls = (op >> 25) & 7;
  const int rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF, rs = (op >> 8) & 0xF, rm = op & 0xF;
  const bool isMul = (op & 0x0FC000F0) == 0x00000090, isMull = (op & 0x0F8000F0) == 0x00800090;
  const bool isSwp = (op & 0x0FB00FF0) == 0x01000090;
  const bool isHalf = cls == 0 && (op & 0x90) == 0x90 && !isMul && !isMull && !isSwp;
  if ((op & 0x0FBF0FFF) == 0x010F0000 && rd == 15) return true;                       // MRS para o PC
  if ((cls == 2 || cls == 3) && rn == 15 && ((op & (1u << 21)) || !(op & (1u << 24)))) return true;
  if (cls == 3 && rm == 15) return true;
  if (cls < 2 && !(op & (1u << 25)) && (op & 0x10) && !isMul && !isMull && !isSwp && !isHalf && rs == 15) return true;
  if (cls == 4 && rn == 15) return true;
  if (cls == 4 && (op & (1u << 22)) && (op & (1u << 21)) && !((op & (1u << 20)) && (op & 0x8000))) return true;
  if (isMul && (rn == 15 || rd == 15 || rs == 15 || rm == 15)) return true;
  if (isMull && (rn == 15 || rd == 15 || rs == 15 || rm == 15 || rn == rd)) return true;
  if (isSwp && (rn == 15 || rd == 15 || rm == 15)) return true;
  if (isHalf && (rd == 15 || (rn == 15 && ((op & (1u << 21)) || !(op & (1u << 24)))) || (!(op & (1u << 22)) && rm == 15))) return true;
  if ((op & 0x0FB0FFF0) == 0x0120F000 && rm == 15) return true;
  if ((op & 0x0FFFFFF0) == 0x012FFF10 && rm == 15) return true;  // BX PC
  // S com destino PC no modo usuário/sistema (não há SPSR)
  if (cls < 2 && (op & (1u << 20)) && rd == 15 && !isMul && !isMull && !isSwp && !isHalf && (mode == 0x10 || mode == 0x1F)) return true;
  if (cls == 4 && (op & (1u << 22)) && (op & (1u << 20)) && (op & 0x8000) && (mode == 0x10 || mode == 0x1F)) return true;
  return false;
}

void runFile(const std::string& path, bool mul, bool msr, bool thumb, int& ok, int& bad, int& skip) {
  const auto b = readAll(path);
  if (b.size() < 8 || rd32(b, 0) != 0xD33DBAE0) { std::printf("  arquivo inválido: %s\n", path.c_str()); bad++; return; }
  const uint32_t n = rd32(b, 4);
  size_t p = 8;
  Bus bus;
  Arm cpu(bus, false);
  cpu.reset();
  cpu.costShift = 0;
  int shown = 0;
  for (uint32_t i = 0; i < n && p < b.size(); i++) {
    Test t;
    const uint32_t full = rd32(b, p);
    size_t q = p + 4;
    auto state = [&](State& s) { const uint32_t sz = rd32(b, q); for (int k = 0; k < 40; k++) s.v[k] = rd32(b, q + 8 + 4 * k); q += sz; };
    state(t.ini);
    state(t.fin);
    { const uint32_t nt = rd32(b, q + 8);
      const uint32_t sz = rd32(b, q);
      for (uint32_t k = 0; k < nt; k++) { const size_t o = q + 12 + 24 * k; t.tr.push_back({rd32(b, o), rd32(b, o + 4), rd32(b, o + 8), rd32(b, o + 12)}); }
      q += sz; }
    t.op = rd32(b, q + 8);
    t.base = rd32(b, q + 12);
    p += full;
    const uint32_t im = t.ini.v[31] & 0x1F, fm = t.fin.v[31] & 0x1F;
    if (!validMode(im) || !validMode(fm) || unpredictable(t.op, thumb, im)) { skip++; continue; }
    if (msr && ((t.ini.v[31] ^ t.fin.v[31]) & 0x20)) { skip++; continue; }
    // estado inicial: R = banco do usuário; bancos dos modos em 16..30; SPSRs em 32..36
    cpu.reset();
    cpu.switchMode(M_USR);
    for (int k = 0; k < 15; k++) cpu.r[k] = t.ini.v[k];
    for (uint32_t m : {0x11u, 0x12u, 0x13u, 0x17u, 0x1Bu}) {
      cpu.switchMode(int(m));
      const int bb = bankBase(m);
      cpu.r[13] = t.ini.v[bb]; cpu.r[14] = t.ini.v[bb + 1];
      if (m == 0x11) for (int k = 0; k < 5; k++) cpu.r[8 + k] = t.ini.v[16 + k];
      cpu.setSpsr(t.ini.v[32 + spsrIdx(m)]);
    }
    cpu.setCpsr(t.ini.v[31]);
    cpu.nextPC = t.base;
    bus.start(t);
    cpu.step();
    // estado final
    uint32_t got[31], sp[5];
    const uint32_t keep = cpu.cpsr();
    cpu.switchMode(M_USR);
    for (int k = 0; k < 15; k++) got[k] = cpu.r[k];
    for (uint32_t m : {0x11u, 0x12u, 0x13u, 0x17u, 0x1Bu}) {
      cpu.switchMode(int(m));
      const int bb = bankBase(m);
      got[bb - 1] = cpu.r[13]; got[bb] = cpu.r[14];
      if (m == 0x11) for (int k = 0; k < 5; k++) got[15 + k] = cpu.r[8 + k];
      sp[spsrIdx(m)] = cpu.spsr();
    }
    cpu.switchMode(int(keep & 0x1F));
    bool same = !bus.bad && bus.wri == bus.writes.size();
    int firstBad = -1;
    for (int k = 0; k < 15 && same; k++) if (got[k] != t.fin.v[k]) { same = false; firstBad = k; }
    for (int k = 16; k < 31 && same; k++) if (got[k - 1] != t.fin.v[k]) { same = false; firstBad = k; }
    const bool tf = keep & 0x20;
    const uint32_t pcView = cpu.nextPC + (tf ? 4 : 8);
    const uint32_t pcMask = tf ? ~1u : ~3u;  // PC desalinhado no ARM é imprevisível: só os bits que valem
    if (same && (pcView & pcMask) != (t.fin.v[15] & pcMask)) { same = false; firstBad = 15; }
    // MSR: bits reservados e o T (imprevisíveis) ficam de fora
    const uint32_t cmask = mul ? 0xD00000FFu : msr ? 0xF00000DFu : 0xF00000FFu;
    if (same && (keep & cmask) != (t.fin.v[31] & cmask)) { same = false; firstBad = 31; }
    for (int k = 0; k < 5 && same; k++) if ((sp[k] & 0xF00000FFu) != (t.fin.v[32 + k] & 0xF00000FFu)) { same = false; firstBad = 32 + k; }
    if (same) ok++;
    else {
      bad++;
      if (shown++ < 3) {
        if (getenv("SST_DEBUG")) { for (const auto& x : t.tr) std::printf("      tr kind %u size %u addr %08X data %08X\n", x.kind, x.size, x.addr, x.data);
          for (const auto& x : bus.mine) std::printf("      meu: size %u addr %08X data %08X\n", x.size, x.addr, x.data);
          for (int k = 0; k < 16; k++) std::printf("      r%d ini %08X fin %08X got %08X\n", k, t.ini.v[k], t.fin.v[k], k < 15 ? got[k] : 0); }
        std::printf("    op %08X em %08X (%s): campo %d: %08X esperado %08X; cpsr %08X/%08X pc %08X/%08X bus %d escritas %zu/%zu\n",
                    t.op, t.base, thumb ? "thumb" : "arm", firstBad,
                    firstBad >= 0 && firstBad < 15 ? got[firstBad] : firstBad > 15 && firstBad < 31 ? got[firstBad - 1] : 0,
                    firstBad >= 0 && firstBad < 40 ? t.fin.v[firstBad] : 0, keep, t.fin.v[31], pcView, t.fin.v[15], bus.bad ? 1 : 0,
                    bus.wri, bus.writes.size());
      }
    }
  }
}
}  // namespace sst

static void testCpuRuler(const std::string& dir) {
  std::printf("== CPU ARM7 (ARMv4T) x SingleStepTests ARM7TDMI\n");
  const std::string mine = dir + "/arm7tdmi/v1/", other = dir + "/../3do/arm7tdmi/v1/";
  static const char* FILES[] = {
    "arm_b_bl", "arm_bx", "arm_data_proc_immediate", "arm_data_proc_immediate_shift", "arm_data_proc_register_shift",
    "arm_ldm_stm", "arm_ldr_str_immediate_offset", "arm_ldr_str_register_offset", "arm_ldrh_strh", "arm_ldrsb_ldrsh",
    "arm_mrs", "arm_msr_imm", "arm_msr_reg", "arm_mul_mla", "arm_mull_mlal", "arm_swi", "arm_swp",
    "thumb_add_cmp_mov_hi", "thumb_add_sp_or_pc", "thumb_add_sub", "thumb_add_sub_sp", "thumb_b", "thumb_bcc",
    "thumb_bl_blx_prefix", "thumb_bl_suffix", "thumb_bx", "thumb_data_proc", "thumb_ldm_stm", "thumb_ldr_pc_rel",
    "thumb_ldr_str_imm_offset", "thumb_ldr_str_reg_offset", "thumb_ldr_str_sp_rel", "thumb_ldrb_strb_imm_offset",
    "thumb_ldrh_strh_imm_offset", "thumb_ldrh_strh_reg_offset", "thumb_ldrsb_strb_reg_offset",
    "thumb_ldrsh_ldrsb_reg_offset", "thumb_lsl_lsr_asr", "thumb_mov_cmp_add_sub", "thumb_push_pop", "thumb_swi"};
  int total = 0, totalBad = 0, files = 0;
  for (const char* f : FILES) {
    std::string path = mine + f + ".json.bin";
    if (!std::filesystem::exists(path)) path = other + f + ".json.bin";
    if (!std::filesystem::exists(path)) continue;
    files++;
    int ok = 0, bad = 0, skip = 0;
    const bool mul = std::strstr(f, "mul") || std::strcmp(f, "thumb_data_proc") == 0;
    sst::runFile(path, mul, std::strncmp(f, "arm_msr", 7) == 0, std::strncmp(f, "thumb", 5) == 0, ok, bad, skip);
    if (bad) std::printf("  %-32s %6d iguais, %d diferentes, %d pulados\n", f, ok, bad, skip);
    total += ok;
    totalBad += bad;
  }
  if (!files) { std::printf("  pulados (sem a régua em %s)\n", mine.c_str()); skipped++; return; }
  CHECK(totalBad == 0, "CPU: %d casos diferentes da régua", totalBad);
  std::printf("  %d arquivos, %d casos iguais, %d diferentes\n", files, total, totalBad);
}

// ================================================================================================ utilidades
static uint32_t rd32(NDS& s, uint32_t a) { return s.bus9.read32(a); }
static uint32_t px(const uint32_t* fb, int x, int y) { return fb[y * 256 + x] & 0xFFFFFF; }
static uint32_t rgb(int r5, int g5, int b5) {
  auto e = [](int c) { const int c6 = c << 1; return uint32_t((c6 << 2) | (c6 >> 4)); };
  return e(r5) | (e(g5) << 8) | (e(b5) << 16);
}
static uint64_t hashFb(const uint32_t* fb, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) { h ^= fb[i]; h *= 1099511628211ull; }
  return h;
}

// ================================================================================================ ARMv5TE
static void testCpu5(const std::string& dir) {
  std::printf("== CPU ARM9 (ARMv5TE): instruções novas\n");
  const auto rom = readAll(dir + "/cpu5.nds");
  if (rom.empty()) { std::printf("  pulados (sem cpu5.nds)\n"); skipped++; return; }
  NDSCore c(rom, nullptr);
  for (int i = 0; i < 3; i++) c.runFrame();
  NDS& s = c.nds();
  const uint32_t R = 0x02300000;
  CHECK(rd32(s, R + 0xFC) == 0xC0DE5555, "cpu5 não chegou ao fim (%08X; PC %08X, T %d, indefinidas %u)", rd32(s, R + 0xFC), s.arm9.nextPC, s.arm9.t, s.arm9.undefCount);
  struct E { int off; uint32_t v; const char* n; } E5[] = {
    {0, 8, "CLZ"}, {4, 32, "CLZ(0)"}, {8, 0x7FFFFFFF, "QADD satura"}, {12, 0x08000000, "QADD liga Q"},
    {16, 0x80000000, "QSUB satura"}, {20, 0x7FFFFFFF, "QDADD"}, {24, 86, "SMLABB"}, {28, 0xFFFFFFFA, "SMULTT"},
    {32, 6, "SMULWB"}, {36, 0xFFFFFFF6, "SMLALBB baixo"}, {40, 0xFFFFFFFF, "SMLALBB alto"},
    {44, 0x11223344, "LDRD 1"}, {48, 0x55667788, "LDRD 2"}, {52, 0x55, "BLX endereço"}, {56, 0x66, "BLX registrador"},
    {60, 0x77, "LDR PC para Thumb"}, {64, 0x99, "POP {pc} para ARM"}, {68, 0x41059461, "MRC p15 c0"},
    {72, 0x118, "LDM base na lista (escreve)"}, {76, 0xBBBB0000, "LDM base por último (não escreve)"}};
  for (const auto& e : E5) CHECK(rd32(s, R + e.off) == e.v, "%s: %08X, esperado %08X", e.n, rd32(s, R + e.off), e.v);
  CHECK(s.arm9.undefCount == 0, "instruções indefinidas no ARM9: %u", s.arm9.undefCount);
}

// ================================================================================================ homebrew
static void testHomebrew(const std::string& dir) {
  std::printf("== homebrew de teste (ARM9 + ARM7)\n");
  const auto rom = readAll(dir + "/teste.nds");
  if (rom.empty()) { std::printf("  pulados (sem teste.nds)\n"); skipped++; return; }
  auto core = createCore(rom, "teste.nds", nullptr);
  CHECK(core && core->system() == "nds", "registro não reconheceu o .nds");
  if (!core) return;
  auto* nc = dynamic_cast<NDSCore*>(core.get());
  NDS& s = nc->nds();
  CHECK(core->width() == 256 && core->height() == 384, "imagem %dx%d", core->width(), core->height());
  CHECK(core->title() == "INDIGO TESTE", "título '%s'", core->title().c_str());
  core->setSampleRate(48000);
  core->setPointer(100, 192 + 50, true);  // toque na tela de baixo
  int frames = 0;
  for (; frames < 120 && rd32(s, 0x02300000 + 255 * 4) != 0xD0D0D0D0; frames++) {
    core->runFrame();
    std::vector<float> l(4096), r(4096);
    while (core->audioAvailable() > 4096) core->pullAudio(l.data(), r.data(), 4096);
  }
  for (int i = 0; i < 3; i++) core->runFrame();
  auto R = [&](int i) { return rd32(s, 0x02300000 + i * 4); };
  CHECK(R(0) == 0x4E445339, "ARM9 não começou (%08X); PC9 %08X PC7 %08X", R(0), s.arm9.nextPC, s.arm7.nextPC);
  CHECK(R(255) == 0xD0D0D0D0, "ARM9 não terminou em %d quadros (PC9 %08X, PC7 %08X)", frames, s.arm9.nextPC, s.arm7.nextPC);
  CHECK(rd32(s, 0x02300400) == 0x4E445337, "ARM7 não começou (PC7 %08X)", s.arm7.nextPC);
  // divisor e raiz
  CHECK(R(1) == uint32_t(-142) && R(2) == uint32_t(-6), "divisor 32: %d resto %d", int(R(1)), int(R(2)));
  { const uint64_t n = 0x123456789ABull; const uint64_t q = n / 1000;
    CHECK(R(3) == uint32_t(q) && R(4) == uint32_t(q >> 32) && R(5) == uint32_t(n % 1000), "divisor 64/32: %08X%08X resto %u", R(4), R(3), R(5)); }
  CHECK(R(6) == 0x7FFFFFFF, "divisor 64/64: %08X", R(6));
  CHECK(R(7) == 0x4000, "divisão por zero não marcou o bit 14 (%X)", R(7));
  CHECK(R(8) == 1000 && R(9) == 0x100000, "raiz: %u, %X", R(8), R(9));
  // SWI
  CHECK(R(10) == 14 && R(11) == uint32_t(-2) && R(12) == 12, "SWI Div/Sqrt: %u %d %u", R(10), int(R(11)), R(12));
  CHECK(R(13) != 0 && R(13) != 0xFFFF, "SWI CRC16: %X", R(13));
  CHECK(R(14) == 1, "SWI CpuSet (preencher 32 bits): %08X", R(14));
  CHECK(R(15) == 1, "SWI CpuFastSet");
  CHECK(R(16) == 1, "SWI LZ77");
  CHECK(R(17) == 1, "SWI RLE");
  CHECK(R(64) == 1, "SWI Huffman por callback");
  CHECK(R(18) == 1, "SWI LZ77 por callback (16 bits na VRAM)");
  CHECK(rd32(s, 0x02300000 + 63 * 4) == 0x1234, "callback de abrir não recebeu o parâmetro (%X)", R(63));
  CHECK(R(62) != 0, "callback de fechar não foi chamado");
  { uint32_t e0 = 0, e1 = 0; const uint8_t src[2] = {0xA5, 0x0F};
    for (int i = 0; i < 16; i++) { const uint32_t bit = (src[i / 8] >> (i % 8)) & 1; const uint32_t v = bit ? bit + 1 : 0; if (i < 8) e0 |= v << (i * 4); else e1 |= v << ((i - 8) * 4); }
    CHECK(R(19) == e0 && R(20) == e1, "SWI BitUnPack: %08X %08X (esperado %08X %08X)", R(19), R(20), e0, e1); }
  CHECK(R(21) > 0, "timer em cascata não contou");
  CHECK(R(22) == 1 && R(23) == 1, "DMA: cópia %u, preenchimento %u", R(22), R(23));
  CHECK(R(24) == 7, "IPCSYNC: %u", R(24));
  CHECK(R(25) == 41, "IPC FIFO eco: %u", R(25));
  CHECK(R(26) == 3, "VBlankIntrWait: %u IRQs em 3 esperas", R(26));
  CHECK(R(27) >= 1, "IRQ de IPCSYNC não chegou");
  // cartucho
  auto romWord = [&](uint32_t a) { return uint32_t(rom[a]) | rom[a + 1] << 8 | rom[a + 2] << 16 | uint32_t(rom[a + 3]) << 24; };
  CHECK(R(28) == romWord(0x20000) && R(29) == romWord(0x201FC), "leitura do cartucho por E/S: %08X %08X", R(28), R(29));
  CHECK(R(30) == romWord(0x20200) && R(31) == romWord(0x203FC), "leitura do cartucho por DMA: %08X %08X", R(30), R(31));
  CHECK(R(32) == s.cart.chipId && R(33) == s.cart.chipId, "ID do chip: %08X / %08X", R(32), R(33));
  // save
  CHECK(R(34) == 0xFF, "save novo não veio apagado: %X", R(34));
  CHECK(R(35) == 0x45564153, "save: lido %08X", R(35));
  CHECK(s.cart.addrBytes == 2, "tipo de save detectado: %d bytes de endereço", s.cart.addrBytes);
  {
    const auto sv = core->exportSave();
    CHECK(sv.size() == 0x10000 && sv[0x10] == 'S' && sv[0x13] == 'E', "save exportado (%zu bytes)", sv.size());
  }
  // toque (ADC = pixel * 16 + 8 pela calibração do firmware gerado) e relógio
  CHECK((R(36) >> 31) == 1, "caneta não apareceu como abaixada");
  CHECK(((R(36) >> 16) & 0xFFF) == 100 * 16 + 8 && (R(36) & 0xFFF) == 50 * 16 + 8, "toque: x %u y %u", (R(36) >> 16) & 0xFFF, R(36) & 0xFFF);
  { const uint32_t m = (R(37) >> 16) & 0xFF, d = (R(37) >> 8) & 0xFF, h = R(37) & 0x3F;
    CHECK(m >= 1 && m <= 0x12 && (m & 0xF) <= 9 && d >= 1 && d <= 0x31 && h <= 0x23, "RTC em BCD: %08X", R(37)); }
  CHECK(R(38) == ('I' | ('n' << 8)) || R(38) == 'I', "firmware: nome do usuário começa com %04X", R(38));
  CHECK((R(39) & 0xFF) == 127 && ((R(39) >> 8) & 0xFF) == 0, "SWI GetVolumeTable: %08X", R(39));
  // som
  { std::vector<float> l(2048), r(2048);
    core->runFrame();
    const size_t n = std::min<size_t>(core->audioAvailable(), 2048);
    core->pullAudio(l.data(), r.data(), n);
    float peak = 0;
    for (size_t i = 0; i < n; i++) peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    CHECK(n > 700 && peak > 0.01f, "som: %zu amostras, pico %.3f", n, peak);
    CHECK(s.spu.ch[0].on, "canal de som 0 parou"); }
  // vídeo: tela de cima (engine A com 3D) e de baixo (engine B)
  const uint32_t* fb = core->framebuffer();
  CHECK(px(fb, 10, 180) == rgb(31, 0, 0), "3D: triângulo vermelho (%06X)", px(fb, 10, 180));
  CHECK(px(fb, 192, 96) == rgb(0, 31, 0), "3D: quadrilátero com textura verde (%06X)", px(fb, 192, 96));
  CHECK(px(fb, 200, 8) == rgb(31, 31, 0), "BG de texto atrás do 3D (%06X)", px(fb, 200, 8));
  CHECK(px(fb, 240, 100) == 0, "fundo (%06X)", px(fb, 240, 100));
  CHECK(px(fb, 45, 192 + 35) == rgb(31, 31, 31), "sprite branco na engine B (%06X)", px(fb, 45, 192 + 35));
  CHECK(px(fb, 100, 192 + 100) == rgb(100 >> 3, 100 >> 3, 0), "bitmap de cor direta (%06X)", px(fb, 100, 192 + 100));
  CHECK(px(fb, 250, 192 + 180) == rgb(250 >> 3, 180 >> 3, 0), "bitmap de cor direta, canto (%06X)", px(fb, 250, 192 + 180));
  CHECK(s.gpu3d.rendered >= 1, "3D não desenhou");
  CHECK(s.arm9.undefCount == 0 && s.arm7.undefCount == 0, "instruções indefinidas: ARM9 %u, ARM7 %u", s.arm9.undefCount, s.arm7.undefCount);

  // cena 2: efeitos 2D (botão A)
  core->setKeys(KEY_A);
  for (int i = 0; i < 30 && R(254) != 0xD0D0D002; i++) core->runFrame();
  core->setKeys(0);
  core->runFrame();
  CHECK(R(254) == 0xD0D0D002, "cena 2 não começou (botão A)");
  auto rgb6 = [](int r6, int g6, int b6) { return uint32_t((r6 << 2) | (r6 >> 4)) | (uint32_t((g6 << 2) | (g6 >> 4)) << 8) | (uint32_t((b6 << 2) | (b6 >> 4)) << 16); };
  CHECK(px(fb, 100, 80) == rgb(15, 0, 15), "alfa entre BGs dentro da janela (%06X)", px(fb, 100, 80));
  CHECK(px(fb, 200, 150) == rgb(31, 0, 0), "fora da janela só a BG0 (%06X)", px(fb, 200, 150));
  CHECK(px(fb, 47, 47) == rgb(0, 31, 0) && px(fb, 41, 41) == rgb(0, 31, 0), "sprite afim 2x com paleta estendida (%06X)", px(fb, 47, 47));
  CHECK(px(fb, 38, 47) != rgb(0, 31, 0) && px(fb, 57, 47) != rgb(0, 31, 0), "sprite afim do tamanho certo (%06X %06X)", px(fb, 38, 47), px(fb, 57, 47));
  CHECK(px(fb, 100, 192 + 100) == rgb6(12, 12, 0), "brilho mestre da engine B (%06X)", px(fb, 100, 192 + 100));
  CHECK(R(60) == (0x8000u | 15 | (15 << 10)) && R(61) == 0, "captura de tela: %04X, bit 31 %u", R(60), R(61));
  // cena 3: 3D com translucidez, toon e teste de alfa (botão B)
  core->setKeys(KEY_B);
  for (int i = 0; i < 30 && R(253) != 0xD0D0D003; i++) core->runFrame();
  core->setKeys(0);
  core->runFrame();
  CHECK(R(253) == 0xD0D0D003, "cena 3 não começou (botão B)");
  CHECK(px(fb, 32, 96) == rgb(31, 0, 0), "3D opaco (%06X)", px(fb, 32, 96));
  CHECK(px(fb, 96, 96) == rgb(15, 0, 15), "3D translúcido sobre vermelho (%06X)", px(fb, 96, 96));
  CHECK(px(fb, 160, 96) == rgb(0, 0, 15), "3D translúcido sobre o fundo (%06X)", px(fb, 160, 96));
  CHECK(px(fb, 230, 170) == rgb(31, 31, 0), "toon (%06X)", px(fb, 230, 170));
  CHECK(px(fb, 230, 30) == 0, "teste de alfa descartou (%06X)", px(fb, 230, 30));

  // save state: mesmos quadros depois de carregar
  const auto st = core->saveState();
  for (int i = 0; i < 10; i++) core->runFrame();
  const uint64_t h1 = hashFb(core->framebuffer(), 256 * 384);
  const uint32_t pc1 = s.arm9.nextPC;
  core->loadState(st);
  for (int i = 0; i < 10; i++) core->runFrame();
  CHECK(hashFb(core->framebuffer(), 256 * 384) == h1 && s.arm9.nextPC == pc1, "estado carregado não repete os quadros");
  // estados adulterados: recusados sem derrubar
  int refused = 0;
  { auto bad = st; bad.resize(bad.size() / 2); try { core->loadState(bad); } catch (const std::exception&) { refused++; } }
  { auto bad = st; bad[0] ^= 0xFF; try { core->loadState(bad); } catch (const std::exception&) { refused++; } }
  { auto bad = st; bad.push_back(1); try { core->loadState(bad); } catch (const std::exception&) { refused++; } }
  CHECK(refused == 3, "estados adulterados recusados: %d de 3", refused);
  // bytes aleatórios no meio: não pode derrubar (pode carregar algo estranho, mas seguro)
  uint32_t seed = 12345;
  for (int k = 0; k < 20; k++) {
    auto bad = st;
    for (int j = 0; j < 64; j++) { seed = seed * 1103515245 + 12345; bad[8 + (seed >> 8) % (bad.size() - 8)] ^= uint8_t(seed >> 3); }
    try { core->loadState(bad); for (int i = 0; i < 2; i++) core->runFrame(); } catch (const std::exception&) {}
  }
  core->loadState(st);
  core->runFrame();
  // save do cartucho importado volta no próximo começo
  {
    auto sv = core->exportSave();
    auto c2 = createCore(rom, "teste.nds", &sv);
    auto* n2 = dynamic_cast<NDSCore*>(c2.get());
    CHECK(n2 && n2->nds().cart.save.size() == 0x10000 && n2->nds().cart.save[0x10] == 'S', "save importado");
  }
  // velocidade
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 300; i++) core->runFrame();
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  velocidade: %.0f quadros/s (%.1fx) no homebrew de teste (3D + 2D, CPUs esperando VBlank)\n", 300 / sec, 300 / sec / 59.83);
}

// velocidade com as duas CPUs ocupadas o tempo todo (pior caso do interpretador)
static void testSpeedBusy(const std::string& dir) {
  const auto rom = readAll(dir + "/cpu5.nds");
  if (rom.empty()) return;
  NDSCore c(rom, nullptr);
  // ARM9 num laço sem fim (fim: b fim) e ARM7 esperando a FIFO: o ARM9 roda todos os ciclos
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 120; i++) c.runFrame();
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  velocidade com as duas CPUs ocupadas: %.0f quadros/s (%.1fx)\n", 120 / sec, 120 / sec / 59.83);
}

// SWI de descompressão por callback com uma rotina do jogo que nunca volta ("b ."): a SWI desiste
// sem prender o quadro (antes: 2M passos por byte × até 16M bytes)
static void testCallbackTravado(const std::string& dir) {
  std::printf("== SWI por callback com rotina travada\n");
  const auto rom = readAll(dir + "/cpu5.nds");
  if (rom.empty()) { std::printf("  pulados (sem cpu5.nds)\n"); skipped++; return; }
  NDSCore c(rom, nullptr);
  c.runFrame();
  NDS& s = c.nds();
  const uint32_t code = 0x02380100, tab = 0x02380200;
  s.bus9.write32(code + 0, 0xE59F0000);   // ldr r0, [pc]  (cabeçalho de 16 MB)
  s.bus9.write32(code + 4, 0xE12FFF1E);   // bx lr
  s.bus9.write32(code + 16, 0xEAFFFFFE);  // b .
  s.bus9.write32(tab + 0, code); s.bus9.write32(tab + 4, 0); s.bus9.write32(tab + 8, code + 16);
  s.bus9.write32(tab + 12, 0); s.bus9.write32(tab + 16, code + 16);
  for (int n : {0x12, 0x15, 0x13}) {
    s.bus9.write32(code + 8, n == 0x12 ? 0xFFFFFF10 : n == 0x15 ? 0xFFFFFF30 : 0xFFFFFF28);
    uint32_t saved[16];
    std::memcpy(saved, s.arm9.r, sizeof saved);
    s.arm9.r[0] = 0x02200000; s.arm9.r[1] = 0x02390000; s.arm9.r[2] = 0; s.arm9.r[3] = tab;
    const auto t0 = std::chrono::steady_clock::now();
    s.hleSwi(0, n, 0);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::memcpy(s.arm9.r, saved, sizeof saved);
    CHECK(s.guestStuck(), "SWI %02X não percebeu a rotina travada", n);
    CHECK(ms < 2000, "SWI %02X demorou %.0f ms com a rotina travada", n, ms);
    std::printf("  SWI %02X desistiu em %.0f ms\n", n, ms);
  }
  c.runFrame();  // o núcleo segue rodando depois
  CHECK(true, "quadro depois da SWI travada");
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/nds";
  testCpuRuler(dir);
  testCpu5(dir);
  testHomebrew(dir);
  testSpeedBusy(dir);
  testCallbackTravado(dir);
  std::printf("RESULTADO nds: %d verificações, %d falhas%s\n", checks, fails, skipped ? " (algumas partes puladas)" : "");
  return fails ? 1 : 0;
}
