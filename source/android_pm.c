/* android_pm.c -- PackageManager, PackageInfo, ApplicationInfo.
 *
 * WHY
 *
 * Opening Options -> Advanced crashed:
 *
 *     [jni] auto-method on REAL class android/content/Context.getPackageManager()
 *     [jni] NULL OBJECT from getPackageManager()Landroid/content/pm/PackageManager;
 *     CRASH: data abort, read of 0x0 -- ldr wzr, [x0] with x0 = 0
 *
 * `Context.getPackageManager()` was not implemented, so it fell through to the
 * auto-method path and returned null, and the managed null check on the result
 * was fatal. An Advanced or About screen wants the version string, which on
 * Android means `getPackageManager().getPackageInfo(getPackageName(), 0)`.
 *
 * `Context.getApplicationInfo()` was explicitly wired to `ctx_null` and is the
 * same landmine one step further on -- it is answered here too.
 *
 * FIELDS, NOT GETTERS
 *
 * PackageInfo and ApplicationInfo expose public FIELDS. GetFieldID would have
 * manufactured stubs for them, and a stub OBJECT field hands back a stub
 * instance rather than a String -- so `pkgInfo.versionName` would come back as
 * something that is not a string, and the first String method called on it
 * would be another auto-stub. Real fields with real values are barely more work
 * and stop that chain before it starts.
 *
 * The values are the ones the rest of the shim already claims: the package name
 * matches Context.getPackageName(), and the library path matches
 * MonoPackageManager.getNativeLibraryPath(). Two different answers to the same
 * question is its own class of bug.
 */

#include <string.h>

#include "android_classes.h"
#include "android_pm.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "util.h"

#define JV_ZERO jvalue r; memset(&r, 0, sizeof(r))

/* Kept identical to android_classes.c and interop_classes.c. */
#define PKG_NAME    "com.pvz.ultimate"
#define APK_PATH    "/data/data/com.pvz.ultimate/base.apk"
#define LIB_PATH    "/data/data/com.pvz.ultimate/lib"
#define DATA_DIR    "/data/data/com.pvz.ultimate"
#define VERSION_STR "1.0"
#define VERSION_INT 1

/* ------------------------------------------------------- ApplicationInfo -- */

static FakeObject g_appinfo_obj;

static jvalue ai_packageName(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.l = jni_make_string(PKG_NAME);  return r;
}
static jvalue ai_sourceDir(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.l = jni_make_string(APK_PATH);  return r;
}
static jvalue ai_nativeLibraryDir(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.l = jni_make_string(LIB_PATH);  return r;
}
static jvalue ai_dataDir(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.l = jni_make_string(DATA_DIR);  return r;
}
static jvalue ai_targetSdk(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.i = 29; return r;   /* matches Build.VERSION */
}
static jvalue ai_zero_int(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.i = 0; return r;
}

static FakeField g_appinfo_fields[] = {
  { "packageName",      "Ljava/lang/String;", ai_packageName,      NULL, {0} },
  { "sourceDir",        "Ljava/lang/String;", ai_sourceDir,        NULL, {0} },
  { "publicSourceDir",  "Ljava/lang/String;", ai_sourceDir,        NULL, {0} },
  { "nativeLibraryDir", "Ljava/lang/String;", ai_nativeLibraryDir, NULL, {0} },
  { "dataDir",          "Ljava/lang/String;", ai_dataDir,          NULL, {0} },
  { "targetSdkVersion", "I",                  ai_targetSdk,        NULL, {0} },
  { "flags",            "I",                  ai_zero_int,         NULL, {0} },
  { "labelRes",         "I",                  ai_zero_int,         NULL, {0} },
  { "icon",             "I",                  ai_zero_int,         NULL, {0} },
};

static FakeClass g_class_ApplicationInfo = {
  {NULL}, "android/content/pm/ApplicationInfo", NULL,
  NULL, 0,
  g_appinfo_fields,
  (int)(sizeof(g_appinfo_fields) / sizeof(g_appinfo_fields[0])), 0
};

jobject android_application_info(void) {
  return jniref_new(&g_appinfo_obj, REF_LOCAL);
}

/* ----------------------------------------------------------- PackageInfo -- */

static FakeObject g_pkginfo_obj;

static jvalue pi_packageName(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.l = jni_make_string(PKG_NAME); return r;
}
static jvalue pi_versionName(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.l = jni_make_string(VERSION_STR); return r;
}
static jvalue pi_versionCode(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.i = VERSION_INT; return r;
}
static jvalue pi_longVersionCode(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.j = VERSION_INT; return r;
}
static jvalue pi_appInfo(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.l = android_application_info(); return r;
}
static jvalue pi_time(JNIEnv *e, jobject self) {
  (void)e; (void)self; JV_ZERO; r.j = 0; return r;
}

static FakeField g_pkginfo_fields[] = {
  { "packageName",      "Ljava/lang/String;",                    pi_packageName,     NULL, {0} },
  { "versionName",      "Ljava/lang/String;",                    pi_versionName,     NULL, {0} },
  { "versionCode",      "I",                                     pi_versionCode,     NULL, {0} },
  { "longVersionCode",  "J",                                     pi_longVersionCode, NULL, {0} },
  { "applicationInfo",  "Landroid/content/pm/ApplicationInfo;",  pi_appInfo,         NULL, {0} },
  { "firstInstallTime", "J",                                     pi_time,            NULL, {0} },
  { "lastUpdateTime",   "J",                                     pi_time,            NULL, {0} },
};

static FakeClass g_class_PackageInfo = {
  {NULL}, "android/content/pm/PackageInfo", NULL,
  NULL, 0,
  g_pkginfo_fields,
  (int)(sizeof(g_pkginfo_fields) / sizeof(g_pkginfo_fields[0])), 0
};

/* -------------------------------------------------------- PackageManager -- */

static FakeObject g_pm_obj;

static jvalue pm_getPackageInfo(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO;
  r.l = jniref_new(&g_pkginfo_obj, REF_LOCAL);
  debug_log("[pm] getPackageInfo -> %s version %s (%d)\n",
            PKG_NAME, VERSION_STR, VERSION_INT);
  return r;
}

static jvalue pm_getApplicationInfo(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.l = android_application_info(); return r;
}

static jvalue pm_getApplicationLabel(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.l = jni_make_string("Plants vs. Zombies"); return r;
}

/* Genuinely nullable on a real device: no activity handles the intent, no
 * installer recorded. Callers null-check these because that is the normal
 * answer on a device without a Play Store. */
static jvalue pm_null(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; return r;
}

static jvalue pm_false(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.z = JNI_FALSE; return r;
}

/* PERMISSION_DENIED is -1, GRANTED is 0. The shim already grants everything
 * through Context.checkSelfPermission, so agree with it. */
static jvalue pm_granted(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.i = 0; return r;
}

/* An empty list, not null: callers iterate the result of a query without
 * checking, because on Android it is never null.
 *
 * It has to be a REAL empty list, not an auto-manufactured one. FindClass on an
 * unregistered java/util/ArrayList would produce a stub whose isEmpty() returns
 * 0 -- that is, FALSE -- so `if (!list.isEmpty()) list.get(0)` would take the
 * branch and dereference a null. A stub list is worse than no list.
 *
 * And it must NOT be registered as java/util/ArrayList: the game may well build
 * real ArrayLists elsewhere, and an always-empty one would silently break them.
 * Collections$EmptyList is a real JDK type that nothing constructs directly,
 * so claiming that name hijacks nothing. */
static FakeObject g_emptylist_obj;
static FakeObject g_emptyiter_obj;

static jvalue lst_size(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.i = 0; return r;
}
static jvalue lst_isEmpty(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.z = JNI_TRUE; return r;
}
static jvalue lst_minus_one(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.i = -1; return r;
}
static jvalue lst_false(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; r.z = JNI_FALSE; return r;
}
static jvalue lst_null(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a; JV_ZERO; return r;
}
static jvalue lst_iterator(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.l = jniref_new(&g_emptyiter_obj, REF_LOCAL); return r;
}

static FakeMethod g_emptylist_methods[] = {
  { lst_size,      "size",     "()I",                          NULL, 0 },
  { lst_isEmpty,   "isEmpty",  "()Z",                          NULL, 0 },
  { lst_iterator,  "iterator", "()Ljava/util/Iterator;",       NULL, 0 },
  { lst_null,      "get",      "(I)Ljava/lang/Object;",        NULL, 0 },
  { lst_false,     "contains", "(Ljava/lang/Object;)Z",        NULL, 0 },
  { lst_minus_one, "indexOf",  "(Ljava/lang/Object;)I",        NULL, 0 },
};

static FakeMethod g_emptyiter_methods[] = {
  { lst_false, "hasNext", "()Z",                  NULL, 0 },
  { lst_null,  "next",    "()Ljava/lang/Object;", NULL, 0 },
};

static FakeClass g_class_EmptyList = {
  {NULL}, "java/util/Collections$EmptyList", NULL,
  g_emptylist_methods,
  (int)(sizeof(g_emptylist_methods) / sizeof(g_emptylist_methods[0])), NULL, 0, 0
};

static FakeClass g_class_EmptyIterator = {
  {NULL}, "java/util/Collections$EmptyIterator", NULL,
  g_emptyiter_methods,
  (int)(sizeof(g_emptyiter_methods) / sizeof(g_emptyiter_methods[0])), NULL, 0, 0
};

static jvalue pm_empty_list(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.l = jniref_new(&g_emptylist_obj, REF_LOCAL); return r;
}

static FakeMethod g_pm_methods[] = {
  { pm_getPackageInfo,      "getPackageInfo",           "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",       NULL, 0 },
  { pm_getApplicationInfo,  "getApplicationInfo",       "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",  NULL, 0 },
  { pm_getApplicationLabel, "getApplicationLabel",      "(Landroid/content/pm/ApplicationInfo;)Ljava/lang/CharSequence;", NULL, 0 },
  { pm_null,                "getLaunchIntentForPackage","(Ljava/lang/String;)Landroid/content/Intent;",               NULL, 0 },
  { pm_null,                "resolveActivity",          "(Landroid/content/Intent;I)Landroid/content/pm/ResolveInfo;",NULL, 0 },
  { pm_null,                "getInstallerPackageName",  "(Ljava/lang/String;)Ljava/lang/String;",                     NULL, 0 },
  { pm_empty_list,          "queryIntentActivities",    "(Landroid/content/Intent;I)Ljava/util/List;",                NULL, 0 },
  { pm_false,               "hasSystemFeature",         "(Ljava/lang/String;)Z",                                      NULL, 0 },
  { pm_granted,             "checkPermission",          "(Ljava/lang/String;Ljava/lang/String;)I",                    NULL, 0 },
};

static FakeClass g_class_PackageManager = {
  {NULL}, "android/content/pm/PackageManager", NULL,
  g_pm_methods, (int)(sizeof(g_pm_methods) / sizeof(g_pm_methods[0])),
  NULL, 0, 0
};

jobject android_package_manager(void) {
  return jniref_new(&g_pm_obj, REF_LOCAL);
}

void android_pm_init(void) {
  jni_register_class(&g_class_PackageManager);
  jni_register_class(&g_class_PackageInfo);
  jni_register_class(&g_class_ApplicationInfo);
  jni_register_class(&g_class_EmptyList);
  jni_register_class(&g_class_EmptyIterator);

  g_emptylist_obj.cls = &g_class_EmptyList;
  g_emptyiter_obj.cls = &g_class_EmptyIterator;

  g_pm_obj.cls      = &g_class_PackageManager;
  g_pkginfo_obj.cls = &g_class_PackageInfo;
  g_appinfo_obj.cls = &g_class_ApplicationInfo;

  debug_log("[pm] PackageManager registered; getPackageInfo reports %s %s\n",
            PKG_NAME, VERSION_STR);
}
