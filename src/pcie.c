// New OrgOS integration, validation and diagnostics are licensed under MIT;
// Controller operations are adapted from the following ISC sources, whose
// original copyrights and notices are retained below:
// FreeBSD bcm2838_pci.c, a05af6ddf9016e4ea4f0b361aa674e7ece6fe7ec;
// OpenBSD bcm2711_pcie.c, a5eb81483dd17781492c4e8a54e1317ff8fd9837.

// Copyright (c) 2020 Dr Robert Harvey Crowston <crowston@protonmail.com>
// Copyright (c) 2020, 2025 Mark Kettenis <kettenis@openbsd.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "pcie.h"
#include "pcie_io.h"
#include "fdt.h"
#include "timer.h"
#include "console.h"

// CPU mappings are identity mappings, including Device memory for PCIe MMIO.
#define RC_ADDRESS 0xfd500000UL
#define XHCI_PCI_ADDRESS 0xc0000000U
#define XHCI_ARM_ADDRESS 0x600000000ULL
#define OUTBOUND_BYTES 0x04000000ULL
#define RAM_PCI_ADDRESS 0x100000000ULL
#define RAM_ADDRESS_CEILING 0xc0000000ULL
#define RAM_SIZE_CODE (32U - 15U)
#define VL805_ID 0x34831106U
#define XHCI_CLASS 0x0c0330U
#define MSI_ADDRESS 0xffffffffcULL
#define MSI_MATCH 0xffe06540U
#define MSI_VECTOR_DATA 0x6540U
#define MSI_REVISION 0x0304U

// Controller offsets and field encodings: FreeBSD/OpenBSD sources above.
enum {
  BAR2_ENDIAN = 0x0188,
  DMA_CONTROL = 0x4008,
  OUTBOUND_LOW = 0x400c,
  OUTBOUND_HIGH = 0x4010,
  INBOUND1_LOW = 0x402c,
  INBOUND1_HIGH = 0x4030,
  INBOUND2_LOW = 0x4034,
  INBOUND2_HIGH = 0x4038,
  INBOUND3_LOW = 0x403c,
  MSI_TARGET_LOW = 0x4044,
  MSI_TARGET_HIGH = 0x4048,
  MSI_MATCH_DATA = 0x404c,
  LINK_STATE = 0x4068,
  REVISION = 0x406c,
  OUTBOUND_RANGE = 0x4070,
  OUTBOUND_START_HIGH = 0x4080,
  OUTBOUND_END_HIGH = 0x4084,
  SERDES_CONTROL = 0x4204,
  MSI_PENDING = 0x4500,
  MSI_ACK = 0x4508,
  MSI_MASK = 0x4510,
  MSI_UNMASK = 0x4514,
  CONFIG_DATA = 0x8000,
  CONFIG_SELECT = 0x9000,
  RESET_CONTROL = 0x9210
};

enum {
  RESET_ENDPOINT = 1U,
  RESET_BRIDGE = 2U,
  LINK_PHY = 1U << 4,
  LINK_DATA = 1U << 5,
  MODE_ROOT = 1U << 7,
  COMPLETION_64 = 1U << 7,
  COMPLETION_MPS = 1U << 10,
  SCB_ACCESS = 1U << 12,
  CONFIG_UR = 1U << 13,
  BURST_FIELD = 3U << 20,
  SERDES_POWERDOWN = 1U << 27,
  ENDIAN_FIELD = 3U << 2
};
#define SCB0_FIELD (31U << 27)

// Standard PCI fields; numerical layout corroborated by FreeBSD pcireg.h.
enum {
  CFG_ID = 0x00,
  CFG_COMMAND = 0x04,
  CFG_CLASS = 0x08,
  CFG_HEADER = 0x0c,
  CFG_BAR_LOW = 0x10,
  CFG_BAR_HIGH = 0x14,
  CFG_BUSES = 0x18,
  CFG_MEMORY = 0x20,
  CFG_CAPABILITIES = 0x34,
  CFG_INTERRUPT = 0x3c,
  CFG_FIRMWARE = 0x50,
  COMMAND_MEMORY = 2U,
  COMMAND_MASTER = 4U,
  STATUS_CAPABILITIES = 1U << 4,
  CAP_PM = 1U,
  CAP_MSI = 5U,
  CAP_MSIX = 0x11U,
  MSI_ENABLE = 1U,
  MSI_MULTIPLE_ENABLE = 7U << 4,
  MSI_WIDE_ADDRESS = 1U << 7
};

static bool link_started;
static bool resources_assigned;
static bool ram_window_configured;

static uint32_t host_read(unsigned int reg) {
  return pcie_read32(RC_ADDRESS + reg);
}

static void host_write(unsigned int reg, uint32_t value) {
  pcie_write32(RC_ADDRESS + reg, value);
  pcie_barrier();
}

static void host_change(unsigned int reg, uint32_t mask, uint32_t bits) {
  host_write(reg, (host_read(reg) & ~mask) | bits);
}

static bool active_link(uint32_t state) {
  return (state & (LINK_PHY | LINK_DATA)) == (LINK_PHY | LINK_DATA);
}

// Only the root and the VL805 at device/function 0/0 are used by this API.
// IRQ handling accesses host MSI registers only; it never changes the selector.
static uintptr_t config_address(uint8_t bus, unsigned int reg) {
  if (bus == 0)
    return RC_ADDRESS + reg;
  host_write(CONFIG_SELECT, (uint32_t)bus << 20);
  return RC_ADDRESS + CONFIG_DATA + reg;
}

static uint32_t config_read(uint8_t bus, unsigned int reg) {
  return pcie_read32(config_address(bus, reg));
}

static uint16_t config_half(uint8_t bus, unsigned int reg) {
  return pcie_read16(config_address(bus, reg));
}

static void config_write(uint8_t bus, unsigned int reg, uint32_t value) {
  pcie_write32(config_address(bus, reg), value);
  pcie_barrier();
}

static void config_write_half(uint8_t bus, unsigned int reg, uint16_t value) {
  pcie_write16(config_address(bus, reg), value);
  pcie_barrier();
}

static void command_change(uint8_t bus, uint16_t mask, uint16_t bits) {
  uint16_t command = config_half(bus, CFG_COMMAND);
  config_write_half(bus, CFG_COMMAND, (command & (uint16_t)~mask) | bits);
}

bool pcie_xhci_ready(void) {
  return resources_assigned;
}

void pcie_probe(pcie_info_t *info) {
  if (!info)
    return;
  *info = (pcie_info_t){0};
  uint32_t state = host_read(LINK_STATE);
  info->status = state;
  info->revision = host_read(REVISION);
  info->root_complex = (state & MODE_ROOT) != 0;
  info->link_up = active_link(state);
  if (!info->root_complex || !info->link_up)
    return;

  uint32_t root = config_read(0, CFG_ID);
  info->root_vendor = (uint16_t)root;
  info->root_device = (uint16_t)(root >> 16);
  uint32_t buses = config_read(0, CFG_BUSES);
  uint8_t bus = (uint8_t)(buses >> 8);
  info->secondary_bus = bus;
  info->subordinate_bus = (uint8_t)(buses >> 16);
  if (!bus || bus > info->subordinate_bus)
    return;

  uint32_t endpoint = config_read(bus, CFG_ID);
  if ((uint16_t)endpoint == 0xffffU)
    return;
  info->endpoint_present = true;
  info->endpoint_vendor = (uint16_t)endpoint;
  info->endpoint_device = (uint16_t)(endpoint >> 16);
  info->endpoint_class = config_read(bus, CFG_CLASS) >> 8;
  info->endpoint_bar0 = config_read(bus, CFG_BAR_LOW);
  info->endpoint_bar1 = config_read(bus, CFG_BAR_HIGH);
}

static bool is_vl805(const pcie_info_t *info) {
  return info->root_complex && info->link_up && info->endpoint_present &&
      info->endpoint_vendor == (uint16_t)VL805_ID &&
      info->endpoint_device == (uint16_t)(VL805_ID >> 16);
}

static bool mapped_endpoint(uint8_t *bus) {
  if (!resources_assigned)
    return false;
  pcie_info_t info;
  pcie_probe(&info);
  if (!is_vl805(&info) || info.endpoint_class != XHCI_CLASS)
    return false;
  *bus = info.secondary_bus;
  return true;
}

pcie_init_result_t pcie_init_link(void) {
  if (link_started)
    return PCIE_INIT_ALREADY_UP;

  // Adapted reset mechanism; delays and polling budget are the OrgOS contract.
  // Do not sample controller status until the bridge has left reset.
  host_change(RESET_CONTROL, 0, RESET_BRIDGE | RESET_ENDPOINT);
  timer_delay_us(150);
  host_change(RESET_CONTROL, RESET_BRIDGE, 0);
  host_change(SERDES_CONTROL, SERDES_POWERDOWN, 0);
  timer_delay_us(150);
  host_change(DMA_CONTROL, BURST_FIELD,
      SCB_ACCESS | CONFIG_UR | COMPLETION_64 | COMPLETION_MPS);
  if (!(host_read(LINK_STATE) & MODE_ROOT))
    return PCIE_INIT_NOT_ROOT_COMPLEX;

  host_change(RESET_CONTROL, RESET_ENDPOINT, 0);
  timer_delay_ms(100);
  uint64_t started = timer_now_us();
  do {
    if (active_link(host_read(LINK_STATE))) {
      link_started = true;
      return PCIE_INIT_OK;
    }
    timer_delay_ms(5);
  } while (timer_now_us() - started < 100000ULL);
  return PCIE_INIT_LINK_TIMEOUT;
}

// FreeBSD encode_cpu_window_* encoding, specialized to OrgOS's one aperture.
static void assign_outbound_window(void) {
  uint64_t last = XHCI_ARM_ADDRESS + OUTBOUND_BYTES - 1;
  host_write(OUTBOUND_LOW, XHCI_PCI_ADDRESS);
  host_write(OUTBOUND_HIGH, 0);
  host_write(OUTBOUND_RANGE,
      ((XHCI_ARM_ADDRESS >> 16) & 0xfff0U) | (last & 0xfff00000U));
  host_write(OUTBOUND_START_HIGH, (XHCI_ARM_ADDRESS >> 32) & 0xffU);
  host_write(OUTBOUND_END_HIGH, (last >> 32) & 0xffU);
}

pcie_map_result_t pcie_map_xhci(void) {
  if (resources_assigned)
    return PCIE_MAP_OK;
  pcie_info_t info;
  pcie_probe(&info);
  if (!info.root_complex || !info.link_up)
    return PCIE_MAP_LINK_DOWN;
  if (!info.secondary_bus || info.secondary_bus > info.subordinate_bus) {
    uint32_t value = config_read(0, CFG_BUSES);
    config_write(0, CFG_BUSES, (value & 0xff000000U) | 0x00010100U);
    if ((config_read(0, CFG_BUSES) & 0x00ffffffU) != 0x00010100U)
      return PCIE_MAP_BUS_FAILED;
    pcie_probe(&info);
  }
  if (!is_vl805(&info) || info.endpoint_class != XHCI_CLASS)
    return PCIE_MAP_WRONG_DEVICE;
  if ((info.endpoint_bar0 & 15U) != 4U)
    return PCIE_MAP_BAD_BAR;

  assign_outbound_window();
  // Type 1 forwarding covers the first MiB of the assigned PCIe aperture.
  config_write(0, CFG_MEMORY, 0xc000c000U);
  uint8_t bus = info.secondary_bus;
  command_change(bus, COMMAND_MEMORY, 0);
  config_write(bus, CFG_BAR_LOW, XHCI_PCI_ADDRESS | 4U);
  config_write(bus, CFG_BAR_HIGH, 0);
  if ((config_read(bus, CFG_BAR_LOW) & ~15U) != XHCI_PCI_ADDRESS ||
      config_read(bus, CFG_BAR_HIGH) != 0)
    return PCIE_MAP_VERIFY_FAILED;
  command_change(0, COMMAND_MEMORY, COMMAND_MEMORY);
  command_change(bus, COMMAND_MEMORY, COMMAND_MEMORY);
  pcie_instruction_sync();
  resources_assigned = true;
  return PCIE_MAP_OK;
}

bool pcie_xhci_get_command(uint16_t *command) {
  uint8_t bus;
  if (!command || !mapped_endpoint(&bus))
    return false;
  uint16_t value = config_half(bus, CFG_COMMAND);
  if (value == 0xffffU)
    return false;
  *command = value;
  return true;
}

bool pcie_xhci_bar_valid(void) {
  uint8_t bus;
  return mapped_endpoint(&bus) &&
      (config_read(bus, CFG_BAR_LOW) & ~15U) == XHCI_PCI_ADDRESS &&
      config_read(bus, CFG_BAR_HIGH) == 0;
}

bool pcie_xhci_disable_bus_master(void) {
  uint8_t bus;
  if (!mapped_endpoint(&bus))
    return false;
  command_change(bus, COMMAND_MASTER, 0);
  uint16_t result;
  return pcie_xhci_get_command(&result) && !(result & COMMAND_MASTER);
}

bool pcie_xhci_enable_bus_master(void) {
  uint8_t bus;
  if (!mapped_endpoint(&bus) || !pcie_xhci_bar_valid())
    return false;
  uint16_t command = config_half(bus, CFG_COMMAND);
  if (command == 0xffffU || !(command & COMMAND_MEMORY))
    return false;
  config_write_half(bus, CFG_COMMAND, command | COMMAND_MASTER);
  uint16_t result;
  return pcie_xhci_get_command(&result) &&
      (result & (COMMAND_MEMORY | COMMAND_MASTER)) ==
          (COMMAND_MEMORY | COMMAND_MASTER);
}

bool pcie_dma_probe(pcie_dma_info_t *info) {
  if (!info)
    return false;
  *info = (pcie_dma_info_t){0};
  if (!(host_read(LINK_STATE) & MODE_ROOT))
    return false;
  info->misc_ctrl = host_read(DMA_CONTROL);
  info->bar2_lo = host_read(INBOUND2_LOW);
  info->bar2_hi = host_read(INBOUND2_HIGH);
  info->bar3_lo = host_read(INBOUND3_LOW);
  info->endian_ctrl = host_read(BAR2_ENDIAN);
  info->bar1_lo = host_read(INBOUND1_LOW);
  info->bar1_hi = host_read(INBOUND1_HIGH);
  return true;
}

static bool supported_firmware_dma(void) {
  fdt_pcie_dma_t description;
  if (!fdt_get_pcie_dma(&description))
    return false;
  if (description.child_address_cells != 3 ||
      description.parent_address_cells != 2 || description.size_cells != 2 ||
      description.dma_cell_count != 7 || description.scb_cell_count != 0)
    return false;
  // This is an input whitelist, not the base programmed by OrgOS.
  const uint32_t expected[] = {0x02000000U, 4, 0, 0, 0, 0, 0xc0000000U};
  for (unsigned int cell = 0; cell < 7; cell++) {
    if (description.dma_cells[cell] != expected[cell])
      return false;
  }
  return true;
}

bool pcie_dma_init(void) {
  // A running controller must never have its address translation rebuilt.
  if (ram_window_configured)
    return true;
  if (!resources_assigned || !supported_firmware_dma())
    return false;

  uint16_t command;
  if (!pcie_xhci_get_command(&command)) {
    console_puts("\nPCIe pre-DMA: Command read failed (BME unknown)\n");
    return false;
  }
  console_printf("\nPCIe pre-DMA: Command=0x%04x BME=%u\n",
      (unsigned int)command, (unsigned int)((command >> 2) & 1U));
  if (command & COMMAND_MASTER)
    return false;

  // OpenBSD/FreeBSD single-RAM-window model: eliminate other inbound decodes.
  host_write(INBOUND1_LOW, 0);
  host_write(INBOUND3_LOW, 0);
  if ((host_read(INBOUND1_LOW) & 31U) || (host_read(INBOUND3_LOW) & 31U))
    return false;

  // BAR2 targets physical RAM at zero; PCIe base is the deliberate 4 GiB
  // override. The 4 GiB aperture admits only the lower 3 GiB in software.
  host_write(INBOUND2_LOW, RAM_SIZE_CODE);
  host_write(INBOUND2_HIGH, RAM_PCI_ADDRESS >> 32);
  host_change(DMA_CONTROL, SCB0_FIELD, RAM_SIZE_CODE << 27);
  host_change(BAR2_ENDIAN, ENDIAN_FIELD, 0);
  if (host_read(INBOUND2_LOW) != RAM_SIZE_CODE ||
      host_read(INBOUND2_HIGH) != (RAM_PCI_ADDRESS >> 32) ||
      (host_read(DMA_CONTROL) & SCB0_FIELD) != (RAM_SIZE_CODE << 27) ||
      (host_read(BAR2_ENDIAN) & ENDIAN_FIELD))
    return false;
  ram_window_configured = true;
  return true;
}

bool pcie_phys_to_dma(uint64_t physical, uint64_t length, uint64_t *address) {
  if (!address || !ram_window_configured || !length ||
      physical >= RAM_ADDRESS_CEILING)
    return false;
  if (length > RAM_ADDRESS_CEILING - physical)
    return false;
  *address = physical + RAM_PCI_ADDRESS;
  return true;
}

bool pcie_xhci_get_firmware_version(uint32_t *version) {
  if (!version || !resources_assigned)
    return false;
  pcie_info_t info;
  pcie_probe(&info);
  if (!is_vl805(&info))
    return false;
  *version = config_read(info.secondary_bus, CFG_FIRMWARE);
  return true;
}

// Each standard capability occupies a DWORD-aligned position in [0x40,0xfc].
// A 64-bit visited bitmap bounds traversal without trusting endpoint pointers.
typedef struct {
  uint64_t visited;
  uint8_t next;
} capability_cursor_t;

static int capability_read(uint8_t bus, capability_cursor_t *cursor,
    uint8_t *position, uint32_t *header) {
  unsigned int offset = cursor->next;
  if (!offset)
    return 0;
  if (offset < 0x40U || (offset & 3U))
    return -1;
  uint64_t bit = 1ULL << (offset / 4);
  if (cursor->visited & bit)
    return -1;
  cursor->visited |= bit;
  *position = offset;
  *header = config_read(bus, offset);
  cursor->next = (uint8_t)(*header >> 8);
  return 1;
}

bool pcie_xhci_diagnose(pcie_xhci_diagnostic_t *result) {
  uint8_t bus;
  if (!result || !mapped_endpoint(&bus))
    return false;
  *result = (pcie_xhci_diagnostic_t){0};
  result->class_revision = config_read(bus, CFG_CLASS);
  result->command_status = config_read(bus, CFG_COMMAND);
  result->header = config_read(bus, CFG_HEADER);
  result->capabilities_present =
      ((result->command_status >> 16) & STATUS_CAPABILITIES) != 0;
  if (!result->capabilities_present)
    return true;
  result->capability_pointer = config_read(bus, CFG_CAPABILITIES);
  capability_cursor_t cursor = {.next = result->capability_pointer & 0xfcU};
  uint8_t offset;
  uint32_t header;
  int step;
  while ((step = capability_read(bus, &cursor, &offset, &header)) > 0) {
    if ((uint8_t)header != CAP_PM)
      continue;
    if (offset > 0xf8U)
      return false;
    result->pm_present = true;
    result->pm_offset = offset;
    result->pm_capabilities = (uint16_t)(header >> 16);
    result->pm_control = config_half(bus, offset + 4);
    return true;
  }
  return step == 0;
}

bool pcie_xhci_get_interrupt_info(pcie_xhci_interrupt_info_t *info) {
  uint8_t bus;
  if (!info || !mapped_endpoint(&bus))
    return false;
  *info = (pcie_xhci_interrupt_info_t){0};
  uint32_t irq = config_read(bus, CFG_INTERRUPT);
  info->interrupt_line = (uint8_t)irq;
  info->interrupt_pin = (uint8_t)(irq >> 8);
  info->capabilities_present =
      (config_half(bus, CFG_COMMAND + 2) & STATUS_CAPABILITIES) != 0;
  if (!info->capabilities_present)
    return true;
  capability_cursor_t cursor = {.next = config_read(bus, CFG_CAPABILITIES)};
  uint8_t offset;
  uint32_t header;
  int step;
  while ((step = capability_read(bus, &cursor, &offset, &header)) > 0) {
    uint16_t control = (uint16_t)(header >> 16);
    info->capabilities_scanned++;
    switch ((uint8_t)header) {
      case CAP_MSI:
        if (info->msi_present)
          return false;
        info->msi_present = true;
        info->msi_offset = offset;
        info->msi_control = control;
        info->msi_enabled = (control & MSI_ENABLE) != 0;
        info->msi_64bit = (control & MSI_WIDE_ADDRESS) != 0;
        break;
      case CAP_MSIX:
        if (info->msix_present)
          return false;
        info->msix_present = true;
        info->msix_offset = offset;
        info->msix_control = control;
        info->msix_enabled = (control & 0x8000U) != 0;
        info->msix_masked = (control & 0x4000U) != 0;
        break;
    }
  }
  return step == 0;
}

// Validate the entire 64-bit MSI message before touching its data fields.
static bool msi_endpoint(uint8_t *bus, pcie_xhci_interrupt_info_t *info) {
  return pcie_xhci_get_interrupt_info(info) && info->msi_present &&
      info->msi_64bit && info->msi_offset <= 0xf0U && mapped_endpoint(bus);
}

bool pcie_xhci_msi_get_message(pcie_xhci_msi_message_t *message) {
  uint8_t bus;
  pcie_xhci_interrupt_info_t info;
  if (!message || !msi_endpoint(&bus, &info))
    return false;
  unsigned int offset = info.msi_offset;
  message->address_lo = config_read(bus, offset + 4);
  message->address_hi = config_read(bus, offset + 8);
  message->data = config_half(bus, offset + 12);
  message->control = info.msi_control;
  return true;
}

bool pcie_msi_host_probe(pcie_msi_host_info_t *info) {
  if (!info || !link_started)
    return false;
  *info = (pcie_msi_host_info_t){0};
  if (!(host_read(LINK_STATE) & MODE_ROOT))
    return false;
  info->revision = host_read(REVISION);
  info->target_lo = host_read(MSI_TARGET_LOW);
  info->target_hi = host_read(MSI_TARGET_HIGH);
  info->data_config = host_read(MSI_MATCH_DATA);
  info->interrupt_status = host_read(MSI_PENDING);
  return true;
}

static bool msi_receiver_ready(void) {
  pcie_msi_host_info_t state;
  return pcie_msi_host_probe(&state) && state.revision == MSI_REVISION &&
      state.target_lo == ((uint32_t)MSI_ADDRESS | 1U) &&
      state.target_hi == (MSI_ADDRESS >> 32) &&
      state.data_config == MSI_MATCH &&
      state.interrupt_status == 0;
}

pcie_msi_prepare_result_t pcie_msi_host_prepare(void) {
  if (!link_started || !resources_assigned || !ram_window_configured)
    return PCIE_MSI_PREPARE_NOT_READY;
  if (host_read(REVISION) != MSI_REVISION)
    return PCIE_MSI_PREPARE_UNSUPPORTED_REVISION;
  pcie_xhci_interrupt_info_t endpoint;
  if (!pcie_xhci_get_interrupt_info(&endpoint) || !endpoint.msi_present ||
      endpoint.msi_enabled || endpoint.msix_enabled)
    return PCIE_MSI_PREPARE_ENDPOINT_STATE;

  // Host receiver mechanics from FreeBSD; OrgOS arms only vector zero later.
  host_write(MSI_MASK, UINT32_MAX);
  host_write(MSI_ACK, UINT32_MAX);
  host_write(MSI_TARGET_HIGH, MSI_ADDRESS >> 32);
  host_write(MSI_MATCH_DATA, MSI_MATCH);
  host_write(MSI_TARGET_LOW, (uint32_t)MSI_ADDRESS | 1U);
  if (msi_receiver_ready())
    return PCIE_MSI_PREPARE_OK;
  host_write(MSI_MASK, UINT32_MAX);
  host_write(MSI_TARGET_LOW, (uint32_t)MSI_ADDRESS);
  return PCIE_MSI_PREPARE_VERIFY_FAILED;
}

uint32_t pcie_msi_host_irq_quiesce(void) {
  // The GIC route is closed by irq.c; foreground USB service later rearms it.
  // Retain the tested mask/clear contract, without FreeBSD's extra host EOI.
  host_write(MSI_MASK, UINT32_MAX);
  uint32_t pending = host_read(MSI_PENDING);
  if (pending)
    host_write(MSI_ACK, pending);
  pcie_barrier();
  return pending;
}

bool pcie_msi_host_unmask_vector0(void) {
  if (!msi_receiver_ready())
    return false;
  host_write(MSI_UNMASK, 1U);
  return true;
}

bool pcie_xhci_msi_program(void) {
  uint8_t bus;
  pcie_xhci_interrupt_info_t endpoint;
  if (!msi_endpoint(&bus, &endpoint) || endpoint.msi_enabled ||
      endpoint.msix_enabled || (endpoint.msi_control & MSI_MULTIPLE_ENABLE) ||
      !msi_receiver_ready())
    return false;
  unsigned int offset = endpoint.msi_offset;
  config_write(bus, offset + 4, (uint32_t)MSI_ADDRESS);
  config_write(bus, offset + 8, MSI_ADDRESS >> 32);
  config_write_half(bus, offset + 12, MSI_VECTOR_DATA);
  pcie_xhci_msi_message_t message;
  return pcie_xhci_msi_get_message(&message) &&
      message.address_lo == (uint32_t)MSI_ADDRESS &&
      message.address_hi == (MSI_ADDRESS >> 32) &&
      message.data == MSI_VECTOR_DATA &&
      message.control == endpoint.msi_control;
}

bool pcie_xhci_msi_set_enabled(bool enabled) {
  uint8_t bus;
  pcie_xhci_interrupt_info_t endpoint;
  if (!msi_endpoint(&bus, &endpoint) || endpoint.msix_enabled)
    return false;
  if (enabled) {
    pcie_xhci_msi_message_t message;
    if (endpoint.msi_enabled || (endpoint.msi_control & MSI_MULTIPLE_ENABLE) ||
        !pcie_xhci_msi_get_message(&message) ||
        message.address_lo != (uint32_t)MSI_ADDRESS ||
        message.address_hi != (MSI_ADDRESS >> 32) ||
        message.data != MSI_VECTOR_DATA)
      return false;
  }
  uint16_t control = endpoint.msi_control & (uint16_t)~MSI_ENABLE;
  if (enabled)
    control |= MSI_ENABLE;
  config_write_half(bus, endpoint.msi_offset + 2, control);
  pcie_xhci_interrupt_info_t verify;
  return pcie_xhci_get_interrupt_info(&verify) && verify.msi_enabled == enabled;
}
