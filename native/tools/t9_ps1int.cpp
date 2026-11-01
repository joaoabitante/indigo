// Testes do PlayStation inteiro (etapa 9, integração: CPU + GTE + SPU + CD-ROM + MDEC + DMA + GPU). Pasta de
// testes como argumento (normalmente /root/indigo-env/roms/ps1int, montada pelo t9_ps1int.sh; usa também a
// ../ps1 do t9_ps1.sh). Tudo precisa da BIOS do usuário (bios-joao/scph1001.bin ou PS1_BIOS); sem ela os testes
// são pulados (saída 0). Réguas:
//  1. ps1-tests (console de verdade): gte/test-all (a CPU usando o COP2), spu/memory-transfer, cdrom/getloc,
//     cdrom/timing, cdrom/disc-swap (com a tampa aberta e fechada), mdec/4bit, 8bit, step-by-step-log e
//     dma/chopping pelo psx.log; mdec/4bit, 8bit, frame (15/24 bits, com e sem DMA) e movie pelo vram.png;
//     spu/test (registradores de 16 e 32 bits), spu/stereo (som na esquerda e na direita) e cdrom/volume-regs;
//  2. disco de teste montado aqui (t9_ps1int_jogo.c + t9_ps1int_disco.py): reconhecido como "ps1", partida
//     pela BIOS até o programa 3D (GTE), textura e dados lidos do CD conferidos, som da SPU e da faixa de CD-DA,
//     controle, save state no meio do jogo (salvar -> carregar -> salvar igual, mesmo caminho depois);
//  3. quadros do disco de teste comparados com o Beetle PSX (ref/beetle-*.ppm, gravados pelo t9_ps1int.sh);
//  4. velocidade: quadros por segundo do disco de teste;
//  5. resolução interna 2x / 4x no disco de teste: estado salvo igual ao de 1x, canto dos blocos igual a 1x, média
//     dos blocos perto de 1x, detalhe nas bordas, troca no meio do jogo e velocidade (PS1RES_DIR grava os quadros).
// Uso: indigo-t9_ps1int pasta
//      indigo-t9_ps1int disco imagem.cue quadros [saida.ppm]       (ferramenta)
//      indigo-t9_ps1int exe programa.exe quadros [imagem.cue] [saida.ppm]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
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
// fração de pontos parecidos (diferença de até 'tol' por canal) no melhor deslocamento de a dentro de b
double similarity(const Image& a, const Image& b, int tol = 8) {
  if (a.w > b.w || a.h > b.h || !a.w || !a.h) return 0;
  double best = 0;
  for (int dy = 0; dy <= b.h - a.h; dy++)
    for (int dx = 0; dx <= b.w - a.w; dx++) {
      long eq = 0;
      for (int y = 0; y < a.h; y++) {
        const uint8_t* pa = &a.rgb[size_t(y) * a.w * 3];
        const uint8_t* pb = &b.rgb[(size_t(y + dy) * b.w + dx) * 3];
        for (int x = 0; x < a.w * 3; x += 3)
          eq += std::abs(pa[x] - pb[x]) <= tol && std::abs(pa[x + 1] - pb[x + 1]) <= tol && std::abs(pa[x + 2] - pb[x + 2]) <= tol;
      }
      best = std::max(best, double(eq) / (a.w * a.h));
    }
  return best;
}
Image vramImage(Ps1Core& c) {
  Image im;
  im.w = 1024; im.h = 512;
  im.rgb.resize(1024 * 512 * 3);
  const uint16_t* v = c.sys().gpu.vram();
  for (int i = 0; i < 1024 * 512; i++) {
    im.rgb[size_t(i) * 3] = uint8_t((v[i] & 31) << 3);
    im.rgb[size_t(i) * 3 + 1] = uint8_t(((v[i] >> 5) & 31) << 3);
    im.rgb[size_t(i) * 3 + 2] = uint8_t(((v[i] >> 10) & 31) << 3);
  }
  return im;
}
std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find('\n', p);
    if (e == std::string::npos) e = s.size();
    std::string l = s.substr(p, e - p);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    out.push_back(l);
    p = e + 1;
  }
  return out;
}
// o texto do programa: a TTY a partir da primeira linha que o psx.log também tem (antes vem o texto da BIOS)
std::string fromLineText(const std::string& tty, const std::string& prefix) {
  const size_t p = tty.find("\n" + prefix);
  return p == std::string::npos ? tty : tty.substr(p + 1);
}
std::vector<std::string> fromLine(const std::vector<std::string>& v, const std::string& first) {
  for (size_t i = 0; i < v.size(); i++)
    if (v[i] == first) return std::vector<std::string>(v.begin() + long(i), v.end());
  return {};
}
int countPrefix(const std::vector<std::string>& v, const char* pre) {
  int n = 0;
  for (const auto& l : v) n += l.rfind(pre, 0) == 0;
  return n;
}
std::unique_ptr<cdrom::Disc> openDisc(const std::string& p) {
  std::string e;
  auto d = cdrom::open(p, &e);
  if (!d) std::printf("não abriu %s: %s\n", p.c_str(), e.c_str());
  return d;
}


std::string norm(std::string l) {
  // endereços de RAM dependem do compilador usado para montar os testes
  for (size_t p = 0; (p = l.find("80", p)) != std::string::npos; p++) {
    const bool pre = p >= 2 && l.compare(p - 2, 2, "0x") == 0, eq = p >= 1 && l[p - 1] == '=';
    if (!pre && !eq) continue;
    size_t e = p;
    while (e < l.size() && std::isxdigit(uint8_t(l[e]))) e++;
    if (e - p == 8) { l.replace(p, 8, "ADDR"); }
  }
  return l;
}
std::string noBrackets(std::string l) {
  size_t a;
  while ((a = l.find('[')) != std::string::npos) {
    const size_t b = l.find(']', a);
    if (b == std::string::npos) break;
    l.replace(a, b - a + 1, "<>");
  }
  return l;
}
// compara a TTY com o psx.log do console a partir da primeira linha do programa; devolve linhas iguais / total
struct Cmp { int equal = 0, total = 0; std::string diff; };
Cmp compareLog(const std::string& tty, const std::string& log, bool structural = false) {
  std::vector<std::string> got, exp;
  for (auto& l : lines(tty)) got.push_back(structural ? noBrackets(norm(l)) : norm(l));
  for (auto& l : lines(log)) if (l.rfind("ResetGraph", 0) != 0) exp.push_back(structural ? noBrackets(norm(l)) : norm(l));
  while (!exp.empty() && exp.front().empty()) exp.erase(exp.begin());
  Cmp c;
  c.total = int(exp.size());
  if (exp.empty()) return c;
  got = fromLine(got, exp.front());
  for (size_t i = 0; i < exp.size(); i++) {
    const std::string g = i < got.size() ? got[i] : std::string("(nada)");
    if (g == exp[i]) c.equal++;
    else if (c.diff.empty()) c.diff = "linha " + std::to_string(i) + ": \"" + g.substr(0, 90) + "\" x console \"" + exp[i].substr(0, 90) + "\"";
  }
  return c;
}
// quantas linhas do psx.log aparecem na TTY na mesma ordem
int inOrder(const std::string& tty, const std::string& log) {
  std::vector<std::string> got, exp;
  for (auto& l : lines(tty)) got.push_back(norm(l));
  for (auto& l : lines(log)) if (l.rfind("ResetGraph", 0) != 0 && !l.empty()) exp.push_back(norm(l));
  size_t g = 0;
  int n = 0;
  for (const auto& e : exp) {
    while (g < got.size() && got[g] != e) g++;
    if (g == got.size()) break;
    n++;
    g++;
  }
  return n;
}
// bytes do hexdump ("  xx: aa bb ...") de um texto
std::vector<int> hexdump(const std::string& t) {
  std::vector<int> out;
  for (const auto& l : lines(t)) {
    const size_t c = l.find(": ");
    if (c == std::string::npos || c > 6 || l.find_first_not_of(" 0123456789abcdef") < c) continue;
    for (size_t p = c + 2; p + 1 < l.size() + 1; p += 3) {
      if (!std::isxdigit(uint8_t(l[p])) || p + 1 >= l.size() || !std::isxdigit(uint8_t(l[p + 1]))) break;
      out.push_back(int(std::strtol(l.substr(p, 2).c_str(), nullptr, 16)));
    }
  }
  return out;
}
std::string slurp(const std::string& p) {
  const auto b = readAll(p);
  return std::string(b.begin(), b.end());
}
std::unique_ptr<Ps1Core> exeCore(const std::string& path, std::unique_ptr<cdrom::Disc> d = nullptr) {
  auto c = std::make_unique<Ps1Core>(readAll(path), path, nullptr);
  if (d) c->insertDisc(std::move(d));
  return c;
}
void runFrames(Ps1Core& c, int n) {
  std::vector<float> L(4096), R(4096);
  for (int f = 0; f < n; f++) {
    c.runFrame();
    while (size_t k = std::min<size_t>(c.audioAvailable(), L.size())) c.pullAudio(L.data(), R.data(), k);
  }
}
bool runUntil(Ps1Core& c, const std::string& text, int maxFrames) {
  std::vector<float> L(4096), R(4096);
  for (int f = 0; f < maxFrames; f++) {
    if (c.tty().find(text) != std::string::npos) return true;
    c.runFrame();
    while (size_t k = std::min<size_t>(c.audioAvailable(), L.size())) c.pullAudio(L.data(), R.data(), k);
  }
  return c.tty().find(text) != std::string::npos;
}
void press(Ps1Core& c, uint32_t keys, int frames) {
  c.setKeys(keys);
  runFrames(c, frames);
  c.setKeys(0);
  runFrames(c, 4);
}
int vramDiff(Ps1Core& c, const Image& ref, int x0, int y0, int w, int h, bool bytes24) {
  const uint16_t* v = c.sys().gpu.vram();
  auto refHalf = [&](int x, int y) {
    const uint8_t* q = &ref.rgb[size_t(y * 1024 + x) * 3];
    return uint16_t((q[0] >> 3) | (q[1] >> 3) << 5 | (q[2] >> 3) << 10);
  };
  int diff = 0;
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++) {
      const uint16_t a = v[y * 1024 + x] & 0x7FFF, b = refHalf(x, y);
      if (!bytes24) {
        diff += std::abs((a & 31) - (b & 31)) > 1 || std::abs(((a >> 5) & 31) - ((b >> 5) & 31)) > 1 ||
                std::abs(((a >> 10) & 31) - ((b >> 10) & 31)) > 1;
      } else {
        // 24 bits: cada meia palavra são dois bytes de cor (o bit 15 não está no vram.png)
        const int lo = std::abs((a & 0xFF) - (b & 0xFF)), hi = std::abs(((a >> 8) & 0x7F) - ((b >> 8) & 0x7F));
        diff += (lo > 1 && lo < 255) || (hi > 1 && hi < 127);
      }
    }
  return diff;
}

// disco sintético de uma faixa Mode 2 do tamanho dado (setores gerados na hora): o cdrom/getloc lê em 40:00 e
// busca em 74:00, então precisa de um disco do tamanho do usado no console (70:25:16 de área de dados)
class SynthDisc : public cdrom::Disc {
public:
  explicit SynthDisc(uint32_t sectors) {
    cdrom::Track t;
    t.number = 1; t.type = cdrom::TrackType::Mode2; t.start = 0; t.length = sectors;
    tracks_.push_back(t);
    total_ = sectors;
    name_ = "sintetico";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, 2352);
    if (lba >= total_) return false;
    std::memset(out + 1, 0xFF, 10);
    const uint32_t a = lba + 150;
    auto bcd = [](uint32_t v) { return uint8_t((v / 10) << 4 | (v % 10)); };
    out[12] = bcd(a / 4500); out[13] = bcd(a / 75 % 60); out[14] = bcd(a % 75); out[15] = 2;
    out[18] = out[22] = 0x08;  // subcabeçalho: dados (Form 1)
    if (lba == 4) std::memcpy(out + 24, "          Licensed  by          Sony Computer Entertainment Amer  ica ", 68);
    for (int i = 32; i < 2048; i++) out[24 + i] = uint8_t(lba * 31 + uint32_t(i));
    return true;
  }
};

// som: energia (RMS) de cada canal nos próximos n quadros
struct Energy { double l = 0, r = 0; };
Energy runAudio(Ps1Core& c, int frames) {
  double sl = 0, sr = 0;
  long n = 0;
  std::vector<float> L(4096), R(4096);
  for (int f = 0; f < frames; f++) {
    c.runFrame();
    size_t k = c.audioAvailable();
    while (k) {
      const size_t m = std::min<size_t>(k, L.size());
      c.pullAudio(L.data(), R.data(), m);
      for (size_t i = 0; i < m; i++) { sl += double(L[i]) * L[i]; sr += double(R[i]) * R[i]; }
      n += long(m);
      k -= m;
    }
  }
  Energy e;
  if (n) { e.l = std::sqrt(sl / double(n)); e.r = std::sqrt(sr / double(n)); }
  return e;
}

int runTool(int argc, char** argv) {
  const char* bp = std::getenv("PS1_BIOS");
  const auto bios = readAll(bp ? bp : "/root/indigo-env/bios-joao/scph1001.bin");
  setFirmware("ps1", bios);
  std::unique_ptr<Ps1Core> core;
  int frames = 0;
  std::string ppm;
  if (argc >= 4 && std::string(argv[1]) == "disco") {
    auto d = openDisc(argv[2]);
    if (!d) return 1;
    std::printf("detectSystem: \"%s\"\n", cdrom::detectSystem(*d).c_str());
    core = std::make_unique<Ps1Core>(std::move(d), nullptr);
    frames = std::atoi(argv[3]);
    if (argc > 4) ppm = argv[4];
  } else if (argc >= 4 && std::string(argv[1]) == "exe") {
    core = std::make_unique<Ps1Core>(readAll(argv[2]), argv[2], nullptr);
    frames = std::atoi(argv[3]);
    if (argc > 4 && std::string(argv[4]) == "sintetico") {
      core->insertDisc(std::make_unique<SynthDisc>(cdrom::msfToLba(70, 25, 16)));
    } else if (argc > 4 && std::string(argv[4]) != "-") {
      auto d = openDisc(argv[4]);
      if (!d) return 1;
      core->insertDisc(std::move(d));
    }
    if (argc > 5) ppm = argv[5];
  } else {
    return -1;
  }
  if (const char* sp = std::getenv("PS1SPUTRACE")) {
    const int from = std::atoi(sp);
    for (int f = 0; f < frames; f++) {
      core->runFrame();
      if (f >= from && f < from + 40) {
        auto& y = core->sys();
        std::printf("q%d env0=%04X env1=%04X vol=%04X/%04X ctrl=%04X stat=%04X\n", f, y.spu.read(0x00C, 2), y.spu.read(0x01C, 2), y.spu.read(0x1B8, 2), y.spu.read(0x1BA, 2), y.spu.read(0x1AA, 2), y.spu.read(0x1AE, 2));
      }
    }
    frames = 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const Energy e = runAudio(*core, frames);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("%s", core->tty().c_str());
  std::printf("\n[%d quadros em %.2f s = %.1f q/s; pc=%08X; %dx%d; quadro %08X; som L %.4f R %.4f]\n", frames, s, frames / s,
              core->sys().cpu.pc, core->width(), core->height(), frameHash(*core), e.l, e.r);
  if (!ppm.empty()) writePpm(frameOf(*core), ppm);
  if (std::getenv("PS1DUMP")) {
    auto& y = core->sys();
    std::printf("gpustat=%08X istat=%X imask=%X sr=%08X\n", y.gpu.status(), y.istat, y.imask, y.cpu.sr);
    for (int c = 0; c < 7; c++) std::printf("dma%d: madr=%08X bcr=%08X chcr=%08X\n", c, y.dma.read(0x80 + c * 16), y.dma.read(0x84 + c * 16), y.dma.read(0x88 + c * 16));
    std::printf("dpcr=%08X dicr=%08X\n", y.dma.read(0xF0), y.dma.read(0xF4));
    std::printf("spu:");
    for (uint32_t o : {0x180u, 0x182u, 0x1AAu, 0x1AEu, 0x1B0u, 0x1B2u, 0x1B8u, 0x1BAu, 0x000u, 0x002u, 0x004u, 0x006u, 0x008u, 0x00Au, 0x00Cu, 0x00Eu, 0x188u, 0x18Au, 0x19Cu})
      std::printf(" %03X=%04X", o, y.spu.read(o, 2));
    std::printf("\n");
  }
  if (const char* vp = std::getenv("PS1VRAM")) writePpm(vramImage(*core), vp);
  if (const char* rp = std::getenv("PS1RAM")) {
    if (FILE* f = std::fopen(rp, "wb")) { std::fwrite(core->sys().ram, 1, ps1::RAM_SIZE, f); std::fclose(f); }
  }
  return 0;
}

// imagem ampliada reduzida para a resolução do console (média de cada bloco n x n)
Image boxDown(const Image& a, int n) {
  Image o;
  o.w = a.w / n; o.h = a.h / n;
  o.rgb.resize(size_t(o.w) * o.h * 3);
  for (int y = 0; y < o.h; y++)
    for (int x = 0; x < o.w; x++)
      for (int k = 0; k < 3; k++) {
        int sum = 0;
        for (int j = 0; j < n; j++)
          for (int i = 0; i < n; i++) sum += a.rgb[(size_t(y * n + j) * a.w + size_t(x * n + i)) * 3 + k];
        o.rgb[(size_t(y) * o.w + x) * 3 + k] = uint8_t((sum + n * n / 2) / (n * n));
      }
  return o;
}
// o ponto do canto de cima à esquerda de cada bloco n x n
Image cornerDown(const Image& a, int n) {
  Image o;
  o.w = a.w / n; o.h = a.h / n;
  o.rgb.resize(size_t(o.w) * o.h * 3);
  for (int y = 0; y < o.h; y++)
    for (int x = 0; x < o.w; x++)
      for (int k = 0; k < 3; k++) o.rgb[(size_t(y) * o.w + x) * 3 + k] = a.rgb[(size_t(y * n) * a.w + size_t(x * n)) * 3 + k];
  return o;
}
double psnr(const Image& a, const Image& b) {
  double se = 0;
  for (size_t i = 0; i < a.rgb.size() && i < b.rgb.size(); i++) se += double(a.rgb[i] - b.rgb[i]) * (a.rgb[i] - b.rgb[i]);
  const double mse = se / double(std::max<size_t>(a.rgb.size(), 1));
  return mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
}
// pontos iguais (até 'tol' por canal) na mesma posição
double samePixels(const Image& a, const Image& b, int tol) {
  if (a.w != b.w || a.h != b.h || !a.w) return 0;
  long eq = 0;
  for (size_t i = 0; i < a.rgb.size(); i += 3)
    eq += std::abs(a.rgb[i] - b.rgb[i]) <= tol && std::abs(a.rgb[i + 1] - b.rgb[i + 1]) <= tol && std::abs(a.rgb[i + 2] - b.rgb[i + 2]) <= tol;
  return double(eq) / (a.w * a.h);
}
// nitidez das bordas: dos pontos de borda da imagem em 1x (vizinho à direita ou embaixo com diferença > 24), quantos
// têm o bloco n x n da imagem ampliada com mais de uma cor (a borda passa por dentro do ponto, com detalhe abaixo
// de um ponto do console); em 1x ampliado por repetição seria 0
double edgeDetail(const Image& one, const Image& hi, int n, long* edges) {
  auto px = [](const Image& im, int x, int y, int k) { return int(im.rgb[(size_t(y) * im.w + x) * 3 + k]); };
  long e = 0, mixed = 0;
  for (int y = 0; y + 1 < one.h; y++)
    for (int x = 0; x + 1 < one.w; x++) {
      int d = 0;
      for (int k = 0; k < 3; k++) d = std::max({d, std::abs(px(one, x, y, k) - px(one, x + 1, y, k)), std::abs(px(one, x, y, k) - px(one, x, y + 1, k))});
      if (d <= 24) continue;
      e++;
      bool m = false;
      for (int j = 0; j < n && !m; j++)
        for (int i = 0; i < n && !m; i++)
          for (int k = 0; k < 3; k++) m = m || px(hi, x * n + i, y * n + j, k) != px(hi, x * n, y * n, k);
      mixed += m;
    }
  if (edges) *edges = e;
  return e ? double(mixed) / e : 0;
}
// resolução interna 2x / 4x: o mesmo trecho do disco de teste em 1x, 2x e 4x
void resolutionTests(Ps1Core& c, const std::vector<uint8_t>& st, double fps1) {
  const char* dumpDir = std::getenv("PS1RES_DIR");  // grava os quadros (ps1-res-1x/2x/4x.ppm) para olhar
  Image img[3];
  std::vector<uint8_t> after[3];
  const int scales[3] = {1, 2, 4};
  for (int i = 0; i < 3; i++) {
    c.setResolution(scales[i]);
    c.loadState(st);
    check(c.saveState() == st, "resolução " + std::to_string(scales[i]) + "x: carregar -> salvar igual");
    runFrames(c, 4);
    img[i] = frameOf(c);
    after[i] = c.saveState();
    check(c.resolution() == scales[i] && c.width() == 320 * scales[i] && c.height() == 240 * scales[i],
          "resolução " + std::to_string(scales[i]) + "x: imagem de " + std::to_string(c.width()) + "x" + std::to_string(c.height()));
    if (dumpDir) writePpm(img[i], std::string(dumpDir) + "/ps1-res-" + std::to_string(scales[i]) + "x.ppm");
  }
  check(after[1] == after[0] && after[2] == after[0], "resolução 2x/4x: estado salvo igual ao de 1x depois de 4 quadros");
  for (int i = 1; i < 3; i++) {
    const int n = scales[i];
    // 1) o ponto do canto de cada bloco é o mesmo ponto que o console amostra: tem de dar a imagem de 1x (mesma
    //    geometria, regra de preenchimento, textura, cor e pontilhado)
    const double corner = samePixels(cornerDown(img[i], n), img[0], 0);
    // 2) a média de cada bloco (filtro de caixa) perto da de 1x; a diferença que sobra é o serrilhado da textura
    //    xadrez de 1 texel dos cubos, que em 1x aparece amostrada e na ampliada sai média (medido com a PSNR)
    const Image down = boxDown(img[i], n);
    const double same = samePixels(down, img[0], 16), db = psnr(down, img[0]);
    long edges = 0;
    const double detail = edgeDetail(img[0], img[i], n, &edges);
    check(corner >= 0.995, "resolução " + std::to_string(n) + "x: canto dos blocos igual a 1x em " + std::to_string(corner * 100) + "% dos pontos");
    check(same >= 0.80 && db >= 20, "resolução " + std::to_string(n) + "x: reduzida igual à de 1x em " + std::to_string(same * 100) + "% dos pontos, PSNR " + std::to_string(db));
    check(detail >= 0.5, "resolução " + std::to_string(n) + "x: bordas com detalhe em " + std::to_string(detail * 100) + "% dos pontos de borda");
    std::printf("resolução %dx: imagem %dx%d; canto dos blocos %.2f%% igual a 1x; média %dx%d igual à de 1x em %.2f%% dos pontos "
                "(até 16 por canal, PSNR %.1f dB); %.1f%% dos %ld pontos de borda com detalhe abaixo de um ponto\n",
                n, img[i].w, img[i].h, corner * 100, n, n, same * 100, db, detail * 100, edges);
  }
  // trocar a escala no meio do jogo: 1x -> 4x -> 2x -> 1x a cada 10 quadros, mesmo caminho que ficar em 1x
  c.setResolution(1);
  c.loadState(st);
  runFrames(c, 40);
  const uint32_t h1 = frameHash(c);
  const auto s1 = c.saveState();
  c.loadState(st);
  bool sizes = true;
  for (int n : {1, 4, 2, 1}) {
    c.setResolution(n);
    sizes = sizes && c.width() == 320 * n;
    runFrames(c, 10);
    sizes = sizes && c.width() == 320 * n && c.height() == 240 * n;
  }
  check(sizes && frameHash(c) == h1 && c.saveState() == s1, "resolução: trocar 1x -> 4x -> 2x -> 1x no meio do jogo");
  // velocidade em 2x e 4x (o mesmo trecho da medida em 1x)
  double fps[3] = {fps1, 0, 0};
  for (int i = 1; i < 3; i++) {
    c.setResolution(scales[i]);
    c.loadState(st);
    const auto t0 = std::chrono::steady_clock::now();
    runFrames(c, 600);
    fps[i] = 600 / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
  c.setResolution(1);
  std::printf("resolução: velocidade %.0f / %.0f / %.0f quadros/s em 1x / 2x / 4x\n", fps[0], fps[1], fps[2]);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3) {
    const int r = runTool(argc, argv);
    if (r >= 0) return r;
  }
  if (argc != 2) {
    std::printf("uso: %s pasta | disco imagem.cue quadros [saida.ppm] | exe programa.exe quadros [imagem.cue|sintetico|-] [saida.ppm]\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1], ps1 = dir + "/../ps1", E = ps1 + "/build/exe/", T = ps1 + "/ps1-tests/", B = dir + "/build/";
  const char* bp = std::getenv("PS1_BIOS");
  const std::string biosPath = bp ? bp : dir + "/../../bios-joao/scph1001.bin";
  const auto bios = readAll(biosPath);
  if (!ps1::isPs1Bios(bios)) {
    std::printf("Sem a BIOS do PlayStation em %s: testes do console inteiro pulados\n", biosPath.c_str());
    std::printf("RESULTADO: 0 verificações, 0 falhas (pulados)\n");
    return 0;
  }
  setFirmware("ps1", bios);
  check(!readAll(E + "gte-test-all.exe").empty() && !readAll(dir + "/disco/teste.cue").empty(), "testes montados (rode o t9_ps1.sh e o t9_ps1int.sh)");
  if (g_fails) { std::printf("RESULTADO: %ld verificações, %ld falhas\n", g_checks, g_fails); return 1; }

  // ---------------------------------------------------------------- GTE pela CPU (COP2)
  {
    auto c = exeCore(E + "gte-test-all.exe");
    runFrames(*c, 900);
    const Cmp k = compareLog(c->tty(), slurp(T + "gte/test-all/psx.log"));
    check(c->tty().find("Passed tests: 1150") != std::string::npos && c->tty().find("Failed tests: 0") != std::string::npos,
          "gte/test-all: 1150 testes do GTE pela CPU");
    // o programa de hoje escreve uma linha a mais que o do console ("Total tests"): as do console vêm na mesma ordem
    const int sub = inOrder(c->tty(), slurp(T + "gte/test-all/psx.log"));
    check(sub == k.total, "gte/test-all: psx.log " + k.diff);
    std::printf("gte/test-all: %d de %d linhas do console na mesma ordem (1150 testes pelo COP2)\n", sub, k.total);
  }

  // ---------------------------------------------------------------- SPU
  {
    auto c = exeCore(E + "spu-memory-transfer.exe");
    runFrames(*c, 600);
    const auto got = lines(c->tty()), exp = lines(slurp(T + "spu/memory-transfer/psx.log"));
    int passes = 0, want = 0;
    for (const auto& l : exp)
      if (l.rfind("pass - ", 0) == 0) { want++; passes += std::find(got.begin(), got.end(), l) != got.end(); }
    check(countPrefix(got, "fail") == 0 && passes == want && want > 0, "spu/memory-transfer: " + std::to_string(passes) + " de " + std::to_string(want));
    std::printf("spu/memory-transfer: %d de %d \"pass\" do console, %d \"fail\" (inclui os de DMA em modo 0, que o psx.log não tem)\n",
                passes, want, countPrefix(got, "fail"));
  }
  {
    auto c = exeCore(E + "spu-test.exe");
    runFrames(*c, 900);
    check(c->tty().find("Done, crashing now") != std::string::npos && c->tty().find("written, but read") == std::string::npos,
          "spu/test: registradores da SPU com acesso de 16 e 32 bits");
    std::printf("spu/test: 16 e 32 bits %s\n", c->tty().find("written, but read") == std::string::npos ? "iguais" : "DIFERENTES");
  }
  {
    // stereo: a voz toca só na esquerda e depois só na direita (120 quadros cada)
    auto c = exeCore(E + "spu-stereo.exe");
    runUntil(*c, "Voice 1 (L: 0x3fff, R: 0x0000)", 600);
    int left = 0, right = 0, frames = 0;
    std::vector<float> L(4096), R(4096);
    for (int f = 0; f < 480; f++) {
      c->runFrame();
      double sl = 0, sr = 0;
      size_t k;
      while ((k = std::min<size_t>(c->audioAvailable(), L.size()))) {
        c->pullAudio(L.data(), R.data(), k);
        for (size_t i = 0; i < k; i++) { sl += double(L[i]) * L[i]; sr += double(R[i]) * R[i]; }
      }
      frames++;
      if (sl > 0.01 && sl > sr * 100) left++;
      if (sr > 0.01 && sr > sl * 100) right++;
    }
    check(left > 60 && right > 60, "spu/stereo: som só na esquerda (" + std::to_string(left) + " quadros) e só na direita (" + std::to_string(right) + ")");
    std::printf("spu/stereo: %d quadros só na esquerda, %d só na direita (de %d)\n", left, right, frames);
  }

  // ---------------------------------------------------------------- CD-ROM
  {
    auto c = exeCore(E + "cdrom-getloc.exe", std::make_unique<SynthDisc>(cdrom::msfToLba(70, 25, 16)));
    runFrames(*c, 2500);
    const std::string log = slurp(T + "cdrom/getloc/psx.log");
    const Cmp exact = compareLog(c->tty(), log), st = compareLog(c->tty(), log, true);
    // tudo igual fora as posições, menos o "Test passed": neste .exe (compilado aqui) o buffer da resposta do
    // Getstat fica na pilha onde o CdReadSync guarda s0, e o psxcd continua gravando nele as respostas seguintes
    // (o INT2 do Pause) — o resultado do teste se perde também num console, o que não depende do emulador
    check(st.equal >= st.total - 2 && c->tty().find("Seek result irq=5, status=0x04") != std::string::npos &&
              c->tty().find("Expected") == std::string::npos,
          "cdrom/getloc: " + std::to_string(st.equal) + " de " + std::to_string(st.total) + " " + st.diff);
    std::printf("cdrom/getloc: %d de %d linhas iguais ao console fora as posições ([mm:ss:ff]), %d iguais com elas\n", st.equal, st.total,
                exact.equal);
  }
  {
    auto c = exeCore(E + "cdrom-timing.exe", std::make_unique<SynthDisc>(cdrom::msfToLba(70, 25, 16)));
    runFrames(*c, 3000);
    const auto got = lines(c->tty()), exp = lines(slurp(T + "cdrom/timing/psx.log"));
    auto avg = [](const std::vector<std::string>& v, const std::string& key, const char* field) {
      double sum = 0;
      int n = 0;
      for (const auto& l : v) {
        if (l.rfind(key, 0) != 0) continue;
        const size_t p = l.find(field);
        if (p == std::string::npos) continue;
        sum += std::atof(l.c_str() + p + std::strlen(field));
        n++;
      }
      return n ? sum / n : -1.0;
    };
    struct K { const char* key; const char* field; double tol; };
    const K ks[] = {{"single-speed", "avg ", 0.03}, {"double-speed", "avg ", 0.03}, {"single-speed", "First Sector = ", 0.35},
                    {"double-speed", "First Sector = ", 0.6}, {"CdlNop", "average ", 0.35}, {"CdlSetloc", "average ", 0.35},
                    {"init ack", "average ", 0.35}, {"init complete", "average ", 0.10}};
    int ok = 0, n = 0;
    for (const K& k : ks) {
      const double g = avg(got, k.key, k.field), e = avg(exp, k.key, k.field);
      const bool good = g > 0 && e > 0 && std::fabs(g - e) <= e * k.tol;
      check(good, std::string("cdrom/timing ") + k.key + " " + k.field + std::to_string(g) + " (console " + std::to_string(e) + ")");
      ok += good;
      n++;
    }
    const double ic = avg(got, "init complete", "average "), ice = avg(exp, "init complete", "average ");
    std::printf("cdrom/timing: %d de %d tempos perto do console (setor seguinte %.0f x %.0f ciclos; Init completo %.0f x %.0f; "
                "fim do Pause em 1x %.0f x %.0f, informativo)\n", ok, n, avg(got, "single-speed", "avg "), avg(exp, "single-speed", "avg "), ic, ice,
                avg(got, "single-speed", "Pause Complete = "), avg(exp, "single-speed", "Pause Complete = "));
  }
  {
    // disc-swap: o teste pede para abrir e fechar a tampa
    auto c = exeCore(E + "cdrom-disc-swap.exe", openDisc(dir + "/disco/teste.cue"));
    const bool o = runUntil(*c, "Open the shell now", 900);
    runFrames(*c, 30);
    c->setLidOpen(true);
    const bool cl = runUntil(*c, "Close the shell now", 600);
    runFrames(*c, 30);
    c->setLidOpen(false);
    runFrames(*c, 600);
    const Cmp k = compareLog(c->tty(), slurp(T + "cdrom/disc-swap/psx.log"));
    // a última linha mudou no programa depois de gravado o psx.log ("Disc inserted?" virou "Disc detected")
    check(o && cl && k.equal >= k.total - 1, "cdrom/disc-swap: " + std::to_string(k.equal) + " de " + std::to_string(k.total) + " " + k.diff);
    std::printf("cdrom/disc-swap: tampa aberta e fechada; %d de %d linhas iguais ao console\n", k.equal, k.total);
  }
  {
    // volume-regs: R1 toca a faixa 2 (CD-DA), Triângulo liga/desliga o mudo, Baixo abaixa o volume do CD
    auto c = exeCore(E + "cdrom-volume-regs.exe", openDisc(dir + "/disco/teste.cue"));
    runUntil(*c, "Done.", 900);
    runFrames(*c, 60);
    const Energy e0 = runAudio(*c, 60);
    press(*c, KEY_X, 3);  // o leitor começa mudo depois da BIOS: Triângulo duas vezes (Mute e Demute)
    press(*c, KEY_X, 3);
    press(*c, KEY_R, 3);
    runFrames(*c, 200);
    const Energy e1 = runAudio(*c, 120);
    press(*c, KEY_X, 3);
    runFrames(*c, 30);
    const Energy e2 = runAudio(*c, 60);
    press(*c, KEY_X, 3);
    runFrames(*c, 30);
    const Energy e3 = runAudio(*c, 60);
    c->setKeys(KEY_DOWN);
    runFrames(*c, 100);
    c->setKeys(0);
    const Energy e4 = runAudio(*c, 60);
    char b[256];
    std::snprintf(b, sizeof b, "parado %.3f, tocando %.3f/%.3f, mudo %.3f, de volta %.3f, volume menor %.3f", e0.l, e1.l, e1.r, e2.l, e3.l, e4.l);
    const bool good = e0.l < 0.005 && e1.l > 0.05 && e1.r > 0.05 && e2.l < e1.l * 0.05 && e3.l > e1.l * 0.8 && e4.l < e3.l * 0.7;
    check(good, std::string("cdrom/volume-regs: ") + b);
    if (!good) { const std::string& t = c->tty(); const size_t p = t.find("Done."); std::printf("%s\n", t.substr(p == std::string::npos ? 0 : p, 300).c_str()); }
    std::printf("cdrom/volume-regs (som do CD-DA pela SPU): %s\n", b);
  }

  // ---------------------------------------------------------------- MDEC
  for (const char* t : {"4bit", "8bit"}) {
    auto c = exeCore(B + "mdec-" + t + ".exe");
    runFrames(*c, 600);
    const std::string log = slurp(T + "mdec/" + t + "/psx.log");
    const auto g = hexdump(fromLineText(c->tty(), "mdec/")), e = hexdump(log);
    int eq = 0, md = 0;
    for (size_t i = 0; i < std::min(g.size(), e.size()); i++) {
      eq += g[i] == e[i];
      md = std::max(md, t[0] == '4' ? std::max(std::abs((g[i] & 15) - (e[i] & 15)), std::abs((g[i] >> 4) - (e[i] >> 4))) : std::abs(g[i] - e[i]));
    }
    // 4 bits igual; 8 bits: a IDCT erra o bit menos significativo em alguns pontos (como no t9_cdmdec)
    const bool good = g.size() == e.size() && !e.empty() && (t[0] == '4' ? eq == int(e.size()) : eq >= 54 && md <= 1);
    check(good, std::string("mdec/") + t + ": " + std::to_string(eq) + " de " + std::to_string(e.size()) + " bytes");
    std::printf("mdec/%s: %d de %zu bytes do bloco decodificado iguais ao console (maior diferença %d)\n", t, eq, e.size(), md);
  }
  {
    auto c = exeCore(E + "mdec-step-by-step-log.exe");
    runFrames(*c, 900);
    const auto got = lines(c->tty()), exp = lines(slurp(T + "mdec/step-by-step-log/psx.log"));
    std::vector<uint32_t> g, e;
    for (const auto& l : got) { unsigned v; if (std::sscanf(l.c_str(), "MDEC_DATA -> 0x%x", &v) == 1) g.push_back(v); }
    for (const auto& l : exp) { unsigned v; if (std::sscanf(l.c_str(), "MDEC_DATA -> 0x%x", &v) == 1) e.push_back(v); }
    int same = 0, maxd = 0;
    for (size_t i = 0; i < std::min(g.size(), e.size()); i++) {
      same += g[i] == e[i];
      for (int h = 0; h < 2; h++)
        for (int ch = 0; ch < 3; ch++)
          maxd = std::max(maxd, std::abs(int((g[i] >> (16 * h + 5 * ch)) & 31) - int((e[i] >> (16 * h + 5 * ch)) & 31)));
    }
    // status (1F801824h) lido antes de cada passo: igual ao do console em todas as leituras
    std::vector<uint32_t> gs, es;
    for (const auto& l : got) { unsigned v; if (std::sscanf(l.c_str(), "==== MDEC_STATUS: 0x%x", &v) == 1) gs.push_back(v); }
    for (const auto& l : exp) { unsigned v; if (std::sscanf(l.c_str(), "==== MDEC_STATUS: 0x%x", &v) == 1) es.push_back(v); }
    int sameSt = 0;
    for (size_t i = 0; i < std::min(gs.size(), es.size()); i++) sameSt += gs[i] == es[i];
    check(g.size() == e.size() && e.size() == 512 && same >= 469 && maxd <= 1,
          "mdec/step-by-step-log: " + std::to_string(same) + " de " + std::to_string(e.size()) + " palavras");
    check(gs.size() == es.size() && es.size() == 777 && sameSt == 777, "mdec/step-by-step-log: status igual em " + std::to_string(sameSt));
    std::printf("mdec/step-by-step-log: %d de %zu palavras lidas pela CPU iguais ao console (maior diferença %d); status igual em %d de %zu\n",
                same, e.size(), maxd, sameSt, es.size());
  }
  // frame: o pôr do sol em faixas de 16 pontos. O vram.png do console é do programa com DMA na saída; lido pela
  // CPU, o programa espera um dado a mais depois do último e a última faixa não chega à VRAM (também no console),
  // e em 24 bits ele nem reordena os blocos: esses dois só conferem as faixas que existem
  for (const char* t : {"15bit-dma", "15bit", "24bit-dma"}) {
    auto c = exeCore(B + "mdec-frame-" + t + ".exe");
    runFrames(*c, 600);
    Image ref;
    const bool b24 = t[0] == '2', cpu = std::string(t) == "15bit";
    if (!readPpm(B + std::string("mdec-frame-") + (b24 ? "24bit" : "15bit") + ".ppm", ref)) { check(false, "falta o vram de mdec/frame"); continue; }
    const uint16_t* v = c->sys().gpu.vram();
    const int w = b24 ? 480 : (cpu ? 304 : 320);
    int equal = 0, maxd = 0;
    for (int y = 0; y < 240; y++)
      for (int x = 0; x < w; x++) {
        const uint8_t* q = &ref.rgb[size_t(y * 1024 + x) * 3];
        const uint16_t real = uint16_t((q[0] >> 3) | (q[1] >> 3) << 5 | (q[2] >> 3) << 10), mine = v[y * 1024 + x] & 0x7FFF;
        equal += mine == real;
        if (!b24) {
          for (int ch = 0; ch < 3; ch++) maxd = std::max(maxd, std::abs(int((mine >> (5 * ch)) & 31) - int((real >> (5 * ch)) & 31)));
        } else {
          const int dh = std::abs(int(mine >> 8) - int(real >> 8));
          maxd = std::max({maxd, std::abs(int(mine & 0xFF) - int(real & 0xFF)), std::min(dh, 128 - dh)});
        }
      }
    // mesmos limites do t9_cdmdec (o MDEC sozinho contra o mesmo vram.png)
    const bool good = b24 ? equal >= 78043 && maxd <= 4 : equal * 1000 >= w * 240 * 800 && maxd <= 1;
    check(good, std::string("mdec/frame ") + t + ": " + std::to_string(equal) + " meias-palavras iguais, maior diferença " + std::to_string(maxd));
    std::printf("mdec/frame %-9s VRAM: %d de %d meias-palavras iguais ao console (%.1f%%), maior diferença %d\n", t, equal, w * 240,
                100.0 * equal / (w * 240), maxd);
  }
  {
    // movie: 120 quadros do Bad Apple; o quadro guardado no console depende de quanto tempo o vídeo andou
    auto c = exeCore(B + "mdec-movie-24bit.exe");
    runFrames(*c, 400);
    int lit = 0;
    const uint16_t* v = c->sys().gpu.vram();
    for (int y = 0; y < 240; y++) for (int x = 0; x < 384; x++) lit += (v[y * 1024 + x] & 0x7FFF) != 0;
    check(lit > 1000, "mdec/movie: o vídeo aparece na VRAM");
    std::printf("mdec/movie (24 bits): vídeo decodificado (%d pontos acesos)\n", lit);
  }

  // ---------------------------------------------------------------- DMA
  {
    auto c = exeCore(E + "dma-chopping.exe");
    runFrames(*c, 1500);
    // ciclos de cada transferência (blocos sob pedido de 1 a 128 palavras, bloco único, chopping com janelas de
    // 1 a 128 palavras/ciclos) comparados com os do console; a marca "(GPU busy)" (fila da GPU ainda cheia no
    // fim) fica só informativa
    std::vector<std::string> g, e;
    for (const auto& l : lines(c->tty())) if (l.find("... took") != std::string::npos) g.push_back(l);
    for (const auto& l : lines(slurp(T + "dma/chopping/psx.log"))) if (l.find("... took") != std::string::npos) e.push_back(l);
    int near5 = 0, near10 = 0, busy = 0;
    double worst = 0;
    for (size_t i = 0; i < std::min(g.size(), e.size()); i++) {
      const double a = std::atof(g[i].c_str() + g[i].find("took") + 4), b = std::atof(e[i].c_str() + e[i].find("took") + 4);
      const double r = b > 0 ? std::fabs(a / b - 1) : 1;
      worst = std::max(worst, r);
      near5 += r <= 0.05;
      near10 += r <= 0.10;
      busy += (g[i].find("busy") != std::string::npos) == (e[i].find("busy") != std::string::npos);
    }
    check(g.size() == 130 && e.size() == 130 && near10 == 130 && near5 >= 128,
          "dma/chopping: " + std::to_string(near5) + " de 130 a 5% do console, " + std::to_string(near10) + " a 10%");
    std::printf("dma/chopping: %d de %zu tempos a 5%% do console, %d a 10%% (pior %.1f%%); \"GPU busy\" igual em %d\n", near5,
                e.size(), near10, worst * 100, busy);
  }

  // ---------------------------------------------------------------- disco de teste montado aqui
  {
    auto d0 = openDisc(dir + "/disco/sem_licenca.cue");
    check(d0 && cdrom::detectSystem(*d0) == "ps1", "disco sem a licença reconhecido pelo SYSTEM.CNF");
    auto d = openDisc(dir + "/disco/teste.cue");
    check(d && cdrom::detectSystem(*d) == "ps1", "disco de teste reconhecido como PlayStation");
    if (d) {
      Ps1Core c(std::move(d), nullptr);
      const bool started = runUntil(c, "Indigo: disco de teste", 2000);
      const int bootFrames = int(c.sys().now / 564480);
      check(started, "disco de teste: a BIOS deu a partida no JOGO.EXE pelo SYSTEM.CNF");
      std::printf("disco de teste: partida pela BIOS em %d quadros\n", bootFrames);
      runUntil(c, "leitura 3 ", 600);
      const Energy spu = runAudio(c, 200);
      // save state no meio das leituras do CD
      const auto st = c.saveState();
      runFrames(c, 60);
      const uint32_t h1 = frameHash(c);
      const auto st2 = c.saveState();
      c.loadState(st);
      check(c.saveState() == st, "disco de teste: salvar -> carregar -> salvar igual (CD lendo)");
      runFrames(c, 60);
      check(frameHash(c) == h1 && c.saveState() == st2, "disco de teste: carregar o estado repete o mesmo caminho");
      const bool cdda = runUntil(c, "cdda: faixa 2 tocando", 900);
      runFrames(c, 30);
      const Energy cd = runAudio(c, 200);
      check(cdda && cd.l > spu.l + 0.05 && cd.r > spu.r + 0.05 && spu.l > 0.01 && spu.r > 0.01,
            "disco de teste: som da SPU e da faixa de CD-DA");
      std::printf("disco de teste: som só da SPU %.3f/%.3f, com o CD-DA %.3f/%.3f (RMS esquerda/direita)\n", spu.l, spu.r, cd.l, cd.r);
      // quadro parado (os cubos param enquanto a faixa toca), comparado com o do Beetle no mesmo ponto do programa
      Image ref;
      if (readPpm(dir + "/ref/beetle-parado.ppm", ref)) {
        const Image a = frameOf(c);
        Image cut, rc;
        cut.w = a.w; cut.h = a.h - 40;
        cut.rgb.assign(a.rgb.begin() + long(size_t(a.w) * 40 * 3), a.rgb.end());
        rc.w = ref.w; rc.h = ref.h - 40;
        rc.rgb.assign(ref.rgb.begin() + long(size_t(ref.w) * 40 * 3), ref.rgb.end());
        const double sim = similarity(cut, rc, 8);
        check(sim > 0.995, "disco de teste: quadro " + std::to_string(sim * 100) + "% igual ao do Beetle");
        std::printf("disco de teste: quadro 3D (GTE + GPU) %.2f%% igual ao do Beetle PSX\n", sim * 100);
      } else {
        check(false, "falta ref/beetle-parado.ppm (t9_ps1int.sh)");
      }
      const std::string tty = c.tty();
      int oks = 0;
      for (size_t p = 0; (p = tty.find(": ok\n", p)) != std::string::npos; p++) oks++;
      check(tty.find("cd: textura lida (0 pontos diferentes)") != std::string::npos && tty.find("ERRO") == std::string::npos && oks >= 8,
            "disco de teste: textura e dados lidos do CD conferem");
      // controle: digital e analógico
      press(c, KEY_RIGHT | KEY_B, 6);
      check(c.tty().find("controle: tipo 4 botoes BFDF") != std::string::npos, "disco de teste: controle digital (direita + Xis)");
      c.pressAnalogButton();
      c.setAnalog(200, 60, 128, 128);
      c.setExtraButtons(true, false, false, true);
      runFrames(c, 10);
      check(c.tty().find("controle: tipo 7 botoes FEFB analog 200 60") != std::string::npos, "disco de teste: DualShock analógico (L2, R3)");
      c.setExtraButtons(false, false, false, false);
      // velocidade
      const auto stSpeed = c.saveState();
      const auto t0 = std::chrono::steady_clock::now();
      runFrames(c, 600);
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::printf("disco de teste: %d leituras do CD conferidas; velocidade %.0f quadros/s (%.1fx o console)\n", oks, 600 / secs, 600 / secs / 59.94);
      resolutionTests(c, stSpeed, 600 / secs);
    }
  }

  setFirmware("ps1", {});
  std::printf("RESULTADO: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
