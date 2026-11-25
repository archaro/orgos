#include "mmu_tables.h"

#define DESC_VALID 1ULL
#define DESC_TABLE 2ULL
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)
#define PHYSICAL_MASK ((1ULL << MMU_PA_BITS) - 1ULL)
#define TABLE_ADDRESS_MASK (PHYSICAL_MASK & ~0xfffULL)

static bool geometry_valid(mmu_geometry_t geometry) {
  return geometry.va_bits == MMU_VA_BITS && geometry.pa_bits == MMU_PA_BITS &&
      geometry.granule_shift == 12U && geometry.start_level == 1U;
}

static bool attrs_valid(mmu_mapping_attrs_t attrs) {
  return attrs.attr_index <= MMU_ATTR_NORMAL_WB && attrs.ap <= 3U &&
      (attrs.shareability == 0U || attrs.shareability == 2U ||
       attrs.shareability == 3U);
}

static uint64_t mapping_size(mmu_descriptor_kind_t kind) {
  return kind == MMU_DESC_BLOCK ? MMU_BLOCK_SIZE :
      kind == MMU_DESC_PAGE ? MMU_PAGE_SIZE : 0;
}

bool mmu_table_descriptor(paddr_t physical, uint64_t *descriptor) {
  if (!descriptor || physical > PHYSICAL_MASK || (physical & 0xfffU))
    return false;
  *descriptor = physical | DESC_VALID | DESC_TABLE;
  return true;
}

bool mmu_leaf_descriptor(paddr_t physical, mmu_descriptor_kind_t kind,
    mmu_mapping_attrs_t attrs, uint64_t *descriptor) {
  uint64_t size = mapping_size(kind);
  if (!descriptor || !size || physical > PHYSICAL_MASK ||
      (physical & (size - 1U)) || !attrs_valid(attrs))
    return false;
  uint64_t result = physical | DESC_VALID |
      (kind == MMU_DESC_PAGE ? DESC_TABLE : 0) |
      ((uint64_t)attrs.attr_index << 2) | ((uint64_t)attrs.ap << 6) |
      ((uint64_t)attrs.shareability << 8);
  if (attrs.access_flag)
    result |= 1ULL << 10;
  if (attrs.pxn)
    result |= DESC_PXN;
  if (attrs.uxn)
    result |= DESC_UXN;
  *descriptor = result;
  return true;
}

static bool overlaps(uintptr_t a, size_t a_size, uintptr_t b, size_t b_size) {
  return a < b + b_size && b < a + a_size;
}

bool mmu_space_init(mmu_address_space_t *space, mmu_geometry_t geometry,
    const mmu_table_ref_t *tables, size_t count, paddr_t root_physical) {
  if (!space || space->state != MMU_SPACE_EMPTY || !tables || !count ||
      count > SIZE_MAX / sizeof(*tables) || !geometry_valid(geometry))
    return false;
  size_t refs_size = count * sizeof(*tables);
  uintptr_t refs = (uintptr_t)tables, object = (uintptr_t)space;
  if (refs_size > UINTPTR_MAX - refs ||
      sizeof(*space) > UINTPTR_MAX - object ||
      overlaps(refs, refs_size, object, sizeof(*space)))
    return false;

  bool root_found = false;
  for (size_t i = 0; i < count; i++) {
    const mmu_table_ref_t *table = &tables[i];
    uintptr_t cpu = (uintptr_t)table->entries;
    uint64_t descriptor;
    if (!cpu || (cpu & 0xfffU) || MMU_PAGE_SIZE > UINTPTR_MAX - cpu ||
        !mmu_table_descriptor(table->physical, &descriptor) ||
        table->level < 1U || table->level > 3U ||
        overlaps(cpu, MMU_PAGE_SIZE, refs, refs_size) ||
        overlaps(cpu, MMU_PAGE_SIZE, object, sizeof(*space)))
      return false;
    for (size_t j = 0; j < i; j++) {
      if (table->physical == tables[j].physical ||
          overlaps(cpu, MMU_PAGE_SIZE, (uintptr_t)tables[j].entries,
                   MMU_PAGE_SIZE))
        return false;
    }
    if (table->physical == root_physical && table->level == 1U)
      root_found = true;
  }
  if (!root_found)
    return false;

  // Commit only after the entire ownership registration has passed.
  for (size_t i = 0; i < count; i++) {
    for (size_t entry = 0; entry < MMU_TABLE_ENTRIES; entry++)
      tables[i].entries[entry] = 0;
  }
  *space = (mmu_address_space_t){
    .geometry = geometry, .mair = MMU_MAIR_DEVICE_NC_WB,
    .tcr = MMU_TCR_4K_36BIT,
    .root_physical = root_physical, .tables = tables, .table_count = count,
    .state = MMU_SPACE_BUILDING
  };
  return true;
}

static const mmu_table_ref_t *find_table(const mmu_address_space_t *space,
    paddr_t physical, unsigned int level) {
  if (!space || !space->tables || !space->table_count ||
      !geometry_valid(space->geometry) ||
      (space->state != MMU_SPACE_BUILDING &&
       space->state != MMU_SPACE_SEALED && space->state != MMU_SPACE_ACTIVE))
    return NULL;
  for (size_t i = 0; i < space->table_count; i++) {
    if (space->tables[i].physical == physical &&
        space->tables[i].level == level)
      return &space->tables[i];
  }
  return NULL;
}

static const mmu_table_ref_t *child_table(const mmu_address_space_t *space,
    uint64_t descriptor, unsigned int level) {
  if ((descriptor & ~TABLE_ADDRESS_MASK) != (DESC_VALID | DESC_TABLE))
    return NULL;
  return find_table(space, descriptor & TABLE_ADDRESS_MASK, level);
}

static unsigned int index_at(vaddr_t address, unsigned int level) {
  unsigned int shift = level == 1U ? 30U : level == 2U ? 21U : 12U;
  return (unsigned int)((address >> shift) & 0x1ffU);
}

static uint64_t *entry_at(const mmu_address_space_t *space, vaddr_t address,
    unsigned int target_level) {
  const mmu_table_ref_t *table = find_table(space, space->root_physical, 1);
  for (unsigned int level = 1; table && level < target_level; level++) {
    uint64_t descriptor = table->entries[index_at(address, level)];
    table = child_table(space, descriptor, level + 1U);
  }
  return table ? &table->entries[index_at(address, target_level)] : NULL;
}

bool mmu_space_link_table(mmu_address_space_t *space, vaddr_t address,
    unsigned int parent_level, paddr_t child_physical) {
  if (!space || space->state != MMU_SPACE_BUILDING ||
      address >= (1ULL << MMU_VA_BITS) ||
      (parent_level != 1U && parent_level != 2U))
    return false;
  uint64_t span = parent_level == 1U ? MMU_L1_SIZE : MMU_BLOCK_SIZE;
  if (address & (span - 1U))
    return false;
  if (!find_table(space, child_physical, parent_level + 1U))
    return false;
  uint64_t descriptor;
  if (!mmu_table_descriptor(child_physical, &descriptor))
    return false;
  uint64_t *entry = entry_at(space, address, parent_level);
  if (!entry || *entry)
    return false;
  // One parent per registered table during construction. This prevents
  // aliasing leaf slots and makes range preflight/commit unambiguous.
  for (size_t i = 0; i < space->table_count; i++) {
    if (space->tables[i].level == 3U)
      continue;
    for (size_t j = 0; j < MMU_TABLE_ENTRIES; j++) {
      uint64_t value = space->tables[i].entries[j];
      if ((value & 3U) == 3U &&
          (value & TABLE_ADDRESS_MASK) == child_physical)
        return false;
    }
  }
  *entry = descriptor;
  return true;
}

bool mmu_space_map(mmu_address_space_t *space, vaddr_t virtual_address,
    paddr_t physical_address, uint64_t size, mmu_descriptor_kind_t kind,
    mmu_mapping_attrs_t attrs) {
  uint64_t unit = mapping_size(kind), bits;
  if (!space || space->state != MMU_SPACE_BUILDING || !unit || !size ||
      ((virtual_address | physical_address | size) & (unit - 1U)) ||
      virtual_address >= (1ULL << MMU_VA_BITS) ||
      physical_address >= (1ULL << MMU_PA_BITS) ||
      size > (1ULL << MMU_VA_BITS) - virtual_address ||
      size > (1ULL << MMU_PA_BITS) - physical_address ||
      !mmu_leaf_descriptor(0, kind, attrs, &bits))
    return false;
  unsigned int level = kind == MMU_DESC_BLOCK ? 2U : 3U;
  for (uint64_t offset = 0; offset < size; offset += unit) {
    uint64_t *entry = entry_at(space, virtual_address + offset, level);
    if (!entry || *entry)
      return false;
  }
  // Exclusive inactive ownership and immutable links guarantee that the
  // same walks succeed after preflight. Leaf writes cannot change paths.
  for (uint64_t offset = 0; offset < size; offset += unit) {
    uint64_t *entry = entry_at(space, virtual_address + offset, level);
    *entry = (physical_address + offset) | bits;
  }
  return true;
}

bool mmu_space_seal(mmu_address_space_t *space) {
  if (!space || space->state != MMU_SPACE_BUILDING ||
      !find_table(space, space->root_physical, 1))
    return false;
  space->state = MMU_SPACE_SEALED;
  return true;
}

bool mmu_space_inspect(const mmu_address_space_t *space, vaddr_t address,
    mmu_descriptor_info_t *info) {
  if (!space || !info || address >= (1ULL << MMU_VA_BITS))
    return false;
  const mmu_table_ref_t *table = find_table(space, space->root_physical, 1);
  if (!table)
    return false;
  *info = (mmu_descriptor_info_t){
    .virtual_address = address, .kind = MMU_DESC_UNMAPPED
  };
  for (unsigned int level = 1; level <= 3U; level++) {
    uint64_t descriptor = table->entries[index_at(address, level)];
    info->level = level;
    if (level == 1U)
      info->l1_descriptor = descriptor;
    else if (level == 2U)
      info->l2_descriptor = descriptor;
    else
      info->l3_descriptor = descriptor;
    if (!(descriptor & DESC_VALID))
      return true;
    if (level < 3U && (descriptor & DESC_TABLE)) {
      table = child_table(space, descriptor, level + 1U);
      if (!table)
        return false;
      continue;
    }
    if (level == 1U || (level == 3U && !(descriptor & DESC_TABLE)))
      return false;
    mmu_descriptor_kind_t kind = level == 2U ? MMU_DESC_BLOCK : MMU_DESC_PAGE;
    mmu_mapping_attrs_t attrs = {
      .attr_index = (unsigned int)((descriptor >> 2) & 7U),
      .ap = (unsigned int)((descriptor >> 6) & 3U),
      .shareability = (unsigned int)((descriptor >> 8) & 3U),
      .access_flag = (descriptor & (1ULL << 10)) != 0,
      .pxn = (descriptor & DESC_PXN) != 0,
      .uxn = (descriptor & DESC_UXN) != 0
    };
    paddr_t base = descriptor & TABLE_ADDRESS_MASK;
    uint64_t canonical;
    if (!mmu_leaf_descriptor(base, kind, attrs, &canonical) ||
        canonical != descriptor)
      return false;
    info->kind = kind;
    info->leaf_descriptor = descriptor;
    info->mapping_size = mapping_size(kind);
    info->physical_address = base | (address & (info->mapping_size - 1U));
    info->attr_index = attrs.attr_index;
    info->access_permissions = attrs.ap;
    info->read_only = (attrs.ap & 2U) != 0;
    info->access_flag = attrs.access_flag;
    info->pxn = attrs.pxn;
    info->uxn = attrs.uxn;
    return true;
  }
  return false;
}
