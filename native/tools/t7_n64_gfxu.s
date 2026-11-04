# Microcódigo de teste (escrito para o Índigo): manda ao RDP a lista de comandos que a CPU pôs na DMEM em 0x800,
# pelo XBUS (DPC_STATUS: liga o XBUS; DPC_START/DPC_END pelo COP0 do RSP, registros 8 e 9), e termina com BREAK.
.include "t7_n64.inc"
.text
  li $8, 2
  mtc0 $8, $11
  li $8, 0x800
  mtc0 $8, $8
  lw $9, 0x7FC($0)
  mtc0 $9, $9
  # espera o RDP consumir (DPC_CURRENT = DPC_END)
1: mfc0 $10, $10
  bne $10, $9, 1b
  nop
  li $8, 1
  mtc0 $8, $11
  break
  nop
