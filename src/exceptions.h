#pragma once

// Shared by preprocessed assembly and C. All offsets are bytes.
#define EXCEPTION_FRAME_X30     240
#define EXCEPTION_FRAME_SP      248
#define EXCEPTION_FRAME_ELR     256
#define EXCEPTION_FRAME_SPSR    264
#define EXCEPTION_FRAME_ESR     272
#define EXCEPTION_FRAME_FAR     280
#define EXCEPTION_FRAME_VECTOR  288
#define EXCEPTION_FRAME_SIZE    304
#define EXCEPTION_TEST_SVC      0x21

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

// Owned by the entry stub on the EL1 stack, valid only during dispatch.
// sp is the interrupted EL1h SP, recorded for diagnostics (read-only).
// x[], elr and spsr are restored by the common return path. ESR/FAR and
// vector describe entry and are not restored. No FP/SIMD state is saved;
// all reachable handlers must be built with -mgeneral-regs-only.
// This frame currently supports returning to EL1h only, not EL0/EL1t.
typedef struct {
  uint64_t x[31];
  uint64_t sp;
  uint64_t elr;
  uint64_t spsr;
  uint64_t esr;
  uint64_t far;
  uint64_t vector;
  uint64_t reserved;
} exception_frame_t;

#define CHECK_FRAME(field, offset) \
  _Static_assert(offsetof(exception_frame_t, field) == (offset), \
                 "exception frame offset: " #field)
CHECK_FRAME(x, 0);
CHECK_FRAME(x[30], EXCEPTION_FRAME_X30);
CHECK_FRAME(sp, EXCEPTION_FRAME_SP);
CHECK_FRAME(elr, EXCEPTION_FRAME_ELR);
CHECK_FRAME(spsr, EXCEPTION_FRAME_SPSR);
CHECK_FRAME(esr, EXCEPTION_FRAME_ESR);
CHECK_FRAME(far, EXCEPTION_FRAME_FAR);
CHECK_FRAME(vector, EXCEPTION_FRAME_VECTOR);
#undef CHECK_FRAME
_Static_assert(sizeof(exception_frame_t) == EXCEPTION_FRAME_SIZE,
               "exception frame size");

void exception_dispatch(exception_frame_t *frame);
// EL1 diagnostic: mismatch bits 0..30 = GPRs, 31 = SP, 32 = NZCV, 33 = DAIF.
uint64_t exception_test(void);
__attribute__((noreturn))
void exception_panic(uint64_t vector, uint64_t esr, uint64_t elr,
                     uint64_t far, uint64_t spsr);
#endif
