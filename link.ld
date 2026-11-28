
ENTRY(_start)

/*
 * ELF program headers.
 *
 * These describe the intended access permissions
 * of the loadable portions of the kernel.
 *
 * PF_X = 1
 * PF_W = 2
 * PF_R = 4
 */
PHDRS
{
  text   PT_LOAD FLAGS(5);  /* Read + execute */
  el0    PT_LOAD FLAGS(5);  /* EL0 read + execute, separate pages */
  rodata PT_LOAD FLAGS(4);  /* Read only */
  data   PT_LOAD FLAGS(6);  /* Read + write */
}

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
    . = ALIGN(16);
    __c_island_start = .;
    KEEP(*(.h2_c.text))
    __c_island_end = .;
    *(.text .text.*)
    . = ALIGN(4096);
    __text_end = .;
  } :text

  .el0_text : ALIGN(4096)
  {
    __el0_text_start = .;
    KEEP(*(.el0.text))
    . = ALIGN(4096);
    __el0_text_end = .;
  } :el0

  .rodata : ALIGN(4096)
  {
    __rodata_start = .;
    *(.rodata .rodata.*)
    . = ALIGN(4096);
    __rodata_end = .;
  } :rodata

  .data : ALIGN(4096)
  {
    __data_start = .;
    *(.data .data.*)
    . = ALIGN(4096);
    __data_end = .;
  } :data

  .bss (NOLOAD) : ALIGN(4096)
  {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4096);
    __bss_end = .;
  } :data

  .stack (NOLOAD) : ALIGN(4096)
  {
    __stack_bottom = .;
    . += 16384;
    __stack_top = .;
  } :data
  .el0_stack (NOLOAD) : ALIGN(4096)
  {
    __el0_stack_bottom = .;
    . += 4096;
    __el0_stack_top = .;
  } :data
  .el0_stack1 (NOLOAD) : ALIGN(4096)
  {
    __el0_stack1_bottom = .;
    . += 4096;
    __el0_stack1_top = .;
  } :data
  . = ALIGN(4096);
  __kernel_end = .;

  ASSERT((exception_vectors & 2047) == 0,
         "Exception vectors require 2 KiB alignment")
  ASSERT(exception_vector_table_end - exception_vectors == 2048,
         "Exception table must contain sixteen 128-byte slots")

  ASSERT(__el0_text_end - __el0_text_start == 4096,
         "EL0 diagnostic must occupy exactly one code page")

  /*
   * The initial kernel page table covers
   * only the lowest 2 MiB at page granularity.
   *
   * Fail at link time if the kernel grows
   * beyond that region.
   */
  ASSERT(__c_island_end > __c_island_start &&
         __c_island_end - __c_island_start <= 2048,
         "C execution island exceeds its bounded region")
  ASSERT(__kernel_end <= 0x200000, "Kernel exceeds initial L3 mapping")

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}

