#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"

static uint8_t channel_data_length(uint8_t status) {
  uint8_t kind = status & 0xf0U;
  return kind == 0xc0U || kind == 0xd0U ? 1U : 2U;
}

static midi_stream_event_t *append(midi_stream_output_t *output,
    midi_stream_event_type_t type) {

  midi_stream_event_t *event = &output->events[output->count++];
  event->type = type;
  return event;
}

void midi_stream_parser_reset(midi_stream_parser_t *parser) {
  if (parser)
    *parser = (midi_stream_parser_t){0};
}

bool midi_stream_parser_cancel(midi_stream_parser_t *parser,
    midi_stream_output_t *output) {

  if (!parser || !output)
    return false;

  *output = (midi_stream_output_t){0};

  bool in_sysex = parser->in_sysex;
  midi_stream_parser_reset(parser);

  if (in_sysex)
    append(output, MIDI_STREAM_SYSEX_ABORT);

  return true;
}

bool midi_stream_parser_feed(midi_stream_parser_t *parser,
    uint8_t byte, midi_stream_output_t *output) {

  if (!parser || !output)
    return false;

  *output = (midi_stream_output_t){0};

  // Realtime is transparent to every other MIDI
  // message, including an unfinished SysEx.
  if (byte >= 0xf8U) {
    if (byte == 0xf9U || byte == 0xfdU)
      return true;

    midi_stream_event_t *event = append(output, MIDI_STREAM_SHORT);
    event->message.status = byte;
    event->message.length = 1U;
    return true;
  }

  if ((byte & 0x80U) != 0U) {
    if (parser->in_sysex) {
      parser->in_sysex = false;

      if (byte == 0xf7U) {
        append(output, MIDI_STREAM_SYSEX_END);
        return true;
      }

      // An unexpected non-realtime status ends
      // SysEx without EOX. Process that same status.
      midi_stream_event_t *event = append(output, MIDI_STREAM_SYSEX_ABORT);
      event->byte = byte;
    }

    // Every non-realtime status replaces the
    // current message and cancels running status.
    parser->active_status = 0U;
    parser->needed = 0U;
    parser->collected = 0U;
    parser->running_status = 0U;

    if (byte < 0xf0U) {
      parser->running_status = byte;
      parser->active_status = byte;
      parser->needed = channel_data_length(byte);
      return true;
    }

    if (byte == 0xf0U) {
      parser->in_sysex = true;
      append(output, MIDI_STREAM_SYSEX_BEGIN);
    } else if (byte == 0xf1U || byte == 0xf3U) {
      parser->active_status = byte;
      parser->needed = 1U;
    } else if (byte == 0xf2U) {
      parser->active_status = byte;
      parser->needed = 2U;
    } else if (byte == 0xf6U || byte == 0xf7U) {
      midi_stream_event_t *event = append(output, MIDI_STREAM_SHORT);
      event->message.status = byte;
      event->message.length = 1U;
    }

    // F4 and F5 are undefined and ignored.
    return true;
  }

  if (parser->in_sysex) {
    midi_stream_event_t *event = append(output, MIDI_STREAM_SYSEX_DATA);

    event->byte = byte;
    return true;
  }

  if (parser->active_status == 0U) {
    if (parser->running_status == 0U)
      return true;

    parser->active_status = parser->running_status;
    parser->needed = channel_data_length(parser->active_status);
    parser->collected = 0U;
  }

  parser->data[parser->collected++] = byte;

  if (parser->collected != parser->needed)
    return true;

  midi_stream_event_t *event = append(output, MIDI_STREAM_SHORT);
  event->message.status = parser->active_status;
  event->message.length = (uint8_t)(parser->needed + 1U);
  event->message.data[0] = parser->data[0];

  if (parser->needed == 2U)
    event->message.data[1] = parser->data[1];

  parser->active_status = 0U;
  parser->needed = 0U;
  parser->collected = 0U;

  return true;
}

