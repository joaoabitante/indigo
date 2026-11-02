#!/bin/bash
# Prepara os testes do Game & Watch em /root/indigo-env/roms/gnw (ou na pasta do 1º argumento).
#
# Os jogos de Game & Watch e os desenhos de tela do MAME não são livres: o teste (native/tools/t7_gnw.cpp)
# monta os próprios pacotes .zip, com programas para as CPUs SM510, SM5A e SM511 escritos nele mesmo e telas
# .svg geradas (grades de retângulos), e grava nesta pasta os pacotes e a imagem de cada tela.
#
# Régua da CPU: baixa o núcleo SM510/SM511/SM5A do MAME (licença BSD-3-Clause; commit fixo abaixo), compila
# FORA do repositório do Índigo com um esqueleto mínimo do framework do MAME (escrito aqui) e grava
# vetores.txt: programas aleatórios rodados ciclo a ciclo, com o resultado (escritas em S/R, LCD, registros e
# RAM) que o Índigo tem de reproduzir.
set -e
DEST="${1:-/root/indigo-env/roms/gnw}"
CASOS="${CASOS:-1000}"     # programas por CPU
CICLOS="${CICLOS:-60000}"  # ciclos de 32768 Hz por programa
MAME_COMMIT=079ac58c453b09a3ffd8267d1c611b4d2c63795b
B="$DEST/build/regua"
mkdir -p "$B"
cd "$B"
for f in sm510base.cpp sm510base.h sm510.cpp sm510.h sm511.cpp sm511.h sm500.cpp sm500.h sm500op.cpp sm5a.cpp sm5a.h sm510op.cpp; do
  [ -s "$f" ] || curl -sSfL -o "$f" "https://raw.githubusercontent.com/mamedev/mame/$MAME_COMMIT/src/devices/cpu/sm510/$f"
done

cat > emu.h <<'FIM'
// Stub mínimo do framework do MAME para compilar o núcleo SM510 (BSD-3-Clause) como régua, fora do Índigo.
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef u32 offs_t;
#define ATTR_COLD
#define BIT(x, n) (((x) >> (n)) & 1)
#define NAME(x) x
#define FUNC(x) nullptr
#define TIMER_CALLBACK_MEMBER(name) void name(s32 param)
#define DECLARE_DEVICE_TYPE(a, b) extern int a;
#define DEFINE_DEVICE_TYPE(a, b, c, d) int a = 0;
enum { INPUT_LINE_RESET = 100, CLEAR_LINE = 0, ASSERT_LINE = 1 };
enum { AS_PROGRAM = 0, AS_DATA = 1 };
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3 };
enum endianness_t { ENDIANNESS_LITTLE };
typedef int device_type;
struct machine_config {};
class device_t;
namespace util { class disasm_interface { public: virtual ~disasm_interface() = default; }; }

struct attotime {
  static attotime from_ticks(u64, u32) { return {}; }
  static attotime from_hz(double) { return {}; }
};
struct emu_timer { void adjust(attotime, int = 0, attotime = {}) {} };

struct address_map_entry {
  address_map_entry& rom() { return *this; }
  address_map_entry& ram() { return *this; }
  address_map_entry& share(const char*) { return *this; }
  address_map_entry& mirror(u32) { return *this; }
  template <typename A, typename B> address_map_entry& rw(A, B) { return *this; }
};
struct address_map { address_map_entry e; address_map_entry& operator()(u32, u32) { return e; } };
struct address_map_constructor { template <typename... T> address_map_constructor(T...) {} };
struct address_space_config { template <typename... T> address_space_config(T...) {} };

// espaço de endereços: funções postas pela régua
struct address_space {
  std::function<u8(offs_t)> rd;
  std::function<void(offs_t, u8)> wr;
  u8 read_byte(offs_t a) { return rd(a); }
  void write_byte(offs_t a, u8 v) { wr(a, v); }
};

struct state_entry { state_entry& formatstr(const char*) { return *this; } state_entry& noshow() { return *this; } };

template <typename T> struct devcb_r {
  std::function<T()> f; T def;
  template <typename D> devcb_r(D&, T d = 0) : def(d) {}
  T operator()() { return f ? f() : def; }
  int bind() { return 0; }
};
typedef devcb_r<u8> devcb_read8;
typedef devcb_r<int> devcb_read_line;
template <typename T> struct devcb_w {
  std::function<void(offs_t, T)> f;
  template <typename D> devcb_w(D&) {}
  void operator()(T v) { if (f) f(0, v); }
  void operator()(offs_t o, T v) { if (f) f(o, v); }
  int bind() { return 0; }
};
typedef devcb_w<u8> devcb_write8;
typedef devcb_w<u16> devcb_write16;

template <typename T> struct optional_shared_ptr {
  T* p = nullptr;
  template <typename D> optional_shared_ptr(D&, const char*) {}
  operator T*() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};
template <typename T> struct optional_region_ptr {
  T* p = nullptr;
  template <typename D> optional_region_ptr(D&, const char*) {}
  T& operator[](int i) const { return p[i]; }
  explicit operator bool() const { return p != nullptr; }
};

class device_memory_interface { public: typedef std::vector<std::pair<int, const address_space_config*>> space_config_vector; };

class cpu_device : public device_memory_interface {
public:
  cpu_device(const machine_config&, device_type, const char*, device_t*, u32 clock) : m_clock(clock) {}
  virtual ~cpu_device() = default;
  address_space m_spaces[2];
  address_space& space(int i) { return m_spaces[i]; }
  u32 m_clock;
  u32 unscaled_clock() const { return m_clock; }
  template <typename T> void save_item(T&) {}
  template <typename T> state_entry state_add(int, const char*, T&) { return {}; }
  void set_icountptr(int&) {}
  template <typename F, typename O> emu_timer* timer_alloc(F, O) { static emu_timer t; return &t; }
  void notify_clock_changed() {}
  void debugger_instruction_hook(u16) {}
  void debugger_wait_hook() {}
  void standard_irq_callback(int, u16) {}
  void logerror(const char*, ...) {}
  virtual void device_start() {}
  virtual void device_reset() {}
  virtual void device_post_load() {}
  virtual u64 execute_clocks_to_cycles(u64 c) const noexcept { return c; }
  virtual u64 execute_cycles_to_clocks(u64 c) const noexcept { return c; }
  virtual u32 execute_min_cycles() const noexcept { return 1; }
  virtual u32 execute_max_cycles() const noexcept { return 1; }
  virtual void execute_set_input(int, int) {}
  virtual void execute_run() {}
  virtual space_config_vector memory_space_config() const { return {}; }
  virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }
};
FIM

cat > sm510d.h <<'FIM'
#pragma once
#include "emu.h"
class sm510_disassembler : public util::disasm_interface {};
class sm511_disassembler : public util::disasm_interface {};
class sm500_disassembler : public util::disasm_interface {};
class sm5a_disassembler : public util::disasm_interface {};
FIM

cat > ref.cpp <<'FIM'
// Régua: roda o núcleo SM510/SM511/SM5A do MAME (BSD-3-Clause) com programas aleatórios e grava o resultado.
// uso: ref <saida.txt> <casos por CPU> <ciclos>
#include <cstdio>
#include "emu.h"
#define protected public  // só a régua: acesso aos campos do núcleo do MAME
#include "sm510.h"
#include "sm511.h"
#include "sm5a.h"

struct Rng { uint32_t s; uint32_t operator()() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; } };
static uint32_t fnv(uint32_t h, uint32_t v) { for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 0x01000193u; } return h; }

#define EXPOE(Base)                                                                         \
  struct R_##Base : Base {                                                                  \
    using Base::Base;                                                                       \
    void start() { device_start(); }                                                        \
    void rst() { device_reset(); }                                                          \
    void div() { div_timer_cb(0); }                                                         \
    void lcd() { lcd_update(); }                                                            \
    int step(int k) { execute_set_input(0, k != 0); const bool h = m_halt; m_icount = 1; execute_run(); return (h && m_icount == 0) ? 0 : 1 - m_icount; } \
    int clk() const { return m_clk_div; }                                                   \
    void setR(int m) { m_r_mask_option = m; }                                               \
    std::vector<int> regs() {                                                               \
      std::vector<int> v = {m_pc, m_prev_pc, m_op, m_prev_op, m_param, m_stack[0], m_stack[1], m_acc, m_bl, m_bm, \
                            m_bmask, m_c, m_skip, m_halt, m_w, m_r, m_r_out, m_div, m_gamma, m_l, m_x, m_y, m_bp, m_bc, \
                            m_clk_div, m_melody_rd, m_melody_step_count, m_melody_duty_count, m_melody_duty_index, \
                            m_melody_address};                                              \
      extra(v);                                                                             \
      return v;                                                                             \
    }

EXPOE(sm510_device)
  void extra(std::vector<int>& v) { for (int i = 0; i < 21; i++) v.push_back(0); }
  void linkLcd(u8* d) { m_lcd_ram_a.p = d + 0x60; m_lcd_ram_b.p = d + 0x70; }
  void mel(u8*) {}
};
EXPOE(sm511_device)
  void extra(std::vector<int>& v) { for (int i = 0; i < 21; i++) v.push_back(0); }
  void linkLcd(u8* d) { m_lcd_ram_a.p = d + 0x60; m_lcd_ram_b.p = d + 0x70; }
  void mel(u8* m) { m_melody_rom.p = m; }
};
EXPOE(sm512_device)
  void extra(std::vector<int>& v) { for (int i = 0; i < 21; i++) v.push_back(0); }
  void linkLcd(u8* d) { m_lcd_ram_a.p = d + 0x60; m_lcd_ram_b.p = d + 0x70; m_lcd_ram_c.p = d + 0x50; }
  void mel(u8* m) { m_melody_rom.p = m; }
};
EXPOE(sm5a_device)
  void extra(std::vector<int>& v) {
    for (int i = 0; i < 9; i++) v.push_back(m_ox[i]);
    for (int i = 0; i < 9; i++) v.push_back(m_o[i]);
    v.push_back(m_mx); v.push_back(m_cb); v.push_back(m_rsub);
  }
  void linkLcd(u8*) {}
  void mel(u8*) {}
};

template <typename D>
static void caso(FILE* out, int tipo, uint32_t seed, int ticks) {
  machine_config mc;
  D d(mc, "cpu", nullptr, 32768);
  Rng rng{seed};
  static u8 rom[0x1000], mel[0x100], ram[0x80];
  const int prgSize = tipo == 0 ? 0x740 : 0x1000;
  // saltos (0x80-0xFF) em 1/8 dos bytes, para o programa andar mais antes de cair num laço
  for (int i = 0; i < 0x1000; i++) {
    uint32_t b = rng();
    if ((b & 0x80) && (rng() & 3)) b &= 0x7F;
    rom[i] = i < prgSize ? static_cast<u8>(b) : 0;
  }
  for (int i = 0; i < 0x100; i++) mel[i] = static_cast<u8>(rng() & 0x3F);
  for (int i = 0; i < 0x80; i++) ram[i] = static_cast<u8>(rng() & 0xF);
  const bool sm510 = tipo == 1, sm5a = tipo == 0;
  d.space(AS_PROGRAM).rd = [&](offs_t a) -> u8 {
    a &= 0xFFF;
    if (sm510) return (a & 0x3FF) < 0x2C0 ? rom[a] : 0;
    if (sm5a) { a &= 0x7FF; if (a >= 0x700) a = 0x700 | (a & 0x3F); }
    return rom[a];
  };
  auto mapa = [&](offs_t a) -> offs_t {
    a &= 0x7F;
    if (!sm5a) return a;
    if (a >= 0x40) return 0x40 | std::min<offs_t>(a & 0xF, 0xC);  // banco 4 (espelho 0x30) e D-F -> C
    if ((a & 0xF) > 0xC) return a & 0x7C;  // mirror_r/mirror_w
    return a;
  };
  d.space(AS_DATA).rd = [&](offs_t a) -> u8 { return ram[mapa(a)]; };
  d.space(AS_DATA).wr = [&](offs_t a, u8 v) { ram[mapa(a)] = v; };
  uint32_t t = 0, lastS = 0, lastR = 0, hS = 0x811C9DC5u, hR = 0x811C9DC5u, hL = 0x811C9DC5u;
  bool log = false;
  auto K = [&]() -> u8 { return static_cast<u8>(((lastS * 7) ^ (lastR * 13) ^ (t >> 10)) & 0xF); };
  d.m_read_k.f = K;
  d.m_read_ba.f = [&]() { return static_cast<int>(t >> 8 & 1); };
  d.m_read_b.f = [&]() { return static_cast<int>((t >> 11 & 1) ^ 1); };
  d.m_write_s.f = [&](offs_t, u8 v) { lastS = v; if (log) hS = fnv(fnv(hS, t), v); };
  d.m_write_r.f = [&](offs_t, u8 v) { lastR = v; if (log) hR = fnv(fnv(hR, t), v); };
  u16 segs[32] = {};
  d.m_write_segs.f = [&](offs_t o, u16 v) { segs[o & 31] = v; };
  d.linkLcd(ram);
  d.mel(tipo >= 2 ? mel : nullptr);
  if (sm510) d.setR(2);
  d.start();
  // RAM aleatória depois de ligar (o device_start do MAME não mexe na RAM)
  d.rst();
  log = true;
  int credito = 0;
  for (t = 0; t < static_cast<uint32_t>(ticks); t++) {
    if (t && (t & 8191) == 0) d.rst();
    d.div();
    if (--credito <= 0) {
      const int c = d.step(K());
      credito = c == 0 ? 1 : credito + c * d.clk();
    }
    if ((t & 31) == 31) {
      std::memset(segs, 0, sizeof segs);
      d.lcd();
      for (int i = 0; i < 32; i++) hL = fnv(hL, segs[i]);
    }
  }
  std::fprintf(out, "%d %u %08X %08X %08X", tipo, seed, hS, hR, hL);
  for (int v : d.regs()) std::fprintf(out, " %d", v);
  std::fprintf(out, " ");
  for (int i = 0; i < 0x80; i++) std::fprintf(out, "%X", ram[i]);
  std::fprintf(out, "\n");
}

int main(int argc, char** argv) {
  FILE* out = std::fopen(argv[1], "w");
  const int n = std::atoi(argv[2]), ticks = std::atoi(argv[3]);
  if (argc > 5) { const int tp = std::atoi(argv[4]); const uint32_t sd = std::atoi(argv[5]); if (tp == 1) caso<R_sm510_device>(out, 1, sd, ticks); else if (tp == 2) caso<R_sm511_device>(out, 2, sd, ticks); else caso<R_sm5a_device>(out, 0, sd, ticks); return 0; }
  std::fprintf(out, "#ciclos %d\n", ticks);
  for (int i = 0; i < n; i++) {
    caso<R_sm5a_device>(out, 0, 1000 + i, ticks);
    caso<R_sm510_device>(out, 1, 2000 + i, ticks);
    caso<R_sm511_device>(out, 2, 3000 + i, ticks);
    caso<R_sm512_device>(out, 3, 4000 + i, ticks);
  }
  std::fclose(out);
}
FIM

g++ -std=c++17 -O2 -w -I. ref.cpp sm510base.cpp sm510op.cpp sm510.cpp sm511.cpp sm500.cpp sm500op.cpp sm5a.cpp -o regua
./regua "$DEST/vetores.txt" "$CASOS" "$CICLOS"
echo "pronto: $DEST/vetores.txt ($CASOS programas por CPU, $CICLOS ciclos)"
