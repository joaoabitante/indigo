; Teste 2 do System Card de teste: CD-DA (incluído por t7_pcecd_card.asm)
; Disco: faixa 2 (LBA 300-449) com amostras constantes L = $1234, R = $5678; faixa 3 (LBA 450-749) com tom.

c_play_lba:	db	10, $D8,$01,$00,$00,$01,$2C,$00,$00,$00,$00	; LBA 300, tocando
c_end_stop:	db	10, $D9,$03,$00,$00,$01,$36,$00,$00,$00,$00	; para no LBA 310
c_play_msf:	db	10, $D8,$01,$00,$08,$00,$00,$00,$00,$00,$40	; 00:08:00 = LBA 450
c_pause:	db	10, $DA,$00,$00,$00,$00,$00,$00,$00,$00,$00
c_play_trk:	db	10, $D8,$01,$02,$00,$00,$00,$00,$00,$00,$80	; faixa 2
c_end_irq:	db	10, $D9,$02,$00,$00,$01,$31,$00,$00,$00,$00	; LBA 305, com status no fim
c_play_tone:	db	10, $D8,$01,$00,$00,$01,$C2,$00,$00,$00,$00	; LBA 450
c_end_loop:	db	10, $D9,$01,$00,$00,$02,$ED,$00,$00,$00,$00	; LBA 749, em laço
c_subq:		db	10, $DD,$0A,$00,$00,$00,$00,$00,$00,$00,$00

cmd_ay:		jsr	loadcmd
		jsr	set_buf
		stz	<mode
		jmp	do_cmd

; SUBCODE Q: bytes 0-3 em RES, 4-9 em RES2
subq:		lda	#<c_subq
		ldy	#>c_subq
		jsr	cmd_ay
		ldy	#0
.a:		lda	BUF, y
		jsr	put
		iny
		cpy	#4
		bne	.a
.b:		lda	BUF, y
		jsr	put2
		iny
		cpy	#10
		bne	.b
		rts

; espera mais que os 700 relógios em que $1805 ignora uma segunda escrita
latch_wait:	ldx	#200
.w:		dex
		bne	.w
		rts

; troca o canal e guarda: bit 1 de $1803, $1805, $1806
latch:		jsr	latch_wait
		sta	CD_LATCH
		lda	CD_FLAGS
		and	#$02
		jsr	put
		lda	CD_LATCH
		jsr	put
		lda	CD_LATCH+1
		jmp	put

main:		jsr	cd_reset
		; linha 0: começa a faixa 2 pelo LBA
		lda	#<c_play_lba
		ldy	#>c_play_lba
		jsr	cmd_ay
		jsr	put_cmd
		jsr	nextrow
		ldx	#10
		jsr	wait_n
		; linha 1: amostra do canal direito, do esquerdo e uma segunda escrita logo em seguida (ignorada)
		jsr	latch
		jsr	latch
		jsr	latch_wait
		sta	CD_LATCH
		sta	CD_LATCH
		lda	CD_FLAGS
		and	#$02
		jsr	put
		jsr	nextrow
		; linha 2: SUBCODE Q tocando
		jsr	subq
		jsr	nextrow
		jsr	show
		; linha 3: fim no LBA 310 com parada; depois de 30 quadros: parado e amostra zero
		lda	#<c_end_stop
		ldy	#>c_end_stop
		jsr	cmd_ay
		jsr	put_cmd
		jsr	nextrow
		ldx	#30
		jsr	wait_n
		jsr	subq
		jsr	latch
		jsr	nextrow
		; linha 5: faixa 3 pelo MSF, pausa
		lda	#<c_play_msf
		ldy	#>c_play_msf
		jsr	cmd_ay
		jsr	put_cmd
		jsr	nextrow
		ldx	#5
		jsr	wait_n
		lda	#<c_pause
		ldy	#>c_pause
		jsr	cmd_ay
		jsr	put_cmd
		jsr	nextrow
		jsr	subq
		jsr	latch
		jsr	nextrow
		jsr	show
		; linha 8: faixa 2 pelo número, fim com status (a resposta só vem no LBA 305)
		lda	#<c_play_trk
		ldy	#>c_play_trk
		jsr	cmd_ay
		jsr	put_cmd
		jsr	nextrow
		lda	<frames
		pha
		lda	#<c_end_irq
		ldy	#>c_end_irq
		jsr	cmd_ay
		jsr	put_cmd
		pla
		eor	#$FF
		sec
		adc	<frames
		lsr	a
		lsr	a
		lsr	a
		jsr	put
		jsr	nextrow
		jsr	subq
		jsr	nextrow
		; linha 11: IRQ do subcódigo durante 8 quadros (uma a cada 6 amostras)
		stz	<irqseen
		lda	#$10
		sta	<irqmask
		sta	CD_IRQ
		lda	#$04
		sta	IRQ_DIS
		jsr	wait_frame
		stz	<irqcnt
		stz	<irqcnt+1
		ldx	#8
		jsr	wait_n
		lda	<irqcnt+1
		sta	<tmp+1
		lda	<irqcnt
		ldx	#5
.sh:		lsr	<tmp+1
		ror	a
		dex
		bne	.sh
		jsr	put
		lda	<irqseen
		jsr	put
		lda	#$05
		sta	IRQ_DIS
		stz	<irqmask
		stz	CD_IRQ
		; fader lido de volta
		lda	#$0C
		sta	CD_FADER
		lda	CD_FADER
		jsr	put
		stz	CD_FADER
		lda	CD_FADER
		jsr	put
		jsr	nextrow
		lda	#$EE
		jsr	put
		jsr	show
		; som: a faixa 3 (tom) em laço; 120 quadros depois, o fader rápido do CD-DA
		lda	#<c_play_tone
		ldy	#>c_play_tone
		jsr	cmd_ay
		lda	#<c_end_loop
		ldy	#>c_end_loop
		jsr	loadcmd
		jsr	send_cmd
		ldx	#120
		jsr	wait_n
		lda	#$0C
		sta	CD_FADER
		rts
