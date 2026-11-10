#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool enabled;
  uint32_t attempts;
  uint32_t started;
  uint32_t skipped;
  uint32_t failed;
  uint32_t last_generation;
} midi_auto_info_t;

// Controls admission of new automatic MIDI
// receivers. Disabling never tears down a
// previously started receiver.
void midi_auto_set_enabled(bool enabled);

bool midi_auto_get_info(midi_auto_info_t *info);

// Admit eligible device generations once.
// Called by the USB foreground service.
void midi_auto_service(void);
