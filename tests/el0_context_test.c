#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>

// Synthetic linker symbols are supplied by el0_context_symbols.S.
// Only the stack page is backed by host memory.
#include "../src/el0_context.c"
#include "../src/console_el0_context.c"

const char exception_test_resume[1] = {0};
static bool mappings = true, legacy_active, reject_traps;
static unsigned int switches;
static int current_reg = -1, resumed_reg = -1;
static uint64_t current_bit, snapshot_bit, kernel_mismatch;
static bool corrupt_guard, early_exit, skip_fault;
static bool inject_arbitrary_fault;
static exception_frame_t arbitrary_fault;
static uint64_t fault_esr_xor;
static intptr_t fault_far_delta, fault_pc_delta;
static int fault_reg = -1;
volatile uint64_t el0_test_kernel_canary = EL0_TEST_KERNEL_CANARY;
static jmp_buf panic_return;
static uint64_t panic_args[5];

static char output[8192];
static size_t output_length;

void console_puts(const char *text) {
  size_t length = strlen(text);
  assert(output_length + length < sizeof(output));
  memcpy(output + output_length, text, length + 1U);
  output_length += length;
}

void console_putc(char c) { char text[] = {c, '\0'}; console_puts(text); }
void console_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int length = vsnprintf(output + output_length,
      sizeof(output) - output_length, format, args);
  va_end(args);
  assert(length >= 0 && (size_t)length < sizeof(output) - output_length);
  output_length += (size_t)length;
}
void console_puthex64(uint64_t value) {
  console_printf("%016llX", (unsigned long long)value);
}

static void command(const char *args) {
  output_length = 0;
  output[0] = '\0';
  cmd_el0ctx(args);
}

bool el0_test_active(void) { return legacy_active; }
bool el0_test_mappings_ready(void) { return mappings; }
bool el0_test_handle_exception(exception_frame_t *frame) {
  (void)frame;
  return false;
}

__attribute__((noreturn))
void exception_panic(uint64_t vector, uint64_t esr, uint64_t elr,
                     uint64_t far, uint64_t spsr) {
  panic_args[0] = vector;
  panic_args[1] = esr;
  panic_args[2] = elr;
  panic_args[3] = far;
  panic_args[4] = spsr;
  longjmp(panic_return, 1);
}

static void expect_panic(exception_frame_t frame) {
  exception_frame_t before = frame, saved_before = saved;
  el0_context_info_t info_before = info;
  if (setjmp(panic_return) == 0) {
    exception_dispatch(&frame);
    assert(!"unowned exception returned");
  }
  assert(memcmp(&frame, &before, sizeof(frame)) == 0);
  assert(memcmp(&saved, &saved_before, sizeof(saved)) == 0);
  assert(memcmp(&info, &info_before, sizeof(info)) == 0);
  assert(panic_args[0] == frame.vector);
  assert(panic_args[1] == frame.esr);
  assert(panic_args[2] == frame.elr);
  assert(panic_args[3] == frame.far);
  assert(panic_args[4] == frame.spsr);
}

static exception_frame_t yield_frame(void) {
  return (exception_frame_t){
    .vector = 8, .spsr = 0xa0000340U,
    .elr = 0x100080U, .sp = 0x101ed0U, .sp_el0 = 0x101ed0U,
    .esr = (0x15ULL << 26) | (1ULL << 25) | EL0_CONTEXT_YIELD_SVC,
  };
}

uint64_t el0_context_switch(void) {
  switches++;
  exception_frame_t kernel = {
    .sp = 0x180000U, .sp_el0 = 0xdeadbeefU,
    .elr = 0x80004U,
    // Show that each kernel return restores its own masks while the EL0
    // context retains the masks established on its initial entry.
    .spsr = info.resumptions ? 0x600003c5U : 0x60000345U,
    .esr = 0x9876U, .far = 0x1234U, .vector = 4U,
  };
  for (unsigned int i = 0; i < 31U; i++)
    kernel.x[i] = 0xfedcba9800000000ULL + i;
  exception_frame_t frame = *el0_context_enter(&kernel);
  assert(info.state == EL0_CONTEXT_RUNNING && el0_context_owns_stack());
  assert(!el0_context_start() && !el0_context_reset());
  assert(!el0_context_resume());
  assert(frame.spsr == 0xa0000340U);
  if (inject_arbitrary_fault) {
    frame = arbitrary_fault;
    goto dispatch_frame;
  }

  uint64_t *stack = (uint64_t *)(uintptr_t)0x101ed0U;
  if (info.resumptions == 1U) {
    assert(frame.elr == (info.fault_test ? 0x100010U : 0x100000U));
    assert(frame.sp_el0 == 0x102000U);
    assert(frame.x[0] == 0 && frame.x[1] == 0);
    frame.sp = frame.sp_el0 = 0x101ed0U;
    stack[34] = 0;
    stack[35] = 0x9abc567812340102ULL;
    stack[36] = info.fault_test ? 1 : 0;
  } else {
    assert(frame.elr == 0x100080U && frame.sp_el0 == 0x101ed0U);
    assert(frame.x[0] == (info.resumptions - 1U) * 7U);
    assert(frame.x[1] == info.resumptions - 1U);
    for (unsigned int i = 0; i < 31U; i++)
      stack[i] = frame.x[i];
    stack[31] = frame.spsr & 0xf0000000ULL;
    stack[32] = frame.sp_el0;
    if (resumed_reg >= 0)
      stack[resumed_reg] ^= 1ULL << 48;
    if (snapshot_bit == 31U)
      stack[32] -= 16U;
    if (snapshot_bit == 32U)
      stack[31] ^= 1ULL << 29;
  }
  if (info.fault_test && info.resumptions == 2U) {
    frame.vector = 8;
    if (skip_fault) {
      frame.esr = (0x15ULL << 26) | (1ULL << 25) | EL0_CONTEXT_EXIT_SVC;
      frame.elr = 0x100100U;
    } else {
      frame.x[3] = (uintptr_t)&el0_test_kernel_canary;
      frame.elr = 0x100090U + fault_pc_delta;
      frame.esr = 0x9200000fU ^ fault_esr_xor;
      frame.far = (uintptr_t)&el0_test_kernel_canary + fault_far_delta;
      if (fault_reg >= 0)
        frame.x[fault_reg] ^= 1ULL << 48;
    }
    if (corrupt_guard)
      stack[35] ^= 1;
    goto dispatch_frame;
  }
  stack[34] += 7;
  frame.x[0] = stack[34];
  frame.x[1]++;
  frame.vector = 8;
  bool exit = early_exit || info.resumptions == 6U;
  frame.elr = exit ? 0x100100U : 0x100080U;
  frame.esr = (0x15ULL << 26) | (1ULL << 25) |
      (exit ? EL0_CONTEXT_EXIT_SVC : EL0_CONTEXT_YIELD_SVC);
  if (reject_traps) {
    exception_frame_t bad = frame;
    bad.vector = 4;
    expect_panic(bad);
    bad = frame;
    bad.vector = 9;
    expect_panic(bad);
    bad = frame;
    bad.spsr |= 5;
    expect_panic(bad);
    bad = frame;
    bad.spsr |= 0x10;
    expect_panic(bad);
    bad = frame;
    bad.esr = 0x9600000fU;
    bad.spsr = 5;
    bad.vector = 4; // A kernel abort while an EL0 context is active.
    expect_panic(bad);
    bad = frame;
    bad.esr = 0xbc000000U; // SError remains a system-level failure.
    expect_panic(bad);
    bad.vector = 11;
    expect_panic(bad);
    exception_frame_t *live_return = kernel_return;
    kernel_return = NULL; // RUNNING alone is insufficient ownership.
    expect_panic(frame);
    kernel_return = live_return;
  }
  if (current_reg >= 0)
    frame.x[current_reg] ^= 1ULL << 48;
  switch (current_bit) {
    case 31: frame.sp_el0 -= 16; break;
    case 32: frame.spsr ^= 1ULL << 29; break;
    case 33: frame.spsr ^= 1ULL << 7; break;
    case 34: frame.elr += 4; break;
  }
  if (corrupt_guard)
    stack[35] ^= 1;
dispatch_frame:
  exception_dispatch(&frame);
  assert(kernel_return == NULL);
  assert(memcmp(&kernel, &frame, sizeof(frame)) == 0);
  return kernel_mismatch;
}

static void reset_start(void) {
  assert(el0_context_reset());
  assert(el0_context_start());
}

static void start_fault_run(void) {
  assert(el0_context_reset());
  assert(el0_context_start_fault_test());
  assert(el0_context_resume());
  assert(info.state == EL0_CONTEXT_YIELDED && info.result == 7);
}

static void check_contained_fault(exception_frame_t frame, bool far_valid) {
  reset_start();
  assert(mprotect((void *)0x101000U, 4096U, PROT_NONE) == 0);
  inject_arbitrary_fault = true;
  arbitrary_fault = frame;
  assert(el0_context_resume());
  inject_arbitrary_fault = false;
  assert(info.state == EL0_CONTEXT_FAULTED && info.result == 0);
  assert(info.resumptions == 1 && info.yields == 0 && !info.passed);
  assert(el0_context_owns_stack());
  el0_context_fault_t fault;
  assert(el0_context_get_fault(&fault));
  assert(memcmp(&fault.frame, &frame, sizeof(frame)) == 0);
  assert(fault.far_valid == far_valid);
  command("status"); // Logging must not follow the inaccessible user SP.
  assert(strstr(output, "EL0 context: FAULTED\n"));
  if (!far_valid)
    assert(strstr(output, "unavailable for this exception"));
  assert(!el0_context_resume() && !el0_context_start());
  assert(!el0_context_start_fault_test());
  el0_context_fault_t retained;
  assert(el0_context_get_fault(&retained));
  assert(memcmp(&retained.frame, &frame, sizeof(frame)) == 0);
  expect_panic(frame); // The continuation is no longer live after return.
  assert(el0_context_reset());
  assert(!el0_context_get_fault(&fault) && !fault.far_valid);
  assert(fault.frame.esr == 0 && fault.frame.elr == 0);
  assert(mprotect((void *)0x101000U, 4096U, PROT_READ | PROT_WRITE) == 0);
}

int main(void) {
  void *stack_page = mmap((void *)0x101000U, 4096U,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
      MAP_FIXED_NOREPLACE, -1, 0);
  assert(stack_page == (void *)0x101000U);
  assert(!el0_context_resume() && !el0_context_owns_stack());
  expect_panic(yield_frame());
  el0_context_get_info(NULL);
  mappings = false;
  assert(!el0_context_start() && info.state == EL0_CONTEXT_EMPTY);
  mappings = true;
  legacy_active = true;
  assert(!el0_context_start() && info.state == EL0_CONTEXT_EMPTY);
  legacy_active = false;
  assert(el0_context_start() && info.state == EL0_CONTEXT_READY);
  assert(el0_context_owns_stack() && !el0_context_start());
  reject_traps = true;
  for (unsigned int i = 1; i <= 6; i++) {
    assert(el0_context_resume());
    el0_context_info_t result;
    el0_context_get_info(&result);
    assert(result.resumptions == i && result.result == i * 7U);
    assert(result.yields == (i < 6U ? i : 5U));
    assert(result.state ==
        (i < 6U ? EL0_CONTEXT_YIELDED : EL0_CONTEXT_EXITED));
    assert(result.context_failures == 0 && result.kernel_failures == 0);
    assert(result.stack_ok && result.passed == (i == 6U));
    assert(el0_context_owns_stack() == (i < 6U));
    if (i < 6U) {
      unsigned char before[4096];
      memcpy(before, stack_page, sizeof(before));
      assert(!el0_context_start());
      assert(memcmp(before, stack_page, sizeof(before)) == 0);
      expect_panic(yield_frame()); // No running context between calls.
    }
  }
  assert(!el0_context_resume() && switches == 6U);
  expect_panic(yield_frame());
  reject_traps = false;
  // Starting after exit clears the entire page and all accumulated state.
  assert(el0_context_start());
  for (unsigned int i = 0; i < 512; i++)
    assert(((uint64_t *)stack_page)[i] == 0);
  assert(info.resumptions == 0 && info.yields == 0 && !info.passed);
  assert(el0_context_reset() && !el0_context_owns_stack());

  for (int i = 0; i < 31; i++) {
    reset_start();
    current_reg = i;
    assert(el0_context_resume());
    assert(info.context_failures == (1ULL << i));
    assert(!el0_context_resume());
    current_reg = -1;
    reset_start();
    assert(el0_context_resume());
    resumed_reg = i;
    assert(el0_context_resume());
    assert(info.context_failures == (1ULL << i));
    assert(!el0_context_resume());
    resumed_reg = -1;
  }
  for (current_bit = 31; current_bit <= 34; current_bit++) {
    reset_start();
    assert(el0_context_resume());
    assert(info.context_failures == (1ULL << current_bit));
    assert(!el0_context_resume());
  }
  current_bit = 0;
  for (snapshot_bit = 31; snapshot_bit <= 32; snapshot_bit++) {
    reset_start();
    assert(el0_context_resume());
    assert(el0_context_resume());
    assert(info.context_failures == (1ULL << snapshot_bit));
  }
  snapshot_bit = 0;
  reset_start();
  corrupt_guard = true;
  assert(el0_context_resume() && !info.stack_ok && !info.passed);
  assert(!el0_context_resume());
  corrupt_guard = false;
  reset_start();
  kernel_mismatch = 1ULL << 19;
  assert(el0_context_resume() && info.kernel_failures == kernel_mismatch);
  assert(!el0_context_resume());
  kernel_mismatch = 0;
  reset_start();
  early_exit = true;
  assert(el0_context_resume() && info.context_failures == (1ULL << 35));
  assert(!info.passed && !el0_context_resume());
  early_exit = false;
  reset_start();
  assert(el0_context_resume());
  assert(el0_context_reset() && !el0_context_owns_stack());
  assert(el0_context_start());
  for (unsigned int i = 0; i < 6; i++)
    assert(el0_context_resume());
  assert(info.passed);
  command("reset");
  assert(strcmp(output, "EL0 context: EMPTY\n") == 0);
  command("start");
  assert(strstr(output, "EL0 context: READY\n"));
  command("resume");
  assert(strstr(output, "EL0 context: YIELDED\n"));
  assert(info.resumptions == 1 && info.result == 7);
  const char *invalid[] = {"status start", "start reset", "resume resume",
    "reset start", "invalid", "status extra", "resume start",
    "start fault extra", "start fault resume"};
  for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    command(invalid[i]);
    assert(strstr(output, "Usage: el0ctx"));
    assert(info.state == EL0_CONTEXT_YIELDED && info.resumptions == 1);
  }
  command("status");
  assert(strstr(output, "EL0 context: YIELDED\n"));
  assert(info.resumptions == 1);
  command("start");
  assert(strstr(output, "Cannot start EL0 context"));
  assert(info.resumptions == 1);
  for (unsigned int i = 0; i < 5; i++)
    command("resume");
  assert(strstr(output, "EL0 context: EXITED\n"));
  assert(strstr(output, "EL0 cooperative test: PASS\n"));
  command("resume");
  assert(strstr(output, "Cannot resume EL0 context"));
  // Faults are contained by origin/lifetime, including invalid user PCs
  // and SPs. Protect the user stack to prove the generic path never reads
  // it, even when the fault precedes payload stack initialisation.
  exception_frame_t fault_frame = yield_frame();
  fault_frame.elr = 0xfffffffffffff000ULL;
  fault_frame.sp = fault_frame.sp_el0 = 0xdeadbeefU;
  fault_frame.far = 0xabcdefU;
  for (unsigned int i = 0; i < 31U; i++)
    fault_frame.x[i] = 0x1234567800000000ULL + i;
  const struct { uint64_t esr; bool far_valid; } fault_cases[] = {
    {0x9200000fU, true},  // Data permission fault.
    {0x92000007U, true},  // Data translation fault.
    {0x8200000fU, true},  // Instruction abort, PC outside the program.
    {0x02000000U, false}, // Undefined: FAR may be stale.
    {0x56000042U, false}, // Unknown SVC number.
    {0x54000040U, false}, // Malformed SVC syndrome.
    {0x56000040U, false}, // Yield at an unauthorised PC.
    {0x56000041U, false}, // Exit at an unauthorised PC.
    {0x92000410U, false}, // External data abort with FnV.
    {0x82000410U, false}, // Instruction abort with FnV.
    {0x8a000000U, true},  // PC alignment sets FAR.
    {0xd2000000U, true},  // Watchpoint with valid FAR.
    {0xd2000400U, false}, // Watchpoint with FnV.
    {0xf2000000U, false}, // BRK does not set FAR.
    {0x9a000000U, false}, // SP alignment does not set FAR.
  };
  for (unsigned int i = 0;
       i < sizeof(fault_cases) / sizeof(fault_cases[0]); i++) {
    fault_frame.esr = fault_cases[i].esr;
    check_contained_fault(fault_frame, fault_cases[i].far_valid);
  }
  fault_frame.elr = 0x100080U;
  fault_frame.esr = 0x56000042U; // Unknown SVC even at an allowed code PC.
  check_contained_fault(fault_frame, false);
  assert(!el0_context_get_fault(NULL));
  start_fault_run();
  assert(el0_context_resume());
  assert(info.state == EL0_CONTEXT_FAULTED && info.fault_test_passed);
  assert(info.resumptions == 2 && info.yields == 1 && info.result == 7);
  assert(info.context_failures == 0 && info.kernel_failures == 0);
  command("status");
  assert(strstr(output, "EL0 context: FAULTED\n"));
  assert(strstr(output, "EL0 fault containment test: PASS\n"));
  command("resume");
  assert(strstr(output, "Cannot resume EL0 context"));
  command("start");
  assert(strstr(output, "Cannot start EL0 context"));
  command("start fault");
  assert(strstr(output, "Cannot start EL0 context"));
  el0_context_fault_t retained;
  assert(el0_context_get_fault(&retained));
  assert(retained.frame.elr == 0x100090U);
  assert(retained.frame.esr == 0x9200000fU);
  assert(retained.frame.far == (uintptr_t)&el0_test_kernel_canary);
  assert(retained.far_valid);
  assert(el0_context_reset());
  assert(!el0_context_get_fault(&retained));

  // A different fault is contained but never reported as this test PASS.
  const uint64_t bad_esr[] = {1ULL << 26, 1ULL << 25, 1ULL << 10,
      1ULL << 9, 1ULL << 8, 1ULL << 7, 1ULL << 6, 1ULL, 1ULL << 3};
  for (unsigned int i = 0; i < sizeof(bad_esr) / sizeof(bad_esr[0]); i++) {
    start_fault_run();
    fault_esr_xor = bad_esr[i];
    assert(el0_context_resume());
    assert(info.state == EL0_CONTEXT_FAULTED && !info.fault_test_passed);
  }
  fault_esr_xor = 0;
  start_fault_run();
  fault_far_delta = 8;
  assert(el0_context_resume() && !info.fault_test_passed);
  fault_far_delta = 0;
  start_fault_run();
  fault_pc_delta = 0x100000;
  assert(el0_context_resume() && !info.fault_test_passed);
  fault_pc_delta = 0;
  for (fault_reg = 0; fault_reg < 31; fault_reg++) {
    start_fault_run();
    assert(el0_context_resume() && !info.fault_test_passed);
    assert(info.context_failures == (1ULL << fault_reg));
  }
  fault_reg = -1;
  start_fault_run();
  resumed_reg = 3;
  assert(el0_context_resume() && !info.fault_test_passed);
  resumed_reg = -1;
  start_fault_run();
  kernel_mismatch = 1ULL << 19;
  assert(el0_context_resume() && !info.fault_test_passed);
  kernel_mismatch = 0;
  start_fault_run();
  corrupt_guard = true;
  assert(el0_context_resume() && !info.fault_test_passed);
  corrupt_guard = false;
  start_fault_run();
  el0_test_kernel_canary ^= 1;
  assert(el0_context_resume() && !info.fault_test_passed);
  el0_test_kernel_canary = EL0_TEST_KERNEL_CANARY;
  start_fault_run();
  skip_fault = true;
  assert(el0_context_resume());
  assert(info.state == EL0_CONTEXT_EXITED && !info.fault_test_passed);
  assert(!el0_context_get_fault(&retained) && !info.passed);
  command("status");
  assert(strstr(output, "expected fault missing"));
  skip_fault = false;
  command("reset");
  command("start fault");
  assert(info.state == EL0_CONTEXT_READY && info.fault_test);
  command("resume");
  command("resume");
  assert(strstr(output, "EL0 fault containment test: PASS\n"));
  command("reset");
  command("start");
  assert(!info.fault_test);
  for (unsigned int i = 0; i < 6; i++)
    command("resume");
  assert(info.passed && !info.fault_test_passed);
  assert(strstr(output, "EL0 cooperative test: PASS\n"));
  assert(munmap(stack_page, 4096U) == 0);
  puts("EL0 context tests: PASS");
  return 0;
}
