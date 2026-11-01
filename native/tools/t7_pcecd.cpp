// Testes do PC Engine CD (pasta de testes como argumento, normalmente /root/indigo-env/roms/pcecd, montada pelo
// t7_pcecd.sh). Sem o System Card de verdade (direitos autorais), a régua é:
//  1. System Cards de teste próprios (cards/card1-4.pce, t7_pcecd_card.asm) que acionam a interface do CD
//     pelos registradores: SCSI e memórias, CD-DA, ADPCM e Arcade Card. Os bytes de resultado (RAM $2200 e
//     $2300) são conferidos contra os valores calculados aqui a partir do disco sintético (disco/teste.cue).
//  2. Os mesmos programas no Mesen 2 (ref/*.mesen.txt, hashes de cada quadro; ref/*.mesen.raw, o som): telas
//     comparadas quadro a quadro e o som comparado pelo envelope e pela frequência.
//  3. Programas de teste do John Brandwood (HuC) que acionam o CD sem BIOS (scsitest, okitest), como System
//     Card, comparados com o Mesen 2.
//  4. Reconhecimento da BIOS, núcleo sem BIOS, save state (ida e volta igual, determinismo, estados
//     danificados), BRAM como save.
// Uso: indigo-t7_pcecd pasta
//      indigo-t7_pcecd hashes card.pce disco.cue tipo quadros [teclas]   (ferramenta: hashes de cada quadro)
//      indigo-t7_pcecd shot card.pce disco.cue tipo quadros saida.ppm [teclas]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/pcecd/pcecd.h"

using indigo::pcecd::Model;
using indigo::pcecd::PceCdCore;

namespace {

long g_checks = 0, g_fails = 0;

void check(bool ok, const std::string& what) {
  g_checks++;
  if (!ok) { g_fails++; if (g_fails < 300) std::printf("FALHA: %s\n", what.c_str()); }
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

std::string hex(const uint8_t* p, size_t n) {
  std::string s;
  char b[4];
  for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%02X ", p[i]); s += b; }
  return s;
}

// mesmo hash do t7_pcecd_mesen.cc: FNV-1a das cores de 9 bits das 240 linhas
uint32_t frameHash(indigo::Core& c) {
  const int w = c.width(), step = c.height() == 480 ? 2 : 1, h = c.height() / step;
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

unsigned keysAt(const std::string& keys, int frame) {
  unsigned k = 0;
  const char* p = keys.c_str();
  while (*p) {
    const int fr = std::atoi(p);
    const char* c = std::strchr(p, ':');
    if (!c) break;
    if (fr <= frame) k = static_cast<unsigned>(std::strtoul(c + 1, nullptr, 0));
    p = std::strchr(c, ',');
    if (!p) break;
    p++;
  }
  return k;
}

// System Card de teste de qualquer tamanho (os HuCards de teste têm 16 a 40 KB): espelhado até 256 KB
std::vector<uint8_t> asCard(std::vector<uint8_t> b) {
  if (b.empty()) return b;
  std::vector<uint8_t> o(0x40000);
  for (size_t i = 0; i < o.size(); i++) o[i] = b[i % b.size()];
  return o;
}

std::unique_ptr<PceCdCore> makeCore(const std::string& card, const std::string& cue, int type) {
  std::vector<uint8_t> bios;
  if (!readFile(card, bios)) return nullptr;
  bios = asCard(bios);
  std::string err;
  auto disc = indigo::cdrom::open(cue, &err);
  if (!disc) { std::printf("não abriu %s: %s\n", cue.c_str(), err.c_str()); return nullptr; }
  const Model m = Model(type);
  auto c = std::make_unique<PceCdCore>(std::move(disc), nullptr, &bios, &m);
  c->setSampleRate(48000);
  return c;
}

struct Run {
  std::vector<uint32_t> hashes;
  std::vector<float> l, r;
};

Run runCore(PceCdCore& c, int frames, const std::string& keys = "") {
  Run out;
  std::vector<float> L, R;
  for (int f = 0; f < frames; f++) {
    c.setKeys(keysAt(keys, f));
    c.runFrame();
    out.hashes.push_back(frameHash(c));
    const size_t n = c.audioAvailable();
    L.resize(n); R.resize(n);
    c.pullAudio(L.data(), R.data(), n);
    out.l.insert(out.l.end(), L.begin(), L.end());
    out.r.insert(out.r.end(), R.begin(), R.end());
  }
  return out;
}

void writePpm(PceCdCore& c, const std::string& path) {
  FILE* o = std::fopen(path.c_str(), "wb");
  if (!o) return;
  std::fprintf(o, "P6 %d %d 255\n", c.width(), c.height());
  const uint32_t* fb = c.framebuffer();
  for (int i = 0; i < c.width() * c.height(); i++) {
    const uint8_t px[3] = {uint8_t(fb[i]), uint8_t(fb[i] >> 8), uint8_t(fb[i] >> 16)};
    std::fwrite(px, 1, 3, o);
  }
  std::fclose(o);
}

std::vector<uint32_t> readRef(const std::string& path) {
  std::vector<uint32_t> v;
  std::ifstream f(path);
  int fr, w, h;
  std::string hs;
  while (f >> fr >> w >> h >> hs) v.push_back(uint32_t(std::strtoul(hs.c_str(), nullptr, 16)));
  return v;
}

// quadros iguais com o melhor deslocamento de -2 a 2 (o Mesen fecha o quadro em outro ponto); o último quadro
// tem de ser igual
int compareFrames(const std::vector<uint32_t>& mine, const std::vector<uint32_t>& ref, bool& lastOk, int& firstBad) {
  int best = -1;
  lastOk = false;
  firstBad = -1;
  for (int off = -2; off <= 2; off++) {
    int same = 0, bad = -1;
    bool last = false;
    for (int k = 0; k < int(mine.size()); k++) {
      const int r = k + off;
      if (r < 0 || r >= int(ref.size())) continue;
      const bool eq = mine[size_t(k)] == ref[size_t(r)];
      if (eq) same++;
      else if (bad < 0) bad = k;
      if (k == int(mine.size()) - 1 || r == int(ref.size()) - 1) last = eq;
    }
    if (same > best) { best = same; lastOk = last; firstBad = bad; }
  }
  return best;
}

// ------------------------------------------------------------ disco sintético (o mesmo do t7_pcecd_disco.py)
uint8_t pat(uint32_t n, uint32_t i) { return uint8_t((n * 31 + i + (i >> 8) * 7 + (n >> 8)) & 255); }

uint16_t sectorSum(uint32_t n) {
  uint32_t s = 0;
  for (uint32_t i = 0; i < 2048; i++) s += pat(n, i);
  return uint16_t(s);
}

std::vector<uint8_t> res(PceCdCore& c, uint16_t base) {
  std::vector<uint8_t> v(256);
  for (int i = 0; i < 256; i++) v[size_t(i)] = c.pce().cpu.peek(uint16_t(base + i));
  return v;
}

// confere uma linha de RES (8 bytes); -1 = não importa
void row(const std::vector<uint8_t>& r, int line, std::initializer_list<int> want, const std::string& what) {
  int k = 0;
  bool ok = true;
  for (int w : want) {
    if (w >= 0 && r[size_t(line * 8 + k)] != uint8_t(w)) ok = false;
    k++;
  }
  check(ok, what + ": " + hex(&r[size_t(line * 8)], 8));
}

// ------------------------------------------------------------ som
struct Sound { double rmsL, rmsR; std::vector<double> envL, envR; double freqL, freqR; };

// envelope (RMS a cada 800 amostras = 1 quadro a 48 kHz) e frequência pelos cruzamentos de zero
Sound analyze(const std::vector<float>& L, const std::vector<float>& R, size_t from, size_t to) {
  Sound s{};
  to = std::min(to, std::min(L.size(), R.size()));
  if (from >= to) return s;
  double el = 0, er = 0;
  size_t n = 0, zl = 0, zr = 0;
  double al = 0, ar = 0;
  for (size_t i = from; i < to; i++) {
    el += double(L[i]) * L[i]; er += double(R[i]) * R[i]; n++;
    if (i > from) {
      if ((L[i - 1] < 0) != (L[i] < 0)) zl++;
      if ((R[i - 1] < 0) != (R[i] < 0)) zr++;
    }
    al += double(L[i]) * L[i]; ar += double(R[i]) * R[i];
    if ((i - from) % 800 == 799) { s.envL.push_back(std::sqrt(al / 800)); s.envR.push_back(std::sqrt(ar / 800)); al = ar = 0; }
  }
  s.rmsL = std::sqrt(el / double(n)); s.rmsR = std::sqrt(er / double(n));
  const double secs = double(to - from) / 48000.0;
  s.freqL = zl / 2.0 / secs; s.freqR = zr / 2.0 / secs;
  return s;
}

std::vector<float> readRaw(const std::string& path, std::vector<float>& R) {
  std::vector<uint8_t> b;
  std::vector<float> L;
  R.clear();
  if (!readFile(path, b)) return L;
  for (size_t i = 0; i + 4 <= b.size(); i += 4) {
    L.push_back(float(int16_t(uint16_t(b[i] | b[i + 1] << 8))) / 32768.0f);
    R.push_back(float(int16_t(uint16_t(b[i + 2] | b[i + 3] << 8))) / 32768.0f);
  }
  return L;
}

// correlação dos envelopes normalizados (com o melhor atraso de até 6 quadros)
double envCorr(const std::vector<double>& a, const std::vector<double>& b) {
  double best = -2;
  for (int lag = -6; lag <= 6; lag++) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    int n = 0;
    for (int i = 0; i < int(a.size()); i++) {
      const int j = i + lag;
      if (j < 0 || j >= int(b.size())) continue;
      sa += a[size_t(i)]; sb += b[size_t(j)]; saa += a[size_t(i)] * a[size_t(i)]; sbb += b[size_t(j)] * b[size_t(j)];
      sab += a[size_t(i)] * b[size_t(j)]; n++;
    }
    if (n < 10) continue;
    const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    if (va <= 1e-12 || vb <= 1e-12) continue;
    best = std::max(best, (sab - sa * sb / n) / std::sqrt(va * vb));
  }
  return best;
}

// ------------------------------------------------------------ casos
struct Case { const char* name; const char* card; const char* cue; int type; int frames; const char* keys; };
const Case CASES[] = {
  {"t1-cd", "cards/card1.pce", "disco/teste.cue", 0, 400, ""},
  {"t1-super", "cards/card1.pce", "disco/teste.cue", 1, 400, ""},
  {"t1-arcade", "cards/card1.pce", "disco/teste.cue", 2, 400, ""},
  {"t2", "cards/card2.pce", "disco/teste.cue", 2, 700, ""},
  {"t3", "cards/card3.pce", "disco/teste.cue", 2, 700, ""},
  {"t4", "cards/card4.pce", "disco/teste.cue", 2, 200, ""},
  {"t4-super", "cards/card4.pce", "disco/teste.cue", 1, 200, ""},
  // John Brandwood: os 16 testes SCSI (RUN a cada 600 quadros) e os do ADPCM (até a tela das flags; depois o
  // Mesen 2 perde a escrita na RAM do ADPCM quando ela cai a menos de 3 relógios da janela de acesso)
  {"scsitest", "elmer/scsitest-card.pce", "elmer/scsitest.cue", 2, 9900, "600:8,606:0,1200:8,1206:0,1800:8,1806:0,2400:8,2406:0,3000:8,3006:0,3600:8,3606:0,4200:8,4206:0,4800:8,4806:0,5400:8,5406:0,6000:8,6006:0,6600:8,6606:0,7200:8,7206:0,7800:8,7806:0,8400:8,8406:0,9000:8,9006:0,9600:8,9606:0"},
  {"okitest", "elmer/okitest-card.pce", "elmer/okitest.cue", 2, 2100, "760:8,766:0,1160:8,1166:0,1560:8,1566:0,1960:8,1966:0"},
};

void expectT1(const std::vector<uint8_t>& r, const std::vector<uint8_t>& r2, int type, const std::string& n) {
  row(r, 0, {0x00, 0x00, 0x00, -1, 0x00, 0x00}, n + " registradores depois do reset");
  row(r, 1, {0, 0, 0, 0, 0}, n + " TEST UNIT READY");
  row(r, 2, {0x01, 0x04, 0, 0, 4, 0, 0, 0}, n + " TOC 0 (faixas 1 a 4)");
  row(r, 3, {0x00, 0x12, 0x50, 0, 4, 0, 0, 0}, n + " TOC 1 (fim em 00:12:50)");
  row(r, 4, {0x00, 0x02, 0x00, 4, 4, 0, 0, 0}, n + " TOC 2 faixa 1 (dados)");
  row(r, 5, {0x00, 0x06, 0x00, 0, 4, 0, 0, 0}, n + " TOC 2 faixa 2 (áudio)");
  row(r, 6, {0x00, 0x08, 0x00, 0, 4, 0, 0, 0}, n + " TOC 2 faixa 3");
  row(r, 7, {0x00, 0x12, 0x00, 4, 4, 0, 0, 0}, n + " TOC 2 faixa 4");
  row(r, 8, {0x00, 0x12, 0x50, 0, 4, 0, 0, 0}, n + " TOC 2 faixa 5 (fim do disco)");
  uint32_t s1 = 0;
  {
    std::vector<uint8_t> d(2048);
    for (uint32_t i = 0; i < 2048; i++) d[i] = pat(1, i);
    std::memcpy(d.data() + 32, "PC Engine CD-ROM SYSTEM", 23);
    for (uint8_t b : d) s1 += b;
  }
  row(r, 9, {0x00, 0x08, 0, 0, 0, int(s1 & 255), int((s1 >> 8) & 255)}, n + " READ(6) de 1 setor com ACK manual");
  row(r, 10, {'P', 'C', ' ', 'E', 'n', 'g', 'i', 'n'}, n + " texto do setor 1");
  uint32_t s4 = 0;
  for (uint32_t k = 20; k < 24; k++) s4 += sectorSum(k);
  row(r, 11, {0x00, 0x20, 0, 0, 0, int(s4 & 255), int((s4 >> 8) & 255)}, n + " READ(6) de 4 setores por $1808");
  row(r, 12, {0x01, 0x41, 0x01, 0x01, 0x00, 0x00, 0x24, 0x0A}, n + " SUBCODE Q depois da leitura");
  check(r2[0] == 0x00 && r2[1] == 0x02 && r2[2] == 0x24, n + " SUBCODE Q: tempo absoluto 00:02:24 (" + hex(r2.data(), 3) + ")");
  row(r, 13, {0, 0, 0, 0, 0}, n + " READ além do fim do disco");
  row(r, 14, {0, 0, 0, 0, 0}, n + " comando desconhecido");
  row(r, 15, {0x40, 0x01, 0x00, 0x60, 0x02, 0x00}, n + " IRQ de dados e de status");
  row(r, 16, {0x00, 0x18, 0x00, 0x18, 0x00, 0x18, 0x00, 0x18}, n + " seleção durante a leitura: status");
  row(r, 18, {0x00, 0x38, 0x00}, n + " seleção durante a leitura: mensagem");
  row(r, 19, {0x09, 0x00, 0, 0, 0, 0, 0}, n + " 9 bytes e depois TEST UNIT READY normal");
  if (type == 0) row(r, 20, {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF}, n + " sem assinatura do Super System Card");
  else row(r, 20, {0x00, 0xAA, 0x55, 0x03, 0x00, type == 2 ? 0x51 : 0xFF}, n + " assinaturas");
  if (type != 0) row(r, 21, {0x5A, 0xA5, 0x3C, 0xC3, 0xFF, 'H', 'U', 'M'}, n + " RAM do Super System Card, do CD e BRAM");
  else row(r, 21, {-1, -1, -1, 0xC3, 0xFF, 'H', 'U', 'M'}, n + " RAM do CD e BRAM");
  row(r, 22, {0x77, 0xFF, 0x1F, 0x00}, n + " BRAM gravada e travada, $1802");
  row(r, 23, {0xEE}, n + " fim do teste");
}

void expectT2(const std::vector<uint8_t>& r, const std::string& n) {
  row(r, 0, {0, 0, 0, 0, 0}, n + " AUDIO START pelo LBA");
  row(r, 1, {0x00, 0x78, 0x56, 0x02, 0x34, 0x12, 0x00}, n + " amostra direita/esquerda em $1805 e escrita ignorada");
  row(r, 2, {0x00, 0x01, 0x02, 0x01}, n + " SUBCODE Q tocando a faixa 2");
  row(r, 3, {0, 0, 0, 0, 0}, n + " AUDIO STOP com parada");
  row(r, 4, {0x03, 0x01, 0x02, 0x01, 0x02, 0x00, 0x00}, n + " parado depois do fim; amostra zero");
  row(r, 5, {0, 0, 0, 0, 0}, n + " AUDIO START pelo MSF");
  row(r, 6, {0, 0, 0, 0, 0}, n + " PAUSE");
  row(r, 7, {0x02, 0x01, 0x03, 0x01, 0x00, 0x00, 0x00}, n + " pausado na faixa 3; amostra zero");
  row(r, 8, {0, 0, 0, 0, 0}, n + " AUDIO START pela faixa");
  row(r, 9, {0, 0, 0, 0, 0, 0}, n + " AUDIO STOP com status no fim");
  row(r, 10, {0x03, 0x01, 0x02, 0x01}, n + " parado depois do status");
  row(r, 11, {0x1E, 0x10, 0x0C, 0x00}, n + " IRQ do subcódigo (~980 em 8 quadros) e fader");
  row(r, 12, {0xEE}, n + " fim do teste");
}

void expectT3(const std::vector<uint8_t>& r, const std::string& n, PceCdCore& c, const std::vector<uint8_t>& adpcmDisc) {
  row(r, 0, {0x00, 0x00, 0x00, 0x00, 0x00}, n + " ADPCM depois do reset");
  uint32_t s = 0;
  for (int i = 0; i < 256; i++) s += uint8_t(i * 7 + 3);
  // a primeira escrita com comprimento 0 liga o fim ($180C bit 0, IRQ $08) e a metade ($04)
  row(r, 1, {0x01, 0x0C, int(s & 255), int((s >> 8) & 255), 3, 10, uint8_t(255 * 7 + 3)}, n + " 256 bytes gravados e lidos");
  // comprimento $7FFE: metade ao gravar até $8000; comprimento 2: o fim só na leitura com comprimento 0
  row(r, 2, {0x00, 0x04, 0x04, 0x00, 0x00, 0x04, 0x04, 0x08}, n + " flags de metade e de fim");
  row(r, 3, {0x01, 0x08, 0x01}, n + " fim continua ligado");
  row(r, 4, {0, 0, 0, 0, 0, 0x02}, n + " DMA de 16 setores");
  bool same = adpcmDisc.size() == 32768;
  for (size_t i = 0; same && i < 32768; i++) same = c.cd().adpcmRam()[i] == adpcmDisc[i];
  check(same, n + " RAM do ADPCM igual aos 16 setores do disco (DMA)");
  if (adpcmDisc.size() >= 8)
    row(r, 5, {adpcmDisc[0], adpcmDisc[1], adpcmDisc[2], adpcmDisc[3], adpcmDisc[4], adpcmDisc[5], adpcmDisc[6], adpcmDisc[7]},
        n + " RAM do ADPCM lida de volta");
  // 8192 amostras a 16 kHz = 30,7 quadros: (quadros + 2) / 8 = 4; no fim: metade e fim, sem tocar
  row(r, 6, {0x00, 0x04, 0x0C, 0x01, 0x0C, 0x01, 0x00}, n + " reprodução de 4 KB a 16 kHz com parada automática");
  row(r, 7, {0xEE}, n + " fim do teste");
}

void expectT4(const std::vector<uint8_t>& r, int type, const std::string& n) {
  if (type != 2) {  // sem Arcade Card: tudo $FF
    row(r, 0, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, n + " sem Arcade Card");
    return;
  }
  row(r, 0, {0x10, 0x51, 0x00, 0x00, 0xFF}, n + " assinatura do Arcade Card");
  row(r, 1, {0, 0, 0, 0, 0, 0, 0, 0}, n + " registradores iniciais");
  // 64 escritas com o incremento somado à base: base $000140
  row(r, 3, {0x40, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11}, n + " base incrementada a cada escrita");
  uint32_t s = 0;
  for (int i = 0; i < 64; i++) s += uint8_t(i * 3 + 1);
  row(r, 5, {int(s & 255), int((s >> 8) & 255), 0x40, 0x01, 0x13, 0x00}, n + " leitura pela janela $40 e deslocamento incrementado");
  // $100010-$100012 pela porta 2; deslocamento com sinal: $100020 + 5 + $FF0000 = $0F0025
  row(r, 6, {0xA1, 0xA2, 0xA3, 0x99, 0x99}, n + " deslocamento somado e com sinal");
  // gatilhos: byte baixo ($40), byte alto ($0140 -> base $0180), $0A ($02C0), com sinal ($FF0400)
  row(r, 7, {0x40, 0x00, 0x80, 0x01, 0x80, 0xC0, 0x02, 0xFF}, n + " gatilhos de soma do deslocamento");
  row(r, 8, {0x20, 0x88, 0x77, 0x10, 0x90}, n + " volta dos 2 MB e incremento de 16 bits");
  row(r, 9, {0xC0, 0xB3, 0xA2, 0x91, 0x3C, 0x2B, 0x1A, 0x09}, n + " deslocamento 3 à esquerda e 1 à direita");
  row(r, 10, {0xAC, 0x68, 0x24, 0x00, 0x81, 0x67, 0x45, 0x23}, n + " 7 à direita e rotação de 4 à esquerda");
  row(r, 11, {0xCF, 0x8A, 0x46, 0x02, 0x09, 0x0D}, n + " rotação de 3 à direita");
  row(r, 12, {0x01, 0x01, 0xEE}, n + " janela do banco $41");
}

// ------------------------------------------------------------ partes
void testCases(const std::string& dir) {
  std::vector<uint8_t> adpcmDisc;
  {
    std::string e;
    auto d = indigo::cdrom::open(dir + "/disco/teste.cue", &e);
    if (d) {
      uint8_t b[2048];
      for (uint32_t k = 100; k < 116; k++) { d->readData(k, b); adpcmDisc.insert(adpcmDisc.end(), b, b + 2048); }
    }
  }
  for (const Case& cs : CASES) {
    auto c = makeCore(dir + "/" + cs.card, dir + "/" + cs.cue, cs.type);
    if (!c) { check(false, std::string(cs.name) + ": não montou o caso (rode o t7_pcecd.sh)"); continue; }
    const Run run = runCore(*c, cs.frames, cs.keys);
    const auto r = res(*c, 0x2200), r2 = res(*c, 0x2300);
    const std::string n = cs.name;
    if (n.rfind("t1", 0) == 0) expectT1(r, r2, cs.type, n);
    else if (n == "t2") expectT2(r, n);
    else if (n == "t3") expectT3(r, n, *c, adpcmDisc);
    else if (n.rfind("t4", 0) == 0) expectT4(r, cs.type, n);
    // tela contra o Mesen 2
    const auto ref = readRef(dir + "/ref/" + n + ".mesen.txt");
    if (ref.empty()) { check(false, n + ": falta ref/" + n + ".mesen.txt (rode o t7_pcecd.sh)"); continue; }
    bool lastOk;
    int firstBad;
    const int same = compareFrames(run.hashes, ref, lastOk, firstBad);
    std::printf("%-10s %3d de %3zu quadros iguais ao Mesen 2%s\n", cs.name, same, run.hashes.size(), lastOk ? "" : " (último diferente)");
    check(lastOk, n + ": tela final igual ao Mesen 2 (primeira diferença no quadro " + std::to_string(firstBad) + ")");
    check(same >= int(run.hashes.size()) * 9 / 10, n + ": 90% dos quadros iguais ao Mesen 2");
    // som contra o Mesen 2 (CD-DA e ADPCM)
    if (n == "t2" || n == "t3") {
      std::vector<float> RR;
      const auto LL = readRaw(dir + "/ref/" + n + ".mesen.raw", RR);
      if (LL.empty()) { check(false, n + ": falta o som do Mesen 2"); continue; }
      // t2: a faixa 3 (tom) em laço nos quadros ~170-290 e o fader rápido até ~440; t3: o seno em ADPCM no fim
      const size_t from = size_t(n == "t2" ? 185 : 300) * 800, to = size_t(n == "t2" ? 285 : 690) * 800;
      const Sound a = analyze(run.l, run.r, from, to), b = analyze(LL, RR, from, to);
      // envelope: o tom e o fader (antes disso o Mesen 2 atrasa o CD-DA de forma variável ao sair das buscas)
      const Sound ea = analyze(run.l, run.r, 160 * 800, 460 * 800), eb = analyze(LL, RR, 160 * 800, 460 * 800);
      const double cl = envCorr(ea.envL, eb.envL), cr = envCorr(ea.envR, eb.envR);
      if (std::getenv("T7_ENV"))
        for (size_t k = 0; k < ea.envL.size(); k += 10)
          std::printf("  env %zu: %.3f %.3f | %.3f %.3f\n", k + 160, ea.envL[k], ea.envR[k], k < eb.envL.size() ? eb.envL[k] : -1.0, k < eb.envR.size() ? eb.envR[k] : -1.0);
      std::printf("%-10s som: freq %.1f/%.1f Hz (Mesen %.1f/%.1f), RMS %.3f/%.3f (Mesen %.3f/%.3f), envelope %.3f/%.3f\n",
                  cs.name, a.freqL, a.freqR, b.freqL, b.freqR, a.rmsL, a.rmsR, b.rmsL, b.rmsR, cl, cr);
      const double fl = n == "t2" ? 440 : 500, fr = n == "t2" ? 660 : 500;
      check(std::fabs(a.freqL - fl) < fl * 0.02 && std::fabs(a.freqR - fr) < fr * 0.02, n + ": frequência do tom");
      check(std::fabs(a.freqL - b.freqL) < fl * 0.02 && std::fabs(a.freqR - b.freqR) < fr * 0.02, n + ": frequência igual à do Mesen 2");
      check(a.rmsL > 0.05 && a.rmsR > 0.05, n + ": som audível");
      if (n == "t2") check(cl > 0.95 && cr > 0.95, n + ": envelope (CD-DA, pausa, fader) igual ao do Mesen 2");
    }
  }
}

// seno de 500 Hz em ADPCM: decodificado pelo núcleo tem de voltar a um seno (independe do Mesen)
void testAdpcmTable() {
  // valores do MSM5205 (passo 0, 1, 16 e 48)
  check(indigo::pcecd::CdUnit::seekMs(0, 1) == 150 && indigo::pcecd::CdUnit::seekMs(1000, 1003) == 216,
        "tempo de busca curto (150 e 216 ms)");
  const double far = indigo::pcecd::CdUnit::seekMs(0, 300000);
  check(far > 2000 && far < 2800, "tempo de busca de uma ponta à outra do disco (" + std::to_string(far) + " ms)");
}

void testBios() {
  std::vector<uint8_t> b(0x40000, 0x11);
  check(!indigo::pcecd::isPceCdBios(b), "256 KB sem o texto não é System Card");
  std::memcpy(b.data() + 0x3FFB6, "PC Engine CD-ROM SYSTEM", 23);
  check(indigo::pcecd::isPceCdBios(b), "256 KB com o texto é System Card");
  std::vector<uint8_t> h(512, 0);
  h.insert(h.end(), b.begin(), b.end());
  check(indigo::pcecd::isPceCdBios(h) && indigo::pcecd::normalizeBios(h) == b, "com cabeçalho de 512 bytes");
  std::vector<uint8_t> m = b;
  m.insert(m.end(), b.begin(), b.end());
  check(indigo::pcecd::isPceCdBios(m) && indigo::pcecd::normalizeBios(m) == b, "dump de 512 KB com espelho");
  std::vector<uint8_t> rv = b;
  for (auto& x : rv) { uint8_t y = 0; for (int k = 0; k < 8; k++) if (x & (1 << k)) y |= uint8_t(0x80 >> k); x = y; }
  check(indigo::pcecd::isPceCdBios(rv) && indigo::pcecd::normalizeBios(rv) == b, "dump americano com os bits invertidos");
  check(!indigo::pcecd::isPceCdBios(std::vector<uint8_t>(b.begin(), b.begin() + 0x20000)), "128 KB não é System Card");
  check(indigo::pcecd::modelForBios(b) == Model::ArcadeCdRom2, "System Card desconhecido roda com tudo ligado");
}

void testNoBios(const std::string& dir) {
  indigo::setFirmware("pcecd", {});
  bool threw = false;
  try {
    auto d = indigo::cdrom::open(dir + "/disco/teste.cue");
    PceCdCore c(std::move(d), nullptr);
  } catch (const std::exception&) { threw = true; }
  check(threw, "sem BIOS o núcleo não é criado");
  std::string err;
  check(!indigo::pcecd::createPceCdCore(dir + "/disco/teste.cue", nullptr, &err) && !err.empty(), "createPceCdCore sem BIOS: " + err);
  // com um "System Card" instalado (o de teste, com o texto)
  std::vector<uint8_t> card;
  readFile(dir + "/cards/card1.pce", card);
  check(indigo::pcecd::isPceCdBios(card), "o System Card de teste é reconhecido pelo texto");
  indigo::setFirmware("pcecd", card);
  auto c = indigo::pcecd::createPceCdCore(dir + "/disco/teste.cue", nullptr, &err);
  check(c != nullptr && c->system() == "pcecd" && c->title() == "teste", "createPceCdCore com a BIOS instalada");
  indigo::setFirmware("pcecd", {});
  auto d = indigo::cdrom::open(dir + "/disco/teste.cue");
  check(d && indigo::cdrom::detectSystem(*d) == "pcecd", "o disco de teste é reconhecido como PC Engine CD");
}

void testState(const std::string& dir) {
  int bad = 0, refused = 0, loaded = 0;
  for (const char* card : {"cards/card1.pce", "cards/card2.pce", "cards/card3.pce", "cards/card4.pce"}) {
    auto c = makeCore(dir + "/" + card, dir + "/disco/teste.cue", 2);
    if (!c) { check(false, "save state: não montou"); return; }
    for (int f = 0; f < 47; f++) c->runFrame();  // no meio de leituras, músicas e ADPCM
    for (int k = 0; k < 6; k++) {
      const auto st = c->saveState();
      c->loadState(st);
      if (c->saveState() != st) bad++;
      // determinismo: o mesmo estado, os mesmos quadros
      std::vector<uint32_t> h1, h2;
      for (int f = 0; f < 20; f++) { c->runFrame(); h1.push_back(frameHash(*c)); }
      const auto st2 = c->saveState();
      c->loadState(st);
      for (int f = 0; f < 20; f++) { c->runFrame(); h2.push_back(frameHash(*c)); }
      if (h1 != h2 || c->saveState() != st2) bad++;
      // estados danificados: recusados ou carregados sem travar
      uint32_t x = 12345u + uint32_t(k);
      for (int t = 0; t < 30; t++) {
        auto b = st;
        if (t % 3 == 0) b.resize(b.size() / 2);
        else for (int j = 0; j < 6; j++) { x = x * 1103515245u + 12345u; b[x % b.size()] ^= uint8_t(1 + (x >> 24) % 255); }
        const auto before = c->saveState();
        try { c->loadState(b); loaded++; for (int f = 0; f < 5; f++) c->runFrame(); c->loadState(before); }
        catch (const std::exception&) { refused++; if (c->saveState() != before) bad++; }
      }
      for (int f = 0; f < 31; f++) c->runFrame();
    }
  }
  check(bad == 0, "save state: ida e volta igual, determinístico, estado recusado sem mudar nada (" + std::to_string(bad) + " problemas)");
  std::printf("save state: %d carregados, %d recusados, %d problemas\n", loaded, refused, bad);
  // estado de outro modelo é recusado
  auto a = makeCore(dir + "/cards/card1.pce", dir + "/disco/teste.cue", 0);
  auto b = makeCore(dir + "/cards/card1.pce", dir + "/disco/teste.cue", 2);
  bool threw = false;
  try { b->loadState(a->saveState()); } catch (const std::exception&) { threw = true; }
  check(threw, "estado de CD-ROM² não carrega no Arcade CD-ROM²");
}

void testBram(const std::string& dir) {
  auto c = makeCore(dir + "/cards/card1.pce", dir + "/disco/teste.cue", 2);
  if (!c) return;
  check(!c->saveDirty(), "BRAM começa sem mudanças");
  for (int f = 0; f < 400; f++) c->runFrame();
  check(c->saveDirty(), "BRAM mudou (o teste grava $77 em $F7:0010)");
  const auto s = c->exportSave();
  check(s.size() == 2048 && s[0x10] == 0x77 && s[0] == 'H' && s[1] == 'U', "BRAM exportada como save");
  // o save volta no próximo núcleo
  std::vector<uint8_t> bios;
  readFile(dir + "/cards/card1.pce", bios);
  const Model m = Model::ArcadeCdRom2;
  PceCdCore c2(indigo::cdrom::open(dir + "/disco/teste.cue"), &s, &bios, &m);
  check(c2.exportSave() == s, "BRAM carregada do save");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 6 && (std::string(argv[1]) == "hashes" || std::string(argv[1]) == "shot")) {
    const bool shot = std::string(argv[1]) == "shot";
    auto c = makeCore(argv[2], argv[3], std::atoi(argv[4]));
    if (!c) return 1;
    const int frames = std::atoi(argv[5]);
    const std::string keys = argc > (shot ? 7 : 6) ? argv[shot ? 7 : 6] : "";
    const Run r = runCore(*c, frames, keys);
    if (shot) writePpm(*c, argv[6]);
    else for (size_t i = 0; i < r.hashes.size(); i++) std::printf("%zu %d %d %08x\n", i, c->width(), 240, r.hashes[i]);
    const auto a = res(*c, 0x2200);
    for (int i = 0; i < 28; i++) std::fprintf(stderr, "%2d: %s\n", i, hex(&a[size_t(i * 8)], 8).c_str());
    return 0;
  }
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta\n", argv[0]); return 1; }
  const std::string dir = argv[1];
  testBios();
  testAdpcmTable();
  testNoBios(dir);
  testCases(dir);
  testBram(dir);
  testState(dir);
  std::printf("RESULTADO pcecd: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
