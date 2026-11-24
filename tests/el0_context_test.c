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
  exception_frame_t before = frame;
  el0_slot_t contexts_before[EL0_CONTEXT_COUNT];
  memcpy(contexts_before, contexts, sizeof(contexts));
  if (setjmp(panic_return) == 0) {
    exception_dispatch(&frame);
    assert(!"unowned exception returned");
  }
  assert(memcmp(&frame, &before, sizeof(frame)) == 0);
  assert(memcmp(contexts, contexts_before, sizeof(contexts)) == 0);
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
  assert(active);
  el0_slot_t *ctx = active;
  unsigned int id = (unsigned int)(ctx - contexts);
  el0_slot_t other_before = contexts[1U - id];
  exception_frame_t kernel = {
    .sp = 0x180000U, .sp_el0 = 0xdeadbeefU,
    .elr = 0x80004U,
    // Show that each kernel return restores its own masks while the EL0
    // context retains the masks established on its initial entry.
    .spsr = 0x60000005U | (ctx->info.resumptions ?
        (id ? 0x340U : 0x3c0U) : (id ? 0x3c0U : 0x340U)),
    .esr = 0x9876U, .far = 0x1234U, .vector = 4U,
  };
  for (unsigned int i = 0; i < 31U; i++)
    kernel.x[i] = 0xfedcba9800000000ULL + i;
  exception_frame_t frame = *el0_context_enter(&kernel);
  assert(ctx->info.state == EL0_CONTEXT_RUNNING && el0_context_owns_stack(id));
  for (unsigned int other = 0; other < EL0_CONTEXT_COUNT; other++) {
    assert(!el0_context_start(other) && !el0_context_reset(other));
    assert(!el0_context_resume(other));
  }
  assert(frame.spsr == (0xa0000000U | (id ? 0x3c0U : 0x340U)));
  if (inject_arbitrary_fault) {
    frame = arbitrary_fault;
    goto dispatch_frame;
  }

  uintptr_t top = 0x102000U + id * 4096U;
  uintptr_t sp = top - EL0_CONTEXT_STACK_BYTES;
  uint64_t *stack = (uint64_t *)sp;
  if (ctx->info.resumptions == 1U) {
    assert(frame.elr == (ctx->info.fault_test ? 0x100010U : 0x100000U));
    assert(frame.sp_el0 == top);
    assert(frame.x[0] == 0 && frame.x[1] == 0);
    frame.sp = frame.sp_el0 = sp;
    stack[34] = 0;
    stack[35] = frame.x[2];
    stack[37] = frame.x[3];
    for (unsigned int i = 2; i < 31; i++)
      assert(frame.x[i] == 0x9abc567812340100ULL +
          ((uint64_t)id << 32) + i);
    stack[36] = ctx->info.fault_test ? 1 : 0;
  } else {
    assert(frame.elr == 0x100080U && frame.sp_el0 == sp);
    assert(frame.x[0] == (ctx->info.resumptions - 1U) * 7U);
    assert(frame.x[1] == ctx->info.resumptions - 1U);
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
  if (ctx->info.fault_test && ctx->info.resumptions == 2U) {
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
  bool exit = early_exit || ctx->info.resumptions == 6U;
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
  assert(kernel_return == NULL && active == NULL);
  assert(memcmp(&kernel, &frame, sizeof(frame)) == 0);
  assert(memcmp(&other_before, &contexts[1U - id], sizeof(other_before)) == 0);
  return kernel_mismatch;
}

static void reset_start(void) {
  assert(el0_context_reset(0));
  assert(el0_context_start(0));
}

static void start_fault_run(void) {
  assert(el0_context_reset(0));
  assert(el0_context_start_fault_test(0));
  assert(el0_context_resume(0));
  assert(contexts[0].info.state == EL0_CONTEXT_YIELDED && contexts[0].info.result == 7);
}

static void check_contained_fault(unsigned int id, exception_frame_t frame,
                                  bool far_valid) {
  assert(el0_context_reset(id));
  assert(el0_context_start(id));
  void *page = (void *)(uintptr_t)(0x101000U + id * 4096U);
  assert(mprotect(page, 4096U, PROT_NONE) == 0);
  inject_arbitrary_fault = true;
  arbitrary_fault = frame;
  assert(el0_context_resume(id));
  inject_arbitrary_fault = false;
  assert(contexts[id].info.state == EL0_CONTEXT_FAULTED && contexts[id].info.result == 0);
  assert(contexts[id].info.resumptions == 1 && contexts[id].info.yields == 0 && !contexts[id].info.passed);
  assert(el0_context_owns_stack(id));
  el0_context_fault_t fault;
  assert(el0_context_get_fault(id, &fault));
  assert(memcmp(&fault.frame, &frame, sizeof(frame)) == 0);
  assert(fault.far_valid == far_valid && fault.context_id == id);
  command("status"); // Logging must not follow the inaccessible user SP.
  assert(strstr(output, id == 0U ? "EL0 context 0: FAULTED\n" :
      "EL0 context 1: FAULTED\n"));
  if (!far_valid)
    assert(strstr(output, "unavailable for this exception"));
  assert(!el0_context_resume(id) && !el0_context_start(id));
  assert(!el0_context_start_fault_test(id));
  el0_context_fault_t retained;
  assert(el0_context_get_fault(id, &retained));
  assert(memcmp(&retained.frame, &frame, sizeof(frame)) == 0);
  expect_panic(frame); // The continuation is no longer live after return.
  assert(el0_context_reset(id));
  assert(!el0_context_get_fault(id, &fault) && !fault.far_valid);
  assert(fault.frame.esr == 0 && fault.frame.elr == 0);
  assert(mprotect(page, 4096U, PROT_READ | PROT_WRITE) == 0);
}

// Check resources after the entire foreground API call, not just dispatch.
static void check_operation(unsigned int id, unsigned int operation,
                            bool fault) {
  el0_slot_t other = contexts[1U - id];
  unsigned char page[4096];
  void *other_page = (void *)(uintptr_t)(0x101000U + (1U - id) * 4096U);
  memcpy(page, other_page, sizeof(page));
  if (operation == 0U)
    assert(fault ? el0_context_start_fault_test(id) : el0_context_start(id));
  else if (operation == 1U)
    assert(el0_context_resume(id));
  else
    assert(el0_context_reset(id));
  assert(memcmp(&other, &contexts[1U - id], sizeof(other)) == 0);
  assert(memcmp(page, other_page, sizeof(page)) == 0);
  assert(active == NULL && kernel_return == NULL);
}

static void run_two_context_tests(void) {
  check_operation(0, 2, false);
  check_operation(1, 2, false);
  check_operation(0, 0, false);
  check_operation(1, 0, false);
  assert(contexts[0].saved.sp_el0 != contexts[1].saved.sp_el0);
  assert(contexts[0].saved.x[2] != contexts[1].saved.x[2]);
  const unsigned int order[] = {0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0};
  uint64_t counts[2] = {0};
  for (unsigned int n = 0; n < sizeof(order) / sizeof(order[0]); n++) {
    unsigned int id = order[n];
    check_operation(id, 1, false);
    counts[id]++;
    for (unsigned int slot = 0; slot < 2; slot++) {
      el0_context_info_t result;
      assert(el0_context_get_info(slot, &result));
      assert(result.resumptions == counts[slot]);
      assert(result.result == counts[slot] * 7U);
      assert(result.stack_ok && result.context_failures == 0);
      assert(result.kernel_failures == 0);
      assert(result.passed == (counts[slot] == 6U));
    }
  }
  assert(contexts[0].info.passed && contexts[1].info.passed);
  assert(!el0_context_resume(0) && !el0_context_resume(1));

  // All 924 possible interleavings of six slices per context. Every
  // operation must preserve the peer's complete state and stack bytes.
  unsigned int interleavings = 0;
  for (unsigned int order_bits = 0; order_bits < (1U << 12); order_bits++) {
    if (__builtin_popcount(order_bits) != 6)
      continue;
    check_operation(0, 2, false);
    check_operation(1, 2, false);
    check_operation(0, 0, false);
    check_operation(1, 0, false);
    for (unsigned int n = 0; n < 12; n++)
      check_operation((order_bits >> n) & 1U, 1, false);
    assert(contexts[0].info.passed && contexts[1].info.passed);
    interleavings++;
  }
  assert(interleavings == 924U);

  // Each faulted context retains its own frame while the peer completes.
  for (unsigned int bad = 0; bad < 2; bad++) {
    unsigned int good = 1U - bad;
    check_operation(0, 2, false);
    check_operation(1, 2, false);
    check_operation(bad, 0, true);
    check_operation(good, 0, false);
    check_operation(bad, 1, false);
    check_operation(good, 1, false);
    check_operation(bad, 1, false);
    assert(contexts[bad].info.state == EL0_CONTEXT_FAULTED);
    assert(contexts[bad].info.fault_test_passed);
    el0_context_fault_t record;
    assert(el0_context_get_fault(bad, &record) && record.context_id == bad);
    assert(record.frame.sp_el0 == 0x101ed0U + bad * 4096U);
    assert(!el0_context_get_fault(good, &record));
    assert(!el0_context_resume(bad) && !el0_context_start(bad));
    check_operation(good, 1, false);
    check_operation(bad, 2, false);
    assert(!el0_context_get_fault(bad, &record));
    check_operation(bad, 0, false);
    for (unsigned int n = 0; n < 4; n++)
      check_operation(good, 1, false);
    for (unsigned int n = 0; n < 6; n++)
      check_operation(bad, 1, false);
    assert(contexts[0].info.passed && contexts[1].info.passed);
  }

  check_operation(0, 2, false);
  check_operation(1, 2, false);
  for (unsigned int id = 0; id < 2; id++) {
    check_operation(id, 0, true);
    check_operation(id, 1, false);
  }
  check_operation(0, 1, false);
  check_operation(1, 1, false);
  el0_context_fault_t records[2], retained;
  assert(el0_context_get_fault(0, &records[0]));
  assert(el0_context_get_fault(1, &records[1]));
  assert(records[0].context_id == 0 && records[1].context_id == 1);
  assert(records[0].frame.x[2] != records[1].frame.x[2]);
  command("status");
  assert(strstr(output, "EL0 context 0: FAULTED"));
  assert(strstr(output, "EL0 context 1: FAULTED"));
  assert(strstr(output, "Fault owner        : 0"));
  assert(strstr(output, "Fault owner        : 1"));
  check_operation(0, 2, false);
  assert(el0_context_get_fault(1, &retained));
  assert(memcmp(&retained.frame, &records[1].frame, sizeof(retained.frame)) == 0);
  check_operation(0, 0, false);
  for (unsigned int n = 0; n < 6; n++)
    check_operation(0, 1, false);
  assert(contexts[0].info.passed && contexts[1].info.state == EL0_CONTEXT_FAULTED);

  // A generic fault before stack setup in context 1 also leaves 0 alone.
  check_operation(1, 2, false);
  check_operation(0, 0, false);
  check_operation(0, 1, false);
  el0_slot_t peer = contexts[0];
  unsigned char peer_page[4096];
  memcpy(peer_page, (void *)0x101000U, sizeof(peer_page));
  exception_frame_t arbitrary = yield_frame();
  arbitrary.elr = 0xfffff000U;
  arbitrary.sp = arbitrary.sp_el0 = 3;
  arbitrary.esr = 0x8200000fU;
  arbitrary.far = arbitrary.elr;
  check_contained_fault(1, arbitrary, true);
  assert(memcmp(&peer, &contexts[0], sizeof(peer)) == 0);
  assert(memcmp(peer_page, (void *)0x101000U, sizeof(peer_page)) == 0);
  for (unsigned int n = 0; n < 5; n++)
    check_operation(0, 1, false);
  assert(contexts[0].info.passed);

  // Invalid APIs and console IDs cannot change either slot or stack page.
  el0_slot_t before[2];
  memcpy(before, contexts, sizeof(before));
  unsigned char pages[8192];
  memcpy(pages, (void *)0x101000U, sizeof(pages));
  const unsigned int invalid[] = {2U, 3U, UINT32_MAX};
  for (unsigned int n = 0; n < sizeof(invalid) / sizeof(invalid[0]); n++) {
    unsigned int id = invalid[n];
    el0_context_info_t result;
    assert(!el0_context_start(id) && !el0_context_start_fault_test(id));
    assert(!el0_context_resume(id) && !el0_context_reset(id));
    assert(!el0_context_owns_stack(id));
    assert(!el0_context_get_info(id, &result) && result.state == EL0_CONTEXT_EMPTY);
    assert(!el0_context_get_fault(id, &retained));
  }
  const char *commands[] = {"start 2", "resume 2", "reset 2", "status 2",
      "start -1", "resume 4294967295", "resume 4294967296", "status -1",
      "start 1 fault extra", "reset 1 0", "resume 0 1", "start fault 1"};
  for (unsigned int n = 0; n < sizeof(commands) / sizeof(commands[0]); n++) {
    command(commands[n]);
    assert(strstr(output, "Invalid EL0") || strstr(output, "Usage:"));
  }
  assert(memcmp(before, contexts, sizeof(before)) == 0);
  assert(memcmp(pages, (void *)0x101000U, sizeof(pages)) == 0);

  // A mistaken cross-slot frame restoration must fail preservation checks.
  check_operation(0, 2, false);
  check_operation(1, 2, false);
  check_operation(0, 0, false);
  check_operation(1, 0, false);
  check_operation(0, 1, false);
  check_operation(1, 1, false);
  inject_arbitrary_fault = true;
  arbitrary_fault = contexts[1].saved;
  check_operation(0, 1, false);
  inject_arbitrary_fault = false;
  assert(contexts[0].info.context_failures != 0);
  assert(!el0_context_resume(0));
  for (unsigned int n = 0; n < 5; n++)
    check_operation(1, 1, false);
  assert(contexts[1].info.passed);

  command("reset 0");
  command("reset 1");
  command("start"); // Legacy mutations still select context 0.
  command("start 1");
  command("resume 1");
  assert(contexts[1].info.result == 7 && contexts[0].info.resumptions == 0);
  command("resume");
  assert(contexts[0].info.result == 7);
  command("status");
  assert(strstr(output, "EL0 context 0: YIELDED"));
  assert(strstr(output, "EL0 context 1: YIELDED"));
  command("status 1");
  assert(strstr(output, "EL0 context 1: YIELDED"));
  assert(!strstr(output, "EL0 context 0:"));
  command("reset 0");
  command("reset 1");
  command("start 1 fault");
  command("resume 1");
  command("resume 1");
  assert(strstr(output, "EL0 fault containment test: PASS"));
  assert(strstr(output, "Fault owner        : 1"));
  assert(contexts[0].info.state == EL0_CONTEXT_EMPTY);
  command("reset 1");
}

int main(void) {
  void *stack_page = mmap((void *)0x101000U, 8192U,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS |
      MAP_FIXED_NOREPLACE, -1, 0);
  assert(stack_page == (void *)0x101000U);
  assert(!el0_context_resume(0) && !el0_context_owns_stack(0));
  expect_panic(yield_frame());
  el0_context_get_info(0, NULL);
  mappings = false;
  assert(!el0_context_start(0) && contexts[0].info.state == EL0_CONTEXT_EMPTY);
  mappings = true;
  legacy_active = true;
  assert(!el0_context_start(0) && contexts[0].info.state == EL0_CONTEXT_EMPTY);
  legacy_active = false;
  assert(el0_context_start(0) && contexts[0].info.state == EL0_CONTEXT_READY);
  assert(el0_context_owns_stack(0) && !el0_context_start(0));
  reject_traps = true;
  for (unsigned int i = 1; i <= 6; i++) {
    assert(el0_context_resume(0));
    el0_context_info_t result;
    el0_context_get_info(0, &result);
    assert(result.resumptions == i && result.result == i * 7U);
    assert(result.yields == (i < 6U ? i : 5U));
    assert(result.state ==
        (i < 6U ? EL0_CONTEXT_YIELDED : EL0_CONTEXT_EXITED));
    assert(result.context_failures == 0 && result.kernel_failures == 0);
    assert(result.stack_ok && result.passed == (i == 6U));
    assert(el0_context_owns_stack(0) == (i < 6U));
    if (i < 6U) {
      unsigned char before[4096];
      memcpy(before, stack_page, sizeof(before));
      assert(!el0_context_start(0));
      assert(memcmp(before, stack_page, sizeof(before)) == 0);
      expect_panic(yield_frame()); // No running context between calls.
    }
  }
  assert(!el0_context_resume(0) && switches == 6U);
  expect_panic(yield_frame());
  reject_traps = false;
  // Starting after exit clears the entire page and all accumulated state.
  assert(el0_context_start(0));
  for (unsigned int i = 0; i < 512; i++)
    assert(((uint64_t *)stack_page)[i] == 0);
  assert(contexts[0].info.resumptions == 0 && contexts[0].info.yields == 0 && !contexts[0].info.passed);
  assert(el0_context_reset(0) && !el0_context_owns_stack(0));

  for (int i = 0; i < 31; i++) {
    reset_start();
    current_reg = i;
    assert(el0_context_resume(0));
    assert(contexts[0].info.context_failures == (1ULL << i));
    assert(!el0_context_resume(0));
    current_reg = -1;
    reset_start();
    assert(el0_context_resume(0));
    resumed_reg = i;
    assert(el0_context_resume(0));
    assert(contexts[0].info.context_failures == (1ULL << i));
    assert(!el0_context_resume(0));
    resumed_reg = -1;
  }
  for (current_bit = 31; current_bit <= 34; current_bit++) {
    reset_start();
    assert(el0_context_resume(0));
    assert(contexts[0].info.context_failures == (1ULL << current_bit));
    assert(!el0_context_resume(0));
  }
  current_bit = 0;
  for (snapshot_bit = 31; snapshot_bit <= 32; snapshot_bit++) {
    reset_start();
    assert(el0_context_resume(0));
    assert(el0_context_resume(0));
    assert(contexts[0].info.context_failures == (1ULL << snapshot_bit));
  }
  snapshot_bit = 0;
  reset_start();
  corrupt_guard = true;
  assert(el0_context_resume(0) && !contexts[0].info.stack_ok && !contexts[0].info.passed);
  assert(!el0_context_resume(0));
  corrupt_guard = false;
  reset_start();
  kernel_mismatch = 1ULL << 19;
  assert(el0_context_resume(0) && contexts[0].info.kernel_failures == kernel_mismatch);
  assert(!el0_context_resume(0));
  kernel_mismatch = 0;
  reset_start();
  early_exit = true;
  assert(el0_context_resume(0) && contexts[0].info.context_failures == (1ULL << 35));
  assert(!contexts[0].info.passed && !el0_context_resume(0));
  early_exit = false;
  reset_start();
  assert(el0_context_resume(0));
  assert(el0_context_reset(0) && !el0_context_owns_stack(0));
  assert(el0_context_start(0));
  for (unsigned int i = 0; i < 6; i++)
    assert(el0_context_resume(0));
  assert(contexts[0].info.passed);
  command("reset");
  assert(strcmp(output, "EL0 context 0: EMPTY\n") == 0);
  command("start");
  assert(strstr(output, "EL0 context 0: READY\n"));
  command("resume");
  assert(strstr(output, "EL0 context 0: YIELDED\n"));
  assert(contexts[0].info.resumptions == 1 && contexts[0].info.result == 7);
  const char *invalid[] = {"status start", "start reset", "resume resume",
    "reset start", "invalid", "status extra", "resume start",
    "start fault extra", "start fault resume"};
  for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    command(invalid[i]);
    assert(strstr(output, "Usage: el0ctx") || strstr(output, "Invalid EL0"));
    assert(contexts[0].info.state == EL0_CONTEXT_YIELDED && contexts[0].info.resumptions == 1);
  }
  command("status");
  assert(strstr(output, "EL0 context 0: YIELDED\n"));
  assert(contexts[0].info.resumptions == 1);
  command("start");
  assert(strstr(output, "Cannot start EL0 context"));
  assert(contexts[0].info.resumptions == 1);
  for (unsigned int i = 0; i < 5; i++)
    command("resume");
  assert(strstr(output, "EL0 context 0: EXITED\n"));
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
    check_contained_fault(0, fault_frame, fault_cases[i].far_valid);
  }
  fault_frame.elr = 0x100080U;
  fault_frame.esr = 0x56000042U; // Unknown SVC even at an allowed code PC.
  check_contained_fault(0, fault_frame, false);
  assert(!el0_context_get_fault(0, NULL));
  start_fault_run();
  assert(el0_context_resume(0));
  assert(contexts[0].info.state == EL0_CONTEXT_FAULTED && contexts[0].info.fault_test_passed);
  assert(contexts[0].info.resumptions == 2 && contexts[0].info.yields == 1 && contexts[0].info.result == 7);
  assert(contexts[0].info.context_failures == 0 && contexts[0].info.kernel_failures == 0);
  command("status");
  assert(strstr(output, "EL0 context 0: FAULTED\n"));
  assert(strstr(output, "EL0 fault containment test: PASS\n"));
  command("resume");
  assert(strstr(output, "Cannot resume EL0 context"));
  command("start");
  assert(strstr(output, "Cannot start EL0 context"));
  command("start fault");
  assert(strstr(output, "Cannot start EL0 context"));
  el0_context_fault_t retained;
  assert(el0_context_get_fault(0, &retained));
  assert(retained.frame.elr == 0x100090U);
  assert(retained.frame.esr == 0x9200000fU);
  assert(retained.frame.far == (uintptr_t)&el0_test_kernel_canary);
  assert(retained.far_valid);
  assert(el0_context_reset(0));
  assert(!el0_context_get_fault(0, &retained));

  // A different fault is contained but never reported as this test PASS.
  const uint64_t bad_esr[] = {1ULL << 26, 1ULL << 25, 1ULL << 10,
      1ULL << 9, 1ULL << 8, 1ULL << 7, 1ULL << 6, 1ULL, 1ULL << 3};
  for (unsigned int i = 0; i < sizeof(bad_esr) / sizeof(bad_esr[0]); i++) {
    start_fault_run();
    fault_esr_xor = bad_esr[i];
    assert(el0_context_resume(0));
    assert(contexts[0].info.state == EL0_CONTEXT_FAULTED && !contexts[0].info.fault_test_passed);
  }
  fault_esr_xor = 0;
  start_fault_run();
  fault_far_delta = 8;
  assert(el0_context_resume(0) && !contexts[0].info.fault_test_passed);
  fault_far_delta = 0;
  start_fault_run();
  fault_pc_delta = 0x100000;
  assert(el0_context_resume(0) && !contexts[0].info.fault_test_passed);
  fault_pc_delta = 0;
  for (fault_reg = 0; fault_reg < 31; fault_reg++) {
    start_fault_run();
    assert(el0_context_resume(0) && !contexts[0].info.fault_test_passed);
    assert(contexts[0].info.context_failures == (1ULL << fault_reg));
  }
  fault_reg = -1;
  start_fault_run();
  resumed_reg = 3;
  assert(el0_context_resume(0) && !contexts[0].info.fault_test_passed);
  resumed_reg = -1;
  start_fault_run();
  kernel_mismatch = 1ULL << 19;
  assert(el0_context_resume(0) && !contexts[0].info.fault_test_passed);
  kernel_mismatch = 0;
  start_fault_run();
  corrupt_guard = true;
  assert(el0_context_resume(0) && !contexts[0].info.fault_test_passed);
  corrupt_guard = false;
  start_fault_run();
  el0_test_kernel_canary ^= 1;
  assert(el0_context_resume(0) && !contexts[0].info.fault_test_passed);
  el0_test_kernel_canary = EL0_TEST_KERNEL_CANARY;
  start_fault_run();
  skip_fault = true;
  assert(el0_context_resume(0));
  assert(contexts[0].info.state == EL0_CONTEXT_EXITED && !contexts[0].info.fault_test_passed);
  assert(!el0_context_get_fault(0, &retained) && !contexts[0].info.passed);
  command("status");
  assert(strstr(output, "expected fault missing"));
  skip_fault = false;
  command("reset");
  command("start fault");
  assert(contexts[0].info.state == EL0_CONTEXT_READY && contexts[0].info.fault_test);
  command("resume");
  command("resume");
  assert(strstr(output, "EL0 fault containment test: PASS\n"));
  command("reset");
  command("start");
  assert(!contexts[0].info.fault_test);
  for (unsigned int i = 0; i < 6; i++)
    command("resume");
  assert(contexts[0].info.passed && !contexts[0].info.fault_test_passed);
  assert(strstr(output, "EL0 cooperative test: PASS\n"));
  run_two_context_tests();
  assert(munmap(stack_page, 8192U) == 0);
  puts("EL0 context tests: PASS");
  return 0;
}
