#include "mmu_c_test.h"

static bool within(vaddr_t address, vaddr_t base, vaddr_t end,
                    uint64_t bytes) {
  return address >= base && address < end && bytes <= end - address;
}
static bool mapped(const mmu_address_space_t *space, vaddr_t va,
    paddr_t pa, unsigned int ap, bool xn) {
  mmu_descriptor_info_t i;
  return mmu_space_inspect(space, va, &i) && i.kind == MMU_DESC_PAGE &&
      i.physical_address == pa && i.access_permissions == ap &&
      i.pxn == xn && i.uxn && i.access_flag &&
      i.attr_index == MMU_ATTR_NORMAL_WB &&
      ((i.leaf_descriptor >> 8) & 3U) == 3U;
}
static bool translated(vaddr_t va, paddr_t pa, bool write, bool denied) {
  mmu_translation_t t;
  return mmu_high_hw_translate(va, write, &t) &&
      (denied ? t.fault && t.fault_status == 0xfU :
       !t.fault && t.physical_address == pa);
}
static bool extent(const mmu_high_test_control_t *c, vaddr_t base,
    uint64_t bytes, unsigned int ap, bool xn) {
  // Examine both ends of every touched page, not just the entry page.
  for (uint64_t offset = 0; offset < bytes;) {
    vaddr_t low = base + offset, high = MMU_TTBR1_BASE + low;
    if (!mapped(c->kernel, low, low, ap, xn) ||
        !mapped(c->higher, high, low, ap, xn) ||
        !translated(low, low, false, false) ||
        !translated(high, low, false, false) ||
        !translated(low, low, true, ap == 2U) ||
        !translated(high, low, true, ap == 2U))
      return false;
    uint64_t last = offset + (4095U - (low & 4095U));
    if (last >= bytes) last = bytes - 1U;
    if (last != offset) {
      if (!mapped(c->kernel, base + last, base + last, ap, xn) ||
          !mapped(c->higher, MMU_TTBR1_BASE + base + last,
                    base + last, ap, xn) ||
          !translated(MMU_TTBR1_BASE + base + last,
                        base + last, ap == 0U, false))
        return false;
    }
    offset = last + 1U;
  }
  return true;
}
static bool manifest(const mmu_high_test_control_t *c,
                     const mmu_c_manifest_t *m) {
  if (!m || (m->start | m->end | m->entry | m->low_return) & 3U ||
      m->end <= m->start || m->end - m->start > 2048U ||
      !within(m->start, c->layout.text.start, c->layout.text.end,
                 m->end - m->start) ||
      !within(m->low_return, c->layout.text.start, c->layout.text.end, 4) ||
      !within(m->ro, c->layout.rodata.start, c->layout.rodata.end, 24) ||
      !within(m->rw, c->layout.bss.start, c->layout.bss.end,
                 sizeof(mmu_c_storage_t)) || (m->ro | m->rw) & 7U ||
      m->entry != m->function[0].start)
    return false;
  // Linked order is leaf, middle, outer; require a complete partition.
  if (m->function[2].start != m->start ||
      m->function[2].end != m->function[1].start ||
      m->function[1].end != m->function[0].start ||
      m->function[0].end != m->end)
    return false;
  for (unsigned int i = 0; i < 3; i++)
    if ((m->function[i].start | m->function[i].end | m->site[i]) & 3U ||
        !within(m->site[i], m->function[i].start, m->function[i].end, 4))
      return false;
  return extent(c, m->start, m->end - m->start, 2, false) &&
      translated(MMU_TTBR1_BASE + m->entry, m->entry, false, false) &&
      translated(MMU_TTBR1_BASE + m->entry, 0, true, true) &&
      extent(c, m->low_return, 4, 2, false) &&
      extent(c, m->ro, 24, 2, true) &&
      extent(c, m->rw, sizeof(mmu_c_storage_t), 0, true);
}
static bool same(const mmu_high_hw_state_t *a,
                   const mmu_high_hw_state_t *b) {
  return a->ttbr0 == b->ttbr0 && a->ttbr1 == b->ttbr1 &&
      a->tcr == b->tcr && a->mair == b->mair && a->sctlr == b->sctlr &&
      a->vbar == b->vbar && a->sp == b->sp && a->sp_el0 == b->sp_el0 &&
      a->current_el == b->current_el && a->spsel == b->spsel;
}
static bool observations(const mmu_high_test_control_t *c,
    const mmu_c_manifest_t *m, mmu_c_result_t *r) {
  mmu_c_evidence_t *e = &r->evidence;
  uint64_t high_base = MMU_TTBR1_BASE + c->layout.stack.start;
  if (r->call.before_sp != r->before.sp - 160U ||
      r->call.after_sp != r->call.before_sp ||
      r->call.low_return != m->low_return || r->call.failures ||
      r->call.nzcv_before != r->call.nzcv_after)
    return false;
  r->registers_preserved = true;
  for (unsigned int i = 0; i < 3; i++) {
    vaddr_t ceiling = i ? e->sp[i - 1] : r->call.before_sp;
    if (e->pc[i] != MMU_TTBR1_BASE + m->site[i] ||
        !mmu_el1_stack_contains(c, e->sp[i]) || e->sp[i] >= ceiling ||
        !within(e->local[i], e->sp[i], ceiling, 8) ||
        (e->local[i] & 7U) || e->local[i] < high_base ||
        !translated(e->pc[i], m->site[i], false, false) ||
        !translated(e->sp[i], e->sp[i] - MMU_TTBR1_BASE, true, false) ||
        !translated(e->local[i], e->local[i] - MMU_TTBR1_BASE, true, false))
      return false;
    r->code_pa[i] = m->site[i];
    r->stack_pa[i] = e->sp[i] - MMU_TTBR1_BASE;
    r->local_pa[i] = e->local[i] - MMU_TTBR1_BASE;
  }
  for (unsigned int i = 0; i < 2; i++)
    if ((e->caller[i] & 3U) ||
        !within(e->caller[i], MMU_TTBR1_BASE + m->function[i].start,
                    MMU_TTBR1_BASE + m->function[i].end, 4) ||
        !translated(e->caller[i], e->caller[i] - MMU_TTBR1_BASE,
                       false, false))
      return false;
  r->chain_verified = true;
  return true;
}
bool mmu_c_run(mmu_high_test_control_t *c, const mmu_c_manifest_t *m,
    const volatile uint64_t ro[3], volatile mmu_c_storage_t *rw,
    mmu_c_result_t *r) {
  if (!r) return false;
  *r = (mmu_c_result_t){.status = "Precondition rejected"};
  if (!c || !ro || !rw || c->busy) return false;
  r->original_daif = mmu_high_hw_lock();
  mmu_high_hw_read(&r->before);
  if (c->stack_phase != MMU_STACK_HIGH || !c->vectors_high ||
      r->before.ttbr0 != c->kernel_root ||
      c->kernel->state != MMU_SPACE_ACTIVE ||
      !mmu_high_persistent_matches(c, &r->before) ||
      !translated(r->before.sp - 16U,
                     r->before.sp - 16U - MMU_TTBR1_BASE, true, false) ||
      r->before.sp <= MMU_TTBR1_BASE + c->layout.stack.start + 1024U ||
      !manifest(c, m))
    goto done;
  r->high_entry = MMU_TTBR1_BASE + m->entry;
  r->high_ro = MMU_TTBR1_BASE + m->ro;
  r->high_rw = MMU_TTBR1_BASE + m->rw;
  for (unsigned int i = 0; i < 3; i++)
    r->expected_code_pa[i] = m->site[i];
  r->validated = true;
  // Only private diagnostic storage is initialized. No kernel table/state
  // is changed, and input pointers are never supplied by the console.
  if (ro[0] != MMU_C_GUARD_BEFORE || ro[1] != 6U || ro[2] != 7U) {
    r->status = "Read-only input corrupted";
    goto done;
  }
  *rw = (mmu_c_storage_t){MMU_C_GUARD_BEFORE, 0, MMU_C_GUARD_AFTER};
  c->busy = true;
  mmu_c_call(r->high_entry, &r->evidence,
      (const volatile uint64_t *)(uintptr_t)r->high_ro,
      (volatile mmu_c_storage_t *)(uintptr_t)r->high_rw, &r->call);
  r->executed = true;
  mmu_high_hw_read(&r->after);
  c->busy = false;
  if (!same(&r->before, &r->after) ||
      !mmu_high_persistent_matches(c, &r->after) ||
      mmu_high_hw_daif() != 0x3c0U)
    mmu_high_hw_fatal("C island changed the low kernel environment");
  r->preserved = true;
  r->data_verified = r->evidence.ro_address == r->high_ro + 8U &&
      r->evidence.rw_address == r->high_rw + 8U &&
      r->evidence.ro_value[0] == 6 && r->evidence.ro_value[1] == 7 &&
      r->evidence.local_value[0] == 6 &&
      r->evidence.local_value[1] == 7 &&
      r->evidence.local_value[2] == 42 && r->evidence.written == 42 &&
      ro[0] == MMU_C_GUARD_BEFORE && ro[1] == 6 && ro[2] == 7 &&
      rw->guard_before == MMU_C_GUARD_BEFORE && rw->value == 42 &&
      rw->guard_after == MMU_C_GUARD_AFTER &&
      translated(r->evidence.ro_address, m->ro + 8U, false, false) &&
      translated(r->evidence.ro_address + 8U, m->ro + 16U, false, false) &&
      translated(r->evidence.rw_address, m->rw + 8U, true, false);
  if (r->data_verified) {
    r->ro_pa = m->ro + 8U;
    r->rw_pa = m->rw + 8U;
  }
  r->passed = observations(c, m, r) && r->data_verified &&
      r->call.result == 42;
  r->status = r->passed ? "PASS" : "C execution evidence mismatch";
done:
  mmu_high_hw_unlock(r->original_daif);
  r->final_daif = mmu_high_hw_daif();
  if (r->final_daif != r->original_daif) {
    (void)mmu_high_hw_lock();
    mmu_high_hw_fatal("C island failed to restore DAIF");
  }
  r->masks_restored = true;
  return r->passed;
}
