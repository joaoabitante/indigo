// Testes do PC Engine (pasta de testes como argumento, normalmente /root/indigo-env/roms/pce):
//  1. CPU contra os SingleStepTests do HuC6280 (huc6280/v1, gerados pelo Mesen 2): registradores, MPR,
//     memória, número de ciclos e o endereço/tipo de cada ciclo de barramento.
//  2. Testes do compilador HuC (huc-tests/*.pce, 4 variantes de compilação): cada programa C termina com
//     exit(0) (opcode $63 com X = 0) ou abort() (opcode $E2) rodando no núcleo inteiro.
//  3. ROMs homebrew e de teste de hardware (pasta roms/, entre elas rcrtest, tiatest, dmatest, mwrtest e
//     buftest do John Brandwood, que medem tempos do VDC): 600 quadros comparados um a um (cores de 9 bits)
//     com o Mesen 2 (ref/*.mesen.txt, a régua) e com o Geargrafx (ref/*.txt, só informativo), gerados pelo
//     t7_pce.sh.
//  4. Save state determinístico, estados danificados, ROMs de lixo, BRAM e frequência do PSG.
// Uso: indigo-t7_pce pasta [cpu|huc|roms|state]
//      indigo-t7_pce rom.pce shot saida.png [quadros]   (ferramenta: tira um PNG)
//      indigo-t7_pce rom.pce hashes saida.txt quadros    (ferramenta: hashes de cada quadro)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

#include "../src/pce/huc6280.h"
#include "../src/pce/pce.h"

using indigo::pce::HuC6280;

namespace {

long g_checks = 0, g_fails = 0;

void check(bool ok, const std::string& what) {
  g_checks++;
  if (!ok) { g_fails++; if (g_fails < 200) std::printf("FALHA: %s\n", what.c_str()); }
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::vector<std::string> listDir(const std::string& dir, const std::string& suf) {
  std::vector<std::string> r;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0) r.push_back(n);
    }
    closedir(d);
  }
  std::sort(r.begin(), r.end());
  return r;
}

// ------------------------------------------------------------ SingleStepTests do HuC6280
struct Cyc { uint32_t addr; uint8_t data; char kind; };  // kind: r, w, - (ciclo interno)

struct SstBus : indigo::pce::CpuBus {
  std::vector<uint8_t> mem = std::vector<uint8_t>(1 << 21);
  std::vector<Cyc> log;
  HuC6280* cpu = nullptr;
  uint8_t slowRead(uint8_t bank, uint16_t off) override {
    uint32_t a = (uint32_t(bank) << 13) | off;
    if (!log.empty()) log.back() = {a, mem[a], 'r'};
    return mem[a];
  }
  void slowWrite(uint8_t bank, uint16_t off, uint8_t v) override {
    uint32_t a = (uint32_t(bank) << 13) | off;
    mem[a] = v;
    if (!log.empty()) log.back() = {a, v, 'w'};
  }
  void stWrite(int, uint8_t) override {}
  void sync() override { log.push_back({0, 0, '-'}); }
};

struct SstState { uint8_t a, x, y, s, p; uint16_t pc; uint8_t mpr[8]; std::vector<std::pair<uint32_t, uint8_t>> ram; };

template <typename T> T rd(const uint8_t* b, size_t& p) { T v; std::memcpy(&v, b + p, sizeof v); p += sizeof v; return v; }

bool readState(const std::vector<uint8_t>& f, size_t& p, SstState& s) {
  const size_t start = p;
  if (p + 4 > f.size()) return false;
  const int32_t sz = rd<int32_t>(f.data(), p);
  if (sz < 0 || start + sz > f.size()) return false;
  s.a = f[p++]; s.x = f[p++]; s.y = f[p++]; s.s = f[p++]; s.p = f[p++];
  s.pc = rd<uint16_t>(f.data(), p);
  for (int i = 0; i < 8; i++) s.mpr[i] = f[p++];
  const int32_t n = rd<int32_t>(f.data(), p);
  s.ram.clear();
  for (int i = 0; i < n; i++) { uint32_t a = rd<uint32_t>(f.data(), p); uint8_t v = f[p++]; s.ram.push_back({a & 0x1FFFFF, v}); }
  p = start + sz;
  return true;
}

void testCpu(const std::string& dir) {
  long total = 0, bad = 0;
  int opsBad = 0;
  for (int op = 0; op < 256; op++) {
    char name[64];
    std::snprintf(name, sizeof name, "%s/%02x.json.bin", dir.c_str(), op);
    std::vector<uint8_t> f;
    if (!readFile(name, f) || f.size() < 4) { check(false, std::string("sem arquivo ") + name); continue; }
    size_t p = 0;
    const uint32_t count = rd<uint32_t>(f.data(), p);
    long opBad = 0;
    std::string first;
    for (uint32_t t = 0; t < count && p + 4 <= f.size(); t++) {
      const size_t start = p;
      const int32_t full = rd<int32_t>(f.data(), p);
      std::string tname(reinterpret_cast<const char*>(&f[p]), strnlen(reinterpret_cast<const char*>(&f[p]), 50));
      p += 50;
      p += 4;  // opcode
      SstState in, out;
      if (!readState(f, p, in) || !readState(f, p, out)) break;
      const uint32_t numCycles = rd<uint32_t>(f.data(), p);
      const uint32_t rec = rd<uint32_t>(f.data(), p);
      std::vector<Cyc> want;
      for (uint32_t i = 0; i < rec; i++) {
        uint32_t c = rd<uint32_t>(f.data(), p);
        bool r = c & 1, w = (c >> 1) & 1;
        want.push_back({r || w ? (c >> 3) & 0x1FFFFF : 0, static_cast<uint8_t>(r || w ? c >> 24 : 0), r ? 'r' : w ? 'w' : '-'});
      }
      p = start + full;

      static SstBus bus;
      static HuC6280 cpu;
      for (int b = 0; b < 256; b++) { cpu.rd[b] = nullptr; cpu.wr[b] = nullptr; }
      cpu.bus = &bus;
      for (auto& r : in.ram) bus.mem[r.first] = r.second;
      cpu.a = in.a; cpu.x = in.x; cpu.y = in.y; cpu.s = in.s; cpu.p = in.p; cpu.pc = in.pc;
      std::memcpy(cpu.mpr, in.mpr, 8);
      cpu.fast = true; cpu.clk = 0; cpu.nextEvent = 0; cpu.irqActive = 0; cpu.irqDisabled = 0;
      bus.log.clear();
      cpu.step();
      // transferências em bloco: o arquivo só guarda 500 pares de RAM e 500 ciclos, então a memória que o
      // bloco lê (e a pilha, quando o destino passa por ela) não é conhecida; ficam os ciclos e os endereços
      const bool block = op == 0x73 || op == 0xC3 || op == 0xD3 || op == 0xE3 || op == 0xF3;
      bool ok = (block || (cpu.a == out.a && cpu.x == out.x && cpu.y == out.y)) && cpu.s == out.s && cpu.p == out.p &&
                cpu.pc == out.pc && std::memcmp(cpu.mpr, out.mpr, 8) == 0 && bus.log.size() == numCycles;
      if (!block) for (auto& r : out.ram) ok = ok && bus.mem[r.first] == r.second;
      for (size_t i = 0; ok && i < want.size() && i < bus.log.size(); i++) {
        const Cyc& g = bus.log[i];
        const Cyc& w = want[i];
        if (g.kind != w.kind || (w.kind != '-' && (g.addr != w.addr || (!block && g.data != w.data)))) ok = false;
      }
      for (auto& r : out.ram) bus.mem[r.first] = 0;
      for (auto& r : in.ram) bus.mem[r.first] = 0;
      for (auto& c : bus.log) if (c.kind == 'w') bus.mem[c.addr] = 0;
      total++;
      if (!ok) {
        bad++; opBad++;
        if (first.empty()) {
          char b[256];
          std::snprintf(b, sizeof b, "%s: A%02X X%02X Y%02X S%02X P%02X PC%04X ciclos %zu (esperado A%02X X%02X Y%02X S%02X P%02X PC%04X ciclos %u)",
                        tname.c_str(), cpu.a, cpu.x, cpu.y, cpu.s, cpu.p, cpu.pc, bus.log.size(), out.a, out.x, out.y, out.s,
                        out.p, out.pc, numCycles);
          first = b;
        }
      }
    }
    if (opBad) { opsBad++; std::printf("  opcode %02X: %ld falhas; %s\n", op, opBad, first.c_str()); }
  }
  std::printf("CPU HuC6280 (SingleStepTests): %ld testes, %ld falhas, %d opcodes com falha\n", total, bad, opsBad);
  check(bad == 0 && total > 0, "SingleStepTests do HuC6280");
}

// ------------------------------------------------------------ PNG (sem compressão) e hash
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}

void writePng(const std::string& path, const uint32_t* px, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) { const uint32_t c = px[y * w + x]; raw.push_back(c & 255); raw.push_back((c >> 8) & 255); raw.push_back((c >> 16) & 255); }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t o = 0; o < raw.size(); o += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - o);
    z.push_back(o + n == raw.size() ? 1 : 0);
    z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
    z.insert(z.end(), raw.begin() + static_cast<long>(o), raw.begin() + static_cast<long>(o + n));
  }
  const uint32_t ad = (b << 16) | a;
  for (int k = 3; k >= 0; k--) z.push_back((ad >> (k * 8)) & 255);
  std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
    for (int k = 3; k >= 0; k--) f.push_back((d.size() >> (k * 8)) & 255);
    std::vector<uint8_t> td(t, t + 4);
    td.insert(td.end(), d.begin(), d.end());
    f.insert(f.end(), td.begin(), td.end());
    const uint32_t c = crc32(td.data(), td.size());
    for (int k = 3; k >= 0; k--) f.push_back((c >> (k * 8)) & 255);
  };
  std::vector<uint8_t> ih = {0, 0, uint8_t(w >> 8), uint8_t(w), 0, 0, uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
  chunk("IHDR", ih); chunk("IDAT", z); chunk("IEND", {});
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<long>(f.size()));
}


bool loadCore(const std::string& path, std::unique_ptr<indigo::pce::PceCore>& core) {
  std::vector<uint8_t> rom;
  if (!readFile(path, rom)) return false;
  std::string low = path;
  for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  try {
    core = indigo::pce::createPceCore(rom, low, nullptr);
  } catch (const std::exception& e) {
    std::printf("erro ao abrir %s: %s\n", path.c_str(), e.what());
    return false;
  }
  return true;
}

// ------------------------------------------------------------ testes do compilador HuC
// exit(n) é o opcode $63 com o código em X (ou um BRA para si mesmo, com X); abort() é o $E2 (como no tgemu)
// dump_screen() ($33): a tela é comparada com o BMP do tgemu (320x240 em RGB565, imagem de 256x240 em x=32),
// em 3 bits por cor, procurando o alinhamento vertical (o tgemu começa a imagem em outra linha)
bool compareBmp(indigo::pce::PceCore& core, const std::string& bmp) {
  std::vector<uint8_t> f;
  if (!readFile(bmp, f) || f.size() < 0x8a + 320 * 240 * 2) return false;
  auto ref = [&](int x, int y) {
    const size_t o = 0x8a + (size_t(239 - y) * 320 + 32 + x) * 2;
    const unsigned v = f[o] | (f[o + 1] << 8);
    return int(((v >> 13) << 6) | (((v >> 8) & 7) << 3) | ((v >> 2) & 7));  // R G B, 3 bits cada
  };
  const uint32_t* fb = core.framebuffer();
  const int w = core.width();
  if (w != 256) return false;
  auto mine = [&](int x, int y) {
    const uint32_t c = fb[y * w + x];
    return int((((c & 255) >> 5) << 6) | ((((c >> 8) & 255) >> 5) << 3) | (((c >> 16) & 255) >> 5));
  };
  for (int dy = -24; dy <= 24; dy++) {
    bool ok = true;
    for (int y = 16; y < 224 && ok; y++)
      for (int x = 0; x < 256 && ok; x++)
        if (y + dy >= 0 && y + dy < 240 && ref(x, y + dy) != mine(x, y)) ok = false;
    if (ok) return true;
  }
  return false;
}

int runHucTest(const std::string& path, int maxFrames) {
  std::unique_ptr<indigo::pce::PceCore> core;
  if (!loadCore(path, core)) return -2;
  auto& m = core->pce();
  for (int f = 0; f < maxFrames; f++) {
    const int64_t limit = m.cpu.clk + 1365LL * 263;
    while (m.cpu.clk < limit) {
      const uint8_t op = m.cpu.peek(m.cpu.pc);
      if (op == 0x33) {
        m.cpu.step();
        core->runFrame();
        core->runFrame();
        std::string base = path.substr(path.find_last_of('/') + 1);
        base = base.substr(base.find('-') + 1);
        base = base.substr(0, base.size() - 4);
        const std::string bmp = path.substr(0, path.find_last_of('/') + 1) + base + "-huc.bmp";
        return compareBmp(*core, bmp) ? 0 : 2000;
      }
      if (op == 0x63) return m.cpu.x;
      if (op == 0xE2) return 1000;
      if (op == 0x80 && m.cpu.peek(static_cast<uint16_t>(m.cpu.pc + 1)) == 0xFE) return m.cpu.x;  // BRA para si mesmo
      m.cpu.step();
    }
  }
  return -1;
}

void testHuc(const std::string& dir) {
  auto files = listDir(dir, ".pce");
  int pass = 0, fail = 0;
  for (auto& f : files) {
    const int r = runHucTest(dir + "/" + f, 600);
    if (r == 0) pass++;
    else { fail++; std::printf("  %s: %s\n", f.c_str(), r == 1000 ? "abort()" : r == 2000 ? "tela diferente do tgemu" : r == -1 ? "não terminou" : ("exit(" + std::to_string(r) + ")").c_str()); }
  }
  std::printf("Testes do compilador HuC: %d passaram, %d falharam\n", pass, fail);
  check(fail == 0 && pass > 0, "testes do compilador HuC");
}

// teclas de um roteiro "quadro:máscara,..." (cada máscara vale do quadro dado em diante)
uint32_t keysAt(const std::string& script, int frame) {
  uint32_t k = 0;
  size_t p = 0;
  while (p < script.size()) {
    const size_t c = script.find(':', p);
    if (c == std::string::npos) break;
    const int fr = std::atoi(script.c_str() + p);
    if (fr <= frame) k = static_cast<uint32_t>(std::strtoul(script.c_str() + c + 1, nullptr, 0));
    const size_t n = script.find(',', c);
    if (n == std::string::npos) break;
    p = n + 1;
  }
  return k;
}

// ------------------------------------------------------------ comparação com o Geargrafx
// mesmo hash do t7_pce_ref.c: FNV-1a das cores RGB565 (no modo de 512 pixels, uma linha de cada par)
uint32_t refHash(indigo::Core& c, int& w, int& h) {
  w = c.width();
  const int step = c.height() == 480 ? 2 : 1;
  h = c.height() / step;
  uint32_t hash = 2166136261u;
  const uint32_t* fb = c.framebuffer();
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const uint32_t p = fb[size_t(y * step) * w + x];
      const unsigned r = ((p & 255) * 7 + 127) / 255, g = (((p >> 8) & 255) * 7 + 127) / 255, b = (((p >> 16) & 255) * 7 + 127) / 255;
      hash ^= (g << 6) | (r << 3) | b;
      hash *= 16777619u;
    }
  return hash;
}

struct RefFrame { int w, h; uint32_t hash; };

// compara os quadros; o Geargrafx fecha o quadro em outro ponto da varredura, então se procura o melhor
// deslocamento de 0 a 2 quadros. Devolve o número de quadros iguais.
int compareRef(const std::string& rom, const std::string& refTxt, int frames, int& total, int& firstBad,
               const std::string& pngOnBad, const std::string& keys = "") {
  std::vector<RefFrame> ref;
  std::ifstream f(refTxt);
  int i; unsigned w, h; std::string hs;
  while (f >> i >> w >> h >> hs) ref.push_back({int(w), int(h), uint32_t(std::stoul(hs, nullptr, 16))});
  std::unique_ptr<indigo::pce::PceCore> core;
  total = 0; firstBad = -1;
  if (ref.empty() || !loadCore(rom, core)) return -1;
  std::vector<RefFrame> mine;
  for (int k = 0; k < frames && k < int(ref.size()); k++) {
    core->setKeys(keysAt(keys, k));
    core->runFrame();
    int mw, mh;
    const uint32_t hh = refHash(*core, mw, mh);
    mine.push_back({mw, mh, hh});
    if (!pngOnBad.empty() && k == frames - 1) writePng(pngOnBad, core->framebuffer(), core->width(), core->height());
  }
  int best = -1;
  for (int off = -2; off <= 2; off++) {
    int same = 0, bad = -1, n = 0;
    for (int k = 2; k < int(mine.size()); k++) {
      const int r = k + off;
      if (r < 0 || r >= int(ref.size())) continue;
      n++;
      if (mine[k].w == ref[r].w && mine[k].h == ref[r].h && mine[k].hash == ref[r].hash) same++;
      else if (bad < 0) bad = k;
    }
    if (same > best) { best = same; firstBad = bad; total = n; }
  }
  return best;
}

// casos com teclas (mesmo roteiro no t7_pce.sh): mudam a resolução (SELECT) nos testes de tempo do VDC, para
// cobrir os modos de 7,16 e 10,7 MHz, e jogam o shmup (START, direcional e tiro)
const char* const KEY_CASES[][3] = {
    {"rcrtest.pce", "sel", "60:4,64:0,250:4,254:0,400:8,404:0,450:128,460:0"},
    {"tiatest.pce", "sel", "60:4,64:0,250:4,254:0,400:64,404:0"},
    {"dmatest.pce", "sel", "60:128,64:0,200:4,204:0,350:4,354:0,500:16,504:0"},
    {"mwrtest.pce", "sel", "60:4,64:0,250:4,254:0,400:16,404:0"},
    {"shmup.pce", "jogo", "60:8,66:0,120:17,200:33,260:65,330:129,420:1,480:0"},
    {"shmup.sgx", "jogo", "60:8,66:0,120:17,200:33,260:65,330:129,420:1,480:0"},
    {"hugerom.pce", "jogo", "60:8,66:0,100:16,140:32,180:1,200:0"},
};

void testRoms(const std::string& dir, const std::string& pngDir) {
  struct Case { std::string rom, tag, keys; };
  std::vector<Case> cases;
  for (auto& r : listDir(dir + "/roms", ".pce")) cases.push_back({r, "", ""});
  for (auto& r : listDir(dir + "/roms", ".sgx")) cases.push_back({r, "", ""});
  for (auto& c : KEY_CASES) cases.push_back({c[0], std::string("-") + c[1], c[2]});
  for (auto& cs : cases) {
    const std::string& r = cs.rom;
    for (int k = 0; k < 2; k++) {
      const char* name = k == 0 ? "Mesen 2" : "Geargrafx";
      const std::string ref = dir + "/ref/" + r + cs.tag + (k == 0 ? ".mesen.txt" : ".txt");
      int total = 0, firstBad = -1;
      const int same = compareRef(dir + "/roms/" + r, ref, 600, total, firstBad,
                                  pngDir.empty() || k ? "" : pngDir + "/" + r + cs.tag + ".png", cs.keys);
      if (same < 0) { std::printf("  %s%s: sem referência do %s\n", r.c_str(), cs.tag.c_str(), name); continue; }
      std::printf("  %-24s %3d de %3d quadros iguais ao %s%s\n", (r + cs.tag).c_str(), same, total, name,
                  firstBad >= 0 ? (" (primeiro diferente: " + std::to_string(firstBad) + ")").c_str() : "");
      if (k == 0) check(same == total, r + cs.tag + ": quadros diferentes do Mesen 2");
    }
  }
}

// ------------------------------------------------------------ save state, BRAM e robustez
uint32_t frameHash(indigo::Core& c) {
  return crc32(reinterpret_cast<const uint8_t*>(c.framebuffer()), size_t(c.width()) * c.height() * 4);
}

void testStates(const std::string& path) {
  std::unique_ptr<indigo::pce::PceCore> cp;
  if (!loadCore(path, cp)) { std::printf("save state: %s não existe, pulando\n", path.c_str()); return; }
  indigo::pce::PceCore& core = *cp;
  core.setSampleRate(48000);
  std::vector<float> l(8192), r(8192);
  auto drain = [&] { while (core.audioAvailable()) core.pullAudio(l.data(), r.data(), std::min<size_t>(8192, core.audioAvailable())); };
  auto runN = [&](int n, uint32_t keys, std::vector<float>* audio) {
    for (int i = 0; i < n; i++) {
      core.setKeys((i / 7) % 3 == 0 ? keys : 0);
      core.runFrame();
      const size_t k = std::min<size_t>(core.audioAvailable(), 8192);
      core.pullAudio(l.data(), r.data(), k);
      if (audio) audio->insert(audio->end(), l.begin(), l.begin() + static_cast<long>(k));
    }
  };
  runN(200, indigo::KEY_START, nullptr);
  drain();
  const auto st = core.saveState();
  std::vector<float> a1, a2;
  runN(120, indigo::KEY_RIGHT | indigo::KEY_A, &a1);
  const uint32_t h1 = frameHash(core);
  core.loadState(st);
  drain();
  runN(120, indigo::KEY_RIGHT | indigo::KEY_A, &a2);
  check(frameHash(core) == h1, "save state: tela diferente depois de carregar");
  check(a1 == a2, "save state: som diferente depois de carregar");
  float peak = 0;
  for (float v : a1) peak = std::max(peak, std::fabs(v));
  check(a1.size() > 48000 && peak <= 1.0f, "som: sem amostras ou fora da escala");
  core.loadState(st);
  {
    const auto st2 = core.saveState();
    size_t d = 0;
    while (d < st.size() && d < st2.size() && st[d] == st2[d]) d++;
    if (st2 != st) std::printf("  primeiro byte diferente: %zu de %zu\n", d, st.size());
    check(st2 == st, "save state: salvar → carregar → salvar mudou bytes");
  }
  uint32_t seed = 12345;
  auto rnd = [&] { seed = seed * 1103515245u + 12345u; return seed >> 8; };
  int thrown = 0;
  for (int t = 0; t < 300; t++) {
    std::vector<uint8_t> bad = st;
    if (t % 3 == 0) bad.resize(rnd() % bad.size());
    else for (int k = 0; k < 1 + t % 40; k++) bad[rnd() % bad.size()] = static_cast<uint8_t>(rnd());
    try { core.loadState(bad); } catch (const std::runtime_error&) { thrown++; }
    core.runFrame();
    const auto again = core.saveState();
    core.loadState(again);
    if (core.saveState() != again) { check(false, "estado danificado: salvar → carregar → salvar mudou bytes"); break; }
  }
  core.loadState(st);
  std::printf("save state: determinístico (%zu bytes, som de pico %.2f); 300 estados danificados (%d recusados) sem travar\n",
              st.size(), peak, thrown);
  // ROMs de lixo (PC Engine e SuperGrafx, com e sem o mapeador do SF2)
  for (int t = 0; t < 40; t++) {
    std::vector<uint8_t> junk((t % 4 == 3) ? 0x280000 : (0x8000u << (t % 3)));
    for (auto& b : junk) b = static_cast<uint8_t>(rnd());
    try {
      indigo::pce::PceCore c(junk, nullptr, t % 2 == 1);
      for (int i = 0; i < 20; i++) { c.setKeys(rnd() & 0xFFF); c.runFrame(); }
      c.saveState();
    } catch (const std::runtime_error&) {}
  }
  std::printf("ROMs de lixo: 40 rodaram 20 quadros sem travar\n");
  check(true, "robustez");
}

// PSG: um programa mínimo toca uma onda quadrada no canal 1 com período $100 (3,58 MHz / 32 / 256 = 437 Hz)
void testPsg() {
  std::vector<uint8_t> rom(0x2000, 0xEA);
  const uint8_t prog[] = {
      0xA9, 0xFF, 0x53, 0x01,                                      // MPR0 = E/S
      0x9C, 0x00, 0x08,                                            // canal 1
      0xA9, 0xFF, 0x8D, 0x01, 0x08,                                // volume geral máximo
      0xA9, 0x00, 0x8D, 0x02, 0x08, 0xA9, 0x01, 0x8D, 0x03, 0x08,  // período $100
      0xA9, 0x1F, 0xA2, 0x10, 0x8D, 0x06, 0x08, 0xCA, 0xD0, 0xFA,  // 16 amostras altas
      0xA9, 0x00, 0xA2, 0x10, 0x8D, 0x06, 0x08, 0xCA, 0xD0, 0xFA,  // 16 baixas
      0xA9, 0xFF, 0x8D, 0x05, 0x08,                                // volume do canal
      0xA9, 0x9F, 0x8D, 0x04, 0x08,                                // liga, amplitude 31
      0x80, 0xFE};
  std::copy(prog, prog + sizeof prog, rom.begin());
  rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;
  indigo::pce::PceCore c(rom, nullptr);
  c.setSampleRate(48000);
  std::vector<float> l(8192), r(8192), all;
  for (int f = 0; f < 120; f++) {
    c.runFrame();
    const size_t k = std::min<size_t>(c.audioAvailable(), 8192);
    c.pullAudio(l.data(), r.data(), k);
    if (f >= 30) all.insert(all.end(), l.begin(), l.begin() + static_cast<long>(k));
  }
  int cross = 0;
  float peak = 0;
  for (size_t i = 1; i < all.size(); i++) {
    if ((all[i - 1] < 0) != (all[i] < 0)) cross++;
    peak = std::max(peak, std::fabs(all[i]));
  }
  const double hz = cross / 2.0 / (all.size() / 48000.0);
  std::printf("PSG: onda quadrada de %.1f Hz (esperado 437,0), pico %.2f\n", hz, peak);
  check(std::fabs(hz - 3579545.45 / 32 / 256) < 4 && peak > 0.1f && peak <= 1.0f, "PSG: frequência ou volume errados");
}

// BRAM: um programa mínimo libera a BRAM ($1807), escreve nela e trava ($1803); o save tem de mudar
void testBram() {
  std::vector<uint8_t> rom(0x2000, 0xEA);
  const uint8_t prog[] = {
      0xA9, 0xF7, 0x53, 0x10,              // LDA #$F7; TAM #$10 (MPR4 = BRAM, $8000)
      0xA9, 0xFF, 0x53, 0x01,              // LDA #$FF; TAM #$01 (MPR0 = E/S)
      0xA9, 0x80, 0x8D, 0x07, 0x18,        // LDA #$80; STA $1807 (libera)
      0xA9, 0x5A, 0x8D, 0x10, 0x80,        // LDA #$5A; STA $8010
      0xAD, 0x03, 0x18,                    // LDA $1803 (trava)
      0xA9, 0xA5, 0x8D, 0x11, 0x80,        // LDA #$A5; STA $8011 (travada: não grava)
      0x80, 0xFE};                         // BRA *
  std::copy(prog, prog + sizeof prog, rom.begin());
  rom[0x1FFE] = 0x00; rom[0x1FFF] = 0xE0;  // reset em $E000 (MPR7 = banco 0)
  indigo::pce::PceCore c(rom, nullptr);
  c.runFrame();
  const auto sv = c.exportSave();
  check(c.hasSave() && sv.size() == 2048 && sv[0x10] == 0x5A && sv[0x11] == 0x00 && sv[0] == 'H' && sv[3] == 'M',
        "BRAM: escrita liberada/travada");
  std::printf("BRAM: liberar, gravar e travar funcionam (%s)\n", sv.size() == 2048 && sv[0x10] == 0x5A ? "ok" : "falhou");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("uso: %s pasta [cpu|huc|roms|state]\n", argv[0]); return 1; }
  const std::string dir = argv[1];
  const std::string what = argc > 2 ? argv[2] : "";
  if (what == "shot" && argc > 3) {  // ferramenta: indigo-t7_pce rom.pce shot saida.png [quadros] [teclas]
    std::unique_ptr<indigo::pce::PceCore> core;
    if (!loadCore(dir, core)) return 1;
    const int frames = argc > 4 ? std::atoi(argv[4]) : 300;
    const std::string keys = argc > 5 ? argv[5] : "";
    for (int i = 0; i < frames; i++) { core->setKeys(keysAt(keys, i)); core->runFrame(); }
    writePng(argv[3], core->framebuffer(), core->width(), core->height());
    std::printf("%dx%d, %d quadros\n", core->width(), core->height(), frames);
    return 0;
  }
  DIR* d = opendir(dir.c_str());
  if (!d) { std::printf("pasta %s não existe: testes do PC Engine pulados\n", dir.c_str()); return 0; }
  closedir(d);
  if (what.empty() || what == "cpu") testCpu(dir + "/huc6280/v1");
  if (what.empty() || what == "huc") testHuc(dir + "/huc-tests");
  if (what.empty() || what == "roms") testRoms(dir, argc > 3 ? argv[3] : "");
  if (what.empty() || what == "state") { testStates(dir + "/roms/shmup.pce"); testStates(dir + "/roms/shmup.sgx"); testBram(); testPsg(); }
  std::printf("RESULTADO pce: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
