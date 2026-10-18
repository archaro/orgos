#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "timer.h"
#include "mmio.h"
#include "pcie.h"
#include "xhci.h"
#include "dma.h"
#include "string.h"

// The VL805 BAR has been assigned PCI bus address
// 0xC0000000, which the BCM2711 outbound window
// maps to CPU physical address 0x600000000.
// This is only valid AFTER pcie_map_xhci().
#define XHCI_BASE 0x600000000ULL

// xHCI capability register offsets.
#define XHCI_CAPLENGTH_HCIVERSION 0x00U
#define XHCI_HCSPARAMS1           0x04U
#define XHCI_HCSPARAMS2           0x08U
#define XHCI_HCSPARAMS3           0x0cU
#define XHCI_HCCPARAMS1           0x10U
#define XHCI_HCCPARAMS2           0x1cU
#define XHCI_DBOFF                0x14U
#define XHCI_RTSOFF               0x18U

// Operational registers are relative to CAPLENGTH.
#define XHCI_USBCMD     0x00U
#define XHCI_USBSTS     0x04U
#define XHCI_PAGESIZE   0x08U
#define XHCI_CRCR_LO    0x18U
#define XHCI_CRCR_HI    0x1cU
#define XHCI_DCBAAP_LO  0x30U
#define XHCI_DCBAAP_HI  0x34U
#define XHCI_CONFIG     0x38U

#define XHCI_STS_HALTED   (1U << 0)
#define XHCI_STS_CNR      (1U << 11)
#define XHCI_CMD_RUN      (1U << 0)
#define XHCI_CMD_HCRST    (1U << 1)

#define PCI_CMD_BUS_MASTER (1U << 2)

#define XHCI_RESET_TIMEOUT_US  1000000ULL
#define XHCI_READY_TIMEOUT_US  1000000ULL

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

  // Operational registers must begin at a valid,
  // DWORD-aligned offset.
  // Firmware initialisation can temporarily make
  // capability-register reads unreliable. Never
  // use an unvalidated CAPLENGTH to form an
  // MMIO address.
  uint8_t caplength = (uint8_t)(cap & 0xffU);
  if (caplength < 0x20U || (caplength & 3U) != 0)
  return false;

  info->capability_length = caplength;
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

bool xhci_get_state(xhci_state_t *state) {
  if (!state)
    return false;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return false;

  // We expect an xHCI operational-register
  // block beginning at the reported CAPLENGTH.
  if (info.capability_length < 0x20U || (info.capability_length & 3U) != 0)
    return false;

  uint32_t base = info.capability_length;
  state->command = xhci_read(base + XHCI_USBCMD);
  state->status = xhci_read(base + XHCI_USBSTS);
  state->pagesize = xhci_read(base + XHCI_PAGESIZE);
  state->config = xhci_read(base + XHCI_CONFIG);

  uint32_t lo = xhci_read(base + XHCI_CRCR_LO);
  uint32_t hi = xhci_read(base + XHCI_CRCR_HI);
  state->command_ring = ((uint64_t)hi << 32) | lo;
  lo = xhci_read(base + XHCI_DCBAAP_LO);
  hi = xhci_read(base + XHCI_DCBAAP_HI);
  state->dcbaa = ((uint64_t)hi << 32) | lo;
  return true;
}

#define XHCI_RING_TRBS 256U
#define XHCI_TRB_LINK  6U
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_TOGGLE_CYCLE (1U << 1)

// The xHCI structures have fixed hardware layouts.
_Static_assert(sizeof(xhci_trb_t) == 16, "xHCI TRB must be 16 bytes");
_Static_assert(sizeof(xhci_erst_entry_t) == 16,
    "xHCI ERST entry must be 16 bytes");

bool xhci_dma_release(xhci_dma_t *dma) {
  if (!dma)
    return false;

  bool ok = true;

  // All buffers must be CPU-owned before release.
  if (dma->dcbaa.pages && !dma_free(&dma->dcbaa))
    ok = false;

  if (dma->erst.pages && !dma_free(&dma->erst))
    ok = false;

  if (dma->event_ring.pages && !dma_free(&dma->event_ring))
    ok = false;

  if (dma->command_ring.pages && !dma_free(&dma->command_ring))
    ok = false;

  return ok;
}

bool xhci_dma_prepare(xhci_dma_t *dma) {
  if (!dma)
    return false;

  *dma = (xhci_dma_t){0};
  if (!dma_alloc(PAGE_SIZE, 64, &dma->command_ring))
    goto fail;

  if (!dma_alloc(PAGE_SIZE, 64, &dma->event_ring))
    goto fail;

  if (!dma_alloc(PAGE_SIZE, 64, &dma->erst))
    goto fail;

  if (!dma_alloc(PAGE_SIZE, 64, &dma->dcbaa))
    goto fail;

  // Configure a one-segment command ring.
  // The final TRB links back to the start.
  // Toggle Cycle ensures that wrapping the ring
  // changes its producer cycle state.
  xhci_trb_t *commands = (xhci_trb_t *)dma->command_ring.virtual;
  xhci_trb_t *link = &commands[XHCI_RING_TRBS - 1U];
  link->parameter = dma->command_ring.bus;
  link->status = 0;
  link->control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
      XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;

  // The event ring is initially zeroed.
  // Its producer cycle state is established
  // by the controller when ERSTBA is programmed.
  xhci_erst_entry_t *erst = (xhci_erst_entry_t *)dma->erst.virtual;
  erst[0].base = dma->event_ring.bus;
  erst[0].size = XHCI_RING_TRBS;
  erst[0].reserved = 0;
  // DCBAA entries remain zero. We have not yet
  // enabled any device slots.
  return true;
fail:
  xhci_dma_release(dma);
  return false;
}

bool xhci_check_readiness(xhci_readiness_t *result) {
  if (!result)
    return false;

  xhci_info_t info;

  if (!xhci_probe(&info))
    return false;

  uint32_t params = xhci_read(XHCI_HCSPARAMS2);
  result->hcsparams2 = params;

  // HCSPARAMS2 scratchpad count is split between
  // bits [25:21] and [31:27].
  result->scratchpad_buffers = (((params >> 21) & 0x1fU) << 5) |
      ((params >> 27) & 0x1fU);

  // Poll CNR for up to one second.
  // Do not write any controller registers.
  uint64_t deadline = timer_now_us() + 1000000ULL;
  uint32_t status;
  do {
    status = xhci_read(info.capability_length + XHCI_USBSTS);
    if (!(status & XHCI_STS_CNR))
      break;
    timer_delay_ms(1);
  } while (timer_now_us() < deadline);

  result->ready = (status & XHCI_STS_CNR) == 0;
  result->halted = (status & XHCI_STS_HALTED) != 0;
  return true;
}

bool xhci_diagnose_capabilities( xhci_cap_diagnostic_t *result) {

  if (!result)
    return false;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return false;

  *result = (xhci_cap_diagnostic_t){0};
  result->hcsparams1 = xhci_read(XHCI_HCSPARAMS1);
  result->hcsparams2 = xhci_read(XHCI_HCSPARAMS2);
  result->hcsparams3 = xhci_read(XHCI_HCSPARAMS3);
  result->hccparams1 = xhci_read(XHCI_HCCPARAMS1);

  // HCCPARAMS2 exists in xHCI 1.1 and later.
  // The VL805 reports xHCI 1.0, so do not
  // read offset 0x1c as HCCPARAMS2.
  if (info.version >= 0x0110U)
    result->hccparams2 = xhci_read(XHCI_HCCPARAMS2);

  // HCCPARAMS1[31:16] contains the extended
  // capabilities pointer, in DWORD units.
  uint32_t xecp = (result->hccparams1 >> 16) & 0xffffU;
  result->ext_cap_offset = xecp * 4U;

  // Read the first capability header if the
  // pointer lies inside the initial 4 KiB BAR
  // region used by our current diagnostics.
  if (result->ext_cap_offset >= 0x20U && result->ext_cap_offset <= 0xffcU) {
    result->ext_cap_header = xhci_read(result->ext_cap_offset);
  }

  return true;
}

xhci_reset_result_t xhci_reset(xhci_reset_info_t *result) {
  if (!result)
    return XHCI_RESET_UNAVAILABLE;

  *result = (xhci_reset_info_t){0};

  // Require a configured PCIe link and VL805 BAR.
  if (!pcie_xhci_ready())
    return XHCI_RESET_UNAVAILABLE;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return XHCI_RESET_UNAVAILABLE;

  if (info.capability_length < 0x20U || (info.capability_length & 3U))
    return XHCI_RESET_UNAVAILABLE;

  // Never reset a controller which is permitted
  // to perform PCIe bus-master transactions.
  uint16_t pci_command;

  if (!pcie_xhci_get_command(&pci_command))
    return XHCI_RESET_UNAVAILABLE;

  if (pci_command & PCI_CMD_BUS_MASTER)
    return XHCI_RESET_BUS_MASTER_ENABLED;

  uint32_t base = info.capability_length;
  uintptr_t command_address = (uintptr_t)(XHCI_BASE + base + XHCI_USBCMD);
  result->command_before = xhci_read(base + XHCI_USBCMD);
  result->status_before = xhci_read(base + XHCI_USBSTS);
  // HCRST may only be initiated while halted.
  if (!(result->status_before & XHCI_STS_HALTED))
    return XHCI_RESET_NOT_HALTED;

  // Do not interfere with a running or already
  // resetting controller.
  if (result->command_before & (XHCI_CMD_RUN | XHCI_CMD_HCRST))
    return XHCI_RESET_ALREADY_ACTIVE;

  // Issue exactly one software host-controller reset.
  //
  // Preserve the other USBCMD bits. Hardware clears
  // HCRST when the reset operation completes.
  mmio_write(command_address, result->command_before | XHCI_CMD_HCRST);

  __asm__ volatile("dsb sy" ::: "memory");
  uint64_t started = timer_now_us();
  uint64_t deadline = started + XHCI_RESET_TIMEOUT_US;

  // First wait for HCRST to clear.
  do {
    result->command_after = xhci_read(base + XHCI_USBCMD);
    if (!(result->command_after & XHCI_CMD_HCRST))
      break;
    timer_delay_ms(1);
  } while (timer_now_us() < deadline);

  result->reset_elapsed_us = timer_now_us() - started;
  result->status_after = xhci_read(base + XHCI_USBSTS);
  if (result->command_after & XHCI_CMD_HCRST)
    return XHCI_RESET_TIMEOUT;

  // HCRST has cleared. The controller may still
  // be internally initialising, so now wait for
  // Controller Not Ready to clear.
  started = timer_now_us();
  deadline = started + XHCI_READY_TIMEOUT_US;

  do {
    result->status_after = xhci_read(base + XHCI_USBSTS);
    if (!(result->status_after & XHCI_STS_CNR))
      break;
    timer_delay_ms(1);
  } while (timer_now_us() < deadline);

  result->ready_elapsed_us = timer_now_us() - started;
  result->command_after = xhci_read(base + XHCI_USBCMD);
  if (result->status_after & XHCI_STS_CNR)
    return XHCI_RESET_READY_TIMEOUT;

  // Following reset, the controller should
  // remain halted until explicitly started.
  if (!(result->status_after & XHCI_STS_HALTED))
    return XHCI_RESET_NOT_HALTED;

  return XHCI_RESET_OK;
}
