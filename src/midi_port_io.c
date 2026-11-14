#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midi_port_io.h"
#include "midi_ports.h"
#include "midi_input.h"
#include "midi_output.h"
#include "usb_midi_rx.h"

#define PORT_RETIRED_CAPACITY (MIDI_INPUT_QUEUE_CAPACITY + 1U)
#define PORT_CHANGE_CAPACITY 8U

typedef struct {
  uint32_t serial;
  midi_port_info_t previous;
  uint32_t opened_generation;

  midi_port_event_t retired[PORT_RETIRED_CAPACITY];
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

      port_retire(runtime, id, runtime->previous.generation,
          runtime->previous.key.cable);
    }

    port_save_change(runtime, id, &current);

    // A previous open never authorises a different
    // binding, even on the same physical socket.
    runtime->opened_generation = 0U;
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

static bool port_target(unsigned int id, midi_port_direction_t expected,
    uint32_t *generation, uint8_t *cable) {

  midi_port_info_t current = {0};

  if (!port_update(id, &current) || current.state != MIDI_PORT_BOUND ||
      current.key.direction != expected)
    return false;

  // A previous open cannot authorise operations
  // on a newly bound USB generation.
  if (runtimes[id - 1U].opened_generation != current.generation)
    return false;

  *generation = current.generation;
  *cable = current.key.cable;
  return true;
}

bool midi_port_open(unsigned int id) {
  midi_port_info_t current = {0};

  if (!port_update(id, &current) || current.state != MIDI_PORT_BOUND)
    return false;

  uint32_t generation = current.generation;
  uint8_t cable = current.key.cable;
  bool opened = false;

  if (current.key.direction == MIDI_PORT_OUT) {
    midi_output_info_t output = {0};

    if (midi_output_get_info(generation, &output) &&
        output.state == MIDI_OUTPUT_RUNNING)
      opened = cable < output.cables;
    else
      opened = midi_output_start_generation(generation);

  } else if (current.key.direction == MIDI_PORT_IN) {
    midi_input_info_t input = {0};

    if (midi_input_get_info(generation, &input) && input.attached) {
      opened = cable < input.cables;
    } else {
      usb_midi_rx_info_t rx = {0};

      if (!usb_midi_rx_get_info(generation, &rx) ||
          rx.state != USB_MIDI_RX_RUNNING) {
        if (!usb_midi_rx_start_generation(generation))
          return false;
      }

      opened = midi_input_start_generation(generation);
    }
  }

  if (opened)
    runtimes[id - 1U].opened_generation = generation;

  return opened;
}

bool midi_port_send_short(unsigned int id,
    const midi_short_message_t *message) {
  uint32_t generation = 0U;
  uint8_t cable = 0U;

  if (!port_target(id, MIDI_PORT_OUT, &generation, &cable))
    return false;

  return midi_output_enqueue_short(generation, cable, message);
}

bool midi_port_read(unsigned int id, midi_port_event_t *event) {
  if (!event)
    return false;

  *event = (midi_port_event_t){0};

  midi_port_info_t current = {0};

  if (!port_update(id, &current) || current.key.direction != MIDI_PORT_IN)
    return false;

  port_runtime_t *runtime = &runtimes[id - 1U];

  // Retired events precede all events received
  // from a replacement device generation.
  if (runtime->retired_depth != 0U) {
    *event = runtime->retired[runtime->retired_head];

    runtime->retired_head =
        (runtime->retired_head + 1U) % PORT_RETIRED_CAPACITY;
    runtime->retired_depth--;
    return true;
  }

  if (current.state != MIDI_PORT_BOUND ||
      runtime->opened_generation != current.generation)
    return false;

  midi_input_event_t received = {0};

  if (!midi_input_read_cable(current.generation,
          current.key.cable, &received))
    return false;

  *event = (midi_port_event_t){
    .port = id,
    .generation = received.generation,
    .event = received.event
  };

  return true;
}

bool midi_port_next_change(unsigned int id, midi_port_change_t *change) {
  if (!change)
    return false;

  *change = (midi_port_change_t){0};

  if (!port_update(id, NULL))
    return false;

  port_runtime_t *runtime = &runtimes[id - 1U];

  if (runtime->change_depth == 0U)
    return false;

  *change = runtime->changes[runtime->change_head];

  runtime->change_head = (runtime->change_head + 1U) % PORT_CHANGE_CAPACITY;
  runtime->change_depth--;
  return true;
}

bool midi_port_lifecycle_get_info(unsigned int id,
    midi_port_lifecycle_info_t *info) {
  if (!info)
    return false;

  *info = (midi_port_lifecycle_info_t){0};

  if (!port_update(id, NULL))
    return false;

  port_runtime_t *runtime = &runtimes[id - 1U];

  *info = (midi_port_lifecycle_info_t){
    .opened_generation = runtime->opened_generation,
    .retired_depth = runtime->retired_depth,
    .retired_dropped = runtime->retired_dropped,
    .retirement_missed = runtime->retirement_missed,
    .pending_changes = runtime->change_depth,
    .changes_dropped = runtime->changes_dropped
  };

  return true;
}

bool midi_port_sysex_begin(unsigned int id, midi_port_sysex_t *session) {
  if (!session || session->active)
    return false;

  uint32_t generation = 0U;
  uint8_t cable = 0U;

  if (!port_target(id, MIDI_PORT_OUT, &generation, &cable))
    return false;

  midi_port_info_t current = {0};

  if (!midi_ports_get(id, &current))
    return false;

  uint32_t claim = 0U;

  if (!midi_output_sysex_claim(generation, cable, &claim))
    return false;

  *session = (midi_port_sysex_t){
    .active = true,
    .port = id,
    .definition_serial = current.definition_serial,
    .generation = generation,
    .cable = cable,
    .claim = claim
  };

  return true;
}

static bool sysex_target_current(const midi_port_sysex_t *session) {
  if (!session || !session->active || session->claim == 0U)
    return false;

  midi_port_info_t current = {0};

  if (!port_update(session->port, &current))
    return false;

  if (current.definition_serial != session->definition_serial ||
      current.state != MIDI_PORT_BOUND ||
      current.key.direction != MIDI_PORT_OUT ||
      current.generation != session->generation ||
      current.key.cable != session->cable)
    return false;

  return runtimes[session->port - 1U].opened_generation ==
      session->generation;
}

bool midi_port_sysex_write(midi_port_sysex_t *session, uint8_t byte) {

  if (!sysex_target_current(session))
    return false;

  return midi_output_sysex_write_claim(
      session->generation, session->claim, byte);
}

bool midi_port_sysex_end(midi_port_sysex_t *session) {
  if (!sysex_target_current(session))
    return false;

  if (!midi_output_sysex_end_claim(session->generation, session->claim))
    return false;

  session->active = false;
  return true;
}

bool midi_port_sysex_cancel(midi_port_sysex_t *session) {
  if (!session || !session->active || session->claim == 0U)
    return false;

  if (midi_output_sysex_cancel_claim(session->generation, session->claim)) {
    session->active = false;
    return true;
  }

  // The original driver record might already have
  // been recycled, or another session may have
  // replaced this claim. Never target a replacement.
  midi_output_info_t info = {0};

  if (!midi_output_get_info(session->generation, &info) ||
      !info.sysex_active || info.sysex_claim != session->claim) {
    session->active = false;
    return true;
  }

  // The original claim is still active.
  // Most importantly, a full output queue may
  // require the caller to retry cancellation.
  return false;
}

