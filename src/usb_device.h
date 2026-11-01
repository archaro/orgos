#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  USB_DEVICE_DETACHED,
  USB_DEVICE_DETECTED,
  USB_DEVICE_ENUMERATING,
  USB_DEVICE_CONFIGURED,
  USB_DEVICE_ACTIVE,
  USB_DEVICE_DISCONNECTING,
  USB_DEVICE_FAILED
} usb_device_state_t;

typedef enum {
  USB_SPEED_UNKNOWN,
  USB_SPEED_FULL = 1,
  USB_SPEED_LOW = 2,
  USB_SPEED_HIGH = 3,
  USB_SPEED_SUPER = 4,
  USB_SPEED_SUPER_PLUS = 5
} usb_speed_t;

typedef struct {
  // Topology: parent_slot == 0 denotes a root port.
  uint8_t root_port;
  uint8_t parent_slot;
  uint8_t parent_port;
  // Controller-assigned device identity.
  uint8_t slot_id;
  uint8_t address;
  // Device descriptors.
  uint16_t vendor_id;
  uint16_t product_id;
  // Connection and lifecycle.
  usb_speed_t speed;
  usb_device_state_t state;
  uint32_t generation;
} usb_device_t;
