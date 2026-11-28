#include "mmu_vectors.h"
#include "irq.h"
#include "gic.h"
#include "string.h"

// Single-core one-shot storage: foreground arms/copies with DAIF masked;
// IRQ entry publishes at most one acknowledged observation per path.
static struct {
  mmu_high_test_control_t *owner;
  bool armed[2];
  mmu_vector_irq_record_t records[2];
} diagnostic;

void exception_irq_entry(exception_frame_t *frame, vaddr_t pc) {
  unsigned int path = frame->vector == 5U ? 0U : 1U;
  if (!diagnostic.armed[path]) {
    irq_dispatch(frame);
    return;
  }
  mmu_high_test_control_t *c = diagnostic.owner;
  if ((frame->vector != 5U && frame->vector != 9U) ||
      (frame->spsr & 0x3fU) != (path ? 0U : 5U) ||
      pc != MMU_TTBR1_BASE + c->vectors.irq_sites[path])
    exception_panic(frame->vector, frame->esr, frame->elr,
                     frame->far, frame->spsr);
  mmu_vector_irq_record_t *r = &diagnostic.records[path];
  r->frame = *frame;
  r->pc = pc; // Executing assembly's ADR, not a value inferred from VBAR.
  r->expected_pc = MMU_TTBR1_BASE + c->vectors.irq_sites[path];
  mmu_high_hw_read(&r->state);
  if (r->state.vbar != c->live_vbar || r->state.ttbr1 != c->root ||
      r->state.tcr != MMU_TCR_DUAL_4K ||
      r->state.mair != MMU_MAIR_DEVICE_NC_WB ||
      r->state.sctlr != c->live_sctlr || r->state.current_el != 4U ||
      r->state.spsel != 1U)
    mmu_high_hw_fatal("IRQ entered an incompatible high-vector regime");
  r->count_before = irq_count();
  r->source = irq_dispatch_observed(frame);
  r->count_after = irq_count();
  mmu_high_hw_state_t after;
  mmu_high_hw_read(&after);
  const mmu_high_hw_state_t *before = &r->state;
  if (after.ttbr0 != before->ttbr0 || after.ttbr1 != before->ttbr1 ||
      after.tcr != before->tcr || after.mair != before->mair ||
      after.sctlr != before->sctlr || after.vbar != before->vbar ||
      after.sp != before->sp || after.sp_el0 != before->sp_el0 ||
      after.current_el != before->current_el || after.spsel != before->spsel)
    mmu_high_hw_fatal("IRQ handler changed the architectural state");
  if (memcmp(&r->frame, frame, sizeof(*frame)))
    mmu_high_hw_fatal("IRQ handler changed the interrupted frame");
  if (r->source == 1023U)
    return; // No suitable IRQ; stay armed. Never promote spurious to PASS.
  r->handler_completed = true; // Existing handler and GIC EOI completed.
  r->observed = true;
  diagnostic.armed[path] = false;
}

bool mmu_vectors_irq_arm_owner(mmu_high_test_control_t *c) {
  uint64_t daif = mmu_high_hw_lock();
  mmu_high_hw_state_t state;
  mmu_high_hw_read(&state);
  bool valid = mmu_vectors_validate(c, &state);
  if (valid) {
    diagnostic.owner = c;
    memset(diagnostic.records, 0, sizeof(diagnostic.records));
    diagnostic.armed[0] = diagnostic.armed[1] = true;
  }
  mmu_high_hw_unlock(daif);
  return valid;
}

static bool root_valid(mmu_high_test_control_t *c, paddr_t root) {
  if (root == c->kernel_root)
    return true;
  for (unsigned int i = 0; i < 2; i++) {
    const mmu_address_space_t *space = mmu_context_address_space(i);
    if (space && root == space->root_physical &&
        mmu_boot_compatible(c->kernel, space, &c->layout, (int)i))
      return true;
  }
  return false;
}

bool mmu_vectors_irq_read_owner(mmu_high_test_control_t *c,
    mmu_vector_irq_result_t *result) {
  if (!result)
    return false;
  *result = (mmu_vector_irq_result_t){0};
  uint64_t daif = mmu_high_hw_lock();
  mmu_high_hw_state_t state;
  mmu_high_hw_read(&state);
  result->valid = mmu_vectors_validate(c, &state);
  if (result->valid && diagnostic.owner == c) {
    memcpy(result->paths, diagnostic.records, sizeof(result->paths));
    for (unsigned int i = 0; i < 2; i++) {
      mmu_vector_irq_record_t *r = &result->paths[i];
      if (!r->observed)
        continue;
      const mmu_high_hw_state_t *s = &r->state;
      mmu_translation_t mapping;
      r->verified = r->handler_completed &&
          r->frame.vector == (i ? 9U : 5U) &&
          (r->frame.spsr & 0x3fU) == (i ? 0U : 5U) &&
          !(r->frame.elr & 3U) && !(r->frame.sp & 15U) &&
          r->frame.sp_el0 == s->sp_el0 &&
          (i ? r->frame.sp == r->frame.sp_el0 &&
             r->frame.sp >= EL0_STACK_VA && r->frame.sp <= EL0_STACK_TOP &&
             r->frame.elr >= c->layout.el0_text.start &&
             r->frame.elr < c->layout.el0_text.end :
           r->frame.sp > c->layout.stack.start &&
             r->frame.sp <= c->layout.stack.end &&
             r->frame.elr >= c->layout.text.start &&
             r->frame.elr < c->layout.text.end) &&
          r->pc == MMU_TTBR1_BASE + c->vectors.irq_sites[i] &&
          r->count_after == r->count_before + 1U &&
          (r->source == GIC_IRQ_UART0 || r->source == GIC_IRQ_PCIE_MSI) &&
          s->vbar == c->live_vbar && s->ttbr1 == c->root &&
          s->tcr == MMU_TCR_DUAL_4K && s->mair == state.mair &&
          s->sctlr == state.sctlr && root_valid(c, s->ttbr0) &&
          s->pc >= c->layout.text.start && s->pc < c->layout.text.end &&
          s->current_el == 4U && s->spsel == 1U && !(s->sp & 15U) &&
          s->sp > c->layout.stack.start && s->sp <= c->layout.stack.end &&
          mmu_high_hw_translate(r->pc, false, &mapping) && !mapping.fault &&
          mapping.physical_address == c->vectors.irq_sites[i];
      if (r->verified)
        r->physical = mapping.physical_address;
    }
  }
  mmu_high_hw_unlock(daif);
  return result->valid;
}
