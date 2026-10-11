#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "mmio.h"
#include "mailbox.h"

#define MAILBOX_BASE    (MMIO_BASE + 0x0000b880UL)

#define MAILBOX_READ    (MAILBOX_BASE + 0x00)
#define MAILBOX_STATUS  (MAILBOX_BASE + 0x18)
#define MAILBOX_WRITE   (MAILBOX_BASE + 0x20)

#define MAILBOX_FULL    (1U << 31)
#define MAILBOX_EMPTY   (1U << 30)

#define MAILBOX_CHANNEL_PROPERTY 8U
#define MAILBOX_TAG_GET_CLOCK    0x00030002U

#define MAILBOX_TIMEOUT 1000000U

 // A single reusable mailbox message.
 // At this stage OrgOS is single-core, and mailbox calls are synchronous.
static volatile uint32_t message[8] __attribute__((aligned(16)));

static inline void mailbox_barrier(void) {
  __asm__ volatile("dsb sy" ::: "memory");
}

static bool mailbox_call(void) {
  uintptr_t address = (uintptr_t)message;

  // The property mailbox uses a 32-bit address.
  // Our kernel's static memory must be below 4 GiB.
  if (address > UINT32_MAX)
    return false;

  uint32_t request = (uint32_t)address | MAILBOX_CHANNEL_PROPERTY;

  mailbox_barrier();

  // Wait for space in the outgoing mailbox.
  uint32_t timeout = MAILBOX_TIMEOUT;

  while (mmio_read(MAILBOX_STATUS) & MAILBOX_FULL) {
    if (--timeout == 0)
      return false;
  }

  mmio_write(MAILBOX_WRITE, request);

  // Wait for the matching response.
  timeout = MAILBOX_TIMEOUT;

  while (timeout--) {
    if (mmio_read(MAILBOX_STATUS) & MAILBOX_EMPTY)
      continue;

    uint32_t response = mmio_read(MAILBOX_READ);

    if (response == request) {
      mailbox_barrier();
      return message[1] == 0x80000000U;
    }
  }

  return false;
}

bool mailbox_get_clock_rate(uint32_t clock_id, uint32_t *rate) {
  if (rate == NULL)
    return false;

  // Property message:
  // [0] total size in bytes
  // [1] request code
  // [2] tag ID
  // [3] value buffer size
  // [4] request value length
  // [5] clock ID
  // [6] returned clock frequency
  // [7] end tag
  message[0] = sizeof(message);
  message[1] = 0;
  message[2] = MAILBOX_TAG_GET_CLOCK;
  message[3] = 8;
  message[4] = 0;
  message[5] = clock_id;
  message[6] = 0;
  message[7] = 0;

  if (!mailbox_call())
    return false;

  // Check that the tag itself succeeded.
  if (message[2] != MAILBOX_TAG_GET_CLOCK)
    return false;

  if (message[3] < 8)
    return false;

  if (message[4] != 0x80000008U)
    return false;

  if (message[5] != clock_id)
    return false;

  if (message[6] == 0)
    return false;

  *rate = message[6];

  return true;
}

