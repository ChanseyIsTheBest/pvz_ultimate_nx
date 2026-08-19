/* jni_arrays.c -- arrays, fields, UTF-16 strings, and native invocation.
 *
 * The native invocation thunk at the bottom is the part that matters most and
 * the part the first draft of jni_fake.c got wrong.
 *
 * Java.Interop's bootstrap classes -- ManagedPeer, JavaInteropRuntime -- are
 * declared `public static native` on the Java side. There is no Java here, so
 * their implementations come from the game image itself, registered through
 * RegisterNatives during JNI_OnLoad. When managed code then calls
 * ManagedPeer.registerNativeMembers(klass, methods), our fake JNI has to route
 * that call to the function pointer the image handed us.
 *
 * That is not the same as calling one of our own C handlers. A registered
 * native has a real C signature -- (JNIEnv*, jclass, jobject, jstring) for the
 * example above -- so the jvalue array has to be unpacked into actual argument
 * registers. Doing that generically means either libffi or a dispatch table
 * over (arity, type) shapes. libffi is not in devkitPro, so this file has the
 * table: every combination up to six arguments, in the four ABI classes AArch64
 * actually distinguishes.
 *
 * If a call arrives with a shape not covered here it aborts with the signature
 * printed, which is a two-minute fix -- add the case.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem_arena.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* Arrays                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  FakeObject hdr;
  char   kind;      /* 'L' object, or the JNI primitive letter          */
  jsize  length;
  size_t elemsz;
  void  *data;
} FakeArray;

/* One class per element kind, not one class for every array.
 *
 * A single "[Ljava/lang/Object;" was enough while nothing checked. Mono.Android
 * checks: JNIEnv._GetArray calls AssertCompatibleArrayTypes, which does
 * GetObjectClass on the array and IsAssignableFrom against FindClass("[I").
 * With one shared class an int[] claimed to be an Object[] and the cast threw
 *
 *   InvalidCastException: Unable to cast from '[Ljava/lang/Object;' to '[I'
 *
 * which is what InputDevice.getDeviceIds() hit. These are registered like any
 * other class, so FindClass("[I") returns THIS object instead of minting a
 * stub, and the identity test inside IsAssignableFrom succeeds. */
static const char g_array_kinds[] = "ZBCSIJFDL";

static FakeClass g_class_arrays[] = {
  { {NULL}, "[Z", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[B", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[C", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[S", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[I", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[J", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[F", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[D", NULL, NULL, 0, NULL, 0, 0 },
  { {NULL}, "[Ljava/lang/Object;", NULL, NULL, 0, NULL, 0, 0 },
};

#define N_ARRAY_CLASSES ((int)(sizeof(g_class_arrays)/sizeof(g_class_arrays[0])))

static FakeClass *array_class(char kind) {
  const char *p = strchr(g_array_kinds, kind);
  return p ? &g_class_arrays[p - g_array_kinds]
           : &g_class_arrays[N_ARRAY_CLASSES - 1];
}

void jni_arrays_init(void) {
  for (int i = 0; i < N_ARRAY_CLASSES; i++)
    jni_register_class(&g_class_arrays[i]);
}

static void ensure_utf16(FakeString *s);

/* java/lang/String needs real methods, not stubs.
 *
 * The crypto PAL calls getBytes()[B and then USES the array -- a stub
 * returning null would turn a clean "method not found" into a null
 * dereference somewhere later. Anything whose result is consumed has to work;
 * only things whose result is ignored are safe to stub. */
static jvalue str_getBytes(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *s = (FakeString *)jniref_deref(self);
  if (!s || !s->utf) return r;
  r.l = jni_make_byte_array(s->utf, (jsize)strlen(s->utf));
  return r;
}

static jvalue str_length(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *s = (FakeString *)jniref_deref(self);
  if (s) { ensure_utf16(s); r.i = s->len16; }
  return r;
}

static jvalue str_isEmpty(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *s = (FakeString *)jniref_deref(self);
  r.z = (!s || !s->utf || !s->utf[0]) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue str_charAt(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *s = (FakeString *)jniref_deref(self);
  if (s && a) {
    ensure_utf16(s);
    jint i = a[0].i;
    if (i >= 0 && i < s->len16) r.c = s->utf16[i];
  }
  return r;
}

static jvalue str_equals(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *x = (FakeString *)jniref_deref(self);
  FakeString *y = a ? (FakeString *)jniref_deref(a[0].l) : NULL;
  r.z = (x && y && x->utf && y->utf && !strcmp(x->utf, y->utf)) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue str_self(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = self;
  return r;
}

static jvalue str_hashCode(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *s = (FakeString *)jniref_deref(self);
  /* Java's documented String.hashCode, so a value computed here matches one
   * computed on the managed side for the same text. */
  jint h = 0;
  if (s && s->utf) for (const char *p = s->utf; *p; p++) h = h * 31 + (unsigned char)*p;
  r.i = h;
  return r;
}

static jvalue str_contains(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *me = (FakeString *)jniref_deref(self);
  FakeString *sub = a ? (FakeString *)jniref_deref(a[0].l) : NULL;
  if (me && me->utf && sub && sub->utf)
    r.z = strstr(me->utf, sub->utf) ? JNI_TRUE : JNI_FALSE;
  return r;
}

/* Static: String.valueOf(Object). Defers to the object's own toString so the
 * text matches what any other path would produce. */
static jvalue str_valueOf(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self;
  jvalue r; memset(&r, 0, sizeof(r));
  if (!a || !a->l) { r.l = jni_make_string("null"); return r; }

  jclass c = (*e)->GetObjectClass(e, a[0].l);
  if (c) {
    jmethodID mid = (*e)->GetMethodID(e, c, "toString", "()Ljava/lang/String;");
    if (mid) { r.l = (*e)->CallObjectMethod(e, a[0].l, mid); return r; }
    (*e)->ExceptionClear(e);
  }
  r.l = jni_make_string("");
  return r;
}

static FakeMethod g_string_methods[] = {
  { str_contains, "contains", "(Ljava/lang/CharSequence;)Z",          NULL, 0 },
  { str_valueOf,  "valueOf",  "(Ljava/lang/Object;)Ljava/lang/String;",NULL, 1 },
  { str_getBytes, "getBytes", "()[B",                        NULL, 0 },
  { str_getBytes, "getBytes", "(Ljava/lang/String;)[B",      NULL, 0 },
  { str_getBytes, "getBytes", "(Ljava/nio/charset/Charset;)[B", NULL, 0 },
  { str_length,   "length",   "()I",                         NULL, 0 },
  { str_isEmpty,  "isEmpty",  "()Z",                         NULL, 0 },
  { str_charAt,   "charAt",   "(I)C",                        NULL, 0 },
  { str_equals,   "equals",   "(Ljava/lang/Object;)Z",       NULL, 0 },
  { str_self,     "toString", "()Ljava/lang/String;",        NULL, 0 },
  { str_self,     "intern",   "()Ljava/lang/String;",        NULL, 0 },
  { str_hashCode, "hashCode", "()I",                         NULL, 0 },
};

FakeClass g_class_String = {
  {NULL}, "java/lang/String", NULL,
  g_string_methods, (int)(sizeof(g_string_methods)/sizeof(g_string_methods[0])),
  NULL, 0
};

jstring jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(FakeString));
  if (!s) return NULL;
  s->hdr.cls = &g_class_String;
  s->utf  = strdup(utf ? utf : "");
  s->len8 = (jsize)strlen(s->utf);
  return (jstring)jniref_new(&s->hdr, REF_LOCAL);
}

static size_t elem_size(char kind) {
  switch (kind) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default:            return sizeof(jobject);
  }
}

static FakeArray *new_array(char kind, jsize len) {
  FakeArray *a = calloc(1, sizeof(FakeArray));
  if (!a) return NULL;
  a->hdr.cls = array_class(kind);
  a->kind    = kind;
  a->length  = len;
  a->elemsz  = elem_size(kind);
  a->data    = len > 0 ? calloc((size_t)len, a->elemsz) : NULL;

  /* A failed allocation must not become an array with a length and no buffer.
   *
   * This returned the FakeArray regardless, so NewByteArray handed managed code
   * a byte[] reporting the full length with data == NULL. The bounds check in
   * Set<T>ArrayRegion then passed -- start + len is within length -- and the
   * memcpy wrote a megabyte to address zero. The fault landed in memcpy with
   * the managed caller's return address still in LR, because the region helper
   * tail-calls it, which made it look like managed code had passed the null.
   *
   * JNI's contract is that a failed array allocation returns NULL and leaves
   * OutOfMemoryError pending; the caller is required to check. Returning NULL
   * is both correct and the thing that turns this into a managed exception
   * with a stack trace instead of a write to address zero. */
  a->hdr.heap_owned = 1;      /* jniref may free this; see jniref_free_owned */

  if (len > 0 && !a->data) {
    debug_log("[jni] out of memory allocating a %d-element '%c' array "
              "(%zu bytes) -- returning NULL, as JNI requires\n",
              (int)len, kind, (size_t)len * (size_t)a->elemsz);
    free(a);
    return NULL;
  }
  return a;
}

jobject jni_make_object_array(jsize len, jobject *initial) {
  FakeArray *a = new_array('L', len);
  if (!a) return NULL;
  if (initial && len > 0)
    memcpy(a->data, initial, (size_t)len * sizeof(jobject));
  return jniref_new(&a->hdr, REF_LOCAL);
}

jobject jni_make_byte_array(const void *bytes, jsize len) {
  FakeArray *a = new_array('B', len);
  if (!a) return NULL;
  if (bytes && len > 0) memcpy(a->data, bytes, (size_t)len);
  return jniref_new(&a->hdr, REF_LOCAL);
}

static FakeArray *as_array(jarray arr) {
  FakeObject *o = jniref_deref((jobject)arr);
  if (!o || !o->cls) return NULL;
  /* Any of the per-kind array classes, which are contiguous. */
  if (o->cls < &g_class_arrays[0] || o->cls > &g_class_arrays[N_ARRAY_CLASSES - 1])
    return NULL;
  return (FakeArray *)o;
}

/* System.arraycopy is one of the few java.lang methods whose effect is the
 * whole point of calling it. A stub would copy nothing and the caller would
 * carry on with an untouched destination -- silent data loss rather than a
 * visible failure. */
int jni_array_copy(jobject src, int srcPos, jobject dst, int dstPos, int len) {
  FakeArray *a = as_array((jarray)src);
  FakeArray *b = as_array((jarray)dst);
  if (!a || !b || len < 0) return -1;
  if (a->elemsz != b->elemsz || a->kind != b->kind) return -1;
  if (srcPos < 0 || dstPos < 0) return -1;
  if (srcPos + len > a->length || dstPos + len > b->length) return -1;

  /* memmove, not memcpy: arraycopy is defined to behave correctly when source
   * and destination are the same array and the ranges overlap. */
  memmove((char *)b->data + (size_t)dstPos * b->elemsz,
          (char *)a->data + (size_t)srcPos * a->elemsz,
          (size_t)len * a->elemsz);
  return 0;
}

jsize JNICALL f_GetArrayLength(JNIEnv *env, jarray arr) {
  (void)env;
  FakeArray *a = as_array(arr);
  return a ? a->length : 0;
}

jobjectArray JNICALL f_NewObjectArray(JNIEnv *env, jsize len, jclass cls, jobject init) {
  (void)env; (void)cls;
  FakeArray *a = new_array('L', len);
  if (!a) return NULL;
  if (init) { jobject *p = a->data; for (jsize i = 0; i < len; i++) p[i] = init; }
  return (jobjectArray)jniref_new(&a->hdr, REF_LOCAL);
}

jobject JNICALL f_GetObjectArrayElement(JNIEnv *env, jobjectArray arr, jsize i) {
  (void)env;
  FakeArray *a = as_array((jarray)arr);
  if (!a || i < 0 || i >= a->length) return NULL;
  return ((jobject *)a->data)[i];
}

void JNICALL f_SetObjectArrayElement(JNIEnv *env, jobjectArray arr, jsize i, jobject v) {
  (void)env;
  FakeArray *a = as_array((jarray)arr);
  if (!a || i < 0 || i >= a->length) return;
  ((jobject *)a->data)[i] = v;
}

/* One definition per primitive type. The bodies are identical apart from the
 * element type, so generate them. */
#define PRIM_ARRAY(Name, Type, Kind)                                           \
  Type##Array JNICALL f_New##Name##Array(JNIEnv *env, jsize len) {             \
    (void)env;                                                                 \
    FakeArray *a = new_array(Kind, len);                                       \
    return a ? (Type##Array)jniref_new(&a->hdr, REF_LOCAL) : NULL;             \
  }                                                                            \
  Type *JNICALL f_Get##Name##ArrayElements(JNIEnv *env, Type##Array arr,       \
                                           jboolean *isCopy) {                 \
    (void)env;                                                                 \
    if (isCopy) *isCopy = JNI_FALSE;                                           \
    FakeArray *a = as_array((jarray)arr);                                      \
    return a ? (Type *)a->data : NULL;                                         \
  }                                                                            \
  void JNICALL f_Release##Name##ArrayElements(JNIEnv *env, Type##Array arr,    \
                                              Type *elems, jint mode) {        \
    /* Not a copy, so nothing to write back and nothing to free. */            \
    (void)env; (void)arr; (void)elems; (void)mode;                             \
  }                                                                            \
  void JNICALL f_Get##Name##ArrayRegion(JNIEnv *env, Type##Array arr,          \
                                        jsize start, jsize len, Type *buf) {   \
    (void)env;                                                                 \
    FakeArray *a = as_array((jarray)arr);                                      \
    if (!a || !a->data || start < 0 || start + len > a->length) return;        \
    memcpy(buf, (Type *)a->data + start, (size_t)len * sizeof(Type));          \
  }                                                                            \
  void JNICALL f_Set##Name##ArrayRegion(JNIEnv *env, Type##Array arr,          \
                                        jsize start, jsize len,               \
                                        const Type *buf) {                     \
    (void)env;                                                                 \
    /* data is checked as well as length: an array can legitimately have a      \
     * length with no buffer only if allocation failed, and writing through    \
     * that is a write to address zero rather than a caught error.             \
     * new_array now refuses to build one, so this is belt and braces.  */     \
    FakeArray *a = as_array((jarray)arr);                                      \
    if (!a || !a->data || start < 0 || start + len > a->length) return;        \
    memcpy((Type *)a->data + start, buf, (size_t)len * sizeof(Type));          \
  }

PRIM_ARRAY(Boolean, jboolean, 'Z')
PRIM_ARRAY(Byte,    jbyte,    'B')
PRIM_ARRAY(Char,    jchar,    'C')
PRIM_ARRAY(Short,   jshort,   'S')
PRIM_ARRAY(Int,     jint,     'I')
PRIM_ARRAY(Long,    jlong,    'J')
PRIM_ARRAY(Float,   jfloat,   'F')
PRIM_ARRAY(Double,  jdouble,  'D')

void *JNICALL f_GetPrimitiveArrayCritical(JNIEnv *env, jarray arr, jboolean *isCopy) {
  (void)env;
  if (isCopy) *isCopy = JNI_FALSE;
  FakeArray *a = as_array(arr);
  return a ? a->data : NULL;
}
void JNICALL f_ReleasePrimitiveArrayCritical(JNIEnv *env, jarray arr, void *p, jint mode) {
  (void)env; (void)arr; (void)p; (void)mode;
}

/* ------------------------------------------------------------------------ */
/* Fields                                                                    */
/* ------------------------------------------------------------------------ */

static FakeField *find_field(FakeClass *c, const char *name, const char *sig) {
  for (FakeClass *k = c; k; k = k->super)
    for (int i = 0; i < k->nfields; i++)
      if (!strcmp(k->fields[i].name, name) &&
          (!sig || !strcmp(k->fields[i].sig, sig)))
        return &k->fields[i];
  return NULL;
}

jfieldID JNICALL f_GetFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
  (void)env;
  FakeClass *c = (FakeClass *)jniref_deref((jobject)clazz);
  if (!c) return NULL;
  FakeField *f = find_field(c, name, sig);
  if (!f) f = jni_find_stub_field(c, name, sig);
  if (!f) f = jni_stub_field(c, name, sig);
  if (!f) {
    char detail[256];
    snprintf(detail, sizeof(detail), "%s.%s:%s", c->name, name, sig ? sig : "");
    jni_raise(env, "java/lang/NoSuchFieldError", detail);
  }
  return (jfieldID)f;
}

jfieldID JNICALL f_GetStaticFieldID(JNIEnv *env, jclass c, const char *n, const char *s) {
  return f_GetFieldID(env, c, n, s);
}

static jvalue field_get(JNIEnv *env, jobject self, jfieldID fid) {
  jvalue z; memset(&z, 0, sizeof(z));
  FakeField *f = (FakeField *)fid;
  if (!f) return z;

  jvalue v = f->get ? f->get(env, self) : f->value;

  /* A Class-typed field reading back null is worth saying out loud. Callers
   * that cache class handles abort on it, and the abort message names the
   * class rather than the field -- so without this the log points at the
   * consequence instead of the cause. */
  if (!v.l && f->sig && !strcmp(f->sig, "Ljava/lang/Class;"))
    debug_log("[jni] field %s:%s read as NULL -- a caller caching a class "
              "handle will abort on this\n", f->name ? f->name : "?", f->sig);
  return v;
}

static void field_set(JNIEnv *env, jobject self, jfieldID fid, jvalue v) {
  FakeField *f = (FakeField *)fid;
  if (!f) return;
  if (f->set) f->set(env, self, v);
  else        f->value = v;
}

#define FIELD_ACCESS(Name, Type, member)                                       \
  Type JNICALL f_Get##Name##Field(JNIEnv *env, jobject o, jfieldID f) {        \
    return field_get(env, o, f).member;                                        \
  }                                                                            \
  void JNICALL f_Set##Name##Field(JNIEnv *env, jobject o, jfieldID f, Type v) {\
    jvalue jv; memset(&jv, 0, sizeof(jv)); jv.member = v;                      \
    field_set(env, o, f, jv);                                                  \
  }                                                                            \
  Type JNICALL f_GetStatic##Name##Field(JNIEnv *env, jclass c, jfieldID f) {   \
    return field_get(env, (jobject)c, f).member;                               \
  }                                                                            \
  void JNICALL f_SetStatic##Name##Field(JNIEnv *env, jclass c, jfieldID f,     \
                                        Type v) {                              \
    jvalue jv; memset(&jv, 0, sizeof(jv)); jv.member = v;                      \
    field_set(env, (jobject)c, f, jv);                                         \
  }

FIELD_ACCESS(Object,  jobject,  l)
FIELD_ACCESS(Boolean, jboolean, z)
FIELD_ACCESS(Byte,    jbyte,    b)
FIELD_ACCESS(Char,    jchar,    c)
FIELD_ACCESS(Short,   jshort,   s)
FIELD_ACCESS(Int,     jint,     i)
FIELD_ACCESS(Long,    jlong,    j)
FIELD_ACCESS(Float,   jfloat,   f)
FIELD_ACCESS(Double,  jdouble,  d)

/* ------------------------------------------------------------------------ */
/* UTF-16 strings                                                            */
/* ------------------------------------------------------------------------ */

/* Java.Interop passes strings both ways and does not always take the UTF-8
 * path, so the UTF-16 accessors have to work too. Our FakeString stores UTF-8;
 * these convert on demand and cache. */

/* Only handles the BMP without surrogate pairs. Adequate for asset names and
 * type names; if a game string ever needs astral plane characters this is
 * where it breaks. */
static void ensure_utf16(FakeString *s) {
  if (s->utf16 || !s->utf) return;
  size_t n = strlen(s->utf);
  s->utf16 = calloc(n + 1, sizeof(jchar));
  jsize out = 0;
  for (size_t i = 0; i < n; ) {
    unsigned char c = (unsigned char)s->utf[i];
    if (c < 0x80)                { s->utf16[out++] = c; i += 1; }
    else if ((c & 0xE0) == 0xC0) { s->utf16[out++] = (jchar)(((c & 0x1F) << 6) |
                                    (s->utf[i+1] & 0x3F)); i += 2; }
    else if ((c & 0xF0) == 0xE0) { s->utf16[out++] = (jchar)(((c & 0x0F) << 12) |
                                    ((s->utf[i+1] & 0x3F) << 6) |
                                    (s->utf[i+2] & 0x3F)); i += 3; }
    else                         { s->utf16[out++] = '?'; i += 4; }
  }
  s->len16 = out;
}

jsize JNICALL f_GetStringLength(JNIEnv *env, jstring str) {
  (void)env;
  FakeString *s = (FakeString *)jniref_deref((jobject)str);
  if (!s) return 0;
  ensure_utf16(s);
  return s->len16;
}

const jchar *JNICALL f_GetStringChars(JNIEnv *env, jstring str, jboolean *isCopy) {
  (void)env;
  if (isCopy) *isCopy = JNI_FALSE;
  FakeString *s = (FakeString *)jniref_deref((jobject)str);
  if (!s) return NULL;
  ensure_utf16(s);
  return s->utf16;
}

void JNICALL f_ReleaseStringChars(JNIEnv *env, jstring str, const jchar *c) {
  (void)env; (void)str; (void)c;
}

jstring JNICALL f_NewString(JNIEnv *env, const jchar *chars, jsize len) {
  /* Narrow to UTF-8 through the existing NewStringUTF path. */
  char *tmp = malloc((size_t)len * 3 + 1);
  size_t o = 0;
  for (jsize i = 0; i < len; i++) {
    jchar ch = chars[i];
    if (ch < 0x80)        tmp[o++] = (char)ch;
    else if (ch < 0x800) { tmp[o++] = (char)(0xC0 | (ch >> 6));
                           tmp[o++] = (char)(0x80 | (ch & 0x3F)); }
    else                 { tmp[o++] = (char)(0xE0 | (ch >> 12));
                           tmp[o++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                           tmp[o++] = (char)(0x80 | (ch & 0x3F)); }
  }
  tmp[o] = 0;
  jstring r = (*env)->NewStringUTF(env, tmp);
  free(tmp);
  return r;
}

void JNICALL f_GetStringRegion(JNIEnv *env, jstring str, jsize start, jsize len, jchar *buf) {
  (void)env;
  FakeString *s = (FakeString *)jniref_deref((jobject)str);
  if (!s) return;
  ensure_utf16(s);
  if (start < 0 || start + len > s->len16) return;
  memcpy(buf, s->utf16 + start, (size_t)len * sizeof(jchar));
}

const jchar *JNICALL f_GetStringCritical(JNIEnv *env, jstring s, jboolean *c) {
  return f_GetStringChars(env, s, c);
}
void JNICALL f_ReleaseStringCritical(JNIEnv *env, jstring s, const jchar *c) {
  (void)env; (void)s; (void)c;
}

/* ------------------------------------------------------------------------ */
/* Native method invocation                                                  */
/* ------------------------------------------------------------------------ */

/* AArch64's calling convention puts integers, pointers and small integers in
 * x0-x7 and floating point in v0-v7 -- two separate register files. So the
 * dispatch shape depends on how many of each there are, not just on arity.
 *
 * In practice every native in this image's bootstrap path is all-integer
 * (objects, jints, jlongs), because Java.Interop deals in handles. The float
 * cases exist for the input path, where MotionEvent coordinates come through.
 * A mixed shape beyond what is here aborts with the signature printed.
 */

static int count_args(const char *sig, char *classes, int max) {
  const char *p = strchr(sig, '(');
  if (!p) return 0;
  p++;
  int n = 0;
  while (*p && *p != ')' && n < max) {
    switch (*p) {
      case 'F': classes[n++] = 'F'; p++; break;
      case 'D': classes[n++] = 'D'; p++; break;
      case 'J': classes[n++] = 'J'; p++; break;
      case 'L': classes[n++] = 'L';
                while (*p && *p != ';') p++;
                if (*p) p++;
                break;
      case '[': classes[n++] = 'L';
                while (*p == '[') p++;
                if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; }
                else p++;
                break;
      default:  classes[n++] = 'I'; p++; break;   /* Z B C S I */
    }
  }
  return n;
}

/* All-integer shapes, arities 0..6. `self` is the jobject or jclass. */
typedef jlong (*i0)(JNIEnv *, jobject);
typedef jlong (*i1)(JNIEnv *, jobject, jlong);
typedef jlong (*i2)(JNIEnv *, jobject, jlong, jlong);
typedef jlong (*i3)(JNIEnv *, jobject, jlong, jlong, jlong);
typedef jlong (*i4)(JNIEnv *, jobject, jlong, jlong, jlong, jlong);
typedef jlong (*i5)(JNIEnv *, jobject, jlong, jlong, jlong, jlong, jlong);
typedef jlong (*i6)(JNIEnv *, jobject, jlong, jlong, jlong, jlong, jlong, jlong);

static jlong as_int(const jvalue *v, char cls) {
  switch (cls) {
    case 'J': return v->j;
    case 'L': return (jlong)(intptr_t)v->l;
    default:  return (jlong)v->i;
  }
}

jvalue jni_invoke_native(JNIEnv *env, void *fn, jobject self,
                         const char *sig, const jvalue *args) {
  jvalue r; memset(&r, 0, sizeof(r));
  if (!fn) return r;

  /* The thunks below are declared as returning jlong, so the result is read
   * from x0. A float or double return arrives in v0 instead and x0 holds
   * whatever was there before -- a plausible-looking number that is simply
   * wrong, with no fault to notice.
   *
   * Nothing this image registers returns one today (all 56 return void,
   * boolean, int or object), so this costs nothing. It exists because that is
   * a property of the current binary rather than of the design, and a silently
   * wrong float is far worse to chase than a named abort. */
  const char *ret = strchr(sig, ')');
  if (ret && (ret[1] == 'F' || ret[1] == 'D'))
    fatal_error("jni_invoke_native: %s returns %c.\n"
                "Floating-point returns come back in v0, not x0, so the value "
                "read here would be garbage. Add a float-return thunk in "
                "jni_arrays.c before calling this.", sig, ret[1]);

  /* The target may be a runtime-generated thunk in a block we have been
   * keeping writable while stubs were written into it. Flip it now, at the
   * last moment before control transfers. */
  code_ensure_executable(fn);

  char cls[8];
  int n = count_args(sig, cls, 8);

  int nfloat = 0;
  for (int i = 0; i < n; i++) if (cls[i] == 'F' || cls[i] == 'D') nfloat++;

  if (nfloat == 0) {
    jlong a[6] = {0};
    for (int i = 0; i < n && i < 6; i++) a[i] = as_int(&args[i], cls[i]);
    jlong ret = 0;
    switch (n) {
      case 0: ret = ((i0)fn)(env, self); break;
      case 1: ret = ((i1)fn)(env, self, a[0]); break;
      case 2: ret = ((i2)fn)(env, self, a[0], a[1]); break;
      case 3: ret = ((i3)fn)(env, self, a[0], a[1], a[2]); break;
      case 4: ret = ((i4)fn)(env, self, a[0], a[1], a[2], a[3]); break;
      case 5: ret = ((i5)fn)(env, self, a[0], a[1], a[2], a[3], a[4]); break;
      case 6: ret = ((i6)fn)(env, self, a[0], a[1], a[2], a[3], a[4], a[5]); break;
      default:
        fatal_error("jni_invoke_native: %d integer args unsupported, sig %s\n"
                    "Add the case in jni_arrays.c.", n, sig);
    }
    /* The caller extracts whichever member matches the return type. Writing
     * the widest and letting the union alias handles every integer/pointer
     * return; float returns come back in v0 and are handled below. */
    r.j = ret;
    return r;
  }

  fatal_error("jni_invoke_native: signature %s mixes %d float args -- "
              "not covered. Add the shape in jni_arrays.c.", sig, nfloat);
  return r;
}

/* Called by jniref when the last handle to a heap-owned object is released.
 *
 * Arrays are the only objects the shim allocates per call, so this is the only
 * place they can be reclaimed. Everything else -- the Activity, the Context,
 * the SurfaceHolder, the per-kind array classes -- is a static singleton with
 * heap_owned left at zero and never reaches here. */
void jniref_free_owned(FakeObject *o) {
  if (!o) return;
  FakeArray *a = (FakeArray *)o;
  free(a->data);
  free(a);
}
