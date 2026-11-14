#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "midi_ports.h"

int main(void) {
  midi_port_key_t input = {
    .root_port = 1U,
    .hub_port = 2U,
    .vendor_id = 0x7104U,
    .product_id = 0x1400U,
    .cable = 1U,
    .direction = MIDI_PORT_IN
  };

  midi_port_key_t output = input;
  output.direction = MIDI_PORT_OUT;
  output.cable = 0U;

  midi_port_device_t devices[2] = {{
    .generation = 1U,
    .slot_id = 3U,
    .root_port = 1U,
    .hub_port = 2U,
    .vendor_id = 0x7104U,
    .product_id = 0x1400U,
    .in_cables = 3U,
    .out_cables = 1U
  }};

  midi_port_info_t info = {0};
  uint32_t generation = 0U;
  uint8_t cable = 0U;
  midi_port_direction_t direction = MIDI_PORT_OUT;

  // Validate definitions and prevent aliases.
  assert(!midi_ports_define(0U, &input));
  assert(!midi_ports_define(33U, &input));
  assert(midi_ports_define(1U, &input));
  assert(!midi_ports_define(1U, &output));
  assert(!midi_ports_define(3U, &input));
  assert(midi_ports_define(2U, &output));

  assert(midi_ports_get(1U, &info));
  assert(info.state == MIDI_PORT_UNBOUND);
  assert(!midi_ports_resolve(
      1U, &generation, &cable, &direction));

  // Initially both logical endpoints are present.
  assert(midi_ports_reconcile(devices, 1U));
  assert(midi_ports_resolve(
      1U, &generation, &cable, &direction));
  assert(generation == 1U && cable == 1U &&
      direction == MIDI_PORT_IN);

  assert(midi_ports_resolve(
      2U, &generation, &cable, &direction));
  assert(generation == 1U && cable == 0U &&
      direction == MIDI_PORT_OUT);

  // Disconnection removes bindings, not identities.
  assert(midi_ports_reconcile(NULL, 0U));
  assert(!midi_ports_resolve(
      1U, &generation, &cable, &direction));
  assert(midi_ports_get(1U, &info));
  assert(info.id == 1U && info.state == MIDI_PORT_UNBOUND);

  // Reconnect with a different generation and slot.
  devices[0].generation = 9U;
  devices[0].slot_id = 5U;
  assert(midi_ports_reconcile(devices, 1U));
  assert(midi_ports_resolve(
      1U, &generation, &cable, &direction));
  assert(generation == 9U);

  // A different product must not inherit the port.
  devices[0].product_id++;
  assert(midi_ports_reconcile(devices, 1U));
  assert(!midi_ports_resolve(
      1U, &generation, &cable, &direction));
  devices[0].product_id--;

  // Moving to another socket must not rebind.
  devices[0].hub_port++;
  assert(midi_ports_reconcile(devices, 1U));
  assert(!midi_ports_resolve(
      1U, &generation, &cable, &direction));
  devices[0].hub_port--;

  // Removing cable 1 affects only its logical port.
  devices[0].in_cables = 1U;
  assert(midi_ports_reconcile(devices, 1U));
  assert(!midi_ports_resolve(
      1U, &generation, &cable, &direction));
  assert(midi_ports_resolve(
      2U, &generation, &cable, &direction));
  devices[0].in_cables = 3U;

  // Ambiguous matches must not select a device.
  devices[1] = devices[0];
  devices[1].generation = 10U;
  devices[1].slot_id = 6U;

  assert(midi_ports_reconcile(devices, 2U));
  assert(midi_ports_get(1U, &info));
  assert(info.state == MIDI_PORT_AMBIGUOUS);
  assert(!midi_ports_resolve(
      1U, &generation, &cable, &direction));

  // Removing ambiguity restores the binding.
  assert(midi_ports_reconcile(devices, 1U));
  assert(midi_ports_resolve(
      1U, &generation, &cable, &direction));

  assert(midi_ports_get(1U, &info));
  uint32_t original_serial = info.definition_serial;

  // Definition can be explicitly cleared and reused.
  assert(midi_ports_clear(1U));
  assert(!midi_ports_get(1U, &info));
  assert(!midi_ports_clear(1U));
  assert(midi_ports_define(1U, &input));

  assert(midi_ports_get(1U, &info));
  assert(info.definition_serial != original_serial);

  puts("MIDI logical ports: PASS");
  return 0;
}
