#include "mmu_high_test.h"
#include "string.h"

static bool code_valid(mmu_high_code_t code, const mmu_boot_layout_t *layout) {
  return ((code.entry | code.site | code.end | code.low_return) & 3U) == 0 &&
      code.entry >= layout->text.start && code.end > code.entry &&
      code.end - code.entry <= 64U && code.end <= layout->text.end &&
      code.site >= code.entry && code.site <= code.end - 4U &&
      code.low_return >= layout->text.start &&
      code.low_return < layout->text.end;
}

static bool executable(const mmu_address_space_t *space, vaddr_t va,
                       paddr_t pa) {
  mmu_descriptor_info_t info;
  return mmu_space_inspect(space, va, &info) &&
      info.kind == MMU_DESC_PAGE && info.physical_address == pa &&
      info.access_permissions == 2U && !info.pxn && info.uxn &&
      info.access_flag && info.attr_index == 2U &&
      ((info.leaf_descriptor >> 8) & 3U) == 3U;
}

static bool profile_valid(const mmu_address_space_t *kernel,
    const mmu_address_space_t *high, const mmu_boot_layout_t *layout,
    mmu_high_code_t code, mmu_space_state_t state) {
  if (!kernel || !high || !layout || !code_valid(code, layout) ||
      !mmu_boot_compatible(kernel, kernel, layout, -1) ||
      high->state != state || !high->tables ||
      high->table_count != MMU_BOOT_TABLE_COUNT ||
      high->geometry.region != MMU_REGION_TTBR1 ||
      high->geometry.va_bits != 39U || high->geometry.pa_bits != 36U ||
      high->geometry.granule_shift != 12U ||
      high->geometry.start_level != 1U ||
      high->mair != MMU_MAIR_DEVICE_NC_WB || high->tcr != MMU_TCR_DUAL_4K ||
      high->root_physical != high->tables[0].physical)
    return false;
  // Exact policy equivalence. No extra aliases or unexamined entries.
  for (unsigned int t = 0; t < MMU_BOOT_TABLE_COUNT; t++) {
    const mmu_table_ref_t *ref = &high->tables[t];
    const mmu_table_ref_t *base = &kernel->tables[t];
    uint64_t descriptor;
    if (!ref->entries || ((uintptr_t)ref->entries & 4095U) ||
        ref->level != base->level ||
        !mmu_table_descriptor(ref->physical, &descriptor) ||
        ref->physical < layout->bss.start ||
        ref->physical + 4096U > layout->bss.end)
      return false;
    for (unsigned int j = 0; j < MMU_BOOT_TABLE_COUNT; j++) {
      if (ref->physical == kernel->tables[j].physical ||
          ref->entries == kernel->tables[j].entries ||
          (j < t && (ref->physical == high->tables[j].physical ||
                     ref->entries == high->tables[j].entries)))
        return false;
    }
    for (unsigned int i = 0; i < MMU_TABLE_ENTRIES; i++) {
      uint64_t expected = base->entries[i];
      if (t == 0U) {
        unsigned int child = i < 8U ? i + 1U : i == 24U ? 9U : 0U;
        expected = 0;
        if (child && !mmu_table_descriptor(high->tables[child].physical,
                                            &expected))
          return false;
      } else if (t == 1U && i == 0U) {
        if (!mmu_table_descriptor(high->tables[10].physical, &expected))
          return false;
      } else if (t == 10U) {
        expected = (expected & ~(1ULL << 6)) | (1ULL << 54);
      }
      if (ref->entries[i] != expected)
        return false;
    }
  }
  return executable(kernel, code.entry, code.entry) &&
      executable(kernel, code.end - 1U, code.end - 1U) &&
      executable(kernel, code.low_return, code.low_return) &&
      executable(high, MMU_TTBR1_BASE + code.entry, code.entry) &&
      executable(high, MMU_TTBR1_BASE + code.site, code.site) &&
      executable(high, MMU_TTBR1_BASE + code.end - 1U, code.end - 1U);
}

bool mmu_high_test_admit(mmu_high_test_control_t *control,
    const mmu_address_space_t *kernel, mmu_address_space_t *higher,
    const mmu_boot_layout_t *layout, mmu_high_code_t code) {
  if (!control || control->admitted || control->busy ||
      !profile_valid(kernel, higher, layout, code, MMU_SPACE_SEALED))
    return false;
  // Caller supplies exclusive static controller storage, disjoint from
  // all registered pages/metadata. No table is edited by this copy.
  control->kernel = kernel;
  control->higher = higher;
  control->kernel_root = kernel->root_physical;
  control->root = higher->root_physical;
  control->registry = higher->tables;
  control->layout = *layout;
  control->code = code;
  for (unsigned int t = 0; t < MMU_BOOT_TABLE_COUNT; t++) {
    control->refs[t] = higher->tables[t];
    memcpy(control->snapshot[t], higher->tables[t].entries, 4096);
  }
  control->admitted = true;
  return true;
}

static bool unchanged(const mmu_high_test_control_t *c) {
  const mmu_address_space_t *high = c->higher;
  if (high->root_physical != c->root || high->tables != c->registry ||
      high->table_count != MMU_BOOT_TABLE_COUNT ||
      c->kernel->root_physical != c->kernel_root)
    return false;
  for (unsigned int t = 0; t < MMU_BOOT_TABLE_COUNT; t++) {
    const mmu_table_ref_t *ref = &high->tables[t];
    if (ref->entries != c->refs[t].entries ||
        ref->physical != c->refs[t].physical ||
        ref->level != c->refs[t].level ||
        memcmp(ref->entries, c->snapshot[t], 4096) != 0)
      return false;
  }
  return profile_valid(c->kernel, high, &c->layout, c->code,
      c->persistent ? MMU_SPACE_ACTIVE : MMU_SPACE_SEALED);
}

static bool translation(vaddr_t va, paddr_t pa, bool write, bool denied) {
  mmu_translation_t result;
  return mmu_high_hw_translate(va, write, &result) &&
      (denied ? result.fault && result.fault_status == 0xfU :
       !result.fault && result.physical_address == pa);
}

bool mmu_el1_stack_contains(const mmu_high_test_control_t *c, vaddr_t sp) {
  if (!c || (c->stack_phase != MMU_STACK_BOOT_LOW &&
             c->stack_phase != MMU_STACK_HIGH))
    return false;
  vaddr_t offset = c->stack_phase == MMU_STACK_HIGH ? MMU_TTBR1_BASE : 0;
  return sp > offset + c->layout.stack.start &&
      sp <= offset + c->layout.stack.end && !(sp & 15U);
}
paddr_t mmu_el1_stack_physical(const mmu_high_test_control_t *c,
    vaddr_t address) {
  return c->stack_phase == MMU_STACK_HIGH ? address - MMU_TTBR1_BASE : address;
}

static bool anchors_valid(const mmu_high_test_control_t *c,
                           const mmu_high_hw_state_t *s) {
  const uint64_t required = 1ULL | 4ULL | (1ULL << 12) | (1ULL << 19);
  vaddr_t vectors = s->vbar;
  if (c->vectors_high) {
    if (vectors != MMU_TTBR1_BASE + c->vectors.vectors)
      return false;
    vectors = c->vectors.vectors;
  }
  return s->mair == MMU_MAIR_DEVICE_NC_WB &&
      (s->sctlr & required) == required && s->current_el == 4U &&
      s->spsel == 1U && s->pc >= c->layout.text.start &&
      s->pc < c->layout.text.end && mmu_el1_stack_contains(c, s->sp) &&
      !(vectors & 2047U) && vectors >= c->layout.text.start &&
      vectors <= c->layout.text.end - 2048U;
}

bool mmu_high_persistent_matches(const mmu_high_test_control_t *c,
    const mmu_high_hw_state_t *s) {
  return c && s && c->admitted && c->persistent && !c->busy &&
      s->ttbr1 == c->root && s->tcr == MMU_TCR_DUAL_4K &&
      s->sctlr == c->live_sctlr && s->vbar == c->live_vbar &&
      anchors_valid(c, s) && unchanged(c);
}

static bool environment_valid(const mmu_high_test_control_t *c,
                               const mmu_high_hw_state_t *s) {
  uint64_t tcr = c->persistent ? MMU_TCR_DUAL_4K : MMU_TCR_4K_36BIT;
  if (s->ttbr0 != c->kernel_root || s->tcr != tcr ||
      !anchors_valid(c, s) || c->kernel->state != MMU_SPACE_ACTIVE ||
      (c->persistent && (s->ttbr1 != c->root ||
        s->sctlr != c->live_sctlr || s->vbar != c->live_vbar)))
    return false;
  if (!translation(s->vbar, c->vectors_high ? c->vectors.vectors : s->vbar,
                   false, false) ||
      !translation(s->sp - 16U, mmu_el1_stack_physical(c, s->sp - 16U),
                   true, false) ||
      !translation(c->code.entry, c->code.entry, false, false) ||
      !translation(c->code.entry, 0, true, true))
    return false;
  for (unsigned int t = 0; t < MMU_BOOT_TABLE_COUNT; t++) {
    vaddr_t cpu = (uintptr_t)c->refs[t].entries;
    paddr_t pa = c->refs[t].physical;
    if (!translation(cpu, pa, false, false) ||
        !translation(cpu + 4095U, pa + 4095U, true, false))
      return false;
  }
  return true;
}

static bool same_low(const mmu_high_hw_state_t *a,
                     const mmu_high_hw_state_t *b) {
  return a->ttbr0 == b->ttbr0 && a->mair == b->mair &&
      a->sctlr == b->sctlr && a->vbar == b->vbar && a->sp == b->sp &&
      a->sp_el0 == b->sp_el0 && a->current_el == b->current_el &&
      a->spsel == b->spsel;
}

static bool probe_valid(const mmu_high_test_control_t *c,
    const mmu_high_probe_result_t *probe, vaddr_t expected_pc) {
  return probe->result == 42U && probe->observed_pc == expected_pc &&
      !probe->failures && probe->before_sp == probe->after_sp &&
      probe->observed_sp == probe->before_sp && !(probe->before_sp & 15U) &&
      mmu_el1_stack_contains(c, probe->before_sp) &&
      probe->low_return == c->code.low_return;
}

static void flush(void) {
  mmu_high_hw_operation(MMU_HIGH_TLBI, 0);
  mmu_high_hw_operation(MMU_HIGH_DSB, 0);
  mmu_high_hw_operation(MMU_HIGH_ISB, 0);
}

static bool run(mmu_high_test_control_t *c, mmu_high_test_result_t *r,
                bool keep_enabled) {
  if (!r)
    return false;
  *r = (mmu_high_test_result_t){.status = "Precondition rejected"};
  if (!c || !c->admitted || c->busy)
    return false;
  c->busy = true;
  r->persistent = keep_enabled || c->persistent;
  r->already_active = c->persistent;
  r->root = c->root;
  r->expected_physical = c->code.site;
  r->high_entry = MMU_TTBR1_BASE + c->code.entry;
  r->high_site = MMU_TTBR1_BASE + c->code.site;
  r->original_daif = mmu_high_hw_lock();
  mmu_high_hw_read(&r->before);
  if (!unchanged(c) || !environment_valid(c, &r->before))
    goto done;
  r->validated = true;
  mmu_high_call(c->code.entry, &r->low);
  mmu_high_hw_state_t baseline;
  mmu_high_hw_read(&baseline);
  if (!same_low(&r->before, &baseline) || baseline.tcr != r->before.tcr ||
      baseline.ttbr1 != r->before.ttbr1 ||
      baseline.pc < c->layout.text.start || baseline.pc >= c->layout.text.end)
    mmu_high_hw_fatal("Low baseline changed the kernel environment");
  if (!probe_valid(c, &r->low, c->code.site)) {
    r->status = "Low execution baseline failed";
    goto done;
  }
  uint64_t enabled_tcr = (r->before.tcr & ~MMU_HIGH_T1_FIELDS) |
      (c->higher->tcr & MMU_HIGH_T1_FIELDS);
  if (!c->persistent) {
    // Root first with EPD1 still set. The ISBs establish each change before
    // flushing; only low C/stack/vector mappings are used during transition.
    mmu_high_hw_operation(MMU_HIGH_DSB, 0);
    mmu_high_hw_operation(MMU_HIGH_WRITE_TTBR1, c->root);
    mmu_high_hw_operation(MMU_HIGH_ISB, 0);
    mmu_high_hw_operation(MMU_HIGH_WRITE_TCR, enabled_tcr);
    mmu_high_hw_operation(MMU_HIGH_ISB, 0);
    flush();
  }
  r->activated = true;
  c->higher->state = MMU_SPACE_ACTIVE;
  mmu_high_hw_read(&r->enabled);
  if (!same_low(&r->before, &r->enabled) || r->enabled.ttbr1 != c->root ||
      r->enabled.tcr != enabled_tcr ||
      r->enabled.pc < c->layout.text.start ||
      r->enabled.pc >= c->layout.text.end)
    mmu_high_hw_fatal("Enablement changed the low kernel environment");
  r->status = "Hardware translation check failed";
  mmu_translation_t site;
  if (translation(r->high_entry, c->code.entry, false, false) &&
      translation(r->high_entry, 0, true, true) &&
      mmu_high_hw_translate(r->high_site, false, &site) && !site.fault &&
      site.physical_address == c->code.site) {
    mmu_high_call(r->high_entry, &r->high);
    mmu_high_hw_state_t returned;
    mmu_high_hw_read(&returned);
    if (!same_low(&r->before, &returned) || returned.tcr != enabled_tcr ||
        returned.ttbr1 != c->root || returned.pc < c->layout.text.start ||
        returned.pc >= c->layout.text.end)
      mmu_high_hw_fatal("Probe did not return to the low environment");
    // Resolve the returned ADR value itself, not just the requested entry.
    // Only the authorised site is queried; no returned pointer is loaded.
    if (r->high.observed_pc == r->high_site &&
        mmu_high_hw_translate(r->high.observed_pc, false, &site) &&
        !site.fault && site.physical_address == c->code.site) {
      r->physical_verified = true;
      r->observed_physical = site.physical_address;
    }
    r->passed = r->physical_verified &&
        probe_valid(c, &r->high, r->high_site);
    r->status = r->passed ? "PASS" : "Probe evidence mismatch";
  }
  if (r->persistent) {
    // After a boot-time register change, failed proof is a kernel failure.
    // An established regime can reject bad diagnostic evidence only after
    // the low environment and unchanged dual configuration are verified.
    if (!r->already_active && !r->passed)
      mmu_high_hw_fatal("Persistent higher-half activation proof failed");
    mmu_high_hw_read(&r->after);
    if (!same_low(&r->before, &r->after) ||
        r->after.ttbr1 != c->root || r->after.tcr != enabled_tcr ||
        r->after.pc < c->layout.text.start ||
        r->after.pc >= c->layout.text.end)
      mmu_high_hw_fatal("Persistent translation environment changed");
    c->live_sctlr = r->before.sctlr;
    c->live_vbar = r->before.vbar;
    c->persistent = true;
    r->preserved = true;
    goto done;
  }
  // Disable walks and retire upper translations before restoring the old,
  // possibly unused/opaque TTBR1 value. TTBR0, MAIR, SCTLR and VBAR stay put.
  mmu_high_hw_operation(MMU_HIGH_DSB, 0);
  mmu_high_hw_operation(MMU_HIGH_WRITE_TCR, r->before.tcr);
  mmu_high_hw_operation(MMU_HIGH_ISB, 0);
  flush();
  mmu_high_hw_state_t disabled;
  mmu_high_hw_read(&disabled);
  if (!same_low(&r->before, &disabled) || disabled.tcr != r->before.tcr ||
      disabled.ttbr1 != c->root || disabled.pc < c->layout.text.start ||
      disabled.pc >= c->layout.text.end)
    mmu_high_hw_fatal("Could not disable upper translation safely");
  mmu_high_hw_operation(MMU_HIGH_WRITE_TTBR1, r->before.ttbr1);
  mmu_high_hw_operation(MMU_HIGH_ISB, 0);
  mmu_high_hw_read(&r->after);
  if (!same_low(&r->before, &r->after) || r->after.ttbr1 != r->before.ttbr1 ||
      r->after.tcr != r->before.tcr || r->after.pc < c->layout.text.start ||
      r->after.pc >= c->layout.text.end)
    mmu_high_hw_fatal("Original translation configuration was not restored");
  c->higher->state = MMU_SPACE_SEALED;
  r->restored = true;
done:
  mmu_high_hw_unlock(r->original_daif);
  r->final_daif = mmu_high_hw_daif();
  if (r->final_daif != r->original_daif) {
    (void)mmu_high_hw_lock();
    mmu_high_hw_fatal("Original exception masks were not restored");
  }
  r->masks_restored = true;
  c->busy = false;
  return r->passed;
}

bool mmu_high_test_run(mmu_high_test_control_t *control,
                       mmu_high_test_result_t *result) {
  return run(control, result, false);
}

bool mmu_high_enable(mmu_high_test_control_t *control) {
  mmu_high_test_result_t result;
  return run(control, &result, true);
}
