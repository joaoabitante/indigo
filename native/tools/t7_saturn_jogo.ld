SECTIONS {
  . = 0x06004000;
  .text : { *(.inicio) *(.text*) *(.rodata*) *(.data*) }
  . = ALIGN(4);
  _bss_ini = .;
  .bss (NOLOAD) : { *(.bss*) *(COMMON) }
  . = ALIGN(4);
  _bss_fim = .;
}
