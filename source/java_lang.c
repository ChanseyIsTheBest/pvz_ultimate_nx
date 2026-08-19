/* java_lang.c -- the core java.lang surface the runtime cannot start without.
 *
 * These are not Android classes, they are the language's own, and the runtime
 * assumes they exist the way it assumes a heap exists. The GC bridge asks for
 * java/lang/Runtime during JNI_OnLoad and aborts outright if it is missing --
 * which is exactly where the last run stopped.
 *
 * Everything here is deliberately inert. There is no Java heap, no Java GC and
 * no Java threads, so the honest implementation of Runtime.gc() is to do
 * nothing. What matters is that the lookups succeed and return objects the
 * runtime can hold, because a failed lookup is fatal while a no-op is not.
 *
 * Classes are added here as the log names them. That loop is cheap: a missing
 * class now produces a ClassNotFoundException naming it, rather than a crash.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "java_lang.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "util.h"

#define JV_ZERO jvalue r; memset(&r, 0, sizeof(r))

/* ------------------------------------------------------------------------ */
/* java/lang/Runtime                                                         */
/* ------------------------------------------------------------------------ */

/* The GC bridge wants the singleton and a way to ask for a collection. We have
 * no Java heap to collect, so gc() is a no-op -- but it must be callable, and
 * getRuntime() must return something non-null or the bridge treats it as a
 * failed initialisation. */
static FakeObject g_runtime_obj;
static FakeClass  g_class_Runtime;
static jobject    g_ref_runtime;

static jvalue rt_getRuntime(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.l = g_ref_runtime;
  return r;
}

static jvalue rt_gc(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  static int logged;
  if (!logged) {
    logged = 1;
    debug_log("[java] Runtime.gc() -- nothing to collect on this side; "
              "the managed GC is unaffected\n");
  }
  return r;
}

/* Memory figures come from the same understated numbers sysconf reports, so
 * the runtime gets one consistent picture of the machine rather than two. */
static jvalue rt_totalMemory(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.j = 256ll * 1024 * 1024; return r;
}
static jvalue rt_freeMemory(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.j = 128ll * 1024 * 1024; return r;
}
static jvalue rt_maxMemory(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.j = 512ll * 1024 * 1024; return r;
}
static jvalue rt_availableProcessors(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.i = 3; return r;
}

static FakeMethod g_runtime_methods[] = {
  { rt_getRuntime,          "getRuntime",          "()Ljava/lang/Runtime;", NULL, 1 },
  { rt_gc,                  "gc",                  "()V",                   NULL, 0 },
  { rt_gc,                  "runFinalization",     "()V",                   NULL, 0 },
  { rt_totalMemory,         "totalMemory",         "()J",                   NULL, 0 },
  { rt_freeMemory,          "freeMemory",          "()J",                   NULL, 0 },
  { rt_maxMemory,           "maxMemory",           "()J",                   NULL, 0 },
  { rt_availableProcessors, "availableProcessors", "()I",                   NULL, 0 },
};

static FakeClass g_class_Runtime = {
  {NULL}, "java/lang/Runtime", NULL,
  g_runtime_methods, (int)(sizeof(g_runtime_methods)/sizeof(g_runtime_methods[0])),
  NULL, 0
};

/* ------------------------------------------------------------------------ */
/* java/lang/Object                                                          */
/* ------------------------------------------------------------------------ */

static FakeClass g_class_Object;

/* Identity, not content. Our references are tagged handles, so the handle
 * value itself is a stable identity hash for the lifetime of the reference --
 * which is all identityHashCode promises. */
static jvalue obj_hashCode(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  r.i = (jint)(intptr_t)jniref_deref(self);
  return r;
}

static jvalue obj_equals(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  r.z = (a && jniref_deref(self) == jniref_deref(a[0].l)) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue obj_getClass(JNIEnv *e, jobject self, const jvalue *a) {
  (void)a;
  JV_ZERO;
  r.l = (*e)->GetObjectClass(e, self);
  return r;
}

static jvalue obj_toString(JNIEnv *e, jobject self, const jvalue *a) {
  (void)a;
  JV_ZERO;
  FakeObject *o = jniref_deref(self);
  char buf[128];
  snprintf(buf, sizeof(buf), "%s@%p",
           (o && o->cls) ? o->cls->name : "java.lang.Object", (void *)o);
  r.l = jni_make_string(buf);
  return r;
}

static jvalue obj_nop(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; return r;
}

static FakeMethod g_object_methods[] = {
  { obj_nop, "<init>", "()V", NULL, 0 },
  { obj_hashCode, "hashCode", "()I",                        NULL, 0 },
  { obj_equals,   "equals",   "(Ljava/lang/Object;)Z",      NULL, 0 },
  { obj_getClass, "getClass", "()Ljava/lang/Class;",        NULL, 0 },
  { obj_toString, "toString", "()Ljava/lang/String;",       NULL, 0 },
  { obj_nop,      "notify",   "()V",                        NULL, 0 },
  { obj_nop,      "notifyAll","()V",                        NULL, 0 },
  { obj_nop,      "wait",     "()V",                        NULL, 0 },
};

static FakeClass g_class_Object = {
  {NULL}, "java/lang/Object", NULL,
  g_object_methods, (int)(sizeof(g_object_methods)/sizeof(g_object_methods[0])),
  NULL, 0
};

/* ------------------------------------------------------------------------ */
/* java/lang/System                                                          */
/* ------------------------------------------------------------------------ */

static jvalue sys_identityHashCode(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV_ZERO;
  r.i = a ? (jint)(intptr_t)jniref_deref(a[0].l) : 0;
  return r;
}

static jvalue sys_currentTimeMillis(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.j = (jlong)(armTicksToNs(armGetSystemTick()) / 1000000ull);
  return r;
}

static jvalue sys_nanoTime(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.j = (jlong)armTicksToNs(armGetSystemTick());
  return r;
}

static jvalue sys_getProperty(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self; (void)a;
  JV_ZERO;
  r.l = jni_make_string("");
  (void)e;
  return r;
}

/* Delegates to the real element copy in jni_arrays.c. Out-of-range or
 * mismatched types raise rather than silently doing nothing, because a partial
 * or skipped copy corrupts data the caller then trusts. */
static jvalue sys_arraycopy(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self;
  JV_ZERO;
  if (!a) return r;
  if (jni_array_copy(a[0].l, a[1].i, a[2].l, a[3].i, a[4].i) != 0)
    jni_raise(e, "java/lang/ArrayStoreException", "System.arraycopy");
  return r;
}

/* The runtime loads its own libraries through dlopen; a System.loadLibrary
 * here is the managed side asking for something already resolved. */
static jvalue sys_loadLibrary(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV_ZERO;
  FakeString *n = a ? (FakeString *)jniref_deref(a[0].l) : NULL;
  debug_log("[java] System.loadLibrary(%s) -- already resolved by the loader\n",
            (n && n->utf) ? n->utf : "?");
  return r;
}

static FakeMethod g_system_methods[] = {
  { sys_arraycopy,  "arraycopy",   "(Ljava/lang/Object;ILjava/lang/Object;II)V", NULL, 1 },
  { sys_loadLibrary,"loadLibrary", "(Ljava/lang/String;)V",                      NULL, 1 },
  { obj_nop,                "gc",                "()V",                                   NULL, 1 },
  { obj_nop,                "runFinalization",   "()V",                                   NULL, 1 },
  { sys_identityHashCode,   "identityHashCode",  "(Ljava/lang/Object;)I",                 NULL, 1 },
  { sys_currentTimeMillis,  "currentTimeMillis", "()J",                                   NULL, 1 },
  { sys_nanoTime,           "nanoTime",          "()J",                                   NULL, 1 },
  { sys_getProperty,        "getProperty",       "(Ljava/lang/String;)Ljava/lang/String;",NULL, 1 },
};

static FakeClass g_class_System = {
  {NULL}, "java/lang/System", NULL,
  g_system_methods, (int)(sizeof(g_system_methods)/sizeof(g_system_methods[0])),
  NULL, 0
};

/* ------------------------------------------------------------------------ */
/* java/lang/Thread                                                          */
/* ------------------------------------------------------------------------ */

static FakeObject g_thread_obj;
static FakeClass  g_class_Thread;
static jobject    g_ref_thread;

static jvalue th_currentThread(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.l = g_ref_thread;
  return r;
}
static jvalue th_getName(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.l = jni_make_string("main");
  return r;
}

/* Stored and returned so a get after a set is consistent. Nothing here ever
 * invokes it -- unhandled managed exceptions arrive through
 * UncaughtExceptionMarshaler instead -- but the runtime installs one and may
 * read it back. */
static jobject g_default_ueh;

static jvalue th_setDefaultUEH(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV_ZERO;
  if (a) g_default_ueh = a[0].l;
  return r;
}
static jvalue th_getDefaultUEH(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.l = g_default_ueh;
  return r;
}

static FakeMethod g_thread_methods[] = {
  { th_setDefaultUEH, "setDefaultUncaughtExceptionHandler",
    "(Ljava/lang/Thread$UncaughtExceptionHandler;)V", NULL, 1 },
  { th_getDefaultUEH, "getDefaultUncaughtExceptionHandler",
    "()Ljava/lang/Thread$UncaughtExceptionHandler;",  NULL, 1 },
  { th_currentThread, "currentThread", "()Ljava/lang/Thread;",  NULL, 1 },
  { th_getName,       "getName",       "()Ljava/lang/String;",  NULL, 0 },
  { obj_nop,          "setName",       "(Ljava/lang/String;)V", NULL, 0 },
};

static FakeClass g_class_Thread = {
  {NULL}, "java/lang/Thread", NULL,
  g_thread_methods, (int)(sizeof(g_thread_methods)/sizeof(g_thread_methods[0])),
  NULL, 0
};

/* ------------------------------------------------------------------------ */
/* java/lang/ref/WeakReference                                               */
/* ------------------------------------------------------------------------ */

/* The GC bridge builds weak references to managed peers. With no Java GC these
 * never clear, so get() always returns the referent -- conservative, and it
 * cannot cause a use-after-free. It can leak, which is the right trade here. */
typedef struct { FakeObject hdr; jobject referent; } FakeWeakRef;
static FakeClass g_class_WeakRef;

static jvalue wr_get(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeWeakRef *w = (FakeWeakRef *)jniref_deref(self);
  r.l = w ? w->referent : NULL;
  return r;
}
static jvalue wr_clear(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeWeakRef *w = (FakeWeakRef *)jniref_deref(self);
  if (w) w->referent = NULL;
  return r;
}

static FakeMethod g_weakref_methods[] = {
  { wr_get,   "get",   "()Ljava/lang/Object;", NULL, 0 },
  { wr_clear, "clear", "()V",                  NULL, 0 },
};

static FakeClass g_class_WeakRef = {
  {NULL}, "java/lang/ref/WeakReference", NULL,
  g_weakref_methods, (int)(sizeof(g_weakref_methods)/sizeof(g_weakref_methods[0])),
  NULL, 0
};

/* ------------------------------------------------------------------------ */
/* java/lang/Class and java/lang/ClassLoader                                 */
/* ------------------------------------------------------------------------ */

/* Java hands out dotted names (java.lang.String); our registry is keyed on the
 * JNI slashed form. Convert in both directions rather than storing two copies. */
static void to_dotted(const char *slashed, char *out, size_t n) {
  size_t i = 0;
  for (; slashed[i] && i + 1 < n; i++) out[i] = (slashed[i] == '/') ? '.' : slashed[i];
  out[i] = 0;
}
static void to_slashed(const char *dotted, char *out, size_t n) {
  size_t i = 0;
  for (; dotted[i] && i + 1 < n; i++) out[i] = (dotted[i] == '.') ? '/' : dotted[i];
  out[i] = 0;
}

static jvalue cls_getName(JNIEnv *e, jobject self, const jvalue *a) {
  (void)a;
  JV_ZERO;
  FakeClass *c = (FakeClass *)jniref_deref(self);
  char buf[200];
  to_dotted(c ? c->name : "java.lang.Object", buf, sizeof(buf));
  r.l = jni_make_string(buf);
  return r;
}

static jvalue cls_getSimpleName(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeClass *c = (FakeClass *)jniref_deref(self);
  const char *n = c ? c->name : "Object";
  const char *slash = strrchr(n, '/');
  r.l = jni_make_string(slash ? slash + 1 : n);
  return r;
}

/* Class.forName and ClassLoader.loadClass both resolve a name to a class, and
 * both are given the dotted form. Routing through FindClass reuses the
 * manufacture-on-demand path, so a type Java.Interop asks for that we have not
 * implemented resolves to a stub instead of failing the lookup. */
static jvalue resolve_dotted(JNIEnv *e, const jvalue *a) {
  JV_ZERO;
  if (!a) return r;
  FakeString *s = (FakeString *)jniref_deref(a[0].l);
  if (!s || !s->utf) return r;
  char slashed[200];
  to_slashed(s->utf, slashed, sizeof(slashed));
  r.l = (*e)->FindClass(e, slashed);
  return r;
}

static jvalue cls_forName(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self; return resolve_dotted(e, a);
}

static jvalue cls_isAssignableFrom(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeClass *me = (FakeClass *)jniref_deref(self);
  FakeClass *other = a ? (FakeClass *)jniref_deref(a[0].l) : NULL;
  for (FakeClass *k = other; k; k = k->super)
    if (k == me) { r.z = JNI_TRUE; return r; }
  return r;
}

static FakeMethod g_class_methods[] = {
  { cls_getName,          "getName",          "()Ljava/lang/String;",  NULL, 0 },
  { cls_getName,          "getCanonicalName", "()Ljava/lang/String;",  NULL, 0 },
  { cls_getSimpleName,    "getSimpleName",    "()Ljava/lang/String;",  NULL, 0 },
  { cls_forName,          "forName",          "(Ljava/lang/String;)Ljava/lang/Class;", NULL, 1 },
  { cls_isAssignableFrom, "isAssignableFrom", "(Ljava/lang/Class;)Z",  NULL, 0 },
};

static FakeObject g_loader_obj;
static FakeClass  g_class_ClassLoader;
static jobject    g_ref_loader;

static jvalue ldr_getSystem(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.l = g_ref_loader; return r;
}
static jvalue ldr_loadClass(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self; return resolve_dotted(e, a);
}
static jvalue ldr_getParent(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; return r;   /* bootstrap loader */
}

static FakeMethod g_loader_methods[] = {
  { ldr_getParent, "<init>", "(Ljava/lang/ClassLoader;)V", NULL, 0 },
  { ldr_getSystem,  "getSystemClassLoader", "()Ljava/lang/ClassLoader;", NULL, 1 },
  { ldr_loadClass,  "loadClass",  "(Ljava/lang/String;)Ljava/lang/Class;",  NULL, 0 },
  { ldr_loadClass,  "loadClass",  "(Ljava/lang/String;Z)Ljava/lang/Class;", NULL, 0 },
  { ldr_loadClass,  "findClass",  "(Ljava/lang/String;)Ljava/lang/Class;",  NULL, 0 },
  { ldr_getParent,  "getParent",  "()Ljava/lang/ClassLoader;",              NULL, 0 },
};

static FakeClass g_class_ClassLoader = {
  {NULL}, "java/lang/ClassLoader", NULL,
  g_loader_methods, (int)(sizeof(g_loader_methods)/sizeof(g_loader_methods[0])), NULL, 0
};

jobject java_lang_classloader(void) { return g_ref_loader; }

/* ------------------------------------------------------------------------ */
/* java/io writers -- the path an exception takes to become readable text    */
/* ------------------------------------------------------------------------ */

/* These exist for one reason: when Java.Interop cannot complete an operation
 * it wraps the managed exception, then renders it through
 * printStackTrace(PrintWriter) into a StringWriter and reads the result back.
 * Every one of those was a stub returning nothing, so the failure arrived as
 * "pending exception:" followed by an empty string -- the diagnosis destroyed
 * by the machinery meant to deliver it.
 *
 * A growable buffer and a few forwarding methods are enough to get the text
 * back. */
typedef struct { FakeObject hdr; char *buf; size_t len, cap; } FakeWriter;

static FakeClass g_class_StringWriter;
static FakeClass g_class_PrintWriter;

static void writer_append(FakeWriter *w, const char *text) {
  if (!w || !text) return;
  size_t add = strlen(text);
  if (w->len + add + 1 > w->cap) {
    size_t cap = w->cap ? w->cap * 2 : 512;
    while (cap < w->len + add + 1) cap *= 2;
    char *nb = realloc(w->buf, cap);
    if (!nb) return;
    w->buf = nb;
    w->cap = cap;
  }
  memcpy(w->buf + w->len, text, add);
  w->len += add;
  w->buf[w->len] = 0;
}

/* A PrintWriter wraps a Writer; follow the wrap so text lands in the buffer
 * the caller will read back. */
static FakeWriter *writer_target(jobject self) {
  FakeWriter *w = (FakeWriter *)jniref_deref(self);
  if (w && w->hdr.cls == &g_class_PrintWriter)
    w = (FakeWriter *)jniref_deref((jobject)w->buf);   /* stored target */
  return w;
}

static jvalue sw_init(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeWriter *w = (FakeWriter *)jniref_deref(self);
  if (w) { w->buf = NULL; w->len = w->cap = 0; }
  return r;
}

static jvalue sw_write(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeWriter *w = writer_target(self);
  FakeString *s2 = a ? (FakeString *)jniref_deref(a[0].l) : NULL;
  if (w && s2 && s2->utf) writer_append(w, s2->utf);
  return r;
}

static jvalue sw_println(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeWriter *w = writer_target(self);
  FakeString *s2 = a ? (FakeString *)jniref_deref(a[0].l) : NULL;
  if (w) { if (s2 && s2->utf) writer_append(w, s2->utf); writer_append(w, "\n"); }
  return r;
}

static jvalue sw_toString(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeWriter *w = (FakeWriter *)jniref_deref(self);
  r.l = jni_make_string((w && w->buf) ? w->buf : "");
  return r;
}

static jvalue sw_nop(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; return r;
}

static FakeMethod g_stringwriter_methods[] = {
  { sw_init,     "<init>",   "()V",                        NULL, 0 },
  { sw_write,    "write",    "(Ljava/lang/String;)V",       NULL, 0 },
  { sw_write,    "append",   "(Ljava/lang/CharSequence;)Ljava/lang/Appendable;", NULL, 0 },
  { sw_println,  "println",  "(Ljava/lang/String;)V",       NULL, 0 },
  { sw_toString, "toString", "()Ljava/lang/String;",        NULL, 0 },
  { sw_nop,      "flush",    "()V",                        NULL, 0 },
  { sw_nop,      "close",    "()V",                        NULL, 0 },
};

static FakeClass g_class_StringWriter = {
  {NULL}, "java/io/StringWriter", NULL, g_stringwriter_methods,
  (int)(sizeof(g_stringwriter_methods)/sizeof(g_stringwriter_methods[0])), NULL, 0,
  sizeof(FakeWriter)
};

/* The wrapped writer is kept in the buf slot -- this object never holds text
 * of its own, it only forwards. */
static jvalue pw_init(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeWriter *w = (FakeWriter *)jniref_deref(self);
  if (w && a) w->buf = (char *)a[0].l;
  return r;
}

static FakeMethod g_printwriter_methods[] = {
  { pw_init,     "<init>",   "(Ljava/io/Writer;)V",   NULL, 0 },
  { sw_write,    "write",    "(Ljava/lang/String;)V", NULL, 0 },
  { sw_write,    "print",    "(Ljava/lang/String;)V", NULL, 0 },
  { sw_println,  "println",  "(Ljava/lang/String;)V", NULL, 0 },
  { sw_println,  "println",  "(Ljava/lang/Object;)V", NULL, 0 },
  { sw_nop,      "flush",    "()V",                   NULL, 0 },
  { sw_nop,      "close",    "()V",                   NULL, 0 },
};

static FakeClass g_class_PrintWriter = {
  {NULL}, "java/io/PrintWriter", NULL, g_printwriter_methods,
  (int)(sizeof(g_printwriter_methods)/sizeof(g_printwriter_methods[0])), NULL, 0,
  sizeof(FakeWriter)
};

/* ------------------------------------------------------------------------ */

void java_lang_init(void) {
  jni_arrays_init();
  jni_register_class(&g_class_Object);
  jni_register_class(&g_class_Runtime);
  jni_register_class(&g_class_System);
  jni_register_class(&g_class_Thread);
  jni_register_class(&g_class_WeakRef);
  jni_register_class(&g_class_ClassLoader);
  jni_register_class(&g_class_StringWriter);
  jni_register_class(&g_class_PrintWriter);

  /* java/lang/Class is registered by jni_fake as the metaclass; give it the
   * methods Java.Interop expects rather than letting them be stubbed. */
  g_class_Class.methods  = g_class_methods;
  g_class_Class.nmethods = (int)(sizeof(g_class_methods)/sizeof(g_class_methods[0]));

  /* Object sits at the root of the hierarchy, so IsInstanceOf and inherited
   * method lookup behave. */
  g_class_Runtime.super = &g_class_Object;
  g_class_System.super  = &g_class_Object;
  g_class_Thread.super  = &g_class_Object;
  g_class_WeakRef.super = &g_class_Object;

  g_runtime_obj.cls = &g_class_Runtime;
  g_thread_obj.cls  = &g_class_Thread;

  /* Global refs: these singletons outlive any frame. */
  g_class_ClassLoader.super = &g_class_Object;
  g_loader_obj.cls = &g_class_ClassLoader;

  g_ref_runtime = jniref_new(&g_runtime_obj, REF_GLOBAL);
  g_ref_thread  = jniref_new(&g_thread_obj,  REF_GLOBAL);
  g_ref_loader  = jniref_new(&g_loader_obj,  REF_GLOBAL);

  debug_log("[java] core java.lang classes registered\n");
}
