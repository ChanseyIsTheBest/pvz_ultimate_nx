/* main.c -- host entry point.
 *
 * This is deliberately staged to match the roadmap. Each stage prints a banner
 * and each one is expected to fail the first several times you run it. Read
 * debug.log top to bottom: the last banner you see is the stage you are on.
 *
 * Stage 3 (runtime init) is where most of the early time goes, and the usual
 * cause of a silent hang there is dl_iterate_phdr returning nothing -- the
 * runtime concludes its own image is not loaded and gives up before any of the
 * other shims run. dl_shim.c logs its first call for exactly this reason.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "android_classes.h"
#include "android_os.h"
#include "android_pm.h"
#include "android_text.h"
#include "android_runtime.h"
#include "dl_shim.h"
#include "aaudio_shim.h"
#include "atomics_check.h"
#include "egl_shim.h"
#include "fileio.h"
#include "icu_probe.h"
#include "ime_shim.h"
#include "input.h"
#include "interop_classes.h"
#include "java_lang.h"
#include "java_net.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "lawn_natives.h"
#include "lawn_register.h"
#include "libc_shim.h"
#include "mem_arena.h"
#include "nx_exception_dump.h"
#include "runtime_glue.h"
#include "so_util.h"
#include "threads.h"
#include "vfs.h"
#include "watchdog.h"
#include "util.h"

/* Two data directories, because this constant was serving two masters.
 *
 * HOST_DATA_DIR is a real path. The log file, the game library and the input
 * configuration are opened through it directly -- nothing translates them --
 * so it must carry the device prefix.
 *
 * ANDROID_DATA_DIR is a string handed to managed code, which parses it as a
 * Unix path and throws on the colon a device prefix would put there.
 *
 * Collapsing these into one constant and giving it the Android form pointed
 * the log and libLawn.Android.so at a location that does not exist, which is
 * why that run produced no log at all: the failure happened before there was
 * anywhere to record it. */
#define HOST_DATA_DIR    "sdmc:/switch/pvzultimate"
#define ANDROID_DATA_DIR "/data/data/com.pvz.ultimate"
/* .NET for Android names the library after the assembly (Lawn.Android), so the
 * separator is a DOT. so_file_load also tries the underscore spelling, since
 * APK-extraction tooling often normalises it. */
#define GAME_SO  HOST_DATA_DIR "/libLawn.Android.so"

static so_module g_lawn;

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLSurface g_surface = EGL_NO_SURFACE;

/* Timing of the game's DoFrame; see the periodic report. */
static u64 g_doframe_last_ns, g_doframe_max_ns;

/* Heap setup lives in mem_arena.c -- it owns __libnx_initheap because the
 * split between newlib's heap and the GC arena is its decision to make. */

/* ---- EGL ---------------------------------------------------------------- */

static int egl_init(void) {
  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY) { debug_log("[egl] no display\n"); return -1; }
  if (!eglInitialize(g_display, NULL, NULL)) { debug_log("[egl] init failed\n"); return -1; }
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig cfg;
  EGLint    n = 0;
  static const EGLint attrs[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  if (!eglChooseConfig(g_display, attrs, &cfg, 1, &n) || n == 0) {
    debug_log("[egl] no matching config\n");
    return -1;
  }

  g_surface = eglCreateWindowSurface(g_display, cfg, (EGLNativeWindowType)nwindowGetDefault(), NULL);
  if (g_surface == EGL_NO_SURFACE) { debug_log("[egl] no surface\n"); return -1; }

  static const EGLint ctxattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  g_context = eglCreateContext(g_display, cfg, EGL_NO_CONTEXT, ctxattrs);
  if (g_context == EGL_NO_CONTEXT) { debug_log("[egl] no context\n"); return -1; }

  eglMakeCurrent(g_display, g_surface, g_surface, g_context);

  /* This surface is a bootstrap only: it exists so there is a current context
   * while the runtime comes up. The game will want the same NWindow, and an
   * NWindow backs only one EGLSurface, so ownership goes to egl_shim, which
   * releases it at the moment the game asks. */
  egl_shim_adopt_host_surface(g_display, g_surface);
  g_surface = EGL_NO_SURFACE;

  debug_log("[egl] ready (bootstrap surface held for the game)\n");
  return 0;
}

static void egl_deinit(void) {
  if (g_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (g_context) eglDestroyContext(g_display, g_context);
  /* No-op if the game already took the window. */
  egl_shim_release_host_surface();
  eglTerminate(g_display);
}

/* ---- import table -------------------------------------------------------
 * The full table is ~257 entries; imports.c is where it belongs. Only the
 * entries that are genuinely NativeAOT-specific live here so they are easy to
 * find. Anything missing traps with its name at first call, so build, run, read
 * the log, add the symbol, repeat -- that loop is the whole of Stage 2. */
extern DynLibFunction g_imports[];
extern size_t         g_imports_count;

/* ---- main --------------------------------------------------------------- */

/* 1080p in both modes, deliberately.
 *
 * This used to follow the operation mode -- 1920x1080 docked, 1280x720
 * handheld -- and that produced the cropped corner. The framebuffer is
 * allocated once at 1920x1080 and cannot be resized afterwards, so in handheld
 * the game was told 1280x720, drew a 720p image into the bottom-left of a 1080p
 * buffer (GL's origin is bottom-left), and the console presented the whole
 * buffer. The crop that was supposed to select the used region never ran
 * either: update_screen_size only acted when the size CHANGED, and 1280x720 is
 * what it was initialised to, so handheld never took the branch.
 *
 * Rendering 1080p in both modes removes the failure rather than patching it.
 * There is no size change, so no crop, no resize, and no branch that can be
 * skipped. Handheld gets 1080p downscaled to a 720p panel, which is
 * supersampling -- sharper than rendering native, not softer.
 *
 * The cost is 2.25x the pixels in handheld. This game is light enough on the
 * GPU for that to be the right trade; if a future one is not, the honest fix is
 * a smaller buffer chosen before the surface is created, not a crop applied
 * after. */
#define SCREEN_W 1920
#define SCREEN_H 1080

static int g_screen_w = SCREEN_W, g_screen_h = SCREEN_H;

static void update_screen_size(void) {
  /* Nothing varies any more. Kept as the single place that would have to change
   * if it ever does, and so the dock transition has an obvious home. */
  if (g_screen_w != SCREEN_W || g_screen_h != SCREEN_H) {
    g_screen_w = SCREEN_W; g_screen_h = SCREEN_H;
    input_set_screen(g_screen_w, g_screen_h);
    lawn_surface_changed(g_screen_w, g_screen_h);
    debug_log("[main] screen now %dx%d\n", g_screen_w, g_screen_h);
  }
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  /* Logging first, before anything that can fail. The previous ordering put
   * socket and romfs init ahead of it, so any failure there produced a crash
   * with no log at all -- which is exactly the situation you cannot debug.
   *
   * Neither is needed: every socket call is stubbed to fail cleanly, and
   * assets live on the SD card rather than in romfs. Both were pure risk. */
  log_init(HOST_DATA_DIR);
  debug_log("[main] alive; libnx startup completed\n");

  /* Print the host load base unconditionally, in every log, at the top.
   *
   * Every address in a crash dump that is not inside a .so we loaded is
   * relative to this. Without it those addresses had to be symbolized by
   * guessing a base and checking whether the instruction at the resulting
   * offset looked like it could fault -- which produced a confident wrong
   * answer and cost a full round. It is one line and it removes the whole
   * class of mistake. */
  debug_log("[main] host image at %p .. %p; symbolize any host+N in this log "
            "with: addr2line -e pvzultimate_nx.elf -f -C -i 0xN\n",
            (void *)nx_host_base(),
            (void *)(nx_host_base() + nx_host_span()));
  runtime_env_dump();

  if (arena_init() != 0)
    fatal_error("no memory arena. Relaunch via a title override "
                "(hold R while starting an installed game).");
  threads_init();
  /* 45 s rather than 90.
   *
   * The break is what produces the Atmosphere creport, and the creport is the
   * only thing that gives a stack for every thread -- which is exactly what
   * the last few hangs needed and did not have, because every log so far has
   * been captured before the 90 s mark. 45 s is still far longer than any
   * legitimate load reached so far (the slowest real step has been under a
   * second), and watchdog_disarm exists for anything genuinely slower. */
  watchdog_init(15, 45);
  libc_shim_init();
  vfs_init();

  debug_log("\n=== STAGE 1: map and relocate ===\n");
  watchdog_checkpoint("Stage 1: map and relocate");
  if (so_file_load(&g_lawn, GAME_SO) != 0)
    fatal_error("could not load %s\nCheck the file is present and is the "
                "arm64-v8a build.", GAME_SO);
  so_relocate(&g_lawn, g_imports, g_imports_count);
  so_flush_caches(&g_lawn);

  debug_log("\n=== STAGE 2: JNI environment + init_array ===\n");
  watchdog_checkpoint("Stage 2: JNI env + init_array");
  jni_fake_init();
  java_lang_init();
  java_net_init();
  ime_init();
  android_os_init();
  android_runtime_init();
  interop_classes_init();
  android_classes_init();
  android_pm_init();
  android_text_init();
  lawn_natives_init();

  if (egl_init() != 0) fatal_error("EGL init failed");

  watchdog_checkpoint("Stage 2: running init_array");
  so_initialize(&g_lawn);   /* runs the 17 init_array entries */

  debug_log("\n=== STAGE 3: runtime init ===\n");
  watchdog_checkpoint("Stage 3: runtime init");
  heap_report();
  typedef jint (*jni_onload_t)(JavaVM *, void *);
  jni_onload_t jni_onload = (jni_onload_t)so_symbol(&g_lawn, "JNI_OnLoad");
  if (!jni_onload) fatal_error("JNI_OnLoad not found in the image");

  watchdog_checkpoint("Stage 3: inside JNI_OnLoad");
  jint ver = jni_onload(jni_get_vm(), NULL);
  debug_log("[main] JNI_OnLoad returned 0x%x\n", ver);

  if (ver == 0 || ver == -1)
    fatal_error("JNI_OnLoad rejected our JavaVM (returned 0x%x)", ver);

  /* NOT a gate here, deliberately.
   *
   * registerNativeMembers is not registered by JNI_OnLoad -- it is registered
   * when the managed runtime starts, which is JavaInteropRuntime.init below.
   * Aborting on its absence at this point failed a run in which JNI_OnLoad had
   * in fact succeeded and returned JNI_VERSION_1_6. Report the state and carry
   * on; the meaningful check is after init. */
  interop_check_bootstrap();

  typedef void (*ji_init_t)(JNIEnv *, jclass, jobject, jstring, jstring, jstring);
  ji_init_t ji_init = (ji_init_t)so_symbol(
      &g_lawn, "Java_net_dot_jni_nativeaot_JavaInteropRuntime_init");
  if (!ji_init) fatal_error("JavaInteropRuntime_init not found");

  /* Signature taken from classes.dex, not guessed:
   *     init(ClassLoader, String, String, String)
   * so the native is (JNIEnv*, jclass, ClassLoader, String, String, String).
   *
   * The ClassLoader is the one that matters -- Java.Interop resolves types
   * through it, and NULL leaves it with nothing to resolve against. The three
   * strings are all path-like; the data root is a defensible answer for each
   * and is safer than NULL, since a runtime that measures the string would
   * fault on a null pointer but not on a valid one. If init misbehaves, these
   * are the first thing to vary -- which is why they are logged. */
  JNIEnv *env = jni_get_env();
  jobject loader = java_lang_classloader();
  jstring p1 = (*env)->NewStringUTF(env, ANDROID_DATA_DIR);
  jstring p2 = (*env)->NewStringUTF(env, ANDROID_DATA_DIR);
  jstring p3 = (*env)->NewStringUTF(env, ANDROID_DATA_DIR);

  debug_log("[main] JavaInteropRuntime.init(loader=%p, \"%s\" x3)\n",
            loader, ANDROID_DATA_DIR);
  watchdog_checkpoint("Stage 3: inside JavaInteropRuntime.init");
  ji_init(env, NULL, loader, p1, p2, p3);
  debug_log("[main] JavaInteropRuntime.init returned\n");

  /* Now it means something. Still not fatal: Stage 4 tests for the thing we
   * actually need (n_doFrame), and a missing registerNativeMembers with a
   * present n_doFrame would be a puzzle worth seeing rather than an abort. */
  if (interop_check_bootstrap() != 0)
    debug_log("[interop] registerNativeMembers still absent after init -- "
              "if Stage 4 also fails, this is why.\n");

  /* Construct the managed peers before checking what is bound.
   *
   * Registration is a side effect of a type's first peer being created, and
   * nothing else in this port creates one -- on Android that is the framework's
   * job. Doing it here is the step that turns "0 of 19 natives bound" into a
   * runnable game, and it is only possible now because ManagedPeer.construct
   * became a real function pointer once thunk allocation started working. */
  debug_log("\n=== STAGE 4: waiting for RegisterNatives ===\n");
  watchdog_checkpoint("Stage 4: constructing managed peers");

  interop_construct_peer(env, lawn_activity_instance(), "()V", NULL, 0);

  jobject sv_args[1] = { android_get_context() };
  interop_construct_peer(env, lawn_surfaceview_instance(),
                         "(Landroid/content/Context;)V", sv_args, 1);

  /* Nothing else does this. Java.Interop registers a type's natives from the
   * generated Java <clinit>, which never runs here -- constructing the peers
   * got managed code executing but left all nineteen unbound. */
  lawn_register_natives(env);

  watchdog_checkpoint("Stage 4: RegisterNatives");
  lawn_natives_report();
  if (!lawn_natives_ready())
    fatal_error("n_doFrame was never registered.\n"
                "Look for 'RegisterNatives ... DROPPED' lines above and declare "
                "those methods in lawn_natives.c.");

  debug_log("\n=== STAGE 5: first frame ===\n");
  watchdog_checkpoint("Stage 5: first frame");
  heap_report();
  /* Android's real order, which this did not follow.
   *
   *     onCreate -> onStart -> onResume -> surfaceCreated -> surfaceChanged
   *              -> onWindowFocusChanged
   *
   * The surface callbacks used to come BEFORE onResume here, and that is not a
   * cosmetic difference. MonoGame's view splits startup across the two: the
   * surface callback creates the GL context and the resume marks the view
   * runnable, and whichever arrives second is what actually starts rendering.
   * Deliver them in the wrong order and each one looks at the other's state
   * before it has been set, so neither starts the loop -- which is exactly the
   * end state we keep landing in: surface up, doFrame ticking, nothing drawn,
   * and the game pausing its own audio because it does not believe it is
   * running.
   *
   * onStart is sent too if the game bound it; it is part of the sequence and
   * costs nothing. lawn_activity_lifecycle already refuses anything that is
   * not ()V, so an absent or differently-shaped method is a logged no-op. */
  lawn_activity_create();
  lawn_activity_lifecycle("n_onStart");
  lawn_activity_lifecycle("n_onResume");

  watchdog_checkpoint("Stage 5: surfaceCreated");
  lawn_surface_created();
  lawn_surface_changed(g_screen_w, g_screen_h);

  /* Focus last, as Android does -- it is what tells the game it is in front,
   * and it must arrive after the surface exists. */
  lawn_activity_focus(1);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  input_init(g_screen_w, g_screen_h, HOST_DATA_DIR);
  debug_log("[input] controls: touchscreen (handheld) | '+' cursor on/off | "
            "'-' gyro | left stick moves | A/ZL/ZR tap | L or R recenter | "
            "D-pad up/down sensitivity | "
            "L+R+Plus exits the host\n");

  /* Before the loop, and before the game ever asks for audio: creating this
   * thread from inside the engine's requestStart is what hung the previous
   * build. See aaudio_shim.c. */
  /* Before the frame loop and before anything can be blamed on the game: does
   * a compare-and-swap loop actually complete on each kind of memory the
   * runtime keeps lock state in? See atomics_check.c -- the creport showed
   * both game threads spinning in lock acquisition, and this either
   * eliminates the mapping as the cause or names the region that is broken. */
  /* This thread drains the posted-Runnable queue, so it is Android's "UI
   * thread"; runOnUiThread has to know that to decide between running inline
   * and queueing. */
  android_os_mark_looper_thread();

  atomics_check_run();

  aaudio_shim_init();

  debug_log("\n=== STAGE 6: frame loop (watching GC suspension) ===\n");
  unsigned frames = 0;
  unsigned idle_frames = 0;

  while (appletMainLoop()) {
    padUpdate(&pad);

    /* Exit combo: L + R + Plus, held together.
     *
     * It used to be Plus alone, which nx_pointer also claims -- '+' toggles
     * the cursor. Leaving both on the same button would have made the cursor
     * untoggleable and the exit accidental, and it is the kind of collision
     * that reads as "the cursor toggle does not work" rather than as a
     * conflict. nx_pointer owns Plus and Minus; the host takes a combo.
     *
     * Held rather than pressed, so the order the three go down does not
     * matter. nx_pointer will also see this frame's Plus and toggle the
     * cursor, and L/R will recenter it -- both harmless one frame before
     * teardown. */
    const u64 quit_combo = HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus;
    if ((padGetButtons(&pad) & quit_combo) == quit_combo) {
      debug_log("[input] L+R+Plus held -- exiting\n");
      break;
    }

    update_screen_size();

    /* Runs the software keyboard if the game asked for one.
     *
     * Deliberately here, at the TOP of the loop and outside any managed call:
     * swkbdShow blocks until the user is finished, and showSoftInput is invoked
     * from inside DoFrame. Blocking there would hold a managed->native
     * transition open for as long as someone takes to type. A no-op when
     * nothing is queued. */
    ime_pump();

    input_update();
    input_handle_buttons(padGetButtonsDown(&pad), padGetButtonsUp(&pad));

    /* doFrame ticks the game AND swaps, because on Android the Choreographer
     * callback owns the frame. The cursor overlay is drawn inside our
     * eglSwapBuffers interception (egl_shim.c) rather than here -- drawing it
     * after this call would put it underneath the next frame. */
    /* One checkpoint per phase, because the label IS the diagnosis.
     *
     * There used to be a single "inside doFrame" set here, before the drain --
     * so a hang anywhere in a posted Runnable was reported as a hang in
     * doFrame. The whole of LawnApp's construction runs in a posted Runnable,
     * which is exactly where the last two hangs were, and the report named the
     * wrong phase both times. */
    watchdog_checkpoint("frame loop: draining posted Runnables");
    /* Posted Runnables run here because this is the looper's thread -- the
     * same guarantee Android gives, and the reason post() queues instead of
     * running inline. */
    android_os_run_posted(env);

    aaudio_shim_pump();   /* empty; see aaudio_shim.c */

    unsigned swaps_before = egl_shim_swap_count();
    u64 df_start = armTicksToNs(armGetSystemTick());
    watchdog_checkpoint("frame loop: inside doFrame");
    lawn_do_frame((int64_t)df_start);
    g_doframe_last_ns = armTicksToNs(armGetSystemTick()) - df_start;
    if (g_doframe_last_ns > g_doframe_max_ns) g_doframe_max_ns = g_doframe_last_ns;
    watchdog_checkpoint("frame loop: between frames");
    watchdog_beat();

    /* Pace the loop ourselves when the game did not present.
     *
     * doFrame both ticks and swaps, so vsync normally throttles us. A frame
     * that renders nothing -- during loading, or before the game starts
     * drawing at all -- returns immediately, and without this the loop spins
     * at full speed on a console with passive cooling. It also makes a
     * not-yet-drawing game indistinguishable from a hang.
     *
     * Roughly one frame at 60 Hz: enough to idle politely, short enough not to
     * add latency once real rendering starts. */
    if (egl_shim_swap_count() == swaps_before) {
      svcSleepThread(16000000ull);
      if (++idle_frames == 300)
        debug_log("[main] 300 consecutive frames without a present -- the game "
                  "is ticking but not drawing\n");
    } else {
      idle_frames = 0;
    }

    /* Follow the console's real focus state.
     *
     * Now that the game acts on window focus, it has to be told the truth
     * about it: leaving it permanently focused means it keeps rendering and
     * holding audio while the user is in the home menu or the console is
     * suspended. appletGetFocusState is the Horizon equivalent of the Android
     * callback, so it is wired straight through. */
    {
      static int focused = 1;
      int now_focused = (appletGetFocusState() == AppletFocusState_InFocus);
      if (now_focused != focused) {
        focused = now_focused;
        lawn_activity_focus(focused);
      }
    }

    ++frames;

    /* Every 300 frames (~5 s), not 600.
     *
     * The previous version reported every 600 frames AND only dumped threads
     * when the delta was zero -- so the earliest possible dump was the SECOND
     * report, twenty seconds in. The capture ended at ten and the diagnostic
     * never ran. A diagnostic that needs the session to outlast it is not a
     * diagnostic. */
    if (frames % 300 == 0) {
      /* How long the game's DoFrame actually takes.
       *
       * t6 sits in a pump waiting for work that nothing produces, and the only
       * plausible producer is the game's own update loop -- which runs out of
       * DoFrame. But DoFrame is gated inside the game on a flag its Start()
       * sets, and if that gate is closed the call returns immediately and does
       * nothing at all. From out here the two cases look identical: we call
       * it every frame either way.
       *
       * Timing separates them. A DoFrame that early-outs costs tens of
       * nanoseconds; one that updates and renders costs milliseconds. */
      debug_log("[main] doFrame: last %llu us, max %llu us over the last %u "
                "frames -- microseconds means it is running, ~0 means it is "
                "gated off and doing nothing\n",
                (unsigned long long)(g_doframe_last_ns / 1000),
                (unsigned long long)(g_doframe_max_ns / 1000), 300u);
      g_doframe_max_ns = 0;

      unsigned swaps = egl_shim_swap_count();
      static unsigned last_swaps;
      debug_log("[main] %u frames, %u presents total (%u since last report)\n",
                frames, swaps, swaps - last_swaps);

      /* Dumped when nothing was presented since the last report, and ALSO
       * whenever the total is still tiny -- one present in five seconds is a
       * game that is not running, and waiting for a zero delta to prove it
       * just costs another report cycle.
       *
       * The stall report prints the same thing, but a stall only fires when
       * the frame loop stops beating and it does not stop: the loop ticks at
       * full speed while something else is wedged, so the report that would
       * name it never runs. The game's own threads have never had any
       * diagnostics on them at all. */
      /* Only when NOTHING was presented since the last report.
       *
       * This used to also fire while the total was under ten, to get a dump
       * out early -- which made sense while nothing rendered at all. It is
       * actively harmful now that the render gate is open: the dump suspends
       * every thread to read its context, and doing that to a game that IS
       * drawing, merely because it has not reached ten frames yet, is a
       * needless stall in the exact code path we just spent rounds unblocking.
       * No presents in a whole report window is the honest test for stuck. */
      if (swaps == last_swaps) {
        debug_log("[main] no presents in the last report window -- "
                  "thread states:\n");
        watchdog_report_calls();
        threads_report_state();
      }
      last_swaps = swaps;
    }

    if (frames % 600 == 0) {
      gc_watch_report();
      arena_report();
      unsigned adds, clears;
      interop_peer_stats(&adds, &clears);
      jniref_dump_stats();
      debug_log("[interop] peer refs: %u added, %u cleared\n", adds, clears);
    }
  }

  debug_log("\n=== shutdown ===\n");
  icu_probe_report();
  gc_watch_report();
  arena_report();
  threads_report();
  watchdog_shutdown();
  input_shutdown();
  lawn_activity_lifecycle("n_onPause");

  /* Drain anything onPause scheduled.
   *
   * android_os_run_posted only runs inside the frame loop, so work the game
   * posts while shutting down -- a profile flush is exactly the kind of thing
   * that gets posted -- would be queued and then dropped on the floor. A few
   * passes with a short wait between them costs nothing and is the difference
   * between a save happening and silently not. */
  for (int i = 0; i < 30; i++) {
    android_os_run_posted(jni_get_env());
    svcSleepThread(10000000LL);            /* 10 ms */
  }
  lawn_surface_destroyed();
  lawn_activity_lifecycle("n_onDestroy");

  /* Last chance for anything the game changed and never committed. */
  android_prefs_flush();

  egl_deinit();
  log_shutdown();
  return 0;
}
