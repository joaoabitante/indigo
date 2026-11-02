| Programa do 68000 do homebrew de teste do Saturn (escrito do zero): marca a RAM de som, liga o temporizador A
| do SCSP com interrupção de nível 5 e toca o canal 0 (onda quadrada de 64 amostras em 0x10000, em laço).
	.text
	.long	0x0007F000		| pilha
	.long	partida			| PC
	.rept	27
	.long	nada
	.endr
	.long	irq_timer		| 29: autovetor de nível 5
	.long	nada
	.long	nada
	.rept	32
	.long	nada
	.endr
partida:
	move.w	#0x2700, %sr
	move.l	#0x12345678, 0x7F100
	clr.l	0x7F104
	| nível 5 (101) para o temporizador A (bit 6): SCILV0 e SCILV2 com o bit 6
	move.w	#0x0040, 0x100424
	move.w	#0x0000, 0x100426
	move.w	#0x0040, 0x100428
	move.w	#0x0040, 0x10041E	| SCIEB: temporizador A
	move.w	#0x00F0, 0x100418	| TACTL = 0, TIMA = 0xF0
	| canal 0: 16 bits, laço normal, início 0x10000, laço 0..63, ataque imediato, volume direto máximo no centro
	move.w	#0x0000, 0x100002	| SA (baixo)
	move.w	#0x0000, 0x100004	| LSA
	move.w	#0x003F, 0x100006	| LEA
	move.w	#0x001F, 0x100008	| AR = 31
	move.w	#0x0000, 0x10000A	| DL, RR
	move.w	#0x0000, 0x10000C	| TL = 0
	move.w	#0x0000, 0x100010	| OCT = 0, FNS = 0
	move.w	#0xE000, 0x100016	| DISDL = 7, DIPAN = 0
	move.w	#0x000F, 0x100400	| MVOL
	move.w	#0x1821, 0x100000	| KYONEX + KYONB, laço normal (LPCTL = 1), SA alto = 1
	move.w	#0x2000, %sr		| liga as interrupções
laco:
	addq.l	#1, 0x7F108
	bra.s	laco
irq_timer:
	addq.l	#1, 0x7F104
	move.w	#0x0040, 0x100422	| SCIRE: limpa o temporizador A
	move.w	#0x00F0, 0x100418
	rte
nada:
	rte
