| Macros do RISC do Jaguar (GPU/DSP) para o binutils do m68k, usadas pelos cartuchos de teste do t7_jaguar.
| Escritas para o Índigo (GPL-3). Endereços de memória: s = registro do endereço, d/v = registro do valor.
        .macro  rop op, s, d
        .word   ((\op) << 10) + (((\s) & 31) << 5) + ((\d) & 31)
        .endm
        .macro  r_add s, d
        rop 0, \s, \d
        .endm
        .macro  r_addc s, d
        rop 1, \s, \d
        .endm
        .macro  r_addq n, d
        rop 2, \n, \d
        .endm
        .macro  r_addqt n, d
        rop 3, \n, \d
        .endm
        .macro  r_sub s, d
        rop 4, \s, \d
        .endm
        .macro  r_subc s, d
        rop 5, \s, \d
        .endm
        .macro  r_subq n, d
        rop 6, \n, \d
        .endm
        .macro  r_neg d
        rop 8, 0, \d
        .endm
        .macro  r_and s, d
        rop 9, \s, \d
        .endm
        .macro  r_or s, d
        rop 10, \s, \d
        .endm
        .macro  r_xor s, d
        rop 11, \s, \d
        .endm
        .macro  r_not d
        rop 12, 0, \d
        .endm
        .macro  r_bset n, d
        rop 14, \n, \d
        .endm
        .macro  r_bclr n, d
        rop 15, \n, \d
        .endm
        .macro  r_mult s, d
        rop 16, \s, \d
        .endm
        .macro  r_imult s, d
        rop 17, \s, \d
        .endm
        .macro  r_imultn s, d
        rop 18, \s, \d
        .endm
        .macro  r_resmac d
        rop 19, 0, \d
        .endm
        .macro  r_imacn s, d
        rop 20, \s, \d
        .endm
        .macro  r_div s, d
        rop 21, \s, \d
        .endm
        .macro  r_abs d
        rop 22, 0, \d
        .endm
        .macro  r_sh s, d
        rop 23, \s, \d
        .endm
        .macro  r_shlq n, d
        rop 24, 32 - (\n), \d
        .endm
        .macro  r_shrq n, d
        rop 25, \n, \d
        .endm
        .macro  r_sharq n, d
        rop 27, \n, \d
        .endm
        .macro  r_ror s, d
        rop 28, \s, \d
        .endm
        .macro  r_rorq n, d
        rop 29, \n, \d
        .endm
        .macro  r_cmp s, d
        rop 30, \s, \d
        .endm
        .macro  r_sat8 d
        rop 32, 0, \d
        .endm
        .macro  r_move s, d
        rop 34, \s, \d
        .endm
        .macro  r_moveq n, d
        rop 35, \n, \d
        .endm
        .macro  r_moveta s, d
        rop 36, \s, \d
        .endm
        .macro  r_movefa s, d
        rop 37, \s, \d
        .endm
        .macro  r_movei v, d
        rop 38, 0, \d
        .word   (\v) & 0xFFFF
        .word   ((\v) >> 16) & 0xFFFF
        .endm
        .macro  r_loadb s, d
        rop 39, \s, \d
        .endm
        .macro  r_loadw s, d
        rop 40, \s, \d
        .endm
        .macro  r_load s, d
        rop 41, \s, \d
        .endm
        .macro  r_loadp s, d
        rop 42, \s, \d
        .endm
        .macro  r_load14n n, d
        rop 43, \n, \d
        .endm
        .macro  r_storeb a, v
        rop 45, \a, \v
        .endm
        .macro  r_store a, v
        rop 47, \a, \v
        .endm
        .macro  r_storep a, v
        rop 48, \a, \v
        .endm
        .macro  r_movepc d
        rop 51, 0, \d
        .endm
        .macro  r_jump cc, s
        rop 52, \s, \cc
        .endm
        .macro  r_jr cc, to
        rop 53, ((\to) - . - 2) >> 1, \cc
        .endm
        .macro  r_normi s, d
        rop 56, \s, \d
        .endm
        .macro  r_nop
        rop 57, 0, 0
        .endm
        .macro  r_load14r s, d
        rop 58, \s, \d
        .endm
        .macro  r_storew a, v
        rop 46, \a, \v
        .endm
        .macro  r_btst n, d
        rop 13, \n, \d
        .endm
        | só no DSP
        .macro  r_subqmod n, d
        rop 32, \n, \d
        .endm
        .macro  r_sat16s d
        rop 33, 0, \d
        .endm
        .macro  r_sat32s d
        rop 42, 0, \d
        .endm
        .macro  r_mirror d
        rop 48, 0, \d
        .endm
        .macro  r_addqmod n, d
        rop 63, \n, \d
        .endm
        | grava um resultado do teste do GPU: (r15) = registro; r15 += 4
        .macro  res d
        r_store 15, \d
        r_addq 4, 15
        .endm

        .set    CC_T, 0
        .set    CC_NE, 1
        .set    CC_EQ, 2
        .set    CC_CC, 4
        .set    CC_CS, 8
        .set    CC_PL, 0x14
        .set    CC_MI, 0x18

