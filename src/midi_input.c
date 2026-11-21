#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "midi_input.h"
#include "midi_usb_bridge.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"

_Static_assert(MIDI_INPUT_QUEUE_CAPACITY >= MIDI_USB_BRIDGE_MAX_EVENTS,
    "Application queue must fit a complete USB-MIDI batch");

typedef struct {
  midi_input_info_t info;
  midi_usb_bridge_t bridge;
  midi_input_event_t queue[MIDI_INPUT_QUEUE_CAPACITY];
  unsigned int head;
  uint16_t pending_abort_cables;
  bool stalled;
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

static bool input_has_space(midi_input_record_t *input, unsigned int needed) {
  if (needed <= MIDI_INPUT_QUEUE_CAPACITY - input->info.queue_depth) {
    if (needed != 0U)
      input->stalled = false;
    return true;
  }

  if (!input->stalled) {
    input->info.stalls++;
    input->stalled = true;
  }

  return false;
}

// Caller has reserved capacity for every event.
// The packet is inserted atomically in FIFO order.
static void input_enqueue(midi_input_record_t *input,
    const midi_usb_batch_t *batch) {

  for (unsigned int i = 0U; i < batch->count; i++) {
    unsigned int tail = (input->head +
        input->info.queue_depth) % MIDI_INPUT_QUEUE_CAPACITY;

    input->queue[tail] = (midi_input_event_t){
      .generation = input->info.generation,
      .cable = batch->cable,
      .event = batch->events[i]
    };
    input->info.queue_depth++;
  }

  if (input->info.queue_depth > input->info.queue_peak)
    input->info.queue_peak = input->info.queue_depth;

  input_count(input, batch);
}

// Once disconnected there is no incoming traffic.
// Keep terminal aborts outside the bounded FIFO so
// they cannot be lost when that FIFO is full.
static void input_cancel_all(midi_input_record_t *input) {
  for (unsigned int cable = 0U; cable < input->bridge.cables; cable++) {

    midi_usb_batch_t batch = {0};
    if (!midi_usb_bridge_cancel(&input->bridge, cable, &batch) ||
        batch.count == 0U)
      continue;

    uint16_t bit = (uint16_t)(1U << cable);

    if (!(input->pending_abort_cables & bit)) {
      input->pending_abort_cables |= bit;
      input->info.pending_aborts++;
    }

    input_count(input, &batch);
  }
}

static void input_detach(midi_input_record_t *input, bool lost) {
  input_cancel_all(input);
  input->info.attached = false;
  input->info.lost = lost;
  input->stalled = false;
}

static bool input_accept(uint32_t generation,
    const usb_midi_event_t *packet, void *context) {

  midi_input_record_t *input = context;

  if (!input || !packet || !input->info.attached ||
      input->info.generation != generation)
    return false;

  // Logical MIDI ports subscribe independently
  // to the virtual cables of one USB device.
  //
  // Do not let traffic on an unopened cable
  // consume another port's application queue.
  if (input->info.port_filtered && packet->cable < input->info.cables &&
      !(input->info.open_cables & (uint16_t)(1U << packet->cable))) {
    input->info.unclaimed_packets++;
    return true;
  }

  midi_stream_parser_t next = {0};
  midi_usb_batch_t batch = {0};

  if (!midi_usb_bridge_prepare(&input->bridge, packet, &next, &batch)) {

    // Invalid transport data consumes no application
    // capacity unless it aborts an unfinished SysEx.
    bool in_sysex = packet->cable < input->bridge.cables &&
        input->bridge.parsers[packet->cable].in_sysex;

    if (!input_has_space(input, in_sysex ? 1U : 0U))
      return false;

    if (packet->cable < input->bridge.cables) {
      midi_usb_batch_t cancelled = {0};

      if (!midi_usb_bridge_cancel(&input->bridge, packet->cable, &cancelled))
        return false;

      input_enqueue(input, &cancelled);
    }

    input->info.malformed++;
    return true;
  }

  // Nothing has been consumed or committed yet.
  // Declining preserves both parser and USB FIFO state.
  if (!input_has_space(input, batch.count))
    return false;

  if (!midi_usb_bridge_commit(&input->bridge, &batch, &next))
    return false;

  input->info.packets++;
  input_enqueue(input, &batch);

  return true;
}

bool midi_input_start_generation(uint32_t generation) {
  if (generation == 0U)
    return false;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {

    usb_device_t device = {0};

    if (!usb_get_device_at(i, &device) ||
        device.generation != generation)
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

    // A stopped receiver cannot be restarted while
    // its previous application events remain unread.
    if (input->info.attached || (input->info.generation == generation &&
         (input->info.queue_depth != 0U || input->info.pending_aborts != 0U)))
      return false;

    midi_input_record_t fresh = {0};

    if (!midi_usb_bridge_init(&fresh.bridge, rx.cables))
      return false;

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

bool midi_input_close_port_cable(uint32_t generation, unsigned int cable) {
  midi_input_record_t *input = input_find(generation);

  if (!input || !input->info.attached ||
      !input->info.port_filtered || cable >= input->info.cables)
    return false;

  uint16_t bit = (uint16_t)(1U << cable);
  if (!(input->info.open_cables & bit))
    return false;

  bool last = input->info.open_cables == bit;
  // Releasing the final cable must also release
  // the parser's exclusive receiver subscription.
  // Do this before changing application state.
  if (last && !usb_midi_rx_unsubscribe(generation, input_accept, input))
    return false;

  // Discard unread events for the closing cable.
  // Preserve the relative order of all other cables.
  unsigned int depth = input->info.queue_depth;
  unsigned int kept = 0U;

  for (unsigned int i = 0U; i < depth; i++) {
    unsigned int src = (input->head + i) % MIDI_INPUT_QUEUE_CAPACITY;

    midi_input_event_t event = input->queue[src];
    if (event.cable == cable)
      continue;

    unsigned int dest = (input->head + kept) % MIDI_INPUT_QUEUE_CAPACITY;

    input->queue[dest] = event;
    kept++;
  }

  input->info.closed_events += depth - kept;
  input->info.queue_depth = kept;

  // Closing a cable abandons any unfinished MIDI
  // message or SysEx stream belonging to it.
  midi_stream_parser_reset(&input->bridge.parsers[cable]);

  // A previously generated terminal abort is no
  // longer deliverable to the closing application.
  if (input->pending_abort_cables & bit) {
    input->pending_abort_cables &= (uint16_t)~bit;
    input->info.pending_aborts--;
  }

  input->info.open_cables &= (uint16_t)~bit;
  input->stalled = false;

  // The USB receiver stays running. With no
  // subscriber it resumes its idle-discard policy.
  if (last)
    input->info.attached = false;

  return true;
}

bool midi_input_stop_generation(uint32_t generation) {
  midi_input_record_t *input = input_find(generation);

  if (!input || !input->info.attached ||
      !usb_midi_rx_unsubscribe(generation, input_accept, input))
    return false;

  input_detach(input, false);
  return true;
}

bool midi_input_open_port_cable(uint32_t generation, unsigned int cable) {
  midi_input_record_t *input = input_find(generation);

  if (input && input->info.attached) {
    if (cable >= input->info.cables)
      return false;

    // A parser explicitly started by the caller
    // retains its original all-cable policy.
    if (input->info.port_filtered)
      input->info.open_cables |= (uint16_t)(1U << cable);

    return true;
  }

  usb_midi_rx_info_t rx = {0};

  if (!usb_midi_rx_get_info(generation, &rx) ||
      rx.state != USB_MIDI_RX_RUNNING || cable >= rx.cables)
    return false;

  // Create one parser for this device generation.
  // Additional logical ports share that parser.
  if (!midi_input_start_generation(generation))
    return false;

  input = input_find(generation);

  // Start with only the requested cable selected.
  input->info.port_filtered = true;
  input->info.open_cables = (uint16_t)(1U << cable);

  return true;
}

static bool input_read_matching(midi_input_record_t *input,
    int wanted_cable, midi_input_event_t *event) {

  unsigned int depth = input->info.queue_depth;

  for (unsigned int offset = 0U; offset < depth; offset++) {
    unsigned int index =
        (input->head + offset) % MIDI_INPUT_QUEUE_CAPACITY;

    if (wanted_cable >= 0 &&
        input->queue[index].cable != (unsigned int)wanted_cable)
      continue;

    *event = input->queue[index];

    if (offset == 0U) {
      input->head =
          (input->head + 1U) % MIDI_INPUT_QUEUE_CAPACITY;
    } else {
      // Remove an interior event without disturbing
      // the relative order of any other events.
      for (unsigned int j = offset; j + 1U < depth; j++) {
        unsigned int dest =
            (input->head + j) % MIDI_INPUT_QUEUE_CAPACITY;
        unsigned int src =
            (input->head + j + 1U) % MIDI_INPUT_QUEUE_CAPACITY;

        input->queue[dest] = input->queue[src];
      }
    }

    input->info.queue_depth--;
    return true;
  }

  // Terminal SysEx aborts follow the accepted
  // events belonging to their respective cables.
  for (unsigned int cable = 0U;
      cable < input->bridge.cables; cable++) {

    if (wanted_cable >= 0 &&
        cable != (unsigned int)wanted_cable)
      continue;

    uint16_t bit = (uint16_t)(1U << cable);

    if (!(input->pending_abort_cables & bit))
      continue;

    input->pending_abort_cables &= (uint16_t)~bit;
    input->info.pending_aborts--;

    *event = (midi_input_event_t){
      .generation = input->info.generation,
      .cable = (uint8_t)cable,
      .event = {.type = MIDI_STREAM_SYSEX_ABORT}
    };

    return true;
  }

  return false;
}

bool midi_input_read(uint32_t generation, midi_input_event_t *event) {

  if (!event)
    return false;

  *event = (midi_input_event_t){0};

  midi_input_record_t *input = input_find(generation);

  if (!input)
    return false;

  return input_read_matching(input, -1, event);
}

bool midi_input_read_cable(uint32_t generation,
    unsigned int cable, midi_input_event_t *event) {

  if (!event)
    return false;

  *event = (midi_input_event_t){0};

  midi_input_record_t *input = input_find(generation);

  if (!input || cable >= input->bridge.cables)
    return false;

  return input_read_matching(input, (int)cable, event);
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

    usb_midi_rx_unsubscribe(generation, input_accept, input);

    input_detach(input, true);
  }
}

bool midi_input_backpressure_selftest(void) {
  // Static storage avoids placing the entire
  // application queue on the kernel stack.
  static midi_input_record_t test;

  midi_input_event_t received = {0};

  usb_midi_event_t cc = {
    .cable = 0U,
    .cin = 0x0bU,
    .length = 3U,
    .data = {0xbfU, 0x66U, 0U}
  };

  test = (midi_input_record_t){0};
  test.info.generation = 1U;
  test.info.attached = true;
  test.info.cables = 1U;

  if (!midi_usb_bridge_init(&test.bridge, 1U))
    return false;

  // Fill the application queue completely.
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    cc.data[2] = (uint8_t)i;

    if (!input_accept(1U, &cc, &test))
      return false;
  }

  if (test.info.queue_depth != MIDI_INPUT_QUEUE_CAPACITY ||
      test.info.packets != MIDI_INPUT_QUEUE_CAPACITY ||
      test.info.stalls != 0U)
    return false;

  // The next packet must be refused without
  // modifying the parser or application queue.
  cc.data[2] = (uint8_t)MIDI_INPUT_QUEUE_CAPACITY;

  if (input_accept(1U, &cc, &test) || input_accept(1U, &cc, &test) ||
      test.info.stalls != 1U ||
      test.info.packets != MIDI_INPUT_QUEUE_CAPACITY)
    return false;

  // Free exactly one event and retry the packet.
  if (!input_read_matching(&test, -1, &received) ||
      received.event.type != MIDI_STREAM_SHORT ||
      received.event.message.data[1] != 0U)
    return false;

  if (!input_accept(1U, &cc, &test) ||
      test.info.queue_depth != MIDI_INPUT_QUEUE_CAPACITY ||
      test.info.stalls != 1U || test.stalled)
    return false;

  // All remaining events, including the retried
  // packet, must appear exactly once and in order.
  for (unsigned int i = 1U; i <= MIDI_INPUT_QUEUE_CAPACITY; i++) {
    if (!input_read_matching(&test, -1, &received) ||
        received.event.type != MIDI_STREAM_SHORT ||
        received.event.message.status != 0xbfU ||
        received.event.message.data[0] != 0x66U ||
        received.event.message.data[1] != (uint8_t)i)
      return false;
  }

  if (test.info.queue_depth != 0U ||
      test.info.packets != MIDI_INPUT_QUEUE_CAPACITY + 1U)
    return false;

  // Repeat with a three-event SysEx packet.
  // It must be accepted atomically, never partly.
  test = (midi_input_record_t){0};
  test.info.generation = 1U;
  test.info.attached = true;
  test.info.cables = 1U;

  if (!midi_usb_bridge_init(&test.bridge, 1U))
    return false;

  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY - 1U; i++) {
    cc.data[2] = (uint8_t)i;
    if (!input_accept(1U, &cc, &test))
      return false;
  }

  usb_midi_event_t begin = {
    .cable = 0U,
    .cin = 0x04U,
    .length = 3U,
    .data = {0xf0U, 0x11U, 0x22U}
  };

  // One free entry is insufficient for the
  // BEGIN event and two payload-byte events.
  if (input_accept(1U, &begin, &test) || test.bridge.parsers[0].in_sysex ||
      test.info.queue_depth != MIDI_INPUT_QUEUE_CAPACITY - 1U)
    return false;

  // Free enough space for the complete packet.
  for (unsigned int i = 0U; i < 2U; i++) {
    if (!input_read_matching(&test, -1, &received) ||
        received.event.type != MIDI_STREAM_SHORT ||
        received.event.message.data[1] != i)
      return false;
  }

  if (!input_accept(1U, &begin, &test) || !test.bridge.parsers[0].in_sysex ||
      test.info.queue_depth != MIDI_INPUT_QUEUE_CAPACITY)
    return false;

  // Verify all preceding short messages retain
  // their order after the previously rejected SysEx.
  for (unsigned int i = 2U; i < MIDI_INPUT_QUEUE_CAPACITY - 1U; i++) {

    if (!input_read_matching(&test, -1, &received) ||
        received.event.type != MIDI_STREAM_SHORT ||
        received.event.message.data[1] != i)
      return false;
  }

  if (!input_read_matching(&test, -1, &received) ||
      received.event.type != MIDI_STREAM_SYSEX_BEGIN)
    return false;

  if (!input_read_matching(&test, -1, &received) ||
      received.event.type != MIDI_STREAM_SYSEX_DATA ||
      received.event.byte != 0x11U)
    return false;

  if (!input_read_matching(&test, -1, &received) ||
      received.event.type != MIDI_STREAM_SYSEX_DATA ||
      received.event.byte != 0x22U)
    return false;

  usb_midi_event_t end = {
    .cable = 0U,
    .cin = 0x05U,
    .length = 1U,
    .data = {0xf7U, 0U, 0U}
  };

  if (!input_accept(1U, &end, &test) ||
      !input_read_matching(&test, -1, &received) ||
      received.event.type != MIDI_STREAM_SYSEX_END ||
      test.bridge.parsers[0].in_sysex ||
      test.info.queue_depth != 0U)
    return false;

  return true;
}

