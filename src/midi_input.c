#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "midi_input.h"
#include "midi_usb_bridge.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"

_Static_assert(MIDI_INPUT_QUEUE_CAPACITY >= MIDI_USB_BRIDGE_MAX_EVENTS,
    "Application queue must fit a complete USB-MIDI batch");

_Static_assert(MIDI_INPUT_MAX_CABLES == USB_MIDI_MAX_CABLES,
    "Input and USB cable limits differ");

typedef struct {
  midi_stream_event_t event;
  uint64_t order;
} midi_input_queued_t;

typedef struct {
  midi_input_queued_t events[MIDI_INPUT_QUEUE_CAPACITY];
  unsigned int head;
  uint64_t abort_order;
} midi_input_fifo_t;

typedef struct {
  midi_input_info_t info;
  midi_usb_bridge_t bridge;
  midi_input_fifo_t fifo[MIDI_INPUT_MAX_CABLES];
  uint64_t next_order;
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

// Caller has reserved capacity for every event in this one cable FIFO.
static void input_enqueue(midi_input_record_t *input,
    const midi_usb_batch_t *batch) {
  midi_input_cable_info_t *info = &input->info.cable[batch->cable];
  midi_input_fifo_t *fifo = &input->fifo[batch->cable];
  for (unsigned int i = 0U; i < batch->count; i++) {
    unsigned int tail = (fifo->head + info->queue_depth) % MIDI_INPUT_QUEUE_CAPACITY;
    fifo->events[tail] = (midi_input_queued_t){
      .event = batch->events[i], .order = input->next_order++
    };
    info->queue_depth++;
    input->info.queue_depth++;
  }
  if (info->queue_depth > info->queue_peak)
    info->queue_peak = info->queue_depth;
  if (input->info.queue_depth > input->info.queue_peak)
    input->info.queue_peak = input->info.queue_depth;
  input_count(input, batch);
}

// A terminal ABORT has reserved storage outside the full FIFO. Its order
// follows all previously accepted events and precedes later cable traffic.
static void input_cancel_cable(midi_input_record_t *input, unsigned int cable) {
  midi_usb_batch_t batch = {0};
  if (!midi_usb_bridge_cancel(&input->bridge, cable, &batch) || batch.count == 0U)
    return;
  midi_input_cable_info_t *info = &input->info.cable[cable];
  if (!info->pending_abort) {
    info->pending_abort = true;
    input->info.pending_aborts++;
    input->fifo[cable].abort_order = input->next_order++;
    input_count(input, &batch);
  }
}

static void input_overflow(midi_input_record_t *input, unsigned int cable) {
  midi_input_cable_info_t *info = &input->info.cable[cable];
  info->overflow_packets++;
  info->dropped_packets++;
  info->data_lost = true;
  input->info.stalls++;
  // Cancel the committed parser, never the rejected packet's proposed state.
  input_cancel_cable(input, cable);
}

static void input_detach(midi_input_record_t *input, bool lost) {
  for (unsigned int cable = 0U; cable < input->bridge.cables; cable++)
    input_cancel_cable(input, cable);
  input->info.attached = false;
  input->info.lost = lost;
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

  if (packet->cable < input->info.cables &&
      input->info.cable[packet->cable].data_lost) {
    input->info.cable[packet->cable].dropped_packets++;
    return true; // Consumed, not delivered: never pin another cable behind it.
  }

  midi_stream_parser_t next = {0};
  midi_usb_batch_t batch = {0};
  if (!midi_usb_bridge_prepare(&input->bridge, packet, &next, &batch)) {
    input->info.malformed++;
    if (packet->cable < input->bridge.cables) {
      midi_input_cable_info_t *info = &input->info.cable[packet->cable];
      // A malformed packet may require one terminal SysEx ABORT. Saturation
      // must not block the receiver even on this error path.
      if (input->bridge.parsers[packet->cable].in_sysex &&
          info->queue_depth == MIDI_INPUT_QUEUE_CAPACITY)
        input_overflow(input, packet->cable);
      else {
        midi_usb_batch_t cancelled = {0};
        if (!midi_usb_bridge_cancel(&input->bridge, packet->cable, &cancelled))
          return false;
        input_enqueue(input, &cancelled);
      }
    }
    return true;
  }

  if (batch.count > MIDI_INPUT_QUEUE_CAPACITY -
      input->info.cable[batch.cable].queue_depth) {
    input_overflow(input, batch.cable);
    return true; // Drop the entire packet without committing its parser/events.
  }
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

    if (rx.cables == 0U || rx.cables > MIDI_INPUT_MAX_CABLES)
      return false;
    // Do not put sixteen FIFOs on the kernel stack. Subscribe first; failure
    // must leave the previous record intact. Subscription invokes no callback.
    midi_input_info_t previous = input->info;
    if (!usb_midi_rx_subscribe(generation, input_accept, input))
      return false;
    *input = (midi_input_record_t){0};
    if (previous.generation == generation) {
      // A diagnostic restart or last-port reopen keeps cumulative diagnostics,
      // but starts fresh parser, FIFO and fault state.
      input->info = previous;
      input->info.lost = false;
      input->info.port_filtered = false;
      input->info.open_cables = 0U;
      for (unsigned int cable = 0U; cable < MIDI_INPUT_MAX_CABLES; cable++) {
        input->info.cable[cable].data_lost = false;
        input->info.cable[cable].queue_depth = 0U;
        input->info.cable[cable].queue_peak = 0U;
        input->info.cable[cable].pending_abort = false;
      }
    }
    midi_usb_bridge_init(&input->bridge, rx.cables);
    input->info.generation = generation;
    input->info.cables = rx.cables;
    input->info.attached = true;

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

  midi_input_cable_info_t *info = &input->info.cable[cable];
  input->info.closed_events += info->queue_depth;
  input->info.queue_depth -= info->queue_depth;
  info->queue_depth = 0U;
  info->queue_peak = 0U;
  info->data_lost = false;
  input->fifo[cable] = (midi_input_fifo_t){0};
  midi_stream_parser_reset(&input->bridge.parsers[cable]);
  if (info->pending_abort) {
    info->pending_abort = false;
    input->info.pending_aborts--;
  }
  input->info.open_cables &= (uint16_t)~bit;

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
  int selected = -1;
  uint64_t earliest = UINT64_MAX;
  for (unsigned int cable = 0U; cable < input->bridge.cables; cable++) {
    if (wanted_cable >= 0 && cable != (unsigned int)wanted_cable)
      continue;
    midi_input_cable_info_t *info = &input->info.cable[cable];
    midi_input_fifo_t *fifo = &input->fifo[cable];
    if (info->queue_depth == 0U && !info->pending_abort)
      continue;
    uint64_t order = info->queue_depth ? fifo->events[fifo->head].order : fifo->abort_order;
    if (selected < 0 || order < earliest) {
      selected = (int)cable;
      earliest = order;
    }
  }
  if (selected < 0)
    return false;
  unsigned int cable = (unsigned int)selected;
  midi_input_cable_info_t *info = &input->info.cable[cable];
  midi_input_fifo_t *fifo = &input->fifo[cable];
  *event = (midi_input_event_t){.generation = input->info.generation, .cable = (uint8_t)cable};
  if (info->queue_depth != 0U) {
    event->event = fifo->events[fifo->head].event;
    fifo->head = (fifo->head + 1U) % MIDI_INPUT_QUEUE_CAPACITY;
    info->queue_depth--;
    input->info.queue_depth--;
  } else {
    event->event.type = MIDI_STREAM_SYSEX_ABORT;
    info->pending_abort = false;
    input->info.pending_aborts--;
  }
  return true;
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
  static midi_input_record_t test;
  test = (midi_input_record_t){0};
  test.info.generation = 1U;
  test.info.attached = true;
  test.info.cables = 2U;
  midi_usb_bridge_init(&test.bridge, 2U);
  usb_midi_event_t packet = {.cable = 0U, .cin = 0xbU,
    .length = 3U, .data = {0xbfU, 0x66U, 0U}};
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    packet.data[2] = (uint8_t)i;
    if (!input_accept(1U, &packet, &test))
      return false;
  }
  if (!input_accept(1U, &packet, &test) || !test.info.cable[0].data_lost)
    return false;
  midi_input_event_t event = {0};
  for (unsigned int i = 0U; i < MIDI_INPUT_QUEUE_CAPACITY; i++) {
    if (!input_read_matching(&test, 0, &event) || event.event.message.data[1] != i)
      return false;
  }
  if (!input_accept(1U, &packet, &test) || test.info.queue_depth != 0U)
    return false; // Space alone never recovers the fault.
  packet.cable = 1U;
  return input_accept(1U, &packet, &test) &&
      input_read_matching(&test, -1, &event) && event.cable == 1U &&
      test.info.cable[0].overflow_packets == 1U &&
      test.info.cable[0].dropped_packets == 2U && test.info.cable[1].dropped_packets == 0U;
}
