/* fileio.c -- POSIX file layer with Android path redirection.
 *
 * The ABI hazard here is quieter than the mmap one and just as damaging:
 * bionic's `struct stat` is not newlib's. Sizes match closely enough that a
 * memcpy looks like it works, but the field OFFSETS differ, so the game reads
 * st_size out of what newlib put in st_blocks. Asset loaders that trust
 * st_size then allocate the wrong buffer. Everything below fills bionic's
 * layout explicitly, field by field, and the same reasoning applies to dirent.
 *
 * All real I/O goes through the locked wrappers in util.c. devkitPro's newlib
 * handle table is not thread safe and the runtime will be loading content from
 * worker threads.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

#include "errno_bionic.h"
#include "fileio.h"
#include "util.h"
#include "vfs.h"

/* ------------------------------------------------------------------------ */
/* bionic struct stat, LP64. 128 bytes.                                      */
/* ------------------------------------------------------------------------ */

struct bionic_stat {
  unsigned long  st_dev;
  unsigned long  st_ino;
  unsigned int   st_mode;
  unsigned int   st_nlink;
  unsigned int   st_uid;
  unsigned int   st_gid;
  unsigned long  st_rdev;
  unsigned long  __pad1;
  long           st_size;
  int            st_blksize;
  int            __pad2;
  long           st_blocks;
  long           st_atime_sec;  long st_atime_nsec;
  long           st_mtime_sec;  long st_mtime_nsec;
  long           st_ctime_sec;  long st_ctime_nsec;
  unsigned int   __unused4;
  unsigned int   __unused5;
};

/* Locked down deliberately. These structs are filled into memory the GAME
 * allocated, sized by the GAME's headers -- so sizeof() on our copy is only
 * safe while our copy is byte-identical. sysinfo was 16 bytes too large and
 * the overflow landed on a stack canary; the failure looked like a bug in the
 * game rather than in us. An assert costs nothing and makes the next
 * divergence a build error instead of a crash. */
_Static_assert(sizeof(struct bionic_stat) == 128,
               "bionic struct stat is 128 bytes on LP64");

static void fill_stat(struct bionic_stat *bs, const struct stat *ns) {
  memset(bs, 0, sizeof(*bs));
  bs->st_dev     = (unsigned long)ns->st_dev;
  bs->st_ino     = (unsigned long)ns->st_ino;
  bs->st_mode    = (unsigned int) ns->st_mode;
  bs->st_nlink   = (unsigned int) ns->st_nlink;
  bs->st_uid     = 1000;
  bs->st_gid     = 1000;
  bs->st_size    = (long)ns->st_size;
  bs->st_blksize = 4096;
  bs->st_blocks  = (long)((ns->st_size + 511) / 512);
  bs->st_atime_sec = (long)ns->st_atime;
  bs->st_mtime_sec = (long)ns->st_mtime;
  bs->st_ctime_sec = (long)ns->st_ctime;
}

/* devkitA64's newlib declares pread/pwrite in <unistd.h> but does not
 * implement them, so emulate with seek/read/restore.
 *
 * The three-syscall sequence has to be atomic with respect to anything else
 * touching the same descriptor -- the whole point of pread is that it does not
 * disturb the file position, and the runtime does content loading from worker
 * threads. Without the lock, one thread's restore lands after another's seek
 * and both read from the wrong offset. That surfaces as intermittently
 * corrupt assets, which is a miserable thing to track down. */
static Mutex g_pos_lock;

long pread_impl(int fd, void *buf, size_t count, long offset) {
  mutexLock(&g_pos_lock);
  long saved = lseek(fd, 0, SEEK_CUR);
  if (saved < 0) { mutexUnlock(&g_pos_lock); return -1; }
  long n = -1;
  if (lseek(fd, offset, SEEK_SET) >= 0)
    n = read(fd, buf, count);
  lseek(fd, saved, SEEK_SET);      /* restore even when the read failed */
  mutexUnlock(&g_pos_lock);
  return n;
}

long pwrite_impl(int fd, const void *buf, size_t count, long offset) {
  mutexLock(&g_pos_lock);
  long saved = lseek(fd, 0, SEEK_CUR);
  if (saved < 0) { mutexUnlock(&g_pos_lock); return -1; }
  long n = -1;
  if (lseek(fd, offset, SEEK_SET) >= 0)
    n = write(fd, buf, count);
  lseek(fd, saved, SEEK_SET);
  mutexUnlock(&g_pos_lock);
  return n;
}

/* ------------------------------------------------------------------------ */
/* open / close / read / write                                               */
/* ------------------------------------------------------------------------ */

/* bionic/Linux O_* values, spelled numerically because they are what the GAME
 * passes us -- not what our headers happen to define.
 *
 * The earlier comment here claimed the two agree and passed flags straight
 * through. That was an assumption, not a check: newlib historically uses
 * BSD-derived values (O_CREAT 0x200, O_TRUNC 0x400, O_APPEND 0x008) which do
 * not match Linux at all. If they disagree on this toolchain, a save opened
 * with O_CREAT|O_TRUNC gets flags that mean something else entirely, and the
 * failure looks like corrupt saves rather than a bad constant.
 *
 * Translating explicitly is correct whichever set newlib turns out to use. */
#define BIONIC_O_ACCMODE   000003
#define BIONIC_O_CREAT     000100
#define BIONIC_O_EXCL      000200
#define BIONIC_O_TRUNC     001000
#define BIONIC_O_APPEND    002000
#define BIONIC_O_NONBLOCK  004000
#define BIONIC_O_DIRECTORY 040000

static int translate_open_flags(int bionic_flags) {
  int f = bionic_flags & BIONIC_O_ACCMODE;   /* 0/1/2 are universal */
  if (bionic_flags & BIONIC_O_CREAT)    f |= O_CREAT;
  if (bionic_flags & BIONIC_O_EXCL)     f |= O_EXCL;
  if (bionic_flags & BIONIC_O_TRUNC)    f |= O_TRUNC;
  if (bionic_flags & BIONIC_O_APPEND)   f |= O_APPEND;
#ifdef O_NONBLOCK
  if (bionic_flags & BIONIC_O_NONBLOCK) f |= O_NONBLOCK;
#endif
  return f;
}

/* Create every missing directory along a path's parent chain.
 *
 * vfs_init pre-creates files/, cache/, external/, tmp/ and data/, but the game
 * writes its profile list into a SUBdirectory of files/, and open(O_CREAT) does
 * not create intermediate directories -- it fails with ENOENT. That is what
 * "Save profile list failed: Writing the profile list failed" was. */
void mkdir_parents(const char *real) {
  char tmp[512];
  size_t n = strlen(real);
  if (n >= sizeof(tmp)) return;
  memcpy(tmp, real, n + 1);

  /* Skip the "sdmc:/" prefix so the colon is never treated as a separator. */
  char *p = strchr(tmp, ':');
  p = p ? p + 1 : tmp;
  if (*p == '/') p++;

  for (; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    mkdir(tmp, 0777);          /* EEXIST is the normal case and is ignored */
    *p = '/';
  }
}

int open_fake(const char *path, int flags, ...) {
  if (vfs_is_synthetic(path)) return vfs_open_synthetic(path);

  char buf[512];
  const char *real = vfs_translate(path, buf, sizeof(buf));

  int fd = open(real, translate_open_flags(flags), 0666);

  /* Retry once, having built the parent directories.
   *
   * Only when the caller asked to create: a failed read of a file that is not
   * there is normal and must stay silent. */
  if (fd < 0 && (flags & BIONIC_O_CREAT)) {
    mkdir_parents(real);
    fd = open(real, translate_open_flags(flags), 0666);
    if (fd >= 0)
      debug_log("[io] created the parent directories for %s\n", real);
  }

  /* NOTE: the test is BIONIC_O_CREAT, not O_CREAT.
   *
   * `flags` are bionic values; newlib's O_CREAT is 0x200, which is bionic's
   * O_TRUNC. So `!(flags & O_CREAT)` was really "is O_TRUNC clear", and an
   * ordinary write open -- O_WRONLY|O_CREAT|O_TRUNC -- took the silent branch.
   * That is why a failing profile save produced no [io] line at all. */
  if (fd < 0)
    debug_log("[io] open(%s -> %s%s) failed: %s\n", path, real,
              (flags & BIONIC_O_CREAT) ? ", creating" : "", strerror(errno));

  /* Name every file the game WRITES.
   *
   * Level progress still is not persisting, and there is no evidence either
   * way: the last run contains no failing open and no successful write either,
   * so it is not yet known whether the game is saving to somewhere unexpected,
   * saving nothing, or saving fine and failing to read it back. One line per
   * write-open settles which. Reads are far too numerous to log and are not in
   * question. */
  else if (flags & (BIONIC_O_CREAT | 1 /*O_WRONLY*/ | 2 /*O_RDWR*/))
    debug_log("[io] writing %s\n", real);

  /* Report the size of the game's data bundles when they are opened.
   *
   * "Failed to load reanim from RSB" is the current blocker, the RSB is copied
   * out of the assets before it is read, and a truncated copy would look
   * exactly like this: the index at the front of the file parses, so the
   * bundle reports itself initialised, and only a resource further in fails.
   * The size is the cheapest way to tell a truncated bundle from a bundle that
   * is intact and simply does not contain what was asked for -- and those two
   * lead in completely different directions. */
  /* Every open of a .dat, whichever direction.
   *
   * The profile files are .dat and nothing else in the game is, so this is a
   * handful of lines per session and it answers the question that three rounds
   * of save work have not: does the game READ users.dat at boot, and does it
   * WRITE anything after a level. Writes are already named above; without the
   * reads there is no way to tell "saving is broken" from "the game is not
   * using these files at all". */
  if (strstr(real, ".dat")) {
    const char *how = (flags & (BIONIC_O_CREAT | 1 | 2)) ? "write" : "read";
    if (fd >= 0) debug_log("[io] %s open of %s\n", how, real);
    else         debug_log("[io] %s open of %s FAILED: %s\n", how, real,
                           strerror(errno));
  }

  if (fd >= 0 && strstr(real, ".rsb")) {
    off_t end = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    debug_log("[io] opened %s -- %lld bytes\n", real, (long long)end);
  }
  return fd;
}

/* bionic emits __open_2 under FORTIFY for the two-argument form. */
int __open_2_fake(const char *path, int flags) { return open_fake(path, flags); }

int close_fake(int fd) {
  if (vfs_is_fake_fd(fd)) return vfs_fake_close(fd);
  return close(fd);
}

/* Same reasoning as short writes, from the other direction: a read that stops
 * early leaves the caller with a partial buffer, and a decompressor handed a
 * partial buffer reports corrupt data rather than a truncated file. */
static void note_short_read(int fd, size_t asked, long got) {
  if (got == (long)asked || got < 0) {
    if (got < 0) debug_log("[fd %d] READ ERROR after asking for %zu\n", fd, asked);
    return;
  }
  static int told[64];
  int slot = fd & 63;
  if (told[slot]) return;
  told[slot] = 1;
  debug_log("[fd %d] short read: asked %zu, got %ld (end of file, or a "
            "caller that must loop)\n", fd, asked, got);
}

long read_fake(int fd, void *buf, size_t count) {
  if (vfs_is_fake_fd(fd)) return vfs_fake_read(fd, buf, count);
  long n = read(fd, buf, count);
  note_short_read(fd, count, n);
  return n;
}

long __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  if (count > buflen)
    debug_log("[fortify] read: %zu into a %zu buffer\n", count, buflen);
  return read_fake(fd, buf, count);
}

long write_fake(int fd, const void *buf, size_t count) {
  if (vfs_is_fake_fd(fd)) return (long)count;   /* writes to /proc: discard */

  long n = write(fd, buf, count);

  /* A short or failed write here is how a copied file ends up truncated, and
   * that is invisible afterwards: the beginning of the file is intact, so an
   * index or header at the start still parses and only a resource further in
   * fails. The game copies main.rsb out of its assets before reading it, the
   * RSB index lives at the front, and the failure is "load this one resource"
   * -- exactly that shape. Worth saying out loud rather than inferring.
   *
   * Once per descriptor: a genuinely short write usually repeats. */
  if (n != (long)count) {
    static int told[64];
    int slot = fd & 63;
    if (!told[slot]) {
      told[slot] = 1;
      debug_log("[fd %d] SHORT WRITE: asked %zu, wrote %ld%s\n",
                fd, count, n,
                n < 0 ? " (error -- the file being written is now truncated)"
                      : " (caller must loop, or the file is truncated)");
    }
  }
  return n;
}

long lseek64_fake(int fd, long offset, int whence) {
  if (vfs_is_fake_fd(fd)) return vfs_fake_seek(fd, offset, whence);
  return lseek(fd, offset, whence);
}

long pread_fake(int fd, void *buf, size_t count, long offset) {
  if (vfs_is_fake_fd(fd)) {
    long save = vfs_fake_seek(fd, 0, 1);
    vfs_fake_seek(fd, offset, 0);
    long n = vfs_fake_read(fd, buf, count);
    vfs_fake_seek(fd, save, 0);
    return n;
  }
  return pread_impl(fd, buf, count, offset);
}

long pwrite_fake(int fd, const void *buf, size_t count, long offset) {
  if (vfs_is_fake_fd(fd)) return (long)count;
  return pwrite_impl(fd, buf, count, offset);
}

/* ------------------------------------------------------------------------ */
/* stat family                                                               */
/* ------------------------------------------------------------------------ */

int stat64_fake(const char *path, void *out) {
  if (vfs_is_synthetic(path)) {
    /* Report a plausible regular file so callers that stat before opening do
     * not bail out early. */
    struct bionic_stat *bs = out;
    memset(bs, 0, sizeof(*bs));
    bs->st_mode = 0100444;
    bs->st_size = 4096;
    bs->st_blksize = 4096;
    return 0;
  }
  char buf[512];
  struct stat ns;
  if (stat(vfs_translate(path, buf, sizeof(buf)), &ns) != 0) return -1;
  fill_stat(out, &ns);
  return 0;
}

int lstat64_fake(const char *path, void *out) { return stat64_fake(path, out); }

int fstat64_fake(int fd, void *out) {
  if (vfs_is_fake_fd(fd)) {
    struct bionic_stat *bs = out;
    memset(bs, 0, sizeof(*bs));
    bs->st_mode = 0100444;
    bs->st_size = vfs_fake_size(fd);
    bs->st_blksize = 4096;
    return 0;
  }
  struct stat ns;
  if (fstat(fd, &ns) != 0) return -1;
  fill_stat(out, &ns);
  return 0;
}

/* bionic struct statfs, LP64. Only f_bsize/f_blocks/f_bavail get read in
 * practice -- free-space checks before a save. */
struct bionic_statfs {
  unsigned long f_type, f_bsize, f_blocks, f_bfree, f_bavail;
  unsigned long f_files, f_ffree;
  struct { int val[2]; } f_fsid;
  unsigned long f_namelen, f_frsize, f_flags, f_spare[4];
};

_Static_assert(sizeof(struct bionic_statfs) == 120,
               "bionic struct statfs is 120 bytes on LP64");

static void fill_statfs(struct bionic_statfs *sf) {
  memset(sf, 0, sizeof(*sf));
  sf->f_type    = 0x4d44;      /* MSDOS_SUPER_MAGIC -- the SD card is FAT/exFAT */
  sf->f_bsize   = 4096;
  sf->f_blocks  = 1024ul * 1024ul;   /* 4 GB reported free-ish; the game only
                                      * needs "enough for a save file" */
  sf->f_bfree   = 512ul * 1024ul;
  sf->f_bavail  = 512ul * 1024ul;
  sf->f_namelen = 255;
  sf->f_frsize  = 4096;
}

int statfs_fake(const char *path, void *out)  { (void)path; fill_statfs(out); return 0; }
int fstatfs_fake(int fd, void *out)           { (void)fd;   fill_statfs(out); return 0; }

/* ------------------------------------------------------------------------ */
/* Directory operations                                                      */
/* ------------------------------------------------------------------------ */

int mkdir_fake(const char *path, unsigned int mode) {
  char buf[512];
  return mkdir(vfs_translate(path, buf, sizeof(buf)), mode);
}
int rmdir_fake(const char *path) {
  char buf[512];
  return rmdir(vfs_translate(path, buf, sizeof(buf)));
}
int unlink_fake(const char *path) {
  char buf[512];
  return unlink(vfs_translate(path, buf, sizeof(buf)));
}
int rename_fake(const char *from, const char *to) {
  char b1[512], b2[512];
  /* Translate both before either call -- vfs_translate uses the caller's
   * buffer, so sharing one would clobber the first path. */
  const char *f = vfs_translate(from, b1, sizeof(b1));
  const char *t = vfs_translate(to,   b2, sizeof(b2));

  int rc = rename(f, t);

  /* POSIX rename REPLACES an existing destination. FAT-derived filesystems --
   * which is what the SD card is -- do not: they fail when the target already
   * exists.
   *
   * That asymmetry is exactly the shape of the reported bug. A save written as
   * "write user1.dat.tmp, then rename it over user1.dat" succeeds the FIRST
   * time, when there is nothing to replace, and fails silently every time
   * afterwards. The file exists, has plausible contents, and never updates
   * again -- which is precisely what the save directory shows.
   *
   * Not atomic, and it cannot be here: the window between the unlink and the
   * rename is unavoidable without filesystem support. It is still the right
   * trade, because the alternative is a save that never lands at all. */
  if (rc != 0) {
    int first_errno = errno;
    if (access(t, 0) == 0 && unlink(t) == 0) {
      rc = rename(f, t);
      if (rc == 0)
        debug_log("[io] rename onto an existing %s needed the destination "
                  "removed first (the SD card is not POSIX here)\n", t);
    }
    if (rc != 0)
      debug_log("[io] rename(%s -> %s) FAILED: %s\n", f, t,
                strerror(first_errno));
  }

  if (rc == 0 && strstr(t, ".dat"))
    debug_log("[io] renamed into place: %s\n", t);
  return rc;
}
int access_fake(const char *path, int mode) {
  if (vfs_is_synthetic(path)) return 0;
  char buf[512];
  return access(vfs_translate(path, buf, sizeof(buf)), mode);
}
int chmod_fake(const char *path, unsigned int mode) { (void)path; (void)mode; return 0; }
int fchmod_fake(int fd, unsigned int mode)          { (void)fd;   (void)mode; return 0; }
int link_fake(const char *a, const char *b)         { (void)a; (void)b; errno = BIONIC_EPERM; return -1; }

char *realpath_fake(const char *path, char *resolved) {
  char buf[512];
  const char *real = vfs_translate(path, buf, sizeof(buf));
  if (!resolved) resolved = malloc(512);
  if (resolved) snprintf(resolved, 512, "%s", real);
  return resolved;
}

/* bionic struct dirent, LP64. d_name at offset 19 -- newlib's differs, and a
 * mismatch here shows up as asset filenames that are subtly truncated or
 * shifted, which is a miserable thing to debug from the symptoms. */
struct bionic_dirent {
  uint64_t       d_ino;
  int64_t        d_off;
  unsigned short d_reclen;
  unsigned char  d_type;
  char           d_name[256];
};

_Static_assert(sizeof(struct bionic_dirent) == 280,
               "bionic struct dirent is 280 bytes on LP64");

typedef struct { DIR *real; struct bionic_dirent ent; } FakeDir;

void *opendir_fake(const char *path) {
  char buf[512];
  DIR *d = opendir(vfs_translate(path, buf, sizeof(buf)));
  if (!d) return NULL;
  FakeDir *fd = calloc(1, sizeof(FakeDir));
  if (!fd) { closedir(d); return NULL; }
  fd->real = d;
  return fd;
}

void *readdir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd) return NULL;
  struct dirent *de = readdir(fd->real);
  if (!de) return NULL;
  memset(&fd->ent, 0, sizeof(fd->ent));
  fd->ent.d_ino    = 1;
  fd->ent.d_reclen = (unsigned short)sizeof(fd->ent);
  fd->ent.d_type   = de->d_type;
  snprintf(fd->ent.d_name, sizeof(fd->ent.d_name), "%s", de->d_name);
  return &fd->ent;
}

int closedir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd) return -1;
  int r = closedir(fd->real);
  free(fd);
  return r;
}

/* ------------------------------------------------------------------------ */
/* Odds and ends                                                             */
/* ------------------------------------------------------------------------ */

int fcntl_fake(int fd, int cmd, ...) {
  (void)fd;
  /* F_SETFD/F_SETFL are the only ones the runtime uses in anger and neither
   * means anything here. F_GETFL (3) should report a readable/writable fd. */
  if (cmd == 3) return 2 /* O_RDWR */;
  return 0;
}

int ftruncate64_fake(int fd, long length) { return ftruncate(fd, length); }
/* No-op rather than a call into newlib: fsync is frequently absent on embedded
 * targets, and the SD card devoptab commits on close regardless. Reporting
 * success is honest -- there is no writeback cache of ours to flush. */
int fsync_fake(int fd)                    { (void)fd; return 0; }
int flock_fake(int fd, int op)            { (void)fd; (void)op; return 0; }
int fallocate_fake(int fd, int mode, long off, long len) {
  (void)fd; (void)mode; (void)off; (void)len; return 0;
}
int posix_fadvise64_fake(int fd, long off, long len, int advice) {
  (void)fd; (void)off; (void)len; (void)advice; return 0;
}
int futimens_fake(int fd, const void *times)  { (void)fd; (void)times; return 0; }
int utimensat_fake(int dirfd, const char *p, const void *t, int f) {
  (void)dirfd; (void)p; (void)t; (void)f; return 0;
}
int dup2_fake(int o, int n) { (void)o; (void)n; return n; }
int pipe_fake(int fds[2])   { fds[0] = fds[1] = -1; errno = BIONIC_ENOSYS; return -1; }

long sendfile_fake(int out, int in, long *off, size_t count) {
  /* Copy by hand; newlib has no sendfile. Used by the runtime for file moves. */
  char buf[8192];
  size_t total = 0;
  if (off) lseek(in, *off, SEEK_SET);
  while (total < count) {
    size_t want = count - total;
    if (want > sizeof(buf)) want = sizeof(buf);
    long n = read(in, buf, want);
    if (n <= 0) break;
    if (write(out, buf, (size_t)n) != n) break;
    total += (size_t)n;
  }
  if (off) *off += (long)total;
  return (long)total;
}
