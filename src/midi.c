#include "midi.h"

// MIDI channel voice messages.
// Status byte:
//   Upper nibble identifies the message type.
//   Lower nibble identifies channel 0-15.
// Our public API numbers channels 1-16, so we
// subtract one when constructing the status byte.

static bool valid_channel(unsigned int channel) {
  return channel >= 1 && channel <= 16;
}

static bool valid_data(unsigned int value) {
  return value <= 127;
}

static bool make_three_byte(midi_message_t *msg, unsigned int status, unsigned int channel, unsigned int a, unsigned int b) {
  if (!msg || !valid_channel(channel) || !valid_data(a) || !valid_data(b))
    return false;

  msg->data[0] = (uint8_t)(status | (channel - 1));
  msg->data[1] = (uint8_t)a;
  msg->data[2] = (uint8_t)b;
  msg->length = 3;

  return true;
}

bool midi_note_on(midi_message_t *msg, unsigned int channel, unsigned int note, unsigned int velocity) {
  return make_three_byte(msg, 0x90, channel, note, velocity);
}

bool midi_note_off(midi_message_t *msg, unsigned int channel, unsigned int note, unsigned int velocity) {
  return make_three_byte(msg, 0x80, channel, note, velocity);
}

bool midi_control_change(midi_message_t *msg, unsigned int channel, unsigned int controller, unsigned int value) {
  return make_three_byte(msg, 0xb0, channel, controller, value);
}

bool midi_program_change(midi_message_t *msg, unsigned int channel, unsigned int program) {
  if (!msg || !valid_channel(channel) || !valid_data(program))
    return false;

  msg->data[0] = (uint8_t)(0xc0 | (channel - 1));
  msg->data[1] = (uint8_t)program;
  msg->data[2] = 0;
  msg->length = 2;

  return true;
}

