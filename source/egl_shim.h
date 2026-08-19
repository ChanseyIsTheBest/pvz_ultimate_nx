#ifndef PVZU_EGL_SHIM_H
#define PVZU_EGL_SHIM_H

#include <EGL/egl.h>

/* Returns our replacement for an intercepted EGL entry point, or NULL to let
 * the caller fall through to mesa. dl_shim consults this before its
 * eglGetProcAddress fallthrough, so the substitution holds however the engine
 * resolves the symbol. */
void *egl_shim_lookup(const char *symbol);

/* Total presented frames. The host loop uses this to tell a frame that drew
 * from one that did not -- see the pacing note in main.c. */
/* main.c hands its bootstrap surface over after creating it; the shim releases
 * it when the game asks for the window. Both are safe to call more than once. */
void egl_shim_adopt_host_surface(EGLDisplay dpy, EGLSurface surf);
void egl_shim_release_host_surface(void);

unsigned egl_shim_swap_count(void);

EGLSurface egl_shim_surface(void);
EGLDisplay egl_shim_display(void);

#endif
