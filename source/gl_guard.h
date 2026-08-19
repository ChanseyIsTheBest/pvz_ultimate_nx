/* gl_guard.h -- GL-level interception.
 *
 * Two jobs, both of which exist because of the same crash:
 *
 *   1. Stop glFramebufferTexture2D from attaching a texture that has no
 *      storage behind it. mesa dereferences the null pipe_resource without
 *      checking, so this is a hard fault inside the driver rather than the
 *      GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT the spec asks for.
 *
 *   2. Own the glGetError drain, so that it can be called from the point of
 *      a suspicious call rather than only at swap time. A drain that only
 *      runs on eglSwapBuffers structurally cannot report an error raised in
 *      the frame that crashes -- the swap never happens.
 *
 * dl_shim routes every symbol beginning "gl" or "egl" through
 * egl_shim_lookup, which consults this first. Silk.NET resolves the whole
 * GLES surface dynamically, so there is no static link to patch and this is
 * the only interception point needed.
 */
#ifndef PVZU_GL_GUARD_H
#define PVZU_GL_GUARD_H

/* Our replacement for an intercepted GL entry point, or NULL to fall through.
 *
 * Returns NULL if the underlying function does not resolve, so that a wrapper
 * which cannot call through is never handed to the game. Silently swallowing
 * a call is the single most expensive bug shape in this port's history. */
void *gl_guard_lookup(const char *symbol);

/* Drain glGetError, printing each distinct code once with the heap alongside.
 * Returns the first error found, or 0. `when` appears in the log line. */
unsigned gl_guard_drain_errors(const char *when);

/* How many FBO attaches have been blocked. Nonzero means the port hit the
 * missing-storage path, which is worth stating plainly at shutdown. */
unsigned gl_guard_blocked_attaches(void);

/* Live/uploaded/freed texture bytes. Print on the same cadence as
 * heap_report() -- the two numbers only mean something next to each other. */
void gl_guard_report(void);

#endif /* PVZU_GL_GUARD_H */
