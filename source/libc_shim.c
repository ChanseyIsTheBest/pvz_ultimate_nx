/* libc_shim.c -- bionic surface that cannot simply forward to newlib.
 *
 * Roughly 200 of the 257 imports are honest forwards (strlen, memcpy, sin,
 * malloc) and belong in the table in imports.c pointing straight at newlib.
 * What lives here is the remainder: symbols bionic has that newlib does not,
 * symbols whose ABI differs, and symbols where forwarding would be actively
 * wrong on this platform.
 *
 * The awkward one is __sF. bionic declares `extern FILE __sF[]` and defines
 * stdout as &__sF[1], so the game's compiled code holds the array base and adds
 * a compile-time stride. That stride is baked into the instruction stream, not
 * into any relocation -- the image has exactly one reference to __sF, with
 * addend 0 -- so it cannot be recovered from the binary. Rather than guess
 * bionic's sizeof(FILE) and be silently wrong, we publish a generously sized
 * region and identify our streams by RANGE. Any FILE* landing inside it is one
 * of the three standard streams, and all of them go to the log. That is correct
 * for any stride bionic might be using.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include "errno_bionic.h"
#include "libc_shim.h"
#include "fileio.h"
#include "mem_arena.h"
#include "runtime_glue.h"
#include "threads.h"
#include "util.h"
#include "vfs.h"

/* ------------------------------------------------------------------------ */
/* errno                                                                     */
/* ------------------------------------------------------------------------ */

/* bionic's errno is a function returning a pointer; newlib's is a macro over
 * its own per-thread slot. Forward to newlib's so our shims and the game agree
 * on the same value. */
int *__errno_fake(void) { return &errno; }

/* ------------------------------------------------------------------------ */
/* Standard streams                                                          */
/* ------------------------------------------------------------------------ */

/* Wide enough that &__sF[2] lands inside it for any plausible bionic FILE
 * size. Publishing this as a data symbol is the whole point -- see the header
 * comment. */
#define SF_STRIDE 512
unsigned char __sF_fake[SF_STRIDE * 3];

static int is_std_stream(const void *f) {
  return (uintptr_t)f >= (uintptr_t)__sF_fake &&
         (uintptr_t)f <  (uintptr_t)__sF_fake + sizeof(__sF_fake);
}

/* ------------------------------------------------------------------------ */
/* stdio                                                                     */
/* ------------------------------------------------------------------------ */

int fprintf_fake(void *stream, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (is_std_stream(stream)) { debug_log("%s", buf); return n; }
  return fputs(buf, (FILE *)stream) < 0 ? -1 : n;
}

int vfprintf_fake(void *stream, const char *fmt, va_list ap) {
  char buf[1024];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (is_std_stream(stream)) { debug_log("%s", buf); return n; }
  return fputs(buf, (FILE *)stream) < 0 ? -1 : n;
}

int fputs_fake(const char *s, void *stream) {
  if (is_std_stream(stream)) { debug_log("%s", s); return 0; }
  return fputs(s, (FILE *)stream);
}

int fputc_fake(int c, void *stream) {
  if (is_std_stream(stream)) { debug_log("%c", c); return c; }
  return fputc(c, (FILE *)stream);
}

size_t fwrite_fake(const void *p, size_t sz, size_t n, void *stream) {
  if (is_std_stream(stream)) {
    debug_log("%.*s", (int)(sz * n), (const char *)p);
    return n;
  }
  return locked_fwrite(p, sz, n, (FILE *)stream);
}

int fflush_fake(void *stream) {
  if (!stream || is_std_stream(stream)) return 0;
  return fflush((FILE *)stream);
}

void *fopen_fake(const char *path, const char *mode) {
  /* open() consults the synthetic /proc table; fopen never did, so these fell
   * through to the SD card and logged a failure on every probe. Failing is
   * still the right answer for the ones we do not synthesise -- callers handle
   * it -- but it should be quiet and it should not hit the filesystem. */
  if (vfs_is_synthetic(path)) {
    errno = BIONIC_ENOENT;
    return NULL;
  }

  char buf[512];
  const char *real = vfs_translate(path, buf, sizeof(buf));
  FILE *f = locked_fopen(real, mode);

  /* Same two bugs open_fake had, in the other half of the I/O surface.
   *
   * A write mode needs its parent directories -- fopen does not create them
   * either -- and a failing WRITE was the one case this did not log, because
   * the test was strchr(mode, 'r'). A save that cannot open its file was
   * therefore silent from both entry points at once. */
  const int writing = strpbrk(mode, "wa+") != NULL;
  if (!f && writing) {
    mkdir_parents(real);
    f = locked_fopen(real, mode);
    if (f) debug_log("[io] created the parent directories for %s\n", real);
  }

  if (!f)
    debug_log("[io] fopen(%s -> %s, \"%s\") failed: %s\n", path, real, mode,
              strerror(errno));
  else if (writing)
    debug_log("[io] writing %s\n", real);
  return f;
}

int fclose_fake(void *f) { return f ? locked_fclose((FILE *)f) : 0; }

size_t fread_fake(void *p, size_t sz, size_t n, void *f) {
  return locked_fread(p, sz, n, (FILE *)f);
}

/* getline is a GNU/bionic extension newlib may not export. */
long getline_fake(char **lineptr, size_t *n, void *stream) {
  if (!lineptr || !n) { errno = BIONIC_EINVAL; return -1; }
  size_t cap = *n, len = 0;
  char *buf = *lineptr;
  if (!buf || cap == 0) { cap = 128; buf = malloc(cap); if (!buf) return -1; }

  /* __sF entries are not real FILE objects -- they exist only so the game's
   * stdout/stderr arithmetic lands somewhere readable. Reading from one is
   * end-of-input, not a dereference. */
  if (is_std_stream(stream)) { *lineptr = buf; *n = cap; return -1; }

  int c;
  while ((c = fgetc((FILE *)stream)) != EOF) {
    if (len + 2 > cap) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) { *lineptr = buf; *n = cap / 2; return -1; }
      buf = nb;
    }
    buf[len++] = (char)c;
    if (c == '\n') break;
  }
  if (len == 0 && c == EOF) { *lineptr = buf; *n = cap; return -1; }
  buf[len] = 0;
  *lineptr = buf;
  *n = cap;
  return (long)len;
}

int asprintf_fake(char **out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) return -1;
  *out = malloc((size_t)n + 1);
  if (!*out) return -1;
  va_start(ap, fmt);
  vsnprintf(*out, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return n;
}

int vasprintf_fake(char **out, const char *fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  int n = vsnprintf(NULL, 0, fmt, cp);
  va_end(cp);
  if (n < 0) return -1;
  *out = malloc((size_t)n + 1);
  if (!*out) return -1;
  vsnprintf(*out, (size_t)n + 1, fmt, ap);
  return n;
}

/* ------------------------------------------------------------------------ */
/* FORTIFY_SOURCE variants                                                   */
/* ------------------------------------------------------------------------ */

/* bionic emits these when _FORTIFY_SOURCE is on. They take an extra
 * destination-size argument and abort on overflow. Since the game is already
 * built and we are not the ones being protected, forwarding to the unchecked
 * version is correct -- but log a violation rather than aborting, because a
 * trip here usually means our shim handed back a wrong size somewhere. */

static void fortify_warn(const char *who, size_t want, size_t have) {
  debug_log("[fortify] %s: wanted %zu into a %zu buffer -- "
            "check whatever produced that size\n", who, want, have);
}

void *__memcpy_chk_fake(void *d, const void *s, size_t n, size_t dlen) {
  if (n > dlen) fortify_warn("memcpy", n, dlen);
  return memcpy(d, s, n);
}
void *__memset_chk_fake(void *d, int c, size_t n, size_t dlen) {
  if (n > dlen) fortify_warn("memset", n, dlen);
  return memset(d, c, n);
}
char *__strcpy_chk_fake(char *d, const char *s, size_t dlen) {
  size_t n = strlen(s) + 1;
  if (n > dlen) fortify_warn("strcpy", n, dlen);
  return strcpy(d, s);
}
char *__strncpy_chk2_fake(char *d, const char *s, size_t n, size_t dlen, size_t slen) {
  (void)slen;
  if (n > dlen) fortify_warn("strncpy", n, dlen);
  return strncpy(d, s, n);
}
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }

int __vsnprintf_chk_fake(char *d, size_t n, int flag, size_t dlen,
                         const char *fmt, va_list ap) {
  (void)flag; (void)dlen;
  return vsnprintf(d, n, fmt, ap);
}

void __assert2_fake(const char *file, int line, const char *fn, const char *expr) {
  fatal_error("assertion failed: %s\n  at %s:%d in %s",
              expr ? expr : "?", file ? file : "?", line, fn ? fn : "?");
}

void __stack_chk_fail_fake(void) {
  fatal_error("stack smashing detected in the game image.\n"
              "Usually a shim returned a larger size than the caller's buffer.");
}

/* ------------------------------------------------------------------------ */
/* Locale                                                                    */
/* ------------------------------------------------------------------------ */

/* With DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 the runtime should not lean on
 * these, but they are still linked and still called from a few paths. A single
 * opaque "C locale" token satisfies all of them. */
static int g_c_locale = 1;

void *newlocale_fake(int mask, const char *name, void *base) {
  (void)mask; (void)name; (void)base;
  return &g_c_locale;
}
void  freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc)  { (void)loc; return &g_c_locale; }

int    __ctype_get_mb_cur_max_fake(void) { return 1; }
double strtod_l_fake(const char *s, char **e, void *l)        { (void)l; return strtod(s, e); }
long long strtoll_l_fake(const char *s, char **e, int b, void *l)  { (void)l; return strtoll(s, e, b); }
unsigned long long strtoull_l_fake(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
/* strtod, not strtold: newlib may not carry the long-double parser, and this
 * file predates that discovery -- it would have failed the same link. */
long double strtold_l_fake(const char *s, char **e, void *l)  { (void)l; return (long double)strtod(s, e); }
size_t strftime_l_fake(char *b, size_t n, const char *f, const struct tm *t, void *l) {
  (void)l; return strftime(b, n, f, t);
}
int iswlower_l_fake(wint_t c, void *l) { (void)l; return iswlower(c); }

/* ------------------------------------------------------------------------ */
/* Time                                                                      */
/* ------------------------------------------------------------------------ */

/* CLOCK_MONOTONIC from the system tick. newlib's clock_gettime exists but its
 * monotonic clock is not guaranteed to be the one the runtime expects for
 * Stopwatch and GC timing, and a monotonic clock that jumps is very hard to
 * debug from the symptoms. */
int clock_gettime_fake(int clk, struct timespec *ts) {
  if (!ts) { errno = BIONIC_EFAULT; return -1; }
  /* Linux clock ids, which is what the game passes. CLOCK_MONOTONIC_COARSE(6)
   * was missing and is what .NET's Environment.TickCount reaches for; without
   * it the call fell through to the realtime branch and time appeared to jump.
   * The two CPU-time clocks are answered with monotonic rather than left to
   * fail -- an approximate thread time is far better for the threadpool's
   * heuristics than an error it does not expect. */
  if (clk == 1 /* MONOTONIC */      || clk == 4 /* MONOTONIC_RAW */ ||
      clk == 6 /* MONOTONIC_COARSE */|| clk == 7 /* BOOTTIME */     ||
      clk == 2 /* PROCESS_CPUTIME */ || clk == 3 /* THREAD_CPUTIME */) {
    u64 ns = armTicksToNs(armGetSystemTick());
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)  (ns % 1000000000ull);
    return 0;
  }
  return clock_gettime(CLOCK_REALTIME, ts);
}

int clock_nanosleep_fake(int clk, int flags, const struct timespec *req,
                         struct timespec *rem) {
  (void)clk; (void)rem;
  if (!req) return 0;
  u64 ns = (u64)req->tv_sec * 1000000000ull + (u64)req->tv_nsec;
  if (flags & 1 /* TIMER_ABSTIME */) {
    struct timespec now;
    clock_gettime_fake(1, &now);
    s64 d = (s64)ns - ((s64)now.tv_sec * 1000000000ll + now.tv_nsec);
    ns = d > 0 ? (u64)d : 0;
  }
  if (ns) svcSleepThread(ns);
  return 0;
}

int nanosleep_fake(const struct timespec *req, struct timespec *rem) {
  (void)rem;
  if (req) svcSleepThread((u64)req->tv_sec * 1000000000ull + (u64)req->tv_nsec);
  return 0;
}

/* sched_yield -- the weakest possible yield was the wrong one.
 *
 * This was svcSleepThread(0), which on Horizon is
 * YieldType_WithoutCoreMigration: give way to another thread of the SAME
 * priority already queued on THIS core. That is the narrowest of the three
 * yields the kernel offers, and it is not what POSIX sched_yield promises --
 * "relinquish the CPU" to whatever else can run.
 *
 * It matters because .NET's spin-waits bottom out here. Thread.StartCore()
 * spins on `while (state has ThreadState.Unstarted) Thread.Yield()`, waiting
 * for the thread it just created to clear that bit -- and the creport caught
 * exactly that: main spinning on bit 3 of the thread state (Unstarted == 8)
 * while the new thread sat on the first instruction of its entry wrapper with
 * no stack frames at all, 45 seconds after being started successfully.
 *
 * -2 is YieldType_ToAnyThread: yield to anything runnable, on any core, at any
 * priority. That is the semantic sched_yield is supposed to have, and it is
 * the one that can actually break a wait-for-another-thread loop.
 *
 * The periodic real sleep is deliberate belt and braces. A yield only offers
 * the CPU; a sleep unambiguously takes the caller off it, so whatever the
 * kernel's yield semantics turn out to be, a loop spinning here cannot lock
 * the machine out of running the thread it is waiting for. One in every 256
 * iterations costs nothing -- a spin that reaches 256 yields is already
 * pathological -- and it bounds the damage from any future livelock of this
 * shape instead of leaving it to hang forever. */
int sched_yield_fake(void) {
  static _Thread_local unsigned n;
  if ((++n & 0xFF) == 0) svcSleepThread(100000);   /* 100 us */
  else                   svcSleepThread(-2);       /* YieldType_ToAnyThread */
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Process / system information                                              */
/* ------------------------------------------------------------------------ */

/* EXACTLY bionic's LP64 layout. 112 bytes.
 *
 * The trailing pad is declared `char _f[20 - 2*sizeof(unsigned long) -
 * sizeof(unsigned int)]`, which is TWENTY bytes on 32-bit and ZERO on 64-bit.
 * Writing `char _f[20]` made our struct 128 bytes, and memset(si, 0,
 * sizeof(*si)) then wrote 16 bytes past a caller that had allocated 112 --
 * straight over the stack canary and saved registers. That is what
 * "stack smashing detected in the game image" was: not a game bug, ours.
 *
 * Anything here that fills a caller-supplied struct must match the caller's
 * idea of its size, not ours. sizeof() is not a safe way to bound a write into
 * memory somebody else allocated. */
struct fake_sysinfo {
  long           uptime;
  unsigned long  loads[3];
  unsigned long  totalram, freeram, sharedram, bufferram;
  unsigned long  totalswap, freeswap;
  unsigned short procs, pad;
  unsigned long  totalhigh, freehigh;
  unsigned int   mem_unit;
  char           _f[20 - 2 * sizeof(unsigned long) - sizeof(unsigned int)];
};
_Static_assert(sizeof(struct fake_sysinfo) == 112,
               "struct sysinfo must be 112 bytes to match bionic on LP64");

int sysinfo_fake(void *info) {
  struct fake_sysinfo *si = info;
  if (!si) return -1;
  memset(si, 0, sizeof(*si));

  /* Deliberately understated, and deliberately consistent with
   * sysconf(_SC_PHYS_PAGES) in imports.c. Reporting the real total makes the
   * GC size its reservation for a machine whose memory we have already taken.
   * If you change one of these, change the other. */
  si->mem_unit = 1;
  si->totalram = 1024ul * 1024ul * 1024ul;
  si->freeram  =  512ul * 1024ul * 1024ul;
  si->procs    = 1;
  si->uptime   = (long)(armTicksToNs(armGetSystemTick()) / 1000000000ull);
  return 0;
}

struct fake_rlimit { unsigned long rlim_cur, rlim_max; };

int getrlimit_fake(int res, void *rl) {
  struct fake_rlimit *r = rl;
  if (!r) return -1;
  switch (res) {
    case 3: /* RLIMIT_STACK */ r->rlim_cur = r->rlim_max = 1024 * 1024; break;
    case 7: /* RLIMIT_NOFILE*/ r->rlim_cur = r->rlim_max = 256;         break;
    default:                   r->rlim_cur = r->rlim_max = ~0ul;       break;
  }
  return 0;
}

/* struct rusage on LP64: two timevals (32) plus 14 longs (112). */
#define BIONIC_RUSAGE_SIZE 144

int getrusage_fake(int who, void *usage) {
  (void)who;
  if (usage) memset(usage, 0, BIONIC_RUSAGE_SIZE);
  return 0;
}

int sched_getaffinity_fake(int pid, size_t setsize, void *mask) {
  (void)pid;
  if (!mask || setsize < 8) return -1;
  memset(mask, 0, setsize);
  *(unsigned long *)mask = 0x7;   /* cores 0,1,2 */
  return 0;
}

int __sched_cpucount_fake(size_t setsize, const void *mask) {
  if (!mask || setsize < 8) return 3;
  unsigned long m = *(const unsigned long *)mask;
  int n = 0;
  while (m) { n += (int)(m & 1); m >>= 1; }
  return n ? n : 3;
}

int uname_fake(void *buf) {
  /* struct utsname is 6 fixed 65-byte fields on Linux. */
  if (!buf) return -1;
  char *b = buf;
  memset(b, 0, 65 * 6);
  snprintf(b +   0, 65, "Linux");
  snprintf(b +  65, 65, "Switch");
  snprintf(b + 130, 65, "4.9.0");
  snprintf(b + 195, 65, "#1 SMP");
  snprintf(b + 260, 65, "aarch64");
  snprintf(b + 325, 65, "localdomain");
  return 0;
}

int prctl_fake(int option, unsigned long a, unsigned long b,
               unsigned long c, unsigned long d) {
  (void)a; (void)b; (void)c; (void)d;
  /* PR_SET_NAME (15) is the common one and threads.c already handles naming
   * through pthread_setname_np. */
  if (option != 15) debug_log("[libc] prctl(%d) ignored\n", option);
  return 0;
}

/* ---- futex ---------------------------------------------------------------
 *
 * The runtime does reach futex directly, despite the pthread shims -- its
 * low-level monitor is built on it. Returning ENOSYS left waiters with no way
 * to block or be woken, which is a silent hang rather than a visible error.
 *
 * Waiters are parked on one of a fixed set of condition variables chosen by
 * hashing the address. Two unrelated addresses can therefore share a bucket
 * and wake each other spuriously -- which is fine, because the futex contract
 * already requires callers to re-check their predicate after waking. Erring
 * towards too many wakeups is safe; too few is a deadlock. */
#define FUTEX_WAIT_OP        0
#define FUTEX_WAKE_OP        1
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BUCKETS        256

static struct { Mutex m; CondVar cv; } g_futex[FUTEX_BUCKETS];
static bool g_futex_ready;

static void futex_setup(void) {
  if (g_futex_ready) return;
  for (int i = 0; i < FUTEX_BUCKETS; i++) {
    mutexInit(&g_futex[i].m);
    condvarInit(&g_futex[i].cv);
  }
  g_futex_ready = true;
}

/* Shift past the low bits -- these addresses are word-aligned -- then mix,
 * because futex words in one object are often a fixed small distance apart and
 * a plain shift maps them onto a handful of neighbouring buckets. Collisions
 * are no longer a correctness problem now that a wake signals the whole
 * bucket, but every collision turns into spurious wakeups for unrelated
 * threads, so it is worth spreading them out. */
static unsigned futex_bucket(const void *a) {
  uintptr_t x = (uintptr_t)a >> 3;
  x *= 0x9E3779B97F4A7C15ull;          /* 64-bit golden-ratio mix */
  x ^= x >> 29;
  return (unsigned)(x % FUTEX_BUCKETS);
}

static long futex_impl(int *uaddr, int op, int val, const struct timespec *ts) {
  if (!uaddr) { errno = BIONIC_EINVAL; return -1; }
  futex_setup();

  int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
  unsigned b = futex_bucket(uaddr);

  if (cmd == FUTEX_WAIT_OP) {
    mutexLock(&g_futex[b].m);
    /* The value check must happen under the lock, or a wake between the test
     * and the wait is lost and the thread sleeps forever. */
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
      mutexUnlock(&g_futex[b].m);
      errno = BIONIC_EAGAIN;
      return -1;
    }
    /* Never block indefinitely -- re-poll in bounded hops instead.
     *
     * An infinite condvarWait is correct only if every wake reaches its waiter.
     * That is a strong assumption for a shim: the buckets are shared, wakes can
     * be issued before a waiter has parked, and a single missed signal is a
     * permanent hang with nothing in any log. This port has already lost a wake
     * once, to condvarWakeOne on a shared bucket.
     *
     * Waiting in 16 ms hops and re-reading the word makes a lost wake cost 16
     * milliseconds instead of the process. The futex contract already permits
     * spurious wakeups, so every caller re-checks its own predicate; this just
     * guarantees the re-check actually happens. Borrowed from the
     * fruitninja_nx loader, which does the same thing for the same reason.
     *
     * Cost when nothing is wrong: one extra condvar timeout per 16 ms of idle
     * waiting, which is noise next to what the waiting thread is doing anyway. */
    const u64 HOP_NS = 16000000ull;          /* 16 ms */
    u64 remaining = 0;
    int bounded = ts != NULL;
    if (bounded) remaining = (u64)ts->tv_sec * 1000000000ull + (u64)ts->tv_nsec;

    for (;;) {
      u64 hop = HOP_NS;
      if (bounded && remaining < hop) hop = remaining;

      Result rc = condvarWaitTimeout(&g_futex[b].cv, &g_futex[b].m, hop);

      /* Woken, or the hop expired -- either way, re-read the word. If it moved
       * we are done, whether or not a wake actually reached us. */
      if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
        mutexUnlock(&g_futex[b].m);
        return 0;
      }
      if (R_SUCCEEDED(rc)) continue;         /* genuine wake, value unchanged */

      if (bounded) {
        remaining -= hop;
        if (remaining == 0) {
          mutexUnlock(&g_futex[b].m);
          errno = BIONIC_ETIMEDOUT;
          return -1;
        }
      }
      /* Unbounded wait: keep hopping. */
    }
  }

  if (cmd == FUTEX_WAKE_OP) {
    mutexLock(&g_futex[b].m);
    /* ALWAYS wake the whole bucket, never just one.
     *
     * This used to signal one waiter when the caller asked for one, which is
     * what a real futex does -- but a real futex wakes a thread waiting on
     * *that address*, and this bucket is shared by every address that hashes
     * to it. Waking one could therefore wake a thread parked on a completely
     * different address, which re-checks its own predicate, finds it unchanged
     * and goes back to sleep, while the thread the wake was meant for is never
     * woken at all. A lost wakeup, and permanent: nothing retries a futex
     * wake.
     *
     * That is the failure the paragraph above this function already warns
     * about -- "too many wakeups is safe; too few is a deadlock" -- and the
     * code then did the unsafe thing anyway. Waking the bucket costs a few
     * spurious returns, which the futex contract explicitly permits and every
     * caller already handles by re-checking. */
    condvarWakeAll(&g_futex[b].cv);
    mutexUnlock(&g_futex[b].m);
    return val < 0 ? 0 : val;
  }

  static int warned;
  if (!warned) {
    warned = 1;
    debug_log("[libc] futex op %d (cmd %d) unsupported -- only WAIT and WAKE "
              "are implemented\n", op, cmd);
  }
  errno = BIONIC_ENOSYS;
  return -1;
}

/* Raw syscalls.
 *
 * Most of what the runtime needs goes through libc and lands in the shims
 * above. A handful it issues directly, and those arrive here as numbers. Three
 * kinds of answer are correct, and picking the wrong one is worse than not
 * implementing the call at all:
 *
 *   route    the shim already exists -- forward to it, so raw and libc paths
 *            cannot disagree about the same state
 *   ENOSYS   the honest answer, and one the caller is built to handle. glibc
 *            probes for rseq and seccomp exactly this way and falls back.
 *   log      anything unrecognised, once per number, with a name where known
 *
 * What must NOT happen is a blanket 0. A syscall the runtime believes
 * succeeded when it did nothing is the hardest class of bug to find here.
 */

/* asm-generic/unistd.h numbering, which is what AArch64 uses. */
#define SYS_ioctl_a64          29
#define SYS_ftruncate_a64      46
#define SYS_close_a64          57
#define SYS_lseek_a64          62
#define SYS_read_a64           63
#define SYS_write_a64          64
#define SYS_pread64_a64        67
#define SYS_pwrite64_a64       68
#define SYS_fsync_a64          82
#define SYS_exit_a64           93
#define SYS_exit_group_a64     94
#define SYS_set_tid_address_a64 96
#define SYS_futex_a64          98
#define SYS_set_robust_list_a64 99
#define SYS_nanosleep_a64      101
#define SYS_clock_gettime_a64  113
#define SYS_sched_getaffinity_a64 123
#define SYS_sched_yield_a64    124
#define SYS_kill_a64           129
#define SYS_tkill_a64          130
#define SYS_tgkill_a64         131
#define SYS_sigaltstack_a64    132
#define SYS_uname_a64          160
#define SYS_getrlimit_a64      163
#define SYS_getrusage_a64      165
#define SYS_prctl_a64          167
#define SYS_gettimeofday_a64   169
#define SYS_getpid_a64         172
#define SYS_getuid_a64         174
#define SYS_geteuid_a64        175
#define SYS_getegid_a64        177
#define SYS_gettid_a64         178
#define SYS_sysinfo_a64        179
#define SYS_munmap_a64         215
#define SYS_mmap_a64           222
#define SYS_mprotect_a64       226
#define SYS_madvise_a64        233
#define SYS_prlimit64_a64      261
#define SYS_getrandom_a64      278
#define SYS_rseq_a64           293

static const char *syscall_name(long n) {
  switch (n) {
    /* 283 turned up as "unknown" in a log. It is membarrier: the runtime probes
     * it for GCToOSInterface::FlushProcessWriteBuffers and falls back to an
     * mprotect-based barrier when it is refused, so ENOSYS is the right answer
     * -- but the name belongs in the log so the next reader does not have to
     * look it up. */
    case 283: return "membarrier";
    case SYS_futex_a64:           return "futex";
    case SYS_set_tid_address_a64: return "set_tid_address";
    case SYS_set_robust_list_a64: return "set_robust_list";
    case SYS_rseq_a64:            return "rseq";
    case SYS_sigaltstack_a64:     return "sigaltstack";
    case SYS_kill_a64:            return "kill";
    case SYS_prlimit64_a64:       return "prlimit64";
    case SYS_gettimeofday_a64:    return "gettimeofday";
    default:                      return "unknown";
  }
}

long syscall_fake(long number, ...) {
  va_list ap;
  va_start(ap, number);
  long a0 = va_arg(ap, long);
  long a1 = va_arg(ap, long);
  long a2 = va_arg(ap, long);
  long a3 = va_arg(ap, long);
  long a4 = va_arg(ap, long);
  long a5 = va_arg(ap, long);
  va_end(ap);
  (void)a3; (void)a4; (void)a5;

  switch (number) {
    /* ---- identity ---- */
    case SYS_gettid_a64:  return gettid_fake();
    case SYS_getpid_a64:  return getpid_fake();
    case SYS_getuid_a64:  return getuid_fake();
    case SYS_geteuid_a64: return geteuid_fake();
    case SYS_getegid_a64: return getegid_fake();

    /* ---- signals ----
     * tkill/tgkill are the reason this dispatch exists at all. Routing them
     * through the same path as pthread_kill keeps the GC watch honest: a
     * hijack sent by raw syscall would otherwise never be counted, and the
     * hijack total is the number the whole port is judged on. */
    case SYS_tkill_a64:
      return pthread_kill_fake(threads_by_tid((int)a0), (int)a1);
    case SYS_tgkill_a64:
      return pthread_kill_fake(threads_by_tid((int)a1), (int)a2);
    case SYS_kill_a64:
      debug_log("[libc] kill(%ld, %ld) ignored\n", a0, a1);
      return 0;

    /* ---- scheduling and time ---- */
    case SYS_sched_yield_a64:       return sched_yield_fake();
    case SYS_sched_getaffinity_a64: return sched_getaffinity_fake((int)a0, (size_t)a1, (void *)a2);
    case SYS_nanosleep_a64:         return nanosleep_fake((const struct timespec *)a0,
                                                          (struct timespec *)a1);
    case SYS_clock_gettime_a64:     return clock_gettime_fake((int)a0, (struct timespec *)a1);

    /* ---- memory: must go to the arena, never to newlib ---- */
    case SYS_mmap_a64:
      return (long)(intptr_t)mmap_fake((void *)a0, (size_t)a1, (int)a2, (int)a3, (int)a4, a5);
    case SYS_munmap_a64:   return munmap_fake((void *)a0, (size_t)a1);
    case SYS_mprotect_a64: return mprotect_fake((void *)a0, (size_t)a1, (int)a2);
    case SYS_madvise_a64:  return madvise_fake((void *)a0, (size_t)a1, (int)a2);

    /* ---- files ---- */
    case SYS_read_a64:      return read_fake((int)a0, (void *)a1, (size_t)a2);
    case SYS_write_a64:     return write_fake((int)a0, (const void *)a1, (size_t)a2);
    case SYS_close_a64:     return close_fake((int)a0);
    case SYS_lseek_a64:     return lseek64_fake((int)a0, a1, (int)a2);
    case SYS_pread64_a64:   return pread_fake((int)a0, (void *)a1, (size_t)a2, a3);
    case SYS_pwrite64_a64:  return pwrite_fake((int)a0, (const void *)a1, (size_t)a2, a3);
    case SYS_fsync_a64:     return fsync_fake((int)a0);
    case SYS_ftruncate_a64: return ftruncate64_fake((int)a0, a1);
    case SYS_ioctl_a64:     errno = BIONIC_ENOTTY; return -1;

    /* ---- system description ---- */
    case SYS_uname_a64:     return uname_fake((void *)a0);
    case SYS_sysinfo_a64:   return sysinfo_fake((void *)a0);
    case SYS_getrlimit_a64: return getrlimit_fake((int)a0, (void *)a1);
    case SYS_getrusage_a64: return getrusage_fake((int)a0, (void *)a1);
    case SYS_prctl_a64:     return prctl_fake((int)a0, (unsigned long)a1,
                                              (unsigned long)a2, (unsigned long)a3,
                                              (unsigned long)a4);
    case SYS_getrandom_a64:
      randomGet((void *)a0, (size_t)a1);
      return (long)a1;

    case SYS_gettimeofday_a64: {
      struct timespec ts;
      clock_gettime_fake(0 /* CLOCK_REALTIME */, &ts);
      if (a0) {
        long *tv = (long *)a0;      /* struct timeval { time_t; suseconds_t; } */
        tv[0] = (long)ts.tv_sec;
        tv[1] = ts.tv_nsec / 1000;
      }
      return 0;
    }

    /* ---- correctly unsupported ----
     * Each of these is a probe. Returning failure is the answer the caller is
     * written to handle; pretending success is not. */
    case SYS_rseq_a64:            errno = BIONIC_ENOSYS; return -1;  /* falls back */
    case SYS_set_robust_list_a64: errno = BIONIC_ENOSYS; return -1;  /* advisory    */
    case SYS_sigaltstack_a64:     return 0;   /* no signals; accepting is fine */
    case SYS_set_tid_address_a64: return gettid_fake();
    case SYS_futex_a64:
      return futex_impl((int *)a0, (int)a1, (int)a2, (const struct timespec *)a3);

    case SYS_exit_a64:
    case SYS_exit_group_a64:
      fatal_error("the runtime called exit(%ld) directly", a0);

    default: break;
  }

  static int logged[24];
  static int nlogged;
  for (int i = 0; i < nlogged; i++) if (logged[i] == (int)number) { errno = BIONIC_ENOSYS; return -1; }
  if (nlogged < 24) {
    logged[nlogged++] = (int)number;
    debug_log("[libc] syscall %ld (%s) unimplemented -> ENOSYS. "
              "Add a case in libc_shim.c if the runtime depends on it.\n",
              number, syscall_name(number));
  }
  errno = BIONIC_ENOSYS;
  return -1;
}

int getpid_fake(void)  { return 1; }
int getuid_fake(void)  { return 1000; }
int geteuid_fake(void) { return 1000; }
int getegid_fake(void) { return 1000; }
int getgroups_fake(int size, void *list) { (void)size; (void)list; return 0; }
int gethostname_fake(char *name, size_t len) { snprintf(name, len, "Switch"); return 0; }
int getpagesize_fake(void) { return 0x1000; }
mode_t umask_fake(mode_t m) { (void)m; return 0; }
mode_t __umask_chk_fake(mode_t m) { (void)m; return 0; }

char *getcwd_fake(char *buf, size_t size) {
  /* Android form, not sdmc:. Managed code reaches this through
   * Directory.GetCurrentDirectory and then parses it -- a device prefix put a
   * colon where .NET expects a path separator and threw out of
   * Path.RemoveRelativeSegments. vfs_translate maps it back. */
  snprintf(buf, size, "/data/data/com.pvz.ultimate");
  return buf;
}

void arc4random_buf_fake(void *buf, size_t n) {
  randomGet(buf, n);
}

/* Declared in <stdlib.h> but not implemented by devkitA64's newlib.
 *
 * Note the return convention: posix_memalign returns the error code directly
 * and does NOT set errno. Getting that backwards produces a caller that thinks
 * every allocation failed. */
int posix_memalign_fake(void **memptr, size_t alignment, size_t size) {
  if (!memptr) return EINVAL;
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;
  void *p = memalign(alignment, size);
  if (!p) return ENOMEM;
  *memptr = p;
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Networking and IPC: fail cleanly                                          */
/* ------------------------------------------------------------------------ */

/* The game is offline. Every one of these returning a clean failure is better
 * than a stub returning success, which would leave the runtime waiting on a
 * socket that will never speak. */
int   net_fail_int(void)   { errno = BIONIC_ENETDOWN; return -1; }

/* bionic exposes __cmsg_nxthdr as a real function where glibc has a macro.
 * It walks ancillary control messages on a received socket message; with no
 * sockets there is never a next header, and NULL is how that is spelled. */
void *cmsg_nxthdr_fake(void *mhdr, void *cmsg) { (void)mhdr; (void)cmsg; return NULL; }
void *net_fail_ptr(void)   { errno = BIONIC_ENETDOWN; return NULL; }
const char *gai_strerror_fake(int e) { (void)e; return "networking unavailable"; }
void  freeaddrinfo_fake(void *ai)    { (void)ai; }
unsigned if_nametoindex_fake(const char *n) { (void)n; return 0; }

void openlog_fake(const char *id, int opt, int fac) { (void)id; (void)opt; (void)fac; }
void closelog_fake(void) {}
void syslog_fake(int prio, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debug_log("[syslog:%d] %s\n", prio, buf);
}

/* epoll: the runtime creates one for its event loop even with no sockets. An
 * epoll_wait that returns 0 (timeout, no events) forever is correct here. */
int epoll_create1_fake(int flags) { (void)flags; return 900; }
int epoll_ctl_fake(int ep, int op, int fd, void *ev) {
  (void)ep; (void)op; (void)fd; (void)ev; return 0;
}
int epoll_wait_fake(int ep, void *events, int maxev, int timeout) {
  (void)ep; (void)events; (void)maxev;
  if (timeout > 0) svcSleepThread((u64)timeout * 1000000ull);
  else if (timeout < 0) svcSleepThread(1000000ull);
  return 0;
}

int poll_fake(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds;
  if (timeout > 0) svcSleepThread((u64)timeout * 1000000ull);
  return 0;
}

void libc_shim_init(void) {
  memset(__sF_fake, 0, sizeof(__sF_fake));
  debug_log("[libc] __sF region %p .. %p (stride-agnostic)\n",
            __sF_fake, __sF_fake + sizeof(__sF_fake));
}
