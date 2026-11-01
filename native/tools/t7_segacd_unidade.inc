// Testes de unidade do Sega CD (incluído por t7_segacd.cpp): os mesmos casos aleatórios (t7_segacd_unidade.h)
// rodados no ASIC e no PCM do Índigo e no gfx.c/pcm.c do Genesis Plus GX (ref/unidade.txt, gerado pelo
// t7_segacd.sh); o resumo (hash da Word RAM, ou das amostras e dos endereços dos canais) tem de ser igual.
#include <map>

#include "t7_segacd_unidade.h"

namespace {
void unitTests() {
  std::ifstream f(g_dir + "/ref/unidade.txt");
  if (!f) { check(false, "falta ref/unidade.txt (rode o t7_segacd.sh)"); return; }
  std::map<std::string, std::string> ref;
  std::string tipo, hash;
  int k;
  while (f >> tipo >> k >> hash) ref[tipo + std::to_string(k)] = hash;
  auto c = makeCore();
  if (!c) return;
  int okA = 0, okP = 0, firstA = -1, firstP = -1;
  std::vector<uint8_t> w(0x40000);
  for (k = 0; k < UN_ASIC_CASOS; k++) {
    uint16_t r[9];
    un_asic(k, w.data(), r);
    c->cd().testAsic(w.data(), r);
    const auto& wr = c->cd().wordRam();
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < 0x40000; i += 2) h = un_hash(h, static_cast<uint32_t>(wr[i] << 8 | wr[i + 1]));
    h = un_hash(h, c->cd().reg(0x58));
    h = un_hash(h, c->cd().reg(0x64));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    if (ref["asic" + std::to_string(k)] == buf) okA++;
    else if (firstA < 0) firstA = k;
  }
  std::vector<un_op> ops(20000);
  for (k = 0; k < UN_PCM_CASOS; k++) {
    const int n = un_pcm(k, ops.data(), 20000);
    segacd::Rf5c164& p = c->cd().testPcm();
    p.reset();
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
      if (ops[i].tipo == 0) p.write(ops[i].a, static_cast<uint8_t>(ops[i].v));
      else
        for (int j = 0; j < ops[i].v; j++) {
          int32_t l, r;
          p.generate(l, r);
          h = un_hash(h, static_cast<uint32_t>(l));
          h = un_hash(h, static_cast<uint32_t>(r));
        }
    }
    for (int j = 0; j < 8; j++) h = un_hash(h, p.voicePosition(j));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    if (ref["pcm" + std::to_string(k)] == buf) okP++;
    else if (firstP < 0) firstP = k;
  }
  char buf[200];
  std::snprintf(buf, sizeof buf, "ASIC (rotação/escala): %d de %d casos aleatórios iguais ao gfx.c do GPGX (primeiro diferente: %d)",
                okA, UN_ASIC_CASOS, firstA);
  std::printf("  %s\n", buf);
  check(okA == UN_ASIC_CASOS, buf);
  std::snprintf(buf, sizeof buf, "PCM RF5C164: %d de %d casos aleatórios iguais ao pcm.c do GPGX (primeiro diferente: %d)",
                okP, UN_PCM_CASOS, firstP);
  std::printf("  %s\n", buf);
  check(okP == UN_PCM_CASOS, buf);
}
}  // namespace
