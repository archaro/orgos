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
  USB_HUB_DEBOUNCE_TIMEOUT,
  USB_HUB_OVERCURRENT
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

typedef enum {
  USB_HUB_INIT_OK,
  USB_HUB_INIT_NOT_READY,
  USB_HUB_INIT_CONFIG_FAILED,
  USB_HUB_INIT_DESCRIPTOR_FAILED,
  USB_HUB_INIT_CONTEXT_FAILED,
  USB_HUB_INIT_POWER_FAILED,
  USB_HUB_INIT_INTERRUPT_FAILED
} usb_hub_init_result_t;

typedef struct {
  usb_hub_init_result_t result;
  uint32_t detail;
  uint8_t ports;
  uint8_t interrupt_endpoint;
  uint8_t interrupt_interval;
  uint16_t interrupt_mps;
} usb_hub_init_info_t;

usb_hub_init_result_t usb_hub_init(usb_hub_init_info_t *info);
bool usb_hub_get_init_info(usb_hub_init_info_t *info);

typedef struct {
  uint8_t ports;
  uint16_t connected;
  uint16_t enabled;
  uint16_t changed;
  uint16_t overcurrent;
  uint8_t failed_port;
  usb_hub_result_t error;
} usb_hub_scan_info_t;

usb_hub_result_t usb_hub_scan(usb_hub_scan_info_t *info);
bool usb_hub_get_scan(usb_hub_scan_info_t *info);

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
