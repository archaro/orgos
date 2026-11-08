#include <stddef.h>
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
#define USB_AUTO_ENUM_RECHECK_US 50000ULL

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

// Lifecycle storage for the four downstream
// ports of the internal USB 2.0 hub.
//
// Capacity does not imply permission to run
// multiple lifecycle executors yet.
#define USB_LIFECYCLE_RUN_CAPACITY 4U
static usb_lifecycle_run_info_t lifecycle_runs[USB_LIFECYCLE_RUN_CAPACITY];

// Index from which the next foreground lifecycle
// scheduling pass begins.
//
// Only one lifecycle operation may be dispatched
// per pass, even when several runs are armed.
static unsigned int lifecycle_next_index;

// Most recently armed lifecycle executor.
// Retained for the existing single-run diagnostic
// interface; it does not determine scheduling
// or ownership.
static usb_lifecycle_run_info_t *lifecycle_last_run = &lifecycle_runs[0];

static uint64_t auto_enum_next_check_us;
static usb_auto_enum_info_t auto_enum = {
  .enabled = true,
  .pending = true
};

// Future multi-port scheduling state.
//
// This bookkeeping does not authorise enumeration.
// The existing controller-wide admission path
// remains the only operational scheduler.
static uint16_t auto_enum_port_pending;
static uint8_t auto_enum_next_port = 1U;

// A completed notification may identify several ports.
// Retain progress until all have been processed.
static uint16_t change_pending_mask;
static uint8_t change_next_port = 1U;

static usb_lifecycle_run_info_t *usb_lifecycle_by_generation(
    uint32_t generation) {

  if (generation == 0U)
    return NULL;

  usb_lifecycle_run_info_t *found = NULL;
  for (unsigned int i = 0U; i < USB_LIFECYCLE_RUN_CAPACITY; i++) {
    usb_lifecycle_run_info_t *run = &lifecycle_runs[i];
    if (run->state == USB_LIFECYCLE_RUN_DISARMED ||
        run->generation != generation)
      continue;

    // A device incarnation must never have two
    // lifecycle executors claiming ownership.
    if (found)
      return NULL;

    found = run;
  }

  return found;
}

static bool usb_lifecycle_any_armed(void) {
  for (unsigned int i = 0U; i < USB_LIFECYCLE_RUN_CAPACITY; i++) {
    if (lifecycle_runs[i].state == USB_LIFECYCLE_RUN_ARMED)
      return true;
  }

  return false;
}

static bool usb_lifecycle_any_failed(void) {
  for (unsigned int i = 0U; i < USB_LIFECYCLE_RUN_CAPACITY; i++) {
    if (lifecycle_runs[i].state == USB_LIFECYCLE_RUN_FAILED)
      return true;
  }

  return false;
}

static usb_lifecycle_run_info_t *usb_lifecycle_select_for_arm(
    uint32_t generation) {

  if (generation == 0U || usb_lifecycle_any_armed() ||
      usb_lifecycle_any_failed())
    return NULL;

  // A generation may never acquire a second
  // lifecycle executor, even if its previous
  // executor has already completed.
  for (unsigned int i = 0U; i < USB_LIFECYCLE_RUN_CAPACITY; i++) {
    usb_lifecycle_run_info_t *run = &lifecycle_runs[i];
    if (run->state != USB_LIFECYCLE_RUN_DISARMED &&
        run->generation == generation)
      return NULL;
  }

  usb_lifecycle_run_info_t *oldest_complete = NULL;

  for (unsigned int i = 0U; i < USB_LIFECYCLE_RUN_CAPACITY; i++) {
    usb_lifecycle_run_info_t *run = &lifecycle_runs[i];

    // Prefer unused records so completed
    // lifecycle history remains available
    // for as long as possible.
    if (run->state == USB_LIFECYCLE_RUN_DISARMED)
      return run;

    // COMPLETE means the previous incarnation
    // passed through successful recycling and
    // enumeration rearm.
    if (run->state == USB_LIFECYCLE_RUN_COMPLETE &&
        (!oldest_complete || run->generation < oldest_complete->generation))
      oldest_complete = run;
  }

  // Reuse only the oldest completed record.
  // Failed records are never candidates.
  return oldest_complete;
}

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

bool usb_auto_enum_ports_get_info(usb_auto_enum_ports_info_t *info) {
  if (!info)
    return false;

  *info = (usb_auto_enum_ports_info_t){
    .pending_ports = auto_enum_port_pending,
    .next_port = auto_enum_next_port
  };

  return true;
}

bool usb_auto_enum_ports_seed(uint8_t ports, uint16_t connected) {
  if (!usb_service_available() || ports == 0U || ports > 4U)
    return false;

  uint16_t valid = (uint16_t)((1U << ports) - 1U);
  if ((connected & (uint16_t)~valid) != 0U)
    return false;

  // The hub scan numbers ports from bit zero.
  // Our scheduler reserves bit zero and uses
  // the physical port number as its bit index.
  for (uint8_t port = 1U; port <= ports; port++) {
    if (connected & (1U << (port - 1U)))
      auto_enum_port_pending |= (uint16_t)(1U << port);
  }

  // Merge rather than replace: an earlier request
  // must never be discarded by initial discovery.
  return true;
}

void usb_auto_enum_attempt_recorded(uint8_t port) {
  if (port == 0U || port > 4U)
    return;

  // The attempted connection no longer has a
  // pending request. Success and failure both
  // consume it; recovery requires an explicit
  // lifecycle transition.
  auto_enum_port_pending &= (uint16_t)~(1U << port);

  // Start future round-robin selection at the
  // port following this attempted connection.
  auto_enum_next_port = port == 4U ?  1U : (uint8_t)(port + 1U);
}

static void usb_auto_enum_request(void) {
  auto_enum.pending = true;
  auto_enum_next_check_us = 0ULL;
}

void usb_auto_enum_set_enabled(bool enabled) {
  auto_enum.enabled = enabled;

  // Enabling requests a fresh admission check.
  // It never overrides existing enumeration
  // or lifecycle ownership restrictions.
  if (enabled)
    usb_auto_enum_request();
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

static void usb_change_enqueue(uint8_t port, const usb_hub_port_t *status) {
  // The diagnostic queue must never prevent
  // hardware notifications from being processed.
  // Discard the oldest record when full.
  if (change_count == USB_CHANGE_QUEUE_CAPACITY) {
    change_head = (change_head + 1U) % USB_CHANGE_QUEUE_CAPACITY;
    change_count--;
    change_stats.dropped++;
  }

  unsigned int tail = (change_head + change_count) %
      USB_CHANGE_QUEUE_CAPACITY;

  change_stats.recorded++;
  change_queue[tail] = (usb_change_event_t){
    .sequence = change_stats.recorded,
    .port = port,
    .status = status->status,
    .change = status->change
  };

  // Retain the latest notification independently
  // of the consumable diagnostic queue.
  if (port < 16U)
    last_port_change[port] = change_queue[tail];

  change_count++;

  // Retain connection requests by physical port.
  // A disconnection cancels an unprocessed request.
  //
  // The compatibility scheduler still uses its
  // existing controller-wide pending flag.
  if (status->change & 0x0001U) {
    if (port >= 1U && port <= 4U) {
      uint16_t bit = (uint16_t)(1U << port);
      if (status->status & 0x0001U)
        auto_enum_port_pending |= bit;
      else
        auto_enum_port_pending &= (uint16_t)~bit;
    }

    usb_auto_enum_request();
  }

  // Lifecycle notification is operational state,
  // not a diagnostic queue consumer.
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
      // A confirmed disconnection can release a
      // failed pre-slot enumeration attempt.
      // usb_change_enqueue() has already requested
      // a new automatic enumeration admission check.
      if ((status.change & 0x0001U) != 0U && (status.status & 0x0001U) == 0U)
        (void)usb_enumeration_rearm_rejected(port);
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

static bool usb_lifecycle_inspect_record(const usb_device_t *device,
    const usb_lifecycle_run_info_t *run, bool enumeration_attempted,
    usb_lifecycle_info_t *info) {

  if (!device || !info)
    return false;

  *info = (usb_lifecycle_info_t){0};
  info->generation = device->generation;
  info->slot_id = device->slot_id;

  // Detachment clears the USB device's slot ID,
  // but the lifecycle executor retains the
  // original hardware identity for recycling.
  uint8_t inspection_slot = device->slot_id;
  if (device->state == USB_DEVICE_DETACHED &&
      run && run->state == USB_LIFECYCLE_RUN_ARMED &&
      run->generation == device->generation && run->slot_id != 0U)
    inspection_slot = run->slot_id;

  // Inspect the original xHCI slot even after
  // the public USB slot identity is cleared.
  info->quiesced = xhci_downstream_is_quiesced(inspection_slot);
  info->slot_disabled = xhci_downstream_is_slot_disabled(inspection_slot);
  info->cleaned = xhci_downstream_is_cleaned(inspection_slot);

  if (device->state == USB_DEVICE_ACTIVE)
    return true;

  if (device->state == USB_DEVICE_DETACHED) {
    xhci_connection_ownership_t ownership = XHCI_CONNECTION_UNCERTAIN;
    // Determine whether the selected physical
    // connection still owns an xHCI context.
    if (!xhci_downstream_connection_ownership( device->root_port,
        device->parent_slot, device->parent_port, &ownership)) {
      info->next = USB_LIFECYCLE_BLOCKED;
      return true;
    }

    // Ambiguous ownership must never authorise
    // recycling or rearm.
    if (ownership == XHCI_CONNECTION_UNCERTAIN) {
      info->next = USB_LIFECYCLE_BLOCKED;
      return true;
    }

    if (ownership == XHCI_CONNECTION_OWNED) {
      // The context remains allocated.
      // Only its original executor may recycle it,
      // after the complete teardown sequence.
      if (run && run->state == USB_LIFECYCLE_RUN_ARMED &&
          run->generation == device->generation && run->slot_id != 0U &&
          device->slot_id == 0U && info->quiesced &&
          info->slot_disabled && info->cleaned) {
        info->next = USB_LIFECYCLE_RECYCLE;
      } else {
        info->next = USB_LIFECYCLE_BLOCKED;
      }

      return true;
    }

    // The selected connection is now unowned.
    // An unrelated device may still own another
    // downstream controller context.
    if (!enumeration_attempted)
      return true;

    // An unowned connection alone is insufficient.
    // The original executor must have successfully
    // recycled its retained hardware slot.
    if (!run || run->state != USB_LIFECYCLE_RUN_ARMED ||
        run->generation != device->generation || run->slot_id == 0U ||
        device->slot_id != 0U || run->last_action != USB_LIFECYCLE_RECYCLE ||
        !run->last_succeeded ||
        run->last_result != (uint32_t)XHCI_RECYCLE_OK) {
      info->next = USB_LIFECYCLE_BLOCKED;
      return true;
    }

    info->next = USB_LIFECYCLE_REARM;
    return true;
  }

  if (device->state != USB_DEVICE_DISCONNECTING ||
      !info->quiesced || !usb_service_available()) {
    info->next = USB_LIFECYCLE_BLOCKED;
    return true;
  }

  if (info->cleaned) {
    info->next = USB_LIFECYCLE_DETACH;
    return true;
  }

  xhci_downstream_transfer_state_t transfers = {0};
  if (!xhci_downstream_transfer_state(device->slot_id, &transfers)) {
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

bool usb_lifecycle_inspect(usb_lifecycle_info_t *info) {
  if (!info)
    return false;

  *info = (usb_lifecycle_info_t){0};
  usb_device_t device = {0};
  if (!usb_get_device(&device))
    return true;

  usb_enum_info_t enumeration = {0};
  bool attempted = usb_get_enumeration(&enumeration);
  usb_lifecycle_run_info_t *run =
      usb_lifecycle_by_generation(device.generation);

  return usb_lifecycle_inspect_record(&device, run, attempted, info);
}

bool usb_lifecycle_inspect_generation(
    uint32_t generation, usb_lifecycle_info_t *info) {

  if (!info)
    return false;

  *info = (usb_lifecycle_info_t){0};
  usb_device_t device = {0};
  // Unknown or ambiguous generations are not
  // permission to select another device.
  if (!usb_get_device_by_generation(generation, &device))
    return false;

  usb_enum_info_t enumeration = {0};
  bool attempted = usb_get_enumeration_generation( generation, &enumeration);

  usb_lifecycle_run_info_t *run = usb_lifecycle_by_generation(generation);

  return usb_lifecycle_inspect_record( &device, run, attempted, info);
}

bool usb_lifecycle_get_run_info(usb_lifecycle_run_info_t *info) {
  if (!info)
    return false;

  *info = *lifecycle_last_run;
  return true;
}

bool usb_lifecycle_get_run_info_generation(
    uint32_t generation, usb_lifecycle_run_info_t *info) {

  if (!info)
    return false;

  *info = (usb_lifecycle_run_info_t){0};
  usb_lifecycle_run_info_t *run = usb_lifecycle_by_generation(generation);
  if (!run)
    return false;

  *info = *run;
  return true;
}

bool usb_lifecycle_arm_generation(uint32_t generation) {
  if (!usb_service_available() || change_stats.failed || generation == 0U)
    return false;

  usb_device_t device = {0};
  // The requested generation must identify
  // exactly one managed USB device.
  if (!usb_get_device_by_generation(generation, &device))
    return false;

  if (device.slot_id == 0U || (device.state != USB_DEVICE_ACTIVE &&
       device.state != USB_DEVICE_DISCONNECTING))
    return false;

  // Confirm that the hardware slot is uniquely
  // associated with the selected incarnation.
  usb_device_t slot_owner = {0};
  if (!usb_get_device_by_slot(device.slot_id, &slot_owner) ||
      slot_owner.generation != generation ||
      slot_owner.root_port != device.root_port ||
      slot_owner.parent_slot != device.parent_slot ||
      slot_owner.parent_port != device.parent_port)
    return false;

  // The physical connection must also resolve
  // uniquely to the same device incarnation.
  usb_device_t connection_owner = {0};
  if (!usb_get_device_by_connection(device.root_port,
      device.parent_slot, device.parent_port, &connection_owner))
    return false;

  if (connection_owner.generation != generation ||
      connection_owner.slot_id != device.slot_id)
    return false;

  usb_lifecycle_run_info_t *run = usb_lifecycle_select_for_arm(generation);
  if (!run)
    return false;

  // The selected lifecycle record now owns
  // this specific device incarnation.
  *run = (usb_lifecycle_run_info_t){
    .state = USB_LIFECYCLE_RUN_ARMED,
    .generation = generation,
    .observed_generation = generation,
    .slot_id = device.slot_id
  };

  lifecycle_last_run = run;

  return true;
}

bool usb_lifecycle_arm(void) {
  usb_device_t device = {0};
  if (!usb_get_device(&device))
    return false;

  // Compatibility interface: select the current
  // device, then use the generation-specific path.
  return usb_lifecycle_arm_generation(device.generation);
}

static void usb_lifecycle_auto_arm(void) {
  // Only one executor may be armed at a time
  // until concurrent lifecycle admission is
  // explicitly enabled.
  if (usb_lifecycle_any_armed() || usb_lifecycle_any_failed())
    return;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_device_t device = {0};
    if (!usb_get_device_at(i, &device))
      continue;

    // Pre-generation enumeration failures
    // cannot be handled by this lifecycle path.
    if (device.generation == 0U || device.state != USB_DEVICE_DISCONNECTING ||
        device.slot_id == 0U)
      continue;

    // Automatic teardown requires an explicitly
    // quiesced hardware slot.
    if (!xhci_downstream_is_quiesced(device.slot_id))
      continue;

    // Only a successfully enumerated incarnation
    // may enter automatic lifecycle teardown.
    //
    // Failed enumeration continues to use the
    // separately controlled recovery path.
    usb_enum_info_t enumeration = {0};
    if (!usb_get_enumeration_generation(device.generation, &enumeration))
      continue;

    if (enumeration.result != USB_ENUM_OK ||
        enumeration.slot_id != device.slot_id)
      continue;

    // Never create a second executor for the
    // same device incarnation.
    if (usb_lifecycle_by_generation(device.generation))
      continue;

    // Arming performs the final generation,
    // slot, connection and allocation checks.
    if (usb_lifecycle_arm_generation(device.generation))
      return;
  }
}

static void usb_lifecycle_fail_run(
    usb_lifecycle_run_info_t *run, usb_lifecycle_fault_t fault) {

  if (!run || run->state != USB_LIFECYCLE_RUN_ARMED)
    return;

  run->fault = fault;
  run->state = USB_LIFECYCLE_RUN_FAILED;
  run->last_succeeded = false;
}

static void usb_lifecycle_fail(usb_lifecycle_fault_t fault) {
  // Controller-wide failures invalidate every
  // currently armed lifecycle executor.
  //
  // Preserve completed and previously failed
  // records without modifying their history.
  for (unsigned int i = 0U; i < USB_LIFECYCLE_RUN_CAPACITY; i++) {
    usb_lifecycle_fail_run(&lifecycle_runs[i], fault);
  }
}

static void usb_lifecycle_service_run(usb_lifecycle_run_info_t *run) {
  if (!run || run->state != USB_LIFECYCLE_RUN_ARMED)
    return;

  // The selected executor must be the unique
  // owner of its device incarnation.
  if (run->generation == 0U || run->slot_id == 0U ||
      usb_lifecycle_by_generation(run->generation) != run) {
    usb_lifecycle_fail_run(run, USB_LIFECYCLE_FAULT_GENERATION);
    return;
  }

  usb_lifecycle_info_t info = {0};
  if (!usb_lifecycle_inspect_generation(run->generation, &info)) {
    usb_lifecycle_fail_run(run, USB_LIFECYCLE_FAULT_INSPECTION);
    return;
  }

  run->observed_generation = info.generation;
  if (info.generation != run->generation) {
    usb_lifecycle_fail_run(run, USB_LIFECYCLE_FAULT_GENERATION);
    return;
  }

  // All slot-specific teardown operations must
  // target the slot originally owned by this
  // lifecycle executor.
  if ((info.next == USB_LIFECYCLE_RECONCILE_IN ||
       info.next == USB_LIFECYCLE_STOP_IN ||
       info.next == USB_LIFECYCLE_DISABLE ||
       info.next == USB_LIFECYCLE_RETIRE_IN ||
       info.next == USB_LIFECYCLE_CLEANUP) &&
      (info.slot_id == 0U ||
       info.slot_id != run->slot_id)) {
    usb_lifecycle_fail_run(run, USB_LIFECYCLE_FAULT_BLOCKED);
    return;
  }

  // Recycling occurs after USB detachment has
  // cleared the public device record's slot ID.
  //
  // The lifecycle executor must still retain
  // the original hardware slot identity.
  if (info.next == USB_LIFECYCLE_RECYCLE && (info.slot_id != 0U ||
       run->slot_id == 0U || !info.quiesced ||
       !info.slot_disabled || !info.cleaned)) {
    usb_lifecycle_fail_run(run, USB_LIFECYCLE_FAULT_BLOCKED);
    return;
  }

  // No teardown is needed while the device
  // remains active.
  if (info.next == USB_LIFECYCLE_IDLE)
    return;

  run->last_action = info.next;
  run->last_succeeded = false;
  run->last_result = 0U;
  run->command_result = 0U;
  run->completion_code = 0U;
  bool succeeded = false;

  switch (info.next) {
    case USB_LIFECYCLE_RECONCILE_IN: {
      xhci_in_reconcile_result_t result =
          xhci_bulk_in_quiesced_reconcile(run->slot_id);
      run->last_result = (uint32_t)result;
      succeeded = result == XHCI_IN_RECONCILE_OK ||
          result == XHCI_IN_RECONCILE_RESOLVED;
      break;
    }
    case USB_LIFECYCLE_STOP_IN: {
      xhci_stop_info_t result = {0};
      xhci_stop_result_t status =
          xhci_downstream_stop_bulk_in(run->slot_id, &result);
      run->last_result = (uint32_t)status;
      run->command_result = (uint8_t)result.command_result;
      run->completion_code = result.completion_code;
      succeeded = status == XHCI_STOP_OK;
      break;
    }
    case USB_LIFECYCLE_DISABLE: {
      xhci_disable_info_t result = {0};
      xhci_disable_result_t status =
          xhci_downstream_disable(run->slot_id, &result);
      run->last_result = (uint32_t)status;
      run->command_result = (uint8_t)result.command_result;
      run->completion_code = result.completion_code;
      succeeded = status == XHCI_DISABLE_OK;
      break;
    }
    case USB_LIFECYCLE_RETIRE_IN: {
      xhci_bulk_in_retire_result_t status =
          xhci_bulk_in_stream_retire(run->slot_id);
      run->last_result = (uint32_t)status;
      succeeded = status == XHCI_BULK_IN_RETIRE_OK;
      break;
    }
    case USB_LIFECYCLE_CLEANUP: {
      xhci_cleanup_info_t result = {0};
      xhci_cleanup_result_t status =
          xhci_downstream_cleanup(run->slot_id, &result);
      run->last_result = (uint32_t)status;
      succeeded = status == XHCI_CLEANUP_OK;
      break;
    }
    case USB_LIFECYCLE_DETACH: {
      succeeded = usb_device_finalize_disconnect_generation(
          run->generation, run->slot_id);
      run->last_result = succeeded ? 0U : 1U;
      break;
    }
    case USB_LIFECYCLE_RECYCLE: {
      xhci_recycle_result_t status =
          xhci_downstream_recycle(run->slot_id);
      run->last_result = (uint32_t)status;
      succeeded = status == XHCI_RECYCLE_OK;
      break;
    }
    case USB_LIFECYCLE_REARM: {
      succeeded = usb_enumeration_rearm_generation(run->generation);
      run->last_result = succeeded ? 0U : 1U;
      break;
    }
    default: {
      // A blocked or unsupported state is not
      // permission to guess the next operation.
      usb_lifecycle_fail_run(run, USB_LIFECYCLE_FAULT_BLOCKED);
      return;
    }
  }

  run->steps++;
  run->last_succeeded = succeeded;
  if (!succeeded) {
    usb_lifecycle_fail_run(run, USB_LIFECYCLE_FAULT_OPERATION);
    return;
  }

  if (info.next == USB_LIFECYCLE_REARM) {
    run->state = USB_LIFECYCLE_RUN_COMPLETE;

    // The previous generation no longer owns
    // the downstream controller resources.
    // Check for a device already reconnected.
    usb_auto_enum_request();
  }
}

static void usb_lifecycle_service(void) {
  // Visit armed executors in round-robin order.
  //
  // The scheduler dispatches at most one run
  // per foreground service pass. xHCI lifecycle
  // commands therefore remain serialised.
  for (unsigned int scanned = 0U;
      scanned < USB_LIFECYCLE_RUN_CAPACITY; scanned++) {

    unsigned int index =
        (lifecycle_next_index + scanned) % USB_LIFECYCLE_RUN_CAPACITY;
    usb_lifecycle_run_info_t *run = &lifecycle_runs[index];
    if (run->state != USB_LIFECYCLE_RUN_ARMED)
      continue;

    // Start the next scheduling pass after the
    // selected executor, even if this operation
    // completes or fails.
    lifecycle_next_index = (index + 1U) % USB_LIFECYCLE_RUN_CAPACITY;

    usb_lifecycle_service_run(run);
    return;
  }
}

static void usb_auto_enum_service(void) {
  if (!auto_enum.enabled || !auto_enum.pending || usb_lifecycle_any_armed())
    return;

  // Deferred hub-change inspections are rate-limited.
  // A new notification or completed lifecycle rearm
  // can clear this delay and request immediate work.
  if (auto_enum_next_check_us != 0ULL &&
      timer_now_us() < auto_enum_next_check_us)
    return;

  usb_enum_admission_info_t admission = {0};

  if (!usb_enum_admission_inspect(&admission)) {
    auto_enum.pending = false;
    auto_enum_next_check_us = 0ULL;
    return;
  }

  if (admission.result != USB_ENUM_ADMIT_INITIAL &&
      admission.result != USB_ENUM_ADMIT_RECONNECT) {

    // Pending hub changes can settle without a new
    // device-generation transition. Keep the request
    // but avoid repeatedly polling the hub at full
    // foreground-loop speed.
    if (admission.result == USB_ENUM_ADMIT_CHANGE_PENDING ||
        (admission.result == USB_ENUM_ADMIT_RECONNECT_BLOCKED &&
         admission.reconnect_reason == USB_REENUM_CHANGE_PENDING)) {
      auto_enum_next_check_us = timer_now_us() + USB_AUTO_ENUM_RECHECK_US;
      return;
    }

    // Other refusals consume this request. A new
    // connection event or completed teardown can
    // request another check, but this code must not
    // retry a failed or resource-owning generation.
    auto_enum.pending = false;
    auto_enum_next_check_us = 0ULL;
    return;
  }

  // Consume the request before enumeration.
  // Failed enumeration must never be retried
  // automatically against the same generation.
  auto_enum.pending = false;
  auto_enum_next_check_us = 0ULL;
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

typedef struct {
  uint8_t connected_count;
  uint8_t port;
  uint16_t status;
  uint16_t change;
  uint8_t unmanaged_root_count;
  bool change_pending;
} usb_topology_inventory_t;

// Inspect controller root ports which are not
// occupied by the internal USB 2.0 hub.
//
// Supported Protocol capabilities identify the
// root-port ranges. The controller has already
// supplied these through xhci_protocol_get().
static bool usb_unmanaged_root_inventory_read(
    usb_topology_inventory_t *inventory) {

  uint8_t total_ports = 0U;

  if (!xhci_port_count(&total_ports))
    return false;

  // Verify that every root port belongs to
  // exactly one Supported Protocol capability.
  bool covered[256] = {false};

  for (unsigned int index = 0U;; index++) {
    xhci_protocol_t protocol = {0};
    bool found = false;

    if (!xhci_protocol_get(index, &protocol, &found))
      return false;

    if (!found)
      break;

    unsigned int first = protocol.first_port;
    unsigned int count = protocol.port_count;

    // "USB " is the standard protocol name.
    // Reject malformed or unsupported mappings.
    if (protocol.name != 0x20425355U ||
        protocol.major < 2U ||
        first == 0U || count == 0U ||
        first + count - 1U > total_ports)
      return false;

    for (unsigned int number = first;
        number < first + count; number++) {

      if (covered[number])
        return false;

      covered[number] = true;

      // Root port 1 contains our internal hub.
      // Its downstream devices are counted
      // separately by the hub inventory.
      if (number == USB_ROOT_PORT) {
        if (protocol.major != 2U)
          return false;
        continue;
      }

      xhci_port_t root = {0};

      if (!xhci_port_read((uint8_t)number, &root))
        return false;

      // An unsettled root-port change must not
      // be mistaken for a stable topology.
      if (root.connect_change || root.enable_change ||
          root.warm_reset_change || root.overcurrent_change ||
          root.reset_change || root.link_change ||
          root.config_error_change)
        inventory->change_pending = true;

      // Count the connection even if it has not
      // acquired a slot or negotiated its final
      // operational link state.
      if (root.connected)
        inventory->unmanaged_root_count++;
    }
  }

  // A missing protocol mapping makes the
  // controller-wide inventory incomplete.
  for (unsigned int number = 1U;
      number <= total_ports; number++) {
    if (!covered[number])
      return false;
  }

  return true;
}

static bool usb_topology_inventory_read(uint8_t ports,
    usb_topology_inventory_t *inventory) {

  if (!inventory || ports == 0U || ports >= 16U)
    return false;

  *inventory = (usb_topology_inventory_t){0};
  inventory->change_pending = change_pending_mask != 0U;

  // Inspect devices connected to the internal hub.
  for (uint8_t port = 1U; port <= ports; port++) {
    usb_hub_port_t observed = {0};

    if (usb_hub_port_status(port, &observed) != USB_HUB_OK)
      return false;

    if (observed.change != 0U)
      inventory->change_pending = true;

    if (!(observed.status & 0x0001U))
      continue;

    inventory->connected_count++;
    if (inventory->connected_count == 1U) {
      inventory->port = port;
      inventory->status = observed.status;
      inventory->change = observed.change;
    }
  }

  // A hub candidate is meaningful only when
  // exactly one device occupies the hub.
  if (inventory->connected_count != 1U) {
    inventory->port = 0U;
    inventory->status = 0U;
    inventory->change = 0U;
  }

  // Complete the observation with the remaining
  // controller root ports.
  return usb_unmanaged_root_inventory_read(inventory);
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
  info->previous_port = device.parent_port;
  info->device_state = device.state;
  usb_enum_info_t enumeration = {0};
  info->enumeration_attempted = usb_get_enumeration(&enumeration);

  if (device.state != USB_DEVICE_DETACHED) {
    info->result = USB_REENUM_DEVICE_OWNED;
    return true;
  }

  // Reconnection must not proceed while a
  // lifecycle executor is still active.
  // This is controller-wide until independent
  // device admission is implemented.
  if (usb_lifecycle_any_armed()) {
    info->result = USB_REENUM_EXECUTOR_ACTIVE;
    return true;
  }

  // A failed executor may retain uncertain
  // hardware ownership, even if it belongs
  // to an earlier generation.
  if (usb_lifecycle_any_failed()) {
    info->result = USB_REENUM_EXECUTOR_FAILED;
    return true;
  }

  if (info->enumeration_attempted) {
    info->result = USB_REENUM_NOT_REARMED;
    return true;
  }

  if (xhci_downstream_any_owned()) {
    info->result = USB_REENUM_XHCI_BUSY;
    return true;
  }

  // The previous generation's hub port is history,
  // not a restriction on the next generation.
  usb_hub_init_info_t hub = {0};

  if (!usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK ||
      hub.ports == 0U || hub.ports >= 16U) {
    info->result = USB_REENUM_PORT_UNAVAILABLE;
    return true;
  }

  // Do not inspect a topology whose notifications
  // have not yet been completely processed.
  usb_topology_inventory_t inventory = {0};
  if (!usb_topology_inventory_read(hub.ports, &inventory)) {
    info->result = USB_REENUM_PORT_UNAVAILABLE;
    return true;
  }

  info->connected_count = inventory.connected_count;
  info->unmanaged_root_count = inventory.unmanaged_root_count;
  info->port = inventory.port;
  info->port_status = inventory.status;
  info->port_change = inventory.change;

  if (inventory.change_pending) {
    info->result = USB_REENUM_CHANGE_PENDING;
    return true;
  }

  if (inventory.connected_count == 0U) {
    info->result = inventory.unmanaged_root_count != 0U ?
        USB_REENUM_UNMANAGED_ROOT : USB_REENUM_DISCONNECTED;
    return true;
  }

  if (inventory.connected_count != 1U) {
    info->result = USB_REENUM_MULTIPLE_DEVICES;
    return true;
  }

  if (!(inventory.status & 0x0100U)) {
    info->result = USB_REENUM_NOT_POWERED;
    return true;
  }

  info->result = USB_REENUM_READY;
  return true;
}

// Root-port connections outside the internal
// USB 2.0 hub are visible to admission.
// TODO: They are not yet eligible for enumeration.
// Device ownership and lifecycle management must
// be generalised before admitting those devices.
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
    info->connected_count = reconnect.connected_count;
    info->unmanaged_root_count = reconnect.unmanaged_root_count;
    info->reconnect_reason = reconnect.result;
    info->result = reconnect.result == USB_REENUM_READY ?
        USB_ENUM_ADMIT_RECONNECT : USB_ENUM_ADMIT_RECONNECT_BLOCKED;
    return true;
  }

  // Initial enumeration has no previous device
  // generation or downstream slot to reclaim.
  if (xhci_downstream_any_owned()) {
    info->result = USB_ENUM_ADMIT_XHCI_BUSY;
    return true;
  }

  usb_topology_inventory_t inventory = {0};
  if (!usb_topology_inventory_read(hub.ports, &inventory)) {
    info->result = USB_ENUM_ADMIT_PORT_FAILED;
    return true;
  }

  info->connected_count = inventory.connected_count;
  info->unmanaged_root_count = inventory.unmanaged_root_count;
  info->port = inventory.port;

  if (inventory.change_pending) {
    info->result = USB_ENUM_ADMIT_CHANGE_PENDING;
    return true;
  }

  if (inventory.connected_count == 0U) {
    info->result = inventory.unmanaged_root_count != 0U ?
        USB_ENUM_ADMIT_UNMANAGED_ROOT : USB_ENUM_ADMIT_NO_DEVICE;
    return true;
  }

  if (inventory.connected_count != 1U) {
    info->result = USB_ENUM_ADMIT_MULTIPLE_DEVICES;
    return true;
  }

  if (!(inventory.status & 0x0100U)) {
    info->result = USB_ENUM_ADMIT_PORT_FAILED;
    return true;
  }

  info->result = USB_ENUM_ADMIT_INITIAL;
  return true;
}

bool usb_enum_port_inspect(uint8_t port, usb_enum_port_info_t *info) {
  if (!info)
    return false;

  *info = (usb_enum_port_info_t){
    .result = USB_ENUM_PORT_INVALID,
    .port = port
  };

  if (!usb_service_available()) {
    info->result = USB_ENUM_PORT_SERVICE_FAILED;
    return true;
  }

  if (change_stats.failed) {
    info->result = USB_ENUM_PORT_HUB_FAILED;
    return true;
  }

  usb_init_info_t root = {0};
  usb_hub_init_info_t hub = {0};

  if (!usb_get_init_info(&root) || root.result != USB_INIT_OK ||
      root.root_port == 0U || root.slot_id == 0U ||
      !usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK) {
    info->result = USB_ENUM_PORT_HUB_NOT_READY;
    return true;
  }

  // Current scope: the four downstream ports
  // of the internal USB 2.0 hub.
  if (port == 0U || port > hub.ports || port > 4U)
    return true;

  usb_hub_port_t current = {0};

  if (usb_hub_port_status(port, &current) != USB_HUB_OK) {
    info->result = USB_ENUM_PORT_STATUS_FAILED;
    return true;
  }

  info->status = current.status;
  info->change = current.change;

  // Never inspect a connection as stable while
  // its change notification is still outstanding.
  if (current.change != 0U ||
      (change_pending_mask & (uint16_t)(1U << port)) != 0U) {
    info->result = USB_ENUM_PORT_CHANGE_PENDING;
    return true;
  }

  if ((current.status & (1U << 3)) != 0U) {
    info->result = USB_ENUM_PORT_OVERCURRENT;
    return true;
  }

  if ((current.status & (1U << 4)) != 0U) {
    info->result = USB_ENUM_PORT_RESETTING;
    return true;
  }

  if ((current.status & 0x0001U) == 0U) {
    info->result = USB_ENUM_PORT_DISCONNECTED;
    return true;
  }

  if ((current.status & 0x0100U) == 0U) {
    info->result = USB_ENUM_PORT_NOT_POWERED;
    return true;
  }

  // Find every managed record claiming this
  // physical connection, including records
  // without an assigned generation or slot.
  usb_device_t matching = {0};
  bool found = false;

  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_device_t device = {0};
    if (!usb_get_device_at(i, &device))
      continue;

    if (device.root_port != root.root_port ||
        device.parent_slot != root.slot_id || device.parent_port != port)
      continue;

    // Duplicate physical ownership must never
    // produce an eligible admission result.
    if (found) {
      info->result = USB_ENUM_PORT_AMBIGUOUS;
      return true;
    }

    matching = device;
    found = true;
  }

  if (found) {
    info->generation = matching.generation;
    info->slot_id = matching.slot_id;

    usb_enum_info_t enumeration = {0};

    // A failed or completed attempt remains
    // latched until explicitly rearmed.
    if (usb_get_enumeration_by_connection(root.root_port, root.slot_id,
        port, &enumeration)) {
      info->result = USB_ENUM_PORT_ALREADY_ATTEMPTED;
      return true;
    }

    // A retained historical record is acceptable
    // only after detachment and rearm.
    if (matching.state != USB_DEVICE_DETACHED || matching.slot_id != 0U) {
      info->result = USB_ENUM_PORT_DEVICE_OWNED;
      return true;
    }

    if (matching.generation == 0U) {
      info->result = USB_ENUM_PORT_UNSAFE_HISTORY;
      return true;
    }
  }

  // Finally, confirm that no xHCI context still
  // owns the selected physical connection.
  xhci_connection_ownership_t ownership = XHCI_CONNECTION_UNCERTAIN;
  if (!xhci_downstream_connection_ownership(
      root.root_port, root.slot_id, port, &ownership)) {
    info->result = USB_ENUM_PORT_XHCI_UNCERTAIN;
    return true;
  }

  if (ownership == XHCI_CONNECTION_OWNED) {
    info->result = USB_ENUM_PORT_XHCI_OWNED;
    return true;
  }

  if (ownership != XHCI_CONNECTION_UNOWNED) {
    info->result = USB_ENUM_PORT_XHCI_UNCERTAIN;
    return true;
  }

  info->result = USB_ENUM_PORT_ELIGIBLE;
  return true;
}

bool usb_enum_candidate_inspect(
    uint8_t port, usb_enum_candidate_info_t *info) {

  if (!info)
    return false;

  *info = (usb_enum_candidate_info_t){0};

  // Establish the selected port's local state.
  if (!usb_enum_port_inspect(port, &info->port))
    return false;

  // Independently obtain the existing
  // controller-wide admission decision.
  if (!usb_enum_admission_inspect(&info->controller))
    return false;

  bool controller_ready = info->controller.result == USB_ENUM_ADMIT_INITIAL ||
      info->controller.result == USB_ENUM_ADMIT_RECONNECT;

  // Both decisions must concern the same port.
  // A READY decision for another port never
  // authorises this candidate.
  info->would_admit = info->port.result == USB_ENUM_PORT_ELIGIBLE &&
      controller_ready && info->controller.port == port;

  return true;
}

bool usb_auto_enum_select_inspect(usb_auto_enum_select_info_t *info) {
  if (!info)
    return false;

  *info = (usb_auto_enum_select_info_t){
    .pending_ports = auto_enum_port_pending,
    .next_port = auto_enum_next_port
  };

  usb_hub_init_info_t hub = {0};
  if (!usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK ||
      hub.ports == 0U || hub.ports > 4U)
    return false;

  if (info->next_port == 0U || info->next_port > hub.ports)
    return false;

  // Visit each hub port once, beginning at the
  // retained round-robin position.
  for (uint8_t i = 0U; i < hub.ports; i++) {
    uint8_t port = (uint8_t)(((info->next_port - 1U + i) % hub.ports) + 1U);

    uint16_t bit = (uint16_t)(1U << port);
    if (!(info->pending_ports & bit))
      continue;

    usb_enum_port_info_t candidate = {0};
    if (!usb_enum_port_inspect(port, &candidate))
      return false;

    if (candidate.result != USB_ENUM_PORT_ELIGIBLE)
      continue;

    info->eligible_ports |= bit;

    // Retain the first eligible port encountered.
    // Continue inspecting to obtain the complete
    // eligible bitmap.
    if (info->selected_port == 0U)
      info->selected_port = port;
  }

  // Controller-wide admission is independent of
  // local candidate selection and remains mandatory.
  if (!usb_enum_admission_inspect(&info->controller))
    return false;

  bool controller_ready = info->controller.result == USB_ENUM_ADMIT_INITIAL ||
      info->controller.result == USB_ENUM_ADMIT_RECONNECT;

  info->would_admit = info->selected_port != 0U &&
      controller_ready && info->controller.port == info->selected_port;

  return true;
}

