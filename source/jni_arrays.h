#ifndef PVZU_JNI_ARRAYS_H
#define PVZU_JNI_ARRAYS_H

#include <jni.h>

#include "jni_ref.h"

/* THE string representation, shared by every file that makes or reads one.
 * Three files previously declared their own layout and cast between them,
 * which works for `utf` (first field in all of them) and reads past the
 * allocation for anything after it. Define it once. */
typedef struct FakeString {
  FakeObject hdr;
  char      *utf;      /* owned          */
  jsize      len8;
  jchar     *utf16;    /* built on demand */
  jsize      len16;
} FakeString;

extern FakeClass g_class_String;

/* Registers the per-kind array classes ([I, [B, [Ljava/lang/Object; ...).
 * Must run with the other core registrations: FindClass("[I") has to resolve
 * to the same object GetObjectClass returns for an int[], or Mono.Android's
 * array cast check fails. */
void jni_arrays_init(void);

/* Allocates and returns a LOCAL ref. */
jstring jni_make_string(const char *utf);

/* Helpers for building return values from the fake classes. */
jobject jni_make_object_array(jsize len, jobject *initial);
jobject jni_make_byte_array(const void *bytes, jsize len);

/* Real element copy for System.arraycopy. Returns -1 on a type or
 * bounds mismatch, which the caller should surface as an exception. */
int jni_array_copy(jobject src, int srcPos, jobject dst, int dstPos, int len);

/* Calls a function pointer that came from RegisterNatives, unpacking the jvalue
 * array into real argument registers. Aborts with the signature printed if the
 * shape is not covered -- adding a case is a two-minute fix. */
jvalue jni_invoke_native(JNIEnv *env, void *fn, jobject self,
                         const char *sig, const jvalue *args);

/* Vtable entries, referenced from jni_fake.c's designated initializer. */
jsize        JNICALL f_GetArrayLength(JNIEnv *, jarray);
jobjectArray JNICALL f_NewObjectArray(JNIEnv *, jsize, jclass, jobject);
jobject      JNICALL f_GetObjectArrayElement(JNIEnv *, jobjectArray, jsize);
void         JNICALL f_SetObjectArrayElement(JNIEnv *, jobjectArray, jsize, jobject);
void        *JNICALL f_GetPrimitiveArrayCritical(JNIEnv *, jarray, jboolean *);
void         JNICALL f_ReleasePrimitiveArrayCritical(JNIEnv *, jarray, void *, jint);

#define DECL_PRIM_ARRAY(Name, Type)                                            \
  Type##Array JNICALL f_New##Name##Array(JNIEnv *, jsize);                     \
  Type *JNICALL f_Get##Name##ArrayElements(JNIEnv *, Type##Array, jboolean *);  \
  void  JNICALL f_Release##Name##ArrayElements(JNIEnv *, Type##Array, Type *, jint); \
  void  JNICALL f_Get##Name##ArrayRegion(JNIEnv *, Type##Array, jsize, jsize, Type *); \
  void  JNICALL f_Set##Name##ArrayRegion(JNIEnv *, Type##Array, jsize, jsize, const Type *);

DECL_PRIM_ARRAY(Boolean, jboolean)
DECL_PRIM_ARRAY(Byte,    jbyte)
DECL_PRIM_ARRAY(Char,    jchar)
DECL_PRIM_ARRAY(Short,   jshort)
DECL_PRIM_ARRAY(Int,     jint)
DECL_PRIM_ARRAY(Long,    jlong)
DECL_PRIM_ARRAY(Float,   jfloat)
DECL_PRIM_ARRAY(Double,  jdouble)
#undef DECL_PRIM_ARRAY

jfieldID JNICALL f_GetFieldID(JNIEnv *, jclass, const char *, const char *);
jfieldID JNICALL f_GetStaticFieldID(JNIEnv *, jclass, const char *, const char *);

#define DECL_FIELD(Name, Type)                                                 \
  Type JNICALL f_Get##Name##Field(JNIEnv *, jobject, jfieldID);                \
  void JNICALL f_Set##Name##Field(JNIEnv *, jobject, jfieldID, Type);          \
  Type JNICALL f_GetStatic##Name##Field(JNIEnv *, jclass, jfieldID);           \
  void JNICALL f_SetStatic##Name##Field(JNIEnv *, jclass, jfieldID, Type);

DECL_FIELD(Object,  jobject)
DECL_FIELD(Boolean, jboolean)
DECL_FIELD(Byte,    jbyte)
DECL_FIELD(Char,    jchar)
DECL_FIELD(Short,   jshort)
DECL_FIELD(Int,     jint)
DECL_FIELD(Long,    jlong)
DECL_FIELD(Float,   jfloat)
DECL_FIELD(Double,  jdouble)
#undef DECL_FIELD

jsize         JNICALL f_GetStringLength(JNIEnv *, jstring);
const jchar  *JNICALL f_GetStringChars(JNIEnv *, jstring, jboolean *);
void          JNICALL f_ReleaseStringChars(JNIEnv *, jstring, const jchar *);
jstring       JNICALL f_NewString(JNIEnv *, const jchar *, jsize);
void          JNICALL f_GetStringRegion(JNIEnv *, jstring, jsize, jsize, jchar *);
const jchar  *JNICALL f_GetStringCritical(JNIEnv *, jstring, jboolean *);
void          JNICALL f_ReleaseStringCritical(JNIEnv *, jstring, const jchar *);

#endif
