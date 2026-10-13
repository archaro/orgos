#include <stdint.h>

#include "mmio.h"
#include "timer.h"

#define TIMER_BASE  (MMIO_BASE + 0x00003000UL)

#define TIMER_CLO   (TIMER_BASE + 0x04)
#define TIMER_CHI   (TIMER_BASE + 0x08)

static uint64_t boot_time_us;

uint64_t timer_now_us(void) {
  uint32_t hi1, hi2, lo;

  // Read the 64-bit counter consistently.
  // The upper word might change while we're
  // reading the lower word. If so, retry.
  do {
    hi1 = mmio_read(TIMER_CHI);
    lo  = mmio_read(TIMER_CLO);
    hi2 = mmio_read(TIMER_CHI);
  } while (hi1 != hi2);

  return ((uint64_t)hi1 << 32) | lo;
}

void timer_init(void) {
  boot_time_us = timer_now_us();
}

uint64_t timer_uptime_us(void) {
  return timer_now_us() - boot_time_us;
}

void timer_delay_us(uint64_t us) {
  uint64_t start = timer_now_us();

  while ((uint64_t)(timer_now_us() - start) < us)
    ;
}

void timer_delay_ms(uint32_t ms) {
  timer_delay_us((uint64_t)ms * 1000U);
}

