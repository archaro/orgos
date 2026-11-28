#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mmu_high.h"
#include "mmu.h"
#include "console_commands.h"

#define __text_start high_test_text_start
#define __rodata_start high_test_rodata_start
#define __data_start high_test_data_start
#define __bss_start high_test_bss_start
#define __stack_bottom high_test_stack_bottom
#define exception_vectors high_test_exception_vectors
#include "../src/console_mmu.c"
#undef __text_start
#undef __rodata_start
#undef __data_start
#undef __bss_start
#undef __stack_bottom
#undef exception_vectors

static uint64_t pages[2][11][512] __attribute__((aligned(4096)));
static mmu_table_ref_t refs[2][11];
static mmu_address_space_t spaces[2];
static const mmu_boot_layout_t layout = {
  .kernel = {0x80000, 0x1b6000}, .text = {0x80000, 0x90000},
  .el0_text = {0x90000, 0x91000}, .rodata = {0x91000, 0x98000},
  .data = {0x98000, 0x99000}, .bss = {0x99000, 0x1b0000},
  .stack = {0x1b0000, 0x1b4000},
  .el0_stack = {{0x1b4000, 0x1b5000}, {0x1b5000, 0x1b6000}}
};
// Console samples use independent linker-style constants, never pointers.
__asm__(".global high_test_text_start\n.set high_test_text_start, 0x80000\n"
        ".global high_test_exception_vectors\n"
        ".set high_test_exception_vectors, 0x80800\n"
        ".global high_test_rodata_start\n"
        ".set high_test_rodata_start, 0x91000\n"
        ".global high_test_data_start\n.set high_test_data_start, 0x98000\n"
        ".global high_test_bss_start\n.set high_test_bss_start, 0x99000\n"
        ".global high_test_stack_bottom\n"
        ".set high_test_stack_bottom, 0x1b0000\n");
static char output[8192];
static size_t written;
static unsigned int register_reads;
static unsigned int execution_requests, vector_requests;
bool mmu_vectors_inspect(mmu_vectors_result_t *r) {
  *r = (mmu_vectors_result_t){.permanent = true};
  return true;
}
bool mmu_vectors_irq_arm(void) { return true; }
bool mmu_vectors_irq_read(mmu_vector_irq_result_t *r) {
  *r = (mmu_vector_irq_result_t){.valid = true};
  return true;
}
bool mmu_vectors_test(mmu_vectors_result_t *r) {
  vector_requests++;
  *r = (mmu_vectors_result_t){.status = "PASS", .passed = true,
    .validated = true, .observed = true, .restored = true,
    .persistent = true, .masks_restored = true, .vector = 4,
    .esr = 0x56000021};
  return true;
}
static bool persistent;
bool mmu_high_test(mmu_high_test_result_t *r) {
  execution_requests++;
  *r = (mmu_high_test_result_t){.passed = true, .validated = true,
    .activated = true, .restored = true, .masks_restored = true,
    .physical_verified = true, .observed_physical = 0x8100c, .status = "PASS",
    .high = {.result = 42, .observed_pc = MMU_TTBR1_BASE + 0x8100c}};
  r->persistent = r->already_active = r->preserved = persistent;
  return true;
}
const mmu_address_space_t *mmu_high_address_space(void) { return &spaces[1]; }
void mmu_get_info(mmu_info_t *info) {
  register_reads++;
  *info = (mmu_info_t){.ttbr0 = 0x190000, .ttbr1 = persistent ? spaces[1].root_physical : 0,
    .mair = MMU_MAIR_DEVICE_NC_WB, .tcr = persistent ? MMU_TCR_DUAL_4K : MMU_TCR_4K_36BIT,
    .sctlr = 0x81005};
}
void console_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(output + written, sizeof(output) - written, fmt, args);
  va_end(args);
  assert(n >= 0 && (size_t)n < sizeof(output) - written);
  written += (size_t)n;
}
void console_puts(const char *text) { console_printf("%s", text); }
void console_putc(char c) { console_printf("%c", c); }
void console_puthex64(uint64_t value) {
  console_printf("%016llX", (unsigned long long)value);
}

static void geometry_and_rejection(void) {
  static uint64_t table[4][512] __attribute__((aligned(4096)));
  static uint64_t snapshot[4][512];
  mmu_table_ref_t refs[] = {
    {table[0], 0x180000, 1}, {table[1], 0x181000, 2},
    {table[2], 0x182000, 2}, {table[3], 0x183000, 3}
  };
  mmu_address_space_t space = {0};
  mmu_geometry_t geometry = MMU_GEOMETRY_TTBR1_4K_39BIT;
  assert(mmu_space_init(&space, geometry, refs, 4, refs[0].physical));
  assert(space.mair == MMU_MAIR_DEVICE_NC_WB);
  assert(space.tcr == MMU_TCR_DUAL_4K);
  assert(((space.tcr >> 16) & 63) == 25);
  assert(((space.tcr >> 30) & 3) == 2); // TG1 differs from TG0 encoding.
  assert(((space.tcr >> 28) & 3) == 3);
  assert(((space.tcr >> 24) & 3) == 1 && ((space.tcr >> 26) & 3) == 1);
  assert((space.tcr & (1ULL << 23)) == 0); // Proposal only, not live.
  assert((space.tcr & 63) == 28 && ((space.tcr >> 32) & 7) == 1);
  mmu_mapping_attrs_t attrs = {2, 0, 3, true, true, true};
  assert(mmu_space_link_table(&space, MMU_TTBR1_BASE, 1, refs[1].physical));
  vaddr_t last_region = UINT64_MAX - (MMU_L1_SIZE - 1);
  vaddr_t last_block = UINT64_MAX - (MMU_BLOCK_SIZE - 1);
  vaddr_t last_page = UINT64_MAX - 4095;
  assert(mmu_space_link_table(&space, last_region, 1, refs[2].physical));
  assert(mmu_space_link_table(&space, last_block, 2, refs[3].physical));
  assert(table[0][0] == (refs[1].physical | 3));
  assert(table[0][511] == (refs[2].physical | 3));
  assert(table[2][511] == (refs[3].physical | 3));
  assert(mmu_space_map(&space, MMU_TTBR1_BASE, 0x4000000, MMU_BLOCK_SIZE,
                        MMU_DESC_BLOCK, attrs));
  assert(mmu_space_map(&space, last_page, (1ULL << 36) - 4096,
                        4096, MMU_DESC_PAGE, attrs));
  mmu_descriptor_info_t info;
  assert(mmu_space_inspect(&space, MMU_TTBR1_BASE + 0x123, &info));
  assert(info.kind == MMU_DESC_BLOCK && info.physical_address == 0x4000123);
  assert(mmu_space_inspect(&space, UINT64_MAX, &info));
  assert(info.kind == MMU_DESC_PAGE);
  assert(info.physical_address == (1ULL << 36) - 1);
  assert(info.virtual_address == UINT64_MAX);
  assert(info.pxn && info.uxn && info.access_permissions == 0);
  assert(mmu_space_inspect(&space, MMU_TTBR1_BASE + MMU_BLOCK_SIZE, &info));
  assert(info.kind == MMU_DESC_UNMAPPED && info.level == 2);
  const vaddr_t invalid[] = {0, 1ULL << 36, (1ULL << 39) - 1,
    MMU_TTBR1_BASE - 1, MMU_TTBR1_BASE ^ (1ULL << 63),
    MMU_TTBR1_BASE ^ (1ULL << 56)};
  memcpy(snapshot, table, sizeof(table));
  for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    assert(!mmu_space_inspect(&space, invalid[i], &info));
    assert(!mmu_space_map(&space, invalid[i], 0, 4096, MMU_DESC_PAGE, attrs));
    assert(!mmu_space_link_table(&space, invalid[i], 1, refs[1].physical));
  }
  assert(!mmu_space_map(&space, last_page, 0, 8192, MMU_DESC_PAGE, attrs));
  // First free page followed by conflict; preflight must leave both alone.
  assert(!mmu_space_map(&space, last_page - 4096, 0, 8192,
                         MMU_DESC_PAGE, attrs));
  assert(!mmu_space_map(&space, MMU_TTBR1_BASE + 1, 0, 4096,
                         MMU_DESC_PAGE, attrs));
  assert(!mmu_space_map(&space, MMU_TTBR1_BASE, 1, MMU_BLOCK_SIZE,
                         MMU_DESC_BLOCK, attrs));
  assert(!mmu_space_map(&space, last_page - 4096, 1ULL << 36,
                         4096, MMU_DESC_PAGE, attrs));
  assert(memcmp(snapshot, table, sizeof(table)) == 0);
  assert(mmu_space_seal(&space));
  assert(!mmu_space_map(&space, last_page - 4096, 0, 4096,
                         MMU_DESC_PAGE, attrs));
  assert(memcmp(snapshot, table, sizeof(table)) == 0);
  mmu_address_space_t empty = {0};
  geometry.region = (mmu_region_t)9;
  assert(!mmu_space_init(&empty, geometry, refs, 4, refs[0].physical));
  geometry = MMU_GEOMETRY_4K_36BIT;
  geometry.va_bits = 39; // Unsupported lower geometry remains rejected.
  assert(!mmu_space_init(&empty, geometry, refs, 4, refs[0].physical));
  assert(memcmp(snapshot, table, sizeof(table)) == 0);
}

static void policy(void) {
  const physmem_map_t memory = {.bank_count = 3, .banks = {
    {0, 0x8000000}, {0x80000000, 0x40000000},
    {0x100000000, 0x100000000}
  }};
  for (unsigned int s = 0; s < 2; s++) {
    for (unsigned int t = 0; t < 11; t++)
      refs[s][t] = (mmu_table_ref_t){pages[s][t],
        0x100000 + (s * 11 + t) * 4096, t == 0 ? 1 : t == 10 ? 3 : 2};
    if (s) {
      assert(mmu_high_build(&spaces[s], refs[s], &memory, &layout));
    } else {
      uint64_t mapped;
      assert(mmu_boot_build(&spaces[s], refs[s], &memory, &layout, &mapped));
    }
  }
  assert(spaces[1].state == MMU_SPACE_SEALED);
  assert(spaces[1].root_physical != spaces[0].root_physical);
  // Every low page preserves physical outputs and protections, with all
  // former EL0 pages restricted. One direct-map VA per physical page.
  for (paddr_t p = 0; p < MMU_BLOCK_SIZE; p += 4096) {
    mmu_descriptor_info_t low, high;
    vaddr_t va;
    assert(mmu_high_address(p + 23, &va));
    assert(mmu_space_inspect(&spaces[0], p + 23, &low));
    assert(mmu_space_inspect(&spaces[1], va, &high));
    assert(high.physical_address == p + 23 && high.kind == MMU_DESC_PAGE);
    assert(high.access_permissions == (low.access_permissions & ~1U));
    assert(high.uxn && high.pxn == low.pxn);
    assert(high.attr_index == low.attr_index);
    assert(high.access_flag && ((high.leaf_descriptor >> 8) & 3) == 3);
    assert(high.read_only == low.read_only);
  }
  for (paddr_t p = MMU_BLOCK_SIZE; p < (8ULL << 30); p += MMU_BLOCK_SIZE) {
    mmu_descriptor_info_t low, high;
    assert(mmu_space_inspect(&spaces[0], p + 43, &low));
    assert(mmu_space_inspect(&spaces[1], MMU_TTBR1_BASE + p + 43, &high));
    assert(high.kind == low.kind);
    assert(high.physical_address == low.physical_address);
    assert(high.leaf_descriptor == low.leaf_descriptor);
  }
  const paddr_t devices[] = {0xfc000000, 0xfd500123, 0xffffffff,
                             0x600000000, 0x603ffffff};
  for (unsigned int i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
    mmu_descriptor_info_t info;
    assert(mmu_space_inspect(&spaces[1], MMU_TTBR1_BASE + devices[i], &info));
    assert(info.kind == MMU_DESC_BLOCK && info.physical_address == devices[i]);
    assert(info.attr_index == 0 && ((info.leaf_descriptor >> 8) & 3) == 0);
    assert(info.access_permissions == 0 && info.pxn && info.uxn);
    assert(info.access_flag);
  }
  const paddr_t holes[] = {0x10000000, 0xc0000000, 0x200000000,
                            0x604000000, 0xfffffffff};
  for (unsigned int i = 0; i < sizeof(holes) / sizeof(holes[0]); i++) {
    mmu_descriptor_info_t info;
    assert(mmu_space_inspect(&spaces[1], MMU_TTBR1_BASE + holes[i], &info));
    assert(info.kind == MMU_DESC_UNMAPPED);
  }
  vaddr_t va = 42;
  assert(!mmu_high_address(1ULL << 36, &va) && va == 42);
  assert(!mmu_high_address(0, NULL));
  assert(mmu_high_address(0x600000000, &va) && va == 0xffffff8600000000ULL);
  mmu_descriptor_info_t info;
  assert(!mmu_space_inspect(&spaces[1], 0x80000, &info));
  assert(!mmu_space_inspect(&spaces[0], MMU_TTBR1_BASE, &info));
  assert(spaces[0].geometry.region == MMU_REGION_TTBR0);
  assert(spaces[0].tcr == MMU_TCR_4K_36BIT);
}

int main(void) {
  geometry_and_rejection();
  policy();
  cmd_kmap("");
  assert(strstr(output, "software inspection only"));
  assert(strstr(output, "State: SEALED; not activated"));
  assert(strstr(output, "FFFFFF8600000000"));
  assert(strstr(output, "Samples: PASS"));
  assert(strstr(output, "Live TTBR1 walks: DISABLED"));
  assert(strstr(output, "Live regime unchanged: YES"));
  assert(register_reads == 2);
  // The read-only diagnostic must flag a broken protection, not bless it.
  pages[1][10][0x80000 / 4096] ^= 1ULL << 6;
  written = 0;
  cmd_kmap("");
  assert(strstr(output, "Samples: FAIL"));
  pages[1][10][0x80000 / 4096] ^= 1ULL << 6;
  assert(register_reads == 4);
  written = 0;
  cmd_kmap("activate");
  assert(strstr(output, "Usage: kmap") && register_reads == 4);
  assert(spaces[1].state == MMU_SPACE_SEALED);
  assert(execution_requests == 0);
  written = 0;
  cmd_kmap("test extra");
  assert(strstr(output, "Usage: kmap") && execution_requests == 0);
  written = 0;
  cmd_kmap("test");
  assert(execution_requests == 1 && register_reads == 4);
  assert(strstr(output, "Higher-half execution test: PASS"));
  assert(strstr(output, "Calculation: 42 (expected 42)"));
  assert(strstr(output, "Translation environment: RESTORED"));
  persistent = true;
  spaces[1].state = MMU_SPACE_ACTIVE;
  written = 0;
  cmd_kmap("");
  assert(strstr(output, "State: ACTIVE; persistent"));
  assert(strstr(output, "Live TTBR1 walks: ENABLED"));
  assert(strstr(output, "Live hierarchy matches: YES"));
  assert(strstr(output, "Samples: PASS"));
  assert(strstr(output, "Live regime unchanged: YES"));
  assert(execution_requests == 1);
  written = 0;
  cmd_kmap("test");
  assert(strstr(output, "Mode: PERSISTENT (already enabled)"));
  assert(strstr(output, "Higher-half execution test: PASS"));
  assert(strstr(output, "PRESERVED (TTBR1 remains enabled)"));
  assert(execution_requests == 2);
  written = 0;
  cmd_kmap("vectors test extra");
  assert(strstr(output, "Usage: kmap") && vector_requests == 0);
  written = 0;
  cmd_kmap("vectors");
  assert(strstr(output, "Permanent higher-half vectors: ACTIVE") &&
      vector_requests == 0);
  written = 0;
  cmd_kmap("vectors test");
  assert(strstr(output, "Higher-half vector test: PASS"));
  assert(strstr(output, "Exception vector: 4"));
  assert(strstr(output, "Vector configuration preserved: YES"));
  assert(strstr(output, "Persistent TTBR1: PRESERVED (ENABLED)"));
  assert(vector_requests == 1 && execution_requests == 2);
  written = 0;
  cmd_kmap("vectors irq");
  assert(strstr(output, "EL1h IRQ: NOT OBSERVED"));
  assert(strstr(output, "EL0 IRQ: NOT OBSERVED"));
  assert(!strstr(output, "IRQ: PASS"));
  written = 0;
  cmd_kmap("vectors irq arm");
  assert(strstr(output, "IRQ observation ARMED"));
  puts("Higher-half geometry, policy and active/inactive inspection: PASS");
  return 0;
}
