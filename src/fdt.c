
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "fdt.h"

#define FDT_MAGIC 0xd00dfeedU
#define FDT_MAX_SIZE (16U * 1024U * 1024U)
#define FDT_HEADER_SIZE 40U

// Written by the bootstrap after clearing .bss.
uint64_t firmware_fdt_address;

static fdt_info_t current_fdt;
static bool fdt_valid;

// Permanent kernel-owned FDT storage.
// 4 KiB alignment will allow us to protect or
// remap this storage at page granularity later.
static uint8_t fdt_storage[FDT_STORAGE_CAPACITY] __attribute__((aligned(4096)));

static uint64_t original_fdt_address;
static bool fdt_relocated;

// Read a big-endian 32-bit integer from an FDT.
// Byte accesses also avoid alignment assumptions
// about the supplied firmware data.
static uint32_t read_be32(const volatile uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Verify that a section lies entirely within
// the advertised FDT blob.
static bool section_valid(uint32_t offset, uint32_t size, uint32_t total) {
  return offset <= total && size <= total - offset;
}

bool fdt_init(void) {
  fdt_valid = false;
  fdt_relocated = false;
  original_fdt_address = 0;

  uint64_t address = firmware_fdt_address;
  if (address == 0 || (address & 7U) != 0)
    return false;

  // The current kernel is booted into low RAM.
  // Reject addresses in the BCM2711 peripheral
  // region or beyond the CPU's known physical
  // address capabilities.
  // The firmware pointer is trusted as a boot
  // input; without a memory map we cannot yet
  // prove every nonzero physical address is RAM.
  if (address >= 0xfc000000ULL)
    return false;

  const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)address;

  if (read_be32(p + 0) != FDT_MAGIC)
    return false;

  uint32_t total = read_be32(p + 4);
  uint32_t structure = read_be32(p + 8);
  uint32_t strings = read_be32(p + 12);
  uint32_t reservations = read_be32(p + 16);
  uint32_t version = read_be32(p + 20);
  uint32_t compatible = read_be32(p + 24);
  uint32_t strings_size = read_be32(p + 32);
  uint32_t structure_size = read_be32(p + 36);

  if (total < FDT_HEADER_SIZE || total > FDT_MAX_SIZE)
    return false;

  // Do not allow a blob that crosses into the
  // peripheral address region.
  if ((uint64_t)total > 0xfc000000ULL - address)
    return false;

  // We support the version 17 header, compatible
  // with version 16, as used by modern firmware.
  if (version < 17 || compatible > 17)
    return false;

  if (!section_valid(structure, structure_size, total))
    return false;

  if (!section_valid(strings, strings_size, total))
    return false;

  if (!section_valid(reservations, 16, total))
    return false;

  current_fdt.address = address;
  current_fdt.total_size = total;
  current_fdt.version = version;
  current_fdt.last_compatible_version = compatible;
  current_fdt.structure_offset = structure;
  current_fdt.strings_offset = strings;
  current_fdt.reservations_offset = reservations;
  current_fdt.structure_size = structure_size;
  current_fdt.strings_size = strings_size;

  fdt_valid = true;
  return true;
}

bool fdt_get_info(fdt_info_t *info) {
  if (!fdt_valid || !info)
    return false;

  *info = current_fdt;
  return true;
}


// FDT structure tokens, as defined by the
// Flattened Device Tree specification.
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE   2U
#define FDT_PROP       3U
#define FDT_NOP        4U
#define FDT_END        9U

#define FDT_MAX_DEPTH 32U

// A pointer into the FDT's validated byte range.
// These are views, not copies.
typedef struct {
  const volatile uint8_t *data;
  uint32_t length;
} fdt_property_t;

static bool name_equals(const volatile uint8_t *name, uint32_t length, const char *expected) {
  uint32_t i = 0;

  while (expected[i]) {
    if (i >= length || name[i] != (uint8_t)expected[i])
      return false;
    i++;
  }
  return i == length;
}

static bool name_has_prefix(const volatile uint8_t *name, uint32_t length, const char *prefix) {
  uint32_t i = 0;

  while (prefix[i]) {
    if (i >= length || name[i] != (uint8_t)prefix[i])
      return false;
    i++;
  }
  return true;
}

// Locate the end of a NUL-terminated string
// inside a bounded region.
static bool string_length(const volatile uint8_t *p, uint32_t limit, uint32_t *length) {
  for (uint32_t i = 0; i < limit; i++) {
    if (p[i] == 0) {
      *length = i;
      return true;
    }
  }
  return false;
}

static bool advance_aligned(uint32_t *offset, uint32_t amount, uint32_t limit) {
  if (amount > limit - *offset)
    return false;
  uint32_t next = *offset + amount;
  if (next > UINT32_MAX - 3U)
    return false;
  next = (next + 3U) & ~3U;
  if (next > limit)
    return false;
  *offset = next;
  return true;
}

// Decode one or two big-endian 32-bit cells.
// Addresses and sizes are at most 64 bits in
// the memory descriptions we support.
static bool decode_cells(const volatile uint8_t *data, unsigned int cells, uint64_t *value) {
  if (cells == 1U) {
    *value = read_be32(data);
    return true;
  }
  if (cells == 2U) {
    *value = ((uint64_t)read_be32(data) << 32) | read_be32(data + 4);
    return true;
  }
  return false;
}

// Add a RAM bank to the report.
// Reject zero-length entries, overflow and
// output-capacity exhaustion.
static bool add_bank(fdt_memory_map_t *map, uint64_t address, uint64_t size) {
  if (size == 0)
    return true;
  if (size > UINT64_MAX - address)
    return false;
  if (map->bank_count >= FDT_MAX_MEMORY_BANKS)
    return false;
  fdt_memory_range_t *range = &map->banks[map->bank_count++];
  range->address = address;
  range->size = size;
  return true;
}

static bool add_reservation(fdt_memory_map_t *map, uint64_t address, uint64_t size, fdt_reservation_kind_t kind) {
  if (size == 0)
    return true;
  if (size > UINT64_MAX - address)
    return false;
  if (map->reservation_count >= FDT_MAX_RESERVATIONS)
    return false;
  fdt_reservation_t *entry = &map->reservations[map->reservation_count++];
  entry->range.address = address;
  entry->range.size = size;
  entry->kind = kind;
  return true;
}

// Decode a reg property into one or more address
// and size pairs.
static bool parse_reg(fdt_memory_map_t *map, fdt_property_t property, uint32_t address_cells, uint32_t size_cells, bool reservation) {
  if (address_cells < 1 || address_cells > 2 ||
      size_cells < 1 || size_cells > 2)
    return false;

  uint32_t tuple_bytes = (address_cells + size_cells) * 4U;
  if (property.length % tuple_bytes != 0)
    return false;

  for (uint32_t offset = 0; offset < property.length; offset += tuple_bytes) {
    uint64_t address;
    uint64_t size;
    if (!decode_cells(property.data + offset, address_cells, &address))
      return false;
    if (!decode_cells( property.data + offset + address_cells * 4U, size_cells, &size))
      return false;
    if (reservation) {
      if (!add_reservation(map, address, size, FDT_RESERVE_NODE))
        return false;
    } else {
      if (!add_bank(map, address, size))
        return false;
    }
  }
  return true;
}

// Parse the FDT reservation table.
// Every entry contains two big-endian 64-bit
// integers: address and size.
// An all-zero entry terminates the table.
static uint64_t read_be64(const volatile uint8_t *p) {
  return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static bool parse_header_reservations( const volatile uint8_t *blob, const fdt_info_t *info, fdt_memory_map_t *map) {
  uint32_t offset = info->reservations_offset;
  uint32_t limit = info->structure_offset;

  if (info->reservations_offset >= limit)
    return false;

  bool terminated = false;

  while (section_valid(offset, 16U, limit)) {
    uint64_t address = read_be64(blob + offset);
    uint64_t size = read_be64(blob + offset + 8U);
    offset += 16U;
    if (address == 0 && size == 0) {
      terminated = true;
      break;
    }

    if (!add_reservation(map, address, size, FDT_RESERVE_HEADER))
      return false;
  }

  return terminated;
}

// Look up a property name in the strings block.
// Return false if the offset points outside the
// strings block or the string has no terminator.
static bool property_name(const volatile uint8_t *strings, uint32_t strings_size, uint32_t name_offset, const volatile uint8_t **name, uint32_t *length) {
  if (name_offset >= strings_size)
    return false;

  *name = strings + name_offset;

  return string_length(*name, strings_size - name_offset, length);
}

// Determine the root node's address and size cells.
// The FDT specification defines defaults of
// 2 address cells and 1 size cell when absent.
// We traverse the structure rather than assuming
// the root properties precede all child nodes.
static bool read_root_cells(const volatile uint8_t *structure, uint32_t structure_size, const volatile uint8_t *strings, uint32_t strings_size, uint32_t *address_cells, uint32_t *size_cells) {
  uint32_t offset = 0;
  unsigned int depth = 0;
  bool ended = false;

  *address_cells = 2;
  *size_cells = 1;

  while (section_valid(offset, 4U, structure_size)) {
    uint32_t token = read_be32(structure + offset);
    offset += 4U;

    if (token == FDT_BEGIN_NODE) {
      uint32_t length;
      if (!string_length(structure + offset, structure_size - offset, &length))
        return false;
      if (!advance_aligned(&offset, length + 1U, structure_size))
        return false;
      if (++depth > FDT_MAX_DEPTH)
        return false;
    } else if (token == FDT_END_NODE) {
      if (depth == 0)
        return false;
      depth--;
    } else if (token == FDT_PROP) {
      if (!section_valid(offset, 8U, structure_size))
        return false;
      uint32_t length = read_be32(structure + offset);
      uint32_t name_offset = read_be32(structure + offset + 4U);
      offset += 8U;
      if (!section_valid(offset, length, structure_size))
        return false;
      const volatile uint8_t *name;
      uint32_t name_length;
      if (!property_name(strings, strings_size, name_offset, &name, &name_length))
        return false;
      if (depth == 1 && length == 4U) {
        if (name_equals(name, name_length, "#address-cells"))
          *address_cells = read_be32(structure + offset);
        if (name_equals(name, name_length,
                "#size-cells")) *size_cells = read_be32(structure + offset);
      }
      if (!advance_aligned(&offset, length, structure_size))
        return false;
    } else if (token == FDT_NOP) {
      continue;
    } else if (token == FDT_END) {
      if (depth != 0)
        return false;
      ended = true;
      break;
    } else {
      return false;
    }
  }

  return ended && *address_cells >= 1 && *address_cells <= 2 &&
      *size_cells >= 1 && *size_cells <= 2;
}

// Node classifications needed for memory discovery.
typedef enum {
  NODE_OTHER,
  NODE_MEMORY,
  NODE_RESERVED_ROOT,
  NODE_RESERVED_CHILD
} node_kind_t;

typedef struct {
  node_kind_t kind;
  uint32_t address_cells;
  uint32_t size_cells;
  fdt_property_t reg;
  bool has_reg;
} node_state_t;

// Parse the structure block with a bounded node stack.
// Cell counts belong to a parent bus and determine
// how each immediate child's reg property is decoded.
static bool parse_structure(const volatile uint8_t *structure, uint32_t structure_size, const volatile uint8_t *strings, uint32_t strings_size, fdt_memory_map_t *map) {
  node_state_t stack[FDT_MAX_DEPTH];
  uint32_t offset = 0;
  unsigned int depth = 0;
  bool ended = false;

  while (section_valid(offset, 4U, structure_size)) {
    uint32_t token = read_be32(structure + offset);
    offset += 4U;
    if (token == FDT_BEGIN_NODE) {
      uint32_t length;
      if (!string_length(structure + offset, structure_size - offset, &length))
        return false;
      const volatile uint8_t *name = structure + offset;
      if (depth >= FDT_MAX_DEPTH)
        return false;
      node_state_t *node = &stack[depth];
      node->kind = NODE_OTHER;
      node->has_reg = false;
      node->reg.data = NULL;
      node->reg.length = 0;
      if (depth == 0) {
        node->address_cells = map->address_cells;
        node->size_cells = map->size_cells;
      } else {
        node_state_t *parent = &stack[depth - 1U];
        node->address_cells = parent->address_cells;
        node->size_cells = parent->size_cells;
        if (depth == 1 &&
            (name_equals(name, length, "memory") || name_has_prefix(name, length, "memory@"))) {
          node->kind = NODE_MEMORY;
        } else if (depth == 1 && name_equals(name, length, "reserved-memory")) {
          node->kind = NODE_RESERVED_ROOT;
        } else if (depth == 2 && parent->kind == NODE_RESERVED_ROOT) {
          node->kind = NODE_RESERVED_CHILD;
        }
      }
      depth++;
      if (!advance_aligned(&offset, length + 1U, structure_size))
        return false;
    } else if (token == FDT_END_NODE) {
      if (depth == 0)
        return false;

      node_state_t *node = &stack[depth - 1U];
      if (node->has_reg && (node->kind == NODE_MEMORY || node->kind == NODE_RESERVED_CHILD)) {
        uint32_t address_cells = map->address_cells;
        uint32_t size_cells = map->size_cells;
        if (depth >= 2) {
          node_state_t *parent = &stack[depth - 2U];
          address_cells = parent->address_cells;
          size_cells = parent->size_cells;
        }
        if (!parse_reg(map, node->reg, address_cells, size_cells, node->kind == NODE_RESERVED_CHILD))
          return false;
      }
      depth--;
    } else if (token == FDT_PROP) {
      if (depth == 0 || !section_valid(offset, 8U, structure_size))
        return false;

      uint32_t length = read_be32(structure + offset);
      uint32_t name_offset = read_be32(structure + offset + 4U);
      offset += 8U;

      if (!section_valid(offset, length, structure_size))
        return false;

      const volatile uint8_t *name;
      uint32_t name_length;

      if (!property_name(strings, strings_size, name_offset, &name, &name_length))
        return false;

      node_state_t *node = &stack[depth - 1U];

      if (name_equals(name, name_length, "reg")) {
        node->reg.data = structure + offset;
        node->reg.length = length;
        node->has_reg = true;
      }

      if (length == 4U) {
        if (name_equals(name, name_length, "#address-cells"))
          node->address_cells = read_be32(structure + offset);

        if (name_equals(name, name_length, "#size-cells"))
          node->size_cells = read_be32(structure + offset);
      }
      if (!advance_aligned(&offset, length, structure_size))
        return false;
    } else if (token == FDT_NOP) {
      continue;
    } else if (token == FDT_END) {
      if (depth != 0)
        return false;
      ended = true;
      break;
    } else {
      return false;
    }
  }

  return ended;
}

bool fdt_get_memory_map(fdt_memory_map_t *map) {
  if (!fdt_valid || !map)
    return false;

  // We only parse OrgOS's private FDT copy.
  // The storage is part of the kernel's .bss,
  // so it is accessible both before enabling
  // translation and afterward.
  if (!fdt_relocated)
    return false;

  const volatile uint8_t *blob = (const volatile uint8_t *)(uintptr_t) current_fdt.address;
  const volatile uint8_t *structure = blob + current_fdt.structure_offset;
  const volatile uint8_t *strings = blob + current_fdt.strings_offset;

  // Clear all output counts before parsing.
  // Arrays are written only up to those counts.
  map->bank_count = 0;
  map->reservation_count = 0;

  if (!read_root_cells(
          structure, current_fdt.structure_size,
          strings, current_fdt.strings_size,
          &map->address_cells, &map->size_cells))
    return false;

  if (!parse_header_reservations(blob, &current_fdt, map))
    return false;

  if (!parse_structure( structure, current_fdt.structure_size,
          strings, current_fdt.strings_size, map))
    return false;

  return true;
}

uint64_t fdt_original_address(void) {
  return original_fdt_address;
}

bool fdt_relocate(void) {
  if (!fdt_valid)
    return false;

  if (fdt_relocated)
    return true;

  if (current_fdt.total_size > sizeof(fdt_storage))
    return false;

  uint64_t source_address = current_fdt.address;
  uint64_t destination_address = (uint64_t)(uintptr_t)fdt_storage;
  uint64_t length = current_fdt.total_size;

  // Refuse overlapping source and destination ranges.
  // Our copy operation is not intended to be memmove().
  if (source_address < destination_address + length &&
      destination_address < source_address + length)
    return false;

  const volatile uint8_t *source =
      (const volatile uint8_t *)(uintptr_t)source_address;

  // Copy bytes explicitly.
  // This code works before or after enabling the MMU,
  // provided both addresses are mapped.
  for (uint32_t i = 0; i < current_fdt.total_size; i++)
    fdt_storage[i] = source[i];

  // Verify the copied bytes before adopting the new
  // address as our authoritative Device Tree.
  for (uint32_t i = 0; i < current_fdt.total_size; i++) {
    if (fdt_storage[i] != source[i])
      return false;
  }

  // Make our kernel-owned copy authoritative.
  // All existing FDT parsing functions refer to
  // current_fdt.address, so they will automatically
  // use the private copy from this point onward.
  original_fdt_address = source_address;
  current_fdt.address = destination_address;
  fdt_relocated = true;

  return true;
}

// Test whether a string-list property contains
// the requested compatible identifier.
static bool property_has_string(
    fdt_property_t property, const char *expected) {

  uint32_t offset = 0;

  while (offset < property.length) {
    uint32_t length;

    if (!string_length(property.data + offset,
            property.length - offset, &length))
      return false;

    if (name_equals(property.data + offset,
            length, expected))
      return true;

    offset += length + 1U;
  }

  return false;
}

// Decode a property as an array of 32-bit cells.
// FDT integers are stored in big-endian order.
static bool copy_property_cells(
    fdt_property_t property,
    uint32_t *output,
    unsigned int capacity,
    unsigned int *count) {

  if ((property.length & 3U) != 0)
    return false;

  unsigned int cells = property.length / 4U;

  if (cells > capacity)
    return false;

  for (unsigned int i = 0; i < cells; i++)
    output[i] = read_be32(property.data + i * 4U);

  *count = cells;
  return true;
}

bool fdt_get_pcie_dma(fdt_pcie_dma_t *result) {
  if (!fdt_valid || !fdt_relocated || !result)
    return false;

  *result = (fdt_pcie_dma_t){0};
  const volatile uint8_t *blob =
      (const volatile uint8_t *)(uintptr_t)current_fdt.address;
  const volatile uint8_t *structure = blob + current_fdt.structure_offset;
  const volatile uint8_t *strings = blob + current_fdt.strings_offset;

  // Each level carries its own address/size-cell
  // settings. PCIe dma-ranges uses the parent bus's
  // address cells and the PCIe node's own cells.
  typedef struct {
    uint32_t address_cells;
    uint32_t size_cells;
    bool compatible;
    fdt_property_t dma;
    fdt_property_t scb;
    bool has_dma;
    bool has_scb;
  } dma_node_t;
  dma_node_t stack[FDT_MAX_DEPTH] = {0};
  unsigned int depth = 0;
  uint32_t offset = 0;

  while (section_valid(offset, 4U, current_fdt.structure_size)) {
    uint32_t token = read_be32(structure + offset);
    offset += 4U;

    if (token == FDT_BEGIN_NODE) {
      uint32_t length;
      if (depth >= FDT_MAX_DEPTH || !string_length(structure + offset,
              current_fdt.structure_size - offset, &length))
        return false;

      if (!advance_aligned(&offset, length + 1U, current_fdt.structure_size))
        return false;
      dma_node_t *node = &stack[depth];
      *node = (dma_node_t){0};
      if (depth == 0) {
        node->address_cells = 2;
        node->size_cells = 1;
      } else {
        node->address_cells = stack[depth - 1U].address_cells;
        node->size_cells = stack[depth - 1U].size_cells;
      }
      depth++;
    } else if (token == FDT_PROP) {
      if (depth == 0 || !section_valid(offset, 8U,
              current_fdt.structure_size))
        return false;

      uint32_t length = read_be32(structure + offset);
      uint32_t name_offset = read_be32(structure + offset + 4U);
      offset += 8U;
      if (!section_valid(offset, length, current_fdt.structure_size))
        return false;

      const volatile uint8_t *name;
      uint32_t name_length;
      if (!property_name(strings, current_fdt.strings_size,
              name_offset, &name, &name_length))
        return false;

      dma_node_t *node = &stack[depth - 1U];
      fdt_property_t property = {
        .data = structure + offset,
        .length = length
      };
      if (name_equals(name, name_length, "compatible")) {
        if (property_has_string(property, "brcm,bcm2711-pcie"))
          node->compatible = true;
      } else if (name_equals(name, name_length, "dma-ranges")) {
        node->dma = property;
        node->has_dma = true;
      } else if (name_equals(name, name_length, "brcm,scb-sizes")) {
        node->scb = property;
        node->has_scb = true;
      } else if (length == 4U &&
          name_equals(name, name_length, "#address-cells")) {
        node->address_cells = read_be32(property.data);
      } else if (length == 4U &&
          name_equals(name, name_length, "#size-cells")) {
        node->size_cells = read_be32(property.data);
      }
      if (!advance_aligned(&offset, length,
              current_fdt.structure_size))
        return false;
    } else if (token == FDT_END_NODE) {
      if (depth == 0)
        return false;
      dma_node_t *node = &stack[depth - 1U];
      if (node->compatible) {
        if (!node->has_dma || depth < 2)
          return false;
        result->child_address_cells = node->address_cells;
        result->parent_address_cells = stack[depth - 2U].address_cells;
        result->size_cells = node->size_cells;
        if (!copy_property_cells( node->dma, result->dma_cells,
                FDT_MAX_DMA_CELLS, &result->dma_cell_count))
          return false;
        if (node->has_scb &&
            !copy_property_cells( node->scb,
                result->scb_cells, 8U, &result->scb_cell_count))
          return false;
        return true;
      }
      depth--;
    } else if (token == FDT_NOP) {
      continue;
    } else if (token == FDT_END) {
      break;
    } else {
      return false;
    }
  }

  return false;
}
