#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "timer.h"
#include "mmio.h"
#include "cache.h"
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
#define XHCI_EXT_CAP_PROTOCOL     2U
#define XHCI_EXT_CAP_LIMIT        0x1000U
#define XHCI_EXT_CAP_MAX_HOPS     64U

// Operational registers are relative to CAPLENGTH.
#define XHCI_USBCMD     0x00U
#define XHCI_USBSTS     0x04U
#define XHCI_PAGESIZE   0x08U
#define XHCI_CRCR_LO    0x18U
#define XHCI_CRCR_HI    0x1cU
#define XHCI_DCBAAP_LO  0x30U
#define XHCI_DCBAAP_HI  0x34U
#define XHCI_CONFIG     0x38U
#define XHCI_PORTSC_BASE  0x400U
#define XHCI_PORT_STRIDE  0x10U

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
#define XHCI_STS_HOST_ERROR (1U << 2)
#define XHCI_CMD_RUN      (1U << 0)
#define XHCI_CMD_HCRST    (1U << 1)

#define PCI_CMD_BUS_MASTER (1U << 2)

#define XHCI_PORT_CCS     (1U << 0)
#define XHCI_PORT_PED     (1U << 1)
#define XHCI_PORT_OCA     (1U << 3)
#define XHCI_PORT_PR      (1U << 4)
#define XHCI_PORT_PP      (1U << 9)
#define XHCI_PORT_CSC     (1U << 17)
#define XHCI_PORT_PEC     (1U << 18)
#define XHCI_PORT_WRC     (1U << 19)
#define XHCI_PORT_OCC     (1U << 20)
#define XHCI_PORT_PRC     (1U << 21)
#define XHCI_PORT_PLC     (1U << 22)
#define XHCI_PORT_CEC     (1U << 23)

#define XHCI_RESET_TIMEOUT_US  1000000ULL
#define XHCI_READY_TIMEOUT_US  1000000ULL
#define XHCI_START_TIMEOUT_US 1000000ULL
#define XHCI_PORT_RESET_TIMEOUT_US 1000000ULL

#define XHCI_BULK_IN_DCI   3U
#define XHCI_BULK_OUT_DCI  4U
#define XHCI_EP_TYPE_OUT   2U
#define XHCI_EP_TYPE_IN    6U
#define XHCI_EP_TYPE_INTERRUPT_IN 7U
#define XHCI_EP_STATE_HALTED 2U
#define XHCI_EP_STATE_ERROR  4U
#define XHCI_COMPLETION_SHORT_PACKET 13U

typedef struct {
  // Operations which must not be retried after
  // an uncertain controller completion.
  bool slot_requested;
  bool address_attempted;
  bool ep0_update_attempted;
  bool bulk_attempted;
  bool disable_attempted;
  bool stop_out_attempted;
  bool stop_in_attempted;
  // State established by controlled teardown.
  bool quiesced;
  bool slot_disabled;
  bool cleaned;
} xhci_device_lifecycle_t;

// The full waiter definition appears below.
// A receiver retains a pointer to its outstanding
// completion owner, not a copy of that owner.
typedef struct xhci_event_waiter xhci_event_waiter_t;

typedef struct {
  // Current receive transfer.
  xhci_bulk_in_state_t state;
  xhci_bulk_in_info_t info;
  xhci_event_waiter_t *waiter;
  // Completed transfers awaiting consumption.
  xhci_bulk_in_info_t queue[XHCI_BULK_IN_QUEUE_CAPACITY];
  unsigned int queue_head;
  unsigned int queue_count;
  // Continuous receiver state.
  bool started;
  bool failed;
  bool queued;
  // Statistics belong to this receiver's lifetime.
  xhci_bulk_in_stream_stats_t stats;
} xhci_bulk_in_receiver_t;

typedef struct {
  dma_buffer_t input;
  dma_buffer_t output;
  dma_buffer_t ep0_ring;
  dma_buffer_t bulk_in_ring;
  dma_buffer_t bulk_out_ring;
  dma_buffer_t bulk_in_buffer;
  dma_buffer_t control_buffer;
  uint8_t slot_id;
  uint8_t root_port;
  uint8_t hub_slot;
  uint8_t hub_port;
  uint8_t speed;
  uint8_t context_size;
  uint16_t ep0_max_packet;
  uint32_t route_string;
  unsigned int ep0_index;
  unsigned int ep0_cycle;
  bool ep0_wrap_pending;
  bool ep0_failed;
  bool ep0_active;
  bool ep0_submitted;
  bool ep0_completion_seen;
  uint8_t ep0_completion_code;
  bool prepared;
  bool addressed;
  xhci_device_info_t info;
  dma_buffer_t bulk_out_buffer;
  unsigned int bulk_out_index;
  unsigned int bulk_out_cycle;
  bool bulk_out_ready;
  bool bulk_out_active;
  bool bulk_out_failed;
  bool bulk_out_wrap_pending;
  unsigned int bulk_in_index;
  unsigned int bulk_in_cycle;
  bool bulk_in_ready;
  bool bulk_in_wrap_pending;
  dma_buffer_t interrupt_ring;
  dma_buffer_t interrupt_buffer;
  uint8_t interrupt_dci;
  uint8_t interrupt_interval;
  uint16_t interrupt_mps;
  bool interrupt_ready;
  unsigned int interrupt_index;
  unsigned int interrupt_cycle;
  bool interrupt_wrap_pending;
  xhci_device_lifecycle_t lifecycle;
  xhci_bulk_in_receiver_t receiver;
} xhci_device_t;

typedef enum {
  XHCI_EVENT_NONE,
  XHCI_EVENT_READY,
  XHCI_EVENT_ERROR
} xhci_event_result_t;

typedef struct {
  uint64_t parameter;
  uint32_t status;
  uint32_t control;
  uint8_t type;
  uint8_t completion_code;
  uint8_t slot_id;
  uint8_t port_id;
} xhci_event_t;

static bool hub_configuration_attempted;
static bool hub_configured;
static bool hub_interrupt_attempted;
#define XHCI_MAX_DEVICES 2U
static xhci_device_t devices[XHCI_MAX_DEVICES];
#define internal_hub devices[0]
static bool address_attempted;
static bool device_addressed;
static xhci_address_info_t address_info;
static xhci_dma_t controller_dma;
static bool controller_configured;
static bool controller_verified;
static bool controller_running;
// Single-producer, single-consumer, sequential commands.
// These are persistent for the lifetime of the controller.
static unsigned int command_index;
static unsigned int event_index;
static unsigned int command_cycle = 1U;
static unsigned int event_cycle = 1U;
static uint32_t completed_commands;
static bool command_outstanding;
static bool command_ring_failed;
static xhci_event_stats_t event_stats;

#define XHCI_PORT_EVENT_QUEUE_CAPACITY 16U
static xhci_event_t port_event_queue[XHCI_PORT_EVENT_QUEUE_CAPACITY];
static unsigned int port_event_count;

#define XHCI_EVENT_WAITERS 8U
struct xhci_event_waiter {
  bool active;
  bool completed;
  uint8_t type;
  uint8_t slot_id;
  uint8_t endpoint_id;
  uint64_t trb_address;
  uint64_t td_start;
  xhci_event_t event;
};
static xhci_event_waiter_t event_waiters[XHCI_EVENT_WAITERS];

typedef struct {
  bool armed;
  bool received;
  uint8_t slot_id;
  uint8_t endpoint_id;
  uint8_t completion_code;
  uint64_t trb_address;
} xhci_stop_event_state_t;
static xhci_stop_event_state_t stop_out_event;
static xhci_stop_event_state_t stop_in_event;

static xhci_hub_notify_info_t hub_notify_info;
static xhci_event_waiter_t *hub_notify_waiter;
static bool hub_notify_started;

static bool slot_requested;
static uint8_t internal_hub_slot;

// Forward-reference
static void xhci_bulk_in_stream_service_device(xhci_device_t *device);

static xhci_device_t *xhci_device_by_slot(uint8_t slot_id) {
  if (slot_id == 0U)
    return NULL;

  xhci_device_t *found = NULL;
  for (unsigned int i = 0U; i < XHCI_MAX_DEVICES; i++) {
    if (devices[i].slot_id != slot_id)
      continue;
    // Slot identity must be unique. An ambiguous
    // owner must never receive a hardware event.
    if (found != NULL)
      return NULL;
    found = &devices[i];
  }

  return found;
}

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

bool xhci_port_count(uint8_t *count) {
  if (!count || !controller_running || !pcie_xhci_ready())
    return false;

  xhci_info_t info;
  if (!xhci_probe(&info) || info.max_ports == 0)
    return false;

  *count = info.max_ports;
  return true;
}

bool xhci_port_read(uint8_t number, xhci_port_t *port) {
  if (!port)
    return false;

  *port = (xhci_port_t){0};

  uint8_t count;
  if (!xhci_port_count(&count) || number == 0 || number > count)
    return false;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return false;

  uint32_t offset = info.capability_length + XHCI_PORTSC_BASE +
      (uint32_t)(number - 1U) * XHCI_PORT_STRIDE;

  uint32_t value = xhci_read(offset);
  port->number = number;
  port->portsc = value;
  port->connected = (value & XHCI_PORT_CCS) != 0;
  port->enabled = (value & XHCI_PORT_PED) != 0;
  port->overcurrent = (value & XHCI_PORT_OCA) != 0;
  port->resetting = (value & XHCI_PORT_PR) != 0;
  port->powered = (value & XHCI_PORT_PP) != 0;
  port->link_state = (uint8_t)((value >> 5) & 0x0fU);
  port->speed = (uint8_t)((value >> 10) & 0x0fU);
  port->indicator = (uint8_t)((value >> 14) & 0x03U);
  port->connect_change = (value & XHCI_PORT_CSC) != 0;
  port->enable_change = (value & XHCI_PORT_PEC) != 0;
  port->warm_reset_change = (value & XHCI_PORT_WRC) != 0;
  port->overcurrent_change = (value & XHCI_PORT_OCC) != 0;
  port->reset_change = (value & XHCI_PORT_PRC) != 0;
  port->link_change = (value & XHCI_PORT_PLC) != 0;
  port->config_error_change = (value & XHCI_PORT_CEC) != 0;

  return true;
}

xhci_port_clear_result_t xhci_port_clear(
    uint8_t number, xhci_port_clear_info_t *result) {

  if (!result)
    return XHCI_PORT_CLEAR_UNAVAILABLE;

  *result = (xhci_port_clear_info_t){0};

  if (!controller_running || !controller_verified)
    return XHCI_PORT_CLEAR_NOT_RUNNING;

  if (!pcie_xhci_ready() || !pcie_xhci_bar_valid())
    return XHCI_PORT_CLEAR_UNAVAILABLE;

  xhci_info_t info;
  if (!xhci_probe(&info) || number == 0U || number > info.max_ports)
    return XHCI_PORT_CLEAR_UNAVAILABLE;

  uint32_t op = info.capability_length;
  uint32_t usbcmd = xhci_read(op + XHCI_USBCMD);
  uint32_t usbsts = xhci_read(op + XHCI_USBSTS);
  if (!(usbcmd & XHCI_CMD_RUN) ||
      (usbsts & (XHCI_STS_HALTED | XHCI_STS_CNR | XHCI_STS_HOST_ERROR)))
    return XHCI_PORT_CLEAR_NOT_RUNNING;

  uint32_t offset = op + XHCI_PORTSC_BASE +
      (uint32_t)(number - 1U) * XHCI_PORT_STRIDE;

  result->before = xhci_read(offset);
  result->after = result->before;

  if (result->before & XHCI_PORT_PR)
    return XHCI_PORT_CLEAR_RESET_ACTIVE;

  // The seven change flags occupy bits 17-23.
  // Only bits currently set will be acknowledged.
  const uint32_t changes = XHCI_PORT_CSC | XHCI_PORT_PEC |
      XHCI_PORT_WRC | XHCI_PORT_OCC | XHCI_PORT_PRC |
      XHCI_PORT_PLC | XHCI_PORT_CEC;

  result->acknowledged = result->before & changes;

  if (result->acknowledged == 0)
    return XHCI_PORT_CLEAR_OK;

  // Preserve ordinary RW fields. Never write back PED, PR,
  // LWS or WPR; those bits have control side effects.
  const uint32_t preserve = XHCI_PORT_PP | (3U << 14) | (7U << 25);
  uint32_t value = (result->before & preserve) | result->acknowledged;

  xhci_write(offset, value);
  __asm__ volatile("dsb sy" ::: "memory");

  result->after = xhci_read(offset);
  // A concurrent hardware change could set a new flag here.
  // Report the actual result rather than silently retrying.
  if (result->after & result->acknowledged)
    return XHCI_PORT_CLEAR_FAILED;

  return XHCI_PORT_CLEAR_OK;
}

xhci_port_reset_result_t xhci_port_reset(
    uint8_t number, xhci_port_reset_info_t *result) {

  if (!result)
    return XHCI_PORT_RESET_UNAVAILABLE;

  *result = (xhci_port_reset_info_t){0};

  if (!controller_running || !controller_verified)
    return XHCI_PORT_RESET_NOT_RUNNING;

  if (!pcie_xhci_ready() || !pcie_xhci_bar_valid())
    return XHCI_PORT_RESET_UNAVAILABLE;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return XHCI_PORT_RESET_UNAVAILABLE;

  // Initially support only the internal USB 2.0 hub on Port 1.
  // We will generalise port reset once enumeration is working.
  if (number != 1U || number > info.max_ports)
    return XHCI_PORT_RESET_UNAVAILABLE;

  uint32_t op = info.capability_length;
  uint32_t usbcmd = xhci_read(op + XHCI_USBCMD);
  uint32_t usbsts = xhci_read(op + XHCI_USBSTS);
  if (!(usbcmd & XHCI_CMD_RUN) ||
      (usbsts & (XHCI_STS_HALTED | XHCI_STS_CNR | XHCI_STS_HOST_ERROR)))
    return XHCI_PORT_RESET_NOT_RUNNING;

  uint32_t offset = op + XHCI_PORTSC_BASE +
      (uint32_t)(number - 1U) * XHCI_PORT_STRIDE;

  result->before = xhci_read(offset);
  result->after = result->before;
  result->controller_status = usbsts;

  if (!(result->before & XHCI_PORT_CCS))
    return XHCI_PORT_RESET_DISCONNECTED;

  if (!(result->before & XHCI_PORT_PP))
    return XHCI_PORT_RESET_NOT_POWERED;

  if (result->before & XHCI_PORT_PED)
    return XHCI_PORT_RESET_ALREADY_ENABLED;

  // Preserve writable configuration fields only. Status-change
  // bits are write-one-to-clear and must not be copied back.
  // LWS remains zero, so the existing link state is not written.
  uint32_t writable = XHCI_PORT_PP | (3U << 14) | (7U << 25);
  uint32_t value = (result->before & writable) | XHCI_PORT_PR;
  uint64_t started = timer_now_us();

  // Set Port Reset. Hardware clears PR when reset completes.
  xhci_write(offset, value);
  __asm__ volatile("dsb sy" ::: "memory");

  while (timer_now_us() - started < XHCI_PORT_RESET_TIMEOUT_US) {
    uint32_t current = xhci_read(offset);
    uint32_t status = xhci_read(op + XHCI_USBSTS);
    result->after = current;
    result->controller_status = status;
    result->elapsed_us = timer_now_us() - started;
    if (status & (XHCI_STS_HALTED | XHCI_STS_HOST_ERROR))
      return XHCI_PORT_RESET_FAILED;
    if (!(current & XHCI_PORT_CCS))
      return XHCI_PORT_RESET_DISCONNECTED;
    if (!(current & XHCI_PORT_PR)) {
      if (current & XHCI_PORT_PED)
        return XHCI_PORT_RESET_OK;
      return XHCI_PORT_RESET_FAILED;
    }
    timer_delay_ms(1);
  }
  result->after = xhci_read(offset);
  result->controller_status = xhci_read(op + XHCI_USBSTS);
  result->elapsed_us = timer_now_us() - started;

  return XHCI_PORT_RESET_TIMEOUT;
}

#define XHCI_RING_TRBS 256U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRB_CHAIN (1U << 4)
#define XHCI_TRB_IOC          (1U << 5)
#define XHCI_TRB_IDT          (1U << 6)
#define XHCI_TRB_DIR_IN       (1U << 16)
#define XHCI_SLOT_HUB_FLAG (1U << 26)
#define XHCI_TRB_SETUP_STAGE   2U
#define XHCI_TRB_DATA_STAGE    3U
#define XHCI_TRB_TRT_IN       (3U << 16)
#define XHCI_TRB_STATUS_STAGE  4U
#define XHCI_TRB_LINK          6U
#define XHCI_TRB_NOOP_TRANSFER 8U
#define XHCI_TRB_ENABLE_SLOT   9U
#define XHCI_TRB_DISABLE_SLOT  10U
#define XHCI_TRB_TYPE_SHIFT    10U
#define XHCI_TRB_ADDRESS_DEVICE 11U
#define XHCI_TRB_CONFIGURE_ENDPOINT 12U
#define XHCI_TRB_EVALUATE_CONTEXT 13U
#define XHCI_TRB_STOP_ENDPOINT 15U
#define XHCI_TRB_NOOP_CMD       23U
#define XHCI_TRB_TRANSFER_EVENT 32U
#define XHCI_TRB_COMMAND_EVENT  33U
#define XHCI_TRB_PORT_STATUS_EVENT 34U

#define XHCI_TRANSFER_TIMEOUT_US 1000000ULL
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_COMMAND_TIMEOUT_US    1000000ULL

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

bool xhci_protocol_get(unsigned int index, xhci_protocol_t *protocol,
    bool *found) {
  if (!protocol || !found)
    return false;

  *protocol = (xhci_protocol_t){0};
  *found = false;

  if (!pcie_xhci_ready() || !pcie_xhci_bar_valid())
    return false;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return false;

  uint32_t hccparams = xhci_read(XHCI_HCCPARAMS1);
  uint32_t offset = ((hccparams >> 16) & 0xffffU) * 4U;
  if (offset == 0)
    return true;

  unsigned int protocol_index = 0;
  for (unsigned int hop = 0; hop < XHCI_EXT_CAP_MAX_HOPS; hop++) {
    if (offset < 0x20U || offset > XHCI_EXT_CAP_LIMIT - 4U)
      return false;

    uint32_t header = xhci_read(offset);
    uint8_t id = (uint8_t)(header & 0xffU);
    uint8_t next = (uint8_t)((header >> 8) & 0xffU);
    if (id == XHCI_EXT_CAP_PROTOCOL) {
      if (offset > XHCI_EXT_CAP_LIMIT - 16U)
        return false;

      if (protocol_index == index) {
        protocol->offset = offset;
        protocol->header = header;
        protocol->name = xhci_read(offset + 4U);
        protocol->ports = xhci_read(offset + 8U);
        protocol->slot = xhci_read(offset + 12U);
        protocol->major = (uint8_t)(header >> 24);
        protocol->minor = (uint8_t)((header >> 16) & 0xffU);
        protocol->first_port = (uint8_t)(protocol->ports & 0xffU);
        protocol->port_count = (uint8_t)((protocol->ports >> 8) & 0xffU);
        protocol->speed_id_count = (uint8_t)((protocol->ports >> 28) & 0x0fU);
        protocol->slot_type = (uint8_t)(protocol->slot & 0x1fU);
        *found = true;
        return true;
      }
      protocol_index++;
    }
    if (next == 0)
      return true;
    // Next pointers are positive DWORD offsets from the
    // current capability, not absolute BAR offsets.
    offset += (uint32_t)next * 4U;
  }

  // Reject an unexpectedly long or malformed chain.
  return false;
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

  // OrgOS currently polls the xHCI event ring.
  // Keep interrupter 0 disabled.
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

static bool xhci_command_link_cycle(unsigned int cycle) {
  xhci_trb_t *ring =
      (xhci_trb_t *)controller_dma.command_ring.virtual;

  xhci_trb_t *link = &ring[XHCI_RING_TRBS - 1U];

  // The Link TRB shares a cache line with the
  // preceding command TRBs. We only modify it
  // after the first completion of a new cycle.
  link->control =
      (link->control & ~XHCI_TRB_CYCLE) |
      (cycle ? XHCI_TRB_CYCLE : 0U);

  cache_info_t cache;
  if (!cache_get_info(&cache) ||
      cache.data_line_size == 0 ||
      (cache.data_line_size &
          (cache.data_line_size - 1U)))
    return false;

  uintptr_t address =
      (uintptr_t)link & ~(uintptr_t)(cache.data_line_size - 1U);

  return cache_clean((const void *)address,
      cache.data_line_size);
}


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

    for (unsigned int i = 0; i < controller_dma.scratchpad_count; i++) {
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

static xhci_event_result_t xhci_event_poll(xhci_event_t *result) {
  if (!result || !controller_running || !controller_verified)
    return XHCI_EVENT_ERROR;

  *result = (xhci_event_t){0};

  if (controller_dma.event_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_EVENT_ERROR;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return XHCI_EVENT_ERROR;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0 ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_EVENT_ERROR;

  xhci_trb_t *ring = (xhci_trb_t *)controller_dma.event_ring.virtual;
  xhci_trb_t *trb = &ring[event_index];

  size_t line = cache.data_line_size;
  uintptr_t address = (uintptr_t)trb & ~(uintptr_t)(line - 1U);
  if (!cache_invalidate((const void *)address, line))
    return XHCI_EVENT_ERROR;

  uint32_t control = trb->control;
  if ((control & XHCI_TRB_CYCLE) != event_cycle)
    return XHCI_EVENT_NONE;

  result->parameter = trb->parameter;
  result->status = trb->status;
  result->control = control;
  result->type = (uint8_t)((control >> XHCI_TRB_TYPE_SHIFT) & 0x3fU);
  result->completion_code = (uint8_t)(result->status >> 24);
  result->slot_id = (uint8_t)(control >> 24);

  // Port Status Change Events report the port in parameter[31:24].
  if (result->type == XHCI_TRB_PORT_STATUS_EVENT)
    result->port_id = (uint8_t)(result->parameter >> 24);

  event_index++;

  if (event_index == XHCI_RING_TRBS) {
    event_index = 0;
    event_cycle ^= 1U;
  }

  uint64_t next = controller_dma.event_ring.bus +
      (uint64_t)event_index * sizeof(xhci_trb_t);
  uint32_t intr = info.runtime_offset + XHCI_INTR0_OFFSET;

  // EHB is write-one-to-clear. The other ERDP reserved
  // bits remain zero because the ring is TRB-aligned.
  xhci_write64(intr + XHCI_ERDP_LO, next | (1ULL << 3));
  __asm__ volatile("dsb sy" ::: "memory");

  return XHCI_EVENT_READY;
}

static bool xhci_event_record(const xhci_event_t *event) {
  if (!event || event->type != XHCI_TRB_PORT_STATUS_EVENT)
    return false;

  event_stats.port_changes++;
  event_stats.last_port = event->port_id;
  event_stats.last_status = event->status;
  event_stats.last_control = event->control;

  return true;
}

bool xhci_event_get_stats(xhci_event_stats_t *stats) {
  if (!stats)
    return false;

  *stats = event_stats;
  stats->port_queue_depth = port_event_count;
  return true;
}

static bool xhci_port_event_enqueue(const xhci_event_t *event) {
  if (!event || port_event_count == XHCI_PORT_EVENT_QUEUE_CAPACITY) {
    event_stats.port_queue_overflows++;
    return false;
  }

  port_event_queue[port_event_count++] = *event;

  if (port_event_count > event_stats.port_queue_peak)
    event_stats.port_queue_peak = port_event_count;

  return true;
}

static bool xhci_port_event_take(uint8_t port, xhci_event_t *result) {
  if (!result || port == 0U)
    return false;

  for (unsigned int i = 0U; i < port_event_count; i++) {
    if (port_event_queue[i].port_id != port)
      continue;
    *result = port_event_queue[i];
    // Remove this event without disturbing the
    // arrival order of events for other ports.
    for (unsigned int j = i + 1U; j < port_event_count; j++)
      port_event_queue[j - 1U] = port_event_queue[j];
    port_event_count--;
    return true;
  }

  return false;
}

bool xhci_port_event_read(xhci_port_change_t *result) {
  if (!result || port_event_count == 0U)
    return false;

  xhci_event_t event = port_event_queue[0];

  for (unsigned int i = 1U; i < port_event_count; i++)
    port_event_queue[i - 1U] = port_event_queue[i];

  port_event_count--;

  *result = (xhci_port_change_t){
    .port = event.port_id,
    .completion_code = event.completion_code,
    .status = event.status,
    .control = event.control
  };

  return true;
}

static bool xhci_waiter_matches(const xhci_event_waiter_t *waiter,
    const xhci_event_t *event, uint8_t endpoint_id) {
  if (!waiter || !event || !waiter->active || waiter->type != event->type)
    return false;

  if (event->type == XHCI_TRB_TRANSFER_EVENT) {
    if (waiter->slot_id != event->slot_id ||
        waiter->endpoint_id != endpoint_id)
      return false;
  }

  // Normal completion: match the registered TRB.
  if (event->parameter == waiter->trb_address)
    return true;

  // Only EP0 control transfers may report errors
  // against an earlier TRB in their registered TD.
  if (event->type != XHCI_TRB_TRANSFER_EVENT || endpoint_id != 1U ||
      event->completion_code == XHCI_COMPLETION_SUCCESS ||
      waiter->td_start == 0U)
    return false;

  if (event->parameter < waiter->td_start ||
      event->parameter >= waiter->trb_address)
    return false;

  uint64_t offset = event->parameter - waiter->td_start;

  // A control TD has either a Setup and Status TRB,
  // or a Setup, Data and Status TRB.
  return offset <= sizeof(xhci_trb_t) && (offset % sizeof(xhci_trb_t)) == 0U;
}

bool xhci_ep0_match_selftest(void) {
  const uint64_t first = 0x1000ULL;
  const uint64_t data = first + sizeof(xhci_trb_t);
  const uint64_t status = data + sizeof(xhci_trb_t);

  xhci_event_waiter_t waiter = {
    .active = true,
    .type = XHCI_TRB_TRANSFER_EVENT,
    .slot_id = 2U,
    .endpoint_id = 1U,
    .trb_address = status,
    .td_start = first
  };

  xhci_event_t event = {
    .type = XHCI_TRB_TRANSFER_EVENT,
    .slot_id = 2U,
    .parameter = status,
    .completion_code = XHCI_COMPLETION_SUCCESS
  };

  // Successful Status Stage.
  if (!xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  // Error against the Setup Stage.
  event.parameter = first;
  event.completion_code = 6U;
  if (!xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  // Error against the Data Stage.
  event.parameter = data;
  if (!xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  // Success against an earlier TRB is not accepted.
  event.completion_code = XHCI_COMPLETION_SUCCESS;
  if (xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  // An address outside the TD is not accepted.
  event.parameter = first - sizeof(xhci_trb_t);
  event.completion_code = 6U;
  if (xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  // Nor is an unaligned address.
  event.parameter = first + 1U;
  if (xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  // Slot and endpoint ownership must still match.
  event.parameter = first;
  event.slot_id = 3U;
  if (xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  event.slot_id = 2U;
  if (xhci_waiter_matches(&waiter, &event, 2U))
    return false;

  // No TD extent means exact matching only.
  waiter.td_start = 0U;
  if (xhci_waiter_matches(&waiter, &event, 1U))
    return false;

  // Command completions retain exact matching.
  waiter.type = XHCI_TRB_COMMAND_EVENT;
  waiter.trb_address = status;
  event.type = XHCI_TRB_COMMAND_EVENT;

  if (xhci_waiter_matches(&waiter, &event, 0U))
    return false;

  event.parameter = status;
  if (!xhci_waiter_matches(&waiter, &event, 0U))
    return false;

  return true;
}

static xhci_event_waiter_t *xhci_waiter_register(
    uint8_t type, uint8_t slot_id, uint8_t endpoint_id,
    uint64_t trb_address) {

  if (trb_address == 0U)
    return NULL;

  // A completion must have exactly one owner.
  for (unsigned int i = 0; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];
    if (waiter->active &&
        waiter->type == type &&
        waiter->slot_id == slot_id &&
        waiter->endpoint_id == endpoint_id &&
        waiter->trb_address == trb_address)
      return NULL;
  }

  for (unsigned int i = 0; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];
    if (waiter->active)
      continue;
    *waiter = (xhci_event_waiter_t){
      .active = true,
      .type = type,
      .slot_id = slot_id,
      .endpoint_id = endpoint_id,
      .trb_address = trb_address
    };
    return waiter;
  }

  return NULL;
}

typedef enum {
  XHCI_DISPATCH_NONE,
  XHCI_DISPATCH_OK,
  XHCI_DISPATCH_READ_ERROR,
  XHCI_DISPATCH_UNHANDLED
} xhci_dispatch_result_t;

// An event-stream fault cannot safely be recovered
// without resetting and rebuilding controller state.
static xhci_dispatch_result_t dispatch_fault = XHCI_DISPATCH_NONE;

static xhci_dispatch_result_t xhci_dispatch_unhandled(
    const xhci_event_t *event) {

  event_stats.unhandled_events++;
  event_stats.last_unhandled_parameter = event->parameter;
  event_stats.last_unhandled_status = event->status;
  event_stats.last_unhandled_control = event->control;

  dispatch_fault = XHCI_DISPATCH_UNHANDLED;
  return dispatch_fault;
}

static xhci_dispatch_result_t xhci_dispatch_one(void) {
  // Never consume further events after a fatal
  // dispatch error. Preserve the original fault.
  if (dispatch_fault != XHCI_DISPATCH_NONE)
    return dispatch_fault;

  xhci_event_t event;
  xhci_event_result_t status = xhci_event_poll(&event);

  if (status == XHCI_EVENT_NONE)
    return XHCI_DISPATCH_NONE;

  if (status == XHCI_EVENT_ERROR) {
    event_stats.read_errors++;
    dispatch_fault = XHCI_DISPATCH_READ_ERROR;
    return dispatch_fault;
  }

  // Port events are recorded independently of
  // whichever transfer is currently waiting.
  if (event.type == XHCI_TRB_PORT_STATUS_EVENT) {
    if (!xhci_event_record(&event)) {
      event_stats.unsupported_events++;
      return xhci_dispatch_unhandled(&event);
    }

    // Preserve port notifications independently
    // of whichever operation is being serviced.
    if (!xhci_port_event_enqueue(&event))
      return xhci_dispatch_unhandled(&event);

    return XHCI_DISPATCH_OK;
  }

  // Only registered command or transfer completions
  // can currently be delivered to waiters.
  if (event.type != XHCI_TRB_COMMAND_EVENT &&
      event.type != XHCI_TRB_TRANSFER_EVENT) {
    event_stats.unsupported_events++;
    return xhci_dispatch_unhandled(&event);
  }

  uint8_t endpoint_id = 0U;

  if (event.type == XHCI_TRB_TRANSFER_EVENT)
    endpoint_id = (uint8_t)((event.control >> 16) & 0x1fU);

  for (unsigned int i = 0; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];

    if (!xhci_waiter_matches(waiter, &event, endpoint_id))
      continue;

    // A duplicate completion is a protocol error.
    // A duplicate completion is a protocol error.
    if (waiter->completed) {
      event_stats.duplicate_events++;
      return xhci_dispatch_unhandled(&event);
    }
    waiter->event = event;
    waiter->completed = true;
    // A stopped Bulk IN event may belong to
    // the outstanding receive TD itself.
    if (stop_in_event.armed && !stop_in_event.received &&
        event.type == XHCI_TRB_TRANSFER_EVENT &&
        event.slot_id == stop_in_event.slot_id &&
        endpoint_id == stop_in_event.endpoint_id &&
        event.completion_code >= 26U && event.completion_code <= 28U) {
      stop_in_event.received = true;
      stop_in_event.completion_code = event.completion_code;
      stop_in_event.trb_address = event.parameter;
    }
    if (event.type == XHCI_TRB_COMMAND_EVENT)
      event_stats.command_events++;
    else
      event_stats.transfer_events++;
    return XHCI_DISPATCH_OK;
  }

  // Stop Endpoint may report a dequeue position
  // rather than the outstanding Bulk IN TD.
  if (event.type == XHCI_TRB_TRANSFER_EVENT && stop_in_event.armed &&
      !stop_in_event.received && event.slot_id == stop_in_event.slot_id &&
      endpoint_id == stop_in_event.endpoint_id &&
      event.completion_code >= 26U && event.completion_code <= 28U) {

    xhci_device_t *device = xhci_device_by_slot(event.slot_id);
    if (!device || device->bulk_in_ring.pages == 0U)
      return xhci_dispatch_unhandled(&event);

    uint64_t base = device->bulk_in_ring.bus;
    if (event.parameter < base || event.parameter - base >= PAGE_SIZE ||
        ((event.parameter - base) % sizeof(xhci_trb_t)) != 0U)
      return xhci_dispatch_unhandled(&event);

    stop_in_event.received = true;
    stop_in_event.completion_code = event.completion_code;
    stop_in_event.trb_address = event.parameter;

    event_stats.transfer_events++;
    return XHCI_DISPATCH_OK;
  }

  // Stop Endpoint can force a Transfer Event for
  // the endpoint's current dequeue position.
  // It need not correspond to an outstanding TD.
  if (event.type == XHCI_TRB_TRANSFER_EVENT && stop_out_event.armed &&
      !stop_out_event.received && event.slot_id == stop_out_event.slot_id &&
      endpoint_id == stop_out_event.endpoint_id &&
      event.completion_code >= 26U && event.completion_code <= 28U) {

    xhci_device_t *device = xhci_device_by_slot(event.slot_id);
    if (!device || device->bulk_out_ring.pages == 0U)
      return xhci_dispatch_unhandled(&event);

    uint64_t base = device->bulk_out_ring.bus;
    uint64_t offset = event.parameter - base;
    // Accept only a TRB address belonging to
    // this endpoint's existing transfer ring.
    if (event.parameter < base || offset >= PAGE_SIZE ||
        (offset % sizeof(xhci_trb_t)) != 0U)
      return xhci_dispatch_unhandled(&event);

    stop_out_event.received = true;
    stop_out_event.completion_code = event.completion_code;
    stop_out_event.trb_address = event.parameter;

    event_stats.transfer_events++;
    return XHCI_DISPATCH_OK;
  }

  // The hardware event has no registered owner.
  // Retain its contents and prevent further dispatch.
  event_stats.unmatched_events++;
  return xhci_dispatch_unhandled(&event);
}

static xhci_dispatch_result_t xhci_dispatch_wait_step(void) {
  xhci_dispatch_result_t status = xhci_dispatch_one();
  // A synchronous controller operation may also
  // dispatch a completion for an active Bulk IN
  // receiver on any managed device.
  // Service completed receivers without recursively
  // entering the hardware event dispatcher.
  if (status == XHCI_DISPATCH_OK) {
    for (unsigned int i = 0U; i < XHCI_MAX_DEVICES; i++) {
      xhci_device_t *device = &devices[i];
      xhci_bulk_in_receiver_t *receiver = &device->receiver;
      if (device->slot_id == 0U ||
          !receiver->started || receiver->failed ||
          !receiver->waiter || !receiver->waiter->completed ||
          receiver->waiter->slot_id != device->slot_id)
        continue;

      xhci_bulk_in_stream_service_device(device);
      event_stats.sync_in_services++;
    }
  }

  return status;
}

xhci_service_result_t xhci_service(unsigned int budget) {
  if (!controller_running || !controller_verified || budget == 0U)
    return XHCI_SERVICE_NOT_READY;

  event_stats.service_calls++;
  bool progressed = false;
  for (unsigned int i = 0U; i < budget; i++) {
    xhci_dispatch_result_t status = xhci_dispatch_one();

    if (status == XHCI_DISPATCH_READ_ERROR ||
        status == XHCI_DISPATCH_UNHANDLED) {
      event_stats.service_failures++;
      return XHCI_SERVICE_FAILED;
    }

    if (status == XHCI_DISPATCH_NONE)
      return progressed ?  XHCI_SERVICE_PROGRESS : XHCI_SERVICE_IDLE;

    event_stats.service_events++;
    progressed = true;
  }

  return XHCI_SERVICE_PROGRESS;
}

xhci_port_event_result_t xhci_port_event_wait(
    uint8_t port, uint32_t timeout_ms) {

  if (!controller_running || !controller_verified || port == 0U)
    return XHCI_PORT_EVENT_UNAVAILABLE;

  xhci_info_t info;
  if (!xhci_probe(&info) || port > info.max_ports)
    return XHCI_PORT_EVENT_UNAVAILABLE;

  uint64_t started = timer_now_us();
  uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;
  for (;;) {
    // A fatal dispatcher error takes precedence
    // even if port notifications remain queued.
    if (dispatch_fault != XHCI_DISPATCH_NONE)
      return XHCI_PORT_EVENT_FAILED;

    xhci_event_t event;
    // Inspect queued events before reading any
    // further events from the hardware ring.
    if (xhci_port_event_take(port, &event)) {
      return event.completion_code == XHCI_COMPLETION_SUCCESS ?
          XHCI_PORT_EVENT_OK : XHCI_PORT_EVENT_FAILED;
    }

    if (timer_now_us() - started >= timeout_us)
      return XHCI_PORT_EVENT_TIMEOUT;

    xhci_dispatch_result_t status = xhci_dispatch_one();
    if (status == XHCI_DISPATCH_READ_ERROR ||
        status == XHCI_DISPATCH_UNHANDLED)
      return XHCI_PORT_EVENT_FAILED;

    if (status == XHCI_DISPATCH_NONE)
      timer_delay_ms(1U);
  }
}

static void xhci_waiter_release(xhci_event_waiter_t *waiter) {
  if (waiter)
    *waiter = (xhci_event_waiter_t){0};
}

xhci_command_result_t xhci_command_submit(const xhci_trb_t *command,
    xhci_command_completion_t *result) {

  if (!command || !result)
    return XHCI_COMMAND_UNAVAILABLE;

  *result = (xhci_command_completion_t){0};

  if (!controller_running || !controller_verified)
    return XHCI_COMMAND_NOT_RUNNING;

  if (command_outstanding || command_ring_failed)
    return XHCI_COMMAND_BUSY;

  if (!pcie_xhci_ready() || !pcie_xhci_bar_valid())
    return XHCI_COMMAND_UNAVAILABLE;

  xhci_info_t info;
  if (!xhci_probe(&info))
    return XHCI_COMMAND_UNAVAILABLE;

  uint32_t op = info.capability_length;
  uint32_t status = xhci_read(op + XHCI_USBSTS);
  uint32_t usbcmd = xhci_read(op + XHCI_USBCMD);
  uint16_t pci_command;
  if (!pcie_xhci_get_command(&pci_command) ||
      !(pci_command & PCI_CMD_BUS_MASTER) ||
      !(usbcmd & XHCI_CMD_RUN) ||
      (status & (XHCI_STS_HALTED | XHCI_STS_CNR | XHCI_STS_HOST_ERROR)))
    return XHCI_COMMAND_NOT_RUNNING;

  if (controller_dma.command_ring.owner != DMA_OWNER_DEVICE ||
      controller_dma.event_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_COMMAND_UNAVAILABLE;

  // Validate the controller's cache-line geometry.
  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0 ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_COMMAND_CACHE_ERROR;

  size_t line = cache.data_line_size;
  xhci_trb_t *commands = (xhci_trb_t *)controller_dma.command_ring.virtual;
  xhci_trb_t *trb = &commands[command_index];
  uint64_t command_address = controller_dma.command_ring.bus +
      (uint64_t)command_index * sizeof(xhci_trb_t);
  uintptr_t command_line = (uintptr_t)trb & ~(uintptr_t)(line - 1U);
  result->command_address = command_address;

  // Copy the caller's command into the ring.
  // The transport, not the caller, controls the cycle bit.
  trb->parameter = command->parameter;
  trb->status = command->status;
  trb->control = (command->control & ~XHCI_TRB_CYCLE) |
      (command_cycle ? XHCI_TRB_CYCLE : 0U);

  // Make the complete TRB visible before ringing the doorbell.
  if (!cache_clean((const void *)command_line, line))
    return XHCI_COMMAND_CACHE_ERROR;

  // Register before ringing the command doorbell.
  xhci_event_waiter_t *waiter = xhci_waiter_register(
      XHCI_TRB_COMMAND_EVENT, 0U, 0U, command_address);

  if (!waiter)
    return XHCI_COMMAND_BUSY;

  command_outstanding = true;
  uint64_t started = timer_now_us();
  // Doorbell 0 selects the host-controller command ring.
  xhci_write(info.doorbell_offset, 0U);

  __asm__ volatile("dsb sy" ::: "memory");

  while (timer_now_us() - started < XHCI_COMMAND_TIMEOUT_US) {
    xhci_dispatch_result_t status = xhci_dispatch_wait_step();

    if (status == XHCI_DISPATCH_READ_ERROR) {
      xhci_waiter_release(waiter);
      command_ring_failed = true;
      return XHCI_COMMAND_CACHE_ERROR;
    }

    if (status == XHCI_DISPATCH_UNHANDLED) {
      xhci_waiter_release(waiter);
      command_ring_failed = true;
      return XHCI_COMMAND_BAD_EVENT;
    }

    if (waiter->completed) {
      xhci_event_t event = waiter->event;
      xhci_waiter_release(waiter);

      result->event_parameter = event.parameter;
      result->event_status = event.status;
      result->event_control = event.control;
      result->event_type = event.type;
      result->completion_code = event.completion_code;
      result->slot_id = event.slot_id;
      result->elapsed_us = timer_now_us() - started;
      result->controller_status = xhci_read(op + XHCI_USBSTS);

      if (event.completion_code != XHCI_COMPLETION_SUCCESS) {
        command_ring_failed = true;
        return XHCI_COMMAND_FAILED;
      }

      command_outstanding = false;

      if (command_index == 0 && completed_commands != 0) {
        if (!xhci_command_link_cycle(command_cycle)) {
          command_ring_failed = true;
          return XHCI_COMMAND_CACHE_ERROR;
        }
      }

      command_index++;
      if (command_index == XHCI_RING_TRBS - 1U) {
        command_index = 0;
        command_cycle ^= 1U;
      }
      completed_commands++;
      result->command_number = completed_commands;
      return XHCI_COMMAND_OK;
    }

    uint32_t current_status = xhci_read(op + XHCI_USBSTS);
    if (current_status & (XHCI_STS_HALTED | XHCI_STS_HOST_ERROR)) {
      result->controller_status = current_status;
      result->elapsed_us = timer_now_us() - started;
      xhci_waiter_release(waiter);
      command_ring_failed = true;
      return XHCI_COMMAND_FAILED;
    }
  }

  result->elapsed_us = timer_now_us() - started;
  result->controller_status = xhci_read(op + XHCI_USBSTS);
  xhci_waiter_release(waiter);
  command_ring_failed = true;
  return XHCI_COMMAND_TIMEOUT;
}

xhci_slot_result_t xhci_enable_slot(xhci_slot_info_t *result) {
  if (!result)
    return XHCI_SLOT_NOT_READY;

  *result = (xhci_slot_info_t){0};
  result->root_port = 1U;
  result->command_result = XHCI_COMMAND_UNAVAILABLE;

  if (slot_requested)
    return XHCI_SLOT_ALREADY_REQUESTED;

  if (!controller_running || !controller_verified ||
      command_outstanding || command_ring_failed)
    return XHCI_SLOT_NOT_READY;

  xhci_port_t port;
  if (!xhci_port_read(1U, &port) || !port.connected || !port.enabled ||
      !port.powered || port.resetting || port.speed != 3U)
    return XHCI_SLOT_NOT_READY;

  xhci_info_t controller;
  if (!xhci_probe(&controller) || controller.max_slots == 0)
    return XHCI_SLOT_NOT_READY;

  // The USB 2.0 Supported Protocol capability reports Slot Type 0.
  // All other Enable Slot command fields are zero.
  xhci_trb_t command = {
    .parameter = 0,
    .status = 0,
    .control = XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT
  };

  // Conservatively reserve this attempt. If completion is lost,
  // another Enable Slot could allocate an additional device slot.
  slot_requested = true;

  xhci_command_completion_t completion;
  xhci_command_result_t status = xhci_command_submit(&command, &completion);
  result->command_result = status;
  result->completion_code = completion.completion_code;
  result->controller_status = completion.controller_status;
  result->slot_id = completion.slot_id;

  if (status != XHCI_COMMAND_OK)
    return XHCI_SLOT_COMMAND_FAILED;

  if (completion.slot_id == 0 || completion.slot_id > controller.max_slots)
    return XHCI_SLOT_INVALID_ID;

  internal_hub_slot = completion.slot_id;
  return XHCI_SLOT_OK;
}

xhci_slot_result_t xhci_enable_downstream_slot(
    xhci_slot_info_t *result) {

  if (!result)
    return XHCI_SLOT_NOT_READY;

  *result = (xhci_slot_info_t){0};
  result->root_port = 1U;
  result->command_result = XHCI_COMMAND_UNAVAILABLE;

  // Do not repeat Enable Slot for the same device incarnation.
  // An uncertain completion may have succeeded, so the guard
  // is cleared only after confirmed teardown and successful recycling.
  if (devices[1].lifecycle.slot_requested)
    return XHCI_SLOT_ALREADY_REQUESTED;

  // The upstream hub must already be operational.
  if (!internal_hub.prepared || !internal_hub.addressed ||
      internal_hub.slot_id == 0U ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed)
    return XHCI_SLOT_NOT_READY;

  xhci_info_t controller;
  if (!xhci_probe(&controller) || controller.max_slots < 2U)
    return XHCI_SLOT_NOT_READY;

  // Enable Slot does not yet describe the device's topology.
  // It merely reserves a hardware device slot.
  xhci_trb_t command = {
    .parameter = 0U,
    .status = 0U,
    .control = XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT
  };

  devices[1].lifecycle.slot_requested = true;
  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);
  result->command_result = status;
  result->completion_code = completion.completion_code;
  result->controller_status = completion.controller_status;
  result->slot_id = completion.slot_id;

  if (status != XHCI_COMMAND_OK)
    return XHCI_SLOT_COMMAND_FAILED;

  if (completion.slot_id == 0U ||
      completion.slot_id > controller.max_slots ||
      completion.slot_id == internal_hub.slot_id)
    return XHCI_SLOT_INVALID_ID;

  devices[1].slot_id = completion.slot_id;
  devices[1].root_port = 1U;
  devices[1].hub_slot = internal_hub.slot_id;
  return XHCI_SLOT_OK;
}

#define XHCI_CONTEXT_SIZE_FLAG (1U << 2)
#define XHCI_EP0_TYPE_CONTROL 4U
#define XHCI_EP0_MAX_PACKET 64U

static bool xhci_device_alloc_buffers(xhci_device_t *device,
    uint32_t context_size) {

  if (!device || (context_size != 32U && context_size != 64U))
    return false;

  uint32_t input_size = 34U * context_size;
  uint32_t output_size = 33U * context_size;

  if (!dma_alloc(input_size, 64U, &device->input))
    goto failed;

  if (!dma_alloc(output_size, 64U, &device->output))
    goto failed;

  if (!dma_alloc(PAGE_SIZE, 64U, &device->ep0_ring))
    goto failed;

  // Prepare the transfer-ring Link TRB.
  // The controller will follow this when it reaches
  // the end of the ring.
  xhci_trb_t *ring = device->ep0_ring.virtual;
  xhci_trb_t *link = &ring[XHCI_RING_TRBS - 1U];
  link->parameter = device->ep0_ring.bus;
  link->status = 0U;
  link->control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
      XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
  device->context_size = (uint8_t)context_size;
  device->ep0_index = 0U;
  device->ep0_cycle = 1U;
  device->ep0_wrap_pending = false;
  device->ep0_failed = false;
  device->ep0_active = false;
  return true;

failed:
  // These allocations have not been handed to the controller.
  if (device->ep0_ring.pages)
    dma_free(&device->ep0_ring);

  if (device->output.pages)
    dma_free(&device->output);

  if (device->input.pages)
    dma_free(&device->input);

  return false;
}

bool xhci_device_get_info(xhci_device_info_t *result) {
  if (!result || !internal_hub.prepared)
    return false;

  *result = internal_hub.info;
  return true;
}

xhci_device_result_t xhci_device_prepare(xhci_device_info_t *result) {
  if (!result)
    return XHCI_DEVICE_NOT_READY;

  *result = (xhci_device_info_t){0};

  if (internal_hub.prepared)
    return XHCI_DEVICE_ALREADY_PREPARED;

  if (!controller_running || !controller_verified ||
      internal_hub_slot == 0)
    return XHCI_DEVICE_NOT_READY;

  xhci_info_t controller;
  xhci_port_t port;
  if (!xhci_probe(&controller) ||
      !xhci_port_read(1U, &port) ||
      !port.connected || !port.enabled || !port.powered ||
      port.resetting || port.speed != 3U ||
      internal_hub_slot > controller.max_slots)
    return XHCI_DEVICE_NOT_READY;

  uint32_t hccparams = xhci_read(XHCI_HCCPARAMS1);
  uint32_t context_size =
      (hccparams & XHCI_CONTEXT_SIZE_FLAG) ? 64U : 32U;
  if (!xhci_device_alloc_buffers(&internal_hub, context_size))
    return XHCI_DEVICE_ALLOCATION_FAILED;

  internal_hub.slot_id = internal_hub_slot;
  internal_hub.root_port = 1U;
  internal_hub.hub_slot = 0U;
  internal_hub.hub_port = 0U;
  internal_hub.speed = port.speed;
  internal_hub.ep0_max_packet = XHCI_EP0_MAX_PACKET;
  internal_hub.route_string = 0U;

  // Context offsets are measured in context entries,
  // not necessarily in 32-byte units.
  uint32_t *control = internal_hub.input.virtual;
  uint32_t *slot = (uint32_t *)((uint8_t *)control + context_size);
  uint32_t *ep0 = (uint32_t *)((uint8_t *)slot + context_size);
  // Input Control Context:
  // Add Context flags: A0 (Slot) and A1 (EP0).
  control[1] = 0x00000003U;
  // Slot Context DW0:
  // Context Entries = 1, Speed = High Speed (3).
  slot[0] = (1U << 27) | (3U << 20);
  // Slot Context DW1:
  // Root Hub Port Number = 1.
  slot[1] = 1U << 16;
  // Endpoint 0 Context DW1:
  // Maximum Packet Size = 64.
  // Endpoint Type = 4 (Control).
  // Error Count = 3.
  ep0[1] = (XHCI_EP0_MAX_PACKET << 16) |
      (XHCI_EP0_TYPE_CONTROL << 3) | (3U << 1);
  // Endpoint 0 Context DW2/DW3:
  // Dequeue pointer and initial DCS = 1.
  uint64_t dequeue = internal_hub.ep0_ring.bus | 1ULL;
  ep0[2] = (uint32_t)dequeue;
  ep0[3] = (uint32_t)(dequeue >> 32);
  // Endpoint 0 Context DW4: Average TRB Length.
  // Control endpoints use 8 bytes (xHCI section 6.2.3).
  ep0[4] = 8U;
  // Return inspection values before transferring DMA ownership.
  result->slot_id = internal_hub_slot;
  result->root_port = 1U;
  result->speed = port.speed;
  result->context_size = (uint8_t)context_size;
  result->max_packet_size = XHCI_EP0_MAX_PACKET;
  result->input_context = internal_hub.input.bus;
  result->output_context = internal_hub.output.bus;
  result->ep0_ring = internal_hub.ep0_ring.bus;
  result->input_add_flags = control[1];
  result->slot_dword0 = slot[0];
  result->slot_dword1 = slot[1];
  result->ep0_dword1 = ep0[1];
  result->ep0_dword2 = ep0[2];
  result->ep0_dword3 = ep0[3];

  // The input context and ring will be read by the controller.
  // The output context will be written by the controller.
  if (!dma_begin_device(&internal_hub.input, DMA_TO_DEVICE))
    goto dma_failed;

  if (!dma_begin_device(&internal_hub.output, DMA_BIDIRECTIONAL))
    goto dma_failed;

  if (!dma_begin_device(&internal_hub.ep0_ring, DMA_BIDIRECTIONAL))
    goto dma_failed;

  // DCBAA is already device-owned. Update only the relevant
  // entry, then clean its cache line for PCIe visibility.
  uint64_t *dcbaa = controller_dma.dcbaa.virtual;
  dcbaa[internal_hub_slot] = internal_hub.output.bus;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0 ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    goto published_failed;

  uintptr_t address = (uintptr_t)&dcbaa[internal_hub_slot];
  address &= ~(uintptr_t)(cache.data_line_size - 1U);
  if (!cache_clean((const void *)address, cache.data_line_size))
    goto published_failed;

  __asm__ volatile("dsb sy" ::: "memory");

  result->dcbaa_entry = dcbaa[internal_hub_slot];
  internal_hub.info = *result;
  internal_hub.prepared = true;
  return XHCI_DEVICE_OK;

published_failed:
  // The DCBAA entry may already be visible to the controller.
  // Preserve all DMA allocations for safe diagnosis.
  return XHCI_DEVICE_DMA_FAILED;

dma_failed:
  // Keep handed-off allocations alive. This is conservative;
  // cleanup can be generalised with the device lifecycle.
  return XHCI_DEVICE_DMA_FAILED;
}

xhci_device_result_t xhci_downstream_prepare(
    uint8_t hub_port, uint8_t speed, xhci_device_info_t *result) {

  if (!result)
    return XHCI_DEVICE_NOT_READY;

  *result = (xhci_device_info_t){0};
  xhci_device_t *device = &devices[1];
  if (device->prepared)
    return XHCI_DEVICE_ALREADY_PREPARED;

  // For now, support the single downstream Full-Speed
  // device attached directly to our four-port hub.
  if (hub_port == 0U || hub_port > 4U || speed != 1U ||
      device->slot_id == 0U ||
      !internal_hub.prepared || !internal_hub.addressed ||
      !controller_running || !controller_verified)
    return XHCI_DEVICE_NOT_READY;

  xhci_info_t controller;
  if (!xhci_probe(&controller) ||
      device->slot_id > controller.max_slots ||
      device->slot_id == internal_hub.slot_id)
    return XHCI_DEVICE_NOT_READY;

  uint32_t hccparams = xhci_read(XHCI_HCCPARAMS1);
  uint32_t context_size = (hccparams & XHCI_CONTEXT_SIZE_FLAG) ? 64U : 32U;

  if (!xhci_device_alloc_buffers(device, context_size))
    return XHCI_DEVICE_ALLOCATION_FAILED;

  device->root_port = internal_hub.root_port;
  device->hub_slot = internal_hub.slot_id;
  device->hub_port = hub_port;
  device->speed = speed;
  device->ep0_max_packet = 8U;
  device->route_string = hub_port;

  // Input Context layout:
  //   Context 0: Input Control Context
  //   Context 1: Slot Context
  //   Context 2: EP0 Context
  uint32_t *control = device->input.virtual;
  uint32_t *slot = (uint32_t *)((uint8_t *)control + context_size);
  uint32_t *ep0 = (uint32_t *)((uint8_t *)slot + context_size);

  // Add Slot Context and EP0 Context.
  control[1] = 0x00000003U;

  // Slot Context DW0:
  //   Context Entries = 1
  //   Speed = 1 (Full Speed)
  //   Route String = downstream port number
  slot[0] = (1U << 27) | ((uint32_t)speed << 20) | device->route_string;

  // Slot Context DW1:
  //   Root Hub Port = same root port as parent hub
  slot[1] = (uint32_t)device->root_port << 16;

  // Slot Context DW2:
  //   TT Hub Slot ID (bits 7:0)
  //   TT Port Number (bits 15:8)
  slot[2] = (uint32_t)device->hub_slot | ((uint32_t)device->hub_port << 8);

  // EP0 Context DW1:
  //   Initial Full-Speed MPS = 8
  //   Endpoint Type = Control (4)
  //   Error Count = 3
  ep0[1] = ((uint32_t)device->ep0_max_packet << 16) |
      (XHCI_EP0_TYPE_CONTROL << 3) | (3U << 1);

  // EP0 transfer-ring dequeue pointer, DCS = 1.
  uint64_t dequeue = device->ep0_ring.bus | 1ULL;
  ep0[2] = (uint32_t)dequeue;
  ep0[3] = (uint32_t)(dequeue >> 32);
  ep0[4] = 8U;

  result->slot_id = device->slot_id;
  result->root_port = device->root_port;
  result->speed = device->speed;
  result->context_size = (uint8_t)context_size;
  result->max_packet_size = device->ep0_max_packet;
  result->input_context = device->input.bus;
  result->output_context = device->output.bus;
  result->ep0_ring = device->ep0_ring.bus;
  result->input_add_flags = control[1];
  result->slot_dword0 = slot[0];
  result->slot_dword1 = slot[1];
  result->slot_dword2 = slot[2];
  result->ep0_dword1 = ep0[1];
  result->ep0_dword2 = ep0[2];
  result->ep0_dword3 = ep0[3];

  // Make contexts and ring available for DMA.
  if (!dma_begin_device(&device->input, DMA_TO_DEVICE))
    return XHCI_DEVICE_DMA_FAILED;

  if (!dma_begin_device(&device->output, DMA_BIDIRECTIONAL))
    return XHCI_DEVICE_DMA_FAILED;

  if (!dma_begin_device(&device->ep0_ring, DMA_BIDIRECTIONAL))
    return XHCI_DEVICE_DMA_FAILED;

  // Publish this device's Output Context in its own
  // DCBAA entry, leaving the upstream hub untouched.
  uint64_t *dcbaa = controller_dma.dcbaa.virtual;
  dcbaa[device->slot_id] = device->output.bus;
  cache_info_t cache;
  if (!cache_get_info(&cache) ||
      cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_DEVICE_DMA_FAILED;

  uintptr_t address = (uintptr_t)&dcbaa[device->slot_id];
  address &= ~(uintptr_t)(cache.data_line_size - 1U);
  if (!cache_clean((const void *)address, cache.data_line_size))
    return XHCI_DEVICE_DMA_FAILED;

  __asm__ volatile("dsb sy" ::: "memory");

  result->dcbaa_entry = dcbaa[device->slot_id];
  device->info = *result;
  device->prepared = true;
  return XHCI_DEVICE_OK;
}

static bool xhci_bulk_ring_init(dma_buffer_t *buffer) {
  if (!dma_alloc(PAGE_SIZE, 64U, buffer))
    return false;

  xhci_trb_t *ring = buffer->virtual;
  xhci_trb_t *link = &ring[XHCI_RING_TRBS - 1U];
  link->parameter = buffer->bus;
  link->status = 0U;
  link->control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
      XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;

  return true;
}

xhci_bulk_config_result_t xhci_downstream_bulk_configure(
    uint8_t in_address, uint16_t in_mps,
    uint8_t out_address, uint16_t out_mps,
    xhci_bulk_config_info_t *result) {

  if (!result)
    return XHCI_BULK_CONFIG_NOT_READY;

  *result = (xhci_bulk_config_info_t){0};

  if (devices[1].lifecycle.bulk_attempted)
    return XHCI_BULK_CONFIG_ALREADY_ATTEMPTED;

  xhci_device_t *device = &devices[1];
  if (!device->prepared || !device->addressed ||
      device->slot_id == 0U || !controller_running ||
      !controller_verified || command_outstanding ||
      command_ring_failed || device->ep0_active ||
      device->ep0_failed)
    return XHCI_BULK_CONFIG_NOT_READY;

  if (in_address != 0x81U || out_address != 0x02U ||
      in_mps != 64U || out_mps != 64U)
    return XHCI_BULK_CONFIG_INVALID_ENDPOINT;

  if (device->context_size != 32U && device->context_size != 64U)
    return XHCI_BULK_CONFIG_NOT_READY;

  if (device->input.owner != DMA_OWNER_DEVICE ||
      device->output.owner != DMA_OWNER_DEVICE)
    return XHCI_BULK_CONFIG_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) ||
      cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_BULK_CONFIG_DMA_FAILED;

  devices[1].lifecycle.bulk_attempted = true;

  if (!xhci_bulk_ring_init(&device->bulk_in_ring))
    return XHCI_BULK_CONFIG_ALLOCATION_FAILED;

  if (!xhci_bulk_ring_init(&device->bulk_out_ring))
    return XHCI_BULK_CONFIG_ALLOCATION_FAILED;

  uint32_t size = device->context_size;
  uint8_t *input = device->input.virtual;
  uint8_t *output = device->output.virtual;
  uint32_t *control = (uint32_t *)input;
  uint32_t *slot = (uint32_t *)(input + size);
  uint32_t *in = (uint32_t *)(input + (XHCI_BULK_IN_DCI + 1U) * size);
  uint32_t *out = (uint32_t *)(input + (XHCI_BULK_OUT_DCI + 1U) * size);

  // Add Slot, EP1 IN and EP2 OUT contexts.
  // Do not change the existing EP0 context.
  control[0] = 0U;
  control[1] = (1U << 0) | (1U << XHCI_BULK_IN_DCI) |
      (1U << XHCI_BULK_OUT_DCI);

  // Context Entries must include the highest DCI.
  slot[0] = (slot[0] & ~(0x1fU << 27)) | (XHCI_BULK_OUT_DCI << 27);

  uint64_t in_dequeue = device->bulk_in_ring.bus | 1ULL;
  uint64_t out_dequeue = device->bulk_out_ring.bus | 1ULL;

  // EP1 IN: type 6, error count 3, MPS 64.
  in[0] = 0U;
  in[1] = ((uint32_t)in_mps << 16) | (XHCI_EP_TYPE_IN << 3) | (3U << 1);
  in[2] = (uint32_t)in_dequeue;
  in[3] = (uint32_t)(in_dequeue >> 32);
  in[4] = in_mps;

  // EP2 OUT: type 2, error count 3, MPS 64.
  out[0] = 0U;
  out[1] = ((uint32_t)out_mps << 16) | (XHCI_EP_TYPE_OUT << 3) | (3U << 1);
  out[2] = (uint32_t)out_dequeue;
  out[3] = (uint32_t)(out_dequeue >> 32);
  out[4] = out_mps;

  result->slot_id = device->slot_id;
  result->in_ring = device->bulk_in_ring.bus;
  result->out_ring = device->bulk_out_ring.bus;
  result->in_context = in[1];
  result->out_context = out[1];

  // Publish the rings before the controller can use them.
  if (!dma_begin_device(&device->bulk_in_ring, DMA_BIDIRECTIONAL))
    return XHCI_BULK_CONFIG_DMA_FAILED;

  if (!dma_begin_device(&device->bulk_out_ring, DMA_BIDIRECTIONAL))
    return XHCI_BULK_CONFIG_DMA_FAILED;

  // The Input Context is already device-owned.
  // Clean all entries up to the highest new DCI.
  if (!cache_clean(input,
      (XHCI_BULK_OUT_DCI + 2U) * size)) return XHCI_BULK_CONFIG_DMA_FAILED;

  __asm__ volatile("dsb sy" ::: "memory");

  xhci_trb_t command = {
    .parameter = device->input.bus,
    .status = 0U,
    .control = (XHCI_TRB_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)device->slot_id << 24)
  };

  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);
  result->command_result = status;
  result->completion_code = completion.completion_code;

  if (status != XHCI_COMMAND_OK ||
      completion.slot_id != device->slot_id)
    return XHCI_BULK_CONFIG_COMMAND_FAILED;

  // Inspect the controller-owned Output Context.
  if (!cache_invalidate(output, (XHCI_BULK_OUT_DCI + 1U) * size))
    return XHCI_BULK_CONFIG_DMA_FAILED;

  uint32_t *output_slot = (uint32_t *)output;
  uint32_t *output_in = (uint32_t *)(output + XHCI_BULK_IN_DCI * size);
  uint32_t *output_out = (uint32_t *)(output + XHCI_BULK_OUT_DCI * size);
  result->in_state = (uint8_t)(output_in[0] & 0x07U);
  result->out_state = (uint8_t)(output_out[0] & 0x07U);
  if (((output_slot[0] >> 27) & 0x1fU) != XHCI_BULK_OUT_DCI ||
      result->in_state != 1U || result->out_state != 1U ||
      ((output_in[1] >> 16) & 0xffffU) != in_mps ||
      ((output_out[1] >> 16) & 0xffffU) != out_mps ||
      ((output_in[1] >> 3) & 0x07U) != XHCI_EP_TYPE_IN ||
      ((output_out[1] >> 3) & 0x07U) != XHCI_EP_TYPE_OUT)
    return XHCI_BULK_CONFIG_VERIFY_FAILED;

  device->bulk_out_index = 0U;
  device->bulk_out_cycle = 1U;
  device->bulk_out_active = false;
  device->bulk_out_failed = false;
  device->bulk_out_ready = true;
  device->bulk_in_index = 0U;
  device->bulk_in_cycle = 1U;
  device->bulk_in_wrap_pending = false;
  device->bulk_in_ready = true;
  return XHCI_BULK_CONFIG_OK;
}

static bool xhci_bulk_out_publish(xhci_device_t *device,
    unsigned int index, size_t line) {

  xhci_trb_t *ring = device->bulk_out_ring.virtual;
  uintptr_t address = (uintptr_t)&ring[index];
  address &= ~(uintptr_t)(line - 1U);

  return cache_clean((const void *)address, line);
}

xhci_bulk_out_result_t xhci_downstream_bulk_out(
    const uint8_t *data, uint32_t length,
    xhci_bulk_out_info_t *result) {

  if (!result)
    return XHCI_BULK_OUT_NOT_READY;

  *result = (xhci_bulk_out_info_t){0};
  xhci_device_t *device = &devices[1];
  if (!data || device->lifecycle.quiesced || !device->bulk_out_ready ||
      device->bulk_out_active || device->bulk_out_failed ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed ||
      device->bulk_out_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_BULK_OUT_NOT_READY;

  if (length == 0U || length > 64U)
    return XHCI_BULK_OUT_INVALID_LENGTH;

  xhci_info_t controller;
  xhci_port_t port;

  if (!xhci_probe(&controller) ||
      !xhci_port_read(device->root_port, &port) ||
      !port.connected || !port.enabled || !port.powered ||
      port.resetting || port.speed != 3U)
    return XHCI_BULK_OUT_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) ||
      cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_BULK_OUT_NOT_READY;

  dma_buffer_t *buffer = &device->bulk_out_buffer;

  if (!buffer->pages && !dma_alloc(64U, 64U, buffer))
    return XHCI_BULK_OUT_ALLOCATION_FAILED;

  uint8_t *bytes = buffer->virtual;
  for (uint32_t i = 0U; i < length; i++)
    bytes[i] = data[i];

  if (!dma_begin_device(buffer, DMA_TO_DEVICE))
    return XHCI_BULK_OUT_DMA_FAILED;

  unsigned int index = device->bulk_out_index;
  uint32_t cycle = device->bulk_out_cycle ?  XHCI_TRB_CYCLE : 0U;

  xhci_trb_t *ring = device->bulk_out_ring.virtual;
  xhci_trb_t *trb = &ring[index];

  // Normal TRB: one data buffer, one transfer event.
  trb->parameter = buffer->bus;
  trb->status = length;
  trb->control = (1U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | cycle;

  result->slot_id = device->slot_id;
  result->endpoint_id = XHCI_BULK_OUT_DCI;
  result->transfer_trb = device->bulk_out_ring.bus +
      (uint64_t)index * sizeof(xhci_trb_t);
  result->length = length;

  if (!xhci_bulk_out_publish(device, index, cache.data_line_size)) {
    device->bulk_out_failed = true;
    dma_end_device(buffer);
    return XHCI_BULK_OUT_RING_FAILED;
  }

  // Register the transfer completion before
  // ringing the Bulk OUT endpoint doorbell.
  xhci_event_waiter_t *waiter = xhci_waiter_register(
      XHCI_TRB_TRANSFER_EVENT, device->slot_id,
      XHCI_BULK_OUT_DCI, result->transfer_trb);

  if (!waiter) {
    device->bulk_out_failed = true;
    if (!dma_end_device(buffer))
      return XHCI_BULK_OUT_DMA_FAILED;
    return XHCI_BULK_OUT_EVENT_FAILED;
  }

  device->bulk_out_active = true;
  __asm__ volatile("dsb sy" ::: "memory");

  // Device doorbell, target 4: EP2 OUT.
  xhci_write(controller.doorbell_offset +
      4U * device->slot_id, XHCI_BULK_OUT_DCI);

  uint64_t started = timer_now_us();
  while (timer_now_us() - started < XHCI_TRANSFER_TIMEOUT_US) {
    xhci_dispatch_result_t status = xhci_dispatch_wait_step();

    if (status == XHCI_DISPATCH_READ_ERROR ||
        status == XHCI_DISPATCH_UNHANDLED)
      goto event_failed;

    if (!waiter->completed) {
      uint32_t usbsts = xhci_read(
          controller.capability_length + XHCI_USBSTS);
      if (usbsts & (XHCI_STS_HALTED | XHCI_STS_HOST_ERROR)) {
        result->controller_status = usbsts;
        goto event_failed;
      }
      continue;
    }

    // The dispatcher has matched the event to
    // our slot, endpoint and submitted transfer TRB.
    xhci_event_t event = waiter->event;
    xhci_waiter_release(waiter);

    result->completion_code = event.completion_code;
    result->event_parameter = event.parameter;
    result->event_status = event.status;
    result->elapsed_us = timer_now_us() - started;
    result->controller_status = xhci_read(
        controller.capability_length + XHCI_USBSTS);

    if (event.completion_code != XHCI_COMPLETION_SUCCESS)
      goto transfer_failed;

    // The controller has finished reading the data buffer.
    if (!dma_end_device(buffer))
      goto dma_failed;

    // Once a transfer in the new cycle completes, the
    // controller must have traversed the old Link TRB.
    if (device->bulk_out_wrap_pending) {
      xhci_trb_t *link = &ring[XHCI_RING_TRBS - 1U];
      link->control = (link->control & ~XHCI_TRB_CYCLE) |
          (device->bulk_out_cycle ? XHCI_TRB_CYCLE : 0U);
      if (!xhci_bulk_out_publish(device,
          XHCI_RING_TRBS - 1U, cache.data_line_size))
        goto ring_failed;
      device->bulk_out_wrap_pending = false;
    }
    device->bulk_out_index++;
    if (device->bulk_out_index == XHCI_RING_TRBS - 1U) {
      device->bulk_out_index = 0U;
      device->bulk_out_cycle ^= 1U;
      device->bulk_out_wrap_pending = true;
    }

    device->bulk_out_active = false;
    return XHCI_BULK_OUT_OK;
  }

  result->elapsed_us = timer_now_us() - started;
  result->controller_status = xhci_read(
      controller.capability_length + XHCI_USBSTS);
  xhci_waiter_release(waiter);
  device->bulk_out_failed = true;
  device->bulk_out_active = false;
  return XHCI_BULK_OUT_TIMEOUT;

transfer_failed:
  device->bulk_out_failed = true;
  device->bulk_out_active = false;
  return XHCI_BULK_OUT_TRANSFER_FAILED;

event_failed:
  xhci_waiter_release(waiter);
  device->bulk_out_failed = true;
  device->bulk_out_active = false;
  return XHCI_BULK_OUT_EVENT_FAILED;

dma_failed:
  device->bulk_out_failed = true;
  device->bulk_out_active = false;
  return XHCI_BULK_OUT_DMA_FAILED;

ring_failed:
  device->bulk_out_failed = true;
  device->bulk_out_active = false;
  return XHCI_BULK_OUT_RING_FAILED;
}

static bool xhci_bulk_in_publish(xhci_device_t *device,
    unsigned int index, size_t line) {

  xhci_trb_t *ring = device->bulk_in_ring.virtual;
  uintptr_t address = (uintptr_t)&ring[index];
  address &= ~(uintptr_t)(line - 1U);

  return cache_clean((const void *)address, line);
}

static bool xhci_bulk_in_advance(xhci_device_t *device, size_t line) {
  xhci_trb_t *ring = device->bulk_in_ring.virtual;

  // Once the first transfer in a new cycle completes,
  // the controller has traversed the old Link TRB.
  if (device->bulk_in_wrap_pending) {
    xhci_trb_t *link = &ring[XHCI_RING_TRBS - 1U];
    link->control = (link->control & ~XHCI_TRB_CYCLE) |
        (device->bulk_in_cycle ? XHCI_TRB_CYCLE : 0U);
    if (!xhci_bulk_in_publish(device, XHCI_RING_TRBS - 1U, line))
      return false;
    device->bulk_in_wrap_pending = false;
  }

  device->bulk_in_index++;
  // Entry 255 is the Link TRB, not a data TRB.
  if (device->bulk_in_index == XHCI_RING_TRBS - 1U) {
    device->bulk_in_index = 0U;
    device->bulk_in_cycle ^= 1U;
    device->bulk_in_wrap_pending = true;
  }

  return true;
}

static xhci_bulk_in_arm_result_t xhci_bulk_in_arm_one(
    xhci_device_t *device) {
  if (!device)
    return XHCI_BULK_IN_ARM_NOT_READY;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;
  if (receiver->state != XHCI_BULK_IN_IDLE &&
      receiver->state != XHCI_BULK_IN_COMPLETE)
    return XHCI_BULK_IN_ARM_ALREADY_STARTED;

  if (device->lifecycle.quiesced || !device->bulk_in_ready || !device->addressed ||
      !controller_running || !controller_verified ||
      command_ring_failed || dispatch_fault != XHCI_DISPATCH_NONE ||
      device->bulk_in_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_BULK_IN_ARM_NOT_READY;

  xhci_info_t controller;
  xhci_port_t port;
  if (!xhci_probe(&controller) || !xhci_port_read(device->root_port, &port) ||
      !port.connected || !port.enabled || !port.powered ||
      port.overcurrent || port.resetting || port.speed != 3U)
    return XHCI_BULK_IN_ARM_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_BULK_IN_ARM_NOT_READY;

  dma_buffer_t *buffer = &device->bulk_in_buffer;
  if (!buffer->pages && !dma_alloc(XHCI_BULK_IN_MAX_DATA, 64U, buffer))
    return XHCI_BULK_IN_ARM_ALLOCATION_FAILED;

  // The device will write into this buffer.
  if (!dma_begin_device(buffer, DMA_FROM_DEVICE))
    return XHCI_BULK_IN_ARM_DMA_FAILED;

  // One outstanding receive TD, submitted at
  // the current Bulk IN producer position.
  unsigned int index = device->bulk_in_index;
  uint32_t cycle = device->bulk_in_cycle ? XHCI_TRB_CYCLE : 0U;
  xhci_trb_t *ring = device->bulk_in_ring.virtual;
  xhci_trb_t *trb = &ring[index];
  trb->parameter = buffer->bus;
  trb->status = XHCI_BULK_IN_MAX_DATA;
  trb->control = (1U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | cycle;

  uint64_t trb_address = device->bulk_in_ring.bus +
      (uint64_t)index * sizeof(xhci_trb_t);
  if (!xhci_bulk_in_publish(device, index, cache.data_line_size)) {
    receiver->state = XHCI_BULK_IN_FAILED;
    return XHCI_BULK_IN_ARM_RING_FAILED;
  }

  // A completion may arrive immediately after
  // the doorbell, so register its owner first.
  receiver->waiter = xhci_waiter_register(
      XHCI_TRB_TRANSFER_EVENT, device->slot_id,
      XHCI_BULK_IN_DCI, trb_address);

  if (!receiver->waiter) {
    receiver->state = XHCI_BULK_IN_FAILED;
    return XHCI_BULK_IN_ARM_WAITER_FAILED;
  }

  receiver->info = (xhci_bulk_in_info_t){
    .transfer_trb = trb_address,
    .requested = XHCI_BULK_IN_MAX_DATA,
    .slot_id = device->slot_id,
    .endpoint_id = XHCI_BULK_IN_DCI
  };
  receiver->state = XHCI_BULK_IN_PENDING;

  __asm__ volatile("dsb sy" ::: "memory");

  // Device doorbell, target 3: EP1 IN.
  xhci_write(controller.doorbell_offset +
      4U * device->slot_id, XHCI_BULK_IN_DCI);

  return XHCI_BULK_IN_ARM_OK;
}

static xhci_bulk_in_state_t xhci_downstream_bulk_in_status(
    xhci_device_t *device, xhci_bulk_in_info_t *result) {
  if (!device || !result)
    return XHCI_BULK_IN_FAILED;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;

  if (!result)
    return XHCI_BULK_IN_FAILED;

  if (receiver->state == XHCI_BULK_IN_PENDING) {
    // An event-stream failure makes completion
    // uncertain. Do not inspect device-owned DMA.
    if (dispatch_fault != XHCI_DISPATCH_NONE) {
      receiver->state = XHCI_BULK_IN_FAILED;
    } else if (receiver->waiter && devices[1].receiver.waiter->completed) {
      xhci_event_t event = receiver->waiter->event;
      xhci_waiter_release(receiver->waiter);
      receiver->waiter = NULL;
      receiver->info.completion_code = event.completion_code;
      receiver->info.event_parameter = event.parameter;
      receiver->info.event_status = event.status;

      // For this single-TRB TD, the low 24 bits
      // represent the number of bytes not received.
      uint32_t residual = event.status & 0x00ffffffU;
      receiver->info.residual = residual;
      bool successful = event.completion_code == XHCI_COMPLETION_SUCCESS ||
          event.completion_code == XHCI_COMPLETION_SHORT_PACKET;
      if (!successful || residual > XHCI_BULK_IN_MAX_DATA ||
          (event.completion_code == XHCI_COMPLETION_SUCCESS &&
           residual != 0U)) {
        receiver->state = XHCI_BULK_IN_FAILED;
      } else {
        dma_buffer_t *buffer = &device->bulk_in_buffer;
        if (!dma_end_device(buffer)) {
          receiver->state = XHCI_BULK_IN_FAILED;
        } else {
          uint32_t received = XHCI_BULK_IN_MAX_DATA - residual;
          const uint8_t *bytes = buffer->virtual;
          for (uint32_t i = 0U; i < received; i++)
            receiver->info.data[i] = bytes[i];

          receiver->info.received = received;
          // The completed TD has been consumed.
          // Advance the producer before another arm.
          cache_info_t cache;
          if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
              cache.data_line_size > PAGE_SIZE ||
              (cache.data_line_size & (cache.data_line_size - 1U)) ||
              !xhci_bulk_in_advance(device, cache.data_line_size)) {
            receiver->state = XHCI_BULK_IN_FAILED;
          } else {
            receiver->state = XHCI_BULK_IN_COMPLETE;
          }
        }
      }
    }
  }

  *result = receiver->info;
  return receiver->state;
}

static xhci_in_reconcile_result_t
xhci_bulk_in_quiesced_reconcile_device(xhci_device_t *device) {
  if (!device)
    return XHCI_IN_RECONCILE_NOT_READY;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;

  if (!receiver->started)
    return XHCI_IN_RECONCILE_NOT_STARTED;

  // Never inspect an active stream or a controller
  // whose event-dispatch state is uncertain.
  if (!device->lifecycle.quiesced || device->lifecycle.slot_disabled ||
      dispatch_fault != XHCI_DISPATCH_NONE)
    return XHCI_IN_RECONCILE_NOT_READY;

  if (receiver->state == XHCI_BULK_IN_FAILED ||
      receiver->state == XHCI_BULK_IN_COMPLETE)
    return XHCI_IN_RECONCILE_RESOLVED;

  if (receiver->state != XHCI_BULK_IN_PENDING)
    return XHCI_IN_RECONCILE_FAILED;

  // A registered waiter must remain intact until
  // its completion has been delivered and consumed.
  if (!receiver->waiter)
    return XHCI_IN_RECONCILE_FAILED;

  if (!receiver->waiter->completed)
    return XHCI_IN_RECONCILE_PENDING;

  xhci_bulk_in_info_t info = {0};
  xhci_bulk_in_state_t state = xhci_downstream_bulk_in_status(device, &info);
  if (state != XHCI_BULK_IN_FAILED && state != XHCI_BULK_IN_COMPLETE)
    return XHCI_IN_RECONCILE_FAILED;

  // The stream is no longer operational. Preserve
  // its transfer result for diagnostics, and do
  // not submit another transfer after disconnection.
  receiver->failed = true;

  return XHCI_IN_RECONCILE_OK;
}

static xhci_bulk_in_arm_result_t xhci_bulk_in_stream_start_device(
    xhci_device_t *device) {
  if (!device)
    return XHCI_BULK_IN_ARM_NOT_READY;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;

  if (receiver->started)
    return XHCI_BULK_IN_ARM_ALREADY_STARTED;

  xhci_bulk_in_arm_result_t result = xhci_bulk_in_arm_one(device);
  receiver->stats.last_arm_result = result;
  if (result != XHCI_BULK_IN_ARM_OK)
    return result;

  receiver->started = true;
  receiver->queued = false;

  return XHCI_BULK_IN_ARM_OK;
}

static void xhci_bulk_in_stream_service_device(xhci_device_t *device) {
  if (!device)
    return;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;

  if (!receiver->started || receiver->failed || device->lifecycle.quiesced)
    return;

  xhci_bulk_in_info_t info;
  xhci_bulk_in_state_t state = xhci_downstream_bulk_in_status(device, &info);
  if (state == XHCI_BULK_IN_FAILED) {
    receiver->failed = true;
    return;
  }

  // Nothing has completed yet.
  if (state == XHCI_BULK_IN_PENDING)
    return;

  if (state != XHCI_BULK_IN_COMPLETE) {
    receiver->failed = true;
    return;
  }

  if (!receiver->queued) {
    // With one outstanding TD and arming disabled
    // at capacity, this must never be reached.
    if (receiver->queue_count == XHCI_BULK_IN_QUEUE_CAPACITY) {
      receiver->failed = true;
      return;
    }

    unsigned int tail = (receiver->queue_head +
        receiver->queue_count) % XHCI_BULK_IN_QUEUE_CAPACITY;
    receiver->queue[tail] = info;
    receiver->queue_count++;
    receiver->queued = true;
    receiver->stats.transfers++;
    receiver->stats.bytes += info.received;
    receiver->stats.last_completion_code = info.completion_code;

    if (receiver->queue_count > receiver->stats.queue_peak)
      receiver->stats.queue_peak = receiver->queue_count;

    // Count transitions past the final data TRB.
    uint64_t last_trb = device->bulk_in_ring.bus +
        (uint64_t)(XHCI_RING_TRBS - 2U) * sizeof(xhci_trb_t);
    if (info.transfer_trb == last_trb)
      receiver->stats.ring_wraps++;
    if (receiver->queue_count == XHCI_BULK_IN_QUEUE_CAPACITY)
      receiver->stats.pauses++;
  }

  // Do not submit another transfer until there is
  // space to retain its eventual completion.
  if (receiver->queue_count == XHCI_BULK_IN_QUEUE_CAPACITY)
    return;

  xhci_bulk_in_arm_result_t result = xhci_bulk_in_arm_one(device);
  receiver->stats.last_arm_result = result;
  if (result != XHCI_BULK_IN_ARM_OK) {
    receiver->failed = true;
    return;
  }

  receiver->queued = false;
}

static bool xhci_bulk_in_stream_get_stats_device( xhci_device_t *device,
    xhci_bulk_in_stream_stats_t *result) {
  if (!device || !result)
    return false;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;

  if (!result)
    return false;

  *result = receiver->stats;
  result->started = receiver->started;
  result->failed = receiver->failed;
  result->queue_depth = receiver->queue_count;
  result->paused = receiver->started && receiver->failed &&
      receiver->queued &&
      receiver->queue_count == XHCI_BULK_IN_QUEUE_CAPACITY;

  return true;
}

static bool xhci_bulk_in_stream_read_device(xhci_device_t *device,
    xhci_bulk_in_info_t *result) {
  if (!device || !result)
    return false;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;

  if (!result || receiver->queue_count == 0U)
    return false;

  *result = receiver->queue[device->receiver.queue_head];
  receiver->queue_head = (device->receiver.queue_head + 1U) %
      XHCI_BULK_IN_QUEUE_CAPACITY;

  receiver->queue_count--;
  return true;
}

static xhci_bulk_in_retire_result_t xhci_bulk_in_stream_retire_device(
    xhci_device_t *device) {
  if (!device)
    return XHCI_BULK_IN_RETIRE_NOT_READY;

  xhci_bulk_in_receiver_t *receiver = &device->receiver;

  if (!receiver->started)
    return XHCI_BULK_IN_RETIRE_NOT_STARTED;

  // The controller must have relinquished
  // the slot before retiring receiver state.
  if (!device->lifecycle.quiesced || !device->lifecycle.slot_disabled ||
      device->lifecycle.cleaned || !controller_running ||
      !controller_verified || command_outstanding ||
      command_ring_failed || dispatch_fault != XHCI_DISPATCH_NONE)
    return XHCI_BULK_IN_RETIRE_NOT_READY;

  // The event dispatcher must no longer have
  // any outstanding owner for this device.
  if (receiver->waiter || receiver->state == XHCI_BULK_IN_PENDING ||
      device->ep0_active || device->bulk_out_active)
    return XHCI_BULK_IN_RETIRE_BUSY;

  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];
    if (waiter->active && waiter->slot_id == device->slot_id)
      return XHCI_BULK_IN_RETIRE_BUSY;
  }

  // The receiver must have reached a terminal
  // transfer state before we discard its state.
  if (receiver->state != XHCI_BULK_IN_FAILED &&
      receiver->state != XHCI_BULK_IN_COMPLETE)
    return XHCI_BULK_IN_RETIRE_BUSY;

  // Disable Slot has completed. The controller
  // no longer owns this endpoint's execution.
  receiver->started = false;
  receiver->failed = false;
  receiver->queued = false;
  receiver->state = XHCI_BULK_IN_IDLE;
  receiver->waiter = NULL;
  receiver->queue_head = 0U;
  receiver->queue_count = 0U;

  // Preserve the last transfer information and
  // statistics until downstream recycling.
  return XHCI_BULK_IN_RETIRE_OK;
}

// Compatibility entry points for the current
// single managed downstream device.
//
// Device selection will move into the USB core
// once multiple device contexts are admitted.

xhci_in_reconcile_result_t xhci_bulk_in_quiesced_reconcile(void) {
  return xhci_bulk_in_quiesced_reconcile_device(&devices[1]);
}

xhci_bulk_in_arm_result_t xhci_bulk_in_stream_start(void) {
  return xhci_bulk_in_stream_start_device(&devices[1]);
}

void xhci_bulk_in_stream_service(void) {
  xhci_bulk_in_stream_service_device(&devices[1]);
}

bool xhci_bulk_in_stream_get_stats(
    xhci_bulk_in_stream_stats_t *result) {
  return xhci_bulk_in_stream_get_stats_device(&devices[1], result);
}

bool xhci_bulk_in_stream_read(xhci_bulk_in_info_t *result) {
  return xhci_bulk_in_stream_read_device(&devices[1], result);
}

xhci_bulk_in_retire_result_t xhci_bulk_in_stream_retire(void) {
  return xhci_bulk_in_stream_retire_device(&devices[1]);
}

xhci_hub_result_t xhci_hub_configure(
    uint8_t ports, uint8_t tt_think_time, xhci_hub_info_t *result) {

  if (!result)
    return XHCI_HUB_NOT_READY;

  *result = (xhci_hub_info_t){0};
  if (hub_configuration_attempted)
    return hub_configured ?  XHCI_HUB_ALREADY_CONFIGURED : XHCI_HUB_NOT_READY;

  if (!internal_hub.prepared || !internal_hub.addressed ||
      internal_hub.slot_id == 0U ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed ||
      ports == 0U || tt_think_time > 3U)
    return XHCI_HUB_NOT_READY;

  if (internal_hub.input.owner != DMA_OWNER_DEVICE ||
      internal_hub.output.owner != DMA_OWNER_DEVICE)
    return XHCI_HUB_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_HUB_DMA_FAILED;

  uint32_t size = internal_hub.context_size;
  uint8_t *input = internal_hub.input.virtual;
  uint32_t *control = (uint32_t *)input;
  uint32_t *slot = (uint32_t *)(input + size);
  // Configure Endpoint:
  // Drop Context flags = 0.
  // Add Context flags = A0 only (Slot Context).
  // No endpoint contexts are added or removed.
  control[0] = 0U;
  control[1] = 1U;

  // Preserve route, speed and context entries.
  slot[0] |= XHCI_SLOT_HUB_FLAG;

  // Slot DW1: Number of downstream ports, bits 31:24.
  slot[1] = (slot[1] & 0x00ffffffU) | ((uint32_t)ports << 24);

  // Slot DW2: TT Think Time, bits 17:16.
  // MTT remains zero for this single-TT hub.
  slot[2] = (slot[2] & ~0x00030000U) | ((uint32_t)tt_think_time << 16);

  result->slot_id = internal_hub.slot_id;
  result->ports = ports;
  result->tt_think_time = tt_think_time;
  result->slot_dword0 = slot[0];
  result->slot_dword1 = slot[1];
  result->slot_dword2 = slot[2];

  // The input allocation is already device-owned.
  // Publish the modified control and slot contexts.
  if (!cache_clean(input, 2U * size))
    return XHCI_HUB_DMA_FAILED;

  __asm__ volatile("dsb sy" ::: "memory");

  xhci_trb_t command = {
    .parameter = internal_hub.input.bus,
    .status = 0U,
    .control = (XHCI_TRB_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)internal_hub.slot_id << 24)
  };

  // A completion timeout makes the controller's state
  // uncertain. Never submit this operation twice.
  hub_configuration_attempted = true;
  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);
  result->command_result = status;
  result->completion_code = completion.completion_code;

  if (status != XHCI_COMMAND_OK || completion.slot_id != internal_hub.slot_id)
    return XHCI_HUB_COMMAND_FAILED;

  // Refresh the first two Output Context entries.
  uint8_t *output = internal_hub.output.virtual;
  if (!cache_invalidate(output, 2U * size))
    return XHCI_HUB_DMA_FAILED;

  uint32_t *output_slot = (uint32_t *)output;
  result->output_dword0 = output_slot[0];
  result->output_dword1 = output_slot[1];
  result->output_dword2 = output_slot[2];
  if (!(output_slot[0] & XHCI_SLOT_HUB_FLAG) ||
      ((output_slot[1] >> 24) & 0xffU) != ports ||
      ((output_slot[2] >> 16) & 3U) != tt_think_time)
    return XHCI_HUB_VERIFY_FAILED;

  hub_configured = true;
  return XHCI_HUB_OK;
}

xhci_address_result_t xhci_address_device(
    xhci_address_info_t *result) {

  if (!result)
    return XHCI_ADDRESS_NOT_READY;

  *result = (xhci_address_info_t){0};

  if (address_attempted) {
    if (device_addressed)
      *result = address_info;
    return XHCI_ADDRESS_ALREADY_ATTEMPTED;
  }

  if (!internal_hub.prepared || internal_hub_slot == 0 ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed)
    return XHCI_ADDRESS_NOT_READY;

  if (internal_hub.input.owner != DMA_OWNER_DEVICE ||
      internal_hub.output.owner != DMA_OWNER_DEVICE ||
      internal_hub.ep0_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_ADDRESS_NOT_READY;

  xhci_port_t port;
  if (!xhci_port_read(1U, &port) ||
      !port.connected || !port.enabled || !port.powered ||
      port.resetting || port.speed != 3U)
    return XHCI_ADDRESS_NOT_READY;

  xhci_info_t controller;
  if (!xhci_probe(&controller) ||
      internal_hub_slot > controller.max_slots)
    return XHCI_ADDRESS_NOT_READY;

  result->slot_id = internal_hub_slot;
  result->command_result = XHCI_COMMAND_UNAVAILABLE;

  // The input context must be 16-byte aligned.
  if ((internal_hub.input.bus & 0x0fULL) != 0)
    return XHCI_ADDRESS_NOT_READY;

  xhci_trb_t command = {
    .parameter = internal_hub.input.bus,
    .status = 0,
    .control = (XHCI_TRB_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)internal_hub_slot << 24)
  };
  // BSR bit 9 is zero: issue USB SET_ADDRESS.
  // Never blindly resubmit if completion is lost.
  address_attempted = true;
  xhci_command_completion_t completion;
  xhci_command_result_t status = xhci_command_submit( &command, &completion);
  result->command_result = status;
  result->completion_code = completion.completion_code;
  result->controller_status = completion.controller_status;
  result->elapsed_us = completion.elapsed_us;

  if (status != XHCI_COMMAND_OK)
    return XHCI_ADDRESS_COMMAND_FAILED;

  if (completion.slot_id != internal_hub_slot)
    return XHCI_ADDRESS_BAD_SLOT;

  // The controller has written the Output Device Context.
  // Invalidate only the relevant cache lines, while keeping
  // the allocation under device ownership.
  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0 ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_ADDRESS_VERIFY_FAILED;

  uint32_t size = internal_hub.info.context_size;
  uint8_t *output = internal_hub.output.virtual;
  if (!cache_invalidate(output, 2U * size))
    return XHCI_ADDRESS_VERIFY_FAILED;

  uint32_t *slot = (uint32_t *)output;
  uint32_t *ep0 = (uint32_t *)(output + size);
  result->slot_dword3 = slot[3];
  result->ep0_dword0 = ep0[0];
  result->usb_address = (uint8_t)(slot[3] & 0xffU);
  result->slot_state = (uint8_t)((slot[3] >> 27) & 0x1fU);
  result->ep0_state = (uint8_t)(ep0[0] & 0x07U);

  // Addressed = 2; endpoint Running = 1.
  if (result->usb_address == 0 || result->usb_address > 127 ||
      result->slot_state != 2U || result->ep0_state != 1U)
    return XHCI_ADDRESS_VERIFY_FAILED;

  internal_hub.addressed = true;
  address_info = *result;
  device_addressed = true;
  return XHCI_ADDRESS_OK;
}

xhci_address_result_t xhci_address_downstream(xhci_address_info_t *result) {
  if (!result)
    return XHCI_ADDRESS_NOT_READY;

  *result = (xhci_address_info_t){0};
  xhci_device_t *device = &devices[1];
  // Never repeat Address Device after an uncertain completion.
  if (device->lifecycle.address_attempted)
    return XHCI_ADDRESS_ALREADY_ATTEMPTED;

  // The parent hub must already be registered as a hub
  // in the xHCI Output Slot Context.
  if (!hub_configured || !internal_hub.addressed || !device->prepared ||
      device->addressed || device->slot_id == 0U ||
      device->hub_slot != internal_hub.slot_id ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed)
    return XHCI_ADDRESS_NOT_READY;

  if (device->input.owner != DMA_OWNER_DEVICE ||
      device->output.owner != DMA_OWNER_DEVICE ||
      device->ep0_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_ADDRESS_NOT_READY;

  // Ensure the expected downstream topology is present.
  if (device->root_port != 1U || device->hub_port == 0U ||
      device->hub_port > 4U || device->speed != 1U ||
      device->route_string != device->hub_port ||
      device->ep0_max_packet != 8U)
    return XHCI_ADDRESS_NOT_READY;

  xhci_info_t controller;
  if (!xhci_probe(&controller) || device->slot_id > controller.max_slots ||
      device->slot_id == internal_hub.slot_id)
    return XHCI_ADDRESS_NOT_READY;

  // Input Context must be aligned to at least 16 bytes.
  if (device->input.bus & 0x0fULL)
    return XHCI_ADDRESS_NOT_READY;

  if (device->context_size != 32U && device->context_size != 64U)
    return XHCI_ADDRESS_NOT_READY;

  // The contexts were prepared in the previous sprint.
  // Check their essential topology fields before use.
  uint32_t size = device->context_size;
  uint8_t *input = device->input.virtual;
  uint32_t *control = (uint32_t *)input;
  uint32_t *slot = (uint32_t *)(input + size);
  uint32_t *ep0 = (uint32_t *)(input + 2U * size);

  if (control[1] != 3U ||
      (slot[0] & 0x000fffffU) != device->route_string ||
      ((slot[0] >> 20) & 0x0fU) != device->speed ||
      ((slot[1] >> 16) & 0xffU) != device->root_port ||
      (slot[2] & 0xffU) != device->hub_slot ||
      ((slot[2] >> 8) & 0xffU) != device->hub_port ||
      ((ep0[1] >> 16) & 0xffffU) != 8U)
    return XHCI_ADDRESS_NOT_READY;

  result->slot_id = device->slot_id;
  result->command_result = XHCI_COMMAND_UNAVAILABLE;

  // Address Device Command:
  //   Parameter: Input Context bus address
  //   Slot ID:   downstream device slot
  //   BSR = 0:   controller performs SET_ADDRESS
  xhci_trb_t command = {
    .parameter = device->input.bus,
    .status = 0U,
    .control = (XHCI_TRB_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)device->slot_id << 24)
  };

  device->lifecycle.address_attempted = true;
  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);
  result->command_result = status;
  result->completion_code = completion.completion_code;
  result->controller_status = completion.controller_status;
  result->elapsed_us = completion.elapsed_us;

  if (status != XHCI_COMMAND_OK)
    return XHCI_ADDRESS_COMMAND_FAILED;

  if (completion.slot_id != device->slot_id)
    return XHCI_ADDRESS_BAD_SLOT;

  // Refresh the Output Slot and EP0 contexts.
  // DMA ownership remains with the controller.
  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_ADDRESS_VERIFY_FAILED;

  uint8_t *output = device->output.virtual;
  if (!cache_invalidate(output, 2U * size))
    return XHCI_ADDRESS_VERIFY_FAILED;

  uint32_t *output_slot = (uint32_t *)output;
  uint32_t *output_ep0 = (uint32_t *)(output + size);
  result->slot_dword3 = output_slot[3];
  result->ep0_dword0 = output_ep0[0];
  result->usb_address = (uint8_t)(output_slot[3] & 0xffU);
  result->slot_state = (uint8_t)((output_slot[3] >> 27) & 0x1fU);
  result->ep0_state = (uint8_t)(output_ep0[0] & 0x07U);
  // A successful Address Device command should leave
  // the slot Addressed and Endpoint 0 Running.
  if (result->usb_address == 0U || result->usb_address > 127U ||
      result->slot_state != 2U || result->ep0_state != 1U)
    return XHCI_ADDRESS_VERIFY_FAILED;

  device->addressed = true;
  return XHCI_ADDRESS_OK;
}

xhci_ep0_update_result_t xhci_downstream_ep0_update(
    uint16_t max_packet_size, xhci_ep0_update_info_t *result) {

  if (!result)
    return XHCI_EP0_UPDATE_NOT_READY;

  *result = (xhci_ep0_update_info_t){0};

  xhci_device_t *device = &devices[1];
  if (device->lifecycle.ep0_update_attempted)
    return XHCI_EP0_UPDATE_ALREADY_ATTEMPTED;

  if (!device->prepared || !device->addressed ||
      device->slot_id == 0U || !controller_running ||
      !controller_verified || command_outstanding ||
      command_ring_failed || device->ep0_active ||
      device->ep0_failed || max_packet_size != 64U)
    return XHCI_EP0_UPDATE_NOT_READY;

  if (device->input.owner != DMA_OWNER_DEVICE ||
      device->output.owner != DMA_OWNER_DEVICE ||
      device->ep0_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_EP0_UPDATE_NOT_READY;

  if (device->context_size != 32U && device->context_size != 64U)
    return XHCI_EP0_UPDATE_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_EP0_UPDATE_DMA_FAILED;

  uint32_t size = device->context_size;
  uint8_t *input = device->input.virtual;
  uint8_t *output = device->output.virtual;
  // Output Context layout:
  //   Context 0: Slot
  //   Context 1: EP0
  // Input Context layout:
  //   Context 0: Input Control
  //   Context 1: Slot
  //   Context 2: EP0
  if (!cache_invalidate(output, 2U * size))
    return XHCI_EP0_UPDATE_DMA_FAILED;

  uint32_t *output_ep0 = (uint32_t *)(output + size);
  uint32_t *input_ep0 = (uint32_t *)(input + 2U * size);
  uint32_t *control = (uint32_t *)input;
  // Copy the controller's current EP0 Context.
  for (uint32_t i = 0U; i < size / sizeof(uint32_t); i++)
    input_ep0[i] = output_ep0[i];

  result->slot_id = device->slot_id;
  result->old_mps = (uint16_t)(output_ep0[1] >> 16);
  if (result->old_mps != 8U || (output_ep0[0] & 0x07U) != 1U)
    return XHCI_EP0_UPDATE_NOT_READY;

  // Evaluate Context:
  //   Drop flags = 0
  //   Add flags = A1 (EP0) only
  control[0] = 0U;
  control[1] = 2U;

  // Change only Max Packet Size, bits 31:16 of EP0 DW1.
  input_ep0[1] = (input_ep0[1] & 0x0000ffffU) |
      ((uint32_t)max_packet_size << 16);

  result->input_dword1 = input_ep0[1];
  result->new_mps = max_packet_size;

  // Publish the Input Control Context and Input EP0
  // Context. Include the intervening Slot Context.
  if (!cache_clean(input, 3U * size))
    return XHCI_EP0_UPDATE_DMA_FAILED;

  __asm__ volatile("dsb sy" ::: "memory");

  xhci_trb_t command = {
    .parameter = device->input.bus,
    .status = 0U,
    .control = (XHCI_TRB_EVALUATE_CONTEXT << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)device->slot_id << 24)
  };

  // An uncertain command completion must not be retried.
  device->lifecycle.ep0_update_attempted = true;
  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);
  result->command_result = status;
  result->completion_code = completion.completion_code;
  result->elapsed_us = completion.elapsed_us;

  if (status != XHCI_COMMAND_OK || completion.slot_id != device->slot_id)
    return XHCI_EP0_UPDATE_COMMAND_FAILED;

  // Inspect the controller's updated EP0 Context.
  if (!cache_invalidate(output, 2U * size))
    return XHCI_EP0_UPDATE_DMA_FAILED;

  result->output_dword1 = output_ep0[1];
  result->ep0_state = (uint8_t)(output_ep0[0] & 0x07U);
  if ((output_ep0[1] >> 16) != max_packet_size || result->ep0_state != 1U)
    return XHCI_EP0_UPDATE_VERIFY_FAILED;

  device->ep0_max_packet = max_packet_size;
  return XHCI_EP0_UPDATE_OK;
}

static bool xhci_ep0_publish(xhci_device_t *device,
    unsigned int first, unsigned int count, size_t line) {

  xhci_trb_t *ring = device->ep0_ring.virtual;
  uintptr_t start = (uintptr_t)&ring[first];
  uintptr_t end = (uintptr_t)&ring[first + count];

  start &= ~(uintptr_t)(line - 1U);
  end = (end + line - 1U) & ~(uintptr_t)(line - 1U);

  return cache_clean((const void *)start, end - start);
}

static bool xhci_ep0_link_update(xhci_device_t *device, size_t line) {
  xhci_trb_t *ring = device->ep0_ring.virtual;
  xhci_trb_t *link = &ring[XHCI_RING_TRBS - 1U];

  link->control = (link->control & ~XHCI_TRB_CYCLE) |
      (device->ep0_cycle ? XHCI_TRB_CYCLE : 0U);

  return xhci_ep0_publish(device, XHCI_RING_TRBS - 1U, 1U, line);
}

static bool xhci_ep0_prepare(xhci_device_t *device,
    unsigned int count, size_t line) {

  const unsigned int usable = XHCI_RING_TRBS - 1U;

  if (count == 0U || count > usable || device->ep0_wrap_pending)
    return false;

  if (device->ep0_index + count <= usable)
    return true;

  // Fill the remaining ring entries with transfer No Ops.
  xhci_trb_t *ring = device->ep0_ring.virtual;
  unsigned int first = device->ep0_index;

  for (unsigned int i = first; i < usable; i++) {
    ring[i].parameter = 0U;
    ring[i].status = 0U;
    ring[i].control =
        (XHCI_TRB_NOOP_TRANSFER << XHCI_TRB_TYPE_SHIFT) |
        (device->ep0_cycle ? XHCI_TRB_CYCLE : 0U);
  }

  if (!xhci_ep0_publish(device, first, usable - first, line))
    return false;

  // The old-cycle Link TRB must remain valid until
  // the controller has followed it.
  device->ep0_index = 0U;
  device->ep0_cycle ^= 1U;
  device->ep0_wrap_pending = true;

  return true;
}

static bool xhci_ep0_advance(xhci_device_t *device,
    unsigned int count, size_t line) {
  const unsigned int usable = XHCI_RING_TRBS - 1U;

  device->ep0_index += count;
  if (device->ep0_index > usable)
    return false;

  if (device->ep0_wrap_pending) {
    device->ep0_wrap_pending = false;
    if (!xhci_ep0_link_update(device, line))
      return false;
  }

  if (device->ep0_index == usable) {
    device->ep0_index = 0U;
    device->ep0_cycle ^= 1U;
    if (!xhci_ep0_link_update(device, line))
      return false;
  }

  return true;
}


static xhci_control_result_t xhci_control_transfer(
    xhci_device_t *device, const xhci_setup_packet_t *setup,
    xhci_control_info_t *result) {

  if (!result)
    return XHCI_CONTROL_NOT_READY;

  *result = (xhci_control_info_t){0};

  if (!device || !setup || setup->length > XHCI_CONTROL_MAX_DATA)
    return XHCI_CONTROL_NOT_READY;

  if (setup->length != 0U && !(setup->request_type & 0x80U))
    return XHCI_CONTROL_NOT_READY;

  bool has_data = setup->length != 0U;
  unsigned int count = has_data ? 3U : 2U;
  if (!device->addressed || !device->prepared ||
      device->slot_id == 0U || !controller_running ||
      !controller_verified || command_outstanding ||
      command_ring_failed || device->ep0_active ||
      device->ep0_failed)
    return XHCI_CONTROL_NOT_READY;

  if (device->ep0_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_CONTROL_NOT_READY;

  // The upstream root port must remain operational.
  xhci_info_t controller;
  xhci_port_t port;
  if (!xhci_probe(&controller) ||
      !xhci_port_read(device->root_port, &port) ||
      !port.connected || !port.enabled || !port.powered ||
      port.resetting || port.speed != 3U)
    return XHCI_CONTROL_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_CONTROL_NOT_READY;

  // A new control-transfer attempt has passed the
  // controller and endpoint readiness checks.
  // Preserve evidence from the previous transfer
  // when an operation is rejected before this point.
  device->ep0_submitted = false;
  device->ep0_completion_seen = false;
  device->ep0_completion_code = 0U;

  dma_buffer_t *buffer = &device->control_buffer;
  if (has_data) {
    if (!buffer->pages && !dma_alloc(XHCI_CONTROL_MAX_DATA, 64U, buffer))
      return XHCI_CONTROL_ALLOCATION_FAILED;

    if (!dma_begin_device(buffer, DMA_FROM_DEVICE))
      return XHCI_CONTROL_DMA_FAILED;
  }

  if (!xhci_ep0_prepare(device, count, cache.data_line_size)) {
    device->ep0_failed = true;
    if (has_data)
      dma_end_device(buffer);
    return XHCI_CONTROL_RING_FAILED;
  }

  unsigned int first = device->ep0_index;
  uint32_t cycle_bit = device->ep0_cycle ? XHCI_TRB_CYCLE : 0U;
  xhci_trb_t *ring = device->ep0_ring.virtual;

  uint64_t packet = (uint64_t)setup->request_type |
      ((uint64_t)setup->request << 8) |
      ((uint64_t)setup->value << 16) |
      ((uint64_t)setup->index << 32) |
      ((uint64_t)setup->length << 48);
  ring[first].parameter = packet;
  ring[first].status = 8U;
  ring[first].control =
      (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT |
      (has_data ? XHCI_TRB_TRT_IN : 0U) | cycle_bit;

  if (has_data) {
    ring[first + 1U].parameter = buffer->bus;
    ring[first + 1U].status = setup->length;
    ring[first + 1U].control =
        (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_DIR_IN | cycle_bit;
  }

  unsigned int status_index = first + count - 1U;
  ring[status_index].parameter = 0U;
  ring[status_index].status = 0U;
  ring[status_index].control =
      (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) |
      XHCI_TRB_IOC | (has_data ? 0U : XHCI_TRB_DIR_IN) | cycle_bit;

  uint64_t status_trb = device->ep0_ring.bus +
      (uint64_t)status_index * sizeof(xhci_trb_t);

  result->slot_id = device->slot_id;
  result->status_trb = status_trb;
  if (!xhci_ep0_publish(device, first, count, cache.data_line_size)) {
    if (has_data)
      dma_end_device(buffer);
    device->ep0_failed = true;
    return XHCI_CONTROL_RING_FAILED;
  }

  // Register the status-stage completion before
  // ringing the endpoint doorbell.
  xhci_event_waiter_t *waiter = xhci_waiter_register(
      XHCI_TRB_TRANSFER_EVENT, device->slot_id, 1U, status_trb);
  if (!waiter) {
    if (has_data)
      dma_end_device(buffer);
    device->ep0_failed = true;
    return XHCI_CONTROL_EVENT_FAILED;
  }

  // Associate earlier error-generating TRBs
  // with the same completion owner.
  waiter->td_start = device->ep0_ring.bus +
      (uint64_t)first * sizeof(xhci_trb_t);

  device->ep0_active = true;
  __asm__ volatile("dsb sy" ::: "memory");

  // Doorbell index is the hardware slot ID.
  // Target 1 selects the default control endpoint.
  xhci_write(controller.doorbell_offset + 4U * device->slot_id, 1U);
  device->ep0_submitted = true;

  uint64_t started = timer_now_us();
  while (timer_now_us() - started < XHCI_TRANSFER_TIMEOUT_US) {
    xhci_dispatch_result_t status = xhci_dispatch_wait_step();
    if (status == XHCI_DISPATCH_READ_ERROR ||
        status == XHCI_DISPATCH_UNHANDLED)
      goto event_failed;

    if (!waiter->completed) {
      uint32_t usbsts = xhci_read(controller.capability_length + XHCI_USBSTS);
      if (usbsts & (XHCI_STS_HALTED | XHCI_STS_HOST_ERROR)) {
        result->controller_status = usbsts;
        goto event_failed;
      }
      continue;
    }

    // The dispatcher has matched the transfer event
    // to our slot, endpoint and Status Stage TRB.
    xhci_event_t event = waiter->event;
    xhci_waiter_release(waiter);
    device->ep0_completion_seen = true;
    device->ep0_completion_code = event.completion_code;

    result->event_type = event.type;
    result->completion_code = event.completion_code;
    result->endpoint_id = (uint8_t)((event.control >> 16) & 0x1fU);
    result->event_parameter = event.parameter;
    result->event_status = event.status;
    result->elapsed_us = timer_now_us() - started;
    result->controller_status = xhci_read(
        controller.capability_length + XHCI_USBSTS);

    if (event.completion_code != XHCI_COMPLETION_SUCCESS) {
      device->ep0_failed = true;
      device->ep0_active = false;
      return XHCI_CONTROL_TRANSFER_FAILED;
    }

    if (has_data) {
      if (!dma_end_device(buffer))
        goto dma_failed;
      uint8_t *bytes = buffer->virtual;
      for (unsigned int i = 0; i < setup->length; i++)
        result->data[i] = bytes[i];
      result->length = setup->length;
    }

    if (!xhci_ep0_advance(device, count, cache.data_line_size)) {
      device->ep0_failed = true;
      device->ep0_active = false;
      return XHCI_CONTROL_RING_FAILED;
    }

    device->ep0_active = false;
    return XHCI_CONTROL_OK;
  }
  result->elapsed_us = timer_now_us() - started;
  result->controller_status = xhci_read(
      controller.capability_length + XHCI_USBSTS);
  xhci_waiter_release(waiter);
  device->ep0_failed = true;
  device->ep0_active = false;
  return XHCI_CONTROL_TIMEOUT;

event_failed:
  xhci_waiter_release(waiter);
  device->ep0_failed = true;
  device->ep0_active = false;
  return XHCI_CONTROL_EVENT_FAILED;

dma_failed:
  device->ep0_failed = true;
  device->ep0_active = false;
  return XHCI_CONTROL_DMA_FAILED;
}

xhci_control_result_t xhci_control_in(
    const xhci_setup_packet_t *setup, xhci_control_info_t *result) {

  if (!setup || !(setup->request_type & 0x80U) ||
      setup->length == 0U)
    return XHCI_CONTROL_NOT_READY;

  return xhci_control_transfer(&internal_hub, setup, result);
}

xhci_control_result_t xhci_control_no_data(
    const xhci_setup_packet_t *setup, xhci_control_info_t *result) {

  if (!setup || setup->length != 0U)
    return XHCI_CONTROL_NOT_READY;

  return xhci_control_transfer(&internal_hub, setup, result);
}

xhci_control_result_t xhci_downstream_control_in(
    const xhci_setup_packet_t *setup, xhci_control_info_t *result) {

  if (devices[1].lifecycle.quiesced || !setup ||
      !(setup->request_type & 0x80U) || setup->length == 0U)
    return XHCI_CONTROL_NOT_READY;

  return xhci_control_transfer(&devices[1], setup, result);
}

xhci_control_result_t xhci_downstream_control_no_data(
    const xhci_setup_packet_t *setup, xhci_control_info_t *result) {

  if (devices[1].lifecycle.quiesced || !setup || setup->length != 0U)
    return XHCI_CONTROL_NOT_READY;

  return xhci_control_transfer(&devices[1], setup, result);
}

xhci_noop_result_t xhci_noop(xhci_noop_info_t *result) {
  if (!result)
    return XHCI_NOOP_UNAVAILABLE;

  *result = (xhci_noop_info_t){0};

  xhci_trb_t command = {
    .parameter = 0,
    .status = 0,
    .control = XHCI_TRB_NOOP_CMD << XHCI_TRB_TYPE_SHIFT
  };

  xhci_command_completion_t completion;
  xhci_command_result_t status = xhci_command_submit(&command, &completion);

  // Preserve all existing diagnostic information.
  result->command_address = completion.command_address;
  result->event_parameter = completion.event_parameter;
  result->event_status = completion.event_status;
  result->event_control = completion.event_control;
  result->controller_status = completion.controller_status;
  result->completion_code = completion.completion_code;
  result->event_type = completion.event_type;
  result->elapsed_us = completion.elapsed_us;
  result->command_number = completion.command_number;
  switch (status) {
    case XHCI_COMMAND_OK:
      return XHCI_NOOP_OK;
    case XHCI_COMMAND_NOT_RUNNING:
      return XHCI_NOOP_NOT_RUNNING;
    case XHCI_COMMAND_BUSY:
      return XHCI_NOOP_ALREADY_SUBMITTED;
    case XHCI_COMMAND_UNAVAILABLE:
      return XHCI_NOOP_UNAVAILABLE;
    case XHCI_COMMAND_CACHE_ERROR:
      return XHCI_NOOP_CACHE_FAILED;
    case XHCI_COMMAND_TIMEOUT:
      return XHCI_NOOP_TIMEOUT;
    case XHCI_COMMAND_BAD_EVENT:
      return XHCI_NOOP_BAD_EVENT;
    case XHCI_COMMAND_FAILED:
      return XHCI_NOOP_COMMAND_FAILED;
  }

  return XHCI_NOOP_UNAVAILABLE;
}

xhci_interrupt_config_result_t xhci_hub_interrupt_configure(
    uint8_t address, uint16_t mps, uint8_t interval,
    xhci_interrupt_config_info_t *info) {

  if (!info)
    return XHCI_INTERRUPT_CONFIG_NOT_READY;

  *info = (xhci_interrupt_config_info_t){0};
  info->command_result = XHCI_COMMAND_UNAVAILABLE;
  if (hub_interrupt_attempted)
    return XHCI_INTERRUPT_CONFIG_ALREADY_ATTEMPTED;

  xhci_device_t *device = &internal_hub;
  if (!hub_configured || !device->prepared ||
      !device->addressed || device->slot_id == 0U ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed ||
      device->ep0_active || device->ep0_failed)
    return XHCI_INTERRUPT_CONFIG_NOT_READY;

  // This implementation supports a High-Speed hub.
  // Interrupt-IN endpoint addresses are 0x81..0x8f.
  if (device->speed != 3U || (address & 0xf0U) != 0x80U ||
      (address & 0x0fU) == 0U || mps == 0U || mps > 64U ||
      interval == 0U || interval > 16U)
    return XHCI_INTERRUPT_CONFIG_INVALID_ENDPOINT;

  uint8_t dci = (uint8_t)(2U * (address & 0x0fU) + 1U);
  if (device->context_size != 32U && device->context_size != 64U)
    return XHCI_INTERRUPT_CONFIG_NOT_READY;

  if (device->input.owner != DMA_OWNER_DEVICE ||
      device->output.owner != DMA_OWNER_DEVICE)
    return XHCI_INTERRUPT_CONFIG_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) ||
      cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_INTERRUPT_CONFIG_DMA_FAILED;

  // Do not retry after a partially completed operation.
  hub_interrupt_attempted = true;

  if (!xhci_bulk_ring_init(&device->interrupt_ring))
    return XHCI_INTERRUPT_CONFIG_ALLOCATION_FAILED;

  if (!dma_alloc(mps, 64U, &device->interrupt_buffer))
    return XHCI_INTERRUPT_CONFIG_ALLOCATION_FAILED;

  uint32_t size = device->context_size;
  uint8_t *input = device->input.virtual;
  uint8_t *output = device->output.virtual;
  uint32_t *control = (uint32_t *)input;
  uint32_t *slot = (uint32_t *)(input + size);
  uint32_t *endpoint = (uint32_t *)(input + (dci + 1U) * size);

  // Include the Slot Context and the new endpoint.
  control[0] = 0U;
  control[1] = 1U | (1U << dci);
  // Context Entries identifies the highest valid DCI.
  slot[0] = (slot[0] & ~(0x1fU << 27)) | ((uint32_t)dci << 27);

  // High-Speed bInterval is an exponent.
  // xHCI stores the exponent minus one.
  uint8_t xhci_interval = interval - 1U;
  uint64_t dequeue = device->interrupt_ring.bus | 1ULL;

  // Endpoint Context DW0: polling interval.
  endpoint[0] = (uint32_t)xhci_interval << 16;

  // Endpoint Context DW1: interrupt-IN, CErr 3,
  // no bursting, maximum packet size from descriptor.
  endpoint[1] = ((uint32_t)mps << 16) |
      (XHCI_EP_TYPE_INTERRUPT_IN << 3) | (3U << 1);

  // Endpoint Context DW2/DW3: transfer ring and DCS.
  endpoint[2] = (uint32_t)dequeue;
  endpoint[3] = (uint32_t)(dequeue >> 32);

  // Max ESIT Payload = one maximum-sized packet.
  // Average TRB Length = one maximum-sized packet.
  endpoint[4] = ((uint32_t)mps << 16) | mps;

  info->slot_id = device->slot_id;
  info->endpoint_id = dci;
  info->interval = xhci_interval;
  info->ring = device->interrupt_ring.bus;

  // Publish the transfer ring before configuring
  // the endpoint that will reference it.
  if (!dma_begin_device( &device->interrupt_ring, DMA_BIDIRECTIONAL))
    return XHCI_INTERRUPT_CONFIG_DMA_FAILED;

  // Publish the Input Context, including the new
  // endpoint entry and updated Slot Context.
  if (!cache_clean(input, (dci + 2U) * size))
    return XHCI_INTERRUPT_CONFIG_DMA_FAILED;

  __asm__ volatile("dsb sy" ::: "memory");

  xhci_trb_t command = {
    .parameter = device->input.bus,
    .status = 0U,
    .control = (XHCI_TRB_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)device->slot_id << 24)
  };

  xhci_command_completion_t completion = {0};
  xhci_command_result_t result = xhci_command_submit(&command, &completion);

  info->command_result = result;
  info->completion_code = completion.completion_code;

  if (result != XHCI_COMMAND_OK || completion.slot_id != device->slot_id)
    return XHCI_INTERRUPT_CONFIG_COMMAND_FAILED;

  // Inspect the controller-owned Output Context.
  if (!cache_invalidate(output, (dci + 1U) * size))
    return XHCI_INTERRUPT_CONFIG_DMA_FAILED;

  uint32_t *output_slot = (uint32_t *)output;
  uint32_t *output_endpoint = (uint32_t *)(output + dci * size);
  if (((output_slot[0] >> 27) & 0x1fU) != dci ||
      (output_endpoint[0] & 0x07U) != 1U ||
      ((output_endpoint[0] >> 16) & 0xffU) != xhci_interval ||
      ((output_endpoint[1] >> 16) & 0xffffU) != mps ||
      ((output_endpoint[1] >> 3) & 0x07U) != XHCI_EP_TYPE_INTERRUPT_IN)
    return XHCI_INTERRUPT_CONFIG_VERIFY_FAILED;

  device->interrupt_dci = dci;
  device->interrupt_interval = xhci_interval;
  device->interrupt_mps = mps;
  device->interrupt_ready = true;
  device->interrupt_index = 0U;
  device->interrupt_cycle = 1U;
  device->interrupt_wrap_pending = false;
  return XHCI_INTERRUPT_CONFIG_OK;
}

bool xhci_hub_notify_get_info(xhci_hub_notify_info_t *info) {
  if (!info)
    return false;

  *info = hub_notify_info;
  return hub_notify_started;
}

static bool xhci_hub_notify_publish(
    unsigned int index, size_t line) {

  xhci_trb_t *ring = internal_hub.interrupt_ring.virtual;
  uintptr_t address = (uintptr_t)&ring[index];
  address &= ~(uintptr_t)(line - 1U);

  return cache_clean((const void *)address, line);
}

static bool xhci_hub_notify_advance(size_t line) {
  xhci_device_t *device = &internal_hub;
  xhci_trb_t *ring = device->interrupt_ring.virtual;

  // The first completed TRB in a new cycle proves
  // that the controller traversed the old Link TRB.
  if (device->interrupt_wrap_pending) {
    xhci_trb_t *link = &ring[XHCI_RING_TRBS - 1U];
    link->control = (link->control & ~XHCI_TRB_CYCLE) |
        (device->interrupt_cycle ? XHCI_TRB_CYCLE : 0U);

    if (!xhci_hub_notify_publish(
        XHCI_RING_TRBS - 1U, line))
      return false;

    device->interrupt_wrap_pending = false;
  }

  device->interrupt_index++;
  if (device->interrupt_index == XHCI_RING_TRBS - 1U) {
    device->interrupt_index = 0U;
    device->interrupt_cycle ^= 1U;
    device->interrupt_wrap_pending = true;
    hub_notify_info.ring_wraps++;
  }

  return true;
}

static xhci_hub_notify_result_t xhci_hub_notify_arm(void) {
  xhci_device_t *device = &internal_hub;
  if (!device->interrupt_ready ||
      !controller_running || !controller_verified ||
      dispatch_fault != XHCI_DISPATCH_NONE ||
      device->interrupt_ring.owner != DMA_OWNER_DEVICE)
    return XHCI_HUB_NOTIFY_NOT_READY;

  cache_info_t cache;
  if (!cache_get_info(&cache) ||
      cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_HUB_NOTIFY_NOT_READY;

  // Verify controller access before submitting a transfer.
  xhci_info_t controller;
  if (!xhci_probe(&controller))
    return XHCI_HUB_NOTIFY_NOT_READY;

  dma_buffer_t *buffer = &device->interrupt_buffer;
  if (!dma_begin_device(buffer, DMA_FROM_DEVICE))
    return XHCI_HUB_NOTIFY_DMA_FAILED;

  // Submit one TD at the current producer position.
  unsigned int index = device->interrupt_index;
  uint32_t cycle = device->interrupt_cycle ? XHCI_TRB_CYCLE : 0U;

  xhci_trb_t *ring = device->interrupt_ring.virtual;
  xhci_trb_t *trb = &ring[index];
  trb->parameter = buffer->bus;
  trb->status = device->interrupt_mps;
  trb->control = (1U << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | cycle;
  if (!xhci_hub_notify_publish(index, cache.data_line_size)) {
    dma_end_device(buffer);
    return XHCI_HUB_NOTIFY_RING_FAILED;
  }

  uint64_t trb_address = device->interrupt_ring.bus +
      (uint64_t)index * sizeof(xhci_trb_t);
  hub_notify_waiter = xhci_waiter_register(XHCI_TRB_TRANSFER_EVENT,
      device->slot_id, device->interrupt_dci, trb_address);
  if (!hub_notify_waiter) {
    dma_end_device(buffer);
    return XHCI_HUB_NOTIFY_WAITER_FAILED;
  }

  hub_notify_info.state = XHCI_HUB_NOTIFY_PENDING;
  hub_notify_info.slot_id = device->slot_id;
  hub_notify_info.endpoint_id = device->interrupt_dci;
  hub_notify_info.transfer_trb = trb_address;
  hub_notify_started = true;

  __asm__ volatile("dsb sy" ::: "memory");

  xhci_write(controller.doorbell_offset +
      4U * device->slot_id, device->interrupt_dci);

  return XHCI_HUB_NOTIFY_OK;
}

xhci_hub_notify_result_t xhci_hub_notify_start(void) {
  if (hub_notify_started)
    return XHCI_HUB_NOTIFY_ALREADY_STARTED;

  return xhci_hub_notify_arm();
}

void xhci_hub_notify_service(void) {
  if (!hub_notify_started || hub_notify_info.state != XHCI_HUB_NOTIFY_PENDING)
    return;

  if (dispatch_fault != XHCI_DISPATCH_NONE) {
    hub_notify_info.state = XHCI_HUB_NOTIFY_FAILED;
    return;
  }

  if (!hub_notify_waiter || !hub_notify_waiter->completed)
    return;

  xhci_event_t event = hub_notify_waiter->event;
  xhci_waiter_release(hub_notify_waiter);
  hub_notify_waiter = NULL;
  hub_notify_info.completion_code = event.completion_code;
  uint32_t residual = event.status & 0x00ffffffU;
  uint32_t requested = internal_hub.interrupt_mps;
  if (event.completion_code != XHCI_COMPLETION_SUCCESS &&
      event.completion_code != XHCI_COMPLETION_SHORT_PACKET) {
    hub_notify_info.state = XHCI_HUB_NOTIFY_FAILED;
    return;
  }

  if (residual > requested ||
      (event.completion_code == XHCI_COMPLETION_SUCCESS && residual != 0U)) {
    hub_notify_info.state = XHCI_HUB_NOTIFY_FAILED;
    return;
  }

  dma_buffer_t *buffer = &internal_hub.interrupt_buffer;
  if (!dma_end_device(buffer)) {
    hub_notify_info.state = XHCI_HUB_NOTIFY_FAILED;
    return;
  }

  uint32_t received = requested - residual;
  if (received == 0U || received > 2U) {
    hub_notify_info.state = XHCI_HUB_NOTIFY_FAILED;
    return;
  }

  const uint8_t *data = buffer->virtual;
  uint16_t changes = data[0];
  if (received == 2U)
    changes |= (uint16_t)data[1] << 8;

  hub_notify_info.changes = changes;
  hub_notify_info.length = (uint8_t)received;
  hub_notify_info.notifications++;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)) ||
      !xhci_hub_notify_advance(cache.data_line_size)) {
    hub_notify_info.state = XHCI_HUB_NOTIFY_FAILED;
    return;
  }

  // Retain the completed notification until the
  // corresponding hub status has been processed.
  hub_notify_info.state = XHCI_HUB_NOTIFY_COMPLETE;
}

xhci_hub_notify_result_t xhci_hub_notify_resume(void) {
  if (!hub_notify_started ||
      hub_notify_info.state != XHCI_HUB_NOTIFY_COMPLETE)
    return XHCI_HUB_NOTIFY_NOT_READY;

  return xhci_hub_notify_arm();
}

void xhci_downstream_quiesce(void) {
  devices[1].lifecycle.quiesced = true;
}

bool xhci_downstream_is_quiesced(void) {
  return devices[1].lifecycle.quiesced;
}

bool xhci_downstream_is_slot_disabled(void) {
  return devices[1].lifecycle.slot_disabled;
}

static bool xhci_downstream_ep0_state(uint8_t *state) {
  if (!state)
    return false;

  xhci_device_t *device = &devices[1];

  if (!device->addressed || device->slot_id == 0U ||
      device->lifecycle.slot_disabled || device->output.owner != DMA_OWNER_DEVICE ||
      (device->context_size != 32U && device->context_size != 64U))
    return false;

  uint32_t size = device->context_size;
  uint8_t *output = device->output.virtual;
  if (!output)
    return false;

  // The Output Device Context is controller-owned.
  // Read the Slot Context and EP0 Context only.
  if (!cache_invalidate(output, 2U * size))
    return false;

  uint32_t *ep0 = (uint32_t *)(output + size);
  *state = (uint8_t)(ep0[0] & 0x07U);
  return true;
}

bool xhci_downstream_ep0_fault_reclaimable(void) {
  xhci_device_t *device = &devices[1];

  if (device->slot_id == 0U || !device->addressed ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed ||
      dispatch_fault != XHCI_DISPATCH_NONE || device->lifecycle.slot_disabled ||
      device->ep0_active || !device->ep0_failed ||
      !device->ep0_submitted || !device->ep0_completion_seen ||
      device->ep0_completion_code == 0U ||
      device->ep0_completion_code == XHCI_COMPLETION_SUCCESS)
    return false;

  // No completion owner may remain on EP0.
  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];

    if (waiter->active && waiter->slot_id == device->slot_id &&
        waiter->endpoint_id == 1U)
      return false;
  }

  uint8_t state = 0U;

  if (!xhci_downstream_ep0_state(&state))
    return false;

  // Halted and Error endpoints are not executing
  // transfers. Their slot may be discarded without
  // restarting the failed endpoint.
  return state == XHCI_EP_STATE_HALTED || state == XHCI_EP_STATE_ERROR;
}

static bool xhci_downstream_endpoint_states(
    uint8_t *in_state, uint8_t *out_state) {

  if (!in_state || !out_state)
    return false;

  xhci_device_t *device = &devices[1];
  if (!device->addressed || device->slot_id == 0U ||
      device->lifecycle.slot_disabled || device->output.owner != DMA_OWNER_DEVICE ||
      (device->context_size != 32U && device->context_size != 64U))
    return false;

  // The Output Device Context belongs to the controller.
  // Invalidate its cache lines before inspecting it.
  uint32_t size = device->context_size;
  uint8_t *output = device->output.virtual;

  if (!output)
    return false;

  if (!cache_invalidate(output, (XHCI_BULK_OUT_DCI + 1U) * size))
    return false;

  uint32_t *in = (uint32_t *)(output + XHCI_BULK_IN_DCI * size);
  uint32_t *out = (uint32_t *)(output + XHCI_BULK_OUT_DCI * size);
  *in_state = (uint8_t)(in[0] & 0x07U);
  *out_state = (uint8_t)(out[0] & 0x07U);

  return true;
}

bool xhci_downstream_transfer_state(xhci_downstream_transfer_state_t *state) {
  if (!state)
    return false;

  *state = (xhci_downstream_transfer_state_t){0};
  xhci_device_t *device = &devices[1];
  state->slot_id = device->slot_id;
  state->quiesced = device->lifecycle.quiesced;
  state->ep0_active = device->ep0_active;
  state->ep0_failed = device->ep0_failed;
  state->ep0_submitted = device->ep0_submitted;
  state->ep0_completion_seen = device->ep0_completion_seen;
  state->ep0_completion_code = device->ep0_completion_code;
  state->ep0_state_valid = xhci_downstream_ep0_state(&state->ep0_state);
  state->bulk_out_active = device->bulk_out_active;
  state->bulk_in_started = device->receiver.started;
  state->bulk_in_pending = device->receiver.state == XHCI_BULK_IN_PENDING;
  state->bulk_in_completed = device->receiver.waiter && device->receiver.waiter->completed;
  state->bulk_in_failed = device->receiver.state == XHCI_BULK_IN_FAILED;
  state->bulk_in_completion_code = device->receiver.info.completion_code;
  state->endpoint_states_valid = xhci_downstream_endpoint_states(
          &state->bulk_in_ep_state, &state->bulk_out_ep_state);

  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    const xhci_event_waiter_t *waiter = &event_waiters[i];
    if (waiter->active && waiter->slot_id == device->slot_id)
      state->active_waiters++;
  }

  return true;
}

static bool xhci_release_buffer(
    dma_buffer_t *buffer, xhci_cleanup_info_t *info) {

  if (!buffer->pages)
    return true;

  // Disable Slot has completed.
  // The controller no longer uses
  // this device's allocations.
  if (buffer->owner == DMA_OWNER_DEVICE && !dma_end_device(buffer))
    return false;

  unsigned int pages = buffer->pages;

  if (!dma_free(buffer))
    return false;

  info->buffers_released++;
  info->pages_released += pages;

  return true;
}

bool xhci_downstream_is_cleaned(void) {
  return devices[1].lifecycle.cleaned;
}

xhci_cleanup_result_t xhci_downstream_cleanup(xhci_cleanup_info_t *info) {
  if (!info)
    return XHCI_CLEANUP_NOT_READY;

  *info = (xhci_cleanup_info_t){0};

  if (devices[1].lifecycle.cleaned)
    return XHCI_CLEANUP_ALREADY_DONE;

  xhci_device_t *device = &devices[1];
  if (!devices[1].lifecycle.slot_disabled || !devices[1].lifecycle.quiesced ||
      device->slot_id == 0U || !controller_running ||
      !controller_verified || command_outstanding ||
      command_ring_failed || device->ep0_active || device->bulk_out_active)
    return XHCI_CLEANUP_NOT_READY;

  info->slot_id = device->slot_id;

  // A continuous receiver must be retired
  // before its buffers can be reclaimed.
  if (device->receiver.started || device->receiver.waiter ||
      device->receiver.state == XHCI_BULK_IN_PENDING)
    return XHCI_CLEANUP_BUSY;

  // Never destroy the state belonging
  // to an unresolved transfer.
  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];
    if (waiter->active && waiter->slot_id == device->slot_id)
      return XHCI_CLEANUP_BUSY;
  }

  // Keep the DMA descriptors intact
  // until each allocation is released.
  if (!xhci_release_buffer(&device->bulk_in_buffer, info))
    return XHCI_CLEANUP_DMA_FAILED;

  if (!xhci_release_buffer(&device->bulk_out_buffer, info))
    return XHCI_CLEANUP_DMA_FAILED;

  if (!xhci_release_buffer(&device->control_buffer, info))
    return XHCI_CLEANUP_DMA_FAILED;

  if (!xhci_release_buffer(&device->bulk_in_ring, info))
    return XHCI_CLEANUP_DMA_FAILED;

  if (!xhci_release_buffer(&device->bulk_out_ring, info))
    return XHCI_CLEANUP_DMA_FAILED;

  if (!xhci_release_buffer(&device->ep0_ring, info))
    return XHCI_CLEANUP_DMA_FAILED;

  if (!xhci_release_buffer(&device->input, info))
    return XHCI_CLEANUP_DMA_FAILED;

  if (!xhci_release_buffer(&device->output, info))
    return XHCI_CLEANUP_DMA_FAILED;

  // Leave the old slot identity intact
  // until software teardown completes.
  device->bulk_in_ready = false;
  device->bulk_out_ready = false;
  device->prepared = false;
  device->addressed = false;

  device->lifecycle.cleaned = true;
  return XHCI_CLEANUP_OK;
}

xhci_stop_result_t xhci_downstream_stop_bulk_out( xhci_stop_info_t *info) {
  if (!info)
    return XHCI_STOP_NOT_READY;

  *info = (xhci_stop_info_t){
    .command_result = XHCI_COMMAND_UNAVAILABLE
  };

  if (devices[1].lifecycle.stop_out_attempted)
    return XHCI_STOP_ALREADY_ATTEMPTED;

  xhci_device_t *device = &devices[1];
  if (!devices[1].lifecycle.quiesced || devices[1].lifecycle.slot_disabled ||
      !device->addressed || !device->bulk_out_ready ||
      device->slot_id == 0U || !controller_running ||
      !controller_verified || command_outstanding ||
      command_ring_failed)
    return XHCI_STOP_NOT_READY;

  // This first operation is restricted to an idle
  // Bulk OUT endpoint after device disconnection.
  if (device->bulk_out_active || device->ep0_active)
    return XHCI_STOP_BUSY;

  // No outstanding waiter may belong to Bulk OUT.
  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];
    if (waiter->active && waiter->slot_id == device->slot_id &&
        waiter->endpoint_id == XHCI_BULK_OUT_DCI)
      return XHCI_STOP_BUSY;
  }

  uint8_t in_state = 0U;
  uint8_t out_state = 0U;

  if (!xhci_downstream_endpoint_states(&in_state, &out_state))
    return XHCI_STOP_NOT_READY;

  info->slot_id = device->slot_id;
  info->endpoint_id = XHCI_BULK_OUT_DCI;
  info->before_state = out_state;

  // Stop Endpoint is meaningful here only when
  // the endpoint is currently Running.
  if (out_state != 1U)
    return XHCI_STOP_INVALID_STATE;

  xhci_trb_t command = {
    .parameter = 0U,
    .status = 0U,
    .control = (XHCI_TRB_STOP_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)XHCI_BULK_OUT_DCI << 16) |
        ((uint32_t)device->slot_id << 24)
  };

  devices[1].lifecycle.stop_out_attempted = true;
  stop_out_event = (xhci_stop_event_state_t){
    .armed = true,
    .slot_id = device->slot_id,
    .endpoint_id = XHCI_BULK_OUT_DCI
  };

  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);

  info->command_result = status;
  info->completion_code = completion.completion_code;
  stop_out_event.armed = false;
  info->stop_event_received = stop_out_event.received;
  info->stop_event_code = stop_out_event.completion_code;
  info->stop_event_trb = stop_out_event.trb_address;

  if (status != XHCI_COMMAND_OK || completion.slot_id != device->slot_id)
    return XHCI_STOP_COMMAND_FAILED;

  // A successful Stop Endpoint command must have
  // generated its stopped Transfer Event.
  if (!stop_out_event.received)
    return XHCI_STOP_VERIFY_FAILED;

  if (!xhci_downstream_endpoint_states(&in_state, &out_state))
    return XHCI_STOP_VERIFY_FAILED;

  info->after_state = out_state;
  if (out_state != 3U)
    return XHCI_STOP_VERIFY_FAILED;

  return XHCI_STOP_OK;
}

xhci_stop_result_t xhci_downstream_stop_bulk_in(xhci_stop_info_t *info) {

  if (!info)
    return XHCI_STOP_NOT_READY;

  *info = (xhci_stop_info_t){
    .command_result = XHCI_COMMAND_UNAVAILABLE
  };

  if (devices[1].lifecycle.stop_in_attempted)
    return XHCI_STOP_ALREADY_ATTEMPTED;

  xhci_device_t *device = &devices[1];
  if (!devices[1].lifecycle.quiesced || devices[1].lifecycle.slot_disabled ||
      !device->addressed || !device->bulk_in_ready ||
      !device->receiver.started || device->slot_id == 0U ||
      !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed ||
      dispatch_fault != XHCI_DISPATCH_NONE)
    return XHCI_STOP_NOT_READY;

  // A completed waiter should be reconciled,
  // not cancelled.
  if (device->receiver.state != XHCI_BULK_IN_PENDING ||
      !device->receiver.waiter || device->receiver.waiter->completed ||
      device->ep0_active || device->bulk_out_active)
    return XHCI_STOP_BUSY;

  // Other outstanding operations must not
  // interfere with this cancellation.
  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];
    if (!waiter->active || waiter == device->receiver.waiter)
      continue;
    if (waiter->slot_id == device->slot_id)
      return XHCI_STOP_BUSY;
  }

  uint8_t in_state = 0U;
  uint8_t out_state = 0U;

  if (!xhci_downstream_endpoint_states(&in_state, &out_state))
    return XHCI_STOP_NOT_READY;

  info->slot_id = device->slot_id;
  info->endpoint_id = XHCI_BULK_IN_DCI;
  info->before_state = in_state;

  // Only a Running endpoint requires stopping.
  if (in_state != 1U)
    return XHCI_STOP_INVALID_STATE;

  xhci_trb_t command = {
    .parameter = 0U,
    .status = 0U,
    .control = (XHCI_TRB_STOP_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)XHCI_BULK_IN_DCI << 16) |
        ((uint32_t)device->slot_id << 24)
  };

  device->lifecycle.stop_in_attempted = true;

  stop_in_event = (xhci_stop_event_state_t){
    .armed = true,
    .slot_id = device->slot_id,
    .endpoint_id = XHCI_BULK_IN_DCI
  };

  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);

  stop_in_event.armed = false;

  info->command_result = status;
  info->completion_code = completion.completion_code;
  info->stop_event_received = stop_in_event.received;
  info->stop_event_code = stop_in_event.completion_code;
  info->stop_event_trb = stop_in_event.trb_address;

  if (status != XHCI_COMMAND_OK || completion.slot_id != device->slot_id)
    return XHCI_STOP_COMMAND_FAILED;

  if (!stop_in_event.received)
    return XHCI_STOP_VERIFY_FAILED;

  if (!xhci_downstream_endpoint_states(&in_state, &out_state))
    return XHCI_STOP_VERIFY_FAILED;

  info->after_state = in_state;
  if (in_state != 3U)
    return XHCI_STOP_VERIFY_FAILED;

  // The controller has stopped executing this
  // endpoint. Reconcile its software owner.
  if (device->receiver.waiter && device->receiver.waiter->completed) {
    xhci_in_reconcile_result_t result = xhci_bulk_in_quiesced_reconcile_device(device);
    if (result != XHCI_IN_RECONCILE_OK)
      return XHCI_STOP_VERIFY_FAILED;
  } else if (device->receiver.waiter && device->receiver.state == XHCI_BULK_IN_PENDING) {
    // The stopped event described the dequeue
    // position, not the pending receive TD.
    // Stop Endpoint completed successfully,
    // so this TD can no longer execute.
    xhci_waiter_release(device->receiver.waiter);
    device->receiver.waiter = NULL;
    device->receiver.info.completion_code = stop_in_event.completion_code;
    device->receiver.info.event_parameter = stop_in_event.trb_address;
    device->receiver.state = XHCI_BULK_IN_FAILED;
    device->receiver.failed = true;
  } else {
    return XHCI_STOP_VERIFY_FAILED;
  }

  return XHCI_STOP_OK;
}

xhci_disable_result_t xhci_downstream_disable(xhci_disable_info_t *info) {
  if (!info)
    return XHCI_DISABLE_NOT_READY;

  *info = (xhci_disable_info_t){
    .command_result = XHCI_COMMAND_UNAVAILABLE
  };

  if (devices[1].lifecycle.disable_attempted)
    return XHCI_DISABLE_ALREADY_ATTEMPTED;

  xhci_device_t *device = &devices[1];
  if (!devices[1].lifecycle.quiesced || devices[1].lifecycle.slot_disabled ||
      device->slot_id == 0U || !controller_running ||
      !controller_verified || command_outstanding ||
      command_ring_failed || device->ep0_active ||
      device->bulk_out_active)
    return XHCI_DISABLE_NOT_READY;

  // A failed EP0 must have a confirmed terminal
  // outcome before its slot can be disabled.
  if (device->ep0_failed && !xhci_downstream_ep0_fault_reclaimable())
    return XHCI_DISABLE_NOT_READY;

  // Outstanding completions must retain their owners.
  // Do not disable the slot while a waiter is active.
  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];

    if (waiter->active && waiter->slot_id == device->slot_id)
      return XHCI_DISABLE_BUSY;
  }

  xhci_trb_t command = {
    .parameter = 0U,
    .status = 0U,
    .control = (XHCI_TRB_DISABLE_SLOT << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)device->slot_id << 24)
  };

  info->slot_id = device->slot_id;

  // Set before submission. A timeout leaves the controller's
  // state uncertain, so we must not blindly retry.
  devices[1].lifecycle.disable_attempted = true;
  xhci_command_completion_t completion = {0};
  xhci_command_result_t status = xhci_command_submit(&command, &completion);
  info->command_result = status;
  info->completion_code = completion.completion_code;

  if (status != XHCI_COMMAND_OK || completion.slot_id != device->slot_id)
    return XHCI_DISABLE_COMMAND_FAILED;

  device->lifecycle.slot_disabled = true;
  return XHCI_DISABLE_OK;
}

xhci_recycle_result_t xhci_downstream_recycle(void) {
  xhci_device_t *device = &devices[1];

  if (!device->lifecycle.slot_disabled || !device->lifecycle.cleaned ||
      !device->lifecycle.quiesced || !controller_running || !controller_verified ||
      command_outstanding || command_ring_failed || device->slot_id == 0U)
    return XHCI_RECYCLE_NOT_READY;

  if (device->receiver.started || device->receiver.waiter || device->ep0_active ||
      device->bulk_out_active)
    return XHCI_RECYCLE_BUSY;

  // No completion owner may remain for this slot.
  for (unsigned int i = 0U; i < XHCI_EVENT_WAITERS; i++) {
    xhci_event_waiter_t *waiter = &event_waiters[i];
    if (waiter->active && waiter->slot_id == device->slot_id)
      return XHCI_RECYCLE_BUSY;
  }

  // Cleanup must have released every allocation.
  if (device->input.pages || device->output.pages ||
      device->ep0_ring.pages || device->control_buffer.pages ||
      device->bulk_in_ring.pages || device->bulk_out_ring.pages ||
      device->bulk_in_buffer.pages || device->bulk_out_buffer.pages)
    return XHCI_RECYCLE_BUSY;

  dma_buffer_t *dcbaa_buffer = &controller_dma.dcbaa;
  if (dcbaa_buffer->owner != DMA_OWNER_DEVICE)
    return XHCI_RECYCLE_DMA_FAILED;

  cache_info_t cache;
  if (!cache_get_info(&cache) || cache.data_line_size == 0U ||
      cache.data_line_size > PAGE_SIZE ||
      (cache.data_line_size & (cache.data_line_size - 1U)))
    return XHCI_RECYCLE_DMA_FAILED;

  // Disable Slot has succeeded. Remove the
  // obsolete Output Device Context pointer.
  uint64_t *dcbaa = dcbaa_buffer->virtual;
  uint8_t slot = device->slot_id;
  dcbaa[slot] = 0ULL;
  uintptr_t address = (uintptr_t)&dcbaa[slot];
  address &= ~(uintptr_t)(cache.data_line_size - 1U);
  if (!cache_clean((const void *)address, cache.data_line_size))
    return XHCI_RECYCLE_DMA_FAILED;

  __asm__ volatile("dsb sy" ::: "memory");

  // All downstream allocations have been freed.
  // Discard the old software device context.
  *device = (xhci_device_t){0};

  stop_out_event = (xhci_stop_event_state_t){0};
  stop_in_event = (xhci_stop_event_state_t){0};

  return XHCI_RECYCLE_OK;
}

