#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "timer.h"
#include "usb_device.h"

typedef enum {
  USB_INIT_OK,
  USB_INIT_PORT_CLEAR_FAILED,
  USB_INIT_PORT_READ_FAILED,
  USB_INIT_PORT_RESET_FAILED,
  USB_INIT_PORT_STATE_FAILED,
  USB_INIT_EVENT_FAILED,
  USB_INIT_EVENT_TIMEOUT,
  USB_INIT_SLOT_FAILED,
  USB_INIT_CONTEXT_FAILED,
  USB_INIT_ADDRESS_FAILED
} usb_init_result_t;

typedef struct {
  usb_init_result_t result;
  uint32_t detail;
  uint8_t root_port;
  uint8_t speed;
  uint8_t slot_id;
  uint8_t address;
  uint8_t slot_state;
  uint8_t ep0_state;
} usb_init_info_t;

usb_init_result_t usb_init(usb_init_info_t *info);
bool usb_get_init_info(usb_init_info_t *info);
void usb_service(void);
bool usb_service_available(void);

typedef struct {
  bool enabled;
  bool pending;
  uint32_t attempts;
  uint32_t successes;
  uint32_t failures;
  uint32_t last_result;
} usb_auto_enum_info_t;

bool usb_auto_enum_get_info(usb_auto_enum_info_t *info);
// Per-port requests retained independently of the
// current single-device automatic enumeration flag.
//
// Port n occupies bit n; bit zero is unused.
typedef struct {
  uint16_t pending_ports;
  uint8_t next_port;
} usb_auto_enum_ports_info_t;

bool usb_auto_enum_ports_get_info(usb_auto_enum_ports_info_t *info);

void usb_auto_enum_set_enabled(bool enabled);

typedef enum {
  USB_LIFECYCLE_IDLE,
  USB_LIFECYCLE_RECONCILE_IN,
  USB_LIFECYCLE_STOP_IN,
  USB_LIFECYCLE_DISABLE,
  USB_LIFECYCLE_RETIRE_IN,
  USB_LIFECYCLE_CLEANUP,
  USB_LIFECYCLE_DETACH,
  USB_LIFECYCLE_RECYCLE,
  USB_LIFECYCLE_REARM,
  USB_LIFECYCLE_BLOCKED
} usb_lifecycle_action_t;

typedef struct {
  usb_lifecycle_action_t next;
  uint32_t generation;
  uint8_t slot_id;
  uint8_t waiters;
  bool quiesced;
  bool slot_disabled;
  bool cleaned;
  bool in_started;
  bool in_pending;
  bool in_completed;
} usb_lifecycle_info_t;

bool usb_lifecycle_inspect(usb_lifecycle_info_t *info);
// Inspect the lifecycle of a specific device
// incarnation, without compatibility fallback.
bool usb_lifecycle_inspect_generation(
    uint32_t generation, usb_lifecycle_info_t *info);

typedef enum {
  USB_LIFECYCLE_RUN_DISARMED,
  USB_LIFECYCLE_RUN_ARMED,
  USB_LIFECYCLE_RUN_COMPLETE,
  USB_LIFECYCLE_RUN_FAILED
} usb_lifecycle_run_state_t;

typedef enum {
  USB_LIFECYCLE_FAULT_NONE,
  USB_LIFECYCLE_FAULT_INSPECTION,
  USB_LIFECYCLE_FAULT_GENERATION,
  USB_LIFECYCLE_FAULT_BLOCKED,
  USB_LIFECYCLE_FAULT_OPERATION,
  USB_LIFECYCLE_FAULT_XHCI_SERVICE,
  USB_LIFECYCLE_FAULT_ROOT_PORT,
  USB_LIFECYCLE_FAULT_HUB
} usb_lifecycle_fault_t;

typedef struct {
  usb_lifecycle_run_state_t state;
  usb_lifecycle_action_t last_action;
  uint32_t generation;
  uint32_t steps;
  uint32_t last_result;
  bool last_succeeded;
  usb_lifecycle_fault_t fault;
  uint8_t slot_id;
  uint8_t command_result;
  uint8_t completion_code;
  uint32_t observed_generation;
} usb_lifecycle_run_info_t;

bool usb_lifecycle_arm(void);
// Arm lifecycle teardown for a specific device
// incarnation. Multiple concurrent executors
// are not yet supported.
bool usb_lifecycle_arm_generation(uint32_t generation);
bool usb_lifecycle_get_run_info(usb_lifecycle_run_info_t *info);
// Read-only inspection of a lifecycle executor
// belonging to a specific device incarnation.
bool usb_lifecycle_get_run_info_generation(
    uint32_t generation, usb_lifecycle_run_info_t *info);

typedef struct {
  uint32_t processed;
  uint32_t failures;
  uint32_t last_portsc;
  uint8_t last_port;
  uint8_t last_completion_code;
  bool root_fault;
} usb_port_monitor_info_t;

bool usb_get_port_monitor_info(usb_port_monitor_info_t *result);

typedef struct {
  uint32_t sequence;
  uint8_t port;
  uint16_t status;
  uint16_t change;
} usb_change_event_t;

typedef struct {
  uint32_t recorded;
  uint32_t consumed;
  uint32_t dropped;
  uint32_t acknowledgements;
  uint32_t errors;
  uint8_t queued;
  uint8_t last_error_port;
  bool failed;
} usb_change_stats_t;

bool usb_change_read(usb_change_event_t *event);
bool usb_change_get_stats(usb_change_stats_t *stats);

typedef struct {
  uint32_t generation;
  uint32_t last_sequence;
  uint8_t port;
  uint16_t last_status;
  uint16_t last_change;
  uint16_t current_status;
  uint16_t current_change;
  bool notification_seen;
  bool connected;
  bool powered;
} usb_lifecycle_port_info_t;

bool usb_lifecycle_port_inspect(usb_lifecycle_port_info_t *info);

typedef enum {
  USB_REENUM_READY,
  USB_REENUM_SERVICE_FAILED,
  USB_REENUM_HUB_FAILED,
  USB_REENUM_NO_HISTORY,
  USB_REENUM_DEVICE_OWNED,
  USB_REENUM_EXECUTOR_ACTIVE,
  USB_REENUM_EXECUTOR_FAILED,
  USB_REENUM_NOT_REARMED,
  USB_REENUM_XHCI_BUSY,
  USB_REENUM_PORT_UNAVAILABLE,
  USB_REENUM_DISCONNECTED,
  USB_REENUM_NOT_POWERED,
  USB_REENUM_CHANGE_PENDING,
  USB_REENUM_MULTIPLE_DEVICES,
  USB_REENUM_UNMANAGED_ROOT
} usb_reenum_result_t;

typedef struct {
  usb_reenum_result_t result;
  uint32_t generation;
  uint8_t port;
  uint8_t previous_port;
  uint8_t connected_count;
  usb_device_state_t device_state;
  uint16_t port_status;
  uint16_t port_change;
  bool enumeration_attempted;
  uint8_t unmanaged_root_count;
} usb_reenum_info_t;

bool usb_reenum_inspect(usb_reenum_info_t *info);

typedef enum {
  USB_ENUM_ADMIT_INITIAL,
  USB_ENUM_ADMIT_RECONNECT,
  USB_ENUM_ADMIT_SERVICE_FAILED,
  USB_ENUM_ADMIT_HUB_FAILED,
  USB_ENUM_ADMIT_HUB_NOT_READY,
  USB_ENUM_ADMIT_ALREADY_ATTEMPTED,
  USB_ENUM_ADMIT_XHCI_BUSY,
  USB_ENUM_ADMIT_NO_DEVICE,
  USB_ENUM_ADMIT_MULTIPLE_DEVICES,
  USB_ENUM_ADMIT_PORT_FAILED,
  USB_ENUM_ADMIT_CHANGE_PENDING,
  USB_ENUM_ADMIT_RECONNECT_BLOCKED,
  USB_ENUM_ADMIT_UNMANAGED_ROOT
} usb_enum_admission_result_t;

typedef struct {
  usb_enum_admission_result_t result;
  usb_reenum_result_t reconnect_reason;
  uint32_t generation;
  uint8_t port;
  uint8_t connected_count;
  uint8_t unmanaged_root_count;
} usb_enum_admission_info_t;

bool usb_enum_admission_inspect(usb_enum_admission_info_t *info);

// Port-local enumeration prerequisites.
//
// ELIGIBLE does not grant permission to enumerate.
// The controller-wide admission policy remains
// authoritative.
typedef enum {
  USB_ENUM_PORT_ELIGIBLE,
  USB_ENUM_PORT_SERVICE_FAILED,
  USB_ENUM_PORT_HUB_FAILED,
  USB_ENUM_PORT_HUB_NOT_READY,
  USB_ENUM_PORT_INVALID,
  USB_ENUM_PORT_STATUS_FAILED,
  USB_ENUM_PORT_CHANGE_PENDING,
  USB_ENUM_PORT_DISCONNECTED,
  USB_ENUM_PORT_NOT_POWERED,
  USB_ENUM_PORT_OVERCURRENT,
  USB_ENUM_PORT_RESETTING,
  USB_ENUM_PORT_AMBIGUOUS,
  USB_ENUM_PORT_ALREADY_ATTEMPTED,
  USB_ENUM_PORT_DEVICE_OWNED,
  USB_ENUM_PORT_UNSAFE_HISTORY,
  USB_ENUM_PORT_XHCI_OWNED,
  USB_ENUM_PORT_XHCI_UNCERTAIN
} usb_enum_port_result_t;

typedef struct {
  usb_enum_port_result_t result;
  uint8_t port;
  uint16_t status;
  uint16_t change;
  uint32_t generation;
  uint8_t slot_id;
} usb_enum_port_info_t;

// Inspect one downstream port without changing
// enumeration, lifecycle or ownership state.
bool usb_enum_port_inspect(uint8_t port, usb_enum_port_info_t *info);

// Read-only combination of port-local eligibility
// and the existing controller-wide admission policy.
//
// would_admit is a diagnostic assessment, not
// authorisation to begin enumeration. The observations
// are not an atomic hardware snapshot.
typedef struct {
  usb_enum_port_info_t port;
  usb_enum_admission_info_t controller;
  bool would_admit;
} usb_enum_candidate_info_t;

bool usb_enum_candidate_inspect(
    uint8_t port, usb_enum_candidate_info_t *info);

