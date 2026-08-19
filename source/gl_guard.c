/* gl_guard.c -- texture storage tracking and the FBO attach guard.
 *
 * WHY THIS FILE EXISTS
 *
 * The crash at the end of the previous session:
 *
 *     PC == LR == host+0x171350   st_update_renderbuffer_surface + 0x2a0
 *     ldrh w7, [x20, #0xe]        with x20 = 0
 *     FAULT ADDRESS 0x0e
 *
 * x20 is loaded in the prologue by `ldr x20, [x1, #0x58]`, which is
 * strb->texture -- a struct pipe_resource *. Offset 0xe within it is the
 * `format` field, confirmed by the sibling loads in the same function:
 * [x20,#4] is width0, [x20,#0x10] is target (compared against 6, which is
 * PIPE_TEXTURE_1D_ARRAY) and [x20,#0x11] is last_level.
 *
 * The call chain above it is unambiguous:
 *
 *     _mesa_FramebufferTexture2D
 *       _mesa_framebuffer_texture
 *         _mesa_update_texture_renderbuffer
 *           st_render_texture
 *             st_update_renderbuffer_surface   <- faults
 *
 * st_render_texture takes the pipe_resource off the texture object and stores
 * it in strb->texture. If the texture object has no resource -- no successful
 * glTexImage2D, or one that failed -- that store puts NULL there, and mesa's
 * assert(pt) is compiled out in a release build. So a texture with no storage,
 * attached to an FBO, is a null dereference inside the driver.
 *
 * Per the GLES spec this is supposed to be legal and merely produce
 * GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT from glCheckFramebufferStatus. It is a
 * robustness gap in mesa, not something the game is doing illegally, so the
 * fix belongs here rather than in the game.
 *
 * WHAT IT DOES
 *
 * Tracks, per texture name and level, whether storage was successfully
 * established. Then glFramebufferTexture2D refuses to attach a texture that
 * has none, and detaches that attachment point instead -- which produces
 * exactly the incomplete-attachment state the spec calls for, with no fault.
 *
 * IMPORTANT: dimensions are recorded only after the upload is confirmed to
 * have raised no error. An upload that fails with GL_OUT_OF_MEMORY leaves no
 * storage, and recording the requested size would wave the texture straight
 * through into the crash this file exists to stop.
 *
 * ASSUMPTION, stated because it is the one that could bite: binding state is
 * tracked globally, not per context or per thread. The last log shows every
 * eglMakeCurrent on 'main' with the same context, so GL here is
 * single-threaded. If that ever stops being true, the bindings recorded below
 * are wrong and the guard may block a texture that is in fact fine. That
 * failure mode is visible -- it prints -- and is still better than the fault.
 */

#include <stddef.h>
#include <string.h>

#include <switch.h>
#include <EGL/egl.h>

#include "gl_guard.h"
#include "mem_arena.h"
#include "util.h"

/* Set to 0 to log the bad attaches but let them through, i.e. to reproduce
 * the crash deliberately. 1 is the shipping value. */
#define GL_GUARD_ENFORCE 1

/* GL types and constants are declared locally rather than by including
 * GLES2/gl2.h. Nothing else in this file needs a GL header, and not depending
 * on one keeps the build independent of which mesa package is installed.
 * These typedefs are identical to the ones in gl2.h on aarch64, so including
 * that header alongside this file would still be legal. */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int          GLint;
typedef int          GLsizei;

#define GL_NO_ERROR                     0x0000
#define GL_TEXTURE_2D                   0x0DE1
#define GL_TEXTURE_CUBE_MAP             0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X  0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z  0x851A
#define GL_TEXTURE0                     0x84C0
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5

/* ---------------------------------------------------------------------- */
/* Texture storage table                                                   */
/* ---------------------------------------------------------------------- */

#define TEX_LEVELS 16
#define TEX_SLOTS  4096            /* power of two; ~272 KB of .bss */
#define TEX_TOMB   0xFFFFFFFFu     /* deleted marker; never a real GL name */

typedef struct {
  unsigned       name;             /* 0 = empty, TEX_TOMB = deleted */
  unsigned char  opaque;           /* storage from a path we cannot measure */
  unsigned short w[TEX_LEVELS];
  unsigned short h[TEX_LEVELS];
  unsigned       bytes[TEX_LEVELS];
} TexRec;

static TexRec   g_tex[TEX_SLOTS];
static unsigned g_tex_live;
static int      g_tex_full;
static unsigned g_blocked;

/* Texture bytes, so that "the heap is full" can be attributed.
 *
 * The heap figure alone cannot distinguish "the game is caching 700 MB of
 * textures" from "something else is eating the heap and textures are
 * incidental" -- and the two have opposite fixes. This is the cheapest place
 * to settle it: every path that gives a texture storage passes through here. */
static unsigned long long g_bytes_live;
static unsigned long long g_bytes_uploaded;
static unsigned long long g_bytes_freed;
static unsigned           g_uploads;
static unsigned           g_peak_live_mb;

/* Uncompressed bytes per pixel for a GLES format/type pair. Approximate by
 * design -- the driver may pad or expand -- but the ratios are what matter. */
static unsigned bytes_per_pixel(GLenum format, GLenum type) {
  unsigned comps;
  switch (format) {
    case 0x1902: comps = 1; break;  /* GL_DEPTH_COMPONENT */
    case 0x1903: comps = 1; break;  /* GL_RED           */
    case 0x1906: comps = 1; break;  /* GL_ALPHA         */
    case 0x1907: comps = 3; break;  /* GL_RGB           */
    case 0x1908: comps = 4; break;  /* GL_RGBA          */
    case 0x1909: comps = 1; break;  /* GL_LUMINANCE     */
    case 0x190A: comps = 2; break;  /* GL_LUMINANCE_ALPHA */
    case 0x8227: comps = 2; break;  /* GL_RG            */
    case 0x80E1: comps = 4; break;  /* GL_BGRA_EXT      */
    case 0x84F9: comps = 4; break;  /* GL_DEPTH_STENCIL */
    default:     comps = 4; break;
  }
  switch (type) {
    case 0x1401: return comps;      /* GL_UNSIGNED_BYTE        */
    case 0x1403: return comps * 2;  /* GL_UNSIGNED_SHORT       */
    case 0x1405: return comps * 4;  /* GL_UNSIGNED_INT         */
    case 0x1406: return comps * 4;  /* GL_FLOAT                */
    case 0x140B: return comps * 2;  /* GL_HALF_FLOAT_OES       */
    case 0x8033: return 2;          /* GL_UNSIGNED_SHORT_4_4_4_4 */
    case 0x8034: return 2;          /* GL_UNSIGNED_SHORT_5_5_5_1 */
    case 0x8363: return 2;          /* GL_UNSIGNED_SHORT_5_6_5   */
    case 0x84FA: return 4;          /* GL_UNSIGNED_INT_24_8    */
    default:     return comps;
  }
}

static unsigned tex_hash(unsigned name) {
  return (name * 2654435761u) & (TEX_SLOTS - 1u);
}

static TexRec *tex_find(unsigned name) {
  unsigned i = tex_hash(name);
  for (unsigned n = 0; n < TEX_SLOTS; n++) {
    TexRec *r = &g_tex[(i + n) & (TEX_SLOTS - 1u)];
    if (r->name == name) return r;
    if (r->name == 0) return NULL;   /* empty slot ends the probe chain */
  }
  return NULL;
}

static TexRec *tex_intern(unsigned name) {
  unsigned i = tex_hash(name);
  TexRec *tomb = NULL;
  for (unsigned n = 0; n < TEX_SLOTS; n++) {
    TexRec *r = &g_tex[(i + n) & (TEX_SLOTS - 1u)];
    if (r->name == name) return r;
    if (r->name == TEX_TOMB) { if (!tomb) tomb = r; continue; }
    if (r->name == 0) {
      /* Reuse the earliest tombstone if there was one -- it sits before this
       * empty slot in probe order, so tex_find will still reach it. */
      if (tomb) r = tomb;
      memset(r, 0, sizeof(*r));
      r->name = name;
      g_tex_live++;
      return r;
    }
  }

  if (!g_tex_full) {
    g_tex_full = 1;
    debug_log("[gl] *** texture table full at %u live entries -- the FBO "
              "attach guard is now DISABLED, because it can no longer tell a "
              "texture that was never uploaded from one it has merely "
              "forgotten. Raise TEX_SLOTS in gl_guard.c. ***\n", g_tex_live);
  }
  return NULL;
}

static void tex_forget(unsigned name) {
  TexRec *r = tex_find(name);
  if (!r) return;
  for (int l = 0; l < TEX_LEVELS; l++) {
    if (g_bytes_live >= r->bytes[l]) g_bytes_live -= r->bytes[l];
    g_bytes_freed += r->bytes[l];
  }
  memset(r, 0, sizeof(*r));
  r->name = TEX_TOMB;
  if (g_tex_live) g_tex_live--;
}

static void tex_record(unsigned name, GLint level, GLsizei w, GLsizei h,
                       unsigned bytes) {
  if (!name || level < 0 || level >= TEX_LEVELS) return;
  TexRec *r = tex_intern(name);
  if (!r) return;

  r->w[level] = (w > 0 && w < 65536) ? (unsigned short)w : 0;
  r->h[level] = (h > 0 && h < 65536) ? (unsigned short)h : 0;
  if (!r->w[level] || !r->h[level]) bytes = 0;

  /* Re-uploading a level replaces its storage rather than adding to it. */
  if (g_bytes_live >= r->bytes[level]) g_bytes_live -= r->bytes[level];
  r->bytes[level] = bytes;
  g_bytes_live += bytes;

  if (bytes) { g_bytes_uploaded += bytes; g_uploads++; }

  unsigned mb = (unsigned)(g_bytes_live >> 20);
  if (mb > g_peak_live_mb) g_peak_live_mb = mb;
}

/* ---------------------------------------------------------------------- */
/* Binding state                                                           */
/* ---------------------------------------------------------------------- */

#define MAX_UNITS 32

static unsigned g_unit;
static unsigned g_bound_2d[MAX_UNITS];
static unsigned g_bound_cube[MAX_UNITS];

static int is_cube_face(GLenum t) {
  return t >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && t <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

static unsigned bound_texture(GLenum target) {
  unsigned u = (g_unit < MAX_UNITS) ? g_unit : 0;
  if (target == GL_TEXTURE_2D) return g_bound_2d[u];
  if (target == GL_TEXTURE_CUBE_MAP || is_cube_face(target)) return g_bound_cube[u];
  return 0;
}

/* ---------------------------------------------------------------------- */
/* Error drain                                                             */
/* ---------------------------------------------------------------------- */

unsigned gl_guard_drain_errors(const char *when) {
  typedef GLenum (*get_err_t)(void);
  static get_err_t get_err;
  static int probed;

  /* Say whether the drain is even live.
   *
   * This used to return silently when eglGetProcAddress had not produced a
   * glGetError, which made "301 frames and no GL errors" an unfalsifiable
   * claim: an empty log looked identical whether there were no errors or no
   * way to ask. One line settles it permanently. */
  if (!probed) {
    probed = 1;
    get_err = (get_err_t)eglGetProcAddress("glGetError");
    if (get_err)
      debug_log("[gl] glGetError resolved -- GL error reporting is live, so an "
                "absence of [gl] lines below is a real absence of errors\n");
    else
      debug_log("[gl] *** glGetError did NOT resolve through eglGetProcAddress. "
                "The error drain is a no-op and every 'no GL errors' result "
                "below is vacuous. ***\n");
  }
  if (!get_err) return 0;

  static unsigned seen[16];
  static int nseen;
  unsigned first = 0;

  for (int guard = 0; guard < 32; guard++) {
    unsigned e = get_err();
    if (e == GL_NO_ERROR) break;
    if (!first) first = e;

    int known = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == e) known = 1;
    if (known) continue;
    if (nseen < 16) seen[nseen++] = e;

    const char *name =
        e == 0x0500 ? "GL_INVALID_ENUM" :
        e == 0x0501 ? "GL_INVALID_VALUE" :
        e == 0x0502 ? "GL_INVALID_OPERATION" :
        e == 0x0503 ? "GL_STACK_OVERFLOW" :
        e == 0x0504 ? "GL_STACK_UNDERFLOW" :
        e == 0x0505 ? "GL_OUT_OF_MEMORY" :
        e == 0x0506 ? "GL_INVALID_FRAMEBUFFER_OPERATION" : "unknown";

    debug_log("[gl] *** %s (0x%x) reported at %s -- the game does not check "
              "for this, so it has been silent until now ***\n", name, e, when);

    /* The heap belongs on the same line of enquiry: mesa/nouveau backs its
     * buffer objects with the newlib heap, so a full heap surfaces as
     * GL_OUT_OF_MEMORY here. Correlating two figures from different parts of
     * a long log is how that took several rounds to see. */
    heap_report();
  }
  return first;
}

unsigned gl_guard_blocked_attaches(void) { return g_blocked; }

/* Called on the same cadence as heap_report, so the two sit together.
 *
 * The number to read is "live". Compare it against the [heap] line directly
 * above: if live textures account for most of the heap, the game's cache is
 * the appetite and the fix is to bound it. If they do not, something else owns
 * the heap and raising NEWLIB_HEAP_MAX only moves the wall. */
void gl_guard_report(void) {
  debug_log("[gl] textures: %u live objects, %llu MB live (peak %u MB); "
            "%u uploads totalling %llu MB, %llu MB freed; %u attaches blocked\n",
            g_tex_live,
            (unsigned long long)(g_bytes_live >> 20), g_peak_live_mb,
            g_uploads,
            (unsigned long long)(g_bytes_uploaded >> 20),
            (unsigned long long)(g_bytes_freed >> 20),
            g_blocked);
}

/* ---------------------------------------------------------------------- */
/* Wrappers                                                                */
/* ---------------------------------------------------------------------- */

typedef void (*fn_active_texture_t)(GLenum);
typedef void (*fn_bind_texture_t)(GLenum, GLuint);
typedef void (*fn_delete_textures_t)(GLsizei, const GLuint *);
typedef void (*fn_teximage2d_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                GLenum, GLenum, const void *);
typedef void (*fn_cteximage2d_t)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint,
                                 GLsizei, const void *);
typedef void (*fn_copyteximage2d_t)(GLenum, GLint, GLenum, GLint, GLint,
                                    GLsizei, GLsizei, GLint);
typedef void (*fn_texstorage2d_t)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*fn_generate_mipmap_t)(GLenum);
typedef void (*fn_eglimage_tex2d_t)(GLenum, void *);
typedef void (*fn_fbtex2d_t)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*fn_checkfb_t)(GLenum);

/* Held as void * rather than as the typed pointer so that the lookup table
 * below can write them through a void ** without an aliasing violation. */
static void *r_active_texture;
static void *r_bind_texture;
static void *r_delete_textures;
static void *r_teximage2d;
static void *r_cteximage2d;
static void *r_copyteximage2d;
static void *r_texstorage2d;
static void *r_generate_mipmap;
static void *r_eglimage_tex2d;
static void *r_fbtex2d;
static void *r_checkfb;

static void nx_glActiveTexture(GLenum unit) {
  unsigned u = (unsigned)unit - GL_TEXTURE0;   /* wraps if unit < GL_TEXTURE0 */
  g_unit = (u < MAX_UNITS) ? u : 0;
  ((fn_active_texture_t)r_active_texture)(unit);
}

static void nx_glBindTexture(GLenum target, GLuint texture) {
  unsigned u = (g_unit < MAX_UNITS) ? g_unit : 0;
  if      (target == GL_TEXTURE_2D)       g_bound_2d[u]   = texture;
  else if (target == GL_TEXTURE_CUBE_MAP) g_bound_cube[u] = texture;
  ((fn_bind_texture_t)r_bind_texture)(target, texture);
}

static void nx_glDeleteTextures(GLsizei n, const GLuint *textures) {
  if (textures) {
    for (GLsizei i = 0; i < n; i++) {
      unsigned t = textures[i];
      if (!t) continue;
      tex_forget(t);
      /* GL unbinds a deleted texture everywhere it was bound. Mirror that, or
       * a later upload is recorded against a name that no longer exists. */
      for (unsigned k = 0; k < MAX_UNITS; k++) {
        if (g_bound_2d[k]   == t) g_bound_2d[k]   = 0;
        if (g_bound_cube[k] == t) g_bound_cube[k] = 0;
      }
    }
  }
  ((fn_delete_textures_t)r_delete_textures)(n, textures);
}

static void nx_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, const void *pixels) {
  unsigned tex = bound_texture(target);

  /* Clear stale error state first, so what is read afterwards belongs to this
   * call and not to something that failed earlier. */
  gl_guard_drain_errors("before a texture upload");
  ((fn_teximage2d_t)r_teximage2d)(target, level, internalformat, width, height,
                                  border, format, type, pixels);
  unsigned err = gl_guard_drain_errors("a glTexImage2D upload");

  unsigned bpp = bytes_per_pixel(format, type);
  tex_record(tex, level, err ? 0 : width, err ? 0 : height,
             err ? 0 : (unsigned)width * (unsigned)height * bpp);
}

static void nx_glCompressedTexImage2D(GLenum target, GLint level,
                                      GLenum internalformat, GLsizei width,
                                      GLsizei height, GLint border,
                                      GLsizei imageSize, const void *data) {
  unsigned tex = bound_texture(target);
  gl_guard_drain_errors("before a compressed texture upload");
  ((fn_cteximage2d_t)r_cteximage2d)(target, level, internalformat, width,
                                    height, border, imageSize, data);
  unsigned err = gl_guard_drain_errors("a glCompressedTexImage2D upload");
  tex_record(tex, level, err ? 0 : width, err ? 0 : height,
             err ? 0 : (unsigned)imageSize);
}

static void nx_glCopyTexImage2D(GLenum target, GLint level,
                                GLenum internalformat, GLint x, GLint y,
                                GLsizei width, GLsizei height, GLint border) {
  unsigned tex = bound_texture(target);
  gl_guard_drain_errors("before glCopyTexImage2D");
  ((fn_copyteximage2d_t)r_copyteximage2d)(target, level, internalformat, x, y,
                                          width, height, border);
  unsigned err = gl_guard_drain_errors("glCopyTexImage2D");
  tex_record(tex, level, err ? 0 : width, err ? 0 : height,
             err ? 0 : (unsigned)width * (unsigned)height * 4);
}

static void nx_glTexStorage2D(GLenum target, GLsizei levels,
                              GLenum internalformat, GLsizei width,
                              GLsizei height) {
  unsigned tex = bound_texture(target);
  gl_guard_drain_errors("before glTexStorage2D");
  ((fn_texstorage2d_t)r_texstorage2d)(target, levels, internalformat, width,
                                      height);
  unsigned err = gl_guard_drain_errors("glTexStorage2D");

  GLsizei w = width, h = height;
  for (GLsizei l = 0; l < levels && l < TEX_LEVELS; l++) {
    tex_record(tex, (GLint)l, err ? 0 : w, err ? 0 : h,
               err ? 0 : (unsigned)w * (unsigned)h * 4);
    w = (w > 1) ? w / 2 : 1;
    h = (h > 1) ? h / 2 : 1;
  }
}

static void nx_glGenerateMipmap(GLenum target) {
  unsigned tex = bound_texture(target);
  gl_guard_drain_errors("before glGenerateMipmap");
  ((fn_generate_mipmap_t)r_generate_mipmap)(target);
  unsigned err = gl_guard_drain_errors("glGenerateMipmap");
  if (err) return;

  TexRec *r = tex_find(tex);
  if (!r || !r->w[0] || !r->h[0]) return;
  GLsizei w = r->w[0], h = r->h[0];
  unsigned bpp = r->bytes[0] / ((unsigned)w * (unsigned)h);
  if (!bpp) bpp = 4;
  for (int l = 1; l < TEX_LEVELS; l++) {
    w = (w > 1) ? w / 2 : 1;
    h = (h > 1) ? h / 2 : 1;
    tex_record(tex, l, w, h, (unsigned)w * (unsigned)h * bpp);
  }
}

/* An EGLImage gives a texture storage by a route whose dimensions we cannot
 * see. Mark it as backed rather than measured, so the guard lets it through
 * instead of blocking something that is perfectly valid. */
static void nx_glEGLImageTargetTexture2DOES(GLenum target, void *image) {
  unsigned tex = bound_texture(target);
  ((fn_eglimage_tex2d_t)r_eglimage_tex2d)(target, image);
  if (!gl_guard_drain_errors("glEGLImageTargetTexture2DOES") && tex) {
    TexRec *r = tex_intern(tex);
    if (r) r->opaque = 1;
  }
}

/* THE GUARD. See the header comment for the full derivation. */
static void nx_glFramebufferTexture2D(GLenum target, GLenum attachment,
                                      GLenum textarget, GLuint texture,
                                      GLint level) {
  fn_fbtex2d_t call = (fn_fbtex2d_t)r_fbtex2d;

  /* texture == 0 is a detach and is always legal. If tracking has overflowed
   * we can no longer distinguish "never uploaded" from "forgotten", so fail
   * open -- a wrongly blocked attach breaks rendering, and at that point the
   * table-full warning above is the thing to act on. */
  if (texture == 0 || g_tex_full) {
    call(target, attachment, textarget, texture, level);
    return;
  }

  GLint lv = (level >= 0 && level < TEX_LEVELS) ? level : 0;
  TexRec *r = tex_find(texture);
  if (r && (r->opaque || (r->w[lv] && r->h[lv]))) {
    call(target, attachment, textarget, texture, level);
    return;
  }

  g_blocked++;
  if (g_blocked <= 16 || (g_blocked % 256) == 0) {
    if (!r)
      debug_log("[gl] *** BLOCKED glFramebufferTexture2D(attachment=0x%x, "
                "textarget=0x%x, texture=%u, level=%d): no successful upload "
                "was ever recorded for this texture. Attaching it would store "
                "a NULL pipe_resource into strb->texture and fault in "
                "st_update_renderbuffer_surface reading ->format at +0xe. "
                "Detaching instead. (%u blocked so far)\n",
                attachment, textarget, texture, level, g_blocked);
    else
      debug_log("[gl] *** BLOCKED glFramebufferTexture2D(attachment=0x%x, "
                "textarget=0x%x, texture=%u, level=%d): level %d has no "
                "storage (level 0 is %ux%u). Either the upload failed or it "
                "was made with zero dimensions. Detaching instead. "
                "(%u blocked so far)\n",
                attachment, textarget, texture, level, lv,
                (unsigned)r->w[0], (unsigned)r->h[0], g_blocked);

    /* Drain here rather than at swap. The whole reason this was invisible for
     * several rounds is that the swap-time drain cannot run in the frame that
     * crashes -- eglSwapBuffers is never reached. */
    gl_guard_drain_errors("the blocked FBO attach");
    heap_report();
  }

#if GL_GUARD_ENFORCE
  /* Detach rather than simply skipping. Skipping would leave whatever was
   * previously bound at this attachment point still attached, and the game
   * would render into the wrong texture -- a subtler bug than the one being
   * fixed. An empty attachment point gives exactly the
   * GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT the spec asks for. */
  call(target, attachment, textarget, 0, 0);
#else
  call(target, attachment, textarget, texture, level);
#endif
}

/* Worth seeing whether the game checks completeness at all -- if it does, the
 * blocked attaches above will show up here as a status it can act on. */
static GLenum nx_glCheckFramebufferStatus(GLenum target) {
  GLenum st = ((fn_checkfb_t)r_checkfb)(target);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    static unsigned n;
    if (++n <= 16 || (n % 256) == 0)
      debug_log("[gl] glCheckFramebufferStatus -> 0x%x (not complete); the "
                "game did ask, so it can handle this (%u times)\n", st, n);
  }
  return st;
}

/* ---------------------------------------------------------------------- */
/* Lookup                                                                  */
/* ---------------------------------------------------------------------- */

typedef struct {
  const char *name;
  void       *wrapper;
  void      **real;
} Hook;

static const Hook g_hooks[] = {
  { "glActiveTexture",               (void *)nx_glActiveTexture,            &r_active_texture   },
  { "glBindTexture",                 (void *)nx_glBindTexture,              &r_bind_texture     },
  { "glDeleteTextures",              (void *)nx_glDeleteTextures,           &r_delete_textures  },
  { "glTexImage2D",                  (void *)nx_glTexImage2D,               &r_teximage2d       },
  { "glCompressedTexImage2D",        (void *)nx_glCompressedTexImage2D,     &r_cteximage2d      },
  { "glCopyTexImage2D",              (void *)nx_glCopyTexImage2D,           &r_copyteximage2d   },
  { "glTexStorage2D",                (void *)nx_glTexStorage2D,             &r_texstorage2d     },
  { "glGenerateMipmap",              (void *)nx_glGenerateMipmap,           &r_generate_mipmap  },
  { "glEGLImageTargetTexture2DOES",  (void *)nx_glEGLImageTargetTexture2DOES, &r_eglimage_tex2d },
  { "glFramebufferTexture2D",        (void *)nx_glFramebufferTexture2D,     &r_fbtex2d          },
  { "glCheckFramebufferStatus",      (void *)nx_glCheckFramebufferStatus,   &r_checkfb          },
};

void *gl_guard_lookup(const char *symbol) {
  if (!symbol) return NULL;

  for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); i++) {
    if (strcmp(symbol, g_hooks[i].name) != 0) continue;

    if (!*g_hooks[i].real) {
      *g_hooks[i].real = (void *)eglGetProcAddress(symbol);
      if (!*g_hooks[i].real) {
        /* Do NOT hand back a wrapper that cannot call through. Six multi-round
         * failures in this port were all "work handed to the shim and silently
         * not performed"; refusing to intercept is the safe direction. */
        debug_log("[gl] %s did not resolve through eglGetProcAddress -- NOT "
                  "intercepting it\n", symbol);
        return NULL;
      }
      debug_log("[gl] intercepting %s\n", symbol);
    }
    return g_hooks[i].wrapper;
  }
  return NULL;
}
