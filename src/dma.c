#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "dma.h"
#include "page.h"
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

  return true;
}

bool dma_free(dma_buffer_t *buffer) {
  if (!buffer || buffer->pages == 0)
    return false;

  // Release the physical pages. If this fails,
  // retain the descriptor for diagnosis.
  if (!page_free_run(buffer->physical, buffer->pages))
    return false;

  *buffer = (dma_buffer_t){0};
  return true;
}
