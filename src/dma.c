#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "dma.h"
#include "page.h"
#include "cache.h"
#include "pcie.h"
#include "string.h"
#include "console.h"
#include "dma_io.h"

// Current firmware-advertised inbound DMA limit.
// Physical range [0, 3 GiB).
//
// This constant will eventually come from the
// decoded PCIe dma-ranges information.
#define DMA_PHYSICAL_LIMIT 0xC0000000ULL

static bool dma_allocation_geometry(uint64_t size, uint64_t alignment,
    unsigned int *pages, unsigned int *alignment_pages) {
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

  *pages = (unsigned int)pages64;
  *alignment_pages = (unsigned int)alignment_pages64;
  return true;
}

static void dma_initialize(paddr_t physical, uint64_t bus, uint64_t size,
    unsigned int pages, dma_buffer_t *buffer) {
  uint64_t allocated_size = (uint64_t)pages * PAGE_SIZE;
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
}

bool dma_alloc(uint64_t size, uint64_t alignment, dma_buffer_t *buffer) {
  if (!buffer)
    return false;
  *buffer = (dma_buffer_t){0};

  unsigned int pages, alignment_pages;
  if (!dma_allocation_geometry(size, alignment, &pages, &alignment_pages))
    return false;

  paddr_t physical;
  if (!page_alloc_run_below(pages, alignment_pages,
          DMA_PHYSICAL_LIMIT, &physical))
    return false;

  uint64_t bus;
  if (!pcie_phys_to_dma(physical, (uint64_t)pages * PAGE_SIZE, &bus)) {
    if (!page_free_run(physical, pages)) {
      // An allocator invariant failed. Preserve the exact extent in the
      // diagnostic rather than concealing an allocation leak.
      console_puts("DMA allocation rollback failed: physical=");
      console_puthex64(physical);
      console_puts(" pages=");
      console_putu32(pages);
      console_puts("\n");
    }
    return false;
  }

  dma_initialize(physical, bus, size, pages, buffer);
  return true;
}

// TODO: Stage 9 - High-address PCIe DMA integrity testing.
//
// Targeted page allocation and DMA buffer allocation are implemented,
// but actual DMA integrity above low physical memory is unverified.
//
// Build a diagnostic xHCI transfer path which uses dma_alloc_at()
// buffers directly, without copying payloads into private buffers.
// Verify both DMA directions at selected physical addresses below
// the 3 GiB software allocation ceiling.
//
// Use an independent USB test fixture (provisionally an RP2040 Pico)
// with Bulk IN/OUT endpoints and separate UART verification. Test
// known patterns, cache synchronization, address boundaries, and
// repeated transfers. Never release buffers with uncertain DMA
// ownership.

bool dma_alloc_at(paddr_t physical, uint64_t size,
    uint64_t alignment, dma_buffer_t *buffer) {
  if (!buffer)
    return false;
  *buffer = (dma_buffer_t){0};

  unsigned int pages, alignment_pages;
  if (!dma_allocation_geometry(size, alignment, &pages, &alignment_pages))
    return false;

  uint64_t effective_alignment = (uint64_t)alignment_pages * PAGE_SIZE;
  uint64_t allocated_size = (uint64_t)pages * PAGE_SIZE;
  if ((physical & (effective_alignment - 1U)) ||
      physical >= DMA_PHYSICAL_LIMIT ||
      allocated_size > DMA_PHYSICAL_LIMIT - physical)
    return false;

  // Validate the complete rounded extent before acquiring any pages.
  uint64_t bus;
  if (!pcie_phys_to_dma(physical, allocated_size, &bus))
    return false;
  if (!page_claim_run(physical, pages))
    return false;

  // Initialisation cannot report a recoverable failure. No translation or
  // other fallible operation follows the successful exact claim.
  dma_initialize(physical, bus, size, pages, buffer);
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
        dma_barrier();
        return true;
      case DMA_FROM_DEVICE:
      case DMA_BIDIRECTIONAL:
        return cache_invalidate(buffer->virtual, length);
    }
  }

  return false;
}

// Transfer logical ownership from the CPU to the device.
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

bool dma_end_device(dma_buffer_t *buffer) {
  if (!buffer || buffer->pages == 0 ||
      buffer->owner != DMA_OWNER_DEVICE)
    return false;

  // Hardware completion must be established by
  // the caller before returning ownership to the CPU.
  if (!dma_cache_sync(buffer, false))
    return false;

  buffer->owner = DMA_OWNER_CPU;

  return true;
}
