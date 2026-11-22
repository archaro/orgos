#pragma once

#include "midi_ports.h"

// Registry-only operation. Callers outside the
// logical-port implementation must use
// midi_port_undefine() instead.
bool midi_ports_clear_registry(unsigned int id);

// Foreground coordination after the registry has inspected USB topology.
// This retires old application events and revokes obsolete open permissions.
void midi_port_io_service(void);
