/* util.h -- logging + fatal error reporting.
 *
 * Wire this up FIRST. Everything downstream is debugged through debug_log().
 */
#ifndef PVZU_UTIL_H
#define PVZU_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Opens <data_dir>/debug.log and installs the crash handler. */
void log_init(const char *data_dir);
void log_shutdown(void);

/* Routine logging, compiled out entirely with DEBUG_LOG=0.
 *
 * Worth switching off for a release build: debug_log flushes on EVERY line, so
 * each one is an SD card write, and a normal session produces well over a
 * thousand. Crash reporting is deliberately NOT covered by this -- see
 * log_write below -- because a crash with no log is unhelpable, and a dump
 * costs nothing on a run that does not crash. */
#ifndef DEBUG_LOG
#define DEBUG_LOG 1
#endif

/* Always writes, whatever DEBUG_LOG says. The crash dumper and fatal_error use
 * this directly so a failure is still recorded in a release build. */
void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#if DEBUG_LOG
#  define debug_log(...) log_write(__VA_ARGS__)
#else
/* `if (0)` rather than an empty macro: the compiler still parses and
 * type-checks every call against -Wformat, then discards it. An empty macro
 * would let a formatting bug hide in whichever configuration you do not build,
 * and this project ships the one that is compiled out.
 *
 * Safe because no debug_log argument anywhere has a side effect -- checked by
 * script across all call sites, including the two that call auto_of(), which is
 * a pure lookup. */
#  define debug_log(...) do { if (0) log_write(__VA_ARGS__); } while (0)
#endif

/* Prints, flushes, then hangs so the message is readable. Does not return. */
void fatal_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---- Locked stdio -------------------------------------------------------
 * devkitPro's newlib handle table is NOT thread safe. A multithreaded .NET
 * runtime doing content loading plus save I/O will corrupt it. Every file call
 * that reaches newlib -- ours, the shim's, and nx_pointer's -- must go through
 * one of these. Pass locked_fopen/locked_fclose to NxpConfig.fopen_fn/fclose_fn.
 */
#include <stdio.h>
FILE  *locked_fopen(const char *path, const char *mode);
int    locked_fclose(FILE *f);
size_t locked_fread(void *p, size_t sz, size_t n, FILE *f);
size_t locked_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int    locked_fseek(FILE *f, long off, int whence);
long   locked_ftell(FILE *f);

#endif /* PVZU_UTIL_H */
