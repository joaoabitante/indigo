// Testes do Neo Geo (pasta de testes como argumento, normalmente /root/indigo-env/roms/neogeo, preparada pelo
// t7_neogeo.sh):
//  1. quadros: cada pacote de lista.txt (exemplos do ngdevkit) roda com a nullbios e é comparado ponto a ponto com
//     os quadros do Geolith (ref/<nome>.ref, gravados pelo t7_neogeo_ref.c), no formato .zip do MAME e no .neo;
//  2. .zip e .neo do mesmo jogo dão os mesmos quadros e o mesmo som;
//  3. save state: salvar -> rodar -> carregar -> rodar dá a mesma tela e o mesmo som; salvar -> carregar ->
//     salvar dá os mesmos bytes; estados truncados/danificados são recusados sem travar;
//  4. robustez: pacotes e .neo aleatórios/danificados não travam;
//  5. som: o volume (RMS) de cada trecho comparado com o do Geolith nos exemplos de som.
//  6. dois jogadores: o controle 2 chega ao REG_P2CNT e aos botões do jogador 2.
// Uso: indigo-t7_neogeo pasta            (todos os testes)
//      indigo-t7_neogeo pasta rodar arquivo quadros saida.ppm [teclas]   (depuração: grava o último quadro)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/zip.h"
#include "../src/neogeo/neogeo.h"

using indigo::neogeo::NeoGeo;

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

// mesma regra de teclas do frontend do Geolith: apertadas nos períodos ímpares de 30 quadros
uint32_t keysAt(int frame, uint32_t keys) { return (keys && ((frame / 30) % 2)) ? keys : 0; }

void writePpm(const std::string& path, const uint32_t* fb, int w, int h) {
  std::ofstream f(path, std::ios::binary);
  f << "P6\n" << w << " " << h << "\n255\n";
  for (int i = 0; i < w * h; i++) {
    const uint32_t p = fb[i];
    const char c[3] = {static_cast<char>(p & 255), static_cast<char>((p >> 8) & 255), static_cast<char>((p >> 16) & 255)};
    f.write(c, 3);
  }
}

uint64_t fbHash(const uint32_t* fb, int n) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n; i++) { h ^= fb[i]; h *= 1099511628211ull; }
  return h;
}

std::string nameOf(const std::string& path) {
  const size_t s = path.find_last_of('/');
  return s == std::string::npos ? path : path.substr(s + 1);
}

struct Ref {
  int w = 0, h = 0;
  std::vector<std::vector<uint8_t>> frames;
};
bool readRef(const std::string& path, Ref& r) {
  std::vector<uint8_t> d;
  if (!readFile(path, d) || d.size() < 4) return false;
  r.w = d[0] | (d[1] << 8);
  r.h = d[2] | (d[3] << 8);
  const size_t n = 4 + static_cast<size_t>(r.w) * r.h * 3;
  for (size_t o = 0; o + n <= d.size(); o += n) r.frames.emplace_back(d.begin() + static_cast<long>(o + 4), d.begin() + static_cast<long>(o + n));
  return !r.frames.empty();
}

// ------------------------------------------------------------------ 1/2. quadros contra o Geolith
struct Item {
  std::string name, file;
  int frames = 0, step = 0;
  uint32_t keys = 0;
  int minEqual = 100;  // % mínimo de quadros idênticos (ROMs sensíveis a poucos ciclos de CPU)
};

// roda e compara; devolve o número de quadros diferentes; grava a sequência de hashes em hashes
int runCompare(const std::string& dir, const Item& it, const std::string& file, const Ref* ref, std::vector<uint64_t>& hashes,
               std::vector<float>* audio = nullptr) {
  std::vector<uint8_t> rom;
  if (!readFile(dir + "/" + file, rom)) { check(false, "lê " + file); return -1; }
  std::unique_ptr<NeoGeo> ng;
  try {
    ng = std::make_unique<NeoGeo>(rom, nameOf(file), nullptr);
  } catch (const std::exception& e) {
    check(false, it.name + ": " + e.what());
    return -1;
  }
  ng->setSampleRate(44100);
  int bad = 0, k = 0;
  for (int f = 0; f < it.frames; f++) {
    ng->setKeys(keysAt(f, it.keys));
    ng->runFrame();
    if (audio) {
      const size_t n = ng->audioAvailable();
      std::vector<float> l(n), r(n);
      ng->pullAudio(l.data(), r.data(), n);
      for (size_t i = 0; i < n; i++) { audio->push_back(l[i]); audio->push_back(r[i]); }
    } else {
      const size_t n = ng->audioAvailable();
      std::vector<float> l(n), r(n);
      ng->pullAudio(l.data(), r.data(), n);
    }
    if ((f + 1) % it.step) continue;
    const uint32_t* fb = ng->lspc.fb;
    hashes.push_back(fbHash(fb, indigo::neogeo::LSPC::OUT_W * indigo::neogeo::LSPC::OUT_H));
    if (ref && k < static_cast<int>(ref->frames.size())) {
      const std::vector<uint8_t>& rf = ref->frames[k];
      int diff = 0, first = -1;
      for (int i = 0; i < ref->w * ref->h; i++) {
        const uint32_t p = fb[i];
        if ((p & 255) != rf[i * 3] || ((p >> 8) & 255) != rf[i * 3 + 1] || ((p >> 16) & 255) != rf[i * 3 + 2]) {
          if (first < 0) first = i;
          diff++;
        }
      }
      if (diff) {
        bad++;
        if (bad <= 2)
          std::printf("  %s quadro %d: %d pontos diferentes (primeiro em %d,%d)\n", it.name.c_str(), f + 1, diff,
                      first % ref->w, first / ref->w);
        if (std::getenv("NGDUMP")) writePpm("/tmp/ng_" + it.name + "_" + std::to_string(f + 1) + ".ppm", fb, ref->w, ref->h);
      }
    }
    k++;
  }
  return bad;
}

void testFrames(const std::string& dir, const std::string& lista, const std::string& prefixo) {
  std::ifstream lst(dir + "/" + lista);
  std::string line;
  while (std::getline(lst, line)) {
    std::istringstream ss(line);
    Item it;
    if (!(ss >> it.name >> it.file >> it.frames >> it.step)) continue;
    ss >> it.keys >> it.minEqual;
    Ref ref;
    if (!readRef(dir + "/ref/" + prefixo + it.name + ".ref", ref)) { check(false, "referência de " + it.name); continue; }
    it.name = prefixo + it.name;
    std::vector<uint64_t> hz, hn;
    const int badZip = runCompare(dir, it, "pacotes/" + it.file + ".zip", &ref, hz);
    const int badNeo = runCompare(dir, it, "pacotes/" + it.file + ".neo", &ref, hn);
    const int total = static_cast<int>(ref.frames.size());
    std::printf("%-22s %d/%d quadros iguais ao Geolith (.zip), %d/%d (.neo)\n", it.name.c_str(), total - std::max(badZip, 0),
                total, total - std::max(badNeo, 0), total);
    check(badZip >= 0 && (total - badZip) * 100 >= total * it.minEqual, it.name + ": quadros iguais ao Geolith (.zip)");
    check(badNeo >= 0 && (total - badNeo) * 100 >= total * it.minEqual, it.name + ": quadros iguais ao Geolith (.neo)");
    check(hz == hn, it.name + ": .zip e .neo dão os mesmos quadros");
  }
}

// ------------------------------------------------------------------ 3. save state
void runFrames(NeoGeo& ng, int n, int start, uint32_t keys, std::vector<uint64_t>& hashes, std::vector<float>& audio) {
  for (int f = 0; f < n; f++) {
    ng.setKeys(keysAt(start + f, keys));
    ng.runFrame();
    hashes.push_back(fbHash(ng.lspc.fb, indigo::neogeo::LSPC::OUT_W * indigo::neogeo::LSPC::OUT_H));
    const size_t k = ng.audioAvailable();
    std::vector<float> l(k), r(k);
    ng.pullAudio(l.data(), r.data(), k);
    audio.insert(audio.end(), l.begin(), l.end());
    audio.insert(audio.end(), r.begin(), r.end());
  }
}

void testState(const std::string& dir, const std::string& file, uint32_t keys) {
  std::vector<uint8_t> rom;
  if (!readFile(dir + "/pacotes/" + file, rom)) { check(false, "lê " + file); return; }
  NeoGeo ng(rom, file, nullptr);
  ng.setSampleRate(48000);
  std::vector<uint64_t> h0, h1, h2;
  std::vector<float> a0, a1, a2;
  runFrames(ng, 200, 0, keys, h0, a0);
  const std::vector<uint8_t> s = ng.saveState();
  runFrames(ng, 120, 200, keys, h1, a1);
  ng.loadState(s);
  runFrames(ng, 120, 200, keys, h2, a2);
  check(h1 == h2, file + ": save state - mesmos quadros depois de carregar");
  check(a1 == a2, file + ": save state - mesmo som depois de carregar");
  ng.loadState(s);
  check(ng.saveState() == s, file + ": salvar -> carregar -> salvar dá os mesmos bytes");
  // estados danificados: truncados e com bytes trocados; o núcleo recusa ou continua rodando
  std::mt19937 rng(1234);
  int refused = 0, ran = 0;
  for (int t = 0; t < 60; t++) {
    std::vector<uint8_t> bad = s;
    if (t < 20) bad.resize(rng() % s.size());
    else for (int k = 0; k < 1 + t; k++) bad[rng() % bad.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
    try {
      ng.loadState(bad);
      for (int f = 0; f < 3; f++) ng.runFrame();
      ran++;
    } catch (const std::runtime_error&) {
      refused++;
    }
    ng.loadState(s);
  }
  check(refused + ran == 60, file + ": estados danificados recusados ou rodados sem travar");
  // reset no meio do jogo: o som continua saindo na taxa certa
  ng.reset();
  std::vector<uint64_t> h3;
  std::vector<float> a3;
  runFrames(ng, 60, 0, keys, h3, a3);
  const double expect = 60 / ng.fps() * 48000 * 2;
  check(std::fabs(static_cast<double>(a3.size()) - expect) < expect * 0.02, file + ": som na taxa certa depois do reset");
  std::printf("save state (%s): determinístico, %d estados danificados recusados, %d rodaram\n", file.c_str(), refused, ran);
}

// ------------------------------------------------------------------ 4. robustez
void testRobust(const std::string& dir) {
  std::mt19937 rng(99);
  int ok = 0, refused = 0;
  // .neo com programa, gráficos e som aleatórios
  for (int t = 0; t < 12; t++) {
    const uint32_t ps = 0x1000u << (rng() % 10), ss = 0x800u << (rng() % 7), ms = 0x400u << (rng() % 8),
                   vs = rng() % 0x40000, cs = (0x80u << (rng() % 14));
    std::vector<uint8_t> neo(4096 + ps + ss + ms + vs + cs);
    for (auto& b : neo) b = static_cast<uint8_t>(rng());
    neo[0] = 'N'; neo[1] = 'E'; neo[2] = 'O'; neo[3] = 1;
    const uint32_t sizes[6] = {ps, ss, ms, vs, 0, cs};
    for (int i = 0; i < 6; i++) for (int b = 0; b < 4; b++) neo[4 + i * 4 + b] = static_cast<uint8_t>(sizes[i] >> (8 * b));
    if (t & 1) {  // vetores de reset apontando para o programa (para rodar código aleatório de verdade)
      for (int i = 0; i < 8; i++) neo[4096 + i] = 0;
    }
    try {
      NeoGeo ng(neo, "lixo.neo", nullptr);
      for (int f = 0; f < 40; f++) { ng.setKeys(rng()); ng.runFrame(); }
      ok++;
    } catch (const std::runtime_error&) {
      refused++;
    }
  }
  // .neo truncados e cabeçalhos absurdos
  for (int t = 0; t < 8; t++) {
    std::vector<uint8_t> neo(4096 + (rng() % 5000));
    neo[0] = 'N'; neo[1] = 'E'; neo[2] = 'O'; neo[3] = 1;
    for (int i = 4; i < 28; i++) neo[i] = static_cast<uint8_t>(rng());
    try { NeoGeo ng(neo, "x.neo", nullptr); ng.runFrame(); ok++; } catch (const std::runtime_error&) { refused++; }
  }
  // .zip de exemplo com bytes trocados
  std::vector<uint8_t> z;
  if (readFile(dir + "/pacotes/02_sprite.zip", z)) {
    for (int t = 0; t < 10; t++) {
      std::vector<uint8_t> bad = z;
      for (int k = 0; k < 8; k++) bad[rng() % bad.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
      try { NeoGeo ng(bad, "02_sprite.zip", nullptr); for (int f = 0; f < 10; f++) ng.runFrame(); ok++; }
      catch (const std::runtime_error&) { refused++; }
    }
  }
  check(ok + refused == 30, "robustez: pacotes aleatórios/danificados não travam");
  std::printf("robustez: %d pacotes aleatórios rodaram, %d recusados com mensagem\n", ok, refused);
}

// pacotes falsos (conteúdo aleatório com os nomes/tamanhos do MAME): o caminho de carga e decifração de cada tipo
// de cartucho roda inteiro; o resultado não pode ser conferido sem as ROMs de verdade
void testFake(const std::string& dir) {
  std::ifstream lst(dir + "/falsos/lista.txt");
  std::string line;
  int n = 0;
  while (std::getline(lst, line)) {
    std::istringstream ss(line);
    std::string name, expect;
    int board = 0;
    if (!(ss >> name >> expect >> board)) continue;
    std::vector<uint8_t> zip;
    if (!readFile(dir + "/falsos/" + name + ".zip", zip)) { check(false, "lê falsos/" + name); continue; }
    check(indigo::neogeo::isNeoGeoSet(zip, name + ".zip"), name + ": reconhecido como pacote de Neo Geo");
    std::string err;
    try {
      NeoGeo ng(zip, name + ".zip", nullptr);
      check(expect == "ok", name + ": devia recusar");
      check(ng.cart.board == board, name + ": placa " + std::to_string(ng.cart.board) + " (esperada " + std::to_string(board) + ")");
      check(ng.cart.setName == name, name + ": pacote escolhido " + ng.cart.setName);
      for (int f = 0; f < 20; f++) { ng.setKeys(f & 8 ? 0xFFF : 0); ng.runFrame(); }
      const std::vector<uint8_t> s = ng.saveState();
      ng.runFrame();
      ng.loadState(s);
      check(ng.saveState() == s, name + ": save state");
    } catch (const std::runtime_error& e) {
      err = e.what();
      check(expect == "erro", name + ": " + err);
    }
    if (!err.empty()) std::printf("  %s: recusado (\"%s\")\n", name.c_str(), err.c_str());
    n++;
  }
  std::printf("pacotes falsos (carga, decifração e quadros com conteúdo aleatório): %d\n", n);
}

// ------------------------------------------------------------------ 5. som contra o Geolith (envelope de volume)
void testAudio(const std::string& dir) {
  std::ifstream lst(dir + "/lista.txt");
  std::string line;
  while (std::getline(lst, line)) {
    std::istringstream ss(line);
    Item it;
    if (!(ss >> it.name >> it.file >> it.frames >> it.step)) continue;
    ss >> it.keys;
    std::vector<uint8_t> aud;
    if (!readFile(dir + "/ref/" + it.name + ".aud", aud)) continue;
    const size_t refN = aud.size() / 4;
    it.frames = 900;
    it.step = 900;
    std::vector<uint64_t> hz;
    std::vector<float> mine;
    runCompare(dir, it, "pacotes/" + it.file + ".zip", nullptr, hz, &mine);
    // trechos de 15 quadros: RMS do Geolith (16 bits) e do Índigo (float, 44100 Hz)
    const int W = 60;
    const double refPerFrame = static_cast<double>(refN) / 900.0, minePerFrame = (mine.size() / 2) / 900.0;
    int windows = 0, agree = 0, silentRef = 0;
    double sumR = 0, sumM = 0;
    for (int w = 0; w + W <= 900; w += W) {
      double er = 0, em = 0;
      const size_t r0 = static_cast<size_t>(w * refPerFrame), r1 = static_cast<size_t>((w + W) * refPerFrame);
      for (size_t i = r0; i < r1 && i < refN; i++) {
        const int16_t l = static_cast<int16_t>(aud[4 * i] | (aud[4 * i + 1] << 8));
        const int16_t r = static_cast<int16_t>(aud[4 * i + 2] | (aud[4 * i + 3] << 8));
        er += (l / 32768.0) * (l / 32768.0) + (r / 32768.0) * (r / 32768.0);
      }
      const size_t m0 = static_cast<size_t>(w * minePerFrame), m1 = static_cast<size_t>((w + W) * minePerFrame);
      for (size_t i = m0; i < m1 && 2 * i + 1 < mine.size(); i++) em += mine[2 * i] * mine[2 * i] + mine[2 * i + 1] * mine[2 * i + 1];
      er = std::sqrt(er / std::max<double>(1, 2.0 * (r1 - r0)));
      em = std::sqrt(em / std::max<double>(1, 2.0 * (m1 - m0)));
      sumR += er; sumM += em;
      windows++;
      if (er < 0.002) { silentRef++; if (em < 0.01) agree++; }
      else if (em > er / 1.5 && em < er * 1.5) agree++;
    }
    std::printf("som %-14s %d/%d trechos com volume parecido ao do Geolith (média %.4f x %.4f, %d trechos mudos)\n",
                it.name.c_str(), agree, windows, sumM / windows, sumR / windows, silentRef);
    check(agree * 10 >= windows * 8, it.name + ": volume do som parecido com o do Geolith");
  }
}

int runMode(const std::string& dir, int argc, char** argv) {
  if (argc < 6) return 2;
  std::vector<uint8_t> rom;
  if (!readFile(argv[3], rom)) { std::printf("não leu %s\n", argv[3]); return 1; }
  NeoGeo ng(rom, nameOf(argv[3]), nullptr);
  std::printf("%s | %s | P %zu S %zu M %zu V %zu/%zu C %zu | placa %d\n", ng.cart.title.c_str(), ng.cart.note.c_str(),
              ng.cart.p.size(), ng.cart.s.size(), ng.cart.m.size(), ng.cart.v1.size(), ng.cart.v2.size(), ng.cart.c.size(),
              ng.cart.board);
  const int frames = std::atoi(argv[4]);
  const uint32_t keys = argc > 6 ? static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 0)) : 0;
  double e = 0;
  size_t total = 0;
  for (int f = 0; f < frames; f++) {
    ng.setKeys(keysAt(f, keys));
    ng.runFrame();
    const size_t n = ng.audioAvailable();
    std::vector<float> l(n), r(n);
    ng.pullAudio(l.data(), r.data(), n);
    for (size_t i = 0; i < n; i++) e += l[i] * l[i] + r[i] * r[i];
    total += n;
    if (std::getenv("NGZ80") && f < 8) std::printf("q%d z80 %04X\n", f, ng.z80.pc);
    if ((f + 1) % 60 == 0) {
      std::printf("quadro %d: som RMS %.4f, PC %06X, Z80 PC %04X\n", f + 1, std::sqrt(e / std::max<size_t>(1, 2 * total)), ng.cpu.pc, ng.z80.pc);
      e = 0;
      total = 0;
    }
  }
  writePpm(argv[5], ng.lspc.fb, indigo::neogeo::LSPC::OUT_W, indigo::neogeo::LSPC::OUT_H);
  (void)dir;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // linha a linha (acompanhar o progresso nos testes longos)
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/neogeo";
  std::vector<uint8_t> bios;
  if (!readFile(dir + "/bios/neogeo.zip", bios)) {
    std::printf("Neo Geo: pasta de testes %s não encontrada; pulando (rode tools/t7_neogeo.sh)\n", dir.c_str());
    return 0;
  }
  indigo::setFirmware("neogeo", bios);
  if (argc > 2 && std::string(argv[2]) == "rodar") return runMode(dir, argc, argv);
  // detecção e mensagens
  {
    std::vector<uint8_t> z, neo;
    readFile(dir + "/pacotes/02_sprite.zip", z);
    readFile(dir + "/pacotes/02_sprite.neo", neo);
    check(indigo::neogeo::isNeoGeoBiosZip(bios, "neogeo.zip"), "neogeo.zip reconhecido como BIOS");
    check(!indigo::neogeo::isNeoGeoSet(bios, "neogeo.zip"), "neogeo.zip não é jogo");
    check(indigo::neogeo::isNeoGeoSet(z, "02_sprite.zip") && !indigo::neogeo::isNeoGeoBiosZip(z, "02_sprite.zip"),
          ".zip de jogo reconhecido");
    check(indigo::neogeo::isNeoGeoRom(neo, "02_sprite.neo") && indigo::neogeo::isNeoGeoRom(z, "02_sprite.zip"),
          ".neo e .zip reconhecidos por isNeoGeoRom");
    indigo::setFirmware("neogeo", {});
    std::string msg;
    try { NeoGeo ng(neo, "02_sprite.neo", nullptr); } catch (const std::runtime_error& e) { msg = e.what(); }
    check(msg == "Abra o arquivo neogeo.zip (BIOS do Neo Geo) como um jogo", "sem BIOS: mensagem pedindo o neogeo.zip");
    indigo::setFirmware("neogeo", bios);
    // save: RAM de backup + cartão de memória, exportado e importado
    NeoGeo ng(neo, "02_sprite.neo", nullptr);
    for (int f = 0; f < 120; f++) ng.runFrame();
    std::vector<uint8_t> sv = ng.exportSave();
    check(sv.size() == 0x10800, "save do MVS: 64 KB de RAM de backup + 2 KB de cartão");
    sv[0x10000 + 0x7F0] ^= 0x5A;
    NeoGeo ng2(neo, "02_sprite.neo", &sv);
    check(ng2.exportSave() == sv, "save importado volta igual");
    // dois jogadores: REG_P2CNT, start do jogador 2 (REG_STATUS_B) e moeda 2 (REG_STATUS_A) sem mexer no 1
    indigo::neogeo::NeoGeoCore core(neo, "02_sprite.neo", nullptr);
    NeoGeo& m = core.ng();
    check(core.players() == 2, "2 jogadores: players() == 2");
    core.setKeys(indigo::KEY_UP);
    core.setKeys2(indigo::KEY_B | indigo::KEY_DOWN | indigo::KEY_START | indigo::KEY_SELECT);
    check(m.read8(0x300000) == 0xFE && m.read8(0x340000) == 0xED && (m.read16(0x340000) >> 8) == 0xED,
          "2 jogadores: REG_P1CNT e REG_P2CNT");
    check((m.read8(0x380000) & 0x0F) == 0x0B && (m.read8(0x320001) & 0x03) == 0x01, "2 jogadores: start e moeda do jogador 2");
    core.setKeys2(0);
    check(m.read8(0x340000) == 0xFF && (m.read8(0x380000) & 0x0F) == 0x0F, "2 jogadores: jogador 2 solto");
  }
  testFrames(dir, "lista.txt", "");
  testAudio(dir);
  // modo AES: a mesma régua com a BIOS de console da nullbios
  std::vector<uint8_t> aes;
  if (readFile(dir + "/bios-aes/aes.zip", aes)) {
    indigo::setFirmware("neogeo", aes);
    testFrames(dir, "lista-aes.txt", "aes-");
    indigo::setFirmware("neogeo", bios);
  }
  testState(dir, "07_attract_and_game.zip", 12);
  testState(dir, "16_sound_music.neo", 0);
  testRobust(dir);
  testFake(dir);
  std::printf("RESULTADO neogeo: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
