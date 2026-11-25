#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "address.h"

#define MMU_TABLE_ENTRIES 512U
#define MMU_PAGE_SIZE 4096ULL
#define MMU_BLOCK_SIZE (2ULL * 1024ULL * 1024ULL)
#define MMU_L1_SIZE (1ULL << 30)
#define MMU_VA_BITS 36U
#define MMU_PA_BITS 36U
// Fixed backend configuration: Device-nGnRnE, Normal NC and Normal WB;
// 36-bit TTBR0, WB/inner-shareable walks, TTBR1 disabled, 36-bit IPS.
#define MMU_MAIR_DEVICE_NC_WB 0xff4400ULL
#define MMU_TCR_4K_36BIT (28ULL | (1ULL << 8) | (1ULL << 10) | \
                        (3ULL << 12) | (1ULL << 23) | (1ULL << 32))

typedef struct {
  unsigned int va_bits;
  unsigned int pa_bits;
  unsigned int granule_shift;
  unsigned int start_level;
} mmu_geometry_t;
#define MMU_GEOMETRY_4K_36BIT ((mmu_geometry_t){36, 36, 12, 1})

typedef enum {
  MMU_ATTR_DEVICE = 0,
  MMU_ATTR_NORMAL_NC = 1,
  MMU_ATTR_NORMAL_WB = 2
} mmu_attribute_t;

typedef struct {
  unsigned int attr_index; // MAIR index 0..2 currently supported.
  unsigned int ap;         // Architectural AP[2:1], 0..3.
  unsigned int shareability; // 0=non-shareable, 2=outer, 3=inner.
  bool access_flag;
  bool pxn;
  bool uxn;
} mmu_mapping_attrs_t;

typedef enum {
  MMU_DESC_UNMAPPED,
  MMU_DESC_BLOCK,
  MMU_DESC_PAGE
} mmu_descriptor_kind_t;

typedef struct {
  vaddr_t virtual_address;
  paddr_t physical_address;
  uint64_t l1_descriptor;
  uint64_t l2_descriptor;
  uint64_t l3_descriptor;
  uint64_t leaf_descriptor;
  uint64_t mapping_size;
  unsigned int level;
  unsigned int attr_index;
  unsigned int access_permissions;
  bool read_only;
  bool pxn;
  bool uxn;
  bool access_flag;
  mmu_descriptor_kind_t kind;
} mmu_descriptor_info_t;

// Storage and registration are borrowed from the caller, which retains
// ownership for the space's entire lifetime. entries is a CPU pointer;
// physical is the PA encoded in descriptors/TTBR, not a pointer conversion.
typedef struct {
  uint64_t *entries;
  paddr_t physical;
  unsigned int level;
} mmu_table_ref_t;

typedef enum {
  MMU_SPACE_EMPTY,
  MMU_SPACE_BUILDING,
  MMU_SPACE_SEALED,
  MMU_SPACE_ACTIVE
} mmu_space_state_t;

typedef struct {
  mmu_geometry_t geometry;
  uint64_t mair;
  uint64_t tcr;
  paddr_t root_physical;
  const mmu_table_ref_t *tables;
  size_t table_count;
  mmu_space_state_t state;
} mmu_address_space_t;

// Only the existing 4 KiB, 36-bit, L1-start geometry is supported.
// Output remains unchanged on invalid descriptor requests.
bool mmu_table_descriptor(paddr_t physical, uint64_t *descriptor);
bool mmu_leaf_descriptor(paddr_t physical, mmu_descriptor_kind_t kind,
    mmu_mapping_attrs_t attrs, uint64_t *descriptor);

// Requires a zero-initialised space and exclusive, inactive table storage.
// Validates the full registration before clearing any entries. Pages and
// refs must not overlap each other or the space object. No allocation,
// unmapping, raw PA dereference, activation or live table editing occurs.
bool mmu_space_init(mmu_address_space_t *space, mmu_geometry_t geometry,
    const mmu_table_ref_t *tables, size_t count, paddr_t root_physical);
bool mmu_space_link_table(mmu_address_space_t *space, vaddr_t address,
    unsigned int parent_level, paddr_t child_physical);
// Map into existing linked tables. The complete range is preflighted;
// failure does not modify any descriptor. Existing mappings conflict even
// if identical. Construction must use these APIs, without raw table writes.
bool mmu_space_map(mmu_address_space_t *space, vaddr_t virtual_address,
    paddr_t physical_address, uint64_t size, mmu_descriptor_kind_t kind,
    mmu_mapping_attrs_t attrs);
bool mmu_space_seal(mmu_address_space_t *space);
// Reads registered table pages only; never dereferences the inspected VA
// or assumes that a descriptor PA equals a CPU pointer. False means an
// invalid request or unsupported/malformed hierarchy; info is then invalid.
bool mmu_space_inspect(const mmu_address_space_t *space, vaddr_t address,
    mmu_descriptor_info_t *info);
