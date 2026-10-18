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

typedef enum {
  PCIE_MAP_OK,
  PCIE_MAP_LINK_DOWN,
  PCIE_MAP_WRONG_DEVICE,
  PCIE_MAP_BAD_BAR,
  PCIE_MAP_VERIFY_FAILED
} pcie_map_result_t;

pcie_map_result_t pcie_map_xhci(void);

typedef struct {
  uint32_t misc_ctrl;
  uint32_t bar2_lo;
  uint32_t bar2_hi;
  uint32_t bar3_lo;
  uint32_t endian_ctrl;
} pcie_dma_info_t;

bool pcie_dma_probe(pcie_dma_info_t *info);
bool pcie_dma_init(void);
bool pcie_phys_to_dma(uint64_t physical,
    uint64_t length, uint64_t *dma_address);

bool pcie_xhci_ready(void);
bool pcie_xhci_get_command(uint16_t *command);

typedef struct {
  uint32_t class_revision;
  uint32_t command_status;
  uint32_t header;
  uint32_t capability_pointer;
  bool capabilities_present;
  bool pm_present;
  uint8_t pm_offset;
  uint16_t pm_capabilities;
  uint16_t pm_control;
} pcie_xhci_diagnostic_t;

bool pcie_xhci_diagnose(pcie_xhci_diagnostic_t *result);
bool pcie_xhci_get_firmware_version(uint32_t *version);
bool pcie_xhci_disable_bus_master(void);
bool pcie_xhci_enable_bus_master(void);
bool pcie_xhci_bar_valid(void);
