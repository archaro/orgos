#include <stdint.h>
#include <stdbool.h>

#include "mailbox.h"
#include "timer.h"
#include "fdt.h"
#include "physmem.h"
#include "gic.h"
#include "irq.h"
#include "mmu.h"
#include "page.h"
#include "pcie.h"
#include "xhci.h"
#include "usb.h"
#include "uart.h"
#include "console.h"

#define UART_BAUD 115200U

static unsigned int current_el(void) {
  uint64_t value;

  __asm__ volatile(
    "mrs %0, CurrentEL"
    : "=r"(value)
  );

  return (unsigned int)((value >> 2) & 3U);
}

bool pcie_subsystem_init(void) {
  // Initialise the PCIe subsystem.
  // First establish the link to the VL805, then
  // configure its memory resources.
  // PCIe failures are non-fatal for now: preserve
  // the diagnostic console.
  console_puts("\nInitialising PCIe... ");
  pcie_init_result_t pcie_result = pcie_init_link();
  switch (pcie_result) {
    case PCIE_INIT_OK:
      console_puts("Link established.\n");
      break;
    case PCIE_INIT_ALREADY_UP:
      console_puts("Link already active.\n");
      break;
    case PCIE_INIT_NOT_ROOT_COMPLEX:
      console_puts("Failed: not in root-complex mode.\n");
      break;
    case PCIE_INIT_LINK_TIMEOUT:
      console_puts("Failed: link timeout.\n");
      break;
  }
  
  // Only configure PCI resources when the link
  // has been successfully established.
  pcie_map_result_t map_result = PCIE_MAP_LINK_DOWN;
  if (pcie_result == PCIE_INIT_OK ||
      pcie_result == PCIE_INIT_ALREADY_UP) {
    console_puts("Configuring VL805 PCI resources... ");
    map_result = pcie_map_xhci();
    switch (map_result) {
      case PCIE_MAP_OK:
        console_puts("OK.\n");
        break;
      case PCIE_MAP_LINK_DOWN:
        console_puts("Failed: PCIe link down.\n");
        break;
      case PCIE_MAP_WRONG_DEVICE:
        console_puts("Failed: unexpected PCIe device.\n");
        break;
      case PCIE_MAP_BAD_BAR:
        console_puts("Failed: unexpected BAR type.\n");
        break;
      case PCIE_MAP_VERIFY_FAILED:
        console_puts("Failed: BAR verification error.\n");
        break;
    }
  }

  if (map_result != PCIE_MAP_OK)
  return false;

  // Configure the inbound PCIe DMA aperture.
  // This must happen after the PCIe link and VL805
  // resources are established, but before enabling
  // bus mastering or starting the xHCI controller.
  console_puts("Configuring PCIe inbound DMA... ");
  if (!pcie_dma_init()) {
    console_puts("FAILED.\n");
    return false;
  }
  console_puts("OK.\n");

  // A fundamental PCIe reset may leave the VL805
  // without its internal firmware. On Raspberry Pi 4,
  // VideoCore provides a mailbox property to restore it.
  // Firmware initialisation must take place after PCIe
  // link establishment and resource configuration, but
  // before the xHCI controller is started.
  // Do not request another load if the controller is
  // already ready, for example when the bootloader has
  // left the PCIe link operational.
  xhci_state_t state;

  if (!xhci_get_state(&state)) {
    console_puts("VL805 state unavailable.\n");
    return false;
  }

  bool needs_firmware = (state.status & (1U << 11)) != 0;
  mailbox_diagnostic_t diagnostic = {0};
  bool acknowledged = true;

  if (needs_firmware) {
    console_puts("Initialising VL805 firmware... ");
    uint16_t command;
    if (!pcie_xhci_get_command(&command) || (command & (1U << 2))) {
      console_puts("FAILED (invalid PCI state).\n");
      return false;
    }

    // Raspberry Pi 4B: bus 1, device 0, function 0.
    acknowledged = mailbox_notify_xhci_reset(0x00100000U, &diagnostic);
    // Firmware initialisation may restore PCI command
    // bits asynchronously. First establish that the
    // controller is ready, then revoke bus mastering.
    xhci_readiness_t readiness;
    if (!xhci_check_readiness(&readiness) ||
        !readiness.ready || !readiness.halted) {
      console_puts("FAILED (controller not ready).\n");
      return false;
    }
    uint32_t version;
    if (!pcie_xhci_get_firmware_version(&version) || version == 0) {
      console_puts("FAILED (firmware version).\n");
      return false;
    }
  }

  // Firmware may have modified PCI configuration.
  // The VL805 must not be permitted to perform DMA
  // until OrgOS has installed its xHCI DMA structures.
  if (!pcie_xhci_bar_valid()) {
    console_puts("FAILED (VL805 BAR mapping).\n");
    return false;
  }

  // Diagnostic settling interval. Keep until we
  // establish whether firmware completion reliably
  // precedes the ready and version observations.
  timer_delay_ms(100);
  if (!pcie_xhci_disable_bus_master()) {
    console_puts("FAILED (bus mastering).\n");
    return false;
  }

  // Verify that the firmware has not subsequently
  // restored bus mastering.
  timer_delay_ms(100);
  uint16_t final_command;
  if (!pcie_xhci_get_command(&final_command) || (final_command & (1U << 2))) {
    console_puts("FAILED (bus mastering restored).\n");
    return false;
  }
  if (!pcie_xhci_bar_valid()) {
    console_puts("FAILED (VL805 BAR mapping changed).\n");
    return false;
  }
  if (needs_firmware) {
    if (acknowledged) {
      console_puts("OK.\n");
    } else {
      console_puts("OK (mailbox anomaly).\n");
      console_puts("  Mailbox result : ");
      console_putu32((uint32_t)diagnostic.result);
      console_putc('\n');
      console_puts("  Response seen  : ");
      console_puts(diagnostic.response_received ?
          "Yes\n" : "No\n");
      if (diagnostic.response_received) {
        console_puts("  Response code  : ");
        console_putu32(diagnostic.response_code);
        console_putc('\n');
        console_puts("  Tag length     : ");
        console_putu32(diagnostic.tag_length);
        console_putc('\n');
      }
    }
  } else {
    console_puts("VL805 firmware already initialised.\n");
  }
  return true;
}

void main(void) {
  uint32_t clock_hz;
  bool clock_ok;

  // Record the time that the OS actually starts
  timer_init();

  // Ask the firmware for the UART clock
  clock_ok = mailbox_get_clock_rate(
      MAILBOX_CLOCK_UART, &clock_hz);

  // A sensible fallback, just in case
  if (!clock_ok)
    clock_hz = 48000000U;

  uart_init(clock_hz, UART_BAUD);

  console_puts("\nWelcome to OrgOS\n");
  console_puts("----------------\n");
  // Identify the Raspberry Pi hardware.
  // The board revision includes the model, processor
  // and installed RAM capacity in its modern encoding.
  // Query the firmware before enabling the MMU,
  // while mailbox access is still straightforward.
  uint32_t board_revision;
  console_puts("Board revision: ");
  if (mailbox_get_board_revision(&board_revision)) {
    static const char hex[] = "0123456789ABCDEF";
    console_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
      console_putc(hex[(board_revision >> shift) & 0x0fU]);
    console_putc('\n');
  } else {
    console_puts("Unavailable\n");
  }

  // What EL are we at?
  console_puts("Current exception level: EL");
  console_putu32(current_el());
  console_puts("\n");

  // Configure the UART clock
  if (clock_ok) {
    console_puts("Firmware mailbox: OK\n");
  } else {
    console_puts("Firmware mailbox: FAILED (using fallback)\n");
  }

  console_puts("UART clock: ");
  console_putu32(clock_hz);
  console_puts(" Hz\n");

  console_puts("Baud rate: ");
  console_putu32(UART_BAUD);
  console_puts("\n");

  // Inspect the firmware-supplied Device Tree
  // before enabling address translation.
  // This is diagnostic only. An absent or invalid
  // FDT must not prevent OrgOS from booting.
  console_puts("\nChecking firmware Device Tree... ");
  if (fdt_init()) {
    console_puts("Valid FDT detected.\n");
    console_puts("Relocating Device Tree... ");
    if (fdt_relocate()) {
      console_puts("OK.\n");
    } else {
      console_puts("FAILED.\n");
    }
  } else {
    console_puts("Not available or invalid.\n");
  }

  // Discover the firmware-reported ARM memory region
  // before constructing the EL1 translation tables.
  // At this point:
  //   - The MMU is still disabled.
  //   - Data caches are disabled.
  //   - UART is available for diagnostics.
  //   - CPU interrupts remain masked.
  uint32_t arm_base = 0;
  uint32_t arm_size = 0;

  console_puts("\nDiscovering ARM memory... ");
  bool memory_ok = mailbox_get_arm_memory(&arm_base, &arm_size);
  if (memory_ok) {
    console_puts("OK (");
    console_putu32(arm_size / (1024U * 1024U));
    console_puts(" MiB).\n");
  } else {
    console_puts("FAILED.\n");
  }

  // Build our physical-memory inventory before
  // constructing the translation tables.
  // The FDT has already been relocated into
  // kernel-owned memory, and translation is
  // still disabled.
  console_puts("Building physical memory map... ");
  if (physmem_init()) {
    console_puts("OK.\n");
  } else {
    console_puts("FAILED.\n");
    console_puts("System halted.\n");
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }

  // The MMU requires valid memory information.
  // A failed initialization is fatal, because
  // later subsystems will depend on translation.
  console_puts("Initialising MMU... ");
  if (memory_ok && mmu_init(physmem_get_map())) {
    console_puts("Translation enabled.\n");
  } else {
    console_puts("FAILED.\n");
    console_puts("System halted.\n");
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }

  // Now we have the physical memory map and the MMU is configured,
  // it is time to initialise the physical page allocator.
  console_puts("Initialising page allocator... ");
  if (page_init(physmem_get_map())) {
    console_puts("OK.\n");
  } else {
    console_puts("FAILED.\n");
    console_puts("System halted.\n");
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;) {
      __asm__ volatile("wfe");
    }
  }

  // Initialise interrupt delivery.
  // VBAR_EL1 was installed by boot.S.
  // The CPU's IRQ mask is still set.
  gic_init();

  // Enable UART0 at the interrupt controller.
  gic_enable_irq(GIC_IRQ_UART0);

  // Enable receive interrupts at the UART itself.
  uart_enable_rx_irq();

  // Ensure device configuration is complete before
  // permitting the processor to accept IRQs.
  __asm__ volatile("dsb sy\nisb" ::: "memory");

  // Unmask IRQs at the processor.
  // DAIFClr #2 clears the I bit (IRQ mask).
  // Other exception masks remain unchanged.
  __asm__ volatile("msr daifclr, #2\nisb" ::: "memory");

  if (!pcie_subsystem_init()) {
    console_puts("\nUNABLE TO INITIALISE PCIE BUS!\n");
  } else {
    // PCIe and VL805 firmware are ready.
    // Install the persistent xHCI DMA structures.
    console_puts("Configuring USB subsystem... ");

    xhci_configure_info_t configure_info;
    xhci_configure_result_t configure_result =
        xhci_configure(&configure_info);

    if (configure_result != XHCI_CONFIGURE_OK) {
      console_puts("FAILED (configuration result ");
      console_putu32((uint32_t)configure_result);
      console_puts(").\n");
    } else {
      console_puts("OK.\n");

      // Enable PCI bus mastering and start xHCI.
      // This operation verifies that the controller
      // transitions from halted to running.
      console_puts("Starting USB subsystem... ");

      xhci_start_info_t start_info;
      xhci_start_result_t start_result = xhci_start(&start_info);

      if (start_result != XHCI_START_OK) {
        console_puts("FAILED (start result ");
        console_putu32((uint32_t)start_result);
        console_puts(").\n");
      } else {
        console_puts("OK.\n");

        console_puts("Enumerating internal USB hub... ");

        usb_init_info_t usb_info;
        usb_init_result_t usb_result = usb_init(&usb_info);

        if (usb_result == USB_INIT_OK) {
          console_puts("OK (Slot ");
          console_putu32(usb_info.slot_id);
          console_puts(", Address ");
          console_putu32(usb_info.address);
          console_puts(").\n");
        } else {
          console_puts("FAILED (stage ");
          console_putu32((uint32_t)usb_result);
          console_puts(", detail 0x");
          console_puthex(usb_info.detail, 8);
          console_puts(").\n");
        }
      }
    }
  }

  // USB failure is non-fatal. Retain the diagnostic
  // console even if the controller cannot start.
  console_puts("\nOrgOS interactive console.\n");
  console_run();
}

