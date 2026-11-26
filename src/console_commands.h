#pragma once

// Top-level handlers dispatched by console.c.
// Their internal helpers remain private to each module.
void cmd_pci(const char *args);
void cmd_dma(const char *args);
void cmd_usb(const char *args);
void cmd_fdt(const char *args);
void cmd_physmem(const char *args);
void cmd_page(const char *args);
void cmd_cache(const char *args);
void cmd_el0ctx(const char *args);

void cmd_kmap(const char *args);
