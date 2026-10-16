#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

static inline void procExit(int32_t code) { exit((int)code); }

static inline void procSleepMs(int64_t ms) {
  if (ms <= 0)
    return;
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec req;
  req.tv_sec = (time_t)(ms / 1000);
  req.tv_nsec = (long)((ms % 1000) * 1000000L);
  nanosleep(&req, NULL);
#endif
}

static inline int32_t procSystem(const char *cmd) {
  if (!cmd)
    return -1;
  return (int32_t)system(cmd);
}

static inline bool procSetEnv(const char *name, const char *value) {
  if (!name || !value)
    return false;
#ifdef _WIN32
  return _putenv_s(name, value) == 0;
#else
  return setenv(name, value, 1) == 0;
#endif
}

static inline bool procUnsetEnv(const char *name) {
  if (!name)
    return false;
#ifdef _WIN32
  return _putenv_s(name, "") == 0;
#else
  return unsetenv(name) == 0;
#endif
}
