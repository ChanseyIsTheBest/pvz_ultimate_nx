/* imports.c -- the symbol resolution table.
 *
 * All 257 undefined symbols in the image, classified three ways:
 *
 *   shimmed    behaviour differs from newlib, or newlib has no such symbol.
 *              These are the ones with opinions; read the file each lives in.
 *   forwarded  identical semantics, straight through to newlib.
 *   failed     networking and process control, which must fail rather than
 *              pretend to succeed.
 *
 * The table was generated against the binary's own symbol list rather than
 * written by hand, so a symbol missing here means the image does not import it.
 * Anything that slips through still traps by name at first call -- see
 * so_relocate -- so the build/run/read-the-log loop still works.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <switch.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>

#include "android_classes.h"
#include "dl_shim.h"
#include "fileio.h"
#include "libc_shim.h"
#include "mem_arena.h"
#include "runtime_glue.h"
#include "so_util.h"
#include "threads.h"
#include "util.h"

/* ---- Android logging: wire these first, they are the only debug channel -- */

static int android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debug_log("[%s] %s\n", tag ? tag : "?", buf);
  return 0;
}

static int android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list ap) {
  (void)prio;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  debug_log("[%s] %s\n", tag ? tag : "?", buf);
  return 0;
}

static int android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio;
  debug_log("[%s] %s\n", tag ? tag : "?", text ? text : "");
  return 0;
}

static void android_set_abort_message_fake(const char *msg) {
  debug_log("[abort] %s\n", msg ? msg : "(null)");
}

/* Handed to dlsym so the managed DllImport("liblog") path reaches the same
 * implementations the image's own imports use. */
void *dlsym_android_log_print(void)  { return (void *)android_log_print_fake; }
void *dlsym_android_log_vprint(void) { return (void *)android_log_vprint_fake; }
void *dlsym_android_log_write(void)  { return (void *)android_log_write_fake; }

/* ---- system properties -------------------------------------------------- */

static int system_property_get_fake(const char *name, char *value) {
  if (!name || !value) return 0;
  const char *v = NULL;
  if      (!strcmp(name, "ro.build.version.sdk"))     v = "33";
  else if (!strcmp(name, "ro.build.version.release")) v = "13";
  else if (!strcmp(name, "ro.product.cpu.abi"))       v = "arm64-v8a";
  else if (!strcmp(name, "ro.product.manufacturer"))  v = "Nintendo";
  else if (!strcmp(name, "ro.product.model"))         v = "Switch";
  else if (!strcmp(name, "ro.product.brand"))         v = "Nintendo";
  else if (!strcmp(name, "ro.arch"))                  v = "arm64";
  else if (!strcmp(name, "ro.product.cpu.abilist"))   v = "arm64-v8a";
  else if (!strcmp(name, "ro.build.version.sdk_int")) v = "33";
  /* Newer alias for the same number; asked for on Android 14 and later. */
  else if (!strcmp(name, "ro.build.version.sdk_full")) v = "33";
  /* .NET for Android reads these to decide how chatty to be. Empty is a valid
   * answer meaning "no extra logging", and answering beats the unhandled path. */
  else if (!strncmp(name, "debug.mono.", 11))         v = "";
  else if (!strncmp(name, "debug.dotnet.", 13))       v = "";
  /* Asked for by TimeZoneInfo. UTC rather than a real zone: the console does
   * not expose one to us, and a name the runtime cannot then find in a tzdata
   * file it also does not have is worse than the one zone that needs no
   * database. Pairs with DOTNET_SYSTEM_TIMEZONE_INVARIANT in runtime_glue.c. */
  else if (!strcmp(name, "persist.sys.timezone"))     v = "UTC";
  if (!v) { value[0] = 0; debug_log("[prop] unhandled: %s\n", name); return 0; }
  strcpy(value, v);
  return (int)strlen(v);
}

/* ---- CPU topology -------------------------------------------------------
 * The GC sizes heaps and the threadpool sizes itself from these. Homebrew gets
 * three cores; reporting anything else configures the runtime for hardware
 * that is not here. */

/* BIONIC's _SC_* numbering, which is NOT glibc's.
 *
 * The first version of this used glibc constants and answered 0 for the ones
 * that mattered. The GC asks how much memory the machine has (_SC_PHYS_PAGES,
 * 98 here, 85 in glibc), got zero, and fell back to sizing its reservation off
 * total RAM -- it then asked the arena for 3.75 GB.
 *
 * Report a deliberately modest machine. These numbers exist to tell the GC how
 * big a heap is reasonable, and the honest answer is "far less than the
 * hardware has", because most of it is already ours. */
#define REPORTED_RAM_MB 1024

static long sysconf_fake(int name) {
  switch (name) {
    case 11: return 256;                       /* _SC_OPEN_MAX            */
    case 39:                                   /* _SC_PAGESIZE            */
    case 40: return 0x1000;                    /* _SC_PAGE_SIZE           */
    case 96:                                   /* _SC_NPROCESSORS_CONF    */
    case 97: return 3;                         /* _SC_NPROCESSORS_ONLN    */
    case 98:                                   /* _SC_PHYS_PAGES          */
      return (long)((REPORTED_RAM_MB * 1024ull * 1024ull) / 0x1000);
    case 99:                                   /* _SC_AVPHYS_PAGES        */
      return (long)((REPORTED_RAM_MB * 1024ull * 1024ull) / 0x1000) / 2;
    case 6:  return 100;                       /* _SC_CLK_TCK             */
    case 10: return 32;                        /* _SC_NGROUPS_MAX         */
    default:
      debug_log("[sysconf] unhandled name=%d -> 0 "
                "(bionic numbering; check bits/sysconf.h)\n", name);
      return 0;
  }
}

static int sched_getcpu_fake(void) { return (int)svcGetCurrentProcessorNumber(); }

/* AT_HWCAP. Zero is safe -- .NET falls back to scalar paths. Filling in the
 * Cortex-A57 feature bits is an optimisation, not a fix. */
static unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

/* ---- process control: must fail -------------------------------------- */

static int fork_fail(void) { errno = ENOSYS; return -1; }

/* ---- symbols newlib has but does not declare in C ----------------------
 * The Itanium C++ ABI functions exist in libc but are only declared in
 * <cxxabi.h>, which is C++-only. Declaring them here is safe -- the linker
 * resolves against the real implementations. */
extern int  __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
extern void __cxa_finalize(void *dso);

/* ---- symbols newlib does not have at all ------------------------------- */

/* No device nodes here. ENOTTY is the answer callers expect for "this is not
 * a terminal / not an ioctl-able object" and it is what the runtime probes
 * for when it checks whether a stream is interactive. */
static int ioctl_fake(int fd, unsigned long request, ...) {
  (void)fd; (void)request;
  errno = ENOTTY;
  return -1;
}

/* GNU restartable multibyte conversions. newlib has mbsrtowcs/wcsrtombs but
 * not the length-bounded 'n' variants; express them in terms of what exists.
 * With DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 everything is effectively
 * single-byte, so the bound only has to be respected, not cleverly. */
static size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms,
                              size_t len, mbstate_t *ps) {
  if (!src || !*src) return 0;
  size_t avail = strnlen(*src, nms);
  char *tmp = malloc(avail + 1);
  if (!tmp) return (size_t)-1;
  memcpy(tmp, *src, avail);
  tmp[avail] = 0;
  const char *p = tmp;
  size_t r = mbsrtowcs(dst, &p, len, ps);
  /* Advance the caller's pointer by however much was consumed. */
  *src = (p == NULL) ? NULL : *src + (size_t)(p - tmp);
  free(tmp);
  return r;
}

static size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc,
                              size_t len, mbstate_t *ps) {
  if (!src || !*src) return 0;
  size_t n = 0;
  while (n < nwc && (*src)[n]) n++;
  wchar_t *tmp = malloc((n + 1) * sizeof(wchar_t));
  if (!tmp) return (size_t)-1;
  memcpy(tmp, *src, n * sizeof(wchar_t));
  tmp[n] = 0;
  const wchar_t *p = tmp;
  size_t r = wcsrtombs(dst, &p, len, ps);
  *src = (p == NULL) ? NULL : *src + (size_t)(p - tmp);
  free(tmp);
  return r;
}

/* ---- the table ---------------------------------------------------------- */


/* Wrappers in imports_extra.c for what newlib does not provide directly. */
int  iswalpha_l_fake(wint_t, locale_t);   int  iswblank_l_fake(wint_t, locale_t);
int  iswcntrl_l_fake(wint_t, locale_t);   int  iswdigit_l_fake(wint_t, locale_t);
int  iswprint_l_fake(wint_t, locale_t);   int  iswpunct_l_fake(wint_t, locale_t);
int  iswspace_l_fake(wint_t, locale_t);   int  iswupper_l_fake(wint_t, locale_t);
int  iswxdigit_l_fake(wint_t, locale_t);
wint_t towlower_l_fake(wint_t, locale_t); wint_t towupper_l_fake(wint_t, locale_t);
int    strcoll_l_fake(const char *, const char *, locale_t);
size_t strxfrm_l_fake(char *, const char *, size_t, locale_t);
int    wcscoll_l_fake(const wchar_t *, const wchar_t *, locale_t);
size_t wcsxfrm_l_fake(wchar_t *, const wchar_t *, size_t, locale_t);
void   sincos_fake(double, double *, double *);
void   sincosf_fake(float, float *, float *);
int    __cxa_thread_atexit_impl_fake(void (*)(void *), void *, void *);

extern FILE *nx_stderr;
int  stat_fake(const char *, struct stat *);
int  lstat_fake(const char *, struct stat *);
int  truncate_fake(const char *, off_t);
int  chdir_fake(const char *);
long pathconf_fake(const char *, int);
int  statvfs_fake(const char *, void *);
long readlink_fake(const char *, char *, size_t);
int  symlink_fake(const char *, const char *);
int  openat_fake(int, const char *, int, ...);
int  unlinkat_fake(int, const char *, int);
int  fchmodat_fake(int, const char *, mode_t, int);
void *fdopendir_fake(int);
int  __register_atfork_fake(void (*)(void), void (*)(void), void (*)(void), void *);

int         wcscoll_fake(const wchar_t *, const wchar_t *);
size_t      wcsxfrm_fake(wchar_t *, const wchar_t *, size_t);
long double strtold_fake(const char *, char **);
long double wcstold_fake(const wchar_t *, wchar_t **);
#define SYM(name, fn) { name, (uintptr_t)&fn }

DynLibFunction g_imports[] = {
  /* logging */
  SYM("__android_log_print",       android_log_print_fake),
  SYM("__android_log_vprint",      android_log_vprint_fake),
  SYM("__android_log_write",       android_log_write_fake),
  SYM("android_set_abort_message", android_set_abort_message_fake),

  /* platform description */
  SYM("__system_property_get", system_property_get_fake),
  SYM("sysconf",               sysconf_fake),
  SYM("sched_getcpu",          sched_getcpu_fake),
  SYM("getauxval",             getauxval_fake),

  /* process control */
  SYM("fork",  fork_fail),
  SYM("execv", fork_fail),

  /* These three were classified as hand-written entries when the table was
   * generated, and then no hand-written entry was added -- so they trapped as
   * unresolved. The game is offline; a clean failure is what callers expect. */
  SYM("socket",  net_fail_int),
  SYM("bind",    net_fail_int),
  SYM("connect", net_fail_int),

  /* New in game 2.1.2, which imports three more socket entry points than 2.x.
   *
   * Nothing here is expected to call them -- the port is offline and java_net.c
   * fails every connection deliberately -- but an unresolved import is a fatal
   * trap the moment it IS called, so a clean failure beats finding out. Same
   * treatment as the three above. */
  SYM("listen",  net_fail_int),
  SYM("accept",  net_fail_int),
  SYM("accept4", net_fail_int),
  SYM("__cmsg_nxthdr", cmsg_nxthdr_fake),

  /* ---- shimmed: behaviour differs from newlib, or newlib has no such thing */
  SYM("__assert2", __assert2_fake),
  SYM("__ctype_get_mb_cur_max", __ctype_get_mb_cur_max_fake),
  SYM("__errno", __errno_fake),
  SYM("__libc_current_sigrtmin", __libc_current_sigrtmin_fake),
  SYM("__memcpy_chk", __memcpy_chk_fake),
  SYM("__memset_chk", __memset_chk_fake),
  SYM("__open_2", __open_2_fake),
  SYM("__read_chk", __read_chk_fake),
  SYM("__sched_cpucount", __sched_cpucount_fake),
  SYM("__stack_chk_fail", __stack_chk_fail_fake),
  SYM("__strcpy_chk", __strcpy_chk_fake),
  SYM("__strlen_chk", __strlen_chk_fake),
  SYM("__strncpy_chk2", __strncpy_chk2_fake),
  SYM("__umask_chk", __umask_chk_fake),
  SYM("__vsnprintf_chk", __vsnprintf_chk_fake),
  SYM("arc4random_buf", arc4random_buf_fake),
  SYM("asprintf", asprintf_fake),
  SYM("chmod", chmod_fake),
  SYM("clock_gettime", clock_gettime_fake),
  SYM("clock_nanosleep", clock_nanosleep_fake),
  SYM("close", close_fake),
  SYM("closedir", closedir_fake),
  SYM("closelog", closelog_fake),
  SYM("dl_iterate_phdr", dl_iterate_phdr_fake),
  SYM("dladdr", dladdr_fake),
  SYM("dlclose", dlclose_fake),
  SYM("dlerror", dlerror_fake),
  SYM("dlopen", dlopen_fake),
  SYM("dlsym", dlsym_fake),
  SYM("dup2", dup2_fake),
  SYM("epoll_create1", epoll_create1_fake),
  SYM("epoll_ctl", epoll_ctl_fake),
  SYM("epoll_wait", epoll_wait_fake),
  SYM("fallocate", fallocate_fake),
  SYM("fchmod", fchmod_fake),
  SYM("fclose", fclose_fake),
  SYM("fcntl", fcntl_fake),
  SYM("fflush", fflush_fake),
  SYM("flock", flock_fake),
  SYM("fopen", fopen_fake),
  SYM("fprintf", fprintf_fake),
  SYM("fputc", fputc_fake),
  SYM("fputs", fputs_fake),
  SYM("freeaddrinfo", freeaddrinfo_fake),
  SYM("freelocale", freelocale_fake),
  SYM("fstat64", fstat64_fake),
  SYM("fstatfs", fstatfs_fake),
  SYM("fsync", fsync_fake),
  SYM("ftruncate64", ftruncate64_fake),
  SYM("futimens", futimens_fake),
  SYM("fwrite", fwrite_fake),
  SYM("gai_strerror", gai_strerror_fake),
  SYM("getcwd", getcwd_fake),
  SYM("getegid", getegid_fake),

  /* --- what real libraries loaded from the SD card need ------------------
   * libc++_shared and libopenmpt between them imported 85 symbols this table
   * did not carry. The C++ ones now resolve against libc++_shared itself via
   * inter-module resolution; these are the plain libc ones. */
  /* A data symbol: the address of the FILE* is what gets registered. */
  { "stderr", (uintptr_t)&nx_stderr },
  SYM("stat", stat_fake),              SYM("lstat", lstat_fake),
  SYM("fstat", fstat),                 SYM("truncate", truncate_fake),
  SYM("ftruncate", ftruncate),         SYM("chdir", chdir_fake),
  SYM("pathconf", pathconf_fake),      SYM("statvfs", statvfs_fake),
  SYM("readlink", readlink_fake),      SYM("symlink", symlink_fake),
  SYM("openat", openat_fake),          SYM("unlinkat", unlinkat_fake),
  SYM("fchmodat", fchmodat_fake),      SYM("fdopendir", fdopendir_fake),
  SYM("__register_atfork", __register_atfork_fake),
  SYM("btowc", btowc),                 SYM("wctob", wctob),
  SYM("wmemcmp", wmemcmp),             SYM("fputwc", fputwc),
  SYM("getwc", getwc),                 SYM("ungetwc", ungetwc),
  SYM("swprintf", swprintf),
  SYM("wcstod", wcstod),               SYM("wcstof", wcstof),
  SYM("wcstol", wcstol),               SYM("wcstold", wcstold_fake),
  SYM("wcstoll", wcstoll),             SYM("wcstoul", wcstoul),
  SYM("wcstoull", wcstoull),
  SYM("strtoul", strtoul),             SYM("strtold", strtold_fake),
  SYM("setlocale", setlocale),         SYM("localeconv", localeconv),
  SYM("qsort", qsort),                 SYM("div", div),
  SYM("remove", remove),
  SYM("exp2", exp2),                   SYM("exp2f", exp2f),
  SYM("log10f", log10f),               SYM("logf", logf),
  SYM("powf", powf),                   SYM("tanf", tanf),
  SYM("sinhf", sinhf),                 SYM("hypot", hypot),
  SYM("ldexp", ldexp),
  SYM("fread", fread),                 SYM("fseek", fseek),
  SYM("fseeko", fseeko),               SYM("ftello", ftello),
  SYM("getc", getc),                   SYM("ungetc", ungetc),
  SYM("pthread_join", pthread_join),
  SYM("pthread_mutex_trylock", pthread_mutex_trylock),

  /* Locale variants and GNU extensions -- see imports_extra.c. */
  SYM("iswalpha_l", iswalpha_l_fake),  SYM("iswblank_l", iswblank_l_fake),
  SYM("iswcntrl_l", iswcntrl_l_fake),  SYM("iswdigit_l", iswdigit_l_fake),
  SYM("iswprint_l", iswprint_l_fake),  SYM("iswpunct_l", iswpunct_l_fake),
  SYM("iswspace_l", iswspace_l_fake),  SYM("iswupper_l", iswupper_l_fake),
  SYM("iswxdigit_l", iswxdigit_l_fake),
  SYM("towlower_l", towlower_l_fake),  SYM("towupper_l", towupper_l_fake),
  SYM("strcoll_l", strcoll_l_fake),    SYM("strxfrm_l", strxfrm_l_fake),
  SYM("wcscoll_l", wcscoll_l_fake),    SYM("wcsxfrm_l", wcsxfrm_l_fake),
  SYM("sincos", sincos_fake),          SYM("sincosf", sincosf_fake),
  SYM("__cxa_thread_atexit_impl", __cxa_thread_atexit_impl_fake),

  SYM("getenv", getenv_fake),
  SYM("geteuid", geteuid_fake),
  SYM("getgroups", getgroups_fake),
  SYM("gethostname", gethostname_fake),
  SYM("getline", getline_fake),
  SYM("getpagesize", getpagesize_fake),
  SYM("getpid", getpid_fake),
  SYM("getrlimit", getrlimit_fake),
  SYM("getrusage", getrusage_fake),
  SYM("gettid", gettid_fake),
  SYM("if_nametoindex", if_nametoindex_fake),
  SYM("iswlower_l", iswlower_l_fake),
  SYM("link", link_fake),
  SYM("lseek64", lseek64_fake),
  SYM("lstat64", lstat64_fake),
  SYM("madvise", madvise_fake),
  SYM("mkdir", mkdir_fake),
  SYM("mlock", mlock_fake),
  SYM("mmap", mmap_fake),
  SYM("mmap64", mmap_fake),
  SYM("mprotect", mprotect_fake),
  SYM("munlock", munlock_fake),
  SYM("munmap", munmap_fake),
  SYM("nanosleep", nanosleep_fake),
  SYM("newlocale", newlocale_fake),
  SYM("open", open_fake),
  SYM("opendir", opendir_fake),
  SYM("openlog", openlog_fake),
  SYM("pipe", pipe_fake),
  SYM("poll", poll_fake),
  SYM("posix_fadvise64", posix_fadvise64_fake),
  SYM("prctl", prctl_fake),
  SYM("pread", pread_fake),
  SYM("pthread_attr_destroy", pthread_attr_destroy_fake),
  SYM("pthread_attr_getstack", pthread_attr_getstack_fake),
  SYM("pthread_attr_init", pthread_attr_init_fake),
  SYM("pthread_attr_setdetachstate", pthread_attr_setdetachstate_fake),
  SYM("pthread_attr_setstacksize", pthread_attr_setstacksize_fake),
  SYM("pthread_cond_broadcast", pthread_cond_broadcast_fake),
  SYM("pthread_cond_destroy", pthread_cond_destroy_fake),
  SYM("pthread_cond_init", pthread_cond_init_fake),
  SYM("pthread_cond_signal", pthread_cond_signal_fake),
  SYM("pthread_cond_timedwait", pthread_cond_timedwait_fake),
  SYM("pthread_cond_wait", pthread_cond_wait_fake),
  SYM("pthread_condattr_destroy", pthread_condattr_destroy_fake),
  SYM("pthread_condattr_init", pthread_condattr_init_fake),
  SYM("pthread_condattr_setclock", pthread_condattr_setclock_fake),
  SYM("pthread_create", pthread_create_fake),
  SYM("pthread_detach", pthread_detach_fake),
  SYM("pthread_getattr_np", pthread_getattr_np_fake),
  SYM("pthread_getspecific", pthread_getspecific_fake),
  SYM("pthread_key_create", pthread_key_create_fake),
  SYM("pthread_key_delete", pthread_key_delete_fake),
  SYM("pthread_kill", pthread_kill_fake),
  SYM("pthread_mutex_destroy", pthread_mutex_destroy_fake),
  SYM("pthread_mutex_init", pthread_mutex_init_fake),
  SYM("pthread_mutex_lock", pthread_mutex_lock_fake),
  SYM("pthread_mutex_unlock", pthread_mutex_unlock_fake),
  SYM("pthread_mutexattr_destroy", pthread_mutexattr_destroy_fake),
  SYM("pthread_mutexattr_init", pthread_mutexattr_init_fake),
  SYM("pthread_mutexattr_settype", pthread_mutexattr_settype_fake),
  SYM("pthread_once", pthread_once_fake),
  SYM("pthread_rwlock_rdlock", pthread_rwlock_rdlock_fake),
  SYM("pthread_rwlock_unlock", pthread_rwlock_unlock_fake),
  SYM("pthread_rwlock_wrlock", pthread_rwlock_wrlock_fake),
  SYM("pthread_self", pthread_self_fake),
  SYM("pthread_setname_np", pthread_setname_np_fake),
  SYM("pthread_setspecific", pthread_setspecific_fake),
  SYM("pthread_sigmask", pthread_sigmask_fake),
  SYM("pwrite", pwrite_fake),
  SYM("read", read_fake),
  SYM("readdir", readdir_fake),
  SYM("realpath", realpath_fake),
  SYM("rename", rename_fake),
  SYM("rmdir", rmdir_fake),
  SYM("sched_getaffinity", sched_getaffinity_fake),
  SYM("sched_yield", sched_yield_fake),
  SYM("sendfile", sendfile_fake),
  SYM("setenv", setenv_fake),
  SYM("sigaction", sigaction_fake),
  SYM("sigaddset", sigaddset_fake),
  SYM("sigemptyset", sigemptyset_fake),
  SYM("stat64", stat64_fake),
  SYM("statfs", statfs_fake),
  SYM("strftime_l", strftime_l_fake),
  SYM("strtold_l", strtold_l_fake),
  SYM("strtoll_l", strtoll_l_fake),
  SYM("strtoull_l", strtoull_l_fake),
  SYM("syscall", syscall_fake),
  SYM("sysinfo", sysinfo_fake),
  SYM("syslog", syslog_fake),
  SYM("uname", uname_fake),
  SYM("unlink", unlink_fake),
  SYM("uselocale", uselocale_fake),
  SYM("utimensat", utimensat_fake),
  SYM("vasprintf", vasprintf_fake),
  SYM("vfprintf", vfprintf_fake),
  SYM("write", write_fake),

  /* ---- __sF is a DATA symbol, not a function ---------------------------
   * bionic's stdout/stderr are &__sF[1] and &__sF[2], with the stride baked
   * into the game's instruction stream. Bind the array itself; libc_shim's
   * stdio wrappers identify our streams by address range. */
  { "__sF", (uintptr_t)__sF_fake },

  /* ---- straight forwards to newlib -------------------------------------
   * Generated from the image's actual undefined-symbol list, so this is
   * exhaustive rather than a guess. If one of these is missing from devkitA64's
   * newlib the LINKER will name it, which beats finding out at runtime. */
  SYM("__cxa_atexit", __cxa_atexit),
  SYM("__cxa_finalize", __cxa_finalize),
  SYM("abort", abort),
  SYM("acos", acos),
  SYM("asin", asin),
  SYM("atan", atan),
  SYM("atan2", atan2),
  SYM("atan2f", atan2f),
  SYM("atoi", atoi),
  SYM("calloc", calloc),
  SYM("cos", cos),
  SYM("cosf", cosf),
  SYM("cosh", cosh),
  SYM("crc32", crc32),
  SYM("deflate", deflate),
  SYM("deflateEnd", deflateEnd),
  SYM("deflateInit2_", deflateInit2_),
  SYM("exit", exit),
  SYM("exp", exp),
  SYM("expf", expf),
  SYM("fmod", fmod),
  SYM("free", free),
  SYM("gettimeofday", gettimeofday),
  SYM("inflate", inflate),
  SYM("inflateEnd", inflateEnd),
  SYM("inflateInit2_", inflateInit2_),
  SYM("inflateReset2", inflateReset2),
  SYM("ioctl", ioctl_fake),
  SYM("log", log),
  SYM("log2", log2),
  SYM("malloc", malloc),
  SYM("mbrlen", mbrlen),
  SYM("mbrtowc", mbrtowc),
  SYM("mbsnrtowcs", mbsnrtowcs_fake),
  SYM("mbsrtowcs", mbsrtowcs),
  SYM("mbtowc", mbtowc),
  SYM("memchr", memchr),
  SYM("memcmp", memcmp),
  SYM("memcpy", memcpy),
  SYM("memmove", memmove),
  SYM("memset", memset),
  SYM("posix_memalign", posix_memalign_fake),
  SYM("pow", pow),
  SYM("realloc", realloc),
  SYM("signal", signal),
  SYM("sin", sin),
  SYM("sinf", sinf),
  SYM("sinh", sinh),
  SYM("snprintf", snprintf),
  SYM("sscanf", sscanf),
  SYM("strcasecmp", strcasecmp),
  SYM("strcat", strcat),
  SYM("strchr", strchr),
  SYM("strcmp", strcmp),
  SYM("strcpy", strcpy),
  SYM("strdup", strdup),
  SYM("strerror", strerror),
  SYM("strerror_r", strerror_r),
  SYM("strlen", strlen),
  SYM("strncat", strncat),
  SYM("strncmp", strncmp),
  SYM("strncpy", strncpy),
  SYM("strrchr", strrchr),
  SYM("strstr", strstr),
  SYM("strtod", strtod),
  SYM("strtof", strtof),
  SYM("strtok_r", strtok_r),
  SYM("strtol", strtol),
  SYM("strtoll", strtoll),
  SYM("strtoull", strtoull),
  SYM("tan", tan),
  SYM("tanh", tanh),
  SYM("vsnprintf", vsnprintf),
  SYM("vsscanf", vsscanf),
  SYM("wcrtomb", wcrtomb),
  SYM("wcslen", wcslen),
  SYM("wcsnrtombs", wcsnrtombs_fake),
  SYM("wmemchr", wmemchr),

  /* ---- networking: fail cleanly ----------------------------------------
   * The game is offline. A stub returning success leaves the runtime waiting
   * on a socket that will never answer; a clean failure it handles. */
  SYM("getaddrinfo", net_fail_int),
  SYM("getnameinfo", net_fail_int),
  SYM("getpeername", net_fail_int),
  SYM("getsockname", net_fail_int),
  SYM("getsockopt", net_fail_int),
  SYM("recvfrom", net_fail_int),
  SYM("recvmsg", net_fail_int),
  SYM("sendmsg", net_fail_int),
  SYM("sendto", net_fail_int),
  SYM("setsockopt", net_fail_int),
  SYM("shutdown", net_fail_int),
  SYM("waitpid", net_fail_int),
};

size_t g_imports_count = sizeof(g_imports) / sizeof(g_imports[0]);

/* The managed side P/Invokes into libc by name -- [DllImport("c")] for
 * system_property_get, among others -- which arrives as dlopen + dlsym rather
 * than as an ELF import. Everything it could ask for is already in the table
 * above, resolved for the image's own imports; this just makes that table
 * reachable from the other direction instead of maintaining a second copy. */
void *imports_lookup(const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < g_imports_count; i++)
    if (!strcmp(name, g_imports[i].symbol)) return (void *)g_imports[i].func;
  return NULL;
}
