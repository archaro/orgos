#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usb_midi.h"

typedef struct {
  uint32_t generation;
  bool attached;
  bool have_last;
  uint64_t events;
  uint64_t by_cable[USB_MIDI_MAX_CABLES];
  usb_midi_event_t last;
} midi_monitor_info_t;

bool midi_monitor_start(uint32_t generation);
bool midi_monitor_stop(uint32_t generation);

bool midi_monitor_get_info(uint32_t generation, midi_monitor_info_t *info);
