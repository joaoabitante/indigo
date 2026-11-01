| BIOS de teste do Sega CD (Índigo): partida dos dois programas (68000 principal e sub-CPU) e a multiplicação de
| 32 bits que o gcc chama no 68000. Montado duas vezes: com --defsym SUB=1 para o sub-CPU (vetores em PRG-RAM 0,
| níveis 1-6 nos autovetores) e sem para o principal (vetores e cabeçalho "SEGA" da BIOS em 000000).

        .section .vectors,"ax"
        .global _start
.ifdef SUB
        .long   0x0007D000              | pilha do sub-CPU
        .long   _start
        .rept   22
        .long   falha
        .endr
        .long   falha                   | 24: espúria
        .long   int1, int2, int3, int4, int5, int6
        .long   falha
        .rept   32
        .long   falha
        .endr
.else
        .long   0x00FFFE00              | pilha do principal
        .long   _start
        .rept   28
        .long   falha
        .endr
        .long   vint                    | 30: nível 6 (interrupção de quadro)
        .long   falha
        .rept   32
        .long   falha
        .endr
        | cabeçalho do Mega Drive (0x100): BIOS do CD ("BOOT ROM" no nome, "BR" no número de série)
        .ascii  "SEGA MEGA DRIVE "
        .ascii  "(C)INDIGO 2026  "
        .ascii  "INDIGO TESTE MEGA-CD BOOT ROM                   "
        .ascii  "INDIGO TESTE MEGA-CD BOOT ROM                   "
        .ascii  "BR 000000-0.00"
        .word   0
        .ascii  "J               "
        .long   0x00000000, 0x0001FFFF
        .long   0x00FF0000, 0x00FFFFFF
        .ascii  "            "
        .ascii  "            "
        .ascii  "                                        "
        .ascii  "U               "
.endif

        .text
_start:
        move.w  #0x2700,%sr
.ifndef SUB
        lea     __data_load,%a0
        lea     __data_start,%a1
        lea     __data_end,%a2
1:      cmp.l   %a2,%a1
        bcc.s   2f
        move.b  (%a0)+,(%a1)+
        bra.s   1b
2:
.endif
        lea     __bss_start,%a1
        lea     __bss_end,%a2
3:      cmp.l   %a2,%a1
        bcc.s   4f
        clr.b   (%a1)+
        bra.s   3b
4:      jsr     main
5:      bra.s   5b

falha:  bra.s   falha

| unsigned/signed long __mulsi3(long a, long b): 3 multiplicações de 16 bits
        .global __mulsi3
__mulsi3:
        move.w  4(%sp),%d0
        mulu.w  10(%sp),%d0
        move.w  6(%sp),%d1
        mulu.w  8(%sp),%d1
        add.w   %d1,%d0
        swap    %d0
        clr.w   %d0
        move.w  6(%sp),%d1
        mulu.w  10(%sp),%d1
        add.l   %d1,%d0
        rts

.ifndef SUB
| programa do sub-CPU (copiado para a PRG-RAM pelo principal)
        .section .rodata
        .global sub_bin, sub_bin_end
        .balign 2
sub_bin:
        .incbin "sub.bin"
sub_bin_end:
.endif
