#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Reuse the white-box host approach to exercise the actual command parser
// and service state. Unrelated MIDI diagnostics are discarded by the linker.
#include "../src/console_midi.c"

static midi_port_info_t current_port;
static midi_port_handle_t original;
static uint32_t current_token;
static midi_status_t read_status, inspect_status, service_status;
static bool defined;
static unsigned int retired, rx_depth, rx_staged;
static midi_port_event_t incoming[1024];
static unsigned int queued, consumed, read_calls;
static char output[32768];
static size_t output_length;
static bool in_service;

void console_puts(const char *text) {
  assert(!in_service);
  size_t length = strlen(text);
  assert(output_length + length < sizeof(output));
  memcpy(output + output_length, text, length + 1U);
  output_length += length;
}

void console_printf(const char *format, ...) {
  assert(!in_service);
  va_list args;
  va_start(args, format);
  int length = vsnprintf(output + output_length,
      sizeof(output) - output_length, format, args);
  va_end(args);
  assert(length >= 0 && (size_t)length < sizeof(output) - output_length);
  output_length += (size_t)length;
}

midi_status_t midi_port_get_info(unsigned int id, midi_port_info_t *info) {
  *info = (midi_port_info_t){0};
  if (id != 1U || !defined)
    return MIDI_STATUS_NOT_DEFINED;
  *info = current_port;
  return MIDI_STATUS_OK;
}

midi_status_t midi_port_lifecycle_get_info(unsigned int id,
    midi_port_lifecycle_info_t *info) {
  assert(id == 1U);
  *info = (midi_port_lifecycle_info_t){
    .opened_generation = current_token ? current_port.generation : 0U,
    .retired_depth = retired
  };
  return MIDI_STATUS_OK;
}

midi_status_t midi_port_get_handle_info(const midi_port_handle_t *handle,
    midi_port_handle_info_t *info) {
  *info = (midi_port_handle_info_t){.handle = *handle};
  if (inspect_status != MIDI_STATUS_OK)
    return inspect_status;
  info->status = service_status;
  if (!handle->active)
    info->status = MIDI_STATUS_NOT_OPEN;
  else if (!defined || current_port.state != MIDI_PORT_BOUND ||
      handle->generation != current_port.generation ||
      handle->definition_serial != current_port.definition_serial ||
      handle->open_token != current_token)
    info->status = MIDI_STATUS_DISCONNECTED;
  return MIDI_STATUS_OK;
}

bool midi_input_get_info(uint32_t generation, midi_input_info_t *info) {
  assert(generation == current_port.generation);
  *info = (midi_input_info_t){
    .attached = true, .generation = generation, .queue_depth = queued - consumed
  };
  return true;
}

bool usb_midi_rx_get_info(uint32_t generation, usb_midi_rx_info_t *info) {
  assert(generation == current_port.generation);
  *info = (usb_midi_rx_info_t){
    .generation = generation, .state = USB_MIDI_RX_RUNNING,
    .queue_depth = rx_depth, .staged = rx_staged
  };
  return true;
}

midi_status_t midi_port_read(const midi_port_handle_t *handle,
    midi_port_event_t *event) {
  // The verifier must never adopt the new console handle after a reopen.
  assert(handle->port == original.port && handle->generation == original.generation);
  assert(handle->definition_serial == original.definition_serial);
  assert(handle->open_token == original.open_token && handle->active);
  read_calls++;
  if (read_status != MIDI_STATUS_OK)
    return read_status;
  if (consumed == queued)
    return MIDI_STATUS_EMPTY;
  *event = incoming[consumed++];
  return MIDI_STATUS_OK;
}

static void command(const char *args) {
  output_length = 0U;
  output[0] = '\0';
  cmd_midi_port_verify(args);
}

static void service(void) {
  size_t before = output_length;
  unsigned int reads = read_calls;
  in_service = true;
  console_midi_service();
  in_service = false;
  assert(output_length == before);
  assert(read_calls - reads <= 32U);
}

static void fixture(void) {
  memset(console_handles, 0, sizeof(console_handles));
  port_verify = (midi_port_verify_t){0};
  current_port = (midi_port_info_t){
    .id = 1U, .key = {.direction = MIDI_PORT_IN},
    .state = MIDI_PORT_BOUND, .generation = 7U, .definition_serial = 5U
  };
  original = (midi_port_handle_t){
    .active = true, .port = 1U, .generation = 7U,
    .definition_serial = 5U, .open_token = 11U, .direction = MIDI_PORT_IN
  };
  console_handles[0] = original;
  current_token = original.open_token;
  read_status = inspect_status = service_status = MIDI_STATUS_OK;
  defined = true;
  retired = rx_depth = rx_staged = queued = consumed = read_calls = 0U;
  output_length = 0U;
  output[0] = '\0';
}

static void sequence(unsigned int first, unsigned int count) {
  assert(queued + count <= sizeof(incoming) / sizeof(incoming[0]));
  for (unsigned int i = 0U; i < count; i++) {
    incoming[queued++] = (midi_port_event_t){
      .port = 1U, .generation = original.generation,
      .event = {.type = MIDI_STREAM_SHORT,
        .message = {.status = 0xbfU, .data = {0x66U, (uint8_t)((first + i) & 0x7fU)},
          .length = 3U}}
    };
  }
}

static void test_automatic_sequence(void) {
  fixture();
  command("start 1 600 auto");
  assert(port_verify.active && port_verify.automatic && port_verify.running);
  service(); // EMPTY is normal and does not end the run.
  assert(port_verify.observed == 0U && port_verify.running);
  sequence(0U, 600U);
  for (unsigned int i = 0U; i < 18U; i++) {
    unsigned int reads = read_calls;
    service();
    assert(read_calls == reads + 32U);
    assert(port_verify.observed == (uint64_t)(i + 1U) * 32U);
  }
  service();
  assert(port_verify.observed == 600U && port_verify.mismatches == 0U);
  assert(port_verify.running && port_verify.first_position == 0U);
  command("status");
  assert(strstr(output, "Result     : RUNNING\n"));
  for (unsigned int i = 0U; i < 3U; i++)
    service();
  assert(port_verify.running && port_verify.observed == 600U);
  command("stop");
  assert(strstr(output, "Result     : PASS\n"));
  assert(!port_verify.running && port_verify.active);
  // Stop freezes both counts and the validity decision at that moment.
  current_token++;
  sequence(600U, 1U);
  unsigned int reads = read_calls;
  service();
  command("status");
  assert(strstr(output, "Result     : PASS\n"));
  assert(port_verify.observed == 600U && read_calls == reads);
  command("drain");
  assert(strstr(output, "Verification is stopped"));
  assert(read_calls == reads && consumed == 600U);
}

static void test_paced_and_extra(void) {
  fixture();
  command("start 1 600 auto");
  // Sender spacing is simulated by many empty foreground iterations.
  for (unsigned int i = 0U; i < 600U; i++) {
    for (unsigned int idle = 0U; idle < 10U; idle++)
      service();
    sequence(i, 1U);
    service();
  }
  assert(port_verify.observed == 600U && port_verify.running);
  sequence(600U, 2U);
  service();
  assert(port_verify.observed == 602U && port_verify.mismatches == 0U);
  assert(port_verify.running);
  command("stop");
  assert(strstr(output, "Result     : FAIL\n"));
}

static void test_missing_and_malformed(void) {
  fixture();
  command("start 1 5 auto");
  sequence(0U, 4U);
  service();
  assert(port_verify.observed == 4U && port_verify.mismatches == 0U);
  command("stop");
  assert(strstr(output, "Result     : INCOMPLETE\n"));

  fixture();
  command("start 1 5 auto");
  sequence(0U, 2U);
  sequence(3U, 2U); // The missing event shifts subsequent expected values.
  service();
  assert(port_verify.observed == 4U && port_verify.mismatches == 2U);
  assert(port_verify.first_position == 3U && port_verify.first_expected == 2U);
  assert(port_verify.first_actual.event.message.data[1] == 3U);
  command("stop");
  assert(strstr(output, "Result     : FAIL\n"));

  fixture();
  command("start 1 132 auto");
  sequence(0U, 132U);
  incoming[129U].event.message.data[1] = 42U;
  incoming[131U].event.type = MIDI_STREAM_SYSEX_DATA;
  incoming[131U].event.byte = 99U;
  for (unsigned int i = 0U; i < 5U; i++)
    service();
  assert(port_verify.mismatches == 2U && port_verify.first_position == 130U);
  assert(port_verify.first_expected == 1U); // Verify modulo-128 cycling.
  assert(port_verify.first_actual.event.type == MIDI_STREAM_SHORT);
  assert(port_verify.first_actual.event.message.data[1] == 42U);
  command("status");
  assert(strstr(output, "event 130 (1-based), expected CC 102 value 1"));
  assert(strstr(output, "message bf 66 2a"));

  // Reject wrong message shape, status, controller, value, and generation.
  fixture();
  command("start 1 5 auto");
  sequence(0U, 5U);
  incoming[0U].event.message.length = 2U;
  incoming[1U].event.message.status = 0xbeU;
  incoming[2U].event.message.data[0] = 0x65U;
  incoming[3U].event.message.data[1] = 128U;
  incoming[4U].generation++;
  service();
  assert(port_verify.observed == 5U && port_verify.mismatches == 5U);
  assert(port_verify.first_position == 1U && port_verify.first_expected == 0U);
  assert(port_verify.first_actual.event.message.length == 2U);
}

static void test_stop_reset_and_commands(void) {
  fixture();
  command("start 1 10 auto");
  sequence(0U, 5U);
  unsigned int reads = read_calls;
  command("drain 3");
  assert(strstr(output, "Manual drain unavailable"));
  assert(read_calls == reads && port_verify.observed == 0U);
  command("stop");
  assert(strstr(output, "Result     : INCOMPLETE\n"));
  service();
  assert(read_calls == reads && consumed == 0U);
  command("reset");
  assert(!port_verify.active && !port_verify.running && port_verify.observed == 0U);
  assert(queued == 5U && consumed == 0U && console_handles[0].active);
  service();
  assert(read_calls == reads);
  command("status");
  assert(strstr(output, "No receive verification armed"));

  fixture();
  command("start 1 10 auto");
  sequence(0U, 5U);
  command("reset"); // Reset must stop a still-running automatic consumer.
  service();
  assert(!port_verify.active && read_calls == 0U && consumed == 0U);
  assert(console_handles[0].active);
}

static void test_terminal_failures(void) {
  fixture();
  command("start 1 3 auto");
  sequence(0U, 3U);
  service();
  current_port.state = MIDI_PORT_UNBOUND;
  service();
  assert(!port_verify.running && port_verify.failure == VERIFY_DISCONNECTED);
  command("status");
  assert(strstr(output, "Result     : DISCONNECTED\n"));
  current_port.state = MIDI_PORT_BOUND;
  current_port.generation++;
  current_token++;
  unsigned int reads = read_calls;
  service();
  command("stop");
  assert(read_calls == reads && strstr(output, "Result     : DISCONNECTED\n"));
  assert(port_verify.handle.generation == original.generation);

  for (unsigned int scenario = 0U; scenario < 3U; scenario++) {
    fixture();
    command("start 1 3 auto");
    if (scenario == 0U) {
      current_token++; // Close/reopen on the same device generation.
      console_handles[0].open_token = current_token;
    } else if (scenario == 1U) {
      current_port.generation++;
    } else {
      defined = false; // Logical definition was removed.
    }
    service();
    assert(!port_verify.running && port_verify.failure == VERIFY_STALE_HANDLE);
    assert(read_calls == 0U && port_verify.handle.open_token == original.open_token);
    command("status");
    assert(strstr(output, "Result     : STALE HANDLE\n"));
  }
  const midi_status_t terminal[] = {MIDI_STATUS_NOT_OPEN, MIDI_STATUS_FAILED,
    MIDI_STATUS_INVALID_ARGUMENT};
  for (unsigned int i = 0U; i < sizeof(terminal) / sizeof(terminal[0]); i++) {
    fixture();
    command("start 1 3 auto");
    read_status = terminal[i];
    service();
    assert(!port_verify.running && port_verify.failure == VERIFY_READ_FAILED);
    assert(port_verify.terminal_status == terminal[i]);
    command("status");
    assert(strstr(output, "Result     : READ FAILED\n"));
    assert(strstr(output, midi_status_name(terminal[i])));
    read_status = MIDI_STATUS_OK;
    sequence(0U, 3U);
    unsigned int calls = read_calls;
    service();
    assert(read_calls == calls && port_verify.observed == 0U);
  }
  // Invalidation immediately before stop must prevent an exact-count PASS.
  fixture();
  command("start 1 3 auto");
  sequence(0U, 3U);
  service();
  current_token = 0U;
  command("stop");
  assert(strstr(output, "Result     : STALE HANDLE\n"));
}

static void test_manual_compatibility(void) {
  fixture();
  command("start 1 100");
  assert(port_verify.active && !port_verify.automatic && port_verify.running);
  sequence(0U, 100U);
  service();
  assert(read_calls == 0U && consumed == 0U);
  command("drain 3");
  assert(port_verify.observed == 3U && strstr(output, "Drained    : 3"));
  command("drain");
  assert(port_verify.observed == 67U && strstr(output, "Drained    : 64"));
  command("status");
  assert(strstr(output, "Result     : INCOMPLETE\n"));
  command("drain 64");
  assert(port_verify.observed == 100U && port_verify.mismatches == 0U);
  command("status");
  assert(strstr(output, "Result     : PASS\n")); // Existing manual behaviour.
  assert(port_verify.running);
  command("drain 0");
  assert(strstr(output, "Usage:"));
  command("drain 65");
  assert(strstr(output, "Usage:"));
  command("reset");
  assert(!port_verify.active && console_handles[0].active);
  service();
  command("drain");
  assert(strstr(output, "No receive verification armed"));
}

static void test_start_guards(void) {
  fixture();
  const char *invalid[] = {"start 0 1 auto", "start 33 1 auto", "start 1 0 auto",
    "start 1 1 auto junk", "start 1 1 nonsense"};
  for (unsigned int i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    command(invalid[i]);
    assert(!port_verify.active && strstr(output, "Usage:"));
  }
  console_handles[0].active = false;
  command("start 1 1 auto");
  assert(!port_verify.active && strstr(output, "Open a bound MIDI IN port first"));
  console_handles[0] = original;
  rx_staged = 1U;
  command("start 1 1 auto");
  assert(!port_verify.active && strstr(output, "Input queues must be empty first"));
  rx_staged = 0U;
  retired = 1U;
  command("start 1 1");
  assert(!port_verify.active && strstr(output, "Input queues must be empty first"));
  retired = 0U;
  command("start 1 10 auto");
  sequence(0U, 1U);
  command("start 1 20");
  assert(port_verify.running && port_verify.automatic && port_verify.expected_count == 10U);
}

int main(void) {
  test_automatic_sequence();
  test_paced_and_extra();
  test_missing_and_malformed();
  test_stop_reset_and_commands();
  test_terminal_failures();
  test_manual_compatibility();
  test_start_guards();
  puts("MIDI receive verification: PASS");
  return 0;
}
