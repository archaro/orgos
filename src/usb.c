#include <stdbool.h>
#include <stdint.h>

#include "xhci.h"
#include "usb.h"
#include "usb_hub.h"
#include "usb_enumerate.h"
#include "usb_midi_test.h"

#define USB_ROOT_PORT 1U
#define USB_HIGH_SPEED 3U
#define USB_LINK_U0 0U
#define USB_ROOT_EVENT_WAIT_MS 100U
#define USB_CHANGE_QUEUE_CAPACITY 16U

static bool usb_initialised;
static usb_init_info_t usb_state;
static bool usb_attempted;
static bool usb_service_failed;
static usb_port_monitor_info_t port_monitor;
static usb_change_event_t change_queue[USB_CHANGE_QUEUE_CAPACITY];
static unsigned int change_head;
static unsigned int change_count;
static usb_change_stats_t change_stats;
static usb_change_event_t last_port_change[16];
static usb_lifecycle_run_info_t lifecycle_run;
static usb_auto_enum_info_t auto_enum = {
  .enabled = true,
  .pending = true
};

// A completed notification may identify several ports.
// Retain progress until all have been processed.
static uint16_t change_pending_mask;
static uint8_t change_next_port = 1U;

static bool usb_port_valid(const xhci_port_t *port) {
  return port->connected && port->enabled && port->powered &&
      !port->overcurrent && !port->resetting &&
      port->speed == USB_HIGH_SPEED &&
      port->link_state == USB_LINK_U0;
}

static usb_init_result_t usb_root_port_prepare(usb_init_info_t *info) {
  xhci_port_clear_info_t clear;
  xhci_port_reset_info_t reset;
  xhci_port_t port;
  xhci_port_clear_result_t clear_result =
      xhci_port_clear(USB_ROOT_PORT, &clear);
  if (clear_result != XHCI_PORT_CLEAR_OK) {
    info->detail = (uint32_t)clear_result;
    return USB_INIT_PORT_CLEAR_FAILED;
  }

  if (!xhci_port_read(USB_ROOT_PORT, &port))
    return USB_INIT_PORT_READ_FAILED;

  // A previously enabled port does not require another reset.
  if (port.enabled) {
    if (!usb_port_valid(&port))
      return USB_INIT_PORT_STATE_FAILED;
    info->speed = port.speed;
    return USB_INIT_OK;
  }

  xhci_port_reset_result_t reset_result =
      xhci_port_reset(USB_ROOT_PORT, &reset);

  if (reset_result != XHCI_PORT_RESET_OK) {
    info->detail = (uint32_t)reset_result;
    return USB_INIT_PORT_RESET_FAILED;
  }

  if (!xhci_port_read(USB_ROOT_PORT, &port))
    return USB_INIT_PORT_READ_FAILED;

  if (!usb_port_valid(&port)) {
    info->detail = port.portsc;
    return USB_INIT_PORT_STATE_FAILED;
  }

  // Wait for the root port's change notification.
  // The xHCI dispatcher owns the hardware event ring.
  xhci_port_event_result_t event_result =
      xhci_port_event_wait(USB_ROOT_PORT, USB_ROOT_EVENT_WAIT_MS);

  if (event_result != XHCI_PORT_EVENT_OK) {
    info->detail = (uint32_t)event_result;

    if (event_result == XHCI_PORT_EVENT_TIMEOUT)
      return USB_INIT_EVENT_TIMEOUT;

    return USB_INIT_EVENT_FAILED;
  }

  info->speed = port.speed;
  return USB_INIT_OK;
}

bool usb_get_init_info(usb_init_info_t *info) {
  if (!info)
    return false;

  *info = usb_state;
  return true;
}

bool usb_service_available(void) {
  return usb_initialised && !usb_service_failed;
}

usb_init_result_t usb_init(usb_init_info_t *info) {
  if (!info)
    return USB_INIT_PORT_READ_FAILED;

  if (usb_attempted) {
    *info = usb_state;
    return usb_state.result;
  }

  usb_attempted = true;
  usb_init_info_t state = {0};
  state.root_port = USB_ROOT_PORT;

  usb_init_result_t result = usb_root_port_prepare(&state);
  if (result != USB_INIT_OK)
    goto complete;

  xhci_slot_info_t slot;
  xhci_slot_result_t slot_result = xhci_enable_slot(&slot);
  if (slot_result != XHCI_SLOT_OK) {
    state.detail = (uint32_t)slot_result;
    result = USB_INIT_SLOT_FAILED;
    goto complete;
  }

  state.slot_id = slot.slot_id;
  xhci_device_info_t device;
  xhci_device_result_t device_result = xhci_device_prepare(&device);
  if (device_result != XHCI_DEVICE_OK) {
    state.detail = (uint32_t)device_result;
    result = USB_INIT_CONTEXT_FAILED;
    goto complete;
  }

  xhci_address_info_t address;
  xhci_address_result_t address_result = xhci_address_device(&address);
  if (address_result != XHCI_ADDRESS_OK) {
    // Diagnostic: preserve the address result, xHCI
    // command result and hardware completion code.
    state.detail = (uint32_t)address_result |
        ((uint32_t)address.command_result << 8) |
        ((uint32_t)address.completion_code << 16);
    result = USB_INIT_ADDRESS_FAILED;
    goto complete;
  }
  state.address = address.usb_address;
  state.slot_state = address.slot_state;
  state.ep0_state = address.ep0_state;
  usb_initialised = true;

complete:
  state.result = result;
  usb_state = state;
  *info = state;
  return result;
}

bool usb_get_port_monitor_info(usb_port_monitor_info_t *result) {
  if (!result)
    return false;

  *result = port_monitor;
  return true;
}

bool usb_auto_enum_get_info(usb_auto_enum_info_t *info) {
  if (!info)
    return false;

  *info = auto_enum;
  return true;
}

void usb_auto_enum_set_enabled(bool enabled) {
  auto_enum.enabled = enabled;

  // Enabling requests one fresh admission check.
  // It never overrides the existing enumeration
  // or lifecycle ownership restrictions.
  if (enabled)
    auto_enum.pending = true;
}

static void usb_port_monitor_fault(uint8_t port) {
  port_monitor.failures++;

  // The internal USB 2.0 hub is on root port 1.
  // Losing it invalidates our current USB topology.
  if (port == USB_ROOT_PORT) {
    port_monitor.root_fault = true;
    usb_service_failed = true;
  }
}

static void usb_service_port_events(void) {
  xhci_port_change_t event;

  // Bound the work performed by each foreground pass.
  for (unsigned int i = 0U; i < 8U; i++) {
    if (!xhci_port_event_read(&event))
      return;

    port_monitor.processed++;
    port_monitor.last_port = event.port;
    port_monitor.last_completion_code = event.completion_code;
    port_monitor.last_portsc = 0U;

    if (event.completion_code != 1U) {
      usb_port_monitor_fault(event.port);
      if (usb_service_failed)
        return;
      continue;
    }

    // An event is a notification, not a snapshot.
    // Read the current state of the affected port.
    xhci_port_t port;
    if (!xhci_port_read(event.port, &port)) {
      usb_port_monitor_fault(event.port);
      if (usb_service_failed)
        return;
      continue;
    }

    port_monitor.last_portsc = port.portsc;

    // A connection change on the internal hub means
    // that our existing device contexts may be stale,
    // even if the port has already reconnected.
    if (event.port == USB_ROOT_PORT &&
        (!usb_port_valid(&port) || port.connect_change)) {
      usb_port_monitor_fault(event.port);
      return;
    }

    xhci_port_clear_info_t clear;
    if (xhci_port_clear(event.port, &clear) != XHCI_PORT_CLEAR_OK) {
      usb_port_monitor_fault(event.port);
      if (usb_service_failed)
        return;
      continue;
    }

    // Check again after acknowledging the change.
    // We must not continue using a lost root hub.
    if (event.port == USB_ROOT_PORT) {
      if (!xhci_port_read(event.port, &port) ||
          !usb_port_valid(&port) || port.connect_change) {
        usb_port_monitor_fault(event.port);
        return;
      }
    }
  }
}

bool usb_change_get_stats(usb_change_stats_t *stats) {
  if (!stats)
    return false;

  *stats = change_stats;
  stats->queued = (uint8_t)change_count;
  return true;
}

bool usb_change_read(usb_change_event_t *event) {
  if (!event || change_count == 0U)
    return false;

  *event = change_queue[change_head];
  change_head = (change_head + 1U) % USB_CHANGE_QUEUE_CAPACITY;
  change_count--;
  change_stats.consumed++;

  return true;
}

static void usb_change_enqueue( uint8_t port, const usb_hub_port_t *status) {
  unsigned int tail = (change_head + change_count) %
      USB_CHANGE_QUEUE_CAPACITY;

  change_stats.recorded++;
  change_queue[tail] = (usb_change_event_t){
    .sequence = change_stats.recorded,
    .port = port,
    .status = status->status,
    .change = status->change
  };

  // Keep the latest notification independently
  // of the consumable diagnostic event queue.
  if (port < 16U)
    last_port_change[port] = change_queue[tail];

  change_count++;

  // A connection change may introduce a new
  // device or invalidate the previous generation.
  // Admission will decide whether enumeration
  // is presently permitted.
  if (status->change & 0x0001U)
    auto_enum.pending = true;

  // Notify the lifecycle manager independently
  // of diagnostic queue consumption.
  usb_device_port_changed(port, status->status, status->change);
}

static void usb_change_fail(uint8_t port) {
  change_stats.failed = true;
  change_stats.errors++;
  change_stats.last_error_port = port;
}

static void usb_service_hub_changes(void) {
  if (change_stats.failed)
    return;

  xhci_hub_notify_info_t notify = {0};
  if (!xhci_hub_notify_get_info(&notify))
    return;

  if (notify.state == XHCI_HUB_NOTIFY_FAILED) {
    usb_change_fail(0U);
    return;
  }

  if (notify.state != XHCI_HUB_NOTIFY_COMPLETE)
    return;

  // Acquire the notification bitmap once.
  if (change_pending_mask == 0U) {
    change_pending_mask = notify.changes;
    change_next_port = 1U;
  }

  // Bit zero reports hub-level changes, which
  // require separate hub-status handling.
  if (change_pending_mask & 1U) {
    usb_change_fail(0U);
    return;
  }

  usb_hub_init_info_t hub = {0};
  if (!usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK) {
    usb_change_fail(0U);
    return;
  }

  uint16_t supported = (uint16_t)((1U << (hub.ports + 1U)) - 1U);
  if (change_pending_mask & ~supported) {
    usb_change_fail(0U);
    return;
  }

  for (uint8_t port = change_next_port; port <= hub.ports; port++) {
    uint16_t bit = (uint16_t)(1U << port);
    if (!(change_pending_mask & bit))
      continue;

    // Pause without acknowledging anything if
    // there is no room to preserve another event.
    if (change_count == USB_CHANGE_QUEUE_CAPACITY) {
      change_next_port = port;
      return;
    }

    usb_hub_port_t status = {0};
    usb_hub_result_t result = usb_hub_port_status(port, &status);

    if (result != USB_HUB_OK) {
      usb_change_fail(port);
      return;
    }

    // Preserve the full snapshot before clearing
    // any of its hardware change flags.
    if (status.change != 0U) {
      // Overcurrent and other unsupported changes
      // require an explicit recovery policy.
      if (status.change & ~((1U << 0) | (1U << 1) | (1U << 4))) {
        usb_change_fail(port);
        return;
      }

      usb_change_enqueue(port, &status);
      result = usb_hub_port_acknowledge(port, status.change);
      if (result != USB_HUB_OK) {
        usb_change_fail(port);
        return;
      }

      change_stats.acknowledgements++;
    }
    change_pending_mask &= (uint16_t)~bit;
    change_next_port = port + 1U;
  }

  // All ports described by this notification
  // have been recorded and acknowledged.
  change_pending_mask = 0U;
  change_next_port = 1U;

  if (xhci_hub_notify_resume() != XHCI_HUB_NOTIFY_OK)
    usb_change_fail(0U);
}

bool usb_lifecycle_inspect(usb_lifecycle_info_t *info) {
  if (!info)
    return false;

  *info = (usb_lifecycle_info_t){0};
  usb_device_t device = {0};
  if (!usb_get_device(&device))
    return true;

  info->generation = device.generation;
  info->slot_id = device.slot_id;
  info->quiesced = xhci_downstream_is_quiesced();
  info->slot_disabled = xhci_downstream_is_slot_disabled();
  info->cleaned = xhci_downstream_is_cleaned();

  if (device.state == USB_DEVICE_ACTIVE)
    return true;

  if (device.state == USB_DEVICE_DETACHED) {
    if (info->quiesced)
      info->next = USB_LIFECYCLE_RECYCLE;
    else {
      usb_enum_info_t enumeration = {0};
      if (usb_get_enumeration(&enumeration))
        info->next = USB_LIFECYCLE_REARM;
    }
    return true;
  }

  if (device.state != USB_DEVICE_DISCONNECTING ||
      !info->quiesced || !usb_service_available()) {
    info->next = USB_LIFECYCLE_BLOCKED;
    return true;
  }

  if (info->cleaned) {
    info->next = USB_LIFECYCLE_DETACH;
    return true;
  }

  xhci_downstream_transfer_state_t transfers = {0};
  if (!xhci_downstream_transfer_state(&transfers)) {
    info->next = USB_LIFECYCLE_BLOCKED;
    return true;
  }

  info->waiters = transfers.active_waiters;
  info->in_started = transfers.bulk_in_started;
  info->in_pending = transfers.bulk_in_pending;
  info->in_completed = transfers.bulk_in_completed;
  if (info->slot_disabled) {
    if (transfers.active_waiters != 0U || transfers.bulk_in_pending) {
      info->next = USB_LIFECYCLE_BLOCKED;
    } else if (transfers.bulk_in_started) {
      info->next = USB_LIFECYCLE_RETIRE_IN;
    } else {
      info->next = USB_LIFECYCLE_CLEANUP;
    }
    return true;
  }

  if (transfers.ep0_active || transfers.bulk_out_active) {
    info->next = USB_LIFECYCLE_BLOCKED;
    return true;
  }

  if (transfers.bulk_in_pending) {
    if (!transfers.bulk_in_started || transfers.active_waiters != 1U) {
      info->next = USB_LIFECYCLE_BLOCKED;
    } else if (transfers.bulk_in_completed) {
      info->next = USB_LIFECYCLE_RECONCILE_IN;
    } else if (transfers.endpoint_states_valid &&
        transfers.bulk_in_ep_state == 1U) {
      info->next = USB_LIFECYCLE_STOP_IN;
    } else {
      info->next = USB_LIFECYCLE_BLOCKED;
    }
    return true;
  }

  info->next = transfers.active_waiters == 0U ?
      USB_LIFECYCLE_DISABLE : USB_LIFECYCLE_BLOCKED;

  return true;
}

bool usb_lifecycle_get_run_info(
    usb_lifecycle_run_info_t *info) {
  if (!info)
    return false;

  *info = lifecycle_run;
  return true;
}

bool usb_lifecycle_arm(void) {
  usb_device_t device = {0};

  if (!usb_service_available() || change_stats.failed ||
      !usb_get_device(&device) ||
      (device.state != USB_DEVICE_ACTIVE &&
       device.state != USB_DEVICE_DISCONNECTING))
    return false;

  if (lifecycle_run.state == USB_LIFECYCLE_RUN_ARMED)
    return false;

  // Never retry a failed teardown or start another
  // one for the same generation.
  if (lifecycle_run.state != USB_LIFECYCLE_RUN_DISARMED &&
      lifecycle_run.generation == device.generation)
    return false;

  lifecycle_run = (usb_lifecycle_run_info_t){
    .state = USB_LIFECYCLE_RUN_ARMED,
    .generation = device.generation,
    .observed_generation = device.generation,
    .slot_id = device.slot_id
  };

  return true;
}

static void usb_lifecycle_auto_arm(void) {
  usb_device_t device = {0};
  usb_enum_info_t enumeration = {0};

  if (!usb_get_device(&device) || device.state != USB_DEVICE_DISCONNECTING ||
      device.slot_id == 0U || !xhci_downstream_is_quiesced() ||
      !usb_get_enumeration(&enumeration) ||
      enumeration.result != USB_ENUM_OK ||
      enumeration.slot_id != device.slot_id)
    return;

  // Never restart teardown for a generation whose
  // executor is already armed, complete or failed.
  if (lifecycle_run.generation == device.generation &&
      lifecycle_run.state != USB_LIFECYCLE_RUN_DISARMED)
    return;

  // All controller operations remain the
  // responsibility of the existing executor.
  (void)usb_lifecycle_arm();
}

static void usb_lifecycle_fail(usb_lifecycle_fault_t fault) {
  if (lifecycle_run.state != USB_LIFECYCLE_RUN_ARMED)
    return;

  lifecycle_run.fault = fault;
  lifecycle_run.state = USB_LIFECYCLE_RUN_FAILED;
  lifecycle_run.last_succeeded = false;
}

static void usb_lifecycle_service(void) {
  if (lifecycle_run.state != USB_LIFECYCLE_RUN_ARMED)
    return;

  usb_lifecycle_info_t info = {0};
  if (!usb_lifecycle_inspect(&info)) {
    usb_lifecycle_fail(USB_LIFECYCLE_FAULT_INSPECTION);
    return;
  }

  lifecycle_run.observed_generation = info.generation;
  if (info.generation != lifecycle_run.generation) {
    usb_lifecycle_fail(USB_LIFECYCLE_FAULT_GENERATION);
    return;
  }

  // No teardown is needed while the device
  // remains active.
  if (info.next == USB_LIFECYCLE_IDLE)
    return;

  lifecycle_run.last_action = info.next;
  lifecycle_run.last_succeeded = false;
  lifecycle_run.last_result = 0U;
  lifecycle_run.command_result = 0U;
  lifecycle_run.completion_code = 0U;
  bool succeeded = false;

  switch (info.next) {
    case USB_LIFECYCLE_RECONCILE_IN: {
      xhci_in_reconcile_result_t result = xhci_bulk_in_quiesced_reconcile();
      lifecycle_run.last_result = (uint32_t)result;
      succeeded = result == XHCI_IN_RECONCILE_OK ||
          result == XHCI_IN_RECONCILE_RESOLVED;
      break;
    }
    case USB_LIFECYCLE_STOP_IN: {
      xhci_stop_info_t result = {0};
      xhci_stop_result_t status = xhci_downstream_stop_bulk_in(&result);
      lifecycle_run.last_result = (uint32_t)status;
      lifecycle_run.command_result = (uint8_t)result.command_result;
      lifecycle_run.completion_code = result.completion_code;
      succeeded = status == XHCI_STOP_OK;
      break;
    }
    case USB_LIFECYCLE_DISABLE: {
      xhci_disable_info_t result = {0};
      xhci_disable_result_t status = xhci_downstream_disable(&result);
      lifecycle_run.last_result = (uint32_t)status;
      lifecycle_run.command_result = (uint8_t)result.command_result;
      lifecycle_run.completion_code = result.completion_code;
      succeeded = status == XHCI_DISABLE_OK;
      break;
    }
    case USB_LIFECYCLE_RETIRE_IN: {
      xhci_bulk_in_retire_result_t status = xhci_bulk_in_stream_retire();
      lifecycle_run.last_result = (uint32_t)status;
      succeeded = status == XHCI_BULK_IN_RETIRE_OK;
      break;
    }
    case USB_LIFECYCLE_CLEANUP: {
      xhci_cleanup_info_t result = {0};
      xhci_cleanup_result_t status = xhci_downstream_cleanup(&result);
      lifecycle_run.last_result = (uint32_t)status;
      succeeded = status == XHCI_CLEANUP_OK;
      break;
    }
    case USB_LIFECYCLE_DETACH: {
      succeeded = usb_device_finalize_disconnect();
      lifecycle_run.last_result = succeeded ? 0U : 1U;
      break;
    }
    case USB_LIFECYCLE_RECYCLE: {
      xhci_recycle_result_t status = xhci_downstream_recycle();
      lifecycle_run.last_result = (uint32_t)status;
      succeeded = status == XHCI_RECYCLE_OK;
      break;
    }
    case USB_LIFECYCLE_REARM: {
      succeeded = usb_enumeration_rearm();
      lifecycle_run.last_result = succeeded ? 0U : 1U;
      break;
    }
    default: {
      // A blocked or unsupported state is not
      // permission to guess the next operation.
      usb_lifecycle_fail(USB_LIFECYCLE_FAULT_BLOCKED);
      return;
    }
  }

  lifecycle_run.steps++;
  lifecycle_run.last_succeeded = succeeded;
  if (!succeeded) {
    usb_lifecycle_fail(USB_LIFECYCLE_FAULT_OPERATION);
    return;
  }

  if (info.next == USB_LIFECYCLE_REARM) {
    lifecycle_run.state = USB_LIFECYCLE_RUN_COMPLETE;

    // The previous generation no longer owns
    // the downstream controller resources.
    // Check for a device already reconnected.
    auto_enum.pending = true;
  }
}

static void usb_auto_enum_service(void) {
  if (!auto_enum.enabled || !auto_enum.pending ||
      lifecycle_run.state == USB_LIFECYCLE_RUN_ARMED)
    return;

  usb_enum_admission_info_t admission = {0};
  if (!usb_enum_admission_inspect(&admission)) {
    auto_enum.pending = false;
    return;
  }

  if (admission.result != USB_ENUM_ADMIT_INITIAL &&
      admission.result != USB_ENUM_ADMIT_RECONNECT) {
    // Allow the hub-change handler to finish
    // acknowledging a connection notification.
    if (admission.result == USB_ENUM_ADMIT_CHANGE_PENDING ||
        (admission.result == USB_ENUM_ADMIT_RECONNECT_BLOCKED &&
         admission.reconnect_reason == USB_REENUM_CHANGE_PENDING))
      return;

    // All other refusals end this request.
    // Another connection event, completed teardown
    // or explicit enable will request a fresh check.
    auto_enum.pending = false;
    return;
  }

  // Consume the request before attempting enumeration.
  // Failure must never cause an automatic retry loop.
  auto_enum.pending = false;
  usb_enum_info_t enumeration = {0};
  usb_enum_result_t result = usb_enumerate(&enumeration);
  auto_enum.attempts++;
  auto_enum.last_result = (uint32_t)result;

  if (result == USB_ENUM_OK)
    auto_enum.successes++;
  else
    auto_enum.failures++;
}

void usb_service(void) {
  if (!usb_initialised || usb_service_failed)
    return;

  // Dispatch hardware events to their owners.
  xhci_service_result_t result = xhci_service(8U);
  if (result == XHCI_SERVICE_FAILED || result == XHCI_SERVICE_NOT_READY) {
    usb_lifecycle_fail(USB_LIFECYCLE_FAULT_XHCI_SERVICE);
    usb_service_failed = true;
    return;
  }

  // Consume root-port notifications and acknowledge
  // their change flags before servicing endpoints.
  usb_service_port_events();
  if (usb_service_failed) {
    usb_lifecycle_fail(USB_LIFECYCLE_FAULT_ROOT_PORT);
    return;
  }

  // Finalise any internal hub interrupt notification.
  xhci_hub_notify_service();

  // Record and acknowledge hub changes, then
  // automatically rearm the notification endpoint.
  usb_service_hub_changes();
  if (change_stats.failed) {
    usb_lifecycle_fail(USB_LIFECYCLE_FAULT_HUB);
    return;
  }

  // Finalise and rearm completed Bulk IN transfers.
  xhci_bulk_in_stream_service();

  // Service the diagnostic MIDI receiver, if active.
  usb_midi_in_stress_service();

  // A disconnected, successfully enumerated device
  // is automatically admitted to lifecycle teardown.
  usb_lifecycle_auto_arm();

  // Perform at most one lifecycle operation per
  // foreground service pass, once armed.
  usb_lifecycle_service();

  // Enumerate only when a connection or a completed
  // teardown has requested a fresh admission check.
  usb_auto_enum_service();
}

bool usb_lifecycle_port_inspect(usb_lifecycle_port_info_t *info) {
  if (!info)
    return false;

  *info = (usb_lifecycle_port_info_t){0};

  usb_device_t device = {0};
  if (!usb_get_device(&device))
    return false;

  uint8_t port = device.parent_port;
  if (port == 0U || port >= 16U)
    return false;

  usb_hub_port_t current = {0};
  if (usb_hub_port_status(port, &current) != USB_HUB_OK)
    return false;

  info->generation = device.generation;
  info->port = port;
  info->current_status = current.status;
  info->current_change = current.change;

  // Hub port status bit 0 is CurrentConnectStatus.
  // Bit 8 is PortPower.
  info->connected = (current.status & 0x0001U) != 0U;
  info->powered = (current.status & 0x0100U) != 0U;

  const usb_change_event_t *last = &last_port_change[port];

  if (last->sequence != 0U) {
    info->notification_seen = true;
    info->last_sequence = last->sequence;
    info->last_status = last->status;
    info->last_change = last->change;
  }

  return true;
}

bool usb_reenum_inspect(usb_reenum_info_t *info) {
  if (!info)
    return false;

  *info = (usb_reenum_info_t){0};

  if (!usb_service_available()) {
    info->result = USB_REENUM_SERVICE_FAILED;
    return true;
  }

  if (change_stats.failed) {
    info->result = USB_REENUM_HUB_FAILED;
    return true;
  }

  usb_device_t device = {0};
  if (!usb_get_device(&device)) {
    info->result = USB_REENUM_NO_HISTORY;
    return true;
  }

  info->generation = device.generation;
  info->port = device.parent_port;
  info->device_state = device.state;

  if (device.state != USB_DEVICE_DETACHED) {
    info->result = USB_REENUM_DEVICE_OWNED;
    return true;
  }

  if (lifecycle_run.generation == device.generation) {
    if (lifecycle_run.state == USB_LIFECYCLE_RUN_ARMED) {
      info->result = USB_REENUM_EXECUTOR_ACTIVE;
      return true;
    }

    if (lifecycle_run.state == USB_LIFECYCLE_RUN_FAILED) {
      info->result = USB_REENUM_EXECUTOR_FAILED;
      return true;
    }
  }

  usb_enum_info_t enumeration = {0};
  info->enumeration_attempted = usb_get_enumeration(&enumeration);

  if (info->enumeration_attempted) {
    info->result = USB_REENUM_NOT_REARMED;
    return true;
  }

  if (xhci_downstream_is_quiesced() || xhci_downstream_is_slot_disabled() ||
      xhci_downstream_is_cleaned()) {
    info->result = USB_REENUM_XHCI_BUSY;
    return true;
  }

  usb_lifecycle_port_info_t port = {0};
  if (!usb_lifecycle_port_inspect(&port)) {
    info->result = USB_REENUM_PORT_UNAVAILABLE;
    return true;
  }

  info->port_status = port.current_status;
  info->port_change = port.current_change;

  if (!port.powered) {
    info->result = USB_REENUM_NOT_POWERED;
    return true;
  }

  if (!port.connected) {
    info->result = USB_REENUM_DISCONNECTED;
    return true;
  }

  // A pending hub change must be processed before
  // another device generation can be enumerated.
  uint16_t bit = (uint16_t)(1U << info->port);
  if (port.current_change != 0U || (change_pending_mask & bit) != 0U) {
    info->result = USB_REENUM_CHANGE_PENDING;
    return true;
  }

  info->result = USB_REENUM_READY;
  return true;
}

bool usb_enum_admission_inspect(usb_enum_admission_info_t *info) {
  if (!info)
    return false;

  *info = (usb_enum_admission_info_t){0};

  if (!usb_service_available()) {
    info->result = USB_ENUM_ADMIT_SERVICE_FAILED;
    return true;
  }

  if (change_stats.failed) {
    info->result = USB_ENUM_ADMIT_HUB_FAILED;
    return true;
  }

  usb_hub_init_info_t hub = {0};
  if (!usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK) {
    info->result = USB_ENUM_ADMIT_HUB_NOT_READY;
    return true;
  }

  usb_enum_info_t enumeration = {0};
  if (usb_get_enumeration(&enumeration)) {
    info->result = USB_ENUM_ADMIT_ALREADY_ATTEMPTED;
    return true;
  }

  usb_device_t device = {0};
  if (usb_get_device(&device)) {
    info->generation = device.generation;

    usb_reenum_info_t reconnect = {0};
    if (!usb_reenum_inspect(&reconnect)) {
      info->result = USB_ENUM_ADMIT_RECONNECT_BLOCKED;
      return true;
    }

    info->port = reconnect.port;
    info->reconnect_reason = reconnect.result;
    info->result = reconnect.result == USB_REENUM_READY ?
        USB_ENUM_ADMIT_RECONNECT : USB_ENUM_ADMIT_RECONNECT_BLOCKED;

    return true;
  }

  // Initial enumeration has no previous device
  // generation or downstream slot to reclaim.
  if (xhci_downstream_is_quiesced() || xhci_downstream_is_slot_disabled() ||
      xhci_downstream_is_cleaned()) {
    info->result = USB_ENUM_ADMIT_XHCI_BUSY;
    return true;
  }

  for (uint8_t port = 1U; port <= hub.ports; port++) {
    usb_hub_port_t status = {0};

    if (usb_hub_port_status(port, &status) != USB_HUB_OK) {
      info->result = USB_ENUM_ADMIT_PORT_FAILED;
      return true;
    }

    if (!(status.status & 0x0001U))
      continue;

    info->connected_count++;
    info->port = port;

    if (!(status.status & 0x0100U)) {
      info->result = USB_ENUM_ADMIT_PORT_FAILED;
      return true;
    }

    uint16_t bit = (uint16_t)(1U << port);
    if (status.change != 0U || (change_pending_mask & bit) != 0U) {
      info->result = USB_ENUM_ADMIT_CHANGE_PENDING;
      return true;
    }
  }

  if (info->connected_count == 0U)
    info->result = USB_ENUM_ADMIT_NO_DEVICE;
  else if (info->connected_count != 1U)
    info->result = USB_ENUM_ADMIT_MULTIPLE_DEVICES;
  else
    info->result = USB_ENUM_ADMIT_INITIAL;

  return true;
}

