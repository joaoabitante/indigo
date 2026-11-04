# Teste das interfaces escrito para o Índigo: controle pelo PIF (joybus, canais 0 e 1), EEPROM 4K (canal 4),
# Controller Pak (gravar e ler 32 bytes), DMA do PI do cartucho, som pelo AI (onda quadrada contínua).
# Resultados: blocos do PIF devolvidos em 0xA0220000 + 64n; depois do primeiro quadro o laço relê o controle a
# cada quadro (bytes em 0xA0228000) e mantém o AI tocando.
.include "t7_n64.inc"

.text
.globl _start
_start:
  LI32 $s7, RES_BASE
  # controle: lê os botões do canal 0, info do canal 1 (sem controle)
  LI32 $a0, bloco_controle
  LI32 $a1, 0xA0220000
  jal pif
  nop
  LI32 $t0, MI_INTR
  lw $t1, 0($t0)
  andi $t1, 2
  RES 0, $t1
  WREG SI_STATUS, 0
  # EEPROM: info, grava o bloco 2, lê o bloco 2
  LI32 $a0, bloco_ee_info
  LI32 $a1, 0xA0220040
  jal pif
  nop
  LI32 $a0, bloco_ee_grava
  LI32 $a1, 0xA0220080
  jal pif
  nop
  LI32 $a0, bloco_ee_le
  LI32 $a1, 0xA02200C0
  jal pif
  nop
  # Controller Pak: grava 32 bytes em 0x0060 e lê de volta
  LI32 $a0, bloco_pak_grava
  LI32 $a1, 0xA0220100
  jal pif
  nop
  LI32 $a0, bloco_pak_le
  LI32 $a1, 0xA0220140
  jal pif
  nop
  # PI: copia 16 bytes do cartucho para a RDRAM 0x221000 e espera a interrupção
  LI32 $t0, dados_pi
  LI32 $t1, 0x80000400 - 0x10001000
  subu $t0, $t0, $t1
  LI32 $t2, PI_CART_ADDR
  sw $t0, 0($t2)
  WREG PI_DRAM_ADDR, 0x221000
  WREG PI_WR_LEN, 15
  LI32 $t0, PI_STATUS
1: lw $t1, 0($t0)
  andi $t1, 3
  bnez $t1, 1b
  nop
  lw $t1, 0($t0)
  RES 1, $t1
  LI32 $t0, MI_INTR
  lw $t1, 0($t0)
  andi $t1, 0x10
  RES 2, $t1
  WREG PI_STATUS, 2
  LI32 $t0, MI_INTR
  lw $t1, 0($t0)
  andi $t1, 0x10
  RES 3, $t1
  # som: 1024 amostras estéreo em 0xA0230000: esquerda +-8000 com período de 100 amostras, direita 0x1000
  LI32 $t0, 0xA0230000
  li $t1, 0
1: li $t2, 100
  divu $zero, $t1, $t2
  mfhi $t2
  sltiu $t2, $t2, 50
  li $t3, 8000
  bnez $t2, 2f
  nop
  li $t3, -8000
2: sh $t3, 0($t0)
  li $t3, 0x1000
  sh $t3, 2($t0)
  addiu $t0, 4
  addiu $t1, 1
  li $t2, 1000
  bne $t1, $t2, 1b
  nop
  WREG AI_DACRATE, 1103
  WREG AI_BITRATE, 15
  WREG AI_CONTROL, 1
  WREG AI_DRAM_ADDR, 0x230000
  WREG AI_LEN, 4000
  LI32 $t0, AI_STATUS
  lw $t1, 0($t0)
  RES 4, $t1
  LI32 $t0, AI_LEN
  lw $t1, 0($t0)
  RES 5, $t1
  LI32 $t0, MI_INTR
  lw $t1, 0($t0)
  andi $t1, 4
  RES 6, $t1
  WREG AI_STATUS, 0
  LI32 $t0, FIM
  LI32 $t1, 0x600DF00D
  sw $t1, 0($t0)
  # laço: a cada quadro relê o controle e mantém o AI com dois buffers na fila
quadro:
  LI32 $a0, bloco_controle
  LI32 $a1, 0xA0228000
  jal pif
  nop
  LI32 $t0, AI_STATUS
  lw $t1, 0($t0)
  bltz $t1, 2f
  nop
  WREG AI_DRAM_ADDR, 0x230000
  WREG AI_LEN, 4000
2: LI32 $t0, VI_CURRENT
1: lw $t1, 0($t0)
  sltiu $t1, $t1, 4
  beqz $t1, 1b
  nop
1: lw $t1, 0($t0)
  sltiu $t1, $t1, 4
  bnez $t1, 1b
  nop
  b quadro
  nop

# a0 = bloco de 64 bytes (RDRAM), a1 = onde pôr a resposta
pif:
  LI32 $t1, 0x1FFFFFFF
  and $t0, $a0, $t1
  LI32 $t2, SI_DRAM_ADDR
  sw $t0, 0($t2)
  LI32 $t2, SI_WR64B
  LI32 $t3, 0x1FC007C0
  sw $t3, 0($t2)
  LI32 $t2, SI_STATUS
1: lw $t4, 0($t2)
  andi $t4, 3
  bnez $t4, 1b
  nop
  and $t0, $a1, $t1
  LI32 $t2, SI_DRAM_ADDR
  sw $t0, 0($t2)
  LI32 $t2, SI_RD64B
  sw $t3, 0($t2)
  LI32 $t2, SI_STATUS
1: lw $t4, 0($t2)
  andi $t4, 3
  bnez $t4, 1b
  nop
  jr $ra
  nop

.balign 8
bloco_controle:
  .byte 0x01, 0x04, 0x01, 0xFF, 0xFF, 0xFF, 0xFF
  .byte 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF
  .byte 0xFE
  .fill 49, 1, 0
  .byte 0x01
.balign 8
bloco_ee_info:
  .byte 0, 0, 0, 0
  .byte 0x01, 0x03, 0x00, 0xFF, 0xFF, 0xFF
  .byte 0xFE
  .fill 52, 1, 0
  .byte 0x01
.balign 8
bloco_ee_grava:
  .byte 0, 0, 0, 0
  .byte 0x0A, 0x01, 0x05, 0x02
  .ascii "INDIGO64"
  .byte 0xFF
  .byte 0xFE
  .fill 45, 1, 0
  .byte 0x01
.balign 8
bloco_ee_le:
  .byte 0, 0, 0, 0
  .byte 0x02, 0x08, 0x04, 0x02
  .fill 8, 1, 0xFF
  .byte 0xFE
  .fill 46, 1, 0
  .byte 0x01
.balign 8
bloco_pak_grava:
  .byte 0x23, 0x01, 0x03, 0x00, 0x60
  .byte 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
  .byte 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
  .byte 0xFF
  .byte 0xFE
  .fill 24, 1, 0
  .byte 0x01
.balign 8
bloco_pak_le:
  .byte 0x03, 0x21, 0x02, 0x00, 0x60
  .fill 33, 1, 0xFF
  .byte 0xFE
  .fill 24, 1, 0
  .byte 0x01
.balign 8
dados_pi:
  .ascii "CARTUCHO PI DMA!"
.balign 8
  .word 0, 0
