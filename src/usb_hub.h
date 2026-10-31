#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xhci.h"

typedef enum {
  USB_HUB_OK,
  USB_HUB_INVALID_PORT,
  USB_HUB_NOT_READY,
  USB_HUB_TRANSFER_FAILED,
  USB_HUB_INVALID_RESPONSE,
  USB_HUB_DISCONNECTED,
  USB_HUB_NOT_POWERED,
  USB_HUB_RESET_TIMEOUT,
  USB_HUB_RESET_FAILED,
  USB_HUB_DEBOUNCE_TIMEOUT
} usb_hub_result_t;

typedef struct {
  uint8_t port;
  uint16_t status;
  uint16_t change;
  uint8_t completion_code;
  xhci_control_result_t transfer_result;
} usb_hub_port_t;

usb_hub_result_t usb_hub_port_status(uint8_t port, usb_hub_port_t *result);
usb_hub_result_t usb_hub_port_debounce(uint8_t port, usb_hub_port_t *result);

typedef struct {
  uint8_t port;
  uint8_t speed;
  uint16_t before_status;
  uint16_t before_change;
  uint16_t after_status;
  uint16_t after_change;
  uint16_t final_change;
  uint32_t elapsed_ms;
  uint8_t completion_code;
  xhci_control_result_t transfer_result;
} usb_hub_reset_info_t;

usb_hub_result_t usb_hub_port_reset(
    uint8_t port, usb_hub_reset_info_t *result);

usb_hub_result_t usb_hub_power_on(
    uint32_t power_good_ms, usb_hub_port_t *result);
