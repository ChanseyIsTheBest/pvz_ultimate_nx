/* ime_shim.h -- Android InputMethodManager backed by the Switch keyboard.
 *
 * The game overrides onCreateInputConnection/onCheckIsTextEditor, so it expects
 * a soft keyboard. showSoftInput queues a request; ime_pump() runs the keyboard
 * from the frame loop and feeds the result back in. See ime_shim.c for why it
 * is deferred rather than run inline.
 */
#ifndef PVZU_IME_SHIM_H
#define PVZU_IME_SHIM_H

#include <jni.h>

void ime_init(void);

/* Call once per frame, at the top of the loop, OUTSIDE any managed call.
 * Opens the keyboard if one was requested, blocks while it is up, and delivers
 * the text. A no-op otherwise, so calling it unconditionally is fine. */
void ime_pump(void);

/* Open the keyboard without the game having asked.
 *
 * The fallback for a field the game never raises an IME for: the text is
 * delivered through exactly the same path, so if the field is listening it
 * fills in. Bound to a button in input.c. */
void ime_request_keyboard(void);

/* A showSoftInput is queued but the keyboard has not run yet. */
int ime_keyboard_pending(void);

/* The InputMethodManager instance, for Context.getSystemService. */
jobject ime_input_method_manager(void);

#endif /* PVZU_IME_SHIM_H */
