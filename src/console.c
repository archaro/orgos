
#include <stddef.h>
#include <stdbool.h>

#include "console.h"
#include "mailbox.h"
#include "uart.h"
#include "fdt.h"
#include "physmem.h"
#include "irq.h"
#include "mmu.h"
#include "pcie.h"
#include "xhci.h"
#include "midi.h"

#define CONSOLE_LINE_SIZE 256

typedef void (*console_command_fn)(const char *args);

#include "timer.h"

static void cmd_timer(const char *args) {
  (void)args;

  // This function tells us some interesting information about the timer.
  uint64_t tick = timer_now_us();

  console_puts("System timer:\n");
  console_puts("  Counter    : ");
  console_putu64(tick);
  console_puts(" ticks\n");
  console_puts("  Frequency  : 1000000 Hz\n");
  console_puts("  Resolution : 1 us\n");
  console_puts("  Elapsed    : ");
  console_putu64(tick / 1000000U);
  console_putc('.');

  uint32_t fraction = (uint32_t)(tick % 1000000U);
  uint32_t divisor = 100000U;
  while (divisor) {
    console_putc('0' + (fraction / divisor) % 10U);
    divisor /= 10U;
  }
  console_puts(" seconds since time began.\n");
}

static void cmd_uptime(const char *args) {
  (void)args;

  uint64_t seconds = timer_uptime_us() / 1000000U;
  uint64_t days = seconds / 86400U;
  seconds %= 86400U;
  uint32_t hours = (uint32_t)(seconds / 3600U);
  seconds %= 3600U;
  uint32_t minutes = (uint32_t)(seconds / 60U);
  seconds %= 60U;

  console_puts("OrgOS uptime: ");
  if (days) {
    console_putu64(days);
    console_puts(days == 1 ? " day, " : " days, ");
  }
  console_putu32(hours);
  console_puts("h ");
  console_putu32(minutes);
  console_puts("m ");
  console_putu32((uint32_t)seconds);
  console_puts("s\n");
}

static void cmd_irq(const char *args) {
  (void)args;

  console_puts("Interrupts handled: ");
  console_putu64(irq_count());
  console_putc('\n');

  console_puts("UART RX overruns : ");
  console_putu32(uart_rx_overruns());
  console_putc('\n');
}

static void cmd_help(const char *args);

/*
 * Return true if the next whitespace-delimited word
 * matches the supplied string.
 *
 * Advance the input pointer past the matched word.
 */
static bool
next_word_is(const char **input, const char *word)
{
  const char *p = *input;
  const char *w = word;

  while (*p == ' ' || *p == '\t')
    p++;

  while (*w && *p == *w) {
    p++;
    w++;
  }

  if (*w || (*p && *p != ' ' && *p != '\t'))
    return false;

  *input = p;
  return true;
}


// Parse an unsigned 64-bit hexadecimal address.
// Accept either:
//   0x100000000
//   100000000
// Reject missing digits, invalid characters,
// overflow, and trailing non-whitespace text.
static bool next_hex64(const char **input, uint64_t *value) {
  const char *p = *input;
  uint64_t result = 0;
  bool found = false;

  while (*p == ' ' || *p == '\t')
    p++;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    p += 2;
  for (;;) {
    unsigned int digit;
    if (*p >= '0' && *p <= '9')
      digit = (unsigned int)(*p - '0');
    else if (*p >= 'a' && *p <= 'f')
      digit = (unsigned int)(*p - 'a') + 10U;
    else if (*p >= 'A' && *p <= 'F')
      digit = (unsigned int)(*p - 'A') + 10U;
    else
      break;
    if (result > (UINT64_MAX - digit) / 16ULL)
      return false;
    result = result * 16ULL + digit;
    found = true;
    p++;
  }
  if (!found || (*p && *p != ' ' && *p != '\t'))
    return false;
  *value = result;
  *input = p;
  return true;
}

static bool next_uint(const char **input, unsigned int *value) {
  // Read an unsigned decimal integer.
  // Reject missing values, non-decimal characters and overflow.
  const char *p = *input;
  unsigned int result = 0;
  bool found = false;

  while (*p == ' ' || *p == '\t')
    p++;

  while (*p >= '0' && *p <= '9') {
    unsigned int digit = (unsigned int)(*p - '0');

    if (result > (UINT32_MAX - digit) / 10U)
      return false;

    result = result * 10U + digit;
    found = true;
    p++;
  }

  if (!found || (*p && *p != ' ' && *p != '\t'))
    return false;

  *value = result;
  *input = p;
  return true;
}

static bool end_of_args(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;

  return *p == '\0';
}

static void console_puthex8(uint8_t value) {
  // Print one byte as two hexadecimal digits.
  static const char hex[] = "0123456789ABCDEF";

  console_putc(hex[value >> 4]);
  console_putc(hex[value & 0x0f]);
}

static void console_puthex(uint32_t value, unsigned int digits) {
  static const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < digits; i++) {
    unsigned int shift = (digits - 1U - i) * 4U;
    console_putc(hex[(value >> shift) & 0x0fU]);
  }
}

static void console_puthex64(uint64_t value) {
  static const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < 16; i++) {
    unsigned int shift = (15U - i) * 4U;
    console_putc(hex[(value >> shift) & 0x0fU]);
  }
}

static void print_mmu_translation(uint64_t address) {
  mmu_translation_t result;

  console_puts("  VA 0x");
  console_puthex64(address);
  if (!mmu_translate(address, &result)) {
    console_puts(" : diagnostic unavailable\n");
    return;
  }
  if (result.fault) {
    console_puts(" : FAULT (status 0x");
    console_puthex(result.fault_status, 2);
    console_puts(")\n");
  } else {
    console_puts(" -> PA 0x");
    console_puthex64(result.physical_address);
    console_putc('\n');
  }
}

static void print_mmu_write_test(uint64_t address) {
  mmu_translation_t result;

  console_puts("  VA 0x");
  console_puthex64(address);
  if (!mmu_translate_write(address, &result)) {
    console_puts(" : diagnostic unavailable\n");
  } else if (result.fault) {
    console_puts(" : WRITE DENIED (status 0x");
    console_puthex(result.fault_status, 2);
    console_puts(")\n");
  } else {
    console_puts(" : WRITE ALLOWED\n");
  }
}

static void cmd_pci(const char *args) {
  if (next_word_is(&args, "init") && end_of_args(args)) {
    console_puts("Initialising BCM2711 PCIe link...\n");
    pcie_init_result_t result = pcie_init_link();
    switch (result) {
      case PCIE_INIT_OK:
        console_puts("PCIe link established.\n");
        break;
      case PCIE_INIT_ALREADY_UP:
        console_puts("PCIe link is already active.\n");
        break;
      case PCIE_INIT_NOT_ROOT_COMPLEX:
        console_puts("PCIe initialization failed: not in RC mode.\n");
        break;
      case PCIE_INIT_LINK_TIMEOUT:
        console_puts("PCIe initialization failed: link timeout.\n");
        break;
    }
    console_putc('\n');
  } else if (!end_of_args(args)) {
    console_puts("Usage: pci [init]\n");
    return;
  }

  pcie_info_t info;
  pcie_probe(&info);

  console_puts("BCM2711 PCIe controller:\n");
  console_puts("  Status      : 0x");
  console_puthex(info.status, 8);
  console_putc('\n');
  console_puts("  Revision    : 0x");
  console_puthex(info.revision, 8);
  console_putc('\n');
  console_puts("  Mode        : ");
  console_puts(info.root_complex ?
      "Root complex\n" : "Not root complex\n");
  console_puts("  Link        : ");
  console_puts(info.link_up ? "Up\n" : "Down\n");
  if (!info.root_complex || !info.link_up) {
    console_puts(
        "\nPCIe host bridge is not ready.\n");
    return;
  }

  console_puts("\nRoot bridge:\n");
  console_puts("  Vendor ID   : 0x");
  console_puthex(info.root_vendor, 4);
  console_putc('\n');
  console_puts("  Device ID   : 0x");
  console_puthex(info.root_device, 4);
  console_putc('\n');
  console_puts("  Secondary   : ");
  console_putu32(info.secondary_bus);
  console_putc('\n');
  console_puts("  Subordinate : ");
  console_putu32(info.subordinate_bus);
  console_putc('\n');
  if (info.secondary_bus == 0 ||
      info.subordinate_bus < info.secondary_bus) {
    console_puts(
        "\nDownstream bus is not configured.\n");
    return;
  }

  if (!info.endpoint_present) {
    console_puts("\nNo downstream device found.\n");
    return;
  }

  console_puts("\nDownstream device:\n");
  console_puts("  Vendor ID   : 0x");
  console_puthex(info.endpoint_vendor, 4);
  console_putc('\n');
  console_puts("  Device ID   : 0x");
  console_puthex(info.endpoint_device, 4);
  console_putc('\n');
  console_puts("  Class       : 0x");
  console_puthex(info.endpoint_class, 6);
  console_putc('\n');
  console_puts("  BAR0        : 0x");
  console_puthex(info.endpoint_bar0, 8);
  console_putc('\n');
  console_puts("  BAR1        : 0x");
  console_puthex(info.endpoint_bar1, 8);
  console_putc('\n');
  if (info.endpoint_vendor == 0x1106U &&
      info.endpoint_device == 0x3483U) {
    console_puts("\nVIA VL805 USB controller detected.\n");
  } else {
    console_puts("Unknown USB device detected.\n");
  }
}

static void cmd_usb(const char *args) {
  (void)args;
  // The VL805 is configured at boot.
  // This command merely inspects the controller.
  if (!pcie_xhci_ready()) {
    console_puts("VL805 PCI resources are not configured.\n");
    return;
  }

  xhci_info_t info;
  if (!xhci_probe(&info)) {
    console_puts("xHCI capability probe failed.\n");
    return;
  }
  console_puts("\nVL805 xHCI controller:\n");
  console_puts("  Capability length : ");
  console_putu32(info.capability_length);
  console_putc('\n');
  console_puts("  xHCI version      : 0x");
  console_puthex(info.version, 4);
  console_putc('\n');
  console_puts("  Maximum slots     : ");
  console_putu32(info.max_slots);
  console_putc('\n');
  console_puts("  Maximum interrupters: ");
  console_putu32(info.max_interrupters);
  console_putc('\n');
  console_puts("  Maximum ports     : ");
  console_putu32(info.max_ports);
  console_putc('\n');
  console_puts("  Doorbell offset   : 0x");
  console_puthex(info.doorbell_offset, 8);
  console_putc('\n');
  console_puts("  Runtime offset    : 0x");
  console_puthex(info.runtime_offset, 8);
  console_putc('\n');
}


static void cmd_fdt_memory(void) {
  // Static storage avoids placing the entire
  // discovery report on our 16 KiB kernel stack.
  // The console is currently single-threaded.
  static fdt_memory_map_t map;

  if (!fdt_get_memory_map(&map)) {
    console_puts("FDT memory-map parsing failed.\n");
    return;
  }

  console_puts("Firmware Device Tree memory map:\n");
  console_puts("  Root address cells : ");
  console_putu32(map.address_cells);
  console_putc('\n');
  console_puts("  Root size cells    : ");
  console_putu32(map.size_cells);
  console_putc('\n');
  console_puts("\nMemory banks:\n");

  uint64_t total_ram = 0;
  if (map.bank_count == 0)
    console_puts("  None described.\n");

  for (unsigned int i = 0; i < map.bank_count; i++) {
    const fdt_memory_range_t *range = &map.banks[i];
    console_puts("  Bank ");
    console_putu32(i);
    console_puts(":\n");
    console_puts("    Base : 0x");
    console_puthex64(range->address);
    console_putc('\n');
    console_puts("    Size : ");
    console_putu64(range->size / (1024ULL * 1024ULL));
    console_puts(" MiB\n");
    total_ram += range->size;
  }
  console_puts("  Total described RAM: ");
  console_putu64(total_ram / (1024ULL * 1024ULL));
  console_puts(" MiB\n");
  console_puts("\nReserved regions:\n");
  if (map.reservation_count == 0)
    console_puts("  None explicitly described.\n");
  for (unsigned int i = 0;
       i < map.reservation_count; i++) {
    const fdt_reservation_t *entry =
        &map.reservations[i];
    console_puts("  Reservation ");
    console_putu32(i);
    console_puts(entry->kind == FDT_RESERVE_HEADER ?
        " (header):\n" : " (/reserved-memory):\n");
    console_puts("    Base : 0x");
    console_puthex64(entry->range.address);
    console_putc('\n');
    console_puts("    Size : ");
    console_putu64(entry->range.size);
    console_puts(" bytes\n");
  }
  console_puts(
      "\nNote: These are firmware-described ranges,\n"
      "not an allocation-ready free-memory map.\n");
}

static void cmd_fdt(const char *args) {
  if (next_word_is(&args, "memory") && end_of_args(args)) {
    cmd_fdt_memory();
    return;
  }

  if (!end_of_args(args)) {
    console_puts("Usage: fdt [memory]\n");
    return;
  }

  fdt_info_t info;
  console_puts("Firmware Flattened Device Tree:\n");
  console_puts("  Firmware address : 0x");
  console_puthex64(fdt_original_address());
  console_putc('\n');

  if (fdt_get_info(&info)) {
    console_puts("  OrgOS copy       : 0x");
    console_puthex64(info.address);
    console_putc('\n');
  } else {
    console_puts("  Status           : Not available or invalid\n");
    return;
  }
  console_puts("  Status           : Valid header\n");
  console_puts("  Total size       : ");
  console_putu32(info.total_size);
  console_puts(" bytes\n");
  console_puts("  Version          : ");
  console_putu32(info.version);
  console_putc('\n');
  console_puts("  Compatible with  : ");
  console_putu32(info.last_compatible_version);
  console_putc('\n');
  console_puts("  Structure offset : 0x");
  console_puthex(info.structure_offset, 8);
  console_putc('\n');
  console_puts("  Structure size   : ");
  console_putu32(info.structure_size);
  console_puts(" bytes\n");
  console_puts("  Strings offset   : 0x");
  console_puthex(info.strings_offset, 8);
  console_putc('\n');
  console_puts("  Strings size     : ");
  console_putu32(info.strings_size);
  console_puts(" bytes\n");
  console_puts("  Reservations     : 0x");
  console_puthex(info.reservations_offset, 8);
  console_puts(" (offset)\n");
}

static void cmd_physmem(const char *args) {
  if (!end_of_args(args)) {
    console_puts("Usage: physmem\n");
    return;
  }

  const physmem_map_t *map = physmem_get_map();
  if (!map) {
    console_puts("Physical memory map unavailable.\n");
    return;
  }
  console_puts("OrgOS physical memory inventory:\n");
  console_puts("\nRAM banks:\n");

  for (unsigned int i = 0; i < map->bank_count; i++) {
    const fdt_memory_range_t *bank = &map->banks[i];
    console_puts("  Bank ");
    console_putu32(i);
    console_puts(": 0x");
    console_puthex64(bank->address);
    console_puts(" - 0x");
    console_puthex64(bank->address + bank->size);
    console_puts(" (exclusive)\n");
  }
  console_puts("\nFirmware-described RAM: ");
  console_putu64( map->described_ram_bytes / (1024ULL * 1024ULL));
  console_puts(" MiB\n");
  console_puts("\nReserved physical ranges:\n");

  static const char *names[] = {
    "FDT header",
    "/reserved-memory",
    "OrgOS kernel",
    "Original FDT",
    "Low memory page"
  };

  for (unsigned int i = 0;
       i < map->reservation_count; i++) {
    const physmem_reservation_t *entry = &map->reservations[i];
    console_puts("  ");
    console_puts(names[entry->kind]);
    console_puts(": 0x");
    console_puthex64(entry->address);
    console_puts(" - 0x");
    console_puthex64(entry->address + entry->size);
    console_puts(" (exclusive)\n");
  }
  console_puts( "\nStatus: Inventory constructed; allocation not enabled.\n");
}

static void print_mmu_descriptor(uint64_t address) {
  mmu_descriptor_info_t info;

  if (!mmu_inspect(address, &info)) {
    console_puts("Descriptor inspection unavailable.\n");
    return;
  }

  console_puts("MMU descriptor inspection:\n");
  console_puts("  Virtual address : 0x");
  console_puthex64(info.virtual_address);
  console_putc('\n');
  console_puts("  L1 descriptor   : 0x");
  console_puthex64(info.l1_descriptor);
  console_putc('\n');

  if (info.level >= 2) {
    console_puts("  L2 descriptor   : 0x");
    console_puthex64(info.l2_descriptor);
    console_putc('\n');
  }

  if (info.level >= 3) {
    console_puts("  L3 descriptor   : 0x");
    console_puthex64(info.l3_descriptor);
    console_putc('\n');
  }

  if (info.kind == MMU_DESC_UNMAPPED) {
    console_puts("  Mapping         : Unmapped\n");
    console_puts("  Fault level     : ");
    console_putu32(info.level);
    console_putc('\n');
    return;
  }
  console_puts("  Mapping         : ");
  console_puts(info.kind == MMU_DESC_PAGE ?
      "4 KiB page\n" : "2 MiB block\n");
  console_puts("  Physical address: 0x");
  console_puthex64(info.physical_address);
  console_putc('\n');
  console_puts("  Attribute index : ");
  console_putu32(info.attr_index);
  if (info.attr_index == 0)
    console_puts(" (Device-nGnRnE)\n");
  else if (info.attr_index == 1)
    console_puts(" (Normal non-cacheable)\n");
  else if (info.attr_index == 2)
    console_puts(" (Normal write-back)\n");
  else
    console_puts(" (Unknown)\n");
  console_puts("  Access          : ");
  console_puts(info.read_only ?  "Read-only\n" : "Read/write\n");
  console_puts("  EL0 access      : ");
  console_puts((info.access_permissions & 1U) ?
      "Permitted by AP\n" : "Denied by AP\n");
  console_puts("  Access flag     : ");
  console_puts(info.access_flag ?  "Set\n" : "Clear\n");
  console_puts("  PXN             : ");
  console_puts(info.pxn ? "Yes\n" : "No\n");
  console_puts("  UXN             : ");
  console_puts(info.uxn ? "Yes\n" : "No\n");
  console_puts("  WXN             : ");
  mmu_info_t registers;
  mmu_get_info(&registers);
  console_puts((registers.sctlr & (1ULL << 19)) ?
      "Enabled\n" : "Disabled\n");
  bool el1_executable =
      !info.pxn && !(registers.sctlr & (1ULL << 19) && !info.read_only);
  console_puts("  EL1 execution   : ");
  console_puts(el1_executable ?
      "Permitted by leaf permissions\n" :
      "Prevented by leaf permissions/WXN\n");
}

static void cmd_mmu(const char *args) {
  // Inspect transalation descriptors
  if (next_word_is(&args, "inspect")) {
    uint64_t address;
    if (!next_hex64(&args, &address) ||
        !end_of_args(args)) {
      console_puts("Usage: mmu inspect <hex-address>\n");
      return;
    }
    print_mmu_descriptor(address);
    return;
  }

  // Non-destructive permissions diagnostics.
  if (next_word_is(&args, "permissions") && end_of_args(args)) {
    extern char __text_start[];
    extern char __rodata_start[];
    extern char __data_start[];
    extern char __bss_start[];
    extern char __stack_bottom[];
    console_puts("EL1 kernel write permissions:\n");
    console_puts("Text:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__text_start);
    console_puts("Read-only data:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__rodata_start);
    console_puts("Initialized data:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__data_start);
    console_puts("BSS:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__bss_start);
    console_puts("Kernel stack:\n");
    print_mmu_write_test( (uint64_t)(uintptr_t)__stack_bottom);
    return;
  }

  // Non-destructive translation diagnostics.
  // With no address, examine representative
  // addresses from each RAM bank and the
  // currently configured MMIO regions.
  if (next_word_is(&args, "translate")) {
    if (!mmu_enabled()) {
      console_puts("MMU is disabled.\n");
      return;
    }
    if (end_of_args(args)) {
      static const uint64_t test_addresses[] = {
        0x0000000000008000ULL,  // Bank 0
        0x000000003B400000ULL,  // Gap after Bank 0
        0x0000000040000000ULL,  // Bank 1
        0x00000000FC000000ULL,  // Peripheral MMIO
        0x0000000100000000ULL,  // Bank 2
        0x0000000180000000ULL,  // Bank 3
        0x0000000200000000ULL,  // Gap after Bank 3
        0x0000000600000000ULL   // VL805 MMIO
      };
      console_puts("EL1 stage-1 read translations:\n");
      for (unsigned int i = 0; i < sizeof(test_addresses) /
               sizeof(test_addresses[0]); i++) {
        print_mmu_translation(test_addresses[i]);
      }
      return;
    }
    uint64_t address;
    if (!next_hex64(&args, &address) || !end_of_args(args)) {
      console_puts(
        "Usage: mmu [inspect <hex-address> | "
        "permissions | translate [hex-address] | fault]\n");
      return;
    }
    console_puts("EL1 stage-1 read translation:\n");
    print_mmu_translation(address);
    return;
  }

  // Deliberately access an unmapped virtual address.
  // Address 0x3c000000 has no valid translation.
  // This is a destructive diagnostic: a successful
  // test enters exception_panic() and halts OrgOS.
    if (next_word_is(&args, "fault") && end_of_args(args)) {
    if (!mmu_enabled()) {
      console_puts("MMU is disabled; refusing fault test.\n");
      return;
    }

    console_puts(
        "MMU translation fault test.\n"
        "Reading unmapped virtual address 0x3c000000ULL...\n"
        "Expected result: synchronous EL1 data abort.\n"
        "OrgOS will halt. Reboot required.\n\n");

    // The volatile load ensures that the compiler
    // actually emits a memory access.
    // Because the virtual address is unmapped,
    // the load should never complete.
    volatile const uint32_t *address =
        (volatile const uint32_t *)(uintptr_t)0x3c000000ULL;

    uint32_t value = *address;

    // If execution reaches here, the fault test
    // did not behave as expected.
    console_puts("UNEXPECTED: read succeeded: 0x");
    console_puthex(value, 8);
    console_putc('\n');
    return;
  }

  if (!end_of_args(args)) {
    console_puts("Usage: mmu [translate [hex-address] | fault]\n");
    return;
  }

  // Existing MMU diagnostic code follows here.
  mmu_info_t info;
  mmu_get_info(&info);

  console_puts("AArch64 EL1 memory management:\n");
  console_puts("  Translation       : ");
  console_puts((info.sctlr & 1U) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Data cache        : ");
  console_puts((info.sctlr & (1U << 2)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Instruction cache : ");
  console_puts((info.sctlr & (1U << 12)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Alignment check   : ");
  console_puts((info.sctlr & (1U << 1)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Write-execute-never: ");
  console_puts((info.sctlr & (1ULL << 19)) ?  "Enabled\n" : "Disabled\n");
  console_puts("  Physical addr bits: ");
  unsigned int pa_bits = mmu_physical_address_bits();
  if (pa_bits)
    console_putu32(pa_bits);
  else
    console_puts("Unknown encoding");
  console_putc('\n');
  console_puts("  4 KiB granule     : ");
  console_puts(mmu_granule_4k_supported() ?
      "Supported\n" : "Unsupported/reserved\n");
  console_puts("\nArchitectural registers:\n");
  console_puts("  ID_AA64MMFR0_EL1 : 0x");
  console_puthex64(info.id_aa64mmfr0);
  console_putc('\n');
  console_puts("  SCTLR_EL1        : 0x");
  console_puthex64(info.sctlr);
  console_putc('\n');
  console_puts("  TCR_EL1          : 0x");
  console_puthex64(info.tcr);
  console_putc('\n');
  console_puts("  MAIR_EL1         : 0x");
  console_puthex64(info.mair);
  console_putc('\n');
  console_puts("  TTBR0_EL1        : 0x");
  console_puthex64(info.ttbr0);
  console_putc('\n');
  console_puts("  TTBR1_EL1        : 0x");
  console_puthex64(info.ttbr1);
  console_putc('\n');
}

static void cmd_midi(const char *args) {
  midi_message_t msg;
  const char *p = args;
  unsigned int channel, a, b;
  bool valid = false;

  if (next_word_is(&p, "noteon")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        next_uint(&p, &b) && end_of_args(p))
      valid = midi_note_on(&msg, channel, a, b);

  } else if (next_word_is(&p, "noteoff")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        next_uint(&p, &b) && end_of_args(p))
      valid = midi_note_off(&msg, channel, a, b);

  } else if (next_word_is(&p, "cc")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        next_uint(&p, &b) && end_of_args(p))
      valid = midi_control_change(&msg, channel, a, b);

  } else if (next_word_is(&p, "pc")) {
    if (next_uint(&p, &channel) && next_uint(&p, &a) &&
        end_of_args(p))
      valid = midi_program_change(&msg, channel, a);
  }

  if (!valid) {
    console_puts("Invalid MIDI command or arguments.\n");
    console_puts("Usage:\n");
    console_puts("  midi noteon  channel note velocity\n");
    console_puts("  midi noteoff channel note velocity\n");
    console_puts("  midi cc      channel controller value\n");
    console_puts("  midi pc      channel program\n");
    return;
  }

  console_puts("MIDI: ");
  for (unsigned int i = 0; i < msg.length; i++) {
    if (i)
      console_putc(' ');
    console_puthex8(msg.data[i]);
  }
  console_putc('\n');
}

typedef struct {
  const char *name;
  console_command_fn handler;
  const char *description;
} console_command_t;


// Decode the RAM capacity field in the modern
// Raspberry Pi board revision encoding.
// Bits [22:20] identify the installed RAM size.
// Bit 23 distinguishes the modern encoding from
// the original revision-number scheme.
// Return zero if the code cannot be decoded.
static uint32_t board_ram_mib(uint32_t revision) {
  if (!(revision & (1U << 23)))
    return 0;

  static const uint32_t sizes[] = {
    256, 512, 1024, 2048, 4096, 8192, 16384
  };

  uint32_t code = (revision >> 20) & 7U;
  if (code >= sizeof(sizes) / sizeof(sizes[0]))
    return 0;

  return sizes[code];
}

static void cmd_memory(const char *args) {
  (void)args;

  uint32_t base;
  uint32_t size;

  console_puts("ARM memory allocation:\n");
  if (!mailbox_get_arm_memory(&base, &size)) {
    console_puts("  Firmware memory query failed.\n");
    return;
  }

  uint32_t revision;
  if (mailbox_get_board_revision(&revision)) {
    console_puts("  Board revision : 0x");
    console_puthex(revision, 8);
    console_putc('\n');
    if (revision & (1U << 23)) {
      uint32_t model = (revision >> 4) & 0xffU;
      uint32_t processor = (revision >> 12) & 0x0fU;
      uint32_t ram_mib = board_ram_mib(revision);
      console_puts("  Board model    : ");
      if (model == 0x11U)
        console_puts("Raspberry Pi 4 Model B\n");
      else {
        console_puts("Type 0x");
        console_puthex(model, 2);
        console_putc('\n');
      }
      console_puts("  Processor      : ");
      if (processor == 3U)
        console_puts("BCM2711\n");
      else {
        console_puts("Code ");
        console_putu32(processor);
        console_putc('\n');
      }
      console_puts("  Installed RAM  : ");
      if (ram_mib) {
        console_putu32(ram_mib);
        console_puts(" MiB\n");
      } else {
        console_puts("Unknown\n");
      }
    } else {
      console_puts("  Legacy revision encoding\n");
    }
  } else {
    console_puts("  Board revision query failed.\n");
  }
  console_putc('\n');

  uint64_t end = (uint64_t)base + size;
  uint64_t mapped = mmu_ram_mapped_bytes();
  console_puts("  Base address : 0x");
  console_puthex(base, 8);
  console_putc('\n');
  console_puts("  Size         : ");
  console_putu32(size / (1024U * 1024U));
  console_puts(" MiB\n");
  console_puts("  End address  : 0x");
  console_puthex64(end);
  console_puts(" (exclusive)\n");
  console_puts("  MMU-mapped RAM: ");
  console_putu64(mapped / (1024ULL * 1024ULL));
  console_puts(" MiB\n");
  if (mapped < size) {
    console_puts("  Note: A partial final RAM block is not mapped.\n");
  }
}

// Commands will be added here as they are implemented.
// The NULL entry terminates the table.
static const console_command_t commands[] = {
  { "timer",  cmd_timer,  "Display system timer information" },
  { "uptime", cmd_uptime, "Show time since OrgOS started" },
  { "help",   cmd_help,   "List available commands" },
  { "memory", cmd_memory, "Display firmware ARM memory allocation" },
  { "irq", cmd_irq, "Display interrupt statistics" },
  { "fdt", cmd_fdt, "Inspect firmware Device Tree and memory map" },
  { "physmem", cmd_physmem, "Display physical RAM and reservations" },
  { "mmu", cmd_mmu, "Display AArch64 memory management state" },
  { "pci", cmd_pci, "Inspect or initialise PCIe controller" },
  { "usb", cmd_usb, "Inspect VL805 xHCI capabilities" },
  { "midi", cmd_midi, "Construct and inspect MIDI messages" },
  { NULL, NULL, NULL }
};

static void cmd_help(const char *args) {
  (void)args;

  console_puts("Available commands:\n\n");

  for (size_t i = 0; commands[i].name != NULL; i++) {
    console_puts("  ");
    console_puts(commands[i].name);

    // Align descriptions at column 12
    size_t length = 0;
    const char *p = commands[i].name;

    while (*p++) {
      length++;
    }

    do {
      console_putc(' ');
      length++;
    } while (length < 10);

    console_puts(commands[i].description);
    console_putc('\n');
  }
}

static bool is_space(char c) {
  return c == ' '  ||
         c == '\t' ||
         c == '\n' ||
         c == '\r' ||
         c == '\v' ||
         c == '\f';
}

static bool string_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b)
      return false;

    a++;
    b++;
  }

  return *a == *b;
}

static void console_prompt(void) {
  console_puts("OrgOS> ");
}

// Dispatch a completed, NUL-terminated input line.
// The buffer may be modified in place.
static void console_dispatch(char *line) {
  char *command = line;
  char *args;

  // Skip leading whitespace.
  while (is_space(*command))
    command++;

  // An empty line is not a command.
  if (*command == '\0')
    return;

  // Locate the end of the first word.
  char *p = command;

  while (*p && !is_space(*p))
    p++;

  if (*p) {
    // Terminate the command name and skip
    // whitespace preceding its arguments.
    *p++ = '\0';

    while (is_space(*p))
      p++;
  }

  args = p;

  // Look up the command.
  for (size_t i = 0; commands[i].name != NULL; i++) {
    if (string_equal(command, commands[i].name)) {
      commands[i].handler(args);
      return;
    }
  }

  console_puts("No such command.\n");
}

void console_run(void) {
  char line[CONSOLE_LINE_SIZE];
  size_t length = 0;
  bool last_was_cr = false;

  console_puts("Console ready.\n");
  console_prompt();

  for (;;) {
    unsigned char c = (unsigned char)uart_getc();

    // Treat CRLF as one newline.
    if (c == '\n' && last_was_cr) {
      last_was_cr = false;
      continue;
    }

    last_was_cr = (c == '\r');

    // A completed input line.
    if (c == '\r' || c == '\n') {
      console_puts("\n");

      line[length] = '\0';
      console_dispatch(line);

      length = 0;
      console_prompt();
      continue;
    }

    // Backspace (0x08) or DEL (0x7f).
    // Remove one character from the buffer
    // and erase it from the terminal.
    if (c == '\b' || c == 0x7f) {
      if (length > 0) {
        length--;
        console_puts("\b \b");
      }

      continue;
    }

    // Convert TAB to one ordinary space.
    // The display and buffer must agree.
    if (c == '\t')
      c = ' ';

    // Ignore unsupported control characters
    // and non-ASCII input.
    if (c < 0x20 || c > 0x7e)
      continue;

    // Reserve the final byte for '\0'.
    // If the line is full, ring the terminal
    // bell and refuse the additional character.
    // The user can still use backspace.
    if (length >= sizeof(line) - 1) {
      console_putc('\a');
      continue;
    }

    // Append and echo the character.
    line[length++] = (char)c;
    console_putc((char)c);
  }
}

void console_putc(char c) {
  if (c == '\n')
    uart_putc('\r');

  uart_putc(c);
}

void console_puts(const char *s) {
  while (*s)
    console_putc(*s++);
}

void console_putu32(uint32_t value) {
  char buffer[10];
  unsigned int length = 0;

  do {
    buffer[length++] = '0' + (value % 10U);
    value /= 10U;
  } while (value);

  while (length)
    uart_putc(buffer[--length]);
}

void console_putu64(uint64_t value) {
  char buffer[20];
  unsigned int length = 0;

  do {
    buffer[length++] = '0' + (value % 10U);
    value /= 10U;
  } while (value);

  while (length)
    uart_putc(buffer[--length]);
}

