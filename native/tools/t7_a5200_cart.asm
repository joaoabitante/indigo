; Cartucho de teste do Atari 5200 (32 KB, $4000-$BFFF), escrito para os testes do Índigo (tools/t7_a5200.sh).
; Liga pela BIOS (de teste ou outra; também como cartucho de diagnóstico, com $FF em $BFFD), instala os próprios tratadores de VBI, DLI e IRQ nos vetores de RAM e
; anota em $3000-$30FF o que viu: quadros, VCOUNT, potenciômetro, gatilho, teclas, botão de baixo, DLIs.
; A tela: 32 linhas em branco (DLI na última), modo F com listras, DLI sem WSYNC (troca no meio da linha),
; modo 2 com texto e JVB. Som: canal 1 em tom puro (AUDF1 = $1F: 998,8 Hz).
        processor 6502

COLPF0  = $C016
COLPF1  = $C017
COLPF2  = $C018
COLBK   = $C01A
TRIG0   = $C010
DMACTL  = $D400
CHACTL  = $D401
DLISTL  = $D402
DLISTH  = $D403
CHBASE  = $D409
WSYNC   = $D40A
VCOUNT  = $D40B
NMIEN   = $D40E
POT0    = $E800
ALLPOT  = $E808
KBCODE  = $E809
POTGO   = $E80B
IRQST   = $E80E
IRQEN   = $E80E
SKSTAT  = $E80F
SKCTL   = $E80F
AUDF1   = $E800
AUDC1   = $E801
AUDCTL  = $E808

SIG     = $3000     ; $A5 quando o cartucho começou
FRAMES  = $3001
VCNT    = $3002
POTV    = $3003
ALLP    = $3004
TRIG    = $3005
SKST    = $3006
DLIS    = $3007
IRQS    = $3008
KEY     = $3009
KEYS    = $300A
BRKS    = $300B
DLIN    = $300C     ; DLI da vez no quadro (0 = primeiro)
VCDLI   = $300D     ; VCOUNT dentro do primeiro DLI
LOOPS   = $3010     ; voltas do laço principal (2 bytes)

        org $4000
start:  sei
        lda #$A5
        sta SIG
        lda #<vbi
        sta $0204
        lda #>vbi
        sta $0205
        lda #<dli
        sta $0206
        lda #>dli
        sta $0207
        lda #<irq
        sta $0200
        lda #>irq
        sta $0201
        lda $BFFD           ; cartucho de diagnóstico ($FF): a BIOS não preparou nada, o VBI imediato é nosso
        cmp #$FF
        bne bios
        lda #<vbient
        sta $0202
        lda #>vbient
        sta $0203
bios:   jsr video
        lda #$C0
        sta NMIEN
        lda #3
        sta SKCTL
        lda #$C0
        sta IRQEN
        lda #$1F
        sta AUDF1
        lda #$AF
        sta AUDC1
        lda #0
        sta AUDCTL
        cli
loop:   inc LOOPS
        bne loop
        inc LOOPS+1
        jmp loop

video:  lda #<dlist
        sta DLISTL
        lda #>dlist
        sta DLISTH
        lda #$22            ; largura normal + DMA da lista
        sta DMACTL
        lda #>chars
        sta CHBASE
        lda #2
        sta CHACTL
        lda #$46
        sta COLBK
        lda #$0E
        sta COLPF1
        lda #$94
        sta COLPF2
        lda #$C8
        sta COLPF0
        rts

vbient: pha                 ; VBI imediato do cartucho de diagnóstico: faz o que a BIOS faria com o potenciômetro
        txa
        pha
        tya
        pha
        lda POT0
        sta $11
        lda ALLPOT
        sta ALLP
        sta POTGO
vbi:    inc FRAMES
        lda VCOUNT
        sta VCNT
        lda $11             ; PADDL0: potenciômetro lido no VBI (pela BIOS, antes do POTGO)
        sta POTV
        lda TRIG0
        sta TRIG
        lda SKSTAT
        sta SKST
        lda #0
        sta DLIN
        jsr video
        pla
        tay
        pla
        tax
        pla
        rti

dli:    pha
        lda DLIN
        bne dli2
        lda VCOUNT
        sta VCDLI
        lda #$18
        sta WSYNC
        sta COLBK           ; vale a partir da linha seguinte
        inc DLIN
        inc DLIS
        pla
        rti
dli2:   lda #$86
        sta COLBK           ; sem WSYNC: troca no meio da linha
        inc DLIS
        pla
        rti

irq:    pha
        lda IRQST
        sta IRQS
        and #$40
        bne nokb
        lda KBCODE
        sta KEY
        inc KEYS
nokb:   lda IRQST
        and #$80
        bne nobrk
        inc BRKS
nobrk:  lda #0
        sta IRQEN
        lda #$C0
        sta IRQEN
        pla
        rti

        ; lista de exibição: linhas 8-31 em branco, 32-39 em branco com DLI na última,
        ; 40-47 modo F (listras $F0), linha 48 modo F com DLI (o segundo DLI acontece nela),
        ; 49-56 modo 2 (texto), JVB
        align 256
dlist:  .byte $70, $70, $70, $F0
        .byte $4F, <stripes, >stripes
        .byte $4F, <stripes, >stripes
        .byte $4F, <stripes, >stripes
        .byte $4F, <stripes, >stripes
        .byte $4F, <stripes, >stripes
        .byte $4F, <stripes, >stripes
        .byte $4F, <stripes, >stripes
        .byte $4F, <stripes, >stripes
        .byte $CF, <stripes, >stripes
        .byte $42, <text, >text
        .byte $41, <dlist, >dlist

        align 256
stripes: ds 40, $F0
text:   .byte $21, $22, $23, $A1, $00, $01
        ds 34, $00

        align 1024
chars:  ds 8, $00                           ; espaço
        .byte $FF, $81, $81, $81, $81, $81, $81, $FF    ; 1: moldura
        ds 8*31, $00
        .byte $18, $3C, $66, $66, $7E, $66, $66, $00    ; $21: A
        .byte $7C, $66, $66, $7C, $66, $66, $7C, $00    ; $22: B
        .byte $3C, $66, $60, $60, $60, $66, $3C, $00    ; $23: C

        org $BFE8,$00
        .byte $00, $00, $34, $25, $33, $34, $25, $00, $29, $2E, $24, $29, $27, $2F, $00, $00, $00, $00, $00, $00
        .byte $00, $58      ; ano ("8"): a BIOS prepara tudo antes de ligar (com $FF ligaria direto)
        .word start
