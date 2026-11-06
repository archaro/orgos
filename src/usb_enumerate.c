
#include <stdbool.h>
#include <stdint.h>

#include "usb.h"
#include "usb_hub.h"
#include "usb_enumerate.h"
#include "xhci.h"

#define USB_DESC_DEVICE 0x01U
#define USB_DESC_CONFIGURATION 0x02U

#define USB_REQ_GET_DESCRIPTOR    0x06U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_REQ_SET_CONFIGURATION 0x09U

#define USB_PORT_CONNECTED 0x0001U
#define USB_PORT_POWERED 0x0100U

static bool enumeration_attempted;
static bool test_abort_after_address;
static bool test_abort_after_config;
static usb_enum_info_t enumeration_state;
static usb_device_t current_device;
static bool device_record_valid;
static uint32_t device_generation;

static uint16_t read_u16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool control_in(usb_enum_info_t *state, bool downstream,
    uint8_t request_type, uint8_t request, uint16_t value,
    uint16_t length, xhci_control_info_t *info) {

  xhci_setup_packet_t setup = {
    .request_type = request_type,
    .request = request,
    .value = value,
    .index = 0U,
    .length = length
  };

  xhci_control_result_t result = downstream ?
    xhci_downstream_control_in(state->slot_id, &setup, info) :
    xhci_control_in(&setup, info);

  if (result != XHCI_CONTROL_OK) {
    state->ep0_operation_failed = true;
    state->ep0_result = result;
    return false;
  }

  if (info->length != length) {
    state->ep0_operation_failed = true;
    return false;
  }

  return true;
}

static usb_enum_result_t discover_port(usb_enum_info_t *state) {
  uint8_t found = 0U;

  for (uint8_t port = 1U; port <= state->hub_ports; port++) {
    usb_hub_port_t info;
    usb_hub_result_t result = usb_hub_port_status(port, &info);

    if (result != USB_HUB_OK) {
      state->detail = (uint32_t)result;
      return USB_ENUM_PORT_FAILED;
    }

    if (!(info.status & USB_PORT_POWERED)) {
      state->detail = port;
      return USB_ENUM_PORT_FAILED;
    }

    if (info.status & USB_PORT_CONNECTED) {
      if (found != 0U)
        return USB_ENUM_MULTIPLE_DEVICES;

      found = port;
    }
  }

  if (found == 0U)
    return USB_ENUM_DEVICE_NOT_FOUND;

  state->hub_port = found;

  // Confirm a stable connection before resetting
  // the downstream port and addressing its device.
  usb_hub_port_t stable;
  usb_hub_result_t result = usb_hub_port_debounce(found, &stable);
  if (result != USB_HUB_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_DEBOUNCE_FAILED;
  }

  usb_hub_reset_info_t reset;
  result = usb_hub_port_reset(found, &reset);
  if (result != USB_HUB_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_RESET_FAILED;
  }

  state->speed = reset.speed;

  if (reset.speed != 1U)
    return USB_ENUM_UNSUPPORTED_SPEED;

  return USB_ENUM_OK;
}

static usb_enum_result_t address_device( usb_enum_info_t *state) {
  xhci_slot_info_t slot = {0};
  state->stage = USB_ENUM_STAGE_ENABLE_SLOT;
  state->slot_attempted = true;
  xhci_slot_result_t slot_result =
    xhci_enable_downstream_slot(state->hub_port, &slot);

  if (slot_result != XHCI_SLOT_OK) {
    state->detail = (uint32_t)slot_result;
    return USB_ENUM_SLOT_FAILED;
  }

  state->slot_confirmed = true;
  state->slot_id = slot.slot_id;

  // Preserve identity immediately, not after
  // the later preparation/addressing operations.
  current_device.slot_id = slot.slot_id;

  state->stage = USB_ENUM_STAGE_PREPARE;
  state->prepare_attempted = true;
  xhci_device_info_t device = {0};
  xhci_device_result_t device_result =
      xhci_downstream_prepare(state->hub_port, state->speed, &device);

  if (device_result != XHCI_DEVICE_OK) {
    state->detail = (uint32_t)device_result;
    return USB_ENUM_PREPARE_FAILED;
  }

  state->stage = USB_ENUM_STAGE_ADDRESS;
  state->address_attempted = true;
  xhci_address_info_t address = {0};
  xhci_address_result_t address_result =
    xhci_address_downstream(state->slot_id, &address);

  if (address_result != XHCI_ADDRESS_OK) {
    state->detail = (uint32_t)address_result;
    return USB_ENUM_ADDRESS_FAILED;
  }

  state->address = address.usb_address;
  return USB_ENUM_OK;
}

static usb_enum_result_t read_device(usb_enum_info_t *state) {
  xhci_control_info_t info;

  // Full-Speed enumeration begins with eight bytes.
  if (!control_in(state, true, 0x80U, USB_REQ_GET_DESCRIPTOR,
      (uint16_t)(USB_DESC_DEVICE << 8), 8U, &info) ||
      info.data[0] != 18U || info.data[1] != USB_DESC_DEVICE) {
    state->detail = info.completion_code;
    return USB_ENUM_DESCRIPTOR_FAILED;
  }

  // The present xHCI driver supports a 64-byte MPS update.
  if (info.data[7] != 64U) {
    state->detail = info.data[7];
    return USB_ENUM_MPS_FAILED;
  }

  xhci_ep0_update_info_t update;
  xhci_ep0_update_result_t result =
    xhci_downstream_ep0_update(state->slot_id, info.data[7], &update);

  if (result != XHCI_EP0_UPDATE_OK) {
    state->detail = (uint32_t)result;
    state->ep0_operation_failed = true;
    return USB_ENUM_MPS_FAILED;
  }

  if (!control_in(state, true, 0x80U, USB_REQ_GET_DESCRIPTOR,
      (uint16_t)(USB_DESC_DEVICE << 8), 18U, &info) ||
      info.data[0] != 18U || info.data[1] != USB_DESC_DEVICE ||
      info.data[7] != 64U || info.data[17] == 0U) {
    state->detail = info.completion_code;
    return USB_ENUM_DESCRIPTOR_FAILED;
  }

  state->vendor_id = read_u16(&info.data[8]);
  state->product_id = read_u16(&info.data[10]);

  return USB_ENUM_OK;
}

static usb_enum_result_t read_configuration( usb_enum_info_t *state) {
  xhci_control_info_t info;

  if (!control_in(state, true, 0x80U, USB_REQ_GET_DESCRIPTOR,
      (uint16_t)(USB_DESC_CONFIGURATION << 8), 9U, &info) ||
      info.data[0] != 9U || info.data[1] != USB_DESC_CONFIGURATION) {
    state->detail = info.completion_code;
    return USB_ENUM_CONFIG_FAILED;
  }

  uint16_t length = read_u16(&info.data[2]);
  if (length < 9U || length > XHCI_CONTROL_MAX_DATA) {
    state->detail = length;
    return USB_ENUM_CONFIG_FAILED;
  }

  state->configuration_length = length;

  if (!control_in(state, true, 0x80U, USB_REQ_GET_DESCRIPTOR,
      (uint16_t)(USB_DESC_CONFIGURATION << 8), length, &info)) {
    state->detail = info.completion_code;
    return USB_ENUM_CONFIG_FAILED;
  }

  if (!usb_midi_parse_config(info.data, info.length,
      &state->midi))
    return USB_ENUM_PARSE_FAILED;

  return USB_ENUM_OK;
}

static usb_enum_result_t select_configuration(usb_enum_info_t *state) {

  uint8_t configuration = state->midi.configuration;
  if (configuration == 0U) {
    state->detail = 0U;
    return USB_ENUM_SET_CONFIG_FAILED;
  }

  xhci_setup_packet_t setup = {
    .request_type = 0x00U,
    .request = USB_REQ_SET_CONFIGURATION,
    .value = configuration,
    .index = 0U,
    .length = 0U
  };

  xhci_control_info_t info = {0};
  xhci_control_result_t result =
    xhci_downstream_control_no_data(state->slot_id, &setup, &info);
  if (result != XHCI_CONTROL_OK) {
    state->detail = (uint32_t)result;
    state->ep0_operation_failed = true;
    state->ep0_result = result;
    return USB_ENUM_SET_CONFIG_FAILED;
  }

  if (!control_in(state, true, 0x80U, USB_REQ_GET_CONFIGURATION,
        0U, 1U, &info)) {
    state->detail = info.completion_code;
    return USB_ENUM_VERIFY_CONFIG_FAILED;
  }


  if (info.data[0] != configuration) {
    state->detail = info.data[0];
    return USB_ENUM_VERIFY_CONFIG_FAILED;
  }

  // Windows issues SET_INTERFACE after selecting the
  // U2MIDI Pro's configuration, before the first MIDI
  // transfer. Match that observed initialisation step.
  xhci_setup_packet_t interface_setup = {
    .request_type = 0x01U,
    .request = 0x0bU,
    .value = state->midi.midi_alternate,
    .index = state->midi.midi_interface,
    .length = 0U
  };

  xhci_control_info_t interface_info = {0};
  xhci_control_result_t interface_result = xhci_downstream_control_no_data(
        state->slot_id, &interface_setup, &interface_info);

  if (interface_result != XHCI_CONTROL_OK) {
    state->detail = (uint32_t)interface_result |
        ((uint32_t)interface_info.completion_code << 8);
    state->ep0_operation_failed = true;
    state->ep0_result = interface_result;
    return USB_ENUM_SET_INTERFACE_FAILED;
  }

  return USB_ENUM_OK;
}

static usb_enum_result_t configure_bulk( usb_enum_info_t *state) {

  state->bulk_attempted = true;
  xhci_bulk_config_info_t info = {0};
  xhci_bulk_config_result_t result = xhci_downstream_bulk_configure(
          state->midi.bulk_in, state->midi.bulk_in_mps,
          state->midi.bulk_out, state->midi.bulk_out_mps, &info);

  if (result != XHCI_BULK_CONFIG_OK) {
    // Low byte: configuration result.
    // High byte: xHCI completion code.
    state->detail = (uint32_t)result | ((uint32_t)info.completion_code << 8);
    return USB_ENUM_BULK_CONFIG_FAILED;
  }

  return USB_ENUM_OK;
}

bool usb_get_enumeration(usb_enum_info_t *info) {
  if (!info)
    return false;
  *info = enumeration_state;
  return enumeration_attempted;
}

usb_enum_result_t usb_enumerate(usb_enum_info_t *info) {
  if (!info)
    return USB_ENUM_NOT_READY;

  if (enumeration_attempted) {
    *info = enumeration_state;
    return USB_ENUM_ALREADY_ATTEMPTED;
  }

  usb_enum_admission_info_t admission = {0};
  if (!usb_enum_admission_inspect(&admission) ||
      (admission.result != USB_ENUM_ADMIT_INITIAL &&
       admission.result != USB_ENUM_ADMIT_RECONNECT)) {
    *info = (usb_enum_info_t){0};
    info->result = USB_ENUM_NOT_READY;
    return USB_ENUM_NOT_READY;
  }

  usb_init_info_t root;
  if (!usb_get_init_info(&root) ||
      root.result != USB_INIT_OK || root.address == 0U)
    return USB_ENUM_NOT_READY;

  // The internal hub must be configured and powered
  // before downstream devices can be enumerated.
  usb_hub_init_info_t hub = {0};
  if (!usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK)
    return USB_ENUM_NOT_READY;

  // Some enumeration operations are not safe to retry
  // following an uncertain command completion.
  enumeration_attempted = true;
  usb_enum_info_t state = {0};
  state.stage = USB_ENUM_STAGE_DISCOVER;

  // Start with no downstream device identified.
  current_device = (usb_device_t){0};
  current_device.state = USB_DEVICE_DETACHED;
  device_record_valid = true;
  state.hub_ports = hub.ports;
  usb_enum_result_t result = USB_ENUM_OK;

  result = discover_port(&state);
  if (result != USB_ENUM_OK)
    goto complete;
  // A device has been detected and its port reset.
  current_device.root_port = root.root_port;
  current_device.parent_slot = root.slot_id;
  current_device.parent_port = state.hub_port;
  current_device.speed = (usb_speed_t)state.speed;
  current_device.state = USB_DEVICE_DETECTED;
  // Each new device incarnation receives
  // a distinct generation identifier.
  if (device_generation == UINT32_MAX) {
    result = USB_ENUM_NOT_READY;
    goto complete;
  }
  device_generation++;
  current_device.generation = device_generation;
  // Enumeration will now assign a controller slot
  // and USB address to the detected device.
  current_device.state = USB_DEVICE_ENUMERATING;

  result = address_device(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  current_device.slot_id = state.slot_id;
  current_device.address = state.address;

  state.stage = USB_ENUM_STAGE_DEVICE_DESCRIPTOR;

  // Diagnostic: leave a confirmed, addressed
  // downstream slot for controlled recovery.
  if (test_abort_after_address) {
    test_abort_after_address = false;
    result = USB_ENUM_TEST_ABORT;
    goto complete;
  }

  result = read_device(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  current_device.vendor_id = state.vendor_id;
  current_device.product_id = state.product_id;
  state.quirks = usb_midi_quirks_lookup(state.vendor_id, state.product_id);

  state.stage = USB_ENUM_STAGE_CONFIGURATION_DESCRIPTOR;
  result = read_configuration(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  // Diagnostic: descriptor transfers and parsing
  // succeeded, but enumeration stops before the
  // device configuration is selected.
  if (test_abort_after_config) {
    test_abort_after_config = false;
    result = USB_ENUM_TEST_CONFIG_ABORT;
    goto complete;
  }

  state.stage = USB_ENUM_STAGE_SELECT_CONFIGURATION;
  result = select_configuration(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  state.stage = USB_ENUM_STAGE_CONFIGURE_BULK;
  result = configure_bulk(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  current_device.state = USB_DEVICE_CONFIGURED;
  state.stage = USB_ENUM_STAGE_MIDI_QUIRK;
  if (!usb_midi_quirks_apply(state.quirks, &state.detail))
    result = USB_ENUM_MIDI_QUIRK_FAILED;

  if (result == USB_ENUM_OK) {
    current_device.state = USB_DEVICE_ACTIVE;
    state.stage = USB_ENUM_STAGE_COMPLETE;
  }

complete:
  if (result != USB_ENUM_OK && current_device.state != USB_DEVICE_DETACHED)
    current_device.state = USB_DEVICE_FAILED;
  state.result = result;
  enumeration_state = state;
  *info = state;

  return result;
}

bool usb_get_device(usb_device_t *device) {
  if (!device || !device_record_valid)
    return false;

  *device = current_device;
  return true;
}

bool usb_enumeration_recovery_inspect(usb_recovery_info_t *info) {
  if (!info)
    return false;

  *info = (usb_recovery_info_t){0};

  if (!device_record_valid || current_device.state != USB_DEVICE_FAILED ||
      !enumeration_attempted || enumeration_state.result == USB_ENUM_OK) {
    info->result = USB_RECOVERY_NO_FAILED_DEVICE;
    return true;
  }

  info->generation = current_device.generation;
  info->slot_id = current_device.slot_id;

  if (!enumeration_state.slot_confirmed || current_device.slot_id == 0U) {
    info->result = USB_RECOVERY_NO_CONFIRMED_SLOT;
    return true;
  }

  if (!enumeration_state.address_attempted ||
      enumeration_state.address == 0U ||
      enumeration_state.stage < USB_ENUM_STAGE_DEVICE_DESCRIPTOR) {
    info->result = USB_RECOVERY_ADDRESS_UNCONFIRMED;
    return true;
  }

  if (enumeration_state.bulk_attempted) {
    info->result = USB_RECOVERY_BULK_ATTEMPTED;
    return true;
  }

  if (xhci_downstream_is_quiesced() || xhci_downstream_is_slot_disabled() ||
      xhci_downstream_is_cleaned()) {
    info->result = USB_RECOVERY_ALREADY_QUIESCED;
    return true;
  }

  xhci_downstream_transfer_state_t transfers = {0};

  if (!xhci_downstream_transfer_state(&transfers)) {
    info->result = USB_RECOVERY_TRANSFER_STATE_UNAVAILABLE;
    return true;
  }

  info->ep0_submitted = transfers.ep0_submitted;
  info->ep0_completion_seen = transfers.ep0_completion_seen;
  info->ep0_completion_code = transfers.ep0_completion_code;
  info->ep0_state_valid = transfers.ep0_state_valid;
  info->ep0_state = transfers.ep0_state;

  if (transfers.slot_id != current_device.slot_id) {
    info->result = USB_RECOVERY_TRANSFER_STATE_UNAVAILABLE;
    return true;
  }

  if (transfers.active_waiters != 0U || transfers.ep0_active ||
      transfers.bulk_out_active || transfers.bulk_in_pending ||
      transfers.bulk_in_started) {
    info->result = USB_RECOVERY_TRANSFER_BUSY;
    return true;
  }

  // EP0 error recovery limitations:
  //
  // TODO: Uncertain completions (timeouts and event
  // dispatch failures) have no proven cancellation
  // or reconciliation path. They remain blocked.
  //
  // TODO: Controlled hardware failure testing has
  // not yet verified teardown following a genuine
  // EP0 error completion. The confirmed-error path
  // is implemented but remains unproven on hardware.
  if (enumeration_state.ep0_operation_failed || transfers.ep0_failed) {
    if (enumeration_state.ep0_operation_failed &&
        enumeration_state.ep0_result == XHCI_CONTROL_TRANSFER_FAILED &&
        transfers.ep0_failed && xhci_downstream_ep0_fault_reclaimable()) {
      info->result = USB_RECOVERY_READY_EP0_ERROR;
    } else if (transfers.ep0_submitted && transfers.ep0_completion_seen &&
        transfers.ep0_completion_code != 1U) {
      info->result = USB_RECOVERY_EP0_ERROR_COMPLETION;
    } else if (transfers.ep0_submitted && !transfers.ep0_completion_seen) {
      info->result = USB_RECOVERY_EP0_OUTCOME_UNKNOWN;
    } else {
      info->result = USB_RECOVERY_EP0_FAULT;
    }
    return true;
  }

  info->result = USB_RECOVERY_READY;
  return true;
}

bool usb_enumeration_recovery_ready(void) {
  usb_recovery_info_t info = {0};

  if (!usb_enumeration_recovery_inspect(&info))
    return false;

  return info.result == USB_RECOVERY_READY ||
      info.result == USB_RECOVERY_READY_EP0_ERROR;
}

bool usb_enumeration_recovery_begin(void) {
  if (!usb_enumeration_recovery_ready())
    return false;

  // The old xHCI slot must not accept further
  // transfers while recovery dismantles it.
  xhci_downstream_quiesce();
  current_device.state = USB_DEVICE_DISCONNECTING;

  return true;
}

bool usb_device_test_quiesce(void) {
  if (!device_record_valid || current_device.state != USB_DEVICE_ACTIVE ||
      current_device.slot_id == 0U || xhci_downstream_is_quiesced())
    return false;

  xhci_downstream_transfer_state_t state = {0};
  if (!xhci_downstream_transfer_state(&state))
    return false;

  // Require exactly one outstanding Bulk IN
  // transfer, with no other active operations.
  if (state.slot_id != current_device.slot_id || !state.bulk_in_started ||
      !state.bulk_in_pending || state.bulk_in_completed ||
      state.bulk_in_failed || state.active_waiters != 1U ||
      state.ep0_active || state.bulk_out_active)
    return false;

  // Verify that the endpoint is Running before
  // deliberately interrupting its operation.
  if (!state.endpoint_states_valid || state.bulk_in_ep_state != 1U)
    return false;

  // This is an irreversible transition for
  // the current device generation.
  xhci_downstream_quiesce();

  // Present the same software lifecycle state
  // as an actual downstream disconnection.
  current_device.state = USB_DEVICE_DISCONNECTING;

  return true;
}

void usb_device_port_changed(uint8_t port, uint16_t status, uint16_t change) {
  if (!device_record_valid)
    return;

  if (current_device.state == USB_DEVICE_DETACHED ||
      current_device.state == USB_DEVICE_DISCONNECTING)
    return;

  // Ignore changes affecting other downstream ports.
  if (port != current_device.parent_port)
    return;

  // A connection-change flag invalidates the
  // existing device identity even if the port
  // currently reports a connected device.
  bool connection_changed = (change & 0x0001U) != 0U;
  bool connected = (status & 0x0001U) != 0U;
  if (!connection_changed && connected)
    return;

  // Prevent new transfers through this device's
  // existing xHCI slot before declaring it stale.
  xhci_downstream_quiesce();

  // Retain the slot and address for teardown.
  // They are not yet safe to reuse.
  current_device.state = USB_DEVICE_DISCONNECTING;
}

bool usb_device_finalize_disconnect(void) {
  if (!device_record_valid ||
      current_device.state != USB_DEVICE_DISCONNECTING ||
      !xhci_downstream_is_quiesced() || !xhci_downstream_is_cleaned())
    return false;

  // Controller teardown is complete.
  // Invalidate the old device identity.
  current_device.slot_id = 0U;
  current_device.address = 0U;
  current_device.vendor_id = 0U;
  current_device.product_id = 0U;
  current_device.speed = USB_SPEED_UNKNOWN;

  // Preserve topology and generation
  // for subsequent diagnostics.
  current_device.state = USB_DEVICE_DETACHED;

  return true;
}

bool usb_enumeration_rearm(void) {
  if (!device_record_valid || !enumeration_attempted ||
      current_device.state != USB_DEVICE_DETACHED ||
      xhci_downstream_is_quiesced() || xhci_downstream_is_cleaned())
    return false;

  // The old device has been detached and
  // its xHCI state has been recycled.
  enumeration_attempted = false;
  enumeration_state = (usb_enum_info_t){0};

  return true;
}

bool usb_enumeration_rearm_rejected(uint8_t port) {
  // Only recover an unsupported-speed rejection
  // which occurred before acquiring an xHCI slot.
  // A failed operation after slot acquisition
  // requires the existing controlled recovery path.
  if (!enumeration_attempted || enumeration_state.slot_attempted ||
      enumeration_state.result != USB_ENUM_UNSUPPORTED_SPEED ||
      enumeration_state.hub_port != port || !device_record_valid ||
      current_device.state != USB_DEVICE_DETACHED)
    return false;

  // There must be no outstanding downstream
  // controller resources awaiting reclamation.
  if (xhci_downstream_is_quiesced() || xhci_downstream_is_slot_disabled() ||
      xhci_downstream_is_cleaned())
    return false;

  // Confirm that the rejected port is now empty
  // and that all hub change flags have settled.
  usb_hub_port_t status = {0};
  if (usb_hub_port_status(port, &status) != USB_HUB_OK ||
      (status.status & USB_PORT_CONNECTED) != 0U || status.change != 0U)
    return false;

  // No slot was acquired and no downstream
  // allocations were handed to the controller.
  // Discard the unsuccessful enumeration record.
  enumeration_attempted = false;
  enumeration_state = (usb_enum_info_t){0};
  current_device = (usb_device_t){0};
  device_record_valid = false;

  return true;
}

bool usb_enumeration_test_abort_arm(void) {
  if (enumeration_attempted || test_abort_after_address ||
      test_abort_after_config)
    return false;

  test_abort_after_address = true;
  return true;
}

bool usb_enumeration_test_config_abort_arm(void) {
  if (enumeration_attempted || test_abort_after_address ||
      test_abort_after_config)
    return false;

  test_abort_after_config = true;
  return true;
}

