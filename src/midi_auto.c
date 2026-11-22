#include <stdbool.h>
#include <stdint.h>

#include "timer.h"
#include "xhci_transfer.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"
#include "midi_auto.h"
#include "midi_output.h"

#define MIDI_AUTO_RETRY_US 500000ULL
#define MIDI_AUTO_IN  0x01U
#define MIDI_AUTO_OUT 0x02U

typedef struct {
  uint32_t generation;
  uint64_t next_attempt_us;
  uint32_t in_attempts;
  uint32_t out_attempts;
  uint8_t pending;
} midi_auto_record_t;

static midi_auto_record_t records[USB_MANAGED_DEVICE_CAPACITY];

// Attach supported MIDI devices automatically.
// Diagnostics may disable admission before
// connecting a device they need to control.
static midi_auto_info_t auto_state = {
  .enabled = true
};

void midi_auto_set_enabled(bool enabled) {
  auto_state.enabled = enabled;
}

bool midi_auto_get_info(midi_auto_info_t *info) {
  if (!info)
    return false;

  *info = auto_state;
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {

    usb_device_t device = {0};
    if (!usb_get_device_at(i, &device) || device.state != USB_DEVICE_ACTIVE ||
        device.generation != records[i].generation)
      continue;

    if (records[i].pending & MIDI_AUTO_IN)
      info->input_pending++;

    if (records[i].pending & MIDI_AUTO_OUT)
      info->output_pending++;
  }
  return true;
}

static void midi_auto_try_output(
    const usb_device_t *device, midi_auto_record_t *record) {

  if (!(record->pending & MIDI_AUTO_OUT))
    return;

  midi_output_info_t output = {0};
  if (midi_output_get_info(device->generation, &output) &&
      output.state != MIDI_OUTPUT_IDLE) {

    // Existing manual ownership must not be
    // displaced, including an explicit stop.
    if (output.state == MIDI_OUTPUT_RUNNING ||
        output.state == MIDI_OUTPUT_STOPPED)
      auto_state.output_skipped++;
    else
      auto_state.output_terminal++;

    record->pending &= (uint8_t)~MIDI_AUTO_OUT;
    return;
  }

  xhci_bulk_out_async_info_t hw = {0};
  if (xhci_bulk_out_async_get(device->slot_id, &hw)) {
    // Another operation may own this endpoint.
    if (hw.state == XHCI_BULK_OUT_ASYNC_PENDING ||
        hw.state == XHCI_BULK_OUT_ASYNC_COMPLETE) {
      auto_state.output_skipped++;
      record->pending &= (uint8_t)~MIDI_AUTO_OUT;
      return;
    }

    // Never recycle an uncertain or failed
    // hardware transfer through automatic retry.
    if (hw.state == XHCI_BULK_OUT_ASYNC_FAILED ||
        hw.state == XHCI_BULK_OUT_ASYNC_UNCERTAIN) {
      auto_state.output_terminal++;
      record->pending &= (uint8_t)~MIDI_AUTO_OUT;
      return;
    }
  }

  if (record->out_attempts++ != 0U)
    auto_state.output_retries++;

  if (midi_output_start_generation(device->generation)) {
    auto_state.output_started++;
    record->pending &= (uint8_t)~MIDI_AUTO_OUT;
  } else {
    auto_state.output_failed++;
  }
}

static void midi_auto_try_input(
    const usb_device_t *device, midi_auto_record_t *record) {

  if (!(record->pending & MIDI_AUTO_IN))
    return;

  usb_midi_rx_info_t rx = {0};
  if (usb_midi_rx_get_info(device->generation, &rx) &&
      rx.state != USB_MIDI_RX_IDLE) {

    if (rx.state == USB_MIDI_RX_RUNNING)
      auto_state.skipped++;
    else
      auto_state.input_terminal++;

    record->pending &= (uint8_t)~MIDI_AUTO_IN;
    return;
  }

  xhci_bulk_in_stream_stats_t stream = {0};
  if (xhci_bulk_in_stream_get_stats(device->slot_id, &stream)) {

    if (stream.failed ||
        stream.last_arm_result == XHCI_BULK_IN_ARM_DMA_FAILED ||
        stream.last_arm_result == XHCI_BULK_IN_ARM_RING_FAILED ||
        stream.last_arm_result == XHCI_BULK_IN_ARM_WAITER_FAILED ||
        (!stream.started && stream.last_arm_result ==
             XHCI_BULK_IN_ARM_ALREADY_STARTED)) {

      auto_state.input_terminal++;
      record->pending &= (uint8_t)~MIDI_AUTO_IN;
      return;
    }

    // A manual receiver or diagnostic owns
    // this stream. Leave it entirely alone.
    if (stream.started) {
      auto_state.skipped++;
      record->pending &= (uint8_t)~MIDI_AUTO_IN;
      return;
    }
  }

  if (record->in_attempts++ != 0U)
    auto_state.input_retries++;

  if (usb_midi_rx_start_auto_generation(device->generation)) {
    auto_state.started++;
    record->pending &= (uint8_t)~MIDI_AUTO_IN;
  } else {
    auto_state.failed++;
  }
}

void midi_auto_service(void) {
  if (!auto_state.enabled || !usb_service_available())
    return;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {

    usb_device_t device = {0};
    if (!usb_get_device_at(i, &device) || device.state != USB_DEVICE_ACTIVE ||
        device.generation == 0U || device.slot_id == 0U)
      continue;

    usb_enum_info_t enumeration = {0};
    if (!usb_get_enumeration_generation(device.generation, &enumeration) ||
        enumeration.result != USB_ENUM_OK ||
        enumeration.slot_id != device.slot_id ||
        enumeration.hub_port != device.parent_port)
      continue;

    midi_auto_record_t *record = &records[i];

    if (record->generation != device.generation) {
      uint8_t pending = 0U;

      if (enumeration.midi.in_found &&
          enumeration.midi.bulk_in_jacks != 0U &&
          enumeration.midi.bulk_in_jacks <= USB_MIDI_MAX_CABLES)
        pending |= MIDI_AUTO_IN;

      if (enumeration.midi.out_found &&
          enumeration.midi.bulk_out_jacks != 0U &&
          enumeration.midi.bulk_out_jacks <= USB_MIDI_MAX_CABLES)
        pending |= MIDI_AUTO_OUT;

      // A recycled managed-device record must
      // never inherit the previous incarnation's
      // retry deadlines or attachment decisions.
      *record = (midi_auto_record_t){
        .generation = device.generation,
        .pending = pending
      };

      if (pending != 0U) {
        auto_state.attempts++;
        auto_state.last_generation = device.generation;
      }
    }

    if (record->pending == 0U || timer_now_us() < record->next_attempt_us)
      continue;

    midi_auto_try_output(&device, record);
    midi_auto_try_input(&device, record);

    // Retry only outstanding directions. The
    // foreground service must never spin while
    // waiting for an endpoint to become ready.
    if (record->pending != 0U)
      record->next_attempt_us = timer_now_us() + MIDI_AUTO_RETRY_US;
  }
}
