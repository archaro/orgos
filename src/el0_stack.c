#include "el0_stack.h"

static bool user_translation(vaddr_t va, paddr_t pa, bool write,
                             bool denied) {
  mmu_translation_t result;
  return mmu_translate_user(va, write, &result) &&
      (denied ? result.fault && result.fault_status == 0xfU :
       !result.fault && result.physical_address == pa);
}

bool el0_stack_probe(paddr_t backing, paddr_t peer, el0_stack_probe_t *probe) {
  if (!probe)
    return false;
  *probe = (el0_stack_probe_t){.checked = true};
  if (backing >= MMU_BLOCK_SIZE || peer >= MMU_BLOCK_SIZE ||
      ((backing | peer) & 4095U) || backing == peer)
    return false;
  mmu_descriptor_info_t info;
  if (!mmu_inspect(EL0_STACK_VA, &info) || info.kind != MMU_DESC_PAGE ||
      info.physical_address != backing || info.access_permissions != 1U ||
      !info.access_flag || !info.pxn || !info.uxn || info.attr_index != 2U ||
      ((info.leaf_descriptor >> 8) & 3U) != 3U)
    return false;
  mmu_translation_t read, write;
  if (!mmu_translate_user(EL0_STACK_VA, false, &read) ||
      !mmu_translate_user(EL0_STACK_VA, true, &write))
    return false;
  probe->read_physical = read.physical_address;
  probe->write_physical = write.physical_address;
  if (read.fault || write.fault || read.physical_address != backing ||
      write.physical_address != backing)
    return false;
  if (!user_translation(EL0_STACK_TOP - 1U, backing + 4095U, false, false) ||
      !user_translation(EL0_STACK_TOP - 1U, backing + 4095U, true, false))
    return false;
  const paddr_t aliases[] = {backing, peer};
  for (unsigned int i = 0; i < 2U; i++) {
    if (!mmu_inspect(aliases[i], &info) || info.kind != MMU_DESC_PAGE ||
        info.physical_address != aliases[i] || info.access_permissions != 0U ||
        !info.access_flag || !info.pxn || !info.uxn || info.attr_index != 2U ||
        ((info.leaf_descriptor >> 8) & 3U) != 3U)
      return false;
    for (unsigned int write_access = 0; write_access < 2U; write_access++) {
      if (!user_translation(aliases[i], 0, write_access, true) ||
          !user_translation(aliases[i] + 4095U, 0, write_access, true))
        return false;
    }
  }
  probe->valid = true;
  return true;
}
