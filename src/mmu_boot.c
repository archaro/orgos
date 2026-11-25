#include "mmu_boot.h"

#define RAM_LIMIT (8ULL << 30)
#define PERIPHERAL_START 0xfc000000ULL
#define PERIPHERAL_END 0x100000000ULL
#define PCIE_START 0x600000000ULL
#define PCIE_END 0x604000000ULL

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
    const mmu_boot_layout_t *layout) {
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
    attrs.ap = 1;
  }
  return attrs;
}

bool mmu_boot_build(mmu_address_space_t *space,
    const mmu_table_ref_t tables[MMU_BOOT_TABLE_COUNT],
    const physmem_map_t *memory, const mmu_boot_layout_t *layout,
    uint64_t *mapped_ram_bytes) {
  uint64_t total;
  if (!space || !tables || !mapped_ram_bytes || !layout_valid(layout) ||
      !memory_valid(memory, &total) ||
      !ram_contains(memory, layout->kernel.start,
                    layout->kernel.end - layout->kernel.start))
    return false;
  for (unsigned int i = 0; i < MMU_BOOT_TABLE_COUNT; i++) {
    unsigned int expected_level = i == 0U ? 1U : i == 10U ? 3U : 2U;
    if (tables[i].level != expected_level ||
        !ram_contains(memory, tables[i].physical, MMU_PAGE_SIZE))
      return false;
  }
  if (!mmu_space_init(space, MMU_GEOMETRY_4K_36BIT, tables,
                      MMU_BOOT_TABLE_COUNT, tables[0].physical))
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
                       low_page_attrs(address, layout)))
      return false;
  }
  mmu_mapping_attrs_t device = normal;
  device.attr_index = MMU_ATTR_DEVICE;
  device.shareability = 0;
  if (!mmu_space_map(space, PERIPHERAL_START, PERIPHERAL_START,
                     PERIPHERAL_END - PERIPHERAL_START,
                     MMU_DESC_BLOCK, device) ||
      !mmu_space_map(space, PCIE_START, PCIE_START, PCIE_END - PCIE_START,
                     MMU_DESC_BLOCK, device) || !mmu_space_seal(space))
    return false;
  *mapped_ram_bytes = total;
  return true;
}
