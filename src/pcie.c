
#include <stdint.h>
#include <stdbool.h>

#include "mmio.h"
#include "fdt.h"
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

// BCM2711 root-complex inbound windows.
// BAR2 is the principal system-memory DMA window.
#define PCIE_RC_BAR2_LO        (PCIE_BASE + 0x4034)
#define PCIE_RC_BAR2_HI        (PCIE_BASE + 0x4038)
#define PCIE_RC_BAR3_LO        (PCIE_BASE + 0x403c)

// Root-complex vendor-specific BAR2 endian control.
#define PCIE_RC_ENDIAN_CTRL    (PCIE_BASE + 0x0188)

#define PCIE_RC_BAR2_SIZE_MASK  0x1fU
#define PCIE_SCB0_SIZE_MASK    0xf8000000U
#define PCIE_SCB0_SIZE_SHIFT   27U
#define PCIE_BAR2_ENDIAN_MASK  0x0cU

#define PCIE_DMA_WINDOW_SIZE   0x100000000ULL
#define PCIE_DMA_VALID_SIZE    0xC0000000ULL
#define PCIE_DMA_BUS_BASE      0x100000000ULL

// log2(4 GiB) - 15
#define PCIE_DMA_SIZE_CODE     17U

#define PCIE_SERDES_IDDQ  (1U << 27)

#define PCIE_SCB_ACCESS   (1U << 12)
#define PCIE_CFG_READ_UR  (1U << 13)
#define PCIE_RCB_MPS      (1U << 10)
#define PCIE_RCB_64B      (1U << 7)

#define PCIE_BURST_MASK   (3U << 20)

/*
 * BCM2711 PCIe outbound MMIO aperture.
 *
 * CPU physical 0x600000000 maps to PCI bus 0xC0000000.
 * Window size: 64 MiB.
 */
#define PCIE_CPU_MMIO_BASE   0x600000000ULL
#define PCIE_BUS_MMIO_BASE   0xc0000000U
#define PCIE_MMIO_SIZE_MB    64U

/*
 * BCM2711 outbound window 0 registers.
 */
#define PCIE_MEM_WIN0_LO         (PCIE_BASE + 0x400c)
#define PCIE_MEM_WIN0_HI         (PCIE_BASE + 0x4010)
#define PCIE_MEM_WIN0_BASE_LIMIT (PCIE_BASE + 0x4070)
#define PCIE_MEM_WIN0_BASE_HI    (PCIE_BASE + 0x4080)
#define PCIE_MEM_WIN0_LIMIT_HI   (PCIE_BASE + 0x4084)

/*
 * PCI configuration-space registers.
 */
#define PCI_COMMAND             0x04U
#define PCI_MEMORY_BASE_LIMIT   0x20U

#define PCI_COMMAND_MEMORY      (1U << 1)

static bool xhci_resources_ready;
static bool dma_window_ready;

bool pcie_xhci_ready(void) {
  return xhci_resources_ready;
}

// Read a register from the root-complex configuration
// space, which is directly mapped at PCIE_BASE.
static uint32_t root_config_read(uint32_t offset) {
  return mmio_read(PCIE_BASE + offset);
}

// Read a 32-bit configuration register from a
// downstream PCIe device.
// The Broadcom controller requires us to select a
// bus/device/function using the extended config index.
// Only call this after confirming the link is up.
static uint32_t device_config_read(uint8_t bus, uint8_t device, uint8_t function, uint32_t offset) {
  uint32_t index = ((uint32_t)bus << 20) | ((uint32_t)device << 15) | ((uint32_t)function << 12);

  mmio_write(PCIE_CFG_INDEX, index);

  // Ensure the index write completes before reading
  // the selected configuration register.
  __asm__ volatile("dsb sy" ::: "memory");
  return mmio_read(PCIE_CFG_DATA + offset);
}

static void pcie_set_memory_decode(uint8_t bus, bool enabled) {
  uintptr_t address;

  if (bus == 0) {
    address = PCIE_BASE + PCI_COMMAND;
  } else {
    mmio_write(PCIE_CFG_INDEX, ((uint32_t)bus << 20));
    __asm__ volatile("dsb sy" ::: "memory");
    address = PCIE_CFG_DATA + PCI_COMMAND;
  }

  volatile uint16_t *command =
      (volatile uint16_t *)address;
  uint16_t value = *command;
  if (enabled)
    value |= PCI_COMMAND_MEMORY;
  else
    value &= (uint16_t)~PCI_COMMAND_MEMORY;
  *command = value;
  __asm__ volatile("dsb sy" ::: "memory");
}

// Write a 32-bit downstream configuration register.
// This mirrors device_config_read() but performs
// a write to the selected BDF.
static void device_config_write(uint8_t bus, uint8_t device, uint8_t function, uint32_t offset, uint32_t value) {
  uint32_t index = ((uint32_t)bus << 20) | ((uint32_t)device << 15) | ((uint32_t)function << 12);

  mmio_write(PCIE_CFG_INDEX, index);
  __asm__ volatile("dsb sy" ::: "memory");
  mmio_write(PCIE_CFG_DATA + offset, value);
  __asm__ volatile("dsb sy" ::: "memory");
}

void pcie_probe(pcie_info_t *info) {
  if (!info)
    return;

  // Initialise all results explicitly.
  *info = (pcie_info_t){0};

  // Inspect the controller's current state.
  // We do not change resets, clocks, address windows
  // or controller configuration at this stage.
  info->status = mmio_read(PCIE_STATUS);
  info->revision = mmio_read(PCIE_REVISION);

  info->root_complex = (info->status & PCIE_STATUS_RC) != 0;

  info->link_up = (info->status & (PCIE_STATUS_DL | PCIE_STATUS_PHY)) ==
     (PCIE_STATUS_DL | PCIE_STATUS_PHY);

  // If the link is inactive, stop here.
  // Linux's Broadcom driver specifically avoids
  // downstream configuration reads when the link
  // is down because they can cause a CPU abort.
  if (!info->root_complex || !info->link_up)
    return;

  // Read the root bridge's PCI identification.
  uint32_t id = root_config_read(PCI_VENDOR_DEVICE);

  info->root_vendor = (uint16_t)(id & 0xffffU);
  info->root_device = (uint16_t)(id >> 16);

  // A Type 1 PCI bridge contains primary,
  // secondary and subordinate bus numbers.
  uint32_t buses = root_config_read(PCI_BUS_NUMBERS);

  info->secondary_bus = (uint8_t)((buses >> 8) & 0xffU);

  info->subordinate_bus = (uint8_t)((buses >> 16) & 0xffU);

  // If the firmware hasn't configured a downstream
  // bus number, don't invent one.
  if (info->secondary_bus == 0 ||
      info->subordinate_bus < info->secondary_bus)
    return;

  // On a Raspberry Pi 4B, the VL805 is normally
  // the first device (device 0, function 0)
  // on the downstream bus.
  id = device_config_read(info->secondary_bus, 0, 0, PCI_VENDOR_DEVICE);

  // PCI configuration reads commonly return all
  // ones when no device responds.
  if (id == 0xffffffffU || (id & 0xffffU) == 0xffffU)
    return;

  info->endpoint_present = true;
  info->endpoint_vendor = (uint16_t)(id & 0xffffU);
  info->endpoint_device = (uint16_t)(id >> 16);

  // Class code occupies bits [31:8].
  // USB xHCI class/subclass/interface = 0x0C0330.
  uint32_t class_rev = device_config_read(
      info->secondary_bus, 0, 0,
      PCI_CLASS_REV);

  info->endpoint_class = class_rev >> 8;

  // Read the BARs without writing to them.
  // These may be unassigned if the host bridge
  // has not been configured.
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


pcie_map_result_t pcie_map_xhci(void) {
  pcie_info_t info;

  if (xhci_resources_ready)
    return PCIE_MAP_OK;
  // Only proceed when the link is active and
  // the expected VIA VL805 has been identified.
  pcie_probe(&info);

  if (!info.link_up)
    return PCIE_MAP_LINK_DOWN;

  // This code is tested on the CME U2MIDI Pro:
  // Other devices will need to be tested and enabled.
  if (!info.endpoint_present ||
      info.endpoint_vendor != 0x1106U ||
      info.endpoint_device != 0x3483U ||
      info.endpoint_class != 0x0c0330U)
    return PCIE_MAP_WRONG_DEVICE;

  // BAR0 should describe a 64-bit, non-I/O,
  // non-prefetchable memory resource.
  // PCI memory BAR bits [2:1] = 10 for 64-bit.
  if ((info.endpoint_bar0 & 0x0fU) != 0x04U)
    return PCIE_MAP_BAD_BAR;

  // Configure outbound window 0.
  // PCIe address base = 0xC0000000.
  // ARM physical base = 0x600000000.
  // BCM2711 base/limit registers express
  // CPU addresses in units of 1 MiB.
  uint64_t cpu_base_mb = PCIE_CPU_MMIO_BASE >> 20;
  uint64_t cpu_limit_mb = cpu_base_mb + PCIE_MMIO_SIZE_MB - 1U;

  mmio_write(PCIE_MEM_WIN0_LO, PCIE_BUS_MMIO_BASE);
  mmio_write(PCIE_MEM_WIN0_HI, 0);

  uint32_t base_limit =
      (uint32_t)((cpu_base_mb & 0xfffU) << 4) |
      (uint32_t)((cpu_limit_mb & 0xfffU) << 20);

  mmio_write(PCIE_MEM_WIN0_BASE_LIMIT, base_limit);
  mmio_write(PCIE_MEM_WIN0_BASE_HI, (uint32_t)(cpu_base_mb >> 12));
  mmio_write(PCIE_MEM_WIN0_LIMIT_HI, (uint32_t)(cpu_limit_mb >> 12));

  // Configure the root bridge's downstream
  // memory forwarding aperture.
  // Type 1 bridge memory-base/limit registers
  // have 1 MiB granularity.
  // 0xC000C000 describes a 1 MiB aperture
  // beginning at bus address 0xC0000000.
  // The VL805 occupies only 4 KiB, so 1 MiB
  // is enough for this initial experiment.
  mmio_write(PCIE_BASE + PCI_MEMORY_BASE_LIMIT, 0xc000c000U);

  // Disable the endpoint's memory decoding
  // before changing its BAR.
  // Preserve unrelated PCI command bits.
  uint8_t bus = info.secondary_bus;
  pcie_set_memory_decode(bus, false);

  // Assign the 64-bit BAR.
  // The low DWORD contains the base address
  // and the BAR type bits.
  // The high DWORD is zero because our PCIe
  // bus address fits in 32 bits.
  device_config_write(bus, 0, 0, PCI_BAR0, PCIE_BUS_MMIO_BASE | 0x04U);
  device_config_write(bus, 0, 0, PCI_BAR1, 0);

  // Read back the assignment before allowing
  // memory transactions to reach the device.
  uint32_t bar0 = device_config_read( bus, 0, 0, PCI_BAR0);
  uint32_t bar1 = device_config_read( bus, 0, 0, PCI_BAR1);

  if ((bar0 & ~0x0fU) != PCIE_BUS_MMIO_BASE || bar1 != 0)
    return PCIE_MAP_VERIFY_FAILED;

  // Enable downstream memory forwarding at the
  // root bridge, followed by endpoint decoding.
  // PCI bus mastering is deliberately NOT enabled.
  pcie_set_memory_decode(0, true);
  pcie_set_memory_decode(bus, true);
  __asm__ volatile("dsb sy\nisb" ::: "memory");
  xhci_resources_ready = true;
  return PCIE_MAP_OK;
}

bool pcie_dma_probe(pcie_dma_info_t *info) {
  if (!info)
    return false;

  *info = (pcie_dma_info_t){0};
  // These registers belong to the BCM2711 root
  // complex. They are not VL805 endpoint registers.
  // Do not inspect them until the PCIe controller
  // has been brought out of reset and is active.
  uint32_t status = mmio_read(PCIE_STATUS);

  if (!(status & PCIE_STATUS_RC))
    return false;

  info->misc_ctrl = mmio_read(PCIE_MISC_CTRL);
  info->bar2_lo = mmio_read(PCIE_RC_BAR2_LO);
  info->bar2_hi = mmio_read(PCIE_RC_BAR2_HI);
  info->bar3_lo = mmio_read(PCIE_RC_BAR3_LO);
  info->endian_ctrl = mmio_read(PCIE_RC_ENDIAN_CTRL);
  return true;
}

// Important: This function must only
// ever be called once, during startup.
bool pcie_dma_init(void) {
  fdt_pcie_dma_t dma;

  dma_window_ready = false;

  if (!pcie_xhci_ready())
    return false;

  if (!fdt_get_pcie_dma(&dma))
    return false;

  // This initial implementation deliberately supports
  // only the exact BCM2711 mapping reported by the
  // firmware on our development Raspberry Pi.
  // The code will have to be made more intelligent if
  // another revision is used.
  // PCIe address: 0x100000000
  // CPU address:  0x000000000
  // Valid size:   3 GiB
  //
  // The hardware aperture is rounded up to 4 GiB.
  if (dma.child_address_cells != 3 ||
      dma.parent_address_cells != 2 ||
      dma.size_cells != 2 ||
      dma.dma_cell_count != 7)
    return false;

  if (dma.dma_cells[0] != 0x02000000U ||
      dma.dma_cells[1] != 0x00000004U ||
      dma.dma_cells[2] != 0x00000000U ||
      dma.dma_cells[3] != 0x00000000U ||
      dma.dma_cells[4] != 0x00000000U ||
      dma.dma_cells[5] != 0x00000000U ||
      dma.dma_cells[6] != 0xC0000000U)
    return false;

  // We have no explicit memory-controller sizes.
  // For this single-range BCM2711 configuration,
  // use the same power-of-two fallback as Linux.
  if (dma.scb_cell_count != 0)
    return false;

  // The VL805 must not be bus mastering while
  // its inbound memory window is being configured.
  // Existing OrgOS PCIe initialisation leaves
  // bus mastering disabled.
  uint32_t bar2_lo = (uint32_t)PCIE_DMA_BUS_BASE | PCIE_DMA_SIZE_CODE;
  uint32_t bar2_hi = (uint32_t)(PCIE_DMA_BUS_BASE >> 32);

  // Program the inbound PCIe-to-RAM window.
  mmio_write(PCIE_RC_BAR2_LO, bar2_lo);
  mmio_write(PCIE_RC_BAR2_HI, bar2_hi);

  // Configure the system-memory controller aperture.
  uint32_t misc = mmio_read(PCIE_MISC_CTRL);
  misc &= ~PCIE_SCB0_SIZE_MASK;
  misc |= PCIE_DMA_SIZE_CODE << PCIE_SCB0_SIZE_SHIFT;
  mmio_write(PCIE_MISC_CTRL, misc);

  // Explicitly select little-endian DMA operation.
  uint32_t endian = mmio_read(PCIE_RC_ENDIAN_CTRL);
  endian &= ~PCIE_BAR2_ENDIAN_MASK;
  mmio_write(PCIE_RC_ENDIAN_CTRL, endian);

  // Ensure all register writes have completed
  // before verifying the programmed values.
  __asm__ volatile("dsb sy" ::: "memory");
  if (mmio_read(PCIE_RC_BAR2_LO) != bar2_lo ||
      mmio_read(PCIE_RC_BAR2_HI) != bar2_hi)
    return false;

  if ((mmio_read(PCIE_MISC_CTRL) &
          PCIE_SCB0_SIZE_MASK) !=
      (PCIE_DMA_SIZE_CODE << PCIE_SCB0_SIZE_SHIFT))
    return false;

  if (mmio_read(PCIE_RC_ENDIAN_CTRL) &
      PCIE_BAR2_ENDIAN_MASK)
    return false;

  dma_window_ready = true;
  return true;
}

bool pcie_phys_to_dma(uint64_t physical,
    uint64_t length, uint64_t *dma_address) {

  if (!dma_window_ready || !dma_address || length == 0)
    return false;

  // Firmware advertises CPU physical addresses
  // [0, 3 GiB) through the BCM2711 PCIe inbound
  // window, whose bus base is 0x100000000.
  // Check subtraction rather than computing the
  // end address, avoiding integer overflow.
  if (physical >= PCIE_DMA_VALID_SIZE)
    return false;
  if (length > PCIE_DMA_VALID_SIZE - physical)
    return false;

  *dma_address = PCIE_DMA_BUS_BASE + physical;
  return true;
}

bool pcie_xhci_get_command(uint16_t *command) {
  if (!command || !xhci_resources_ready)
    return false;

  pcie_info_t info;
  pcie_probe(&info);
  if (!info.link_up || !info.endpoint_present ||
      info.endpoint_vendor != 0x1106U ||
      info.endpoint_device != 0x3483U ||
      info.endpoint_class != 0x0c0330U)
    return false;

  uint32_t value = device_config_read( info.secondary_bus, 0, 0, PCI_COMMAND);
  *command = (uint16_t)(value & 0xffffU);
  return true;
}
