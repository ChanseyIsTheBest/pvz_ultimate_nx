/* ime_shim.c -- Android's InputMethodManager, backed by the Switch keyboard.
 *
 * WHY
 *
 *     [ctx] getSystemService(input_method) -> null (not implemented).
 *
 * The game overrides onCreateInputConnection and onCheckIsTextEditor, so it is
 * a text-editing view and expects to raise a soft keyboard. With no
 * InputMethodManager there was nothing to raise, and no way to type a name.
 *
 * HOW TEXT GETS BACK IN
 *
 * Android's own answer for a View that is not a full editor is
 * `BaseInputConnection(view, false)`, whose commitText turns the committed
 * string into KEY EVENTS delivered to the view. So synthesising key events with
 * the unicode character set is not a workaround -- it is what the framework
 * does for exactly this kind of view, and the shim already has every piece:
 * `android_make_key_event` takes a unicode argument and the fake KeyEvent
 * implements getUnicodeChar().
 *
 * If the game returns an InputConnection that has a REAL commitText -- one
 * registered through RegisterNatives rather than auto-manufactured -- that is
 * used instead, because a full editor will not be listening for key events.
 * The two are chosen between by inspecting the method table BEFORE calling, so
 * no auto-stub is manufactured and text is never delivered twice.
 *
 * WHY IT IS DEFERRED
 *
 * swkbdShow blocks until the user is done, and showSoftInput is called from
 * managed code inside DoFrame. Blocking there would hold a managed->native
 * transition open for as long as someone takes to type, inside a frame call,
 * with the GC's cooperative-mode invariant to worry about. So showSoftInput
 * only records the request and returns; the keyboard runs from `ime_pump()` at
 * the top of the frame loop, as a clean top-level call.
 *
 * That is also the more faithful shape: Android's showSoftInput is asynchronous
 * and returns immediately, with text arriving later through the
 * InputConnection.
 *
 * THE WATCHDOG MUST BE DISARMED
 *
 * It breaks into creport after 45 seconds without progress. Someone typing a
 * name on a thumbstick keyboard will exceed that easily, and the resulting
 * "hang" would be the user reading the screen.
 */

#include <string.h>
#include <stdio.h>

#include <switch.h>

#include "android_classes.h"
#include "ime_shim.h"
#include "android_text.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "lawn_natives.h"
#include "util.h"
#include "watchdog.h"

#define JV_ZERO jvalue r; memset(&r, 0, sizeof(r))

/* Android keycodes we need. */
#define AKEYCODE_DEL     67
#define AKEYCODE_ENTER   66

/* Send ENTER after the text. swkbd's OK button is a deliberate confirm, and a
 * hardware keyboard would produce this; without it a field that ends on Done
 * may never close. Set to 0 if it turns out to double-confirm something. */
#define IME_SEND_ENTER 1

#define IME_MAX_TEXT 256

/* Backspaces sent before new text. Generous on purpose: the field length is
 * unknowable from here, and a backspace on an empty field is a no-op. */
#define IME_CLEAR_KEYS 64

/* ------------------------------------------------------------- request --- */

static volatile int g_pending;          /* a showSoftInput is waiting        */
static int          g_shown;            /* the keyboard is currently up      */

/* The best guess at what the field currently holds.
 *
 * The shim cannot read the game's text: the field belongs to the engine's own
 * widget, drawn by the engine, and the InputConnection it hands back has no
 * real methods to query. So the keyboard opens empty the first time even when
 * the field visibly contains something -- which is what "the keyboard is blank"
 * was reporting.
 *
 * What CAN be done is remember what this shim last put there, so a second visit
 * opens pre-filled and editable rather than blank. And, far more importantly,
 * the field is CLEARED before new text is typed -- see deliver(). Without that
 * the delivery appends, and editing a profile called "Chomper" to "Bob" would
 * have produced "ChomperBob". */
static char g_field[IME_MAX_TEXT];

/* --------------------------------------------------------- EditorInfo ---- */

/* The game fills this in to describe the field it wants. We pass it through
 * and read nothing back: honouring inputType (password, number) would be an
 * improvement, but guessing at it wrongly would give the wrong keyboard, and
 * the default is right for a name. */
static FakeObject g_editorinfo_obj;
static FakeClass  g_class_EditorInfo = {
  {NULL}, "android/view/inputmethod/EditorInfo", NULL, NULL, 0, NULL, 0, 0
};

/* -------------------------------------------------- InputMethodManager --- */

/* Name every method the game calls on the manager, once each.
 *
 * The keyboard not appearing had no signature at all: the game held the
 * manager and simply never asked. Knowing WHICH methods it does call is the
 * difference between "it never tried" and "it tried and something we answered
 * talked it out of it". */
static void imm_seen(const char *what) {
  static const char *seen[12];
  static int n;
  for (int i = 0; i < n; i++) if (seen[i] == what) return;
  if (n < 12) seen[n++] = what;
  debug_log("[ime] the game called InputMethodManager.%s\n", what);
}

static jvalue imm_showSoftInput(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  imm_seen("showSoftInput");
  JV_ZERO;
  if (!g_pending) {
    g_pending = 1;
    debug_log("[ime] showSoftInput -- the Switch keyboard will open at the top "
              "of the next frame\n");
  }
  r.z = JNI_TRUE;                 /* "yes, a keyboard will be shown"         */
  return r;
}

static jvalue imm_hide(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  imm_seen("hideSoftInputFromWindow");
  JV_ZERO;
  /* Nothing to hide: swkbd is modal and already gone by the time anything can
   * ask. Cancelling a queued request is meaningful though. */
  if (g_pending) {
    g_pending = 0;
    debug_log("[ime] hideSoftInput -- cancelled the queued keyboard\n");
  }
  r.z = JNI_TRUE;
  return r;
}

/* isActive means "is the IME accepting input for this view", not "is the
 * keyboard visible right now". Returning false whenever the modal keyboard
 * happens to be closed -- which is almost always -- invites the game to decide
 * there is no IME and give up before ever calling showSoftInput. */
static jvalue imm_isActive(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  imm_seen("isActive");
  JV_ZERO; r.z = JNI_TRUE; return r;
}

static jvalue imm_void(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  imm_seen("restartInput/viewClicked/updateSelection");
  JV_ZERO; return r;
}

static jvalue imm_toggle(JNIEnv *e, jobject self, const jvalue *a) {
  return imm_showSoftInput(e, self, a);
}

static FakeMethod g_imm_methods[] = {
  { imm_showSoftInput, "showSoftInput",              "(Landroid/view/View;I)Z",                     NULL, 0 },
  { imm_showSoftInput, "showSoftInput",              "(Landroid/view/View;ILandroid/os/ResultReceiver;)Z", NULL, 0 },
  { imm_toggle,        "toggleSoftInput",            "(II)V",                                       NULL, 0 },
  { imm_toggle,        "showSoftInputFromInputMethod","(Landroid/os/IBinder;I)V",                   NULL, 0 },
  { imm_hide,          "hideSoftInputFromWindow",    "(Landroid/os/IBinder;I)Z",                    NULL, 0 },
  { imm_hide,          "hideSoftInputFromWindow",    "(Landroid/os/IBinder;ILandroid/os/ResultReceiver;)Z", NULL, 0 },
  { imm_isActive,      "isActive",                   "()Z",                                         NULL, 0 },
  { imm_isActive,      "isActive",                   "(Landroid/view/View;)Z",                      NULL, 0 },
  { imm_isActive,      "isAcceptingText",            "()Z",                                         NULL, 0 },
  { imm_void,          "restartInput",               "(Landroid/view/View;)V",                      NULL, 0 },
  { imm_void,          "viewClicked",                "(Landroid/view/View;)V",                      NULL, 0 },
  { imm_void,          "updateSelection",            "(Landroid/view/View;IIII)V",                  NULL, 0 },
};

static FakeObject g_imm_obj;
static FakeClass  g_class_IMM = {
  {NULL}, "android/view/inputmethod/InputMethodManager", NULL,
  g_imm_methods, (int)(sizeof(g_imm_methods) / sizeof(g_imm_methods[0])),
  NULL, 0, 0
};

jobject ime_input_method_manager(void) {
  return jniref_new(&g_imm_obj, REF_LOCAL);
}

/* ------------------------------------------------------------ delivery --- */

/* Is this a method the game actually implements, rather than one jni_fake would
 * manufacture on demand? Walking the table is side-effect free -- asking through
 * GetMethodID would CREATE the stub and then we could not tell the difference. */
static int has_real_method(FakeClass *c, const char *name) {
  for (FakeClass *k = c; k; k = k->super)
    for (int i = 0; i < k->nmethods; i++)
      if (!strcmp(k->methods[i].name, name)) return 1;
  return 0;
}

/* Ask the view for its InputConnection, exactly as the framework would when a
 * field takes focus. Returns NULL if the game does not supply one. */
static jobject make_input_connection(void) {
  JNIEnv *env = jni_get_env();
  jobject ei  = jniref_new(&g_editorinfo_obj, REF_LOCAL);
  return lawn_create_input_connection(env, ei);
}

/* The Android keycode for an ASCII character, or 0 if there is not a sensible
 * one. Filling this in matters because there are two ways a view can read text
 * out of a KeyEvent -- getUnicodeChar() and getKeyCode() -- and which one this
 * engine uses is not visible from here. Sending only the unicode would work for
 * the first and silently do nothing for the second. */
static int keycode_for(int cp) {
  if (cp >= 'a' && cp <= 'z') return 29 + (cp - 'a');   /* AKEYCODE_A  */
  if (cp >= 'A' && cp <= 'Z') return 29 + (cp - 'A');
  if (cp >= '0' && cp <= '9') return  7 + (cp - '0');   /* AKEYCODE_0  */
  switch (cp) {
    case ' ':  return 62;    /* SPACE  */
    case ',':  return 55;    /* COMMA  */
    case '.':  return 56;    /* PERIOD */
    case '-':  return 69;    /* MINUS  */
    case '=':  return 70;    /* EQUALS */
    case '/':  return 76;    /* SLASH  */
    case '@':  return 77;    /* AT     */
    case '+':  return 81;    /* PLUS   */
    default:   return 0;
  }
}

/* One UTF-8 character -> one down/up pair.
 *
 * The unicode is always exact. The keycode is a best effort, and META_SHIFT_ON
 * goes with a capital so a keycode-reading view can tell A from a -- without
 * it that path would render every name in lower case. */
static void send_char(int unicode) {
  int kc   = keycode_for(unicode);
  int meta = (unicode >= 'A' && unicode <= 'Z') ? 0x1 : 0;   /* META_SHIFT_ON */
  lawn_key_full(0 /*down*/, kc, unicode, meta);
  lawn_key_full(1 /*up*/,   kc, unicode, meta);
}

static void send_keycode(int keycode) {
  lawn_key_unicode(0, keycode, 0);
  lawn_key_unicode(1, keycode, 0);
}

/* Minimal UTF-8 decode. swkbd returns UTF-8; the fake KeyEvent carries an int
 * code point, so anything the on-screen keyboard can produce survives. */
static const char *utf8_next(const char *s, int *out) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80)              { *out = c;                    return s + 1; }
  if ((c & 0xE0) == 0xC0 && s[1]) {
    *out = ((c & 0x1F) << 6) | (s[1] & 0x3F);               return s + 2;
  }
  if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
    *out = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return s + 3;
  }
  if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
    *out = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
           ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return s + 4;
  }
  *out = c;                                                 return s + 1;
}

/* Invoke a no-argument InputConnection method, if the class really implements
 * it. Quiet when it does not -- these are protocol niceties, not requirements,
 * and a missing one should not stop the text going in. */
static void ic_simple_call(JNIEnv *env, jobject ic, jclass c,
                           const char *name, const char *sig) {
  FakeObject *o = jniref_deref(ic);
  if (!o || !o->cls || !c || !has_real_method(o->cls, name)) return;
  jmethodID mid = (*env)->GetMethodID(env, c, name, sig);
  if (!mid) return;
  (*env)->CallBooleanMethod(env, ic, mid);
  (*env)->ExceptionClear(env);
  debug_log("[ime] InputConnection.%s\n", name);
}

static void deliver(const char *text) {
  /* Prefer a real InputConnection: a full editor will not be watching key
   * events, and would ignore everything we sent. */
  jobject ic = make_input_connection();
  if (ic) {
    FakeObject *o = jniref_deref(ic);
    if (o && o->cls && has_real_method(o->cls, "commitText")) {
      JNIEnv *env = jni_get_env();
      jclass    c   = (*env)->GetObjectClass(env, ic);
      jmethodID mid = c ? (*env)->GetMethodID(env, c, "commitText",
                              "(Ljava/lang/CharSequence;I)Z") : NULL;
      /* Call a no-argument InputConnection method by name, if it is real.
       * Returns 1 if it ran. */
      if (mid) {
        /* Clear through the IME protocol, not with a burst of backspaces.
         *
         * deleteSurroundingText(n, n) removes n characters either side of the
         * cursor, which empties the buffer wherever the cursor happens to be --
         * and it is one call into the game's own editable rather than 64
         * synthetic key events that a full editor may not even be watching.
         * The key-event path below still uses backspaces, because that is all
         * it has. */
        ic_simple_call(env, ic, c, "beginBatchEdit", "()Z");

        /* The editable is the game's own buffer, and writing to it is the
         * delivery. commitText was reaching BaseInputConnection and finding
         * nothing to write -- the CharSequence does not survive the managed
         * round trip -- so this stops depending on that argument at all. The
         * game reads the buffer back with toString(), which is the half that
         * demonstrably works. */
        jobject ed = NULL;
        jmethodID ge = (*env)->GetMethodID(env, c, "getEditable",
                                           "()Landroid/text/Editable;");
        if (ge) {
          ed = (*env)->CallObjectMethod(env, ic, ge);
          (*env)->ExceptionClear(env);
        }

        if (ed && android_text_set(ed, text)) {
          debug_log("[ime] wrote %zu bytes straight into the editable\n",
                    strlen(text));
          ic_simple_call(env, ic, c, "finishComposingText", "()Z");
          ic_simple_call(env, ic, c, "endBatchEdit", "()Z");
#if IME_SEND_ENTER
          send_keycode(AKEYCODE_ENTER);
#endif
          return;
        }

        debug_log("[ime] the InputConnection has no editable this shim can "
                  "write to; falling back to commitText\n");

        size_t had = 0;
        { const char *cur = ed ? android_text_get(ed) : NULL;
          had = cur ? strlen(cur) : 0; }

        if (had && has_real_method(o->cls, "deleteSurroundingText")) {
          jmethodID del = (*env)->GetMethodID(env, c, "deleteSurroundingText",
                                              "(II)Z");
          if (del) {
            (*env)->CallBooleanMethod(env, ic, del, (jint)had, (jint)had);
            (*env)->ExceptionClear(env);
          }
        }

        debug_log("[ime] delivering %zu bytes through InputConnection."
                  "commitText, after clearing %zu\n", strlen(text), had);
        (*env)->CallBooleanMethod(env, ic, mid, jni_make_string(text), 1);
        (*env)->ExceptionClear(env);

        /* Finish the edit properly.
         *
         * A real IME brackets its changes with beginBatchEdit/endBatchEdit and
         * ends the session with finishComposingText. An editor is entitled to
         * hold the change in its buffer and only push it to the widget when the
         * batch ends -- which would leave the field looking exactly as it does
         * now: cleared, and then apparently never written to. Skipping these
         * was violating the contract, not taking a shortcut. */
        ic_simple_call(env, ic, c, "finishComposingText", "()Z");
        ic_simple_call(env, ic, c, "endBatchEdit", "()Z");

#if IME_SEND_ENTER
        send_keycode(AKEYCODE_ENTER);
#endif
        return;
      }
    }
    debug_log("[ime] the view's InputConnection has no commitText of its own, "
              "so text goes in as key events -- which is what Android's own "
              "BaseInputConnection(view, false) does for a view like this\n");
  } else {
    debug_log("[ime] the view supplied no InputConnection; delivering as key "
              "events\n");
  }

  /* Clear first, or this appends to whatever is already there.
   *
   * The keyboard returns the WHOLE intended contents of the field, not an
   * insertion, so anything already present has to go. Nothing tells us how long
   * it is, so send a generous run of backspaces: on a shorter field the extras
   * land on an empty field and do nothing, which is exactly what a real
   * keyboard's backspace does. */
  int clears = IME_CLEAR_KEYS;
  for (int i = 0; i < clears; i++) send_keycode(AKEYCODE_DEL);

  int n = 0;
  for (const char *p = text; *p;) {
    int cp = 0;
    p = utf8_next(p, &cp);
    if (cp == '\n' || cp == '\r') continue;
    send_char(cp);
    n++;
  }
  debug_log("[ime] cleared with %d backspaces, then delivered %d characters as "
            "key events\n", clears, n);
#if IME_SEND_ENTER
  send_keycode(AKEYCODE_ENTER);
#endif
}

/* What the field currently holds, via the InputConnection's editable.
 *
 * This is why the keyboard used to open blank on a field reading "Nezha
 * Shooter": there was no way to see the text. There is now -- getEditable()
 * returns a real buffer (android_text.c), and the editable is the same object
 * the game keeps its field contents in. Returns 0 if it cannot be read, which
 * is not a failure, just an empty keyboard. */
static int read_current_text(char *out, size_t n) {
  out[0] = '\0';

  jobject ic = make_input_connection();
  if (!ic) return 0;

  FakeObject *o = jniref_deref(ic);
  if (!o || !o->cls || !has_real_method(o->cls, "getEditable")) return 0;

  JNIEnv   *env = jni_get_env();
  jclass    c   = (*env)->GetObjectClass(env, ic);
  jmethodID mid = c ? (*env)->GetMethodID(env, c, "getEditable",
                                          "()Landroid/text/Editable;") : NULL;
  if (!mid) return 0;

  jobject ed = (*env)->CallObjectMethod(env, ic, mid);
  (*env)->ExceptionClear(env);
  if (!ed) return 0;

  const char *txt = android_text_get(ed);
  if (!txt || !txt[0]) return 0;

  snprintf(out, n, "%s", txt);
  debug_log("[ime] the field currently holds %zu bytes; opening the keyboard "
            "on it rather than blank\n", strlen(out));
  return 1;
}

/* ---------------------------------------------------------------- pump --- */

void ime_pump(void) {
  /* Raise the keyboard when the view becomes a text editor.
   *
   * The game holds an InputMethodManager and never calls showSoftInput, so
   * nothing else would ever trigger. onCheckIsTextEditor is the framework's own
   * test for "should a keyboard be up", the game overrides it, and it is the
   * one honest signal available. Only the false->true edge fires, so a field
   * that stays open does not reopen the keyboard every frame. */
  {
    static int was_editor, seeded;
    static unsigned tick;
    if (!g_shown && ++tick >= 15) {          /* ~4 Hz; it is a JNI call */
      tick = 0;
      int now = lawn_is_text_editor() ? 1 : 0;

      /* Do not fire on the FIRST observation, only on a genuine change.
       *
       * If the view reports itself a text editor permanently -- which a custom
       * view that always accepts IME quite reasonably does -- an edge measured
       * from an assumed `false` fires once, a quarter of a second into the
       * game, and throws the keyboard over the main menu. Seeding costs one
       * missed raise in the case where a field really is already open at
       * startup, and Y covers that. */
      if (!seeded) { seeded = 1; was_editor = now; }
      else if (now && !was_editor) {
        debug_log("[ime] the view has become a text editor -- raising the "
                  "keyboard without waiting for showSoftInput\n");
        ime_request_keyboard();
      }
      was_editor = now;
    }
  }

  if (!g_pending) return;
  g_pending = 0;

  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_FAILED(rc)) {
    debug_log("[ime] swkbdCreate failed (0x%x) -- no keyboard this time\n",
              (unsigned)rc);
    return;
  }

  /* Default rather than the UserName preset: UserName applies
   * SwkbdKeyDisableBitmask_UserName, which removes characters the game may
   * legitimately want, and this manager serves every field the game raises,
   * not only the name. The return button is switched off because this is a
   * single-line field -- newlines are stripped on delivery anyway, and the
   * confirm is the OK button. */
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetReturnButtonFlag(&kbd, 0);
  swkbdConfigSetStringLenMax(&kbd, IME_MAX_TEXT - 1);
  swkbdConfigSetStringLenMin(&kbd, 0);
  swkbdConfigSetBlurBackground(&kbd, 1);

  /* Pre-fill from the field itself where possible, falling back to whatever
   * this shim last put there. */
  char current[IME_MAX_TEXT];
  if (!read_current_text(current, sizeof(current)))
    snprintf(current, sizeof(current), "%s", g_field);
  if (current[0]) swkbdConfigSetInitialText(&kbd, current);

  /* Say plainly that this replaces the field, because it does, and because
   * confirming an empty keyboard on a field that already reads "Chomper" would
   * otherwise look like it should clear it. */
  swkbdConfigSetHeaderText(&kbd, "Text entry");
  swkbdConfigSetSubText(&kbd, "This replaces the whole field");
  swkbdConfigSetGuideText(&kbd, current[0] ? "Edit the text"
                                           : "Type the full text");

  char out[IME_MAX_TEXT];
  memset(out, 0, sizeof(out));

  /* The user is about to spend an unbounded amount of time typing. Without
   * this the watchdog reports a stall and breaks into creport at 45 s, and the
   * "hang" is somebody choosing a name. */
  g_shown = 1;
  watchdog_disarm();
  debug_log("[ime] opening the Switch keyboard (watchdog disarmed)\n");

  rc = swkbdShow(&kbd, out, sizeof(out));

  watchdog_rearm();          /* rearm() beats internally, so the long keyboard
                                session does not count as a stall */
  g_shown = 0;
  swkbdClose(&kbd);

  if (R_FAILED(rc)) {
    /* Cancelling is a normal outcome, not an error. Nothing is delivered --
     * which matches dismissing the keyboard on Android. */
    debug_log("[ime] keyboard dismissed without confirming (0x%x); nothing "
              "delivered\n", (unsigned)rc);
    return;
  }

  out[sizeof(out) - 1] = '\0';
  debug_log("[ime] keyboard returned %zu bytes\n", strlen(out));

  if (!out[0]) {
    /* Confirmed with nothing typed. Deliberately NOT treated as "clear the
     * field": the keyboard opens blank the first time even when the field has
     * content, so an accidental confirm would wipe a name the user never meant
     * to touch. Doing nothing is recoverable; deleting is not. */
    debug_log("[ime] confirmed with an empty string -- leaving the field "
              "alone rather than clearing it\n");
#if IME_SEND_ENTER
    send_keycode(AKEYCODE_ENTER);
#endif
    return;
  }

  deliver(out);

  /* Remember it, so the next visit opens on this instead of blank. */
  memcpy(g_field, out, sizeof(g_field));
  g_field[sizeof(g_field) - 1] = '\0';
}

int ime_keyboard_pending(void) { return g_pending; }

void ime_request_keyboard(void) {
  if (g_pending) return;
  g_pending = 1;
  debug_log("[ime] keyboard queued\n");
}

void ime_init(void) {
  jni_register_class(&g_class_IMM);
  jni_register_class(&g_class_EditorInfo);
  g_imm_obj.cls        = &g_class_IMM;
  g_editorinfo_obj.cls = &g_class_EditorInfo;
  debug_log("[ime] InputMethodManager registered; showSoftInput raises the "
            "Switch keyboard\n");
}
