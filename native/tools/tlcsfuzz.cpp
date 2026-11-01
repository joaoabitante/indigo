// Teste diferencial da CPU TLCS-900H (lado C++): mesma geração de casos de native/tools/tlcsfuzz.mjs.
// uso: indigo-tlcsfuzz casos [inicio] [detalhe]
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "../src/ngp/tlcs900h.h"

using namespace indigo::ngp;

namespace {
uint32_t seed = 0;
std::unordered_map<uint32_t, uint8_t> mem;
std::vector<std::pair<uint32_t, uint32_t>> writes;
long reads = 0;
struct Hang {};

uint32_t def(uint32_t a) { return ((a ^ seed) * 0x9E3779B1u) >> 24; }
uint32_t rb(uint32_t a) { if (++reads > 60000) throw Hang{}; a &= 0xFFFFFF; auto it = mem.find(a); return it != mem.end() ? it->second : def(a); }
void wb(uint32_t a, uint32_t v) { a &= 0xFFFFFF; mem[a] = v & 0xFF; writes.push_back({a, v & 0xFF}); }

struct Bus : TlcsBus {
  uint32_t read8(uint32_t a) override { return rb(a); }
  uint32_t read16(uint32_t a) override { return rb(a) | (rb(a + 1) << 8); }
  uint32_t read32(uint32_t a) override { return rb(a) | (rb(a + 1) << 8) | (rb(a + 2) << 16) | (rb(a + 3) << 24); }
  void write8(uint32_t a, uint32_t v) override { wb(a, v); }
  void write16(uint32_t a, uint32_t v) override { wb(a, v); wb(a + 1, v >> 8); }
  void write32(uint32_t a, uint32_t v) override { wb(a, v); wb(a + 1, v >> 8); wb(a + 2, v >> 16); wb(a + 3, v >> 24); }
};

uint32_t x = 0;
uint32_t next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s casos [inicio] [detalhe]\n", argv[0]); return 2; }
  const long count = std::atol(argv[1]);
  const long start = argc > 2 ? std::atol(argv[2]) : 1;
  const bool detail = argc > 3;
  Bus bus;
  TLCS900H cpu(bus);
  cpu.onBios = [] {};
  for (long i = start; i < start + count; i++) {
    seed = static_cast<uint32_t>(i) * 0x85EBCA6Bu; x = seed ? seed : 1;
    mem.clear(); writes.clear();
    cpu.reset();
    for (int k = 0; k < 20; k++) cpu.r[k] = next();
    cpu.r[20] = 0;
    cpu.pc = next() & 0xFFFFFF; cpu.sr = next() & 0xFFFF; cpu.fd = next() & 0xFF;
    for (int k = 0; k < 4; k++) {
      cpu.dmaS[k] = next(); cpu.dmaD[k] = next();
      cpu.dmaC[k] = static_cast<uint16_t>(next()); cpu.dmaM[k] = static_cast<uint8_t>(next());
    }
    cpu.intNest = next() & 0xFFFF;
    reads = 0;
    int cyc;
    try { cyc = cpu.step(); } catch (Hang&) { std::printf("HANG\n"); continue; }
    std::vector<uint32_t> st = {cpu.pc & 0xFFFFFF, static_cast<uint32_t>(cpu.sr & 0xFFFF), static_cast<uint32_t>(cpu.fd & 0xFF)};
    for (uint32_t v : cpu.r) st.push_back(v);
    for (uint32_t v : cpu.dmaS) st.push_back(v);
    for (uint32_t v : cpu.dmaD) st.push_back(v);
    for (uint32_t v : cpu.dmaC) st.push_back(v);
    for (uint32_t v : cpu.dmaM) st.push_back(v);
    st.push_back(static_cast<uint32_t>(cpu.intNest)); st.push_back(cpu.halted ? 1 : 0); st.push_back(static_cast<uint32_t>(cyc));
    for (const auto& [a, v] : writes) { st.push_back(a); st.push_back(v); }
    if (detail) {
      for (size_t k = 0; k < st.size(); k++) std::printf("%s%08x", k ? " " : "", st[k]);
      std::printf("\n");
      continue;
    }
    uint32_t h = 0x811C9DC5u;
    for (uint32_t v : st) { h ^= v; h *= 0x01000193u; }
    std::printf("%08x\n", h);
  }
  return 0;
}
