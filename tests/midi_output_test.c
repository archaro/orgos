#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "midi_output.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "xhci.h"

static usb_device_t live = {
  .root_port = 1U,
  .parent_slot = 1U,
  .parent_port = 1U,
  .slot_id = 2U,
  .generation = 1U,
  .state = USB_DEVICE_ACTIVE
};

static xhci_bulk_out_async_info_t hw;
static uint64_t fake_now;
static unsigned int transfers_started;
static uint8_t transmitted[66U * 4U];
static unsigned int transmitted_length;

uint64_t timer_now_us(void) {
  return fake_now;
}

bool usb_service_available(void) {
  return true;
}

bool usb_get_device_at(unsigned int index, usb_device_t *device) {
  if (index != 0U)
    return false;

  *device = live;
  return true;
}

bool usb_get_device_by_slot(uint8_t slot, usb_device_t *device) {
  if (slot != live.slot_id)
    return false;

  *device = live;
  return true;
}

bool usb_get_device_by_connection(uint8_t root, uint8_t parent,
    uint8_t port, usb_device_t *device) {

  if (root != live.root_port || parent != live.parent_slot ||
      port != live.parent_port)
    return false;

  *device = live;
  return true;
}

bool usb_get_enumeration_generation(uint32_t generation,
    usb_enum_info_t *info) {

  if (generation != live.generation)
    return false;

  *info = (usb_enum_info_t){
    .result = USB_ENUM_OK,
    .slot_id = live.slot_id,
    .hub_port = live.parent_port,
    .midi = {
      .out_found = true,
      .bulk_out_jacks = 2U
    }
  };

  return true;
}

bool xhci_bulk_out_async_get(uint8_t slot,
    xhci_bulk_out_async_info_t *info) {

  if (slot != live.slot_id)
    return false;

  *info = hw;
  return true;
}

bool xhci_bulk_out_async_reap(uint8_t slot,
    xhci_bulk_out_async_info_t *info) {

  if (slot != live.slot_id ||
      (hw.state != XHCI_BULK_OUT_ASYNC_COMPLETE &&
       hw.state != XHCI_BULK_OUT_ASYNC_FAILED))
    return false;

  *info = hw;
  hw = (xhci_bulk_out_async_info_t){0};
  return true;
}

xhci_bulk_out_result_t xhci_bulk_out_async_start(uint8_t slot,
    const uint8_t *data, uint32_t length) {

  if (slot != live.slot_id ||
      hw.state != XHCI_BULK_OUT_ASYNC_IDLE)
    return XHCI_BULK_OUT_NOT_READY;

  assert(length > 0U && length <= 64U);
  assert(length % 4U == 0U);
  assert(transmitted_length + length <= sizeof(transmitted));

  memcpy(transmitted + transmitted_length, data, length);
  transmitted_length += length;
  transfers_started++;

  hw.state = XHCI_BULK_OUT_ASYNC_PENDING;
  hw.result = XHCI_BULK_OUT_OK;
  hw.transfer.length = length;

  return XHCI_BULK_OUT_OK;
}

static void complete(void) {
  assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);

  hw.state = XHCI_BULK_OUT_ASYNC_COMPLETE;
  hw.transfer.completion_code = 1U;
  fake_now += 16000ULL;

  midi_output_service();
}

int main(void) {
  assert(midi_output_start_generation(1U));
  assert(!midi_output_start_generation(1U));
  assert(midi_output_owns_generation(1U));

  midi_short_message_t message = {
    .status = 0xbfU,
    .data = {0x66U, 0U},
    .length = 3U
  };

  // Fill all 64 queue positions without allowing
  // the simulated hardware service to run.
  for (unsigned int i = 0U; i < 64U; i++) {
    message.data[1] = (uint8_t)i;
    assert(midi_output_enqueue_short(1U, 1U, &message));
  }

  assert(!midi_output_enqueue_short(1U, 1U, &message));

  midi_output_info_t info = {0};
  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 64U);
  assert(info.full_rejections == 1U);

  midi_output_service();

  // The first batch has been submitted but none
  // of its messages has yet been confirmed.
  assert(transfers_started == 1U);
  assert(hw.transfer.length == 64U);
  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 64U);
  assert(info.confirmed == 0U);

  for (unsigned int i = 0U; i < 4U; i++)
    complete();

  assert(transfers_started == 4U);
  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 0U);
  assert(info.confirmed == 64U);
  assert(info.transfers == 4U);
  assert(info.in_flight == 0U);

  // Confirm exact ordering across all four batches.
  for (unsigned int i = 0U; i < 64U; i++) {
    const uint8_t *packet = &transmitted[i * 4U];

    assert(packet[0] == 0x1bU);
    assert(packet[1] == 0xbfU);
    assert(packet[2] == 0x66U);
    assert(packet[3] == i);
  }

  assert(midi_output_stop_generation(1U));
  assert(!midi_output_owns_generation(1U));
  assert(midi_output_start_generation(1U));

  message.data[0] = 0x80U;
  assert(!midi_output_enqueue_short(1U, 0U, &message));

  message.data[0] = 0x66U;
  message.data[1] = 0x20U;
  assert(midi_output_enqueue_short(1U, 0U, &message));

  midi_output_service();
  assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);

  // Disconnect while a batch remains outstanding.
  live.state = USB_DEVICE_DISCONNECTING;
  midi_output_service();

  assert(midi_output_get_info(1U, &info));
  assert(info.state == MIDI_OUTPUT_DISCONNECTED);
  assert(info.queue_depth == 1U);
  assert(info.in_flight == 1U);

  // Its eventual definitive completion can still
  // be reconciled against the original generation.
  complete();

  assert(midi_output_get_info(1U, &info));
  assert(info.confirmed == 1U);
  assert(info.queue_depth == 0U);

  // A new incarnation begins with fresh state.
  live.generation = 2U;
  live.state = USB_DEVICE_ACTIVE;

  assert(midi_output_start_generation(2U));
  assert(midi_output_get_info(2U, &info));
  assert(info.accepted == 0U);
  assert(info.queue_depth == 0U);

  assert(midi_output_enqueue_short(2U, 0U, &message));
  midi_output_service();
  assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);

  // An uncertain completion must not release the
  // batch or initiate a speculative retransmission.
  hw.state = XHCI_BULK_OUT_ASYNC_UNCERTAIN;
  hw.result = XHCI_BULK_OUT_TIMEOUT;
  midi_output_service();

  assert(midi_output_get_info(2U, &info));
  assert(info.state == MIDI_OUTPUT_FAILED);
  assert(info.queue_depth == 1U);
  assert(info.confirmed == 0U);
  assert(info.in_flight == 1U);
  assert(!midi_output_enqueue_short(2U, 0U, &message));
  assert(!midi_output_stop_generation(2U));

  // Even a late terminal failure must not be
  // mistaken for successful delivery.
  hw.state = XHCI_BULK_OUT_ASYNC_FAILED;
  midi_output_service();

  assert(midi_output_get_info(2U, &info));
  assert(info.queue_depth == 1U);
  assert(info.confirmed == 0U);
  assert(info.in_flight == 0U);
  assert(info.failures == 1U);

  puts("MIDI output queue: PASS");
  return 0;
}
