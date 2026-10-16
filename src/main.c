#include <stdint.h>
#include <stdbool.h>

#include "mailbox.h"
#include "timer.h"
#include "gic.h"
#include "irq.h"
#include "pcie.h"
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

  if (map_result == PCIE_MAP_OK)
    return true;
  else
    return false;
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

  console_puts("\nOrgOS - bare metal Raspberry Pi 4\n");
  console_puts("--------------------------------\n");

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

  if (!pcie_subsystem_init())
    console_puts("\nUNABLE TO INITIALISE PCIE BUS!\n");

  console_puts("\nOrgOS interactive console.\n");
  console_run();
}

