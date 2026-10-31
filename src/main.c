#include <stdint.h>
#include <stdbool.h>

#include "board.h"
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
  // Establish the PCIe link to the VL805.
  console_puts("\nInitialising PCIe... ");

  uint64_t link_started = timer_now_us();
  pcie_init_result_t pcie_result = pcie_init_link();
  uint64_t link_elapsed = timer_now_us() - link_started;
  switch (pcie_result) {
    case PCIE_INIT_OK:
      console_printf("Link established (%llu ms).\n",
          (unsigned long long)(link_elapsed / 1000ULL));
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
  console_printf("  PCIe link elapsed: %llu us\n",
      (unsigned long long)link_elapsed);

  if (pcie_result != PCIE_INIT_OK && pcie_result != PCIE_INIT_ALREADY_UP)
    return false;

  // Configure the VL805's PCI resources.
  console_puts("Configuring VL805 PCI resources... ");
  pcie_map_result_t map_result = pcie_map_xhci();
  switch (map_result) {
    case PCIE_MAP_OK:
      console_puts("OK.\n");
      break;
    case PCIE_MAP_LINK_DOWN:
      console_puts("Failed: PCIe link down.\n");
      break;
    case PCIE_MAP_BUS_FAILED:
      console_puts("Failed: PCIe bus configuration.\n");
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
  if (map_result != PCIE_MAP_OK)
    return false;

  // Establish the inbound PCIe DMA aperture.
  console_puts("Configuring PCIe inbound DMA... ");
  if (!pcie_dma_init()) {
    console_puts("FAILED.\n");
    return false;
  }
  console_puts("OK.\n");

  // Determine whether the VL805 needs its firmware
  // reloaded after PCIe initialisation.
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
    acknowledged = mailbox_notify_xhci_reset( 0x00100000U, &diagnostic);

    // Verify that the firmware has left the controller
    // ready and halted.
    xhci_readiness_t readiness;
    if (!xhci_check_readiness(&readiness) ||
        !readiness.ready || !readiness.halted) {
      console_puts("FAILED (controller not ready).\n");
      return false;
    }

    uint32_t version;
    if (!pcie_xhci_get_firmware_version(&version) || version == 0U) {
      console_puts("FAILED (firmware version).\n");
      return false;
    }
  }

  // Verify that firmware loading has not altered
  // the VL805's PCI BAR assignment.
  if (!pcie_xhci_bar_valid()) {
    console_puts("FAILED (VL805 BAR mapping).\n");
    return false;
  }

  // Temporary diagnostic:
  // Keep PCI bus mastering enabled while attempting
  // HCRST, in case VL805 firmware requires DMA access.
  //
  // DO NOT proceed into normal USB configuration
  // using this experimental arrangement.
  timer_delay_ms(100);

  if (!pcie_xhci_enable_bus_master()) {
    console_puts("FAILED (enabling bus mastering).\n");
    return false;
  }

  uint16_t final_command = 0;
  if (!pcie_xhci_get_command(&final_command) ||
      !(final_command & (1U << 2))) {
    console_puts("FAILED (bus mastering not enabled).\n");
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
      console_printf("  Mailbox result : %u\n",
          (unsigned int)diagnostic.result);
      console_printf("  Response seen  : %s\n",
          diagnostic.response_received ? "Yes" : "No");
      if (diagnostic.response_received) {
        console_printf("  Response code  : %u\n", diagnostic.response_code);
        console_printf("  Tag length     : %u\n", diagnostic.tag_length);
      }
    }
  } else {
    console_puts("VL805 firmware already initialised.\n");
  }

  // Attempt a Host Controller Reset while PCI bus
  // mastering remains enabled.
  //
  // The controller must be halted and must not
  // have been configured with OrgOS DMA structures.
  console_puts("Resetting xHCI controller... ");
  xhci_reset_info_t reset = {0};
  xhci_reset_result_t reset_result = xhci_reset(&reset);
  if (reset_result != XHCI_RESET_OK) {
    console_printf( "FAILED (%u, cmd 0x%08x, sts 0x%08x).\n",
        (unsigned int)reset_result, reset.command_after, reset.status_after);
    console_printf("  Reset elapsed: %llu us\n",
        (unsigned long long)reset.reset_elapsed_us);
    return false;
  }
  console_puts("OK.\n");

  console_puts("Restoring PCI state: ");
  // HCRST completed with bus mastering enabled.
  // Disable it before installing OrgOS DMA structures.
  if (!pcie_xhci_disable_bus_master()) {
    console_puts("FAILED (bus mastering).\n");
    return false;
  }

  uint16_t post_reset_command;
  if (!pcie_xhci_get_command(&post_reset_command) ||
      (post_reset_command & (1U << 2))) {
    console_puts("FAILED (PCI command).\n");
    return false;
  }

  xhci_readiness_t post_reset;
  if (!xhci_check_readiness(&post_reset) ||
      !post_reset.ready || !post_reset.halted) {
    console_puts("FAILED (controller state).\n");
    return false;
  }
  console_puts("OK.\n");
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

  uint32_t board_revision = 0U;
  bool board_ok = mailbox_get_board_revision(&board_revision);
  if (board_ok && (board_revision & (1U << 23)) &&
      ((board_revision >> 4) & 0xffU) == 0x11U) {
    console_printf("Hardware: Raspberry Pi 4 Model B (rev 0x%08x, EL%u)\n",
        board_revision, current_el());
  } else if (board_ok) {
    console_printf("Hardware: board rev 0x%08x, EL%u\n",
        board_revision, current_el());
  } else {
    console_printf("Hardware: unknown board, EL%u\n", current_el());
  }

  if (!clock_ok)
    console_puts("UART: firmware clock query failed; using fallback.\n");

  // Inspect the firmware-supplied Device Tree
  // before enabling address translation.
  // This is diagnostic only. An absent or invalid
  // FDT must not prevent OrgOS from booting.
  console_puts("\nFirmware Device Tree... ");
  if (!fdt_init()) {
    console_puts("Unavailable or invalid.\n");
  } else if (!fdt_relocate()) {
    console_puts("FAILED (relocation).\n");
  } else {
    console_puts("OK (relocated).\n");
  }

  // Discover the firmware-reported ARM memory region
  // before constructing the EL1 translation tables.
  // At this point:
  //   - The MMU is still disabled.
  //   - Data caches are disabled.
  //   - UART is available for diagnostics.
  //   - CPU interrupts remain masked.
  uint32_t arm_base = 0U;
  uint32_t arm_size = 0U;
  bool memory_ok = mailbox_get_arm_memory(&arm_base, &arm_size);
  uint32_t installed_mib = board_ok ? board_ram_mib(board_revision) : 0U;
  console_puts("Memory: ");
  if (installed_mib)
    console_printf("%u MiB installed", installed_mib);
  else
    console_puts("installed RAM unknown");
  if (memory_ok) {
    console_printf(", %u MiB ARM low\n", arm_size / (1024U * 1024U));
  } else {
    console_puts(", ARM low memory query FAILED\n");
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
    console_puts("OK.\n");
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
          console_printf("OK (address %u).\n",
              (unsigned int)usb_info.address);
        } else {
          console_printf("FAILED (stage %d, detail 0x%u).\n",
              (uint32_t)usb_result, usb_info.detail);
        }
      }
    }
  }

  // USB failure is non-fatal. Retain the diagnostic
  // console even if the controller cannot start.
  console_puts("\nOrgOS interactive console.\n");
  console_run();
}

