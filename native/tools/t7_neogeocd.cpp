// Testes do Neo Geo CD (pasta de testes como argumento, normalmente /root/indigo-env/roms/neogeocd, preparada pelo
// t7_neogeocd.sh):
//  1. BIOS: isNeoGeoCdBios reconhece pela estrutura (nas duas ordens de bytes) e recusa o resto; sem BIOS o
//     núcleo não é criado; CD que não é do Neo Geo CD é recusado;
//  2. quadros: cada CD de lista.txt (exemplos do ngdevkit convertidos para CD) roda com a BIOS de teste
//     (t7_neogeocd_bios.s, que faz a partida pelo CD usando o hardware) e é comparado ponto a ponto com os quadros
//     do NeoCD (ref/<nome>.ref, em RGB565, gravados pelo t7_neogeocd_ref.c);
//  3. som: o volume (RMS) de cada trecho comparado com o do NeoCD (exemplos de som e o CD-DA da faixa 2);
//  4. save state: salvar -> rodar -> carregar -> rodar dá a mesma tela e o mesmo som; salvar -> carregar ->
//     salvar dá os mesmos bytes; estados truncados/danificados são recusados sem travar;
//  5. RAM de backup (o save): exportar/importar; janela de upload e DMA (operações do NeoCD) no barramento;
//  6. robustez: BIOS e CDs aleatórios ou danificados não travam.
//  7. dois jogadores: o controle 2 chega às entradas do jogador 2.
// Uso: indigo-t7_neogeocd pasta            (todos os testes)
//      indigo-t7_neogeocd pasta rodar disco.cue quadros saida.ppm [teclas]   (depuração: grava o último quadro)
#include <algorithm>
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

#include "../src/cdrom/cdrom.h"
#include "../src/neogeocd/neogeocd.h"

using indigo::neogeocd::NeoGeoCd;
using indigo::neogeo::LSPC;

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
bool writeFile(const std::string& path, const std::vector<uint8_t>& d) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
  return static_cast<bool>(f);
}

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

std::unique_ptr<NeoGeoCd> openDisc(const std::string& path, const std::vector<uint8_t>* save = nullptr) {
  std::string err;
  auto d = indigo::cdrom::open(path, &err);
  if (!d) throw std::runtime_error("não abriu " + path + ": " + err);
  return std::make_unique<NeoGeoCd>(std::move(d), save);
}

void pullAll(NeoGeoCd& ng, std::vector<float>* out) {
  const size_t n = ng.audioAvailable();
  std::vector<float> l(n), r(n);
  ng.pullAudio(l.data(), r.data(), n);
  if (out)
    for (size_t i = 0; i < n; i++) { out->push_back(l[i]); out->push_back(r[i]); }
}

// cor do Índigo (a mesma conversão da LSPC) -> cores RGB565 que o NeoCD dá para a mesma cor do Neo Geo
// (normal e com sombra); a mesma cor de 8 bits pode vir de mais de uma cor do Neo Geo, então guarda todas
std::multimap<uint32_t, uint16_t> colorMap() {
  std::multimap<uint32_t, uint16_t> m;
  std::set<std::pair<uint32_t, uint16_t>> seen;
  auto add = [&](uint32_t a, uint16_t b) { if (seen.insert({a, b}).second) m.insert({a, b}); };
  for (uint32_t d = 0; d < 0x10000; d++) {
    auto comp = [&](unsigned hi4, unsigned lo1) {
      const unsigned raw = ((hi4 << 2) | (lo1 << 1) | (d >> 15)) ^ 1;
      return (raw * 259 + 33) >> 6;
    };
    const unsigned r = comp((d >> 8) & 15, (d >> 14) & 1), g = comp((d >> 4) & 15, (d >> 13) & 1),
                   b = comp(d & 15, (d >> 12) & 1);
    const uint32_t mine = 0xFF000000u | (b << 16) | (g << 8) | r;
    const uint32_t mineSh = 0xFF000000u | ((b >> 1) << 16) | ((g >> 1) << 8) | (r >> 1);
    uint16_t o = static_cast<uint16_t>(((d & 0x0F00) << 4) | ((d & 0x4000) >> 3) | ((d & 0x00F0) << 3) |
                                       ((d & 0x2000) >> 7) | ((d & 0x000F) << 1) | ((d & 0x1000) >> 12));
    if (!(d & 0x8000)) o |= 0x0020;
    add(mine, o);
    add(mineSh, static_cast<uint16_t>((o >> 1) & 0x7BEF));
  }
  add(0xFF000000u, 0);  // vídeo desligado
  // o NeoCD só converte uma cor da paleta quando ela é escrita: as que o programa nunca escreveu saem 0x0000 (a
  // cor $0000 do Neo Geo convertida seria 0x0020, o preto com o bit "escuro" desligado)
  add(0xFF040404u, 0);
  return m;
}

struct Ref {
  int w = 0, h = 0;
  std::vector<std::vector<uint16_t>> frames;
};
bool readRef(const std::string& path, Ref& r) {
  std::vector<uint8_t> d;
  if (!readFile(path, d) || d.size() < 4) return false;
  r.w = d[0] | (d[1] << 8);
  r.h = d[2] | (d[3] << 8);
  const size_t n = 4 + static_cast<size_t>(r.w) * r.h * 2;
  for (size_t o = 0; o + n <= d.size(); o += n) {
    std::vector<uint16_t> f(static_cast<size_t>(r.w) * r.h);
    for (size_t i = 0; i < f.size(); i++) f[i] = static_cast<uint16_t>(d[o + 4 + 2 * i] | (d[o + 5 + 2 * i] << 8));
    r.frames.push_back(std::move(f));
  }
  return !r.frames.empty();
}

// ------------------------------------------------------------------ 2. quadros contra o NeoCD
struct Item {
  std::string name, file;
  int frames = 0, step = 0;
  uint32_t keys = 0;
  int minEqual = 100;
};

int runCompare(const std::string& dir, const Item& it, const Ref* ref, std::vector<uint64_t>& hashes,
               std::vector<float>* audio = nullptr) {
  static const std::multimap<uint32_t, uint16_t> CM = colorMap();
  std::unique_ptr<NeoGeoCd> ng;
  try {
    ng = openDisc(dir + "/discos/" + it.file + ".cue");
  } catch (const std::exception& e) {
    check(false, it.name + ": " + e.what());
    return -1;
  }
  ng->setSampleRate(44100);
  int bad = 0, k = 0;
  for (int f = 0; f < it.frames; f++) {
    // a interrupção de quadro do NeoCD fica no começo do quadro seguinte (o quadro dele começa no apagamento
    // vertical) e lê as teclas desse quadro; aqui ela vem no fim do quadro: as teclas do quadro seguinte
    ng->setKeys(keysAt(f + 1, it.keys));
    ng->runFrame();
    pullAll(*ng, audio);
    if ((f + 1) % it.step) continue;
    const uint32_t* fb = ng->lspc.fb;
    hashes.push_back(fbHash(fb, LSPC::OUT_W * LSPC::OUT_H));
    if (ref && k < static_cast<int>(ref->frames.size())) {
      const std::vector<uint16_t>& rf = ref->frames[k];
      int diff = 0, first = -1;
      for (int i = 0; i < ref->w * ref->h; i++) {
        auto range = CM.equal_range(fb[i]);
        bool ok = false;
        for (auto x = range.first; x != range.second && !ok; ++x) ok = x->second == rf[i];
        if (!ok) {
          if (first < 0) first = i;
          diff++;
        }
      }
      if (diff) {
        bad++;
        if (bad <= 2)
          std::printf("  %s quadro %d: %d pontos diferentes (primeiro em %d,%d: %08X x %04X)\n", it.name.c_str(), f + 1,
                      diff, first % ref->w, first / ref->w, fb[first], rf[first]);
        if (std::getenv("NGDUMP")) writePpm("/tmp/ngcd_" + it.name + "_" + std::to_string(f + 1) + ".ppm", fb, ref->w, ref->h);
      }
    }
    k++;
  }
  return bad;
}

void testFrames(const std::string& dir) {
  std::ifstream lst(dir + "/lista.txt");
  std::string line;
  int n = 0;
  while (std::getline(lst, line)) {
    std::istringstream ss(line);
    Item it;
    if (!(ss >> it.name >> it.file >> it.frames >> it.step)) continue;
    ss >> it.keys >> it.minEqual;
    Ref ref;
    if (!readRef(dir + "/ref/" + it.name + ".ref", ref)) { check(false, "referência de " + it.name); continue; }
    std::vector<uint64_t> h;
    const int bad = runCompare(dir, it, &ref, h);
    const int total = static_cast<int>(ref.frames.size());
    std::printf("%-22s %d/%d quadros iguais ao NeoCD\n", it.name.c_str(), total - std::max(bad, 0), total);
    check(bad >= 0 && (total - bad) * 100 >= total * it.minEqual, it.name + ": quadros iguais ao NeoCD");
    n++;
  }
  check(n > 0, "lista.txt com CDs de teste");
}

// ------------------------------------------------------------------ 3. som (envelope de volume)
// O som de cada CD é medido em trechos de 0,25 s a partir do começo do som (a carga do CD leva tempos parecidos
// mas não iguais) e o RMS de cada trecho é comparado com o da régua:
//  - cdda: o CD-DA da faixa 2 (e o YM2610 mudo) contra o NeoCD;
//  - som-teste: SSG, FM, ADPCM-A, ADPCM-B e temporizador do YM2610 tocados pelo Z80 com as amostras carregadas
//    do CD na RAM de PCM, contra o Geolith rodando o mesmo programa em cartucho (ref do t7_neogeo). O NeoCD não
//    serve de régua aqui: o YM2610 dele (o fm.c antigo do MAME) toca o ADPCM-A ~60% mais alto e não toca o ADPCM-B
//    deste teste; SSG e FM saem iguais nos três (ver o relatório do t7_neogeocd.sh);
std::vector<double> windowsRms(const std::vector<float>& st, double rate, int count) {
  size_t start = 0;
  while (start * 2 + 1 < st.size() && std::fabs(st[start * 2]) + std::fabs(st[start * 2 + 1]) < 0.01) start++;
  const size_t W = static_cast<size_t>(rate * 0.25);
  std::vector<double> out;
  for (int k = 0; k < count; k++) {
    const size_t a = start + k * W, b = a + W;
    if (2 * b > st.size()) break;
    double e = 0;
    for (size_t i = a; i < b; i++) e += st[2 * i] * st[2 * i] + st[2 * i + 1] * st[2 * i + 1];
    out.push_back(std::sqrt(e / (2.0 * W)));
  }
  return out;
}

std::vector<float> readI16(const std::string& path) {
  std::vector<uint8_t> d;
  readFile(path, d);
  std::vector<float> out(d.size() / 2);
  for (size_t i = 0; i < out.size(); i++) out[i] = static_cast<int16_t>(d[2 * i] | (d[2 * i + 1] << 8)) / 32768.0f;
  return out;
}

void compareAudio(const std::string& name, const std::vector<float>& mine, const std::vector<float>& ref, double refRate,
                  const char* refName) {
  const std::vector<double> a = windowsRms(mine, 44100, 24), b = windowsRms(ref, refRate, 24);
  const size_t n = std::min(a.size(), b.size());
  int agree = 0, silent = 0;
  for (size_t k = 0; k < n; k++) {
    if (b[k] < 0.002) { silent++; if (a[k] < 0.01) agree++; }
    else if (a[k] > b[k] / 1.25 && a[k] < b[k] * 1.25) agree++;
    if (std::getenv("NGAUD")) std::printf("    %s trecho %zu: %.4f x %.4f\n", name.c_str(), k, a[k], b[k]);
  }
  std::printf("som %-10s %d/%zu trechos de 0,25 s com volume parecido ao do %s (%d mudos)\n", name.c_str(), agree, n,
              refName, silent);
  check(n >= 16 && agree * 10 >= static_cast<int>(n) * 8, name + ": volume do som parecido com o do " + refName);
}

void testAudio(const std::string& dir) {
  auto run = [&](const std::string& disc, int frames) {
    std::vector<float> mine;
    Item it;
    it.name = disc; it.file = disc; it.frames = frames; it.step = frames;
    std::vector<uint64_t> h;
    runCompare(dir, it, nullptr, h, &mine);
    if (const char* o = std::getenv("NGAUDOUT")) {  // depuração: grava o som do Índigo (float estéreo)
      std::ofstream f(std::string(o) + "_" + disc + ".f32", std::ios::binary);
      f.write(reinterpret_cast<const char*>(mine.data()), static_cast<std::streamsize>(mine.size() * sizeof(float)));
    }
    return mine;
  };
  const std::vector<float> cdda = run("cdda", 900), ref = readI16(dir + "/ref/cdda.aud");
  compareAudio("cdda", cdda, ref, 44100, "NeoCD");
  const std::vector<float> geo = readI16(dir + "/../neogeo/ref/som-teste.aud");
  if (geo.empty()) { check(false, "som-teste: falta a referência do Geolith (rode o t7_neogeo.sh)"); return; }
  compareAudio("som-teste", run("som-teste", 900), geo, 8000000.0 / 144.0, "Geolith (cartucho)");
}

// ------------------------------------------------------------------ 4. save state
void runFrames(NeoGeoCd& ng, int n, int start, uint32_t keys, std::vector<uint64_t>& hashes, std::vector<float>& audio) {
  for (int f = 0; f < n; f++) {
    ng.setKeys(keysAt(start + f, keys));
    ng.runFrame();
    hashes.push_back(fbHash(ng.lspc.fb, LSPC::OUT_W * LSPC::OUT_H));
    pullAll(ng, &audio);
  }
}

void testState(const std::string& dir, const std::string& file, uint32_t keys, int warm) {
  std::unique_ptr<NeoGeoCd> p;
  try { p = openDisc(dir + "/discos/" + file + ".cue"); } catch (const std::exception& e) { check(false, e.what()); return; }
  NeoGeoCd& ng = *p;
  ng.setSampleRate(48000);
  std::vector<uint64_t> h0, h1, h2;
  std::vector<float> a0, a1, a2;
  // estado salvo no meio da carga do CD (setores chegando) e depois, com o programa rodando
  for (int pass = 0; pass < 2; pass++) {
    runFrames(ng, pass == 0 ? 40 : warm, 0, keys, h0, a0);
    const std::vector<uint8_t> s = ng.saveState();
    h1.clear(); h2.clear(); a1.clear(); a2.clear();
    runFrames(ng, 150, 200, keys, h1, a1);
    ng.loadState(s);
    runFrames(ng, 150, 200, keys, h2, a2);
    const std::string when = pass == 0 ? " (durante a carga)" : " (rodando)";
    check(h1 == h2, file + ": save state - mesmos quadros depois de carregar" + when);
    check(a1 == a2, file + ": save state - mesmo som depois de carregar" + when);
    ng.loadState(s);
    check(ng.saveState() == s, file + ": salvar -> carregar -> salvar dá os mesmos bytes" + when);
  }
  const std::vector<uint8_t> s = ng.saveState();
  std::mt19937 rng(1234);
  int refused = 0, ran = 0;
  for (int t = 0; t < 60; t++) {
    std::vector<uint8_t> bad = s;
    if (t < 20) bad.resize(rng() % s.size());
    else if (t < 40) for (int k = 0; k < 1 + t; k++) bad[rng() % bad.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
    else for (int k = 0; k < 8; k++) bad[rng() % std::min<size_t>(bad.size(), 4096)] ^= static_cast<uint8_t>(1 + rng() % 255);
    try {
      ng.loadState(bad);
      for (int f = 0; f < 3; f++) { ng.runFrame(); pullAll(ng, nullptr); }
      ran++;
    } catch (const std::runtime_error&) {
      refused++;
      check(ng.saveState() == s, file + ": estado recusado deixa o jogo como estava");
    }
    ng.loadState(s);
  }
  check(refused + ran == 60, file + ": estados danificados recusados ou rodados sem travar");
  ng.reset();
  std::vector<uint64_t> h3;
  std::vector<float> a3;
  runFrames(ng, 60, 0, keys, h3, a3);
  const double expect = 60 / ng.fps() * 48000 * 2;
  check(std::fabs(static_cast<double>(a3.size()) - expect) < expect * 0.02, file + ": som na taxa certa depois do reset");
  std::printf("save state (%s): determinístico, %d estados danificados recusados, %d rodaram\n", file.c_str(), refused, ran);
}

// ------------------------------------------------------------------ dois jogadores
// o controle 2 chega a $340000 e aos bits 2-3 de $380000, sem mexer no jogador 1
void testPlayer2(const std::string& dir) {
  indigo::neogeocd::NeoGeoCdCore core(indigo::cdrom::open(dir + "/discos/helloworld.cue"), nullptr);
  NeoGeoCd& ng = core.ngcd();
  check(core.players() == 2, "2 jogadores: players() == 2");
  core.setKeys(indigo::KEY_UP | indigo::KEY_START);
  core.setKeys2(indigo::KEY_A | indigo::KEY_LEFT | indigo::KEY_SELECT);
  check(ng.read8(0x300000) == 0xFE && ng.read8(0x340000) == 0xDB, "2 jogadores: controles 1 e 2");
  check((ng.read8(0x380000) & 0x0F) == 0x06, "2 jogadores: start do 1 e select do 2");
  core.setKeys2(0);
  check(ng.read8(0x340000) == 0xFF && (ng.read8(0x380000) & 0x0F) == 0x0E, "2 jogadores: jogador 2 solto");
}

// ------------------------------------------------------------------ 5. RAM de backup, janela de upload e DMA
void testBus(const std::string& dir) {
  auto p = openDisc(dir + "/discos/helloworld.cue");
  NeoGeoCd& ng = *p;
  // RAM de backup: 8 KB nos bytes ímpares de $800000, espelhada; é o save
  check(!ng.saveDirty(), "backup: limpo ao abrir");
  ng.write8(0x800001, 0x12);
  ng.write16(0x800002, 0xAB34);
  ng.write8(0x800004, 0x77);  // byte par: não existe
  check(ng.read8(0x800001) == 0x12 && ng.read8(0x800003) == 0x34 && ng.read8(0x804003) == 0x34, "backup: bytes ímpares e espelho");
  check(ng.read16(0x800000) == 0xFF12 && ng.read8(0x800000) == 0xFF && ng.read8(0x800005) == 0x00, "backup: leitura de palavra");
  check(ng.saveDirty(), "backup: escrita marca o save");
  std::vector<uint8_t> sv = ng.exportSave();
  check(sv.size() == 0x2000 && sv[0] == 0x12 && sv[1] == 0x34 && !ng.saveDirty(), "backup: exportado (8 KB)");
  sv[0x1FFF] = 0x99;
  auto p2 = openDisc(dir + "/discos/helloworld.cue", &sv);
  check(p2->read8(0x803FFF) == 0x99 && p2->exportSave() == sv, "backup: importado volta igual");
  for (int f = 0; f < 3; f++) p2->runFrame();
  p2->reset();
  check(p2->read8(0x803FFF) == 0x99, "backup: reset não apaga");

  // janela de upload ($E00000): só com a área escolhida em $FF0105 e o barramento pedido
  ng.write8(0xFF0105, 0);  // sprites
  ng.write16(0xE00010, 0x1234);
  check(ng.read16(0xE00010) == 0xFFFF, "janela: sem pedir o barramento não escreve");
  ng.write8(0xFF0121, 1);
  ng.write8(0xFF01A1, 2);
  ng.write16(0xE00010, 0x1234);
  ng.write8(0xE00013, 0x56);
  check(ng.read16(0xE00010) == 0x1234 && ng.read8(0xE00013) == 0x56, "janela: sprites (banco 2)");
  ng.write8(0xFF01A1, 6);  // banco só tem 2 bits
  check(ng.read16(0xE00010) == 0x1234, "janela: banco de sprites com 2 bits");
  ng.write8(0xFF0141, 1);
  ng.write8(0xFF0105, 5);  // camada fixa: bytes ímpares
  ng.write8(0xFF0129, 1);
  ng.write16(0xE00020, 0xAA55);
  ng.write8(0xE00023, 0x66);
  ng.write8(0xE00024, 0x77);  // byte par não grava
  check(ng.read8(0xE00021) == 0x55 && ng.read16(0xE00022) == 0xFF66 && ng.read8(0xE00025) == 0x00, "janela: camada fixa");
  ng.write8(0xFF0149, 1);
  ng.write8(0xFF0105, 1);  // PCM: bancos de 512 KB
  ng.write8(0xFF0123, 1);
  ng.write8(0xFF01A3, 1);
  ng.write8(0xE00001, 0x42);
  ng.write8(0xFF01A3, 0);
  check(ng.read8(0xE00001) != 0x42 || ng.read8(0xE00001) == 0x00, "janela: PCM banco 0");
  ng.write8(0xFF01A3, 1);
  check(ng.read8(0xE00001) == 0x42, "janela: PCM banco 1");
  ng.write8(0xFF0143, 1);
  ng.write8(0xFF0105, 4);  // Z80
  ng.write8(0xFF0127, 1);
  ng.write8(0xE1FFFF, 0x3C);
  check(ng.read(0xFFFF) == 0x3C, "janela: RAM do Z80 (64 KB)");
  ng.write8(0xFF0147, 1);

  // DMA: padrão ($FFCD) e cópia ($FE6D, origem em $FF0064) na RAM de programa
  auto dma = [&](uint16_t cfg, uint32_t a, uint32_t b, uint32_t len, uint16_t pat) {
    ng.write16(0xFF0064, static_cast<uint16_t>(a >> 16)); ng.write16(0xFF0066, static_cast<uint16_t>(a));
    ng.write16(0xFF0068, static_cast<uint16_t>(b >> 16)); ng.write16(0xFF006A, static_cast<uint16_t>(b));
    ng.write16(0xFF0070, static_cast<uint16_t>(len >> 16)); ng.write16(0xFF0072, static_cast<uint16_t>(len));
    ng.write16(0xFF006C, pat);
    ng.write16(0xFF007E, cfg);
    ng.write8(0xFF0061, 0x40);
    ng.write8(0xFF0061, 0x00);
  };
  dma(0xFFCD, 0x120000, 0, 8, 0xBEEF);
  bool ok = true;
  for (uint32_t a = 0x120000; a < 0x120010; a += 2) ok &= ng.read16(a) == 0xBEEF;
  check(ok && ng.read16(0x120010) == 0, "DMA: preenche com o padrão");
  dma(0xFE6D, 0x120000, 0x130000, 4, 0);
  check(ng.read16(0x130006) == 0xBEEF && ng.read16(0x130008) == 0, "DMA: cópia");
  dma(0xFEF5, 0x140000, 0, 2, 0);
  check(ng.read16(0x140000) == 0x0014 && ng.read16(0x140002) == 0x0000 && ng.read16(0x140004) == 0x0014 &&
            ng.read16(0x140006) == 0x0004,
        "DMA: preenche com o endereço");
  // DMA escreve nos vetores da RAM mesmo com os vetores da BIOS no lugar (como no NeoCD)
  dma(0xFFCD, 0x000040, 0, 1, 0x1357);
  check(ng.read16(0x000040) != 0x1357, "vetores da BIOS em $000000 até $3A0013");
  ng.write8(0x3A0013, 1);
  check(ng.read16(0x000040) == 0x1357, "DMA grava a RAM debaixo dos vetores");
  ng.write8(0x3A0003, 1);
  // registradores do CD
  ng.write16(0xFF0004, 0x0331);
  check(ng.read16(0xFF0004) == 0x0331, "$FF0004 lido de volta");
  check((ng.read16(0xFF011C) & 0x1000) != 0, "$FF011C: tampa fechada (não é CDZ)");
}

// arquivo do diretório raiz do ISO 9660 (para conferir o que a BIOS carregou)
bool isoFile(indigo::cdrom::Disc& d, const std::string& name, std::vector<uint8_t>& out) {
  uint8_t s[2048];
  if (!d.readData(16, s)) return false;
  const uint32_t root = s[158] | (s[159] << 8) | (s[160] << 16) | (static_cast<uint32_t>(s[161]) << 24);
  if (!d.readData(root, s)) return false;
  for (int i = 0; i < 2048 && s[i];) {
    const int len = s[i], nl = s[i + 32];
    const std::string n(reinterpret_cast<const char*>(&s[i + 33]), static_cast<size_t>(nl));
    if (n == name + ";1") {
      const uint32_t lba = s[i + 2] | (s[i + 3] << 8) | (s[i + 4] << 16), size = s[i + 10] | (s[i + 11] << 8) | (s[i + 12] << 16);
      out.clear();
      uint8_t b[2048];
      for (uint32_t k = 0; k * 2048 < size; k++) {
        if (!d.readData(lba + k, b)) return false;
        out.insert(out.end(), b, b + std::min<uint32_t>(2048, size - k * 2048));
      }
      return true;
    }
    i += len;
  }
  return false;
}

// a BIOS de teste carregou cada arquivo do IPL.TXT no lugar certo (DMA para a RAM e pela janela de upload)
void testUploads(const std::string& dir) {
  struct T { const char* disc; const char* name; };
  for (T t : {T{"sprite", "SPRITE"}, T{"som-teste", "SOMTESTE"}, T{"scrolling", "SCROLL"}}) {
    auto ng = openDisc(dir + "/discos/" + t.disc + ".cue");
    for (int f = 0; f < 320; f++) { ng->runFrame(); pullAll(*ng, nullptr); }
    auto d = indigo::cdrom::open(dir + "/discos/" + t.disc + ".cue");
    struct E { const char* ext; const std::vector<uint8_t>* mem; } es[] = {
        {".PRG", &ng->workRam()}, {".SPR", &ng->sprRam()}, {".FIX", &ng->fixRam()}, {".PCM", &ng->pcmRam()}, {".Z80", &ng->z80Ram()}};
    int n = 0;
    for (const E& e : es) {
      std::vector<uint8_t> f;
      if (!isoFile(*d, std::string(t.name) + e.ext, f)) continue;
      n++;
      size_t from = std::string(e.ext) == ".Z80" ? 0x100 : 0;  // o Z80 já rodou (o começo do nullsound tem variáveis)
      if (std::string(e.ext) == ".Z80" && f.size() > 0xF000) f.resize(0xF000);
      const bool same = f.size() <= e.mem->size() && std::equal(f.begin() + static_cast<long>(from), f.end(), e.mem->begin() + static_cast<long>(from));
      check(same, std::string(t.disc) + ": " + t.name + e.ext + " carregado igual ao arquivo");
    }
    check(n >= 3, std::string(t.disc) + ": arquivos conferidos");
    std::printf("carga do CD (%s): %d arquivos iguais na memória\n", t.disc, n);
  }
}

// ------------------------------------------------------------------ 6. robustez
void testRobust(const std::string& dir, const std::vector<uint8_t>& bios) {
  std::mt19937 rng(77);
  int ran = 0, refused = 0;
  std::vector<uint8_t> disc;
  readFile(dir + "/discos/helloworld.bin", disc);
  const std::string tmp = dir + "/tmp";
  // BIOS aleatórias com a estrutura de uma BIOS (pilha e vetores na BIOS): código aleatório rodando no CD
  for (int t = 0; t < 6; t++) {
    std::vector<uint8_t> b(0x80000);
    for (auto& x : b) x = static_cast<uint8_t>(rng());
    const uint8_t hdr[8] = {0x00, 0x10, 0xF3, 0x00, 0x00, 0xC0, 0x00, static_cast<uint8_t>(0x80 + (rng() & 0x7E))};
    std::memcpy(b.data(), hdr, 8);
    for (size_t v : {0x54, 0x58, 0x64, 0x68, 0x6C}) {
      b[v] = 0x00; b[v + 1] = 0xC0; b[v + 2] = static_cast<uint8_t>(rng()); b[v + 3] = static_cast<uint8_t>(rng() & 0xFE);
    }
    indigo::setFirmware("neogeocd", b);
    try {
      auto ng = openDisc(dir + "/discos/helloworld.cue");
      for (int f = 0; f < 60; f++) { ng->setKeys(rng()); ng->runFrame(); pullAll(*ng, nullptr); }
      const auto s = ng->saveState();
      ng->loadState(s);
      check(ng->saveState() == s, "robustez: estado com BIOS aleatória volta igual");
      ran++;
    } catch (const std::runtime_error&) {
      refused++;
    }
  }
  indigo::setFirmware("neogeocd", bios);
  // CDs danificados (setores com bytes trocados, inclusive o diretório e o IPL.TXT) e truncados
  for (int t = 0; t < 10; t++) {
    std::vector<uint8_t> bad = disc;
    if (t < 6) {
      for (int k = 0; k < 40; k++) bad[(16 * 2352 + rng() % (bad.size() - 16 * 2352))] ^= static_cast<uint8_t>(1 + rng() % 255);
    } else {
      bad.resize(rng() % bad.size());
    }
    writeFile(tmp + ".bin", bad);
    std::ofstream(tmp + ".cue") << "FILE \"tmp.bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n";
    try {
      auto ng = openDisc(tmp + ".cue");
      for (int f = 0; f < 240; f++) { ng->setKeys(rng()); ng->runFrame(); pullAll(*ng, nullptr); }
      ran++;
    } catch (const std::runtime_error&) {
      refused++;
    }
  }
  std::remove((tmp + ".bin").c_str());
  std::remove((tmp + ".cue").c_str());
  check(ran + refused == 16, "robustez: BIOS e CDs aleatórios/danificados não travam");
  std::printf("robustez: %d rodaram, %d recusados com mensagem\n", ran, refused);
}

// ------------------------------------------------------------------ 1. BIOS e detecção
void testBios(const std::string& dir, const std::vector<uint8_t>& bios) {
  int fam = -1;
  check(indigo::neogeocd::isNeoGeoCdBios(bios, &fam) && fam == 0, "BIOS de teste reconhecida (estrutura, família desconhecida)");
  std::vector<uint8_t> sw = bios;
  for (size_t i = 0; i + 1 < sw.size(); i += 2) std::swap(sw[i], sw[i + 1]);
  check(indigo::neogeocd::isNeoGeoCdBios(sw), "BIOS com as palavras invertidas reconhecida");
  std::vector<uint8_t> fl = bios;
  const uint8_t l3[4] = {0x00, 0xC0, 0xC8, 0x5E};
  std::memcpy(&fl[0x6C], l3, 4);
  check(indigo::neogeocd::isNeoGeoCdBios(fl, &fam) && fam == 1, "front-loader: reconhecida pelo vetor de nível 3");
  fl[0x6D] = 0xC0; fl[0x6E] = 0xC2; fl[0x6F] = 0x22;
  check(indigo::neogeocd::isNeoGeoCdBios(fl, &fam) && fam == 2, "top-loader");
  fl[0x6E] = 0xA3; fl[0x6F] = 0xE8;
  check(indigo::neogeocd::isNeoGeoCdBios(fl, &fam) && fam == 3, "CDZ");
  std::vector<uint8_t> small(bios.begin(), bios.begin() + 0x20000), big = bios, rnd(0x80000);
  big.resize(0x100000, 0xFF);
  std::mt19937 rng(5);
  for (auto& x : rnd) x = static_cast<uint8_t>(rng());
  check(!indigo::neogeocd::isNeoGeoCdBios(small) && !indigo::neogeocd::isNeoGeoCdBios(big) &&
            !indigo::neogeocd::isNeoGeoCdBios(rnd) && !indigo::neogeocd::isNeoGeoCdBios({}),
        "tamanho errado ou conteúdo aleatório: não é BIOS do Neo Geo CD");
  std::vector<uint8_t> cart;
  if (readFile(dir + "/../neogeo/ngdk/share/ngdevkit/neogeo.zip", cart))
    check(!indigo::neogeocd::isNeoGeoCdBios(cart), "neogeo.zip do cartucho não é BIOS do CD");
  std::vector<uint8_t> noReset = bios;
  noReset[4] = 0x00; noReset[5] = 0x00;
  check(!indigo::neogeocd::isNeoGeoCdBios(noReset), "reset fora da BIOS: recusada");

  // sem BIOS o núcleo não é criado
  indigo::setFirmware("neogeocd", {});
  std::string msg;
  try { openDisc(dir + "/discos/helloworld.cue"); } catch (const std::runtime_error& e) { msg = e.what(); }
  check(msg.find("BIOS do Neo Geo CD") != std::string::npos, "sem BIOS: o núcleo não é criado (" + msg + ")");
  indigo::setFirmware("neogeocd", rnd);
  msg.clear();
  try { openDisc(dir + "/discos/helloworld.cue"); } catch (const std::runtime_error& e) { msg = e.what(); }
  check(!msg.empty(), "BIOS inválida: recusada");
  indigo::setFirmware("neogeocd", sw);
  try {
    auto ng = openDisc(dir + "/discos/helloworld.cue");
    for (int f = 0; f < 150; f++) ng->runFrame();
    indigo::setFirmware("neogeocd", bios);
    auto ng2 = openDisc(dir + "/discos/helloworld.cue");
    for (int f = 0; f < 150; f++) ng2->runFrame();
    check(fbHash(ng->lspc.fb, LSPC::OUT_W * LSPC::OUT_H) == fbHash(ng2->lspc.fb, LSPC::OUT_W * LSPC::OUT_H),
          "BIOS invertida roda igual");
  } catch (const std::runtime_error& e) {
    check(false, std::string("BIOS invertida: ") + e.what());
  }
  // remendo do reconhecimento do disco (como no NeoCD): só quando os bytes originais estão lá
  {
    std::vector<uint8_t> f = bios;
    const uint8_t l3[4] = {0x00, 0xC0, 0xC8, 0x5E};
    std::memcpy(&f[0x6C], l3, 4);
    f[0x10B64] = 0x66; f[0x10B65] = 0x04;
    indigo::setFirmware("neogeocd", f);
    auto ng = openDisc(dir + "/discos/helloworld.cue");
    check(ng->read16(0xC10B64) == 0x4E71 && ng->biosName == "Neo Geo CD front-loader", "front-loader: remendo do reconhecimento do CD");
    f[0x10B65] = 0x05;
    indigo::setFirmware("neogeocd", f);
    auto ng2 = openDisc(dir + "/discos/helloworld.cue");
    check(ng2->read16(0xC10B64) == 0x6605, "front-loader diferente: sem remendo");
    std::vector<uint8_t> z = bios;
    const uint8_t cdz[4] = {0x00, 0xC0, 0xA3, 0xE8};
    std::memcpy(&z[0x6C], cdz, 4);
    z[0xEB82] = 0x66; z[0xEB83] = 0x10; z[0xD280] = 0x66; z[0xD281] = 0x74;
    indigo::setFirmware("neogeocd", z);
    auto ng3 = openDisc(dir + "/discos/helloworld.cue");
    check(ng3->read16(0xC0EB82) == 0x4E71 && ng3->read16(0xC0D280) == 0x4E71 && (ng3->read16(0xFF011C) & 0x1000) == 0,
          "CDZ: remendos e a tampa do CDZ");
  }
  indigo::setFirmware("neogeocd", bios);
  // um CD que não é do Neo Geo CD
  const std::string tmp = dir + "/tmp";
  std::vector<uint8_t> blank(40 * 2352, 0);
  writeFile(tmp + ".bin", blank);
  std::ofstream(tmp + ".cue") << "FILE \"tmp.bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n";
  msg.clear();
  try { openDisc(tmp + ".cue"); } catch (const std::runtime_error& e) { msg = e.what(); }
  check(msg.find("Neo Geo CD") != std::string::npos, "CD sem IPL.TXT recusado");
  std::remove((tmp + ".bin").c_str());
  std::remove((tmp + ".cue").c_str());
  // núcleo pela interface Core
  indigo::neogeocd::NeoGeoCdCore core(indigo::cdrom::open(dir + "/discos/helloworld.cue"), nullptr);
  check(core.system() == "neogeocd" && core.width() == 304 && core.height() == 224 && core.hasSave(), "interface Core");
  check(std::fabs(core.fps() - 59.599) < 0.01, "quadros por segundo (59,6)");
}

int runMode(int argc, char** argv) {
  if (argc < 6) return 2;
  auto p = openDisc(argv[3]);
  NeoGeoCd& ng = *p;
  std::printf("%s | BIOS %s\n", ng.title.c_str(), ng.biosName.c_str());
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
    static uint64_t last = 0;
    const uint64_t hh = fbHash(ng.lspc.fb, LSPC::OUT_W * LSPC::OUT_H);
    if (std::getenv("NGMUDA") && hh != last) std::printf("quadro %d: tela mudou\n", f + 1);
    last = hh;
    if ((f + 1) % 30 == 0) {
      std::printf("quadro %d: som RMS %.4f, PC %06X, Z80 PC %04X\n", f + 1, std::sqrt(e / std::max<size_t>(1, 2 * total)),
                  ng.cpu.pc, ng.z80.pc);
      e = 0;
      total = 0;
    }
  }
  std::printf("Z80 RAM:");
  for (int a = 0; a < 16; a++) std::printf(" %02X", ng.read(a));
  std::printf("\nRAM $000000:");
  for (uint32_t a = 0; a < 16; a++) std::printf(" %02X", ng.workRam()[a]);
  std::printf("\n");
  writePpm(argv[5], ng.lspc.fb, LSPC::OUT_W, LSPC::OUT_H);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/neogeocd";
  std::vector<uint8_t> bios;
  if (!readFile(dir + "/bios/neocd.bin", bios)) {
    std::printf("Neo Geo CD: pasta de testes %s não encontrada; pulando (rode tools/t7_neogeocd.sh)\n", dir.c_str());
    return 0;
  }
  indigo::setFirmware("neogeocd", bios);
  try {
    if (argc > 2 && std::string(argv[2]) == "rodar") return runMode(argc, argv);
    testBios(dir, bios);
    testBus(dir);
    testPlayer2(dir);
    testUploads(dir);
    testFrames(dir);
    testAudio(dir);
    testState(dir, "helloworld", 0, 200);
    testState(dir, "som-teste", 0, 300);
    testState(dir, "sprite", 80, 300);
    testState(dir, "cdda", 0, 300);  // com o CD-DA tocando
    testRobust(dir, bios);
  } catch (const std::exception& e) {
    check(false, std::string("exceção: ") + e.what());
  }
  std::printf("RESULTADO neogeocd: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
