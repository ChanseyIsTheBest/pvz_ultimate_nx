/* android_text.c -- SpannableStringBuilder, Editable, Selection.
 *
 * WHY
 *
 * With commitText finally bound, typing put this in the game's field:
 *
 *     android/text/Spannab
 *
 * which is a class name, truncated to the width of the box. The chain was in
 * the log:
 *
 *     [jni] auto android/text/SpannableStringBuilder (stubbed; not implemented)
 *     [jni] auto android/text/SpannableStringBuilder.<init>()V
 *     [jni] auto android/text/SpannableStringBuilder.append(Ljava/lang/CharSequence;)...
 *     [jni] auto android/text/Selection.setSelection(Landroid/text/Spannable;II)V
 *
 * `LawnInputConnection.getEditable()` returns a SpannableStringBuilder. The
 * managed BaseInputConnection appends the committed text to it and moves the
 * selection; the game then reads the buffer back. Every one of those was an
 * auto-manufactured stub, so the append went nowhere and toString() returned
 * the only thing a stub can return about itself -- its class name.
 *
 * So this file is a real mutable text buffer. It is also what makes the
 * keyboard able to open pre-filled: once the editable holds the field's actual
 * text, ime_shim can read it out (android_text_get) instead of opening blank.
 *
 * KNOWN LIMIT: lengths and offsets here are in BYTES, where Java counts UTF-16
 * code units. They agree for ASCII, which is what a profile name usually is,
 * and they do not for anything else -- so a name with non-ASCII characters may
 * place the caret wrongly. Clearing is unaffected, because over-estimating a
 * delete is clamped. Fixing it properly means storing UTF-16 alongside, which
 * is not worth doing until something actually needs it.
 *
 * One class serves SpannableStringBuilder, Editable and Spannable. The shim's
 * JNI does not type-check casts, and the three are the same object on Android
 * anyway -- Editable and Spannable are interfaces SpannableStringBuilder
 * implements.
 */

#include <string.h>

#include "android_text.h"
#include "jni_arrays.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "util.h"

#define JV_ZERO jvalue r; memset(&r, 0, sizeof(r))

#define EDIT_CAP 1024

typedef struct {
  FakeObject hdr;
  char       buf[EDIT_CAP];
  int        len;
  int        sel_start, sel_end;
} FakeEditable;

static FakeClass g_class_SSB;   /* defined below; needed by helpers first */

/* The text of a CharSequence argument, whichever of the two shapes it is.
 *
 * Type-CHECKED. A String and an editable have different layouts, and reading
 * one as the other hands a pointer-sized run of characters to the caller as if
 * it were a pointer -- the same trap that was caught in java_net.c. */
static const char *cs_text(jobject o) {
  FakeObject *f = jniref_deref(o);
  if (!f || !f->cls) return NULL;
  if (f->cls == &g_class_String)  return ((FakeString *)f)->utf;
  if (f->cls == &g_class_SSB)     return ((FakeEditable *)f)->buf;
  return NULL;
}

const char *android_text_get(jobject o) {
  FakeObject *f = jniref_deref(o);
  if (!f || f->cls != &g_class_SSB) return NULL;
  return ((FakeEditable *)f)->buf;
}

static FakeEditable *self_of(jobject self) {
  FakeObject *f = jniref_deref(self);
  return (f && f->cls == &g_class_SSB) ? (FakeEditable *)f : NULL;
}

static void clamp_range(const FakeEditable *e, int *a, int *b) {
  if (*a < 0) *a = 0;
  if (*b < 0) *b = 0;
  if (*a > e->len) *a = e->len;
  if (*b > e->len) *b = e->len;
  if (*a > *b) { int t = *a; *a = *b; *b = t; }
}

/* replace [start,end) with `ins`. Every mutation goes through here, so the
 * bounds and the terminator are handled in exactly one place. */
static void edit_replace(FakeEditable *e, int start, int end, const char *ins) {
  clamp_range(e, &start, &end);
  int inslen = ins ? (int)strlen(ins) : 0;
  int tail   = e->len - end;

  if (start + inslen + tail > EDIT_CAP - 1) {
    inslen = EDIT_CAP - 1 - start - tail;
    if (inslen < 0) inslen = 0;
  }

  memmove(e->buf + start + inslen, e->buf + end, (size_t)tail);
  if (inslen) memcpy(e->buf + start, ins, (size_t)inslen);
  e->len = start + inslen + tail;
  e->buf[e->len] = '\0';

  e->sel_start = e->sel_end = start + inslen;
}

/* Replace an editable's entire contents, cursor left at the end.
 *
 * The delivery path uses this instead of commitText. commitText was reaching
 * BaseInputConnection correctly and then finding no text to write -- the
 * CharSequence argument does not survive the managed round trip, and chasing
 * that through Java.Interop's marshalling is not something this shim can see
 * into. It does not need to: the editable IS the game's buffer, the game reads
 * it back with toString(), and writing to it directly is both simpler and
 * deterministic. Replace rather than insert, so the result is exactly what the
 * keyboard returned however many times it runs. */
int android_text_set(jobject o, const char *text) {
  FakeObject *f = jniref_deref(o);
  if (!f || f->cls != &g_class_SSB) return 0;
  FakeEditable *ed = (FakeEditable *)f;
  edit_replace(ed, 0, ed->len, text);
  return 1;
}

/* ------------------------------------------------------------- methods --- */

static jvalue ssb_init(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  if (!ed) return r;
  ed->len = 0;
  ed->buf[0] = '\0';
  ed->sel_start = ed->sel_end = 0;
  return r;
}

/* Separate from the no-argument form on purpose: sharing one handler means it
 * reads a[0] for a signature that supplies no arguments, which is only safe
 * because the caller happens to zero the array. One handler per arity does not
 * depend on that. */
static jvalue ssb_init_cs(JNIEnv *e, jobject self, const jvalue *a) {
  ssb_init(e, self, NULL);
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  const char *init = a ? cs_text(a[0].l) : NULL;
  if (ed && init) edit_replace(ed, 0, 0, init);
  return r;
}

/* Fluent: returns itself so a chain survives. */
static jvalue ssb_append(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  if (ed) edit_replace(ed, ed->len, ed->len, a ? cs_text(a[0].l) : NULL);
  r.l = self;
  return r;
}

static jvalue ssb_replace(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  if (ed && a) edit_replace(ed, a[0].i, a[1].i, cs_text(a[2].l));
  r.l = self;
  return r;
}

static jvalue ssb_insert(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  if (ed && a) edit_replace(ed, a[0].i, a[0].i, cs_text(a[1].l));
  r.l = self;
  return r;
}

static jvalue ssb_delete(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  if (ed && a) edit_replace(ed, a[0].i, a[1].i, NULL);
  r.l = self;
  return r;
}

static jvalue ssb_clear(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  if (ed) { ed->len = 0; ed->buf[0] = '\0'; ed->sel_start = ed->sel_end = 0; }
  return r;
}

static jvalue ssb_length(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  r.i = ed ? ed->len : 0;
  return r;
}

static jvalue ssb_charAt(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  int i = a ? a[0].i : 0;
  if (ed && i >= 0 && i < ed->len) r.c = (unsigned short)(unsigned char)ed->buf[i];
  return r;
}

static jvalue ssb_toString(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)a;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  r.l = jni_make_string(ed ? ed->buf : "");
  return r;
}

static jvalue ssb_subSequence(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e;
  JV_ZERO;
  FakeEditable *ed = self_of(self);
  if (!ed || !a) { r.l = jni_make_string(""); return r; }
  int s = a[0].i, t = a[1].i;
  clamp_range(ed, &s, &t);
  char tmp[EDIT_CAP];
  int n = t - s;
  if (n < 0) n = 0;
  if (n > EDIT_CAP - 1) n = EDIT_CAP - 1;
  memcpy(tmp, ed->buf + s, (size_t)n);
  tmp[n] = '\0';
  r.l = jni_make_string(tmp);
  return r;
}

/* Spans carry styling, which nothing here renders. Accepting and forgetting
 * them is correct; refusing would break the caller's chain. */
static jvalue ssb_void(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; return r;
}

static jvalue ssb_minus_one(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.i = -1; return r;
}

static FakeMethod g_ssb_methods[] = {
  { ssb_init,        "<init>",      "()V",                                                 NULL, 0 },
  { ssb_init_cs,     "<init>",      "(Ljava/lang/CharSequence;)V",                         NULL, 0 },
  { ssb_append,      "append",      "(Ljava/lang/CharSequence;)Landroid/text/SpannableStringBuilder;", NULL, 0 },
  { ssb_append,      "append",      "(Ljava/lang/CharSequence;)Landroid/text/Editable;",    NULL, 0 },
  { ssb_append,      "append",      "(Ljava/lang/CharSequence;)Ljava/lang/Appendable;",     NULL, 0 },
  { ssb_replace,     "replace",     "(IILjava/lang/CharSequence;)Landroid/text/SpannableStringBuilder;", NULL, 0 },
  { ssb_replace,     "replace",     "(IILjava/lang/CharSequence;)Landroid/text/Editable;",  NULL, 0 },
  { ssb_insert,      "insert",      "(ILjava/lang/CharSequence;)Landroid/text/SpannableStringBuilder;",  NULL, 0 },
  { ssb_insert,      "insert",      "(ILjava/lang/CharSequence;)Landroid/text/Editable;",   NULL, 0 },
  { ssb_delete,      "delete",      "(II)Landroid/text/SpannableStringBuilder;",            NULL, 0 },
  { ssb_delete,      "delete",      "(II)Landroid/text/Editable;",                          NULL, 0 },
  { ssb_clear,       "clear",       "()V",                                                  NULL, 0 },
  { ssb_length,      "length",      "()I",                                                  NULL, 0 },
  { ssb_charAt,      "charAt",      "(I)C",                                                 NULL, 0 },
  { ssb_toString,    "toString",    "()Ljava/lang/String;",                                 NULL, 0 },
  { ssb_subSequence, "subSequence", "(II)Ljava/lang/CharSequence;",                         NULL, 0 },
  { ssb_void,        "setSpan",     "(Ljava/lang/Object;III)V",                             NULL, 0 },
  { ssb_void,        "removeSpan",  "(Ljava/lang/Object;)V",                                NULL, 0 },
  { ssb_minus_one,   "getSpanStart","(Ljava/lang/Object;)I",                                NULL, 0 },
  { ssb_minus_one,   "getSpanEnd",  "(Ljava/lang/Object;)I",                                NULL, 0 },
};

static FakeClass g_class_SSB = {
  {NULL}, "android/text/SpannableStringBuilder", NULL,
  g_ssb_methods, (int)(sizeof(g_ssb_methods) / sizeof(g_ssb_methods[0])),
  NULL, 0, sizeof(FakeEditable)
};

/* ----------------------------------------------------------- Selection --- */

/* Static helpers that move the cursor within a Spannable. The IME calls these
 * after every commit, and a stub silently did nothing -- which on a real
 * editable would leave the cursor at 0 and make the next insert land at the
 * front of the string. */
static jvalue sel_setSelection(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV_ZERO;
  if (!a) return r;
  FakeObject *f = jniref_deref(a[0].l);
  if (!f || f->cls != &g_class_SSB) return r;
  FakeEditable *ed = (FakeEditable *)f;
  int s = a[1].i, t = a[2].i;
  clamp_range(ed, &s, &t);
  ed->sel_start = s;
  ed->sel_end   = t;
  return r;
}

/* setSelection(Spannable, int) -- the CARET form, collapsed at index.
 *
 * This shared the two-argument handler, which reads a[2] for the end. The
 * caller zeroes the argument array and fills only what the signature names, so
 * a[2] was 0: the caret placed at n became a SELECTION of [0, n]. The next
 * commitText would then replace the whole field instead of inserting, and the
 * IME calls this after every commit. */
static jvalue sel_setCursor(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV_ZERO;
  if (!a) return r;
  FakeObject *f = jniref_deref(a[0].l);
  if (!f || f->cls != &g_class_SSB) return r;
  FakeEditable *ed = (FakeEditable *)f;
  int s = a[1].i, t = s;
  clamp_range(ed, &s, &t);
  ed->sel_start = ed->sel_end = s;
  return r;
}

static jvalue sel_start(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV_ZERO;
  FakeObject *f = a ? jniref_deref(a[0].l) : NULL;
  r.i = (f && f->cls == &g_class_SSB) ? ((FakeEditable *)f)->sel_start : -1;
  return r;
}

static jvalue sel_end(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self;
  JV_ZERO;
  FakeObject *f = a ? jniref_deref(a[0].l) : NULL;
  r.i = (f && f->cls == &g_class_SSB) ? ((FakeEditable *)f)->sel_end : -1;
  return r;
}

static FakeMethod g_sel_methods[] = {
  { sel_setSelection, "setSelection",      "(Landroid/text/Spannable;II)V",     NULL, 1 },
  { sel_setCursor,    "setSelection",      "(Landroid/text/Spannable;I)V",      NULL, 1 },
  { sel_start,        "getSelectionStart", "(Ljava/lang/CharSequence;)I",       NULL, 1 },
  { sel_end,          "getSelectionEnd",   "(Ljava/lang/CharSequence;)I",       NULL, 1 },
};

static FakeClass g_class_Selection = {
  {NULL}, "android/text/Selection", NULL,
  g_sel_methods, (int)(sizeof(g_sel_methods) / sizeof(g_sel_methods[0])),
  NULL, 0, 0
};

/* ------------------------------------------- BaseInputConnection --------- */

/* THE class that actually edits the buffer, and it was a stub.
 *
 * The game's LawnInputConnection.CommitText overrides the framework method and
 * then calls base.CommitText(...) to do the work -- which is the normal shape
 * for an IME client. The log showed exactly what happened next:
 *
 *   [jni] auto android/view/inputmethod/BaseInputConnection (stubbed)
 *   [jni] auto ...BaseInputConnection.commitText(Ljava/lang/CharSequence;I)Z
 *   [jni] commitText(...)Z returned false -- it is a stub
 *
 * So every character went into a base class that does nothing, and the editable
 * the game reads back stayed empty. That is both reported symptoms at once: the
 * keyboard opens blank because the editable is empty, and typed text lands as
 * blank because it never reached the editable either.
 *
 * These implementations are what Android's BaseInputConnection does: take the
 * editable from getEditable(), replace the selected range with the committed
 * text, and move the cursor after it. */

/* The editable belonging to this connection.
 *
 * getEditable() is looked up on the object's OWN class, so the game's override
 * runs and we operate on the game's buffer rather than one of our own. */
static FakeEditable *editable_of(JNIEnv *e, jobject self) {
  jclass c = (*e)->GetObjectClass(e, self);
  if (!c) return NULL;
  jmethodID mid = (*e)->GetMethodID(e, c, "getEditable",
                                    "()Landroid/text/Editable;");
  if (!mid) return NULL;
  jobject ed = (*e)->CallObjectMethod(e, self, mid);
  (*e)->ExceptionClear(e);
  FakeObject *o = jniref_deref(ed);
  return (o && o->cls == &g_class_SSB) ? (FakeEditable *)o : NULL;
}

static jvalue bic_commitText(JNIEnv *e, jobject self, const jvalue *a) {
  JV_ZERO;
  FakeEditable *ed = editable_of(e, self);
  const char *txt = a ? cs_text(a[0].l) : NULL;
  if (!ed || !txt) {
    const char *what = "nothing to commit";
    if (a) {
      FakeObject *arg = jniref_deref(a[0].l);
      what = !arg ? "the CharSequence argument is null"
                  : (arg->cls && arg->cls->name ? arg->cls->name : "an unknown class");
    }
    debug_log("[text] BaseInputConnection.commitText: %s\n",
              !ed ? "no editable to write into" : what);
    return r;
  }
  edit_replace(ed, ed->sel_start, ed->sel_end, txt);   /* moves the cursor */
  r.z = JNI_TRUE;
  debug_log("[text] BaseInputConnection.commitText wrote %zu bytes; the "
            "editable now holds %d\n", strlen(txt), ed->len);
  return r;
}

static jvalue bic_deleteSurrounding(JNIEnv *e, jobject self, const jvalue *a) {
  JV_ZERO;
  FakeEditable *ed = editable_of(e, self);
  if (!ed || !a) return r;
  int before = a[0].i, after = a[1].i;
  int s = ed->sel_start - (before > 0 ? before : 0);
  int t = ed->sel_end   + (after  > 0 ? after  : 0);
  edit_replace(ed, s, t, NULL);
  r.z = JNI_TRUE;
  debug_log("[text] BaseInputConnection.deleteSurroundingText(%d,%d); the "
            "editable now holds %d\n", before, after, ed->len);
  return r;
}

static jvalue bic_setSelection(JNIEnv *e, jobject self, const jvalue *a) {
  JV_ZERO;
  FakeEditable *ed = editable_of(e, self);
  if (!ed || !a) return r;
  int s = a[0].i, t = a[1].i;
  clamp_range(ed, &s, &t);
  ed->sel_start = s;
  ed->sel_end   = t;
  r.z = JNI_TRUE;
  return r;
}

static jvalue bic_text_around(JNIEnv *e, jobject self, const jvalue *a,
                              int before) {
  JV_ZERO;
  FakeEditable *ed = editable_of(e, self);
  int n = a ? a[0].i : 0;
  if (!ed || n <= 0) { r.l = jni_make_string(""); return r; }

  int s, t;
  if (before) { s = ed->sel_start - n; t = ed->sel_start; }
  else        { s = ed->sel_end;       t = ed->sel_end + n; }
  clamp_range(ed, &s, &t);

  char tmp[EDIT_CAP];
  int len = t - s;
  if (len < 0) len = 0;
  if (len > EDIT_CAP - 1) len = EDIT_CAP - 1;
  memcpy(tmp, ed->buf + s, (size_t)len);
  tmp[len] = '\0';
  r.l = jni_make_string(tmp);
  return r;
}

static jvalue bic_before(JNIEnv *e, jobject self, const jvalue *a) {
  return bic_text_around(e, self, a, 1);
}
static jvalue bic_after(JNIEnv *e, jobject self, const jvalue *a) {
  return bic_text_around(e, self, a, 0);
}

static jvalue bic_true(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.z = JNI_TRUE; return r;
}
static jvalue bic_void(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; return r;
}
/* No composing region is tracked, and -1 is what Android returns when there is
 * none. Returning 0 -- which the stub did -- claims a composing span at the
 * start of the buffer, and an editor acting on that would replace the wrong
 * range. */
static jvalue bic_no_span(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; r.i = -1; return r;
}
static jvalue bic_null(JNIEnv *e, jobject self, const jvalue *a) {
  (void)e; (void)self; (void)a;
  JV_ZERO; return r;
}

static FakeMethod g_bic_methods[] = {
  { bic_void,             "<init>",                "(Landroid/view/View;Z)V",                NULL, 0 },
  { bic_commitText,       "commitText",            "(Ljava/lang/CharSequence;I)Z",           NULL, 0 },
  { bic_commitText,       "setComposingText",      "(Ljava/lang/CharSequence;I)Z",           NULL, 0 },
  { bic_deleteSurrounding,"deleteSurroundingText", "(II)Z",                                  NULL, 0 },
  { bic_deleteSurrounding,"deleteSurroundingTextInCodePoints", "(II)Z",                      NULL, 0 },
  { bic_setSelection,     "setSelection",          "(II)Z",                                  NULL, 0 },
  { bic_setSelection,     "setComposingRegion",    "(II)Z",                                  NULL, 0 },
  { bic_before,           "getTextBeforeCursor",   "(II)Ljava/lang/CharSequence;",           NULL, 0 },
  { bic_after,            "getTextAfterCursor",    "(II)Ljava/lang/CharSequence;",           NULL, 0 },
  { bic_true,             "finishComposingText",   "()Z",                                    NULL, 0 },
  { bic_true,             "beginBatchEdit",        "()Z",                                    NULL, 0 },
  { bic_true,             "endBatchEdit",          "()Z",                                    NULL, 0 },
  { bic_true,             "performEditorAction",   "(I)Z",                                   NULL, 0 },
  { bic_no_span,          "getComposingSpanStart", "(Landroid/text/Spannable;)I",            NULL, 1 },
  { bic_no_span,          "getComposingSpanEnd",   "(Landroid/text/Spannable;)I",            NULL, 1 },
  /* The subclass overrides this; the base has no buffer of its own. */
  { bic_null,             "getEditable",           "()Landroid/text/Editable;",              NULL, 0 },
};

static FakeClass g_class_BaseInputConnection = {
  {NULL}, "android/view/inputmethod/BaseInputConnection", NULL,
  g_bic_methods, (int)(sizeof(g_bic_methods) / sizeof(g_bic_methods[0])),
  NULL, 0, 0
};

void android_text_init(void) {
  jni_register_class(&g_class_BaseInputConnection);
  jni_register_class(&g_class_SSB);
  jni_register_class(&g_class_Selection);
  debug_log("[text] SpannableStringBuilder, Selection and BaseInputConnection "
            "are implemented; commitText now reaches the editable\n");
}
