
#include <stddef.h>
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

typedef struct {
  usb_device_t device;
  usb_enum_info_t enumeration;
  bool valid;
  bool attempted;
} usb_managed_device_t;

// The internal USB 2.0 hub has four downstream
// ports. This is a storage limit, not permission
// to enumerate four devices yet.
#define USB_MANAGED_DEVICE_CAPACITY 4U
static usb_managed_device_t managed_devices[USB_MANAGED_DEVICE_CAPACITY];

// Compatibility owner for the existing single-
// device inspection, recovery and lifecycle
// interfaces.
//
// Enumeration selects its record explicitly.
// Remaining consumers will be migrated before
// multiple-device admission is enabled.
static usb_managed_device_t *const managed_device = &managed_devices[0];

static bool test_abort_after_address;
static bool test_abort_after_config;

// Generations must be unique across all device
// incarnations, regardless of physical port.
static uint32_t device_generation;

static usb_managed_device_t *usb_managed_by_connection(
    uint8_t root_port, uint8_t parent_slot, uint8_t parent_port) {

  if (root_port == 0U || parent_port == 0U)
    return NULL;

  usb_managed_device_t *found = NULL;
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_managed_device_t *record = &managed_devices[i];
    if (!record->valid || record->device.root_port != root_port ||
        record->device.parent_slot != parent_slot ||
        record->device.parent_port != parent_port)
      continue;

    // Never choose arbitrarily between two records
    // claiming the same physical connection.
    if (found)
      return NULL;

    found = record;
  }

  return found;
}

static usb_managed_device_t *usb_managed_by_slot(uint8_t slot_id) {
  if (slot_id == 0U)
    return NULL;

  usb_managed_device_t *found = NULL;
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_managed_device_t *record = &managed_devices[i];
    if (!record->valid || record->device.slot_id != slot_id)
      continue;

    if (found)
      return NULL;

    found = record;
  }

  return found;
}

static usb_managed_device_t *usb_managed_by_generation(uint32_t generation) {
  if (generation == 0U)
    return NULL;

  usb_managed_device_t *found = NULL;
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_managed_device_t *record = &managed_devices[i];
    if (!record->valid || record->device.generation != generation)
      continue;

    if (found)
      return NULL;

    found = record;
  }

  return found;
}

static usb_managed_device_t *usb_managed_select_for_enumeration(
    uint8_t root_port, uint8_t parent_slot, uint8_t parent_port) {

  if (root_port == 0U || parent_slot == 0U || parent_port == 0U)
    return NULL;

  // Multiple records are not yet supported by
  // the USB enumeration and lifecycle layers.
  for (unsigned int i = 1U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    if (managed_devices[i].valid || managed_devices[i].attempted)
      return NULL;
  }

  // Reuse the record already associated with
  // this physical connection, if one exists.
  usb_managed_device_t *record = usb_managed_by_connection(
      root_port, parent_slot, parent_port);

  // The compatibility interfaces still expose
  // only entry zero. Other records cannot yet
  // participate in enumeration.
  if (record && record != managed_device)
    return NULL;

  if (!record)
    record = managed_device;

  // A detached and rearmed record may be reused,
  // including on a different physical port.
  //
  // An active or uncertain device incarnation
  // must never be overwritten.
  if (record->attempted ||
      (record->valid && record->device.state != USB_DEVICE_DETACHED))
    return NULL;

  return record;
}

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

static usb_enum_result_t discover_port(
    usb_enum_info_t *state, uint8_t admitted_port) {

  if (admitted_port == 0U || admitted_port > state->hub_ports)
    return USB_ENUM_PORT_FAILED;

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

  // Enumeration must operate on the connection
  // selected by topology admission.
  //
  // Retain the single-connected-device restriction
  // until independent USB records and lifecycles
  // have been implemented.
  if (found != admitted_port) {
    state->detail = (uint32_t)admitted_port | ((uint32_t)found << 8);
    return USB_ENUM_PORT_FAILED;
  }

  state->hub_port = admitted_port;

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

static usb_enum_result_t address_device(
    usb_managed_device_t *record, usb_enum_info_t *state) {

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
  record->device.slot_id = slot.slot_id;

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
    state->slot_id, state->midi.bulk_in, state->midi.bulk_in_mps,
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
  *info = managed_device->enumeration;
  return managed_device->attempted;
}

usb_enum_result_t usb_enumerate(usb_enum_info_t *info) {
  if (!info)
    return USB_ENUM_NOT_READY;

  if (managed_device->attempted) {
    *info = managed_device->enumeration;
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

  // Bind this enumeration attempt to the record
  // representing the admitted physical connection.
  usb_managed_device_t *record = usb_managed_select_for_enumeration(
      root.root_port, root.slot_id, admission.port);
  if (!record)
    return USB_ENUM_NOT_READY;

  // Some enumeration operations are not safe to retry
  // following an uncertain command completion.
  record->attempted = true;
  usb_enum_info_t state = {0};
  state.stage = USB_ENUM_STAGE_DISCOVER;

  // Start with no downstream device identified.
  record->device = (usb_device_t){0};
  record->device.state = USB_DEVICE_DETACHED;
  record->valid = true;
  state.hub_ports = hub.ports;
  usb_enum_result_t result = USB_ENUM_OK;

  result = discover_port(&state, admission.port);
  if (result != USB_ENUM_OK)
    goto complete;
  // A device has been detected and its port reset.
  record->device.root_port = root.root_port;
  record->device.parent_slot = root.slot_id;
  record->device.parent_port = state.hub_port;
  record->device.speed = (usb_speed_t)state.speed;
  record->device.state = USB_DEVICE_DETECTED;
  // Each new device incarnation receives
  // a distinct generation identifier.
  if (device_generation == UINT32_MAX) {
    result = USB_ENUM_NOT_READY;
    goto complete;
  }
  device_generation++;
  record->device.generation = device_generation;
  // Enumeration will now assign a controller slot
  // and USB address to the detected device.
  record->device.state = USB_DEVICE_ENUMERATING;

  result = address_device(record, &state);
  if (result != USB_ENUM_OK)
    goto complete;

  record->device.slot_id = state.slot_id;
  record->device.address = state.address;

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

  record->device.vendor_id = state.vendor_id;
  record->device.product_id = state.product_id;
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

  record->device.state = USB_DEVICE_CONFIGURED;
  state.stage = USB_ENUM_STAGE_MIDI_QUIRK;
  if (!usb_midi_quirks_apply(state.slot_id, state.quirks, &state.detail))
    result = USB_ENUM_MIDI_QUIRK_FAILED;

  if (result == USB_ENUM_OK) {
    record->device.state = USB_DEVICE_ACTIVE;
    state.stage = USB_ENUM_STAGE_COMPLETE;
  }

complete:
  if (result != USB_ENUM_OK && record->device.state != USB_DEVICE_DETACHED)
    record->device.state = USB_DEVICE_FAILED;
  state.result = result;
  record->enumeration = state;
  *info = state;

  return result;
}

bool usb_get_device(usb_device_t *device) {
  if (!device || !managed_device->valid)
    return false;

  *device = managed_device->device;
  return true;
}

bool usb_get_device_by_connection(uint8_t root_port, uint8_t parent_slot,
    uint8_t parent_port, usb_device_t *device) {

  if (!device)
    return false;

  usb_managed_device_t *record = usb_managed_by_connection(
      root_port, parent_slot, parent_port);
  if (!record)
    return false;

  *device = record->device;
  return true;
}

bool usb_get_device_by_slot(uint8_t slot_id, usb_device_t *device) {
  if (!device)
    return false;

  usb_managed_device_t *record = usb_managed_by_slot(slot_id);
  if (!record)
    return false;

  *device = record->device;
  return true;
}

bool usb_get_device_by_generation(uint32_t generation, usb_device_t *device) {

  if (!device)
    return false;

  usb_managed_device_t *record = usb_managed_by_generation(generation);
  if (!record)
    return false;

  *device = record->device;
  return true;
}

bool usb_enumeration_recovery_inspect(usb_recovery_info_t *info) {
  if (!info)
    return false;

  *info = (usb_recovery_info_t){0};

  if (!managed_device->valid || managed_device->device.state != USB_DEVICE_FAILED ||
      !managed_device->attempted || managed_device->enumeration.result == USB_ENUM_OK) {
    info->result = USB_RECOVERY_NO_FAILED_DEVICE;
    return true;
  }

  info->generation = managed_device->device.generation;
  info->slot_id = managed_device->device.slot_id;

  if (!managed_device->enumeration.slot_confirmed || managed_device->device.slot_id == 0U) {
    info->result = USB_RECOVERY_NO_CONFIRMED_SLOT;
    return true;
  }

  if (!managed_device->enumeration.address_attempted ||
      managed_device->enumeration.address == 0U ||
      managed_device->enumeration.stage < USB_ENUM_STAGE_DEVICE_DESCRIPTOR) {
    info->result = USB_RECOVERY_ADDRESS_UNCONFIRMED;
    return true;
  }

  if (managed_device->enumeration.bulk_attempted) {
    info->result = USB_RECOVERY_BULK_ATTEMPTED;
    return true;
  }

  if (xhci_downstream_is_quiesced(managed_device->device.slot_id) ||
      xhci_downstream_is_slot_disabled(managed_device->device.slot_id) ||
      xhci_downstream_is_cleaned(managed_device->device.slot_id)) {
    info->result = USB_RECOVERY_ALREADY_QUIESCED;
    return true;
  }

  xhci_downstream_transfer_state_t transfers = {0};

  if (!xhci_downstream_transfer_state(managed_device->device.slot_id, &transfers)) {
    info->result = USB_RECOVERY_TRANSFER_STATE_UNAVAILABLE;
    return true;
  }

  info->ep0_submitted = transfers.ep0_submitted;
  info->ep0_completion_seen = transfers.ep0_completion_seen;
  info->ep0_completion_code = transfers.ep0_completion_code;
  info->ep0_state_valid = transfers.ep0_state_valid;
  info->ep0_state = transfers.ep0_state;

  if (transfers.slot_id != managed_device->device.slot_id) {
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
  if (managed_device->enumeration.ep0_operation_failed || transfers.ep0_failed) {
    if (managed_device->enumeration.ep0_operation_failed &&
        managed_device->enumeration.ep0_result == XHCI_CONTROL_TRANSFER_FAILED &&
        transfers.ep0_failed &&
       	xhci_downstream_ep0_fault_reclaimable(managed_device->device.slot_id)) {
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
  if (!xhci_downstream_quiesce(managed_device->device.slot_id))
    return false;
  managed_device->device.state = USB_DEVICE_DISCONNECTING;

  return true;
}

bool usb_device_test_quiesce(void) {
  if (!managed_device->valid || managed_device->device.state != USB_DEVICE_ACTIVE ||
      managed_device->device.slot_id == 0U ||
      xhci_downstream_is_quiesced(managed_device->device.slot_id))
    return false;

  xhci_downstream_transfer_state_t state = {0};
  if (!xhci_downstream_transfer_state(managed_device->device.slot_id, &state))
    return false;

  // Require exactly one outstanding Bulk IN
  // transfer, with no other active operations.
  if (state.slot_id != managed_device->device.slot_id || !state.bulk_in_started ||
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
  if (!xhci_downstream_quiesce(managed_device->device.slot_id))
    return false;

  // Present the same software lifecycle state
  // as an actual downstream disconnection.
  managed_device->device.state = USB_DEVICE_DISCONNECTING;

  return true;
}

void usb_device_port_changed(uint8_t port, uint16_t status, uint16_t change) {
  if (!managed_device->valid)
    return;

  if (managed_device->device.state == USB_DEVICE_DETACHED ||
      managed_device->device.state == USB_DEVICE_DISCONNECTING)
    return;

  // Ignore changes affecting other downstream ports.
  if (port != managed_device->device.parent_port)
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
  if (!xhci_downstream_quiesce(managed_device->device.slot_id))
    return;

  // Retain the slot and address for teardown.
  // They are not yet safe to reuse.
  managed_device->device.state = USB_DEVICE_DISCONNECTING;
}

bool usb_device_finalize_disconnect(void) {
  if (!managed_device->valid ||
      managed_device->device.state != USB_DEVICE_DISCONNECTING ||
      !xhci_downstream_is_quiesced(managed_device->device.slot_id) ||
      !xhci_downstream_is_cleaned(managed_device->device.slot_id))
    return false;

  // Controller teardown is complete.
  // Invalidate the old device identity.
  managed_device->device.slot_id = 0U;
  managed_device->device.address = 0U;
  managed_device->device.vendor_id = 0U;
  managed_device->device.product_id = 0U;
  managed_device->device.speed = USB_SPEED_UNKNOWN;

  // Preserve topology and generation
  // for subsequent diagnostics.
  managed_device->device.state = USB_DEVICE_DETACHED;

  return true;
}

bool usb_enumeration_rearm(void) {
  if (!managed_device->valid || !managed_device->attempted ||
      managed_device->device.state != USB_DEVICE_DETACHED ||
      xhci_downstream_any_owned())
    return false;

  // The old device has been detached and
  // its xHCI state has been recycled.
  managed_device->attempted = false;
  managed_device->enumeration = (usb_enum_info_t){0};

  return true;
}

bool usb_enumeration_rearm_rejected(uint8_t port) {
  // Only recover an unsupported-speed rejection
  // which occurred before acquiring an xHCI slot.
  // A failed operation after slot acquisition
  // requires the existing controlled recovery path.
  if (!managed_device->attempted || managed_device->enumeration.slot_attempted ||
      managed_device->enumeration.result != USB_ENUM_UNSUPPORTED_SPEED ||
      managed_device->enumeration.hub_port != port || !managed_device->valid ||
      managed_device->device.state != USB_DEVICE_DETACHED)
    return false;

  // There must be no outstanding downstream
  // controller resources awaiting reclamation.
  if (xhci_downstream_any_owned())
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
  managed_device->attempted = false;
  managed_device->enumeration = (usb_enum_info_t){0};
  managed_device->device = (usb_device_t){0};
  managed_device->valid = false;

  return true;
}

bool usb_enumeration_test_abort_arm(void) {
  if (managed_device->attempted || test_abort_after_address ||
      test_abort_after_config)
    return false;

  test_abort_after_address = true;
  return true;
}

bool usb_enumeration_test_config_abort_arm(void) {
  if (managed_device->attempted || test_abort_after_address ||
      test_abort_after_config)
    return false;

  test_abort_after_config = true;
  return true;
}

