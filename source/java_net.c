/* java_net.c -- java.net.URL / URLConnection, enough of them to fail properly.
 *
 * WHY THIS FILE EXISTS
 *
 * Confirming the player name crashed the game:
 *
 *     [jni] auto java/net/URL (stubbed; not implemented)
 *     [jni] auto java/net/URL.<init>(Ljava/lang/String;)V
 *     [jni] auto java/net/Proxy.NO_PROXY:Ljava/net/Proxy;
 *     [jni] auto java/net/URL.openConnection()Ljava/net/URLConnection;
 *     [jni] NULL OBJECT from openConnection()Ljava/net/URLConnection;
 *     CRASH: data abort, read of 0x0 -- ldr wzr, [x0] with x0 = 0
 *
 * `java/net/URL` was an auto-manufactured stub, so `openConnection()` returned
 * null, and the managed null check on the result was fatal. On Linux that null
 * check raises NullReferenceException; here signals cannot be delivered, so it
 * is a hard crash.
 *
 * THE SHAPE OF THE FIX
 *
 * Returning any non-null object would stop this particular crash and move it to
 * the next call. What actually helps is failing the way the game has already
 * been tested against: **an Android device with no network.** On such a device
 *
 *   - `new URL(s)` succeeds,
 *   - `openConnection()` succeeds -- it does not connect, it only builds the
 *     connection object,
 *   - and the first call that needs the network -- connect, getResponseCode,
 *     getInputStream -- throws UnknownHostException, an IOException.
 *
 * Any code doing HTTP handles that path, because it is the one that fires
 * whenever the user is on a train. A null does not resemble any state the
 * game's authors could have anticipated.
 *
 * This is the same bargain as the audio shim, which resolves the library so the
 * P/Invoke binds and then reports no device at openStream: produce the error
 * the engine knows how to handle rather than one it has never seen.
 *
 * WHAT IS DELIBERATELY NOT DONE
 *
 * No sockets. The Switch has networking, but a game phoning home from a
 * homebrew port is not something to enable silently, and whatever this request
 * is -- the log will now print the URL -- the game must already cope with it
 * failing. If it turns out to be load-bearing, the URL in the log is the
 * starting point.
 */

#include <string.h>
#include <stdio.h>

#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "util.h"

#define JV_ZERO jvalue r; memset(&r, 0, sizeof(r))

/* The exception an offline device produces. A subclass of IOException, so a
 * catch of either shape sees it. */
#define NET_EXC    "java/net/UnknownHostException"
#define NET_DETAIL "no network in this port -- java_net.c fails every " \
                   "connection deliberately"

/* ------------------------------------------------------------------ URL --- */

typedef struct {
  FakeObject hdr;
  char       spec[512];
} FakeURL;

/* Type-CHECKED, because the alternative is a wild pointer.
 *
 * FakeString is { FakeObject hdr; char *utf; ... } and FakeURL is
 * { FakeObject hdr; char spec[512]; } -- so `utf` and the first eight bytes of
 * `spec` occupy the same offset. Casting an arbitrary argument to FakeString
 * and reading ->utf therefore turns URL text into a pointer and hands it to
 * snprintf. URL(URL, String) passes a URL in slot 0, so this was reachable.
 * Compare the class before believing the cast. */
static const char *arg_string(const jvalue *a, int i) {
  if (!a) return NULL;
  FakeObject *o = jniref_deref(a[i].l);
  if (!o || o->cls != &g_class_String) return NULL;
  return ((FakeString *)o)->utf;
}

static jvalue url_init(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeURL *u = (FakeURL *)jniref_deref(self);
  if (!u) return r;

  /* Every variant here ends with the spec, so the last real String wins.
   * Unused slots are zeroed by the caller and a NULL handle derefs to NULL. */
  const char *s = NULL;
  for (int i = 0; i < 3; i++) {
    const char *cand = arg_string(a, i);
    if (cand) s = cand;
  }
  snprintf(u->spec, sizeof(u->spec), "%s", s ? s : "(unknown)");

  /* Print it. Whatever the game is contacting, this is the only place the
   * address is visible, and it decides whether failing the request is
   * harmless or the thing to fix next. */
  debug_log("[net] the game built a URL: %s\n", u->spec);
  return r;
}

static jvalue url_toString(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeURL *u = (FakeURL *)jniref_deref(self);
  r.l = jni_make_string(u ? u->spec : "");
  return r;
}

/* Empty strings rather than null: these are declared to return a String and
 * callers concatenate or compare them without checking. */
static jvalue url_empty_string(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.l = jni_make_string("");
  return r;
}

static jvalue url_getPort(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.i = -1; return r;          /* -1 == "not specified", as in Java */
}

static jobject make_connection(void);

static jvalue url_openConnection(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeURL *u = (FakeURL *)jniref_deref(self);
  debug_log("[net] openConnection(%s) -- returning a connection that will "
            "fail at connect, the way an offline device does\n",
            u ? u->spec : "?");
  r.l = make_connection();
  return r;
}

/* openStream() is connect + getInputStream in one, so it throws here. */
static jvalue url_openStream(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self; (void)a;
  JV_ZERO;
  jni_raise(e, NET_EXC, NET_DETAIL);
  return r;
}

static FakeMethod g_url_methods[] = {
  { url_init,           "<init>",          "(Ljava/lang/String;)V",                       NULL, 0 },
  { url_init,           "<init>",          "(Ljava/net/URL;Ljava/lang/String;)V",         NULL, 0 },
  { url_init,           "<init>",          "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", NULL, 0 },
  { url_openConnection, "openConnection",  "()Ljava/net/URLConnection;",                  NULL, 0 },
  { url_openConnection, "openConnection",  "(Ljava/net/Proxy;)Ljava/net/URLConnection;",  NULL, 0 },
  { url_openStream,     "openStream",      "()Ljava/io/InputStream;",                     NULL, 0 },
  { url_toString,       "toString",        "()Ljava/lang/String;",                        NULL, 0 },
  { url_toString,       "toExternalForm",  "()Ljava/lang/String;",                        NULL, 0 },
  { url_empty_string,   "getHost",         "()Ljava/lang/String;",                        NULL, 0 },
  { url_empty_string,   "getProtocol",     "()Ljava/lang/String;",                        NULL, 0 },
  { url_empty_string,   "getPath",         "()Ljava/lang/String;",                        NULL, 0 },
  { url_empty_string,   "getFile",         "()Ljava/lang/String;",                        NULL, 0 },
  { url_empty_string,   "getQuery",        "()Ljava/lang/String;",                        NULL, 0 },
  { url_getPort,        "getPort",         "()I",                                         NULL, 0 },
  { url_getPort,        "getDefaultPort",  "()I",                                         NULL, 0 },
};

static FakeClass g_class_URL = {
  {NULL}, "java/net/URL", NULL,
  g_url_methods, (int)(sizeof(g_url_methods) / sizeof(g_url_methods[0])),
  NULL, 0, sizeof(FakeURL)
};

/* ------------------------------------------------------- URLConnection --- */

/* Every method that would touch the network. Declared `throws IOException` in
 * Java, so raising here is the documented contract, not an improvisation. */
static jvalue conn_throw(JNIEnv *e, jobject self, const jvalue *a) {
  (void)self; (void)a;
  JV_ZERO;
  static int said;
  if (!said) {
    said = 1;
    debug_log("[net] a connection method was called -- raising %s. If the game "
              "handles it, this is invisible; if it does not, the managed stack "
              "trace will name the caller.\n", NET_EXC);
  }
  jni_raise(e, NET_EXC, NET_DETAIL);
  return r;
}

/* Configuration is accepted and discarded. These return void and never throw,
 * so swallowing them is exactly right -- and a stub that threw here would fail
 * before the game had finished describing the request it wants to make. */
static jvalue conn_noop(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; return r;
}

static jvalue conn_minus_one(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.i = -1; return r;
}

static jvalue conn_zero_long(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.j = 0; return r;
}

/* Genuinely nullable in Java -- absent header, no error body. Callers null-check
 * these because on a real device they are null all the time. */
static jvalue conn_null(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.l = NULL; return r;
}

static FakeMethod g_conn_methods[] = {
  /* the network */
  { conn_throw,     "connect",                   "()V",                                   NULL, 0 },
  { conn_throw,     "getResponseCode",           "()I",                                   NULL, 0 },
  { conn_throw,     "getResponseMessage",        "()Ljava/lang/String;",                  NULL, 0 },
  { conn_throw,     "getInputStream",            "()Ljava/io/InputStream;",               NULL, 0 },
  { conn_throw,     "getOutputStream",           "()Ljava/io/OutputStream;",              NULL, 0 },
  { conn_throw,     "getContent",                "()Ljava/lang/Object;",                  NULL, 0 },

  /* configuration */
  { conn_noop,      "setConnectTimeout",         "(I)V",                                  NULL, 0 },
  { conn_noop,      "setReadTimeout",            "(I)V",                                  NULL, 0 },
  { conn_noop,      "setRequestMethod",          "(Ljava/lang/String;)V",                 NULL, 0 },
  { conn_noop,      "setRequestProperty",        "(Ljava/lang/String;Ljava/lang/String;)V", NULL, 0 },
  { conn_noop,      "addRequestProperty",        "(Ljava/lang/String;Ljava/lang/String;)V", NULL, 0 },
  { conn_noop,      "setDoInput",                "(Z)V",                                  NULL, 0 },
  { conn_noop,      "setDoOutput",               "(Z)V",                                  NULL, 0 },
  { conn_noop,      "setUseCaches",              "(Z)V",                                  NULL, 0 },
  { conn_noop,      "setAllowUserInteraction",   "(Z)V",                                  NULL, 0 },
  { conn_noop,      "setInstanceFollowRedirects","(Z)V",                                  NULL, 0 },
  { conn_noop,      "setIfModifiedSince",        "(J)V",                                  NULL, 0 },
  { conn_noop,      "setFixedLengthStreamingMode","(I)V",                                 NULL, 0 },
  { conn_noop,      "setChunkedStreamingMode",   "(I)V",                                  NULL, 0 },
  { conn_noop,      "disconnect",                "()V",                                   NULL, 0 },

  /* metadata that does not need the network */
  { conn_minus_one, "getContentLength",          "()I",                                   NULL, 0 },
  { conn_zero_long, "getContentLengthLong",      "()J",                                   NULL, 0 },
  { conn_zero_long, "getDate",                   "()J",                                   NULL, 0 },
  { conn_zero_long, "getLastModified",           "()J",                                   NULL, 0 },
  { conn_zero_long, "getExpiration",             "()J",                                   NULL, 0 },
  { conn_null,      "getContentType",            "()Ljava/lang/String;",                  NULL, 0 },
  { conn_null,      "getContentEncoding",        "()Ljava/lang/String;",                  NULL, 0 },
  { conn_null,      "getHeaderField",            "(Ljava/lang/String;)Ljava/lang/String;",NULL, 0 },
  { conn_null,      "getErrorStream",            "()Ljava/io/InputStream;",               NULL, 0 },
};

static FakeClass g_class_URLConnection = {
  {NULL}, "java/net/URLConnection", NULL,
  g_conn_methods, (int)(sizeof(g_conn_methods) / sizeof(g_conn_methods[0])),
  NULL, 0, 0
};

/* HttpURLConnection is what the game will cast the result to. Sharing the
 * method table via `super` means a cast finds the same behaviour either way. */
static FakeClass g_class_HttpURLConnection = {
  {NULL}, "java/net/HttpURLConnection", &g_class_URLConnection,
  NULL, 0, NULL, 0, 0
};

/* One shared instance: it holds no per-request state, and the game makes these
 * one at a time. Static, so jniref never tries to free it. */
static FakeObject g_connection;

static jobject make_connection(void) {
  g_connection.cls = &g_class_HttpURLConnection;
  return jniref_new(&g_connection, REF_LOCAL);
}

/* -------------------------------------------------------------- Proxy --- */

/* Only ever passed straight back to openConnection, so it needs to exist and
 * be non-null and nothing more. NO_PROXY resolves through jni_stub_field,
 * which already hands out a stub instance rather than null. */
static FakeClass g_class_Proxy = {
  {NULL}, "java/net/Proxy", NULL, NULL, 0, NULL, 0, 0
};

void java_net_init(void) {
  jni_register_class(&g_class_URL);
  jni_register_class(&g_class_URLConnection);
  jni_register_class(&g_class_HttpURLConnection);
  jni_register_class(&g_class_Proxy);
  debug_log("[net] java.net registered -- connections resolve and then fail "
            "with %s, rather than openConnection returning null\n", NET_EXC);
}
