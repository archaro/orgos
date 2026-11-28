#include "mmu_stack.h"
#include "string.h"

// Sole static probe destination: no report writes into the frozen caller
// frames during the stack interval. It is not a persistent continuation.
static mmu_stack_probe_t probe;

static bool overlaps(uint64_t a, uint64_t bytes, uint64_t b, uint64_t size) {
  return a < b + size && b < a + bytes;
}
static uint64_t fill(unsigned int index) {
  return 0x535441434b000000ULL ^ index;
}
static bool bss_range(const mmu_high_test_control_t *c, paddr_t pa,
                       uint64_t size) {
  return !(pa & 4095U) && pa >= c->layout.bss.start &&
      pa < c->layout.bss.end && size <= c->layout.bss.end - pa;
}
static bool writable(const mmu_address_space_t *space, vaddr_t va,
                       paddr_t pa) {
  mmu_descriptor_info_t i;
  return mmu_space_inspect(space, va, &i) && i.kind == MMU_DESC_PAGE &&
      i.physical_address == pa && i.access_permissions == 0U &&
      i.pxn && i.uxn && i.access_flag && i.attr_index == MMU_ATTR_NORMAL_WB &&
      ((i.leaf_descriptor >> 8) & 3U) == 3U;
}
static bool translated(vaddr_t va, paddr_t pa, bool write) {
  mmu_translation_t t;
  return mmu_high_hw_translate(va, write, &t) &&
      !t.fault && t.physical_address == pa;
}
static bool buffer_valid(const mmu_high_test_control_t *c, uint64_t *cpu,
    paddr_t pa, uint64_t size, bool high) {
  if (!cpu || ((uintptr_t)cpu & 4095U) ||
      (uintptr_t)cpu > UINTPTR_MAX - size || !bss_range(c, pa, size))
    return false;
  for (uint64_t off = 0; off < size; off += 4096U) {
    for (unsigned int end = 0; end < 2; end++) {
      uint64_t d = off + (end ? 4095U : 0);
      if (!writable(c->kernel, pa + d, pa + d) ||
          !translated((uintptr_t)cpu + d, pa + d, false) ||
          !translated((uintptr_t)cpu + d, pa + d, true))
        return false;
      if (high && (!writable(c->higher, MMU_TTBR1_BASE + pa + d, pa + d) ||
          !translated(MMU_TTBR1_BASE + pa + d, pa + d, false) ||
          !translated(MMU_TTBR1_BASE + pa + d, pa + d, true)))
        return false;
    }
  }
  return true;
}
static bool storage_valid(const mmu_high_test_control_t *c,
    const mmu_stack_storage_t *s) {
  if (!buffer_valid(c, s->page, s->physical, STACK_PAGE_BYTES, true) ||
      !buffer_valid(c, s->witness, s->witness_physical,
                     STACK_WITNESS_BYTES, false) ||
      overlaps(s->physical, STACK_PAGE_BYTES, s->witness_physical,
                STACK_WITNESS_BYTES) ||
      overlaps((uintptr_t)s->page, STACK_PAGE_BYTES, (uintptr_t)s->witness,
                STACK_WITNESS_BYTES))
    return false;
  const mmu_address_space_t *spaces[] = {c->kernel, c->higher,
    mmu_context_address_space(0), mmu_context_address_space(1)};
  for (unsigned int a = 0; a < 4; a++) {
    if (!spaces[a] || (a >= 2U &&
        (spaces[a]->state != MMU_SPACE_SEALED ||
         !mmu_boot_compatible(c->kernel, spaces[a], &c->layout, (int)a - 2))))
      return false;
    for (size_t t = 0; t < spaces[a]->table_count; t++) {
      const mmu_table_ref_t *ref = &spaces[a]->tables[t];
      if (overlaps(s->physical, STACK_PAGE_BYTES, ref->physical, 4096) ||
          overlaps(s->witness_physical, STACK_WITNESS_BYTES,
                     ref->physical, 4096) ||
          overlaps((uintptr_t)s->page, STACK_PAGE_BYTES,
                     (uintptr_t)ref->entries, 4096) ||
          overlaps((uintptr_t)s->witness, STACK_WITNESS_BYTES,
                     (uintptr_t)ref->entries, 4096))
        return false;
    }
  }
  return true;
}
static bool code_valid(const mmu_high_test_control_t *c,
                        const mmu_stack_code_t *code) {
  if ((code->entry | code->end | code->wrapper_return |
       code->nested_return) & 3U)
    return false;
  if (code->entry < c->layout.text.start || code->end <= code->entry ||
      code->end - code->entry > 4096 || code->end > c->layout.text.end ||
      code->wrapper_return < code->entry ||
      code->wrapper_return >= code->end ||
      code->nested_return < code->entry || code->nested_return >= code->end)
    return false;
  for (vaddr_t va = code->entry; va < code->end;) {
    mmu_descriptor_info_t i;
    if (!mmu_space_inspect(c->kernel, va, &i) || i.kind != MMU_DESC_PAGE ||
        i.physical_address != va || i.access_permissions != 2U || i.pxn ||
        !i.uxn || !i.access_flag || i.attr_index != MMU_ATTR_NORMAL_WB ||
        ((i.leaf_descriptor >> 8) & 3U) != 3U || !translated(va, va, false))
      return false;
    va = (va & ~4095ULL) + 4096U;
  }
  return true;
}
static bool same_state(const mmu_high_hw_state_t *a,
                        const mmu_high_hw_state_t *b) {
  return a->ttbr0 == b->ttbr0 && a->ttbr1 == b->ttbr1 && a->tcr == b->tcr &&
      a->mair == b->mair && a->sctlr == b->sctlr && a->vbar == b->vbar &&
      a->sp == b->sp && a->sp_el0 == b->sp_el0 &&
      a->current_el == b->current_el && a->spsel == b->spsel;
}
static bool content_valid(const mmu_stack_storage_t *s,
                          const mmu_stack_code_t *code) {
  // Exact footprint: both real saved FP/LR pairs, local data and guards.
  for (unsigned int i = 0; i < STACK_PAGE_BYTES / 8U; i++) {
    uint64_t expected = fill(i);
    unsigned int outer = (STACK_PAGE_BYTES - 16U - 64U) / 8U;
    unsigned int inner = outer - 4U;
    if (i == outer) expected = probe.saved_fp;
    else if (i == outer + 1) expected = code->wrapper_return;
    else if (i == outer + 2 || i == inner + 2) expected = 6;
    else if (i == outer + 3 || i == inner + 3) expected = 7;
    else if (i == outer + 4) expected = 0xabc;
    else if (i == outer + 5) expected = 0xdef;
    else if (i == inner) expected = probe.high_top - 64U;
    else if (i == inner + 1) expected = code->nested_return;
    if (s->page[i] != expected)
      return false;
  }
  return true;
}

bool mmu_stack_run(mmu_high_test_control_t *c,
    const mmu_stack_storage_t *s, const mmu_stack_code_t *code,
    mmu_stack_result_t *r) {
  if (!r)
    return false;
  *r = (mmu_stack_result_t){.status = "Precondition rejected"};
  if (!c || !s || !code || c->busy ||
      c->stack_phase != MMU_STACK_BOOT_LOW)
    return false;
  r->original_daif = mmu_high_hw_lock();
  mmu_high_hw_read(&r->before);
  if (!c->vectors_high || !mmu_high_persistent_matches(c, &r->before) ||
      c->kernel->state != MMU_SPACE_ACTIVE ||
      r->before.ttbr0 != c->kernel_root ||
      c->layout.stack.end - c->layout.stack.start != STACK_WITNESS_BYTES ||
      r->before.sp < c->layout.stack.start + 256U ||
      !code_valid(c, code) || !storage_valid(c, s) ||
      !translated(r->before.sp - 128U, r->before.sp - 128U, true) ||
      !translated(r->before.vbar, c->vectors.vectors, false))
    goto done;
  r->validated = true;
  r->physical = s->physical;
  r->high_base = MMU_TTBR1_BASE + s->physical;
  r->high_top = r->high_base + STACK_PAGE_BYTES - 16U;
  c->busy = true;
  for (unsigned int i = 0; i < STACK_PAGE_BYTES / 8U; i++)
    s->page[i] = fill(i);
  probe = (mmu_stack_probe_t){0};
  mmu_stack_call(r->high_top, &probe, s->witness, c->layout.stack.end);
  r->executed = true;
  mmu_high_hw_read(&r->after);
  if (!same_state(&r->before, &r->after) || !probe.low_match ||
      probe.low_sp != r->before.sp || probe.low_frame != probe.low_sp - 128U ||
      probe.copy_bytes != c->layout.stack.end - probe.low_frame ||
      probe.copy_bytes > STACK_WITNESS_BYTES ||
      probe.copy_pointer != (uintptr_t)s->witness || probe.daif != 0x3c0U ||
      probe.initial_nzcv != probe.final_nzcv ||
      probe.saved_fp != s->witness[10] ||
      probe.initial_nzcv != s->witness[12] ||
      probe.daif != s->witness[13] || s->witness[15] != 0x5354U ||
      probe.low_return != code->wrapper_return)
    mmu_high_hw_fatal("Higher-stack return did not preserve the low context");
  r->restored = r->registers_preserved = r->low_intact = true;
  r->probe = probe;
  r->canaries = s->page[0] == fill(0) && s->page[1] == fill(1) &&
      s->page[510] == fill(510) && s->page[511] == fill(511);
  r->contents = content_valid(s, code);
  mmu_translation_t observed, lowest;
  r->translated = probe.high_top == r->high_top &&
      probe.observed_sp == r->high_top - 64U &&
      probe.lowest_sp == r->high_top - 96U &&
      mmu_high_hw_translate(probe.observed_sp, true, &observed) &&
      !observed.fault &&
      observed.physical_address == s->physical + STACK_PAGE_BYTES - 80U &&
      mmu_high_hw_translate(probe.lowest_sp, true, &lowest) && !lowest.fault &&
      lowest.physical_address == s->physical + STACK_PAGE_BYTES - 112U;
  if (r->translated) {
    r->observed_physical = observed.physical_address;
    r->lowest_physical = lowest.physical_address;
  }
  r->passed = r->translated && r->canaries && r->contents &&
      !probe.local_failures && !probe.callee_failures && probe.result == 42U;
  r->status = r->passed ? "PASS" : "Stack execution evidence mismatch";
  c->busy = false;
  if (!mmu_high_persistent_matches(c, &r->after))
    mmu_high_hw_fatal("Persistent regime changed during stack diagnostic");
done:
  mmu_high_hw_unlock(r->original_daif);
  r->final_daif = mmu_high_hw_daif();
  if (r->final_daif != r->original_daif) {
    (void)mmu_high_hw_lock();
    mmu_high_hw_fatal("Stack diagnostic exception-mask restoration failed");
  }
  r->masks_restored = true;
  return r->passed;
}
