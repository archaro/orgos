#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "physmem.h"

#define PAGE_SIZE 4096U

typedef uint64_t paddr_t;

typedef struct {
  uint64_t total_pages;
  uint64_t reserved_pages;
  uint64_t allocated_pages;
  uint64_t free_pages;
} page_stats_t;

bool page_init(const physmem_map_t *map);
bool page_get_stats(page_stats_t *stats);

