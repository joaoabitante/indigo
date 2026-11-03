@ BIOS de teste do Índigo para o 3DO (etapa 9b). Escrita do zero para o Índigo; não é a BIOS do 3DO e não
@ contém nada dela. Serve só para os testes automáticos e para mostrar o núcleo rodando sem a BIOS do usuário:
@   0. parte do endereço 0 (a ROM aparece por cima da DRAM) e pula para 0x03000000; diz ao MADAM que há 2 MB de
@      DRAM e 1 MB de VRAM (MSYSBITS = 0x29);
@   1. liga o vídeo (VDL na DRAM, quadro na VRAM, MCTL) e desenha o logotipo "3DO" com o motor de cels (retângulos
@      e um quadrado com degradê girado 45 graus);
@   2. lê o disco pela unidade de CD (XBUS, comandos 0x02, 0x09 e 0x10, DMA), confere o rótulo do sistema de
@      arquivos Opera, acha o arquivo "LaunchMe" no diretório raiz, carrega em 0x00010000 e pula para ele;
@   3. sem disco (ou sem o arquivo), mostra um quadrado vermelho e tenta de novo a cada segundo.
@ Montagem: arm-none-eabi-as -mbig-endian -march=armv3; ld -EB -Ttext=0x03000000 (ver t9b_3do.sh).
        .equ MADAM, 0x03300000
        .equ CLIO,  0x03400000
        .equ SPORT, 0x03200000
        .equ VDL,   0x000F0000
        .equ CCBS,  0x000E0000
        .equ PIXS,  0x000E8000
        .equ DEGR,  0x000E9000
        .equ BUF,   0x00080000
        .equ CMDB,  0x000FF000
        .equ PROG,  0x00010000
        .equ BASEF, 0x3F660000          @ ponteiros absolutos, carrega tudo do CCB, aceita os dois sentidos
        .equ LAST,  0x40000000

        .text
        .global inicio
inicio:
        ldr     pc, =reset              @ da cópia em 0 para a ROM em 0x03000000
        .ascii  "Indigo 3DO test BIOS (GPL-3.0)\0"
        .balign 4
reset:
        ldr     r0, =MADAM
        mov     r1, #0x29               @ 2 MB de DRAM (dois conjuntos de 1 MB) e 1 MB de VRAM
        str     r1, [r0, #4]
        mov     r1, #0
        str     r1, [r1]                @ a 1ª escrita na DRAM só tira a ROM de cima (não grava)
        ldr     sp, =0x001FF000
        bl      video_init
        mov     r0, #0
        bl      limpa_tela
        ldr     r0, =logo
        mov     r1, #19
        bl      desenha_retangulos
        bl      desenha_degrade
        mov     r4, #60                 @ um segundo de logotipo
1:      bl      espera_quadro
        subs    r4, r4, #1
        bne     1b
tenta:
        bl      parte_do_cd             @ só volta se não deu para partir
        ldr     r0, =semdisco
        mov     r1, #1
        bl      desenha_retangulos
        mov     r4, #60
2:      bl      espera_quadro
        subs    r4, r4, #1
        bne     2b
        b       tenta

@ ---------------------------------------------------------------- vídeo
video_init:
        ldr     r0, =VDL
        ldr     r1, =0x002182F0         @ vídeo ligado, carrega o endereço atual e o anterior, 1 palavra, 240 linhas
        str     r1, [r0]
        ldr     r1, =0x00200000
        str     r1, [r0, #4]
        str     r1, [r0, #8]
        str     r0, [r0, #12]           @ próxima = ela mesma
        ldr     r1, =0xE0101040         @ fundo azul-escuro
        str     r1, [r0, #16]
        ldr     r2, =MADAM
        str     r0, [r2, #0x580]
        mov     r1, #0x6000             @ MCTL: VDL e DAC de vídeo ligados
        str     r1, [r2, #8]
        ldr     r1, =0x1414             @ REGCTL0: quadros de 320 pixels (256 + 64)
        str     r1, [r2, #0x130]
        ldr     r1, =0x00EF013F
        str     r1, [r2, #0x134]
        ldr     r1, =0x00200000
        str     r1, [r2, #0x138]
        str     r1, [r2, #0x13C]
        mov     pc, lr

@ r0 = palavra de preenchimento (dois pixels): cor do SPORT (0x2000) e "flash" das 75 páginas de 2 KB (0x4000;
@ a página vem dos bits 2-10 do endereço) com a máscara inteira
limpa_tela:
        ldr     r1, =SPORT
        add     r1, r1, #0x2000         @ deslocamento imediato do ARM só vai até 4095
        str     r0, [r1]
        add     r1, r1, #0x2000
        mvn     r3, #0
        mov     r2, #75                 @ 240 linhas * 640 bytes / 2048
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

@ r0 = tabela (x, y, largura, altura, cor), r1 = quantidade: um CCB por retângulo (um pixel de 16 bits esticado)
desenha_retangulos:
        stmfd   sp!, {r4-r11, lr}
        ldr     r4, =CCBS
        ldr     r5, =PIXS
        mov     r6, r1
1:      ldmia   r0!, {r7-r11}           @ x, y, l, a, cor
        mov     r11, r11, lsl #16
        str     r11, [r5]
        ldr     r1, =BASEF
        subs    r6, r6, #1
        orreq   r1, r1, #LAST
        str     r1, [r4, #0]
        add     r1, r4, #64
        str     r1, [r4, #4]            @ próximo CCB
        str     r5, [r4, #8]            @ origem
        mov     r1, #0
        str     r1, [r4, #12]
        mov     r7, r7, lsl #16
        str     r7, [r4, #16]
        mov     r8, r8, lsl #16
        str     r8, [r4, #20]
        mov     r9, r9, lsl #20
        str     r9, [r4, #24]           @ HDX = largura
        str     r1, [r4, #28]
        str     r1, [r4, #32]
        mov     r10, r10, lsl #16
        str     r10, [r4, #36]          @ VDY = altura
        str     r1, [r4, #40]
        str     r1, [r4, #44]
        ldr     r2, =0x1F001F00
        str     r2, [r4, #48]
        mov     r2, #0x16               @ PRE0: 16 bits sem PLUT, 1 linha
        str     r2, [r4, #52]
        str     r1, [r4, #56]           @ PRE1: 1 pixel por linha
        add     r4, r4, #64
        add     r5, r5, #4
        cmp     r6, #0
        bne     1b
        ldr     r2, =MADAM
        ldr     r1, =CCBS
        str     r1, [r2, #0x5A4]        @ primeiro CCB
        str     r1, [r2, #0x100]        @ desenha
        ldmfd   sp!, {r4-r11, pc}

desenha_degrade:
        @ degradê 16x16 de 16 bits gerado aqui (vermelho na horizontal, verde na vertical)
        ldr     r0, =DEGR
        mov     r1, #0                  @ linha
1:      mov     r2, #0                  @ coluna (de 2 em 2)
2:      mov     r3, r2, lsl #11         @ vermelho = coluna * 2
        orr     r3, r3, r1, lsl #6      @ verde = linha * 2
        orr     r3, r3, #31             @ azul
        add     r12, r2, #1
        mov     r12, r12, lsl #11
        orr     r12, r12, r1, lsl #6
        orr     r12, r12, #24
        orr     r3, r12, r3, lsl #16
        str     r3, [r0], #4
        add     r2, r2, #2
        cmp     r2, #16
        bne     2b
        add     r1, r1, #1
        cmp     r1, #16
        bne     1b
        ldr     r0, =CCBS + 0x1000
        ldr     r1, =BASEF | LAST
        str     r1, [r0, #0]
        mov     r1, #0
        str     r1, [r0, #4]
        ldr     r1, =DEGR
        str     r1, [r0, #8]
        mov     r1, #0
        str     r1, [r0, #12]
        ldr     r1, =75 << 16
        str     r1, [r0, #16]
        ldr     r1, =76 << 16
        str     r1, [r0, #20]
        ldr     r1, =2224360            @ 3 * cos 45 em 12.20
        str     r1, [r0, #24]
        str     r1, [r0, #28]
        ldr     r1, =-139022            @ 3 * sen 45 em 16.16
        str     r1, [r0, #32]
        rsb     r1, r1, #0
        str     r1, [r0, #36]
        mov     r1, #0
        str     r1, [r0, #40]
        str     r1, [r0, #44]
        ldr     r1, =0x1F001F00
        str     r1, [r0, #48]
        ldr     r1, =0x3D6              @ 16 bits sem PLUT, 16 linhas
        str     r1, [r0, #52]
        ldr     r1, =0x0006000F         @ 16 pixels, 8 palavras por linha
        str     r1, [r0, #56]
        ldr     r2, =MADAM
        str     r0, [r2, #0x5A4]
        str     r0, [r2, #0x100]
        mov     pc, lr

        .ltorg

@ ---------------------------------------------------------------- CD
@ r0 = 7 bytes do comando; devolve em r0 o último byte da resposta (estado da unidade). Na leitura (0x10) a
@ resposta só vem depois do último dado (que o DMA leva).
cd_cmd:
        ldr     r1, =CLIO
        mov     r2, #0
        str     r2, [r1, #0x500]        @ dispositivo 0 do XBUS = CD
        mov     r2, #7
1:      ldrb    r3, [r0], #1
        str     r3, [r1, #0x580]
        subs    r2, r2, #1
        bne     1b
2:      ldr     r3, [r1, #0x540]
        tst     r3, #0x10
        beq     2b
3:      ldr     r12, [r1, #0x580]
        ldr     r3, [r1, #0x540]
        tst     r3, #0x10
        bne     3b
        mov     r0, r12
        mov     pc, lr

@ r0 = LBA, r1 = setores, r2 = destino; r0 = 0 deu certo, -1 erro
cd_le:
        stmfd   sp!, {r4-r7, lr}
        mov     r5, r1
        mov     r6, r2
        add     r3, r0, #150
        mov     r4, #0                  @ minutos
        ldr     r7, =4500
1:      cmp     r3, r7
        subhs   r3, r3, r7
        addhs   r4, r4, #1
        bhs     1b
        mov     r2, #0                  @ segundos
2:      cmp     r3, #75
        subhs   r3, r3, #75
        addhs   r2, r2, #1
        bhs     2b
        ldr     r0, =CMDB
        mov     r1, #0x10
        strb    r1, [r0, #0]
        strb    r4, [r0, #1]
        strb    r2, [r0, #2]
        strb    r3, [r0, #3]
        mov     r1, #0
        strb    r1, [r0, #4]
        mov     r1, r5, lsr #8
        strb    r1, [r0, #5]
        strb    r5, [r0, #6]
        ldr     r1, =MADAM
        str     r6, [r1, #0x540]
        mov     r2, r5, lsl #11
        sub     r2, r2, #4              @ contagem em bytes menos 4
        str     r2, [r1, #0x544]
        ldr     r1, =CLIO
        mov     r2, #0x800
        str     r2, [r1, #0x400]        @ EXPCTL: DMA do XBUS para a RAM
        mov     r2, #0x100000
        str     r2, [r1, #0x304]        @ liga o DMA do XBUS
        bl      cd_cmd
        tst     r0, #0x10
        mvnne   r0, #0
        ldmnefd sp!, {r4-r7, pc}
        ldr     r1, =CLIO
3:      ldr     r2, [r1, #0x304]
        tst     r2, #0x100000
        bne     3b
        mov     r0, #0
        ldmfd   sp!, {r4-r7, pc}

parte_do_cd:
        stmfd   sp!, {r4-r8, lr}
        ldr     r0, =cmd_gira
        bl      cd_cmd
        tst     r0, #0x40               @ disco presente?
        beq     falhou
        ldr     r0, =cmd_veloc
        bl      cd_cmd
        mov     r0, #0
        mov     r1, #1
        ldr     r2, =BUF
        bl      cd_le
        cmp     r0, #0
        bne     falhou
        ldr     r4, =BUF
        ldrb    r0, [r4, #0]
        cmp     r0, #1
        bne     falhou
        ldrb    r0, [r4, #1]
        cmp     r0, #0x5A
        bne     falhou
        ldr     r0, [r4, #100]          @ bloco do diretório raiz
        mov     r1, #1
        add     r2, r4, #0x800
        bl      cd_le
        cmp     r0, #0
        bne     falhou
        add     r5, r4, #0x800
        ldr     r0, [r5, #16]           @ primeira entrada
        add     r5, r5, r0
        mov     r8, #40
entrada:
        add     r0, r5, #32
        ldr     r1, =nome
        mov     r2, #9
1:      ldrb    r3, [r0], #1
        ldrb    r12, [r1], #1
        cmp     r3, r12
        bne     outra
        subs    r2, r2, #1
        bne     1b
        ldr     r0, [r5, #68]           @ bloco da primeira cópia
        ldr     r1, [r5, #16]           @ bytes
        add     r1, r1, #2032
        add     r1, r1, #15
        mov     r1, r1, lsr #11
        ldr     r2, =PROG
        bl      cd_le
        cmp     r0, #0
        bne     falhou
        mov     lr, pc
        ldr     pc, =PROG               @ o programa do disco
3:      b       3b
outra:
        ldr     r0, [r5, #0]
        tst     r0, #0x80000000         @ última do diretório
        bne     falhou
        ldr     r0, [r5, #64]
        add     r5, r5, #72
        add     r5, r5, r0, lsl #2
        subs    r8, r8, #1
        bne     entrada
falhou:
        ldmfd   sp!, {r4-r8, pc}

        .ltorg
        .balign 4
cmd_gira:
        .byte   0x02, 0, 0, 0, 0, 0, 0
cmd_veloc:
        .byte   0x09, 3, 0x80, 0, 0, 0, 0   @ velocidade dupla
nome:
        .asciz  "LaunchMe"
        .balign 4
@ logotipo: "3DO" em blocos, uma linha rosa e seis quadrados cor de anil ("Índigo")
logo:
        .word   130, 80, 40, 10, 0x7FFF
        .word   145, 105, 25, 10, 0x7FFF
        .word   130, 130, 40, 10, 0x7FFF
        .word   160, 80, 10, 60, 0x7FFF
        .word   180, 80, 10, 60, 0x7FFF
        .word   180, 80, 30, 10, 0x7FFF
        .word   180, 130, 30, 10, 0x7FFF
        .word   205, 88, 10, 44, 0x7FFF
        .word   225, 80, 10, 60, 0x7FFF
        .word   255, 80, 10, 60, 0x7FFF
        .word   225, 80, 40, 10, 0x7FFF
        .word   225, 130, 40, 10, 0x7FFF
        .word   130, 150, 135, 4, 0x7D1F
        .word   130, 162, 18, 8, 0x241F
        .word   153, 162, 18, 8, 0x283F
        .word   176, 162, 18, 8, 0x2C5F
        .word   199, 162, 18, 8, 0x307F
        .word   222, 162, 18, 8, 0x349F
        .word   247, 162, 18, 8, 0x38BF
semdisco:
        .word   150, 190, 20, 20, 0x7C00
