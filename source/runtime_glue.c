/* runtime_glue.c -- .NET runtime configuration and the GC suspension watch.
 *
 * Two jobs, both small, both disproportionately important.
 *
 * 1. Environment variables. The runtime reads its whole configuration through
 *    getenv() at startup. Two of these remove entire subsystems from the port:
 *    invariant globalization deletes the ICU dependency outright, and disabling
 *    concurrent GC deletes a thread that would otherwise need suspending.
 *
 * 2. The pthread_kill watch. This is the instrument for the project's go/no-go
 *    question -- see the roadmap, section 7. NativeAOT suspends managed threads
 *    for GC by sending SIGRTMIN via pthread_kill and reading the target's
 *    register context in the handler. Horizon delivers no signals, so if that
 *    call ever fires we cannot honour it.
 *
 *    Do NOT "solve" this the way the IL2CPP ports solve the analogous Boehm
 *    problem. Boehm's stop-the-world is acknowledgement-driven, so posting the
 *    ack the undelivered handler would have posted works: Boehm is conservative
 *    and non-moving, and a thread that keeps running can at worst cause a
 *    missed reference. .NET's GC COMPACTS. A thread running through compaction
 *    holds stale pointers and you get heap corruption that surfaces later as
 *    unrelated crashes. There is also nothing to fake: PalHijack needs the
 *    target's registers, and no semaphore stands in for that.
 *
 *    The viable approach is to arrange that no managed thread is ever in
 *    cooperative mode at GC time -- threads blocked inside our shim are in a
 *    P/Invoke, which the runtime already treats as safe and does not hijack.
 *    This file does not enforce that; it MEASURES it. If the counter below
 *    stays at zero across a long session with forced collections, the
 *    invariant holds. If it moves, the log names the thread that broke it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "runtime_glue.h"
#include "threads.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* Environment                                                               */
/* ------------------------------------------------------------------------ */

typedef struct { const char *k, *v; } EnvPair;

static EnvPair g_env[] = {
  /* Removes the ICU dependency. Without this the runtime tries to dlopen
   * libicuuc.so / libicui18n.so and fails initialization. Costs correct
   * culture-aware string comparison, which a game does not need. */
  /* "1" rather than "true". The reader has a single-character fast path that
   * returns without going near bool.TryParse; the string form takes the parse
   * route, which does its own ordinal comparison. Both were tried and both
   * failed, so this is not the problem -- but the fast path is the simpler
   * thing to have in place while looking elsewhere. */
  { "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1" },
  /* There is no timezone database on this system.
   *
   * The crash after the thread fix was memcpy(NULL, src, 0x100000) -- a 1 MB
   * copy into a null destination -- reached through a P/Invoke from managed
   * code, immediately after the runtime called ucal_getWindowsTimeZoneID,
   * probed this variable and found it unset, and asked for the
   * persist.sys.timezone property. That is TimeZoneInfo starting up: on
   * Android it reads the tzdata blob, and there is no such file here, so the
   * buffer it copies into is null.
   *
   * Setting this makes the runtime skip the database entirely and use UTC,
   * which is the honest answer for a console with no timezone configuration
   * exposed to us. It is the runtime's own documented switch, read at run
   * time -- unlike GLOBALIZATION_INVARIANT above, which NativeAOT bakes in at
   * compile time and therefore ignores here. */
  { "DOTNET_SYSTEM_TIMEZONE_INVARIANT", "1" },

  /* APPLOCALICU is deliberately ABSENT.
   *
   * Setting it to "" was a mistake on my part: an empty string is not the same
   * as unset, and a non-null value sends LoadICU down the app-local branch
   * instead of the native one. Leaving it out lets the runtime take its normal
   * path. Do not "helpfully" add it back with an empty value. */

  { "DOTNET_SYSTEM_GLOBALIZATION_PREDEFINED_CULTURES_ONLY", "1" },

  /* No background GC thread -- one fewer thread in the suspension problem. */
  { "DOTNET_gcConcurrent", "0" },
  /* Workstation GC: single heap, no server GC thread pool. */
  { "DOTNET_gcServer",     "0" },

  /* Cap the heap. Left alone, the GC sizes itself from sysinfo/getrlimit,
   * which our shim answers with invented numbers. Better to be explicit.
   * 768 MB. */
  { "DOTNET_GCHeapHardLimit", "30000000" },   /* hex: 0x30000000 = 768 MB */

  /* With regions (the default since .NET 7) the GC reserves a contiguous
   * address range up front, independently of the hard limit. On a real OS
   * that reservation is free; our arena is backed by real memory, so it is
   * not. Pin it rather than letting the GC infer a size from how much RAM it
   * thinks the machine has. Hex, 1 GB. */
  { "DOTNET_GCRegionRange", "40000000" },

  /* Raised from 8 MB now that the question it was set small to answer has
   * been answered: the hijack count stayed at zero, so the GC never needs to
   * suspend a thread in cooperative mode on this platform.
   *
   * What a small gen0 buys now is collections during startup, and one of those
   * finalized a freshly built managed peer and threw on the finalizer thread.
   * This does not fix that -- a peer collected later will fail the same way --
   * but it stops provoking it while the underlying reference problem is still
   * being pinned down. Hex, 64 MB. */
  { "DOTNET_GCgen0size", "4000000" },

  /* Enabled deliberately, having watched the alternative fail.
   *
   * With this off, the runtime expects one address it can write to and execute
   * from -- it installed thunk stubs, ran them, then wrote more into the same
   * pages. Horizon grants a range RW or RX, never both, so that pattern faults
   * however carefully the permissions are juggled underneath it.
   *
   * With it on, the runtime keeps the write and execute views apart itself,
   * which is the model this platform can actually satisfy. If it instead
   * starts asking for two mappings of the same memory, that is memfd-backed
   * double mapping and libnx jit_t is what would back it. */
  { "DOTNET_EnableWriteXorExecute", "1" },

  /* Android host expects these; harmless if unread. */
  { "HOME", "/switch/pvzultimate" },
  { "TMPDIR", "/switch/pvzultimate/tmp" },
};

/* Every query is logged, hit or miss.
 *
 * The runtime asked for ICU even with DOTNET_SYSTEM_GLOBALIZATION_INVARIANT
 * set, and there are only two explanations: either it never asks for that
 * name, or something answers before the environment is consulted (an
 * AppContext feature switch baked in at build time wins over the env var, and
 * a .NET for Android app that ships ICU will have set exactly that).
 *
 * Guessing between those costs a build cycle either way, so log the queries
 * and let the next run say which it is. If the name never appears here, the
 * switch is baked in and no environment change can help -- ICU has to be
 * provided. If it does appear and we answer "1", the problem is elsewhere. */
static int g_env_queries;

const char *getenv_fake(const char *name) {
  if (!name) return NULL;

  for (size_t i = 0; i < sizeof(g_env)/sizeof(g_env[0]); i++) {
    if (!strcmp(name, g_env[i].k)) {
      if (g_env_queries < 120) {
        g_env_queries++;
        debug_log("[env] %s -> \"%s\"\n", name, g_env[i].v);
      }
      return g_env[i].v;
    }
  }

  /* The runtime accepts either prefix for its configuration knobs and tries
   * COMPlus_ first in some paths. Answer from the same table rather than
   * duplicating every entry. */
  if (!strncmp(name, "COMPlus_", 8)) {
    char alt[128];
    snprintf(alt, sizeof(alt), "DOTNET_%s", name + 8);
    for (size_t i = 0; i < sizeof(g_env)/sizeof(g_env[0]); i++)
      if (!strcmp(alt, g_env[i].k)) {
        debug_log("[env] %s -> \"%s\" (via %s)\n", name, g_env[i].v, alt);
        return g_env[i].v;
      }
  }

  if (g_env_queries < 120) {
    g_env_queries++;
    debug_log("[env] %s -> (unset)\n", name);
  }
  return NULL;
}

int setenv_fake(const char *name, const char *value, int overwrite) {
  (void)overwrite;
  debug_log("[env] setenv(%s = %s) ignored -- table is fixed\n",
            name ? name : "?", value ? value : "?");
  return 0;
}

void runtime_env_dump(void) {
  debug_log("[env] runtime configuration:\n");
  for (size_t i = 0; i < sizeof(g_env)/sizeof(g_env[0]); i++)
    debug_log("[env]   %s = %s\n", g_env[i].k, g_env[i].v);
}

/* ------------------------------------------------------------------------ */
/* GC suspension watch                                                       */
/* ------------------------------------------------------------------------ */

static unsigned g_hijack_attempts;
static unsigned g_other_signals;

/* bionic's __libc_current_sigrtmin. The value does not matter much -- what
 * matters is that whatever the runtime asks for here is the same number we
 * compare against in pthread_kill_fake. */
int __libc_current_sigrtmin_fake(void) { return 34; }
int __libc_current_sigrtmax_fake(void) { return 64; }

int pthread_kill_fake(void *thread, int sig) {
  if (sig == __libc_current_sigrtmin_fake()) {
    g_hijack_attempts++;
    if (g_hijack_attempts <= 8 || (g_hijack_attempts % 256) == 0) {
      debug_log("[gc] *** PalHijack attempt #%u on thread '%s' (%p) ***\n",
                g_hijack_attempts, threads_name_of(thread), thread);
      debug_log("[gc]     That thread was in cooperative mode at GC time, so the\n"
                "[gc]     invariant Approach A depends on does not hold. The name\n"
                "[gc]     above tells you what to chase: a MonoGame worker can\n"
                "[gc]     usually be made to block in native code, the finalizer\n"
                "[gc]     thread cannot.\n");
    }
    /* Deliberately do nothing else. Returning 0 lets the runtime proceed as if
     * the signal were queued. If the invariant holds this never runs; if it
     * does run, faking success is no worse than faking an ack and it keeps the
     * failure visible rather than papering over it. */
    return 0;
  }

  g_other_signals++;
  return 0;
}

void gc_watch_report(void) {
  if (g_hijack_attempts == 0) {
    debug_log("[gc] hijack attempts: 0 -- cooperative-mode invariant HELD "
              "(%u other signals ignored)\n", g_other_signals);
  } else {
    debug_log("[gc] hijack attempts: %u -- invariant BROKEN. "
              "Suspension must be emulated (roadmap section 7, Approach B).\n",
              g_hijack_attempts);
  }
}

unsigned gc_watch_hijack_count(void) { return g_hijack_attempts; }

/* ------------------------------------------------------------------------ */
/* Signals: everything else                                                  */
/* ------------------------------------------------------------------------ */

/* Record-and-succeed. The runtime installs SIGSEGV/SIGBUS handlers to turn
 * hardware faults into NullReferenceException. We cannot deliver those, so a
 * managed null dereference becomes a hard fault instead of a catchable
 * exception. That is an acceptable trade for a shipping game, which should not
 * be using NREs for control flow -- and nx_exception_dump.c will at least tell
 * you where it happened. */
int sigaction_fake(int sig, const void *act, void *oldact) {
  (void)act; (void)oldact;
  static int logged[64];
  if (sig >= 0 && sig < 64 && !logged[sig]) {
    logged[sig] = 1;
    debug_log("[sig] sigaction(%d) recorded, no delivery possible\n", sig);
  }
  return 0;
}

int sigemptyset_fake(void *set)          { if (set) *(uint64_t *)set = 0; return 0; }
int sigaddset_fake(void *set, int sig)   { if (set && sig < 64) *(uint64_t *)set |= (1ull << sig); return 0; }
int pthread_sigmask_fake(int how, const void *set, void *old) {
  (void)how; (void)set; (void)old; return 0;
}
