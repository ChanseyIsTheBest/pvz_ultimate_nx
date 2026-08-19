/* interop_classes.c -- the Java.Interop bootstrap surface.
 *
 * These are the classes the roadmap calls Tier 1 and Tier 2: everything the
 * managed runtime touches between JNI_OnLoad and the first RegisterNatives.
 * Nothing renders, nothing plays, nothing is loaded until these exist.
 *
 * They split into two kinds, and the distinction decides how much work each
 * one is:
 *
 *   NATIVE on the Java side -- ManagedPeer.construct,
 *   ManagedPeer.registerNativeMembers, JavaInteropRuntime.init,
 *   ApplicationRegistration.registerApplications. The game image supplies the
 *   implementation and registers it during JNI_OnLoad. We only have to DECLARE
 *   them, with signatures that match, so RegisterNatives has somewhere to
 *   attach and jni_invoke_native can call back in. Getting a signature wrong
 *   here means the call is dispatched with mismatched argument registers,
 *   which crashes somewhere unrelated.
 *
 *   IMPLEMENTED on the Java side -- MonoPackageManager's accessors. There is
 *   no Java, so we implement them in C.
 *
 * Signatures are taken from the game's own classes.dex. Cross-check any you
 * change against dotnet/java-interop rather than guessing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_classes.h"
#include "interop_classes.h"
#include "jni_arrays.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "util.h"

/* Returned to managed code, so it must parse as a Unix path -- no device
 * prefix. Nothing actually resolves libraries through it: dlopen matches on
 * the basename, which is why the runtime's probing worked at all. */
#define NATIVE_LIB_PATH "/data/data/com.pvz.ultimate/lib"

/* ------------------------------------------------------------------------ */
/* net/dot/jni/ManagedPeer                                                   */
/* ------------------------------------------------------------------------ */

/* Both are `public static native` in java-interop. Declared only -- the image
 * fills in native_fn. registerNativeMembers is the single most important entry
 * in this file: it is how every n_* pointer in lawn_natives.c arrives. */
static FakeMethod g_managedpeer_methods[] = {
  { NULL, "<init>", "()V", NULL, 0 },
  /* Three parameters, not four. The earlier declaration invented a
   * java/lang/Class argument; jni_invoke_native unpacks by parsing this
   * string, so the extra entry would have shifted every argument register
   * when the managed side called it. Taken from classes.dex. */
  { NULL, "construct",
    "(Ljava/lang/Object;Ljava/lang/String;[Ljava/lang/Object;)V",
    NULL, 1 },
  { NULL, "registerNativeMembers",
    "(Ljava/lang/Class;Ljava/lang/String;)V", NULL, 1 },
};

static FakeClass g_class_ManagedPeer = {
  {NULL}, "net/dot/jni/ManagedPeer", NULL,
  g_managedpeer_methods,
  (int)(sizeof(g_managedpeer_methods)/sizeof(g_managedpeer_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* net/dot/jni/nativeaot/JavaInteropRuntime                                  */
/* ------------------------------------------------------------------------ */

/* init is native and exported from the image as
 * Java_net_dot_jni_nativeaot_JavaInteropRuntime_init, so main.c can call it
 * directly by symbol. It is declared here as well because the managed side may
 * reach it through JNI rather than through the export. */
static FakeMethod g_jiruntime_methods[] = {
  { NULL, "<init>", "()V", NULL, 0 },
  { NULL, "init",
    "(Ljava/lang/ClassLoader;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
    NULL, 1 },
};

static FakeClass g_class_JavaInteropRuntime = {
  {NULL}, "net/dot/jni/nativeaot/JavaInteropRuntime", NULL,
  g_jiruntime_methods,
  (int)(sizeof(g_jiruntime_methods)/sizeof(g_jiruntime_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* net/dot/jni/nativeaot/NativeAotEnvironmentVars                            */
/* ------------------------------------------------------------------------ */

/* On Android this reads a generated table and pushes each entry through
 * android.system.Os.setenv before the runtime starts. Our configuration is
 * fixed in runtime_glue.c and served from getenv_fake, so this is a no-op --
 * but it must EXIST, because the bootstrap calls it unconditionally. */
static jvalue naev_Initialize(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  debug_log("[interop] NativeAotEnvironmentVars.Initialize -- configuration "
            "comes from runtime_glue.c instead\n");
  return r;
}

static FakeMethod g_naev_methods[] = {
  { naev_Initialize, "<init>", "()V", NULL, 0 },
  { naev_Initialize, "Initialize", "()V", NULL, 1 },
};

static FakeClass g_class_NativeAotEnvironmentVars = {
  {NULL}, "net/dot/jni/nativeaot/NativeAotEnvironmentVars", NULL,
  g_naev_methods, (int)(sizeof(g_naev_methods)/sizeof(g_naev_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* net/dot/android/ApplicationRegistration                                   */
/* ------------------------------------------------------------------------ */

static FakeMethod g_appreg_methods[] = {
  { NULL, "registerApplications", "()V", NULL, 1 },   /* native */
};

static FakeClass g_class_ApplicationRegistration = {
  {NULL}, "net/dot/android/ApplicationRegistration", NULL,
  g_appreg_methods, (int)(sizeof(g_appreg_methods)/sizeof(g_appreg_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* mono/MonoPackageManager                                                   */
/* ------------------------------------------------------------------------ */

static jobject g_saved_context;

static jvalue mpm_setContext(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  jvalue r; memset(&r, 0, sizeof(r));
  if (a) g_saved_context = a[0].l;
  debug_log("[interop] MonoPackageManager.setContext\n");
  return r;
}

/* Under NativeAOT every assembly is compiled into the image, so there is
 * nothing to enumerate. Returning an EMPTY array rather than null matters: the
 * bootstrap iterates the result and a null dereferences. */
static jvalue mpm_getAssemblies(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jni_make_object_array(0, NULL);
  return r;
}

static jvalue mpm_getNativeLibraryPath(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = (*e)->NewStringUTF(e, NATIVE_LIB_PATH);
  return r;
}

static jvalue mpm_isEmulator(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.z = JNI_FALSE;
  return r;
}

static jvalue mpm_LoadApplication(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  debug_log("[interop] MonoPackageManager.LoadApplication\n");
  return r;
}

static FakeMethod g_mpm_methods[] = {
  { mpm_LoadApplication, "<init>", "()V", NULL, 0 },
  { mpm_setContext,          "setContext",          "(Landroid/content/Context;)V", NULL, 1 },
  { mpm_getAssemblies,       "getAssemblies",       "()[Ljava/lang/String;",        NULL, 1 },
  { mpm_getAssemblies,       "getDependencies",     "()[Ljava/lang/String;",        NULL, 1 },
  { mpm_getNativeLibraryPath,"getNativeLibraryPath","(Landroid/content/Context;)Ljava/lang/String;", NULL, 1 },
  /* Second overload, also present in the dex. */
  { mpm_getNativeLibraryPath,"getNativeLibraryPath","(Landroid/content/pm/ApplicationInfo;)Ljava/lang/String;", NULL, 1 },
  { mpm_isEmulator,          "isEmulator",          "()Z",                          NULL, 1 },
  { mpm_LoadApplication,     "LoadApplication",     "(Landroid/content/Context;)V", NULL, 1 },
};

static FakeClass g_class_MonoPackageManager = {
  {NULL}, "mono/MonoPackageManager", NULL,
  g_mpm_methods, (int)(sizeof(g_mpm_methods)/sizeof(g_mpm_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* GC peer tracking (Tier 2)                                                 */
/* ------------------------------------------------------------------------ */

/* On Android these maintain a Java-side strong reference from a peer object to
 * the managed objects it must keep alive, so ART's GC and .NET's GC agree
 * about liveness across the boundary.
 *
 * There is no Java heap here and nothing on our side ever collects, so the
 * conservative answer is to accept the calls and do nothing: we simply never
 * free, which cannot cause a use-after-free. It CAN leak, and if the port ever
 * reaches the point of running for hours, this is a place to look. Counting
 * the calls makes that measurable rather than speculative.
 */
static unsigned g_peer_adds, g_peer_clears;

static jvalue peer_add(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  g_peer_adds++;
  return r;
}
static jvalue peer_clear(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  g_peer_clears++;
  return r;
}

static FakeMethod g_peer_methods[] = {
  /* The GC bridge fetches this class and immediately constructs one, so the
   * constructor has to be findable. It carries no state, so a no-op body is
   * the whole implementation. */
  { peer_add,   "<init>",                   "()V",                   NULL, 0 },
  { peer_add,   "jiAddManagedReference",    "(Ljava/lang/Object;)V", NULL, 0 },
  { peer_clear, "jiClearManagedReferences", "()V",                   NULL, 0 },
  { peer_add,   "monodroidAddReference",    "(Ljava/lang/Object;)V", NULL, 0 },
  { peer_clear, "monodroidClearReferences", "()V",                   NULL, 0 },
};

#define PEER_CLASS(varname, javaname)                                          \
  static FakeClass varname = {                                                 \
    {NULL}, javaname, NULL,                                                    \
    g_peer_methods,                                                            \
    (int)(sizeof(g_peer_methods)/sizeof(g_peer_methods[0])), NULL, 0           \
  }

PEER_CLASS(g_class_GCUserPeerable, "net/dot/jni/GCUserPeerable");
PEER_CLASS(g_class_GCUserPeer,     "mono/android/GCUserPeer");
PEER_CLASS(g_class_IGCUserPeer,    "mono/android/IGCUserPeer");
PEER_CLASS(g_class_JavaProxyObject,"net/dot/jni/internal/JavaProxyObject");

void interop_peer_stats(unsigned *adds, unsigned *clears) {
  if (adds)   *adds   = g_peer_adds;
  if (clears) *clears = g_peer_clears;
}

/* ------------------------------------------------------------------------ */
/* mono/android/Runtime -- legacy MonoVM path                                */
/* ------------------------------------------------------------------------ */

/* Present in classes.dex but should never be reached under NativeAOT. If one
 * of these fires it means the bootstrap took the MonoVM branch, which is a
 * different and much larger problem than a missing method -- so say so loudly
 * rather than quietly returning. */
static jvalue mono_runtime_trap(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  debug_log("[interop] *** mono/android/Runtime called ***\n"
            "[interop] The bootstrap took the MonoVM path, not NativeAOT. "
            "Check that JavaInteropRuntime.init ran first.\n");
  return r;
}

static FakeMethod g_monort_methods[] = {
  { mono_runtime_trap, "<init>", "()V", NULL, 0 },
  { mono_runtime_trap, "createNewContextWithData",
      "([Ljava/lang/String;[Ljava/lang/String;[[B[Ljava/lang/String;Ljava/lang/ClassLoader;Z)I",
      NULL, 1 },
  { mono_runtime_trap, "init",         "(Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/ClassLoader;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;I[Ljava/lang/String;)V", NULL, 1 },
  { mono_runtime_trap, "register",     "(Ljava/lang/String;Ljava/lang/Class;Ljava/lang/String;)V", NULL, 1 },
  { mono_runtime_trap, "createNewContext",
      "([Ljava/lang/String;[Ljava/lang/String;Ljava/lang/ClassLoader;)I", NULL, 1 },
  { mono_runtime_trap, "switchToContext",  "(I)V",   NULL, 1 },
  { mono_runtime_trap, "destroyContexts",  "([I)V",  NULL, 1 },
  { mono_runtime_trap, "propagateUncaughtException",
      "(Ljava/lang/Thread;Ljava/lang/Throwable;)V", NULL, 1 },
  { mono_runtime_trap, "notifyTimeZoneChanged", "()V", NULL, 1 },
  { mono_runtime_trap, "dumpTimingData",         "()V", NULL, 1 },
  { mono_runtime_trap, "initInternal", "(Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;ILjava/lang/ClassLoader;[Ljava/lang/String;ZZ)V", NULL, 1 },
};

/* The GC bridge reads this static field during runtime init and aborts if it
 * comes back null. On Android the DEX assigns it; we do not run the DEX, so it
 * has to be supplied here.
 *
 * A real field with an accessor rather than the generic stub path: the stub
 * resolves the name to a class by convention, which worked, but left the value
 * one indirection away from anything we can observe. This logs each read, so a
 * failure after this point is definitely not "the field was empty". */
static jobject g_ref_gcuserpeer_class;
static jobject g_ref_igcuserpeer_class;

static jvalue rt_field_gcuserpeer(JNIEnv *e, jobject self) {
  (void)e; (void)self;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = g_ref_gcuserpeer_class;
  debug_log("[interop] Runtime.mono_android_GCUserPeer read -> %p\n", r.l);
  return r;
}

static jvalue rt_field_igcuserpeer(JNIEnv *e, jobject self) {
  (void)e; (void)self;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = g_ref_igcuserpeer_class;
  debug_log("[interop] Runtime.mono_android_IGCUserPeer read -> %p\n", r.l);
  return r;
}

static FakeField g_monort_fields[] = {
  { "mono_android_GCUserPeer",  "Ljava/lang/Class;", rt_field_gcuserpeer,  NULL, {0} },
  { "mono_android_IGCUserPeer", "Ljava/lang/Class;", rt_field_igcuserpeer, NULL, {0} },
};

static FakeClass g_class_MonoRuntime = {
  {NULL}, "mono/android/Runtime", NULL,
  g_monort_methods, (int)(sizeof(g_monort_methods)/sizeof(g_monort_methods[0])),
  g_monort_fields,  (int)(sizeof(g_monort_fields)/sizeof(g_monort_fields[0]))
};

/* ------------------------------------------------------------------------ */

/* Ask Java.Interop to build the managed peer for a Java instance.
 *
 * This closes the gap that left every game native unbound. registerNativeMembers
 * binds a type's natives when the managed side first constructs a peer of that
 * type -- and on Android the framework constructs the Activity, a step that
 * simply does not exist here. So nothing was ever registered, and the report
 * said so plainly: 0 of 19.
 *
 * ManagedPeer.construct is the entry point the framework would have reached,
 * and it is a real function pointer now that thunk allocation works.
 * Java.Interop maps the Java class name to the managed type through its type
 * map, so the instance's class -- which matches the name in classes.dex -- is
 * what selects the managed Activity.
 *
 * Failure is reported rather than fatal: the peer may already exist, or the
 * type map may not carry this name, and the binding report a few lines later
 * is a better place to judge the outcome than an abort that hides it. */
int interop_construct_peer(JNIEnv *env, jobject instance, const char *ctor_sig,
                           jobject *args, int nargs) {
  if (!instance) return -1;

  FakeClass *mp = jni_find_class("net/dot/jni/ManagedPeer");
  if (!mp) { debug_log("[interop] ManagedPeer is not registered\n"); return -1; }

  jclass mpc = (jclass)jniref_new(&mp->hdr, REF_LOCAL);
  jmethodID mid = (*env)->GetStaticMethodID(
      env, mpc, "construct",
      "(Ljava/lang/Object;Ljava/lang/String;[Ljava/lang/Object;)V");
  if (!mid) { debug_log("[interop] ManagedPeer.construct not found\n"); return -1; }

  FakeMethod *m = (FakeMethod *)mid;
  if (!m->native_fn) {
    debug_log("[interop] ManagedPeer.construct has no implementation yet -- "
              "the runtime has not finished registering it\n");
    return -1;
  }

  /* A FULL JNI method signature: "()V", "(Landroid/content/Context;)V".
   *
   * Established from the runtime's own complaint, after I had changed this to
   * a bare parameter list on the strength of a half-remembered convention:
   *
   *     Member signature `` is not a method signature.
   *     Method signatures must start with `(`.
   *       at JniMemberSignature.GetParameterCountFromMethodSignature
   *       at ManagedPeer.GetConstructorCandidateParameterTypes
   *
   * The original form was right. It had appeared to fail only because the
   * exception explaining why was being discarded by a stubbed
   * JavaProxyThrowable constructor, so a working call and a failing one looked
   * identical.
   *
   * The argument array must still match the signature's parameter list. */
  jstring sig = (*env)->NewStringUTF(env, ctor_sig ? ctor_sig : "()V");

  jobject params = jni_make_object_array(nargs, NULL);
  for (int i = 0; i < nargs; i++)
    (*env)->SetObjectArrayElement(env, (jobjectArray)params, i, args[i]);

  FakeObject *o = jniref_deref(instance);
  debug_log("[interop] constructing the managed peer for %s, signature %s, "
            "%d arg(s)\n",
            (o && o->cls) ? o->cls->name : "?", ctor_sig ? ctor_sig : "()V", nargs);

  (*env)->CallStaticVoidMethod(env, mpc, mid, instance, sig, params);

  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return -1;
  }
  return 0;
}

void interop_classes_init(void) {
  jni_register_class(&g_class_ManagedPeer);
  jni_register_class(&g_class_JavaInteropRuntime);
  jni_register_class(&g_class_NativeAotEnvironmentVars);
  jni_register_class(&g_class_ApplicationRegistration);
  jni_register_class(&g_class_MonoPackageManager);
  jni_register_class(&g_class_GCUserPeerable);
  jni_register_class(&g_class_GCUserPeer);
  jni_register_class(&g_class_IGCUserPeer);
  jni_register_class(&g_class_JavaProxyObject);
  jni_register_class(&g_class_MonoRuntime);

  /* Global refs to the peer CLASSES themselves, which is what the bridge
   * stores. Created after registration so the class objects have their
   * metaclass pointer set. */
  g_ref_gcuserpeer_class  = jniref_new(&g_class_GCUserPeer.hdr,  REF_GLOBAL);
  g_ref_igcuserpeer_class = jniref_new(&g_class_IGCUserPeer.hdr, REF_GLOBAL);

  debug_log("[interop] bootstrap classes registered "
            "(GCUserPeer class ref %p)\n", g_ref_gcuserpeer_class);
}

/* Reports whether the image has supplied registerNativeMembers yet.
 *
 * Timing matters and cost a run to learn: JNI_OnLoad does NOT register it. The
 * managed runtime does, during JavaInteropRuntime.init. Calling this between
 * the two and treating the answer as fatal aborts a bootstrap that was working
 * -- JNI_OnLoad had returned JNI_VERSION_1_6. Informational before init,
 * meaningful after. */
int interop_check_bootstrap(void) {
  FakeMethod *rnm = NULL;
  for (int i = 0; i < g_class_ManagedPeer.nmethods; i++)
    if (!strcmp(g_class_ManagedPeer.methods[i].name, "registerNativeMembers"))
      rnm = &g_class_ManagedPeer.methods[i];

  if (!rnm || !rnm->native_fn) {
    debug_log("[interop] registerNativeMembers not registered yet "
              "(expected before JavaInteropRuntime.init)\n");
    return -1;
  }
  debug_log("[interop] ManagedPeer.registerNativeMembers -> %p\n", rnm->native_fn);
  return 0;
}
