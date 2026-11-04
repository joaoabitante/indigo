# Teste da CPU VR4300 escrito para o Índigo: inteiros de 32/64 bits, loads/stores desalinhados, desvios
# "likely", exceções (syscall, break, overflow, endereço, trap, TLB), timer Count/Compare, interrupção do RCP
# pelo MI, FPU (arredondamento, flags, exceção, modo FR=0). Resultados em RES_BASE + 8n; t7_n64.cpp confere.
.include "t7_n64.inc"

.macro LI64 reg, hi, lo
  lui \reg, ((\hi) >> 16) & 0xFFFF
  ori \reg, \reg, (\hi) & 0xFFFF
  dsll \reg, \reg, 16
  ori \reg, \reg, ((\lo) >> 16) & 0xFFFF
  dsll \reg, \reg, 16
  ori \reg, \reg, (\lo) & 0xFFFF
.endm

.text
.globl _start
_start:
  LI32 $s7, RES_BASE
  # copia os tratadores de exceção para 0x80000000 (TLB refill) e 0x80000180 (geral)
  LI32 $t0, exc_refill
  LI32 $t1, 0x80000000
  li $t2, 32
1: lw $t3, 0($t0)
  sw $t3, 0($t1)
  addiu $t0, 4
  addiu $t2, -1
  bnez $t2, 1b
  addiu $t1, 4
  LI32 $t0, exc_gen
  LI32 $t1, 0x80000180
  li $t2, 48
1: lw $t3, 0($t0)
  sw $t3, 0($t1)
  addiu $t0, 4
  addiu $t2, -1
  bnez $t2, 1b
  addiu $t1, 4
  LI32 $s1, EXC_LOG
  sw $zero, 0($s1)
  sw $zero, 4($s1)
  sw $zero, 0x10($s1)
  sw $zero, 0x14($s1)

  # ---------------------------------------------------------------- inteiros
  LI32 $t0, 0x7FFFFFFF
  addiu $t1, $t0, 1
  RES 0, $t1
  li $t0, 1
  dsll32 $t0, $t0, 0
  ori $t0, 5
  li $t1, 3
  addu $t2, $t0, $t1
  RES 1, $t2
  LI64 $t0, 0x12345678, 0x9ABCDEF0
  LI64 $t1, 0x0FEDCBA9, 0x87654321
  daddu $t2, $t0, $t1
  RES 2, $t2
  li $t0, 5
  li $t1, 7
  dsubu $t2, $t0, $t1
  RES 3, $t2
  li $t0, -1
  li $t1, 1
  slt $t2, $t0, $t1
  sltu $t3, $t0, $t1
  sll $t2, $t2, 1
  or $t2, $t2, $t3
  RES 4, $t2
  LI32 $t0, 0x80000001
  sll $t1, $t0, 1
  RES 5, $t1
  LI32 $t0, 0x80000000
  srl $t1, $t0, 4
  RES 6, $t1
  li $t0, -16
  sra $t1, $t0, 2
  RES 7, $t1
  li $t0, 1
  dsll32 $t1, $t0, 4
  RES 8, $t1
  LI64 $t0, 0x80000000, 0x00000000
  dsra32 $t1, $t0, 3
  RES 9, $t1
  dsrl $t1, $t0, 4
  RES 10, $t1
  li $t0, -3
  LI32 $t1, 0x10000000
  mult $t0, $t1
  mflo $t2
  mfhi $t3
  RES 11, $t2
  RES 12, $t3
  li $t0, -1
  multu $t0, $t0
  mflo $t2
  mfhi $t3
  RES 13, $t2
  RES 14, $t3
  li $t0, -7
  li $t1, 2
  div $zero, $t0, $t1
  mflo $t2
  mfhi $t3
  RES 15, $t2
  RES 16, $t3
  li $t0, 5
  divu $zero, $t0, $zero
  mflo $t2
  mfhi $t3
  RES 17, $t2
  RES 18, $t3
  LI64 $t0, 0x00000001, 0x00000000
  LI64 $t1, 0x00000001, 0x00000003
  dmult $t0, $t1
  mflo $t2
  mfhi $t3
  RES 19, $t2
  RES 20, $t3
  li $t0, -1
  li $t1, 2
  dmultu $t0, $t1
  mflo $t2
  mfhi $t3
  RES 21, $t2
  RES 22, $t3
  li $t0, -100
  li $t1, 7
  ddiv $zero, $t0, $t1
  mflo $t2
  mfhi $t3
  RES 23, $t2
  RES 24, $t3
  li $t0, -1
  li $t1, 16
  ddivu $zero, $t0, $t1
  mflo $t2
  mfhi $t3
  RES 25, $t2
  RES 26, $t3

  # ---------------------------------------------------------------- loads
  LI32 $s0, 0xA0280000
  LI32 $t0, 0x89ABCDEF
  sw $t0, 0($s0)
  lb $t1, 0($s0)
  RES 27, $t1
  lbu $t1, 1($s0)
  RES 28, $t1
  lh $t1, 2($s0)
  RES 29, $t1
  lhu $t1, 0($s0)
  RES 30, $t1
  lw $t1, 0($s0)
  RES 31, $t1
  lwu $t1, 0($s0)
  RES 32, $t1
  LI32 $t0, 0x00112233
  sw $t0, 0x10($s0)
  LI32 $t0, 0x44556677
  sw $t0, 0x14($s0)
  LI32 $t0, 0x8899AABB
  sw $t0, 0x18($s0)
  LI32 $t0, 0xCCDDEEFF
  sw $t0, 0x1C($s0)
  li $t1, 0
  lwl $t1, 0x11($s0)
  lwr $t1, 0x14($s0)
  RES 33, $t1
  ldl $t1, 0x13($s0)
  ldr $t1, 0x1A($s0)
  RES 34, $t1
  sd $zero, 0x20($s0)
  LI32 $t0, 0xAABBCCDD
  swl $t0, 0x22($s0)
  swr $t0, 0x25($s0)
  ld $t1, 0x20($s0)
  RES 35, $t1
  sd $zero, 0x30($s0)
  sd $zero, 0x38($s0)
  LI64 $t0, 0x01020304, 0x05060708
  sdl $t0, 0x31($s0)
  sdr $t0, 0x38($s0)
  ld $t1, 0x30($s0)
  RES 36, $t1
  ld $t1, 0x38($s0)
  RES 37, $t1

  # ---------------------------------------------------------------- desvios
  li $t0, 0
  beql $zero, $s7, 1f
  addiu $t0, 1
1: beql $zero, $zero, 2f
  addiu $t0, 2
  addiu $t0, 100
2: RES 38, $t0
  li $t0, -1
  bltzal $t0, 3f
  nop
volta1:
  b 4f
  nop
3: LI32 $t1, volta1
  xor $t1, $t1, $ra
  sltiu $t1, $t1, 1
  RES 39, $t1
  jr $ra
  nop
4: LI32 $t2, 5f
  jalr $t2
  nop
volta2:
  b 6f
  nop
5: LI32 $t1, volta2
  xor $t1, $t1, $ra
  sltiu $t1, $t1, 1
  RES 40, $t1
  jr $ra
  nop
6:
  # ---------------------------------------------------------------- exceções
  LI32 $s1, EXC_LOG
  syscall
  lw $t0, 0($s1)
  RES 41, $t0
  break
  lw $t0, 0($s1)
  RES 42, $t0
  LI32 $t0, 0x7FFFFFFF
  li $t1, 1
  li $t2, 0x55
  add $t2, $t0, $t1
  RES 43, $t2
  lw $t0, 0($s1)
  RES 44, $t0
  LI32 $t0, 0x80100001
  lw $t1, 0($t0)
  lw $t0, 0($s1)
  RES 45, $t0
  lw $t0, 8($s1)
  RES 46, $t0
  teq $zero, $zero
  lw $t0, 0($s1)
  RES 47, $t0
  # TLB: 0x00010000 (par de páginas de 4 KB) -> física 0x400000/0x401000, mapeado pelo tratador de refill
  mtc0 $zero, $10
  LI32 $t0, 0xA0400010
  LI32 $t1, 0x5A5A1234
  sw $t1, 0($t0)
  LI32 $t0, 0xA0401010
  li $t1, 0x77
  sw $t1, 0($t0)
  LI32 $t0, 0x00010000
  lw $t1, 0x10($t0)
  RES 48, $t1
  lw $t1, 0x10($s1)
  RES 49, $t1
  lw $t1, 0x1010($t0)
  RES 50, $t1
  lw $t1, 0x10($s1)
  RES 51, $t1
  mtc0 $t0, $10
  nop
  tlbp
  nop
  mfc0 $t1, $0
  srl $t1, $t1, 31
  xori $t1, 1
  LI32 $t0, 0x00200000
  mtc0 $t0, $10
  nop
  tlbp
  nop
  mfc0 $t2, $0
  srl $t2, $t2, 31
  addu $t1, $t1, $t2
  RES 52, $t1
  # timer: Compare = Count + 2000, IE + IM7
  mfc0 $t0, $9
  addiu $t0, 2000
  mtc0 $t0, $11
  mfc0 $t0, $12
  ori $t0, 0x8001
  mtc0 $t0, $12
  LI32 $t2, 100000
1: lw $t1, 0x14($s1)
  bnez $t1, 2f
  addiu $t2, -1
  bnez $t2, 1b
  nop
2: lw $t1, 0x14($s1)
  RES 53, $t1
  # interrupção do RCP: máscara do SP no MI, IM2, e o SP pede interrupção
  mfc0 $t0, $12
  li $t1, ~0x8000
  and $t0, $t0, $t1
  ori $t0, 0x0401
  mtc0 $t0, $12
  WREG MI_MASK, 0x2
  WREG SP_STATUS, 0x10
  nop
  nop
  lw $t1, 0x14($s1)
  RES 54, $t1
  LI32 $t0, MI_INTR
  lw $t1, 0($t0)
  andi $t1, 1
  RES 55, $t1
  mfc0 $t0, $12
  li $t1, ~1
  and $t0, $t0, $t1
  mtc0 $t0, $12

  # ---------------------------------------------------------------- FPU
  LI32 $t0, 0x3FC00000
  mtc1 $t0, $f0
  LI32 $t0, 0x40100000
  mtc1 $t0, $f10
  add.s $f2, $f0, $f10
  mfc1 $t1, $f2
  RES 56, $t1
  li $t0, 1
  mtc1 $t0, $f4
  cvt.d.w $f4, $f4
  li $t0, 3
  mtc1 $t0, $f6
  cvt.d.w $f6, $f6
  div.d $f8, $f4, $f6
  dmfc1 $t1, $f8
  RES 57, $t1
  LI32 $t0, 0x40000000
  mtc1 $t0, $f0
  sqrt.s $f2, $f0
  mfc1 $t1, $f2
  RES 58, $t1
  LI64 $t0, 0x3FB99999, 0x9999999A
  dmtc1 $t0, $f4
  cvt.s.d $f2, $f4
  mfc1 $t1, $f2
  RES 59, $t1
  # 2.5 e -2.5 com os 4 modos de arredondamento
  LI32 $t0, 0x40200000
  mtc1 $t0, $f0
  ctc1 $zero, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 60, $t1
  li $t0, 2
  ctc1 $t0, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 61, $t1
  li $t0, 3
  ctc1 $t0, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 62, $t1
  li $t0, 1
  ctc1 $t0, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 63, $t1
  LI32 $t0, 0xC0200000
  mtc1 $t0, $f0
  ctc1 $zero, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 64, $t1
  li $t0, 2
  ctc1 $t0, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 65, $t1
  li $t0, 3
  ctc1 $t0, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 66, $t1
  li $t0, 1
  ctc1 $t0, $31
  cvt.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 67, $t1
  ctc1 $zero, $31
  LI64 $t0, 0x400C0000, 0x00000000
  dmtc1 $t0, $f4
  round.w.d $f2, $f4
  mfc1 $t1, $f2
  RES 68, $t1
  LI64 $t0, 0xBFFB3333, 0x33333333
  dmtc1 $t0, $f4
  trunc.w.d $f2, $f4
  mfc1 $t1, $f2
  RES 69, $t1
  LI64 $t0, 0xBFF80000, 0x00000000
  dmtc1 $t0, $f4
  floor.l.d $f6, $f4
  dmfc1 $t1, $f6
  RES 70, $t1
  LI32 $t0, 0x3F8CCCCD
  mtc1 $t0, $f0
  ceil.w.s $f2, $f0
  mfc1 $t1, $f2
  RES 71, $t1
  # flags: 1/0 e 1/3
  ctc1 $zero, $31
  LI32 $t0, 0x3F800000
  mtc1 $t0, $f0
  mtc1 $zero, $f10
  div.s $f2, $f0, $f10
  mfc1 $t1, $f2
  RES 72, $t1
  cfc1 $t1, $31
  RES 73, $t1
  ctc1 $zero, $31
  LI32 $t0, 0x40400000
  mtc1 $t0, $f10
  div.s $f2, $f0, $f10
  cfc1 $t1, $31
  RES 74, $t1
  # comparações
  li $t0, 1
  mtc1 $t0, $f4
  cvt.d.w $f4, $f4
  li $t0, 2
  mtc1 $t0, $f6
  cvt.d.w $f6, $f6
  li $t1, 0
  c.lt.d $f4, $f6
  bc1t 1f
  nop
  b 2f
  nop
1: li $t1, 1
2: RES 75, $t1
  c.eq.s $f0, $f10
  cfc1 $t1, $31
  srl $t1, $t1, 23
  andi $t1, 1
  RES 76, $t1
  # exceção do FPU: divisão por zero com a exceção Z ligada
  li $t0, 0x400
  ctc1 $t0, $31
  LI32 $t0, 0x12345678
  mtc1 $t0, $f2
  mtc1 $zero, $f10
  div.s $f2, $f0, $f10
  lw $t1, 0($s1)
  RES 77, $t1
  mfc1 $t1, $f2
  RES 78, $t1
  ctc1 $zero, $31
  # FR = 0: o registro ímpar é a metade alta do par
  mfc0 $t0, $12
  LI32 $t1, ~0x04000000
  and $t2, $t0, $t1
  mtc0 $t2, $12
  nop
  LI32 $t1, 0x11111111
  mtc1 $t1, $f2
  LI32 $t1, 0x22222222
  mtc1 $t1, $f3
  dmfc1 $t1, $f2
  RES 79, $t1
  mtc0 $t0, $12
  nop
  li $t0, -3
  mtc1 $t0, $f0
  cvt.d.w $f2, $f0
  dmfc1 $t1, $f2
  RES 80, $t1
  LI64 $t0, 0x3FF80000, 0
  dmtc1 $t0, $f4
  LI64 $t0, 0xC0000000, 0
  dmtc1 $t0, $f6
  mul.d $f8, $f4, $f6
  dmfc1 $t1, $f8
  RES 81, $t1
  LI32 $t0, 0x3F800000
  mtc1 $t0, $f0
  neg.s $f2, $f0
  mfc1 $t1, $f2
  RES 82, $t1
  LI64 $t0, 0x42700000, 0x00000080
  dmtc1 $t0, $f4
  cvt.l.d $f6, $f4
  dmfc1 $t1, $f6
  RES 83, $t1
  # LL/SC, EPC de 64 bits, lógicas
  ll $t0, 0($s0)
  sc $t1, 0($s0)
  RES 84, $t1
  LI64 $t0, 0x12345678, 0x9ABCDEF0
  dmtc0 $t0, $14
  dmfc0 $t1, $14
  RES 85, $t1
  li $t0, 0x0F0F
  li $t1, 0x00FF
  nor $t2, $t0, $t1
  RES 86, $t2
  sltiu $t0, $zero, -1
  RES 87, $t0
  li $t0, 1
  li $t1, 63
  dsllv $t0, $t0, $t1
  dsrav $t0, $t0, $t1
  RES 88, $t0
  li $t0, 3
  dsll32 $t0, $t0, 0
  dsrl $t0, $t0, 1
  sra $t1, $t0, 4
  RES 89, $t1
  li $t0, 0xABC
  mthi $t0
  mfhi $t1
  RES 90, $t1
  FIM_TESTE

# ---------------------------------------------------------------- tratadores (copiados para os vetores)
exc_refill:
  LI32 $k1, EXC_LOG
  lw $k0, 0x10($k1)
  addiu $k0, 1
  sw $k0, 0x10($k1)
  LI32 $k0, (0x400 << 6) | 0x1F
  mtc0 $k0, $2
  LI32 $k0, (0x401 << 6) | 0x1F
  mtc0 $k0, $3
  mtc0 $zero, $5
  nop
  tlbwr
  nop
  eret
  nop

exc_gen:
  LI32 $k1, EXC_LOG
  mfc0 $k0, $13
  sw $k0, 0xC($k1)
  andi $k0, $k0, 0x7C
  srl $k0, $k0, 2
  sw $k0, 0($k1)
  lw $k0, 4($k1)
  addiu $k0, 1
  sw $k0, 4($k1)
  mfc0 $k0, $8
  sw $k0, 8($k1)
  lw $k0, 0($k1)
  bnez $k0, 1f
  nop
  lw $k0, 0x14($k1)
  addiu $k0, 1
  sw $k0, 0x14($k1)
  mfc0 $k0, $13
  andi $k0, 0x8000
  beqz $k0, 2f
  nop
  mtc0 $zero, $11
  nop
  eret
2: LI32 $k0, SP_STATUS
  li $k1, 8
  sw $k1, 0($k0)
  eret
1: mfc0 $k0, $14
  addiu $k0, 4
  mtc0 $k0, $14
  nop
  eret
  nop
