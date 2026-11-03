// Teste do núcleo PC-FX: indigo-t7_pcfx <pasta> (preparada pelo t7_pcfx.sh)
// Só material próprio: a BIOS de teste do Índigo (escrita do zero, t7_pcfx_bios.c) dá a partida no CD de teste
// (t7_pcfx_disco.py), que carrega o programa t7_pcfx_prog.c. Confere:
//  1) reconhecimento da BIOS e do disco, e o erro claro sem a BIOS;
//  2) partida: a BIOS leu o TOC, o cabeçalho e carregou o programa por DMA (marca "BOOT" e os campos);
//  3) CPU V810: os resultados do teste de contas são iguais aos calculados no PC;
//  4) CD: arquivo de 64 KB lido por DMA para a KRAM e um setor por PIO, somas iguais às do arquivo;
//  5) interrupções com prioridade: vblank do VDC, temporizador, linha do KING e controle (com o botão certo);
//  6) memória de save com a proteção; save do jogo exportado;
//  7) tela: imagem calculada aqui (VDC A com fundo e sprite, VDC B por cima, fundos 0 e 1 do KING com as
//     prioridades do HuC6261 e a conversão YUV -> RGB) comparada ponto a ponto;
//  8) som: PSG a 998,7 Hz, CD-DA (faixa 2, seno de 441 Hz, direita com metade), ADPCM tocando, silêncio antes;
//  9) RAINBOW (vídeo comprimido): tabelas de Huffman, IDCT, faixas YUV (com tabelas, escala, colunas vazias e
//     bytes de enchimento) e de paleta com repetição, codificadas aqui e comparadas com uma IDCT de ponto
//     flutuante; depois o programa no V810 passa o fluxo pela KRAM e a tela é conferida, com chroma key;
// 10) mouse na porta 2 lido pelo programa (identificação, movimento acumulado, botões);
// 11) save state determinístico e estado de lixo recusado; velocidade (informativa).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/pcfx/pcfx.h"
#include "t7_pcfx_rb.h"

using namespace indigo;
using namespace indigo::pcfx;

namespace {
int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("  FALHOU: %s\n", what.c_str()); }
}
std::vector<uint8_t> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
uint32_t rd32(const Pcfx& s, uint32_t a) {
  return uint32_t(s.ram[a]) | uint32_t(s.ram[a + 1]) << 8 | uint32_t(s.ram[a + 2]) << 16 | uint32_t(s.ram[a + 3]) << 24;
}
enum { R_DONE, R_FRAMES, R_VBL, R_TIMER, R_RASTER, R_PAD, R_SUM, R_DSTAT, R_CPU, R_BRAM, R_KING, R_PHASE, R_PADIRQ,
       R_PIOSUM, R_TOC, R_CDCMD, R_SPRX, R_ADPCM, R_SUBQ, R_GO2, R_DONE2, R_GO3, R_DONE3, R_RBLEN, R_MOUSE, R_MOUSEX,
       R_MOUSEY, R_MOUSEB, R_MOUSEN, R_CKY, R_CKU, R_CKV };
uint32_t res(const Pcfx& s, int i) { return rd32(s, 0x100000 + uint32_t(i) * 4); }

// mesma fórmula do programa (palc) e da conversão do HuC6261
uint16_t palc(int i) {
  const uint16_t y = uint16_t(0x30 + ((i * 37) & 0x7F));
  return uint16_t(y << 8 | ((i * 5) & 15) << 4 | ((i * 11) & 15));
}
uint32_t rgbOf(uint16_t c) {
  const int y = c >> 8, u = (c & 0xF0) - 128, v = ((c & 0x0F) << 4) - 128;
  auto cl = [](int x) { return x < 0 ? 0 : x > 255 ? 255 : x; };
  const int r = cl(y + int(0 - 0.000039457070707 * u + 1.139827967171717 * v));
  const int g = cl(y + int(0 - 0.394610164141414 * u - 0.580500315656566 * v));
  const int b = cl(y + int(0 + 2.031999684343434 * u - 0.000481376262626 * v));
  return 0xFF000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | uint32_t(r);
}

uint32_t rgbYuv(uint32_t p) {
  const int y = (p >> 16) & 0xFF, u = int((p >> 8) & 0xFF) - 128, v = int(p & 0xFF) - 128;
  auto cl = [](int x) { return x < 0 ? 0 : x > 255 ? 255 : x; };
  const int r = cl(y + int(0 - 0.000039457070707 * u + 1.139827967171717 * v));
  const int g = cl(y + int(0 - 0.394610164141414 * u - 0.580500315656566 * v));
  const int b = cl(y + int(0 + 2.031999684343434 * u - 0.000481376262626 * v));
  return 0xFF000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | uint32_t(r);
}
int chanDiff(uint32_t a, uint32_t b) {
  int m = 0;
  for (int sh = 0; sh < 24; sh += 8) m = std::max(m, std::abs(int((a >> sh) & 0xFF) - int((b >> sh) & 0xFF)));
  return m;
}

// vídeo de teste do RAINBOW: 15 faixas de 256x16. Faixas 0..11 YUV (0 com as tabelas, 3 com escala nova, 5 com
// três colunas vazias, 7 com a última coluna vazia), 12..14 de paleta (tipos F0, F1 e F3)
struct RbVideo {
  std::vector<uint8_t> stream;
  std::vector<size_t> starts;       // começo de cada faixa no fluxo
  rbt::Out ref[15];                  // YUV esperado (faixas YUV)
  uint8_t idx[15][4096] = {};        // índices esperados (faixas de paleta)
  double psnrY = 0;
  int stuffed = 0, scaleQ0 = 0;
  uint8_t qAfterScale[2][64] = {};
  static constexpr uint32_t NULLC = 0x9070A0;  // registros de corrida nula 0x10, -0x10, 0x20
  static bool isYuv(int k) { return k < 12; }
};
RbVideo makeRbVideo() {
  RbVideo v;
  rbt::Encoder enc;
  enc.nullColor = RbVideo::NULLC;
  double se = 0;
  int n = 0;
  for (int k = 0; k < 15; k++) {
    v.starts.push_back(v.stream.size());
    std::vector<uint8_t> blk;
    if (RbVideo::isYuv(k)) {
      static rbt::Strip st;
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 256; x++) {
          const int gy = k * 16 + y;
          double l = 128 + 70 * std::sin(x / 23.0) * std::cos(gy / 17.0) + 30 * std::sin((x + gy) / 7.0);
          if ((x - 128) * (x - 128) + (gy - 100) * (gy - 100) < 40 * 40) l = 235;  // disco claro (bordas duras)
          if (x >= 200 && ((x / 4 + gy / 4) & 1)) l = 20;                        // xadrez fino
          st.y[y][x] = std::clamp(int(l), 0, 255);
        }
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 128; x++) {
          st.u[y][x] = std::clamp(128 + int(60 * std::sin(x / 19.0 + k)), 0, 255);
          st.v[y][x] = std::clamp(128 + int(50 * std::cos((k * 8 + y) / 11.0)) - (x > 64 ? 40 : 0), 0, 255);
        }
      rbt::Opt o;
      o.tables = k == 0;
      if (k == 3) o.scale = 8;
      if (k == 5) { o.nullFrom = 4; o.nullCount = 3; }
      if (k == 7) { o.nullFrom = 15; o.nullCount = 1; }
      blk = enc.yuv(st, o, v.ref[k]);
      if (k == 3) { std::memcpy(v.qAfterScale, enc.q, sizeof v.qAfterScale); v.scaleQ0 = enc.q[0][0]; }
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 256; x++) {
          const int c = x / 16;
          if ((k == 5 && c >= 4 && c < 7) || (k == 7 && c == 15)) continue;
          const double d = double((v.ref[k].px[y][x] >> 16) & 0xFF) - st.y[y][x];
          se += d * d;
          n++;
        }
    } else {
      static const int types[3] = {0xF0, 0xF1, 0xF3};
      const int type = types[k - 12];
      const int maxIdx = type == 0xF0 ? 15 : type == 0xF1 ? 31 : 127;
      for (int i = 0; i < 4096; i++) {
        const int x = i & 255, y = i >> 8;
        int c = ((x / 24) * 7 + y / 4) % (maxIdx + 1);
        if (x < 40) c = 0;                       // transparente
        if (y >= 8 && x >= 100) c = maxIdx;      // corrida longa (passa de 256 com o fim da linha)
        v.idx[k][i] = uint8_t(c);
      }
      blk = rbt::Encoder::rle(type, v.idx[k], v.stuffed);
    }
    v.stream.insert(v.stream.end(), blk.begin(), blk.end());
    // lixo entre as faixas (sem 0xFF): o chip procura o próximo marcador
    for (int i = 0; i < k % 3; i++) v.stream.push_back(uint8_t(0x11 * (i + 1)));
  }
  v.stuffed += enc.stuffed;
  v.psnrY = n ? 10 * std::log10(255.0 * 255.0 / (se / n)) : 0;
  if (v.stream.size() & 1) v.stream.push_back(0);
  return v;
}

// a tela do programa de teste depois do quadro 200 (parada), com o VDC deslocado (dx, dy)
uint32_t expected(int x, int y, int dx, int dy) {
  // VDC A: fundo (cor 1 à esquerda, listras 2/3 da paleta 1 à direita), sprite 16x16 cor 5 da paleta 2 em
  // (48, 150); VDC B: quadrado 32x32 cor 7 da paleta 2 em (16, 16)
  const int vx = x - dx, vy = y - dy;
  int vdc = 0;  // índice de 9 bits (bit 8 = sprite), 0 = transparente
  if (vx >= 0 && vx < 256 && vy >= 0 && vy < 240) {
    vdc = vx < 128 ? 1 : (16 + ((vx & 1) ? 3 : 2));
    const int sx = 40 + (200 & 63);
    if (vx >= sx && vx < sx + 16 && vy >= 150 && vy < 166) vdc = 0x100 | 0x25;
    if (vx >= 16 && vx < 48 && vy >= 16 && vy < 48 && !(vdc & 0x100)) vdc = 0x27;
  }
  // KING: fundo 0 (retângulo em faixas) e fundo 1 (faixa diagonal, rolagem 200, infinita) por cima
  int king = -1, kprio = 0;
  if (x >= 160 && x < 224 && y >= 40 && y < 104) { king = 0x20 + ((x >> 3) & 7); kprio = 2; }
  const int xx = (x + 200) & 255, tx = xx >> 3, ty = y >> 3;
  if (((tx - ty) & 31) < 4) { king = 0x80 + 0x10 + ((((xx & 7) + (y & 7)) & 15) | 1); kprio = 3; }
  (void)kprio;
  if (vdc & 0x100) return rgbOf(palc(0x100 + (vdc & 0xFF)));
  if (king >= 0) return rgbOf(palc(king));
  if (vdc) return rgbOf(palc(vdc));
  return rgbOf(palc(0));
}

double rms(const std::vector<float>& a, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to && i < a.size(); i++) s += double(a[i]) * a[i];
  return to > from ? std::sqrt(s / double(to - from)) : 0;
}
double freq(const std::vector<float>& a, size_t from, size_t to, int rate) {
  double m = 0;
  for (size_t i = from; i < to; i++) m += a[i];
  m /= double(to - from);
  int cross = 0;
  for (size_t i = from + 1; i < to; i++) if ((a[i - 1] - m < 0) != (a[i] - m < 0)) cross++;
  return cross / 2.0 / (double(to - from) / rate);
}
}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/pcfx";
  const std::vector<uint8_t> bios = readFile(dir + "/pcfx_bios_teste.rom");
  const std::vector<uint8_t> esperado = readFile(dir + "/cputeste.esperado");
  if (bios.size() != 0x100000 || esperado.empty()) {
    std::printf("falta o material de teste em %s (rode tools/t7_pcfx.sh)\n", dir.c_str());
    std::printf("RESULTADO pcfx: 1 verificações, 1 falhas\n");
    return 1;
  }

  std::printf("Reconhecimento:\n");
  check(isPcfxBios(bios, "pcfx_bios_teste.rom") && isPcfxBios(bios, "PCFX.ROM"), "BIOS reconhecida pelo nome e tamanho");
  check(!isPcfxBios(bios, "panafz10.bin") && !isPcfxBios(std::vector<uint8_t>(0x80000), "pcfx.rom"), "outros arquivos não viram BIOS");
  check(!isPcfxBios(bios, "pcfx_jogo.sfc") && !isPcfxBios(bios, "pc-fx hack.pce") && isPcfxBios(bios, "pc-fx bios.bin"),
        "só .rom/.bin com o nome do PC-FX viram BIOS");
  std::string err;
  auto disc = cdrom::open(dir + "/pcfxteste.cue", &err);
  check(disc != nullptr, "o CD de teste abre: " + err);
  if (!disc) { std::printf("RESULTADO pcfx: %d verificações, %d falhas\n", checks, fails); return 1; }
  check(isPcfxDisc(*disc) && cdrom::detectSystem(*disc) == "pcfx", "o CD é reconhecido como PC-FX");
  check(disc->tracks().size() == 2 && disc->tracks()[1].type == cdrom::TrackType::Audio, "faixa de dados + faixa de áudio");
  // soma do arquivo de dados direto da imagem
  uint32_t sumFile = 0, sumSector = 0;
  for (int s = 0; s < 32; s++) {
    uint8_t b[2048];
    disc->readData(disc->tracks()[0].start + 100 + uint32_t(s), b);
    for (int i = 0; i < 2048; i += 2) {
      const uint32_t w = uint32_t(b[i] | b[i + 1] << 8);
      sumFile += w;
      if (s == 1) sumSector += w;
    }
  }
  {
    bool threw = false;
    std::string msg;
    setFirmware("pcfx", {});
    try { PcfxCore c(cdrom::open(dir + "/pcfxteste.cue"), nullptr); } catch (const std::exception& e) { threw = true; msg = e.what(); }
    check(threw && msg.find("BIOS") != std::string::npos, "sem a BIOS o núcleo não abre e explica: " + msg);
  }

  PcfxCore core(std::move(disc), nullptr, &bios);
  Pcfx& s = core.sys();
  check(core.system() == "pcfx" && core.title() == "INDIGO PC-FX TESTE", "título do disco: " + core.title());
  check(core.note().find("teste") != std::string::npos, "nota da BIOS de teste");
  core.setSampleRate(48000);
  std::vector<float> aL, aR;
  std::vector<size_t> frameAt;
  const auto t0 = std::chrono::steady_clock::now();
  const int FRAMES = 270;
  for (int f = 0; f < FRAMES; f++) {
    // o programa conta os quadros a partir das interrupções ligadas; as fases do teste seguem a conta dele
    const uint32_t pf = res(s, R_FRAMES);
    core.setKeys((pf >= 30 && pf < 40) ? (KEY_A | KEY_RIGHT) : 0);
    core.runFrame();
    const size_t n = core.audioAvailable();
    std::vector<float> l(n), r(n);
    core.pullAudio(l.data(), r.data(), n);
    frameAt.push_back(aL.size());
    aL.insert(aL.end(), l.begin(), l.end());
    aR.insert(aR.end(), r.begin(), r.end());
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::printf("Partida pela BIOS de teste:\n");
  const uint32_t mark = 0x1FFF00;
  std::printf("  marca: %08X faixa %d, %u setores, carga %08X, partida %08X, passo %u\n", rd32(s, mark), int(rd32(s, mark + 4)),
              rd32(s, mark + 8), rd32(s, mark + 12), rd32(s, mark + 16), rd32(s, mark + 20));
  check(rd32(s, mark) == 0x544F4F42, "a BIOS carregou o programa (\"BOOT\")");
  check(rd32(s, mark + 4) == 0 && rd32(s, mark + 12) == 0x8000 && rd32(s, mark + 16) == 0x8000, "cabeçalho do CD lido (faixa, carga, partida)");
  std::printf("  programa: DONE=%08X quadros=%u fase=%u\n", res(s, R_DONE), res(s, R_FRAMES), res(s, R_PHASE));
  check(res(s, R_DONE) == 0x454E4F44, "o programa de teste terminou (\"DONE\")");

  std::printf("CPU V810:\n");
  {
    const uint32_t n = res(s, R_CPU);
    int bad = 0;
    for (uint32_t i = 0; i < n && i * 4 + 3 < esperado.size(); i++) {
      const uint32_t want = uint32_t(esperado[i * 4]) | uint32_t(esperado[i * 4 + 1]) << 8 | uint32_t(esperado[i * 4 + 2]) << 16 | uint32_t(esperado[i * 4 + 3]) << 24;
      if (rd32(s, 0x110000 + i * 4) != want) { if (bad < 4) std::printf("  resultado %u: %08X, esperado %08X\n", i, rd32(s, 0x110000 + i * 4), want); bad++; }
    }
    std::printf("  %u resultados, %d diferentes\n", n, bad);
    check(n * 4 == esperado.size() && bad == 0, "contas iguais às do PC");
  }

  std::printf("CD:\n");
  std::printf("  TOC %d, estado %08X, soma DMA %08X (arquivo %08X), PIO %08X (setor %08X)\n", int(res(s, R_TOC)), res(s, R_DSTAT),
              res(s, R_SUM), sumFile, res(s, R_PIOSUM), sumSector + (2048u << 24));
  check(res(s, R_TOC) == 0 && res(s, R_DSTAT) == 0, "READ TOC e READ(10) sem erro");
  check(res(s, R_SUM) == sumFile, "64 KB por DMA para a KRAM iguais ao arquivo");
  check(res(s, R_PIOSUM) == sumSector + (2048u << 24), "setor por PIO igual ao arquivo");
  std::printf("  CD-DA: comandos %06X, subcanal Q %06X\n", res(s, R_CDCMD), res(s, R_SUBQ));
  check(res(s, R_CDCMD) == 0, "comandos de áudio da NEC (D8, D9, DA) aceitos");
  check((res(s, R_SUBQ) & 0xFF) == 2 && ((res(s, R_SUBQ) >> 8) & 0xFF) == 0x02, "subcanal Q: pausado na faixa 2");

  std::printf("Interrupções:\n");
  const uint32_t vbl = res(s, R_VBL);
  std::printf("  vblank %u, temporizador %u, linha %u, KING %u, controle %u (valor %08X)\n", vbl, res(s, R_TIMER), res(s, R_RASTER),
              res(s, R_KING), res(s, R_PADIRQ), res(s, R_PAD));
  check(vbl >= 240 && vbl == res(s, R_FRAMES) && vbl <= FRAMES, "vblank do VDC A uma vez por quadro");
  const double perFrame = Pcfx::CPU_HZ / 59.83 / 15000.0;
  check(std::fabs(res(s, R_TIMER) - perFrame * vbl) < perFrame * vbl * 0.06, "temporizador (1000 x 15 ciclos): " + std::to_string(perFrame) + " por quadro");
  check(res(s, R_RASTER) + 3 >= vbl && res(s, R_RASTER) <= vbl + 3, "interrupção de linha do KING uma vez por quadro");
  check(res(s, R_PADIRQ) + 3 >= vbl, "controle lido por interrupção a cada quadro");
  check(res(s, R_PAD) == (0xF0000000u | 1u | (1u << 9)), "botões I e direita no quadro 35");

  std::printf("Memória de save:\n");
  check(res(s, R_BRAM) == (uint32_t('I') | uint32_t('N') << 8 | uint32_t('D') << 16 | uint32_t('O') << 24), "gravação com a proteção aberta e recusada com ela fechada");
  check(core.hasSave() && core.saveDirty(), "o save do jogo ficou sujo");
  const std::vector<uint8_t> sv = core.exportSave();
  check(sv.size() == 0x10000 && sv[0x1000] == 'I' && sv[0x1003] == 'O' && std::memcmp(&sv[3], "PCFXSram", 8) == 0, "save exportado (64 KB, formatado)");

  std::printf("Tela:\n");
  {
    const uint32_t* fb = core.framebuffer();
    check(core.width() == 256 && core.height() == 240, "256x240");
    int best = -1, bdx = 0, bdy = 0;
    for (int dy = -4; dy <= 4; dy++)
      for (int dx = -12; dx <= 12; dx++) {
        int bad = 0;
        for (int y = 0; y < 240; y++)
          for (int x = 0; x < 256; x++) bad += fb[y * 256 + x] != expected(x, y, dx, dy);
        if (best < 0 || bad < best) { best = bad; bdx = dx; bdy = dy; }
      }
    int bad0 = 0;
    for (int y = 0; y < 240; y++)
      for (int x = 0; x < 256; x++) bad0 += fb[y * 256 + x] != expected(x, y, 0, 0);
    std::printf("  pontos diferentes: %d (melhor deslocamento do VDC: %d,%d com %d)\n", bad0, bdx, bdy, best);
    check(bad0 == 0, "imagem igual à calculada (VDC A/B, sprite, fundos do KING, prioridades, cores)");
    // dump PPM para olhar, se pedido
    if (argc > 2) {
      FILE* f = std::fopen(argv[2], "wb");
      std::fprintf(f, "P6 256 240 255\n");
      for (int i = 0; i < 256 * 240; i++) { const uint32_t p = fb[i]; std::fputc(p & 255, f); std::fputc((p >> 8) & 255, f); std::fputc((p >> 16) & 255, f); }
      std::fclose(f);
    }
  }

  std::printf("Tela de 7,16 MHz com rotação:\n");
  {
    auto w32 = [&](uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) s.ram[a + uint32_t(i)] = uint8_t(v >> (8 * i)); };
    w32(0x100000 + R_GO2 * 4, 1);
    for (int i = 0; i < 4; i++) core.runFrame();
    check(res(s, R_DONE2) == 0x32454E4F, "o programa trocou de tela");
    check(core.width() == 341, "341 pontos por linha: " + std::to_string(core.width()));
    const uint32_t* fb = core.framebuffer();
    int bad = 0, seen = 0;
    for (int y = 0; y < 240; y++) {
      for (int x = 0; x < 341; x++) {
        const int xb = x * 256 / 341;
        int king = -1;
        const int nx = ((xb - 192) * 0x80 + (192 << 8)) >> 8, ny = ((y - 72) * 0x80 + (72 << 8)) >> 8;
        if (nx >= 160 && nx < 224 && ny >= 40 && ny < 104) king = 0x20 + ((nx >> 3) & 7);
        const int xx = (xb + 200) & 255, tx = xx >> 3, ty = y >> 3;
        if (((tx - ty) & 31) < 4) king = 0x90 + ((((xx & 7) + (y & 7)) & 15) | 1);
        if (king < 0) continue;
        seen++;
        if (fb[y * 341 + x] != rgbOf(palc(king))) { if (bad < 5) std::printf("  (%d,%d) %08X esperado %08X (%02X)\n", x, y, fb[y * 341 + x], rgbOf(palc(king)), king); bad++; }
      }
    }
    std::printf("  %d pontos dos fundos do KING conferidos, %d diferentes\n", seen, bad);
    if (argc > 3) {
      FILE* f = std::fopen(argv[3], "wb");
      std::fprintf(f, "P6 341 240 255\n");
      for (int i = 0; i < 341 * 240; i++) { const uint32_t p = fb[i]; std::fputc(p & 255, f); std::fputc((p >> 8) & 255, f); std::fputc((p >> 16) & 255, f); }
      std::fclose(f);
    }
    check(seen > 10000 && bad == 0, "fundo 0 com zoom 2x e fundo 1 esticados para 341 pontos");
    // alinhamento VDC -> KING a 7,16 MHz: o VDC começa 8 + (HDS+1)*8 pontos depois do fim do HSYNC (203 ciclos do
    // começo da linha) e a imagem do HuC6261 aos 341 ciclos; com HDS = 2 o VDC fica 14 pontos à esquerda, e as
    // listras do VDC A (a partir do ponto 128 dele) começam no ponto 114 da tela (linha 150, sem fundos do KING)
    int edge = -1;
    for (int x = 100; x < 140 && edge < 0; x++) if (fb[150 * 341 + x] == rgbOf(palc(16 + 2)) || fb[150 * 341 + x] == rgbOf(palc(16 + 3))) edge = x;
    std::printf("  listras do VDC a partir do ponto %d\n", edge);
    check(edge == 114 && fb[150 * 341 + 113] == rgbOf(palc(1)), "VDC a 7,16 MHz alinhado com o KING pelo tempo do HSYNC");
  }

  const uint32_t framesSom = res(s, R_FRAMES);  // a conta do som usa os quadros até aqui
  std::printf("RAINBOW (descompressor):\n");
  const RbVideo vid = makeRbVideo();
  {
    check(rbt::prefixFree(RB_DC_Y) && rbt::prefixFree(RB_DC_UV) && rbt::prefixFree(RB_AC_Y) && rbt::prefixFree(RB_AC_UV),
          "tabelas de Huffman do chip sem código que comece outro");
    // IDCT inteira: só DC dá bloco liso; coeficientes variados ficam a 1 da IDCT de ponto flutuante
    int32_t b[64] = {};
    b[0] = 80;
    rbIdct(b);
    bool flat = true;
    for (int i = 0; i < 64; i++) flat &= b[i] == 10;
    int worst = 0;
    uint32_t x = 777;
    for (int t = 0; t < 200; t++) {
      int32_t c[64];
      double in[64];
      for (int i = 0; i < 64; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; c[i] = int32_t(x % 801) - 400; if (i > 20 && (x & 3)) c[i] = 0; in[i] = c[i]; }
      rbIdct(c);
      for (int yy = 0; yy < 8; yy++)
        for (int xx = 0; xx < 8; xx++) {
          double sum = 0;
          for (int v = 0; v < 8; v++)
            for (int u = 0; u < 8; u++)
              sum += (u ? 1 : 1 / std::sqrt(2.0)) * (v ? 1 : 1 / std::sqrt(2.0)) * in[v * 8 + u] *
                     std::cos((2 * xx + 1) * u * M_PI / 16) * std::cos((2 * yy + 1) * v * M_PI / 16);
          worst = std::max(worst, std::abs(c[yy * 8 + xx] - int(std::lround(sum / 4))));
        }
    }
    check(flat && worst <= 1, "IDCT 8x8: DC 80 -> 10 em todo o bloco; 200 blocos aleatórios a no máximo 1 da exata (" + std::to_string(worst) + ")");

    // decodifica as 15 faixas direto da "KRAM" (página 0, palavra 0x30000)
    static uint16_t kr[262144];
    std::memset(kr, 0, sizeof kr);
    for (size_t i = 0; i < vid.stream.size(); i += 2) kr[0x30000 + i / 2] = uint16_t(vid.stream[i] | vid.stream[i + 1] << 8);
    static Rainbow rb;
    rb.reset();
    rb.write16(0x208, 0x10); rb.write16(0x20C, 0xFFF0); rb.write16(0x210, 0x20);
    uint32_t pos = 0x30000 << 1;
    rb.decodeBlock(true, kr, pos);
    check(rb.format(rb.backBuffer()) == -1, "com o RAINBOW desligado nada é descomprimido");
    rb.write16(0x204, 1);
    pos = 0x30000 << 1;
    int yuvWorst = 0, yuvExact = 0, yuvTotal = 0, idxBad = 0, nullBad = 0;
    bool fmtOk = true, qOk = true, posOk = true;
    for (int k = 0; k < 15; k++) {
      rb.decodeBlock(k == 0, kr, pos);
      const int bb = rb.backBuffer();
      fmtOk &= rb.format(bb) == (RbVideo::isYuv(k) ? 1 : 0);
      if (RbVideo::isYuv(k)) {
        const uint32_t* px = rb.yuv(bb);
        for (int y = 0; y < 16; y++)
          for (int xx = 0; xx < 256; xx++) {
            const uint32_t a = px[y * 256 + xx], e = vid.ref[k].px[y][xx];
            const bool isNull = (k == 5 && xx / 16 >= 4 && xx / 16 < 7) || (k == 7 && xx / 16 == 15);
            if (isNull) { nullBad += a != RbVideo::NULLC; continue; }
            int d = 0;
            for (int sh = 0; sh < 24; sh += 8) d = std::max(d, std::abs(int((a >> sh) & 0xFF) - int((e >> sh) & 0xFF)));
            yuvWorst = std::max(yuvWorst, d);
            yuvExact += d == 0;
            yuvTotal++;
          }
        if (k == 3)
          for (int t = 0; t < 2; t++)
            for (int i = 0; i < 64; i++) qOk &= rb.quant(t, i) == vid.qAfterScale[t][i];
      } else {
        idxBad += std::memcmp(rb.idx(bb), vid.idx[k], 4096) != 0;
      }
      // a leitura para no fim do bloco ou pouco antes (o resto é pulado na busca do próximo marcador)
      if (k < 14) posOk &= (pos >> 1) - 0x30000 <= vid.starts[size_t(k) + 1] / 2 + 1;
      rb.swapBuffers();
    }
    std::printf("  fluxo de %zu bytes (%d bytes de enchimento), Y a %.1f dB do original; YUV: %d de %d pontos exatos, "
                "maior diferença %d\n", vid.stream.size(), vid.stuffed, vid.psnrY, yuvExact, yuvTotal, yuvWorst);
    check(fmtOk, "formato de cada faixa (12 YUV, 3 de paleta)");
    check(yuvWorst <= 1 && yuvExact * 10 > yuvTotal * 9, "faixas YUV iguais à IDCT de ponto flutuante (diferença <= 1)");
    check(vid.psnrY > 28, "codificação de ida e volta fiel à imagem original (PSNR do Y > 28 dB)");
    check(vid.stuffed > 0, "o fluxo tem bytes 0xFF com enchimento");
    check(qOk && vid.scaleQ0 == 8, "código de escala refaz as tabelas de quantização (Y x2, DC do UV / 4)");
    check(nullBad == 0, "colunas vazias pintadas com a cor dos registros de corrida nula");
    check(idxBad == 0, "faixas de paleta com repetição (F0, F1, F3; contagens longas) iguais");
    check(posOk, "cada bloco lido só até o seu fim");

    // leitura das linhas: rolagem, modo infinito, paleta e transparência; 16 linhas e o buffer se apaga
    rb.reset();
    rb.write16(0x204, 1);
    pos = uint32_t(0x30000 * 2 + vid.starts[1]);
    rb.decodeBlock(true, kr, pos);   // faixa 1 (YUV)
    rb.swapBuffers();
    const uint32_t* front = rb.yuv(rb.backBuffer() ^ 1);
    std::vector<uint32_t> f0(front, front + 4096);
    uint32_t pal[512];
    for (int i = 0; i < 512; i++) pal[i] = 0x10000u * uint32_t(i & 0xFF) + 0x0101u;
    uint32_t line[256];
    rb.write16(0x200, 10);
    int t1 = rb.fetchRaster(line, pal, 0, 7u << 28);
    bool scrollOk = t1 == 1 && line[0] == (f0[10] | 7u << 28) && line[245] == (f0[255] | 7u << 28) && line[246] == 0;
    rb.write16(0x204, 3);  // modo infinito
    rb.fetchRaster(line, pal, 0, 7u << 28);
    scrollOk &= line[246] == (f0[256 + 0] | 7u << 28) && line[0] == (f0[256 + 10] | 7u << 28);
    for (int i = 2; i < 16; i++) rb.fetchRaster(line, pal, 0, 0);
    scrollOk &= rb.fetchRaster(line, pal, 0, 0) == -1 && line[0] == 0;
    check(scrollOk, "linhas do RAINBOW: rolagem horizontal, modo infinito e fim da faixa depois de 16 linhas");
    rb.write16(0x200, 0);
    rb.write16(0x204, 1);
    pos = uint32_t(0x30000 * 2 + vid.starts[12]);
    rb.decodeBlock(true, kr, pos);   // faixa 12 (paleta)
    rb.swapBuffers();
    const int t2 = rb.fetchRaster(line, pal, 0x80, 7u << 28);
    check(t2 == 0 && line[0] == 0 && line[100] == (pal[0x80 + vid.idx[12][100]] | 7u << 28),
          "paleta do RAINBOW com o deslocamento da camada; índice 0 transparente");
    // bytes 0x8000 bytes sem marcador: a busca desiste e o buffer fica vazio, sem travar
    for (int i = 0; i < 0x4000; i++) kr[0x20000 + i] = 0x1234;
    pos = 0x20000 * 2;
    rb.decodeBlock(true, kr, pos);
    const bool g1 = rb.format(rb.backBuffer()) == 0 && rb.idx(rb.backBuffer())[0] == 0;
    const uint32_t p1 = pos;
    rb.decodeBlock(false, kr, pos);
    check(g1 && p1 - 0x40000 <= 0x210 && pos - p1 <= 8, "lixo sem marcador: desiste em 512 bytes e depois não procura mais");
  }

  std::printf("Vídeo comprimido na tela e mouse:\n");
  {
    for (size_t i = 0; i < vid.stream.size(); i++) s.ram[0x180000 + i] = vid.stream[i];
    auto w32 = [&](int r, uint32_t v) { for (int i = 0; i < 4; i++) s.ram[0x100000 + uint32_t(r) * 4 + uint32_t(i)] = uint8_t(v >> (8 * i)); };
    w32(R_RBLEN, uint32_t(vid.stream.size() / 2));
    w32(R_GO3, 1);
    int sentX = 0, sentY = 0;
    for (int f = 0; f < 24; f++) {
      if (f >= 4 && f < 14) { core.setMouse(7, -4, 1); sentX += 7; sentY -= 4; }
      else if (f == 14) { core.setMouse(300, 0, 2); sentX += 300; }
      else core.setMouse(0, 0, 0);
      core.runFrame();
    }
    check(res(s, R_DONE3) == 0x33454E4F, "o programa armou o RAINBOW");
    check(core.width() == 256, "volta a 256 pontos");
    const uint32_t* fb = core.framebuffer();
    const uint32_t bpc = rgbOf(palc(0));
    int worst = 0, exact = 0, total = 0, palBad = 0, nullBad = 0;
    for (int y = 0; y < 240; y++)
      for (int x = 0; x < 256; x++) {
        const int k = y / 16;
        const uint32_t got = fb[y * 256 + x];
        if (RbVideo::isYuv(k)) {
          const bool isNull = (k == 5 && x / 16 >= 4 && x / 16 < 7) || (k == 7 && x / 16 == 15);
          if (isNull) { nullBad += got != rgbYuv(RbVideo::NULLC); continue; }
          const int d = chanDiff(got, rgbYuv(vid.ref[k].px[y & 15][x]));
          worst = std::max(worst, d);
          exact += d == 0;
          total++;
        } else {
          const int i = vid.idx[k][(y & 15) * 256 + x];
          palBad += got != (i ? rgbOf(palc(0x80 + i)) : bpc);
        }
      }
    std::printf("  YUV: %d de %d pontos exatos, maior diferença de canal %d; paleta: %d diferentes\n", exact, total, worst, palBad);
    check(worst <= 4 && exact * 10 > total * 8 && nullBad == 0, "faixas YUV na tela (linhas 22 a 213), com as colunas vazias");
    check(palBad == 0, "faixas de paleta na tela, com a cor de trás onde o índice é 0");
    std::printf("  mouse: último %08X, X %d (enviado %d), Y %d (enviado %d), botões %u, %u leituras\n", res(s, R_MOUSE),
                int32_t(res(s, R_MOUSEX)), sentX, int32_t(res(s, R_MOUSEY)), sentY, res(s, R_MOUSEB), res(s, R_MOUSEN));
    check(s.mouseOn(1) && !s.mouseOn(0) && (res(s, R_MOUSE) >> 28) == 0xD, "mouse na porta 2 (identificação 0xD), controle na 1");
    check(int32_t(res(s, R_MOUSEX)) == sentX && int32_t(res(s, R_MOUSEY)) == sentY,
          "movimento somado igual ao enviado (300 de uma vez sai em partes de até 127)");
    check(res(s, R_MOUSEB) == 3 && res(s, R_MOUSEN) >= 15, "botões esquerdo e direito; uma leitura por quadro");

    // chroma key: Y de 0x80 para cima vira transparente (aparece a cor de trás)
    w32(R_CKY, 0xFF80); w32(R_CKU, 0xFF00); w32(R_CKV, 0xFF00);
    w32(R_GO3, 2);
    for (int f = 0; f < 3; f++) core.runFrame();
    check(res(s, R_DONE3) == 0x34454E4F, "o programa ligou o chroma key");
    int keyBad = 0, keyed = 0;
    for (int y = 0; y < 192; y++)
      for (int x = 0; x < 256; x++) {
        const int k = y / 16;
        if ((k == 5 && x / 16 >= 4 && x / 16 < 7) || (k == 7 && x / 16 == 15)) continue;
        const uint32_t e = vid.ref[k].px[y & 15][x];
        const int yy = int((e >> 16) & 0xFF);
        if (std::abs(yy - 0x80) <= 1) continue;
        const uint32_t got = fb[y * 256 + x];
        if (yy > 0x80) { keyed++; keyBad += got != bpc; }
        else keyBad += chanDiff(got, rgbYuv(e)) > 4;
      }
    check(keyed > 5000 && keyBad == 0, "chroma key do HuC6261 no plano YUV (" + std::to_string(keyed) + " pontos chaveados)");
  }

  std::printf("Som:\n");
  {
    const int off = FRAMES - int(framesSom);  // quadros do console antes do programa ligar as interrupções
    std::printf("  o programa começou a contar no quadro %d\n", off);
    auto win = [&](int f0, int f1) { return std::make_pair(frameAt[size_t(f0 + off)], frameAt[size_t(f1 + off)]); };
    auto [s0, e0] = win(10, 50);
    auto [s1, e1] = win(75, 110);
    auto [s2, e2] = win(140, 170);
    auto [s3, e3] = win(190, 230);
    const double r0 = rms(aL, s0, e0), r1 = rms(aL, s1, e1), r2 = rms(aL, s2, e2), r2r = rms(aR, s2, e2), r3 = rms(aL, s3, e3);
    const double f1 = freq(aL, s1, e1, 48000), f2 = freq(aL, s2, e2, 48000);
    std::printf("  %zu amostras; silêncio RMS %.4f; PSG %.1f Hz RMS %.3f; CD-DA %.1f Hz RMS %.3f/%.3f; ADPCM RMS %.3f (%llu nibbles)\n",
                aL.size(), r0, f1, r1, f2, r2, r2r, r3, static_cast<unsigned long long>(s.sound.adpcmNibbles));
    check(aL.size() > size_t(FRAMES) * 795 && aL.size() < size_t(FRAMES) * 810, "48 kHz: ~802 amostras por quadro");
    check(r0 < 0.01, "silêncio antes do som");
    check(std::fabs(f1 - 998.7) < 15 && r1 > 0.05, "PSG: onda quadrada de 998,7 Hz");
    check(std::fabs(f2 - 441) < 6 && r2 > 0.05 && std::fabs(r2 / r2r - 2.0) < 0.15, "CD-DA: seno de 441 Hz da faixa 2 (direita com metade)");
    check(r3 > 0.01 && s.sound.adpcmNibbles > 10000, "ADPCM tocando da KRAM");
    check((res(s, R_ADPCM) & 1) == 1, "ADPCM: fim do bloco marcado (com repetição)");
  }

  std::printf("Save state:\n");
  {
    const std::vector<uint8_t> st = core.saveState();
    for (int i = 0; i < 20; i++) core.runFrame();
    const std::vector<uint32_t> a(core.framebuffer(), core.framebuffer() + core.width() * 240);
    const std::vector<uint8_t> ramA = s.ram;
    core.loadState(st);
    for (int i = 0; i < 20; i++) core.runFrame();
    const std::vector<uint32_t> b(core.framebuffer(), core.framebuffer() + core.width() * 240);
    check(a == b && ramA == s.ram, "estado carregado continua igual (tela e RAM depois de 20 quadros)");
    bool threw = false;
    try { core.loadState(std::vector<uint8_t>(st.begin(), st.begin() + 1000)); } catch (const std::exception&) { threw = true; }
    check(threw, "estado truncado é recusado");
    for (int i = 0; i < 5; i++) core.runFrame();
    check(res(s, R_DONE) == 0x454E4F44, "o jogo continua depois do estado recusado");

    // relógios adulterados: acha no estado os int64 que parecem instantes (perto de ts) e os doubles idem,
    // troca cada um por um valor absurdo e confere que o estado é recusado ou roda sem travar
    const std::vector<uint8_t> boa = core.saveState();
    const int64_t agora = s.ts;
    std::vector<size_t> i64, f64;
    for (size_t o = 0; o + 8 <= boa.size(); o++) {
      int64_t v; double d;
      std::memcpy(&v, &boa[o], 8); std::memcpy(&d, &boa[o], 8);
      if (v > agora - (1 << 24) && v < agora + (1 << 24) && v > 1000) i64.push_back(o);
      else if (std::isfinite(d) && d > double(agora) - 1e7 && d < double(agora) + 1e7 && d > 1000) f64.push_back(o);
    }
    int recusados = 0, testes = 0;
    bool travou = false;
    const int64_t ruins[] = {-(int64_t(1) << 60), int64_t(1) << 60, INT64_MAX - 5};
    for (size_t o : i64)
      for (int64_t r : ruins) {
        std::vector<uint8_t> bad = boa;
        std::memcpy(&bad[o], &r, 8);
        testes++;
        try {
          core.loadState(bad);
        } catch (const std::exception&) { recusados++; continue; }
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; i++) core.runFrame();
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) travou = true;
        core.loadState(boa);
      }
    for (size_t o : f64) {
      std::vector<uint8_t> bad = boa;
      const double nan = std::nan("");
      std::memcpy(&bad[o], &nan, 8);
      testes++;
      try { core.loadState(bad); } catch (const std::exception&) { recusados++; continue; }
      for (int i = 0; i < 5; i++) core.runFrame();
      core.loadState(boa);
    }
    std::printf("  %zu instantes e %zu doubles achados no estado; %d de %d adulterações recusadas\n", i64.size(), f64.size(),
                recusados, testes);
    check(i64.size() >= 8 && f64.size() >= 2 && !travou, "relógios adulterados no estado não travam o núcleo");
    // estados embaralhados com o RAINBOW e o mouse em uso: recusados ou rodam sem cair; o bom volta igual
    {
      uint32_t z = 4242;
      int aceitos = 0;
      for (int k = 0; k < 150; k++) {
        std::vector<uint8_t> bad = boa;
        for (int m = 0; m < 24; m++) { z ^= z << 13; z ^= z >> 17; z ^= z << 5; bad[z % bad.size()] ^= uint8_t(z >> 8 | 1); }
        try { core.loadState(bad); } catch (const std::exception&) { continue; }
        aceitos++;
        core.setMouse(int(z % 9) - 4, 3, z & 3);
        for (int i = 0; i < 2; i++) core.runFrame();
      }
      core.loadState(boa);
      core.setMouse(0, 0, 0);
      for (int i = 0; i < 3; i++) core.runFrame();
      const std::vector<uint32_t> c1(core.framebuffer(), core.framebuffer() + core.width() * 240);
      core.loadState(boa);
      for (int i = 0; i < 3; i++) core.runFrame();
      const std::vector<uint32_t> c2(core.framebuffer(), core.framebuffer() + core.width() * 240);
      std::printf("  150 estados embaralhados com o RAINBOW ligado: %d aceitos e rodados\n", aceitos);
      check(c1 == c2 && res(s, R_DONE3) == 0x34454E4F, "estados embaralhados com o RAINBOW não derrubam; o estado bom volta igual");
    }
    for (int i = 0; i < 5; i++) core.runFrame();
    check(res(s, R_DONE) == 0x454E4F44, "o jogo continua depois dos estados adulterados");
  }
  std::printf("Robustez:\n");
  {
    // BIOS de lixo (código aleatório) e estados de lixo: nada pode travar nem derrubar o programa
    std::vector<uint8_t> junk(0x100000);
    uint32_t x = 12345;
    for (auto& v : junk) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v = uint8_t(x); }
    bool ok = true;
    try {
      PcfxCore j(cdrom::open(dir + "/pcfxteste.cue"), nullptr, &junk);
      for (int i = 0; i < 60; i++) j.runFrame();
      std::vector<uint8_t> st = j.saveState();
      for (int k = 0; k < 200; k++) {
        std::vector<uint8_t> bad = st;
        for (int m = 0; m < 16; m++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; bad[x % bad.size()] ^= uint8_t(x >> 8); }
        try { j.loadState(bad); j.runFrame(); } catch (const std::exception&) {}
      }
    } catch (...) { ok = false; }
    check(ok, "BIOS de lixo roda 60 quadros e 200 estados embaralhados não derrubam o núcleo");
  }
  std::printf("Velocidade: %.0f quadros/s (o console roda a 59,8)\n", FRAMES / secs);
  std::printf("RESULTADO pcfx: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
