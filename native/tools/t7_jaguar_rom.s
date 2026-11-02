| Cartucho de teste do Atari Jaguar (escrito para o Índigo; licença do projeto, GPL-3). Montado pelo
| t7_jaguar.sh com o binutils do m68k-linux-gnu, ligado em 0x800000. O 68000 prepara tudo e o t7_jaguar.cpp confere
| a tela, a RAM, o som, o save e os estados:
|  - Object Processor: bitmaps de 8, 16 e 1 bit (com transparência e CLUT), bitmap com escala 2x, objeto de desvio
|    e de parada; a lista é recopiada na interrupção de vídeo (o OP altera os objetos ao desenhar);
|  - GPU: um programa com as instruções do RISC grava os resultados em 0x20000 e interrompe o 68000 no fim;
|  - Blitter: preenchimento (modo phrase), cópia, transparência (DCOMPEN), expansão de 1 bit (BCOMPEN), Gouraud e
|    recorte, num quadro de 64x32 pontos de 16 bits em 0x40000;
|  - DSP: onda quadrada no DAC (interrupção I2S) e contagem do temporizador 1 do Jerry;
|  - interrupções do 68000: vídeo, GPU, PIT do Tom e temporizador 2 do Jerry (contadores em 0x3000..0x300C);
|  - controle (0x3010/0x3012) e EEPROM do cartucho (grava 0xBEEF na palavra 5 e lê as palavras 5 e 6).
| As instruções do GPU/DSP são montadas com as macros abaixo (o binutils não conhece o RISC do Jaguar).

        .set    INT1,   0xF000E0
        .set    OLP,    0xF00020
        .set    PIT0,   0xF00050
        .set    G_FLAGS, 0xF02100
        .set    G_PC,   0xF02110
        .set    G_CTRL, 0xF02114
        .set    G_HIDATA, 0xF02118
        .set    G_REMAIN, 0xF0211C
        .set    G_DIVCTRL, 0xF0211C
        .set    D_FLAGS, 0xF1A100
        .set    D_PC,   0xF1A110
        .set    D_CTRL, 0xF1A114
        .set    LTXD,   0xF1A148
        .set    RTXD,   0xF1A14C
        .set    SCLK,   0xF1A150
        .set    SMODE,  0xF1A154
        .set    JPIT1,  0xF10000
        .set    J_INT,  0xF10020
        .set    JOYSTICK, 0xF14000
        .set    JOYBUTS, 0xF14002
        .set    BLIT,   0xF02200
        .set    A1_BASE, 0xF02200
        .set    A1_FLAGS, 0xF02204
        .set    A1_CLIP, 0xF02208
        .set    A1_PIXEL, 0xF0220C
        .set    A1_STEP, 0xF02210
        .set    A2_BASE, 0xF02224
        .set    A2_FLAGS, 0xF02228
        .set    A2_PIXEL, 0xF02230
        .set    A2_STEP, 0xF02234
        .set    B_CMD,  0xF02238
        .set    B_COUNT, 0xF0223C
        .set    B_PATD, 0xF02268
        .set    B_IINC, 0xF02270
        .set    B_I0,   0xF02288

        .set    INT_EN, 0x1B            | vídeo + GPU + PIT + Jerry

        .include "t7_jaguar_risc.inc"

| ------------------------------------------------------------------ cabeçalho do cartucho
        .text
        .org    0x400
        .long   0x04040404              | marca do cartucho
        .long   0x00802000              | partida
        .org    0x2000

start:
        move.w  #0x2700, %sr
        | zera as áreas de trabalho
        lea     0x3000, %a0
        move.w  #0x3F, %d0
1:      clr.l   (%a0)+
        dbra    %d0, 1b
        lea     0x40000, %a0
        move.w  #0x3FF, %d0
1:      clr.l   (%a0)+
        dbra    %d0, 1b
        | CLUT: 1 = azul, 2 = vermelho, 5 = quase branco (CRY)
        move.w  #0x00FF, 0xF00402
        move.w  #0xF0FF, 0xF00404
        move.w  #0x88FF, 0xF0040A
        bsr     make_bitmaps
        move.l  #irq, 0x100
        bsr     copy_list
        move.l  #0x10000, %d0
        swap    %d0
        move.l  %d0, OLP

        | GPU: copia o programa para a RAM local e liga
        lea     gpu_code, %a0
        lea     0xF03000, %a1
        move.w  #(gpu_end - gpu_code) / 2 - 1, %d0
1:      move.w  (%a0)+, (%a1)+
        dbra    %d0, 1b
        move.l  #0xF03000, G_PC
        move.l  #1, G_CTRL

        | interrupções: vídeo, GPU, PIT do Tom (~1000 Hz), temporizador 2 do Jerry (~100 Hz)
        move.w  #INT_EN, INT1
        move.w  #25, PIT0
        move.w  #1022, PIT0 + 2
        move.w  #0x08, J_INT
        move.w  #99, JPIT1 + 4
        move.w  #2659, JPIT1 + 6
        move.w  #0x2100, %sr

        | espera o GPU terminar
        move.l  #2000000, %d1
2:      cmp.l   #0x600DF00D, 0x3030
        beq.s   3f
        subq.l  #1, %d1
        bne.s   2b
3:
        bsr     blit_tests
        bsr     dsp_start
        bsr     eeprom_test

main_loop:
        move.w  #0x81FE, JOYSTICK
        move.w  JOYSTICK, 0x3010
        move.w  JOYBUTS, 0x3012
        bra.s   main_loop

| ------------------------------------------------------------------ interrupção do Tom (vetor 64)
irq:
        movem.l %d0-%d1/%a0-%a1, -(%sp)
        move.w  INT1, %d0
        btst    #0, %d0
        beq.s   1f
        addq.l  #1, 0x3000
        bsr     copy_list
        move.w  #INT_EN + 0x100, INT1
1:      btst    #1, %d0
        beq.s   1f
        addq.l  #1, 0x3004
        move.w  #INT_EN + 0x200, INT1
1:      btst    #3, %d0
        beq.s   1f
        addq.l  #1, 0x3008
        move.w  #INT_EN + 0x800, INT1
1:      btst    #4, %d0
        beq.s   1f
        move.w  J_INT, %d1
        btst    #3, %d1
        beq.s   2f
        addq.l  #1, 0x300C
2:      lsl.w   #8, %d1
        or.w    #0x08, %d1
        move.w  %d1, J_INT
        move.w  #INT_EN + 0x1000, INT1
1:      movem.l (%sp)+, %d0-%d1/%a0-%a1
        rte

copy_list:
        lea     list_tpl, %a0
        lea     0x10000, %a1
        move.w  #(list_end - list_tpl) / 4 - 1, %d1
1:      move.l  (%a0)+, (%a1)+
        dbra    %d1, 1b
        rts

| ------------------------------------------------------------------ dados dos bitmaps
make_bitmaps:
        | 8 bits, 64 x 20: metade 1 (azul), metade 2 (vermelho)
        lea     0x11000, %a0
        move.w  #19, %d1
1:      moveq   #31, %d0
2:      move.b  #1, (%a0)+
        dbra    %d0, 2b
        moveq   #31, %d0
2:      move.b  #2, (%a0)+
        dbra    %d0, 2b
        dbra    %d1, 1b
        | 16 bits, 8 x 10: CRY 0x0088
        lea     0x12000, %a0
        moveq   #79, %d0
1:      move.w  #0x0088, (%a0)+
        dbra    %d0, 1b
        | 1 bit, 64 x 4: 11110000...
        lea     0x12200, %a0
        moveq   #31, %d0
1:      move.b  #0xF0, (%a0)+
        dbra    %d0, 1b
        | 8 bits, 8 x 4 (bitmap com escala): 1,2,1,2...
        lea     0x12300, %a0
        moveq   #15, %d0
1:      move.b  #1, (%a0)+
        move.b  #2, (%a0)+
        dbra    %d0, 1b
        | 16 bits, 8 x 20: CRY 0xF0FF (vermelho)
        lea     0x12400, %a0
        move.w  #159, %d0
1:      move.w  #0xF0FF, (%a0)+
        dbra    %d0, 1b
        rts

| ------------------------------------------------------------------ lista de objetos (copiada para 0x10000)
| bitmap: p0 = tipo | ypos<<3 | altura<<14 | (link>>3)<<24 | (dados>>3)<<43
|         p1 = x | prof<<12 | pitch<<15 | dwidth<<18 | iwidth<<28 | index<<38 | trans<<47
        .macro  obj_bitmap ypos, h, link, data, x, depth, dw, iw, index, trans
        .long   (((\link) >> 3) >> 8) + (((\data) >> 3) << 11)
        .long   0 + ((\ypos) << 3) + ((\h) << 14) + ((((\link) >> 3) & 0xFF) << 24)
        .long   ((\iw) >> 4) + ((\index) << 6) + ((\trans) << 15)
        .long   ((\x) & 0xFFF) + ((\depth) << 12) + (1 << 15) + ((\dw) << 18) + (((\iw) & 15) << 28)
        .endm
        .macro  obj_scaled ypos, h, link, data, x, depth, dw, iw, hs, vs, rem
        .long   (((\link) >> 3) >> 8) + (((\data) >> 3) << 11)
        .long   1 + ((\ypos) << 3) + ((\h) << 14) + ((((\link) >> 3) & 0xFF) << 24)
        .long   ((\iw) >> 4)
        .long   ((\x) & 0xFFF) + ((\depth) << 12) + (1 << 15) + ((\dw) << 18) + (((\iw) & 15) << 28)
        .long   0
        .long   (\hs) + ((\vs) << 8) + ((\rem) << 16)
        .endm

        .balign 8
list_tpl:
        obj_bitmap  58, 20, 0x10010, 0x11000, 10, 3, 8, 8, 0, 0     | 0x10000: 8 bits, linhas 10..29
        obj_bitmap  68, 4, 0x10020, 0x12200, 10, 0, 1, 1, 2, 1      | 0x10010: 1 bit transparente, linhas 15..18
        obj_bitmap  138, 10, 0x10040, 0x12000, 100, 4, 2, 2, 0, 0   | 0x10020: 16 bits, linhas 50..59
        .long   0, 0, 0, 0
        obj_scaled  198, 4, 0x10068, 0x12300, 200, 3, 1, 1, 0x40, 0x40, 0x20  | 0x10040: escala 2x, linhas 80..87
        .long   0, 0                                                | 0x10058
        .long   0, 0                                                | 0x10060
        .long   (0x10080 >> 3) >> 8                                 | 0x10068: desvio se VC > 278 (linha 120)
        .long   3 + (278 << 3) + (2 << 14) + (((0x10080 >> 3) & 0xFF) << 24)
        obj_bitmap  258, 20, 0x10080, 0x12400, 150, 4, 2, 2, 0, 0   | 0x10070: 16 bits, linhas 110..120
        .long   0, 4                                                | 0x10080: parada
list_end:

| ------------------------------------------------------------------ Blitter
        .macro  bset32 addr, val
        move.l  #\val, \addr
        .endm
blit_tests:
        | origem: 64 x 9 pontos de 16 bits em 0x48000, valor = y*256 + x + 0x1000; linha 8: 0, 0x2222 alternados
        lea     0x48000, %a0
        moveq   #0, %d1
1:      moveq   #0, %d0
2:      move.w  %d1, %d2
        lsl.w   #8, %d2
        add.w   %d0, %d2
        add.w   #0x1000, %d2
        move.w  %d2, (%a0)+
        addq.w  #1, %d0
        cmp.w   #64, %d0
        bne.s   2b
        addq.w  #1, %d1
        cmp.w   #8, %d1
        bne.s   1b
        moveq   #31, %d0
1:      move.w  #0, (%a0)+
        move.w  #0x2222, (%a0)+
        dbra    %d0, 1b
        move.b  #0xA5, 0x4A000                      | fonte de 1 bit
        | fundo do teste de transparência
        lea     0x40000 + (12 * 64 + 30) * 2, %a0
        moveq   #7, %d0
1:      move.w  #0x5555, (%a0)+
        dbra    %d0, 1b

        | 1. preenchimento em modo phrase: (4..11, 2..6) = 0xF0FF
        bset32  A1_BASE, 0x40000
        bset32  A1_FLAGS, 0x3020
        bset32  A1_PIXEL, (2 << 16) + 4
        bset32  A1_STEP, 0x0001FFF8
        bset32  B_PATD, 0xF0FFF0FF
        bset32  B_PATD + 4, 0xF0FFF0FF
        bset32  B_COUNT, (5 << 16) + 8
        bset32  B_CMD, 0x00010200
        | 2. cópia (A2 -> A1), modo pixel: origem (2,1) 8x4 -> destino (20,10)
        bset32  A2_BASE, 0x48000
        bset32  A2_FLAGS, 0x13020
        bset32  A2_PIXEL, (1 << 16) + 2
        bset32  A2_STEP, 0x0001FFF8
        bset32  A1_FLAGS, 0x13020
        bset32  A1_PIXEL, (10 << 16) + 20
        bset32  B_COUNT, (4 << 16) + 8
        bset32  B_CMD, 0x01800601
        | 3. cópia com transparência (DCOMPEN, padrão 0): origem linha 8 -> destino (30,12)
        bset32  A2_PIXEL, (8 << 16) + 0
        bset32  A1_PIXEL, (12 << 16) + 30
        bset32  B_PATD, 0
        bset32  B_PATD + 4, 0
        bset32  B_COUNT, (1 << 16) + 8
        bset32  B_CMD, 0x09800001
        | 4. expansão de 1 bit (BCOMPEN): 0xA5 -> cor 0x1234 em (0..7, 20)
        bset32  A2_BASE, 0x4A000
        bset32  A2_FLAGS, 0x13000
        bset32  A2_PIXEL, 0
        bset32  A1_PIXEL, (20 << 16) + 0
        bset32  B_PATD, 0x12341234
        bset32  B_PATD + 4, 0x12341234
        bset32  B_CMD, 0x04010001
        | 5. Gouraud: cor 0x77, intensidade 0x10 + 1 por ponto em (0..7, 25)
        bset32  A1_PIXEL, (25 << 16) + 0
        bset32  B_PATD, 0
        bset32  B_PATD + 4, 0x00007700
        bset32  B_I0, 0x00100000
        bset32  B_IINC, 0x00010000
        bset32  B_CMD, 0x00011000
        | 6. recorte: janela de 62 pontos; (56..63, 30) -> só 56..61
        bset32  A1_CLIP, (32 << 16) + 62
        bset32  A1_PIXEL, (30 << 16) + 56
        bset32  B_PATD, 0x77777777
        bset32  B_PATD + 4, 0x77777777
        bset32  B_CMD, 0x00010040
        move.l  #0xB1170000, 0x3034
        rts

| ------------------------------------------------------------------ DSP
dsp_start:
        move.w  #19, SCLK + 2
        move.w  #0x15, SMODE + 2
        move.w  #99, JPIT1
        move.w  #2659, JPIT1 + 2
        lea     dsp_code, %a0
        lea     0xF1B000, %a1
        move.w  #(dsp_end - dsp_code) / 2 - 1, %d0
1:      move.w  (%a0)+, (%a1)+
        dbra    %d0, 1b
        move.l  #0xF1B030, D_PC
        move.l  #1, D_CTRL
        rts

| ------------------------------------------------------------------ EEPROM 93C46
ee_send:                                | d0 = bits, d1 = quantos
        subq.w  #1, %d1
1:      moveq   #0, %d2
        btst    %d1, %d0
        beq.s   2f
        moveq   #1, %d2
2:      move.w  %d2, 0xF14800
        dbra    %d1, 1b
        rts
ee_read:                                | d3 = endereço -> d4
        tst.w   0xF15000
        move.w  #0x180, %d0
        or.w    %d3, %d0
        moveq   #9, %d1
        bsr.s   ee_send
        moveq   #15, %d1
        moveq   #0, %d4
1:      tst.w   0xF14800
        move.w  0xF14000, %d2
        lsr.w   #1, %d2
        roxl.w  #1, %d4
        dbra    %d1, 1b
        rts
eeprom_test:
        tst.w   0xF15000
        move.w  #0x130, %d0             | EWEN
        moveq   #9, %d1
        bsr.s   ee_send
        tst.w   0xF15000
        move.w  #0x145, %d0             | WRITE 5
        moveq   #9, %d1
        bsr.s   ee_send
        move.w  #0xBEEF, %d0
        moveq   #16, %d1
        bsr.s   ee_send
        tst.w   0xF15000
        moveq   #5, %d3
        bsr.s   ee_read
        move.w  %d4, 0x3020
        moveq   #6, %d3
        bsr.s   ee_read
        move.w  %d4, 0x3022
        rts

| ------------------------------------------------------------------ programa do GPU (copiado para 0xF03000)
        .balign 4
gpu_code:
        r_movei 0x20000, 15
        r_movei 0x12345678, 1
        r_movei 0x0F0F0F0F, 2
        r_move 1, 3
        r_add 2, 3
        res 3                                   | 0
        r_move 1, 3
        r_sub 2, 3
        res 3                                   | 1
        r_move 1, 3
        r_and 2, 3
        res 3                                   | 2
        r_move 1, 3
        r_or 2, 3
        res 3                                   | 3
        r_move 1, 3
        r_xor 2, 3
        res 3                                   | 4
        r_move 1, 3
        r_not 3
        res 3                                   | 5
        r_move 2, 3
        r_neg 3
        res 3                                   | 6
        r_move 1, 3
        r_mult 2, 3
        res 3                                   | 7
        r_movei 0xFFFF8000, 4
        r_move 1, 3
        r_imult 4, 3
        res 3                                   | 8
        r_movei 1000000, 3
        r_moveq 7, 4
        r_div 4, 3
        res 3                                   | 9
        r_movei G_REMAIN, 5
        r_load 5, 6
        res 6                                   | 10
        r_move 1, 3
        r_shlq 4, 3
        res 3                                   | 11
        r_move 1, 3
        r_shrq 8, 3
        res 3                                   | 12
        r_movei 0x80000010, 3
        r_sharq 4, 3
        res 3                                   | 13
        r_move 1, 3
        r_movei -4, 4
        r_sh 4, 3
        res 3                                   | 14
        r_move 1, 3
        r_moveq 8, 4
        r_ror 4, 3
        res 3                                   | 15
        r_move 1, 3
        r_rorq 4, 3
        res 3                                   | 16
        r_movei 300, 3
        r_sat8 3
        res 3                                   | 17
        r_movei -77, 3
        r_abs 3
        res 3                                   | 18
        r_movei 0x00800000, 4
        r_normi 4, 3
        res 3                                   | 19
        r_moveq 1, 4
        r_normi 4, 3
        res 3                                   | 20
        r_moveq 3, 3
        r_moveq 4, 4
        r_imultn 3, 4
        r_moveq 5, 5
        r_moveq 6, 6
        r_imacn 5, 6
        r_resmac 7
        res 7                                   | 21
        r_moveta 1, 2
        r_movefa 2, 9
        res 9                                   | 22
        r_moveq 10, 3
        r_moveq 0, 4
g_loop: r_add 3, 4
        r_subq 1, 3
        r_jr CC_NE, g_loop
        r_nop
        res 4                                   | 23
        r_movei g_target - gpu_code + 0xF03000, 5
        r_jump CC_T, 5
        r_moveq 7, 6
        r_moveq 9, 6
g_target:
        res 6                                   | 24
        r_moveq 5, 3
        r_moveq 9, 4
        r_moveq 0, 6
        r_cmp 4, 3
        r_jr CC_PL, g_skip1
        r_nop
        r_moveq 1, 6
g_skip1:
        res 6                                   | 25
        r_moveq 0, 6
        r_cmp 4, 3
        r_jr CC_CC, g_skip2
        r_nop
        r_moveq 2, 6
g_skip2:
        res 6                                   | 26
        r_movei 0x20100, 10
        r_movei 0xA1B2C3D4, 3
        r_store 10, 3
        r_loadb 10, 4
        res 4                                   | 27
        r_movei 0x20102, 11
        r_loadw 11, 4
        res 4                                   | 28
        r_moveq 0x15, 3
        r_storeb 10, 3
        r_load 10, 4
        res 4                                   | 29
        r_movei 0x55AA55AA, 3
        r_movei 0x20104, 11
        r_store 11, 3
        r_move 10, 14
        r_load14n 1, 4
        res 4                                   | 30
        r_moveq 4, 5
        r_load14r 5, 4
        res 4                                   | 31
        r_movei 0xFFFFFFFF, 3
        r_moveq 1, 4
        r_add 4, 3
        r_moveq 0, 5
        r_moveq 0, 6
        r_addc 6, 5
        res 5                                   | 32
        r_movei 0x20200, 10
        r_movei 0x11112222, 3
        r_movei G_HIDATA, 5
        r_movei 0xCAFEBABE, 6
        r_store 5, 6
        r_storep 10, 3
        r_moveq 0, 6
        r_store 5, 6
        r_loadp 10, 4
        res 4                                   | 33
        r_load 5, 7
        res 7                                   | 34
g_pc:   r_movepc 4
        res 4                                   | 35
        r_moveq 0, 3
        r_bset 31, 3
        r_bset 0, 3
        res 3                                   | 36
        r_bclr 31, 3
        res 3                                   | 37
        r_moveq 0, 3
        r_moveq 1, 4
        r_sub 4, 3
        r_moveq 5, 5
        r_moveq 2, 6
        r_subc 6, 5
        res 5                                   | 38
        r_movei G_DIVCTRL, 5
        r_moveq 1, 6
        r_store 5, 6
        r_moveq 1, 3
        r_moveq 4, 4
        r_div 4, 3
        res 3                                   | 39
        r_moveq 0, 6
        r_store 5, 6
        | fim: marca, interrompe o 68000 e para
        r_movei 0x3030, 3
        r_movei 0x600DF00D, 4
        r_store 3, 4
        r_movei G_CTRL, 3
        r_moveq 3, 4
        r_store 3, 4
        r_moveq 0, 4
        r_store 3, 4
        r_nop
g_halt: r_jr CC_T, g_halt
        r_nop
gpu_end:

| ------------------------------------------------------------------ programa do DSP (copiado para 0xF1B000)
        .balign 4
dsp_code:
        .word   0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400   | interrupção 0 (sem uso)
        r_movei d_i2s - dsp_code + 0xF1B000, 28                  | interrupção 1: I2S
        r_jump CC_T, 28
        r_nop
        .word   0xE400, 0xE400, 0xE400
        r_movei d_tim1 - dsp_code + 0xF1B000, 28                 | interrupção 2: temporizador 1
        r_jump CC_T, 28
        r_nop
        .word   0xE400, 0xE400, 0xE400
d_main:                                         | 0xF1B030
        r_movei 0xF1CFF0, 31
        r_movei LTXD, 13
        r_movei RTXD, 15
        r_movei 8000, 12
        r_moveq 0, 10
        r_movei 24, 11
        r_moveq 0, 16
        r_movei 0xF1C000, 17
        r_movei D_FLAGS, 1
        r_movei 0x60, 2
        r_store 1, 2
d_loop: r_jr CC_T, d_loop
        r_nop
d_i2s:  r_addq 1, 10
        r_cmp 11, 10
        r_jr CC_NE, 1f
        r_nop
        r_moveq 0, 10
        r_neg 12
1:      r_store 13, 12
        r_move 12, 14
        r_neg 14
        r_store 15, 14
        r_movei D_FLAGS, 30
        r_load 30, 29
        r_bclr 3, 29
        r_bset 10, 29
        r_load 31, 28
        r_addq 2, 28
        r_addq 4, 31
        r_jump CC_T, 28
        r_store 30, 29
d_tim1: r_addq 1, 16
        r_store 17, 16
        r_movei D_FLAGS, 30
        r_load 30, 29
        r_bclr 3, 29
        r_bset 11, 29
        r_load 31, 28
        r_addq 2, 28
        r_addq 4, 31
        r_jump CC_T, 28
        r_store 30, 29
        .balign 4
dsp_end:
        .org    0x20000 - 4
        .long   0
