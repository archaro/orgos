#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

// Exercise the production allocator, including its actual bitmaps and cursor.
// Synthetic physical addresses are numbers only: no RAM is dereferenced.
#include "../src/page.c"

static const physmem_map_t fixture = {
  .bank_count = 5U,
  .banks = {
    { .address = 0, .size = 16U * PAGE_SIZE },
    { .address = 0x3fffc000ULL, .size = 8U * PAGE_SIZE },
    { .address = 0x7fffc000ULL, .size = 8U * PAGE_SIZE },
    { .address = 0xbfffc000ULL, .size = 8U * PAGE_SIZE },
    { .address = 0x100000000ULL, .size = 8U * PAGE_SIZE }
  },
  .reservation_count = 4U,
  .reservations = {
    { .address = 0, .size = PAGE_SIZE },
    { .address = 0x3000, .size = PAGE_SIZE },
    { .address = 0x5007, .size = 1U },
    { .address = 0x40002001ULL, .size = 1U }
  }
};

static struct {
  uint8_t unavailable[PAGE_BITMAP_BYTES];
  uint8_t allocated[PAGE_BITMAP_BYTES];
  page_bank_t banks[FDT_MAX_MEMORY_BANKS];
  page_stats_t statistics;
  uint64_t next_page;
  unsigned int bank_count;
  bool ready;
} saved;

static void save_state(void) {
  memcpy(saved.unavailable, unavailable, sizeof(unavailable));
  memcpy(saved.allocated, allocated, sizeof(allocated));
  memcpy(saved.banks, banks, sizeof(banks));
  saved.statistics = statistics;
  saved.next_page = next_page;
  saved.bank_count = bank_count;
  saved.ready = ready;
}

static void assert_unchanged(void) {
  assert(memcmp(saved.unavailable, unavailable, sizeof(unavailable)) == 0);
  assert(memcmp(saved.allocated, allocated, sizeof(allocated)) == 0);
  assert(memcmp(saved.banks, banks, sizeof(banks)) == 0);
  assert(memcmp(&saved.statistics, &statistics, sizeof(statistics)) == 0);
  assert(saved.next_page == next_page);
  assert(saved.bank_count == bank_count);
  assert(saved.ready == ready);
}

static void reject_claim(paddr_t address, unsigned int count) {
  save_state();
  assert(!page_claim_run(address, count));
  assert_unchanged();
}

static void reject_free(paddr_t address, unsigned int count) {
  save_state();
  assert(!page_free_run(address, count));
  assert_unchanged();
}

static void assert_accounting(uint64_t total, uint64_t reserved,
    uint64_t used) {
  page_stats_t stats;
  assert(page_get_stats(&stats));
  assert(stats.total_pages == total);
  assert(stats.reserved_pages == reserved);
  assert(stats.allocated_pages == used);
  assert(stats.free_pages == total - reserved - used);
}

static void reset(void) {
  assert(page_init(&fixture));
  assert_accounting(48U, 4U, 0U);
}

static void test_claim_and_release(void) {
  reset();
  assert(page_claim_run(0x1000, 1U));
  assert_accounting(48U, 4U, 1U);
  reject_claim(0x1000, 1U);
  assert(page_free_run(0x1000, 1U));
  reject_free(0x1000, 1U);
  assert(page_claim_run(0x1000, 1U));
  // The single-page release interface also recognises claimed pages.
  assert(page_free(0x1000));
  assert(page_claim_run(0x8000, 4U));
  assert_accounting(48U, 4U, 4U);
  reject_claim(0x7000, 2U);
  reject_claim(0xb000, 2U);
  reject_claim(0x8000, 4U);
  assert(page_free_run(0x8000, 4U));
  assert_accounting(48U, 4U, 0U);
}

static void test_high_addresses(void) {
  reset();
  const paddr_t addresses[] = {
    0x3ffff000ULL, 0x40000000ULL, 0x40001000ULL,
    0x7ffff000ULL, 0x80000000ULL, 0x80001000ULL,
    0xbffff000ULL, 0xc0000000ULL, 0xc0001000ULL,
    0x100000000ULL
  };
  for (unsigned int i = 0; i < sizeof(addresses) / sizeof(addresses[0]);
      i++) {
    assert(page_claim_run(addresses[i], 1U));
    assert_accounting(48U, 4U, i + 1U);
  }
  for (unsigned int i = 0; i < sizeof(addresses) / sizeof(addresses[0]);
      i++)
    assert(page_free_run(addresses[i], 1U));
  assert_accounting(48U, 4U, 0U);
}

static void test_bank_edges(void) {
  reset();
  // Bank zero's first page is reserved; its first eligible page is 0x1000.
  assert(page_claim_run(0x1000, 1U));
  assert(page_claim_run(0xf000, 1U));
  for (unsigned int b = 1U; b < fixture.bank_count; b++) {
    paddr_t first = fixture.banks[b].address;
    paddr_t last = first + fixture.banks[b].size - PAGE_SIZE;
    assert(page_claim_run(first, 1U));
    assert(page_claim_run(last, 1U));
    reject_claim(last, 2U);
    assert(page_free_run(first, 1U));
    assert(page_free_run(last, 1U));
    // Reject a free last page too, without partially claiming it.
    reject_claim(last, 2U);
  }
  assert(page_free_run(0x1000, 1U));
  assert(page_free_run(0xf000, 1U));
  assert(page_claim_run(0xe000, 2U));
  assert(page_free_run(0xe000, 2U));
  reject_claim(0xf000, 2U);
  assert_accounting(48U, 4U, 0U);
}

static void test_invalid_requests(void) {
  reset();
  reject_claim(0x1001, 1U);
  reject_claim(0x1000, 0U);
  reject_claim(UINT64_MAX, 1U);
  reject_claim(UINT64_MAX - (PAGE_SIZE - 1U), 1U);
  reject_claim(UINT64_MAX - (2U * PAGE_SIZE - 1U), 2U);
  reject_claim(0x1000, UINT_MAX);
  reject_claim(0x10000, 1U);
  reject_claim(0x20000000ULL, 1U);
  reject_claim(0x40004000ULL, 1U);
  reject_claim(0xf000, 0x40000U);
  reject_claim(0, 1U);
  reject_claim(0x3000, 1U);
  reject_claim(0x5000, 1U);
  // Unavailable pages late in a run must not leave an allocated prefix.
  reject_claim(0x1000, 3U);
  reject_claim(0x4000, 2U);
  reject_claim(0x40000000ULL, 3U);
  assert(page_claim_run(0x1000, 2U));
  assert(page_claim_run(0x4000, 1U));
  assert(page_free_run(0x1000, 2U));
  assert(page_free_run(0x4000, 1U));
  assert_accounting(48U, 4U, 0U);
}

static void test_existing_allocators(void) {
  reset();
  paddr_t address;
  assert(page_alloc_run_below(2U, 2U, 0x10000, &address));
  assert(address == 0x6000);
  reject_claim(address, 2U);
  reject_claim(address - PAGE_SIZE, 2U);
  assert(page_free_run(address, 2U));

  assert(page_claim_run(0x8000, 4U));
  assert(page_alloc_run_below(2U, 4U, 0x10000, &address));
  assert(address == 0xc000);
  assert(page_free_run(address, 2U));
  assert(page_free_run(0x8000, 4U));

  assert(page_claim_run(0x1000, 2U));
  assert(page_alloc(&address));
  assert(address == 0x4000);
  reject_claim(address, 1U);
  assert(page_free(address));
  assert(page_free_run(0x1000, 2U));
  assert(page_alloc_run(2U, 1U, &address));
  assert(address == 0x1000);
  assert(page_free_run(address, 2U));
  assert_accounting(48U, 4U, 0U);
}

static void test_release_safety_and_cycles(void) {
  reset();
  reject_free(0x3000, 1U);
  reject_free(0x5000, 1U);
  reject_free(0x8000, 1U);
  for (unsigned int i = 0; i < 128U; i++) {
    assert(page_claim_run(0x8000, 3U));
    assert_accounting(48U, 4U, 3U);
    reject_free(0x8001, 3U);
    reject_free(0x8000, 0U);
    reject_free(0x7000, 4U);
    reject_free(0x8000, 4U);
    reject_claim(0x7000, 4U);
    assert(page_free_run(0x8000, 3U));
    reject_free(0x8000, 3U);
    assert_accounting(48U, 4U, 0U);
  }
}

static void test_allocator_exhaustion(void) {
  reset();
  assert(page_claim_run(0x3ffff000ULL, 2U));
  paddr_t addresses[48];
  unsigned int count = 0U;
  paddr_t address;
  while (page_alloc(&address)) {
    assert(count < 42U);
    assert(address != 0x3ffff000ULL && address != 0x40000000ULL);
    assert(address != 0 && address != 0x3000 && address != 0x5000);
    assert(address != 0x40002000ULL);
    bool in_bank = false;
    for (unsigned int b = 0; b < fixture.bank_count; b++) {
      const fdt_memory_range_t *bank = &fixture.banks[b];
      if (address >= bank->address && address < bank->address + bank->size)
        in_bank = true;
    }
    assert(in_bank);
    for (unsigned int i = 0; i < count; i++)
      assert(address != addresses[i]);
    addresses[count++] = address;
  }
  assert(count == 42U);
  assert_accounting(48U, 4U, 44U);
  reject_claim(0x1000, 1U);
  for (unsigned int i = 0; i < count; i++)
    assert(page_free(addresses[i]));
  assert(page_free_run(0x3ffff000ULL, 2U));
  assert_accounting(48U, 4U, 0U);
}

static void test_layout_variants(void) {
  physmem_map_t map = {
    .bank_count = 2U,
    .banks = {
      { .address = 0x100000000ULL, .size = 2U * PAGE_SIZE },
      { .address = 0x100002000ULL, .size = 2U * PAGE_SIZE }
    }
  };
  assert(page_init(&map));
  reject_claim(0x100001000ULL, 2U);
  assert(page_claim_run(0x100000000ULL, 2U));
  assert(page_claim_run(0x100002000ULL, 2U));
  assert_accounting(4U, 0U, 4U);
  reject_claim(0x100000000ULL, 1U);
  reject_free(0x100001000ULL, 2U);
  assert(page_free_run(0x100000000ULL, 2U));
  assert(page_free_run(0x100002000ULL, 2U));

  // Bitmap order follows the inventory, not ascending physical addresses.
  map = fixture;
  fdt_memory_range_t low = map.banks[0];
  map.banks[0] = map.banks[4];
  map.banks[4] = low;
  assert(page_init(&map));
  assert(page_claim_run(0x1000, 2U));
  assert(page_claim_run(0x100000000ULL, 2U));
  assert_accounting(48U, 4U, 4U);
  assert(page_free_run(0x1000, 2U));
  assert(page_free_run(0x100000000ULL, 2U));

  // A reservation crossing a page boundary excludes both complete pages.
  map = (physmem_map_t){
    .bank_count = 1U,
    .banks = {{ .address = 0x1000, .size = 4U * PAGE_SIZE }},
    .reservation_count = 1U,
    .reservations = {{ .address = 0x2fff, .size = 2U }}
  };
  assert(page_init(&map));
  reject_claim(0x2000, 1U);
  reject_claim(0x3000, 1U);
  assert(page_claim_run(0x1000, 1U));
  assert(page_claim_run(0x4000, 1U));
  assert_accounting(4U, 2U, 2U);
  assert(page_free_run(0x1000, 1U));
  assert(page_free_run(0x4000, 1U));
}

static void test_representability(void) {
  // Exercise the final representable bitmap entry, including cursor wrap.
  physmem_map_t map = {
    .bank_count = 1U,
    .banks = {{ .address = 0, .size = PAGE_MAX_RAM_BYTES }}
  };
  assert(page_init(&map));
  paddr_t last = PAGE_MAX_RAM_BYTES - PAGE_SIZE;
  assert(page_claim_run(last, 1U));
  assert(next_page == 0);
  assert_accounting(PAGE_MAX_PAGES, 0U, 1U);
  reject_claim(PAGE_MAX_RAM_BYTES, 1U);
  reject_claim(last, 2U);
  assert(page_free_run(last, 1U));

  // Capacity bounds RAM page count, not the highest physical address.
  map.banks[0].address = UINT64_MAX - (5ULL * PAGE_SIZE - 1U);
  map.banks[0].size = 4U * PAGE_SIZE;
  assert(page_init(&map));
  last = map.banks[0].address + 3U * PAGE_SIZE;
  reject_claim(last, 3U);
  assert(page_claim_run(last, 1U));
  assert(page_free_run(last, 1U));

  map.banks[0].address = 0;
  map.banks[0].size = PAGE_MAX_RAM_BYTES + PAGE_SIZE;
  assert(!page_init(&map));
  reject_claim(0x1000, 1U);
}

int main(void) {
  reject_claim(0x1000, 1U);
  test_claim_and_release();
  test_high_addresses();
  test_bank_edges();
  test_invalid_requests();
  test_existing_allocators();
  test_release_safety_and_cycles();
  test_allocator_exhaustion();
  test_layout_variants();
  test_representability();
  puts("page_test: all tests passed");
  return 0;
}
