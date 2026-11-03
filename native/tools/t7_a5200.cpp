// Testes do núcleo Atari 5200.
//   indigo-t7_a5200 pasta          roda as verificações (pasta = /root/indigo-env/roms/a5200, preparada por
//                                  tools/t7_a5200.sh: BIOS de teste e cartucho de teste montados com o dasm)
//   indigo-t7_a5200 --png rom quadros saida.png    grava a tela (para conferir a olho; usa a BIOS de teste)
//
// O que é conferido:
//   - reconhecimento (.a52, .car, .bin de 5200 e os .bin de outros consoles que não podem virar 5200)
//   - mapeamento dos cartuchos (4, 8, 16 KB de um e de dois chips, 32 KB, Bounty Bob, super cartucho)
//   - partida pela BIOS de teste, NMI de VBI e de DLI pelos vetores de RAM, WSYNC, VCOUNT, troca de cor no meio
//     da linha, potenciômetro, gatilho, teclado e botão de baixo com IRQ do POKEY, som (frequência do tom)
//   - ANTIC: centenas de telas sorteadas (modos 2 a F, larguras, LMS, linhas em branco, rolagem fina horizontal
//     e vertical, CHACTL) comparadas ponto a ponto com um desenhista de referência escrito à parte aqui
//   - ciclos roubados pelo ANTIC por linha (refresh, lista, jogadores/mísseis, modos de texto e de bitmap)
//   - ANTIC ciclo a ciclo: mapa exato dos ciclos roubados (nomes, desenhos, bitmap, refresh adiado), CHBASE,
//     CHACTL e DMACTL trocados num ciclo exato no meio da linha, VSCROL comparado no fim da linha, brancos que
//     fecham a região rolada, troca de CHBASE pela CPU depois de WSYNC (borda da linha ou meio dela), estado
//     gravado no meio da linha
//   - GTIA no meio da linha: HPOS trocado durante a imagem, jogador reaproveitado, colisão lida no meio da linha
//   - POKEY: primeira IRQ do temporizador na grade do divisor de 64 kHz, contador do POT na grade de 15 kHz
//   - GTIA: posição e tamanho de jogadores e mísseis, DMA de linha simples e dupla, VDELAY, prioridades
//     (PRIOR 1, 2, 4, 8 e 0), quinto jogador, cores múltiplas, colisões e HITCLR, modos GTIA 1, 2 e 3
//   - save state determinístico e estados estragados recusados sem estragar a máquina
//   - (se existir altirra5200.rom na pasta, a BIOS livre do Altirra) o cartucho de teste liga por ela
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../src/a5200/a5200.h"

using namespace indigo;
using indigo::a5200::A5200;
using indigo::a5200::Cart;

namespace {

int fails = 0, checks = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA %s\n", what.c_str()); }
}
void info(const std::string& s) { std::printf("  %s\n", s.c_str()); }

std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- PNG sem compressão
uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = ~0u;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
void writePng(const std::string& path, const uint32_t* px, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const uint32_t c = px[y * w + x];
      raw.push_back(c & 0xFF); raw.push_back((c >> 8) & 0xFF); raw.push_back((c >> 16) & 0xFF);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n == raw.size());
    z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + static_cast<long>(i), raw.begin() + static_cast<long>(i + n));
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  const uint32_t ad = (b << 16) | a;
  for (int k = 3; k >= 0; k--) z.push_back((ad >> (8 * k)) & 0xFF);
  std::ofstream f(path, std::ios::binary);
  auto be = [&](uint32_t v) { uint8_t t[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; f.write((char*)t, 4); };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
    be(static_cast<uint32_t>(d.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), d.begin(), d.end());
    f.write((char*)td.data(), static_cast<long>(td.size()));
    be(crc32(td.data(), td.size()));
  };
  f.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ih = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                             uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
  chunk("IHDR", ih);
  chunk("IDAT", z);
  chunk("IEND", {});
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

std::vector<uint8_t> BIOS;

// cartucho mínimo (32 KB): laço parado em $4000, partida direta ($BFFD = $FF)
std::vector<uint8_t> idleCart() {
  std::vector<uint8_t> c(0x8000, 0);
  c[0] = 0x4C; c[1] = 0x00; c[2] = 0x40;  // JMP $4000
  c[0x7FFD] = 0xFF; c[0x7FFE] = 0x00; c[0x7FFF] = 0x40;
  return c;
}

std::unique_ptr<A5200> machine(const std::vector<uint8_t>& rom, const std::string& name = "t.a52") {
  Cart c;
  std::string err;
  if (!c.load(rom, name, err)) { std::printf("FALHA cartucho: %s\n", err.c_str()); std::exit(1); }
  return std::make_unique<A5200>(std::move(c), BIOS);
}

// ponto da tela (linha da TV, ponto de cor; meio = 0 ou 1)
uint32_t px(const A5200& m, int line, int cc, int half = 0) { return m.fb[size_t(line - 8) * A5200::W + (cc - 32) * 2 + half]; }

// ---------------------------------------------------------------- desenhista de referência do playfield
// Lê a lista de exibição e desenha a tela inteira sem jogadores, com as regras do ANTIC escritas de novo
// (por fluxo de pontos: bits por ponto, pontos de cor por ponto), sem olhar o núcleo.
struct Regs { uint8_t dmactl, chactl, chbase, hscrol, vscrol, colpf[4], colbk; uint16_t dlist; };

std::vector<uint32_t> reference(const A5200& m, const Regs& r) {
  std::vector<uint32_t> out(size_t(A5200::W) * A5200::H, A5200::color(r.colbk & 0xFE));
  auto mem = [&](uint16_t a) { return m.peek(a); };
  static const int LINES[16] = {0, 0, 8, 10, 8, 16, 8, 16, 8, 4, 4, 2, 1, 2, 1, 1};
  // bits por ponto e pontos de cor por ponto, por modo (texto: o ponto vem do desenho do caractere)
  static const int BPP[16] = {0, 0, 2, 2, 2, 2, 1, 1, 2, 1, 2, 1, 1, 2, 2, 2};
  static const int CCP[16] = {0, 0, 1, 1, 1, 1, 1, 1, 4, 2, 2, 1, 1, 1, 1, 1};
  const int width = r.dmactl & 3;
  const int left[4] = {0, 64, 48, 32}, size[4] = {0, 128, 160, 192};
  uint16_t dl = r.dlist, ms = 0;
  auto next = [&] { const uint8_t b = mem(dl); dl = static_cast<uint16_t>((dl & 0xFC00) | ((dl + 1) & 0x3FF)); return b; };
  bool prevVs = false;
  int y = 8;
  while (y < 248) {
    const uint8_t ir = next();
    const int mode = ir & 15;
    // brancos e saltos depois de uma região rolada também fecham a região: VSCROL + 1 linhas
    if (mode == 0) { y += (prevVs ? r.vscrol : (ir >> 4) & 7) + 1; prevVs = false; continue; }
    if (mode == 1) {
      const uint8_t lo = next(), hi = next();
      dl = static_cast<uint16_t>(lo | hi << 8);
      if (ir & 0x40) break;
      y += prevVs ? r.vscrol + 1 : 1;
      prevVs = false;
      continue;
    }
    if (ir & 0x40) { const uint8_t lo = next(), hi = next(); ms = static_cast<uint16_t>(lo | hi << 8); }
    const bool vs = ir & 0x20, hs = ir & 0x10;
    const int fw = hs ? std::min(width + 1, 3) : width;
    const int bytes = size[fw] * BPP[mode] / (8 * CCP[mode]) / ((mode >= 2 && mode <= 7) ? 1 : 1);
    // em texto um byte é um caractere de 8 bits de desenho: 4 pontos de 2 bits (2–5) ou 8 de 1 bit (6, 7)
    const int nbytes = (mode >= 2 && mode <= 5) ? size[fw] / 4 : (mode == 6 || mode == 7) ? size[fw] / 8 : bytes;
    std::vector<int> rows;
    int row = (vs && !prevVs) ? r.vscrol : 0;
    const int last = (!vs && prevVs) ? r.vscrol : LINES[mode] - 1;
    for (;;) { rows.push_back(row); if (row == last) break; row = (row + 1) & 15; }
    for (int rr : rows) {
      if (y >= 248) break;
      if (width) {
        std::vector<int> code(240, 0), half(240, 0);  // código de cor e meios pontos acesos (alta resolução)
        int x = left[fw] + (hs ? r.hscrol : 0);
        for (int i = 0; i < nbytes; i++) {
          const uint8_t b = mem(static_cast<uint16_t>((ms & 0xF000) | ((ms + i) & 0xFFF)));
          uint8_t d = b;
          int colorSet = 0;  // 6/7: cor do caractere
          if (mode <= 7) {
            const bool big = mode == 5 || mode == 7;
            int line = big ? (rr >> 1) & 7 : rr;
            const uint16_t base = static_cast<uint16_t>((r.chbase & (mode <= 5 ? 0xFC : 0xFE)) * 256);
            const int ch = mode <= 5 ? (b & 0x7F) : (b & 0x3F);
            auto font = [&](int l) {
              if (r.chactl & 4) l = 7 - l;
              return mem(static_cast<uint16_t>(base + ch * 8 + (l & 7)));
            };
            if (mode == 3) {
              if ((b & 0x60) == 0x60) d = line < 2 || line > 9 ? 0 : line < 8 ? font(line) : font(line - 8);
              else d = line < 8 ? font(line) : 0;
            } else {
              d = font(line & 7);
            }
            if (mode <= 3 && (b & 0x80)) { if (r.chactl & 1) d = 0; if (r.chactl & 2) d = static_cast<uint8_t>(~d); }
            colorSet = b >> 6;
          }
          const int bpp = BPP[mode], n = 8 / bpp;
          for (int k = 0; k < n; k++) {
            const int v = (d >> (8 - bpp * (k + 1))) & ((1 << bpp) - 1);
            for (int j = 0; j < CCP[mode]; j++, x++) {
              if (x < 0 || x >= 240) continue;
              if (mode == 2 || mode == 3 || mode == 0xF) { code[x] = 9; half[x] = v; continue; }
              int c;
              if (mode == 6 || mode == 7) c = v ? 1 + colorSet : 0;
              else if (bpp == 1) c = v;  // 9, B, C: PF0
              else if ((mode == 4 || mode == 5) && v == 3) c = (b & 0x80) ? 4 : 3;
              else c = v;
              code[x] = c;
            }
          }
        }
        for (int cc = 32; cc < 224; cc++) {
          if (cc < left[width] || cc >= left[width] + size[width]) continue;
          uint32_t* o = &out[size_t(y - 8) * A5200::W + (cc - 32) * 2];
          const int c = code[cc];
          if (c == 9) {
            const int base = r.colpf[2], lit = (r.colpf[2] & 0xF0) | (r.colpf[1] & 0x0E);
            o[0] = A5200::color(((half[cc] & 2) ? lit : base) & 0xFE);
            o[1] = A5200::color(((half[cc] & 1) ? lit : base) & 0xFE);
          } else {
            const int col = c == 0 ? r.colbk : r.colpf[c - 1];
            o[0] = o[1] = A5200::color(col & 0xFE);
          }
        }
      }
      y++;
    }
    if (width) ms = static_cast<uint16_t>((ms & 0xF000) | ((ms + nbytes) & 0xFFF));
    prevVs = vs;
  }
  return out;
}

void setRegs(A5200& m, const Regs& r) {
  m.poke(0xD400, r.dmactl); m.poke(0xD401, r.chactl); m.poke(0xD409, r.chbase);
  m.poke(0xD404, r.hscrol); m.poke(0xD405, r.vscrol);
  for (int i = 0; i < 4; i++) m.poke(static_cast<uint16_t>(0xC016 + i), r.colpf[i]);
  m.poke(0xC01A, r.colbk);
  m.poke(0xD402, r.dlist & 0xFF); m.poke(0xD403, r.dlist >> 8);
}

// ---------------------------------------------------------------- seções
void testDetect(const std::string& root) {
  std::printf("== reconhecimento\n");
  const auto cart = readAll(root + "/a5200/cart.a52");
  check(a5200::isA5200Rom(cart, "cart.a52"), ".a52 reconhecido");
  check(a5200::isA5200Rom(cart, "cart.bin"), ".bin de 32 KB com vetor de 5200 reconhecido");
  auto core = createCore(cart, "Cart.BIN", nullptr);
  check(core && core->system() == "a5200", "createCore escolhe o 5200 para o .bin");
  check(core && core->title() == "TESTE INDIGO", "título lido de $BFE8: '" + (core ? core->title() : std::string()) + "'");
  // .bin de Atari 2600 (vetores de reset e de IRQ em $Fxxx) não é 5200
  std::vector<uint8_t> a26(4096, 0xEA);
  a26[0xFFC] = 0x00; a26[0xFFD] = 0xF0; a26[0xFFE] = 0x00; a26[0xFFF] = 0xF0;
  check(!a5200::isA5200Rom(a26, "x.bin"), ".bin de 2600 de 4 KB recusado");
  a26[0xFFD] = 0xB0; a26[0xFFF] = 0xB0;  // 2600 montado em $B000
  check(!a5200::isA5200Rom(a26, "x.bin"), ".bin de 2600 montado em $B000 recusado");
  // .car com tipo de 5200 (4 = 32 KB) e com tipo de computador (1 = 8 KB) recusado
  std::vector<uint8_t> car = {'C', 'A', 'R', 'T', 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0};
  car.insert(car.end(), cart.begin(), cart.end());
  check(a5200::isA5200Rom(car, "x.car"), ".car do tipo 4 reconhecido");
  car[7] = 1;
  check(!a5200::isA5200Rom(car, "x.car"), ".car de computador recusado");
  check(a5200::isA5200Bios(BIOS, "5200.rom") && !a5200::isA5200Bios(BIOS, "outro.rom"), "BIOS reconhecida pelo nome e tamanho");
  // nenhum .bin/.rom das pastas de teste dos outros consoles vira 5200
  int seen = 0, stolen = 0;
  for (auto& e : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied)) {
    if (!e.is_regular_file()) continue;
    const std::string p = e.path().string();
    if (p.find("/a5200/") != std::string::npos) continue;
    std::string ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".bin" && ext != ".a26" && ext != ".rom") continue;
    if (e.file_size() > 0xA000) continue;
    seen++;
    std::string n = e.path().filename().string();
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    if (a5200::isA5200Rom(readAll(p), n)) { stolen++; std::printf("  roubado: %s\n", p.c_str()); }
  }
  info(std::to_string(seen) + " arquivos .bin/.a26/.rom de até 40 KB dos outros consoles conferidos");
  check(stolen == 0, "nenhum arquivo de outro console reconhecido como 5200");
}

void testCarts() {
  std::printf("== cartuchos\n");
  auto img = [](size_t n) { std::vector<uint8_t> v(n); for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>((i >> 12) * 16 + 1); return v; };
  auto bank = [](const A5200& m, uint16_t a) { return m.peek(a); };
  {
    auto m = machine(img(0x1000));
    check(m->cart.kind == Cart::K4 && bank(*m, 0x8000) == 1 && bank(*m, 0xB000) == 1 && bank(*m, 0x4000) == 0xFF, "4 KB espelhado em $8000-$BFFF");
  }
  {
    auto v = img(0x2000);
    auto m = machine(v);
    check(m->cart.kind == Cart::K8 && bank(*m, 0x8000) == 1 && bank(*m, 0x9000) == 0x11 && bank(*m, 0xA000) == 1 && bank(*m, 0xB000) == 0x11, "8 KB em $8000 e $A000");
  }
  {
    auto v = img(0x4000);
    auto m = machine(v);
    check(m->cart.kind == Cart::NS16 && bank(*m, 0x8000) == 1 && bank(*m, 0xB000) == 0x31 && bank(*m, 0x4000) == 0xFF, "16 KB de um chip em $8000");
    for (size_t i = 0; i < 0x2000; i += 64) { v[i] = 0x20; v[i + 1] = 0x00; v[i + 2] = 0x45; }  // JSR $45xx
    auto m2 = machine(v);
    check(m2->cart.kind == Cart::EE16 && bank(*m2, 0x4000 + 3) == 1 && bank(*m2, 0x6000 + 3) == 1 && bank(*m2, 0x8000 + 3) == 0x21 && bank(*m2, 0xA000 + 3) == 0x21,
          "16 KB de dois chips (código chama $4xxx): $4000=$6000, $8000=$A000");
  }
  {
    auto m = machine(img(0x8000));
    check(bank(*m, 0x4000) == 1 && bank(*m, 0xBFFF) == 0x71, "32 KB em $4000-$BFFF");
  }
  {
    auto v = img(0xA000);
    auto m = machine(v);
    check(m->cart.kind == Cart::BB40 && bank(*m, 0x4000) == 1 && bank(*m, 0x5000) == 0x41 && bank(*m, 0x8000) == 0x81 && bank(*m, 0xA000) == 0x81, "Bounty Bob: bancos 0 e 4 ligados");
    m->read(0x4FF8);
    m->write(0x5FF9, 0);
    check(bank(*m, 0x4000) == 0x21 && bank(*m, 0x5000) == 0x71, "Bounty Bob: $4FF8 e $5FF9 trocam os bancos");
  }
  {
    auto v = img(0x20000);  // 128 KB: 4 bancos de 32 KB
    auto m = machine(v, "s.a52");
    check(m->cart.kind == Cart::SUPER, "super cartucho de 128 KB");
    m->read(0xBFD4);  // bits baixos = 1
    check(bank(*m, 0x4000) == static_cast<uint8_t>(8 * 16 + 1), "super cartucho: $BFD4 liga o banco 1");
    m->read(0xBFF0);
    check(bank(*m, 0x4000) == static_cast<uint8_t>(24 * 16 + 1), "super cartucho: $BFFx liga o último banco");
    // título: vem do último banco (onde a BIOS o lê), não do banco 0; o cartucho volta ao banco 0
    const std::string t = "ULTIMO BANCO", t0 = "BANCO ZERO";
    for (size_t i = 0; i < 20; i++) {
      v[v.size() - 0x18 + i] = static_cast<uint8_t>((i < t.size() ? t[i] : ' ') - 0x20);
      v[0x8000 - 0x18 + i] = static_cast<uint8_t>((i < t0.size() ? t0[i] : ' ') - 0x20);
    }
    auto m2 = machine(v, "s.a52");
    check(m2->cart.title == t && bank(*m2, 0x4000) == 1, "super cartucho: título do último banco: '" + m2->cart.title + "'");
  }
}

void testBoot(const std::string& root) {
  std::printf("== partida pela BIOS de teste, NMI, DLI, WSYNC, entradas e som\n");
  auto m = machine(readAll(root + "/a5200/cart.a52"));
  for (int i = 0; i < 15; i++) m->runFrame();
  check(m->peek(0x3000) == 0xA5, "o cartucho começou pelo vetor $BFFE");
  {
    auto rom = readAll(root + "/a5200/cart.a52");
    rom[0x7FFD] = 0xFF;
    auto d = machine(rom);
    for (int i = 0; i < 12; i++) d->runFrame();
    check(d->peek(0x3000) == 0xA5 && d->peek(0x3001) >= 10 && d->peek(0x3007) >= 20, "cartucho de diagnóstico ($FF em $BFFD): liga direto e trata o próprio VBI");
  }
  const uint8_t f0 = m->peek(0x3001);
  for (int i = 0; i < 10; i++) m->runFrame();
  check(static_cast<uint8_t>(m->peek(0x3001) - f0) == 10, "VBI pelo vetor VVBLKD: um por quadro");
  check(m->peek(0x3002) == 125, "VCOUNT no VBI depois do VBI imediato da BIOS = 125 (linhas 250-251): " + std::to_string(m->peek(0x3002)));
  check(m->peek(0x300D) == 19, "VCOUNT no primeiro DLI = 19 (linha 39): " + std::to_string(m->peek(0x300D)));
  check(m->peek(0x3007) >= 20, "DLI pelo vetor VDSLST: dois por quadro");
  // cores: linhas 8–39 com $46; o DLI com WSYNC troca para $18 a partir da linha 40; o DLI sem WSYNC (linha 48)
  // troca para $86 no meio da linha
  check(px(*m, 20, 40) == A5200::color(0x46) && px(*m, 39, 40) == A5200::color(0x46), "fundo antes do DLI");
  check(px(*m, 40, 40) == A5200::color(0x18) && px(*m, 47, 210) == A5200::color(0x18), "DLI com WSYNC: cor nova da linha 40 em diante");
  check(px(*m, 48, 40) == A5200::color(0x18) && px(*m, 48, 215) == A5200::color(0x86), "DLI sem WSYNC: cor trocada no meio da linha 48");
  check(px(*m, 49, 40) == A5200::color(0x86), "cor nova na linha seguinte");
  // modo F com listras $F0: 2 pontos de cor acesos (PF1 com a matiz de PF2), 2 apagados (PF2)
  check(px(*m, 41, 48) == A5200::color(0x9E) && px(*m, 41, 49, 1) == A5200::color(0x9E) && px(*m, 41, 50) == A5200::color(0x94) &&
        px(*m, 41, 207, 1) == A5200::color(0x94) && px(*m, 41, 47) == A5200::color(0x18), "modo F: alta resolução, largura normal");
  // modo 2: "A" na primeira coluna; o quarto caractere ($A1) é invertido (CHACTL = 2)
  check(px(*m, 49, 49, 0) == A5200::color(0x94) && px(*m, 49, 49, 1) == A5200::color(0x9E) && px(*m, 49, 50, 0) == A5200::color(0x9E),
        "modo 2: linha 0 do A ($18)");
  check(px(*m, 49, 61, 1) == A5200::color(0x94) && px(*m, 49, 60, 0) == A5200::color(0x9E), "modo 2: caractere invertido");
  // ciclos roubados: linha em branco = refresh (9); primeira linha de modo F com LMS = 1 + 2 + 40 + 9
  check(m->stolenLine[20] == 9, "linha em branco: 9 ciclos de refresh (" + std::to_string(m->stolenLine[20]) + ")");
  check(m->stolenLine[41] == 52, "modo F com LMS: 52 ciclos (" + std::to_string(m->stolenLine[41]) + ")");
  // 1 + 2 + 40 nomes + 40 desenhos + 1 refresh: os pedidos de refresh caem nos ciclos do playfield e só um
  // espera o ciclo livre 98 (mapa de ciclos medido no aparelho; até a 2.4 eram 9 refresh = 92)
  check(m->stolenLine[49] == 84, "modo 2, primeira linha com LMS: 84 ciclos (" + std::to_string(m->stolenLine[49]) + ")");
  check(m->stolenLine[50] == 49, "modo 2, outras linhas: 49 ciclos (" + std::to_string(m->stolenLine[50]) + ")");
  check(m->stolenLine[250] == 9, "retraço vertical: só refresh");

  // potenciômetro (lido no VBI, depois do POTGO do quadro anterior), ALLPOT, gatilho, SKSTAT
  {
    auto rom = readAll(root + "/a5200/cart.a52");
    rom[0x7FFD] = 0xFF;  // diagnóstico: o próprio cartucho lê POT0 e ALLPOT antes do POTGO
    auto d = machine(rom);
    for (uint8_t pos : {uint8_t(6), uint8_t(114), uint8_t(220)}) {
      m->setInput(pos, 114, false, false, -1);
      d->setInput(pos, 114, false, false, -1);
      for (int i = 0; i < 3; i++) { m->runFrame(); d->runFrame(); }
      check(m->peek(0x3003) == pos && d->peek(0x3003) == pos, "POT0 = " + std::to_string(pos) + " (pela BIOS " + std::to_string(m->peek(0x3003)) +
            ", direto " + std::to_string(d->peek(0x3003)) + ")");
      check(d->peek(0x3004) == 0, "ALLPOT = 0 depois de um quadro (contagem terminada)");
    }
    check(d->peek(0x3002) == 124, "VCOUNT no VBI = 124 (linha 248): " + std::to_string(d->peek(0x3002)));
  }
  check(m->peek(0x3005) == 1, "TRIG0 solto = 1");
  m->setInput(114, 114, true, false, -1);
  m->runFrame(); m->runFrame();
  check(m->peek(0x3005) == 0, "TRIG0 apertado = 0");
  // teclado: Start gera IRQ de tecla (uma por quadro enquanto apertada, com o bit 5 alternando)
  const uint8_t k0 = m->peek(0x300A);
  m->setInput(114, 114, false, false, a5200::KP_START);
  for (int i = 0; i < 4; i++) m->runFrame();
  check(static_cast<uint8_t>(m->peek(0x300A) - k0) == 4, "IRQ de tecla: uma por quadro");
  check((m->peek(0x3009) & 0x1F) == (a5200::KP_START & 0x1F), "KBCODE da tecla Start");
  check((m->peek(0x3006) & 0x04) == 0, "SKSTAT: tecla apertada (bit 2 = 0)");
  // teclado completo pelo app: X + direcional/botões e o teclado numérico do PC (bits KEY_KP*)
  {
    struct { uint32_t keys; int code; const char* nome; } tab[] = {
        {KEY_X | KEY_UP | KEY_LEFT, a5200::KP_1, "X+↖ = 1"}, {KEY_X | KEY_UP, a5200::KP_2, "X+↑ = 2"},
        {KEY_X | KEY_UP | KEY_RIGHT, a5200::KP_3, "X+↗ = 3"}, {KEY_X | KEY_LEFT, a5200::KP_4, "X+← = 4"},
        {KEY_X | KEY_A, a5200::KP_5, "X+A = 5"}, {KEY_X | KEY_RIGHT, a5200::KP_6, "X+→ = 6"},
        {KEY_X | KEY_DOWN | KEY_LEFT, a5200::KP_7, "X+↙ = 7"}, {KEY_X | KEY_DOWN, a5200::KP_8, "X+↓ = 8"},
        {KEY_X | KEY_DOWN | KEY_RIGHT, a5200::KP_9, "X+↘ = 9"}, {KEY_X | KEY_B, a5200::KP_0, "X+B = 0"},
        {KEY_X | KEY_START, a5200::KP_RESET, "X+Start = Reset"}, {KEY_X | KEY_L, a5200::KP_STAR, "X+L = *"},
        {KEY_X | KEY_R, a5200::KP_HASH, "X+R = #"}, {KEY_KP0 << 7, a5200::KP_7, "teclado numérico 7"},
        {KEY_KP0, a5200::KP_0, "teclado numérico 0"}, {KEY_KPHASH, a5200::KP_HASH, "teclado numérico #"},
        {KEY_KPSTAR | KEY_START, a5200::KP_STAR, "teclado numérico * antes do Start"}};
    int certos = 0;
    std::string erro;
    for (const auto& e : tab) {
      m->setKeys(e.keys);
      m->runFrame(); m->runFrame();
      if ((m->peek(0x3009) & 0x1F) == (e.code & 0x1F)) certos++;
      else if (erro.empty()) erro = std::string(" (falhou: ") + e.nome + ")";
    }
    check(certos == int(sizeof tab / sizeof tab[0]), "teclas 0–9, *, # e Reset pelo app: " + std::to_string(certos) + erro);
    m->setKeys(KEY_X | KEY_A);
    m->runFrame(); m->runFrame();
    check(m->peek(0x3005) == 1, "com X apertado o gatilho A não dispara (vira a tecla 5)");
    m->setKeys(KEY_X);
    m->runFrame(); m->runFrame();
    check(m->peek(0x3006) & 0x04, "X sozinho não aperta tecla nenhuma");
  }
  m->setInput(114, 114, false, false, -1);
  m->runFrame(); m->runFrame();
  const uint8_t k1 = m->peek(0x300A);
  m->runFrame();
  check(m->peek(0x300A) == k1 && (m->peek(0x3006) & 0x04), "tecla solta: sem IRQ");
  // botão de baixo: IRQ de Break uma vez por aperto, SKSTAT bit 3
  const uint8_t b0 = m->peek(0x300B);
  m->setInput(114, 114, false, true, -1);
  for (int i = 0; i < 15; i++) m->runFrame();
  check(static_cast<uint8_t>(m->peek(0x300B) - b0) == 1, "botão de baixo: uma IRQ de Break");
  check((m->peek(0x3006) & 0x08) == 0, "SKSTAT: botão de baixo apertado (bit 3 = 0)");
  m->setInput(114, 114, false, false, -1);
  m->runFrame();

  // som: tom puro no canal 1, AUDF1 = $1F (relógio de 64 kHz): 63920,4 / (2 x 32) = 998,8 Hz
  m->setSampleRate(48000);
  std::vector<float> l(48000), r(48000);
  size_t got = 0;
  while (got < 48000) {
    m->runFrame();
    const size_t n = std::min(m->audioAvailable(), size_t(48000) - got);
    m->pullAudio(l.data() + got, r.data() + got, n);
    got += n;
  }
  int cross = 0;
  float peak = 0;
  for (size_t i = 1000; i < 48000; i++) { if ((l[i - 1] < 0) != (l[i] < 0)) cross++; peak = std::max(peak, std::fabs(l[i])); }
  const double hz = cross / 2.0 / (47000 / 48000.0);
  check(std::fabs(hz - 998.8) < 6, "tom de " + std::to_string(hz) + " Hz (esperado 998,8)");
  check(peak > 0.1, "som com volume");

  // save state: o mesmo futuro depois de carregar
  const auto st = m->saveState();
  m->setInput(60, 200, true, false, a5200::KP_5);
  for (int i = 0; i < 30; i++) m->runFrame();
  const uint64_t h1 = fnv(m->fb.data(), m->fb.size() * 4);
  const auto st1 = m->saveState();
  m->loadState(st);
  for (int i = 0; i < 30; i++) m->runFrame();
  check(fnv(m->fb.data(), m->fb.size() * 4) == h1 && m->saveState() == st1, "save state: mesma tela e mesmo estado 30 quadros depois");
  // estados estragados: recusados sem estragar a máquina
  auto bad = st;
  bad.resize(bad.size() / 2);
  bool threw = false;
  try { m->loadState(bad); } catch (const std::exception&) { threw = true; }
  check(threw && m->saveState() == st1, "estado truncado recusado e a máquina continua igual");
  std::mt19937 rng(5);
  for (int k = 0; k < 50; k++) {
    auto junk = st;
    for (int j = 0; j < 40; j++) junk[4 + rng() % (junk.size() - 4)] = static_cast<uint8_t>(rng());
    try { m->loadState(junk); for (int i = 0; i < 3; i++) m->runFrame(); } catch (const std::exception&) {}
  }
  m->loadState(st);
  m->runFrame();
  check(m->peek(0x3000) == 0xA5, "50 estados com bytes trocados carregados e rodados sem quebrar");
}

void testPokeyIrq() {
  std::printf("== POKEY: IRQ do temporizador 1, BIOS ausente\n");
  auto rom = idleCart();
  rom[0] = 0x58; rom[1] = 0x4C; rom[2] = 0x01; rom[3] = 0x40;  // CLI; JMP $4001
  auto m = machine(rom);
  // tratador na RAM: PHA; contador de 16 bits em $0680; IRQEN = 0 e 1 (reconhece); PLA; RTI
  const uint8_t h[] = {0x48, 0xEE, 0x80, 0x06, 0xD0, 0x03, 0xEE, 0x81, 0x06, 0xA9, 0x00, 0x8D, 0x0E, 0xE8, 0xA9, 0x01, 0x8D, 0x0E, 0xE8, 0x68, 0x40};
  for (size_t i = 0; i < sizeof h; i++) m->poke(static_cast<uint16_t>(0x0600 + i), h[i]);
  m->poke(0x0200, 0x00); m->poke(0x0201, 0x06);
  m->runFrame();
  m->poke(0xE80F, 3);     // SKCTL
  m->poke(0xE808, 0);     // AUDCTL: 64 kHz
  m->poke(0xE800, 99);    // AUDF1: 100 x 28 = 2800 ciclos
  m->poke(0xE80E, 1);     // IRQEN: temporizador 1
  m->poke(0xE809, 0);     // STIMER
  m->runFrame();
  auto count = [&] { return m->peek(0x0680) | m->peek(0x0681) << 8; };
  const int a = count();
  for (int i = 0; i < 60; i++) m->runFrame();
  const int n = count() - a;
  const double want = 60.0 * 114 * 262 / 2800;
  check(std::abs(n - want) <= 2, "IRQ do temporizador 1: " + std::to_string(n) + " em 60 quadros (esperado " + std::to_string(want) + ")");
  // sem a BIOS do usuário o núcleo não abre, com a mensagem pedindo o arquivo
  const auto bios = firmware("a5200");
  setFirmware("a5200", {});
  std::string msg;
  try { createCore(idleCart(), "x.a52", nullptr); } catch (const std::exception& e) { msg = e.what(); }
  check(msg.find("BIOS") != std::string::npos && msg.find("5200") != std::string::npos, "sem BIOS: \"" + msg + "\"");
  setFirmware("a5200", bios);
}

void testFuzz() {
  std::printf("== cartuchos aleatórios (código e listas de exibição quaisquer)\n");
  std::mt19937 rng(5200);
  int ran = 0;
  for (int k = 0; k < 40; k++) {
    std::vector<uint8_t> rom(k % 4 == 0 ? 0x4000 : k % 4 == 1 ? 0xA000 : 0x8000);
    for (auto& b : rom) b = static_cast<uint8_t>(rng());
    rom[rom.size() - 3] = static_cast<uint8_t>(rng() % 2 ? 0xFF : 0x40);
    rom[rom.size() - 1] = static_cast<uint8_t>(0x80 + rng() % 0x40);
    auto m = machine(rom, "f.a52");
    for (int f = 0; f < 20; f++) {
      m->setInput(static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()), rng() % 2, rng() % 2, rng() % 3 ? -1 : a5200::KP_START);
      m->runFrame();
    }
    const auto st = m->saveState();
    m->loadState(st);
    ran += m->frameCount >= 20;
  }
  check(ran == 40, std::to_string(ran) + " de 40 cartuchos aleatórios rodaram 20 quadros sem travar o núcleo");
}

void testAntic() {
  std::printf("== ANTIC: telas sorteadas contra o desenhista de referência\n");
  auto m = machine(idleCart());
  for (int i = 0; i < 15; i++) m->runFrame();
  std::mt19937 rng(1982);
  int bad = 0, total = 0;
  for (int scene = 0; scene < 400; scene++) {
    for (int a = 0x1000; a < 0x3000; a++) m->poke(static_cast<uint16_t>(a), static_cast<uint8_t>(rng()));
    Regs r{};
    r.dmactl = static_cast<uint8_t>(0x20 | (1 + rng() % 3));
    r.chactl = static_cast<uint8_t>(rng() % 8);
    r.chbase = static_cast<uint8_t>(0x20 + (rng() % 2) * 4);
    r.hscrol = static_cast<uint8_t>(rng() % 16);
    r.vscrol = static_cast<uint8_t>(rng() % 16);
    for (auto& c : r.colpf) c = static_cast<uint8_t>(rng());
    r.colbk = static_cast<uint8_t>(rng());
    r.dlist = 0x0800;
    // lista: modos sorteados (às vezes só um modo, para testar sequências), LMS em parte deles, rolagens, brancos
    std::vector<uint8_t> dl;
    const int only = (scene % 3 == 0) ? int(2 + rng() % 14) : 0;
    int lines = 0;
    bool hadLms = false;
    while (lines < 260 && dl.size() < 900) {
      if (rng() % 10 == 0) { const int n = int(rng() % 8); dl.push_back(static_cast<uint8_t>(n << 4)); lines += n + 1; continue; }
      const int mode = only ? only : int(2 + rng() % 14);
      uint8_t ir = static_cast<uint8_t>(mode);
      if (rng() % 3 == 0) ir |= 0x10;
      if (rng() % 3 == 0) ir |= 0x20;
      const bool lms = rng() % 2 == 0 || !hadLms;  // a primeira linha de modo sempre com endereço
      hadLms = true;
      if (lms) ir |= 0x40;
      dl.push_back(ir);
      if (lms) { const uint16_t a = static_cast<uint16_t>(0x1000 + rng() % 0x1000); dl.push_back(a & 0xFF); dl.push_back(a >> 8); }
      static const int L[16] = {0, 0, 8, 10, 8, 16, 8, 16, 8, 4, 4, 2, 1, 2, 1, 1};
      lines += L[mode];
    }
    dl.push_back(0x41); dl.push_back(0x00); dl.push_back(0x08);
    for (size_t i = 0; i < dl.size(); i++) m->poke(static_cast<uint16_t>(0x0800 + i), dl[i]);
    setRegs(*m, r);
    m->runFrame();
    m->poke(0xD402, 0x00); m->poke(0xD403, 0x08);  // a lista passa de 240 linhas: recarregada como a BIOS faz no VBI
    m->runFrame();
    const auto ref = reference(*m, r);
    total++;
    if (ref != m->fb) {
      bad++;
      if (bad == 1 && std::getenv("A5200_PNG")) {
        writePng(std::string(std::getenv("A5200_PNG")) + "/ref.png", ref.data(), A5200::W, A5200::H);
        writePng(std::string(std::getenv("A5200_PNG")) + "/nucleo.png", m->fb.data(), A5200::W, A5200::H);
        for (int y = 0; y < 40; y++) {
          int match = -1;
          for (int y2 = 0; y2 < A5200::H; y2++)
            if (std::equal(m->fb.begin() + y * A5200::W, m->fb.begin() + (y + 1) * A5200::W, ref.begin() + y2 * A5200::W)) { match = y2; break; }
          std::printf("  linha %d do núcleo = linha %d da referência\n", y + 8, match + 8);
        }
        {
          const size_t i0 = std::mismatch(ref.begin(), ref.end(), m->fb.begin()).first - ref.begin();
          const size_t y0 = i0 / A5200::W;
          std::printf("  linha %zu ref:", y0 + 8);
          for (int x = 0; x < 48; x += 2) std::printf(" %06X", ref[y0 * A5200::W + x] & 0xFFFFFF);
          std::printf("\n  linha %zu núc:", y0 + 8);
          for (int x = 0; x < 48; x += 2) std::printf(" %06X", m->fb[y0 * A5200::W + x] & 0xFFFFFF);
          std::printf("\n  cores: bk %06X pf0 %06X pf1 %06X pf2 %06X pf3 %06X\n", A5200::color(r.colbk & 0xFE) & 0xFFFFFF,
                      A5200::color(r.colpf[0] & 0xFE) & 0xFFFFFF, A5200::color(r.colpf[1] & 0xFE) & 0xFFFFFF,
                      A5200::color(r.colpf[2] & 0xFE) & 0xFFFFFF, A5200::color(r.colpf[3] & 0xFE) & 0xFFFFFF);
          std::printf("  dados em $1DD3:");
          for (int k = 0; k < 6; k++) std::printf(" %02X", m->peek(static_cast<uint16_t>(0x1DD3 + k)));
          std::printf("\n");
        }
        std::printf("  lista:");
        for (size_t i = 0; i < 40 && i < dl.size(); i++) std::printf(" %02X", dl[i]);
        std::printf("\n");
      }
      if (bad <= 3) {
        for (size_t i = 0; i < ref.size(); i++)
          if (ref[i] != m->fb[i]) {
            std::printf("  cena %d: primeira diferença na linha %zu, ponto de cor %zu (dmactl %02X chactl %d hs %d vs %d; "
                        "ref %08X núcleo %08X; lista %02X %02X %02X %02X)\n", scene,
                        i / A5200::W + 8, (i % A5200::W) / 2 + 32, r.dmactl, r.chactl, r.hscrol, r.vscrol, ref[i], m->fb[i],
                        dl[0], dl[1], dl[2], dl[3]);
            break;
          }
      }
    }
  }
  check(bad == 0, std::to_string(total - bad) + " de " + std::to_string(total) + " telas iguais à referência");
}

// ---------------------------------------------------------------- ANTIC ciclo a ciclo
// mapa dos ciclos roubados de uma linha ("." = CPU, "#" = ANTIC) e o mapa esperado a partir de uma lista
std::string stealStr(const A5200& m, int line) {
  std::string s;
  for (int c = 0; c < A5200::CYCLES; c++) s += m.stolenAt[line][c] ? '#' : '.';
  return s;
}
std::string mapOf(const std::vector<int>& cycles) {
  std::string s(A5200::CYCLES, '.');
  for (int c : cycles) s[size_t(c)] = '#';
  return s;
}
std::vector<int> range(int from, int to, int step) {
  std::vector<int> v;
  for (int c = from; c <= to; c += step) v.push_back(c);
  return v;
}
std::vector<int> join(std::initializer_list<std::vector<int>> parts) {
  std::vector<int> v;
  for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
  return v;
}

// máquina parada (JMP $4000) com lista em $0800, tela em $1000, fonte A em $2000 (caractere 1 aceso, 0 apagado)
// e fonte B em $2400 (tudo apagado); cores: PF2 = $94 (apagado), PF1 = $0E (aceso em $9E), fundo $46
std::unique_ptr<A5200> cycleMachine(const std::vector<uint8_t>& dl, uint8_t dmactl, uint8_t name,
                                    const std::vector<uint8_t>& cart = idleCart()) {
  auto m = machine(cart);
  for (int i = 0; i < 15; i++) m->runFrame();
  for (size_t i = 0; i < dl.size(); i++) m->poke(static_cast<uint16_t>(0x0800 + i), dl[i]);
  for (int a = 0x1000; a < 0x1800; a++) m->poke(static_cast<uint16_t>(a), name);
  for (int a = 0x2000; a < 0x2800; a++) m->poke(static_cast<uint16_t>(a), 0);
  for (int r = 0; r < 8; r++) m->poke(static_cast<uint16_t>(0x2008 + r), 0xFF);
  m->poke(0xC018, 0x94); m->poke(0xC017, 0x0E); m->poke(0xC01A, 0x46);
  m->poke(0xD409, 0x20); m->poke(0xD401, 0); m->poke(0xD404, 0); m->poke(0xD405, 0);
  m->poke(0xD402, 0x00); m->poke(0xD403, 0x08); m->poke(0xD400, dmactl);
  m->runFrame();
  m->runFrame();
  return m;
}
// leva o relógio da máquina (no começo de um quadro) até a linha e o ciclo pedidos
void goTo(A5200& m, int line, int cyc) { m.runCycles(line * A5200::CYCLES + cyc - (m.line * A5200::CYCLES + m.cyc)); }

void testCycles() {
  std::printf("== ANTIC ciclo a ciclo: posição da DMA, registradores trocados no meio da linha, WSYNC\n");
  const uint32_t ON = A5200::color(0x9E), OFF = A5200::color(0x94), BK = A5200::color(0x46);
  // modo 2, largura normal: nomes nos ciclos pares 18–96, desenhos nos ímpares 21–99, refresh só no 98
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x42, 0x00, 0x10, 0x02, 0x41, 0x00, 0x08}, 0x22, 1);
    check(stealStr(*m, 20) == mapOf(range(25, 57, 4)), "linha em branco: refresh nos ciclos 25, 29, ... 57\n    " + stealStr(*m, 20));
    check(stealStr(*m, 32) == mapOf(join({{1, 6, 7, 98}, range(18, 96, 2), range(21, 99, 2)})),
          "modo 2 com LMS: instrução 1, endereço 6-7, nomes 18-96, desenhos 21-99, refresh 98\n    " + stealStr(*m, 32));
    check(stealStr(*m, 33) == mapOf(join({range(21, 99, 2), range(26, 58, 4)})),
          "modo 2, linha seguinte: desenhos 21-99 e refresh adiado para 26, 30, ... 58\n    " + stealStr(*m, 33));
    check(stealStr(*m, 40) == mapOf(join({{1, 98}, range(18, 96, 2), range(21, 99, 2)})), "modo 2 sem LMS: sem os ciclos 6-7");
  }
  // modo F largo: bytes nos pares 12–104 (o 48º, no ciclo 106, já não toma o barramento); refresh nos ímpares
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x4F, 0x00, 0x10, 0x0F, 0x41, 0x00, 0x08}, 0x23, 0xFF);
    check(stealStr(*m, 32) == mapOf(join({{1, 6, 7}, range(12, 104, 2), range(25, 57, 4)})),
          "modo F largo com LMS: bytes 12-104, refresh 25-57\n    " + stealStr(*m, 32));
    check(px(*m, 33, 220, 1) == ON && px(*m, 33, 223, 1) == ON, "modo F largo: o último byte aparece mesmo sem tomar o ciclo 106");
  }
  // modo 8 estreito com rolagem horizontal (HSCROL 5: busca na largura normal, 2 ciclos depois): bytes 22–94
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x58, 0x00, 0x10, 0x41, 0x00, 0x08}, 0x21, 0x55);
    m->poke(0xD404, 5);
    m->runFrame();
    check(stealStr(*m, 32) == mapOf(join({{1, 6, 7}, range(22, 94, 8), range(25, 57, 4)})),
          "modo 8 com HSCROL 5: bytes nos ciclos 22, 30, ... 94\n    " + stealStr(*m, 32));
    check(stealStr(*m, 33) == mapOf(range(25, 57, 4)), "modo 8, linha seguinte: sem DMA de playfield");
  }
  // modo 6 com jogadores e mísseis: mísseis 0, jogadores 2-5, nomes 18-94 de 4 em 4, desenhos 21-97
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x46, 0x00, 0x10, 0x41, 0x00, 0x08}, 0x2E, 1);
    m->poke(0xC01D, 3);
    m->runFrame();
    check(stealStr(*m, 32) == mapOf(join({{0, 1, 2, 3, 4, 5, 6, 7}, range(18, 94, 4), range(21, 97, 4), range(27, 59, 4)})),
          "modo 6 com jogadores: 0-7, nomes 18-94, desenhos 21-97, refresh adiado para 27-59\n    " + stealStr(*m, 32));
  }
  // CHBASE trocado no ciclo 61 da linha 34: os desenhos buscados depois (byte 21 em diante, ciclo 63) são da fonte B
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x42, 0x00, 0x10, 0x02, 0x41, 0x00, 0x08}, 0x22, 1);
    goTo(*m, 34, 61);
    m->poke(0xD409, 0x24);
    m->runFrame();
    check(px(*m, 33, 200) == ON && px(*m, 35, 50) == OFF, "CHBASE: linha anterior inteira com a fonte A, seguinte com a B");
    check(px(*m, 34, 48) == ON && px(*m, 34, 131, 1) == ON && px(*m, 34, 132) == OFF && px(*m, 34, 207) == OFF,
          "CHBASE no ciclo 61: bytes 0-20 com a fonte A, 21-39 com a B (corte no ponto de cor 132)");
    m->poke(0xD409, 0x20);
    m->runFrame();
    goTo(*m, 34, 40);
    m->poke(0xD409, 0x24);
    m->runFrame();
    check(px(*m, 34, 87, 1) == ON && px(*m, 34, 88) == OFF, "CHBASE no ciclo 40: corte no byte 10 (ponto de cor 88)");
  }
  // CHACTL (inversão) trocado no ciclo 41: só os caracteres buscados depois saem invertidos
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x42, 0x00, 0x10, 0x02, 0x41, 0x00, 0x08}, 0x22, 0x81);
    goTo(*m, 34, 41);
    m->poke(0xD401, 2);
    m->runFrame();
    check(px(*m, 34, 91, 1) == ON && px(*m, 34, 92) == OFF && px(*m, 35, 48) == OFF && px(*m, 33, 207) == ON,
          "CHACTL no ciclo 41: bytes 0-10 normais, 11-39 invertidos");
  }
  // DMACTL: playfield desligado no ciclo 60: a DMA para ali e o resto da linha fica com o fundo
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x42, 0x00, 0x10, 0x02, 0x41, 0x00, 0x08}, 0x22, 1);
    goTo(*m, 34, 60);
    m->poke(0xD400, 0x20);
    m->runFrame();
    check(stealStr(*m, 34) == mapOf(join({range(21, 59, 2), range(26, 58, 4)})),
          "DMACTL no ciclo 60: desenhos só até o 59\n    " + stealStr(*m, 34));
    check(px(*m, 34, 127, 1) == ON && px(*m, 34, 128) == BK && px(*m, 35, 60) == BK, "DMACTL no ciclo 60: bytes 0-19 e depois o fundo");
  }
  // VSCROL comparado no fim da linha: a linha de modo que fecha a região rolada acaba onde o VSCROL novo manda
  {
    std::vector<uint8_t> dl = {0x70, 0x70, 0x70, 0x62, 0x00, 0x10, 0x02, 0x4F, 0x00, 0x14, 0x0F, 0x0F, 0x41, 0x00, 0x08};
    auto m = cycleMachine(dl, 0x22, 0);
    for (int a = 0x1400; a < 0x1500; a++) m->poke(static_cast<uint16_t>(a), 0xFF);
    m->poke(0xD405, 7);
    m->runFrame();
    // VSCROL 7: primeira linha de modo só com a linha 7 (32), segunda com 0-7 (33-40), modo F a partir da 41
    check(px(*m, 40, 100) == OFF && px(*m, 41, 100) == ON, "VSCROL 7: fim da região na linha 40");
    m->runFrame();
    goTo(*m, 35, 50);
    m->poke(0xD405, 2);
    m->runFrame();
    check(px(*m, 35, 100) == OFF && px(*m, 36, 100) == ON && px(*m, 38, 100) == ON,
          "VSCROL 7 -> 2 no meio da linha 35 (linha 2 da região): a linha de modo acaba nela e o modo F vem na 36");
  }
  // brancos depois de uma região rolada também a fecham: VSCROL + 1 linhas
  {
    std::vector<uint8_t> dl = {0x70, 0x70, 0x70, 0x62, 0x00, 0x10, 0x70, 0x4F, 0x00, 0x14, 0x0F, 0x41, 0x00, 0x08};
    auto m = cycleMachine(dl, 0x22, 0);
    for (int a = 0x1400; a < 0x1500; a++) m->poke(static_cast<uint16_t>(a), 0xFF);
    m->poke(0xD405, 4);
    m->runFrame();
    // modo 2 rolado: linhas 4-7 (32-35); branco com VSCROL 4: 5 linhas (36-40); modo F na 41
    check(px(*m, 35, 100) == OFF && px(*m, 36, 100) == BK && px(*m, 40, 100) == BK && px(*m, 41, 100) == ON,
          "linha em branco que fecha a região rolada: VSCROL + 1 = 5 linhas");
  }
  // com a CPU de verdade: espera VCOUNT = $32 (linhas 100-101), STA WSYNC, LDX #n, laço DEX/BNE e STA CHBASE:
  // com n = 1 a escrita cai no ciclo 2 da linha seguinte (antes do primeiro desenho, no 21: borda limpa); com
  // n = 6 cai no meio da linha seguinte
  for (int ws = 1; ws >= 0; ws--) {
    std::vector<uint8_t> cart = idleCart();
    const uint8_t prog[] = {0xAD, 0x0B, 0xD4, 0xD0, 0xFB, 0xA9, 0x20, 0x8D, 0x09, 0xD4, 0xAD, 0x0B, 0xD4, 0xC9, 0x32,
                            0xD0, 0xF9, 0x8D, 0x0A, 0xD4, 0xA2, uint8_t(ws ? 1 : 6), 0xCA, 0xD0, 0xFD, 0xA9, 0x24,
                            0x8D, 0x09, 0xD4, 0x4C, 0x00, 0x40};
    std::copy(std::begin(prog), std::end(prog), cart.begin());
    std::vector<uint8_t> dl = {0x70, 0x70, 0x70, 0x42, 0x00, 0x10};
    for (int i = 0; i < 25; i++) dl.push_back(0x02);
    dl.push_back(0x41); dl.push_back(0x00); dl.push_back(0x08);
    auto m = cycleMachine(dl, 0x22, 1, cart);
    m->runFrame();
    int mixed = 0, first = -1;
    bool clean = true;
    for (int y = 90; y < 120; y++) {
      int on = 0, off = 0;
      for (int cc = 48; cc < 208; cc++) (px(*m, y, cc, 1) == ON ? on : off)++;
      if (on && off) mixed++;
      if (std::getenv("A5200_DBG")) std::printf("    linha %d: %d acesos %d apagados\n", y, on, off);
      if (!on && first < 0) first = y;
      if (first >= 0 && on) clean = false;
    }
    if (ws)
      check(mixed == 0 && clean && (first == 101 || first == 102), "WSYNC + 8 ciclos + STA CHBASE: troca na borda da linha " + std::to_string(first));
    else
      check(mixed == 1 && clean, "WSYNC + laço de 6 voltas + STA CHBASE: a troca cai no meio de uma linha (" + std::to_string(mixed) + " linha mista)");
  }
  // estado gravado no meio de uma linha (DMA do playfield, refresh pendente e jogador disparado no meio do caminho)
  {
    auto m = cycleMachine({0x70, 0x70, 0x70, 0x42, 0x00, 0x10, 0x02, 0x41, 0x00, 0x08}, 0x2E, 1);
    m->poke(0xC00D, 0xFF); m->poke(0xC008, 3); m->poke(0xC000, 100); m->poke(0xC012, 0x36);
    m->runFrame();
    goTo(*m, 34, 55);  // ciclo 55: desenhos em curso, P0 disparado no ponto 100
    const auto st = m->saveState();
    m->poke(0xD409, 0x24);
    m->runFrame(); m->runFrame();
    const auto fb1 = m->fb;
    const auto end1 = m->saveState();
    m->loadState(st);
    m->poke(0xD409, 0x24);
    m->runFrame(); m->runFrame();
    check(m->fb == fb1 && m->saveState() == end1, "estado do meio da linha: carregar e rodar dá a mesma tela e o mesmo estado");
    std::mt19937 rng(5200);
    int ok = 0;
    for (int t = 0; t < 40; t++) {
      auto bad = st;
      for (int k = 0; k < 6; k++) bad[rng() % bad.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
      try { m->loadState(bad); } catch (const std::exception&) {}
      for (int f = 0; f < 3; f++) m->runFrame();
      ok++;
    }
    m->loadState(st);
    check(ok == 40 && m->saveState() == st, "40 estados do meio da linha estragados rodados sem quebrar; o bom volta igual");
    // estado adulterado com o disparo do P0 e do M0 depois do ponto já desenhado: tem de ser descartado ao
    // carregar, e não virar um deslocamento fora da faixa no desenho. A escrita em HITCLR desenha até o ciclo 55
    // (ponto 110), com o P0 disparado no ponto 100 e o M0 no 0
    m->loadState(st);
    m->poke(0xC01E, 0);
    const auto st2 = m->saveState();
    const uint8_t pat[] = {110, 0, 0, 0, 100, 0, 0, 0};  // rendered, pTrig[0], mTrig[0] (HPOSM0 = 0)
    size_t at = 0;
    int achados = 0;
    for (size_t i = 0; i + sizeof pat <= st2.size(); i++)
      if (std::equal(pat, pat + sizeof pat, st2.begin() + i)) { at = i; achados++; }
    bool limpo = false;
    if (achados == 1) {
      auto bad = st2;
      bad[at + 4] = 220; bad[at + 6] = 200; bad[at + 7] = 0;  // pTrig[0] = 220, mTrig[0] = 200
      m->loadState(bad);
      const auto re = m->saveState();
      limpo = re[at + 4] == 0xFF && re[at + 5] == 0xFF && re[at + 6] == 0xFF && re[at + 7] == 0xFF;
      m->runFrame(); m->runFrame();
      m->loadState(st2);
      limpo = limpo && m->saveState() == st2;
    }
    check(achados == 1 && limpo, "estado com jogador e míssil disparados adiante do ponto desenhado: disparo descartado ao carregar");
  }
}

// GTIA no meio da linha: HPOS trocado enquanto o jogador sai (a imagem não é cortada e o jogador volta na posição
// nova), reaproveitamento do jogador na mesma linha e colisões lidas no meio da linha
void testGtiaLine() {
  std::printf("== GTIA no meio da linha: HPOS, reaproveitamento de jogador, colisões\n");
  auto m = machine(idleCart());
  for (int i = 0; i < 15; i++) m->runFrame();
  const uint32_t P = A5200::color(0x36), Z = A5200::color(0);
  m->poke(0xC01A, 0); m->poke(0xC012, 0x36); m->poke(0xC013, 0x86);
  m->poke(0xC00D, 0xFF); m->poke(0xC008, 3); m->poke(0xC000, 100);  // P0 quádruplo: pontos 100-131
  m->runFrame(); m->runFrame();
  goTo(*m, 120, 55);  // ponto de cor 110: no meio da imagem
  m->poke(0xC000, 200);
  m->runFrame();
  check(px(*m, 120, 100) == P && px(*m, 120, 131, 1) == P && px(*m, 120, 132) == Z,
        "HPOS trocado no ponto 110: a imagem já disparada sai inteira (100-131)");
  check(px(*m, 120, 199) == Z && px(*m, 120, 200) == P && px(*m, 120, 223) == P, "e o jogador dispara de novo no HPOS novo (200)");
  check(px(*m, 119, 200) == Z && px(*m, 121, 100) == Z && px(*m, 121, 200) == P, "linhas vizinhas só com uma posição");
  m->poke(0xC000, 60);
  m->runFrame();
  goTo(*m, 130, 50);
  m->poke(0xC000, 150);
  m->runFrame();
  check(px(*m, 130, 60) == P && px(*m, 130, 91) == P && px(*m, 130, 150) == P && px(*m, 130, 181) == P && px(*m, 130, 120) == Z,
        "jogador reaproveitado: 60-91 e 150-181 na mesma linha");
  // colisão P0 x P1 nos pontos 160-167 (ciclo 80): lida no ciclo 60 ainda não existe, no 100 já
  m->poke(0xC00E, 0xFF); m->poke(0xC001, 160);
  m->runFrame();
  goTo(*m, 140, 10);
  m->poke(0xC01E, 0);
  goTo(*m, 140, 60);
  const uint8_t antes = m->read(0xC00C);
  goTo(*m, 140, 100);
  const uint8_t depois = m->read(0xC00C);
  m->runFrame();
  check(antes == 0 && depois == 2, "colisão P0PL lida no meio da linha: " + std::to_string(antes) + " antes do ponto 160, " +
                                   std::to_string(depois) + " depois");
  check(px(*m, 140, 160) == P && px(*m, 140, 167, 1) == P, "P0 na frente de P1 onde se cruzam");
}

// POKEY: o primeiro pulso do temporizador depois de STIMER cai na grade do divisor de 64 kHz (que corre sempre), e
// o contador dos potenciômetros sobe na grade do relógio de 15 kHz
void testPokeyClock() {
  std::printf("== POKEY: divisores de 64 kHz e 15 kHz (temporizador e potenciômetros)\n");
  auto m = machine(idleCart());  // a CPU fica com I = 1: a IRQ só aparece em IRQST
  for (int i = 0; i < 15; i++) m->runFrame();
  m->poke(0xE80F, 3); m->poke(0xE808, 0); m->poke(0xE800, 9);  // 64 kHz, AUDF1 = 9: 10 x 28 = 280 ciclos
  // espera a IRQ do temporizador 1 lendo IRQST; devolve o ciclo da leitura que a viu
  auto waitIrq = [&]() -> int64_t {
    for (int k = 0; k < 600; k++) {
      const uint8_t v = m->read(0xE80E);
      if (!(v & 1)) return static_cast<int64_t>(m->clk) - 1;
    }
    return -1;
  };
  int dmin = 1 << 30, dmax = 0, foraDaGrade = 0, periodoErrado = 0;
  for (int ph = 0; ph < 28; ph++) {
    m->runFrame();
    goTo(*m, 250, 60);
    m->runCycles(int((ph + 28 - m->clk % 28) % 28));  // STIMER na fase ph do divisor
    m->poke(0xE80E, 0); m->poke(0xE80E, 1);
    const int64_t c0 = static_cast<int64_t>(m->clk);
    m->poke(0xE809, 0);  // STIMER
    const int64_t t1 = waitIrq();
    const int64_t want = (c0 / 28 + 1) * 28 + 9 * 28;
    if (t1 < want || t1 > want + 1) foraDaGrade++;
    dmin = std::min<int>(dmin, int(t1 - c0));
    dmax = std::max<int>(dmax, int(t1 - c0));
    m->poke(0xE80E, 0); m->poke(0xE80E, 1);
    const int64_t t2 = waitIrq();
    if (t2 < want + 280 || t2 > want + 281) periodoErrado++;
  }
  check(foraDaGrade == 0, "primeira IRQ depois de STIMER na grade de 28 ciclos, nas 28 fases (" + std::to_string(foraDaGrade) + " fora)");
  check(dmin >= 253 && dmax <= 281 && dmax - dmin >= 26, "atraso da primeira IRQ varia com a fase: " + std::to_string(dmin) + " a " + std::to_string(dmax) + " ciclos");
  check(periodoErrado == 0, "segunda IRQ 280 ciclos depois da primeira em todas as fases");
  // potenciômetros: POTGO no ciclo 100 de uma linha; 13 ciclos depois o contador ainda é 0, 14 depois já é 1
  m->setInput(200, 200, false, false, -1);
  m->runFrame();
  goTo(*m, 250, 0);
  m->runCycles(int((100 + 114 - m->clk % 114) % 114));
  m->poke(0xE80B, 0);  // POTGO
  m->runCycles(13);
  const uint8_t a = m->read(0xE800);  // lido 13 ciclos depois (ciclo 113 da linha)
  const uint8_t b = m->read(0xE800);  // 14 ciclos depois: passou um passo de 15 kHz
  m->runCycles(114 * 5);
  const uint8_t c = m->read(0xE800);
  const uint8_t all = m->read(0xE808);
  check(a == 0 && b == 1 && c == 6 && (all & 1), "POT0 contando na grade de 114 ciclos: " + std::to_string(a) + ", " +
        std::to_string(b) + ", " + std::to_string(c) + " (esperado 0, 1, 6) e ALLPOT ainda contando");
  m->runFrame(); m->runFrame();
  check(m->read(0xE800) == 200 && !(m->read(0xE808) & 1), "POT0 = 200 depois da varredura, ALLPOT livre");
}

void testPm() {
  std::printf("== GTIA: jogadores, mísseis, prioridade, colisões, modos GTIA\n");
  auto m = machine(idleCart());
  for (int i = 0; i < 15; i++) m->runFrame();
  auto run = [&] { m->runFrame(); m->runFrame(); };
  auto C = [](uint8_t c) { return A5200::color(c & 0xFE); };
  m->poke(0xC01A, 0x00);
  m->poke(0xC012, 0x36);  // COLPM0
  m->poke(0xC013, 0x86);  // COLPM1
  m->poke(0xC00D, 0x81);  // GRAFP0 = 10000001 (sem DMA: vale em todas as linhas)
  m->poke(0xC000, 100);
  run();
  check(px(*m, 100, 100) == C(0x36) && px(*m, 100, 107) == C(0x36) && px(*m, 100, 101) == C(0) && px(*m, 100, 108) == C(0), "jogador 0: tamanho normal em HPOSP0 = 100");
  m->poke(0xC008, 1);
  run();
  check(px(*m, 100, 101) == C(0x36) && px(*m, 100, 102) == C(0) && px(*m, 100, 114) == C(0x36) && px(*m, 100, 115) == C(0x36) && px(*m, 100, 116) == C(0), "jogador 0: tamanho duplo");
  m->poke(0xC008, 3);
  run();
  check(px(*m, 100, 103) == C(0x36) && px(*m, 100, 104) == C(0) && px(*m, 100, 128) == C(0x36) && px(*m, 100, 131) == C(0x36) && px(*m, 100, 132) == C(0), "jogador 0: tamanho quádruplo");
  m->poke(0xC008, 0);
  // míssil 0: 2 bits na cor do jogador 0; míssil 1 duplo
  m->poke(0xC011, 0x0B);  // GRAFM: míssil 0 = 11, míssil 1 = 10
  m->poke(0xC004, 60);
  m->poke(0xC005, 70);
  m->poke(0xC00C, 0x04);  // SIZEM: míssil 1 duplo
  run();
  check(px(*m, 100, 60) == C(0x36) && px(*m, 100, 61) == C(0x36) && px(*m, 100, 62) == C(0), "míssil 0: 2 pontos na cor do jogador 0");
  check(px(*m, 100, 70) == C(0x86) && px(*m, 100, 71) == C(0x86) && px(*m, 100, 72) == C(0), "míssil 1: bit de cima, tamanho duplo");
  m->poke(0xC01B, 0x10);  // quinto jogador: mísseis na cor de PF3
  m->poke(0xC019, 0xC4);
  run();
  check(px(*m, 100, 60) == C(0xC4), "quinto jogador: mísseis com COLPF3");
  m->poke(0xC01B, 0x00);
  m->poke(0xC011, 0);
  // jogador 1 sobre o 0: P0 vence; com cores múltiplas (PRIOR bit 5) as cores se juntam (OU)
  m->poke(0xC00E, 0xFF);
  m->poke(0xC001, 100);
  run();
  check(px(*m, 100, 100) == C(0x36) && px(*m, 100, 103) == C(0x86), "P0 na frente de P1");
  m->poke(0xC01B, 0x20);
  run();
  check(px(*m, 100, 100) == C(0x36 | 0x86) && px(*m, 100, 103) == C(0x86), "cores múltiplas: P0 OU P1");
  m->poke(0xC01B, 0x00);
  m->poke(0xC00E, 0);
  // playfield de PF0 (modo D, cor 01) em todas as linhas com P0 e P2 por cima: prioridades
  for (int a = 0; a < 0x1000; a++) m->poke(static_cast<uint16_t>(0x1000 + a), 0x55);
  std::vector<uint8_t> dl = {0x4D, 0x00, 0x10};
  for (int i = 0; i < 100; i++) dl.push_back(0x0D);
  dl.push_back(0x41); dl.push_back(0x00); dl.push_back(0x08);
  for (size_t i = 0; i < dl.size(); i++) m->poke(static_cast<uint16_t>(0x0800 + i), dl[i]);
  m->poke(0xD402, 0x00); m->poke(0xD403, 0x08); m->poke(0xD400, 0x22);
  m->poke(0xC016, 0x0A);  // COLPF0
  m->poke(0xC014, 0x46);  // COLPM2
  m->poke(0xC00F, 0xFF);  // GRAFP2
  m->poke(0xC002, 150);
  m->poke(0xC01E, 0);     // HITCLR
  struct { uint8_t prior; bool p0Front, p2Front; } cases[] = {{1, true, true}, {2, true, false}, {4, false, false}, {8, false, false}};
  for (auto& c : cases) {
    m->poke(0xC01B, c.prior);
    run();
    const bool p0 = px(*m, 100, 100) == C(0x36), p2 = px(*m, 100, 150) == C(0x46);
    const bool pf0 = px(*m, 100, 100) == C(0x0A), pf2 = px(*m, 100, 150) == C(0x0A);
    char buf[120];
    std::snprintf(buf, sizeof buf, " (%06X %06X)", px(*m, 100, 100) & 0xFFFFFF, px(*m, 100, 150) & 0xFFFFFF);
    check(p0 == c.p0Front && pf0 == !c.p0Front && p2 == c.p2Front && pf2 == !c.p2Front, "PRIOR " + std::to_string(c.prior) + ": ordem de jogadores e playfield" + buf);
  }
  m->poke(0xC01B, 0);
  run();
  check(px(*m, 100, 100) == C(0x36 | 0x0A) && px(*m, 100, 150) == C(0x0A), "PRIOR 0: P0 e PF0 se juntam (OU), P2 atrás de PF0");
  // colisões: P0 e P2 sobre PF0; HITCLR zera
  const uint8_t p0pf = m->read(0xC004), p2pf = m->read(0xC006), p0pl = m->read(0xC00C);
  check(p0pf == 1 && p2pf == 1 && p0pl == 0, "colisões: P0PF = P2PF = 1 (PF0), P0PL = 0");
  m->poke(0xC00E, 0x80);  // P1 em 100: encosta em P0
  m->poke(0xC01E, 0);
  run();
  check(m->read(0xC00C) == 2 && m->read(0xC00D) == 1, "colisão entre jogadores: P0PL = 2, P1PL = 1");
  m->poke(0xC01E, 0);
  check(m->read(0xC004) == 0 && m->read(0xC00C) == 0, "HITCLR zera as colisões");
  m->poke(0xC00E, 0); m->poke(0xC00D, 0); m->poke(0xC00F, 0);
  // DMA de jogadores e mísseis: linha simples (PMBASE = $20: jogador 0 em $2400) e linha dupla ($2000 + $200)
  for (int a = 0x2000; a < 0x2800; a++) m->poke(static_cast<uint16_t>(a), 0);
  m->poke(static_cast<uint16_t>(0x2400 + 120), 0xFF);
  m->poke(0xC01B, 1);
  m->poke(0xD407, 0x20);
  m->poke(0xD400, 0x3E);  // linha simples, jogadores + mísseis, largura normal, lista
  m->poke(0xC01D, 3);
  m->poke(0xC000, 80);
  run();
  check(px(*m, 120, 80) == C(0x36) && px(*m, 119, 80) != C(0x36) && px(*m, 121, 80) != C(0x36), "DMA de linha simples: byte 120 na linha 120");
  m->poke(static_cast<uint16_t>(0x2400 + 120), 0);
  m->poke(static_cast<uint16_t>(0x2200 + 60), 0xFF);
  m->poke(0xD400, 0x2E);  // linha dupla
  run();
  check(px(*m, 120, 80) == C(0x36) && px(*m, 121, 80) == C(0x36) && px(*m, 119, 80) != C(0x36) && px(*m, 122, 80) != C(0x36), "DMA de linha dupla: byte 60 nas linhas 120 e 121");
  m->poke(0xC01C, 0x10);  // VDELAY do jogador 0
  run();
  check(px(*m, 121, 80) == C(0x36) && px(*m, 122, 80) == C(0x36) && px(*m, 120, 80) != C(0x36), "VDELAY: desce uma linha");
  check(m->stolenLine[100] == 1 + 40 + 9 + 5 && m->stolenLine[101] == 9 + 5, "modo D com jogadores e mísseis: 55 e 14 ciclos (" +
        std::to_string(m->stolenLine[100]) + ", " + std::to_string(m->stolenLine[101]) + ")");
  m->poke(0xC01C, 0); m->poke(0xC01D, 0); m->poke(0xD400, 0x22); m->poke(0xC00D, 0);
  // modos GTIA com modo F: byte $5A = valores 5 e 10 de 4 bits
  for (int a = 0; a < 0x1000; a++) m->poke(static_cast<uint16_t>(0x1000 + a), 0x5A);
  dl = {0x4F, 0x00, 0x10};
  for (int i = 0; i < 100; i++) dl.push_back(0x0F);
  dl.push_back(0x41); dl.push_back(0x00); dl.push_back(0x08);
  for (size_t i = 0; i < dl.size(); i++) m->poke(static_cast<uint16_t>(0x0800 + i), dl[i]);
  m->poke(0xC01A, 0x74);
  m->poke(0xC01B, 0x40);
  run();
  check(px(*m, 60, 48) == A5200::color(0x75) && px(*m, 60, 50) == A5200::color(0x7A), "GTIA 1: 16 luminâncias na matiz de COLBK");
  m->poke(0xC01B, 0xC0);
  run();
  check(px(*m, 60, 48) == C(0x54) && px(*m, 60, 50) == C(0xA4), "GTIA 3: 16 matizes na luminância de COLBK");
  for (int i = 0; i < 4; i++) m->poke(static_cast<uint16_t>(0xC016 + i), static_cast<uint8_t>(0x10 * (i + 1) + 2));
  m->poke(0xC01B, 0x80);
  run();
  check(px(*m, 60, 48) == C(0x22) && px(*m, 60, 50) == C(0x74), "GTIA 2: 9 cores (5 = PF1, 10 = fundo)");
}

void testAltirra(const std::string& root) {
  const auto os = readAll(root + "/a5200/altirra5200.rom");
  if (os.size() != 0x800) { info("altirra5200.rom ausente: teste com a BIOS livre do Altirra pulado"); return; }
  std::printf("== BIOS livre do Altirra (5200 OS, licença permissiva)\n");
  for (int diag = 0; diag < 2; diag++) {
    auto rom = readAll(root + "/a5200/cart.a52");
    if (diag) rom[0x7FFD] = 0xFF;
    Cart c;
    std::string err;
    c.load(rom, "cart.a52", err);
    A5200 m(std::move(c), os);
    int f = 0, colors = 0;
    for (; f < 600 && m.peek(0x3000) != 0xA5; f++) {
      m.runFrame();
      if (f == 60) {
        std::vector<uint32_t> u(m.fb);
        std::sort(u.begin(), u.end());
        colors = int(std::unique(u.begin(), u.end()) - u.begin());
      }
    }
    const std::string tipo = diag ? " (diagnóstico, $FF em $BFFD)" : " (com título)";
    check(m.peek(0x3000) == 0xA5, "o cartucho de teste liga pela BIOS do Altirra em " + std::to_string(f) + " quadros" + tipo);
    if (!diag) check(colors >= 3, "tela de título da BIOS do Altirra com " + std::to_string(colors) + " cores");
    for (int i = 0; i < 3; i++) m.runFrame();
    const uint8_t f0 = m.peek(0x3001), d0 = m.peek(0x3007);
    for (int i = 0; i < 10; i++) m.runFrame();
    check(static_cast<uint8_t>(m.peek(0x3001) - f0) == 10 && static_cast<uint8_t>(m.peek(0x3007) - d0) == 20,
          "VBI e DLI do cartucho chamados pela BIOS do Altirra" + tipo);
    if (!diag) {
      std::vector<uint32_t> img(m.fb);
      if (const char* dir = std::getenv("A5200_PNG")) writePng(std::string(dir) + "/altirra.png", img.data(), A5200::W, A5200::H);
      // mesma tela da BIOS de teste: as duas BIOS chegam ao mesmo cartucho rodando igual
      auto mine = machine(rom);
      for (int i = 0; i < 20; i++) mine->runFrame();
      check(mine->fb == img, "mesma tela com a BIOS de teste e com a do Altirra");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::string(argv[1]) == "--png") {
    BIOS = readAll(std::string(argc > 5 ? argv[5] : "/root/indigo-env/roms/a5200/bios_teste.rom"));
    auto m = machine(readAll(argv[2]), argv[2]);
    for (int i = 0, n = std::atoi(argv[3]); i < n; i++) m->runFrame();
    writePng(argv[4], m->fb.data(), A5200::W, A5200::H);
    return 0;
  }
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/a5200";
  const std::string root = std::filesystem::path(dir).parent_path().string();
  BIOS = readAll(dir + "/bios_teste.rom");
  if (BIOS.size() != 0x800) { std::printf("FALHA: %s/bios_teste.rom ausente (rode tools/t7_a5200.sh)\n", dir.c_str()); return 1; }
  setFirmware("a5200", BIOS);
  testDetect(root);
  testCarts();
  testBoot(root);
  testPokeyIrq();
  testFuzz();
  testAntic();
  testCycles();
  testPm();
  testGtiaLine();
  testPokeyClock();
  testAltirra(root);
  std::printf("RESULTADO a5200: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
