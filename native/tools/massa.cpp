// Teste em massa: roda um jogo caseiro (homebrew de licença livre, baixado por tools/massa.sh) sem interface e
// confere o que dá para conferir sem olhar a tela: o console certo foi reconhecido, nada trava nem lança erro, a
// imagem muda (o jogo não ficou parado numa tela só), há som, e o save state volta igual. Uma ROM por execução,
// para que uma queda do programa pare só aquele jogo (o massa.sh roda vários ao mesmo tempo, com tempo limite).
//
// uso: indigo-massa console rom [quadros] [tela_final.ppm]
// MASSA_TESTE=1: é ROM de teste (tela parada e silêncio não são aviso).
// INDIGO_FW="nome=arquivo;...": BIOS do usuário para os consoles que pedem (ex.: ps1=/pasta/scph1001.bin); os
// discos (.cue, .gdi) leem os arquivos ao lado pelo caminho.
// saída: uma linha "OK|AVISO|FALHA console rom: detalhes". Código de saída 0 (OK ou AVISO) ou 1 (FALHA).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/dc/dc.h"
#include "../src/neogeocd/neogeocd.h"
#include "../src/pcecd/pcecd.h"
#include "../src/pcfx/pcfx.h"
#include "../src/ps1/ps1.h"
#include "../src/saturn/saturn.h"
#include "../src/segacd/segacd.h"
#include "../src/tdo/tdo.h"

using namespace indigo;

namespace {

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

uint32_t fnv(const uint32_t* p, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

// teclas de um "jogador" sem olhar a tela, sempre iguais: Start e A de vez em quando (para passar das telas de
// título e de menus), e no resto do tempo direções e botões variados, segurados por alguns quadros
uint32_t teclas(int f) {
  const int ciclo = f % 240;
  if (ciclo < 6) return KEY_START;
  if (ciclo >= 60 && ciclo < 66) return KEY_A;
  if (ciclo >= 120 && ciclo < 126) return KEY_START;
  static const uint32_t mov[] = {KEY_RIGHT, KEY_RIGHT | KEY_A, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_B, KEY_RIGHT | KEY_B, KEY_A};
  uint32_t x = uint32_t(f / 20) * 2654435761u;
  return mov[(x >> 13) & 7];
}

// o console da lista aceita estes nomes de Core::system()
bool consoleBate(const std::string& esperado, const std::string& s) {
  if (esperado == "gb") return s == "gb" || s == "gbc";
  if (esperado == "sg") return s == "sg1000";
  if (esperado == "ngp") return s == "ngp" || s == "ngpc";
  if (esperado == "ws") return s == "ws" || s == "wsc";
  if (esperado == "pce") return s == "pce" || s == "sgx";
  return s == esperado;
}

// disco (.cue, .chd, .iso, .gdi): o mesmo caminho do app (src/app/main.cpp, openDisc)
std::unique_ptr<Core> abreDisco(const std::string& path) {
  std::string err;
  auto disc = cdrom::open(path, &err);
  if (!disc) throw std::runtime_error("o CD não abriu: " + err);
  const std::string sys = cdrom::detectSystem(*disc);
  if (sys == "pcfx") return std::make_unique<pcfx::PcfxCore>(std::move(disc), nullptr);
  if (sys == "dc") return std::make_unique<dc::DcCore>(std::move(disc), nullptr);
  if (sys == "saturn") return std::make_unique<saturn::SaturnCore>(std::move(disc), nullptr);
  if (sys == "3do") return std::make_unique<tdo::TdoCore>(std::move(disc), nullptr);
  if (sys == "ps1") return std::make_unique<ps1::Ps1Core>(std::move(disc), nullptr);
  if (sys == "segacd") return segacd::createSegaCdCore(std::move(disc), nullptr);
  if (sys == "pcecd") { disc.reset(); return pcecd::createPceCdCore(path, nullptr, &err); }
  if (sys == "neogeocd") return std::make_unique<neogeocd::NeoGeoCdCore>(std::move(disc), nullptr);
  return nullptr;
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "uso: %s console rom [quadros] [tela_final.ppm]\n", argv[0]);
    return 2;
  }
  const std::string esperado = argv[1];
  const char* path = argv[2];
  const int quadros = argc > 3 ? std::max(600, std::atoi(argv[3])) : 3600;
  const std::string nome = std::string(path).substr(std::string(path).find_last_of('/') + 1);
  auto falha = [&](const std::string& m) {
    std::printf("FALHA %s %s: %s\n", esperado.c_str(), nome.c_str(), m.c_str());
    return 1;
  };
  if (const char* fw = std::getenv("INDIGO_FW")) {
    const std::string all = fw;
    for (size_t p = 0; p < all.size();) {
      size_t e = all.find(';', p);
      if (e == std::string::npos) e = all.size();
      const std::string item = all.substr(p, e - p);
      const size_t eq = item.find('=');
      if (eq != std::string::npos) setFirmware(item.substr(0, eq), readFile(item.substr(eq + 1).c_str()));
      p = e + 1;
    }
  }
  const auto rom = readFile(path);
  if (rom.empty()) return falha("arquivo vazio ou não encontrado");
  try {
    const bool disco = cdrom::isDiscPath(path);
    auto mk = [&] { return disco ? abreDisco(path) : createCore(rom, path, nullptr); };
    auto c = mk();
    if (!c) return falha("ROM não reconhecida");
    if (!consoleBate(esperado, c->system())) return falha("reconhecida como " + c->system());
    c->setSampleRate(48000);
    std::vector<float> l(4096), r(4096);
    double pico = 0;
    std::set<uint32_t> telas;
    int vazias = 0, amostras = 0;
    const int meio = quadros / 2;
    std::vector<uint8_t> estado;
    std::vector<uint32_t> depois;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < quadros; f++) {
      c->setKeys(teclas(f));
      c->runFrame();
      while (size_t n = std::min<size_t>(c->audioAvailable(), l.size())) {
        c->pullAudio(l.data(), r.data(), n);
        for (size_t i = 0; i < n; i++) pico = std::max(pico, double(std::fabs(l[i]) + std::fabs(r[i])));
      }
      const uint32_t* fb = c->framebuffer();
      const size_t px = size_t(c->width()) * c->height();
      if (f % 10 == 9) {
        amostras++;
        telas.insert(fnv(fb, px));
        bool uniforme = true;
        for (size_t i = 1; i < px && uniforme; i += 7) uniforme = fb[i] == fb[0];
        if (uniforme) vazias++;
      }
      if (f == meio) estado = c->saveState();
      if (f > meio && f <= meio + 300 && f % 30 == 0) depois.push_back(fnv(fb, px));
    }
    const double seg = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double vel = quadros / std::max(seg, 1e-6) / c->fps();
    if (argc > 4) writePpm(argv[4], c->framebuffer(), c->width(), c->height());
    // o save state do meio volta igual: um núcleo novo, carregado com ele, repete as mesmas telas
    auto d = mk();
    d->setSampleRate(48000);
    d->runFrame();
    d->loadState(estado);
    std::vector<uint32_t> repete;
    for (int f = meio + 1; f <= meio + 300; f++) {
      d->setKeys(teclas(f));
      d->runFrame();
      while (size_t n = std::min<size_t>(d->audioAvailable(), l.size())) d->pullAudio(l.data(), r.data(), n);
      if (f % 30 == 0) repete.push_back(fnv(d->framebuffer(), size_t(d->width()) * d->height()));
    }
    // ROM de teste (MASSA_TESTE=1): tela parada e silêncio são normais (o teste mostra o resultado e para); tela
    // sempre lisa vira só aviso (alguns testes só tocam som, outros mostram tela lisa quando falham: dá uma olhada)
    const bool teste = std::getenv("MASSA_TESTE") && std::string(std::getenv("MASSA_TESTE")) == "1";
    std::string avisos;
    bool ruim = false;
    if (repete != depois) { ruim = true; avisos += " save-state-diferente"; }
    if (vazias == amostras) {
      if (teste) avisos += " tela-lisa-olhar";
      else { ruim = true; avisos += " tela-sempre-vazia"; }
    } else if (telas.size() < 3 && !teste) {
      avisos += " tela-parada";
    }
    if (pico < 1e-4 && !teste) avisos += " sem-som";
    char det[256];
    std::snprintf(det, sizeof det, "%s, %zu telas diferentes, %d%% vazias, som %.3f, estado %zu KB, %.1fx", c->system().c_str(),
                  telas.size(), amostras ? vazias * 100 / amostras : 0, pico, estado.size() / 1024, vel);
    std::printf("%s %s %s: %s%s\n", ruim ? "FALHA" : avisos.empty() ? "OK" : "AVISO", esperado.c_str(), nome.c_str(), det,
                avisos.c_str());
    return ruim ? 1 : 0;
  } catch (const std::exception& e) {
    return falha(std::string("erro: ") + e.what());
  } catch (...) {
    return falha("erro desconhecido");
  }
}
