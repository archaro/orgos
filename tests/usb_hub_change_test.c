#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// White-box fixture: seed enumerated generations or run a pre-slot attempt
// without emulating USB descriptors. Exercise notification, ownership, lifecycle and
// admission code; section GC excludes unrelated hardware/MSI entry points.
#include "../src/usb.c"
#include "../src/usb_enumerate.c"

#define CONNECTED 0x0001U
#define ENABLED 0x0002U
#define POWERED 0x0100U
#define C_CONNECTION 0x0001U
#define C_ENABLE 0x0002U
#define C_RESET 0x0010U

static usb_hub_port_t hub_port;
static bool notify_pending;
static bool quiesced, disabled, cleaned;
static bool ep0_uncertain;
static xhci_connection_ownership_t ownership;
static xhci_disable_result_t disable_result;
static unsigned int quiesce_calls, disable_calls, recycle_calls;
static bool disappear_on_discovery;
static bool ownership_available = true;
static unsigned int enable_slot_calls;

bool xhci_downstream_quiesce(uint8_t slot) {
  assert(slot == 2U);
  quiesce_calls++;
  quiesced = true;
  return true;
}

bool xhci_downstream_is_quiesced(uint8_t slot) {
  return slot == 2U && quiesced;
}

bool xhci_downstream_is_slot_disabled(uint8_t slot) {
  return slot == 2U && disabled;
}

bool xhci_downstream_is_cleaned(uint8_t slot) {
  return slot == 2U && cleaned;
}

bool xhci_downstream_connection_ownership(uint8_t root, uint8_t parent,
    uint8_t port, xhci_connection_ownership_t *result) {
  assert(root == 1U && parent == 1U && port == 1U);
  *result = ownership;
  return ownership_available;
}

bool xhci_downstream_transfer_state(uint8_t slot,
    xhci_downstream_transfer_state_t *state) {
  assert(slot == 2U);
  *state = (xhci_downstream_transfer_state_t){
    .slot_id = slot, .quiesced = quiesced, .ep0_uncertain = ep0_uncertain
  };
  return true;
}

bool xhci_resource_get_info(xhci_resource_info_t *info) {
  *info = (xhci_resource_info_t){
    .controller_running = true, .downstream_free = 3U,
    .waiter_capacity = 16U, .command_waiter_reserve = 1U
  };
  return true;
}

xhci_disable_result_t xhci_downstream_disable(uint8_t slot,
    xhci_disable_info_t *info) {
  assert(slot == 2U && quiesced);
  disable_calls++;
  *info = (xhci_disable_info_t){0};
  if (disable_result == XHCI_DISABLE_OK)
    disabled = true;
  return disable_result;
}

xhci_cleanup_result_t xhci_downstream_cleanup(uint8_t slot,
    xhci_cleanup_info_t *info) {
  assert(slot == 2U && quiesced && disabled);
  *info = (xhci_cleanup_info_t){0};
  cleaned = true;
  return XHCI_CLEANUP_OK;
}

xhci_recycle_result_t xhci_downstream_recycle(uint8_t slot) {
  assert(slot == 2U && quiesced && disabled && cleaned);
  assert(managed_devices[0].device.state == USB_DEVICE_DETACHED);
  assert(managed_devices[0].device.slot_id == 0U);
  recycle_calls++;
  ownership = XHCI_CONNECTION_UNOWNED;
  return XHCI_RECYCLE_OK;
}

// This fixture has no submitted transfers. An uncertain transfer blocks
// retirement instead of speculatively entering any of these operations.
xhci_in_reconcile_result_t xhci_bulk_in_quiesced_reconcile(uint8_t slot) {
  (void)slot;
  assert(false);
  return XHCI_IN_RECONCILE_NOT_READY;
}

xhci_stop_result_t xhci_downstream_stop_bulk_out(uint8_t slot,
    xhci_stop_info_t *info) {
  (void)slot;
  (void)info;
  assert(false);
  return XHCI_STOP_NOT_READY;
}

xhci_stop_result_t xhci_downstream_stop_bulk_in(uint8_t slot,
    xhci_stop_info_t *info) {
  (void)slot;
  (void)info;
  assert(false);
  return XHCI_STOP_NOT_READY;
}

xhci_bulk_in_retire_result_t xhci_bulk_in_stream_retire(uint8_t slot) {
  (void)slot;
  assert(false);
  return XHCI_BULK_IN_RETIRE_NOT_READY;
}

bool usb_hub_get_init_info(usb_hub_init_info_t *info) {
  *info = (usb_hub_init_info_t){.result = USB_HUB_INIT_OK, .ports = 4U};
  return true;
}

usb_hub_result_t usb_hub_port_status(uint8_t port, usb_hub_port_t *info) {
  assert(port == 1U);
  if (disappear_on_discovery && managed_devices[0].valid &&
      managed_devices[0].attempted) {
    // Admission saw a connected port. Its first discovery read now sees
    // the removal, before debounce, reset or any Enable Slot command.
    hub_port.status = POWERED;
    hub_port.change = C_CONNECTION;
    disappear_on_discovery = false;
  }
  *info = hub_port;
  return USB_HUB_OK;
}

usb_hub_result_t usb_hub_port_acknowledge(uint8_t port, uint16_t changes) {
  assert(port == 1U && changes == hub_port.change);
  hub_port.change = 0U;
  return USB_HUB_OK;
}

bool xhci_hub_notify_get_info(xhci_hub_notify_info_t *info) {
  *info = (xhci_hub_notify_info_t){
    .state = notify_pending ? XHCI_HUB_NOTIFY_COMPLETE : XHCI_HUB_NOTIFY_PENDING,
    .changes = 1U << 1
  };
  return true;
}

xhci_hub_notify_result_t xhci_hub_notify_resume(void) {
  assert(notify_pending);
  notify_pending = false;
  return XHCI_HUB_NOTIFY_OK;
}

// None of these operations is permitted after discovery finds the port
// empty. Keep the real enumeration engine linked and fail on any attempt
// to advance to hardware allocation, addressing, EP0 or endpoint setup.
xhci_slot_result_t xhci_enable_downstream_slot(uint8_t port,
    xhci_slot_info_t *info) {
  (void)port;
  (void)info;
  enable_slot_calls++;
  assert(false);
  return XHCI_SLOT_NOT_READY;
}

#define UNUSED __attribute__((unused))
usb_hub_result_t usb_hub_port_debounce(uint8_t port UNUSED, usb_hub_port_t *info UNUSED) {
  assert(false);
  return USB_HUB_NOT_READY;
}

usb_hub_result_t usb_hub_port_reset(uint8_t port UNUSED, usb_hub_reset_info_t *info UNUSED) {
  assert(false);
  return USB_HUB_NOT_READY;
}

xhci_device_result_t xhci_downstream_prepare(uint8_t port UNUSED, uint8_t speed UNUSED,
    xhci_device_info_t *info UNUSED) {
  assert(false);
  return XHCI_DEVICE_NOT_READY;
}

xhci_address_result_t xhci_address_downstream(uint8_t slot UNUSED,
    xhci_address_info_t *info UNUSED) {
  assert(false);
  return XHCI_ADDRESS_NOT_READY;
}

xhci_control_result_t xhci_control_in(const xhci_setup_packet_t *setup UNUSED,
    xhci_control_info_t *info UNUSED) {
  assert(false);
  return XHCI_CONTROL_NOT_READY;
}

xhci_control_result_t xhci_downstream_control_in(uint8_t slot UNUSED,
    const xhci_setup_packet_t *setup UNUSED, xhci_control_info_t *info UNUSED) {
  assert(false);
  return XHCI_CONTROL_NOT_READY;
}

xhci_control_result_t xhci_downstream_control_no_data(uint8_t slot UNUSED,
    const xhci_setup_packet_t *setup UNUSED, xhci_control_info_t *info UNUSED) {
  assert(false);
  return XHCI_CONTROL_NOT_READY;
}

xhci_ep0_update_result_t xhci_downstream_ep0_update(uint8_t slot UNUSED,
    uint16_t mps UNUSED, xhci_ep0_update_info_t *info UNUSED) {
  assert(false);
  return XHCI_EP0_UPDATE_NOT_READY;
}

xhci_bulk_config_result_t xhci_downstream_bulk_configure(uint8_t slot UNUSED,
    uint8_t in UNUSED, uint16_t in_mps UNUSED, uint8_t out UNUSED,
    uint16_t out_mps UNUSED, xhci_bulk_config_info_t *info UNUSED) {
  assert(false);
  return XHCI_BULK_CONFIG_NOT_READY;
}

bool usb_midi_parse_config(const uint8_t *data UNUSED, unsigned int length UNUSED,
    usb_midi_config_t *info UNUSED) {
  assert(false);
  return false;
}

uint32_t usb_midi_quirks_lookup(uint16_t vendor UNUSED, uint16_t product UNUSED) {
  assert(false);
  return 0U;
}

bool usb_midi_quirks_apply(uint8_t slot UNUSED, uint32_t quirks UNUSED,
    uint32_t *detail UNUSED) {
  assert(false);
  return false;
}
#undef UNUSED

static void fixture(void) {
  memset(managed_devices, 0, sizeof(managed_devices));
  memset(&lifecycle_scheduler, 0, sizeof(lifecycle_scheduler));
  memset(&auto_enum_scheduler, 0, sizeof(auto_enum_scheduler));
  memset(&change_stats, 0, sizeof(change_stats));
  memset(last_port_change, 0, sizeof(last_port_change));
  change_head = change_count = change_pending_mask = 0U;
  change_next_port = 1U;
  auto_enum_scheduler.status.enabled = true;
  auto_enum_scheduler.next_port = 1U;
  usb_initialised = true;
  usb_service_failed = false;
  usb_state = (usb_init_info_t){
    .result = USB_INIT_OK, .root_port = 1U, .slot_id = 1U, .address = 1U
  };
  managed_devices[0] = (usb_managed_device_t){
    .valid = true, .attempted = true,
    .device = {.root_port = 1U, .parent_slot = 1U, .parent_port = 1U,
      .slot_id = 2U, .address = 3U, .generation = 7U,
      .state = USB_DEVICE_ACTIVE},
    .enumeration = {.result = USB_ENUM_OK, .slot_id = 2U, .hub_port = 1U}
  };
  hub_port = (usb_hub_port_t){.port = 1U, .status = POWERED | CONNECTED | ENABLED};
  notify_pending = quiesced = disabled = cleaned = ep0_uncertain = false;
  ownership = XHCI_CONNECTION_OWNED;
  ownership_available = true;
  disappear_on_discovery = false;
  enable_slot_calls = 0U;
  disable_result = XHCI_DISABLE_OK;
  quiesce_calls = disable_calls = recycle_calls = 0U;
  // Initial attachment's connection epoch was consumed by enumeration.
  assert(usb_auto_enum_ports_seed(4U, 1U));
  assert(usb_auto_enum_port_request_epoch(1U) == 1U);
  usb_auto_enum_attempt_recorded(1U, 1U);
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);
}

static void notification(uint16_t status, uint16_t change) {
  hub_port.status = status;
  hub_port.change = change;
  notify_pending = true;
  usb_service_hub_changes();
  assert(!notify_pending && hub_port.change == 0U);
  assert(!change_stats.failed);
}

static void check_original_identity(usb_device_state_t state) {
  usb_device_t device = {0};
  assert(usb_get_device_by_generation(7U, &device));
  assert(device.state == state && device.generation == 7U);
  assert(device.slot_id == 2U && device.address == 3U);
  assert(device.root_port == 1U && device.parent_slot == 1U && device.parent_port == 1U);
}

static void check_no_admission(void) {
  usb_auto_enum_select_info_t selection = {0};
  assert(usb_auto_enum_select_inspect(&selection));
  assert(!selection.would_admit);
  assert(!usb_enum_port_dispatch_ready(1U));
}

static void retire(void) {
  usb_lifecycle_auto_arm();
  usb_lifecycle_run_info_t *run = usb_lifecycle_by_generation(7U);
  assert(run && run->state == USB_LIFECYCLE_RUN_ARMED);
  // Execute Disable -> Cleanup -> Detach -> Recycle -> Rearm. Admission
  // remains blocked, with the generation latched, before the final step.
  for (unsigned int step = 0U; step < 5U; step++) {
    check_no_admission();
    assert(managed_devices[0].attempted);
    assert(usb_auto_enum_port_request_epoch(1U) == 0U);
    usb_lifecycle_service();
  }
  assert(run->state == USB_LIFECYCLE_RUN_COMPLETE);
  assert(run->generation == 7U && run->slot_id == 2U);
  assert(run->last_action == USB_LIFECYCLE_REARM && run->last_succeeded);
  assert(managed_devices[0].device.state == USB_DEVICE_DETACHED);
  assert(managed_devices[0].device.generation == 7U);
  assert(managed_devices[0].device.slot_id == 0U && !managed_devices[0].attempted);
  assert(disable_calls == 1U && recycle_calls == 1U);
}

static void test_disconnect(void) {
  fixture();
  notification(POWERED, C_CONNECTION);
  check_original_identity(USB_DEVICE_DISCONNECTING);
  assert(quiesced && quiesce_calls == 1U);
  assert(auto_enum_scheduler.port_epoch[1U] == 2U);
  notification(POWERED, C_CONNECTION);
  assert(quiesce_calls == 1U); // No duplicate lifecycle transition.
  assert(auto_enum_scheduler.port_epoch[1U] == 3U); // Existing connection semantics.
  retire();
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);
  check_no_admission();
  notification(POWERED | CONNECTED, C_CONNECTION);
  assert(usb_auto_enum_port_request_epoch(1U) == 4U);
  assert(usb_enum_port_dispatch_ready(1U));
}

static void test_disable(void) {
  fixture();
  notification(POWERED | CONNECTED, C_ENABLE);
  check_original_identity(USB_DEVICE_DISCONNECTING);
  assert(quiesced && quiesce_calls == 1U);
  assert(auto_enum_scheduler.port_epoch[1U] == 1U);
  notification(POWERED | CONNECTED, C_ENABLE);
  notification(POWERED | CONNECTED, C_ENABLE);
  assert(quiesce_calls == 1U && disable_calls == 0U);
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);
  retire();
  // Same cable, same connection epoch: one new admission request only
  // after the original slot has been safely disabled, cleaned and recycled.
  assert(usb_auto_enum_port_request_epoch(1U) == 1U);
  usb_auto_enum_select_info_t selection = {0};
  assert(usb_auto_enum_select_inspect(&selection) && selection.would_admit);
  assert(selection.pending_ports == (1U << 1));
  assert(selection.selected_port == 1U && usb_enum_port_dispatch_ready(1U));
  notification(POWERED | CONNECTED, C_ENABLE);
  assert(usb_auto_enum_port_request_epoch(1U) == 1U && quiesce_calls == 1U);
  usb_auto_enum_attempt_recorded(1U, 1U);
  notification(POWERED | CONNECTED, C_ENABLE);
  usb_lifecycle_service();
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);
  assert(auto_enum_scheduler.port_epoch[1U] == 1U);
  assert(quiesce_calls == 1U && recycle_calls == 1U);
}

static void test_reconnect_during_retirement(void) {
  fixture();
  notification(POWERED | CONNECTED, C_ENABLE);
  usb_lifecycle_auto_arm();
  usb_lifecycle_run_info_t *run = usb_lifecycle_by_generation(7U);
  assert(run && run->state == USB_LIFECYCLE_RUN_ARMED);
  notification(POWERED, C_CONNECTION);
  notification(POWERED | CONNECTED, C_CONNECTION);
  uint32_t reconnect_epoch = usb_auto_enum_port_request_epoch(1U);
  assert(reconnect_epoch == 3U);
  // Even a real new connection cannot overtake the old slot's teardown.
  for (unsigned int step = 0U; step < 5U; step++) {
    check_no_admission();
    assert(usb_auto_enum_port_request_epoch(1U) == reconnect_epoch);
    usb_lifecycle_service();
  }
  assert(run->state == USB_LIFECYCLE_RUN_COMPLETE);
  assert(usb_auto_enum_port_request_epoch(1U) == reconnect_epoch);
  assert(auto_enum_scheduler.port_epoch[1U] == reconnect_epoch);
  assert(disable_calls == 1U && recycle_calls == 1U && quiesce_calls == 1U);
  // A late completion for the earlier epoch cannot consume the reconnect.
  usb_auto_enum_attempt_recorded(1U, 1U);
  assert(usb_auto_enum_port_request_epoch(1U) == reconnect_epoch);
  assert(usb_enum_port_dispatch_ready(1U));
  // Reconsideration remains subject to the existing live ownership gate.
  ownership = XHCI_CONNECTION_UNCERTAIN;
  check_no_admission();
  assert(usb_auto_enum_port_request_epoch(1U) == reconnect_epoch);
}

static void test_disconnect_before_retirement(void) {
  fixture();
  notification(POWERED | CONNECTED, C_ENABLE);
  notification(POWERED, C_CONNECTION);
  retire();
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);
  assert(auto_enum_scheduler.port_epoch[1U] == 2U);
  check_no_admission();
}

static void test_harmless(void) {
  fixture();
  notification(POWERED | CONNECTED | ENABLED, C_ENABLE);
  notification(POWERED | CONNECTED | ENABLED, C_ENABLE | C_RESET);
  notification(POWERED | CONNECTED | ENABLED, C_RESET);
  check_original_identity(USB_DEVICE_ACTIVE);
  assert(!quiesced && quiesce_calls == 0U);
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);
  // A port need not be enabled yet while its enumeration reset is running.
  managed_devices[0].device.state = USB_DEVICE_ENUMERATING;
  notification(POWERED | CONNECTED | (1U << 4), C_ENABLE);
  check_original_identity(USB_DEVICE_ENUMERATING);
  assert(quiesce_calls == 0U);
  assert(auto_enum_scheduler.port_epoch[1U] == 1U);
}

static void test_fail_closed(void) {
  fixture();
  notification(POWERED | CONNECTED, C_ENABLE);
  ep0_uncertain = true;
  usb_lifecycle_auto_arm();
  usb_lifecycle_service();
  usb_lifecycle_run_info_t *run = usb_lifecycle_by_generation(7U);
  assert(run && run->state == USB_LIFECYCLE_RUN_FAILED);
  check_original_identity(USB_DEVICE_DISCONNECTING);
  assert(disable_calls == 0U && recycle_calls == 0U);
  check_no_admission();
  notification(POWERED | CONNECTED, C_ENABLE);
  usb_lifecycle_auto_arm();
  usb_lifecycle_service();
  assert(quiesce_calls == 1U && recycle_calls == 0U);
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);

  fixture();
  notification(POWERED | CONNECTED, C_ENABLE);
  disable_result = XHCI_DISABLE_COMMAND_FAILED;
  usb_lifecycle_auto_arm();
  usb_lifecycle_service();
  run = usb_lifecycle_by_generation(7U);
  assert(run && run->state == USB_LIFECYCLE_RUN_FAILED);
  check_original_identity(USB_DEVICE_DISCONNECTING);
  assert(disable_calls == 1U && recycle_calls == 0U);
  assert(managed_devices[0].attempted);
  check_no_admission();
  notification(POWERED | CONNECTED, C_ENABLE);
  usb_lifecycle_service();
  assert(disable_calls == 1U && recycle_calls == 0U);
}

static void disappear_before_discovery(void) {
  fixture();
  memset(managed_devices, 0, sizeof(managed_devices));
  ownership = XHCI_CONNECTION_UNOWNED;
  // A new physical connection has been acknowledged and admitted.
  notification(POWERED | CONNECTED, C_CONNECTION);
  assert(usb_auto_enum_port_request_epoch(1U) == 2U);
  assert(usb_enum_port_dispatch_ready(1U));
  disappear_on_discovery = true;
  usb_enum_info_t enumeration = {0};
  uint32_t previous_generation = device_generation;
  assert(usb_enumerate_port(1U, &enumeration) == USB_ENUM_DEVICE_NOT_FOUND);
  assert(device_generation == previous_generation);
  assert(!disappear_on_discovery && enable_slot_calls == 0U);
  assert(enumeration.hub_port == 1U && enumeration.hub_ports == 4U);
  assert(enumeration.stage == USB_ENUM_STAGE_DISCOVER);
  assert(!enumeration.slot_attempted && !enumeration.slot_confirmed);
  assert(enumeration.slot_id == 0U && enumeration.address == 0U);
  assert(managed_devices[0].valid && managed_devices[0].attempted);
  usb_device_t device = {0};
  assert(usb_get_device_by_connection(1U, 1U, 1U, &device));
  assert(device.parent_port == 1U && device.root_port == 1U && device.parent_slot == 1U);
  assert(device.state == USB_DEVICE_DETACHED);
  assert(device.generation == 0U && device.slot_id == 0U);
  assert(usb_auto_enum_port_request_epoch(1U) == 0U); // Attempt consumed its epoch.
}

static void test_discovery_disappearance(void) {
  disappear_before_discovery();
  // A second enumeration cannot bypass the latched failed attempt.
  usb_enum_info_t enumeration = {0};
  assert(usb_enumerate_port(1U, &enumeration) == USB_ENUM_ALREADY_ATTEMPTED);
  assert(enumeration.hub_port == 1U && enumeration.result == USB_ENUM_DEVICE_NOT_FOUND);
  assert(!usb_enumeration_rearm_rejected(1U)); // C_CONNECTION not acknowledged.
  assert(managed_devices[0].attempted);
  // The normal notification path acknowledges disconnection, then rechecks
  // the port and ownership before discarding just this pre-slot attempt.
  notification(POWERED, C_CONNECTION);
  assert(!managed_devices[0].valid && !managed_devices[0].attempted);
  assert(!usb_enumeration_rearm_rejected(1U)); // No duplicate rearm.
  assert(enable_slot_calls == 0U && quiesce_calls == 0U && recycle_calls == 0U);
  check_no_admission();
  assert(usb_auto_enum_port_request_epoch(1U) == 0U);
  notification(POWERED | CONNECTED, C_CONNECTION);
  assert(usb_auto_enum_port_request_epoch(1U) == 4U);
  usb_auto_enum_select_info_t selection = {0};
  assert(usb_auto_enum_select_inspect(&selection) && selection.would_admit);
  assert(selection.selected_port == 1U && usb_enum_port_dispatch_ready(1U));
  assert(usb_managed_record_available(1U, 1U, 1U));
}

static void test_discovery_rearm_gates(void) {
  disappear_before_discovery();
  hub_port.status = POWERED | CONNECTED;
  hub_port.change = 0U;
  assert(!usb_enumeration_rearm_rejected(1U));
  assert(managed_devices[0].valid && managed_devices[0].attempted);
  hub_port.status = POWERED;
  hub_port.change = C_ENABLE;
  assert(!usb_enumeration_rearm_rejected(1U));
  hub_port.change = 0U;
  ownership = XHCI_CONNECTION_UNCERTAIN;
  assert(!usb_enumeration_rearm_rejected(1U));
  ownership = XHCI_CONNECTION_OWNED;
  assert(!usb_enumeration_rearm_rejected(1U));
  ownership_available = false;
  assert(!usb_enumeration_rearm_rejected(1U));
  ownership_available = true;
  ownership = XHCI_CONNECTION_UNOWNED;
  assert(!usb_enumeration_rearm_rejected(2U)); // A different physical port.
  assert(managed_devices[0].valid && managed_devices[0].attempted);
  assert(usb_enumeration_rearm_rejected(1U));
  assert(!managed_devices[0].valid && enable_slot_calls == 0U);
}

static void test_discovery_rearm_history(void) {
  disappear_before_discovery();
  hub_port.change = 0U;
  usb_managed_device_t pristine = managed_devices[0];
  for (unsigned int scenario = 0U; scenario < 14U; scenario++) {
    managed_devices[0] = pristine;
    usb_managed_device_t *record = &managed_devices[0];
    // Even inconsistent partial history must not turn DEVICE_NOT_FOUND
    // into permission to discard uncertain controller ownership.
    switch (scenario) {
      case 0U: record->enumeration.slot_attempted = true; break;
      case 1U: record->enumeration.slot_confirmed = true; break;
      case 2U: record->device.generation = 1U; break;
      case 3U: record->device.slot_id = 2U; break;
      case 4U: record->enumeration.slot_id = 2U; break;
      case 5U: record->enumeration.prepare_attempted = true; break;
      case 6U: record->enumeration.address_attempted = true; break;
      case 7U: record->enumeration.address = 3U; break;
      case 8U: record->enumeration.ep0_operation_failed = true; break;
      case 9U: record->enumeration.bulk_attempted = true; break;
      case 10U: record->enumeration.stage = USB_ENUM_STAGE_ENABLE_SLOT; break;
      case 11U: record->enumeration.result = USB_ENUM_SLOT_FAILED; break;
      case 12U: record->enumeration.hub_port = 2U; break;
      case 13U: record->device.address = 3U; break;
    }
    usb_managed_device_t before = *record;
    assert(!usb_enumeration_rearm_rejected(1U));
    assert(memcmp(record, &before, sizeof(before)) == 0);
  }
  const usb_enum_result_t permitted[] = {USB_ENUM_DEVICE_NOT_FOUND,
    USB_ENUM_UNSUPPORTED_SPEED, USB_ENUM_DEBOUNCE_FAILED, USB_ENUM_RESET_FAILED};
  for (unsigned int i = 0U; i < sizeof(permitted) / sizeof(permitted[0]); i++) {
    managed_devices[0] = pristine;
    managed_devices[0].enumeration.result = permitted[i];
    assert(usb_enumeration_rearm_rejected(1U));
    assert(!managed_devices[0].valid);
  }
  assert(enable_slot_calls == 0U);
}

int main(void) {
  test_disconnect();
  test_disable();
  test_reconnect_during_retirement();
  test_disconnect_before_retirement();
  test_harmless();
  test_fail_closed();
  test_discovery_disappearance();
  test_discovery_rearm_gates();
  test_discovery_rearm_history();
  puts("USB hub port-change lifecycle: PASS");
  return 0;
}
