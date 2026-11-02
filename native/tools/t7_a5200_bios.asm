; BIOS de teste do Atari 5200 (2 KB em $F800), escrita para os testes do Índigo (tools/t7_a5200.sh).
; Não é cópia da BIOS da Atari: segue só as convenções públicas que os cartuchos usam (as mesmas da BIOS livre do
; Altirra): vetores na RAM em $0200 (VIMIRQ, VVBLKI, VVBLKD, VDSLST, VKYBDI, VKYBDF, VTRIGR...), sombras na página
; zero ($00 POKMSK, $01/$02 RTCLOK, $05/$06 SDLSTL/H, $07 SDMCTL, $08-$10 cores, $11-$18 potenciômetros), VBI
; imediato que guarda A/X/Y na pilha e sai em $FCB2 (PLA/TAY/PLA/TAX/PLA/RTI), cartucho com $FF em $BFFD liga
; direto (sem preparar nada). Sem tela de título: o cartucho liga logo depois da preparação.
        processor 6502

POKMSK  = $00
RTCLOK  = $01       ; 2 bytes, o de cima primeiro
SDLSTL  = $05
SDMCTL  = $07
PCOLR0  = $08       ; 9 cores: COLPM0-3, COLPF0-3, COLBK
PADDL0  = $11       ; 8 potenciômetros
VIMIRQ  = $0200
VVBLKI  = $0202
VVBLKD  = $0204
VDSLST  = $0206
VKYBDI  = $0208
VKYBDF  = $020A
VTRIGR  = $020C
VBRKOP  = $020E
DMACTL  = $D400
DLISTL  = $D402
NMIEN   = $D40E
NMIST   = $D40F
NMIRES  = $D40F
POTGO   = $E80B
IRQEN   = $E80E
IRQST   = $E80E
SKCTL   = $E80F

        org $F800
reset:  sei
        cld
        ldx #$FF
        txs
        lda $BFFD
        cmp #$FF
        bne prep
        jmp ($BFFE)         ; cartucho de diagnóstico: direto
prep:   ldx #0
        txa
clr:    sta $00,x           ; página zero, GTIA, ANTIC e POKEY zerados
        sta $C000,x
        sta $D400,x
        sta $E800,x
        inx
        bne clr
        ldx #15
vec:    lda vecs,x
        sta VIMIRQ,x
        dex
        bpl vec
        lda #$22
        sta SDMCTL
        lda #3
        sta SKCTL
        lda #$C0
        sta POKMSK
        sta IRQEN
        sta NMIEN
        cli
        jmp ($BFFE)

vecs:   .word irqdef, vbiimm, vbiexit, rtiexit, kbdef, kbdef, kbdef, rtiexit

; NMI: DLI pelo VDSLST, VBI pelo VVBLKI (sem nada na pilha)
nmi:    bit NMIST
        sta NMIRES
        bpl vbi
        jmp (VDSLST)
vbi:    jmp (VVBLKI)

; IRQ: VIMIRQ; o padrão separa tecla e botão de baixo, reconhece pelo POKMSK e salta pelo vetor
irq:    jmp (VIMIRQ)
irqdef: pha
        lda #$40
        bit IRQST
        bne nokb
        eor POKMSK
        sta IRQEN
        lda POKMSK
        sta IRQEN
        jmp (VKYBDI)
nokb:   bmi nobrk
        lda #$7F
        and POKMSK
        sta IRQEN
        lda POKMSK
        sta IRQEN
        jmp (VTRIGR)
nobrk:  pla
rtiexit: rti
kbdef:  pla
        rti

        org $FCB2,$FF
vbiexit: pla
        tay
        pla
        tax
        pla
        rti
; VBI imediato: relógio, sombras para o hardware, potenciômetros
vbiimm: pha
        txa
        pha
        tya
        pha
        inc RTCLOK+1
        bne vb1
        inc RTCLOK
vb1:    lda SDLSTL
        sta DLISTL
        lda SDLSTL+1
        sta DLISTL+1
        lda SDMCTL
        sta DMACTL
        ldx #8
cols:   lda PCOLR0,x
        sta $C012,x
        dex
        bpl cols
        ldx #7
pots:   lda $E800,x
        sta PADDL0,x
        dex
        bpl pots
        sta POTGO
        jmp (VVBLKD)

        org $FFFA,$FF
        .word nmi, reset, irq
