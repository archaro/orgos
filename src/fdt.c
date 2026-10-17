
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

