; Programa de teste do Famicom Disk System (escrito para o Índigo), carregado em $6000 pela BIOS.
; Montado com o ca65 pelo t7_fds.sh; o t7_fds_disco.py põe ele num disco de dois lados. Faz, em ordem:
;   1. desenha uma tela com os padrões do arquivo CHR (carregado pela BIOS direto na memória do PPU);
;   2. toca uma nota no canal de onda do FDS (tabela triangular, frequência $200 = ~218 Hz, sem modulação);
;   3. liga o temporizador de IRQ do adaptador (a cada 1000 ciclos, repetindo) e conta as interrupções;
;   4. lê o contador do arquivo SAVE ($D000), soma 1 e grava o arquivo de volta no disco (BIOS $E003);
;   5. pede o lado B (BIOS $E000 com A=1) e confere o arquivo de lá ($C000 = "LADO B OK").
; Variáveis (página zero), lidas pelo indigo-t7_fds:
;   $10/$11 IRQs do temporizador  $12 quadros (NMI)  $20 contador lido do SAVE  $21 assinatura do SAVE ok (1)
;   $30 estado: 0 começando, 1 SAVE gravado, 2 lado B carregado, 3 lado B conferido; $E0+erro = falha

.setcpu "6502"

irqLo  = $10
irqHi  = $11
frames = $12
lido   = $20
assin  = $21
linha  = $22
estado = $30
SAVE   = $D000
LADOB  = $C000

.segment "PROG"
        jmp Main
        jmp Nmi
        jmp Irq

Main:
        sei
        cld
        ldx #$FF
        txs
        lda #$40              ; sem o IRQ do contador de quadros do APU
        sta $4017
        lda #$00
        sta $2000
        sta $2001
        sta irqLo
        sta irqHi
        sta frames
        sta estado
        sta assin
        ; contador e assinatura do arquivo SAVE
        lda SAVE
        sta lido
        ldx #3
@sig:   lda SAVE+1,x
        cmp TxtSave,x
        bne @semSig
        dex
        bpl @sig
        inc assin
@semSig:
        ; tela: espera o vblank, paleta e tabela de nomes
        bit $2002
@vb:    bit $2002
        bpl @vb
        lda #$3F
        sta $2006
        lda #$00
        sta $2006
        ldx #0
@pal:   lda Paleta,x
        sta $2007
        inx
        cpx #32
        bne @pal
        lda #$20
        sta $2006
        lda #$00
        sta $2006
        sta linha
@lin:   ldx #0
@col:   txa
        clc
        adc linha
        adc lido              ; o contador do save muda o desenho
        and #$0F
        sta $2007
        inx
        cpx #32
        bne @col
        inc linha
        lda linha
        cmp #30
        bne @lin
        ldx #64
        lda #$E4
@atr:   sta $2007
        dex
        bne @atr
        lda #$00
        sta $2005
        sta $2005
        ; som: tabela de onda, volume direto 32, frequência $200, modulação parada
        lda #$80
        sta $4089
        ldx #0
@onda:  lda Onda,x
        sta $4040,x
        inx
        cpx #64
        bne @onda
        lda #$00
        sta $4089
        lda #$A0
        sta $4080
        lda #$80
        sta $4087
        sta $4084
        lda #$00
        sta $4082
        lda #$02
        sta $4083
        ; NMI e tela
        lda #$80
        sta $2000
        lda #$0A
        sta $2001
        ; temporizador: 1000 ciclos, repetindo
        jsr LigaTimer
        cli
        ; espera uns quadros e grava o SAVE com o contador + 1
        lda #10
        jsr Quadros
        jsr DesligaTimer
        inc SAVE
        ldx #2
        jsr $E003
        bcs @erro
        lda #1
        sta estado
        jsr LigaTimer
        lda #10
        jsr Quadros
        jsr DesligaTimer
        ; lado B
        lda #1
        jsr $E000
        bcs @erro
        lda #$0A
        sta $2001
        lda #2
        sta estado
        ldx #8
@b:     lda LADOB,x
        cmp TxtLadoB,x
        bne @fim
        dex
        bpl @b
        lda #3
        sta estado
        jsr LigaTimer
@fim:   jmp @fim
@erro:  lda $FF
        ora #$E0
        sta estado
        jmp @fim

LigaTimer:
        lda #<999
        sta $4020
        lda #>999
        sta $4021
        lda #$03
        sta $4022
        rts

DesligaTimer:
        lda #$00
        sta $4022
        rts

; espera A quadros
Quadros:
        clc
        adc frames
@l:     cmp frames
        bne @l
        rts

Nmi:
        inc frames
        rti

Irq:
        pha
        lda $4030             ; reconhece o IRQ do temporizador
        inc irqLo
        bne @s
        inc irqHi
@s:     pla
        rti

TxtSave:  .byte "SAVE"
TxtLadoB: .byte "LADO B OK"
Paleta:   .byte $0F,$01,$21,$30, $0F,$06,$16,$26, $0F,$09,$19,$29, $0F,$02,$12,$22
          .byte $0F,$01,$21,$30, $0F,$06,$16,$26, $0F,$09,$19,$29, $0F,$02,$12,$22
Onda:
.repeat 32, i
          .byte i*2
.endrepeat
.repeat 32, i
          .byte 62-i*2
.endrepeat
