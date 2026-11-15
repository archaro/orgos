#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GIC_IRQ_UART0 153U
#define GIC_IRQ_PCIE_MSI 180U

void gic_init(void);
void gic_enable_irq(uint32_t id);
uint32_t gic_interrupt_capacity(void);

typedef struct {
  bool enabled;
  bool pending;
  bool edge_triggered;
  uint8_t priority;
  uint8_t targets;
} gic_pcie_msi_info_t;

bool gic_prepare_pcie_msi(void);
bool gic_get_pcie_msi_info(gic_pcie_msi_info_t *info);

uint32_t gic_acknowledge(void);
void gic_end_irq(uint32_t token);
