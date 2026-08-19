#ifndef PVZU_RUNTIME_GLUE_H
#define PVZU_RUNTIME_GLUE_H

#include <stdint.h>

const char *getenv_fake(const char *name);
int         setenv_fake(const char *name, const char *value, int overwrite);
void        runtime_env_dump(void);

int  __libc_current_sigrtmin_fake(void);
int  __libc_current_sigrtmax_fake(void);
int  pthread_kill_fake(void *thread, int sig);

/* Stage 6 go/no-go. Zero hijack attempts means no managed thread was in
 * cooperative mode when the GC ran, which is the outcome the port depends on. */
void     gc_watch_report(void);
unsigned gc_watch_hijack_count(void);

int sigaction_fake(int sig, const void *act, void *oldact);
int sigemptyset_fake(void *set);
int sigaddset_fake(void *set, int sig);
int pthread_sigmask_fake(int how, const void *set, void *old);

#endif
