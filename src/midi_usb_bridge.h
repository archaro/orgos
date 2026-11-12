#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_stream.h"
#include "usb_midi.h"

#define MIDI_USB_BRIDGE_MAX_EVENTS (3U * MIDI_STREAM_EVENTS_PER_BYTE)

typedef struct {
  uint8_t cables;
  midi_stream_parser_t parsers[USB_MIDI_MAX_CABLES];
} midi_usb_bridge_t;

typedef struct {
  uint8_t cable;
  uint8_t count;
  midi_stream_event_t events[MIDI_USB_BRIDGE_MAX_EVENTS];
} midi_usb_batch_t;

bool midi_usb_bridge_init(midi_usb_bridge_t *bridge, unsigned int cables);

// Prepare the resulting parser state and events
// without modifying the live parser.
bool midi_usb_bridge_prepare(const midi_usb_bridge_t *bridge,
    const usb_midi_event_t *packet, midi_stream_parser_t *next,
    midi_usb_batch_t *batch);

// Commit a successfully prepared and accepted batch.
bool midi_usb_bridge_commit(midi_usb_bridge_t *bridge,
    const midi_usb_batch_t *batch, const midi_stream_parser_t *next);

// Cancel one cable's partial message and report
// any unfinished SysEx.
bool midi_usb_bridge_cancel(midi_usb_bridge_t *bridge,
    unsigned int cable, midi_usb_batch_t *batch);
