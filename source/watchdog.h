#ifndef PVZU_WATCHDOG_H
#define PVZU_WATCHDOG_H

/* Start the watchdog thread.
 *   warn_seconds   -- log a stall report and keep going
 *   break_seconds  -- svcBreak deliberately, so Atmosphere's creport runs and
 *                     dumps every thread's stack. A hang tells you nothing; a
 *                     crash report tells you everything.
 * Set break_seconds generously: a slow first-time asset load is not a hang. */
void watchdog_init(unsigned warn_seconds, unsigned break_seconds);
void watchdog_shutdown(void);

/* Name the phase you are entering. On a stall this label is most of the
 * diagnosis, so make it specific -- "Stage 3: JavaInteropRuntime.init" rather
 * than "init". Also resets the timer. */
void watchdog_checkpoint(const char *what);

/* Change the label WITHOUT resetting the timer.
 *
 * Use this inside a loop. watchdog_checkpoint resets the stall timer, so
 * calling it once per iteration means a loop that never terminates keeps
 * refreshing the timer and is never reported -- the one failure the watchdog
 * most needs to catch. Label from inside a loop, checkpoint on entering it. */
void watchdog_label(const char *what);

/* Reset the timer without changing the label. Call once per frame. */
void watchdog_beat(void);

/* Suspend and resume stall detection around something legitimately slow, such
 * as a first-run asset scan. Do not leave it disarmed. */
void watchdog_disarm(void);
void watchdog_rearm(void);

/* Record that execution stopped deliberately, so the watchdog reports the
 * failure already logged rather than treating the silence as a hang. */
void watchdog_note_fatal(void);

/* The last managed->native call that started, and whether it came back.
 *
 * A checkpoint says which phase we are in; this says which single call is
 * outstanding inside it. Both strings must have static lifetime -- they are
 * stored by pointer, not copied, so this stays cheap enough to run on every
 * JNI call. Pass NULL for cls when there is no class to name. */
void watchdog_note_call(const char *cls, const char *method);
void watchdog_note_return(void);

/* Print every thread's last JNI call and whether it is still inside it.
 *
 * Used by the stall report, and also callable directly: a stall only fires
 * when the frame loop stops beating, so a game that ticks happily while a
 * WORKER thread is wedged produces no report at all. That is the blind spot
 * this closes -- the threads the game creates for itself have never had any
 * diagnostics on them. */
void watchdog_report_calls(void);

#endif
