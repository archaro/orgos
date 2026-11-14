#include <stdbool.h>
#include <stdint.h>

#include "midi_ports_usb.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi.h"

_Static_assert(MIDI_PORT_MAX_CABLES == USB_MIDI_MAX_CABLES,
    "Logical and USB MIDI cable limits differ");

static bool usable(const usb_device_t *device,
    const usb_enum_info_t *enumeration) {

  return device->state == USB_DEVICE_ACTIVE &&
      device->generation != 0U && device->slot_id != 0U &&
      device->root_port != 0U &&
      device->parent_slot != 0U && device->parent_port != 0U &&
      enumeration->result == USB_ENUM_OK &&
      enumeration->slot_id == device->slot_id &&
      enumeration->hub_port == device->parent_port &&
      enumeration->vendor_id == device->vendor_id &&
      enumeration->product_id == device->product_id &&
      enumeration->midi.midi_found;
}

void midi_ports_usb_service(void) {
  midi_port_device_t devices[USB_MANAGED_DEVICE_CAPACITY] = {0};
  unsigned int count = 0U;

  if (usb_service_available()) {
    for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
      usb_device_t device = {0};
      usb_enum_info_t enumeration = {0};

      if (!usb_get_device_at(i, &device) ||
          !usb_get_enumeration_generation(device.generation, &enumeration) ||
          !usable(&device, &enumeration))
        continue;

      devices[count++] = (midi_port_device_t){
        .generation = device.generation,
        .slot_id = device.slot_id,
        .root_port = device.root_port,
        .hub_port = device.parent_port,
        .vendor_id = device.vendor_id,
        .product_id = device.product_id,
        .in_cables = enumeration.midi.in_found ?
            enumeration.midi.bulk_in_jacks : 0U,
        .out_cables = enumeration.midi.out_found ?
            enumeration.midi.bulk_out_jacks : 0U
      };
    }
  }

  midi_ports_reconcile(devices, count);
}

bool midi_ports_usb_define(unsigned int id, midi_port_direction_t direction,
    uint32_t generation, unsigned int cable) {
  if (!usb_service_available() || generation == 0U ||
      cable >= MIDI_PORT_MAX_CABLES || (direction != MIDI_PORT_IN &&
       direction != MIDI_PORT_OUT))
    return false;

  usb_device_t device = {0};
  usb_enum_info_t enumeration = {0};
  usb_device_t by_connection = {0};

  if (!usb_get_device_by_generation(generation, &device) ||
      !usb_get_enumeration_generation(generation, &enumeration) ||
      !usable(&device, &enumeration) ||
      !usb_get_device_by_connection(device.root_port,
          device.parent_slot, device.parent_port, &by_connection) ||
      by_connection.generation != device.generation ||
      by_connection.slot_id != device.slot_id)
    return false;

  unsigned int cables = direction == MIDI_PORT_IN ?
      (enumeration.midi.in_found ?  enumeration.midi.bulk_in_jacks : 0U) :
      (enumeration.midi.out_found ?  enumeration.midi.bulk_out_jacks : 0U);

  if (cables > MIDI_PORT_MAX_CABLES || cable >= cables)
    return false;

  midi_port_key_t key = {
    .root_port = device.root_port,
    .hub_port = device.parent_port,
    .vendor_id = device.vendor_id,
    .product_id = device.product_id,
    .cable = (uint8_t)cable,
    .direction = direction
  };

  if (!midi_ports_define(id, &key))
    return false;

  midi_ports_usb_service();
  return true;
}

