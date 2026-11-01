; Teste 3 do System Card de teste: ADPCM (incluído por t7_pcecd_card.asm)
; Disco: setores 100-115 com 32 KB de um seno codificado em ADPCM.

c_read_ad:	db	6, $08,$00,$00,$64,$10,$00,$00,$00,$00,$00	; LBA 100, 16 setores

; endereço do ADPCM ($1808/$1809) = A (baixo), X (alto)
ad_addr:	sta	AD_ADDRL
		stx	AD_ADDRH
		rts

ad_wwait:	lda	AD_STAT
		and	#$04
		bne	ad_wwait
		rts

ad_rwait:	lda	AD_STAT
		and	#$80
		bne	ad_rwait
		rts

flags:		lda	CD_FLAGS
		and	#$0C
		jmp	put

ad_write:	sta	AD_DATA
		jmp	ad_wwait

ad_read:	lda	AD_DATA
		pha
		jsr	ad_rwait
		pla
		rts

main:		jsr	cd_reset
		; linha 0: reset do ADPCM
		lda	#$80
		sta	AD_CTRL
		stz	AD_CTRL
		lda	AD_STAT
		jsr	put
		jsr	flags
		lda	AD_CTRL
		jsr	put
		lda	AD_DMA
		jsr	put
		lda	AD_RATE
		jsr	put
		jsr	nextrow
		; linha 1: grava 256 bytes em $0100 e lê de volta
		lda	#$00
		ldx	#$01
		jsr	ad_addr
		lda	#$03			; endereço de escrita = $0100
		sta	AD_CTRL
		stz	AD_CTRL
		cly
.w:		tya				; y * 7 + 3
		asl	a
		asl	a
		asl	a
		sty	<cnt2
		sec
		sbc	<cnt2
		clc
		adc	#3
		jsr	ad_write
		iny
		bne	.w
		lda	AD_STAT
		jsr	put
		jsr	flags
		lda	#$00
		ldx	#$01
		jsr	ad_addr
		lda	#$0C			; endereço de leitura = $0100
		sta	AD_CTRL
		stz	AD_CTRL
		jsr	ad_read			; a primeira leitura traz o valor antigo
		cly
.r:		jsr	ad_read
		sta	BUF, y
		iny
		bne	.r
		stz	<tmp
		lda	#1
		sta	<tmp+1
		jsr	sum_buf
		jsr	put_sum
		lda	BUF
		jsr	put
		lda	BUF+1
		jsr	put
		lda	BUF+255
		jsr	put
		jsr	nextrow
		jsr	show
		; linha 2: comprimento e flags de metade ($04) e fim ($08)
		lda	#$FE
		ldx	#$7F
		jsr	ad_addr
		lda	#$10			; comprimento = $7FFE
		sta	AD_CTRL
		stz	AD_CTRL
		jsr	flags
		lda	#$11
		jsr	ad_write
		jsr	flags
		lda	#$22
		jsr	ad_write
		jsr	flags
		lda	#$33
		jsr	ad_write
		jsr	flags
		lda	#$02
		ldx	#$00
		jsr	ad_addr
		lda	#$10			; comprimento = 2
		sta	AD_CTRL
		stz	AD_CTRL
		jsr	flags
		jsr	ad_read
		jsr	flags
		jsr	ad_read
		jsr	flags
		jsr	ad_read
		jsr	flags
		jsr	nextrow
		lda	AD_STAT
		jsr	put
		jsr	ad_read
		jsr	flags
		lda	AD_STAT
		jsr	put
		jsr	nextrow
		; linha 4: DMA de 16 setores (LBA 100) para o endereço 0
		lda	#$00
		ldx	#$00
		jsr	ad_addr
		lda	#$03
		sta	AD_CTRL
		stz	AD_CTRL
		lda	#$02
		sta	AD_DMA
		lda	#<c_read_ad
		ldy	#>c_read_ad
		jsr	loadcmd
		lda	#2
		sta	<mode
		jsr	do_cmd
		jsr	put_cmd
		lda	AD_DMA
		jsr	put
		stz	AD_DMA
		jsr	nextrow
		; linha 5: os primeiros 8 bytes da RAM do ADPCM
		lda	#$00
		ldx	#$00
		jsr	ad_addr
		lda	#$0C
		sta	AD_CTRL
		stz	AD_CTRL
		jsr	ad_read
		ldy	#8
.f:		jsr	ad_read
		jsr	put
		dey
		bne	.f
		jsr	show
		; linha 6: toca 4 KB a 16 kHz com parada automática
		lda	#$00
		ldx	#$00
		jsr	ad_addr
		lda	#$0C
		sta	AD_CTRL
		stz	AD_CTRL
		lda	#$FF
		ldx	#$0F
		jsr	ad_addr
		lda	#$10
		sta	AD_CTRL
		stz	AD_CTRL
		lda	#$0E
		sta	AD_RATE
		lda	<frames
		sta	<cnt2
		lda	#$60
		sta	AD_CTRL
		lda	AD_STAT
		jsr	put
.p:		lda	AD_STAT
		and	#$01
		bne	.pe
		lda	<frames
		sec
		sbc	<cnt2
		cmp	#TIMEOUT
		bcc	.p
.pe:		lda	<frames
		sec
		sbc	<cnt2
		clc
		adc	#2
		lsr	a
		lsr	a
		lsr	a
		jsr	put
		jsr	flags
		ldx	#2
		jsr	wait_n
		lda	AD_STAT
		jsr	put
		jsr	flags
		stz	AD_CTRL
		lda	AD_STAT
		jsr	put
		jsr	nextrow
		lda	#$EE
		jsr	put
		jsr	show
		; som: o seno inteiro (32 KB) em laço
.loop:		lda	#$00
		ldx	#$00
		jsr	ad_addr
		lda	#$0C
		sta	AD_CTRL
		stz	AD_CTRL
		lda	#$FF
		ldx	#$7F
		jsr	ad_addr
		lda	#$10
		sta	AD_CTRL
		stz	AD_CTRL
		lda	#$60
		sta	AD_CTRL
.e:		jsr	wait_frame
		lda	AD_STAT
		and	#$01
		beq	.e
		stz	AD_CTRL
		bra	.loop
