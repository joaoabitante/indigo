/* Roteiro de comandos do controlador de CD-ROM do PS1, o mesmo para o Índigo (t9_cdmdec.cpp) e para a régua
 * (t9_cdmdec_ref.c, compilado fora do repositório com o cdc.c do Beetle PSX). Faz o papel do programa da CPU:
 * escreve os comandos, consulta a interrupção a cada POLL ciclos, lê as respostas, lê o setor pelo DMA (canal 3)
 * nos INT1 e reconhece a interrupção. Cada interrupção vira uma linha no registro:
 *   t=<ciclo> INT<n> <bytes da resposta> [D <hash do setor> <8 primeiros bytes>]
 * Linhas do roteiro:
 *   cmd XX [p1 p2 ...]   manda o comando XX (hex) com os parâmetros (hex)
 *   irq N [limite]        roda até chegarem N interrupções (limite em ciclos, padrão 400 milhões)
 *   wait C                roda C ciclos atendendo as interrupções
 *   rd N                  bytes de dados lidos em cada INT1 (0 = não lê)
 *   vol a b c d           volumes L->L, L->R, R->R, R->L (hex) e aplica
 *   reg i a v             escreve v (hex) no registrador a do índice i
 *   lid 0|1               fecha/abre a tampa
 *   disc nome|none        troca o disco
 *   audio 1|0             começa/termina a captura do som (44,1 kHz)
 *   mark texto            linha de marcação no registro
 * Código C99 que também compila como C++. */
#ifndef T9_CDMDEC_DRIVER_H
#define T9_CDMDEC_DRIVER_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T9_POLL 256

typedef struct T9Port {
  void* ctx;
  uint8_t (*rd)(void* ctx, int a);
  void (*wr)(void* ctx, int a, uint8_t v);
  void (*run)(void* ctx, int cycles);
  uint32_t (*dma)(void* ctx);
  void (*lid)(void* ctx, int open);
  void (*disc)(void* ctx, const char* name);
  void (*audio)(void* ctx, int on, int idx);
} T9Port;

typedef struct T9Drv {
  T9Port p;
  FILE* log;
  uint64_t t;
  int rdsize;
  int audioIdx;
} T9Drv;

/* atende uma interrupção pendente; devolve 1 se havia */
static int t9_service(T9Drv* d) {
  uint8_t f, b[16], data[2340];
  int n = 0, i;
  char line[512];
  size_t len;
  d->p.wr(d->p.ctx, 0, 1);
  f = (uint8_t)(d->p.rd(d->p.ctx, 3) & 7);
  if (!f) return 0;
  while ((d->p.rd(d->p.ctx, 0) & 0x20) && n < 16) b[n++] = d->p.rd(d->p.ctx, 1);
  len = (size_t)snprintf(line, sizeof(line), "t=%llu INT%d", (unsigned long long)d->t, f);
  for (i = 0; i < n; i++) len += (size_t)snprintf(line + len, sizeof(line) - len, " %02x", b[i]);
  if (f == 1 && d->rdsize > 0) {
    uint32_t h = 2166136261u;
    d->p.wr(d->p.ctx, 0, 0);
    d->p.wr(d->p.ctx, 3, 0x80);
    for (i = 0; i < d->rdsize / 4; i++) {
      uint32_t w = d->p.dma(d->p.ctx);
      data[i * 4] = (uint8_t)w;
      data[i * 4 + 1] = (uint8_t)(w >> 8);
      data[i * 4 + 2] = (uint8_t)(w >> 16);
      data[i * 4 + 3] = (uint8_t)(w >> 24);
    }
    for (i = 0; i < d->rdsize; i++) h = (h ^ data[i]) * 16777619u;
    d->p.wr(d->p.ctx, 3, 0x00);
    len += (size_t)snprintf(line + len, sizeof(line) - len, " D %08x", h);
    for (i = 0; i < 8; i++) len += (size_t)snprintf(line + len, sizeof(line) - len, " %02x", data[i]);
  }
  d->p.wr(d->p.ctx, 0, 1);
  d->p.wr(d->p.ctx, 3, 0x1F);
  fprintf(d->log, "%s\n", line);
  return 1;
}

static void t9_step(T9Drv* d, int c) {
  d->p.run(d->p.ctx, c);
  d->t += (uint64_t)c;
}

static void t9_script(T9Drv* d, const char* path) {
  FILE* f = fopen(path, "r");
  char line[512];
  if (!f) {
    fprintf(d->log, "ERRO roteiro %s\n", path);
    return;
  }
  d->p.wr(d->p.ctx, 0, 1);
  d->p.wr(d->p.ctx, 2, 0x1F); /* HINTMSK */
  while (fgets(line, sizeof(line), f)) {
    char op[32];
    char* rest;
    int k;
    if (sscanf(line, "%31s", op) != 1 || op[0] == '#') continue;
    rest = strstr(line, op) + strlen(op);
    if (!strcmp(op, "cmd")) {
      unsigned v[17];
      int n = 0, off = 0, used;
      while (n < 17 && sscanf(rest + off, "%x%n", &v[n], &used) == 1) {
        off += used;
        n++;
      }
      if (n < 1) continue;
      d->p.wr(d->p.ctx, 0, 0);
      for (k = 1; k < n; k++) d->p.wr(d->p.ctx, 2, (uint8_t)v[k]);
      d->p.wr(d->p.ctx, 1, (uint8_t)v[0]);
    } else if (!strcmp(op, "irq")) {
      long long n = 1, lim = 400000000LL, got = 0, spent = 0;
      sscanf(rest, "%lld %lld", &n, &lim);
      while (got < n && spent < lim) {
        t9_step(d, T9_POLL);
        spent += T9_POLL;
        got += t9_service(d);
      }
      if (got < n) fprintf(d->log, "TIMEOUT\n");
    } else if (!strcmp(op, "wait")) {
      long long c = 0, spent = 0;
      sscanf(rest, "%lld", &c);
      while (spent < c) {
        t9_step(d, T9_POLL);
        spent += T9_POLL;
        t9_service(d);
      }
    } else if (!strcmp(op, "rd")) {
      sscanf(rest, "%d", &d->rdsize);
    } else if (!strcmp(op, "vol")) {
      unsigned a = 0x80, b = 0, c = 0x80, e = 0;
      sscanf(rest, "%x %x %x %x", &a, &b, &c, &e);
      d->p.wr(d->p.ctx, 0, 2);
      d->p.wr(d->p.ctx, 2, (uint8_t)a);
      d->p.wr(d->p.ctx, 3, (uint8_t)b);
      d->p.wr(d->p.ctx, 0, 3);
      d->p.wr(d->p.ctx, 1, (uint8_t)c);
      d->p.wr(d->p.ctx, 2, (uint8_t)e);
      d->p.wr(d->p.ctx, 3, 0x20);
    } else if (!strcmp(op, "reg")) {
      unsigned i = 0, a = 0, v = 0;
      sscanf(rest, "%x %x %x", &i, &a, &v);
      d->p.wr(d->p.ctx, 0, (uint8_t)i);
      d->p.wr(d->p.ctx, (int)a, (uint8_t)v);
    } else if (!strcmp(op, "lid")) {
      int o = 0;
      sscanf(rest, "%d", &o);
      d->p.lid(d->p.ctx, o);
    } else if (!strcmp(op, "disc")) {
      char nm[256];
      if (sscanf(rest, "%255s", nm) == 1) d->p.disc(d->p.ctx, nm);
    } else if (!strcmp(op, "audio")) {
      int o = 0;
      sscanf(rest, "%d", &o);
      d->p.audio(d->p.ctx, o, d->audioIdx);
      if (!o) d->audioIdx++;
    } else if (!strcmp(op, "mark")) {
      char* s = rest;
      while (*s == ' ' || *s == '\t') s++;
      s[strcspn(s, "\r\n")] = 0;
      fprintf(d->log, "MARK %s\n", s);
    }
  }
  fclose(f);
}

#endif
