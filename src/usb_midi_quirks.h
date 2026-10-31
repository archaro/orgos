#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_MIDI_QUIRK_STARTUP_EOX (1U << 0)

uint32_t usb_midi_quirks_lookup(uint16_t vendor_id,uint16_t product_id);
bool usb_midi_quirks_apply(uint32_t quirks, uint32_t *detail);
