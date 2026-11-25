#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mmu_boot.h"

#define PAGE MMU_PAGE_SIZE
#define BLOCK MMU_BLOCK_SIZE
#define LIMIT (1ULL << 36)
static uint64_t pages[11][512] __attribute__((aligned(4096)));
static uint64_t snapshot[11][512];
static mmu_table_ref_t refs[11];
static const mmu_mapping_attrs_t normal = {
  .attr_index = 2, .ap = 1, .shareability = 3,
  .access_flag = true, .pxn = true, .uxn = true
};

static void registration(void) {
  for (unsigned i = 0; i < 11; i++)
    refs[i] = (mmu_table_ref_t){pages[i], 0x1000000 + i * PAGE,
                              i == 0 ? 1 : i == 10 ? 3 : 2};
}

static mmu_address_space_t hierarchy(void) {
  registration();
  mmu_address_space_t space = {0};
  assert(mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11,
                        refs[0].physical));
  assert(mmu_space_link_table(&space, 0, 1, refs[1].physical));
  assert(mmu_space_link_table(&space, 0, 2, refs[10].physical));
  return space;
}

static void descriptors(void) {
  uint64_t d = 0;
  assert(mmu_table_descriptor(0x12345000, &d));
  assert(d == 0x12345003);
  assert(mmu_leaf_descriptor(0x12345000, MMU_DESC_PAGE, normal, &d));
  assert(d == (0x12345000ULL | 3 | (2 << 2) | (1 << 6) |
               (3 << 8) | (1 << 10) | (1ULL << 53) | (1ULL << 54)));
  assert(mmu_leaf_descriptor(LIMIT - BLOCK, MMU_DESC_BLOCK, normal, &d));
  assert((d & 3) == 1 && (d & (LIMIT - BLOCK)) == LIMIT - BLOCK);
  for (unsigned ap = 0; ap < 4; ap++) {
    for (unsigned attr = 0; attr < 3; attr++) {
      for (unsigned sh = 0; sh < 4; sh++) {
        mmu_mapping_attrs_t a = {attr, ap, sh, false, false, false};
        bool ok = mmu_leaf_descriptor(0, MMU_DESC_PAGE, a, &d);
        assert(ok == (sh != 1));
        if (ok)
          assert(d == (3ULL | (attr << 2) | (ap << 6) | (sh << 8)));
      }
    }
  }
  d = 0xdeadbeef;
  assert(!mmu_table_descriptor(1, &d));
  assert(!mmu_table_descriptor(LIMIT, &d));
  assert(!mmu_table_descriptor(0, NULL));
  assert(!mmu_leaf_descriptor(PAGE, MMU_DESC_BLOCK, normal, &d));
  assert(!mmu_leaf_descriptor(1, MMU_DESC_PAGE, normal, &d));
  assert(!mmu_leaf_descriptor(LIMIT, MMU_DESC_PAGE, normal, &d));
  assert(!mmu_leaf_descriptor(0, MMU_DESC_UNMAPPED, normal, &d));
  assert(!mmu_leaf_descriptor(0, (mmu_descriptor_kind_t)99, normal, &d));
  assert(!mmu_leaf_descriptor(0, MMU_DESC_PAGE, normal, NULL));
  mmu_mapping_attrs_t bad = normal;
  bad.ap = 4;
  assert(!mmu_leaf_descriptor(0, MMU_DESC_PAGE, bad, &d));
  bad = normal;
  bad.attr_index = 3;
  assert(!mmu_leaf_descriptor(0, MMU_DESC_PAGE, bad, &d));
  bad = normal;
  bad.shareability = 4;
  assert(!mmu_leaf_descriptor(0, MMU_DESC_PAGE, bad, &d));
  assert(d == 0xdeadbeef);
}

static void inspection_and_roots(void) {
  mmu_address_space_t a = hierarchy();
  assert(mmu_space_map(&a, 0x4000, 0x32000, 2 * PAGE,
                       MMU_DESC_PAGE, normal));
  assert(mmu_space_map(&a, BLOCK, 8 * BLOCK, 2 * BLOCK,
                       MMU_DESC_BLOCK, normal));
  mmu_descriptor_info_t info;
  assert(mmu_space_inspect(&a, 0x4123, &info));
  assert(info.kind == MMU_DESC_PAGE && info.level == 3);
  assert(info.virtual_address == 0x4123 && info.physical_address == 0x32123);
  assert(info.mapping_size == PAGE && info.attr_index == 2);
  assert(info.access_permissions == 1 && !info.read_only);
  assert(info.pxn && info.uxn && info.access_flag);
  assert(((info.leaf_descriptor >> 8) & 3) == 3);
  assert(info.l1_descriptor == (refs[1].physical | 3));
  assert(info.l2_descriptor == (refs[10].physical | 3));
  assert(info.l3_descriptor == pages[10][4]);
  assert(mmu_space_inspect(&a, BLOCK + 0x12345, &info));
  assert(info.kind == MMU_DESC_BLOCK && info.level == 2);
  assert(info.physical_address == 8 * BLOCK + 0x12345);
  assert(info.mapping_size == BLOCK && info.l3_descriptor == 0);
  assert(mmu_space_inspect(&a, 0x9000, &info));
  assert(info.kind == MMU_DESC_UNMAPPED && info.level == 3);
  assert(mmu_space_inspect(&a, 3 * BLOCK, &info));
  assert(info.kind == MMU_DESC_UNMAPPED && info.level == 2);
  assert(mmu_space_inspect(&a, MMU_L1_SIZE, &info));
  assert(info.kind == MMU_DESC_UNMAPPED && info.level == 1);
  assert(!mmu_space_inspect(&a, LIMIT, &info));
  assert(!mmu_space_inspect(&a, 0, NULL));

  // A second hierarchy uses different CPU pages and different physical
  // registrations. Its root is deliberately not the first registration.
  static uint64_t other[3][512] __attribute__((aligned(4096)));
  mmu_table_ref_t second[] = {
    {other[0], 0x2100000, 3}, {other[1], 0x2110000, 1},
    {other[2], 0x2120000, 2}
  };
  mmu_address_space_t b = {0};
  assert(mmu_space_init(&b, MMU_GEOMETRY_4K_36BIT, second, 3,
                        second[1].physical));
  assert(mmu_space_link_table(&b, 0, 1, second[2].physical));
  assert(mmu_space_link_table(&b, 0, 2, second[0].physical));
  mmu_mapping_attrs_t ro = {0, 3, 0, true, true, false};
  assert(mmu_space_map(&b, 0x4000, 0x45000, PAGE, MMU_DESC_PAGE, ro));
  assert(mmu_space_inspect(&b, 0x4123, &info));
  assert(info.physical_address == 0x45123 && info.read_only);
  assert(info.attr_index == 0 && info.access_permissions == 3);
  assert(info.pxn && !info.uxn && ((info.leaf_descriptor >> 8) & 3) == 0);
  assert(mmu_space_inspect(&a, 0x4123, &info));
  assert(info.physical_address == 0x32123);

  // A corrupt/unsupported descriptor is rejected, never followed as a
  // CPU pointer. These deliberate raw edits are inspector-negative tests.
  uint64_t saved = other[1][0];
  other[1][0] = 0x34567003; // Unregistered table PA.
  assert(!mmu_space_inspect(&b, 0x4000, &info));
  other[1][0] = second[0].physical | 3; // Wrong registered level.
  assert(!mmu_space_inspect(&b, 0x4000, &info));
  other[1][0] = saved | (1ULL << 63); // Unsupported table attributes.
  assert(!mmu_space_inspect(&b, 0x4000, &info));
  other[1][0] = 1; // Unsupported L1 block.
  assert(!mmu_space_inspect(&b, 0x4000, &info));
  other[1][0] = saved;
  saved = other[0][4];
  const uint64_t invalid[] = {saved & ~2ULL, saved | (1ULL << 36),
                             saved | (1ULL << 11), saved | (1ULL << 8)};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    other[0][4] = invalid[i];
    assert(!mmu_space_inspect(&b, 0x4000, &info));
  }
  other[0][4] = saved;
  assert(mmu_space_seal(&b));
  assert(mmu_space_inspect(&b, 0x4123, &info));
  b.state = MMU_SPACE_ACTIVE; // Boot's state transition, no activation.
  assert(mmu_space_inspect(&b, 0x4123, &info));
  assert(!mmu_space_map(&b, 0x5000, 0, PAGE, MMU_DESC_PAGE, normal));
  assert(!mmu_space_link_table(&b, MMU_L1_SIZE, 1, second[2].physical));
  assert(!mmu_space_init(&b, MMU_GEOMETRY_4K_36BIT, second, 3,
                         second[1].physical));
}

static void unchanged(void) {
  assert(memcmp(snapshot, pages, sizeof(pages)) == 0);
}

static void failures_are_atomic(void) {
  mmu_address_space_t space = hierarchy();
  assert(mmu_space_map(&space, 0x5000, 0x32000, PAGE, MMU_DESC_PAGE, normal));
  memcpy(snapshot, pages, sizeof(pages));
  // The first slot is free, the second conflicts. Neither may change.
  assert(!mmu_space_map(&space, 0x4000, 0x40000, 3 * PAGE,
                        MMU_DESC_PAGE, normal));
  unchanged();
  assert(!mmu_space_map(&space, 0x5000, 0x32000, PAGE, MMU_DESC_PAGE, normal));
  unchanged();
  assert(!mmu_space_map(&space, BLOCK - PAGE, 0, 2 * PAGE,
                        MMU_DESC_PAGE, normal)); // Next L3 table missing.
  unchanged();
  assert(!mmu_space_map(&space, 0, 0, BLOCK, MMU_DESC_BLOCK, normal));
  unchanged();
  assert(!mmu_space_map(&space, MMU_L1_SIZE - BLOCK, 0, 2 * BLOCK,
                        MMU_DESC_BLOCK, normal)); // Next L2 missing.
  unchanged();
  assert(!mmu_space_map(&space, 1, 0, PAGE, MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, 0, 1, PAGE, MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, 0, 0, PAGE + 1, MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, 0, 0, 0, MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, 0, 0, PAGE, MMU_DESC_UNMAPPED, normal));
  assert(!mmu_space_map(&space, LIMIT, 0, PAGE, MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, 0, LIMIT, PAGE, MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, LIMIT - PAGE, 0, 2 * PAGE,
                        MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, 0, LIMIT - PAGE, 2 * PAGE,
                        MMU_DESC_PAGE, normal));
  assert(!mmu_space_map(&space, 0, 0, UINT64_MAX, MMU_DESC_PAGE, normal));
  mmu_mapping_attrs_t bad = normal;
  bad.ap = 4;
  assert(!mmu_space_map(&space, 0, 0, PAGE, MMU_DESC_PAGE, bad));
  assert(!mmu_space_link_table(&space, 0, 1, refs[2].physical));
  assert(!mmu_space_link_table(&space, MMU_L1_SIZE, 1, refs[1].physical));
  assert(!mmu_space_link_table(&space, 1, 1, refs[2].physical));
  assert(!mmu_space_link_table(&space, LIMIT, 1, refs[2].physical));
  assert(!mmu_space_link_table(&space, BLOCK, 2, 0x55555000));
  assert(!mmu_space_link_table(&space, BLOCK, 2, refs[2].physical));
  assert(!mmu_space_link_table(&space, 0, 3, refs[10].physical));
  unchanged();
  assert(mmu_space_seal(&space));
  assert(!mmu_space_seal(&space));
  assert(!mmu_space_map(&space, 0, 0, PAGE, MMU_DESC_PAGE, normal));
  assert(!mmu_space_link_table(&space, MMU_L1_SIZE, 1, refs[2].physical));
  unchanged();

  space = (mmu_address_space_t){0};
  mmu_geometry_t bad_geometry = MMU_GEOMETRY_4K_36BIT;
  bad_geometry.va_bits = 48;
  assert(!mmu_space_init(&space, bad_geometry, refs, 11, refs[0].physical));
  assert(!mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 0, 0));
  assert(!mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11, 0));
  mmu_table_ref_t saved = refs[10];
  refs[10].physical = refs[1].physical;
  assert(!mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11,
                         refs[0].physical));
  refs[10] = saved;
  refs[10].entries = refs[1].entries;
  assert(!mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11,
                         refs[0].physical));
  refs[10] = saved;
  refs[10].physical++;
  assert(!mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11,
                         refs[0].physical));
  refs[10] = saved;
  refs[10].entries++;
  assert(!mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11,
                         refs[0].physical));
  refs[10] = saved;
  refs[10].level = 4;
  assert(!mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11,
                         refs[0].physical));
  refs[10] = saved;
  unchanged();
  assert(space.state == MMU_SPACE_EMPTY);

  // Last legal VA/PA page, including offsets up to the boundary.
  assert(mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 11,
                        refs[0].physical));
  assert(mmu_space_link_table(&space, LIMIT - MMU_L1_SIZE, 1,
                              refs[1].physical));
  assert(mmu_space_link_table(&space, LIMIT - BLOCK, 2, refs[10].physical));
  assert(mmu_space_map(&space, LIMIT - PAGE, LIMIT - 2 * PAGE, PAGE,
                       MMU_DESC_PAGE, normal));
  mmu_descriptor_info_t info;
  assert(mmu_space_inspect(&space, LIMIT - 1, &info));
  assert(info.physical_address == LIMIT - PAGE - 1);
}

static const mmu_boot_layout_t layout = {
  .kernel = {0x80000, 0xba000}, .text = {0x80000, 0x90000},
  .el0_text = {0x90000, 0x91000}, .rodata = {0x91000, 0x98000},
  .data = {0x98000, 0x99000}, .bss = {0x99000, 0xb4000},
  .stack = {0xb4000, 0xb8000},
  .el0_stack = {{0xb8000, 0xb9000}, {0xb9000, 0xba000}}
};

// Independent reference for the pre-refactor rules in mmu.c at 375862f.
// Table PA values are supplied rather than cast from host pointers. No
// production descriptor/map helper is used to construct this oracle.
static void old_boot(uint64_t out[11][512], const physmem_map_t *memory) {
  memset(out, 0, 11 * 512 * sizeof(uint64_t));
  for (unsigned i = 0; i < 8; i++)
    out[0][i] = refs[i + 1].physical | 3;
  out[0][24] = refs[9].physical | 3;
  const uint64_t xn = (1ULL << 53) | (1ULL << 54);
  const uint64_t ram = 1 | (1 << 10) | (3 << 8) | (2 << 2) | xn;
  for (unsigned b = 0; b < memory->bank_count; b++) {
    uint64_t end = memory->banks[b].address + memory->banks[b].size;
    for (uint64_t p = memory->banks[b].address; p < end; p += BLOCK)
      out[1 + (p >> 30)][(p >> 21) & 511] = p | ram;
  }
  for (uint64_t p = 0xfc000000; p < 0x100000000; p += BLOCK)
    out[1 + (p >> 30)][(p >> 21) & 511] = p | 1 | (1 << 10) | xn;
  for (uint64_t p = 0x600000000; p < 0x604000000; p += BLOCK)
    out[9][(p >> 21) & 511] = p | 1 | (1 << 10) | xn;
  for (unsigned i = 0; i < 512; i++) {
    uint64_t p = i * PAGE, d = p | ram | 2;
    if (p >= layout.text.start && p < layout.text.end)
      d = (d | (2 << 6)) & ~(1ULL << 53);
    else if (p >= layout.rodata.start && p < layout.rodata.end)
      d |= 2 << 6;
    if (p >= layout.el0_text.start && p < layout.el0_text.end)
      d = (d | (3 << 6)) & ~(1ULL << 54);
    else if (p >= layout.el0_stack[0].start && p < layout.el0_stack[1].end)
      d |= 1 << 6;
    out[10][i] = d;
  }
  out[1][0] = refs[10].physical | 3;
}

static void boot_policy(void) {
  const physmem_map_t memories[] = {
    {.bank_count = 1, .banks = {{0, 0x8000000}}},
    {.bank_count = 3, .banks = {
      {0x100000000, 0x100000000}, {0, 0xc0000000}, {0xc0000000, 0x3c000000}
    }},
    {.bank_count = 2, .banks = {{0, 0x4000000}, {0x80000000, 0x40000000}}}
  };
  for (unsigned m = 0; m < sizeof(memories) / sizeof(memories[0]); m++) {
    registration();
    mmu_address_space_t space = {0};
    uint64_t mapped = 0, total = 0;
    old_boot(snapshot, &memories[m]);
    assert(mmu_boot_build(&space, refs, &memories[m], &layout, &mapped));
    assert(memcmp(snapshot, pages, sizeof(pages)) == 0);
    for (unsigned b = 0; b < memories[m].bank_count; b++)
      total += memories[m].banks[b].size;
    assert(mapped == total && space.state == MMU_SPACE_SEALED);
    assert(space.root_physical == refs[0].physical);
    assert(space.mair == 0xff4400);
    assert(space.tcr == (28ULL | (1ULL << 8) | (1ULL << 10) |
                         (3ULL << 12) | (1ULL << 23) | (1ULL << 32)));
    mmu_descriptor_info_t info;
    for (unsigned i = 0; i < 512; i++) {
      assert(mmu_space_inspect(&space, i * PAGE + 123, &info));
      assert(info.kind == MMU_DESC_PAGE);
      assert(info.physical_address == i * PAGE + 123);
      assert(info.leaf_descriptor == snapshot[10][i]);
    }
    for (uint64_t p = BLOCK; p < (8ULL << 30); p += BLOCK) {
      assert(mmu_space_inspect(&space, p + 123, &info));
      uint64_t expected = snapshot[1 + (p >> 30)][(p >> 21) & 511];
      assert(info.kind == (expected ? MMU_DESC_BLOCK : MMU_DESC_UNMAPPED));
      if (expected) {
        assert(info.leaf_descriptor == expected);
        assert(info.physical_address == p + 123);
      }
    }
    assert(mmu_space_inspect(&space, 0x600001234, &info));
    assert(info.kind == MMU_DESC_BLOCK && info.attr_index == 0);
    assert(info.physical_address == 0x600001234);
    assert(mmu_space_inspect(&space, 0x604000000, &info));
    assert(info.kind == MMU_DESC_UNMAPPED);
  }

  // Invalid boot descriptions fail before clearing registered storage.
  const physmem_map_t invalid[] = {
    {.bank_count = 0}, {.bank_count = 1, .banks = {{0, 1}}},
    {.bank_count = 1, .banks = {{PAGE, BLOCK}}},
    {.bank_count = 1, .banks = {{0, 0x100000000}}}, // RAM/MMIO overlap.
    {.bank_count = 2, .banks = {{0, 0x4000000}, {BLOCK, BLOCK}}},
    {.bank_count = 1, .banks = {{8ULL << 30, BLOCK}}},
    {.bank_count = 1, .banks = {{0, UINT64_MAX}}},
    {.bank_count = 1, .banks = {{BLOCK, BLOCK}}} // Kernel outside RAM.
  };
  memcpy(snapshot, pages, sizeof(pages));
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    mmu_address_space_t space = {0};
    uint64_t mapped = 0xdeadbeef;
    assert(!mmu_boot_build(&space, refs, &invalid[i], &layout, &mapped));
    assert(mapped == 0xdeadbeef && space.state == MMU_SPACE_EMPTY);
    unchanged();
  }
  mmu_address_space_t space = {0};
  mmu_boot_layout_t bad = layout;
  bad.el0_text.end += PAGE;
  uint64_t mapped = 0;
  assert(!mmu_boot_build(&space, refs, &memories[0], &bad, &mapped));
  unchanged();
  refs[10].physical = 0x8000000; // Outside the fixture's RAM.
  assert(!mmu_boot_build(&space, refs, &memories[0], &layout, &mapped));
  unchanged();
}

int main(void) {
  descriptors();
  inspection_and_roots();
  failures_are_atomic();
  boot_policy();
  puts("mmu_tables_test: PASS "
       "(descriptors, roots, non-identity, atomicity, boot equivalence)");
  return 0;
}
