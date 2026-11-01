#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_MIDI_IN_STRESS_PACKETS 600U
#define USB_MIDI_IN_STRESS_MIN_TRANSFERS 520U

typedef enum {
  USB_MIDI_IN_STRESS_IDLE,
  USB_MIDI_IN_STRESS_RUNNING,
  USB_MIDI_IN_STRESS_PASS,
  USB_MIDI_IN_STRESS_INCONCLUSIVE,
  USB_MIDI_IN_STRESS_FAILED
} usb_midi_in_stress_state_t;

typedef enum {
  USB_MIDI_IN_STRESS_NO_ERROR,
  USB_MIDI_IN_STRESS_STREAM_ERROR,
  USB_MIDI_IN_STRESS_TRB_ERROR,
  USB_MIDI_IN_STRESS_LENGTH_ERROR,
  USB_MIDI_IN_STRESS_PACKET_ERROR
} usb_midi_in_stress_error_t;

typedef struct {
  usb_midi_in_stress_state_t state;
  usb_midi_in_stress_error_t error;
  uint32_t transfers;
  uint32_t packets;
  uint32_t wraps;
  uint32_t bytes;
  uint32_t fault_transfer;
  uint32_t fault_packet;
  uint64_t expected_trb;
  uint64_t actual_trb;
  uint64_t elapsed_us;
  uint64_t discarded_transfers;
} usb_midi_in_stress_info_t;

bool usb_midi_in_stress_start(void);
bool usb_midi_in_stress_get_info(usb_midi_in_stress_info_t *result);
void usb_midi_in_stress_service(void);
