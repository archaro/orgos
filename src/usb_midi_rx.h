#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usb_midi.h"

#define USB_MIDI_RX_QUEUE_CAPACITY 64U

typedef enum {
  USB_MIDI_RX_IDLE,
  USB_MIDI_RX_RUNNING,
  USB_MIDI_RX_DISCONNECTED,
  USB_MIDI_RX_FAILED
} usb_midi_rx_state_t;

typedef struct {
  usb_midi_rx_state_t state;
  uint32_t generation;
  uint64_t transfers;
  uint64_t events;
  uint64_t rejected;
  unsigned int staged;
  unsigned int queue_depth;
  unsigned int queue_peak;
  uint8_t slot_id;
  uint8_t cables;
} usb_midi_rx_info_t;

// Start a persistent receiver for one active
// USB-MIDI device incarnation.
//
// An existing Bulk IN owner is never displaced.
bool usb_midi_rx_start_generation(uint32_t generation);

// Determine whether this generation belongs to
// the persistent receiver rather than a diagnostic.
bool usb_midi_rx_owns_generation(uint32_t generation);

// Inspect receiver state without consuming events.
bool usb_midi_rx_get_info(uint32_t generation, usb_midi_rx_info_t *info);

// Remove one decoded event from the selected
// active device's receive queue.
bool usb_midi_rx_read(uint32_t generation, usb_midi_event_t *event);

// Return true only when the event has been accepted.
// Returning false retains the event at the FIFO head.
//
// Callbacks execute in the USB foreground service.
// They must not block, recurse into usb_service(),
// or change the receiver's subscription.
typedef bool (*usb_midi_rx_consumer_t)(uint32_t generation,
    const usb_midi_event_t *event, void *context);

// Attach exactly one consumer to an active receiver.
// The consumer exclusively owns its event FIFO.
bool usb_midi_rx_subscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context);

// Release only the specified consumer's subscription.
bool usb_midi_rx_unsubscribe(uint32_t generation,
    usb_midi_rx_consumer_t consumer, void *context);

// Service all persistent receivers.
// Called from the USB foreground service loop.
void usb_midi_rx_service(void);

