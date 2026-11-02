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
//  9) save state determinístico e estado de lixo recusado; velocidade (informativa).
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
       R_PIOSUM, R_TOC, R_CDCMD, R_SPRX, R_ADPCM, R_SUBQ, R_GO2, R_DONE2 };
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
  }

  std::printf("Som:\n");
  {
    const int off = FRAMES - int(res(s, R_FRAMES));  // quadros do console antes do programa ligar as interrupções
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
