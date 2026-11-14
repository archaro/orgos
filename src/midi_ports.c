#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midi_ports.h"

static midi_port_info_t ports[MIDI_PORT_CAPACITY];

static bool valid_id(unsigned int id) {
  return id >= 1U && id <= MIDI_PORT_CAPACITY;
}

static bool same_key(const midi_port_key_t *a, const midi_port_key_t *b) {
  return a->root_port == b->root_port &&
      a->hub_port == b->hub_port &&
      a->vendor_id == b->vendor_id &&
      a->product_id == b->product_id &&
      a->cable == b->cable &&
      a->direction == b->direction;
}

bool midi_ports_define(unsigned int id, const midi_port_key_t *key) {
  if (!valid_id(id) || !key || key->root_port == 0U || key->hub_port == 0U ||
      key->cable >= MIDI_PORT_MAX_CABLES || (key->direction != MIDI_PORT_IN &&
       key->direction != MIDI_PORT_OUT) || ports[id - 1U].id != 0U)
    return false;

  for (unsigned int i = 0U; i < MIDI_PORT_CAPACITY; i++) {
    if (ports[i].id != 0U && same_key(&ports[i].key, key))
      return false;
  }

  ports[id - 1U] = (midi_port_info_t){
    .id = id,
    .key = *key,
    .state = MIDI_PORT_UNBOUND
  };
  return true;
}

bool midi_ports_clear(unsigned int id) {
  if (!valid_id(id) || ports[id - 1U].id == 0U)
    return false;

  ports[id - 1U] = (midi_port_info_t){0};
  return true;
}

bool midi_ports_get(unsigned int id, midi_port_info_t *info) {
  if (!info)
    return false;

  *info = (midi_port_info_t){0};
  if (!valid_id(id) || ports[id - 1U].id == 0U)
    return false;

  *info = ports[id - 1U];
  return true;
}

static bool matches(const midi_port_key_t *key,
    const midi_port_device_t *device) {
  if (!device->generation || !device->slot_id ||
      device->root_port != key->root_port ||
      device->hub_port != key->hub_port ||
      device->vendor_id != key->vendor_id ||
      device->product_id != key->product_id)
    return false;

  unsigned int cables = key->direction == MIDI_PORT_IN ?
      device->in_cables : device->out_cables;

  return cables <= MIDI_PORT_MAX_CABLES && key->cable < cables;
}

bool midi_ports_reconcile(const midi_port_device_t *devices, size_t count) {
  if (!devices && count != 0U)
    return false;

  for (unsigned int i = 0U; i < MIDI_PORT_CAPACITY; i++) {
    midi_port_info_t *port = &ports[i];
    if (port->id == 0U)
      continue;

    port->state = MIDI_PORT_UNBOUND;
    port->generation = 0U;
    port->slot_id = 0U;

    for (size_t j = 0U; j < count; j++) {
      if (!matches(&port->key, &devices[j]))
        continue;

      if (port->state == MIDI_PORT_BOUND) {
        // Ambiguous topology must never choose
        // whichever device was visited first.
        port->state = MIDI_PORT_AMBIGUOUS;
        port->generation = 0U;
        port->slot_id = 0U;
        break;
      }

      port->state = MIDI_PORT_BOUND;
      port->generation = devices[j].generation;
      port->slot_id = devices[j].slot_id;
    }
  }

  return true;
}

bool midi_ports_resolve(unsigned int id, uint32_t *generation,
    uint8_t *cable, midi_port_direction_t *direction) {
  if (!valid_id(id) || !generation || !cable || !direction)
    return false;

  const midi_port_info_t *port = &ports[id - 1U];
  if (port->id != id || port->state != MIDI_PORT_BOUND)
    return false;

  *generation = port->generation;
  *cable = port->key.cable;
  *direction = port->key.direction;
  return true;
}

