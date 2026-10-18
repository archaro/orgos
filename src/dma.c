#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "dma.h"
#include "page.h"
#include "cache.h"
#include "pcie.h"
#include "string.h"

// Current firmware-advertised inbound DMA limit.
// Physical range [0, 3 GiB).
//
// This constant will eventually come from the
// decoded PCIe dma-ranges information.
#define DMA_PHYSICAL_LIMIT 0xC0000000ULL

bool dma_alloc(uint64_t size, uint64_t alignment, dma_buffer_t *buffer) {

  if (!buffer)
    return false;

  // Preserve a predictable empty descriptor if
  // validation or allocation fails.
  *buffer = (dma_buffer_t){0};

  if (size == 0 || alignment == 0)
    return false;

  // Only power-of-two alignments are supported.
  if (alignment & (alignment - 1U))
    return false;

  // Page allocation already guarantees 4 KiB
  // alignment, satisfying smaller requirements.
  if (alignment < PAGE_SIZE)
    alignment = PAGE_SIZE;

  // Prevent overflow when rounding up.
  if (size > UINT64_MAX - (PAGE_SIZE - 1U))
    return false;

  uint64_t pages64 = (size + PAGE_SIZE - 1U) / PAGE_SIZE;
  if (pages64 == 0 || pages64 > UINT32_MAX)
    return false;

  // This also rejects alignments that cannot
  // be represented by the page allocator API.
  uint64_t alignment_pages64 = alignment / PAGE_SIZE;
  if (alignment_pages64 > UINT32_MAX)
    return false;

  unsigned int pages = (unsigned int)pages64;
  unsigned int alignment_pages = (unsigned int)alignment_pages64;
  paddr_t physical;

  if (!page_alloc_run_below(pages, alignment_pages,
          DMA_PHYSICAL_LIMIT, &physical))
    return false;

  uint64_t allocated_size = pages64 * PAGE_SIZE;
  uint64_t bus;
  // Validate the entire allocated extent, not
  // merely the requested byte count.
  if (!pcie_phys_to_dma(
          physical, allocated_size, &bus)) {
    page_free_run(physical, pages);
    return false;
  }

  // OrgOS currently identity-maps all physical RAM.
  // This conversion must change if virtual mappings
  // become independent of physical addresses.
  void *virtual = (void *)(uintptr_t)physical;

  // Initialise the complete allocation, including
  // the unused portion of its final page.
  //
  // This is CPU-side initialisation only. It does
  // not yet make the buffer coherent for device DMA.
  memset(virtual, 0, (size_t)allocated_size);
  buffer->virtual = virtual;
  buffer->physical = physical;
  buffer->bus = bus;
  buffer->size = size;
  buffer->pages = pages;
  buffer->owner = DMA_OWNER_CPU;
  buffer->direction = DMA_TO_DEVICE;
  return true;
}

bool dma_free(dma_buffer_t *buffer) {
  if (!buffer || buffer->pages == 0 ||
      buffer->owner != DMA_OWNER_CPU)
    return false;

  // Release the physical pages. If this fails,
  // retain the descriptor for diagnosis.
  if (!page_free_run(buffer->physical, buffer->pages))
    return false;

  *buffer = (dma_buffer_t){0};
  return true;
}

static bool dma_direction_valid(dma_direction_t direction) {
  return direction == DMA_TO_DEVICE ||
      direction == DMA_FROM_DEVICE ||
      direction == DMA_BIDIRECTIONAL;
}

// Synchronise a complete DMA allocation.
//
// The allocation owns whole physical pages, so
// no cache line is shared with another allocation.
static bool dma_cache_sync(dma_buffer_t *buffer, bool for_device) {

  if (!buffer || !buffer->virtual ||
      buffer->pages == 0)
    return false;

  cache_info_t info;
  if (!cache_get_info(&info))
    return false;

  size_t line = info.data_line_size;
  if (line == 0 || (line & (line - 1U)) != 0 ||
      line > PAGE_SIZE || (PAGE_SIZE % line) != 0)
    return false;

  if (((uintptr_t)buffer->virtual & (line - 1U)) != 0)
    return false;

  size_t length = (size_t)buffer->pages * PAGE_SIZE;
  if (for_device) {
    switch (buffer->direction) {
      case DMA_TO_DEVICE:
        return cache_clean(buffer->virtual, length);
      case DMA_FROM_DEVICE:
      case DMA_BIDIRECTIONAL:
        return cache_clean_invalidate(
            buffer->virtual, length);
    }
  } else {
    switch (buffer->direction) {
      case DMA_TO_DEVICE:
        // No device writes to invalidate.
        // The preceding handoff and cache clean
        // already established the necessary ordering.
        __asm__ volatile("dsb sy" ::: "memory");
        return true;
      case DMA_FROM_DEVICE:
      case DMA_BIDIRECTIONAL:
        return cache_invalidate(buffer->virtual, length);
    }
  }

  return false;
}

// Transfer logical ownership from the CPU to the device.
// Cache maintenance will be added in 4.4b.
bool dma_begin_device(dma_buffer_t *buffer, dma_direction_t direction) {
  if (!buffer || buffer->pages == 0 ||
      buffer->owner != DMA_OWNER_CPU ||
      !dma_direction_valid(direction))
    return false;

  // The direction determines which cache
  // operation is required before device access.
  // Do not change ownership if maintenance fails.
  dma_direction_t previous_direction = buffer->direction;
  buffer->direction = direction;
  if (!dma_cache_sync(buffer, true)) {
    buffer->direction = previous_direction;
    return false;
  }

  // Cache maintenance has completed at the
  // Point of Coherency. Device access may now
  // begin once the driver submits the operation.
  buffer->owner = DMA_OWNER_DEVICE;

  return true;
}

// Return logical ownership to the CPU.
// The caller must establish that the device has
// completed its access before calling this.
bool dma_end_device(dma_buffer_t *buffer) {
  if (!buffer || buffer->pages == 0 ||
      buffer->owner != DMA_OWNER_DEVICE)
    return false;

  // The caller must already have established
  // hardware completion before invoking this.
  // Synchronise memory before allowing the CPU
  // to access the buffer again.
  if (!dma_cache_sync(buffer, false))
    return false;

  buffer->owner = DMA_OWNER_CPU;

  return true;
}
