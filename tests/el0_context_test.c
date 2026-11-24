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
static bool corrupt_guard, early_exit;
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

  uint64_t *stack = (uint64_t *)(uintptr_t)0x101ed0U;
  if (info.resumptions == 1U) {
    assert(frame.elr == 0x100000U && frame.sp_el0 == 0x102000U);
    assert(frame.x[0] == 0 && frame.x[1] == 0);
    frame.sp = frame.sp_el0 = 0x101ed0U;
    stack[34] = 0;
    stack[35] = 0x9abc567812340102ULL;
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
    bad.elr = 0x100104U;
    expect_panic(bad);
    bad.elr = 0xffffcU;
    expect_panic(bad);
    bad.elr = 0x100081U;
    expect_panic(bad);
    bad = frame;
    bad.esr ^= 1ULL << 25;
    expect_panic(bad);
    bad = frame;
    bad.esr = (0x15ULL << 26) | (1ULL << 25) | 0x42;
    expect_panic(bad);
    bad = frame;
    bad.esr = 0x9200000fU; // Memory aborts are never context operations.
    expect_panic(bad);
    bad.esr = 0x8200000fU;
    expect_panic(bad);
    bad.esr = 0x02000000U;
    expect_panic(bad);
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
  exception_dispatch(&frame);
  assert(kernel_return == NULL);
  assert(memcmp(&kernel, &frame, sizeof(frame)) == 0);
  return kernel_mismatch;
}

static void reset_start(void) {
  assert(el0_context_reset());
  assert(el0_context_start());
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
    "reset start", "invalid", "status extra", "resume start"};
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
  assert(munmap(stack_page, 4096U) == 0);
  puts("EL0 context tests: PASS");
  return 0;
}
