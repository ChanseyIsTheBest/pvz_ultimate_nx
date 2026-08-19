#ifndef PVZU_ANDROID_RUNTIME_H
#define PVZU_ANDROID_RUNTIME_H

#include <jni.h>

/* Register the Java.Interop runtime classes, stream adapters, callbacks and
 * framework bases enumerated from classes.dex. Must run before JNI_OnLoad. */
void android_runtime_init(void);


/* The view the Activity installed via setContentView, or NULL if it has not
 * run yet. The game builds its own LawnSurfaceView rather than using the one
 * the loader constructs, and this is that object. */
jobject android_content_view(void);

#endif