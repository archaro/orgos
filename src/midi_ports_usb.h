#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "midi_ports.h"

// Snapshot the currently enumerated physical endpoint
// and assign it to a logical ID. No hardware claim occurs.
bool midi_ports_usb_define(unsigned int id, midi_port_direction_t direction,
    uint32_t generation, unsigned int cable);

// Refresh all logical bindings from current USB topology.
void midi_ports_usb_service(void);
