#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bulk/control data transfer contract. This interface exposes copied transfer
// records and asynchronous ownership states, not controller DMA allocations.
// Endpoint setup, quiescence and slot lifecycle remain in xhci.h.

#define XHCI_CONTROL_MAX_DATA 256U
#define XHCI_BULK_IN_MAX_DATA 64U
#define XHCI_BULK_IN_QUEUE_CAPACITY 16U

typedef enum {
  XHCI_BULK_OUT_OK,
  XHCI_BULK_OUT_NOT_READY,
  XHCI_BULK_OUT_INVALID_LENGTH,
  XHCI_BULK_OUT_ALLOCATION_FAILED,
  XHCI_BULK_OUT_DMA_FAILED,
  XHCI_BULK_OUT_RING_FAILED,
  XHCI_BULK_OUT_EVENT_FAILED,
  XHCI_BULK_OUT_TIMEOUT,
  XHCI_BULK_OUT_TRANSFER_FAILED,
  XHCI_BULK_OUT_CANCELLED
} xhci_bulk_out_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t endpoint_id;
  uint8_t completion_code;
  uint32_t event_status;
  uint32_t controller_status;
  uint64_t event_parameter;
  uint64_t transfer_trb;
  uint64_t elapsed_us;
  uint32_t length;
} xhci_bulk_out_info_t;

xhci_bulk_out_result_t xhci_downstream_bulk_out(uint8_t slot_id,
    const uint8_t *data, uint32_t length, xhci_bulk_out_info_t *result);

typedef enum {
  XHCI_BULK_OUT_ASYNC_IDLE,
  XHCI_BULK_OUT_ASYNC_PENDING,
  XHCI_BULK_OUT_ASYNC_COMPLETE,
  XHCI_BULK_OUT_ASYNC_FAILED,
  XHCI_BULK_OUT_ASYNC_UNCERTAIN
} xhci_bulk_out_async_state_t;

typedef struct {
  xhci_bulk_out_async_state_t state;
  xhci_bulk_out_result_t result;
  xhci_bulk_out_info_t transfer;
} xhci_bulk_out_async_info_t;

// OK means submitted, not completed.
// Only one outstanding Bulk OUT transfer is permitted per device.
xhci_bulk_out_result_t xhci_bulk_out_async_start(
    uint8_t slot_id, const uint8_t *data, uint32_t length);

// Inspect without acknowledging completion.
bool xhci_bulk_out_async_get(
    uint8_t slot_id, xhci_bulk_out_async_info_t *info);

// Acknowledge a definitive terminal outcome.
// Uncertain transfers may not be reaped.
bool xhci_bulk_out_async_reap(
    uint8_t slot_id, xhci_bulk_out_async_info_t *info);

// Advance asynchronous transfers whose hardware
// completion events have been dispatched.
void xhci_bulk_out_async_service(void);

typedef enum {
  XHCI_BULK_IN_IDLE,
  XHCI_BULK_IN_PENDING,
  XHCI_BULK_IN_COMPLETE,
  XHCI_BULK_IN_FAILED
} xhci_bulk_in_state_t;

typedef enum {
  XHCI_BULK_IN_ARM_OK,
  XHCI_BULK_IN_ARM_NOT_READY,
  XHCI_BULK_IN_ARM_ALREADY_STARTED,
  XHCI_BULK_IN_ARM_ALLOCATION_FAILED,
  XHCI_BULK_IN_ARM_DMA_FAILED,
  XHCI_BULK_IN_ARM_RING_FAILED,
  XHCI_BULK_IN_ARM_WAITER_FAILED
} xhci_bulk_in_arm_result_t;

typedef struct {
  uint64_t transfer_trb;
  uint64_t event_parameter;
  uint32_t event_status;
  uint32_t requested;
  uint32_t received;
  uint32_t residual;
  uint8_t slot_id;
  uint8_t endpoint_id;
  uint8_t completion_code;
  uint8_t data[XHCI_BULK_IN_MAX_DATA];
} xhci_bulk_in_info_t;

typedef struct {
  bool started;
  bool failed;
  bool paused;
  uint32_t queue_depth;
  uint32_t queue_peak;
  uint32_t pauses;
  uint32_t ring_wraps;
  uint64_t transfers;
  uint64_t bytes;
  uint8_t last_completion_code;
  xhci_bulk_in_arm_result_t last_arm_result;
} xhci_bulk_in_stream_stats_t;

xhci_bulk_in_arm_result_t xhci_bulk_in_stream_start( uint8_t slot_id);
void xhci_bulk_in_stream_service(void);
bool xhci_bulk_in_stream_get_stats( uint8_t slot_id,
    xhci_bulk_in_stream_stats_t *result);
bool xhci_bulk_in_stream_read( uint8_t slot_id, xhci_bulk_in_info_t *result);

typedef enum {
  XHCI_BULK_IN_RETIRE_OK,
  XHCI_BULK_IN_RETIRE_NOT_READY,
  XHCI_BULK_IN_RETIRE_BUSY,
  XHCI_BULK_IN_RETIRE_NOT_STARTED
} xhci_bulk_in_retire_result_t;

xhci_bulk_in_retire_result_t xhci_bulk_in_stream_retire(uint8_t slot_id);

typedef enum {
  XHCI_IN_RECONCILE_OK,
  XHCI_IN_RECONCILE_NOT_READY,
  XHCI_IN_RECONCILE_NOT_STARTED,
  XHCI_IN_RECONCILE_PENDING,
  XHCI_IN_RECONCILE_RESOLVED,
  XHCI_IN_RECONCILE_FAILED
} xhci_in_reconcile_result_t;

xhci_in_reconcile_result_t xhci_bulk_in_quiesced_reconcile(uint8_t slot_id);

typedef enum {
  XHCI_CONTROL_OK,
  XHCI_CONTROL_NOT_READY,
  XHCI_CONTROL_ALLOCATION_FAILED,
  XHCI_CONTROL_DMA_FAILED,
  XHCI_CONTROL_RING_FAILED,
  XHCI_CONTROL_EVENT_FAILED,
  XHCI_CONTROL_TIMEOUT,
  XHCI_CONTROL_TRANSFER_FAILED
} xhci_control_result_t;

typedef struct {
  uint8_t request_type;
  uint8_t request;
  uint16_t value;
  uint16_t index;
  uint16_t length;
} xhci_setup_packet_t;

typedef struct {
  uint8_t slot_id;
  uint8_t completion_code;
  uint8_t event_type;
  uint8_t endpoint_id;
  uint8_t data[XHCI_CONTROL_MAX_DATA];
  uint16_t length;
  uint32_t event_status;
  uint32_t controller_status;
  uint64_t event_parameter;
  uint64_t status_trb;
  uint64_t elapsed_us;
} xhci_control_info_t;

xhci_control_result_t xhci_control_in(
    const xhci_setup_packet_t *setup, xhci_control_info_t *result);
xhci_control_result_t xhci_control_no_data(
    const xhci_setup_packet_t *setup, xhci_control_info_t *result);
xhci_control_result_t xhci_downstream_control_in(uint8_t slot_id,
    const xhci_setup_packet_t *setup, xhci_control_info_t *result);
xhci_control_result_t xhci_downstream_control_no_data(uint8_t slot_id,
    const xhci_setup_packet_t *setup, xhci_control_info_t *result);
