/* Régua do t9_cdmdec: o MDEC (mdec.c) e o controlador de CD-ROM (cdc.c) do Beetle PSX (Mednafen, GPL),
 * compilados FORA do repositório pelo t9_cdmdec.sh (só como régua; nada do código deles entra no Índigo).
 *   t9_cdmdec_ref mdec <saida.bin> <casos> <semente>
 *       gera fluxos aleatórios para o MDEC (tabelas aleatórias ou a padrão, coeficientes válidos, os 4 formatos
 *       de saída, sinal e bit 15) e grava, para cada caso, as palavras de entrada e a saída como fica na RAM
 *       depois do DMA1 (o Beetle devolve o deslocamento de cada palavra; aqui ele é aplicado)
 *   t9_cdmdec_ref cd <pasta dos discos> <disco|none> <roteiro> <registro>
 *       roda o roteiro (t9_cdmdec_driver.h) no cdc.c, com um CDIF próprio que lê o .cue/.bin sintético e gera
 *       o subcanal Q, e grava o registro das interrupções e o som capturado (<registro>.a<n>, 16 bits estéreo) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mednafen/psx/cdc.h"
#include "mednafen/psx/mdec.h"
#include "mednafen/cdrom/CDUtility.h"

#include "t9_cdmdec_driver.h"

/* ------------------------------------------------------------------ o que o resto do Beetle forneceria */
int32_t EventCycles = 1 << 28;
int32_t psx_overclock_factor = 0;
unsigned cd_2x_speedup = 1;
bool cd_async = false;
bool cd_warned_slow = false;
int64_t cd_slow_timeout = 8000;
PS_CDC* PSX_CDC;
void DMA_NoteActivity(void) {}
bool PSX_AnyEventVirtual(void) { return false; }
void PSX_WakeVirtual(bool f) { (void)f; }
uint32_t PSX_GetRandU32(uint32_t mina, uint32_t maxa) { (void)maxa; return mina; }
void PSX_SetEventNT(const int type, const int32_t t) { (void)type; (void)t; }
void IRQ_Assert(int which, bool a) { (void)which; (void)a; }
void osd_message(unsigned p, int l, int t, int ty, const char* fmt, ...) { (void)p; (void)l; (void)t; (void)ty; (void)fmt; }
int MDFNSS_StateAction(void* st, int load, int data_only, SFORMAT* sf, const char* name) {
  (void)st; (void)load; (void)data_only; (void)sf; (void)name;
  return 1;
}
void VCD_FeedSector(const uint8_t* s, int32_t l) { (void)s; (void)l; }
int VCD_GetMode(void) { return 0; }
void VCD_SetAVSwitch(bool b) { (void)b; }
void VCD_SioExchange(const uint8_t* a, uint8_t* b) { (void)a; (void)b; }
int CheckEDC(const unsigned char* a, bool b) { (void)a; (void)b; return 1; }
int ValidateRawSector(unsigned char* f, bool x) { (void)f; (void)x; return 1; }
bool Init_LEC_Correct(void) { return true; }
void Kill_LEC_Correct(void) {}
void lec_tables_init(void) {}
void lec_encode_mode0_sector(uint32_t a, uint8_t* s) { (void)a; (void)s; }
void lec_encode_mode1_sector(uint32_t a, uint8_t* s) { (void)a; (void)s; }
void lec_encode_mode2_sector(uint32_t a, uint8_t* s) { (void)a; (void)s; }
void lec_encode_mode2_form1_sector(uint32_t a, uint8_t* s) { (void)a; (void)s; }
void lec_encode_mode2_form2_sector(uint32_t a, uint8_t* s) { (void)a; (void)s; }
void SPU_Power(void) {}

static int16_t* g_audio;
static size_t g_audioN, g_audioCap;
static int g_capture;
static int32_t g_spuAcc;
static int16_t* g_xa;
static size_t g_xaN, g_xaCap;
static int g_xaSeen;
/* guarda cada buffer de XA decodificado (antes do reamostrador) quando ele começa a tocar */
static void grab_xa(void) {
  CD_Audio_Buffer* ab = &PSX_CDC->AudioBuffer;
  uint32_t i;
  if (ab->ReadPos) {
    g_xaSeen = 0;
    return;
  }
  if (g_xaSeen || !ab->Size || (ab->Freq != 3 && ab->Freq != 6)) return;
  g_xaSeen = 1;
  for (i = 0; i < ab->Size; i++) {
    if (g_xaN + 2 > g_xaCap) {
      g_xaCap = g_xaCap ? g_xaCap * 2 : 1 << 16;
      g_xa = (int16_t*)realloc(g_xa, g_xaCap * sizeof(int16_t));
    }
    g_xa[g_xaN++] = ab->Samples[0][i];
    g_xa[g_xaN++] = ab->Samples[1][i];
  }
}
int32_t SPU_UpdateFromCDC(int32_t clocks) {
  g_spuAcc += clocks;
  while (g_spuAcc >= 768) {
    int32_t s[2];
    g_spuAcc -= 768;
    if (g_capture) grab_xa();
    CDC_GetCDAudioSample(s);
    if (g_capture) {
      if (g_audioN + 2 > g_audioCap) {
        g_audioCap = g_audioCap ? g_audioCap * 2 : 1 << 16;
        g_audio = (int16_t*)realloc(g_audio, g_audioCap * sizeof(int16_t));
      }
      g_audio[g_audioN++] = (int16_t)s[0];
      g_audio[g_audioN++] = (int16_t)s[1];
    }
  }
  return 768 - g_spuAcc;
}

/* ------------------------------------------------------------------ CDIF: .cue/.bin sintético */
struct CDIF {
  FILE* f;
  int n;
  int type[100]; /* 0 áudio, 1 Mode1, 2 Mode2 */
  int32_t i0[100], i1[100];
  int32_t total;
  TOC toc;
};

static CDIF* cdif_open(const char* dir, const char* name) {
  char path[1024], line[512], bin[512] = "";
  FILE* c;
  CDIF* d;
  long sz;
  int i;
  snprintf(path, sizeof(path), "%s/%s.cue", dir, name);
  c = fopen(path, "r");
  if (!c) return NULL;
  d = (CDIF*)calloc(1, sizeof(CDIF));
  while (fgets(line, sizeof(line), c)) {
    int num, m, s, f, idx;
    char typ[64];
    if (sscanf(line, " FILE \"%511[^\"]\"", bin) == 1) continue;
    if (sscanf(line, " TRACK %d %63s", &num, typ) == 2) {
      d->type[d->n] = !strncmp(typ, "AUDIO", 5) ? 0 : (!strncmp(typ, "MODE1", 5) ? 1 : 2);
      d->i0[d->n] = d->i1[d->n] = -1;
      d->n++;
    } else if (sscanf(line, " INDEX %d %d:%d:%d", &idx, &m, &s, &f) == 4 && d->n) {
      if (idx == 0) d->i0[d->n - 1] = (m * 60 + s) * 75 + f;
      else if (idx == 1) d->i1[d->n - 1] = (m * 60 + s) * 75 + f;
    }
  }
  fclose(c);
  snprintf(path, sizeof(path), "%s/%s", dir, bin);
  d->f = fopen(path, "rb");
  if (!d->f) return NULL;
  fseek(d->f, 0, SEEK_END);
  sz = ftell(d->f);
  d->total = (int32_t)(sz / 2352);
  TOC_Clear(&d->toc);
  d->toc.first_track = 1;
  d->toc.last_track = (uint8_t)d->n;
  for (i = 0; i < d->n; i++) {
    if (d->i0[i] < 0) d->i0[i] = d->i1[i];
    d->toc.tracks[i + 1].adr = 1;
    d->toc.tracks[i + 1].control = d->type[i] ? 4 : 0;
    d->toc.tracks[i + 1].lba = (uint32_t)d->i1[i];
    d->toc.tracks[i + 1].valid = true;
    if (d->type[i] == 2) d->toc.disc_type = 0x20;
  }
  d->toc.tracks[100].adr = 1;
  d->toc.tracks[100].control = d->toc.tracks[d->n].control;
  d->toc.tracks[100].lba = (uint32_t)d->total;
  d->toc.tracks[100].valid = true;
  return d;
}

/* subcanal Q como o CDAccess_Image do Mednafen gera (posição, índice 0 no pregap, tempo relativo) */
static void cdif_q(CDIF* d, int32_t lba, uint8_t* pw) {
  uint8_t q[12];
  int t = 0, i;
  uint32_t rel, a;
  for (i = 0; i < d->n; i++)
    if (lba >= d->i0[i] && lba < (i + 1 < d->n ? d->i0[i + 1] : d->total)) t = i;
  rel = (uint32_t)abs(lba - d->i1[t]);
  a = (uint32_t)lba + 150;
  memset(q, 0, sizeof(q));
  q[0] = (uint8_t)(1 | ((d->type[t] ? 4 : 0) << 4));
  q[1] = U8_to_BCD((uint8_t)(t + 1));
  q[2] = U8_to_BCD(lba < d->i1[t] ? 0 : 1);
  q[3] = U8_to_BCD((uint8_t)(rel / 75 / 60));
  q[4] = U8_to_BCD((uint8_t)(rel / 75 % 60));
  q[5] = U8_to_BCD((uint8_t)(rel % 75));
  q[7] = U8_to_BCD((uint8_t)(a / 75 / 60));
  q[8] = U8_to_BCD((uint8_t)(a / 75 % 60));
  q[9] = U8_to_BCD((uint8_t)(a % 75));
  subq_generate_checksum(q);
  for (i = 0; i < 96; i++) pw[i] = (uint8_t)(((q[i >> 3] >> (7 - (i & 7))) & 1) << 6);
}

void CDIF_ReadTOC(CDIF* d, TOC* out) { *out = d->toc; }

bool CDIF_ReadRawSector(CDIF* d, uint8_t* buf, uint32_t lba, int64_t timeout) {
  (void)timeout;
  if ((int32_t)lba < 0 || (int32_t)lba >= d->total) {
    memset(buf, 0, 2352 + 96);
    return false;
  }
  fseek(d->f, (long)lba * 2352, SEEK_SET);
  if (fread(buf, 1, 2352, d->f) != 2352) memset(buf, 0, 2352);
  cdif_q(d, (int32_t)lba, buf + 2352);
  return true;
}

bool CDIF_ReadRawSectorPWOnly(CDIF* d, uint8_t* buf, uint32_t lba, bool hint) {
  (void)hint;
  if ((int32_t)lba < 0 || (int32_t)lba >= d->total) {
    memset(buf, 0, 96);
    return false;
  }
  cdif_q(d, (int32_t)lba, buf);
  return true;
}

/* região pela licença no setor 4 (como o libretro faz) */
static const char* disc_id(CDIF* d) {
  uint8_t raw[2352 + 96];
  int i;
  if (!d->n || d->type[0] == 0) return NULL;
  if (!CDIF_ReadRawSector(d, raw, 4, 0)) return NULL;
  for (i = 24; i + 32 < 24 + 2048; i++) {
    if (memcmp(raw + i, "Sony Computer Entertainment ", 28)) continue;
    if (!memcmp(raw + i + 28, "Amer", 4)) return "SCEA";
    if (!memcmp(raw + i + 28, "Euro", 4)) return "SCEE";
    if (!memcmp(raw + i + 28, "Inc", 3)) return "SCEI";
  }
  return "SCEA"; /* sem licença: como com um modchip */
}

/* ------------------------------------------------------------------ porta do roteiro */
static PS_CDC g_cdc;
static int32_t g_ts;
static const char* g_dir;
static const char* g_log;
static CDIF* g_disc;

static uint8_t p_rd(void* c, int a) { (void)c; return PS_CDC_Read(&g_cdc, g_ts, 0x1F801800 + a); }
static void p_wr(void* c, int a, uint8_t v) { (void)c; PS_CDC_Write(&g_cdc, g_ts, 0x1F801800 + a, v); }
static void p_run(void* c, int cyc) {
  (void)c;
  g_ts += cyc;
  PS_CDC_Update(&g_cdc, g_ts);
  if (g_ts > (1 << 29)) {
    PS_CDC_ResetTS(&g_cdc);
    g_ts = 0;
  }
}
static uint32_t p_dma(void* c) { (void)c; return PS_CDC_DMARead(&g_cdc); }
static void p_lid(void* c, int open) {
  (void)c;
  if (open) PS_CDC_SetDisc(&g_cdc, true, NULL, NULL);
  else PS_CDC_SetDisc(&g_cdc, false, g_disc, g_disc ? disc_id(g_disc) : NULL);
}
static void p_disc(void* c, const char* name) {
  (void)c;
  g_disc = strcmp(name, "none") ? cdif_open(g_dir, name) : NULL;
  PS_CDC_SetDisc(&g_cdc, false, g_disc, g_disc ? disc_id(g_disc) : NULL);
}
static void p_audio(void* c, int on, int idx) {
  (void)c;
  if (on) {
    g_audioN = 0;
    g_xaN = 0;
    g_xaSeen = 0;
    g_capture = 1;
  } else {
    char path[1024];
    FILE* f;
    g_capture = 0;
    snprintf(path, sizeof(path), "%s.a%d", g_log, idx);
    f = fopen(path, "wb");
    if (f) {
      fwrite(g_audio, sizeof(int16_t), g_audioN, f);
      fclose(f);
    }
    if (g_xaN) {
      snprintf(path, sizeof(path), "%s.x%d", g_log, idx);
      f = fopen(path, "wb");
      if (f) {
        fwrite(g_xa, sizeof(int16_t), g_xaN, f);
        fclose(f);
      }
    }
  }
}

static int run_cd(const char* dir, const char* disc, const char* script, const char* log) {
  T9Drv d;
  g_dir = dir;
  g_log = log;
  PS_CDC_Init(&g_cdc);
  PSX_CDC = &g_cdc;
  PS_CDC_Power(&g_cdc);
  g_disc = strcmp(disc, "none") ? cdif_open(dir, disc) : NULL;
  if (strcmp(disc, "none") && !g_disc) {
    fprintf(stderr, "disco %s/%s não abriu\n", dir, disc);
    return 1;
  }
  if (g_disc) PS_CDC_SetDisc(&g_cdc, false, g_disc, disc_id(g_disc));
  memset(&d, 0, sizeof(d));
  d.p.rd = p_rd;
  d.p.wr = p_wr;
  d.p.run = p_run;
  d.p.dma = p_dma;
  d.p.lid = p_lid;
  d.p.disc = p_disc;
  d.p.audio = p_audio;
  d.log = fopen(log, "w");
  if (!d.log) return 1;
  /* 1,2 s para o motor partir, igual dos dois lados */
  {
    int i;
    for (i = 0; i < 40000000 / T9_POLL; i++) p_run(NULL, T9_POLL);
  }
  t9_script(&d, script);
  fclose(d.log);
  return 0;
}

/* ------------------------------------------------------------------ MDEC */
static uint32_t g_rng;
static uint32_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static const uint16_t STD_SCALE[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x7D8A, 0x6A6D, 0x471C, 0x18F8, 0xE707,
    0xB8E3, 0x9592, 0x8275, 0x7641, 0x30FB, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FB, 0x7641, 0x6A6D, 0xE707,
    0x8275, 0xB8E3, 0x471C, 0x7D8A, 0x18F8, 0x9592, 0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x5A82, 0xA57D, 0xA57D,
    0x5A82, 0x471C, 0x8275, 0x18F8, 0x6A6D, 0x9592, 0xE707, 0x7D8A, 0xB8E3, 0x30FB, 0x89BE, 0x7641, 0xCF04,
    0xCF04, 0x7641, 0x89BE, 0x30FB, 0x18F8, 0xB8E3, 0x6A6D, 0x8275, 0x7D8A, 0x9592, 0x471C, 0xE707};

typedef struct {
  uint32_t* w;
  size_t n, cap;
} Vec;
static void vpush(Vec* v, uint32_t x) {
  if (v->n == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 256;
    v->w = (uint32_t*)realloc(v->w, v->cap * 4);
  }
  v->w[v->n++] = x;
}

static uint32_t* g_ram;
static size_t g_k, g_nram;
static void drain(void) {
  while (!(MDEC_Read(0, 4) & 0x80000000u)) {
    uint32_t offs, v = MDEC_DMARead(&offs);
    const long a = (long)g_k + (int32_t)offs;
    if (a >= 0 && a < 65536) {
      g_ram[a] = v;
      if ((size_t)a + 1 > g_nram) g_nram = (size_t)a + 1;
    }
    g_k++;
  }
}

static uint16_t nofe(uint16_t h) { return h == 0xFE00 ? 0xFE01 : h; }

static void gen_block(Vec* h) {
  int idx = 1;
  uint32_t qs = (rnd() & 1) ? 1 + rnd() % 16 : rnd() % 64;
  if (rnd() % 8 == 0) vpush(h, 0xFE00);
  vpush(h, nofe((uint16_t)(qs << 10 | (rnd() & 0x3FF))));
  while (idx < 64) {
    uint32_t run, lvl;
    if (rnd() % 6 == 0) {
      vpush(h, 0xFE00);
      return;
    }
    run = (rnd() % 3 == 0) ? rnd() % (uint32_t)(64 - idx) : rnd() % 3;
    if ((int)run + idx > 63) run = (uint32_t)(63 - idx);
    lvl = (rnd() & 3) ? (rnd() % 64) - 32 : rnd();
    vpush(h, nofe((uint16_t)(run << 10 | (lvl & 0x3FF))));
    idx += (int)run + 1;
  }
}

static int run_mdec(const char* out, int cases, uint32_t seed) {
  FILE* f = fopen(out, "wb");
  int c;
  if (!f) return 1;
  g_rng = seed ? seed : 1;
  for (c = 0; c < cases; c++) {
    Vec in = {0, 0, 0}, h = {0, 0, 0};
    uint32_t* ram;
    size_t nram = 0, i;
    int depth = c % 4, mb, nmb = 1 + (int)(rnd() % 6), b;
    uint32_t cmd;
    uint8_t q[128];
    uint32_t tmp;
    /* tabelas */
    vpush(&in, 0x40000001);
    for (i = 0; i < 128; i++) q[i] = (uint8_t)((rnd() % 16 == 0) ? 0 : ((rnd() & 1) ? 1 + rnd() % 32 : 1 + rnd() % 255));
    for (i = 0; i < 32; i++) vpush(&in, q[i * 4] | q[i * 4 + 1] << 8 | q[i * 4 + 2] << 16 | (uint32_t)q[i * 4 + 3] << 24);
    vpush(&in, 0x60000000);
    for (i = 0; i < 32; i++) {
      uint16_t a, bb;
      if (c % 3) {
        a = STD_SCALE[i * 2];
        bb = STD_SCALE[i * 2 + 1];
      } else {
        a = (uint16_t)rnd();
        bb = (uint16_t)rnd();
      }
      vpush(&in, a | (uint32_t)bb << 16);
    }
    /* fluxo */
    for (mb = 0; mb < nmb; mb++)
      for (b = 0; b < ((depth & 2) ? 6 : 4); b++) gen_block(&h);
    if (h.n & 1) vpush(&h, 0xFE00);
    if (rnd() & 1)
      while ((h.n / 2) % 32) {
        vpush(&h, 0xFE00);
        vpush(&h, 0xFE00);
      }
    cmd = (1u << 29) | (uint32_t)depth << 27 | (rnd() & 1) << 26 | (rnd() & 1) << 25 | (uint32_t)(h.n / 2);
    vpush(&in, cmd);
    for (i = 0; i < h.n; i += 2) vpush(&in, h.w[i] | (uint32_t)h.w[i + 1] << 16);
    /* roda o MDEC do Beetle */
    MDEC_Power();
    MDEC_Write(0, 4, 0x60000000);
    ram = (uint32_t*)calloc(65536, 4);
    g_ram = ram;
    g_k = 0;
    g_nram = 0;
    for (i = 0; i < in.n; i++) {
      drain();
      while (MDEC_Read(0, 4) & 0x40000000u) {
        MDEC_Run(1 << 16);
        drain();
      }
      MDEC_Write(0, 0, in.w[i]);
      MDEC_Run(1 << 12);
      drain();
    }
    for (i = 0; i < 16; i++) {
      MDEC_Run(1 << 20);
      drain();
    }
    nram = g_nram;
    tmp = (uint32_t)in.n;
    fwrite(&tmp, 4, 1, f);
    fwrite(in.w, 4, in.n, f);
    tmp = (uint32_t)nram;
    fwrite(&tmp, 4, 1, f);
    fwrite(ram, 4, nram, f);
    free(ram);
    free(in.w);
    free(h.w);
  }
  fclose(f);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 5 && !strcmp(argv[1], "mdec")) return run_mdec(argv[2], atoi(argv[3]), (uint32_t)strtoul(argv[4], 0, 0));
  if (argc >= 6 && !strcmp(argv[1], "cd")) return run_cd(argv[2], argv[3], argv[4], argv[5]);
  fprintf(stderr, "uso: t9_cdmdec_ref mdec saida casos semente | cd pasta disco roteiro registro\n");
  return 2;
}
