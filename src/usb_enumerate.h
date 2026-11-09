#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xhci.h"
#include "usb_device.h"
#include "usb_midi.h"
#include "usb_midi_quirks.h"

// Maximum number of managed downstream device
// records. This is storage capacity, not an
// enumeration admission limit.
#define USB_MANAGED_DEVICE_CAPACITY 4U

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

// Enumerate one independently admitted internal
// hub port, without whole-hub device restrictions.
//
// Only a safely admitted physical connection can
// acquire a managed-device record or xHCI slot.
usb_enum_result_t usb_enumerate_port(uint8_t port, usb_enum_info_t *info);

// Preview whether a physical connection can acquire
// a managed-device record. This does not reserve a
// record or admit enumeration.
bool usb_managed_record_available(uint8_t root_port,
    uint8_t parent_slot, uint8_t parent_port);

bool usb_get_enumeration(usb_enum_info_t *info);
bool usb_get_enumeration_generation(
    uint32_t generation, usb_enum_info_t *info);
// Inspect an enumeration attempt by managed
// record index, including attempts which failed
// before generation or slot assignment.
bool usb_get_enumeration_at(unsigned int index, usb_enum_info_t *info);
bool usb_get_device(usb_device_t *device);
// Read a managed device by table index.
// Returns false for an unused or out-of-range
// entry. Never falls back to the compatibility
// record and never exposes mutable state.
bool usb_get_device_at(unsigned int index, usb_device_t *device);
// Inspect the enumeration attempt associated
// with one physical downstream connection.
// Unknown or ambiguous connections are rejected.
// A generation or confirmed slot is not required.
bool usb_get_enumeration_by_connection(uint8_t root_port, uint8_t parent_slot,
    uint8_t parent_port, usb_enum_info_t *info);
// Read-only lookup of managed USB device records.
// Unknown or ambiguous identities are rejected.
bool usb_get_device_by_connection(uint8_t root_port, uint8_t parent_slot,
    uint8_t parent_port, usb_device_t *device);
bool usb_get_device_by_slot(uint8_t slot_id, usb_device_t *device);
bool usb_get_device_by_generation(uint32_t generation, usb_device_t *device);

typedef enum {
  USB_RECOVERY_READY,
  USB_RECOVERY_READY_EP0_ERROR,
  USB_RECOVERY_NO_FAILED_DEVICE,
  USB_RECOVERY_NO_CONFIRMED_SLOT,
  USB_RECOVERY_ADDRESS_UNCONFIRMED,
  USB_RECOVERY_BULK_ATTEMPTED,
  USB_RECOVERY_ALREADY_QUIESCED,
  USB_RECOVERY_EP0_ERROR_COMPLETION,
  USB_RECOVERY_EP0_OUTCOME_UNKNOWN,
  USB_RECOVERY_EP0_FAULT,
  USB_RECOVERY_TRANSFER_BUSY,
  USB_RECOVERY_TRANSFER_STATE_UNAVAILABLE
} usb_recovery_result_t;

typedef struct {
  usb_recovery_result_t result;
  uint32_t generation;
  uint8_t slot_id;
  bool ep0_submitted;
  bool ep0_completion_seen;
  uint8_t ep0_completion_code;
  bool ep0_state_valid;
  uint8_t ep0_state;
} usb_recovery_info_t;

bool usb_enumeration_recovery_inspect(usb_recovery_info_t *info);
// Generation-specific recovery interfaces.
// These reject unknown device incarnations and
// never fall back to the compatibility record.
bool usb_enumeration_recovery_inspect_generation(
    uint32_t generation, usb_recovery_info_t *info);
bool usb_enumeration_recovery_begin_generation( uint32_t generation);

bool usb_device_test_quiesce(void);
void usb_device_port_changed(uint8_t port, uint16_t status, uint16_t change);
// Teardown operations selected by the lifecycle
// executor's retained device incarnation.
bool usb_device_finalize_disconnect_generation(
    uint32_t generation, uint8_t slot_id);
bool usb_enumeration_rearm_generation(uint32_t generation);
bool usb_enumeration_rearm_rejected(uint8_t port);
bool usb_enumeration_test_abort_arm(void);
bool usb_enumeration_test_config_abort_arm(void);
