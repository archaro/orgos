#pragma once

#include <stdint.h>

uint32_t irq_pcie_msi_hardware_count(void);
uint32_t irq_pcie_msi_last_status(void);
uint64_t irq_count(void);
uint32_t irq_pcie_msi_count(void);
void irq_dispatch(void);
