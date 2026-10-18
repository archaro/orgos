#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "page.h"

// 8 GiB of RAM at 4 KiB per page requires a 256 KiB bitmap.
#define PAGE_MAX_RAM_BYTES (8ULL * 1024 * 1024 * 1024)
#define PAGE_MAX_PAGES (PAGE_MAX_RAM_BYTES / PAGE_SIZE)
#define PAGE_BITMAP_BYTES (PAGE_MAX_PAGES / 8U)

typedef struct {
  uint64_t start;
  uint64_t end;          // Exclusive physical end address.
  uint64_t first_bit;    // First bitmap bit assigned to this bank.
} page_bank_t;

static uint8_t unavailable[PAGE_BITMAP_BYTES];
static page_bank_t banks[FDT_MAX_MEMORY_BANKS];
static unsigned int bank_count;
static page_stats_t statistics;
static bool ready;

static bool page_is_set(uint64_t index) {
  return (unavailable[index >> 3] & (uint8_t)(1U << (index & 7U))) != 0;
}

static void page_mark_reserved(uint64_t index) {
  if (page_is_set(index))
    return;

  unavailable[index >> 3] |= (uint8_t)(1U << (index & 7U));
  statistics.reserved_pages++;
}

// A reservation may cover only part of a page; in that case
// the entire page is unavailable. Reservations outside RAM
// are ignored, while reservations intersecting several banks
// are clipped separately to each bank.
static void reserve_intersection(uint64_t start, uint64_t end) {
  for (unsigned int bank = 0; bank < bank_count; bank++) {
    const page_bank_t *b = &banks[bank];
    uint64_t lo = start > b->start ? start : b->start;
    uint64_t hi = end < b->end ? end : b->end;
    if (lo >= hi)
      continue;

    uint64_t first = (lo - b->start) / PAGE_SIZE;
    uint64_t last = (hi - b->start - 1U) / PAGE_SIZE;
    for (uint64_t p = first; p <= last; p++)
      page_mark_reserved(b->first_bit + p);
  }
}

bool page_init(const physmem_map_t *map) {
  ready = false;
  bank_count = 0;
  statistics = (page_stats_t){0};

  if (!map || map->bank_count == 0 ||
      map->bank_count > FDT_MAX_MEMORY_BANKS ||
      map->reservation_count > PHYSMEM_MAX_RESERVATIONS)
    return false;

  // Validate the complete layout before altering the bitmap.
  for (unsigned int i = 0; i < map->bank_count; i++) {
    uint64_t start = map->banks[i].address;
    uint64_t size = map->banks[i].size;

    if (!size || (start % PAGE_SIZE) || (size % PAGE_SIZE) ||
        size > UINT64_MAX - start)
      return false;

    uint64_t end = start + size;
    uint64_t pages = size / PAGE_SIZE;
    if (pages > PAGE_MAX_PAGES - statistics.total_pages)
      return false;

    for (unsigned int j = 0; j < i; j++) {
      if (start < banks[j].end && banks[j].start < end)
        return false;
    }

    banks[i] = (page_bank_t){
      .start = start,
      .end = end,
      .first_bit = statistics.total_pages
    };
    statistics.total_pages += pages;
    bank_count++;
  }

  for (unsigned int i = 0; i < map->reservation_count; i++) {
    uint64_t start = map->reservations[i].address;
    uint64_t size = map->reservations[i].size;
    if (size > UINT64_MAX - start)
      return false;
  }

  // Start with every discovered RAM page available.
  for (uint64_t i = 0; i < (statistics.total_pages + 7U) / 8U; i++)
    unavailable[i] = 0;

  // Reserve all intersected pages; overlapping reservations
  // are counted only once.
  for (unsigned int i = 0; i < map->reservation_count; i++) {
    uint64_t start = map->reservations[i].address;
    uint64_t size = map->reservations[i].size;
    if (size != 0)
      reserve_intersection(start, start + size);
  }

  statistics.free_pages = statistics.total_pages - statistics.reserved_pages;
  statistics.allocated_pages = 0;
  ready = true;
  return true;
}

bool page_get_stats(page_stats_t *stats) {
  if (!ready || !stats)
    return false;
  *stats = statistics;
  return true;
}

