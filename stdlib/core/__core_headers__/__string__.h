#pragma once

#include <string.h>

static inline int32_t strLength(const char *s) {
  if (!s)
    return 0;
  return (int32_t)strlen(s);
}

static inline char *strCharAt(const char *s, int32_t idx) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  if (idx < 0 || (size_t)idx >= len) {
    char *empty = (char *)risk_alloc(1);
    empty[0] = '\0';
    return empty;
  }
  char *ch = (char *)risk_alloc(2);
  ch[0] = s[idx];
  ch[1] = '\0';
  return ch;
}

static inline char *strConcat(const char *s1, const char *s2) {
  if (!s1 && !s2)
    return NULL;
  if (!s1)
    s1 = "";
  if (!s2)
    s2 = "";

  size_t len1 = strlen(s1);
  size_t len2 = strlen(s2);
  char *result = (char *)risk_alloc(len1 + len2 + 1);

  memcpy(result, s1, len1);
  memcpy(result + len1, s2, len2);
  result[len1 + len2] = '\0';

  return result;
}
