#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mmu_activation.h"

static uint64_t pages[3][13][512] __attribute__((aligned(4096)));
static uint64_t saved_pages[3][13][512];
static mmu_table_ref_t tables[3][13];
static mmu_address_space_t spaces[3];
static mmu_activation_t control;
static paddr_t hardware_root;
static uint64_t hardware_mair = MMU_MAIR_DEVICE_NC_WB;
static uint64_t hardware_tcr = MMU_TCR_4K_36BIT;
static bool enabled = true, locked;
static uint64_t daif = 0x80;
static unsigned int writes;

static const mmu_boot_layout_t layout = {
  .kernel = {0x80000, 0x1b6000}, .text = {0x80000, 0x90000},
  .el0_text = {0x90000, 0x91000}, .rodata = {0x91000, 0x98000},
  .data = {0x98000, 0x99000}, .bss = {0x99000, 0x1b0000},
  .stack = {0x1b0000, 0x1b4000},
  .el0_stack = {{0x1b4000, 0x1b5000}, {0x1b5000, 0x1b6000}}
};
static const physmem_map_t memory = {
  .bank_count = 2, .banks = {{0, 0x8000000}, {0x100000000, 0x40000000}}
};

uint64_t mmu_activation_lock(void) {
  assert(!locked);
  locked = true;
  uint64_t saved = daif;
  daif = 0x3c0;
  return saved;
}
void mmu_activation_unlock(uint64_t saved) {
  assert(locked && daif == 0x3c0);
  locked = false;
  daif = saved;
}
bool mmu_activation_hw_matches(paddr_t root) {
  return enabled && hardware_root == root &&
      hardware_mair == MMU_MAIR_DEVICE_NC_WB &&
      hardware_tcr == MMU_TCR_4K_36BIT;
}
void mmu_replace_ttbr0(paddr_t root) {
  assert(locked && daif == 0x3c0);
  hardware_root = root;
  writes++;
}

static void build(void) {
  memset(spaces, 0, sizeof(spaces));
  for (unsigned int s = 0; s < 3; s++) {
    for (unsigned int t = 0; t < (s ? 13U : 11U); t++) {
      tables[s][t] = (mmu_table_ref_t){
        pages[s][t], 0x100000U + (s * 13U + t) * 4096U,
        t == 0 ? 1 : (t == 10 || t == 12) ? 3 : 2
      };
    }
    if (s == 0) {
      uint64_t mapped;
      assert(mmu_boot_build(&spaces[0], tables[0], &memory, &layout,
                              &mapped));
      assert(mapped == 0x48000000);
    } else {
      assert(mmu_context_build(&spaces[s], tables[s], &memory, &layout,
                                 s - 1));
    }
  }
  assert(!mmu_context_build(&spaces[1], tables[1], &memory, &layout, 2));
  hardware_root = spaces[0].root_physical;
  spaces[0].state = MMU_SPACE_ACTIVE;
  control = (mmu_activation_t){0};
}

static void policy(void) {
  for (unsigned int s = 0; s < 3; s++) {
    assert(mmu_boot_compatible(&spaces[0], &spaces[s], &layout, (int)s - 1));
    assert(spaces[s].root_physical == tables[s][0].physical);
    if (s)
      assert(spaces[s].root_physical != spaces[0].root_physical);
    // Check every low page, not just the selected stack.
    for (uint64_t va = 0; va < MMU_BLOCK_SIZE; va += MMU_PAGE_SIZE) {
      mmu_descriptor_info_t info;
      assert(mmu_space_inspect(&spaces[s], va, &info));
      assert(info.kind == MMU_DESC_PAGE && info.physical_address == va);
      bool code = va == layout.el0_text.start;
      bool stack0 = va == layout.el0_stack[0].start;
      bool stack1 = va == layout.el0_stack[1].start;
      bool owned = s == 0 && (stack0 || stack1);
      assert((info.access_permissions & 1U) == (code || owned));
      if (stack0 || stack1) {
        assert(info.access_permissions == (owned ? 1U : 0U));
        assert(info.pxn && info.uxn && !info.read_only);
      }
      if (code)
        assert(info.read_only && info.pxn && !info.uxn);
      if (!code && !stack0 && !stack1) {
        mmu_descriptor_info_t original;
        assert(mmu_space_inspect(&spaces[0], va, &original));
        assert(info.leaf_descriptor == original.leaf_descriptor);
      }
    }
    mmu_descriptor_info_t stack;
    assert(mmu_space_inspect(&spaces[s], EL0_STACK_VA, &stack));
    if (!s) {
      assert(stack.kind == MMU_DESC_UNMAPPED);
    } else {
      assert(stack.physical_address == layout.el0_stack[s - 1].start);
      assert(stack.physical_address != EL0_STACK_VA);
      assert(stack.access_permissions == 1 && stack.pxn && stack.uxn);
      assert(stack.access_flag && stack.attr_index == 2);
      assert(((stack.leaf_descriptor >> 8) & 3) == 3);
      assert(mmu_space_inspect(&spaces[s], EL0_STACK_TOP - 1, &stack));
      assert(stack.physical_address == layout.el0_stack[s - 1].end - 1);
      assert(mmu_space_inspect(&spaces[s], EL0_STACK_VA - 1, &stack));
      assert(stack.kind == MMU_DESC_UNMAPPED);
      assert(mmu_space_inspect(&spaces[s], EL0_STACK_TOP, &stack));
      assert(stack.kind == MMU_DESC_UNMAPPED);
    }
    const uint64_t addresses[] = {
      0x200000, 0x7ffffff, 0x100000000, 0xfc000000, 0xfd500000,
      0x600000000, 0x603ffffff, 0x604000000, 0x200000000
    };
    for (unsigned int i = 0; i < sizeof(addresses) / sizeof(addresses[0]);
         i++) {
      mmu_descriptor_info_t base, info;
      assert(mmu_space_inspect(&spaces[0], addresses[i], &base));
      assert(mmu_space_inspect(&spaces[s], addresses[i], &info));
      assert(info.kind == base.kind);
      assert(info.leaf_descriptor == base.leaf_descriptor);
      assert(info.physical_address == base.physical_address);
    }
  }
}

static void reject(mmu_address_space_t *space) {
  mmu_activation_t before = control;
  memcpy(saved_pages, pages, sizeof(pages));
  mmu_space_state_t states[] = {
    spaces[0].state, spaces[1].state, spaces[2].state
  };
  unsigned int old_writes = writes;
  paddr_t old_root = hardware_root;
  uint64_t old_daif = daif;
  assert(!mmu_activation_select(&control, space));
  assert(memcmp(&control, &before, sizeof(control)) == 0);
  for (unsigned int s = 0; s < 3; s++)
    assert(spaces[s].state == states[s]);
  assert(writes == old_writes && hardware_root == old_root);
  assert(memcmp(saved_pages, pages, sizeof(pages)) == 0);
  assert(daif == old_daif && !locked);
}

static void invalid_activation(void) {
  reject(NULL);
  mmu_address_space_t unregistered = spaces[1];
  reject(&unregistered); // Correct root is insufficient: ownership matters.
  reject(&spaces[0]); // Already ACTIVE, not a SEALED destination.
  const mmu_space_state_t invalid_states[] = {
    MMU_SPACE_EMPTY, MMU_SPACE_BUILDING, MMU_SPACE_ACTIVE
  };
  for (unsigned int i = 0; i < 3; i++) {
    spaces[1].state = invalid_states[i];
    reject(&spaces[1]);
  }
  spaces[1].state = MMU_SPACE_SEALED;
  spaces[2].state = MMU_SPACE_ACTIVE;
  reject(&spaces[1]);
  spaces[2].state = MMU_SPACE_SEALED;
  spaces[1].root_physical += 4096;
  reject(&spaces[1]);
  spaces[1].root_physical -= 4096;
  spaces[1].geometry.va_bits = 48;
  reject(&spaces[1]);
  spaces[1].geometry.va_bits = 36;
  spaces[1].mair ^= 1;
  reject(&spaces[1]);
  spaces[1].mair ^= 1;
  spaces[1].tcr ^= 1;
  reject(&spaces[1]);
  spaces[1].tcr ^= 1;
  spaces[1].table_count--;
  reject(&spaces[1]);
  spaces[1].table_count++;
  spaces[1].tables = NULL;
  reject(&spaces[1]);
  spaces[1].tables = tables[1];
  // Deliberate immutable-table corruption probes: never accepted for use.
  uint64_t original = pages[1][0][0];
  pages[1][0][0] = tables[0][1].physical | 3;
  reject(&spaces[1]);
  pages[1][0][0] = original;
  const unsigned int indices[] = {
    (unsigned int)(layout.el0_stack[1].start / 4096), // Peer EL0 access.
    (unsigned int)(layout.text.start / 4096), // Kernel execute permissions.
    (unsigned int)(layout.stack.start / 4096), // EL1 stack.
    (unsigned int)(tables[1][0].physical / 4096) // Table backing.
  };
  for (unsigned int i = 0; i < 4; i++) {
    pages[1][10][indices[i]] ^= 1ULL << 6;
    reject(&spaces[1]);
    pages[1][10][indices[i]] ^= 1ULL << 6;
  }
  pages[1][4][480] ^= 1ULL << 2; // Device attribute mismatch.
  reject(&spaces[1]);
  pages[1][4][480] ^= 1ULL << 2;
  tables[1][9].physical = tables[0][9].physical; // Shared table ownership.
  pages[1][0][24] = tables[1][9].physical | 3;
  reject(&spaces[1]);
  tables[1][9].physical = 0x100000U + (13 + 9) * 4096U;
  pages[1][0][24] = tables[1][9].physical | 3;
  tables[1][10].entries = tables[0][10].entries;
  reject(&spaces[1]);
  tables[1][10].entries = pages[1][10];
  // Reject wrong backing, permissions, XN, and extra user-range entries.
  const uint64_t bad_stack_bits[] = {
    4096, 1ULL << 6, 1ULL << 53, 1ULL << 54, 1ULL << 10, 1ULL << 8
  };
  for (unsigned int i = 0; i < 6; i++) {
    pages[1][12][0] ^= bad_stack_bits[i];
    reject(&spaces[1]);
    pages[1][12][0] ^= bad_stack_bits[i];
  }
  pages[1][12][1] = pages[1][12][0]; // Extra writable alias.
  reject(&spaces[1]);
  pages[1][12][1] = 0;
  pages[1][11][1] = tables[1][12].physical | 3;
  reject(&spaces[1]);
  pages[1][11][1] = 0;
  pages[1][0][33] = tables[1][11].physical | 3;
  reject(&spaces[1]);
  pages[1][0][33] = 0;
  enabled = false;
  reject(&spaces[1]);
  enabled = true;
  hardware_mair ^= 1;
  reject(&spaces[1]);
  hardware_mair ^= 1;
  hardware_tcr ^= 1;
  reject(&spaces[1]);
  hardware_tcr ^= 1;
  hardware_root ^= 4096;
  reject(&spaces[1]);
  hardware_root ^= 4096;
}

static void new_range_conflicts(void) {
  static uint64_t inactive[3][512] __attribute__((aligned(4096)));
  static uint64_t before[3][512];
  mmu_table_ref_t refs[] = {
    {inactive[0], 0x130000, 1}, {inactive[1], 0x131000, 2},
    {inactive[2], 0x132000, 3}
  };
  mmu_address_space_t space = {0};
  mmu_mapping_attrs_t attrs = {2, 1, 3, true, true, true};
  assert(mmu_space_init(&space, MMU_GEOMETRY_4K_36BIT, refs, 3,
                         refs[0].physical));
  assert(mmu_space_link_table(&space, EL0_STACK_VA, 1, refs[1].physical));
  assert(mmu_space_link_table(&space, EL0_STACK_VA, 2, refs[2].physical));
  assert(mmu_space_map(&space, EL0_STACK_TOP, layout.el0_stack[1].start,
                        4096, MMU_DESC_PAGE, attrs));
  memcpy(before, inactive, sizeof(before));
  // First slot free, second conflicts: reject without writing the first.
  assert(!mmu_space_map(&space, EL0_STACK_VA, layout.el0_stack[0].start,
                         8192, MMU_DESC_PAGE, attrs));
  assert(!mmu_space_map(&space, EL0_STACK_VA + 1,
                         layout.el0_stack[0].start, 4096,
                         MMU_DESC_PAGE, attrs));
  assert(!mmu_space_map(&space, EL0_STACK_VA, 1ULL << 36,
                         4096, MMU_DESC_PAGE, attrs));
  assert(memcmp(before, inactive, sizeof(before)) == 0);
}

int main(void) {
  new_range_conflicts();
  build();
  policy();
  assert(mmu_activation_init(&control, &spaces[0], &spaces[1], &spaces[2],
                               &layout));
  assert(writes == 0 && control.active == &spaces[0]);
  invalid_activation();
  // Repeatedly alternate, with a kernel restoration after every slice.
  for (unsigned int n = 0; n < 40; n++) {
    unsigned int s = 1U + n % 2;
    assert(mmu_activation_select(&control, &spaces[s]));
    assert(hardware_root == spaces[s].root_physical);
    assert(control.active == &spaces[s]);
    assert(spaces[0].state == MMU_SPACE_SEALED);
    assert(spaces[s].state == MMU_SPACE_ACTIVE && daif == 0x80 && !locked);
    reject(&spaces[s]);
    assert(mmu_activation_select(&control, &spaces[0]));
    assert(hardware_root == spaces[0].root_physical);
    assert(spaces[s].state == MMU_SPACE_SEALED);
    assert(daif == 0x80 && !locked);
  }
  assert(writes == 80);
  // Admission failures do not publish partially initialised control state.
  mmu_activation_t empty = {0};
  assert(!mmu_activation_init(&empty, &spaces[0], &spaces[1], &spaces[1],
                                &layout));
  assert(empty.active == NULL);
  spaces[1].state = MMU_SPACE_BUILDING;
  assert(!mmu_activation_init(&empty, &spaces[0], &spaces[1], &spaces[2],
                                &layout));
  assert(empty.active == NULL);
  spaces[1].state = MMU_SPACE_SEALED;
  tables[1][9].physical = tables[0][9].physical;
  pages[1][0][24] = tables[1][9].physical | 3;
  assert(!mmu_activation_init(&empty, &spaces[0], &spaces[1], &spaces[2],
                                &layout));
  assert(empty.active == NULL && writes == 80);
  puts("MMU activation tests: PASS "
       "(private mappings, owned roots, rejection, switching)");
  return 0;
}
