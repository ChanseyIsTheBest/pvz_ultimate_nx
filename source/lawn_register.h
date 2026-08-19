#ifndef PVZU_LAWN_REGISTER_H
#define PVZU_LAWN_REGISTER_H

#include <jni.h>

/* Register the game's native methods through ManagedPeer.registerNativeMembers,
 * using descriptors taken from classes.dex. Nothing else does this: the Java
 * <clinit> that normally would is never executed here. */
void lawn_register_natives(JNIEnv *env);

/* The InputConnection's natives, bound on first use rather than at startup:
 * the game constructs that class inside onCreateInputConnection, so it does not
 * exist when the others are registered. Safe to call repeatedly. */
void lawn_register_input_connection(JNIEnv *env);

#endif
