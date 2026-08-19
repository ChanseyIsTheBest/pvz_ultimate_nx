#ifndef PVZU_LAWN_NATIVES_H
#define PVZU_LAWN_NATIVES_H

#include <jni.h>
#include <stdint.h>

/* Android action codes, re-exported so main.c can build batches. */
#define LAWN_ACTION_DOWN          0
#define LAWN_ACTION_UP            1
#define LAWN_ACTION_MOVE          2
#define LAWN_ACTION_CANCEL        3
#define LAWN_ACTION_POINTER_DOWN  5
#define LAWN_ACTION_POINTER_UP    6

void lawn_natives_init(void);

/* True once RegisterNatives has handed us n_doFrame. Until then there is
 * nothing to drive and the frame loop should not start. */
int  lawn_natives_ready(void);

/* Every declared native and whether RegisterNatives bound it. */
void lawn_natives_report(void);

/* Global refs to the instances the host drives. Constructing a managed peer
 * for these is what registers their native methods. */
jobject lawn_activity_instance(void);
jobject lawn_surfaceview_instance(void);

/* The view every callback INTO the game must target: the one the Activity
 * installed with setContentView, falling back to ours only before OnCreate has
 * run. Using lawn_surfaceview_instance() instead resolves to a managed peer
 * with none of the game's state, and the call still appears to succeed. */
jobject lawn_view_target(void);
jobject lawn_surface_instance(void);

void lawn_do_frame(int64_t frame_time_nanos);
void lawn_surface_created(void);
void lawn_surface_changed(int w, int h);
void lawn_surface_destroyed(void);

/* which: "n_onResume", "n_onPause", "n_onDestroy", "n_onBackPressed".
 *
 * ()V ONLY -- it passes no arguments, and a method that declares any would
 * read an uninitialised register. It checks the signature and refuses rather
 * than calling. n_onCreate has its own wrapper below for exactly that reason. */
void lawn_activity_lifecycle(const char *which);

/* n_onCreate, with an explicit null savedInstanceState. */
void lawn_activity_create(void);

/* n_onWindowFocusChanged(Z). Must be sent, or the game believes it is in the
 * background: it pauses audio and does not draw. */
void lawn_activity_focus(int has_focus);

/* action: 0 = down, 1 = up. Routes to the SurfaceView first, falling back to
 * the Activity's onBackPressed if the view declines a BACK press. */
void lawn_key(int action, int keycode);

/* As lawn_key, but carries a unicode code point in the KeyEvent. Text from the
 * software keyboard uses keycode 0 with the code point set. */
void lawn_key_unicode(int action, int keycode, int unicode);

/* As above, plus meta state -- needed so a game reading getKeyCode() can tell
 * an upper-case letter from a lower-case one. */
void lawn_key_full(int action, int keycode, int unicode, int meta);

/* The view's InputConnection, or NULL if it does not supply one. */
jobject lawn_create_input_connection(JNIEnv *env, jobject editor_info);

/* Whether the view is currently a text editor -- the framework's own test for
 * "should a keyboard be up". Polled, because the game never asks for one. */
jboolean lawn_is_text_editor(void);

/* How many times the game has read a field off the MotionEvent we hand it.
 * Zero while dispatches climb means the event arrives and nothing looks at
 * it -- a completely different problem from it not arriving. */
unsigned lawn_motion_reads(void);

jboolean lawn_generic_motion(void);

void lawn_dispatch_touch(int action, int action_index,
                         int count, const int *ids,
                         const float *xs, const float *ys);

/* ANativeWindow entry points handed out by dlsym on libandroid.so. */
void *nx_ANativeWindow_fromSurface(void *env, void *surface);
void  nx_ANativeWindow_release(void *win);
int   nx_ANativeWindow_getWidth(void *win);
int   nx_ANativeWindow_getHeight(void *win);
int   nx_ANativeWindow_setBuffersGeometry(void *win, int w, int h, int fmt);

#endif
