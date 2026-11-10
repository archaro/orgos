#include <stdbool.h>
#include <stdint.h>

#include "midi_auto.h"
#include "midi_monitor.h"
#include "usb.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"
#include "xhci.h"

// Automatic MIDI reception is opt-in while
// diagnostics still require direct stream access.
static midi_auto_info_t auto_state;

// A device generation is attempted at most once.
// Record indices may be reused, but generations
// are unique across device incarnations.
static uint32_t seen_generation[USB_MANAGED_DEVICE_CAPACITY];

void midi_auto_set_enabled(bool enabled) {
  auto_state.enabled = enabled;
}

bool midi_auto_get_info(midi_auto_info_t *info) {
  if (!info)
    return false;

  *info = auto_state;
  return true;
}

void midi_auto_service(void) {
  if (!auto_state.enabled || !usb_service_available())
    return;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_device_t device = {0};
    if (!usb_get_device_at(i, &device) || device.state != USB_DEVICE_ACTIVE ||
        device.generation == 0U || device.slot_id == 0U)
      continue;

    if (seen_generation[i] == device.generation)
      continue;

    // Only completed enumeration records may
    // authorise automatic MIDI reception.
    usb_enum_info_t enumeration = {0};
    if (!usb_get_enumeration_generation(device.generation, &enumeration) ||
        enumeration.result != USB_ENUM_OK ||
        enumeration.slot_id != device.slot_id ||
        enumeration.hub_port != device.parent_port)
      continue;

    // Mark this incarnation as inspected.
    // An ineligible device must not be
    // reconsidered on every service pass.
    seen_generation[i] = device.generation;

    if (!enumeration.midi.in_found || enumeration.midi.bulk_in_jacks == 0U ||
        enumeration.midi.bulk_in_jacks > USB_MIDI_MAX_CABLES)
      continue;

    auto_state.attempts++;
    auto_state.last_generation = device.generation;

    // Never displace a manually started
    // persistent receiver.
    if (usb_midi_rx_owns_generation(device.generation)) {
      auto_state.skipped++;
      continue;
    }

    // A diagnostic may own the xHCI stream
    // without owning a persistent receiver.
    xhci_bulk_in_stream_stats_t stream = {0};
    if (!xhci_bulk_in_stream_get_stats(device.slot_id, &stream)) {
      auto_state.failed++;
      continue;
    }

    if (stream.started) {
      auto_state.skipped++;
      continue;
    }

    // The receiver verifies generation, slot,
    // physical connection and MIDI configuration
    // before starting its hardware stream.
    if (!usb_midi_rx_start_generation(device.generation)) {
      auto_state.failed++;
      continue;
    }

    // The counting monitor is our temporary
    // normal-operation event consumer.
    // Later, application MIDI processing can
    // replace it without changing USB ownership.
    if (!midi_monitor_start(device.generation)) {
      auto_state.failed++;
      continue;
    }

    auto_state.started++;
  }
}

