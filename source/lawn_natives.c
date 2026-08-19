/* lawn_natives.c -- the game-facing surface.
 *
 * Class names and method signatures here were read out of the game's
 * classes.dex. The important discovery is that this build drives itself from
 * Choreographer: LawnSurfaceView.doFrame(long) is the tick. So the host does
 * not need to find or start a main loop inside the managed image -- it calls
 * n_doFrame once per vsync and the game advances.
 *
 * The methods listed below must be DECLARED here for RegisterNatives to bind
 * them; anything the managed side registers that is not declared gets dropped
 * with a log line. If you see a DROPPED message at Stage 4, add the method to
 * the relevant table and rebuild.
 */

#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "android_classes.h"
#include "android_os.h"
#include "android_runtime.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "lawn_natives.h"
#include "lawn_register.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* MotionEvent                                                               */
/* ------------------------------------------------------------------------ */

/* Android action codes. */
#define AMOTION_DOWN          0
#define AMOTION_UP            1
#define AMOTION_MOVE          2
#define AMOTION_CANCEL        3
#define AMOTION_POINTER_DOWN  5
#define AMOTION_POINTER_UP    6

#define MAX_POINTERS 8

typedef struct {
  FakeObject hdr;
  int    action;         /* already includes the pointer index in bits 8..15 */
  int    action_index;
  int    count;
  int    ids[MAX_POINTERS];
  float  xs[MAX_POINTERS];
  float  ys[MAX_POINTERS];
  int64_t event_time_ms;
  int64_t down_time_ms;
} FakeMotionEvent;

static FakeMotionEvent g_motion;      /* one reused instance; the game reads it
                                       * synchronously inside onTouchEvent   */

/* Whether the game ever READS the event.
 *
 * This was the last invisible link. jni_fake logs a method call only when the
 * method is NOT in our table (auto_method); implemented accessors dispatch
 * silently. So "no MotionEvent lines in the log" was consistent with two
 * opposite situations -- the game reading every field perfectly, and the game
 * never touching the object at all -- and there was no way to tell them apart.
 *
 * A dispatch count with a zero read count is decisive: the event arrived, the
 * native returned true, and nothing looked at it. A read count that climbs
 * means the plumbing is correct and the fault is in what the game does with
 * the values. */
static unsigned g_me_reads;
static unsigned g_me_logged;

unsigned lawn_motion_reads(void) { return g_me_reads; }

static void me_log(const char *what, int idx, double val) {
  g_me_reads++;
  if (g_me_logged >= 40) return;
  g_me_logged++;
  if (idx < 0) debug_log("[lawn]   read %s() -> %g\n", what, val);
  else         debug_log("[lawn]   read %s(%d) -> %g\n", what, idx, val);
}

/* Accessors. Each returns a jvalue; the dispatcher in jni_fake.c extracts the
 * field matching the call variant the game used. */
#define ME_SELF() FakeMotionEvent *me = (FakeMotionEvent *)jniref_deref(self); \
                  jvalue r; memset(&r, 0, sizeof(r)); if (!me) return r;

static jvalue me_getAction(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.i = me->action; me_log("getAction", -1, r.i); return r;
}
static jvalue me_getActionMasked(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.i = me->action & 0xff;
  me_log("getActionMasked", -1, r.i); return r;
}
static jvalue me_getActionIndex(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.i = me->action_index;
  me_log("getActionIndex", -1, r.i); return r;
}
static jvalue me_getPointerCount(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.i = me->count;
  me_log("getPointerCount", -1, r.i); return r;
}
static jvalue me_getPointerId(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; ME_SELF();
  int i = a ? a[0].i : 0;
  r.i = (i >= 0 && i < me->count) ? me->ids[i] : 0;
  me_log("getPointerId", i, r.i);
  return r;
}
static jvalue me_getX(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; ME_SELF();
  int i = a ? a[0].i : 0;
  r.f = (i >= 0 && i < me->count) ? me->xs[i] : 0.f;
  me_log("getX", i, r.f);
  return r;
}
static jvalue me_getY(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; ME_SELF();
  int i = a ? a[0].i : 0;
  r.f = (i >= 0 && i < me->count) ? me->ys[i] : 0.f;
  me_log("getY", i, r.f);
  return r;
}
static jvalue me_getEventTime(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.j = me->event_time_ms; return r;
}
static jvalue me_getDownTime(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.j = me->down_time_ms; return r;
}

/* Some backends call the no-arg getX()/getY() for the primary pointer. */
static jvalue me_getX0(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.f = me->count ? me->xs[0] : 0.f;
  me_log("getX", -1, r.f); return r;
}
static jvalue me_getY0(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; ME_SELF(); r.f = me->count ? me->ys[0] : 0.f;
  me_log("getY", -1, r.f); return r;
}

static FakeMethod g_motion_methods[] = {
  { me_getAction,       "getAction",       "()I",   NULL, 0 },
  { me_getActionMasked, "getActionMasked", "()I",   NULL, 0 },
  { me_getActionIndex,  "getActionIndex",  "()I",   NULL, 0 },
  { me_getPointerCount, "getPointerCount", "()I",   NULL, 0 },
  { me_getPointerId,    "getPointerId",    "(I)I",  NULL, 0 },
  { me_getX,            "getX",            "(I)F",  NULL, 0 },
  { me_getY,            "getY",            "(I)F",  NULL, 0 },
  { me_getX0,           "getX",            "()F",   NULL, 0 },
  { me_getY0,           "getY",            "()F",   NULL, 0 },
  { me_getEventTime,    "getEventTime",    "()J",   NULL, 0 },
  { me_getDownTime,     "getDownTime",     "()J",   NULL, 0 },
};

static FakeClass g_class_MotionEvent = {
  {NULL}, "android/view/MotionEvent", NULL,
  g_motion_methods, (int)(sizeof(g_motion_methods)/sizeof(g_motion_methods[0])),
  NULL, 0
};

/* ------------------------------------------------------------------------ */
/* Surface / SurfaceView / Activity                                          */
/* ------------------------------------------------------------------------ */

static FakeObject g_surface_obj;
static FakeClass  g_class_Surface = { {NULL}, "android/view/Surface", NULL, NULL, 0, NULL, 0 };

/* Declared so RegisterNatives can bind them. fn stays NULL: these are native
 * methods -- the managed side supplies the implementation, we only call it. */
/* Java-side wrappers.
 *
 * The host drives the game by calling the n_* natives directly, so these are
 * not on our own path. They exist because the managed side can invoke the
 * Java method -- that is how a peer calls back into its own view -- and a
 * lookup that misses there would be stubbed to a no-op, silently dropping the
 * call rather than delivering it. Each simply forwards. */
static jvalue sv_forward(JNIEnv *e, jobject self, const jvalue *a,
                         const char *nname);

#define SV_FWD(fn, nname)                                                     \
  static jvalue fn(JNIEnv *e, jobject self, const jvalue *a) {                \
    return sv_forward(e, self, a, nname);                                     \
  }

SV_FWD(sv_doFrame,          "n_doFrame")
SV_FWD(sv_surfaceCreated,   "n_surfaceCreated")
SV_FWD(sv_surfaceChanged,   "n_surfaceChanged")
SV_FWD(sv_surfaceDestroyed, "n_surfaceDestroyed")
SV_FWD(sv_onTouchEvent,     "n_onTouchEvent")
SV_FWD(sv_onKeyDown,        "n_onKeyDown")
SV_FWD(sv_onKeyUp,          "n_onKeyUp")
SV_FWD(sv_onGenericMotion,  "n_onGenericMotionEvent")
SV_FWD(sv_onCheckIsTextEd,  "n_onCheckIsTextEditor")
SV_FWD(sv_onCreateInputCon, "n_onCreateInputConnection")

static jvalue sv_ctor(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  return r;
}

/* Focus, and why these have to answer yes.
 *
 * The keyboard did not appear even though the game had fetched the
 * InputMethodManager and cached it, and showSoftInput was never called. None of
 * these existed, so every one auto-stubbed to 0 -- requestFocus() returned
 * false, isFocused() returned false, hasWindowFocus() returned false. The usual
 * Android idiom is
 *
 *     if (view.requestFocus()) imm.showSoftInput(view, 0);
 *
 * and with a stub that guard never opens. Nothing logs, nothing fails, the
 * keyboard simply never comes up -- the same silent-false shape as the
 * MotionEvent read count and the wrong view instance before it.
 *
 * On the Switch these answers are not a convenient lie. There is exactly one
 * view, it fills the screen, it is the only thing that can take input, and the
 * app has window focus whenever it is running at all. "Yes" is the true answer
 * to every one of them. */
static jvalue sv_true(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); r.z = JNI_TRUE;
  static int logged;
  if (!logged) {
    logged = 1;
    debug_log("[lawn] the game is querying the view's focus -- answering yes; "
              "on Switch this view is the only one and always has focus\n");
  }
  return r;
}

static jvalue sv_void(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); return r;
}

/* View.post / postDelayed.
 *
 * These were reaching the auto-method path and returning false, which meant the
 * Runnable was DROPPED -- the shim even said so:
 *
 *   [jni] post(Ljava/lang/Runnable;)Z returned false -- it is a stub
 *
 * The queue they belong in already exists and is already drained once a frame
 * by android_os_run_posted; Handler.post has used it since the boot fix. Only
 * the View forms were missing, so anything the game scheduled through the view
 * rather than through a Handler never ran. */
static jvalue sv_post(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  jvalue r; memset(&r, 0, sizeof(r));
  if (a && a[0].l) android_os_post_runnable(a[0].l, 0);
  r.z = JNI_TRUE;
  return r;
}

static jvalue sv_postDelayed(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  jvalue r; memset(&r, 0, sizeof(r));
  if (a && a[0].l) android_os_post_runnable(a[0].l, (uint64_t)a[1].j);
  r.z = JNI_TRUE;
  return r;
}

static jvalue sv_false(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); r.z = JNI_FALSE; return r;
}

static FakeMethod g_surfaceview_methods[] = {
  /* Focus. See the note above -- these being absent is what kept the soft
     keyboard from ever being asked for. */
  { sv_true,  "requestFocus",             "()Z",   NULL, 0 },
  { sv_true,  "requestFocus",             "(I)Z",  NULL, 0 },
  { sv_true,  "requestFocus",             "(ILandroid/graphics/Rect;)Z", NULL, 0 },
  { sv_true,  "requestFocusFromTouch",    "()Z",   NULL, 0 },
  { sv_true,  "isFocused",                "()Z",   NULL, 0 },
  { sv_true,  "hasFocus",                 "()Z",   NULL, 0 },
  { sv_true,  "hasWindowFocus",           "()Z",   NULL, 0 },
  { sv_true,  "isFocusable",              "()Z",   NULL, 0 },
  { sv_true,  "isFocusableInTouchMode",   "()Z",   NULL, 0 },
  { sv_true,  "isShown",                  "()Z",   NULL, 0 },
  { sv_true,  "isEnabled",                "()Z",   NULL, 0 },
  { sv_true,  "isAttachedToWindow",       "()Z",   NULL, 0 },
  /* Touch mode: false means "not in touch mode", which is the state in which
     Android lets a view hold focus. Reporting true would undo the above. */
  { sv_false, "isInTouchMode",            "()Z",   NULL, 0 },
  { sv_void,  "setFocusable",             "(Z)V",  NULL, 0 },
  { sv_void,  "setFocusable",             "(I)V",  NULL, 0 },
  { sv_void,  "setFocusableInTouchMode",  "(Z)V",  NULL, 0 },
  { sv_void,  "clearFocus",               "()V",   NULL, 0 },
  { sv_post,      "post",             "(Ljava/lang/Runnable;)Z",  NULL, 0 },
  { sv_postDelayed,"postDelayed",     "(Ljava/lang/Runnable;J)Z", NULL, 0 },
  { sv_true,      "removeCallbacks",  "(Ljava/lang/Runnable;)Z",  NULL, 0 },
  { sv_void,  "invalidate",               "()V",   NULL, 0 },
  { sv_void,  "requestLayout",            "()V",   NULL, 0 },

  { sv_ctor,             "<init>",        "(Landroid/content/Context;)V", NULL, 0 },
  { sv_ctor,             "<init>",        "(Landroid/content/Context;Landroid/util/AttributeSet;)V", NULL, 0 },
  { sv_ctor,             "<init>",        "(Landroid/content/Context;Landroid/util/AttributeSet;I)V", NULL, 0 },
  { sv_doFrame,          "doFrame",       "(J)V",   NULL, 0 },
  { sv_surfaceCreated,   "surfaceCreated","(Landroid/view/SurfaceHolder;)V",    NULL, 0 },
  { sv_surfaceChanged,   "surfaceChanged","(Landroid/view/SurfaceHolder;III)V", NULL, 0 },
  { sv_surfaceDestroyed, "surfaceDestroyed","(Landroid/view/SurfaceHolder;)V",  NULL, 0 },
  { sv_onTouchEvent,     "onTouchEvent",  "(Landroid/view/MotionEvent;)Z",      NULL, 0 },
  { sv_onKeyDown,        "onKeyDown",     "(ILandroid/view/KeyEvent;)Z",        NULL, 0 },
  { sv_onKeyUp,          "onKeyUp",       "(ILandroid/view/KeyEvent;)Z",        NULL, 0 },
  { sv_onGenericMotion,  "onGenericMotionEvent","(Landroid/view/MotionEvent;)Z",NULL, 0 },
  { sv_onCheckIsTextEd,  "onCheckIsTextEditor", "()Z",                          NULL, 0 },
  { sv_onCreateInputCon, "onCreateInputConnection",
      "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;", NULL, 0 },
  { NULL, "n_onCreateInputConnection",
      "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;", NULL, 0 },
  { NULL, "n_doFrame",              "(J)V",   NULL, 0 },
  { NULL, "n_surfaceCreated",       "(Landroid/view/SurfaceHolder;)V",    NULL, 0 },
  { NULL, "n_surfaceChanged",       "(Landroid/view/SurfaceHolder;III)V", NULL, 0 },
  { NULL, "n_surfaceDestroyed",     "(Landroid/view/SurfaceHolder;)V",    NULL, 0 },
  { NULL, "n_onTouchEvent",         "(Landroid/view/MotionEvent;)Z",      NULL, 0 },
  { NULL, "n_onKeyDown",            "(ILandroid/view/KeyEvent;)Z",        NULL, 0 },
  { NULL, "n_onKeyUp",              "(ILandroid/view/KeyEvent;)Z",        NULL, 0 },
  { NULL, "n_onGenericMotionEvent", "(Landroid/view/MotionEvent;)Z",      NULL, 0 },
  { NULL, "n_onCheckIsTextEditor",  "()Z",                                NULL, 0 },
};

static FakeClass g_class_SurfaceView = {
  {NULL}, "crc646ec5dadb3c3b2bda/AndroidNativeActivity_LawnSurfaceView", NULL,
  g_surfaceview_methods,
  (int)(sizeof(g_surfaceview_methods)/sizeof(g_surfaceview_methods[0])),
  NULL, 0
};

SV_FWD(ac_onCreate,        "n_onCreate")
SV_FWD(ac_onResume,        "n_onResume")
SV_FWD(ac_onPause,         "n_onPause")
SV_FWD(ac_onDestroy,       "n_onDestroy")
SV_FWD(ac_onBackPressed,   "n_onBackPressed")
SV_FWD(ac_onWindowFocus,   "n_onWindowFocusChanged")
SV_FWD(ac_onConfigChanged, "n_onConfigurationChanged")
SV_FWD(ac_onActivityResult,"n_onActivityResult")
SV_FWD(ac_onMultiWindow,   "n_onMultiWindowModeChanged")

static FakeMethod g_activity_methods[] = {
  { sv_ctor,              "<init>",       "()V",  NULL, 0 },
  { ac_onCreate,          "onCreate",     "(Landroid/os/Bundle;)V", NULL, 0 },
  { ac_onResume,          "onResume",     "()V",  NULL, 0 },
  { ac_onPause,           "onPause",      "()V",  NULL, 0 },
  { ac_onDestroy,         "onDestroy",    "()V",  NULL, 0 },
  { ac_onBackPressed,     "onBackPressed","()V",  NULL, 0 },
  { ac_onWindowFocus,     "onWindowFocusChanged", "(Z)V", NULL, 0 },
  { ac_onConfigChanged,   "onConfigurationChanged",
      "(Landroid/content/res/Configuration;)V", NULL, 0 },
  { ac_onActivityResult,  "onActivityResult", "(IILandroid/content/Intent;)V", NULL, 0 },
  { ac_onMultiWindow,     "onMultiWindowModeChanged",
      "(ZLandroid/content/res/Configuration;)V", NULL, 0 },
  /* Declared so RegisterNatives can bind them -- both were missing entirely. */
  { NULL, "n_onActivityResult", "(IILandroid/content/Intent;)V", NULL, 0 },
  { NULL, "n_onMultiWindowModeChanged",
      "(ZLandroid/content/res/Configuration;)V", NULL, 0 },
  { NULL, "n_onCreate",              "(Landroid/os/Bundle;)V", NULL, 0 },
  { NULL, "n_onResume",              "()V",                    NULL, 0 },
  { NULL, "n_onPause",               "()V",                    NULL, 0 },
  { NULL, "n_onDestroy",             "()V",                    NULL, 0 },
  { NULL, "n_onBackPressed",         "()V",                    NULL, 0 },
  { NULL, "n_onWindowFocusChanged",  "(Z)V",                   NULL, 0 },
  { NULL, "n_onConfigurationChanged","(Landroid/content/res/Configuration;)V", NULL, 0 },
};

static FakeClass g_class_Activity = {
  {NULL}, "crc646ec5dadb3c3b2bda/AndroidNativeActivity", NULL,
  g_activity_methods,
  (int)(sizeof(g_activity_methods)/sizeof(g_activity_methods[0])),
  NULL, 0
};

/* Instances. The managed side gets these as `self` on every native call. */
static FakeObject g_surfaceview_obj;
static FakeObject g_activity_obj;

static jobject g_ref_surfaceview;   /* global refs -- these outlive any frame */
static jobject g_ref_activity;
static jobject g_ref_surface;
static jobject g_ref_motion;

/* ------------------------------------------------------------------------ */
/* ANativeWindow                                                             */
/* ------------------------------------------------------------------------ */

/* The game calls ANativeWindow_fromSurface(env, surface) in surfaceCreated and
 * hands the result to eglCreateWindowSurface. devkitPro's mesa EGL takes an
 * NWindow* as its native window type, so returning the default window is
 * exactly right -- no wrapper struct needed. */
void *nx_ANativeWindow_fromSurface(void *env, void *surface) {
  (void)env; (void)surface;
  debug_log("[anw] fromSurface -> nwindowGetDefault()\n");
  return (void *)nwindowGetDefault();
}
void nx_ANativeWindow_release(void *win) { (void)win; }

/* Fixed at 1080p, matching the framebuffer.
 *
 * These followed the operation mode, which contradicted the buffer: the window
 * is allocated once at 1920x1080 and never resized, so reporting 1280x720 in
 * handheld told the game to lay out for a surface half the size of the one it
 * was drawing into. That is the same disagreement that put the picture in the
 * bottom-left corner, just reached through a different question. */
int nx_ANativeWindow_getWidth(void *win)  { (void)win; return 1920; }
int nx_ANativeWindow_getHeight(void *win) { (void)win; return 1080; }

int nx_ANativeWindow_setBuffersGeometry(void *win, int w, int h, int fmt) {
  (void)win; (void)fmt;
  /* Deliberately ignored.
   *
   * nwindowSetDimensions only takes effect before the buffers are created, so
   * honouring this after eglCreateWindowSurface would silently do nothing --
   * and honouring it BEFORE would undo the 1080p allocation and put us back to
   * a game rendering into a corner. The buffer size is the host's decision
   * here, not the game's. Reporting success is correct: the geometry the game
   * asked for is the geometry it will be told it has, by the two calls above. */
  if (w > 0 && h > 0 && (w != 1920 || h != 1080))
    debug_log("[anw] setBuffersGeometry(%dx%d) ignored -- the window is fixed "
              "at 1920x1080\n", w, h);
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Typed wrappers over the captured natives                                  */
/* ------------------------------------------------------------------------ */

/* Defined here because the wrappers above need it; the class tables are
 * declared before native_of in file order. */
static jvalue sv_forward(JNIEnv *e, jobject self, const jvalue *a,
                         const char *nname) {
  jvalue r; memset(&r, 0, sizeof(r));
  FakeObject *o = jniref_deref(self);
  if (!o || !o->cls) return r;
  for (FakeClass *k = o->cls; k; k = k->super)
    for (int i = 0; i < k->nmethods; i++)
      if (!strcmp(k->methods[i].name, nname) && k->methods[i].native_fn)
        return jni_invoke_native(e, k->methods[i].native_fn, self,
                                 k->methods[i].sig, a);
  return r;
}

static void *native_of(FakeClass *c, const char *name) {
  for (int i = 0; i < c->nmethods; i++)
    if (!strcmp(c->methods[i].name, name)) return c->methods[i].native_fn;
  return NULL;
}

int lawn_natives_ready(void) {
  return native_of(&g_class_SurfaceView, "n_doFrame") != NULL;
}

/* What actually got bound.
 *
 * "n_doFrame was never registered" on its own cannot distinguish three very
 * different situations: registerNativeMembers never ran at all; it ran for
 * other types but not these; or it ran for these and one method is missing.
 * Listing every declared native with its binding state separates them, and
 * that determines whether the problem is the bootstrap or the class tables. */
/* The pinned instances, so the peer can be constructed for each. */
jobject lawn_activity_instance(void)    { return g_ref_activity; }
jobject lawn_surface_instance(void)     { return g_ref_surface; }
/* OUR view, always -- never the adopted one.
 *
 * Only correct before setContentView has run (stage 4 peer construction). For
 * anything that dispatches INTO the game, use lawn_view_target() instead;
 * passing this one sends the call to an instance the game never initialised,
 * and the call will appear to succeed. */
jobject lawn_surfaceview_instance(void) { return g_ref_surfaceview; }

void lawn_natives_report(void) {
  struct { FakeClass *c; const char *label; } tables[] = {
    { &g_class_SurfaceView, "LawnSurfaceView" },
    { &g_class_Activity,    "AndroidNativeActivity" },
  };

  int bound = 0, total = 0;
  for (size_t t = 0; t < sizeof(tables)/sizeof(tables[0]); t++) {
    debug_log("[lawn] %s:\n", tables[t].label);
    FakeClass *c = tables[t].c;
    for (int i = 0; i < c->nmethods; i++) {
      if (strncmp(c->methods[i].name, "n_", 2) != 0) continue;
      total++;
      if (c->methods[i].native_fn) bound++;
      debug_log("[lawn]     %-28s %s\n", c->methods[i].name,
                c->methods[i].native_fn ? "bound" : "NOT BOUND");
    }
  }
  debug_log("[lawn] %d of %d natives bound\n", bound, total);

  if (bound == 0)
    debug_log("[lawn] None bound at all. registerNativeMembers registers a\n"
              "[lawn] type's natives when the managed side first constructs a\n"
              "[lawn] peer of that type -- on Android the framework creates the\n"
              "[lawn] Activity, which we never do. If nothing above is bound,\n"
              "[lawn] the peer was never constructed, and that is the problem\n"
              "[lawn] to solve rather than anything in these tables.\n");
}

/* The SurfaceHolder, NOT the Surface.
 *
 * surfaceCreated/Changed/Destroyed are declared (Landroid/view/SurfaceHolder;...)
 * and the managed marshal method turns that argument into a peer with
 * Java.Lang.Object.GetObject<ISurfaceHolder>(handle, DoNotTransfer). Passing the
 * Surface here did not fail as a type error -- it came back as a plain null,
 * because the value manager returns null when a handle cannot be resolved to
 * the requested type rather than throwing. The game's SurfaceCreated then
 * dereferenced its argument on the first instruction that used it, which is a
 * hardware null check inside managed code: fatal here, and at an address with
 * no relation to the cause.
 *
 * The Surface is still reachable from managed code -- through this object's
 * getSurface(), which is how the game gets to ANativeWindow_fromSurface. It is
 * the holder that the callback signature asks for. */
static jobject surface_holder(void) {
  jobject h = android_os_surfaceholder();
  if (!h)
    debug_log("[lawn] the SurfaceHolder is null -- android_os_init() has not "
              "run. The managed side is about to receive a null holder and "
              "will die on its first use of it.\n");
  return h;
}

/* The whole FakeMethod, not just its native_fn: the signature is what tells us
 * how many arguments the managed side is going to read. */
static const FakeMethod *method_of(FakeClass *c, const char *name) {
  for (int i = 0; i < c->nmethods; i++)
    if (!strcmp(c->methods[i].name, name)) return &c->methods[i];
  return NULL;
}

/* The view every SurfaceView-side callback should target.
 *
 * The game's Activity builds its own LawnSurfaceView in OnCreate and installs
 * it with setContentView; ours, built in stage 4, is a second instance nothing
 * else knows about. Callbacks must go to the one the game is actually using,
 * or the state they set lands on an object the game never looks at. Falls back
 * to ours only before OnCreate has run. */
jobject lawn_view_target(void) {
  jobject v = android_content_view();
  return v ? v : g_ref_surfaceview;
}
#define view_target() lawn_view_target()

void lawn_do_frame(int64_t frame_time_nanos) {
  typedef void (*fn_t)(JNIEnv *, jobject, jlong);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_doFrame");
  if (f) f(jni_get_env(), view_target(), (jlong)frame_time_nanos);
}

void lawn_surface_created(void) {
  typedef void (*fn_t)(JNIEnv *, jobject, jobject);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_surfaceCreated");
  if (!f) { debug_log("[lawn] n_surfaceCreated not registered\n"); return; }
  debug_log("[lawn] -> n_surfaceCreated\n");
  f(jni_get_env(), view_target(), surface_holder());
}

void lawn_surface_changed(int w, int h) {
  typedef void (*fn_t)(JNIEnv *, jobject, jobject, jint, jint, jint);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_surfaceChanged");
  if (!f) { debug_log("[lawn] n_surfaceChanged not registered\n"); return; }
  debug_log("[lawn] -> n_surfaceChanged(%d x %d)\n", w, h);
  f(jni_get_env(), view_target(), surface_holder(), 4 /*RGBA_8888*/, w, h);
}

void lawn_surface_destroyed(void) {
  typedef void (*fn_t)(JNIEnv *, jobject, jobject);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_surfaceDestroyed");
  if (!f) { debug_log("[lawn] n_surfaceDestroyed not registered\n"); return; }
  debug_log("[lawn] -> n_surfaceDestroyed\n");
  f(jni_get_env(), view_target(), surface_holder());
}

void lawn_activity_lifecycle(const char *which) {
  typedef void (*fn_t)(JNIEnv *, jobject);
  const FakeMethod *m = method_of(&g_class_Activity, which);
  if (!m || !m->native_fn) {
    debug_log("[lawn] lifecycle %s not registered\n", which);
    return;
  }

  /* Only ()V goes through here.
   *
   * n_onCreate takes a Bundle and n_onWindowFocusChanged takes a boolean.
   * Calling either through this two-argument prototype leaves the extra
   * parameter register holding whatever the previous call left in it, and the
   * managed marshal method reads it as a jobject and hands it to GetObject.
   * n_onCreate happened to see zero, which is a legal `null`
   * savedInstanceState -- but that was the register being clean, not a
   * decision. Anything with parameters gets its own wrapper below. */
  if (!m->sig || strcmp(m->sig, "()V") != 0) {
    debug_log("[lawn] %s is %s, not ()V -- it needs a wrapper that passes its "
              "arguments explicitly; refusing to call it with uninitialised "
              "argument registers\n", which, m->sig ? m->sig : "(no signature)");
    return;
  }

  debug_log("[lawn] -> %s\n", which);
  ((fn_t)m->native_fn)(jni_get_env(), g_ref_activity);
}

/* n_onWindowFocusChanged(Z)V.
 *
 * Bound since the first round natives were registered, and never called once.
 *
 * Android delivers this when the activity gains or loses the input focus, and a
 * game that has not been told it has focus reasonably concludes it is in the
 * background: it pauses audio and stops drawing. That is exactly the state the
 * last run ended in --
 *
 *     [aaudio] requestPause -- the callback thread will idle
 *     [Lawn] AAudio stream state: Paused.
 *     [main] 300 consecutive frames without a present -- ticking but not drawing
 *
 * -- with everything else finally working: the surface initialised, the GC
 * committing 120 MB, doFrame being called every frame. The game was awake and
 * deliberately idle, waiting to be told it was in front.
 *
 * This is the same family as the earlier gaps -- a callback the shim was given
 * and never delivered -- but inverted: nothing was dropped, it simply was never
 * sent. A bound native that is never invoked is as invisible as a Runnable that
 * is never run. */
void lawn_activity_focus(int has_focus) {
  typedef void (*fn_t)(JNIEnv *, jobject, jboolean);
  const FakeMethod *m = method_of(&g_class_Activity, "n_onWindowFocusChanged");
  if (!m || !m->native_fn) {
    debug_log("[lawn] n_onWindowFocusChanged not registered\n");
    return;
  }
  debug_log("[lawn] -> n_onWindowFocusChanged(%s)\n", has_focus ? "true" : "false");
  ((fn_t)m->native_fn)(jni_get_env(), g_ref_activity,
                       has_focus ? JNI_TRUE : JNI_FALSE);
}

/* n_onCreate(Landroid/os/Bundle;)V.
 *
 * null is the right value rather than a placeholder: Android passes null for
 * savedInstanceState on a first launch and the managed parameter is nullable,
 * so the game takes its cold-start path. What matters is that it is passed
 * deliberately, in the third argument register, instead of being whatever
 * survived the previous call. */
void lawn_activity_create(void) {
  typedef void (*fn_t)(JNIEnv *, jobject, jobject);
  const FakeMethod *m = method_of(&g_class_Activity, "n_onCreate");
  if (!m || !m->native_fn) {
    debug_log("[lawn] n_onCreate not registered\n");
    return;
  }
  debug_log("[lawn] -> n_onCreate (savedInstanceState = null)\n");
  ((fn_t)m->native_fn)(jni_get_env(), g_ref_activity, NULL);
}

/* Feed one batch of pointer state. Call once per frame with the current set of
 * active pointers plus whichever transition happened this frame. */
void lawn_dispatch_touch(int action, int action_index,
                         int count, const int *ids,
                         const float *xs, const float *ys) {
  typedef jboolean (*fn_t)(JNIEnv *, jobject, jobject);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_onTouchEvent");
  if (!f) {
    /* Was a bare `return`. A touch dispatch that goes nowhere and says nothing
     * is the exact failure this port keeps rediscovering, so say it once. */
    static int warned;
    if (!warned) {
      warned = 1;
      debug_log("[lawn] *** n_onTouchEvent is NOT bound -- every touch is "
                "being dropped here. Look for its RegisterNatives line above; "
                "if it is missing, the game never registered it. ***\n");
    }
    return;
  }

  if (count > MAX_POINTERS) count = MAX_POINTERS;
  g_motion.action       = action | (action_index << 8);
  g_motion.action_index = action_index;
  g_motion.count        = count;
  for (int i = 0; i < count; i++) {
    g_motion.ids[i] = ids[i];
    g_motion.xs[i]  = xs[i];
    g_motion.ys[i]  = ys[i];
  }
  g_motion.event_time_ms = (int64_t)(armTicksToNs(armGetSystemTick()) / 1000000ull);
  if (action == AMOTION_DOWN) g_motion.down_time_ms = g_motion.event_time_ms;

  /* view_target(), NOT g_ref_surfaceview.
   *
   * THIS WAS THE BUG that made every tap vanish. The game's Activity builds its
   * own LawnSurfaceView in OnCreate and installs it via setContentView; ours,
   * built in stage 4, is a second instance nothing else knows about. When
   * adoption was introduced, the four SurfaceView callbacks that existed then
   * -- doFrame, surfaceCreated/Changed/Destroyed -- were moved onto
   * view_target(). The three input entry points were not, because nothing
   * called them yet: input was wired up several rounds later.
   *
   * The symptom was maddeningly specific. n_onTouchEvent was entered and
   * returned "consumed", so every counter in the chain looked healthy, but the
   * managed peer resolved from OUR orphan view had none of the game's state,
   * so OnTouchEvent early-returned true and never read a field off the event.
   * That is exactly what "23 dispatches -> 0 MotionEvent reads" was reporting. */
  jobject target = view_target();
  static int announced;
  if (!announced) {
    announced = 1;
    debug_log("[lawn] touch target %p (%s)\n", (void *)target,
              target == g_ref_surfaceview
                  ? "*** our own view -- setContentView has not run, the game "
                    "will ignore this ***"
                  : "the game's adopted view");
  }
  jboolean consumed = f(jni_get_env(), target, g_ref_motion);

  /* Whether the game CONSUMED it is the difference between "the event never
   * arrived" and "it arrived and the game chose to ignore it" -- two problems
   * with nothing in common. The first few, then only on change. */
  static int nlogged; static int last_consumed = -1;
  if (nlogged < 8 || consumed != last_consumed) {
    if (nlogged < 8) nlogged++;
    last_consumed = consumed;
    debug_log("[lawn] n_onTouchEvent(action=0x%x count=%d) -> %s\n",
              g_motion.action, g_motion.count,
              consumed ? "consumed" : "NOT consumed (the game ignored it)");
  }
}

/* Key events. The SurfaceView gets first refusal -- menus usually want BACK
 * before the Activity does -- and only if it declines does the Activity see it
 * as onBackPressed. That ordering is what Android does, and MonoGame's input
 * handling assumes it. */
void lawn_key(int action, int keycode) { lawn_key_unicode(action, keycode, 0); }

/* The unicode-carrying form.
 *
 * android_make_key_event has always taken a unicode argument and the fake
 * KeyEvent has always implemented getUnicodeChar(); lawn_key just hardcoded 0,
 * because until the keyboard existed nothing produced characters. Text from
 * swkbd arrives here with keycode 0 and the code point set, which is what a
 * view reading getUnicodeChar() expects. */
void lawn_key_unicode(int action, int keycode, int unicode) {
  lawn_key_full(action, keycode, unicode, 0);
}

void lawn_key_full(int action, int keycode, int unicode, int meta) {
  typedef jboolean (*fn_t)(JNIEnv *, jobject, jint, jobject);
  const char *which = (action == 0) ? "n_onKeyDown" : "n_onKeyUp";
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, which);
  if (!f) {
    /* Was a bare return. A key that goes nowhere and says nothing is the shape
       this port keeps rediscovering; say it once. */
    static int warned[2];
    int i = (action == 0) ? 0 : 1;
    if (!warned[i]) {
      warned[i] = 1;
      debug_log("[lawn] *** %s is NOT bound -- every key event is being "
                "dropped here, including software-keyboard text ***\n", which);
    }
    return;
  }

  jobject ev = android_make_key_event_meta(action, keycode, unicode, meta);
  jboolean consumed = f(jni_get_env(), view_target(), (jint)keycode, ev);

  if (!consumed && action == 0 && keycode == 4 /* BACK */)
    lawn_activity_lifecycle("n_onBackPressed");
}

/* Ask the view for an InputConnection, as the framework does when a field
 * takes focus. NULL if the game does not override it -- a legitimate answer,
 * and the caller falls back to key events. */
jobject lawn_create_input_connection(JNIEnv *env, jobject editor_info) {
  typedef jobject (*fn_t)(JNIEnv *, jobject, jobject);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_onCreateInputConnection");
  if (!f) {
    debug_log("[lawn] n_onCreateInputConnection is not bound; text will go in "
              "as key events\n");
    return NULL;
  }
  jobject ic = f(env, view_target(), editor_info);

  /* The class only comes into existence here, so this is the first moment its
     natives CAN be bound. Without it commitText is a declared-but-unbound stub
     that returns zero, and every character typed is silently dropped. */
  lawn_register_input_connection(env);
  return ic;
}

/* Does the view want text input right now?
 *
 * This is the question the Android framework asks a focused view before it
 * brings a keyboard up, and the game overrides it -- so it is the one honest
 * signal available that a field is waiting. The game never calls
 * showSoftInput, so without polling this there is nothing to trigger on. */
jboolean lawn_is_text_editor(void) {
  typedef jboolean (*fn_t)(JNIEnv *, jobject);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_onCheckIsTextEditor");
  if (!f) return JNI_FALSE;
  return f(jni_get_env(), view_target());
}

jboolean lawn_generic_motion(void) {
  typedef jboolean (*fn_t)(JNIEnv *, jobject, jobject);
  fn_t f = (fn_t)native_of(&g_class_SurfaceView, "n_onGenericMotionEvent");
  if (!f) return JNI_FALSE;
  return f(jni_get_env(), view_target(), g_ref_motion);
}

/* ------------------------------------------------------------------------ */

void lawn_natives_init(void) {
  jni_register_class(&g_class_MotionEvent);
  jni_register_class(&g_class_Surface);
  jni_register_class(&g_class_SurfaceView);
  jni_register_class(&g_class_Activity);

  g_motion.hdr.cls          = &g_class_MotionEvent;
  g_surface_obj.cls         = &g_class_Surface;
  g_surfaceview_obj.cls     = &g_class_SurfaceView;
  g_activity_obj.cls        = &g_class_Activity;

  /* Global refs: these instances live for the whole session, so a local ref
   * would be wrong -- and GetObjectRefType reporting LOCAL for something the
   * managed peer holds long-term is exactly the class of bug that surfaces as
   * a GC crash rather than a JNI error. */
  g_ref_surfaceview = jniref_new(&g_surfaceview_obj, REF_GLOBAL);
  g_ref_activity    = jniref_new(&g_activity_obj,    REF_GLOBAL);
  g_ref_surface     = jniref_new(&g_surface_obj,     REF_GLOBAL);
  g_ref_motion      = jniref_new(&g_motion.hdr,      REF_GLOBAL);

  /* Owned by the host: the frame loop calls into these every frame, so a
   * managed finalizer must not be able to release them out from under it. */
  jniref_pin(g_ref_surfaceview);
  jniref_pin(g_ref_activity);
  jniref_pin(g_ref_surface);
  jniref_pin(g_ref_motion);

  debug_log("[lawn] classes registered, instances pinned\n");
}
