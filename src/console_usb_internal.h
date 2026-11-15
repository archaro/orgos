#pragma once

// Internal USB console command handlers.
// Not part of the USB subsystem API.

void cmd_usb_state(void);
void cmd_usb_test_ep0match(void);
void cmd_usb_test_noops(void);
void cmd_usb_events(void);
void cmd_usb_hubports(void);
void cmd_usb_devices(void);
void cmd_usb_enumerate(const char *args);
void cmd_usb_enumstate(const char *args);
void cmd_usb_recover_status(const char *args);
void cmd_usb_recover(const char *args);
void cmd_usb_transfers(const char *args);
void cmd_usb_lifecycle_status(const char *args);
void cmd_usb_lifecycle_port(const char *args);
void cmd_usb_topology(bool detail);
void cmd_usb_changes(void);
void cmd_usb_changes_dump(void);
void cmd_usb_hubirq(void);
void cmd_usb_auto_status(void);
void cmd_usb_auto_ports(void);
void cmd_usb_resources(void);
void cmd_usb_auto(const char *args);
void cmd_usb_test_midiout(const char *args);
void cmd_usb_test_midiasync(const char *args);
void cmd_usb_test_midiin(const char *args);
void cmd_usb_test_midiwrap(const char *args);
