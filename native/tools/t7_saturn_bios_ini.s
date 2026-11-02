! BIOS de teste do Saturn do Índigo (escrita do zero): vetores e a partida dos dois SH-2.
! O mestre vai para _bios_main; o escravo (BCR1 bit 15 = 1) para _bios_escravo.
	.section .vetores, "ax"
	.global inicio
inicio:
	.long	partida		! 0: PC ao ligar
	.long	0x06002000	! 1: pilha
	.long	partida		! 2: PC no reset manual
	.long	0x06002000	! 3
	.rept	124
	.long	excecao
	.endr

	.text
	.align 2
partida:
	mov.l	bcr1, r0
	mov.l	@r0, r1
	mov.l	bit15, r2
	tst	r2, r1
	bf	escravo
	mov.l	pilha_m, r15
	mov.l	f_main, r0
	jmp	@r0
	nop
escravo:
	mov.l	pilha_s, r15
	mov.l	f_escravo, r0
	jmp	@r0
	nop
excecao:
	mov.l	marca_exc, r0
	mov.l	r15, @r0
	bra	excecao
	nop
	.align 2
bcr1:	.long 0xFFFFFFE0
bit15:	.long 0x00008000
pilha_m: .long 0x06002000
pilha_s: .long 0x06001000
f_main:	.long _bios_main
f_escravo: .long _bios_escravo
marca_exc: .long 0x26000244

! salta para o programa com a pilha nova: r4 = endereço, r5 = pilha
	.global _bios_salta
_bios_salta:
	mov	r5, r15
	jmp	@r4
	nop
