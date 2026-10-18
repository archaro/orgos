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
// Tracks ownership separately from reservations.
// A set bit identifies a page allocated by page_alloc().
static uint8_t allocated[PAGE_BITMAP_BYTES];
static uint64_t next_page;
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
  next_page = 0;

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
  for (uint64_t i = 0; i < (statistics.total_pages + 7U) / 8U; i++) {
    unavailable[i] = 0;
    allocated[i] = 0;
  }

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

static void bitmap_set(uint8_t *bitmap, uint64_t index) {
  bitmap[index >> 3] |= (uint8_t)(1U << (index & 7U));
}

static void bitmap_clear(uint8_t *bitmap, uint64_t index) {
  bitmap[index >> 3] &= (uint8_t)~(1U << (index & 7U));
}

static bool bitmap_test(const uint8_t *bitmap, uint64_t index) {
  return (bitmap[index >> 3] & (uint8_t)(1U << (index & 7U))) != 0;
}

// Convert a bitmap index into a physical address.
static bool index_to_address(uint64_t index, paddr_t *address) {
  for (unsigned int i = 0; i < bank_count; i++) {
    const page_bank_t *bank = &banks[i];
    uint64_t count = (bank->end - bank->start) / PAGE_SIZE;

    if (index >= bank->first_bit &&
        index - bank->first_bit < count) {
      *address = bank->start +
          (index - bank->first_bit) * PAGE_SIZE;
      return true;
    }
  }

  return false;
}

// Convert a physical address into its bitmap index.
static bool address_to_index(paddr_t address, uint64_t *index) {
  if (address & (PAGE_SIZE - 1U))
    return false;

  for (unsigned int i = 0; i < bank_count; i++) {
    const page_bank_t *bank = &banks[i];

    if (address >= bank->start && address < bank->end) {
      *index = bank->first_bit +
          (address - bank->start) / PAGE_SIZE;
      return true;
    }
  }

  return false;
}

bool page_alloc(paddr_t *address) {
  if (!ready || !address || statistics.free_pages == 0)
    return false;

  uint64_t total = statistics.total_pages;

  // Begin after the previous allocation, wrapping
  // around when we reach the end of the bitmap.
  for (uint64_t n = 0; n < total; n++) {
    uint64_t index = next_page + n;

    if (index >= total)
      index -= total;

    if (page_is_set(index))
      continue;

    paddr_t result;
    if (!index_to_address(index, &result))
      return false;

    bitmap_set(unavailable, index);
    bitmap_set(allocated, index);
    statistics.allocated_pages++;
    statistics.free_pages--;

    next_page = index + 1;
    if (next_page == total)
      next_page = 0;

    *address = result;
    return true;
  }

  return false;
}

bool page_free(paddr_t address) {
  if (!ready)
    return false;

  uint64_t index;
  if (!address_to_index(address, &index))
    return false;

  // Reject reserved pages, pages never allocated,
  // and pages which have already been released.
  if (!bitmap_test(allocated, index))
    return false;

  bitmap_clear(allocated, index);
  bitmap_clear(unavailable, index);
  statistics.allocated_pages--;
  statistics.free_pages++;

  // Prefer recently freed pages for subsequent use.
  next_page = index;

  return true;
}

// Allocate consecutive physical pages within a single
// RAM bank, satisfying the requested address alignment.
bool page_alloc_run(unsigned int count,
    unsigned int alignment_pages, paddr_t *address) {

  if (!ready || !address || count == 0 ||
      alignment_pages == 0 ||
      (alignment_pages & (alignment_pages - 1U)) != 0 ||
      count > statistics.free_pages)
    return false;

  uint64_t alignment = (uint64_t)alignment_pages * PAGE_SIZE;

  for (unsigned int b = 0; b < bank_count; b++) {
    const page_bank_t *bank = &banks[b];
    uint64_t bank_pages = (bank->end - bank->start) / PAGE_SIZE;
    if (count > bank_pages)
      continue;

    // Start at the first aligned physical address
    // in this RAM bank.
    uint64_t remainder = bank->start % alignment;
    uint64_t start = bank->start;

    if (remainder != 0)
      start += alignment - remainder;

    if (start >= bank->end)
      continue;

    uint64_t first = (start - bank->start) / PAGE_SIZE;

    while (first <= bank_pages - count) {
      uint64_t index = bank->first_bit + first;
      unsigned int n;

      // Check that the entire candidate run is free.
      for (n = 0; n < count; n++) {
        if (page_is_set(index + n))
          break;
      }

      if (n == count) {
        // Every page is free. Commit the allocation.
        for (n = 0; n < count; n++) {
          bitmap_set(unavailable, index + n);
          bitmap_set(allocated, index + n);
        }

        statistics.allocated_pages += count;
        statistics.free_pages -= count;

        next_page = index + count;
        if (next_page >= statistics.total_pages)
          next_page = 0;

        *address = bank->start + first * PAGE_SIZE;
        return true;
      }

      // Skip beyond the occupied page, then round
      // upwards to the next alignment boundary.
      uint64_t next = first + n + 1U;
      uint64_t mask = (uint64_t)alignment_pages - 1U;
      uint64_t physical_page = bank->start / PAGE_SIZE;
      uint64_t absolute = physical_page + next;
      absolute = (absolute + mask) & ~mask;
      first = absolute - physical_page;
    }
  }

  return false;
}

// Release a contiguous allocation, validating the
// entire run before changing any ownership bits.
bool page_free_run(paddr_t address, unsigned int count) {
  if (!ready || count == 0)
    return false;

  uint64_t index;
  if (!address_to_index(address, &index))
    return false;

  // Find the bank containing the starting address.
  const page_bank_t *bank = NULL;
  for (unsigned int i = 0; i < bank_count; i++) {
    if (address >= banks[i].start && address < banks[i].end) {
      bank = &banks[i];
      break;
    }
  }

  if (!bank)
    return false;

  // The run must not extend beyond the bank.
  if ((uint64_t)count > (bank->end - address) / PAGE_SIZE)
    return false;

  // All pages must currently be allocated.
  // No state changes until validation completes.
  for (unsigned int i = 0; i < count; i++) {
    if (!bitmap_test(allocated, index + i))
      return false;
  }

  for (unsigned int i = 0; i < count; i++) {
    bitmap_clear(allocated, index + i);
    bitmap_clear(unavailable, index + i);
  }

  statistics.allocated_pages -= count;
  statistics.free_pages += count;
  next_page = index;

  return true;
}

