#ifndef RISK_EMBEDDED_RUNTIME_H
#define RISK_EMBEDDED_RUNTIME_H

static const char *RISK_RUNTIME_SOURCE = R"RISK(
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32

  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <process.h>
  #include <conio.h>

  #define strncasecmp _strnicmp
  #define strcasecmp  _stricmp

  #define getcwd(buf, sz) _getcwd(buf, sz)
  #define getpid()        ((int)_getpid())

  #if !defined(_WINSOCK2API_) && !defined(_WINSOCKAPI_)
  struct timeval { long tv_sec; long tv_usec; };
  #endif
  static inline int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t =
        ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    tv->tv_sec  = (long)(t / 10000000ULL);
    tv->tv_usec = (long)((t % 10000000ULL) / 10ULL);
    return 0;
  }

  #if defined(_MSC_VER)
    #if !defined(_TIMESPEC_DEFINED)
    #define _TIMESPEC_DEFINED
    struct timespec { long tv_sec; long tv_nsec; };
    #endif
    static int risk__nanosleep(const struct timespec *req, struct timespec *rem) {
      (void)rem;
      DWORD ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000L);
      Sleep(ms);
      return 0;
    }
    #define CLOCK_MONOTONIC 1
    static int risk__clock_gettime(int clk, struct timespec *ts) {
      (void)clk;
      LARGE_INTEGER freq, cnt;
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&cnt);
      ts->tv_sec  = (long)(cnt.QuadPart / freq.QuadPart);
      ts->tv_nsec = (long)((cnt.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
      return 0;
    }
    #define nanosleep     risk__nanosleep
    #define clock_gettime risk__clock_gettime
  #endif

#else
  #include <strings.h>
  #include <unistd.h>
  #include <sys/time.h>
  #include <sys/utsname.h>
  #include <sys/select.h>
  #include <fcntl.h>
  #include <termios.h>
  #include <errno.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct risk_array_t {
  size_t elem_size;
  size_t size;
  size_t capacity;
  void *data;
} risk_array_t;

typedef struct risk_hashmap_slot_t {
  uint8_t occupied;
} risk_hashmap_slot_t;

typedef struct risk_hashmap_t {
  size_t key_size;
  size_t value_size;
  size_t size;
  size_t capacity;
  void *keys;
  void *values;
  risk_hashmap_slot_t *slots;
  int is_string_key;
} risk_hashmap_t;

#define RISK_MAX_FRAMES 256
typedef struct {
  const char *func;
  const char *file;
  int line;
} risk_frame_t;

static risk_frame_t risk_frames[RISK_MAX_FRAMES];
static int risk_frame_top = 0;

static inline void risk_push_frame(const char *func, const char *file, int line) {
  if (risk_frame_top < RISK_MAX_FRAMES) {
    risk_frames[risk_frame_top].func = func;
    risk_frames[risk_frame_top].file = file;
    risk_frames[risk_frame_top].line = line;
    risk_frame_top++;
  }
}

static inline void risk_pop_frame(void) {
  if (risk_frame_top > 0)
    --risk_frame_top;
}

static inline void risk_print_traceback(void) {
  if (risk_frame_top == 0)
    return;
  fprintf(stderr, "Traceback (most recent call last):\n");
  for (int i = risk_frame_top - 1; i >= 0; --i) {
    fprintf(stderr, "  at %s (%s:%d)\n",
            risk_frames[i].func ? risk_frames[i].func : "(unknown)",
            risk_frames[i].file ? risk_frames[i].file : "(unknown)",
            risk_frames[i].line);
  }
}

static inline void risk_panic(const char *msg) {
  if (msg)
    fprintf(stderr, "panic: %s\n", msg);
  risk_print_traceback();
  abort();
}

static inline void *risk_alloc(size_t n) {
  void *p = malloc(n);
  if (!p) {
    fprintf(stderr, "panic: out of memory allocating %zu bytes\n", n);
    abort();
  }
  return p;
}

static inline void risk_free(void *p) {
  if (p)
    free(p);
}

typedef struct risk_strbuf_t {
  char *data;
  size_t len;
  size_t cap;
} risk_strbuf_t;

static inline void risk_strbuf_init(risk_strbuf_t *sb) {
  sb->cap = 64;
  sb->len = 0;
  sb->data = (char *)risk_alloc(sb->cap);
  sb->data[0] = '\0';
}

static inline void risk_strbuf_append(risk_strbuf_t *sb, const char *s) {
  if (!s)
    s = "";
  size_t slen = strlen(s);
  if (sb->len + slen + 1 > sb->cap) {
    size_t new_cap = sb->cap ? sb->cap : 64;
    while (sb->len + slen + 1 > new_cap)
      new_cap *= 2;
    char *new_data = (char *)realloc(sb->data, new_cap);
    if (!new_data) {
      fprintf(stderr, "panic: out of memory growing strbuf to %zu bytes\n",
              new_cap);
      abort();
    }
    sb->data = new_data;
    sb->cap = new_cap;
  }
  memcpy(sb->data + sb->len, s, slen + 1);
  sb->len += slen;
}

static inline char *risk_strbuf_finish(risk_strbuf_t *sb) { return sb->data; }

static char risk_float_fmt[16] = "%g"; // runtime float formatting

static inline void risk_set_float_precision(int digits) {
  if (digits < 1) digits = 1;
  if (digits > 17) digits = 17;
  snprintf(risk_float_fmt, sizeof(risk_float_fmt), "%%.%dg", digits);
}

static inline void risk_set_float_fixed(int decimals) {
  if (decimals < 0) decimals = 0;
  if (decimals > 17) decimals = 17;
  snprintf(risk_float_fmt, sizeof(risk_float_fmt), "%%.%df", decimals);
}

static inline void risk_reset_float_format(void) {
  snprintf(risk_float_fmt, sizeof(risk_float_fmt), "%%g");
}

typedef struct {
  size_t refcount;

  void  (*destructor)(void *value_ptr);
  char   value[];
} risk_rc_t;

static inline risk_rc_t *risk_rc_new(const void *init, size_t value_size,
                                     void (*destructor)(void *)) {
  risk_rc_t *rc = (risk_rc_t *)risk_alloc(sizeof(risk_rc_t) + value_size);
  rc->refcount   = 1;
  rc->destructor = destructor;
  if (init)
    memcpy(rc->value, init, value_size);
  else
    memset(rc->value, 0, value_size);
  return rc;
}

static inline risk_rc_t *risk_rc_retain(risk_rc_t *rc) {
  if (rc) rc->refcount++;
  return rc;
}

static inline void risk_rc_release(risk_rc_t *rc) {
  if (!rc) return;
  if (--rc->refcount == 0) {
    if (rc->destructor)
      rc->destructor(rc->value);
    free(rc);
  }
}

static inline void *risk_rc_get(risk_rc_t *rc) {
  return rc ? (void *)rc->value : NULL;
}

static inline size_t risk_rc_count(risk_rc_t *rc) {
  return rc ? rc->refcount : 0;
}

#define RISK_ARENA_DEFAULT_ALIGN 16

typedef struct risk_arena_block_t risk_arena_block_t;
struct risk_arena_block_t {
  risk_arena_block_t *prev;
  size_t              capacity;
  size_t              used;
  char                data[];
};

typedef struct {
  risk_arena_block_t *current;
  size_t              block_size;
} risk_arena_t;

static inline risk_arena_t *risk_arena_create(size_t initial_size) {
  if (initial_size == 0) initial_size = 65536;
  risk_arena_t *a = (risk_arena_t *)risk_alloc(sizeof(risk_arena_t));
  risk_arena_block_t *blk =
      (risk_arena_block_t *)risk_alloc(sizeof(risk_arena_block_t) + initial_size);
  blk->prev     = NULL;
  blk->capacity = initial_size;
  blk->used     = 0;
  a->current    = blk;
  a->block_size = initial_size;
  return a;
}

static inline void *risk_arena_alloc(risk_arena_t *a, size_t size) {
  if (!a || size == 0) return NULL;

  size_t aligned = (size + RISK_ARENA_DEFAULT_ALIGN - 1)
                   & ~(size_t)(RISK_ARENA_DEFAULT_ALIGN - 1);
  risk_arena_block_t *blk = a->current;
  if (blk->used + aligned > blk->capacity) {

    size_t new_cap = a->block_size > aligned ? a->block_size : aligned * 2;
    risk_arena_block_t *nb =
        (risk_arena_block_t *)risk_alloc(sizeof(risk_arena_block_t) + new_cap);
    nb->prev      = blk;
    nb->capacity  = new_cap;
    nb->used      = 0;
    a->current    = nb;
    a->block_size = new_cap;
    blk           = nb;
  }
  void *ptr = blk->data + blk->used;
  blk->used += aligned;
  return ptr;
}

static inline void risk_arena_reset(risk_arena_t *a) {
  if (!a) return;
  risk_arena_block_t *blk = a->current;
  while (blk) {
    blk->used = 0;
    blk = blk->prev;
  }
}

static inline void risk_arena_destroy(risk_arena_t *a) {
  if (!a) return;
  risk_arena_block_t *blk = a->current;
  while (blk) {
    risk_arena_block_t *prev = blk->prev;
    free(blk);
    blk = prev;
  }
  free(a);
}

static inline size_t risk_arena_used(risk_arena_t *a) {
  return (a && a->current) ? a->current->used : 0;
}

static inline size_t risk_arena_capacity(risk_arena_t *a) {
  size_t total = 0;
  if (!a) return 0;
  for (risk_arena_block_t *blk = a->current; blk; blk = blk->prev)
    total += blk->capacity;
  return total;
}

static inline size_t risk_arena_total_used(risk_arena_t *a) {
  size_t total = 0;
  if (!a) return 0;
  for (risk_arena_block_t *blk = a->current; blk; blk = blk->prev)
    total += blk->used;
  return total;
}

static inline double risk_bytes_to_kib(size_t bytes) {
  return (double)bytes / 1024.0;
}
static inline double risk_bytes_to_mib(size_t bytes) {
  return (double)bytes / (1024.0 * 1024.0);
}
static inline double risk_bytes_to_gib(size_t bytes) {
  return (double)bytes / (1024.0 * 1024.0 * 1024.0);
}

static inline double risk_arena_capacity_kib(risk_arena_t *a) {
  return risk_bytes_to_kib(risk_arena_capacity(a));
}
static inline double risk_arena_capacity_mib(risk_arena_t *a) {
  return risk_bytes_to_mib(risk_arena_capacity(a));
}
static inline double risk_arena_capacity_gib(risk_arena_t *a) {
  return risk_bytes_to_gib(risk_arena_capacity(a));
}

static inline double risk_arena_used_kib(risk_arena_t *a) {
  return risk_bytes_to_kib(risk_arena_total_used(a));
}
static inline double risk_arena_used_mib(risk_arena_t *a) {
  return risk_bytes_to_mib(risk_arena_total_used(a));
}
static inline double risk_arena_used_gib(risk_arena_t *a) {
  return risk_bytes_to_gib(risk_arena_total_used(a));
}

typedef struct {
  risk_arena_block_t *block;
  size_t              used;
} risk_arena_checkpoint_t;

static inline void *risk_arena_scratch_begin(risk_arena_t *a) {
  if (!a || !a->current) return NULL;
  risk_arena_checkpoint_t *cp =
      (risk_arena_checkpoint_t *)risk_alloc(sizeof(risk_arena_checkpoint_t));
  cp->block = a->current;
  cp->used  = a->current->used;
  return (void *)cp;
}

static inline void risk_arena_scratch_end(risk_arena_t *a, void *checkpoint) {
  if (!a || !checkpoint) return;
  risk_arena_checkpoint_t *cp = (risk_arena_checkpoint_t *)checkpoint;

  risk_arena_block_t *blk = a->current;
  while (blk && blk != cp->block) {
    risk_arena_block_t *prev = blk->prev;
    free(blk);
    blk = prev;
  }
  a->current = blk ? blk : cp->block;
  if (a->current)
    a->current->used = cp->used;
  free(cp);
}

static inline void risk_array_free(risk_array_t *arr);

static inline void risk_array_free_all_ptrs(risk_array_t *arr) {
  if (!arr) return;
  for (size_t i = 0; i < arr->size; i++) {
    void **slot = (void **)((char *)arr->data + i * arr->elem_size);
    if (*slot) {
      free(*slot);
      *slot = NULL;
    }
  }
  arr->size = 0;
}

static inline void risk_array_free_all_rc(risk_array_t *arr) {
  if (!arr) return;
  for (size_t i = 0; i < arr->size; i++) {
    risk_rc_t **slot = (risk_rc_t **)((char *)arr->data + i * arr->elem_size);
    risk_rc_release(*slot);
    *slot = NULL;
  }
  arr->size = 0;
}

static inline void risk_array_destroy(risk_array_t *arr) {
  risk_array_free(arr);
}

static inline char *risk_io_input(void) {
  char buf[4096];
  if (!fgets(buf, sizeof(buf), stdin)) {
    char *empty = (char *)risk_alloc(1);
    empty[0] = '\0';
    return empty;
  }
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  char *r = (char *)risk_alloc(len + 1);
  memcpy(r, buf, len + 1);
  return r;
}

static inline risk_array_t *risk_array_create(size_t elem_size) {
  risk_array_t *arr = (risk_array_t *)risk_alloc(sizeof(risk_array_t));
  arr->elem_size = elem_size;
  arr->size = 0;
  arr->capacity = 10;
  arr->data = risk_alloc(elem_size * 10);
  return arr;
}

static inline void risk_array_free(risk_array_t *arr) {
  if (arr) {
    risk_free(arr->data);
    risk_free(arr);
  }
}

static inline void *risk_array_get(risk_array_t *arr, size_t index) {
  if (!arr || index >= arr->size)
    return NULL;
  return (char *)arr->data + (index * arr->elem_size);
}

static inline int risk_array_set(risk_array_t *arr, size_t index, const void *element) {
  if (!arr || !element || index >= arr->size)
    return 0;
  memcpy((char *)arr->data + (index * arr->elem_size), element, arr->elem_size);
  return 1;
}

static inline int risk_array_push(risk_array_t *arr, const void *element) {
  if (!arr || !element)
    return 0;
  if (arr->size >= arr->capacity) {
    size_t new_cap = (size_t)(arr->capacity * 1.5);
    if (new_cap <= arr->capacity)
      new_cap = arr->capacity + 1;
    void *new_data = risk_alloc(arr->elem_size * new_cap);
    if (arr->size > 0)
      memcpy(new_data, arr->data, arr->elem_size * arr->size);
    risk_free(arr->data);
    arr->data = new_data;
    arr->capacity = new_cap;
  }
  memcpy((char *)arr->data + (arr->size * arr->elem_size), element,
         arr->elem_size);
  arr->size++;
  return 1;
}

static inline int risk_array_pop(risk_array_t *arr, void *out_element) {
  if (!arr || arr->size == 0 || !out_element)
    return 0;
  arr->size--;
  memcpy(out_element, (char *)arr->data + (arr->size * arr->elem_size),
         arr->elem_size);
  return 1;
}

static inline size_t risk_array_length(risk_array_t *arr) { return arr ? arr->size : 0; }

static inline int risk_array_push_front(risk_array_t *arr, const void *element) {
  if (!arr || !element) return 0;

  if (arr->size >= arr->capacity) {
    size_t new_cap = (size_t)(arr->capacity * 1.5);
    if (new_cap <= arr->capacity) new_cap = arr->capacity + 1;
    void *new_data = risk_alloc(arr->elem_size * new_cap);
    if (arr->size > 0)
      memcpy((char *)new_data + arr->elem_size, arr->data, arr->elem_size * arr->size);
    risk_free(arr->data);
    arr->data = new_data;
    arr->capacity = new_cap;
  } else {

    memmove((char *)arr->data + arr->elem_size, arr->data, arr->elem_size * arr->size);
  }
  memcpy(arr->data, element, arr->elem_size);
  arr->size++;
  return 1;
}

static inline int risk_array_pop_front(risk_array_t *arr, void *out_element) {
  if (!arr || arr->size == 0 || !out_element) return 0;
  memcpy(out_element, arr->data, arr->elem_size);
  arr->size--;
  if (arr->size > 0)
    memmove(arr->data, (char *)arr->data + arr->elem_size, arr->elem_size * arr->size);
  return 1;
}

static inline int risk_array_insert(risk_array_t *arr, size_t index, const void *element) {
  if (!arr || !element || index > arr->size) return 0;
  if (index == arr->size) return risk_array_push(arr, element);
  if (index == 0) return risk_array_push_front(arr, element);

  if (arr->size >= arr->capacity) {
    size_t new_cap = (size_t)(arr->capacity * 1.5);
    if (new_cap <= arr->capacity) new_cap = arr->capacity + 1;
    void *new_data = risk_alloc(arr->elem_size * new_cap);
    memcpy(new_data, arr->data, arr->elem_size * index);
    memcpy((char *)new_data + arr->elem_size * (index + 1),
           (char *)arr->data + arr->elem_size * index,
           arr->elem_size * (arr->size - index));
    risk_free(arr->data);
    arr->data = new_data;
    arr->capacity = new_cap;
  } else {
    memmove((char *)arr->data + arr->elem_size * (index + 1),
            (char *)arr->data + arr->elem_size * index,
            arr->elem_size * (arr->size - index));
  }
  memcpy((char *)arr->data + arr->elem_size * index, element, arr->elem_size);
  arr->size++;
  return 1;
}

static inline int risk_array_remove_at(risk_array_t *arr, size_t index) {
  if (!arr || index >= arr->size) return 0;
  if (index < arr->size - 1)
    memmove((char *)arr->data + arr->elem_size * index,
            (char *)arr->data + arr->elem_size * (index + 1),
            arr->elem_size * (arr->size - index - 1));
  arr->size--;
  return 1;
}

static inline size_t risk_array_capacity(risk_array_t *arr) {
  return arr ? arr->capacity : 0;
}

static inline void risk_array_clear(risk_array_t *arr) {
  if (arr)
    arr->size = 0;
}

static inline uint64_t risk__fnv1a(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static inline risk_hashmap_t *risk_hashmap_create(size_t key_size, size_t value_size, int is_string_key) {
  risk_hashmap_t *map = (risk_hashmap_t *)risk_alloc(sizeof(risk_hashmap_t));
  map->key_size      = key_size;
  map->value_size    = value_size;
  map->size          = 0;
  map->capacity      = 16;
  map->is_string_key = is_string_key;
  map->keys   = risk_alloc(key_size   * 16);
  map->values = risk_alloc(value_size * 16);
  map->slots  = (risk_hashmap_slot_t *)risk_alloc(sizeof(risk_hashmap_slot_t) * 16);
  memset(map->slots, 0, sizeof(risk_hashmap_slot_t) * 16);
  return map;
}

static inline void risk_hashmap_free(risk_hashmap_t *map) {
  if (map) {

    if (map->is_string_key) {
      for (size_t i = 0; i < map->capacity; i++) {
        if (map->slots[i].occupied == 1) {
          char **kp = (char **)((char *)map->keys + i * map->key_size);
          if (*kp) { free(*kp); *kp = NULL; }
        }
      }
    }
    risk_free(map->keys);
    risk_free(map->values);
    risk_free(map->slots);
    risk_free(map);
  }
}

static inline uint64_t risk__hashmap_hash_key(risk_hashmap_t *map, const void *key) {
  if (map->is_string_key) {
    const char *s = *(const char **)key;
    return s ? risk__fnv1a(s, strlen(s)) : 0;
  }
  return risk__fnv1a(key, map->key_size);
}

static inline int risk__hashmap_keys_equal(risk_hashmap_t *map,
                                            const void *a, const void *b) {
  if (map->is_string_key) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    if (!sa && !sb) return 1;
    if (!sa || !sb) return 0;
    return strcmp(sa, sb) == 0;
  }
  return memcmp(a, b, map->key_size) == 0;
}

static inline size_t risk__hashmap_probe(risk_hashmap_t *map, const void *key,
                                         int *found_out) {
  uint64_t h    = risk__hashmap_hash_key(map, key);
  size_t   mask = map->capacity - 1;
  size_t   i    = (size_t)(h & mask);
  size_t   first_tomb = (size_t)-1;

  for (size_t step = 0; step < map->capacity; step++) {
    uint8_t state = map->slots[i].occupied;
    if (state == 0) {
      *found_out = 0;
      return (first_tomb != (size_t)-1) ? first_tomb : i;
    }
    if (state == 2) {
      if (first_tomb == (size_t)-1) first_tomb = i;
    } else {
      void *k = (char *)map->keys + i * map->key_size;
      if (risk__hashmap_keys_equal(map, k, key)) {
        *found_out = 1;
        return i;
      }
    }
    i = (i + 1) & mask;
  }
  *found_out = 0;
  return (first_tomb != (size_t)-1) ? first_tomb : i;
}

static inline void risk__hashmap_rehash(risk_hashmap_t *map) {
  size_t old_cap  = map->capacity;
  void  *old_keys = map->keys;
  void  *old_vals = map->values;
  risk_hashmap_slot_t *old_slots = map->slots;

  size_t new_cap  = old_cap * 2;
  map->capacity   = new_cap;
  map->size       = 0;
  map->keys   = risk_alloc(map->key_size   * new_cap);
  map->values = risk_alloc(map->value_size * new_cap);
  map->slots  = (risk_hashmap_slot_t *)risk_alloc(sizeof(risk_hashmap_slot_t) * new_cap);
  memset(map->slots, 0, sizeof(risk_hashmap_slot_t) * new_cap);

  for (size_t i = 0; i < old_cap; i++) {
    if (old_slots[i].occupied != 1) continue;
    void *k = (char *)old_keys + i * map->key_size;
    void *v = (char *)old_vals + i * map->value_size;
    int found;
    size_t slot = risk__hashmap_probe(map, k, &found);

    memcpy((char *)map->keys   + slot * map->key_size,   k, map->key_size);
    memcpy((char *)map->values + slot * map->value_size, v, map->value_size);
    map->slots[slot].occupied = 1;
    map->size++;
  }

  risk_free(old_keys);
  risk_free(old_vals);
  risk_free(old_slots);
}

static inline void *risk_hashmap_get(risk_hashmap_t *map, const void *key) {
  if (!map || !key) return NULL;
  int found;
  size_t slot = risk__hashmap_probe(map, key, &found);
  if (!found) return NULL;
  return (char *)map->values + slot * map->value_size;
}

static inline int risk_hashmap_set(risk_hashmap_t *map, const void *key,
                                   const void *value) {
  if (!map || !key || !value) return 0;

  if (map->size * 10 >= map->capacity * 7)
    risk__hashmap_rehash(map);

  int found;
  size_t slot = risk__hashmap_probe(map, key, &found);
  if (!found) {

    if (map->is_string_key) {
      const char *src = *(const char **)key;
      char *copy = src ? strdup(src) : NULL;
      memcpy((char *)map->keys + slot * map->key_size, &copy, map->key_size);
    } else {
      memcpy((char *)map->keys + slot * map->key_size, key, map->key_size);
    }
    map->slots[slot].occupied = 1;
    map->size++;
  }

  memcpy((char *)map->values + slot * map->value_size, value, map->value_size);
  return 1;
}

static inline int risk_hashmap_remove(risk_hashmap_t *map, const void *key) {
  if (!map || !key) return 0;
  int found;
  size_t slot = risk__hashmap_probe(map, key, &found);
  if (!found) return 0;

  if (map->is_string_key) {
    char **kp = (char **)((char *)map->keys + slot * map->key_size);
    if (*kp) { free(*kp); *kp = NULL; }
  }
  map->slots[slot].occupied = 2;
  map->size--;
  return 1;
}

static inline size_t risk_hashmap_size(risk_hashmap_t *map) {
  return map ? map->size : 0;
}

static inline int risk_hashmap_has(risk_hashmap_t *map, const void *key) {
  if (!map || !key) return 0;
  int found;
  risk__hashmap_probe(map, key, &found);
  return found;
}

static inline void risk_hashmap_clear(risk_hashmap_t *map) {
  if (!map) return;
  if (map->is_string_key) {
    for (size_t i = 0; i < map->capacity; i++) {
      if (map->slots[i].occupied == 1) {
        char **kp = (char **)((char *)map->keys + i * map->key_size);
        if (*kp) { free(*kp); *kp = NULL; }
      }
    }
  }
  memset(map->slots, 0, sizeof(risk_hashmap_slot_t) * map->capacity);
  map->size = 0;
}

static inline char *str_repeat(const char *s, int32_t count) {
  if (!s || count < 0)
    return NULL;

  if (count == 0) {
    char *result = (char *)risk_alloc(1);
    result[0] = '\0';
    return result;
  }

  size_t s_len = strlen(s);
  size_t total_len = s_len * count;
  char *result = (char *)risk_alloc(total_len + 1);

  char *dest = result;
  for (int32_t i = 0; i < count; i++) {
    memcpy(dest, s, s_len);
    dest += s_len;
  }
  result[total_len] = '\0';

  return result;
}

static inline int32_t risk_to_int(const char *s) {
  if (!s)
    return 0;
  while (*s && isspace((unsigned char)*s))
    s++;

  int sign = 1;
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  int32_t result = 0;
  while (*s && isdigit((unsigned char)*s)) {
    result = result * 10 + (*s - '0');
    s++;
  }

  return result * sign;
}

static inline double risk_to_float(const char *s) {
  if (!s)
    return 0.0;
  return strtod(s, NULL);
}

static inline int32_t risk_streq(const char *a, const char *b) {
  if (a == b)
    return 1;
  if (!a || !b)
    return 0;
  return strcmp(a, b) == 0;
}

static inline int32_t risk_to_bool(const char *s) {
  if (!s)
    return 0;

  while (*s && isspace((unsigned char)*s))
    s++;

  if (strncasecmp(s, "true", 4) == 0)
    return 1;
  if (strncasecmp(s, "yes", 3) == 0)
    return 1;
  if (strncasecmp(s, "on", 2) == 0)
    return 1;
  if (*s == '1' && *(s + 1) == '\0')
    return 1;
  if (*s == 't' && *(s + 1) == '\0')
    return 1;
  if (*s == 'y' && *(s + 1) == '\0')
    return 1;

  return 0;
}

static inline char *risk_to_str_float(double value) {
  char *buf = (char *)malloc(32);
  if (buf) {
    snprintf(buf, 32, risk_float_fmt, value);
  }
  return buf;
}

static inline char *risk_to_str_int(int32_t value) {
  char *buf = (char *)malloc(32);
  if (buf) {
    snprintf(buf, 32, "%d", value);
  }
  return buf;
}

static inline char *risk_to_str_bool(int32_t value) {
  char *buf = (char *)malloc(6);
  if (buf) {
    strcpy(buf, value ? "true" : "false");
  }
  return buf;
}

static inline void core_print(const char *s) {
  fputs(s ? s : "", stdout);
}

static inline void core_println(const char *s) {
  fputs(s ? s : "", stdout);
  fputc('\n', stdout);
}
#ifdef __cplusplus
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static inline double mod__PI(void) { return M_PI; }
#ifndef M_E
#define M_E 2.71828182845904523536
#endif
static inline double mod__E(void) { return M_E; }

static inline int32_t is_inf(double x) { return isinf(x) ? 1 : 0; }

static inline int32_t is_nan(double x) { return isnan(x) ? 1 : 0; }

static inline char *char_to_str(char c) {
  char *r = (char *)risk_alloc(2);
  r[0] = c;
  r[1] = '\0';
  return r;
}

#ifdef _WIN32

static inline char *kbRead(void) {
  char *r = (char *)risk_alloc(2);
  if (_kbhit()) {
    r[0] = (char)_getch();
    r[1] = '\0';
  } else {
    r[0] = '\0';
    r[1] = '\0';
  }
  return r;
}

static inline char *kbWait(void) {
  char *r = (char *)risk_alloc(2);
  r[0] = (char)_getch();
  r[1] = '\0';
  return r;
}

static inline void clrScr(void) {
  printf("\033[2J\033[H");
  fflush(stdout);
}

#else

static inline void clrScr(void) {
  printf("\033[2J\033[H");
  fflush(stdout);
}

static inline char *kbRead(void) {
  struct termios oldt, newt;
  int ch;
  int oldf;

  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  newt.c_cc[VMIN] = 0;
  newt.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

  ch = getchar();

  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);

  char *r = (char *)risk_alloc(2);
  if (ch != EOF) {
    r[0] = (char)ch;
    r[1] = '\0';
  } else {
    r[0] = '\0';
  }
  return r;
}

static inline char *kbWait(void) {
  struct termios oldt, newt;
  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  newt.c_cc[VMIN] = 1;
  newt.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);

  int ch = getchar();

  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

  char *r = (char *)risk_alloc(2);
  r[0] = (ch != EOF) ? (char)ch : '\0';
  r[1] = '\0';
  return r;
}
#endif
)RISK";
#endif
