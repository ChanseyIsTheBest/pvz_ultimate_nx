#ifndef PVZU_ANDROID_CLASSES_H
#define PVZU_ANDROID_CLASSES_H

#include <jni.h>

#include "jni_ref.h"   /* FakeClass */

#include <jni.h>
#include <stddef.h>

void android_classes_init(void);

/* A global ref to the fake Context. Hand this to anything that wants one. */
jobject android_get_context(void);

/* Fills the shared KeyEvent instance and returns a ref to it. Not reentrant --
 * the managed side reads it synchronously inside onKeyDown/onKeyUp, so one
 * instance is enough, but do not queue these. */
jobject android_make_key_event(int action, int keycode, int unicode);

/* As above, with an explicit meta state (META_SHIFT_ON is 0x1). */
jobject android_make_key_event_meta(int action, int keycode, int unicode,
                                    int meta);

/* NDK AAsset API, handed out by dlsym on libandroid.so. This is the path
 * MonoGame's content pipeline actually takes. */
void       *nx_AAssetManager_fromJava(void *env, void *obj);
void       *nx_AAssetManager_open(void *mgr, const char *filename, int mode);
int         nx_AAsset_read(void *asset, void *buf, size_t count);
long        nx_AAsset_seek(void *asset, long off, int whence);
long        nx_AAsset_getLength(void *asset);
long        nx_AAsset_getRemainingLength(void *asset);
const void *nx_AAsset_getBuffer(void *asset);
void        nx_AAsset_close(void *asset);
int         nx_AAsset_isAllocated(void *asset);
int         nx_AAsset_openFileDescriptor(void *asset, long *start, long *length);

/* Activity inherits from this, as it does on Android via ContextWrapper.
 * Everything the game asks an Activity for -- assets, files dir, package name
 * -- is implemented here already. */
extern FakeClass g_class_Context;

/* Force any pending SharedPreferences write out now. commit()/apply() already
 * persist, so this is only for shutdown, where the game may never call either.
 * A no-op when nothing has changed. */
void android_prefs_flush(void);

#endif
