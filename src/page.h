#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "physmem.h"
#include "address.h"

#define PAGE_SIZE 4096U

typedef struct {
  uint64_t total_pages;
  uint64_t reserved_pages;
  uint64_t allocated_pages;
  uint64_t free_pages;
} page_stats_t;

bool page_init(const physmem_map_t *map);
bool page_get_stats(page_stats_t *stats);
bool page_alloc(paddr_t *address);
bool page_free(paddr_t address);
bool page_alloc_run(unsigned int count,
    unsigned int alignment_pages, paddr_t *address);
// Claim exactly this page-aligned run within one RAM bank. Failure leaves
// allocator state unchanged. Release with page_free_run(); track ownership
// in the caller, as for other allocations. Does not access physical memory.
bool page_claim_run(paddr_t address, unsigned int count);
bool page_free_run(paddr_t address, unsigned int count);
bool page_alloc_run_below(unsigned int count,
    unsigned int alignment_pages, paddr_t limit, paddr_t *address);
