#ifndef PVZU_JAVA_LANG_H
#define PVZU_JAVA_LANG_H

#include <jni.h>

/* Register the core java.lang classes. Must run before JNI_OnLoad -- the GC
 * bridge looks up java/lang/Runtime there and aborts if it is absent. */
void java_lang_init(void);

/* Global ref to the fake system ClassLoader. JavaInteropRuntime.init takes one
 * as its first argument, and passing NULL leaves the runtime with nothing to
 * resolve types against. */
jobject java_lang_classloader(void);

#endif
