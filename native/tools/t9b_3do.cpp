// Testes do 3DO (etapa 9b): CPU ARM60, MADAM (cels e matrizes), CLIO (timers, interrupções, DMA), DSP, unidade
// de CD com disco sintético, save state e, se existirem, a BIOS de teste do Índigo com o disco homebrew próprio
// (t9b_3do.sh) e a BIOS do usuário.
//   indigo-t9b_3do pasta   (pasta = /root/indigo-env/roms/3do, montada pelo t9b_3do.sh)
// Régua da CPU: SingleStepTests/ARM7TDMI (MIT, gerados com o NanoBoyAdvance) só nas instruções que o ARMv3 também
// tem (sem Thumb, meia palavra, BX e multiplicação longa), em pasta/arm7tdmi/v1.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/tdo/tdo.h"

using namespace indigo;
using namespace indigo::tdo;

static int fails = 0, checks = 0, skipped = 0;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    checks++;                                               \
    if (!(cond)) {                                          \
      fails++;                                              \
      std::printf("  FALHA: " __VA_ARGS__);                 \
      std::printf("\n");                                    \
    }                                                       \
  } while (0)

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ================================================================================================ CPU: régua

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
  std::vector<Tr> reads, writes, mine;
  void start(const Test& x) {
    t = &x; rdi = wri = 0; fetched = false; bad = false;
    reads.clear(); writes.clear(); mine.clear();
    for (const auto& r : x.tr) { if (r.kind == 1) reads.push_back(r); else if (r.kind == 2) writes.push_back(r); }
  }
  uint32_t read32(uint32_t a) override {
    if (!fetched) { fetched = true; return t->op; }
    if (rdi < reads.size()) return reads[rdi++].data;
    bad = true;
    return 0;
  }
  uint8_t read8(uint32_t a) override { return uint8_t(read32(a)); }
  void write32(uint32_t a, uint32_t v) override {
    mine.push_back({2, 4, a, v});
    if (wri >= writes.size() || (writes[wri].addr & ~3u) != a || writes[wri].data != v || writes[wri].size != 4) bad = true;
    wri++;
  }
  void write8(uint32_t a, uint8_t v) override {
    if (wri >= writes.size() || writes[wri].addr != a || (writes[wri].data & 0xFF) != v || writes[wri].size != 1) bad = true;
    wri++;
  }
};

bool validMode(uint32_t m) { return m == 0x10 || m == 0x11 || m == 0x12 || m == 0x13 || m == 0x17 || m == 0x1B || m == 0x1F; }
// índice do SPSR no arquivo: fiq, svc, abt, irq, und
int spsrIdx(uint32_t m) { switch (m) { case 0x11: return 0; case 0x13: return 1; case 0x17: return 2; case 0x12: return 3; case 0x1B: return 4; default: return -1; } }
// banco de r13/r14 no arquivo
int bankBase(uint32_t m) { switch (m) { case 0x11: return 21; case 0x13: return 23; case 0x17: return 25; case 0x12: return 27; case 0x1B: return 29; default: return -1; } }

void runFile(const std::string& path, bool mul, int& ok, int& bad, int& skip) {
  const auto b = readAll(path);
  if (b.size() < 8 || rd32(b, 0) != 0xD33DBAE0) { std::printf("  arquivo inválido: %s\n", path.c_str()); bad++; return; }
  const uint32_t n = rd32(b, 4);
  size_t p = 8;
  Bus bus;
  Arm60 cpu(bus);
  int shown = 0;
  for (uint32_t i = 0; i < n && p < b.size(); i++) {
    Test t;
    const uint32_t full = rd32(b, p);
    size_t q = p + 4;
    auto state = [&](State& s) { const uint32_t sz = rd32(b, q); for (int k = 0; k < 40; k++) s.v[k] = rd32(b, q + 8 + 4 * k); q += sz; };
    state(t.ini);
    state(t.fin);
    { const uint32_t sz = rd32(b, q), nt = rd32(b, q + 8);
      for (uint32_t k = 0; k < nt; k++) { const size_t o = q + 12 + 24 * k; t.tr.push_back({rd32(b, o), rd32(b, o + 4), rd32(b, o + 8), rd32(b, o + 12)}); }
      q += sz; }
    t.op = rd32(b, q + 8);
    t.base = rd32(b, q + 12);
    p += full;

    const uint32_t im = t.ini.v[31] & 0x1F, fm = t.fin.v[31] & 0x1F;
    // MRS/MSR/LDR com destino PC e outras formas imprevisíveis do manual ficam de fora
    const bool mrsPc = (t.op & 0x0FBF0FFF) == 0x010F0000 && ((t.op >> 12) & 0xF) == 15;
    const uint32_t op = t.op, cls = (op >> 25) & 7;
    const int rn = (op >> 16) & 0xF, rdd = (op >> 12) & 0xF, rs = (op >> 8) & 0xF, rm = op & 0xF;
    const bool isMul = (op & 0x0FC000F0) == 0x00000090, isSwp = (op & 0x0FB00FF0) == 0x01000090;
    const bool unpred = mrsPc ||
        ((cls == 2 || cls == 3) && rn == 15 && ((op & (1u << 21)) || !(op & (1u << 24)))) ||  // PC base com escrita de volta
        (cls == 3 && rm == 15) ||
        (cls < 2 && !(op & (1u << 25)) && (op & 0x10) && rs == 15) ||                       // deslocamento pelo PC                                                              // PC como deslocamento
        (cls == 4 && rn == 15) ||
        (cls == 4 && (op & (1u << 22)) && (op & (1u << 21)) && !((op & (1u << 20)) && (op & 0x8000))) ||  // ^ com escrita de volta                                                              // LDM/STM com base PC
        (isMul && (rn == 15 || rdd == 15 || rs == 15 || rm == 15 || ((op >> 16) & 0xF) == 15)) ||
        (isSwp && (rn == 15 || rdd == 15 || rm == 15)) ||
        ((op & 0x0FB0FFF0) == 0x0120F000 && rm == 15);                                      // MSR com Rm = PC
    if (!validMode(im) || !validMode(fm) || (t.ini.v[31] & 0x20) || (t.fin.v[31] & 0x20) || unpred) { skip++; continue; }
    // no arquivo, R = banco do usuário; R_fiq (r8-r14) e R_svc/abt/irq/und (r13-r14) = bancos dos modos
    auto load = [&](const State& s) {
      cpu.reset(0);
      cpu.cpsr = 0x10;
      for (int k = 0; k < 15; k++) cpu.r[k] = s.v[k];
      for (uint32_t m : {0x11u, 0x12u, 0x13u, 0x17u, 0x1Bu}) {
        cpu.setMode(m);
        const int bb = bankBase(m);
        cpu.r[13] = s.v[bb]; cpu.r[14] = s.v[bb + 1];
        if (m == 0x11) for (int k = 0; k < 5; k++) cpu.r[8 + k] = s.v[16 + k];
        cpu.spsrRef() = s.v[32 + spsrIdx(m)] & 0xF00000FFu;
      }
      cpu.setMode(s.v[31] & 0x1F);
      cpu.cpsr = s.v[31] & 0xF00000FFu;
    };
    auto banks = [&](uint32_t out[31], uint32_t spsrs[5]) {
      const uint32_t keep = cpu.cpsr;
      cpu.setMode(0x10);
      for (int k = 0; k < 15; k++) out[k] = cpu.r[k];
      for (uint32_t m : {0x11u, 0x12u, 0x13u, 0x17u, 0x1Bu}) {
        cpu.setMode(m);
        const int bb = bankBase(m);
        out[bb - 1] = cpu.r[13]; out[bb] = cpu.r[14];
        if (m == 0x11) for (int k = 0; k < 5; k++) out[15 + k] = cpu.r[8 + k];
        spsrs[spsrIdx(m)] = cpu.spsr();
      }
      cpu.setMode(keep & 0x1F);
      cpu.cpsr = keep;
    };
    load(t.ini);
    cpu.r[15] = t.base;
    bus.start(t);
    cpu.step();
    uint32_t got[31], sp[5];
    banks(got, sp);
    bool same = !bus.bad && bus.wri == bus.writes.size();
    int firstBad = -1;
    for (int k = 0; k < 15 && same; k++) if (got[k] != t.fin.v[k]) { same = false; firstBad = k; }
    for (int k = 16; k < 31 && same; k++) if (got[k - 1] != t.fin.v[k]) { same = false; firstBad = k; }
    if (same && ((cpu.r[15] + 8) & ~3u) != (t.fin.v[15] & ~3u)) same = false;
    const uint32_t cmask = mul ? 0xD00000FFu : 0xF00000FFu;  // MUL: o C do ARM7 é lixo da multiplicação
    if (same && (cpu.cpsr & cmask) != (t.fin.v[31] & cmask)) same = false;
    for (int k = 0; k < 5 && same; k++) if ((sp[k] & 0xF00000FFu) != (t.fin.v[32 + k] & 0xF00000FFu)) same = false;
    if (same) ok++;
    else {
      bad++;
      if (shown++ < 3) {
        std::printf("    op %08X em %08X: modo %02X->%02X cpsr %08X (esperado %08X) pc %08X (esperado %08X) bus %d\n", t.op, t.base,
                    im, fm, cpu.cpsr, t.fin.v[31], cpu.r[15] + 8, t.fin.v[15], bus.bad ? 1 : 0);
        std::printf("      campo %d: %08X esperado %08X (antes %08X); escritas %zu/%zu\n", firstBad, firstBad < 0 ? 0 : got[firstBad < 15 ? firstBad : firstBad - 1],
                    firstBad < 0 ? 0 : t.fin.v[firstBad], firstBad < 0 ? 0 : t.ini.v[firstBad], bus.wri, bus.writes.size());
        for (const auto& x : bus.mine) std::printf("        meu: addr %08X data %08X\n", x.addr, x.data);
        std::printf("        r%d antes %08X\n", rn, t.ini.v[rn]);
        for (const auto& x : t.tr) std::printf("        kind %u size %u addr %08X data %08X\n", x.kind, x.size, x.addr, x.data);
      }
    }
  }
}
}  // namespace sst

static void testCpuRuler(const std::string& dir) {
  std::printf("== CPU ARM60 x SingleStepTests ARM7TDMI (instruções do ARMv3)\n");
  const std::string d = dir + "/arm7tdmi/v1/";
  if (!std::filesystem::exists(d + "arm_b_bl.json.bin")) { std::printf("  pulados (sem %s)\n", d.c_str()); skipped++; return; }
  static const char* FILES[] = {"arm_b_bl", "arm_data_proc_immediate", "arm_data_proc_immediate_shift",
                                "arm_data_proc_register_shift", "arm_ldm_stm", "arm_ldr_str_immediate_offset",
                                "arm_ldr_str_register_offset", "arm_mrs", "arm_msr_imm", "arm_msr_reg", "arm_mul_mla",
                                "arm_swi", "arm_swp"};
  int total = 0, totalBad = 0;
  for (const char* f : FILES) {
    int ok = 0, bad = 0, skip = 0;
    sst::runFile(d + f + ".json.bin", std::strcmp(f, "arm_mul_mla") == 0, ok, bad, skip);
    std::printf("  %-32s %6d iguais, %d diferentes, %d pulados (formas imprevisíveis no manual)\n", f, ok, bad, skip);
    total += ok;
    totalBad += bad;
  }
  CHECK(totalBad == 0, "CPU: %d casos diferentes da régua", totalBad);
  std::printf("  total: %d casos iguais\n", total);
}

// ================================================================================================ utilidades

namespace {
// BIOS mínima montada aqui mesmo (palavras do ARM em big-endian a partir de 0x03000000)
std::vector<uint8_t> biosFrom(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> b(0x100000, 0);
  for (size_t i = 0; i < words.size(); i++) {
    b[i * 4] = uint8_t(words[i] >> 24); b[i * 4 + 1] = uint8_t(words[i] >> 16);
    b[i * 4 + 2] = uint8_t(words[i] >> 8); b[i * 4 + 3] = uint8_t(words[i]);
  }
  const char tag[] = "Indigo 3DO test";
  std::memcpy(&b[0x3F000], tag, sizeof tag);
  return b;
}
uint32_t branch(uint32_t from, uint32_t to, bool link = false) {
  return (link ? 0xEB000000u : 0xEA000000u) | (((to - (from + 8)) >> 2) & 0xFFFFFF);
}
const std::vector<uint8_t>& idleBios() {
  static const std::vector<uint8_t> b = biosFrom({0xEAFFFFFE});  // B .
  return b;
}

// disco Opera sintético na memória: rótulo, diretório raiz com LaunchMe e DADOS.BIN, e uma faixa de áudio
class OperaDisc : public cdrom::Disc {
public:
  explicit OperaDisc(uint32_t dataSectors = 64, uint32_t audioSectors = 150) : sectors_(dataSectors) {
    data_.assign(size_t(dataSectors) * 2048, 0);
    for (size_t i = 0; i < data_.size(); i++) data_[i] = uint8_t(i * 7 + (i >> 11) * 13);
    uint8_t* l = &data_[0];
    std::memset(l, 0, 2048);
    l[0] = 1; std::memset(l + 1, 0x5A, 5); l[6] = 1;
    std::memcpy(l + 8, "INDIGO TESTE", 12);
    std::memcpy(l + 40, "CD-ROM", 6);
    put(l, 76, 2048); put(l, 80, dataSectors); put(l, 88, 1); put(l, 92, 2048); put(l, 96, 0); put(l, 100, 1);
    uint8_t* d = &data_[2048];
    std::memset(d, 0, 2048);
    put(d, 0, 0xFFFFFFFF); put(d, 4, 0xFFFFFFFF); put(d, 12, 20 + 72 * 2); put(d, 16, 20);
    entry(d + 20, "LaunchMe", 2, 300, 0);
    entry(d + 20 + 72, "DADOS.BIN", 3, 4096, 0xC0000000);
    tracks_.push_back({1, cdrom::TrackType::Mode1, 0, 0, dataSectors});
    if (audioSectors) tracks_.push_back({2, cdrom::TrackType::Audio, dataSectors + 150, 150, audioSectors});
    total_ = dataSectors + (audioSectors ? 150 + audioSectors : 0);
    name_ = "opera";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, cdrom::RAW_SECTOR);
    if (lba >= total_) return false;
    if (lba < sectors_) {
      std::memset(out + 1, 0xFF, 10);
      const uint32_t a = lba + 150;
      auto bcd = [](uint32_t v) { return uint8_t(((v / 10) << 4) | (v % 10)); };
      out[12] = bcd(a / 4500); out[13] = bcd(a / 75 % 60); out[14] = bcd(a % 75); out[15] = 1;
      std::memcpy(out + 16, &data_[size_t(lba) * 2048], 2048);
    } else {
      for (int i = 0; i < cdrom::RAW_SECTOR; i++) out[i] = uint8_t(i ^ lba);
    }
    return true;
  }
  const std::vector<uint8_t>& image() const { return data_; }
private:
  static void put(uint8_t* p, int o, uint32_t v) { p[o] = uint8_t(v >> 24); p[o + 1] = uint8_t(v >> 16); p[o + 2] = uint8_t(v >> 8); p[o + 3] = uint8_t(v); }
  static void entry(uint8_t* e, const char* name, uint32_t block, uint32_t bytes, uint32_t flags) {
    put(e, 0, flags | 2); put(e, 8, 0x2A66696C); put(e, 12, 2048); put(e, 16, bytes); put(e, 20, (bytes + 2047) / 2048);
    std::strncpy(reinterpret_cast<char*>(e + 32), name, 32);
    put(e, 64, 0); put(e, 68, block);
  }
  uint32_t sectors_;
  std::vector<uint8_t> data_;
};

uint16_t pix(System& s, int x, int y, uint32_t base = 0x200000) {
  return s.madam.m16(base + uint32_t(y >> 1) * 1280 + uint32_t(x) * 4 + uint32_t(y & 1) * 2);
}
void fillVram(System& s, uint16_t v) {
  for (uint32_t a = 0x200000; a < 0x300000; a += 2) s.madam.w16(a, v);
}
void w32(System& s, uint32_t a, uint32_t v) { s.write32(a, v); }
uint32_t hashFb(const uint32_t* fb, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= fb[i]; h *= 16777619u; }
  return h;
}

namespace F {
constexpr uint32_t SKIP = 0x80000000, LAST = 0x40000000, NPABS = 0x20000000, SPABS = 0x10000000, PPABS = 0x08000000,
                   LDSIZE = 0x04000000, LDPRS = 0x02000000, LDPPMP = 0x01000000, LDPLUT = 0x00800000,
                   CCBPRE = 0x00400000, YOXY = 0x00200000, ACW = 0x00040000, ACCW = 0x00020000, PACKED = 0x00000200,
                   BGND = 0x00000020;
constexpr uint32_t BASE = SPABS | NPABS | PPABS | LDSIZE | LDPRS | LDPPMP | YOXY | ACW | ACCW | CCBPRE;
}
struct Cel {
  uint32_t flags = F::BASE | F::LAST, next = 0, src = 0, plut = 0;
  int32_t x = 0, y = 0, hdx = 1 << 20, hdy = 0, vdx = 0, vdy = 1 << 16, hddx = 0, hddy = 0;
  uint32_t pixc = 0x1F001F00, pre0 = 0, pre1 = 0;
};
void putCel(System& s, uint32_t a, const Cel& c) {
  const uint32_t w[] = {c.flags, c.next, c.src, c.plut, uint32_t(c.x), uint32_t(c.y), uint32_t(c.hdx), uint32_t(c.hdy),
                        uint32_t(c.vdx), uint32_t(c.vdy), uint32_t(c.hddx), uint32_t(c.hddy), c.pixc, c.pre0, c.pre1};
  for (int i = 0; i < 15; i++) w32(s, a + uint32_t(i) * 4, w[i]);
}
void celSetup(System& s) {
  w32(s, 0x03300130, 320);
  w32(s, 0x03300134, (239u << 16) | 319);
  w32(s, 0x03300138, 0x200000);
  w32(s, 0x0330013C, 0x200000);
}
void draw(System& s, uint32_t ccb) { w32(s, 0x033005A0, ccb); w32(s, 0x03300100, 1); }
// PRE0/PRE1 de uma cel não compactada
uint32_t pre0(int bppCode, bool linear, int h) { return uint32_t(bppCode) | (linear ? 0x10 : 0) | uint32_t(h - 1) << 6; }
uint32_t pre1(int w, uint32_t woff) { return uint32_t(w - 1) | woff << 16; }
}  // namespace

// ================================================================================================ CPU: casos dirigidos

static void testCpuDirected() {
  std::printf("== CPU ARM60: casos dirigidos do ARMv3 e do 3DO (big-endian, exceções, FIQ)\n");
  // meia palavra (LDRH) e multiplicação longa (UMULL) não existem no ARMv3: instrução indefinida
  {
    System s(biosFrom({0xE1D000B0, 0xEAFFFFFE}), nullptr);
    s.cpu.step();
    CHECK((s.cpu.cpsr & 0x1F) == 0x1B && s.cpu.r[15] == 4 && s.cpu.r[14] == 0x03000004, "LDRH: indefinida (modo %02X pc %08X lr %08X)", s.cpu.cpsr & 0x1F, s.cpu.r[15], s.cpu.r[14]);
    System u(biosFrom({0xE0810392}), nullptr);
    u.cpu.step();
    CHECK((u.cpu.cpsr & 0x1F) == 0x1B && u.cpu.undefinedCount == 1, "UMULL: indefinida");
    System c(biosFrom({0xEE010F10}), nullptr);  // MCR p15: sem coprocessador
    c.cpu.step();
    CHECK((c.cpu.cpsr & 0x1F) == 0x1B, "MCR: indefinida");
  }
  // big-endian: STR de uma palavra e LDRB dos 4 bytes; LDR desalinhado gira a palavra
  {
    // r0 = 0x1000; r1 = [pool]; str r1,[r0]; ldrb r2,[r0]; ldrb r3,[r0,#3]; ldr r4,[r0,#1]; b .
    System s(biosFrom({0xE3A00A01, 0xE59F1014, 0xE5801000, 0xE5D02000, 0xE5D03003, 0xE5904001, 0xEAFFFFFE, 0, 0x11223344}), nullptr);
    for (int i = 0; i < 7; i++) s.cpu.step();
    CHECK(s.mem[0x1000] == 0x11 && s.mem[0x1003] == 0x44, "big-endian: bytes na memória %02X..%02X", s.mem[0x1000], s.mem[0x1003]);
    CHECK(s.cpu.r[2] == 0x11 && s.cpu.r[3] == 0x44, "LDRB big-endian: %X %X", s.cpu.r[2], s.cpu.r[3]);
    CHECK(s.cpu.r[4] == 0x44112233, "LDR desalinhado: %08X", s.cpu.r[4]);
  }
  // SWI: modo supervisor, vetor 0x08, LR = próxima instrução, SPSR = CPSR anterior
  {
    System s(biosFrom({0xE321F010, 0xEF000042}), nullptr);  // MSR CPSR_c, #0x10 (usuário); SWI
    s.cpu.step(); s.cpu.step();
    CHECK((s.cpu.cpsr & 0x1F) == 0x13 && s.cpu.r[15] == 8 && s.cpu.r[14] == 0x03000008 && (s.cpu.spsr() & 0x1F) == 0x10, "SWI");
  }
  // usuário não troca de modo com MSR
  {
    System s(biosFrom({0xE321F010, 0xE321F013}), nullptr);
    s.cpu.step(); s.cpu.step();
    CHECK((s.cpu.cpsr & 0x1F) == 0x10, "MSR no modo de usuário trocou o modo");
  }
  // FIQ do CLIO: o timer 1 estoura e a CPU entra no vetor 0x1C com os registradores r8-r14 próprios
  {
    // BIOS: r8 = 7; vetor FIQ (na DRAM) = "mov r8, #0x55; b ."; liga a máscara do timer 1 e libera a FIQ
    std::vector<uint32_t> code = {
        0xE3A08007,              // mov r8, #7
        0xE59F0020,              // ldr r0, =0x1C
        0xE59F1020,              // ldr r1, = instrução "mov r8,#0x55"
        0xE5801000,              // str r1, [r0]
        0xE59F101C,              // ldr r1, = "b ."
        0xE5801004,              // str r1, [r0, #4]
        0xE59F0018,              // ldr r0, =0x03400048 (máscara 0)
        0xE3A01B01,              // mov r1, #0x400 (timer 1)
        0xE5801000,              // str r1, [r0]
        0xE321F013,              // msr cpsr_c, #0x13 (FIQ e IRQ liberadas)
        0xEAFFFFFE,              // b .
        0x0000001C, 0xE3A08055, 0xEAFFFFFE, 0x03400048};
    System s(biosFrom(code), nullptr);
    s.clioWrite(0x0108, 2);         // timer 1: contador
    s.clioWrite(0x010C, 2);         // recarga
    s.clioWrite(0x0200, 0x3u << 4); // timer 1 conta e recarrega
    s.runFrame();
    CHECK((s.cpu.cpsr & 0x1F) == 0x11 && s.cpu.r[8] == 0x55, "FIQ do timer: modo %02X r8 %X", s.cpu.cpsr & 0x1F, s.cpu.r[8]);
    s.cpu.setMode(0x13);
    CHECK(s.cpu.r[8] == 7, "FIQ: r8 do supervisor preservado (%X)", s.cpu.r[8]);
    CHECK(s.pending0() & (1u << 10), "pendência do timer 1");
  }
}

// ================================================================================================ MADAM: cels

static void testCels() {
  std::printf("== MADAM: motor de cels (cels compactadas e não, PLUT, cantos, PIXC, recorte) x imagens esperadas\n");
  // A: 16 bits sem PLUT, 4x3, com um pixel 0 transparente
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0x1234);
    celSetup(s);
    const uint32_t src = 0x10000;
    uint16_t img[3][4];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) img[r][c] = uint16_t(0x0421 * (r * 4 + c + 1));
    img[1][2] = 0;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) s.madam.w16(src + uint32_t(r) * 8 + uint32_t(c) * 2, img[r][c]);
    Cel c; c.src = src; c.x = 10 << 16; c.y = 20 << 16; c.pre0 = pre0(6, true, 3); c.pre1 = pre1(4, 0);
    putCel(s, 0x8000, c);
    draw(s, 0x8000);
    bool ok = true;
    for (int y = 18; y < 25; y++) for (int x = 8; x < 16; x++) {
      const bool in = x >= 10 && x < 14 && y >= 20 && y < 23;
      const uint16_t e = in ? (img[y - 20][x - 10] ? img[y - 20][x - 10] : 0x1234) : 0x1234;
      if (pix(s, x, y) != e) ok = false;
    }
    CHECK(ok, "cel de 16 bits não compactada");
    CHECK(s.madam.celsDrawn == 1, "uma cel desenhada");
  }
  // B: 4 bits com PLUT carregada pelo CCB; índice 0 -> cor 0 -> transparente
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0x0001);
    celSetup(s);
    for (int i = 0; i < 16; i++) s.madam.w16(0x9000 + uint32_t(i) * 2, uint16_t(i ? 0x7C00 | i : 0));
    // 8x2 pixels de 4 bits = 1 palavra por linha (woffset10 = 0 -> 2 palavras por linha)
    w32(s, 0x10000, 0x01234567); w32(s, 0x10004, 0); w32(s, 0x10008, 0x89ABCDEF); w32(s, 0x1000C, 0);
    Cel c; c.flags |= F::LDPLUT; c.src = 0x10000; c.plut = 0x9000; c.x = 100 << 16; c.y = 50 << 16;
    c.pre0 = pre0(3, false, 2); c.pre1 = pre1(8, 0);
    putCel(s, 0x8000, c);
    draw(s, 0x8000);
    bool ok = true;
    for (int r = 0; r < 2; r++) for (int k = 0; k < 8; k++) {
      const int v = r * 8 + k;
      const uint16_t e = v ? uint16_t(0x7C00 | v) : 0x0001;
      if (pix(s, 100 + k, 50 + r) != e) ok = false;
    }
    CHECK(ok, "cel de 4 bits com PLUT");
  }
  // C: compactada de 8 bits: literal de 3, 2 transparentes, repetição de 4; uma linha
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0x0002);
    celSetup(s);
    for (int i = 0; i < 32; i++) s.madam.w16(0x9000 + uint32_t(i) * 2, uint16_t(0x0400 * (i & 31) + 1));
    // bits: offset(10) | 01 000010 | p p p (8 bits) | 10 000001 | 11 000011 | p | 00 000000
    struct BW { std::vector<uint8_t> bits; void put(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back((v >> i) & 1); } } bw;
    bw.put(4, 10);              // próxima linha a 6 palavras
    bw.put(1, 2); bw.put(2, 6); bw.put(3, 8); bw.put(4, 8); bw.put(5, 8);
    bw.put(2, 2); bw.put(1, 6);
    bw.put(3, 2); bw.put(3, 6); bw.put(9, 8);
    bw.put(0, 2); bw.put(0, 6);
    while (bw.bits.size() % 32) bw.bits.push_back(0);
    for (size_t w = 0; w < bw.bits.size() / 32; w++) {
      uint32_t v = 0;
      for (int b = 0; b < 32; b++) v = v << 1 | bw.bits[w * 32 + size_t(b)];
      w32(s, 0x10000 + uint32_t(w) * 4, v);
    }
    Cel c; c.flags |= F::LDPLUT | F::PACKED; c.src = 0x10000; c.plut = 0x9000; c.x = 40 << 16; c.y = 41 << 16;
    c.pre0 = pre0(5, false, 1);
    putCel(s, 0x8000, c);
    draw(s, 0x8000);
    const uint16_t e[10] = {0x0C01, 0x1001, 0x1401, 2, 2, 0x2401, 0x2401, 0x2401, 0x2401, 2};
    bool ok = true;
    for (int k = 0; k < 10; k++) if (pix(s, 40 + k, 41) != e[k]) ok = false;
    CHECK(ok, "cel compactada (literal, transparente, repetição)");
  }
  // D/E/F: escala 2x, espelho e rotação de 90 graus de uma cel 2x2 de 16 bits
  {
    auto run = [](int32_t hdx, int32_t hdy, int32_t vdx, int32_t vdy, int x0, int y0, uint32_t flags, System& s) {
      fillVram(s, 0);
      celSetup(s);
      const uint16_t v[4] = {0x7C00, 0x03E0, 0x001F, 0x7FFF};
      for (int i = 0; i < 4; i++) s.madam.w16(0x10000 + uint32_t(i / 2) * 8 + uint32_t(i % 2) * 2, v[i]);
      Cel c; c.flags = flags; c.src = 0x10000; c.x = x0 << 16; c.y = y0 << 16; c.hdx = hdx; c.hdy = hdy; c.vdx = vdx; c.vdy = vdy;
      c.pre0 = pre0(6, true, 2); c.pre1 = pre1(2, 0);
      putCel(s, 0x8000, c);
      draw(s, 0x8000);
    };
    System s(idleBios(), nullptr);
    run(2 << 20, 0, 0, 2 << 16, 20, 20, F::BASE | F::LAST, s);
    bool ok = true;
    const uint16_t v[4] = {0x7C00, 0x03E0, 0x001F, 0x7FFF};
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (pix(s, 20 + x, 20 + y) != v[(y / 2) * 2 + x / 2]) ok = false;
    CHECK(ok && pix(s, 24, 20) == 0 && pix(s, 20, 24) == 0, "escala 2x");
    run(-(1 << 20), 0, 0, 1 << 16, 30, 10, F::BASE | F::LAST, s);
    CHECK(pix(s, 29, 10) == v[0] && pix(s, 28, 10) == v[1] && pix(s, 29, 11) == v[2] && pix(s, 28, 11) == v[3] && pix(s, 30, 10) == 0, "espelho horizontal");
    run(0, 1 << 20, -(1 << 16), 0, 50, 50, F::BASE | F::LAST, s);
    CHECK(pix(s, 49, 50) == v[0] && pix(s, 49, 51) == v[1] && pix(s, 48, 50) == v[2] && pix(s, 48, 51) == v[3], "rotação de 90 graus");
    run(-(1 << 20), 0, 0, 1 << 16, 30, 10, (F::BASE & ~F::ACCW) | F::LAST, s);
    CHECK(pix(s, 29, 10) == 0 && pix(s, 28, 11) == 0, "sentido anti-horário recusado sem ACCW");
  }
  // H: PIXC metade cel + metade quadro
  {
    System s(idleBios(), nullptr);
    fillVram(s, uint16_t(20 << 10 | 10 << 5 | 30));
    celSetup(s);
    s.madam.w16(0x10000, uint16_t(10 << 10 | 30 << 5 | 4));
    Cel c; c.src = 0x10000; c.x = 5 << 16; c.y = 5 << 16; c.pre0 = pre0(6, true, 1); c.pre1 = pre1(1, 0);
    c.pixc = 0x01810181;
    putCel(s, 0x8000, c);
    draw(s, 0x8000);
    const uint16_t e = uint16_t((5 + 10) << 10 | (15 + 5) << 5 | (2 + 15));
    CHECK(pix(s, 5, 5) == e, "PIXC meio a meio: %04X esperado %04X", pix(s, 5, 5), e);
  }
  // I: recorte
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0);
    celSetup(s);
    w32(s, 0x03300134, (9u << 16) | 9);
    for (int i = 0; i < 100; i++) s.madam.w16(0x10000 + uint32_t(i) * 2, 0x7FFF);
    Cel c; c.src = 0x10000; c.x = 5 << 16; c.y = 5 << 16; c.pre0 = pre0(6, true, 10); c.pre1 = pre1(10, 3);
    putCel(s, 0x8000, c);
    draw(s, 0x8000);
    CHECK(pix(s, 9, 9) == 0x7FFF && pix(s, 10, 9) == 0 && pix(s, 9, 10) == 0 && pix(s, 5, 5) == 0x7FFF, "recorte em 9,9");
  }
  // J: perspectiva (HDDX): a largura de cada linha cresce 1/4 de pixel por pixel a cada linha
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0);
    celSetup(s);
    for (int i = 0; i < 8 * 8; i++) s.madam.w16(0x10000 + uint32_t(i) * 2, 0x7FFF);
    Cel c; c.src = 0x10000; c.x = 0; c.y = 100 << 16; c.hddx = 1 << 18; c.pre0 = pre0(6, true, 8); c.pre1 = pre1(8, 2);
    putCel(s, 0x8000, c);
    draw(s, 0x8000);
    auto width = [&](int y) { int n = 0; for (int x = 0; x < 64; x++) n += pix(s, x, y) != 0; return n; };
    CHECK(width(100) == 9 && width(104) == 17 && width(107) == 23, "perspectiva: larguras %d %d %d", width(100), width(104), width(107));
  }
  // K: lista de cels com ponteiros relativos, SKIP e LAST
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0);
    celSetup(s);
    s.madam.w16(0x10000, 0x001F);
    Cel a; a.flags = (F::BASE & ~F::NPABS); a.src = 0x10000; a.x = 1 << 16; a.y = 1 << 16; a.pre0 = pre0(6, true, 1); a.pre1 = pre1(1, 0);
    a.next = 0x100 - 8;  // relativo a (endereço do campo + 4)
    Cel b = a; b.flags = F::BASE | F::SKIP; b.next = 0x8200; b.x = 2 << 16;
    Cel c = a; c.flags = F::BASE | F::LAST; c.x = 3 << 16;
    putCel(s, 0x8000, a); putCel(s, 0x8100, b); putCel(s, 0x8200, c);
    draw(s, 0x8000);
    CHECK(pix(s, 1, 1) == 0x1F && pix(s, 2, 1) == 0 && pix(s, 3, 1) == 0x1F && s.madam.celsDrawn == 2, "lista com SKIP e LAST");
  }
  // L: 6 bits, o bit P escolhe a metade do PIXC (P=1 opaca, P=0 soma AV=31 -> branco)
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0);
    celSetup(s);
    for (int i = 0; i < 32; i++) s.madam.w16(0x9000 + uint32_t(i) * 2, uint16_t(i << 5));
    // 2 pixels de 6 bits: P=1 índice 5, P=0 índice 7 -> 100101 000111
    w32(s, 0x10000, 0x947u << 20); w32(s, 0x10004, 0);
    Cel c; c.flags |= F::LDPLUT; c.src = 0x10000; c.plut = 0x9000; c.x = 70 << 16; c.y = 70 << 16;
    c.pre0 = pre0(4, false, 1); c.pre1 = pre1(2, 0);
    c.pixc = 0x1F000000u | (1u << 6) | (31u << 1);
    putCel(s, 0x8000, c);
    draw(s, 0x8000);
    CHECK(pix(s, 70, 70) == (0x8000 | 5 << 5) && pix(s, 71, 70) == 0x7FFF, "6 bits com P: %04X %04X", pix(s, 70, 70), pix(s, 71, 70));
  }
  // M: 1 bit com PLUTA (usa as entradas 6 e 7 da PLUT carregada antes)
  {
    System s(idleBios(), nullptr);
    fillVram(s, 0);
    celSetup(s);
    for (int i = 0; i < 32; i++) s.madam.w16(0x9000 + uint32_t(i) * 2, uint16_t(0x100 + i));
    w32(s, 0x10000, 5u << 24);  // 1 pixel de 8 bits (índice 5) só para carregar a PLUT
    w32(s, 0x10004, 0);
    w32(s, 0x10010, 0xA0000000u); w32(s, 0x10014, 0);  // 1 bit: 1 0 1 0
    Cel a; a.flags = (F::BASE | F::LDPLUT) & ~F::LAST; a.next = 0x8100; a.src = 0x10000; a.plut = 0x9000; a.x = 0; a.y = 0;
    a.pre0 = pre0(5, false, 1); a.pre1 = pre1(1, 0);
    Cel b; b.flags = F::BASE | F::LAST | 3; b.src = 0x10010; b.x = 200 << 16; b.y = 3 << 16; b.pre0 = pre0(1, false, 1); b.pre1 = pre1(4, 0);
    putCel(s, 0x8000, a); putCel(s, 0x8100, b);
    draw(s, 0x8000);
    CHECK(pix(s, 0, 0) == 0x105 && pix(s, 200, 3) == 0x107 && pix(s, 201, 3) == 0x106 && pix(s, 202, 3) == 0x107, "1 bit com PLUTA");
  }
}

// ================================================================================================ matrizes e VDL

static void testMatrixVdl() {
  std::printf("== MADAM: motor de matrizes e VDL (CLUT, fundo, linhas sem vídeo)\n");
  System s(idleBios(), nullptr);
  for (int i = 0; i < 16; i++) w32(s, 0x03300600 + uint32_t(i) * 4, (i % 5 == 0) ? (2u << 16) : 0);
  w32(s, 0x03300600 + 3 * 4, 5u << 16);  // translação x na 4ª coluna
  w32(s, 0x03300640, 1u << 16); w32(s, 0x03300644, 2u << 16); w32(s, 0x03300648, 3u << 16); w32(s, 0x0330064C, 1u << 16);
  w32(s, 0x033007FC, 1);
  CHECK(s.read32(0x03300660) == (2u << 16) && s.read32(0x03300664) == (4u << 16) && s.read32(0x03300668) == (6u << 16), "3x3");
  w32(s, 0x033007FC, 2);
  CHECK(s.read32(0x03300660) == (7u << 16) && s.read32(0x0330066C) == (2u << 16), "4x4 com translação");
  w32(s, 0x03300680, 100);
  w32(s, 0x033007FC, 3);
  CHECK(int32_t(s.read32(0x03300660)) == 33 && int32_t(s.read32(0x03300664)) == 66, "projeção: %d %d", int32_t(s.read32(0x03300660)), int32_t(s.read32(0x03300664)));

  // VDL: 120 linhas com vídeo (CLUT do vermelho invertida no índice 31, fundo verde), 120 sem vídeo
  fillVram(s, 0);
  s.madam.w16(0x200000 + 10 * 4, 0x7C00);              // (10,0) vermelho máximo
  s.madam.w16(0x200000 + 11 * 4 + 2, 0x001F);          // (11,1) azul máximo
  const uint32_t v = 0x4000;
  w32(s, v, 0x00200000u | 0x00010000u | 2u << 9 | 120);
  w32(s, v + 4, 0x200000); w32(s, v + 8, 0x200000); w32(s, v + 12, 0x5000);
  w32(s, v + 16, 0x60000000u | 31u << 24 | 0x400000);  // vermelho do índice 31 = 0x40
  w32(s, v + 20, 0xE000FF00u);                         // fundo verde
  w32(s, 0x5000, 120); w32(s, 0x5004, 0); w32(s, 0x5008, 0); w32(s, 0x500C, 0x5000);
  w32(s, 0x03300580, v);
  s.madam.renderFrame(s.fb);
  CHECK(s.fb[10] == 0xFF000040u, "VDL: CLUT vermelha %08X", s.fb[10]);
  CHECK(s.fb[320 + 11] == 0xFFFF0000u, "VDL: azul %08X", s.fb[320 + 11]);
  CHECK(s.fb[5] == 0xFF00FF00u, "VDL: pixel 0 = fundo %08X", s.fb[5]);
  CHECK(s.fb[200 * 320 + 10] == 0xFF00FF00u, "VDL: linhas sem vídeo mostram o fundo");
}

// ================================================================================================ CLIO

static void testClio() {
  std::printf("== CLIO: timers, interrupções, VINT, DMA do DSP, do XBUS e do controle\n");
  {
    System s(idleBios(), nullptr);
    s.clioWrite(0x0108, 3); s.clioWrite(0x010C, 3);
    s.clioWrite(0x0110, 1); s.clioWrite(0x0114, 1);
    s.clioWrite(0x0200, (0x3u << 4) | (0x7u << 8));  // timer 1 conta/recarrega; timer 2 encadeado no 1
    s.tickHardware(64 * 3);
    CHECK(s.timer(1) == 0 && !(s.pending0() & (1u << 10)), "timer 1 conta (%u)", s.timer(1));
    s.tickHardware(64);
    CHECK(s.timer(1) == 3 && (s.pending0() & (1u << 10)), "timer 1 estoura e recarrega (%u)", s.timer(1));
    CHECK(s.timer(2) == 0, "timer 2 encadeado anda 1 (%u)", s.timer(2));
    s.clioWrite(0x0044, 1u << 10);
    CHECK(!(s.pending0() & (1u << 10)), "limpar a pendência");
    s.clioWrite(0x0220, 64);  // SLACK: passo a cada 128 ciclos
    s.tickHardware(128 * 4);
    CHECK(s.timer(1) == 3 && (s.pending0() & (1u << 10)), "SLACK muda a velocidade");
    s.clioWrite(0x0060, 1);
    CHECK(s.pending0() & 0x80000000u, "bit 31 = pendência na palavra 1");
    s.clioWrite(0x0064, 1);
    CHECK(!(s.pending0() & 0x80000000u), "palavra 1 limpa");
  }
  {
    System s(idleBios(), nullptr);
    s.clioWrite(0x0008, 100);
    s.clioWrite(0x000C, 250);
    s.runFrame();
    CHECK((s.pending0() & 3) == 3, "VINT0 e VINT1 no quadro");
    CHECK((s.clioRead(0x0034) & 0x7FF) == 0 && (s.clioRead(0x0034) & 0x800), "linha 0 do campo seguinte");
  }
  {
    System s(idleBios(), nullptr);
    for (int i = 0; i < 4; i++) s.madam.w16(0x1000 + uint32_t(i) * 2, uint16_t(0x1110 * (i + 1)));
    s.madam.w16(0x2000, 0x7777);
    w32(s, 0x03300400, 0x1000); w32(s, 0x03300404, 4); w32(s, 0x03300408, 0x2000); w32(s, 0x0330040C, 2);
    s.clioWrite(0x0304, 1);
    const uint16_t a = s.dsp.read(0x300), b = s.dsp.read(0x300), c = s.dsp.read(0x300), d = s.dsp.read(0x300);
    CHECK(a == 0x1110 && b == 0x2220 && c == 0x7777 && d == 0, "FIFO do DSP com próximo bloco: %X %X %X %X", a, b, c, d);
    CHECK((s.pending0() & (1u << 16)) && !(s.clioRead(0x0304) & 1), "fim do DMA do canal 0");
  }
  {
    System s(idleBios(), nullptr);
    w32(s, 0x03300570, 0x3000); w32(s, 0x03300574, 4);
    s.mem[0x3002] = 0xEE;
    s.setPad(0x8000 | 0x0100 | 0x0020);  // A e P
    s.clioWrite(0x0304, 0x80000000u);
    s.runFrame();
    CHECK(s.mem[0x3000] == 0x81 && s.mem[0x3001] == 0x20 && s.mem[0x3002] == 0, "controle pelo PBUS: %02X %02X", s.mem[0x3000], s.mem[0x3001]);
    CHECK(s.clioRead(0x0060) & 1, "fim da leitura do controle");
  }
}

// ================================================================================================ DSP

namespace {
uint16_t arith(int nops, int muxa, int muxb, int alu, int bs, bool m2 = false) {
  return uint16_t(nops << 13 | (m2 ? 0x1000 : 0) | muxa << 10 | muxb << 8 | alu << 4 | bs);
}
uint16_t ctl(int kind, int sub, int target) { return uint16_t(0x8000 | kind << 13 | sub << 10 | target); }
void loadDsp(Dspp& d, const std::vector<uint16_t>& prog) {
  std::memset(d.code, 0, sizeof d.code);
  for (size_t i = 0; i < prog.size(); i++) d.code[i] = prog[i];
  d.running = true;
}
}  // namespace

static void testDsp() {
  std::printf("== DSP (DSPP): microcódigo de teste (MOVE, ULA, multiplicador, desvios, sub-rotina, FIFO, DAC)\n");
  Dspp d;
  d.reset();
  const uint16_t SLEEP = ctl(0, 4, 0);
  loadDsp(d, {ctl(2, 0, 0x3F0), 0x1234, ctl(2, 0, 0x3F1), 0xFEDC, SLEEP});
  d.runSample();
  CHECK(d.outL == 0x1234 && d.outR == int16_t(0xFEDC), "MOVE para o DAC");
  d.data[0x100] = 100; d.data[0x101] = 23;
  loadDsp(d, {arith(3, 1, 1, 2, 0), 0x100, 0x101, 0x102, SLEEP});
  d.runSample();
  CHECK(d.data[0x102] == 123 && d.acc == 123, "soma com 3 operandos (%u)", d.data[0x102]);
  d.data[0x100] = 0x4000; d.data[0x101] = 1000;
  loadDsp(d, {arith(3, 3, 2, 2, 0), 0x100, 0x101, 0x103, SLEEP});
  d.runSample();
  CHECK(d.data[0x103] == 500, "multiplicador 0,5 x 1000 (%d)", int16_t(d.data[0x103]));
  d.data[0x110] = 5; d.data[0x111] = 0;
  // laço: [0x111]++; [0x110]--; desvia se não zero
  loadDsp(d, {arith(2, 1, 2, 6, 0), 0x111, 0x111, arith(2, 1, 2, 7, 0), 0x110, 0x110, ctl(1, 4, 0), SLEEP});
  d.runSample();
  CHECK(d.data[0x111] == 5 && d.data[0x110] == 0, "laço com desvio condicional (%u)", d.data[0x111]);
  d.data[0x120] = 30000; d.data[0x121] = 30000;
  loadDsp(d, {arith(3, 1, 1, 2, 7), 0x120, 0x121, 0x122, SLEEP});
  d.runSample();
  CHECK(int16_t(d.data[0x122]) == 32767 && d.fv, "saturação");
  // imediato com sinal, sub-rotina e retorno
  loadDsp(d, {ctl(3, 4, 4), ctl(2, 0, 0x3F0), 0x0001, SLEEP, arith(2, 1, 2, 2, 0), uint16_t(0x8000 | 0x1FFF), 0x130, ctl(0, 2, 0)});
  d.runSample();
  CHECK(int16_t(d.data[0x130]) == -1 && d.outL == 1, "JSR/RTS e imediato negativo");
  int irqs = 0;
  d.irq = [&]() { irqs++; };
  loadDsp(d, {ctl(2, 0, 0x3FE), 1, SLEEP});
  d.runSample();
  CHECK(irqs == 1, "interrupção para a CPU");
  // DSP dentro do sistema: lê amostras do DMA e entrega ao DAC (áudio a 44,1 kHz)
  {
    System s(idleBios(), nullptr);
    for (int i = 0; i < 2000; i++) s.madam.w16(0x20000 + uint32_t(i) * 2, uint16_t(i * 16));
    w32(s, 0x03300400, 0x20000); w32(s, 0x03300404, 4000);
    s.clioWrite(0x0304, 1);
    const std::vector<uint16_t> prog = {arith(2, 1, 2, 0, 0), 0x300, 0x3F0, arith(1, 0, 2, 0, 0), 0x3F1, SLEEP};
    for (size_t i = 0; i < prog.size(); i++) s.clioWrite(0x1800 + uint32_t(i) * 4, prog[i]);
    s.clioWrite(0x17E8, 1);
    s.runFrame();
    const size_t n = s.audio.size() / 2;
    bool ok = n > 700 && n < 770;
    for (size_t i = 0; i < n && ok; i++) if (s.audio[2 * i] != int16_t(i * 16) || s.audio[2 * i + 1] != int16_t(i * 16)) ok = false;
    CHECK(ok, "amostras do DMA no DAC (%zu amostras no quadro)", n);
  }
}

// ================================================================================================ CD

namespace {
void cdCmd(System& s, std::initializer_list<uint8_t> b) {
  std::vector<uint8_t> v(b);
  v.resize(7, 0);
  for (uint8_t x : v) s.clioWrite(0x0580, x);
  s.tickHardware(CdDrive::REPLY_DELAY + 10);
}
std::vector<uint8_t> cdReply(System& s) {
  std::vector<uint8_t> r;
  while (s.clioRead(0x0540) & 0x10) r.push_back(uint8_t(s.clioRead(0x0580)));
  return r;
}
}  // namespace

static void testCd() {
  std::printf("== Unidade de CD (XBUS) com disco sintético e o sistema de arquivos Opera\n");
  OperaDisc disc;
  CHECK(cdrom::detectSystem(disc) == "3do", "detectSystem do disco Opera");
  CHECK(isOperaDisc(disc), "isOperaDisc");
  // .iso e .cue/.bin gravados de verdade
  {
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string iso = (tmp / "indigo-t9b.iso").string(), bin = (tmp / "indigo-t9b.bin").string(), cue = (tmp / "indigo-t9b.cue").string();
    { std::ofstream f(iso, std::ios::binary); f.write(reinterpret_cast<const char*>(disc.image().data()), std::streamsize(disc.image().size())); }
    {
      std::ofstream f(bin, std::ios::binary);
      uint8_t raw[2352];
      for (uint32_t l = 0; l < 64; l++) { disc.readRaw(l, raw); f.write(reinterpret_cast<const char*>(raw), 2352); }
      std::ofstream c(cue);
      c << "FILE \"indigo-t9b.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n";
    }
    std::string err;
    auto a = cdrom::open(iso, &err), b = cdrom::open(cue, &err);
    CHECK(a && cdrom::detectSystem(*a) == "3do", ".iso reconhecido como 3DO");
    CHECK(b && cdrom::detectSystem(*b) == "3do", ".cue/.bin reconhecido como 3DO");
    std::filesystem::remove(iso); std::filesystem::remove(bin); std::filesystem::remove(cue);
  }
  System s(idleBios(), &disc);
  cdCmd(s, {0x83});
  auto r = cdReply(s);
  CHECK(r.size() == 8 && r[0] == 0x83 && (r.back() & 0xC1) == 0xC1, "identificação (%zu bytes)", r.size());
  cdCmd(s, {0x8E});
  r = cdReply(s);
  CHECK(r.size() == 8 && r[2] == 1 && r[3] == 2 && r[4] == 0 && r[5] == 2 + (64 + 150 + 150) / 75 && r[6] == (64 + 150 + 150) % 75, "informações do disco");
  cdCmd(s, {0x8F, 2});
  r = cdReply(s);
  CHECK(r.size() == 9 && r[2] == 0x10 && r[3] == 2 && r[6] == 2 + (64 + 150) / 75 && r[7] == (64 + 150) % 75, "TOC da faixa de áudio");
  cdCmd(s, {0x8F, 7});
  r = cdReply(s);
  CHECK(!r.empty() && (r.back() & 0x10), "TOC de faixa inexistente dá erro");
  // leitura de 2 setores pela porta de dados (setores 1 e 2, MSF 00:02:01)
  cdCmd(s, {0x10, 0, 2, 1, 0, 0, 2});
  r = cdReply(s);
  s.tickHardware(CdDrive::CYCLES_PER_SECTOR_2X * 2 + 100);
  std::vector<uint8_t> got;
  while (s.clioRead(0x0540) & 0x20) got.push_back(uint8_t(s.clioRead(0x05C0)));
  CHECK(got.size() == 4096 && std::memcmp(got.data(), &disc.image()[2048], 4096) == 0, "leitura pela porta de dados (%zu bytes)", got.size());
  // leitura por DMA do XBUS
  w32(s, 0x03300540, 0x40000); w32(s, 0x03300544, 2048);
  s.clioWrite(0x0304, 1u << 20);
  cdCmd(s, {0x10, 0, 2, 0, 0, 0, 1});
  cdReply(s);
  s.tickHardware(CdDrive::CYCLES_PER_SECTOR_2X + 100);
  CHECK(std::memcmp(&s.mem[0x40000], &disc.image()[0], 2048) == 0 && (s.pending0() & (1u << 29)), "leitura pelo DMA do XBUS");
  // fora do disco: erro
  cdCmd(s, {0x10, 9, 0, 0, 0, 0, 1});
  r = cdReply(s);
  CHECK(!r.empty() && (r.back() & 0x10), "leitura fora do disco dá erro");
  // gaveta aberta: sem disco
  cdCmd(s, {0x06});
  r = cdReply(s);
  CHECK(!r.empty() && !(r.back() & 0x40) && !(r.back() & 0x80), "gaveta aberta");
  cdCmd(s, {0x07});
  r = cdReply(s);
  CHECK(!r.empty() && (r.back() & 0xC0) == 0xC0, "gaveta fechada com disco");
  // interrupção do XBUS quando a resposta chega
  s.clioWrite(0x0540, 1);
  s.clioWrite(0x0044, 4);
  cdCmd(s, {0x02});
  CHECK(s.pending0() & 4, "interrupção do XBUS com a resposta");
}

// ================================================================================================ save state

static void testState() {
  std::printf("== Save state, NVRAM e BIOS\n");
  // programa: conta na DRAM, desenha uma cel por quadro e o DSP toca as amostras do DMA
  const std::vector<uint32_t> prog = {
      0xE59F0010,  // ldr r0, =0x1000
      0xE3A01000,  // mov r1, #0
      0xE2811001,  // add r1, r1, #1
      0xE5801000,  // str r1, [r0]
      0xEAFFFFFC,  // b -2
      0, 0x00001000};
  auto bios = biosFrom(prog);
  CHECK(isTdoBios(bios), "BIOS de teste reconhecida pelo cabeçalho");
  std::vector<uint8_t> junk(0x80000, 0x11);
  CHECK(!isTdoBios(junk), "lixo não é BIOS");
  auto c = std::make_unique<TdoCore>(std::make_unique<OperaDisc>(), nullptr, &bios);
  {
    System& s = c->sys();
    celSetup(s);
    for (int i = 0; i < 16; i++) s.madam.w16(0x10000 + uint32_t(i) * 2, uint16_t(0x1111 * i + 1));
    Cel cel; cel.src = 0x10000; cel.x = 10 << 16; cel.y = 10 << 16; cel.pre0 = pre0(6, true, 4); cel.pre1 = pre1(4, 0);
    putCel(s, 0x8000, cel);
    w32(s, 0x4000, 0x00200000u | 0x00010000u | 240);
    w32(s, 0x4004, 0x200000);
    w32(s, 0x03300580, 0x4000);
    const uint16_t prg[] = {arith(2, 1, 2, 0, 0), 0x3F8, 0x3F0, ctl(0, 4, 0)};
    for (int i = 0; i < 4; i++) s.clioWrite(0x1800 + uint32_t(i) * 4, prg[i]);
    s.clioWrite(0x17E8, 1);
    s.clioWrite(0x0108, 100); s.clioWrite(0x010C, 100); s.clioWrite(0x0200, 0x3u << 4);
  }
  for (int f = 0; f < 10; f++) { c->runFrame(); c->sys().write32(0x03300100, 1); }
  const uint32_t count = c->sys().madam.m32(0x1000);
  CHECK(count > 1000, "a CPU roda a BIOS (%u voltas)", count);
  c->sys().audio.clear();
  const auto st = c->saveState();
  auto runHash = [&](int n) {
    uint32_t h = 0;
    for (int f = 0; f < n; f++) {
      c->runFrame();
      h = h * 31 + hashFb(c->framebuffer(), 320 * 240) + c->sys().madam.m32(0x1000);
      auto& au = c->sys().audio;  // amostras do DSP (a 44,1 kHz, antes do reamostrador do app)
      for (int16_t x : au) h = h * 3 + uint32_t(x);
      au.clear();
    }
    return h;
  };
  const uint32_t h1 = runHash(20);
  c->loadState(st);
  const uint32_t h2 = runHash(20);
  CHECK(h1 == h2, "estado carregado repete os mesmos 20 quadros (imagem, som, CPU)");
  c->loadState(st);
  CHECK(c->saveState() == st, "salvar -> carregar -> salvar dá os mesmos bytes");
  const auto before = c->saveState();
  bool refused = false;
  try { c->loadState(std::vector<uint8_t>(st.begin(), st.begin() + long(st.size() / 2))); } catch (const std::exception&) { refused = true; }
  CHECK(refused && c->saveState() == before, "estado truncado recusado sem mexer no jogo");
  refused = false;
  try { c->loadState(std::vector<uint8_t>(100, 7)); } catch (const std::exception&) { refused = true; }
  CHECK(refused && c->saveState() == before, "estado de outro console recusado");
  // NVRAM: um byte por palavra em 0x03140000
  c->sys().write32(0x03140000 + 5 * 4, 0xAB);
  CHECK(c->saveDirty() && c->exportSave().size() == 32768 && c->exportSave()[5] == 0xAB && !c->saveDirty(), "NVRAM de 32 KB como save");
  CHECK(c->sys().read32(0x03140014) == 0xAB, "NVRAM lida pela CPU");
  std::vector<uint8_t> sv(32768, 0); sv[7] = 0x42;
  TdoCore d(std::make_unique<OperaDisc>(), &sv, &bios);
  CHECK(d.sys().nvram[7] == 0x42 && d.title() == "INDIGO TESTE", "save importado e nome do volume (%s)", d.title().c_str());
  bool threw = false;
  try { setFirmware("3do", {}); TdoCore e(std::make_unique<OperaDisc>(), nullptr); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "sem BIOS o núcleo não abre");
  CHECK(createCore(bios, "panafz10.bin", nullptr) == nullptr, "a BIOS não é aberta como jogo");
}

// ================================================================================================ BIOS de teste + disco

// BIOS de teste do Índigo (t9b_3do_bios.s) e o disco homebrew próprio (t9b_3do_disco.py), montados pelo t9b_3do.sh:
// a BIOS mostra o logotipo com cels, lê o disco pela unidade de CD, acha o LaunchMe no sistema de arquivos Opera e
// o executa; o homebrew desenha cels que se movem, toca som pelo DSP e lê o controle.
static void testBoot(const std::string& dir) {
  std::printf("== BIOS de teste do Índigo + disco homebrew próprio (partida pelo CD, cels, DSP, controle)\n");
  const std::string biosPath = dir + "/bios/teste.bin", cue = dir + "/disco/homebrew.cue";
  if (!std::filesystem::exists(biosPath) || !std::filesystem::exists(cue)) { std::printf("  pulados (rode t9b_3do.sh)\n"); skipped++; return; }
  const auto bios = readAll(biosPath);
  CHECK(isTdoBios(bios), "BIOS de teste reconhecida");
  std::string err;
  auto disc = cdrom::open(cue, &err);
  CHECK(disc && cdrom::detectSystem(*disc) == "3do", "disco homebrew reconhecido como 3DO");
  if (!disc) return;
  TdoCore c(std::move(disc), nullptr, &bios);
  c.setSampleRate(48000);
  // logotipo da BIOS antes de o disco terminar de carregar
  for (int f = 0; f < 8; f++) c.runFrame();
  const uint32_t logo = hashFb(c.framebuffer(), 320 * 240);
  int lit = 0;
  for (int i = 0; i < 320 * 240; i++) lit += (c.framebuffer()[i] & 0xFFFFFF) != 0;
  CHECK(lit > 2000, "logotipo da BIOS na tela (%d pixels acesos)", lit);
  uint32_t& sig = *reinterpret_cast<uint32_t*>(&c.sys().mem[0]);  // só para leitura indireta abaixo
  (void)sig;
  int frame = 0;
  for (; frame < 600 && c.sys().madam.m32(0x000FFF00) != 0x484F4D45; frame++) c.runFrame();  // "HOME"
  CHECK(c.sys().madam.m32(0x000FFF00) == 0x484F4D45, "o homebrew do disco começou (quadro %d)", frame);
  if (frame >= 600)
    std::printf("    PC %08X modo %02X; rótulo lido %08X; raiz %08X; DMA %08X; programa %08X\n", c.sys().cpu.r[15], c.sys().cpu.cpsr & 0x1F,
                c.sys().madam.m32(0x80000), c.sys().madam.m32(0x80800), c.sys().clioRead(0x304), c.sys().madam.m32(0x10000));
  for (int f = 0; f < 30; f++) c.runFrame();
  const uint32_t a = hashFb(c.framebuffer(), 320 * 240);
  c.setKeys(KEY_RIGHT | KEY_Y);
  for (int f = 0; f < 30; f++) c.runFrame();
  const uint32_t b = hashFb(c.framebuffer(), 320 * 240);
  c.setKeys(0);
  CHECK(a != logo && b != a, "o homebrew desenha e responde ao controle");
  CHECK(c.sys().madam.m32(0x000FFF04) != 0, "o homebrew leu o botão A (%08X)", c.sys().madam.m32(0x000FFF04));
  size_t n = c.audioAvailable();
  std::vector<float> l(n), r(n);
  c.pullAudio(l.data(), r.data(), n);
  float peak = 0;
  for (float x : l) peak = std::max(peak, std::abs(x));
  CHECK(n > 400 && peak > 0.05f, "som do DSP (%zu amostras, pico %.2f)", n, double(peak));
  // estado no meio do jogo
  const auto st = c.saveState();
  for (int f = 0; f < 10; f++) c.runFrame();
  const uint32_t h1 = hashFb(c.framebuffer(), 320 * 240);
  c.loadState(st);
  for (int f = 0; f < 10; f++) c.runFrame();
  CHECK(hashFb(c.framebuffer(), 320 * 240) == h1, "estado no meio do homebrew repete os quadros");
  // velocidade
  const auto t0 = std::chrono::steady_clock::now();
  for (int f = 0; f < 300; f++) c.runFrame();
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  velocidade: %.0f quadros/s (%.1fx)\n", 300 / sec, 300 / sec / 59.94);
  // imagem para conferir a olho
  if (const char* out = std::getenv("T9B_PPM")) {
    std::ofstream f(out, std::ios::binary);
    f << "P6\n320 240\n255\n";
    for (int i = 0; i < 320 * 240; i++) { const uint32_t p = c.framebuffer()[i]; const char rgb[3] = {char(p), char(p >> 8), char(p >> 16)}; f.write(rgb, 3); }
  }
}

static void testUserBios(const std::string& dir) {
  std::printf("== BIOS do usuário (bios-joao/3do)\n");
  const char* env = std::getenv("INDIGO_3DO_BIOS");
  const std::string p = env ? env : "/root/indigo-env/bios-joao/3do/panafz10.bin";
  if (!std::filesystem::exists(p)) { std::printf("  pulados (sem a BIOS do usuário em %s)\n", p.c_str()); skipped++; return; }
  const auto bios = readAll(p);
  CHECK(isTdoBios(bios), "BIOS do usuário reconhecida");
  TdoCore c(std::make_unique<OperaDisc>(), nullptr, &bios);
  for (int f = 0; f < 300; f++) c.runFrame();
  std::printf("  rodou 300 quadros; instruções indefinidas: %u, PC %08X\n", c.sys().cpu.undefinedCount, c.sys().cpu.r[15]);
  (void)dir;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/3do";
  testCpuRuler(dir);
  testCpuDirected();
  testCels();
  testMatrixVdl();
  testClio();
  testDsp();
  testCd();
  testState();
  testBoot(dir);
  testUserBios(dir);
  std::printf("\nRESULTADO: %d verificações, %d falhas, %d grupos pulados\n", checks, fails, skipped);
  return fails ? 1 : 0;
}
