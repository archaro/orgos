
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
static usb_enum_info_t enumeration_state;
static usb_device_t current_device;
static bool device_record_valid;

static uint16_t read_u16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool control_in(bool downstream, uint8_t request_type,
    uint8_t request, uint16_t value, uint16_t length,
    xhci_control_info_t *info) {

  xhci_setup_packet_t setup = {
    .request_type = request_type,
    .request = request,
    .value = value,
    .index = 0U,
    .length = length
  };

  xhci_control_result_t result = downstream ?
      xhci_downstream_control_in(&setup, info) :
      xhci_control_in(&setup, info);

  return result == XHCI_CONTROL_OK &&
      info->length == length;
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

static usb_enum_result_t address_device(usb_enum_info_t *state) {
  xhci_slot_info_t slot;
  xhci_slot_result_t slot_result = xhci_enable_downstream_slot(&slot);

  if (slot_result != XHCI_SLOT_OK) {
    state->detail = (uint32_t)slot_result;
    return USB_ENUM_SLOT_FAILED;
  }

  state->slot_id = slot.slot_id;
  xhci_device_info_t device;
  xhci_device_result_t device_result =
      xhci_downstream_prepare(state->hub_port, state->speed, &device);

  if (device_result != XHCI_DEVICE_OK) {
    state->detail = (uint32_t)device_result;
    return USB_ENUM_PREPARE_FAILED;
  }

  xhci_address_info_t address;
  xhci_address_result_t address_result = xhci_address_downstream(&address);

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
  if (!control_in(true, 0x80U, USB_REQ_GET_DESCRIPTOR,
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
      xhci_downstream_ep0_update(info.data[7], &update);

  if (result != XHCI_EP0_UPDATE_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_MPS_FAILED;
  }

  if (!control_in(true, 0x80U, USB_REQ_GET_DESCRIPTOR,
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

  if (!control_in(true, 0x80U, USB_REQ_GET_DESCRIPTOR,
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

  if (!control_in(true, 0x80U, USB_REQ_GET_DESCRIPTOR,
      (uint16_t)(USB_DESC_CONFIGURATION << 8), length, &info)) {
    state->detail = info.completion_code;
    return USB_ENUM_CONFIG_FAILED;
  }

  if (!usb_midi_parse_config(info.data, info.length,
      &state->midi))
    return USB_ENUM_PARSE_FAILED;

  return USB_ENUM_OK;
}

static usb_enum_result_t select_configuration( usb_enum_info_t *state) {

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
      xhci_downstream_control_no_data(&setup, &info);
  if (result != XHCI_CONTROL_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_SET_CONFIG_FAILED;
  }

  if (!control_in(true, 0x80U, USB_REQ_GET_CONFIGURATION, 0U, 1U, &info)) {
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
          &interface_setup, &interface_info);

  if (interface_result != XHCI_CONTROL_OK) {
    state->detail = (uint32_t)interface_result |
        ((uint32_t)interface_info.completion_code << 8);
    return USB_ENUM_SET_INTERFACE_FAILED;
  }

  return USB_ENUM_OK;
}

static usb_enum_result_t configure_bulk( usb_enum_info_t *state) {

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
  current_device.generation = 1U;
  // Enumeration will now assign a controller slot
  // and USB address to the detected device.
  current_device.state = USB_DEVICE_ENUMERATING;

  result = address_device(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  current_device.slot_id = state.slot_id;
  current_device.address = state.address;

  result = read_device(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  current_device.vendor_id = state.vendor_id;
  current_device.product_id = state.product_id;
  state.quirks = usb_midi_quirks_lookup(state.vendor_id, state.product_id);
  result = read_configuration(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  result = select_configuration(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  result = configure_bulk(&state);
  if (result != USB_ENUM_OK)
    goto complete;
  current_device.state = USB_DEVICE_CONFIGURED;

  if (!usb_midi_quirks_apply(state.quirks, &state.detail))
    result = USB_ENUM_MIDI_QUIRK_FAILED;

  if (result == USB_ENUM_OK)
    current_device.state = USB_DEVICE_ACTIVE;

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
