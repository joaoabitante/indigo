# Teste do RSP escrito para o Índigo: a CPU carrega o microcódigo (t7_n64_rspu.s) na IMEM e os dados na DMEM por
# DMA do SP, testa o semáforo, solta o RSP e espera o BREAK; os resultados voltam para a RDRAM por DMA.
.include "t7_n64.inc"
.text
.globl _start
_start:
  LI32 $s7, RES_BASE
  # microcódigo -> IMEM
  WREG SP_MEM_ADDR, 0x1000
  LI32 $t0, ucode
  LI32 $t1, 0x1FFFFFFF
  and $t0, $t0, $t1
  LI32 $t1, SP_DRAM_ADDR
  sw $t0, 0($t1)
  WREG SP_RD_LEN, 0x3FF
  # dados -> DMEM
  WREG SP_MEM_ADDR, 0
  LI32 $t0, dados
  LI32 $t1, 0x1FFFFFFF
  and $t0, $t0, $t1
  LI32 $t1, SP_DRAM_ADDR
  sw $t0, 0($t1)
  WREG SP_RD_LEN, 0x1F
  # semáforo
  LI32 $t0, SP_SEMA
  lw $t1, 0($t0)
  RES 0, $t1
  lw $t1, 0($t0)
  RES 1, $t1
  sw $zero, 0($t0)
  lw $t1, 0($t0)
  RES 2, $t1
  sw $zero, 0($t0)
  # roda o RSP: PC 0, tira o HALT, liga a interrupção no BREAK
  WREG SP_PC, 0
  WREG SP_STATUS, 0x101
  LI32 $t0, SP_STATUS
  LI32 $t2, 1000000
1: lw $t1, 0($t0)
  andi $t1, 3
  li $t3, 3
  beq $t1, $t3, 2f
  addiu $t2, -1
  bnez $t2, 1b
  nop
2: lw $t1, 0($t0)
  andi $t3, $t1, 3
  RES 3, $t3
  srl $t3, $t1, 7
  andi $t3, 1
  RES 5, $t3
  LI32 $t0, MI_INTR
  lw $t1, 0($t0)
  andi $t1, 1
  RES 4, $t1
  # resultados DMEM 0x100-0x1FF -> RDRAM 0x210000
  WREG SP_MEM_ADDR, 0x100
  WREG SP_DRAM_ADDR, 0x210000
  WREG SP_WR_LEN, 0xFF
  # o PC do RSP parou depois do BREAK
  LI32 $t0, SP_PC
  lw $t1, 0($t0)
  RES 6, $t1
  FIM_TESTE

.balign 8
dados:
  .half 0x4000, 0x8000, 0x7FFF, 0xC000, 0x0001, 0xFFFF, 0x1234, 0x0000
  .half 0x4000, 0x8000, 0x7FFF, 0x4000, 0x7FFF, 0x0002, 0x0100, 0x5555
.balign 8
ucode:
  .incbin "rspu.bin"
.balign 8
  .word 0, 0
