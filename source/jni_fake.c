/* jni_fake.c -- the fake JNIEnv / JavaVM.
 *
 * Two design notes worth reading before you extend this.
 *
 * 1. The vtable is built with DESIGNATED INITIALIZERS against the real jni.h
 *    from the Android NDK (see README -- you must drop it into source/). Do
 *    not hand-write the struct: JNINativeInterface has ~230 function pointers
 *    in a fixed order, the game indexes it by offset, and one misplaced entry
 *    produces a jump into an unrelated function with a mismatched signature.
 *    Designated initializers make the order the compiler's problem.
 *
 * 2. Every entry we do NOT implement is filled at init with a trap that logs
 *    its index and aborts. That converts "mystery crash inside the runtime"
 *    into "JNI slot 118 called, not implemented" -- which you can look up in
 *    jni.h and implement. This is the single highest-leverage debugging aid in
 *    the whole port; do not remove it to save a few instructions.
 */

#include <switch.h>

#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem_arena.h"
#include "jni_arrays.h"
#include "android_text.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "watchdog.h"
#include "util.h"

/* Defined further down with the exception machinery, but called from the
 * lookup functions above it. Without this forward declaration the first call
 * creates an implicit non-static declaration and the real definition then
 * conflicts with it. */
static void raise_pending(const char *what, const char *detail);

/* ------------------------------------------------------------------------ */
/* Class registry                                                            */
/* ------------------------------------------------------------------------ */

/* Grown on demand rather than capped.
 *
 * The crypto PAL alone manufactured 79 classes before the old ceiling of 128
 * stopped it mid-list, and it was still going -- it pulls in the whole TLS
 * surface plus the java.util collections. Any fixed number here is a guess
 * that eventually fails in the middle of initialisation.
 *
 * Growing THIS array is safe because it holds POINTERS; the FakeClass objects
 * live elsewhere and do not move. The method and field tables below cannot be
 * grown the same way -- see the note there. */
static FakeClass **g_classes;
static int         g_nclasses;
static int         g_classes_cap;

FakeClass g_class_Class = { {NULL}, "java/lang/Class", NULL, NULL, 0, NULL, 0 };

void jni_register_class(FakeClass *c) {
  if (g_nclasses >= g_classes_cap) {
    int cap = g_classes_cap ? g_classes_cap * 2 : 128;
    FakeClass **grown = realloc(g_classes, (size_t)cap * sizeof(*grown));
    if (!grown) fatal_error("out of memory growing the class registry to %d", cap);
    g_classes = grown;
    g_classes_cap = cap;
  }
  c->hdr.cls = &g_class_Class;
  g_classes[g_nclasses++] = c;
}

FakeClass *jni_find_class(const char *name) {
  for (int i = 0; i < g_nclasses; i++)
    if (!strcmp(g_classes[i]->name, name)) return g_classes[i];
  return NULL;
}

/* ---- automatic stub classes ---------------------------------------------
 *
 * A missing class is fatal; a class that exists and does nothing usually is
 * not. The crypto PAL makes that asymmetry expensive: pal_jni.c caches global
 * refs for a long list of java.io, java.security and javax.crypto types the
 * moment it initialises, unconditionally, whether or not the game ever
 * performs a single cryptographic operation -- and this game has no
 * networking at all. Implementing them one per build cycle would take a dozen
 * runs to reach the same place.
 *
 * So an unknown class is manufactured on demand rather than refused, and every
 * one is logged with a marker that is easy to grep for. Methods on such a
 * class are manufactured too, returning zero.
 *
 * The trade is deliberate and worth stating plainly: this converts "aborts
 * naming the class" into "continues, possibly with wrong behaviour". That is
 * the right way round for a subsystem the game does not use, and the wrong way
 * round for one it does -- so if something misbehaves later, the [jni] auto
 * lines are the first place to look. Anything appearing there that the game
 * genuinely depends on should be implemented properly.
 */
typedef struct AutoClass {
  FakeClass  cls;
  char       name[160];
  FakeMethod methods[64];
  char       mname[64][72];
  char       msig[64][160];
  int        nmethods;
} AutoClass;

static AutoClass **g_auto;
static int g_nauto;
static int g_auto_cap;

/* Every stub returns zero. For a method declared to return void or a
 * primitive that is a defensible answer; for one declared to return an OBJECT
 * it is a null the caller did not expect, and null propagates silently until
 * something dereferences it or stores it where a reference belongs.
 *
 * That is not hypothetical here: a peer finalizer reported "Do not know how to
 * dispose: Invalid", which is a JniObjectReference holding a null. Our handles
 * always carry a non-zero kind in their low bits, so the only way to get one
 * is for something to have handed back a real null -- and a stub is the
 * obvious candidate.
 *
 * Creation was logged; invocation was not. Now the object-returning ones
 * announce themselves the first time they are actually called, which is the
 * difference between "we stubbed 200 methods" and "this one was used". */
static jvalue auto_method(JNIEnv *e, jobject self, const jvalue *a);


/* Recognise the fluent-builder pattern and return the object rather than null.
 *
 * Android is full of APIs that return `this` so calls can be chained --
 * AlertDialog.Builder, Notification.Builder, and many more. A stub returning
 * null breaks the chain at the first link, and the caller dereferences the
 * result immediately, so it dies far from the missing method.
 *
 * That cost two rounds on AlertDialog.Builder alone: the CharSequence overload
 * of setTitle was implemented, then setPositiveButton turned out to be called
 * through its resource-id overload, which was not. Enumerating overloads one
 * crash at a time is a losing game -- the pattern is what should be handled.
 *
 * The test is exact and cheap: a method whose declared return type is
 * `L<this object's own class>;` is returning itself, so return a fresh local
 * reference to it. Fresh, not the incoming handle: Mono.Android wraps a
 * returned object with TransferLocalRef and deletes it, which would free the
 * caller's own reference mid-chain.
 *
 * Anything else still returns zero, which is the honest answer for a method we
 * do not implement. */
static jvalue auto_method_fluent(JNIEnv *e, jobject self, const jvalue *a,
                                 const char *sig) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  if (!sig || !self) return r;

  const char *ret = strchr(sig, ')');
  if (!ret || ret[1] != 'L') return r;
  ret += 2;                                   /* past ")L" */

  FakeObject *o = jniref_deref(self);
  if (!o || !o->cls || !o->cls->name) return r;

  size_t n = strlen(o->cls->name);
  if (strncmp(ret, o->cls->name, n) != 0 || ret[n] != ';') return r;

  static const char *told[32];
  static int ntold;
  int seen = 0;
  for (int i = 0; i < ntold; i++) if (told[i] == o->cls->name) seen = 1;
  if (!seen && ntold < 32) {
    told[ntold++] = o->cls->name;
    debug_log("[jni] %s has fluent methods returning itself; unimplemented "
              "ones return the object rather than null so the chain "
              "survives\n", o->cls->name);
  }
  r.l = jniref_new(o, REF_LOCAL);
  return r;
}

/* The signature of the method currently being dispatched.
 *
 * FakeMethod.fn takes (env, self, args) and not the method itself, so the
 * stub cannot see its own declared return type. Rather than change that
 * signature across every implementation in the port, call_method parks it here
 * for the duration of the call. Thread-local because two threads dispatch
 * concurrently. */
static _Thread_local const char *tl_dispatch_sig;

/* Redirect targets currently on this thread's stack.
 *
 * Virtual dispatch on its own is unbounded recursion, and the boot log said so
 * in as many words:
 *
 *     [jni] virtual dispatch: onCreate(Landroid/os/Bundle;)V -> the
 *           implementation on .../AndroidNativeActivity        (x16, then a
 *     CRASH: write to SP-0x30 -- stack overflow, call_method_ex recursing
 *
 * Java.Interop's generated override calls `base.OnCreate(...)`, and that base
 * call reaches JNI with the method ID of the BASE class. Redirecting it to the
 * receiver's class sends it straight back into the override that is already
 * running. Every `super` call in the game is that shape.
 *
 * So a redirect target already active for this same receiver is not redirected
 * again: the call falls through to the implementation the ID actually named,
 * which is precisely what `super` means. Keyed on the pair, so two different
 * objects of the same class do not block each other. */
#define VDEPTH 64
static _Thread_local const void *tl_vmeth[VDEPTH];
static _Thread_local const void *tl_vself[VDEPTH];
static _Thread_local int         tl_vn;

static int vguard_active(const void *meth, const void *self) {
  for (int i = 0; i < tl_vn; i++)
    if (tl_vmeth[i] == meth && tl_vself[i] == self) return 1;
  return 0;
}

static jvalue auto_method(JNIEnv *e, jobject self, const jvalue *a) {
  return auto_method_fluent(e, self, a, tl_dispatch_sig);
}

static FakeClass *auto_make_class(const char *name) {
  if (g_nauto >= g_auto_cap) {
    int cap = g_auto_cap ? g_auto_cap * 2 : 128;
    AutoClass **grown = realloc(g_auto, (size_t)cap * sizeof(*grown));
    if (!grown) { debug_log("[jni] out of memory growing the auto-class pool\n"); return NULL; }
    g_auto = grown;
    g_auto_cap = cap;
  }
  AutoClass *a = calloc(1, sizeof(AutoClass));
  if (!a) return NULL;

  snprintf(a->name, sizeof(a->name), "%s", name);
  a->cls.name     = a->name;
  a->cls.methods  = a->methods;
  a->cls.nmethods = 0;
  a->cls.super    = NULL;

  g_auto[g_nauto++] = a;
  jni_register_class(&a->cls);
  debug_log("[jni] auto %s (stubbed; not implemented)\n", name);
  return &a->cls;
}

static AutoClass *auto_of(FakeClass *c) {
  for (int i = 0; i < g_nauto; i++)
    if (&g_auto[i]->cls == c) return g_auto[i];
  return NULL;
}

/* ---- method stubs on ANY class ------------------------------------------
 *
 * A class's method table is a fixed array, so a stub cannot be appended to it.
 * This side table holds the overflow and is consulted after the class's own
 * table misses -- which lets a REAL class receive a stub too.
 *
 * That case matters more than the manufactured one and is logged differently.
 * A manufactured class is one we deliberately declined to implement; a missing
 * method on a class we DID implement is a gap in our implementation, and if
 * anything consumes its return value the stub's zero will surface as a null
 * dereference somewhere else. java/lang/String.getBytes was exactly that: the
 * crypto layer uses the array it returns, so it is implemented properly rather
 * than stubbed. Treat every REAL-class line here as a question about whether
 * the result is used. */
/* Each entry is allocated on its own and threaded onto a list, NOT held in a
 * growable array.
 *
 * The address of the embedded FakeMethod is handed to the runtime as a
 * jmethodID and cached there indefinitely. Reallocating an array of these
 * would move every previously issued id, and the runtime would keep using the
 * old addresses -- a use-after-free that would surface as corruption far from
 * here. Individual allocation makes the addresses permanent, which is worth
 * more than the contiguity. */
typedef struct ExtMethod {
  struct ExtMethod *next;
  FakeClass *cls;
  FakeMethod m;
  char name[80];
  char sig[176];
} ExtMethod;

static ExtMethod *g_ext_head;
static int g_next_ext;

static FakeMethod *ext_find(FakeClass *c, const char *name, const char *sig) {
  for (ExtMethod *x = g_ext_head; x; x = x->next) {
    if (x->cls != c) continue;
    if (strcmp(x->name, name)) continue;
    if (sig && strcmp(x->sig, sig)) continue;
    return &x->m;
  }
  return NULL;
}

static FakeMethod *ext_make(FakeClass *c, const char *name, const char *sig) {
  if (!c) return NULL;
  ExtMethod *x = calloc(1, sizeof(ExtMethod));
  if (!x) { debug_log("[jni] out of memory stubbing %s.%s\n", c->name, name); return NULL; }
  x->next = g_ext_head;
  g_ext_head = x;
  g_next_ext++;
  x->cls = c;
  snprintf(x->name, sizeof(x->name), "%s", name);
  snprintf(x->sig,  sizeof(x->sig),  "%s", sig ? sig : "()V");
  x->m.fn        = auto_method;
  x->m.name      = x->name;
  x->m.sig       = x->sig;
  x->m.native_fn = NULL;
  x->m.is_static = 0;

  debug_log(auto_of(c) ? "[jni] auto %s.%s%s\n"
                       : "[jni] auto-method on REAL class %s.%s%s "
                         "-- check whether the caller uses the result\n",
            c->name, name, sig ? sig : "");
  return &x->m;
}

static FakeMethod *find_method(FakeClass *c, const char *name, const char *sig) {
  for (FakeClass *k = c; k; k = k->super)
    for (int i = 0; i < k->nmethods; i++)
      if (!strcmp(k->methods[i].name, name) &&
          (!sig || !strcmp(k->methods[i].sig, sig)))
        return &k->methods[i];
  return NULL;
}

/* ---- field stubs --------------------------------------------------------
 *
 * Same reasoning as the method stubs, with one extra wrinkle: the crypto PAL
 * does not merely look a static field up, it reads the VALUE and wraps it in a
 * global reference. A stub yielding null would satisfy GetStaticFieldID and
 * then fail the null check one step later -- trading a clear "field not found"
 * for a vaguer failure.
 *
 * So an object-typed stub field is given a stub INSTANCE of whatever class its
 * signature names, manufacturing that class too if necessary. Primitive fields
 * stay zero, which is a legitimate value for them. */
/* Individually allocated for the same reason as the methods: the embedded
 * FakeField's address becomes a jfieldID the runtime keeps. */
typedef struct ExtField {
  struct ExtField *next;
  FakeClass *cls;
  FakeField  f;
  char       name[80];
  char       sig[176];
} ExtField;

static ExtField *g_extf_head;
static int g_next_extf;

static FakeField *extf_find(FakeClass *c, const char *name, const char *sig) {
  for (ExtField *x = g_extf_head; x; x = x->next) {
    if (x->cls != c) continue;
    if (strcmp(x->name, name)) continue;
    if (sig && strcmp(x->sig, sig)) continue;
    return &x->f;
  }
  return NULL;
}

/* "Ljava/security/spec/MGF1ParameterSpec;" -> the class name inside. */
static int class_name_of_sig(const char *sig, char *out, size_t n) {
  if (!sig || sig[0] != 'L') return 0;
  const char *end = strchr(sig, ';');
  if (!end) return 0;
  size_t len = (size_t)(end - sig - 1);
  if (len == 0 || len >= n) return 0;
  memcpy(out, sig + 1, len);
  out[len] = 0;
  return 1;
}

FakeField *jni_stub_field(FakeClass *c, const char *name, const char *sig) {
  if (!c) return NULL;
  ExtField *x = calloc(1, sizeof(ExtField));
  if (!x) return NULL;
  x->next = g_extf_head;
  g_extf_head = x;
  g_next_extf++;
  x->cls = c;
  snprintf(x->name, sizeof(x->name), "%s", name);
  snprintf(x->sig,  sizeof(x->sig),  "%s", sig ? sig : "I");
  x->f.name = x->name;
  x->f.sig  = x->sig;

  /* A field of type java/lang/Class named after a type -- the convention
   * .NET for Android uses for its cached class handles, e.g.
   * mono_android_GCUserPeer -> mono/android/GCUserPeer -- must resolve to that
   * CLASS, not to a nondescript instance of java.lang.Class. Handing back the
   * latter is what produced "(null).<init>()V": the bridge treated the stub as
   * a class and read a name field that was never allocated. */
  if (!strcmp(x->sig, "Ljava/lang/Class;") && strchr(name, '_')) {
    char guess[160];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(guess); i++)
      guess[i] = (name[i] == '_') ? '/' : name[i];
    guess[i] = 0;

    FakeClass *target = jni_find_class(guess);
    if (target) {
      x->f.value.l = jniref_new(&target->hdr, REF_GLOBAL);
      debug_log("[jni] field %s.%s resolved to class %s\n",
                c->name, name, guess);
      return &x->f;
    }
    debug_log("[jni] field %s.%s looks like a class handle for '%s', "
              "but no such class is registered\n", c->name, name, guess);
  }

  char cname[160];
  if (class_name_of_sig(x->sig, cname, sizeof(cname))) {
    FakeClass *fc = jni_find_class(cname);
    if (!fc) fc = auto_make_class(cname);
    if (fc) {
      FakeObject *inst = calloc(1, sizeof(FakeObject));
      if (inst) {
        inst->cls = fc;
        /* Global, because the caller will hold it for the process lifetime. */
        x->f.value.l = jniref_new(inst, REF_GLOBAL);
      }
    }
  }

  debug_log(auto_of(c) ? "[jni] auto %s.%s:%s\n"
                       : "[jni] auto-field on REAL class %s.%s:%s "
                         "-- check whether the caller uses the value\n",
            c->name, name, sig ? sig : "");
  return &x->f;
}

FakeField *jni_find_stub_field(FakeClass *c, const char *name, const char *sig) {
  return extf_find(c, name, sig);
}

/* ------------------------------------------------------------------------ */
/* Core JNI                                                                  */
/* ------------------------------------------------------------------------ */

static jint JNICALL f_GetVersion(JNIEnv *env) { (void)env; return JNI_VERSION_1_6; }

static jclass JNICALL f_FindClass(JNIEnv *env, const char *name) {
  (void)env;
  FakeClass *c = jni_find_class(name);
  if (!c) c = auto_make_class(name);
  if (!c) {
    raise_pending("java/lang/ClassNotFoundException", name);
    return NULL;
  }
  return (jclass)jniref_new(&c->hdr, REF_LOCAL);
}

static jmethodID JNICALL f_GetMethodID(JNIEnv *env, jclass clazz,
                                       const char *name, const char *sig) {
  (void)env;
  FakeClass *c = (FakeClass *)jniref_deref((jobject)clazz);
  if (!c) {
    /* Silent before. A caller that aborts on a null method id reports the
     * method, or worse the class it wanted -- never that the handle it passed
     * had already been released. */
    debug_log("[jni] GetMethodID(%s%s): the class handle %p did not resolve\n",
              name, sig ? sig : "", (void *)clazz);
    return NULL;
  }
  FakeMethod *m = find_method(c, name, sig);
  if (!m) m = ext_find(c, name, sig);
  if (!m) m = ext_make(c, name, sig);
  if (!m) {
    char detail[256];
    snprintf(detail, sizeof(detail), "%s.%s%s", c->name, name, sig ? sig : "");
    raise_pending("java/lang/NoSuchMethodError", detail);
    return NULL;
  }
  return (jmethodID)m;
}

static jmethodID JNICALL f_GetStaticMethodID(JNIEnv *env, jclass clazz,
                                             const char *name, const char *sig) {
  return f_GetMethodID(env, clazz, name, sig);
}

/* RegisterNatives is how the managed side hands us the n_* entry points.
 * Everything the game does after startup goes through pointers captured here,
 * so log all of them -- the log is your index of what you can call. */
static jint JNICALL f_RegisterNatives(JNIEnv *env, jclass clazz,
                                      const JNINativeMethod *methods, jint n) {
  (void)env;
  FakeClass *c = (FakeClass *)jniref_deref((jobject)clazz);
  if (!c) { debug_log("[jni] RegisterNatives on unknown class\n"); return JNI_ERR; }

  for (jint i = 0; i < n; i++) {
    FakeMethod *m = find_method(c, methods[i].name, methods[i].signature);
    if (!m) m = find_method(c, methods[i].name, NULL);
    if (m) {
      m->native_fn = methods[i].fnPtr;
      /* Registration is the runtime declaring it has finished generating this
       * stub, so the block can safely become executable now rather than on
       * first use. */
      code_ensure_executable(methods[i].fnPtr);
      debug_log("[jni] RegisterNatives %s.%s%s -> %p\n",
                c->name, methods[i].name, methods[i].signature, methods[i].fnPtr);
    } else {
      /* Declare the method in the class table in lawn_natives.c and it will be
       * captured on the next run. */
      debug_log("[jni] RegisterNatives %s.%s%s -> DROPPED (no such method declared)\n",
                c->name, methods[i].name, methods[i].signature);
    }
  }
  return JNI_OK;
}

/* -- references ---------------------------------------------------------- */

static jobject JNICALL f_NewGlobalRef(JNIEnv *env, jobject o) {
  (void)env; return jniref_new(jniref_deref(o), REF_GLOBAL);
}
static jobject JNICALL f_NewWeakGlobalRef(JNIEnv *env, jobject o) {
  (void)env; return jniref_new(jniref_deref(o), REF_WEAK);
}
static jobject JNICALL f_NewLocalRef(JNIEnv *env, jobject o) {
  (void)env; return jniref_new(jniref_deref(o), REF_LOCAL);
}
/* Each Delete* releases ONLY its own kind of reference.
 *
 * A handle encodes its kind, and jniref_delete honoured that kind whoever
 * asked -- so DeleteLocalRef on a global handle freed the global slot. That is
 * exactly what happens around a cached class handle: the caller takes a
 * NewGlobalRef of a field value and then releases what it thinks is the local
 * original. Every later read of that field then resolved to a freed slot and
 * came back null, and the abort blamed the class rather than the release.
 *
 * Real JNI treats a mismatched Delete* as undefined and implementations
 * ignore it. Ignoring it here is both correct and the fix. */
static void delete_checked(jobject o, int kind, const char *who) {
  if (!o) return;
  int actual = jniref_kind(o);
  if (actual != kind) {
    debug_log("[jni] %s called on a %s reference -- ignored\n", who,
              actual == REF_LOCAL  ? "local" :
              actual == REF_GLOBAL ? "global" :
              actual == REF_WEAK   ? "weak global" : "invalid");
    return;
  }
  jniref_delete(o);
}

static void JNICALL f_DeleteGlobalRef(JNIEnv *env, jobject o) {
  (void)env; delete_checked(o, REF_GLOBAL, "DeleteGlobalRef");
}
static void JNICALL f_DeleteWeakGlobalRef(JNIEnv *env, jweak o) {
  (void)env; delete_checked((jobject)o, REF_WEAK, "DeleteWeakGlobalRef");
}
static void JNICALL f_DeleteLocalRef(JNIEnv *env, jobject o) {
  (void)env; delete_checked(o, REF_LOCAL, "DeleteLocalRef");
}

static jobjectRefType JNICALL f_GetObjectRefType(JNIEnv *env, jobject o) {
  (void)env;
  int kind = jniref_kind(o);

  /* Invalid is what Java.Interop reports as "Do not know how to dispose", and
   * the exception names the type rather than the reference -- so without this
   * the log points at the symptom. A null handle answering Invalid is correct
   * per the specification; a non-null one doing so means the handle is
   * malformed, which is a different problem entirely. */
  if (kind == REF_INVALID)
    debug_log("[jni] GetObjectRefType(%p) -> Invalid%s\n", (void *)o,
              o ? " (non-null handle -- malformed)" : " (null handle)");
  return (jobjectRefType)kind;
}

static jboolean JNICALL f_IsSameObject(JNIEnv *env, jobject a, jobject b) {
  (void)env;
  return jniref_deref(a) == jniref_deref(b) ? JNI_TRUE : JNI_FALSE;
}

static jint JNICALL f_PushLocalFrame(JNIEnv *env, jint cap) {
  (void)env; jniref_push_frame(cap); return JNI_OK;
}
static jobject JNICALL f_PopLocalFrame(JNIEnv *env, jobject result) {
  (void)env; return jniref_pop_frame(result);
}
static jint JNICALL f_EnsureLocalCapacity(JNIEnv *env, jint cap) {
  (void)env; (void)cap; return JNI_OK;
}

/* Announce every object the game constructs whose class looks like a view.
 *
 * If the Activity builds its own LawnSurfaceView instead of using the one we
 * constructed, two of them exist and we drive the wrong one. That would be
 * invisible today: object creation is silent, and both would answer JNI calls
 * identically. Cheap to say out loud, once per object. */
void jni_note_object_created(FakeObject *o) {
  if (!o || !o->cls || !o->cls->name) return;
  if (!strstr(o->cls->name, "SurfaceView") && !strstr(o->cls->name, "GameView"))
    return;
  debug_log("[jni] a %s object was constructed at %p\n", o->cls->name,
            (void *)o);
}

static jclass JNICALL f_GetObjectClass(JNIEnv *env, jobject o) {
  (void)env;
  FakeObject *obj = jniref_deref(o);
  if (!obj || !obj->cls) {
    /* Peer creation starts here. A null class means the caller gets no peer,
     * which it reports as a null object rather than an error -- so say it now,
     * while the handle is still in hand. */
    debug_log("[jni] GetObjectClass(%p) has no class -- any peer built from "
              "this handle will come back null\n", (void *)o);
    return NULL;
  }
  return (jclass)jniref_new(&obj->cls->hdr, REF_LOCAL);
}

static jboolean JNICALL f_IsInstanceOf(JNIEnv *env, jobject o, jclass clazz) {
  (void)env;
  FakeObject *obj = jniref_deref(o);
  FakeClass  *c   = (FakeClass *)jniref_deref((jobject)clazz);
  if (!obj || !c) return JNI_FALSE;
  for (FakeClass *k = obj->cls; k; k = k->super) if (k == c) return JNI_TRUE;
  return JNI_FALSE;
}

/* -- exceptions ---------------------------------------------------------- */

/* A pending exception has to be a real object, and lookups that fail have to
 * raise one.
 *
 * This is not pedantry about the spec. Callers follow FindClass with
 * ExceptionCheck, and a NULL result paired with "no exception pending" tells
 * the runtime the lookup succeeded and produced null -- so it carries on and
 * dereferences it. Reporting the failure properly turns a crash into an
 * exception the runtime already knows how to handle, and puts the missing
 * class name in the log instead of leaving a mystery. */
typedef struct { FakeObject hdr; char msg[192]; } FakeThrowable;

/* getMessage's result is read and printed, so a stub returning null would
 * throw away the one useful thing about an exception. The text is already in
 * the object; hand it over. getCause legitimately returns null -- we never
 * chain exceptions -- so that one is honest rather than lossy. */
static jvalue thr_getMessage(JNIEnv *e, jobject self, const jvalue *a);
static jvalue thr_getCause(JNIEnv *e, jobject self, const jvalue *a);

/* Writes the message into the supplied writer. Java.Interop renders an
 * exception this way and reads the writer back, so a stub here means the
 * caller receives an empty string and the reason for the failure is destroyed
 * by the machinery meant to report it. */
static jvalue thr_printStackTrace(JNIEnv *e, jobject self, const jvalue *a) {
  jvalue r; memset(&r, 0, sizeof(r));
  FakeThrowable *t = (FakeThrowable *)jniref_deref(self);
  const char *msg = t ? t->msg : "(no message)";

  if (!a || !a[0].l) { debug_log("[jni] printStackTrace: %s\n", msg); return r; }

  jclass wc = (*e)->GetObjectClass(e, a[0].l);
  if (wc) {
    jmethodID mid = (*e)->GetMethodID(e, wc, "println", "(Ljava/lang/String;)V");
    if (mid) (*e)->CallVoidMethod(e, a[0].l, mid, (*e)->NewStringUTF(e, msg));
    (*e)->ExceptionClear(e);
  }
  debug_log("[jni] printStackTrace: %s\n", msg);
  return r;
}

static FakeMethod g_throwable_methods[] = {
  { thr_printStackTrace, "printStackTrace", "(Ljava/io/PrintWriter;)V", NULL, 0 },
  { thr_printStackTrace, "printStackTrace", "(Ljava/io/PrintStream;)V", NULL, 0 },
  { thr_printStackTrace, "printStackTrace", "()V",                      NULL, 0 },
  { thr_getMessage, "getMessage",          "()Ljava/lang/String;",    NULL, 0 },
  { thr_getMessage, "getLocalizedMessage", "()Ljava/lang/String;",    NULL, 0 },
  { thr_getMessage, "toString",            "()Ljava/lang/String;",    NULL, 0 },
  { thr_getCause,   "getCause",            "()Ljava/lang/Throwable;", NULL, 0 },
};

static FakeClass g_class_Throwable = {
  {NULL}, "java/lang/Throwable", NULL,
  g_throwable_methods,
  (int)(sizeof(g_throwable_methods)/sizeof(g_throwable_methods[0])), NULL, 0,
  sizeof(FakeThrowable)
};

/* Per-thread, for two reasons that compound. JNI specifies exception state as
 * thread-local, and the reference we store here is a LOCAL ref -- and local
 * tables became per-thread in the audit. A single shared slot meant a pending
 * exception raised on one thread was resolved against a different thread's
 * table when read, yielding a stale handle or a wrong object. */
static __thread jthrowable g_pending;

static jvalue thr_getMessage(JNIEnv *e, jobject self, const jvalue *a) {
  (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeThrowable *t = (FakeThrowable *)jniref_deref(self);
  if (t) r.l = (*e)->NewStringUTF(e, t->msg);
  return r;
}

static jvalue thr_getCause(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));   /* never chained */
  return r;
}

static void raise_pending(const char *what, const char *detail) {
  if (g_pending) return;          /* first exception wins, as in real JNI */
  FakeThrowable *t = calloc(1, sizeof(FakeThrowable));
  if (!t) return;
  t->hdr.cls = &g_class_Throwable;
  snprintf(t->msg, sizeof(t->msg), "%s: %s", what, detail ? detail : "?");
  g_pending = (jthrowable)jniref_new(&t->hdr, REF_LOCAL);
  debug_log("[jni] raising %s\n", t->msg);
}

static jthrowable JNICALL f_ExceptionOccurred(JNIEnv *env) { (void)env; return g_pending; }
static void JNICALL f_ExceptionClear(JNIEnv *env)          { (void)env; g_pending = NULL; }
static jboolean JNICALL f_ExceptionCheck(JNIEnv *env)      { (void)env; return g_pending ? JNI_TRUE : JNI_FALSE; }

static void JNICALL f_ExceptionDescribe(JNIEnv *env) {
  if (!g_pending) { debug_log("[jni] pending exception: (none)\n"); return; }

  /* Ask the object rather than reading a field off it.
   *
   * A pending exception can be one of ours or a JavaProxyThrowable carrying a
   * managed message, and the two have different layouts. Casting to the one we
   * happen to know reads the right bytes only by coincidence -- toString goes
   * through whichever implementation the object actually has. */
  jclass c = (*env)->GetObjectClass(env, (jobject)g_pending);
  if (c) {
    jmethodID mid = (*env)->GetMethodID(env, c, "toString", "()Ljava/lang/String;");
    if (mid) {
      jthrowable saved = g_pending;
      g_pending = NULL;                     /* toString must not see it pending */
      jobject str = (*env)->CallObjectMethod(env, (jobject)saved, mid);
      g_pending = saved;

      FakeString *fs = (FakeString *)jniref_deref(str);
      if (fs && fs->utf && fs->utf[0]) {
        debug_log("[jni] pending exception: %s\n", fs->utf);
        return;
      }
    }
    (*env)->ExceptionClear(env);
    g_pending = g_pending;                  /* ExceptionClear must not lose it */
  }
  debug_log("[jni] pending exception: (no message available)\n");
}

static jint JNICALL f_ThrowNew(JNIEnv *env, jclass c, const char *msg) {
  (void)env;
  FakeClass *k = (FakeClass *)jniref_deref((jobject)c);
  raise_pending(k ? k->name : "java/lang/Throwable", msg);
  return JNI_OK;
}

static jint JNICALL f_Throw(JNIEnv *env, jthrowable obj) {
  (void)env;
  if (obj) g_pending = obj;
  return JNI_OK;
}

/* -- method dispatch ------------------------------------------------------ */

/* JNI's Call<Type>Method is VIRTUAL: it dispatches on the runtime class of the
 * object, not on the class the method ID was looked up in. This did not, and
 * the consequences were invisible and wide.
 *
 * Java.Interop caches `id_toString` once, from java/lang/Object, and uses it for
 * every object it ever prints. Without virtual dispatch that always reached
 * java_lang.c's obj_toString, which returns "<class name>@<pointer>" -- and
 * truncated to the width of a text box that reads
 *
 *     android/text/Spannab
 *
 * which is exactly what was appearing in the rename field. Implementing
 * SpannableStringBuilder.toString() changed nothing, because it was never
 * reached. The same applies to every override of an Object method: equals,
 * hashCode, toString were all unreachable on every class in the shim.
 *
 * CallNonvirtual<Type>Method passes virt = 0 and keeps the old behaviour, which
 * is what that family is for.
 */
static jvalue call_method_ex(JNIEnv *env, jobject self, jmethodID mid,
                             const jvalue *args, int virt) {
  jvalue zero; memset(&zero, 0, sizeof(zero));
  FakeMethod *m = (FakeMethod *)mid;
  if (!m) return zero;

  /* A method registered through RegisterNatives is implemented in the game
   * image, not here. Java.Interop's bootstrap classes are all of this kind --
   * ManagedPeer.registerNativeMembers and friends are `public static native`
   * on the Java side -- so this branch is what makes Stage 3 possible at all.
   * It takes precedence: if the image supplied an implementation, ours (if
   * any) was only ever a placeholder. */
  /* Breadcrumb for the stall report. The class name comes from the object
   * rather than the method, since the same FakeMethod can be reached through
   * a subclass. */
  FakeObject *self_obj = jniref_deref(self);
  int redirected = 0;                 /* pushed onto the re-entrancy guard */

  /* Re-resolve on the object's actual class. Skipped for static methods (where
   * `self` is a jclass), for constructors, and when the ID already belongs to
   * the object's own table -- that last check is a pointer-range test, so the
   * common case costs almost nothing. */
  if (virt && self_obj && self_obj->cls && m->name &&
      !(m->is_static & 1) && strcmp(m->name, "<init>") != 0) {
    FakeClass *rc = self_obj->cls;
    int already_ours = rc->methods && m >= rc->methods && m < rc->methods + rc->nmethods;
    if (!already_ours) {
      FakeMethod *ov = find_method(rc, m->name, m->sig);

      /* An auto stub is NOT an override.
       *
       * auto_method entries exist because someone once asked for that method
       * ID on that class, not because the class implements anything. Without
       * this test, a real implementation reached through a base class -- 
       * java/lang/Object.toString is the obvious one -- would be redirected
       * into a stub that returns null, which is worse than the wrong-but-
       * non-null answer it replaced. Redirect only to something real. */
      int real_impl = ov && (ov->native_fn || (ov->fn && ov->fn != auto_method));
      /* `tl_vn < VDEPTH` is part of the CONDITION, not a check afterwards:
       * redirecting without recording it would reopen the recursion the guard
       * exists to close. Out of slots means do not redirect. */
      if (ov && ov != m && real_impl && !vguard_active(ov, self_obj) &&
          tl_vn < VDEPTH) {
        static int told;
        if (told < 16) {
          told++;
          debug_log("[jni] virtual dispatch: %s%s -> the implementation on %s\n",
                    m->name, m->sig ? m->sig : "", rc->name ? rc->name : "?");
        }
        tl_vmeth[tl_vn] = ov;
        tl_vself[tl_vn] = self_obj;
        tl_vn++;
        redirected = 1;
        m = ov;
      }
    }
  }

  watchdog_note_call(self_obj && self_obj->cls ? self_obj->cls->name : NULL,
                     m->name);

  if (m->native_fn) {
    jvalue nr = jni_invoke_native(env, m->native_fn, self, m->sig, args);
    if (redirected) tl_vn--;
    watchdog_note_return();
    return nr;
  }

  if (!m->fn) {
    debug_log("[jni] call to unimplemented method %s%s\n",
              m->name, m->sig ? m->sig : "");
    if (redirected) tl_vn--;
    watchdog_note_return();
    return zero;
  }
  const char *saved_sig = tl_dispatch_sig;
  tl_dispatch_sig = m->sig;
  jvalue res = m->fn(env, self, args);
  tl_dispatch_sig = saved_sig;      /* restored, because calls nest */
  if (redirected) tl_vn--;
  watchdog_note_return();

  /* Any method declared to return an object, returning null.
   *
   * This was previously checked only for auto-stubs, which missed the larger
   * source: methods we DO implement but wired to a no-op because the return
   * looked unimportant -- getDecorView, getIntent, getSurfaceFrame and so on.
   * They are indistinguishable from a working call at the call site, and the
   * null surfaces later as a hardware null check in managed code, which is
   * fatal here rather than a NullReferenceException.
   *
   * Reported once per method, so a call in a loop says it once. */
  if (m->sig) {
    const char *close = strchr(m->sig, ')');
    if (close && (close[1] == 'L' || close[1] == '[') && !res.l &&
        !(m->is_static & 0x40)) {
      m->is_static |= 0x40;
      debug_log("[jni] NULL OBJECT from %s%s -- declared to return an object. "
                "If a crash follows with a read of address 0, this is why.\n",
                m->name, m->sig);
    }

    /* The same problem for PRIMITIVE returns, which the check above misses.
     *
     * A method returning zero or false does not crash, so nothing forces it
     * into the log -- it just gives a wrong answer that surfaces somewhere
     * else entirely. That is not hypothetical: View.requestFocus returning
     * false convinced the game it was in the background and stopped rendering
     * for several rounds, and Intent.getIntExtra returned 0 instead of the
     * caller's own default.
     *
     * So: any zero or false coming out of a method that is not implemented
     * says so, once. Most will be harmless -- zero is often the right answer
     * -- but they are now a list to check rather than a blind spot, and the
     * next one of these costs a grep instead of a test cycle. */
    /* Restricted to auto-methods -- methods we never implemented at all.
     * Applying it to everything would flag every legitimate getInt() that
     * happens to return 0, and a diagnostic that cries wolf gets ignored. */
    if (close && m->fn == auto_method && !(m->is_static & 0x80)) {
      char ret = close[1];
      int zero = 0;
      switch (ret) {
        case 'Z': case 'B': case 'C': case 'S': case 'I': zero = (res.i == 0); break;
        case 'J': zero = (res.j == 0); break;
        case 'F': zero = (res.f == 0.0f); break;
        case 'D': zero = (res.d == 0.0); break;
        default: break;
      }
      if (zero) {
        m->is_static |= 0x80;
        debug_log("[jni] %s%s returned %s -- it is a stub, so this is a "
                  "guess rather than an answer. Harmless if the caller ignores "
                  "it; check here first if behaviour is wrong.\n",
                  m->name, m->sig, ret == 'Z' ? "false" : "0");
      }
    }
  }
  return res;
}

/* va_list -> jvalue[]. We cannot know the argument types from the va_list
 * alone, so parse the descriptor. Only the shapes that actually occur in this
 * game's signatures are handled; anything else logs loudly rather than
 * silently reading the wrong register. */
static void va_to_jvalues(const char *sig, va_list ap, jvalue *out, int max) {
  const char *p = strchr(sig, '(');
  if (!p) return;
  p++;
  int n = 0;
  while (*p && *p != ')' && n < max) {
    switch (*p) {
      case 'Z': out[n++].z = (jboolean)va_arg(ap, int);    p++; break;
      case 'B': out[n++].b = (jbyte)va_arg(ap, int);       p++; break;
      case 'C': out[n++].c = (jchar)va_arg(ap, int);       p++; break;
      case 'S': out[n++].s = (jshort)va_arg(ap, int);      p++; break;
      case 'I': out[n++].i = va_arg(ap, jint);             p++; break;
      case 'J': out[n++].j = va_arg(ap, jlong);            p++; break;
      case 'F': out[n++].f = (jfloat)va_arg(ap, double);   p++; break;
      case 'D': out[n++].d = va_arg(ap, jdouble);          p++; break;
      case 'L': out[n++].l = va_arg(ap, jobject);
                while (*p && *p != ';') p++;
                if (*p) p++;
                break;
      case '[': { while (*p == '[') p++;
                  if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; }
                  else p++;
                  out[n++].l = va_arg(ap, jobject); }       break;
      default:
        debug_log("[jni] unparsed signature char '%c' in %s\n", *p, sig);
        return;
    }
  }
}

/* Instance calls. Static and non-virtual calls go straight to call_method_ex
 * with virt = 0: a static call has no receiver to dispatch on, and relying on
 * every table in the port having its is_static flag right would be a worse
 * guarantee than not asking the question. */
static jvalue call_method(JNIEnv *env, jobject self, jmethodID mid,
                          const jvalue *args) {
  return call_method_ex(env, self, mid, args, 1);
}

/* Generate Call<Type>Method / ...V / ...A for each return type. */
#define CALL_VARIANTS(Name, Type, field)                                        \
  static Type JNICALL f_Call##Name##MethodA(JNIEnv *env, jobject o,             \
                                            jmethodID m, const jvalue *a) {     \
    return call_method(env, o, m, a).field;                                     \
  }                                                                             \
  static Type JNICALL f_Call##Name##MethodV(JNIEnv *env, jobject o,             \
                                            jmethodID m, va_list ap) {          \
    jvalue args[16]; memset(args, 0, sizeof(args));                             \
    FakeMethod *fm = (FakeMethod *)m;                                           \
    if (fm) va_to_jvalues(fm->sig, ap, args, 16);                               \
    return call_method(env, o, m, args).field;                                  \
  }                                                                             \
  static Type JNICALL f_Call##Name##Method(JNIEnv *env, jobject o,              \
                                           jmethodID m, ...) {                  \
    va_list ap; va_start(ap, m);                                                \
    Type r = f_Call##Name##MethodV(env, o, m, ap);                              \
    va_end(ap);                                                                 \
    return r;                                                                   \
  }

CALL_VARIANTS(Object,  jobject,  l)
CALL_VARIANTS(Boolean, jboolean, z)
CALL_VARIANTS(Byte,    jbyte,    b)
CALL_VARIANTS(Char,    jchar,    c)
CALL_VARIANTS(Short,   jshort,   s)
CALL_VARIANTS(Int,     jint,     i)
CALL_VARIANTS(Long,    jlong,    j)
CALL_VARIANTS(Float,   jfloat,   f)
CALL_VARIANTS(Double,  jdouble,  d)

static void JNICALL f_CallVoidMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) {
  call_method(env, o, m, a);
}
static void JNICALL f_CallVoidMethodV(JNIEnv *env, jobject o, jmethodID m, va_list ap) {
  jvalue args[16]; memset(args, 0, sizeof(args));
  FakeMethod *fm = (FakeMethod *)m;
  if (fm) va_to_jvalues(fm->sig, ap, args, 16);
  call_method(env, o, m, args);
}
static void JNICALL f_CallVoidMethod(JNIEnv *env, jobject o, jmethodID m, ...) {
  va_list ap; va_start(ap, m);
  f_CallVoidMethodV(env, o, m, ap);
  va_end(ap);
}

/* Static variants occupy their own vtable slots. Java.Interop reaches the
 * bootstrap almost entirely through static calls, so omitting these means
 * Stage 3 dies on the first ManagedPeer call. The dispatcher is the same --
 * `self` is simply the jclass rather than an instance. */
#define CALL_STATIC_VARIANTS(Name, Type, field)                                 \
  static Type JNICALL f_CallStatic##Name##MethodA(JNIEnv *env, jclass c,        \
                                                  jmethodID m, const jvalue *a) {\
    return call_method_ex(env, (jobject)c, m, a, 0).field;                            \
  }                                                                             \
  static Type JNICALL f_CallStatic##Name##MethodV(JNIEnv *env, jclass c,        \
                                                  jmethodID m, va_list ap) {    \
    jvalue args[16]; memset(args, 0, sizeof(args));                             \
    FakeMethod *fm = (FakeMethod *)m;                                           \
    if (fm) va_to_jvalues(fm->sig, ap, args, 16);                               \
    return call_method_ex(env, (jobject)c, m, args, 0).field;                         \
  }                                                                             \
  static Type JNICALL f_CallStatic##Name##Method(JNIEnv *env, jclass c,         \
                                                 jmethodID m, ...) {            \
    va_list ap; va_start(ap, m);                                                \
    Type r = f_CallStatic##Name##MethodV(env, c, m, ap);                        \
    va_end(ap);                                                                 \
    return r;                                                                   \
  }

CALL_STATIC_VARIANTS(Object,  jobject,  l)
CALL_STATIC_VARIANTS(Boolean, jboolean, z)
CALL_STATIC_VARIANTS(Byte,    jbyte,    b)
CALL_STATIC_VARIANTS(Char,    jchar,    c)
CALL_STATIC_VARIANTS(Short,   jshort,   s)
CALL_STATIC_VARIANTS(Int,     jint,     i)
CALL_STATIC_VARIANTS(Long,    jlong,    j)
CALL_STATIC_VARIANTS(Float,   jfloat,   f)
CALL_STATIC_VARIANTS(Double,  jdouble,  d)

static void JNICALL f_CallStaticVoidMethodA(JNIEnv *env, jclass c, jmethodID m,
                                            const jvalue *a) {
  call_method_ex(env, (jobject)c, m, a, 0);
}
static void JNICALL f_CallStaticVoidMethodV(JNIEnv *env, jclass c, jmethodID m,
                                            va_list ap) {
  jvalue args[16]; memset(args, 0, sizeof(args));
  FakeMethod *fm = (FakeMethod *)m;
  if (fm) va_to_jvalues(fm->sig, ap, args, 16);
  call_method_ex(env, (jobject)c, m, args, 0);
}
static void JNICALL f_CallStaticVoidMethod(JNIEnv *env, jclass c, jmethodID m, ...) {
  va_list ap; va_start(ap, m);
  f_CallStaticVoidMethodV(env, c, m, ap);
  va_end(ap);
}

/* Object allocation.
 *
 * These were left on the trap-fill, which would have aborted the moment the
 * GC bridge tried to create a peer -- it fetches the GCUserPeer class and
 * immediately instantiates it. A bare FakeObject carrying the right class
 * pointer is enough: our objects have no fields of their own unless a specific
 * implementation gives them some, and identity is what the bridge is after. */
static jobject alloc_instance(jclass clazz) {
  FakeClass *c = (FakeClass *)jniref_deref((jobject)clazz);
  if (!c) return NULL;
  size_t sz = c->instance_size ? c->instance_size : sizeof(FakeObject);
  FakeObject *o = calloc(1, sz);
  if (!o) return NULL;
  o->cls = c;
  /* Deliberately NOT heap_owned: jniref_free_owned casts to FakeArray and
   * frees a->data, which is only meaningful for arrays. Marking generic
   * objects owned would hand it a garbage pointer to free. Reclaiming these
   * needs a per-class destructor, which is a separate job. */
  jni_note_object_created(o);
  return jniref_new(o, REF_LOCAL);
}

static jobject JNICALL f_AllocObject(JNIEnv *env, jclass clazz) {
  (void)env;
  return alloc_instance(clazz);
}

/* The constructor runs only if the class declares one; a stub <init> is a
 * no-op, which is correct for objects that carry no state. */
static jobject JNICALL f_NewObjectA(JNIEnv *env, jclass clazz, jmethodID mid,
                                    const jvalue *args) {
  jobject o = alloc_instance(clazz);
  if (o && mid) call_method(env, o, mid, args);
  return o;
}
static jobject JNICALL f_NewObjectV(JNIEnv *env, jclass clazz, jmethodID mid,
                                    va_list ap) {
  jvalue a[16];
  memset(a, 0, sizeof(a));
  FakeMethod *fm = (FakeMethod *)mid;
  if (fm) va_to_jvalues(fm->sig, ap, a, 16);
  return f_NewObjectA(env, clazz, mid, a);
}
static jobject JNICALL f_NewObject(JNIEnv *env, jclass clazz, jmethodID mid, ...) {
  va_list ap;
  va_start(ap, mid);
  jobject o = f_NewObjectV(env, clazz, mid, ap);
  va_end(ap);
  return o;
}

/* -- reflection bridging ---------------------------------------------------
 *
 * Java.Interop converts between jmethodID/jfieldID and java.lang.reflect
 * objects when it marshals members across the boundary. These were the last
 * slots still trapping.
 *
 * We cannot produce a real reflect.Method, but we do not need to: what the
 * runtime does with one is hand it back later to recover the id. So the object
 * simply carries the id, and the round trip is exact. Anything that tried to
 * introspect it would get a stub -- which is the same bargain as everywhere
 * else here, and is logged the same way. */
typedef struct { FakeObject hdr; jmethodID mid; } FakeReflectedMethod;
typedef struct { FakeObject hdr; jfieldID  fid; } FakeReflectedField;

static FakeClass g_class_ReflectMethod = {
  {NULL}, "java/lang/reflect/Method", NULL, NULL, 0, NULL, 0
};
static FakeClass g_class_ReflectField = {
  {NULL}, "java/lang/reflect/Field", NULL, NULL, 0, NULL, 0
};

static jobject JNICALL f_ToReflectedMethod(JNIEnv *env, jclass cls,
                                           jmethodID mid, jboolean isStatic) {
  (void)env; (void)cls; (void)isStatic;
  FakeReflectedMethod *m = calloc(1, sizeof(FakeReflectedMethod));
  if (!m) return NULL;
  m->hdr.cls = &g_class_ReflectMethod;
  m->mid = mid;
  return jniref_new(&m->hdr, REF_LOCAL);
}

static jmethodID JNICALL f_FromReflectedMethod(JNIEnv *env, jobject method) {
  (void)env;
  FakeObject *o = jniref_deref(method);
  if (!o || o->cls != &g_class_ReflectMethod) {
    debug_log("[jni] FromReflectedMethod on something we did not create\n");
    return NULL;
  }
  return ((FakeReflectedMethod *)o)->mid;
}

static jobject JNICALL f_ToReflectedField(JNIEnv *env, jclass cls,
                                          jfieldID fid, jboolean isStatic) {
  (void)env; (void)cls; (void)isStatic;
  FakeReflectedField *f = calloc(1, sizeof(FakeReflectedField));
  if (!f) return NULL;
  f->hdr.cls = &g_class_ReflectField;
  f->fid = fid;
  return jniref_new(&f->hdr, REF_LOCAL);
}

static jfieldID JNICALL f_FromReflectedField(JNIEnv *env, jobject field) {
  (void)env;
  FakeObject *o = jniref_deref(field);
  if (!o || o->cls != &g_class_ReflectField) {
    debug_log("[jni] FromReflectedField on something we did not create\n");
    return NULL;
  }
  return ((FakeReflectedField *)o)->fid;
}

/* Defining a class from bytecode is genuinely impossible here -- there is no
 * verifier and no interpreter. Returning NULL with a pending exception is the
 * honest answer, and callers treat it as a load failure they can report. */
static jclass JNICALL f_DefineClass(JNIEnv *env, const char *name, jobject loader,
                                    const jbyte *buf, jsize len) {
  (void)env; (void)loader; (void)buf;
  debug_log("[jni] DefineClass(%s, %d bytes) -- cannot define classes from "
            "bytecode; no interpreter exists here\n", name ? name : "?", (int)len);
  raise_pending("java/lang/UnsupportedOperationException",
                name ? name : "DefineClass");
  return NULL;
}

/* -- the rest of the surface ----------------------------------------------
 *
 * Implemented pre-emptively rather than waiting to be named. Each of these is
 * plausibly the next one reached, and every miss costs a full build-and-test
 * cycle on hardware. */

/* Java.Interop takes monitors around peer bookkeeping. A single recursive
 * mutex is coarser than per-object monitors but is correct -- it can only
 * over-serialise, never under-serialise. Per-object locking would matter for
 * throughput, not correctness, and nothing here is contended enough to care. */
static RMutex g_monitor;

static jint JNICALL f_MonitorEnter(JNIEnv *env, jobject o) {
  (void)env; (void)o; rmutexLock(&g_monitor); return JNI_OK;
}
static jint JNICALL f_MonitorExit(JNIEnv *env, jobject o) {
  (void)env; (void)o; rmutexUnlock(&g_monitor); return JNI_OK;
}

static jint JNICALL f_GetJavaVM(JNIEnv *env, JavaVM **vm) {
  (void)env;
  if (vm) *vm = jni_get_vm();
  return JNI_OK;
}

/* Reported once per class: the walk below runs on every peer creation, and the
 * same class showing up in a loop would bury everything else. */
static const char *g_walk_seen[64];
static int g_walk_nseen;

static int walk_note_once(const char *name) {
  if (!name) return 0;
  for (int i = 0; i < g_walk_nseen; i++)
    if (!strcmp(g_walk_seen[i], name)) return 0;
  if (g_walk_nseen < (int)(sizeof(g_walk_seen)/sizeof(g_walk_seen[0])))
    g_walk_seen[g_walk_nseen++] = name;
  return 1;
}

static jclass JNICALL f_GetSuperclass(JNIEnv *env, jclass clazz) {
  (void)env;
  FakeClass *c = (FakeClass *)jniref_deref((jobject)clazz);
  if (!c) return NULL;
  if (!c->super) {
    /* Java.Interop resolves a jobject to a managed peer by taking its class
     * name, looking it up, and walking to the superclass on a miss. Our
     * classes mostly have no super, so the walk ends here -- and when it ends
     * without a match the value manager returns NULL rather than raising
     * anything. That null then arrives in managed code as a parameter and
     * kills the process on the first instruction that dereferences it, at an
     * address with no relation to this one.
     *
     * So: a line here is not itself an error -- the lookup usually succeeded
     * on the first name and never asked. But if a MANAGED NULL CHECK follows,
     * the class named here is the argument that went missing. */
    if (walk_note_once(c->name))
      debug_log("[jni] superclass walk reached %s, which has no super -- if a "
                "MANAGED NULL CHECK follows, this is the type that could not "
                "be resolved to a peer\n", c->name);
    return NULL;
  }
  return (jclass)jniref_new(&c->super->hdr, REF_LOCAL);
}

static jboolean JNICALL f_IsAssignableFrom(JNIEnv *env, jclass sub, jclass sup) {
  (void)env;
  FakeClass *a = (FakeClass *)jniref_deref((jobject)sub);
  FakeClass *b = (FakeClass *)jniref_deref((jobject)sup);
  for (FakeClass *k = a; k; k = k->super) if (k == b) return JNI_TRUE;
  /* Identity up the super chain is the normal answer, but a name that was
   * auto-stubbed before the real class registered produces two distinct
   * FakeClass objects describing the same Java type. Same name is the same
   * class here -- there is only ever one loader. */
  if (a && b && a->name && b->name && !strcmp(a->name, b->name)) return JNI_TRUE;
  return JNI_FALSE;
}

static void JNICALL f_GetStringUTFRegion(JNIEnv *env, jstring str, jsize start,
                                         jsize len, char *buf) {
  (void)env;
  if (!buf) return;
  buf[0] = 0;
  FakeString *fs = (FakeString *)jniref_deref((jobject)str);
  if (!fs || !fs->utf) return;
  jsize n = (jsize)strlen(fs->utf);
  if (start < 0 || start > n) return;
  if (start + len > n) len = n - start;
  memcpy(buf, fs->utf + start, (size_t)len);
  buf[len] = 0;
}

/* Returns jint, not void -- JNI_OK on success. */
static jint JNICALL f_UnregisterNatives(JNIEnv *env, jclass clazz) {
  (void)env; (void)clazz;
  /* Nothing to undo: captured pointers stay valid for the process lifetime. */
  return JNI_OK;
}

static void JNICALL f_FatalError(JNIEnv *env, const char *msg) {
  (void)env;
  fatal_error("the runtime called FatalError: %s", msg ? msg : "(no message)");
}

/* Direct byte buffers. The managed side uses these to hand raw memory across
 * the boundary without copying, so the address and capacity have to survive
 * the round trip -- a stub returning null here would silently corrupt any
 * buffer-based interop. */
typedef struct { FakeObject hdr; void *addr; jlong capacity; } FakeDirectBuffer;
static FakeClass g_class_ByteBuffer = {
  {NULL}, "java/nio/ByteBuffer", NULL, NULL, 0, NULL, 0
};

static jobject JNICALL f_NewDirectByteBuffer(JNIEnv *env, void *address, jlong capacity) {
  (void)env;
  FakeDirectBuffer *b = calloc(1, sizeof(FakeDirectBuffer));
  if (!b) return NULL;
  b->hdr.cls  = &g_class_ByteBuffer;
  b->addr     = address;
  b->capacity = capacity;
  return jniref_new(&b->hdr, REF_LOCAL);
}
static void *JNICALL f_GetDirectBufferAddress(JNIEnv *env, jobject buf) {
  (void)env;
  FakeObject *o = jniref_deref(buf);
  if (!o || o->cls != &g_class_ByteBuffer) return NULL;
  return ((FakeDirectBuffer *)o)->addr;
}
static jlong JNICALL f_GetDirectBufferCapacity(JNIEnv *env, jobject buf) {
  (void)env;
  FakeObject *o = jniref_deref(buf);
  if (!o || o->cls != &g_class_ByteBuffer) return -1;
  return ((FakeDirectBuffer *)o)->capacity;
}

/* Nonvirtual calls resolve the same way here: our method tables have no
 * overriding, so the nonvirtual form and the ordinary one are identical. */
#define CALL_NONVIRTUAL(Name, Type, field)                                      \
  static Type JNICALL f_CallNonvirtual##Name##MethodA(JNIEnv *env, jobject o,   \
                          jclass c, jmethodID m, const jvalue *a) {             \
    (void)c; return call_method_ex(env, o, m, a, 0).field;                            \
  }                                                                             \
  static Type JNICALL f_CallNonvirtual##Name##MethodV(JNIEnv *env, jobject o,   \
                          jclass c, jmethodID m, va_list ap) {                  \
    (void)c;                                                                    \
    jvalue args[16]; memset(args, 0, sizeof(args));                             \
    FakeMethod *fm = (FakeMethod *)m;                                           \
    if (fm) va_to_jvalues(fm->sig, ap, args, 16);                               \
    return call_method_ex(env, o, m, args, 0).field;                            \
  }                                                                             \
  static Type JNICALL f_CallNonvirtual##Name##Method(JNIEnv *env, jobject o,    \
                          jclass c, jmethodID m, ...) {                         \
    va_list ap; va_start(ap, m);                                                \
    Type r = f_CallNonvirtual##Name##MethodV(env, o, c, m, ap);                 \
    va_end(ap);                                                                 \
    return r;                                                                   \
  }

CALL_NONVIRTUAL(Object,  jobject,  l)
CALL_NONVIRTUAL(Boolean, jboolean, z)
CALL_NONVIRTUAL(Byte,    jbyte,    b)
CALL_NONVIRTUAL(Char,    jchar,    c)
CALL_NONVIRTUAL(Short,   jshort,   s)
CALL_NONVIRTUAL(Int,     jint,     i)
CALL_NONVIRTUAL(Long,    jlong,    j)
CALL_NONVIRTUAL(Float,   jfloat,   f)
CALL_NONVIRTUAL(Double,  jdouble,  d)

static void JNICALL f_CallNonvirtualVoidMethodA(JNIEnv *env, jobject o, jclass c,
                                                jmethodID m, const jvalue *a) {
  (void)c; call_method(env, o, m, a);
}
static void JNICALL f_CallNonvirtualVoidMethodV(JNIEnv *env, jobject o, jclass c,
                                                jmethodID m, va_list ap) {
  (void)c;
  jvalue args[16]; memset(args, 0, sizeof(args));
  FakeMethod *fm = (FakeMethod *)m;
  if (fm) va_to_jvalues(fm->sig, ap, args, 16);
  call_method(env, o, m, args);
}
static void JNICALL f_CallNonvirtualVoidMethod(JNIEnv *env, jobject o, jclass c,
                                               jmethodID m, ...) {
  va_list ap; va_start(ap, m);
  f_CallNonvirtualVoidMethodV(env, o, c, m, ap);
  va_end(ap);
}

/* -- strings -------------------------------------------------------------- */

/* FakeString and g_class_String live in jni_arrays.{c,h} so that the UTF-16
 * accessors there and the UTF-8 ones here agree on the layout. */
static jstring JNICALL f_NewStringUTF(JNIEnv *env, const char *s) {
  (void)env;
  return s ? jni_make_string(s) : NULL;
}
/* The text of a jstring -- type-CHECKED, because the cast is unsafe.
 *
 * FakeString::utf and FakeClass::name are both at offset 16, so casting an
 * arbitrary object to FakeString and reading ->utf returns a CLASS NAME when
 * handed a jclass, complete with slashes. And for an editable, whose buffer is
 * inline at that offset rather than a pointer, it reads eight characters of
 * text AS a pointer and dereferences it.
 *
 * A CharSequence is accepted properly rather than punned: it is a legitimate
 * thing to pass here and the shim now has a real one. */
static const char *string_text(jobject o) {
  FakeObject *f = jniref_deref(o);
  if (!f || !f->cls) return NULL;
  if (f->cls == &g_class_String) return ((FakeString *)f)->utf;

  const char *ed = android_text_get(o);
  if (ed) return ed;

  static int told;
  if (told < 8) {
    told++;
    debug_log("[jni] GetStringUTFChars on a %s, which is not a String -- "
              "returning NULL rather than whatever sits at that offset\n",
              f->cls->name ? f->cls->name : "?");
  }
  return NULL;
}

static const char *JNICALL f_GetStringUTFChars(JNIEnv *env, jstring s, jboolean *copy) {
  (void)env;
  if (copy) *copy = JNI_FALSE;
  return string_text((jobject)s);
}
static void JNICALL f_ReleaseStringUTFChars(JNIEnv *env, jstring s, const char *c) {
  (void)env; (void)s; (void)c;   /* not a copy; nothing to free */
}
static jsize JNICALL f_GetStringUTFLength(JNIEnv *env, jstring s) {
  (void)env;
  const char *t = string_text((jobject)s);
  return t ? (jsize)strlen(t) : 0;
}

/* ------------------------------------------------------------------------ */
/* Vtable assembly + trap fill                                               */
/* ------------------------------------------------------------------------ */

static struct JNINativeInterface g_jni_vtable = {
  .GetVersion             = f_GetVersion,
  .FindClass              = f_FindClass,
  .GetMethodID            = f_GetMethodID,
  .GetStaticMethodID      = f_GetStaticMethodID,
  .RegisterNatives        = f_RegisterNatives,
  .NewGlobalRef           = f_NewGlobalRef,
  .NewWeakGlobalRef       = f_NewWeakGlobalRef,
  .NewLocalRef            = f_NewLocalRef,
  .DeleteGlobalRef        = f_DeleteGlobalRef,
  .DeleteWeakGlobalRef    = f_DeleteWeakGlobalRef,
  .DeleteLocalRef         = f_DeleteLocalRef,
  .GetObjectRefType       = f_GetObjectRefType,
  .IsSameObject           = f_IsSameObject,
  .PushLocalFrame         = f_PushLocalFrame,
  .PopLocalFrame          = f_PopLocalFrame,
  .EnsureLocalCapacity    = f_EnsureLocalCapacity,
  .GetObjectClass         = f_GetObjectClass,
  .IsInstanceOf           = f_IsInstanceOf,
  .ExceptionOccurred      = f_ExceptionOccurred,
  .ExceptionClear         = f_ExceptionClear,
  .ExceptionCheck         = f_ExceptionCheck,
  .ExceptionDescribe      = f_ExceptionDescribe,
  .ThrowNew               = f_ThrowNew,
  .Throw                  = f_Throw,
  .NewStringUTF           = f_NewStringUTF,
  .GetStringUTFChars      = f_GetStringUTFChars,
  .ReleaseStringUTFChars  = f_ReleaseStringUTFChars,
  .GetStringUTFLength     = f_GetStringUTFLength,

  .CallObjectMethod   = f_CallObjectMethod,   .CallObjectMethodV   = f_CallObjectMethodV,   .CallObjectMethodA   = f_CallObjectMethodA,
  .CallBooleanMethod  = f_CallBooleanMethod,  .CallBooleanMethodV  = f_CallBooleanMethodV,  .CallBooleanMethodA  = f_CallBooleanMethodA,
  .CallByteMethod     = f_CallByteMethod,     .CallByteMethodV     = f_CallByteMethodV,     .CallByteMethodA     = f_CallByteMethodA,
  .CallCharMethod     = f_CallCharMethod,     .CallCharMethodV     = f_CallCharMethodV,     .CallCharMethodA     = f_CallCharMethodA,
  .CallShortMethod    = f_CallShortMethod,    .CallShortMethodV    = f_CallShortMethodV,    .CallShortMethodA    = f_CallShortMethodA,
  .CallIntMethod      = f_CallIntMethod,      .CallIntMethodV      = f_CallIntMethodV,      .CallIntMethodA      = f_CallIntMethodA,
  .CallLongMethod     = f_CallLongMethod,     .CallLongMethodV     = f_CallLongMethodV,     .CallLongMethodA     = f_CallLongMethodA,
  .CallFloatMethod    = f_CallFloatMethod,    .CallFloatMethodV    = f_CallFloatMethodV,    .CallFloatMethodA    = f_CallFloatMethodA,
  .CallDoubleMethod   = f_CallDoubleMethod,   .CallDoubleMethodV   = f_CallDoubleMethodV,   .CallDoubleMethodA   = f_CallDoubleMethodA,
  .CallVoidMethod     = f_CallVoidMethod,     .CallVoidMethodV     = f_CallVoidMethodV,     .CallVoidMethodA     = f_CallVoidMethodA,

  .CallStaticObjectMethod  = f_CallStaticObjectMethod,  .CallStaticObjectMethodV  = f_CallStaticObjectMethodV,  .CallStaticObjectMethodA  = f_CallStaticObjectMethodA,
  .CallStaticBooleanMethod = f_CallStaticBooleanMethod, .CallStaticBooleanMethodV = f_CallStaticBooleanMethodV, .CallStaticBooleanMethodA = f_CallStaticBooleanMethodA,
  .CallStaticByteMethod    = f_CallStaticByteMethod,    .CallStaticByteMethodV    = f_CallStaticByteMethodV,    .CallStaticByteMethodA    = f_CallStaticByteMethodA,
  .CallStaticCharMethod    = f_CallStaticCharMethod,    .CallStaticCharMethodV    = f_CallStaticCharMethodV,    .CallStaticCharMethodA    = f_CallStaticCharMethodA,
  .CallStaticShortMethod   = f_CallStaticShortMethod,   .CallStaticShortMethodV   = f_CallStaticShortMethodV,   .CallStaticShortMethodA   = f_CallStaticShortMethodA,
  .CallStaticIntMethod     = f_CallStaticIntMethod,     .CallStaticIntMethodV     = f_CallStaticIntMethodV,     .CallStaticIntMethodA     = f_CallStaticIntMethodA,
  .CallStaticLongMethod    = f_CallStaticLongMethod,    .CallStaticLongMethodV    = f_CallStaticLongMethodV,    .CallStaticLongMethodA    = f_CallStaticLongMethodA,
  .CallStaticFloatMethod   = f_CallStaticFloatMethod,   .CallStaticFloatMethodV   = f_CallStaticFloatMethodV,   .CallStaticFloatMethodA   = f_CallStaticFloatMethodA,
  .CallStaticDoubleMethod  = f_CallStaticDoubleMethod,  .CallStaticDoubleMethodV  = f_CallStaticDoubleMethodV,  .CallStaticDoubleMethodA  = f_CallStaticDoubleMethodA,
  .CallStaticVoidMethod    = f_CallStaticVoidMethod,    .CallStaticVoidMethodV    = f_CallStaticVoidMethodV,    .CallStaticVoidMethodA    = f_CallStaticVoidMethodA,

  .DefineClass          = f_DefineClass,
  .ToReflectedMethod    = f_ToReflectedMethod,
  .FromReflectedMethod  = f_FromReflectedMethod,
  .ToReflectedField     = f_ToReflectedField,
  .FromReflectedField   = f_FromReflectedField,
  .MonitorEnter = f_MonitorEnter,   .MonitorExit = f_MonitorExit,
  .GetJavaVM    = f_GetJavaVM,
  .GetSuperclass = f_GetSuperclass, .IsAssignableFrom = f_IsAssignableFrom,
  .GetStringUTFRegion = f_GetStringUTFRegion,
  .UnregisterNatives  = f_UnregisterNatives,
  .FatalError         = f_FatalError,
  .NewDirectByteBuffer     = f_NewDirectByteBuffer,
  .GetDirectBufferAddress  = f_GetDirectBufferAddress,
  .GetDirectBufferCapacity = f_GetDirectBufferCapacity,
  .CallNonvirtualObjectMethod = f_CallNonvirtualObjectMethod,  .CallNonvirtualObjectMethodV = f_CallNonvirtualObjectMethodV,  .CallNonvirtualObjectMethodA = f_CallNonvirtualObjectMethodA,
  .CallNonvirtualBooleanMethod = f_CallNonvirtualBooleanMethod,  .CallNonvirtualBooleanMethodV = f_CallNonvirtualBooleanMethodV,  .CallNonvirtualBooleanMethodA = f_CallNonvirtualBooleanMethodA,
  .CallNonvirtualByteMethod = f_CallNonvirtualByteMethod,  .CallNonvirtualByteMethodV = f_CallNonvirtualByteMethodV,  .CallNonvirtualByteMethodA = f_CallNonvirtualByteMethodA,
  .CallNonvirtualCharMethod = f_CallNonvirtualCharMethod,  .CallNonvirtualCharMethodV = f_CallNonvirtualCharMethodV,  .CallNonvirtualCharMethodA = f_CallNonvirtualCharMethodA,
  .CallNonvirtualShortMethod = f_CallNonvirtualShortMethod,  .CallNonvirtualShortMethodV = f_CallNonvirtualShortMethodV,  .CallNonvirtualShortMethodA = f_CallNonvirtualShortMethodA,
  .CallNonvirtualIntMethod = f_CallNonvirtualIntMethod,  .CallNonvirtualIntMethodV = f_CallNonvirtualIntMethodV,  .CallNonvirtualIntMethodA = f_CallNonvirtualIntMethodA,
  .CallNonvirtualLongMethod = f_CallNonvirtualLongMethod,  .CallNonvirtualLongMethodV = f_CallNonvirtualLongMethodV,  .CallNonvirtualLongMethodA = f_CallNonvirtualLongMethodA,
  .CallNonvirtualFloatMethod = f_CallNonvirtualFloatMethod,  .CallNonvirtualFloatMethodV = f_CallNonvirtualFloatMethodV,  .CallNonvirtualFloatMethodA = f_CallNonvirtualFloatMethodA,
  .CallNonvirtualDoubleMethod = f_CallNonvirtualDoubleMethod,  .CallNonvirtualDoubleMethodV = f_CallNonvirtualDoubleMethodV,  .CallNonvirtualDoubleMethodA = f_CallNonvirtualDoubleMethodA,
  .CallNonvirtualVoidMethod = f_CallNonvirtualVoidMethod,  .CallNonvirtualVoidMethodV = f_CallNonvirtualVoidMethodV,  .CallNonvirtualVoidMethodA = f_CallNonvirtualVoidMethodA,

  .AllocObject = f_AllocObject,
  .NewObject   = f_NewObject,
  .NewObjectV  = f_NewObjectV,
  .NewObjectA  = f_NewObjectA,

  /* arrays */
  .GetArrayLength              = f_GetArrayLength,
  .NewObjectArray              = f_NewObjectArray,
  .GetObjectArrayElement       = f_GetObjectArrayElement,
  .SetObjectArrayElement       = f_SetObjectArrayElement,
  .GetPrimitiveArrayCritical   = f_GetPrimitiveArrayCritical,
  .ReleasePrimitiveArrayCritical = f_ReleasePrimitiveArrayCritical,

  .NewBooleanArray = f_NewBooleanArray, .GetBooleanArrayElements = f_GetBooleanArrayElements,
  .ReleaseBooleanArrayElements = f_ReleaseBooleanArrayElements,
  .GetBooleanArrayRegion = f_GetBooleanArrayRegion, .SetBooleanArrayRegion = f_SetBooleanArrayRegion,
  .NewByteArray = f_NewByteArray, .GetByteArrayElements = f_GetByteArrayElements,
  .ReleaseByteArrayElements = f_ReleaseByteArrayElements,
  .GetByteArrayRegion = f_GetByteArrayRegion, .SetByteArrayRegion = f_SetByteArrayRegion,
  .NewCharArray = f_NewCharArray, .GetCharArrayElements = f_GetCharArrayElements,
  .ReleaseCharArrayElements = f_ReleaseCharArrayElements,
  .GetCharArrayRegion = f_GetCharArrayRegion, .SetCharArrayRegion = f_SetCharArrayRegion,
  .NewShortArray = f_NewShortArray, .GetShortArrayElements = f_GetShortArrayElements,
  .ReleaseShortArrayElements = f_ReleaseShortArrayElements,
  .GetShortArrayRegion = f_GetShortArrayRegion, .SetShortArrayRegion = f_SetShortArrayRegion,
  .NewIntArray = f_NewIntArray, .GetIntArrayElements = f_GetIntArrayElements,
  .ReleaseIntArrayElements = f_ReleaseIntArrayElements,
  .GetIntArrayRegion = f_GetIntArrayRegion, .SetIntArrayRegion = f_SetIntArrayRegion,
  .NewLongArray = f_NewLongArray, .GetLongArrayElements = f_GetLongArrayElements,
  .ReleaseLongArrayElements = f_ReleaseLongArrayElements,
  .GetLongArrayRegion = f_GetLongArrayRegion, .SetLongArrayRegion = f_SetLongArrayRegion,
  .NewFloatArray = f_NewFloatArray, .GetFloatArrayElements = f_GetFloatArrayElements,
  .ReleaseFloatArrayElements = f_ReleaseFloatArrayElements,
  .GetFloatArrayRegion = f_GetFloatArrayRegion, .SetFloatArrayRegion = f_SetFloatArrayRegion,
  .NewDoubleArray = f_NewDoubleArray, .GetDoubleArrayElements = f_GetDoubleArrayElements,
  .ReleaseDoubleArrayElements = f_ReleaseDoubleArrayElements,
  .GetDoubleArrayRegion = f_GetDoubleArrayRegion, .SetDoubleArrayRegion = f_SetDoubleArrayRegion,

  /* fields */
  .GetFieldID = f_GetFieldID, .GetStaticFieldID = f_GetStaticFieldID,
  .GetObjectField  = f_GetObjectField,  .SetObjectField  = f_SetObjectField,
  .GetBooleanField = f_GetBooleanField, .SetBooleanField = f_SetBooleanField,
  .GetByteField    = f_GetByteField,    .SetByteField    = f_SetByteField,
  .GetCharField    = f_GetCharField,    .SetCharField    = f_SetCharField,
  .GetShortField   = f_GetShortField,   .SetShortField   = f_SetShortField,
  .GetIntField     = f_GetIntField,     .SetIntField     = f_SetIntField,
  .GetLongField    = f_GetLongField,    .SetLongField    = f_SetLongField,
  .GetFloatField   = f_GetFloatField,   .SetFloatField   = f_SetFloatField,
  .GetDoubleField  = f_GetDoubleField,  .SetDoubleField  = f_SetDoubleField,
  .GetStaticObjectField  = f_GetStaticObjectField,  .SetStaticObjectField  = f_SetStaticObjectField,
  .GetStaticBooleanField = f_GetStaticBooleanField, .SetStaticBooleanField = f_SetStaticBooleanField,
  .GetStaticByteField    = f_GetStaticByteField,    .SetStaticByteField    = f_SetStaticByteField,
  .GetStaticCharField    = f_GetStaticCharField,    .SetStaticCharField    = f_SetStaticCharField,
  .GetStaticShortField   = f_GetStaticShortField,   .SetStaticShortField   = f_SetStaticShortField,
  .GetStaticIntField     = f_GetStaticIntField,     .SetStaticIntField     = f_SetStaticIntField,
  .GetStaticLongField    = f_GetStaticLongField,    .SetStaticLongField    = f_SetStaticLongField,
  .GetStaticFloatField   = f_GetStaticFloatField,   .SetStaticFloatField   = f_SetStaticFloatField,
  .GetStaticDoubleField  = f_GetStaticDoubleField,  .SetStaticDoubleField  = f_SetStaticDoubleField,

  /* UTF-16 strings */
  .GetStringLength      = f_GetStringLength,
  .GetStringChars       = f_GetStringChars,
  .ReleaseStringChars   = f_ReleaseStringChars,
  .NewString            = f_NewString,
  .GetStringRegion      = f_GetStringRegion,
  .GetStringCritical    = f_GetStringCritical,
  .ReleaseStringCritical = f_ReleaseStringCritical,
};

static JNIEnv g_env = &g_jni_vtable;

/* ---- per-slot traps -----------------------------------------------------
 *
 * The single shared trap could only report "approx index -1", because nothing
 * ever told it which slot had been called. Each slot now gets its own
 * trampoline that knows its index, and the index maps to a name.
 *
 * The ordering below is the JNINativeInterface layout. It is not guesswork:
 * indices 33, 144 and 145 were each confirmed against the game's own
 * disassembly (it calls vtable+0x108, +0x480 and +0x488 for GetMethodID,
 * GetStaticFieldID and GetStaticObjectField), and the total matches the slot
 * count this file reports at startup. */
static const char *const g_slot_names[] = {
  "reserved0",
  "reserved1",
  "reserved2",
  "reserved3",
  "GetVersion",
  "DefineClass",
  "FindClass",
  "FromReflectedMethod",
  "FromReflectedField",
  "ToReflectedMethod",
  "GetSuperclass",
  "IsAssignableFrom",
  "ToReflectedField",
  "Throw",
  "ThrowNew",
  "ExceptionOccurred",
  "ExceptionDescribe",
  "ExceptionClear",
  "FatalError",
  "PushLocalFrame",
  "PopLocalFrame",
  "NewGlobalRef",
  "DeleteGlobalRef",
  "DeleteLocalRef",
  "IsSameObject",
  "NewLocalRef",
  "EnsureLocalCapacity",
  "AllocObject",
  "NewObject",
  "NewObjectV",
  "NewObjectA",
  "GetObjectClass",
  "IsInstanceOf",
  "GetMethodID",
  "CallObjectMethod",
  "CallObjectMethodV",
  "CallObjectMethodA",
  "CallBooleanMethod",
  "CallBooleanMethodV",
  "CallBooleanMethodA",
  "CallByteMethod",
  "CallByteMethodV",
  "CallByteMethodA",
  "CallCharMethod",
  "CallCharMethodV",
  "CallCharMethodA",
  "CallShortMethod",
  "CallShortMethodV",
  "CallShortMethodA",
  "CallIntMethod",
  "CallIntMethodV",
  "CallIntMethodA",
  "CallLongMethod",
  "CallLongMethodV",
  "CallLongMethodA",
  "CallFloatMethod",
  "CallFloatMethodV",
  "CallFloatMethodA",
  "CallDoubleMethod",
  "CallDoubleMethodV",
  "CallDoubleMethodA",
  "CallVoidMethod",
  "CallVoidMethodV",
  "CallVoidMethodA",
  "CallNonvirtualObjectMethod",
  "CallNonvirtualObjectMethodV",
  "CallNonvirtualObjectMethodA",
  "CallNonvirtualBooleanMethod",
  "CallNonvirtualBooleanMethodV",
  "CallNonvirtualBooleanMethodA",
  "CallNonvirtualByteMethod",
  "CallNonvirtualByteMethodV",
  "CallNonvirtualByteMethodA",
  "CallNonvirtualCharMethod",
  "CallNonvirtualCharMethodV",
  "CallNonvirtualCharMethodA",
  "CallNonvirtualShortMethod",
  "CallNonvirtualShortMethodV",
  "CallNonvirtualShortMethodA",
  "CallNonvirtualIntMethod",
  "CallNonvirtualIntMethodV",
  "CallNonvirtualIntMethodA",
  "CallNonvirtualLongMethod",
  "CallNonvirtualLongMethodV",
  "CallNonvirtualLongMethodA",
  "CallNonvirtualFloatMethod",
  "CallNonvirtualFloatMethodV",
  "CallNonvirtualFloatMethodA",
  "CallNonvirtualDoubleMethod",
  "CallNonvirtualDoubleMethodV",
  "CallNonvirtualDoubleMethodA",
  "CallNonvirtualVoidMethod",
  "CallNonvirtualVoidMethodV",
  "CallNonvirtualVoidMethodA",
  "GetFieldID",
  "GetObjectField",
  "GetBooleanField",
  "GetByteField",
  "GetCharField",
  "GetShortField",
  "GetIntField",
  "GetLongField",
  "GetFloatField",
  "GetDoubleField",
  "SetObjectField",
  "SetBooleanField",
  "SetByteField",
  "SetCharField",
  "SetShortField",
  "SetIntField",
  "SetLongField",
  "SetFloatField",
  "SetDoubleField",
  "GetStaticMethodID",
  "CallStaticObjectMethod",
  "CallStaticObjectMethodV",
  "CallStaticObjectMethodA",
  "CallStaticBooleanMethod",
  "CallStaticBooleanMethodV",
  "CallStaticBooleanMethodA",
  "CallStaticByteMethod",
  "CallStaticByteMethodV",
  "CallStaticByteMethodA",
  "CallStaticCharMethod",
  "CallStaticCharMethodV",
  "CallStaticCharMethodA",
  "CallStaticShortMethod",
  "CallStaticShortMethodV",
  "CallStaticShortMethodA",
  "CallStaticIntMethod",
  "CallStaticIntMethodV",
  "CallStaticIntMethodA",
  "CallStaticLongMethod",
  "CallStaticLongMethodV",
  "CallStaticLongMethodA",
  "CallStaticFloatMethod",
  "CallStaticFloatMethodV",
  "CallStaticFloatMethodA",
  "CallStaticDoubleMethod",
  "CallStaticDoubleMethodV",
  "CallStaticDoubleMethodA",
  "CallStaticVoidMethod",
  "CallStaticVoidMethodV",
  "CallStaticVoidMethodA",
  "GetStaticFieldID",
  "GetStaticObjectField",
  "GetStaticBooleanField",
  "GetStaticByteField",
  "GetStaticCharField",
  "GetStaticShortField",
  "GetStaticIntField",
  "GetStaticLongField",
  "GetStaticFloatField",
  "GetStaticDoubleField",
  "SetStaticObjectField",
  "SetStaticBooleanField",
  "SetStaticByteField",
  "SetStaticCharField",
  "SetStaticShortField",
  "SetStaticIntField",
  "SetStaticLongField",
  "SetStaticFloatField",
  "SetStaticDoubleField",
  "NewString",
  "GetStringLength",
  "GetStringChars",
  "ReleaseStringChars",
  "NewStringUTF",
  "GetStringUTFLength",
  "GetStringUTFChars",
  "ReleaseStringUTFChars",
  "GetArrayLength",
  "NewObjectArray",
  "GetObjectArrayElement",
  "SetObjectArrayElement",
  "NewBooleanArray",
  "NewByteArray",
  "NewCharArray",
  "NewShortArray",
  "NewIntArray",
  "NewLongArray",
  "NewFloatArray",
  "NewDoubleArray",
  "GetBooleanArrayElements",
  "GetByteArrayElements",
  "GetCharArrayElements",
  "GetShortArrayElements",
  "GetIntArrayElements",
  "GetLongArrayElements",
  "GetFloatArrayElements",
  "GetDoubleArrayElements",
  "ReleaseBooleanArrayElements",
  "ReleaseByteArrayElements",
  "ReleaseCharArrayElements",
  "ReleaseShortArrayElements",
  "ReleaseIntArrayElements",
  "ReleaseLongArrayElements",
  "ReleaseFloatArrayElements",
  "ReleaseDoubleArrayElements",
  "GetBooleanArrayRegion",
  "GetByteArrayRegion",
  "GetCharArrayRegion",
  "GetShortArrayRegion",
  "GetIntArrayRegion",
  "GetLongArrayRegion",
  "GetFloatArrayRegion",
  "GetDoubleArrayRegion",
  "SetBooleanArrayRegion",
  "SetByteArrayRegion",
  "SetCharArrayRegion",
  "SetShortArrayRegion",
  "SetIntArrayRegion",
  "SetLongArrayRegion",
  "SetFloatArrayRegion",
  "SetDoubleArrayRegion",
  "RegisterNatives",
  "UnregisterNatives",
  "MonitorEnter",
  "MonitorExit",
  "GetJavaVM",
  "GetStringRegion",
  "GetStringUTFRegion",
  "GetPrimitiveArrayCritical",
  "ReleasePrimitiveArrayCritical",
  "GetStringCritical",
  "ReleaseStringCritical",
  "NewWeakGlobalRef",
  "DeleteWeakGlobalRef",
  "ExceptionCheck",
  "NewDirectByteBuffer",
  "GetDirectBufferAddress",
  "GetDirectBufferCapacity",
  "GetObjectRefType"
};
#define N_SLOT_NAMES (sizeof(g_slot_names)/sizeof(g_slot_names[0]))

static long jni_slot_trap(int idx);

static long jt0(void) { return jni_slot_trap(0); }
static long jt1(void) { return jni_slot_trap(1); }
static long jt2(void) { return jni_slot_trap(2); }
static long jt3(void) { return jni_slot_trap(3); }
static long jt4(void) { return jni_slot_trap(4); }
static long jt5(void) { return jni_slot_trap(5); }
static long jt6(void) { return jni_slot_trap(6); }
static long jt7(void) { return jni_slot_trap(7); }
static long jt8(void) { return jni_slot_trap(8); }
static long jt9(void) { return jni_slot_trap(9); }
static long jt10(void) { return jni_slot_trap(10); }
static long jt11(void) { return jni_slot_trap(11); }
static long jt12(void) { return jni_slot_trap(12); }
static long jt13(void) { return jni_slot_trap(13); }
static long jt14(void) { return jni_slot_trap(14); }
static long jt15(void) { return jni_slot_trap(15); }
static long jt16(void) { return jni_slot_trap(16); }
static long jt17(void) { return jni_slot_trap(17); }
static long jt18(void) { return jni_slot_trap(18); }
static long jt19(void) { return jni_slot_trap(19); }
static long jt20(void) { return jni_slot_trap(20); }
static long jt21(void) { return jni_slot_trap(21); }
static long jt22(void) { return jni_slot_trap(22); }
static long jt23(void) { return jni_slot_trap(23); }
static long jt24(void) { return jni_slot_trap(24); }
static long jt25(void) { return jni_slot_trap(25); }
static long jt26(void) { return jni_slot_trap(26); }
static long jt27(void) { return jni_slot_trap(27); }
static long jt28(void) { return jni_slot_trap(28); }
static long jt29(void) { return jni_slot_trap(29); }
static long jt30(void) { return jni_slot_trap(30); }
static long jt31(void) { return jni_slot_trap(31); }
static long jt32(void) { return jni_slot_trap(32); }
static long jt33(void) { return jni_slot_trap(33); }
static long jt34(void) { return jni_slot_trap(34); }
static long jt35(void) { return jni_slot_trap(35); }
static long jt36(void) { return jni_slot_trap(36); }
static long jt37(void) { return jni_slot_trap(37); }
static long jt38(void) { return jni_slot_trap(38); }
static long jt39(void) { return jni_slot_trap(39); }
static long jt40(void) { return jni_slot_trap(40); }
static long jt41(void) { return jni_slot_trap(41); }
static long jt42(void) { return jni_slot_trap(42); }
static long jt43(void) { return jni_slot_trap(43); }
static long jt44(void) { return jni_slot_trap(44); }
static long jt45(void) { return jni_slot_trap(45); }
static long jt46(void) { return jni_slot_trap(46); }
static long jt47(void) { return jni_slot_trap(47); }
static long jt48(void) { return jni_slot_trap(48); }
static long jt49(void) { return jni_slot_trap(49); }
static long jt50(void) { return jni_slot_trap(50); }
static long jt51(void) { return jni_slot_trap(51); }
static long jt52(void) { return jni_slot_trap(52); }
static long jt53(void) { return jni_slot_trap(53); }
static long jt54(void) { return jni_slot_trap(54); }
static long jt55(void) { return jni_slot_trap(55); }
static long jt56(void) { return jni_slot_trap(56); }
static long jt57(void) { return jni_slot_trap(57); }
static long jt58(void) { return jni_slot_trap(58); }
static long jt59(void) { return jni_slot_trap(59); }
static long jt60(void) { return jni_slot_trap(60); }
static long jt61(void) { return jni_slot_trap(61); }
static long jt62(void) { return jni_slot_trap(62); }
static long jt63(void) { return jni_slot_trap(63); }
static long jt64(void) { return jni_slot_trap(64); }
static long jt65(void) { return jni_slot_trap(65); }
static long jt66(void) { return jni_slot_trap(66); }
static long jt67(void) { return jni_slot_trap(67); }
static long jt68(void) { return jni_slot_trap(68); }
static long jt69(void) { return jni_slot_trap(69); }
static long jt70(void) { return jni_slot_trap(70); }
static long jt71(void) { return jni_slot_trap(71); }
static long jt72(void) { return jni_slot_trap(72); }
static long jt73(void) { return jni_slot_trap(73); }
static long jt74(void) { return jni_slot_trap(74); }
static long jt75(void) { return jni_slot_trap(75); }
static long jt76(void) { return jni_slot_trap(76); }
static long jt77(void) { return jni_slot_trap(77); }
static long jt78(void) { return jni_slot_trap(78); }
static long jt79(void) { return jni_slot_trap(79); }
static long jt80(void) { return jni_slot_trap(80); }
static long jt81(void) { return jni_slot_trap(81); }
static long jt82(void) { return jni_slot_trap(82); }
static long jt83(void) { return jni_slot_trap(83); }
static long jt84(void) { return jni_slot_trap(84); }
static long jt85(void) { return jni_slot_trap(85); }
static long jt86(void) { return jni_slot_trap(86); }
static long jt87(void) { return jni_slot_trap(87); }
static long jt88(void) { return jni_slot_trap(88); }
static long jt89(void) { return jni_slot_trap(89); }
static long jt90(void) { return jni_slot_trap(90); }
static long jt91(void) { return jni_slot_trap(91); }
static long jt92(void) { return jni_slot_trap(92); }
static long jt93(void) { return jni_slot_trap(93); }
static long jt94(void) { return jni_slot_trap(94); }
static long jt95(void) { return jni_slot_trap(95); }
static long jt96(void) { return jni_slot_trap(96); }
static long jt97(void) { return jni_slot_trap(97); }
static long jt98(void) { return jni_slot_trap(98); }
static long jt99(void) { return jni_slot_trap(99); }
static long jt100(void) { return jni_slot_trap(100); }
static long jt101(void) { return jni_slot_trap(101); }
static long jt102(void) { return jni_slot_trap(102); }
static long jt103(void) { return jni_slot_trap(103); }
static long jt104(void) { return jni_slot_trap(104); }
static long jt105(void) { return jni_slot_trap(105); }
static long jt106(void) { return jni_slot_trap(106); }
static long jt107(void) { return jni_slot_trap(107); }
static long jt108(void) { return jni_slot_trap(108); }
static long jt109(void) { return jni_slot_trap(109); }
static long jt110(void) { return jni_slot_trap(110); }
static long jt111(void) { return jni_slot_trap(111); }
static long jt112(void) { return jni_slot_trap(112); }
static long jt113(void) { return jni_slot_trap(113); }
static long jt114(void) { return jni_slot_trap(114); }
static long jt115(void) { return jni_slot_trap(115); }
static long jt116(void) { return jni_slot_trap(116); }
static long jt117(void) { return jni_slot_trap(117); }
static long jt118(void) { return jni_slot_trap(118); }
static long jt119(void) { return jni_slot_trap(119); }
static long jt120(void) { return jni_slot_trap(120); }
static long jt121(void) { return jni_slot_trap(121); }
static long jt122(void) { return jni_slot_trap(122); }
static long jt123(void) { return jni_slot_trap(123); }
static long jt124(void) { return jni_slot_trap(124); }
static long jt125(void) { return jni_slot_trap(125); }
static long jt126(void) { return jni_slot_trap(126); }
static long jt127(void) { return jni_slot_trap(127); }
static long jt128(void) { return jni_slot_trap(128); }
static long jt129(void) { return jni_slot_trap(129); }
static long jt130(void) { return jni_slot_trap(130); }
static long jt131(void) { return jni_slot_trap(131); }
static long jt132(void) { return jni_slot_trap(132); }
static long jt133(void) { return jni_slot_trap(133); }
static long jt134(void) { return jni_slot_trap(134); }
static long jt135(void) { return jni_slot_trap(135); }
static long jt136(void) { return jni_slot_trap(136); }
static long jt137(void) { return jni_slot_trap(137); }
static long jt138(void) { return jni_slot_trap(138); }
static long jt139(void) { return jni_slot_trap(139); }
static long jt140(void) { return jni_slot_trap(140); }
static long jt141(void) { return jni_slot_trap(141); }
static long jt142(void) { return jni_slot_trap(142); }
static long jt143(void) { return jni_slot_trap(143); }
static long jt144(void) { return jni_slot_trap(144); }
static long jt145(void) { return jni_slot_trap(145); }
static long jt146(void) { return jni_slot_trap(146); }
static long jt147(void) { return jni_slot_trap(147); }
static long jt148(void) { return jni_slot_trap(148); }
static long jt149(void) { return jni_slot_trap(149); }
static long jt150(void) { return jni_slot_trap(150); }
static long jt151(void) { return jni_slot_trap(151); }
static long jt152(void) { return jni_slot_trap(152); }
static long jt153(void) { return jni_slot_trap(153); }
static long jt154(void) { return jni_slot_trap(154); }
static long jt155(void) { return jni_slot_trap(155); }
static long jt156(void) { return jni_slot_trap(156); }
static long jt157(void) { return jni_slot_trap(157); }
static long jt158(void) { return jni_slot_trap(158); }
static long jt159(void) { return jni_slot_trap(159); }
static long jt160(void) { return jni_slot_trap(160); }
static long jt161(void) { return jni_slot_trap(161); }
static long jt162(void) { return jni_slot_trap(162); }
static long jt163(void) { return jni_slot_trap(163); }
static long jt164(void) { return jni_slot_trap(164); }
static long jt165(void) { return jni_slot_trap(165); }
static long jt166(void) { return jni_slot_trap(166); }
static long jt167(void) { return jni_slot_trap(167); }
static long jt168(void) { return jni_slot_trap(168); }
static long jt169(void) { return jni_slot_trap(169); }
static long jt170(void) { return jni_slot_trap(170); }
static long jt171(void) { return jni_slot_trap(171); }
static long jt172(void) { return jni_slot_trap(172); }
static long jt173(void) { return jni_slot_trap(173); }
static long jt174(void) { return jni_slot_trap(174); }
static long jt175(void) { return jni_slot_trap(175); }
static long jt176(void) { return jni_slot_trap(176); }
static long jt177(void) { return jni_slot_trap(177); }
static long jt178(void) { return jni_slot_trap(178); }
static long jt179(void) { return jni_slot_trap(179); }
static long jt180(void) { return jni_slot_trap(180); }
static long jt181(void) { return jni_slot_trap(181); }
static long jt182(void) { return jni_slot_trap(182); }
static long jt183(void) { return jni_slot_trap(183); }
static long jt184(void) { return jni_slot_trap(184); }
static long jt185(void) { return jni_slot_trap(185); }
static long jt186(void) { return jni_slot_trap(186); }
static long jt187(void) { return jni_slot_trap(187); }
static long jt188(void) { return jni_slot_trap(188); }
static long jt189(void) { return jni_slot_trap(189); }
static long jt190(void) { return jni_slot_trap(190); }
static long jt191(void) { return jni_slot_trap(191); }
static long jt192(void) { return jni_slot_trap(192); }
static long jt193(void) { return jni_slot_trap(193); }
static long jt194(void) { return jni_slot_trap(194); }
static long jt195(void) { return jni_slot_trap(195); }
static long jt196(void) { return jni_slot_trap(196); }
static long jt197(void) { return jni_slot_trap(197); }
static long jt198(void) { return jni_slot_trap(198); }
static long jt199(void) { return jni_slot_trap(199); }
static long jt200(void) { return jni_slot_trap(200); }
static long jt201(void) { return jni_slot_trap(201); }
static long jt202(void) { return jni_slot_trap(202); }
static long jt203(void) { return jni_slot_trap(203); }
static long jt204(void) { return jni_slot_trap(204); }
static long jt205(void) { return jni_slot_trap(205); }
static long jt206(void) { return jni_slot_trap(206); }
static long jt207(void) { return jni_slot_trap(207); }
static long jt208(void) { return jni_slot_trap(208); }
static long jt209(void) { return jni_slot_trap(209); }
static long jt210(void) { return jni_slot_trap(210); }
static long jt211(void) { return jni_slot_trap(211); }
static long jt212(void) { return jni_slot_trap(212); }
static long jt213(void) { return jni_slot_trap(213); }
static long jt214(void) { return jni_slot_trap(214); }
static long jt215(void) { return jni_slot_trap(215); }
static long jt216(void) { return jni_slot_trap(216); }
static long jt217(void) { return jni_slot_trap(217); }
static long jt218(void) { return jni_slot_trap(218); }
static long jt219(void) { return jni_slot_trap(219); }
static long jt220(void) { return jni_slot_trap(220); }
static long jt221(void) { return jni_slot_trap(221); }
static long jt222(void) { return jni_slot_trap(222); }
static long jt223(void) { return jni_slot_trap(223); }
static long jt224(void) { return jni_slot_trap(224); }
static long jt225(void) { return jni_slot_trap(225); }
static long jt226(void) { return jni_slot_trap(226); }
static long jt227(void) { return jni_slot_trap(227); }
static long jt228(void) { return jni_slot_trap(228); }
static long jt229(void) { return jni_slot_trap(229); }
static long jt230(void) { return jni_slot_trap(230); }
static long jt231(void) { return jni_slot_trap(231); }
static long jt232(void) { return jni_slot_trap(232); }
static long jt233(void) { return jni_slot_trap(233); }
static long jt234(void) { return jni_slot_trap(234); }
static long jt235(void) { return jni_slot_trap(235); }
static long jt236(void) { return jni_slot_trap(236); }
static long jt237(void) { return jni_slot_trap(237); }
static long jt238(void) { return jni_slot_trap(238); }
static long jt239(void) { return jni_slot_trap(239); }
static long jt240(void) { return jni_slot_trap(240); }
static long jt241(void) { return jni_slot_trap(241); }
static long jt242(void) { return jni_slot_trap(242); }
static long jt243(void) { return jni_slot_trap(243); }
static long jt244(void) { return jni_slot_trap(244); }
static long jt245(void) { return jni_slot_trap(245); }
static long jt246(void) { return jni_slot_trap(246); }
static long jt247(void) { return jni_slot_trap(247); }

static long (*const g_slot_traps[248])(void) = {
  jt0,
  jt1,
  jt2,
  jt3,
  jt4,
  jt5,
  jt6,
  jt7,
  jt8,
  jt9,
  jt10,
  jt11,
  jt12,
  jt13,
  jt14,
  jt15,
  jt16,
  jt17,
  jt18,
  jt19,
  jt20,
  jt21,
  jt22,
  jt23,
  jt24,
  jt25,
  jt26,
  jt27,
  jt28,
  jt29,
  jt30,
  jt31,
  jt32,
  jt33,
  jt34,
  jt35,
  jt36,
  jt37,
  jt38,
  jt39,
  jt40,
  jt41,
  jt42,
  jt43,
  jt44,
  jt45,
  jt46,
  jt47,
  jt48,
  jt49,
  jt50,
  jt51,
  jt52,
  jt53,
  jt54,
  jt55,
  jt56,
  jt57,
  jt58,
  jt59,
  jt60,
  jt61,
  jt62,
  jt63,
  jt64,
  jt65,
  jt66,
  jt67,
  jt68,
  jt69,
  jt70,
  jt71,
  jt72,
  jt73,
  jt74,
  jt75,
  jt76,
  jt77,
  jt78,
  jt79,
  jt80,
  jt81,
  jt82,
  jt83,
  jt84,
  jt85,
  jt86,
  jt87,
  jt88,
  jt89,
  jt90,
  jt91,
  jt92,
  jt93,
  jt94,
  jt95,
  jt96,
  jt97,
  jt98,
  jt99,
  jt100,
  jt101,
  jt102,
  jt103,
  jt104,
  jt105,
  jt106,
  jt107,
  jt108,
  jt109,
  jt110,
  jt111,
  jt112,
  jt113,
  jt114,
  jt115,
  jt116,
  jt117,
  jt118,
  jt119,
  jt120,
  jt121,
  jt122,
  jt123,
  jt124,
  jt125,
  jt126,
  jt127,
  jt128,
  jt129,
  jt130,
  jt131,
  jt132,
  jt133,
  jt134,
  jt135,
  jt136,
  jt137,
  jt138,
  jt139,
  jt140,
  jt141,
  jt142,
  jt143,
  jt144,
  jt145,
  jt146,
  jt147,
  jt148,
  jt149,
  jt150,
  jt151,
  jt152,
  jt153,
  jt154,
  jt155,
  jt156,
  jt157,
  jt158,
  jt159,
  jt160,
  jt161,
  jt162,
  jt163,
  jt164,
  jt165,
  jt166,
  jt167,
  jt168,
  jt169,
  jt170,
  jt171,
  jt172,
  jt173,
  jt174,
  jt175,
  jt176,
  jt177,
  jt178,
  jt179,
  jt180,
  jt181,
  jt182,
  jt183,
  jt184,
  jt185,
  jt186,
  jt187,
  jt188,
  jt189,
  jt190,
  jt191,
  jt192,
  jt193,
  jt194,
  jt195,
  jt196,
  jt197,
  jt198,
  jt199,
  jt200,
  jt201,
  jt202,
  jt203,
  jt204,
  jt205,
  jt206,
  jt207,
  jt208,
  jt209,
  jt210,
  jt211,
  jt212,
  jt213,
  jt214,
  jt215,
  jt216,
  jt217,
  jt218,
  jt219,
  jt220,
  jt221,
  jt222,
  jt223,
  jt224,
  jt225,
  jt226,
  jt227,
  jt228,
  jt229,
  jt230,
  jt231,
  jt232,
  jt233,
  jt234,
  jt235,
  jt236,
  jt237,
  jt238,
  jt239,
  jt240,
  jt241,
  jt242,
  jt243,
  jt244,
  jt245,
  jt246,
  jt247
};

static long jni_slot_trap(int idx) {
  const char *name = (idx >= 0 && (size_t)idx < N_SLOT_NAMES)
                   ? g_slot_names[idx] : "<beyond the known layout>";
  fatal_error("unimplemented JNI function called: %s (slot %d, vtable+0x%x).\n"
              "Implement it in jni_fake.c and add it to the vtable.",
              name, idx, idx * 8);
  return 0;
}



/* -- JavaVM --------------------------------------------------------------- */

static jint JNICALL f_GetEnv(JavaVM *vm, void **out, jint version) {
  (void)vm; (void)version;
  *out = &g_env;
  return JNI_OK;
}
static jint JNICALL f_AttachCurrentThread(JavaVM *vm, JNIEnv **out, void *args) {
  (void)vm; (void)args;
  *out = &g_env;   /* single shared env: our JNIEnv holds no per-thread state */
  return JNI_OK;
}
static jint JNICALL f_DetachCurrentThread(JavaVM *vm) { (void)vm; return JNI_OK; }
static jint JNICALL f_DestroyJavaVM(JavaVM *vm)       { (void)vm; return JNI_OK; }

static struct JNIInvokeInterface g_vm_vtable = {
  .GetEnv                     = f_GetEnv,
  .AttachCurrentThread        = f_AttachCurrentThread,
  .AttachCurrentThreadAsDaemon= f_AttachCurrentThread,
  .DetachCurrentThread        = f_DetachCurrentThread,
  .DestroyJavaVM              = f_DestroyJavaVM,
};

static JavaVM g_vm = &g_vm_vtable;

/* ------------------------------------------------------------------------ */

JNIEnv  *jni_get_env(void) { return &g_env; }
JavaVM  *jni_get_vm(void)  { return &g_vm;  }

void jni_raise(JNIEnv *env, const char *cls, const char *detail) {
  (void)env;
  raise_pending(cls, detail);
}

void jni_fake_init(void) {
  jniref_init();

  /* Fill every NULL slot with the trap. The struct is nothing but function
   * pointers, so walking it as an array is safe here. */
  void **slots = (void **)&g_jni_vtable;
  size_t n = sizeof(g_jni_vtable) / sizeof(void *);
  int filled = 0;
  for (size_t i = 0; i < n; i++) {
    if (!slots[i]) {
      slots[i] = (i < sizeof(g_slot_traps)/sizeof(g_slot_traps[0]))
               ? (void *)g_slot_traps[i] : NULL;
      filled++;
    }
  }

  void **vmslots = (void **)&g_vm_vtable;
  size_t vn = sizeof(g_vm_vtable) / sizeof(void *);
  for (size_t i = 0; i < vn; i++)
    if (!vmslots[i]) vmslots[i] = (void *)g_slot_traps[0];

  jni_register_class(&g_class_Class);
  jni_register_class(&g_class_String);
  jni_register_class(&g_class_Throwable);
  jni_register_class(&g_class_ByteBuffer);
  jni_register_class(&g_class_ReflectMethod);
  jni_register_class(&g_class_ReflectField);
  rmutexInit(&g_monitor);

  debug_log("[jni] env ready: %zu slots, %d trapped as unimplemented\n", n, filled);
}
