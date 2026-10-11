ENTRY(_start)

SECTIONS
{
  . = 0x80000;

  __kernel_start = .;

  .text : ALIGN(16)
  {
    KEEP(*(.text.boot))
    *(.text .text.*)
  }

  .rodata : ALIGN(16)
  {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(16)
  {
    *(.data .data.*)
  }

  .bss : ALIGN(16)
  {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  .stack (NOLOAD) : ALIGN(16)
  {
    __stack_bottom = .;
    . += 16384;
    __stack_top = .;
  }

  __kernel_end = .;

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}
