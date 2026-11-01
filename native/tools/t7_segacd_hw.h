/* BIOS de teste do Sega CD (Índigo): definições comuns aos programas do 68000 principal e do sub-CPU.
 * Compilado com o gcc do m68k-linux-gnu (-m68000, sem biblioteca): nada de multiplicação/divisão de 32 bits
 * fora das rotinas de t7_segacd_crt.s. */
#ifndef T7_SEGACD_HW_H
#define T7_SEGACD_HW_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef signed short s16;
typedef signed long s32;

#define R8(a) (*(volatile u8*)(a))
#define R16(a) (*(volatile u16*)(a))
#define R32(a) (*(volatile u32*)(a))

/* protocolo entre as CPUs:
 *   A12010 (principal -> sub): número do teste; A1200E (flag do principal) = sequência
 *   o sub roda o teste, grava 16 palavras de resultado em PRG-RAM RESULTS + 32*teste e põe a sequência na flag dele
 *   o principal para o sub (BUSREQ), lê os resultados pela janela da PRG-RAM (banco 0) e solta o sub */
#define RESULTS 0x1E000UL
#define NTESTS 26

/* testes */
#define T_CDD 1
#define T_TOC 2
#define T_HOST 3
#define T_PRGDMA 4
#define T_WRAMDMA 5
#define T_PCMDMA 6
#define T_W1MDMA 7
#define T_MAINHOST 8   /* preparação do sub; a leitura é do principal */
#define T_W2M 9        /* Word RAM 2M: troca de dono */
#define T_W1M 10       /* Word RAM 1M: imagem de pontos, prioridades, troca de bancos */
#define T_ASIC1 11
#define T_ASIC2 12
#define T_ASIC3 13
#define T_TIMER 14
#define T_PCM 15
#define T_CDDA 16
#define T_INT2 17
#define T_MISC 18
#define T_PCMSTOP 19
#define T_CDDASTOP 20
#define T_TOMAIN 21    /* devolve a Word RAM 2M ao principal */
#define T_MODE2M 22    /* volta para 2M com a Word RAM do sub */
#define T_WP 23
#define T_W1MB 24     /* Word RAM 1M: troca de bancos pelo RET */

/* padrão dos setores de dados do disco: palavra k do setor L = ((L << 10) | k) ^ 0x5A5A */
#define PATTERN(L, k) ((u16)((((u16)(L)) << 10 | (k)) ^ 0x5A5A))

#endif
