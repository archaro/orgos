
#include <stdint.h>

#include "gic.h"
#include "irq.h"
#include "uart.h"

static volatile uint64_t total_irqs;
static volatile uint32_t pcie_msi_irqs;

uint64_t irq_count(void) {
  return total_irqs;
}

uint32_t irq_pcie_msi_count(void) {
  return pcie_msi_irqs;
}

void irq_dispatch(void) {
  // Read GICC_IAR to acknowledge the highest-priority
  // pending interrupt.
  // Bits [9:0] contain the interrupt identifier.
  uint32_t token = gic_acknowledge();
  uint32_t id = token & 0x3ffU;

  // ID 1023 indicates a spurious interrupt:
  // there is nothing to acknowledge or finish.
  if (id == 1023U)
    return;

  total_irqs++;

  switch (id) {
    case GIC_IRQ_UART0:
      uart_handle_irq();
      break;
    case GIC_IRQ_PCIE_MSI:
      // This is currently a one-shot software
      // interrupt test, not a PCIe ISR.
      pcie_msi_irqs++;
      gic_pcie_msi_test_finish();
      break;
    default:
      // We have no handler for this interrupt.
      // Do not silently treat an unexpected interrupt
      // as successfully handled.
      for (;;) {
        __asm__ volatile("wfe");
      }
  }

  // Tell the GIC that we have finished servicing this interrupt.
  gic_end_irq(token);
}

