#pragma once

#include <stddef.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *restrict dest, const void *restrict src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *a, const void *b, size_t count);
