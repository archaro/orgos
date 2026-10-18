#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint8_t capability_length;
  uint16_t version;
  uint8_t max_slots;
  uint16_t max_interrupters;
  uint8_t max_ports;
  uint32_t runtime_offset;
  uint32_t doorbell_offset;
} xhci_info_t;

bool xhci_probe(xhci_info_t *info);

typedef struct {
  uint32_t command;
  uint32_t status;
  uint32_t pagesize;
  uint32_t config;
  uint64_t command_ring;
  uint64_t dcbaa;
} xhci_state_t;

bool xhci_get_state(xhci_state_t *state);
