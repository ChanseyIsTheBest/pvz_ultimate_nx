/* opensles.c -- OpenSL ES 1.0.1 shim backed by libnx audout.
 *
 * Structure adapted from the angrybirdsjourney_nx port by Andy Nguyen and
 * fgsfds (MIT licensed), which implements the same slice of the API over SDL2.
 * The interface layer here follows that design closely because it is the right
 * shape; the device layer is rewritten, since this port has no SDL2 and talks
 * to audout directly.
 *
 * What the API demands and what we provide:
 *
 *   Object    Realize / GetInterface / Destroy
 *   Engine    CreateOutputMix / CreateAudioPlayer
 *   Play      SetPlayState / GetPlayState / RegisterCallback
 *   BufferQueue (and AndroidSimpleBufferQueue) Enqueue / Clear / GetState
 *   Volume    SetVolumeLevel / SetMute
 *   PlaybackRate  SetRate
 *
 * Every player is software-mixed into one audout stream. The buffer-queue
 * completion callback fires from the mixing thread, which is where Android
 * fires it too -- a game that enqueues its next buffer from inside that
 * callback therefore behaves the same way here.
 *
 * audout is fixed at 48 kHz stereo signed 16-bit, so players at other rates
 * are resampled per-player during mixing rather than by reconfiguring the
 * device.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "opensles.h"
#include "util.h"

/* ---- OpenSL ES constants -------------------------------------------------- */

#define SL_RESULT_SUCCESS             0
#define SL_RESULT_PARAMETER_INVALID   0x0D
#define SL_RESULT_FEATURE_UNSUPPORTED 0x0C
#define SL_RESULT_MEMORY_FAILURE      0x04

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

/* Device format. audout does not negotiate. */
#define DEV_RATE     48000
#define DEV_CHANNELS 2
#define DEV_FRAMES   960          /* 20 ms, the usual audout granularity */
#define DEV_BUFFERS  3            /* enough to hide a late mix without lag */

#define MAX_PLAYERS 32
#define BQ_SLOTS    16

typedef uint32_t SLuint32;
typedef int32_t  SLresult;
typedef void    *SLObjectItf_ptr;

/* ---- interface identifiers ------------------------------------------------ */

/* The values are opaque to callers: they compare pointers, never contents. */
static const char IID_ENGINE[]        = "engine";
static const char IID_PLAY[]          = "play";
static const char IID_BUFFERQUEUE[]   = "bufferqueue";
static const char IID_VOLUME[]        = "volume";
static const char IID_PLAYBACKRATE[]  = "playbackrate";
static const char IID_ANDROIDCONFIG[] = "androidconfig";
static const char IID_ANDROIDSBQ[]    = "androidsimplebufferqueue";

const void *SL_IID_ENGINE                   = IID_ENGINE;
const void *SL_IID_PLAY                     = IID_PLAY;
const void *SL_IID_BUFFERQUEUE              = IID_BUFFERQUEUE;
const void *SL_IID_VOLUME                   = IID_VOLUME;
const void *SL_IID_PLAYBACKRATE             = IID_PLAYBACKRATE;
const void *SL_IID_ANDROIDCONFIGURATION     = IID_ANDROIDCONFIG;
const void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE = IID_ANDROIDSBQ;

/* ---- objects -------------------------------------------------------------- */

typedef void (*bq_callback)(void *bq, void *ctx);

typedef struct { const uint8_t *data; SLuint32 size; } BQBuffer;

typedef struct Player {
  const struct SLObjectItf_       *obj_vt;
  const struct SLPlayItf_         *play_vt;
  const struct SLBufferQueueItf_  *bq_vt;
  const struct SLVolumeItf_       *vol_vt;
  const struct SLPlaybackRateItf_ *rate_vt;
  const struct SLAndroidConfigurationItf_ *config_vt;

  int in_use;
  int channels;
  int rate;
  int sbytes;        /* bytes per sample in enqueued data: 2 = s16, 4 = float */
  int is_float;
  int playing;
  float gain;        /* linear, converted from millibels                     */
  float rate_scale;  /* from SetRate, 1.0 = normal                           */

  bq_callback cb;
  void *cb_ctx;

  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail;

  const uint8_t *cur;
  SLuint32 cur_size, cur_pos;
  double   cur_fpos;   /* fractional read position, for resampling           */

  Mutex lock;
} Player;

typedef struct { const struct SLObjectItf_ *obj_vt; } OutputMix;
typedef struct { const struct SLObjectItf_ *obj_vt;
                 const struct SLEngineItf_ *eng_vt; } Engine;

static Player   *g_players[MAX_PLAYERS];
static Mutex     g_players_lock;
static bool      g_audio_up;
static Thread    g_mix_thread;
static bool      g_mix_stop;

/* ---- mixing --------------------------------------------------------------- */

/* Millibels to a linear gain. -100 mB is one decibel down; the floor matches
 * Android's SL_MILLIBEL_MIN, below which a source is simply silent. */
static float mb_to_linear(int32_t mb) {
  if (mb <= -9600) return 0.0f;
  if (mb >= 0)     return 1.0f;
  return powf(10.0f, (float)mb / 2000.0f);
}

/* Pull one sample from a player's current buffer, converting format and
 * channel count on the way. Returns 0 when the buffer is exhausted. */
static int player_sample(Player *p, int32_t *l, int32_t *r) {
  if (!p->cur || p->cur_pos >= p->cur_size) return 0;

  const uint8_t *base = p->cur + p->cur_pos;
  int32_t a = 0, b = 0;

  if (p->is_float) {
    const float *f = (const float *)base;
    a = (int32_t)(f[0] * 32767.0f);
    b = (p->channels > 1) ? (int32_t)(f[1] * 32767.0f) : a;
  } else {
    const int16_t *s = (const int16_t *)base;
    a = s[0];
    b = (p->channels > 1) ? s[1] : a;
  }
  *l = a; *r = b;
  return 1;
}

static void player_advance(Player *p) {
  /* Advance by the resampling ratio: a 22 kHz source consumed by a 48 kHz
   * device moves through its own data at less than one frame per output
   * frame. Fractional position is kept so the error does not accumulate. */
  double step = ((double)p->rate / (double)DEV_RATE) * (double)p->rate_scale;
  p->cur_fpos += step;

  SLuint32 frame = (SLuint32)(p->sbytes * p->channels);
  while (p->cur_fpos >= 1.0) {
    p->cur_fpos -= 1.0;
    p->cur_pos  += frame;
  }
}

/* Take the next queued buffer, firing the completion callback for the one just
 * finished. The callback runs here, on the mixing thread, exactly as Android
 * runs it on its fast-track thread. */
static void player_next_buffer(Player *p) {
  if (p->q_head == p->q_tail) { p->cur = NULL; return; }

  p->cur      = p->q[p->q_head].data;
  p->cur_size = p->q[p->q_head].size;
  p->cur_pos  = 0;
  p->cur_fpos = 0.0;
  p->q_head   = (p->q_head + 1) % BQ_SLOTS;
}

static void mix_player(Player *p, int32_t *acc, int frames) {
  mutexLock(&p->lock);
  if (!p->playing) { mutexUnlock(&p->lock); goto done; }

  for (int i = 0; i < frames; i++) {
    if (!p->cur || p->cur_pos >= p->cur_size) {
      const uint8_t *finished = p->cur;
      player_next_buffer(p);

      if (finished && p->cb) {
        /* Released while the callback runs: games commonly enqueue the next
         * buffer from inside it, and holding our own lock across that call
         * would deadlock against Enqueue. */
        bq_callback cb = p->cb;
        void *ctx = p->cb_ctx;
        mutexUnlock(&p->lock);
        cb(&p->bq_vt, ctx);
        mutexLock(&p->lock);
      }
      if (!p->cur) break;
    }

    int32_t l, r;
    if (!player_sample(p, &l, &r)) break;
    acc[i * 2 + 0] += (int32_t)(l * p->gain);
    acc[i * 2 + 1] += (int32_t)(r * p->gain);
    player_advance(p);
  }
  mutexUnlock(&p->lock);
done:
  return;
}

static void mix_thread(void *arg) {
  (void)arg;

  /* audout requires page-aligned buffers whose size is a multiple of the
   * page size, so this is aligned explicitly rather than relying on a libnx
   * macro that may not be in scope here. */
  static __attribute__((aligned(0x1000)))
      int16_t bufs[DEV_BUFFERS][DEV_FRAMES * DEV_CHANNELS];
  static AudioOutBuffer aob[DEV_BUFFERS];
  int32_t acc[DEV_FRAMES * DEV_CHANNELS];

  for (int i = 0; i < DEV_BUFFERS; i++) {
    memset(bufs[i], 0, sizeof(bufs[i]));
    aob[i].next        = NULL;
    aob[i].buffer      = bufs[i];
    aob[i].buffer_size = sizeof(bufs[i]);
    aob[i].data_size   = sizeof(bufs[i]);
    aob[i].data_offset = 0;
    audoutAppendAudioOutBuffer(&aob[i]);
  }

  while (!g_mix_stop) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ull))) continue;
    if (!released) continue;

    memset(acc, 0, sizeof(acc));

    mutexLock(&g_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (g_players[i] && g_players[i]->in_use)
        mix_player(g_players[i], acc, DEV_FRAMES);
    mutexUnlock(&g_players_lock);

    /* Clip rather than wrap. Summing several sources overflows int16 easily,
     * and wrapping turns a loud moment into a burst of noise. */
    int16_t *out = (int16_t *)released->buffer;
    for (int i = 0; i < DEV_FRAMES * DEV_CHANNELS; i++) {
      int32_t v = acc[i];
      if (v >  32767) v =  32767;
      if (v < -32768) v = -32768;
      out[i] = (int16_t)v;
    }

    released->data_size = DEV_FRAMES * DEV_CHANNELS * sizeof(int16_t);
    audoutAppendAudioOutBuffer(released);
  }
}

/* ---- lifecycle ------------------------------------------------------------ */

int opensles_init(void) {
  if (g_audio_up) return 0;
  mutexInit(&g_players_lock);

  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    debug_log("[sl] audoutInitialize failed: %08x -- continuing without audio\n", rc);
    return -1;
  }
  audoutStartAudioOut();

  g_mix_stop = false;
  rc = threadCreate(&g_mix_thread, mix_thread, NULL, NULL, 0x4000, 0x2B, -2);
  if (R_FAILED(rc)) {
    debug_log("[sl] could not start the mixing thread: %08x\n", rc);
    audoutExit();
    return -1;
  }
  threadStart(&g_mix_thread);

  g_audio_up = true;
  debug_log("[sl] audio up: %d Hz, %d ch, %d-frame buffers\n",
            DEV_RATE, DEV_CHANNELS, DEV_FRAMES);
  return 0;
}

void opensles_shutdown(void) {
  if (!g_audio_up) return;
  g_mix_stop = true;
  threadWaitForExit(&g_mix_thread);
  threadClose(&g_mix_thread);
  audoutStopAudioOut();
  audoutExit();
  g_audio_up = false;
}

/* ---- interface vtables ---------------------------------------------------- */

/* OpenSL ES is COM-shaped: every handle is a pointer to a pointer to a table
 * of function pointers, and the object itself is recovered by subtracting the
 * offset of that table within its struct. Each interface a Player exposes is a
 * separate member, so the container_of has to name the right one. */
#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

struct SLObjectItf_ {
  SLresult (*Realize)(void *self, uint32_t async);
  SLresult (*Resume)(void *self, uint32_t async);
  SLresult (*GetState)(void *self, uint32_t *state);
  SLresult (*GetInterface)(void *self, const void *iid, void *out);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  void     (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, int32_t prio, uint32_t preempt);
  SLresult (*GetPriority)(void *self, int32_t *prio, uint32_t *preempt);
  SLresult (*SetLossOfControlInterfaces)(void *self, int16_t n, void *iids, uint32_t enabled);
};

struct SLPlayItf_ {
  SLresult (*SetPlayState)(void *self, uint32_t state);
  SLresult (*GetPlayState)(void *self, uint32_t *state);
  SLresult (*GetDuration)(void *self, uint32_t *msec);
  SLresult (*GetPosition)(void *self, uint32_t *msec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, uint32_t events);
  SLresult (*GetCallbackEventsMask)(void *self, uint32_t *events);
  SLresult (*SetMarkerPosition)(void *self, uint32_t msec);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, uint32_t *msec);
  SLresult (*SetPositionUpdatePeriod)(void *self, uint32_t msec);
  SLresult (*GetPositionUpdatePeriod)(void *self, uint32_t *msec);
};

struct SLBufferQueueItf_ {
  SLresult (*Enqueue)(void *self, const void *buf, uint32_t size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *state);
  SLresult (*RegisterCallback)(void *self, bq_callback cb, void *ctx);
};

struct SLVolumeItf_ {
  SLresult (*SetVolumeLevel)(void *self, int32_t mb);
  SLresult (*GetVolumeLevel)(void *self, int32_t *mb);
  SLresult (*GetMaxVolumeLevel)(void *self, int32_t *mb);
  SLresult (*SetMute)(void *self, uint32_t mute);
  SLresult (*GetMute)(void *self, uint32_t *mute);
  SLresult (*EnableStereoPosition)(void *self, uint32_t enable);
  SLresult (*IsEnabledStereoPosition)(void *self, uint32_t *enable);
  SLresult (*SetStereoPosition)(void *self, int16_t pos);
  SLresult (*GetStereoPosition)(void *self, int16_t *pos);
};

struct SLPlaybackRateItf_ {
  SLresult (*SetRate)(void *self, int16_t permille);
  SLresult (*GetRate)(void *self, int16_t *permille);
  SLresult (*SetPropertyConstraints)(void *self, uint32_t c);
  SLresult (*GetProperties)(void *self, uint32_t *c);
  SLresult (*GetCapabilitiesOfRate)(void *self, int16_t r, uint32_t *c);
  SLresult (*GetRateRange)(void *self, uint8_t i, int16_t *min, int16_t *max,
                           int16_t *step, uint32_t *caps);
};

struct SLAndroidConfigurationItf_ {
  SLresult (*SetConfiguration)(void *self, const char *key, const void *v, uint32_t sz);
  SLresult (*GetConfiguration)(void *self, const char *key, uint32_t *sz, void *v);
};

struct SLEngineItf_ {
  SLresult (*CreateLEDDevice)(void *self, void **o, uint32_t id, uint32_t n, const void *ids, const uint32_t *req);
  SLresult (*CreateVibraDevice)(void *self, void **o, uint32_t id, uint32_t n, const void *ids, const uint32_t *req);
  SLresult (*CreateAudioPlayer)(void *self, void **player, void *src, void *snk,
                                uint32_t n, const void *ids, const uint32_t *req);
  SLresult (*CreateAudioRecorder)(void *self, void **o, void *src, void *snk, uint32_t n, const void *ids, const uint32_t *req);
  SLresult (*CreateMidiPlayer)(void *self, void **o, void *a, void *b, void *c, void *d, void *e, uint32_t n, const void *ids, const uint32_t *req);
  SLresult (*CreateListener)(void *self, void **o, uint32_t n, const void *ids, const uint32_t *req);
  SLresult (*Create3DGroup)(void *self, void **o, uint32_t n, const void *ids, const uint32_t *req);
  SLresult (*CreateOutputMix)(void *self, void **mix, uint32_t n, const void *ids, const uint32_t *req);
};

/* --- data locators, as the caller lays them out --- */
typedef struct { uint32_t locatorType; uint32_t numBuffers; } DataLocator_BQ;
typedef struct {
  uint32_t formatType, numChannels, samplesPerSec, bitsPerSample;
  uint32_t containerSize, channelMask, endianness;
} DataFormat_PCM;
typedef struct { void *pLocator; void *pFormat; } DataSource;

#define SL_DATAFORMAT_PCM 0x00000002
/* Android's float extension; the sample path branches on it. */
#define SL_ANDROID_DATAFORMAT_PCM_EX 0x00000004

/* --- Play --- */
static SLresult play_SetPlayState(void *self, uint32_t state) {
  Player *p = CONTAINER(self, Player, play_vt);
  mutexLock(&p->lock);
  p->playing = (state == SL_PLAYSTATE_PLAYING);
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, uint32_t *state) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (state) *state = p->playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_u32(void *self, uint32_t v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_pu32(void *self, uint32_t *v) { (void)self; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult play_reg(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static const struct SLPlayItf_ g_play_vt = {
  play_SetPlayState, play_GetPlayState, play_pu32, play_pu32, play_reg,
  play_u32, play_pu32, play_u32, play_ok, play_pu32, play_u32, play_pu32,
};

/* --- BufferQueue --- */
static SLresult bq_Enqueue(void *self, const void *buf, uint32_t size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  mutexLock(&p->lock);
  int next = (p->q_tail + 1) % BQ_SLOTS;
  if (next == p->q_head) { mutexUnlock(&p->lock); return SL_RESULT_MEMORY_FAILURE; }
  p->q[p->q_tail].data = (const uint8_t *)buf;
  p->q[p->q_tail].size = size;
  p->q_tail = next;
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  mutexLock(&p->lock);
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(void *self, void *state) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (state) {
    uint32_t *s = (uint32_t *)state;
    s[0] = (uint32_t)((p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS);
    s[1] = 0;
  }
  return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(void *self, bq_callback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  mutexLock(&p->lock);
  p->cb = cb; p->cb_ctx = ctx;
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static const struct SLBufferQueueItf_ g_bq_vt = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

/* --- Volume --- */
static SLresult vol_SetVolumeLevel(void *self, int32_t mb) {
  Player *p = CONTAINER(self, Player, vol_vt);
  p->gain = mb_to_linear(mb);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_SetMute(void *self, uint32_t mute) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (mute) p->gain = 0.0f;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_i32(void *self, int32_t *v) { (void)self; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_u32(void *self, uint32_t *v) { (void)self; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setu32(void *self, uint32_t v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_seti16(void *self, int16_t v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_geti16(void *self, int16_t *v) { (void)self; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static const struct SLVolumeItf_ g_vol_vt = {
  vol_SetVolumeLevel, vol_i32, vol_i32, vol_SetMute, vol_u32,
  vol_setu32, vol_u32, vol_seti16, vol_geti16,
};

/* --- PlaybackRate --- */
static SLresult rate_SetRate(void *self, int16_t permille) {
  Player *p = CONTAINER(self, Player, rate_vt);
  p->rate_scale = (permille > 0) ? (float)permille / 1000.0f : 1.0f;
  return SL_RESULT_SUCCESS;
}
static SLresult rate_GetRate(void *self, int16_t *v) {
  Player *p = CONTAINER(self, Player, rate_vt);
  if (v) *v = (int16_t)(p->rate_scale * 1000.0f);
  return SL_RESULT_SUCCESS;
}
static SLresult rate_u32(void *self, uint32_t c) { (void)self; (void)c; return SL_RESULT_SUCCESS; }
static SLresult rate_pu32(void *self, uint32_t *c) { (void)self; if (c) *c = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_caps(void *self, int16_t r, uint32_t *c) { (void)self; (void)r; if (c) *c = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_range(void *self, uint8_t i, int16_t *mn, int16_t *mx, int16_t *st, uint32_t *c) {
  (void)self; (void)i;
  if (mn) *mn = 500;
  if (mx) *mx = 2000;
  if (st) *st = 1;
  if (c)  *c  = 0;
  return SL_RESULT_SUCCESS;
}
static const struct SLPlaybackRateItf_ g_rate_vt = {
  rate_SetRate, rate_GetRate, rate_u32, rate_pu32, rate_caps, rate_range,
};

/* --- AndroidConfiguration: accepted and ignored, which is all it needs --- */
static SLresult cfg_Set(void *self, const char *k, const void *v, uint32_t sz) {
  (void)self; (void)v; (void)sz;
  debug_log("[sl] AndroidConfiguration '%s' accepted and ignored\n", k ? k : "?");
  return SL_RESULT_SUCCESS;
}
static SLresult cfg_Get(void *self, const char *k, uint32_t *sz, void *v) {
  (void)self; (void)k; (void)v; if (sz) *sz = 0;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static const struct SLAndroidConfigurationItf_ g_cfg_vt = { cfg_Set, cfg_Get };

/* --- Object, shared by players, mixes and the engine --- */
static SLresult obj_Realize(void *self, uint32_t async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, uint32_t async)  { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, uint32_t *st)  { (void)self; if (st) *st = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static void     obj_Abort(void *self) { (void)self; }
static SLresult obj_SetPriority(void *self, int32_t p, uint32_t pe) { (void)self; (void)p; (void)pe; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, int32_t *p, uint32_t *pe) { (void)self; if (p) *p = 0; if (pe) *pe = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLoss(void *self, int16_t n, void *ids, uint32_t en) { (void)self; (void)n; (void)ids; (void)en; return SL_RESULT_SUCCESS; }

static SLresult player_GetInterface(void *self, const void *iid, void *out);
static void     player_Destroy(void *self);
static SLresult mix_GetInterface(void *self, const void *iid, void *out);
static void     generic_Destroy(void *self);
static SLresult engine_GetInterface(void *self, const void *iid, void *out);

static const struct SLObjectItf_ g_player_obj_vt = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface,
  obj_RegisterCallback, obj_Abort, player_Destroy,
  obj_SetPriority, obj_GetPriority, obj_SetLoss,
};
static const struct SLObjectItf_ g_mix_obj_vt = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface,
  obj_RegisterCallback, obj_Abort, generic_Destroy,
  obj_SetPriority, obj_GetPriority, obj_SetLoss,
};
static const struct SLObjectItf_ g_engine_obj_vt = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface,
  obj_RegisterCallback, obj_Abort, generic_Destroy,
  obj_SetPriority, obj_GetPriority, obj_SetLoss,
};

static SLresult player_GetInterface(void *self, const void *iid, void *out) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!out) return SL_RESULT_PARAMETER_INVALID;

  if (iid == SL_IID_PLAY)          { *(const void **)out = &p->play_vt;   return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_BUFFERQUEUE ||
      iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE)
                                   { *(const void **)out = &p->bq_vt;     return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_VOLUME)        { *(const void **)out = &p->vol_vt;    return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_PLAYBACKRATE)  { *(const void **)out = &p->rate_vt;   return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_ANDROIDCONFIGURATION)
                                   { *(const void **)out = &p->config_vt; return SL_RESULT_SUCCESS; }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  mutexLock(&g_players_lock);
  for (int i = 0; i < MAX_PLAYERS; i++) if (g_players[i] == p) g_players[i] = NULL;
  mutexUnlock(&g_players_lock);
  free(p);
}

static void generic_Destroy(void *self) {
  /* The engine and output mix are singletons the caller may destroy at
   * shutdown; freeing the container is enough. */
  free((char *)self - offsetof(OutputMix, obj_vt));
}

static SLresult mix_GetInterface(void *self, const void *iid, void *out) {
  (void)self; (void)iid; (void)out;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

/* --- Engine --- */
static SLresult eng_CreateOutputMix(void *self, void **mix, uint32_t n,
                                    const void *ids, const uint32_t *req) {
  (void)self; (void)n; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(OutputMix));
  if (!m) return SL_RESULT_MEMORY_FAILURE;
  m->obj_vt = &g_mix_obj_vt;
  *mix = &m->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateAudioPlayer(void *self, void **out, void *src, void *snk,
                                      uint32_t n, const void *ids, const uint32_t *req) {
  (void)self; (void)snk; (void)n; (void)ids; (void)req;
  if (!out) return SL_RESULT_PARAMETER_INVALID;

  Player *p = calloc(1, sizeof(Player));
  if (!p) return SL_RESULT_MEMORY_FAILURE;

  p->obj_vt   = &g_player_obj_vt;
  p->play_vt  = &g_play_vt;
  p->bq_vt    = &g_bq_vt;
  p->vol_vt   = &g_vol_vt;
  p->rate_vt  = &g_rate_vt;
  p->config_vt= &g_cfg_vt;
  p->channels = 2;
  p->rate     = DEV_RATE;
  p->sbytes   = 2;
  p->gain     = 1.0f;
  p->rate_scale = 1.0f;
  mutexInit(&p->lock);

  /* Read the source format if one was supplied. A caller that omits it gets
   * the device defaults, which is better than refusing to create the player. */
  DataSource *ds = (DataSource *)src;
  if (ds && ds->pFormat) {
    DataFormat_PCM *f = (DataFormat_PCM *)ds->pFormat;
    if (f->formatType == SL_DATAFORMAT_PCM ||
        f->formatType == SL_ANDROID_DATAFORMAT_PCM_EX) {
      p->channels = (f->numChannels >= 2) ? 2 : 1;
      /* samplesPerSec is in milliHertz in this structure. */
      p->rate     = (int)(f->samplesPerSec / 1000);
      if (p->rate <= 0) p->rate = DEV_RATE;
      p->is_float = (f->formatType == SL_ANDROID_DATAFORMAT_PCM_EX &&
                     f->bitsPerSample == 32);
      p->sbytes   = p->is_float ? 4 : (int)(f->bitsPerSample / 8);
      if (p->sbytes != 2 && p->sbytes != 4) p->sbytes = 2;
    }
  }

  mutexLock(&g_players_lock);
  int slot = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) if (!g_players[i]) { slot = i; break; }
  if (slot < 0) { mutexUnlock(&g_players_lock); free(p); return SL_RESULT_MEMORY_FAILURE; }
  p->in_use = 1;
  g_players[slot] = p;
  mutexUnlock(&g_players_lock);

  debug_log("[sl] player %d: %d Hz, %d ch, %d-bit%s\n",
            slot, p->rate, p->channels, p->sbytes * 8, p->is_float ? " float" : "");
  *out = &p->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported() { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const struct SLEngineItf_ g_engine_vt = {
  (SLresult (*)(void *, void **, uint32_t, uint32_t, const void *, const uint32_t *))eng_unsupported,
  (SLresult (*)(void *, void **, uint32_t, uint32_t, const void *, const uint32_t *))eng_unsupported,
  eng_CreateAudioPlayer,
  (SLresult (*)(void *, void **, void *, void *, uint32_t, const void *, const uint32_t *))eng_unsupported,
  (SLresult (*)(void *, void **, void *, void *, void *, void *, void *, uint32_t, const void *, const uint32_t *))eng_unsupported,
  (SLresult (*)(void *, void **, uint32_t, const void *, const uint32_t *))eng_unsupported,
  (SLresult (*)(void *, void **, uint32_t, const void *, const uint32_t *))eng_unsupported,
  eng_CreateOutputMix,
};

static SLresult engine_GetInterface(void *self, const void *iid, void *out) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!out) return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) { *(const void **)out = &e->eng_vt; return SL_RESULT_SUCCESS; }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

/* ---- entry point ---------------------------------------------------------- */

SLresult slCreateEngine(void **engine, uint32_t numOptions, const void *options,
                        uint32_t numInterfaces, const void *ids, const uint32_t *req) {
  (void)numOptions; (void)options; (void)numInterfaces; (void)ids; (void)req;
  if (!engine) return SL_RESULT_PARAMETER_INVALID;

  if (opensles_init() != 0) {
    /* No device: refuse rather than hand back an engine whose players are
     * silently discarded. A caller that checks the result can fall back. */
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }

  Engine *e = calloc(1, sizeof(Engine));
  if (!e) return SL_RESULT_MEMORY_FAILURE;
  e->obj_vt = &g_engine_obj_vt;
  e->eng_vt = &g_engine_vt;
  *engine = &e->obj_vt;
  debug_log("[sl] engine created\n");
  return SL_RESULT_SUCCESS;
}
