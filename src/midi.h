#pragma once

#include <stdint.h>
#include <stdbool.h>

// Channels are numbered 1-16 at the API boundary.
// Data bytes have values 0-127.

typedef struct {
  uint8_t data[3];
  uint8_t length;
} midi_message_t;

bool midi_note_on(midi_message_t *msg,
    unsigned int channel, unsigned int note, unsigned int velocity);
bool midi_note_off(midi_message_t *msg,
    unsigned int channel, unsigned int note, unsigned int velocity);
bool midi_control_change(midi_message_t *msg,
    unsigned int channel, unsigned int controller, unsigned int value);
bool midi_program_change(midi_message_t *msg,
    unsigned int channel, unsigned int program);

