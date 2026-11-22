#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// White-box host fixture, as used by the MIDI verifier tests. Exercise actual
// command handlers with CPU-backed memory and deterministic ownership mocks.
#include "../src/console_dma.c"

static _Alignas(PAGE_SIZE) uint8_t memory[2U * PAGE_SIZE];
static char output[4096];
static size_t output_length;
static unsigned int used_pages, stats_calls, allocations, frees, returns;
static dma_direction_t handed_off[32];
static unsigned int handoffs;
static int bad_counter;
static bool stats_unavailable, allocation_failed, return_failed;

void console_puts(const char *text) {
  size_t length = strlen(text);
  assert(output_length + length < sizeof(output));
  memcpy(output + output_length, text, length + 1U);
  output_length += length;
}

void console_putu64(uint64_t value) {
  char text[32];
  snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
  console_puts(text);
}

bool page_get_stats(page_stats_t *stats) {
  stats_calls++;
  if (stats_unavailable && stats_calls == 2U)
    return false;
  *stats = (page_stats_t){.total_pages = 1024U, .reserved_pages = 8U,
    .allocated_pages = used_pages, .free_pages = 1016U - used_pages};
  if (stats_calls == 2U) {
    switch (bad_counter) {
      case 0: stats->total_pages++; break;
      case 1: stats->reserved_pages++; break;
      case 2: stats->allocated_pages++; break;
      case 3: stats->free_pages++; break;
      default: break;
    }
  }
  return true;
}

bool cache_get_info(cache_info_t *info) {
  *info = (cache_info_t){.data_line_size = 64U};
  return true;
}

bool dma_alloc(uint64_t size, uint64_t alignment, dma_buffer_t *buffer) {
  allocations++;
  assert(used_pages == 0U && size <= sizeof(memory) && alignment == PAGE_SIZE);
  if (allocation_failed)
    return false;
  used_pages = (unsigned int)((size + PAGE_SIZE - 1U) / PAGE_SIZE);
  *buffer = (dma_buffer_t){.virtual = memory, .size = size,
    .pages = used_pages, .owner = DMA_OWNER_CPU};
  return true;
}

bool dma_free(dma_buffer_t *buffer) {
  frees++;
  if (!buffer || buffer->pages == 0U || buffer->owner != DMA_OWNER_CPU)
    return false;
  assert(buffer->virtual == memory && used_pages == buffer->pages);
  used_pages = 0U;
  *buffer = (dma_buffer_t){0};
  return true;
}

bool dma_begin_device(dma_buffer_t *buffer, dma_direction_t direction) {
  if (!buffer || buffer->pages == 0U || buffer->owner != DMA_OWNER_CPU ||
      (direction != DMA_TO_DEVICE && direction != DMA_FROM_DEVICE &&
       direction != DMA_BIDIRECTIONAL))
    return false;
  assert(handoffs < sizeof(handed_off) / sizeof(handed_off[0]));
  handed_off[handoffs++] = direction;
  buffer->owner = DMA_OWNER_DEVICE;
  buffer->direction = direction;
  return true;
}

bool dma_end_device(dma_buffer_t *buffer) {
  if (!buffer || buffer->pages == 0U || buffer->owner != DMA_OWNER_DEVICE)
    return false;
  returns++;
  if (return_failed)
    return false; // Preserve ownership: the command must neither free nor continue.
  buffer->owner = DMA_OWNER_CPU;
  return true;
}

static void fixture(void) {
  output[0] = '\0';
  output_length = 0U;
  used_pages = stats_calls = allocations = frees = returns = handoffs = 0U;
  bad_counter = -1;
  stats_unavailable = allocation_failed = return_failed = false;
}

static void test_normal_commands(void) {
  fixture();
  cmd_dma("owner");
  assert(strstr(output, "DMA ownership self-test:\n"));
  assert(strstr(output, "ALL TESTS PASSED\n"));
  assert(stats_calls == 2U && used_pages == 0U && allocations == 1U);
  assert(handoffs == 2U && handed_off[0] == DMA_FROM_DEVICE &&
      handed_off[1] == DMA_BIDIRECTIONAL && returns == 2U);

  const char *commands[] = {"cache", "edge"};
  const dma_direction_t order[] = {DMA_TO_DEVICE, DMA_FROM_DEVICE, DMA_BIDIRECTIONAL};
  for (unsigned int command = 0U; command < 2U; command++) {
    fixture();
    cmd_dma(commands[command]);
    assert(strstr(output, "ALL TESTS PASSED\n"));
    assert(stats_calls == 2U && used_pages == 0U);
    assert(allocations == (command ? 2U : 1U));
    assert(handoffs == (command ? 18U : 3U) && returns == handoffs);
    for (unsigned int i = 0U; i < handoffs; i++)
      assert(handed_off[i] == order[i % 3U]);
    if (command == 0U)
      assert(strstr(output, "  CPU to device  : PASS\n"
          "  Device to CPU  : PASS\n  Bidirectional  : PASS\n"));
  }
}

static void test_accounting_failures(void) {
  const char *commands[] = {"owner", "cache", "edge"};
  for (unsigned int command = 0U; command < 3U; command++) {
    for (int fault = 0; fault < 5; fault++) {
      fixture();
      bad_counter = fault < 4 ? fault : -1;
      stats_unavailable = fault == 4;
      cmd_dma(commands[command]);
      assert(strstr(output, "FAIL: Page accounting mismatch.\n"));
      assert(strstr(output, "Result: TEST FAILED\n"));
      assert(stats_calls == 2U && used_pages == 0U);
    }
    fixture();
    allocation_failed = true;
    cmd_dma(commands[command]);
    assert(strstr(output, "FAIL: Allocation failed.\n"));
    assert(handoffs == 0U && used_pages == 0U);
  }
}

static void test_failed_handoff_and_syntax(void) {
  fixture();
  return_failed = true;
  cmd_dma("cache");
  assert(strstr(output, "FAIL: Buffer remains device-owned.\n"));
  assert(strstr(output, "Result: TEST FAILED\n"));
  assert(handoffs == 1U && returns == 1U && used_pages == 1U && frees == 0U);
  fixture();
  cmd_dma("cache extra");
  assert(strcmp(output, "Usage: dma [owner | cache | edge]\n") == 0);
  assert(stats_calls == 0U && allocations == 0U && handoffs == 0U);
}

int main(void) {
  test_normal_commands();
  test_accounting_failures();
  test_failed_handoff_and_syntax();
  puts("Console DMA diagnostics: PASS");
  return 0;
}
