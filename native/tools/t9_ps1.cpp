// Testes do PlayStation (etapa 9: núcleo central — CPU, memória, DMA, timers, SIO0, GPU por software).
// Pasta de testes como argumento (normalmente /root/indigo-env/roms/ps1, montada pelo t9_ps1.sh). Réguas:
//  1. Testes de CPU do PeterLemon (PASS/FAIL na tela), carregados direto sem BIOS e pela BIOS de teste própria.
//  2. BIOS de teste do Índigo (t9_ps1_bios.s): partida, TTY, interrupções, carga do .exe depois do "menu".
//  3. SIO0 pelos registradores: controle digital/analógico (DualShock), controle 2 (digital, na porta 2), vibração
//     e cartão de memória (comandos de leitura, escrita e identificação, flag de cartão novo, o cartão como save do
//     jogo).
//  4. Save state: ida e volta igual, determinismo, estados danificados recusados sem mexer no jogo.
//  5. Com a BIOS do usuário (pulado se não existir: <pasta>/../../bios-joao/scph1001.bin ou PS1_BIOS):
//     - partida até o menu (logo da Sony e menu), comparada com o Beetle PSX (ref/menu.ppm);
//     - ps1-tests com o psx.log do console de verdade (cpu, dma, gpu): nenhum "fail" e os mesmos "pass";
//     - cpu/access-time (ciclos de leitura de cada região) e cpu/io-access-bitwidth (escritas de 8/16/32 bits nos
//       registradores) comparados com o psx.log do console; timers (contador com volta a 0 no alvo, leituras por valor);
//     - ps1-tests de GPU com o vram.png do console de verdade: VRAM igual ponto a ponto;
//     - ps1-tests de GPU com o quadro final do Beetle PSX (ref/gpu-*.ppm);
//     - gpu/bandwidth: tempos de desenho a menos de 12% dos medidos no console.
// Uso: indigo-t9_ps1 pasta
//      indigo-t9_ps1 run programa.exe quadros [direct|bios] [saida.ppm]   (ferramenta; PS1_BIOS escolhe a BIOS)
//      indigo-t9_ps1 bios quadros [saida.ppm]
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/ps1/ps1.h"
#include "../src/ps1/system.h"

using namespace indigo;
using ps1::Ps1Core;

namespace {

long g_checks = 0, g_fails = 0;
void check(bool ok, const std::string& what) {
  g_checks++;
  if (!ok) { g_fails++; if (g_fails < 300) std::printf("FALHA: %s\n", what.c_str()); }
}

std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

struct Image { int w = 0, h = 0; std::vector<uint8_t> rgb; };
bool readPpm(const std::string& path, Image& im) {
  const auto d = readAll(path);
  if (d.size() < 16 || d[0] != 'P' || d[1] != '6') return false;
  size_t p = 2;
  int vals[3] = {0, 0, 0}, n = 0;
  while (n < 3 && p < d.size()) {
    while (p < d.size() && std::isspace(d[p])) p++;
    if (p < d.size() && d[p] == '#') { while (p < d.size() && d[p] != '\n') p++; continue; }
    int v = 0;
    while (p < d.size() && std::isdigit(d[p])) v = v * 10 + (d[p++] - '0');
    vals[n++] = v;
  }
  p++;
  im.w = vals[0]; im.h = vals[1];
  if (d.size() < p + size_t(im.w) * im.h * 3) return false;
  im.rgb.assign(d.begin() + long(p), d.begin() + long(p + size_t(im.w) * im.h * 3));
  return true;
}
Image frameOf(const Core& c) {
  Image im;
  im.w = c.width(); im.h = c.height();
  im.rgb.resize(size_t(im.w) * im.h * 3);
  const uint32_t* f = c.framebuffer();
  for (int i = 0; i < im.w * im.h; i++) {
    im.rgb[size_t(i) * 3] = uint8_t(f[i]); im.rgb[size_t(i) * 3 + 1] = uint8_t(f[i] >> 8); im.rgb[size_t(i) * 3 + 2] = uint8_t(f[i] >> 16);
  }
  return im;
}
void writePpm(const Image& im, const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
  std::fwrite(im.rgb.data(), 1, im.rgb.size(), f);
  std::fclose(f);
}
uint32_t frameHash(const Core& c) {
  uint32_t h = 2166136261u;
  const uint32_t* p = c.framebuffer();
  for (int i = 0; i < c.width() * c.height(); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}
// fração de pontos iguais (5 bits por canal) no melhor deslocamento de a dentro de b (bordas do Beetle)
double similarity(const Image& a, const Image& b) {
  if (a.w > b.w || a.h > b.h || !a.w || !a.h) return 0;
  double best = 0;
  for (int dy = 0; dy <= b.h - a.h; dy++)
    for (int dx = 0; dx <= b.w - a.w; dx++) {
      long eq = 0;
      for (int y = 0; y < a.h; y++) {
        const uint8_t* pa = &a.rgb[size_t(y) * a.w * 3];
        const uint8_t* pb = &b.rgb[(size_t(y + dy) * b.w + dx) * 3];
        for (int x = 0; x < a.w * 3; x += 3)
          eq += (pa[x] >> 3) == (pb[x] >> 3) && (pa[x + 1] >> 3) == (pb[x + 1] >> 3) && (pa[x + 2] >> 3) == (pb[x + 2] >> 3);
      }
      best = std::max(best, double(eq) / (a.w * a.h));
    }
  return best;
}
std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find('\n', p);
    if (e == std::string::npos) e = s.size();
    std::string l = s.substr(p, e - p);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (l.rfind("% ", 0) == 0) l = l.substr(2);
    out.push_back(l);
    p = e + 1;
  }
  return out;
}
int countPrefix(const std::vector<std::string>& v, const char* pre) {
  int n = 0;
  for (const auto& l : v) n += l.rfind(pre, 0) == 0;
  return n;
}
std::string hex(const std::vector<uint8_t>& v) {
  std::string s;
  char b[4];
  for (uint8_t x : v) { std::snprintf(b, sizeof b, "%02X ", x); s += b; }
  return s;
}

// ---------------------------------------------------------------- SIO0 pelos registradores
struct Sio {
  ps1::System& s;
  void ctrl(uint32_t v) { s.ioWrite(0x1F80104A, v, 2); }
  uint8_t xfer(uint8_t b, bool* ack = nullptr) {
    s.istat &= ~(1u << 7);
    s.ioWrite(0x1F80104A, (s.sio.read(0xA, 2) & 0x2000) | 0x1013, 2);  // reconhece e mantém TX, /JOY, IRQ do /ACK
    s.ioWrite(0x1F801040, b, 1);
    s.now += 1200; s.sio.sync();
    s.now += 400; s.sio.sync();
    if (ack) *ack = (s.istat >> 7) & 1;
    return uint8_t(s.ioRead(0x1F801040, 1));
  }
  std::vector<uint8_t> cmd(const std::vector<uint8_t>& out, std::vector<bool>* acks = nullptr) {
    ctrl(0x1003);
    std::vector<uint8_t> r;
    for (uint8_t b : out) { bool a; r.push_back(xfer(b, &a)); if (acks) acks->push_back(a); }
    ctrl(0);
    return r;
  }
};

void testSio(const std::vector<uint8_t>& testBios, const std::vector<uint8_t>& anyExe) {
  Ps1Core core(anyExe, "sio.exe", nullptr, Ps1Core::ExeBoot::Bios, &testBios);
  for (int f = 0; f < 3; f++) core.runFrame();
  Sio io{core.sys()};
  // controle digital (ID 41h); A = Círculo (bit 13), Cima = bit 4
  core.setKeys(KEY_A | KEY_UP);
  std::vector<bool> acks;
  auto r = io.cmd({0x01, 0x42, 0x00, 0x00, 0x00}, &acks);
  check(r == std::vector<uint8_t>({0xFF, 0x41, 0x5A, 0xEF, 0xDF}), "controle digital: " + hex(r));
  check(acks == std::vector<bool>({true, true, true, true, false}), "controle digital: /ACK em todos menos o último");
  {
    // o /ACK é um pulso: o JOY_STAT.7 sobe e volta a 0 sozinho, sem outro byte. O remendo da biblioteca do cartão
    // de memória no tratador de interrupção do BIOS espera esse bit cair (NASCAR 2001 travava no logo)
    auto& y = core.sys();
    for (uint8_t dev : {uint8_t(0x01), uint8_t(0x81)}) {  // controle e cartão de memória
      io.ctrl(0x1003);
      y.ioWrite(0x1F801040, dev, 1);
      int high = 0, firstHigh = -1;
      bool lowAfter = false;
      for (int t = 0; t < 4000; t += 8) {
        y.now += 8;
        const bool b = (y.ioRead(0x1F801044, 4) >> 7) & 1;
        if (b) { high += 8; if (firstHigh < 0) firstHigh = t; }
        else if (firstHigh >= 0) lowAfter = true;
      }
      io.ctrl(0);
      check(firstHigh >= 0 && lowAfter && high >= 50 && high <= 200,
            std::string(dev == 1 ? "controle" : "cartão") + ": /ACK em pulso curto (" + std::to_string(high) +
                " ciclos em 1, volta a 0 sozinho)");
    }
  }
  // modo analógico pelo botão (ID 73h) e os direcionais
  core.pressAnalogButton();
  core.setAnalog(0x10, 0x20, 0x30, 0x40);
  r = io.cmd({0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  check(r == std::vector<uint8_t>({0xFF, 0x73, 0x5A, 0xEF, 0xDF, 0x30, 0x40, 0x10, 0x20}), "controle analógico: " + hex(r));
  // vibração: modo de configuração, 4Dh liga os motores, 42h com os valores
  r = io.cmd({0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});
  check(r.size() > 1 && r[1] == 0x73, "entrar na configuração");
  r = io.cmd({0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  check(r == std::vector<uint8_t>({0xFF, 0xF3, 0x5A, 0x01, 0x02, 0x01, 0x02, 0x01, 0x00}), "configuração 45h: " + hex(r));
  io.cmd({0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
  io.cmd({0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  io.cmd({0x01, 0x42, 0x00, 0x01, 0xA0, 0x00, 0x00, 0x00, 0x00});
  check(core.rumbleSmall() == 0xFF && core.rumbleLarge() == 0xA0, "vibração: motores pequeno e grande");
  io.cmd({0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  check(core.rumbleSmall() == 0 && core.rumbleLarge() == 0, "vibração desligada");
  // porta 2 vazia
  io.ctrl(0x3003);
  check(io.xfer(0x01) == 0xFF, "porta 2 sem controle");
  io.ctrl(0);
  // dois jogadores: controle digital na porta 2 depois de setKeys2, sem mexer na porta 1
  check(core.players() == 2, "2 jogadores: players() == 2");
  core.setKeys(0);
  core.setKeys2(KEY_B | KEY_LEFT);  // B = Xis (bit 14), Esquerda = bit 7
  {
    io.ctrl(0x3003);
    std::vector<uint8_t> r2;
    std::vector<bool> a2;
    for (uint8_t b : {0x01, 0x42, 0x00, 0x00, 0x00}) { bool a; r2.push_back(io.xfer(b, &a)); a2.push_back(a); }
    io.ctrl(0);
    check(r2 == std::vector<uint8_t>({0xFF, 0x41, 0x5A, 0x7F, 0xBF}), "2 jogadores: controle digital na porta 2: " + hex(r2));
    check(a2 == std::vector<bool>({true, true, true, true, false}), "2 jogadores: /ACK da porta 2 em todos menos o último");
    io.ctrl(0x3003);
    check(io.xfer(0x81) == 0xFF, "2 jogadores: porta 2 sem cartão de memória");
    io.ctrl(0);
  }
  r = io.cmd({0x01, 0x42, 0x00, 0x00, 0x00});
  check(r.size() == 5 && r[3] == 0xFF && r[4] == 0xFF, "2 jogadores: porta 1 não vê o controle 2: " + hex(r));
  core.setKeys2(0);

  // cartão de memória: identificação, escrita, leitura
  r = io.cmd({0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0});
  check(r == std::vector<uint8_t>({0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80}), "cartão 53h: " + hex(r));
  check(!core.saveDirty(), "cartão sem escrita: save limpo");
  std::vector<uint8_t> w = {0x81, 0x57, 0, 0, 0x00, 0x05};
  uint8_t chk = 0x05;
  for (int i = 0; i < 128; i++) { w.push_back(uint8_t(i * 7 + 1)); chk ^= uint8_t(i * 7 + 1); }
  w.push_back(chk);
  w.insert(w.end(), {0, 0, 0});
  r = io.cmd(w);
  check(r.size() == w.size() && r.back() == 0x47 && r[r.size() - 3] == 0x5C, "cartão: escrita aceita (47h): " +
        hex(std::vector<uint8_t>(r.end() - 4, r.end())));
  check(core.saveDirty(), "cartão escrito: save sujo");
  std::vector<uint8_t> rd = {0x81, 0x52, 0, 0, 0x00, 0x05};
  rd.resize(rd.size() + 4 + 128 + 2, 0);
  r = io.cmd(rd);
  bool same = r.size() == rd.size() && r[1] == 0x00 && r[6] == 0x5C && r[8] == 0x00 && r[9] == 0x05;
  for (int i = 0; same && i < 128; i++) same = r[10 + size_t(i)] == uint8_t(i * 7 + 1);
  check(same && r.size() > 139 && r[138] == chk && r[139] == 0x47, "cartão: leitura igual à escrita, flag 00h depois da escrita");
  auto wbad = w;
  wbad[6 + 3] ^= 1;
  r = io.cmd(wbad);
  check(r.back() == 0x4E, "cartão: soma errada recusada (4Eh)");
  io.cmd(w);
  const auto sav = core.exportSave();
  check(sav.size() == 128 * 1024 && sav[5 * 128 + 1] == 8 && !core.saveDirty(), "cartão como save (128 KB)");
  check(sav[0] == 'M' && sav[1] == 'C', "cartão novo já formatado");
  auto other = sav;
  other[5 * 128] = 0xAB;
  core.importSave(other);
  r = io.cmd(rd);
  check(r.size() > 10 && r[10] == 0xAB, "importSave muda o cartão");
}

// ---------------------------------------------------------------- testes do PeterLemon (PASS/FAIL na tela)
// PASS: caixa verde (0,132,0); FAIL: caixa vermelha (255,0,0), na coluna "Test" (x >= 270), abaixo do título
bool peterResult(const Core& c, int& passPixels, int& failPixels) {
  passPixels = failPixels = 0;
  const uint32_t* f = c.framebuffer();
  for (int y = 20; y < c.height(); y++)
    for (int x = 270; x < c.width(); x++) {
      const uint32_t p = f[y * c.width() + x] & 0xFFFFFF;
      if (p == 0x008400) passPixels++;
      if (p == 0x0000FF) failPixels++;
    }
  return passPixels > 0 && failPixels == 0;
}

int runTool(int argc, char** argv) {
  const char* bp = std::getenv("PS1_BIOS");
  const std::string biosPath = bp ? bp : "/root/indigo-env/bios-joao/scph1001.bin";
  if (argc >= 4 && std::string(argv[1]) == "run") {
    const auto exe = readAll(argv[2]);
    const int frames = std::atoi(argv[3]);
    const bool direct = argc > 4 && std::string(argv[4]) == "direct";
    const auto bios = direct ? std::vector<uint8_t>() : readAll(biosPath);
    Ps1Core core(exe, argv[2], nullptr, direct ? Ps1Core::ExeBoot::Direct : Ps1Core::ExeBoot::Bios, direct ? nullptr : &bios);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++) core.runFrame();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s", core.tty().c_str());
    std::printf("\n[%d quadros em %.2f s = %.1f q/s; pc=%08X; %dx%d; quadro %08X]\n", frames, s, frames / s, core.sys().cpu.pc,
                core.width(), core.height(), frameHash(core));
    if (argc > 5) writePpm(frameOf(core), argv[5]);
    if (std::getenv("PS1DUMP")) {
      auto& c = core.sys().cpu;
      for (int i = 0; i < 32; i++) std::printf("r%d=%08X%s", i, c.r[i], i % 8 == 7 ? "\n" : " ");
      std::printf("sr=%08X cause=%08X epc=%08X badv=%08X istat=%X imask=%X\n", c.sr, c.cause, c.epc, c.badv, core.sys().istat, core.sys().imask);
    }
    if (const char* vp = std::getenv("PS1VRAM")) {
      Image im;
      im.w = 1024; im.h = 512;
      const uint16_t* v = core.sys().gpu.vram();
      for (int i = 0; i < 1024 * 512; i++)
        im.rgb.insert(im.rgb.end(), {uint8_t((v[i] & 31) << 3), uint8_t(((v[i] >> 5) & 31) << 3), uint8_t(((v[i] >> 10) & 31) << 3)});
      writePpm(im, vp);
    }
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "bios") {
    const auto bios = readAll(biosPath);
    std::vector<uint8_t> fake(0x800, 0);
    std::memcpy(fake.data(), "PS-X EXE", 8);
    Ps1Core core(fake, "bios", nullptr, Ps1Core::ExeBoot::Bios, &bios);
    core.sys().cpu.hookPc = 1;  // não carrega nada: fica no menu
    const int frames = std::atoi(argv[2]);
    if (const char* tr = std::getenv("PS1TRACE")) {  // pula 'frames' quadros e mostra os desvios das próximas N instruções
      for (int f = 0; f < frames; f++) core.runFrame();
      uint32_t last = 0;
      for (int i = 0; i < std::atoi(tr); i++) {
        auto& s = core.sys();
        s.nextEvent = s.now + 1;
        const uint32_t pc = s.cpu.pc;
        s.cpu.run();
        if (pc != last + 4) std::printf("%08X ", pc);
        last = pc;
      }
      std::printf("\n");
      return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++) core.runFrame();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s", core.tty().c_str());
    std::printf("\n[%d quadros em %.2f s = %.1f q/s; pc=%08X; %dx%d; quadro %08X]\n", frames, s, frames / s, core.sys().cpu.pc,
                core.width(), core.height(), frameHash(core));
    if (argc > 3) writePpm(frameOf(core), argv[3]);
    return 0;
  }
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3) {
    const int r = runTool(argc, argv);
    if (r >= 0) return r;
  }
  if (argc != 2) {
    std::printf("uso: %s pasta | run programa.exe quadros [direct|bios] [saida.ppm] | bios quadros [saida.ppm]\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const auto testBios = readAll(dir + "/bios/teste.bin");
  check(testBios.size() == 512 * 1024, "BIOS de teste (bios/teste.bin; rode o t9_ps1.sh)");
  if (testBios.size() != 512 * 1024) { std::printf("RESULTADO: %ld verificações, %ld falhas\n", g_checks, g_fails); return 1; }

  // ---------------------------------------------------------------- reconhecimento, núcleo sem BIOS
  {
    std::vector<uint8_t> b(512 * 1024, 0);
    check(!ps1::isPs1Bios(b), "512 KB de zeros não são BIOS");
    std::memcpy(&b[0x108], "Sony Computer Entertainment Inc.", 32);
    check(ps1::isPs1Bios(b), "512 KB com o texto da Sony são BIOS");
    b.resize(256 * 1024);
    check(!ps1::isPs1Bios(b), "tamanho errado não é BIOS");
    check(!ps1::isPs1Bios(testBios), "a BIOS de teste não se passa pela da Sony");
    std::vector<uint8_t> exe(0x800, 0);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    check(ps1::isPsxExe(exe), "cabeçalho PS-X EXE");
    bool threw = false;
    setFirmware("ps1", {});
    try { Ps1Core c(exe, "x.exe", nullptr, Ps1Core::ExeBoot::Bios); } catch (const std::exception&) { threw = true; }
    check(threw, "sem BIOS o núcleo não é criado");
    threw = false;
    setFirmware("ps1", testBios);  // sem o texto da Sony: recusada como firmware do usuário
    try { Ps1Core c(exe, "x.exe", nullptr, Ps1Core::ExeBoot::Bios); } catch (const std::exception&) { threw = true; }
    check(threw, "arquivo qualquer de 512 KB recusado como BIOS");
    setFirmware("ps1", {});
  }

  // ---------------------------------------------------------------- BIOS de teste
  {
    std::vector<uint8_t> exe(0x800, 0);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    Ps1Core core(exe, "x.exe", nullptr, Ps1Core::ExeBoot::Bios, &testBios);
    core.sys().cpu.hookPc = 1;
    for (int f = 0; f < 60; f++) core.runFrame();
    check(core.tty() == "BIOS de teste do Indigo\n", "BIOS de teste: TTY \"" + core.tty() + "\"");
    uint32_t vb;
    std::memcpy(&vb, core.sys().ram + 0x200, 4);
    check(vb >= 50 && vb <= 60, "BIOS de teste: " + std::to_string(vb) + " interrupções de vblank em 60 quadros");
    check((core.sys().cpu.pc & ~4u) == 0x80030000, "BIOS de teste: chegou ao laço do menu");
    const uint32_t* fb = core.framebuffer();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%06X %06X", fb[0] & 0xFFFFFF, fb[120 * 320 + 160] & 0xFFFFFF);
    check(core.width() == 320 && core.height() == 240 && (fb[0] & 0xFFFFFF) == 0xA54210 && (fb[120 * 320 + 160] & 0xFFFFFF) == 0xF7C600,
          std::string("BIOS de teste: tela azul com o quadrado (") + buf + ")");
  }

  // ---------------------------------------------------------------- CPU: testes do PeterLemon
  {
    const char* T[] = {"ADD/CPUADD", "ADDI/CPUADDI", "ADDIU/CPUADDIU", "ADDU/CPUADDU", "AND/CPUAND", "ANDI/CPUANDI",
                       "DIV/CPUDIV", "DIVU/CPUDIVU", "MULT/CPUMULT", "MULTU/CPUMULTU", "NOR/CPUNOR", "OR/CPUOR", "ORI/CPUORI",
                       "SUB/CPUSUB", "SUBU/CPUSUBU", "XOR/CPUXOR", "XORI/CPUXORI", "LOADSTORE/LB/CPULB", "LOADSTORE/LH/CPULH",
                       "LOADSTORE/LW/CPULW", "LOADSTORE/SB/CPUSB", "LOADSTORE/SH/CPUSH", "LOADSTORE/SW/CPUSW", "SHIFT/SLL/CPUSLL",
                       "SHIFT/SLLV/CPUSLLV", "SHIFT/SRA/CPUSRA", "SHIFT/SRAV/CPUSRAV", "SHIFT/SRL/CPUSRL", "SHIFT/SRLV/CPUSRLV"};
    int ok = 0, runs = 0, n = 0;
    for (const char* t : T) {
      const auto exe = readAll(dir + "/PeterLemon/CPUTest/CPU/" + t + ".exe");
      if (exe.empty()) { check(false, std::string("falta ") + t + ".exe"); continue; }
      for (int mode = 0; mode < 2; mode++) {
        if (mode == 1 && n % 4) continue;  // pela BIOS de teste: um de cada quatro
        Ps1Core core(exe, t, nullptr, mode ? Ps1Core::ExeBoot::Bios : Ps1Core::ExeBoot::Direct, mode ? &testBios : nullptr);
        for (int f = 0; f < 90; f++) core.runFrame();
        int p, fl;
        const bool r = peterResult(core, p, fl);
        check(r, std::string("PeterLemon ") + t + (mode ? " (BIOS de teste)" : " (direto)") + ": " + std::to_string(p / 162) +
                     " PASS, " + std::to_string(fl) + " pontos de FAIL");
        ok += r;
        runs++;
      }
      n++;
    }
    std::printf("CPU (PeterLemon): %d de %d execuções com todos os PASS (%d testes)\n", ok, runs, n);
  }

  // ---------------------------------------------------------------- SIO0
  testSio(testBios, readAll(dir + "/PeterLemon/CPUTest/CPU/ADD/CPUADD.exe"));

  // ---------------------------------------------------------------- save state
  {
    const auto exe = readAll(dir + "/PeterLemon/CPUTest/CPU/SHIFT/SLL/CPUSLL.exe");
    Ps1Core a(exe, "a", nullptr, Ps1Core::ExeBoot::Bios, &testBios), b(exe, "b", nullptr, Ps1Core::ExeBoot::Bios, &testBios);
    for (int f = 0; f < 20; f++) { a.runFrame(); b.runFrame(); }
    check(a.saveState() == b.saveState(), "determinismo: dois núcleos iguais");
    const auto st = a.saveState();
    for (int f = 0; f < 20; f++) a.runFrame();
    const uint32_t h1 = frameHash(a);
    const auto st2 = a.saveState();
    a.loadState(st);
    const auto st3 = a.saveState();
    size_t at = 0;
    while (at < st.size() && at < st3.size() && st[at] == st3[at]) at++;
    check(st3 == st, "salvar -> carregar -> salvar volta igual (diferença no byte " + std::to_string(at) + " de " +
                         std::to_string(st.size()) + ")");
    for (int f = 0; f < 20; f++) a.runFrame();
    check(frameHash(a) == h1 && a.saveState() == st2, "carregar o estado repete o mesmo caminho");
    std::vector<std::vector<uint8_t>> bad = {{}, std::vector<uint8_t>(st.begin(), st.begin() + long(st.size() / 2)),
                                             std::vector<uint8_t>(64, 0x55)};
    auto magic = st;
    magic[0] = 'X';
    bad.push_back(magic);
    for (const auto& s : bad) {
      const auto before = a.saveState();
      bool threw = false;
      try { a.loadState(s); } catch (const std::exception&) { threw = true; }
      check(threw && a.saveState() == before, "estado danificado recusado sem mexer no jogo");
    }
  }

  // ---------------------------------------------------------------- com a BIOS do usuário
  const char* bp = std::getenv("PS1_BIOS");
  const std::string biosPath = bp ? bp : dir + "/../../bios-joao/scph1001.bin";
  const auto bios = readAll(biosPath);
  if (!ps1::isPs1Bios(bios)) {
    std::printf("Sem a BIOS do PlayStation em %s: testes com a BIOS verdadeira pulados\n", biosPath.c_str());
  } else {
    setFirmware("ps1", bios);
    // partida até o menu
    {
      std::vector<uint8_t> exe(0x800, 0);
      std::memcpy(exe.data(), "PS-X EXE", 8);
      Ps1Core core(exe, "x.exe", nullptr);
      core.sys().cpu.hookPc = 1;
      const auto t0 = std::chrono::steady_clock::now();
      for (int f = 0; f < 400; f++) core.runFrame();
      const uint32_t logo = frameHash(core);
      if (std::getenv("PS1_PPM")) writePpm(frameOf(core), "logo.ppm");
      for (int f = 400; f < 1100; f++) core.runFrame();
      const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      check(core.tty().find("System ROM Version") != std::string::npos && core.tty().find("PAD Driver") != std::string::npos,
            "BIOS: kernel e driver do controle na TTY");
      char hb[16];
      std::snprintf(hb, sizeof hb, "%08X", logo);
      check(logo == 0x9C453433u, std::string("BIOS: logo da Sony no quadro 400 (hash ") + hb + ", fixado depois de conferido a olho)");
      Image ref;
      if (readPpm(dir + "/ref/menu.ppm", ref)) {
        const double sim = similarity(frameOf(core), ref);
        check(sim > 0.80, "BIOS: menu parecido com o do Beetle (" + std::to_string(int(sim * 100)) + "%; as bolas se mexem)");
        std::printf("BIOS: menu %.1f%% igual ao do Beetle; %.0f quadros/s (%.1fx a velocidade do console)\n", sim * 100, 1100 / s,
                    1100 / s / 59.83);
      }
    }
    // ps1-tests com o psx.log do console
    {
      const char* T[] = {"cpu-cop", "dma-dpcr", "dma-otc-test", "gpu-gp0-e1", "gpu-gpustat", "gpu-mask-bit", "dma-chain-looping",
                         "cpu-code-in-io"};
      for (const char* t : T) {
        const auto exe = readAll(dir + "/build/exe/" + t + ".exe");
        std::string area = t;
        const size_t dash = area.find('-');
        const std::string name = area.substr(dash + 1);
        area = area.substr(0, dash);
        const auto logBytes = readAll(dir + "/ps1-tests/" + area + "/" + name + "/psx.log");
        if (exe.empty() || logBytes.empty()) { check(false, std::string("falta ") + t); continue; }
        Ps1Core core(exe, t, nullptr);
        for (int f = 0; f < 900; f++) core.runFrame();
        const auto got = lines(core.tty());
        const auto exp = lines(std::string(logBytes.begin(), logBytes.end()));
        if (std::string(t) == "dma-chain-looping") {
          // os tempos dependem do compilador; os resultados não
          std::string g, e;
          for (const auto& l : got) if (l.find("finished") != std::string::npos) g += l.substr(l.find("finished")) + ";";
          for (const auto& l : exp) if (l.find("finished") != std::string::npos) e += l.substr(l.find("finished")) + ";";
          check(!g.empty() && g == e, std::string(t) + ": " + g);
          continue;
        }
        const int fails = countPrefix(got, "fail"), passes = countPrefix(got, "pass"), expPass = countPrefix(exp, "pass");
        check(fails == 0 && passes >= expPass && got.size() > 3,
              std::string(t) + ": " + std::to_string(passes) + " pass, " + std::to_string(fails) + " fail (console: " +
                  std::to_string(expPass) + " pass)");
        std::printf("%-20s %d pass, %d fail (console: %d pass)\n", t, passes, fails, expPass);
      }
    }
    // cpu/access-time: ciclos de leitura de 8/16/32 bits de cada região, a até 2 ciclos do console (a de 32 bits
    // no SPUCNT é desalinhada e cai numa exceção: fica de fora)
    {
      const auto exe = readAll(dir + "/build/exe/cpu-access-time.exe");
      const auto logBytes = readAll(dir + "/ps1-tests/cpu/access-time/psx.log");
      if (exe.empty() || logBytes.empty()) check(false, "falta cpu-access-time");
      else {
        Ps1Core core(exe, "cpu-access-time", nullptr);
        for (int f = 0; f < 300; f++) core.runFrame();
        // "  5.21" = 5 + 21/100 (o teste imprime %d.%d de 100 leituras)
        auto table = [](const std::string& t) {
          std::vector<std::pair<std::string, std::vector<double>>> out;
          for (const auto& l : lines(t)) {
            const size_t p = l.find(')');
            if (l.size() < 12 || l[11] != '(' || p == std::string::npos) continue;
            std::vector<double> v;
            const char* q = l.c_str() + p + 1;
            while (*q) {
              char* e;
              const long a = std::strtol(q, &e, 10);
              if (e == q) break;
              q = e;
              if (*q != '.') break;
              const long b = std::strtol(q + 1, &e, 10);
              q = e;
              v.push_back(double(a) + double(b) / 100);
            }
            out.emplace_back(l.substr(0, 10), v);
          }
          return out;
        };
        const auto got = table(core.tty()), exp = table(std::string(logBytes.begin(), logBytes.end()));
        int n = 0, ok = 0;
        std::string bad;
        for (size_t i = 0; i < exp.size() && i < got.size(); i++)
          for (size_t k = 0; k < exp[i].second.size() && k < got[i].second.size(); k++) {
            if (exp[i].first.rfind("SPUCNT", 0) == 0 && k == 2) continue;
            n++;
            if (std::fabs(got[i].second[k] - exp[i].second[k]) <= 2.0) ok++;
            else bad += " " + exp[i].first.substr(0, exp[i].first.find(' ')) + "/" + std::to_string(8 << k) + "=" +
                        std::to_string(got[i].second[k]).substr(0, 5);
          }
        check(exp.size() == got.size() && ok == n && n == 50, "cpu/access-time: " + std::to_string(ok) + " de " +
              std::to_string(n) + " tempos a até 2 ciclos do console;" + bad);
        std::printf("cpu/access-time: %d de %d tempos a até 2 ciclos do console\n", ok, n);
      }
    }
    // cpu/io-access-bitwidth: o que se lê depois de escrever 0x12345678 com 8/16/32 bits (as linhas que não
    // dependem de lixo do barramento nem do instante: JOY_MODE, SIO_MODE, CDROM_STAT, DMA, RAM)
    {
      const auto exe = readAll(dir + "/build/exe/cpu-io-access-bitwidth.exe");
      const auto logBytes = readAll(dir + "/ps1-tests/cpu/io-access-bitwidth/psx.log");
      if (exe.empty() || logBytes.empty()) check(false, "falta cpu-io-access-bitwidth");
      else {
        Ps1Core core(exe, "cpu-io-access-bitwidth", nullptr);
        for (int f = 0; f < 300; f++) core.runFrame();
        auto pick = [](const std::string& t) {
          std::vector<std::string> out;
          for (const auto& l : lines(t))
            for (const char* k : {"RAM ", "SCRATCHPAD", "DMA0_ADDR", "DMAC_", "JOY_", "SIO_MODE", "CDROM_STAT", "SPUCNT"})
              if (l.rfind(k, 0) == 0) out.push_back(l);
          return out;
        };
        const auto got = pick(core.tty()), exp = pick(std::string(logBytes.begin(), logBytes.end()));
        int eq = 0;
        std::string first;
        for (size_t i = 0; i < exp.size(); i++) {
          if (i < got.size() && got[i] == exp[i]) eq++;
          else if (first.empty()) first = " " + (i < got.size() ? got[i] : std::string("(nada)")) + " x console " + exp[i];
        }
        check(exp.size() == 30 && eq == int(exp.size()), "cpu/io-access-bitwidth: " + std::to_string(eq) + " de " +
              std::to_string(exp.size()) + " linhas iguais ao console;" + first);
        std::printf("cpu/io-access-bitwidth: %d de %zu linhas iguais ao console\n", eq, exp.size());
      }
    }
    // timers: alvo com volta a 0 (relógio do sistema, alvo 10): quantas de 10000 leituras caem em cada valor do
    // contador, a até 5 das do console (lá 0 aparece em dobro: a volta a 0 segura um passo; e a leitura do timer
    // espera o buffer de escrita do sh da volta anterior, senão o laço tem 16 ciclos e só vê 3 valores)
    {
      const auto exe = readAll(dir + "/build/exe/timers-timers.exe");
      const auto logBytes = readAll(dir + "/ps1-tests/timers/psx.log");
      if (exe.empty() || logBytes.empty()) check(false, "falta timers-timers");
      else {
        Ps1Core core(exe, "timers-timers", nullptr);
        for (int f = 0; f < 1500 && core.tty().find("Target + 1 not reached") == std::string::npos; f++) core.runFrame();
        auto ticks = [](const std::string& t) {
          std::vector<int> v;
          const size_t p = t.find("Check when Timer2");
          if (p == std::string::npos) return v;
          for (const auto& l : lines(t.substr(p))) {
            if (l.rfind("counter == ", 0) != 0) continue;
            const size_t c = l.find(',');
            if (c != std::string::npos) v.push_back(std::atoi(l.c_str() + c + 1));
          }
          return v;
        };
        const auto got = ticks(core.tty()), exp = ticks(std::string(logBytes.begin(), logBytes.end()));
        int near = 0;
        for (size_t i = 0; i < exp.size() && i < got.size(); i++) near += std::abs(got[i] - exp[i]) <= 5;
        std::string g;
        for (int x : got) g += " " + std::to_string(x);
        check(exp.size() == 12 && got.size() == 12 && near == 12, "timers: leituras por valor do contador:" + g);
        std::printf("timers: alvo com volta a 0: %d de %zu valores com as leituras do console (0: %d, 10: %d)\n", near, exp.size(),
                    got.empty() ? -1 : got[0], got.size() > 10 ? got[10] : -1);
      }
    }
    // GPU: VRAM igual à do console
    {
      struct V { const char* t; int maxDiff; bool screenOnly; };
      // lines: o último segmento da polilinha gouraud usa uma cor não inicializada do teste (depende do compilador);
      // transparency: fora da tela, a VRAM do console tinha o que ficou do carregador usado
      const V T[] = {{"clipping", 0, false}, {"clut-cache", 0, false}, {"lines", 64, false}, {"quad", 0, false},
                     {"rectangles", 0, false}, {"texture-flip", 0, false}, {"texture-overflow", 0, false},
                     {"transparency", 0, true}, {"triangle", 0, false}, {"uv-interpolation", 0, false},
                     {"vram-to-vram-overlap", 0, false}};
      for (const V& v : T) {
        const std::string n = std::string("gpu-") + v.t;
        const auto exe = readAll(dir + "/build/exe/" + n + ".exe");
        Image ref;
        if (exe.empty() || !readPpm(dir + "/build/vram/" + n + ".ppm", ref) || ref.w != 1024 || ref.h != 512) {
          check(false, "falta " + n); continue;
        }
        Ps1Core core(exe, n, nullptr);
        for (int f = 0; f < 300; f++) core.runFrame();
        const uint16_t* vr = core.sys().gpu.vram();
        int diff = 0;
        for (int y = 0; y < 512; y++)
          for (int x = 0; x < 1024; x++) {
            if (v.screenOnly && (x >= 320 || y >= 240)) continue;
            const uint16_t p = vr[y * 1024 + x];
            const uint8_t* q = &ref.rgb[size_t(y * 1024 + x) * 3];
            diff += (p & 31) != (q[0] >> 3) || ((p >> 5) & 31) != (q[1] >> 3) || ((p >> 10) & 31) != (q[2] >> 3);
          }
        check(diff <= v.maxDiff, n + ": " + std::to_string(diff) + " pontos da VRAM diferentes do console");
        std::printf("GPU %-22s VRAM: %d pontos diferentes do console", v.t, diff);
        Image bq;
        if (readPpm(dir + "/ref/" + n + ".ppm", bq)) {
          const double sim = similarity(frameOf(core), bq);
          check(sim > 0.995, n + ": quadro " + std::to_string(sim * 100) + "% igual ao do Beetle");
          std::printf("; quadro %.2f%% igual ao do Beetle", sim * 100);
        }
        std::printf("\n");
      }
      // resolução interna 2x e 4x: a VRAM de verdade igual à de 1x (o programa não vê diferença) e, na imagem
      // ampliada, o ponto do canto de cada bloco (o que o console amostra) igual à imagem de 1x
      {
        int same = 0, total = 0;
        double worst = 1;
        for (const V& v : T) {
          const std::string n = std::string("gpu-") + v.t;
          const auto exe = readAll(dir + "/build/exe/" + n + ".exe");
          if (exe.empty()) continue;
          Ps1Core one(exe, n, nullptr);
          for (int f = 0; f < 300; f++) one.runFrame();
          for (int sc : {2, 4}) {
            Ps1Core hi(exe, n, nullptr);
            hi.setResolution(sc);
            for (int f = 0; f < 300; f++) hi.runFrame();
            const bool vramSame = std::equal(one.sys().gpu.vram(), one.sys().gpu.vram() + 1024 * 512, hi.sys().gpu.vram());
            long eq = 0;
            const uint32_t* a1 = one.framebuffer();
            const uint32_t* ah = hi.framebuffer();
            const bool sizeOk = hi.width() == one.width() * sc && hi.height() == one.height() * sc;
            if (sizeOk)
              for (int y = 0; y < one.height(); y++)
                for (int x = 0; x < one.width(); x++) eq += a1[y * one.width() + x] == ah[size_t(y * sc) * hi.width() + size_t(x * sc)];
            const double frac = double(eq) / (one.width() * one.height());
            check(vramSame && sizeOk && frac >= 0.99, n + " em " + std::to_string(sc) + "x: VRAM " + (vramSame ? "igual" : "diferente") +
                                                          ", canto dos blocos " + std::to_string(frac * 100) + "% igual a 1x");
            same += vramSame && sizeOk && frac >= 0.99;
            total++;
            worst = std::min(worst, frac);
          }
        }
        std::printf("GPU em 2x e 4x: %d de %d com a VRAM igual à de 1x; canto dos blocos igual a 1x em pelo menos %.2f%% dos pontos\n",
                    same, total, worst * 100);
      }
      for (const char* t : {"gp0-e1", "gpustat", "mask-bit", "version-detect"}) {
        const std::string n = std::string("gpu-") + t;
        Image bq;
        const auto exe = readAll(dir + "/build/exe/" + n + ".exe");
        if (exe.empty() || !readPpm(dir + "/ref/" + n + ".ppm", bq)) continue;
        Ps1Core core(exe, n, nullptr);
        for (int f = 0; f < 600; f++) core.runFrame();
        const double sim = similarity(frameOf(core), bq);
        check(sim > 0.98, n + ": quadro " + std::to_string(sim * 100) + "% igual ao do Beetle");
        std::printf("GPU %-22s quadro %.2f%% igual ao do Beetle\n", t, sim * 100);
      }
    }
    // tempos de desenho (gpu/bandwidth) contra o console
    {
      const auto exe = readAll(dir + "/build/exe/gpu-bandwidth.exe");
      const auto logBytes = readAll(dir + "/ps1-tests/gpu/bandwidth/psx.log");
      if (!exe.empty() && !logBytes.empty()) {
        Ps1Core core(exe, "bw", nullptr);
        for (int f = 0; f < 1200; f++) core.runFrame();
        const auto got = lines(core.tty());
        const auto exp = lines(std::string(logBytes.begin(), logBytes.end()));
        auto ms = [](const std::vector<std::string>& v, const std::string& key) {
          for (const auto& l : v)
            if (l.rfind(key, 0) == 0 && l.find("dT:") != std::string::npos) return std::atof(l.c_str() + l.find("dT:") + 3);
          return -1.0;
        };
        int n = 0;
        for (const char* k : {"vramToVram", "FillScreen", "Rectangle   ", "Rectangle (semitransparent)", "Rectangle textured",
                              "Polygon quad   ", "Polygon quad (semi)", "Polygon quad textured", "Polygon quad (1/4",
                              "Polygon quad (1/2"}) {
          const double g = ms(got, k), e = ms(exp, k);
          check(g > 0 && e > 0 && std::fabs(g - e) <= e * 0.12, std::string("gpu/bandwidth ") + k + ": " + std::to_string(g) +
                                                                     " ms (console " + std::to_string(e) + ")");
          n++;
        }
        std::printf("GPU: %d tempos de desenho conferidos com o console (gpu/bandwidth)\n", n);
      }
    }
    setFirmware("ps1", {});
  }

  std::printf("RESULTADO: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
