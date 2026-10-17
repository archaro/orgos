#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAILBOX_CLOCK_UART 2U
#define MAILBOX_CLOCK_ARM  3U
#define MAILBOX_CLOCK_CORE 4U

bool mailbox_get_board_revision(uint32_t *revision);
bool mailbox_get_clock_rate(uint32_t clock_id, uint32_t *rate);
bool mailbox_get_arm_memory(uint32_t *base, uint32_t *size);
