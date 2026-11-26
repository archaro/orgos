#include "el0_context.h"
#include "el0_test.h"
#include "mmu.h"
#include "el0_stack.h"
#include "string.h"

extern char __el0_stack_bottom[], __el0_stack_top[];
extern char __el0_stack1_bottom[], __el0_stack1_top[];
extern const char el0_context_program[], el0_context_program_end[];
extern const char el0_context_program_resume[], el0_context_exit_resume[];
extern const char el0_context_fault_program[], el0_context_fault_read[];
extern const char el0_context_stack_fault_program[], el0_context_stack_read[];
extern volatile uint64_t el0_test_kernel_canary;
extern uint64_t el0_context_switch(void);

// Exactly two persistent contexts, no allocation. Neither holds a pointer
// into an expired EL1 transition stack.
typedef struct {
  const mmu_address_space_t *space;
  exception_frame_t saved;
  uint64_t expected_daif;
  el0_context_info_t info;
} el0_slot_t;
static el0_slot_t contexts[EL0_CONTEXT_COUNT];

// Transient ownership for the single explicit foreground transition.
static el0_slot_t *active;
static exception_frame_t *kernel_return;

static uintptr_t stack_kernel_top(unsigned int id) {
  return (uintptr_t)(id == 0U ? __el0_stack_top : __el0_stack1_top);
}

static uintptr_t stack_kernel_bottom(unsigned int id) {
  return (uintptr_t)(id == 0U ? __el0_stack_bottom : __el0_stack1_bottom);
}

// Trusted CPU alias into this slot's static backing, never its saved SP.
static uintptr_t frame_kernel_base(const el0_slot_t *ctx) {
  return stack_kernel_top((unsigned int)(ctx - contexts)) -
      EL0_CONTEXT_STACK_BYTES;
}

static uint64_t pattern(const el0_slot_t *ctx, unsigned int reg) {
  return 0x9abc567812340100ULL +
      ((uint64_t)(ctx - contexts) << 32) + reg;
}

bool el0_context_owns_stack(unsigned int id) {
  if (id >= EL0_CONTEXT_COUNT)
    return false;
  const el0_slot_t *ctx = &contexts[id];
  return ctx->info.state == EL0_CONTEXT_READY ||
      ctx->info.state == EL0_CONTEXT_RUNNING ||
      ctx->info.state == EL0_CONTEXT_YIELDED ||
      ctx->info.state == EL0_CONTEXT_FAULTED;
}

static bool start_context(unsigned int id, unsigned int variant) {
  if (id >= EL0_CONTEXT_COUNT || active || el0_context_owns_stack(id) ||
      el0_test_active() || !el0_test_mappings_ready())
    return false;

  const mmu_address_space_t *space = mmu_context_address_space(id);
  const mmu_address_space_t *kernel = mmu_kernel_address_space();
  if (!space || !kernel || mmu_current_root() != kernel->root_physical)
    return false;
  el0_slot_t *ctx = &contexts[id];
  ctx->space = space;
  memset((void *)stack_kernel_bottom(id), 0, 4096U);
  ctx->expected_daif = 0;
  ctx->saved = (exception_frame_t){0};
  for (unsigned int i = 2; i < 31U; i++)
    ctx->saved.x[i] = pattern(ctx, i);
  const char *program = variant >= 2U ? el0_context_stack_fault_program :
      variant == 1U ? el0_context_fault_program : el0_context_program;
  ctx->saved.elr = (uintptr_t)program;
  ctx->saved.sp = ctx->saved.sp_el0 = EL0_STACK_TOP;
  if (variant >= 2U) {
    uint64_t *kernel_frame = (uint64_t *)frame_kernel_base(ctx);
    kernel_frame[EL0_CONTEXT_TARGET_OFFSET / 8] =
        stack_kernel_bottom(variant == 2U ? 1U - id : id);
  }
  ctx->saved.spsr = 0xa0000000ULL; // EL0t. DAIF added on first switch.
  ctx->info = (el0_context_info_t){
    .state = EL0_CONTEXT_READY, .stack_ok = true, .fault_test = variant != 0U,
    .cross_stack_test = variant == 2U, .own_alias_test = variant == 3U,
    .virtual_stack = EL0_STACK_VA,
    // Identity is retained only for this trusted kernel backing alias.
    .physical_stack = (paddr_t)stack_kernel_bottom(id),
    .address_space_root = space->root_physical
  };
  return true;
}

bool el0_context_start(unsigned int id) {
  return start_context(id, 0);
}

bool el0_context_start_fault_test(unsigned int id) {
  return start_context(id, 1);
}

bool el0_context_start_stack_fault_test(unsigned int id) {
  return start_context(id, 2);
}

bool el0_context_start_alias_fault_test(unsigned int id) {
  return start_context(id, 3);
}

bool el0_context_reset(unsigned int id) {
  if (id >= EL0_CONTEXT_COUNT || active)
    return false;
  contexts[id] = (el0_slot_t){0};
  return true;
}

bool el0_context_get_info(unsigned int id, el0_context_info_t *result) {
  if (!result)
    return false;
  *result = (el0_context_info_t){0};
  if (id >= EL0_CONTEXT_COUNT)
    return false;
  *result = contexts[id].info;
  return true;
}

static bool fault_far_valid(uint64_t esr) {
  // Cortex-A72 TRM, ESR_EL1/FAR_EL1 register descriptions:
  // https://documentation-service.arm.com/static/5e7b6c287158f500bd5bfb61
  // FAR is meaningful for aborts, PC alignment and watchpoints.
  // For aborts/watchpoints FnV (bit 10) marks an unavailable fault address.
  unsigned int ec = (unsigned int)((esr >> 26) & 0x3fU);
  if (ec == 0x22U)
    return true;
  return (ec == 0x20U || ec == 0x24U || ec == 0x34U) &&
      (esr & (1ULL << 10)) == 0;
}

bool el0_context_get_fault(unsigned int id, el0_context_fault_t *fault) {
  if (!fault)
    return false;
  *fault = (el0_context_fault_t){0};
  if (id >= EL0_CONTEXT_COUNT)
    return false;
  const el0_slot_t *ctx = &contexts[id];
  if (ctx->info.state != EL0_CONTEXT_FAULTED)
    return false;
  fault->context_id = id;
  fault->frame = ctx->saved;
  fault->far_valid = fault_far_valid(ctx->saved.esr);
  return true;
}

// Internal assembly callback; active was selected by the foreground caller.
exception_frame_t *el0_context_enter(exception_frame_t *kernel) {
  el0_slot_t *ctx = active;
  if (!ctx || mmu_current_root() != ctx->space->root_physical)
    exception_panic(4, 0, (uintptr_t)el0_context_enter, 0, 5);
  ctx->info.entered_root = mmu_current_root();
  if (ctx->info.state == EL0_CONTEXT_READY) {
    ctx->expected_daif = kernel->spsr & 0x3c0U;
    ctx->saved.spsr |= ctx->expected_daif;
  }
  kernel_return = kernel;
  ctx->info.state = EL0_CONTEXT_RUNNING;
  ctx->info.resumptions++;
  return &ctx->saved;
}

bool el0_context_handle_exception(exception_frame_t *frame) {
  el0_slot_t *ctx = active;
  if (!ctx || ctx->info.state != EL0_CONTEXT_RUNNING || !kernel_return ||
      frame->vector != 8U || (frame->spsr & 0x1fU) != 0U)
    return false;

  if (mmu_current_root() != ctx->space->root_physical)
    return false;

  bool yield = frame->esr == ((0x15ULL << 26) | (1ULL << 25) |
                              EL0_CONTEXT_YIELD_SVC);
  bool exit = frame->esr == ((0x15ULL << 26) | (1ULL << 25) |
                             EL0_CONTEXT_EXIT_SVC);
  // SError is a system-level failure, never a context termination case,
  // even if an inconsistent record claims the synchronous vector.
  if (((frame->esr >> 26) & 0x3fU) == 0x2fU)
    return false;

  bool service = (yield || exit) && (frame->elr & 3U) == 0 &&
      frame->elr >= (uintptr_t)el0_context_program &&
      frame->elr < (uintptr_t)el0_context_program_end;
  ctx->info.trapped_root = mmu_current_root();
  ctx->saved = *frame;
  if (service) {
    ctx->info.state = yield ? EL0_CONTEXT_YIELDED : EL0_CONTEXT_EXITED;
    if (yield)
      ctx->info.yields++;
    ctx->info.result = frame->x[0];
  } else {
    // Ownership is established by RUNNING + live kernel continuation +
    // vector 8 + saved EL0t, not by the user PC, SP, or an expected site.
    // Keep the faulting PC unchanged for diagnosis; never return to it.
    ctx->info.state = EL0_CONTEXT_FAULTED;
  }
  // Restore the established kernel environment before returning from the
  // owned synchronous exception. A restoration failure is a kernel fault.
  if (!mmu_restore_kernel_space())
    return false;
  const mmu_address_space_t *kernel = mmu_kernel_address_space();
  ctx->info.restored_root = mmu_current_root();
  if (!kernel || ctx->info.restored_root != kernel->root_physical)
    return false;
  ctx->info.address_space_ok =
      ctx->info.entered_root == ctx->info.address_space_root &&
      ctx->info.trapped_root == ctx->info.address_space_root &&
      ctx->info.entered_root != ctx->info.restored_root;
  // The EL1 PC, mode, GPRs and original SP_EL0 come only from trusted
  // storage for this switch. Never retain that stack pointer after return.
  *frame = *kernel_return;
  kernel_return = NULL;
  active = NULL;
  return true;
}

static uint64_t check_frame(el0_slot_t *ctx) {
  uint64_t failures = 0;
  for (unsigned int i = 0; i < 31U; i++) {
    uint64_t expected = i == 0U ? ctx->info.resumptions * 7U :
        i == 1U ? ctx->info.resumptions : pattern(ctx, i);
    if (ctx->saved.x[i] != expected)
      failures |= 1ULL << i;
  }
  vaddr_t stack = EL0_STACK_TOP - EL0_CONTEXT_STACK_BYTES;
  if (ctx->saved.sp != stack || ctx->saved.sp_el0 != stack)
    failures |= 1ULL << 31;
  if ((ctx->saved.spsr & 0xf000001fULL) != 0xa0000000ULL)
    failures |= 1ULL << 32;
  if ((ctx->saved.spsr & 0x3c0U) != ctx->expected_daif)
    failures |= 1ULL << 33;
  const char *pc = ctx->info.state == EL0_CONTEXT_YIELDED ?
      el0_context_program_resume : el0_context_exit_resume;
  if (ctx->saved.elr != (uintptr_t)pc)
    failures |= 1ULL << 34;
  if (ctx->info.resumptions > EL0_CONTEXT_SLICES ||
      (ctx->info.resumptions < EL0_CONTEXT_SLICES &&
       ctx->info.state != EL0_CONTEXT_YIELDED) ||
      (ctx->info.resumptions == EL0_CONTEXT_SLICES &&
       ctx->info.state != EL0_CONTEXT_EXITED))
    failures |= 1ULL << 35;
  return failures;
}

static void check_fault_test(el0_slot_t *ctx,
                            const exception_frame_t *previous) {
  // Site/syndrome checks decide only whether this deliberate test PASSED.
  // They do not grant authority to resume an instruction after an abort.
  unsigned int id = (unsigned int)(ctx - contexts);
  bool alias = ctx->info.cross_stack_test || ctx->info.own_alias_test;
  uintptr_t target = alias ? stack_kernel_bottom(
      ctx->info.cross_stack_test ? 1U - id : id) :
      (uintptr_t)&el0_test_kernel_canary;
  const char *site = alias ?
      el0_context_stack_read : el0_context_fault_read;
  bool expected = ctx->info.fault_test && ctx->info.resumptions == 2U &&
      ctx->info.yields == 1U && ctx->info.result == 7U &&
      ctx->saved.elr == (uintptr_t)site &&
      (ctx->saved.esr & 0xfe0007ffULL) == 0x9200000fULL &&
      fault_far_valid(ctx->saved.esr) &&
      ctx->saved.far == target;
  if (!expected)
    return;

  uint64_t failures = 0;
  for (unsigned int i = 0; i < 31U; i++) {
    uint64_t expected_reg = i == 0U ? 7U : i == 1U ? 1U :
        i == 3U ? target : pattern(ctx, i);
    if (ctx->saved.x[i] != expected_reg)
      failures |= 1ULL << i;
  }
  uintptr_t base = frame_kernel_base(ctx);
  if (ctx->saved.sp != EL0_STACK_TOP - EL0_CONTEXT_STACK_BYTES ||
      ctx->saved.sp_el0 != EL0_STACK_TOP - EL0_CONTEXT_STACK_BYTES)
    failures |= 1ULL << 31;
  if ((ctx->saved.spsr & 0xf000001fULL) != 0xa0000000ULL)
    failures |= 1ULL << 32;
  if ((ctx->saved.spsr & 0x3c0U) != ctx->expected_daif)
    failures |= 1ULL << 33;
  ctx->info.context_failures |= failures;
  // Read only fixed, kernel-accessible diagnostic storage after confirming
  // the known test site. Never follow pointers from an arbitrary fault.
  const volatile uint64_t *stack = (const volatile uint64_t *)base;
  for (unsigned int i = 0; i < 31U; i++) {
    if (stack[i] != previous->x[i])
      ctx->info.context_failures |= 1ULL << i;
  }
  if (stack[31] != (previous->spsr & 0xf0000000ULL))
    ctx->info.context_failures |= 1ULL << 32;
  if (stack[32] != previous->sp_el0)
    ctx->info.context_failures |= 1ULL << 31;
  if (stack[EL0_CONTEXT_PROGRESS_OFFSET / 8] != 7U ||
      stack[EL0_CONTEXT_GUARD_OFFSET / 8] != pattern(ctx, 2) ||
      stack[EL0_CONTEXT_PATTERN_OFFSET / 8] != pattern(ctx, 3) ||
      stack[EL0_CONTEXT_VARIANT_OFFSET / 8] !=
          (alias ? 2U : 1U) ||
      (alias && stack[EL0_CONTEXT_TARGET_OFFSET / 8] != target) ||
      el0_test_kernel_canary != EL0_TEST_KERNEL_CANARY)
    ctx->info.stack_ok = false;
  ctx->info.fault_test_passed = ctx->info.context_failures == 0 &&
      ctx->info.kernel_failures == 0 && ctx->info.stack_ok &&
      ctx->info.address_space_ok && ctx->info.stack_mapping.valid;
}

bool el0_context_resume(unsigned int id) {
  if (id >= EL0_CONTEXT_COUNT || active)
    return false;
  el0_slot_t *ctx = &contexts[id];
  if ((ctx->info.state != EL0_CONTEXT_READY &&
       ctx->info.state != EL0_CONTEXT_YIELDED) ||
      ctx->info.context_failures || ctx->info.kernel_failures ||
      !ctx->info.stack_ok)
    return false;

  // The previous yield is the expected state at the very first resumed
  // instruction. The EL0 payload snapshots it before touching scratch GPRs.
  exception_frame_t previous = ctx->saved;
  bool resuming = ctx->info.state == EL0_CONTEXT_YIELDED;
  if (ctx->space != mmu_context_address_space(id) ||
      !mmu_activate_address_space(ctx->space))
    return false;
  if (!el0_stack_probe(ctx->info.physical_stack,
                       (paddr_t)stack_kernel_bottom(1U - id),
                       &ctx->info.stack_mapping)) {
    if (!mmu_restore_kernel_space())
      exception_panic(4, 0, ctx->saved.elr, 0, 5);
    return false;
  }
  active = ctx;
  ctx->info.kernel_failures |= el0_context_switch();
  // Defensive restoration also covers an assembly return probe failure.
  if (!mmu_restore_kernel_space())
    exception_panic(4, 0, ctx->saved.elr, 0, 5);
  // The handler clears these before ERET; also bound their lifetime to
  // this call boundary if the return probe reports a transition failure.
  kernel_return = NULL;
  active = NULL;
  if (ctx->info.state == EL0_CONTEXT_FAULTED) {
    // A generic fault may precede stack initialisation or carry an invalid
    // SP. Return without reading any EL0 memory. The fixed test alone has
    // a separate verifier for its known site and established stack.
    check_fault_test(ctx, &previous);
    return true;
  }
  ctx->info.context_failures |= check_frame(ctx);
  const volatile uint64_t *stack =
      (const volatile uint64_t *)frame_kernel_base(ctx);
  if (resuming) {
    for (unsigned int i = 0; i < 31U; i++) {
      if (stack[i] != previous.x[i])
        ctx->info.context_failures |= 1ULL << i;
    }
    if (stack[31] != (previous.spsr & 0xf0000000ULL))
      ctx->info.context_failures |= 1ULL << 32;
    if (stack[32] != previous.sp_el0)
      ctx->info.context_failures |= 1ULL << 31;
  }
  if (stack[EL0_CONTEXT_PROGRESS_OFFSET / 8] != ctx->info.result ||
      stack[EL0_CONTEXT_GUARD_OFFSET / 8] != pattern(ctx, 2) ||
      stack[EL0_CONTEXT_PATTERN_OFFSET / 8] != pattern(ctx, 3))
    ctx->info.stack_ok = false;
  ctx->info.passed = !ctx->info.fault_test &&
      ctx->info.state == EL0_CONTEXT_EXITED &&
      ctx->info.resumptions == EL0_CONTEXT_SLICES && ctx->info.yields == 5U &&
      ctx->info.result == 42U && ctx->info.stack_ok &&
      ctx->info.context_failures == 0U && ctx->info.kernel_failures == 0U &&
      ctx->info.address_space_ok && ctx->info.stack_mapping.valid;
  return true;
}
