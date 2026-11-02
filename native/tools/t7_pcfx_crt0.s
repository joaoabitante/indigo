/* Partida da BIOS de teste e do programa de teste do Índigo para o PC-FX (v810-gcc).
   BIOS (-DBIOS): começa em 0xFFF00000; os vetores de 0xFFFFFE00 a 0xFFFFFFEF desviam com JR para a RAM
   (0x400 + deslocamento do vetor) e o de partida (0xFFFFFFF0) pula para _start.
   Programa: começa no endereço de carga; irq_stub guarda os registros e chama irq_dispatch(nível). */
	.section .text.start, "ax"
	.align	1
	.global	_start
_start:
	movhi	hi(__stack), r0, sp
	movea	lo(__stack), sp, sp
	movhi	hi(__gp), r0, gp
	movea	lo(__gp), gp, gp
#ifdef BIOS
	movea	0x1000, r0, r1
	ldsr	r1, psw
#endif
	movhi	hi(__bss_start), r0, r7
	movea	lo(__bss_start), r7, r7
	movhi	hi(__bss_end), r0, r8
	movea	lo(__bss_end), r8, r8
1:	cmp	r8, r7
	bnl	2f
	st.b	r0, 0[r7]
	add	1, r7
	br	1b
2:	jal	_main
3:	br	3b

#ifdef BIOS
	.section .vectors, "ax"
	.rept	31
	.hword	0xA800, 0x0600
	.fill	12, 1, 0xFF
	.endr
	movhi	hi(_start), r0, r1
	movea	lo(_start), r1, r1
	jmp	[r1]
	.fill	6, 1, 0xFF
#else
	.text
	.align	1
	.global	_irq_stub
_irq_stub:
	addi	-80, sp, sp
	st.w	r1, 0[sp]
	st.w	r2, 4[sp]
	st.w	r5, 8[sp]
	st.w	r6, 12[sp]
	st.w	r7, 16[sp]
	st.w	r8, 20[sp]
	st.w	r9, 24[sp]
	st.w	r10, 28[sp]
	st.w	r11, 32[sp]
	st.w	r12, 36[sp]
	st.w	r13, 40[sp]
	st.w	r14, 44[sp]
	st.w	r15, 48[sp]
	st.w	r16, 52[sp]
	st.w	r17, 56[sp]
	st.w	r18, 60[sp]
	st.w	r19, 64[sp]
	st.w	r30, 68[sp]
	st.w	r31, 72[sp]
	stsr	ecr, r6
	shr	4, r6
	andi	15, r6, r6
	jal	_irq_dispatch
	ld.w	0[sp], r1
	ld.w	4[sp], r2
	ld.w	8[sp], r5
	ld.w	12[sp], r6
	ld.w	16[sp], r7
	ld.w	20[sp], r8
	ld.w	24[sp], r9
	ld.w	28[sp], r10
	ld.w	32[sp], r11
	ld.w	36[sp], r12
	ld.w	40[sp], r13
	ld.w	44[sp], r14
	ld.w	48[sp], r15
	ld.w	52[sp], r16
	ld.w	56[sp], r17
	ld.w	60[sp], r18
	ld.w	64[sp], r19
	ld.w	68[sp], r30
	ld.w	72[sp], r31
	addi	80, sp, sp
	reti
#endif
