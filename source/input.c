/* input.c -- nx_pointer events to Android MotionEvent dispatches.
 *
 * The part that is easy to get wrong: Android's MotionEvent protocol is not
 * one event per finger. Every dispatch carries the FULL set of currently-down
 * pointers, and the action encodes what changed relative to the previous
 * dispatch:
 *
 *   first finger down          ACTION_DOWN          (index always 0)
 *   additional finger down     ACTION_POINTER_DOWN  (index in bits 8..15)
 *   any movement               ACTION_MOVE          (no index)
 *   non-last finger up         ACTION_POINTER_UP    (index in bits 8..15)
 *   last finger up             ACTION_UP
 *
 * and a down or up must be dispatched ON ITS OWN, with the pointer set as it
 * is at that instant -- a POINTER_DOWN whose set already includes a second new
 * finger will desynchronise the engine's tracking. So this module diffs the
 * previous frame's pointer set against the current one and emits a SEQUENCE of
 * dispatches per frame rather than a single batch. Feeding raw touch state
 * straight through is the usual cause of "drag works but pinch does not".
 *
 * nx_pointer.{c,h} are present and NXP_AVAILABLE defaults to 1;
 * set it to 0 to isolate an input problem from a rendering one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "android_classes.h"
#include "input.h"
#include "ime_shim.h"
#include "lawn_natives.h"
#include "util.h"

#ifndef NXP_AVAILABLE
#define NXP_AVAILABLE 0
#endif

#if NXP_AVAILABLE
#include "nx_pointer.h"
#else
/* Mirrors nx_pointer.h so this file compiles and the wiring can be reviewed
 * before the module is added. Delete once the real header is present. */
enum { NXP_DOWN = 1, NXP_MOVE = 2, NXP_UP = 3 };
typedef struct { int id; float x, y; int phase; } NxpEvent;
#endif

#define MAX_PTR 8

#if NXP_AVAILABLE
/* Route nx_pointer's internal log into ours.
 *
 * This was NULL, which threw the whole stream away: the init line with the
 * resolved panel/screen sizes, whether the gyro sensors bound, whether a mouse
 * is seen, every cursor toggle and sensitivity change. The library was wired
 * in and its diagnostics were not, so the first "taps do not register" report
 * arrived with no way to tell which stage was failing. That is the recurring
 * bug shape in this port, committed inside the tooling meant to catch it. */
static void nxp_log_bridge(const char *msg) {
  if (msg) debug_log("[nxp] %s", msg);
}

/* Stage counters for the periodic probe below. */
static unsigned g_probe_frames;      /* frames since the last report        */
static unsigned g_probe_raw_frames;  /* frames the PANEL reported a touch   */
static unsigned g_probe_raw_max;     /* most simultaneous points seen       */
static unsigned g_probe_events;      /* events nx_pointer handed us         */
static unsigned g_probe_dispatch;    /* dispatches sent to n_onTouchEvent   */
static unsigned g_dispatch_logged;   /* detail lines emitted so far         */
static unsigned g_gestures;          /* completed DOWN..UP gestures         */
static float    g_down_x, g_down_y;  /* where the current gesture started   */
static u64      g_down_ns;
#endif

/* Android keycodes we care about. */
/* B -> Android BACK. OFF.
 *
 * It was doing more than "go back". lawn_key_full escalates an UNCONSUMED
 * BACK key-down to n_onBackPressed on the Activity, and in this game that is
 * the quit path -- so any press the current screen did not happen to want
 * became an exit request. That is a much worse failure than the button simply
 * doing nothing, and it is why B was causing trouble.
 *
 * Set to 1 to restore the mapping. If it misbehaves again, the escalation in
 * lawn_key_full is the thing to look at first, not this binding: a back button
 * that quits when a menu ignores it is wrong regardless of what presses it.
 *
 * Nothing else is affected -- the exit combo is L+R+Plus in main.c, and the
 * software keyboard's own B (backspace) belongs to the keyboard applet, which
 * runs modally with this loop stopped. */
#define BACK_BUTTON 0

#define AKEYCODE_BACK   4
#define AKEY_ACTION_DOWN 0
#define AKEY_ACTION_UP   1

typedef struct { int id; float x, y; } Ptr;

static Ptr g_prev[MAX_PTR];  static int g_nprev;
static Ptr g_cur[MAX_PTR];   static int g_ncur;

#if BACK_BUTTON
static int  g_back_held;
#endif
static int  g_screen_w = 1280, g_screen_h = 720;

/* ------------------------------------------------------------------------ */

static int find_id(const Ptr *set, int n, int id) {
  for (int i = 0; i < n; i++) if (set[i].id == id) return i;
  return -1;
}

static void dispatch(int action, int index, const Ptr *set, int n) {
  int   ids[MAX_PTR];
  float xs[MAX_PTR], ys[MAX_PTR];
  for (int i = 0; i < n && i < MAX_PTR; i++) {
    ids[i] = set[i].id;
    xs[i]  = set[i].x;
    ys[i]  = set[i].y;
  }

#if NXP_AVAILABLE
  g_probe_dispatch++;

  /* One line per completed gesture, rather than per dispatch.
   *
   * A tap at 59 fps is a DOWN, a handful of MOVEs and an UP; logging each of
   * them buried the shape in noise and the 8-line cap was spent entirely
   * inside the slow loading phase, where frames took 700 ms and several
   * separate taps merged into what looked like one long drag. Duration and
   * displacement say directly whether the game is being handed a tap or a
   * drag. */
  if (action == LAWN_ACTION_DOWN && n) {
    g_down_x = xs[0]; g_down_y = ys[0];
    g_down_ns = armTicksToNs(armGetSystemTick());
  } else if (action == LAWN_ACTION_UP && n) {
    g_gestures++;
    const double ms = g_down_ns
        ? (double)(armTicksToNs(armGetSystemTick()) - g_down_ns) / 1.0e6 : -1.0;
    const double dx = (double)xs[0] - (double)g_down_x;
    const double dy = (double)ys[0] - (double)g_down_y;
    const double d2 = dx * dx + dy * dy;
    debug_log("[input] gesture %u: (%.0f,%.0f) -> (%.0f,%.0f)  %.0f ms, "
              "moved^2 %.0f px  [%s]\n", g_gestures,
              (double)g_down_x, (double)g_down_y, (double)xs[0], (double)ys[0],
              ms, d2,
              (d2 <= 400.0 && ms >= 0 && ms < 700) ? "a TAP"
                                                   : "a drag or long press");
    g_down_ns = 0;
  }

  /* The first few in full. If these appear and the game still does nothing,
     the coordinates or the action encoding are wrong, not the plumbing. */
  if (g_dispatch_logged < 8) {
    g_dispatch_logged++;
    const char *nm = action == LAWN_ACTION_DOWN         ? "DOWN"
                   : action == LAWN_ACTION_UP           ? "UP"
                   : action == LAWN_ACTION_MOVE         ? "MOVE"
                   : action == LAWN_ACTION_POINTER_DOWN ? "POINTER_DOWN"
                   : action == LAWN_ACTION_POINTER_UP   ? "POINTER_UP" : "?";
    debug_log("[input] dispatch %s index=%d count=%d  p0=(%.0f,%.0f) id=%d\n",
              nm, index, n, n ? (double)xs[0] : 0.0, n ? (double)ys[0] : 0.0,
              n ? ids[0] : -1);
  }
#endif

  lawn_dispatch_touch(action, index, n, ids, xs, ys);
}

/* ------------------------------------------------------------------------ */

void input_init(int screen_w, int screen_h, const char *data_dir) {
  g_screen_w = screen_w;
  g_screen_h = screen_h;
  g_nprev = g_ncur = 0;

#if NXP_AVAILABLE
  NxpConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.screen_w        = screen_w;
  cfg.screen_h        = screen_h;
  cfg.panel_w         = 1280;      /* touch panel space, always 1280x720   */
  cfg.panel_h         = 720;
  cfg.data_dir        = data_dir;
  cfg.cursor_id       = MAX_PTR - 1;      /* 7 */
  cfg.max_touch_slots = MAX_PTR - 1;      /* touch ids 0..6 */
  /* One slot is reserved for the cursor.
   *
   * With max_touch_slots == MAX_PTR the ids run 0..7 and the cursor takes 8,
   * which is nine possible pointers into eight-element arrays here and in
   * lawn_natives (MAX_POINTERS == 8). The ninth is silently dropped by the
   * bounds check below -- it would never crash, and with a game that uses one
   * or two fingers it would never even be reached, which is exactly the kind
   * of thing that sits unnoticed until it is a bug report about multitouch. */
  cfg.log             = nxp_log_bridge;
  /* newlib's handle table is not thread safe and the runtime will be doing
   * file I/O from workers by the time settings get written. */
  cfg.fopen_fn        = locked_fopen;
  cfg.fclose_fn       = locked_fclose;
  /* nxp_init calls this too. Doing it here as well is harmless and removes
     init ORDER as a variable if the panel turns out to report nothing. */
  hidInitializeTouchScreen();

  nxp_init(&cfg);
  debug_log("[input] nx_pointer initialised (%dx%d), touch slots 0..%d, "
            "cursor id %d\n", screen_w, screen_h, MAX_PTR - 2, MAX_PTR - 1);
#else
  (void)data_dir;
  debug_log("[input] nx_pointer NOT COMPILED IN -- touch will not work.\n"
            "[input] Add nx_pointer.{c,h} to source/ and build with "
            "-DNXP_AVAILABLE=1\n");
#endif
}

void input_set_screen(int w, int h) {
  g_screen_w = w;
  g_screen_h = h;
#if NXP_AVAILABLE
  nxp_set_screen(w, h);
#endif
}

/* ------------------------------------------------------------------------ */

void input_update(void) {
#if NXP_AVAILABLE
  /* Read the panel DIRECTLY, before nx_pointer does.
   *
   * This exists to bisect the chain in a single run. There are four stages
   * between a finger and the game, and a silent failure looks identical from
   * the outside at every one of them:
   *
   *   panel -> nx_pointer events -> our dispatch -> n_onTouchEvent
   *
   * The report below gives a count for each, so the first zero names the
   * broken stage:
   *
   *   panel 0            -> the touchscreen is not delivering at all; the
   *                         problem is init or environment, not this file
   *   panel >0, events 0 -> nx_pointer's do_touch is dropping them
   *   events >0, disp. 0 -> the pointer-set diff below is wrong
   *   disp. >0, and the game still ignores it -> the MotionEvent contents or
   *                         the action encoding are wrong; compare against the
   *                         per-dispatch lines, and look for MotionEvent
   *                         accessor traffic in the [jni] log
   *
   * Reading the state twice per frame is safe: hidGetTouchScreenStates copies
   * out of the shared-memory ring and does not consume anything. */
  {
    HidTouchScreenState ts;
    memset(&ts, 0, sizeof(ts));
    if (hidGetTouchScreenStates(&ts, 1) > 0 && ts.count > 0) {
      g_probe_raw_frames++;
      if ((unsigned)ts.count > g_probe_raw_max) g_probe_raw_max = (unsigned)ts.count;
    }
  }

  nxp_update();

  NxpEvent evs[MAX_PTR * 2];
  int n = nxp_poll(evs, (int)(sizeof(evs) / sizeof(evs[0])));
  g_probe_events += (unsigned)n;

  /* Report on a fixed cadence rather than only when something happens --
     "nothing arrived" and "this code is not running" must not look the same. */
  if (++g_probe_frames >= 600) {
    float cx = 0, cy = 0;
    nxp_cursor_pos(&cx, &cy);
    debug_log("[input] last %u frames: panel touched on %u frames (max %u "
              "points) -> %u nx_pointer events -> %u dispatches -> %u "
              "MotionEvent reads by the game. "
              "cursor %s at (%.0f,%.0f); mouse %s; gyro %s\n",
              g_probe_frames, g_probe_raw_frames, g_probe_raw_max,
              g_probe_events, g_probe_dispatch, lawn_motion_reads(),
              nxp_cursor_visible() ? "ON" : "OFF (press '+')",
              (double)cx, (double)cy,
              nxp_mouse_connected() ? "connected" : "none",
              nxp_gyro_enabled() ? "on" : "off");
    g_probe_frames = g_probe_raw_frames = g_probe_raw_max = 0;
    g_probe_events = g_probe_dispatch = 0;
  }

  /* Fold this frame's events into the current pointer set. */
  for (int i = 0; i < n; i++) {
    int idx = find_id(g_cur, g_ncur, evs[i].id);

    if (evs[i].phase == NXP_UP) {
      if (idx >= 0) {
        for (int j = idx; j < g_ncur - 1; j++) g_cur[j] = g_cur[j + 1];
        g_ncur--;
      }
      continue;
    }

    if (idx < 0) {
      if (g_ncur >= MAX_PTR) continue;
      idx = g_ncur++;
      g_cur[idx].id = evs[i].id;
    }
    g_cur[idx].x = evs[i].x;
    g_cur[idx].y = evs[i].y;
  }
#else
  /* Without nx_pointer there is nothing to read; the diff below then does
   * nothing, which is the correct no-op. */
#endif

  /* ---- diff previous against current, emitting one dispatch per change ---- */

  /* Lifts first: a finger that left must be reported while the set still
   * contains it, and before any new finger is announced. */
  for (int i = 0; i < g_nprev; i++) {
    if (find_id(g_cur, g_ncur, g_prev[i].id) >= 0) continue;

    /* Rebuild the set as it was at the moment of the lift: everything still
     * down, plus the one leaving. */
    Ptr set[MAX_PTR];
    int m = 0;
    for (int j = 0; j < g_ncur && m < MAX_PTR; j++) set[m++] = g_cur[j];
    int lift_index = m;
    if (m < MAX_PTR) set[m++] = g_prev[i];

    dispatch(m == 1 ? LAWN_ACTION_UP : LAWN_ACTION_POINTER_UP,
             m == 1 ? 0 : lift_index, set, m);
  }

  /* Then presses, one at a time, each with the set as it stands including
   * that finger but not any later one. */
  for (int i = 0; i < g_ncur; i++) {
    if (find_id(g_prev, g_nprev, g_cur[i].id) >= 0) continue;

    Ptr set[MAX_PTR];
    int m = 0;
    for (int j = 0; j <= i && m < MAX_PTR; j++) set[m++] = g_cur[j];

    dispatch(m == 1 ? LAWN_ACTION_DOWN : LAWN_ACTION_POINTER_DOWN,
             m - 1, set, m);
  }

  /* Finally movement, if anything moved and nothing changed shape. */
  if (g_ncur > 0) {
    int moved = 0;
    for (int i = 0; i < g_ncur; i++) {
      int p = find_id(g_prev, g_nprev, g_cur[i].id);
      if (p < 0) continue;
      if (g_prev[p].x != g_cur[i].x || g_prev[p].y != g_cur[i].y) { moved = 1; break; }
    }
    if (moved) dispatch(LAWN_ACTION_MOVE, 0, g_cur, g_ncur);
  }

  memcpy(g_prev, g_cur, sizeof(Ptr) * (size_t)g_ncur);
  g_nprev = g_ncur;
}

/* ------------------------------------------------------------------------ */

void input_handle_buttons(unsigned long long down, unsigned long long up) {
#if BACK_BUTTON
  /* Routed through the key path first so the SurfaceView can consume it --
   * menus usually want it before the Activity does. See BACK_BUTTON above for
   * why this is off. */
  if (down & HidNpadButton_B) {
    if (!g_back_held) {
      g_back_held = 1;
      lawn_key(AKEY_ACTION_DOWN, AKEYCODE_BACK);
    }
  }
  if (up & HidNpadButton_B) {
    if (g_back_held) {
      g_back_held = 0;
      lawn_key(AKEY_ACTION_UP, AKEYCODE_BACK);
    }
  }
#else
  (void)down; (void)up;
#endif

  /* Y used to open the keyboard by hand.
   *
   * That existed because the game holds an InputMethodManager and never calls
   * showSoftInput, so nothing raised a keyboard for a field that wanted one.
   * ime_pump polls onCheckIsTextEditor now and raises on the edge, which is
   * the game's own signal and needs no button -- so the manual binding is
   * gone and Y is free again.
   *
   * ime_request_keyboard() is kept, and is what the poll calls. If a field
   * ever turns up that does not report itself a text editor, rebinding a
   * button here is a one-line change. */
}

void input_draw_overlay(void) {
#if NXP_AVAILABLE
  nxp_draw();
#endif
}

void input_shutdown(void) {
#if NXP_AVAILABLE
  nxp_save_settings();
#endif
}
