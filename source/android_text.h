/* android_text.h -- a real editable text buffer for the IME path.
 *
 * LawnInputConnection.getEditable() returns a SpannableStringBuilder that the
 * managed BaseInputConnection appends committed text to. While it was a stub,
 * toString() returned the class name and that is what appeared in the game's
 * field. See android_text.c.
 */
#ifndef PVZU_ANDROID_TEXT_H
#define PVZU_ANDROID_TEXT_H

#include <jni.h>

void android_text_init(void);

/* The current text of an editable, or NULL if the object is not one. Lets the
 * keyboard open pre-filled with what the field already holds. */
const char *android_text_get(jobject o);

/* Replace an editable's whole contents. Returns 0 if the object is not one.
 * The delivery path uses this rather than commitText -- see android_text.c. */
int android_text_set(jobject o, const char *text);

#endif /* PVZU_ANDROID_TEXT_H */
