#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"
#include "xhci.h"

#define MIDI_EVENTS_PER_TRANSFER (XHCI_BULK_IN_MAX_DATA / 4U)

_Static_assert(USB_MIDI_RX_QUEUE_CAPACITY >= MIDI_EVENTS_PER_TRANSFER,
    "MIDI receive queue must hold one complete transfer");

typedef struct {
  usb_midi_rx_info_t info;
  usb_midi_event_t queue[USB_MIDI_RX_QUEUE_CAPACITY];
  unsigned int head;
  usb_midi_rx_consumer_t consumer;
  void *consumer_context;
  usb_midi_event_t staged[MIDI_EVENTS_PER_TRANSFER];
  unsigned int staged_index;
  unsigned int staged_count;
  uint8_t root_port;
  uint8_t parent_slot;
  uint8_t parent_port;
} usb_midi_rx_record_t;

// One receiver per managed-device record.
// Record reuse never transfers an old generation's
// receiver state to the new incarnation.
static usb_midi_rx_record_t receivers[USB_MANAGED_DEVICE_CAPACITY];

static usb_midi_rx_record_t *rx_find(
    uint32_t generation, unsigned int *index) {

  if (generation == 0U)
    return NULL;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    if (receivers[i].info.generation == generation) {
      if (index)
        *index = i;

      return &receivers[i];
    }
  }

  return NULL;
}

static bool rx_owner_matches(unsigned int index,
    const usb_midi_rx_record_t *rx) {

  usb_device_t device = {0};
  usb_device_t by_slot = {0};
  usb_device_t by_connection = {0};

  if (!rx || !usb_get_device_at(index, &device) ||
      device.state != USB_DEVICE_ACTIVE ||
      device.generation != rx->info.generation ||
      device.slot_id == 0U ||
      device.slot_id != rx->info.slot_id ||
      device.root_port != rx->root_port ||
      device.parent_slot != rx->parent_slot ||
      device.parent_port != rx->parent_port)
    return false;

  if (!usb_get_device_by_slot(device.slot_id, &by_slot) ||
      by_slot.generation != device.generation ||
      by_slot.root_port != device.root_port ||
      by_slot.parent_slot != device.parent_slot ||
      by_slot.parent_port != device.parent_port)
    return false;

  if (!usb_get_device_by_connection(device.root_port,
          device.parent_slot, device.parent_port, &by_connection) ||
      by_connection.generation != device.generation ||
      by_connection.slot_id != device.slot_id)
    return false;

  return true;
}

bool usb_midi_rx_start_generation(uint32_t generation) {
  if (!usb_service_available() || generation == 0U)
    return false;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_device_t device = {0};

    if (!usb_get_device_at(i, &device) || device.generation != generation)
      continue;

    if (device.state != USB_DEVICE_ACTIVE || device.slot_id == 0U)
      return false;

    usb_midi_rx_record_t *rx = &receivers[i];

    // Never replace a running receiver or
    // restart a failed receiver in-place.
    if (rx->info.state == USB_MIDI_RX_RUNNING ||
        (rx->info.generation == generation &&
         rx->info.state != USB_MIDI_RX_IDLE))
      return false;

    usb_midi_rx_record_t identity = {
      .info = {
        .generation = generation,
        .slot_id = device.slot_id
      },
      .root_port = device.root_port,
      .parent_slot = device.parent_slot,
      .parent_port = device.parent_port
    };

    if (!rx_owner_matches(i, &identity))
      return false;

    usb_enum_info_t enumeration = {0};

    if (!usb_get_enumeration_generation(generation, &enumeration) ||
        enumeration.result != USB_ENUM_OK ||
        enumeration.slot_id != device.slot_id ||
        enumeration.hub_port != device.parent_port ||
        !enumeration.midi.in_found ||
        enumeration.midi.bulk_in_jacks == 0U ||
        enumeration.midi.bulk_in_jacks > USB_MIDI_MAX_CABLES)
      return false;

    // No other consumer may already own
    // this device's Bulk IN receiver.
    xhci_bulk_in_stream_stats_t stream = {0};

    if (!xhci_bulk_in_stream_get_stats(
            device.slot_id, &stream) || stream.started)
      return false;

    if (xhci_bulk_in_stream_start(device.slot_id) != XHCI_BULK_IN_ARM_OK)
      return false;

    *rx = (usb_midi_rx_record_t){0};
    rx->info.state = USB_MIDI_RX_RUNNING;
    rx->info.generation = generation;
    rx->info.slot_id = device.slot_id;
    rx->info.cables = enumeration.midi.bulk_in_jacks;
    rx->root_port = device.root_port;
    rx->parent_slot = device.parent_slot;
    rx->parent_port = device.parent_port;

    return true;
  }

  return false;
}

bool usb_midi_rx_owns_generation(uint32_t generation) {
  usb_midi_rx_record_t *rx = rx_find(generation, NULL);
  return rx && rx->info.state != USB_MIDI_RX_IDLE;
}

bool usb_midi_rx_get_info(uint32_t generation, usb_midi_rx_info_t *info) {
  if (!info)
    return false;

  *info = (usb_midi_rx_info_t){0};

  usb_midi_rx_record_t *rx = rx_find(generation, NULL);
  if (!rx)
    return false;

  *info = rx->info;
  info->staged = rx->staged_count - rx->staged_index;
  return true;
}

bool usb_midi_rx_read(uint32_t generation, usb_midi_event_t *event) {
  if (!event)
    return false;

  *event = (usb_midi_event_t){0};

  unsigned int index = 0U;
  usb_midi_rx_record_t *rx = rx_find(generation, &index);

    if (!rx || rx->info.state != USB_MIDI_RX_RUNNING ||
        !rx_owner_matches(index, rx) || rx->consumer ||
        rx->info.queue_depth == 0U)
      return false;

  *event = rx->queue[rx->head];
  rx->head = (rx->head + 1U) % USB_MIDI_RX_QUEUE_CAPACITY;
  rx->info.queue_depth--;

  return true;
}

bool usb_midi_rx_subscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context) {

  if (!consumer)
    return false;

  unsigned int index = 0U;
  usb_midi_rx_record_t *rx = rx_find(generation, &index);
  if (!rx || rx->info.state != USB_MIDI_RX_RUNNING ||
      rx->consumer || !rx_owner_matches(index, rx))
    return false;

  rx->consumer = consumer;
  rx->consumer_context = context;

  return true;
}

bool usb_midi_rx_unsubscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context) {

  usb_midi_rx_record_t *rx = rx_find(generation, NULL);
  if (!rx || !consumer || rx->consumer != consumer ||
      rx->consumer_context != context)
    return false;

  rx->consumer = NULL;
  rx->consumer_context = NULL;

  return true;
}

void usb_midi_rx_service(void) {
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_midi_rx_record_t *rx = &receivers[i];
    if (rx->info.state != USB_MIDI_RX_RUNNING)
      continue;

    // Disconnection or changed ownership makes
    // this receiver permanently ineligible.
    // Never inspect a recycled hardware slot.
    if (!rx_owner_matches(i, rx)) {
      rx->info.state = USB_MIDI_RX_DISCONNECTED;
      rx->info.queue_depth = 0U;
      rx->head = 0U;
      continue;
    }

    xhci_bulk_in_stream_stats_t stream = {0};

    if (!xhci_bulk_in_stream_get_stats(rx->info.slot_id, &stream) ||
        !stream.started || stream.failed) {
      rx->info.state = USB_MIDI_RX_FAILED;
      continue;
    }

    // Move staged events into the application FIFO.
    // Fetch another transfer only when the previous
    // transfer has been completely delivered.
    while (rx->info.queue_depth < USB_MIDI_RX_QUEUE_CAPACITY) {

      if (rx->staged_index == rx->staged_count) {
        xhci_bulk_in_info_t transfer = {0};

        if (!xhci_bulk_in_stream_read(rx->info.slot_id, &transfer))
          break;

        if (transfer.slot_id != rx->info.slot_id ||
            transfer.event_parameter != transfer.transfer_trb ||
            (transfer.completion_code != 1U &&
             transfer.completion_code != 13U) ||
            transfer.requested != XHCI_BULK_IN_MAX_DATA ||
            transfer.received > XHCI_BULK_IN_MAX_DATA ||
            transfer.residual != transfer.requested - transfer.received ||
            (transfer.received % 4U) != 0U) {
          rx->info.state = USB_MIDI_RX_FAILED;
          break;
        }

        rx->info.transfers++;
        rx->staged_index = 0U;
        rx->staged_count = 0U;

        // Decode the entire transfer into staging.
        // All events remain available even when
        // the application FIFO subsequently fills.
        for (uint32_t offset = 0U; offset < transfer.received; offset += 4U) {

          usb_midi_event_t event = {0};

          if (!usb_midi_decode_packet(&transfer.data[offset], &event) ||
              event.cable >= rx->info.cables) {
            rx->info.rejected++;
            continue;
          }

          rx->staged[rx->staged_count++] = event;
        }

        // A transfer containing no accepted events
        // does not occupy the staging buffer.
        if (rx->staged_count == 0U)
          continue;
      }

      unsigned int tail =
          (rx->head + rx->info.queue_depth) % USB_MIDI_RX_QUEUE_CAPACITY;

      rx->queue[tail] = rx->staged[rx->staged_index++];
      rx->info.queue_depth++;
      rx->info.events++;

      if (rx->info.queue_depth > rx->info.queue_peak)
        rx->info.queue_peak = rx->info.queue_depth;
    }

    // Deliver queued events to the exclusive
    // consumer in FIFO order.
    //
    // A declined event remains at the head.
    // The next service pass may retry it.
    if (rx->info.state != USB_MIDI_RX_RUNNING || !rx->consumer)
      continue;

    while (rx->info.queue_depth != 0U) {
      const usb_midi_event_t *event = &rx->queue[rx->head];
      if (!rx->consumer(rx->info.generation, event, rx->consumer_context))
        break;

      rx->head = (rx->head + 1U) % USB_MIDI_RX_QUEUE_CAPACITY;
      rx->info.queue_depth--;
    }
  }
}

