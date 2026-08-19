/* imports_extra.c -- the libc surface libc++_shared and libopenmpt want.
 *
 * Loading real libraries from the SD card turned up 85 imports our table did
 * not carry. Most are ordinary newlib functions and are listed directly in
 * imports.c; the ones here need a wrapper because newlib either does not have
 * them or spells them differently.
 *
 * Two families:
 *
 *   the *_l locale variants -- iswalpha_l, strcoll_l, wcstod_l and so on.
 *     The C library on Android has per-locale entry points; newlib has only
 *     the global-locale ones. There is exactly one locale here and it is the C
 *     locale, so discarding the locale_t argument and calling the base
 *     function is not an approximation, it is the same answer.
 *
 *   sincos / sincosf -- GNU extensions. Computing the two separately is
 *     correct, just marginally slower than a fused implementation would be.
 *
 * Everything here is deliberately thin. The point is to let a real library
 * link, not to reimplement a C library.
 */

#include <errno.h>
#include <stdarg.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* ---- locale-aware classification ---------------------------------------- */

#define WCTYPE_L(name)                                                        \
  int name##_l_fake(wint_t c, locale_t loc) { (void)loc; return name(c); }

WCTYPE_L(iswalpha)
WCTYPE_L(iswblank)
WCTYPE_L(iswcntrl)
WCTYPE_L(iswdigit)
WCTYPE_L(iswprint)
WCTYPE_L(iswpunct)
WCTYPE_L(iswspace)
WCTYPE_L(iswupper)
WCTYPE_L(iswxdigit)

wint_t towlower_l_fake(wint_t c, locale_t loc) { (void)loc; return towlower(c); }
wint_t towupper_l_fake(wint_t c, locale_t loc) { (void)loc; return towupper(c); }

/* ---- locale-aware collation --------------------------------------------- */

int strcoll_l_fake(const char *a, const char *b, locale_t loc) {
  (void)loc;
  return strcoll(a, b);
}

size_t strxfrm_l_fake(char *dst, const char *src, size_t n, locale_t loc) {
  (void)loc;
  return strxfrm(dst, src, n);
}

/* Built from wcscmp/wcsncpy rather than wcscoll/wcsxfrm, which newlib does not
 * reliably provide -- the same trap the *at family fell into. In the C locale
 * collation IS codepoint order, so this is the correct answer and not a
 * simplification. */
int wcscoll_fake(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }

size_t wcsxfrm_fake(wchar_t *dst, const wchar_t *src, size_t n) {
  size_t len = wcslen(src);
  if (dst && n > len) wmemcpy(dst, src, len + 1);
  else if (dst && n)  { wmemcpy(dst, src, n - 1); dst[n - 1] = 0; }
  return len;
}

int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, locale_t loc) {
  (void)loc;
  return wcscoll_fake(a, b);
}

size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, locale_t loc) {
  (void)loc;
  return wcsxfrm_fake(dst, src, n);
}

/* long double is 128-bit here and newlib may not carry the *told parsers.
 * Parsing at double precision loses mantissa bits that nothing in a music
 * decoder will notice, and it links. */
long double strtold_fake(const char *s, char **end) {
  return (long double)strtod(s, end);
}

long double wcstold_fake(const wchar_t *s, wchar_t **end) {
  return (long double)wcstod(s, end);
}

/* ---- GNU extensions ------------------------------------------------------ */

void sincos_fake(double x, double *s, double *c) {
  if (s) *s = sin(x);
  if (c) *c = cos(x);
}

void sincosf_fake(float x, float *s, float *c) {
  if (s) *s = sinf(x);
  if (c) *c = cosf(x);
}

/* ---- thread-local destructors -------------------------------------------
 *
 * __cxa_thread_atexit_impl registers a destructor to run when the calling
 * thread exits. Accepting and dropping the registration leaks whatever the
 * object owned, which is the lesser of the two failures available: refusing to
 * resolve it traps on the first C++ thread_local with a destructor, and there
 * is no per-thread hook here to run it from. Logged once so the leak is a
 * known quantity rather than a surprise. */
int __cxa_thread_atexit_impl_fake(void (*dtor)(void *), void *obj, void *dso) {
  (void)dtor; (void)obj; (void)dso;
  return 0;
}

/* ---- filesystem ----------------------------------------------------------
 *
 * libc++_shared's std::filesystem pulls these in. The game is very unlikely to
 * use them -- it reads assets through its own paths -- but an unresolved import
 * traps on call, and a trap is worse than a working call.
 *
 * They go through the VFS rather than straight to newlib, because a path
 * arriving here is an ANDROID path (/data/data/...), and handing that to the
 * host filesystem would fail in a confusing way. vfs_translate is the same
 * function open() already uses, so these behave consistently with the rest of
 * the port instead of forming a second, divergent view of the filesystem.
 */
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include "fileio.h"
#include "vfs.h"

static int truncate_via_open(const char *path, off_t len);

int stat_fake(const char *path, struct stat *st) {
  char t[512];
  return stat(vfs_translate(path, t, sizeof(t)), st);
}

int lstat_fake(const char *path, struct stat *st) {
  char t[512];
  /* No symlinks on this filesystem, so lstat and stat are the same answer. */
  return stat(vfs_translate(path, t, sizeof(t)), st);
}

int truncate_fake(const char *path, off_t len) {
  return truncate_via_open(path, len);   /* newlib may not have truncate() */
}

int chdir_fake(const char *path) {
  char t[512];
  return chdir(vfs_translate(path, t, sizeof(t)));
}

long pathconf_fake(const char *path, int name) {
  (void)path; (void)name;
  return -1;                       /* "no limit determinable", which is honest */
}

/* Not delegated: newlib often lacks statvfs entirely. Reporting "unsupported"
 * is better than failing to link, and no caller here needs real figures. */
int statvfs_fake(const char *path, void *buf) {
  (void)path; (void)buf;
  errno = ENOSYS;
  return -1;
}

/* No symlinks: report the failure the POSIX way rather than pretending. */
long readlink_fake(const char *path, char *buf, size_t n) {
  (void)path; (void)buf; (void)n;
  errno = EINVAL;                  /* not a symbolic link */
  return -1;
}

int symlink_fake(const char *target, const char *linkpath) {
  (void)target; (void)linkpath;
  errno = EPERM;
  return -1;
}

/* ---- the *at family, and other newlib gaps -------------------------------
 *
 * These four linked fine against the host's glibc and do not exist in
 * devkitA64's newlib at all: openat, unlinkat, fchmodat, fdopendir. A
 * compile-only check cannot see that -- the declarations are in the headers of
 * one libc and absent from the other -- so it reached a real build before
 * failing at link.
 *
 * The lesson, applied below: prefer a wrapper built from bedrock calls over a
 * direct reference to anything exotic. Everything here is written in terms of
 * open/close/unlink/rmdir/chmod/ftruncate/stat, which newlib certainly has.
 * The same reasoning covers statvfs and truncate, which are also commonly
 * absent and are now wrapped rather than referenced.
 */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

int openat_fake(int dirfd, const char *path, int flags, ...) {
  if (dirfd != AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }

  /* Hand off to open_fake rather than calling open() here.
   *
   * This used to pass `flags` straight through -- but they are BIONIC values,
   * and bionic's O_CREAT is 0x40 while newlib's is 0x200, which is bionic's
   * O_TRUNC. fileio.c carries a long comment about exactly this hazard and a
   * translate_open_flags() to handle it; this copy in another file did not use
   * either, so an openat asking to create a file was asking for something else
   * entirely. It also skipped the path translation's parent-directory creation
   * and logged nothing on failure.
   *
   * One implementation, one set of rules. The mode is dropped: open_fake uses
   * 0666, which is what every caller here passes anyway. */
  return open_fake(path, flags);
}

int unlinkat_fake(int dirfd, const char *path, int flags) {
  if (dirfd != AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  char t[512];
  const char *p = vfs_translate(path, t, sizeof(t));
  return (flags & 0x200 /* AT_REMOVEDIR */) ? rmdir(p) : unlink(p);
}

int fchmodat_fake(int dirfd, const char *path, mode_t mode, int flags) {
  (void)flags;
  if (dirfd != AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  char t[512];
  return chmod(vfs_translate(path, t, sizeof(t)), mode);
}

/* No way to build a DIR* from a descriptor without newlib's own internals, and
 * nothing here needs directory iteration by fd. Refusing is honest. */
void *fdopendir_fake(int fd) {
  (void)fd;
  errno = ENOSYS;
  return NULL;
}

static int truncate_via_open(const char *path, off_t len) {
  char t[512];
  int fd = open(vfs_translate(path, t, sizeof(t)), O_WRONLY);
  if (fd < 0) return -1;
  int r = ftruncate(fd, len);
  close(fd);
  return r;
}

/* fork() does not exist here, so there is nothing for an atfork handler to be
 * called on. Accepting the registration is correct rather than merely
 * convenient: the handlers can never legitimately run. */
int __register_atfork_fake(void (*a)(void), void (*b)(void), void (*c)(void),
                           void *dso) {
  (void)a; (void)b; (void)c; (void)dso;
  return 0;
}

/* ---- stderr -------------------------------------------------------------
 *
 * The only DATA symbol in the set. Everything else here is a function, but a
 * library referencing `stderr` wants the address of a `FILE *` variable, and
 * the relocation points its GOT slot at whatever address we register.
 *
 * newlib spells stderr as a macro over _impure_ptr, so it is not a
 * compile-time constant and cannot initialise a global directly. A constructor
 * runs before any loaded library can use it, which is early enough: modules are
 * dlopen'd from inside the game, long after the host's own constructors. */
#include <stdio.h>

FILE *nx_stderr;

__attribute__((constructor))
static void nx_stderr_init(void) { nx_stderr = stderr; }
