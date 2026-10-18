#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAILBOX_CLOCK_UART 2U
#define MAILBOX_CLOCK_ARM  3U
#define MAILBOX_CLOCK_CORE 4U

bool mailbox_get_board_revision(uint32_t *revision);
bool mailbox_get_clock_rate(uint32_t clock_id, uint32_t *rate);
bool mailbox_get_arm_memory(uint32_t *base, uint32_t *size);

typedef enum {
  MAILBOX_RESULT_OK,
  MAILBOX_RESULT_UNAVAILABLE,
  MAILBOX_RESULT_WRITE_TIMEOUT,
  MAILBOX_RESULT_READ_TIMEOUT,
  MAILBOX_RESULT_CACHE_ERROR,
  MAILBOX_RESULT_FIRMWARE_ERROR,
  MAILBOX_RESULT_BAD_TAG,
  MAILBOX_RESULT_BAD_RESPONSE
} mailbox_result_t;

typedef struct {
  mailbox_result_t result;
  uint32_t response_code;
  uint32_t tag;
  uint32_t tag_length;
  bool response_received;
} mailbox_diagnostic_t;

bool mailbox_notify_xhci_reset(uint32_t device_address,
    mailbox_diagnostic_t *diagnostic);
