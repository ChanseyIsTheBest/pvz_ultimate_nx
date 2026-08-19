#ifndef PVZU_ERRNO_BIONIC_H
#define PVZU_ERRNO_BIONIC_H

/* errno values as BIONIC defines them.
 *
 * errno crosses the boundary in the wrong direction: our shims set it, the
 * game reads it and compares against its own constants. Values 1..34 are
 * identical everywhere, but above that newlib and Linux diverge sharply --
 * newlib puts ENOSYS at 88 where Linux has 38, ENETDOWN at 115 vs 100,
 * ETIMEDOUT at 116 vs 110. Setting the newlib symbol means the game tests
 * `== ENOSYS` against 38, sees 88, and concludes something else went wrong.
 *
 * Spelled numerically on purpose: these must be the game's values, not
 * whatever our headers define. */
#define BIONIC_EPERM        1
#define BIONIC_ENOENT       2
#define BIONIC_EINTR        4
#define BIONIC_EIO          5
#define BIONIC_EBADF        9
#define BIONIC_EAGAIN      11
#define BIONIC_ENOMEM      12
#define BIONIC_EACCES      13
#define BIONIC_EFAULT      14
#define BIONIC_EBUSY       16
#define BIONIC_EEXIST      17
#define BIONIC_EINVAL      22
#define BIONIC_EMFILE      24
#define BIONIC_ENOTTY      25
#define BIONIC_ENOSPC      28
#define BIONIC_ESPIPE      29
#define BIONIC_EROFS       30
#define BIONIC_ERANGE      34
#define BIONIC_ENOSYS      38    /* newlib says 88 */
#define BIONIC_ENOTEMPTY   39    /* newlib says 90 */
#define BIONIC_ENETDOWN   100    /* newlib says 115 */
#define BIONIC_ETIMEDOUT  110    /* newlib says 116 */

#endif
