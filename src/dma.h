#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "page.h"

typedef enum {
  DMA_OWNER_CPU,
  DMA_OWNER_DEVICE
} dma_owner_t;

typedef enum {
  DMA_TO_DEVICE,
  DMA_FROM_DEVICE,
  DMA_BIDIRECTIONAL
} dma_direction_t;

typedef struct {
  void *virtual;
  paddr_t physical;
  uint64_t bus;
  uint64_t size;
  unsigned int pages;
  dma_owner_t owner;
  dma_direction_t direction;
} dma_buffer_t;

bool dma_alloc(uint64_t size, uint64_t alignment, dma_buffer_t *buffer);
bool dma_free(dma_buffer_t *buffer);
bool dma_begin_device(dma_buffer_t *buffer, dma_direction_t direction);
bool dma_end_device(dma_buffer_t *buffer);
