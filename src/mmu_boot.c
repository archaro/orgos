#include "mmu_boot.h"

#define RAM_LIMIT (8ULL << 30)
#define PERIPHERAL_START 0xfc000000ULL
#define PERIPHERAL_END 0x100000000ULL
#define PCIE_START 0x600000000ULL
#define PCIE_END 0x604000000ULL

_Static_assert((EL0_STACK_VA & (MMU_L1_SIZE - 1U)) == 0,
               "Diagnostic stack starts a fresh L1 region");
_Static_assert(EL0_STACK_VA >= RAM_LIMIT &&
               EL0_STACK_TOP <= (1ULL << MMU_VA_BITS) &&
               (EL0_STACK_VA >= PCIE_END || EL0_STACK_TOP <= PCIE_START),
               "Diagnostic stack must be outside RAM and Device windows");
_Static_assert(EL0_STACK_TOP == EL0_STACK_VA + MMU_PAGE_SIZE,
               "Diagnostic stack is exactly one page");

static bool range_valid(mmu_boot_range_t range) {
  return range.start <= range.end &&
      ((range.start | range.end) & (MMU_PAGE_SIZE - 1U)) == 0;
}

static bool layout_valid(const mmu_boot_layout_t *layout) {
  if (!layout)
    return false;
  const mmu_boot_range_t ordered[] = {
    layout->text, layout->el0_text, layout->rodata, layout->data,
    layout->bss, layout->stack, layout->el0_stack[0], layout->el0_stack[1]
  };
  if (!range_valid(layout->kernel) || layout->text.start < 0x80000U ||
      layout->kernel.start != layout->text.start ||
      layout->kernel.end != layout->el0_stack[1].end ||
      layout->kernel.end > MMU_BLOCK_SIZE)
    return false;
  for (unsigned int i = 0; i < sizeof(ordered) / sizeof(ordered[0]); i++) {
    if (!range_valid(ordered[i]) ||
        (i && ordered[i - 1U].end > ordered[i].start))
      return false;
  }
  return layout->el0_text.end - layout->el0_text.start == MMU_PAGE_SIZE &&
      layout->el0_stack[0].end - layout->el0_stack[0].start == MMU_PAGE_SIZE &&
      layout->el0_stack[1].end - layout->el0_stack[1].start == MMU_PAGE_SIZE;
}

static bool in_range(vaddr_t address, mmu_boot_range_t range) {
  return address >= range.start && address < range.end;
}

static bool ram_contains(const physmem_map_t *memory, paddr_t start,
    uint64_t size) {
  for (unsigned int i = 0; i < memory->bank_count; i++) {
    uint64_t base = memory->banks[i].address;
    uint64_t length = memory->banks[i].size;
    if (start >= base && start - base <= length &&
        size <= length - (start - base))
      return true;
  }
  return false;
}

static bool memory_valid(const physmem_map_t *memory, uint64_t *total) {
  if (!memory || !memory->bank_count ||
      memory->bank_count > FDT_MAX_MEMORY_BANKS)
    return false;
  *total = 0;
  for (unsigned int i = 0; i < memory->bank_count; i++) {
    uint64_t start = memory->banks[i].address, size = memory->banks[i].size;
    if (!size || ((start | size) & (MMU_BLOCK_SIZE - 1U)) ||
        start >= RAM_LIMIT || size > RAM_LIMIT - start ||
        size > UINT64_MAX - *total)
      return false;
    uint64_t end = start + size;
    if (start < PERIPHERAL_END && end > PERIPHERAL_START)
      return false;
    for (unsigned int j = 0; j < i; j++) {
      uint64_t other = memory->banks[j].address;
      if (start < other + memory->banks[j].size && other < end)
        return false;
    }
    *total += size;
  }
  return true;
}

static mmu_mapping_attrs_t ram_attrs(void) {
  return (mmu_mapping_attrs_t){
    .attr_index = MMU_ATTR_NORMAL_WB, .ap = 0, .shareability = 3,
    .access_flag = true, .pxn = true, .uxn = true
  };
}

static mmu_mapping_attrs_t low_page_attrs(vaddr_t address,
    const mmu_boot_layout_t *layout, int owner) {
  mmu_mapping_attrs_t attrs = ram_attrs();
  if (in_range(address, layout->text)) {
    attrs.ap = 2;
    attrs.pxn = false;
  } else if (in_range(address, layout->rodata)) {
    attrs.ap = 2;
  } else if (in_range(address, layout->el0_text)) {
    attrs.ap = 3;
    attrs.uxn = false;
  } else if (in_range(address, layout->el0_stack[0]) ||
             in_range(address, layout->el0_stack[1])) {
    if (owner < 0)
      attrs.ap = 1;
  }
  return attrs;
}

static bool build(mmu_address_space_t *space,
    const mmu_table_ref_t *tables,
    const physmem_map_t *memory, const mmu_boot_layout_t *layout,
    uint64_t *mapped_ram_bytes, int owner) {
  uint64_t total;
  if (!space || !tables || !mapped_ram_bytes || !layout_valid(layout) ||
      !memory_valid(memory, &total) ||
      !ram_contains(memory, layout->kernel.start,
                    layout->kernel.end - layout->kernel.start))
    return false;
  unsigned int count = owner < 0 ? MMU_BOOT_TABLE_COUNT :
      MMU_CONTEXT_TABLE_COUNT;
  for (unsigned int i = 0; i < count; i++) {
    unsigned int expected_level = i == 0U ? 1U :
        (i == 10U || i == 12U) ? 3U : 2U;
    if (tables[i].level != expected_level ||
        !ram_contains(memory, tables[i].physical, MMU_PAGE_SIZE))
      return false;
  }
  if (!mmu_space_init(space, MMU_GEOMETRY_4K_36BIT, tables,
                      count, tables[0].physical))
    return false;
  // Preserve links to all eight RAM regions, including empty L2 tables.
  for (unsigned int region = 0; region < MMU_BOOT_RAM_REGIONS; region++) {
    if (!mmu_space_link_table(space, (uint64_t)region << 30, 1,
                             tables[region + 1U].physical))
      return false;
  }
  if (!mmu_space_link_table(space, PCIE_START, 1, tables[9].physical) ||
      !mmu_space_link_table(space, 0, 2, tables[10].physical))
    return false;

  mmu_mapping_attrs_t normal = ram_attrs();
  for (unsigned int i = 0; i < memory->bank_count; i++) {
    uint64_t start = memory->banks[i].address, size = memory->banks[i].size;
    if (start == 0) { // The first block is supplied as protected L3 pages.
      start += MMU_BLOCK_SIZE;
      size -= MMU_BLOCK_SIZE;
    }
    if (size && !mmu_space_map(space, start, start, size,
                                MMU_DESC_BLOCK, normal))
      return false;
  }
  for (vaddr_t address = 0; address < MMU_BLOCK_SIZE;
       address += MMU_PAGE_SIZE) {
    if (!mmu_space_map(space, address, address, MMU_PAGE_SIZE, MMU_DESC_PAGE,
                       low_page_attrs(address, layout, owner)))
      return false;
  }
  mmu_mapping_attrs_t device = normal;
  device.attr_index = MMU_ATTR_DEVICE;
  device.shareability = 0;
  if (!mmu_space_map(space, PERIPHERAL_START, PERIPHERAL_START,
                     PERIPHERAL_END - PERIPHERAL_START,
                     MMU_DESC_BLOCK, device) ||
      !mmu_space_map(space, PCIE_START, PCIE_START, PCIE_END - PCIE_START,
                     MMU_DESC_BLOCK, device))
    return false;
  if (owner >= 0) {
    mmu_mapping_attrs_t user_stack = normal;
    user_stack.ap = 1;
    if (!mmu_space_link_table(space, EL0_STACK_VA, 1, tables[11].physical) ||
        !mmu_space_link_table(space, EL0_STACK_VA, 2, tables[12].physical) ||
        !mmu_space_map(space, EL0_STACK_VA, layout->el0_stack[owner].start,
                        MMU_PAGE_SIZE, MMU_DESC_PAGE, user_stack))
      return false;
  }
  if (!mmu_space_seal(space))
    return false;
  *mapped_ram_bytes = total;
  return true;
}

bool mmu_boot_build(mmu_address_space_t *space,
    const mmu_table_ref_t tables[MMU_BOOT_TABLE_COUNT],
    const physmem_map_t *memory, const mmu_boot_layout_t *layout,
    uint64_t *mapped_ram_bytes) {
  return build(space, tables, memory, layout, mapped_ram_bytes, -1);
}

bool mmu_context_build(mmu_address_space_t *space,
    const mmu_table_ref_t tables[MMU_CONTEXT_TABLE_COUNT],
    const physmem_map_t *memory, const mmu_boot_layout_t *layout,
    unsigned int owner) {
  uint64_t mapped;
  return owner < 2U &&
      build(space, tables, memory, layout, &mapped, (int)owner);
}

static bool configuration_valid(const mmu_address_space_t *space,
                                unsigned int count) {
  return space && space->tables &&
      space->table_count == count &&
      (space->state == MMU_SPACE_SEALED || space->state == MMU_SPACE_ACTIVE) &&
      space->geometry.va_bits == 36U && space->geometry.pa_bits == 36U &&
      space->geometry.granule_shift == 12U &&
      space->geometry.start_level == 1U &&
      space->mair == MMU_MAIR_DEVICE_NC_WB && space->tcr == MMU_TCR_4K_36BIT &&
      space->root_physical == space->tables[0].physical;
}

// The reference is the immutable, successfully constructed boot hierarchy.
// Compare every entry, allowing only the two privileged stack aliases
// and the exact, owner-specific non-identity stack mapping to differ.
// This covers the whole image (PC, vectors, EL1 stack, data and tables),
// physical RAM and Device windows, not just the next instruction address.
bool mmu_boot_compatible(const mmu_address_space_t *kernel,
    const mmu_address_space_t *space, const mmu_boot_layout_t *layout,
    int owner) {
  unsigned int count = owner < 0 ? MMU_BOOT_TABLE_COUNT :
      MMU_CONTEXT_TABLE_COUNT;
  if (owner < -1 || owner > 1 ||
      !configuration_valid(kernel, MMU_BOOT_TABLE_COUNT) ||
      !configuration_valid(space, count) || !layout_valid(layout))
    return false;
  for (unsigned int t = 0; t < count; t++) {
    unsigned int level = t == 0U ? 1U :
        (t == 10U || t == 12U) ? 3U : 2U;
    const mmu_table_ref_t *ref = &space->tables[t];
    const mmu_table_ref_t *base = &kernel->tables[
        t < MMU_BOOT_TABLE_COUNT ? t : 0];
    uint64_t descriptor;
    if (!ref->entries || !base->entries || ref->level != level ||
        (t < MMU_BOOT_TABLE_COUNT && base->level != level) ||
        ((uintptr_t)ref->entries & 4095U) ||
        ((uintptr_t)base->entries & 4095U) ||
        !mmu_table_descriptor(ref->physical, &descriptor) ||
        !mmu_table_descriptor(base->physical, &descriptor) ||
        ref->physical < layout->bss.start ||
        ref->physical + MMU_PAGE_SIZE > layout->bss.end ||
        base->physical < layout->bss.start ||
        base->physical + MMU_PAGE_SIZE > layout->bss.end)
      return false;
    for (unsigned int i = 0; i < MMU_TABLE_ENTRIES; i++) {
      uint64_t expected = t < MMU_BOOT_TABLE_COUNT ? base->entries[i] : 0;
      if (t == 0U) {
        unsigned int child = i < 8U ? i + 1U : i == 24U ? 9U : 0U;
        uint64_t original = child ? kernel->tables[child].physical | 3U : 0;
        if (expected != original)
          return false;
        if (owner >= 0 && i == (EL0_STACK_VA >> 30))
          child = 11U;
        expected = child ? space->tables[child].physical | 3U : 0;
      } else if (t == 1U && i == 0U) {
        if (expected != (kernel->tables[10].physical | 3U))
          return false;
        expected = space->tables[10].physical | 3U;
      } else if (t == 10U) {
        vaddr_t va = (vaddr_t)i * MMU_PAGE_SIZE;
        uint64_t original;
        if (!mmu_leaf_descriptor(va, MMU_DESC_PAGE,
                                  low_page_attrs(va, layout, -1), &original) ||
            expected != original ||
            !mmu_leaf_descriptor(va, MMU_DESC_PAGE,
                                  low_page_attrs(va, layout, owner),
                                  &expected))
          return false;
      } else if (t == 11U && i == 0U) {
        expected = space->tables[12].physical | 3U;
      } else if (t == 12U && i == 0U) {
        mmu_mapping_attrs_t attrs = ram_attrs();
        attrs.ap = 1;
        if (!mmu_leaf_descriptor(layout->el0_stack[owner].start,
                                  MMU_DESC_PAGE, attrs, &expected))
          return false;
      }
      if (ref->entries[i] != expected)
        return false;
    }
  }
  return true;
}
