/* Régua dos testes de unidade do Sega CD: roda os casos de t7_segacd_unidade.h no gfx.c (ASIC) e no pcm.c (RF5C164)
 * do próprio Genesis Plus GX, compilados com um shared.h mínimo (t7_segacd.sh), e escreve um resumo por caso:
 *   asic <caso> <hash da Word RAM depois da operação>
 *   pcm <caso> <hash das amostras e dos endereços dos canais>
 * As amostras do PCM saem do blip_buf substituído aqui por um registro dos valores (sem reamostrar). */
#include "shared.h"
#include "t7_segacd_unidade.h"

calco_ext_t ext;
calco_s68k_t s68k;
calco_config_t config;
calco_snd_t snd;
int8 audio_hard_disable = 0;
int8 reset_do_not_clear_buffers = 0;
void s68k_update_irq(unsigned int mask) { (void)mask; }
void error(char* fmt, ...) { (void)fmt; }

/* blip_buf trocado: guarda o valor absoluto de cada amostra */
static int g_l, g_r, g_out[2][4096], g_n;
void blip_add_delta_fast(blip_t* b, unsigned t, int dl, int dr) {
  (void)b;
  g_l += dl; g_r += dr;
  if (t < 4096) { g_out[0][t] = g_l; g_out[1][t] = g_r; if ((int)t + 1 > g_n) g_n = (int)t + 1; }
}
void blip_end_frame(blip_t* b, unsigned t) { (void)b; (void)t; }
void blip_clear(blip_t* b) { (void)b; }
void blip_set_rates(blip_t* b, double c, double r) { (void)b; (void)c; (void)r; }
int blip_clocks_needed(const blip_t* b, int n) { (void)b; return n; }

extern void pcm_run(unsigned int length);

static uint8_t w[0x40000];

int main(void) {
  int k;
  config.pcm_volume = 100;
  gfx_init();
  for (k = 0; k < UN_ASIC_CASOS; k++) {
    uint16_t r[9];
    uint32_t i;
    uint64_t h = 1469598103934665603ull;
    un_asic(k, w, r);
    for (i = 0; i < 0x40000; i++) WRITE_BYTE(scd.word_ram_2M, i, w[i]);
    memset(scd.regs, 0, sizeof scd.regs);
    scd.regs[0x02 >> 1].byte.l = (uint8)(r[8] << 3);   /* 2M, Word RAM do sub, prioridade */
    scd.regs[0x58 >> 1].w = r[0];
    scd.regs[0x5a >> 1].w = r[1];
    scd.regs[0x5c >> 1].w = r[2];
    scd.regs[0x5e >> 1].w = r[3];
    scd.regs[0x60 >> 1].w = r[4];
    scd.regs[0x62 >> 1].w = r[5];
    scd.regs[0x64 >> 1].w = r[6];
    scd.regs[0x66 >> 1].w = r[7];
    gfx_start(r[7], 0);
    gfx_update(100000000);
    for (i = 0; i < 0x40000; i += 2) h = un_hash(h, (uint32_t)(READ_BYTE(scd.word_ram_2M, i) << 8 | READ_BYTE(scd.word_ram_2M, i + 1)));
    h = un_hash(h, scd.regs[0x58 >> 1].w);
    h = un_hash(h, scd.regs[0x64 >> 1].w);
    printf("asic %d %016llx\n", k, (unsigned long long)h);
  }
  {
    static un_op ops[20000];
    for (k = 0; k < UN_PCM_CASOS; k++) {
      int n = un_pcm(k, ops, 20000), i, j;
      uint64_t h = 1469598103934665603ull;
      pcm_reset();
      g_l = g_r = 0;
      for (i = 0; i < n; i++) {
        if (ops[i].tipo == 0) pcm_write(ops[i].a, (unsigned char)ops[i].v, scd.pcm_hw.cycles);
        else {
          int len = ops[i].v;
          g_n = 0;
          /* amostras que o chip não registrou (desligado, saída já zerada) repetem o valor atual */
          for (j = 0; j < 4096; j++) { g_out[0][j] = g_l; g_out[1][j] = g_r; }
          pcm_run((unsigned)len);
          for (j = 0; j < len; j++) {
            if (j > 0 && j >= g_n) { g_out[0][j] = g_out[0][j - 1]; g_out[1][j] = g_out[1][j - 1]; }
            h = un_hash(h, (uint32_t)g_out[0][j]);
            h = un_hash(h, (uint32_t)g_out[1][j]);
          }
        }
      }
      for (j = 0; j < 8; j++) { h = un_hash(h, scd.pcm_hw.chan[j].addr); }
      printf("pcm %d %016llx\n", k, (unsigned long long)h);
    }
  }
  return 0;
}
