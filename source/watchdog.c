/* watchdog.c -- stall detection.
 *
 * A hang gives you nothing. No crash report, no log tail, no indication of
 * which of twenty subsystems stopped. On this port the likely hang sites are
 * all places where you cannot easily attach a debugger: inside the .NET
 * runtime's startup, inside a GC that is waiting for a thread suspension that
 * will never arrive, or inside a JNI call that dispatched into the image and
 * did not come back.
 *
 * So this does two things.
 *
 * First, it names WHERE. Each bootstrap stage calls watchdog_checkpoint() with
 * a label, and the frame loop calls watchdog_beat(). If the clock runs past
 * the warning threshold with no beat, the label tells you which stage stopped
 * -- which is most of the diagnosis.
 *
 * Second, and more usefully: past a longer threshold it calls svcBreak
 * deliberately. That converts a silent hang into an Atmosphere crash report,
 * and creport dumps EVERY thread's registers and stack trace, not just one.
 * For a GC suspension deadlock -- the failure this port is most likely to hit
 * -- that report is the entire answer: you get the stack of the thread holding
 * things up and the stack of the collector waiting on it, side by side.
 *
 * The watchdog thread is created with libnx directly rather than through our
 * pthread shim, deliberately. It must not appear in the thread registry that
 * runtime_glue's GC watch inspects, and it must never itself become a
 * suspension target -- a watchdog that can be stopped by the thing it is
 * watching is not a watchdog.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "aaudio_shim.h"
#include "mem_arena.h"
#include "runtime_glue.h"
#include "threads.h"
#include "util.h"
#include "watchdog.h"

static Thread    g_thread;
static bool      g_running;
static u64       g_last_beat_ns;
static char      g_label[96] = "startup";
static u64       g_warn_ns;
static u64       g_break_ns;
static unsigned  g_warnings;
static bool      g_armed = true;
static bool      g_fatal = false;

static u64 now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* ------------------------------------------------------------------------ */

void watchdog_checkpoint(const char *what) {
  if (what) snprintf(g_label, sizeof(g_label), "%s", what);
  g_last_beat_ns = now_ns();
  g_warnings = 0;
}

void watchdog_label(const char *what) {
  if (what) snprintf(g_label, sizeof(g_label), "%s", what);
}

void watchdog_beat(void) {
  g_last_beat_ns = now_ns();
  g_warnings = 0;
}

void watchdog_disarm(void) {
  g_armed = false;
  debug_log("[wd] disarmed\n");
}

void watchdog_rearm(void) {
  g_armed = true;
  watchdog_beat();
}

/* ------------------------------------------------------------------------ */

void watchdog_note_fatal(void) { g_fatal = true; }

/* Set on every JNI call, PER THREAD.
 *
 * The first version was a single global pair plus one counter, which was wrong
 * on both counts once the audio callback thread started making JNI calls of
 * its own. Two threads overwrote each other's method name, so the report named
 * one thread's call while the other was the one stuck; and `depth++` on a
 * plain int is not atomic, so concurrent increments and decrements lose
 * updates and the depth drifts -- it can read 1 with nothing outstanding, or 0
 * with a thread stuck inside a call. Both failures point the reader at the
 * wrong thread, which is worse than no breadcrumb at all.
 *
 * A small table keyed by thread handle. The claim is a plain store rather than
 * a compare-and-swap on purpose: __atomic_compare_exchange_n compiled to
 * `bl __aarch64_cas8_acq_rel`, GCC's out-of-line atomic helper, which is a
 * libgcc symbol that exists on some aarch64 targets and not others. A
 * diagnostic must not be able to fail the link. The race is confined to the
 * instant a thread first claims a slot, and the worst outcome is two threads
 * sharing one -- their breadcrumbs mix, which is a slightly degraded report,
 * not corruption. Every field is a pointer or an int written only by its
 * owner. */
#define CALL_SLOTS 16
#define CALL_NEST   24

typedef struct {
  void *volatile owner;   /* pthread handle; NULL is the main thread */
  volatile int   claimed; /* NULL is a legal owner, so free-ness needs its own flag */
  int            depth;   /* touched only by the owning thread */
  const char    *cls[CALL_NEST];
  const char    *method[CALL_NEST];
} CallSlot;

static CallSlot g_calls[CALL_SLOTS];

static CallSlot *slot_for_current(void) {
  void *self = pthread_self_fake();
  for (int i = 0; i < CALL_SLOTS; i++) {
    if (g_calls[i].claimed) {
      if (g_calls[i].owner == self) return &g_calls[i];
      continue;
    }
    /* Free slot. owner is written before claimed so another thread reading
     * claimed as set always sees a valid owner. */
    g_calls[i].owner = self;
    g_calls[i].claimed = 1;
    return &g_calls[i];
  }
  return NULL;              /* more threads than slots; drop the breadcrumb */
}

void watchdog_note_call(const char *cls, const char *method) {
  CallSlot *s = slot_for_current();
  if (!s) return;
  if (s->depth >= 0 && s->depth < CALL_NEST) {
    s->cls[s->depth] = cls;
    s->method[s->depth] = method;
  }
  s->depth++;
}

void watchdog_note_return(void) {
  CallSlot *s = slot_for_current();
  if (s && s->depth > 0) s->depth--;
}

/* The whole nest, outermost first.
 *
 * Recording only the most recent call was not enough, and the way it failed is
 * worth keeping in mind: JNI calls nest, an inner call overwrites the name, and
 * on return the outer call is still running under the inner call's name. A
 * report naming `System.identityHashCode` -- three lines of pointer arithmetic
 * that cannot block -- was really a much larger outer call that had already had
 * its name overwritten by an inner one that completed. The outermost frame is
 * the one to read. */
void watchdog_report_calls(void) {
  int any = 0;
  for (int i = 0; i < CALL_SLOTS; i++) {
    if (!g_calls[i].claimed) continue;
    const char *tn = threads_name_of(g_calls[i].owner);
    int d = g_calls[i].depth;
    if (d <= 0) {
      debug_log("  %-16s no JNI call outstanding\n", tn ? tn : "?");
      any = 1;
      continue;
    }
    debug_log("  %-16s inside %d nested JNI call(s), outermost first:\n",
              tn ? tn : "?", d);
    int shown = d < CALL_NEST ? d : CALL_NEST;
    for (int k = 0; k < shown; k++)
      debug_log("      [%d] %s.%s%s\n", k,
                g_calls[i].cls[k] ? g_calls[i].cls[k] : "?",
                g_calls[i].method[k] ? g_calls[i].method[k] : "?",
                k == shown - 1 ? "   <- innermost, still running" : "");
    if (d > CALL_NEST) debug_log("      ... %d deeper, not recorded\n", d - CALL_NEST);
    any = 1;
  }
  if (!any) debug_log("  no JNI calls recorded yet\n");
}

static void report_stall(u64 stalled_ns) {
  /* A deliberate failure is not a hang. fatal_error parks its thread forever,
   * so silence follows -- but the reason was already printed, and burying it
   * under a thread dump every few seconds and then a break for creport turns a
   * clean diagnosis into something that looks like a crash. */
  if (g_fatal) {
    debug_log("\n[wd] execution stopped by a deliberate failure; see the FATAL "
              "message above for the reason. This is not a hang.\n");
    return;
  }

  debug_log("\n"
            "================================================================\n"
            "  STALL: no progress for %llu s\n"
            "  last checkpoint: %s\n"
            "================================================================\n",
            (unsigned long long)(stalled_ns / 1000000000ull), g_label);

  /* Per thread, so the reader can tell which one is stuck rather than which
   * one logged last. depth > 0 means that thread entered a call and never came
   * back, which points at the hang far more directly than the phase does. */
  watchdog_report_calls();

  /* The mixer callback is managed code called directly rather than through
   * JNI, so it is invisible to the breadcrumb above. */
  unsigned long long ms = 0;
  if (aaudio_shim_in_callback(&ms))
    debug_log("  audio: INSIDE the mixer callback, %llu ms so far\n", ms);

  /* Everything we can cheaply say about the state of the world. The thread
   * roster matters most: a stall with an unexpected thread in it usually means
   * the runtime spawned a worker we did not anticipate. */
  threads_report();
  arena_report();
  gc_watch_report();

  debug_log("[wd] if this is a GC deadlock, the hijack count above is the "
            "thing to read: non-zero means a managed thread was in "
            "cooperative mode and the collector is waiting for it.\n");
}

static void watchdog_main(void *arg) {
  (void)arg;
  while (g_running) {
    svcSleepThread(1000000000ull);          /* poll once a second */
    if (!g_armed) continue;

    u64 stalled = now_ns() - g_last_beat_ns;

    if (stalled > g_break_ns) {
      report_stall(stalled);
      debug_log("[wd] breaking deliberately so Atmosphere's creport runs.\n"
                "[wd] The report will contain a stack trace for EVERY thread; "
                "that is the point of doing this rather than hanging.\n");
      log_shutdown();
      svcBreak(BreakReason_Panic, 0, 0);
    }

    if (stalled > g_warn_ns) {
      /* Warn repeatedly but with decreasing frequency, so a long legitimate
       * load does not fill the log while a genuine hang still gets noticed. */
      unsigned due = 1u << (g_warnings > 4 ? 4 : g_warnings);
      if ((stalled / g_warn_ns) >= due) {
        g_warnings++;
        report_stall(stalled);
      }
    }
  }
}

/* ------------------------------------------------------------------------ */

void watchdog_init(unsigned warn_seconds, unsigned break_seconds) {
  g_warn_ns  = (u64)warn_seconds  * 1000000000ull;
  g_break_ns = (u64)break_seconds * 1000000000ull;
  g_last_beat_ns = now_ns();
  g_running = true;

  /* Priority 0x28 -- ABOVE the 0x2C the game's threads get, and above main.
   *
   * It was 0x2F, chosen so the watchdog "never competes with the game for
   * CPU". That reasoning holds on a cooperative scheduler and not on this one.
   * Horizon is strictly priority-ordered: a thread at 0x2F does not run at all
   * while anything at 0x2C is runnable. So the one situation the watchdog
   * exists for -- a game thread spinning and refusing to yield -- was exactly
   * the situation in which it was guaranteed not to fire. A run ended with the
   * log simply stopping: no stall banner, no break, no creport, nothing to
   * read.
   *
   * The cost of running above the game is nil. This thread sleeps for a full
   * second, compares two integers, and sleeps again; it cannot meaningfully
   * take CPU from anything. The benefit is that a spin or a deadlock now
   * produces a report naming every thread instead of silence. */
  /* Tried in order. 0x28 is the one that matters; the fallbacks exist because
   * a watchdog that fails to start is strictly worse than a slow one, and a
   * priority outside what the process is permitted would be refused here
   * rather than at run time. 0x2F is last so behaviour never regresses below
   * what it was. */
  /* Must stay strictly above the main thread, which turned out to sit between
   * 0x28 and 0x2C rather than at the 0x2C that was assumed. 0x20 leaves room
   * if that ever moves again; the fallbacks only matter if a priority is
   * refused. */
  static const int prios[] = { 0x20, 0x24, 0x28, 0x2A, 0x2C, 0x2F };
  Result rc = 0;
  int    chosen = -1;
  for (size_t i = 0; i < sizeof(prios)/sizeof(prios[0]); i++) {
    rc = threadCreate(&g_thread, watchdog_main, NULL, NULL, 0x4000,
                      prios[i], -2);
    if (R_SUCCEEDED(rc)) { chosen = prios[i]; break; }
  }
  if (chosen >= 0)
    debug_log("[wd] watchdog running at priority %#x\n", chosen);
  if (chosen >= 0 && chosen != prios[0])
    debug_log("[wd] priority %#x was refused; running at %#x. A game thread "
              "spinning at 0x2C can still starve this.\n", prios[0], chosen);
  if (R_FAILED(rc)) {
    debug_log("[wd] could not create watchdog thread: %08x "
              "(continuing without stall detection)\n", rc);
    g_running = false;
    return;
  }
  rc = threadStart(&g_thread);
  if (R_FAILED(rc)) {
    debug_log("[wd] could not start watchdog thread: %08x\n", rc);
    threadClose(&g_thread);
    g_running = false;
    return;
  }

  debug_log("[wd] armed: warn at %us, break at %us\n",
            warn_seconds, break_seconds);
}

void watchdog_shutdown(void) {
  if (!g_running) return;
  g_running = false;
  threadWaitForExit(&g_thread);
  threadClose(&g_thread);
}
