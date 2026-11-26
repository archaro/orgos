#include "mmu_activation.h"

static bool metadata_valid(const mmu_activation_t *control, unsigned int i) {
  const mmu_address_space_t *space = control->spaces[i];
  return space && space->root_physical == control->roots[i] &&
      space->tables == control->registries[i] &&
      mmu_boot_compatible(control->spaces[0], space, &control->layout,
                          (int)i - 1);
}

static bool ownership_distinct(mmu_address_space_t *const spaces[3]) {
  for (unsigned int a = 0; a < 3U; a++) {
    for (unsigned int b = a; b < 3U; b++) {
      for (unsigned int i = 0; i < spaces[a]->table_count; i++) {
        for (unsigned int j = 0; j < spaces[b]->table_count; j++) {
          if (a == b && i == j)
            continue;
          const mmu_table_ref_t *x = &spaces[a]->tables[i];
          const mmu_table_ref_t *y = &spaces[b]->tables[j];
          // Each validated CPU/physical registration is page-aligned.
          if (x->entries == y->entries || x->physical == y->physical)
            return false;
        }
      }
    }
  }
  return true;
}

bool mmu_activation_init(mmu_activation_t *control,
    mmu_address_space_t *kernel, mmu_address_space_t *context0,
    mmu_address_space_t *context1, const mmu_boot_layout_t *layout) {
  if (!control || control->active || !layout || !kernel || !context0 ||
      !context1 || kernel == context0 || kernel == context1 ||
      context0 == context1 || kernel->state != MMU_SPACE_ACTIVE ||
      context0->state != MMU_SPACE_SEALED ||
      context1->state != MMU_SPACE_SEALED)
    return false;
  mmu_activation_t next = {.spaces = {kernel, context0, context1},
                            .layout = *layout, .active = kernel};
  for (unsigned int i = 0; i < 3U; i++) {
    next.roots[i] = next.spaces[i]->root_physical;
    next.registries[i] = next.spaces[i]->tables;
    if (!metadata_valid(&next, i))
      return false;
  }
  if (!ownership_distinct(next.spaces) ||
      !mmu_activation_hw_matches(kernel->root_physical))
    return false;
  *control = next;
  return true;
}

bool mmu_activation_select(mmu_activation_t *control,
    mmu_address_space_t *space) {
  if (!control || !control->active || !space)
    return false;
  unsigned int target = 3U, current = 3U;
  for (unsigned int i = 0; i < 3U; i++) {
    if (control->spaces[i] == space)
      target = i;
    if (control->spaces[i] == control->active)
      current = i;
  }
  if (target == 3U || current == 3U || target == current)
    return false;
  uint64_t daif = mmu_activation_lock();
  bool valid = space->state == MMU_SPACE_SEALED &&
      control->active->state == MMU_SPACE_ACTIVE &&
      mmu_activation_hw_matches(control->roots[current]);
  for (unsigned int i = 0; valid && i < 3U; i++)
    valid = control->spaces[i]->state ==
        (i == current ? MMU_SPACE_ACTIVE : MMU_SPACE_SEALED) &&
        metadata_valid(control, i);
  valid = valid && ownership_distinct(control->spaces);
  if (valid) {
    mmu_replace_ttbr0(control->roots[target]);
    control->active->state = MMU_SPACE_SEALED;
    space->state = MMU_SPACE_ACTIVE;
    control->active = space;
  }
  mmu_activation_unlock(daif);
  return valid;
}
