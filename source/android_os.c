/* android_os.c -- the message loop and surface plumbing the managed peer uses.
 *
 * With peer construction working, the managed LawnSurfaceView runs its real
 * Android setup: it builds a HandlerThread, starts it, takes its Looper, wraps
 * that in a Handler, and asks itself for its SurfaceHolder. Every one of those
 * was a stub returning null, and the null holder was dereferenced immediately.
 *
 * Two decisions worth stating.
 *
 * There is one Looper and it is not a thread. Android's HandlerThread exists to
 * own a message queue on a background thread; here the frame loop already is
 * that thread, so start() creates nothing and getLooper() returns the same
 * singleton every time. What matters to the caller is that it receives an
 * object and that work posted to it eventually runs.
 *
 * Posted Runnables are queued rather than run inline. Running one from inside
 * post() would execute it on whichever thread happened to call, re-entering
 * managed code from an arbitrary point -- and Android's contract is that they
 * run on the looper's thread. Draining from the frame loop honours that and
 * keeps the ordering the caller expects.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "android_os.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "lawn_natives.h"
#include "threads.h"
#include "util.h"
#include "watchdog.h"

#define JV jvalue r; memset(&r, 0, sizeof(r))

static jvalue hthread_true(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.z = JNI_TRUE; return r;
}

static jvalue nop(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV; return r;
}

/* ------------------------------------------------------------------------ */
/* Posted work                                                               */
/* ------------------------------------------------------------------------ */

#define MAX_POSTED 128

typedef struct { jobject runnable; u64 due_ns; } Posted;

static Posted g_posted[MAX_POSTED];
static int    g_nposted;
static Mutex  g_post_lock;

static void post_runnable(jobject runnable, u64 delay_ms) {
  if (!runnable) return;
  mutexLock(&g_post_lock);
  if (g_nposted < MAX_POSTED) {
    /* A global ref: the caller's local one dies when its frame does, and this
     * outlives that by design. */
    g_posted[g_nposted].runnable = jniref_new(jniref_deref(runnable), REF_GLOBAL);
    g_posted[g_nposted].due_ns =
        armTicksToNs(armGetSystemTick()) + delay_ms * 1000000ull;
    g_nposted++;
  } else {
    debug_log("[os] posted-work queue full; dropping a Runnable\n");
  }
  mutexUnlock(&g_post_lock);
}

/* The thread that drains the queue -- Android's "UI thread". Recorded rather
 * than assumed, because the answer decides whether runOnUiThread may run its
 * Runnable inline or must queue it. */
static void *g_looper_thread;
static int   g_looper_known;

void android_os_mark_looper_thread(void) {
  g_looper_thread = pthread_self_fake();
  g_looper_known = 1;
}

int android_os_on_looper_thread(void) {
  return g_looper_known && pthread_self_fake() == g_looper_thread;
}

/* Invoke a Runnable's run() now, on the calling thread. */
void android_os_run_now(JNIEnv *env, jobject runnable) {
  if (!runnable) return;
  jclass c = (*env)->GetObjectClass(env, runnable);
  if (!c) return;
  jmethodID mid = (*env)->GetMethodID(env, c, "run", "()V");
  if (mid) (*env)->CallVoidMethod(env, runnable, mid);
  (*env)->ExceptionClear(env);
}

void android_os_post_runnable(jobject runnable, uint64_t delay_ms) {
  post_runnable(runnable, delay_ms);
}

void android_os_run_posted(JNIEnv *env) {
  u64 now = armTicksToNs(armGetSystemTick());
  int ran = 0;

  for (;;) {
    /* A Runnable may post another, so this loop is not obviously finite.
     * Report a drain that is clearly not converging rather than spinning in
     * it silently. */
    if (ran == 512)
      debug_log("[os] 512 Runnables in a single drain -- one of them is "
                "probably re-posting itself\n");

    jobject due = NULL;

    mutexLock(&g_post_lock);
    for (int i = 0; i < g_nposted; i++) {
      if (g_posted[i].due_ns > now) continue;
      due = g_posted[i].runnable;
      /* Remove before running: a Runnable that posts another must not see
       * this slot still occupied, and must not be re-run if it throws. */
      memmove(&g_posted[i], &g_posted[i + 1],
              (size_t)(g_nposted - i - 1) * sizeof(Posted));
      g_nposted--;
      break;
    }
    mutexUnlock(&g_post_lock);

    if (!due) break;

    /* Narrows a stall from "somewhere in the drain" to one Runnable. The
     * surface-init Runnable alone builds the whole app object, so this can be
     * a very long-running call that is not a hang.
     *
     * label, not checkpoint: a Runnable that posts another immediately-due
     * Runnable would otherwise reset the stall timer on every iteration, and a
     * livelock in this drain -- which is a real possibility, since Runnables
     * are free to post more work -- would never be reported. */
    watchdog_label("running a posted Runnable");
    ran++;

    jclass c = (*env)->GetObjectClass(env, due);
    if (c) {
      jmethodID mid = (*env)->GetMethodID(env, c, "run", "()V");
      if (mid) (*env)->CallVoidMethod(env, due, mid);
      (*env)->ExceptionClear(env);
    }
    jniref_delete(due);
  }
}

/* ------------------------------------------------------------------------ */
/* android/os/Looper                                                         */
/* ------------------------------------------------------------------------ */

static FakeObject g_looper_obj;
static FakeClass  g_class_Looper;
static jobject    g_ref_looper;

static jvalue looper_get(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.l = g_ref_looper; return r;
}

static FakeMethod g_looper_methods[] = {
  { looper_get, "getMainLooper", "()Landroid/os/Looper;", NULL, 1 },
  { looper_get, "myLooper",      "()Landroid/os/Looper;", NULL, 1 },
  { nop,        "prepare",       "()V",                   NULL, 1 },
  { nop,        "loop",          "()V",                   NULL, 1 },
  { nop,        "quit",          "()V",                   NULL, 0 },
  { nop,        "quitSafely",    "()V",                   NULL, 0 },
};

static FakeClass g_class_Looper = {
  {NULL}, "android/os/Looper", NULL,
  g_looper_methods, (int)(sizeof(g_looper_methods)/sizeof(g_looper_methods[0])),
  NULL, 0, 0
};

/* ------------------------------------------------------------------------ */
/* android/os/HandlerThread                                                  */
/* ------------------------------------------------------------------------ */

/* start() deliberately creates no thread. The frame loop is the looper thread,
 * and spawning a real one would give posted work a second place to run. */
static jvalue ht_getName(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.l = jni_make_string("LawnHandlerThread"); return r;
}

static FakeMethod g_handlerthread_methods[] = {
  { nop,        "<init>",     "(Ljava/lang/String;)V",  NULL, 0 },
  { nop,        "<init>",     "(Ljava/lang/String;I)V", NULL, 0 },
  { nop,        "start",      "()V",                    NULL, 0 },
  { nop,        "run",        "()V",                    NULL, 0 },
  { looper_get, "getLooper",  "()Landroid/os/Looper;",  NULL, 0 },
  /* true: the thread is asked to stop and that request is honoured -- the
   * frame loop IS the looper, so there is nothing to keep running. false
   * means "the quit was refused", which is not what happens here. */
  { hthread_true, "quit",       "()Z",                  NULL, 0 },
  { hthread_true, "quitSafely", "()Z",                  NULL, 0 },
  { nop,        "setName",    "(Ljava/lang/String;)V",  NULL, 0 },
  { ht_getName, "getName",    "()Ljava/lang/String;",   NULL, 0 },
  { nop,        "join",       "()V",                    NULL, 0 },
  { nop,        "interrupt",  "()V",                    NULL, 0 },
};

static FakeClass g_class_HandlerThread = {
  {NULL}, "android/os/HandlerThread", NULL,
  g_handlerthread_methods,
  (int)(sizeof(g_handlerthread_methods)/sizeof(g_handlerthread_methods[0])),
  NULL, 0, 0
};

/* ------------------------------------------------------------------------ */
/* android/os/Handler                                                        */
/* ------------------------------------------------------------------------ */

static jvalue handler_post(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV;
  if (a) post_runnable(a[0].l, 0);
  r.z = JNI_TRUE;
  return r;
}

static jvalue handler_postDelayed(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV;
  if (a) post_runnable(a[0].l, (u64)a[1].j);
  r.z = JNI_TRUE;
  return r;
}

/* Removing work that has already run is normal and not an error. */
static jvalue handler_removeCallbacks(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV;
  if (!a || !a[0].l) return r;

  FakeObject *target = jniref_deref(a[0].l);
  mutexLock(&g_post_lock);
  for (int i = 0; i < g_nposted; ) {
    if (jniref_deref(g_posted[i].runnable) == target) {
      jniref_delete(g_posted[i].runnable);
      memmove(&g_posted[i], &g_posted[i + 1],
              (size_t)(g_nposted - i - 1) * sizeof(Posted));
      g_nposted--;
    } else i++;
  }
  mutexUnlock(&g_post_lock);
  return r;
}

static jvalue handler_true(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.z = JNI_TRUE; return r;
}

static FakeMethod g_handler_methods[] = {
  { nop,                     "<init>",             "()V",                          NULL, 0 },
  { nop,                     "<init>",             "(Landroid/os/Looper;)V",       NULL, 0 },
  { nop,                     "<init>",             "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V", NULL, 0 },
  { handler_post,            "post",               "(Ljava/lang/Runnable;)Z",      NULL, 0 },
  { handler_postDelayed,     "postDelayed",        "(Ljava/lang/Runnable;J)Z",     NULL, 0 },
  { handler_postDelayed,     "postAtTime",         "(Ljava/lang/Runnable;J)Z",     NULL, 0 },
  { handler_removeCallbacks, "removeCallbacks",    "(Ljava/lang/Runnable;)V",      NULL, 0 },
  { handler_true,            "sendEmptyMessage",   "(I)Z",                         NULL, 0 },
  { handler_true,            "sendEmptyMessageDelayed", "(IJ)Z",                   NULL, 0 },
  { looper_get,              "getLooper",          "()Landroid/os/Looper;",        NULL, 0 },
  { nop,                     "removeCallbacksAndMessages", "(Ljava/lang/Object;)V",NULL, 0 },
};

static FakeClass g_class_Handler = {
  {NULL}, "android/os/Handler", NULL,
  g_handler_methods, (int)(sizeof(g_handler_methods)/sizeof(g_handler_methods[0])),
  NULL, 0, 0
};

/* ------------------------------------------------------------------------ */
/* android/view/SurfaceHolder                                                */
/* ------------------------------------------------------------------------ */

static FakeObject g_holder_obj;
static FakeClass  g_class_SurfaceHolder;
static jobject    g_ref_holder;

/* The one real answer here: the Surface the frame loop already draws into. */
static jvalue holder_getSurface(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.l = lawn_surface_instance(); return r;
}

/* ------------------------------------------------------------------------ */
/* android/view/Choreographer                                                */
/* ------------------------------------------------------------------------ */
/*
 * Reached for the first time now that the render gate opens. Start() sets the
 * "running" flag and then immediately does:
 *
 *     bl   Choreographer.getInstance()      ; +0x1ba0cc0
 *     ldr  wzr, [x0]                        ; null check -- died here
 *     b    postFrameCallback(this)          ; +0x1ba0d30
 *
 * It registers the view as a frame callback and expects a call back once per
 * vsync. Android does not need to provide that here: the host frame loop
 * already calls n_doFrame every iteration, which IS the callback the
 * Choreographer would invoke. The registration only has to be accepted; the
 * delivery already happens by another route. Scheduling anything here would
 * mean the game's doFrame ran twice per frame.
 */
static FakeObject g_choreographer_obj;

static jvalue chor_getInstance(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  JV; r.l = jniref_new(&g_choreographer_obj, REF_LOCAL); return r;
}

static jvalue chor_postFrameCallback(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  static int once;
  if (!once) {
    once = 1;
    debug_log("[chor] postFrameCallback(%p) accepted -- n_doFrame is already "
              "driven by the host frame loop, so it is delivered that way "
              "rather than scheduled here.\n", (void *)(a ? a[0].l : NULL));
  }
  JV; return r;
}

static FakeMethod g_choreographer_methods[] = {
  { chor_getInstance,       "getInstance", "()Landroid/view/Choreographer;", NULL, 1 },
  { chor_postFrameCallback, "postFrameCallback",
    "(Landroid/view/Choreographer$FrameCallback;)V", NULL, 0 },
  { chor_postFrameCallback, "postFrameCallbackDelayed",
    "(Landroid/view/Choreographer$FrameCallback;J)V", NULL, 0 },
  { chor_postFrameCallback, "removeFrameCallback",
    "(Landroid/view/Choreographer$FrameCallback;)V", NULL, 0 },
};

static FakeClass g_class_Choreographer = {
  {NULL}, "android/view/Choreographer", NULL,
  g_choreographer_methods,
  (int)(sizeof(g_choreographer_methods)/sizeof(g_choreographer_methods[0])),
  NULL, 0, 0
};

/* android/graphics/Rect, because getSurfaceFrame() is declared to return one.
 *
 * Returning null from a method whose descriptor ends in an object type is the
 * single most reliable way to kill this port: managed code tests a reference
 * by dereferencing it, and we cannot turn that fault into a
 * NullReferenceException. The frame is cheap to model honestly -- it is the
 * window, which is exactly what the surface covers -- so there is no reason to
 * leave a null here waiting for the first caller that asks. */
typedef struct {
  FakeObject hdr;
  jint left, top, right, bottom;
} FakeRect;

static FakeRect  g_rect_obj;
static jobject   g_ref_rect;

static jvalue rect_get_left(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV; r.i = 0; return r;
}
static jvalue rect_get_top(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV; r.i = 0; return r;
}
static jvalue rect_get_right(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV; r.i = nx_ANativeWindow_getWidth(NULL); return r;
}
static jvalue rect_get_bottom(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV; r.i = nx_ANativeWindow_getHeight(NULL); return r;
}

static jvalue rect_width(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV; r.i = nx_ANativeWindow_getWidth(NULL);
  return r;
}
static jvalue rect_height(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV; r.i = nx_ANativeWindow_getHeight(NULL);
  return r;
}

static FakeMethod g_rect_methods[] = {
  { rect_width,  "width",  "()I", NULL, 0 },
  { rect_height, "height", "()I", NULL, 0 },
};

/* Public instance fields, which is how Android.Graphics.Rect reads them. */
static FakeField g_rect_fields[] = {
  { "left",   "I", rect_get_left,   NULL, {0} },
  { "top",    "I", rect_get_top,    NULL, {0} },
  { "right",  "I", rect_get_right,  NULL, {0} },
  { "bottom", "I", rect_get_bottom, NULL, {0} },
};

static FakeClass g_class_Rect = {
  {NULL}, "android/graphics/Rect", NULL,
  g_rect_methods, (int)(sizeof(g_rect_methods)/sizeof(g_rect_methods[0])),
  g_rect_fields,  (int)(sizeof(g_rect_fields)/sizeof(g_rect_fields[0])),
  sizeof(FakeRect)
};

static jvalue holder_getSurfaceFrame(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.l = g_ref_rect; return r;
}

/* The managed side registers itself here to receive surfaceCreated and
 * surfaceChanged. We deliver those directly from the host instead, so
 * recording the callback is enough -- but note it, because a caller that
 * never receives them will look like a game that never starts drawing. */
static jvalue holder_addCallback(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV;
  static int noted;
  if (!noted && a && a[0].l) {
    noted = 1;
    debug_log("[os] SurfaceHolder.addCallback registered; the host delivers "
              "surfaceCreated/Changed directly rather than through it\n");
  }
  return r;
}

static FakeMethod g_holder_methods[] = {
  { holder_addCallback,    "addCallback",         "(Landroid/view/SurfaceHolder$Callback;)V", NULL, 0 },
  { nop,                   "removeCallback",      "(Landroid/view/SurfaceHolder$Callback;)V", NULL, 0 },
  { holder_getSurface,     "getSurface",          "()Landroid/view/Surface;", NULL, 0 },
  { holder_getSurfaceFrame,"getSurfaceFrame",     "()Landroid/graphics/Rect;", NULL, 0 },
  { nop,                   "setFormat",           "(I)V",  NULL, 0 },
  { nop,                   "setType",             "(I)V",  NULL, 0 },
  { nop,                   "setFixedSize",        "(II)V", NULL, 0 },
  { nop,                   "setSizeFromLayout",   "()V",   NULL, 0 },
  { nop,                   "setKeepScreenOn",     "(Z)V",  NULL, 0 },
  /* Null is correct, not a gap: Android returns null from lockCanvas when the
   * surface is owned by GL, which ours always is. */
  { nop,                   "lockCanvas",          "()Landroid/graphics/Canvas;", NULL, 0 },
  { nop,                   "unlockCanvasAndPost", "(Landroid/graphics/Canvas;)V", NULL, 0 },
  { nop,                   "isCreating",          "()Z",   NULL, 0 },
};

static FakeClass g_class_SurfaceHolder = {
  {NULL}, "android/view/SurfaceHolder", NULL,
  g_holder_methods, (int)(sizeof(g_holder_methods)/sizeof(g_holder_methods[0])),
  NULL, 0, 0
};

jobject android_os_surfaceholder(void) { return g_ref_holder; }

/* ------------------------------------------------------------------------ */

void android_os_init(void) {
  mutexInit(&g_post_lock);

  jni_register_class(&g_class_Looper);
  jni_register_class(&g_class_HandlerThread);
  jni_register_class(&g_class_Handler);
  jni_register_class(&g_class_SurfaceHolder);
  jni_register_class(&g_class_Rect);
  jni_register_class(&g_class_Choreographer);

  g_looper_obj.cls   = &g_class_Looper;
  g_holder_obj.cls   = &g_class_SurfaceHolder;
  g_rect_obj.hdr.cls = &g_class_Rect;
  g_choreographer_obj.cls = &g_class_Choreographer;

  g_ref_looper = jniref_new(&g_looper_obj, REF_GLOBAL);
  g_ref_holder = jniref_new(&g_holder_obj, REF_GLOBAL);
  g_ref_rect   = jniref_new(&g_rect_obj.hdr, REF_GLOBAL);

  /* Host-owned, like the other singletons: a managed finalizer must not be
   * able to release something the frame loop still calls into. */
  jniref_pin(g_ref_looper);
  jniref_pin(g_ref_holder);
  jniref_pin(g_ref_rect);

  debug_log("[os] looper, handler and surface holder registered\n");
}
