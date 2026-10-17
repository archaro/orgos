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

// Create a level-1 table descriptor pointing to
// a level-2 translation table.
static uint64_t table_descriptor(const uint64_t *table) {
  return ((uint64_t)(uintptr_t)table & ~0xfffULL) | DESC_VALID | DESC_TABLE;
}

// Create a level-2 block descriptor.
// Physical address must be 2 MiB aligned.
// Device memory must never be executable.
// We also mark ordinary RAM UXN, but initially
// leave PXN clear because the kernel executes
// from low RAM.
// We'll introduce page-level code permissions
// when we separate executable and writable RAM.
static uint64_t block_descriptor(uint64_t address, unsigned int attribute, bool device) {
  uint64_t descriptor = (address & ~(MMU_BLOCK_SIZE - 1ULL)) | DESC_VALID |
      DESC_AF | DESC_ATTR(attribute) | DESC_UXN;

  if (device) {
    descriptor |= DESC_PXN;
  } else {
    descriptor |= DESC_SH_INNER;
  }

  return descriptor;
}

// Populate one 2 MiB identity-mapped entry.
// The index is relative to the 1 GiB region represented by the parent level-1 entry.
static void map_block(uint64_t *table, uint64_t physical_address, unsigned int attribute, bool device) {
  unsigned int index = (unsigned int)((physical_address >> 21) & 0x1ffU);

  table[index] = block_descriptor( physical_address, attribute, device);
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


// Assembly routine responsible for changing the
// EL1 system registers and enabling translation.
extern void mmu_enable_el1( uint64_t translation_table, uint64_t mair, uint64_t tcr);
extern char __kernel_start[];
extern char __kernel_end[];

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


