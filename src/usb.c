#include <stdbool.h>
#include <stdint.h>

#include "xhci.h"
#include "usb.h"
#include "usb_midi_test.h"

#define USB_ROOT_PORT 1U
#define USB_HIGH_SPEED 3U
#define USB_LINK_U0 0U
#define USB_ROOT_EVENT_WAIT_MS 100U

static bool usb_initialised;
static usb_init_info_t usb_state;
static bool usb_attempted;
static bool usb_service_failed;

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

bool usb_service_available(void) {
  return usb_initialised && !usb_service_failed;
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
