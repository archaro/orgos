#include <stdbool.h>
#include <stdint.h>

#include "midi_short.h"

static uint8_t channel_data_length(uint8_t status) {
  uint8_t kind = status & 0xf0U;
  return kind == 0xc0U || kind == 0xd0U ? 1U : 2U;
}

void midi_short_parser_reset(midi_short_parser_t *parser) {
  if (parser)
    *parser = (midi_short_parser_t){0};
}

bool midi_short_parser_feed(midi_short_parser_t *parser,
    uint8_t byte, midi_short_message_t *message) {

  if (!message)
    return false;

  *message = (midi_short_message_t){0};

  if (!parser)
    return false;

  // Realtime does not interrupt running status,
  // partial messages or SysEx.
  if (byte >= 0xf8U) {
    // F9 and FD are reserved.
    if (byte == 0xf9U || byte == 0xfdU)
      return false;

    message->status = byte;
    message->length = 1U;
    return true;
  }

  if ((byte & 0x80U) != 0U) {
    bool was_sysex = parser->in_sysex;

    // Any non-realtime status cancels an incomplete
    // short message. System status also cancels
    // running status.
    parser->active_status = 0U;
    parser->needed = 0U;
    parser->collected = 0U;
    parser->in_sysex = false;

    if (byte < 0xf0U) {
      parser->running_status = byte;
      parser->active_status = byte;
      parser->needed = channel_data_length(byte);
      return false;
    }

    parser->running_status = 0U;

    if (byte == 0xf0U) {
      parser->in_sysex = true;
      return false;
    }

    if (byte == 0xf1U || byte == 0xf3U) {
      parser->active_status = byte;
      parser->needed = 1U;
      return false;
    }

    if (byte == 0xf2U) {
      parser->active_status = byte;
      parser->needed = 2U;
      return false;
    }

    // An EOX within SysEx will be handled by
    // the future SysEx processor. A standalone
    // EOX is a one-byte System Common message.
    if (byte == 0xf6U || (byte == 0xf7U && !was_sysex)) {
      message->status = byte;
      message->length = 1U;
      return true;
    }

    // F4/F5 are undefined; SysEx is deferred.
    return false;
  }

  if (parser->in_sysex)
    return false;

  // Running status starts a fresh channel message
  // when the previous one has completed.
  if (parser->active_status == 0U) {
    if (parser->running_status == 0U)
      return false;

    parser->active_status = parser->running_status;
    parser->needed = channel_data_length(parser->active_status);
    parser->collected = 0U;
  }

  parser->data[parser->collected++] = byte;

  if (parser->collected != parser->needed)
    return false;

  message->status = parser->active_status;
  message->length = (uint8_t)(parser->needed + 1U);
  message->data[0] = parser->data[0];

  if (parser->needed == 2U)
    message->data[1] = parser->data[1];

  parser->active_status = 0U;
  parser->needed = 0U;
  parser->collected = 0U;

  return true;
}
