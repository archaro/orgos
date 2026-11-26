#pragma once

// Fixed diagnostic VA policy, shared by C and preprocessed assembly.
// L1 slot 32 is outside RAM (slots 0..7) and PCIe MMIO (slot 24).
#define EL0_STACK_VA  0x800000000
#define EL0_STACK_TOP 0x800001000
