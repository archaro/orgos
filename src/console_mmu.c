#include "console.h"
#include "console_args.h"
#include "console_commands.h"
#include "mmu.h"
#include "mmu_high.h"
#include "mmu_high_test.h"
#include "mmu_vectors.h"
#include "mmu_kernel_stack.h"
#include "mmu_c_test.h"

extern char __text_start[], __rodata_start[], __data_start[], __bss_start[];
extern char __stack_bottom[], exception_vectors[];

static void hex_value(const char *label, uint64_t value) {
  console_printf("  %s: 0x", label);
  console_puthex64(value);
  console_putc('\n');
}

static bool sample(const mmu_address_space_t *space, const char *name,
                   paddr_t physical, unsigned int ap, bool pxn,
                   unsigned int attribute) {
  vaddr_t va;
  mmu_descriptor_info_t info;
  if (!mmu_high_address(physical, &va) ||
      !mmu_space_inspect(space, va, &info) ||
      info.kind == MMU_DESC_UNMAPPED || info.physical_address != physical)
    return false;
  console_printf("  %s\n", name);
  hex_value("VA", va);
  hex_value("PA", info.physical_address);
  console_printf("    %s AP=%u Attr=%u SH=%u PXN=%u UXN=%u AF=%u\n",
      info.kind == MMU_DESC_PAGE ? "PAGE" : "BLOCK",
      info.access_permissions, info.attr_index,
      (unsigned int)((info.leaf_descriptor >> 8) & 3U),
      info.pxn, info.uxn, info.access_flag);
  return info.access_permissions == ap && info.pxn == pxn && info.uxn &&
      info.access_flag && info.attr_index == attribute &&
      ((info.leaf_descriptor >> 8) & 3U) == (attribute == 0U ? 0U : 3U);
}

static void run_high_test(void) {
  mmu_high_test_result_t r;
  bool pass = mmu_high_test(&r);
  console_printf("Higher-half execution test: %s\n", pass ? "PASS" : "FAIL");
  console_printf("  Status: %s\n", r.status);
  console_printf("  Mode: %s\n", r.persistent ?
      "PERSISTENT (already enabled)" : "TEMPORARY (H2.6e)");
  console_printf("  Hierarchy validation: %s\n", r.validated ? "PASS" : "FAIL");
  hex_value("Original TCR_EL1", r.before.tcr);
  hex_value("Original TTBR0_EL1", r.before.ttbr0);
  hex_value("Original TTBR1_EL1", r.before.ttbr1);
  hex_value("TTBR1 root PA", r.root);
  hex_value("High entry VA", r.high_entry);
  hex_value("Expected observed VA", r.high_site);
  hex_value("Expected code PA", r.expected_physical);
  if (r.validated) {
    hex_value("Low baseline PC", r.low.observed_pc);
    hex_value("Low baseline failures", r.low.failures);
  }
  if (r.activated) {
    hex_value("Enabled TCR_EL1", r.enabled.tcr);
    hex_value("Observed high PC", r.high.observed_pc);
    if (r.physical_verified)
      hex_value("Observed code PA", r.observed_physical);
    else
      console_puts("  Observed code PA: NOT VERIFIED\n");
    console_printf("  Calculation: %llu (expected 42)\n",
        (unsigned long long)r.high.result);
    hex_value("Probe SP", r.high.observed_sp);
    hex_value("Low return PC", r.high.low_return);
    hex_value("Register/stack failures", r.high.failures);
    hex_value(r.persistent ? "Final TCR_EL1" : "Restored TCR_EL1",
               r.after.tcr);
    hex_value(r.persistent ? "Final TTBR0_EL1" : "Restored TTBR0_EL1",
               r.after.ttbr0);
    hex_value(r.persistent ? "Final TTBR1_EL1" : "Restored TTBR1_EL1",
               r.after.ttbr1);
  }
  console_printf("  Translation environment: %s\n", !r.activated ?
      "NOT CHANGED" : r.persistent ?
      (r.preserved ? "PRESERVED (TTBR1 remains enabled)" : "NOT PRESERVED") :
      (r.restored ? "RESTORED" : "NOT RESTORED"));
  hex_value("Original DAIF", r.original_daif);
  hex_value("Final DAIF", r.final_daif);
  console_printf("  Exception masks restored: %s\n",
      r.masks_restored ? "YES" : "NO");
}

static void run_vector_test(void) {
  mmu_vectors_result_t r;
  bool pass = mmu_vectors_test(&r);
  console_printf("Higher-half vector test: %s\n", pass ? "PASS" : "FAIL");
  console_printf("  Status: %s\n", r.status);
  console_printf("  Mode: %s\n", r.permanent ? "PERMANENT" : "TEMPORARY H2.6f2");
  console_printf("  Mapping validation: %s\n", r.validated ? "PASS" : "FAIL");
  hex_value("Original VBAR_EL1", r.before.vbar);
  hex_value("Required high VBAR_EL1", r.high_vbar);
  hex_value("Installed VBAR_EL1", r.installed.vbar);
  hex_value("Vector table PA", r.vector_physical);
  hex_value("Expected entry VA", r.expected_pc);
  hex_value("Expected entry PA", r.expected_physical);
  if (r.observed) {
    hex_value("Observed entry VA", r.observed_pc);
    hex_value("Observed entry PA", r.observed_physical);
    console_printf("  Exception vector: %llu\n", (unsigned long long)r.vector);
    hex_value("ESR_EL1", r.esr);
    hex_value("SPSR_EL1", r.spsr);
    hex_value("Low continuation", r.continuation);
    hex_value("Interrupted SP", r.entry_sp);
    hex_value("Actual handler SP", r.handler_sp);
    hex_value("Handler stack PA (SP-16)", r.handler_stack_physical);
    hex_value("SP_EL0 test seed", r.sp_el0);
    hex_value("Context failures", r.failures);
  } else {
    console_puts("  Exception entry: NOT OBSERVED\n");
  }
  hex_value("Final VBAR_EL1", r.after.vbar);
  hex_value("Original TTBR0_EL1", r.before.ttbr0);
  hex_value("Final TTBR0_EL1", r.after.ttbr0);
  hex_value("Original TTBR1_EL1", r.before.ttbr1);
  hex_value("Final TTBR1_EL1", r.after.ttbr1);
  hex_value("Original TCR_EL1", r.before.tcr);
  hex_value("Final TCR_EL1", r.after.tcr);
  console_printf("  Vector configuration preserved: %s\n",
      r.restored ? "YES" : "NO");
  console_printf("  Persistent TTBR1: %s\n",
      r.persistent ? "PRESERVED (ENABLED)" : "NOT VERIFIED");
  hex_value("Original DAIF", r.original_daif);
  hex_value("Final DAIF", r.final_daif);
  console_printf("  Exception masks restored: %s\n",
      r.masks_restored ? "YES" : "NO");
}

static void inspect_vectors(void) {
  mmu_vectors_result_t r;
  bool valid = mmu_vectors_inspect(&r);
  console_printf("Permanent higher-half vectors: %s\n",
      valid && r.permanent ? "ACTIVE" : "INVALID");
  hex_value("Vector table PA", r.vector_physical);
  hex_value("Required VBAR_EL1", r.high_vbar);
  hex_value("Live VBAR_EL1", r.before.vbar);
  hex_value("Live TTBR0_EL1", r.before.ttbr0);
  hex_value("Live TTBR1_EL1", r.before.ttbr1);
  hex_value("Live TCR_EL1", r.before.tcr);
  console_puts("  Inspection validates mappings; no exception was triggered.\n");
}

static void show_irq_vectors(void) {
  mmu_vector_irq_result_t r;
  if (!mmu_vectors_irq_read(&r)) {
    console_puts("Higher-half IRQ observations: INVALID VECTOR REGIME\n");
    return;
  }
  console_puts("Higher-half IRQ observations (armed hardware entry)\n");
  for (unsigned int i = 0; i < 2; i++) {
    const mmu_vector_irq_record_t *p = &r.paths[i];
    console_printf("  %s IRQ: %s\n", i ? "EL0" : "EL1h", !p->observed ?
        "NOT OBSERVED" : p->verified ? "PASS" : "FAIL");
    if (!p->observed)
      continue;
    hex_value("Observed entry VA", p->pc);
    hex_value("Expected entry VA", p->expected_pc);
    hex_value("Observed entry PA", p->physical);
    console_printf("  Vector: %llu; GIC source: %u\n",
        (unsigned long long)p->frame.vector, p->source);
    hex_value("Interrupted SPSR", p->frame.spsr);
    hex_value("Interrupted PC", p->frame.elr);
    hex_value("Interrupted SP", p->frame.sp);
    hex_value("Actual handler SP", p->state.sp);
    hex_value("Handler stack PA (SP-16)", p->handler_stack_physical);
    hex_value("Entry TTBR0_EL1", p->state.ttbr0);
    hex_value("Entry TTBR1_EL1", p->state.ttbr1);
    hex_value("Entry VBAR_EL1", p->state.vbar);
    console_printf("  IRQ count: %llu -> %llu\n",
        (unsigned long long)p->count_before,
        (unsigned long long)p->count_after);
    console_printf("  Handler/EOI completed: %s\n",
        p->handler_completed ? "YES" : "NO");
    console_puts("  Foreground resumed; common ERET restores the saved frame.\n");
  }
}

static void report_stack(bool execute) {
  mmu_kernel_stack_report_t r;
  bool pass = mmu_kernel_stack_report(execute, &r);
  console_printf("Permanent EL1 stack %s: %s\n", execute ? "test" : "inspection",
      pass ? "PASS" : "FAIL");
  console_printf("  State: %s\n", r.phase == MMU_STACK_HIGH ? "HIGH" :
      r.phase == MMU_STACK_TRANSITION ? "TRANSITION" : "BOOT LOW");
  hex_value("Physical base", r.physical_base);
  hex_value("Physical top", r.physical_top);
  hex_value("High virtual base", r.high_base);
  hex_value("High virtual top", r.high_top);
  hex_value("Observed SP_EL1 (MOV SP, SPSel=1)", r.state.sp);
  if (pass) hex_value("Observed SP physical translation", r.current_physical);
  hex_value("TTBR0_EL1", r.state.ttbr0);
  hex_value("TTBR1_EL1", r.state.ttbr1);
  hex_value("TCR_EL1", r.state.tcr);
  hex_value("VBAR_EL1", r.state.vbar);
  console_puts("  Required mapping: privileged RW/XN, WB, inner-shareable\n");
  if (r.executed) {
    hex_value("Observed outer SP", r.execution.outer_sp);
    hex_value("Observed inner SP", r.execution.inner_sp);
    if (r.translated) {
      hex_value("Outer SP physical translation", r.outer_physical);
      hex_value("Inner SP physical translation", r.inner_physical);
    }
    console_printf("  Nested result: %llu (expected 42)\n",
        (unsigned long long)r.execution.result);
    hex_value("Local failures", r.execution.failures);
    console_printf("  Permanent SP/registers retained: %s\n",
        r.preserved ? "YES" : "NO");
  }
  hex_value("Original DAIF", r.original_daif);
  hex_value("Final DAIF", r.final_daif);
  if (!execute)
    console_puts("  Mapping/live-SP inspection; no nested test triggered.\n");
}

static void run_c_test(void) {
  mmu_c_result_t r;
  bool pass = mmu_c_test(&r);
  console_printf("Higher-half C execution test: %s\n", pass ? "PASS" : "FAIL");
  console_printf("  Status: %s\n", r.status);
  hex_value("High C entry", r.high_entry);
  if (r.validated)
    hex_value("Expected entry PA", r.high_entry - MMU_TTBR1_BASE);
  if (r.executed) {
    const char *names[] = {"Outer", "Middle", "Leaf"};
    for (unsigned int i = 0; i < 3; i++) {
      console_printf("  %s compiled C function\n", names[i]);
      hex_value("Observed PC", r.evidence.pc[i]);
      hex_value("Expected code PA", r.expected_code_pa[i]);
      hex_value("Verified hardware code PA", r.code_pa[i]);
      hex_value("Observed SP", r.evidence.sp[i]);
      hex_value("Verified hardware SP PA", r.stack_pa[i]);
      hex_value("Observed automatic object", r.evidence.local[i]);
      hex_value("Verified hardware object PA", r.local_pa[i]);
    }
    hex_value("Middle observed caller LR", r.evidence.caller[0]);
    hex_value("Leaf observed caller LR", r.evidence.caller[1]);
    hex_value("Observed RO read address", r.evidence.ro_address);
    hex_value("Verified hardware RO PA", r.ro_pa);
    hex_value("Observed RW write address", r.evidence.rw_address);
    hex_value("Verified hardware RW PA", r.rw_pa);
    hex_value("Observed low return", r.call.low_return);
    console_printf("  Calculation: %llu (expected 42)\n",
        (unsigned long long)r.call.result);
  }
  console_printf("  Internal call chain: %s\n",
      r.chain_verified ? "OK" : "FAIL");
  console_printf("  Data and guards: %s\n", r.data_verified ? "OK" : "FAIL");
  console_printf("  AAPCS64 registers/SP and NZCV: %s\n",
      r.registers_preserved ? "PRESERVED" : "NOT VERIFIED");
  console_printf("  Translation registers/high VBAR: %s\n",
      r.preserved ? "PRESERVED" : "NOT VERIFIED");
  hex_value("Original DAIF", r.original_daif);
  hex_value("Final DAIF", r.final_daif);
}

void cmd_kmap(const char *args) {
  if (console_arg_word(&args, "code")) {
    if (console_arg_word(&args, "test") && console_arg_end(args))
      run_c_test();
    else
      console_puts("Usage: kmap code test\n");
    return;
  }
  if (console_arg_word(&args, "stack")) {
    if (console_arg_end(args))
      report_stack(false);
    else if (console_arg_word(&args, "test") && console_arg_end(args))
      report_stack(true);
    else
      console_puts("Usage: kmap stack [test]\n");
    return;
  }
  if (console_arg_word(&args, "vectors")) {
    if (console_arg_end(args))
      inspect_vectors();
    else if (console_arg_word(&args, "test") && console_arg_end(args))
      run_vector_test();
    else if (console_arg_word(&args, "irq")) {
      if (console_arg_end(args))
        show_irq_vectors();
      else if (console_arg_word(&args, "arm") && console_arg_end(args))
        console_puts(mmu_vectors_irq_arm() ?
            "IRQ observation ARMED; type the next command to generate UART IRQ.\n" :
            "Cannot arm IRQ observation: invalid vector regime.\n");
      else
        console_puts("Usage: kmap vectors irq [arm]\n");
    } else
      console_puts("Usage: kmap [code test | test | stack [test] | "
                   "vectors [test | irq [arm]]]\n");
    return;
  }
  if (console_arg_word(&args, "test")) {
    if (console_arg_end(args))
      run_high_test();
    else
      console_puts("Usage: kmap [code test | test | stack [test] | "
                   "vectors [test | irq [arm]]]\n");
    return;
  }
  if (!console_arg_end(args)) {
    console_puts("Usage: kmap [code test | test | stack [test] | "
                   "vectors [test | irq [arm]]]\n");
    return;
  }
  const mmu_address_space_t *space = mmu_high_address_space();
  if (!space || (space->state != MMU_SPACE_SEALED &&
                 space->state != MMU_SPACE_ACTIVE) ||
      space->geometry.region != MMU_REGION_TTBR1) {
    console_puts("Kernel higher-half hierarchy unavailable.\n");
    return;
  }
  mmu_info_t before, after;
  mmu_get_info(&before);
  console_puts("TTBR1 kernel hierarchy (software inspection only)\n");
  bool active = space->state == MMU_SPACE_ACTIVE;
  console_puts(active ? "  State: ACTIVE; persistent\n" :
                       "  State: SEALED; not activated\n");
  console_puts("  Geometry: upper VA=39 PA=36 granule=4KiB start=L1\n");
  hex_value("Higher-half base", MMU_TTBR1_BASE);
  hex_value("Hierarchy root PA", space->root_physical);
  hex_value("Required dual TCR", space->tcr);
  hex_value("Live TCR_EL1", before.tcr);
  hex_value("Live TTBR0_EL1", before.ttbr0);
  hex_value("Live TTBR1_EL1", before.ttbr1);
  console_printf("  Live TTBR1 walks: %s\n",
      (before.tcr & (1ULL << 23)) ? "DISABLED" : "ENABLED");
  if (active)
    console_printf("  Live hierarchy matches: %s\n",
        before.ttbr1 == space->root_physical && before.tcr == space->tcr &&
        before.mair == space->mair ? "YES" : "NO");
  bool ok = true;
  ok &= sample(space, "Kernel text", (uintptr_t)__text_start, 2, false, 2);
  ok &= sample(space, "Exception vectors", (uintptr_t)exception_vectors,
               2, false, 2);
  ok &= sample(space, "Read-only data", (uintptr_t)__rodata_start, 2, true, 2);
  ok &= sample(space, "Writable data", (uintptr_t)__data_start, 0, true, 2);
  ok &= sample(space, "BSS", (uintptr_t)__bss_start, 0, true, 2);
  ok &= sample(space, "Kernel stack", (uintptr_t)__stack_bottom, 0, true, 2);
  ok &= sample(space, "Table storage", space->root_physical, 0, true, 2);
  ok &= sample(space, "Peripheral MMIO", 0xfd500000ULL, 0, true, 0);
  ok &= sample(space, "PCIe MMIO", 0x600000000ULL, 0, true, 0);
  mmu_get_info(&after);
  bool unchanged = before.ttbr0 == after.ttbr0 &&
      before.ttbr1 == after.ttbr1 &&
      before.tcr == after.tcr && before.mair == after.mair &&
      before.sctlr == after.sctlr;
  console_printf("  Samples: %s\n", ok ? "PASS" : "FAIL");
  console_printf("  Live regime unchanged: %s\n", unchanged ? "YES" : "NO");
}
