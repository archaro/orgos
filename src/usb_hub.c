#include <stdint.h>
#include <stdbool.h>

#include "timer.h"
#include "xhci.h"
#include "usb.h"
#include "usb_hub.h"

#define USB_HUB_PORTS 4U

#define USB_RT_HUB_PORT_IN 0xA3U
#define USB_RT_HUB_PORT_OUT 0x23U

#define USB_REQ_GET_STATUS 0x00U
#define USB_REQ_SET_FEATURE 0x03U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_HUB_REQ_CLEAR_FEATURE 0x01U
#define USB_HUB_REQ_SET_FEATURE   0x03U
#define USB_HUB_PORT_REQUEST_OUT  0x23U

#define USB_HUB_C_CONNECTION       (1U << 0)
#define USB_HUB_FEATURE_RESET       4U
#define USB_HUB_CHANGE_CONNECTION  16U
#define USB_HUB_CHANGE_RESET       20U

#define USB_HUB_STATUS_CONNECTION (1U << 0)
#define USB_HUB_STATUS_ENABLE     (1U << 1)
#define USB_HUB_STATUS_RESET      (1U << 4)
#define USB_HUB_STATUS_POWER      (1U << 8)

#define USB_HUB_DEBOUNCE_STEP_MS    25U
#define USB_HUB_DEBOUNCE_STABLE_US  100000ULL
#define USB_HUB_DEBOUNCE_TIMEOUT_US 2000000ULL

#define USB_HUB_RESET_TIMEOUT_MS 500U
#define USB_HUB_RESET_RECOVERY_MS 50U

#define USB_HUB_PORT_POWER 8U
#define USB_HUB_POWER_DELAY_MS 120U

usb_hub_result_t usb_hub_port_status(uint8_t port, usb_hub_port_t *result) {

  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_port_t){0};
  result->port = port;
  if (port == 0U || port > USB_HUB_PORTS)
    return USB_HUB_INVALID_PORT;

  usb_init_info_t state;
  if (!usb_get_init_info(&state) || state.result != USB_INIT_OK ||
      state.address == 0U)
    return USB_HUB_NOT_READY;

  // USB hub class GET_STATUS for a downstream port:
  // bmRequestType = A3 (IN, Class, Other)
  // bRequest      = 00 (GET_STATUS)
  // wValue        = 0000
  // wIndex        = port number
  // wLength       = 0004
  xhci_setup_packet_t setup = {
    .request_type = USB_RT_HUB_PORT_IN,
    .request = USB_REQ_GET_STATUS,
    .value = 0U,
    .index = port,
    .length = 4U
  };

  xhci_control_info_t transfer;
  xhci_control_result_t status = xhci_control_in( &setup, &transfer);
  result->transfer_result = status;
  result->completion_code = transfer.completion_code;
  if (status != XHCI_CONTROL_OK)
    return USB_HUB_TRANSFER_FAILED;

  if (transfer.length != 4U)
    return USB_HUB_INVALID_RESPONSE;

  const uint8_t *d = transfer.data;
  result->status = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
  result->change = (uint16_t)d[2] | ((uint16_t)d[3] << 8);

  return USB_HUB_OK;
}


usb_hub_result_t usb_hub_power_on(usb_hub_port_t *result) {

  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_port_t){0};
  result->port = 1U;
  usb_init_info_t state;
  if (!usb_get_init_info(&state) || state.result != USB_INIT_OK ||
      state.address == 0U)
    return USB_HUB_NOT_READY;

  // Verify that SET_CONFIGURATION(1) has succeeded.
  xhci_setup_packet_t setup = {
    .request_type = 0x80U,
    .request = USB_REQ_GET_CONFIGURATION,
    .value = 0U,
    .index = 0U,
    .length = 1U
  };

  xhci_control_info_t transfer;
  xhci_control_result_t status = xhci_control_in(&setup, &transfer);
  result->transfer_result = status;
  result->completion_code = transfer.completion_code;
  if (status != XHCI_CONTROL_OK)
    return USB_HUB_TRANSFER_FAILED;

  if (transfer.length != 1U || transfer.data[0] != 1U)
    return USB_HUB_NOT_READY;

  // Request PORT_POWER on every downstream port.
  // Even hubs advertising ganged switching may require
  // individual requests for correct power-status reporting.
  for (uint8_t port = 1U; port <= USB_HUB_PORTS; port++) {
    usb_hub_port_t current;
    usb_hub_result_t port_result = usb_hub_port_status(port, &current);

    if (port_result != USB_HUB_OK) {
      *result = current;
      return port_result;
    }

    setup.request_type = USB_RT_HUB_PORT_OUT;
    setup.request = USB_REQ_SET_FEATURE;
    setup.value = USB_HUB_PORT_POWER;
    setup.index = port;
    setup.length = 0U;
    status = xhci_control_no_data(&setup, &transfer);
    result->port = port;
    result->transfer_result = status;
    result->completion_code = transfer.completion_code;
    if (status != XHCI_CONTROL_OK)
      return USB_HUB_TRANSFER_FAILED;
  }

  timer_delay_ms(USB_HUB_POWER_DELAY_MS);

  // Verify that every downstream port reports power.
  for (uint8_t port = 1U; port <= USB_HUB_PORTS; port++) {
    usb_hub_port_t current;
    usb_hub_result_t port_result = usb_hub_port_status(port, &current);

    if (port_result != USB_HUB_OK) {
      *result = current;
      return port_result;
    }

    *result = current;
    if (!(current.status & (1U << 8)))
      return USB_HUB_INVALID_RESPONSE;
  }

  return USB_HUB_OK;
}

static usb_hub_result_t usb_hub_port_feature(
    uint8_t port, uint8_t request, uint16_t feature,
    xhci_control_info_t *result) {

  xhci_setup_packet_t setup = {
    .request_type = USB_HUB_PORT_REQUEST_OUT,
    .request = request,
    .value = feature,
    .index = port,
    .length = 0U
  };

  xhci_control_result_t status = xhci_control_no_data(&setup, result);

  if (status != XHCI_CONTROL_OK)
    return USB_HUB_TRANSFER_FAILED;

  return USB_HUB_OK;
}

usb_hub_result_t usb_hub_port_debounce(uint8_t port, usb_hub_port_t *result) {
  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_port_t){0};
  result->port = port;
  uint64_t started = timer_now_us();
  uint64_t stable_since = started;
  bool have_connection = false;
  bool was_connected = false;

  for (;;) {
    usb_hub_port_t current;
    usb_hub_result_t status = usb_hub_port_status(port, &current);
    *result = current;

    if (status != USB_HUB_OK)
      return status;

    if (!(current.status & USB_HUB_STATUS_POWER))
      return USB_HUB_NOT_POWERED;

    bool connected = (current.status & USB_HUB_STATUS_CONNECTION) != 0U;
    bool changed = (current.change & USB_HUB_C_CONNECTION) != 0U;
    // Acknowledge any connection change. Its presence
    // invalidates the current stability interval.
    if (changed) {
      xhci_control_info_t transfer = {0};
      status = usb_hub_port_feature(port, USB_HUB_REQ_CLEAR_FEATURE,
          USB_HUB_CHANGE_CONNECTION, &transfer);

      if (status != USB_HUB_OK) {
        result->transfer_result = transfer.completion_code == 1U ?
            XHCI_CONTROL_OK : XHCI_CONTROL_TRANSFER_FAILED;
        result->completion_code = transfer.completion_code;
        return status;
      }
    }

    uint64_t now = timer_now_us();
    // Require an uninterrupted 100 ms of stable,
    // connected status with no latched changes.
    if (!have_connection || changed || connected != was_connected) {
      stable_since = now;
    } else if (connected &&
        now - stable_since >= USB_HUB_DEBOUNCE_STABLE_US) {
      return USB_HUB_OK;
    }

    if (now - started >= USB_HUB_DEBOUNCE_TIMEOUT_US)
      return USB_HUB_DEBOUNCE_TIMEOUT;

    was_connected = connected;
    have_connection = true;
    timer_delay_ms(USB_HUB_DEBOUNCE_STEP_MS);
  }
}

usb_hub_result_t usb_hub_port_reset(
    uint8_t port, usb_hub_reset_info_t *result) {

  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_reset_info_t){0};
  result->port = port;

  if (port == 0U || port > USB_HUB_PORTS)
    return USB_HUB_INVALID_PORT;

  usb_hub_port_t before;
  usb_hub_result_t status =
      usb_hub_port_status(port, &before);

  if (status != USB_HUB_OK)
    return status;

  result->before_status = before.status;
  result->before_change = before.change;

  if (!(before.status & USB_HUB_STATUS_POWER))
    return USB_HUB_NOT_POWERED;

  if (!(before.status & USB_HUB_STATUS_CONNECTION))
    return USB_HUB_DISCONNECTED;

  // SET_FEATURE(PORT_RESET).
  // This resets a downstream hub port, not an xHCI root port.
  xhci_control_info_t transfer;
  status = usb_hub_port_feature(
      port, USB_HUB_REQ_SET_FEATURE,
      USB_HUB_FEATURE_RESET, &transfer);

  result->transfer_result = status == USB_HUB_OK ?
      XHCI_CONTROL_OK : XHCI_CONTROL_TRANSFER_FAILED;
  result->completion_code = transfer.completion_code;

  if (status != USB_HUB_OK)
    return status;

  uint64_t started = timer_now_us();

  for (unsigned int ms = 0; ms < USB_HUB_RESET_TIMEOUT_MS; ms++) {
    // Poll using GET_STATUS. A completed reset should
    // clear PORT_RESET and set C_PORT_RESET.
    timer_delay_ms(1);

    usb_hub_port_t current;
    status = usb_hub_port_status(port, &current);

    if (status != USB_HUB_OK)
      return status;

    result->after_status = current.status;
    result->after_change = current.change;
    result->elapsed_ms = (uint32_t)((timer_now_us() - started) / 1000ULL);

    if (!(current.status & USB_HUB_STATUS_CONNECTION))
      return USB_HUB_DISCONNECTED;

    if (current.status & USB_HUB_STATUS_RESET)
      continue;

    if (!(current.change & (1U << 4)))
      continue;

    if (!(current.status & USB_HUB_STATUS_ENABLE))
      return USB_HUB_RESET_FAILED;

    // Decode the speed after reset has completed.
    if (current.status & (1U << 10))
      result->speed = 3U;  // High Speed
    else if (current.status & (1U << 9))
      result->speed = 2U;  // Low Speed (xHCI speed ID)
    else
      result->speed = 1U;  // Full Speed

    // Acknowledge C_PORT_RESET.
    status = usb_hub_port_feature(port, USB_HUB_REQ_CLEAR_FEATURE,
        USB_HUB_CHANGE_RESET, &transfer);
    if (status != USB_HUB_OK)
      return status;

    // Acknowledge any connection change observed
    // during this reset sequence.
    if (current.change & (1U << 0)) {
      status = usb_hub_port_feature(
          port, USB_HUB_REQ_CLEAR_FEATURE,
          USB_HUB_CHANGE_CONNECTION, &transfer);
      if (status != USB_HUB_OK)
        return status;
    }

    // Verify the resulting port state and change flags.
    status = usb_hub_port_status(port, &current);
    if (status != USB_HUB_OK)
      return status;

    result->final_change = current.change;
    if (!(current.status & USB_HUB_STATUS_ENABLE) ||
        !(current.status & USB_HUB_STATUS_CONNECTION) ||
        (current.status & USB_HUB_STATUS_RESET))
      return USB_HUB_RESET_FAILED;
    if (current.change & ((1U << 0) | (1U << 4)))
      return USB_HUB_RESET_FAILED;

    // USB 2.0 reset recovery: allow at least 10 ms
    // for the device to settle, plus a 40 ms margin.
    // Match Linux's normal hub-port reset recovery.
    timer_delay_ms(USB_HUB_RESET_RECOVERY_MS);

    return USB_HUB_OK;
  }

  return USB_HUB_RESET_TIMEOUT;
}

