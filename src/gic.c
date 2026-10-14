#include <stdint.h>

#include "mmio.h"
#include "gic.h"

#define GICD_BASE 0xff841000UL
#define GICC_BASE 0xff842000UL

// GIC distributor registers.
#define GICD_CTLR       (GICD_BASE + 0x000)
#define GICD_IGROUPR    (GICD_BASE + 0x080)
#define GICD_ISENABLER  (GICD_BASE + 0x100)
#define GICD_ICENABLER  (GICD_BASE + 0x180)
#define GICD_IPRIORITYR (GICD_BASE + 0x400)
#define GICD_ITARGETSR  (GICD_BASE + 0x800)

// GIC CPU-interface registers.
#define GICC_CTLR       (GICC_BASE + 0x000)
#define GICC_PMR        (GICC_BASE + 0x004)
#define GICC_IAR        (GICC_BASE + 0x00c)
#define GICC_EOIR       (GICC_BASE + 0x010)

static inline void write_byte(uintptr_t address, uint8_t value) {
  *(volatile uint8_t *)address = value;
}

void gic_init(void) {
  const uint32_t id = GIC_IRQ_UART0;

// Begin with our intended IRQ disabled.
// ISENABLER / ICENABLER are write-one registers.
// Writing one bit changes only that interrupt.
  mmio_write(GICD_ICENABLER + 4U * (id / 32U), 1U << (id % 32U));

// Place UART0 in interrupt Group 1.
// Group 1 is the ordinary non-secure IRQ group
// used by our EL1 kernel.
  uint32_t group_reg = GICD_IGROUPR + 4U * (id / 32U);

  uint32_t value = mmio_read(group_reg);
  value |= 1U << (id % 32U);
  mmio_write(group_reg, value);

// Give UART0 priority 0xa0.
// Smaller numerical priorities represent
// more urgent interrupts in the GIC.
  write_byte(GICD_IPRIORITYR + id, 0xa0);

// Direct this shared interrupt to core 0.
// Each SPI has an 8-bit target mask.
// Bit zero corresponds to CPU interface 0.
  write_byte(GICD_ITARGETSR + id, 0x01);

// Permit all implemented interrupt priorities
// through the CPU interface.
  mmio_write(GICC_PMR, 0xff);

// Enable Group 1 delivery.
// In the non-secure GICv2 register view,
// bit 0 enables Group 1 interrupts.
  mmio_write(GICC_CTLR, 1U);
  mmio_write(GICD_CTLR, 1U);

  __asm__ volatile("dsb sy\nisb" ::: "memory");
}

void gic_enable_irq(uint32_t id) {
  mmio_write(GICD_ISENABLER + 4U * (id / 32U), 1U << (id % 32U));

  __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t gic_acknowledge(void) {
  return mmio_read(GICC_IAR);
}

void gic_end_irq(uint32_t token) {
// Write back the complete acknowledgement token,
// not merely the extracted interrupt ID.
  mmio_write(GICC_EOIR, token);
}

