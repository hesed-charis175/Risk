#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>  /* _getcwd */
#include <process.h> /* _getpid */
#include <windows.h>
#define getcwd(buf, sz) _getcwd(buf, sz)
#define getpid() ((int)_getpid())
#else
#include <unistd.h>
#endif

static inline char *sysPlatform(void) {
#if defined(_WIN32)
  const char *p = "windows";
#elif defined(__APPLE__)
  const char *p = "macos";
#elif defined(__linux__)
  const char *p = "linux";
#else
  const char *p = "unknown";
#endif
  char *r = (char *)risk_alloc(strlen(p) + 1);
  strcpy(r, p);
  return r;
}

static inline char *sysArch(void) {
#if defined(__x86_64__) || defined(_M_X64)
  const char *a = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const char *a = "arm64";
#elif defined(__arm__) || defined(_M_ARM)
  const char *a = "arm";
#elif defined(__i386__) || defined(_M_IX86)
  const char *a = "x86";
#else
  const char *a = "unknown";
#endif
  char *r = (char *)risk_alloc(strlen(a) + 1);
  strcpy(r, a);
  return r;
}

static inline char *sysHostname(void) {
  char buf[256];
  buf[0] = '\0';
#ifdef _WIN32
  DWORD sz = sizeof(buf);
  GetComputerNameA(buf, &sz);
#else
  gethostname(buf, sizeof(buf));
#endif
  buf[255] = '\0';
  char *r = (char *)risk_alloc(strlen(buf) + 1);
  strcpy(r, buf);
  return r;
}

static inline char *sysUsername(void) {
  const char *u = getenv("USER");
  if (!u)
    u = getenv("USERNAME");
  if (!u)
    u = "";
  char *r = (char *)risk_alloc(strlen(u) + 1);
  strcpy(r, u);
  return r;
}

static inline char *sysCwd(void) {
#ifdef _WIN32
  char buf[4096];
  if (!_getcwd(buf, sizeof(buf))) {
    char *r = (char *)risk_alloc(2);
    r[0] = '.';
    r[1] = '\0';
    return r;
  }
  char *r = (char *)risk_alloc(strlen(buf) + 1);
  strcpy(r, buf);
  return r;
#else
  char *buf = getcwd(NULL, 0);
  if (!buf) {
    char *r = (char *)risk_alloc(2);
    r[0] = '.';
    r[1] = '\0';
    return r;
  }
  char *r = (char *)risk_alloc(strlen(buf) + 1);
  strcpy(r, buf);
  free(buf);
  return r;
#endif
}

static inline char *sysEnv(const char *name) {
  const char *v = name ? getenv(name) : NULL;
  if (!v)
    v = "";
  char *r = (char *)risk_alloc(strlen(v) + 1);
  strcpy(r, v);
  return r;
}

static inline int64_t sysPid(void) { return (int64_t)getpid(); }
