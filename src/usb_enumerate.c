
#include <stdbool.h>
#include <stdint.h>

#include "usb.h"
#include "usb_hub.h"
#include "usb_enumerate.h"
#include "xhci.h"

#define USB_DESC_DEVICE 0x01U
#define USB_DESC_CONFIGURATION 0x02U
#define USB_DESC_HUB 0x29U

#define USB_REQ_GET_DESCRIPTOR 0x06U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_REQ_SET_CONFIGURATION 0x09U

#define USB_PORT_CONNECTED 0x0001U
#define USB_PORT_POWERED 0x0100U

static bool enumeration_attempted;
static usb_enum_info_t enumeration_state;

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

static usb_enum_result_t configure_hub(usb_enum_info_t *state) {
  xhci_control_info_t info;

  if (!control_in(false, 0x80U, USB_REQ_GET_CONFIGURATION,
      0U, 1U, &info)) {
    state->detail = info.completion_code;
    return USB_ENUM_HUB_CONFIG_FAILED;
  }

  if (info.data[0] == 0U) {
    xhci_setup_packet_t setup = {
      .request_type = 0x00U,
      .request = USB_REQ_SET_CONFIGURATION,
      .value = 1U,
      .index = 0U,
      .length = 0U
    };

    if (xhci_control_no_data(&setup, &info) != XHCI_CONTROL_OK) {
      state->detail = info.completion_code;
      return USB_ENUM_HUB_CONFIG_FAILED;
    }
  } else if (info.data[0] != 1U) {
    state->detail = info.data[0];
    return USB_ENUM_HUB_CONFIG_FAILED;
  }

  if (!control_in(false, 0x80U, USB_REQ_GET_CONFIGURATION,
      0U, 1U, &info) || info.data[0] != 1U) {
    state->detail = info.completion_code;
    return USB_ENUM_HUB_CONFIG_FAILED;
  }

  if (!control_in(false, 0xa0U, USB_REQ_GET_DESCRIPTOR,
      (uint16_t)(USB_DESC_HUB << 8), 9U, &info) ||
      info.data[0] != 9U || info.data[1] != USB_DESC_HUB) {
    state->detail = info.completion_code;
    return USB_ENUM_HUB_CONTEXT_FAILED;
  }

  uint8_t ports = info.data[2];
  uint16_t characteristics = read_u16(&info.data[3]);
  uint8_t ttt = (uint8_t)((characteristics >> 5) & 3U);
  // Current USB hub driver supports precisely four ports.
  if (ports != 4U) {
    state->detail = ports;
    return USB_ENUM_HUB_CONTEXT_FAILED;
  }

  state->hub_ports = ports;
  xhci_hub_info_t hub;
  xhci_hub_result_t result = xhci_hub_configure(ports, ttt, &hub);
  if (result != XHCI_HUB_OK) {
    state->detail = (uint32_t)result;
    return USB_ENUM_HUB_CONTEXT_FAILED;
  }

  usb_hub_port_t power;
  usb_hub_result_t power_result = usb_hub_power_on(&power);

  if (power_result != USB_HUB_OK) {
    state->detail = (uint32_t)power_result;
    return USB_ENUM_HUB_POWER_FAILED;
  }

  return USB_ENUM_OK;
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

  usb_hub_reset_info_t reset;
  usb_hub_result_t result = usb_hub_port_reset(found, &reset);
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

  // Some enumeration operations are not safe to retry
  // following an uncertain command completion.
  enumeration_attempted = true;
  usb_enum_info_t state = {0};
  usb_enum_result_t result = configure_hub(&state);

  if (result != USB_ENUM_OK)
    goto complete;

  result = discover_port(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  result = address_device(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  result = read_device(&state);
  if (result != USB_ENUM_OK)
    goto complete;

  result = read_configuration(&state);

complete:
  state.result = result;
  enumeration_state = state;
  *info = state;

  return result;
}

