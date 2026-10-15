
#include <stdint.h>
#include <stdbool.h>

#include "mmio.h"
#include "timer.h"
#include "pcie.h"

//BCM2711 PCIe root-complex registers.
//The controller occupies the physical MMIO region
//beginning at 0xFD500000.
//OrgOS currently runs with the MMU disabled, so
//physical and CPU-visible addresses coincide.
//Based on the Linux Broadcom PCIe driver
#define PCIE_BASE         0xfd500000UL

#define PCIE_STATUS       (PCIE_BASE + 0x4068)
#define PCIE_REVISION     (PCIE_BASE + 0x406c)

//Extended configuration access.
//INDEX selects the bus/device/function.
//DATA maps the selected device's config space.
#define PCIE_CFG_INDEX    (PCIE_BASE + 0x9000)
#define PCIE_CFG_DATA     (PCIE_BASE + 0x8000)

//PCI configuration-space offsets.
#define PCI_VENDOR_DEVICE 0x00
#define PCI_CLASS_REV     0x08
#define PCI_BUS_NUMBERS   0x18
#define PCI_BAR0          0x10
#define PCI_BAR1          0x14

//PCIe link status bits.
//Bit 7: Controller in root-complex mode
//Bit 5: Data Link Layer active
//Bit 4: PHY reports link up
#define PCIE_STATUS_RC    (1U << 7)
#define PCIE_STATUS_DL    (1U << 5)
#define PCIE_STATUS_PHY   (1U << 4)

// BCM2711 PCIe bridge reset register.
// Bit 1: bridge software initialization/reset.
// Bit 0: downstream PERST# control.
// Both controls are active-high requests:
// setting a bit asserts the corresponding reset.
#define PCIE_SW_INIT       (PCIE_BASE + 0x9210)
#define PCIE_SW_INIT_BRIDGE (1U << 1)
#define PCIE_SW_INIT_PERST  (1U << 0)

// PCIe SerDes and miscellaneous configuration.
#define PCIE_HARD_DEBUG   (PCIE_BASE + 0x4204)
#define PCIE_MISC_CTRL    (PCIE_BASE + 0x4008)

#define PCIE_SERDES_IDDQ  (1U << 27)

#define PCIE_SCB_ACCESS   (1U << 12)
#define PCIE_CFG_READ_UR  (1U << 13)
#define PCIE_RCB_MPS      (1U << 10)
#define PCIE_RCB_64B      (1U << 7)

#define PCIE_BURST_MASK   (3U << 20)

//Read a register from the root-complex configuration
//space, which is directly mapped at PCIE_BASE.
static uint32_t root_config_read(uint32_t offset)
{
  return mmio_read(PCIE_BASE + offset);
}

//Read a 32-bit configuration register from a
//downstream PCIe device.
//The Broadcom controller requires us to select a
//bus/device/function using the extended config index.
//Only call this after confirming the link is up.
static uint32_t device_config_read(uint8_t bus, uint8_t device, uint8_t function, uint32_t offset) {
  uint32_t index =
      ((uint32_t)bus << 20) |
      ((uint32_t)device << 15) |
      ((uint32_t)function << 12);

  mmio_write(PCIE_CFG_INDEX, index);

  //Ensure the index write completes before reading
  //the selected configuration register.
  __asm__ volatile("dsb sy" ::: "memory");

  return mmio_read(PCIE_CFG_DATA + offset);
}

void pcie_probe(pcie_info_t *info) {
  if (!info)
    return;

  //Initialise all results explicitly.
  *info = (pcie_info_t){0};

  //Inspect the controller's current state.
  //We do not change resets, clocks, address windows
  //or controller configuration at this stage.
  info->status = mmio_read(PCIE_STATUS);
  info->revision = mmio_read(PCIE_REVISION);

  info->root_complex = (info->status & PCIE_STATUS_RC) != 0;

  info->link_up = (info->status & (PCIE_STATUS_DL | PCIE_STATUS_PHY)) ==
     (PCIE_STATUS_DL | PCIE_STATUS_PHY);

  //If the link is inactive, stop here.
  //Linux's Broadcom driver specifically avoids
  //downstream configuration reads when the link
  //is down because they can cause a CPU abort.
  if (!info->root_complex || !info->link_up)
    return;

  //Read the root bridge's PCI identification.
  uint32_t id = root_config_read(PCI_VENDOR_DEVICE);

  info->root_vendor = (uint16_t)(id & 0xffffU);
  info->root_device = (uint16_t)(id >> 16);

  //A Type 1 PCI bridge contains primary,
  //secondary and subordinate bus numbers.
  uint32_t buses = root_config_read(PCI_BUS_NUMBERS);

  info->secondary_bus = (uint8_t)((buses >> 8) & 0xffU);

  info->subordinate_bus = (uint8_t)((buses >> 16) & 0xffU);

  //If the firmware hasn't configured a downstream
  //bus number, don't invent one.
  if (info->secondary_bus == 0 ||
      info->subordinate_bus < info->secondary_bus)
    return;

  //On a Raspberry Pi 4B, the VL805 is normally
  //the first device (device 0, function 0)
  //on the downstream bus.
  id = device_config_read(info->secondary_bus, 0, 0, PCI_VENDOR_DEVICE);

  //PCI configuration reads commonly return all
  //ones when no device responds.
  if (id == 0xffffffffU || (id & 0xffffU) == 0xffffU)
    return;

  info->endpoint_present = true;
  info->endpoint_vendor = (uint16_t)(id & 0xffffU);
  info->endpoint_device = (uint16_t)(id >> 16);

  //Class code occupies bits [31:8].
  //USB xHCI class/subclass/interface = 0x0C0330.
  uint32_t class_rev = device_config_read(
      info->secondary_bus, 0, 0,
      PCI_CLASS_REV);

  info->endpoint_class = class_rev >> 8;

  //Read the BARs without writing to them.
  //These may be unassigned if the host bridge
  //has not been configured.
  info->endpoint_bar0 = device_config_read(
      info->secondary_bus, 0, 0, PCI_BAR0);

  info->endpoint_bar1 = device_config_read(
      info->secondary_bus, 0, 0, PCI_BAR1);
}

// Change selected bits in a 32-bit MMIO register
// without disturbing the remaining bits.
static void pcie_update_bits(uintptr_t address, uint32_t clear_mask, uint32_t set_mask) {
  uint32_t value = mmio_read(address);

  value &= ~clear_mask;
  value |= set_mask;
  mmio_write(address, value);
  __asm__ volatile("dsb sy" ::: "memory");
}


pcie_init_result_t pcie_init_link(void) {
  uint32_t status = mmio_read(PCIE_STATUS);

  // Avoid resetting an already functioning link.
  // In the future, xHCI may be running on it,
  // so an unnecessary reset would be destructive.
  if ((status & (PCIE_STATUS_RC | PCIE_STATUS_DL | PCIE_STATUS_PHY)) ==
                (PCIE_STATUS_RC | PCIE_STATUS_DL | PCIE_STATUS_PHY))
    return PCIE_INIT_ALREADY_UP;

  // Assert bridge reset and endpoint PERST#.
  // On BCM2711 both are controlled through the
  // RGR1 software initialization register.
  pcie_update_bits(PCIE_SW_INIT, 0, PCIE_SW_INIT_BRIDGE | PCIE_SW_INIT_PERST);
  timer_delay_us(150);

  // Release the bridge itself from reset.
  // Keep the downstream VL805 in reset while
  // configuring the root complex.
  pcie_update_bits(PCIE_SW_INIT, PCIE_SW_INIT_BRIDGE, 0);

  // Wake the PCIe SerDes from IDDQ power-down.
  pcie_update_bits(PCIE_HARD_DEBUG, PCIE_SERDES_IDDQ, 0);
  timer_delay_us(150);

  // Configure the basic PCIe controller mode.
  // BCM2711:
  //   SCB maximum burst size = 128 bytes (00)
  //   Unsupported config reads permitted to report UR
  //   SCB access enabled
  //   RCB modes enabled
  // This does not configure DMA translation windows.
  pcie_update_bits(PCIE_MISC_CTRL, PCIE_BURST_MASK,
      PCIE_SCB_ACCESS | PCIE_CFG_READ_UR | PCIE_RCB_MPS | PCIE_RCB_64B);

  // Verify the controller is in root-complex mode
  // before releasing the endpoint.
  status = mmio_read(PCIE_STATUS);

  if (!(status & PCIE_STATUS_RC))
    return PCIE_INIT_NOT_ROOT_COMPLEX;

  // Release the VL805 from fundamental reset.
  // This is the point at which its PCIe link
  // can begin training.
  pcie_update_bits( PCIE_SW_INIT, PCIE_SW_INIT_PERST, 0);

  // Give the downstream device time to respond
  // after reset, as required by PCIe.
  timer_delay_ms(100);

  // Poll for link establishment for up to a
  // further 100 milliseconds.
  // Both physical link-up and data-link-active
  // must be reported.
  uint64_t deadline = timer_now_us() + 100000U;

  do {
    status = mmio_read(PCIE_STATUS);

    if ((status & (PCIE_STATUS_PHY | PCIE_STATUS_DL)) ==
                  (PCIE_STATUS_PHY | PCIE_STATUS_DL))
      return PCIE_INIT_OK;

    timer_delay_ms(5);

  } while (timer_now_us() < deadline);

  return PCIE_INIT_LINK_TIMEOUT;
}

