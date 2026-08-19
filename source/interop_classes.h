#ifndef PVZU_INTEROP_CLASSES_H
#define PVZU_INTEROP_CLASSES_H

#include <jni.h>

/* Register the Java.Interop bootstrap surface. Call before JNI_OnLoad -- the
 * image looks these up by name during its own initialisation. */
void interop_classes_init(void);

/* Call after JNI_OnLoad. Verifies that ManagedPeer.registerNativeMembers got
 * an implementation; returns -1 with a diagnosis if not. Failing here is far
 * clearer than failing at Stage 4 with a message about n_doFrame. */
int  interop_check_bootstrap(void);

/* Build the managed peer for a Java instance via ManagedPeer.construct.
 * Constructing a peer is what causes Java.Interop to register that type's
 * native methods, which nothing else here triggers. */
/* ctor_sig is a full JNI method signature -- "()V", "(Landroid/content/Context;)V".
 * Java.Interop rejects anything not starting with '('. args must match the
 * parameter list within it. */
int  interop_construct_peer(JNIEnv *env, jobject instance, const char *ctor_sig,
                            jobject *args, int nargs);

/* Peer reference counters. If the port ever runs for hours, a large and
 * ever-growing gap between these is the first place to look for a leak. */
void interop_peer_stats(unsigned *adds, unsigned *clears);

#endif
