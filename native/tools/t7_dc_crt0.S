! Partida dos programas de teste do Dreamcast (BIOS de teste e jogo de teste do Índigo): pilha, FPSCR (PR=1,
! como o GCC do SH-4 espera), zera o .bss e chama main. A BIOS de teste fica em 0xA0000000 (pilha abaixo de
! 0x8C010000); o jogo, em 0x8C010000 (pilha no topo da RAM).
	.section .text.start, "ax"
	.global _start
_start:
	mov.l	psr, r0
	ldc	r0, sr
	mov.l	pstack, r15
	mov.l	pfpscr, r0
	lds	r0, fpscr
	mov.l	pbss, r0
	mov.l	pend, r1
	mov	#0, r2
1:	cmp/hs	r1, r0
	bt	2f
	mov.l	r2, @r0
	bra	1b
	add	#4, r0
2:	mov.l	pmain, r0
	jsr	@r0
	nop
3:	sleep
	bra	3b
	nop
	.align 2
psr:	.long	0x400000F0
pstack:	.long	STACK_TOP
pfpscr:	.long	0x00080000
pbss:	.long	__bss_start
pend:	.long	_end
pmain:	.long	main
