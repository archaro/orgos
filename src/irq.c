
#include <stdint.h>

#include "gic.h"
#include "irq.h"
#include "uart.h"

static volatile uint64_t total_irqs;

uint64_t irq_count(void) {
  return total_irqs;
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

