/* android_classes.c -- the Android framework surface the game reaches for.
 *
 * MonoGame's Android backend does not talk to the OS directly. It goes through
 * Context for its directories, AssetManager for content, and KeyEvent for
 * input. All three are Java types, so all three have to exist here before the
 * managed side can finish starting.
 *
 * The asset path is the one that matters for getting a frame on screen: every
 * texture, sound and font the game loads comes through AssetManager.open() or
 * the NDK AAsset API underneath it. Both are wired to the same directory on
 * the SD card so it does not matter which the engine picks.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <switch.h>

#include "android_classes.h"
#include "jni_arrays.h"
#include "android_pm.h"
#include "ime_shim.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "util.h"
#include "vfs.h"

#define ASSET_ROOT "sdmc:/switch/pvzultimate/assets"
/* Android paths, not sdmc: ones.
 *
 * These strings cross into managed code, and .NET parses them as Unix paths.
 * A leading "sdmc:" puts a colon at position four, which walked
 * Path.RemoveRelativeSegments off the end of its buffer and threw
 * IndexOutOfRangeException out of Directory.Exists -- from inside the game's
 * own storage-migration check, so it looked like a game bug rather than ours.
 *
 * vfs_translate already maps /data/data/<pkg>/... onto the data root, so
 * handing over the path an Android app would actually see costs nothing and
 * keeps every consumer happy: managed code parses it, and our file layer
 * resolves it. The device prefix belongs below the VFS, not above it. */
#define FILES_DIR  "/data/data/com.pvz.ultimate/files"
#define CACHE_DIR  "/data/data/com.pvz.ultimate/cache"
#define EXTERNAL_DIR "/data/data/com.pvz.ultimate/external"

/* ------------------------------------------------------------------------ */
/* java/lang/String helper                                                   */
/* ------------------------------------------------------------------------ */

/* Strings come from jni_arrays.c so every file agrees on the layout. */
#define make_string(s) ((jobject)jni_make_string(s))

/* ------------------------------------------------------------------------ */
/* java/io/File                                                              */
/* ------------------------------------------------------------------------ */

typedef struct { FakeObject hdr; char path[256]; } FileObj;
static FakeClass g_class_File;

static jvalue file_getAbsolutePath(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FileObj *f = (FileObj *)jniref_deref(self);
  r.l = make_string(f ? f->path : "");
  return r;
}

/* File had only the three accessors above, so exists(), mkdirs(),
 * isDirectory() and the rest were auto-stubbed to false and zero.
 *
 * That is not a harmless gap: a storage check asks whether its directory
 * exists, gets false, asks to create it, gets false again, and concludes the
 * device has no usable storage -- which is precisely the
 * "Android internal storage is unavailable" this port has been reporting.
 *
 * Every path goes through vfs_translate first, so the Android path the caller
 * holds resolves to the sdmc: location it actually lives at. */

/* Defined below; getParentFile needs it above that point. */
static jobject make_file(const char *path);

static const char *file_real(jobject self, char *buf, size_t n) {
  FileObj *f = (FileObj *)jniref_deref(self);
  if (!f) return NULL;
  return vfs_translate(f->path, buf, n);
}

static jvalue file_exists(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  char buf[512];
  const char *p = file_real(self, buf, sizeof(buf));
  struct stat st;
  r.z = (p && stat(p, &st) == 0) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue file_isDirectory(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  char buf[512];
  const char *p = file_real(self, buf, sizeof(buf));
  struct stat st;
  r.z = (p && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue file_isFile(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  char buf[512];
  const char *p = file_real(self, buf, sizeof(buf));
  struct stat st;
  r.z = (p && stat(p, &st) == 0 && S_ISREG(st.st_mode)) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue file_length(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  char buf[512];
  const char *p = file_real(self, buf, sizeof(buf));
  struct stat st;
  if (p && stat(p, &st) == 0) r.j = (jlong)st.st_size;
  return r;
}

/* mkdirs creates intermediate components; mkdir does not. Both report success
 * when the directory already exists, which is what callers actually mean. */
static int make_tree(char *path) {
  for (char *q = path + 1; *q; q++) {
    if (*q != '/') continue;
    *q = 0;
    mkdir(path, 0777);
    *q = '/';
  }
  return mkdir(path, 0777);
}

static jvalue file_mkdirs(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  char buf[512], work[512];
  const char *p = file_real(self, buf, sizeof(buf));
  if (!p) return r;

  struct stat st;
  if (stat(p, &st) == 0) { r.z = JNI_TRUE; return r; }

  snprintf(work, sizeof(work), "%s", p);
  make_tree(work);
  r.z = (stat(p, &st) == 0) ? JNI_TRUE : JNI_FALSE;
  if (!r.z) debug_log("[file] could not create %s\n", p);
  return r;
}

static jvalue file_delete(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  char buf[512];
  const char *p = file_real(self, buf, sizeof(buf));
  if (!p) return r;
  if (remove(p) == 0) { r.z = JNI_TRUE; return r; }
  r.z = (rmdir(p) == 0) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue file_getName(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FileObj *f = (FileObj *)jniref_deref(self);
  if (!f) return r;
  const char *slash = strrchr(f->path, '/');
  r.l = make_string(slash ? slash + 1 : f->path);
  return r;
}

static jvalue file_getParent(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  FileObj *f = (FileObj *)jniref_deref(self);
  if (!f) return r;
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", f->path);
  char *slash = strrchr(buf, '/');
  if (!slash || slash == buf) return r;
  *slash = 0;
  r.l = make_string(buf);
  return r;
}

static jvalue file_getParentFile(JNIEnv *e, jobject self, const jvalue *a) {
  jvalue r = file_getParent(e, self, a);
  if (r.l) {
    FakeString *s2 = (FakeString *)jniref_deref(r.l);
    r.l = (s2 && s2->utf) ? make_file(s2->utf) : NULL;
  }
  return r;
}

/* Writable, and with room. A storage check that believes the card is full
 * refuses to start just as surely as one that thinks it is missing. */
static jvalue file_true(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); r.z = JNI_TRUE; return r;
}
static jvalue file_space(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.j = 2ll * 1024 * 1024 * 1024;   /* 2 GB, plausible for an SD card */
  return r;
}

static FakeMethod g_file_methods[] = {
  { file_getAbsolutePath, "getAbsolutePath", "()Ljava/lang/String;", NULL, 0 },
  { file_getAbsolutePath, "getPath",         "()Ljava/lang/String;", NULL, 0 },
  { file_getAbsolutePath, "getCanonicalPath","()Ljava/lang/String;", NULL, 0 },
  { file_getAbsolutePath, "toString",        "()Ljava/lang/String;", NULL, 0 },
  { file_getName,         "getName",         "()Ljava/lang/String;", NULL, 0 },
  { file_getParent,       "getParent",       "()Ljava/lang/String;", NULL, 0 },
  { file_getParentFile,   "getParentFile",   "()Ljava/io/File;",     NULL, 0 },
  { file_exists,          "exists",          "()Z",                  NULL, 0 },
  { file_isDirectory,     "isDirectory",     "()Z",                  NULL, 0 },
  { file_isFile,          "isFile",          "()Z",                  NULL, 0 },
  { file_mkdirs,          "mkdirs",          "()Z",                  NULL, 0 },
  { file_mkdirs,          "mkdir",           "()Z",                  NULL, 0 },
  { file_delete,          "delete",          "()Z",                  NULL, 0 },
  { file_length,          "length",          "()J",                  NULL, 0 },
  { file_true,            "canRead",         "()Z",                  NULL, 0 },
  { file_true,            "canWrite",        "()Z",                  NULL, 0 },
  { file_true,            "setReadable",     "(Z)Z",                 NULL, 0 },
  { file_true,            "setWritable",     "(Z)Z",                 NULL, 0 },
  { file_space,           "getFreeSpace",    "()J",                  NULL, 0 },
  { file_space,           "getUsableSpace",  "()J",                  NULL, 0 },
  { file_space,           "getTotalSpace",   "()J",                  NULL, 0 },
};

static FakeClass g_class_File = {
  {NULL}, "java/io/File", NULL,
  g_file_methods, (int)(sizeof(g_file_methods)/sizeof(g_file_methods[0])), NULL, 0,
  sizeof(FileObj)
};

static jobject make_file(const char *path) {
  FileObj *f = calloc(1, sizeof(FileObj));
  f->hdr.cls = &g_class_File;
  snprintf(f->path, sizeof(f->path), "%s", path);
  return jniref_new(&f->hdr, REF_LOCAL);
}

/* ------------------------------------------------------------------------ */
/* AAsset -- the NDK side                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  FILE  *fp;
  long   length;
  void  *buffer;    /* populated only if AAsset_getBuffer is called */
} AAsset;

static FakeObject g_assetmgr_obj;
static FakeClass  g_class_AssetManager;

void *nx_AAssetManager_fromJava(void *env, void *obj) {
  (void)env; (void)obj;
  return &g_assetmgr_obj;   /* single manager; the token is not dereferenced */
}

void *nx_AAssetManager_open(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", ASSET_ROOT, filename);

  FILE *fp = locked_fopen(path, "rb");
  if (!fp) {
    debug_log("[asset] MISSING %s\n", filename);
    return NULL;
  }

  AAsset *a = calloc(1, sizeof(AAsset));
  a->fp = fp;
  locked_fseek(fp, 0, SEEK_END);
  a->length = locked_ftell(fp);
  locked_fseek(fp, 0, SEEK_SET);
  return a;
}

int nx_AAsset_read(void *asset, void *buf, size_t count) {
  AAsset *a = asset;
  if (!a || !a->fp) return -1;
  return (int)locked_fread(buf, 1, count, a->fp);
}

long nx_AAsset_seek(void *asset, long off, int whence) {
  AAsset *a = asset;
  if (!a || !a->fp) return -1;
  locked_fseek(a->fp, off, whence);
  return locked_ftell(a->fp);
}

long nx_AAsset_getLength(void *asset) {
  AAsset *a = asset;
  return a ? a->length : -1;
}

long nx_AAsset_getRemainingLength(void *asset) {
  AAsset *a = asset;
  if (!a || !a->fp) return 0;
  return a->length - locked_ftell(a->fp);
}

/* Some loaders prefer a whole-file pointer over streaming. Reading the file
 * into memory on demand is the only way to honour that here -- there is no
 * mmap-a-file path on this platform. */
const void *nx_AAsset_getBuffer(void *asset) {
  AAsset *a = asset;
  if (!a || !a->fp) return NULL;
  if (a->buffer) return a->buffer;
  a->buffer = malloc((size_t)a->length);
  if (!a->buffer) return NULL;
  long save = locked_ftell(a->fp);
  locked_fseek(a->fp, 0, SEEK_SET);
  locked_fread(a->buffer, 1, (size_t)a->length, a->fp);
  locked_fseek(a->fp, save, SEEK_SET);
  return a->buffer;
}

void nx_AAsset_close(void *asset) {
  AAsset *a = asset;
  if (!a) return;
  if (a->fp) locked_fclose(a->fp);
  free(a->buffer);
  free(a);
}

int nx_AAsset_isAllocated(void *asset) { AAsset *a = asset; return a && a->buffer; }

/* AAsset_openFileDescriptor is used for streaming audio. We cannot hand out a
 * real fd with an offset window, so report failure and let the engine fall
 * back to AAsset_read. If audio silently fails to stream, look here first. */
int nx_AAsset_openFileDescriptor(void *asset, long *start, long *length) {
  (void)asset; (void)start; (void)length;
  return -1;
}

/* ------------------------------------------------------------------------ */
/* android/content/res/AssetManager -- the Java side                         */
/* ------------------------------------------------------------------------ */

/* ---- java/io/InputStream over an asset --------------------------------
 * MonoGame's content pipeline normally reaches assets through the NDK AAsset
 * API, but the Java path exists and some loaders take it. Returning null there
 * produces a game that starts cleanly and renders nothing, which is a bad
 * failure to debug -- so it is implemented rather than stubbed. */

typedef struct { FakeObject hdr; void *asset; } StreamObj;
static FakeClass g_class_InputStream;

static jvalue is_read1(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  StreamObj *s = (StreamObj *)jniref_deref(self);
  unsigned char b;
  r.i = (s && nx_AAsset_read(s->asset, &b, 1) == 1) ? (jint)b : -1;
  return r;
}

/* read(byte[], int, int). Returns -1 at EOF, not 0 -- a loader looping until
 * -1 will spin forever on a 0. */
static jvalue is_read3(JNIEnv *e, jobject self, const jvalue *a) {
  jvalue r; memset(&r, 0, sizeof(r));
  StreamObj *s = (StreamObj *)jniref_deref(self);
  if (!s || !a) { r.i = -1; return r; }

  jbyteArray arr = (jbyteArray)a[0].l;
  jint off = a[1].i, len = a[2].i;
  if (len <= 0) { r.i = 0; return r; }

  jbyte *elems = (*e)->GetByteArrayElements(e, arr, NULL);
  if (!elems) { r.i = -1; return r; }
  int n = nx_AAsset_read(s->asset, elems + off, (size_t)len);
  (*e)->ReleaseByteArrayElements(e, arr, elems, 0);
  r.i = (n > 0) ? n : -1;
  return r;
}

static jvalue is_available(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  StreamObj *s = (StreamObj *)jniref_deref(self);
  r.i = s ? (jint)nx_AAsset_getRemainingLength(s->asset) : 0;
  return r;
}

static jvalue is_skip(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  jvalue r; memset(&r, 0, sizeof(r));
  StreamObj *s = (StreamObj *)jniref_deref(self);
  if (s && a) { nx_AAsset_seek(s->asset, (long)a[0].j, SEEK_CUR); r.j = a[0].j; }
  return r;
}

static jvalue is_close(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  StreamObj *s = (StreamObj *)jniref_deref(self);
  if (s && s->asset) { nx_AAsset_close(s->asset); s->asset = NULL; }
  return r;
}

static FakeMethod g_is_methods[] = {
  { is_close, "<init>", "()V", NULL, 0 },
  { is_read1,     "read",      "()I",     NULL, 0 },
  { is_read3,     "read",      "([BII)I", NULL, 0 },
  { is_available, "available", "()I",     NULL, 0 },
  { is_skip,      "skip",      "(J)J",    NULL, 0 },
  { is_close,     "close",     "()V",     NULL, 0 },
};

static FakeClass g_class_InputStream = {
  {NULL}, "java/io/InputStream", NULL,
  g_is_methods, (int)(sizeof(g_is_methods)/sizeof(g_is_methods[0])), NULL, 0
};

static jvalue am_open(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  jvalue r; memset(&r, 0, sizeof(r));
  FakeString *name = a ? (FakeString *)jniref_deref(a[0].l) : NULL;
  if (!name) return r;

  void *asset = nx_AAssetManager_open(NULL, name->utf, 0);
  if (!asset) return r;   /* null InputStream == FileNotFoundException */

  StreamObj *s = calloc(1, sizeof(StreamObj));
  s->hdr.cls = &g_class_InputStream;
  s->asset = asset;
  r.l = jniref_new(&s->hdr, REF_LOCAL);
  return r;
}

/* list(path) -> String[]. Returns an empty array rather than null: callers
 * iterate the result and a null dereferences. */
static jvalue am_list(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jni_make_object_array(0, NULL);
  return r;
}

static FakeMethod g_am_methods[] = {
  { am_open, "open", "(Ljava/lang/String;)Ljava/io/InputStream;",  NULL, 0 },
  { am_open, "open", "(Ljava/lang/String;I)Ljava/io/InputStream;", NULL, 0 },
  { am_list, "list", "(Ljava/lang/String;)[Ljava/lang/String;",    NULL, 0 },
};

static FakeClass g_class_AssetManager = {
  {NULL}, "android/content/res/AssetManager", NULL,
  g_am_methods, (int)(sizeof(g_am_methods)/sizeof(g_am_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* android/content/Context                                                   */
/* ------------------------------------------------------------------------ */

static FakeObject g_context_obj;
FakeClass         g_class_Context;   /* not static: Activity inherits it */

/* getExternalFilesDir takes a subdirectory type -- "Music", "Pictures" and so
 * on -- or null for the root. Android returns a distinct directory per type;
 * so do we, and creating it is the caller's business exactly as it is there. */
static jvalue ctx_getExternalFilesDir(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));

  const char *type = NULL;
  if (a && a[0].l) {
    FakeString *fs = (FakeString *)jniref_deref(a[0].l);
    if (fs && fs->utf && fs->utf[0]) type = fs->utf;
  }

  char path[320];
  if (type) snprintf(path, sizeof(path), EXTERNAL_DIR "/%s", type);
  else      snprintf(path, sizeof(path), EXTERNAL_DIR);
  r.l = make_file(path);
  return r;
}

static jvalue ctx_getExternalCacheDir(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = make_file(EXTERNAL_DIR "/cache");
  return r;
}

/* getDir(name, mode) is app-private storage under a prefixed directory. */
static jvalue ctx_getDir(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  const char *name = "dir";
  if (a && a[0].l) {
    FakeString *fs = (FakeString *)jniref_deref(a[0].l);
    if (fs && fs->utf && fs->utf[0]) name = fs->utf;
  }
  char path[320];
  snprintf(path, sizeof(path), "/data/data/com.pvz.ultimate/app_%s", name);
  r.l = make_file(path);
  return r;
}

/* PERMISSION_GRANTED is 0. Everything the game could ask for is available on a
 * console, so refusing would only make it take an error path for no reason. */
static jvalue ctx_checkSelfPermission(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.i = 0;
  return r;
}

static jvalue ctx_getFilesDir(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); r.l = make_file(FILES_DIR); return r;
}
static jvalue ctx_getCacheDir(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); r.l = make_file(CACHE_DIR); return r;
}
static jvalue ctx_getPackageName(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = make_string("com.pvz.ultimate");
  return r;
}
static jvalue ctx_getAssets(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jniref_new(&g_assetmgr_obj, REF_LOCAL);
  return r;
}
/* ------------------------------------------------------------------------ */
/* android/content/SharedPreferences (+ $Editor)                             */
/* ------------------------------------------------------------------------ */
/*
 * The game reads its settings through this during construction:
 *
 *     lock (_lock) using (var p = GetPrefs())
 *         if (p.Contains(key)) value = p.GetInt(key, 0);
 *
 * and it dereferences the prefs object with no null check, so a null here is
 * fatal rather than merely empty. Returning a real object with an empty store
 * is also the correct answer for a first launch: Contains() says no and the
 * game takes its defaults, which is exactly what Android does before anything
 * has been saved.
 *
 * The store PERSISTS now, which it did not until this round. The note that used
 * to sit here said the gap was functional rather than a boot blocker and named
 * the two places to fix it -- prefs_commit() and android_classes_init() -- and
 * that is exactly where it was fixed.
 *
 * It matters more than "settings will not persist" suggested. The .dat files
 * the game writes through System.IO were surviving fine: users.dat and
 * user1.dat came off the SD card intact, valid MessagePack, with a name that
 * had been typed in and a level number in them. What was not surviving was
 * everything the game keeps HERE -- and a level-select that opens with no
 * progress is the visible half of that.
 *
 * One store serves every name passed to getSharedPreferences. Android would
 * give one file per name; nothing in this game has been seen to use more than
 * one, and merging them is safe as long as the keys do not collide. If a second
 * name ever appears the file format below already carries the key, so splitting
 * it later is a small change.
 *
 * Editor methods return the editor, not void -- Android chains them
 * (edit().putInt(..).putString(..).apply()), and each link is declared to
 * return an object, so a null would fault at the next call in the chain.
 */

#define PREF_MAX 256

typedef enum { PREF_UNSET = 0, PREF_INT, PREF_LONG, PREF_FLOAT, PREF_BOOL,
               PREF_STRING } PrefKind;

typedef struct {
  char     key[128];
  PrefKind kind;
  int64_t  i;      /* int, long and bool all live here */
  float    f;
  char     s[256];
} PrefEntry;

static PrefEntry g_prefs[PREF_MAX];
static int       g_nprefs;
static int       g_prefs_dirty;

/* Translated through the VFS rather than hardcoded.
 *
 * "sdmc:/switch/pvzultimate" was already spelled out in vfs.c and main.c, and
 * writing it a third time here means a data root that moves takes the
 * preferences with it silently. FILES_DIR is the path the GAME sees; the VFS
 * maps it to wherever the host actually keeps files, which is the same mapping
 * every save the game writes goes through -- so the preferences land beside
 * them rather than somewhere only this file knows about. */
#define PREFS_VPATH FILES_DIR "/shared_prefs.txt"

static const char *prefs_path(void) {
  static char resolved[512];
  if (!resolved[0]) vfs_translate(PREFS_VPATH, resolved, sizeof(resolved));
  return resolved;
}

/* One line per entry: kind<TAB>key<TAB>value.
 *
 * Text rather than a binary blob, because when a save does not come back the
 * first question is "what did it actually store", and that should be answerable
 * by opening the file. Tabs and newlines inside a string value are escaped so a
 * value can never be mistaken for a record boundary. */
static void pref_escape(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const char *p = in; *p && o + 3 < n; p++) {
    if      (*p == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
    else if (*p == '\t') { out[o++] = '\\'; out[o++] = 't';  }
    else if (*p == '\n') { out[o++] = '\\'; out[o++] = 'n';  }
    else                 out[o++] = *p;
  }
  out[o] = '\0';
}

static void pref_unescape(char *sptr) {
  char *w = sptr;
  for (char *p = sptr; *p; p++) {
    if (*p != '\\') { *w++ = *p; continue; }
    p++;
    if      (*p == 't')  *w++ = '\t';
    else if (*p == 'n')  *w++ = '\n';
    else if (*p == '\0') { break; }
    else                 *w++ = *p;
  }
  *w = '\0';
}

static void prefs_save(void) {
  if (!g_prefs_dirty) return;

  FILE *f = locked_fopen(prefs_path(), "w");
  if (!f) {
    debug_log("[prefs] could not open %s for writing -- progress kept here "
              "will not survive a restart\n", prefs_path());
    return;
  }

  int written = 0;
  /* Worst case is every byte escaped: a 256-byte value needs 513, a 128-byte
     key needs 257. Sized so nothing is silently truncated. */
  char esc[520], kesc[264];
  for (int i = 0; i < g_nprefs; i++) {
    PrefEntry *p = &g_prefs[i];
    if (p->kind == PREF_UNSET) continue;

    /* The KEY is escaped too, not just the value.
     *
     * A round-trip test with a tab in the key caught this: the loader splits on
     * tabs, so an unescaped one in the key silently truncates the key and
     * misreads the rest of the line as the value. Preference keys are normally
     * plain identifiers, which is exactly why it would have sat there. */
    pref_escape(p->key, kesc, sizeof(kesc));

    char line[1024];
    int n;
    if (p->kind == PREF_STRING) {
      pref_escape(p->s, esc, sizeof(esc));
      n = snprintf(line, sizeof(line), "%d\t%s\t%s\n", (int)p->kind, kesc, esc);
    } else if (p->kind == PREF_FLOAT) {
      n = snprintf(line, sizeof(line), "%d\t%s\t%.9g\n", (int)p->kind, kesc, (double)p->f);
    } else {
      n = snprintf(line, sizeof(line), "%d\t%s\t%lld\n", (int)p->kind, kesc,
                   (long long)p->i);
    }
    if (n > 0) { locked_fwrite(line, 1, (size_t)n, f); written++; }
  }
  locked_fclose(f);
  g_prefs_dirty = 0;
  debug_log("[prefs] wrote %d entries to %s\n", written, prefs_path());
}

static void prefs_load(void) {
  FILE *f = locked_fopen(prefs_path(), "r");
  if (!f) {
    debug_log("[prefs] no stored preferences yet (%s); starting empty, which "
              "is what Android does before anything has been saved\n",
              prefs_path());
    return;
  }

  char line[1024];
  int loaded = 0;
  while (fgets(line, sizeof(line), f)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    char *t1 = strchr(line, '\t');
    if (!t1) continue;
    *t1++ = '\0';
    char *t2 = strchr(t1, '\t');
    if (!t2) continue;
    *t2++ = '\0';

    int kind = atoi(line);
    if (kind <= PREF_UNSET || kind > PREF_STRING) continue;
    if (g_nprefs >= PREF_MAX) break;

    PrefEntry *p = &g_prefs[g_nprefs++];
    memset(p, 0, sizeof(*p));
    pref_unescape(t1);
    snprintf(p->key, sizeof(p->key), "%s", t1);
    p->kind = (PrefKind)kind;
    if (kind == PREF_STRING) {
      pref_unescape(t2);
      snprintf(p->s, sizeof(p->s), "%s", t2);
    } else if (kind == PREF_FLOAT) {
      p->f = (float)atof(t2);
    } else {
      p->i = (int64_t)strtoll(t2, NULL, 10);
    }
    loaded++;
  }
  locked_fclose(f);
  g_prefs_dirty = 0;
  debug_log("[prefs] loaded %d entries from %s\n", loaded, prefs_path());
}

void android_prefs_flush(void) { prefs_save(); }

static PrefEntry *pref_find(const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < g_nprefs; i++)
    if (!strcmp(g_prefs[i].key, key)) return &g_prefs[i];
  return NULL;
}

static PrefEntry *pref_slot(const char *key) {
  g_prefs_dirty = 1;
  PrefEntry *p = pref_find(key);
  if (p) return p;
  if (g_nprefs >= PREF_MAX) {
    debug_log("[prefs] out of slots at '%s' -- raise PREF_MAX\n", key);
    return NULL;
  }
  p = &g_prefs[g_nprefs++];
  snprintf(p->key, sizeof(p->key), "%s", key);
  return p;
}

/* The key argument, as a C string. */
static const char *pref_key(const jvalue *a, int idx) {
  if (!a || !a[idx].l) return NULL;
  FakeString *fs = (FakeString *)jniref_deref(a[idx].l);
  return (fs && fs->utf) ? fs->utf : NULL;
}

static jvalue prefs_contains(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  PrefEntry *p = pref_find(pref_key(a, 0));
  r.z = (p && p->kind != PREF_UNSET) ? JNI_TRUE : JNI_FALSE;
  return r;
}

static jvalue prefs_getInt(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  PrefEntry *p = pref_find(pref_key(a, 0));
  r.i = (p && p->kind == PREF_INT) ? (jint)p->i : (a ? a[1].i : 0);
  return r;
}

static jvalue prefs_getLong(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  PrefEntry *p = pref_find(pref_key(a, 0));
  r.j = (p && p->kind == PREF_LONG) ? (jlong)p->i : (a ? a[1].j : 0);
  return r;
}

static jvalue prefs_getFloat(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  PrefEntry *p = pref_find(pref_key(a, 0));
  r.f = (p && p->kind == PREF_FLOAT) ? p->f : (a ? a[1].f : 0.0f);
  return r;
}

static jvalue prefs_getBoolean(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  PrefEntry *p = pref_find(pref_key(a, 0));
  r.z = (p && p->kind == PREF_BOOL) ? (jboolean)(p->i != 0)
                                    : (a ? a[1].z : JNI_FALSE);
  return r;
}

/* Absent means "return the caller's default", which may legitimately be null.
 *
 * The default is COPIED rather than handed straight back: a[1].l belongs to the
 * caller, and returning it would let the caller's wrapper delete a handle the
 * caller still holds. */
static jvalue prefs_getString(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  PrefEntry *p = pref_find(pref_key(a, 0));
  if (p && p->kind == PREF_STRING) {
    r.l = make_string(p->s);
  } else {
    const char *def = pref_key(a, 1);
    if (def) r.l = make_string(def);
  }
  return r;
}

static FakeObject g_editor_obj;

static jvalue prefs_edit(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jniref_new(&g_editor_obj, REF_LOCAL);
  return r;
}

static FakeMethod g_prefs_methods[] = {
  { prefs_contains,   "contains",   "(Ljava/lang/String;)Z",                    NULL, 0 },
  { prefs_getInt,     "getInt",     "(Ljava/lang/String;I)I",                   NULL, 0 },
  { prefs_getLong,    "getLong",    "(Ljava/lang/String;J)J",                   NULL, 0 },
  { prefs_getFloat,   "getFloat",   "(Ljava/lang/String;F)F",                   NULL, 0 },
  { prefs_getBoolean, "getBoolean", "(Ljava/lang/String;Z)Z",                   NULL, 0 },
  { prefs_getString,  "getString",  "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", NULL, 0 },
  { prefs_edit,       "edit",       "()Landroid/content/SharedPreferences$Editor;", NULL, 0 },
};

static FakeClass g_class_Prefs = {
  {NULL}, "android/content/SharedPreferences", NULL,
  g_prefs_methods, (int)(sizeof(g_prefs_methods)/sizeof(g_prefs_methods[0])),
  NULL, 0, 0
};

static FakeObject g_prefs_obj;

/* Every put returns the editor so the caller can chain.
 *
 * A FRESH local ref, not the incoming `self` handle. Returning `self` would
 * hand back a reference the caller already owns, and Mono.Android wraps a
 * returned object with TransferLocalRef and deletes it -- which would free the
 * caller's own handle to the editor out from under it, mid-chain. */
static jvalue editor_self(jobject s) {
  (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jniref_new(&g_editor_obj, REF_LOCAL);
  return r;
}

static jvalue ed_putInt(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e;
  PrefEntry *p = pref_slot(pref_key(a, 0));
  if (p) { p->kind = PREF_INT;  p->i = a[1].i; }
  return editor_self(s);
}
static jvalue ed_putLong(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e;
  PrefEntry *p = pref_slot(pref_key(a, 0));
  if (p) { p->kind = PREF_LONG; p->i = a[1].j; }
  return editor_self(s);
}
static jvalue ed_putFloat(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e;
  PrefEntry *p = pref_slot(pref_key(a, 0));
  if (p) { p->kind = PREF_FLOAT; p->f = a[1].f; }
  return editor_self(s);
}
static jvalue ed_putBoolean(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e;
  PrefEntry *p = pref_slot(pref_key(a, 0));
  if (p) { p->kind = PREF_BOOL; p->i = a[1].z ? 1 : 0; }
  return editor_self(s);
}
static jvalue ed_putString(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e;
  PrefEntry *p = pref_slot(pref_key(a, 0));
  if (p) {
    p->kind = PREF_STRING;
    const char *v = pref_key(a, 1);          /* same shape: a jstring arg */
    snprintf(p->s, sizeof(p->s), "%s", v ? v : "");
  }
  return editor_self(s);
}
static jvalue ed_remove(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e;
  PrefEntry *p = pref_find(pref_key(a, 0));
  if (p) { p->kind = PREF_UNSET; g_prefs_dirty = 1; }
  return editor_self(s);
}
static jvalue ed_clear(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)a;
  for (int i = 0; i < g_nprefs; i++) g_prefs[i].kind = PREF_UNSET;
  g_prefs_dirty = 1;
  return editor_self(s);
}
/* commit() and apply() are where the store reaches the disk.
 *
 * The puts have already updated memory; these are the points Android defines as
 * "make it durable". commit() is synchronous and reports success, apply() is
 * asynchronous and returns void -- writing synchronously in both is the safe
 * reading of that, and the file is a few hundred bytes.
 *
 * prefs_save() is a no-op when nothing changed, so a game that calls apply()
 * every frame does not touch the SD card every frame. */
static jvalue ed_commit(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  prefs_save();
  r.z = JNI_TRUE;
  return r;
}
static jvalue ed_apply(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  prefs_save();
  return r;
}

static FakeMethod g_editor_methods[] = {
  { ed_putInt,     "putInt",     "(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", NULL, 0 },
  { ed_putLong,    "putLong",    "(Ljava/lang/String;J)Landroid/content/SharedPreferences$Editor;", NULL, 0 },
  { ed_putFloat,   "putFloat",   "(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;", NULL, 0 },
  { ed_putBoolean, "putBoolean", "(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;", NULL, 0 },
  { ed_putString,  "putString",  "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", NULL, 0 },
  { ed_remove,     "remove",     "(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", NULL, 0 },
  { ed_clear,      "clear",      "()Landroid/content/SharedPreferences$Editor;", NULL, 0 },
  { ed_commit,     "commit",     "()Z", NULL, 0 },
  { ed_apply,      "apply",      "()V", NULL, 0 },
};

static FakeClass g_class_PrefsEditor = {
  {NULL}, "android/content/SharedPreferences$Editor", NULL,
  g_editor_methods, (int)(sizeof(g_editor_methods)/sizeof(g_editor_methods[0])),
  NULL, 0, 0
};

/* ------------------------------------------------------------------------ */
/* android/app/AlertDialog$Builder                                           */
/* ------------------------------------------------------------------------ */
/*
 * The game reports fatal errors by building an AlertDialog. Every Builder
 * method returns the Builder so calls can be chained --
 *
 *     new Builder(ctx).setTitle(t).setMessage(m).setPositiveButton(...).show()
 *
 * -- so a stub returning null kills the chain on the very first link. That is
 * what happened: the game caught "Failed to load reanim from RSB", went to
 * display it, and died inside setTitle before anything reached the screen.
 *
 * There is no UI to show a dialog on, so this is a logging sink: the chain
 * completes, and the title and message go to the log. That is strictly better
 * than a working dialog would be here -- the text is the diagnosis, and a
 * dialog on a console with no touch input would just be a wall.
 *
 * Every method returns a FRESH local reference to the same object rather than
 * the incoming `self` handle. Mono.Android wraps a returned object with
 * TransferLocalRef and deletes it; handing back the caller's own handle would
 * free it mid-chain. Same trap as the SharedPreferences Editor.
 */
static FakeObject g_dialog_builder_obj;
static FakeObject g_dialog_obj;

static const char *charseq_text(const jvalue *a, int idx) {
  if (!a || !a[idx].l) return NULL;
  FakeString *fs = (FakeString *)jniref_deref(a[idx].l);
  return (fs && fs->utf) ? fs->utf : NULL;
}

static jvalue builder_self(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jniref_new(&g_dialog_builder_obj, REF_LOCAL);
  return r;
}

static jvalue builder_setTitle(JNIEnv *e, jobject s, const jvalue *a) {
  const char *t = charseq_text(a, 0);
  debug_log("\n*** THE GAME IS REPORTING AN ERROR ***\n    title:   %s\n",
            t ? t : "(null)");
  return builder_self(e, s, a);
}

static jvalue builder_setMessage(JNIEnv *e, jobject s, const jvalue *a) {
  const char *t = charseq_text(a, 0);
  debug_log("    message: %s\n\n", t ? t : "(null)");
  return builder_self(e, s, a);
}

static jvalue builder_create(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jniref_new(&g_dialog_obj, REF_LOCAL);
  return r;
}

static jvalue dialog_nop(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); return r;
}

static FakeMethod g_builder_methods[] = {
  { builder_setTitle,   "setTitle",   "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_setTitle,   "setTitle",   "(I)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_setMessage, "setMessage", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_setMessage, "setMessage", "(I)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setPositiveButton", "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setNegativeButton", "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setNeutralButton",  "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setCancelable",     "(Z)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setOnCancelListener","(Landroid/content/DialogInterface$OnCancelListener;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setOnDismissListener","(Landroid/content/DialogInterface$OnDismissListener;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setView",           "(Landroid/view/View;)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_self, "setIcon",           "(I)Landroid/app/AlertDialog$Builder;", NULL, 0 },
  { builder_create, "create",          "()Landroid/app/AlertDialog;", NULL, 0 },
  { builder_create, "show",            "()Landroid/app/AlertDialog;", NULL, 0 },
  { dialog_nop,     "getContext",      "()Landroid/content/Context;", NULL, 0 },
};

static FakeClass g_class_AlertDialogBuilder = {
  {NULL}, "android/app/AlertDialog$Builder", NULL,
  g_builder_methods,
  (int)(sizeof(g_builder_methods)/sizeof(g_builder_methods[0])),
  NULL, 0, 0
};

/* android/widget/Button, for the error dialog's buttons.
 *
 * AlertDialog.getButton(int) returns a Button and the game configures it, so a
 * null here kills the chain the same way the Builder methods did -- and this
 * one is NOT the fluent case, because the declared return type is a different
 * class, which is exactly why the generic fluent rule correctly did not fire.
 *
 * Void configuration methods do not need to do anything: there is no dialog on
 * screen. The object only has to exist so the game can finish its own error
 * handling instead of dying inside it. */
static FakeObject g_dialog_button_obj;

static jvalue button_text(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = make_string("");
  return r;
}

static FakeMethod g_button_methods[] = {
  { dialog_nop, "setEnabled",         "(Z)V", NULL, 0 },
  { dialog_nop, "setVisibility",      "(I)V", NULL, 0 },
  { dialog_nop, "setText",            "(Ljava/lang/CharSequence;)V", NULL, 0 },
  { dialog_nop, "setText",            "(I)V", NULL, 0 },
  { dialog_nop, "setTextColor",       "(I)V", NULL, 0 },
  { dialog_nop, "setAllCaps",         "(Z)V", NULL, 0 },
  { dialog_nop, "setOnClickListener", "(Landroid/view/View$OnClickListener;)V", NULL, 0 },
  { dialog_nop, "setContentDescription","(Ljava/lang/CharSequence;)V", NULL, 0 },
  { button_text,"getText",            "()Ljava/lang/CharSequence;", NULL, 0 },
};

static FakeClass g_class_Button = {
  {NULL}, "android/widget/Button", NULL,
  g_button_methods,
  (int)(sizeof(g_button_methods)/sizeof(g_button_methods[0])),
  NULL, 0, 0
};

static jvalue dialog_getButton(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jniref_new(&g_dialog_button_obj, REF_LOCAL);
  return r;
}

static FakeMethod g_dialog_methods[] = {
  { dialog_nop, "show",    "()V", NULL, 0 },
  { dialog_nop, "dismiss", "()V", NULL, 0 },
  { dialog_nop, "cancel",  "()V", NULL, 0 },
  { dialog_getButton, "getButton", "(I)Landroid/widget/Button;", NULL, 0 },
};

static FakeClass g_class_AlertDialog = {
  {NULL}, "android/app/AlertDialog", NULL,
  g_dialog_methods,
  (int)(sizeof(g_dialog_methods)/sizeof(g_dialog_methods[0])),
  NULL, 0, 0
};

/* ------------------------------------------------------------------------ */
/* android/media/AudioManager                                                */
/* ------------------------------------------------------------------------ */
/*
 * getSystemService returned null for everything and never said what had been
 * asked for, so "[Lawn] Unable to obtain AudioManager" was the only clue that
 * a service was missing at all, and there was no way to tell which others were
 * being requested and silently refused.
 *
 * Audio is the one that clearly matters: the engine asked for the AudioManager
 * during audio setup and again during the callback, and the hang is in that
 * area. requestAudioFocus is the specific risk -- an engine that waits for
 * focus it never receives waits forever, and null here means it cannot even
 * ask. AUDIOFOCUS_REQUEST_GRANTED is the honest answer on a console: nothing
 * else is competing for the output.
 */
#define AUDIOFOCUS_REQUEST_GRANTED 1
#define STREAM_MUSIC_MAX_VOLUME    15

static jvalue am_requestAudioFocus(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.i = AUDIOFOCUS_REQUEST_GRANTED;
  return r;
}
static jvalue am_abandonAudioFocus(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.i = AUDIOFOCUS_REQUEST_GRANTED;
  return r;
}
static jvalue am_getStreamMaxVolume(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.i = STREAM_MUSIC_MAX_VOLUME;
  return r;
}
static jvalue am_getStreamVolume(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.i = STREAM_MUSIC_MAX_VOLUME;   /* full; the console owns real volume */
  return r;
}
static jvalue am_isMusicActive(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.z = JNI_FALSE;
  return r;
}
/* getProperty returns a String; null is legal and means "unknown", which the
 * framework itself does on plenty of devices. The two the engine is likely to
 * ask for are answered concretely so it does not have to guess. */
static jvalue am_getProperty(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));
  const char *key = NULL;
  if (a && a[0].l) {
    FakeString *fs = (FakeString *)jniref_deref(a[0].l);
    if (fs && fs->utf) key = fs->utf;
  }
  if (key && strstr(key, "OUTPUT_SAMPLE_RATE"))       r.l = make_string("48000");
  else if (key && strstr(key, "OUTPUT_FRAMES_PER_BUFFER")) r.l = make_string("512");
  else debug_log("[am] getProperty(%s) -> null\n", key ? key : "(null)");
  return r;
}

static FakeMethod g_audiomanager_methods[] = {
  { am_requestAudioFocus, "requestAudioFocus",
    "(Landroid/media/AudioManager$OnAudioFocusChangeListener;II)I", NULL, 0 },
  { am_abandonAudioFocus, "abandonAudioFocus",
    "(Landroid/media/AudioManager$OnAudioFocusChangeListener;)I", NULL, 0 },
  { am_getStreamMaxVolume, "getStreamMaxVolume", "(I)I", NULL, 0 },
  { am_getStreamVolume,    "getStreamVolume",    "(I)I", NULL, 0 },
  { am_isMusicActive,      "isMusicActive",      "()Z",  NULL, 0 },
  { am_getProperty,        "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", NULL, 0 },
};

static FakeClass g_class_AudioManager = {
  {NULL}, "android/media/AudioManager", NULL,
  g_audiomanager_methods,
  (int)(sizeof(g_audiomanager_methods)/sizeof(g_audiomanager_methods[0])),
  NULL, 0, 0
};

static FakeObject g_audiomanager_obj;

/* Says WHAT was asked for, always. A null return here is a legitimate Android
 * answer -- getSystemService returns null for a service the device does not
 * have -- but it has to be visible, because an engine that then dereferences
 * it dies at an address with nothing to do with this call. */
static jvalue ctx_getSystemService(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s;
  jvalue r; memset(&r, 0, sizeof(r));

  const char *name = NULL;
  if (a && a[0].l) {
    FakeString *fs = (FakeString *)jniref_deref(a[0].l);
    if (fs && fs->utf) name = fs->utf;
  }

  if (name && !strcmp(name, "audio")) {
    r.l = jniref_new(&g_audiomanager_obj, REF_LOCAL);
    debug_log("[ctx] getSystemService(audio) -> AudioManager\n");
    return r;
  }

  if (name && !strcmp(name, "input_method")) {
    r.l = ime_input_method_manager();
    debug_log("[ctx] getSystemService(input_method) -> InputMethodManager "
              "(backed by the Switch keyboard)\n");
    return r;
  }

  debug_log("[ctx] getSystemService(%s) -> null (not implemented). If a crash "
            "or hang follows, this is the service to add.\n",
            name ? name : "(null)");
  return r;
}

static jvalue ctx_getSharedPreferences(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = jniref_new(&g_prefs_obj, REF_LOCAL);
  return r;
}

/* ------------------------------------------------------------------------ */
/* android/os/Build$VERSION                                                  */
/* ------------------------------------------------------------------------ */
/*
 * SDK_INT was auto-stubbing to 0, which is not merely vague -- it contradicts
 * what the runtime already believes. .NET's own OS check
 * (OperatingSystem.IsAndroidVersionAtLeast) tests against 29 and passes, so
 * managed code thinks it is on Android 10 while Build.VERSION.SDK_INT says
 * "no version at all". Code that gates a feature on SDK_INT then takes a path
 * inconsistent with the one .NET picked, and 0 fails every `>= N` test there
 * is -- including for APIs we do support.
 *
 * 29 rather than something newer: it is the value .NET is already asserting,
 * and picking a higher one would enable APIs nothing here implements.
 */
static FakeField g_buildversion_fields[] = {
  { "SDK_INT", "I", NULL, NULL, {0} },     /* set to 29 in the init below */
  { "RELEASE", "Ljava/lang/String;", NULL, NULL, {0} },
};

static FakeClass g_class_BuildVersion = {
  {NULL}, "android/os/Build$VERSION", NULL,
  NULL, 0,
  g_buildversion_fields,
  (int)(sizeof(g_buildversion_fields)/sizeof(g_buildversion_fields[0])), 0
};

static jvalue ctx_null(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r)); return r;
}

static jvalue ctx_getPackageManager(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = android_package_manager();
  return r;
}

static jvalue ctx_getApplicationInfo(JNIEnv *e, jobject s, const jvalue *a) {
  (void)e; (void)s; (void)a;
  jvalue r; memset(&r, 0, sizeof(r));
  r.l = android_application_info();
  return r;
}

static FakeMethod g_ctx_methods[] = {
  { ctx_getFilesDir,    "getFilesDir",        "()Ljava/io/File;",   NULL, 0 },
  { ctx_getCacheDir,    "getCacheDir",        "()Ljava/io/File;",   NULL, 0 },
  { ctx_getPackageName, "getPackageName",     "()Ljava/lang/String;", NULL, 0 },
  { ctx_getAssets,      "getAssets",          "()Landroid/content/res/AssetManager;", NULL, 0 },
  { ctx_getExternalFilesDir,  "getExternalFilesDir",  "(Ljava/lang/String;)Ljava/io/File;", NULL, 0 },
  { ctx_getExternalCacheDir,  "getExternalCacheDir",  "()Ljava/io/File;",                   NULL, 0 },
  { ctx_getDir,               "getDir",               "(Ljava/lang/String;I)Ljava/io/File;",NULL, 0 },
  { ctx_checkSelfPermission,  "checkSelfPermission",  "(Ljava/lang/String;)I",              NULL, 0 },
  { ctx_null,           "getClassLoader",     "()Ljava/lang/ClassLoader;", NULL, 0 },
  { ctx_getPackageManager, "getPackageManager",
    "()Landroid/content/pm/PackageManager;", NULL, 0 },
  /* Was ctx_null. Same landmine as getPackageManager, one step further on:
     an Advanced/About screen that gets past the manager reads this next. */
  { ctx_getApplicationInfo, "getApplicationInfo",
    "()Landroid/content/pm/ApplicationInfo;", NULL, 0 },
  { ctx_null,           "getResources",       "()Landroid/content/res/Resources;", NULL, 0 },
  { ctx_getSystemService, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", NULL, 0 },
  { ctx_getSharedPreferences, "getSharedPreferences",
    "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", NULL, 0 },
  { ctx_null,           "registerReceiver",   "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;", NULL, 0 },
};

FakeClass g_class_Context = {
  {NULL}, "android/content/Context", NULL,
  g_ctx_methods, (int)(sizeof(g_ctx_methods)/sizeof(g_ctx_methods[0])), NULL, 0
};

/* ------------------------------------------------------------------------ */
/* android/view/KeyEvent                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
  FakeObject hdr;
  int action, keycode, unicode, repeat, meta;
} KeyEventObj;

static KeyEventObj g_keyevent;
static FakeClass   g_class_KeyEvent;
static jobject     g_ref_keyevent;

#define KE_SELF() KeyEventObj *k = (KeyEventObj *)jniref_deref(self); \
                  jvalue r; memset(&r, 0, sizeof(r)); if (!k) return r;

static jvalue ke_getAction(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; KE_SELF(); r.i = k->action; return r;
}
static jvalue ke_getKeyCode(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; KE_SELF(); r.i = k->keycode; return r;
}
static jvalue ke_getUnicodeChar(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; KE_SELF(); r.i = k->unicode; return r;
}
static jvalue ke_getRepeatCount(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; KE_SELF(); r.i = k->repeat; return r;
}
static jvalue ke_getMetaState(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a; KE_SELF(); r.i = k->meta; return r;
}

static FakeMethod g_ke_methods[] = {
  { ke_getAction,      "getAction",      "()I", NULL, 0 },
  { ke_getKeyCode,     "getKeyCode",     "()I", NULL, 0 },
  { ke_getUnicodeChar, "getUnicodeChar", "()I", NULL, 0 },
  { ke_getUnicodeChar, "getUnicodeChar", "(I)I", NULL, 0 },
  { ke_getRepeatCount, "getRepeatCount", "()I", NULL, 0 },
  { ke_getMetaState,   "getMetaState",   "()I", NULL, 0 },
};

static FakeClass g_class_KeyEvent = {
  {NULL}, "android/view/KeyEvent", NULL,
  g_ke_methods, (int)(sizeof(g_ke_methods)/sizeof(g_ke_methods[0])), NULL, 0
};

jobject android_make_key_event(int action, int keycode, int unicode) {
  return android_make_key_event_meta(action, keycode, unicode, 0);
}

/* `meta` was always a field on the fake KeyEvent and was always written as 0.
 * Software-keyboard text needs it: a game that reads getKeyCode() rather than
 * getUnicodeChar() has no other way to tell A from a. */
jobject android_make_key_event_meta(int action, int keycode, int unicode,
                                    int meta) {
  g_keyevent.action  = action;
  g_keyevent.keycode = keycode;
  g_keyevent.unicode = unicode;
  g_keyevent.repeat  = 0;
  g_keyevent.meta    = meta;
  return g_ref_keyevent;
}

/* ------------------------------------------------------------------------ */

jobject android_get_context(void) {
  return jniref_new(&g_context_obj, REF_GLOBAL);
}

void android_classes_init(void) {
  /* Before anything can read a preference. */
  prefs_load();

  jni_register_class(&g_class_String);
  jni_register_class(&g_class_File);
  jni_register_class(&g_class_AssetManager);
  jni_register_class(&g_class_Context);
  jni_register_class(&g_class_BuildVersion);
  jni_register_class(&g_class_AudioManager);
  jni_register_class(&g_class_AlertDialogBuilder);
  jni_register_class(&g_class_AlertDialog);
  jni_register_class(&g_class_Button);
  jni_register_class(&g_class_Prefs);
  jni_register_class(&g_class_PrefsEditor);
  jni_register_class(&g_class_KeyEvent);
  jni_register_class(&g_class_InputStream);

  g_assetmgr_obj.cls = &g_class_AssetManager;
  g_context_obj.cls  = &g_class_Context;
  /* Set here rather than in the initialiser: make_string needs the String
   * class registered, which happens above. */
  g_buildversion_fields[0].value.i = 29;
  g_buildversion_fields[1].value.l = make_string("10");

  g_audiomanager_obj.cls = &g_class_AudioManager;
  g_dialog_builder_obj.cls = &g_class_AlertDialogBuilder;
  g_dialog_obj.cls         = &g_class_AlertDialog;
  g_dialog_button_obj.cls  = &g_class_Button;
  g_prefs_obj.cls    = &g_class_Prefs;
  g_editor_obj.cls   = &g_class_PrefsEditor;
  g_keyevent.hdr.cls = &g_class_KeyEvent;

  g_ref_keyevent = jniref_new(&g_keyevent.hdr, REF_GLOBAL);

  /* Report whether assets are actually present. A missing directory here is
   * the single most common reason a port reaches a black screen and stays
   * there -- the game starts fine and then finds nothing to draw. */
  void *d = opendir(ASSET_ROOT);
  if (d) { closedir(d); debug_log("[asset] root %s present\n", ASSET_ROOT); }
  else    debug_log("[asset] *** %s NOT FOUND -- the game will start and "
                    "render nothing ***\n", ASSET_ROOT);

  debug_log("[android] framework classes registered\n");
}
