/* aaudio_shim.c -- enough AAudio to fail the way the game expects.
 *
 * The problem this solves:
 *
 *   [dl] dlopen(libaaudio.so) -> refused on purpose (forcing fallback path)
 *   [Lawn] Android Surface Initialization Error:
 *     System.DllNotFoundException: DllNotFound_Linux, libaaudio.so
 *       at Lawn.AndroidNativeBackend.AAudio.AAudio_createStreamBuilder
 *       at Sexy.AndroidNativeAudioEngine.CreateRenderer
 *       at Sexy.AndroidNativeAudioEngine..ctor
 *       at Sexy.AndroidNativePlatformAbility.CreateSoundManager
 *       at Sexy.SexyAppBase..ctor  ->  Lawn.LawnApp..ctor
 *
 * dl_shim refused libaaudio on the theory that the engine would fall back to
 * OpenSL ES. It does not. `AndroidNativeAudioEngine` calls AAudio directly and
 * nothing catches the failure, so refusing the load threw a
 * DllNotFoundException out of a P/Invoke and took the whole of LawnApp's
 * construction with it. There is no app object, which is why the loop ticks
 * forever and nothing draws.
 *
 * The distinction that matters: a missing *library* is an exception thrown
 * from managed code that never expected it, while a failing *device* is an
 * error code the audio engine was written to handle. So the library resolves,
 * and the failure moves to where AAudio's own contract puts it.
 *
 * A first version failed the open, on the reasoning that openStream is the one
 * call every AAudio user must check. The engine checks it and then throws:
 *
 *   InvalidOperationException: AAudioStreamBuilder_openStream failed: -881
 *
 * -- still out through SexyAppBase..ctor, still no app object. This engine has
 * no soundless path at all; it treats any audio failure as fatal. So the
 * stream has to open, start, and behave like a working device.
 *
 * It therefore reports success and keeps a real state machine, so
 * getState() answers consistently with whatever was last requested rather than
 * with a constant. Nothing is audible: samples are consumed and discarded.
 *
 * AAudio has two drive models and this serves both, because which one the
 * engine uses could not be settled from the binary:
 *
 *   - blocking write. AAudioStream_write consumes the buffer and sleeps for
 *     exactly its duration, so the engine's writer thread is paced by the same
 *     clock a real device would pace it by. Returning immediately would let
 *     that thread spin.
 *
 *   - data callback. The engine registers a function AAudio would call from
 *     its own audio thread, and this calls it from a thread of its own, paced
 *     by sleeping a chunk's worth of real time between calls. See the long
 *     note above audio_thread_main for why it is not the frame loop's thread.
 *
 * Real output means feeding libnx `audout` from the same samples this
 * discards. The builder already records the format, sample rate, channel count
 * and callback pointer such an implementation would need.
 */

#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "aaudio_shim.h"
#include "watchdog.h"
#include "threads.h"
#include "util.h"

/* From AAudio/NDK. Errors are negative; AAUDIO_OK is 0. */
#define AAUDIO_OK                      0
#define AAUDIO_ERROR_NULL           (-886)
#define AAUDIO_ERROR_NO_SERVICE     (-881)

/* AAudio stream states, for getState. */
#define AAUDIO_STREAM_STATE_UNINITIALIZED 0
#define AAUDIO_STREAM_STATE_OPEN          2
#define AAUDIO_STREAM_STATE_STARTED       4
#define AAUDIO_STREAM_STATE_PAUSED        6
#define AAUDIO_STREAM_STATE_STOPPED      10
#define AAUDIO_STREAM_STATE_CLOSED       12

/* Data callback return values. */
#define AAUDIO_CALLBACK_RESULT_CONTINUE   0
#define AAUDIO_CALLBACK_RESULT_STOP       1

/* Formats, for the bytes-per-frame calculation. */
#define AAUDIO_FORMAT_PCM_I16             1
#define AAUDIO_FORMAT_PCM_FLOAT           2
#define AAUDIO_FORMAT_PCM_I24_PACKED      3
#define AAUDIO_FORMAT_PCM_I32             4

typedef struct {
  int32_t  sample_rate;
  int32_t  channel_count;
  int32_t  format;
  int32_t  direction;
  int32_t  sharing_mode;
  int32_t  performance_mode;
  int32_t  usage;
  int32_t  content_type;
  int32_t  capture_policy;
  int32_t  frames_per_data_callback;
  void    *data_callback;
  void    *error_callback;
  void    *user_data;
} AAudioBuilder;

/* One builder is enough: the engine creates it, configures it, tries to open,
 * fails, and deletes it. Static rather than malloc'd so a delete followed by a
 * stray setter cannot touch freed memory. */
static AAudioBuilder g_builder;
static int           g_builder_live;

static int32_t nx_AAudio_createStreamBuilder(AAudioBuilder **out) {
  static int once;
  if (!once) {
    once = 1;
    debug_log("[aaudio] the game is opening an audio stream. This shim "
              "resolves the library so the P/Invoke binds, then reports no "
              "device at openStream -- an error the engine handles, rather "
              "than a missing-library exception it does not.\n");
  }
  if (!out) return AAUDIO_ERROR_NULL;
  memset(&g_builder, 0, sizeof(g_builder));
  g_builder.sample_rate   = 48000;
  g_builder.channel_count = 2;
  g_builder_live = 1;
  *out = &g_builder;
  return AAUDIO_OK;
}

/* Setters return void in the NDK; the engine calls them for effect only. */
#define SETTER_I32(name, field)                                               \
  static void nx_##name(AAudioBuilder *b, int32_t v) {                        \
    if (b) b->field = v;                                                      \
  }

SETTER_I32(AAudioStreamBuilder_setSampleRate,           sample_rate)
SETTER_I32(AAudioStreamBuilder_setChannelCount,         channel_count)
SETTER_I32(AAudioStreamBuilder_setFormat,               format)
SETTER_I32(AAudioStreamBuilder_setDirection,            direction)
SETTER_I32(AAudioStreamBuilder_setSharingMode,          sharing_mode)
SETTER_I32(AAudioStreamBuilder_setPerformanceMode,      performance_mode)
SETTER_I32(AAudioStreamBuilder_setUsage,                usage)
SETTER_I32(AAudioStreamBuilder_setContentType,          content_type)
SETTER_I32(AAudioStreamBuilder_setAllowedCapturePolicy, capture_policy)
SETTER_I32(AAudioStreamBuilder_setFramesPerDataCallback, frames_per_data_callback)

static void nx_AAudioStreamBuilder_setDataCallback(AAudioBuilder *b, void *cb,
                                                   void *userData) {
  if (!b) return;
  b->data_callback = cb;
  b->user_data     = userData;
}

static void nx_AAudioStreamBuilder_setErrorCallback(AAudioBuilder *b, void *cb,
                                                    void *userData) {
  if (!b) return;
  b->error_callback = cb;
  if (userData) b->user_data = userData;
}

/* The open stream. Static, so a stray call after close cannot touch freed
 * memory -- the same reasoning as the builder. */

/* Declared here because requestStart, further down, reports whether the
 * callback thread exists. */
static void        *g_audio_thread;
static volatile int g_audio_run;

/* Set by audio_out_open() below. Declared up here because openStream reports
 * whether the port is going to be audible, and that log line is the first
 * place anyone looks when it is not. */
static int g_aout_live;

typedef struct {
  AAudioBuilder cfg;
  /* volatile: the audio thread spins on this while the game's thread writes
   * it. Without it the compiler is entitled to hoist the load out of the idle
   * loop, and the thread would never notice the stream starting -- a hang that
   * would appear and disappear with optimisation level. */
  volatile int32_t state;
} AAudioStreamObj;

static AAudioStreamObj g_stream;

static int32_t bytes_per_frame(const AAudioBuilder *b) {
  int32_t ch = b->channel_count > 0 ? b->channel_count : 2;
  switch (b->format) {
    case AAUDIO_FORMAT_PCM_FLOAT:      return ch * 4;
    case AAUDIO_FORMAT_PCM_I32:        return ch * 4;
    case AAUDIO_FORMAT_PCM_I24_PACKED: return ch * 3;
    case AAUDIO_FORMAT_PCM_I16:        /* fall through */
    default:                           return ch * 2;
  }
}

/* Frames handed to the data callback in one go. */
static int32_t callback_frames(const AAudioBuilder *b) {
  int32_t n = b->frames_per_data_callback > 0 ? b->frames_per_data_callback
                                              : 192;   /* 4 ms at 48 kHz */
  if (n > 2048) n = 2048;
  return n;
}

static int32_t nx_AAudioStreamBuilder_openStream(AAudioBuilder *b, void **stream) {
  static int once;
  if (stream) *stream = NULL;
  if (!b || !stream) return AAUDIO_ERROR_NULL;

  /* Order matters: the audio thread reads cfg only once it sees STARTED, so
   * the configuration has to be in place before any state that lets it look.
   * The barrier stops the compiler reordering the two. */
  g_stream.state = AAUDIO_STREAM_STATE_UNINITIALIZED;
  memset(&g_stream.cfg, 0, sizeof(g_stream.cfg));
  g_stream.cfg = *b;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  g_stream.state = AAUDIO_STREAM_STATE_OPEN;

  if (!once) {
    once = 1;
    debug_log("[aaudio] openStream(%d Hz, %d ch, format %d, %d B/frame) -> OK. "
              "%s\n",
              (int)b->sample_rate, (int)b->channel_count, (int)b->format,
              (int)bytes_per_frame(b),
              g_aout_live ? "Samples go to the Switch audio out."
                          : "Audio out is NOT open -- samples are consumed and "
                            "discarded, so this will be silent.");
    debug_log("[aaudio] drive model: %s\n",
              b->data_callback ? "DATA CALLBACK -- served by the shim's callback thread"
                               : "blocking write -- paced inside AAudioStream_write");
  }
  *stream = &g_stream;
  return AAUDIO_OK;
}

static int32_t nx_AAudioStreamBuilder_delete(AAudioBuilder *b) {
  (void)b;
  g_builder_live = 0;
  return AAUDIO_OK;
}

/* Stream functions.
 *
 * The stream opens now, so these are on the live path rather than unreachable.
 * Each one moves the state machine, so a later getState() answers consistently
 * with what was last requested.
 */
static int32_t nx_AAudioStream_requestStart(void *s) {
  (void)s;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  g_stream.state = AAUDIO_STREAM_STATE_STARTED;
  debug_log("[aaudio] requestStart -- the callback thread will pick it up%s\n",
            g_audio_thread ? "" : " (BUT THERE IS NO CALLBACK THREAD)");
  return AAUDIO_OK;
}
static int32_t nx_AAudioStream_requestPause(void *s) {
  (void)s;
  debug_log("[aaudio] requestPause -- the callback thread will idle\n");
  /* The game pauses its own audio immediately after finishing surface setup,
   * and then stops drawing. Whatever decides that is the thing to find, and
   * this is the moment it decides -- so snapshot every thread here rather than
   * waiting for the next timed report several seconds later, by which point
   * the threads have moved on. */
  debug_log("[aaudio] thread states at the moment of the pause:\n");
  watchdog_report_calls();
  threads_report_state();
  g_stream.state = AAUDIO_STREAM_STATE_PAUSED;
  return AAUDIO_OK;
}
static int32_t nx_AAudioStream_requestStop(void *s) {
  (void)s;
  debug_log("[aaudio] requestStop -- the callback thread will idle\n");
  g_stream.state = AAUDIO_STREAM_STATE_STOPPED;
  return AAUDIO_OK;
}
static int32_t nx_AAudioStream_close(void *s) {
  (void)s;
  debug_log("[aaudio] close\n");
  g_stream.state = AAUDIO_STREAM_STATE_CLOSED;
  /* The thread is deliberately NOT torn down here. It costs nothing to leave
   * idling, and killing it would leave a later open with no callback thread
   * and no way to make one -- aaudio_shim_init runs once, before the loop. */
  return AAUDIO_OK;
}

/* Consume and discard, then sleep for exactly the duration just consumed.
 *
 * The sleep is the point. A real device paces the writer by taking the samples
 * at the sample rate; returning immediately would let the engine's writer
 * thread produce audio as fast as the CPU allows and spin a core. This is a
 * native call, so a managed writer thread is in preemptive mode for its
 * duration and the GC is not held up by it. */
static int32_t nx_AAudioStream_write(void *s, const void *buf, int32_t frames,
                                     int64_t timeoutNanos) {
  (void)s; (void)buf; (void)timeoutNanos;
  static int once;
  if (!once) {
    once = 1;
    debug_log("[aaudio] the engine is driving audio with blocking writes "
              "(%d frames per call)\n", (int)frames);
  }
  if (frames <= 0) return 0;

  int32_t rate = g_stream.cfg.sample_rate > 0 ? g_stream.cfg.sample_rate : 48000;
  svcSleepThread((int64_t)frames * 1000000000LL / rate);
  return frames;
}

static int32_t nx_AAudioStream_getState(void *s) {
  (void)s; return g_stream.state;
}
static int32_t nx_AAudioStream_setBufferSizeInFrames(void *s, int32_t n) {
  (void)s;
  return n > 0 ? n : AAUDIO_ERROR_NULL;   /* echo the granted size, as AAudio does */
}

/* ------------------------------------------------------------------------ */
/* The data callback, on its own thread                                      */
/* ------------------------------------------------------------------------ */
/*
 * This ran from the frame loop in the previous version, to avoid introducing a
 * thread that enters managed code. That was the wrong trade and the run that
 * followed showed why: the game hung with the log stopping dead, and the
 * callback is the one place a hang could propagate that far. On a real device
 * AAudio calls this from a dedicated audio thread, so a callback that blocks
 * -- waiting on the decoder, on a mixer lock, on buffer space -- costs audio a
 * glitch. Called from the frame loop, the same block freezes the entire game,
 * because the frame loop is also what pumps posted Runnables and drives
 * doFrame.
 *
 * The reason for avoiding a thread has also expired. The game itself now
 * creates managed threads -- 'Lawn music deco' is one -- so the runtime is
 * already handling more than one, and the callback is invoked through
 * pthread_create_fake's own trampoline rather than a bare threadCreate, so
 * this thread is set up exactly like every other thread the game makes:
 * bionic TLS installed, tl_self set, TLS destructors run on exit. That last
 * one matters -- NativeAOT registers a destructor to tear down its managed
 * thread context, and skipping it would leave the runtime believing the
 * thread is still attached.
 *
 * Priority stays at the game's 0x2C rather than the realtime priority a true
 * audio thread would use: nothing is audible, so there is nothing to protect
 * from underrun, and running above the frame loop would only let a spin here
 * starve the game.
 */

/* ------------------------------------------------------------------------ */
/* Switch audio out                                                          */
/* ------------------------------------------------------------------------ */

/* The shim used to consume the engine's samples and throw them away, which is
 * why the port was silent. audout is the right service for this: it is fixed at
 * 48000 Hz, 2 channels, PCM16, and the engine opens
 *
 *     [aaudio] openStream(48000 Hz, 2 ch, format 1, 4 B/frame)
 *
 * -- an exact match, so in the normal case the callback's output is handed
 * straight through with no conversion and no resampling. The conversions below
 * exist for the cases where it is not.
 *
 * Buffers must be 0x1000-aligned in both address and size. Each holds enough
 * for one maximum-size callback chunk (2048 frames) so a large chunk can never
 * fail to fit, but is submitted once about 21 ms has accumulated, which keeps
 * latency near 4 x 21 ms rather than 4 x 43 ms. */
#define AOUT_BUFFERS   4
#define AOUT_BUF_BYTES 0x2000        /* headroom for one max chunk           */
#define AOUT_SUBMIT_AT 0x1000        /* ~21 ms of stereo PCM16 at 48 kHz     */

static AudioOutBuffer  g_aout[AOUT_BUFFERS];
static AudioOutBuffer *g_aout_free[AOUT_BUFFERS];
static int             g_aout_nfree;
static void           *g_aout_raw;          /* the unaligned allocation      */
static AudioOutBuffer *g_aout_fill;         /* buffer being filled           */
static size_t          g_aout_used;         /* bytes written into it         */

static void audio_out_open(void) {
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    debug_log("[aaudio] audoutInitialize failed (0x%x) -- the port stays "
              "silent, but the engine is unaffected\n", (unsigned)rc);
    return;
  }
  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    debug_log("[aaudio] audoutStartAudioOut failed (0x%x) -- staying silent\n",
              (unsigned)rc);
    audoutExit();
    return;
  }

  /* Aligned by hand rather than with memalign: one malloc plus arithmetic
   * depends on nothing beyond newlib's core, and this port has been bitten
   * before by assuming a libc function is present. */
  size_t total = (size_t)AOUT_BUF_BYTES * AOUT_BUFFERS;
  g_aout_raw = malloc(total + 0x1000);
  if (!g_aout_raw) {
    debug_log("[aaudio] could not allocate %zu bytes of mix buffer -- staying "
              "silent\n", total);
    audoutStopAudioOut();
    audoutExit();
    return;
  }
  memset(g_aout_raw, 0, total + 0x1000);

  unsigned char *base =
      (unsigned char *)(((uintptr_t)g_aout_raw + 0xFFF) & ~(uintptr_t)0xFFF);

  for (int i = 0; i < AOUT_BUFFERS; i++) {
    g_aout[i].next        = NULL;
    g_aout[i].buffer      = base + (size_t)i * AOUT_BUF_BYTES;
    g_aout[i].buffer_size = AOUT_BUF_BYTES;
    g_aout[i].data_size   = 0;
    g_aout[i].data_offset = 0;
    g_aout_free[i]        = &g_aout[i];
  }
  g_aout_nfree = AOUT_BUFFERS;
  g_aout_live  = 1;

  debug_log("[aaudio] audio out open: %u Hz, %u ch, PCM format %d; "
            "%d buffers of %d bytes (~%d ms of latency)\n",
            (unsigned)audoutGetSampleRate(), (unsigned)audoutGetChannelCount(),
            (int)audoutGetPcmFormat(), AOUT_BUFFERS, AOUT_SUBMIT_AT,
            (AOUT_BUFFERS * AOUT_SUBMIT_AT) / (48 * 4));
}

/* Hand released buffers back to the free list. Blocking here is what paces the
 * whole loop once audio is live -- it is the device consuming at the sample
 * rate, which is exactly the thing the old svcSleepThread was imitating. */
static void audio_out_reclaim(u64 timeout_ns) {
  AudioOutBuffer *rel = NULL;
  u32 n = 0;
  if (R_FAILED(audoutWaitPlayFinish(&rel, &n, timeout_ns))) return;
  if (n && rel && g_aout_nfree < AOUT_BUFFERS) g_aout_free[g_aout_nfree++] = rel;

  /* More than one can come back from a single wake-up. */
  for (;;) {
    rel = NULL; n = 0;
    if (R_FAILED(audoutGetReleasedAudioOutBuffer(&rel, &n))) break;
    if (!n || !rel || g_aout_nfree >= AOUT_BUFFERS) break;
    g_aout_free[g_aout_nfree++] = rel;
  }
}

/* Peak level since the last heartbeat.
 *
 * Worth its handful of instructions: "the engine is mixing silence" and "we are
 * failing to play what it mixes" are the two explanations for a silent port and
 * they look identical from outside. A peak of 0 while callbacks run means the
 * problem is upstream of this file -- the engine's own volume, or a mixer with
 * nothing loaded -- and no amount of work here would help. */
static int32_t g_peak;

static void note_peak(const int16_t *p, size_t frames) {
  int32_t peak = g_peak;
  for (size_t i = 0; i < frames * 2; i++) {
    int32_t v = p[i];
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  g_peak = peak;
}

static int16_t clamp_s16(int32_t v) {
  if (v >  32767) return  32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

/* One callback chunk -> interleaved stereo PCM16. Returns bytes written.
 *
 * In the observed configuration this is a straight memcpy; the other branches
 * are here so a differently-configured stream is quiet-but-correct rather than
 * loud-and-wrong. */
static size_t convert_chunk(const void *src, int32_t frames, int32_t src_ch,
                            int32_t src_fmt, int16_t *dst) {
  if (src_ch < 1) src_ch = 2;

  if (src_fmt == AAUDIO_FORMAT_PCM_I16 && src_ch == 2) {
    memcpy(dst, src, (size_t)frames * 4);
    return (size_t)frames * 4;
  }

  for (int32_t f = 0; f < frames; f++) {
    int32_t l = 0, rr = 0;
    switch (src_fmt) {
      case AAUDIO_FORMAT_PCM_FLOAT: {
        const float *p = (const float *)src + (size_t)f * src_ch;
        l  = (int32_t)(p[0] * 32767.0f);
        rr = (int32_t)(p[src_ch > 1 ? 1 : 0] * 32767.0f);
        break;
      }
      case AAUDIO_FORMAT_PCM_I32: {
        const int32_t *p = (const int32_t *)src + (size_t)f * src_ch;
        l  = p[0] >> 16;
        rr = p[src_ch > 1 ? 1 : 0] >> 16;
        break;
      }
      case AAUDIO_FORMAT_PCM_I24_PACKED: {
        const unsigned char *p =
            (const unsigned char *)src + (size_t)f * src_ch * 3;
        l  = (int16_t)((p[1]) | (p[2] << 8));
        rr = (src_ch > 1) ? (int16_t)((p[4]) | (p[5] << 8)) : l;
        break;
      }
      default: {                                   /* PCM_I16, mono or >2 ch */
        const int16_t *p = (const int16_t *)src + (size_t)f * src_ch;
        l  = p[0];
        rr = p[src_ch > 1 ? 1 : 0];
        break;
      }
    }
    dst[f * 2 + 0] = clamp_s16(l);
    dst[f * 2 + 1] = clamp_s16(rr);
  }
  return (size_t)frames * 4;
}

typedef int32_t (*aaudio_data_cb)(void *stream, void *userData,
                                  void *audioData, int32_t numFrames);

/* Scratch the callback fills. 2048 frames of 8 bytes covers every format and
 * channel count this shim reports. */
static uint8_t g_scratch[2048 * 8];

/* Read by aaudio_shim_in_callback() from the watchdog thread. */
static volatile int      g_in_callback;
static volatile uint64_t g_callback_entered_ns;

static void *audio_thread_main(void *arg) {
  (void)arg;
  debug_log("[aaudio] callback thread running (idle until the stream starts)\n");

  int      reported    = 0;
  uint64_t calls       = 0;
  uint64_t frames_done = 0;
  uint64_t last_report = armTicksToNs(armGetSystemTick());

  while (g_audio_run) {
    /* Idle until the engine starts the stream. The thread exists from host
     * startup, so requestStart has nothing to allocate and nothing to wait
     * for. */
    if (g_stream.state != AAUDIO_STREAM_STATE_STARTED ||
        !g_stream.cfg.data_callback) {
      svcSleepThread(5000000LL);            /* 5 ms */
      continue;
    }

    /* Read each pass rather than once: the stream is configured after this
     * thread already exists, and could be reconfigured by a later open. */
    int32_t rate  = g_stream.cfg.sample_rate > 0 ? g_stream.cfg.sample_rate
                                                 : 48000;
    int32_t chunk = callback_frames(&g_stream.cfg);
    int32_t bpf   = bytes_per_frame(&g_stream.cfg);
    if (chunk * bpf > (int32_t)sizeof(g_scratch))
      chunk = (int32_t)sizeof(g_scratch) / bpf;

    aaudio_data_cb cb = (aaudio_data_cb)g_stream.cfg.data_callback;

    /* Warn once if the engine is not at the device's rate. audout is fixed at
       48 kHz, so anything else plays at the wrong pitch rather than failing. */
    if (g_aout_live) {
      static int rate_warned;
      if (!rate_warned && rate != (int32_t)audoutGetSampleRate()) {
        rate_warned = 1;
        debug_log("[aaudio] *** the stream is %d Hz but audio out is %u Hz. "
                  "Nothing here resamples, so playback will be pitched. ***\n",
                  (int)rate, (unsigned)audoutGetSampleRate());
      }
      if (!g_aout_fill) {
        if (g_aout_nfree == 0) audio_out_reclaim(500000000ULL);   /* 0.5 s */
        if (g_aout_nfree > 0) {
          g_aout_fill = g_aout_free[--g_aout_nfree];
          g_aout_used = 0;
        }
      }
    }

    /* Bracket the callback so a stall report can tell "the mixer is running"
     * from "the mixer went in and never came back". The first call took about
     * fifteen seconds to return in the last run and there was no way to see
     * that while it was happening. */
    uint64_t entered = armTicksToNs(armGetSystemTick());
    g_callback_entered_ns = entered;
    g_in_callback = 1;
    int32_t r = cb(&g_stream, g_stream.cfg.user_data, g_scratch, chunk);
    g_in_callback = 0;

    uint64_t took = armTicksToNs(armGetSystemTick()) - entered;
    if (took > 100000000ull)
      debug_log("[aaudio] the mixer callback took %llu ms -- it is blocking on "
                "something, not merely slow\n",
                (unsigned long long)(took / 1000000ull));

    if (!reported) {
      reported = 1;
      debug_log("[aaudio] first callback returned %d (%s)\n", (int)r,
                r == AAUDIO_CALLBACK_RESULT_CONTINUE ? "CONTINUE" : "STOP");
    }
    if (r == AAUDIO_CALLBACK_RESULT_STOP) {
      debug_log("[aaudio] the data callback returned STOP\n");
      g_stream.state = AAUDIO_STREAM_STATE_STOPPED;
      continue;
    }

    /* Heartbeat. Without it, "audio is running" and "audio stopped being
     * called ten seconds ago" look identical in the log. The count is against
     * elapsed time, so a figure well under the sample rate means the engine's
     * callback is taking longer than the audio it returns -- which is the
     * shape of a mixer that is struggling rather than one that is stuck. */
    calls++;
    frames_done += chunk;
    uint64_t nowt = armTicksToNs(armGetSystemTick());
    if (nowt - last_report > 10000000000ull) {
      uint64_t secs = (nowt - last_report) / 1000000000ull;
      debug_log("[aaudio] %llu callbacks, %llu frames in %llus "
                "(%llu frames/s; the stream asks for %d); peak level %d/32767 "
                "-- %s\n",
                (unsigned long long)calls, (unsigned long long)frames_done,
                (unsigned long long)secs,
                (unsigned long long)(secs ? frames_done / secs : 0), (int)rate,
                (int)g_peak,
                !g_aout_live ? "audio out is NOT open, so this is silent here"
                : g_peak == 0 ? "the ENGINE is producing digital silence; the "
                                "cause is upstream of the shim"
                              : "samples are non-silent and going to the device");
      g_peak = 0;
      last_report = nowt;
      calls = 0;
      frames_done = 0;
    }

    /* Where the samples actually go.
     *
     * Once audio out is live, the pacing comes from waiting for a buffer to be
     * released -- the device consuming at the sample rate, which is the thing
     * the sleep below was imitating. Sleeping as WELL would underrun, so the
     * two are alternatives, and the sleep remains the fallback for when audout
     * could not be opened or a buffer could not be acquired. */
    if (g_aout_live && g_aout_fill) {
      size_t need = (size_t)chunk * 4;              /* stereo PCM16 out */
      if (g_aout_used + need <= AOUT_BUF_BYTES) {
        int16_t *dst = (int16_t *)((unsigned char *)g_aout_fill->buffer +
                                   g_aout_used);
        convert_chunk(g_scratch, chunk, g_stream.cfg.channel_count,
                      g_stream.cfg.format, dst);
        note_peak(dst, (size_t)chunk);
        g_aout_used += need;
      }
      if (g_aout_used >= AOUT_SUBMIT_AT ||
          g_aout_used + need > AOUT_BUF_BYTES) {
        g_aout_fill->data_size = g_aout_used;
        if (R_FAILED(audoutAppendAudioOutBuffer(g_aout_fill))) {
          /* Put it back rather than leaking it out of the rotation. */
          if (g_aout_nfree < AOUT_BUFFERS) g_aout_free[g_aout_nfree++] = g_aout_fill;
          static int append_warned;
          if (!append_warned) {
            append_warned = 1;
            debug_log("[aaudio] audoutAppendAudioOutBuffer failed; audio will "
                      "gap\n");
          }
        }
        g_aout_fill = NULL;
        g_aout_used = 0;
      }
    } else {
      svcSleepThread((int64_t)chunk * 1000000000LL / rate);
    }
  }

  debug_log("[aaudio] callback thread exiting\n");
  return NULL;
}

/* Created ONCE from the host's own startup, deliberately not from inside a
 * P/Invoke.
 *
 * The previous version created it in requestStart, which the engine calls from
 * managed code on the frame loop's thread. That run stopped dead there:
 * "[aaudio] requestStart" is logged, and then neither pthread_create_fake's
 * own "[thr] created" line nor the failure line that follows it ever appears.
 * The thread was never created and the caller never came back -- main blocked
 * inside pthread_create_fake, which allocates a 512 KB stack through newlib
 * malloc and then calls threadCreate, all while the engine is part-way through
 * starting its audio device.
 *
 * Creating the thread up front removes every one of those steps from the
 * engine's call stack: requestStart now sets a field and returns. It also
 * fixes the other candidate for the same hang -- an engine waiting for its
 * first callback before returning -- because the thread is already alive and
 * picks the stream up within 5 ms. */
void aaudio_shim_init(void) {
  if (g_audio_thread) return;

  /* Opened here, from host startup, for the same reason the thread is: doing
     service initialisation inside a P/Invoke is what hung this shim once
     already. By the time the engine calls requestStart, everything exists. */
  audio_out_open();

  g_audio_run = 1;
  if (pthread_create_fake(&g_audio_thread, NULL, audio_thread_main, NULL) != 0) {
    debug_log("[aaudio] could not create the callback thread -- audio will "
              "not advance. The game should still run.\n");
    g_audio_thread = NULL;
    g_audio_run = 0;
    return;
  }
  debug_log("[aaudio] callback thread created up front; requestStart will "
            "not have to allocate\n");
}

/* Retained so main.c keeps compiling and so there is one obvious place to hook
 * per-frame audio work if it is ever needed again. Deliberately empty: the
 * callback must not run on the frame loop's thread. */
void aaudio_shim_pump(void) { }

/* For the stall report: is the mixer callback outstanding, and for how long. */
int aaudio_shim_in_callback(unsigned long long *ms) {
  if (!g_in_callback) return 0;
  if (ms) {
    uint64_t now = armTicksToNs(armGetSystemTick());
    *ms = (unsigned long long)((now - g_callback_entered_ns) / 1000000ull);
  }
  return 1;
}

/* Getters echo the builder, which is what a real stream would report after a
 * successful open. Zero would be a lie that could produce a divide. */
static int32_t nx_AAudioStream_getSampleRate(void *s) {
  (void)s; return g_stream.cfg.sample_rate ? g_stream.cfg.sample_rate : 48000;
}
static int32_t nx_AAudioStream_getChannelCount(void *s) {
  (void)s; return g_stream.cfg.channel_count ? g_stream.cfg.channel_count : 2;
}
static int32_t nx_AAudioStream_getFormat(void *s) {
  (void)s; return g_stream.cfg.format;
}
static int32_t nx_AAudioStream_getFramesPerBurst(void *s) {
  (void)s; return callback_frames(&g_stream.cfg);                     /* 4 ms at 48 kHz */
}
static int32_t nx_AAudioStream_getBufferCapacityInFrames(void *s) {
  (void)s; return 1920;                    /* 40 ms at 48 kHz */
}
static int32_t nx_AAudioStream_getPerformanceMode(void *s) {
  (void)s; return g_stream.cfg.performance_mode;
}
static int32_t nx_AAudioStream_getSharingMode(void *s) {
  (void)s; return g_stream.cfg.sharing_mode;
}
static int32_t nx_AAudioStream_getUsage(void *s) {
  (void)s; return g_stream.cfg.usage;
}
static int32_t nx_AAudioStream_getContentType(void *s) {
  (void)s; return g_stream.cfg.content_type;
}
static int32_t nx_AAudioStream_getAllowedCapturePolicy(void *s) {
  (void)s; return g_stream.cfg.capture_policy;
}

/* ------------------------------------------------------------------------ */

typedef struct { const char *name; void *fn; } AAudioSym;

static const AAudioSym g_aaudio_syms[] = {
  { "AAudio_createStreamBuilder",                (void *)nx_AAudio_createStreamBuilder },
  { "AAudioStreamBuilder_setSampleRate",         (void *)nx_AAudioStreamBuilder_setSampleRate },
  { "AAudioStreamBuilder_setChannelCount",       (void *)nx_AAudioStreamBuilder_setChannelCount },
  { "AAudioStreamBuilder_setFormat",             (void *)nx_AAudioStreamBuilder_setFormat },
  { "AAudioStreamBuilder_setDirection",          (void *)nx_AAudioStreamBuilder_setDirection },
  { "AAudioStreamBuilder_setSharingMode",        (void *)nx_AAudioStreamBuilder_setSharingMode },
  { "AAudioStreamBuilder_setPerformanceMode",    (void *)nx_AAudioStreamBuilder_setPerformanceMode },
  { "AAudioStreamBuilder_setUsage",              (void *)nx_AAudioStreamBuilder_setUsage },
  { "AAudioStreamBuilder_setContentType",        (void *)nx_AAudioStreamBuilder_setContentType },
  { "AAudioStreamBuilder_setAllowedCapturePolicy",(void *)nx_AAudioStreamBuilder_setAllowedCapturePolicy },
  { "AAudioStreamBuilder_setFramesPerDataCallback",(void *)nx_AAudioStreamBuilder_setFramesPerDataCallback },
  { "AAudioStreamBuilder_setDataCallback",       (void *)nx_AAudioStreamBuilder_setDataCallback },
  { "AAudioStreamBuilder_setErrorCallback",      (void *)nx_AAudioStreamBuilder_setErrorCallback },
  { "AAudioStreamBuilder_openStream",            (void *)nx_AAudioStreamBuilder_openStream },
  { "AAudioStreamBuilder_delete",                (void *)nx_AAudioStreamBuilder_delete },
  { "AAudioStream_requestStart",                 (void *)nx_AAudioStream_requestStart },
  { "AAudioStream_requestPause",                 (void *)nx_AAudioStream_requestPause },
  { "AAudioStream_requestStop",                  (void *)nx_AAudioStream_requestStop },
  { "AAudioStream_close",                        (void *)nx_AAudioStream_close },
  { "AAudioStream_write",                        (void *)nx_AAudioStream_write },
  { "AAudioStream_getState",                     (void *)nx_AAudioStream_getState },
  { "AAudioStream_setBufferSizeInFrames",        (void *)nx_AAudioStream_setBufferSizeInFrames },
  { "AAudioStream_getSampleRate",                (void *)nx_AAudioStream_getSampleRate },
  { "AAudioStream_getChannelCount",              (void *)nx_AAudioStream_getChannelCount },
  { "AAudioStream_getFormat",                    (void *)nx_AAudioStream_getFormat },
  { "AAudioStream_getFramesPerBurst",            (void *)nx_AAudioStream_getFramesPerBurst },
  { "AAudioStream_getBufferCapacityInFrames",    (void *)nx_AAudioStream_getBufferCapacityInFrames },
  { "AAudioStream_getPerformanceMode",           (void *)nx_AAudioStream_getPerformanceMode },
  { "AAudioStream_getSharingMode",               (void *)nx_AAudioStream_getSharingMode },
  { "AAudioStream_getUsage",                     (void *)nx_AAudioStream_getUsage },
  { "AAudioStream_getContentType",               (void *)nx_AAudioStream_getContentType },
  { "AAudioStream_getAllowedCapturePolicy",      (void *)nx_AAudioStream_getAllowedCapturePolicy },
};

void *aaudio_shim_lookup(const char *symbol) {
  if (!symbol) return NULL;
  for (size_t i = 0; i < sizeof(g_aaudio_syms)/sizeof(g_aaudio_syms[0]); i++)
    if (!strcmp(symbol, g_aaudio_syms[i].name)) return g_aaudio_syms[i].fn;

  /* A name we do not serve would bind to NULL and be called, so say which. */
  debug_log("[aaudio] no such symbol: %s -- add it to aaudio_shim.c\n", symbol);
  return NULL;
}
