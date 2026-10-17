#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "mmio.h"
#include "cache.h"
#include "mailbox.h"

#define MAILBOX_BASE    (MMIO_BASE + 0x0000b880UL)

#define MAILBOX_READ    (MAILBOX_BASE + 0x00)
#define MAILBOX_STATUS  (MAILBOX_BASE + 0x18)
#define MAILBOX_WRITE   (MAILBOX_BASE + 0x20)

#define MAILBOX_FULL    (1U << 31)
#define MAILBOX_EMPTY   (1U << 30)

#define MAILBOX_CHANNEL_PROPERTY 8U
#define MAILBOX_TAG_GET_BOARD_REVISION 0x00010002U
#define MAILBOX_TAG_GET_CLOCK    0x00030002U
#define MAILBOX_TAG_GET_ARM_MEMORY 0x00010005U

#define MAILBOX_TIMEOUT 1000000U

// Firmware property mailbox buffer.
// The Cortex-A72 reports 64-byte D-cache lines.
// Reserve one complete, individually aligned line,
// preventing cache maintenance from touching
// unrelated kernel variables.
// Although storage occupies 64 bytes, our current
// property messages use only 28 or 32 bytes.
#define MAILBOX_BUFFER_BYTES 64U
#define MAILBOX_MESSAGE_WORDS 8U
static volatile uint32_t message[MAILBOX_BUFFER_BYTES / 4U]
    __attribute__((aligned(64)));
static bool mailbox_poisoned;

static inline void mailbox_barrier(void) {
  __asm__ volatile("dsb sy" ::: "memory");
}

// Check that cache maintenance cannot affect
// memory outside our dedicated mailbox buffer.
static bool mailbox_cache_layout_valid(void) {
  cache_info_t info;

  if (!cache_get_info(&info))
    return false;

  return info.data_line_size != 0 &&
      info.data_line_size <= MAILBOX_BUFFER_BYTES &&
      MAILBOX_BUFFER_BYTES % info.data_line_size == 0;
}

// Transfer ownership of the buffer from the
// CPU to the firmware.
// Clean and invalidate rather than merely clean:
// the CPU must not retain dirty data that could
// subsequently overwrite the firmware's response.
static bool mailbox_release_to_firmware(void) {
  if (!mailbox_cache_layout_valid())
    return false;

  // While D-cache is disabled, no cache maintenance
  // is necessary. This also keeps early firmware
  // queries independent of MMU initialization.
  uint64_t sctlr;
  __asm__ volatile( "mrs %0, sctlr_el1" : "=r"(sctlr));

  if (sctlr & (1ULL << 2)) {
    if (!cache_clean_invalidate((const void *)(uintptr_t)message, sizeof(message)))
      return false;
  }

  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

// Return ownership to the CPU after receiving
// the firmware's completion notification.
// Invalidate before examining any response fields.
static bool mailbox_acquire_from_firmware(void) {
  uint64_t sctlr;
  __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

  if (sctlr & (1ULL << 2)) {
    if (!cache_invalidate((const void *)(uintptr_t)message, sizeof(message)))
      return false;
  }
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

static bool mailbox_call(void) {
  if (mailbox_poisoned)
    return false;
  uintptr_t address = (uintptr_t)message;

  // Property mailbox addresses must be
  // aligned to 16 bytes, leaving the low
  // four bits available for the channel.
  if ((address & 0x0fU) != 0)
    return false;

  // The firmware property mailbox currently
  // uses a 32-bit address field.
  // Our kernel-owned static storage is in
  // low RAM and identity-mapped.
  if (address > UINT32_MAX)
    return false;

  uint32_t request =
      (uint32_t)address | MAILBOX_CHANNEL_PROPERTY;

  // Before submitting the request, make
  // all CPU-written message fields visible
  // to the firmware.
  if (!mailbox_release_to_firmware())
    return false;

  // Wait for space in the outgoing mailbox.
  uint32_t timeout = MAILBOX_TIMEOUT;
  while (mmio_read(MAILBOX_STATUS) & MAILBOX_FULL) {
    if (--timeout == 0)
      return false;
  }

  // The buffer now belongs to the firmware.
  // Do not access it until completion.
  mmio_write(MAILBOX_WRITE, request);

  // Wait for the matching response.
  timeout = MAILBOX_TIMEOUT;

  while (timeout--) {
    if (mmio_read(MAILBOX_STATUS) & MAILBOX_EMPTY)
      continue;

    uint32_t response = mmio_read(MAILBOX_READ);
    if (response != request)
      continue;

    // The firmware has finished using the
    // shared buffer.
    // Invalidate any stale CPU cache contents
    // before reading the response status.
    if (!mailbox_acquire_from_firmware())
      return false;

    return message[1] == 0x80000000U;
  }

  // A timeout after submission is potentially
  // dangerous: the firmware might still be
  // processing this request.
  // The current driver has no recovery protocol,
  // so do not reuse its buffer after such a
  // timeout until the driver or system is reset.
  mailbox_poisoned = true;
  return false;
}

// Retrieve the board revision from Raspberry Pi firmware.
// Property tag 0x00010002:
//   [0] Message size
//   [1] Request / response code
//   [2] Tag identifier
//   [3] Value buffer size (4 bytes)
//   [4] Tag request / response length
//   [5] Board revision code
//   [6] End tag
// This reuses the existing mailbox buffer and
// synchronous mailbox_call() implementation.
bool mailbox_get_board_revision(uint32_t *revision) {
  if (!revision)
    return false;

  message[0] = 7U * sizeof(uint32_t);
  message[1] = 0;
  message[2] = MAILBOX_TAG_GET_BOARD_REVISION;
  message[3] = 4;
  message[4] = 0;
  message[5] = 0;
  message[6] = 0;

  if (!mailbox_call())
    return false;

  if (message[2] != MAILBOX_TAG_GET_BOARD_REVISION)
    return false;

  if (message[3] < 4)
    return false;

  if (message[4] != 0x80000004U)
    return false;

  *revision = message[5];
  return true;
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
  message[0] = MAILBOX_MESSAGE_WORDS * sizeof(uint32_t);
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

// Retrieve the ARM memory region from the firmware.
// Property tag 0x00010005 (Get ARM Memory):
//   [0] Total message size
//   [1] Request / response status
//   [2] Tag identifier
//   [3] Value buffer size (8 bytes)
//   [4] Tag request / response length
//   [5] ARM memory base address
//   [6] ARM memory size in bytes
//   [7] End tag
// The message buffer is the same statically allocated,
// 16-byte-aligned buffer used by our clock queries.
// This reports firmware-assigned ARM memory, not
// necessarily every usable or unreserved RAM page.
bool mailbox_get_arm_memory(uint32_t *base, uint32_t *size) {
  if (!base || !size)
    return false;

  message[0] = MAILBOX_MESSAGE_WORDS * sizeof(uint32_t);
  message[1] = 0;
  message[2] = MAILBOX_TAG_GET_ARM_MEMORY;
  message[3] = 8;
  message[4] = 0;
  message[5] = 0;
  message[6] = 0;
  message[7] = 0;

  if (!mailbox_call())
    return false;

  // Check that the firmware returned the expected
  // tag and a successful eight-byte response.
  if (message[2] != MAILBOX_TAG_GET_ARM_MEMORY)
    return false;

  if (message[3] < 8)
    return false;

  if (message[4] != 0x80000008U)
    return false;

  if (message[6] == 0)
    return false;

  // Both values fit into uint32_t according to
  // the mailbox property interface.
  *base = message[5];
  *size = message[6];

  return true;
}

