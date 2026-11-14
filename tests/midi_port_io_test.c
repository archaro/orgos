#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "midi_port_io.h"
#include "midi_ports.h"
#include "midi_input.h"
#include "midi_output.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"

static usb_device_t live = {
  .root_port = 1U,
  .parent_slot = 1U,
  .parent_port = 2U,
  .slot_id = 3U,
  .generation = 1U,
  .state = USB_DEVICE_ACTIVE
};

static bool rx_running;
static usb_midi_rx_consumer_t subscriber;
static void *subscriber_context;
static uint32_t output_generation;
static uint32_t sent_generation;
static unsigned int sent_cable;

bool usb_get_device_at(unsigned int index, usb_device_t *device) {
  if (index != 0U)
    return false;

  *device = live;
  return true;
}

bool usb_midi_rx_get_info(uint32_t generation,
    usb_midi_rx_info_t *info) {

  if (generation != live.generation)
    return false;

  *info = (usb_midi_rx_info_t){
    .generation = generation,
    .slot_id = live.slot_id,
    .cables = 2U,
    .state = rx_running ?
        USB_MIDI_RX_RUNNING : USB_MIDI_RX_IDLE
  };

  return true;
}

bool usb_midi_rx_start_generation(uint32_t generation) {
  if (generation != live.generation || rx_running ||
      live.state != USB_DEVICE_ACTIVE)
    return false;

  rx_running = true;
  return true;
}

bool usb_midi_rx_subscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context) {

  if (generation != live.generation ||
      !rx_running || subscriber)
    return false;

  subscriber = consumer;
  subscriber_context = context;
  return true;
}

bool usb_midi_rx_unsubscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context) {

  if (generation != live.generation ||
      subscriber != consumer ||
      subscriber_context != context)
    return false;

  subscriber = NULL;
  subscriber_context = NULL;
  return true;
}

bool midi_output_get_info(uint32_t generation,
    midi_output_info_t *info) {

  if (generation != output_generation)
    return false;

  *info = (midi_output_info_t){
    .state = MIDI_OUTPUT_RUNNING,
    .cables = 1U
  };

  return true;
}

bool midi_output_start_generation(uint32_t generation) {
  if (generation != live.generation ||
      output_generation == generation ||
      live.state != USB_DEVICE_ACTIVE)
    return false;

  output_generation = generation;
  return true;
}

bool midi_output_enqueue_short(uint32_t generation,
    unsigned int cable, const midi_short_message_t *message) {

  if (!message || generation != output_generation ||
      cable != 0U)
    return false;

  sent_generation = generation;
  sent_cable = cable;
  return true;
}

static void inject(unsigned int cable, uint8_t status,
    uint8_t note) {

  usb_midi_event_t packet = {
    .cable = (uint8_t)cable,
    .cin = 9U,
    .length = 3U,
    .data = {status, note, 0x40U}
  };

  assert(subscriber);
  assert(subscriber(live.generation,
      &packet, subscriber_context));
}

int main(void) {
  midi_port_key_t key = {
    .root_port = 1U,
    .hub_port = 2U,
    .vendor_id = 0x7104U,
    .product_id = 0x1400U,
    .direction = MIDI_PORT_IN,
    .cable = 0U
  };

  assert(midi_ports_define(1U, &key));

  key.cable = 1U;
  assert(midi_ports_define(2U, &key));

  key.direction = MIDI_PORT_OUT;
  key.cable = 0U;
  assert(midi_ports_define(3U, &key));

  midi_port_device_t device = {
    .generation = 1U,
    .slot_id = 3U,
    .root_port = 1U,
    .hub_port = 2U,
    .vendor_id = 0x7104U,
    .product_id = 0x1400U,
    .in_cables = 2U,
    .out_cables = 1U
  };

  assert(midi_ports_reconcile(&device, 1U));

  // Opening a second cable reuses the receiver.
  assert(midi_port_open(1U));
  assert(midi_port_open(2U));
  assert(midi_port_open(3U));

  midi_short_message_t message = {
    .status = 0x90U,
    .data = {60U, 100U},
    .length = 3U
  };

  assert(!midi_port_send_short(1U, &message));
  assert(midi_port_send_short(3U, &message));
  assert(sent_generation == 1U && sent_cable == 0U);

  // Interleave three received events across
  // two virtual cables.
  inject(0U, 0x90U, 60U);
  inject(1U, 0x91U, 61U);
  inject(0U, 0x90U, 62U);

  midi_port_event_t event = {0};

  assert(!midi_port_read(3U, &event));

  // Reading cable 1 must not consume cable 0.
  assert(midi_port_read(2U, &event));
  assert(event.port == 2U && event.generation == 1U);
  assert(event.event.message.status == 0x91U);

  assert(midi_port_read(1U, &event));
  assert(event.event.message.data[0] == 60U);

  assert(midi_port_read(1U, &event));
  assert(event.event.message.data[0] == 62U);

  assert(!midi_port_read(1U, &event));

  // Exercise a wrapped FIFO and interior removal.
  for (unsigned int i = 0U; i < 64U; i++)
    inject(0U, 0x90U, (uint8_t)i);

  assert(midi_port_read(1U, &event));
  assert(event.event.message.data[0] == 0U);

  inject(1U, 0x91U, 99U);

  assert(midi_port_read(2U, &event));
  assert(event.event.message.data[0] == 99U);

  for (unsigned int i = 1U; i < 64U; i++) {
    assert(midi_port_read(1U, &event));
    assert(event.event.message.data[0] == i);
  }

  // An unfinished SysEx produces a terminal
  // abort when its physical receiver disconnects.
  usb_midi_event_t sysex = {
    .cable = 1U,
    .cin = 4U,
    .length = 3U,
    .data = {0xf0U, 0x7dU, 0x01U}
  };

  assert(subscriber(live.generation,
      &sysex, subscriber_context));

  live.state = USB_DEVICE_DISCONNECTING;
  midi_input_service();

  midi_input_event_t raw = {0};

  assert(midi_input_read_cable(1U, 1U, &raw));
  assert(raw.event.type == MIDI_STREAM_SYSEX_BEGIN);

  assert(midi_input_read_cable(1U, 1U, &raw));
  assert(raw.event.type == MIDI_STREAM_SYSEX_DATA);

  assert(midi_input_read_cable(1U, 1U, &raw));
  assert(raw.event.type == MIDI_STREAM_SYSEX_DATA);

  assert(midi_input_read_cable(1U, 1U, &raw));
  assert(raw.event.type == MIDI_STREAM_SYSEX_ABORT);

  assert(!midi_input_read_cable(1U, 1U, &raw));

  // An unbound port cannot route new operations.
  assert(midi_ports_reconcile(NULL, 0U));
  assert(!midi_port_open(3U));
  assert(!midi_port_send_short(3U, &message));

  // Rebind the same logical ports to generation 9.
  live.state = USB_DEVICE_ACTIVE;
  live.generation = 9U;
  live.slot_id = 4U;
  device.generation = 9U;
  device.slot_id = 4U;
  rx_running = false;
  output_generation = 0U;

  assert(midi_ports_reconcile(&device, 1U));
  assert(midi_port_open(1U));
  assert(midi_port_open(3U));

  assert(midi_port_send_short(3U, &message));
  assert(sent_generation == 9U);
  assert(!midi_port_read(1U, &event));

  puts("MIDI logical I/O: PASS");
  return 0;
}
