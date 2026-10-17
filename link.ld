
ENTRY(_start)

SECTIONS
{
  . = 0x80000;

  __kernel_start = .;

  /*
   * Executable kernel code.
   *
   * Includes bootstrap, exception vectors,
   * ordinary C functions and MMU activation.
   */
  .text : ALIGN(4096)
  {
    __text_start = .;
    KEEP(*(.text.boot))
    *(.text .text.*)
    . = ALIGN(4096);
    __text_end = .;
  }

  /*
   * Read-only kernel data.
   */
  .rodata : ALIGN(4096)
  {
    __rodata_start = .;
    *(.rodata .rodata.*)
    . = ALIGN(4096);
    __rodata_end = .;
  }

  /*
   * Initialized writable data.
   */
  .data : ALIGN(4096)
  {
    __data_start = .;
    *(.data .data.*)
    . = ALIGN(4096);
    __data_end = .;
  }

  /*
   * Zero-initialized writable data.
   *
   * Keep the bounds page-aligned so
   * boot.S can safely clear this region.
   */
  .bss (NOLOAD) : ALIGN(4096)
  {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4096);
    __bss_end = .;
  }

  /*
   * Initial EL1 kernel stack.
   */
  .stack (NOLOAD) : ALIGN(4096)
  {
    __stack_bottom = .;
    . += 16384;
    __stack_top = .;
  }

  . = ALIGN(4096);
  __kernel_end = .;

  /*
   * The initial kernel page table covers
   * only the lowest 2 MiB at page granularity.
   *
   * Fail at link time if the kernel grows
   * beyond that region.
   */
  ASSERT(__kernel_end <= 0x200000, "Kernel exceeds initial L3 mapping")

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}

