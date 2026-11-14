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
  // At most two bytes await a complete
  // USB-MIDI SysEx event packet.
  uint8_t sysex_pending[2];
} midi_output_record_t;

static midi_output_record_t outputs[USB_MANAGED_DEVICE_CAPACITY];

static uint32_t next_sysex_claim = 1U;

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
      output->info.queue_depth != 0U || output->info.in_flight != 0U ||
      output->info.sysex_active)
    return false;

  output->info.state = MIDI_OUTPUT_STOPPED;
  return true;
}

bool midi_output_owns_generation(uint32_t generation) {
  midi_output_record_t *output = output_find(generation);

  return output && output->info.state != MIDI_OUTPUT_IDLE &&
      output->info.state != MIDI_OUTPUT_STOPPED;
}

static bool output_enqueue_packet(midi_output_record_t *output,
    const uint8_t packet[4], uint8_t length) {

  if (!output || !packet || length == 0U || length > 3U)
    return false;

  if (output->info.queue_depth == MIDI_OUTPUT_QUEUE_CAPACITY) {
    output->info.full_rejections++;
    return false;
  }

  unsigned int tail = (output->head + output->info.queue_depth) %
      MIDI_OUTPUT_QUEUE_CAPACITY;

  midi_output_packet_t *entry = &output->queue[tail];

  for (unsigned int i = 0U; i < 4U; i++)
    entry->packet[i] = packet[i];

  entry->length = length;

  output->info.queue_depth++;
  output->info.accepted++;

  if (output->info.queue_depth > output->info.queue_peak)
    output->info.queue_peak = output->info.queue_depth;

  return true;
}

bool midi_output_enqueue_short(uint32_t generation,
    unsigned int cable, const midi_short_message_t *message) {

  midi_output_record_t *output = output_find(generation);

  if (!output || output->info.state != MIDI_OUTPUT_RUNNING ||
      output->info.sysex_active)
    return false;

  uint8_t packet[4] = {0};

  if (!usb_midi_short_packet(&output->config, cable, message, packet))
    return false;

  return output_enqueue_packet(output, packet, message->length);
}

static bool sysex_begin(midi_output_record_t *output,
    unsigned int cable, uint32_t claim) {

  if (!output || output->info.state != MIDI_OUTPUT_RUNNING ||
      output->info.sysex_active ||
      cable >= output->info.cables)
    return false;

  output->info.sysex_active = true;
  output->info.sysex_cable = (uint8_t)cable;
  output->info.sysex_staged = 1U;
  output->info.sysex_claim = claim;
  output->info.sysex_fragments = 0U;
  output->sysex_pending[0] = 0xf0U;
  output->info.sysex_started++;

  return true;
}

static bool sysex_owned(const midi_output_record_t *output,
    uint32_t claim) {
  return output && output->info.sysex_active &&
      output->info.sysex_claim == claim;
}

static void sysex_release(midi_output_record_t *output) {
  output->info.sysex_active = false;
  output->info.sysex_staged = 0U;
  output->info.sysex_claim = 0U;
  output->info.sysex_fragments = 0U;
}

static bool sysex_write(midi_output_record_t *output,
    uint32_t claim, uint8_t byte) {

  if (!sysex_owned(output, claim) ||
      output->info.state != MIDI_OUTPUT_RUNNING ||
      byte >= 0x80U)
    return false;

  unsigned int staged = output->info.sysex_staged;

  if (staged < 2U) {
    output->sysex_pending[staged] = byte;
    output->info.sysex_staged++;

  } else if (staged == 2U) {
    uint8_t bytes[3] = {
      output->sysex_pending[0],
      output->sysex_pending[1],
      byte
    };
    uint8_t packet[4] = {0};

    if (!usb_midi_sysex_packet(&output->config,
            output->info.sysex_cable,
            bytes, 3U, false, packet))
      return false;

    if (!output_enqueue_packet(output, packet, 3U))
      return false;

    output->info.sysex_staged = 0U;
    output->info.sysex_fragments++;

  } else {
    return false;
  }

  output->info.sysex_payload_bytes++;
  return true;
}

static bool sysex_end(midi_output_record_t *output,
    uint32_t claim) {

  if (!sysex_owned(output, claim) ||
      output->info.state != MIDI_OUTPUT_RUNNING ||
      output->info.sysex_staged > 2U)
    return false;

  unsigned int staged = output->info.sysex_staged;
  uint8_t bytes[3] = {0};
  uint8_t packet[4] = {0};

  for (unsigned int i = 0U; i < staged; i++)
    bytes[i] = output->sysex_pending[i];

  bytes[staged] = 0xf7U;

  if (!usb_midi_sysex_packet(&output->config,
          output->info.sysex_cable,
          bytes, staged + 1U, true, packet))
    return false;

  if (!output_enqueue_packet(output, packet,
          (uint8_t)(staged + 1U)))
    return false;

  output->info.sysex_completed++;
  sysex_release(output);
  return true;
}

static bool sysex_cancel(midi_output_record_t *output,
    uint32_t claim) {

  if (!sysex_owned(output, claim))
    return false;

  // If no complete fragment was queued, F0 has
  // never left the staging buffer. Nothing needs
  // terminating on the MIDI connection.
  //
  // If the hardware is already disconnected or
  // failed, only local ownership can be released.
  // Existing queued/in-flight DMA data is untouched.
  if (output->info.state == MIDI_OUTPUT_RUNNING &&
      output->info.sysex_fragments != 0U) {

    uint8_t bytes[1] = {0xf7U};
    uint8_t packet[4] = {0};

    if (!usb_midi_sysex_packet(&output->config,
            output->info.sysex_cable,
            bytes, 1U, true, packet))
      return false;

    // The terminator must follow every earlier
    // fragment. On backpressure, leave the whole
    // session active for an explicit retry.
    if (!output_enqueue_packet(output, packet, 1U))
      return false;
  }

  output->info.sysex_cancelled++;
  sysex_release(output);
  return true;
}

bool midi_output_sysex_begin(uint32_t generation,
    unsigned int cable) {

  return sysex_begin(output_find(generation), cable, 0U);
}

bool midi_output_sysex_write(uint32_t generation,
    uint8_t byte) {

  return sysex_write(output_find(generation), 0U, byte);
}

bool midi_output_sysex_end(uint32_t generation) {
  return sysex_end(output_find(generation), 0U);
}

bool midi_output_sysex_cancel(uint32_t generation) {
  return sysex_cancel(output_find(generation), 0U);
}

bool midi_output_sysex_claim(uint32_t generation,
    unsigned int cable, uint32_t *claim) {

  if (!claim)
    return false;

  *claim = 0U;

  // Do not wrap and accidentally authorise an old
  // session handle with a reused claim identifier.
  if (next_sysex_claim == 0U)
    return false;

  uint32_t candidate = next_sysex_claim;

  if (!sysex_begin(output_find(generation), cable, candidate))
    return false;

  next_sysex_claim++;
  *claim = candidate;
  return true;
}

bool midi_output_sysex_write_claim(uint32_t generation,
    uint32_t claim, uint8_t byte) {

  return claim != 0U && sysex_write(output_find(generation), claim, byte);
}

bool midi_output_sysex_end_claim(uint32_t generation, uint32_t claim) {

  return claim != 0U && sysex_end(output_find(generation), claim);
}

bool midi_output_sysex_cancel_claim(uint32_t generation, uint32_t claim) {

  return claim != 0U && sysex_cancel(output_find(generation), claim);
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
