#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "cache.h"

// CTR_EL0:
// Bits [19:16] DminLine:
//   log2(number of 32-bit words per
//   minimum data-cache line)
// Bits [3:0] IminLine:
//   equivalent for instruction cache
// Thus line size in bytes = 4 << field.
bool cache_get_info(cache_info_t *info) {
  if (!info)
    return false;

  uint64_t ctr;
  __asm__ volatile( "mrs %0, ctr_el0" : "=r"(ctr));
  unsigned int dmin = (unsigned int)((ctr >> 16) & 15U);
  unsigned int imin = (unsigned int)(ctr & 15U);

  info->ctr_el0 = ctr;
  info->data_line_size = (size_t)4U << dmin;
  info->instruction_line_size = (size_t)4U << imin;

  return true;
}

typedef enum {
  CACHE_CLEAN,
  CACHE_INVALIDATE,
  CACHE_CLEAN_INVALIDATE
} cache_operation_t;

// Perform cache maintenance over every line
// intersecting [address, address + length).
// Use exclusive-end arithmetic with explicit
// overflow checking.
static bool cache_maintain(const void *address, size_t length, cache_operation_t operation) {
  if (length == 0)
    return true;

  if (!address)
    return false;

  cache_info_t info;

  if (!cache_get_info(&info))
    return false;

  uintptr_t line_size = info.data_line_size;
  if (line_size == 0 || (line_size & (line_size - 1U)) != 0)
    return false;

  uintptr_t start = (uintptr_t)address;
  if (length > UINTPTR_MAX - start)
    return false;

  uintptr_t end = start + length;
  uintptr_t first = start & ~(line_size - 1U);

  // Last line is inclusive, avoiding
  // overflow when rounding end upward.
  uintptr_t last = (end - 1U) & ~(line_size - 1U);

  // Ensure earlier CPU memory writes are
  // ordered before cache maintenance.
  __asm__ volatile("dsb sy" ::: "memory");
  for (uintptr_t p = first;; p += line_size) {
    switch (operation) {
      case CACHE_CLEAN:
        __asm__ volatile(
            "dc cvac, %0"
            :: "r"(p) : "memory");
        break;
      case CACHE_INVALIDATE:
        __asm__ volatile(
            "dc ivac, %0"
            :: "r"(p) : "memory");
        break;
      case CACHE_CLEAN_INVALIDATE:
        __asm__ volatile(
            "dc civac, %0"
            :: "r"(p) : "memory");
        break;
    }
    if (p == last)
      break;
  }

  // Complete all maintenance to the Point
  // of Coherency before returning.
  __asm__ volatile("dsb sy" ::: "memory");

  return true;
}

bool cache_clean(const void *address, size_t length) {
  return cache_maintain( address, length, CACHE_CLEAN);
}

bool cache_invalidate(const void *address, size_t length) {
  return cache_maintain( address, length, CACHE_INVALIDATE);
}

bool cache_clean_invalidate(const void *address, size_t length) {
  return cache_maintain( address, length, CACHE_CLEAN_INVALIDATE);
}

