#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "midi_output.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi.h"
#include "xhci.h"
#include "timer.h"

_Static_assert(MIDI_OUTPUT_QUEUE_CAPACITY >= MIDI_OUTPUT_BATCH_PACKETS,
    "MIDI output queue must hold one transfer");

_Static_assert(MIDI_OUTPUT_BATCH_PACKETS * 4U <= 64U,
    "MIDI output batch exceeds xHCI Bulk OUT limit");

typedef struct {
  uint8_t packet[4];
  uint8_t length;
} midi_output_packet_t;

typedef struct {
  midi_output_info_t info;
  usb_midi_config_t config;
  midi_output_packet_t queue[MIDI_OUTPUT_QUEUE_CAPACITY];
  unsigned int head;
  uint8_t root_port;
  uint8_t parent_slot;
  uint8_t parent_port;
  bool failure_recorded;
  uint64_t next_submit_us;
} midi_output_record_t;

static midi_output_record_t outputs[USB_MANAGED_DEVICE_CAPACITY];

static midi_output_record_t *output_find(uint32_t generation) {
  if (generation == 0U)
    return NULL;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    if (outputs[i].info.generation == generation)
      return &outputs[i];
  }

  return NULL;
}

static bool output_matches(unsigned int index,
    const midi_output_record_t *output, usb_device_t *device) {

  if (!output || !device || output->info.generation == 0U ||
      !usb_get_device_at(index, device) ||
      device->generation != output->info.generation ||
      device->slot_id != output->info.slot_id ||
      device->root_port != output->root_port ||
      device->parent_slot != output->parent_slot ||
      device->parent_port != output->parent_port)
    return false;

  return true;
}

static void output_fail(midi_output_record_t *output,
    xhci_bulk_out_result_t error) {

  if (!output->failure_recorded) {
    output->info.failures++;
    output->failure_recorded = true;
  }

  output->info.last_error = (uint32_t)error;

  if (output->info.state == MIDI_OUTPUT_RUNNING)
    output->info.state = MIDI_OUTPUT_FAILED;
}

bool midi_output_start_generation(uint32_t generation) {
  if (!usb_service_available() || generation == 0U)
    return false;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_device_t device = {0};

    if (!usb_get_device_at(i, &device) || device.generation != generation)
      continue;

    if (device.state != USB_DEVICE_ACTIVE || device.slot_id == 0U)
      return false;

    usb_device_t by_slot = {0};
    usb_device_t by_connection = {0};

    if (!usb_get_device_by_slot(device.slot_id, &by_slot) ||
        by_slot.generation != generation ||
        !usb_get_device_by_connection(device.root_port,
            device.parent_slot, device.parent_port, &by_connection) ||
        by_connection.generation != generation ||
        by_connection.slot_id != device.slot_id)
      return false;

    usb_enum_info_t enumeration = {0};

    if (!usb_get_enumeration_generation(generation, &enumeration) ||
        enumeration.result != USB_ENUM_OK ||
        enumeration.slot_id != device.slot_id ||
        enumeration.hub_port != device.parent_port ||
        !enumeration.midi.out_found ||
        enumeration.midi.bulk_out_jacks == 0U ||
        enumeration.midi.bulk_out_jacks > USB_MIDI_MAX_CABLES)
      return false;

    midi_output_record_t *output = &outputs[i];

    if (output->info.in_flight != 0U ||
        output->info.state == MIDI_OUTPUT_RUNNING ||
        (output->info.generation == generation &&
         output->info.state != MIDI_OUTPUT_IDLE &&
         output->info.state != MIDI_OUTPUT_STOPPED))
      return false;

    xhci_bulk_out_async_info_t hw = {0};

    if (!xhci_bulk_out_async_get(device.slot_id, &hw) ||
        hw.state != XHCI_BULK_OUT_ASYNC_IDLE)
      return false;

    *output = (midi_output_record_t){0};
    output->info.state = MIDI_OUTPUT_RUNNING;
    output->info.generation = generation;
    output->info.slot_id = device.slot_id;
    output->info.cables = enumeration.midi.bulk_out_jacks;
    output->config = enumeration.midi;
    output->root_port = device.root_port;
    output->parent_slot = device.parent_slot;
    output->parent_port = device.parent_port;

    return true;
  }

  return false;
}

bool midi_output_stop_generation(uint32_t generation) {
  midi_output_record_t *output = output_find(generation);

  if (!output || output->info.state != MIDI_OUTPUT_RUNNING ||
      output->info.queue_depth != 0U || output->info.in_flight != 0U)
    return false;

  output->info.state = MIDI_OUTPUT_STOPPED;
  return true;
}

bool midi_output_owns_generation(uint32_t generation) {
  midi_output_record_t *output = output_find(generation);

  return output && output->info.state != MIDI_OUTPUT_IDLE &&
      output->info.state != MIDI_OUTPUT_STOPPED;
}

bool midi_output_enqueue_short(uint32_t generation,
    unsigned int cable, const midi_short_message_t *message) {

  midi_output_record_t *output = output_find(generation);

  if (!output || output->info.state != MIDI_OUTPUT_RUNNING)
    return false;

  uint8_t packet[4] = {0};

  if (!usb_midi_short_packet(&output->config, cable, message, packet))
    return false;

  if (output->info.queue_depth == MIDI_OUTPUT_QUEUE_CAPACITY) {
    output->info.full_rejections++;
    return false;
  }

  unsigned int tail = (output->head + output->info.queue_depth) %
      MIDI_OUTPUT_QUEUE_CAPACITY;

  for (unsigned int j = 0U; j < 4U; j++)
    output->queue[tail].packet[j] = packet[j];

  output->queue[tail].length = message->length;
  output->info.queue_depth++;
  output->info.accepted++;

  if (output->info.queue_depth > output->info.queue_peak)
    output->info.queue_peak = output->info.queue_depth;

  return true;
}

bool midi_output_get_info(uint32_t generation, midi_output_info_t *info) {

  if (!info)
    return false;

  *info = (midi_output_info_t){0};

  midi_output_record_t *output = output_find(generation);

  if (!output)
    return false;

  *info = output->info;
  return true;
}

void midi_output_service(void) {
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    midi_output_record_t *output = &outputs[i];

    if (output->info.state != MIDI_OUTPUT_RUNNING &&
        output->info.in_flight == 0U)
      continue;

    usb_device_t device = {0};
    bool same = output_matches(i, output, &device);
    bool active = same && device.state == USB_DEVICE_ACTIVE;
    uint8_t slot = output->info.slot_id;

    // Inspect the old slot only while this managed
    // record retains the same device generation.
    if (output->info.in_flight != 0U && same) {
      xhci_bulk_out_async_info_t hw = {0};

      if (!xhci_bulk_out_async_get(slot, &hw)) {
        output_fail(output, XHCI_BULK_OUT_EVENT_FAILED);

      } else if (hw.state == XHCI_BULK_OUT_ASYNC_UNCERTAIN) {
        output_fail(output, hw.result);

      } else if (hw.state == XHCI_BULK_OUT_ASYNC_COMPLETE ||
          hw.state == XHCI_BULK_OUT_ASYNC_FAILED) {

        xhci_bulk_out_async_info_t done = {0};

        if (!xhci_bulk_out_async_reap(slot, &done)) {
          output_fail(output, XHCI_BULK_OUT_EVENT_FAILED);

        } else if (done.state == XHCI_BULK_OUT_ASYNC_COMPLETE &&
            done.transfer.length == 4U * output->info.in_flight &&
            done.result == XHCI_BULK_OUT_OK && !output->failure_recorded) {

          // Remove the original batch only after
          // its complete successful transfer.
          unsigned int packets = output->info.in_flight;
          output->head = (output->head + packets) %
              MIDI_OUTPUT_QUEUE_CAPACITY;
          output->info.queue_depth -= packets;
          output->info.confirmed += packets;
          output->info.transfers++;
          output->info.in_flight = 0U;

        } else {
          output_fail(output, done.result == XHCI_BULK_OUT_OK ?
              XHCI_BULK_OUT_TRANSFER_FAILED : done.result);

          output->info.in_flight = 0U;
        }

      } else if (hw.state != XHCI_BULK_OUT_ASYNC_PENDING) {
        output_fail(output, XHCI_BULK_OUT_EVENT_FAILED);
      }
    }

    if (!active) {
      if (output->info.state == MIDI_OUTPUT_RUNNING)
        output->info.state = MIDI_OUTPUT_DISCONNECTED;

      continue;
    }

    if (output->info.state != MIDI_OUTPUT_RUNNING ||
        output->info.in_flight != 0U || output->info.queue_depth == 0U)
      continue;

    // Do not accidentally acknowledge a hardware
    // transaction belonging to another sender.
    xhci_bulk_out_async_info_t hw = {0};

    if (!xhci_bulk_out_async_get(slot, &hw) ||
        hw.state != XHCI_BULK_OUT_ASYNC_IDLE) {
      output_fail(output, XHCI_BULK_OUT_NOT_READY);
      continue;
    }

    // Allow the previous USB batch enough time to
    // drain through a standard MIDI DIN connection.
    if (timer_now_us() < output->next_submit_us)
      continue;

    unsigned int count = output->info.queue_depth;

    if (count > MIDI_OUTPUT_BATCH_PACKETS)
      count = MIDI_OUTPUT_BATCH_PACKETS;

    uint8_t data[4U * MIDI_OUTPUT_BATCH_PACKETS] = {0};
    uint64_t serial_us = 0ULL;

    for (unsigned int j = 0U; j < count; j++) {
      unsigned int index = (output->head + j) % MIDI_OUTPUT_QUEUE_CAPACITY;

      for (unsigned int k = 0U; k < 4U; k++)
        data[4U * j + k] = output->queue[index].packet[k];

      // MIDI DIN: 31,250 bits/s, ten serial bits
      // per byte, or 320 microseconds per byte.
      serial_us += 320ULL * output->queue[index].length;
    }

    xhci_bulk_out_result_t result = xhci_bulk_out_async_start(
        slot, data, 4U * count);

    if (result == XHCI_BULK_OUT_OK) {
      output->info.in_flight = count;
      output->failure_recorded = false;
      output->next_submit_us = timer_now_us() + serial_us;

    } else if (result != XHCI_BULK_OUT_NOT_READY) {
      output_fail(output, result);
    }

    // NOT_READY may be temporary. Preserve the
    // queued messages and reconsider them later.
  }
}
