; BIOS de teste do Famicom Disk System, escrita para o Índigo (não é a disksys.rom da Nintendo).
; Montada com o ca65 (cc65) pelo t7_fds.sh. Usa só o hardware do adaptador, como a BIOS de verdade:
;   - liga o motor, espera a cabeça voltar ao começo e lê os blocos do disco byte a byte ($4030/$4031);
;   - confere o CRC de cada bloco (bit 4 de $4030 com o bit de CRC de $4025 ligado);
;   - confere o texto "*NINTENDO-HVC*" do bloco 1 e o número do lado pedido (lado errado: espera trocar);
;   - carrega todos os arquivos do lado: tipo 0 na RAM da CPU, tipos 1/2 na memória do PPU;
;   - grava de volta os dados de um arquivo no mesmo lugar do disco (intervalo, marca $80, bloco e CRC).
; Chamadas para o programa do disco:
;   $E000  JMP CarregaLado   A = lado (0 = A, 1 = B, ...)       volta com C=0 (ok) ou C=1 e o erro em $FF
;   $E003  JMP GravaArquivo  X = índice do arquivo no lado        idem
; Partida: carrega o lado 0, liga os vetores do disco ($0100/$0101 = $C0) e salta para ($DFFC).
; NMI: $0100 bits 7-6 = 11 -> ($DFFA), 10 -> ($DFF8), 01 -> ($DFF6). IRQ: $0101 bits 7-6 = 11 -> ($DFFE).

.setcpu "6502"

; registradores do adaptador
TIMER_LO  = $4020
TIMER_HI  = $4021
TIMER_CTL = $4022
IO_ENABLE = $4023
WR_DATA   = $4024
CONTROL   = $4025
EXT_OUT   = $4026
STATUS    = $4030
RD_DATA   = $4031
DRIVE     = $4032

; $4025: bit 0 motor, 1 volta a cabeça, 2 leitura, 3 espelhamento, 4 CRC, 5 sempre 1, 6 dados (fim do intervalo)
C_PARA    = $26       ; motor parado, cabeça no começo, leitura
C_LER     = $25       ; motor, leitura, ainda no intervalo
C_BLOCO   = $65       ; motor, leitura, esperando a marca $80
C_CRC     = $75       ; motor, leitura, conferindo o CRC
C_GRAVA0  = $21       ; motor, gravação, intervalo (zeros)
C_GRAVA   = $61       ; motor, gravação, dados
C_GRAVACRC= $71       ; motor, gravação, CRC

; página zero usada pela BIOS (o programa do disco não deve contar com ela durante as chamadas)
ptr       = $F0       ; endereço de carga (2 bytes)
tam       = $F2       ; bytes que faltam (2 bytes)
tipo      = $F4       ; tipo do arquivo
nArq      = $F5       ; arquivos no lado
iArq      = $F6       ; arquivo atual
lado      = $F7       ; lado pedido
alvo      = $F8       ; arquivo a gravar ($FF = só ler)
espelho   = $FA       ; cópia do bit 3 de $4025 (o programa pode mudar)
conta     = $FB
erro      = $FF
buf       = $0200     ; bloco 1 (56 bytes)

.segment "BIOS"
        jmp CarregaLado
        jmp GravaArquivo

; ------------------------------------------------------------------ partida
Reset:
        sei
        cld
        ldx #$FF
        txs
        lda #$00
        sta $2000
        sta $2001
        sta $0100
        sta $0101
        sta espelho
        lda #$40              ; sem o IRQ do contador de quadros do APU
        sta $4017
        lda #$83
        sta IO_ENABLE
        lda #$FF
        sta EXT_OUT
        lda #$E8
        sta $408A
        lda #C_PARA
        sta CONTROL
        bit $2002
@vb1:   bit $2002
        bpl @vb1
@vb2:   bit $2002
        bpl @vb2
        lda #$00
        jsr CarregaLado
        bcs @falhou
        lda #$C0
        sta $0100
        sta $0101
        jmp ($DFFC)
@falhou:
        jmp @falhou

; ------------------------------------------------------------------ NMI e IRQ
Nmi:
        bit $0100
        bpl @n1
        bvc @n2
        jmp ($DFFA)
@n2:    jmp ($DFF8)
@n1:    bvc @fim
        jmp ($DFF6)
@fim:   rti

Irq:
        bit $0101
        bpl @bios
        bvc @bios
        jmp ($DFFE)
@bios:  pha
        lda STATUS            ; reconhece o IRQ do disco/temporizador
        pla
        rti

; ------------------------------------------------------------------ disco: utilidades
; escreve A | espelho em $4025
Ctl:
        ora espelho
        sta CONTROL
        rts

; espera um byte do drive e devolve em A
LeByte:
        lda STATUS
        and #$02
        beq LeByte
        lda RD_DATA
        rts

; ~300 ciclos (mais de dois bytes do drive)
Espera:
        ldy #60
@l:     dey
        bne @l
        rts

; liga o motor do começo do disco e espera o drive ficar pronto
LigaMotor:
        lda #C_PARA
        jsr Ctl
        jsr Espera
        lda #C_LER
        jsr Ctl
@pronto:
        lda DRIVE
        and #$02
        bne @pronto
        rts

Para:
        lda #C_PARA
        jmp Ctl

; começa um bloco: espera a marca $80 e confere o código do bloco (A); C=1 se não bate
Bloco:
        pha
        lda #C_LER
        jsr Ctl
        jsr Espera            ; um byte com o bit 6 desligado zera o CRC
        lda #C_BLOCO
        jsr Ctl
        jsr LeByte            ; a marca $80 (sem IRQ, mas o bit de transferência liga, como no drive)
        jsr LeByte
        sta conta
        pla
        cmp conta
        bne @ruim
        clc
        rts
@ruim:  lda #$02              ; erro 2: bloco fora de ordem
        sta erro
        sec
        rts

; fim de um bloco: lê o CRC e confere; C=1 se o CRC está errado (erro 3)
FimBloco:
        jsr LeByte            ; primeiro byte do CRC
        lda #C_CRC
        jsr Ctl
@l:     lda STATUS
        tax
        and #$02
        beq @l
        lda #C_LER
        jsr Ctl
        txa
        and #$10
        bne @ruim
        clc
        rts
@ruim:  lda #$03
        sta erro
        sec
        rts

; ------------------------------------------------------------------ cabeçalho do lado (blocos 1 e 2)
; C=1 com erro; erro 4 = não é disco do FDS; erro 5 = outro lado
Cabecalho:
        lda #$01
        jsr Bloco
        bcs @sai
        ldx #0
@b1:    jsr LeByte
        sta buf,x
        inx
        cpx #55
        bne @b1
        jsr FimBloco
        bcs @sai
        ldx #13
@hvc:   lda buf,x
        cmp Hvc,x
        bne @naoE
        dex
        bpl @hvc
        lda #$02
        jsr Bloco
        bcs @sai
        jsr LeByte
        sta nArq
        jsr FimBloco
@sai:   rts
@naoE:  lda #$04
        sta erro
        sec
        rts

; bloco 3 (cabeçalho do arquivo): endereço, tamanho e tipo
CabArquivo:
        lda #$03
        jsr Bloco
        bcs @sai
        ldx #0
@l:     jsr LeByte
        sta buf+64,x
        inx
        cpx #15
        bne @l
        lda buf+64+10
        sta ptr
        lda buf+64+11
        sta ptr+1
        lda buf+64+12
        sta tam
        lda buf+64+13
        sta tam+1
        lda buf+64+14
        sta tipo
        jsr FimBloco
@sai:   rts

; bloco 4: lê os dados para o destino (A=0 descarta)
Dados:
        sta conta
        lda #$04
        jsr Bloco
        bcs @sai
        lda conta
        beq @descarta
        lda tipo
        beq @cpu
        lda ptr+1             ; padrões e telas: pela porta do PPU
        sta $2006
        lda ptr
        sta $2006
@ppu:   jsr Fim16
        beq @fim
        jsr LeByte
        sta $2007
        jmp @ppu
@cpu:   ldy #0
@c:     jsr Fim16
        beq @fim
        jsr LeByte
        sta (ptr),y
        inc ptr
        bne @c
        inc ptr+1
        jmp @c
@descarta:
        jsr Fim16
        beq @fim
        jsr LeByte
        jmp @descarta
@fim:   jsr FimBloco
@sai:   rts

; tam == 0? (Z=1); senão decrementa
Fim16:
        lda tam
        ora tam+1
        beq @z
        lda tam
        bne @d
        dec tam+1
@d:     dec tam
        lda #1                ; Z=0
@z:     rts

; ------------------------------------------------------------------ CarregaLado (A = lado)
CarregaLado:
        sta lado
        lda #$FF
        sta alvo
        lda #$00
        sta erro
        sta $2001             ; tela desligada durante a carga
@espera:
        lda DRIVE             ; sem disco: espera entrar
        and #$01
        bne @espera
        jsr LigaMotor
        jsr Cabecalho
        bcs @falha
        lda buf+20            ; número do lado (byte $15 do bloco 1)
        cmp lado
        beq @certo
        jsr Para              ; lado errado: espera tirar o disco e pôr outro
@tira:  lda DRIVE
        and #$01
        beq @tira
        jmp @espera
@certo: lda #$00
        sta iArq
@arq:   lda iArq
        cmp nArq
        beq @ok
        jsr CabArquivo
        bcs @falha
        lda #$01
        jsr Dados
        bcs @falha
        inc iArq
        jmp @arq
@ok:    jsr Para
        clc
        rts
@falha: jsr Para
        sec
        rts

; ------------------------------------------------------------------ GravaArquivo (X = arquivo)
; regrava o bloco 4 do arquivo X com os dados que estão no endereço de carga dele (mesmo tamanho)
GravaArquivo:
        stx alvo
        lda #$00
        sta erro
        jsr LigaMotor
        jsr Cabecalho
        bcs @falha
        lda #$00
        sta iArq
@arq:   lda iArq
        cmp alvo
        beq @grava
        cmp nArq
        beq @naoTem
        jsr CabArquivo
        bcs @falha
        lda #$00
        jsr Dados
        bcs @falha
        inc iArq
        jmp @arq
@naoTem:
        lda #$06
        sta erro
@falha: jsr Para
        sec
        rts
@grava: jsr CabArquivo
        bcs @falha
        ; a cabeça está no intervalo logo depois do CRC do bloco 3: grava os 122 bytes do intervalo, a marca $80,
        ; o código do bloco, os dados e o CRC
        lda #$00
        sta WR_DATA
        lda #C_GRAVA0
        jsr Ctl
        ldx #121
@gap:   jsr Gravou
        dex
        bne @gap
        jsr Gravou            ; 122º zero: a marca vai no próximo byte
        lda #$80
        sta WR_DATA
        lda #C_GRAVA
        jsr Ctl
        jsr Gravou
        lda #$04
        sta WR_DATA
        ldy #0
@d:     jsr Gravou
        jsr Fim16
        beq @crc
        lda (ptr),y
        sta WR_DATA
        inc ptr
        bne @d
        inc ptr+1
        jmp @d
@crc:   lda #C_GRAVACRC       ; o último byte já foi: os dois próximos são o CRC
        jsr Ctl
        jsr Espera
        jsr Espera
        jsr Para
        clc
        rts

; espera o drive pegar o byte de $4024
Gravou:
        lda STATUS
        and #$02
        beq Gravou
        rts

Hvc:    .byte "*NINTENDO-HVC*"

.segment "VECTORS"
        .word Nmi, Reset, Irq
