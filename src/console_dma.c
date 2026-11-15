#include <stddef.h>
#include <stdarg.h>

#include "format.h"
#include "console.h"
#include "console_args.h"
#include "board.h"
#include "mailbox.h"
#include "timer.h"
#include "uart.h"
#include "fdt.h"
#include "physmem.h"
#include "irq.h"
#include "mmu.h"
#include "page.h"
#include "cache.h"
#include "dma.h"
#include "pcie.h"
#include "xhci.h"
#include "usb.h"
#include "usb_hub.h"
#include "usb_enumerate.h"
#include "usb_midi.h"
#include "usb_midi_rx.h"
#include "midi_output.h"
#include "usb_midi_test.h"

#include "console_commands.h"

static void cmd_dma_owner_test(void) {
  dma_buffer_t buffer;
  page_stats_t before, after;
  unsigned int failed = 0;

  console_puts("DMA ownership self-test:\n");
  if (!page_get_stats(&before)) {
    console_puts("FAIL: Page allocator unavailable.\n");
    return;
  }
  if (!dma_alloc(4096, 4096, &buffer)) {
    console_puts("FAIL: Allocation failed.\n");
    return;
  }
  // 1. Newly allocated memory belongs to the CPU.
  if (buffer.owner != DMA_OWNER_CPU) {
    console_puts("FAIL: Initial ownership.\n");
    failed++;
  } else {
    console_puts("  Initial CPU ownership : PASS\n");
  }
  // 2. Cannot return memory from a device which
  // does not currently own it.
  if (dma_end_device(&buffer)) {
    console_puts("FAIL: Invalid return accepted.\n");
    failed++;
  } else {
    console_puts("  Invalid return        : PASS\n");
  }
  // 3. Reject an invalid transfer direction.
  if (dma_begin_device(&buffer, (dma_direction_t)99) ||
      buffer.owner != DMA_OWNER_CPU) {
    console_puts("FAIL: Invalid direction accepted.\n");
    failed++;
  } else {
    console_puts("  Direction validation  : PASS\n");
  }
  // 4. Transfer ownership to the device.
  if (!dma_begin_device(&buffer, DMA_FROM_DEVICE) ||
      buffer.owner != DMA_OWNER_DEVICE ||
      buffer.direction != DMA_FROM_DEVICE) {
    console_puts("FAIL: Device handoff.\n");
    failed++;
  } else {
    console_puts("  Device handoff        : PASS\n");
  }
  // 5. Reject repeated handoff.
  if (dma_begin_device(&buffer, DMA_TO_DEVICE)) {
    console_puts("FAIL: Double handoff accepted.\n");
    failed++;
  } else {
    console_puts("  Double handoff        : PASS\n");
  }
  // 6. Device-owned memory must not be freed.
  if (dma_free(&buffer)) {
    console_puts("FAIL: Device-owned buffer freed.\n");
    failed++;
  } else {
    console_puts("  Free protection       : PASS\n");
  }
  // 7. Return ownership to the CPU.
  if (!dma_end_device(&buffer) ||
      buffer.owner != DMA_OWNER_CPU) {
    console_puts("FAIL: CPU handoff.\n");
    failed++;
  } else {
    console_puts("  CPU handoff           : PASS\n");
  }
  // 8. A second return must be rejected.
  if (dma_end_device(&buffer)) {
    console_puts("FAIL: Double return accepted.\n");
    failed++;
  } else {
    console_puts("  Double return         : PASS\n");
  }
  // 9. Test another direction on the same buffer.
  if (!dma_begin_device(&buffer, DMA_BIDIRECTIONAL) ||
      buffer.direction != DMA_BIDIRECTIONAL || !dma_end_device(&buffer)) {
    console_puts("FAIL: Bidirectional transition.\n");
    failed++;
  } else {
    console_puts("  Bidirectional cycle   : PASS\n");
  }
  // 10. Release the buffer and check accounting.
  if (!dma_free(&buffer)) {
    console_puts("FAIL: Final release.\n");
    failed++;
  } else if (dma_free(&buffer)) {
    console_puts("FAIL: Double free accepted.\n");
    failed++;
  } else {
    console_puts("  Final release         : PASS\n");
  }
  if (!page_get_stats(&after) ||
      before.total_pages != after.total_pages ||
      before.reserved_pages != after.reserved_pages ||
      before.allocated_pages != after.allocated_pages ||
      before.free_pages != after.free_pages) {
    console_puts("FAIL: Page accounting mismatch.\n");
    failed++;
  } else {
    console_puts("  Page accounting       : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(failed == 0 ?
      "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_dma_cache_test(void) {
  dma_buffer_t buffer;
  page_stats_t before, after;
  unsigned int failed = 0;

  console_puts("DMA cache synchronisation self-test:\n");
  if (!page_get_stats(&before)) {
    console_puts("FAIL: Page allocator unavailable.\n");
    return;
  }
  if (!dma_alloc(4096, 4096, &buffer)) {
    console_puts("FAIL: Allocation failed.\n");
    return;
  }

  volatile uint8_t *memory = (volatile uint8_t *)buffer.virtual;

  // Exercise each DMA direction.
  static const dma_direction_t directions[] = {
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL
  };

  static const char *names[] = {
    "CPU to device",
    "Device to CPU",
    "Bidirectional"
  };

  for (unsigned int direction = 0; direction < 3; direction++) {
    // CPU owns the buffer here.
    // Write a distinctive pattern to every byte.
    for (unsigned int i = 0; i < PAGE_SIZE; i++)
      memory[i] = (uint8_t)(i ^ (i >> 6) ^ (direction * 37U) ^ 0xA5U);
    bool ok = dma_begin_device( &buffer, directions[direction]);
    // No actual DMA occurs in this test. We
    // immediately simulate completion by returning
    // ownership without touching the buffer.
    if (ok)
      ok = dma_end_device(&buffer);
    if (ok) {
      // CPU ownership has been restored.
      for (unsigned int i = 0; i < PAGE_SIZE; i++) {
        uint8_t expected = (uint8_t)(
            i ^ (i >> 6) ^ (direction * 37U) ^ 0xA5U);
        if (memory[i] != expected) {
          ok = false;
          break;
        }
      }
    }
    console_puts("  ");
    console_puts(names[direction]);
    console_puts("  : ");
    console_puts(ok ? "PASS\n" : "FAIL\n");
    if (!ok) {
      failed++;
      // Do not access a potentially device-owned
      // buffer or continue testing.
      break;
    }
  }
  if (buffer.owner == DMA_OWNER_CPU) {
    if (!dma_free(&buffer)) {
      console_puts("FAIL: Release failed.\n");
      failed++;
    }
  } else {
    console_puts("FAIL: Buffer remains device-owned.\n");
    failed++;
  }
  if (!page_get_stats(&after) ||
      before.total_pages != after.total_pages ||
      before.reserved_pages != after.reserved_pages ||
      before.allocated_pages != after.allocated_pages ||
      before.free_pages != after.free_pages) {
    console_puts("FAIL: Page accounting mismatch.\n");
    failed++;
  } else {
    console_puts("  Page accounting : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(failed == 0 ?
      "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

static void cmd_dma_edge_test(void) {
  static const uint64_t sizes[] = {100, 4097};
  static const dma_direction_t directions[] = {
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_BIDIRECTIONAL
  };
  page_stats_t before, after;
  unsigned int failed = 0;
  console_puts("DMA edge-case self-test:\n");
  if (!page_get_stats(&before)) {
    console_puts("FAIL: Page allocator unavailable.\n");
    return;
  }
  cache_info_t cache;
  if (!cache_get_info(&cache)) {
    console_puts("FAIL: Cache information unavailable.\n");
    return;
  }
  console_puts("  Data cache line size : ");
  console_putu64(cache.data_line_size);
  console_puts(" bytes\n");
  bool alignment_ok = cache.data_line_size != 0 &&
      cache.data_line_size <= PAGE_SIZE &&
      (cache.data_line_size & (cache.data_line_size - 1U)) == 0 &&
      (PAGE_SIZE % cache.data_line_size) == 0;
  if (!alignment_ok) {
    console_puts("FAIL: Unsupported cache-line size.\n");
    return;
  }
  // Invalid arguments must be rejected.
  dma_buffer_t empty = {0};
  bool invalid_ok =
      !dma_begin_device(NULL, DMA_TO_DEVICE) && !dma_end_device(NULL) &&
      !dma_free(NULL) && !dma_begin_device(&empty, DMA_TO_DEVICE) &&
      !dma_end_device(&empty) && !dma_free(&empty);
  console_puts("  Invalid descriptors   : ");
  console_puts(invalid_ok ? "PASS\n" : "FAIL\n");
  if (!invalid_ok)
    failed++;
  for (unsigned int test = 0; test < 2; test++) {
    dma_buffer_t buffer = {0};
    uint64_t requested = sizes[test];
    if (!dma_alloc(requested, 4096, &buffer)) {
      console_puts("FAIL: Allocation failed.\n");
      failed++;
      break;
    }
    bool ok = true;
    uint64_t capacity = (uint64_t)buffer.pages * PAGE_SIZE;
    if (buffer.size != requested || capacity < requested ||
        ((uintptr_t)buffer.virtual % cache.data_line_size) != 0 ||
        buffer.owner != DMA_OWNER_CPU)
      ok = false;
    volatile uint8_t *memory = (volatile uint8_t *)buffer.virtual;
    // Include unused padding in the test.
    // Cache maintenance covers the entire allocation.
    for (uint64_t i = 0; i < capacity; i++)
      memory[i] = (uint8_t)(i ^ (i >> 8) ^ 0x5AU);
    for (unsigned int cycle = 0; cycle < 9 && ok; cycle++) {
      dma_direction_t direction = directions[cycle % 3];
      if (!dma_begin_device(&buffer, direction)) {
        ok = false;
        break;
      }
      // Reject repeated handoff while device-owned.
      if (dma_begin_device(&buffer, direction))
        ok = false;
      // Reject freeing a device-owned allocation.
      if (dma_free(&buffer))
        ok = false;
      // No actual device DMA is performed.
      if (!dma_end_device(&buffer)) {
        ok = false;
        break;
      }
      // Reject returning ownership twice.
      if (dma_end_device(&buffer))
        ok = false;
      // Verify that cache maintenance preserved
      // every byte, including padding.
      for (uint64_t i = 0; i < capacity; i++) {
        uint8_t expected = (uint8_t)(i ^ (i >> 8) ^ 0x5AU);
        if (memory[i] != expected) {
          ok = false;
          break;
        }
      }
    }
    console_puts("  ");
    console_putu64(requested);
    console_puts("-byte buffer cycles : ");
    console_puts(ok ? "PASS\n" : "FAIL\n");
    if (!ok)
      failed++;
    if (buffer.owner != DMA_OWNER_CPU || !dma_free(&buffer)) {
      console_puts("FAIL: Buffer cleanup.\n");
      failed++;
      // Do not risk reusing this allocation.
      break;
    }
  }
  if (!page_get_stats(&after) || before.total_pages != after.total_pages ||
      before.reserved_pages != after.reserved_pages ||
      before.allocated_pages != after.allocated_pages ||
      before.free_pages != after.free_pages) {
    console_puts("FAIL: Page accounting mismatch.\n");
    failed++;
  } else {
    console_puts("  Page accounting      : PASS\n");
  }
  console_puts("\nResult: ");
  console_puts(failed == 0 ?
      "ALL TESTS PASSED\n" : "TEST FAILED\n");
}

void cmd_dma(const char *args) {
  if (console_arg_word(&args, "cache") && console_arg_end(args)) {
    cmd_dma_cache_test();
    return;
  }
  if (console_arg_word(&args, "owner") && console_arg_end(args)) {
    cmd_dma_owner_test();
    return;
  }
  if (console_arg_word(&args, "edge") && console_arg_end(args)) {
    cmd_dma_edge_test();
    return;
  }
  console_puts("Usage: dma [owner | cache | edge]\n");
}

