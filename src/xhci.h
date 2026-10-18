#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "dma.h"

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

typedef struct {
  uint64_t parameter;
  uint32_t status;
  uint32_t control;
} xhci_trb_t;

typedef struct {
  uint64_t base;
  uint32_t size;
  uint32_t reserved;
} xhci_erst_entry_t;

// Current implementation supports the 31 scratchpad
// buffers reported by the Raspberry Pi 4B's VL805.
// Larger requirements are rejected rather than
// silently truncated.
#define XHCI_MAX_SCRATCHPADS 31U

typedef struct {
  dma_buffer_t command_ring;
  dma_buffer_t event_ring;
  dma_buffer_t erst;
  dma_buffer_t dcbaa;
  dma_buffer_t scratchpad_array;
  dma_buffer_t scratchpads[XHCI_MAX_SCRATCHPADS];
  unsigned int scratchpad_count;
} xhci_dma_t;

bool xhci_dma_prepare(xhci_dma_t *dma);
bool xhci_dma_release(xhci_dma_t *dma);

typedef enum {
  XHCI_CONFIGURE_OK,
  XHCI_CONFIGURE_ALREADY,
  XHCI_CONFIGURE_UNAVAILABLE,
  XHCI_CONFIGURE_UNSAFE_STATE,
  XHCI_CONFIGURE_ALLOCATION_FAILED,
  XHCI_CONFIGURE_DMA_FAILED,
  XHCI_CONFIGURE_VERIFY_FAILED
} xhci_configure_result_t;

typedef struct {
  uint64_t command_ring;
  uint64_t event_ring;
  uint64_t erst;
  uint64_t dcbaa;
  uint64_t scratchpad_array;
  unsigned int scratchpads;

  uint32_t command;
  uint32_t status;
  uint32_t config;
  uint64_t crcr;
  uint64_t dcbaap;

  uint32_t erstsz;
  uint64_t erstba;
  uint64_t erdp;
  uint32_t iman;
} xhci_configure_info_t;

xhci_configure_result_t xhci_configure(xhci_configure_info_t *result);

typedef struct {
  uint32_t hcsparams2;
  unsigned int scratchpad_buffers;
  bool ready;
  bool halted;
} xhci_readiness_t;

bool xhci_check_readiness(xhci_readiness_t *result);

typedef struct {
  uint32_t hcsparams1;
  uint32_t hcsparams2;
  uint32_t hcsparams3;
  uint32_t hccparams1;
  uint32_t hccparams2;
  uint32_t ext_cap_offset;
  uint32_t ext_cap_header;
} xhci_cap_diagnostic_t;

bool xhci_diagnose_capabilities(xhci_cap_diagnostic_t *result);

typedef enum {
  XHCI_RESET_OK,
  XHCI_RESET_UNAVAILABLE,
  XHCI_RESET_BUS_MASTER_ENABLED,
  XHCI_RESET_NOT_HALTED,
  XHCI_RESET_ALREADY_ACTIVE,
  XHCI_RESET_TIMEOUT,
  XHCI_RESET_READY_TIMEOUT
} xhci_reset_result_t;

typedef struct {
  uint32_t command_before;
  uint32_t status_before;
  uint32_t command_after;
  uint32_t status_after;
  uint64_t reset_elapsed_us;
  uint64_t ready_elapsed_us;
} xhci_reset_info_t;

xhci_reset_result_t xhci_reset(xhci_reset_info_t *result);

