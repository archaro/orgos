#pragma once

#include <stdint.h>

// Numeric addresses, not kernel-accessible pointers. Equal values in the
// current identity map do not make these concepts interchangeable.
typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;
