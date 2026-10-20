#include <stdint.h>
#include <stdbool.h>

#include "usb.h"
#include "usb_hub.h"
#include "xhci.h"

#define USB_HUB_PORTS 4U

#define USB_REQ_GET_STATUS 0x00U
#define USB_RT_HUB_PORT_IN 0xA3U

usb_hub_result_t usb_hub_port_status(
    uint8_t port, usb_hub_port_t *result) {

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
