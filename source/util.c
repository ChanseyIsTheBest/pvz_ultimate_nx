#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util.h"
#include "watchdog.h"

static FILE  *g_log;
static Mutex  g_log_lock;   /* zero-init is a valid unlocked libnx Mutex */
static Mutex  g_io_lock;

void log_init(const char *data_dir) {
  char path[256];
  snprintf(path, sizeof(path), "%s/debug.log", data_dir);
  g_log = fopen(path, "w");
  if (g_log) setvbuf(g_log, NULL, _IOLBF, 4096);
  /* log_write, not debug_log: this line must appear even when routine logging
     is compiled out, so that anyone opening a near-empty debug.log after a
     crash can tell the difference between "logging is off" and "it died before
     it could write anything". */
  log_write("[log] started -- routine logging %s\n",
            DEBUG_LOG ? "ON" : "OFF (built with DEBUG_LOG=0; crash dumps still land here)");
}

void log_shutdown(void) {
  mutexLock(&g_log_lock);
  if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
  mutexUnlock(&g_log_lock);
}

void log_write(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;

  /* Always emit to the debug console -- survives a log file that never opened. */
  svcOutputDebugString(buf, (u64)strnlen(buf, sizeof(buf)));

  mutexLock(&g_log_lock);
  if (g_log) { fputs(buf, g_log); fflush(g_log); }
  mutexUnlock(&g_log_lock);
}

void fatal_error(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  log_write("\n*** FATAL: %s\n", buf);

  /* Flush but do NOT close. This function parks the thread forever, and the
   * watchdog keeps running -- closing the log here threw away the stall report
   * that would have said which checkpoint we died at. */
  mutexLock(&g_log_lock);
  if (g_log) fflush(g_log);
  mutexUnlock(&g_log_lock);

  /* Tell the watchdog we failed on purpose.
   *
   * This function parks the thread forever, so the watchdog sees no progress
   * and reports a stall, then breaks for creport -- and a clean, fully
   * diagnosed failure arrives looking like a hang followed by a crash. The
   * reason is already printed above; the watchdog should say so rather than
   * bury it under a thread dump and a deliberate break. */
  watchdog_note_fatal();

  /* Hang rather than exit: on a title-override launch an immediate exit drops
   * you back to the menu with the message unread. */
  for (;;) svcSleepThread(1000000000ull);
}

/* ---- locked stdio ------------------------------------------------------- */

FILE *locked_fopen(const char *path, const char *mode) {
  mutexLock(&g_io_lock);
  FILE *f = fopen(path, mode);
  mutexUnlock(&g_io_lock);
  return f;
}

int locked_fclose(FILE *f) {
  mutexLock(&g_io_lock);
  int r = fclose(f);
  mutexUnlock(&g_io_lock);
  return r;
}

size_t locked_fread(void *p, size_t sz, size_t n, FILE *f) {
  mutexLock(&g_io_lock);
  size_t r = fread(p, sz, n, f);
  mutexUnlock(&g_io_lock);
  return r;
}

size_t locked_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
  mutexLock(&g_io_lock);
  size_t r = fwrite(p, sz, n, f);
  mutexUnlock(&g_io_lock);
  return r;
}

int locked_fseek(FILE *f, long off, int whence) {
  mutexLock(&g_io_lock);
  int r = fseek(f, off, whence);
  mutexUnlock(&g_io_lock);
  return r;
}

long locked_ftell(FILE *f) {
  mutexLock(&g_io_lock);
  long r = ftell(f);
  mutexUnlock(&g_io_lock);
  return r;
}
