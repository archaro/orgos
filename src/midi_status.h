#pragma once

// Application results: compare explicitly with MIDI_STATUS_OK.
// OK means accepted locally, never confirmation of external delivery.
typedef enum {
  MIDI_STATUS_OK,
  MIDI_STATUS_EMPTY,
  MIDI_STATUS_NOT_OPEN,
  MIDI_STATUS_DISCONNECTED,
  MIDI_STATUS_BUSY,
  MIDI_STATUS_QUEUE_FULL,
  MIDI_STATUS_FAILED,
  MIDI_STATUS_INVALID_ARGUMENT,
  MIDI_STATUS_NOT_DEFINED,
  MIDI_STATUS_AMBIGUOUS,
} midi_status_t;

static inline const char *midi_status_name(midi_status_t status) {
  switch (status) {
    case MIDI_STATUS_OK: return "OK";
    case MIDI_STATUS_EMPTY: return "EMPTY";
    case MIDI_STATUS_NOT_OPEN: return "NOT_OPEN";
    case MIDI_STATUS_DISCONNECTED: return "DISCONNECTED";
    case MIDI_STATUS_BUSY: return "BUSY";
    case MIDI_STATUS_QUEUE_FULL: return "QUEUE_FULL";
    case MIDI_STATUS_FAILED: return "FAILED";
    case MIDI_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case MIDI_STATUS_NOT_DEFINED: return "NOT_DEFINED";
    case MIDI_STATUS_AMBIGUOUS: return "AMBIGUOUS";
    default: return "UNKNOWN";
  }
}
