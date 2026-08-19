/* android_runtime.c -- the rest of the Java surface, taken from classes.dex.
 *
 * Every class and signature here was read out of the game's own dex rather
 * than guessed, because guessing costs a build cycle each time and the log
 * only ever names one missing class per run.
 *
 * Almost all of it follows a single shape: a Java method that forwards to an
 * n_* native which the managed side registers during startup. read([BII)I
 * calls n_read([BII)I; run()V calls n_run()V. So the bodies here are thin --
 * find the registered native on this object's class and invoke it. What makes
 * the class matter is existing at all, since a failed FindClass aborts the
 * bootstrap while a method that does nothing usually does not.
 *
 * Two of these are worth more than the rest for debugging:
 *   android/util/Log            -- the game's own logging, into debug.log
 *   UncaughtExceptionMarshaler  -- unhandled managed exceptions, with message
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android_runtime.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "android_classes.h"
#include "lawn_natives.h"
#include "android_os.h"
#include "jni_ref.h"
#include "util.h"

#define JV jvalue r; memset(&r, 0, sizeof(r))

/* ------------------------------------------------------------------------ */
/* Forwarding to a registered native                                         */
/* ------------------------------------------------------------------------ */

/* The managed side attaches implementations to the n_* declarations through
 * RegisterNatives. Until it does, these are absent and the call is a no-op.
 *
 * That silence was wrong. An unbound native is indistinguishable from a method
 * that legitimately does nothing, and the difference matters enormously: a
 * dropped Runnable held up the entire boot for a round, producing no crash and
 * no log line. Any n_* that is declared but never bound gets named here, once,
 * the first time something actually calls it. If a class turns up in this list
 * it needs a descriptor in lawn_register.c. */
static const char *g_unbound_seen[64][2];
static int g_unbound_nseen;

static void note_unbound(const char *cls, const char *nname) {
  for (int i = 0; i < g_unbound_nseen; i++)
    if (g_unbound_seen[i][0] == cls && g_unbound_seen[i][1] == nname) return;
  if (g_unbound_nseen < 64) {
    g_unbound_seen[g_unbound_nseen][0] = cls;
    g_unbound_seen[g_unbound_nseen][1] = nname;
    g_unbound_nseen++;
  }
  debug_log("[reg] %s.%s is declared but NOT BOUND -- the call did nothing and "
            "returned zero. Nothing registered it, because its Java <clinit> "
            "never ran. Add a descriptor for %s to lawn_register.c.\n",
            cls, nname, cls);
}

static jvalue forward(JNIEnv *e, jobject self, const char *nname, const jvalue *a) {
  JV;
  FakeObject *o = jniref_deref(self);
  if (!o || !o->cls) return r;

  int declared = 0;
  for (FakeClass *k = o->cls; k; k = k->super)
    for (int i = 0; i < k->nmethods; i++)
      if (!strcmp(k->methods[i].name, nname)) {
        if (k->methods[i].native_fn)
          return jni_invoke_native(e, k->methods[i].native_fn, self,
                                   k->methods[i].sig, a);
        declared = 1;
      }

  if (declared) note_unbound(o->cls->name, nname);
  return r;
}

#define FWD(fn, nname)                                                        \
  static jvalue fn(JNIEnv *e, jobject self, const jvalue *a) {                \
    return forward(e, self, nname, a);                                        \
  }

static jvalue nop(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV; return r;
}

/* jiAddManagedReference / jiClearManagedReferences appear on nearly every one
 * of these. With no Java heap there is nothing to pin, so accepting and doing
 * nothing is both correct and safe -- it can leak, never dangle. */
#define PEER_METHODS                                                          \
  { nop, "jiAddManagedReference",    "(Ljava/lang/Object;)V", NULL, 0 },      \
  { nop, "jiClearManagedReferences", "()V",                   NULL, 0 },      \
  { nop, "monodroidAddReference",    "(Ljava/lang/Object;)V", NULL, 0 },      \
  { nop, "monodroidClearReferences", "()V",                   NULL, 0 }

/* ------------------------------------------------------------------------ */
/* android/util/Log -- the game's own logging                                */
/* ------------------------------------------------------------------------ */

static const char *str_of(jobject o) {
  FakeString *s = (FakeString *)jniref_deref(o);
  return (s && s->utf) ? s->utf : "";
}

static jvalue log_at(JNIEnv *e, jobject self, const jvalue *a, char level) {
  (void)e; (void)self;
  JV;
  if (a) debug_log("[game:%c] %s: %s\n", level, str_of(a[0].l), str_of(a[1].l));
  r.i = 0;
  return r;
}
static jvalue log_d(JNIEnv *e, jobject s, const jvalue *a) { return log_at(e,s,a,'D'); }
static jvalue log_v(JNIEnv *e, jobject s, const jvalue *a) { return log_at(e,s,a,'V'); }
static jvalue log_w(JNIEnv *e, jobject s, const jvalue *a) { return log_at(e,s,a,'W'); }
static jvalue log_i(JNIEnv *e, jobject s, const jvalue *a) { return log_at(e,s,a,'I'); }
static jvalue log_e(JNIEnv *e, jobject s, const jvalue *a) { return log_at(e,s,a,'E'); }

static FakeMethod g_log_methods[] = {
  { log_d, "d", "(Ljava/lang/String;Ljava/lang/String;)I", NULL, 1 },
  { log_v, "v", "(Ljava/lang/String;Ljava/lang/String;)I", NULL, 1 },
  { log_w, "w", "(Ljava/lang/String;Ljava/lang/String;)I", NULL, 1 },
  { log_i, "i", "(Ljava/lang/String;Ljava/lang/String;)I", NULL, 1 },
  /* The dex only has the 3-arg form. Keeping the 2-arg one costs nothing --
   * other callers use it -- but the throwable overload must be present or
   * error logging silently falls through to a stub. */
  { log_e, "e", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", NULL, 1 },
  { log_e, "e", "(Ljava/lang/String;Ljava/lang/String;)I", NULL, 1 },
};
static FakeClass g_class_Log = {
  {NULL}, "android/util/Log", NULL,
  g_log_methods, (int)(sizeof(g_log_methods)/sizeof(g_log_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* android/system/Os                                                         */
/* ------------------------------------------------------------------------ */

static jvalue os_setenv(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV;
  /* The configuration comes from runtime_glue's fixed table, so this is
   * recorded rather than honoured -- but silently dropping it would hide a
   * variable the runtime believes it set. */
  if (a) debug_log("[java] Os.setenv(%s=%s) noted; the table in "
                   "runtime_glue.c is authoritative\n",
                   str_of(a[0].l), str_of(a[1].l));
  return r;
}
static FakeMethod g_os_methods[] = {
  { os_setenv, "setenv", "(Ljava/lang/String;Ljava/lang/String;Z)V", NULL, 1 },
};
static FakeClass g_class_Os = {
  {NULL}, "android/system/Os", NULL,
  g_os_methods, (int)(sizeof(g_os_methods)/sizeof(g_os_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* Unhandled managed exceptions                                              */
/* ------------------------------------------------------------------------ */

/* This is where a managed exception nobody caught arrives. Printing it is the
 * difference between "the game stopped" and knowing which exception and where,
 * so it is worth the effort of reaching into the Throwable for a message. */
static jvalue uncaught(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self;
  JV;
  debug_log("\n*** UNHANDLED MANAGED EXCEPTION ***\n");

  if (a && a[1].l) {
    jobject thr = a[1].l;
    jclass cls = (*e)->GetObjectClass(e, thr);
    if (cls) {
      jmethodID mid = (*e)->GetMethodID(e, cls, "toString", "()Ljava/lang/String;");
      if (mid) {
        jobject s = (*e)->CallObjectMethod(e, thr, mid);
        if (s) debug_log("    %s\n", str_of(s));
      }
      /* GetMethodID raises NoSuchMethodError when it misses; clear it so the
       * miss does not look like a second, unrelated failure. */
      (*e)->ExceptionClear(e);
    }
  }

  /* Hand it on so the managed handler still runs and can print its own trace,
   * which is usually richer than anything reachable from here. */
  forward(e, self, "n_uncaughtException", a);
  return r;
}

static FakeMethod g_uncaught_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { nop, "<init>", "(Ljava/lang/Thread$UncaughtExceptionHandler;)V", NULL, 0 },
  { uncaught, "uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V", NULL, 0 },
  { NULL,     "n_uncaughtException","(Ljava/lang/Thread;Ljava/lang/Throwable;)V", NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_UncaughtMarshaler = {
  {NULL}, "crc64e4855764d7e4f582/UncaughtExceptionMarshaler", NULL,
  g_uncaught_methods, (int)(sizeof(g_uncaught_methods)/sizeof(g_uncaught_methods[0])), NULL, 0
};
static FakeClass g_class_XamarinUncaught = {
  {NULL}, "mono/android/XamarinUncaughtExceptionHandler", NULL,
  g_uncaught_methods, (int)(sizeof(g_uncaught_methods)/sizeof(g_uncaught_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* Java.Interop peer bases                                                   */
/* ------------------------------------------------------------------------ */

FWD(jo_equals,   "n_equals")
FWD(jo_hashCode, "n_hashCode")
FWD(jo_toString, "n_toString")

static FakeMethod g_javaobject_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { jo_equals,   "equals",     "(Ljava/lang/Object;)Z",  NULL, 0 },
  { jo_hashCode, "hashCode",   "()I",                    NULL, 0 },
  { jo_toString, "toString",   "()Ljava/lang/String;",   NULL, 0 },
  { NULL,        "n_equals",   "(Ljava/lang/Object;)Z",  NULL, 0 },
  { NULL,        "n_hashCode", "()I",                    NULL, 0 },
  { NULL,        "n_toString", "()Ljava/lang/String;",   NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_JavaObject = {
  {NULL}, "mono/android/runtime/JavaObject", NULL,
  g_javaobject_methods, (int)(sizeof(g_javaobject_methods)/sizeof(g_javaobject_methods[0])), NULL, 0
};

static FakeMethod g_peeronly_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  PEER_METHODS
};

#define PEER_ONLY(var, name)                                                  \
  static FakeClass var = { {NULL}, name, NULL, g_peeronly_methods,            \
    (int)(sizeof(g_peeronly_methods)/sizeof(g_peeronly_methods[0])), NULL, 0 }

/* JavaProxyThrowable carries the text of a managed exception across the
 * boundary, and its constructor is the only place that text appears.
 *
 * It was a stub, so every message was discarded on arrival -- which is why
 * three rounds of "pending exception:" printed nothing at all. The writers and
 * printStackTrace were implemented to recover it, but they read a field
 * nothing had ever filled in.
 *
 * The message is logged the moment it is constructed rather than waiting for
 * anyone to ask, because a failure that is reported and then swallowed by a
 * later stub is exactly the problem this is fixing. */
typedef struct { FakeObject hdr; char msg[768]; } FakeProxyThrowable;

static jvalue jpt_init(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV;
  FakeProxyThrowable *t = (FakeProxyThrowable *)jniref_deref(self);
  const char *text = a ? str_of(a[0].l) : "";
  if (t) snprintf(t->msg, sizeof(t->msg), "%s", text);

  debug_log("\n*** MANAGED EXCEPTION ***\n%s\n\n", (text && *text) ? text : "(empty)");
  return r;
}

static jvalue jpt_getMessage(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV;
  FakeProxyThrowable *t = (FakeProxyThrowable *)jniref_deref(self);
  r.l = jni_make_string(t ? t->msg : "");
  return r;
}

/* Writes into the supplied writer so the caller reading it back gets the text,
 * and logs it too in case the caller discards it. */
static jvalue jpt_printStackTrace(JNIEnv *e, jobject self, const jvalue *a) {
  JV;
  FakeProxyThrowable *t = (FakeProxyThrowable *)jniref_deref(self);
  const char *msg = t ? t->msg : "";

  if (a && a[0].l) {
    jclass wc = (*e)->GetObjectClass(e, a[0].l);
    if (wc) {
      jmethodID mid = (*e)->GetMethodID(e, wc, "println", "(Ljava/lang/String;)V");
      if (mid) (*e)->CallVoidMethod(e, a[0].l, mid, (*e)->NewStringUTF(e, msg));
      (*e)->ExceptionClear(e);
    }
  }
  debug_log("[jni] JavaProxyThrowable.printStackTrace: %s\n", *msg ? msg : "(empty)");
  return r;
}

static FakeMethod g_proxythrowable_methods[] = {
  { jpt_init,            "<init>",              "(Ljava/lang/String;)V",    NULL, 0 },
  { jpt_init,            "<init>",              "(Ljava/lang/String;Ljava/lang/Throwable;)V", NULL, 0 },
  { jpt_getMessage,      "getMessage",          "()Ljava/lang/String;",     NULL, 0 },
  { jpt_getMessage,      "getLocalizedMessage", "()Ljava/lang/String;",     NULL, 0 },
  { jpt_getMessage,      "toString",            "()Ljava/lang/String;",     NULL, 0 },
  { jpt_printStackTrace, "printStackTrace",     "(Ljava/io/PrintWriter;)V", NULL, 0 },
  { jpt_printStackTrace, "printStackTrace",     "(Ljava/io/PrintStream;)V", NULL, 0 },
  { jpt_printStackTrace, "printStackTrace",     "()V",                      NULL, 0 },
  PEER_METHODS,
};

#define PROXY_THROWABLE(var, name)                                            \
  static FakeClass var = { {NULL}, name, NULL, g_proxythrowable_methods,      \
    (int)(sizeof(g_proxythrowable_methods)/sizeof(g_proxythrowable_methods[0])), \
    NULL, 0, sizeof(FakeProxyThrowable) }

PROXY_THROWABLE(g_class_JavaProxyThrowable_dot, "net/dot/jni/internal/JavaProxyThrowable");
PROXY_THROWABLE(g_class_JavaProxyThrowable_and, "android/runtime/JavaProxyThrowable");

/* ------------------------------------------------------------------------ */
/* NativeAotRuntimeProvider -- the ContentProvider that starts the runtime   */
/* ------------------------------------------------------------------------ */

static jvalue narp_onCreate(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV;
  debug_log("[interop] NativeAotRuntimeProvider.onCreate -> true\n");
  r.z = JNI_TRUE;
  return r;
}
static jvalue narp_null(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV; return r;
}

static FakeMethod g_narp_methods[] = {
  { narp_null, "<init>", "()V", NULL, 0 },
  { narp_onCreate, "onCreate",   "()Z", NULL, 0 },
  { narp_null,     "attachInfo", "(Landroid/content/Context;Landroid/content/pm/ProviderInfo;)V", NULL, 0 },
  { narp_null,     "getType",    "(Landroid/net/Uri;)Ljava/lang/String;", NULL, 0 },
  { narp_null,     "insert",     "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;", NULL, 0 },
  { narp_null,     "delete",     "(Landroid/net/Uri;Ljava/lang/String;[Ljava/lang/String;)I", NULL, 0 },
  { narp_null,     "update",     "(Landroid/net/Uri;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I", NULL, 0 },
  { narp_null,     "query",      "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;", NULL, 0 },
};
static FakeClass g_class_NativeAotRuntimeProvider = {
  {NULL}, "net/dot/jni/nativeaot/NativeAotRuntimeProvider", NULL,
  g_narp_methods, (int)(sizeof(g_narp_methods)/sizeof(g_narp_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* Stream adapters -- how managed code reads assets and writes saves         */
/* ------------------------------------------------------------------------ */

FWD(isa_read0, "n_read")
FWD(isa_close, "n_close")

static FakeMethod g_isa_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { isa_read0, "read",    "()I",      NULL, 0 },
  { isa_read0, "read",    "([B)I",    NULL, 0 },
  { isa_read0, "read",    "([BII)I",  NULL, 0 },
  { isa_close, "close",   "()V",      NULL, 0 },
  { NULL,      "n_read",  "()I",      NULL, 0 },
  { NULL,      "n_read",  "([B)I",    NULL, 0 },
  { NULL,      "n_read",  "([BII)I",  NULL, 0 },
  { NULL,      "n_close", "()V",      NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_InputStreamAdapter = {
  {NULL}, "mono/android/runtime/InputStreamAdapter", NULL,
  g_isa_methods, (int)(sizeof(g_isa_methods)/sizeof(g_isa_methods[0])), NULL, 0
};

FWD(osa_write, "n_write")
FWD(osa_flush, "n_flush")
FWD(osa_close, "n_close")

static FakeMethod g_osa_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { osa_write, "write",   "(I)V",     NULL, 0 },
  { osa_write, "write",   "([B)V",    NULL, 0 },
  { osa_write, "write",   "([BII)V",  NULL, 0 },
  { osa_flush, "flush",   "()V",      NULL, 0 },
  { osa_close, "close",   "()V",      NULL, 0 },
  { NULL,      "n_write", "(I)V",     NULL, 0 },
  { NULL,      "n_write", "([B)V",    NULL, 0 },
  { NULL,      "n_write", "([BII)V",  NULL, 0 },
  { NULL,      "n_flush", "()V",      NULL, 0 },
  { NULL,      "n_close", "()V",      NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_OutputStreamAdapter = {
  {NULL}, "mono/android/runtime/OutputStreamAdapter", NULL,
  g_osa_methods, (int)(sizeof(g_osa_methods)/sizeof(g_osa_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* Callbacks and listeners                                                   */
/* ------------------------------------------------------------------------ */

FWD(cb_run,          "n_run")
FWD(cb_onBackInvoked,"n_onBackInvoked")
FWD(cb_onDismiss,    "n_onDismiss")
FWD(cb_onClick,      "n_onClick")

static FakeMethod g_runnable_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { cb_run, "run",   "()V", NULL, 0 },
  { NULL,   "n_run", "()V", NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_Runnable = {
  {NULL}, "mono/java/lang/RunnableImplementor", NULL,
  g_runnable_methods, (int)(sizeof(g_runnable_methods)/sizeof(g_runnable_methods[0])), NULL, 0
};

static FakeMethod g_backinvoked_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { nop, "<init>", "(Lcrc646ec5dadb3c3b2bda/AndroidNativeActivity;)V", NULL, 0 },
  { cb_onBackInvoked, "onBackInvoked",   "()V", NULL, 0 },
  { NULL,             "n_onBackInvoked", "()V", NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_BackInvoked = {
  {NULL}, "crc646ec5dadb3c3b2bda/AndroidNativeActivity_BackInvokedCallback", NULL,
  g_backinvoked_methods, (int)(sizeof(g_backinvoked_methods)/sizeof(g_backinvoked_methods[0])), NULL, 0
};
static FakeClass g_class_OnBackInvokedCallback = {
  {NULL}, "android/window/OnBackInvokedCallback", NULL,
  g_backinvoked_methods, (int)(sizeof(g_backinvoked_methods)/sizeof(g_backinvoked_methods[0])), NULL, 0
};

static FakeMethod g_dismiss_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { cb_onDismiss, "onDismiss",   "(Landroid/content/DialogInterface;)V", NULL, 0 },
  { NULL,         "n_onDismiss", "(Landroid/content/DialogInterface;)V", NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_DialogDismiss = {
  {NULL}, "crc646ec5dadb3c3b2bda/AndroidNativeActivity_DialogDismissListener", NULL,
  g_dismiss_methods, (int)(sizeof(g_dismiss_methods)/sizeof(g_dismiss_methods[0])), NULL, 0
};

/* Two different interfaces that happen to share a method name. Sharing one
 * table meant View's implementor advertised the DialogInterface signature,
 * so a lookup for the View form would have missed and been stubbed. */
static FakeMethod g_viewclick_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { cb_onClick, "onClick",   "(Landroid/view/View;)V", NULL, 0 },
  { NULL,       "n_onClick", "(Landroid/view/View;)V", NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_ViewOnClick = {
  {NULL}, "mono/android/view/View_OnClickListenerImplementor", NULL,
  g_viewclick_methods,
  (int)(sizeof(g_viewclick_methods)/sizeof(g_viewclick_methods[0])), NULL, 0
};

static FakeMethod g_dialogclick_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
  { cb_onClick, "onClick",   "(Landroid/content/DialogInterface;I)V", NULL, 0 },
  { NULL,       "n_onClick", "(Landroid/content/DialogInterface;I)V", NULL, 0 },
  PEER_METHODS,
};
static FakeClass g_class_DialogOnClick = {
  {NULL}, "mono/android/content/DialogInterface_OnClickListenerImplementor", NULL,
  g_dialogclick_methods,
  (int)(sizeof(g_dialogclick_methods)/sizeof(g_dialogclick_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* Framework bases, so inherited lookups resolve                             */
/* ------------------------------------------------------------------------ */

/* android/view/Window
 *
 * OnCreate asks for one and then configures it -- flags, decor view, keeping
 * the screen on. None of that means anything on a console with one fullscreen
 * surface, but the object has to exist: the result is dereferenced the moment
 * it comes back. */
/* LayoutParams and Intent: neither carries information we can supply, but both
 * are routinely dereferenced right after being fetched -- flags read off the
 * one, extras queried on the other. An empty object survives that; null does
 * not. Their own accessors returning null is fine, because a caller asking for
 * an absent extra expects null. */
static FakeObject g_lp_obj, g_intent_obj;
static FakeClass  g_class_LayoutParams, g_class_Intent;
static jobject    g_ref_lp, g_ref_intent;

static FakeMethod g_lp_methods[] = {
  { nop, "<init>", "()V", NULL, 0 },
};
static FakeClass g_class_LayoutParams = {
  {NULL}, "android/view/WindowManager$LayoutParams", NULL,
  g_lp_methods, 1, NULL, 0, 0
};

/* getXExtra(name, defaultValue) must return the caller's DEFAULT when the
 * extra is absent -- not zero. There are no extras here, so the default is
 * always the answer, and returning 0 instead silently overrides whatever the
 * caller asked for. A game reading getIntExtra("level", 1) got 0. */
static jvalue view_true(JNIEnv *e, jobject self, const jvalue *a);

static jvalue intent_int_default(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV; r.i = a ? a[1].i : 0; return r;
}
static jvalue intent_bool_default(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV; r.z = a ? a[1].z : JNI_FALSE; return r;
}
static jvalue intent_long_default(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV; r.j = a ? a[1].j : 0; return r;
}

static FakeMethod g_intent_methods[] = {
  { nop, "<init>",            "()V",                                    NULL, 0 },
  { nop, "getExtras",         "()Landroid/os/Bundle;",                  NULL, 0 },
  { nop, "getStringExtra",    "(Ljava/lang/String;)Ljava/lang/String;", NULL, 0 },
  { nop, "getAction",         "()Ljava/lang/String;",                   NULL, 0 },
  { nop, "getData",           "()Landroid/net/Uri;",                    NULL, 0 },
  { nop, "hasExtra",          "(Ljava/lang/String;)Z",                  NULL, 0 },
  { intent_bool_default, "getBooleanExtra", "(Ljava/lang/String;Z)Z",   NULL, 0 },
  { intent_int_default,  "getIntExtra",     "(Ljava/lang/String;I)I",   NULL, 0 },
};
static FakeClass g_class_Intent = {
  {NULL}, "android/content/Intent", NULL,
  g_intent_methods, (int)(sizeof(g_intent_methods)/sizeof(g_intent_methods[0])),
  NULL, 0, 0
};

static jvalue win_getAttributes(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); r.l = g_ref_lp; return r;
}
static jvalue act_getIntent(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); r.l = g_ref_intent; return r;
}

static jvalue win_getDecorView(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  /* The adopted view once OnCreate has installed one -- same reasoning as the
     input dispatches: handing back our orphan makes calls on it no-ops. */
  r.l = lawn_view_target();
  return r;
}

static FakeObject g_window_obj;
static FakeClass  g_class_Window;
static jobject    g_ref_window;

static FakeMethod g_window_methods[] = {
  { nop, "addFlags",              "(I)V",  NULL, 0 },
  { nop, "clearFlags",            "(I)V",  NULL, 0 },
  { nop, "setFlags",              "(II)V", NULL, 0 },
  { nop, "setFormat",             "(I)V",  NULL, 0 },
  /* The decor view is the root of the hierarchy; ours is the SurfaceView the
   * game already draws into. Returning null here is indistinguishable from a
   * working call until managed code dereferences it. */
  { win_getDecorView,  "getDecorView", "()Landroid/view/View;", NULL, 0 },
  { nop, "setContentView",        "(Landroid/view/View;)V", NULL, 0 },
  { nop, "takeSurface",           "(Landroid/view/SurfaceHolder$Callback2;)V", NULL, 0 },
  { nop, "setSoftInputMode",      "(I)V",  NULL, 0 },
  { win_getAttributes, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;", NULL, 0 },
  { nop, "setAttributes",         "(Landroid/view/WindowManager$LayoutParams;)V", NULL, 0 },
};

static FakeClass g_class_Window = {
  {NULL}, "android/view/Window", NULL,
  g_window_methods, (int)(sizeof(g_window_methods)/sizeof(g_window_methods[0])),
  NULL, 0, 0
};

/* android/window/OnBackInvokedDispatcher
 *
 * Android 13's replacement for onBackPressed. The activity asks for one during
 * setup and immediately registers a callback on it, so a null here is
 * dereferenced on the very next instruction -- which is the crash this run
 * ended on.
 *
 * Registration is accepted and ignored: the back gesture does not exist on a
 * console, and the callbacks it would fire are already reachable through the
 * BackInvokedCallback class we register natives for. */
static FakeObject g_backdisp_obj;
static FakeClass  g_class_BackDispatcher;
static jobject    g_ref_backdisp;

static FakeMethod g_backdisp_methods[] = {
  { nop, "registerOnBackInvokedCallback",
      "(ILandroid/window/OnBackInvokedCallback;)V", NULL, 0 },
  { nop, "unregisterOnBackInvokedCallback",
      "(Landroid/window/OnBackInvokedCallback;)V",  NULL, 0 },
};

static FakeClass g_class_BackDispatcher = {
  {NULL}, "android/window/OnBackInvokedDispatcher", NULL,
  g_backdisp_methods,
  (int)(sizeof(g_backdisp_methods)/sizeof(g_backdisp_methods[0])), NULL, 0, 0
};

static jvalue act_getBackDispatcher(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = g_ref_backdisp;
  return r;
}

static jvalue act_getWindow(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = g_ref_window;
  return r;
}

/* Everything else an Activity is asked for -- getAssets, getFilesDir,
 * getPackageName -- is inherited from Context below, exactly as it is on
 * Android through ContextWrapper. Those were returning null only because this
 * class had no ancestry, not because they were unimplemented. */
/* Activity.runOnUiThread
 *
 * This was a nop: it took a Runnable and dropped it, silently, forever.
 *
 * Android's contract is not "post it" -- it is:
 *
 *     if (Thread.currentThread() != mUiThread) mHandler.post(action);
 *     else                                     action.run();
 *
 * and the difference is the whole bug. Code that calls runOnUiThread from the
 * UI thread and then waits for the action to signal completion relies on the
 * inline branch. Queue it instead and the wait can never end, because the only
 * thread that drains the queue is the one now blocked. Drop it entirely, as
 * this did, and nothing ever completes at all.
 *
 * That is exactly the shape of the hang: main inside the surface-init
 * Runnable, spinning on a state word that nothing will ever change, with the
 * queue drain sitting one frame away and unreachable.
 *
 * Same family as the RunnableImplementor.n_run gap fixed earlier -- a Runnable
 * accepted and never run. Worth remembering that a `nop` on a method taking a
 * callback is never harmless; it is a silent promise to do the work later and
 * then not doing it. */
/* Which view is the game actually using?
 *
 * We construct a LawnSurfaceView peer ourselves in stage 4 and drive
 * surfaceCreated/surfaceChanged/doFrame at it. But the Activity's own OnCreate
 * is free to construct its own view and hand that to setContentView -- and
 * this was a nop, so the argument was thrown away unexamined.
 *
 * If those are two different objects, everything observed follows at once: the
 * GL setup runs on OUR view (hence "surface initialized"), while the Activity
 * resumes and enables ITS view, so the flag at +0x41 that gates Start is set on
 * an object we never call DoFrame on. Ours stays disabled, DoFrame early-outs,
 * nothing draws.
 *
 * Compared by dereferenced object, not by handle: the same object legitimately
 * has different handles as a local and a global reference. */
/* The view the Activity actually installed, or NULL before OnCreate. */
static jobject g_content_view;
jobject android_content_view(void) { return g_content_view; }

static jvalue act_setContentView(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV;
  jobject v = (a ? a[0].l : NULL);
  FakeObject *got  = v ? jniref_deref(v) : NULL;
  FakeObject *ours = jniref_deref(lawn_surfaceview_instance());
  debug_log("[act] setContentView(view=%p class=%s) -- the view we built is "
            "%p: %s\n",
            (void *)got, (got && got->cls) ? got->cls->name : "(none)",
            (void *)ours,
            got == ours ? "SAME OBJECT"
                        : "DIFFERENT -- adopting the game's view");

  /* Adopt it.
   *
   * The Activity constructs its own LawnSurfaceView in OnCreate and installs
   * it here; the one we build in stage 4 is a second, unrelated instance. Every
   * callback we were sending -- surfaceCreated, surfaceChanged, doFrame -- went
   * to ours, while the Activity resumed and enabled its own. The flag that
   * gates Start therefore got set on an object we never drove, so our view's
   * DoFrame early-returned forever and nothing was ever drawn.
   *
   * A global reference, and pinned: this has to outlive the local frame the
   * Activity's OnCreate is running in, and it is referenced every frame for
   * the life of the process. */
  if (got && got != ours) {
    g_content_view = jniref_new(got, REF_GLOBAL);
    jniref_pin(g_content_view);
    debug_log("[act] driving the game's own view from now on\n");
  }
  return r;
}

static jvalue act_has_focus(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.z = JNI_TRUE; return r;
}

static jvalue act_runOnUiThread(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self;
  JV;
  if (!a || !a[0].l) return r;

  if (android_os_on_looper_thread()) {
    /* Already the UI thread: Android runs it inline, and callers depend on it
     * having finished by the time this returns. */
    android_os_run_now(e, a[0].l);
  } else {
    android_os_post_runnable(a[0].l, 0);
  }
  return r;
}

static FakeMethod g_activity_base_methods[] = {
  { act_getWindow, "getWindow",         "()Landroid/view/Window;", NULL, 0 },
  { act_getBackDispatcher, "getOnBackInvokedDispatcher",
      "()Landroid/window/OnBackInvokedDispatcher;", NULL, 0 },
  { act_setContentView, "setContentView", "(Landroid/view/View;)V", NULL, 0 },
  { act_runOnUiThread, "runOnUiThread",  "(Ljava/lang/Runnable;)V", NULL, 0 },
  /* Same reasoning as View.requestFocus above: the activity is the only thing
   * on screen, so it has the window focus. Left to the auto-stub this returns
   * false and the game concludes it is in the background. */
  { act_has_focus,     "hasWindowFocus",  "()Z", NULL, 0 },
  { nop,           "finish",            "()V",                     NULL, 0 },
  /* true: the feature IS honoured -- there is no title bar to remove, so a
   * request to remove it has already succeeded. false means "not available",
   * which can send the caller down a fallback path for no reason. */
  { act_has_focus, "requestWindowFeature","(I)Z",                  NULL, 0 },
  { nop,           "setRequestedOrientation","(I)V",               NULL, 0 },
  { act_getIntent, "getIntent",         "()Landroid/content/Intent;", NULL, 0 },
};

static FakeClass g_class_Activity = {
  {NULL}, "android/app/Activity", NULL,
  g_activity_base_methods,
  (int)(sizeof(g_activity_base_methods)/sizeof(g_activity_base_methods[0])),
  NULL, 0, 0
};
/* getHolder is called by the managed SurfaceView during construction and its
 * result is dereferenced immediately, so a stub returning null crashes there
 * rather than later. */
static jvalue sv_getHolder(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = android_os_surfaceholder();
  return r;
}

static FakeMethod g_surfaceview_base_methods[] = {
  { sv_getHolder, "getHolder", "()Landroid/view/SurfaceHolder;", NULL, 0 },
  { nop,          "setZOrderOnTop",       "(Z)V", NULL, 0 },
  { nop,          "setZOrderMediaOverlay","(Z)V", NULL, 0 },
  { nop,          "setWillNotDraw",       "(Z)V", NULL, 0 },
  { nop,          "setFocusable",         "(Z)V", NULL, 0 },
  { nop,          "setFocusableInTouchMode","(Z)V", NULL, 0 },
  /* This SHADOWED the fix on android/view/View.
   *
   * A class's own table is searched before its superclass, so this nop won and
   * View.requestFocus (which returns true) was never reached for the object
   * that matters -- the SurfaceView. The generic fix was in place for rounds
   * while the specific one quietly overrode it. Same answer as View's now. */
  { view_true,    "requestFocus",         "()Z",  NULL, 0 },
};

static FakeClass g_class_SurfaceView = {
  {NULL}, "android/view/SurfaceView", NULL,
  g_surfaceview_base_methods,
  (int)(sizeof(g_surfaceview_base_methods)/sizeof(g_surfaceview_base_methods[0])),
  NULL, 0, 0
};
/* android/view/View
 *
 * getContext() returns the Context the view was constructed with, and we know
 * exactly what that was: main.c passes android_get_context() as the single
 * argument to the LawnSurfaceView constructor. Leaving it to the auto-stub
 * handed managed code a null Context instead. */
static jvalue view_getContext(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.l = android_get_context(); return r;
}

/* Focus.
 *
 * requestFocus() was reaching the auto-stub, which returns 0 -- so the view
 * asked for focus and was told it had been refused. MonoGame's Android view
 * calls RequestFocus() during setup and treats the result as authoritative: a
 * view that believes it is not focused is not active, so it does not render
 * and it pauses audio. That is precisely the end state of the last run --
 * surface initialised, doFrame running, nothing drawn, AAudio paused.
 *
 * There is exactly one window and one view here and nothing to compete with
 * them, so "yes" is not a convenient lie, it is the truth. The whole family is
 * answered together rather than one at a time, because they are asked
 * interchangeably and a stub returning 0 for any one of them reproduces the
 * same bug in a slightly different place. */
static jvalue view_true(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV; r.z = JNI_TRUE; return r;
}

static FakeMethod g_view_methods[] = {
  { view_getContext, "getContext", "()Landroid/content/Context;", NULL, 0 },
  { view_true, "requestFocus",           "()Z",  NULL, 0 },
  { view_true, "requestFocusFromTouch",  "()Z",  NULL, 0 },
  { view_true, "hasFocus",               "()Z",  NULL, 0 },
  { view_true, "isFocused",              "()Z",  NULL, 0 },
  { view_true, "hasWindowFocus",         "()Z",  NULL, 0 },
  { view_true, "isShown",                "()Z",  NULL, 0 },
  { view_true, "isEnabled",              "()Z",  NULL, 0 },
};

static FakeClass g_class_View = {
  {NULL}, "android/view/View", NULL,
  g_view_methods, (int)(sizeof(g_view_methods)/sizeof(g_view_methods[0])),
  NULL, 0, 0
};

/* android/view/InputDevice
 *
 * MonoGame enumerates input devices while the game object is being built:
 * getDeviceIds(), then getDevice() for each id. The auto-stub returned null
 * for an int[], and the caller loads the array length from +8 without checking
 * -- a read of address 8, which is precisely what crashed.
 *
 * An empty array is not a placeholder here, it is the truth: nx_pointer is not
 * compiled in, so this build has no input devices to report. The caller tests
 * `length <= 0` and skips the loop cleanly. When input is wired up this is
 * where a device would be announced. */
static jvalue inputdev_getDeviceIds(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self; (void)a;
  JV; r.l = (*e)->NewIntArray(e, 0); return r;
}

static jvalue inputdev_getDevice(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  /* Never reached while getDeviceIds() is empty, and the caller null-checks
   * the result anyway -- Android returns null for an id that has gone away. */
  JV; return r;
}

static FakeMethod g_inputdevice_methods[] = {
  { inputdev_getDeviceIds, "getDeviceIds", "()[I",                          NULL, 1 },
  { inputdev_getDevice,    "getDevice",    "(I)Landroid/view/InputDevice;", NULL, 1 },
};

static FakeClass g_class_InputDevice = {
  {NULL}, "android/view/InputDevice", NULL,
  g_inputdevice_methods,
  (int)(sizeof(g_inputdevice_methods)/sizeof(g_inputdevice_methods[0])),
  NULL, 0, 0
};

static FakeClass g_class_ContentProv  = { {NULL}, "android/content/ContentProvider", NULL, NULL, 0, NULL, 0 };

/* The IME surface. None of it matters without a software keyboard, but each
 * method forwards to its native rather than being stubbed -- so if text entry
 * is ever wired up it works, instead of silently swallowing every edit. */
FWD(ime_beginBatchEdit, "n_beginBatchEdit")
FWD(ime_endBatchEdit, "n_endBatchEdit")
FWD(ime_finishComposingText, "n_finishComposingText")
FWD(ime_commitCompletion, "n_commitCompletion")
FWD(ime_commitText, "n_commitText")
FWD(ime_deleteSurroundingText, "n_deleteSurroundingText")
FWD(ime_deleteSurroundingTextInCodePoints, "n_deleteSurroundingTextInCodePoints")
FWD(ime_getEditable, "n_getEditable")
FWD(ime_performContextMenuAction, "n_performContextMenuAction")
FWD(ime_performEditorAction, "n_performEditorAction")
FWD(ime_sendKeyEvent, "n_sendKeyEvent")
FWD(ime_setComposingRegion, "n_setComposingRegion")
FWD(ime_setComposingText, "n_setComposingText")
FWD(ime_setSelection, "n_setSelection")
FWD(ime_clearMetaKeyStates, "n_clearMetaKeyStates")
FWD(ime_closeConnection, "n_closeConnection")
FWD(ime_reportFullscreenMode, "n_reportFullscreenMode")
FWD(ime_requestCursorUpdates, "n_requestCursorUpdates")
FWD(ime_performPrivateCommand, "n_performPrivateCommand")
FWD(ime_commitContent, "n_commitContent")

static FakeMethod g_ime_methods[] = {
  { nop, "<init>", "(Landroid/view/View;Z)V", NULL, 0 },
  { ime_beginBatchEdit, "beginBatchEdit", "()Z", NULL, 0 },
  { NULL,    "n_beginBatchEdit", "()Z", NULL, 0 },
  { ime_endBatchEdit, "endBatchEdit", "()Z", NULL, 0 },
  { NULL,    "n_endBatchEdit", "()Z", NULL, 0 },
  { ime_finishComposingText, "finishComposingText", "()Z", NULL, 0 },
  { NULL,    "n_finishComposingText", "()Z", NULL, 0 },
  { ime_commitCompletion, "commitCompletion", "(Landroid/view/inputmethod/CompletionInfo;)Z", NULL, 0 },
  { NULL,    "n_commitCompletion", "(Landroid/view/inputmethod/CompletionInfo;)Z", NULL, 0 },
  { ime_commitText, "commitText", "(Ljava/lang/CharSequence;I)Z", NULL, 0 },
  { NULL,    "n_commitText", "(Ljava/lang/CharSequence;I)Z", NULL, 0 },
  { ime_deleteSurroundingText, "deleteSurroundingText", "(II)Z", NULL, 0 },
  { NULL,    "n_deleteSurroundingText", "(II)Z", NULL, 0 },
  { ime_deleteSurroundingTextInCodePoints, "deleteSurroundingTextInCodePoints", "(II)Z", NULL, 0 },
  { NULL,    "n_deleteSurroundingTextInCodePoints", "(II)Z", NULL, 0 },
  { ime_getEditable, "getEditable", "()Landroid/text/Editable;", NULL, 0 },
  { NULL,    "n_getEditable", "()Landroid/text/Editable;", NULL, 0 },
  { ime_performContextMenuAction, "performContextMenuAction", "(I)Z", NULL, 0 },
  { NULL,    "n_performContextMenuAction", "(I)Z", NULL, 0 },
  { ime_performEditorAction, "performEditorAction", "(I)Z", NULL, 0 },
  { NULL,    "n_performEditorAction", "(I)Z", NULL, 0 },
  { ime_sendKeyEvent, "sendKeyEvent", "(Landroid/view/KeyEvent;)Z", NULL, 0 },
  { NULL,    "n_sendKeyEvent", "(Landroid/view/KeyEvent;)Z", NULL, 0 },
  { ime_setComposingRegion, "setComposingRegion", "(II)Z", NULL, 0 },
  { NULL,    "n_setComposingRegion", "(II)Z", NULL, 0 },
  { ime_setComposingText, "setComposingText", "(Ljava/lang/CharSequence;I)Z", NULL, 0 },
  { NULL,    "n_setComposingText", "(Ljava/lang/CharSequence;I)Z", NULL, 0 },
  { ime_setSelection, "setSelection", "(II)Z", NULL, 0 },
  { NULL,    "n_setSelection", "(II)Z", NULL, 0 },
  { ime_clearMetaKeyStates, "clearMetaKeyStates", "(I)Z", NULL, 0 },
  { NULL,    "n_clearMetaKeyStates", "(I)Z", NULL, 0 },
  { ime_closeConnection, "closeConnection", "()V", NULL, 0 },
  { NULL,    "n_closeConnection", "()V", NULL, 0 },
  { ime_reportFullscreenMode, "reportFullscreenMode", "(Z)Z", NULL, 0 },
  { NULL,    "n_reportFullscreenMode", "(Z)Z", NULL, 0 },
  { ime_requestCursorUpdates, "requestCursorUpdates", "(I)Z", NULL, 0 },
  { NULL,    "n_requestCursorUpdates", "(I)Z", NULL, 0 },
  { ime_performPrivateCommand, "performPrivateCommand", "(Ljava/lang/String;Landroid/os/Bundle;)Z", NULL, 0 },
  { NULL,    "n_performPrivateCommand", "(Ljava/lang/String;Landroid/os/Bundle;)Z", NULL, 0 },
  { ime_commitContent, "commitContent", "(Landroid/view/inputmethod/InputContentInfo;ILandroid/os/Bundle;)Z", NULL, 0 },
  { NULL,    "n_commitContent", "(Landroid/view/inputmethod/InputContentInfo;ILandroid/os/Bundle;)Z", NULL, 0 },
  PEER_METHODS,
};

static FakeClass g_class_LawnInputConnection = {
  {NULL}, "crc646ec5dadb3c3b2bda/AndroidNativeActivity_LawnSurfaceView_LawnInputConnection",
  NULL, g_ime_methods, (int)(sizeof(g_ime_methods)/sizeof(g_ime_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */

void android_runtime_init(void) {
  FakeClass *all[] = {
    &g_class_Log, &g_class_Os,
    &g_class_UncaughtMarshaler, &g_class_XamarinUncaught,
    &g_class_JavaObject,
    &g_class_JavaProxyThrowable_dot, &g_class_JavaProxyThrowable_and,
    &g_class_NativeAotRuntimeProvider,
    &g_class_InputStreamAdapter, &g_class_OutputStreamAdapter,
    &g_class_Runnable, &g_class_BackInvoked, &g_class_OnBackInvokedCallback,
    &g_class_DialogDismiss, &g_class_ViewOnClick, &g_class_DialogOnClick,
    &g_class_Activity, &g_class_SurfaceView, &g_class_View,
    &g_class_InputDevice,
    &g_class_Window, &g_class_BackDispatcher,
    &g_class_LayoutParams, &g_class_Intent,
    &g_class_ContentProv, &g_class_LawnInputConnection,
  };
  for (size_t i = 0; i < sizeof(all)/sizeof(all[0]); i++)
    jni_register_class(all[i]);

  /* Superclass links, so a lookup that misses on the subclass still finds the
   * inherited method rather than raising NoSuchMethodError. */
  g_class_NativeAotRuntimeProvider.super = &g_class_ContentProv;
  g_class_SurfaceView.super              = &g_class_View;

  /* The ancestry that makes getAssets and getFilesDir resolve. */
  g_class_Activity.super = &g_class_Context;
  g_window_obj.cls       = &g_class_Window;
  g_ref_window           = jniref_new(&g_window_obj, REF_GLOBAL);
  jniref_pin(g_ref_window);

  g_lp_obj.cls     = &g_class_LayoutParams;
  g_intent_obj.cls = &g_class_Intent;
  g_ref_lp     = jniref_new(&g_lp_obj,     REF_GLOBAL);
  g_ref_intent = jniref_new(&g_intent_obj, REF_GLOBAL);
  jniref_pin(g_ref_lp);
  jniref_pin(g_ref_intent);

  g_backdisp_obj.cls = &g_class_BackDispatcher;
  g_ref_backdisp     = jniref_new(&g_backdisp_obj, REF_GLOBAL);
  jniref_pin(g_ref_backdisp);
  g_class_BackInvoked.super              = &g_class_OnBackInvokedCallback;

  debug_log("[java] %zu runtime/adapter classes registered\n",
            sizeof(all)/sizeof(all[0]));
}
