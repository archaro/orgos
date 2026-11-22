#pragma once

#include <stddef.h>

#include "midi.h"

// Internal provisioning and binding registry. Applications discover existing
// definitions through midi.h; they do not supply topology snapshots.

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

// Identities are explicit and stable within a boot.
// Redefining an occupied ID or aliasing an endpoint
// in the same direction is rejected.
bool midi_ports_define(unsigned int id, const midi_port_key_t *key);
bool midi_ports_get(unsigned int id, midi_port_info_t *info);

// Reconcile all defined ports against the *complete*
// snapshot of eligible physical devices.
bool midi_ports_reconcile(const midi_port_device_t *devices, size_t count);

// Resolve only an unambiguous, currently bound port.
// This does not claim or start the input/output service.
bool midi_ports_resolve(unsigned int id, uint32_t *generation,
    uint8_t *cable, midi_port_direction_t *direction);

// Close and remove a logical-port definition.
//
// Refuses removal if an active output SysEx
// session cannot be released safely.
midi_status_t midi_port_undefine(unsigned int id);
