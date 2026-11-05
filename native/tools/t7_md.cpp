// Testes do Mega Drive (pasta de testes como argumento, normalmente /root/indigo-env/roms/md):
//  1. CPU 68000 contra os SingleStepTests (680x0/68000/v1): registradores, SR, PC, fila de pré-busca, memória
//     e o número total de ciclos de cada instrução (inclusive exceções de erro de endereço).
//  2. (mais abaixo) ROMs de teste e comparação de quadros com o Genesis Plus GX.
//  3. Dois jogadores: o controle 2 chega à porta 2.
// Uso: indigo-t7_md pasta [cpu [arquivo]]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/backup.h"
#include "../src/core/zip.h"
#include "../src/md/m68k.h"
#include "../src/md/md.h"

using indigo::backup::Json;

namespace {

long g_checks = 0, g_fails = 0;

void check(bool ok, const std::string& what) {
  g_checks++;
  if (!ok) { g_fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool gunzip(const std::vector<uint8_t>& gz, std::vector<uint8_t>& out) {
  if (gz.size() < 18 || gz[0] != 0x1F || gz[1] != 0x8B || gz[2] != 8) return false;
  const int flg = gz[3];
  size_t p = 10;
  if (flg & 4) { if (p + 2 > gz.size()) return false; p += 2 + (gz[p] | (gz[p + 1] << 8)); }
  if (flg & 8) { while (p < gz.size() && gz[p]) p++; p++; }
  if (flg & 16) { while (p < gz.size() && gz[p]) p++; p++; }
  if (flg & 2) p += 2;
  if (p >= gz.size() - 8) return false;
  const size_t n = gz[gz.size() - 4] | (gz[gz.size() - 3] << 8) | (gz[gz.size() - 2] << 16) | (size_t(gz[gz.size() - 1]) << 24);
  return indigo::zip::inflate(gz.data() + p, gz.size() - 8 - p, out, n);
}

// ------------------------------------------------------------ SingleStepTests do 68000
struct TestBus : indigo::md::M68kBus {
  std::vector<uint8_t> mem = std::vector<uint8_t>(1 << 24);
  uint8_t read8(uint32_t a) override { return mem[a]; }
  uint16_t read16(uint32_t a) override { return static_cast<uint16_t>((mem[a] << 8) | mem[a + 1]); }
  void write8(uint32_t a, uint8_t v) override { mem[a] = v; }
  void write16(uint32_t a, uint16_t v) override { mem[a] = static_cast<uint8_t>(v >> 8); mem[a + 1] = static_cast<uint8_t>(v); }
};

const char* REGN[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "a5", "a6"};

uint32_t num(const Json& j, const char* k) { return static_cast<uint32_t>(static_cast<int64_t>(j.num(k))); }

void setState(indigo::md::M68k& cpu, TestBus& bus, const Json& s) {
  for (int i = 0; i < 8; i++) cpu.d[i] = num(s, REGN[i]);
  for (int i = 0; i < 7; i++) cpu.a[i] = num(s, REGN[8 + i]);
  cpu.sMode = 1; cpu.a[7] = num(s, "ssp"); cpu.otherSp = num(s, "usp");
  cpu.setSR(static_cast<uint16_t>(num(s, "sr")));
  cpu.pc = num(s, "pc");
  const Json* pf = s.get("prefetch");
  cpu.ir = static_cast<uint16_t>(pf->a[0].n); cpu.irc = static_cast<uint16_t>(pf->a[1].n);
  for (const Json& r : s.get("ram")->a) bus.mem[static_cast<uint32_t>(r.a[0].n) & 0xFFFFFF] = static_cast<uint8_t>(r.a[1].n);
  cpu.stopped = cpu.halted = false; cpu.ipl = 0;
}

// devolve a descrição da primeira diferença ("" = igual)
std::string compare(indigo::md::M68k& cpu, TestBus& bus, const Json& s, int cycles, int length) {
  char buf[160];
  for (int i = 0; i < 8; i++)
    if (cpu.d[i] != num(s, REGN[i])) { std::snprintf(buf, sizeof buf, "%s=%08X esperado %08X", REGN[i], cpu.d[i], num(s, REGN[i])); return buf; }
  for (int i = 0; i < 7; i++)
    if (cpu.a[i] != num(s, REGN[8 + i])) { std::snprintf(buf, sizeof buf, "%s=%08X esperado %08X", REGN[8 + i], cpu.a[i], num(s, REGN[8 + i])); return buf; }
  if (cpu.usp() != num(s, "usp")) { std::snprintf(buf, sizeof buf, "usp=%08X esperado %08X", cpu.usp(), num(s, "usp")); return buf; }
  if (cpu.ssp() != num(s, "ssp")) { std::snprintf(buf, sizeof buf, "ssp=%08X esperado %08X", cpu.ssp(), num(s, "ssp")); return buf; }
  if (cpu.getSR() != num(s, "sr")) { std::snprintf(buf, sizeof buf, "sr=%04X esperado %04X", cpu.getSR(), num(s, "sr")); return buf; }
  if (cpu.pc != num(s, "pc")) { std::snprintf(buf, sizeof buf, "pc=%08X esperado %08X", cpu.pc, num(s, "pc")); return buf; }
  const Json* pf = s.get("prefetch");
  if (cpu.ir != static_cast<uint16_t>(pf->a[0].n) || cpu.irc != static_cast<uint16_t>(pf->a[1].n)) {
    std::snprintf(buf, sizeof buf, "fila=%04X,%04X esperado %04X,%04X", cpu.ir, cpu.irc, static_cast<unsigned>(pf->a[0].n), static_cast<unsigned>(pf->a[1].n));
    return buf;
  }
  for (const Json& r : s.get("ram")->a) {
    const uint32_t ad = static_cast<uint32_t>(r.a[0].n) & 0xFFFFFF;
    if (bus.mem[ad] != static_cast<uint8_t>(r.a[1].n)) {
      std::snprintf(buf, sizeof buf, "mem[%06X]=%02X esperado %02X", ad, bus.mem[ad], static_cast<unsigned>(r.a[1].n));
      return buf;
    }
  }
  if (cycles != length) { std::snprintf(buf, sizeof buf, "ciclos=%d esperado %d", cycles, length); return buf; }
  return "";
}

void cpuTests(const std::string& dir, const std::string& only) {
  static const char* FILES[] = {
      "ABCD", "ADD.b", "ADD.l", "ADD.w", "ADDA.l", "ADDA.w", "ADDX.b", "ADDX.l", "ADDX.w", "AND.b", "AND.l", "AND.w",
      "ANDItoCCR", "ANDItoSR", "ASL.b", "ASL.l", "ASL.w", "ASR.b", "ASR.l", "ASR.w", "BCHG", "BCLR", "BSET", "BSR",
      "BTST", "Bcc", "CHK", "CLR.b", "CLR.l", "CLR.w", "CMP.b", "CMP.l", "CMP.w", "CMPA.l", "CMPA.w", "DBcc", "DIVS",
      "DIVU", "EOR.b", "EOR.l", "EOR.w", "EORItoCCR", "EORItoSR", "EXG", "EXT.l", "EXT.w", "JMP", "JSR", "LEA", "LINK",
      "LSL.b", "LSL.l", "LSL.w", "LSR.b", "LSR.l", "LSR.w", "MOVE.b", "MOVE.l", "MOVE.q", "MOVE.w", "MOVEA.l",
      "MOVEA.w", "MOVEM.l", "MOVEM.w", "MOVEP.l", "MOVEP.w", "MOVEfromSR", "MOVEfromUSP", "MOVEtoCCR", "MOVEtoSR",
      "MOVEtoUSP", "MULS", "MULU", "NBCD", "NEG.b", "NEG.l", "NEG.w", "NEGX.b", "NEGX.l", "NEGX.w", "NOP", "NOT.b",
      "NOT.l", "NOT.w", "OR.b", "OR.l", "OR.w", "ORItoCCR", "ORItoSR", "PEA", "RESET", "ROL.b", "ROL.l", "ROL.w",
      "ROR.b", "ROR.l", "ROR.w", "ROXL.b", "ROXL.l", "ROXL.w", "ROXR.b", "ROXR.l", "ROXR.w", "RTE", "RTR", "RTS",
      "SBCD", "SUB.b", "SUB.l", "SUB.w", "SUBA.l", "SUBA.w", "SUBX.b", "SUBX.l", "SUBX.w", "SWAP", "Scc", "TAS",
      "TRAP", "TRAPV", "TST.b", "TST.l", "TST.w", "UNLINK"};
  const std::string base = dir + "/680x0/68000/v1/";
  std::vector<uint8_t> probe;
  if (!readFile(base + "NOP.json.gz", probe)) { std::printf("SingleStepTests do 68000: pulado (sem %s)\n", base.c_str()); return; }
  // 3 testes com estado final impossível (ASL.b mudando os 24 bits de cima do registrador; DIVU por zero
  // empilhando o PC 4 bytes atrás, diferente dos outros ~200 casos iguais): ficam de fora, com o motivo
  static const std::set<std::string> SKIP = {"e502 [ASL.b Q, D2] 1583", "e502 [ASL.b Q, D2] 1761",
                                             "80ef [DIVU (d16, A7), D0] 5745"};
  TestBus bus;
  indigo::md::M68k cpu(bus);
  long total = 0, bad = 0, skipped = 0;
  for (const char* f : FILES) {
    if (!only.empty() && only != f) continue;
    std::vector<uint8_t> gz, raw;
    if (!readFile(base + f + ".json.gz", gz) || !gunzip(gz, raw)) { check(false, std::string("ler ") + f); continue; }
    Json root;
    if (!indigo::backup::parseJson(std::string(raw.begin(), raw.end()), root)) { check(false, std::string("JSON ") + f); continue; }
    long fileBad = 0;
    std::string first;
    for (const Json& t : root.a) {
      if (SKIP.count(t.str("name"))) { skipped++; continue; }
      const Json& in = *t.get("initial");
      const Json& out = *t.get("final");
      setState(cpu, bus, in);
      for (const Json& r : out.get("ram")->a) (void)r;
      const int cyc = cpu.step();
      const std::string diff = compare(cpu, bus, out, cyc, static_cast<int>(t.num("length")));
      if (!diff.empty()) {
        if (!fileBad) first = t.str("name") + ": " + diff;
        if (!only.empty() && fileBad < 12) std::printf("    %s: %s\n", t.str("name").c_str(), diff.c_str());
        fileBad++;
      }
      // limpa a memória tocada
      for (const Json& r : in.get("ram")->a) bus.mem[static_cast<uint32_t>(r.a[0].n) & 0xFFFFFF] = 0;
      for (const Json& r : out.get("ram")->a) bus.mem[static_cast<uint32_t>(r.a[0].n) & 0xFFFFFF] = 0;
      total++;
    }
    bad += fileBad;
    if (fileBad) std::printf("  %-12s %5ld de %zu diferentes (ex.: %s)\n", f, fileBad, root.a.size(), first.c_str());
    // a memória de 16 MB pode ter ficado suja por escritas fora da lista: zera tudo a cada arquivo
    std::fill(bus.mem.begin(), bus.mem.end(), 0);
  }
  std::printf("68000 (SingleStepTests): %ld testes, %ld diferentes (%ld com estado impossível, pulados)\n", total, bad, skipped);
  check(bad == 0, "68000: todos os SingleStepTests iguais");
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

bool loadRom(const std::string& path, std::vector<uint8_t>& rom) { return readFile(path, rom); }

// ------------------------------------------------------------ comparação com o Genesis Plus GX
// lista.txt: nome rom quadros passo teclas; ref/<nome>.ref: para cada quadro amostrado, largura e altura (u16) e
// os níveis 0..15 de R, G, B de cada ponto.
void refTests(const std::string& dir, const std::string& only, const std::string& pngDir) {
  std::ifstream lst(dir + "/lista.txt");
  if (!lst) { std::printf("comparação com o Genesis Plus GX: pulada (sem %s/lista.txt)\n", dir.c_str()); return; }
  std::string nome, romPath;
  int quadros, passo;
  unsigned long teclas;
  std::string linhaLista;
  while (std::getline(lst, linhaLista)) {
    std::istringstream ls(linhaLista);
    // mínimo de quadros iguais (%): 100, menos nas ROMs que mostram medidas de tempo (contadores, carga da CPU):
    // a CPU do Índigo segue os ciclos medidos no 68000 de verdade (SingleStepTests) e o GPGX usa outra tabela
    int minimo = 100;
    const int folga = 0;
    if (!(ls >> nome >> romPath >> quadros >> passo >> teclas)) continue;
    ls >> minimo;
    if (!only.empty() && only != nome) continue;
    std::vector<uint8_t> rom, ref;
    if (!readFile(romPath[0] == '/' ? romPath : dir + "/" + romPath, rom) || !readFile(dir + "/ref/" + nome + ".ref", ref)) {
      check(false, nome + ": faltam arquivos");
      continue;
    }
    indigo::md::MDCore core(rom, nullptr);
    core.setSampleRate(48000);
    std::vector<float> l(8192), r(8192);
    std::vector<uint32_t> prev;
    int prevW = 0, prevH = 0;
    size_t pos = 0;
    int iguais = 0, total = 0, primeiro = -1;
    long difPrimeiro = 0;
    for (int f = 0; f < quadros; f++) {
      core.setKeys((teclas && (f / 30) % 2) ? static_cast<uint32_t>(teclas) : 0);
      if (folga) {
        prevW = core.width(); prevH = core.height();
        prev.assign(core.framebuffer(), core.framebuffer() + prevW * prevH);
      }
      core.runFrame();
      size_t n = core.audioAvailable();
      while (n) { const size_t k = std::min<size_t>(n, 8192); core.pullAudio(l.data(), r.data(), k); n -= k; }
      if ((f + 1) % passo) continue;
      total++;
      if (pos + 4 > ref.size()) break;
      const int w = ref[pos] | (ref[pos + 1] << 8), h = ref[pos + 2] | (ref[pos + 3] << 8);
      pos += 4;
      const uint8_t* rp = &ref[pos];
      pos += static_cast<size_t>(w) * h * 3;
      if (pos > ref.size()) break;
      long dif = 0;
      if (w != core.width() || h != core.height()) {
        dif = -1;
        if (primeiro < 0) std::printf("    tamanho: gpgx %dx%d, índigo %dx%d\n", w, h, core.width(), core.height());
      }
      else {
        const uint32_t* fb = core.framebuffer();
        for (int i = 0; i < w * h; i++) {
          const uint32_t c = fb[i];
          const int cr = ((c & 255) * 14 + 127) / 255, cg = (((c >> 8) & 255) * 14 + 127) / 255, cb = (((c >> 16) & 255) * 14 + 127) / 255;
          if (cr != rp[i * 3] || cg != rp[i * 3 + 1] || cb != rp[i * 3 + 2]) {
            if (!pngDir.empty() && primeiro < 0 && dif < 6)
              std::printf("    (%d,%d) gpgx %d,%d,%d índigo %d,%d,%d\n", i % w, i / w, rp[i * 3], rp[i * 3 + 1], rp[i * 3 + 2], cr, cg, cb);
            dif++;
          }
        }
      }
      if (dif != 0 && folga && prevW == w && prevH == h) {
        long d2 = 0;
        for (int i = 0; i < w * h && d2 == 0; i++) {
          const uint32_t c = prev[i];
          const int cr = ((c & 255) * 14 + 127) / 255, cg = (((c >> 8) & 255) * 14 + 127) / 255, cb = (((c >> 16) & 255) * 14 + 127) / 255;
          if (cr != rp[i * 3] || cg != rp[i * 3 + 1] || cb != rp[i * 3 + 2]) d2++;
        }
        if (d2 == 0) dif = 0;
      }
      if (std::getenv("VERB")) std::printf("    quadro %d: %ld\n", f + 1, dif);
      if (dif == 0) iguais++;
      else if (primeiro < 0) {
        primeiro = f + 1; difPrimeiro = dif;
        if (!pngDir.empty()) {
          writePng(pngDir + "/" + nome + "-indigo.png", core.framebuffer(), core.width(), core.height());
          if (w > 0 && h > 0) {
            std::vector<uint32_t> px(static_cast<size_t>(w) * h);
            for (int i = 0; i < w * h; i++)
              px[i] = 0xFF000000u | (rp[i * 3] * 17u) | (rp[i * 3 + 1] * 17u << 8) | (rp[i * 3 + 2] * 17u << 16);
            writePng(pngDir + "/" + nome + "-gpgx.png", px.data(), w, h);
          }
        }
      }
    }
    char buf[200];
    if (primeiro < 0) std::snprintf(buf, sizeof buf, "%s: %d de %d quadros iguais ao Genesis Plus GX", nome.c_str(), iguais, total);
    else if (minimo < 100) std::snprintf(buf, sizeof buf, "%s: %d de %d quadros iguais (mínimo %d%%: sensível a poucos ciclos de CPU)", nome.c_str(), iguais, total, minimo);
    else std::snprintf(buf, sizeof buf, "%s: %d de %d quadros iguais (primeira diferença no quadro %d: %ld pontos%s)",
                       nome.c_str(), iguais, total, primeiro, difPrimeiro, difPrimeiro < 0 ? ", tamanho" : "");
    std::printf("  %s\n", buf);
    check(primeiro < 0 || iguais * 100 >= minimo * total, buf);
  }
}

// ------------------------------------------------------------ som comparado com o Genesis Plus GX
// envelope de volume (RMS a cada 1/60 s) com até 3 quadros de deslocamento: correlação >= 0,9 e volume médio
// entre 0,8 e 1,25 vezes o do GPGX (os sons não são idênticos amostra a amostra: o YM2612 é outra implementação)
void audioTests(const std::string& dir) {
  std::ifstream lst(dir + "/lista.txt");
  std::string linha;
  while (std::getline(lst, linha)) {
    std::istringstream ls(linha);
    std::string nome, romPath;
    if (!(ls >> nome >> romPath)) continue;
    std::vector<uint8_t> rom, ref;
    if (!readFile(dir + "/ref/" + nome + ".aud", ref)) continue;
    if (!readFile(romPath[0] == '/' ? romPath : dir + "/" + romPath, rom)) continue;
    indigo::md::MDCore core(rom, nullptr);
    core.setSampleRate(44100);
    std::vector<float> mine;
    std::vector<float> l(8192), r(8192);
    for (int f = 0; f < 900; f++) {
      core.runFrame();
      size_t n = core.audioAvailable();
      while (n) {
        const size_t k = std::min<size_t>(n, 8192);
        core.pullAudio(l.data(), r.data(), k);
        for (size_t i = 0; i < k; i++) mine.push_back(l[i] * 32767.0f);
        n -= k;
      }
    }
    const size_t W = 735;
    std::vector<double> ea, eb;
    for (size_t i = 0; i + W <= ref.size() / 4 && i + W <= mine.size(); i += W) {
      double sa = 0, sb = 0;
      for (size_t k = 0; k < W; k++) {
        const int16_t v = static_cast<int16_t>(ref[(i + k) * 4] | (ref[(i + k) * 4 + 1] << 8));
        sa += double(v) * v;
        sb += double(mine[i + k]) * mine[i + k];
      }
      ea.push_back(std::sqrt(sa / W)); eb.push_back(std::sqrt(sb / W));
    }
    double best = -1;
    for (int sh = -3; sh <= 3; sh++) {
      double ma = 0, mb = 0; int n = 0;
      for (size_t i = 3; i + 3 < ea.size(); i++) { ma += ea[i]; mb += eb[i + sh]; n++; }
      if (!n) continue;
      ma /= n; mb /= n;
      double c = 0, va = 0, vb = 0;
      for (size_t i = 3; i + 3 < ea.size(); i++) {
        const double x = ea[i] - ma, y = eb[i + sh] - mb;
        c += x * y; va += x * x; vb += y * y;
      }
      if (va > 0 && vb > 0) best = std::max(best, c / std::sqrt(va * vb));
    }
    double ta = 0, tb = 0;
    for (size_t i = 0; i < ea.size(); i++) { ta += ea[i]; tb += eb[i]; }
    const double ratio = ta > 0 ? tb / ta : 0;
    char buf[200];
    std::snprintf(buf, sizeof buf, "som de %s: correlação do envelope %.3f, volume %.2f do GPGX", nome.c_str(), best, ratio);
    std::printf("  %s\n", buf);
    check(best >= 0.9 && ratio > 0.8 && ratio < 1.25, buf);
  }
}

// ------------------------------------------------------------ cartuchos montados aqui, estados e ROMs de lixo
std::vector<uint8_t> makeRom(size_t size, const std::vector<uint16_t>& code, const char* region = "JUE") {
  std::vector<uint8_t> r(size, 0xFF);
  auto w32 = [&](size_t a, uint32_t v) { r[a] = uint8_t(v >> 24); r[a + 1] = uint8_t(v >> 16); r[a + 2] = uint8_t(v >> 8); r[a + 3] = uint8_t(v); };
  w32(0, 0x00FFFE00); w32(4, 0x200);
  std::memcpy(&r[0x100], "SEGA MEGA DRIVE ", 16);
  std::memcpy(&r[0x150], "TESTE DO INDIGO", 15);
  std::memset(&r[0x1F0], ' ', 16);
  std::memcpy(&r[0x1F0], region, std::strlen(region));
  for (size_t i = 0; i < code.size(); i++) { r[0x200 + i * 2] = uint8_t(code[i] >> 8); r[0x201 + i * 2] = uint8_t(code[i]); }
  return r;
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

// dois jogadores: o controle 2 (6 botões com setKeys2) chega à porta 2 sem mexer na 1
void player2Tests() {
  indigo::md::MDCore c(makeRom(0x10000, {0x60FE}), nullptr);
  auto& m = c.md();
  check(c.players() == 2, "2 jogadores: players() == 2");
  c.setKeys(indigo::KEY_LEFT);
  m.write8(0xA1000B, 0x40);  // TH da porta 2 como saída
  m.write8(0xA10005, 0x40);
  check((m.read8(0xA10005) & 0x7F) == 0x7F, "2 jogadores: sem setKeys2 o controle 2 fica solto");
  c.setKeys2(indigo::KEY_UP | indigo::KEY_A | indigo::KEY_X);
  check((m.read8(0xA10005) & 0x7F) == 0x6E, "2 jogadores: porta 2 com cima e B do controle 2");
  check((m.read8(0xA10003) & 0x7F) == 0x7B, "2 jogadores: porta 1 só com o controle 1");
  for (int k = 0; k < 5; k++) m.write8(0xA10005, (k & 1) ? 0x40 : 0x00);
  check((m.read8(0xA10005) & 0x7F) == 0x30, "2 jogadores: controle 2 se identifica como de 6 botões");
  m.write8(0xA10005, 0x40);
  check((m.read8(0xA10005) & 0x7F) == 0x6D, "2 jogadores: botão Y do controle 2 (6 botões)");
}

void cartTests() {
  {  // SRAM do cabeçalho em 0x200001
    auto rom = makeRom(0x10000, {0x13FC, 0x005A, 0x0020, 0x0001, 0x13FC, 0x00A5, 0x0020, 0x0003, 0x1039, 0x0020, 0x0001,
                                 0x13C0, 0x00FF, 0x0000, 0x60FE});
    std::memcpy(&rom[0x1B0], "RA\xF8\x20", 4);
    rom[0x1B4] = 0x00; rom[0x1B5] = 0x20; rom[0x1B6] = 0x00; rom[0x1B7] = 0x01;
    rom[0x1B8] = 0x00; rom[0x1B9] = 0x20; rom[0x1BA] = 0x3F; rom[0x1BB] = 0xFF;
    indigo::md::MDCore c(rom, nullptr);
    c.runFrame(); c.runFrame();
    const auto sv = c.exportSave();
    check(c.hasSave() && sv.size() >= 4 && sv[1] == 0x5A && sv[3] == 0xA5 && c.md().workRam()[0] == 0x5A, "SRAM pelo cabeçalho (0x200001)");
    indigo::md::MDCore c2(rom, &sv);
    check(c2.exportSave() == sv, "SRAM importada no começo");
  }
  {  // mapeador do Super Street Fighter II (5 MB)
    auto rom = makeRom(5u << 20, {0x1039, 0x0008, 0x0000, 0x13C0, 0x00FF, 0x0000, 0x13FC, 0x0009, 0x00A1, 0x30F3,
                                  0x1039, 0x0008, 0x0000, 0x13C0, 0x00FF, 0x0001, 0x60FE});
    std::memcpy(&rom[0x100], "SEGA SSF        ", 16);
    rom[0x080000] = 0x11; rom[0x480000] = 0x77;
    indigo::md::MDCore c(rom, nullptr);
    c.runFrame(); c.runFrame();
    check(c.md().workRam()[0] == 0x11 && c.md().workRam()[1] == 0x77, "mapeador SSF2: página 9 em 0x080000");
  }
  // região: registrador de versão e NTSC/PAL
  const std::vector<uint16_t> ver = {0x1039, 0x00A1, 0x0001, 0x13C0, 0x00FF, 0x0000, 0x60FE};
  const struct { const char* reg; uint8_t v; bool pal; } R[] = {{"JUE", 0xA0, false}, {"J", 0x20, false}, {"E", 0xE0, true}, {"4", 0xA0, false}, {"8", 0xE0, true}};
  for (const auto& t : R) {
    auto rom = makeRom(0x10000, ver, t.reg);
    indigo::md::MDCore c(rom, nullptr);
    c.runFrame(); c.runFrame();
    const double fps = c.fps();
    check(c.md().workRam()[0] == t.v && c.md().pal == t.pal && (t.pal ? std::fabs(fps - 49.70) < 0.01 : std::fabs(fps - 59.92) < 0.01),
          std::string("região ") + t.reg);
  }
  {  // V30 (240 linhas) só no PAL
    const std::vector<uint16_t> v30 = {0x33FC, 0x817C, 0x00C0, 0x0004, 0x60FE};
    indigo::md::MDCore pal(makeRom(0x10000, v30, "E"), nullptr), ntsc(makeRom(0x10000, v30, "U"), nullptr);
    for (int f = 0; f < 3; f++) { pal.runFrame(); ntsc.runFrame(); }
    check(pal.height() == 240 && ntsc.height() == 224 && pal.width() == 256, "V30 no PAL (256x240), V28 no NTSC");
  }
  // .smd intercalado
  {
    auto rom = makeRom(0x8000, ver, "U");
    std::vector<uint8_t> smd(512 + rom.size());
    smd[8] = 0xAA; smd[9] = 0xBB;
    for (size_t b = 0; b < rom.size(); b += 16384)
      for (int i = 0; i < 8192; i++) { smd[512 + b + i] = rom[b + i * 2 + 1]; smd[512 + b + 8192 + i] = rom[b + i * 2]; }
    check(indigo::md::mdNormalize(smd) == rom && indigo::md::isMdRom(smd, "jogo.smd") && indigo::md::isMdRom(rom, "x.bin") &&
              !indigo::md::isMdRom(std::vector<uint8_t>(0x8000, 0), "x.bin"),
          "formato .smd desintercalado e detecção");
  }
}

void stateTests(const std::string& dir) {
  std::vector<uint8_t> rom;
  if (!readFile(dir + "/sgdk/sample/game/sonic/out/release/rom.bin", rom)) rom = makeRom(0x10000, {0x60FE});
  indigo::md::MDCore c(rom, nullptr);
  c.setSampleRate(48000);
  std::vector<float> l(8192), r(8192);
  std::vector<uint64_t> trace;
  auto run = [&](int n, uint32_t keys) {
    uint64_t h = 1469598103934665603ull;
    trace.clear();
    for (int f = 0; f < n; f++) {
      c.setKeys((f / 20) % 2 ? keys : 0);
      c.runFrame();
      h = fnv(c.framebuffer(), size_t(c.width()) * c.height() * 4, h);
      trace.push_back(h);
      size_t k = c.audioAvailable();
      while (k) { const size_t m = std::min<size_t>(k, 8192); c.pullAudio(l.data(), r.data(), m); h = fnv(l.data(), m * 4, h); k -= m; }
      trace.push_back(h);
    }
    return h;
  };
  run(200, 17);
  const auto st = c.saveState();
  const uint64_t a = run(120, 17);
  const auto ta = trace;
  c.loadState(st);
  const auto st2 = c.saveState();
  const uint64_t b = run(120, 17);
  for (size_t i = 0; i < ta.size(); i++)
    if (ta[i] != trace[i]) { std::printf("  estado: primeira diferença no quadro %zu (%s)\n", i / 2, (i & 1) ? "som" : "imagem"); break; }
  check(a == b, "save state determinístico (imagem e som iguais depois de carregar)");
  check(st == st2, "salvar -> carregar -> salvar dá os mesmos bytes");
  // estados danificados: truncados e com bytes trocados
  std::mt19937 rng(1234);
  int ok = 0;
  for (int i = 0; i < 300; i++) {
    std::vector<uint8_t> bad = st;
    if (i % 3 == 0) bad.resize(rng() % bad.size());
    else for (int k = 0; k < 1 + int(rng() % 40); k++) bad[rng() % bad.size()] ^= uint8_t(1 + rng() % 255);
    try { c.loadState(bad); } catch (const std::exception&) {}
    run(3, 0xFFF);
    ok++;
  }
  check(ok == 300, "300 estados danificados sem travar");
  // ROMs de lixo (com e sem cabeçalho)
  int roms = 0;
  for (int i = 0; i < 40; i++) {
    std::vector<uint8_t> g(size_t(512) << (rng() % 13));
    for (auto& v : g) v = uint8_t(rng());
    if (i & 1) std::memcpy(&g[0x100], "SEGA", 4);
    if (i % 5 == 0 && g.size() >= 0x10000) { g[4] = 0; g[5] = 0; g[6] = 0x02; g[7] = 0x00; }
    try {
      indigo::md::MDCore gc(g, nullptr);
      for (int f = 0; f < 20; f++) { gc.setKeys(uint32_t(rng())); gc.runFrame(); }
      gc.saveState();
    } catch (const std::exception&) {}
    roms++;
  }
  check(roms == 40, "40 ROMs de lixo sem travar");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("uso: indigo-t7_md pasta [cpu [arquivo]]\n"); return 2; }
  const std::string dir = argv[1];
  if (argc >= 4 && std::string(argv[2]) == "shot") {  // ferramenta: indigo-t7_md rom.bin shot saida.png [quadros] [teclas]
    std::vector<uint8_t> rom;
    if (!loadRom(dir, rom)) { std::printf("não abriu %s\n", dir.c_str()); return 1; }
    indigo::md::MDCore core(rom, nullptr);
    core.setSampleRate(44100);
    const int frames = argc > 4 ? std::atoi(argv[4]) : 300;
    const uint32_t keys = argc > 5 ? static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 0)) : 0;
    std::vector<float> l(4096), r(4096);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++) {
      core.setKeys((keys && (f / 30) % 2) ? keys : 0);
      core.runFrame();
      size_t n = core.audioAvailable();
      static FILE* aud = std::getenv("AUDOUT") ? std::fopen(std::getenv("AUDOUT"), "wb") : nullptr;
      while (n) {
        const size_t k = std::min<size_t>(n, 4096);
        core.pullAudio(l.data(), r.data(), k);
        if (aud) for (size_t i = 0; i < k; i++) { int16_t s2[2] = {static_cast<int16_t>(l[i] * 32767), static_cast<int16_t>(r[i] * 32767)}; std::fwrite(s2, 2, 2, aud); }
        n -= k;
      }
      if (const char* ro = std::getenv("RAMOUT")) {
        std::ofstream o(ro, f ? std::ios::binary | std::ios::app : std::ios::binary);
        o.write(reinterpret_cast<const char*>(core.md().workRam().data()), 65536);
      }
      if (argc > 6) {  // soma dos níveis de cada quadro (para comparar com o refmd)
        long sum = 0;
        const uint32_t* fb = core.framebuffer();
        for (int i = 0; i < core.width() * core.height(); i++)
          for (int k = 0; k < 3; k++) sum += (((fb[i] >> (8 * k)) & 255) * 14 + 127) / 255;
        std::printf("%d %d %d %ld;", f + 1, core.width(), core.height(), sum);
      }
    }
    if (argc > 6) std::printf("\n");
    if (const char* vo = std::getenv("VDPOUT")) {
      auto& v = core.md().vdp;
      std::ofstream o(vo, std::ios::binary);
      o.write(reinterpret_cast<const char*>(v.vram.data()), 65536);
      o.write(reinterpret_cast<const char*>(v.reg), 32);
      o.write(reinterpret_cast<const char*>(v.vsram), 80);
      o.write(reinterpret_cast<const char*>(v.cram), 128);
    }
    writePng(argv[3], core.framebuffer(), core.width(), core.height());
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d quadros em %.2f s (%.0f quadros/s)\n", frames, secs, frames / (secs > 0 ? secs : 1));
    std::printf("%s: %dx%d, título \"%s\", pc=%06X\n", argv[3], core.width(), core.height(), core.title().c_str(), core.md().cpu.pc);
    return 0;
  }
  const std::string part = argc > 2 ? argv[2] : "";
  std::vector<uint8_t> probe;
  std::ifstream test(dir);
  if (!test.good() && !std::ifstream(dir + "/.").good()) {
    std::printf("pasta %s não existe: testes do Mega Drive pulados\n", dir.c_str());
    std::printf("RESULTADO md: 0 verificações, 0 falhas\n");
    return 0;
  }
  if (part.empty() || part == "cpu") cpuTests(dir, argc > 3 ? argv[3] : "");
  if (part.empty() || part == "cart") { cartTests(); player2Tests(); }
  if (part.empty() || part == "estado") stateTests(dir);
  if (part.empty() || part == "som") audioTests(dir);
  if (part.empty() || part == "ref") refTests(dir, argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
  std::printf("RESULTADO md: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
