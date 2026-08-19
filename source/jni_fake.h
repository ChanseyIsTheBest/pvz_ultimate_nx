#ifndef PVZU_JNI_FAKE_H
#define PVZU_JNI_FAKE_H

#include <jni.h>
#include "jni_ref.h"

void     jni_fake_init(void);
JNIEnv  *jni_get_env(void);

/* Logs, once per object, when a view-like object is constructed. */
void     jni_note_object_created(struct FakeObject *o);
JavaVM  *jni_get_vm(void);

void       jni_register_class(FakeClass *c);
FakeClass *jni_find_class(const char *name);

extern FakeClass g_class_Class;

/* Raise a pending exception. Anything returning NULL for a failed lookup
 * must call this, or callers that test ExceptionCheck see success. */
void jni_raise(JNIEnv *env, const char *cls, const char *detail);

/* Manufacture a stub field so a lookup succeeds rather than aborting. Object
 * fields get a stub instance, not null, because callers wrap the value. */
FakeField *jni_stub_field(FakeClass *c, const char *name, const char *sig);
FakeField *jni_find_stub_field(FakeClass *c, const char *name, const char *sig);

#endif
