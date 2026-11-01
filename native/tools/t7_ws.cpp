// Testes do WonderSwan (pasta de testes como argumento, normalmente /root/indigo-env/roms/ws):
//  1. CPU contra os SingleStepTests do NEC V20 (v20/v1_native_binary, formato MOO): registradores e memória.
//     O V20 é o parente mais próximo do V30MZ com testes gerados em hardware; as diferenças conhecidas
//     (instruções extras do V20, bandeiras indefinidas, opcodes não documentados) ficam de fora com o motivo.
//  2. ROM WSCPUTest (Fredrik Ahlström): testa no "hardware" emulado as bandeiras indefinidas, divisões, BCD e
//     opcodes não documentados do V30MZ, comparando com o que o console real faz.
//  3. ROM WSHWTest (mesmo autor), como Color e como monocromático: interrupções, temporizadores, bits de
//     cada porta de E/S, ruído; telas das ROMs de janela/prioridade de sprites do MiSTer fixadas por hash.
//  4. Save state determinístico, estados danificados e ROMs de lixo.
// Uso: indigo-t7_ws pasta [cpu|roms|hw|state] [opcode | pasta-dos-png]
//      ("roms" inclui a WSCPUTest inteira: ~8 min; "hw" é só a parte rápida)
//      indigo-t7_ws rom.ws shot saida.png [quadros]   (ferramenta: tira um PNG)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/backup.h"
#include "../src/core/zip.h"
#include "../src/ws/v30mz.h"
#include "../src/ws/ws.h"

using indigo::backup::Json;
using indigo::ws::V30MZ;

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

// ------------------------------------------------------------ SingleStepTests (MOO)
struct TestBus : indigo::ws::V30Bus {
  std::vector<uint8_t> mem = std::vector<uint8_t>(1 << 20);
  uint8_t memRead(uint32_t a) override { return mem[a & 0xFFFFF]; }
  void memWrite(uint32_t a, uint8_t v) override { mem[a & 0xFFFFF] = v; }
  uint8_t ioRead(uint16_t) override { return 0xFF; }
  void ioWrite(uint16_t, uint8_t) override {}
  int memTiming(uint32_t) override { return 1 | 16; }
  int ioTiming(uint16_t) override { return 1 | 16; }
};

uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

struct Regs { uint16_t mask = 0; uint16_t v[14] = {}; };
struct MooTest {
  std::string name;
  std::vector<uint8_t> bytes;
  Regs init, fin;
  std::vector<std::pair<uint32_t, uint8_t>> ramInit, ramFin;
};

void parseRegs(const uint8_t* p, size_t n, Regs& r) {
  if (n < 2) return;
  r.mask = static_cast<uint16_t>(p[0] | (p[1] << 8));
  size_t o = 2;
  for (int i = 0; i < 14; i++)
    if ((r.mask >> i) & 1) { if (o + 2 <= n) r.v[i] = static_cast<uint16_t>(p[o] | (p[o + 1] << 8)); o += 2; }
}

void parseState(const uint8_t* p, size_t n, Regs& regs, std::vector<std::pair<uint32_t, uint8_t>>& ram) {
  size_t o = 0;
  while (o + 8 <= n) {
    const std::string id(reinterpret_cast<const char*>(p + o), 4);
    const uint32_t len = u32(p + o + 4);
    const uint8_t* d = p + o + 8;
    if (id == "REGS") parseRegs(d, len, regs);
    else if (id == "RAM ") {
      const uint32_t cnt = u32(d);
      for (uint32_t k = 0; k < cnt; k++) ram.push_back({u32(d + 4 + k * 5), d[4 + k * 5 + 4]});
    }
    o += 8 + len;
  }
}

bool parseMoo(const std::vector<uint8_t>& f, std::vector<MooTest>& tests) {
  size_t o = 0;
  while (o + 8 <= f.size()) {
    const std::string id(reinterpret_cast<const char*>(&f[o]), 4);
    const uint32_t len = u32(&f[o + 4]);
    if (o + 8 + len > f.size()) return false;
    if (id == "TEST") {
      MooTest t;
      const uint8_t* p = &f[o + 8];
      size_t q = 4;
      while (q + 8 <= len) {
        const std::string sid(reinterpret_cast<const char*>(p + q), 4);
        const uint32_t sl = u32(p + q + 4);
        const uint8_t* d = p + q + 8;
        if (sid == "NAME") t.name.assign(reinterpret_cast<const char*>(d + 4), u32(d));
        else if (sid == "BYTS") t.bytes.assign(d + 4, d + 4 + u32(d));
        else if (sid == "INIT") parseState(d, sl, t.init, t.ramInit);
        else if (sid == "FINA") parseState(d, sl, t.fin, t.ramFin);
        q += 8 + sl;
      }
      tests.push_back(std::move(t));
    }
    o += 8 + len;
  }
  return true;
}

// ordem do MOO: ax bx cx dx cs ss ds es sp bp si di ip flags
void setRegs(V30MZ& c, const Regs& r) {
  c.r[V30MZ::AX] = r.v[0]; c.r[V30MZ::BX] = r.v[1]; c.r[V30MZ::CX] = r.v[2]; c.r[V30MZ::DX] = r.v[3];
  c.sreg[V30MZ::CS] = r.v[4]; c.sreg[V30MZ::SS] = r.v[5]; c.sreg[V30MZ::DS] = r.v[6]; c.sreg[V30MZ::ES] = r.v[7];
  c.r[V30MZ::SP] = r.v[8]; c.r[V30MZ::BP] = r.v[9]; c.r[V30MZ::SI] = r.v[10]; c.r[V30MZ::DI] = r.v[11];
  c.ip = r.v[12]; c.flags = r.v[13] & 0x8FD5;
}
uint16_t getReg(const V30MZ& c, int i) {
  switch (i) {
    case 0: return c.r[V30MZ::AX]; case 1: return c.r[V30MZ::BX]; case 2: return c.r[V30MZ::CX]; case 3: return c.r[V30MZ::DX];
    case 4: return c.sreg[V30MZ::CS]; case 5: return c.sreg[V30MZ::SS]; case 6: return c.sreg[V30MZ::DS]; case 7: return c.sreg[V30MZ::ES];
    case 8: return c.r[V30MZ::SP]; case 9: return c.r[V30MZ::BP]; case 10: return c.r[V30MZ::SI]; case 11: return c.r[V30MZ::DI];
    case 12: return c.ip; default: return c.psw();
  }
}
const char* REGN[14] = {"ax", "bx", "cx", "dx", "cs", "ss", "ds", "es", "sp", "bp", "si", "di", "ip", "flags"};

// opcodes do V20 que o V30MZ não tem ou faz diferente (ficam de fora do teste; o comportamento do V30MZ é
// coberto pela ROM WSCPUTest)
std::map<std::string, std::string> skipped() {
  std::map<std::string, std::string> s;
  s["0F"] = "instruções extras do V20 (no V30MZ, 0F é NOP de 1 byte)";
  s["D6"] = "no V20 é não documentado; no V30MZ é SALC (testado pela WSCPUTest)";
  for (int i = 0xD8; i <= 0xDF; i++) { char b[4]; std::snprintf(b, sizeof b, "%02X", i); s[b] = "FPO1: o V20 decodifica ModRM completo; no V30MZ é NOP de 2 bytes"; }
  s["9B"] = "POLL: no V20 depende do pino; no WonderSwan nunca espera";
  for (const char* k : {"C0.6", "C1.6", "D0.6", "D1.6", "D2.6", "D3.6"}) s[k] = "SHL não documentado (/6): o V30MZ zera o operando";
  for (int i = 1; i < 8; i++) {
    s["8F." + std::to_string(i)] = "POP r/m com reg≠0 (indefinido)";
    s["C6." + std::to_string(i)] = "MOV r/m,imm com reg≠0 (indefinido)";
    s["C7." + std::to_string(i)] = "MOV r/m,imm com reg≠0 (indefinido)";
  }
  for (int i = 2; i < 8; i++) s["FE." + std::to_string(i)] = "INC/DEC byte com reg≥2: o V30MZ repete o grupo FF (indefinido no V20)";
  s["FF.7"] = "FF /7 indefinido";
  s["F6.1"] = "TEST não documentado (/1): o V30MZ não lê o imediato";
  s["F7.1"] = "TEST não documentado (/1): o V30MZ não lê o imediato";
  s["27"] = "DAA: o V30MZ testa AL ≥ 9Ah antes do ajuste baixo (difere do V20 com AL 9Ah-9Fh; WSCPUTest cobre)";
  s["2F"] = "DAS: idem ao DAA";
  s["63"] = "no V20 decodifica ModRM; no V30MZ é NOP de 1 byte";
  return s;
}

int runSst(const std::string& dir, const std::string& only) {
  std::vector<uint8_t> metaRaw;
  Json meta;
  if (!readFile(dir + "/metadata.json", metaRaw) ||
      !indigo::backup::parseJson(std::string(metaRaw.begin(), metaRaw.end()), meta)) {
    std::printf("SingleStepTests do V20: pasta %s sem metadata.json, pulando\n", dir.c_str());
    return 0;
  }
  const Json* ops = meta.get("opcodes");
  const auto skip = skipped();
  std::vector<std::pair<std::string, int>> files;  // nome do arquivo, máscara de bandeiras
  for (const auto& [op, info] : ops->o) {
    auto add = [&](const std::string& key, const Json& j) {
      const std::string st = j.str("status");
      if (st == "prefix" || st == "fpu") return;
      int mask = static_cast<int>(j.num("flags-mask", 0xFFFF));
      files.push_back({key, mask});
    };
    if (const Json* reg = info.get("reg")) {
      for (const auto& [r, j] : reg->o) add(op + "." + r, j);
    } else add(op, info);
  }
  long total = 0, bad = 0, skippedTests = 0;
  int badFiles = 0;
  TestBus bus;
  V30MZ cpu(bus);
  for (const auto& [key, flagMask] : files) {
    if (!only.empty() && key.rfind(only, 0) != 0) continue;
    if (skip.count(key) || skip.count(key.substr(0, 2))) {
      const std::string why = skip.count(key) ? skip.at(key) : skip.at(key.substr(0, 2));
      std::printf("  pulado %-5s %s\n", key.c_str(), why.c_str());
      continue;
    }
    std::vector<uint8_t> gz, raw;
    if (!readFile(dir + "/" + key + ".MOO.gz", gz)) {
      // opcodes do V20 que só existem em extensão 0F (0F10...) ficam fora do metadata principal
      continue;
    }
    std::vector<MooTest> tests;
    if (!gunzip(gz, raw) || !parseMoo(raw, tests)) { std::printf("FALHA: %s.MOO.gz ilegível\n", key.c_str()); bad++; continue; }
    long fileBad = 0;
    std::string first;
    const bool isDiv = key == "F6.6" || key == "F6.7" || key == "F7.6" || key == "F7.7" || key == "D4";
    const int op = static_cast<int>(std::strtol(key.substr(0, 2).c_str(), nullptr, 16));
    for (const MooTest& t : tests) {
      // diferenças conhecidas, caso a caso: prefixos REPC/REPNC (64/65, só do V20); contagem de deslocamento
      // ≥ 32 (o V30MZ usa só os 5 bits baixos); AAM com divisor 0 (o V30MZ não altera AX); AAD com base ≠ 10
      // (o V20 ignora o imediato, o V30MZ usa)
      size_t pre = 0;
      bool rc = false;
      while (pre < t.bytes.size() && (t.bytes[pre] == 0x26 || t.bytes[pre] == 0x2E || t.bytes[pre] == 0x36 ||
                                      t.bytes[pre] == 0x3E || t.bytes[pre] == 0xF2 || t.bytes[pre] == 0xF3 ||
                                      t.bytes[pre] == 0x64 || t.bytes[pre] == 0x65 || t.bytes[pre] == 0xF0)) {
        rc |= t.bytes[pre] == 0x64 || t.bytes[pre] == 0x65;
        pre++;
      }
      int cnt = -1;
      if ((op == 0xD2 || op == 0xD3)) cnt = t.init.v[2] & 0xFF;
      if ((op == 0xC0 || op == 0xC1) && !t.bytes.empty()) cnt = t.bytes.back();
      const int imm = t.bytes.empty() ? -1 : t.bytes.back();
      if (rc || cnt >= 32 || (op == 0xD4 && imm == 0) || (op == 0xD5 && imm != 10)) { skippedTests++; continue; }
      // o V20 só usa os 4 bits baixos da contagem de rotação até 31; com contagem ≥ 32 depois do & 0x1F os dois
      // fazem igual, então não há o que pular aqui
      for (const auto& [a, v] : t.ramInit) bus.mem[a & 0xFFFFF] = v;
      cpu.power();
      setRegs(cpu, t.init);
      cpu.flush();
      int guard = 0;
      do { cpu.step(); } while (cpu.prefixActive && ++guard < 1000000);
      std::string why;
      for (int i = 0; i < 14 && why.empty(); i++) {
        if (!((t.fin.mask >> i) & 1)) continue;
        uint16_t got = getReg(cpu, i), want = t.fin.v[i];
        if (i == 13) { got &= flagMask & 0x0FD5; want &= flagMask & 0x0FD5; }
        if (got != want) { char b[96]; std::snprintf(b, sizeof b, "%s=%04X (esperado %04X)", REGN[i], got, want); why = b; }
      }
      // exceção de divisão: as bandeiras empilhadas têm bits indefinidos diferentes no V20
      uint32_t skipA = 0xFFFFFFFF;
      if (isDiv && ((t.fin.mask >> 8) & 1)) skipA = ((uint32_t(cpu.sreg[V30MZ::SS]) << 4) + uint16_t(cpu.r[V30MZ::SP] + 4)) & 0xFFFFF;
      for (const auto& [a, v] : t.ramFin) {
        if (!why.empty()) break;
        if (a == skipA || a == ((skipA + 1) & 0xFFFFF)) continue;
        if (bus.mem[a & 0xFFFFF] != v) { char b[96]; std::snprintf(b, sizeof b, "mem[%05X]=%02X (esperado %02X)", a, bus.mem[a & 0xFFFFF], v); why = b; }
      }
      // limpa a memória usada
      for (const auto& [a, v] : t.ramInit) bus.mem[a & 0xFFFFF] = 0;
      for (const auto& [a, v] : t.ramFin) bus.mem[a & 0xFFFFF] = 0;
      total++;
      if (!why.empty()) { fileBad++; char ib[48]; std::snprintf(ib, sizeof ib, " [ax=%04X f=%04X]", t.init.v[0], t.init.v[13]); if (first.empty()) first = t.name + ": " + why + ib; }
    }
    if (fileBad) {
      badFiles++;
      bad += fileBad;
      std::printf("  %-5s %ld/%zu falhas (1ª: %s)\n", key.c_str(), fileBad, tests.size(), first.c_str());
    }
  }
  std::printf("SingleStepTests V20: %ld casos (%ld pulados por diferença conhecida), %ld falhas, %d arquivos com falha\n", total, skippedTests, bad, badFiles);
  check(total > 0, "SingleStepTests V20: nenhum caso rodou");
  check(bad == 0, "SingleStepTests V20: " + std::to_string(bad) + " casos com falha");
  return 0;
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

uint32_t frameHash(indigo::Core& c) {
  return crc32(reinterpret_cast<const uint8_t*>(c.framebuffer()), size_t(c.width()) * c.height() * 4);
}

// ------------------------------------------------------------ ROMs de teste com texto na tela
// O texto sai por INT 10h (um caractere em AL); o teste junta as linhas. "Expected" = falha (aperta B para
// pular para o próximo teste); "Ok" = passou.
struct TextRun {
  std::vector<std::string> lines;
  long chars = 0;
  std::string cur;
  static void hook(void* ctx, uint8_t vec, uint8_t al) {
    auto* t = static_cast<TextRun*>(ctx);
    if (vec != 0x10) return;
    t->chars++;
    if (al == 10) { t->lines.push_back(t->cur); t->cur.clear(); }
    else if (al >= 32 && al < 127) t->cur += static_cast<char>(al);
  }
};

void runTextRom(const std::string& label, const std::string& path, int downs, int maxFrames, const std::string& png,
                const std::set<std::string>& knownFails, bool forceMono = false) {
  std::vector<uint8_t> rom;
  if (!readFile(path, rom)) { std::printf("%s: %s não existe (rode t7_ws.sh), pulando\n", label.c_str(), path.c_str()); return; }
  if (forceMono) rom[rom.size() - 9] &= 0xFE;  // rodapé: roda no WonderSwan monocromático
  indigo::ws::WSCore core(rom, nullptr);
  TextRun tr;
  core.ws().cpu.swiHook = &TextRun::hook;
  core.ws().cpu.swiCtx = &tr;
  int f = 0;
  auto run = [&](uint32_t keys, int n) { for (int i = 0; i < n; i++, f++) { core.setKeys(keys); core.runFrame(); } };
  run(0, 60);
  for (int i = 0; i < downs; i++) { run(indigo::KEY_DOWN, 4); run(0, 4); }
  run(indigo::KEY_A, 4); run(0, 4);
  size_t seen = 0;
  long seenChars = 0;
  int quiet = 0;
  while (f < maxFrames && quiet < 900) {
    run(0, 30);
    if (tr.chars == seenChars) quiet += 30; else quiet = 0;
    seenChars = tr.chars;
    bool failed = false;
    for (size_t i = seen; i < tr.lines.size(); i++) failed |= (tr.lines[i].find("Expected") != std::string::npos || tr.lines[i].find("Fail") != std::string::npos);
    if (failed) { run(0, 8); run(indigo::KEY_B, 8); run(0, 8); }
    seen = tr.lines.size();
  }
  if (!png.empty()) writePng(png, core.framebuffer(), core.width(), core.height());
  int ok = 0, bad = 0;
  std::string name;
  for (size_t i = 0; i < tr.lines.size(); i++) {
    const std::string& l = tr.lines[i];
    if (l.rfind("Ok", 0) == 0) { ok++; continue; }
    if (l.find("Expected") != std::string::npos || l.find("Fail") != std::string::npos) {
      bad++;
      const bool known = knownFails.count(name) > 0;
      std::string detail = name;
      for (size_t k = i > 0 ? i - 1 : 0; k < std::min(tr.lines.size(), i + 3); k++) detail += " | " + tr.lines[k];
      std::printf("  %s %s\n", known ? "falha conhecida:" : "FALHA:", detail.c_str());
      if (!known) check(false, label + ": " + name);
      continue;
    }
    if (l.find("Input") == std::string::npos && l.find("Tested") == std::string::npos &&
        l.find("Value") == std::string::npos && l.find("Flags") == std::string::npos && !l.empty() &&
        l.find("0x") == std::string::npos)
      name = l;
  }
  std::printf("%s: %d partes Ok, %d falhas (%d quadros)\n", label.c_str(), ok, bad, f);
  check(ok > 0, label + ": nenhum teste terminou");
}

// ------------------------------------------------------------ save state e robustez
void testStates(const std::string& path) {
  std::vector<uint8_t> rom;
  if (!readFile(path, rom)) { std::printf("save state: %s não existe, pulando\n", path.c_str()); return; }
  indigo::ws::WSCore core(rom, nullptr);
  core.setSampleRate(48000);
  std::vector<float> l(4096), r(4096);
  auto drain = [&] { while (core.audioAvailable()) core.pullAudio(l.data(), r.data(), std::min<size_t>(4096, core.audioAvailable())); };
  auto runN = [&](int n, uint32_t keys, std::vector<float>* audio) {
    for (int i = 0; i < n; i++) {
      core.setKeys((i / 7) % 3 == 0 ? keys : 0);
      core.runFrame();
      const size_t k = core.audioAvailable();
      core.pullAudio(l.data(), r.data(), std::min<size_t>(k, 4096));
      if (audio) audio->insert(audio->end(), l.begin(), l.begin() + static_cast<long>(std::min<size_t>(k, 4096)));
    }
  };
  runN(100, indigo::KEY_A, nullptr);
  drain();
  const auto st = core.saveState();
  std::vector<float> a1, a2;
  runN(90, indigo::KEY_DOWN | indigo::KEY_A, &a1);
  const uint32_t h1 = frameHash(core);
  core.loadState(st);
  drain();
  runN(90, indigo::KEY_DOWN | indigo::KEY_A, &a2);
  check(frameHash(core) == h1, "save state: tela diferente depois de carregar");
  check(a1 == a2, "save state: som diferente depois de carregar");
  core.loadState(st);
  check(core.saveState() == st, "save state: salvar → carregar → salvar mudou bytes");
  // estados danificados: truncados e com bytes trocados
  uint32_t seed = 12345;
  auto rnd = [&] { seed = seed * 1103515245u + 12345u; return seed >> 8; };
  int thrown = 0;
  for (int t = 0; t < 300; t++) {
    std::vector<uint8_t> bad = st;
    if (t % 3 == 0) bad.resize(rnd() % bad.size());
    else for (int k = 0; k < 1 + t % 40; k++) bad[rnd() % bad.size()] = static_cast<uint8_t>(rnd());
    try { core.loadState(bad); } catch (const std::runtime_error&) { thrown++; }
    core.runFrame();
  }
  core.loadState(st);
  std::printf("save state: determinístico (%zu bytes); 300 estados danificados (%d recusados) sem travar\n", st.size(), thrown);
  // ROMs de lixo
  for (int t = 0; t < 40; t++) {
    std::vector<uint8_t> junk(0x10000u << (t % 3));
    for (auto& b : junk) b = static_cast<uint8_t>(rnd());
    try {
      indigo::ws::WSCore c(junk, nullptr);
      for (int i = 0; i < 20; i++) { c.setKeys(rnd() & 0xFFF); c.runFrame(); }
      c.saveState();
    } catch (const std::runtime_error&) {}
  }
  std::printf("ROMs de lixo: 40 rodaram 20 quadros sem travar\n");
  check(true, "robustez");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta-de-testes [cpu|roms] [opcode]\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  const std::string mode = argc > 2 ? argv[2] : "";
  const std::string only = argc > 3 ? argv[3] : "";
  std::ifstream probe(dir);
  if (!probe.good()) {
    std::printf("Pasta %s não existe: testes do WonderSwan pulados.\nRESULTADO ws: 0 verificações, 0 falhas\n", dir.c_str());
    return 0;
  }
  if (mode == "shot") {  // ferramenta: indigo-t7_ws rom.ws shot saida.png [quadros]
    std::vector<uint8_t> rom;
    readFile(dir, rom);
    indigo::ws::WSCore core(rom, nullptr);
    const int n = argc > 4 ? std::atoi(argv[4]) : 300;
    for (int i = 0; i < n; i++) core.runFrame();
    writePng(only, core.framebuffer(), core.width(), core.height());
    std::printf("%s: %08X %dx%d %s\n", core.system().c_str(), frameHash(core), core.width(), core.height(), core.note().c_str());
    return 0;
  }
  if (mode.empty() || mode == "cpu") runSst(dir + "/v20/v1_native_binary", only);
  if (mode.empty() || mode == "roms") {
  }
  if (mode.empty() || mode == "roms" || mode == "hw") {
    runTextRom("WSHWTest (Color)", dir + "/WSHWTest/WSHWTest.wsc", 1, 200000, only.empty() ? "" : only + "/wshwtest.png", {});
    runTextRom("WSHWTest (mono)", dir + "/WSHWTest/WSHWTest.wsc", 1, 200000, only.empty() ? "" : only + "/wshwtest-mono.png", {}, true);
  }
  if (mode.empty() || mode == "roms") {
    runTextRom("WSCPUTest", dir + "/WSCPUTest/WSCpuTest.wsc", 0, 2000000, only.empty() ? "" : only + "/wscputest.png", {});
  }
  if (mode.empty() || mode == "roms" || mode == "hw") {
    // telas conferidas no PNG (janela da tela 2 em 50-100 × 20-60; sprite 1 atrás da tela 2, sprite 2 na frente)
    // e fixadas por hash
    const struct { const char* rom; uint32_t hash; } shots[] = {
        {"/mister/testroms/windowtest/windowtest.ws", 0x51F5FBB5u},
        {"/mister/testroms/spritepriority/spritepriority.ws", 0x13E9046Cu},
    };
    for (const auto& s : shots) {
      std::vector<uint8_t> rom;
      if (!readFile(dir + s.rom, rom)) { std::printf("tela: %s não existe, pulando\n", s.rom); continue; }
      indigo::ws::WSCore core(rom, nullptr);
      for (int i = 0; i < 200; i++) core.runFrame();
      const uint32_t h = frameHash(core);
      std::printf("tela %s: %08X\n", s.rom, h);
      check(h == s.hash, std::string("tela diferente da conferida: ") + s.rom);
    }
  }
  if (mode.empty() || mode == "state") testStates(dir + "/WSHWTest/WSHWTest.wsc");
  std::printf("RESULTADO ws: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
