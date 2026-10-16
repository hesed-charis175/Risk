#pragma once

#include <time.h>

static inline int64_t timeNow(void) { return (int64_t)time(NULL); }

static inline int64_t timeMillis(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)(tv.tv_sec) * 1000 + (int64_t)(tv.tv_usec) / 1000;
}

static inline int64_t timeMicros(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)(tv.tv_sec) * 1000000 + (int64_t)(tv.tv_usec);
}

static inline void timeSleep(int32_t ms) {
  if (ms > 0) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
  }
}

static inline int64_t timeMonotonic(void) {
#ifdef __APPLE__
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)(tv.tv_sec) * 1000000000LL + (int64_t)(tv.tv_usec) * 1000LL;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)(ts.tv_sec) * 1000000000LL + (int64_t)(ts.tv_nsec);
#endif
}
