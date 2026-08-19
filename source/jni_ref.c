#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "jni_ref.h"
#include "util.h"

#define MAX_LOCAL   4096
#define MAX_GLOBAL  16384
#define MAX_WEAK    8192
#define MAX_FRAMES  64

typedef struct {
  FakeObject **slots;
  int          cap;
  int          count;      /* high-water mark of used slots */
  int         *freelist;
  int          nfree;
} RefTable;

/* Global and weak refs are process-wide; LOCAL refs are per-thread.
 *
 * That is not a detail -- it is what the JNI specification says, and it is why
 * this used to be latent rather than obviously wrong. A single shared local
 * table works perfectly while everything runs on one thread, and starts
 * corrupting the moment a second thread pushes a frame: PopLocalFrame on
 * thread B walks back to a mark set by thread A and frees references A is
 * still using. The runtime has now started creating threads.
 *
 * Per-thread tables also make the handle encoding correct rather than lucky:
 * a local ref is only ever valid on its creating thread, so resolving it
 * against the caller's table is exactly right. */
static RefTable g_global, g_weak;
static Mutex    g_lock;

static __thread RefTable *tl_local;
static __thread int tl_frames[MAX_FRAMES];
static __thread int tl_depth;

/* cap is set ONLY if both allocations succeeded.
 *
 * The previous version set it unconditionally. When the freelist calloc
 * returned NULL and the slots one did not, every bounds check still passed --
 * `idx >= t->cap` was false -- so the read of t->slots[idx] worked and the
 * write to t->freelist[0] went to address zero. A partially built table must
 * fail its guards, not half of them, so cap stays at zero and the table is
 * inert rather than dangerous. */
static int table_init(RefTable *t, int cap) {
  t->cap = 0;
  t->count = 0;
  t->nfree = 0;
  t->slots    = calloc((size_t)cap, sizeof(FakeObject *));
  t->freelist = calloc((size_t)cap, sizeof(int));

  if (!t->slots || !t->freelist) {
    free(t->slots);
    free(t->freelist);
    t->slots = NULL;
    t->freelist = NULL;
    debug_log("[jniref] OUT OF MEMORY building a %d-entry table "
              "(%zu KB). newlib's heap is exhausted -- raise "
              "NEWLIB_HEAP_SIZE in mem_arena.c.\n",
              cap, (size_t)cap * (sizeof(FakeObject *) + sizeof(int)) / 1024);
    return -1;
  }
  t->cap = cap;
  return 0;
}

void jniref_init(void) {
  if (table_init(&g_global, MAX_GLOBAL) != 0 ||
      table_init(&g_weak,   MAX_WEAK)   != 0)
    fatal_error("could not build the JNI reference tables");
  debug_log("[jniref] tables: local=%d per-thread, global=%d weak=%d\n",
            MAX_LOCAL, MAX_GLOBAL, MAX_WEAK);
}

/* Allocated on first use so threads the runtime creates get one without
 * having to be registered anywhere first. */
static RefTable *local_table(void) {
  if (!tl_local) {
    tl_local = calloc(1, sizeof(RefTable));
    if (!tl_local)
      fatal_error("out of memory allocating a thread's JNI local table");
    if (table_init(tl_local, MAX_LOCAL) != 0)
      fatal_error("out of memory building a thread's JNI local table "
                  "(%d entries). See the [jniref] line above.", MAX_LOCAL);
  }
  return tl_local;
}

static RefTable *table_for(int kind) {
  switch (kind) {
    case REF_LOCAL:  return local_table();
    case REF_GLOBAL: return &g_global;
    case REF_WEAK:   return &g_weak;
    default:         return NULL;
  }
}

jobject jniref_new(FakeObject *obj, int kind) {
  if (!obj) return NULL;              /* NULL object -> NULL ref, always */
  RefTable *t = table_for(kind);
  if (!t) return NULL;

  /* Only the shared tables need the lock. A thread's own local table cannot
   * be touched by anyone else. */
  int shared = (kind != REF_LOCAL);
  if (shared) mutexLock(&g_lock);
  int idx;
  if (t->nfree > 0) {
    idx = t->freelist[--t->nfree];
  } else if (t->count < t->cap) {
    idx = t->count++;
    /* Warn on the way up rather than only failing at the top. Exhaustion is
     * currently fatal and arrives with no notice, which makes a slow leak look
     * like a sudden crash a long way from its cause. */
    if (t->count == (t->cap / 4) * 3 || t->count == (t->cap / 10) * 9) {
      if (shared) mutexUnlock(&g_lock);
      debug_log("[jniref] %s table %d%% full (%d of %d) -- if this keeps "
                "climbing, references are leaking\n",
                kind == REF_LOCAL ? "local" : kind == REF_GLOBAL ? "global" : "weak",
                (t->count * 100) / t->cap, t->count, t->cap);
      if (shared) mutexLock(&g_lock);
    }
  } else {
    if (shared) mutexUnlock(&g_lock);
    /* Exhaustion here almost always means locals are leaking inside a loop --
     * look for a missing DeleteLocalRef or an unbalanced PushLocalFrame before
     * raising the cap. */
    fatal_error("JNI ref table (kind %d) exhausted at %d entries", kind, t->cap);
  }
  t->slots[idx] = obj;
  /* One more live handle. Only meaningful for objects we allocated. */
  if (obj && ((FakeObject *)obj)->heap_owned) ((FakeObject *)obj)->refs++;
  if (shared) mutexUnlock(&g_lock);

  return (jobject)(uintptr_t)(((uintptr_t)idx << 2) | (uintptr_t)kind);
}

int jniref_kind(jobject ref) {
  if (!ref) return REF_INVALID;
  return (int)((uintptr_t)ref & 3);
}

FakeObject *jniref_deref(jobject ref) {
  if (!ref) return NULL;
  int kind = (int)((uintptr_t)ref & 3);
  int idx  = (int)((uintptr_t)ref >> 2);
  RefTable *t = table_for(kind);
  if (!t || !t->slots || idx < 0 || idx >= t->cap) {
    debug_log("[jniref] deref of invalid handle %p\n", ref);
    return NULL;
  }
  FakeObject *o = t->slots[idx];
  if (!o)
    debug_log("[jniref] handle %p refers to a slot that was already freed "
              "(kind %d, index %d) -- something released it early\n",
              ref, kind, idx);
  return o;
}

#define MAX_PINNED 32
static jobject g_pinned[MAX_PINNED];
static int     g_npinned;

void jniref_pin(jobject ref) {
  if (!ref || g_npinned >= MAX_PINNED) return;
  g_pinned[g_npinned++] = ref;
}

int jniref_is_pinned(jobject ref) {
  for (int i = 0; i < g_npinned; i++) if (g_pinned[i] == ref) return 1;
  return 0;
}

void jniref_delete(jobject ref) {
  if (jniref_is_pinned(ref)) {
    debug_log("[jniref] refusing to release pinned host reference %p\n", ref);
    return;
  }
  if (!ref) return;
  int kind = (int)((uintptr_t)ref & 3);
  int idx  = (int)((uintptr_t)ref >> 2);
  RefTable *t = table_for(kind);
  if (!t || !t->slots || !t->freelist || idx < 0 || idx >= t->cap) return;

  int shared = (kind != REF_LOCAL);
  if (shared) mutexLock(&g_lock);
  FakeObject *dying = NULL;
  if (t->slots[idx]) {
    FakeObject *o = (FakeObject *)t->slots[idx];
    t->slots[idx] = NULL;
    if (t->nfree < t->cap) t->freelist[t->nfree++] = idx;
    /* Last handle to something we allocated: hand it back. Static objects have
     * heap_owned == 0 and are never freed here. */
    if (o && o->heap_owned && --o->refs <= 0) dying = o;
  }
  if (shared) mutexUnlock(&g_lock);

  /* Freed outside the lock: jniref_free_owned touches only this object. */
  if (dying) jniref_free_owned(dying);
}

void jniref_push_frame(int capacity) {
  (void)capacity;
  RefTable *t = local_table();
  if (tl_depth < MAX_FRAMES) tl_frames[tl_depth++] = t->count;
}

jobject jniref_pop_frame(jobject result) {
  FakeObject *keep = result ? jniref_deref(result) : NULL;

  RefTable *t = local_table();
  /* Popping without a matching push is a caller bug, but silently treating it
   * as a no-op hides an unbalanced frame that would otherwise leak every local
   * it created. */
  if (tl_depth == 0)
    debug_log("[jniref] PopLocalFrame with no frame pushed -- unbalanced\n");
  if (tl_depth > 0) {
    int mark = tl_frames[--tl_depth];
    for (int i = mark; i < t->count; i++) {
      if (t->slots[i]) {
        FakeObject *o = (FakeObject *)t->slots[i];
        t->slots[i] = NULL;
        if (t->nfree < t->cap) t->freelist[t->nfree++] = i;
        /* Same accounting as jniref_delete. Dropping a whole frame releases
         * every handle in it, and a frame is the usual way a batch of arrays
         * goes away -- if this did not count down, arrays released this way
         * would still leak and the fix would only work for callers that
         * delete each local by hand. `keep` is re-referenced below, so it is
         * safe for it to reach zero here and be freed only if nothing else
         * holds it. */
        if (o && o->heap_owned && --o->refs <= 0 && o != keep)
          jniref_free_owned(o);
      }
    }
  }
  return keep ? jniref_new(keep, REF_LOCAL) : NULL;
}

void jniref_dump_stats(void) {
  RefTable *t = tl_local;
  debug_log("[jniref] this thread local %d/%d (free %d, depth %d);  "
            "global %d/%d (free %d);  weak %d/%d (free %d)\n",
            t ? t->count : 0, t ? t->cap : 0, t ? t->nfree : 0, tl_depth,
            g_global.count, g_global.cap, g_global.nfree,
            g_weak.count,   g_weak.cap,   g_weak.nfree);
}
