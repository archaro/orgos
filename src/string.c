#include <stddef.h>
#include <stdint.h>

void *memset(void *dest, int value, size_t count) {
  unsigned char *p = dest;

  while (count--)
    *p++ = (unsigned char)value;
  return dest;
}

void *memcpy(void *restrict dest,
    const void *restrict src, size_t count) {
  unsigned char *d = dest;
  const unsigned char *s = src;

  while (count--)
    *d++ = *s++;
  return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
  unsigned char *d = dest;
  const unsigned char *s = src;

  if (d == s || count == 0)
    return dest;

  if ((uintptr_t)d < (uintptr_t)s) {
    while (count--)
      *d++ = *s++;
  } else {
    d += count;
    s += count;
    while (count--)
      *--d = *--s;
  }
  return dest;
}

int memcmp(const void *a, const void *b, size_t count) {
  const unsigned char *p = a;
  const unsigned char *q = b;

  while (count--) {
    if (*p != *q)
      return (int)*p - (int)*q;
    p++;
    q++;
  }
  return 0;
}
