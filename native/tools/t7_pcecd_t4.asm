; Teste 4 do System Card de teste: Arcade Card (incluído por t7_pcecd_card.asm)
; Porta n em $1A00 + n * $10: +0/+1 dado, +2..+4 base, +5/+6 deslocamento, +7/+8 incremento, +9 controle,
; +$A gatilho. $1AE0-$1AE3 valor de 32 bits, $1AE4 deslocamento, $1AE5 rotação, $1AFE/$1AFF versão/assinatura.

AC0		= $1A00
AC1		= $1A10
AC2		= $1A20
AC3		= $1A30

; base da porta X (0, $10, $20, $30) = A (baixo), Y (médio), tmp (alto)
ac_base:	sta	AC0+2, x
		tya
		sta	AC0+3, x
		lda	<tmp
		sta	AC0+4, x
		rts

; os 9 registradores da porta X em RES (base 3, deslocamento 2, incremento 2, controle, $0A)
ac_regs:	phx
		txa
		clc
		adc	#2
		tax
		ldy	#9
.r:		lda	AC0, x
		phx
		jsr	put
		plx
		inx
		dey
		bne	.r
		plx
		rts

value:		lda	$1AE0
		jsr	put
		lda	$1AE1
		jsr	put
		lda	$1AE2
		jsr	put
		lda	$1AE3
		jmp	put

set_value:	lda	#$78
		sta	$1AE0
		lda	#$56
		sta	$1AE1
		lda	#$34
		sta	$1AE2
		lda	#$12
		sta	$1AE3
		rts

main:		; linha 0: assinatura e registradores iniciais da porta 0
		lda	$1AFE
		jsr	put
		lda	$1AFF
		jsr	put
		lda	$1AE4
		jsr	put
		lda	$1AE5
		jsr	put
		lda	$1AF0
		jsr	put
		jsr	nextrow
		ldx	#0
		jsr	ac_regs
		jsr	nextrow

		; linha 2: porta 0, incremento somado à base: grava 64 bytes, relê pela janela do banco $40
		stz	<tmp
		ldx	#0
		lda	#$00
		ldy	#$01
		jsr	ac_base
		lda	#1
		sta	AC0+7
		stz	AC0+8
		lda	#$11
		sta	AC0+9
		cly
.w:		tya
		asl	a
		sty	<cnt2
		clc
		adc	<cnt2
		inc	a
		sta	AC0
		iny
		cpy	#64
		bne	.w
		ldx	#0
		jsr	ac_regs
		jsr	nextrow
		stz	<tmp
		ldx	#0
		lda	#$00
		ldy	#$01
		jsr	ac_base
		lda	#$40
		tam2
		stz	<sum
		stz	<sum+1
		ldy	#64
.r:		lda	$4000
		clc
		adc	<sum
		sta	<sum
		bcc	.rn
		inc	<sum+1
.rn:		dey
		bne	.r
		jsr	put_sum
		lda	AC0+2
		jsr	put
		lda	AC0+3
		jsr	put
		lda	#$80
		tam2

		; porta 1: deslocamento somado (e incrementado) na base $100000
		lda	#$10
		sta	<tmp
		ldx	#$10
		lda	#$00
		ldy	#$00
		jsr	ac_base
		lda	#$10
		sta	AC1+5
		stz	AC1+6
		lda	#1
		sta	AC1+7
		stz	AC1+8
		lda	#$03
		sta	AC1+9
		lda	#$A1
		sta	AC1
		lda	#$A2
		sta	AC1+1
		lda	#$A3
		sta	AC1
		lda	AC1+5
		jsr	put
		lda	AC1+6
		jsr	put
		jsr	nextrow
		; relê $100010-$100012 pela porta 2 sem deslocamento
		lda	#$10
		sta	<tmp
		ldx	#$20
		lda	#$10
		ldy	#$00
		jsr	ac_base
		lda	#1
		sta	AC2+7
		stz	AC2+8
		lda	#$11
		sta	AC2+9
		lda	AC2
		jsr	put
		lda	AC2
		jsr	put
		lda	AC2+1
		jsr	put
		; deslocamento "com sinal" (soma $FF0000): base $100020 + 5 -> $0F0025
		lda	#$10
		sta	<tmp
		ldx	#$10
		lda	#$20
		ldy	#$00
		jsr	ac_base
		lda	#5
		sta	AC1+5
		stz	AC1+6
		lda	#$0A
		sta	AC1+9
		lda	#$99
		sta	AC1
		lda	#$0F
		sta	<tmp
		ldx	#$20
		lda	#$25
		ldy	#$00
		jsr	ac_base
		stz	AC2+9
		lda	AC2
		jsr	put
		lda	AC2
		jsr	put
		jsr	nextrow
		jsr	show

		; linha 4: gatilhos que somam o deslocamento à base
		stz	<tmp
		ldx	#$30
		lda	#$00
		ldy	#$00
		jsr	ac_base
		lda	#$20			; ao gravar o byte baixo
		sta	AC3+9
		lda	#$40
		sta	AC3+5
		lda	AC3+2
		jsr	put
		lda	AC3+3
		jsr	put
		lda	#$40			; ao gravar o byte alto
		sta	AC3+9
		lda	#$01
		sta	AC3+6
		lda	AC3+2
		jsr	put
		lda	AC3+3
		jsr	put
		lda	#$60			; ao gravar em $0A
		sta	AC3+9
		lda	AC3+2
		jsr	put
		sta	AC3+10
		lda	AC3+2
		jsr	put
		lda	AC3+3
		jsr	put
		lda	#$68			; com sinal
		sta	AC3+9
		sta	AC3+10
		lda	AC3+4
		jsr	put
		jsr	nextrow
		; linha 5: incremento na base com 16 bits e volta dos 2 MB
		lda	#$1F
		sta	<tmp
		ldx	#0
		lda	#$FF
		ldy	#$FF
		jsr	ac_base
		lda	#1
		sta	AC0+7
		stz	AC0+8
		lda	#$11
		sta	AC0+9
		lda	#$77
		sta	AC0
		lda	#$88
		sta	AC0
		lda	AC0+4
		jsr	put
		stz	<tmp
		lda	#$00
		ldy	#$00
		ldx	#0
		jsr	ac_base
		lda	AC0
		jsr	put
		lda	#$1F
		sta	<tmp
		lda	#$FF
		ldy	#$FF
		ldx	#0
		jsr	ac_base
		lda	AC0
		jsr	put
		lda	#$00			; incremento de $8000 somado ao deslocamento (16 bits)
		sta	AC0+7
		lda	#$80
		sta	AC0+8
		lda	#$03
		sta	AC0+9
		lda	#$00
		sta	AC0+5
		lda	#$90
		sta	AC0+6
		lda	AC0
		lda	AC0+6
		jsr	put
		lda	AC0
		lda	AC0+6
		jsr	put
		jsr	nextrow
		; linha 6: registrador de deslocamento e rotação
		jsr	set_value
		lda	#$03			; 3 para a esquerda
		sta	$1AE4
		jsr	value
		jsr	set_value
		lda	#$0F			; 1 para a direita
		sta	$1AE4
		jsr	value
		jsr	nextrow
		jsr	set_value
		lda	#$09			; 7 para a direita
		sta	$1AE4
		jsr	value
		jsr	set_value
		lda	#$04			; rotação de 4 para a esquerda
		sta	$1AE5
		jsr	value
		jsr	nextrow
		jsr	set_value
		lda	#$0D			; rotação de 3 para a direita
		sta	$1AE5
		jsr	value
		lda	$1AE4
		jsr	put
		lda	$1AE5
		jsr	put
		jsr	nextrow
		; linha 9: as 4 portas pela janela dos bancos $40-$43
		stz	<tmp
		ldx	#$10
		lda	#$00
		ldy	#$01
		jsr	ac_base
		stz	AC1+9
		lda	#$41
		tam2
		lda	$4000
		jsr	put
		lda	$5FFF
		jsr	put
		lda	#$80
		tam2
		lda	#$EE
		jsr	put
		jmp	show
