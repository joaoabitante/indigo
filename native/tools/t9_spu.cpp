// Testes do SPU do PlayStation (src/ps1/spu.cpp).
//   indigo-t9_spu <pasta>              tudo (pasta = /root/indigo-env/roms/ps1spu, preparada por tools/t9_spu.sh)
//   indigo-t9_spu <pasta> velocidade   só a medida de velocidade
//
// 1. Unidade: registradores (o spu/test e o spu/memory-transfer do ps1-tests refeitos sem o console), FIFO e
//    tipos de transferência (0x1AC), bits de SPUSTAT, DMA, fila do CD e captura.
// 2. Diferencial contra o SPU do Mednafen/Beetle PSX (libspuref.so, compilada fora do repositório):
//    cenários aleatórios (amostras ADPCM válidas na RAM por FIFO e por DMA, KON/KOFF, ADSR, varreduras, pitch,
//    FM, ruído, reverberação, IRQ por endereço, áudio do CD) e o registro de escritas de um jogo do ps1-tests
//    (spu/playback). Compara cada amostra da saída, todos os registradores lidos, a linha da IRQ, o DMA e a RAM.
// 3. Save state: salvar -> carregar -> salvar dá os mesmos bytes, e as duas cópias seguem iguais.
// 4. Velocidade: 24 vozes com reverberação e modulação.
#include <dlfcn.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../src/ps1/spu.h"

using namespace indigo;
using indigo::ps1::Spu;

namespace {

long g_checks = 0, g_fails = 0;
int g_shown = 0;

void check(bool ok, const char* fmt, ...) {
  g_checks++;
  if (ok) return;
  g_fails++;
  if (g_shown++ >= 40) return;
  std::va_list ap;
  va_start(ap, fmt);
  std::printf("  falha: ");
  std::vprintf(fmt, ap);
  std::printf("\n");
  va_end(ap);
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return uint32_t(s >> 16);
  }
  uint32_t operator()(uint32_t n) { return n ? next() % n : 0; }
  bool chance(int pct) { return int(next() % 100) < pct; }
};

// ---------------------------------------------------------------------------------------------------------
// régua

struct Ref {
  void (*power)();
  void (*write)(uint32_t, uint16_t);
  uint16_t (*read)(uint32_t);
  void (*run)(int32_t);
  uint32_t (*take)(int16_t*, uint32_t);
  void (*dmaWrite)(uint32_t);
  uint32_t (*dmaRead)();
  void (*pushCd)(int16_t, int16_t);
  int (*irqLine)();
  int (*irqEdges)();
  uint16_t (*ram)(uint32_t);
  bool ok = false;

  bool load(const std::string& path) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
#define SYM(var, name) var = reinterpret_cast<decltype(var)>(dlsym(h, name)); if (!var) return false
    SYM(power, "ref_power"); SYM(write, "ref_write"); SYM(read, "ref_read"); SYM(run, "ref_run");
    SYM(take, "ref_take"); SYM(dmaWrite, "ref_dma_write"); SYM(dmaRead, "ref_dma_read");
    SYM(pushCd, "ref_push_cd"); SYM(irqLine, "ref_irq_line"); SYM(irqEdges, "ref_irq_edges"); SYM(ram, "ref_ram");
#undef SYM
    ok = true;
    return true;
  }
};

// Os dois SPUs lado a lado, recebendo o mesmo.
struct Par {
  Spu& a;
  Ref& r;
  int edges = 0;
  long amostras = 0, diferentes = 0, regsLidos = 0, regsDif = 0;
  long naoNulas = 0, irqsTotal = 0;  // para ver que o teste não passa em silêncio
  const char* nome = "";
  std::vector<int16_t> bufA = std::vector<int16_t>(8192 * 2), bufR = std::vector<int16_t>(8192 * 2);

  Par(Spu& spu, Ref& ref) : a(spu), r(ref) { a.onIrq = [this] { edges++; }; }

  void reset() {
    a.reset();
    r.power();
    irqsTotal += edges;
    edges = 0;
  }
  void w(uint32_t addr, uint16_t v) {
    a.write(addr, v, 2);
    r.write(addr, v);
  }
  uint16_t rd(uint32_t addr) {
    const uint16_t x = uint16_t(a.read(addr, 2)), y = r.read(addr);
    // SPUSTAT bits 7-10 (pedido de DMA e ocupado) não existem no Mednafen
    const uint16_t m = addr == 0x1AE ? 0xF87F : 0xFFFF;
    regsLidos++;
    if ((x & m) != (y & m)) {
      regsDif++;
      check(false, "%s: registrador 0x%03X lido 0x%04X, régua 0x%04X (amostra %ld)", nome, addr, x, y, amostras);
    }
    return x;
  }
  void comparaRegs() {
    for (uint32_t addr = 0; addr < 0x400; addr += 2)
      if (addr != 0x1A8) rd(addr);
  }
  void comparaIrq() {
    const bool ok = a.irqLine() == (r.irqLine() != 0) && edges == r.irqEdges();
    if (!ok)
      check(false, "%s: IRQ linha %d/%d subidas %d/%d (amostra %ld)", nome, a.irqLine(), r.irqLine(), edges,
            r.irqEdges(), amostras);
  }
  void run(int64_t ciclos) {
    while (ciclos > 0) {
      const int c = int(ciclos > 768 * 4000 ? 768 * 4000 : ciclos);
      a.run(c);
      r.run(c);
      ciclos -= c;
      const size_t na = a.take(bufA.data(), 8192), nr = r.take(bufR.data(), 8192);
      if (na != nr) check(false, "%s: %zu amostras, régua %zu", nome, na, nr);
      const size_t n = na < nr ? na : nr;
      for (size_t i = 0; i < n; i++) {
        if (bufA[i * 2] | bufA[i * 2 + 1]) naoNulas++;
        if (bufA[i * 2] != bufR[i * 2] || bufA[i * 2 + 1] != bufR[i * 2 + 1]) {
          if (diferentes < 5)
            check(false, "%s: amostra %ld = (%d, %d), régua (%d, %d)", nome, amostras + long(i), bufA[i * 2],
                  bufA[i * 2 + 1], bufR[i * 2], bufR[i * 2 + 1]);
          diferentes++;
        }
      }
      amostras += long(n);
    }
    comparaIrq();
  }
  void dmaW(uint32_t v) {
    a.dmaWrite(v);
    r.dmaWrite(v);
  }
  void dmaR() {
    const uint32_t x = a.dmaRead(), y = r.dmaRead();
    check(x == y, "%s: DMA leu 0x%08X, régua 0x%08X", nome, x, y);
  }
  void cd(int16_t l, int16_t rr) {
    a.pushCdAudio(l, rr);
    r.pushCd(l, rr);
  }
  void comparaRam() {
    long dif = 0;
    uint32_t primeiro = 0;
    for (uint32_t i = 0; i < 0x40000; i++)
      if (a.ram()[i] != r.ram(i) && !dif++) primeiro = i;
    check(dif == 0, "%s: %ld meias palavras diferentes na RAM (primeira 0x%05X: 0x%04X, régua 0x%04X)", nome, dif,
          primeiro, a.ram()[primeiro], r.ram(primeiro));
  }
};

// ---------------------------------------------------------------------------------------------------------
// ADPCM: codificador simples (busca o melhor filtro e shift de cada bloco), para ter amostras realistas

const int FILTRO[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

// 'flagsFim' vai no último bloco; 'blocoLoop' recebe o flag 4 (início da repetição)
std::vector<uint16_t> codifica(const std::vector<int16_t>& pcm, uint8_t flagsFim, int blocoLoop) {
  const size_t blocos = (pcm.size() + 27) / 28;
  std::vector<uint16_t> out;
  int h1 = 0, h2 = 0;
  for (size_t b = 0; b < blocos; b++) {
    int melhorErro = -1, melhorF = 0, melhorS = 0, mh1 = 0, mh2 = 0;
    uint8_t melhorN[28] = {};
    for (int f = 0; f < 5; f++)
      for (int s = 0; s <= 12; s++) {
        int a1 = h1, a2 = h2;
        long erro = 0;
        uint8_t nib[28];
        for (int i = 0; i < 28; i++) {
          const size_t k = b * 28 + i;
          const int x = k < pcm.size() ? pcm[k] : 0;
          const int pred = ((a1 * FILTRO[f][0]) >> 6) + ((a2 * FILTRO[f][1]) >> 6);
          const int esc = 1 << (12 - s);
          int n = int(std::lround(double(x - pred) / esc));
          if (n < -8) n = -8;
          if (n > 7) n = 7;
          int y = n * esc + pred;
          if (y < -32768) y = -32768;
          if (y > 32767) y = 32767;
          erro += long(std::abs(x - y));
          nib[i] = uint8_t(n & 0xF);
          a2 = a1;
          a1 = y;
        }
        if (melhorErro < 0 || erro < melhorErro) {
          melhorErro = int(erro > 0x7FFFFFFF ? 0x7FFFFFFF : erro);
          melhorF = f; melhorS = s; mh1 = a1; mh2 = a2;
          std::memcpy(melhorN, nib, 28);
        }
      }
    h1 = mh1; h2 = mh2;
    uint8_t flags = 0;
    if (b + 1 == blocos) flags |= flagsFim;
    if (int(b) == blocoLoop) flags |= 4;
    out.push_back(uint16_t(melhorS | melhorF << 4 | flags << 8));
    for (int i = 0; i < 7; i++)
      out.push_back(uint16_t(melhorN[i * 4] | melhorN[i * 4 + 1] << 4 | melhorN[i * 4 + 2] << 8 | melhorN[i * 4 + 3] << 12));
  }
  return out;
}

std::vector<uint16_t> amostraAleatoria(Rng& g) {
  const int blocos = 2 + int(g(90));
  std::vector<uint16_t> data;
  if (g.chance(15)) {
    // blocos com bytes aleatórios (shift e filtro até 15, inclusive os reservados)
    for (int b = 0; b < blocos; b++) {
      uint8_t flags = uint8_t(g.chance(10) ? g(8) : 0);
      if (b + 1 == blocos) flags |= g.chance(50) ? 3 : 1;
      data.push_back(uint16_t(g(256) | flags << 8));
      for (int i = 0; i < 7; i++) data.push_back(uint16_t(g.next()));
    }
    return data;
  }
  std::vector<int16_t> pcm(size_t(blocos) * 28);
  const int tipo = int(g(5));
  const double freq = 0.005 + g(1000) / 4000.0, amp = 2000 + g(30000);
  double fase = 0;
  for (size_t i = 0; i < pcm.size(); i++) {
    double v;
    fase += freq;
    switch (tipo) {
      case 0: v = std::sin(fase); break;
      case 1: v = std::sin(fase) > 0 ? 0.8 : -0.8; break;
      case 2: v = std::fmod(fase / 6.2832, 1.0) * 2 - 1; break;
      case 3: v = (int(g(2001)) - 1000) / 1000.0; break;
      default: v = std::sin(fase) * std::exp(-double(i) / 400) + std::sin(fase * 3.01) * 0.3; break;
    }
    pcm[i] = int16_t(std::lround(v * amp > 32767 ? 32767 : (v * amp < -32768 ? -32768 : v * amp)));
  }
  const uint8_t fim = g.chance(60) ? 3 : (g.chance(80) ? 1 : 0);
  const int loop = g.chance(70) ? (g.chance(50) ? 0 : int(g(uint32_t(blocos)))) : -1;
  return codifica(pcm, fim, loop);
}

// envia dados para a RAM de som pelos caminhos dos jogos: FIFO (32 de cada vez, modo 1) ou DMA (modo 2)
void envia(Par& p, uint16_t ctrl, uint32_t endereco, const std::vector<uint16_t>& d, int jeito) {
  const uint16_t parado = ctrl & ~0x30;
  p.w(0x1AC, 4);
  p.w(0x1AA, parado);
  p.w(0x1A6, uint16_t(endereco >> 2));
  if (jeito == 0) {  // DMA
    p.w(0x1AA, parado | 0x20);
    for (size_t i = 0; i < d.size(); i += 2) p.dmaW(d[i] | uint32_t(i + 1 < d.size() ? d[i + 1] : 0) << 16);
  } else if (jeito == 1) {  // FIFO com o modo parado, depois escrita manual
    for (size_t i = 0; i < d.size(); i += 32) {
      for (size_t k = i; k < i + 32 && k < d.size(); k++) p.w(0x1A8, d[k]);
      p.w(0x1AA, parado | 0x10);
      p.w(0x1AA, parado);
    }
  } else {  // escrita manual direta (modo 1 já ligado, como no registro do ps1-tests)
    p.w(0x1AA, parado | 0x10);
    for (size_t i = 0; i < d.size(); i++) p.w(0x1A8, d[i]);
  }
  p.w(0x1AA, ctrl);
}

struct Amostra {
  uint32_t inicio, fim;  // meias palavras
};

void configuraVoz(Par& p, Rng& g, int v, const std::vector<Amostra>& amostras) {
  const uint32_t b = uint32_t(v) * 16;
  const Amostra& s = amostras[g(uint32_t(amostras.size()))];
  for (int lr = 0; lr < 2; lr++) {
    uint16_t vol;
    if (g.chance(70)) vol = uint16_t(g(0x4000));
    else if (g.chance(30)) vol = uint16_t(0x4000 + g(0x4000));  // fixo negativo
    else vol = uint16_t(0x8000 | g(0x8000));                     // varredura
    p.w(b + lr * 2, vol);
  }
  uint16_t pitch;
  const uint32_t t = g(100);
  if (t < 70) pitch = uint16_t(0x200 + g(0x1E00));
  else if (t < 90) pitch = uint16_t(g(0x4000));
  else pitch = uint16_t(g(0x10000));
  p.w(b + 4, pitch);
  uint32_t inicio = s.inicio;
  if (g.chance(10)) inicio += g(s.fim - s.inicio) & ~7u;
  p.w(b + 6, uint16_t(inicio >> 2));
  uint32_t adsr;
  if (g.chance(60)) {
    const uint32_t ar = g(0x50), dr = g(16), sl = g(16), sr = g.chance(50) ? 0x7F : g(0x80), rr = g(32);
    adsr = sl | dr << 4 | ar << 8 | (g.chance(30) ? 0x8000u : 0) | rr << 16 | (g.chance(50) ? 0x200000u : 0) |
           sr << 22 | (g.chance(50) ? 0x40000000u : 0) | (g.chance(50) ? 0x80000000u : 0);
  } else {
    adsr = g.next() << 16 ^ g.next();
  }
  p.w(b + 8, uint16_t(adsr));
  p.w(b + 10, uint16_t(adsr >> 16));
  if (g.chance(15)) p.w(b + 14, uint16_t((s.inicio + (g(s.fim - s.inicio) & ~7u)) >> 2));
}

void configuraReverb(Par& p, Rng& g, uint32_t base) {
  const uint32_t tam = (0x40000 - base) >> 2;  // em unidades de 8 bytes
  for (int i = 0; i < 32; i++) {
    uint16_t v;
    const bool coef = i == 2 || (i >= 3 && i <= 9) || i >= 30;
    if (coef) {
      if (g.chance(3)) v = 0x8000;
      else v = uint16_t(int16_t(int(g(0xE000)) - 0x7000));
    } else if (i <= 1) {
      v = uint16_t(g(tam < 0x400 ? tam : 0x400));
    } else {
      v = uint16_t(g(tam));
    }
    p.w(0x1C0 + uint32_t(i) * 2, v);
  }
}

// um cenário aleatório: 'segundos' de áudio; devolve o controle final
void cenario(Par& p, Rng& g, double segundos, bool salvaNoMeio, Spu* copia) {
  p.reset();
  uint16_t ctrl = uint16_t(0xC000 | (g.chance(75) ? 0x80 : 0) | (g.chance(50) ? 1 : 0) | (g.chance(40) ? 4 : 0) |
                           g(64) << 8);
  p.w(0x1AC, 4);
  p.w(0x1AA, ctrl);
  for (int lr = 0; lr < 2; lr++) {
    p.w(0x180 + lr * 2, g.chance(80) ? uint16_t(0x3FFF) : uint16_t(0x8000 | g(0x8000)));
    p.w(0x184 + lr * 2, uint16_t(int16_t(int(g(0x10000)) - 0x8000)));
    p.w(0x1B0 + lr * 2, uint16_t(g(0x8000)));
  }
  // área de trabalho da reverberação no fim da RAM
  const uint32_t tams[4] = {0x2000, 0x8000, 0x10000, 0x1F000};
  const uint32_t base = 0x40000 - tams[g(4)];
  p.w(0x1A2, uint16_t(base >> 2));
  configuraReverb(p, g, base);
  // amostras
  std::vector<Amostra> amostras;
  uint32_t livre = 0x800;
  const int n = 4 + int(g(10));
  for (int i = 0; i < n; i++) {
    const std::vector<uint16_t> d = amostraAleatoria(g);
    if (livre + d.size() + 8 >= base) break;
    envia(p, ctrl, livre, d, int(g(3)));
    amostras.push_back({livre, uint32_t(livre + d.size())});
    livre = (livre + uint32_t(d.size()) + 7) & ~7u;
  }
  p.w(0x188, 0); // nada pendente
  for (int v = 0; v < 24; v++) configuraVoz(p, g, v, amostras);
  p.w(0x190, uint16_t(g.chance(30) ? g(0x10000) : 0));
  p.w(0x194, uint16_t(g.chance(20) ? g(0x10000) & g(0x10000) : 0));
  p.w(0x198, uint16_t(g(0x10000)));
  p.w(0x19A, uint16_t(g(0x100)));

  const long alvo = long(segundos * 44100);
  const long inicio = p.amostras;
  bool salvou = false;
  while (p.amostras - inicio < alvo) {
    const uint32_t ev = g(100);
    if (ev < 25) {
      uint32_t m = 0;
      const int k = 1 + int(g(3));
      for (int i = 0; i < k; i++) {
        const int v = int(g(24));
        if (g.chance(70)) configuraVoz(p, g, v, amostras);
        m |= 1u << v;
      }
      if (m & 0xFFFF) p.w(0x188, uint16_t(m));
      if (m >> 16) p.w(0x18A, uint16_t(m >> 16));
    } else if (ev < 35) {
      const uint32_t m = (1u << g(24)) | (g.chance(30) ? g(0x1000000) : 0);
      p.w(0x18C, uint16_t(m));
      p.w(0x18E, uint16_t(m >> 16));
    } else if (ev < 40) {
      const uint32_t r = 0x190 + g(6) * 2;
      p.w(r, uint16_t(g(0x10000) & g(0x10000)));
    } else if (ev < 44) {
      ctrl = uint16_t((ctrl & 0xC0FF) | g(64) << 8);
      if (g.chance(20)) ctrl ^= 0x80;
      if (g.chance(15)) ctrl ^= 1;
      if (g.chance(15)) ctrl ^= 4;
      if (g.chance(3)) ctrl ^= 0x4000;  // mudo
      if (g.chance(2)) ctrl ^= 0x8000;  // SPU desligado
      if (!(ctrl & 0xC000) && g.chance(50)) ctrl |= 0xC000;
      p.w(0x1AA, ctrl);
    } else if (ev < 48) {
      const uint32_t q = g(4);
      if (q == 0) p.w(0x180 + g(2) * 2, uint16_t(g.chance(50) ? g(0x8000) : 0x8000 | g(0x8000)));
      else if (q == 1) p.w(0x184 + g(2) * 2, uint16_t(g(0x10000)));
      else if (q == 2) p.w(0x1B0 + g(2) * 2, uint16_t(g(0x10000)));
      else p.w(0x1B8 + g(2) * 2, uint16_t(g(0x10000)));
    } else if (ev < 50) {
      if (g.chance(30)) configuraReverb(p, g, base);
      else p.w(0x1C0 + g(32) * 2, uint16_t(g(0x10000)));
    } else if (ev < 56) {
      // IRQ por endereço: numa amostra, na captura, na reverberação ou no endereço de transferência
      if ((ctrl & 0x40) && g.chance(60)) {
        p.w(0x1AA, ctrl & ~0x40);  // reconhece
        if (g.chance(70)) p.w(0x1AA, ctrl);
        else ctrl &= ~0x40;
      } else {
        uint32_t alvoIrq;
        const uint32_t q = g(4);
        if (q == 0 && !amostras.empty()) {
          const Amostra& s = amostras[g(uint32_t(amostras.size()))];
          alvoIrq = s.inicio + (g(s.fim - s.inicio) & ~3u);
        } else if (q == 1) alvoIrq = g(0x800) & ~3u;
        else if (q == 2) alvoIrq = base + (g(0x40000 - base) & ~3u);
        else alvoIrq = g(0x40000) & ~3u;
        p.w(0x1A4, uint16_t(alvoIrq >> 2));
        ctrl |= 0x40;
        p.w(0x1AA, ctrl);
      }
    } else if (ev < 62) {
      p.comparaRegs();
    } else if (ev < 65) {
      // DMA durante a música: escreve numa amostra (pode mudar o que toca) ou lê
      p.w(0x1A6, uint16_t((g.chance(50) && !amostras.empty() ? amostras[g(uint32_t(amostras.size()))].inicio
                                                              : g(0x40000)) >> 2));
      const int k = 1 + int(g(64));
      if (g.chance(50)) for (int i = 0; i < k; i++) p.dmaW(g.next());
      else for (int i = 0; i < k; i++) p.dmaR();
    } else if (ev < 70) {
      const int k = 1 + int(g(1200));
      double f = g(100) / 300.0, fase = 0;
      for (int i = 0; i < k; i++) {
        fase += f;
        p.cd(int16_t(std::sin(fase) * 20000), int16_t(std::cos(fase * 1.5) * 20000));
      }
    } else if (ev < 74) {
      const int v = int(g(24));
      const uint32_t q = g(3);
      if (q == 0) p.w(uint32_t(v) * 16 + 12, uint16_t(g(0x8000)));
      else if (q == 1) p.w(0x200 + uint32_t(v) * 4 + g(2) * 2, uint16_t(g(0x10000)));
      else if (!amostras.empty()) {
        const Amostra& s = amostras[g(uint32_t(amostras.size()))];
        p.w(uint32_t(v) * 16 + 14, uint16_t((s.inicio + (g(s.fim - s.inicio) & ~7u)) >> 2));
      }
    } else if (ev < 76) {
      p.w(0x1A6, uint16_t(g(0x10000)));
      for (int i = int(g(8)); i >= 0; i--) p.rd(0x1A8);
    } else if (ev < 77) {
      p.w(0x19C + g(2) * 2, uint16_t(g(0x10000)));  // o Mednafen deixa escrever em ENDX
    } else if (ev < 78) {
      p.w(0x260 + g(16) * 2, uint16_t(g(0x10000)));
    }
    if (salvaNoMeio && !salvou && p.amostras - inicio > alvo / 2 && copia) {
      // save state: salvar -> carregar -> salvar
      salvou = true;
      StateIO s1;
      p.a.save(s1);
      StateIO in(s1.data());
      copia->load(in);
      StateIO s2;
      copia->save(s2);
      check(s1.data() == s2.data(), "save state: salvar -> carregar -> salvar mudou os bytes (%zu e %zu)",
            s1.data().size(), s2.data().size());
    }
    const uint32_t q = g(100);
    p.run(q < 50 ? 1 + g(3000) : (q < 90 ? 1 + g(40000) : 1 + g(400000)));
  }
  p.comparaRegs();
  p.comparaRam();
}

// ---------------------------------------------------------------------------------------------------------
// registro de escritas de um jogo (ps1-tests spu/playback/spu-dump.h)

std::vector<uint8_t> leDump(const std::string& path) {
  std::ifstream f(path);
  std::vector<uint8_t> b;
  if (!f) return b;
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const size_t ini = s.find('{');
  for (size_t i = ini == std::string::npos ? 0 : ini; i + 3 < s.size(); i++)
    if (s[i] == '0' && s[i + 1] == 'x') {
      b.push_back(uint8_t(std::strtoul(s.substr(i + 2, 2).c_str(), nullptr, 16)));
      i += 3;
    }
  return b;
}

void tocaDump(Par& p, const std::vector<uint8_t>& b) {
  p.reset();
  p.nome = "playback";
  // como o spu/playback: cada escrita leva ~100 ciclos; 'V' é um quadro NTSC
  int quadros = 0;
  for (size_t i = 0; i < b.size();) {
    const char op = char(b[i++]);
    if (op == 'X') {
      if (i > 1) break;
    } else if (op == 'V') {
      p.run(565045);
      if (++quadros % 50 == 0) p.comparaRegs();
    } else if (op == 'W' && i + 6 <= b.size()) {
      const uint32_t a = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | uint32_t(b[i + 3]) << 24;
      const uint16_t d = uint16_t(b[i + 4] | b[i + 5] << 8);
      i += 6;
      if (a == 0x1F801DAA && ((d >> 4) & 3) == 2) continue;  // o teste ignora o modo de DMA
      p.w(a & 0x3FF, d);
      p.run(100);
    } else if (op == 'F' && i + 2 <= b.size()) {
      const int n = b[i] | b[i + 1] << 8;
      i += 2;
      for (int k = 0; k < n && i + 2 <= b.size(); k++, i += 2) {
        p.w(0x1A8, uint16_t(b[i] | b[i + 1] << 8));
        p.run(100);
      }
    } else {
      break;
    }
  }
  p.run(44100 * 768);  // mais um segundo
  p.comparaRegs();
  p.comparaRam();
}

// ---------------------------------------------------------------------------------------------------------
// testes de unidade

void unidade() {
  auto s = std::make_unique<Spu>();
  Spu& a = *s;
  int irqs = 0;
  a.onIrq = [&] { irqs++; };

  // spu/test: 16 e 32 bits em 0x1D80
  for (int i = 0; i < 16; i++) {
    a.write(0x180, 1u << i, 2);
    a.run(10000);
    check(a.read(0x180, 2) == (1u << i), "spu/test: 16 bits 0x%04X", 1u << i);
  }
  for (int i = 0; i < 32; i++) {
    a.write(0x180, 1u << i, 4);
    a.run(10000);
    check(a.read(0x180, 4) == (1u << i), "spu/test: 32 bits 0x%08X", 1u << i);
  }
  a.reset();

  // memory-transfer: DTC
  for (uint16_t v : {0x0000, 0xFFFF, 0x0000, 0x5555, 0x0000, 0xAAAA}) {
    a.write(0x1AC, v, 2);
    check(a.read(0x1AC, 2) == v, "memory-transfer: DTC 0x%04X", v);
  }
  // memory-transfer: escrita manual
  const char* texto = "Hello there   :)";
  a.write(0x1AC, 2 << 1, 2);
  a.write(0x1AA, 0xC000, 2);
  a.write(0x1A6, 0x1000 / 8, 2);
  for (int i = 0; i < 16; i += 2) a.write(0x1A8, uint8_t(texto[i]) | uint8_t(texto[i + 1]) << 8, 2);
  check(a.ram()[0x800] == 0, "FIFO: nada vai para a RAM antes do modo de escrita manual");
  a.write(0x1AA, 0xC010, 2);
  check(std::memcmp(&a.ram()[0x800], texto, 16) == 0, "FIFO: escrita manual na RAM");
  check(a.read(0x1AE, 2) & 0x400, "SPUSTAT bit 10 durante a escrita manual");
  a.run(200);
  check(!(a.read(0x1AE, 2) & 0x400), "SPUSTAT bit 10 volta a zero depois da escrita manual");
  // os bits 0-5 de SPUCNT aparecem em SPUSTAT até a próxima amostra
  for (uint16_t c : {0x3F, 0x00, 0x15, 0x2A}) {
    a.write(0x1AA, 0xC000 | c, 2);
    a.run(768);
    check((a.read(0x1AE, 2) & 0x3F) == c, "SPUSTAT bits 0-5 = 0x%02X", c);
  }
  // pedidos de DMA
  a.write(0x1AA, 0xC020, 2);
  check(a.dmaRequest() && (a.read(0x1AE, 2) & 0x380) == 0x180, "SPUSTAT: pedido de DMA de escrita");
  a.write(0x1AA, 0xC030, 2);
  check(a.dmaRequest() && (a.read(0x1AE, 2) & 0x380) == 0x280, "SPUSTAT: pedido de DMA de leitura");
  a.write(0x1AA, 0xC000, 2);
  check(!a.dmaRequest() && (a.read(0x1AE, 2) & 0x380) == 0, "SPUSTAT: sem pedido de DMA parado");
  // memory-transfer: leitura e escrita por DMA
  a.write(0x1A6, 0x1000 / 8, 2);
  a.write(0x1AA, 0xC030, 2);
  char buf[16];
  for (int i = 0; i < 4; i++) {
    const uint32_t w = a.dmaRead();
    std::memcpy(buf + i * 4, &w, 4);
  }
  check(std::memcmp(buf, texto, 16) == 0, "memory-transfer: leitura por DMA");
  a.write(0x1A6, 0x2000 / 8, 2);
  a.write(0x1AA, 0xC020, 2);
  for (int i = 0; i < 4; i++) {
    uint32_t w;
    std::memcpy(&w, texto + i * 4, 4);
    a.dmaWrite(w);
  }
  check(std::memcmp(&a.ram()[0x1000], texto, 16) == 0, "memory-transfer: escrita por DMA");
  // leitura de 8 bits
  a.write(0x1A6, 0x1000 / 8, 2);
  check(a.read(0x1A8, 1) == uint8_t('H'), "leitura de 8 bits da FIFO");
  check(a.read(0x1AD, 1) == 0 && a.read(0x1AC, 1) == 4, "leitura de 8 bits de 0x1AC/0x1AD");
  a.write(0x180, 0x0080, 2);
  a.write(0x181, 0x12, 1);
  check(a.read(0x180, 2) == 0x0080, "escrita de 8 bits em endereço ímpar não tem efeito");

  // tipos de transferência (0x1AC): normal, Rep2, Rep4, Rep8 e preenchimento
  const uint16_t esperado[5][8] = {{1, 2, 3, 4, 5, 6, 7, 8},
                                   {1, 1, 3, 3, 5, 5, 7, 7},
                                   {1, 1, 1, 1, 5, 5, 5, 5},
                                   {8, 8, 8, 8, 8, 8, 8, 8},
                                   {8, 8, 8, 8, 8, 8, 8, 8}};
  const int tipos[5] = {2, 3, 4, 5, 0};
  for (int t = 0; t < 5; t++) {
    a.write(0x1AA, 0xC000, 2);
    a.write(0x1AC, uint32_t(tipos[t]) << 1, 2);
    a.write(0x1A6, 0x4000 / 8, 2);
    for (uint16_t k = 1; k <= 8; k++) a.write(0x1A8, k, 2);
    a.write(0x1AA, 0xC010, 2);
    check(std::memcmp(&a.ram()[0x2000], esperado[t], 16) == 0, "tipo de transferência %d", tipos[t]);
    // por DMA também
    a.write(0x1AA, 0xC020, 2);
    a.write(0x1A6, 0x5000 / 8, 2);
    for (uint32_t k = 1; k <= 8; k += 2) a.dmaWrite(k | (k + 1) << 16);
    check(std::memcmp(&a.ram()[0x2800], esperado[t], 16) == 0, "tipo de transferência %d por DMA", tipos[t]);
  }
  a.write(0x1AC, 4, 2);
  // FIFO cheia: o que passa de 32 meias palavras se perde
  a.write(0x1AA, 0xC000, 2);
  a.write(0x1A6, 0x6000 / 8, 2);
  for (uint16_t k = 0; k < 40; k++) a.write(0x1A8, uint16_t(0x100 + k), 2);
  a.write(0x1AA, 0xC010, 2);
  check(a.ram()[0x3000 + 31] == 0x11F && a.ram()[0x3000 + 32] == 0, "FIFO de 32 meias palavras");

  // IRQ por endereço na transferência
  a.write(0x1AA, 0xC000, 2);
  a.write(0x1A4, 0x7000 / 8, 2);
  a.write(0x1AA, 0xC040, 2);
  irqs = 0;
  a.write(0x1A6, 0x6FF8 / 8, 2);
  a.write(0x1AA, 0xC060, 2);
  for (int i = 0; i < 4; i++) a.dmaWrite(0);
  check(irqs == 1 && a.irqLine() && (a.read(0x1AE, 2) & 0x40) == 0, "IRQ na escrita por DMA (SPUSTAT só depois)");
  a.run(768);
  check(a.read(0x1AE, 2) & 0x40, "SPUSTAT bit 6 depois da IRQ");
  a.write(0x1AA, 0xC000, 2);
  check(!a.irqLine(), "a IRQ some com SPUCNT bit 6 desligado");

  // CD: captura e mistura
  a.reset();
  a.write(0x1AA, 0xC001, 2);
  a.write(0x180, 0x3FFF, 2);
  a.write(0x182, 0x3FFF, 2);
  a.write(0x1B0, 0x7FFF, 2);
  a.write(0x1B2, 0x4000, 2);
  for (int i = 0; i < 600; i++) a.pushCdAudio(int16_t(i * 50), int16_t(-i * 50));
  a.run(768 * 2);  // a primeira amostra pega o volume principal só na segunda
  int16_t o[1200];
  size_t n = a.take(o, 600);
  check(n == 2 && a.ram()[0] == 0 && a.ram()[1] == 50 && a.ram()[0x201] == uint16_t(-50), "CD na captura");
  a.run(768 * 598);
  n = a.take(o, 600);
  check(n == 598 && o[2 * 100] == int16_t((((102 * 50) * 0x7FFF) >> 15) * 0x7FFE >> 15), "CD na saída (esquerdo)");
  check(o[2 * 100 + 1] == int16_t((((-102 * 50) * 0x4000) >> 15) * 0x7FFE >> 15), "CD na saída (direito)");
  a.run(768 * 10);
  n = a.take(o, 600);
  check(n == 10 && o[18] == 0, "CD sem dados dá silêncio");

  // estado danificado: bytes aleatórios não podem levar a acesso fora dos vetores (rodar com sanitizadores)
  {
    StateIO s0;
    a.save(s0);
    Rng g(99);
    for (int k = 0; k < 20; k++) {
      std::vector<uint8_t> lixo = s0.data();
      for (auto& b : lixo) if (g.chance(k < 10 ? 100 : 5)) b = uint8_t(g.next());
      try {
        StateIO in(lixo);
        a.load(in);
      } catch (const std::exception&) {
      }
      a.write(0x188, 0xFFFF, 2);
      a.run(768 * 3000);
      a.take(o, 600);
    }
    check(true, "estado danificado");
  }
}

// ---------------------------------------------------------------------------------------------------------
// velocidade: 24 vozes tocando em repetição, reverberação ligada, metade com modulação

double mede(Spu* a, Ref* r, double segundos) {
  // amostra longa em repetição
  std::vector<int16_t> pcm(28 * 200);
  for (size_t i = 0; i < pcm.size(); i++) pcm[i] = int16_t(std::sin(double(i) * 0.05) * 12000 + std::sin(double(i) * 0.31) * 6000);
  const std::vector<uint16_t> d = codifica(pcm, 3, 0);
  auto w = [&](uint32_t addr, uint16_t v) {
    if (a) a->write(addr, v, 2);
    if (r) r->write(addr, v);
  };
  if (a) a->reset();
  if (r) r->power();
  w(0x1AC, 4);
  w(0x1AA, 0xC010);
  w(0x1A6, 0x1000 >> 2);
  for (uint16_t v : d) w(0x1A8, v);
  w(0x1AA, 0xC080);
  w(0x180, 0x3FFF); w(0x182, 0x3FFF); w(0x184, 0x3000); w(0x186, 0x3000);
  w(0x1A2, 0xE000);  // 64 KB de reverberação
  const uint16_t rvb[32] = {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300,
                            0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000,
                            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C,
                            0x8000, 0x8000};
  for (int i = 0; i < 32; i++) w(0x1C0 + i * 2, rvb[i]);
  for (int v = 0; v < 24; v++) {
    w(v * 16 + 0, 0x0800); w(v * 16 + 2, 0x0800);
    w(v * 16 + 4, uint16_t(0x0800 + v * 0x90));
    w(v * 16 + 6, 0x1000 >> 2);
    w(v * 16 + 8, 0x0F0F); w(v * 16 + 10, 0x1FC0 | 0x8000);
  }
  w(0x190, 0xAAAA); w(0x192, 0xAA);
  w(0x198, 0xFFFF); w(0x19A, 0xFF);
  w(0x188, 0xFFFF); w(0x18A, 0xFF);
  std::vector<int16_t> buf(8192 * 2);
  const auto t0 = std::chrono::steady_clock::now();
  const long total = long(segundos * 44100);
  for (long feitas = 0; feitas < total; feitas += 4000) {
    if (a) { a->run(768 * 4000); a->take(buf.data(), 8192); }
    if (r) { r->run(768 * 4000); r->take(buf.data(), 8192); }
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string pasta = argc > 1 ? argv[1] : "/root/indigo-env/roms/ps1spu";
  const std::string modo = argc > 2 ? argv[2] : "";
  Ref ref;
  const bool temRef = ref.load(pasta + "/libspuref.so");

  if (modo != "velocidade") {
    std::printf("== unidade (registradores, FIFO, tipos de transferência, DMA, CD)\n");
    unidade();

    if (!temRef) {
      check(false, "régua %s/libspuref.so não encontrada (rode tools/t9_spu.sh)", pasta.c_str());
    } else {
      auto spu = std::make_unique<Spu>();
      auto copia = std::make_unique<Spu>();
      Par p(*spu, ref);
      std::printf("== diferencial contra o Mednafen: cenários aleatórios\n");
      const int CENARIOS = 40;
      long amostras = 0, dif = 0, regs = 0, regsDif = 0, naoNulas = 0, irqs = 0;
      for (int c = 0; c < CENARIOS; c++) {
        Rng g(uint64_t(c) + 1);
        char nome[32];
        std::snprintf(nome, sizeof nome, "cenário %d", c);
        p.nome = nome;
        p.amostras = p.diferentes = p.regsLidos = p.regsDif = 0;
        const long f0 = g_fails;
        p.naoNulas = p.irqsTotal = 0;
        cenario(p, g, c < 4 ? 8.0 : 2.5, c == 1, copia.get());
        naoNulas += p.naoNulas; irqs += p.irqsTotal + p.edges;
        amostras += p.amostras; dif += p.diferentes; regs += p.regsLidos; regsDif += p.regsDif;
        if (g_fails != f0) std::printf("  %s: %ld amostras, %ld diferentes\n", nome, p.amostras, p.diferentes);
      }
      std::printf("  %d cenários: %ld amostras estéreo (%ld com som), %ld diferentes; %ld IRQs; %ld leituras de "
                  "registradores, %ld diferentes\n", CENARIOS, amostras, naoNulas, dif, irqs, regs, regsDif);
      check(naoNulas > amostras / 2 && irqs > 100, "cenários com pouco som (%ld) ou poucas IRQs (%ld)", naoNulas, irqs);

      // save state no meio da música: as duas cópias seguem iguais
      {
        Rng g(77);
        Par q(*spu, ref);
        q.nome = "save state";
        cenario(q, g, 1.0, false, nullptr);
        StateIO s1;
        spu->save(s1);
        StateIO in(s1.data());
        copia->load(in);
        StateIO s2;
        copia->save(s2);
        check(s1.data() == s2.data(), "save state: salvar -> carregar -> salvar");
        std::vector<int16_t> x(8192 * 2), y(8192 * 2);
        long iguais = 0, total = 0;
        for (int k = 0; k < 40; k++) {
          const uint16_t kon = uint16_t(g(0x10000));
          spu->write(0x188, kon, 2); copia->write(0x188, kon, 2);
          spu->run(768 * 2000); copia->run(768 * 2000);
          const size_t na = spu->take(x.data(), 8192), nb = copia->take(y.data(), 8192);
          total += long(na);
          if (na == nb && std::memcmp(x.data(), y.data(), na * 4) == 0) iguais += long(na);
        }
        check(iguais == total && total == 80000, "save state: cópia carregada segue igual (%ld de %ld)", iguais, total);
        std::printf("  save state: %zu bytes; salvar->carregar->salvar igual; %ld amostras iguais depois de carregar\n",
                    s1.data().size(), iguais);
      }

      std::printf("== diferencial contra o Mednafen: registro de um jogo (ps1-tests spu/playback)\n");
      const std::vector<uint8_t> dump = leDump(pasta + "/ps1-tests/spu/playback/spu-dump.h");
      if (dump.size() < 1000) {
        check(false, "ps1-tests/spu/playback/spu-dump.h não encontrado");
      } else {
        p.amostras = p.diferentes = p.regsLidos = p.regsDif = p.naoNulas = 0;
        tocaDump(p, dump);
        std::printf("  playback: %ld amostras (%ld com som), %ld diferentes; %ld leituras de registradores, %ld "
                    "diferentes\n", p.amostras, p.naoNulas, p.diferentes, p.regsLidos, p.regsDif);
        check(p.naoNulas > p.amostras / 2, "playback quase sem som");
      }
    }
  }

  std::printf("== velocidade (24 vozes, reverberação, modulação em 12)\n");
  {
    auto spu = std::make_unique<Spu>();
    const double seg = 60;
    const double t = mede(spu.get(), nullptr, seg);
    std::printf("  Índigo: %.0f s de áudio em %.3f s (%.0f vezes o tempo real; %.2f%% de um núcleo)\n", seg, t, seg / t,
                100 * t / seg);
    if (temRef) {
      const double tr = mede(nullptr, &ref, seg);
      std::printf("  Mednafen (régua): %.3f s (%.0f vezes o tempo real)\n", tr, seg / tr);
    }
  }

  std::printf("RESULTADO ps1 spu: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
