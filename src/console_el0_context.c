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
    default: return "INVALID";
  }
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
  console_printf("  Calculation result : %llu\n",
      (unsigned long long)info.result);
  console_printf("  Stack contents     : %s\n", info.stack_ok ? "OK" : "FAIL");
  console_puts("  Context mismatches : 0x");
  console_puthex64(info.context_failures);
  console_puts("\n  Kernel mismatches  : 0x");
  console_puthex64(info.kernel_failures);
  console_putc('\n');
  if (info.state == EL0_CONTEXT_EXITED || info.context_failures ||
      info.kernel_failures || !info.stack_ok)
    console_printf("EL0 cooperative test: %s\n",
        info.passed ? "PASS" : "FAIL");
}

static bool context_command(const char *args, const char *name) {
  return console_arg_word(&args, name) && console_arg_end(args);
}

void cmd_el0ctx(const char *args) {
  if (console_arg_end(args) ||
      context_command(args, "status")) {
    show_context();
    return;
  }
  if (context_command(args, "start")) {
    if (!el0_context_start()) {
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
  console_puts("Usage: el0ctx [start | resume | status | reset]\n");
}
