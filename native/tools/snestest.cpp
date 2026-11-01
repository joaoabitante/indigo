// Testes do núcleo SNES com ROMs públicas.
//   indigo-snestest texto rom.sfc [quadros]
//       roda a ROM e, a cada quadro, procura as palavras PASS e FAIL nos mapas de telhas da VRAM (as ROMs de teste
//       do PeterLemon escrevem o resultado em texto, uma página por instrução); imprime "PASSOU n" ou "FALHOU n"
//   indigo-snestest tela rom.sfc quadros saida.ppm   grava a tela depois de N quadros
//   indigo-snestest roteiro rom.sfc quadros saida.ppm inicio:fim:teclas ...   aperta as teclas (máscara de Key,
//       em hexadecimal) do quadro inicio ao fim e grava a tela final
//   indigo-snestest compara rom.sfc quadros referencia.png   compara a tela depois de N quadros com a imagem de
//       referência (cores em 5 bits, como no SNES); imprime "IGUAL" ou "DIFERENTE n" (pixels diferentes)
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "../src/core/zip.h"
#include "../src/snes/snes.h"

using namespace indigo::snes;

static std::vector<uint8_t> readAll(const char* p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// PNG mínimo (8 bits por canal; RGB, RGBA ou paleta), só para as imagens de referência das ROMs de teste
static bool readPng(const std::vector<uint8_t>& f, int& w, int& h, std::vector<uint32_t>& rgb) {
  if (f.size() < 8 || f[1] != 'P' || f[2] != 'N' || f[3] != 'G') return false;
  auto be32 = [&](size_t i) { return (uint32_t(f[i]) << 24) | (uint32_t(f[i + 1]) << 16) | (uint32_t(f[i + 2]) << 8) | f[i + 3]; };
  std::vector<uint8_t> idat, pal;
  int depth = 0, type = 0;
  for (size_t i = 8; i + 12 <= f.size();) {
    const uint32_t len = be32(i);
    if (i + 12 + len > f.size()) return false;
    const std::string t(f.begin() + i + 4, f.begin() + i + 8);
    const uint8_t* d = f.data() + i + 8;
    if (t == "IHDR") { w = int(be32(i + 8)); h = int(be32(i + 12)); depth = d[8]; type = d[9]; if (d[12]) return false; }
    else if (t == "PLTE") pal.assign(d, d + len);
    else if (t == "IDAT") idat.insert(idat.end(), d, d + len);
    i += 12 + len;
  }
  const int ch = type == 2 ? 3 : type == 6 ? 4 : type == 3 ? 1 : 0;
  if (depth != 8 || !ch || w <= 0 || h <= 0 || w > 4096 || h > 4096 || idat.size() < 2) return false;
  const size_t stride = size_t(w) * ch;
  std::vector<uint8_t> raw;
  if (!indigo::zip::inflate(idat.data() + 2, idat.size() - 2, raw, (stride + 1) * h) || raw.size() < (stride + 1) * h) return false;
  std::vector<uint8_t> img(stride * h);
  for (int y = 0; y < h; y++) {
    const uint8_t ft = raw[y * (stride + 1)];
    const uint8_t* in = &raw[y * (stride + 1) + 1];
    uint8_t* out = &img[y * stride];
    const uint8_t* up = y ? &img[(y - 1) * stride] : nullptr;
    for (size_t x = 0; x < stride; x++) {
      const int a = x >= size_t(ch) ? out[x - ch] : 0, b = up ? up[x] : 0, c = (up && x >= size_t(ch)) ? up[x - ch] : 0;
      int pr;
      switch (ft) {
        case 0: pr = 0; break;
        case 1: pr = a; break;
        case 2: pr = b; break;
        case 3: pr = (a + b) / 2; break;
        case 4: { const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c); pr = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c; break; }
        default: return false;
      }
      out[x] = static_cast<uint8_t>(in[x] + pr);
    }
  }
  rgb.assign(size_t(w) * h, 0);
  for (size_t i = 0; i < rgb.size(); i++) {
    const uint8_t* p = &img[i * ch];
    if (type == 3) { const size_t k = size_t(p[0]) * 3; if (k + 2 >= pal.size()) return false; p = &pal[k]; }
    rgb[i] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "uso: %s texto|tela rom [quadros] [saida.ppm]\n", argv[0]); return 2; }
  const std::string mode = argv[1];
  const auto rom = readAll(argv[2]);
  const int frames = argc > 3 ? std::atoi(argv[3]) : 1200;
  SNESCore core(rom, nullptr);
  SNES& s = core.snes();
  if (mode == "compara") {
    if (argc < 5) return 2;
    int w = 0, h = 0;
    std::vector<uint32_t> ref;
    if (!readPng(readAll(argv[4]), w, h, ref) || w != 256 || h < 224) { std::printf("REFERENCIA INVALIDA\n"); return 2; }
    for (int f = 0; f < frames; f++) core.runFrame();
    int diff = 0;
    for (int i = 0; i < 256 * 224; i++) if ((s.ppu.fb[i] & 0xF8F8F8) != (ref[i] & 0xF8F8F8)) diff++;
    if (diff) { std::printf("DIFERENTE %d\n", diff); return 1; }
    std::printf("IGUAL\n");
    return 0;
  }
  if (mode == "tela" || mode == "roteiro") {
    struct Step { int from, to; uint32_t keys; };
    std::vector<Step> script;
    for (int k = 5; mode == "roteiro" && k < argc; k++) {
      Step st{};
      if (std::sscanf(argv[k], "%d:%d:%x", &st.from, &st.to, &st.keys) == 3) script.push_back(st);
    }
    for (int f = 0; f < frames; f++) {
      uint32_t keys = 0;
      for (const auto& st : script) if (f >= st.from && f <= st.to) keys |= st.keys;
      core.setKeys(keys);
      core.runFrame();
    }
    if (std::getenv("SNES_OAM")) {
      const uint8_t* oam = s.ppu.oam;
      for (int i = 0; i < 128; i++) {
        const uint8_t hi = static_cast<uint8_t>(oam[512 + (i >> 2)] >> ((i & 3) * 2));
        int x = oam[i * 4] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        if (oam[i * 4 + 1] >= 224 && oam[i * 4 + 1] < 240) continue;
        std::printf("spr %3d x=%4d y=%3d tile=%02X attr=%02X big=%d\n", i, x, oam[i * 4 + 1], oam[i * 4 + 2], oam[i * 4 + 3], (hi >> 1) & 1);
      }
    }
    if (argc > 4) {
      FILE* o = std::fopen(argv[4], "wb");
      std::fprintf(o, "P6\n256 224\n255\n");
      for (int i = 0; i < 256 * 224; i++) { const uint32_t c = s.ppu.fb[i]; const uint8_t rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)}; std::fwrite(rgb, 1, 3, o); }
      std::fclose(o);
    }
    return 0;
  }
  // cada ocorrência de PASS/FAIL na VRAM é identificada pelo endereço; conta as distintas ao longo da execução,
  // reconhecendo páginas novas quando o texto ao redor muda
  std::set<std::string> passes, fails;
  auto scan = [&]() {
    const uint8_t* v = s.ppu.vram;
    for (int w = 0; w + 4 < 0x8000; w++) {
      auto ch = [&](int k) { return static_cast<char>(v[(w + k) * 2]); };
      const bool p = ch(0) == 'P' && ch(1) == 'A' && ch(2) == 'S' && ch(3) == 'S';
      const bool fl = ch(0) == 'F' && ch(1) == 'A' && ch(2) == 'I' && ch(3) == 'L';
      if (!p && !fl) continue;
      // contexto: a linha inteira de 32 telhas antes da palavra identifica o caso
      std::string key = std::to_string(w) + ":";
      const int row = w & ~31;
      for (int k = row - 32 * 8; k < row + 32; k++) if (k >= 0) key.push_back(static_cast<char>(v[k * 2]));
      (p ? passes : fails).insert(key);
    }
  };
  for (int f = 0; f < frames; f++) { core.runFrame(); scan(); }
  if (!fails.empty()) { std::printf("FALHOU %zu (passaram %zu)\n", fails.size(), passes.size()); return 1; }
  if (passes.empty()) { std::printf("SEM RESULTADO\n"); return 1; }
  std::printf("PASSOU %zu\n", passes.size());
  return 0;
}
