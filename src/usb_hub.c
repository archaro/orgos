#include <stdint.h>
#include <stdbool.h>

#include "timer.h"
#include "xhci.h"
#include "usb.h"
#include "usb_hub.h"

#define USB_HUB_PORTS 4U

#define USB_REQ_GET_STATUS 0x00U
#define USB_RT_HUB_PORT_IN 0xA3U
#define USB_REQ_SET_FEATURE 0x03U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_RT_HUB_PORT_OUT 0x23U

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
  bool power_requested = false;
  for (uint8_t port = 1U; port <= USB_HUB_PORTS; port++) {
    usb_hub_port_t current;
    usb_hub_result_t port_result = usb_hub_port_status(port, &current);

    if (port_result != USB_HUB_OK) {
      *result = current;
      return port_result;
    }

    // Do not repeat a request on an already-powered port.
    if (current.status & (1U << 8))
      continue;

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
    power_requested = true;
  }

  // The hub descriptor specifies a 100 ms power-good
  // interval. Allow 120 ms after the last power request.
  if (power_requested)
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

