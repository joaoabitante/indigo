// Testes do Famicom Disk System (src/fds), com a BIOS e os discos de teste do t7_fds.sh (nada da Nintendo).
//   indigo-t7_fds <pasta>     (pasta = /root/indigo-env/roms/fds)
// Confere: reconhecimento dos discos e da BIOS (sem roubar arquivos dos outros consoles), o erro sem BIOS, a
// partida pela BIOS de teste (leitura dos blocos com CRC), o temporizador de IRQ, a gravação do arquivo de save
// de volta no disco (com CRC certo, num save separado), a troca de lado, o som de onda (frequência, envoltória,
// modulação), os formatos .fds com e sem cabeçalho e .qd, a recusa de CRC errado e o save state determinístico.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/fds/fds.h"

using namespace indigo;
using namespace indigo::fds;

namespace {
int okN = 0, badN = 0;
void check(bool c, const std::string& what) {
  if (c) okN++;
  else { badN++; std::printf("FALHA: %s\n", what.c_str()); }
}

std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint8_t ram(FdsCore& c, uint16_t a) { return c.nes().peek(a); }
uint8_t estado(FdsCore& c) { return ram(c, 0x30); }

// roda até cond() ou o limite; devolve o número de quadros rodados (-1 = não chegou)
int runUntil(FdsCore& c, const std::function<bool()>& cond, int limite) {
  for (int f = 0; f < limite; f++) {
    if (cond()) return f;
    c.runFrame();
    float l[4096], r[4096];
    while (c.audioAvailable()) c.pullAudio(l, r, std::min<size_t>(4096, c.audioAvailable()));
  }
  return cond() ? limite : -1;
}

uint64_t hashFb(FdsCore& c) {
  uint64_t h = 1469598103934665603ull;
  const uint32_t* fb = c.framebuffer();
  for (int i = 0; i < 256 * 240; i++) { h ^= fb[i]; h *= 1099511628211ull; }
  return h;
}
uint64_t hashMem(FdsCore& c) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t a = 0; a < 0x800; a++) { h ^= ram(c, static_cast<uint16_t>(a)); h *= 1099511628211ull; }
  for (uint32_t a = 0x6000; a < 0xE000; a++) { h ^= ram(c, static_cast<uint16_t>(a)); h *= 1099511628211ull; }
  return h ^ hashFb(c);
}

// percorre os blocos de um lado no formato do drive: confere o CRC de todos e devolve os dados do arquivo n
bool blocosDoLado(const std::vector<uint8_t>& raw, int arquivo, std::vector<uint8_t>& dados, int& crcRuim) {
  size_t p = 0;
  uint32_t tam = 0;
  int arq = -1;
  crcRuim = 0;
  while (p < raw.size()) {
    while (p < raw.size() && raw[p] == 0) p++;
    if (p >= raw.size() || raw[p] != 0x80) break;
    p++;
    size_t len;
    switch (raw[p]) {
      case 1: len = 56; break;
      case 2: len = 2; break;
      case 3: len = 16; tam = raw[p + 13] | (raw[p + 14] << 8); arq++; break;
      case 4: len = 1 + tam; break;
      default: return false;
    }
    if (p + len + 2 > raw.size()) return false;
    const uint16_t c = blockCrc(&raw[p], len);
    if (raw[p + len] != (c & 0xFF) || raw[p + len + 1] != (c >> 8)) crcRuim++;
    if (raw[p] == 4 && arq == arquivo) dados.assign(raw.begin() + static_cast<long>(p + 1), raw.begin() + static_cast<long>(p + len));
    p += len + 2;
  }
  return true;
}

// testes do som e do temporizador direto nos registradores do adaptador (sem rodar a CPU)
void testeRegistradores(const std::vector<uint8_t>& disco, const std::vector<uint8_t>& bios) {
  FdsCore c(disco, "teste.fds", nullptr, &bios);
  FdsMapper& m = c.adapter();
  auto ciclos = [&](int n) { for (int k = 0; k < n; k++) m.cpuCycle(); };
  // temporizador sem repetição: IRQ depois de reload+1 ciclos, uma vez só
  m.writeReg(0x4023, 0x83);
  m.writeReg(0x4020, 100);
  m.writeReg(0x4021, 0);
  m.writeReg(0x4022, 0x02);
  ciclos(100);
  check(!m.irq, "temporizador: sem IRQ antes da hora");
  ciclos(1);
  check(m.irq, "temporizador: IRQ depois de 101 ciclos");
  check((m.cpuRead(0x4030, 0) & 1) == 1, "$4030 bit 0 marca o IRQ do temporizador");
  check(!m.irq, "ler $4030 reconhece o IRQ");
  ciclos(500);
  check(!m.irq, "temporizador sem repetição para depois do primeiro IRQ");
  // com repetição
  m.writeReg(0x4022, 0x03);
  int n = 0;
  for (int k = 0; k < 1010; k++) { m.cpuCycle(); if (m.irq) { n++; m.cpuRead(0x4030, 0); } }
  check(n == 10, "temporizador com repetição: 10 IRQs em 1010 ciclos (deu " + std::to_string(n) + ")");
  m.writeReg(0x4022, 0);
  // $4023 bit 0 desligado: sem registradores do disco nem IRQ
  m.writeReg(0x4023, 0x02);
  m.writeReg(0x4022, 0x03);
  ciclos(300);
  check(!m.irq, "$4023 sem o bit 0: temporizador não liga");
  m.writeReg(0x4023, 0x83);

  // tabela de onda: rampa 0..63, frequência $100 = uma posição a cada 256 ciclos
  m.writeReg(0x4089, 0x80);
  for (int k = 0; k < 64; k++) m.writeReg(static_cast<uint16_t>(0x4040 + k), static_cast<uint8_t>(k));
  check(m.cpuRead(0x4045, 0) == 5, "com $4089.7 a tabela de onda é lida pelo endereço");
  m.writeReg(0x4089, 0x00);
  m.writeReg(0x4080, 0xA0);  // volume direto 32
  m.writeReg(0x4087, 0x80);  // modulação parada
  m.writeReg(0x4083, 0x80);  // para a onda (posição 0)
  m.writeReg(0x4082, 0x00);
  m.writeReg(0x4083, 0x01);
  ciclos(256 * 10 + 1);
  check(m.wavePos() == 10, "onda: posição 10 depois de 2561 ciclos com frequência $100 (deu " + std::to_string(m.wavePos()) + ")");
  check(m.cpuRead(0x4040, 0) == 10, "sem $4089.7, $4040–$407F leem a amostra atual");
  check(m.waveLevel() == 10 * 32 * 36 / 1152, "nível de saída = amostra × ganho × volume mestre");
  m.writeReg(0x4089, 0x03);  // volume mestre 2/5
  ciclos(1);
  check(m.waveLevel() == m.cpuRead(0x4040, 0) * 32 * 14 / 1152, "volume mestre 2/5");
  m.writeReg(0x4089, 0x00);
  // modulação: contador 10, ganho 32 -> desvio de +80 na frequência ($100 + 80 = 336 por ciclo)
  m.writeReg(0x4084, 0xA0);
  m.writeReg(0x4085, 10);
  m.writeReg(0x4083, 0x80);
  m.writeReg(0x4083, 0x01);
  ciclos(2000);
  check(m.wavePos() == 10, "modulação +10: 2000 ciclos andam 10 posições (deu " + std::to_string(m.wavePos()) + ")");
  m.writeReg(0x4085, 0x76);  // -10
  m.writeReg(0x4083, 0x80);
  m.writeReg(0x4083, 0x01);
  ciclos(2000);
  check(m.wavePos() == 5, "modulação -10: 2000 ciclos andam 5 posições (deu " + std::to_string(m.wavePos()) + ")");
  // tabela de modulação: +1 em todas as posições, frequência $800 = um passo a cada 32 ciclos
  for (int k = 0; k < 32; k++) m.writeReg(0x4088, 1);
  m.writeReg(0x4085, 0);
  m.writeReg(0x4086, 0x00);
  m.writeReg(0x4087, 0x08);
  ciclos(32 * 10);
  check(m.modCounter() == 10, "tabela de modulação +1: contador 10 depois de 10 passos (deu " + std::to_string(m.modCounter()) + ")");
  ciclos(32 * 60);
  check(m.modCounter() == -58, "contador da modulação dá a volta em 7 bits (deu " + std::to_string(m.modCounter()) + ")");
  m.writeReg(0x4087, 0x80);
  m.writeReg(0x4088, 4);  // gravação só com a modulação parada; 4 = volta o contador a 0
  // envoltória de volume: diminuindo, velocidade 0 -> um passo a cada 8 × 1 × $E8 = 1856 ciclos
  m.writeReg(0x4080, 0xA0);
  m.writeReg(0x4080, 0x00);
  ciclos(1856 * 5);
  check((m.cpuRead(0x4090, 0) & 0x3F) == 27, "envoltória: ganho 32 -> 27 em 5 passos (deu " + std::to_string(m.cpuRead(0x4090, 0) & 0x3F) + ")");
  m.writeReg(0x4083, 0x41);  // bit 6 para as envoltórias
  ciclos(1856 * 3);
  check((m.cpuRead(0x4090, 0) & 0x3F) == 27, "$4083.6 para a envoltória");
  m.writeReg(0x4080, 0x40);  // subindo
  m.writeReg(0x4083, 0x01);
  ciclos(1856 * 40);
  check((m.cpuRead(0x4090, 0) & 0x3F) == 32, "envoltória subindo para em 32");
  // sem disco: $4032 marca disco fora e drive não pronto
  c.setDiskSide(-1);
  check((m.cpuRead(0x4032, 0) & 7) == 7, "sem disco: $4032 = 7 nos bits baixos");
  check(c.diskSide() == -1, "ejetado: diskSide() = -1");
}

// nenhum arquivo dos outros consoles pode virar disco ou BIOS do FDS
void testeNaoRouba(const std::string& raiz) {
  namespace fs = std::filesystem;
  int vistos = 0, roubados = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(raiz, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& p = it->path();
    if (p.string().find("/fds/") != std::string::npos || p.string().find("/fds") == p.string().size() - 4) continue;
    if (!it->is_regular_file(ec)) continue;
    const auto sz = it->file_size(ec);
    if (ec || sz == 0 || sz > (8u << 20)) continue;
    std::string ext = p.extension().string();
    if (ext == ".json" || ext == ".txt" || ext == ".log" || ext == ".o" || ext == ".c" || ext == ".h" || ext == ".s") continue;
    if (vistos >= 8000) break;
    std::string n = p.filename().string();
    for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const auto d = readAll(p.string());
    vistos++;
    // discos de FDS de verdade nas outras pastas (ex.: fdsirqtests nos nes-test-roms) são FDS mesmo
    const bool ehFds = n.size() > 4 && (n.compare(n.size() - 4, 4, ".fds") == 0 || n.compare(n.size() - 3, 3, ".qd") == 0);
    if (ehFds) { check(isFdsDisk(d, n), "disco .fds de outra pasta reconhecido: " + n); continue; }
    if (isFdsDisk(d, n) || isFdsBios(d, n)) { roubados++; std::printf("  arquivo tomado como FDS: %s\n", p.c_str()); }
  }
  std::printf("  %d arquivos dos outros consoles olhados\n", vistos);
  check(vistos > 100 && roubados == 0, "nenhum arquivo dos outros consoles é tomado como disco/BIOS do FDS");
}
}  // namespace

// grava a tela num .ppm (para olhar um disco: indigo-t7_fds tela bios disco quadros saida.ppm)
int tela(char** argv) {
  const auto bios = readAll(argv[2]);
  const auto d = readAll(argv[3]);
  FdsCore c(d, argv[3], nullptr, &bios);
  const int n = std::atoi(argv[4]);
  for (int q = 0; q < n; q++) c.runFrame();
  std::FILE* o = std::fopen(argv[5], "wb");
  if (!o) return 1;
  std::fprintf(o, "P6 256 240 255\n");
  for (int i = 0; i < 256 * 240; i++) {
    const uint32_t px = c.framebuffer()[i];
    const uint8_t rgb[3] = {uint8_t(px), uint8_t(px >> 8), uint8_t(px >> 16)};
    std::fwrite(rgb, 1, 3, o);
  }
  std::fclose(o);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 6 && std::string(argv[1]) == "tela") return tela(argv);
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/fds";
  const auto bios = readAll(dir + "/testbios.rom");
  const auto disco = readAll(dir + "/teste.fds");
  const auto semcab = readAll(dir + "/teste_semcab.fds");
  const auto qd = readAll(dir + "/teste.qd");
  const auto qdRuim = readAll(dir + "/teste_crcruim.qd");
  if (bios.size() != 8192 || disco.empty() || semcab.empty() || qd.empty() || qdRuim.empty()) {
    std::printf("faltam os arquivos de teste em %s: rode tools/t7_fds.sh\n", dir.c_str());
    return 1;
  }

  std::printf("== reconhecimento\n");
  check(isFdsBios(bios, "testbios.rom"), "BIOS de teste reconhecida pelo conteúdo");
  check(!isFdsBios(std::vector<uint8_t>(8192, 0xFF), "coleco.rom"), "8 KB qualquer não é BIOS do FDS");
  check(isFdsDisk(disco, "teste.fds") && isFdsDisk(semcab, "teste_semcab.fds") && isFdsDisk(qd, "teste.qd"), "discos .fds/.qd reconhecidos");
  check(isFdsDisk(semcab, "jogo.bin"), "disco sem cabeçalho reconhecido pelo bloco 1 mesmo sem a extensão");
  {
    std::vector<uint8_t> nes = {'N', 'E', 'S', 0x1A, 2, 1, 0, 0};
    nes.resize(16 + 32768 + 8192, 0);
    check(!isFdsDisk(nes, "jogo.nes"), "ROM .nes não é disco do FDS");
    std::vector<uint8_t> falso(65500, 0);
    check(!isFdsDisk(falso, "vazio.fds"), "arquivo .fds sem o bloco 1 não é aceito");
  }
  setFirmware("fds", {});
  bool erroBios = false;
  try { createCore(disco, "teste.fds", nullptr); } catch (const std::exception& e) {
    erroBios = std::string(e.what()).find("BIOS") != std::string::npos;
  }
  check(erroBios, "sem a BIOS: erro pedindo o arquivo da BIOS");
  check(createCore(bios, "testbios.rom", nullptr) == nullptr, "a BIOS sozinha não abre como jogo");
  setFirmware("fds", bios);
  auto base = createCore(disco, "teste.fds", nullptr);
  check(base && base->system() == "fds", "createCore abre o .fds no núcleo do FDS");
  check(base && base->diskSides() == 2 && base->diskSide() == 0, "dois lados, lado A no drive");
  {
    std::vector<std::vector<uint8_t>> a, b;
    std::string e;
    rawSides(disco, false, a, e);
    rawSides(qd, true, b, e);
    bool igual = a.size() == 2 && b.size() == 2;
    for (size_t s = 0; igual && s < 2; s++)
      igual = std::equal(a[s].begin(), a[s].begin() + 0xF000, b[s].begin());
    check(igual, ".fds (CRC calculado) e .qd (CRC do arquivo) dão o mesmo disco");
  }

  std::printf("== registradores (temporizador e som)\n");
  testeRegistradores(disco, bios);

  std::printf("== partida, save e troca de lado\n");
  auto* c = dynamic_cast<FdsCore*>(base.get());
  c->setSampleRate(48000);
  const int fGrava = runUntil(*c, [&] { return estado(*c) != 0; }, 1500);
  std::printf("  BIOS de teste carregou o lado A e o jogo gravou o save em %d quadros (estado %02X, erro BIOS %02X)\n",
              fGrava, estado(*c), ram(*c, 0xFF));
  check(fGrava > 0 && estado(*c) == 1, "jogo carregado do disco e SAVE gravado de volta");
  check(ram(*c, 0x20) == 0 && ram(*c, 0x21) == 1, "arquivo SAVE lido do disco (contador 0, assinatura)");
  const int irq0 = ram(*c, 0x10) | (ram(*c, 0x11) << 8);
  check(irq0 > 250, "IRQs do temporizador contados no jogo (" + std::to_string(irq0) + ")");
  check(c->saveDirty(), "gravar no disco marca o save");
  {
    std::vector<uint8_t> dados;
    int ruins = 0;
    const bool ok = blocosDoLado(c->adapter().rawSides()[0], 2, dados, ruins);
    check(ok && dados.size() == 16 && dados[0] == 1 && std::memcmp(&dados[1], "SAVE", 4) == 0,
          "o disco tem o arquivo SAVE regravado com o contador 1");
    check(ok && ruins == 0, "todos os blocos do lado A com CRC certo depois da gravação");
  }
  const auto save = c->exportSave();
  check(!c->saveDirty() && save.size() > 7 && save.size() < 1024 && std::memcmp(save.data(), "FDSD", 4) == 0,
        "save separado com só as diferenças (" + std::to_string(save.size()) + " bytes)");
  check(disco == readAll(dir + "/teste.fds"), "o arquivo do disco não muda");
  // o jogo pede o lado B: com o lado A no drive, fica esperando
  runUntil(*c, [] { return false; }, 120);
  check(estado(*c) == 1, "com o lado errado no drive o jogo espera");
  c->setDiskSide(1);
  check(c->diskSide() == 1 && c->adapter().side() == -1, "troca: o disco sai e o lado B entra depois");
  const int fB = runUntil(*c, [&] { return estado(*c) >= 2; }, 1500);
  std::printf("  lado B carregado %d quadros depois da troca (estado %02X)\n", fB, estado(*c));
  check(estado(*c) == 3, "lado B carregado e conferido ($C000 = \"LADO B OK\")");
  check(c->adapter().side() == 1, "lado B no drive");
  // temporizador: 1000 ciclos por IRQ
  {
    runUntil(*c, [] { return false; }, 5);
    const int a = ram(*c, 0x10) | (ram(*c, 0x11) << 8);
    runUntil(*c, [] { return false; }, 60);
    const int b = ram(*c, 0x10) | (ram(*c, 0x11) << 8);
    const double esperado = 60 * 29780.5 / 1000;
    const int d = (b - a + 65536) % 65536;
    std::printf("  IRQs em 60 quadros: %d (esperado %.0f)\n", d, esperado);
    check(std::abs(d - esperado) < esperado * 0.01, "temporizador: um IRQ a cada 1000 ciclos no jogo");
  }
  // tela e som
  {
    const uint32_t* fb = c->framebuffer();
    std::vector<uint32_t> cores;
    for (int i = 0; i < 256 * 240 && cores.size() < 8; i++)
      if (std::find(cores.begin(), cores.end(), fb[i]) == cores.end()) cores.push_back(fb[i]);
    check(cores.size() >= 4, "tela desenhada com os padrões carregados do disco (" + std::to_string(cores.size()) + " cores)");
    std::vector<float> s;
    for (int f = 0; f < 60; f++) {
      c->runFrame();
      std::vector<float> l(c->audioAvailable()), r(l.size());
      c->pullAudio(l.data(), r.data(), l.size());
      s.insert(s.end(), l.begin(), l.end());
    }
    double media = 0, pico = 0;
    for (float v : s) media += v;
    media /= s.empty() ? 1 : s.size();
    int cruz = 0;
    for (size_t i = 1; i < s.size(); i++) {
      pico = std::max(pico, std::abs(s[i] - media));
      if ((s[i - 1] - media < 0) != (s[i] - media < 0)) cruz++;
    }
    const double hz = cruz / 2.0 / (s.size() / 48000.0);
    std::printf("  som: %zu amostras, pico %.3f, %.1f Hz (esperado %.1f)\n", s.size(), pico, hz, 1789773.0 * 512 / 65536 / 64);
    check(pico > 0.02, "o canal de onda toca");
    check(std::abs(hz - 218.47) < 218.47 * 0.03, "frequência da nota do canal de onda");
  }

  std::printf("== save no disco na próxima partida\n");
  {
    auto c2 = createCore(disco, "teste.fds", &save);
    auto* f2 = dynamic_cast<FdsCore*>(c2.get());
    runUntil(*f2, [&] { return estado(*f2) != 0; }, 1500);
    check(ram(*f2, 0x20) == 1 && ram(*f2, 0x21) == 1, "com o save, o jogo lê o contador 1 do disco");
    check(estado(*f2) == 1, "e grava o contador 2");
    std::vector<uint8_t> dados;
    int ruins = 0;
    blocosDoLado(f2->adapter().rawSides()[0], 2, dados, ruins);
    check(dados.size() == 16 && dados[0] == 2 && ruins == 0, "contador 2 no disco");
    std::vector<uint8_t> lixo = {'F', 'D', 'S', 'D', 1, 2, 0, 0xFF, 0xFF, 0xFF, 0x7F, 9, 0};
    auto c3 = createCore(disco, "teste.fds", &lixo);
    std::vector<std::vector<uint8_t>> orig;
    std::string e;
    rawSides(disco, false, orig, e);
    check(c3 && dynamic_cast<FdsCore*>(c3.get())->adapter().rawSides() == orig, "save danificado é ignorado (o disco fica o original)");
  }

  std::printf("== formatos\n");
  {
    auto quadros = [&](const std::vector<uint8_t>& d, const std::string& n, uint64_t& h) {
      auto k = createCore(d, n, nullptr);
      auto* f = dynamic_cast<FdsCore*>(k.get());
      const int q = runUntil(*f, [&] { return estado(*f) != 0; }, 1500);
      h = hashMem(*f);
      return q;
    };
    uint64_t h1, h2, h3;
    const int q1 = quadros(disco, "teste.fds", h1), q2 = quadros(semcab, "teste_semcab.fds", h2), q3 = quadros(qd, "teste.qd", h3);
    std::printf("  quadros até gravar: .fds %d, sem cabeçalho %d, .qd %d\n", q1, q2, q3);
    check(q1 > 0 && q1 == q2 && h1 == h2, ".fds sem cabeçalho roda igual ao com cabeçalho");
    check(q1 == q3 && h1 == h3, ".qd roda igual ao .fds");
    auto k = createCore(qdRuim, "teste_crcruim.qd", nullptr);
    auto* f = dynamic_cast<FdsCore*>(k.get());
    runUntil(*f, [] { return false; }, 400);
    check(estado(*f) == 0 && ram(*f, 0xFF) == 3, "CRC errado no disco: a BIOS acusa o erro 3 e não roda o jogo");
  }

  std::printf("== save state\n");
  {
    auto k = createCore(disco, "teste.fds", nullptr);
    auto* f = dynamic_cast<FdsCore*>(k.get());
    runUntil(*f, [] { return false; }, 45);  // no meio da carga
    const auto st = f->saveState();
    std::vector<uint64_t> a, b;
    for (int q = 0; q < 400; q++) { f->runFrame(); a.push_back(hashMem(*f)); }
    const auto discoA = f->adapter().rawSides();
    f->loadState(st);
    for (int q = 0; q < 400; q++) { f->runFrame(); b.push_back(hashMem(*f)); }
    check(a == b, "carregar o estado no meio da carga e rodar 400 quadros dá o mesmo resultado");
    check(estado(*f) == 1 && f->adapter().rawSides() == discoA, "a gravação no disco também se repete igual");
    const auto st2 = f->saveState();
    auto k2 = createCore(disco, "teste.fds", nullptr);
    auto* g = dynamic_cast<FdsCore*>(k2.get());
    g->loadState(st2);
    check(g->adapter().rawSides() == discoA && hashMem(*g) == hashMem(*f), "o estado leva o disco gravado para outro núcleo");
    bool recusou = false;
    try { g->loadState(std::vector<uint8_t>(st2.begin(), st2.begin() + 100)); } catch (const std::exception&) { recusou = true; }
    check(recusou && hashMem(*g) == hashMem(*f), "estado truncado é recusado sem estragar o jogo");
  }

  std::printf("== estado e save adulterados\n");
  {
    auto k = createCore(disco, "teste.fds", nullptr);
    auto* f = dynamic_cast<FdsCore*>(k.get());
    runUntil(*f, [] { return false; }, 30);
    const auto A = f->saveState();
    // acha disk_ no estado: a primeira diferença depois de tirar o disco (0 -> -1)
    auto k2 = createCore(disco, "teste.fds", nullptr);
    auto* g = dynamic_cast<FdsCore*>(k2.get());
    g->loadState(A);
    g->adapter().trocar(0);
    const auto B = g->saveState();
    size_t D = 0;
    while (D < A.size() && A[D] == B[D]) D++;
    check(D > 16 && D + 40 < A.size() && B[D] == 0xFF, "disk_ achado no estado");
    if (D > 16 && D + 40 < A.size()) {
      // sem disco, com o disco para entrar no próximo ciclo e a cabeça muito além do fim do lado
      auto M = B;
      auto w32 = [&](size_t o, uint32_t v) { std::memcpy(&M[o], &v, 4); };
      w32(D, 0xFFFFFFFFu); w32(D + 4, 0); w32(D + 8, 1); w32(D + 12, 0x40000000u); w32(D + 16, 0);
      M[D + 26] = 1; M[D + 27] = 0; M[D + 32] = 0;
      auto k3 = createCore(disco, "teste.fds", nullptr);
      auto* h = dynamic_cast<FdsCore*>(k3.get());
      h->loadState(M);
      for (int q = 0; q < 3; q++) h->runFrame();
      check(h->adapter().side() == 0 && h->adapter().headPos() < h->adapter().rawSides()[0].size(),
            "estado com a cabeça fora do disco: o disco entra e a cabeça fica dentro do lado");
      // pedido de IRQ do temporizador pendente (irqOn_ fica 6 bytes antes de disk_, timerIrq_ 4 bytes antes)
      auto I = A;
      I[D - 6] = 0; I[D - 4] = 1;
      auto k4 = createCore(disco, "teste.fds", nullptr);
      auto* r = dynamic_cast<FdsCore*>(k4.get());
      r->adapter().irq = false;
      r->loadState(I);
      check(r->adapter().irq, "IRQ pendente no estado volta ligado na linha do mapeador");
    }
    // save de outro disco com o mesmo número de lados é recusado
    runUntil(*f, [] { return false; }, 400);
    const auto sv = f->exportSave();
    check(sv.size() > 11, "o jogo gravou no disco antes do teste do save");
    auto outro = disco;
    outro[outro.size() - 1] ^= 0x5A;  // fim do último lado (área vazia)
    auto k5 = createCore(outro, "outro.fds", &sv);
    auto* o = dynamic_cast<FdsCore*>(k5.get());
    auto k6 = createCore(outro, "outro.fds", nullptr);
    check(o->adapter().rawSides() == dynamic_cast<FdsCore*>(k6.get())->adapter().rawSides(),
          "save de outro disco com o mesmo número de lados é ignorado");
    auto k7 = createCore(disco, "teste.fds", &sv);
    check(dynamic_cast<FdsCore*>(k7.get())->adapter().rawSides() == f->adapter().rawSides(),
          "o save volta no disco certo");
  }

  std::printf("== régua: Mesen 2 com a mesma BIOS e o mesmo disco\n");
  if (std::ifstream ref(dir + "/ref/mesen.txt"); ref) {
    // mesma sequência do t7_fds_mesen.cc: 120 quadros depois do SAVE gravado tira o disco, 37 depois põe o lado B
    std::vector<std::string> esperado;
    for (std::string l; std::getline(ref, l);) if (!l.empty()) esperado.push_back(l);
    auto k = createCore(disco, "teste.fds", nullptr);
    auto* f = dynamic_cast<FdsCore*>(k.get());
    int e1 = -1, iguais = 0, perto = 0, primeira = -1;
    for (int q = 0; q < static_cast<int>(esperado.size()); q++) {
      f->runFrame();
      const int v[5] = {q + 1, ram(*f, 0x30), ram(*f, 0x10) | (ram(*f, 0x11) << 8), ram(*f, 0x12), ram(*f, 0xFF)};
      int w[5] = {};
      std::sscanf(esperado[q].c_str(), "%d %d %d %d %d", &w[0], &w[1], &w[2], &w[3], &w[4]);
      if (std::getenv("FDS_REF_DUMP")) std::printf("  %d %d %d %d %d | %s\n", v[0], v[1], v[2], v[3], v[4], esperado[q].c_str());
      const bool resto = v[0] == w[0] && v[1] == w[1] && v[3] == w[3] && v[4] == w[4];
      if (resto && v[2] == w[2]) iguais++;
      if (resto && std::abs(v[2] - w[2]) <= 1) perto++;
      else if (primeira < 0) { primeira = q; std::printf("  primeira diferença no quadro %d: Mesen \"%s\"\n", q + 1, esperado[q].c_str()); }
      if (v[1] == 1 && e1 < 0) e1 = q;
      if (e1 >= 0 && q == e1 + 120) f->setDiskSide(-1);
      if (e1 >= 0 && q == e1 + 120 + 37) f->adapter().inserirJa(1);
    }
    std::printf("  %zu quadros: %d idênticos; nos outros, só a contagem de IRQs difere em 1 na virada do quadro\n", esperado.size(), iguais);
    check(perto == static_cast<int>(esperado.size()),
          "quadro a quadro como o Mesen 2: estado do jogo, NMIs e erro da BIOS iguais, IRQs do temporizador com diferença de no máximo 1");
  } else {
    std::printf("  (sem ref/mesen.txt: régua pulada)\n");
  }

  std::printf("== outros consoles\n");
  testeNaoRouba(std::filesystem::path(dir).parent_path().string());

  std::printf("RESULTADO fds: %d verificações, %d falhas\n", okN + badN, badN);
  return badN ? 1 : 0;
}
