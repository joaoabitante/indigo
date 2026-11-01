# BIOS de teste do PlayStation (etapa 9), feita para o Índigo: não tem nada da BIOS da Sony. Monta com o
# gcc mipsel (t9_ps1.sh):
#   mipsel-linux-gnu-as -march=r3000 -EL -o bios.o t9_ps1_bios.s
#   mipsel-linux-gnu-ld -EL -Ttext=0xBFC00000 -e inicio -o bios.elf bios.o
#   mipsel-linux-gnu-objcopy -O binary bios.elf bios.bin   (completada até 512 KB pelo t9_ps1.sh)
# O que faz, como a BIOS de verdade na partida, em pouco:
#  - configura o memory control e a cache, zera a RAM baixa;
#  - copia para a RAM o tratador de exceções (vetor 0x80000080, corpo em 0x80001000) e as entradas A0/B0/C0, que voltam sem fazer nada
#    (putchar em B0 3Dh: o Índigo captura o caractere na TTY);
#  - liga a GPU (480 linhas não: 320x240), pinta a tela de azul e escreve "BIOS de teste" pela TTY;
#  - liga as interrupções de vblank e salta para 0x80030000, onde a BIOS de verdade começa o menu (é ali que o
#    Índigo carrega o .exe pedido). Sem .exe, 0x80030000 tem um laço que conta os vblanks em 0x80000200.
	.set noreorder
	.set noat
	.text
	.globl inicio
inicio:
	# memory control: os mesmos valores da BIOS de verdade (tempos de acesso)
	lui	$t0, 0x1F80
	li	$t1, 0x1F000000
	sw	$t1, 0x1000($t0)
	li	$t1, 0x1F802000
	sw	$t1, 0x1004($t0)
	li	$t1, 0x0013243F
	sw	$t1, 0x1010($t0)
	li	$t1, 0x00003022
	sw	$t1, 0x100C($t0)
	li	$t1, 0x200931E1
	sw	$t1, 0x1014($t0)
	li	$t1, 0x00020843
	sw	$t1, 0x1018($t0)
	li	$t1, 0x00031125
	sw	$t1, 0x1020($t0)
	li	$t1, 0x00000B88
	sw	$t1, 0x1060($t0)
	# cache de instruções ligada, scratchpad ligado
	li	$t1, 0x0001E988
	li	$t2, 0xFFFE0130
	sw	$t1, 0($t2)
	# zera os primeiros 64 KB da RAM
	lui	$t2, 0xA000
	li	$t3, 0x10000
zera:
	sw	$zero, 0($t2)
	addiu	$t3, $t3, -4
	bnez	$t3, zera
	addiu	$t2, $t2, 4
	# vetor de exceção em 0x80000080: salta para o tratador, copiado para 0x80001000 (as entradas A0/B0/C0
	# ficam logo depois do vetor, então ele tem de ser curto)
	lui	$t4, 0xA000
	li	$t5, 0x3C1A8000          # lui k0, 0x8000
	sw	$t5, 0x80($t4)
	li	$t5, 0x375A1000          # ori k0, k0, 0x1000
	sw	$t5, 0x84($t4)
	li	$t5, 0x03400008          # jr k0
	sw	$t5, 0x88($t4)
	sw	$zero, 0x8C($t4)
	la	$t2, tratador
	la	$t3, tratador_fim
	lui	$t4, 0xA000
	ori	$t4, $t4, 0x1000
copia:
	lw	$t5, 0($t2)
	addiu	$t2, $t2, 4
	sw	$t5, 0($t4)
	bne	$t2, $t3, copia
	addiu	$t4, $t4, 4
	# A0/B0/C0: "jr ra; nop"
	lui	$t4, 0xA000
	li	$t5, 0x03E00008
	sw	$t5, 0xA0($t4)
	sw	$zero, 0xA4($t4)
	sw	$t5, 0xB0($t4)
	sw	$zero, 0xB4($t4)
	sw	$t5, 0xC0($t4)
	sw	$zero, 0xC4($t4)
	# laço do "menu" em 0x80030000: conta vblanks em 0x80000200 para sempre
	la	$t2, menu
	la	$t3, menu_fim
	lui	$t4, 0xA003
copia2:
	lw	$t5, 0($t2)
	addiu	$t2, $t2, 4
	sw	$t5, 0($t4)
	bne	$t2, $t3, copia2
	addiu	$t4, $t4, 4
	# GPU: reset, 320x240 NTSC, área de desenho, tela azul, imagem ligada
	lui	$t0, 0x1F80
	sw	$zero, 0x1814($t0)          # GP1(00) reset
	li	$t1, 0x08000001
	sw	$t1, 0x1814($t0)          # GP1(08) 320x240, 15 bits, NTSC
	li	$t1, 0x06C60260
	sw	$t1, 0x1814($t0)          # GP1(06) faixa horizontal
	li	$t1, 0x07040010
	sw	$t1, 0x1814($t0)          # GP1(07) faixa vertical
	li	$t1, 0x05000000
	sw	$t1, 0x1814($t0)          # GP1(05) início da imagem na VRAM
	li	$t1, 0xE3000000
	sw	$t1, 0x1810($t0)          # área de desenho (0,0)-(319,239)
	li	$t1, 0xE403BD3F
	sw	$t1, 0x1810($t0)
	li	$t1, 0xE5000000
	sw	$t1, 0x1810($t0)
	li	$t1, 0x02A04010           # preenche com azul escuro
	sw	$t1, 0x1810($t0)
	sw	$zero, 0x1810($t0)
	li	$t1, 0x00F00140
	sw	$t1, 0x1810($t0)
	li	$t1, 0x28F0C000           # quadrado amarelo-claro no meio
	sw	$t1, 0x1810($t0)
	li	$t1, 0x00500070
	sw	$t1, 0x1810($t0)
	li	$t1, 0x005000D0
	sw	$t1, 0x1810($t0)
	li	$t1, 0x00A00070
	sw	$t1, 0x1810($t0)
	li	$t1, 0x00A000D0
	sw	$t1, 0x1810($t0)
	li	$t1, 0x03000000
	sw	$t1, 0x1814($t0)          # GP1(03) imagem ligada
	# texto pela TTY (B0 3Dh putchar)
	la	$s0, texto
escreve:
	lbu	$a0, 0($s0)
	nop
	beqz	$a0, escreveu
	nop
	li	$t1, 0x3D
	li	$t2, 0xB0
	jalr	$t2
	nop
	b	escreve
	addiu	$s0, $s0, 1
escreveu:
	# vblank ligado, interrupções ligadas (vetores na RAM: BEV=0)
	lui	$t0, 0x1F80
	li	$t1, 0x7FF
	sw	$t1, 0x1070($t0)          # nada pendente
	li	$t1, 1
	sw	$t1, 0x1074($t0)          # I_MASK: vblank
	li	$t1, 0x00000401           # IM2 + IEc
	mtc0	$t1, $12
	nop
	lui	$sp, 0x801F
	ori	$sp, $sp, 0xFFF0
	li	$t1, 0x80030000
	jr	$t1
	nop

# ---------------------------------------------------------------- copiado para 0x80001000
tratador:
	mfc0	$k0, $13                  # CAUSE
	mfc0	$k1, $14                  # EPC
	andi	$k0, $k0, 0x7C
	bnez	$k0, outra
	nop
	# interrupção: conta o vblank e reconhece tudo
	lui	$k0, 0x8000
	lw	$at, 0x200($k0)
	nop
	addiu	$at, $at, 1
	sw	$at, 0x200($k0)
	lui	$k0, 0x1F80
	sw	$zero, 0x1070($k0)
	jr	$k1
	rfe
outra:
	addiu	$k1, $k1, 4               # syscall/break/etc: pula a instrução
	jr	$k1
	rfe
tratador_fim:

# ---------------------------------------------------------------- copiado para 0x80030000
menu:
	b	menu
	nop
menu_fim:

texto:
	.asciz "BIOS de teste do Indigo\n"
	.align 2
