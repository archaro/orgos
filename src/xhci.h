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
  uint8_t number;
  uint32_t portsc;
  uint8_t speed;
  uint8_t link_state;
  uint8_t indicator;
  bool connected;
  bool enabled;
  bool overcurrent;
  bool resetting;
  bool powered;
  bool connect_change;
  bool enable_change;
  bool warm_reset_change;
  bool overcurrent_change;
  bool reset_change;
  bool link_change;
  bool config_error_change;
} xhci_port_t;

bool xhci_port_count(uint8_t *count);
bool xhci_port_read(uint8_t number, xhci_port_t *port);

typedef enum {
  XHCI_PORT_CLEAR_OK,
  XHCI_PORT_CLEAR_NOT_RUNNING,
  XHCI_PORT_CLEAR_UNAVAILABLE,
  XHCI_PORT_CLEAR_RESET_ACTIVE,
  XHCI_PORT_CLEAR_FAILED
} xhci_port_clear_result_t;

typedef struct {
  uint32_t before;
  uint32_t after;
  uint32_t acknowledged;
} xhci_port_clear_info_t;

xhci_port_clear_result_t xhci_port_clear(
    uint8_t number, xhci_port_clear_info_t *result);

typedef enum {
  XHCI_PORT_RESET_OK,
  XHCI_PORT_RESET_NOT_RUNNING,
  XHCI_PORT_RESET_UNAVAILABLE,
  XHCI_PORT_RESET_DISCONNECTED,
  XHCI_PORT_RESET_NOT_POWERED,
  XHCI_PORT_RESET_ALREADY_ENABLED,
  XHCI_PORT_RESET_TIMEOUT,
  XHCI_PORT_RESET_FAILED
} xhci_port_reset_result_t;

typedef struct {
  uint32_t before;
  uint32_t after;
  uint32_t controller_status;
  uint64_t elapsed_us;
} xhci_port_reset_info_t;

xhci_port_reset_result_t xhci_port_reset(
    uint8_t number, xhci_port_reset_info_t *result);

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

typedef enum {
  XHCI_EVENT_NONE,
  XHCI_EVENT_READY,
  XHCI_EVENT_ERROR
} xhci_event_result_t;

typedef struct {
  uint64_t parameter;
  uint32_t status;
  uint32_t control;
  uint8_t type;
  uint8_t completion_code;
  uint8_t slot_id;
  uint8_t port_id;
} xhci_event_t;

typedef struct {
  uint32_t port_changes;
  uint8_t last_port;
  uint32_t last_status;
  uint32_t last_control;
} xhci_event_stats_t;

xhci_event_result_t xhci_event_poll(xhci_event_t *event);
bool xhci_event_record(const xhci_event_t *event);
bool xhci_event_get_stats(xhci_event_stats_t *stats);

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

typedef enum {
  XHCI_START_OK,
  XHCI_START_NOT_CONFIGURED,
  XHCI_START_ALREADY_RUNNING,
  XHCI_START_UNAVAILABLE,
  XHCI_START_UNSAFE_STATE,
  XHCI_START_PCI_FAILED,
  XHCI_START_TIMEOUT,
  XHCI_START_CONTROLLER_ERROR
} xhci_start_result_t;

typedef struct {
  uint16_t pci_command_before;
  uint16_t pci_command_after;
  uint32_t command_before;
  uint32_t command_after;
  uint32_t status_before;
  uint32_t status_after;
  uint64_t elapsed_us;
} xhci_start_info_t;

xhci_start_result_t xhci_start(xhci_start_info_t *result);

typedef struct {
  uint32_t hcsparams2;
  unsigned int scratchpad_buffers;
  bool ready;
  bool halted;
} xhci_readiness_t;

typedef enum {
  XHCI_COMMAND_OK,
  XHCI_COMMAND_NOT_RUNNING,
  XHCI_COMMAND_BUSY,
  XHCI_COMMAND_UNAVAILABLE,
  XHCI_COMMAND_CACHE_ERROR,
  XHCI_COMMAND_TIMEOUT,
  XHCI_COMMAND_BAD_EVENT,
  XHCI_COMMAND_FAILED
} xhci_command_result_t;

typedef struct {
  uint64_t command_address;
  uint64_t event_parameter;
  uint32_t event_status;
  uint32_t event_control;
  uint32_t controller_status;
  uint32_t completion_code;
  uint32_t event_type;
  uint32_t command_number;
  uint64_t elapsed_us;
  uint8_t slot_id;
} xhci_command_completion_t;

xhci_command_result_t xhci_command_submit(
    const xhci_trb_t *command, xhci_command_completion_t *completion);

typedef enum {
  XHCI_SLOT_OK,
  XHCI_SLOT_ALREADY_REQUESTED,
  XHCI_SLOT_NOT_READY,
  XHCI_SLOT_COMMAND_FAILED,
  XHCI_SLOT_INVALID_ID
} xhci_slot_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t root_port;
  uint32_t completion_code;
  uint32_t controller_status;
  xhci_command_result_t command_result;
} xhci_slot_info_t;

xhci_slot_result_t xhci_enable_slot(xhci_slot_info_t *result);

typedef enum {
  XHCI_DEVICE_OK,
  XHCI_DEVICE_NOT_READY,
  XHCI_DEVICE_ALREADY_PREPARED,
  XHCI_DEVICE_ALLOCATION_FAILED,
  XHCI_DEVICE_DMA_FAILED,
  XHCI_DEVICE_VERIFY_FAILED
} xhci_device_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t root_port;
  uint8_t speed;
  uint8_t context_size;
  uint16_t max_packet_size;
  uint64_t input_context;
  uint64_t output_context;
  uint64_t ep0_ring;
  uint64_t dcbaa_entry;
  uint32_t input_add_flags;
  uint32_t slot_dword0;
  uint32_t slot_dword1;
  uint32_t ep0_dword1;
  uint32_t ep0_dword2;
  uint32_t ep0_dword3;
} xhci_device_info_t;

xhci_device_result_t xhci_device_prepare(xhci_device_info_t *result);
bool xhci_device_get_info(xhci_device_info_t *result);

typedef enum {
  XHCI_NOOP_OK,
  XHCI_NOOP_NOT_RUNNING,
  XHCI_NOOP_ALREADY_SUBMITTED,
  XHCI_NOOP_UNAVAILABLE,
  XHCI_NOOP_CACHE_FAILED,
  XHCI_NOOP_TIMEOUT,
  XHCI_NOOP_BAD_EVENT,
  XHCI_NOOP_COMMAND_FAILED
} xhci_noop_result_t;

typedef struct {
  uint64_t command_address;
  uint64_t event_parameter;
  uint32_t event_status;
  uint32_t event_control;
  uint32_t controller_status;
  uint32_t completion_code;
  uint32_t event_type;
  uint64_t elapsed_us;
  uint32_t command_number;
} xhci_noop_info_t;

xhci_noop_result_t xhci_noop( xhci_noop_info_t *result);

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

typedef struct {
  uint32_t offset;
  uint32_t header;
  uint32_t name;
  uint32_t ports;
  uint32_t slot;
  uint8_t major;
  uint8_t minor;
  uint8_t first_port;
  uint8_t port_count;
  uint8_t speed_id_count;
  uint8_t slot_type;
} xhci_protocol_t;

bool xhci_protocol_get(unsigned int index, xhci_protocol_t *protocol,
    bool *found);

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

