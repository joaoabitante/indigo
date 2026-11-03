@ Homebrew de teste do Índigo para o 3DO (etapa 9b), escrito do zero (GPL-3.0). Vai no disco como "LaunchMe" e
@ roda em 0x00010000 depois da BIOS de teste: uma cel compactada de 4 bits (listras), um degradê de 16 bits que
@ gira (motor de cantos), uma bola de 8 bits com PLUT que quica e anda com o direcional, uma faixa translúcida
@ (PIXC) e uma onda quadrada tocada pelo DSP (mais aguda com o botão A). Escreve "HOME" em 0x000FFF00 ao começar
@ e marca 0x000FFF04 quando o botão A é apertado (os testes conferem).
        .equ MADAM, 0x03300000
        .equ CLIO,  0x03400000
        .equ SPORT, 0x03200000
        .equ VDL,   0x000F0000
        .equ CCBS,  0x000E0000
        .equ PAD,   0x000FFE00
        .equ PADSRC, 0x000FFE40
        .equ SINAL, 0x000FFF00
        .equ BRANCO, 0x000E8000
        .equ BASEF, 0x3F660000
        .equ LAST,  0x40000000
        .equ LDPLUT, 0x00800000
        .equ PACKED, 0x00000200

        .text
        .global inicio
inicio:
        ldr     sp, =0x001FE000
        ldr     r0, =SINAL
        ldr     r1, =0x484F4D45         @ "HOME"
        str     r1, [r0]
        mov     r1, #0
        str     r1, [r0, #4]
        ldr     r0, =VDL
        ldr     r1, =0xE0083020         @ fundo verde-escuro
        str     r1, [r0, #16]
        ldr     r0, =BRANCO
        ldr     r1, =0x7FFF0000
        str     r1, [r0]
        bl      dsp_init
        mov     r4, #100                @ x da bola
        mov     r5, #60                 @ y
        mov     r6, #1                  @ vx
        mov     r7, #1                  @ vy
        mov     r8, #0                  @ ângulo
laco:
        bl      espera_quadro
        @ controle: o DMA do PBUS (canal 23) lido no quadro anterior; o controle 1 vem na 2ª palavra.
        @ Pede a próxima leitura: destino, contagem (12 bytes - 4), origem e o bit 15 do MCTL.
        ldr     r0, =PAD
        ldrb    r9, [r0, #4]
        ldr     r1, =MADAM
        str     r0, [r1, #0x570]
        mov     r2, #8
        str     r2, [r1, #0x574]
        ldr     r2, =PADSRC
        str     r2, [r1, #0x578]
        mov     r2, #0xE000
        str     r2, [r1, #8]
        @ direcional (bits 4..1 = baixo, cima, direita, esquerda) ou quique sozinho
        tst     r9, #0x1E
        beq     quica
        tst     r9, #0x04
        addne   r4, r4, #2
        tst     r9, #0x02
        subne   r4, r4, #2
        tst     r9, #0x10
        addne   r5, r5, #2
        tst     r9, #0x08
        subne   r5, r5, #2
        b       limites
quica:
        add     r4, r4, r6
        add     r5, r5, r7
limites:
        cmp     r4, #0
        movle   r4, #0
        movle   r6, #1
        cmp     r4, #288
        movge   r4, #288
        mvnge   r6, #0
        cmp     r5, #0
        movle   r5, #0
        movle   r7, #1
        cmp     r5, #208
        movge   r5, #208
        mvnge   r7, #0
        @ botão A: marca e muda o tom do DSP
        ldr     r0, =CLIO + 0x3440      @ EI[0x10] = passo da fase
        tst     r9, #0x01
        ldreq   r1, =654
        ldrne   r1, =1308
        str     r1, [r0]
        ldrne   r0, =SINAL
        strne   r1, [r0, #4]
        @ tela
        mov     r0, #0
        bl      limpa_tela
        add     r8, r8, #1
        bl      monta_cels
        ldr     r1, =MADAM
        ldr     r0, =CCBS
        str     r0, [r1, #0x5A4]
        str     r0, [r1, #0x100]
        b       laco

@ monta os 4 CCBs: listras, degradê girando, bola, faixa translúcida
monta_cels:
        stmfd   sp!, {r4-r11, lr}
        ldr     r10, =CCBS
        @ 1: listras (compactada, 4 bits, PLUT de 16), 2x
        ldr     r0, =BASEF | LDPLUT | PACKED
        str     r0, [r10, #0]
        add     r0, r10, #64
        str     r0, [r10, #4]
        ldr     r0, =listras_px
        str     r0, [r10, #8]
        ldr     r0, =listras_plut
        str     r0, [r10, #12]
        ldr     r0, =16 << 16
        str     r0, [r10, #16]
        ldr     r0, =150 << 16
        str     r0, [r10, #20]
        mov     r0, #2 << 20
        str     r0, [r10, #24]
        mov     r0, #0
        str     r0, [r10, #28]
        str     r0, [r10, #32]
        mov     r0, #2 << 16
        str     r0, [r10, #36]
        mov     r0, #0
        str     r0, [r10, #40]
        str     r0, [r10, #44]
        ldr     r0, =0x1F001F00
        str     r0, [r10, #48]
        ldr     r0, =3 | (23 << 6)
        str     r0, [r10, #52]
        mov     r0, #0
        str     r0, [r10, #56]
        @ 2: degradê girando em volta do centro (200, 110)
        add     r10, r10, #64
        ldr     r0, =BASEF
        str     r0, [r10, #0]
        add     r0, r10, #64
        str     r0, [r10, #4]
        ldr     r0, =degrade_px
        str     r0, [r10, #8]
        mov     r0, #0
        str     r0, [r10, #12]
        ldr     r11, =seno
        and     r1, r8, #63
        ldr     r2, [r11, r1, lsl #2]   @ sen * 3 (12.20) = HDY
        add     r1, r1, #16
        and     r1, r1, #63
        ldr     r3, [r11, r1, lsl #2]   @ cos * 3 = HDX
        str     r3, [r10, #24]
        str     r2, [r10, #28]
        mov     r12, r2, asr #4
        rsb     r12, r12, #0            @ VDX = -sen (16.16)
        str     r12, [r10, #32]
        mov     r9, r3, asr #4          @ VDY = cos (16.16)
        str     r9, [r10, #36]
        @ X = 200 - 8*(HDX/16 + VDX); Y = 110 - 8*(HDY/16 + VDY)
        add     r0, r12, r3, asr #4
        ldr     r1, =200 << 16
        sub     r1, r1, r0, lsl #3
        str     r1, [r10, #16]
        add     r0, r9, r2, asr #4
        ldr     r1, =110 << 16
        sub     r1, r1, r0, lsl #3
        str     r1, [r10, #20]
        mov     r0, #0
        str     r0, [r10, #40]
        str     r0, [r10, #44]
        ldr     r0, =0x1F001F00
        str     r0, [r10, #48]
        ldr     r0, =0x3D6
        str     r0, [r10, #52]
        ldr     r0, =0x0006000F
        str     r0, [r10, #56]
        @ 3: bola (8 bits com PLUT), 2x
        add     r10, r10, #64
        ldr     r0, =BASEF | LDPLUT
        str     r0, [r10, #0]
        add     r0, r10, #64
        str     r0, [r10, #4]
        ldr     r0, =bola_px
        str     r0, [r10, #8]
        ldr     r0, =bola_plut
        str     r0, [r10, #12]
        mov     r0, r4, lsl #16
        str     r0, [r10, #16]
        mov     r0, r5, lsl #16
        str     r0, [r10, #20]
        mov     r0, #2 << 20
        str     r0, [r10, #24]
        mov     r0, #0
        str     r0, [r10, #28]
        str     r0, [r10, #32]
        mov     r0, #2 << 16
        str     r0, [r10, #36]
        mov     r0, #0
        str     r0, [r10, #40]
        str     r0, [r10, #44]
        ldr     r0, =0x1F001F00
        str     r0, [r10, #48]
        ldr     r0, =5 | (15 << 6)
        str     r0, [r10, #52]
        ldr     r0, =15 | (2 << 16)
        str     r0, [r10, #56]
        @ 4: faixa translúcida: metade branco + metade do quadro
        add     r10, r10, #64
        ldr     r0, =BASEF | LAST
        str     r0, [r10, #0]
        mov     r0, #0
        str     r0, [r10, #4]
        ldr     r0, =BRANCO
        str     r0, [r10, #8]
        mov     r0, #0
        str     r0, [r10, #12]
        ldr     r0, =110 << 16
        str     r0, [r10, #16]
        ldr     r0, =196 << 16
        str     r0, [r10, #20]
        ldr     r0, =200 << 20
        str     r0, [r10, #24]
        mov     r0, #0
        str     r0, [r10, #28]
        str     r0, [r10, #32]
        ldr     r0, =24 << 16
        str     r0, [r10, #36]
        mov     r0, #0
        str     r0, [r10, #40]
        str     r0, [r10, #44]
        ldr     r0, =0x01810181
        str     r0, [r10, #48]
        mov     r0, #0x16
        str     r0, [r10, #52]
        mov     r0, #0
        str     r0, [r10, #56]
        ldmfd   sp!, {r4-r11, pc}

@ SPORT: cor (0x2000) e preenchimento das 75 páginas da tela (0x4000 + 4 * página, máscara inteira)
limpa_tela:
        ldr     r1, =SPORT
        add     r1, r1, #0x2000         @ deslocamento imediato do ARM só vai até 4095
        str     r0, [r1]
        add     r1, r1, #0x2000
        mvn     r3, #0
        mov     r2, #75
1:      str     r3, [r1], #4
        subs    r2, r2, #1
        bne     1b
        mov     pc, lr

espera_quadro:
        ldr     r1, =CLIO
        ldr     r2, =0x7FF
1:      ldr     r0, [r1, #0x34]
        and     r0, r0, r2
        cmp     r0, #240
        bne     1b
2:      ldr     r0, [r1, #0x34]
        and     r0, r0, r2
        cmp     r0, #240
        beq     2b
        mov     pc, lr

@ DSP (conjunto de instruções do DSPP): fase += passo (EI 0x10); saída = +0x1800 ou -0x1800 conforme o sinal
@ da fase. Programa carregado uma instrução por palavra (CLIO 0x2000), DSP reiniciado e ligado (0x17FC).
dsp_init:
        ldr     r0, =CLIO + 0x2000
        ldr     r1, =microcodigo
        mov     r2, #16
1:      ldr     r3, [r1], #4
        str     r3, [r0], #4
        subs    r2, r2, #1
        bne     1b
        ldr     r0, =CLIO + 0x3440
        ldr     r1, =654
        str     r1, [r0]
        ldr     r0, =CLIO + 0x17FC
        mov     r1, #5                  @ reinicia e roda
        str     r1, [r0]
        mov     pc, lr

        .ltorg
        .balign 4
microcodigo:
        .word   0x6520, 0x8100, 0x8010, 0x8100   @ [0x100] = [0x100] + [0x010]  (A+B, 3 endereços)
        .word   0xA80B                           @ negativo (N): vai para 11
        .word   0x9BFE, 0xE300, 0x9BFF, 0xE300   @ MOVED: DAC = +0x1800 (imediato deslocado)
        .word   0x8380, 0x0000                   @ SLEEP
        .word   0x9BFE, 0xFD00, 0x9BFF, 0xFD00   @ DAC = -0x1800
        .word   0x8380                           @ SLEEP

        .include "t9b_3do_dados.inc"
