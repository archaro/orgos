#pragma once

#define HIGH_PROBE_RESULT 0
#define HIGH_PROBE_PC 8
#define HIGH_PROBE_SP 16
#define HIGH_PROBE_BEFORE_SP 24
#define HIGH_PROBE_AFTER_SP 32
#define HIGH_PROBE_RETURN 40
#define HIGH_PROBE_FAILURES 48
#define HIGH_PROBE_SIZE 56

#ifndef __ASSEMBLER__
#include <stddef.h>
#include "address.h"
typedef struct {
  uint64_t result;
  vaddr_t observed_pc, observed_sp, before_sp, after_sp, low_return;
  uint64_t failures;
} mmu_high_probe_result_t;
#define HIGH_OFFSET(field, offset) \
  _Static_assert(offsetof(mmu_high_probe_result_t, field) == offset, \
                 "higher-half probe layout: " #field)
HIGH_OFFSET(result, HIGH_PROBE_RESULT);
HIGH_OFFSET(observed_pc, HIGH_PROBE_PC);
HIGH_OFFSET(observed_sp, HIGH_PROBE_SP);
HIGH_OFFSET(before_sp, HIGH_PROBE_BEFORE_SP);
HIGH_OFFSET(after_sp, HIGH_PROBE_AFTER_SP);
HIGH_OFFSET(low_return, HIGH_PROBE_RETURN);
HIGH_OFFSET(failures, HIGH_PROBE_FAILURES);
#undef HIGH_OFFSET
_Static_assert(sizeof(mmu_high_probe_result_t) == HIGH_PROBE_SIZE,
               "higher-half probe size");
#endif
