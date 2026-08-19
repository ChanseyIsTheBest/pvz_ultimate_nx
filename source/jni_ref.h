/* jni_ref.h -- object model + reference table.
 *
 * Why this is its own file, and why it is worth getting right: .NET for
 * Android's Java.Interop tracks managed<->Java object identity through JNI
 * reference types. Every managed peer holds a global or weak-global ref, and
 * the GC decides object lifetime partly from what GetObjectRefType() reports.
 * A reference table that guesses will not fail as a JNI error -- it will fail
 * later as a collection that frees a live object. Debugging that from a heap
 * corruption crash is far worse than writing this properly now.
 *
 * References are tagged handles, not raw pointers: (index << 2) | kind. That
 * makes GetObjectRefType O(1), makes IsSameObject exact, and means a stale
 * handle is detectable rather than a wild dereference.
 */
#ifndef PVZU_JNI_REF_H
#define PVZU_JNI_REF_H

#include <jni.h>
#include <stdint.h>

typedef struct FakeClass FakeClass;

/* Every fake Java object starts with this header, so a FakeClass*, a fake
 * MotionEvent* and a fake String* are all valid jobjects. */
typedef struct FakeObject {
  FakeClass *cls;
  /* Reference counting, for objects the shim heap-allocates.
   *
   * Almost every fake object here is a static singleton -- the Activity, the
   * SurfaceHolder, the Context -- so releasing a reference to one must not free
   * anything. Arrays are the exception: jni_arrays mallocs a FakeArray and its
   * data for every NewByteArray, and nothing ever freed either. A game reading
   * assets in 1 MB chunks leaked a megabyte per chunk until the 256 MB newlib
   * heap was gone, at which point calloc failed and the array came back with a
   * length and no buffer.
   *
   * heap_owned marks the objects that jniref may free; refs counts the live
   * handles. Static objects leave both zero and are never touched. */
  int        heap_owned;
  int        refs;
} FakeObject;

/* Called by jniref when the last reference to a heap-owned object goes away.
 * Defined in jni_arrays.c, which owns the only such objects. */
void jniref_free_owned(FakeObject *o);

typedef struct {
  jvalue (*fn)(JNIEnv *env, jobject self, const jvalue *args);
  const char *name;
  const char *sig;
  void *native_fn;      /* set by RegisterNatives, if this method is native  */
  int   is_static;
} FakeMethod;

/* Fields carry accessors rather than an offset. Our "objects" are C structs
 * with no uniform layout, so an offset would only work for one of them. */
typedef struct FakeField {
  const char *name;
  const char *sig;
  jvalue (*get)(JNIEnv *env, jobject self);
  void   (*set)(JNIEnv *env, jobject self, jvalue v);
  jvalue  value;        /* storage for static fields with no accessor      */
} FakeField;

struct FakeClass {
  FakeObject  hdr;          /* hdr.cls points at the java/lang/Class singleton */
  const char *name;         /* "crc646ec5dadb3c3b2bda/AndroidNativeActivity_LawnSurfaceView" */
  FakeClass  *super;
  FakeMethod *methods;  int nmethods;
  FakeField  *fields;   int nfields;

  /* Bytes to allocate for an instance of this class. Zero means a bare
   * FakeObject, which is right for the many classes that carry no state.
   *
   * Classes whose methods store something -- a writer's buffer, a throwable's
   * message -- MUST set this. NewObject allocated a fixed FakeObject for every
   * class, so the first field such a method wrote landed past the end of its
   * own allocation and quietly corrupted the heap. Appended last so every
   * existing positional initialiser stays valid and simply leaves it zero. */
  size_t      instance_size;
};

/* Reference kinds -- values match JNI's jobjectRefType. */
enum { REF_INVALID = 0, REF_LOCAL = 1, REF_GLOBAL = 2, REF_WEAK = 3 };

void     jniref_init(void);

jobject  jniref_new(FakeObject *obj, int kind);
FakeObject *jniref_deref(jobject ref);      /* NULL if stale or invalid */
int      jniref_kind(jobject ref);
void     jniref_delete(jobject ref);

/* Mark a reference as owned by the host and refuse to release it.
 *
 * The Activity and SurfaceView instances are ours -- the frame loop calls into
 * them every frame. A managed peer that wraps one will, when finalized, try to
 * dispose the reference it was handed, which is that same global ref. Letting
 * that succeed would leave the host driving a freed handle. */
void     jniref_pin(jobject ref);
int      jniref_is_pinned(jobject ref);

/* Local frames. Java.Interop pushes frames around peer construction; leaking
 * locals across frames is a real source of table exhaustion. */
void     jniref_push_frame(int capacity);
jobject  jniref_pop_frame(jobject result);  /* re-references result in outer frame */

void     jniref_dump_stats(void);           /* call on shutdown / when hunting leaks */

#endif
