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

static void show_fault(unsigned int id, const el0_context_info_t *info) {
  el0_context_fault_t fault;
  if (!el0_context_get_fault(id, &fault))
    return;
  console_printf("  Fault owner        : %u\n", fault.context_id);
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

static void show_context(unsigned int id) {
  el0_context_info_t info;
  el0_context_get_info(id, &info);
  console_printf("EL0 context %u: %s\n", id, state_name(info.state));
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
    show_fault(id, &info);
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

// Parse before calling the mechanism. A missing ID preserves context-0
// shorthand for mutations; status without an ID reports both contexts.
static bool parse_id(const char **args, unsigned int *id) {
  if (!console_arg_uint(args, id) || *id >= EL0_CONTEXT_COUNT) {
    console_puts("Invalid EL0 context ID; use 0 or 1.\n");
    return false;
  }
  return true;
}

void cmd_el0ctx(const char *args) {
  if (console_arg_end(args) || console_arg_word(&args, "status")) {
    if (console_arg_end(args)) {
      for (unsigned int id = 0; id < EL0_CONTEXT_COUNT; id++)
        show_context(id);
    } else {
      unsigned int id;
      if (!parse_id(&args, &id))
        return;
      if (!console_arg_end(args))
        goto usage;
      show_context(id);
    }
    return;
  }
  if (console_arg_word(&args, "start")) {
    unsigned int id = 0;
    bool fault = console_arg_word(&args, "fault"); // Legacy: start fault.
    if (!fault && !console_arg_end(args)) {
      if (!parse_id(&args, &id))
        return;
      fault = console_arg_word(&args, "fault");
    }
    if (!console_arg_end(args))
      goto usage;
    bool started = fault ? el0_context_start_fault_test(id) :
        el0_context_start(id);
    if (!started) {
      console_puts("Cannot start EL0 context: "
          "stack busy or mappings invalid.\n");
      return;
    }
    show_context(id);
    return;
  }
  bool resume = console_arg_word(&args, "resume");
  if (resume || console_arg_word(&args, "reset")) {
    unsigned int id = 0;
    if (!console_arg_end(args) && !parse_id(&args, &id))
      return;
    if (!console_arg_end(args))
      goto usage;
    if (resume) {
      if (!el0_context_resume(id)) {
        console_puts("Cannot resume EL0 context: "
            "not ready or context checks failed.\n");
        return;
      }
      show_context(id);
    } else {
      if (!el0_context_reset(id)) {
        console_puts("Cannot reset during an EL0 transition.\n");
        return;
      }
      console_printf("EL0 context %u: EMPTY\n", id);
    }
    return;
  }
usage:
  console_puts("Usage: el0ctx [start [0|1] [fault] | "
      "resume [0|1] | status [0|1] | reset [0|1]]\n");
}
