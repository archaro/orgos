#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "timer.h"
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
#define MAILBOX_TAG_NOTIFY_XHCI_RESET 0x00030058U

#define MAILBOX_DEFAULT_TIMEOUT_US 1000000ULL
#define MAILBOX_XHCI_TIMEOUT_US   10000000ULL

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

static mailbox_result_t mailbox_call(
    mailbox_diagnostic_t *diagnostic, uint64_t timeout_us) {

  if (diagnostic)
    *diagnostic = (mailbox_diagnostic_t){0};

  if (mailbox_poisoned)
    return MAILBOX_RESULT_UNAVAILABLE;

  uintptr_t address = (uintptr_t)message;
  if ((address & 0x0fU) || address > UINT32_MAX)
    return MAILBOX_RESULT_UNAVAILABLE;

  uint32_t request = (uint32_t)address | MAILBOX_CHANNEL_PROPERTY;
  if (!mailbox_release_to_firmware())
    return MAILBOX_RESULT_CACHE_ERROR;

  uint64_t started = timer_now_us();
  while (mmio_read(MAILBOX_STATUS) & MAILBOX_FULL) {
    if (timer_now_us() - started >= timeout_us)
      return MAILBOX_RESULT_WRITE_TIMEOUT;
  }
  mmio_write(MAILBOX_WRITE, request);
  started = timer_now_us();
  while (timer_now_us() - started < timeout_us) {
    if (mmio_read(MAILBOX_STATUS) & MAILBOX_EMPTY)
      continue;
    uint32_t response = mmio_read(MAILBOX_READ);
    if (response != request)
      continue;
    if (!mailbox_acquire_from_firmware()) {
      mailbox_poisoned = true;
      return MAILBOX_RESULT_CACHE_ERROR;
    }
    if (diagnostic) {
      diagnostic->response_received = true;
      diagnostic->response_code = message[1];
      diagnostic->tag = message[2];
      diagnostic->tag_length = message[4];
    }
    if (message[1] != 0x80000000U)
      return MAILBOX_RESULT_FIRMWARE_ERROR;
    return MAILBOX_RESULT_OK;
  }
  // The firmware may still be using the buffer.
  // Do not permit another request to overwrite it.
  mailbox_poisoned = true;
  return MAILBOX_RESULT_READ_TIMEOUT;
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

  if (mailbox_call(NULL, MAILBOX_DEFAULT_TIMEOUT_US) != MAILBOX_RESULT_OK)
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

  if (mailbox_call(NULL, MAILBOX_DEFAULT_TIMEOUT_US) != MAILBOX_RESULT_OK)
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

  if (mailbox_call(NULL, MAILBOX_DEFAULT_TIMEOUT_US) != MAILBOX_RESULT_OK)
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

bool mailbox_notify_xhci_reset(uint32_t device_address,
    mailbox_diagnostic_t *diagnostic) {

  if (!diagnostic)
    return false;

  *diagnostic = (mailbox_diagnostic_t){0};

  message[0] = 7U * sizeof(uint32_t);
  message[1] = 0;
  message[2] = MAILBOX_TAG_NOTIFY_XHCI_RESET;
  message[3] = 4;
  message[4] = 0;
  message[5] = device_address;
  message[6] = 0;

  mailbox_result_t result = mailbox_call(diagnostic, MAILBOX_XHCI_TIMEOUT_US);
  diagnostic->result = result;

  if (result != MAILBOX_RESULT_OK)
    return false;

  if (diagnostic->tag != MAILBOX_TAG_NOTIFY_XHCI_RESET) {
    diagnostic->result = MAILBOX_RESULT_BAD_TAG;
    return false;
  }

  // A successful property response has bit 31 set.
  // The returned length may be zero for this tag.
  if (!(diagnostic->tag_length & 0x80000000U) ||
      (diagnostic->tag_length & 0x7fffffffU) > 4U) {
    diagnostic->result = MAILBOX_RESULT_BAD_RESPONSE;
    return false;
  }

  return true;
}
