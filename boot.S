.section ".text.boot"

.global _start
.type _start, %function

.extern main
.extern __bss_start
.extern __bss_end
.extern __stack_top

_start:
  /*
   * Only let core 0 continue.
   */
  mrs x0, mpidr_el1
  and x0, x0, #0xff
  cbz x0, 1f

0:
  wfe
  b 0b

1:
  /*
   * Establish our stack.
   */
  ldr x0, =__stack_top
  mov sp, x0

  /*
   * Zero .bss.
   */
  ldr x0, =__bss_start
  ldr x1, =__bss_end

2:
  cmp x0, x1
  b.hs 3f

  str xzr, [x0], #8
  b 2b

3:
  /*
   * Enter C.
   */
  bl main

  /*
   * main() should never return, but if it does,
   * stop the processor here.
   */
4:
  wfe
  b 4b

.size _start, . - _start

