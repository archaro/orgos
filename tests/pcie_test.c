// SPDX-License-Identifier: MIT
// Copyright 2026 Archaro
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Exercise the complete production driver with an instrumented MMIO boundary.
#define ORGOS_PCIE_IO_H
static uint32_t pcie_read32(uintptr_t address);
static uint16_t pcie_read16(uintptr_t address);
static void pcie_write32(uintptr_t address, uint32_t value);
static void pcie_write16(uintptr_t address, uint16_t value);
static void pcie_barrier(void) {}
static void pcie_instruction_sync(void) {}
#include "../src/pcie.c"

typedef struct {
  unsigned int offset;
  uint32_t value;
  unsigned int width;
  bool endpoint;
} operation_t;

static uint32_t registers[0x10000U / 4U];
static uint8_t endpoint[4096];
static operation_t writes[512];
static unsigned int write_count, endpoint_reads, select_count;
static unsigned int ignored_register, ignored_endpoint;
static uint64_t now, link_after;
static uint32_t mask_state;
static bool fdt_available, require_reset, reset_observed;
static fdt_pcie_dma_t firmware;
static char output[256];
static size_t output_size;

static void put16(unsigned int reg, uint16_t value) {
  memcpy(endpoint + reg, &value, sizeof(value));
}

static void put32(unsigned int reg, uint32_t value) {
  memcpy(endpoint + reg, &value, sizeof(value));
}

static uint16_t get16(unsigned int reg) {
  uint16_t value;
  memcpy(&value, endpoint + reg, sizeof(value));
  return value;
}

static unsigned int offset_of(uintptr_t address) {
  assert(address >= RC_ADDRESS && address < RC_ADDRESS + sizeof(registers));
  return address - RC_ADDRESS;
}

static bool endpoint_access(unsigned int offset) {
  if (offset < CONFIG_DATA || offset >= CONFIG_SELECT)
    return false;
  assert((registers[CONFIG_SELECT / 4] >> 20) != 0);
  return true;
}

static uint32_t pcie_read32(uintptr_t address) {
  unsigned int offset = offset_of(address);
  assert((offset & 3U) == 0);
  if (endpoint_access(offset)) {
    endpoint_reads++;
    uint32_t value;
    memcpy(&value, endpoint + offset - CONFIG_DATA, sizeof(value));
    return value;
  }
  // Mask-set/clear are commands, not readable mask-status registers.
  assert(offset != MSI_MASK && offset != MSI_UNMASK && offset != MSI_ACK);
  if (offset == LINK_STATE) {
    if (require_reset)
      assert(reset_observed && !(registers[RESET_CONTROL / 4] & RESET_BRIDGE));
    uint32_t value = registers[offset / 4];
    if (now < link_after)
      value &= ~(LINK_DATA | LINK_PHY);
    return value;
  }
  return registers[offset / 4];
}

static uint16_t pcie_read16(uintptr_t address) {
  unsigned int offset = offset_of(address);
  assert((offset & 1U) == 0);
  if (endpoint_access(offset)) {
    endpoint_reads++;
    return get16(offset - CONFIG_DATA);
  }
  return (uint16_t)(registers[offset / 4] >> (8 * (offset & 2U)));
}

static void record(unsigned int offset, uint32_t value, unsigned int width) {
  bool ep = endpoint_access(offset);
  assert(write_count < sizeof(writes) / sizeof(writes[0]));
  writes[write_count++] = (operation_t){
    .offset = ep ? offset - CONFIG_DATA : offset,
    .value = value, .width = width, .endpoint = ep
  };
}

static void pcie_write32(uintptr_t address, uint32_t value) {
  unsigned int offset = offset_of(address);
  assert((offset & 3U) == 0);
  record(offset, value, 32);
  if (endpoint_access(offset)) {
    offset -= CONFIG_DATA;
    // Accidental DWORD writes here would clear adjacent W1C PCI Status.
    if (offset == CFG_COMMAND) {
      put16(offset, value);
      put16(offset + 2, get16(offset + 2) & ~(value >> 16));
    } else if (offset != ignored_endpoint) {
      put32(offset, value);
    }
    return;
  }
  if (offset == ignored_register)
    return;
  if (offset == CONFIG_SELECT)
    select_count++;
  if (offset == RESET_CONTROL && (value & 3U) == 3U)
    reset_observed = true;
  if (offset == MSI_ACK)
    registers[MSI_PENDING / 4] &= ~value;
  else if (offset == MSI_MASK)
    mask_state |= value;
  else if (offset == MSI_UNMASK)
    mask_state &= ~value;
  else
    registers[offset / 4] = value;
}

static void pcie_write16(uintptr_t address, uint16_t value) {
  unsigned int offset = offset_of(address);
  assert((offset & 1U) == 0);
  record(offset, value, 16);
  if (endpoint_access(offset)) {
    put16(offset - CONFIG_DATA, value);
  } else {
    unsigned int shift = 8 * (offset & 2U);
    registers[offset / 4] &= ~(0xffffU << shift);
    registers[offset / 4] |= (uint32_t)value << shift;
  }
}

uint64_t timer_now_us(void) { return now; }
void timer_delay_us(uint64_t us) { now += us; }
void timer_delay_ms(uint32_t ms) { now += (uint64_t)ms * 1000; }

bool fdt_get_pcie_dma(fdt_pcie_dma_t *result) {
  *result = firmware;
  return fdt_available;
}

void console_puts(const char *text) {
  size_t size = strlen(text);
  assert(output_size + size < sizeof(output));
  memcpy(output + output_size, text, size + 1);
  output_size += size;
}

void console_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int size = vsnprintf(output + output_size, sizeof(output) - output_size,
      format, args);
  va_end(args);
  assert(size >= 0 && (size_t)size < sizeof(output) - output_size);
  output_size += size;
}

static void fixture(void) {
  memset(registers, 0, sizeof(registers));
  memset(endpoint, 0, sizeof(endpoint));
  now = link_after = 0;
  write_count = endpoint_reads = select_count = 0;
  ignored_register = ignored_endpoint = UINT_MAX;
  link_started = resources_assigned = ram_window_configured = false;
  mask_state = 0;
  require_reset = reset_observed = false;
  output_size = 0;
  output[0] = 0;
  registers[LINK_STATE / 4] = MODE_ROOT | LINK_PHY | LINK_DATA;
  registers[REVISION / 4] = MSI_REVISION;
  registers[SERDES_CONTROL / 4] = SERDES_POWERDOWN | 0x40U;
  registers[DMA_CONTROL / 4] = BURST_FIELD | 0x40U;
  registers[CFG_ID / 4] = 0x271114e4U;
  put32(CFG_ID, VL805_ID);
  put32(CFG_CLASS, (XHCI_CLASS << 8) | 3U);
  put32(CFG_BAR_LOW, 4U);
  put16(CFG_COMMAND, 0x140U);
  put16(CFG_COMMAND + 2, 0xf910U);
  put32(CFG_CAPABILITIES, 0x50U);
  put32(0x50, (MSI_WIDE_ADDRESS << 16) | 0x7000U | CAP_MSI);
  put32(0x70, 0x40000000U | CAP_MSIX);
  put32(CFG_INTERRUPT, 0x100U);
  fdt_available = true;
  firmware = (fdt_pcie_dma_t){.child_address_cells = 3,
    .parent_address_cells = 2, .size_cells = 2, .dma_cell_count = 7,
    .dma_cells = {0x02000000U, 4, 0, 0, 0, 0, 0xc0000000U}};
}

static unsigned int host_writes(unsigned int reg) {
  unsigned int count = 0;
  for (unsigned int i = 0; i < write_count; i++)
    count += !writes[i].endpoint && writes[i].offset == reg;
  return count;
}

static void start_and_map(void) {
  assert(pcie_init_link() == PCIE_INIT_OK);
  assert(pcie_map_xhci() == PCIE_MAP_OK);
}

static void test_link_and_mapping(void) {
  fixture();
  require_reset = true;
  link_after = 120000;
  assert(pcie_init_link() == PCIE_INIT_OK);
  assert(now == 120300);
  assert(writes[0].offset == RESET_CONTROL && writes[0].value == 3U);
  assert(writes[1].offset == RESET_CONTROL && writes[1].value == 1U);
  assert(registers[SERDES_CONTROL / 4] == 0x40U);
  assert(registers[DMA_CONTROL / 4] == 0x34c0U);
  unsigned int count = write_count;
  assert(pcie_init_link() == PCIE_INIT_ALREADY_UP);
  assert(write_count == count);
  assert(pcie_map_xhci() == PCIE_MAP_OK);
  assert(registers[CFG_BUSES / 4] == 0x00010100U);
  assert(registers[OUTBOUND_RANGE / 4] == 0x03f00000U);
  assert(registers[OUTBOUND_START_HIGH / 4] == 6U);
  assert(registers[OUTBOUND_END_HIGH / 4] == 6U);
  assert(registers[OUTBOUND_LOW / 4] == XHCI_PCI_ADDRESS);
  assert(registers[CFG_MEMORY / 4] == 0xc000c000U);
  assert(get16(CFG_COMMAND) == 0x142U);
  assert(get16(CFG_COMMAND + 2) == 0xf910U);
  assert(pcie_xhci_bar_valid());
  assert(pcie_xhci_enable_bus_master());
  assert(get16(CFG_COMMAND) == 0x146U);
  assert(pcie_xhci_disable_bus_master());
  assert(get16(CFG_COMMAND) == 0x142U);
  assert(get16(CFG_COMMAND + 2) == 0xf910U);
  for (unsigned int i = 0; i < write_count; i++) {
    if (writes[i].endpoint && writes[i].offset == CFG_COMMAND)
      assert(writes[i].width == 16U);
  }
  count = write_count;
  assert(pcie_map_xhci() == PCIE_MAP_OK && write_count == count);
}

static void test_link_and_map_errors(void) {
  fixture();
  registers[LINK_STATE / 4] = MODE_ROOT;
  pcie_info_t info;
  pcie_probe(&info);
  assert(!info.link_up && endpoint_reads == 0 && select_count == 0);
  assert(pcie_map_xhci() == PCIE_MAP_LINK_DOWN);
  fixture();
  registers[LINK_STATE / 4] &= ~MODE_ROOT;
  assert(pcie_init_link() == PCIE_INIT_NOT_ROOT_COMPLEX);
  assert(registers[RESET_CONTROL / 4] & RESET_ENDPOINT);
  fixture();
  link_after = UINT64_MAX;
  assert(pcie_init_link() == PCIE_INIT_LINK_TIMEOUT);
  assert(now == 200300 && !pcie_xhci_ready());
  fixture();
  uint64_t began = UINT64_MAX - 150000;
  now = began;
  link_after = UINT64_MAX;
  assert(pcie_init_link() == PCIE_INIT_LINK_TIMEOUT);
  assert(now - began == 200300);
  fixture();
  ignored_register = CFG_BUSES;
  assert(pcie_map_xhci() == PCIE_MAP_BUS_FAILED);
  fixture();
  put32(CFG_ID, 0xffffffffU);
  assert(pcie_map_xhci() == PCIE_MAP_WRONG_DEVICE);
  fixture();
  put32(CFG_BAR_LOW, 0);
  assert(pcie_map_xhci() == PCIE_MAP_BAD_BAR);
  fixture();
  ignored_endpoint = CFG_BAR_LOW;
  assert(pcie_map_xhci() == PCIE_MAP_VERIFY_FAILED);
  assert(!(get16(CFG_COMMAND) & COMMAND_MEMORY));
}

static void test_dma_safety_and_bounds(void) {
  fixture();
  start_and_map();
  assert(pcie_xhci_enable_bus_master());
  assert(!pcie_dma_init());
  assert(host_writes(INBOUND2_LOW) == 0);
  assert(host_writes(INBOUND1_LOW) == 0);
  assert(strstr(output, "BME=1"));
  assert(pcie_xhci_disable_bus_master());
  firmware.dma_cells[1] = 1;
  assert(!pcie_dma_init() && host_writes(INBOUND2_LOW) == 0);
  firmware.dma_cells[1] = 4;
  registers[INBOUND1_LOW / 4] = 0x10U;
  registers[INBOUND3_LOW / 4] = 0x10U;
  ignored_register = INBOUND1_LOW;
  assert(!pcie_dma_init() && host_writes(INBOUND2_LOW) == 0);
  ignored_register = UINT_MAX;
  assert(pcie_dma_init());
  assert(registers[INBOUND1_LOW / 4] == 0);
  assert(registers[INBOUND3_LOW / 4] == 0);
  assert(registers[INBOUND2_LOW / 4] == 17U);
  assert(registers[INBOUND2_HIGH / 4] == 1U);
  assert((registers[DMA_CONTROL / 4] & SCB0_FIELD) == 0x88000000U);
  uint64_t address = 0;
  assert(pcie_phys_to_dma(0x1000, 4096, &address));
  assert(address == 0x100001000ULL);
  assert(pcie_phys_to_dma(0xbfffffff, 1, &address));
  assert(address == 0x1bfffffffULL);
  assert(!pcie_phys_to_dma(0xbfffffff, 2, &address));
  assert(!pcie_phys_to_dma(0xc0000000, 1, &address));
  assert(!pcie_phys_to_dma(0, 0, &address));
  assert(!pcie_phys_to_dma(0x1000, UINT64_MAX, &address));
  assert(!pcie_phys_to_dma(UINT64_MAX, 1, &address));
  assert(!pcie_phys_to_dma(0, 1, NULL));
  assert(pcie_xhci_enable_bus_master());
  unsigned int count = write_count;
  assert(pcie_dma_init() && write_count == count);
}

static void test_dma_failure(void) {
  fixture();
  start_and_map();
  ignored_register = INBOUND2_HIGH;
  assert(!pcie_dma_init());
  uint64_t address;
  assert(!pcie_phys_to_dma(0, 1, &address));
  fixture();
  start_and_map();
  put16(CFG_COMMAND, 0xffffU);
  assert(!pcie_dma_init());
  assert(strstr(output, "Command read failed (BME unknown)"));
  assert(host_writes(INBOUND2_LOW) == 0);
}

static void test_capabilities_and_msi(void) {
  fixture();
  assert(pcie_msi_host_prepare() == PCIE_MSI_PREPARE_NOT_READY);
  start_and_map();
  assert(pcie_dma_init());
  pcie_xhci_interrupt_info_t info;
  assert(pcie_xhci_get_interrupt_info(&info));
  assert(info.msi_present && info.msi_64bit && !info.msi_enabled);
  assert(info.msix_present && info.msix_masked && !info.msix_enabled);
  assert(info.capabilities_scanned == 2);
  put16(0x5e, 0xbeefU);
  assert(pcie_msi_host_prepare() == PCIE_MSI_PREPARE_OK);
  assert(mask_state == UINT32_MAX);
  assert(pcie_xhci_msi_program());
  assert(get16(0x5e) == 0xbeefU);
  assert(pcie_xhci_msi_set_enabled(true));
  unsigned int receiver_writes = host_writes(MSI_TARGET_LOW);
  assert(pcie_msi_host_prepare() == PCIE_MSI_PREPARE_ENDPOINT_STATE);
  assert(host_writes(MSI_TARGET_LOW) == receiver_writes);
  assert(!pcie_xhci_msi_set_enabled(true));
  assert(pcie_msi_host_unmask_vector0());
  assert(mask_state == 0xfffffffeU);
  registers[MSI_PENDING / 4] = 5U;
  unsigned int count = write_count;
  assert(pcie_msi_host_irq_quiesce() == 5U);
  assert(write_count == count + 2);
  assert(writes[count].offset == MSI_MASK);
  assert(writes[count + 1].offset == MSI_ACK && writes[count + 1].value == 5U);
  assert(registers[MSI_PENDING / 4] == 0 && mask_state == UINT32_MAX);
  assert(pcie_xhci_msi_set_enabled(false));
  registers[MSI_PENDING / 4] = 1U;
  assert(!pcie_msi_host_unmask_vector0());
  assert(pcie_msi_host_prepare() == PCIE_MSI_PREPARE_OK);
  registers[REVISION / 4] = 0x302;
  assert(pcie_msi_host_prepare() == PCIE_MSI_PREPARE_UNSUPPORTED_REVISION);
  registers[REVISION / 4] = MSI_REVISION;
  registers[MSI_TARGET_LOW / 4] = 0;
  ignored_register = MSI_TARGET_LOW;
  assert(pcie_msi_host_prepare() == PCIE_MSI_PREPARE_VERIFY_FAILED);
  assert(mask_state == UINT32_MAX);
  assert(!(registers[MSI_TARGET_LOW / 4] & 1));
}

static void test_bad_capabilities_and_diagnostics(void) {
  fixture();
  start_and_map();
  pcie_xhci_interrupt_info_t irq;
  put32(0x70, 0x5000U | CAP_MSIX);
  assert(!pcie_xhci_get_interrupt_info(&irq));
  put32(0x70, CAP_MSI);
  assert(!pcie_xhci_get_interrupt_info(&irq));
  put32(CFG_CAPABILITIES, 0xfcU);
  put32(0xfc, (MSI_WIDE_ADDRESS << 16) | CAP_MSI);
  pcie_xhci_msi_message_t message;
  assert(!pcie_xhci_msi_get_message(&message));
  put32(CFG_CAPABILITIES, 0x50U);
  put32(0x50, 0x12340000U | CAP_PM);
  put16(0x54, 0x0403U);
  pcie_xhci_diagnostic_t diagnostic;
  assert(pcie_xhci_diagnose(&diagnostic));
  assert(diagnostic.pm_present && diagnostic.pm_offset == 0x50U);
  assert(diagnostic.pm_capabilities == 0x1234U);
  assert(diagnostic.pm_control == 0x0403U);
  put32(CFG_FIRMWARE, 0x138a1U);
  uint32_t version;
  assert(pcie_xhci_get_firmware_version(&version) && version == 0x138a1U);
  pcie_dma_info_t dma;
  unsigned int count = write_count;
  assert(pcie_dma_probe(&dma) && write_count == count);
  assert(!pcie_dma_probe(NULL));
}

int main(void) {
  test_link_and_mapping();
  test_link_and_map_errors();
  test_dma_safety_and_bounds();
  test_dma_failure();
  test_capabilities_and_msi();
  test_bad_capabilities_and_diagnostics();
  puts("PCIe MMIO, startup, DMA and MSI contracts: PASS");
  return 0;
}
