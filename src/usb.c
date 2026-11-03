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

  change_count++;
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

void usb_service(void) {
  if (!usb_initialised || usb_service_failed)
    return;

  // Dispatch hardware events to their owners.
  xhci_service_result_t result = xhci_service(8U);
  if (result == XHCI_SERVICE_FAILED || result == XHCI_SERVICE_NOT_READY) {
    usb_service_failed = true;
    return;
  }

  // Consume root-port notifications and acknowledge
  // their change flags before servicing endpoints.
  usb_service_port_events();

  if (usb_service_failed)
    return;

  // Finalise any internal hub interrupt notification.
  xhci_hub_notify_service();

  // Record and acknowledge hub changes, then
  // automatically rearm the notification endpoint.
  usb_service_hub_changes();

  // Finalise and rearm completed Bulk IN transfers.
  xhci_bulk_in_stream_service();

  // Service the diagnostic MIDI receiver, if active.
  usb_midi_in_stress_service();
}

