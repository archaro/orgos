#include <stdint.h>
#include <stdbool.h>

#include "mmio.h"
#include "xhci.h"

// The VL805 BAR has been assigned PCI bus address
// 0xC0000000, which the BCM2711 outbound window
// maps to CPU physical address 0x600000000.
// This is only valid AFTER pcie_map_xhci().
#define XHCI_BASE 0x600000000ULL

// xHCI capability register offsets.
#define XHCI_CAPLENGTH_HCIVERSION 0x00
#define XHCI_HCSPARAMS1          0x04
#define XHCI_DBOFF               0x14
#define XHCI_RTSOFF              0x18

static uint32_t xhci_read(uint32_t offset) {
  return mmio_read((uintptr_t)(XHCI_BASE + offset));
}

bool xhci_probe(xhci_info_t *info) {
  if (!info)
    return false;

  *info = (xhci_info_t){0};

  // CAPLENGTH and HCIVERSION occupy the first
  // 32-bit capability register.
  // Bits  7:0  = CAPLENGTH
  // Bits 31:16 = HCIVERSION
  uint32_t cap = xhci_read(
      XHCI_CAPLENGTH_HCIVERSION);

  if (cap == 0xffffffffU || cap == 0)
    return false;

  info->capability_length = (uint8_t)(cap & 0xffU);
  info->version = (uint16_t)(cap >> 16);

  // HCSPARAMS1 contains:
  // Bits  7:0  = Maximum device slots
  // Bits 18:8  = Maximum interrupters
  // Bits 31:24 = Maximum ports
  uint32_t params = xhci_read(XHCI_HCSPARAMS1);

  info->max_slots = (uint8_t)(params & 0xffU);
  info->max_interrupters =
      (uint16_t)((params >> 8) & 0x7ffU);
  info->max_ports = (uint8_t)(params >> 24);

  // The doorbell and runtime register blocks
  // are at controller-reported offsets.
  info->doorbell_offset =
      xhci_read(XHCI_DBOFF) & ~3U;

  info->runtime_offset =
      xhci_read(XHCI_RTSOFF) & ~0x1fU;

  return true;
}

