#include <stdint.h>
#include <stdbool.h>

#include "mmu.h"
#include "mmu_boot.h"
#include "mmu_high.h"
#include "mmu_activation.h"
#include "cache.h"

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

// Boot owns the same eleven static pages for the kernel's lifetime.
// Only this compatibility boundary converts CPU pointers to physical
// table addresses. It relies on the current identity map.
static uint64_t l1_table[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_ram[MMU_BOOT_RAM_REGIONS][MMU_TABLE_ENTRIES]
    __attribute__((aligned(4096)));
static uint64_t l2_pcie[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t l3_kernel[MMU_TABLE_ENTRIES] __attribute__((aligned(4096)));
static mmu_table_ref_t boot_tables[MMU_BOOT_TABLE_COUNT];
static mmu_address_space_t kernel_space;
static uint64_t context_pages[2][MMU_CONTEXT_TABLE_COUNT][MMU_TABLE_ENTRIES]
    __attribute__((aligned(4096)));
static mmu_table_ref_t context_tables[2][MMU_CONTEXT_TABLE_COUNT];
static mmu_address_space_t context_spaces[2];
static mmu_activation_t environments;
static mmu_boot_layout_t kernel_layout;
// Independently owned, never registered with the TTBR0 controller.
static uint64_t high_pages[MMU_BOOT_TABLE_COUNT][MMU_TABLE_ENTRIES]
    __attribute__((aligned(4096)));
static mmu_table_ref_t high_tables[MMU_BOOT_TABLE_COUNT];
static mmu_address_space_t high_space;

extern void mmu_enable_el1(paddr_t root, uint64_t mair, uint64_t tcr);
extern char __kernel_start[];
extern char __kernel_end[];
extern char __text_start[];
extern char __text_end[];
extern char __el0_text_start[];
extern char __el0_text_end[];
extern char __el0_stack_bottom[];
extern char __el0_stack_top[];
extern char __el0_stack1_bottom[], __el0_stack1_top[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];

static mmu_boot_range_t linker_range(const char *start, const char *end) {
  return (mmu_boot_range_t){(uintptr_t)start, (uintptr_t)end};
}

static bool build_initial_tables(const physmem_map_t *memory) {
  boot_tables[0] = (mmu_table_ref_t){l1_table, (uintptr_t)l1_table, 1};
  for (unsigned int region = 0; region < MMU_BOOT_RAM_REGIONS; region++)
    boot_tables[region + 1U] = (mmu_table_ref_t){
      l2_ram[region], (uintptr_t)l2_ram[region], 2
    };
  boot_tables[9] = (mmu_table_ref_t){l2_pcie, (uintptr_t)l2_pcie, 2};
  boot_tables[10] = (mmu_table_ref_t){l3_kernel, (uintptr_t)l3_kernel, 3};
  kernel_layout = (mmu_boot_layout_t){
    .kernel = linker_range(__kernel_start, __kernel_end),
    .text = linker_range(__text_start, __text_end),
    .el0_text = linker_range(__el0_text_start, __el0_text_end),
    .rodata = linker_range(__rodata_start, __rodata_end),
    .data = linker_range(__data_start, __data_end),
    .bss = linker_range(__bss_start, __bss_end),
    .stack = linker_range(__stack_bottom, __stack_top),
    .el0_stack = {
      linker_range(__el0_stack_bottom, __el0_stack_top),
      linker_range(__el0_stack1_bottom, __el0_stack1_top)
    }
  };
  if (!mmu_boot_build(&kernel_space, boot_tables, memory, &kernel_layout,
                       &mapped_ram_bytes))
    return false;
  for (unsigned int id = 0; id < 2U; id++) {
    for (unsigned int t = 0; t < MMU_CONTEXT_TABLE_COUNT; t++) {
      uint64_t *entries = context_pages[id][t];
      context_tables[id][t] = (mmu_table_ref_t){
        entries, (uintptr_t)entries,
        t == 0U ? 1U : (t == 10U || t == 12U) ? 3U : 2U
      };
    }
    if (!mmu_context_build(&context_spaces[id], context_tables[id], memory,
                            &kernel_layout, id))
      return false;
  }
  for (unsigned int t = 0; t < MMU_BOOT_TABLE_COUNT; t++) {
    uint64_t *entries = high_pages[t];
    high_tables[t] = (mmu_table_ref_t){
      entries, (uintptr_t)entries, t == 0U ? 1U : t == 10U ? 3U : 2U
    };
  }
  return mmu_high_build(&high_space, high_tables, memory, &kernel_layout);
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

// Compatible active-regime diagnostic. The underlying walker accepts an
// explicit space and resolves table PAs through its registered CPU pointers.
bool mmu_inspect(uint64_t address, mmu_descriptor_info_t *info) {
  return mmu_enabled() && environments.active &&
      mmu_space_inspect(environments.active, address, info);
}

bool mmu_init(const physmem_map_t *memory) {
  if (mmu_enabled())
    return true;

  if (!memory || memory->bank_count == 0 ||
      memory->bank_count > FDT_MAX_MEMORY_BANKS)
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

  // Initialize the local data-cache hierarchy
  // before constructing the final translation
  // tables and enabling write-back caching.
  // This must occur with SCTLR_EL1.C clear.
  if (!cache_init_data())
    return false;

  if (!build_initial_tables(memory))
    return false;

  // Preserve the existing MAIR/TCR and assembly barrier/cache sequence.
  // This is the existing boot enablement, not a live switching API.
  mmu_enable_el1(kernel_space.root_physical, kernel_space.mair,
                 kernel_space.tcr);
  if (!mmu_enabled())
    return false;
  kernel_space.state = MMU_SPACE_ACTIVE;
  return mmu_activation_init(&environments, &kernel_space, &context_spaces[0],
                              &context_spaces[1], &kernel_layout);
}

uint64_t mmu_activation_lock(void) {
  uint64_t daif;
  __asm__ volatile("mrs %0, daif\nmsr daifset, #0xf"
                   : "=r"(daif) : : "memory");
  return daif;
}

void mmu_activation_unlock(uint64_t daif) {
  __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}

paddr_t mmu_current_root(void) {
  uint64_t ttbr;
  __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
  return ttbr; // ASID and CnP must both remain zero in this experiment.
}

bool mmu_activation_hw_matches(paddr_t root) {
  mmu_info_t info;
  mmu_get_info(&info);
  const uint64_t required = 1ULL | 4ULL | (1ULL << 12) | (1ULL << 19);
  uint64_t sp, pc, vectors;
  __asm__ volatile("mov %0, sp\nadr %1, .\nmrs %2, vbar_el1"
                   : "=r"(sp), "=r"(pc), "=r"(vectors));
  // This experiment admits only the linked kernel's EL1 stack and code.
  // Compatibility validation covers every page of this image, including
  // all static data/tables; check the actual execution anchors as well.
  return info.ttbr0 == root && info.mair == MMU_MAIR_DEVICE_NC_WB &&
      info.tcr == MMU_TCR_4K_36BIT && (info.sctlr & required) == required &&
      sp >= (uintptr_t)__stack_bottom && sp <= (uintptr_t)__stack_top &&
      !(sp & 15U) && pc >= (uintptr_t)__text_start &&
      pc < (uintptr_t)__text_end && !(vectors & 2047U) &&
      vectors >= (uintptr_t)__text_start &&
      vectors <= (uintptr_t)__text_end - 2048U;
}

const mmu_address_space_t *mmu_kernel_address_space(void) {
  return environments.active ? &kernel_space : NULL;
}

const mmu_address_space_t *mmu_context_address_space(unsigned int id) {
  return environments.active && id < 2U ? &context_spaces[id] : NULL;
}

bool mmu_activate_address_space(const mmu_address_space_t *space) {
  // Reject unregistered objects before dereferencing them.
  for (unsigned int i = 0; i < MMU_ENVIRONMENT_COUNT; i++) {
    if (space && space == environments.spaces[i])
      return mmu_activation_select(&environments, environments.spaces[i]);
  }
  return false;
}

bool mmu_restore_kernel_space(void) {
  if (environments.active == &kernel_space)
    return mmu_activation_hw_matches(kernel_space.root_physical);
  return mmu_activate_address_space(&kernel_space);
}

// Query EL0 AP permissions under the actual selected root. AT does not
// access target data. It deliberately returns numbers, never a CPU pointer.
// Arm DDI0601: AT S1E0R/S1E0W and PAR_EL1 (see the H2.6c guide).
bool mmu_translate_user(uint64_t address, bool write,
    mmu_translation_t *result) {
  if (!result || !mmu_enabled())
    return false;
  uint64_t par;
  if (write)
    __asm__ volatile("at s1e0w, %1\nisb\nmrs %0, par_el1"
        : "=r"(par) : "r"(address) : "memory");
  else
    __asm__ volatile("at s1e0r, %1\nisb\nmrs %0, par_el1"
        : "=r"(par) : "r"(address) : "memory");
  *result = (mmu_translation_t){
    .virtual_address = address, .par = par, .fault = (par & 1U) != 0,
    .fault_status = (par & 1U) ? (uint8_t)((par >> 1) & 0x3fU) : 0,
    .physical_address = (par & 1U) ? 0 :
        (par & 0x000FFFFFFFFFF000ULL) | (address & 4095U)
  };
  return true;
}

const mmu_address_space_t *mmu_high_address_space(void) {
  return high_space.state == MMU_SPACE_SEALED ? &high_space : NULL;
}
