#include <stdbool.h>
#include <stdint.h>

#include "midi_monitor.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"

// Monitoring state belongs to the managed
// record which owns each device incarnation.
static midi_monitor_info_t monitors[USB_MANAGED_DEVICE_CAPACITY];

static bool midi_monitor_accept(uint32_t generation,
    const usb_midi_event_t *event, void *context) {

  midi_monitor_info_t *monitor = context;
  if (!monitor || !event || !monitor->attached ||
      monitor->generation != generation ||
      event->cable >= USB_MIDI_MAX_CABLES)
    return false;

  monitor->events++;
  monitor->by_cable[event->cable]++;
  monitor->last = *event;
  monitor->have_last = true;

  return true;
}

bool midi_monitor_start(uint32_t generation) {
  if (generation == 0U)
    return false;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_device_t device = {0};
    if (!usb_get_device_at(i, &device) || device.generation != generation)
      continue;

    midi_monitor_info_t *monitor = &monitors[i];
    if (monitor->attached && monitor->generation == generation)
      return false;

    // Subscription verifies the live receiver
    // and its generation-specific ownership.
    if (!usb_midi_rx_subscribe( generation, midi_monitor_accept, monitor))
      return false;

    *monitor = (midi_monitor_info_t){
      .generation = generation,
      .attached = true
    };

    return true;
  }

  return false;
}

bool midi_monitor_stop(uint32_t generation) {
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    midi_monitor_info_t *monitor = &monitors[i];
    if (!monitor->attached || monitor->generation != generation)
      continue;

    if (!usb_midi_rx_unsubscribe(generation, midi_monitor_accept, monitor))
      return false;

    // Retain statistics for inspection after
    // detaching from the live receive stream.
    monitor->attached = false;
    return true;
  }

  return false;
}

bool midi_monitor_get_info(uint32_t generation, midi_monitor_info_t *info) {
  if (!info)
    return false;

  *info = (midi_monitor_info_t){0};

  if (generation == 0U)
    return false;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    if (monitors[i].generation != generation)
      continue;

    *info = monitors[i];
    return true;
  }

  return false;
}

