#include <stdint.h>

#include "mmio.h"
#include "gic.h"

#define GICD_BASE 0xff841000UL
#define GICC_BASE 0xff842000UL

// GIC distributor registers.
#define GICD_CTLR       (GICD_BASE + 0x000)
#define GICD_TYPER      (GICD_BASE + 0x004)
#define GICD_ISENABLER  (GICD_BASE + 0x100)
#define GICD_ICENABLER  (GICD_BASE + 0x180)
#define GICD_ISPENDR    (GICD_BASE + 0x200)
#define GICD_IPRIORITYR (GICD_BASE + 0x400)
#define GICD_ITARGETSR  (GICD_BASE + 0x800)
#define GICD_ICFGR      (GICD_BASE + 0xc00)

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

  // The firmware establishes interrupt security groups
  // before entering Non-secure execution.
  // GICD_IGROUPR is not writable from our EL1 context.

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

uint32_t gic_interrupt_capacity(void) {
  uint32_t typer = mmio_read(GICD_TYPER);

  return ((typer & 0x1fU) + 1U) * 32U;
}

bool gic_get_pcie_msi_info(gic_pcie_msi_info_t *info) {
  if (!info)
    return false;

  *info = (gic_pcie_msi_info_t){0};

  const uint32_t id = GIC_IRQ_PCIE_MSI;
  uint32_t supported = gic_interrupt_capacity();

  if (id >= supported)
    return false;

  uint32_t bit = 1U << (id % 32U);
  uint32_t offset = 4U * (id / 32U);
  info->enabled = (mmio_read(GICD_ISENABLER + offset) & bit) != 0U;
  info->pending = (mmio_read(GICD_ISPENDR + offset) & bit) != 0U;
  info->priority = *(volatile uint8_t *)(GICD_IPRIORITYR + id);
  info->targets = *(volatile uint8_t *)(GICD_ITARGETSR + id);

  uint32_t shift = (id % 16U) * 2U;
  uint32_t config = mmio_read(GICD_ICFGR + 4U * (id / 16U));

  info->edge_triggered = (config & (2U << shift)) != 0U;

  return true;
}

bool gic_prepare_pcie_msi(void) {
  const uint32_t id = GIC_IRQ_PCIE_MSI;

  uint32_t supported = gic_interrupt_capacity();
  if (id >= supported)
    return false;

  uint32_t bit = 1U << (id % 32U);
  uint32_t offset = 4U * (id / 32U);

  // Disable delivery while configuring the SPI.
  mmio_write(GICD_ICENABLER + offset, bit);
  __asm__ volatile("dsb sy" ::: "memory");

  // Interrupt security groups are configured by
  // Secure-state firmware, not by Non-secure EL1.

  // Give MSI the same priority as UART0.
  write_byte(GICD_IPRIORITYR + id, 0xa0U);

  // Route the shared interrupt to CPU interface 0.
  write_byte(GICD_ITARGETSR + id, 0x01U);

  // BCM2711 PCIe MSI uses level-high signalling.
  // In GICD_ICFGR, bit 2n+1 selects edge triggering.
  uint32_t shift = (id % 16U) * 2U;
  uintptr_t address = GICD_ICFGR + 4U * (id / 16U);
  uint32_t config = mmio_read(address);
  config &= ~(2U << shift);
  mmio_write(address, config);

  __asm__ volatile("dsb sy\nisb" ::: "memory");

  // Verify configuration without enabling delivery.
  gic_pcie_msi_info_t info;

  if (!gic_get_pcie_msi_info(&info))
    return false;

  return !info.enabled && !info.edge_triggered &&
      info.priority == 0xa0U && info.targets == 0x01U;
}

uint32_t gic_acknowledge(void) {
  return mmio_read(GICC_IAR);
}

void gic_end_irq(uint32_t token) {
// Write back the complete acknowledgement token,
// not merely the extracted interrupt ID.
  mmio_write(GICC_EOIR, token);
}

