#pragma once

// Private C/assembly contract for the bounded H2.6f6 execution island.
#define C_CALL_RESULT 0
#define C_CALL_BEFORE_SP 8
#define C_CALL_AFTER_SP 16
#define C_CALL_RETURN 24
#define C_CALL_FAILURES 32
#define C_CALL_NZCV_BEFORE 40
#define C_CALL_NZCV_AFTER 48
#define C_CALL_SIZE 56

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint64_t pc[3], sp[3], local[3], local_value[3];
  uint64_t caller[2];
  uint64_t ro_address, rw_address, ro_value[2], written;
} mmu_c_evidence_t;
typedef struct {
  uint64_t guard_before, value, guard_after;
} mmu_c_storage_t;
typedef struct {
  uint64_t result, before_sp, after_sp, low_return, failures;
  uint64_t nzcv_before, nzcv_after;
} mmu_c_call_t;
#define C_OFFSET(member, offset) \
  _Static_assert(offsetof(mmu_c_call_t, member) == offset, "C call " #member)
C_OFFSET(result, C_CALL_RESULT);
C_OFFSET(before_sp, C_CALL_BEFORE_SP);
C_OFFSET(after_sp, C_CALL_AFTER_SP);
C_OFFSET(low_return, C_CALL_RETURN);
C_OFFSET(failures, C_CALL_FAILURES);
C_OFFSET(nzcv_before, C_CALL_NZCV_BEFORE);
C_OFFSET(nzcv_after, C_CALL_NZCV_AFTER);
_Static_assert(sizeof(mmu_c_call_t) == C_CALL_SIZE, "C call size");
#undef C_OFFSET

uint64_t mmu_c_outer(mmu_c_evidence_t *e, const volatile uint64_t *ro,
                     volatile mmu_c_storage_t *rw);
void mmu_c_call(uint64_t entry, mmu_c_evidence_t *e,
    const volatile uint64_t *ro, volatile mmu_c_storage_t *rw,
    mmu_c_call_t *call);
#endif
