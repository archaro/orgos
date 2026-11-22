#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xhci_transfer.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi.h"
#include "midi_output.h"

static usb_device_t live = {
  .root_port = 1U,
  .parent_slot = 1U,
  .parent_port = 1U,
  .slot_id = 2U,
  .generation = 1U,
  .state = USB_DEVICE_ACTIVE
};

static xhci_bulk_out_async_info_t hw;
static bool hw_available = true;
static bool fail_reap_once;
static uint64_t fake_now;
static unsigned int transfers_started;
static uint8_t transmitted[512U * 4U];
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

  if (slot != live.slot_id || !hw_available)
    return false;

  *info = hw;
  return true;
}

bool xhci_bulk_out_async_reap(uint8_t slot,
    xhci_bulk_out_async_info_t *info) {

  if (fail_reap_once) {
    fail_reap_once = false;
    return false;
  }

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

static void test_sysex(void) {
  unsigned int offset = transmitted_length;

  // Begin a SysEx message on virtual cable 1.
  assert(midi_output_sysex_begin(1U, 1U));
  assert(!midi_output_sysex_begin(1U, 0U));

  midi_short_message_t clock = {
    .status = 0xf8U,
    .length = 1U
  };

  // This first implementation enforces exclusive
  // SysEx ownership of the output stream.
  assert(!midi_output_enqueue_short(1U, 1U, &clock));
  assert(!midi_output_stop_generation(1U));

  assert(!midi_output_sysex_write(1U, 0x80U));

  assert(midi_output_sysex_write(1U, 0x7dU));
  assert(midi_output_sysex_write(1U, 0x01U));
  assert(midi_output_sysex_write(1U, 0x02U));
  assert(midi_output_sysex_write(1U, 0x03U));
  assert(midi_output_sysex_write(1U, 0x04U));
  assert(midi_output_sysex_end(1U));
  assert(!midi_output_sysex_end(1U));

  midi_output_info_t info = {0};

  assert(midi_output_get_info(1U, &info));
  assert(!info.sysex_active);
  assert(info.queue_depth == 3U);
  assert(info.sysex_payload_bytes == 5U);
  assert(info.sysex_started == 1U);
  assert(info.sysex_completed == 1U);

  midi_output_service();

  assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);
  assert(hw.transfer.length == 12U);

  const uint8_t expected[12] = {
    0x14U, 0xf0U, 0x7dU, 0x01U,
    0x14U, 0x02U, 0x03U, 0x04U,
    0x15U, 0xf7U, 0x00U, 0x00U
  };

  assert(transmitted_length == offset + sizeof(expected));
  assert(memcmp(transmitted + offset,
      expected, sizeof(expected)) == 0);

  complete();

  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 0U);
  assert(info.confirmed == 67U);

  // Empty SysEx: F0 F7, encoded as CIN 6.
  offset = transmitted_length;

  assert(midi_output_sysex_begin(1U, 0U));
  assert(midi_output_sysex_end(1U));

  midi_output_service();

  assert(transmitted_length == offset + 4U);
  assert(transmitted[offset] == 0x06U);
  assert(transmitted[offset + 1U] == 0xf0U);
  assert(transmitted[offset + 2U] == 0xf7U);

  complete();

  // One payload byte: F0 7D F7, CIN 7.
  offset = transmitted_length;

  assert(midi_output_sysex_begin(1U, 0U));
  assert(midi_output_sysex_write(1U, 0x7dU));
  assert(midi_output_sysex_end(1U));

  midi_output_service();

  assert(transmitted_length == offset + 4U);
  assert(transmitted[offset] == 0x07U);
  assert(transmitted[offset + 1U] == 0xf0U);
  assert(transmitted[offset + 2U] == 0x7dU);
  assert(transmitted[offset + 3U] == 0xf7U);

  complete();

  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 0U);
  assert(info.sysex_started == 3U);
  assert(info.sysex_completed == 3U);
  assert(info.sysex_payload_bytes == 6U);

  // Normal short messages work after EOX.
  assert(midi_output_enqueue_short(1U, 0U, &clock));
  midi_output_service();
  complete();

  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 0U);
}

static void test_long_sysex(void) {
  unsigned int offset = transmitted_length;
  unsigned int accepted = 0U;
  bool backpressured = false;

  assert(midi_output_sysex_begin(1U, 0U));

  // Deliberately produce more SysEx packets than
  // the entire 64-packet output FIFO can hold.
  while (accepted < 200U) {
    uint8_t byte = (uint8_t)(accepted & 0x7fU);

    if (midi_output_sysex_write(1U, byte)) {
      accepted++;
      continue;
    }

    backpressured = true;

    // Free capacity through definitive hardware
    // completions, then retry the SAME byte.
    midi_output_service();
    assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);
    complete();
  }

  while (!midi_output_sysex_end(1U)) {
    midi_output_service();
    assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);
    complete();
  }

  // Drain every remaining queued packet.
  for (unsigned int i = 0U; i < 16U; i++) {
    midi_output_info_t info = {0};
    assert(midi_output_get_info(1U, &info));

    if (info.queue_depth == 0U)
      break;

    midi_output_service();
    assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);
    complete();
  }

  midi_output_info_t info = {0};
  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 0U);
  assert(info.in_flight == 0U);
  assert(backpressured);

  // F0 + 200 payload bytes + F7 = 202 bytes.
  // 67 three-byte packets plus one final packet.
  assert(transmitted_length == offset + 68U * 4U);

  uint8_t reconstructed[202] = {0};
  unsigned int count = 0U;

  for (unsigned int i = 0U; i < 68U; i++) {
    const uint8_t *packet =
        &transmitted[offset + i * 4U];

    unsigned int cin = packet[0] & 0x0fU;
    unsigned int length = cin == 4U ? 3U : cin - 4U;

    assert(cin >= 4U && cin <= 7U);

    for (unsigned int j = 0U; j < length; j++)
      reconstructed[count++] = packet[j + 1U];
  }

  assert(count == 202U);
  assert(reconstructed[0] == 0xf0U);

  for (unsigned int i = 0U; i < 200U; i++)
    assert(reconstructed[i + 1U] == (uint8_t)(i & 0x7fU));

  assert(reconstructed[201] == 0xf7U);
}

static void test_claimed_sysex(void) {
  uint32_t claim = 0U;
  uint32_t second = 0U;
  unsigned int offset = transmitted_length;

  assert(midi_output_sysex_claim(1U, 1U, &claim));
  assert(claim != 0U);

  // Raw diagnostics cannot mutate a claimed stream.
  assert(!midi_output_sysex_begin(1U, 0U));
  assert(!midi_output_sysex_write(1U, 0x7dU));
  assert(!midi_output_sysex_end(1U));
  assert(!midi_output_sysex_cancel(1U));

  assert(!midi_output_sysex_write_claim(
      1U, claim + 1U, 0x7dU));

  // One complete SysEx fragment is accepted.
  assert(midi_output_sysex_write_claim(1U, claim, 0x7dU));
  assert(midi_output_sysex_write_claim(1U, claim, 0x01U));

  // Cancellation must enqueue a standalone EOX,
  // following the fragment on the same cable.
  assert(midi_output_sysex_cancel_claim(1U, claim));

  assert(!midi_output_sysex_end_claim(1U, claim));
  assert(!midi_output_sysex_write_claim(1U, claim, 0x02U));

  midi_output_service();

  const uint8_t expected[8] = {
    0x14U, 0xf0U, 0x7dU, 0x01U,
    0x15U, 0xf7U, 0x00U, 0x00U
  };

  assert(transmitted_length == offset + sizeof(expected));
  assert(memcmp(transmitted + offset,
      expected, sizeof(expected)) == 0);

  complete();

  // Cancellation before the first complete
  // fragment produces no USB-MIDI packet.
  offset = transmitted_length;

  assert(midi_output_sysex_claim(1U, 0U, &second));
  assert(second != claim);

  assert(midi_output_sysex_write_claim(
      1U, second, 0x7dU));

  assert(midi_output_sysex_cancel_claim(1U, second));

  midi_output_service();
  assert(transmitted_length == offset);

  // The raw diagnostic API still works when no
  // claimed session is active.
  assert(midi_output_sysex_begin(1U, 0U));
  assert(!midi_output_sysex_claim(1U, 0U, &second));
  assert(midi_output_sysex_cancel(1U));

  midi_output_info_t info = {0};

  assert(midi_output_get_info(1U, &info));
  assert(!info.sysex_active);
  assert(info.sysex_claim == 0U);
  assert(info.sysex_cancelled >= 3U);

  // Fill the entire output FIFO with SysEx
  // fragments, then exercise retryable cancellation.
  uint32_t full_claim = 0U;

  assert(midi_output_sysex_claim(1U, 0U, &full_claim));

  // F0 + 191 payload bytes = 64 full fragments.
  for (unsigned int i = 0U; i < 191U; i++) {
    assert(midi_output_sysex_write_claim(
        1U, full_claim, (uint8_t)(i & 0x7fU)));
  }

  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == MIDI_OUTPUT_QUEUE_CAPACITY);
  assert(info.sysex_fragments == MIDI_OUTPUT_QUEUE_CAPACITY);

  assert(!midi_output_sysex_cancel_claim(1U, full_claim));

  // Failed cancellation preserves ownership.
  assert(midi_output_get_info(1U, &info));
  assert(info.sysex_active);
  assert(info.sysex_claim == full_claim);

  midi_output_service();
  assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);
  complete();

  // At least one batch has been confirmed,
  // making space for the terminating F7.
  assert(midi_output_sysex_cancel_claim(1U, full_claim));

  for (unsigned int i = 0U; i < 10U; i++) {
    assert(midi_output_get_info(1U, &info));

    if (info.queue_depth == 0U)
      break;

    midi_output_service();
    assert(hw.state == XHCI_BULK_OUT_ASYNC_PENDING);
    complete();
  }

  assert(midi_output_get_info(1U, &info));
  assert(info.queue_depth == 0U);
  assert(info.in_flight == 0U);
  assert(!info.sysex_active);
}

static void test_usb_midi_decode(void) {
  const uint8_t packet[4] = {
    0x1bU, 0xbfU, 0x66U, 0x21U
  };

  usb_midi_event_t event = {0};

  assert(usb_midi_decode_packet(packet, &event));
  assert(event.cable == 1U);
  assert(event.cin == 0x0bU);
  assert(event.length == 3U);
  assert(event.data[0] == 0xbfU);
  assert(event.data[1] == 0x66U);
  assert(event.data[2] == 0x21U);

  const uint8_t reserved[4] = {
    0x10U, 0x90U, 0x3cU, 0x40U
  };

  assert(!usb_midi_decode_packet(reserved, &event));
}

static void check_losses(uint64_t unsent, uint64_t unconfirmed,
    uint32_t events) {
  midi_output_loss_info_t losses = {0};
  assert(midi_output_loss_get_info(&losses));
  assert(losses.not_submitted == unsent);
  assert(losses.submitted_unconfirmed == unconfirmed);
  assert(losses.loss_events == events);
  assert(losses.invariant_violations == 0U);
}

static void replace_generation(uint32_t generation) {
  live.generation = generation;
  live.state = USB_DEVICE_ACTIVE;
  // The USB lifecycle has retired the old endpoint. Reusing this same
  // slot ID must not let MIDI reap the new generation as an old transfer.
  hw = (xhci_bulk_out_async_info_t){0};
  assert(midi_output_start_generation(generation));
}

static void queue_messages(uint32_t generation, unsigned int count) {
  midi_short_message_t message = {
    .status = 0x90U, .data = {60U, 100U}, .length = 3U
  };
  for (unsigned int i = 0U; i < count; i++)
    assert(midi_output_enqueue_short(generation, 0U, &message));
}

static void test_disconnect_losses(void) {
  midi_output_info_t info = {0};
  midi_output_loss_info_t losses = {0};
  // Generation 1's disconnect succeeded; generation 2's uncertain
  // transfer ended in failure. These are boot-lifetime cumulative counts.
  check_losses(0U, 1U, 1U);

  replace_generation(3U);
  queue_messages(3U, MIDI_OUTPUT_BATCH_PACKETS + 3U);
  midi_output_service();
  unsigned int started = transfers_started;
  assert(midi_output_get_info(3U, &info));
  assert(info.in_flight == MIDI_OUTPUT_BATCH_PACKETS);
  live.state = USB_DEVICE_DISCONNECTING;
  for (unsigned int i = 0U; i < 3U; i++)
    midi_output_service();
  assert(transfers_started == started);
  check_losses(0U, 1U, 1U); // No final classification while pending.
  complete();
  assert(midi_output_get_info(3U, &info));
  assert(info.state == MIDI_OUTPUT_DISCONNECTED);
  assert(info.confirmed == MIDI_OUTPUT_BATCH_PACKETS);
  assert(info.in_flight == 0U && info.queue_depth == 3U);
  assert(info.accepted == info.confirmed + info.queue_depth);
  check_losses(3U, 1U, 2U); // Only the never-submitted tail was lost.
  assert(midi_output_loss_get_info(&losses));
  assert(losses.last_generation == 3U);
  assert(losses.last_not_submitted == 3U);
  assert(losses.last_submitted_unconfirmed == 0U);
  midi_output_service();
  assert(transfers_started == started);
  check_losses(3U, 1U, 2U);
  // Merely observing ACTIVE again cannot authorise the disconnected output.
  live.state = USB_DEVICE_ACTIVE;
  midi_output_service();
  assert(transfers_started == started);
  assert(!midi_output_start_generation(3U));
  check_losses(3U, 1U, 2U);

  replace_generation(4U);
  check_losses(3U, 1U, 2U); // Recycling must not count that tail twice.
  queue_messages(4U, MIDI_OUTPUT_BATCH_PACKETS + 2U);
  midi_output_service();
  started = transfers_started;
  live.state = USB_DEVICE_DISCONNECTING;
  midi_output_service();
  check_losses(3U, 1U, 2U);
  hw.state = XHCI_BULK_OUT_ASYNC_FAILED;
  hw.result = XHCI_BULK_OUT_TRANSFER_FAILED;
  midi_output_service();
  assert(midi_output_get_info(4U, &info));
  assert(info.state == MIDI_OUTPUT_DISCONNECTED);
  assert(info.confirmed == 0U && info.in_flight == 0U);
  assert(info.failures == 1U);
  check_losses(5U, 17U, 3U);
  assert(midi_output_loss_get_info(&losses));
  assert(losses.last_generation == 4U);
  assert(losses.last_error == XHCI_BULK_OUT_TRANSFER_FAILED);
  assert(losses.last_not_submitted == 2U);
  assert(losses.last_submitted_unconfirmed == MIDI_OUTPUT_BATCH_PACKETS);
  midi_output_service();
  assert(transfers_started == started);
  check_losses(5U, 17U, 3U);

  replace_generation(5U);
  check_losses(5U, 17U, 3U);
  queue_messages(5U, MIDI_OUTPUT_BATCH_PACKETS + 4U);
  midi_output_service();
  started = transfers_started;
  live.state = USB_DEVICE_DISCONNECTING;
  midi_output_service();
  // UNCERTAIN is still an unresolved transfer, not a terminal result.
  hw.state = XHCI_BULK_OUT_ASYNC_UNCERTAIN;
  hw.result = XHCI_BULK_OUT_TIMEOUT;
  for (unsigned int i = 0U; i < 3U; i++)
    midi_output_service();
  assert(midi_output_get_info(5U, &info));
  assert(info.confirmed == 0U && info.in_flight == MIDI_OUTPUT_BATCH_PACKETS);
  assert(transfers_started == started);
  check_losses(5U, 17U, 3U);
  // Recycle without another service pass: losing the old record must
  // account for both the unresolved batch and its unsent tail exactly once.
  replace_generation(6U);
  assert(!midi_output_get_info(5U, &info));
  check_losses(9U, 33U, 4U);
  assert(midi_output_loss_get_info(&losses));
  assert(losses.last_generation == 5U);
  assert(losses.last_not_submitted == 4U);
  assert(losses.last_submitted_unconfirmed == MIDI_OUTPUT_BATCH_PACKETS);
  midi_output_service();
  assert(transfers_started == started);
  check_losses(9U, 33U, 4U);

  // No service call before disconnect: all packets are never submitted.
  queue_messages(6U, 5U);
  live.state = USB_DEVICE_DISCONNECTING;
  midi_output_service();
  assert(midi_output_get_info(6U, &info));
  assert(info.confirmed == 0U && info.in_flight == 0U);
  assert(info.queue_depth == 5U && transfers_started == started);
  check_losses(14U, 33U, 5U);
  midi_output_service();
  replace_generation(7U);
  check_losses(14U, 33U, 5U);
  assert(midi_output_loss_get_info(&losses));
  assert(losses.last_generation == 6U);
  assert(losses.last_not_submitted == 5U);
  assert(losses.last_submitted_unconfirmed == 0U);

  // The managed generation can disappear before the output record is
  // replaced. Never inspect/reap the reused slot as if it were still old.
  queue_messages(7U, 2U);
  midi_output_service();
  started = transfers_started;
  live.generation = 8U;
  hw.state = XHCI_BULK_OUT_ASYNC_COMPLETE;
  midi_output_service();
  assert(midi_output_get_info(7U, &info));
  assert(info.state == MIDI_OUTPUT_DISCONNECTED && info.confirmed == 0U);
  assert(hw.state == XHCI_BULK_OUT_ASYNC_COMPLETE); // Was not reaped.
  assert(transfers_started == started);
  check_losses(14U, 35U, 6U);
  replace_generation(8U);
  midi_output_service();
  check_losses(14U, 35U, 6U);

  // A failed reap need not destroy the terminal evidence. Preserve the
  // batch until a subsequent reap succeeds, without restarting the service.
  replace_generation(9U);
  queue_messages(9U, MIDI_OUTPUT_BATCH_PACKETS + 1U);
  midi_output_service();
  started = transfers_started;
  fail_reap_once = true;
  complete();
  assert(midi_output_get_info(9U, &info));
  assert(info.state == MIDI_OUTPUT_FAILED);
  assert(info.confirmed == 0U && info.in_flight == MIDI_OUTPUT_BATCH_PACKETS);
  check_losses(14U, 35U, 6U);
  midi_output_service();
  assert(midi_output_get_info(9U, &info));
  assert(info.state == MIDI_OUTPUT_FAILED);
  assert(info.confirmed == MIDI_OUTPUT_BATCH_PACKETS && info.in_flight == 0U);
  assert(info.queue_depth == 1U && info.failures == 1U);
  assert(transfers_started == started);
  check_losses(15U, 35U, 7U);
  midi_output_service();
  replace_generation(10U);
  check_losses(15U, 35U, 7U);

  // If the old hardware record has disappeared, no result can be reaped.
  // Account before clearing in_flight, then ignore any later slot contents.
  queue_messages(10U, 3U);
  midi_output_service();
  started = transfers_started;
  live.state = USB_DEVICE_DISCONNECTING;
  hw_available = false;
  midi_output_service();
  assert(midi_output_get_info(10U, &info));
  assert(info.confirmed == 0U && info.in_flight == 0U);
  check_losses(15U, 38U, 8U);
  hw_available = true;
  hw.state = XHCI_BULK_OUT_ASYNC_COMPLETE;
  midi_output_service();
  assert(midi_output_get_info(10U, &info));
  assert(info.confirmed == 0U && transfers_started == started);
  check_losses(15U, 38U, 8U);
  replace_generation(11U);
  check_losses(15U, 38U, 8U);
}

int main(void) {
  test_usb_midi_decode();

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

  test_sysex();
  test_long_sysex();
  test_claimed_sysex();

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

  check_losses(0U, 0U, 0U);

  // Disconnect while a batch remains outstanding.
  live.state = USB_DEVICE_DISCONNECTING;
  midi_output_service();

  assert(midi_output_get_info(1U, &info));
  assert(info.state == MIDI_OUTPUT_DISCONNECTED);
  assert(info.queue_depth == 1U);
  assert(info.in_flight == 1U);
  check_losses(0U, 0U, 0U);

  // Its eventual definitive completion can still
  // be reconciled against the original generation.
  complete();

  assert(midi_output_get_info(1U, &info));
  assert(info.confirmed == 1U);
  assert(info.queue_depth == 0U);
  check_losses(0U, 0U, 0U);

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
  check_losses(0U, 0U, 0U);

  // Even a late terminal failure must not be
  // mistaken for successful delivery.
  hw.state = XHCI_BULK_OUT_ASYNC_FAILED;
  midi_output_service();

  assert(midi_output_get_info(2U, &info));
  assert(info.queue_depth == 1U);
  assert(info.confirmed == 0U);
  assert(info.in_flight == 0U);
  assert(info.failures == 1U);

  check_losses(0U, 1U, 1U);
  test_disconnect_losses();

  puts("MIDI output queue: PASS");
  return 0;
}
