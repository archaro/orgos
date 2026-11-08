#include <stdbool.h>
#include <stdint.h>

#include "page.h"
#include "timer.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi_test.h"
#include "xhci.h"

#define USB_MIDI_IN_RING_ENTRIES 255U

static usb_midi_in_stress_info_t in_stress;
static uint64_t in_stress_started;
static uint64_t in_stress_ring_base;
static unsigned int in_stress_first_index;
static uint8_t in_stress_slot_id;
static uint32_t in_stress_generation;

static bool usb_midi_in_stress_owner_matches(uint32_t generation,
    uint8_t slot_id, usb_device_t *owner) {

  if (!owner)
    return false;

  *owner = (usb_device_t){0};

  if (generation == 0U || slot_id == 0U)
    return false;

  usb_device_t by_generation = {0};
  usb_device_t by_slot = {0};
  // Both identities must resolve uniquely.
  if (!usb_get_device_by_generation( generation, &by_generation) ||
      !usb_get_device_by_slot(slot_id, &by_slot))
    return false;

  // Hardware slot IDs may be reused. A matching
  // slot without the original generation does
  // not establish ownership.
  if (by_generation.slot_id != slot_id || by_slot.slot_id != slot_id ||
      by_slot.generation != generation)
    return false;

  // The two lookups must also agree about the
  // physical connection.
  if (by_generation.root_port != by_slot.root_port ||
      by_generation.parent_slot != by_slot.parent_slot ||
      by_generation.parent_port != by_slot.parent_port)
    return false;

  *owner = by_generation;
  return true;
}

static void usb_midi_in_stress_fail(usb_midi_in_stress_error_t error) {
  in_stress.error = error;
  in_stress.state = USB_MIDI_IN_STRESS_FAILED;
  in_stress.elapsed_us = timer_now_us() - in_stress_started;
}

bool usb_midi_in_stress_reset(void) {
  // A running or uninitialised diagnostic cannot
  // be reset. The recorded owner must be known.
  if (in_stress.state == USB_MIDI_IN_STRESS_IDLE ||
      in_stress.state == USB_MIDI_IN_STRESS_RUNNING ||
      in_stress_generation == 0U || in_stress_slot_id == 0U)
    return false;

  usb_device_t device = {0};
  if (!usb_get_device_by_generation(in_stress_generation, &device))
    return false;

  // The original device must have completed
  // teardown and lost its hardware slot.
  if (device.state != USB_DEVICE_DETACHED ||
      device.slot_id != 0U || device.root_port == 0U ||
      device.parent_slot == 0U || device.parent_port == 0U)
    return false;

  // The enumeration attempt must have been
  // explicitly rearmed after recycling.
  usb_enum_info_t enumeration = {0};

  if (usb_get_enumeration_generation(in_stress_generation, &enumeration))
    return false;

  // Only the original physical connection must
  // be unowned. An unrelated active device does
  // not prevent resetting this diagnostic.
  xhci_connection_ownership_t ownership = XHCI_CONNECTION_UNCERTAIN;

  if (!xhci_downstream_connection_ownership( device.root_port,
      device.parent_slot, device.parent_port, &ownership))
    return false;

  if (ownership != XHCI_CONNECTION_UNOWNED)
    return false;

  // Reset diagnostic bookkeeping only.
  // No hardware resources are modified.
  in_stress = (usb_midi_in_stress_info_t){0};
  in_stress_started = 0U;
  in_stress_ring_base = 0U;
  in_stress_first_index = 0U;
  in_stress_slot_id = 0U;
  in_stress_generation = 0U;

  return true;
}

bool usb_midi_in_stress_start_generation(
    uint32_t generation) {

  if (!usb_service_available() ||
      in_stress.state != USB_MIDI_IN_STRESS_IDLE ||
      generation == 0U)
    return false;

  usb_device_t device = {0};

  if (!usb_get_device_by_generation(generation, &device) ||
      device.state != USB_DEVICE_ACTIVE ||
      device.slot_id == 0U)
    return false;

  // Confirm that generation and hardware-slot
  // lookups identify the same device.
  usb_device_t verified = {0};

  if (!usb_midi_in_stress_owner_matches(
      generation, device.slot_id, &verified))
    return false;

  usb_enum_info_t enumeration = {0};

  if (!usb_get_enumeration_generation(
          generation, &enumeration) ||
      enumeration.result != USB_ENUM_OK ||
      enumeration.slot_id != verified.slot_id ||
      enumeration.hub_port != verified.parent_port)
    return false;

  // Do not take over an existing receiver.
  xhci_bulk_in_stream_stats_t stream = {0};

  if (!xhci_bulk_in_stream_get_stats(
          verified.slot_id, &stream) ||
      stream.started)
    return false;

  if (xhci_bulk_in_stream_start(
          verified.slot_id) != XHCI_BULK_IN_ARM_OK)
    return false;

  in_stress = (usb_midi_in_stress_info_t){
    .state = USB_MIDI_IN_STRESS_RUNNING
  };

  in_stress_started = timer_now_us();
  in_stress_ring_base = 0U;
  in_stress_first_index = 0U;

  // Retain the exact incarnation which started
  // the receiver. Neither value may be replaced
  // by subsequent enumeration.
  in_stress_slot_id = verified.slot_id;
  in_stress_generation = verified.generation;

  return true;
}

void usb_midi_in_stress_service(void) {
  if (in_stress.state == USB_MIDI_IN_STRESS_IDLE)
    return;

  usb_device_t device = {0};

  bool owner_present = usb_midi_in_stress_owner_matches(
      in_stress_generation, in_stress_slot_id, &device);

  // A different device generation must never
  // inherit the result of this stress test.
  if (in_stress.state == USB_MIDI_IN_STRESS_RUNNING &&
      (!owner_present || device.state != USB_DEVICE_ACTIVE)) {
    usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_DISCONNECTED);
    return;
  }

  // Following disconnection or recycling, do
  // not inspect a slot which may have acquired
  // another hardware owner.
  if (!owner_present)
    return;

  // Once the diagnostic has finished, retain its
  // result but continue consuming queued transfers
  // belonging to this same device incarnation.
  if (in_stress.state != USB_MIDI_IN_STRESS_RUNNING) {
    xhci_bulk_in_info_t discarded;
    while (xhci_bulk_in_stream_read(in_stress_slot_id, &discarded))
      in_stress.discarded_transfers++;

    return;
  }

  xhci_bulk_in_stream_stats_t stream = {0};
  if (!xhci_bulk_in_stream_get_stats(in_stress_slot_id, &stream) ||
      stream.failed) {
    usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_STREAM_ERROR);
    return;
  }

  xhci_bulk_in_info_t info;
  while (xhci_bulk_in_stream_read(in_stress_slot_id, &info)) {
    in_stress.fault_transfer = in_stress.transfers + 1U;
    // Verify that each completion refers to the
    // transfer TRB which we actually submitted.
    if (info.event_parameter != info.transfer_trb ||
        (info.completion_code != 1U &&
         info.completion_code != 13U)) {
      usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_TRB_ERROR);
      return;
    }

    // The transfer ring occupies one aligned page,
    // with its final entry reserved for the Link TRB.
    if (in_stress.transfers == 0U) {
      in_stress_ring_base = info.transfer_trb & ~((uint64_t)PAGE_SIZE - 1ULL);
      in_stress_first_index = (unsigned int)
          ((info.transfer_trb - in_stress_ring_base) / sizeof(xhci_trb_t));
      if (in_stress_first_index >= USB_MIDI_IN_RING_ENTRIES) {
        usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_TRB_ERROR);
        return;
      }
    }

    uint32_t absolute_index = in_stress_first_index + in_stress.transfers;

    uint64_t expected = in_stress_ring_base +
        (uint64_t)(absolute_index % USB_MIDI_IN_RING_ENTRIES) *
        sizeof(xhci_trb_t);

    in_stress.expected_trb = expected;
    in_stress.actual_trb = info.transfer_trb;

    if (info.transfer_trb != expected) {
      usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_TRB_ERROR);
      return;
    }

    // The xHCI event reports bytes remaining.
    // A USB-MIDI transfer must contain complete
    // four-byte event packets.
    if (info.requested != XHCI_BULK_IN_MAX_DATA ||
        info.received == 0U ||
        info.received > XHCI_BULK_IN_MAX_DATA ||
        (info.received % 4U) != 0U ||
        info.residual != info.requested - info.received) {
      usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_LENGTH_ERROR);
      return;
    }

    // A return to ring entry zero proves that
    // the controller traversed the Link TRB.
    if (in_stress.transfers != 0U &&
        absolute_index % USB_MIDI_IN_RING_ENTRIES == 0U)
      in_stress.wraps++;

    in_stress.transfers++;
    in_stress.bytes += info.received;

    // The Windows sender emits channel 16 Control
    // Changes, controller 102, with values cycling
    // through 0..127. Check every event in order.
    for (uint32_t i = 0U; i < info.received; i += 4U) {
      in_stress.fault_packet = in_stress.packets + 1U;
      if (in_stress.packets >= USB_MIDI_IN_STRESS_PACKETS ||
          info.data[i] != 0x0bU ||
          info.data[i + 1U] != 0xbfU ||
          info.data[i + 2U] != 0x66U ||
          info.data[i + 3U] != (uint8_t)(in_stress.packets & 0x7fU)) {
        usb_midi_in_stress_fail(USB_MIDI_IN_STRESS_PACKET_ERROR);
        return;
      }
      in_stress.packets++;
    }

    if (in_stress.packets == USB_MIDI_IN_STRESS_PACKETS) {
      in_stress.elapsed_us = timer_now_us() - in_stress_started;
      if (in_stress.transfers >=
              USB_MIDI_IN_STRESS_MIN_TRANSFERS && in_stress.wraps >= 2U) {
        in_stress.state = USB_MIDI_IN_STRESS_PASS;
      } else {
        in_stress.state = USB_MIDI_IN_STRESS_INCONCLUSIVE;
      }
      return;
    }
  }
}

bool usb_midi_in_stress_get_info(usb_midi_in_stress_info_t *result) {
  if (!result)
    return false;

  *result = in_stress;
  result->generation = in_stress_generation;
  result->slot_id = in_stress_slot_id;
  if (in_stress.state == USB_MIDI_IN_STRESS_RUNNING)
    result->elapsed_us = timer_now_us() - in_stress_started;

  return true;
}

