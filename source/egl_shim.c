/* egl_shim.c -- interception of the handful of EGL calls we need to see.
 *
 * The problem this solves: the game calls eglSwapBuffers itself, from inside
 * doFrame, because on Android the Choreographer callback owns the frame. The
 * host never gets a moment between "game finished drawing" and "frame is
 * presented", which is exactly where a cursor overlay has to go. Drawing it
 * from the frame loop instead puts it UNDERNEATH the next frame -- it will
 * appear to flicker or lag by one frame, and it is a confusing symptom to
 * diagnose from the outside.
 *
 * So we hand the game our own eglSwapBuffers. dl_shim consults this file
 * before falling through to eglGetProcAddress, so the substitution happens
 * wherever the engine resolves the symbol -- Silk.NET resolves the whole GLES
 * surface dynamically, so there is no static link to patch.
 *
 * Everything not listed here falls through to mesa untouched.
 */

#include <string.h>
#include <switch.h>

#include <EGL/egl.h>

#include "egl_shim.h"
#include "gl_guard.h"
#include "input.h"
#include "mem_arena.h"
#include "threads.h"
#include "util.h"

/* Defined below; used by the swap path above it. */
static void report_gl_errors(const char *when);

static unsigned  g_frames;
static unsigned  g_total_swaps;
static u64       g_last_report;
static u64       g_frame_accum;
static u64       g_last_swap;

static EGLSurface g_tracked_surface = EGL_NO_SURFACE;
static EGLDisplay g_tracked_display = EGL_NO_DISPLAY;

/* ------------------------------------------------------------------------ */

static EGLBoolean nx_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  /* The overlay goes here: the game's draw calls are done, the back buffer is
   * complete, and nothing has been presented yet. nxp_draw saves and restores
   * all GL state, so it is safe to call with the engine's context current. */
  report_gl_errors("the end of a frame");
  input_draw_overlay();

  EGLBoolean ok = eglSwapBuffers(dpy, surface);

  u64 now = armTicksToNs(armGetSystemTick());
  if (g_last_swap) g_frame_accum += now - g_last_swap;
  g_last_swap = now;
  g_frames++;
  g_total_swaps++;

  /* Report roughly every 10 seconds. Frame pacing is the first thing you want
   * to know once anything renders at all, and it is cheap to collect here. */
  if (now - g_last_report > 10000000000ull) {
    if (g_frames > 1) {
      u64 avg_ns = g_frame_accum / g_frames;
      debug_log("[egl] %u frames, avg %llu.%02llu ms (%llu fps)\n",
                g_frames,
                (unsigned long long)(avg_ns / 1000000),
                (unsigned long long)((avg_ns % 1000000) / 10000),
                (unsigned long long)(avg_ns ? 1000000000ull / avg_ns : 0));
    }
    /* The heap on the same cadence.
     *
     * heap_report() used to be called only from the two startup checkpoints in
     * main.c, so the last heap figure in a crashing log was minutes stale and
     * said nothing about the moment that mattered. */
    heap_report();
    gl_guard_report();

    g_last_report = now;
    g_frames = 0;
    g_frame_accum = 0;
  }
  return ok;
}

/* The bootstrap surface main.c created, handed over so it can be released at
 * the right moment. See egl_shim_release_host_surface below. */
static EGLDisplay g_host_display = EGL_NO_DISPLAY;
static EGLSurface g_host_surface = EGL_NO_SURFACE;

void egl_shim_adopt_host_surface(EGLDisplay dpy, EGLSurface surf) {
  g_host_display = dpy;
  g_host_surface = surf;
}

/* Idempotent: safe to call from here and again from egl_deinit. */
void egl_shim_release_host_surface(void) {
  if (g_host_surface == EGL_NO_SURFACE) return;
  /* Unbind before destroying -- a surface that is current is not released. */
  eglMakeCurrent(g_host_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(g_host_display, g_host_surface);
  g_host_surface = EGL_NO_SURFACE;
}

/* The game passes whatever ANativeWindow_fromSurface gave it, which is already
 * nwindowGetDefault(). Tracked here so the swap wrapper and a later resize
 * both know which surface is live.
 *
 * An NWindow backs exactly ONE EGLSurface. main.c already created one on
 * nwindowGetDefault() during bootstrap and left it current, so the game's own
 * eglCreateWindowSurface on the same window was refused and returned
 * EGL_NO_SURFACE -- the game then had nothing to draw into and stalled inside
 * doFrame. The host surface exists only to have a context current while the
 * runtime comes up; once the game wants the window, the host has no further
 * use for it. So hand it over: release ours, then let the game make its own
 * with its own config. */
static EGLSurface nx_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                            EGLNativeWindowType win,
                                            const EGLint *attrib_list) {
  if (!win) {
    debug_log("[egl] CreateWindowSurface with a null window -- substituting "
              "the default\n");
    win = (EGLNativeWindowType)nwindowGetDefault();
  }

  if (g_host_surface != EGL_NO_SURFACE) {
    debug_log("[egl] releasing the host bootstrap surface -- an NWindow backs "
              "only one EGLSurface, and the game wants this window now\n");
    egl_shim_release_host_surface();
  }

  /* Allocate the framebuffer at 1080p, before the surface is created.
   *
   * update_screen_size() has always set nwindowSetCrop to 1920x1080 when
   * docked, but a crop can only select a region of a buffer that already
   * exists -- and libnx's default NWindow is 1280x720, so cropping to 1080p
   * selected a region larger than the buffer and the console went on scaling
   * 720p to the TV. Nothing failed and nothing logged; it just never looked
   * like 1080p.
   *
   * The buffer size is fixed once the surface exists, so it has to be set
   * here. Allocating the docked maximum unconditionally and cropping down is
   * the standard shape for an app whose resolution changes at runtime: the
   * dock transition then costs a crop rather than a surface rebuild, which the
   * game's EGL surface could not survive anyway. */
  nwindowSetDimensions((NWindow *)win, 1920, 1080);
  nwindowSetCrop((NWindow *)win, 0, 0, 1920, 1080);
  debug_log("[egl] window framebuffer allocated at 1920x1080, crop set to the "
            "whole buffer -- the game renders 1080p in handheld and docked "
            "alike, and the console downscales for the handheld panel\n");

  EGLSurface s = eglCreateWindowSurface(dpy, config, win, attrib_list);
  debug_log("[egl] CreateWindowSurface(win=%p) -> %p\n", (void *)win, s);

  /* Say WHY, rather than leaving a bare 0x0 to be guessed at. */
  if (s == EGL_NO_SURFACE) {
    EGLint err = eglGetError();
    const char *name =
        err == EGL_BAD_ALLOC       ? "EGL_BAD_ALLOC (the window is already in use)" :
        err == EGL_BAD_MATCH       ? "EGL_BAD_MATCH (config does not fit the window)" :
        err == EGL_BAD_CONFIG      ? "EGL_BAD_CONFIG" :
        err == EGL_BAD_DISPLAY     ? "EGL_BAD_DISPLAY" :
        err == EGL_BAD_NATIVE_WINDOW ? "EGL_BAD_NATIVE_WINDOW" :
        err == EGL_NOT_INITIALIZED ? "EGL_NOT_INITIALIZED" : "unrecognised";
    debug_log("[egl] *** eglGetError() = 0x%x -- %s ***\n", (unsigned)err, name);
  }

  g_tracked_surface = s;
  g_tracked_display = dpy;
  return s;
}

/* Silk.NET calls eglGetProcAddress for essentially everything, so our
 * interceptions have to be visible through it as well as through dlsym --
 * otherwise the engine resolves the real eglSwapBuffers and the overlay never
 * appears. */
static void *nx_eglGetProcAddress(const char *name) {
  void *ours = egl_shim_lookup(name);
  if (ours) return ours;
  return (void *)eglGetProcAddress(name);
}

/* vsync interval. Horizon presents on vsync regardless, but the engine may set
 * this to 0 expecting to run unlocked; accepting it and doing nothing keeps
 * the timing sane rather than letting the game spin. */
/* Drain and report GL errors.
 *
 * The implementation moved to gl_guard.c so that it can also be called from
 * the point of a suspicious GL call. That matters: a drain which only runs on
 * eglSwapBuffers structurally cannot report an error raised in the frame that
 * crashes, because the swap is never reached. That blind spot is why the
 * last run showed 301 presents and zero GL errors while faulting inside the
 * driver. */
static void report_gl_errors(const char *when) {
  (void)gl_guard_drain_errors(when);
}

static EGLBoolean nx_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
  if (interval < 1) {
    static int logged;
    if (!logged) {
      logged = 1;
      debug_log("[egl] SwapInterval(%d) requested; Horizon presents on vsync "
                "regardless. Clamping to 1.\n", interval);
    }
    interval = 1;
  }
  return eglSwapInterval(dpy, interval);
}

/* ------------------------------------------------------------------------ */

/* Context management, intercepted purely to see it.
 *
 * The crash is inside libdrm_nouveau -- `pushbuf_kref` calling `cli_push_get`,
 * which reads a field off a NULL client. That is the GPU command-submission
 * path, and it ties the two symptoms together: the RSB's "DecompressionTask"
 * with its gpuRead / gpuDecompressed counters really is doing GPU work, and it
 * both produces nothing and then faults in the driver.
 *
 * The classic cause of a NULL client in that path is GL being called on a
 * thread with **no context current**. The decompression is a Task, so it runs
 * on a worker; if the game never makes a context current there -- or tries and
 * fails -- every GL call from it is undefined, which is exactly "decompressed
 * zero bytes, then crashed in the driver".
 *
 * We had no visibility into this at all: eglMakeCurrent and eglCreateContext
 * went straight to mesa. Now every call is logged with the thread that made
 * it, so the next log answers directly whether the worker ever had a context
 * rather than leaving it to be inferred from a driver backtrace. */
static EGLContext nx_eglCreateContext(EGLDisplay dpy, EGLConfig cfg,
                                      EGLContext share, const EGLint *attrs) {
  EGLContext c = eglCreateContext(dpy, cfg, share, attrs);
  debug_log("[egl] CreateContext(share=%p) -> %p  on thread '%s'%s\n",
            (void *)share, (void *)c, threads_self_name(),
            c == EGL_NO_CONTEXT ? "  *** FAILED ***" : "");
  if (c == EGL_NO_CONTEXT)
    debug_log("[egl]   eglGetError() = 0x%x\n", (unsigned)eglGetError());
  return c;
}

static EGLBoolean nx_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                    EGLSurface read, EGLContext ctx) {
  EGLBoolean ok = eglMakeCurrent(dpy, draw, read, ctx);

  /* Only when something CHANGES, plus a periodic count.
   *
   * The last log had 1739 identical lines. The question they were added to
   * answer is now answered -- every one was on 'main', with the same context,
   * and succeeded -- so repeating it is pure noise. What is still worth
   * knowing is a context or thread that differs from the established one, and
   * how often this is being called at all: 1739 times is a lot, and suggests
   * the engine re-binds around every batch. */
  static EGLContext last_ctx; static EGLSurface last_draw;
  static const char *last_thread; static unsigned n_calls;
  const char *self = threads_self_name();
  n_calls++;
  if (!ok || ctx != last_ctx || draw != last_draw || self != last_thread) {
    debug_log("[egl] MakeCurrent(draw=%p read=%p ctx=%p) -> %s  on thread "
              "'%s'  (call %u)\n", (void *)draw, (void *)read, (void *)ctx,
              ok ? "ok" : "FAILED", self, n_calls);
    last_ctx = ctx; last_draw = draw; last_thread = self;
  } else if ((n_calls % 2000) == 0) {
    debug_log("[egl] MakeCurrent called %u times, unchanged\n", n_calls);
  }
  if (!ok)
    debug_log("[egl]   eglGetError() = 0x%x -- GL calls on this thread will "
              "now go to a driver with no context, which is how nouveau ends "
              "up dereferencing a null client\n", (unsigned)eglGetError());
  return ok;
}

void *egl_shim_lookup(const char *symbol) {
  if (!symbol) return NULL;

  /* GL-level interceptions first. dl_shim funnels everything beginning "gl"
   * or "egl" through here, so this is the only hook point needed. */
  void *g = gl_guard_lookup(symbol);
  if (g) return g;

  if (!strcmp(symbol, "eglSwapBuffers"))         return (void *)nx_eglSwapBuffers;
  if (!strcmp(symbol, "eglCreateWindowSurface")) return (void *)nx_eglCreateWindowSurface;
  if (!strcmp(symbol, "eglGetProcAddress"))      return (void *)nx_eglGetProcAddress;
  if (!strcmp(symbol, "eglSwapInterval"))        return (void *)nx_eglSwapInterval;
  if (!strcmp(symbol, "eglMakeCurrent"))         return (void *)nx_eglMakeCurrent;
  if (!strcmp(symbol, "eglCreateContext"))       return (void *)nx_eglCreateContext;
  return NULL;
}

unsigned egl_shim_swap_count(void) { return g_total_swaps; }

EGLSurface egl_shim_surface(void) { return g_tracked_surface; }
EGLDisplay egl_shim_display(void) { return g_tracked_display; }
