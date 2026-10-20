#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xhci.h"

typedef enum {
  USB_HUB_OK,
  USB_HUB_INVALID_PORT,
  USB_HUB_NOT_READY,
  USB_HUB_TRANSFER_FAILED,
  USB_HUB_INVALID_RESPONSE
} usb_hub_result_t;

typedef struct {
  uint8_t port;
  uint16_t status;
  uint16_t change;
  uint8_t completion_code;
  xhci_control_result_t transfer_result;
} usb_hub_port_t;

usb_hub_result_t usb_hub_port_status(uint8_t port, usb_hub_port_t *result);
usb_hub_result_t usb_hub_power_on(usb_hub_port_t *result);
