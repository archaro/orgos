#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "midi_input.h"
#include "midi_usb_bridge.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"

typedef struct {
  midi_input_info_t info;
  midi_usb_bridge_t bridge;
} midi_input_record_t;

static midi_input_record_t inputs[USB_MANAGED_DEVICE_CAPACITY];

static midi_input_record_t *input_find(uint32_t generation) {
  if (generation == 0U)
    return NULL;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    if (inputs[i].info.generation == generation)
      return &inputs[i];
  }

  return NULL;
}

static void input_count(midi_input_record_t *input,
    const midi_usb_batch_t *batch) {

  for (unsigned int i = 0U; i < batch->count; i++) {
    const midi_stream_event_t *event = &batch->events[i];
    switch (event->type) {
      case MIDI_STREAM_SHORT:
        input->info.short_messages++;
        break;
      case MIDI_STREAM_SYSEX_BEGIN:
        input->info.sysex_begins++;
        break;
      case MIDI_STREAM_SYSEX_DATA:
        input->info.sysex_bytes++;
        break;
      case MIDI_STREAM_SYSEX_END:
        input->info.sysex_ends++;
        break;
      case MIDI_STREAM_SYSEX_ABORT:
        input->info.sysex_aborts++;
        break;
    }

    input->info.last = *event;
    input->info.last_cable = batch->cable;
    input->info.have_last = true;
  }
}

static void input_cancel_all(midi_input_record_t *input) {
  for (unsigned int cable = 0U; cable < input->bridge.cables; cable++) {
    midi_usb_batch_t batch = {0};
    if (midi_usb_bridge_cancel(&input->bridge, cable, &batch))
      input_count(input, &batch);
  }
}

static bool input_accept(uint32_t generation,
    const usb_midi_event_t *packet, void *context) {

  midi_input_record_t *input = context;

  if (!input || !packet || !input->info.attached ||
      input->info.generation != generation)
    return false;

  midi_stream_parser_t next = {0};
  midi_usb_batch_t batch = {0};

  if (!midi_usb_bridge_prepare(&input->bridge, packet, &next, &batch)) {

    // A malformed packet invalidates only the
    // corresponding cable's partial MIDI state.
    input->info.malformed++;

    midi_usb_batch_t cancelled = {0};

    if (midi_usb_bridge_cancel( &input->bridge, packet->cable, &cancelled))
      input_count(input, &cancelled);

    // Consume the malformed packet. Repeatedly
    // retrying it would permanently block input.
    return true;
  }

  // Our diagnostic consumer accepts the entire
  // batch. An application queue will later be
  // able to reject it before committing.
  if (!midi_usb_bridge_commit(&input->bridge, &batch, &next))
    return false;

  input->info.packets++;
  input_count(input, &batch);
  return true;
}

bool midi_input_start_generation(uint32_t generation) {
  if (generation == 0U)
    return false;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {

    usb_device_t device = {0};

    if (!usb_get_device_at(i, &device) || device.generation != generation)
      continue;

    if (device.state != USB_DEVICE_ACTIVE)
      return false;

    usb_midi_rx_info_t rx = {0};

    if (!usb_midi_rx_get_info(generation, &rx) ||
        rx.state != USB_MIDI_RX_RUNNING ||
        rx.generation != generation ||
        rx.slot_id != device.slot_id)
      return false;

    midi_input_record_t *input = &inputs[i];

    if (input->info.attached)
      return false;

    midi_input_record_t fresh = {0};

    if (!midi_usb_bridge_init(&fresh.bridge, rx.cables))
      return false;

    // Subscribe before replacing the old record.
    // The subscription itself invokes no callback.
    if (!usb_midi_rx_subscribe(generation, input_accept, input))
      return false;

    fresh.info.generation = generation;
    fresh.info.cables = rx.cables;
    fresh.info.attached = true;
    *input = fresh;

    return true;
  }

  return false;
}

bool midi_input_stop_generation(uint32_t generation) {
  midi_input_record_t *input = input_find(generation);

  if (!input || !input->info.attached ||
      !usb_midi_rx_unsubscribe(generation, input_accept, input))
    return false;

  input_cancel_all(input);
  input->info.attached = false;

  return true;
}

bool midi_input_get_info(uint32_t generation, midi_input_info_t *info) {

  if (!info)
    return false;

  *info = (midi_input_info_t){0};

  midi_input_record_t *input = input_find(generation);
  if (!input)
    return false;

  *info = input->info;
  return true;
}

void midi_input_service(void) {
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    midi_input_record_t *input = &inputs[i];
    if (!input->info.attached)
      continue;

    usb_device_t device = {0};
    usb_midi_rx_info_t rx = {0};
    uint32_t generation = input->info.generation;

    if (usb_get_device_at(i, &device) &&
        device.state == USB_DEVICE_ACTIVE &&
        device.generation == generation &&
        usb_midi_rx_get_info(generation, &rx) &&
        rx.state == USB_MIDI_RX_RUNNING &&
        rx.slot_id == device.slot_id)
      continue;

    // The original incarnation can no longer
    // deliver events. Never let a replacement
    // device inherit its parser state.
    usb_midi_rx_unsubscribe(generation, input_accept, input);

    input_cancel_all(input);
    input->info.attached = false;
    input->info.lost = true;
  }
}
