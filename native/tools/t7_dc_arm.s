@ Programa do ARM7 do AICA para o jogo de teste do Dreamcast (ARMv3, sem Thumb nem meia palavra): grava uma marca
@ na RAM de som, chama o SH-4 (bit 5 do MCIPD) e fica contando numa palavra da RAM.
	.text
	.arm
	.global _start
_start:
	b	reset
	b	.
	b	.
	b	.
	b	.
	b	.
	b	.
	b	.
reset:
	ldr	r0, =0x10000
	ldr	r1, =0xC0FFEE11
	str	r1, [r0]
	mov	r4, #0
	str	r4, [r0, #4]
	ldr	r2, =0x008028B8
	mov	r3, #0x20
	str	r3, [r2]
loop:
	ldr	r4, [r0, #4]
	add	r4, r4, #1
	str	r4, [r0, #4]
	b	loop
	.ltorg
