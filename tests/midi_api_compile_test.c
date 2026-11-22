// Compile only: an ordinary application needs no other header or test fixture
// to use every public operation. Runtime semantics live in midi_port_io_test.
#include "midi.h"

// Keep parser/transport implementation headers out of the application surface.
#if defined(MIDI_STREAM_EVENTS_PER_BYTE) || defined(USB_MIDI_MAX_CABLES) || \
    defined(USB_MIDI_RX_QUEUE_CAPACITY) || defined(MIDI_INPUT_QUEUE_CAPACITY)
#error "midi.h exposes internal parser or transport declarations"
#endif

void midi_api_compile_test(void) {
  unsigned int cursor = 0U;
  midi_port_info_t port = {0};
  midi_port_change_t change = {0};
  midi_port_lifecycle_info_t lifecycle = {0};
  midi_port_handle_t input = {0}, output = {0};
  midi_port_handle_info_t handle_info = {0};
  midi_port_event_t event = {0};
  midi_short_message_t message = {
    .status = 0xbfU, .data = {102U, 0U}, .length = 3U
  };
  midi_port_sysex_t session = {0};
  midi_port_sysex_info_t session_info = {0};
  const uint8_t bytes[] = {1U, 2U};
  size_t accepted = 0U;
  midi_status_t status = midi_port_next(&cursor, &port);
  (void)midi_status_name(status);
  (void)midi_status_name(MIDI_STATUS_DATA_LOST);
  (void)midi_port_get_info(port.id, &port);
  (void)midi_port_next_change(port.id, &change);
  (void)midi_port_lifecycle_get_info(port.id, &lifecycle);
  (void)midi_port_open(1U, &input);
  (void)midi_port_open(2U, &output);
  (void)midi_port_get_handle_info(&input, &handle_info);
  (void)midi_port_read(&input, &event);
  (void)midi_port_write(&output, &message);
  (void)midi_port_send_short(&output, &message);
  (void)midi_port_sysex_begin(&output, &session);
  (void)midi_port_sysex_write(&session, 0x7fU);
  (void)midi_port_sysex_write_bytes(&session, bytes, sizeof(bytes), &accepted);
  (void)midi_port_sysex_get_info(&session, &session_info);
  (void)midi_port_sysex_end(&session);
  (void)midi_port_sysex_cancel(&session);
  (void)midi_port_close(&input);
  (void)midi_port_close(&output);

  // Inspection/event value types and input loss fields are usable without
  // knowledge of the registry, stream parser or transport implementation.
  midi_port_key_t key = {.cable = 2U, .direction = MIDI_PORT_IN};
  midi_stream_event_t abort = {.type = MIDI_STREAM_SYSEX_ABORT};
  midi_stream_event_type_t type = MIDI_STREAM_SHORT;
  midi_port_state_t state = MIDI_PORT_BOUND;
  handle_info.cable_data_lost = true;
  handle_info.cable_overflow_packets = 1U;
  handle_info.cable_dropped_packets = 2U;
  (void)key;
  (void)abort;
  (void)type;
  (void)state;
}
