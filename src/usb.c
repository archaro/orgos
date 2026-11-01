#include <stdbool.h>
#include <stdint.h>

#include "page.h"
#include "timer.h"
#include "xhci.h"
#include "usb.h"
#include "usb_enumerate.h"

#define USB_ROOT_PORT 1U
#define USB_HIGH_SPEED 3U
#define USB_LINK_U0 0U
#define USB_ROOT_EVENT_WAIT_MS 100U

static bool usb_initialised;
static usb_init_info_t usb_state;
static bool usb_attempted;
static bool usb_service_failed;

#define USB_MIDI_IN_RING_ENTRIES 255U
static usb_midi_in_stress_info_t in_stress;
static uint64_t in_stress_started;
static uint64_t in_stress_ring_base;
static unsigned int in_stress_first_index;

static bool usb_port_valid(const xhci_port_t *port) {
  return port->connected && port->enabled && port->powered &&
      !port->overcurrent && !port->resetting &&
      port->speed == USB_HIGH_SPEED &&
      port->link_state == USB_LINK_U0;
}

static usb_init_result_t usb_root_port_prepare(usb_init_info_t *info) {
  xhci_port_clear_info_t clear;
  xhci_port_reset_info_t reset;
  xhci_port_t port;
  xhci_port_clear_result_t clear_result =
      xhci_port_clear(USB_ROOT_PORT, &clear);
  if (clear_result != XHCI_PORT_CLEAR_OK) {
    info->detail = (uint32_t)clear_result;
    return USB_INIT_PORT_CLEAR_FAILED;
  }

  if (!xhci_port_read(USB_ROOT_PORT, &port))
    return USB_INIT_PORT_READ_FAILED;

  // A previously enabled port does not require another reset.
  if (port.enabled) {
    if (!usb_port_valid(&port))
      return USB_INIT_PORT_STATE_FAILED;
    info->speed = port.speed;
    return USB_INIT_OK;
  }

  xhci_port_reset_result_t reset_result =
      xhci_port_reset(USB_ROOT_PORT, &reset);

  if (reset_result != XHCI_PORT_RESET_OK) {
    info->detail = (uint32_t)reset_result;
    return USB_INIT_PORT_RESET_FAILED;
  }

  if (!xhci_port_read(USB_ROOT_PORT, &port))
    return USB_INIT_PORT_READ_FAILED;

  if (!usb_port_valid(&port)) {
    info->detail = port.portsc;
    return USB_INIT_PORT_STATE_FAILED;
  }

  // Wait for the root port's change notification.
  // The xHCI dispatcher owns the hardware event ring.
  xhci_port_event_result_t event_result =
      xhci_port_event_wait(USB_ROOT_PORT, USB_ROOT_EVENT_WAIT_MS);

  if (event_result != XHCI_PORT_EVENT_OK) {
    info->detail = (uint32_t)event_result;

    if (event_result == XHCI_PORT_EVENT_TIMEOUT)
      return USB_INIT_EVENT_TIMEOUT;

    return USB_INIT_EVENT_FAILED;
  }

  info->speed = port.speed;
  return USB_INIT_OK;
}

bool usb_get_init_info(usb_init_info_t *info) {
  if (!info)
    return false;

  *info = usb_state;
  return true;
}

usb_init_result_t usb_init(usb_init_info_t *info) {
  if (!info)
    return USB_INIT_PORT_READ_FAILED;

  if (usb_attempted) {
    *info = usb_state;
    return usb_state.result;
  }

  usb_attempted = true;
  usb_init_info_t state = {0};
  state.root_port = USB_ROOT_PORT;

  usb_init_result_t result = usb_root_port_prepare(&state);
  if (result != USB_INIT_OK)
    goto complete;

  xhci_slot_info_t slot;
  xhci_slot_result_t slot_result = xhci_enable_slot(&slot);
  if (slot_result != XHCI_SLOT_OK) {
    state.detail = (uint32_t)slot_result;
    result = USB_INIT_SLOT_FAILED;
    goto complete;
  }

  state.slot_id = slot.slot_id;
  xhci_device_info_t device;
  xhci_device_result_t device_result = xhci_device_prepare(&device);
  if (device_result != XHCI_DEVICE_OK) {
    state.detail = (uint32_t)device_result;
    result = USB_INIT_CONTEXT_FAILED;
    goto complete;
  }

  xhci_address_info_t address;
  xhci_address_result_t address_result = xhci_address_device(&address);
  if (address_result != XHCI_ADDRESS_OK) {
    // Diagnostic: preserve the address result, xHCI
    // command result and hardware completion code.
    state.detail = (uint32_t)address_result |
        ((uint32_t)address.command_result << 8) |
        ((uint32_t)address.completion_code << 16);
    result = USB_INIT_ADDRESS_FAILED;
    goto complete;
  }
  state.address = address.usb_address;
  state.slot_state = address.slot_state;
  state.ep0_state = address.ep0_state;
  usb_initialised = true;

complete:
  state.result = result;
  usb_state = state;
  *info = state;
  return result;
}

static void usb_midi_in_stress_fail(usb_midi_in_stress_error_t error) {
  in_stress.error = error;
  in_stress.state = USB_MIDI_IN_STRESS_FAILED;
  in_stress.elapsed_us = timer_now_us() - in_stress_started;
}

bool usb_midi_in_stress_start(void) {
  if (!usb_initialised || usb_service_failed ||
      in_stress.state != USB_MIDI_IN_STRESS_IDLE)
    return false;

  usb_enum_info_t enumeration = {0};
  if (!usb_get_enumeration(&enumeration) || enumeration.result != USB_ENUM_OK)
    return false;

  xhci_bulk_in_stream_stats_t stream = {0};
  if (!xhci_bulk_in_stream_get_stats(&stream) || stream.started)
    return false;

  if (xhci_bulk_in_stream_start() != XHCI_BULK_IN_ARM_OK)
    return false;

  in_stress = (usb_midi_in_stress_info_t){
    .state = USB_MIDI_IN_STRESS_RUNNING
  };
  in_stress_started = timer_now_us();
  in_stress_ring_base = 0U;
  in_stress_first_index = 0U;

  return true;
}

static void usb_midi_in_stress_service(void) {
  if (in_stress.state != USB_MIDI_IN_STRESS_RUNNING)
    return;

  xhci_bulk_in_stream_stats_t stream = {0};
  if (!xhci_bulk_in_stream_get_stats(&stream) || stream.failed) {
    usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_STREAM_ERROR);
    return;
  }

  xhci_bulk_in_info_t info;
  while (xhci_bulk_in_stream_read(&info)) {
    in_stress.fault_transfer = in_stress.transfers + 1U;
    // Verify that each completion refers to the
    // transfer TRB which we actually submitted.
    if (info.event_parameter != info.transfer_trb ||
        (info.completion_code != 1U &&
         info.completion_code != 13U)) {
      usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_TRB_ERROR);
      return;
    }

    // The transfer ring occupies one aligned page,
    // with its final entry reserved for the Link TRB.
    if (in_stress.transfers == 0U) {
      in_stress_ring_base = info.transfer_trb & ~((uint64_t)PAGE_SIZE - 1ULL);
      in_stress_first_index = (unsigned int)
          ((info.transfer_trb - in_stress_ring_base) / sizeof(xhci_trb_t));
      if (in_stress_first_index >= USB_MIDI_IN_RING_ENTRIES) {
        usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_TRB_ERROR);
        return;
      }
    }

    uint32_t absolute_index = in_stress_first_index + in_stress.transfers;

    uint64_t expected = in_stress_ring_base +
        (uint64_t)(absolute_index % USB_MIDI_IN_RING_ENTRIES) *
        sizeof(xhci_trb_t);

    in_stress.expected_trb = expected;
    in_stress.actual_trb = info.transfer_trb;

    if (info.transfer_trb != expected) {
      usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_TRB_ERROR);
      return;
    }

    // The xHCI event reports bytes remaining.
    // A USB-MIDI transfer must contain complete
    // four-byte event packets.
    if (info.requested != XHCI_BULK_IN_MAX_DATA ||
        info.received == 0U ||
        info.received > XHCI_BULK_IN_MAX_DATA ||
        (info.received % 4U) != 0U ||
        info.residual != info.requested - info.received) {
      usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_LENGTH_ERROR);
      return;
    }

    // A return to ring entry zero proves that
    // the controller traversed the Link TRB.
    if (in_stress.transfers != 0U &&
        absolute_index % USB_MIDI_IN_RING_ENTRIES == 0U)
      in_stress.wraps++;

    in_stress.transfers++;
    in_stress.bytes += info.received;

    // The Windows sender emits channel 16 Control
    // Changes, controller 102, with values cycling
    // through 0..127. Check every event in order.
    for (uint32_t i = 0U; i < info.received; i += 4U) {
      in_stress.fault_packet = in_stress.packets + 1U;
      if (in_stress.packets >= USB_MIDI_IN_STRESS_PACKETS ||
          info.data[i] != 0x0bU ||
          info.data[i + 1U] != 0xbfU ||
          info.data[i + 2U] != 0x66U ||
          info.data[i + 3U] != (uint8_t)(in_stress.packets & 0x7fU)) {
        usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_PACKET_ERROR);
        return;
      }
      in_stress.packets++;
    }

    if (in_stress.packets == USB_MIDI_IN_STRESS_PACKETS) {
      in_stress.elapsed_us = timer_now_us() - in_stress_started;
      if (in_stress.transfers >=
              USB_MIDI_IN_STRESS_MIN_TRANSFERS && in_stress.wraps >= 2U) {
        in_stress.state = USB_MIDI_IN_STRESS_PASS;
      } else {
        in_stress.state = USB_MIDI_IN_STRESS_INCONCLUSIVE;
      }
      return;
    }
  }
}

bool usb_midi_in_stress_get_info(usb_midi_in_stress_info_t *result) {
  if (!result)
    return false;

  *result = in_stress;
  if (in_stress.state == USB_MIDI_IN_STRESS_RUNNING)
    result->elapsed_us = timer_now_us() - in_stress_started;

  return true;
}

void usb_service(void) {
  if (!usb_initialised || usb_service_failed)
    return;

  // Dispatch hardware events to their owners.
  xhci_service_result_t result = xhci_service(8U);

  // Finalise and rearm completed Bulk IN transfers.
  xhci_bulk_in_stream_service();

  // Drain the test receiver without blocking USB.
  usb_midi_in_stress_service();

  if (result == XHCI_SERVICE_FAILED || result == XHCI_SERVICE_NOT_READY)
    usb_service_failed = true;
}
