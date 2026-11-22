#pragma once

#include "midi_ports.h"

// Registry-only operation. Callers outside the
// logical-port implementation must use
// midi_port_undefine() instead.
bool midi_ports_clear_registry(unsigned int id);
