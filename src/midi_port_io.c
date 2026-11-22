#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midi_port_io.h"
#include "midi_ports.h"
#include "midi_ports_internal.h"
#include "midi_input.h"
#include "midi_output.h"
#include "usb_midi_rx.h"
#include "usb_midi.h"

#define PORT_RETIRED_CAPACITY (MIDI_INPUT_QUEUE_CAPACITY + 1U)
#define PORT_CHANGE_CAPACITY 8U

typedef struct {
  uint32_t serial;
  midi_port_info_t previous;
  uint32_t opened_generation;
  uint32_t open_token;

  midi_port_event_t retired[PORT_RETIRED_CAPACITY];
  uint32_t retired_tokens[PORT_RETIRED_CAPACITY];
  unsigned int retired_head;
  unsigned int retired_depth;
  uint64_t retired_dropped;
  uint64_t retirement_missed;

  midi_port_change_t changes[PORT_CHANGE_CAPACITY];
  unsigned int change_head;
  unsigned int change_depth;
  uint64_t changes_dropped;
} port_runtime_t;

static port_runtime_t runtimes[MIDI_PORT_CAPACITY];
static uint32_t next_open_token = 1U;

static void port_save_change(port_runtime_t *runtime,
    unsigned int id, const midi_port_info_t *next) {

  midi_port_change_t change = {
    .port = id,
    .previous = runtime->previous.state,
    .current = next->state,
    .previous_generation = runtime->previous.generation,
    .current_generation = next->generation
  };

  if (runtime->change_depth == PORT_CHANGE_CAPACITY) {
    // Keep the newest transitions. Report the
    // number of older notifications displaced.
    runtime->change_head = (runtime->change_head + 1U) % PORT_CHANGE_CAPACITY;
    runtime->change_depth--;
    runtime->changes_dropped++;
  }

  unsigned int tail = (runtime->change_head +
      runtime->change_depth) % PORT_CHANGE_CAPACITY;

  runtime->changes[tail] = change;
  runtime->change_depth++;
}

static void port_retire(port_runtime_t *runtime,
    unsigned int id, uint32_t generation, unsigned int cable) {

  midi_input_info_t input = {0};

  if (!midi_input_get_info(generation, &input)) {
    // The old record is already inaccessible.
    // Its unread event count cannot be recovered.
    runtime->retirement_missed++;
    return;
  }

  midi_input_event_t received = {0};

  while (midi_input_read_cable(generation, cable, &received)) {
    if (runtime->retired_depth == PORT_RETIRED_CAPACITY) {
      // Preserve older accepted events, but make
      // loss of newer events explicitly observable.
      runtime->retired_dropped++;
      continue;
    }

    unsigned int tail = (runtime->retired_head +
        runtime->retired_depth) % PORT_RETIRED_CAPACITY;

    runtime->retired_tokens[tail] = runtime->open_token;
    runtime->retired[tail] = (midi_port_event_t){
      .port = id,
      .generation = received.generation,
      .event = received.event
    };
    runtime->retired_depth++;
  }
}

// Synchronise one logical port with the registry.
// All calls originate in foreground context.
static bool port_update(unsigned int id, midi_port_info_t *info) {
  if (id == 0U || id > MIDI_PORT_CAPACITY)
    return false;

  port_runtime_t *runtime = &runtimes[id - 1U];
  midi_port_info_t current = {0};

  if (!midi_ports_get(id, &current)) {
    // Explicitly clearing a port definition also
    // destroys its old application-owned state.
    *runtime = (port_runtime_t){0};
    return false;
  }

  if (runtime->serial != current.definition_serial) {
    // A newly defined port must not inherit events
    // or ownership from an old definition with
    // the same numeric ID.
    *runtime = (port_runtime_t){0};
    runtime->serial = current.definition_serial;
    runtime->previous = current;
    runtime->previous.state = MIDI_PORT_UNBOUND;
    runtime->previous.generation = 0U;
    runtime->previous.slot_id = 0U;
  }

  bool changed = runtime->previous.state != current.state ||
      runtime->previous.generation != current.generation ||
      runtime->previous.slot_id != current.slot_id;

  if (changed) {
    if (runtime->previous.state == MIDI_PORT_BOUND &&
        runtime->previous.key.direction == MIDI_PORT_IN &&
        runtime->opened_generation == runtime->previous.generation) {

      uint32_t old_generation = runtime->previous.generation;
      unsigned int old_cable = runtime->previous.key.cable;
      // Preserve accepted events before releasing
      // the former logical input subscription.
      port_retire(runtime, id, old_generation, old_cable);

      midi_input_info_t input = {0};

      if (midi_input_get_info(old_generation, &input) &&
          input.attached && input.port_filtered) {

        // A binding change must not leave an
        // unowned cable consuming queue capacity.
        if (!midi_input_close_port_cable(old_generation, old_cable))
          return false;
      }
    }

    port_save_change(runtime, id, &current);
    // A previous open never authorises a different
    // binding, even on the same physical socket.
    runtime->opened_generation = 0U;
    runtime->open_token = 0U;
    runtime->previous = current;
  }

  if (info)
    *info = current;

  return true;
}

void midi_port_io_service(void) {
  for (unsigned int id = 1U; id <= MIDI_PORT_CAPACITY; id++)
    port_update(id, NULL);
}

static midi_status_t port_lookup(unsigned int id, midi_port_info_t *info) {
  if (id == 0U || id > MIDI_PORT_CAPACITY)
    return MIDI_STATUS_INVALID_ARGUMENT;
  if (!midi_ports_get(id, info))
    return MIDI_STATUS_NOT_DEFINED;
  return port_update(id, info) ? MIDI_STATUS_OK : MIDI_STATUS_FAILED;
}

static midi_status_t port_binding(const midi_port_info_t *info) {
  if (info->state == MIDI_PORT_AMBIGUOUS)
    return MIDI_STATUS_AMBIGUOUS;
  return info->state == MIDI_PORT_BOUND ?
      MIDI_STATUS_OK : MIDI_STATUS_DISCONNECTED;
}

static midi_status_t handle_definition(const midi_port_handle_t *handle,
    midi_port_info_t *current) {
  if (!handle)
    return MIDI_STATUS_INVALID_ARGUMENT;
  if (!handle->active || handle->open_token == 0U)
    return MIDI_STATUS_NOT_OPEN;
  if (handle->port == 0U || handle->port > MIDI_PORT_CAPACITY)
    return MIDI_STATUS_INVALID_ARGUMENT;
  midi_status_t status = port_lookup(handle->port, current);
  if (status == MIDI_STATUS_NOT_DEFINED)
    return MIDI_STATUS_DISCONNECTED;
  if (status != MIDI_STATUS_OK)
    return status;
  if (current->definition_serial != handle->definition_serial ||
      current->key.cable != handle->cable ||
      current->key.direction != handle->direction)
    return MIDI_STATUS_DISCONNECTED;
  return MIDI_STATUS_OK;
}

static midi_status_t handle_current(const midi_port_handle_t *handle,
    midi_port_info_t *current) {
  midi_status_t status = handle_definition(handle, current);
  if (status != MIDI_STATUS_OK)
    return status;
  port_runtime_t *runtime = &runtimes[handle->port - 1U];
  if (current->state != MIDI_PORT_BOUND ||
      current->generation != handle->generation ||
      runtime->open_token != handle->open_token ||
      runtime->opened_generation != handle->generation)
    return MIDI_STATUS_DISCONNECTED;
  return MIDI_STATUS_OK;
}

static midi_status_t port_target(const midi_port_handle_t *handle,
    midi_port_direction_t expected, uint32_t *generation, uint8_t *cable) {
  midi_port_info_t current = {0};
  midi_status_t status = handle_current(handle, &current);
  if (status != MIDI_STATUS_OK)
    return status;
  if (handle->direction != expected)
    return MIDI_STATUS_INVALID_ARGUMENT;
  *generation = handle->generation;
  *cable = handle->cable;
  return MIDI_STATUS_OK;
}

// Remove one retired event without disturbing the order of other opens.
static void retired_remove(port_runtime_t *runtime, unsigned int offset) {
  if (offset == 0U) {
    runtime->retired_head = (runtime->retired_head + 1U) % PORT_RETIRED_CAPACITY;
    runtime->retired_depth--;
    return;
  }
  for (unsigned int i = offset; i + 1U < runtime->retired_depth; i++) {
    unsigned int dst = (runtime->retired_head + i) % PORT_RETIRED_CAPACITY;
    unsigned int src = (dst + 1U) % PORT_RETIRED_CAPACITY;
    runtime->retired[dst] = runtime->retired[src];
    runtime->retired_tokens[dst] = runtime->retired_tokens[src];
  }
  runtime->retired_depth--;
}

static midi_status_t output_ready(uint32_t generation, midi_output_info_t *info) {
  if (!midi_output_get_info(generation, info))
    return MIDI_STATUS_FAILED;
  switch (info->state) {
    case MIDI_OUTPUT_RUNNING: return MIDI_STATUS_OK;
    case MIDI_OUTPUT_DISCONNECTED: return MIDI_STATUS_DISCONNECTED;
    case MIDI_OUTPUT_FAILED: return MIDI_STATUS_FAILED;
    default: return MIDI_STATUS_NOT_OPEN;
  }
}

// Classify only observable rejection reasons. Do not label an unexplained
// Boolean failure as retryable backpressure.
static midi_status_t output_rejected(uint32_t generation) {
  midi_output_info_t info = {0};
  midi_status_t status = output_ready(generation, &info);
  if (status != MIDI_STATUS_OK)
    return status;
  return info.queue_depth >= MIDI_OUTPUT_QUEUE_CAPACITY ?
      MIDI_STATUS_QUEUE_FULL : MIDI_STATUS_FAILED;
}

midi_status_t midi_port_open(unsigned int id, midi_port_handle_t *handle) {
  if (!handle || id == 0U || id > MIDI_PORT_CAPACITY)
    return MIDI_STATUS_INVALID_ARGUMENT;
  midi_port_info_t current = {0};

  if (handle->active) {
    if (handle->port != id)
      return MIDI_STATUS_BUSY;
    midi_status_t existing = handle_current(handle, &current);
    if (existing != MIDI_STATUS_OK)
      return existing;
  }
  midi_status_t status = port_lookup(id, &current);
  if (status != MIDI_STATUS_OK)
    return status;
  status = port_binding(&current);
  if (status != MIDI_STATUS_OK)
    return status;

  port_runtime_t *runtime = &runtimes[id - 1U];
  if (!handle->active && runtime->open_token != 0U)
    return MIDI_STATUS_BUSY;
  // Never wrap a permission token back onto an obsolete handle.
  if (!handle->active && next_open_token == 0U)
    return MIDI_STATUS_FAILED;

  uint32_t generation = current.generation;
  uint8_t cable = current.key.cable;
  bool opened = false;

  if (current.key.direction == MIDI_PORT_OUT) {
    midi_output_info_t output = {0};

    if (midi_output_get_info(generation, &output)) {
      if (output.state == MIDI_OUTPUT_DISCONNECTED)
        return MIDI_STATUS_DISCONNECTED;
      if (output.state == MIDI_OUTPUT_FAILED)
        return MIDI_STATUS_FAILED;
    }
    if (output.state == MIDI_OUTPUT_RUNNING)
      opened = cable < output.cables;
    else
      opened = midi_output_start_generation(generation);

  } else if (current.key.direction == MIDI_PORT_IN) {
    usb_midi_rx_info_t rx = {0};

    // The USB receiver is normally already active.
    // Retain manual startup for diagnostic operation.
    if (!usb_midi_rx_get_info(generation, &rx) ||
        rx.state != USB_MIDI_RX_RUNNING) {
      if (!usb_midi_rx_start_generation(generation))
        return MIDI_STATUS_FAILED;
    }

    // Claim only this logical port's virtual cable.
    // Other opened ports can share the same parser.
    opened = midi_input_open_port_cable(generation, cable);
  }

  if (!opened)
    return MIDI_STATUS_FAILED;
  if (!handle->active) {
    runtime->opened_generation = generation;
    runtime->open_token = next_open_token++;
    *handle = (midi_port_handle_t){
      .active = true,
      .port = id,
      .definition_serial = current.definition_serial,
      .generation = generation,
      .open_token = runtime->open_token,
      .cable = cable,
      .direction = current.key.direction
    };
  }
  return MIDI_STATUS_OK;
}

static midi_status_t port_close_service(unsigned int id) {
  midi_port_info_t current = {0};

  midi_status_t status = port_lookup(id, &current);
  if (status != MIDI_STATUS_OK)
    return status;
  status = port_binding(&current);
  if (status != MIDI_STATUS_OK)
    return status;

  port_runtime_t *runtime = &runtimes[id - 1U];

  if (runtime->opened_generation != current.generation)
    return MIDI_STATUS_NOT_OPEN;

  uint32_t generation = current.generation;
  unsigned int cable = current.key.cable;

  if (current.key.direction == MIDI_PORT_IN) {
    midi_input_info_t input = {0};
    if (midi_input_get_info(generation, &input) &&
        input.attached && input.port_filtered &&
        !midi_input_close_port_cable(generation, cable))
      return MIDI_STATUS_FAILED;
  } else if (current.key.direction == MIDI_PORT_OUT) {
    midi_output_info_t output = {0};
    // Do not abandon an unfinished SysEx session.
    // Another cable's session does not prevent
    // this logical port from closing.
    if (midi_output_get_info(generation, &output) &&
        output.sysex_active && output.sysex_cable == cable)
      return MIDI_STATUS_BUSY;
  } else {
    return MIDI_STATUS_FAILED;
  }

  // Revoke application access, not device ownership.
  // Previously queued output remains eligible for
  // transmission by the normal output service.
  runtime->opened_generation = 0U;
  runtime->open_token = 0U;

  return MIDI_STATUS_OK;
}

midi_status_t midi_port_close(midi_port_handle_t *handle) {
  if (!handle)
    return MIDI_STATUS_INVALID_ARGUMENT;
  if (!handle->active || handle->open_token == 0U)
    return MIDI_STATUS_NOT_OPEN;
  midi_port_info_t current = {0};
  midi_status_t status = handle_current(handle, &current);
  if (status != MIDI_STATUS_OK && status != MIDI_STATUS_DISCONNECTED)
    return status;
  if (status == MIDI_STATUS_OK) {
    status = port_close_service(handle->port);
    if (status != MIDI_STATUS_OK)
      return status;
  }
  // Only discard events owned by this definition/open; never a replacement.
  port_runtime_t *runtime = &runtimes[handle->port - 1U];
  if (runtime->serial == handle->definition_serial) {
    for (unsigned int i = 0U; i < runtime->retired_depth;) {
      unsigned int index = (runtime->retired_head + i) % PORT_RETIRED_CAPACITY;
      if (runtime->retired_tokens[index] == handle->open_token)
        retired_remove(runtime, i);
      else
        i++;
    }
  }
  handle->active = false;
  return MIDI_STATUS_OK;
}

midi_status_t midi_port_undefine(unsigned int id) {
  midi_port_info_t current = {0};

  midi_status_t status = port_lookup(id, &current);
  if (status != MIDI_STATUS_OK)
    return status;

  port_runtime_t *runtime = &runtimes[id - 1U];

  // Release any application ownership before
  // removing the registry definition.
  if (runtime->opened_generation != 0U) {
    status = port_close_service(id);
    if (status != MIDI_STATUS_OK)
      return status;
  }

  if (!midi_ports_clear_registry(id))
    return MIDI_STATUS_FAILED;

  // No old events, changes or permissions may
  // survive reuse of this numeric port ID.
  *runtime = (port_runtime_t){0};

  return MIDI_STATUS_OK;
}

midi_status_t midi_port_send_short(const midi_port_handle_t *handle,
    const midi_short_message_t *message) {
  uint32_t generation = 0U;
  uint8_t cable = 0U;

  midi_status_t status = port_target(handle, MIDI_PORT_OUT, &generation, &cable);
  if (status != MIDI_STATUS_OK)
    return status;

  // Reuse transport validation without changing its Boolean contract.
  usb_midi_config_t config = {.out_found = true,
      .bulk_out_jacks = MIDI_PORT_MAX_CABLES};
  uint8_t packet[4];
  if (!usb_midi_short_packet(&config, cable, message, packet))
    return MIDI_STATUS_INVALID_ARGUMENT;
  midi_output_info_t output = {0};
  status = output_ready(generation, &output);
  if (status != MIDI_STATUS_OK)
    return status;
  if (output.sysex_active)
    return MIDI_STATUS_BUSY;
  return midi_output_enqueue_short(generation, cable, message) ?
      MIDI_STATUS_OK : output_rejected(generation);
}

midi_status_t midi_port_read(const midi_port_handle_t *handle,
    midi_port_event_t *event) {
  if (!event)
    return MIDI_STATUS_INVALID_ARGUMENT;

  *event = (midi_port_event_t){0};

  midi_port_info_t current = {0};

  midi_status_t status = handle_definition(handle, &current);
  if (status != MIDI_STATUS_OK)
    return status;
  if (current.key.direction != MIDI_PORT_IN)
    return MIDI_STATUS_INVALID_ARGUMENT;

  port_runtime_t *runtime = &runtimes[handle->port - 1U];

  // Retired events belong to an open token, not to whoever next opens ID.
  for (unsigned int i = 0U; i < runtime->retired_depth; i++) {
    unsigned int index = (runtime->retired_head + i) % PORT_RETIRED_CAPACITY;
    if (runtime->retired_tokens[index] != handle->open_token)
      continue;
    *event = runtime->retired[index];
    retired_remove(runtime, i);
    return MIDI_STATUS_OK;
  }

  status = handle_current(handle, &current);
  if (status != MIDI_STATUS_OK)
    return status;

  midi_input_event_t received = {0};

  if (!midi_input_read_cable(current.generation,
          current.key.cable, &received)) {
    midi_input_info_t input = {0};
    if (!midi_input_get_info(current.generation, &input))
      return MIDI_STATUS_FAILED;
    if (input.lost)
      return MIDI_STATUS_DISCONNECTED;
    return input.attached ? MIDI_STATUS_EMPTY : MIDI_STATUS_NOT_OPEN;
  }

  *event = (midi_port_event_t){
    .port = handle->port,
    .generation = received.generation,
    .event = received.event
  };

  return MIDI_STATUS_OK;
}

midi_status_t midi_port_next_change(unsigned int id, midi_port_change_t *change) {
  if (!change)
    return MIDI_STATUS_INVALID_ARGUMENT;

  *change = (midi_port_change_t){0};

  midi_port_info_t current = {0};
  midi_status_t status = port_lookup(id, &current);
  if (status != MIDI_STATUS_OK)
    return status;

  port_runtime_t *runtime = &runtimes[id - 1U];

  if (runtime->change_depth == 0U)
    return MIDI_STATUS_EMPTY;

  *change = runtime->changes[runtime->change_head];

  runtime->change_head = (runtime->change_head + 1U) % PORT_CHANGE_CAPACITY;
  runtime->change_depth--;
  return MIDI_STATUS_OK;
}

midi_status_t midi_port_lifecycle_get_info(unsigned int id,
    midi_port_lifecycle_info_t *info) {
  if (!info)
    return MIDI_STATUS_INVALID_ARGUMENT;

  *info = (midi_port_lifecycle_info_t){0};

  midi_port_info_t current = {0};
  midi_status_t status = port_lookup(id, &current);
  if (status != MIDI_STATUS_OK)
    return status;

  port_runtime_t *runtime = &runtimes[id - 1U];

  *info = (midi_port_lifecycle_info_t){
    .opened_generation = runtime->opened_generation,
    .retired_depth = runtime->retired_depth,
    .retired_dropped = runtime->retired_dropped,
    .retirement_missed = runtime->retirement_missed,
    .pending_changes = runtime->change_depth,
    .changes_dropped = runtime->changes_dropped
  };

  return MIDI_STATUS_OK;
}

midi_status_t midi_port_sysex_begin(const midi_port_handle_t *handle,
    midi_port_sysex_t *session) {
  if (!session)
    return MIDI_STATUS_INVALID_ARGUMENT;
  if (session->active)
    return MIDI_STATUS_BUSY;

  uint32_t generation = 0U;
  uint8_t cable = 0U;

  midi_status_t status = port_target(handle, MIDI_PORT_OUT, &generation, &cable);
  if (status != MIDI_STATUS_OK)
    return status;

  midi_port_info_t current = {0};

  if (!midi_ports_get(handle->port, &current))
    return MIDI_STATUS_FAILED;

  midi_output_info_t output = {0};
  status = output_ready(generation, &output);
  if (status != MIDI_STATUS_OK)
    return status;
  if (output.sysex_active)
    return MIDI_STATUS_BUSY;

  uint32_t claim = 0U;

  if (!midi_output_sysex_claim(generation, cable, &claim))
    return MIDI_STATUS_FAILED;

  *session = (midi_port_sysex_t){
    .active = true,
    .port = handle->port,
    .definition_serial = current.definition_serial,
    .generation = generation,
    .cable = cable,
    .claim = claim,
    .open_token = handle->open_token
  };

  return MIDI_STATUS_OK;
}

static midi_status_t sysex_target_current(const midi_port_sysex_t *session) {
  if (!session)
    return MIDI_STATUS_INVALID_ARGUMENT;
  if (!session->active || session->claim == 0U)
    return MIDI_STATUS_NOT_OPEN;
  midi_port_handle_t handle = {
    .active = true,
    .port = session->port,
    .definition_serial = session->definition_serial,
    .generation = session->generation,
    .open_token = session->open_token,
    .cable = session->cable,
    .direction = MIDI_PORT_OUT
  };
  uint32_t generation;
  uint8_t cable;
  midi_status_t status = port_target(&handle, MIDI_PORT_OUT, &generation, &cable);
  if (status != MIDI_STATUS_OK)
    return status;
  midi_output_info_t output = {0};
  status = output_ready(generation, &output);
  if (status != MIDI_STATUS_OK)
    return status;
  if (!output.sysex_active || output.sysex_claim != session->claim)
    return MIDI_STATUS_NOT_OPEN;
  return MIDI_STATUS_OK;
}

midi_status_t midi_port_sysex_write(midi_port_sysex_t *session, uint8_t byte) {
  if (byte >= 0x80U)
    return MIDI_STATUS_INVALID_ARGUMENT;
  midi_status_t status = sysex_target_current(session);
  if (status != MIDI_STATUS_OK)
    return status;
  return midi_output_sysex_write_claim(session->generation, session->claim, byte) ?
      MIDI_STATUS_OK : output_rejected(session->generation);
}

midi_status_t midi_port_sysex_end(midi_port_sysex_t *session) {
  midi_status_t status = sysex_target_current(session);
  if (status != MIDI_STATUS_OK)
    return status;
  if (!midi_output_sysex_end_claim(session->generation, session->claim))
    return output_rejected(session->generation);
  session->active = false;
  return MIDI_STATUS_OK;
}

midi_status_t midi_port_sysex_cancel(midi_port_sysex_t *session) {
  if (!session)
    return MIDI_STATUS_INVALID_ARGUMENT;
  if (!session->active || session->claim == 0U)
    return MIDI_STATUS_NOT_OPEN;

  if (midi_output_sysex_cancel_claim(session->generation, session->claim)) {
    session->active = false;
    return MIDI_STATUS_OK;
  }

  // The original driver record might already have
  // been recycled, or another session may have
  // replaced this claim. Never target a replacement.
  midi_output_info_t info = {0};

  if (!midi_output_get_info(session->generation, &info) ||
      !info.sysex_active || info.sysex_claim != session->claim) {
    session->active = false;
    return MIDI_STATUS_OK;
  }

  // The original claim is still active.
  // Most importantly, a full output queue may
  // require the caller to retry cancellation.
  return output_rejected(session->generation);
}

