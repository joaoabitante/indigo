# Microcódigo de teste do RSP (escrito para o Índigo): operações vetoriais com resultados conhecidos.
# Entrada: DMEM 0x00 (vetor A) e 0x10 (vetor B). Saída: DMEM 0x100 + 16k e flags em 0x1E0-0x1FC. Termina com BREAK.
.include "t7_n64.inc"
.text
  lqv 0, 0, 0, 0
  lqv 1, 0, 1, 0
  ctc2r 0, 0
  vmulf 2, 0, 1
  sqv 2, 0, 0x10, 0
  vadd 3, 0, 1
  sqv 3, 0, 0x11, 0
  vsub 4, 0, 1
  sqv 4, 0, 0x12, 0
  vaddc 5, 0, 1
  sqv 5, 0, 0x13, 0
  cfc2r 8, 0
  sw $8, 0x1F0($0)
  vsubc 6, 0, 1
  sqv 6, 0, 0x14, 0
  cfc2r 8, 0
  sw $8, 0x1F4($0)
  ctc2r 0, 0
  vlt 7, 0, 1
  sqv 7, 0, 0x15, 0
  cfc2r 8, 1
  sw $8, 0x1F8($0)
  vmudh 8, 0, 1, 8
  sqv 8, 0, 0x16, 0
  vsar 9, 0, 0, 9
  sqv 9, 0, 0x17, 0
  vsar 10, 0, 0, 8
  sqv 10, 0, 0x18, 0
  vrcp 11, 0, 1, 8
  vrcph 11, 1, 1, 8
  sqv 11, 0, 0x19, 0
  lui $8, 0x1234
  ori $8, $8, 0xABCD
  mtc2e 8, 12, 2
  sqv 12, 0, 0x1A, 0
  mfc2e 9, 12, 2
  sw $9, 0x1FC($0)
  ctc2r 0, 0
  ctc2r 0, 1
  ctc2r 0, 2
  vch 15, 0, 1
  sqv 15, 0, 0x1B, 0
  cfc2r 8, 0
  sw $8, 0x1E0($0)
  cfc2r 8, 1
  sw $8, 0x1E4($0)
  # laço com desvio e atraso: soma 1..10 em $10
  li $10, 0
  li $11, 10
1: addu $10, $10, $11
  addiu $11, $11, -1
  bgtz $11, 1b
  nop
  sw $10, 0x1E8($0)
  # sinaliza pelo SIG0 do SP_STATUS (escrita pelo COP0 do RSP: registro 4)
  li $8, 0x400
  mtc0 $8, $4
  break
  nop
