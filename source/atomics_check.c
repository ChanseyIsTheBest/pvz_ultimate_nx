/* atomics_check.c -- do compare-and-swap loops actually complete here?
 *
 * Why this exists.
 *
 * The 45-second creport showed the main thread spinning in .NET's lock
 * acquisition path: load-acquire a state word, test a bit, attempt to take it,
 * Thread.Yield, retest, forever. No timeout and no blocking wait. A second
 * game thread ('Lawn music deco') was in the same code. Neither was blocked on
 * anything the kernel knew about -- both were running, and neither made
 * progress.
 *
 * Every explanation for that shape reduces to "the compare-and-swap at the
 * heart of the lock never succeeds". On AArch64 that operation is
 * ldaxr/stlxr, and stlxr only succeeds if the exclusive monitor is still held
 * for that address. The monitor is a property of how the memory is MAPPED:
 * it works on normal cacheable inner-shareable memory and is not guaranteed
 * anywhere else. This port maps memory in ways a normal program does not --
 * the .so is mapped and relocated by hand, the runtime's arena is built from
 * svcMapProcessCodeMemory with permissions changed underneath it, and the GC
 * heap comes from a third place again. If exclusives silently fail in any one
 * of those regions, every lock whose state lives there spins forever, exactly
 * as observed, with nothing in any log to say so.
 *
 * That is a hypothesis, not a diagnosis, and it is not one that can be settled
 * by reading code -- it depends on what the kernel did with the mapping. So
 * this measures it directly, on the hardware, in one pass at startup. It is
 * cheap, it runs before anything else can be blamed, and either result is
 * worth having: a pass eliminates the largest remaining structural suspect,
 * and a failure names the exact region to fix.
 *
 * Inline asm rather than __atomic_compare_exchange_n on purpose: the built-in
 * can compile to `bl __aarch64_cas4_acq_rel`, a libgcc out-of-line helper that
 * exists on some aarch64 targets and not others, and this file must not be
 * able to fail the link. The asm below is the same instruction pair the
 * runtime itself uses.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "atomics_check.h"
#include "mem_arena.h"
#include "util.h"

/* One CAS attempt: if *p == expect, store want. Returns 1 on success, 0 if
 * stlxr reported the monitor was lost, -1 if the value was not `expect`. */
static int cas_once(volatile uint32_t *p, uint32_t expect, uint32_t want) {
  uint32_t seen, failed;
  __asm__ __volatile__(
      "ldaxr  %w0, [%2]\n\t"
      "cmp    %w0, %w3\n\t"
      "b.ne   1f\n\t"
      "stlxr  %w1, %w4, [%2]\n\t"
      "b      2f\n"
      "1:\n\t"
      "clrex\n\t"
      "mov    %w1, #2\n"
      "2:"
      : "=&r"(seen), "=&r"(failed)
      : "r"(p), "r"(expect), "r"(want)
      : "cc", "memory");
  if (failed == 2) return -1;
  return failed == 0;
}

/* A CAS loop of the shape every lock uses. Bounded, because the whole point is
 * that the real one is not. */
static int cas_loop_completes(volatile uint32_t *p) {
  *p = 1;
  __asm__ __volatile__("dmb ish" ::: "memory");

  for (int spins = 0; spins < 100000; spins++) {
    int r = cas_once(p, 1, 2);
    if (r == 1) return *p == 2;      /* took it */
    if (r == -1) return 0;           /* value was not what we wrote */
  }
  return 0;                          /* stlxr never succeeded: no monitor here */
}

static int check_region(const char *what, volatile uint32_t *p) {
  if (!p) {
    debug_log("[atomics] %-22s SKIPPED (no memory)\n", what);
    return 1;
  }
  int ok = cas_loop_completes(p);
  debug_log("[atomics] %-22s %s  (at %p)\n", what,
            ok ? "ok" : "*** EXCLUSIVES DO NOT WORK HERE ***", (void *)p);
  return ok;
}

int atomics_check_run(void) {
  debug_log("\n=== atomics: can a compare-and-swap loop complete? ===\n");

  uint32_t on_stack = 0;
  int ok = 1;

  ok &= check_region("thread stack", &on_stack);

  uint32_t *heap = (uint32_t *)malloc(sizeof(uint32_t));
  ok &= check_region("newlib heap", heap);
  free(heap);

  /* The mapping the runtime gets its own memory from. Tested through
   * mmap_fake rather than by poking the mapped .so directly: a stray write
   * into the runtime's live data to answer a diagnostic question would be a
   * bad trade, and this exercises the same code path the GC heap is built on.
   */
  void *m = mmap_fake(NULL, 0x1000, 3 /* RW */, 0x22 /* PRIVATE|ANON */, -1, 0);
  if (m && m != (void *)-1) {
    ok &= check_region("runtime mmap (GC heap)", (volatile uint32_t *)m);
    munmap_fake(m, 0x1000);
  } else {
    debug_log("[atomics] runtime mmap        SKIPPED (allocation failed)\n");
  }

  /* The donation arena, which is where executable and long-lived runtime
   * allocations come from. */
  void *dn = donation_alloc(0x1000);
  ok &= check_region("donation arena", (volatile uint32_t *)dn);

  if (ok) {
    debug_log("[atomics] all regions fine -- a lock that never completes is "
              "NOT the memory mapping, so look at what the game is waiting "
              "for instead.\n");
  } else {
    debug_log("[atomics] *** at least one region cannot complete a CAS. Every "
              "lock whose state lives there will spin forever, which is "
              "exactly the hang seen in the creport. This is the bug. ***\n");
  }
  debug_log("\n");
  return ok;
}
