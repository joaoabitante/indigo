; Teste 1 do System Card de teste: SCSI e memórias (incluído por t7_pcecd_card.asm)

c_tur:		db	6, $00,$00,$00,$00,$00,$00,$00,$00,$00,$00
c_toc0:		db	10, $DE,$00,$00,$00,$00,$00,$00,$00,$00,$00
c_toc1:		db	10, $DE,$01,$00,$00,$00,$00,$00,$00,$00,$00
c_toc2:		db	10, $DE,$02,$00,$00,$00,$00,$00,$00,$00,$00
c_read:		db	6, $08,$00,$00,$00,$01,$00,$00,$00,$00,$00
c_subq:		db	10, $DD,$0A,$00,$00,$00,$00,$00,$00,$00,$00
c_bad:		db	6, $12,$00,$00,$00,$00,$00,$00,$00,$00,$00

; READ(6): A = LBA baixo, X = LBA médio, Y = setores
read_cmd:	pha
		phx
		phy
		lda	#<c_read
		ldy	#>c_read
		jsr	loadcmd
		ply
		sty	cmdbuf+4
		plx
		stx	cmdbuf+2
		pla
		sta	cmdbuf+3
		rts

; TOC tipo 0/1/2 (A = endereço do comando) e uma linha: 4 bytes + cnt, st, msg, err
toc_row:	jsr	loadcmd
toc_go:		jsr	set_buf
		stz	<mode
		jsr	do_cmd
		lda	BUF
		jsr	put
		lda	BUF+1
		jsr	put
		lda	BUF+2
		jsr	put
		lda	BUF+3
		jsr	put
		lda	<cnt
		jsr	put
		lda	<st
		jsr	put
		lda	<msg
		jsr	put
		lda	<err
		jmp	put

toc_track:	pha
		lda	#<c_toc2
		ldy	#>c_toc2
		jsr	loadcmd
		pla
		sta	cmdbuf+2
		bra	toc_go

main:		jsr	cd_reset
		; linha 0: registradores depois do reset
		lda	CD_STAT
		jsr	put
		lda	CD_RESET
		jsr	put
		lda	CD_IRQ
		jsr	put
		lda	CD_FLAGS
		jsr	put
		lda	AD_STAT
		jsr	put
		lda	CD_FADER
		jsr	put
		jsr	nextrow
		jsr	show

		; linha 1: TEST UNIT READY
		lda	#<c_tur
		ldy	#>c_tur
		jsr	loadcmd
		jsr	set_buf
		stz	<mode
		jsr	do_cmd
		jsr	put_cmd
		jsr	nextrow

		; linhas 2-8: TOC
		lda	#<c_toc0
		ldy	#>c_toc0
		jsr	toc_row
		lda	#<c_toc1
		ldy	#>c_toc1
		jsr	toc_row
		lda	#$01
		jsr	toc_track
		lda	#$02
		jsr	toc_track
		lda	#$03
		jsr	toc_track
		lda	#$04
		jsr	toc_track
		lda	#$05
		jsr	toc_track
		jsr	show

		; linhas 9-10: READ(6) do LBA 1 com ACK manual
		lda	#1
		ldx	#0
		ldy	#1
		jsr	read_cmd
		jsr	set_buf
		stz	<mode
		jsr	do_cmd
		jsr	put_cmd
		lda	#$00
		sta	<tmp
		lda	#$08
		sta	<tmp+1
		jsr	sum_buf
		jsr	put_sum
		jsr	nextrow
		ldy	#32
.txt:		lda	BUF, y
		jsr	put
		iny
		cpy	#40
		bne	.txt

		; linha 11: READ(6) de 4 setores (LBA 20) para a RAM do CD com $1808
		lda	#20
		ldx	#0
		ldy	#4
		jsr	read_cmd
		stz	<ptr
		lda	#$40
		sta	<ptr+1
		lda	#1
		sta	<mode
		jsr	do_cmd
		jsr	put_cmd
		stz	<sum
		stz	<sum+1
		stz	<ptr
		lda	#$40
		sta	<ptr+1
		stz	<tmp
		lda	#$20
		sta	<tmp+1
		jsr	sum_ptr
		jsr	put_sum
		jsr	nextrow
		jsr	show

		; linha 12: SUBCODE Q (o tempo absoluto vai para RES2)
		lda	#<c_subq
		ldy	#>c_subq
		jsr	loadcmd
		jsr	set_buf
		stz	<mode
		jsr	do_cmd
		ldy	#0
.q1:		lda	BUF, y
		jsr	put
		iny
		cpy	#7
		bne	.q1
		lda	<cnt
		jsr	put
.q2:		lda	BUF, y
		jsr	put2
		iny
		cpy	#10
		bne	.q2

		; linha 13: READ além do fim do disco; linha 14: comando desconhecido
		lda	#$84
		ldx	#$03
		ldy	#1
		jsr	read_cmd
		jsr	set_buf
		stz	<mode
		jsr	do_cmd
		jsr	put_cmd
		jsr	nextrow
		lda	#<c_bad
		ldy	#>c_bad
		jsr	loadcmd
		jsr	set_buf
		stz	<mode
		jsr	do_cmd
		jsr	put_cmd
		jsr	nextrow
		jsr	show

		; linha 15: IRQ de dados (READ) e de status (TEST UNIT READY)
		stz	<irqseen
		stz	<irqcnt
		stz	<irqcnt+1
		lda	#$40
		sta	<irqmask
		sta	CD_IRQ
		lda	#$04
		sta	IRQ_DIS
		lda	#5
		ldx	#0
		ldy	#1
		jsr	read_cmd
		jsr	set_buf
		lda	#1
		sta	<mode
		jsr	do_cmd
		lda	<irqseen
		jsr	put
		lda	<irqcnt
		jsr	put
		lda	<st
		jsr	put
		lda	#$20
		sta	<irqmask
		sta	CD_IRQ
		lda	#<c_tur
		ldy	#>c_tur
		jsr	loadcmd
		stz	<mode
		jsr	do_cmd
		lda	<irqseen
		jsr	put
		lda	<irqcnt
		jsr	put
		lda	<st
		jsr	put
		lda	#$05
		sta	IRQ_DIS
		stz	<irqmask
		stz	CD_IRQ
		jsr	nextrow

		; linhas 16-18: seleção no meio de uma leitura
		lda	#30
		ldx	#0
		ldy	#10
		jsr	read_cmd
		jsr	send_cmd
		lda	#$78
		sta	<wmask
		lda	#$48
		sta	<wval
		jsr	waitsig
		lda	#0
		rol	a
		jsr	put
		ldy	#100
.r100:		lda	#$40
		sta	<wmask
		sta	<wval
		phy
		jsr	waitsig
		ply
		lda	CD_FAST
		dey
		bne	.r100
		sta	CD_STAT			; seleção com a leitura em andamento
		stz	<cnt2
.drain:		lda	#$40
		sta	<wmask
		sta	<wval
		jsr	waitsig
		bcs	.dd
		lda	CD_STAT
		and	#$38
		pha
		jsr	put
		lda	CD_DATA
		jsr	put
		jsr	ack
		inc	<cnt2
		pla
		cmp	#$38
		beq	.dd
		lda	<cnt2
		cmp	#12
		bne	.drain
.dd:		jsr	nextrow
		lda	<cnt2
		jsr	put
		lda	#$80
		sta	<wmask
		stz	<wval
		jsr	waitsig
		lda	#0
		rol	a
		jsr	put
		lda	#<c_tur
		ldy	#>c_tur
		jsr	loadcmd
		jsr	set_buf
		stz	<mode
		jsr	do_cmd
		jsr	put_cmd
		jsr	nextrow
		jsr	show

		; linha 20: memórias
		lda	$18C0
		jsr	put
		lda	$18C1
		jsr	put
		lda	$18C2
		jsr	put
		lda	$18C3
		jsr	put
		lda	JOYPORT
		and	#$80
		jsr	put
		lda	$1AFF
		jsr	put
		jsr	nextrow
		; Super System Card: bancos $68 e $7F
		lda	#$68
		tam2
		lda	#$5A
		sta	$4000
		lda	#$A5
		sta	$5FFF
		lda	#$7F
		tam2
		lda	#$3C
		sta	$4000
		lda	#$68
		tam2
		lda	$4000
		jsr	put
		lda	$5FFF
		jsr	put
		lda	#$7F
		tam2
		lda	$4000
		jsr	put
		; RAM do CD: banco $87
		lda	#$87
		tam2
		lda	#$C3
		sta	$5000
		lda	#$80
		tam2
		lda	#$87
		tam2
		lda	$5000
		jsr	put
		; BRAM: travada, liberada por $1807, travada de novo pela leitura de $1803
		lda	#$F7
		tam2
		lda	$4000
		jsr	put
		lda	#$80
		sta	CD_SUBC
		lda	$4000
		jsr	put
		lda	$4001
		jsr	put
		lda	$4003
		jsr	put
		jsr	nextrow
		lda	#$77
		sta	$4010
		lda	$4010
		jsr	put
		lda	CD_FLAGS
		lda	$4000
		jsr	put
		lda	#$80
		tam2
		; $1802 lido de volta
		lda	#$1F
		sta	CD_IRQ
		lda	CD_IRQ
		jsr	put
		stz	CD_IRQ
		lda	CD_IRQ
		jsr	put
		jsr	nextrow
		lda	#$EE			; fim
		jsr	put
		jmp	show
