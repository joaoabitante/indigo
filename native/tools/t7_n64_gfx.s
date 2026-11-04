# Teste do RDP e do VI escrito para o Índigo: limpa a imagem e o z com retângulos de preenchimento, desenha um
# triângulo com shade, dois triângulos com z (o mais perto fica na frente), um retângulo com textura (8x8 RGBA16
# carregada com LOAD_TILE, ampliada 4x) e o mesmo em modo cópia; depois o RSP manda mais uma lista pelo XBUS.
# O VI mostra a imagem de 320x240 em 16 bits. Depois do primeiro quadro a lista é repetida a cada quadro (para
# medir a velocidade).
.include "t7_n64.inc"
.equ FB, 0x00100000
.equ ZB, 0x00140000

.text
.globl _start
_start:
  LI32 $s7, RES_BASE
  WREG MI_MASK, 0x0AAA
  # textura 8x8 xadrez (amarelo/magenta) em RDRAM 0x180000
  LI32 $t0, 0xA0180000
  li $t1, 0
1: andi $t2, $t1, 7
  srl $t3, $t1, 3
  xor $t2, $t2, $t3
  andi $t2, 1
  li $t4, 0xFFC1
  beqz $t2, 2f
  nop
  li $t4, 0xF83F
2: sh $t4, 0($t0)
  addiu $t0, 2
  addiu $t1, 1
  li $t2, 64
  bne $t1, $t2, 1b
  nop
  # VI: 320x240, 16 bits
  WREG VI_ORIGIN, FB
  WREG VI_WIDTH, 320
  WREG VI_INTR, 2
  WREG VI_HSTART, 0x006C02EC
  WREG VI_VSTART, 0x002501FF
  WREG VI_XSCALE, 0x200
  WREG VI_YSCALE, 0x400
  WREG 0xA4400018, 525
  WREG VI_CTRL, 0x3302
  li $s5, 0
quadro:
  # lista principal pela RDRAM
  WREG DPC_STATUS, 1
  LI32 $t0, lista
  LI32 $t1, 0x1FFFFFFF
  and $t0, $t0, $t1
  LI32 $t2, DPC_START
  sw $t0, 0($t2)
  LI32 $t0, lista_fim
  and $t0, $t0, $t1
  LI32 $t2, DPC_END
  sw $t0, 0($t2)
  # espera a interrupção do DP (full sync)
  LI32 $t0, MI_INTR
  LI32 $t2, 1000000
1: lw $t1, 0($t0)
  andi $t1, 0x20
  bnez $t1, 2f
  addiu $t2, -1
  bnez $t2, 1b
  nop
2: RES 0, $t1
  WREG MI_MODE, 0x800
  # segunda lista pelo RSP (XBUS): microcódigo na IMEM, lista na DMEM 0x800, fim em 0x7FC
  WREG SP_MEM_ADDR, 0x1000
  LI32 $t0, ucode
  LI32 $t1, 0x1FFFFFFF
  and $t0, $t0, $t1
  LI32 $t2, SP_DRAM_ADDR
  sw $t0, 0($t2)
  WREG SP_RD_LEN, 0xFF
  WREG SP_MEM_ADDR, 0x800
  LI32 $t0, lista2
  LI32 $t1, 0x1FFFFFFF
  and $t0, $t0, $t1
  LI32 $t2, SP_DRAM_ADDR
  sw $t0, 0($t2)
  WREG SP_RD_LEN, 0xFF
  LI32 $t0, 0xA40007FC
  LI32 $t1, 0x800 + (lista2_fim - lista2)
  sw $t1, 0($t0)
  WREG SP_PC, 0
  WREG SP_STATUS, 0x1
  LI32 $t0, MI_INTR
  LI32 $t2, 1000000
1: lw $t1, 0($t0)
  andi $t1, 0x20
  bnez $t1, 2f
  addiu $t2, -1
  bnez $t2, 1b
  nop
2: RES 1, $t1
  WREG MI_MODE, 0x800
  LI32 $t0, SP_STATUS
  lw $t1, 0($t0)
  andi $t1, 3
  RES 2, $t1
  addiu $s5, 1
  RES 3, $s5
  LI32 $t0, FIM
  LI32 $t1, 0x600DF00D
  sw $t1, 0($t0)
  # espera o próximo quadro (linha 2 do VI) e repete
  LI32 $t0, VI_CURRENT
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

.balign 8
lista:
  RDP 0x2D000000, 0x005003C0                 # scissor 0,0 - 320,240
  RDP 0x2F300000, 0x00000000                 # modo preenchimento
  RDP 0x3F10013F, ZB                         # imagem de cor = z (16 bits, 320)
  RDP 0x37000000, 0xFFFCFFFC                 # z máximo
  RDP 0x364FC3BC, 0x00000000                 # preenche 0,0 - 319,239
  RDP 0x27000000, 0                          # pipe sync
  RDP 0x3F10013F, FB                         # imagem de cor
  RDP 0x3E000000, ZB                         # imagem de z
  RDP 0x37000000, 0x00210021                 # azul escuro
  RDP 0x364FC3BC, 0x00000000
  RDP 0x27000000, 0
  # triângulo vermelho com shade (1 ciclo, combinador = shade)
  RDP 0x2F000000, 0x00000000
  RDP 0x3CFFFFFF, 0xFFFE793C
  RDP 0x0C800230, 0x023000A0
  RDP 0x00A00000, 0x00000000
  RDP 0x003C0000, 0x00000000
  RDP 0x003C0000, 0x00010000
  RDP 0x00FF0000, 0x000000FF
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  # dois triângulos com z: verde perto (z 0x1000), azul longe (z 0x4000), o azul desenhado depois
  RDP 0x2F000000, 0x00000030
  RDP 0x0D800230, 0x023000A0
  RDP 0x012C0000, 0x00000000
  RDP 0x00C80000, 0x00000000
  RDP 0x00C80000, 0x00010000
  RDP 0x000000FF, 0x000000FF
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0x10000000, 0x00000000
  RDP 0x00000000, 0x00000000
  RDP 0x0D800230, 0x023000A0
  RDP 0x01400000, 0x00000000
  RDP 0x00DC0000, 0x00000000
  RDP 0x00DC0000, 0x00010000
  RDP 0x00000000, 0x00FF00FF
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0, 0
  RDP 0x40000000, 0x00000000
  RDP 0x00000000, 0x00000000
  # textura: imagem, tile de carga 7, LOAD_TILE, tile 0 e tamanho
  RDP 0x27000000, 0
  RDP 0x3D100007, 0x00180000
  RDP 0x35100400, 0x07000000
  RDP 0x28000000, 0                          # load sync
  RDP 0x34000000, 0x0701C01C
  RDP 0x28000000, 0
  RDP 0x35100400, 0x00000000
  RDP 0x32000000, 0x0001C01C
  # retângulo com textura ampliada 4x (1 ciclo, combinador = texel 0)
  RDP 0x2F000000, 0x00000000
  RDP 0x3CFFFFFF, 0xFFFCF279
  RDP 0x240D0300, 0x00050280
  RDP 0x00000000, 0x01000100
  # o mesmo em modo cópia, 1:1, em (100,160)-(107,167)
  RDP 0x27000000, 0
  RDP 0x2F200000, 0x00000000
  RDP 0x241AC29C, 0x00190280
  RDP 0x00000000, 0x10000400
  RDP 0x29000000, 0                          # full sync
lista_fim:

# lista mandada pelo RSP (XBUS): retângulo amarelo de preenchimento em (280,200)-(299,219)
.balign 8
lista2:
  RDP 0x27000000, 0
  RDP 0x2F300000, 0x00000000
  RDP 0x3F10013F, FB
  RDP 0x37000000, 0xFFC1FFC1
  RDP 0x364AC36C, 0x00460320
  RDP 0x29000000, 0
lista2_fim:

.balign 8
ucode:
  .incbin "gfxu.bin"
.balign 8
  .word 0, 0
