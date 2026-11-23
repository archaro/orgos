// SPDX-License-Identifier: MIT
// Copyright 2026 Archaro
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "../src/page.c"
#include "dma.h"
#include "cache.h"

#define TEST_BYTES (16U * PAGE_SIZE)
static const paddr_t bases[] = {
  0x10000000ULL, 0x40000000ULL, 0x80000000ULL, 0xbfff0000ULL
};
static unsigned int claims, allocations, releases, translations, zeros;
static unsigned int barriers, cache_calls;
static uint64_t translated_physical, translated_length;
static const void *cache_address;
static size_t cache_length;
static unsigned int cache_operation;
static bool translation_failed, release_failed, cache_failed, info_failed;
static char diagnostic[256];
static size_t diagnostic_length;

static bool test_claim(paddr_t physical, unsigned int pages) {
  claims++;
  assert(translations > 0U && !translation_failed);
  assert(translated_physical == physical);
  assert(translated_length == (uint64_t)pages * PAGE_SIZE);
  return page_claim_run(physical, pages);
}

static bool test_allocate(unsigned int count, unsigned int alignment,
    paddr_t limit, paddr_t *physical) {
  allocations++;
  assert(limit == 0xc0000000ULL);
  return page_alloc_run_below(count, alignment, limit, physical);
}

static bool test_release(paddr_t physical, unsigned int pages) {
  releases++;
  return !release_failed && page_free_run(physical, pages);
}

static void *test_zero(void *destination, int value, size_t length) {
  zeros++;
  assert(value == 0 && length != 0 && length % PAGE_SIZE == 0);
  assert(translations > 0U && !translation_failed);
  paddr_t physical = (uintptr_t)destination;
  bool mapped = false;
  for (unsigned int b = 0; b < 4U; b++) {
    if (physical >= bases[b] && physical < bases[b] + TEST_BYTES &&
        length <= bases[b] + TEST_BYTES - physical)
      mapped = true;
  }
  assert(mapped);
  uint64_t index;
  assert(address_to_index(physical, &index));
  for (size_t n = 0; n < length / PAGE_SIZE; n++)
    assert(bitmap_test(allocated, index + n));
  return memset(destination, value, length);
}

#define ORGOS_DMA_IO_H
static void dma_barrier(void) { barriers++; }
#define page_claim_run test_claim
#define page_alloc_run_below test_allocate
#define page_free_run test_release
#define memset test_zero
#include "../src/dma.c"
#undef memset
#undef page_free_run
#undef page_alloc_run_below
#undef page_claim_run

bool pcie_phys_to_dma(uint64_t physical, uint64_t length, uint64_t *bus) {
  translations++;
  translated_physical = physical;
  translated_length = length;
  if (translation_failed || !bus || !length || physical >= 0xc0000000ULL ||
      length > 0xc0000000ULL - physical)
    return false;
  *bus = physical + 0x100000000ULL;
  return true;
}

bool cache_get_info(cache_info_t *info) {
  if (info_failed)
    return false;
  *info = (cache_info_t){ .data_line_size = 64U };
  return true;
}

static bool record_cache(const void *address, size_t length,
    unsigned int operation) {
  cache_calls++;
  cache_address = address;
  cache_length = length;
  cache_operation = operation;
  return !cache_failed;
}

bool cache_clean(const void *address, size_t length) {
  return record_cache(address, length, 1U);
}

bool cache_invalidate(const void *address, size_t length) {
  return record_cache(address, length, 2U);
}

bool cache_clean_invalidate(const void *address, size_t length) {
  return record_cache(address, length, 3U);
}

void console_puts(const char *text) {
  size_t length = strlen(text);
  assert(diagnostic_length + length < sizeof(diagnostic));
  memcpy(diagnostic + diagnostic_length, text, length + 1U);
  diagnostic_length += length;
}

void console_puthex64(uint64_t value) {
  char text[32];
  snprintf(text, sizeof(text), "0x%016llx", (unsigned long long)value);
  console_puts(text);
}

void console_putu32(uint32_t value) {
  char text[16];
  snprintf(text, sizeof(text), "%u", value);
  console_puts(text);
}

static void reset(void) {
  physmem_map_t map = { .bank_count = 4U, .reservation_count = 2U };
  for (unsigned int b = 0; b < 4U; b++) {
    map.banks[b] = (fdt_memory_range_t){bases[b], TEST_BYTES};
    memset((void *)(uintptr_t)bases[b], 0xa5, TEST_BYTES);
  }
  map.reservations[0] = (physmem_reservation_t){
    .address = bases[0] + 3U * PAGE_SIZE, .size = PAGE_SIZE
  };
  map.reservations[1] = (physmem_reservation_t){
    .address = bases[0] + 5U * PAGE_SIZE + 7U, .size = 1U
  };
  assert(page_init(&map));
  claims = allocations = releases = translations = zeros = 0;
  cache_calls = barriers = 0;
  translation_failed = release_failed = cache_failed = info_failed = false;
  diagnostic_length = 0;
  diagnostic[0] = '\0';
}

static void assert_empty(const dma_buffer_t *buffer) {
  assert(!buffer->virtual && buffer->physical == 0 && buffer->bus == 0);
  assert(buffer->size == 0 && buffer->pages == 0);
  assert(buffer->owner == DMA_OWNER_CPU);
  assert(buffer->direction == DMA_TO_DEVICE);
}

static void assert_used(unsigned int pages) {
  page_stats_t stats;
  assert(page_get_stats(&stats));
  assert(stats.total_pages == 64U && stats.reserved_pages == 2U);
  assert(stats.allocated_pages == pages && stats.free_pages == 62U - pages);
}

static struct {
  uint8_t unavailable[PAGE_BITMAP_BYTES], allocated[PAGE_BITMAP_BYTES];
  uint8_t memory[4][TEST_BYTES];
  page_stats_t statistics;
  uint64_t next_page;
} saved;

static void save_state(void) {
  memcpy(saved.unavailable, unavailable, sizeof(unavailable));
  memcpy(saved.allocated, allocated, sizeof(allocated));
  saved.statistics = statistics;
  saved.next_page = next_page;
  for (unsigned int b = 0; b < 4U; b++)
    memcpy(saved.memory[b], (void *)(uintptr_t)bases[b], TEST_BYTES);
}

static void reject_at(paddr_t physical, uint64_t size, uint64_t alignment) {
  dma_buffer_t buffer;
  memset(&buffer, 0xa5, sizeof(buffer));
  save_state();
  unsigned int before_zeros = zeros;
  assert(!dma_alloc_at(physical, size, alignment, &buffer));
  assert_empty(&buffer);
  assert(zeros == before_zeros);
  assert(memcmp(saved.unavailable, unavailable, sizeof(unavailable)) == 0);
  assert(memcmp(saved.allocated, allocated, sizeof(allocated)) == 0);
  assert(memcmp(&saved.statistics, &statistics, sizeof(statistics)) == 0);
  assert(saved.next_page == next_page);
  for (unsigned int b = 0; b < 4U; b++)
    assert(memcmp(saved.memory[b], (void *)(uintptr_t)bases[b],
        TEST_BYTES) == 0);
}

static void test_success(void) {
  reset();
  const struct {
    paddr_t physical;
    uint64_t size, alignment;
    unsigned int pages;
  } cases[] = {
    {0x10000000ULL, PAGE_SIZE, PAGE_SIZE, 1U},
    {0x40001000ULL, PAGE_SIZE, PAGE_SIZE, 1U},
    {0x80000000ULL, PAGE_SIZE, PAGE_SIZE, 1U},
    {0xbffff000ULL, PAGE_SIZE, PAGE_SIZE, 1U},
    {0x40002000ULL, PAGE_SIZE + 1U, PAGE_SIZE, 2U},
    {0x80001000ULL, 1U, 1U, 1U},
    {0x80002000ULL, 63U, 64U, 1U},
    {0x80004000ULL, 2U * PAGE_SIZE, 4U * PAGE_SIZE, 2U}
  };
  for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    paddr_t physical = cases[i].physical;
    size_t length = cases[i].pages * PAGE_SIZE;
    memset((void *)(uintptr_t)physical, 0xa5, length);
    dma_buffer_t buffer;
    assert(dma_alloc_at(physical, cases[i].size, cases[i].alignment, &buffer));
    assert(buffer.virtual == (void *)(uintptr_t)physical);
    assert(buffer.physical == physical);
    assert(buffer.bus == physical + 0x100000000ULL);
    assert(buffer.size == cases[i].size && buffer.pages == cases[i].pages);
    assert(buffer.owner == DMA_OWNER_CPU && buffer.direction == DMA_TO_DEVICE);
    const uint8_t *bytes = buffer.virtual;
    for (size_t n = 0; n < length; n++)
      assert(bytes[n] == 0);
    assert_used(cases[i].pages);
    assert(dma_free(&buffer));
    assert_empty(&buffer);
    assert(!dma_free(&buffer));
    assert_used(0U);
  }
  assert(allocations == 0U);
}

static void test_rejection(void) {
  reset();
  assert(!dma_alloc_at(bases[0], PAGE_SIZE, PAGE_SIZE, NULL));
  assert(claims == 0U && translations == 0U && zeros == 0U);
  reject_at(bases[0], 0, PAGE_SIZE);
  reject_at(bases[0], 1U, 0);
  reject_at(bases[0], 1U, 3U);
  reject_at(bases[0] + 1U, 1U, 64U);
  reject_at(bases[0] + PAGE_SIZE, 1U, 2U * PAGE_SIZE);
  reject_at(bases[0], UINT64_MAX, PAGE_SIZE);
  reject_at(bases[0], (uint64_t)UINT32_MAX * PAGE_SIZE + 1U, PAGE_SIZE);
  reject_at(bases[0], 1U, 1ULL << 44);
  reject_at(0xbffff000ULL, PAGE_SIZE + 1U, PAGE_SIZE);
  reject_at(0xc0000000ULL, 1U, PAGE_SIZE);
  reject_at(0xc0001000ULL, 1U, PAGE_SIZE);
  reject_at(UINT64_MAX - (PAGE_SIZE - 1U), PAGE_SIZE, PAGE_SIZE);
  assert(claims == 0U && translations == 0U && zeros == 0U);
  reject_at(bases[0] + TEST_BYTES - PAGE_SIZE, 2U * PAGE_SIZE, PAGE_SIZE);
  reject_at(bases[0] + TEST_BYTES, PAGE_SIZE, PAGE_SIZE);
  reject_at(bases[0] + 3U * PAGE_SIZE, 1U, PAGE_SIZE);
  reject_at(bases[0] + 5U * PAGE_SIZE, 1U, PAGE_SIZE);
  reject_at(bases[0] + 2U * PAGE_SIZE, 2U * PAGE_SIZE, PAGE_SIZE);
  dma_buffer_t live;
  assert(dma_alloc_at(bases[1], PAGE_SIZE, PAGE_SIZE, &live));
  memset(live.virtual, 0x6b, PAGE_SIZE);
  reject_at(bases[1], PAGE_SIZE, PAGE_SIZE);
  reject_at(bases[1], 2U * PAGE_SIZE, PAGE_SIZE);
  unsigned int before_claims = claims;
  translation_failed = true;
  reject_at(bases[2], PAGE_SIZE + 1U, PAGE_SIZE);
  assert(translated_length == 2U * PAGE_SIZE);
  assert(claims == before_claims);
  translation_failed = false;
  assert(dma_free(&live));
  assert_used(0U);
}

static void test_ordinary_and_rollback(void) {
  reset();
  dma_buffer_t exact, ordinary;
  assert(dma_alloc_at(bases[0], PAGE_SIZE, PAGE_SIZE, &exact));
  assert(dma_alloc(PAGE_SIZE, PAGE_SIZE, &ordinary));
  assert(ordinary.physical == bases[0] + PAGE_SIZE);
  reject_at(ordinary.physical, PAGE_SIZE, PAGE_SIZE);
  assert_used(2U);
  assert(dma_free(&ordinary));
  assert(dma_free(&exact));
  assert_used(0U);

  // Ordinary allocation still translates after allocation and must roll back.
  reset();
  translation_failed = true;
  assert(!dma_alloc(PAGE_SIZE + 1U, PAGE_SIZE, &ordinary));
  assert_empty(&ordinary);
  assert(releases == 1U && zeros == 0U && diagnostic_length == 0U);
  assert_used(0U);
  translation_failed = false;
  assert(dma_alloc_at(bases[0], 2U * PAGE_SIZE, PAGE_SIZE, &exact));
  assert(dma_free(&exact));

  reset();
  translation_failed = release_failed = true;
  assert(!dma_alloc(PAGE_SIZE + 1U, PAGE_SIZE, &ordinary));
  assert_empty(&ordinary);
  assert(zeros == 0U);
  assert_used(2U);
  assert(strstr(diagnostic, "DMA allocation rollback failed:") != NULL);
  assert(strstr(diagnostic, "0x0000000010000000 pages=2") != NULL);
  // Fault injection retained the pages; the fixture explicitly repairs it.
  release_failed = translation_failed = false;
  assert(page_free_run(bases[0], 2U));
  assert_used(0U);

  reset();
  assert(!dma_alloc(0, PAGE_SIZE, &ordinary));
  assert_empty(&ordinary);
  assert(allocations == 0U);
}

static void test_ownership(void) {
  reset();
  dma_buffer_t buffer;
  assert(dma_alloc_at(bases[2], PAGE_SIZE + 1U, PAGE_SIZE, &buffer));
  assert(!dma_end_device(&buffer));
  assert(!dma_begin_device(&buffer, (dma_direction_t)99));
  assert(buffer.owner == DMA_OWNER_CPU);
  for (unsigned int direction = 0; direction < 3U; direction++) {
    dma_direction_t previous = buffer.direction;
    cache_failed = true;
    assert(!dma_begin_device(&buffer, (dma_direction_t)direction));
    assert(buffer.owner == DMA_OWNER_CPU && buffer.direction == previous);
    cache_failed = false;
    info_failed = true;
    assert(!dma_begin_device(&buffer, (dma_direction_t)direction));
    assert(buffer.owner == DMA_OWNER_CPU && buffer.direction == previous);
    info_failed = false;
    assert(dma_begin_device(&buffer, (dma_direction_t)direction));
    assert(buffer.owner == DMA_OWNER_DEVICE);
    assert(cache_address == buffer.virtual && cache_length == 2U * PAGE_SIZE);
    assert(cache_operation == (direction == DMA_TO_DEVICE ? 1U : 3U));
    dma_buffer_t before;
    memcpy(&before, &buffer, sizeof(before));
    assert(!dma_free(&buffer));
    assert(memcmp(&before, &buffer, sizeof(before)) == 0);
    assert(!dma_begin_device(&buffer, (dma_direction_t)direction));
    if (direction != DMA_TO_DEVICE) {
      cache_failed = true;
      assert(!dma_end_device(&buffer));
      assert(buffer.owner == DMA_OWNER_DEVICE);
      cache_failed = false;
    }
    unsigned int before_barriers = barriers;
    // No hardware submission exists in this test; completion is simulated.
    assert(dma_end_device(&buffer));
    assert(buffer.owner == DMA_OWNER_CPU);
    if (direction == DMA_TO_DEVICE)
      assert(barriers == before_barriers + 1U);
    else
      assert(cache_operation == 2U && cache_length == 2U * PAGE_SIZE);
    assert(!dma_end_device(&buffer));
  }
  dma_buffer_t before;
  memcpy(&before, &buffer, sizeof(before));
  release_failed = true;
  assert(!dma_free(&buffer));
  assert(memcmp(&before, &buffer, sizeof(before)) == 0);
  assert_used(2U);
  release_failed = false;
  assert(dma_free(&buffer));
  assert_used(0U);
  for (unsigned int i = 0; i < 128U; i++) {
    assert(dma_alloc_at(bases[1], PAGE_SIZE + 1U, PAGE_SIZE, &buffer));
    assert(dma_free(&buffer));
    assert_used(0U);
  }
}

int main(void) {
  for (unsigned int b = 0; b < 4U; b++) {
    // A hint never replaces an existing mapping. Refuse to dereference unless
    // the OS actually provided storage at the exact identity-map address.
    void *address = (void *)(uintptr_t)bases[b];
    void *mapped = mmap(address, TEST_BYTES, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED || mapped != address) {
      fputs("dma_test: cannot map identity-address test storage\n", stderr);
      if (mapped != MAP_FAILED)
        munmap(mapped, TEST_BYTES);
      return 1;
    }
  }
  test_success();
  test_rejection();
  test_ordinary_and_rollback();
  test_ownership();
  for (unsigned int b = 0; b < 4U; b++)
    assert(munmap((void *)(uintptr_t)bases[b], TEST_BYTES) == 0);
  puts("dma_test: allocation and simulated ownership tests passed");
  return 0;
}
