/* Rotinas de CD do PC-FX pelo SCSI do KING (BIOS e programa de teste do Índigo). Comando com seleção, bytes
   do comando com REQ/ACK, dados por E/S programada (PIO) ou por DMA para a KRAM, e status/mensagem no fim. */
#ifndef T7_PCFX_SCSI_H
#define T7_PCFX_SCSI_H
#include "t7_pcfx_hw.h"

static int scsi_wait(uint16_t mask, uint16_t want) {
  for (uint32_t n = 0; n < 4000000; n++)
    if ((kstat() & mask) == want) return 0;
  return -1;
}

/* manda um byte com o handshake (REQ do alvo, ACK nosso) */
static void scsi_ack(void) { kw(1, 0x10); kw(1, 0x00); }

/* seleciona o leitor e manda o comando; devolve 0 se o leitor aceitou */
static int scsi_command(const uint8_t* cdb, int len) {
  if (scsi_wait(S_BSY, 0)) return -1;
  kw(2, 0); kw(3, 0);
  kw(0, 0x81);   /* identificação do iniciador e do alvo no barramento */
  kw(1, 0x04);   /* SEL */
  if (scsi_wait(S_BSY, S_BSY)) return -2;
  kw(1, 0x00);
  for (int i = 0; i < len; i++) {
    if (scsi_wait(S_REQ, S_REQ)) return -3;
    if ((kstat() & (S_CD | S_IO | S_MSG)) != S_CD) return -4;
    kw(0, cdb[i]);
    scsi_ack();
  }
  return 0;
}

/* lê status e mensagem; devolve o status (0 = bom) */
static int scsi_finish(void) {
  int st = -1;
  for (;;) {
    if (scsi_wait(S_REQ | S_BSY, S_REQ | S_BSY)) return (kstat() & S_BSY) ? -5 : st;
    const uint16_t ph = kstat() & (S_CD | S_IO | S_MSG);
    const uint8_t b = (uint8_t)kr(0);
    if (ph == (S_CD | S_IO)) st = b >> 1;
    scsi_ack();
    if (ph == (S_CD | S_IO | S_MSG)) { scsi_wait(S_BSY, 0); return st; }
    if (ph == S_IO) continue; /* dados que sobraram */
  }
}

/* dados por PIO para a RAM (até max bytes); devolve o status */
static int scsi_pio(const uint8_t* cdb, int len, uint8_t* dst, uint32_t max, uint32_t* got) {
  int r = scsi_command(cdb, len);
  if (r) return r;
  uint32_t n = 0;
  for (;;) {
    if (scsi_wait(S_REQ, S_REQ)) return -6;
    if ((kstat() & (S_CD | S_IO | S_MSG)) != S_IO) break;
    const uint8_t b = (uint8_t)kr(0);
    if (n < max) dst[n] = b;
    n++;
    scsi_ack();
  }
  if (got) *got = n;
  return scsi_finish();
}

/* dados por DMA para a KRAM (página 0, endereço em palavras); devolve o status */
static int scsi_dma(const uint8_t* cdb, int len, uint32_t kaddr, uint32_t bytes) {
  int r = scsi_command(cdb, len);
  if (r) return r;
  kw32(0x09, kaddr);
  kw32(0x0A, bytes);
  kw(0x0B, 1);      /* DMA de verdade (sem interrupção) */
  kw(3, 1);         /* fase esperada: dados para o iniciador (I/O) */
  kw(2, 2);         /* modo DMA */
  kw(7, 0);         /* começa a receber */
  for (uint32_t n = 0; n < 40000000; n++) {
    if (kr(0x0B) & 1) break;          /* fim do DMA (lido zera a marca) */
    if ((kstat() & (S_CD | S_IO)) == (S_CD | S_IO)) break;  /* o leitor já foi para o status */
  }
  kw(2, 0);
  kr(7);            /* limpa a interrupção do SCSI */
  return scsi_finish();
}

static void cdb10(uint8_t* c, uint8_t op, uint32_t lba, uint16_t count) {
  c[0] = op; c[1] = 0;
  c[2] = (uint8_t)(lba >> 24); c[3] = (uint8_t)(lba >> 16); c[4] = (uint8_t)(lba >> 8); c[5] = (uint8_t)lba;
  c[6] = 0; c[7] = (uint8_t)(count >> 8); c[8] = (uint8_t)count; c[9] = 0;
}

/* copia palavras da KRAM (página 0) para a RAM pela porta de leitura com incremento */
static void kram_to_ram(uint32_t kaddr, uint16_t* dst, uint32_t words) {
  kw32(0x0C, kaddr | (1u << 18));
  out16(0x600, 0x0E);
  for (uint32_t i = 0; i < words; i++) dst[i] = in16(0x604);
}

/* primeira faixa de dados pelo READ TOC; devolve o LBA ou -1 */
static int32_t first_data_track(void) {
  uint8_t c[10] = {0x43, 0, 0, 0, 0, 0, 1, 0, 200, 0};
  uint8_t toc[200];
  uint32_t got = 0;
  if (scsi_pio(c, 10, toc, sizeof toc, &got) || got < 12) return -1;
  for (uint32_t o = 4; o + 8 <= got; o += 8)
    if (toc[o + 2] != 0xAA && (toc[o + 1] & 4))
      return (int32_t)((uint32_t)toc[o + 4] << 24 | (uint32_t)toc[o + 5] << 16 | (uint32_t)toc[o + 6] << 8 | toc[o + 7]);
  return -1;
}
#endif
