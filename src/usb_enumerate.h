#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usb_midi.h"

typedef enum {
  USB_ENUM_OK,
  USB_ENUM_NOT_READY,
  USB_ENUM_ALREADY_ATTEMPTED,
  USB_ENUM_HUB_CONFIG_FAILED,
  USB_ENUM_HUB_CONTEXT_FAILED,
  USB_ENUM_HUB_POWER_FAILED,
  USB_ENUM_PORT_FAILED,
  USB_ENUM_DEVICE_NOT_FOUND,
  USB_ENUM_MULTIPLE_DEVICES,
  USB_ENUM_UNSUPPORTED_SPEED,
  USB_ENUM_RESET_FAILED,
  USB_ENUM_SLOT_FAILED,
  USB_ENUM_PREPARE_FAILED,
  USB_ENUM_ADDRESS_FAILED,
  USB_ENUM_DESCRIPTOR_FAILED,
  USB_ENUM_MPS_FAILED,
  USB_ENUM_CONFIG_FAILED,
  USB_ENUM_PARSE_FAILED,
  USB_ENUM_SET_CONFIG_FAILED,
  USB_ENUM_VERIFY_CONFIG_FAILED
} usb_enum_result_t;

typedef struct {
  usb_enum_result_t result;
  uint32_t detail;
  uint8_t hub_ports;
  uint8_t hub_port;
  uint8_t speed;
  uint8_t slot_id;
  uint8_t address;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t configuration_length;
  usb_midi_config_t midi;
} usb_enum_info_t;

usb_enum_result_t usb_enumerate(usb_enum_info_t *info);
bool usb_get_enumeration(usb_enum_info_t *info);

