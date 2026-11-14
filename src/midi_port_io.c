#include <stdbool.h>
#include <stdint.h>

#include "midi_port_io.h"
#include "midi_ports.h"
#include "midi_input.h"
#include "midi_output.h"
#include "usb_midi_rx.h"

static bool port_target(unsigned int id, midi_port_direction_t expected,
    uint32_t *generation, uint8_t *cable) {

  midi_port_direction_t actual = MIDI_PORT_IN;

  return midi_ports_resolve(id, generation, cable, &actual) &&
      actual == expected;
}

bool midi_port_open(unsigned int id) {
  uint32_t generation = 0U;
  uint8_t cable = 0U;
  midi_port_direction_t direction = MIDI_PORT_IN;

  if (!midi_ports_resolve(id, &generation, &cable, &direction))
    return false;

  if (direction == MIDI_PORT_OUT) {
    midi_output_info_t info = {0};

    if (midi_output_get_info(generation, &info) &&
        info.state == MIDI_OUTPUT_RUNNING)
      return cable < info.cables;

    return midi_output_start_generation(generation);
  }

  if (direction != MIDI_PORT_IN)
    return false;

  midi_input_info_t input = {0};

  if (midi_input_get_info(generation, &input) && input.attached)
    return cable < input.cables;

  usb_midi_rx_info_t rx = {0};

  if (!usb_midi_rx_get_info(generation, &rx) ||
      rx.state != USB_MIDI_RX_RUNNING) {
    if (!usb_midi_rx_start_generation(generation))
      return false;
  }

  return midi_input_start_generation(generation);
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

  uint32_t generation = 0U;
  uint8_t cable = 0U;

  if (!port_target(id, MIDI_PORT_IN, &generation, &cable))
    return false;

  midi_input_event_t received = {0};

  if (!midi_input_read_cable(generation, cable, &received))
    return false;

  *event = (midi_port_event_t){
    .port = id,
    .generation = received.generation,
    .event = received.event
  };

  return true;
}
