#ifndef PVZU_FILEIO_H
#define PVZU_FILEIO_H

#include <stddef.h>
#include <stdint.h>

int  open_fake(const char *path, int flags, ...);
int  __open_2_fake(const char *path, int flags);
int  close_fake(int fd);
long read_fake(int fd, void *buf, size_t count);
long __read_chk_fake(int fd, void *buf, size_t count, size_t buflen);
long write_fake(int fd, const void *buf, size_t count);
long lseek64_fake(int fd, long offset, int whence);
long pread_fake(int fd, void *buf, size_t count, long offset);
long pwrite_fake(int fd, const void *buf, size_t count, long offset);

/* These fill bionic's struct layout, not newlib's. Do not be tempted to
 * forward them -- the field offsets differ and the failure is silent. */
int stat64_fake(const char *path, void *out);
int lstat64_fake(const char *path, void *out);
int fstat64_fake(int fd, void *out);
int statfs_fake(const char *path, void *out);
int fstatfs_fake(int fd, void *out);

int  mkdir_fake(const char *path, unsigned int mode);
int  rmdir_fake(const char *path);
int  unlink_fake(const char *path);
int  rename_fake(const char *from, const char *to);
int  access_fake(const char *path, int mode);
int  chmod_fake(const char *path, unsigned int mode);
int  fchmod_fake(int fd, unsigned int mode);
int  link_fake(const char *a, const char *b);
char *realpath_fake(const char *path, char *resolved);

void *opendir_fake(const char *path);
void *readdir_fake(void *dirp);
int   closedir_fake(void *dirp);

int  fcntl_fake(int fd, int cmd, ...);
int  ftruncate64_fake(int fd, long length);
int  fsync_fake(int fd);
int  flock_fake(int fd, int op);
int  fallocate_fake(int fd, int mode, long off, long len);
int  posix_fadvise64_fake(int fd, long off, long len, int advice);
int  futimens_fake(int fd, const void *times);
int  utimensat_fake(int dirfd, const char *p, const void *t, int f);
int  dup2_fake(int o, int n);
int  pipe_fake(int fds[2]);
long sendfile_fake(int out, int in, long *off, size_t count);

/* Create every missing directory along a path's parent chain. open(O_CREAT)
 * and fopen("w") do not create intermediate directories -- they fail with
 * ENOENT -- and the game writes its saves into subdirectories vfs_init does
 * not pre-create. Takes an ALREADY-TRANSLATED path. */
void mkdir_parents(const char *real);

#endif