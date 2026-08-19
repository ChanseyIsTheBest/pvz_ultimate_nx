#ifndef PVZU_LIBC_SHIM_H
#define PVZU_LIBC_SHIM_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include <wchar.h>

void libc_shim_init(void);

int  *__errno_fake(void);

/* Bind this to the DATA symbol __sF, not to a function. See libc_shim.c for
 * why identification is by range rather than index. */
extern unsigned char __sF_fake[];

int    fprintf_fake(void *stream, const char *fmt, ...);
int    vfprintf_fake(void *stream, const char *fmt, va_list ap);
int    fputs_fake(const char *s, void *stream);
int    fputc_fake(int c, void *stream);
size_t fwrite_fake(const void *p, size_t sz, size_t n, void *stream);
int    fflush_fake(void *stream);
void  *fopen_fake(const char *path, const char *mode);
int    fclose_fake(void *f);
size_t fread_fake(void *p, size_t sz, size_t n, void *f);
long   getline_fake(char **lineptr, size_t *n, void *stream);
int    asprintf_fake(char **out, const char *fmt, ...);
int    vasprintf_fake(char **out, const char *fmt, va_list ap);

void  *__memcpy_chk_fake(void *d, const void *s, size_t n, size_t dlen);
void  *__memset_chk_fake(void *d, int c, size_t n, size_t dlen);
char  *__strcpy_chk_fake(char *d, const char *s, size_t dlen);
char  *__strncpy_chk2_fake(char *d, const char *s, size_t n, size_t dlen, size_t slen);
size_t __strlen_chk_fake(const char *s, size_t slen);
int    __vsnprintf_chk_fake(char *d, size_t n, int flag, size_t dlen,
                            const char *fmt, va_list ap);
void   __assert2_fake(const char *file, int line, const char *fn, const char *expr);
void   __stack_chk_fail_fake(void);

void  *newlocale_fake(int mask, const char *name, void *base);
void   freelocale_fake(void *loc);
void  *uselocale_fake(void *loc);
int    __ctype_get_mb_cur_max_fake(void);
double strtod_l_fake(const char *s, char **e, void *l);
long long strtoll_l_fake(const char *s, char **e, int b, void *l);
unsigned long long strtoull_l_fake(const char *s, char **e, int b, void *l);
long double strtold_l_fake(const char *s, char **e, void *l);
size_t strftime_l_fake(char *b, size_t n, const char *f, const struct tm *t, void *l);
int    iswlower_l_fake(wint_t c, void *l);

int clock_gettime_fake(int clk, struct timespec *ts);
int clock_nanosleep_fake(int clk, int flags, const struct timespec *req,
                         struct timespec *rem);
int nanosleep_fake(const struct timespec *req, struct timespec *rem);
int sched_yield_fake(void);

int sysinfo_fake(void *info);
int getrlimit_fake(int res, void *rl);
int getrusage_fake(int who, void *usage);
int sched_getaffinity_fake(int pid, size_t setsize, void *mask);
int __sched_cpucount_fake(size_t setsize, const void *mask);
int uname_fake(void *buf);
int prctl_fake(int option, unsigned long a, unsigned long b,
               unsigned long c, unsigned long d);
long syscall_fake(long number, ...);

int    getpid_fake(void);
int    getuid_fake(void);
int    geteuid_fake(void);
int    getegid_fake(void);
int    getgroups_fake(int size, void *list);
int    gethostname_fake(char *name, size_t len);
int    getpagesize_fake(void);
mode_t umask_fake(mode_t m);
mode_t __umask_chk_fake(mode_t m);
char  *getcwd_fake(char *buf, size_t size);
void   arc4random_buf_fake(void *buf, size_t n);
int    posix_memalign_fake(void **memptr, size_t alignment, size_t size);

int         net_fail_int(void);
void       *cmsg_nxthdr_fake(void *mhdr, void *cmsg);
void       *net_fail_ptr(void);
const char *gai_strerror_fake(int e);
void        freeaddrinfo_fake(void *ai);
unsigned    if_nametoindex_fake(const char *n);

void openlog_fake(const char *id, int opt, int fac);
void closelog_fake(void);
void syslog_fake(int prio, const char *fmt, ...);

int epoll_create1_fake(int flags);
int epoll_ctl_fake(int ep, int op, int fd, void *ev);
int epoll_wait_fake(int ep, void *events, int maxev, int timeout);
int poll_fake(void *fds, unsigned long nfds, int timeout);

#endif
