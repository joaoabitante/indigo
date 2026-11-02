SECTIONS {
  . = 0x00000000;
  .text : { *(.vetores) *(.text*) *(.rodata*) }
  .bss 0x06000400 (NOLOAD) : { *(.bss*) *(COMMON) }
}
