#include "console.h"
#include "console_args.h"
#include "console_commands.h"
#include "el0_context.h"

static const char *state_name(el0_context_state_t state) {
  switch (state) {
    case EL0_CONTEXT_EMPTY: return "EMPTY";
    case EL0_CONTEXT_READY: return "READY";
    case EL0_CONTEXT_RUNNING: return "RUNNING";
    case EL0_CONTEXT_YIELDED: return "YIELDED";
    case EL0_CONTEXT_EXITED: return "EXITED";
    case EL0_CONTEXT_FAULTED: return "FAULTED";
    default: return "INVALID";
  }
}

static void fault_register(const char *name, uint64_t value) {
  console_printf("  %s: 0x", name);
  console_puthex64(value);
  console_putc('\n');
}

static void show_fault(const el0_context_info_t *info) {
  el0_context_fault_t fault;
  if (!el0_context_get_fault(&fault))
    return;
  fault_register("Fault EC           ", (fault.frame.esr >> 26) & 0x3fU);
  fault_register("Fault PC           ", fault.frame.elr);
  fault_register("Fault SP           ", fault.frame.sp);
  fault_register("ESR_EL1            ", fault.frame.esr);
  fault_register("SPSR_EL1           ", fault.frame.spsr);
  if (fault.far_valid)
    fault_register("FAR_EL1            ", fault.frame.far);
  else
    console_puts("  FAR_EL1            : unavailable for this exception\n");
  if (info->fault_test)
    console_printf("EL0 fault containment test: %s\n",
        info->fault_test_passed ? "PASS" : "FAIL");
  console_puts("EL0 context terminated; reset required.\n");
}

static void show_context(void) {
  el0_context_info_t info;
  el0_context_get_info(&info);
  console_printf("EL0 context: %s\n", state_name(info.state));
  if (info.state == EL0_CONTEXT_EMPTY)
    return;
  console_printf("  Resumptions        : %llu\n",
      (unsigned long long)info.resumptions);
  console_printf("  Yields             : %llu\n",
      (unsigned long long)info.yields);
  console_printf(info.state == EL0_CONTEXT_FAULTED ?
      "  Last yield result  : %llu\n" : "  Calculation result : %llu\n",
      (unsigned long long)info.result);
  if (info.state != EL0_CONTEXT_FAULTED)
    console_printf("  Stack contents     : %s\n",
        info.stack_ok ? "OK" : "FAIL");
  console_puts("  Context mismatches : 0x");
  console_puthex64(info.context_failures);
  console_puts("\n  Kernel mismatches  : 0x");
  console_puthex64(info.kernel_failures);
  console_putc('\n');
  if (info.state == EL0_CONTEXT_FAULTED) {
    show_fault(&info);
    return;
  }
  if (info.fault_test && info.state == EL0_CONTEXT_EXITED) {
    console_puts("EL0 fault containment test: FAIL "
        "(expected fault missing)\n");
    return;
  }
  if (info.state == EL0_CONTEXT_EXITED || info.context_failures ||
      info.kernel_failures || !info.stack_ok)
    console_printf("EL0 cooperative test: %s\n",
        info.passed ? "PASS" : "FAIL");
}

static bool context_command(const char *args, const char *name) {
  return console_arg_word(&args, name) && console_arg_end(args);
}

static bool fault_start_command(const char *args) {
  return console_arg_word(&args, "start") &&
      console_arg_word(&args, "fault") && console_arg_end(args);
}

void cmd_el0ctx(const char *args) {
  if (console_arg_end(args) ||
      context_command(args, "status")) {
    show_context();
    return;
  }
  if (context_command(args, "start") || fault_start_command(args)) {
    bool started = fault_start_command(args) ?
        el0_context_start_fault_test() : el0_context_start();
    if (!started) {
      console_puts("Cannot start EL0 context: "
          "stack busy or mappings invalid.\n");
      return;
    }
    show_context();
    return;
  }
  if (context_command(args, "resume")) {
    if (!el0_context_resume()) {
      console_puts("Cannot resume EL0 context: "
          "not ready or context checks failed.\n");
      return;
    }
    show_context();
    return;
  }
  if (context_command(args, "reset")) {
    if (!el0_context_reset()) {
      console_puts("Cannot reset a running EL0 context.\n");
      return;
    }
    console_puts("EL0 context: EMPTY\n");
    return;
  }
  console_puts("Usage: el0ctx [start [fault] | resume | status | reset]\n");
}
