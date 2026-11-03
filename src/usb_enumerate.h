#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xhci.h"
#include "usb_device.h"
#include "usb_midi.h"
#include "usb_midi_quirks.h"

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
  USB_ENUM_VERIFY_CONFIG_FAILED,
  USB_ENUM_BULK_CONFIG_FAILED,
  USB_ENUM_SET_INTERFACE_FAILED,
  USB_ENUM_MIDI_QUIRK_FAILED,
  USB_ENUM_DEBOUNCE_FAILED,
  USB_ENUM_TEST_ABORT,
  USB_ENUM_TEST_CONFIG_ABORT
} usb_enum_result_t;

typedef enum {
  USB_ENUM_STAGE_NONE,
  USB_ENUM_STAGE_DISCOVER,
  USB_ENUM_STAGE_ENABLE_SLOT,
  USB_ENUM_STAGE_PREPARE,
  USB_ENUM_STAGE_ADDRESS,
  USB_ENUM_STAGE_DEVICE_DESCRIPTOR,
  USB_ENUM_STAGE_CONFIGURATION_DESCRIPTOR,
  USB_ENUM_STAGE_SELECT_CONFIGURATION,
  USB_ENUM_STAGE_CONFIGURE_BULK,
  USB_ENUM_STAGE_MIDI_QUIRK,
  USB_ENUM_STAGE_COMPLETE
} usb_enum_stage_t;

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
  uint32_t quirks;
  uint16_t configuration_length;
  usb_midi_config_t midi;
  usb_enum_stage_t stage;
  bool slot_attempted;
  bool slot_confirmed;
  bool prepare_attempted;
  bool address_attempted;
  bool bulk_attempted;
  bool ep0_operation_failed;
  xhci_control_result_t ep0_result;
} usb_enum_info_t;

usb_enum_result_t usb_enumerate(usb_enum_info_t *info);
bool usb_get_enumeration(usb_enum_info_t *info);
bool usb_get_device(usb_device_t *device);
bool usb_enumeration_recovery_ready(void);
bool usb_enumeration_recovery_begin(void);
bool usb_device_test_quiesce(void);
void usb_device_port_changed(uint8_t port, uint16_t status, uint16_t change);
bool usb_device_finalize_disconnect(void);
bool usb_enumeration_rearm(void);
bool usb_enumeration_test_abort_arm(void);
bool usb_enumeration_test_config_abort_arm(void);
