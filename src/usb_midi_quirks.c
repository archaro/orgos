#include <stdbool.h>
#include <stdint.h>

#include "timer.h"
#include "usb_midi_quirks.h"
#include "xhci.h"

typedef struct {
  uint16_t vendor_id;
  uint16_t product_id;
  uint32_t quirks;
} usb_midi_quirk_entry_t;

static const usb_midi_quirk_entry_t quirk_table[] = {
  {0x7104U, 0x1400U, USB_MIDI_QUIRK_STARTUP_EOX}
};

uint32_t usb_midi_quirks_lookup(uint16_t vendor_id, uint16_t product_id) {

  for (unsigned int i = 0U;
      i < sizeof(quirk_table) / sizeof(quirk_table[0]); i++) {
    if (quirk_table[i].vendor_id == vendor_id &&
        quirk_table[i].product_id == product_id)
      return quirk_table[i].quirks;
  }
  return 0U;
}

bool usb_midi_quirks_apply(uint32_t quirks, uint32_t *detail) {

  if (detail)
    *detail = 0U;

  if (quirks & USB_MIDI_QUIRK_STARTUP_EOX) {
    // The U2MIDI Pro may leave a receiver in an incomplete
    // Note Off state when powered up with DIN connected.
    // EOX reliably restores correct first-message framing.
    static const uint8_t eox[4] = {0x05U, 0xf7U, 0x00U, 0x00U};
    xhci_bulk_out_info_t info = {0};
    xhci_bulk_out_result_t result =
        xhci_downstream_bulk_out(eox, sizeof(eox), &info);

    if (result != XHCI_BULK_OUT_OK || info.completion_code != 1U) {
      if (detail)
        *detail = (uint32_t)result | ((uint32_t)info.completion_code << 8);
      return false;
    }
    timer_delay_ms(20U);
  }
  return true;
}
