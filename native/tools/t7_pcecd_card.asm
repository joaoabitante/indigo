; ***************************************************************************
; "System Card de teste" do Índigo para o PC Engine CD (teste t7_pcecd).
;
; Não é o System Card de verdade: é um programa próprio, montado com o pceas
; (HuC), que ocupa o lugar da BIOS (256 KB, bancos $00-$1F) e aciona a
; interface do CD direto pelos registradores $1800-$180F e $1A00-$1AFF.
; Cada teste grava bytes de resultado em RES ($2200, 224 bytes, mostrados na
; tela em hexadecimal, 8 por linha) e em RES2 ($2300, conferidos só pelo
; t7_pcecd). A tela é comparada quadro a quadro com o Mesen 2 rodando este
; mesmo programa como System Card.
;
; TEST = 1: SCSI (TEST UNIT READY, READ TOC, READ(6) manual e rápido, SUBCODE
;           Q, comandos inválidos, seleção no meio da leitura, IRQs) e
;           memórias (RAM do CD, do Super System Card, BRAM, assinatura)
; TEST = 2: CD-DA (início por LBA/MSF/faixa, fim com parada/IRQ, pausa,
;           amostra em $1805/$1806, IRQ de subcódigo, fader) e, no fim, uma
;           faixa de tom tocando com o fader (para comparar o som)
; TEST = 3: ADPCM (escrita e leitura da RAM, comprimento e flags de metade/
;           fim, DMA do CD, reprodução) e, no fim, o seno em ADPCM em laço
; TEST = 4: Arcade Card (portas, incremento, deslocamento com e sem sinal,
;           gatilhos, registro de deslocamento/rotação, 2 MB)
;
; O arquivo de entrada (t7_pcecd.sh) define TEST e inclui este.
; ***************************************************************************

; ---------------------------------------------------------------- hardware
VDC_AR		= $0000
VDC_DL		= $0002
VDC_DH		= $0003
VCE_CR		= $0400
VCE_CTA		= $0402
VCE_CTW		= $0404
JOYPORT		= $1000
IRQ_DIS		= $1402
CD_STAT		= $1800
CD_DATA		= $1801
CD_IRQ		= $1802
CD_FLAGS	= $1803
CD_RESET	= $1804
CD_LATCH	= $1805
CD_SUBC		= $1807
CD_FAST		= $1808
AD_ADDRL	= $1808
AD_ADDRH	= $1809
AD_DATA		= $180A
AD_DMA		= $180B
AD_STAT		= $180C
AD_CTRL		= $180D
AD_RATE		= $180E
CD_FADER	= $180F

; ---------------------------------------------------------------- RAM ($2000 = página zero)
frames		= $2000
t0		= $2001
ri		= $2002
irqmask		= $2003
irqseen		= $2004
irqcnt		= $2005		; 2 bytes
ptr		= $2008		; 2 bytes
cnt		= $200A		; 2 bytes
sum		= $200C		; 2 bytes
tmp		= $200E		; 2 bytes
cmdlen		= $2012
st		= $2013
msg		= $2014
err		= $2015
mode		= $2016
wmask		= $2017
cur		= $2018		; 2 bytes
wval		= $201A
ri2		= $201B
cnt2		= $201C
src		= $201E		; 2 bytes
cmdbuf		= $2020		; 16 bytes
RES		= $2200
RES2		= $2300
BUF		= $2800

TIMEOUT		= 240		; quadros

; ---------------------------------------------------------------- vetores
		.bank	0
		.org	$FFF6
		dw	irq2
		dw	irq1
		dw	irq_none
		dw	irq_none
		dw	reset

		.org	$E000

; ---------------------------------------------------------------- partida
reset:		sei
		csh
		cld
		ldx	#$FF
		txs
		lda	#$FF
		tam0
		lda	#$F8
		tam1
		lda	#$80
		tam2
		lda	#$81
		tam3
		lda	#$82
		tam4
		lda	#$83
		tam5
		lda	#$00
		tam6
		lda	#$07
		sta	IRQ_DIS			; tudo desligado por enquanto
		; limpa a RAM ($2000-$3FFF)
		stz	$2000
		tii	$2000, $2001, $1FFF
		ldx	#$FF
		txs
		jsr	video_init
		lda	#$05			; só a IRQ1 (VDC)
		sta	IRQ_DIS
		cli
		jsr	wait_frame
		jsr	main
.hang:		jsr	wait_frame
		bra	.hang

irq_none:	rti

irq1:		pha
		lda	VDC_AR			; reconhece a interrupção do VDC
		inc	<frames
		pla
		rti

; IRQ2 do CD: guarda as fontes vistas, conta e deixa ligada só a do subcódigo
irq2:		pha
		lda	CD_FLAGS
		and	<irqmask
		ora	<irqseen
		sta	<irqseen
		inc	<irqcnt
		bne	.n
		inc	<irqcnt+1
.n:		lda	CD_SUBC			; limpa a do subcódigo
		lda	<irqmask
		and	#$10
		sta	<irqmask
		sta	CD_IRQ
		pla
		rti

wait_frame:	lda	<frames
.w:		cmp	<frames
		beq	.w
		rts

; espera X quadros
wait_n:		jsr	wait_frame
		dex
		bne	wait_n
		rts

; ---------------------------------------------------------------- vídeo
video_init:	stz	VCE_CR			; 5 MHz
		; paleta: 0 = preto, 1 = branco, 17 = verde, 33 = vermelho
		stz	VCE_CTA
		stz	VCE_CTA+1
		stz	VCE_CTW
		stz	VCE_CTW+1
		lda	#$FF
		sta	VCE_CTW
		lda	#$01
		sta	VCE_CTW+1
		lda	#17
		sta	VCE_CTA
		lda	#$C0
		sta	VCE_CTW
		lda	#$01
		sta	VCE_CTW+1
		lda	#33
		sta	VCE_CTA
		lda	#$38
		sta	VCE_CTW
		stz	VCE_CTW+1
		; registradores do VDC (256 x 224, BAT 32 x 32)
		cly
.r:		lda	vdc_regs, y
		bmi	.clr
		sta	VDC_AR
		lda	vdc_regs+1, y
		sta	VDC_DL
		lda	vdc_regs+2, y
		sta	VDC_DH
		iny
		iny
		iny
		bra	.r
		; zera a VRAM inteira
.clr:		st0	#0
		st1	#0
		st2	#0
		st0	#2
		ldy	#128
		clx
.z:		st1	#0
		st2	#0
		dex
		bne	.z
		dey
		bne	.z
		; letras 0-F nos tiles $100-$10F (VRAM $1000)
		st0	#0
		st1	#$00
		st2	#$10
		st0	#2
		clx
.g:		ldy	#8
.gr:		lda	font, x
		sta	VDC_DL
		stz	VDC_DH
		inx
		dey
		bne	.gr
		ldy	#8
.gz:		st1	#0
		st2	#0
		dey
		bne	.gz
		cpx	#128
		bne	.g
		; vídeo ligado com a IRQ do vblank
		st0	#5
		st1	#$88
		st2	#$00
		rts

vdc_regs:	db	$09, $00, $00		; MWR: BAT 32 x 32
		db	$0A, $02, $02		; HSR
		db	$0B, $1F, $04		; HDR
		db	$0C, $02, $17		; VPR
		db	$0D, $DF, $00		; VDW
		db	$0E, $FF, $00		; VCR
		db	$07, $00, $00		; BXR
		db	$08, $00, $00		; BYR
		db	$06, $00, $00		; RCR
		db	$0F, $00, $00		; DCR
		db	$FF

font:		db	$3C,$66,$6E,$76,$66,$66,$3C,$00
		db	$18,$38,$18,$18,$18,$18,$7E,$00
		db	$3C,$66,$06,$0C,$30,$60,$7E,$00
		db	$3C,$66,$06,$1C,$06,$66,$3C,$00
		db	$0C,$1C,$3C,$6C,$7E,$0C,$0C,$00
		db	$7E,$60,$7C,$06,$06,$66,$3C,$00
		db	$3C,$66,$60,$7C,$66,$66,$3C,$00
		db	$7E,$66,$0C,$18,$18,$18,$18,$00
		db	$3C,$66,$66,$3C,$66,$66,$3C,$00
		db	$3C,$66,$66,$3E,$06,$66,$3C,$00
		db	$18,$3C,$66,$7E,$66,$66,$66,$00
		db	$7C,$66,$66,$7C,$66,$66,$7C,$00
		db	$3C,$66,$60,$60,$60,$66,$3C,$00
		db	$78,$6C,$66,$66,$66,$6C,$78,$00
		db	$7E,$60,$60,$78,$60,$60,$7E,$00
		db	$7E,$60,$60,$78,$60,$60,$60,$00

; mostra RES: 28 linhas de 8 bytes, a partir da coluna 2
show:		cly
		stz	<cur
		stz	<cur+1
.row:		st0	#0
		lda	<cur
		clc
		adc	#2
		sta	VDC_DL
		lda	<cur+1
		sta	VDC_DH
		st0	#2
		ldx	#8
.b:		lda	RES, y
		pha
		lsr	a
		lsr	a
		lsr	a
		lsr	a
		sta	VDC_DL
		lda	#$01
		sta	VDC_DH
		pla
		and	#$0F
		sta	VDC_DL
		lda	#$01
		sta	VDC_DH
		st1	#0
		st2	#0
		iny
		dex
		bne	.b
		lda	<cur
		clc
		adc	#32
		sta	<cur
		bcc	.nc
		inc	<cur+1
.nc:		cpy	#224
		bne	.row
		rts

; ---------------------------------------------------------------- resultados
put:		ldx	<ri
		sta	RES, x
		inc	<ri
		rts

put2:		ldx	<ri2
		sta	RES2, x
		inc	<ri2
		rts

; pula para a próxima linha de 8 bytes
nextrow:	lda	<ri
		clc
		adc	#7
		and	#$F8
		sta	<ri
		rts

; ---------------------------------------------------------------- SCSI
; espera ($1800 & wmask) == wval; C = 1 se passou do tempo
waitsig:	lda	<frames
		sta	<t0
.l:		lda	CD_STAT
		and	<wmask
		cmp	<wval
		beq	.ok
		lda	<frames
		sec
		sbc	<t0
		cmp	#TIMEOUT
		bcc	.l
		sec
		rts
.ok:		clc
		rts

; ACK manual: liga, espera o REQ cair, desliga
ack:		php
		sei
		lda	<irqmask
		ora	#$80
		sta	CD_IRQ
		ldx	#0
.w:		lda	CD_STAT
		and	#$40
		beq	.d
		dex
		bne	.w
.d:		lda	<irqmask
		sta	CD_IRQ
		plp
		rts

; copia o comando da tabela (A = endereço baixo, Y = alto; 1º byte = tamanho)
loadcmd:	sta	<src
		sty	<src+1
		lda	[src]
		sta	<cmdlen
		ldy	#1
.c:		lda	[src], y
		sta	cmdbuf-1, y
		iny
		cpy	#11
		bne	.c
		rts

; executa o comando em cmdbuf. mode: 0 = dados guardados em [ptr] com ACK manual,
; 1 = guardados lendo $1808 (ACK automático), 2 = dados não lidos (DMA do ADPCM).
; Sai com cnt = bytes recebidos, st = status, msg = mensagem, err = 0 ou o passo que falhou.
do_cmd:		jsr	send_cmd
		lda	<err
		beq	recv
		rts

; seleção e envio do comando (sem ler a resposta)
send_cmd:	stz	<err
		stz	<cnt
		stz	<cnt+1
		lda	#$FF
		sta	<st
		sta	<msg
		sta	CD_STAT			; seleção
		lda	#$F8
		sta	<wmask
		lda	#$D0
		sta	<wval
		jsr	waitsig
		bcc	.sel
		lda	#1
		sta	<err
		rts
.sel:		cly
.send:		lda	#$40
		sta	<wmask
		sta	<wval
		phy
		jsr	waitsig
		ply
		bcc	.s1
		lda	#2
		sta	<err
		rts
.s1:		lda	CD_STAT
		and	#$38
		cmp	#$10
		bne	.phase			; o leitor já saiu da fase de comando
		lda	cmdbuf, y
		sta	CD_DATA
		phy
		jsr	ack
		ply
		iny
		cpy	<cmdlen
		bne	.send
.phase:		rts

; fases de dados, status e mensagem
recv:		lda	<frames
		sta	<t0
.dl:		lda	CD_STAT
		and	#$78
		cmp	#$48
		beq	.byte
		cmp	#$58
		beq	.status
		inx
		bne	.dl
		lda	<frames
		sec
		sbc	<t0
		cmp	#TIMEOUT
		bcc	.dl
		lda	#3
		sta	<err
		rts
.byte:		lda	<mode
		beq	.man
		cmp	#2
		beq	.dl			; DMA: o ADPCM lê
		lda	CD_FAST
		sta	[ptr]
		bra	.inc
.man:		lda	CD_DATA
		sta	[ptr]
		jsr	ack
.inc:		inc	<ptr
		bne	.i1
		inc	<ptr+1
.i1:		inc	<cnt
		bne	.dl
		inc	<cnt+1
		bra	.dl
.status:	lda	CD_DATA
		sta	<st
		jsr	ack
		lda	#$78
		sta	<wmask
		sta	<wval
		jsr	waitsig
		bcc	.m
		lda	#4
		sta	<err
		rts
.m:		lda	CD_DATA
		sta	<msg
		jsr	ack
		lda	#$80
		sta	<wmask
		stz	<wval
		jsr	waitsig
		bcc	.f
		lda	#5
		sta	<err
.f:		rts

; guarda cnt (2 bytes), st, msg, err
put_cmd:	lda	<cnt
		jsr	put
		lda	<cnt+1
		jsr	put
		lda	<st
		jsr	put
		lda	<msg
		jsr	put
		lda	<err
		jmp	put

; soma de 16 bits de cnt bytes a partir de BUF
sum_buf:	stz	<sum
		stz	<sum+1
		lda	#<BUF
		sta	<ptr
		lda	#>BUF
		sta	<ptr+1
; soma de tmp (2 bytes) bytes a partir de [ptr]
sum_ptr:	lda	[ptr]
		clc
		adc	<sum
		sta	<sum
		bcc	.n
		inc	<sum+1
.n:		inc	<ptr
		bne	.p
		inc	<ptr+1
.p:		lda	<tmp
		bne	.d
		dec	<tmp+1
.d:		dec	<tmp
		lda	<tmp
		ora	<tmp+1
		bne	sum_ptr
		rts

put_sum:	lda	<sum
		jsr	put
		lda	<sum+1
		jmp	put

; reset do leitor ($1804 bit 1)
cd_reset:	lda	#$02
		sta	CD_RESET
		ldx	#2
		jsr	wait_n
		stz	CD_RESET
		ldx	#2
		jmp	wait_n

set_buf:	lda	#<BUF
		sta	<ptr
		lda	#>BUF
		sta	<ptr+1
		rts

	.if	TEST = 1
		include	"t7_pcecd_t1.asm"
	.endif
	.if	TEST = 2
		include	"t7_pcecd_t2.asm"
	.endif
	.if	TEST = 3
		include	"t7_pcecd_t3.asm"
	.endif
	.if	TEST = 4
		include	"t7_pcecd_t4.asm"
	.endif

; ---------------------------------------------------------------- identificação
		.bank	$1F
		.org	$FFB6
		db	"PC Engine CD-ROM SYSTEM", 0
		db	"Indigo: System Card de teste", 0
