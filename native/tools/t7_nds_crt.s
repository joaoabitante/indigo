@ Partida dos programas de teste do DS (ARM9 com -DARM9, ARM7 sem): pilhas, .bss zerado e main().
.section .crt0, "ax"
.arm
.global _start
_start:
  mov r0, #0x04000000
  mov r1, #0
  str r1, [r0, #0x208]        @ IME = 0
#ifdef ARM9
  ldr r0, =0x0B00000A         @ DTCM em 0x0B000000, 16 KB
  mcr p15, 0, r0, c9, c1, 0
  mrc p15, 0, r0, c1, c0, 0
  orr r0, r0, #0x50000        @ DTCM e ITCM ligadas
  mcr p15, 0, r0, c1, c0, 0
  msr cpsr_c, #0xD2
  ldr sp, =0x0B003E00
  msr cpsr_c, #0xD3
  ldr sp, =0x0B003FC0
  msr cpsr_c, #0x1F
  ldr sp, =0x0B003C00
#else
  msr cpsr_c, #0xD2
  ldr sp, =0x0380FF80
  msr cpsr_c, #0xD3
  ldr sp, =0x0380FFC0
  msr cpsr_c, #0x1F
  ldr sp, =0x0380FD80
#endif
  ldr r0, =__bss_start
  ldr r1, =__bss_end
  mov r2, #0
1:
  cmp r0, r1
  strlo r2, [r0], #4
  blo 1b
  ldr r0, =main
  mov lr, pc
  bx r0
2:
  b 2b
.pool

@ chamadas da BIOS (SWI) usadas pelos testes (ARM; r0-r3 de entrada, r0 de saída)
.global swi_div
.type swi_div, %function
swi_div:
  swi 0x090000
  bx lr
.global swi_divrem
.type swi_divrem, %function
swi_divrem:
  swi 0x090000
  mov r0, r1
  bx lr
.global swi_sqrt
.type swi_sqrt, %function
swi_sqrt:
  swi 0x0D0000
  bx lr
.global swi_crc16
.type swi_crc16, %function
swi_crc16:
  swi 0x0E0000
  bx lr
.global swi_cpuset
.type swi_cpuset, %function
swi_cpuset:
  swi 0x0B0000
  bx lr
.global swi_cpufastset
.type swi_cpufastset, %function
swi_cpufastset:
  swi 0x0C0000
  bx lr
.global swi_lz77
.type swi_lz77, %function
swi_lz77:
  swi 0x110000
  bx lr
.global swi_lz77cb
.type swi_lz77cb, %function
swi_lz77cb:
  swi 0x120000
  bx lr
.global swi_huffcb
.type swi_huffcb, %function
swi_huffcb:
  swi 0x130000
  bx lr
.global swi_rle
.type swi_rle, %function
swi_rle:
  swi 0x140000
  bx lr
.global swi_vblank
.type swi_vblank, %function
swi_vblank:
  swi 0x050000
  bx lr
.global swi_intrwait
.type swi_intrwait, %function
swi_intrwait:
  swi 0x040000
  bx lr
.global swi_halt
.type swi_halt, %function
swi_halt:
  swi 0x060000
  bx lr
.global swi_voltable
.type swi_voltable, %function
swi_voltable:
  swi 0x1C0000
  bx lr
.global swi_bitunpack
.type swi_bitunpack, %function
swi_bitunpack:
  swi 0x100000
  bx lr
