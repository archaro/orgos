#include <stdint.h>
#include <stdbool.h>

#include "mmu.h"

// Read the AArch64 memory-model feature register.
// ID_AA64MMFR0_EL1 describes, among other things:
//   Bits [3:0]   PARange  Physical address range
//   Bits [31:28] TGran4  4 KiB translation support
// This is a read-only architectural identification
// register, not an MMU configuration register.
static uint64_t read_mmfr0(void) {
  uint64_t value;

  __asm__ volatile(
      "mrs %0, id_aa64mmfr0_el1"
      : "=r"(value));
  return value;
}

// Collect the current EL1 translation settings.
// Reading these registers does not enable translation
// or otherwise change processor state.
void mmu_get_info(mmu_info_t *info) {
  if (!info)
    return;

  info->id_aa64mmfr0 = read_mmfr0();
  __asm__ volatile(
      "mrs %0, sctlr_el1"
      : "=r"(info->sctlr));
  __asm__ volatile(
      "mrs %0, tcr_el1"
      : "=r"(info->tcr));
  __asm__ volatile(
      "mrs %0, mair_el1"
      : "=r"(info->mair));
  __asm__ volatile(
      "mrs %0, ttbr0_el1"
      : "=r"(info->ttbr0));
  __asm__ volatile(
      "mrs %0, ttbr1_el1"
      : "=r"(info->ttbr1));
}

// SCTLR_EL1.M, bit 0, enables stage-1 translation
// for EL1 and EL0.
bool mmu_enabled(void) {
  uint64_t sctlr;

  __asm__ volatile(
      "mrs %0, sctlr_el1"
      : "=r"(sctlr));
  return (sctlr & 1U) != 0;
}

// For TGran4:
//   0x0 = 4 KiB granule supported
//   0xf = 4 KiB granule not supported
// Other encodings are reserved for this register.
bool mmu_granule_4k_supported(void) {
  uint64_t field = (read_mmfr0() >> 28) & 0xfU;
  return field == 0;
}

// Decode the PARange field.
// These are architectural encodings of the maximum
// supported physical address size.
unsigned int mmu_physical_address_bits(void) {
  static const unsigned int bits[] = {32, 36, 40, 42, 44, 48, 52};
  unsigned int parange = (unsigned int)(read_mmfr0() & 0xfU);

  if (parange >= sizeof(bits) / sizeof(bits[0]))
    return 0;

  return bits[parange];
}

static uint64_t mapped_ram_bytes;

uint64_t mmu_ram_mapped_bytes(void) {
  return mapped_ram_bytes;
}

// Initial OrgOS EL1 identity map.
// Translation granule: 4 KiB
// Virtual address size: 36 bits
// Translation start: level 1
// Level 1 entries cover 1 GiB.
// Level 2 block entries cover 2 MiB.
// We deliberately map only regions required for
// the existing kernel and hardware diagnostics.

#define MMU_TABLE_ENTRIES 512U
#define MMU_BLOCK_SIZE   (2ULL * 1024ULL * 1024ULL)
#define MMU_GIB          (1024ULL * 1024ULL * 1024ULL)

// Translation-table descriptor bits.
// Bit 0      Valid
// Bit 1      Table (at L1) / block (at L2)
// Bits 4:2   MAIR attribute index
// Bits 7:6   Access permissions (00 = EL1 read/write)
// Bits 9:8   Shareability
// Bit 10     Access flag
// Bit 53     Privileged execute-never
// Bit 54     Unprivileged execute-never
#define DESC_VALID      (1ULL << 0)
#define DESC_TABLE      (1ULL << 1)
#define DESC_AF         (1ULL << 10)
#define DESC_SH_INNER   (3ULL << 8)
#define DESC_PXN        (1ULL << 53)
#define DESC_UXN        (1ULL << 54)

#define ATTR_DEVICE     0U
#define ATTR_NORMAL_NC  1U
#define ATTR_NORMAL_WB  2U

#define DESC_ATTR(index) ((uint64_t)(index) << 2)

// AP[2:1] = 10:
//   EL1 read-only
//   EL0 no access
#define DESC_AP_RO (2ULL << 6)

// A 36-bit virtual address space provides 64
// level-1 regions, each covering 1 GiB.
// Our physical RAM occupies the first 8 GiB,
// so eight level-2 tables are sufficient.
// Region 3 also contains the BCM2711 MMIO
// registers. Those entries receive Device
// attributes rather than Normal attributes.
#define MMU_RAM_L1_REGIONS 8U
static uint64_t l1_table[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_ram[MMU_RAM_L1_REGIONS][MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_pcie[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
#define MMU_PAGE_SIZE 4096ULL
// Level-3 page table for the first 2 MiB.
// 512 pages × 4 KiB = 2 MiB.
static uint64_t l3_kernel[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));

// Create a level-1 table descriptor pointing to
// a level-2 translation table.
static uint64_t table_descriptor(const uint64_t *table) {
  return ((uint64_t)(uintptr_t)table & ~0xfffULL) | DESC_VALID | DESC_TABLE;
}

// Create a level-2 block descriptor.
// All block mappings are non-executable at
// both EL1 and EL0.
// Only explicitly protected kernel pages
// may contain executable instructions.
static uint64_t block_descriptor(uint64_t address, unsigned int attribute, bool device) {
  uint64_t descriptor =
      (address & ~(MMU_BLOCK_SIZE - 1ULL)) | DESC_VALID | DESC_AF |
      DESC_ATTR(attribute) | DESC_PXN | DESC_UXN;
  if (!device)
    descriptor |= DESC_SH_INNER;
  return descriptor;
}

// Construct a level-3 page descriptor.
// Level-3 descriptors use bits [1:0] = 11.
// Physical addresses must be 4 KiB aligned.
static uint64_t page_descriptor(uint64_t address, bool read_only, bool executable) {
  uint64_t descriptor =
      (address & ~(MMU_PAGE_SIZE - 1ULL)) | DESC_VALID | DESC_TABLE |
      DESC_AF | DESC_SH_INNER | DESC_ATTR(ATTR_NORMAL_NC) | DESC_UXN;
  if (read_only)
    descriptor |= DESC_AP_RO;
  if (!executable)
    descriptor |= DESC_PXN;
  return descriptor;
}

// Populate one 2 MiB identity-mapped entry.
// The index is relative to the 1 GiB region represented by the parent level-1 entry.
static void map_block(uint64_t *table, uint64_t physical_address, unsigned int attribute, bool device) {
  unsigned int index = (unsigned int)((physical_address >> 21) & 0x1ffU);

  table[index] = block_descriptor( physical_address, attribute, device);
}

// Assembly routine responsible for changing the
// EL1 system registers and enabling translation.
extern void mmu_enable_el1( uint64_t translation_table, uint64_t mair, uint64_t tcr);
extern char __kernel_start[];
extern char __kernel_end[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];

// Check whether a physical address belongs to
// the half-open interval [start, end).
static bool address_in_range(uint64_t address, uint64_t start, uint64_t end) {
  return address >= start && address < end;
}

// Replace the first 2 MiB RAM block with
// 512 individual 4 KiB page descriptors.
// Executable pages are read-only.
// Writable pages are execute-never.
static bool protect_kernel_pages(void) {
  uint64_t text_start = (uint64_t)(uintptr_t)__text_start;
  uint64_t text_end = (uint64_t)(uintptr_t)__text_end;
  uint64_t rodata_start = (uint64_t)(uintptr_t)__rodata_start;
  uint64_t rodata_end = (uint64_t)(uintptr_t)__rodata_end;
  uint64_t data_start = (uint64_t)(uintptr_t)__data_start;
  uint64_t data_end = (uint64_t)(uintptr_t)__data_end;
  uint64_t bss_start = (uint64_t)(uintptr_t)__bss_start;
  uint64_t bss_end = (uint64_t)(uintptr_t)__bss_end;
  uint64_t stack_start = (uint64_t)(uintptr_t)__stack_bottom;
  uint64_t stack_end = (uint64_t)(uintptr_t)__stack_top;

  // All section boundaries must be page-aligned.
  const uint64_t boundaries[] = {
    text_start, text_end,
    rodata_start, rodata_end,
    data_start, data_end,
    bss_start, bss_end,
    stack_start, stack_end
  };

  for (unsigned int i = 0;
       i < sizeof(boundaries) / sizeof(boundaries[0]);
       i++) {
    if (boundaries[i] & (MMU_PAGE_SIZE - 1ULL))
      return false;
  }

  // All protected sections must fit inside
  // our first 2 MiB page table.
  if (text_start < 0x80000ULL ||
      text_end > MMU_BLOCK_SIZE ||
      stack_end > MMU_BLOCK_SIZE)
    return false;

  // Validate the section ordering.
  if (text_start > text_end ||
      text_end > rodata_start ||
      rodata_start > rodata_end ||
      rodata_end > data_start ||
      data_start > data_end ||
      data_end > bss_start ||
      bss_start > bss_end ||
      bss_end > stack_start ||
      stack_start > stack_end)
    return false;

  // Populate every page in the first 2 MiB.
  // Non-kernel pages remain writable but
  // cannot execute code.
  for (unsigned int i = 0; i < MMU_TABLE_ENTRIES; i++) {
    uint64_t address = (uint64_t)i * MMU_PAGE_SIZE;
    bool read_only = false;
    bool executable = false;
    if (address_in_range(address, text_start, text_end)) {
      read_only = true;
      executable = true;
    } else if (address_in_range(address, rodata_start, rodata_end)) {
      read_only = true;
    }
    l3_kernel[i] = page_descriptor(address, read_only, executable);
  }
  // Replace the first 2 MiB block descriptor
  // with a level-2 table descriptor.
  // L2[0][0] points to the level-3 table.
  l2_ram[0][0] = table_descriptor(l3_kernel);
  return true;
}

// Construct identity mappings for all RAM banks
// discovered through the firmware Device Tree.
// Every RAM bank must fit inside our initial
// 8 GiB physical-RAM mapping area.
// This implementation uses 2 MiB blocks,
// so bank bases and sizes must be aligned
// to 2 MiB boundaries.
// No memory is allocated or modified here:
// we are only constructing translations.
static bool build_initial_tables(const physmem_map_t *memory)
{
  if (!memory || memory->bank_count == 0)
    return false;

  // Start with every descriptor invalid.
  for (unsigned int i = 0; i < MMU_TABLE_ENTRIES; i++) {
    l1_table[i] = 0;
    l2_pcie[i] = 0;
  }

  for (unsigned int region = 0; region < MMU_RAM_L1_REGIONS; region++) {
    for (unsigned int i = 0; i < MMU_TABLE_ENTRIES; i++) {
      l2_ram[region][i] = 0;
    }
  }

  // Each level-1 entry selects the corresponding
  // 1 GiB region's level-2 table.
  // Unpopulated level-2 entries remain invalid.
  for (unsigned int region = 0; region < MMU_RAM_L1_REGIONS; region++) {
    l1_table[region] = table_descriptor(l2_ram[region]);
  }

  mapped_ram_bytes = 0;
  // Populate the RAM blocks.
  for (unsigned int bank = 0; bank < memory->bank_count; bank++) {
    uint64_t start = memory->banks[bank].address;
    uint64_t size = memory->banks[bank].size;
    if (size == 0)
      return false;
    if ((start & (MMU_BLOCK_SIZE - 1ULL)) != 0 || (size & (MMU_BLOCK_SIZE - 1ULL)) != 0)
      return false;
    if (size > UINT64_MAX - start)
      return false;
    uint64_t end = start + size;
    if (end > 8ULL * MMU_GIB)
      return false;
    if (size > UINT64_MAX - mapped_ram_bytes)
      return false;
    for (uint64_t address = start; address < end; address += MMU_BLOCK_SIZE) {
      unsigned int region = (unsigned int)(address >> 30);
      unsigned int index = (unsigned int)((address >> 21) & 0x1ffU);
      // Reject overlapping RAM banks.
      if (l2_ram[region][index] & DESC_VALID)
        return false;
      l2_ram[region][index] = block_descriptor(address, ATTR_NORMAL_NC, false);
    }
    mapped_ram_bytes += size;
  }

  // BCM2711 peripheral MMIO occupies:
  //   0xFC000000 - 0xFFFFFFFF
  // It shares level-1 region 3 with the
  // upper part of RAM Bank 1.
  for (uint64_t address = 0xfc000000ULL; address < 0x100000000ULL; address += MMU_BLOCK_SIZE) {
    unsigned int index = (unsigned int)((address >> 21) & 0x1ffU);
    // Never overwrite a RAM descriptor with a
    // Device descriptor. Such an overlap means
    // our memory descriptions are inconsistent.
    if (l2_ram[3][index] & DESC_VALID)
      return false;
    l2_ram[3][index] = block_descriptor(address, ATTR_DEVICE, true);
  }

  // VL805 PCIe outbound MMIO window.
  //   0x600000000 - 0x603FFFFFF
  // This is level-1 region 24, outside
  // the physical RAM area.
  for (uint64_t address = 0x600000000ULL; address < 0x604000000ULL; address += MMU_BLOCK_SIZE) {
    map_block(l2_pcie, address, ATTR_DEVICE, true);
  }
  l1_table[24] = table_descriptor(l2_pcie);
  // Replace the first low-RAM block with
  // page-level kernel protection.
  // The tables are still inactive here,
  // so no TLB maintenance is required.
  if (!protect_kernel_pages())
    return false;

  return true;
}

// Ask the AArch64 MMU to translate a virtual
// address for an EL1 privileged read.
// AT S1E1R performs stage-1 translation at EL1.
// It does not read the target memory.
// PAR_EL1 contains either:
//   F = 0: Successful translation.
//   F = 1: Translation or permission failure.
// On success, PAR_EL1[51:12] contains the
// resulting physical address, aligned to 4 KiB.
// The original page offset is added to produce
// the complete translated address.
// On failure, PAR_EL1[6:1] contains the fault
// status code.
bool mmu_translate(uint64_t address, mmu_translation_t *result) {
  if (!result || !mmu_enabled())
    return false;

  uint64_t par;
  // Ensure the translation instruction completes
  // before reading PAR_EL1.
  // The memory clobber prevents compiler reordering
  // across the inline assembly sequence.
  __asm__ volatile( "at s1e1r, %1\n" "isb\n" "mrs %0, par_el1\n"
      : "=r"(par) : "r"(address) : "memory");
  result->virtual_address = address;
  result->par = par;
  result->fault = (par & 1ULL) != 0;
  result->fault_status = 0;
  result->physical_address = 0;
  if (result->fault) {
    result->fault_status =
        (uint8_t)((par >> 1) & 0x3fU);
  } else {
    result->physical_address =
        (par & 0x000FFFFFFFFFF000ULL) |
        (address & 0xfffULL);
  }
  return true;
}

// Perform a privileged EL1 write translation.
// This checks whether the page would permit
// a write; it does not actually modify memory.
bool mmu_translate_write(uint64_t address, mmu_translation_t *result) {
  if (!result || !mmu_enabled())
    return false;

  uint64_t par;
  __asm__ volatile(
      "at s1e1w, %1\n"
      "isb\n"
      "mrs %0, par_el1\n"
      : "=r"(par)
      : "r"(address)
      : "memory");

  result->virtual_address = address;
  result->par = par;
  result->fault = (par & 1ULL) != 0;
  result->fault_status = 0;
  result->physical_address = 0;
  if (result->fault) {
    result->fault_status = (uint8_t)((par >> 1) & 0x3fU);
  } else {
    result->physical_address =
        (par & 0x000FFFFFFFFFF000ULL) | (address & 0xfffULL);
  }
  return true;
}

// Inspect the stage-1 translation descriptors
// installed by OrgOS.
// Current assumptions:
//   - TTBR0 only
//   - 36-bit virtual addresses
//   - 4 KiB translation granule
//   - Level-1 starting table
//   - Level-2 blocks or level-3 pages
// No access is made to the target VA.
bool mmu_inspect(uint64_t address, mmu_descriptor_info_t *info) {
  if (!info || !mmu_enabled())
    return false;

  // Reject addresses outside our configured
  // 36-bit TTBR0 virtual-address space.
  if (address >= (1ULL << 36))
    return false;

  *info = (mmu_descriptor_info_t){0};
  info->virtual_address = address;
  info->kind = MMU_DESC_UNMAPPED;

  unsigned int l1_index = (unsigned int)((address >> 30) & 0x3fU);
  unsigned int l2_index = (unsigned int)((address >> 21) & 0x1ffU);
  unsigned int l3_index = (unsigned int)((address >> 12) & 0x1ffU);
  uint64_t l1 = l1_table[l1_index];
  info->level = 1;
  info->l1_descriptor = l1;

  if (!(l1 & DESC_VALID))
    return true;

  // OrgOS currently installs L1 table
  // descriptors only, never L1 blocks.
  if ((l1 & DESC_TABLE) == 0)
    return false;

  uint64_t *l2;
  if (l1_index < MMU_RAM_L1_REGIONS) {
    l2 = l2_ram[l1_index];
  } else if (l1_index == 24U) {
    l2 = l2_pcie;
  } else {
    return false;
  }

  // Confirm the L1 descriptor actually
  // points to the expected static table.
  if ((l1 & 0x0000FFFFFFFFF000ULL) != (uint64_t)(uintptr_t)l2)
    return false;

  uint64_t l2_entry = l2[l2_index];
  info->level = 2;
  info->l2_descriptor = l2_entry;

  if (!(l2_entry & DESC_VALID))
    return true;

  uint64_t leaf;
  uint64_t mapping_size;
  if (l2_entry & DESC_TABLE) {
    // At present the only L3 table covers
    // the first 2 MiB of physical RAM.
    if (l1_index != 0 || l2_index != 0)
      return false;

    if ((l2_entry & 0x0000FFFFFFFFF000ULL) != (uint64_t)(uintptr_t)l3_kernel)
      return false;
    uint64_t l3_entry = l3_kernel[l3_index];
    info->level = 3;
    info->l3_descriptor = l3_entry;
    if (!(l3_entry & DESC_VALID))
      return true;
    if (!(l3_entry & DESC_TABLE))
      return false;
    info->kind = MMU_DESC_PAGE;
    leaf = l3_entry;
    mapping_size = MMU_PAGE_SIZE;
  } else {
    info->kind = MMU_DESC_BLOCK;
    leaf = l2_entry;
    mapping_size = MMU_BLOCK_SIZE;
  }
  info->leaf_descriptor = leaf;
  info->mapping_size = mapping_size;

  // For our 36-bit output physical addresses,
  // extract the descriptor's address field
  // at the appropriate mapping granularity.
  uint64_t base = leaf & 0x0000000FFFFFFFFFULL;
  base &= ~(mapping_size - 1ULL);
  info->physical_address = base | (address & (mapping_size - 1ULL));
  info->attr_index = (unsigned int)((leaf >> 2) & 7U);
  info->access_permissions = (unsigned int)((leaf >> 6) & 3U);
  // AP[2] is set for read-only mappings.
  info->read_only = (info->access_permissions & 2U) != 0;
  info->access_flag = (leaf & DESC_AF) != 0;
  info->pxn = (leaf & DESC_PXN) != 0;
  info->uxn = (leaf & DESC_UXN) != 0;
  return true;
}

bool mmu_init(const physmem_map_t *memory) {
  if (mmu_enabled())
    return true;

  if (!memory || memory->bank_count == 0)
    return false;

  if (!mmu_granule_4k_supported())
    return false;

  if (mmu_physical_address_bits() < 36)
    return false;

  // Confirm that the entire kernel, including
  // its stack, relocated FDT and page tables,
  // lies inside one firmware-described RAM bank.
  uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
  uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_end;
  bool kernel_covered = false;
  for (unsigned int i = 0; i < memory->bank_count; i++) {
    uint64_t start = memory->banks[i].address;
    uint64_t size = memory->banks[i].size;
    if (size > UINT64_MAX - start)
      return false;
    uint64_t end = start + size;
    if (kernel_start >= start && kernel_end <= end) {
      kernel_covered = true;
      break;
    }
  }

  if (!kernel_covered)
    return false;

  if (!build_initial_tables(memory))
    return false;

  // MAIR:
  // Attribute 0: Device-nGnRnE
  // Attribute 1: Normal, non-cacheable
  // Attribute 2: Normal, write-back cacheable
  // We continue using non-cacheable RAM
  // during this stage of development.
  uint64_t mair = (0x00ULL << 0) | (0x44ULL << 8) | (0xffULL << 16);

  // TCR:
  // T0SZ  = 28: 36-bit virtual addresses
  // SH0   = 3:  inner-shareable
  // EPD1  = 1:  disable TTBR1 walks
  // IPS   = 1:  36-bit physical addresses
  // Translation-table walks remain non-cacheable.
  uint64_t tcr = 28ULL | (3ULL << 12) | (1ULL << 23) | (1ULL << 32);

  mmu_enable_el1( (uint64_t)(uintptr_t)l1_table, mair, tcr);
  return mmu_enabled();
}


