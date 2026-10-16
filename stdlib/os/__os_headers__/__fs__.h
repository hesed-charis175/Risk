#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define RISK_MKDIR(path) _mkdir(path)
#define RISK_RMDIR(path) _rmdir(path)
#define RISK_GETCWD(buf, sz) _getcwd(buf, sz)
#else
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#define RISK_MKDIR(path) mkdir(path, 0755)
#define RISK_RMDIR(path) rmdir(path)
#define RISK_GETCWD(buf, sz) getcwd(buf, sz)
#endif

static inline char *fsCwd(void) {
#ifdef _WIN32
  char buf[4096];
  if (!RISK_GETCWD(buf, sizeof(buf))) {
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

static inline bool fsChdir(const char *path) {
  if (!path)
    return false;
#ifdef _WIN32
  return _chdir(path) == 0;
#else
  return chdir(path) == 0;
#endif
}

static inline bool fsMkdir(const char *path) {
  if (!path)
    return false;
  return RISK_MKDIR(path) == 0;
}

static inline bool fsMkdirAll(const char *path) {
  if (!path || !path[0])
    return false;

  size_t len = strlen(path);
  char *buf = (char *)malloc(len + 1);
  if (!buf)
    return false;
  memcpy(buf, path, len + 1);

  bool ok = true;
  for (size_t i = 1; i <= len; i++) {
    if (buf[i] == '/' || buf[i] == '\\' || buf[i] == '\0') {
      char saved = buf[i];
      buf[i] = '\0';
      if (buf[0] != '\0') {
        struct stat st;
        if (stat(buf, &st) != 0) {
          if (RISK_MKDIR(buf) != 0) {
            ok = false;
            buf[i] = saved;
            break;
          }
        }
      }
      buf[i] = saved;
      if (saved == '\0')
        break;
    }
  }

  free(buf);
  return ok;
}

static inline bool fsRemoveFile(const char *path) {
  if (!path)
    return false;
  return remove(path) == 0;
}

static inline bool fsRemoveDir(const char *path) {
  if (!path)
    return false;
  return RISK_RMDIR(path) == 0;
}

static inline bool fsExists(const char *path) {
  if (!path)
    return false;
  struct stat st;
  return stat(path, &st) == 0;
}

static inline bool fsIsFile(const char *path) {
  if (!path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == S_IFREG;
}

static inline bool fsIsDir(const char *path) {
  if (!path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == S_IFDIR;
}

static inline int64_t fsFileSize(const char *path) {
  if (!path)
    return -1;
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (int64_t)st.st_size;
}

static inline int64_t fsModTime(const char *path) {
  if (!path)
    return -1;
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (int64_t)st.st_mtime;
}

static inline bool fsRename(const char *from, const char *to) {
  if (!from || !to)
    return false;
  return rename(from, to) == 0;
}

static inline char *fsTempDir(void) {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetTempPathA(sizeof(buf), buf);
  if (n == 0 || n > sizeof(buf)) {
    char *r = (char *)risk_alloc(2);
    r[0] = '.';
    r[1] = '\0';
    return r;
  }
  /* Strip the trailing backslash GetTempPathA leaves on, for consistency
   * with the POSIX branch below (no trailing separator). */
  if (n > 1 && buf[n - 1] == '\\')
    buf[n - 1] = '\0';
  char *r = (char *)risk_alloc(strlen(buf) + 1);
  strcpy(r, buf);
  return r;
#else
  const char *t = getenv("TMPDIR");
  if (!t || !t[0])
    t = "/tmp";
  char *r = (char *)risk_alloc(strlen(t) + 1);
  strcpy(r, t);
  return r;
#endif
}

static inline char *fsListDirRaw(const char *path) {
  if (!path)
    return NULL;

#ifdef _WIN32
  char pattern[4096];
  snprintf(pattern, sizeof(pattern), "%s\\*", path);

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE)
    return NULL;

  size_t cap = 4096, len = 0;
  char *out = (char *)risk_alloc(cap);
  out[0] = '\0';

  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
      continue;
    size_t need = strlen(fd.cFileName) + 1;
    if (len + need + 1 > cap) {
      size_t new_cap = cap * 2;
      while (len + need + 1 > new_cap)
        new_cap *= 2;
      char *bigger = (char *)risk_alloc(new_cap);
      memcpy(bigger, out, len);
      out = bigger;
      cap = new_cap;
    }
    if (len > 0)
      out[len++] = '\n';
    memcpy(out + len, fd.cFileName, strlen(fd.cFileName));
    len += strlen(fd.cFileName);
    out[len] = '\0';
  } while (FindNextFileA(h, &fd));

  FindClose(h);
  return out;
#else
  DIR *d = opendir(path);
  if (!d)
    return NULL;

  size_t cap = 4096, len = 0;
  char *out = (char *)risk_alloc(cap);
  out[0] = '\0';

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    size_t need = strlen(ent->d_name) + 1;
    if (len + need + 1 > cap) {
      size_t new_cap = cap * 2;
      while (len + need + 1 > new_cap)
        new_cap *= 2;
      char *bigger = (char *)risk_alloc(new_cap);
      memcpy(bigger, out, len);
      out = bigger;
      cap = new_cap;
    }
    if (len > 0)
      out[len++] = '\n';
    memcpy(out + len, ent->d_name, strlen(ent->d_name));
    len += strlen(ent->d_name);
    out[len] = '\0';
  }

  closedir(d);
  return out;
#endif
}

static inline bool fsRemoveAll(const char *path) {
  if (!path)
    return false;

  struct stat st;
  if (stat(path, &st) != 0)
    return true; /* already gone */

  if ((st.st_mode & S_IFMT) != S_IFDIR)
    return remove(path) == 0;

#ifdef _WIN32
  char pattern[4096];
  snprintf(pattern, sizeof(pattern), "%s\\*", path);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
        continue;
      char child[4096];
      snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
      if (!fsRemoveAll(child)) {
        FindClose(h);
        return false;
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
#else
  DIR *d = opendir(path);
  if (d) {
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;
      char child[4096];
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (!fsRemoveAll(child)) {
        closedir(d);
        return false;
      }
    }
    closedir(d);
  }
#endif

  return RISK_RMDIR(path) == 0;
}
