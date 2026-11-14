#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIDI_PORT_CAPACITY 32U
#define MIDI_PORT_MAX_CABLES 16U

typedef enum {
  MIDI_PORT_IN,
  MIDI_PORT_OUT
} midi_port_direction_t;

typedef enum {
  MIDI_PORT_UNBOUND,
  MIDI_PORT_BOUND,
  MIDI_PORT_AMBIGUOUS
} midi_port_state_t;

// Physical socket identity within the current
// single-internal-hub USB topology.
typedef struct {
  uint8_t root_port;
  uint8_t hub_port;
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t cable;
  midi_port_direction_t direction;
} midi_port_key_t;

// One active, successfully enumerated USB-MIDI device.
// Supplied by the transport adapter; no USB types leak
// into the logical-port registry.
typedef struct {
  uint32_t generation;
  uint8_t slot_id;
  uint8_t root_port;
  uint8_t hub_port;
  uint16_t vendor_id;
  uint16_t product_id;
  uint8_t in_cables;
  uint8_t out_cables;
} midi_port_device_t;

typedef struct {
  unsigned int id;
  midi_port_key_t key;
  midi_port_state_t state;
  uint32_t generation;
  uint8_t slot_id;
  // Changes whenever this port ID is redefined.
  uint32_t definition_serial;
} midi_port_info_t;

// Identities are explicit and stable within a boot.
// Redefining an occupied ID or aliasing an endpoint
// in the same direction is rejected.
bool midi_ports_define(unsigned int id, const midi_port_key_t *key);
bool midi_ports_clear(unsigned int id);
bool midi_ports_get(unsigned int id, midi_port_info_t *info);

// Reconcile all defined ports against the *complete*
// snapshot of eligible physical devices.
bool midi_ports_reconcile(const midi_port_device_t *devices, size_t count);

// Resolve only an unambiguous, currently bound port.
// This does not claim or start the input/output service.
bool midi_ports_resolve(unsigned int id, uint32_t *generation,
    uint8_t *cable, midi_port_direction_t *direction);
