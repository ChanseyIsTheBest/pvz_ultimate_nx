/* dl_shim.c -- libdl replacement.
 *
 * This is the piece none of the reference ports needed. GameMaker and Unity
 * call dlopen/dlsym and nothing else; NativeAOT additionally calls
 * dl_iterate_phdr() and dladdr() to locate its OWN image at startup, because
 * the runtime has to find the module descriptor in __modules and the unwind
 * data in .dotnet_eh_table. A hand-mapped image is invisible to the real
 * implementations, so if these return nothing the runtime concludes it is not
 * loaded and dies inside initialization -- before any of your other shims get
 * a chance to run.
 *
 * Expect this file to be exercised at Stage 3 and to be the reason Stage 3
 * fails the first several times. The logging is deliberately loud.
 */

#include <elf.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dl_shim.h"
#include "android_classes.h"
#include "egl_shim.h"
#include "aaudio_shim.h"
#include "icu_probe.h"
#include "so_util.h"

extern DynLibFunction g_imports[];
extern size_t         g_imports_count;
#include "util.h"

/* ------------------------------------------------------------------------ */
/* dl_iterate_phdr                                                          */
/* ------------------------------------------------------------------------ */

/* struct dl_phdr_info_compat, Dl_info_compat and phdr_cb live in dl_shim.h so
 * the declarations and definitions are checked against each other. dlpi_addr
 * is the RELOCATION BIAS, not the lowest mapped address: for any program
 * header, p_vaddr + dlpi_addr is the real address.  This .so has its first
 * PT_LOAD at p_vaddr 0, so bias == load_virtbase. */

int dl_iterate_phdr_fake(phdr_cb cb, void *data) {
  int ret = 0;
  int n = 0;

  for (so_module *m = so_module_list; m; m = m->next) {
    if (!m->load_virtbase || !m->phdr) continue;

    struct dl_phdr_info_compat info;
    memset(&info, 0, sizeof(info));
    info.dlpi_addr  = (Elf64_Addr)m->load_bias;   /* bias, not base */
    info.dlpi_name  = m->name;
    info.dlpi_phdr  = m->phdr;
    info.dlpi_phnum = (Elf64_Half)m->phnum;
    info.dlpi_adds  = 1;
    info.dlpi_subs  = 0;

    n++;
    ret = cb(&info, sizeof(info), data);
    if (ret != 0) break;   /* non-zero terminates iteration, per contract */
  }

  static int logged = 0;
  if (!logged) {
    logged = 1;
    debug_log("[dl] dl_iterate_phdr: reported %d module(s), cb returned %d\n", n, ret);
    if (n == 0)
      debug_log("[dl] WARNING: nothing to report. If the runtime is asking this "
                "early, so_file_load has not run yet -- check init order.\n");
  }
  return ret;
}

/* ------------------------------------------------------------------------ */
/* dladdr                                                                    */
/* ------------------------------------------------------------------------ */

/* The target .so is stripped: dynsym holds three usable FUNC entries, so a
 * name lookup nearly always misses. That is fine. NativeAOT wants dli_fbase
 * (which module owns this address) far more than it wants dli_sname, and
 * dli_fbase is always exact. Returning 1 with a NULL dli_sname is legal. */
int dladdr_fake(const void *addr, Dl_info_compat *info) {
  if (!info) return 0;
  memset(info, 0, sizeof(*info));

  so_module *m = so_find_module_by_addr(addr);
  if (!m) return 0;

  info->dli_fname = m->name;
  info->dli_fbase = m->load_virtbase;

  /* Best-effort: nearest preceding defined FUNC symbol. */
  uintptr_t target = (uintptr_t)addr - m->load_bias;
  uintptr_t best_val = 0;
  const char *best_name = NULL;

  for (size_t i = 0; i < m->num_dynsym; i++) {
    Elf64_Sym *s = &m->dynsym[i];
    if (s->st_shndx == SHN_UNDEF) continue;
    if (ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
    if (s->st_value <= target && s->st_value >= best_val) {
      best_val  = s->st_value;
      best_name = m->dynstrtab + s->st_name;
    }
  }

  if (best_name) {
    info->dli_sname = best_name;
    info->dli_saddr = (void *)(m->load_bias + best_val);
  }
  return 1;
}

/* ------------------------------------------------------------------------ */
/* dlopen / dlsym / dlclose / dlerror                                        */
/* ------------------------------------------------------------------------ */

/* Fake handles. Distinct non-NULL values so the caller can tell them apart and
 * so a handle never collides with a real pointer. */
#define H_GLES    ((void *)0x10000001)
#define H_EGL     ((void *)0x10000002)
/* H_OPENSL retired -- see the refusal in dlopen_fake. */
#define H_ANDROID ((void *)0x10000004)
#define H_LOG     ((void *)0x10000006)
#define H_ICU     ((void *)0x10000007)
#define H_LIBC    ((void *)0x10000008)
#define H_AAUDIO  ((void *)0x10000009)
#define H_SELF    ((void *)0x10000005)

static const char *g_dlerror = NULL;

/* Symbols we hand out for libandroid.so. ANativeWindow_fromSurface is the one
 * that matters: the game calls it from surfaceCreated and expects something it
 * can pass to eglCreateWindowSurface. See lawn_natives.c. */
extern void *nx_ANativeWindow_fromSurface(void *env, void *surface);
extern void  nx_ANativeWindow_release(void *win);
extern int   nx_ANativeWindow_getWidth(void *win);
extern int   nx_ANativeWindow_getHeight(void *win);
extern int   nx_ANativeWindow_setBuffersGeometry(void *win, int w, int h, int fmt);

static void *maybe_load_from_disk(const char *base);

void *dlopen_fake(const char *name, int flag) {
  (void)flag;
  g_dlerror = NULL;

  if (!name) return H_SELF;   /* dlopen(NULL) == handle to the main program */

  debug_log("[dl] dlopen(%s)\n", name);

  /* Match on the last path component only.
   *
   * .NET resolves a DllImport by probing a cross product of prefixes and
   * suffixes: for [DllImport("liblog")] it tries liblog, liblog.so,
   * libliblog, libliblog.so, each bare and each prefixed with a directory it
   * derived from the module path. Matching the whole string means seven of the
   * eight miss for no reason. */
  const char *base = name;
  for (const char *p = name; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;

  if (strstr(base, "libGLESv")) return H_GLES;
  if (strstr(base, "libEGL"))   return H_EGL;
  if (strstr(base, "libandroid")) return H_ANDROID;

  /* Android's logging library. The managed side P/Invokes into it, and we
   * already implement these for the image's own imports -- there is no reason
   * to make the managed path go without. Matches liblog and libliblog, with
   * or without .so, because those are the names it will try. */
  if (strstr(base, "liblog")) return H_LOG;

  /* libc, however it is spelled. .NET resolves [DllImport("c")] by probing
   * "c", "libc", "libc.so" and the lib-prefixed variants, so match the whole
   * basename rather than a substring -- "libicuuc" contains "c" too. */
  if (!strcmp(base, "c")     || !strcmp(base, "libc")     ||
      !strcmp(base, "c.so")  || !strcmp(base, "libc.so")  ||
      !strcmp(base, "libc.so.6")) {
    debug_log("[dl]   -> libc, served from the loader's import table\n");
    return H_LIBC;
  }

  /* Deliberate failures. Each one steers the engine down a path we have a
   * shim for instead of one we do not:
   *   libaaudio  -> falls back to OpenSL ES, which opensles.c implements
   *   libvulkan  -> falls back to GLES, which mesa provides
   *   libicu*    -> unreachable anyway once
   *                 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 is set; failing
   *                 here as well makes the intent obvious in the log */
  /* ICU is granted a handle purely to find out what it wants.
   *
   * We cannot serve it, and the load will still fail -- but refusing at
   * dlopen tells us nothing, whereas granting the handle makes the runtime
   * enumerate every symbol it needs through dlsym, and each one gets logged.
   * That turns "we need ICU" into an exact list, which is what decides
   * between writing a small shim and cross-compiling the real library.
   *
   * Once that list is known, either serve it or go back to refusing here. */
  if (strstr(base, "libicu")) {
    debug_log("[dl]   -> granting a handle to enumerate required symbols. "
              "The load will still fail; the point is the log below.\n");
    return H_ICU;
  }

  /* AAudio is SERVED, not refused.
   *
   * The refusal above was written on the theory that the engine would fall
   * back to OpenSL ES. It does not: AndroidNativeAudioEngine calls AAudio
   * directly, nothing catches the failure, and the DllNotFoundException from
   * the unresolved P/Invoke propagated out through SexyAppBase..ctor and
   * killed LawnApp construction outright. Resolving the library turns a fatal
   * exception into an error return the engine was written to handle. See
   * aaudio_shim.c. */
  if (strstr(base, "libaaudio")) {
    debug_log("[dl]   -> served by aaudio_shim (no device; fails at "
              "openStream)\n");
    return H_AAUDIO;
  }

  if (strstr(base, "libvulkan")) {
    debug_log("[dl]   -> refused on purpose (forcing the GLES path)\n");
    g_dlerror = "library not available";
    return NULL;
  }

  /* OpenSL ES is refused rather than handed a token.
   *
   * Granting a handle we cannot service is worse than refusing: the engine
   * takes the success path, asks for slCreateEngine, gets NULL back, and jumps
   * to zero. Refusing makes it fall through to whatever it does when there is
   * no audio device, which is a path it was written to handle.
   *
   * Grant this once opensles.c exists -- and not a moment before. */
  if (strstr(base, "libOpenSLES")) {
    debug_log("[dl]   -> refused: no OpenSL ES implementation yet. "
              "The game will run without audio rather than crash.\n");
    g_dlerror = "library not available";
    return NULL;
  }

  g_dlerror = "not found";
  /* Last chance: a real library sitting next to the game on the SD card. */
  {
    void *h = maybe_load_from_disk(base);
    if (h) return h;
  }

  debug_log("[dl]   -> NULL (unhandled; add it if the engine needs it)\n");
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* Loading a real .so from the SD card                                       */
/* ------------------------------------------------------------------------ */
/*
 * Until now dlopen either served a shim or refused. libopenmpt is neither: it
 * is a genuine AArch64 Android library, the game routes ALL its music through
 * it (crazydave.ogg fails too, and that is not a tracker format), and without
 * it the resource group that music belongs to does not load -- which is the
 * leading explanation for the reanim that fails with an empty name.
 *
 * The loader already supports this: so_file_load / so_relocate / so_initialize
 * are the same calls that map libLawn.Android.so, and so_module_list is
 * already walked by dlsym's last-resort branch, so symbols become visible the
 * moment the module is in the list.
 *
 * Modules are resolved against the same import table libLawn uses. That covers
 * libc, libm and libdl. It does NOT cover libc++_shared, which libopenmpt
 * needs for 109 of its 170 undefined symbols -- so that has to be loaded
 * first, and it is loaded first here, from the same directory.
 *
 * Anything unresolved is reported by so_relocate rather than silently left as
 * a null pointer, which is the failure mode that would otherwise turn into a
 * jump to address zero the first time the game decoded a note.
 */
/* main.c's HOST_DATA_DIR is file-local; the value is the loader's own data
 * directory -- the sdmc: side of the VFS, which is where a real .so on the
 * SD card lives. Kept in step with main.c by hand. */
#define DL_HOST_DATA_DIR "sdmc:/switch/pvzultimate"

static so_module g_extra[4];
static int       g_nextra;

static void *load_real_so(const char *base) {
  if (g_nextra >= (int)(sizeof(g_extra)/sizeof(g_extra[0]))) return NULL;

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", DL_HOST_DATA_DIR, base);

  so_module *m = &g_extra[g_nextra];
  if (so_file_load(m, path) != 0) {
    debug_log("[dl]   -> %s not present at %s\n", base, path);
    return NULL;
  }
  so_relocate(m, g_imports, g_imports_count);
  so_flush_caches(m);
  so_initialize(m);
  g_nextra++;
  debug_log("[dl]   -> loaded %s from the SD card\n", base);
  return (void *)m;
}

/* Returns a handle if `base` names a library we are willing to load from disk.
 * Only an explicit list: dlopen is called with all sorts of speculative names
 * during probing, and attempting a file open for each would be noise. */
static void *maybe_load_from_disk(const char *base) {
  if (!strstr(base, "openmpt")) return NULL;

  /* Already loaded? dlopen is called many times with name variants. */
  for (int i = 0; i < g_nextra; i++)
    if (strstr(g_extra[i].name, "openmpt")) return (void *)&g_extra[i];

  /* libopenmpt is C++ and needs the STL runtime resolved before it. */
  if (!g_nextra) {
    if (!load_real_so("libc++_shared.so"))
      debug_log("[dl]   -> libc++_shared.so is missing; libopenmpt needs it "
                "for 109 of its symbols and will not resolve without it\n");
  }
  return load_real_so("libopenmpt.so");
}

void *dlsym_fake(void *handle, const char *symbol) {
  g_dlerror = NULL;
  if (!symbol) return NULL;

  if (handle == H_ICU) return icu_probe_symbol(symbol);
  if (handle == H_AAUDIO) return aaudio_shim_lookup(symbol);

  if (handle == H_LIBC || handle == H_SELF) {
    void *p = imports_lookup(symbol);

    /* bionic spells several of these with a leading double underscore while
     * the managed declaration drops it (system_property_get vs
     * __system_property_get). Try the other spelling before giving up rather
     * than failing on a naming convention. */
    if (!p) {
      char alt[128];
      if (!strncmp(symbol, "__", 2)) {
        snprintf(alt, sizeof(alt), "%s", symbol + 2);
      } else {
        snprintf(alt, sizeof(alt), "__%s", symbol);
      }
      p = imports_lookup(alt);
      if (p) debug_log("[dl] libc: %s resolved as %s\n", symbol, alt);
    }
    if (p) return p;
    /* Not fatal: the managed loader probes for optional entry points and is
     * built to handle a miss. Log it, because a miss that mattered looks
     * identical to one that did not. */
    debug_log("[dl] libc has no %s\n", symbol);
    g_dlerror = "symbol not found";
    return NULL;
  }

  if (handle == H_LOG) {
    if (!strcmp(symbol, "__android_log_print"))  return dlsym_android_log_print();
    if (!strcmp(symbol, "__android_log_vprint")) return dlsym_android_log_vprint();
    if (!strcmp(symbol, "__android_log_write"))  return dlsym_android_log_write();
    if (!strcmp(symbol, "__android_log_assert")) return dlsym_android_log_write();
    debug_log("[dl] liblog has no %s\n", symbol);
    g_dlerror = "symbol not found";
    return NULL;
  }

  if (handle == H_ANDROID) {
    if (!strcmp(symbol, "ANativeWindow_fromSurface"))       return (void *)nx_ANativeWindow_fromSurface;
    if (!strcmp(symbol, "ANativeWindow_release"))           return (void *)nx_ANativeWindow_release;
    if (!strcmp(symbol, "ANativeWindow_getWidth"))          return (void *)nx_ANativeWindow_getWidth;
    if (!strcmp(symbol, "ANativeWindow_getHeight"))         return (void *)nx_ANativeWindow_getHeight;
    if (!strcmp(symbol, "ANativeWindow_setBuffersGeometry"))return (void *)nx_ANativeWindow_setBuffersGeometry;

    /* The asset API. This is the path MonoGame's content pipeline takes, so a
     * miss here means the game starts and renders nothing. */
    if (!strcmp(symbol, "AAssetManager_fromJava"))     return (void *)nx_AAssetManager_fromJava;
    if (!strcmp(symbol, "AAssetManager_open"))         return (void *)nx_AAssetManager_open;
    if (!strcmp(symbol, "AAsset_read"))                return (void *)nx_AAsset_read;
    if (!strcmp(symbol, "AAsset_seek"))                return (void *)nx_AAsset_seek;
    if (!strcmp(symbol, "AAsset_seek64"))              return (void *)nx_AAsset_seek;
    if (!strcmp(symbol, "AAsset_getLength"))           return (void *)nx_AAsset_getLength;
    if (!strcmp(symbol, "AAsset_getLength64"))         return (void *)nx_AAsset_getLength;
    if (!strcmp(symbol, "AAsset_getRemainingLength"))  return (void *)nx_AAsset_getRemainingLength;
    if (!strcmp(symbol, "AAsset_getBuffer"))           return (void *)nx_AAsset_getBuffer;
    if (!strcmp(symbol, "AAsset_close"))               return (void *)nx_AAsset_close;
    if (!strcmp(symbol, "AAsset_isAllocated"))         return (void *)nx_AAsset_isAllocated;
    if (!strcmp(symbol, "AAsset_openFileDescriptor"))  return (void *)nx_AAsset_openFileDescriptor;
  }

  /* Silk.NET resolves the whole GLES/EGL surface dynamically -- roughly 150
   * entry points. Rather than tabulating them, let EGL do it. This is the
   * trick the Unity port uses and it is entirely engine-neutral. */
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
    /* Our interceptions first. eglSwapBuffers in particular MUST be ours --
     * the game swaps from inside doFrame, so this is the only point at which
     * the cursor overlay can land on the finished frame. */
    void *ours = egl_shim_lookup(symbol);
    if (ours) return ours;

    void *p = (void *)eglGetProcAddress(symbol);
    if (p) return p;
  }

  /* Last resort: a symbol defined by the game module itself. */
  for (so_module *m = so_module_list; m; m = m->next) {
    uintptr_t a = so_symbol(m, symbol);
    if (a) return (void *)a;
  }

  debug_log("[dl] dlsym(%p, %s) -> NULL\n", handle, symbol);
  g_dlerror = "symbol not found";
  return NULL;
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

const char *dlerror_fake(void) {
  const char *e = g_dlerror;
  g_dlerror = NULL;   /* dlerror() clears the condition, per contract */
  return e;
}
