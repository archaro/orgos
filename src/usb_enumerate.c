
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

// The Raspberry Pi 4b internal USB 2.0 hub has four downstream
// ports. This is a storage limit, not permission
// to enumerate four devices yet.
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

static bool usb_managed_record_reusable( const usb_managed_device_t *record) {
  if (!record || record->attempted)
    return false;

  // An unused record has no previous device
  // incarnation or controller resources.
  if (!record->valid)
    return true;

  // A previously used record is reusable only
  // after successful detachment and rearm.
  if (record->device.state != USB_DEVICE_DETACHED ||
      record->device.slot_id != 0U || record->device.generation == 0U)
    return false;

  xhci_connection_ownership_t ownership = XHCI_CONNECTION_UNCERTAIN;
  if (!xhci_downstream_connection_ownership( record->device.root_port,
      record->device.parent_slot, record->device.parent_port, &ownership))
    return false;

  return ownership == XHCI_CONNECTION_UNOWNED;
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
    uint8_t root_port, uint8_t parent_slot,
    uint8_t parent_port, bool multiple_records_enabled) {

  if (root_port == 0U || parent_slot == 0U || parent_port == 0U)
    return NULL;

  usb_managed_device_t *matching = NULL;

  // Look for an existing record associated with
  // this physical connection. Reject duplicate
  // ownership rather than selecting arbitrarily.
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_managed_device_t *record = &managed_devices[i];
    if (!record->valid || record->device.root_port != root_port ||
        record->device.parent_slot != parent_slot ||
        record->device.parent_port != parent_port)
      continue;

    if (matching)
      return NULL;

    matching = record;
  }

  if (!multiple_records_enabled) {
    // Compatibility mode: only entry zero may
    // participate in enumeration. Retain the
    // existing single-device ownership restriction.
    for (unsigned int i = 1U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
      if (managed_devices[i].valid || managed_devices[i].attempted)
        return NULL;
    }

    if (matching && matching != managed_device)
      return NULL;

    return usb_managed_record_reusable(managed_device) ?
        managed_device : NULL;
  }

  // Future multi-record mode.
  //
  // Reuse an existing connection record only
  // when its previous incarnation is fully
  // detached, rearmed and unowned.
  if (matching)
    return usb_managed_record_reusable(matching) ?  matching : NULL;

  // Prefer completely unused records.
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_managed_device_t *record = &managed_devices[i];
    if (!record->valid && usb_managed_record_reusable(record))
      return record;
  }

  // Otherwise reuse the oldest safely released
  // device incarnation.
  usb_managed_device_t *oldest = NULL;
  for (unsigned int i = 0U; i < USB_MANAGED_DEVICE_CAPACITY; i++) {
    usb_managed_device_t *record = &managed_devices[i];
    if (!record->valid || !usb_managed_record_reusable(record))
      continue;

    if (!oldest || record->device.generation < oldest->device.generation)
      oldest = record;
  }

  return oldest;
}

bool usb_managed_record_available(uint8_t root_port,
    uint8_t parent_slot, uint8_t parent_port) {

  // Use the existing multi-record selection rules.
  // This function only inspects; it changes no state.
  return usb_managed_select_for_enumeration(
      root_port, parent_slot, parent_port, true) != NULL;
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

  if (!state || admitted_port == 0U || admitted_port > state->hub_ports)
    return USB_ENUM_PORT_FAILED;

  // Inspect the selected port, not the complete
  // hub topology. Other ports are outside the
  // scope of this operation.
  usb_hub_port_t current = {0};
  usb_hub_result_t result = usb_hub_port_status(admitted_port, &current);

  if (result != USB_HUB_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_PORT_FAILED;
  }

  if (!(current.status & USB_PORT_POWERED)) {
    state->detail = admitted_port;
    return USB_ENUM_PORT_FAILED;
  }

  if (!(current.status & USB_PORT_CONNECTED))
    return USB_ENUM_DEVICE_NOT_FOUND;

  // Retain the physical connection identity
  // before any fallible debounce or reset.
  state->hub_port = admitted_port;
  usb_hub_port_t stable = {0};
  result = usb_hub_port_debounce(admitted_port, &stable);

  if (result != USB_HUB_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_DEBOUNCE_FAILED;
  }

  usb_hub_reset_info_t reset = {0};

  result = usb_hub_port_reset(admitted_port, &reset);
  if (result != USB_HUB_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_RESET_FAILED;
  }

  state->speed = reset.speed;

  // Full-Speed remains the only downstream
  // device speed supported by this driver.
  if (reset.speed != 1U)
    return USB_ENUM_UNSUPPORTED_SPEED;

  return USB_ENUM_OK;
}

static usb_enum_result_t discover_compatibility_topology(
    usb_enum_info_t *state, uint8_t admitted_port) {

  if (!state || admitted_port == 0U || admitted_port > state->hub_ports)
    return USB_ENUM_PORT_FAILED;

  uint8_t found = 0U;
  // Preserve the original single-device policy.
  // Every hub port must be readable and powered,
  // and exactly one may have a connected device.
  for (uint8_t port = 1U; port <= state->hub_ports; port++) {
    usb_hub_port_t info = {0};
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

  // The admitted connection and the sole
  // observed connection must be identical.
  if (found != admitted_port) {
    state->detail = (uint32_t)admitted_port | ((uint32_t)found << 8);
    return USB_ENUM_PORT_FAILED;
  }

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

bool usb_get_enumeration_generation(
    uint32_t generation, usb_enum_info_t *info) {

  if (!info)
    return false;

  *info = (usb_enum_info_t){0};
  usb_managed_device_t *record = usb_managed_by_generation(generation);
  if (!record || !record->attempted)
    return false;

  *info = record->enumeration;
  return true;
}

bool usb_get_enumeration_at(unsigned int index, usb_enum_info_t *info) {
  if (!info)
    return false;

  *info = (usb_enum_info_t){0};

  if (index >= USB_MANAGED_DEVICE_CAPACITY)
    return false;

  const usb_managed_device_t *record = &managed_devices[index];
  if (!record->valid || !record->attempted)
    return false;

  *info = record->enumeration;
  return true;
}

bool usb_get_enumeration_by_connection(uint8_t root_port, uint8_t parent_slot,
    uint8_t parent_port, usb_enum_info_t *info) {

  if (!info)
    return false;

  *info = (usb_enum_info_t){0};

  usb_managed_device_t *record = usb_managed_by_connection(
      root_port, parent_slot, parent_port);

  if (!record || !record->attempted)
    return false;

  *info = record->enumeration;
  return true;
}

static usb_enum_result_t usb_enumerate_record(usb_managed_device_t *record,
    const usb_init_info_t *root, uint8_t admitted_port, uint8_t hub_ports,
    bool require_single_device, usb_enum_info_t *info) {

  if (!record || !root || !info || root->root_port == 0U ||
      root->slot_id == 0U || admitted_port == 0U ||
      admitted_port > hub_ports)
    return USB_ENUM_NOT_READY;

  // An attempted enumeration belongs to its
  // selected record, not to the compatibility
  // device or some other physical connection.
  if (record->attempted) {
    *info = record->enumeration;
    return USB_ENUM_ALREADY_ATTEMPTED;
  }

  // Only unused or safely rearmed records may
  // begin another enumeration attempt.
  if (!usb_managed_record_reusable(record))
    return USB_ENUM_NOT_READY;

  // Some enumeration operations are not safe to retry
  // following an uncertain command completion.
  record->attempted = true;
  usb_enum_info_t state = {0};
  state.stage = USB_ENUM_STAGE_DISCOVER;

  // Record physical ownership before performing
  // any fallible hub-port operation.
  //
  // Discovery may fail before a device generation
  // or xHCI slot has been assigned. The attempt
  // must nevertheless remain attributable to its
  // admitted physical connection.
  record->device = (usb_device_t){
    .root_port = root->root_port,
    .parent_slot = root->slot_id,
    .parent_port = admitted_port,
    .state = USB_DEVICE_DETACHED
  };

  record->valid = true;
  state.hub_ports = hub_ports;
  usb_enum_result_t result = USB_ENUM_OK;

  // Retain the original whole-hub restriction
  // for the public single-device enumeration path.
  //
  // Future independent admission will bypass
  // this policy check, but not the selected-port
  // discovery operation which follows it.
  if (require_single_device) {
    result = discover_compatibility_topology(&state, admitted_port);
    if (result != USB_ENUM_OK)
      goto complete;
  }

  // Only the explicitly admitted physical port
  // may now be debounced and reset.
  result = discover_port(&state, admitted_port);
  if (result != USB_ENUM_OK)
    goto complete;
  // A device has been detected and its port reset.
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

  // The attempt is now permanently associated
  // with this record until explicit rearm.
  // Consume its request regardless of result.
  usb_auto_enum_attempt_recorded(admitted_port);

  return result;
}

usb_enum_result_t usb_enumerate(usb_enum_info_t *info) {
  if (!info)
    return USB_ENUM_NOT_READY;

  // Compatibility behaviour: expose the recorded
  // result of the existing single-device attempt.
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

  usb_init_info_t root = {0};
  if (!usb_get_init_info(&root) || root.result != USB_INIT_OK ||
      root.address == 0U)
    return USB_ENUM_NOT_READY;

  // The internal hub must be configured and
  // powered before enumerating downstream devices.
  usb_hub_init_info_t hub = {0};
  if (!usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK)
    return USB_ENUM_NOT_READY;

  // Remain in single-device compatibility mode
  // until the comprehensive regression is passed.
  usb_managed_device_t *record = usb_managed_select_for_enumeration(
      root.root_port, root.slot_id, admission.port, false);

  if (!record)
    return USB_ENUM_NOT_READY;

  // Admission and record selection are complete.
  // The enumeration engine now operates solely
  // on the explicitly selected managed record.
  // The comprehensive regression gate has not
  // yet been passed. Require the single-device
  // topology policy for every public attempt.
  return usb_enumerate_record(record, &root, admission.port,
      hub.ports, true, info);
}

bool usb_get_device(usb_device_t *device) {
  if (!device || !managed_device->valid)
    return false;

  *device = managed_device->device;
  return true;
}

bool usb_get_device_at( unsigned int index, usb_device_t *device) {
  if (!device)
    return false;

  *device = (usb_device_t){0};

  if (index >= USB_MANAGED_DEVICE_CAPACITY)
    return false;

  const usb_managed_device_t *record = &managed_devices[index];
  if (!record->valid)
    return false;

  *device = record->device;
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

static bool usb_enumeration_recovery_inspect_record(
    const usb_managed_device_t *record, usb_recovery_info_t *info) {

  if (!info)
    return false;

  *info = (usb_recovery_info_t){0};

  if (!record || !record->valid ||
      record->device.state != USB_DEVICE_FAILED || !record->attempted ||
      record->enumeration.result == USB_ENUM_OK) {
    info->result = USB_RECOVERY_NO_FAILED_DEVICE;
    return true;
  }

  info->generation = record->device.generation;
  info->slot_id = record->device.slot_id;

  if (!record->enumeration.slot_confirmed || record->device.slot_id == 0U) {
    info->result = USB_RECOVERY_NO_CONFIRMED_SLOT;
    return true;
  }

  if (!record->enumeration.address_attempted ||
      record->enumeration.address == 0U ||
      record->enumeration.stage < USB_ENUM_STAGE_DEVICE_DESCRIPTOR) {
    info->result = USB_RECOVERY_ADDRESS_UNCONFIRMED;
    return true;
  }

  if (record->enumeration.bulk_attempted) {
    info->result = USB_RECOVERY_BULK_ATTEMPTED;
    return true;
  }

  if (xhci_downstream_is_quiesced(record->device.slot_id) ||
      xhci_downstream_is_slot_disabled(record->device.slot_id) ||
      xhci_downstream_is_cleaned(record->device.slot_id)) {
    info->result = USB_RECOVERY_ALREADY_QUIESCED;
    return true;
  }

  xhci_downstream_transfer_state_t transfers = {0};

  if (!xhci_downstream_transfer_state(record->device.slot_id, &transfers)) {
    info->result = USB_RECOVERY_TRANSFER_STATE_UNAVAILABLE;
    return true;
  }

  info->ep0_submitted = transfers.ep0_submitted;
  info->ep0_completion_seen = transfers.ep0_completion_seen;
  info->ep0_completion_code = transfers.ep0_completion_code;
  info->ep0_state_valid = transfers.ep0_state_valid;
  info->ep0_state = transfers.ep0_state;

  if (transfers.slot_id != record->device.slot_id) {
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
  if (record->enumeration.ep0_operation_failed || transfers.ep0_failed) {
    if (record->enumeration.ep0_operation_failed &&
        record->enumeration.ep0_result == XHCI_CONTROL_TRANSFER_FAILED &&
        transfers.ep0_failed &&
       	xhci_downstream_ep0_fault_reclaimable(record->device.slot_id)) {
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

bool usb_enumeration_recovery_inspect(
    usb_recovery_info_t *info) {

  // Compatibility inspection also handles failures
  // which occurred before a generation was assigned.
  return usb_enumeration_recovery_inspect_record(
      managed_device, info);
}

bool usb_enumeration_recovery_inspect_generation(
    uint32_t generation, usb_recovery_info_t *info) {

  if (!info)
    return false;

  *info = (usb_recovery_info_t){0};

  usb_managed_device_t *record = usb_managed_by_generation(generation);
  if (!record)
    return false;

  return usb_enumeration_recovery_inspect_record( record, info);
}

bool usb_enumeration_recovery_ready(void) {
  usb_recovery_info_t info = {0};

  if (!usb_enumeration_recovery_inspect(&info))
    return false;

  return info.result == USB_RECOVERY_READY ||
      info.result == USB_RECOVERY_READY_EP0_ERROR;
}

static bool usb_enumeration_recovery_begin_record(
    usb_managed_device_t *record) {

  if (!record)
    return false;

  usb_recovery_info_t recovery = {0};

  // Inspection must concern this exact record,
  // not whichever device happens to be exposed
  // through the compatibility interface.
  if (!usb_enumeration_recovery_inspect_record(
      record, &recovery))
    return false;

  if (recovery.result != USB_RECOVERY_READY &&
      recovery.result != USB_RECOVERY_READY_EP0_ERROR)
    return false;

  // An actionable recovery requires a confirmed
  // device incarnation and retained hardware slot.
  if (recovery.generation == 0U ||
      recovery.generation != record->device.generation ||
      recovery.slot_id == 0U ||
      recovery.slot_id != record->device.slot_id)
    return false;

  // Once quiesced, this device's slot must not
  // accept further transfers. Its identity remains
  // intact for the lifecycle executor.
  if (!xhci_downstream_quiesce(recovery.slot_id))
    return false;

  record->device.state = USB_DEVICE_DISCONNECTING;
  return true;
}

bool usb_enumeration_recovery_begin_generation(uint32_t generation) {
  return usb_enumeration_recovery_begin_record(
      usb_managed_by_generation(generation));
}

bool usb_enumeration_recovery_begin(void) {
  return usb_enumeration_recovery_begin_record(managed_device);
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
  // Notifications currently originate from the
  // internal USB 2.0 hub. Resolve its identity
  // before selecting the downstream record.
  usb_init_info_t root = {0};

  if (!usb_get_init_info(&root) || root.result != USB_INIT_OK ||
      root.root_port == 0U || root.slot_id == 0U)
    return;

  usb_managed_device_t *record = usb_managed_by_connection(
      root.root_port, root.slot_id, port);

  if (!record)
    return;

  if (record->device.state == USB_DEVICE_DETACHED ||
      record->device.state == USB_DEVICE_DISCONNECTING)
    return;

  // A connection-change notification invalidates
  // the old incarnation even if another device
  // has already connected on the same port.
  bool connection_changed = (change & 0x0001U) != 0U;
  bool connected = (status & 0x0001U) != 0U;
  if (!connection_changed && connected)
    return;

  // Quiesce only the hardware slot owned by this
  // physical connection. Retain its identity
  // for the subsequent lifecycle teardown.
  uint8_t slot_id = record->device.slot_id;
  if (slot_id == 0U || !xhci_downstream_quiesce(slot_id))
    return;

  record->device.state = USB_DEVICE_DISCONNECTING;
}

bool usb_device_finalize_disconnect_generation(
    uint32_t generation, uint8_t slot_id) {

  if (slot_id == 0U)
    return false;

  usb_managed_device_t *record = usb_managed_by_generation(generation);
  if (!record || record->device.state != USB_DEVICE_DISCONNECTING ||
      record->device.slot_id != slot_id ||
      usb_managed_by_slot(slot_id) != record ||
      !xhci_downstream_is_quiesced(slot_id) ||
      !xhci_downstream_is_cleaned(slot_id))
    return false;

  // Controller cleanup is complete. Invalidate
  // the public hardware identity but retain the
  // physical connection and device generation.
  record->device.slot_id = 0U;
  record->device.address = 0U;
  record->device.vendor_id = 0U;
  record->device.product_id = 0U;
  record->device.speed = USB_SPEED_UNKNOWN;
  record->device.state = USB_DEVICE_DETACHED;

  return true;
}

bool usb_device_finalize_disconnect(void) {
  // Compatibility interface for the original
  // single-device lifecycle.
  return usb_device_finalize_disconnect_generation(
      managed_device->device.generation, managed_device->device.slot_id);
}

bool usb_enumeration_rearm_generation(uint32_t generation) {
  usb_managed_device_t *record = usb_managed_by_generation(generation);

  if (!record || !record->attempted ||
      record->device.state != USB_DEVICE_DETACHED ||
      record->device.slot_id != 0U)
    return false;

  // The retained topology must uniquely identify
  // this managed record. An ambiguous connection
  // cannot be considered safe for rearm.
  if (usb_managed_by_connection(record->device.root_port,
      record->device.parent_slot, record->device.parent_port) != record)
    return false;

  // Only this connection must be unowned.
  // Other downstream devices may legitimately
  // retain their own controller resources.
  xhci_connection_ownership_t ownership = XHCI_CONNECTION_UNCERTAIN;

  if (!xhci_downstream_connection_ownership(record->device.root_port,
      record->device.parent_slot, record->device.parent_port, &ownership))
    return false;

  if (ownership != XHCI_CONNECTION_UNOWNED)
    return false;

  // The lifecycle executor has established
  // successful recycling before requesting
  // this operation.
  //
  // Clear the attempt, retaining the previous
  // generation and physical connection.
  record->attempted = false;
  record->enumeration = (usb_enum_info_t){0};

  return true;
}

bool usb_enumeration_rearm(void) {
  // Retain the original compatibility interface.
  return usb_enumeration_rearm_generation( managed_device->device.generation);
}

bool usb_enumeration_rearm_rejected(uint8_t port) {
  // Rejection recovery is associated with a
  // physical connection, not a device generation.
  usb_init_info_t root = {0};

  if (!usb_get_init_info(&root) || root.result != USB_INIT_OK ||
      root.root_port == 0U || root.slot_id == 0U || port == 0U)
    return false;

  usb_managed_device_t *record =
      usb_managed_by_connection(root.root_port, root.slot_id, port);

  if (!record || !record->attempted ||
      record->device.state != USB_DEVICE_DETACHED ||
      record->device.generation != 0U || record->device.slot_id != 0U)
    return false;

  // This recovery path is exclusively for an
  // unsupported-speed rejection before Enable
  // Slot was attempted.
  //
  // An uncertain Enable Slot command outcome
  // must never be discarded by this mechanism.
  if (record->enumeration.result != USB_ENUM_UNSUPPORTED_SPEED ||
      record->enumeration.hub_port != port ||
      record->enumeration.slot_attempted ||
      record->enumeration.slot_confirmed ||
      record->enumeration.stage != USB_ENUM_STAGE_DISCOVER)
    return false;

  // Confirm that this particular connection
  // owns no xHCI resources.
  xhci_connection_ownership_t ownership = XHCI_CONNECTION_UNCERTAIN;

  if (!xhci_downstream_connection_ownership(
      root.root_port, root.slot_id, port, &ownership))
    return false;

  if (ownership != XHCI_CONNECTION_UNOWNED)
    return false;

  // The rejected connection must be physically
  // empty, with its change flags acknowledged.
  usb_hub_port_t status = {0};
  if (usb_hub_port_status(port, &status) != USB_HUB_OK ||
      (status.status & USB_PORT_CONNECTED) != 0U || status.change != 0U)
    return false;

  // No slot was requested, and the physical
  // connection is confirmed empty and unowned.
  // Discard only this enumeration-attempt record.
  *record = (usb_managed_device_t){0};

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

