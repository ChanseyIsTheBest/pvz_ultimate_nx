/* vfs.c -- Android path translation and synthetic files.
 *
 * The game was built expecting an Android filesystem. It will open things like
 *   /data/user/0/<pkg>/files/save.dat
 *   /storage/emulated/0/Android/data/<pkg>/...
 *   /proc/self/maps
 * none of which exist here. Everything real is redirected under the port's own
 * directory on the SD card; everything synthetic is served from memory.
 *
 * The /proc entries are not padding. NativeAOT reads /proc/self/maps on some
 * paths (stack-bounds fallback, module enumeration) and /proc/cpuinfo when
 * sizing the GC. An open() that returns -1 there is survivable; one that
 * returns an empty file is worse, because the runtime parses success and gets
 * zero CPUs. So the synthetic content is written to be parseable and to say
 * something true about this machine.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "vfs.h"

#define DATA_ROOT "sdmc:/switch/pvzultimate"

/* Fake descriptors start well above anything newlib will hand out. */
#define FAKE_FD_BASE 0x4000
#define MAX_FAKE_FD  16

typedef struct {
  int    in_use;
  char  *data;
  size_t size;
  size_t pos;
  char   path[128];
} FakeFd;

static FakeFd g_fake[MAX_FAKE_FD];
static Mutex  g_lock;

/* ------------------------------------------------------------------------ */
/* Path translation                                                          */
/* ------------------------------------------------------------------------ */

static int starts_with(const char *s, const char *p) {
  return strncmp(s, p, strlen(p)) == 0;
}

/* Find the tail after "/<something>/<pkg>/" for the data-dir forms, so we do
 * not have to hardcode the package name. */
static const char *after_nth_slash(const char *s, int n) {
  while (*s && n > 0) { if (*s == '/') n--; s++; }
  return s;
}

const char *vfs_translate(const char *path, char *out, size_t outsz) {
  if (!path) return NULL;

  /* Already ours. */
  if (starts_with(path, "sdmc:") || starts_with(path, "romfs:")) return path;

  if (path[0] != '/') {
    /* Relative -- resolve against the data root. */
    snprintf(out, outsz, DATA_ROOT "/%s", path);
    return out;
  }

  /* /data/user/0/<pkg>/files/x  and  /data/data/<pkg>/files/x
   * Both have the interesting part after four slashes. */
  if (starts_with(path, "/data/user/0/") || starts_with(path, "/data/data/")) {
    const char *tail = starts_with(path, "/data/user/0/")
                     ? after_nth_slash(path, 5)   /* /data/user/0/<pkg>/     */
                     : after_nth_slash(path, 4);  /* /data/data/<pkg>/       */
    snprintf(out, outsz, DATA_ROOT "/%s", tail);
    return out;
  }

  if (starts_with(path, "/storage/emulated/0/")) {
    snprintf(out, outsz, DATA_ROOT "/external/%s", path + 20);
    return out;
  }
  if (starts_with(path, "/sdcard/")) {
    snprintf(out, outsz, DATA_ROOT "/external/%s", path + 8);
    return out;
  }

  /* Anything else absolute: park it under the data root so a stray write does
   * not escape, and so the log shows what was attempted. */
  snprintf(out, outsz, DATA_ROOT "%s", path);
  return out;
}

/* ------------------------------------------------------------------------ */
/* Synthetic files                                                           */
/* ------------------------------------------------------------------------ */

/* A /proc/self/maps line the runtime can parse, one PT_LOAD per line, built
 * from what we actually mapped. If NativeAOT falls back to this to locate its
 * own image, it needs to agree with dl_iterate_phdr. */
static char *build_self_maps(size_t *out_len) {
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;

  for (so_module *m = so_module_list; m; m = m->next) {
    if (!m->load_virtbase || !m->phdr) continue;
    uintptr_t base = (uintptr_t)m->load_virtbase;

    for (size_t i = 0; i < m->phnum; i++) {
      if (m->phdr[i].p_type != 1 /* PT_LOAD */) continue;
      uintptr_t lo = base + (m->phdr[i].p_vaddr & ~(uintptr_t)0xFFF);
      uintptr_t hi = base + ((m->phdr[i].p_vaddr + m->phdr[i].p_memsz + 0xFFF) & ~(uintptr_t)0xFFF);
      int r = (m->phdr[i].p_flags & 4) ? 1 : 0;
      int w = (m->phdr[i].p_flags & 2) ? 1 : 0;
      int x = (m->phdr[i].p_flags & 1) ? 1 : 0;

      if (len + 256 > cap) {
        cap *= 2;
        char *nb = realloc(buf, cap);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
      }
      len += (size_t)snprintf(buf + len, cap - len,
                              "%012lx-%012lx %c%c%cp 00000000 00:00 0 %s\n",
                              (unsigned long)lo, (unsigned long)hi,
                              r ? 'r' : '-', w ? 'w' : '-', x ? 'x' : '-',
                              m->name);
    }
  }

  if (len == 0) len = (size_t)snprintf(buf, cap, "\n");
  *out_len = len;
  return buf;
}

/* Three cores, presented the way a parser expects. Reporting the host's real
 * topology would be wrong: homebrew gets three. */
static char *build_cpuinfo(size_t *out_len) {
  static const char *tmpl =
    "processor\t: %d\n"
    "BogoMIPS\t: 38.40\n"
    "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 8\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd07\n"
    "CPU revision\t: 1\n\n";
  size_t cap = 2048, len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  for (int i = 0; i < 3; i++)
    len += (size_t)snprintf(buf + len, cap - len, tmpl, i);
  *out_len = len;
  return buf;
}

static char *build_static(const char *text, size_t *out_len) {
  size_t n = strlen(text);
  char *b = malloc(n + 1);
  if (!b) return NULL;
  memcpy(b, text, n + 1);
  *out_len = n;
  return b;
}

/* Returns a malloc'd buffer or NULL if the path is not synthetic. */
static char *synthesize(const char *path, size_t *len) {
  if (!strcmp(path, "/proc/self/maps"))    return build_self_maps(len);
  if (!strcmp(path, "/proc/cpuinfo"))      return build_cpuinfo(len);
  if (!strcmp(path, "/proc/self/cmdline")) return build_static("pvzultimate\0", len);
  if (!strcmp(path, "/proc/self/stat"))    return build_static("1 (pvzultimate) R 0 0 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 3 0 0\n", len);
  if (!strcmp(path, "/proc/meminfo"))
    return build_static("MemTotal:        3145728 kB\nMemFree:         1048576 kB\n"
                        "MemAvailable:    1048576 kB\n", len);
  /* .NET reads /dev/urandom for its cryptographic RNG, and the open was
   * failing -- it translated to sdmc:/switch/pvzultimate/dev/urandom, which
   * does not exist. A failed RNG open is the sort of thing that surfaces much
   * later as something unrelated, so it is worth closing whether or not it is
   * behind the current problem.
   *
   * Filled from the system tick counter, mixed. Not cryptographic, and nothing
   * here needs it to be: the alternative on offer was no entropy at all. */
  if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
    const size_t n = 4096;
    char *buf = malloc(n);
    if (!buf) return NULL;
    u64 x = armGetSystemTick() | 1u;
    for (size_t i = 0; i < n; i++) {
      x ^= x << 13; x ^= x >> 7; x ^= x << 17;   /* xorshift64 */
      buf[i] = (char)(x >> 24);
    }
    *len = n;
    return buf;
  }
  if (!strcmp(path, "/sys/devices/system/cpu/online"))  return build_static("0-2\n", len);
  if (!strcmp(path, "/sys/devices/system/cpu/present")) return build_static("0-2\n", len);
  return NULL;
}

int vfs_is_synthetic(const char *path) {
  if (!path) return 0;
  return strncmp(path, "/proc/", 6) == 0 || strncmp(path, "/sys/", 5) == 0 ||
         !strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random");
}

int vfs_open_synthetic(const char *path) {
  size_t len = 0;
  char *data = synthesize(path, &len);
  if (!data) {
    debug_log("[vfs] synthetic open of unhandled %s -> ENOENT\n", path);
    errno = ENOENT;
    return -1;
  }

  mutexLock(&g_lock);
  for (int i = 0; i < MAX_FAKE_FD; i++) {
    if (!g_fake[i].in_use) {
      g_fake[i].in_use = 1;
      g_fake[i].data   = data;
      g_fake[i].size   = len;
      g_fake[i].pos    = 0;
      snprintf(g_fake[i].path, sizeof(g_fake[i].path), "%s", path);
      mutexUnlock(&g_lock);
      debug_log("[vfs] synthesized %s (%zu bytes) as fd %d\n",
                path, len, FAKE_FD_BASE + i);
      return FAKE_FD_BASE + i;
    }
  }
  mutexUnlock(&g_lock);
  free(data);
  errno = EMFILE;
  return -1;
}

int vfs_is_fake_fd(int fd) {
  return fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + MAX_FAKE_FD;
}

static FakeFd *slot(int fd) {
  if (!vfs_is_fake_fd(fd)) return NULL;
  FakeFd *f = &g_fake[fd - FAKE_FD_BASE];
  return f->in_use ? f : NULL;
}

long vfs_fake_read(int fd, void *buf, size_t count) {
  FakeFd *f = slot(fd);
  if (!f) { errno = EBADF; return -1; }
  size_t left = f->size - f->pos;
  size_t n = count < left ? count : left;
  memcpy(buf, f->data + f->pos, n);
  f->pos += n;
  return (long)n;
}

long vfs_fake_seek(int fd, long off, int whence) {
  FakeFd *f = slot(fd);
  if (!f) { errno = EBADF; return -1; }
  long base = (whence == 0) ? 0 : (whence == 1) ? (long)f->pos : (long)f->size;
  long np = base + off;
  if (np < 0) { errno = EINVAL; return -1; }
  f->pos = (size_t)np > f->size ? f->size : (size_t)np;
  return (long)f->pos;
}

int vfs_fake_close(int fd) {
  FakeFd *f = slot(fd);
  if (!f) { errno = EBADF; return -1; }
  mutexLock(&g_lock);
  free(f->data);
  memset(f, 0, sizeof(*f));
  mutexUnlock(&g_lock);
  return 0;
}

long vfs_fake_size(int fd) {
  FakeFd *f = slot(fd);
  return f ? (long)f->size : -1;
}

/* ------------------------------------------------------------------------ */

void vfs_init(void) {
  /* Create the directories the game will expect to write into. Doing it here
   * rather than lazily means a failed save is a permissions problem you can
   * see, not a silently missing directory. */
  static const char *dirs[] = {
    DATA_ROOT "/files", DATA_ROOT "/cache", DATA_ROOT "/external",
    DATA_ROOT "/tmp",   DATA_ROOT "/data",
    /* The game's own tree, confirmed from the SD card rather than guessed:
     * saves live in files/docs/userdata and the resource log in files/docs.
     * open_fake builds missing parents on demand now, but creating these up
     * front means a first-ever save cannot fail for a missing directory --
     * and ordering matters, each parent must exist before its child. */
    DATA_ROOT "/files/docs",
    DATA_ROOT "/files/docs/userdata",
  };
  for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++)
    mkdir(dirs[i], 0777);
  debug_log("[vfs] data root %s\n", DATA_ROOT);
}
