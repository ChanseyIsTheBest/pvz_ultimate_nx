#ifndef PVZU_THREADS_H
#define PVZU_THREADS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* libnx page size, used for the default guard size in pthread_attr_init. */
#define PAGE_SIZE_DEFAULT 0x1000

void threads_init(void);

/* Dump every live thread with its stack range. Pair this with arena_report()
 * and gc_watch_report() -- together they answer "what was running, where were
 * its roots, and did the GC try to stop it". */
void threads_report(void);

/* PC/LR/SP of every live thread, from svcGetThreadContext3. The only way to
 * see a thread that is wedged inside managed code without waiting for a
 * crash. */
void threads_report_state(void);

/* Name lookup for the GC watch. Returns a static string, never NULL. */
const char *threads_name_of(void *pthread_handle);

/* The calling thread's name; "main" for the host thread. */
const char *threads_self_name(void);

/* Registry entry for a tid as reported by gettid, or NULL. */
void *threads_by_tid(int tid);

int   pthread_create_fake(void **out_thread, const void *attr,
                          void *(*entry)(void *), void *arg);
int   pthread_join_fake(void *thread, void **retval);
int   pthread_detach_fake(void *thread);
void *pthread_self_fake(void);
int   pthread_equal_fake(void *a, void *b);
int   pthread_setname_np_fake(void *thread, const char *name);
int   gettid_fake(void);

int pthread_attr_init_fake(void *attr);
int pthread_attr_destroy_fake(void *attr);
int pthread_attr_setstacksize_fake(void *attr, size_t sz);
int pthread_attr_getstacksize_fake(const void *attr, size_t *sz);
int pthread_attr_setdetachstate_fake(void *attr, int state);
int pthread_attr_getstack_fake(const void *attr, void **base, size_t *size);

/* The GC's conservative stack scan reads whatever this reports. Wrong bounds
 * are silent corruption, not a visible error. */
int pthread_getattr_np_fake(void *thread, void *attr);

int   pthread_key_create_fake(unsigned int *key, void (*dtor)(void *));
int   pthread_key_delete_fake(unsigned int key);
void *pthread_getspecific_fake(unsigned int key);
int   pthread_setspecific_fake(unsigned int key, const void *value);

int pthread_mutex_init_fake(void **m, const void *attr);
int pthread_mutex_destroy_fake(void **m);
int pthread_mutex_lock_fake(void **m);
int pthread_mutex_unlock_fake(void **m);
int pthread_mutex_trylock_fake(void **m);
int pthread_mutexattr_init_fake(void *a);
int pthread_mutexattr_destroy_fake(void *a);
int pthread_mutexattr_settype_fake(void *a, int type);

int pthread_cond_init_fake(void **c, const void *attr);
int pthread_cond_destroy_fake(void **c);
int pthread_cond_signal_fake(void **c);
int pthread_cond_broadcast_fake(void **c);
int pthread_cond_wait_fake(void **c, void **m);
int pthread_cond_timedwait_fake(void **c, void **m, const struct timespec *abs);

int pthread_once_fake(int *control, void (*init)(void));

int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);
int pthread_condattr_init_fake(void *a);
int pthread_condattr_destroy_fake(void *a);
int pthread_condattr_setclock_fake(void *a, int clk);

#endif
