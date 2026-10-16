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

// Page-table storage.
// GCC's alignment attribute guarantees that every
// table begins on a 4 KiB boundary.
// Each table contains 512 64-bit descriptors.
static uint64_t l1_table[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_ram[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_peripherals[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
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

// Fill our initial identity map.
// All tables begin zeroed. A zero descriptor means the address is not mapped.
static void build_initial_tables(void) {
  for (unsigned int i = 0; i < MMU_TABLE_ENTRIES; i++) {
    l1_table[i] = 0;
    l2_ram[i] = 0;
    l2_peripherals[i] = 0;
    l2_pcie[i] = 0;
  }

  // Low 512 MiB: normal, non-cacheable.
  // This includes our kernel at 0x80000, its exception vectors, stack, and tables.
  // The mapping does not grant ownership of all this memory to our kernel allocator.
  for (uint64_t address = 0; address < 0x20000000ULL; address += MMU_BLOCK_SIZE) {
    map_block(l2_ram, address, ATTR_NORMAL_NC, false);
  }

  // BCM2711 peripheral area:
  // 0xFC000000 - 0xFFFFFFFF
  // This includes the PCIe bridge, UART, firmware mailbox, system timer and GIC.
  for (uint64_t address = 0xfc000000ULL; address < 0x100000000ULL; address += MMU_BLOCK_SIZE) {
    map_block(l2_peripherals, address, ATTR_DEVICE, true);
  }

  // VL805 xHCI outbound PCIe MMIO aperture:
  // 0x600000000 - 0x603FFFFFF
  // Exactly 64 MiB.
  for (uint64_t address = 0x600000000ULL; address < 0x604000000ULL; address += MMU_BLOCK_SIZE) {
    map_block(l2_pcie, address, ATTR_DEVICE, true);
  }

  // Install level-1 entries.
  // L1[0]  = 0-1 GiB region containing low RAM
  // L1[3]  = 3-4 GiB region containing peripherals
  // L1[24] = 24-25 GiB region containing VL805 MMIO
  l1_table[0] = table_descriptor(l2_ram);
  l1_table[3] = table_descriptor(l2_peripherals);
  l1_table[24] = table_descriptor(l2_pcie);
}

// Assembly routine responsible for changing the
// EL1 system registers and enabling translation.
extern void mmu_enable_el1( uint64_t translation_table, uint64_t mair, uint64_t tcr);

bool mmu_init(void) {
  if (mmu_enabled())
    return true;

  if (!mmu_granule_4k_supported())
    return false;

  // We need at least 36 physical address bits.
  // This Pi reports 44.
  if (mmu_physical_address_bits() < 36)
    return false;

  build_initial_tables();

  // MAIR_EL1 attribute encodings:
  // Index 0: Device-nGnRnE = 0x00
  // Index 1: Normal non-cacheable = 0x44
  // Index 2: Normal WB/WA cacheable = 0xFF
  uint64_t mair = (0x00ULL << 0) | (0x44ULL << 8) | (0xffULL << 16);

  // TCR_EL1:
  // T0SZ = 28 -> 36-bit TTBR0 address space
  // IRGN0 = 0  -> Non-cacheable table walks
  // ORGN0 = 0  -> Non-cacheable table walks
  // SH0 = 3    -> Inner-shareable
  // TG0 = 0    -> 4 KiB granule
  // EPD1 = 1   -> Disable TTBR1 walks
  // IPS = 1    -> 36-bit physical address output
  // Although the processor supports 44-bit
  // physical addresses, our current mapped
  // physical regions fit within 36 bits.
  uint64_t tcr = 28ULL | (3ULL << 12) | (1ULL << 23) | (1ULL << 32);

  mmu_enable_el1((uint64_t)(uintptr_t)l1_table,
      mair, tcr);

  return mmu_enabled();
}

