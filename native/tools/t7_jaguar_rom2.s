| Segundo cartucho de teste do Atari Jaguar (escrito para o Índigo; GPL-3), montado pelo t7_jaguar.sh:
|  - vídeo em RGB16 com fundo (BGEN) escrito byte a byte pelo 68000;
|  - Object Processor: bitmap de 16 bits normal e refletido, bitmap de 4 bits com o índice da CLUT, objeto do GPU
|    (o GPU conta as linhas e libera o OP escrevendo no OBF) e desvio sempre tomado;
|  - Blitter pelo GPU (escritas de 32 bits): cópia de 8 bits em modo phrase, leitura com passo fracionário
|    (XADDINC, A1 -> A2), Z-buffer (DSTENZ/DSTWRZ, phrases intercaladas) e LFU com o destino (XOR);
|  - interrupção 0 do GPU forçada pelo 68000 (FORCEINT0);
|  - DSP: MAC de 40 bits (RESMAC, D_MACHI, SAT32S), MIRROR, SAT16S, ADDQMOD/SUBQMOD e interrupção do DSP para o
|    68000 (pelo Jerry e pelo Tom);
|  - VC lido pelo 68000 dentro da interrupção de vídeo.

        .set    INT1,   0xF000E0
        .set    VC,     0xF00006
        .set    OLP,    0xF00020
        .set    VMODE,  0xF00028
        .set    G_FLAGS, 0xF02100
        .set    G_PC,   0xF02110
        .set    G_CTRL, 0xF02114
        .set    D_FLAGS, 0xF1A100
        .set    D_PC,   0xF1A110
        .set    D_CTRL, 0xF1A114
        .set    D_MOD,  0xF1A118
        .set    D_MACHI, 0xF1A120
        .set    J_INT,  0xF10020
        .set    INT_EN, 0x11            | vídeo + Jerry

        .include "t7_jaguar_risc.inc"

        .text
        .org    0x400
        .long   0x04040404
        .long   0x00802000
        .org    0x2000

start:
        move.w  #0x2700, %sr
        lea     0x3000, %a0
        move.w  #0x3F, %d0
1:      clr.l   (%a0)+
        dbra    %d0, 1b
        lea     0x40000, %a0
        move.w  #0x13FF, %d0
1:      clr.l   (%a0)+
        dbra    %d0, 1b
        move.w  #0x6C7, VMODE           | RGB16, fundo ligado
        move.b  #0x1F, 0xF00059         | BG = 0x001F (só o byte de baixo)
        move.w  #0x003F, 0xF00400 + 0x30 * 2
        move.w  #0xF800, 0xF00400 + 0x31 * 2
        | bitmap de 16 bits, 8 x 4: vermelho, verde, azul, branco...
        lea     0x11000, %a0
        moveq   #7, %d0
1:      move.w  #0xF800, (%a0)+
        move.w  #0x003F, (%a0)+
        move.w  #0x07C0, (%a0)+
        move.w  #0xFFFF, (%a0)+
        dbra    %d0, 1b
        | bitmap de 4 bits, 16 x 4: nibbles 0,1,0,1...
        lea     0x11100, %a0
        moveq   #31, %d0
1:      move.b  #0x01, (%a0)+
        dbra    %d0, 1b
        | origem de 8 bits (64 x 8): valor = y*64 + x
        lea     0x48000, %a0
        moveq   #0, %d0
1:      move.b  %d0, (%a0)+
        addq.w  #1, %d0
        cmp.w   #512, %d0
        bne.s   1b
        | Z-buffer: pontos 0, Z = 0,10,20..70 (phrases intercaladas: pontos 0-3, Z 0-3, pontos 4-7, Z 4-7)
        lea     0x42000, %a0
        move.w  #0, 8(%a0)
        move.w  #10, 10(%a0)
        move.w  #20, 12(%a0)
        move.w  #30, 14(%a0)
        move.w  #40, 24(%a0)
        move.w  #50, 26(%a0)
        move.w  #60, 28(%a0)
        move.w  #70, 30(%a0)
        | XOR: destino 0x0F0F, origem 0x00FF
        lea     0x43000, %a0
        lea     0x44000, %a1
        moveq   #7, %d0
1:      move.w  #0x0F0F, (%a0)+
        move.w  #0x00FF, (%a1)+
        dbra    %d0, 1b

        move.l  #irq, 0x100
        bsr     copy_list
        move.l  #0x10000, %d0
        swap    %d0
        move.l  %d0, OLP

        lea     gpu_code, %a0
        lea     0xF03000, %a1
        move.w  #(gpu_end - gpu_code) / 2 - 1, %d0
1:      move.w  (%a0)+, (%a1)+
        dbra    %d0, 1b
        move.l  #0xF03050, G_PC
        move.l  #1, G_CTRL
        move.l  #2000000, %d1
2:      cmp.l   #0x600DF00D, 0x3030
        beq.s   3f
        subq.l  #1, %d1
        bne.s   2b
3:
        move.w  #INT_EN, INT1
        move.w  #0x02, J_INT            | interrupção do DSP
        lea     dsp_code, %a0
        lea     0xF1B000, %a1
        move.w  #(dsp_end - dsp_code) / 2 - 1, %d0
1:      move.w  (%a0)+, (%a1)+
        dbra    %d0, 1b
        move.l  #0xF1B000, D_PC
        move.l  #1, D_CTRL
        move.w  #0x2100, %sr
        | espera 5 quadros e força a interrupção 0 do GPU
4:      cmp.l   #5, 0x3000
        bcs.s   4b
        move.l  #5, G_CTRL
5:      bra.s   5b

irq:
        movem.l %d0-%d1/%a0-%a1, -(%sp)
        move.w  INT1, %d0
        btst    #0, %d0
        beq.s   1f
        move.w  VC, 0x3010
        addq.l  #1, 0x3000
        bsr     copy_list
        move.w  #INT_EN + 0x100, INT1
1:      btst    #4, %d0
        beq.s   1f
        move.w  J_INT, %d1
        btst    #1, %d1
        beq.s   2f
        addq.l  #1, 0x300C
2:      lsl.w   #8, %d1
        or.w    #0x02, %d1
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

        | bitmap com os bits altos de p1 (hi = index<<6 | reflect<<13 | trans<<15 ...)
        .macro  obj_bm ypos, h, link, data, x, depth, dw, iw, hi
        .long   (((\link) >> 3) >> 8) + (((\data) >> 3) << 11)
        .long   0 + ((\ypos) << 3) + ((\h) << 14) + ((((\link) >> 3) & 0xFF) << 24)
        .long   ((\iw) >> 4) + (\hi)
        .long   ((\x) & 0xFFF) + ((\depth) << 12) + (1 << 15) + ((\dw) << 18) + (((\iw) & 15) << 28)
        .endm
        .balign 8
list_tpl:
        obj_bm  58, 4, 0x10010, 0x11000, 20, 4, 2, 2, 0                 | 0x10000: 16 bits
        obj_bm  58, 4, 0x10020, 0x11000, 60, 4, 2, 2, (1 << 13)         | 0x10010: refletido
        .long   0, 2                                                    | 0x10020: objeto do GPU
        .long   (0x10030 >> 3) >> 8                                     | 0x10028: desvio sempre tomado
        .long   3 + (0x7FF << 3) + (0 << 14) + (((0x10030 >> 3) & 0xFF) << 24)
        obj_bm  78, 4, 0x10040, 0x11100, 100, 2, 1, 1, (0x18 << 6)      | 0x10030: 4 bits, índice 0x30
        .long   0, 4                                                    | 0x10040: parada
list_end:

| ------------------------------------------------------------------ GPU (0xF03000)
        | escreve um registro do Blitter (32 bits) pelo GPU
        .macro  bw off, val
        r_movei \val, 1
        r_movei 0xF02200 + (\off), 2
        r_store 2, 1
        .endm
        .balign 4
gpu_code:
        r_movei g_isr0 - gpu_code + 0xF03000, 28    | interrupção 0 (forçada pelo 68000)
        r_jump CC_T, 28
        r_nop
        .word   0xE400, 0xE400, 0xE400
        .word   0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400
        .word   0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400
        r_movei g_isr3 - gpu_code + 0xF03000, 28    | interrupção 3: objeto do GPU no OP
        r_jump CC_T, 28
        r_nop
        .word   0xE400, 0xE400, 0xE400
        .word   0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400, 0xE400
g_main:                                             | 0xF03050
        r_movei 0xF03FF0, 31
        | B1: cópia de 8 bits em modo phrase, (16..31, 2..3) da origem -> (8..23, 1..2) do destino
        bw 0x00, 0x40000
        bw 0x04, 0x3018
        bw 0x0C, (1 << 16) + 8
        bw 0x10, 0x0001FFF0
        bw 0x24, 0x48000
        bw 0x28, 0x3018
        bw 0x30, (2 << 16) + 16
        bw 0x34, 0x0001FFF0
        bw 0x3C, (2 << 16) + 16
        bw 0x38, 0x01800601
        | B2: A1 lê a origem com passo de meio ponto (XADDINC), A2 escreve (DSTA2): linha 4 -> (0..7, 5)
        bw 0x00, 0x48000
        bw 0x04, 0x33018
        bw 0x0C, (4 << 16) + 0
        bw 0x18, 0
        bw 0x1C, 0
        bw 0x20, 0x00008000
        bw 0x24, 0x40000
        bw 0x28, 0x13018
        bw 0x30, (5 << 16) + 0
        bw 0x3C, (1 << 16) + 8
        bw 0x38, 0x01800801
        | B3: Z-buffer em 0x42000: Z da origem 35, apaga onde Z da origem > Z do destino, grava o Z novo
        bw 0x00, 0x42000
        bw 0x04, 0x13061
        bw 0x0C, 0
        bw 0x5C, 35
        bw 0x6C, 0x4444
        bw 0x3C, (1 << 16) + 8
        bw 0x38, 0x00110030
        | B4: XOR da origem com o destino (DSTEN, LFU 6)
        bw 0x00, 0x43000
        bw 0x04, 0x13020
        bw 0x0C, 0
        bw 0x24, 0x44000
        bw 0x28, 0x13020
        bw 0x30, 0
        bw 0x38, 0x00C00009
        | fim dos blits; liga as interrupções 0 e 3 e espera
        r_movei 0x3030, 3
        r_movei 0x600DF00D, 4
        r_store 3, 4
        r_movei G_FLAGS, 3
        r_movei 0x90, 4
        r_store 3, 4
g_loop: r_jr CC_T, g_loop
        r_nop
g_isr0: r_movei 0x3040, 26
        r_load 26, 27
        r_addq 1, 27
        r_store 26, 27
        r_movei G_FLAGS, 30
        r_load 30, 29
        r_bclr 3, 29
        r_bset 9, 29
        r_load 31, 28
        r_addq 2, 28
        r_addq 4, 31
        r_jump CC_T, 28
        r_store 30, 29
g_isr3: r_movei 0x3044, 26
        r_load 26, 27
        r_addq 1, 27
        r_store 26, 27
        r_movei 0xF00026, 26                        | OBF: libera o OP
        r_storew 26, 27
        r_movei G_FLAGS, 30
        r_load 30, 29
        r_bclr 3, 29
        r_bset 12, 29
        r_load 31, 28
        r_addq 2, 28
        r_addq 4, 31
        r_jump CC_T, 28
        r_store 30, 29
gpu_end:

| ------------------------------------------------------------------ DSP (0xF1B000), resultados em 0xF1C000
        .balign 4
dsp_code:
        r_movei 0xF1C000, 15
        r_movei 0x7FFF, 1
        r_imultn 1, 1
        r_imacn 1, 1
        r_imacn 1, 1
        r_imacn 1, 1
        r_imacn 1, 1
        r_imacn 1, 1
        r_imacn 1, 1
        r_imacn 1, 1
        r_resmac 2
        res 2                                       | 0: 0xFFF80008
        r_movei D_MACHI, 3
        r_load 3, 4
        res 4                                       | 1: 0x01
        r_moveq 5, 5
        r_sat32s 5
        res 5                                       | 2: 0x7FFFFFFF
        r_movei 0x12345678, 6
        r_mirror 6
        res 6                                       | 3: 0x1E6A2C48
        r_movei 40000, 7
        r_sat16s 7
        res 7                                       | 4: 32767
        r_movei -40000, 7
        r_sat16s 7
        res 7                                       | 5: -32768
        r_movei D_MOD, 8
        r_movei 0xFFFFFF00, 9
        r_store 8, 9
        r_movei 0x1000FC, 10
        r_addqmod 8, 10
        res 10                                      | 6: 0x100004
        r_subqmod 8, 10
        res 10                                      | 7: 0x1000FC
        | interrompe o 68000 e para
        r_movei D_CTRL, 11
        r_moveq 3, 12
        r_store 11, 12
        r_moveq 0, 12
        r_store 11, 12
        r_nop
d_halt: r_jr CC_T, d_halt
        r_nop
        .balign 4
dsp_end:
        .org    0x20000 - 4
        .long   0
