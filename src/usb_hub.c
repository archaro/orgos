#include <stdint.h>
#include <stdbool.h>

#include "timer.h"
#include "xhci.h"
#include "usb.h"
#include "usb_hub.h"

#define USB_HUB_PORTS 4U

#define USB_RT_HUB_PORT_IN 0xA3U
#define USB_RT_HUB_PORT_OUT 0x23U

#define USB_REQ_GET_STATUS         0x00U
#define USB_REQ_SET_FEATURE        0x03U
#define USB_REQ_GET_CONFIGURATION  0x08U
#define USB_REQ_SET_CONFIGURATION  0x09U
#define USB_REQ_GET_DESCRIPTOR     0x06U
#define USB_DESC_CONFIGURATION     0x02U
#define USB_DESC_INTERFACE         0x04U
#define USB_DESC_ENDPOINT          0x05U
#define USB_DESC_HUB               0x29U
#define USB_HUB_REQ_CLEAR_FEATURE  0x01U
#define USB_HUB_REQ_SET_FEATURE    0x03U
#define USB_HUB_PORT_REQUEST_OUT   0x23U

#define USB_HUB_C_CONNECTION       (1U << 0)
#define USB_HUB_C_ENABLE           (1U << 1)
#define USB_HUB_C_OVERCURRENT      (1U << 3)
#define USB_HUB_C_RESET            (1U << 4)

#define USB_HUB_FEATURE_RESET       4U
#define USB_HUB_CHANGE_CONNECTION  16U
#define USB_HUB_CHANGE_ENABLE      17U
#define USB_HUB_CHANGE_RESET       20U

#define USB_HUB_STATUS_CONNECTION (1U << 0)
#define USB_HUB_STATUS_ENABLE     (1U << 1)
#define USB_HUB_STATUS_OVERCURRENT (1U << 3)
#define USB_HUB_STATUS_RESET      (1U << 4)
#define USB_HUB_STATUS_POWER      (1U << 8)

#define USB_HUB_DEBOUNCE_STEP_MS    25U
#define USB_HUB_DEBOUNCE_STABLE_US  100000ULL
#define USB_HUB_DEBOUNCE_TIMEOUT_US 2000000ULL

#define USB_HUB_RESET_TIMEOUT_MS 500U
#define USB_HUB_RESET_RECOVERY_MS 50U

#define USB_HUB_PORT_POWER 8U
#define USB_HUB_POWER_MIN_DELAY_MS 120U

static bool hub_init_attempted;
static usb_hub_init_info_t hub_init_state;
static usb_hub_scan_info_t hub_scan_state;
static bool hub_scan_valid;

static uint16_t usb_hub_read_u16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool usb_hub_get_init_info(usb_hub_init_info_t *info) {
  if (!info || !hub_init_attempted)
    return false;

  *info = hub_init_state;
  return true;
}

static bool usb_hub_find_interrupt( const uint8_t *data, uint16_t length,
    usb_hub_init_info_t *state) {

  if (!data || !state || length < 9U ||
      data[0] != 9U || data[1] != USB_DESC_CONFIGURATION)
    return false;

  bool hub_interface = false;
  bool found = false;

  for (uint32_t offset = 0U; offset < length;) {
    if (length - offset < 2U)
      return false;

    uint8_t size = data[offset];
    uint8_t type = data[offset + 1U];
    if (size < 2U || size > length - offset)
      return false;

    if (type == USB_DESC_INTERFACE) {
      if (size < 9U)
        return false;
      // USB hub class is 0x09.
      hub_interface = data[offset + 5U] == 0x09U;
    } else if (type == USB_DESC_ENDPOINT && hub_interface) {
      if (size < 7U)
        return false;

      uint8_t address = data[offset + 2U];
      uint8_t attributes = data[offset + 3U];
      uint16_t mps = usb_hub_read_u16(&data[offset + 4U]);
      uint8_t interval = data[offset + 6U];

      // Require an interrupt-IN endpoint.
      if ((address & 0x80U) && (attributes & 0x03U) == 0x03U &&
          (address & 0x0fU) != 0U && mps != 0U && mps <= 64U && interval != 0U) {
        if (found)
          return false;

        state->interrupt_endpoint = address;
        state->interrupt_mps = mps;
        state->interrupt_interval = interval;
        found = true;
      }
    }
    offset += size;
  }

  return found;
}

usb_hub_init_result_t usb_hub_init(usb_hub_init_info_t *info) {
  if (!info)
    return USB_HUB_INIT_NOT_READY;

  if (hub_init_attempted) {
    *info = hub_init_state;
    return hub_init_state.result;
  }

  usb_init_info_t root;
  if (!usb_get_init_info(&root) ||
      root.result != USB_INIT_OK || root.address == 0U)
    return USB_HUB_INIT_NOT_READY;

  hub_init_attempted = true;
  usb_hub_init_info_t state = {0};
  usb_hub_init_result_t result = USB_HUB_INIT_OK;
  xhci_control_info_t transfer = {0};

  // Inspect the existing hub configuration.
  xhci_setup_packet_t setup = {
    .request_type = 0x80U,
    .request = USB_REQ_GET_CONFIGURATION,
    .value = 0U,
    .index = 0U,
    .length = 1U
  };

  xhci_control_result_t status = xhci_control_in(&setup, &transfer);

  if (status != XHCI_CONTROL_OK || transfer.length != 1U) {
    state.detail = (uint32_t)status;
    result = USB_HUB_INIT_CONFIG_FAILED;
    goto complete;
  }

  if (transfer.data[0] == 0U) {
    setup.request_type = 0x00U;
    setup.request = USB_REQ_SET_CONFIGURATION;
    setup.value = 1U;
    setup.length = 0U;

    status = xhci_control_no_data(&setup, &transfer);
    if (status != XHCI_CONTROL_OK) {
      state.detail = (uint32_t)status;
      result = USB_HUB_INIT_CONFIG_FAILED;
      goto complete;
    }
  } else if (transfer.data[0] != 1U) {
    state.detail = transfer.data[0];
    result = USB_HUB_INIT_CONFIG_FAILED;
    goto complete;
  }

  // Verify that configuration 1 is active.
  setup.request_type = 0x80U;
  setup.request = USB_REQ_GET_CONFIGURATION;
  setup.value = 0U;
  setup.length = 1U;

  status = xhci_control_in(&setup, &transfer);
  if (status != XHCI_CONTROL_OK ||
      transfer.length != 1U ||
      transfer.data[0] != 1U) {
    state.detail = (uint32_t)status;
    result = USB_HUB_INIT_CONFIG_FAILED;
    goto complete;
  }

  // Read the USB 2.0 hub descriptor.
  setup.request_type = 0xa0U;
  setup.request = USB_REQ_GET_DESCRIPTOR;
  setup.value = (uint16_t)(USB_DESC_HUB << 8);
  setup.length = 9U;

  status = xhci_control_in(&setup, &transfer);
  if (status != XHCI_CONTROL_OK ||
      transfer.length != 9U ||
      transfer.data[0] != 9U ||
      transfer.data[1] != USB_DESC_HUB) {
    state.detail = (uint32_t)status;
    result = USB_HUB_INIT_DESCRIPTOR_FAILED;
    goto complete;
  }

  uint8_t ports = transfer.data[2];
  uint16_t characteristics = usb_hub_read_u16(&transfer.data[3]);
  uint8_t ttt = (uint8_t)((characteristics >> 5) & 3U);
  uint32_t power_good_ms = (uint32_t)transfer.data[5] * 2U;

  // The present driver supports the Pi 4B's
  // four-port internal USB 2.0 hub.
  if (ports != USB_HUB_PORTS) {
    state.detail = ports;
    result = USB_HUB_INIT_DESCRIPTOR_FAILED;
    goto complete;
  }

  state.ports = ports;

  // Read the configuration descriptor header.
  setup.request_type = 0x80U;
  setup.request = USB_REQ_GET_DESCRIPTOR;
  setup.value = (uint16_t)(USB_DESC_CONFIGURATION << 8);
  setup.length = 9U;

  status = xhci_control_in(&setup, &transfer);
  if (status != XHCI_CONTROL_OK || transfer.length != 9U ||
      transfer.data[0] != 9U || transfer.data[1] != USB_DESC_CONFIGURATION) {
    state.detail = (uint32_t)status;
    result = USB_HUB_INIT_DESCRIPTOR_FAILED;
    goto complete;
  }

  uint16_t config_length = usb_hub_read_u16(&transfer.data[2]);
  if (config_length < 9U || config_length > XHCI_CONTROL_MAX_DATA) {
    state.detail = config_length;
    result = USB_HUB_INIT_DESCRIPTOR_FAILED;
    goto complete;
  }

  // Retrieve and inspect the complete descriptor set.
  setup.length = config_length;

  status = xhci_control_in(&setup, &transfer);
  if (status != XHCI_CONTROL_OK || transfer.length != config_length ||
      !usb_hub_find_interrupt( transfer.data, config_length, &state)) {
    state.detail = (uint32_t)status;
    result = USB_HUB_INIT_DESCRIPTOR_FAILED;
    goto complete;
  }

  // Inform xHCI that this device is a hub.
  xhci_hub_info_t hub = {0};
  xhci_hub_result_t hub_result = xhci_hub_configure(ports, ttt, &hub);
  if (hub_result != XHCI_HUB_OK) {
    state.detail = (uint32_t)hub_result;
    result = USB_HUB_INIT_CONTEXT_FAILED;
    goto complete;
  }

  // Power the downstream ports and verify them.
  usb_hub_port_t power = {0};
  usb_hub_result_t power_result = usb_hub_power_on(power_good_ms, &power);
  if (power_result != USB_HUB_OK) {
    state.detail = (uint32_t)power_result;
    result = USB_HUB_INIT_POWER_FAILED;
    goto complete;
  }

complete:
  state.result = result;
  hub_init_state = state;
  *info = state;

  return result;
}

usb_hub_result_t usb_hub_port_status(uint8_t port, usb_hub_port_t *result) {
  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_port_t){0};
  result->port = port;
  if (port == 0U || port > USB_HUB_PORTS)
    return USB_HUB_INVALID_PORT;

  usb_init_info_t state;
  if (!usb_get_init_info(&state) || state.result != USB_INIT_OK ||
      state.address == 0U)
    return USB_HUB_NOT_READY;

  // USB hub class GET_STATUS for a downstream port:
  // bmRequestType = A3 (IN, Class, Other)
  // bRequest      = 00 (GET_STATUS)
  // wValue        = 0000
  // wIndex        = port number
  // wLength       = 0004
  xhci_setup_packet_t setup = {
    .request_type = USB_RT_HUB_PORT_IN,
    .request = USB_REQ_GET_STATUS,
    .value = 0U,
    .index = port,
    .length = 4U
  };

  xhci_control_info_t transfer;
  xhci_control_result_t status = xhci_control_in( &setup, &transfer);
  result->transfer_result = status;
  result->completion_code = transfer.completion_code;
  if (status != XHCI_CONTROL_OK)
    return USB_HUB_TRANSFER_FAILED;

  if (transfer.length != 4U)
    return USB_HUB_INVALID_RESPONSE;

  const uint8_t *d = transfer.data;
  result->status = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
  result->change = (uint16_t)d[2] | ((uint16_t)d[3] << 8);

  return USB_HUB_OK;
}

bool usb_hub_get_scan(usb_hub_scan_info_t *info) {
  if (!info || !hub_scan_valid)
    return false;

  *info = hub_scan_state;
  return true;
}

usb_hub_result_t usb_hub_scan(usb_hub_scan_info_t *info) {
  if (!info)
    return USB_HUB_NOT_READY;

  *info = (usb_hub_scan_info_t){0};
  usb_hub_init_info_t hub = {0};
  if (!usb_hub_get_init_info(&hub) || hub.result != USB_HUB_INIT_OK)
    return USB_HUB_NOT_READY;

  usb_hub_scan_info_t scan = {0};
  scan.ports = hub.ports;
  for (uint8_t port = 1U; port <= scan.ports; port++) {
    usb_hub_port_t status = {0};
    usb_hub_result_t result =usb_hub_port_status(port, &status);
    if (result != USB_HUB_OK) {
      scan.failed_port = port;
      scan.error = result;
      *info = scan;
      return result;
    }

    uint16_t bit = (uint16_t)(1U << (port - 1U));
    if (status.status & (1U << 0))
      scan.connected |= bit;
    if (status.status & (1U << 1))
      scan.enabled |= bit;
    if (status.change != 0U)
      scan.changed |= bit;
    if ((status.status | status.change) & (1U << 3))
      scan.overcurrent |= bit;
  }

  // Publish only a complete snapshot.
  hub_scan_state = scan;
  hub_scan_valid = true;
  *info = scan;

  return USB_HUB_OK;
}

static bool usb_hub_overcurrent(const usb_hub_port_t *port) {
  return (port->status & USB_HUB_STATUS_OVERCURRENT) != 0U ||
      (port->change & USB_HUB_C_OVERCURRENT) != 0U;
}

usb_hub_result_t usb_hub_power_on(
    uint32_t power_good_ms, usb_hub_port_t *result) {
  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_port_t){0};
  result->port = 1U;
  usb_init_info_t state;
  if (!usb_get_init_info(&state) || state.result != USB_INIT_OK ||
      state.address == 0U)
    return USB_HUB_NOT_READY;

  // Verify that SET_CONFIGURATION(1) has succeeded.
  xhci_setup_packet_t setup = {
    .request_type = 0x80U,
    .request = USB_REQ_GET_CONFIGURATION,
    .value = 0U,
    .index = 0U,
    .length = 1U
  };

  xhci_control_info_t transfer;
  xhci_control_result_t status = xhci_control_in(&setup, &transfer);
  result->transfer_result = status;
  result->completion_code = transfer.completion_code;
  if (status != XHCI_CONTROL_OK)
    return USB_HUB_TRANSFER_FAILED;

  if (transfer.length != 1U || transfer.data[0] != 1U)
    return USB_HUB_NOT_READY;

  // Request PORT_POWER on every downstream port.
  // Even hubs advertising ganged switching may require
  // individual requests for correct power-status reporting.
  for (uint8_t port = 1U; port <= USB_HUB_PORTS; port++) {
    usb_hub_port_t current;
    usb_hub_result_t port_result = usb_hub_port_status(port, &current);

    if (port_result != USB_HUB_OK) {
      *result = current;
      return port_result;
    }

    if (usb_hub_overcurrent(&current)) {
      *result = current;
      return USB_HUB_OVERCURRENT;
    }

    setup.request_type = USB_RT_HUB_PORT_OUT;
    setup.request = USB_REQ_SET_FEATURE;
    setup.value = USB_HUB_PORT_POWER;
    setup.index = port;
    setup.length = 0U;
    status = xhci_control_no_data(&setup, &transfer);
    result->port = port;
    result->transfer_result = status;
    result->completion_code = transfer.completion_code;
    if (status != XHCI_CONTROL_OK)
      return USB_HUB_TRANSFER_FAILED;
  }

  // Honour the hub's advertised power-good interval,
  // but always wait at least 120ms.
  uint32_t delay_ms = power_good_ms;
  if (delay_ms < USB_HUB_POWER_MIN_DELAY_MS)
    delay_ms = USB_HUB_POWER_MIN_DELAY_MS;
  timer_delay_ms(delay_ms);

  // Verify that every downstream port reports power.
  for (uint8_t port = 1U; port <= USB_HUB_PORTS; port++) {
    usb_hub_port_t current;
    usb_hub_result_t port_result = usb_hub_port_status(port, &current);

    if (port_result != USB_HUB_OK) {
      *result = current;
      return port_result;
    }

    *result = current;

    if (usb_hub_overcurrent(&current))
      return USB_HUB_OVERCURRENT;

    if (!(current.status & USB_HUB_STATUS_POWER))
      return USB_HUB_INVALID_RESPONSE;
  }

  return USB_HUB_OK;
}

static usb_hub_result_t usb_hub_port_feature(
    uint8_t port, uint8_t request, uint16_t feature,
    xhci_control_info_t *result) {

  xhci_setup_packet_t setup = {
    .request_type = USB_HUB_PORT_REQUEST_OUT,
    .request = request,
    .value = feature,
    .index = port,
    .length = 0U
  };

  xhci_control_result_t status = xhci_control_no_data(&setup, result);

  if (status != XHCI_CONTROL_OK)
    return USB_HUB_TRANSFER_FAILED;

  return USB_HUB_OK;
}

usb_hub_result_t usb_hub_port_debounce(uint8_t port, usb_hub_port_t *result) {
  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_port_t){0};
  result->port = port;
  uint64_t started = timer_now_us();
  uint64_t stable_since = started;
  bool have_connection = false;
  bool was_connected = false;

  for (;;) {
    usb_hub_port_t current;
    usb_hub_result_t status = usb_hub_port_status(port, &current);
    *result = current;

    if (status != USB_HUB_OK)
      return status;

    if (usb_hub_overcurrent(&current))
      return USB_HUB_OVERCURRENT;
    
    if (!(current.status & USB_HUB_STATUS_POWER))
      return USB_HUB_NOT_POWERED;

    bool connected = (current.status & USB_HUB_STATUS_CONNECTION) != 0U;
    bool changed = (current.change & USB_HUB_C_CONNECTION) != 0U;
    // Acknowledge any connection change. Its presence
    // invalidates the current stability interval.
    if (changed) {
      xhci_control_info_t transfer = {0};
      status = usb_hub_port_feature(port, USB_HUB_REQ_CLEAR_FEATURE,
          USB_HUB_CHANGE_CONNECTION, &transfer);

      if (status != USB_HUB_OK) {
        result->transfer_result = transfer.completion_code == 1U ?
            XHCI_CONTROL_OK : XHCI_CONTROL_TRANSFER_FAILED;
        result->completion_code = transfer.completion_code;
        return status;
      }
    }

    uint64_t now = timer_now_us();
    // Require an uninterrupted 100 ms of stable,
    // connected status with no latched changes.
    if (!have_connection || changed || connected != was_connected) {
      stable_since = now;
    } else if (connected &&
        now - stable_since >= USB_HUB_DEBOUNCE_STABLE_US) {
      return USB_HUB_OK;
    }

    if (now - started >= USB_HUB_DEBOUNCE_TIMEOUT_US)
      return USB_HUB_DEBOUNCE_TIMEOUT;

    was_connected = connected;
    have_connection = true;
    timer_delay_ms(USB_HUB_DEBOUNCE_STEP_MS);
  }
}

usb_hub_result_t usb_hub_port_reset(
    uint8_t port, usb_hub_reset_info_t *result) {

  if (!result)
    return USB_HUB_NOT_READY;

  *result = (usb_hub_reset_info_t){0};
  result->port = port;

  if (port == 0U || port > USB_HUB_PORTS)
    return USB_HUB_INVALID_PORT;

  usb_hub_port_t before;
  usb_hub_result_t status =
      usb_hub_port_status(port, &before);

  if (status != USB_HUB_OK)
    return status;

  result->before_status = before.status;
  result->before_change = before.change;

  if (usb_hub_overcurrent(&before))
    return USB_HUB_OVERCURRENT;

  if (!(before.status & USB_HUB_STATUS_POWER))
    return USB_HUB_NOT_POWERED;

  if (!(before.status & USB_HUB_STATUS_CONNECTION))
    return USB_HUB_DISCONNECTED;

  // SET_FEATURE(PORT_RESET).
  // This resets a downstream hub port, not an xHCI root port.
  xhci_control_info_t transfer;
  status = usb_hub_port_feature(
      port, USB_HUB_REQ_SET_FEATURE,
      USB_HUB_FEATURE_RESET, &transfer);

  result->transfer_result = status == USB_HUB_OK ?
      XHCI_CONTROL_OK : XHCI_CONTROL_TRANSFER_FAILED;
  result->completion_code = transfer.completion_code;

  if (status != USB_HUB_OK)
    return status;

  uint64_t started = timer_now_us();

  for (unsigned int ms = 0; ms < USB_HUB_RESET_TIMEOUT_MS; ms++) {
    // Poll using GET_STATUS. A completed reset should
    // clear PORT_RESET and set C_PORT_RESET.
    timer_delay_ms(1);

    usb_hub_port_t current;
    status = usb_hub_port_status(port, &current);

    if (status != USB_HUB_OK)
      return status;

    result->after_status = current.status;
    result->after_change = current.change;
    result->elapsed_ms = (uint32_t)((timer_now_us() - started) / 1000ULL);

    if (usb_hub_overcurrent(&current))
      return USB_HUB_OVERCURRENT;

    if (!(current.status & USB_HUB_STATUS_CONNECTION))
      return USB_HUB_DISCONNECTED;

    if (current.status & USB_HUB_STATUS_RESET)
      continue;

    if (!(current.change & USB_HUB_C_RESET))
      continue;

    if (!(current.status & USB_HUB_STATUS_ENABLE))
      return USB_HUB_RESET_FAILED;

    // Decode the speed after reset has completed.
    if (current.status & (1U << 10))
      result->speed = 3U;  // High Speed
    else if (current.status & (1U << 9))
      result->speed = 2U;  // Low Speed (xHCI speed ID)
    else
      result->speed = 1U;  // Full Speed

    // Acknowledge C_PORT_RESET.
    status = usb_hub_port_feature(port, USB_HUB_REQ_CLEAR_FEATURE,
        USB_HUB_CHANGE_RESET, &transfer);
    if (status != USB_HUB_OK)
      return status;

    // Acknowledge any connection change observed
    // during this reset sequence.
    if (current.change & USB_HUB_C_CONNECTION) {
      status = usb_hub_port_feature(
          port, USB_HUB_REQ_CLEAR_FEATURE,
          USB_HUB_CHANGE_CONNECTION, &transfer);
      if (status != USB_HUB_OK)
        return status;
    }

    // Verify the resulting port state and change flags.
    // Read the port state after acknowledging reset
    // and any connection change detected during reset.
    status = usb_hub_port_status(port, &current);
    if (status != USB_HUB_OK)
      return status;

    // Do not acknowledge an enable change if the
    // port is no longer in a valid operational state.
    if (!(current.status & USB_HUB_STATUS_ENABLE) ||
        !(current.status & USB_HUB_STATUS_CONNECTION) ||
        (current.status & USB_HUB_STATUS_RESET) ||
        (current.change & (USB_HUB_C_CONNECTION | USB_HUB_C_RESET))) {
      result->final_change = current.change;
      return USB_HUB_RESET_FAILED;
    }

    // Reset may also produce a port-enable change.
    // We have verified that the port is enabled,
    // so this transition can now be acknowledged.
    if (current.change & USB_HUB_C_ENABLE) {
      status = usb_hub_port_feature(port, USB_HUB_REQ_CLEAR_FEATURE,
          USB_HUB_CHANGE_ENABLE, &transfer);

      if (status != USB_HUB_OK) {
        result->transfer_result = XHCI_CONTROL_TRANSFER_FAILED;
        result->completion_code = transfer.completion_code;
        return status;
      }

      // Re-read to verify the acknowledgement and
      // detect any further change during the request.
      status = usb_hub_port_status(port, &current);
      if (status != USB_HUB_OK)
        return status;
    }

    result->final_change = current.change;

    if (usb_hub_overcurrent(&current))
      return USB_HUB_OVERCURRENT;

    if (!(current.status & USB_HUB_STATUS_ENABLE) ||
        !(current.status & USB_HUB_STATUS_CONNECTION) ||
        (current.status & USB_HUB_STATUS_RESET) ||
        (current.change &
            (USB_HUB_C_CONNECTION | USB_HUB_C_ENABLE | USB_HUB_C_RESET)))
      return USB_HUB_RESET_FAILED;

    // USB 2.0 reset recovery: allow at least 10 ms
    // for the device to settle, plus a 40 ms margin.
    // Match Linux's normal hub-port reset recovery.
    timer_delay_ms(USB_HUB_RESET_RECOVERY_MS);

    return USB_HUB_OK;
  }

  return USB_HUB_RESET_TIMEOUT;
}

