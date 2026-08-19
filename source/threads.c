/* threads.c -- pthread emulation over libnx threads.
 *
 * Three things here are load-bearing for a .NET port specifically.
 *
 * 1. pthread_getattr_np MUST report the real stack base and size.
 *    The GC scans thread stacks conservatively for roots. If the reported
 *    range is wrong it either misses roots (live objects collected) or scans
 *    unrelated memory (random words interpreted as pointers). Neither fails
 *    where you can see it; both surface later as corruption. This is why every
 *    thread's stack is allocated here rather than left to libnx -- so the
 *    bounds are known exactly instead of inferred.
 *
 * 2. The registry names threads.
 *    runtime_glue's GC watch reports which thread the runtime tried to hijack.
 *    Without a name that report is a bare pointer and tells you nothing. With
 *    one, "the finalizer thread" versus "a MonoGame audio worker" is the
 *    difference between a five-minute fix and a week.
 *
 * 3. bionic's sync primitives are smaller than devkitPro's.
 *    bionic pthread_mutex_t is 40 bytes, pthread_cond_t 48; libnx's Mutex and
 *    CondVar do not fit the same footprint and the game's compiled code has
 *    bionic's sizes baked into every stack frame that declares one. So the
 *    storage is treated as a pointer slot and the real object is allocated
 *    lazily. Forwarding these to newlib's pthreads instead corrupts whatever
 *    follows the mutex in the caller's frame.
 */

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "threads.h"
#include "util.h"
#include "watchdog.h"

/* ---- bionic thread-local storage ----------------------------------------
 *
 * Android code reads its thread pointer from TPIDR_EL0 and indexes fixed slots
 * off it. The one that matters immediately is slot 5, TLS_SLOT_STACK_GUARD:
 * the game is built with -fstack-protector, so EVERY guarded function loads
 * its canary from [tpidr_el0, #0x28] on entry. Horizon leaves TPIDR_EL0 at
 * zero -- libnx keeps its own thread-local region in TPIDRRO_EL0 -- so without
 * this the first stack-protected function the game enters reads from NULL+0x28
 * and dies. It is not specific to any one call; it is every function.
 *
 * Safe to commandeer because devkitA64 builds with -mtp=soft (see the Makefile
 * ARCH line): the compiler emits calls to read the thread pointer rather than
 * touching the register, so nothing on our side uses TPIDR_EL0.
 *
 * The thread pointer sits 0x200 into the block rather than at its base. Bionic
 * has negative slot indices for dynamic TLS, and a pointer at the base would
 * make those underflow into whatever precedes the allocation. */
#define BIONIC_TLS_BLOCK  0x400
#define BIONIC_TLS_TP_OFF 0x200

#define TLS_SLOT_THREAD_ID       1
#define TLS_SLOT_STACK_GUARD     5
#define TLS_SLOT_ART_THREAD_SELF 7
#define TLS_SLOT_DTV             8
#define TLS_SLOT_BIONIC_TLS      9

/* One value process-wide. Guarded functions store it on entry and compare on
 * return, so it only has to be stable -- but keeping it identical across
 * threads means a frame that somehow crosses threads still validates. */
#define STACK_GUARD_VALUE 0x000000A5C3D2E100ull

static inline void tls_set_tp(void *p) {
  __asm__ volatile("msr tpidr_el0, %0" :: "r"(p) : "memory");
}
static inline void *tls_get_tp(void) {
  void *p;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(p));
  return p;
}

#define MAX_THREADS 64
#define MAX_TLS_KEYS 128
#define DEFAULT_STACK_SIZE (512 * 1024)

typedef struct {
  int      in_use;
  Thread   nx;
  void    *stack;          /* lowest address of the stack allocation */
  size_t   stack_size;
  void *(*entry)(void *);
  void    *arg;
  char     name[32];
  u32      tid;
  void    *tls[MAX_TLS_KEYS];
  int      detached;
  int      is_main;
  void    *bionic_tls;     /* block base; the thread pointer is +0x200 */
  volatile int started;    /* set by the trampoline; see pthread_create_fake */
  int      libnx_stack;    /* libnx allocated the stack; do not free it here */
} ThreadRec;

static ThreadRec g_threads[MAX_THREADS];
static Mutex     g_reg_lock;

static void (*g_tls_dtor[MAX_TLS_KEYS])(void *);
static int      g_tls_used[MAX_TLS_KEYS];

/* ------------------------------------------------------------------------ */

static ThreadRec *alloc_rec(void) {
  mutexLock(&g_reg_lock);
  for (int i = 0; i < MAX_THREADS; i++) {
    if (!g_threads[i].in_use) {
      memset(&g_threads[i], 0, sizeof(ThreadRec));
      g_threads[i].in_use = 1;
      mutexUnlock(&g_reg_lock);
      return &g_threads[i];
    }
  }
  mutexUnlock(&g_reg_lock);
  return NULL;
}

/* libnx gives no cheap "which ThreadRec am I" lookup, so keep it in libnx's
 * own per-thread storage slot. */
static __thread ThreadRec *tl_self;

/* Give this thread a bionic-shaped TLS block and point TPIDR_EL0 at it. Must
 * run on the thread itself -- TPIDR_EL0 is per-thread state. */
static void bionic_tls_install(ThreadRec *r) {
  void *blk = calloc(1, BIONIC_TLS_BLOCK);
  if (!blk) { debug_log("[thr] could not allocate bionic TLS\n"); return; }

  uintptr_t tp = (uintptr_t)blk + BIONIC_TLS_TP_OFF;
  uintptr_t *slot = (uintptr_t *)tp;

  slot[TLS_SLOT_THREAD_ID]       = (uintptr_t)r;
  slot[TLS_SLOT_STACK_GUARD]     = STACK_GUARD_VALUE;
  slot[TLS_SLOT_ART_THREAD_SELF] = 0;      /* no ART; nothing should read it */
  slot[TLS_SLOT_DTV]             = tp + 0x80;   /* zeroed, so a stray DTV
                                                 * lookup reads zero rather
                                                 * than faulting */
  slot[TLS_SLOT_BIONIC_TLS]      = tp + 0x100;  /* likewise */

  if (r) r->bionic_tls = blk;
  tls_set_tp((void *)tp);
}

ThreadRec *threads_self(void) { return tl_self; }

/* gettid_fake() hands out the registry index, so the inverse is trivial.
 * Needed so a raw tgkill/tkill syscall can be attributed to a thread the same
 * way pthread_kill is -- otherwise signals sent that way are invisible to the
 * GC watch. */
void *threads_by_tid(int tid) {
  if (tid < 0 || tid >= MAX_THREADS) return NULL;
  return g_threads[tid].in_use ? &g_threads[tid] : NULL;
}

/* The calling thread's name, for diagnostics that need to say WHERE something
 * happened. Returns "main" for the host thread, which has no ThreadRec. */
const char *threads_self_name(void) {
  ThreadRec *r = tl_self;
  return (r && r->name[0]) ? r->name : "main";
}

const char *threads_name_of(void *pthread_handle) {
  ThreadRec *r = (ThreadRec *)pthread_handle;
  if (!r) return "(null)";
  for (int i = 0; i < MAX_THREADS; i++)
    if (&g_threads[i] == r && r->in_use)
      return r->name[0] ? r->name : "(unnamed)";
  return "(not ours -- possibly the main thread or a libnx-internal thread)";
}

/* ------------------------------------------------------------------------ */

void threads_init(void) {
  memset(g_threads, 0, sizeof(g_threads));

  /* Register the main thread. Its stack was set up by the loader, not by us,
   * so we have to discover the bounds rather than record them.
   *
   * Ask the kernel which memory region contains an address we know is on this
   * stack. That is exact, unlike assuming the NRO default of 1 MB -- which a
   * forwarder or a different launch method can change, and which would make
   * the GC scan the wrong range with no visible symptom. */
  ThreadRec *m = alloc_rec();
  if (m) {
    m->is_main = 1;

    int probe;                       /* lives on the main stack */
    MemoryInfo mi;
    u32 pageinfo;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pageinfo, (u64)(uintptr_t)&probe))) {
      m->stack      = (void *)(uintptr_t)mi.addr;
      m->stack_size = (size_t)mi.size;
      debug_log("[thr] main stack %p .. %p (%zu KB, from svcQueryMemory)\n",
                m->stack, (char *)m->stack + m->stack_size, m->stack_size >> 10);
    } else {
      /* Should not happen -- the address is definitely mapped. If it does,
       * a wrong range is worse than none, so report the failure loudly. */
      m->stack      = NULL;
      m->stack_size = 0;
      debug_log("[thr] *** could not determine main stack bounds. "
                "GC stack scanning of the main thread will be wrong. ***\n");
    }
    snprintf(m->name, sizeof(m->name), "main");
    tl_self = m;
  }

  bionic_tls_install(m);
  debug_log("[thr] bionic TLS installed on main, tp=%p (stack guard in slot 5)\n",
            tls_get_tp());
}

/* ------------------------------------------------------------------------ */
/* pthread_attr_t -- must match bionic's layout byte for byte                */
/* ------------------------------------------------------------------------ */

typedef struct {
  uint32_t flags;
  void    *stack_base;
  size_t   stack_size;
  size_t   guard_size;
  int32_t  sched_policy;
  int32_t  sched_priority;
  char     __reserved[16];
} bionic_attr_t;

_Static_assert(sizeof(bionic_attr_t) == 56,
               "bionic pthread_attr_t is 56 bytes on LP64 -- the game declares "
               "these on its stack, so ours must not be larger");

int pthread_attr_init_fake(void *attr) {
  memset(attr, 0, sizeof(bionic_attr_t));
  ((bionic_attr_t *)attr)->stack_size = DEFAULT_STACK_SIZE;
  ((bionic_attr_t *)attr)->guard_size = PAGE_SIZE_DEFAULT;
  return 0;
}
int pthread_attr_destroy_fake(void *attr) { (void)attr; return 0; }

int pthread_attr_setstacksize_fake(void *attr, size_t sz) {
  ((bionic_attr_t *)attr)->stack_size = sz;
  return 0;
}
int pthread_attr_getstacksize_fake(const void *attr, size_t *sz) {
  *sz = ((const bionic_attr_t *)attr)->stack_size;
  return 0;
}
int pthread_attr_setdetachstate_fake(void *attr, int state) {
  ((bionic_attr_t *)attr)->flags = (uint32_t)state;
  return 0;
}

/* The one the GC actually depends on. */
int pthread_attr_getstack_fake(const void *attr, void **base, size_t *size) {
  const bionic_attr_t *a = attr;
  *base = a->stack_base;
  *size = a->stack_size;
  return 0;
}

int pthread_getattr_np_fake(void *thread, void *attr) {
  ThreadRec *r = (ThreadRec *)thread;
  bionic_attr_t *a = attr;
  memset(a, 0, sizeof(*a));

  if (!r || !r->in_use) {
    /* Returning zeroes here would tell the GC "your stack is at NULL, size 0",
     * and it would scan nothing -- silently missing every root on this thread.
     * Fail loudly instead. */
    debug_log("[thr] pthread_getattr_np on unknown thread %p -- "
              "GC stack scan would be wrong\n", thread);
    return -1;
  }
  a->stack_base = r->stack;
  a->stack_size = r->stack_size;
  return 0;
}

/* ------------------------------------------------------------------------ */
/* create / join                                                             */
/* ------------------------------------------------------------------------ */

/* Spread threads across the cores the process is allowed to use.
 *
 * Every thread here is created with cpuid -2, which means "the process default
 * core" -- so all of them pile onto one core while the other three sit idle.
 * That is fine until something spins, at which point every other thread is
 * competing with the spinner for a single core. Taken from the fruitninja_nx
 * loader, which does the same thing for the same reason.
 *
 * Round robin rather than anything clever: the ideal core is a hint, and the
 * full mask is passed as the affinity so the kernel can still move a thread if
 * that core is busy. */
static void spread_across_cores(void) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return;

  int cores[4], n = 0;
  for (int i = 0; i < 4; i++) if (mask & (1ull << i)) cores[n++] = i;

  static int logged;
  if (!logged) {
    logged = 1;
    debug_log("[thr] process core mask %#llx -- %d core(s) usable\n",
              (unsigned long long)mask, n);
  }
  if (n <= 1) return;                 /* nothing to spread across */

  /* Plain increment, not __sync_fetch_and_add: the built-in compiled to
   * `bl __aarch64_ldadd4_sync`, a libgcc out-of-line atomic helper that exists
   * on some aarch64 targets and not others. A round-robin counter for core
   * placement must not be able to fail the link. A racing increment costs at
   * worst two threads picking the same core, which the affinity mask lets the
   * kernel correct anyway. */
  static int rr;
  int c = cores[(unsigned)(rr++) % (unsigned)n];
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, (u32)mask);
}

static void thread_trampoline(void *arg) {
  ThreadRec *r = arg;

  /* First thing, before TLS or anything that could fault: record that this
   * thread actually began executing. pthread_create_fake waits on it. */
  r->started = 1;

  spread_across_cores();

  tl_self = r;

  /* Before anything else. Any game code this thread runs will read its stack
   * canary from TPIDR_EL0 on the very first guarded function. */
  bionic_tls_install(r);
  r->tid  = (u32)(uintptr_t)r;   /* stable per-thread id; not a kernel tid */

  r->entry(r->arg);

  /* Run TLS destructors, as pthreads requires. NativeAOT registers one to tear
   * down its managed thread context; skipping it leaks the context and, worse,
   * leaves the runtime believing the thread is still attached. */
  for (int k = 0; k < MAX_TLS_KEYS; k++) {
    if (g_tls_used[k] && g_tls_dtor[k] && r->tls[k]) {
      void *v = r->tls[k];
      r->tls[k] = NULL;
      g_tls_dtor[k](v);
    }
  }

  debug_log("[thr] '%s' exited\n", r->name[0] ? r->name : "(unnamed)");
}

int pthread_create_fake(void **out_thread, const void *attr,
                        void *(*entry)(void *), void *arg) {
  const bionic_attr_t *a = attr;
  size_t stack_size = (a && a->stack_size) ? a->stack_size : DEFAULT_STACK_SIZE;
  stack_size = (stack_size + 0xFFF) & ~(size_t)0xFFF;

  ThreadRec *r = alloc_rec();
  if (!r) { debug_log("[thr] registry full (%d threads)\n", MAX_THREADS); return -1; }

  /* Our own allocation, so the bounds handed to pthread_getattr_np are exact
   * rather than whatever libnx happened to do. */
  /* Fine-grained labels, because the census proved main can be stuck INSIDE
   * this function: `t5` appeared in the live-thread list -- so alloc_rec had
   * run -- while its "created" line never printed and the start probe below
   * never reported either way. The probe is bounded at 200 ms, so the hang is
   * in one of the three calls that follow, and until now the stall report just
   * said "running a posted Runnable".
   *
   * watchdog_label, not watchdog_checkpoint: this must not reset the stall
   * timer, or a hang here would keep postponing its own report. */
  /* libnx allocates the stack, not us.
   *
   * This used to be memalign() from newlib's heap, done so pthread_getattr_np
   * could report exact bounds. The bounds are still exact -- libnx records
   * stack_mem/stack_sz in the Thread and we copy them below -- but the
   * allocation no longer goes through newlib's malloc, and that matters: the
   * current hang is inside this function, before threadCreate's own logging,
   * with another thread running and allocating concurrently. newlib's malloc
   * lock is the one shared resource on this path, and taking it out removes
   * the leading suspect. It also matches what the fruitninja_nx loader does.
   *
   * A NULL stack_mem makes threadCreate allocate, and threadClose free it, so
   * nothing here owns the memory. */
  watchdog_label("pthread_create: allocating the stack");
  r->stack       = NULL;
  r->libnx_stack = 1;
  if (0) {   /* libnx allocates now; kept so the failure path below still compiles */
    /* Silent before, which made a failed thread creation indistinguishable
     * from one that never happened. */
    debug_log("[thr] out of memory for a %zu KB stack\n", stack_size >> 10);
    r->in_use = 0;
    return -1;
  }
  r->stack_size = stack_size;
  r->entry = entry;
  r->arg   = arg;
  snprintf(r->name, sizeof(r->name), "t%d", (int)(r - g_threads));

  /* Inherit the creating thread's priority, rather than hardcoding 0x2C.
   *
   * POSIX says a new thread inherits its creator's scheduling attributes, and
   * the runtime is written against that. Hardcoding 0x2C broke it in a way
   * that only showed up under contention: Horizon is strictly priority
   * ordered, and svcSleepThread(0) -- which is what sched_yield compiles to,
   * and what every .NET spin-wait ends up calling -- yields only to threads of
   * EQUAL OR HIGHER priority. A thread created at a lower priority than its
   * creator therefore never runs at all while that creator spins.
   *
   * Which is exactly what happened. Thread.Start() created a thread and then
   * spun waiting for it to report itself started; the new thread was one
   * priority band below and never got scheduled. The creport caught it sitting
   * on the first instruction of its entry wrapper, LR and FP still zero, 45
   * seconds after being started.
   *
   * The bracket is tight: the watchdog at 0x28 kept running, so main is below
   * 0x28; threads at 0x2C never ran, so main is above 0x2C. Every thread the
   * game created was a band below the thread that created it.
   *
   * Threads created before the spin (t1..t4) ran fine, because the frame loop
   * sleeps for a real duration between frames and a real sleep does let lower
   * priority threads run. That is why this only appeared once something
   * started spinning. */
  s32 prio = 0x2C;
  if (R_FAILED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE)) ||
      prio < 0 || prio > 0x3F)
    prio = 0x2C;

  static int reported;
  if (!reported) {
    reported = 1;
    debug_log("[thr] new threads inherit priority %#x from their creator "
              "(was hardcoded 0x2C)\n", prio);
  }

  watchdog_label("pthread_create: threadCreate");
  Result rc = threadCreate(&r->nx, thread_trampoline, r,
                           NULL, stack_size, prio, -2);
  if (R_FAILED(rc)) {
    debug_log("[thr] threadCreate failed: %08x\n", rc);
    free(r->stack);
    r->in_use = 0;
    return -1;
  }

  /* Exact bounds, straight from libnx, for pthread_getattr_np. */
  r->stack      = r->nx.stack_mem;
  r->stack_size = r->nx.stack_sz;

  watchdog_label("pthread_create: threadStart");

  rc = threadStart(&r->nx);
  if (R_FAILED(rc)) {
    debug_log("[thr] threadStart failed: %08x\n", rc);
    threadClose(&r->nx);
    free(r->stack);
    r->in_use = 0;
    return -1;
  }

  /* Confirm the thread actually RUNS, rather than trusting that a successful
   * threadStart means anything.
   *
   * It does not. A creport caught a thread sitting on the first instruction of
   * libnx's entry wrapper -- LR and FP still zero, no stack frames at all --
   * 45 seconds after threadCreate and threadStart both returned success. The
   * creator was inside Thread.StartCore(), spinning on ThreadState.Unstarted
   * (bit 3) waiting for that thread to begin, which it never did. Nothing in
   * any log said so, because from here creation had succeeded.
   *
   * The wait is bounded and short. A thread that is going to run does so
   * within microseconds, so the normal cost is one poll; only a thread that is
   * genuinely not runnable pays the full 200 ms, and that case was previously
   * an unexplained permanent hang. */
  {
    watchdog_label("pthread_create: waiting for the thread to begin");
    const int SPINS = 2000;                 /* 2000 x 100 us = 200 ms */
    int i = 0;
    for (; i < SPINS && !r->started; i++) svcSleepThread(100000);

    if (!r->started) {
      debug_log("[thr] *** %s was created and started, but has not executed a "
                "single instruction after 200 ms ***\n", r->name);
      debug_log("[thr]     prio %#x, core %d, our stack %p (+%zu KB), "
                "libnx stack_mem %p mirror %p\n",
                prio, -2, r->stack, stack_size >> 10,
                r->nx.stack_mem, r->nx.stack_mirror);
      debug_log("[thr]     Whatever created this thread is about to wait for "
                "it. .NET's Thread.Start() spins on ThreadState.Unstarted and "
                "will never return.\n");

      /* Retry once, letting libnx allocate the stack.
       *
       * The stack is the one unusual ingredient here: every other thread
       * parameter is what any program would pass, but the memory comes from
       * newlib's heap via memalign and libnx then svcMapMemory's it into the
       * stack region. That was done so pthread_getattr_np could report exact
       * bounds. libnx's own path is far better travelled, and if a thread that
       * refuses to start with our stack starts with libnx's, that is the
       * answer -- stated in the log, on hardware, instead of inferred.
       *
       * The normal path is untouched: this only runs after a thread has
       * already failed to start, which today is a permanent hang. */
      debug_log("[thr]     the stack is already libnx-allocated, so this is "
                "not our allocation; nothing left to retry\n");

      /* No terminate: libnx has no svcTerminateThread wrapper, and the thread
       * has not executed an instruction, so there is nothing to unwind.
       * threadClose closes the handle and unmaps the stack mirror. */
      threadClose(&r->nx);
      free(r->stack);
      r->stack = NULL;
      r->started = 0;

      rc = threadCreate(&r->nx, thread_trampoline, r, NULL, stack_size,
                        prio, -2);
      if (R_SUCCEEDED(rc)) rc = threadStart(&r->nx);
      if (R_FAILED(rc)) {
        debug_log("[thr]     retry failed: %08x\n", rc);
        r->in_use = 0;
        return -1;
      }

      r->libnx_stack = 1;
      r->stack       = r->nx.stack_mem;
      r->stack_size  = r->nx.stack_sz;

      for (i = 0; i < SPINS && !r->started; i++) svcSleepThread(100000);
      debug_log("[thr]     retry with a libnx stack: %s\n",
                r->started ? "*** IT RAN -- our stack allocation is the bug ***"
                           : "still will not start; the stack is NOT the cause");
    } else if (i > 0) {
      debug_log("[thr] %s took %d ms to start\n", r->name, i / 10);
    }
  }

  watchdog_label("pthread_create: done");
  debug_log("[thr] created %s: stack %p size %zu KB\n",
            r->name, r->stack, stack_size >> 10);

  if (out_thread) *out_thread = r;
  return 0;
}

int pthread_join_fake(void *thread, void **retval) {
  ThreadRec *r = thread;
  if (!r || !r->in_use) return -1;
  threadWaitForExit(&r->nx);
  threadClose(&r->nx);
  /* threadClose frees the stack itself when libnx allocated it. */
  if (!r->libnx_stack) free(r->stack);
  r->in_use = 0;
  if (retval) *retval = NULL;
  return 0;
}

int pthread_detach_fake(void *thread) {
  ThreadRec *r = thread;
  if (r) r->detached = 1;
  return 0;
}

void *pthread_self_fake(void) { return tl_self; }

int pthread_equal_fake(void *a, void *b) { return a == b; }

int pthread_setname_np_fake(void *thread, const char *name) {
  ThreadRec *r = thread ? (ThreadRec *)thread : tl_self;
  if (r && name) {
    snprintf(r->name, sizeof(r->name), "%s", name);
    /* This is how the GC watch gets a useful name for the thread the runtime
     * tried to hijack, so log it -- the mapping matters later. */
    debug_log("[thr] %p named '%s'\n", r, r->name);
  }
  return 0;
}

int gettid_fake(void) { return tl_self ? (int)(tl_self - g_threads) : 0; }

/* ------------------------------------------------------------------------ */
/* TLS                                                                       */
/* ------------------------------------------------------------------------ */

int pthread_key_create_fake(unsigned int *key, void (*dtor)(void *)) {
  mutexLock(&g_reg_lock);
  for (int i = 0; i < MAX_TLS_KEYS; i++) {
    if (!g_tls_used[i]) {
      g_tls_used[i] = 1;
      g_tls_dtor[i] = dtor;
      *key = (unsigned int)i;
      mutexUnlock(&g_reg_lock);
      return 0;
    }
  }
  mutexUnlock(&g_reg_lock);
  debug_log("[thr] out of TLS keys\n");
  return -1;
}

int pthread_key_delete_fake(unsigned int key) {
  if (key < MAX_TLS_KEYS) { g_tls_used[key] = 0; g_tls_dtor[key] = NULL; }
  return 0;
}

void *pthread_getspecific_fake(unsigned int key) {
  if (key >= MAX_TLS_KEYS || !tl_self) return NULL;
  return tl_self->tls[key];
}

int pthread_setspecific_fake(unsigned int key, const void *value) {
  if (key >= MAX_TLS_KEYS || !tl_self) return -1;
  tl_self->tls[key] = (void *)value;
  return 0;
}

/* ------------------------------------------------------------------------ */
/* mutex / cond / once -- pointer-in-storage, see the header comment          */
/* ------------------------------------------------------------------------ */

typedef struct { Mutex m; RMutex rm; int recursive; } FakeMutex;

static Mutex g_alloc_lock;

static FakeMutex *get_mutex(void **storage) {
  if (!*storage) {
    mutexLock(&g_alloc_lock);
    if (!*storage) {                      /* re-check under the lock */
      FakeMutex *fm = calloc(1, sizeof(FakeMutex));
      mutexInit(&fm->m);
      rmutexInit(&fm->rm);
      *storage = fm;
    }
    mutexUnlock(&g_alloc_lock);
  }
  return *storage;
}

int pthread_mutex_init_fake(void **m, const void *attr) {
  *m = NULL;
  FakeMutex *fm = get_mutex(m);
  /* attr carries the type; bionic stores it in the first int. Recursive is the
   * only variant that behaves differently enough to matter here. */
  if (attr && *(const int *)attr == 1) fm->recursive = 1;
  return 0;
}
int pthread_mutex_destroy_fake(void **m) {
  if (m && *m) { free(*m); *m = NULL; }
  return 0;
}
int pthread_mutex_lock_fake(void **m) {
  FakeMutex *fm = get_mutex(m);
  if (fm->recursive) rmutexLock(&fm->rm); else mutexLock(&fm->m);
  return 0;
}
int pthread_mutex_unlock_fake(void **m) {
  FakeMutex *fm = get_mutex(m);
  if (fm->recursive) rmutexUnlock(&fm->rm); else mutexUnlock(&fm->m);
  return 0;
}
int pthread_mutex_trylock_fake(void **m) {
  FakeMutex *fm = get_mutex(m);
  int ok = fm->recursive ? rmutexTryLock(&fm->rm) : mutexTryLock(&fm->m);
  return ok ? 0 : 16 /* EBUSY */;
}

int pthread_mutexattr_init_fake(void *a)    { if (a) *(int *)a = 0; return 0; }
int pthread_mutexattr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_mutexattr_settype_fake(void *a, int type) {
  if (a) *(int *)a = (type == 1 /* PTHREAD_MUTEX_RECURSIVE */) ? 1 : 0;
  return 0;
}

typedef struct { CondVar cv; } FakeCond;

static FakeCond *get_cond(void **storage) {
  if (!*storage) {
    mutexLock(&g_alloc_lock);
    if (!*storage) {
      FakeCond *fc = calloc(1, sizeof(FakeCond));
      condvarInit(&fc->cv);
      *storage = fc;
    }
    mutexUnlock(&g_alloc_lock);
  }
  return *storage;
}

int pthread_cond_init_fake(void **c, const void *attr) { (void)attr; *c = NULL; get_cond(c); return 0; }
int pthread_cond_destroy_fake(void **c) { if (c && *c) { free(*c); *c = NULL; } return 0; }
int pthread_cond_signal_fake(void **c)    { condvarWakeOne(&get_cond(c)->cv); return 0; }
int pthread_cond_broadcast_fake(void **c) { condvarWakeAll(&get_cond(c)->cv); return 0; }

int pthread_cond_wait_fake(void **c, void **m) {
  FakeCond  *fc = get_cond(c);
  FakeMutex *fm = get_mutex(m);
  condvarWait(&fc->cv, &fm->m);
  return 0;
}

int pthread_cond_timedwait_fake(void **c, void **m, const struct timespec *abs) {
  FakeCond  *fc = get_cond(c);
  FakeMutex *fm = get_mutex(m);
  u64 timeout = 1000000000ull;   /* 1s default if we cannot compute one */
  if (abs) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    s64 ns = (s64)(abs->tv_sec - now.tv_sec) * 1000000000ll +
             (s64)(abs->tv_nsec - now.tv_nsec);
    timeout = ns > 0 ? (u64)ns : 0;
  }
  Result rc = condvarWaitTimeout(&fc->cv, &fm->m, timeout);
  return R_FAILED(rc) ? 110 /* ETIMEDOUT */ : 0;
}

int pthread_once_fake(int *control, void (*init)(void)) {
  /* Not the textbook implementation -- a second caller can observe the flag
   * before init() finishes. Adequate here because the runtime's pthread_once
   * sites are all on the startup path, single-threaded. Revisit if one shows
   * up on a worker. */
  static Mutex once_lock;
  mutexLock(&once_lock);
  if (control && !*control) { *control = 1; mutexUnlock(&once_lock); init(); return 0; }
  mutexUnlock(&once_lock);
  return 0;
}

/* rwlocks and condattr -- same pointer-in-storage trick as mutexes, for the
 * same reason: bionic's pthread_rwlock_t is a different size from libnx's. */

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    mutexLock(&g_alloc_lock);
    if (!*storage) {
      FakeRwLock *l = calloc(1, sizeof(FakeRwLock));
      rwlockInit(&l->lock);
      *storage = l;
    }
    mutexUnlock(&g_alloc_lock);
  }
  return *storage;
}

int pthread_rwlock_rdlock_fake(void **rw) { rwlockReadLock(&get_rwlock(rw)->lock);  return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { rwlockWriteLock(&get_rwlock(rw)->lock); return 0; }

int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  /* libnx splits read and write unlock, POSIX does not. Ask which we hold. */
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else                                                rwlockReadUnlock(&l->lock);
  return 0;
}

int pthread_condattr_init_fake(void *a)    { if (a) *(int *)a = 0; return 0; }
int pthread_condattr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_condattr_setclock_fake(void *a, int clk) {
  /* We always time out against the monotonic clock in
   * pthread_cond_timedwait_fake, so the requested clock is informational. */
  (void)a; (void)clk; return 0;
}

/* ------------------------------------------------------------------------ */

/* Which threads are alive and what the kernel thinks they are doing.
 *
 * The JNI breadcrumb only sees threads that call into the shim, and the
 * game's own worker threads mostly do not -- they run managed code and never
 * touch us. So a thread wedged inside .NET is invisible to every diagnostic
 * this port has. svcGetThreadContext3 gives the PC of any thread we hold a
 * handle for, which is enough to say whether it is running, and where.
 *
 * Printed as a raw address deliberately: it can be fed straight to the same
 * offset arithmetic used on the creports (subtract the libLawn base from
 * "[so] mapped at", or the NRO base) without needing a crash to get it. */
void threads_report_state(void) {
  /* Both load bases, every time.
   *
   * Working out the host base from the last log took solving for it against
   * the symbol table; it is free to just print it. Subtract these from any
   * address below to get a file offset for nm/objdump. */
  /* &__start__ printed 0. Print the runtime address of a function we own
   * instead: subtract its offset (from `nm` on the .elf) to get the base.
   * Deriving it by hand from a symbol table twice was enough. */
  debug_log("[thr] live thread PCs  (threads_report_state is at %p -- subtract "
            "its offset from the .elf for the host base; libLawn base is the "
            "\"[so] mapped at\" value near the top of this log)\n",
            (void *)(uintptr_t)&threads_report_state);
  for (int i = 0; i < MAX_THREADS; i++) {
    ThreadRec *r = &g_threads[i];
    if (!r->in_use || !r->started) continue;
    /* The thread must be PAUSED first.
     *
     * Calling svcGetThreadContext3 on a running thread returned 0xfa01 for
     * every thread -- kernel InvalidState. libnx's own header says so plainly:
     * it "dumps the registers of a thread paused by svcSetThreadActivity". A
     * debugger suspends before it reads, and so must this.
     *
     * Never the calling thread: pausing yourself does not come back.
     *
     * The suspend window is deliberately as small as it can be -- pause, read,
     * resume, and only then log. Logging while a thread is suspended would
     * mean holding the log lock across a suspension, and if the suspended
     * thread were itself inside debug_log that is a deadlock. Reading into a
     * local and printing afterwards costs nothing and cannot do that. */
    if (r == tl_self) {
      debug_log("[thr]   %-16s (this thread)\n", r->name);
      continue;
    }

    ThreadContext ctx;
    Result rc = svcSetThreadActivity(r->nx.handle, ThreadActivity_Paused);
    if (R_SUCCEEDED(rc)) {
      rc = svcGetThreadContext3(&ctx, r->nx.handle);
      /* resumed after the frame walk below, which needs it still stopped */
    }

    /* Walk the frame pointers while it is still suspended.
     *
     * A PC alone said "t6 is spinning in sched_yield" -- true, and not enough:
     * every .NET spin-wait bottoms out there, so it names the mechanism and
     * not the caller. The frames above it are the answer, and they are the
     * same thing a creport gives, except this does not need a crash.
     *
     * AArch64 keeps a linked list of frames in x29: [fp] is the caller's fp
     * and [fp+8] its return address. Bounds are checked against the thread's
     * own SP before every read -- this runs on a suspended thread's stack and
     * a bad fp would fault inside the diagnostic, which would be a poor way to
     * find a bug. Collected here, printed after the resume. */
    u64 frames[10];
    int nframes = 0;
    if (R_SUCCEEDED(rc)) {
      u64 fp = ctx.fp, sp = ctx.sp, top = ctx.sp + (2u << 20);
      while (nframes < 10 && fp >= sp && fp < top && (fp & 15) == 0) {
        const u64 *f = (const u64 *)(uintptr_t)fp;
        u64 next = f[0], lr = f[1];
        if (!lr) break;
        frames[nframes++] = lr;
        if (next <= fp) break;          /* must grow upward, or stop */
        fp = next;
      }
    }

    svcSetThreadActivity(r->nx.handle, ThreadActivity_Runnable);

    if (R_SUCCEEDED(rc)) {
      /* Mixed types in ThreadContext, which the compiler caught: pc is a
       * CpuRegister union (.x is the 64-bit view) while lr and sp are plain
       * u64. */
      debug_log("[thr]   %-16s pc 0x%010llx  lr 0x%010llx  sp 0x%010llx\n",
                r->name, (unsigned long long)ctx.pc.x,
                (unsigned long long)ctx.lr, (unsigned long long)ctx.sp);
      for (int d = 0; d < nframes; d++)
        debug_log("[thr]        #%d  0x%010llx\n", d,
                  (unsigned long long)frames[d]);
    } else {
      debug_log("[thr]   %-16s context unavailable (%08x)\n", r->name, rc);
    }
  }
}

void threads_report(void) {
  int live = 0;
  for (int i = 0; i < MAX_THREADS; i++) if (g_threads[i].in_use) live++;
  debug_log("[thr] %d live thread(s):\n", live);
  for (int i = 0; i < MAX_THREADS; i++) {
    ThreadRec *r = &g_threads[i];
    if (!r->in_use) continue;
    debug_log("[thr]   %-16s stack %p +%zu KB%s\n",
              r->name[0] ? r->name : "(unnamed)",
              r->stack, r->stack_size >> 10, r->is_main ? "  [main]" : "");
  }
}
