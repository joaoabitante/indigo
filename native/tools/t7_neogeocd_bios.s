| BIOS de teste do Neo Geo CD (escrita para o Índigo; não é a BIOS da SNK): usada pelo t7_neogeocd nos dois
| emuladores, o Índigo e a régua (NeoCD). Faz a partida pelo CD como a BIOS original, usando só o hardware:
|  1. liga a comunicação com a unidade ($FF0181) e as interrupções do CD ($FF0002 = $0550);
|  2. lê setores com o comando "tocar" (30 MM SS FF) pelo protocolo de 4 bits ($FF0161/$FF0163/$FF0165), na
|     interrupção de comunicação (vetor $58); cada setor decodificado pelo LC8951 chega na interrupção do
|     decodificador (vetor $54), que confere o MSF nos registradores HEAD, programa DBC/DAC/DTRG e copia o setor
|     com o DMA do console (receita $FFC5);
|  3. lê o ISO 9660 (descritor no setor 16, diretório raiz), acha o IPL.TXT e carrega cada arquivo listado
|     ("NOME.EXT,banco,deslocamento") com o DMA direto do buffer do CD: .PRG na RAM, .FIX/.SPR/.PCM/.Z80 na
|     janela de upload em $E00000 (com os bancos de sprites e de PCM; receita $FC2D nas memórias de 8 bits), como
|     a BIOS faz;
|  4. solta o Z80 ($FF0183), troca os vetores para a RAM ($3A0013) e chama o programa como a nullbios do
|     ngdevkit faz no AES (USER com o pedido 0 e depois o 2);
|  5. se o disco tem uma faixa 2, toca a faixa 2 (CD-DA) depois de carregar.
| Também traz as chamadas da BIOS que os programas do ngdevkit usam (SYSTEM_IO, FIX_CLEAR, LSP_1ST...), com o
| mesmo comportamento da nullbios no modo AES, e START começa o jogo (console de casa, sem moedas).
|
| Montagem (m68k-linux-gnu-as -m68000, ligada em $C00000, completada com $FF até 512 KB): ver t7_neogeocd.sh.

        .set    REG_P1CNT,      0x300000
        .set    REG_WATCHDOG,   0x300001
        .set    REG_P2CNT,      0x340000
        .set    REG_STATUS_B,   0x380000
        .set    REG_SELECTOR,   0x380001
        .set    REG_SWPROM,     0x3A0013
        .set    REG_VRAMADDR,   0x3C0000
        .set    REG_VRAMRW,     0x3C0002
        .set    REG_VRAMMOD,    0x3C0004
        .set    REG_IRQACK,     0x3C000C

        .set    CD_IRQMASK1,    0xFF0002
        .set    CD_IRQMASK2,    0xFF0004
        .set    CD_IRQACK,      0xFF000F
        .set    DMA_CTRL,       0xFF0061
        .set    DMA_DEST,       0xFF0064
        .set    DMA_LEN,        0xFF0070
        .set    DMA_CFG,        0xFF007E
        .set    CDC_REGSEL,     0xFF0101
        .set    CDC_REG,        0xFF0103
        .set    UP_AREA,        0xFF0105
        .set    VID_SPR,        0xFF0111
        .set    VID_FIX,        0xFF0115
        .set    VID_ON,         0xFF0119
        .set    COMM_RESP,      0xFF0161
        .set    COMM_CMD,       0xFF0163
        .set    COMM_STEP,      0xFF0165
        .set    WATCHDOG_CD,    0xFF016F
        .set    COMM_RESET,     0xFF0181
        .set    Z80_ON,         0xFF0183
        .set    SPR_BANK,       0xFF01A1
        .set    PCM_BANK,       0xFF01A3

        | variáveis da BIOS do Neo Geo (as mesmas da nullbios / ngdevkit)
        .set    BIOS_SYSTEM_MODE,    0x10FD80
        .set    BIOS_MVS_FLAG,       0x10FD82
        .set    BIOS_COUNTRY_CODE,   0x10FD83
        .set    BIOS_P1PREVIOUS,     0x10FD95
        .set    BIOS_P1CURRENT,      0x10FD96
        .set    BIOS_P1CHANGE,       0x10FD97
        .set    BIOS_P2PREVIOUS,     0x10FD9B
        .set    BIOS_P2CURRENT,      0x10FD9C
        .set    BIOS_P2CHANGE,       0x10FD9D
        .set    BIOS_STATCURNT,      0x10FDAC
        .set    BIOS_STATCHANGE,     0x10FDAD
        .set    BIOS_USER_REQUEST,   0x10FDAE
        .set    BIOS_USER_MODE,      0x10FDAF
        .set    BIOS_START_FLAG,     0x10FDB4
        .set    BIOS_STATCURNT_RAW,  0x10FEDC
        .set    BIOS_STATCHANGE_RAW, 0x10FEDD

        | variáveis desta BIOS (área livre da RAM da BIOS)
        .set    CMDBUF,   0x10FE00      | 10 meios-bytes do próximo comando
        .set    CMDPEND,  0x10FE0A      | comando esperando a próxima interrupção
        .set    RESPBUF,  0x10FE10      | 10 meios-bytes da última resposta
        .set    COMMCNT,  0x10FE1C      | interrupções de comunicação
        .set    WANTLBA,  0x10FE20      | próximo setor esperado
        .set    WANTCNT,  0x10FE24      | setores que faltam
        .set    DMADST,   0x10FE28      | destino do próximo setor
        .set    ROOTSIZE, 0x10FE34
        .set    LASTPROG, 0x10FE40      | COMMCNT do último setor recebido
        .set    DIREND,   0x10FE44
        .set    DMACFG,   0x10FE48      | receita do DMA de cada setor ($FFC5 palavras, $FC2D bytes ímpares)
        .set    DSTSTEP,  0x10FE4C      | quanto o destino anda por setor (2048 ou 4096)
        .set    BANKREG,  0x10FE50      | registrador do banco da janela (0 = sem banco)
        .set    BANKVAL,  0x10FE54

        | buffers (RAM acima da área dos programas do ngdevkit)
        .set    DIRBUF,   0x1E0000      | diretório raiz (até 16 setores)
        .set    IPLBUF,   0x1F0000      | IPL.TXT (até 4 setores)

        .set    USER,         0x122
        .set    PLAYER_START, 0x128

        .text
        .globl  _start
_start:
| ------------------------------------------------------------------ vetores do 68000
        dc.l    0x0010F300
        dc.l    reset
        .rept   19                      | vetores 2..20
        dc.l    hang
        .endr
        dc.l    cd_dec_isr              | $54: decodificador do CD
        dc.l    cd_comm_isr             | $58: comunicação com a unidade
        dc.l    exc_rte                 | $5C
        dc.l    exc_rte                 | $60: interrupção espúria
        dc.l    raster_isr              | $64: nível 3 (linha)
        dc.l    vbl_isr                 | $68: nível 1 (quadro)
        dc.l    exc_rte                 | $6C: nível 2 sem vetor do CD
        .rept   228                     | vetores 28..255
        dc.l    exc_rte
        .endr

| ------------------------------------------------------------------ tabela de chamadas (endereços da BIOS)
        .org    0x400
        dc.w    0
        .org    0x402
        jmp     reset.l                   | BIOS_INIT_HARDWARE
        jmp     hang.l                    | erro de barramento
        jmp     hang.l                    | erro de endereço
        jmp     hang.l                    | instrução ilegal
        jmp     hang.l                    | divisão por zero etc.
        jmp     hang.l                    | trace
        jmp     hang.l                    | linha F
        jmp     exc_rte.l                 | vetor não inicializado
        jmp     exc_rte.l                 | espúria
        .org    0x438
        jmp     sys_int1.l                | SYSTEM_INT1
        jmp     sys_int2.l                | SYSTEM_INT2
        jmp     sys_return.l              | SYSTEM_RETURN
        jmp     system_io.l               | SYSTEM_IO
        .rept   19                      | CREDIT_CHECK .. CHECKSUM e livres
        jmp     noop.l
        .endr
        .org    0x4C2
        jmp     fix_clear.l               | FIX_CLEAR
        jmp     lsp_1st.l                 | LSP_1st
        jmp     noop.l                    | MESS_OUT
        jmp     noop.l                    | CONTROLLER_SETUP
        jmp     noop.l
        jmp     noop.l
        .org    0x4E6
        jmp     exc_rte.l                 | CD_UNKNOWN (vetor 23)
        jmp     cd_comm_isr.l             | CD_DATA_TRANSFER (vetor 22)
        jmp     cd_dec_isr.l              | CD_DATA_READY (vetor 21)

        .org    0x500
statuscmd:                              | comando 00 (estado): soma = ~5 & 15
        dc.b    0, 0, 0, 0, 0, 0, 0, 0, 0, 0xA
        .even

| ------------------------------------------------------------------ rotinas simples
noop:
sys_return:
        rts
exc_rte:
        rte
hang:
        bra     hang

sys_int1:
vbl_isr:
        move.w  #4, REG_IRQACK
        move.b  %d0, REG_WATCHDOG
        rte
sys_int2:
raster_isr:
        move.w  #2, REG_IRQACK
        rte

| SYSTEM_IO: controles (como a nullbios) e START começa o jogo
system_io:
        movem.l %d0-%d4, -(%sp)
        move.b  BIOS_P1CURRENT, %d0
        move.b  %d0, BIOS_P1PREVIOUS
        move.b  REG_P1CNT, %d1
        not.b   %d1
        move.b  %d1, BIOS_P1CURRENT
        move.b  %d1, %d2
        eor.b   %d0, %d2
        and.b   %d1, %d2
        move.b  %d2, BIOS_P1CHANGE
        move.b  BIOS_P2CURRENT, %d0
        move.b  %d0, BIOS_P2PREVIOUS
        move.b  REG_P2CNT, %d1
        not.b   %d1
        move.b  %d1, BIOS_P2CURRENT
        move.b  %d1, %d2
        eor.b   %d0, %d2
        and.b   %d1, %d2
        move.b  %d2, BIOS_P2CHANGE
        move.b  BIOS_STATCURNT_RAW, %d0
        move.b  REG_STATUS_B, %d1
        not.b   %d1
        and.b   #0x0F, %d1
        move.b  %d1, BIOS_STATCURNT_RAW
        move.b  %d1, %d2
        eor.b   %d0, %d2
        and.b   %d1, %d2
        move.b  %d2, BIOS_STATCHANGE_RAW
        and.b   #0x55, %d1
        move.b  %d1, BIOS_STATCURNT
        move.b  %d1, BIOS_STATCHANGE
        | START do jogador 1 (bit 0) ou 2 (bit 2) apertado agora: PLAYER_START
        tst.b   BIOS_USER_MODE
        beq.s   9f
        moveq   #0, %d3
        btst    #0, %d2
        beq.s   1f
        bset    #0, %d3
1:      btst    #2, %d2
        beq.s   2f
        bset    #1, %d3
2:      tst.b   %d3
        beq.s   9f
        move.b  %d3, BIOS_START_FLAG
        movem.l %d5-%d7/%a0-%a6, -(%sp)
        jsr     PLAYER_START
        movem.l (%sp)+, %d5-%d7/%a0-%a6
9:      movem.l (%sp)+, %d0-%d4
        rts

| FIX_CLEAR: coluna da esquerda e da direita com o tile $20, o meio transparente ($FF)
fix_clear:
        move.w  #0x7000, REG_VRAMADDR
        move.w  #1, REG_VRAMMOD
        lea     REG_VRAMRW, %a0
        move.w  #31, %d0
1:      move.w  #0x20, (%a0)
        dbra    %d0, 1b
        move.w  #32*38-1, %d0
2:      move.w  #0xFF, (%a0)
        dbra    %d0, 2b
        move.w  #31, %d0
3:      move.w  #0x20, (%a0)
        dbra    %d0, 3b
        rts

| LSP_1st: SCB2 = $0FFF, SCB3 = 0, SCB4 = 380 em todos os sprites; sprite 0 transparente
lsp_1st:
        move.w  #0x8000, REG_VRAMADDR
        move.w  #1, REG_VRAMMOD
        lea     REG_VRAMRW, %a0
        move.w  #511, %d0
1:      move.w  #0x0FFF, (%a0)
        dbra    %d0, 1b
        move.w  #511, %d0
2:      move.w  #0, (%a0)
        dbra    %d0, 2b
        move.w  #511, %d0
3:      move.w  #0xBE00, (%a0)
        dbra    %d0, 3b
        move.w  #0, REG_VRAMADDR
        move.w  #2, REG_VRAMMOD
        move.w  #31, %d0
4:      move.w  #0xFF, (%a0)
        dbra    %d0, 4b
        rts

| ------------------------------------------------------------------ interrupções do CD
| comunicação: lê os 10 meios-bytes da resposta e manda os 10 do comando (o pendente ou "estado")
cd_comm_isr:
        movem.l %d0-%d1/%a0, -(%sp)
        move.b  #0x10, CD_IRQACK
        lea     RESPBUF, %a0
        moveq   #9, %d1
1:      move.b  #2, COMM_STEP
        move.b  COMM_RESP, %d0
        and.b   #0x0F, %d0
        move.b  %d0, (%a0)+
        move.b  #3, COMM_STEP
        dbra    %d1, 1b
        lea     CMDBUF, %a0
        tst.b   CMDPEND
        bne.s   2f
        lea     statuscmd, %a0
2:      moveq   #9, %d1
3:      move.b  (%a0)+, COMM_CMD
        move.b  #1, COMM_STEP
        dbra    %d1, 3b
        clr.b   CMDPEND
        addq.l  #1, COMMCNT
        movem.l (%sp)+, %d0-%d1/%a0
        rte

| decodificador: se o setor (MSF dos registradores HEAD) é o esperado, copia por DMA para DMADST
cd_dec_isr:
        movem.l %d0-%d3, -(%sp)
        move.b  #0x20, CD_IRQACK
        tst.l   WANTCNT
        beq     9f
        move.b  #4, CDC_REGSEL
        move.b  CDC_REG, %d0            | HEAD0: minutos (BCD)
        bsr     bcd2bin
        mulu.w  #60, %d0
        move.l  %d0, %d3
        move.b  CDC_REG, %d0            | HEAD1: segundos
        bsr     bcd2bin
        add.l   %d0, %d3
        mulu.w  #75, %d3
        move.b  CDC_REG, %d0            | HEAD2: quadros
        bsr     bcd2bin
        add.l   %d0, %d3
        sub.l   #150, %d3
        cmp.l   WANTLBA, %d3
        bne     9f
        move.b  #8, CDC_REGSEL          | PTL/PTH: onde o setor está no buffer do LC8951
        moveq   #0, %d1
        move.b  CDC_REG, %d1
        moveq   #0, %d2
        move.b  CDC_REG, %d2
        lsl.w   #8, %d2
        or.w    %d1, %d2
        addq.w  #4, %d2                 | pula o cabeçalho do setor
        move.b  #2, CDC_REGSEL          | DBCL, DBCH, DACL, DACH, DTRG
        move.b  #0xFF, CDC_REG
        move.b  #0x07, CDC_REG
        move.b  %d2, CDC_REG
        lsr.w   #8, %d2
        move.b  %d2, CDC_REG
        move.b  #0, CDC_REG
        move.l  DMADST, DMA_DEST
        move.l  #0x400, DMA_LEN
        move.w  DMACFG+2, DMA_CFG
        move.b  #0x40, DMA_CTRL
        move.b  #0, DMA_CTRL
        move.l  DSTSTEP, %d0
        add.l   %d0, DMADST
        tst.l   BANKREG                 | passou do fim da janela ($F00000): próximo banco
        beq.s   8f
        cmp.l   #0xF00000, DMADST
        blo.s   8f
        sub.l   #0x100000, DMADST
        addq.l  #1, BANKVAL
        move.l  %a0, -(%sp)
        move.l  BANKREG, %a0
        move.b  BANKVAL+3, (%a0)
        move.l  (%sp)+, %a0
8:
        addq.l  #1, WANTLBA
        move.l  COMMCNT, LASTPROG
        subq.l  #1, WANTCNT
9:      movem.l (%sp)+, %d0-%d3
        rte

| d0.b (BCD) -> d0.l (binário); usa d1
bcd2bin:
        and.l   #0xFF, %d0
        move.w  %d0, %d1
        lsr.w   #4, %d1
        mulu.w  #10, %d1
        and.w   #15, %d0
        add.w   %d1, %d0
        rts

| d0.w (0..99) -> d0.w em BCD; usa d1
bin2bcd:
        and.l   #0xFFFF, %d0
        divu.w  #10, %d0
        move.l  %d0, %d1
        swap    %d1
        lsl.w   #4, %d0
        or.w    %d1, %d0
        and.l   #0xFF, %d0
        rts

| ------------------------------------------------------------------ comandos para a unidade
| põe a soma nos 9 meios-bytes de CMDBUF, marca o comando e espera ele ser mandado
send_cmd:
        movem.l %d0-%d1/%a0, -(%sp)
        lea     CMDBUF, %a0
        moveq   #8, %d1
        moveq   #5, %d0
1:      add.b   (%a0)+, %d0
        dbra    %d1, 1b
        not.b   %d0
        and.b   #0x0F, %d0
        move.b  %d0, (%a0)
        st      CMDPEND
2:      tst.b   CMDPEND
        bne.s   2b
        movem.l (%sp)+, %d0-%d1/%a0
        rts

| espera a próxima interrupção de comunicação
wait_comm:
        move.l  %d0, -(%sp)
        move.l  COMMCNT, %d0
1:      cmp.l   COMMCNT, %d0
        beq.s   1b
        move.l  (%sp)+, %d0
        rts

| CMDBUF = comando d2.b (o byte 0) com os outros meios-bytes zerados
clear_cmd:
        move.l  %a0, -(%sp)
        lea     CMDBUF, %a0
        move.b  %d2, %d0
        lsr.b   #4, %d0
        move.b  %d0, (%a0)+
        move.b  %d2, %d0
        and.b   #15, %d0
        move.b  %d0, (%a0)+
        moveq   #7, %d0
1:      clr.b   (%a0)+
        dbra    %d0, 1b
        move.l  (%sp)+, %a0
        rts

| tocar a partir do LBA d0.l (comando 30 MM SS FF)
play_lba:
        movem.l %d0-%d3/%a0, -(%sp)
        move.l  %d0, %d3
        move.b  #0x30, %d2
        bsr     clear_cmd
        add.l   #150, %d3
        move.l  %d3, %d0
        divu.w  #4500, %d0              | minutos; resto em cima
        move.l  %d0, %d3
        bsr     bin2bcd
        lea     CMDBUF+2, %a0
        bsr     put_bcd
        swap    %d3
        and.l   #0xFFFF, %d3
        divu.w  #75, %d3                | segundos; resto = quadros
        move.w  %d3, %d0
        bsr     bin2bcd
        bsr     put_bcd
        swap    %d3
        move.w  %d3, %d0
        bsr     bin2bcd
        bsr     put_bcd
        bsr     send_cmd
        movem.l (%sp)+, %d0-%d3/%a0
        rts

| os dois meios-bytes de d0.b em (a0)+
put_bcd:
        move.b  %d0, %d1
        lsr.b   #4, %d1
        move.b  %d1, (%a0)+
        move.b  %d0, %d1
        and.b   #15, %d1
        move.b  %d1, (%a0)+
        rts

pause_cd:
        move.l  %d2, -(%sp)
        move.b  #0x60, %d2
        bsr     clear_cmd
        bsr     send_cmd
        move.l  (%sp)+, %d2
        rts

| lê d1.l setores a partir do LBA d0.l para a0 (por DMA); se nada chega por 40 interrupções, pede de novo
read_sectors:
        movem.l %d0-%d2/%a0, -(%sp)
        tst.l   %d1
        beq     9f
        move.l  %d0, WANTLBA
        move.l  %a0, DMADST
        move.l  %d1, WANTCNT
1:      move.l  WANTLBA, %d0
        subq.l  #2, %d0                 | começa 2 setores antes (o primeiro setor lido não interrompe)
        bpl.s   2f
        moveq   #0, %d0
2:      bsr     play_lba
        move.l  COMMCNT, LASTPROG
3:      tst.l   WANTCNT
        beq.s   8f
        move.l  COMMCNT, %d0            | (com sinal: a interrupção pode mudar LASTPROG depois da leitura)
        sub.l   LASTPROG, %d0
        cmp.l   #40, %d0
        bgt.s   1b
        bra.s   3b
8:      bsr     pause_cd
9:      movem.l (%sp)+, %d0-%d2/%a0
        rts

| ------------------------------------------------------------------ ISO 9660
| acha o nome em a1 (termina em ',', espaço, fim de linha ou 0) no diretório raiz
| devolve d0 = LBA, d1 = tamanho, d2 = 1 se achou
find_file:
        movem.l %d3-%d5/%a2-%a4, -(%sp)
        moveq   #0, %d2
        lea     DIRBUF, %a2
1:      cmp.l   DIREND, %a2
        bhs     9f
        moveq   #0, %d0
        move.b  (%a2), %d0
        bne.s   2f
        move.l  %a2, %d0                | registro de tamanho 0: o resto do setor está vazio
        sub.l   #DIRBUF, %d0
        add.l   #2048, %d0
        and.l   #0xFFFFF800, %d0
        add.l   #DIRBUF, %d0
        move.l  %d0, %a2
        bra.s   1b
2:      lea     33(%a2), %a3
        moveq   #0, %d3
        move.b  32(%a2), %d3
        move.l  %a1, %a4
3:      move.b  (%a4)+, %d4
        cmp.b   #',', %d4
        beq.s   5f
        cmp.b   #' ', %d4
        bls.s   5f
        tst.w   %d3
        beq.s   6f
        move.b  (%a3)+, %d5
        subq.w  #1, %d3
        cmp.b   #'a', %d4               | maiúscula
        blo.s   4f
        cmp.b   #'z', %d4
        bhi.s   4f
        sub.b   #32, %d4
4:      cmp.b   %d5, %d4
        bne.s   6f
        bra.s   3b
5:      tst.w   %d3                     | fim do nome procurado: no ISO tem de acabar ou vir ";1"
        beq.s   7f
        cmp.b   #';', (%a3)
        beq.s   7f
6:      add.l   %d0, %a2
        bra.s   1b
7:      moveq   #0, %d0                 | LBA e tamanho (little-endian)
        move.b  5(%a2), %d0
        lsl.l   #8, %d0
        move.b  4(%a2), %d0
        lsl.l   #8, %d0
        move.b  3(%a2), %d0
        lsl.l   #8, %d0
        move.b  2(%a2), %d0
        moveq   #0, %d1
        move.b  13(%a2), %d1
        lsl.l   #8, %d1
        move.b  12(%a2), %d1
        lsl.l   #8, %d1
        move.b  11(%a2), %d1
        lsl.l   #8, %d1
        move.b  10(%a2), %d1
        moveq   #1, %d2
9:      movem.l (%sp)+, %d3-%d5/%a2-%a4
        rts

| d1 = (d1 + 2047) / 2048
sectors_of:
        add.l   #2047, %d1
        moveq   #11, %d2
        lsr.l   %d2, %d1
        rts

| ------------------------------------------------------------------ carga de um arquivo do IPL.TXT
| a1 = nome, d6 = banco, d7 = deslocamento
load_entry:
        movem.l %d0-%d7/%a0-%a6, -(%sp)
        bsr     find_file
        tst.l   %d2
        beq     99f
        | extensão: 3 letras depois do último '.'
        move.l  %a1, %a2
        sub.l   %a3, %a3
1:      move.b  (%a2)+, %d2
        cmp.b   #',', %d2
        beq.s   2f
        cmp.b   #' ', %d2
        bls.s   2f
        cmp.b   #'.', %d2
        bne.s   1b
        move.l  %a2, %a3
        bra.s   1b
2:      cmp.l   #0, %a3
        beq     99f
        moveq   #0, %d5                 | d5 = as 3 letras em maiúsculas
        moveq   #2, %d4
3:      lsl.l   #8, %d5
        move.b  (%a3)+, %d2
        cmp.b   #'a', %d2
        blo.s   4f
        cmp.b   #'z', %d2
        bhi.s   4f
        sub.b   #32, %d2
4:      move.b  %d2, %d5
        dbra    %d4, 3b
        bsr     sectors_of              | d1 = setores (d0 = LBA)
        cmp.l   #0x505247, %d5          | PRG: direto na RAM
        bne.s   10f
        move.l  %d7, %a0
        bsr     read_sectors
        bra     99f
10:     cmp.l   #0x464958, %d5          | FIX: bytes ímpares da janela
        bne.s   11f
        move.b  #5, UP_AREA
        move.b  #1, 0xFF0129
        move.l  %d7, %d2
        add.l   %d2, %d2
        bsr     read_window_odd
        move.b  #1, 0xFF0149
        bra     99f
11:     cmp.l   #0x5A3830, %d5          | Z80 (parado enquanto carrega): bytes ímpares
        bne.s   12f
        move.b  #0, Z80_ON
        move.b  #4, UP_AREA
        move.b  #1, 0xFF0127
        move.l  %d7, %d2
        add.l   %d2, %d2
        bsr     read_window_odd
        move.b  #1, 0xFF0147
        bra     99f
12:     cmp.l   #0x50434D, %d5          | PCM: deslocamento em endereços da janela, bancos de 512 KB
        bne.s   13f
        move.b  #1, UP_AREA
        move.b  #1, 0xFF0123
        move.l  %d7, %d2                | índice = deslocamento/2 + banco * $80000
        lsr.l   #1, %d2
        move.l  %d6, %d3
        moveq   #19, %d4
        lsl.l   %d4, %d3
        add.l   %d3, %d2
        move.l  %d2, %d3
        lsr.l   %d4, %d3
        move.l  %d3, BANKVAL
        move.b  %d3, PCM_BANK
        move.l  #PCM_BANK, BANKREG
        and.l   #0x7FFFF, %d2
        add.l   %d2, %d2
        bsr     read_window_odd
        clr.l   BANKREG
        move.b  #1, 0xFF0143
        bra.s   99f
13:     cmp.l   #0x535052, %d5          | SPR: bancos de 1 MB, palavras
        bne.s   99f
        move.b  #0, UP_AREA
        move.b  #1, 0xFF0121
        move.l  %d6, %d2
        moveq   #20, %d4
        lsl.l   %d4, %d2
        add.l   %d7, %d2
        move.l  %d2, %d3
        lsr.l   %d4, %d3
        move.l  %d3, BANKVAL
        move.b  %d3, SPR_BANK
        move.l  #SPR_BANK, BANKREG
        and.l   #0xFFFFF, %d2
        add.l   #0xE00000, %d2
        move.l  %d2, %a0
        bsr     read_sectors
        clr.l   BANKREG
        move.b  #1, 0xFF0141
99:     movem.l (%sp)+, %d0-%d7/%a0-%a6
        rts

| lê d1 setores do LBA d0 para os bytes ímpares da janela a partir de $E00000 + d2 (receita $FC2D: cada palavra
| do setor vira dois bytes)
read_window_odd:
        move.w  #0xFC2D, DMACFG+2
        move.l  #4096, DSTSTEP
        add.l   #0xE00000, %d2
        move.l  %d2, %a0
        bsr     read_sectors
        move.w  #0xFFC5, DMACFG+2
        move.l  #2048, DSTSTEP
        rts

| ------------------------------------------------------------------ IPL.TXT
| lê um número a partir de a1 (base em d3: 10 ou 16) para d0; pula espaços antes
parse_num:
        moveq   #0, %d0
1:      move.b  (%a1), %d1
        cmp.b   #' ', %d1
        bne.s   2f
        addq.l  #1, %a1
        bra.s   1b
2:      move.b  (%a1), %d1
        sub.b   #'0', %d1
        cmp.b   #9, %d1
        bls.s   3f
        cmp.w   #16, %d3
        bne.s   9f
        move.b  (%a1), %d1
        or.b    #0x20, %d1              | minúscula
        sub.b   #'a', %d1
        cmp.b   #5, %d1
        bhi.s   9f
        add.b   #10, %d1
3:      and.l   #0xFF, %d1
        cmp.w   #16, %d3
        bne.s   4f
        lsl.l   #4, %d0
        bra.s   5f
4:      mulu.w  #10, %d0
5:      add.l   %d1, %d0
        addq.l  #1, %a1
        bra.s   2b
9:      rts

boot_cd:
        | descritor do volume (setor 16) e diretório raiz
        moveq   #16, %d0
        moveq   #1, %d1
        lea     DIRBUF, %a0
        bsr     read_sectors
        lea     DIRBUF, %a0
        moveq   #0, %d0                 | LBA do diretório raiz (LE em 158)
        move.b  161(%a0), %d0
        lsl.l   #8, %d0
        move.b  160(%a0), %d0
        lsl.l   #8, %d0
        move.b  159(%a0), %d0
        lsl.l   #8, %d0
        move.b  158(%a0), %d0
        moveq   #0, %d1                 | tamanho (LE em 166)
        move.b  169(%a0), %d1
        lsl.l   #8, %d1
        move.b  168(%a0), %d1
        lsl.l   #8, %d1
        move.b  167(%a0), %d1
        lsl.l   #8, %d1
        move.b  166(%a0), %d1
        cmp.l   #16*2048, %d1
        bls.s   1f
        move.l  #16*2048, %d1
1:      move.l  %d1, ROOTSIZE
        add.l   #DIRBUF, %d1
        move.l  %d1, DIREND
        move.l  ROOTSIZE, %d1
        bsr     sectors_of
        bsr     read_sectors
        | IPL.TXT
        lea     iplname, %a1
        bsr     find_file
        tst.l   %d2
        beq     9f
        move.l  %d1, %d4
        cmp.l   #4*2048-1, %d4
        bls.s   2f
        move.l  #4*2048-1, %d4
2:      move.l  %d4, %d1
        bsr     sectors_of
        lea     IPLBUF, %a0
        bsr     read_sectors
        lea     IPLBUF, %a1
        clr.b   0(%a1,%d4.l)            | fim do texto
3:      move.b  (%a1), %d0              | pula espaços e fins de linha
        beq     9f
        cmp.b   #0x1A, %d0
        beq     9f
        cmp.b   #' ', %d0
        bhi.s   4f
        addq.l  #1, %a1
        bra.s   3b
4:      move.l  %a1, %a2                | a2 = nome
5:      move.b  (%a1)+, %d0             | até a vírgula
        beq     9f
        cmp.b   #',', %d0
        bne.s   5b
        moveq   #10, %d3
        bsr     parse_num
        move.l  %d0, %d6                | banco
6:      move.b  (%a1), %d0
        beq.s   7f
        cmp.b   #',', %d0
        beq.s   61f
        cmp.b   #' ', %d0
        bne.s   7f
        addq.l  #1, %a1
        bra.s   6b
61:     addq.l  #1, %a1
        moveq   #16, %d3
        bsr     parse_num
        move.l  %d0, %d7                | deslocamento
        move.l  %a1, -(%sp)
        move.l  %a2, %a1
        bsr     load_entry
        move.l  (%sp)+, %a1
7:      move.b  (%a1), %d0              | resto da linha
        beq.s   9f
        cmp.b   #0x0A, %d0
        beq.s   3b
        addq.l  #1, %a1
        bra.s   7b
9:      rts

iplname:
        .ascii  "IPL.TXT"
        dc.b    0
        .even

| ------------------------------------------------------------------ partida
reset:
        move    #0x2700, %sr
        lea     0x10F300, %sp
        move.b  #1, WATCHDOG_CD         | vigia do CD desligado
        move.b  #0, REG_SELECTOR        | controles normais
        move.w  #0, CD_IRQMASK2         | sem interrupção de quadro durante a carga
        move.b  #0, VID_SPR
        move.b  #0, VID_FIX
        move.b  #1, VID_ON
        lea     0x10FE00, %a0           | variáveis desta BIOS
        moveq   #31, %d0
1:      clr.l   (%a0)+
        dbra    %d0, 1b
        move.w  #0xFFC5, DMACFG+2       | setores inteiros, palavra por palavra
        move.l  #2048, DSTSTEP
        | LC8951: IFCTRL = DECIEN | DOUTEN; CTRL0 = DECEN; CTRL1 = 0
        move.b  #1, CDC_REGSEL
        move.b  #0x22, CDC_REG
        move.b  #0x0A, CDC_REGSEL
        move.b  #0x80, CDC_REG
        move.b  #0x00, CDC_REG
        | comunicação com a unidade
        move.b  #0, COMM_RESET
        move.b  #1, COMM_RESET
        move.w  #0x0550, CD_IRQMASK1
        move    #0x2000, %sr
        bsr     boot_cd
        move.b  #0xFF, Z80_ON           | solta o Z80 (com reset)
        | faixa 2 de áudio? (comando 20 04: primeira e última faixa)
        move.b  #0x20, %d2
        bsr     clear_cmd
        move.b  #4, CMDBUF+3
        bsr     send_cmd
        bsr     wait_comm
        move.b  RESPBUF+4, %d0          | última faixa (BCD)
        lsl.b   #4, %d0
        or.b    RESPBUF+5, %d0
        cmp.b   #2, %d0
        blo.s   2f
        move.b  #0x0A, CDC_REGSEL       | decodificador desligado
        move.b  #0x00, CDC_REG
        move.b  #0xB0, %d2              | B0 02: toca a faixa 2
        bsr     clear_cmd
        move.b  #2, CMDBUF+3
        bsr     send_cmd
2:      | como a nullbios no AES
        move    #0x2700, %sr
        move.w  #7, REG_IRQACK
        move.b  %d0, REG_WATCHDOG
        move.w  #0x0331, CD_IRQMASK2    | interrupções de quadro e de linha
        move.b  #0, BIOS_MVS_FLAG
        move.b  #2, BIOS_COUNTRY_CODE
        clr.b   BIOS_USER_REQUEST
        clr.b   BIOS_USER_MODE
        jsr     USER
        move.b  #2, BIOS_USER_REQUEST
soft_reset:
        move    #0x2700, %sr
        move.w  #7, REG_IRQACK
        move.b  %d0, REG_WATCHDOG
        move.b  #1, BIOS_USER_MODE
        move.b  #1, REG_SWPROM          | vetores do programa (RAM)
        move    #0x2000, %sr
        bset    #7, BIOS_SYSTEM_MODE
        jsr     USER
        bra     soft_reset
