#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Cortex-A72 data-cache maintenance.
// Addresses are virtual addresses in the
// currently active EL1 address space.
// These operations work at cache-line
// granularity, not byte granularity.

typedef struct {
  uint64_t ctr_el0;
  size_t data_line_size;
  size_t instruction_line_size;
} cache_info_t;

bool cache_get_info(cache_info_t *info);
bool cache_clean(const void *address, size_t length);
bool cache_invalidate(const void *address, size_t length);
bool cache_clean_invalidate( const void *address, size_t length);
bool cache_init_data(void);
