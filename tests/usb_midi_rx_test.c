#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"
#include "xhci.h"

static usb_device_t live = {
  .root_port = 1U, .parent_slot = 1U, .parent_port = 1U,
  .slot_id = 2U, .generation = 1U, .state = USB_DEVICE_ACTIVE
};
static bool stream_started;
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
    .midi = {.in_found = true, .bulk_in_jacks = 1U}
  };
  return true;
}

bool xhci_bulk_in_stream_get_stats(uint8_t slot, xhci_bulk_in_stream_stats_t *info) {
  if (slot != live.slot_id)
    return false;
  *info = (xhci_bulk_in_stream_stats_t){.started = stream_started};
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

int main(void) {
  check_application_policy();
  check_diagnostic_policy();
  puts("USB MIDI receiver policies: PASS");
  return 0;
}
