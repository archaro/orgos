#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "mmu_c_island.h"

int main(void) {
  const uint64_t ro[] = {0x1234, 6, 7};
  mmu_c_storage_t rw = {0x5678, 0, 0x9abc};
  mmu_c_evidence_t e = {0};
  assert(mmu_c_outer(&e, ro, &rw) == 42);
  assert(rw.value == 42 && rw.guard_before == 0x5678 &&
      rw.guard_after == 0x9abc);
  assert(e.ro_value[0] == 6 && e.ro_value[1] == 7 && e.written == 42);
  assert(e.local_value[0] == 6 && e.local_value[1] == 7 &&
      e.local_value[2] == 42);
  assert(e.local[0] && e.local[1] && e.local[2]);
  assert(e.local[0] != e.local[1] && e.local[1] != e.local[2]);
  assert(e.ro_address == (uintptr_t)&ro[1]);
  assert(e.rw_address == (uintptr_t)&rw.value);
  // Observations are deliberately absent on native hosts.
  assert(e.pc[0] == 0 && e.sp[0] == 0);
  puts("C island arithmetic/storage (native, no high execution proof): PASS");
  return 0;
}
