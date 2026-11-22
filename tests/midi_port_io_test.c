#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "midi_port_io.h"
#include "midi_ports.h"
#include "midi_input.h"
#include "midi_output.h"
#include "usb_enumerate.h"
#include "usb_midi_rx.h"

static usb_device_t live = {
  .root_port = 1U,
  .parent_slot = 1U,
  .parent_port = 2U,
  .slot_id = 3U,
  .generation = 1U,
  .state = USB_DEVICE_ACTIVE
};

static bool rx_running;
static usb_midi_rx_consumer_t subscriber;
static void *subscriber_context;
static uint32_t output_generation;
static uint32_t sent_generation;
static unsigned int sent_cable;
static uint32_t next_claim = 1U;
static uint32_t active_claim;
static unsigned int claimed_bytes;
static unsigned int output_depth;
static unsigned int sysex_staged;
static unsigned int sysex_fragments;
static midi_output_state_t output_state = MIDI_OUTPUT_RUNNING;
static bool reject_output;
static midi_port_handle_t handles[5];


bool usb_get_device_at(unsigned int index, usb_device_t *device) {
  if (index != 0U)
    return false;

  *device = live;
  return true;
}

bool usb_midi_rx_get_info(uint32_t generation,
    usb_midi_rx_info_t *info) {

  if (generation != live.generation)
    return false;

  *info = (usb_midi_rx_info_t){
    .generation = generation,
    .slot_id = live.slot_id,
    .cables = 2U,
    .state = rx_running ?
        USB_MIDI_RX_RUNNING : USB_MIDI_RX_IDLE
  };

  return true;
}

bool usb_midi_rx_start_generation(uint32_t generation) {
  if (generation != live.generation || rx_running ||
      live.state != USB_DEVICE_ACTIVE)
    return false;

  rx_running = true;
  return true;
}

bool usb_midi_rx_subscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context) {

  if (generation != live.generation ||
      !rx_running || subscriber)
    return false;

  subscriber = consumer;
  subscriber_context = context;
  return true;
}

bool usb_midi_rx_unsubscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context) {

  if (generation != live.generation ||
      subscriber != consumer ||
      subscriber_context != context)
    return false;

  subscriber = NULL;
  subscriber_context = NULL;
  return true;
}

bool midi_output_get_info(uint32_t generation,
    midi_output_info_t *info) {

  if (generation != output_generation)
    return false;

  *info = (midi_output_info_t){
    .generation = generation,
    .state = output_state,
    .queue_depth = output_depth,
    .sysex_staged = sysex_staged,
    .sysex_fragments = sysex_fragments,
    .cables = 1U,
    .sysex_active = active_claim != 0U,
    .sysex_claim = active_claim
  };

  return true;
}

bool midi_output_sysex_claim(uint32_t generation,
    unsigned int cable, uint32_t *claim) {

  if (!claim || generation != output_generation ||
      cable != 0U || active_claim != 0U)
    return false;

  active_claim = next_claim++;
  sysex_staged = 1U;
  sysex_fragments = 0U;
  *claim = active_claim;
  return true;
}

bool midi_output_sysex_write_claim(uint32_t generation,
    uint32_t claim, uint8_t byte) {

  if (generation != output_generation ||
      claim == 0U || claim != active_claim ||
      byte >= 0x80U)
    return false;

  if (reject_output || output_state != MIDI_OUTPUT_RUNNING)
    return false;
  if (sysex_staged == 2U) {
    if (output_depth == MIDI_OUTPUT_QUEUE_CAPACITY)
      return false;
    sysex_staged = 0U;
    sysex_fragments++;
  } else {
    sysex_staged++;
  }
  claimed_bytes++;
  return true;
}

bool midi_output_sysex_end_claim(uint32_t generation,
    uint32_t claim) {

  if (generation != output_generation ||
      claim == 0U || claim != active_claim)
    return false;

  if (reject_output || output_state != MIDI_OUTPUT_RUNNING ||
      output_depth == MIDI_OUTPUT_QUEUE_CAPACITY)
    return false;
  active_claim = 0U;
  return true;
}

bool midi_output_sysex_cancel_claim(uint32_t generation,
    uint32_t claim) {

  if (generation != output_generation ||
      claim == 0U || claim != active_claim)
    return false;

  if (output_state == MIDI_OUTPUT_RUNNING && sysex_fragments != 0U &&
      (reject_output || output_depth == MIDI_OUTPUT_QUEUE_CAPACITY))
    return false;
  active_claim = 0U;
  return true;
}

bool midi_output_start_generation(uint32_t generation) {
  if (generation != live.generation ||
      output_generation == generation ||
      live.state != USB_DEVICE_ACTIVE)
    return false;

  output_generation = generation;
  active_claim = 0U;
  sysex_staged = 0U;
  sysex_fragments = 0U;
  output_state = MIDI_OUTPUT_RUNNING;
  return true;
}

bool midi_output_enqueue_short(uint32_t generation,
    unsigned int cable, const midi_short_message_t *message) {

  if (!message || generation != output_generation || cable != 0U ||
      reject_output || output_state != MIDI_OUTPUT_RUNNING ||
      output_depth == MIDI_OUTPUT_QUEUE_CAPACITY || active_claim != 0U)
    return false;

  sent_generation = generation;
  sent_cable = cable;
  return true;
}

static void inject(unsigned int cable, uint8_t status,
    uint8_t note) {

  usb_midi_event_t packet = {
    .cable = (uint8_t)cable,
    .cin = 9U,
    .length = 3U,
    .data = {status, note, 0x40U}
  };

  assert(subscriber);
  assert(subscriber(live.generation,
      &packet, subscriber_context));
}

// Exact application statuses, using the same transport state that the
// Boolean APIs expose. Rejections must preserve ownership and payloads.
static void check_statuses(const midi_port_device_t *device) {
  for (unsigned int i = 1U; i <= 3U; i++)
    if (handles[i].active)
      assert(midi_port_close(&handles[i]) == MIDI_STATUS_OK);
  midi_port_event_t event = {.port = 99U};
  midi_port_change_t change = {.port = 99U};
  midi_port_lifecycle_info_t info = {.opened_generation = 99U};
  midi_short_message_t message = {
    .status = 0x90U, .data = {60U, 100U}, .length = 3U
  };
  midi_port_sysex_t session = {0};
  assert(midi_port_open(0U, &handles[0U]) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_open(MIDI_PORT_CAPACITY + 1U, &handles[0U]) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_open(4U, &handles[4U]) == MIDI_STATUS_NOT_DEFINED);
  assert(midi_port_read(&handles[4U], &event) == MIDI_STATUS_NOT_OPEN);
  assert(event.port == 0U);
  assert(midi_port_next_change(0U, &change) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(change.port == 0U);
  assert(midi_port_lifecycle_get_info(4U, &info) == MIDI_STATUS_NOT_DEFINED);
  assert(info.opened_generation == 0U);
  assert(midi_port_read(&handles[1U], NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_next_change(1U, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_lifecycle_get_info(1U, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_begin(&handles[3U], NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_write(NULL, 0U) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_end(NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_cancel(NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_NOT_OPEN);

  assert(midi_port_read(&handles[1U], &event) == MIDI_STATUS_NOT_OPEN);
  assert(midi_port_close(&handles[3U]) == MIDI_STATUS_NOT_OPEN);
  assert(midi_port_open(3U, &handles[3U]) == MIDI_STATUS_OK);
  assert(midi_port_open(3U, &handles[3U]) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&handles[3U], NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  message.length = 2U;
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_INVALID_ARGUMENT);
  message.length = 3U;
  output_depth = MIDI_OUTPUT_QUEUE_CAPACITY;
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_QUEUE_FULL);
  output_depth = 0U;
  reject_output = true;
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_FAILED);
  reject_output = false;
  output_state = MIDI_OUTPUT_FAILED;
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_FAILED);
  assert(midi_port_open(3U, &handles[3U]) == MIDI_STATUS_FAILED);
  output_state = MIDI_OUTPUT_DISCONNECTED;
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_open(3U, &handles[3U]) == MIDI_STATUS_DISCONNECTED);
  output_state = MIDI_OUTPUT_STOPPED;
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_NOT_OPEN);
  output_state = MIDI_OUTPUT_RUNNING;

  assert(midi_port_sysex_begin(&handles[3U], &session) == MIDI_STATUS_OK);
  midi_port_sysex_t other = {0};
  assert(midi_port_sysex_begin(&handles[3U], &other) == MIDI_STATUS_BUSY);
  assert(midi_port_close(&handles[3U]) == MIDI_STATUS_BUSY);
  assert(midi_port_undefine(3U) == MIDI_STATUS_BUSY);
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_BUSY);
  output_depth = MIDI_OUTPUT_QUEUE_CAPACITY;
  // One byte can still be staged without occupying the full queue.
  assert(midi_port_sysex_write(&session, 1U) == MIDI_STATUS_OK);
  unsigned int before = claimed_bytes;
  assert(midi_port_sysex_write(&session, 2U) == MIDI_STATUS_QUEUE_FULL);
  assert(claimed_bytes == before && session.active);
  assert(midi_port_sysex_end(&session) == MIDI_STATUS_QUEUE_FULL);
  assert(session.active);
  output_depth = 0U;
  assert(midi_port_sysex_write(&session, 2U) == MIDI_STATUS_OK);
  output_depth = MIDI_OUTPUT_QUEUE_CAPACITY;
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_QUEUE_FULL);
  assert(session.active);
  output_depth = 0U;
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_OK);
  assert(!session.active);
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_OK);
  assert(midi_port_close(&handles[3U]) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_NOT_OPEN);

  midi_port_device_t duplicates[2] = {*device, *device};
  duplicates[1].generation++;
  duplicates[1].slot_id++;
  assert(midi_ports_reconcile(duplicates, 2U));
  assert(midi_port_open(3U, &handles[3U]) == MIDI_STATUS_AMBIGUOUS);
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_NOT_OPEN);
  assert(midi_ports_reconcile(NULL, 0U));
  assert(midi_port_read(&handles[1U], &event) == MIDI_STATUS_NOT_OPEN);
  assert(midi_port_close(&handles[3U]) == MIDI_STATUS_NOT_OPEN);
  assert(midi_port_undefine(3U) == MIDI_STATUS_OK);
  assert(midi_port_undefine(3U) == MIDI_STATUS_NOT_DEFINED);
  // A vanished claim can always be released locally without routing anew.
  session = (midi_port_sysex_t){.active = true, .generation = 1U, .claim = 99U};
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_OK);
  assert(!session.active);
}

static void check_handle_lifetimes(const midi_port_device_t *device,
    const midi_port_key_t *key) {
  midi_short_message_t message = {
    .status = 0x90U, .data = {60U, 100U}, .length = 3U
  };
  midi_port_handle_t handle = {0};
  midi_port_handle_t replacement = {0};
  midi_port_event_t event = {.port = 99U};
  midi_port_sysex_t session = {0};
  assert(midi_port_open(3U, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_close(NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_read(NULL, &event) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(event.port == 0U);
  assert(midi_port_send_short(NULL, &message) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_begin(NULL, &session) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_ports_define(3U, key));
  assert(midi_ports_reconcile(device, 1U));
  assert(midi_port_open(3U, &handle) == MIDI_STATUS_OK);
  uint32_t original_token = handle.open_token;
  assert(midi_port_open(3U, &replacement) == MIDI_STATUS_BUSY);
  assert(!replacement.active && replacement.open_token == 0U);
  assert(midi_port_open(2U, &handle) == MIDI_STATUS_BUSY);
  assert(handle.port == 3U && handle.open_token == original_token);
  assert(midi_port_open(3U, &handle) == MIDI_STATUS_OK);
  assert(handle.open_token == original_token);

  midi_port_handle_t copy = handle;
  // Closing a copy revokes the original; reopen on the same generation
  // must issue a different permission and must not revive that original.
  assert(midi_port_close(&copy) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&handle, &message) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_open(3U, &handle) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_open(3U, &replacement) == MIDI_STATUS_OK);
  assert(replacement.generation == handle.generation);
  assert(replacement.open_token != original_token);
  assert(midi_port_close(&handle) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&replacement, &message) == MIDI_STATUS_OK);

  // Reusing an ID for the same endpoint and generation still creates a
  // different definition. Stale close cannot revoke the replacement.
  midi_port_handle_t old_definition = replacement;
  assert(midi_port_undefine(3U) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&old_definition, &message) == MIDI_STATUS_DISCONNECTED);
  assert(midi_ports_define(3U, key));
  assert(midi_ports_reconcile(device, 1U));
  assert(midi_port_open(3U, &old_definition) == MIDI_STATUS_DISCONNECTED);
  handle = (midi_port_handle_t){0};
  assert(midi_port_open(3U, &handle) == MIDI_STATUS_OK);
  assert(handle.definition_serial != old_definition.definition_serial);
  assert(handle.generation == old_definition.generation);
  assert(midi_port_send_short(&old_definition, &message) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_sysex_begin(&old_definition, &session) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&old_definition) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&handle, &message) == MIDI_STATUS_OK);

  // The SysEx permission also includes the open token, even if the
  // transport claim survived a binding change to the same generation.
  assert(midi_port_sysex_begin(&handle, &session) == MIDI_STATUS_OK);
  uint32_t old_claim = session.claim;
  original_token = handle.open_token;
  assert(midi_ports_reconcile(NULL, 0U));
  midi_port_io_service();
  assert(midi_ports_reconcile(device, 1U));
  replacement = (midi_port_handle_t){0};
  assert(midi_port_open(3U, &replacement) == MIDI_STATUS_OK);
  assert(replacement.open_token != original_token);
  assert(midi_port_sysex_write(&session, 1U) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_sysex_end(&session) == MIDI_STATUS_DISCONNECTED);
  assert(session.claim == old_claim && session.active);
  // Cancelling still releases only the original transport claim.
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_OK);
  assert(midi_port_close(&handle) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&replacement, &message) == MIDI_STATUS_OK);
  assert(midi_port_close(&replacement) == MIDI_STATUS_OK);
  assert(midi_port_undefine(3U) == MIDI_STATUS_OK);
}

static void input_disconnect(void) {
  live.state = USB_DEVICE_DISCONNECTING;
  midi_input_service();
  assert(midi_ports_reconcile(NULL, 0U));
  midi_port_io_service();
}

static void input_reconnect(midi_port_device_t *device, uint32_t generation) {
  device->generation = generation;
  device->slot_id = (uint8_t)(generation - 10U);
  live.generation = generation;
  live.slot_id = device->slot_id;
  live.state = USB_DEVICE_ACTIVE;
  // A replacement USB receiver has no old subscribers.
  rx_running = false;
  subscriber = NULL;
  subscriber_context = NULL;
  assert(midi_ports_reconcile(device, 1U));
}

static void check_input_handle_lifetimes(midi_port_device_t device) {
  midi_port_handle_t first = {0}, second = {0}, third = {0}, reopened = {0};
  midi_port_event_t event = {0};
  input_reconnect(&device, 20U);
  assert(midi_port_open(1U, &first) == MIDI_STATUS_OK);
  inject(0U, 0x90U, 10U);
  input_disconnect();
  input_reconnect(&device, 21U);
  assert(midi_port_open(1U, &second) == MIDI_STATUS_OK);
  inject(0U, 0x90U, 20U);
  input_disconnect();
  input_reconnect(&device, 22U);
  assert(midi_port_open(1U, &third) == MIDI_STATUS_OK);
  inject(0U, 0x90U, 30U);

  // Two old opens share the bounded retirement queue. Drain the newer
  // retired open first; it must leave the first open's event untouched.
  assert(midi_port_read(&second, &event) == MIDI_STATUS_OK);
  assert(event.generation == 21U && event.event.message.data[0] == 20U);
  assert(midi_port_read(&second, &event) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&second) == MIDI_STATUS_OK);
  assert(midi_port_read(&third, &event) == MIDI_STATUS_OK);
  assert(event.generation == 22U && event.event.message.data[0] == 30U);
  assert(midi_port_read(&first, &event) == MIDI_STATUS_OK);
  assert(event.generation == 20U && event.event.message.data[0] == 10U);
  assert(midi_port_read(&first, &event) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&first) == MIDI_STATUS_OK);
  assert(midi_port_read(&third, &event) == MIDI_STATUS_EMPTY);

  midi_port_handle_t copy = third;
  assert(midi_port_close(&copy) == MIDI_STATUS_OK);
  assert(midi_port_open(1U, &reopened) == MIDI_STATUS_OK);
  inject(0U, 0x90U, 40U);
  assert(midi_port_read(&third, &event) == MIDI_STATUS_DISCONNECTED);
  assert(event.port == 0U);
  assert(midi_port_close(&third) == MIDI_STATUS_OK);
  assert(midi_port_read(&reopened, &event) == MIDI_STATUS_OK);
  assert(event.event.message.data[0] == 40U);
  assert(midi_port_close(&reopened) == MIDI_STATUS_OK);
}

// Exercise the application surface with simultaneous IN/OUT handles. Raw
// registry/transport calls below set up topology and inject device traffic.
static void check_application_io(midi_port_device_t device) {
  device.generation = live.generation;
  device.slot_id = live.slot_id;
  midi_port_info_t port = {0};
  assert(midi_port_get_info(1U, &port) == MIDI_STATUS_OK);
  midi_port_key_t key = port.key;
  key.direction = MIDI_PORT_OUT;
  assert(midi_ports_define(3U, &key));
  key.root_port = 9U;
  assert(midi_ports_define(MIDI_PORT_CAPACITY, &key));
  assert(midi_ports_reconcile(&device, 1U));

  unsigned int cursor = 0U;
  const unsigned int expected[] = {1U, 2U, 3U, MIDI_PORT_CAPACITY};
  for (unsigned int i = 0U; i < 4U; i++) {
    assert(midi_port_next(&cursor, &port) == MIDI_STATUS_OK);
    assert(port.id == expected[i] && cursor == port.id);
    assert(port.key.direction == (i < 2U ? MIDI_PORT_IN : MIDI_PORT_OUT));
    assert(port.state == (i < 3U ? MIDI_PORT_BOUND : MIDI_PORT_UNBOUND));
  }
  assert(midi_port_next(&cursor, &port) == MIDI_STATUS_EMPTY);
  assert(port.id == 0U && cursor == MIDI_PORT_CAPACITY);
  assert(midi_port_next(&cursor, &port) == MIDI_STATUS_EMPTY);
  port.id = 99U;
  assert(midi_port_next(NULL, &port) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(port.id == 0U);
  cursor = MIDI_PORT_CAPACITY + 1U;
  assert(midi_port_next(&cursor, &port) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(cursor == MIDI_PORT_CAPACITY + 1U);
  assert(midi_port_next(&cursor, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_get_info(0U, &port) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_get_info(MIDI_PORT_CAPACITY + 1U, &port) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_get_info(4U, &port) == MIDI_STATUS_NOT_DEFINED);
  assert(port.id == 0U);
  assert(midi_port_get_info(1U, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(!subscriber && !active_claim); // Discovery acquires no service.

  midi_port_handle_t input = {0}, output = {0};
  midi_port_handle_info_t input_info = {0}, output_info = {0};
  midi_port_sysex_t session = {0};
  midi_port_sysex_info_t sysex_info = {0};
  midi_port_event_t event = {0};
  midi_short_message_t message = {
    .status = 0x90U, .data = {60U, 100U}, .length = 3U
  };
  assert(midi_port_get_handle_info(NULL, &input_info) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_get_handle_info(&input, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_get_handle_info(&input, &input_info) == MIDI_STATUS_OK);
  assert(input_info.status == MIDI_STATUS_NOT_OPEN && !input_info.service_present);
  assert(midi_port_sysex_get_info(NULL, &sysex_info) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_get_info(&session, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_get_info(&session, &sysex_info) == MIDI_STATUS_OK);
  assert(sysex_info.status == MIDI_STATUS_NOT_OPEN && !sysex_info.claim_present);
  assert(midi_port_write(NULL, &message) == MIDI_STATUS_INVALID_ARGUMENT);

  // Output alone starts no input subscription.
  assert(midi_port_open(3U, &output) == MIDI_STATUS_OK);
  assert(!subscriber);
  assert(midi_port_write(&output, &message) == MIDI_STATUS_OK);
  assert(midi_port_read(&output, &event) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_close(&output) == MIDI_STATUS_OK);
  // Input alone requires no permission for output.
  assert(midi_port_open(1U, &input) == MIDI_STATUS_OK);
  assert(midi_port_write(&input, &message) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_begin(&input, &session) == MIDI_STATUS_INVALID_ARGUMENT);
  inject(0U, 0x90U, 41U);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.event.message.data[0] == 41U);
  assert(midi_port_open(3U, &output) == MIDI_STATUS_OK);
  assert(input.generation == output.generation && input.open_token != output.open_token);

  assert(midi_port_sysex_begin(&output, &session) == MIDI_STATUS_OK);
  assert(midi_port_get_handle_info(&output, &output_info) == MIDI_STATUS_OK);
  assert(output_info.status == MIDI_STATUS_OK && output_info.service_present);
  assert(output_info.sysex_active && output_info.service_queue_capacity == MIDI_OUTPUT_QUEUE_CAPACITY);
  assert(midi_port_write(&output, &message) == MIDI_STATUS_BUSY);
  assert(midi_port_get_handle_info(&input, &input_info) == MIDI_STATUS_OK);
  assert(input_info.status == MIDI_STATUS_OK && input_info.service_present);
  assert(input_info.service_queue_capacity == MIDI_INPUT_QUEUE_CAPACITY);

  // SysEx input remains usable during output SysEx, on the same endpoint.
  usb_midi_event_t packet = {
    .cable = 0U, .cin = 4U, .length = 3U, .data = {0xf0U, 0x7dU, 0x01U}
  };
  assert(subscriber(live.generation, &packet, subscriber_context));
  assert(midi_port_get_handle_info(&input, &input_info) == MIDI_STATUS_OK);
  assert(input_info.service_queue_depth == 3U);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.event.type == MIDI_STREAM_SYSEX_BEGIN);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.event.type == MIDI_STREAM_SYSEX_DATA && event.event.byte == 0x7dU);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.event.type == MIDI_STREAM_SYSEX_DATA && event.event.byte == 0x01U);
  packet = (usb_midi_event_t){.cable = 0U, .cin = 5U, .length = 1U, .data = {0xf7U}};
  assert(subscriber(live.generation, &packet, subscriber_context));
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.event.type == MIDI_STREAM_SYSEX_END);
  assert(midi_port_close(&input) == MIDI_STATUS_OK);
  assert(!subscriber); // Closing input leaves output and its session alone.

  uint8_t payload[] = {1U, 2U, 3U, 4U};
  uint8_t invalid[] = {1U, 0x80U};
  size_t accepted = 99U;
  unsigned int before = claimed_bytes;
  assert(midi_port_sysex_write_bytes(&session, payload, 4U, NULL) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(midi_port_sysex_write_bytes(&session, NULL, 1U, &accepted) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(accepted == 0U);
  assert(midi_port_sysex_write_bytes(&session, invalid, 2U, &accepted) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(accepted == 0U && claimed_bytes == before);
  assert(midi_port_sysex_write_bytes(NULL, payload, 4U, &accepted) == MIDI_STATUS_INVALID_ARGUMENT);
  assert(accepted == 0U);
  assert(midi_port_sysex_write_bytes(&session, NULL, 0U, &accepted) == MIDI_STATUS_OK);
  assert(accepted == 0U);
  output_depth = MIDI_OUTPUT_QUEUE_CAPACITY;
  assert(midi_port_sysex_write_bytes(&session, payload, 4U, &accepted) == MIDI_STATUS_QUEUE_FULL);
  assert(accepted == 1U && session.active && claimed_bytes == before + 1U);
  assert(midi_port_sysex_get_info(&session, &sysex_info) == MIDI_STATUS_OK);
  assert(sysex_info.status == MIDI_STATUS_OK && sysex_info.claim_present);
  assert(sysex_info.staged == 2U && sysex_info.fragments == 0U);
  output_depth = 0U;
  size_t retried = 0U;
  assert(midi_port_sysex_write_bytes(&session, payload + accepted, 4U - accepted, &retried) == MIDI_STATUS_OK);
  assert(retried == 3U && claimed_bytes == before + 4U);
  output_depth = MIDI_OUTPUT_QUEUE_CAPACITY;
  assert(midi_port_sysex_end(&session) == MIDI_STATUS_QUEUE_FULL);
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_QUEUE_FULL);
  assert(midi_port_close(&output) == MIDI_STATUS_BUSY);
  assert(midi_port_get_handle_info(&output, &output_info) == MIDI_STATUS_OK);
  assert(output_info.service_queue_depth == MIDI_OUTPUT_QUEUE_CAPACITY);
  output_depth = 0U;
  assert(midi_port_sysex_end(&session) == MIDI_STATUS_OK);
  assert(midi_port_sysex_get_info(&session, &sysex_info) == MIDI_STATUS_OK);
  assert(sysex_info.status == MIDI_STATUS_NOT_OPEN && !sysex_info.claim_present);
  assert(midi_port_sysex_write_bytes(&session, NULL, 0U, &accepted) == MIDI_STATUS_NOT_OPEN);
  assert(accepted == 0U);

  // Closing output leaves input usable; output failures also stay local.
  assert(midi_port_open(1U, &input) == MIDI_STATUS_OK);
  uint32_t input_token = input.open_token;
  assert(midi_port_close(&output) == MIDI_STATUS_OK);
  inject(0U, 0x90U, 42U);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.event.message.data[0] == 42U && input.open_token == input_token);
  assert(midi_port_open(3U, &output) == MIDI_STATUS_OK);
  assert(midi_port_sysex_begin(&output, &session) == MIDI_STATUS_OK);
  output_state = MIDI_OUTPUT_FAILED;
  assert(midi_port_get_handle_info(&output, &output_info) == MIDI_STATUS_OK);
  assert(output_info.status == MIDI_STATUS_FAILED && output_info.service_present);
  assert(midi_port_get_handle_info(&input, &input_info) == MIDI_STATUS_OK);
  assert(input_info.status == MIDI_STATUS_OK);
  assert(midi_port_write(&output, &message) == MIDI_STATUS_FAILED);
  assert(midi_port_sysex_write_bytes(&session, payload, 4U, &accepted) == MIDI_STATUS_FAILED);
  assert(accepted == 0U);
  inject(0U, 0x90U, 43U);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.event.message.data[0] == 43U);
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_OK);
  output_state = MIDI_OUTPUT_RUNNING;

  // Inspecting disconnected handles neither consumes their old events
  // nor reports a replacement generation's service/claim as their own.
  assert(midi_port_sysex_begin(&output, &session) == MIDI_STATUS_OK);
  inject(0U, 0x90U, 44U);
  input_disconnect();
  output_state = MIDI_OUTPUT_DISCONNECTED;
  assert(midi_port_get_handle_info(&input, &input_info) == MIDI_STATUS_OK);
  assert(input_info.status == MIDI_STATUS_DISCONNECTED && input_info.retired_depth == 1U);
  assert(!input_info.service_present);
  assert(midi_port_sysex_get_info(&session, &sysex_info) == MIDI_STATUS_OK);
  assert(sysex_info.status == MIDI_STATUS_DISCONNECTED && sysex_info.claim_present);
  assert(midi_port_get_info(3U, &port) == MIDI_STATUS_OK);
  assert(port.state == MIDI_PORT_UNBOUND);
  uint32_t old_generation = output.generation;
  input_reconnect(&device, 23U);
  midi_port_handle_t replacement = {0};
  assert(midi_port_open(3U, &replacement) == MIDI_STATUS_OK);
  assert(midi_port_get_handle_info(&output, &output_info) == MIDI_STATUS_OK);
  assert(output_info.handle.generation == old_generation);
  assert(output_info.status == MIDI_STATUS_DISCONNECTED && !output_info.service_present);
  assert(midi_port_sysex_get_info(&session, &sysex_info) == MIDI_STATUS_OK);
  assert(sysex_info.status == MIDI_STATUS_DISCONNECTED && !sysex_info.claim_present);
  assert(midi_port_sysex_cancel(&session) == MIDI_STATUS_OK);
  assert(midi_port_write(&output, &message) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  assert(event.generation == old_generation && event.event.message.data[0] == 44U);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&input) == MIDI_STATUS_OK);
  assert(midi_port_close(&output) == MIDI_STATUS_OK);
  assert(midi_port_write(&replacement, &message) == MIDI_STATUS_OK);
  assert(midi_port_close(&replacement) == MIDI_STATUS_OK);

  midi_port_device_t duplicates[2] = {device, device};
  duplicates[1].generation++;
  duplicates[1].slot_id++;
  assert(midi_ports_reconcile(duplicates, 2U));
  assert(midi_port_get_info(3U, &port) == MIDI_STATUS_OK);
  assert(port.state == MIDI_PORT_AMBIGUOUS);
  assert(midi_port_open(3U, &replacement) == MIDI_STATUS_AMBIGUOUS);
  assert(midi_port_undefine(3U) == MIDI_STATUS_OK);
  assert(midi_port_undefine(MIDI_PORT_CAPACITY) == MIDI_STATUS_OK);
}

static void check_single_direction_devices(midi_port_device_t device) {
  midi_port_info_t port = {0};
  assert(midi_port_get_info(1U, &port) == MIDI_STATUS_OK);
  midi_port_key_t key = port.key;
  key.direction = MIDI_PORT_OUT;
  assert(midi_ports_define(3U, &key));
  midi_port_handle_t input = {0}, other_input = {0}, output = {0};
  midi_port_handle_info_t info = {0};
  midi_port_event_t event = {0};
  midi_short_message_t message = {
    .status = 0x90U, .data = {60U, 100U}, .length = 3U
  };

  device.out_cables = 0U;
  input_reconnect(&device, 24U);
  assert(midi_port_open(3U, &output) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_open(1U, &input) == MIDI_STATUS_OK);
  assert(midi_port_open(2U, &other_input) == MIDI_STATUS_OK);
  inject(0U, 0x90U, 60U);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_OK);
  // Diagnostic transport operations can stop just one selected cable.
  // Inspection and reads agree on NOT_OPEN, and handle close is still safe.
  assert(midi_input_close_port_cable(input.generation, input.cable));
  assert(midi_port_get_handle_info(&input, &info) == MIDI_STATUS_OK);
  assert(info.status == MIDI_STATUS_NOT_OPEN);
  assert(midi_port_read(&input, &event) == MIDI_STATUS_NOT_OPEN);
  assert(midi_port_close(&input) == MIDI_STATUS_OK);
  inject(1U, 0x91U, 61U);
  assert(midi_port_read(&other_input, &event) == MIDI_STATUS_OK);
  assert(midi_port_close(&other_input) == MIDI_STATUS_OK);

  device.in_cables = 0U;
  device.out_cables = 1U;
  input_reconnect(&device, 25U);
  assert(midi_port_open(1U, &input) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_open(3U, &output) == MIDI_STATUS_OK);
  assert(!subscriber);
  assert(midi_port_write(&output, &message) == MIDI_STATUS_OK);
  assert(midi_port_close(&output) == MIDI_STATUS_OK);
  assert(midi_port_undefine(3U) == MIDI_STATUS_OK);
}

int main(void) {
  unsigned int cursor = 0U;
  midi_port_info_t discovered = {.id = 99U};
  assert(midi_port_next(&cursor, &discovered) == MIDI_STATUS_EMPTY);
  assert(cursor == MIDI_PORT_CAPACITY && discovered.id == 0U);

  midi_port_key_t key = {
    .root_port = 1U,
    .hub_port = 2U,
    .vendor_id = 0x7104U,
    .product_id = 0x1400U,
    .direction = MIDI_PORT_IN,
    .cable = 0U
  };

  assert(midi_ports_define(1U, &key));

  key.cable = 1U;
  assert(midi_ports_define(2U, &key));

  key.direction = MIDI_PORT_OUT;
  key.cable = 0U;
  assert(midi_ports_define(3U, &key));

  midi_port_device_t device = {
    .generation = 1U,
    .slot_id = 3U,
    .root_port = 1U,
    .hub_port = 2U,
    .vendor_id = 0x7104U,
    .product_id = 0x1400U,
    .in_cables = 2U,
    .out_cables = 1U
  };

  assert(midi_ports_reconcile(&device, 1U));

  // Opening a second cable reuses the receiver.
  assert((midi_port_open(1U, &handles[1U]) == MIDI_STATUS_OK));
  assert((midi_port_open(2U, &handles[2U]) == MIDI_STATUS_OK));
  assert((midi_port_open(3U, &handles[3U]) == MIDI_STATUS_OK));

  midi_short_message_t message = {
    .status = 0x90U,
    .data = {60U, 100U},
    .length = 3U
  };

  assert((midi_port_send_short(&handles[1U], &message) == MIDI_STATUS_INVALID_ARGUMENT));
  assert((midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_OK));
  assert(sent_generation == 1U && sent_cable == 0U);

  // Interleave three received events across
  // two virtual cables.
  inject(0U, 0x90U, 60U);
  inject(1U, 0x91U, 61U);
  inject(0U, 0x90U, 62U);

  midi_port_event_t event = {0};

  assert((midi_port_read(&handles[3U], &event) == MIDI_STATUS_INVALID_ARGUMENT));

  // Reading cable 1 must not consume cable 0.
  assert((midi_port_read(&handles[2U], &event) == MIDI_STATUS_OK));
  assert(event.port == 2U && event.generation == 1U);
  assert(event.event.message.status == 0x91U);

  assert((midi_port_read(&handles[1U], &event) == MIDI_STATUS_OK));
  assert(event.event.message.data[0] == 60U);

  assert((midi_port_read(&handles[1U], &event) == MIDI_STATUS_OK));
  assert(event.event.message.data[0] == 62U);

  assert((midi_port_read(&handles[1U], &event) == MIDI_STATUS_EMPTY));

  // Exercise a wrapped FIFO and interior removal.
  for (unsigned int i = 0U; i < 64U; i++)
    inject(0U, 0x90U, (uint8_t)i);

  assert((midi_port_read(&handles[1U], &event) == MIDI_STATUS_OK));
  assert(event.event.message.data[0] == 0U);

  inject(1U, 0x91U, 99U);

  assert((midi_port_read(&handles[2U], &event) == MIDI_STATUS_OK));
  assert(event.event.message.data[0] == 99U);

  for (unsigned int i = 1U; i < 64U; i++) {
    assert((midi_port_read(&handles[1U], &event) == MIDI_STATUS_OK));
    assert(event.event.message.data[0] == i);
  }

  // An unfinished SysEx produces a terminal
  // abort when its physical receiver disconnects.
  usb_midi_event_t sysex = {
    .cable = 1U,
    .cin = 4U,
    .length = 3U,
    .data = {0xf0U, 0x7dU, 0x01U}
  };

  assert(subscriber(live.generation,
      &sysex, subscriber_context));

  live.state = USB_DEVICE_DISCONNECTING;
  midi_input_service();

  // The USB registry now observes the disconnect.
  assert(midi_ports_reconcile(NULL, 0U));
  midi_port_io_service();

  midi_port_lifecycle_info_t lifecycle = {0};

  assert((midi_port_lifecycle_get_info(2U, &lifecycle) == MIDI_STATUS_OK));
  assert(lifecycle.retired_depth == 4U);
  assert(lifecycle.opened_generation == 0U);
  assert(lifecycle.retired_dropped == 0U);

  // The old device FIFO has been retired into
  // the logical port, without losing its events.
  midi_input_event_t raw = {0};
  assert(!midi_input_read_cable(1U, 1U, &raw));

  // The old binding is no longer usable for output.
  assert((midi_port_open(3U, &handles[3U]) == MIDI_STATUS_DISCONNECTED));
  assert((midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_DISCONNECTED));

  // Reconnect the same logical endpoint as gen 9.
  live.state = USB_DEVICE_ACTIVE;
  live.generation = 9U;
  live.slot_id = 4U;

  device.generation = 9U;
  device.slot_id = 4U;

  rx_running = false;
  output_generation = 0U;

  assert(midi_ports_reconcile(&device, 1U));
  midi_port_io_service();

  // Old-generation events remain readable even
  // before the replacement receiver is opened.
  assert((midi_port_read(&handles[2U], &event) == MIDI_STATUS_OK));
  assert(event.generation == 1U);
  assert(event.event.type == MIDI_STREAM_SYSEX_BEGIN);

  assert((midi_port_read(&handles[2U], &event) == MIDI_STATUS_OK));
  assert(event.generation == 1U);
  assert(event.event.type == MIDI_STREAM_SYSEX_DATA);

  // A new open cannot overwrite an obsolete handle. Preserve the old
  // input handle to drain its final events alongside the replacement.
  midi_port_handle_t old_input = handles[2U];
  midi_port_handle_t old_output = handles[3U];
  assert(midi_port_open(3U, &handles[3U]) == MIDI_STATUS_DISCONNECTED);
  assert(handles[3U].generation == 1U);
  assert(midi_port_close(&handles[1U]) == MIDI_STATUS_OK);
  assert(midi_port_close(&handles[3U]) == MIDI_STATUS_OK);
  handles[2U] = (midi_port_handle_t){0};
  assert((midi_port_open(1U, &handles[1U]) == MIDI_STATUS_OK));
  assert((midi_port_open(2U, &handles[2U]) == MIDI_STATUS_OK));
  assert((midi_port_open(3U, &handles[3U]) == MIDI_STATUS_OK));
  assert(midi_port_send_short(&old_output, &message) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&old_output) == MIDI_STATUS_OK);
  assert(midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_OK);
  // New input permission cannot consume the old open's retired events.
  assert(midi_port_read(&handles[2U], &event) == MIDI_STATUS_EMPTY);
  inject(1U, 0x91U, 98U);

  assert((midi_port_read(&old_input, &event) == MIDI_STATUS_OK));
  assert(event.generation == 1U);
  assert(event.event.type == MIDI_STREAM_SYSEX_DATA);

  assert((midi_port_read(&old_input, &event) == MIDI_STATUS_OK));
  assert(event.generation == 1U);
  assert(event.event.type == MIDI_STREAM_SYSEX_ABORT);
  assert(midi_port_read(&old_input, &event) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_read(&handles[2U], &event) == MIDI_STATUS_OK);
  assert(event.generation == 9U && event.event.message.data[0] == 98U);

  // Only the replacement handle may observe new input events.
  inject(1U, 0x91U, 99U);

  assert((midi_port_read(&handles[2U], &event) == MIDI_STATUS_OK));
  assert(event.generation == 9U);
  assert(event.event.type == MIDI_STREAM_SHORT);
  assert(event.event.message.data[0] == 99U);
  assert(midi_port_read(&old_input, &event) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&old_input) == MIDI_STATUS_OK);

  assert((midi_port_read(&handles[2U], &event) == MIDI_STATUS_EMPTY));

  // The connection history must include:
  // initial connection, disconnection, reconnection.
  midi_port_change_t change = {0};

  assert((midi_port_next_change(2U, &change) == MIDI_STATUS_OK));
  assert(change.previous == MIDI_PORT_UNBOUND);
  assert(change.current == MIDI_PORT_BOUND);
  assert(change.current_generation == 1U);

  assert((midi_port_next_change(2U, &change) == MIDI_STATUS_OK));
  assert(change.previous == MIDI_PORT_BOUND);
  assert(change.current == MIDI_PORT_UNBOUND);
  assert(change.previous_generation == 1U);

  assert((midi_port_next_change(2U, &change) == MIDI_STATUS_OK));
  assert(change.previous == MIDI_PORT_UNBOUND);
  assert(change.current == MIDI_PORT_BOUND);
  assert(change.current_generation == 9U);

  assert((midi_port_next_change(2U, &change) == MIDI_STATUS_EMPTY));

  assert((midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_OK));
  assert(sent_generation == 9U);

  // An unbound port cannot route new operations.
  assert(midi_ports_reconcile(NULL, 0U));
  assert((midi_port_open(3U, &handles[3U]) == MIDI_STATUS_DISCONNECTED));
  assert((midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_DISCONNECTED));

  // Rebind the same logical ports to generation 9.
  live.state = USB_DEVICE_ACTIVE;
  live.generation = 9U;
  live.slot_id = 4U;
  device.generation = 9U;
  device.slot_id = 4U;
  rx_running = false;
  output_generation = 0U;

  assert(midi_ports_reconcile(&device, 1U));
  // Even returning to the same generation cannot revive a revoked token.
  midi_port_handle_t same_generation = handles[3U];
  assert(midi_port_send_short(&same_generation, &message) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&handles[1U]) == MIDI_STATUS_OK);
  assert(midi_port_close(&handles[3U]) == MIDI_STATUS_OK);
  assert((midi_port_open(1U, &handles[1U]) == MIDI_STATUS_OK));
  assert((midi_port_open(3U, &handles[3U]) == MIDI_STATUS_OK));
  assert(handles[3U].open_token != same_generation.open_token);
  assert(midi_port_send_short(&same_generation, &message) == MIDI_STATUS_DISCONNECTED);

  assert((midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_OK));
  assert(sent_generation == 9U);
  assert((midi_port_read(&handles[1U], &event) == MIDI_STATUS_EMPTY));

  // A logical SysEx session pins its generation,
  // cable, definition and driver claim.
  midi_port_sysex_t session = {0};

  assert((midi_port_sysex_begin(&handles[3U], &session) == MIDI_STATUS_OK));
  assert(session.active);
  assert(session.generation == 9U);
  assert(session.cable == 0U);
  assert(session.claim != 0U);

  assert((midi_port_sysex_begin(&handles[3U], &session) == MIDI_STATUS_BUSY));
  assert((midi_port_sysex_write(&session, 0x80U) == MIDI_STATUS_INVALID_ARGUMENT));
  assert((midi_port_sysex_write(&session, 0x7dU) == MIDI_STATUS_OK));
  assert(claimed_bytes == 1U);

  assert((midi_port_sysex_end(&session) == MIDI_STATUS_OK));
  assert(!session.active);
  assert((midi_port_sysex_write(&session, 0x01U) == MIDI_STATUS_NOT_OPEN));

  // Start another session and remove its binding.
  assert((midi_port_sysex_begin(&handles[3U], &session) == MIDI_STATUS_OK));
  uint32_t old_claim = session.claim;

  assert(midi_ports_reconcile(NULL, 0U));

  assert((midi_port_sysex_write(&session, 0x02U) == MIDI_STATUS_DISCONNECTED));
  assert((midi_port_sysex_end(&session) == MIDI_STATUS_DISCONNECTED));

  // Explicit cancellation still targets the
  // original generation, not a new binding.
  assert((midi_port_sysex_cancel(&session) == MIDI_STATUS_OK));
  assert(!session.active);
  assert(active_claim == 0U);

  // A different physical incarnation arrives.
  live.generation = 11U;
  live.slot_id = 5U;
  live.state = USB_DEVICE_ACTIVE;
  device.generation = 11U;
  device.slot_id = 5U;
  output_generation = 0U;
  midi_input_service();

  assert(midi_ports_reconcile(&device, 1U));

  // Rebinding alone does not authorise sending or overwrite the handle.
  assert((midi_port_send_short(&handles[3U], &message) == MIDI_STATUS_DISCONNECTED));
  assert((midi_port_sysex_begin(&handles[3U], &session) == MIDI_STATUS_DISCONNECTED));
  assert(midi_port_open(3U, &handles[3U]) == MIDI_STATUS_DISCONNECTED);
  assert(midi_port_close(&handles[3U]) == MIDI_STATUS_OK);
  assert((midi_port_open(3U, &handles[3U]) == MIDI_STATUS_OK));
  assert((midi_port_sysex_begin(&handles[3U], &session) == MIDI_STATUS_OK));

  assert(session.generation == 11U);
  assert(session.claim != old_claim);

  // A copied, stale handle cannot adopt the
  // current driver's ownership claim.
  midi_port_sysex_t stale = session;
  stale.claim = old_claim;

  assert((midi_port_sysex_write(&stale, 0x03U) == MIDI_STATUS_NOT_OPEN));
  assert((midi_port_sysex_end(&stale) == MIDI_STATUS_NOT_OPEN));

  assert((midi_port_sysex_cancel(&session) == MIDI_STATUS_OK));
  assert(!session.active);

  // Exercise the bounded connection-change queue.
  // Port 3 is an output, so these transitions do
  // not involve retiring input events.
  midi_port_change_t ignored = {0};

  while ((midi_port_next_change(3U, &ignored) == MIDI_STATUS_OK)) {
    // Discard previous test notifications.
  }

  assert((midi_port_lifecycle_get_info(3U, &lifecycle) == MIDI_STATUS_OK));
  assert(lifecycle.pending_changes == 0U);

  uint64_t previous_drops = lifecycle.changes_dropped;

  // Generate twelve transitions without consuming
  // notifications. Only the newest eight fit.
  for (unsigned int i = 0U; i < 12U; i++) {
    if ((i & 1U) == 0U)
      assert(midi_ports_reconcile(NULL, 0U));
    else
      assert(midi_ports_reconcile(&device, 1U));

    midi_port_io_service();
  }

  assert((midi_port_lifecycle_get_info(3U, &lifecycle) == MIDI_STATUS_OK));
  assert(lifecycle.pending_changes == 8U);
  assert(lifecycle.changes_dropped == previous_drops + 4U);

  // The oldest four transitions were displaced.
  // The first remaining notification is therefore
  // the fifth transition: Bound -> Unbound.
  assert((midi_port_next_change(3U, &ignored) == MIDI_STATUS_OK));
  assert(ignored.previous == MIDI_PORT_BOUND);
  assert(ignored.current == MIDI_PORT_UNBOUND);

  unsigned int remaining = 0U;
  while ((midi_port_next_change(3U, &ignored) == MIDI_STATUS_OK))
    remaining++;

  assert(remaining == 7U);

  check_statuses(&device);
  check_handle_lifetimes(&device, &key);
  check_input_handle_lifetimes(device);
  check_application_io(device);
  check_single_direction_devices(device);

  puts("MIDI logical I/O: PASS");
  return 0;
}
