#pragma once

#include <stdbool.h>
#include <stdint.h>

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
bool usb_lifecycle_get_run_info(usb_lifecycle_run_info_t *info);

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
  uint32_t acknowledgements;
  uint32_t errors;
  uint8_t queued;
  uint8_t last_error_port;
  bool failed;
} usb_change_stats_t;

bool usb_change_read(usb_change_event_t *event);
bool usb_change_get_stats(usb_change_stats_t *stats);

