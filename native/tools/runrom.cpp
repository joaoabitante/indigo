// Roda uma ROM sem interface e imprime, a cada quadro, o hash da imagem e do som.
// A saída tem o mesmo formato de native/tools/jsref.mjs, que faz o mesmo com o núcleo JavaScript:
// comparar as duas linha a linha prova que o porte em C++ se comporta igual.
//
// uso: indigo-run rom quadros [auto|dmg|cgb|sms|gg|gba] [teclasACada] [saida.ppm]
//      indigo-run --statetest rom
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../src/gb/gb.h"
#include "../src/gba/gba.h"
#include "../src/ngp/ngp.h"
#include "../src/sms/sms.h"

using namespace indigo;

namespace {

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

uint32_t fnvWords(const uint32_t* p, size_t n, uint32_t h = 0x811C9DC5u) {
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

int keysFor(int f, int every) { return (every && (f % every) < 3) ? (1 << (f % 8)) : 0; }

// modo forçado (dmg/cgb para Game Boy; sms/gg para Sega) ou detecção automática pela ROM
std::unique_ptr<Core> makeCore(const std::vector<uint8_t>& rom, const std::string& path, const std::string& mode) {
  if (mode == "dmg") return std::make_unique<gb::GBCore>(rom, nullptr, gb::Mode::Dmg);
  if (mode == "cgb") return std::make_unique<gb::GBCore>(rom, nullptr, gb::Mode::Cgb);
  if (mode == "sms" || mode == "gg") return std::make_unique<sms::SMSCore>(rom, mode == "gg", nullptr);
  if (mode == "gba") {
    // INDIGO_GBA_BIOS=arquivo: usa uma BIOS de verdade em vez da própria (jsref.mjs lê a mesma variável)
    const char* bp = std::getenv("INDIGO_GBA_BIOS");
    const auto bios = bp ? readFile(bp) : std::vector<uint8_t>{};
    return std::make_unique<gba::GBACore>(rom, nullptr, bios.size() >= 0x4000 ? &bios : nullptr);
  }
  return createCore(rom, path, nullptr);
}

void writePpm(const char* path, const uint32_t* fb, int w, int h) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    const uint8_t px[3] = {uint8_t(fb[i]), uint8_t(fb[i] >> 8), uint8_t(fb[i] >> 16)};
    std::fwrite(px, 1, 3, f);
  }
  std::fclose(f);
}

int stateTest(const char* romPath) {
  const auto rom = readFile(romPath);
  auto mk = [&] { return makeCore(rom, romPath, "auto"); };
  auto a = mk();
  if (!a) { std::fprintf(stderr, "ROM não reconhecida\n"); return 2; }
  const size_t px = size_t(a->width()) * a->height();
  for (int f = 0; f < 300; f++) { a->setKeys(keysFor(f, 7)); a->runFrame(); }
  const auto st = a->saveState();
  std::vector<uint32_t> ha, hb;
  for (int f = 300; f < 600; f++) { a->setKeys(keysFor(f, 7)); a->runFrame(); if (f % 50 == 49) ha.push_back(fnvWords(a->framebuffer(), px)); }
  auto b = mk();
  b->runFrame();
  b->loadState(st);
  for (int f = 300; f < 600; f++) { b->setKeys(keysFor(f, 7)); b->runFrame(); if (f % 50 == 49) hb.push_back(fnvWords(b->framebuffer(), px)); }
  const bool same = ha == hb;
  std::printf("%s %s (%zu bytes)\n", romPath, same ? "IGUAL" : "DIFERENTE", st.size());
  return same ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // relógio fixo (o Neo Geo Pocket lê data e hora; jsref.mjs fixa o mesmo instante)
  const ngp::ClockTime fixedClock{2026, 10, 19, 12, 34, 56, 1};
  ngp::setFixedClock(&fixedClock);
  // o relógio dos cartuchos de GBA usa a hora local, como new Date(2026, 9, 19, 12, 34, 56) no JS
  std::tm fixedTm{};
  fixedTm.tm_year = 2026 - 1900; fixedTm.tm_mon = 9; fixedTm.tm_mday = 19;
  fixedTm.tm_hour = 12; fixedTm.tm_min = 34; fixedTm.tm_sec = 56; fixedTm.tm_isdst = -1;
  gba::setFixedNowMs(static_cast<double>(std::mktime(&fixedTm)) * 1000.0);
  if (argc >= 3 && std::strcmp(argv[1], "--statetest") == 0) return stateTest(argv[2]);
  if (argc < 3) {
    std::fprintf(stderr, "uso: %s rom quadros [auto|dmg|cgb|sms|gg|gba] [teclasACada] [saida.ppm]\n", argv[0]);
    return 2;
  }
  const auto rom = readFile(argv[1]);
  const int frames = std::atoi(argv[2]);
  const std::string mode = argc > 3 ? argv[3] : "auto";
  const int every = argc > 4 ? std::atoi(argv[4]) : 0;

  auto core = makeCore(rom, argv[1], mode);
  if (!core) { std::fprintf(stderr, "ROM não reconhecida\n"); return 2; }
  const size_t px = size_t(core->width()) * core->height();
  std::vector<float> L, R, wav;
  // INDIGO_WAV=arquivo.wav grava o som (canal esquerdo, 48 kHz, 16 bits) para conferir frequências e níveis
  const char* wavPath = std::getenv("INDIGO_WAV");
  if (wavPath) core->setSampleRate(48000);
  for (int f = 0; f < frames; f++) {
    core->setKeys(keysFor(f, every));
    core->runFrame();
    const size_t n = core->audioAvailable();
    L.resize(n); R.resize(n);
    core->pullAudio(L.data(), R.data(), n);
    if (wavPath) wav.insert(wav.end(), L.begin(), L.end());
    uint32_t ah = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) {
      uint32_t bl, br;
      std::memcpy(&bl, &L[i], 4); std::memcpy(&br, &R[i], 4);
      ah ^= bl; ah *= 0x01000193u; ah ^= br; ah *= 0x01000193u;
    }
    std::printf("%d %08x %08x %zu\n", f, fnvWords(core->framebuffer(), px), ah, n);
  }
  std::string serial;
  if (auto* g = dynamic_cast<gb::GBCore*>(core.get())) serial = g->gb().serialOut;
  for (char& c : serial) if (c == '\n' || c == '\r') c = ' ';
  std::printf("serial: %s\n", serial.c_str());
  const std::vector<uint8_t> save = core->hasSave() ? core->exportSave() : std::vector<uint8_t>{};
  uint32_t sh = 0x811C9DC5u;
  for (uint8_t b : save) { sh ^= b; sh *= 0x01000193u; }
  std::printf("save: %zu %08x\n", save.size(), sh);
  if (argc > 5) writePpm(argv[5], core->framebuffer(), core->width(), core->height());
  if (wavPath) {
    if (FILE* w = std::fopen(wavPath, "wb")) {
      const uint32_t bytes = static_cast<uint32_t>(wav.size() * 2), rate = 48000, byteRate = rate * 2;
      const uint32_t riff = 36 + bytes, fmtLen = 16;
      const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
      std::fwrite("RIFF", 1, 4, w); std::fwrite(&riff, 4, 1, w); std::fwrite("WAVEfmt ", 1, 8, w);
      std::fwrite(&fmtLen, 4, 1, w); std::fwrite(&pcm, 2, 1, w); std::fwrite(&ch, 2, 1, w); std::fwrite(&rate, 4, 1, w);
      std::fwrite(&byteRate, 4, 1, w); std::fwrite(&align, 2, 1, w); std::fwrite(&bits, 2, 1, w);
      std::fwrite("data", 1, 4, w); std::fwrite(&bytes, 4, 1, w);
      for (float v : wav) {
        const int16_t s16 = static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        std::fwrite(&s16, 2, 1, w);
      }
      std::fclose(w);
    }
  }
  return 0;
}
