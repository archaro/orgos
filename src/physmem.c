#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "physmem.h"
#include "fdt.h"

// Linker-defined bounds of OrgOS.
// These enclose kernel code, static data,
// .bss, our FDT copy, translation tables
// and the initial 16 KiB stack.
extern char __kernel_start[];
extern char __kernel_end[];

static physmem_map_t physical_memory;
static bool physical_memory_ready;

// Add an explicitly reserved physical range.
// Ignore zero-length reservations, but reject
// arithmetic overflow and capacity exhaustion.
static bool reserve_range(uint64_t address, uint64_t size, physmem_reservation_kind_t kind) {
  if (size == 0)
    return true;

  if (size > UINT64_MAX - address)
    return false;

  if (physical_memory.reservation_count >= PHYSMEM_MAX_RESERVATIONS)
    return false;

  physmem_reservation_t *entry =
      &physical_memory.reservations[ physical_memory.reservation_count++];

  entry->address = address;
  entry->size = size;
  entry->kind = kind;

  return true;
}

bool physmem_init(void) {
  physical_memory_ready = false;

  // Use static storage rather than allocating this
  // temporary FDT report on the kernel stack.
  static fdt_memory_map_t discovered;
  if (!fdt_get_memory_map(&discovered))
    return false;

  physical_memory.bank_count = 0;
  physical_memory.reservation_count = 0;
  physical_memory.described_ram_bytes = 0;

  // Copy all firmware-described RAM banks.
  // Check sizes and totals before accepting them.
  for (unsigned int i = 0; i < discovered.bank_count; i++) {
    fdt_memory_range_t range = discovered.banks[i];

    if (range.size > UINT64_MAX - range.address)
      return false;

    if (range.size > UINT64_MAX - physical_memory.described_ram_bytes)
      return false;

    if (physical_memory.bank_count >= FDT_MAX_MEMORY_BANKS)
      return false;
    physical_memory.banks[physical_memory.bank_count++] = range;
    physical_memory.described_ram_bytes += range.size;
  }

  // Preserve both kinds of firmware reservation.
  // Some may overlap, and some may lie outside
  // the RAM banks described in /memory.
  // We deliberately do not subtract their sizes.
  for (unsigned int i = 0;
       i < discovered.reservation_count; i++) {
    const fdt_reservation_t *entry = &discovered.reservations[i];
    physmem_reservation_kind_t kind =
        entry->kind == FDT_RESERVE_HEADER ?
        PHYSMEM_RESERVED_FDT_HEADER :
        PHYSMEM_RESERVED_FDT_NODE;

    if (!reserve_range(entry->range.address,
            entry->range.size, kind))
      return false;
  }

  // Reserve our entire kernel image and static
  // storage, including the private FDT buffer.
  uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
  uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_end;
  if (kernel_end <= kernel_start)
    return false;

  if (!reserve_range(kernel_start,
          kernel_end - kernel_start,
          PHYSMEM_RESERVED_KERNEL))
    return false;

  // Be conservative about the firmware's original
  // FDT storage. Although we now own a copy, keep
  // the original location reserved for the moment.
  fdt_info_t fdt;
  if (!fdt_get_info(&fdt))
    return false;

  uint64_t original = fdt_original_address();
  if (original == 0)
    return false;

  if (!reserve_range(original, fdt.total_size,
          PHYSMEM_RESERVED_FDT_ORIGINAL))
    return false;

  // Keep the first 4 KiB unavailable. This is
  // already described in the firmware's header
  // reservation, but recording our own policy
  // makes it explicit and does not hurt.
  if (!reserve_range(0, 4096,
          PHYSMEM_RESERVED_LOW_PAGE))
    return false;

  physical_memory_ready = true;
  return true;
}

const physmem_map_t *physmem_get_map(void) {
  return physical_memory_ready ?  &physical_memory : NULL;
}

