# Definições comuns dos programas de teste do Nintendo 64 escritos para o Índigo (GNU as, MIPS big-endian).
# Endereços dos registros do RCP (kseg1, sem cache) conforme a documentação pública do hardware.
.set noreorder
.set noat

.equ SP_DMEM,      0xA4000000
.equ SP_IMEM,      0xA4001000
.equ SP_MEM_ADDR,  0xA4040000
.equ SP_DRAM_ADDR, 0xA4040004
.equ SP_RD_LEN,    0xA4040008
.equ SP_WR_LEN,    0xA404000C
.equ SP_STATUS,    0xA4040010
.equ SP_SEMA,      0xA404001C
.equ SP_PC,        0xA4080000
.equ DPC_START,    0xA4100000
.equ DPC_END,      0xA4100004
.equ DPC_CURRENT,  0xA4100008
.equ DPC_STATUS,   0xA410000C
.equ MI_MODE,      0xA4300000
.equ MI_INTR,      0xA4300008
.equ MI_MASK,      0xA430000C
.equ VI_CTRL,      0xA4400000
.equ VI_ORIGIN,    0xA4400004
.equ VI_WIDTH,     0xA4400008
.equ VI_INTR,      0xA440000C
.equ VI_CURRENT,   0xA4400010
.equ VI_HSTART,    0xA4400024
.equ VI_VSTART,    0xA4400028
.equ VI_XSCALE,    0xA4400030
.equ VI_YSCALE,    0xA4400034
.equ AI_DRAM_ADDR, 0xA4500000
.equ AI_LEN,       0xA4500004
.equ AI_CONTROL,   0xA4500008
.equ AI_STATUS,    0xA450000C
.equ AI_DACRATE,   0xA4500010
.equ AI_BITRATE,   0xA4500014
.equ PI_DRAM_ADDR, 0xA4600000
.equ PI_CART_ADDR, 0xA4600004
.equ PI_RD_LEN,    0xA4600008
.equ PI_WR_LEN,    0xA460000C
.equ PI_STATUS,    0xA4600010
.equ SI_DRAM_ADDR, 0xA4800000
.equ SI_RD64B,     0xA4800004
.equ SI_WR64B,     0xA4800010
.equ SI_STATUS,    0xA4800018
.equ PIF_RAM,      0xBFC007C0

# resultados: palavra dupla n em 0xA0200000 + 8n; registro do tratador de exceções em 0xA0300000
.equ RES_BASE,     0xA0200000
.equ EXC_LOG,      0xA0300000
.equ FIM,          0xA02FFFF0

.macro LI32 reg, val
  lui \reg, %hi(\val)
  addiu \reg, \reg, %lo(\val)
.endm

# grava um registro como resultado n (64 bits)
.macro RES n, reg
  sd \reg, (\n * 8)($s7)
.endm

# escreve um valor de 32 bits num registro do hardware (usa $at)
.macro WREG addr, val
  LI32 $at, \addr
  LI32 $k1, \val
  sw $k1, 0($at)
.endm

# marca de fim do teste
.macro FIM_TESTE
  LI32 $t0, FIM
  LI32 $t1, 0x600DF00D
  sw $t1, 0($t0)
9: b 9b
  nop
.endm

# ---------------------------------------------------------------- RSP: instruções vetoriais (codificação manual)
.macro VOP fn, vd, vs, vt, e
  .word 0x4A000000 | ((\e) << 21) | ((\vt) << 16) | ((\vs) << 11) | ((\vd) << 6) | (\fn)
.endm
.macro vmulf vd, vs, vt, e=0
  VOP 0x00, \vd, \vs, \vt, \e
.endm
.macro vmulu vd, vs, vt, e=0
  VOP 0x01, \vd, \vs, \vt, \e
.endm
.macro vmudl vd, vs, vt, e=0
  VOP 0x04, \vd, \vs, \vt, \e
.endm
.macro vmudm vd, vs, vt, e=0
  VOP 0x05, \vd, \vs, \vt, \e
.endm
.macro vmudn vd, vs, vt, e=0
  VOP 0x06, \vd, \vs, \vt, \e
.endm
.macro vmudh vd, vs, vt, e=0
  VOP 0x07, \vd, \vs, \vt, \e
.endm
.macro vmacf vd, vs, vt, e=0
  VOP 0x08, \vd, \vs, \vt, \e
.endm
.macro vmadl vd, vs, vt, e=0
  VOP 0x0C, \vd, \vs, \vt, \e
.endm
.macro vmadm vd, vs, vt, e=0
  VOP 0x0D, \vd, \vs, \vt, \e
.endm
.macro vmadn vd, vs, vt, e=0
  VOP 0x0E, \vd, \vs, \vt, \e
.endm
.macro vmadh vd, vs, vt, e=0
  VOP 0x0F, \vd, \vs, \vt, \e
.endm
.macro vadd vd, vs, vt, e=0
  VOP 0x10, \vd, \vs, \vt, \e
.endm
.macro vsub vd, vs, vt, e=0
  VOP 0x11, \vd, \vs, \vt, \e
.endm
.macro vabs vd, vs, vt, e=0
  VOP 0x13, \vd, \vs, \vt, \e
.endm
.macro vaddc vd, vs, vt, e=0
  VOP 0x14, \vd, \vs, \vt, \e
.endm
.macro vsubc vd, vs, vt, e=0
  VOP 0x15, \vd, \vs, \vt, \e
.endm
.macro vsar vd, vs, vt, e
  VOP 0x1D, \vd, \vs, \vt, \e
.endm
.macro vlt vd, vs, vt, e=0
  VOP 0x20, \vd, \vs, \vt, \e
.endm
.macro veq vd, vs, vt, e=0
  VOP 0x21, \vd, \vs, \vt, \e
.endm
.macro vne vd, vs, vt, e=0
  VOP 0x22, \vd, \vs, \vt, \e
.endm
.macro vge vd, vs, vt, e=0
  VOP 0x23, \vd, \vs, \vt, \e
.endm
.macro vcl vd, vs, vt, e=0
  VOP 0x24, \vd, \vs, \vt, \e
.endm
.macro vch vd, vs, vt, e=0
  VOP 0x25, \vd, \vs, \vt, \e
.endm
.macro vcr vd, vs, vt, e=0
  VOP 0x26, \vd, \vs, \vt, \e
.endm
.macro vmrg vd, vs, vt, e=0
  VOP 0x27, \vd, \vs, \vt, \e
.endm
.macro vand vd, vs, vt, e=0
  VOP 0x28, \vd, \vs, \vt, \e
.endm
.macro vor vd, vs, vt, e=0
  VOP 0x2A, \vd, \vs, \vt, \e
.endm
.macro vxor vd, vs, vt, e=0
  VOP 0x2C, \vd, \vs, \vt, \e
.endm
.macro vrcp vd, de, vt, e
  VOP 0x30, \vd, \de, \vt, \e
.endm
.macro vrcpl vd, de, vt, e
  VOP 0x31, \vd, \de, \vt, \e
.endm
.macro vrcph vd, de, vt, e
  VOP 0x32, \vd, \de, \vt, \e
.endm
.macro vmov vd, de, vt, e
  VOP 0x33, \vd, \de, \vt, \e
.endm
.macro vrsq vd, de, vt, e
  VOP 0x34, \vd, \de, \vt, \e
.endm
# loads/stores vetoriais: tipo (0 B, 1 S, 2 L, 3 D, 4 Q, 5 R, 6 P, 7 U, 8 H, 9 F, 10 W, 11 T)
.macro VLS op, kind, vt, e, off, base
  .word ((\op) << 26) | ((\base) << 21) | ((\vt) << 16) | ((\kind) << 11) | ((\e) << 7) | ((\off) & 0x7F)
.endm
.macro lqv vt, e, off, base
  VLS 0x32, 4, \vt, \e, \off, \base
.endm
.macro sqv vt, e, off, base
  VLS 0x3A, 4, \vt, \e, \off, \base
.endm
.macro lsv vt, e, off, base
  VLS 0x32, 1, \vt, \e, \off, \base
.endm
.macro ssv vt, e, off, base
  VLS 0x3A, 1, \vt, \e, \off, \base
.endm
.macro ldv vt, e, off, base
  VLS 0x32, 3, \vt, \e, \off, \base
.endm
.macro sdv vt, e, off, base
  VLS 0x3A, 3, \vt, \e, \off, \base
.endm
.macro lrv vt, e, off, base
  VLS 0x32, 5, \vt, \e, \off, \base
.endm
.macro lpv vt, e, off, base
  VLS 0x32, 6, \vt, \e, \off, \base
.endm
.macro spv vt, e, off, base
  VLS 0x3A, 6, \vt, \e, \off, \base
.endm
.macro luv vt, e, off, base
  VLS 0x32, 7, \vt, \e, \off, \base
.endm
.macro suv vt, e, off, base
  VLS 0x3A, 7, \vt, \e, \off, \base
.endm
# MFC2/MTC2/CFC2/CTC2: rt, vs, elemento
.macro mfc2e rt, vs, e
  .word 0x48000000 | ((\rt) << 16) | ((\vs) << 11) | ((\e) << 7)
.endm
.macro mtc2e rt, vs, e
  .word 0x48800000 | ((\rt) << 16) | ((\vs) << 11) | ((\e) << 7)
.endm
.macro cfc2r rt, c
  .word 0x48400000 | ((\rt) << 16) | ((\c) << 11)
.endm
.macro ctc2r rt, c
  .word 0x48C00000 | ((\rt) << 16) | ((\c) << 11)
.endm

# ---------------------------------------------------------------- RDP: comandos (palavras de 64 bits)
.macro RDP hi, lo
  .word \hi, \lo
.endm
