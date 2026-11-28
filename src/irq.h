#pragma once

#include <stdint.h>
#include "exceptions.h"

uint32_t irq_pcie_msi_hardware_count(void);
uint32_t irq_pcie_msi_last_status(void);
uint64_t irq_pcie_msi_last_time_us(void);
uint64_t irq_count(void);
uint32_t irq_pcie_msi_count(void);
void irq_dispatch(exception_frame_t *frame);

// Same dispatch/acknowledgement/EOI path, returning the actual IAR source
// for an explicitly armed observer. 1023 denotes a spurious interrupt.
uint32_t irq_dispatch_observed(exception_frame_t *frame);
// Private fatal hardware boundary; never returns to an unowned source.
__attribute__((noreturn)) void irq_unexpected(uint32_t id, uint32_t token);
