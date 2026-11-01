// Testes do Sega CD (pasta de testes como argumento, normalmente /root/indigo-env/roms/segacd, preparada pelo
// t7_segacd.sh): sem a BIOS de verdade, a régua é uma BIOS de teste própria rodando um disco sintético.
//  1. reconhecimento da BIOS (isSegaCdBios) e a recusa sem BIOS;
//  2. quadros comparados ponto a ponto com o Genesis Plus GX rodando a mesma BIOS e o mesmo disco;
//  3. os valores que a BIOS de teste deixa na RAM principal (setores lidos por host/DMA, Word RAM 2M/1M,
//     ASIC, temporizador, PCM, CD-DA, comunicação), conferidos contra o esperado;
//  4. som (PCM e CD-DA) comparado com o do GPGX;
//  5. testes de unidade do ASIC de gráficos e do PCM contra o código do próprio GPGX (ref/unidade.txt);
//  6. save state (salvar -> carregar -> salvar igual; carregar e seguir dá o mesmo resultado) e velocidade.
// Uso: indigo-t7_segacd pasta [parte]   (partes: bios quadros valores som unidade estado)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/segacd/segacd.h"

using namespace indigo;

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

std::string g_dir;
std::vector<uint8_t> g_bios;

std::unique_ptr<segacd::SegaCdCore> makeCore(const std::vector<uint8_t>* save = nullptr) {
  std::string err;
  auto disc = cdrom::open(g_dir + "/teste.cue", &err);
  if (!disc) { std::printf("disco: %s\n", err.c_str()); return nullptr; }
  return std::make_unique<segacd::SegaCdCore>(std::move(disc), g_bios, save);
}

// tabela de resultados da BIOS de teste na RAM principal (0xFF8000): 32 palavras por teste
uint16_t tab(segacd::SegaCdCore& c, int t, int i) {
  const auto& ram = c.md().workRam();
  const size_t o = 0x8000 + (static_cast<size_t>(t) * 32 + i) * 2;
  return static_cast<uint16_t>((ram[o] << 8) | ram[o + 1]);
}

int quant(uint32_t c, int sh) { return ((static_cast<int>((c >> sh) & 255)) * 14 + 127) / 255; }

// ------------------------------------------------------------ 1. BIOS
void biosTests() {
  check(segacd::isSegaCdBios(g_bios), "a BIOS de teste é reconhecida como BIOS do Sega CD");
  std::vector<uint8_t> b = g_bios;
  b.resize(0x10000);
  check(!segacd::isSegaCdBios(b), "64 KB não é BIOS do Sega CD");
  b = g_bios;
  b[0x100] = 'X';
  check(!segacd::isSegaCdBios(b), "sem \"SEGA\" não é BIOS");
  b = g_bios;
  b[0x180] = 'G'; b[0x181] = 'M';
  for (int i = 0x120; i < 0x180; i++) b[i] = ' ';
  check(!segacd::isSegaCdBios(b), "jogo de 128 KB (GM, sem BOOT) não é BIOS");
  check(createCore(g_bios, "bios_CD_U.bin", nullptr) == nullptr, "a BIOS aberta como ROM não vira jogo de Mega Drive");
  // sem BIOS instalada: o núcleo não é criado e a mensagem pede o arquivo
  setFirmware("segacd", {});
  bool threw = false;
  std::string msg;
  try {
    auto d = cdrom::open(g_dir + "/teste.cue");
    segacd::createSegaCdCore(std::move(d), nullptr);
  } catch (const std::exception& e) { threw = true; msg = e.what(); }
  check(threw && msg.find("BIOS") != std::string::npos, "sem BIOS: núcleo recusado com mensagem pedindo a BIOS");
  setFirmware("segacd", g_bios);
  auto d = cdrom::open(g_dir + "/teste.cue");
  auto c = segacd::createSegaCdCore(std::move(d), nullptr);
  check(c && c->system() == "segacd", "com a BIOS instalada (setFirmware) o núcleo é criado, system() = segacd");
  check(segacd::segaCdBiosRegion(g_bios) == "us" && segacd::segaCdFirmwareName(g_bios) == "segacd_us", "região da BIOS (EUA)");
  {
    auto dd = cdrom::open(g_dir + "/teste.cue");
    check(dd && segacd::segaCdDiscRegion(*dd) == "us", "região do disco (EUA)");
  }
  // BIOS guardada pela região: o disco americano usa a segacd_us mesmo com outra em "segacd"
  {
    std::vector<uint8_t> outra = g_bios;
    outra[0x1F0] = 'J';
    setFirmware("segacd", outra);
    setFirmware("segacd_us", g_bios);
    auto c2 = segacd::createSegaCdCore(cdrom::open(g_dir + "/teste.cue"), nullptr);
    auto* sc = dynamic_cast<segacd::SegaCdCore*>(c2.get());
    check(sc && sc->md().overseas && !sc->md().pal, "disco dos EUA escolhe a BIOS dos EUA");
    setFirmware("segacd", g_bios);
    setFirmware("segacd_us", {});
  }
  if (c) check(c->title() == "INDIGO DISCO DE TESTE", "título do disco: " + c->title());
}

// ------------------------------------------------------------ 2/3/4. quadros, valores, som
constexpr int FRAMES = 2100, STEP = 50;

struct Run {
  std::vector<std::vector<uint32_t>> frames;
  std::vector<int> w, h;
  std::vector<float> audio;  // canal esquerdo * 32767
  std::vector<float> audioR;
};

void runAll(segacd::SegaCdCore& c, Run& out, int frames) {
  c.setSampleRate(44100);
  std::vector<float> l(8192), r(8192);
  for (int f = 0; f < frames; f++) {
    c.runFrame();
    size_t n = c.audioAvailable();
    while (n) {
      const size_t k = std::min<size_t>(n, 8192);
      c.pullAudio(l.data(), r.data(), k);
      for (size_t i = 0; i < k; i++) { out.audio.push_back(l[i] * 32767.0f); out.audioR.push_back(r[i] * 32767.0f); }
      n -= k;
    }
    if ((f + 1) % STEP == 0) {
      out.frames.emplace_back(c.framebuffer(), c.framebuffer() + c.width() * c.height());
      out.w.push_back(c.width()); out.h.push_back(c.height());
    }
  }
}

void writePpm(const std::string& path, const uint32_t* px, int w, int h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) { const uint8_t c[3] = {uint8_t(px[i]), uint8_t(px[i] >> 8), uint8_t(px[i] >> 16)}; std::fwrite(c, 1, 3, f); }
  std::fclose(f);
}

void frameTests(Run& run) {
  std::vector<uint8_t> ref;
  if (!readFile(g_dir + "/ref/teste.ref", ref)) { check(false, "falta ref/teste.ref (rode o t7_segacd.sh)"); return; }
  size_t pos = 0;
  int iguais = 0, total = 0, primeiro = -1, iguaisFim = 0, totalFim = 0;
  long difPrimeiro = 0;
  for (size_t k = 0; k < run.frames.size() && pos + 4 <= ref.size(); k++) {
    const int w = ref[pos] | (ref[pos + 1] << 8), h = ref[pos + 2] | (ref[pos + 3] << 8);
    pos += 4;
    const uint8_t* rp = &ref[pos];
    pos += static_cast<size_t>(w) * h * 3;
    if (pos > ref.size()) break;
    long dif = 0;
    if (w != run.w[k] || h != run.h[k]) dif = -1;
    else
      for (int i = 0; i < w * h; i++) {
        const uint32_t c = run.frames[k][i];
        if (quant(c, 0) != rp[i * 3] || quant(c, 8) != rp[i * 3 + 1] || quant(c, 16) != rp[i * 3 + 2]) dif++;
      }
    const int quadro = static_cast<int>(k + 1) * STEP;
    if (std::getenv("VERB")) std::printf("    quadro %d: %ld\n", quadro, dif);
    total++;
    if (dif == 0) iguais++;
    else if (primeiro < 0) {
      primeiro = quadro; difPrimeiro = dif;
      if (std::getenv("PNG")) {
        writePpm(std::string(std::getenv("PNG")) + "/indigo.ppm", run.frames[k].data(), run.w[k], run.h[k]);
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        for (int i = 0; i < w * h; i++) px[i] = 0xFF000000u | (rp[i * 3] * 17u) | (rp[i * 3 + 1] * 17u << 8) | (rp[i * 3 + 2] * 17u << 16);
        writePpm(std::string(std::getenv("PNG")) + "/gpgx.ppm", px.data(), w, h);
      }
    }
    // da tela de resultados em diante (quadro 1550) tudo tem de ser igual
    if (quadro >= 1550) { totalFim++; if (dif == 0) iguaisFim++; }
  }
  char buf[200];
  std::snprintf(buf, sizeof buf, "quadros iguais ao Genesis Plus GX: %d de %d (primeira diferença no quadro %d: %ld pontos)",
                iguais, total, primeiro, difPrimeiro);
  std::printf("  %s\n", buf);
  std::snprintf(buf, sizeof buf, "telas de resultados (quadros 1550 a 2100) iguais ao GPGX: %d de %d", iguaisFim, totalFim);
  std::printf("  %s\n", buf);
  check(totalFim > 0 && iguaisFim == totalFim, buf);
}

void valueTests(segacd::SegaCdCore& c) {
  auto eq = [&](int t, int i, uint16_t want, const char* what) {
    const uint16_t v = tab(c, t, i);
    char buf[200];
    std::snprintf(buf, sizeof buf, "teste %d palavra %d (%s): %04X, esperado %04X", t, i, what, v, want);
    check(v == want, buf);
  };
  auto range = [&](int t, int i, int lo, int hi, const char* what) {
    const uint16_t v = tab(c, t, i);
    char buf[200];
    std::snprintf(buf, sizeof buf, "teste %d palavra %d (%s): %d, esperado %d..%d", t, i, what, v, lo, hi);
    check(v >= lo && v <= hi, buf);
  };
  // registradores iniciais vistos pelo principal
  eq(0, 0, 0x0002, "A12000 ao ligar: sub-CPU em reset e parado");
  eq(0, 1, 0x0001, "A12002 ao ligar: 2M, Word RAM do principal");
  eq(0, 2, 0xFFFF, "vetor 0x70 lido da BIOS");
  eq(0, 3, 0xFFFF, "vetor 0x72 = registrador do vetor de linha (FFFF)");
  eq(0, 4, 0x1234, "vetor 0x72 depois de escrever A12006");
  eq(0, 5, 0x1234, "A12006");
  eq(0, 8, 0x80, "A10001: aparelho americano (NTSC) com a unidade de CD (bit 5 = 0)");
  // CDD
  eq(1, 0, 0x0900, "estado do CDD: 9 (TOC lido)");
  eq(1, 5, 0x0104, "FF8036: sem áudio, HOCK ligado");
  // TOC
  eq(2, 0, 0x9300, "TOC: fim do disco (RS0-RS3: estado 9, código 3)");
  eq(2, 1, 0x1950, "TOC: fim do disco em 00:19:50 (RS4-RS7)");
  eq(2, 2, 0x9401, "TOC: primeira faixa 01 (RS0-RS3)");
  eq(2, 3, 0x0300, "TOC: última faixa 03 (RS4-RS7)");
  eq(2, 13, 350, "LBA do começo da faixa 2");
  // leituras de setores
  eq(3, 2, 0x0020, "cabeçalho do setor lido pelo host (quadro 20)");
  eq(3, 4, 0, "setor pelo host: palavras erradas");
  eq(3, 7, 0xFFFF, "DBC no fim da transferência");
  eq(4, 4, 0, "DMA PRG-RAM: chegou ao fim (EDT)");
  eq(4, 5, 0x8100, "DMA PRG-RAM: endereço final");
  eq(4, 7, 0, "DMA PRG-RAM: palavras erradas");
  eq(5, 5, 0x2100, "DMA Word RAM 2M: endereço final");
  eq(5, 7, 0, "DMA Word RAM 2M: palavras erradas");
  eq(5, 9, 0x02, "FF8003 com a Word RAM 2M do sub (DMNA)");
  eq(6, 5, 0x0200, "DMA PCM: endereço final");
  eq(6, 7, 0, "DMA PCM: bytes errados");
  eq(7, 5, 0x0900, "DMA Word RAM 1M: endereço final");
  eq(7, 7, 0, "DMA Word RAM 1M: palavras erradas");
  eq(8, 8, 0, "leitura do setor pelo principal (A12008): palavras erradas");
  eq(8, 10, 0x8200, "A12004 no fim da leitura do principal");
  // Word RAM
  eq(9, 1, 0, "Word RAM 2M: o sub vê o que o principal escreveu");
  eq(9, 3, 0x01, "FF8003 depois do RET");
  eq(9, 9, 0, "Word RAM 2M: o principal vê o que o sub escreveu");
  eq(9, 12, 0, "DMA do VDP a partir da Word RAM (uma palavra de atraso)");
  eq(10, 0, 0x05, "FF8003 em 1M com RET");
  eq(10, 1, 0xF123, "pontos: normal + underwrite");
  eq(24, 0, 0x04, "FF8003 depois da troca de bancos");
  {
    uint16_t s = 0;
    for (int i = 0; i < 256; i++) s = static_cast<uint16_t>(s + ((static_cast<uint16_t>(i * 0x0101 + 0x1234)) ^ i));
    eq(24, 1, s, "o sub vê o banco que o principal escreveu (troca pelo RET)");
  }
  eq(24, 9, tab(c, 10, 1), "o principal vê os pontos que o sub escreveu");
  // ASIC
  for (int t = 11; t <= 13; t++) {
    eq(t, 0, 0x0000 | (tab(c, t, 0) & 0xFF), "ASIC: fim da operação (FF8058 bit 15 = 0)");
    eq(t, 1, 0, "ASIC: linhas restantes = 0");
    eq(t, 2, 1, "ASIC: uma interrupção de nível 1");
    check((tab(c, t, 8) & 0x8000) != 0, "ASIC: ocupado logo depois de começar");
  }
  // temporizador e cronômetro
  eq(14, 0, 1, "temporizador: 42..45 interrupções em 10/75 s");
  range(14, 16, 43, 44, "temporizador: interrupções");
  eq(14, 1, 100, "FF8031");
  eq(14, 2, 0, "temporizador parado: nenhuma interrupção");
  eq(14, 3, 1, "cronômetro: 10/75 s");
  range(14, 17, 4335 - 4096, 4345 - 4096, "cronômetro em 10/75 s (4340, 12 bits)");
  // PCM
  eq(15, 0, 1, "PCM: canal 0 dando voltas no laço");
  eq(15, 1, 1, "PCM: canal 1 no laço 0x1000");
  // CD-DA
  eq(16, 0, 1, "CD-DA: estado tocando");
  eq(16, 1, 0, "CD-DA: FF8036 com áudio tocando");
  eq(16, 2, 1, "CD-DA: começou");
  eq(20, 0, 4, "CD-DA: pausa");
  // INT2 e diversos
  eq(17, 0, 5, "nível 2: 5 pedidos do principal");
  check((tab(c, 17, 8) & 0x8000) != 0, "A12000: IEN2 visível ao principal");
  eq(18, 4, 0x5A, "fonte: cores");
  eq(18, 13, 0x1111 ^ 0x2222 ^ 0x3333 ^ 0x4444 ^ 0x5555 ^ 0x6666 ^ 0x7777 ^ 0xFFFF, "eco das palavras de comunicação");
  eq(23, 1, 0x0000, "PRG-RAM protegida: escrita ignorada");
  eq(23, 2, 0x5678, "PRG-RAM fora da proteção");
  // RAM de backup: os bytes ímpares escritos pelo sub viram o save
  const auto save = c.exportSave();
  bool ok = save.size() == 0x2000;
  for (int i = 0; i < 16 && ok; i++) ok = save[i] == 0xB0 + i;
  check(ok, "RAM de backup interna (8 KB) no save, com os bytes escritos pelo sub-CPU");
  check(save.size() == 0x2000 && std::memcmp(&save[0x1FE0], "SEGA_CD_ROM", 11) == 0 && save[0x1FD1] == 0x7D,
        "RAM de backup formatada (rodapé SEGA_CD_ROM, 125 blocos)");
}

double envelopeCorr(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to, double* ratio) {
  const size_t W = 735;
  std::vector<double> ea, eb;
  for (size_t i = from; i + W <= to && i + W <= a.size() && i + W <= b.size(); i += W) {
    double sa = 0, sb = 0;
    for (size_t k = 0; k < W; k++) { sa += double(a[i + k]) * a[i + k]; sb += double(b[i + k]) * b[i + k]; }
    ea.push_back(std::sqrt(sa / W)); eb.push_back(std::sqrt(sb / W));
  }
  double best = -1;
  for (int sh = -6; sh <= 6; sh++) {
    double ma = 0, mb = 0; int n = 0;
    for (size_t i = 6; i + 6 < ea.size(); i++) { ma += ea[i]; mb += eb[i + sh]; n++; }
    if (!n) continue;
    ma /= n; mb /= n;
    double c = 0, va = 0, vb = 0;
    for (size_t i = 6; i + 6 < ea.size(); i++) { const double x = ea[i] - ma, y = eb[i + sh] - mb; c += x * y; va += x * x; vb += y * y; }
    if (va > 0 && vb > 0) best = std::max(best, c / std::sqrt(va * vb));
  }
  double ta = 0, tb = 0;
  for (size_t i = 0; i < ea.size(); i++) { ta += ea[i]; tb += eb[i]; }
  *ratio = ta > 0 ? tb / ta : 0;
  return best;
}

void audioTests(Run& run) {
  if (std::getenv("AUDOUT")) {
    FILE* f = std::fopen(std::getenv("AUDOUT"), "wb");
    for (size_t i = 0; i < run.audio.size() && f; i++) {
      const int16_t s2[2] = {static_cast<int16_t>(run.audio[i]), static_cast<int16_t>(run.audioR[i])};
      std::fwrite(s2, 2, 2, f);
    }
    if (f) std::fclose(f);
  }
  std::vector<uint8_t> ref;
  if (!readFile(g_dir + "/ref/teste.aud", ref)) { check(false, "falta ref/teste.aud"); return; }
  std::vector<float> gl, gr;
  for (size_t i = 0; i + 4 <= ref.size(); i += 4) {
    gl.push_back(static_cast<int16_t>(ref[i] | (ref[i + 1] << 8)));
    gr.push_back(static_cast<int16_t>(ref[i + 2] | (ref[i + 3] << 8)));
  }
  // trechos: PCM (quadros 900-1080) e CD-DA (1100-1500), em amostras de 44100 Hz (735,7 por quadro):
  // volume (envelope RMS) e forma de onda (correlação cruzada com deslocamento de até 2 quadros)
  struct Trecho { const char* nome; int q0, q1, meio; double minCorr; } trechos[] = {
      {"PCM", 905, 1075, 1000, 0.97}, {"CD-DA", 1120, 1490, 1300, 0.99}};
  for (const auto& t : trechos) {
    const size_t a = static_cast<size_t>(t.q0 * 735.7), b = static_cast<size_t>(t.q1 * 735.7);
    double rl, rr;
    envelopeCorr(gl, run.audio, a, b, &rl);
    envelopeCorr(gr, run.audioR, a, b, &rr);
    double best[2] = {-1, -1};
    int bestSh = 0;
    for (int ch = 0; ch < 2; ch++) {
      const std::vector<float>& g = ch ? gr : gl;
      const std::vector<float>& m = ch ? run.audioR : run.audio;
      const size_t at = static_cast<size_t>(t.meio * 735.7), n = 4096;
      for (int sh = -1500; sh <= 1500; sh++) {
        double c = 0, va = 0, vb = 0;
        for (size_t i = 0; i < n; i++) {
          const double x = g[at + i], y = m[static_cast<size_t>(static_cast<long>(at + i) + sh)];
          c += x * y; va += x * x; vb += y * y;
        }
        if (va > 0 && vb > 0 && c / std::sqrt(va * vb) > best[ch]) { best[ch] = c / std::sqrt(va * vb); if (!ch) bestSh = sh; }
      }
    }
    char buf[240];
    std::snprintf(buf, sizeof buf, "som do %s: forma de onda com correlação %.4f/%.4f (deslocamento %d amostras), volume %.3f/%.3f do GPGX",
                  t.nome, best[0], best[1], bestSh, rl, rr);
    std::printf("  %s\n", buf);
    check(best[0] >= t.minCorr && best[1] >= t.minCorr && rl > 0.95 && rl < 1.05 && rr > 0.95 && rr < 1.05, buf);
  }
  // fade do fader: no fim do CD-DA (quadro 1500) o volume cai a 0 nos dois
  {
    double sa = 0;
    for (size_t i = static_cast<size_t>(1530 * 735.7); i < static_cast<size_t>(1560 * 735.7); i++) sa += std::fabs(run.audio[i]);
    check(sa < 1000, "CD-DA silencioso depois do fader a 0 e da pausa");
  }
}

// ------------------------------------------------------------ 6. save state
uint64_t hashRun(segacd::SegaCdCore& c, int frames) {
  uint64_t h = 1469598103934665603ull;
  std::vector<float> l(8192), r(8192);
  for (int f = 0; f < frames; f++) {
    c.runFrame();
    size_t n = c.audioAvailable();
    while (n) {
      const size_t k = std::min<size_t>(n, 8192);
      c.pullAudio(l.data(), r.data(), k);
      for (size_t i = 0; i < k; i++) { int32_t v = static_cast<int32_t>(l[i] * 32767) ^ (static_cast<int32_t>(r[i] * 32767) << 16); h = (h ^ static_cast<uint32_t>(v)) * 1099511628211ull; }
      n -= k;
    }
    for (int i = 0; i < c.width() * c.height(); i++) h = (h ^ c.framebuffer()[i]) * 1099511628211ull;
  }
  return h;
}

void stateTests() {
  for (int at : {300, 950, 1200}) {
    auto c = makeCore();
    if (!c) return;
    c->setSampleRate(48000);
    hashRun(*c, at);
    const auto st = c->saveState();
    c->loadState(st);
    {
      const auto s2 = c->saveState();
      if (s2 != st) {
        size_t i = 0;
        while (i < s2.size() && i < st.size() && s2[i] == st[i]) i++;
        std::printf("    estado: %zu bytes, recarregado %zu, primeira diferença no byte %zu\n", st.size(), s2.size(), i);
      }
    }
    check(c->saveState() == st, "estado no quadro " + std::to_string(at) + ": salvar -> carregar -> salvar dá os mesmos bytes");
    const uint64_t h1 = hashRun(*c, 120);
    c->loadState(st);
    const uint64_t h2 = hashRun(*c, 120);
    check(h1 == h2, "estado no quadro " + std::to_string(at) + ": carregar e rodar 120 quadros dá o mesmo vídeo e som");
    // outro núcleo, do zero, carregando o estado
    auto d = makeCore();
    d->setSampleRate(48000);
    d->runFrame();
    {  // o som já produzido (fila de saída, fora do estado) é descartado antes de carregar
      std::vector<float> l(d->audioAvailable() + 1), r(l.size());
      d->pullAudio(l.data(), r.data(), d->audioAvailable());
    }
    d->loadState(st);
    const uint64_t h3 = hashRun(*d, 120);
    check(h1 == h3, "estado no quadro " + std::to_string(at) + ": carregado em outro núcleo continua igual");
    // estados danificados não derrubam
    bool refused = false;
    try { std::vector<uint8_t> bad(st.begin(), st.begin() + static_cast<long>(st.size() / 2)); c->loadState(bad); } catch (const std::exception&) { refused = true; }
    check(refused, "estado truncado recusado");
  }
}

void speedTest() {
  auto c = makeCore();
  if (!c) return;
  c->setSampleRate(48000);
  std::vector<float> l(8192), r(8192);
  const auto t0 = std::chrono::steady_clock::now();
  const int N = std::getenv("SCD_N") ? std::atoi(std::getenv("SCD_N")) : 1500;
  for (int f = 0; f < N; f++) {
    c->runFrame();
    size_t n = c->audioAvailable();
    while (n) { const size_t k = std::min<size_t>(n, 8192); c->pullAudio(l.data(), r.data(), k); n -= k; }
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  velocidade: %d quadros em %.2f s = %.0f quadros/s (%.1fx o tempo real)\n", N, s, N / s, N / s / 60.0);
}

// ------------------------------------------------------------ 5. unidade (ASIC e PCM contra o GPGX)
void unitTests();

}  // namespace

// definido em t7_segacd_unidade.inc (mesmo gerador aleatório do refunidade.c)
#include "t7_segacd_unidade.inc"

int main(int argc, char** argv) {
  g_dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/segacd";
  const std::string part = argc > 2 ? argv[2] : "";
  if (!readFile(g_dir + "/bios_teste.bin", g_bios)) {
    std::printf("falta %s/bios_teste.bin (rode o t7_segacd.sh)\nRESULTADO segacd: 0 verificações, 1 falhas\n", g_dir.c_str());
    return 1;
  }
  if (part.empty() || part == "bios") { std::printf("== BIOS\n"); biosTests(); }
  if (part.empty() || part == "quadros" || part == "valores" || part == "som") {
    std::printf("== BIOS de teste e disco sintético, comparados com o Genesis Plus GX\n");
    auto c = makeCore();
    if (c) {
      Run run;
      runAll(*c, run, FRAMES);
      if (part.empty() || part == "quadros") frameTests(run);
      if (part.empty() || part == "valores") valueTests(*c);
      if (part.empty() || part == "som") audioTests(run);
    } else check(false, "não abriu o disco de teste");
  }
  if (part.empty() || part == "unidade") { std::printf("== unidade: ASIC e PCM contra o código do GPGX\n"); unitTests(); }
  if (part.empty() || part == "estado") { std::printf("== save state\n"); stateTests(); }
  if (part.empty() || part == "estado" || part == "velocidade") speedTest();
  std::printf("RESULTADO segacd: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
