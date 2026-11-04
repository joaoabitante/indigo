# Teste dos saves do domínio 2 do PI escrito para o Índigo: o tipo vem do byte 0x3F do próprio cabeçalho
# (3 = SRAM, 5 = FlashRAM). SRAM: grava 16 bytes por DMA em 0x08000100 e lê de volta (DMA e leitura direta).
# Flash: lê o estado, apaga o setor 0, grava a página 2 (128 bytes) e lê de volta no modo de leitura.
.include "t7_n64.inc"

.text
.globl _start
_start:
  LI32 $s7, RES_BASE
  LI32 $t0, 0xB000003C
  lw $t1, 0($t0)
  andi $t1, 0xF0
  srl $s0, $t1, 4
  RES 0, $s0
  li $t0, 5
  beq $s0, $t0, flash
  nop
  # ---- SRAM
  LI32 $a0, texto
  LI32 $a1, 0x08000100
  li $a2, 15
  jal pi_grava
  nop
  LI32 $a0, 0xA0221000
  LI32 $a1, 0x08000100
  li $a2, 15
  jal pi_le
  nop
  LI32 $t0, 0xA8000104
  lw $t1, 0($t0)
  RES 1, $t1
  b fim
  nop
flash:
  WREG 0xA8010000, 0xE1000000
  LI32 $a0, 0xA0221100
  LI32 $a1, 0x08000000
  li $a2, 7
  jal pi_le
  nop
  WREG 0xA8010000, 0x4B000000
  WREG 0xA8010000, 0xD2000000
  WREG 0xA8010000, 0xB4000000
  LI32 $a0, pagina
  LI32 $a1, 0x08000000
  li $a2, 127
  jal pi_grava
  nop
  WREG 0xA8010000, 0xA5000002
  WREG 0xA8010000, 0xD2000000
  WREG 0xA8010000, 0xF0000000
  LI32 $a0, 0xA0221000
  LI32 $a1, 0x08000080
  li $a2, 127
  jal pi_le
  nop
fim:
  FIM_TESTE

# a0 = RDRAM, a1 = cartucho, a2 = tamanho - 1
pi_grava:
  LI32 $t1, 0x1FFFFFFF
  and $t0, $a0, $t1
  LI32 $t2, PI_DRAM_ADDR
  sw $t0, 0($t2)
  LI32 $t2, PI_CART_ADDR
  sw $a1, 0($t2)
  LI32 $t2, PI_RD_LEN
  sw $a2, 0($t2)
  b pi_espera
  nop
pi_le:
  LI32 $t1, 0x1FFFFFFF
  and $t0, $a0, $t1
  LI32 $t2, PI_DRAM_ADDR
  sw $t0, 0($t2)
  LI32 $t2, PI_CART_ADDR
  sw $a1, 0($t2)
  LI32 $t2, PI_WR_LEN
  sw $a2, 0($t2)
pi_espera:
  LI32 $t2, PI_STATUS
1: lw $t3, 0($t2)
  andi $t3, 3
  bnez $t3, 1b
  nop
  WREG PI_STATUS, 2
  jr $ra
  nop

.balign 8
texto:
  .ascii "SRAM DO INDIGO64"
.balign 8
pagina:
  .ascii "FLASHRAM DO INDIGO 64, PAGINA 2."
  .fill 96, 1, 0x5A
.balign 8
  .word 0, 0
