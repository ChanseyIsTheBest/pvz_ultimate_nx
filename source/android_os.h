#ifndef PVZU_ANDROID_OS_H
#define PVZU_ANDROID_OS_H

#include <stdint.h>
#include <jni.h>

/* Looper, HandlerThread, Handler and SurfaceHolder -- the plumbing the managed
 * SurfaceView builds during construction. */
void    android_os_init(void);

/* The SurfaceHolder singleton, returned by SurfaceView.getHolder(). */
jobject android_os_surfaceholder(void);

/* Run any Runnables whose delay has elapsed. Called once per frame: posted
 * work is meant to run on the looper's thread, and that is this one. */
void    android_os_run_posted(JNIEnv *env);

/* Records the thread that drains the queue. Call once, from that thread,
 * before the frame loop starts. */
void    android_os_mark_looper_thread(void);
int     android_os_on_looper_thread(void);

/* Run a Runnable's run() immediately on the calling thread, or queue it. */
void    android_os_run_now(JNIEnv *env, jobject runnable);
void    android_os_post_runnable(jobject runnable, uint64_t delay_ms);

#endif
