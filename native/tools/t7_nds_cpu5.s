@ Teste das instruções do ARMv5TE no ARM9 do Índigo (escrito do zero). Resultados em 0x02300000; o
@ indigo-t7_nds confere com os valores calculados à mão no próprio teste.
.arm
.global inicio
inicio:
  ldr sp, =0x02380000
  ldr r12, =0x02300000
  @ CLZ
  ldr r0, =0x00F00000
  clz r1, r0
  str r1, [r12, #0]
  mov r0, #0
  clz r1, r0
  str r1, [r12, #4]
  @ QADD satura e liga o Q
  msr cpsr_f, #0
  ldr r0, =0x7FFFFFFF
  mov r1, #1
  qadd r2, r0, r1
  str r2, [r12, #8]
  mrs r3, cpsr
  and r3, r3, #0x08000000
  str r3, [r12, #12]
  @ QSUB
  ldr r0, =0x80000000
  qsub r2, r0, r1
  str r2, [r12, #16]
  @ QDADD: 1 + sat(2 * 0x40000000)
  mov r0, #1
  ldr r1, =0x40000000
  qdadd r2, r0, r1
  str r2, [r12, #20]
  @ SMLABB: -2 * 7 + 100
  ldr r0, =0x0003FFFE
  ldr r1, =0x00050007
  mov r2, #100
  smlabb r3, r0, r1, r2
  str r3, [r12, #24]
  @ SMULTT: -2 * 3
  ldr r0, =0xFFFE0000
  ldr r1, =0x00030000
  smultt r3, r0, r1
  str r3, [r12, #28]
  @ SMULWB: (0x20000 * 3) >> 16
  ldr r0, =0x00020000
  mov r1, #3
  smulwb r3, r0, r1
  str r3, [r12, #32]
  @ SMLALBB: 10 + (-1 * 20)
  mov r4, #10
  mov r5, #0
  ldr r0, =0x0000FFFF
  mov r1, #20
  smlalbb r4, r5, r0, r1
  str r4, [r12, #36]
  str r5, [r12, #40]
  @ STRD / LDRD
  ldr r2, =0x11223344
  ldr r3, =0x55667788
  add r6, r12, #0x100
  strd r2, r3, [r6]
  ldrd r8, r9, [r6]
  str r8, [r12, #44]
  str r9, [r12, #48]
  @ BLX para Thumb (endereço)
  mov r0, #0
  blx thumb1
  str r0, [r12, #52]
  @ BLX registrador
  ldr r1, =thumb2 + 1
  blx r1
  str r0, [r12, #56]
  @ LDR PC com o bit 0 ligado vai para Thumb
  ldr r1, =thumb3 + 1
  str r1, [r12, #0x104]
  adr lr, depois3
  ldr pc, [r12, #0x104]
depois3:
  str r0, [r12, #60]
  @ POP {pc} no Thumb volta para ARM (ARMv5)
  blx thumb4
  str r0, [r12, #64]
  @ MRC: identificação da CPU
  mrc p15, 0, r0, c0, c0, 0
  str r0, [r12, #68]
  @ LDM com a base na lista (não é a última): ARMv5 escreve a base de volta
  add r0, r12, #0x110
  ldr r1, =0xAAAA0000
  str r1, [r0]
  ldr r1, =0xBBBB0000
  str r1, [r0, #4]
  .word 0xE8B00003          @ ldmia r0!, {r0, r1}
  sub r0, r0, r12
  str r0, [r12, #72]
  @ LDM com a base por último: fica o valor carregado
  add r6, r12, #0x110
  .word 0xE8B60060          @ ldmia r6!, {r5, r6}
  str r6, [r12, #76]
  @ PLD não faz nada
  pld [r12]
  ldr r0, =0xC0DE5555
  str r0, [r12, #0xFC]
fim:
  b fim
.pool

.thumb
thumb1:
  movs r0, #0x55
  bx lr
.align 2
thumb2:
  movs r0, #0x66
  bx lr
.align 2
thumb3:
  movs r0, #0x77
  bx lr
.align 2
thumb4:
  push {lr}
  movs r0, #0x99
  pop {pc}
