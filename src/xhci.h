#pragma once

#include <stdint.h>
#include <stdbool.h>

// Controller, root-port and device/endpoint lifecycle management.
// Data transfer clients include xhci_transfer.h separately.

// xHCI Transfer and Command Completion Codes.
// These values are defined by the xHCI specification
// and may be returned by the public driver API.
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_COMPLETION_SHORT_PACKET 13U
#define XHCI_COMPLETION_STOPPED                26U
#define XHCI_COMPLETION_STOPPED_LENGTH_INVALID 27U
#define XHCI_COMPLETION_STOPPED_SHORT_PACKET   28U

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
  uint32_t port_changes;
  uint8_t last_port;
  uint32_t last_status;
  uint32_t last_control;
  uint32_t command_events;
  uint32_t transfer_events;
  uint32_t read_errors;
  uint32_t unhandled_events;
  uint32_t unsupported_events;
  uint32_t unmatched_events;
  uint32_t duplicate_events;
  uint64_t last_unhandled_parameter;
  uint32_t last_unhandled_status;
  uint32_t last_unhandled_control;
  uint32_t port_queue_depth;
  uint32_t port_queue_peak;
  uint32_t port_queue_overflows;
  uint64_t service_calls;
  uint64_t service_events;
  uint64_t sync_in_services;
  uint32_t service_failures;
} xhci_event_stats_t;

bool xhci_event_get_stats(xhci_event_stats_t *stats);

typedef struct {
  uint32_t foreground_events;
  uint32_t synchronous_events;
  uint32_t port_wait_events;
  uint32_t correlated_events;
  uint32_t uncorrelated_events;
  uint32_t foreground_samples;
  uint32_t synchronous_samples;
  uint32_t port_wait_samples;
  uint64_t latency_total_us;
  uint64_t latency_max_us;
  uint64_t foreground_latency_us;
  uint64_t synchronous_latency_us;
  uint64_t port_wait_latency_us;
} xhci_event_latency_stats_t;

bool xhci_event_latency_get_stats(xhci_event_latency_stats_t *stats);

typedef struct {
  uint32_t command;
  uint32_t status;
  uint32_t iman;
  uint64_t erdp;
} xhci_msi_state_t;

bool xhci_msi_get_state(xhci_msi_state_t *state);

// Read-only snapshot of software-owned xHCI
// resources. Counts reflect current driver
// bookkeeping, not a hardware register census.
typedef struct {
  unsigned int downstream_capacity;
  unsigned int downstream_free;
  unsigned int downstream_owned;
  unsigned int assigned_slots;
  unsigned int pending_slots;
  uint32_t controller_dma_pages;
  uint32_t hub_dma_pages;
  uint32_t downstream_dma_pages;
  unsigned int waiter_capacity;
  unsigned int active_waiters;
  unsigned int completed_waiters;
  unsigned int command_waiters;
  unsigned int hub_waiters;
  unsigned int downstream_waiters;
  unsigned int orphan_waiters;
  bool controller_running;
  bool command_outstanding;
  bool command_ring_failed;
  bool dispatch_failed;
  unsigned int waiter_high_water;
  unsigned int command_waiter_reserve;
  uint32_t waiter_capacity_rejections;
  uint32_t waiter_duplicate_rejections;
  uint32_t waiter_invalid_rejections;
} xhci_resource_info_t;

bool xhci_resource_get_info(xhci_resource_info_t *info);

bool xhci_ep0_match_selftest(void);
bool xhci_ep0_late_selftest(void);
bool xhci_ep0_recovery_selftest(void);
bool xhci_bulk_out_stop_selftest(void);
bool xhci_cancellation_fault_selftest(void);
bool xhci_route_fault_selftest(void);
bool xhci_disable_fault_selftest(void);

typedef enum {
  XHCI_SERVICE_IDLE,
  XHCI_SERVICE_PROGRESS,
  XHCI_SERVICE_NOT_READY,
  XHCI_SERVICE_FAILED
} xhci_service_result_t;
xhci_service_result_t xhci_service(unsigned int budget);

typedef enum {
  XHCI_PORT_EVENT_OK,
  XHCI_PORT_EVENT_UNAVAILABLE,
  XHCI_PORT_EVENT_FAILED,
  XHCI_PORT_EVENT_TIMEOUT
} xhci_port_event_result_t;

xhci_port_event_result_t xhci_port_event_wait(
    uint8_t port, uint32_t timeout_ms);

typedef struct {
  uint8_t port;
  uint8_t completion_code;
  uint32_t status;
  uint32_t control;
} xhci_port_change_t;

bool xhci_port_event_read(xhci_port_change_t *result);

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

bool xhci_msi_test_set_enabled(bool enabled);

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
xhci_slot_result_t xhci_enable_downstream_slot(uint8_t hub_port,
    xhci_slot_info_t *result);

typedef enum {
  XHCI_DISABLE_OK,
  XHCI_DISABLE_NOT_READY,
  XHCI_DISABLE_BUSY,
  XHCI_DISABLE_ALREADY_ATTEMPTED,
  XHCI_DISABLE_COMMAND_FAILED
} xhci_disable_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t completion_code;
  xhci_command_result_t command_result;
} xhci_disable_info_t;

xhci_disable_result_t xhci_downstream_disable(uint8_t slot_id,
    xhci_disable_info_t *info);

typedef enum {
  XHCI_STOP_OK,
  XHCI_STOP_NOT_READY,
  XHCI_STOP_BUSY,
  XHCI_STOP_ALREADY_ATTEMPTED,
  XHCI_STOP_INVALID_STATE,
  XHCI_STOP_COMMAND_FAILED,
  XHCI_STOP_VERIFY_FAILED
} xhci_stop_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t endpoint_id;
  uint8_t before_state;
  uint8_t after_state;
  uint8_t completion_code;
  xhci_command_result_t command_result;
  bool stop_event_received;
  uint8_t stop_event_code;
  uint64_t stop_event_trb;
} xhci_stop_info_t;

xhci_stop_result_t xhci_downstream_stop_bulk_out(
    uint8_t slot_id, xhci_stop_info_t *info);
xhci_stop_result_t xhci_downstream_stop_bulk_in(
    uint8_t slot_id, xhci_stop_info_t *info);

typedef enum {
  XHCI_CLEANUP_OK,
  XHCI_CLEANUP_NOT_READY,
  XHCI_CLEANUP_BUSY,
  XHCI_CLEANUP_DMA_FAILED,
  XHCI_CLEANUP_ALREADY_DONE
} xhci_cleanup_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t buffers_released;
  uint32_t pages_released;
} xhci_cleanup_info_t;

xhci_cleanup_result_t xhci_downstream_cleanup(
    uint8_t slot_id, xhci_cleanup_info_t *info);
bool xhci_downstream_is_cleaned(uint8_t slot_id);
bool xhci_downstream_any_owned(void);

typedef enum {
  XHCI_CONNECTION_UNCERTAIN,
  XHCI_CONNECTION_UNOWNED,
  XHCI_CONNECTION_OWNED
} xhci_connection_ownership_t;

// Inspect resource ownership for one connection
// on the internal USB 2.0 hub.
//
// UNOWNED means no context claims the connection.
// OWNED includes a reserved but unconfirmed slot.
// UNCERTAIN means ownership cannot be determined
// uniquely and must not authorise resource reuse.
bool xhci_downstream_connection_ownership(uint8_t root_port, uint8_t hub_slot,
    uint8_t hub_port, xhci_connection_ownership_t *ownership);

typedef enum {
  XHCI_RECYCLE_OK,
  XHCI_RECYCLE_NOT_READY,
  XHCI_RECYCLE_BUSY,
  XHCI_RECYCLE_DMA_FAILED
} xhci_recycle_result_t;

xhci_recycle_result_t xhci_downstream_recycle(uint8_t slot_id);

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
  uint32_t slot_dword2;
  uint32_t ep0_dword1;
  uint32_t ep0_dword2;
  uint32_t ep0_dword3;
} xhci_device_info_t;

xhci_device_result_t xhci_device_prepare(xhci_device_info_t *result);
xhci_device_result_t xhci_downstream_prepare(
    uint8_t hub_port, uint8_t speed, xhci_device_info_t *result);

typedef enum {
  XHCI_ADDRESS_OK,
  XHCI_ADDRESS_NOT_READY,
  XHCI_ADDRESS_ALREADY_ATTEMPTED,
  XHCI_ADDRESS_COMMAND_FAILED,
  XHCI_ADDRESS_BAD_SLOT,
  XHCI_ADDRESS_VERIFY_FAILED
} xhci_address_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t usb_address;
  uint8_t slot_state;
  uint8_t ep0_state;
  uint8_t completion_code;
  uint32_t controller_status;
  uint32_t slot_dword3;
  uint32_t ep0_dword0;
  uint64_t elapsed_us;
  xhci_command_result_t command_result;
} xhci_address_info_t;

xhci_address_result_t xhci_address_device(xhci_address_info_t *result);
xhci_address_result_t xhci_address_downstream(
    uint8_t slot_id, xhci_address_info_t *result);

typedef enum {
  XHCI_EP0_UPDATE_OK,
  XHCI_EP0_UPDATE_NOT_READY,
  XHCI_EP0_UPDATE_ALREADY_ATTEMPTED,
  XHCI_EP0_UPDATE_DMA_FAILED,
  XHCI_EP0_UPDATE_COMMAND_FAILED,
  XHCI_EP0_UPDATE_VERIFY_FAILED
} xhci_ep0_update_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t completion_code;
  uint8_t ep0_state;
  uint16_t old_mps;
  uint16_t new_mps;
  uint32_t input_dword1;
  uint32_t output_dword1;
  uint64_t elapsed_us;
  xhci_command_result_t command_result;
} xhci_ep0_update_info_t;

xhci_ep0_update_result_t xhci_downstream_ep0_update(uint8_t slot_id,
    uint16_t max_packet_size, xhci_ep0_update_info_t *result);

typedef enum {
  XHCI_EP0_RECOVERY_BLOCKED,
  XHCI_EP0_RECOVERY_LATE_SUCCESS,
  XHCI_EP0_RECOVERY_HALTED_ERROR
} xhci_ep0_recovery_t;

// Classify a failed EP0 operation using its
// retained completion owner and hardware state.
//
// This does not quiesce or disable the slot.
xhci_ep0_recovery_t xhci_downstream_ep0_recovery(uint8_t slot_id);

typedef enum {
  XHCI_HUB_OK,
  XHCI_HUB_NOT_READY,
  XHCI_HUB_ALREADY_CONFIGURED,
  XHCI_HUB_DMA_FAILED,
  XHCI_HUB_COMMAND_FAILED,
  XHCI_HUB_VERIFY_FAILED
} xhci_hub_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t ports;
  uint8_t tt_think_time;
  uint8_t completion_code;
  uint32_t slot_dword0;
  uint32_t slot_dword1;
  uint32_t slot_dword2;
  uint32_t output_dword0;
  uint32_t output_dword1;
  uint32_t output_dword2;
  xhci_command_result_t command_result;
} xhci_hub_info_t;

xhci_hub_result_t xhci_hub_configure( uint8_t ports, uint8_t tt_think_time,
    xhci_hub_info_t *result);

typedef enum {
  XHCI_BULK_CONFIG_OK,
  XHCI_BULK_CONFIG_NOT_READY,
  XHCI_BULK_CONFIG_ALREADY_ATTEMPTED,
  XHCI_BULK_CONFIG_INVALID_ENDPOINT,
  XHCI_BULK_CONFIG_ALLOCATION_FAILED,
  XHCI_BULK_CONFIG_DMA_FAILED,
  XHCI_BULK_CONFIG_COMMAND_FAILED,
  XHCI_BULK_CONFIG_VERIFY_FAILED
} xhci_bulk_config_result_t;

typedef struct {
  uint8_t slot_id;
  uint8_t completion_code;
  uint8_t in_state;
  uint8_t out_state;
  uint32_t in_context;
  uint32_t out_context;
  uint64_t in_ring;
  uint64_t out_ring;
  xhci_command_result_t command_result;
} xhci_bulk_config_info_t;

xhci_bulk_config_result_t xhci_downstream_bulk_configure(
    uint8_t slot_id, uint8_t in_address, uint16_t in_mps,
    uint8_t out_address, uint16_t out_mps, xhci_bulk_config_info_t *result);

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

typedef enum {
  XHCI_INTERRUPT_CONFIG_OK,
  XHCI_INTERRUPT_CONFIG_NOT_READY,
  XHCI_INTERRUPT_CONFIG_INVALID_ENDPOINT,
  XHCI_INTERRUPT_CONFIG_ALLOCATION_FAILED,
  XHCI_INTERRUPT_CONFIG_DMA_FAILED,
  XHCI_INTERRUPT_CONFIG_COMMAND_FAILED,
  XHCI_INTERRUPT_CONFIG_VERIFY_FAILED,
  XHCI_INTERRUPT_CONFIG_ALREADY_ATTEMPTED
} xhci_interrupt_config_result_t;

typedef struct {
  uint64_t ring;
  uint8_t slot_id;
  uint8_t endpoint_id;
  uint8_t completion_code;
  uint8_t interval;
  xhci_command_result_t command_result;
} xhci_interrupt_config_info_t;

xhci_interrupt_config_result_t xhci_hub_interrupt_configure(
    uint8_t address, uint16_t mps, uint8_t interval,
    xhci_interrupt_config_info_t *info);

typedef enum {
  XHCI_HUB_NOTIFY_IDLE,
  XHCI_HUB_NOTIFY_PENDING,
  XHCI_HUB_NOTIFY_COMPLETE,
  XHCI_HUB_NOTIFY_FAILED
} xhci_hub_notify_state_t;

typedef enum {
  XHCI_HUB_NOTIFY_OK,
  XHCI_HUB_NOTIFY_NOT_READY,
  XHCI_HUB_NOTIFY_ALREADY_STARTED,
  XHCI_HUB_NOTIFY_DMA_FAILED,
  XHCI_HUB_NOTIFY_RING_FAILED,
  XHCI_HUB_NOTIFY_WAITER_FAILED
} xhci_hub_notify_result_t;

typedef struct {
  xhci_hub_notify_state_t state;
  uint8_t slot_id;
  uint8_t endpoint_id;
  uint8_t completion_code;
  uint8_t length;
  uint16_t changes;
  uint64_t transfer_trb;
  uint32_t notifications;
  uint32_t ring_wraps;
} xhci_hub_notify_info_t;

xhci_hub_notify_result_t xhci_hub_notify_start(void);
void xhci_hub_notify_service(void);
bool xhci_hub_notify_get_info(xhci_hub_notify_info_t *info);
xhci_hub_notify_result_t xhci_hub_notify_resume(void);
bool xhci_downstream_quiesce(uint8_t slot_id);
bool xhci_downstream_is_quiesced(uint8_t slot_id);
bool xhci_downstream_is_slot_disabled(uint8_t slot_id);

typedef struct {
  uint8_t slot_id;
  uint8_t active_waiters;
  bool quiesced;
  bool ep0_active;
  bool ep0_failed;
  bool ep0_submitted;
  bool ep0_completion_seen;
  uint8_t ep0_completion_code;
  bool ep0_uncertain;
  uint32_t ep0_late_completions;
  bool ep0_state_valid;
  uint8_t ep0_state;
  bool bulk_out_active;
  bool bulk_out_waiter_pending;
  bool bulk_in_started;
  bool bulk_in_pending;
  bool bulk_in_completed;
  bool bulk_in_failed;
  uint8_t bulk_in_completion_code;
  bool endpoint_states_valid;
  uint8_t bulk_in_ep_state;
  uint8_t bulk_out_ep_state;
} xhci_downstream_transfer_state_t;

bool xhci_downstream_transfer_state(
    uint8_t slot_id, xhci_downstream_transfer_state_t *state);
