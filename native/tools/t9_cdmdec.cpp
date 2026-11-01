// Testes do controlador de CD-ROM e do MDEC do PlayStation 1 (check 9.5), contra o Beetle PSX (Mednafen)
// compilado fora do repositório pelo t9_cdmdec.sh:
//   1. MDEC: fluxos aleatórios (tabelas aleatórias ou a padrão, os 4 formatos de saída, com e sem sinal, bit 15)
//      decodificados pelo Beetle (ref/mdec.bin) e pelo Índigo; a saída na RAM (depois do DMA1) tem de ser igual
//      palavra por palavra;
//   2. CD-ROM: os roteiros de comandos (roteiros/*.txt, t9_cdmdec_driver.h) nos discos sintéticos; as
//      interrupções (tipo, bytes da resposta, setores lidos) têm de vir na mesma ordem e iguais, e o tempo entre
//      elas dentro da tolerância; o som capturado é alinhado e comparado (CD-DA igual amostra por amostra,
//      XA-ADPCM pela relação sinal/ruído, porque o reamostrador é diferente: o Índigo usa a tabela de 7 fases
//      do hardware);
//   3. comportamentos do hardware que o Beetle não emula (tampa aberta, sem disco, GetID de CD de música etc.),
//      conferidos contra o que o psx-spx documenta;
//   4. estados salvos: salvar -> carregar -> salvar dá os mesmos bytes, inclusive no meio de uma leitura, do XA e
//      de uma decodificação do MDEC, e a execução continua igual depois de carregar.
// uso: indigo-t9_cdmdec <pasta ps1cd>   (a pasta tem disco/, roteiros/ e ref/)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cdrom/cdrom.h"
#include "core/state.h"
#include "ps1/cdrom.h"
#include "ps1/mdec.h"

#include "t9_cdmdec_driver.h"

using namespace indigo;

static int g_ok = 0, g_bad = 0;
static void check(bool c, const std::string& what) {
  if (c) {
    g_ok++;
  } else {
    g_bad++;
    std::printf("  FALHOU: %s\n", what.c_str());
  }
}

static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> d;
  FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return d;
  std::fseek(f, 0, SEEK_END);
  d.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
  std::fclose(f);
  return d;
}

// ------------------------------------------------------------------ MDEC

static std::vector<uint32_t> runMdec(ps1::Mdec& m, const uint32_t* in, size_t n) {
  std::vector<uint32_t> out;
  size_t k = 0;  // posição do DMA1; cada palavra vai para k + deslocamento, como na RAM
  auto drain = [&]() {
    while (m.dmaOutRequest()) {
      int32_t off;
      const uint32_t v = m.dmaRead(off);
      const long a = long(k++) + off;
      if (a < 0 || a >= 65536) continue;
      if (size_t(a) >= out.size()) out.resize(size_t(a) + 1);
      out[size_t(a)] = v;
    }
  };
  for (size_t i = 0; i < n; i++) {
    drain();
    int guard = 0;
    while ((m.status() & 0x40000000u) && guard++ < 1000) {
      m.run(4096);
      drain();
    }
    m.dmaWrite(in[i]);
    m.run(64);
    drain();
  }
  for (int i = 0; i < 64; i++) {
    m.run(1 << 16);
    drain();
  }
  return out;
}

static void testMdec(const std::string& dir) {
  std::printf("== MDEC x Beetle PSX: fluxos aleatórios, 4 formatos, tabelas aleatórias\n");
  auto d = slurp(dir + "/ref/mdec.bin");
  if (d.empty()) {
    check(false, "ref/mdec.bin ausente");
    return;
  }
  size_t p = 0;
  int cases = 0, equal = 0;
  uint64_t words = 0;
  int perFmt[4] = {}, okFmt[4] = {};
  while (p + 4 <= d.size()) {
    uint32_t n;
    std::memcpy(&n, &d[p], 4);
    p += 4;
    std::vector<uint32_t> in(n);
    std::memcpy(in.data(), &d[p], n * 4);
    p += n * 4;
    uint32_t m;
    std::memcpy(&m, &d[p], 4);
    p += 4;
    std::vector<uint32_t> ref(m);
    std::memcpy(ref.data(), &d[p], m * 4);
    p += m * 4;
    // o formato está no comando 1 (o primeiro com os bits 31-29 = 1 depois das tabelas)
    const int fmt = in.size() > 66 ? int((in[66] >> 27) & 3) : 0;  // comando 1 depois das duas tabelas
    ps1::Mdec md;
    md.write(4, 0x60000000, 4);
    auto out = runMdec(md, in.data(), in.size());
    perFmt[fmt]++;
    words += ref.size();
    if (out == ref) {
      equal++;
      okFmt[fmt]++;
    } else if (g_bad < 20) {
      size_t k = 0;
      while (k < out.size() && k < ref.size() && out[k] == ref[k]) k++;
      std::printf("  caso %d (formato %d): %zu palavras x %zu na régua, primeira diferença na palavra %zu (%08x x %08x)\n",
                  cases, fmt, out.size(), ref.size(), k, k < out.size() ? out[k] : 0, k < ref.size() ? ref[k] : 0);
    }
    check(out == ref, "MDEC caso " + std::to_string(cases));
    cases++;
  }
  std::printf("  casos iguais: %d de %d (%llu palavras); por formato 4/8/24/15 bits: %d/%d %d/%d %d/%d %d/%d\n", equal,
              cases, (unsigned long long)words, okFmt[0], perFmt[0], okFmt[1], perFmt[1], okFmt[2], perFmt[2],
              okFmt[3], perFmt[3]);
}

// ------------------------------------------------------------------ MDEC: ps1-tests (resultados do console)

// carrega as tabelas do ps1-tests (common/mdec) como os programas deles fazem
static bool loadTables(ps1::Mdec& m, const std::string& T) {
  auto q = slurp(T + "/common/mdec/quant.bin"), id = slurp(T + "/common/mdec/idct.bin");
  if (q.size() != 128 || id.size() != 128) return false;
  m.write(4, 0x80000000u, 4);
  m.write(0, (2u << 29) | 1, 4);
  for (int i = 0; i < 32; i++) m.write(0, q[i * 4] | q[i * 4 + 1] << 8 | q[i * 4 + 2] << 16 | uint32_t(q[i * 4 + 3]) << 24, 4);
  m.write(0, 3u << 29, 4);
  for (int i = 0; i < 32; i++) m.write(0, id[i * 4] | id[i * 4 + 1] << 8 | id[i * 4 + 2] << 16 | uint32_t(id[i * 4 + 3]) << 24, 4);
  m.write(4, 0x60000000u, 4);
  return true;
}

static std::vector<uint32_t> decodeAll(ps1::Mdec& m, const std::vector<uint8_t>& bits, int depth) {
  const uint32_t words = uint32_t(bits.size() / 4);
  std::vector<uint32_t> in;
  in.push_back((1u << 29) | uint32_t(depth) << 27 | (words & 0xFFFF));
  for (uint32_t i = 0; i < words; i++)
    in.push_back(bits[i * 4] | bits[i * 4 + 1] << 8 | bits[i * 4 + 2] << 16 | uint32_t(bits[i * 4 + 3]) << 24);
  return runMdec(m, in.data(), in.size());
}

// maior diferença entre os componentes de 5 bits de duas cores de 15 bits
static int diff555(uint16_t a, uint16_t b) {
  int m = 0;
  for (int sh = 0; sh < 15; sh += 5) m = std::max(m, std::abs(int((a >> sh) & 31) - int((b >> sh) & 31)));
  return m;
}

static void testPs1TestsMdec(const std::string& dir) {
  std::printf("== MDEC x ps1-tests (JaCzekanski): resultados tirados de um console (diferença = bit menos significativo)\n");
  const std::string T = dir + "/ps1-tests";
  const int bad0 = g_bad;
  // 4bit e 8bit: o bloco 8x8 do coração, comparado com o hexdump do psx.log
  for (int depth = 0; depth < 2; depth++) {
    const std::string sub = T + (depth ? "/mdec/8bit" : "/mdec/4bit");
    auto bits = slurp(sub + "/heart.mdec");
    auto log = slurp(sub + "/psx.log");
    ps1::Mdec m;
    if (bits.empty() || log.empty() || !loadTables(m, T)) {
      check(false, "ps1-tests " + sub + " ausente");
      continue;
    }
    auto out = decodeAll(m, bits, depth);
    std::vector<uint8_t> got;
    for (uint32_t w : out)
      for (int k = 0; k < 4; k++) got.push_back(uint8_t(w >> (8 * k)));
    std::vector<uint8_t> want;
    std::string s(log.begin(), log.end());
    size_t p = s.find("mdec_readDecodedDma");
    p = s.find('\n', p);
    while (p != std::string::npos && p + 1 < s.size()) {
      const size_t e = s.find('\n', p + 1);
      const std::string l = s.substr(p + 1, (e == std::string::npos ? s.size() : e) - p - 1);
      const size_t c = l.find(':');
      if (c == std::string::npos || l.find("mdec") != std::string::npos) break;
      unsigned v;
      int used;
      const char* q = l.c_str() + c + 1;
      while (std::sscanf(q, "%x%n", &v, &used) == 1) {
        want.push_back(uint8_t(v));
        q += used;
      }
      p = e;
    }
    got.resize(std::min(got.size(), want.size()));
    if (std::getenv("T9_DEBUG")) {
      for (size_t i = 0; i < got.size(); i++) std::printf("%02x%s", got[i], (i % 8 == 7) ? "\n" : " ");
      for (size_t i = 0; i < want.size(); i++) std::printf("%02x%s", want[i], (i % 8 == 7) ? "\n" : " ");
    }
    int eq = 0, md = 0;
    for (size_t i = 0; i < got.size(); i++) {
      eq += got[i] == want[i];
      md = std::max(md, depth ? std::abs(int(got[i]) - int(want[i]))
                              : std::max(std::abs((got[i] & 15) - (want[i] & 15)), std::abs((got[i] >> 4) - (want[i] >> 4))));
    }
    std::printf("  %s: %d de %zu bytes iguais ao console, maior diferença %d\n", depth ? "8bit" : "4bit", eq, want.size(), md);
    // o 4 bits tem de ser idêntico; no 8 bits a IDCT (a do Beetle, a mais exata conhecida) erra o bit menos
    // significativo em alguns pixels: vale como piso, para não piorar
    check(!want.empty() && (depth ? eq >= 54 && md <= 1 : eq == int(want.size())), std::string("ps1-tests mdec/") + (depth ? "8bit" : "4bit"));
  }
  // frame: o pôr do sol de 320x240 em 15 e 24 bits, montado na VRAM em faixas de 16 pixels como o programa faz
  for (int v = 0; v < 2; v++) {
    const bool d24 = v == 1;
    auto bits = slurp(T + "/mdec/frame/sunset.mdec");
    auto vram = slurp(T + (d24 ? "/mdec/frame/vram-24bit.raw" : "/mdec/frame/vram-15bit.raw"));
    ps1::Mdec m;
    if (bits.empty() || vram.size() != 1024 * 512 * 2 || !loadTables(m, T)) {
      check(false, "ps1-tests mdec/frame ausente");
      continue;
    }
    auto out = decodeAll(m, bits, d24 ? 2 : 3);
    const int stripeW = d24 ? 24 : 16;  // meias-palavras por linha da faixa
    size_t equal = 0, total = 0;
    int maxd = 0;
    const size_t perStripe = size_t(stripeW) * 240 / 2;  // palavras
    for (int c = 0; c < 20; c++)
      for (int y = 0; y < 240; y++)
        for (int x = 0; x < stripeW; x++) {
          const size_t hw = size_t(c) * perStripe * 2 + size_t(y) * stripeW + size_t(x);
          const uint16_t mine = hw / 2 < out.size() ? uint16_t(out[hw / 2] >> (16 * (hw & 1))) : 0;
          const size_t vx = size_t(c * stripeW + x), off = (size_t(y) * 1024 + vx) * 2;
          const uint16_t real = uint16_t(vram[off] | vram[off + 1] << 8);
          equal += (mine & 0x7FFF) == real;
          total++;
          if (!d24) maxd = std::max(maxd, diff555(mine, real));
          else {  // 24 bits: bytes
            const uint16_t mm = mine & 0x7FFF;  // o PNG não guarda o bit 15
            // o byte de cima perdeu o bit 7 no PNG: a diferença é medida em módulo 128
            const int dh = std::abs(int(mm >> 8) - int(real >> 8));
            const int dd = std::max(std::abs(int(mm & 0xFF) - int(real & 0xFF)), std::min(dh, 128 - dh));
            maxd = std::max(maxd, dd);
          }
        }
    std::printf("  frame %s: %zu de %zu meias-palavras da VRAM iguais ao console (%.1f%%), maior diferença %d\n",
                d24 ? "24 bits" : "15 bits", equal, total, 100.0 * double(equal) / double(total), maxd);
    // em 24 bits um erro de 1 no Y e no Cr/Cb chega a alguns níveis no byte de cor
    check(equal >= (d24 ? 78043u : 61667u) && maxd <= (d24 ? 4 : 1), std::string("ps1-tests mdec/frame ") + (d24 ? "24 bits" : "15 bits"));
  }
  // step-by-step-log: escreve e lê palavra por palavra como no psx.log, conferindo os dados lidos e o status
  {
    auto log = slurp(T + "/mdec/step-by-step-log/psx.log");
    ps1::Mdec m;
    if (log.empty() || !loadTables(m, T)) {
      check(false, "ps1-tests mdec/step-by-step-log ausente");
    } else {
      std::string s(log.begin(), log.end());
      size_t p = s.find("mdec_decode(");
      unsigned len = 0;
      std::sscanf(s.c_str() + s.find("length=", p) + 7, "%x", &len);
      m.write(0, (1u << 29) | (3u << 27) | ((len / 4) & 0xFFFF), 4);  // length= está em bytes
      int reads = 0, readsOk = 0, stats = 0, statsOk = 0, maxd = 0;
      while ((p = s.find('\n', p)) != std::string::npos) {
        p++;
        unsigned v;
        if (std::sscanf(s.c_str() + p, "==== MDEC_STATUS: 0x%x", &v) == 1) {
          m.run(20000);
          stats++;
          statsOk += m.status() == v;
          if (std::getenv("T9_DEBUG") && m.status() != v) std::printf("status %d: %08x x %08x\n", stats, m.status(), v);
        } else if (std::sscanf(s.c_str() + p, "MDEC_DATA <- 0x%x", &v) == 1) {
          m.write(0, v, 4);
        } else if (std::sscanf(s.c_str() + p, "MDEC_DATA -> 0x%x", &v) == 1) {
          reads++;
          const uint32_t got = m.read(0, 4);
          readsOk += got == v;
          maxd = std::max({maxd, diff555(uint16_t(got), uint16_t(v)), diff555(uint16_t(got >> 16), uint16_t(v >> 16))});
          if (std::getenv("T9_DEBUG") && reads < 40) std::printf("dado %08x x %08x\n", got, v);
        }
      }
      std::printf("  step-by-step-log: %d de %d palavras lidas iguais ao console (maior diferença %d); status igual em %d de %d leituras\n",
                  readsOk, reads, maxd, statsOk, stats);
      check(reads == 512 && readsOk >= 469 && maxd <= 1, "ps1-tests mdec/step-by-step-log: dados");
      check(stats == 777 && statsOk == stats, "ps1-tests mdec/step-by-step-log: status");
    }
  }
  std::printf("  %s\n", g_bad == bad0 ? "todos conferem" : "há diferenças");
}

// ------------------------------------------------------------------ CD-ROM: roteiros

// tolerância de tempo entre duas interrupções seguidas: TOL_ABS ciclos ou TOL_REL do intervalo, o que for maior
static const double TOL_ABS = 70000;  // ~2,1 ms
static const double TOL_REL = 0.10;

struct Ev {
  bool mark = false;
  uint64_t t = 0;
  std::string text;
};

static std::vector<Ev> parseLog(const std::string& s) {
  std::vector<Ev> v;
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find('\n', p);
    if (e == std::string::npos) e = s.size();
    std::string l = s.substr(p, e - p);
    p = e + 1;
    if (l.empty()) continue;
    Ev ev;
    if (l.compare(0, 5, "MARK ") == 0) {
      ev.mark = true;
      ev.text = l.substr(5);
    } else if (l.compare(0, 2, "t=") == 0) {
      const size_t sp = l.find(' ');
      ev.t = std::strtoull(l.c_str() + 2, nullptr, 10);
      ev.text = sp == std::string::npos ? "" : l.substr(sp + 1);
    } else {
      ev.text = l;
    }
    v.push_back(ev);
  }
  return v;
}

struct Seg {
  std::string mark;
  std::vector<Ev> ev;
};
static std::vector<Seg> segments(const std::vector<Ev>& v) {
  std::vector<Seg> s(1);
  for (const auto& e : v) {
    if (e.mark) s.push_back(Seg{e.text, {}});
    else s.back().ev.push_back(e);
  }
  return s;
}

struct TimeStats {
  int n = 0, worst = 0;
  double maxAbs = 0, maxRel = 0;
  std::string absWhere, relWhere;
};
static TimeStats g_time;
static int g_firstFree = 0;

// compara os registros; devolve a descrição da primeira diferença ("" se iguais)
static std::string compareLogs(const std::string& mine, const std::string& ref, int* events) {
  auto sm = segments(parseLog(mine)), sr = segments(parseLog(ref));
  if (sm.size() != sr.size()) return "número de marcas diferente";
  uint64_t lastM = 0, lastR = 0;
  *events = 0;
  for (size_t i = 0; i < sm.size(); i++) {
    if (sm[i].mark != sr[i].mark) return "marca '" + sm[i].mark + "' x '" + sr[i].mark + "'";
    const bool skip = sm[i].mark.compare(0, 2, "~ ") == 0;
    const bool freeTime = sm[i].mark.compare(0, 3, "~t ") == 0;  // conteúdo comparado, tempo livre
    if (skip) {
      if (!sm[i].ev.empty()) lastM = sm[i].ev.back().t;
      if (!sr[i].ev.empty()) lastR = sr[i].ev.back().t;
      continue;
    }
    const auto &a = sm[i].ev, &b = sr[i].ev;
    for (size_t k = 0; k < std::max(a.size(), b.size()); k++) {
      if (k >= a.size()) return "[" + sm[i].mark + "] falta no Índigo: " + b[k].text;
      if (k >= b.size()) return "[" + sm[i].mark + "] a mais no Índigo: " + a[k].text;
      if (a[k].text != b[k].text) return "[" + sm[i].mark + "] Índigo '" + a[k].text + "' x régua '" + b[k].text + "'";
      (*events)++;
      const double dm = double(a[k].t - lastM), dr = double(b[k].t - lastR);
      lastM = a[k].t;
      lastR = b[k].t;
      if (freeTime) continue;
      // o primeiro setor depois do aceite da leitura: o Índigo segue o tempo medido no console (ps1-tests
      // cdrom/timing: busca + 2 setores, sem a demora de voltar da pausa que o Beetle soma)
      if (a[k].text.compare(0, 4, "INT1") == 0 && k > 0 && a[k - 1].text.compare(0, 4, "INT3") == 0) {
        g_firstFree++;
        continue;
      }
      const double diff = std::fabs(dm - dr), lim = std::max(TOL_ABS, TOL_REL * std::max(dm, dr));
      g_time.n++;
      char where[200];
      std::snprintf(where, sizeof(where), "%s (%.0f x %.0f ciclos)", a[k].text.substr(0, 40).c_str(), dm, dr);
      if (diff > g_time.maxAbs) {
        g_time.maxAbs = diff;
        g_time.absWhere = where;
      }
      if (dr > 0 && diff / dr > g_time.maxRel && diff > 1000) {
        g_time.maxRel = diff / dr;
        g_time.relWhere = where;
      }
      if (diff > lim) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[%s] tempo de '%s': %.0f ciclos x %.0f na régua", sm[i].mark.c_str(),
                      a[k].text.c_str(), dm, dr);
        return buf;
      }
    }
  }
  return "";
}

// som de saída (44,1 kHz): CD-DA tem de sair igual; no XA o reamostrador do Beetle (25 pontos, fase contínua) e o
// do Índigo (tabela de 7 fases do hardware, grupos de 6 -> 7) diferem na fase fracionária, então vale a relação
// sinal/ruído mínima abaixo
static const double AUDIO_SNR_MIN = 25;

// som: alinha pelo começo do sinal e compara (iguais, ou relação sinal/ruído)
struct AudioCmp {
  size_t n = 0, equal = 0;
  double snr = 0;
  int shift = 0;
};
static AudioCmp compareAudio(const std::vector<int16_t>& m, const std::vector<int16_t>& r) {
  AudioCmp c;
  auto first = [](const std::vector<int16_t>& v) {
    for (size_t i = 0; i < v.size(); i++)
      if (std::abs(v[i]) > 256) return i / 2;
    return v.size() / 2;
  };
  const long fm = long(first(m)), fr = long(first(r));
  double best = -1e9;
  for (int sh = -48; sh <= 48; sh++) {
    const long om = fm + sh;  // amostra do Índigo alinhada com a fr da régua
    if (om < 0) continue;
    const long len = std::min<long>(long(m.size() / 2) - om, long(r.size() / 2) - fr) - 600;
    if (len <= 0) continue;
    double sig = 0, err = 0;
    size_t eq = 0;
    for (long i = 0; i < len; i++)
      for (int ch = 0; ch < 2; ch++) {
        const double x = r[size_t((fr + i) * 2 + ch)], y = m[size_t((om + i) * 2 + ch)];
        sig += x * x;
        err += (x - y) * (x - y);
        if (x == y) eq++;
      }
    const double snr = err == 0 ? 200.0 : 10 * std::log10(sig / err);
    if (snr > best) {
      best = snr;
      c.snr = snr;
      c.n = size_t(len) * 2;
      c.equal = eq;
      c.shift = int(om - fr);
    }
  }
  return c;
}

struct Port {
  ps1::CdRom cd;
  std::unique_ptr<cdrom::Disc> disc;
  std::string dir, refLog;
  bool capture = false;
  std::vector<int16_t> audio, xa;
  std::vector<std::string> audioReport;
  bool audioFail = false;
  bool beetleRound = false;
};

static void runScript(const std::string& dir, const std::string& name, int* evOut, std::string* err,
                      bool beetleRound = false) {
  const std::string script = dir + "/roteiros/" + name + ".txt";
  auto text = slurp(script);
  std::string s(text.begin(), text.end());
  std::string discName = "none";
  const size_t dp = s.find("# disco: ");
  if (dp != std::string::npos) discName = s.substr(dp + 9, s.find('\n', dp) - dp - 9);
  Port P;
  P.beetleRound = beetleRound;
  P.cd.setXaBeetleRounding(beetleRound);
  P.dir = dir;
  P.refLog = dir + "/ref/cd/" + name + ".log";
  if (discName != "none") {
    P.disc = cdrom::open(dir + "/disco/" + discName + ".cue");
    P.cd.setDisc(P.disc.get());
  }
  P.cd.reset();
  P.cd.onXaSample = [&P](int16_t l, int16_t r) {
    if (P.capture) {
      P.xa.push_back(l);
      P.xa.push_back(r);
    }
  };
  P.cd.onAudio = [&P](int16_t l, int16_t r) {
    if (P.capture) {
      P.audio.push_back(l);
      P.audio.push_back(r);
    }
  };
  T9Drv d;
  std::memset(&d, 0, sizeof(d));
  d.p.ctx = &P;
  d.p.rd = [](void* c, int a) { return uint8_t(static_cast<Port*>(c)->cd.read(uint32_t(a), 1)); };
  d.p.wr = [](void* c, int a, uint8_t v) { static_cast<Port*>(c)->cd.write(uint32_t(a), v, 1); };
  d.p.run = [](void* c, int n) { static_cast<Port*>(c)->cd.run(n); };
  d.p.dma = [](void* c) { return static_cast<Port*>(c)->cd.dmaRead(); };
  d.p.lid = [](void* c, int o) { static_cast<Port*>(c)->cd.setLidOpen(o != 0); };
  d.p.disc = [](void* c, const char* nm) {
    Port* p = static_cast<Port*>(c);
    std::unique_ptr<cdrom::Disc> nd;
    if (std::strcmp(nm, "none")) nd = cdrom::open(p->dir + "/disco/" + nm + ".cue");
    p->cd.setDisc(nd.get());
    p->disc = std::move(nd);
  };
  d.p.audio = [](void* c, int on, int idx) {
    Port* p = static_cast<Port*>(c);
    if (on) {
      p->audio.clear();
      p->xa.clear();
      p->capture = true;
      return;
    }
    p->capture = false;
    char buf[200];
    // XA decodificado (antes do reamostrador): tem de bater quase exatamente
    auto xr = slurp(p->refLog + ".x" + std::to_string(idx));
    if (!xr.empty()) {
      std::vector<int16_t> xref(xr.size() / 2);
      std::memcpy(xref.data(), xr.data(), xref.size() * 2);
      const size_t n = std::min(xref.size(), p->xa.size());
      size_t eq = 0;
      int maxd = 0;
      double sig = 0, err = 0;
      for (size_t i = 0; i < n; i++) {
        const int dd = std::abs(int(xref[i]) - int(p->xa[i]));
        maxd = std::max(maxd, dd);
        eq += dd == 0;
        sig += double(xref[i]) * xref[i];
        err += double(dd) * dd;
      }
      const double snr = err == 0 ? 200.0 : 10 * std::log10(sig / err);
      std::snprintf(buf, sizeof(buf), "XA decodificado %d: %zu amostras (régua %zu), %.1f%% iguais, maior diferença %d, SNR %.1f dB",
                    idx, n, xref.size(), n ? 100.0 * double(eq) / double(n) : 0.0, maxd, snr);
      p->audioReport.push_back(buf);
      // com o arredondamento do Beetle tem de ser idêntico; com o do psx-spx a diferença do arredondamento passa
      // pelo filtro (ganho alto nas frequências baixas) e fica perto de 37 dB
      if (n < xref.size() * 9 / 10 || (p->beetleRound ? maxd != 0 : snr < 30)) p->audioFail = true;
    }
    auto raw = slurp(p->refLog + ".a" + std::to_string(idx));
    std::vector<int16_t> ref(raw.size() / 2);
    if (!raw.empty()) std::memcpy(ref.data(), raw.data(), ref.size() * 2);
    // trecho mudo dos dois lados: iguais se ambos forem silêncio
    bool silentM = true, silentR = true;
    for (int16_t x : p->audio) silentM &= std::abs(x) <= 256;
    for (int16_t x : ref) silentR &= std::abs(x) <= 256;
    if (silentM || silentR) {
      std::snprintf(buf, sizeof(buf), "som %d: silêncio (Índigo %s, régua %s)", idx, silentM ? "mudo" : "com som",
                    silentR ? "mudo" : "com som");
      if (silentM != silentR) p->audioFail = true;
    } else {
      const AudioCmp a = compareAudio(p->audio, ref);
      std::snprintf(buf, sizeof(buf), "som %d: %zu amostras, %.1f%% iguais, SNR %.1f dB, defasagem %d", idx, a.n,
                    a.n ? 100.0 * double(a.equal) / double(a.n) : 0.0, a.snr, a.shift);
      if (a.n < 20000 || a.snr < AUDIO_SNR_MIN) p->audioFail = true;
    }
    p->audioReport.push_back(buf);
  };
  std::string outDir = dir + "/out";
  std::string outPath = outDir + "/" + name + ".log";
  d.log = std::fopen(outPath.c_str(), "w");
  if (!d.log) {
    std::string cmd = "mkdir -p '" + outDir + "'";
    if (std::system(cmd.c_str()) == 0) d.log = std::fopen(outPath.c_str(), "w");
  }
  if (!d.log) {
    *err = "não consegui gravar " + outPath;
    return;
  }
  for (int i = 0; i < 40000000 / T9_POLL; i++) P.cd.run(T9_POLL);
  t9_script(&d, script.c_str());
  std::fclose(d.log);
  auto mine = slurp(outPath), ref = slurp(P.refLog);
  *err = compareLogs(std::string(mine.begin(), mine.end()), std::string(ref.begin(), ref.end()), evOut);
  for (const auto& r : P.audioReport) std::printf("    %s\n", r.c_str());
  if (err->empty() && P.audioFail) *err = "som diferente";
}

static void testCdScripts(const std::string& dir) {
  std::printf("== CD-ROM x Beetle PSX: roteiros de comandos nos discos sintéticos\n");
  static const char* NAMES[] = {"01_basico", "02_leitura",  "03_leitura2340", "04_busca",   "05_cdda",
                                "06_report", "07_autopause", "08_xa",         "09_motor", "10_regioes",
                                "11_cd_musica"};
  int total = 0;
  for (const char* n : NAMES) {
    int ev = 0;
    std::string err;
    runScript(dir, n, &ev, &err);
    std::printf("  %-16s %3d interrupções  %s\n", n, ev, err.empty() ? "iguais" : ("DIFERENTE: " + err).c_str());
    check(err.empty(), std::string("roteiro ") + n);
    total += ev;
  }
  {
    int ev = 0;
    std::string err;
    runScript(dir, "08_xa", &ev, &err, true);
    std::printf("  %-16s %3d interrupções  %s\n", "08_xa (Beetle)", ev, err.empty() ? "iguais" : ("DIFERENTE: " + err).c_str());
    check(err.empty(), "roteiro 08_xa com o arredondamento do Beetle");
  }
  std::printf("  interrupções comparadas: %d; tempo: %d intervalos conferidos (tolerância: %.0f ciclos ou %.0f%%)\n",
              total, g_time.n, TOL_ABS, 100 * TOL_REL);
  std::printf("    (%d primeiros setores depois do aceite da leitura ficam sem conferir o tempo, veja compareLogs)\n", g_firstFree);
  std::printf("    maior diferença absoluta: %.0f ciclos em %s\n", g_time.maxAbs, g_time.absWhere.c_str());
  std::printf("    maior diferença relativa: %.1f%% em %s\n", 100 * g_time.maxRel, g_time.relWhere.c_str());
}

// ------------------------------------------------------------------ comportamentos do psx-spx (sem a régua)

struct Mini {
  ps1::CdRom cd;
  std::unique_ptr<cdrom::Disc> disc;
  int irqs = 0;
  explicit Mini(const std::string& cue) {
    if (!cue.empty()) disc = cdrom::open(cue);
    cd.setDisc(disc.get());
    cd.reset();
    cd.onIrq = [this]() { irqs++; };
    cd.write(0, 1, 1);
    cd.write(2, 0x1F, 1);
    run(40000000);
  }
  void run(int64_t c) {
    while (c > 0) {
      const int s = int(std::min<int64_t>(c, 4096));
      cd.run(s);
      c -= s;
    }
  }
  void send(uint8_t c, std::initializer_list<uint8_t> p = {}) {
    cd.write(0, 0, 1);
    for (uint8_t v : p) cd.write(2, v, 1);
    cd.write(1, c, 1);
  }
  // espera a próxima interrupção: "INTn bytes..." (ou "nada")
  std::string next(int64_t limit = 200000000) {
    for (int64_t t = 0; t < limit; t += 256) {
      cd.run(256);
      cd.write(0, 1, 1);
      const int f = cd.read(3, 1) & 7;
      if (!f) continue;
      std::string s = "INT" + std::to_string(f);
      while (cd.read(0, 1) & 0x20) {
        char b[4];
        std::snprintf(b, sizeof(b), " %02x", cd.read(1, 1));
        s += b;
      }
      cd.write(0, 1, 1);
      cd.write(3, 0x1F, 1);
      return s;
    }
    return "nada";
  }
  std::string cmd(uint8_t c, std::initializer_list<uint8_t> p = {}) {
    send(c, p);
    return next();
  }
};

static void expect(const std::string& got, const std::string& want, const std::string& what) {
  if (got != want) std::printf("    %s: '%s' (esperado '%s')\n", what.c_str(), got.c_str(), want.c_str());
  check(got == want, what);
}

static void testHardware(const std::string& dir) {
  std::printf("== CD-ROM: comportamentos documentados no psx-spx que o Beetle não emula\n");
  const int bad0 = g_bad;
  const std::string teste = dir + "/disco/teste.cue";
  {  // sem disco
    Mini m("");
    expect(m.cmd(0x01), "INT3 10", "sem disco: Getstat");
    expect(m.cmd(0x1A), "INT3 10", "sem disco: GetID (1ª)");
    expect(m.next(), "INT5 08 40 00 00 00 00 00 00", "sem disco: GetID (2ª)");
    expect(m.cmd(0x06), "INT5 11 80", "sem disco: ReadN");
    expect(m.cmd(0x02, {0, 2, 0}), "INT5 11 80", "sem disco: Setloc");
    expect(m.cmd(0x13), "INT5 11 80", "sem disco: GetTN");
    expect(m.cmd(0x0A), "INT3 10", "sem disco: Init (1ª)");
    expect(m.next(), "INT2 10", "sem disco: Init (2ª)");
  }
  {  // tampa
    Mini m(teste);
    expect(m.cmd(0x01), "INT3 12", "tampa: Getstat inicial (tampa foi aberta)");
    expect(m.cmd(0x01), "INT3 02", "tampa: Getstat");
    m.cd.setLidOpen(true);
    // no console (ps1-tests cdrom/disc-swap, psx.log) o INT5 da tampa vem com 01h e o Getstat mostra o motor
    // ainda girando (12h) antes de parar (10h)
    expect(m.next(), "INT5 01 08", "tampa: abrir dá INT5 08h");
    expect(m.cmd(0x01), "INT3 12", "tampa aberta: Getstat (motor parando)");
    expect(m.cmd(0x1A), "INT5 13 80", "tampa aberta: GetID");
    m.run(20000000);
    expect(m.cmd(0x01), "INT3 10", "tampa aberta: Getstat (motor parado)");
    expect(m.cmd(0x19, {0x21}), "INT3 03", "tampa aberta: Test 21h (chaves)");
    m.cd.setLidOpen(false);
    expect(m.cmd(0x1A), "INT5 11 80", "tampa fechada, motor partindo: GetID");
    m.run(40000000);
    expect(m.cmd(0x01), "INT3 12", "tampa fechada: Getstat ainda avisa");
    expect(m.cmd(0x01), "INT3 02", "tampa fechada: Getstat limpo");
    expect(m.cmd(0x1A), "INT3 02", "tampa fechada: GetID (1ª)");
    expect(m.next(), "INT2 02 00 20 00 53 43 45 41", "tampa fechada: GetID (2ª)");
  }
  {  // CD de música, região, comandos raros
    Mini a(dir + "/disco/audio.cue");
    a.cmd(0x01);
    expect(a.cmd(0x1A), "INT3 02", "CD de música: GetID (1ª)");
    expect(a.next(), "INT5 0a 90 00 00 00 00 00 00", "CD de música: GetID (2ª)");
    Mini e(dir + "/disco/europa.cue");
    expect(e.cmd(0x19, {0x22}), "INT3 66 6f 72 20 45 75 72 6f 70 65", "disco europeu: Test 22h \"for Europe\"");
    Mini m(teste);
    m.cmd(0x01);
    expect(m.cmd(0x04), "INT5 03 80", "Forward fora do Play");
    expect(m.cmd(0x19, {0x05}), "INT3 01 01", "Test 05h: contadores do SCEx");
    expect(m.cmd(0x12, {0x00}), "INT5 03 10", "SetSession 0");
    expect(m.cmd(0x12, {0x02}), "INT3 02", "SetSession 2 (1ª)");
    expect(m.next(), "INT5 06 40", "SetSession 2 (2ª)");
    expect(m.cmd(0x1D, {0x01, 0x02}), "INT3 02", "GetQ faixa 2 (1ª)");
    expect(m.next(), "INT2 01 00 02 00 00 00 00 00 24 00 00", "GetQ faixa 2 (2ª)");
    // comando inválido não tira os parâmetros da FIFO
    expect(m.cmd(0x20, {0x01, 0x02}), "INT5 03 40", "comando 20h inválido");
    expect(m.cmd(0x01), "INT5 03 20", "Getstat com os parâmetros que sobraram");
    m.cd.write(0, 1, 1);
    m.cd.write(3, 0x40, 1);
    expect(m.cmd(0x01), "INT3 02", "Getstat depois de limpar os parâmetros");
    expect(m.cmd(0x1C), "INT3 02", "Reset (1C)");
    expect(m.cmd(0x01), "INT3 00", "Getstat logo depois do Reset (motor partindo)");
    m.run(0x500000);
    expect(m.cmd(0x01), "INT3 02", "Getstat depois do Reset");
    // HSTS: FIFO de parâmetros, ocupado
    m.cd.write(0, 0, 1);
    for (int i = 0; i < 16; i++) m.cd.write(2, uint8_t(i), 1);
    check((m.cd.read(0, 1) & 0x18) == 0x00, "HSTS: FIFO de parâmetros cheia");
    m.cd.write(0, 1, 1);
    m.cd.write(3, 0x40, 1);
    check((m.cd.read(0, 1) & 0x18) == 0x18, "HSTS: FIFO de parâmetros vazia");
    m.send(0x01);
    check((m.cd.read(0, 1) & 0x80) != 0, "HSTS: ocupado logo depois do comando");
    m.next();
    check((m.cd.read(0, 1) & 0x80) == 0, "HSTS: livre depois da resposta");
    // leitura pelos registradores (sem DMA): 8 e 16 bits, e o byte repetido depois do fim
    m.cmd(0x0E, {0x80});
    m.cmd(0x02, {0x00, 0x02, 0x20});
    m.cmd(0x06);
    expect(m.next(), "INT1 22", "ReadN: INT1");
    m.cd.write(0, 0, 1);
    m.cd.write(3, 0x80, 1);
    const uint32_t w0 = m.cd.read(2, 2);
    const uint32_t b2 = m.cd.read(2, 1);
    check(w0 == 0x0014 && b2 == 0x00, "dados pelo registrador em 16 e 8 bits");
    uint8_t sec[2048];
    sec[0] = uint8_t(w0);
    sec[1] = uint8_t(w0 >> 8);
    sec[2] = uint8_t(b2);
    for (int i = 3; i < 2048; i++) sec[i] = uint8_t(m.cd.read(2, 1));
    check((m.cd.read(0, 1) & 0x40) == 0, "DRQSTS apaga depois do setor inteiro");
    check(m.cd.read(2, 1) == sec[2048 - 8], "depois do fim repete o byte [800h-8]");
    m.cmd(0x09);
    m.next();
  }
  {  // tempos medidos no console pelo ps1-tests (cdrom/timing/psx.log), em ciclos da CPU
    Mini m(teste);
    m.cmd(0x01);
    auto timed = [&](uint8_t c, std::initializer_list<uint8_t> p) {
      m.send(c, p);
      int64_t t = 0;
      std::string r;
      while ((r = m.next(256)) == "nada" && t < 100000000) t += 256;
      return t;
    };
    auto since = [&]() {
      int64_t t = 0;
      while (m.next(256) == "nada" && t < 100000000) t += 256;
      return t;
    };
    const int64_t nop = timed(0x01, {});
    const int64_t initAck = timed(0x0A, {});
    const int64_t initDone = since();
    m.cmd(0x0E, {0x00});
    const int64_t setloc = timed(0x02, {0x00, 0x02, 0x40});
    timed(0x06, {});
    const int64_t first = since();
    const int64_t next = since();
    m.cmd(0x09);
    m.next();
    std::printf("  tempos: Nop %lld (console 24024..137104), Init %lld/%lld (69456..93696 / 138448..600632), Setloc %lld "
                "(33856..182160), 1º setor 1x %lld (668488..984136), setor seguinte %lld (416216..468808)\n",
                (long long)nop, (long long)initAck, (long long)initDone, (long long)setloc, (long long)first, (long long)next);
    check(nop >= 24024 && nop <= 137104, "tempo do Nop dentro do medido no console");
    check(initAck >= 69456 && initAck <= 93696, "tempo do INT3 do Init dentro do medido no console");
    check(initDone >= 138448 && initDone <= 600632, "tempo do INT2 do Init dentro do medido no console");
    check(setloc >= 33856 && setloc <= 182160, "tempo do Setloc dentro do medido no console");
    check(first >= 668488 && first <= 984136, "primeiro setor (1x) dentro do medido no console");
    check(next >= 416216 && next <= 468808, "intervalo entre setores (1x) dentro do medido no console");
  }
  {  // borda da IRQ: uma chamada por interrupção
    Mini m(teste);
    const int before = m.irqs;
    m.cmd(0x01);
    m.cmd(0x01);
    check(m.irqs - before == 2, "onIrq uma vez por interrupção");
  }
  std::printf("  %s\n", g_bad == bad0 ? "todos conferem" : "há diferenças");
}

// ------------------------------------------------------------------ estados salvos

static std::vector<uint8_t> saveCd(ps1::CdRom& c) {
  StateIO w;
  c.save(w);
  return w.data();
}

static void testStates(const std::string& dir) {
  std::printf("== Estados salvos: salvar -> carregar -> salvar, e a execução continua igual\n");
  const int bad0 = g_bad;
  struct Case {
    const char* name;
    std::initializer_list<uint8_t> mode;
    std::initializer_list<uint8_t> loc;
    uint8_t cmd;
    int64_t runBefore;
  };
  const Case cases[] = {
      {"no meio da leitura (ReadN 2x)", {0x80}, {0x00, 0x02, 0x20}, 0x06, 1800000},
      {"durante a busca", {0x80}, {0x00, 0x15, 0x00}, 0x06, 300000},
      {"tocando XA", {0xC8}, {0x00, 0x06, 0x00}, 0x1B, 6000000},
      {"tocando CD-DA", {0x00}, {0x00, 0x24, 0x00}, 0x03, 9000000},
  };
  for (const auto& c : cases) {
    Mini a(dir + "/disco/teste.cue");
    a.cmd(0x0D, {0x01, 0x01});
    a.cmd(0x0E, c.mode);
    a.cmd(0x02, c.loc);
    a.send(c.cmd);
    a.run(c.runBefore + 12345);  // fora dos limites de setor e de amostra
    const auto s1 = saveCd(a.cd);
    Mini b(dir + "/disco/teste.cue");
    StateIO r(s1);
    b.cd.load(r);
    const auto s2 = saveCd(b.cd);
    check(s1 == s2, std::string("CD-ROM ") + c.name + ": salvar->carregar->salvar");
    // as duas continuam iguais: mesmas interrupções, mesmos dados e mesmo som
    std::vector<int16_t> sa, sb;
    a.cd.onAudio = [&sa](int16_t l, int16_t r) { sa.push_back(int16_t(l ^ r)); };
    b.cd.onAudio = [&sb](int16_t l, int16_t r) { sb.push_back(int16_t(l ^ r)); };
    std::string la, lb;
    for (int i = 0; i < 6; i++) {
      la += a.next(20000000) + ";";
      lb += b.next(20000000) + ";";
    }
    a.run(3000000);
    b.run(3000000);
    check(la == lb && sa == sb && saveCd(a.cd) == saveCd(b.cd), std::string("CD-ROM ") + c.name + ": continua igual");
  }
  {  // estados danificados: carregam sem sair dos limites e a execução segue (rodar com os sanitizadores)
    Mini a(dir + "/disco/teste.cue");
    a.cmd(0x0E, {0xC8});
    a.cmd(0x02, {0x00, 0x06, 0x00});
    a.send(0x1B);
    a.run(5000000);
    const auto good = saveCd(a.cd);
    uint32_t r = 12345;
    int ran = 0;
    for (int i = 0; i < 40; i++) {
      auto bad = good;
      for (int k = 0; k < 64; k++) {
        r = r * 1103515245 + 12345;
        bad[(r >> 8) % bad.size()] = uint8_t(r >> 24);
      }
      Mini b(dir + "/disco/teste.cue");
      StateIO rd(bad);
      b.cd.load(rd);
      b.run(2000000);
      for (int k = 0; k < 600; k++) (void)b.cd.dmaRead();
      ran++;
    }
    ps1::Mdec m;
    std::vector<uint8_t> junk(20000);
    for (int i = 0; i < 40; i++) {
      for (auto& x : junk) {
        r = r * 1103515245 + 12345;
        x = uint8_t(r >> 24);
      }
      StateIO w;
      m.save(w);
      auto st = w.data();
      for (size_t k = 0; k < st.size(); k++) st[k] = junk[k % junk.size()];
      StateIO rd(st);
      m.load(rd);
      for (int k = 0; k < 300; k++) {
        m.dmaWrite(r = r * 1103515245 + 12345);
        m.run(600);
        { int32_t off; (void)m.dmaRead(off); }
        (void)m.read(0, 4);
      }
    }
    check(ran == 40, "estados danificados carregam e rodam");
  }
  {  // MDEC no meio de um macrobloco
    auto d = slurp(dir + "/ref/mdec.bin");
    if (d.size() > 8) {
      uint32_t n;
      size_t p = 0;
      for (int skip = 0; skip < 2; skip++) {  // o 3º caso (24 bits)
        std::memcpy(&n, &d[p], 4);
        p += 4 + n * 4;
        uint32_t m2;
        std::memcpy(&m2, &d[p], 4);
        p += 4 + m2 * 4;
      }
      std::memcpy(&n, &d[p], 4);
      std::vector<uint32_t> in(n);
      std::memcpy(in.data(), &d[p + 4], n * 4);
      ps1::Mdec a;
      a.write(4, 0x60000000, 4);
      const size_t half = 66 + (n - 66) / 2;
      auto outA = runMdec(a, in.data(), half);
      StateIO w;
      a.save(w);
      ps1::Mdec b;
      StateIO r(w.data());
      b.load(r);
      StateIO w2;
      b.save(w2);
      check(w.data() == w2.data(), "MDEC no meio do fluxo: salvar->carregar->salvar");
      auto ra = runMdec(a, in.data() + half, n - half), rb = runMdec(b, in.data() + half, n - half);
      check(ra == rb, "MDEC: continua igual depois de carregar");
    }
  }
  std::printf("  %s\n", g_bad == bad0 ? "todos conferem" : "há diferenças");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "uso: indigo-t9_cdmdec pasta_ps1cd\n");
    return 2;
  }
  const std::string dir = argv[1];
  testMdec(dir);
  testPs1TestsMdec(dir);
  testCdScripts(dir);
  testHardware(dir);
  testStates(dir);
  std::printf("RESULTADO t9_cdmdec: %d ok, %d falhas\n", g_ok, g_bad);
  return g_bad ? 1 : 0;
}
