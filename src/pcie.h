#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t status;
  uint32_t revision;

  bool root_complex;
  bool link_up;

  uint16_t root_vendor __attribute__((aligned(4)));
  uint16_t root_device;

  uint8_t secondary_bus;
  uint8_t subordinate_bus;

  bool endpoint_present;
  uint16_t endpoint_vendor;
  uint16_t endpoint_device;
  uint32_t endpoint_class;
  uint32_t endpoint_bar0;
  uint32_t endpoint_bar1;
} pcie_info_t;

void pcie_probe(pcie_info_t *info);

typedef enum {
  PCIE_INIT_OK,
  PCIE_INIT_ALREADY_UP,
  PCIE_INIT_NOT_ROOT_COMPLEX,
  PCIE_INIT_LINK_TIMEOUT
} pcie_init_result_t;

pcie_init_result_t pcie_init_link(void);

