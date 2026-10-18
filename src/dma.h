#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "page.h"

typedef struct {
  void *virtual;
  paddr_t physical;
  uint64_t bus;
  uint64_t size;
  unsigned int pages;
} dma_buffer_t;

bool dma_alloc(uint64_t size, uint64_t alignment, dma_buffer_t *buffer);
bool dma_free(dma_buffer_t *buffer);
