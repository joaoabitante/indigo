! Tabela de exceções do jogo de teste (VBR): 0x100 = exceções gerais (TRAPA), 0x600 = interrupções.
! Salva PR e MAC e chama o C (os R0-R7 já estão no banco 1, então não precisam ser salvos).
	.section .text
	.balign 1024
	.global vbr_base
vbr_base:
	.org	0x100
	sts.l	pr, @-r15
	mov.l	ptrap, r0
	jsr	@r0
	nop
	lds.l	@r15+, pr
	rte
	nop
	.align 2
ptrap:	.long	trap_handler
	.org	0x400
	bra	vbr_base + 0x600
	nop
	.org	0x600
	sts.l	pr, @-r15
	sts.l	mach, @-r15
	sts.l	macl, @-r15
	mov.l	pirq, r0
	jsr	@r0
	nop
	lds.l	@r15+, macl
	lds.l	@r15+, mach
	lds.l	@r15+, pr
	rte
	nop
	.align 2
pirq:	.long	irq_handler
