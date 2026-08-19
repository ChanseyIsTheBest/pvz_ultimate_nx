#ifndef PVZU_VFS_H
#define PVZU_VFS_H

#include <stddef.h>

void vfs_init(void);

/* Map an Android path onto the SD card. Returns either `path` unchanged (if it
 * was already a devoptab path) or `out`. Never returns NULL for non-NULL input. */
const char *vfs_translate(const char *path, char *out, size_t outsz);

/* /proc and /sys are served from memory rather than redirected. */
int  vfs_is_synthetic(const char *path);
int  vfs_open_synthetic(const char *path);

int  vfs_is_fake_fd(int fd);
long vfs_fake_read(int fd, void *buf, size_t count);
long vfs_fake_seek(int fd, long off, int whence);
long vfs_fake_size(int fd);
int  vfs_fake_close(int fd);

#endif
