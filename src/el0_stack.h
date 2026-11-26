#pragma once

#include "mmu.h"
#include "el0_layout.h"

typedef struct {
  paddr_t read_physical;
  paddr_t write_physical;
  bool checked;
  bool valid;
} el0_stack_probe_t;

// Fixed diagnostic only. Call under the selected context root with the
// two trusted static backing PAs. Never dereferences a user VA or returns
// a CPU pointer. Tests hardware EL0 AP translations and software XN bits.
bool el0_stack_probe(paddr_t backing, paddr_t peer, el0_stack_probe_t *probe);
