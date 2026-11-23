#pragma once

// Private boundary for the CPU barrier; cache operations remain in cache.c.
static inline void dma_barrier(void) {
  __asm__ volatile("dsb sy" ::: "memory");
}
