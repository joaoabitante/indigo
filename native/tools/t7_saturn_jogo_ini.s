! Homebrew de teste do Saturn do Índigo (escrito do zero): partida em 0x06004000 e as entradas de interrupção.
	.section .inicio, "ax"
	.global _inicio
_inicio:
	mov.l	pilha, r15
	! zera o .bss
	mov.l	bss_ini, r1
	mov.l	bss_fim, r2
	mov	#0, r0
1:	cmp/hs	r2, r1
	bt	2f
	mov.l	r0, @r1
	bra	1b
	add	#4, r1
2:	mov.l	f_main, r0
	jsr	@r0
	nop
3:	bra	3b
	nop
	.align 2
pilha:	.long 0x060FFF00
bss_ini: .long _bss_ini
bss_fim: .long _bss_fim
f_main:	.long _main

! entrada de interrupção: salva os registradores que o C não preserva e chama a função em C
	.macro TRATA nome, func
	.global \nome
	.align 2
\nome:
	sts.l	pr, @-r15
	mov.l	r0, @-r15
	mov.l	r1, @-r15
	mov.l	r2, @-r15
	mov.l	r3, @-r15
	mov.l	r4, @-r15
	mov.l	r5, @-r15
	mov.l	r6, @-r15
	mov.l	r7, @-r15
	sts.l	mach, @-r15
	sts.l	macl, @-r15
	mov.l	1f, r0
	jsr	@r0
	nop
	lds.l	@r15+, macl
	lds.l	@r15+, mach
	mov.l	@r15+, r7
	mov.l	@r15+, r6
	mov.l	@r15+, r5
	mov.l	@r15+, r4
	mov.l	@r15+, r3
	mov.l	@r15+, r2
	mov.l	@r15+, r1
	mov.l	@r15+, r0
	lds.l	@r15+, pr
	rte
	nop
	.align 2
1:	.long \func
	.endm

	.text
	TRATA _ent_vblank, _int_vblank
	TRATA _ent_timer0, _int_timer0
	TRATA _ent_dma0, _int_dma0
	TRATA _ent_escravo_vb, _int_escravo_vb
	TRATA _ent_padrao, _int_padrao

	.global _escravo_partida
	.align 2
_escravo_partida:
	mov.l	pilha_s, r15
	mov.l	f_escravo, r0
	jmp	@r0
	nop
	.align 2
pilha_s: .long 0x060F0000
f_escravo: .long _escravo_main
