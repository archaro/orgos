#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  USB_INIT_OK,
  USB_INIT_PORT_CLEAR_FAILED,
  USB_INIT_PORT_READ_FAILED,
  USB_INIT_PORT_RESET_FAILED,
  USB_INIT_PORT_STATE_FAILED,
  USB_INIT_EVENT_FAILED,
  USB_INIT_EVENT_TIMEOUT,
  USB_INIT_SLOT_FAILED,
  USB_INIT_CONTEXT_FAILED,
  USB_INIT_ADDRESS_FAILED
} usb_init_result_t;

typedef struct {
  usb_init_result_t result;
  uint32_t detail;
  uint8_t root_port;
  uint8_t speed;
  uint8_t slot_id;
  uint8_t address;
  uint8_t slot_state;
  uint8_t ep0_state;
} usb_init_info_t;

usb_init_result_t usb_init(usb_init_info_t *info);
bool usb_get_init_info(usb_init_info_t *info);
