#include "mmu_kernel_stack.h"
#include "string.h"

// Boot transfer state is static. No automatic object or caller frame is
// retained across the one-way branch. The upper owner owns the lifecycle.
static struct {
  mmu_high_test_control_t *owner;
  mmu_kernel_stack_plan_t plan;
  uint64_t daif;
} transfer;
static mmu_kernel_stack_execution_t execution;

static bool rw_xn(const mmu_address_space_t *space, vaddr_t va, paddr_t pa) {
  mmu_descriptor_info_t i;
  return mmu_space_inspect(space, va, &i) && i.kind == MMU_DESC_PAGE &&
      i.physical_address == pa && i.access_permissions == 0U && i.pxn &&
      i.uxn && i.access_flag && i.attr_index == MMU_ATTR_NORMAL_WB &&
      ((i.leaf_descriptor >> 8) & 3U) == 3U;
}
static bool at(vaddr_t va, paddr_t pa, bool write) {
  mmu_translation_t t;
  return mmu_high_hw_translate(va, write, &t) &&
      !t.fault && t.physical_address == pa;
}
static bool mappings(const mmu_high_test_control_t *c) {
  paddr_t base = c->layout.stack.start, top = c->layout.stack.end;
  if ((base | top) & 4095U || top <= base || top - base != 16384U)
    return false;
  for (unsigned int id = 0; id < 2; id++) {
    const mmu_address_space_t *ctx = mmu_context_address_space(id);
    if (!ctx || !mmu_boot_compatible(c->kernel, ctx, &c->layout, (int)id))
      return false;
    for (paddr_t pa = base; pa < top; pa += 4096U) {
      if (!rw_xn(ctx, pa, pa))
        return false;
    }
  }
  for (paddr_t page = base; page < top; page += 4096U) {
    for (unsigned int end = 0; end < 2; end++) {
      paddr_t pa = page + (end ? 4095U : 0);
      vaddr_t high = MMU_TTBR1_BASE + pa;
      if (!rw_xn(c->kernel, pa, pa) || !rw_xn(c->higher, high, pa) ||
          !at(pa, pa, false) || !at(pa, pa, true) ||
          !at(high, pa, false) || !at(high, pa, true))
        return false;
    }
  }
  return true;
}
static bool regime_unchanged(const mmu_high_hw_state_t *a,
                              const mmu_high_hw_state_t *b) {
  return a->ttbr0 == b->ttbr0 && a->ttbr1 == b->ttbr1 && a->tcr == b->tcr &&
      a->mair == b->mair && a->sctlr == b->sctlr && a->vbar == b->vbar &&
      a->sp_el0 == b->sp_el0 && b->current_el == 4U && b->spsel == 1U;
}
static bool current(const mmu_high_test_control_t *c,
                      const mmu_high_hw_state_t *s) {
  return c->stack_phase == MMU_STACK_HIGH && c->vectors_high &&
      c->kernel->state == MMU_SPACE_ACTIVE && s->ttbr0 == c->kernel_root &&
      mmu_high_persistent_matches(c, s) &&
      at(s->sp - 16U, mmu_el1_stack_physical(c, s->sp - 16U), true);
}

bool mmu_kernel_stack_prepare(mmu_high_test_control_t *c,
    const mmu_high_hw_state_t *s, vaddr_t entry,
    mmu_kernel_stack_plan_t *plan) {
  if (!c || !s || !plan || c->stack_phase != MMU_STACK_BOOT_LOW ||
      !c->vectors_high || c->kernel->state != MMU_SPACE_ACTIVE ||
      s->ttbr0 != c->kernel_root || !mmu_high_persistent_matches(c, s) ||
      !mappings(c) || (entry & 3U) || entry < c->layout.text.start ||
      entry >= c->layout.text.end)
    return false;
  mmu_descriptor_info_t code;
  if (!mmu_space_inspect(c->kernel, entry, &code) ||
      code.kind != MMU_DESC_PAGE || code.physical_address != entry ||
      code.access_permissions != 2U || code.pxn || !code.uxn ||
      !at(entry, entry, false))
    return false;
  *plan = (mmu_kernel_stack_plan_t){.before = *s,
    .high_base = MMU_TTBR1_BASE + c->layout.stack.start,
    .high_top = MMU_TTBR1_BASE + c->layout.stack.end, .continuation = entry};
  return true;
}

void mmu_kernel_stack_enter_owner(mmu_high_test_control_t *c,
    void (*continuation)(void)) {
  uint64_t daif = mmu_high_hw_lock();
  mmu_high_hw_state_t before;
  mmu_high_hw_read(&before);
  if (daif != 0x3c0U || transfer.owner ||
      !mmu_kernel_stack_prepare(c, &before, (uintptr_t)continuation,
                                 &transfer.plan))
    mmu_high_hw_fatal("Permanent stack boot precondition rejected");
  transfer.owner = c;
  transfer.daif = daif;
  c->stack_phase = MMU_STACK_TRANSITION;
  c->busy = true;
  // Non-returning architecture boundary; correctness does not depend on
  // a compiler tail call or on keeping any of these local frames alive.
  mmu_kernel_stack_jump(transfer.plan.high_top, continuation);
}

void mmu_kernel_stack_complete_owner(mmu_high_test_control_t *c) {
  mmu_high_hw_state_t now;
  mmu_high_hw_read(&now);
  if (!c || transfer.owner != c || !c->busy ||
      c->stack_phase != MMU_STACK_TRANSITION ||
      !regime_unchanged(&transfer.plan.before, &now) ||
      (now.sp & 15U) || now.sp <= transfer.plan.high_base ||
      now.sp > transfer.plan.high_top || now.pc < c->layout.text.start ||
      now.pc >= c->layout.text.end ||
      !at(now.sp - 16U, now.sp - 16U - MMU_TTBR1_BASE, true) ||
      mmu_high_hw_daif() != transfer.daif)
    mmu_high_hw_fatal("Permanent stack handoff did not establish high EL1h");
  c->stack_phase = MMU_STACK_HIGH;
  c->busy = false;
  if (!current(c, &now) || !mappings(c))
    mmu_high_hw_fatal("Permanent high-stack contract verification failed");
  memset(&transfer, 0, sizeof(transfer));
}

bool mmu_kernel_stack_report_owner(mmu_high_test_control_t *c,
    bool execute, mmu_kernel_stack_report_t *r) {
  if (!r)
    return false;
  *r = (mmu_kernel_stack_report_t){0};
  if (!c || c->busy)
    return false;
  r->original_daif = mmu_high_hw_lock();
  mmu_high_hw_read(&r->state);
  r->phase = c->stack_phase;
  r->physical_base = c->layout.stack.start;
  r->physical_top = c->layout.stack.end;
  r->high_base = MMU_TTBR1_BASE + r->physical_base;
  r->high_top = MMU_TTBR1_BASE + r->physical_top;
  r->valid = current(c, &r->state) && mappings(c) &&
      (!execute || r->state.sp > r->high_base + 96U) &&
      at(r->state.sp, r->state.sp - MMU_TTBR1_BASE, true);
  if (!r->valid)
    goto done;
  r->current_physical = mmu_el1_stack_physical(c, r->state.sp);
  if (execute) {
    c->busy = true;
    execution = (mmu_kernel_stack_execution_t){0};
    mmu_kernel_stack_call(&execution);
    mmu_high_hw_read(&r->after);
    if (!regime_unchanged(&r->state, &r->after) ||
        r->state.sp != r->after.sp ||
        mmu_high_hw_daif() != 0x3c0U)
      mmu_high_hw_fatal("Permanent-stack nested call changed kernel state");
    r->executed = true;
    r->execution = execution;
    r->translated = execution.outer_sp == r->state.sp - 64U &&
        execution.inner_sp == execution.outer_sp - 32U &&
        at(execution.outer_sp, execution.outer_sp - MMU_TTBR1_BASE, true) &&
        at(execution.inner_sp, execution.inner_sp - MMU_TTBR1_BASE, true);
    if (r->translated) {
      r->outer_physical = execution.outer_sp - MMU_TTBR1_BASE;
      r->inner_physical = execution.inner_sp - MMU_TTBR1_BASE;
    }
    r->preserved = true;
    c->busy = false;
    if (!current(c, &r->after))
      mmu_high_hw_fatal("Permanent-stack test lost its runtime contract");
    r->valid = r->translated && !execution.failures && execution.result == 42U;
  }
done:
  mmu_high_hw_unlock(r->original_daif);
  r->final_daif = mmu_high_hw_daif();
  if (r->final_daif != r->original_daif) {
    (void)mmu_high_hw_lock();
    mmu_high_hw_fatal("Permanent-stack diagnostic failed to restore DAIF");
  }
  return r->valid;
}
