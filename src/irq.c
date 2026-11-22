
#include <stdint.h>

#include "timer.h"
#include "uart.h"
#include "gic.h"
#include "irq.h"
#include "pcie.h"
#include "console.h"

static volatile uint64_t total_irqs;
static volatile uint32_t pcie_msi_irqs;
static volatile uint32_t pcie_msi_hardware_irqs;
static volatile uint32_t pcie_msi_last_vector_status;
static volatile uint64_t pcie_msi_last_time_us;

uint32_t irq_pcie_msi_hardware_count(void) {
  return pcie_msi_hardware_irqs;
}

uint32_t irq_pcie_msi_last_status(void) {
  return pcie_msi_last_vector_status;
}

uint64_t irq_pcie_msi_last_time_us(void) {
  return pcie_msi_last_time_us;
}

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
    case GIC_IRQ_PCIE_MSI: {
      // This handler serves both software-pending
      // tests and subsequent hardware MSI tests.
      //
      // Quiesce the Broadcom receiver before
      // completing the level-triggered GIC IRQ.
      uint32_t pending = pcie_msi_host_irq_quiesce();
      pcie_msi_irqs++;
      if (pending != 0U) {
        // Timestamp this notification before publishing
        // the new sequence number to the foreground.
        pcie_msi_last_time_us = timer_now_us();
        pcie_msi_last_vector_status = pending;
        pcie_msi_hardware_irqs++;
      }
      // Keep the route disabled until explicitly
      // prepared again by the foreground console.
      gic_pcie_msi_test_finish();
      break;
    }
    default:
      // No handler owns this source. Mask exceptions before the polled
      // UART diagnostic, then halt without clearing the source or EOI.
      __asm__ volatile("msr daifset, #0xf" ::: "memory");
      console_puts("\nUnexpected IRQ ");
      console_putu32(id);
      console_puts(" (IAR token 0x");
      console_puthex(token, 8U);
      console_puts("). System halted.\n");
      __asm__ volatile("dsb sy" ::: "memory");
      for (;;) {
        __asm__ volatile("wfe");
      }
  }

  // Tell the GIC that we have finished servicing this interrupt.
  gic_end_irq(token);
}

