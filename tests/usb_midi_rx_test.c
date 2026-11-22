#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"
#include "midi_input.h"
#include "xhci.h"

static usb_device_t live = {
  .root_port = 1U, .parent_slot = 1U, .parent_port = 1U,
  .slot_id = 2U, .generation = 1U, .state = USB_DEVICE_ACTIVE
};
static unsigned int input_cables = 2U;
static bool stream_started;
static bool stream_failed;
static unsigned int stream_starts;
// Completed hardware transfers only. The real receiver implements all
// packet buffering, subscription ownership and discard/retention policy.
static xhci_bulk_in_info_t pending[8];
static unsigned int pending_head;
static unsigned int pending_depth;

typedef struct {
  bool accept;
  unsigned int count;
  uint8_t notes[8];
} capture_t;

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

bool usb_get_enumeration_generation(uint32_t generation, usb_enum_info_t *info) {
  if (generation != live.generation)
    return false;
  *info = (usb_enum_info_t){
    .result = USB_ENUM_OK, .slot_id = live.slot_id, .hub_port = live.parent_port,
    .midi = {.in_found = true, .bulk_in_jacks = input_cables}
  };
  return true;
}

bool xhci_bulk_in_stream_get_stats(uint8_t slot, xhci_bulk_in_stream_stats_t *info) {
  if (slot != live.slot_id)
    return false;
  *info = (xhci_bulk_in_stream_stats_t){.started = stream_started, .failed = stream_failed};
  return true;
}

xhci_bulk_in_arm_result_t xhci_bulk_in_stream_start(uint8_t slot) {
  assert(slot == live.slot_id && !stream_started);
  stream_starts++;
  stream_started = true;
  return XHCI_BULK_IN_ARM_OK;
}

bool xhci_bulk_in_stream_read(uint8_t slot, xhci_bulk_in_info_t *transfer) {
  assert(slot == live.slot_id && stream_started);
  if (pending_depth == 0U)
    return false;
  *transfer = pending[pending_head];
  pending_head = (pending_head + 1U) % 8U;
  pending_depth--;
  return true;
}

static void receive(unsigned int count, uint8_t first_note) {
  assert(count > 0U && count <= XHCI_BULK_IN_MAX_DATA / 4U);
  assert(pending_depth < 8U);
  unsigned int tail = (pending_head + pending_depth++) % 8U;
  pending[tail] = (xhci_bulk_in_info_t){
    .slot_id = live.slot_id, .transfer_trb = 0x1000U, .event_parameter = 0x1000U,
    .completion_code = 1U, .requested = XHCI_BULK_IN_MAX_DATA,
    .received = count * 4U, .residual = XHCI_BULK_IN_MAX_DATA - count * 4U
  };
  for (unsigned int i = 0U; i < count; i++) {
    pending[tail].data[i * 4U] = 0x09U;
    pending[tail].data[i * 4U + 1U] = 0x90U;
    pending[tail].data[i * 4U + 2U] = (uint8_t)(first_note + i);
    pending[tail].data[i * 4U + 3U] = 0x40U;
  }
}

static bool capture(uint32_t generation, const usb_midi_event_t *event,
    void *context) {
  capture_t *received = context;
  assert(generation == live.generation);
  if (!received->accept)
    return false;
  assert(received->count < sizeof(received->notes));
  received->notes[received->count++] = event->data[1];
  return true;
}

static void check_application_policy(void) {
  usb_midi_rx_info_t info = {0};
  usb_midi_event_t event = {0};
  capture_t old = {0}, fresh = {.accept = true};
  assert(usb_midi_rx_start_auto_generation(1U));
  assert(!usb_midi_rx_start_generation(1U));
  assert(!usb_midi_rx_start_auto_generation(1U));
  assert(stream_starts == 1U);

  // More traffic than fits in the FIFO must keep draining without a
  // subscriber. Each service pass has a bounded idle-discard budget.
  for (unsigned int i = 0U; i < 5U; i++)
    receive(16U, (uint8_t)(i * 16U));
  usb_midi_rx_service();
  assert(usb_midi_rx_get_info(1U, &info));
  assert(info.discard_unclaimed && info.state == USB_MIDI_RX_RUNNING);
  assert(info.unclaimed_dropped == 64U && pending_depth == 1U);
  usb_midi_rx_service();
  assert(usb_midi_rx_get_info(1U, &info));
  assert(info.queue_depth == 0U && info.staged == 0U && pending_depth == 0U);
  assert(info.unclaimed_dropped == 80U);

  // A backpressured old subscriber leaves a full FIFO plus a partly
  // delivered transfer. Resubscription must drop both before accepting
  // fresh traffic; only clearing the FIFO would leak the staged suffix.
  assert(usb_midi_rx_subscribe(1U, capture, &old));
  receive(15U, 10U);
  for (unsigned int i = 0U; i < 4U; i++)
    receive(16U, (uint8_t)(25U + i * 16U));
  usb_midi_rx_service();
  assert(usb_midi_rx_get_info(1U, &info));
  assert(info.queue_depth == 64U && info.staged == 15U);
  assert(!usb_midi_rx_read(1U, &event));
  assert(!usb_midi_rx_subscribe(1U, capture, &fresh));
  assert(!usb_midi_rx_unsubscribe(1U, capture, &fresh));
  assert(usb_midi_rx_unsubscribe(1U, capture, &old));
  assert(usb_midi_rx_subscribe(1U, capture, &fresh));
  assert(usb_midi_rx_get_info(1U, &info));
  assert(info.queue_depth == 0U && info.staged == 0U);
  assert(info.unclaimed_dropped == 159U);
  usb_midi_rx_service();
  assert(fresh.count == 0U);
  receive(1U, 99U);
  usb_midi_rx_service();
  assert(fresh.count == 1U && fresh.notes[0] == 99U);

  // Releasing the last consumer leaves the same receiver running and
  // discarding; a later open receives none of the idle-period packets.
  assert(usb_midi_rx_unsubscribe(1U, capture, &fresh));
  receive(2U, 100U);
  usb_midi_rx_service();
  assert(usb_midi_rx_get_info(1U, &info));
  assert(info.state == USB_MIDI_RX_RUNNING && info.discard_unclaimed);
  assert(info.unclaimed_dropped == 161U && info.queue_depth == 0U);
  assert(usb_midi_rx_subscribe(1U, capture, &fresh));
  usb_midi_rx_service();
  assert(fresh.count == 1U);
  receive(1U, 102U);
  usb_midi_rx_service();
  assert(fresh.count == 2U && fresh.notes[1] == 102U);
  assert(stream_starts == 1U);
  assert(usb_midi_rx_unsubscribe(1U, capture, &fresh));
}

static void check_diagnostic_policy(void) {
  usb_midi_rx_info_t info = {0};
  usb_midi_event_t event = {0};
  capture_t diagnostic = {.accept = true}, other = {.accept = true};
  live.state = USB_DEVICE_DISCONNECTING;
  usb_midi_rx_service();
  live.generation = 2U;
  live.state = USB_DEVICE_ACTIVE;
  stream_started = false;
  assert(usb_midi_rx_start_generation(2U));
  receive(2U, 10U);
  usb_midi_rx_service();
  assert(usb_midi_rx_get_info(2U, &info));
  assert(!info.discard_unclaimed && info.queue_depth == 2U);
  assert(info.unclaimed_dropped == 0U);
  assert(usb_midi_rx_read(2U, &event) && event.data[1] == 10U);
  // Diagnostics retain explicit FIFO ownership, including earlier data.
  assert(usb_midi_rx_subscribe(2U, capture, &diagnostic));
  assert(!usb_midi_rx_read(2U, &event));
  assert(!usb_midi_rx_subscribe(2U, capture, &other));
  assert(!usb_midi_rx_start_auto_generation(2U));
  usb_midi_rx_service();
  assert(diagnostic.count == 1U && diagnostic.notes[0] == 11U);
  assert(other.count == 0U && stream_starts == 2U);
  assert(!usb_midi_rx_unsubscribe(2U, capture, &other));
  assert(usb_midi_rx_unsubscribe(2U, capture, &diagnostic));
  receive(1U, 12U);
  usb_midi_rx_service();
  assert(usb_midi_rx_read(2U, &event) && event.data[1] == 12U);
  assert(usb_midi_rx_get_info(2U, &info));
  assert(!info.discard_unclaimed && info.unclaimed_dropped == 0U);

  // An independent hardware stream owner is never displaced either.
  live.state = USB_DEVICE_DISCONNECTING;
  usb_midi_rx_service();
  live.generation = 3U;
  live.state = USB_DEVICE_ACTIVE;
  assert(!usb_midi_rx_start_generation(3U));
  assert(!usb_midi_rx_start_auto_generation(3U));
  assert(stream_starts == 2U);
}

static uint32_t input_generation;

static void start_input_cables(uint32_t generation, bool filtered, unsigned int cables) {
  live.state = USB_DEVICE_DISCONNECTING;
  usb_midi_rx_service();
  midi_input_service();
  input_cables = cables;
  live.generation = input_generation = generation;
  live.state = USB_DEVICE_ACTIVE;
  stream_started = stream_failed = false;
  pending_head = pending_depth = 0U;
  assert(usb_midi_rx_start_auto_generation(generation));
  if (filtered) {
    for (unsigned int cable = 0U; cable < cables; cable++)
      assert(midi_input_open_port_cable(generation, cable));
  } else {
    assert(midi_input_start_generation(generation));
  }
}

static void start_input(uint32_t generation, bool filtered) {
  start_input_cables(generation, filtered, 2U);
}

static void packet(unsigned int cable, unsigned int cin,
    uint8_t first, uint8_t second, uint8_t third) {
  receive(1U, 0U);
  unsigned int tail = (pending_head + pending_depth - 1U) % 8U;
  pending[tail].data[0] = (uint8_t)((cable << 4U) | cin);
  pending[tail].data[1] = first;
  pending[tail].data[2] = second;
  pending[tail].data[3] = third;
}

static void short_packet(unsigned int cable, uint8_t value) {
  packet(cable, 0xbU, 0xbfU, 0x66U, value);
}

static void fill_input(unsigned int count) {
  for (unsigned int i = 0U; i < count; i++) {
    short_packet(0U, (uint8_t)i);
    usb_midi_rx_service();
  }
}

static midi_input_info_t input_info(void) {
  midi_input_info_t info = {0};
  assert(midi_input_get_info(input_generation, &info));
  return info;
}

static void check_cable_progress(void) {
  start_input(10U, true);
  unsigned int started = stream_starts;
  midi_input_event_t event = {0};
  for (unsigned int cycle = 0U; cycle < 3U; cycle++) {
    fill_input(MIDI_INPUT_QUEUE_CAPACITY);
    short_packet(0U, 100U); // This becomes the shared receiver's FIFO head.
    for (unsigned int i = 0U; i < 7U; i++)
      short_packet(1U, (uint8_t)(cycle * 8U + i));
    usb_midi_rx_service();
    usb_midi_rx_info_t rx = {0};
    assert(usb_midi_rx_get_info(input_generation, &rx));
    assert(rx.state == USB_MIDI_RX_RUNNING && rx.queue_depth == 0U && rx.staged == 0U);
    assert(pending_depth == 0U && stream_starts == started);
    midi_input_info_t info = input_info();
    assert(info.attached && !info.lost && info.malformed == 0U);
    assert(rx.rejected == 0U);
    assert(rx.events == info.packets + info.cable[0].dropped_packets);
    assert(info.cable[0].data_lost && !info.cable[1].data_lost);
    assert(info.cable[0].overflow_packets == cycle + 1U);
    assert(info.cable[0].dropped_packets == cycle * 2U + 1U);
    assert(info.cable[0].queue_depth == MIDI_INPUT_QUEUE_CAPACITY);
    assert(info.queue_depth == MIDI_INPUT_QUEUE_CAPACITY + 7U);
    // Consumption acknowledges USB FIFO ownership, not application delivery.
    assert(info.packets == (cycle + 1U) * (MIDI_INPUT_QUEUE_CAPACITY + 8U) - 1U);
    for (unsigned int i = 0U; i < 7U; i++) {
      assert(midi_input_read_cable(input_generation, 1U, &event));
      assert(event.event.message.data[1] == cycle * 8U + i);
    }
    assert(midi_input_read_cable(input_generation, 0U, &event));
    assert(event.event.message.data[1] == 0U);
    short_packet(0U, 101U); // Space becoming available is not resynchronisation.
    short_packet(1U, (uint8_t)(cycle * 8U + 7U));
    usb_midi_rx_service();
    assert(midi_input_read_cable(input_generation, 1U, &event));
    assert(event.event.message.data[1] == cycle * 8U + 7U);
    for (unsigned int i = 1U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
      assert(midi_input_read_cable(input_generation, 0U, &event));
      assert(event.event.message.data[1] == i);
    }
    assert(!midi_input_read_cable(input_generation, 0U, &event));
    info = input_info();
    assert(info.cable[0].data_lost && info.cable[0].dropped_packets == (cycle + 1U) * 2U);
    assert(info.cable[1].dropped_packets == 0U);
    assert(midi_input_open_port_cable(input_generation, 0U));
    assert(input_info().cable[0].data_lost); // Idempotent open does not clear loss.
    assert(midi_input_close_port_cable(input_generation, 0U));
    assert(midi_input_open_port_cable(input_generation, 0U));
    assert(!input_info().cable[0].data_lost);
  }
}

static void check_atomic_boundaries(void) {
  midi_input_event_t event = {0};
  start_input(11U, true);
  fill_input(MIDI_INPUT_QUEUE_CAPACITY - 3U);
  packet(0U, 4U, 0xf0U, 0x11U, 0x22U);
  usb_midi_rx_service();
  midi_input_info_t info = input_info();
  assert(!info.cable[0].data_lost && info.cable[0].queue_depth == MIDI_INPUT_QUEUE_CAPACITY);
  assert(info.sysex_begins == 1U && info.sysex_bytes == 2U);
  start_input(12U, true);
  fill_input(MIDI_INPUT_QUEUE_CAPACITY - 2U);
  packet(0U, 4U, 0xf0U, 0x11U, 0x22U);
  usb_midi_rx_service();
  info = input_info();
  assert(info.cable[0].data_lost && info.cable[0].queue_depth == MIDI_INPUT_QUEUE_CAPACITY - 2U);
  assert(info.cable[0].overflow_packets == 1U && info.cable[0].dropped_packets == 1U);
  assert(info.sysex_begins == 0U && info.sysex_bytes == 0U && info.pending_aborts == 0U);
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY - 2U; i++) {
    assert(midi_input_read(input_generation, &event));
    assert(event.event.type == MIDI_STREAM_SHORT && event.event.message.data[1] == i);
  }
  assert(!midi_input_read(input_generation, &event));
}

static void check_sysex_abort_order(void) {
  start_input(13U, true);
  midi_input_event_t event = {0};
  fill_input(MIDI_INPUT_QUEUE_CAPACITY - 3U);
  packet(0U, 4U, 0xf0U, 0x11U, 0x22U);
  short_packet(1U, 50U);
  usb_midi_rx_service();
  packet(0U, 5U, 0xf7U, 0U, 0U); // END cannot fit; it must never be fabricated.
  short_packet(1U, 51U);
  usb_midi_rx_service();
  midi_input_info_t info = input_info();
  assert(info.cable[0].data_lost && info.cable[0].pending_abort);
  assert(info.pending_aborts == 1U && info.sysex_aborts == 1U && info.sysex_ends == 0U);
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY - 3U; i++) {
    assert(midi_input_read(input_generation, &event));
    assert(event.cable == 0U && event.event.message.data[1] == i);
  }
  const midi_stream_event_type_t types[] = {MIDI_STREAM_SYSEX_BEGIN,
    MIDI_STREAM_SYSEX_DATA, MIDI_STREAM_SYSEX_DATA, MIDI_STREAM_SHORT,
    MIDI_STREAM_SYSEX_ABORT, MIDI_STREAM_SHORT};
  const unsigned int cables[] = {0U, 0U, 0U, 1U, 0U, 1U};
  for (unsigned int i = 0U; i < 6U; i++) {
    assert(midi_input_read(input_generation, &event));
    assert(event.event.type == types[i] && event.cable == cables[i]);
  }
  assert(!midi_input_read(input_generation, &event));
  assert(input_info().pending_aborts == 0U && input_info().cable[0].data_lost);
  packet(0U, 5U, 0xf7U, 0U, 0U);
  usb_midi_rx_service();
  assert(input_info().cable[0].dropped_packets == 2U && input_info().sysex_ends == 0U);

  // A malformed packet whose cancellation cannot fit must not pin the
  // shared receiver either; malformed and cable-overflow counts stay distinct.
  start_input(14U, true);
  fill_input(MIDI_INPUT_QUEUE_CAPACITY - 3U);
  packet(0U, 4U, 0xf0U, 0x11U, 0x22U);
  usb_midi_rx_service();
  packet(0U, 9U, 0x80U, 0U, 0U);
  short_packet(1U, 52U);
  usb_midi_rx_service();
  info = input_info();
  assert(info.malformed == 1U && info.cable[0].overflow_packets == 1U);
  assert(info.cable[0].pending_abort && info.sysex_aborts == 1U);
  assert(midi_input_close_port_cable(input_generation, 0U));
  assert(input_info().pending_aborts == 0U && input_info().closed_events == 64U);
  assert(midi_input_read(input_generation, &event) && event.cable == 1U);
  assert(event.event.message.data[1] == 52U);
}

static void check_independent_capacity_and_wrap(void) {
  start_input(17U, true);
  fill_input(MIDI_INPUT_QUEUE_CAPACITY);
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    short_packet(1U, (uint8_t)i);
    usb_midi_rx_service();
  }
  midi_input_info_t info = input_info();
  assert(info.queue_depth == 2U * MIDI_INPUT_QUEUE_CAPACITY);
  assert(!info.cable[0].data_lost && !info.cable[1].data_lost);
  midi_input_event_t event = {0};
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    assert(midi_input_read(input_generation, &event));
    assert(event.cable == 0U && event.event.message.data[1] == i);
  }
  // Wrap cable 0 while cable 1 retains older events. The global reader
  // must deliver every cable-1 event before the newer wrapped cable-0 data.
  fill_input(MIDI_INPUT_QUEUE_CAPACITY);
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    assert(midi_input_read(input_generation, &event));
    assert(event.cable == 1U && event.event.message.data[1] == i);
  }
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    assert(midi_input_read(input_generation, &event));
    assert(event.cable == 0U && event.event.message.data[1] == i);
  }
  assert(!midi_input_read(input_generation, &event));
}

static void check_diagnostic_loss_and_recycling(void) {
  start_input(15U, false);
  fill_input(MIDI_INPUT_QUEUE_CAPACITY);
  short_packet(0U, 100U);
  short_packet(1U, 90U);
  usb_midi_rx_service();
  midi_input_info_t info = input_info();
  assert(!info.port_filtered && info.cable[0].data_lost && !info.cable[1].data_lost);
  assert(!midi_input_close_port_cable(input_generation, 0U)); // Ownership is explicit.
  assert(midi_input_stop_generation(input_generation));
  midi_input_event_t event = {0};
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    assert(midi_input_read(input_generation, &event));
    assert(event.cable == 0U && event.event.message.data[1] == i);
  }
  assert(midi_input_read(input_generation, &event) && event.cable == 1U);
  assert(!midi_input_read(input_generation, &event));
  assert(midi_input_start_generation(input_generation));
  info = input_info();
  assert(!info.port_filtered && !info.cable[0].data_lost);
  assert(info.cable[0].overflow_packets == 1U && info.cable[0].dropped_packets == 1U);
  short_packet(0U, 91U);
  usb_midi_rx_service();
  assert(midi_input_read(input_generation, &event) && event.event.message.data[1] == 91U);
  // Real transport failure still detaches input; FIFO loss never masks it.
  stream_failed = true;
  usb_midi_rx_service();
  midi_input_service();
  assert(input_info().lost && !input_info().attached);
  start_input(16U, true);
  assert(!midi_input_get_info(15U, &info));
  info = input_info();
  assert(!info.cable[0].data_lost && info.cable[0].dropped_packets == 0U);
  assert(info.queue_depth == 0U && info.pending_aborts == 0U);
  assert(midi_input_backpressure_selftest());
}

// Each six-packet completion carries two messages per cable in a deliberately
// uneven order. Two successive completions are queued before each service pass.
static const unsigned int interleaved_cables[] = {0U, 1U, 2U, 1U, 0U, 2U};

static void receive_interleaved(unsigned int rounds, unsigned int next[3]) {
  assert(rounds > 0U);
  for (unsigned int round = 0U; round < rounds; round++) {
    receive(6U, 0U);
    unsigned int tail = (pending_head + pending_depth - 1U) % 8U;
    for (unsigned int i = 0U; i < 6U; i++) {
      unsigned int cable = interleaved_cables[i];
      assert(next[cable] < 128U);
      pending[tail].data[i * 4U] = (uint8_t)((cable << 4U) | 0xbU);
      pending[tail].data[i * 4U + 1U] = (uint8_t)(0xb0U + cable);
      pending[tail].data[i * 4U + 2U] = (uint8_t)(0x60U + cable);
      pending[tail].data[i * 4U + 3U] = (uint8_t)next[cable]++;
    }
    if (round % 2U == 1U || round + 1U == rounds)
      usb_midi_rx_service();
  }
}

static void expect_cc(const midi_input_event_t *event,
    unsigned int cable, unsigned int sequence) {
  assert(event->generation == input_generation && event->cable == cable);
  assert(event->event.type == MIDI_STREAM_SHORT);
  assert(event->event.message.length == 3U);
  assert(event->event.message.status == 0xb0U + cable);
  assert(event->event.message.data[0] == 0x60U + cable);
  assert(event->event.message.data[1] == sequence);
}

static void drain_cc(unsigned int cable, unsigned int first, unsigned int count) {
  midi_input_event_t event = {0};
  for (unsigned int i = 0U; i < count; i++) {
    assert(midi_input_read_cable(input_generation, cable, &event));
    expect_cc(&event, cable, first + i);
  }
}

static void expect_three_depths(unsigned int a, unsigned int b, unsigned int c) {
  midi_input_info_t info = input_info();
  assert(info.cables == 3U && info.attached && !info.lost);
  assert(info.open_cables == 7U && info.port_filtered);
  assert(info.malformed == 0U && info.pending_aborts == 0U);
  assert(info.cable[0].queue_depth == a && info.cable[1].queue_depth == b &&
      info.cable[2].queue_depth == c);
  assert(info.queue_depth == a + b + c);
}

static void expect_three_receiver(unsigned int starts, unsigned int packets,
    unsigned int drops, unsigned int transfers, bool faulted) {
  usb_midi_rx_info_t rx = {0};
  midi_input_info_t info = input_info();
  assert(usb_midi_rx_get_info(input_generation, &rx));
  assert(rx.cables == 3U && rx.state == USB_MIDI_RX_RUNNING && rx.discard_unclaimed);
  assert(rx.queue_depth == 0U && rx.staged == 0U && pending_depth == 0U);
  assert(rx.rejected == 0U && rx.unclaimed_dropped == 0U);
  assert(rx.events == packets && rx.transfers == transfers);
  assert(info.packets == packets - drops && info.short_messages == packets - drops);
  assert(stream_started && !stream_failed && stream_starts == starts);
  for (unsigned int cable = 0U; cable < 3U; cable++) {
    assert(info.cable[cable].data_lost == (cable == 0U && faulted));
    assert(info.cable[cable].overflow_packets == (cable == 0U && drops != 0U ? 1U : 0U));
    assert(info.cable[cable].dropped_packets == (cable == 0U ? drops : 0U));
  }
}

static void check_three_cable_interleaved_input(void) {
  start_input_cables(20U, true, 3U);
  unsigned int started = stream_starts;
  unsigned int next[3] = {0};
  midi_input_event_t event = {0};
  receive_interleaved(20U, next); // Queue 40 on every cable before any reads.
  expect_three_depths(40U, 40U, 40U);
  drain_cc(1U, 0U, 40U);
  assert(!midi_input_read_cable(input_generation, 1U, &event));
  expect_three_depths(40U, 0U, 40U);
  drain_cc(2U, 0U, 24U);
  expect_three_depths(40U, 0U, 16U);
  receive_interleaved(10U, next);
  expect_three_depths(60U, 20U, 36U);
  drain_cc(0U, 0U, 60U);
  expect_three_depths(0U, 20U, 36U);
  // All three tails now wrap while cables 1 and 2 retain older events.
  receive_interleaved(10U, next);
  expect_three_depths(20U, 40U, 56U);
  drain_cc(0U, 60U, 20U);
  drain_cc(1U, 40U, 40U);
  drain_cc(2U, 24U, 56U);
  expect_three_depths(0U, 0U, 0U);
  assert(!midi_input_read(input_generation, &event));
  expect_three_receiver(started, 240U, 0U, 40U, false);

  // Fresh generation: merge heads in actual arrival order, first with no
  // cable-specific reads, then skipping exactly the events already consumed.
  start_input_cables(21U, true, 3U);
  started = stream_starts;
  for (unsigned int mixed = 0U; mixed < 2U; mixed++) {
    unsigned int base = mixed * 40U;
    next[0] = next[1] = next[2] = base;
    receive_interleaved(20U, next);
    expect_three_depths(40U, 40U, 40U);
    if (mixed) {
      drain_cc(1U, base, 17U);
      drain_cc(2U, base, 9U);
      expect_three_depths(40U, 23U, 31U);
    }
    unsigned int seen[3] = {base, base, base};
    unsigned int read = 0U;
    for (unsigned int i = 0U; i < 120U; i++) {
      unsigned int cable = interleaved_cables[i % 6U];
      unsigned int sequence = seen[cable]++;
      if (mixed && ((cable == 1U && sequence < base + 17U) ||
          (cable == 2U && sequence < base + 9U)))
        continue;
      assert(midi_input_read(input_generation, &event));
      expect_cc(&event, cable, sequence);
      read++;
    }
    assert(read == (mixed ? 94U : 120U));
    expect_three_depths(0U, 0U, 0U);
    assert(!midi_input_read(input_generation, &event));
  }
  expect_three_receiver(started, 240U, 0U, 40U, false);
}

static void check_three_cable_overflow_isolation(void) {
  start_input_cables(22U, true, 3U);
  unsigned int started = stream_starts;
  unsigned int next[3] = {0};
  midi_input_event_t event = {0};
  // Fill with the same cable-distinguishable CC pattern used below.
  for (; next[0] < MIDI_INPUT_QUEUE_CAPACITY; next[0]++) {
    packet(0U, 0xbU, 0xb0U, 0x60U, (uint8_t)next[0]);
    usb_midi_rx_service();
  }
  expect_three_depths(64U, 0U, 0U);
  receive_interleaved(4U, next);
  expect_three_depths(64U, 8U, 8U);
  expect_three_receiver(started, 88U, 8U, 68U, true);
  drain_cc(1U, 0U, 8U); // Leave cable 2 untouched across repeated cable-1 drains.
  expect_three_depths(64U, 0U, 8U);
  receive_interleaved(4U, next);
  drain_cc(1U, 8U, 8U);
  expect_three_depths(64U, 0U, 16U);
  receive_interleaved(4U, next);
  drain_cc(2U, 0U, 24U); // Reverse: leave cable 1 unread now.
  expect_three_depths(64U, 8U, 0U);
  receive_interleaved(4U, next);
  drain_cc(2U, 24U, 8U);
  expect_three_depths(64U, 16U, 0U);
  receive_interleaved(2U, next);
  expect_three_depths(64U, 20U, 4U);
  drain_cc(0U, 0U, 64U); // Overflow never destroyed its accepted prefix.
  assert(!midi_input_read_cable(input_generation, 0U, &event));
  receive_interleaved(2U, next); // Free space alone must not recover cable 0.
  expect_three_depths(0U, 24U, 8U);
  expect_three_receiver(started, 184U, 40U, 84U, true);
  assert(input_info().stalls == 1U);
  assert(midi_input_close_port_cable(input_generation, 0U));
  assert(midi_input_open_port_cable(input_generation, 0U));
  expect_three_depths(0U, 24U, 8U);
  midi_input_info_t info = input_info();
  assert(!info.cable[0].data_lost && info.cable[0].overflow_packets == 1U);
  assert(info.cable[0].dropped_packets == 40U && info.closed_events == 0U);
  receive_interleaved(4U, next);
  expect_three_depths(8U, 32U, 16U);
  drain_cc(0U, 104U, 8U);
  drain_cc(1U, 16U, 32U);
  drain_cc(2U, 32U, 16U);
  expect_three_depths(0U, 0U, 0U);
  assert(!midi_input_read(input_generation, &event));
  // Active loss cleared, but historical loss counters and receiver survive.
  expect_three_receiver(started, 208U, 40U, 88U, false);
  assert(input_info().stalls == 1U);
}

int main(void) {
  check_application_policy();
  check_diagnostic_policy();
  check_cable_progress();
  check_atomic_boundaries();
  check_sysex_abort_order();
  check_independent_capacity_and_wrap();
  check_diagnostic_loss_and_recycling();
  puts("USB MIDI receiver policies: PASS");
  check_three_cable_interleaved_input();
  check_three_cable_overflow_isolation();
  puts("USB MIDI three-cable integration: PASS");
  return 0;
}
