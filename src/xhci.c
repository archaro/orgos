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

// Interrupter registers are relative to the
// interrupter's runtime register block.
#define XHCI_INTR0_OFFSET 0x20U

#define XHCI_IMAN        0x00U
#define XHCI_ERSTSZ      0x08U
#define XHCI_ERSTBA_LO   0x10U
#define XHCI_ERSTBA_HI   0x14U
#define XHCI_ERDP_LO     0x18U
#define XHCI_ERDP_HI     0x1cU

#define XHCI_STS_HALTED   (1U << 0)
#define XHCI_STS_CNR      (1U << 11)
#define XHCI_CMD_RUN      (1U << 0)
#define XHCI_CMD_HCRST    (1U << 1)

#define PCI_CMD_BUS_MASTER (1U << 2)

#define XHCI_RESET_TIMEOUT_US  1000000ULL
#define XHCI_READY_TIMEOUT_US  1000000ULL

static xhci_dma_t controller_dma;
static bool controller_configured;
static bool controller_verified;
static bool controller_running;

static uint32_t xhci_read(uint32_t offset) {
  return mmio_read((uintptr_t)(XHCI_BASE + offset));
}

static void xhci_write(uint32_t offset, uint32_t value) {
  mmio_write((uintptr_t)(XHCI_BASE + offset), value);
}

static void xhci_write64(uint32_t offset, uint64_t value) {
  xhci_write(offset, (uint32_t)value);
  xhci_write(offset + 4U, (uint32_t)(value >> 32));
}

static uint64_t xhci_read64(uint32_t offset) {
  uint32_t lo = xhci_read(offset);
  uint32_t hi = xhci_read(offset + 4U);
  return ((uint64_t)hi << 32) | lo;
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

  if (dma->scratchpad_count > XHCI_MAX_SCRATCHPADS)
    return false;

  bool ok = true;

  // Release scratchpad pages before their pointer array.
  // Each allocation must still belong to the CPU.
  for (unsigned int i = 0;
      i < dma->scratchpad_count; i++) {
    if (dma->scratchpads[i].pages && !dma_free(&dma->scratchpads[i]))
      ok = false;
  }

  if (dma->scratchpad_array.pages && !dma_free(&dma->scratchpad_array))
    ok = false;

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

  // Discover the controller's actual scratchpad
  // requirement from HCSPARAMS2.
  if (!pcie_xhci_ready())
    return false;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return false;

  uint32_t params = xhci_read(XHCI_HCSPARAMS2);
  unsigned int scratchpad_count =
      (((params >> 21) & 0x1fU) << 5) | ((params >> 27) & 0x1fU);

  if (scratchpad_count > XHCI_MAX_SCRATCHPADS)
    return false;

  // Existing controller structures.
  if (!dma_alloc(PAGE_SIZE, 64, &dma->command_ring))
    goto fail;

  if (!dma_alloc(PAGE_SIZE, 64, &dma->event_ring))
    goto fail;

  if (!dma_alloc(PAGE_SIZE, 64, &dma->erst))
    goto fail;

  if (!dma_alloc(PAGE_SIZE, 64, &dma->dcbaa))
    goto fail;

  // Configure the command ring's final Link TRB.
  xhci_trb_t *commands = (xhci_trb_t *)dma->command_ring.virtual;
  xhci_trb_t *link = &commands[XHCI_RING_TRBS - 1U];
  link->parameter = dma->command_ring.bus;
  link->status = 0;
  link->control =
      (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
      XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;

  // Configure the single-segment event ring.
  xhci_erst_entry_t *erst = (xhci_erst_entry_t *)dma->erst.virtual;
  erst[0].base = dma->event_ring.bus;
  erst[0].size = XHCI_RING_TRBS;
  erst[0].reserved = 0;

  // The DCBAA, scratchpad pointer array and
  // scratchpad pages are initialised to zero by
  // dma_alloc().
  uint64_t *dcbaa = (uint64_t *)dma->dcbaa.virtual;
  if (scratchpad_count != 0) {
    // Allocate a complete page for the pointer array.
    // It must be 64-byte aligned.
    if (!dma_alloc(PAGE_SIZE, 64, &dma->scratchpad_array))
      goto fail;
    uint64_t *pointers = (uint64_t *)dma->scratchpad_array.virtual;
    // Allocate each scratchpad page separately.
    // Record each success immediately so partial
    // failure can be cleaned up correctly.
    for (unsigned int i = 0;
        i < scratchpad_count; i++) {
      if (!dma_alloc(PAGE_SIZE, PAGE_SIZE, &dma->scratchpads[i]))
        goto fail;
      dma->scratchpad_count++;
      // The controller must receive a PCIe bus
      // address, not a CPU virtual address.
      pointers[i] = dma->scratchpads[i].bus;
    }
    // DCBAA entry zero points to the scratchpad
    // pointer array, not to an individual page.
    dcbaa[0] = dma->scratchpad_array.bus;
  }
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
 
  if (controller_configured)
    return XHCI_RESET_ALREADY_ACTIVE; 

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

static void xhci_dma_cancel_handoff(xhci_dma_t *dma) {
  // Safe only before any DMA pointers have been
  // installed in the hardware registers.
  for (unsigned int i = 0; i < dma->scratchpad_count; i++) {
    if (dma->scratchpads[i].owner == DMA_OWNER_DEVICE)
      dma_end_device(&dma->scratchpads[i]);
  }

  dma_buffer_t *buffers[] = {
    &dma->scratchpad_array,
    &dma->dcbaa,
    &dma->erst,
    &dma->event_ring,
    &dma->command_ring
  };

  for (unsigned int i = 0;
      i < sizeof(buffers) / sizeof(buffers[0]); i++) {
    if (buffers[i]->owner == DMA_OWNER_DEVICE)
      dma_end_device(buffers[i]);
  }
}

static bool xhci_dma_handoff(xhci_dma_t *dma) {
  // Command ring and static pointer tables are
  // initially read by the device.
  if (!dma_begin_device(&dma->command_ring, DMA_TO_DEVICE))
    goto fail;

  // The event ring will be written by the device.
  if (!dma_begin_device(&dma->event_ring, DMA_FROM_DEVICE))
    goto fail;

  if (!dma_begin_device(&dma->erst, DMA_TO_DEVICE))
    goto fail;

  // The DCBAA initially contains pointers for the
  // device to read. Later device-context management
  // will require a more detailed ownership model.
  if (!dma_begin_device(&dma->dcbaa, DMA_TO_DEVICE))
    goto fail;
  if (dma->scratchpad_count != 0) {
    if (!dma_begin_device(
            &dma->scratchpad_array, DMA_TO_DEVICE))
      goto fail;
    for (unsigned int i = 0;
        i < dma->scratchpad_count; i++) {
      if (!dma_begin_device(
              &dma->scratchpads[i], DMA_BIDIRECTIONAL))
        goto fail;
    }
  }
  return true;

fail:
  xhci_dma_cancel_handoff(dma);
  return false;
}

xhci_configure_result_t xhci_configure(xhci_configure_info_t *result) {
  if (!result)
    return XHCI_CONFIGURE_UNAVAILABLE;

  *result = (xhci_configure_info_t){0};

  if (controller_configured)
    return XHCI_CONFIGURE_ALREADY;

  if (!pcie_xhci_ready() || !pcie_xhci_bar_valid())
    return XHCI_CONFIGURE_UNAVAILABLE;

  uint16_t pci_command;

  if (!pcie_xhci_get_command(&pci_command))
    return XHCI_CONFIGURE_UNAVAILABLE;

  // No controller DMA may be active.
  if (pci_command & PCI_CMD_BUS_MASTER)
    return XHCI_CONFIGURE_UNSAFE_STATE;

  xhci_info_t info;
  xhci_state_t state;

  if (!xhci_probe(&info) || !xhci_get_state(&state))
    return XHCI_CONFIGURE_UNAVAILABLE;

  if (!(state.status & XHCI_STS_HALTED) ||
      (state.status & XHCI_STS_CNR) ||
      (state.command & (XHCI_CMD_RUN | XHCI_CMD_HCRST)) ||
      !(state.pagesize & 1U) ||
      info.max_interrupters == 0 ||
      info.max_slots == 0)
    return XHCI_CONFIGURE_UNSAFE_STATE;

  if (!xhci_dma_prepare(&controller_dma))
    return XHCI_CONFIGURE_ALLOCATION_FAILED;

  if (!xhci_dma_handoff(&controller_dma)) {
    xhci_dma_release(&controller_dma);
    return XHCI_CONFIGURE_DMA_FAILED;
  }

  // These addresses must remain valid until the
  // controller has been safely decommissioned.
  //
  // From this point onwards, even a failed
  // verification must not release the allocations.
  controller_configured = true;

  uint32_t op = info.capability_length;
  uint32_t intr = info.runtime_offset + XHCI_INTR0_OFFSET;

  // Disable interrupter 0. Interrupts are not
  // required for this halted-only test.
  xhci_write(intr + XHCI_IMAN, 0);

  // Device Context Base Address Array.
  xhci_write64(op + XHCI_DCBAAP_LO, controller_dma.dcbaa.bus);

  // Command Ring Control Register.
  // Bit 0 is the initial Ring Cycle State.
  xhci_write64(op + XHCI_CRCR_LO, controller_dma.command_ring.bus | 1ULL);

  // Enable the controller's advertised slot capacity.
  xhci_write(op + XHCI_CONFIG, info.max_slots);

  // Configure interrupter 0's event ring.
  //
  // One event-ring segment, 256 TRBs.
  // Initialise its table and dequeue pointer while
  // the controller is still halted.
  xhci_write(intr + XHCI_ERSTSZ, 1U);
  xhci_write64(intr + XHCI_ERSTBA_LO, controller_dma.erst.bus);
  xhci_write64(intr + XHCI_ERDP_LO, controller_dma.event_ring.bus);
  __asm__ volatile("dsb sy" ::: "memory");

  // Record the programmed bus addresses.
  result->command_ring = controller_dma.command_ring.bus;
  result->event_ring = controller_dma.event_ring.bus;
  result->erst = controller_dma.erst.bus;
  result->dcbaa = controller_dma.dcbaa.bus;
  result->scratchpad_array = controller_dma.scratchpad_array.bus;
  result->scratchpads = controller_dma.scratchpad_count;

  // Read back the operational registers.
  result->command = xhci_read(op + XHCI_USBCMD);
  result->status = xhci_read(op + XHCI_USBSTS);
  result->config = xhci_read(op + XHCI_CONFIG);
  result->crcr = xhci_read64(op + XHCI_CRCR_LO);
  result->dcbaap = xhci_read64(op + XHCI_DCBAAP_LO);

  // Read back interrupter 0.
  result->iman = xhci_read(intr + XHCI_IMAN);
  result->erstsz = xhci_read(intr + XHCI_ERSTSZ);
  result->erstba = xhci_read64(intr + XHCI_ERSTBA_LO);
  result->erdp = xhci_read64(intr + XHCI_ERDP_LO);

  // Some low register bits contain flags rather
  // than address bits. Compare pointer fields.
  // CRCR's command-ring pointer and Ring Cycle State
  // are write-only from software's perspective.
  // Reading those fields returns zero.
  //
  // CRR (bit 3) must remain clear while the command
  // ring is stopped. Verify that instead.
  if ((result->crcr & (1ULL << 3)) != 0 ||
      (result->dcbaap & ~0x3fULL) != controller_dma.dcbaa.bus ||
      (result->config & 0xffU) != info.max_slots ||
      (result->erstsz & 0xffffU) != 1U ||
      (result->erstba & ~0x3fULL) != controller_dma.erst.bus ||
      (result->erdp & ~0xfULL) != controller_dma.event_ring.bus ||
      (result->iman & (1U << 1)) != 0 ||
      !(result->status & XHCI_STS_HALTED) ||
      (result->status & XHCI_STS_CNR) ||
      (result->command & XHCI_CMD_RUN))
    return XHCI_CONFIGURE_VERIFY_FAILED;

  controller_verified = true;
  return XHCI_CONFIGURE_OK;
}

#define XHCI_STS_HOST_ERROR (1U << 2)
#define XHCI_START_TIMEOUT_US 1000000ULL

xhci_start_result_t xhci_start(xhci_start_info_t *result) {

  if (!result)
    return XHCI_START_UNAVAILABLE;

  *result = (xhci_start_info_t){0};

  if (controller_running)
    return XHCI_START_ALREADY_RUNNING;

  if (!controller_configured || !controller_verified)
    return XHCI_START_NOT_CONFIGURED;

  if (!pcie_xhci_ready() || !pcie_xhci_bar_valid())
    return XHCI_START_UNAVAILABLE;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return XHCI_START_UNAVAILABLE;

  uint32_t op = info.capability_length;
  result->command_before = xhci_read(op + XHCI_USBCMD);
  result->status_before = xhci_read(op + XHCI_USBSTS);

  if (!pcie_xhci_get_command( &result->pci_command_before))
    return XHCI_START_UNAVAILABLE;

  // The controller must still be in the exact
  // halted state established by usb configure.
  if (!(result->status_before & XHCI_STS_HALTED) ||
      (result->status_before & XHCI_STS_CNR) ||
      (result->status_before & XHCI_STS_HOST_ERROR) ||
      (result->command_before & (XHCI_CMD_RUN | XHCI_CMD_HCRST)) ||
      (result->pci_command_before & PCI_CMD_BUS_MASTER))
    return XHCI_START_UNSAFE_STATE;

  // Verify the persistent configuration has not
  // disappeared since usb configure.
  if ((xhci_read64(op + XHCI_DCBAAP_LO) & ~0x3fULL) !=
          controller_dma.dcbaa.bus ||
      (xhci_read(op + XHCI_CONFIG) & 0xffU) != info.max_slots)
    return XHCI_START_UNSAFE_STATE;

  uint32_t intr = info.runtime_offset + XHCI_INTR0_OFFSET;
  if ((xhci_read(intr + XHCI_ERSTSZ) & 0xffffU) != 1U ||
      (xhci_read64(intr + XHCI_ERSTBA_LO) & ~0x3fULL) !=
          controller_dma.erst.bus ||
      (xhci_read64(intr + XHCI_ERDP_LO) & ~0xfULL) !=
          controller_dma.event_ring.bus ||
      (xhci_read(intr + XHCI_IMAN) & (1U << 1)))
    return XHCI_START_UNSAFE_STATE;

  // Check that the DMA buffers remain device-owned.
  if (controller_dma.command_ring.owner != DMA_OWNER_DEVICE ||
      controller_dma.event_ring.owner != DMA_OWNER_DEVICE ||
      controller_dma.erst.owner != DMA_OWNER_DEVICE ||
      controller_dma.dcbaa.owner != DMA_OWNER_DEVICE)
    return XHCI_START_UNSAFE_STATE;

  if (controller_dma.scratchpad_count != 0) {
    if (controller_dma.scratchpad_array.owner != DMA_OWNER_DEVICE)
      return XHCI_START_UNSAFE_STATE;

    for (unsigned int i = 0;
        i < controller_dma.scratchpad_count; i++) {
      if (controller_dma.scratchpads[i].owner != DMA_OWNER_DEVICE)
        return XHCI_START_UNSAFE_STATE;
    }
  }

  // Establish ordering before allowing the VL805
  // to access the prepared DMA memory.
  __asm__ volatile("dsb sy" ::: "memory");

  if (!pcie_xhci_enable_bus_master()) {
    // RS has not yet been asserted. Try to restore
    // the safe PCI state before reporting failure.
    pcie_xhci_disable_bus_master();
    pcie_xhci_get_command(&result->pci_command_after);
    return XHCI_START_PCI_FAILED;
  }

  // Bus mastering is now enabled, but the
  // controller is still halted.
  if (!pcie_xhci_get_command( &result->pci_command_after))
    return XHCI_START_PCI_FAILED;

  // Start the host controller. Keep interrupts
  // disabled; completion handling comes later.
  uint64_t started = timer_now_us();

  xhci_write(op + XHCI_USBCMD, result->command_before | XHCI_CMD_RUN);

  __asm__ volatile("dsb sy" ::: "memory");

  uint64_t deadline = started + XHCI_START_TIMEOUT_US;
  do {
    result->status_after = xhci_read(op + XHCI_USBSTS);
    if (result->status_after & XHCI_STS_HOST_ERROR)
      break;
    if (!(result->status_after & XHCI_STS_HALTED))
      break;
    timer_delay_ms(1);
  } while (timer_now_us() < deadline);

  result->elapsed_us = timer_now_us() - started;
  result->command_after = xhci_read(op + XHCI_USBCMD);

  // Capture the latest state before making
  // our final decision.
  result->status_after = xhci_read(op + XHCI_USBSTS);
  pcie_xhci_get_command(&result->pci_command_after);

  if (result->status_after & XHCI_STS_HOST_ERROR)
    return XHCI_START_CONTROLLER_ERROR;

  if ((result->status_after & XHCI_STS_HALTED) ||
      !(result->command_after & XHCI_CMD_RUN))
    return XHCI_START_TIMEOUT;

  controller_running = true;
  return XHCI_START_OK;
}

