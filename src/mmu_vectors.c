#include "mmu_vectors.h"
#include "string.h"

static struct {
  mmu_high_test_control_t *owner;
  const mmu_vector_code_t *code;
  mmu_vectors_result_t *result;
} armed;

static bool in_text(const mmu_high_test_control_t *c, vaddr_t va) {
  return va >= c->layout.text.start && va < c->layout.text.end;
}

static bool executable(const mmu_address_space_t *space, vaddr_t va,
                       paddr_t pa) {
  mmu_descriptor_info_t info;
  return mmu_space_inspect(space, va, &info) &&
      info.kind == MMU_DESC_PAGE && info.physical_address == pa &&
      info.access_permissions == 2U && !info.pxn && info.uxn &&
      info.access_flag && info.attr_index == MMU_ATTR_NORMAL_WB &&
      ((info.leaf_descriptor >> 8) & 3U) == 3U;
}

static bool hardware_mapping(vaddr_t va, paddr_t pa, bool write) {
  mmu_translation_t translation;
  return mmu_high_hw_translate(va, write, &translation) &&
      (write ? translation.fault && translation.fault_status == 0xfU :
       !translation.fault && translation.physical_address == pa);
}

static bool code_mapping(const mmu_high_test_control_t *c, paddr_t pa,
                          bool high) {
  vaddr_t va = high ? MMU_TTBR1_BASE + pa : pa;
  return executable(high ? c->higher : c->kernel, va, pa) &&
      hardware_mapping(va, pa, false) && hardware_mapping(va, pa, true);
}

static bool preflight(const mmu_high_test_control_t *c,
    const mmu_vector_code_t *code, const mmu_high_hw_state_t *s) {
  if (!mmu_high_persistent_matches(c, s) ||
      c->kernel->state != MMU_SPACE_ACTIVE || s->ttbr0 != c->kernel_root ||
      s->vbar != (c->vectors_high ?
        MMU_TTBR1_BASE + code->vectors : code->vectors) ||
      (code->vectors & 2047U) ||
      !in_text(c, code->vectors) || code->end <= code->vectors + 2048U ||
      code->end - code->vectors > 16384U ||
      code->end > c->layout.text.end ||
      code->observed_site < code->vectors + 2048U ||
      code->observed_site > code->end - 4U ||
      ((code->observed_site | code->continuation | code->end) & 3U) ||
      !in_text(c, code->continuation))
    return false;
  for (unsigned int i = 0; i < 2; i++) {
    if ((code->irq_sites[i] & 3U) ||
        code->irq_sites[i] < code->vectors + 2048U ||
        code->irq_sites[i] > code->end - 4U)
      return false;
  }
  if (c->vectors_high && memcmp(code, &c->vectors, sizeof(*code)))
    return false;
  // Whole table, entry/return stubs and literal pool, at both aliases.
  // Check both ends of every intersected page, including partial pages.
  for (paddr_t pa = code->vectors; pa < code->end;) {
    paddr_t next = (pa & ~4095ULL) + 4096U;
    if (next > code->end)
      next = code->end;
    if (!code_mapping(c, pa, false) || !code_mapping(c, next - 1U, false) ||
        !code_mapping(c, pa, true) || !code_mapping(c, next - 1U, true))
      return false;
    pa = next;
  }
  if (!code_mapping(c, code->continuation, false))
    return false;
  for (unsigned int i = 0; i < 4; i++) {
    if ((code->handlers[i] & 3U) || !in_text(c, code->handlers[i]) ||
        !code_mapping(c, code->handlers[i], false))
      return false;
  }
  mmu_translation_t stack;
  return mmu_high_hw_translate(s->sp - 16U, true, &stack) &&
      !stack.fault && stack.physical_address == s->sp - 16U;
}

// During vector selection retain every other pinned invariant. Handler
// SP is deeper on the same low stack and SP_EL0 holds the sentinel. The
// normal owner advances to its exact high-VBAR contract only after proof.
static bool phase_valid(const mmu_high_test_control_t *c,
    const mmu_vectors_result_t *r, const mmu_high_hw_state_t *s,
    vaddr_t vbar, bool handler) {
  const mmu_high_hw_state_t *b = &r->before;
  return s->ttbr0 == b->ttbr0 && s->ttbr1 == b->ttbr1 && s->tcr == b->tcr &&
      s->mair == b->mair && s->sctlr == b->sctlr && s->vbar == vbar &&
      s->current_el == b->current_el && s->spsel == b->spsel &&
      in_text(c, s->pc) && !(s->sp & 15U) &&
      (handler ? s->sp > c->layout.stack.start &&
         s->sp <= c->layout.stack.end && s->sp_el0 == VECTORS_TEST_SP_EL0 :
       s->sp == b->sp && s->sp_el0 == b->sp_el0);
}

void exception_sync_entry(exception_frame_t *frame, vaddr_t pc) {
  if (!armed.result) {
    exception_dispatch(frame);
    return;
  }
  mmu_vectors_result_t *r = armed.result;
  const mmu_vector_code_t *code = armed.code;
  mmu_high_hw_state_t state;
  mmu_high_hw_read(&state);
  mmu_translation_t mapping;
  if (r->observed || frame->vector != 4U ||
      frame->esr != ((0x15ULL << 26) | (1ULL << 25) | EXCEPTION_TEST_SVC) ||
      (frame->spsr & 0xf00003ffULL) != 0xa00003c5ULL ||
      frame->elr != code->continuation || pc != r->expected_pc ||
      frame->sp_el0 != VECTORS_TEST_SP_EL0 || (frame->sp & 15U) ||
      frame->sp <= armed.owner->layout.stack.start ||
      frame->sp > armed.owner->layout.stack.end ||
      !phase_valid(armed.owner, r, &state, r->high_vbar, true) ||
      !mmu_high_hw_translate(pc, false, &mapping) || mapping.fault ||
      mapping.physical_address != code->observed_site)
    exception_panic(frame->vector, frame->esr, frame->elr,
                     frame->far, frame->spsr);
  r->observed = true;
  r->observed_pc = pc;
  r->observed_physical = mapping.physical_address;
  r->vector = frame->vector;
  r->esr = frame->esr;
  r->spsr = frame->spsr;
  r->continuation = frame->elr;
  r->entry_sp = frame->sp;
  r->sp_el0 = frame->sp_el0;
  exception_frame_t before = *frame;
  exception_dispatch(frame);
  if (memcmp(&before, frame, sizeof(before)))
    mmu_high_hw_fatal("Higher-vector dispatcher changed the saved frame");
}

static bool run(mmu_high_test_control_t *c,
    const mmu_vector_code_t *code, mmu_vectors_result_t *r, bool keep) {
  if (!r)
    return false;
  *r = (mmu_vectors_result_t){.status = "Precondition rejected"};
  if (!c || !code || c->busy || armed.result)
    return false;
  r->permanent = keep || c->vectors_high;
  r->already_active = c->vectors_high;
  r->high_vbar = MMU_TTBR1_BASE + code->vectors;
  r->expected_pc = MMU_TTBR1_BASE + code->observed_site;
  r->vector_physical = code->vectors;
  r->expected_physical = code->observed_site;
  r->original_daif = mmu_high_hw_lock();
  mmu_high_hw_read(&r->before);
  if (!preflight(c, code, &r->before)) {
    r->after = r->before;
    goto done;
  }
  r->validated = true;
  c->busy = true;
  armed.owner = c;
  armed.code = code;
  armed.result = r;
  // VBAR is a context change, not a PTE replacement. No TLBI or cache
  // maintenance: MSR + ISB makes the immutable vector alias selectable.
  if (!c->vectors_high) {
    mmu_vectors_hw_operation(MMU_VECTOR_WRITE_VBAR, r->high_vbar);
    mmu_vectors_hw_operation(MMU_VECTOR_ISB, 0);
  }
  r->switched = true;
  mmu_high_hw_read(&r->installed);
  if (!phase_valid(c, r, &r->installed, r->high_vbar, false))
    mmu_high_hw_fatal("Higher VBAR installation changed the kernel state");
  r->failures = mmu_vectors_hw_trigger();
  mmu_high_hw_state_t returned;
  mmu_high_hw_read(&returned);
  if (!r->observed || r->failures ||
      !phase_valid(c, r, &returned, r->high_vbar, false))
    mmu_high_hw_fatal("Higher-vector exception return failed");
  if (!r->permanent) {
    mmu_vectors_hw_operation(MMU_VECTOR_WRITE_VBAR, r->before.vbar);
    mmu_vectors_hw_operation(MMU_VECTOR_ISB, 0);
  }
  mmu_high_hw_read(&r->after);
  if (!phase_valid(c, r, &r->after,
          r->permanent ? r->high_vbar : r->before.vbar, false))
    mmu_high_hw_fatal("Required VBAR configuration was not retained");
  if (r->permanent) {
    c->vectors = *code;
    c->live_vbar = r->high_vbar;
    c->vectors_high = true;
  }
  armed.result = NULL;
  armed.owner = NULL;
  armed.code = NULL;
  c->busy = false;
  if (!mmu_high_persistent_matches(c, &r->after))
    mmu_high_hw_fatal("Persistent hierarchy changed during vector test");
  r->restored = r->persistent = r->passed = true;
  r->status = "PASS";
done:
  mmu_high_hw_unlock(r->original_daif);
  r->final_daif = mmu_high_hw_daif();
  if (r->final_daif != r->original_daif) {
    (void)mmu_high_hw_lock();
    mmu_high_hw_fatal("Vector test exception-mask restoration failed");
  }
  r->masks_restored = true;
  return r->passed;
}


bool mmu_vectors_run(mmu_high_test_control_t *owner,
    const mmu_vector_code_t *code, mmu_vectors_result_t *result) {
  return run(owner, code, result, false);
}
bool mmu_vectors_install(mmu_high_test_control_t *owner,
    const mmu_vector_code_t *code, mmu_vectors_result_t *result) {
  return run(owner, code, result, true);
}
bool mmu_vectors_validate(mmu_high_test_control_t *owner,
    const mmu_high_hw_state_t *state) {
  return owner && owner->vectors_high &&
      preflight(owner, &owner->vectors, state);
}
