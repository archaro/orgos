#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool console_arg_end(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;

  return *p == '\0';
}

// Match one whitespace-delimited word.
// Advance only when the word matches.
static inline bool console_arg_word(const char **input, const char *word) {
  const char *p = *input;
  const char *w = word;

  while (*p == ' ' || *p == '\t')
    p++;

  while (*w && *p == *w) {
    p++;
    w++;
  }

  if (*w || (*p && *p != ' ' && *p != '\t'))
    return false;

  *input = p;
  return true;
}

// Parse one unsigned decimal integer, rejecting
// missing digits, trailing junk and overflow.
static inline bool console_arg_uint(const char **input, unsigned int *value) {
  const char *p = *input;
  unsigned int result = 0U;
  bool found = false;

  while (*p == ' ' || *p == '\t')
    p++;

  while (*p >= '0' && *p <= '9') {
    unsigned int digit = (unsigned int)(*p - '0');

    if (result > (UINT32_MAX - digit) / 10U)
      return false;

    result = result * 10U + digit;
    found = true;
    p++;
  }

  if (!found || (*p && *p != ' ' && *p != '\t'))
    return false;

  *value = result;
  *input = p;
  return true;
}

// A device generation is one nonzero decimal
// integer followed only by optional whitespace.
static inline bool console_arg_generation(const char *args,
    uint32_t *generation) {

  if (!args || !generation)
    return false;

  unsigned int value = 0U;

  if (!console_arg_uint(&args, &value) ||
      value == 0U || !console_arg_end(args))
    return false;

  *generation = (uint32_t)value;
  return true;
}
