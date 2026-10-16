#pragma once

#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#include <stdlib.h> /* _fullpath */
#else
#include <limits.h>
#include <stdlib.h> /* realpath */
#endif

static inline char *pathSeparator(void) {
#ifdef _WIN32
  char *r = (char *)risk_alloc(2);
  r[0] = '\\';
  r[1] = '\0';
  return r;
#else
  char *r = (char *)risk_alloc(2);
  r[0] = '/';
  r[1] = '\0';
  return r;
#endif
}

static inline char *pathAbsolute(const char *path) {
  if (!path) {
    char *r = (char *)risk_alloc(1);
    r[0] = '\0';
    return r;
  }
#ifdef _WIN32
  char *resolved = _fullpath(NULL, path, 0);
  if (!resolved) {
    char *r = (char *)risk_alloc(strlen(path) + 1);
    strcpy(r, path);
    return r;
  }
  char *r = (char *)risk_alloc(strlen(resolved) + 1);
  strcpy(r, resolved);
  free(resolved);
  return r;
#else
  char buf[PATH_MAX];
  if (!realpath(path, buf)) {
    char *r = (char *)risk_alloc(strlen(path) + 1);
    strcpy(r, path);
    return r;
  }
  char *r = (char *)risk_alloc(strlen(buf) + 1);
  strcpy(r, buf);
  return r;
#endif
}
