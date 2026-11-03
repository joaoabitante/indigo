// Testes do Sega Saturn: SCU (interrupções, DMA, DSP), SMPC, VDP1, VDP2, SCSP com o 68000, bloco de CD com disco
// sintético, save state e, se existirem, a BIOS de teste do Índigo com o CD homebrew próprio (t7_saturn.sh) e a
// BIOS do usuário.
//   indigo-t7_saturn pasta   (pasta = /root/indigo-env/roms/saturn, montada pelo t7_saturn.sh)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/saturn/saturn.h"

using namespace indigo;
using namespace indigo::saturn;

static int fails = 0, checks = 0, skipped = 0;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    checks++;                                               \
    if (!(cond)) {                                          \
      fails++;                                              \
      std::printf("  FALHA: " __VA_ARGS__);                 \
      std::printf("\n");                                    \
    }                                                       \
  } while (0)

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t fnv(const uint32_t* p, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

// BIOS mínima montada aqui (sem arquivo): vetores apontando para um laço "bra $; nop" em 0x400
static std::vector<uint8_t> tinyBios() {
  std::vector<uint8_t> b(0x80000, 0);
  auto put32 = [&](size_t o, uint32_t v) { b[o] = uint8_t(v >> 24); b[o + 1] = uint8_t(v >> 16); b[o + 2] = uint8_t(v >> 8); b[o + 3] = uint8_t(v); };
  put32(0, 0x400); put32(4, 0x06002000); put32(8, 0x400); put32(12, 0x06002000);
  b[0x400] = 0xAF; b[0x401] = 0xFE; b[0x402] = 0x00; b[0x403] = 0x09;  // bra $ ; nop
  const char* s = "SEGA SEGASATURN Indigo Saturn (teste)";
  std::memcpy(&b[0x1000], s, std::strlen(s));
  return b;
}

// ================================================================================================ disco sintético

// CD do Saturn montado na memória: IP.BIN, ISO 9660 com 1ST_READ.BIN e DADOS.BIN e uma faixa de áudio
class SatDisc : public cdrom::Disc {
public:
  explicit SatDisc(uint32_t audioSectors = 75, const char* areas = "JTUBKAEL") {
    data_.assign(40 * 2048, 0);
    auto put = [&](size_t o, const char* s) { std::memcpy(&data_[o], s, std::strlen(s)); };
    put(0, "SEGA SEGASATURN ");
    put(0x40, areas);
    put(0x60, "DISCO SINTETICO");
    for (int i = 0x6F; i < 0xD0; i++) data_[size_t(i)] = ' ';
    be32(0xF0, 0x06004000);
    // PVD no setor 16, raiz no 20, arquivos no 21 (1 setor) e 22 (2 setores)
    uint8_t* p = &data_[16 * 2048];
    p[0] = 1; std::memcpy(p + 1, "CD001", 5); p[6] = 1;
    rec(p + 156, 0, 20, 2048, 2);
    uint8_t* r = &data_[20 * 2048];
    size_t o = 0;
    o += rec(r + o, 0, 20, 2048, 2);
    o += rec(r + o, 1, 20, 2048, 2);
    o += rec(r + o, 2, 21, 1000, 0, "1ST_READ.BIN;1");
    o += rec(r + o, 3, 22, 4096, 0, "DADOS.BIN;1");
    for (int i = 0; i < 1000; i++) data_[21 * 2048 + size_t(i)] = uint8_t(i ^ 0x5A);
    for (int i = 0; i < 4096; i++) data_[22 * 2048 + size_t(i)] = uint8_t(i * 7 + (i >> 8));
    tracks_.push_back({1, cdrom::TrackType::Mode1, 0, 0, 40});
    if (audioSectors) tracks_.push_back({2, cdrom::TrackType::Audio, 40 + 150, 150, audioSectors});
    total_ = 40 + (audioSectors ? 150 + audioSectors : 0);
    name_ = "sintetico";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, cdrom::RAW_SECTOR);
    if (lba >= total_) return false;
    if (lba < 40) {
      std::memset(out + 1, 0xFF, 10);
      const uint32_t a = lba + 150;
      auto bcd = [](uint32_t v) { return uint8_t(((v / 10) << 4) | (v % 10)); };
      out[12] = bcd(a / 4500); out[13] = bcd(a / 75 % 60); out[14] = bcd(a % 75); out[15] = 1;
      std::memcpy(out + 16, &data_[size_t(lba) * 2048], 2048);
    } else if (lba >= 190) {
      for (int i = 0; i < 588; i++) {  // onda quadrada: +8000 / -8000 a cada 50 amostras
        const int16_t v = ((i / 50) & 1) ? -8000 : 8000;
        out[i * 4] = uint8_t(v); out[i * 4 + 1] = uint8_t(v >> 8); out[i * 4 + 2] = uint8_t(v); out[i * 4 + 3] = uint8_t(v >> 8);
      }
    }
    return true;
  }

private:
  void be32(size_t o, uint32_t v) { data_[o] = uint8_t(v >> 24); data_[o + 1] = uint8_t(v >> 16); data_[o + 2] = uint8_t(v >> 8); data_[o + 3] = uint8_t(v); }
  static size_t rec(uint8_t* d, int kind, uint32_t lba, uint32_t size, uint8_t flags, const char* name = nullptr) {
    const size_t nl = name ? std::strlen(name) : 1;
    const size_t len = 33 + nl + ((nl & 1) ? 0 : 1);
    std::memset(d, 0, len);
    d[0] = uint8_t(len);
    for (int k = 0; k < 4; k++) { d[2 + k] = uint8_t(lba >> (8 * k)); d[9 - k] = uint8_t(lba >> (8 * k)); }
    for (int k = 0; k < 4; k++) { d[10 + k] = uint8_t(size >> (8 * k)); d[17 - k] = uint8_t(size >> (8 * k)); }
    d[25] = flags;
    d[32] = uint8_t(nl);
    if (name) std::memcpy(d + 33, name, nl); else d[33] = uint8_t(kind);
    return len;
  }
  std::vector<uint8_t> data_;
};

// ================================================================================================ ajudantes

struct Rig {
  std::vector<uint8_t> bios = tinyBios();
  SatDisc disc;
  std::unique_ptr<System> sp = std::make_unique<System>(bios, &disc);  // grande demais para a pilha
  System& s = *sp;
  void w16(uint32_t a, uint16_t v) { s.busWrite(a, 2, v); }
  void w32(uint32_t a, uint32_t v) { s.busWrite(a, 4, v); }
  void w8(uint32_t a, uint8_t v) { s.busWrite(a, 1, v); }
  uint32_t r32(uint32_t a) { return s.busRead(a, 4); }
  uint16_t r16(uint32_t a) { return uint16_t(s.busRead(a, 2)); }
  uint8_t r8(uint32_t a) { return uint8_t(s.busRead(a, 1)); }
  // comando do bloco de CD; devolve CR1..CR4
  // (o bloco responde depois de alguns microssegundos: roda até o CMOK)
  std::vector<uint16_t> cd(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    w16(0x0589000C, 0);
    w16(0x05890008, uint16_t(~1u));
    w16(0x05890018, a); w16(0x0589001C, b); w16(0x05890020, c); w16(0x05890024, d);
    for (int i = 0; i < 100 && !(r16(0x05890008) & 1); i++) s.cd.run(100);
    return {r16(0x05890018), r16(0x0589001C), r16(0x05890020), r16(0x05890024)};
  }
  void cdRun(int lines) { for (int i = 0; i < lines; i++) s.cd.run(System::CYCLES_PER_LINE); }
  // roda o bloco de CD até um bit do HIRQ (devolve as linhas gastas, -1 se não veio)
  int cdUntil(uint16_t bits, int maxLines) {
    for (int i = 0; i < maxLines; i++) { if ((r16(0x05890008) & bits) == bits) return i; s.cd.run(System::CYCLES_PER_LINE); }
    return -1;
  }
  void cdClear(uint16_t bits) { w16(0x05890008, uint16_t(~bits)); }
  // liga o motor (1 s) e autentica o disco
  void cdReady() {
    cdRun(int(1.3 * System::CPU_HZ / System::CYCLES_PER_LINE));
    cdClear(0x200);
    cd(0xE000, 0, 0, 0);
    cdUntil(0x200, 5000);
  }
};

// ================================================================================================ detecção

static void testDetect() {
  const auto b = tinyBios();
  CHECK(isSaturnBios(b), "BIOS mínima reconhecida");
  std::vector<uint8_t> junk(0x80000, 0x5A);
  CHECK(!isSaturnBios(junk), "lixo de 512 KB não é BIOS");
  std::vector<uint8_t> small(b.begin(), b.begin() + 0x40000);
  CHECK(!isSaturnBios(small), "tamanho errado não é BIOS");
  auto mips = b;
  mips[0] = 0x3C; mips[1] = 0x08; mips[2] = 0x00; mips[3] = 0x13;  // primeira instrução de uma BIOS de PS1
  CHECK(!isSaturnBios(mips), "vetor inválido não é BIOS");
  CHECK(createCore(b, "bios.bin", nullptr) == nullptr, "a BIOS aberta como ROM não vira jogo");
  SatDisc d;
  CHECK(cdrom::detectSystem(d) == "saturn", "detectSystem do disco sintético");
  CHECK(isSaturnDisc(d), "isSaturnDisc");
  setFirmware("saturn", {});
  bool threw = false;
  try { SaturnCore c(std::make_unique<SatDisc>(), nullptr); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "sem BIOS o núcleo pede o arquivo do console");
  SaturnCore c(std::make_unique<SatDisc>(), nullptr, &b);
  CHECK(c.title() == "DISCO SINTETICO", "título do IP.BIN (\"%s\")", c.title().c_str());
  CHECK(c.system() == "saturn" && c.hasSave(), "sistema e save");
  CHECK(c.sys().area == 4 && !c.sys().pal && c.fps() > 59.8 && c.fps() < 60, "disco de todas as regiões: EUA, 60 Hz (%.2f)", c.fps());
  SaturnCore e(std::make_unique<SatDisc>(75, "E"), nullptr, &b);
  CHECK(e.sys().area == 0xC && e.sys().pal && e.fps() > 49.9 && e.fps() < 50.5, "disco europeu: PAL, 50 Hz (%.2f)", e.fps());
  CHECK(e.sys().busRead(0x05F80004, 2) & 1, "TVSTAT avisa PAL");
  SaturnCore j(std::make_unique<SatDisc>(75, "J"), nullptr, &b);
  CHECK(j.sys().area == 1 && !j.sys().pal, "disco japonês: área 1");
}

// ================================================================================================ SCU

static void testScuInterrupts() {
  Rig r;
  Scu& scu = r.s.scu;
  r.w32(0x05FE00A0, 0xBFFF & ~(1u << INT_VBIN) & ~(1u << INT_TIMER0));
  scu.raise(INT_TIMER0);
  CHECK(r.s.master.irl() == 12, "temporizador 0 no nível 12 (%d)", r.s.master.irl());
  scu.raise(INT_VBIN);
  CHECK(r.s.master.irl() == 12, "a interrupção apresentada não muda até ser aceita");
  CHECK(scu.masterVector() == 0x43, "vetor do temporizador 0");
  CHECK(r.s.master.irl() == 0, "depois de aceita, a SCU mascara tudo");
  r.w32(0x05FE00A0, 0xBFFF & ~(1u << INT_VBIN) & ~(1u << INT_TIMER0));
  CHECK(r.s.master.irl() == 15, "V-blank no nível 15 ao reescrever IMS (%d)", r.s.master.irl());
  CHECK(scu.masterVector() == 0x40, "vetor do V-blank");
  scu.raise(INT_HBIN);
  CHECK(r.r32(0x05FE00A4) & (1u << INT_HBIN), "fonte mascarada fica em IST");
  CHECK(r.s.master.irl() == 0, "fonte mascarada não interrompe");
  r.w32(0x05FE00A4, ~(1u << INT_HBIN));
  CHECK(!(r.r32(0x05FE00A4) & (1u << INT_HBIN)), "escrever 0 em IST apaga");
  // prioridade entre fontes pendentes: o DMA 0 (nível 5) perde para o SMPC (nível 8)
  scu.raise(INT_DMA0);
  scu.raise(INT_SMPC);
  r.w32(0x05FE00A0, 0xBFFF & ~(1u << INT_DMA0) & ~(1u << INT_SMPC));
  CHECK(r.s.master.irl() == 8 && scu.masterVector() == 0x47, "SMPC antes do DMA 0");
  r.w32(0x05FE00A0, 0xBFFF & ~(1u << INT_DMA0) & ~(1u << INT_SMPC));
  CHECK(r.s.master.irl() == 5 && scu.masterVector() == 0x4B, "depois o DMA 0");
  // o SH-2 aceita de verdade: vetor da SCU na tabela do VBR
  r.w32(0x06000000 + 0x40 * 4, 0x06001000);
  r.w16(0x06001000, 0xAFFE); r.w16(0x06001002, 0x0009);
  r.s.master.vbr = 0x06000000;
  r.s.master.sr = 0;
  r.s.master.r[15] = 0x06002000;
  r.w32(0x05FE00A0, 0xBFFF & ~1u);
  scu.raise(INT_VBIN);
  r.s.master.run(r.s.master.clock + 100);
  CHECK((r.s.master.pc & 0x0FFFFFFF) == 0x06001000, "o SH-2 mestre foi para o vetor 0x40 (PC 0x%08X)", r.s.master.pc);
  CHECK(((r.s.master.sr >> 4) & 15) == 15, "máscara do SR = nível 15");
}

static void testScuDma() {
  Rig r;
  for (int i = 0; i < 256; i++) r.w8(0x06010000 + uint32_t(i), uint8_t(i * 5 + 3));
  r.w32(0x05FE0000, 0x06010000);
  r.w32(0x05FE0004, 0x00200000);
  r.w32(0x05FE0008, 256);
  r.w32(0x05FE000C, 0x101);
  r.w32(0x05FE0014, 7);
  r.w32(0x05FE0010, 0x101);
  bool ok = true;
  for (int i = 0; i < 256; i++) ok &= r.r8(0x00200000 + uint32_t(i)) == uint8_t(i * 5 + 3);
  CHECK(ok, "DMA nível 0: RAM alta -> RAM baixa");
  CHECK(!(r.s.scu.pending() & (1u << INT_DMA0)) && (r.r32(0x05FE007C) & 0x10), "DMA 0 em andamento: DSTA (D0MV) e sem fim em IST");
  r.s.scu.tick(400);
  CHECK(r.s.scu.pending() & (1u << INT_DMA0) && !(r.r32(0x05FE007C) & 0x10), "fim do DMA 0 em IST depois do tempo da transferência");
  // escrita sem avanço (D?AD = 0x100): todas as palavras no mesmo endereço do VDP2
  r.w32(0x05FE0020, 0x06010000);
  r.w32(0x05FE0024, 0x05E00100);
  r.w32(0x05FE0028, 8);
  r.w32(0x05FE002C, 0x100);
  r.w32(0x05FE0034, 7);
  r.w32(0x05FE0030, 0x101);
  CHECK(r.r16(0x05E00100) == uint16_t(r.r8(0x06010006) << 8 | r.r8(0x06010007)), "DMA com passo 0 no barramento B");
  CHECK(r.r16(0x05E00102) == 0, "passo 0 não avança");
  // indireto (nível 2): duas entradas, a última com o bit 31
  for (int i = 0; i < 64; i++) r.w8(0x06020000 + uint32_t(i), uint8_t(0x80 + i));
  r.w32(0x06030000, 32); r.w32(0x06030004, 0x05E10000); r.w32(0x06030008, 0x06020000);
  r.w32(0x0603000C, 32); r.w32(0x06030010, 0x00210000); r.w32(0x06030014, 0x80000000u | 0x06020020);
  r.w32(0x05FE0044, 0x06030000);
  r.w32(0x05FE004C, 0x101);
  r.w32(0x05FE0054, 0x01000007);
  r.w32(0x05FE0050, 0x101);
  ok = true;
  for (int i = 0; i < 32; i++) ok &= r.r8(0x05E10000 + uint32_t(i)) == uint8_t(0x80 + i) && r.r8(0x00210000 + uint32_t(i)) == uint8_t(0xA0 + i);
  CHECK(ok, "DMA indireto com duas entradas");
  r.s.scu.tick(1 << 20);
  CHECK(r.s.scu.pending() & (1u << INT_DMA2), "fim do DMA 2");
  {  // tempo: 4 KB para o SCSP (13 ciclos por palavra) demora muito mais que para o VDP2 (1 ciclo)
    auto tempo = [&](uint32_t dst) {
      r.w32(0x05FE0000, 0x06010000); r.w32(0x05FE0004, dst); r.w32(0x05FE0008, 4096);
      r.w32(0x05FE000C, 0x102); r.w32(0x05FE0014, 7); r.w32(0x05FE0010, 0x101);
      int c = 0;
      while (r.s.scu.dmaActive(0) && c < 1000000) { r.s.scu.tick(16); c += 16; }
      return c;
    };
    const int vdp = tempo(0x05E20000), som = tempo(0x05A40000);
    CHECK(vdp > 2000 && vdp < 8000 && som > vdp * 4, "tempo do DMA: VDP2 %d ciclos, SCSP %d", vdp, som);
  }
  // partida pelo V-blank: nada até o evento
  r.w32(0x05FE0000, 0x06010000); r.w32(0x05FE0004, 0x00220000); r.w32(0x05FE0008, 4);
  r.w32(0x05FE000C, 0x101); r.w32(0x05FE0014, DF_VBIN); r.w32(0x05FE0010, 0x100);
  CHECK(r.r32(0x00220000) == 0, "DMA do V-blank espera o evento");
  r.s.scu.factor(DF_VBIN);
  CHECK(r.r32(0x00220000) == r.r32(0x06010000), "DMA do V-blank feito no evento");
}

static void dspLoad(Rig& r, const std::vector<uint32_t>& prog) {
  r.w32(0x05FE0080, 0x8000);
  for (uint32_t op : prog) r.w32(0x05FE0084, op);
  r.w32(0x05FE0080, 0x8000);
  r.w32(0x05FE0080, 0x10000);
  r.s.scu.dspRun(2000);
}

static void testScuDsp() {
  {
    Rig r;
    dspLoad(r, {0x80000005u, 0x84000007u, 0x00001C00u, 0x00001D00u, 0x01960000u, 0x10040000u, 0x00003209u,
                0x9D824080u, 0x00001E00u, 0xC0011201u, 0xF8000000u});
    CHECK(r.r32(0x06090200) == 12, "DSP: 5 + 7 com DMA para a RAM alta (%u)", r.r32(0x06090200));
    CHECK(!(r.r32(0x05FE0080) & 0x10000), "DSP parou no ENDI");
    CHECK(r.s.scu.pending() & (1u << INT_DSP), "ENDI liga a interrupção do DSP");
  }
  {
    Rig r;  // LPS: a instrução seguinte roda LOP+1 vezes
    dspLoad(r, {0x00001C00u, 0x00001A03u, 0xE8000000u, 0x00001001u, 0xF0000000u});
    const auto& d = r.s.scu.dsp;
    CHECK(d.data[0][0] == 1 && d.data[0][3] == 1 && d.data[0][4] == 0 && d.ct[0] == 4, "DSP: LPS repete 4 vezes (CT0 %d)", d.ct[0]);
  }
  {
    Rig r;  // JMP com a instrução seguinte executada antes do destino
    dspLoad(r, {0xD0000003u, 0x80000001u, 0x84000002u, 0xF0000000u});
    const auto& d = r.s.scu.dsp;
    CHECK(d.data[0][0] == 1 && d.data[1][0] == 0, "DSP: JMP com atraso de uma instrução");
  }
  {
    Rig r;  // laço com BTM: soma 1 dez vezes em A
    // 0 CLR A; 1 MOV #1,PL... (P = 1); 2 MOV #9,LOP; 3 MOV #4,TOP; 4 ADD/MOV ALU,A; 5 BTM; 6 NOP; 7 MOV ALL,MC0; 8 END
    dspLoad(r, {0x00020000u, 0x00001501u, 0x00001A09u, 0x00001B04u, 0x10040000u, 0xE0000000u, 0x00000000u,
                0x00003009u, 0xF0000000u});
    CHECK(r.s.scu.dsp.data[0][0] == 10, "DSP: BTM soma 10 vezes (%u)", r.s.scu.dsp.data[0][0]);
  }
  {
    Rig r;  // porta de dados com o DSP parado
    r.w32(0x05FE0088, 0x40 + 5);
    r.w32(0x05FE008C, 0xCAFEBABE);
    r.w32(0x05FE0088, 0x40 + 5);
    CHECK(r.r32(0x05FE008C) == 0xCAFEBABE && r.s.scu.dsp.data[1][5] == 0xCAFEBABE, "DSP: RAM de dados pela porta");
  }
}

// ================================================================================================ SMPC

static void smpcCmd(Rig& r, uint8_t c) {
  r.w8(0x00100063, 1);
  r.w8(0x0010001F, c);
}

static void testSmpc() {
  Rig r;
  r.w8(0x00100001, 0x01); r.w8(0x00100003, 0x00); r.w8(0x00100005, 0xF0);
  smpcCmd(r, 0x10);
  CHECK(r.r8(0x00100063) == 0, "INTBACK termina (SF = 0)");
  CHECK(r.r8(0x0010005F) == 0x10, "OREG31 = comando");
  CHECK(r.r8(0x00100061) == 0x0F, "SR do estado (0x%02X)", r.r8(0x00100061));
  CHECK(r.r8(0x00100021) == 0x80 && r.r8(0x00100023) == 0x20, "estado: relógio válido e ano (0x%02X)", r.r8(0x00100023));
  CHECK(r.s.scu.pending() & (1u << INT_SMPC), "INTBACK liga a interrupção do SMPC");
  r.s.setPad(0x0800 | 0x0100);  // Start e B
  r.w8(0x00100001, 0x00); r.w8(0x00100003, 0x0A);
  smpcCmd(r, 0x10);
  CHECK(r.r8(0x00100021) == 0xF1 && r.r8(0x00100023) == 0x02, "controle padrão na porta 1");
  CHECK(r.r8(0x00100025) == 0xF6, "botões ativos em 0 (0x%02X)", r.r8(0x00100025));
  // estado e controle juntos: os dados do controle vêm depois do "continue"
  r.w8(0x00100001, 0x01); r.w8(0x00100003, 0x08);
  smpcCmd(r, 0x10);
  CHECK((r.r8(0x00100061) & 0x20) && r.r8(0x00100021) == 0x80, "INTBACK avisa que há mais dados");
  r.w8(0x00100001, 0x80);
  CHECK(r.r8(0x00100021) == 0xF1, "continue entrega o controle");
  smpcCmd(r, 0x02);
  CHECK(r.s.smpc.slaveOn && r.s.slave.pc == 0x400, "SSHON liga o escravo no vetor de reset");
  smpcCmd(r, 0x03);
  CHECK(!r.s.smpc.slaveOn, "SSHOFF desliga o escravo");
  smpcCmd(r, 0x06);
  CHECK(r.s.scsp.cpuOn, "SNDON liga o 68000");
  r.w8(0x00100001, 0x12); r.w8(0x00100003, 0x34); r.w8(0x00100005, 0x56); r.w8(0x00100007, 0x78);
  smpcCmd(r, 0x17);
  r.w8(0x00100001, 0x01); r.w8(0x00100003, 0x00);
  smpcCmd(r, 0x10);
  CHECK(r.r8(0x00100039) == 0x12 && r.r8(0x0010003F) == 0x78, "SETSMEM guarda a memória do SMPC");
}

// ================================================================================================ VDP1

static void cmd(Rig& r, int n, std::initializer_list<uint16_t> w) {
  int k = 0;
  for (uint16_t v : w) r.w16(0x05C00000 + uint32_t(n) * 32 + uint32_t(k++) * 2, v);
  for (; k < 16; k++) r.w16(0x05C00000 + uint32_t(n) * 32 + uint32_t(k) * 2, 0);
}

static uint16_t px1(Rig& r, int x, int y) { return r.s.vdp1.fb[r.s.vdp1.drawBuf][y * 512 + x]; }

static void testVdp1() {
  Rig r;
  auto& v = r.s.vdp1;
  // texturas: 4 bits 8x2 em 0x1000 (pontos 0..15), 8 bits 8x1 em 0x2000, RGB 2x2 em 0x3000, tabela em 0x4000
  for (int i = 0; i < 8; i++) r.w8(0x05C01000 + uint32_t(i), uint8_t((i * 2) << 4 | (i * 2 + 1)));
  for (int i = 0; i < 8; i++) r.w8(0x05C02000 + uint32_t(i), uint8_t(i * 30));
  for (int i = 0; i < 16; i++) r.w16(0x05C03000 + uint32_t(i) * 2, i == 0 ? 0x801F : i == 1 ? 0x83E0 : i == 8 ? 0xFC00 : 0);
  for (int i = 0; i < 16; i++) r.w16(0x05C04000 + uint32_t(i) * 2, uint16_t(0x8000 | i * 0x421));
  cmd(r, 0, {0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 319, 223});
  cmd(r, 1, {0x000A, 0, 0, 0, 0, 0, 0, 0});
  cmd(r, 2, {0x0000, 0, 0x0000, 0x0510, 0x1000 / 8, 0x0102, 10, 10});              // 4 bits banco 0x510
  cmd(r, 3, {0x0000, 0, 0x0008, 0x4000 / 8, 0x1000 / 8, 0x0102, 10, 20});          // 4 bits tabela
  cmd(r, 4, {0x0000, 0, 0x0020, 0x0300, 0x2000 / 8, 0x0101, 10, 30});              // 8 bits 256 cores
  cmd(r, 5, {0x0010, 0, 0x0028, 0, 0x3000 / 8, 0x0102, 30, 10});                  // RGB espelhado
  cmd(r, 6, {0x0004, 0, 0x00C0, 0xABCD, 0, 0, 100, 100, 139, 100, 139, 139, 100, 139});  // polígono 40x40
  cmd(r, 7, {0x0006, 0, 0x00C0, 0x8421, 0, 0, 50, 60, 60, 60});                   // linha
  cmd(r, 8, {0x8000});
  r.w16(0x05C80000 + (10 * 512 + 10) * 2, 0x7777);  // ponto 0 da textura é transparente: fica o que havia
  v.draw();
  CHECK(px1(r, 10, 10) == 0x7777, "ponto 0 transparente");
  CHECK(px1(r, 11, 10) == 0x0511 && px1(r, 17, 10) == 0x0517 && px1(r, 10, 11) == 0x0518, "4 bits com banco");
  CHECK(px1(r, 11, 20) == 0x8421 && px1(r, 16, 21) == (0x8000 | 14 * 0x421), "4 bits com tabela de cores (0x%04X 0x%04X)", px1(r, 11, 20), px1(r, 16, 21));
  CHECK(px1(r, 17, 21) == 0, "código de fim (0xF) não é desenhado");
  CHECK(px1(r, 11, 30) == 0x031E && px1(r, 17, 30) == 0x03D2, "8 bits com 256 cores (0x%04X)", px1(r, 17, 30));
  CHECK(px1(r, 37, 10) == 0x801F && px1(r, 36, 10) == 0x83E0 && px1(r, 30, 10) == 0x0000 && px1(r, 37, 11) == 0xFC00, "RGB espelhado e 0 transparente");
  int in = 0, out = 0;
  for (int y = 90; y < 150; y++)
    for (int x = 90; x < 150; x++) {
      const bool inside = x >= 100 && x <= 139 && y >= 100 && y <= 139;
      if (px1(r, x, y) == 0xABCD) { if (inside) in++; else out++; }
    }
  CHECK(in == 1600 && out == 0, "polígono 40x40 cheio (%d dentro, %d fora)", in, out);
  CHECK(px1(r, 50, 60) == 0x8421 && px1(r, 60, 60) == 0x8421 && px1(r, 55, 60) == 0x8421, "linha horizontal");
  CHECK(v.commandsRun == 8, "comandos executados (%d)", v.commandsRun);
  // o desenho leva tempo: o fim (CEF e a interrupção) só vem depois dos ciclos calculados
  const int64_t custo = v.lastCost();
  CHECK(v.drawing() && !(r.r16(0x05D00010) & 2) && !(r.s.scu.pending() & (1u << INT_VDP1)), "desenho em andamento logo depois de começar");
  CHECK(custo > 40 * 52 && custo < 40 * 52 * 2 + 2000, "tempo de desenho perto de 1 ciclo por ponto (%lld ciclos)", (long long)custo);
  v.run(int(custo - 10));
  CHECK(v.drawing() && !(r.r16(0x05D00010) & 2), "antes do tempo o desenho não terminou");
  v.run(20);
  CHECK(r.r16(0x05D00010) & 2, "EDSR: fim do desenho");
  CHECK(r.s.scu.pending() & (1u << INT_VDP1), "interrupção do fim do desenho");
  {  // meia transparência lê o quadro: o mesmo polígono custa bem mais; ENDR interrompe sem interrupção
    Rig t;
    cmd(t, 0, {0x0004, 0, 0x00C0, 0x801F, 0, 0, 0, 0, 39, 0, 39, 39, 0, 39});
    cmd(t, 1, {0x8000});
    t.s.vdp1.draw();
    const int64_t simples = t.s.vdp1.lastCost();
    cmd(t, 0, {0x0004, 0, 0x00C3, 0x801F, 0, 0, 0, 0, 39, 0, 39, 39, 0, 39});
    t.s.vdp1.draw();
    const int64_t meia = t.s.vdp1.lastCost();
    CHECK(meia > simples * 4, "meia transparência custa mais (%lld x %lld ciclos)", (long long)meia, (long long)simples);
    t.w16(0x05D0000C, 0);
    t.s.vdp1.run(1 << 20);
    CHECK(!t.s.vdp1.drawing() && !(t.r16(0x05D00010) & 2) && !(t.s.scu.pending() & (1u << INT_VDP1)), "ENDR interrompe o desenho");
  }

  // recortes, coordenadas locais, cálculo de cor, malha e saltos
  Rig q;
  cmd(q, 0, {0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 99, 99});                         // recorte do sistema 99x99
  cmd(q, 1, {0x0008, 0, 0, 0, 0, 0, 20, 20, 0, 0, 29, 29});                       // recorte do usuário
  cmd(q, 2, {0x000A, 0, 0, 0, 0, 0, 5, 5});                                       // locais (5,5)
  cmd(q, 3, {0x0004, 0, 0x04C0, 0x801F, 0, 0, 0, 0, 39, 0, 39, 39, 0, 39});       // só dentro do recorte do usuário
  cmd(q, 4, {0x0004, 0, 0x06C0, 0x83E0, 0, 0, 40, 40, 59, 40, 59, 59, 40, 59});   // fora do recorte do usuário
  cmd(q, 5, {0x0004, 0, 0x00C3, 0xFC00, 0, 0, 10, 10, 19, 10, 19, 19, 10, 19});   // meia transparência sobre o vermelho
  cmd(q, 6, {0x0004, 0, 0x01C0, 0xFFFF, 0, 0, 60, 0, 69, 0, 69, 9, 60, 9});       // malha
  cmd(q, 7, {0x1004, 9 * 4, 0x00C0, 0x801F, 0, 0, 80, 80, 90, 80, 90, 90, 80, 90});   // salta para 9 (desenha este)
  cmd(q, 8, {0x0004, 0, 0x00C0, 0x83E0, 0, 0, 80, 0, 90, 0, 90, 10, 80, 10});     // pulado
  cmd(q, 9, {0x4004, 0, 0x00C0, 0x83E0, 0, 0, 0, 80, 10, 80, 10, 90, 0, 90});     // pulado pelo bit SKIP
  cmd(q, 10, {0x0004, 0, 0x00C0, 0x8000 | 0x3DEF, 0, 0, 200, 0, 210, 0, 210, 10, 200, 10});  // fora do recorte do sistema
  cmd(q, 11, {0x8000});
  q.s.vdp1.draw();
  CHECK(px1(q, 25, 25) == 0x801F && px1(q, 29, 29) == 0x801F && px1(q, 30, 30) == 0 && px1(q, 19, 25) == 0, "recorte do usuário (dentro)");
  CHECK(px1(q, 50, 50) == 0x83E0 && px1(q, 25, 34) == 0, "recorte do usuário (fora)");
  CHECK(px1(q, 64, 64) == 0x83E0 && px1(q, 44, 44) == 0, "coordenadas locais somadas");
  CHECK(px1(q, 22, 22) == (0x8000 | 15 << 10 | 15) && px1(q, 15, 15) == 0xFC00, "meia transparência: mistura sobre RGB, cor cheia no vazio");
  CHECK(px1(q, 66, 6) == 0xFFFF && px1(q, 67, 6) == 0, "malha: só pontos pares");
  CHECK(px1(q, 85, 85) == 0x801F && px1(q, 85, 5) == 0 && px1(q, 5, 85) == 0, "salto e comando pulado (0x%04X)", px1(q, 85, 5));
  CHECK(px1(q, 99, 99) == 0 && px1(q, 100, 5) == 0, "recorte do sistema");
  // meia transparência sobre RGB: média por canal
  Rig h;
  cmd(h, 0, {0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 319, 223});
  cmd(h, 1, {0x0004, 0, 0x00C0, 0x801F, 0, 0, 0, 0, 9, 0, 9, 9, 0, 9});
  cmd(h, 2, {0x0004, 0, 0x00C3, 0xFC00, 0, 0, 5, 5, 14, 5, 14, 14, 5, 14});
  cmd(h, 3, {0x0004, 0, 0x00C1, 0x0000, 0, 0, 0, 12, 4, 12, 4, 16, 0, 12});
  cmd(h, 4, {0x0004, 0, 0x80C0, 0x0000, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1});
  cmd(h, 5, {0x8000});
  h.s.vdp1.draw();
  CHECK(px1(h, 7, 7) == (0x8000 | 15 << 10 | 15), "meia transparência (0x%04X)", px1(h, 7, 7));
  CHECK(px1(h, 12, 12) == 0xFC00, "meia transparência sobre o vazio desenha a cor");
  CHECK(px1(h, 0, 0) == 0x801F, "MSB ligado mantém a cor RGB");
  // troca de quadro automática com apagamento
  const int before = h.s.vdp1.drawBuf;
  h.w16(0x05D00002, 0); h.w16(0x05D00004, 0); h.w16(0x05D00006, 0x1234);
  h.s.vdp1.vblankIn();
  CHECK(h.s.vdp1.drawBuf == before, "no começo do V-blank os quadros ainda não trocam");
  h.w16(0x05D00004, 2);                   // desenho automático a cada troca
  h.s.vdp1.vblankOut();
  CHECK(h.s.vdp1.drawing() && h.s.vdp1.commandsRun == 5, "a troca no fim do V-blank começa o desenho automático");
  h.s.vdp1.vblankIn();
  h.s.vdp1.vblankOut();
  CHECK(!(h.r16(0x05D00010) & 1) && !(h.s.scu.pending() & (1u << INT_VDP1)), "troca com o desenho em andamento: abandonado (sem fim)");
  h.w16(0x05D00004, 0);
  h.s.vdp1.run(1 << 20);
  h.s.vdp1.vblankIn();
  h.s.vdp1.vblankOut();
  CHECK(h.s.vdp1.drawBuf != before && h.s.vdp1.display()[7 * 512 + 7] == (0x8000 | 15 << 10 | 15), "troca: o desenho vira a imagem mostrada");
  CHECK(h.s.vdp1.fb[h.s.vdp1.drawBuf][10 * 512 + 10] == 0x1234 && h.s.vdp1.fb[h.s.vdp1.drawBuf][230 * 512] == 0, "o quadro novo é apagado com EWDR na área de EWLR/EWRR");
}

// ================================================================================================ VDP2

static void cram16(Rig& r, int i, uint16_t c) { r.w16(0x05F00000 + uint32_t(i) * 2, c); }
static uint32_t rgb(uint16_t c) {
  const uint32_t R = c & 31, G = (c >> 5) & 31, B = (c >> 10) & 31;
  return 0xFF000000u | (R << 3 | R >> 2) | (G << 3 | G >> 2) << 8 | (B << 3 | B >> 2) << 16;
}
static uint32_t px2(Rig& r, int x, int y) { return r.s.vdp2.out[y * r.s.vdp2.outW + x]; }

static void vdp2Base(Rig& r) {
  r.w16(0x05F80000, 0x8000);              // tela ligada, 320x224
  r.w16(0x05E7FFFE, 0x0C63);              // cor de fundo
  r.w16(0x05F800AC, 0x0003); r.w16(0x05F800AE, 0xFFFF);
  for (int z : {0x78, 0x7C, 0x88, 0x8C}) r.w16(0x05F80000 + uint32_t(z), 1);
}

static void testVdp2() {
  {
    Rig r;
    vdp2Base(r);
    r.s.vdp2.render();
    CHECK(r.s.vdp2.outW == 320 && r.s.vdp2.outH == 224 && px2(r, 0, 0) == rgb(0x0C63) && px2(r, 319, 223) == rgb(0x0C63), "só a cor de fundo");
    r.w16(0x05F80000, 0x8011);
    r.s.vdp2.render();
    CHECK(r.s.vdp2.outW == 352 && r.s.vdp2.outH == 240, "352x240 (%dx%d)", r.s.vdp2.outW, r.s.vdp2.outH);
    r.w16(0x05F80000, 0x80C3);
    r.s.vdp2.render();
    CHECK(r.s.vdp2.outW == 704 && r.s.vdp2.outH == 448, "704x448 entrelaçado (%dx%d)", r.s.vdp2.outW, r.s.vdp2.outH);
    r.w16(0x05F80000, 0x0000);
    r.s.vdp2.render();
    CHECK(px2(r, 5, 5) == 0xFF000000u, "tela desligada fica preta");
  }
  {
    // NBG0: células de 16 cores, nome de 1 palavra; caractere 1 = metade esquerda cor 1, direita cor 2
    Rig r;
    vdp2Base(r);
    cram16(r, 1, 0x001F); cram16(r, 2, 0x03E0); cram16(r, 0x31, 0x7C00); cram16(r, 0x32, 0x7FFF);
    for (int y = 0; y < 8; y++) r.w32(0x05E00020 + uint32_t(y) * 4, 0x11112222);
    r.w16(0x05F80030, 0x8000);
    r.w16(0x05F80040, 0x0202); r.w16(0x05F80042, 0x0202);
    r.w16(0x05E04000, 0x0001);              // célula (0,0): caractere 1
    r.w16(0x05E04002, 0x3401);              // célula (1,0): caractere 1, paleta 3, espelhado
    r.w16(0x05F800F8, 0x0003);
    r.w16(0x05F80020, 0x0001);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x001F) && px2(r, 4, 0) == rgb(0x03E0), "NBG0: caractere de 16 cores");
    CHECK(px2(r, 8, 3) == rgb(0x7FFF) && px2(r, 12, 3) == rgb(0x7C00), "NBG0: paleta e espelhamento horizontal");
    CHECK(px2(r, 16, 0) == rgb(0x0C63), "NBG0: código 0 é transparente");
    r.w16(0x05F80020, 0x0101);              // transparência desligada: código 0 com a cor 0
    r.s.vdp2.render();
    CHECK(px2(r, 16, 0) == rgb(0), "NBG0: código 0 desenhado sem transparência");
    r.w16(0x05F80020, 0x0001);
    r.w16(0x05F80070, 4);                   // rolagem horizontal de 4
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x03E0) && px2(r, 4, 0) == rgb(0x7FFF), "NBG0: rolagem horizontal");
    r.w16(0x05F80070, 0);
    r.w16(0x05F8007A, 0x8000); r.w16(0x05F80078, 0);  // ampliação 2x (passo 0,5)
    r.s.vdp2.render();
    CHECK(px2(r, 7, 0) == rgb(0x001F) && px2(r, 8, 0) == rgb(0x03E0), "NBG0: ampliação horizontal");
    r.w16(0x05F80078, 1); r.w16(0x05F8007A, 0);
    // cálculo de cor: NBG0 com proporção 16 sobre o fundo (no aparelho: 15/32 de cima e 17/32 de baixo)
    r.w16(0x05F800EC, 0x0001); r.w16(0x05F80108, 16);
    r.s.vdp2.render();
    const uint32_t a = rgb(0x001F), b = rgb(0x0C63);
    uint32_t mix = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) mix |= (((a >> sh) & 0xFF) * 15 + ((b >> sh) & 0xFF) * 17) / 32 << sh;
    CHECK(px2(r, 0, 0) == mix, "cálculo de cor por proporção (0x%08X, esperado 0x%08X)", px2(r, 0, 0), mix);
    r.w16(0x05F800EC, 0x0101);
    r.s.vdp2.render();
    uint32_t add = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) add |= std::min<uint32_t>(255, ((a >> sh) & 0xFF) + ((b >> sh) & 0xFF)) << sh;
    CHECK(px2(r, 0, 0) == add, "cálculo de cor por soma");
    r.w16(0x05F800EC, 0);
    // deslocamento de cor A: +16 no azul, -300 vira -256 (9 bits)
    r.w16(0x05F80110, 0x0001); r.w16(0x05F80112, 0); r.w16(0x05F80114, 0x1F0); r.w16(0x05F80118, 16);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == (0xFF000000u | ((rgb(0x001F) & 0xFF) - 16) | 16u << 16), "deslocamento de cor (0x%08X)", px2(r, 0, 0));
    r.w16(0x05F80110, 0);
    // prioridade 0 esconde o plano
    r.w16(0x05F800F8, 0);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x0C63), "prioridade 0 esconde o NBG0");
  }
  {
    // NBG1 bitmap RGB 512x256 em 0x40000 sobre NBG0 (prioridades e empate)
    Rig r;
    vdp2Base(r);
    cram16(r, 1, 0x001F);
    for (int y = 0; y < 8; y++) r.w32(0x05E00020 + uint32_t(y) * 4, 0x11111111);
    r.w16(0x05F80030, 0x8000);
    r.w16(0x05F80040, 0x0202); r.w16(0x05F80042, 0x0202);
    for (int i = 0; i < 64 * 64; i++) r.w16(0x05E04000 + uint32_t(i) * 2, 1);
    r.w16(0x05F80028, 0x3200);              // NBG1: bitmap 32K cores
    r.w16(0x05F8003C, 0x0020);              // deslocamento 2
    r.w16(0x05E40000 + (3 * 512 + 2) * 2, 0x83E0);
    r.w16(0x05F80020, 0x0003);
    r.w16(0x05F800F8, 0x0503);              // NBG1 = 5, NBG0 = 3
    r.s.vdp2.render();
    CHECK(px2(r, 2, 3) == rgb(0x03E0) && px2(r, 3, 3) == rgb(0x001F), "bitmap RGB por cima; MSB 0 transparente");
    r.w16(0x05F800F8, 0x0303);
    r.s.vdp2.render();
    CHECK(px2(r, 2, 3) == rgb(0x001F), "empate: NBG0 ganha do NBG1");
    // NBG2 (16 cores, rolagem inteira) por cima de tudo
    r.w16(0x05F8002A, 0x0000);
    r.w16(0x05F80034, 0x8000);              // PNCN2
    r.w16(0x05F80048, 0x0303); r.w16(0x05F8004A, 0x0303);
    for (int y = 0; y < 8; y++) r.w32(0x05E00040 + uint32_t(y) * 4, 0x22222222);
    cram16(r, 2, 0x7C1F);
    r.w16(0x05E06000 + 2 * 2, 2);           // célula (2,0) do NBG2
    r.w16(0x05F80090, 8);                   // rola 8: a célula 2 aparece em x = 8
    r.w16(0x05F800FA, 0x0007);
    r.w16(0x05F80020, 0x0007);
    r.s.vdp2.render();
    CHECK(px2(r, 8, 0) == rgb(0x7C1F) && px2(r, 16, 0) == rgb(0x001F), "NBG2 com rolagem inteira");
  }
  {
    // NBG0 em 256 cores com caracteres 16x16 e nome de 2 palavras (paleta 0x10)
    Rig r;
    vdp2Base(r);
    for (int q = 0; q < 4; q++)
      for (int i = 0; i < 64; i++) r.w8(0x05E00000 + 0x100 + uint32_t(q * 64 + i), uint8_t(q + 1));
    for (int q = 0; q < 4; q++) cram16(r, 0x100 + q + 1, uint16_t(0x8000 | (q + 1) * 6));
    r.w16(0x05F80028, 0x0011);              // 256 cores, 16x16
    r.w16(0x05F80030, 0x0000);              // 2 palavras
    r.w16(0x05F80040, 0x0101); r.w16(0x05F80042, 0x0101);  // página de 0x2000 palavras... (2 palavras, 16x16: 0x1000 bytes)
    r.w32(0x05E01000, 0x00100008);          // paleta 0x10, caractere 8 (0x100 bytes)
    r.w16(0x05F800F8, 0x0001);
    r.w16(0x05F80020, 0x0001);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(6) && px2(r, 8, 0) == rgb(12) && px2(r, 0, 8) == rgb(18) && px2(r, 15, 15) == rgb(24),
          "caractere 16x16 de 256 cores (0x%08X 0x%08X)", px2(r, 8, 0), px2(r, 15, 15));
  }
  {
    // rolagem por linha, janela e RBG0 (rotação) com o caractere de duas cores
    Rig r;
    vdp2Base(r);
    cram16(r, 1, 0x001F); cram16(r, 2, 0x03E0);
    for (int y = 0; y < 8; y++) r.w32(0x05E00020 + uint32_t(y) * 4, 0x11112222);
    for (int i = 0; i < 64 * 64; i++) r.w16(0x05E04000 + uint32_t(i) * 2, 1);
    r.w16(0x05F80030, 0x8000);
    r.w16(0x05F80040, 0x0202); r.w16(0x05F80042, 0x0202);
    r.w16(0x05F800F8, 0x0003);
    r.w16(0x05F80020, 0x0001);
    // tabela de rolagem em 0x30000: linha 0 com X = 4, linha 1 com X = 0
    r.w32(0x05E30000, 4 << 16); r.w32(0x05E30004, 0);
    r.w16(0x05F800A0, 0x0001); r.w16(0x05F800A2, 0x8000);
    r.w16(0x05F8009A, 0x0002);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x03E0) && px2(r, 0, 1) == rgb(0x001F), "rolagem horizontal por linha");
    r.w16(0x05F8009A, 0);
    // janela W0 em x 0..9 (coordenadas de alta resolução) esconde o NBG0 dentro dela
    r.w16(0x05F800C0, 0); r.w16(0x05F800C2, 0); r.w16(0x05F800C4, 19); r.w16(0x05F800C6, 223);
    r.w16(0x05F800D0, 0x0002);
    r.s.vdp2.render();
    CHECK(px2(r, 2, 5) == rgb(0x0C63) && px2(r, 10, 5) == rgb(0x001F), "janela W0 esconde o plano por dentro");
    r.w16(0x05F800D0, 0x0003);
    r.s.vdp2.render();
    CHECK(px2(r, 2, 5) == rgb(0x001F) && px2(r, 10, 5) == rgb(0x0C63), "janela W0 com área de fora");
    r.w16(0x05F800D0, 0);
    // RBG0: matriz identidade pela tabela de parâmetros em 0x10000 (palavra 0x8000)
    r.w16(0x05F80020, 0x0010);
    r.w16(0x05F800FC, 5);
    r.w16(0x05F80038, 0x8000);
    for (int k = 0; k < 8; k++) r.w16(0x05F80050 + uint32_t(k) * 2, 0x0202);
    r.w16(0x05F800BC, 0); r.w16(0x05F800BE, 0x8000);
    auto par = [&](uint32_t wi, uint32_t v) { r.w32(0x05E10000 + wi * 2, v); };
    for (uint32_t wi = 0; wi < 0x30; wi += 2) par(wi, 0);
    par(0x08, 0x10000);                     // ΔYst = 1
    par(0x0A, 0x10000);                     // ΔX = 1
    par(0x0E, 0x10000); par(0x16, 0x10000); // A = E = 1
    par(0x26, 0x10000); par(0x28, 0x10000); // kx = ky = 1
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x001F) && px2(r, 4, 0) == rgb(0x03E0) && px2(r, 8, 9) == rgb(0x001F), "RBG0 com a matriz identidade");
    par(0x22, 4u << 16);                     // Mx = 4
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x03E0), "RBG0: deslocamento Mx");
    par(0x22, 0);
    par(0x0E, 0); par(0x10, 0x10000u); par(0x14, 0x10000u); par(0x16, 0);  // A=0 B=1 D=1 E=0: troca X e Y
    r.s.vdp2.render();
    CHECK(px2(r, 0, 4) == rgb(0x03E0) && px2(r, 4, 0) == rgb(0x001F), "RBG0: matriz que troca os eixos");
    par(0x0E, 0x10000); par(0x10, 0); par(0x14, 0); par(0x16, 0x10000);
    // coeficiente de 1 palavra em 0x8000 (palavra 0x4000) com k = 2
    r.w16(0x05E08000, 0x0800);
    par(0x2A, 0x40000000u);
    r.w16(0x05F800B4, 0x0003);
    r.s.vdp2.render();
    CHECK(px2(r, 1, 0) == rgb(0x001F) && px2(r, 2, 0) == rgb(0x03E0), "RBG0: tabela de coeficientes (k = 2)");
    r.w16(0x05E08000, 0x8800);
    r.s.vdp2.render();
    CHECK(px2(r, 1, 0) == rgb(0x0C63), "RBG0: coeficiente transparente");
  }
  {
    // camada de sprites: RGB (misturado) e paleta do tipo 0 com CRAOFB
    Rig r;
    vdp2Base(r);
    r.w16(0x05F800E0, 0x0020);
    r.w16(0x05F800F0, 0x0604);              // PRISA: prioridade 0 = 4, 1 = 6
    r.w16(0x05F800E6, 0x0010);              // CRAOFB: sprites a partir da cor 0x100
    cram16(r, 0x100 + 0x45, 0x7C00);
    auto& d = r.s.vdp1.fb[r.s.vdp1.drawBuf ^ 1];
    d[0] = 0x83E0;
    d[1] = 0x4045;                          // tipo 0: prioridade 1, cor 0x45
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x03E0) && px2(r, 1, 0) == rgb(0x7C00) && px2(r, 2, 0) == rgb(0x0C63), "sprites RGB e de paleta");
    r.w16(0x05F800F0, 0x0000);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == rgb(0x0C63), "sprite com prioridade 0 some");
    // sombra normal (código 0x7FE) sobre o fundo com SDCTL e janela de sprite (tipo 2, bit 15) no NBG0
    r.w16(0x05F800F0, 0x0606);
    d[2] = 0x07FE;
    r.w16(0x05F800E2, 0x0020);
    r.s.vdp2.render();
    CHECK(px2(r, 2, 0) == (0xFF000000u | ((rgb(0x0C63) >> 1) & 0x7F7F7Fu)), "sombra normal escurece o fundo (0x%08X)", px2(r, 2, 0));
    cram16(r, 1, 0x001F);
    for (int yy = 0; yy < 8; yy++) r.w32(0x05E00020 + uint32_t(yy) * 4, 0x11111111);
    r.w16(0x05F80030, 0x8000);
    r.w16(0x05F80040, 0x0202); r.w16(0x05F80042, 0x0202);
    for (int i = 0; i < 64; i++) r.w16(0x05E04000 + uint32_t(i) * 2, 1);
    r.w16(0x05F800F8, 0x0003);
    r.w16(0x05F80020, 0x0001);
    r.w16(0x05F800E0, 0x0012);              // tipo 2 com janela de sprite
    d[0] = 0; d[1] = 0; d[2] = 0;
    d[5] = 0x8000;                          // ponto só de janela (sem cor) em x = 5
    r.w16(0x05F800D0, 0x0020);              // NBG0: janela de sprite, esconde dentro
    r.s.vdp2.render();
    CHECK(px2(r, 5, 0) == rgb(0x0C63) && px2(r, 6, 0) == rgb(0x001F), "janela de sprite esconde o NBG0");
  }
}

// cálculo de cor especial, prioridade especial, estendido, cor de linha, gradação e as sombras
static uint32_t mixc(uint32_t a, uint32_t b, int ratio) {  // proporção como no aparelho: (31-r)/32 e (r+1)/32
  uint32_t m = 0xFF000000u;
  for (int sh = 0; sh < 24; sh += 8) m |= ((((a >> sh) & 0xFF) * uint32_t(31 - ratio) + ((b >> sh) & 0xFF) * uint32_t(ratio + 1)) >> 5) << sh;
  return m;
}
static uint32_t avgc(uint32_t a, uint32_t b) {
  uint32_t m = 0xFF000000u;
  for (int sh = 0; sh < 24; sh += 8) m |= ((((a >> sh) & 0xFF) + ((b >> sh) & 0xFF)) >> 1) << sh;
  return m;
}
static uint32_t half(uint32_t a) { return 0xFF000000u | ((a >> 1) & 0x7F7F7Fu); }

static void testVdp2Cc() {
  const uint32_t RED = rgb(0x001F), GREEN = rgb(0x03E0), BLUE = rgb(0x7C00), BACK = rgb(0x0C63), WHITE = rgb(0x7FFF), DARK = rgb(0x0010);
  auto nbg1 = [&](Rig& r, uint16_t ch) { for (int i = 0; i < 128; i++) r.w16(0x05E06000 + uint32_t(i) * 2, ch); };
  // NBG0 (nome de 2 palavras, mapa em 0x8000) sobre NBG1 (verde, 1 palavra, mapa em 0x6000) sobre o fundo
  auto setup = [&](Rig& r) {
    vdp2Base(r);
    cram16(r, 1, 0x001F); cram16(r, 2, 0x03E0); cram16(r, 4, 0x7C00); cram16(r, 5, 0x801F); cram16(r, 6, 0x0010);
    cram16(r, 7, 0x7FFF); cram16(r, 8, 0x0210);
    auto cell = [&](int c, uint32_t v) { for (int y = 0; y < 8; y++) r.w32(0x05E00000 + uint32_t(c) * 32 + uint32_t(y) * 4, v); };
    cell(1, 0x11111111); cell(2, 0x22222222); cell(3, 0x22224444); cell(5, 0x55555555); cell(6, 0x66666666);
    r.w16(0x05F80030, 0x0000); r.w16(0x05F80040, 0x0202); r.w16(0x05F80042, 0x0202);
    r.w16(0x05F80032, 0x8000); r.w16(0x05F80044, 0x0303); r.w16(0x05F80046, 0x0303);
    for (int i = 0; i < 128; i++) r.w16(0x05E06000 + uint32_t(i) * 2, 2);
    r.w16(0x05F80020, 0x0003);
    r.w16(0x05F800F8, 0x0203);              // NBG0 = 3, NBG1 = 2
  };
  auto nb0 = [&](Rig& r, int c, uint16_t w0, uint16_t ch) { r.w32(0x05E08000 + uint32_t(c) * 4, uint32_t(w0) << 16 | ch); };
  {
    Rig r;
    setup(r);
    nb0(r, 0, 0, 1);
    r.w16(0x05F800EC, 0x0001);              // CC do NBG0
    r.w16(0x05F80108, 0x1F00);              // NBG0 proporção 0, NBG1 proporção 31
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, GREEN, 0), "CC: proporção da camada de cima (0x%08X)", px2(r, 0, 0));
    r.w16(0x05F800EC, 0x0201);              // CCRTMD: proporção da segunda camada
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, GREEN, 31), "CC: proporção da camada de baixo (CCRTMD) (0x%08X)", px2(r, 0, 0));
  }
  {
    Rig r;  // cálculo de cor por caractere (SFCCMD = 1): só a célula com o bit SCC mistura
    setup(r);
    nb0(r, 0, 0x1000, 1); nb0(r, 1, 0x0000, 1);
    r.w16(0x05F800EC, 0x0001); r.w16(0x05F80108, 16); r.w16(0x05F800EE, 0x0001);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, GREEN, 16) && px2(r, 8, 0) == RED, "CC especial por caractere");
    // por ponto (SFCCMD = 2): bit SCC e o código de função especial A (dados de cor 2-3)
    nb0(r, 0, 0x1000, 3);
    nbg1(r, 6);
    r.w16(0x05F800EE, 0x0002); r.w16(0x05F80026, 0x0002);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(GREEN, DARK, 16) && px2(r, 4, 0) == BLUE, "CC especial por ponto (código de função)");
    nbg1(r, 2);
    r.w16(0x05F80024, 0x0001);              // SFSEL: NBG0 usa o código B (0): nada mistura
    r.w16(0x05F800F8, 0x0103);
    nb0(r, 0, 0x1000, 1);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == RED, "código de função B escolhido pelo SFSEL");
    // pelo MSB da cor na CRAM (SFCCMD = 3): cor 5 tem MSB, cor 1 não
    r.w16(0x05F800EE, 0x0003);
    nb0(r, 0, 0, 5);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, GREEN, 16) && px2(r, 8, 0) == RED, "CC especial pelo MSB da CRAM");
    r.w16(0x05F800EC, 0x0000);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == RED, "CC especial respeita o bit da camada em CCCTL");
  }
  {
    Rig r;  // prioridade especial por caractere: NBG0 = 2 sobe para 3 na célula com SPR e empata (NBG0 ganha)
    setup(r);
    r.w16(0x05F800F8, 0x0302);
    nb0(r, 0, 0x2000, 1); nb0(r, 1, 0x0000, 1);
    r.w16(0x05F800EA, 0x0001);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == RED && px2(r, 8, 0) == GREEN, "prioridade especial por caractere");
    nb0(r, 0, 0x2000, 3);
    nbg1(r, 6);
    r.w16(0x05F800EA, 0x0002); r.w16(0x05F80026, 0x0002);   // por ponto: só os dados de cor 2-3
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == GREEN && px2(r, 4, 0) == DARK, "prioridade especial por ponto");
    r.w16(0x05F80026, 0x0004);              // código 4-5: o azul sobe
    r.s.vdp2.render();
    CHECK(px2(r, 4, 0) == BLUE && px2(r, 0, 0) == DARK, "prioridade especial pelo código de função (0x%08X)", px2(r, 4, 0));
  }
  {
    Rig r;  // estendido (EXCCEN): a segunda camada (com CC ligado) é misturada com a terceira antes
    setup(r);
    nb0(r, 0, 0, 1);
    r.w16(0x05F800EC, 0x0403); r.w16(0x05F80108, 16);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, avgc(GREEN, BACK), 16), "CC estendido com a terceira camada (0x%08X)", px2(r, 0, 0));
    r.w16(0x05F800EC, 0x0401);              // segunda sem CC: normal
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, GREEN, 16), "CC estendido só quando a segunda tem CC");
    r.w16(0x05F80000, 0x8002);              // 640 pontos: o estendido não existe
    r.w16(0x05F800EC, 0x0403);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, GREEN, 16), "CC estendido desligado em alta resolução");
  }
  {
    Rig r;  // tela de cor de linha: com LNCLEN do NBG0 a mistura é com a cor de linha (uma por linha)
    setup(r);
    nb0(r, 0, 0, 1);
    for (int i = 0; i < 64; i++) nb0(r, 64 + i, 0, 1);
    r.w16(0x05F800EC, 0x0001); r.w16(0x05F80108, 16);
    r.w16(0x05F800A8, 0x8003); r.w16(0x05F800AA, 0xF000);
    r.w16(0x05E7E000, 7); r.w16(0x05E7E002, 8);
    r.w16(0x05F800E8, 0x0001);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, WHITE, 16) && px2(r, 0, 1) == mixc(RED, rgb(0x0210), 16), "cor de linha por linha no lugar da segunda camada");
    r.w16(0x05F800E8, 0x0000);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == mixc(RED, GREEN, 16), "sem LNCLEN a cor de linha não entra");
  }
  {
    Rig r;  // gradação (CCCTL bit 15) no NBG0: a segunda cor é a média horizontal do próprio NBG0
    setup(r);
    nb0(r, 0, 0, 1); nb0(r, 1, 0, 2);
    r.w16(0x05F800EC, 0xA001); r.w16(0x05F80108, 16);
    r.s.vdp2.render();
    CHECK(px2(r, 8, 0) == mixc(GREEN, avgc(RED, GREEN), 16) && px2(r, 4, 0) == mixc(RED, RED, 16), "gradação (0x%08X)", px2(r, 8, 0));
  }
  {
    Rig r;  // sombras: normal só abaixo da prioridade do sprite; MSB (própria e transparente)
    setup(r);
    for (int c = 0; c < 8; c++) nb0(r, c, 0, 1);
    auto& d = r.s.vdp1.fb[r.s.vdp1.drawBuf ^ 1];
    d[20] = 0x07FE;                         // tipo 0: sombra normal, prioridade 0
    r.w16(0x05F800E2, 0x0001);              // SDCTL: NBG0 aceita sombra
    r.w16(0x05F800F0, 0x0002);
    r.s.vdp2.render();
    CHECK(px2(r, 20, 0) == RED, "sombra abaixo do NBG0 não o escurece");
    r.w16(0x05F800F0, 0x0004);
    r.s.vdp2.render();
    CHECK(px2(r, 20, 0) == half(RED), "sombra acima do NBG0 o escurece");
    d[20] = 0;
    r.w16(0x05F800E0, 0x0002);              // tipo 2 sem janela: bit 15 = sombra por MSB
    cram16(r, 0x45, 0x7C00);
    d[24] = 0x8045;                         // com cor: o próprio sprite sai escurecido
    d[25] = 0x8000;                         // sem cor: transparente...
    r.s.vdp2.render();
    CHECK(px2(r, 24, 0) == half(BLUE) && px2(r, 25, 0) == RED, "sombra por MSB: própria; sem TPSHDSL é transparente");
    r.w16(0x05F800E2, 0x0101);              // ...ou sombra transparente (TPSHDSL)
    r.s.vdp2.render();
    CHECK(px2(r, 25, 0) == half(RED), "sombra transparente por MSB escurece o NBG0");
    // CC do sprite pela condição 3: o MSB da cor na CRAM (não o do ponto)
    d[24] = 0x0046; d[25] = 0x0047;
    cram16(r, 0x46, 0x83E0); cram16(r, 0x47, 0x03E0);
    r.w16(0x05F800E0, 0x3002); r.w16(0x05F800EC, 0x0040); r.w16(0x05F80100, 16);
    r.s.vdp2.render();
    CHECK(px2(r, 24, 0) == mixc(GREEN, RED, 16) && px2(r, 25, 0) == GREEN, "sprite: CC pelo MSB da CRAM");
  }
}

// mosaico, rolagem vertical por célula e janela de linha
static void testVdp2Extra() {
  auto setup = [](Rig& r, int ch) {
    vdp2Base(r);
    for (int c = 1; c <= 8; c++) cram16(r, c, uint16_t(c * 0x0C21));
    for (int y = 0; y < 8; y++) r.w32(0x05E00020 + uint32_t(y) * 4, 0x11111111u * uint32_t(y + 1));   // caractere 1: cor = linha
    for (int y = 0; y < 8; y++) r.w32(0x05E00040 + uint32_t(y) * 4, 0x12345678u);                     // caractere 2: cor = coluna
    r.w16(0x05F80030, 0x8000);
    r.w16(0x05F80040, 0x0202); r.w16(0x05F80042, 0x0202);
    for (int i = 0; i < 64 * 64; i++) r.w16(0x05E04000 + uint32_t(i) * 2, uint16_t(ch));
    r.w16(0x05F800F8, 0x0003);
    r.w16(0x05F80020, 0x0001);
  };
  auto C = [](int c) { return rgb(uint16_t(c * 0x0C21)); };
  {
    Rig r; setup(r, 2);
    r.w16(0x05F80022, 0x0301);              // mosaico horizontal de 4 no NBG0
    r.s.vdp2.render();
    CHECK(px2(r, 3, 0) == C(1) && px2(r, 4, 0) == C(5) && px2(r, 7, 0) == C(5), "mosaico horizontal");
  }
  {
    Rig r; setup(r, 1);
    r.w16(0x05F80022, 0x1001);              // mosaico vertical de 2
    r.s.vdp2.render();
    CHECK(px2(r, 0, 1) == C(1) && px2(r, 0, 2) == C(3) && px2(r, 0, 3) == C(3), "mosaico vertical");
    r.w16(0x05F80022, 0x1000);              // plano fora do mosaico
    r.s.vdp2.render();
    CHECK(px2(r, 0, 1) == C(2), "mosaico só nas camadas ligadas em MZCTL");
  }
  {
    Rig r; setup(r, 1);                       // rolagem por célula: coluna 1 desce 3 linhas
    r.w16(0x05F8009C, 0x0003); r.w16(0x05F8009E, 0x0000);   // tabela em 0x60000 (palavra 0x30000)
    r.w32(0x05E60000, 0); r.w32(0x05E60004, 3u << 16);
    r.w16(0x05F8009A, 0x0001);
    r.s.vdp2.render();
    CHECK(px2(r, 0, 0) == C(1) && px2(r, 8, 0) == C(4) && px2(r, 8, 2) == C(6), "rolagem vertical por célula");
    r.w16(0x05F80022, 0x0001);              // com mosaico a rolagem por célula desliga
    r.s.vdp2.render();
    CHECK(px2(r, 8, 0) == C(1), "rolagem por célula desliga com mosaico");
  }
  {
    Rig r; setup(r, 2);                       // janela de linha: linha 0 esconde x 0-9, linha 1 esconde x 20-29
    r.w16(0x05F800C0, 0); r.w16(0x05F800C2, 0); r.w16(0x05F800C4, 0); r.w16(0x05F800C6, 223);
    r.w16(0x05F800D8, 0x8003); r.w16(0x05F800DA, 0x0000);
    r.w16(0x05E60000, 0); r.w16(0x05E60002, 19);
    r.w16(0x05E60004, 40); r.w16(0x05E60006, 59);
    r.w16(0x05F800D0, 0x0002);
    r.s.vdp2.render();
    CHECK(px2(r, 5, 0) == rgb(0x0C63) && px2(r, 25, 0) != rgb(0x0C63) && px2(r, 5, 1) != rgb(0x0C63) && px2(r, 25, 1) == rgb(0x0C63),
          "janela de linha: X da janela por linha");
  }
}

// rotação: troca de parâmetros A/B (RPMD 1, 2 e 3) e o RBG1
static void testVdp2Rot() {
  Rig r;
  vdp2Base(r);
  cram16(r, 1, 0x001F); cram16(r, 2, 0x03E0); cram16(r, 3, 0x7C00);
  const uint32_t RED = rgb(0x001F), GREEN = rgb(0x03E0);
  for (int y = 0; y < 8; y++) r.w32(0x05E00020 + uint32_t(y) * 4, 0x11112222);
  for (int y = 0; y < 8; y++) r.w32(0x05E00060 + uint32_t(y) * 4, 0x33333333);
  for (int i = 0; i < 64 * 64; i++) r.w16(0x05E04000 + uint32_t(i) * 2, 1);
  r.w16(0x05F80038, 0x8000);              // RBG0: 1 palavra
  for (int k = 0; k < 8; k++) { r.w16(0x05F80050 + uint32_t(k) * 2, 0x0202); r.w16(0x05F80060 + uint32_t(k) * 2, 0x0202); }
  r.w16(0x05F800BC, 0); r.w16(0x05F800BE, 0x8000);
  auto par = [&](int ab, uint32_t wi, uint32_t v) { r.w32(0x05E10000 + uint32_t(ab) * 0x80 + wi * 2, v); };
  for (int ab = 0; ab < 2; ab++) {
    for (uint32_t wi = 0; wi < 0x30; wi += 2) par(ab, wi, 0);
    par(ab, 0x08, 0x10000); par(ab, 0x0A, 0x10000); par(ab, 0x0E, 0x10000); par(ab, 0x16, 0x10000);
    par(ab, 0x26, 0x10000); par(ab, 0x28, 0x10000);
  }
  par(1, 0x22, 4u << 16);                 // B deslocado 4 pontos: começa no verde
  r.w16(0x05F80020, 0x0010);
  r.w16(0x05F800FC, 5);
  r.s.vdp2.render();
  CHECK(px2(r, 0, 0) == RED, "RPMD 0: parâmetro A");
  r.w16(0x05F800B0, 1);
  r.s.vdp2.render();
  CHECK(px2(r, 0, 0) == GREEN, "RPMD 1: parâmetro B");
  // RPMD 2: coeficiente de A (1 palavra em 0x20000) transparente -> B
  r.w16(0x05F800B0, 2);
  r.w16(0x05F800B4, 0x0003); r.w16(0x05F800B6, 0x0001);
  r.w16(0x05E20000, 0x0400);              // k = 1,0
  r.s.vdp2.render();
  CHECK(px2(r, 0, 0) == RED, "RPMD 2: coeficiente de A válido usa A");
  r.w16(0x05E20000, 0x8000);
  r.s.vdp2.render();
  CHECK(px2(r, 0, 0) == GREEN, "RPMD 2: coeficiente de A transparente usa B");
  // RPMD 3: dentro da janela de rotação (W0, x 0..9) B, fora A
  r.w16(0x05F800B4, 0);
  r.w16(0x05F800B0, 3);
  r.w16(0x05F800C0, 0); r.w16(0x05F800C2, 0); r.w16(0x05F800C4, 19); r.w16(0x05F800C6, 223);
  r.w16(0x05F800D6, 0x0002);
  r.s.vdp2.render();
  CHECK(px2(r, 8, 0) == GREEN && px2(r, 16, 0) == RED, "RPMD 3: a janela de rotação escolhe B");
  r.w16(0x05F800D6, 0); r.w16(0x05F800B0, 0);
  // RBG1: parâmetro B com os registradores do NBG0, acima do RBG0; NBG1 some
  r.w16(0x05F80030, 0x8000);              // NBG0 (RBG1): 1 palavra, 16 cores
  r.w16(0x05F800F8, 0x0706);              // NBG0/RBG1 = 6, NBG1 = 7
  r.w16(0x05F80032, 0x8000); r.w16(0x05F80044, 0x0303); r.w16(0x05F80046, 0x0303);
  for (int i = 0; i < 64; i++) r.w16(0x05E06000 + uint32_t(i) * 2, 3);
  r.w16(0x05F80020, 0x0032);
  r.s.vdp2.render();
  CHECK(px2(r, 0, 0) == GREEN && px2(r, 8, 0) == GREEN, "RBG1 com o parâmetro B no lugar do NBG0; NBG1 desligado");
  r.w16(0x05F80020, 0x0012);
  r.s.vdp2.render();
  CHECK(px2(r, 0, 0) == rgb(0x7C00), "sem RBG1 o NBG1 volta");
}

// ================================================================================================ SCSP

static std::vector<int16_t> scspRun(Rig& r, int n) {
  std::vector<int16_t> out;
  for (int i = 0; i < n; i++) { int16_t l, rr; r.s.scsp.sample(l, rr); out.push_back(l); }
  return out;
}

static int period(const std::vector<int16_t>& v) {
  std::vector<int> ups;
  for (size_t i = 1; i < v.size(); i++) if (v[i - 1] <= 0 && v[i] > 0) ups.push_back(int(i));
  if (ups.size() < 3) return 0;
  return (ups.back() - ups[1]) / int(ups.size() - 2);
}

static void testScsp() {
  Rig r;
  for (int i = 0; i < 64; i++) { r.w16(0x05A10000 + uint32_t(i) * 2, i < 32 ? 0x2000 : 0xE000); }
  for (int i = 0; i < 64; i++) r.w8(0x05A20000 + uint32_t(i), i < 16 ? 0x20 : 0xE0);
  auto slot = [&](int n, uint16_t r0, uint16_t sa, uint16_t lea, uint16_t tl, uint16_t pitch) {
    const uint32_t b = 0x05B00000 + uint32_t(n) * 0x20;
    r.w16(b + 2, sa); r.w16(b + 4, 0); r.w16(b + 6, lea); r.w16(b + 8, 0x001F); r.w16(b + 0xA, 0x001F);
    r.w16(b + 0xC, tl); r.w16(b + 0x10, pitch); r.w16(b + 0x16, 0xE000);
    r.w16(b, r0);
  };
  r.w16(0x05B00400, 0x000F);
  slot(0, 0x1821, 0x0000, 63, 0, 0);
  auto a = scspRun(r, 1024);
  CHECK(period(a) == 64, "canal de 16 bits: período de 64 amostras (%d)", period(a));
  int peak = 0;
  for (auto v : a) peak = std::max(peak, int(std::abs(v)));
  CHECK(peak > 6000 && peak <= 0x2000, "volume máximo sem atenuação (%d)", peak);
  slot(0, 0x1821, 0x0000, 63, 0, 0x0800);  // oitava +1
  a = scspRun(r, 1024);
  CHECK(period(a) == 32, "uma oitava acima: período de 32 (%d)", period(a));
  slot(0, 0x1821, 0x0000, 63, 0x40, 0);     // TL = 64 (-24 dB)
  a = scspRun(r, 1024);
  int peak2 = 0;
  for (auto v : a) peak2 = std::max(peak2, int(std::abs(v)));
  CHECK(peak2 > peak / 24 && peak2 < peak / 12, "TL de 24 dB (%d de %d)", peak2, peak);
  slot(0, 0x0821 & ~0x0800, 0, 63, 0, 0); r.w16(0x05B00000, 0x1020);  // solta a tecla
  a = scspRun(r, 44100);
  CHECK(a.back() == 0, "depois de soltar a tecla o canal silencia");
  slot(1, 0x1832, 0x0000, 63, 0, 0);       // 8 bits em 0x20000
  a = scspRun(r, 1024);
  CHECK(period(a) == 64, "canal de 8 bits (%d)", period(a));
  // pilha de som e FM: o canal 2 (rampa lenta) modula o canal 3 (onda quadrada)
  {
    Rig f;
    for (int i = 0; i < 64; i++) f.w16(0x05A10000 + uint32_t(i) * 2, i < 32 ? 0x2000 : 0xE000);
    for (int i = 0; i < 1024; i++) f.w16(0x05A30000 + uint32_t(i) * 2, uint16_t(int16_t(i * 64 - 32768)));
    f.w16(0x05B00400, 0x000F);
    auto slotF = [&](int n, uint16_t r0, uint16_t lea, uint16_t r7, uint16_t r11) {
      const uint32_t b = 0x05B00000 + uint32_t(n) * 0x20;
      f.w16(b + 2, 0); f.w16(b + 4, 0); f.w16(b + 6, lea); f.w16(b + 8, 0x001F); f.w16(b + 0xA, 0x001F);
      f.w16(b + 0xC, 0); f.w16(b + 0xE, r7); f.w16(b + 0x10, 0); f.w16(b + 0x16, r11);
      f.w16(b, r0);
    };
    slotF(2, 0x1823, 1023, 0, 0x0000);                   // rampa em 0x30000, sem saída direta
    slotF(3, 0x1821, 63, 0, 0xE000);
    auto plain = scspRun(f, 2048);
    bool stackOk = false;
    for (int i = 0; i < 64; i++) stackOk |= f.r16(0x05B00600 + uint32_t(i) * 2) != 0;
    CHECK(stackOk, "pilha de som recebe as saídas dos canais");
    // MDL = 10, X e Y apontando para a saída do canal 2 (32 posições antes = a amostra anterior)
    slotF(2, 0x1823, 1023, 0, 0x0000);
    slotF(3, 0x1821, 63, uint16_t(10 << 12 | (2 + 4 - 3 + 32 - 4 + 64) % 64 << 6 | (2 + 4 - 3 + 32 - 4 + 64) % 64), 0xE000);
    auto fm = scspRun(f, 2048);
    CHECK(plain != fm && period(plain) == 64, "FM muda a onda do canal modulado");
  }
  // temporizador A e interrupção do SH-2 mestre
  r.w16(0x05B00422, 0x07FF); r.w16(0x05B0042E, 0x07FF);
  r.w32(0x05FE00A4, 0);
  r.w16(0x05B0042A, 0x0040);
  r.w16(0x05B00418, 0x00F0);
  scspRun(r, 15);
  CHECK(!(r.r16(0x05B00420) & 0x40), "temporizador A ainda não estourou");
  scspRun(r, 2);
  CHECK((r.r16(0x05B00420) & 0x40) && (r.r16(0x05B0042C) & 0x40), "temporizador A estourou em 16 amostras");
  CHECK(r.s.scu.pending() & (1u << INT_SCSP), "interrupção do SCSP na SCU");
  // 68000: move.w #0x1234,$1000.w ; bra.s *
  static const uint8_t prog[] = {0, 0, 0x80, 0, 0, 0, 0, 8, 0x31, 0xFC, 0x12, 0x34, 0x10, 0x00, 0x60, 0xFE};
  for (size_t i = 0; i < sizeof prog; i++) r.w8(0x05A00000 + uint32_t(i), prog[i]);
  r.s.soundPower(true);
  r.s.scsp.run68k(200);
  CHECK(r.r16(0x05A01000) == 0x1234, "o 68000 roda na RAM de som");
}

// envelope com o relógio do chip, escala de tecla, LFO, ruído, entrada externa e volume geral
// LPCTL: rampa de 64 amostras tocada para frente, para trás e alternada
static void testScspLoops() {
  for (int mode = 1; mode <= 3; mode++) {
    Rig r;
    for (int i = 0; i < 64; i++) r.w16(0x05A10000 + uint32_t(i) * 2, uint16_t(int16_t((i - 32) * 256)));
    const uint32_t b = 0x05B00000;
    r.w16(b + 2, 0); r.w16(b + 4, 0); r.w16(b + 6, 63); r.w16(b + 8, 0x001F); r.w16(b + 0xA, 0x001F);
    r.w16(b + 0xC, 0); r.w16(b + 0x10, 0); r.w16(b + 0x16, 0xE000);
    r.w16(0x05B00400, 0x000F);
    r.w16(b, uint16_t(0x1801 | mode << 5));
    const auto a = scspRun(r, 1024);
    int up = 0, down = 0;
    for (size_t i = 301; i < 1024; i++) { if (a[i] > a[i - 1]) up++; else if (a[i] < a[i - 1]) down++; }
    if (mode == 1) CHECK(up > down * 10, "LPCTL 1: laço para frente (%d sobem, %d descem)", up, down);
    if (mode == 2) CHECK(down > up * 10 && period(a) == 64, "LPCTL 2: laço para trás (%d descem, %d sobem, período %d)", down, up, period(a));
    if (mode == 3) CHECK(up > 250 && down > 250 && period(a) >= 124 && period(a) <= 132, "LPCTL 3: vai e volta (%d/%d, período %d)", up, down, period(a));
  }
}

static void testScspEnv() {
  auto setup = [](Rig& r) {
    for (int i = 0; i < 64; i++) r.w16(0x05A10000 + uint32_t(i) * 2, i < 32 ? 0x2000 : 0xE000);
    r.w16(0x05B00400, 0x000F);
  };
  // canal 0: onda quadrada de 64 amostras em 0x10000; r4 = D2R/D1R/EGHOLD/AR, r5 = LPSLNK/KRS/DL/RR, r9 = LFO
  auto key = [](Rig& r, uint16_t r4, uint16_t r5, uint16_t pitch, uint16_t r9) {
    const uint32_t b = 0x05B00000;
    r.w16(b + 2, 0); r.w16(b + 4, 0); r.w16(b + 6, 63); r.w16(b + 8, r4); r.w16(b + 0xA, r5);
    r.w16(b + 0xC, 0); r.w16(b + 0x10, pitch); r.w16(b + 0x12, r9); r.w16(b + 0x16, 0xE000);
    r.w16(b, 0x1821);
  };
  auto mon = [](Rig& r) { r.w16(0x05B00408, 0x0000); return r.r16(0x05B00408); };   // canal 0
  auto peakOf = [](const std::vector<int16_t>& v, size_t a, size_t b) { int p = 0; for (size_t i = a; i < b && i < v.size(); i++) p = std::max(p, std::abs(int(v[i]))); return p; };
  {
    Rig r; setup(r);
    key(r, 0x0014, 0x0000, 0, 0);            // AR = 20: começa em 0x280 e sobe em curva
    const int e0 = mon(r) & 31;
    scspRun(r, 200);
    const int e1 = mon(r) & 31;
    int n = 0;
    while ((mon(r) & 0x60) == 0 && n < 100000) { scspRun(r, 16); n += 16; }
    Rig q; setup(q);
    key(q, 0x0018, 0x0000, 0, 0);            // AR = 24: mais rápido
    int m = 0;
    while ((mon(q) & 0x60) == 0 && m < 100000) { scspRun(q, 16); m += 16; }
    CHECK(e0 == 0x14 && e1 < e0 && n > 0 && m < n, "ataque: começa em 0x280 e a taxa maior chega antes (%d x %d amostras)", n, m);
    Rig t; setup(t);
    key(t, 0x001F, 0x0400, 0, 0);            // AR 31 + escala 1 = 32: instantâneo
    CHECK((mon(t) & 31) == 0, "ataque instantâneo (AR + escala >= 32) (0x%04X)", mon(t));
    Rig u; setup(u);
    key(u, 0x001F, 0x0000, 0, 0);            // AR 31 sozinho: metade do caminho por amostra
    const int a0 = mon(u) & 31;
    scspRun(u, 16);
    CHECK(a0 == 0x14 && (mon(u) & 31) == 0, "AR 31 sem escala chega ao máximo em poucas amostras");
  }
  {
    Rig r; setup(r);
    key(r, 0x001F | 16 << 6, 0x0010 << 5, 0, 0);   // D1R = 16 até DL = 16, depois D2R = 0 (segura)
    scspRun(r, 30000);
    const uint16_t m = mon(r);
    CHECK(((m >> 5) & 3) == 2 && (m & 31) == 16, "decaimento 1 até DL e então decaimento 2 (0x%04X)", m);
  }
  {
    Rig r; setup(r);                          // escala de tecla: oitava +4 com KRS = 4 solta mais rápido
    key(r, 0x001F, 0x000C, 0x0000, 0);
    r.w16(0x05B00000, 0x1021);
    int n = 0;
    while ((mon(r) & 31) < 31 && n < 200000) { scspRun(r, 16); n += 16; }
    Rig q; setup(q);
    key(q, 0x001F, 0x100C, 0x2000, 0);
    q.w16(0x05B00000, 0x1021);
    int m = 0;
    while ((mon(q) & 31) < 31 && m < 200000) { scspRun(q, 16); m += 16; }
    CHECK(m * 4 < n, "escala de tecla acelera a soltura (%d x %d amostras)", m, n);
  }
  {
    Rig r; setup(r);                          // EGHOLD: volume máximo já no ataque
    key(r, 0x0020 | 0x0008, 0, 0, 0);
    const int hold = peakOf(scspRun(r, 128), 0, 128);
    Rig q; setup(q);
    key(q, 0x0008, 0, 0, 0);
    const int sem = peakOf(scspRun(q, 128), 0, 128);
    CHECK(hold > 6000 && sem < hold / 8, "EGHOLD segura o volume no ataque (%d x %d)", hold, sem);
  }
  {
    Rig r; setup(r);                          // LFO de altura, onda quadrada: meio ciclo mais agudo, meio mais grave
    key(r, 0x001F, 0, 0, uint16_t(0x1C << 10 | 1 << 8 | 7 << 5));
    auto v = scspRun(r, 4096);
    auto ups = [&](size_t a, size_t b) { int c = 0; for (size_t i = a + 1; i < b; i++) c += v[i - 1] <= 0 && v[i] > 0; return c; };
    const int alto = ups(0, 512), baixo = ups(512, 1024);
    CHECK(alto > baixo, "LFO de altura muda o tom (%d x %d ciclos)", alto, baixo);
    Rig q; setup(q);                          // LFORE segura o LFO no começo: tom fixo
    key(q, 0x001F, 0, 0, uint16_t(0x8000 | 0x1C << 10 | 0 << 8 | 7 << 5));   // dente de serra parado em 0
    auto w = scspRun(q, 1024);
    CHECK(period(w) == 64, "LFORE: sem modulação (%d)", period(w));
  }
  {
    Rig r; setup(r);                          // LFO de amplitude (dente de serra): o volume cai ao longo do ciclo
    key(r, 0x001F, 0, 0, uint16_t(0x1C << 10 | 0 << 3 | 7));
    auto v = scspRun(r, 1024);
    const int ini = peakOf(v, 0, 64), fim = peakOf(v, 900, 1020);
    CHECK(ini > fim * 4, "LFO de amplitude (%d -> %d)", ini, fim);
  }
  {
    Rig r; setup(r);                          // ruído (SSCTL = 1)
    const uint32_t b = 0x05B00000;
    r.w16(b + 8, 0x001F); r.w16(b + 0x16, 0xE000);
    r.w16(b, 0x18A1);
    auto v = scspRun(r, 256);
    int dif = 0;
    for (size_t i = 1; i < v.size(); i++) dif += v[i] != v[i - 1];
    CHECK(dif > 200 && peakOf(v, 0, 256) > 10000, "ruído (%d mudanças)", dif);
  }
  {
    Rig r; setup(r);                          // CD: entra pelos envios de efeito dos canais 16 (esq.) e 17 (dir.)
    int16_t l = 0, rr = 0;
    r.s.scsp.cdda(10000, -10000);
    r.s.scsp.sample(l, rr);
    CHECK(l == 0 && rr == 0, "CD sem nível nos canais 16/17: mudo");
    r.w16(0x05B00000 + 16 * 0x20 + 0x16, 0x00FF);   // EFSDL 7, pan todo à esquerda
    r.w16(0x05B00000 + 17 * 0x20 + 0x16, 0x00EF);   // EFSDL 7, pan todo à direita
    r.s.scsp.sample(l, rr);
    CHECK(l == 10000 && rr == -10000, "CD pelos canais 16/17 com pan (%d %d)", l, rr);
    r.w16(0x05B00400, 0x000E);
    r.s.scsp.sample(l, rr);
    CHECK(l == 7500, "volume geral 14 = 3/4 (%d)", l);
  }
}

// DSP de efeitos do SCSP: passagem do canal 0 (MIXS -> EFREG -> saída de efeito) e escrita no anel de eco
static void testScspDsp() {
  Rig r;
  for (int i = 0; i < 64; i++) r.w16(0x05A10000 + uint32_t(i) * 2, i < 32 ? 0x2000 : 0xE000);
  r.w16(0x05B00400, 0x000F);
  auto prog = [&](int step, uint64_t in) { for (int k = 0; k < 4; k++) r.w16(0x05B00800 + uint32_t(step) * 8 + uint32_t(k) * 2, uint16_t(in >> (48 - 16 * k))); };
  const uint32_t b = 0x05B00000;
  // canal 0: só envio de efeito (EFSDL 7, sem saída direta) e para o DSP em MIXS 0 com nível 7
  r.w16(b + 2, 0); r.w16(b + 4, 0); r.w16(b + 6, 63); r.w16(b + 8, 0x001F); r.w16(b + 0xA, 0x001F);
  r.w16(b + 0x14, 0x0007); r.w16(b + 0x16, 0x00E0);
  r.w16(b, 0x1821);
  auto peakOf = [](const std::vector<int16_t>& v) { int p = 0; for (auto x : v) p = std::max(p, std::abs(int(x))); return p; };
  const int seco = peakOf(scspRun(r, 512));
  CHECK(seco == 0, "sem programa no DSP o envio de efeito fica mudo (%d)", seco);
  // passo 0: SFT = MIXS0 * COEF0 (~1,0); passo 1: EFREG0 = SFT; passo 2: grava no anel (RBP = 2: 0x4000 bytes)
  r.w16(0x05B00700, 0x0FFF << 3);
  r.w16(0x05B00402, 0x0002);
  prog(0, uint64_t(0x20) << 38 | 1ull << 47 | 1ull << 45 | 1ull << 17);
  prog(1, 1ull << 28);
  prog(2, 1ull << 30);
  auto v = scspRun(r, 1024);
  const int fx = peakOf(v);
  CHECK(fx > 6000 && fx <= 0x2000 && period(v) == 64, "DSP: o canal sai pela saída de efeito (pico %d, período %d)", fx, period(v));
  CHECK(r.r16(0x05B00EC0) != 0, "EFREG legível pela CPU (0x%04X)", r.r16(0x05B00EC0));
  bool anel = false;
  for (uint32_t a = 0x4000; a < 0x4000 + 0x4000 * 2; a += 2) anel |= r.r16(0x05A00000 + a) != 0;
  CHECK(anel, "DSP: o anel de eco na RAM de som recebe as amostras");
  // o coeficiente 0 zera tudo
  r.w16(0x05B00700, 0);
  scspRun(r, 8);
  CHECK(peakOf(scspRun(r, 256)) == 0, "DSP: coeficiente 0 silencia a saída de efeito");
}

// ================================================================================================ bloco de CD

static uint16_t cdStat(Rig& r) { return uint16_t(r.cd(0x0000, 0, 0, 0)[0] >> 8); }

static void testCd() {
  constexpr uint16_t HIRQ = 0x0008;  // deslocamento do HIRQ (0x05890008)
  Rig r;
  CHECK(r.r16(0x05890018) == 0x0043 && r.r16(0x0589001C) == 0x4442 && r.r16(0x05890020) == 0x4C4F && r.r16(0x05890024) == 0x434B,
        "assinatura CDBLOCK ao ligar");
  // o comando só responde depois de alguns microssegundos
  r.w16(0x05890008, uint16_t(~1u));
  r.w16(0x05890018, 0x0000); r.w16(0x0589001C, 0); r.w16(0x05890020, 0); r.w16(0x05890024, 0);
  const bool cedo = r.r16(0x05890000 + HIRQ) & 1;
  r.s.cd.run(System::CYCLES_PER_LINE);
  CHECK(!cedo && (r.r16(0x05890008) & 1), "CMOK vem depois do processamento do comando");
  auto res = r.cd(0x0401, 0, 0, 0x040F);
  CHECK((res[0] >> 8) == 0 && (r.r16(0x05890008) & 1), "INIT com o motor ligando: ocupado (0x%04X)", res[0]);
  CHECK((r.cd(0x0200, 0, 0, 0)[0] >> 8) == 0x80, "TOC antes do disco girar: espere (WAIT)");
  r.cdRun(int(1.2 * System::CPU_HZ / System::CYCLES_PER_LINE));
  res = r.cd(0x0000, 0, 0, 0);
  CHECK((res[0] >> 8) == 1 && res[3] == 150 && (res[0] & 0x80), "depois de ~1,2 s: pausa no FAD 150, setor de dados (0x%04X %d)", res[0], res[3]);
  // estado periódico: depois que o programa lê CR4, o bloco escreve o estado sozinho (bit 0x20) e liga SCDQ
  r.cdClear(0x400);
  r.w16(0x05890018, 0);
  r.cdRun(300);
  CHECK((r.r16(0x05890018) >> 8) == 0x21 && (r.r16(0x05890008) & 0x400), "estado periódico em CR1 (0x%04X) e SCDQ", r.r16(0x05890018));
  res = r.cd(0x0200, 0, 0, 0);
  CHECK(res[1] == 0xCC && (r.r16(0x05890008) & 2), "TOC: 0xCC palavras e DRDY");
  std::vector<uint32_t> toc;
  for (int i = 0; i < 102; i++) toc.push_back(r.r32(0x05818000));
  r.cd(0x0600, 0, 0, 0);
  CHECK(toc[0] == 0x41000096u && toc[1] == 0x01000000u + 40 + 150 + 150 && toc[2] == 0xFFFFFFFFu, "TOC: faixas (0x%08X 0x%08X)", toc[0], toc[1]);
  CHECK(toc[99] == 0x41010000u && toc[100] == 0x01020000u && toc[101] == 0x01000000u + 40 + 150 + 75 + 150, "TOC: primeira, última e fim");
  res = r.cd(0x0300, 0, 0, 0);
  CHECK((res[2] >> 8) == 1, "uma sessão");
  // sem autenticação o bloco lê, mas não guarda setores de dados
  r.cd(0x3000, 0, 0x0000, 0);
  r.cdClear(0x14);
  r.cd(0x1080, 0x0096, 0x0080, 0x0004);
  r.cdUntil(0x10, 5000);
  CHECK(r.cd(0x5100, 0, 0x0000, 0)[3] == 0 && (r.r16(0x05890008) & 4), "sem autenticação: setores lidos (CSCT), nada guardado");
  // autenticação: lê o começo do disco; enquanto isso tudo responde 0xFFFF
  r.cdClear(0x200);
  r.cd(0xE000, 0, 0, 0);
  res = r.cd(0x0000, 0, 0, 0);
  CHECK(res[0] == 0xFFFF && res[3] == 0xFFFF, "autenticando: respostas 0xFFFF");
  CHECK(r.cdUntil(0x200, 5000) > 50, "autenticação leva tempo (busca e leitura)");
  CHECK(r.cd(0xE100, 0, 0, 0)[1] == 4, "disco autenticado como Saturn");
  // leitura de 4 setores pelo filtro 0: ocupado, buscando e só então tocando
  r.cd(0x3000, 0, 0x0000, 0);
  r.cdClear(0x14);
  res = r.cd(0x1080, 0x0096, 0x0080, 0x0004);
  CHECK((res[0] >> 8) == 0, "PLAY: ocupado logo depois do comando (0x%04X)", res[0]);
  r.cdRun(40);
  CHECK(r.cd(0x5100, 0, 0x0000, 0)[3] == 0, "a leitura respeita a busca");
  r.cdRun(500);
  CHECK(cdStat(r) == 4, "depois de ~30 ms: buscando (SEEK)");
  const int primeiro = r.cdUntil(0x04, 5000) + 540;
  const double ms = primeiro * 1000.0 * System::CYCLES_PER_LINE / System::CPU_HZ;
  CHECK(ms > 100 && ms < 140, "primeiro setor depois da busca (%.1f ms)", ms);
  r.cdRun(40);
  CHECK(cdStat(r) == 3, "lendo: estado PLAY");
  r.cdUntil(0x10, 5000);
  res = r.cd(0x5100, 0, 0x0000, 0);
  CHECK(res[3] == 4, "4 setores na partição 0 (%d)", res[3]);
  res = r.cd(0x0000, 0, 0, 0);
  CHECK((res[0] >> 8) == 1 && res[3] == 154, "fim: pausa no setor seguinte (0x%04X FAD %d)", res[0], res[3]);
  res = r.cd(0x5000, 0, 0, 0);
  CHECK(res[1] == 196 && res[3] == 200, "buffers livres (%d)", res[1]);
  res = r.cd(0x5400, 1, 0x0000, 0);
  CHECK(res[1] == 151, "informação do setor 1: FAD 151 (%d)", res[1]);
  // busca por FAD: o maior FAD <= 152 a partir do setor 1
  r.cd(0x5500, 1, 0x0000, 152);
  res = r.cd(0x5600, 0, 0, 0);
  CHECK(res[1] == 2 && res[3] == 152 && (res[2] >> 8) == 0, "FAD SEARCH acha o setor 2 (FAD 152)");
  CHECK((r.cd(0x5500, 9, 0x0000, 152)[0] >> 8) == 0xFF, "FAD SEARCH fora da partição é recusada");
  res = r.cd(0x6100, 0, 0x0000, 1);
  std::string head;
  for (int i = 0; i < 8; i++) { const uint16_t w = r.r16(0x05818000); head += char(w >> 8); head += char(w & 0xFF); }
  CHECK(head == "SEGA SEGASATURN ", "GET SECTOR DATA: IP.BIN (\"%s\")", head.c_str());
  res = r.cd(0x0600, 0, 0, 0);
  CHECK(res[1] == 8, "fim da transferência conta as palavras lidas (%d)", res[1]);
  r.cd(0x6001, 0, 0, 0);  // setores de 2336 bytes
  r.cd(0x6100, 1, 0x0000, 1);
  CHECK(r.r32(0x05818000) == 0x00000000u, "2336: começa no subcabeçalho");
  r.cd(0x0600, 0, 0, 0);
  r.cd(0x6000, 0xFF00, 0, 0);
  r.cd(0x6200, 0, 0x0000, 0xFFFF);
  CHECK(r.cd(0x5100, 0, 0x0000, 0)[3] == 0, "DELETE SECTOR DATA esvazia a partição");
  // busca mais longa (e para trás) demora mais que a curta
  auto seekMs = [&](uint16_t fad) {
    r.cdClear(0x14);
    r.cd(0x1080, fad, 0x0080, 0x0001);
    const int l = r.cdUntil(0x04, 8000);
    r.cdUntil(0x10, 8000);
    r.cd(0x6200, 0, 0x0000, 0xFFFF);
    return l * 1000.0 * System::CYCLES_PER_LINE / System::CPU_HZ;
  };
  const double perto = seekMs(156), longe = seekMs(186), tras = seekMs(151);
  CHECK(perto < longe && tras > perto, "tempo de busca cresce com a distância e para trás (%.1f %.1f %.1f ms)", perto, longe, tras);
  // SEEK para um FAD: pausa lá sem PEND; parar: STANDBY depois de ~1 s
  r.cdClear(0x10);
  r.cd(0x1180, 0x00A0, 0, 0);
  r.cdRun(3000);
  res = r.cd(0x0000, 0, 0, 0);
  CHECK((res[0] >> 8) == 1 && res[3] == 160 && !(r.r16(0x05890008) & 0x10), "SEEK: pausa no FAD 160 sem PEND (0x%04X %d)", res[0], res[3]);
  r.cd(0x1100, 0, 0, 0);
  CHECK(cdStat(r) == 0, "parar: ocupado enquanto o motor para");
  r.cdRun(1000);
  res = r.cd(0x0000, 0, 0, 0);
  CHECK((res[0] >> 8) == 2 && res[3] == 0xFFFF && (res[0] & 0x7F) == 0x7F, "parado: STANDBY sem posição (0x%04X)", res[0]);
  // repetição: 2 setores tocados 3 vezes
  r.cdClear(0x10);
  r.cd(0x1080, 0x0096, 0x0280, 0x0002);
  r.cdUntil(0x10, 20000);
  CHECK(r.cd(0x5100, 0, 0x0000, 0)[3] == 6, "PLAY com 2 repetições: 6 setores (%d)", r.cd(0x5100, 0, 0x0000, 0)[3]);
  r.cd(0x6200, 0, 0x0000, 0xFFFF);
  // buffer cheio: a leitura pausa e volta sozinha quando há espaço
  r.cdClear(0x18);
  r.cd(0x1080, 0x0096, 0x0F80, 0x0028);         // repete sem fim
  r.cdUntil(0x08, 200000);
  r.cdRun(3000);
  res = r.cd(0x0000, 0, 0, 0);
  CHECK((r.r16(0x05890008) & 8) && (res[0] >> 8) == 1 && r.cd(0x5000, 0, 0, 0)[1] == 0, "BFUL: 200 setores e a leitura pausa (0x%04X)", res[0]);
  r.cd(0x6200, 0, 0x0000, 100);
  r.cdRun(3000);
  CHECK(cdStat(r) == 3 && r.cd(0x5000, 0, 0, 0)[1] < 100, "com espaço livre a leitura continua");
  r.cd(0x11FF, 0xFFFF, 0, 0);                     // pausa
  r.cdRun(100);
  CHECK(cdStat(r) == 1, "SEEK 0xFFFFFF pausa a leitura");
  r.cd(0x0400, 0, 0, 0);
  r.cd(0x6200, 0, 0x0000, 0xFFFF);
  // filtro por faixa de FAD com a conexão falsa para o filtro 1
  r.cd(0x4000, 0x0097, 0x0000, 0x0002);           // filtro 0: FAD 151..152
  r.cd(0x4440, 0, 0x0000, 0);                     // modo: faixa de FAD
  r.cd(0x4602, 0x0001, 0x0000, 0);                // falso -> filtro 1
  r.cdClear(0x10);
  r.cd(0x1080, 0x0096, 0x0080, 0x0004);
  r.cdUntil(0x10, 8000);
  CHECK(r.cd(0x5100, 0, 0x0000, 0)[3] == 2 && r.cd(0x5100, 0, 0x0100, 0)[3] == 2, "filtro por FAD separa as partições");
  // copiar (filtro 2 -> partição 2) e mover (filtro 3 -> partição 3) setores
  r.cd(0x6502, 0, 0x0000, 0xFFFF);
  r.cd(0x6603, 0, 0x0100, 0xFFFF);
  CHECK(r.cd(0x5100, 0, 0x0200, 0)[3] == 2 && r.cd(0x5100, 0, 0x0000, 0)[3] == 2, "COPY SECTOR DATA duplica");
  CHECK(r.cd(0x5100, 0, 0x0300, 0)[3] == 2 && r.cd(0x5100, 0, 0x0100, 0)[3] == 0 && (r.r16(0x05890008) & 0x100), "MOVE SECTOR DATA move (ECPY)");
  r.cd(0x4800, 0, 0xFC00, 0);                     // reinicia tudo
  r.cd(0x48FC, 0, 0, 0);
  CHECK(r.cd(0x5100, 0, 0x0000, 0)[3] == 0, "RESET SELECTOR limpa as partições");
  // PUT SECTOR DATA: o SH-2 escreve um setor de 2048 bytes no filtro 5 e lê de volta
  res = r.cd(0x6400, 0, 0x0500, 1);
  CHECK((res[0] & 0x4000) && (r.r16(0x05890008) & 2), "PUT SECTOR DATA: DTREQ e DRDY");
  for (int i = 0; i < 1024; i++) r.w16(0x05818000, uint16_t(i * 3 + 1));
  r.cdClear(0x80);
  res = r.cd(0x0600, 0, 0, 0);
  CHECK(res[1] == 1024 && (r.r16(0x05890008) & 0x80) && r.cd(0x5100, 0, 0x0500, 0)[3] == 1, "setor escrito entra na partição 5 (EHST)");
  r.cd(0x6300, 0, 0x0500, 1);
  bool igual = true;
  for (int i = 0; i < 1024; i++) igual &= r.r16(0x05818000) == uint16_t(i * 3 + 1);
  r.cd(0x0600, 0, 0, 0);
  CHECK(igual, "setor escrito volta igual pela porta");
  // setor Mode 2 Form 2 posto com 2352 bytes: em 2048 o bloco entrega 2324 bytes (1162 palavras)
  r.cd(0x60FF, 0x0300, 0, 0);
  r.cd(0x6400, 0, 0x0600, 1);
  for (int i = 0; i < 1176; i++) {
    uint16_t w = 0;
    if (i == 7) w = 0x0002;             // byte 15: modo 2
    if (i == 9) w = 0x2000;             // byte 18: subcabeçalho com Form 2
    r.w16(0x05818000, w);
  }
  r.cd(0x0600, 0, 0, 0);
  r.cd(0x5200, 0, 0x0600, 1);
  CHECK((r.cd(0x5300, 0, 0, 0)[1]) == 1162, "Mode 2 Form 2: 1162 palavras em 2048");
  r.cd(0x6000, 0xFF00, 0, 0);
  r.cd(0x6200, 0, 0x0600, 0xFFFF);
  // sistema de arquivos: CHANGE DIR lê o disco (EFLS depois da busca)
  r.cdClear(0x200);
  res = r.cd(0x7000, 0, 0x17FF, 0xFFFF);
  CHECK(!(r.r16(0x05890008) & 0x200) && (r.cd(0x7200, 0, 0, 0)[0] >> 8) == (cdStat(r) | 0x80), "CHANGE DIR: lendo; escopo espera (WAIT)");
  CHECK(r.cdUntil(0x200, 8000) > 50, "CHANGE DIR: EFLS no fim da leitura");
  res = r.cd(0x7200, 0, 0, 0);
  CHECK(res[1] == 2 && res[3] == 2, "escopo: 2 arquivos a partir do 2 (%d)", res[1]);
  res = r.cd(0x7300, 0, 0x00FF, 0xFFFF);
  std::vector<uint32_t> fi;
  for (int i = 0; i < res[1] / 2; i++) fi.push_back(r.r32(0x05818000));
  r.cd(0x0600, 0, 0, 0);
  CHECK(fi.size() == 6 && fi[0] == 21 + 150 && fi[1] == 1000 && fi[3] == 22 + 150 && fi[4] == 4096, "GET FILE INFO da raiz");
  r.cdClear(0x210);
  r.cd(0x7400, 0, 0x0200, 3);                     // DADOS.BIN para a partição 2
  r.cdUntil(0x200, 8000);
  CHECK((r.r16(0x05890008) & 0x200) && !(r.r16(0x05890008) & 0x10), "READ FILE termina com EFLS (sem PEND)");
  res = r.cd(0x5100, 0, 0x0200, 0);
  CHECK(res[3] == 2, "DADOS.BIN: 2 setores (%d)", res[3]);
  r.cd(0x6300, 0, 0x0200, 2);
  bool ok = true;
  for (int i = 0; i < 4096; i += 4) {
    const uint32_t w = r.r32(0x05818000);
    for (int k = 0; k < 4; k++) ok &= uint8_t(w >> (24 - 8 * k)) == uint8_t((i + k) * 7 + ((i + k) >> 8));
  }
  r.cd(0x0600, 0, 0, 0);
  CHECK(ok, "conteúdo do arquivo lido pelo bloco de CD");
  CHECK(r.cd(0x5100, 0, 0x0200, 0)[3] == 0, "GET THEN DELETE apaga");
  // áudio: faixa 2 (1x)
  r.cd(0x1000, 0x0201, 0x0000, 0x0201);
  r.cdRun(3000);
  int16_t l = 0, rr = 0, hi = 0, lo = 0;
  for (int i = 0; i < 400; i++) if (r.s.cd.audioOut(l, rr)) { hi = std::max(hi, l); lo = std::min(lo, l); }
  CHECK(hi == 8000 && lo == -8000, "CD de áudio sai pelo som (%d %d)", hi, lo);
  r.cdRun(10);
  res = r.cd(0x2000, 0, 0, 0);
  std::vector<uint16_t> q;
  for (int i = 0; i < 5; i++) q.push_back(r.r16(0x05818000));
  r.cd(0x0600, 0, 0, 0);
  const uint32_t qfad = uint32_t(q[3] & 0xFF) << 16 | q[4];
  const uint32_t qrel = uint32_t(q[1] & 0xFF) << 16 | q[2];
  CHECK(res[1] == 5 && q[0] == 0x0102 && (q[1] >> 8) == 1 && qfad >= 340 && qrel == qfad - 340, "subcódigo Q da faixa 2 em binário (0x%04X FAD %u rel %u)", q[0], qfad, qrel);
  CHECK(r.cd(0x2001, 0, 0, 0)[1] == 12 && r.r16(0x05818000) == 0xFFFF, "subcódigo R-W: 12 palavras");
  r.cd(0x0600, 0, 0, 0);
  {  // varredura numa faixa de áudio longa: estado SCAN, posição pula ~100 setores a cada 3 lidos, som a 1/4
    Rig v;
    SatDisc longo(3000);
    v.s.cd.setDisc(&longo);
    v.s.cd.reset();
    v.cdRun(int(1.3 * System::CPU_HZ / System::CYCLES_PER_LINE));
    v.cd(0x1000, 0x0201, 0x0000, 0x0201);
    v.cdRun(3000);
    const uint32_t antes = v.cd(0x0000, 0, 0, 0)[3];
    v.cd(0x1200, 0, 0, 0);
    v.cdRun(1500);
    while (v.s.cd.audioOut(l, rr)) {}
    v.cdRun(300);
    hi = 0;
    for (int i = 0; i < 200; i++) if (v.s.cd.audioOut(l, rr)) hi = std::max(hi, l);
    res = v.cd(0x0000, 0, 0, 0);
    const uint32_t frente = res[3];
    CHECK((res[0] >> 8) == 5 && frente > antes + 100 && frente < antes + 400, "SCAN para frente: estado 5 e o FAD pula (%u -> %u)", antes, frente);
    CHECK(hi == 2000, "SCAN: som a 1/4 do volume (%d)", hi);
    v.cd(0x1201, 0, 0, 0);
    v.cdRun(1500);
    res = v.cd(0x0000, 0, 0, 0);
    CHECK((res[0] >> 8) == 5 && uint32_t(res[3]) + 50 < frente, "SCAN para trás (%u -> %u)", frente, res[3]);
  }
  CHECK((r.cd(0x1202, 0, 0, 0)[0] >> 8) == 0xFF, "SCAN com direção inválida é recusado");
  CHECK((r.cd(0x3000, 0, 0x3000, 0)[0] >> 8) == 0xFF, "filtro inválido é recusado");
  CHECK((r.cd(0x1080, 0x0096, 0x0000, 0x0201)[0] >> 8) == 0xFF, "PLAY com FAD e faixa misturados é recusado");
}

// ================================================================================================ robustez

// programas e DMAs que antes travavam ou estouravam a pilha: aqui só precisam terminar
static void testRobustez() {
  {
    Rig r;  // LPS repetindo MVI #5,LOP: LOP é lido uma vez só
    dspLoad(r, {0xE8000000u, 0xA8000005u, 0xF0000000u});
    CHECK(!r.s.scu.dsp.run, "DSP: LPS com MVI em LOP termina");
    Rig q;  // LPS dentro de LPS
    dspLoad(q, {0xE8000000u, 0xE8000000u, 0xF0000000u});
    CHECK(!q.s.scu.dsp.run, "DSP: LPS seguido de LPS termina");
  }
  {
    Rig r;  // DMA da SCU com destino no próprio D0EN
    r.w32(0x06010000, 0x101);
    r.w32(0x05FE0000, 0x06010000); r.w32(0x05FE0004, 0x05FE0010); r.w32(0x05FE0008, 4);
    r.w32(0x05FE000C, 0x101); r.w32(0x05FE0014, 7); r.w32(0x05FE0010, 0x101);
    r.s.scu.tick(1 << 20);
    CHECK(r.s.scu.pending() & (1u << INT_DMA0), "DMA da SCU não alcança os registradores da SCU");
  }
  {
    Rig r;  // DMA do DSP para o registrador de passo único (0x05FE0080)
    r.w32(0x05FE0088, 0);
    r.w32(0x05FE008C, 0x00020000);  // passo único
    dspLoad(r, {0xC0011001u, 0xF0000000u});  // o DMA usa WA0 (definido abaixo e reexecutado)
    r.s.scu.dsp.wa0 = 0x05FE0080 >> 2;
    r.s.scu.dsp.ct[0] = 0;
    r.w32(0x05FE0080, 0x8000);
    r.w32(0x05FE0080, 0x10000);
    r.s.scu.dspRun(200);
    CHECK(!r.s.scu.dsp.run, "DMA do DSP não alcança os registradores da SCU");
  }
  {
    Rig r;  // DMA do SCSP da RAM para o próprio registrador de DMA, com o bit de partida
    r.w16(0x05A00000, 0x1002);
    r.w16(0x05B00412, 0x0000);
    r.w16(0x05B00414, 0x0416);
    r.w16(0x05B00416, 0x1002);
    CHECK(!(r.r16(0x05B00416) & 0x1000), "DMA do SCSP gravando em 0x416 não se repete");
  }
  {
    Rig r;  // faixa de áudio em tempo real: 150 setores = 2 s
    SatDisc d150(150);
    r.s.cd.setDisc(&d150);
    r.cd(0x1000, 0x0201, 0x0000, 0x0201);
    r.w16(0x05890008, uint16_t(~0x10u));
    const int seg = int(System::CPU_HZ / System::CYCLES_PER_LINE);
    int16_t l = 0, rr = 0;
    size_t amostras = 0;
    for (int i = 0; i < seg; i++) {
      r.cdRun(1);
      for (int k = 0; k < 3; k++) amostras += r.s.cd.audioOut(l, rr);
    }
    CHECK(!(r.r16(0x05890008) & 0x10), "CD de áudio: depois de 1 s ainda tocando");
    // estado salvo com a fila de áudio: a continuação é a mesma
    StateIO w;
    r.s.cd.sync(w);
    const std::vector<uint8_t> st = w.data();
    std::vector<int16_t> a, b;
    for (int k = 0; k < 2000; k++) { r.s.cd.audioOut(l, rr); a.push_back(l); }
    StateIO rd(st);
    r.s.cd.sync(rd);
    for (int k = 0; k < 2000; k++) { r.s.cd.audioOut(l, rr); b.push_back(l); }
    CHECK(a == b, "CD de áudio: a fila entra no estado salvo");
    for (int i = 0; i < seg + seg / 5; i++) {
      r.cdRun(1);
      for (int k = 0; k < 3; k++) amostras += r.s.cd.audioOut(l, rr);
    }
    CHECK(r.r16(0x05890008) & 0x10, "CD de áudio: PEND depois de ~2 s");
    CHECK(amostras > 80000 && amostras < 92000, "CD de áudio a 44,1 kHz (%zu amostras)", amostras);
  }
}

// bloco de CD: estado salvo no meio da busca continua igual; unidade adulterada é recusada
// buffers do CD: autenticação com tudo cheio, INIT que cancela, PUT que reserva e não passa pelo filtro
static void testCdBuffers() {
  auto put = [](Rig& r, int part, int n) {
    r.cd(0x6400, 0, uint16_t(part << 8), uint16_t(n));
    for (int i = 0; i < n * 1024; i++) r.w16(0x05818000, uint16_t(i));
    r.cd(0x0600, 0, 0, 0);
  };
  auto count = [](Rig& r, int part) { return r.cd(0x5100, 0, uint16_t(part << 8), 0)[3]; };
  {
    Rig r;
    r.cdReady();
    put(r, 0, 200);
    CHECK(count(r, 0) == 200 && r.cd(0x5000, 0, 0, 0)[1] == 0, "PUT de 200 setores: nenhum buffer livre");
    r.cdClear(0x200);
    r.cd(0xE000, 0, 0x0100, 0);
    const int lines = r.cdUntil(0x200, 20000);
    const auto st = r.cd(0x0000, 0, 0, 0);
    CHECK(lines > 0 && st[0] != 0xFFFF && r.cd(0xE100, 0, 0, 0)[1] == 4 && count(r, 0) == 200,
          "autenticação com os buffers cheios termina (EFLS em %d linhas, 0x%04X)", lines, st[0]);
    r.cdClear(0x200);
    r.cd(0xE000, 0, 0x0100, 0);
    const bool busy = r.cd(0x0000, 0, 0, 0)[0] == 0xFFFF;
    const auto ini = r.cd(0x0401, 0, 0, 0x040F);
    r.cdRun(10);
    const uint16_t st2 = r.cd(0x0000, 0, 0, 0)[0], typ = r.cd(0xE100, 0, 0, 0)[1];
    CHECK(busy && ini[0] != 0xFFFF && st2 != 0xFFFF && typ == 0,
          "INIT com reinício cancela a autenticação (%d 0x%04X 0x%04X %d)", busy, ini[0], st2, typ);
  }
  {
    Rig r;
    r.cdReady();
    r.cd(0x3000, 0, 0x0000, 0);
    r.cd(0x1080, 0x0096, 0x0F80, 0x0028);          // leitura sem fim pelo filtro 0
    r.cd(0x6400, 0, 0x0500, 100);                  // PUT de 100 setores reserva os buffers
    CHECK(r.cd(0x5000, 0, 0, 0)[1] == 100, "PUT recebido: 100 buffers reservados (%d livres)", r.cd(0x5000, 0, 0, 0)[1]);
    r.cdRun(40000);
    CHECK(count(r, 0) == 100 && r.cd(0x5000, 0, 0, 0)[1] == 0 && cdStat(r) == 1,
          "leitura durante o PUT para nos buffers que sobram (%d setores)", count(r, 0));
    for (int i = 0; i < 100 * 1024; i++) r.w16(0x05818000, uint16_t(i));
    r.cd(0x0600, 0, 0, 0);
    CHECK(count(r, 5) == 100 && count(r, 0) == 100 && r.cd(0x5000, 0, 0, 0)[1] == 0, "fim do PUT: 200 setores, nunca mais que isso");
    StateIO w;
    r.s.cd.sync(w);
    bool ok = true;
    try { StateIO rd(w.data()); Rig b; b.s.cd.sync(rd); } catch (const std::exception&) { ok = false; }
    CHECK(ok, "estado salvo com os buffers cheios carrega");
    // filtro 5 com faixa de FAD: o setor posto (cabeçalho zero) entra mesmo assim na partição 5
    r.cd(0x4800, 0, 0xFC00, 0);
    r.cd(0x48FC, 0, 0, 0);
    r.cd(0x4000, 0x0097, 0x0500, 0x0002);
    r.cd(0x4440, 0, 0x0500, 0);
    put(r, 5, 1);
    CHECK(count(r, 5) == 1, "PUT vai direto para a partição, sem passar pelo filtro");
  }
}

static void testCdState() {
  Rig a;
  a.cdReady();
  a.cd(0x3000, 0, 0x0000, 0);
  a.cd(0x1080, 0x0096, 0x0080, 0x0010);
  a.cdRun(700);                                   // no meio da busca
  StateIO w;
  a.s.cd.sync(w);
  const std::vector<uint8_t> st = w.data();
  Rig b;
  StateIO rd(st);
  b.s.cd.sync(rd);
  std::vector<uint16_t> ha, hb;
  for (int i = 0; i < 40; i++) {
    a.cdRun(100); b.cdRun(100);
    ha.push_back(a.cd(0x5100, 0, 0x0000, 0)[3]); ha.push_back(a.r16(0x05890018));
    hb.push_back(b.cd(0x5100, 0, 0x0000, 0)[3]); hb.push_back(b.r16(0x05890018));
  }
  CHECK(ha == hb && ha[ha.size() - 2] == 16, "CD: estado salvo durante a busca continua igual (%u setores)", ha[ha.size() - 2]);
  // a fase da unidade fica logo depois de stat_, cmdPending_ e cmdClock_: um valor inválido é recusado
  size_t off = 2 + 2 + 8 + 8 + 1 + 1 + 4;
  bool threw = false;
  auto bad = st;
  bad[off] = 0x77;
  try { StateIO r2(bad); Rig c; c.s.cd.sync(r2); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "CD: fase da unidade adulterada é recusada");
}

// LOPR: o último comando do desenho (não o de fim de lista) continua lá depois da troca de quadro
static void testVdp1Lopr() {
  Rig r;
  cmd(r, 0, {0x0009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 319, 223});
  cmd(r, 1, {0x000A, 0, 0, 0, 0, 0, 0, 0});
  cmd(r, 2, {0x8000});
  r.s.vdp1.draw();
  r.s.vdp1.vblankOut();
  CHECK(r.r16(0x05D00012) == 4 && r.r16(0x05D00014) == 8, "LOPR com o último comando (%d), COPR no fim da lista (%d)",
        r.r16(0x05D00012), r.r16(0x05D00014));
}

// ================================================================================================ dados aleatórios

static void testRandom() {
  // listas de comandos, registradores e programas de DSP aleatórios: nada pode travar nem sair da memória
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
  const auto t0 = std::chrono::steady_clock::now();
  for (int round = 0; round < 6; round++) {
    Rig r;
    for (auto& b : r.s.vdp1.vram) b = uint8_t(rnd());
    for (auto& b : r.s.vdp2.vram) b = uint8_t(rnd());
    for (auto& b : r.s.vdp2.cram) b = uint8_t(rnd());
    for (int i = 0; i < 0x90; i++) r.w16(0x05F80000 + uint32_t(i) * 2, uint16_t(rnd()));
    r.w16(0x05F80000, uint16_t(0x8000 | (rnd() & 0x37)));
    r.w16(0x05F800E0, uint16_t(rnd()));
    r.s.vdp1.draw();
    r.s.vdp2.render();
    for (int i = 0; i < 256; i++) r.s.scu.dsp.prog[i] = rnd() | (rnd() << 24);
    r.s.scu.dsp.prog[255] = 0xF0000000u;
    r.w32(0x05FE0080, 0x8000);
    r.w32(0x05FE0080, 0x10000);
    r.s.scu.dspRun(20000);
    for (int i = 0; i < 0x400; i += 2) r.w16(0x05B00000 + uint32_t(i), uint16_t(rnd()));
    scspRun(r, 2000);
    for (int i = 0; i < 64; i++) r.cd(uint16_t(rnd()), uint16_t(rnd()), uint16_t(rnd()), uint16_t(rnd()));
    r.cdRun(200);
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(secs < 20, "dados aleatórios terminam (%.1f s)", secs);
}

// ================================================================================================ estado

static void testState(const std::string& dir) {
  std::vector<uint8_t> bios = readAll(dir + "/bios/teste.bin");
  std::unique_ptr<cdrom::Disc> d1, d2;
  if (bios.size() == 0x80000 && std::filesystem::exists(dir + "/disco/homebrew.cue")) {
    d1 = cdrom::open(dir + "/disco/homebrew.cue");
    d2 = cdrom::open(dir + "/disco/homebrew.cue");
  } else {
    bios = tinyBios();
    d1 = std::make_unique<SatDisc>();
    d2 = std::make_unique<SatDisc>();
  }
  SaturnCore a(std::move(d1), nullptr, &bios), b(std::move(d2), nullptr, &bios);
  a.setSampleRate(48000); b.setSampleRate(48000);
  auto frameHash = [](SaturnCore& c) { return fnv(c.framebuffer(), size_t(c.width()) * c.height()); };
  std::vector<float> L(4096), R(4096);
  auto audioHash = [&](SaturnCore& c) {
    const size_t n = std::min<size_t>(c.audioAvailable(), 4096);
    c.pullAudio(L.data(), R.data(), n);
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) { int32_t v = int32_t(L[i] * 32768); h ^= uint32_t(v); h *= 0x01000193u; }
    return h;
  };
  for (int f = 0; f < 90; f++) { a.runFrame(); b.runFrame(); audioHash(a); audioHash(b); }
  CHECK(frameHash(a) == frameHash(b), "duas execuções iguais (determinismo)");
  const auto st = a.saveState();
  std::vector<uint32_t> ha, hb;
  for (int f = 0; f < 60; f++) { a.setKeys(f % 20 < 5 ? uint32_t(KEY_START) : 0u); a.runFrame(); ha.push_back(frameHash(a) ^ audioHash(a)); }
  b.loadState(st);
  CHECK(b.saveState() == st, "estado carregado volta byte a byte igual ao salvo");
  for (int f = 0; f < 60; f++) { b.setKeys(f % 20 < 5 ? uint32_t(KEY_START) : 0u); b.runFrame(); hb.push_back(frameHash(b) ^ audioHash(b)); }
  CHECK(ha == hb, "estado salvo: as mesmas imagens e o mesmo som depois de carregar (%zu bytes)", st.size());
  auto bad = st;
  bad.resize(bad.size() / 2);
  bool threw = false;
  try { b.loadState(bad); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "estado truncado é recusado");
  b.runFrame();
  bad = st;
  bad[0] ^= 0xFF;
  threw = false;
  try { b.loadState(bad); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "estado de outro console é recusado");
  {  // estado da 2.4 ("SAT1", bloco de CD sem a unidade) é recusado sem travar
    StateIO w;
    uint32_t m = 0x53415431;
    w.sync(m);
    bad = w.data();
    bad.insert(bad.end(), st.begin() + 4, st.end());
    threw = false;
    try { b.loadState(bad); } catch (const std::exception&) { threw = true; }
    b.runFrame();
    CHECK(threw, "estado antigo (SAT1) é recusado");
  }
}

// ================================================================================================ homebrew

static uint32_t hw32(System& s, uint32_t a) { return s.busRead(a, 4); }

static void testHomebrew(const std::string& dir) {
  const std::string biosPath = dir + "/bios/teste.bin", cue = dir + "/disco/homebrew.cue";
  if (!std::filesystem::exists(biosPath) || !std::filesystem::exists(cue)) {
    std::printf("  (sem a BIOS de teste e o homebrew: rode tools/t7_saturn.sh)\n");
    skipped++;
    return;
  }
  const auto bios = readAll(biosPath);
  CHECK(isSaturnBios(bios), "BIOS de teste reconhecida");
  std::string err;
  auto disc = cdrom::open(cue, &err);
  CHECK(disc != nullptr, "abrir o .cue: %s", err.c_str());
  if (!disc) return;
  CHECK(cdrom::detectSystem(*disc) == "saturn", "detectSystem do homebrew");
  SaturnCore core(std::move(disc), nullptr, &bios);
  System& s = core.sys();
  const auto t0 = std::chrono::steady_clock::now();
  int f = 0;
  for (; f < 600; f++) {
    core.runFrame();
    if (hw32(s, 0x06080010) == 0xD0D0D0D0u && f > 30) break;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  homebrew: %d quadros em %.2f s (%.0f quadros/s); etapa da BIOS %u, erro 0x%X\n", f, secs, f / secs,
              hw32(s, 0x06000240), hw32(s, 0x06000248));
  CHECK(hw32(s, 0x06000240) == 6, "BIOS de teste chegou ao programa (etapa %u, erro 0x%X)", hw32(s, 0x06000240), hw32(s, 0x06000248));
  CHECK(hw32(s, 0x06080000) == 0x53415431u, "bloco de resultados");
  const uint32_t ok = hw32(s, 0x06080008), bad = hw32(s, 0x0608000C), n = hw32(s, 0x06080004);
  static const char* nomes[] = {"CPU", "DMA direto", "DMA indireto", "interrupções", "SMPC", "SH-2 escravo", "CD",
                                "DSP", "som (68000)", "VDP1", "RAM de backup", "sem interrupção estranha"};
  for (uint32_t i = 0; i < n && i < 12; i++)
    CHECK((ok >> i) & 1, "homebrew: %s (valor 0x%X%s)", nomes[i], hw32(s, 0x06080020 + i * 4), ((bad >> i) & 1) ? ", falhou" : ", não rodou");
  CHECK(hw32(s, 0x06080010) == 0xD0D0D0D0u, "homebrew terminou os testes");
  // imagem final (VDP1 por cima dos planos do VDP2)
  for (int k = 0; k < 3; k++) core.runFrame();
  auto px = [&](int x, int y) { return core.framebuffer()[y * core.width() + x]; };
  CHECK(core.width() == 320 && core.height() == 224, "resolução %dx%d", core.width(), core.height());
  CHECK(px(205, 25) == rgb(0x001F), "polígono vermelho do VDP1 (0x%08X)", px(205, 25));
  CHECK(px(230, 50) == rgb(0x3C0F), "polígono meio transparente sobre o vermelho (0x%08X)", px(230, 50));
  CHECK(px(250, 65) == rgb(0x7C00), "polígono azul");
  CHECK(px(250, 20) == rgb(0x001F) && px(254, 20) == rgb(0x7FFF) && px(254, 24) == rgb(0x001F), "sprite normal (xadrez)");
  CHECK(px(270, 20) == rgb(0x001F) && px(278, 20) == rgb(0x7FFF) && px(301, 51) == rgb(0x001F), "sprite ampliado 2x");
  CHECK(px(230, 100) != rgb(0x2842) && px(212, 82) == rgb(0x2842), "sprite distorcido (losango)");
  CHECK(px(10, 210) == rgb(0x7C00) && px(300, 215) == rgb(0x7C00), "linha");
  CHECK(px(5, 100) == rgb(0x2842), "cor de fundo");
  CHECK(px(100, 190) == rgb(0x8000 | 12 | 10 << 5), "bitmap RGB do NBG1 (0x%08X)", px(100, 190));
  bool squares = true;
  for (uint32_t i = 0; i < n && i < 12; i++) squares &= px(17 + int(i) * 24, 17) == rgb(((ok >> i) & 1) ? 0x03E0 : 0x001F);
  CHECK(squares && px(17, 17) == rgb(0x03E0), "quadrados verdes do NBG0");
  // controle: o homebrew lê o INTBACK a cada quadro
  core.setKeys(KEY_START | KEY_RIGHT);
  for (int k = 0; k < 4; k++) core.runFrame();
  CHECK((hw32(s, 0x06080018) >> 8) == 0x77, "controle lido pelo homebrew (0x%04X)", hw32(s, 0x06080018));
  core.setKeys(0);
  for (int k = 0; k < 4; k++) core.runFrame();
  CHECK((hw32(s, 0x06080018) >> 8) == 0xFF, "controle solto");
  // som: o 68000 toca a onda quadrada de 64 amostras (689 Hz)
  core.setSampleRate(44100);
  std::vector<float> L(core.audioAvailable()), R(L.size());
  core.pullAudio(L.data(), R.data(), L.size());
  for (int k = 0; k < 10; k++) core.runFrame();
  L.resize(core.audioAvailable()); R.resize(L.size());
  core.pullAudio(L.data(), R.data(), L.size());
  int ups = 0;
  for (size_t i = 1; i < L.size(); i++) if (L[i - 1] <= 0 && L[i] > 0) ups++;
  const double hz = L.empty() ? 0 : ups * 44100.0 / double(L.size());
  CHECK(hz > 640 && hz < 740, "som do canal 0: ~689 Hz (%.0f Hz em %zu amostras)", hz, L.size());
  CHECK(s.backupDirty && s.backup[0x80] == 'I' && s.backup[0x81] == 'N', "RAM de backup escrita pelo homebrew vira save");
  if (std::getenv("SATURN_DEBUG")) {
    for (uint32_t i = 0; i < 12; i++) std::printf("    teste %u: 0x%08X\n", i, hw32(s, 0x06080020 + i * 4));
    for (int k = 0; k < 30; k++) core.runFrame();
    const std::string ppm = std::string(std::getenv("SATURN_DEBUG")) + "/saturn.ppm";  // SATURN_DEBUG=pasta
    FILE* fp = std::fopen(ppm.c_str(), "wb");
    if (fp) {
    std::fprintf(fp, "P6\n%d %d\n255\n", core.width(), core.height());
    for (int i = 0; i < core.width() * core.height(); i++) { const uint32_t c = core.framebuffer()[i]; const uint8_t px[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)}; std::fwrite(px, 1, 3, fp); }
    std::fclose(fp);
    }
  }
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/saturn";
  std::printf("== detecção\n");
  testDetect();
  std::printf("== SCU: interrupções, DMA e DSP\n");
  testScuInterrupts();
  testScuDma();
  testScuDsp();
  std::printf("== SMPC\n");
  testSmpc();
  std::printf("== VDP1\n");
  testVdp1();
  testVdp1Lopr();
  std::printf("== VDP2\n");
  testVdp2();
  testVdp2Cc();
  testVdp2Extra();
  testVdp2Rot();
  std::printf("== SCSP e 68000\n");
  testScsp();
  testScspEnv();
  testScspLoops();
  testScspDsp();
  std::printf("== bloco de CD (disco sintético)\n");
  testCd();
  testCdState();
  testCdBuffers();
  testRobustez();
  std::printf("== dados aleatórios\n");
  testRandom();
  std::printf("== estado salvo\n");
  testState(dir);
  std::printf("== homebrew (BIOS de teste + CD próprio)\n");
  testHomebrew(dir);
  std::printf("RESULTADO saturn: %d verificações, %d falhas%s\n", checks, fails, skipped ? " (algumas partes puladas)" : "");
  return fails ? 1 : 0;
}
