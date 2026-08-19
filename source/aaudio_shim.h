/* aaudio_shim.h -- see aaudio_shim.c for why this exists. */
#ifndef AAUDIO_SHIM_H
#define AAUDIO_SHIM_H

#include <stdint.h>

/* Returns the shim's implementation of an AAudio entry point, or NULL (with a
 * log line naming it) if the symbol is not served. */
void *aaudio_shim_lookup(const char *symbol);

/* Creates the thread that will serve the engine's AAudio data callback. Call
 * once from host startup, BEFORE the game runs -- see aaudio_shim.c for why it
 * must not happen inside a P/Invoke. Harmless if the engine turns out to use
 * blocking writes: the thread idles. */
void aaudio_shim_init(void);

/* Retained so there is one obvious place to hook per-frame audio work.
 * Deliberately empty: the data callback must not run on the frame loop. */
void aaudio_shim_pump(void);

/* True while the engine's mixer callback is executing, with how long it has
 * been in there. The callback is managed code called directly, not a JNI call,
 * so it never appears in the JNI breadcrumb -- without this a mixer that never
 * returns is invisible in a stall report. */
int aaudio_shim_in_callback(unsigned long long *ms);

#endif
