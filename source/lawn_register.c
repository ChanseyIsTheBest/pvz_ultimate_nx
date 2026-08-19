/* lawn_register.c -- register the game's native methods ourselves.
 *
 * Java.Interop binds a type's natives from the generated Java class's
 * <clinit>, which calls ManagedPeer.registerNativeMembers with a descriptor
 * naming every native method and the managed handler that services it. We do
 * not execute the dex, so no static initialiser ever runs, and nothing was
 * ever registered -- constructing peers got the managed side running but left
 * all nineteen natives unbound.
 *
 * The descriptors below were extracted verbatim from classes.dex rather than
 * reconstructed: the connector names follow a mangling scheme (GetOnTouchEvent
 * _Landroid_view_MotionEvent_Handler and so on) that is tedious to reproduce
 * and silently wrong if a single character is off. Some entries carry a fourth
 * field naming the interface invoker in Mono.Android, which matters for the
 * ones reached through an interface -- surfaceCreated and doFrame among them.
 *
 * If the game is rebuilt, re-extract these. They are tied to that dex.
 */

#include <string.h>

#include "interop_classes.h"
#include "jni_fake.h"
#include "jni_ref.h"
#include "lawn_register.h"
#include "util.h"

static const char ACTIVITY_NATIVES[] =
    "n_onCreate:(Landroid/os/Bundle;)V:GetOnCreate_Landroid_os_Bundle_Handler\n"
    "n_onResume:()V:GetOnResumeHandler\n"
    "n_onPause:()V:GetOnPauseHandler\n"
    "n_onDestroy:()V:GetOnDestroyHandler\n"
    "n_onBackPressed:()V:GetOnBackPressedHandler\n"
    "n_onActivityResult:(IILandroid/content/Intent;)V:GetOnActivityResult_IILandroid_content_Intent_Handler\n"
    "n_onConfigurationChanged:(Landroid/content/res/Configuration;)V:GetOnConfigurationChanged_Landroid_content_res_Configuration_Handler\n"
    /* 2.1.2 added the one-argument overload alongside the existing two-argument
     * one. Both are declared, so both must be registered -- a descriptor that
     * omits a declared native leaves it unbound, which is silent. */
    "n_onMultiWindowModeChanged:(Z)V:GetOnMultiWindowModeChanged_ZHandler\n"
    "n_onMultiWindowModeChanged:(ZLandroid/content/res/Configuration;)V:GetOnMultiWindowModeChanged_ZLandroid_content_res_Configuration_Handler\n"
    "n_onWindowFocusChanged:(Z)V:GetOnWindowFocusChanged_ZHandler\n"
    ;

static const char SURFACEVIEW_NATIVES[] =
    "n_onTouchEvent:(Landroid/view/MotionEvent;)Z:GetOnTouchEvent_Landroid_view_MotionEvent_Handler\n"
    "n_onGenericMotionEvent:(Landroid/view/MotionEvent;)Z:GetOnGenericMotionEvent_Landroid_view_MotionEvent_Handler\n"
    "n_onKeyDown:(ILandroid/view/KeyEvent;)Z:GetOnKeyDown_ILandroid_view_KeyEvent_Handler\n"
    "n_onKeyUp:(ILandroid/view/KeyEvent;)Z:GetOnKeyUp_ILandroid_view_KeyEvent_Handler\n"
    "n_onCheckIsTextEditor:()Z:GetOnCheckIsTextEditorHandler\n"
    "n_onCreateInputConnection:(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;:GetOnCreateInputConnection_Landroid_view_inputmethod_EditorInfo_Handler\n"
    "n_surfaceChanged:(Landroid/view/SurfaceHolder;III)V:GetSurfaceChanged_Landroid_view_SurfaceHolder_IIIHandler:Android.Views.ISurfaceHolderCallbackInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
    "n_surfaceCreated:(Landroid/view/SurfaceHolder;)V:GetSurfaceCreated_Landroid_view_SurfaceHolder_Handler:Android.Views.ISurfaceHolderCallbackInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
    "n_surfaceDestroyed:(Landroid/view/SurfaceHolder;)V:GetSurfaceDestroyed_Landroid_view_SurfaceHolder_Handler:Android.Views.ISurfaceHolderCallbackInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
    "n_doFrame:(J)V:GetDoFrame_JHandler:Android.Views.Choreographer+IFrameCallbackInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
    ;

/* mono/java/lang/RunnableImplementor -- the one that was blocking the boot.
 *
 * MonoGameAndroidGameView.SurfaceCreated does not create the GL context
 * inline. It builds the surface state, wraps the setup in an Action, and posts
 * it to the view's Handler; the Runnable that arrives is a
 * RunnableImplementor peer. Handler.post is implemented here and
 * android_os_run_posted drains it every frame, so the Runnable was dequeued
 * and run() was called -- straight into an unbound n_run, which forwards to
 * nothing and returns. The work was silently dropped, the two gates on the
 * view were never set, and DoFrame returned on its first instruction forever
 * after. No crash, no log line, no drawing.
 *
 * Byte-for-byte from classes.dex, trailing newline included: the dex string is
 * 120 characters and the newline is the 120th. */
static const char RUNNABLE_NATIVES[] =
    "n_run:()V:GetRunHandler:Java.Lang.IRunnableInvoker, Mono.Android, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null\n"
    ;

/* Hand one descriptor to ManagedPeer.registerNativeMembers. */
/* crc646ec5dadb3c3b2bda/AndroidNativeActivity_LawnSurfaceView_LawnInputConnection
 *
 * Extracted verbatim from classes.dex, like the others -- the whole 13-method
 * __md_methods string, read out of the raw bytes rather than reassembled from
 * `strings` output, which splits it on the \n separators and loses which class
 * the pieces belong to.
 *
 * This is why typed text went nowhere. The shim said so exactly:
 *
 *   [reg] ...LawnInputConnection.n_commitText is declared but NOT BOUND --
 *         the call did nothing and returned zero.
 *
 * Unlike the other three, this class does not exist at startup: the game
 * constructs it inside onCreateInputConnection, the first time a field asks for
 * input. So it cannot be registered from lawn_register_natives() with the rest
 * -- jni_find_class would not find it -- and is registered on first use
 * instead, from lawn_create_input_connection(). */
static const char INPUTCONNECTION_NATIVES[] =
    "n_getEditable:()Landroid/text/Editable;:GetGetEditableHandler\n"
    "n_beginBatchEdit:()Z:GetBeginBatchEditHandler\n"
    "n_endBatchEdit:()Z:GetEndBatchEditHandler\n"
    "n_commitText:(Ljava/lang/CharSequence;I)Z:GetCommitText_Ljava_lang_CharSequence_IHandler\n"
    "n_setComposingText:(Ljava/lang/CharSequence;I)Z:GetSetComposingText_Ljava_lang_CharSequence_IHandler\n"
    "n_finishComposingText:()Z:GetFinishComposingTextHandler\n"
    "n_setComposingRegion:(II)Z:GetSetComposingRegion_IIHandler\n"
    "n_setSelection:(II)Z:GetSetSelection_IIHandler\n"
    "n_deleteSurroundingText:(II)Z:GetDeleteSurroundingText_IIHandler\n"
    "n_deleteSurroundingTextInCodePoints:(II)Z:GetDeleteSurroundingTextInCodePoints_IIHandler\n"
    "n_commitCompletion:(Landroid/view/inputmethod/CompletionInfo;)Z:GetCommitCompletion_Landroid_view_inputmethod_CompletionInfo_Handler\n"
    "n_replaceText:(IILjava/lang/CharSequence;ILandroid/view/inputmethod/TextAttribute;)Z:GetReplaceText_IILjava_lang_CharSequence_ILandroid_view_inputmethod_TextAttribute_Handler\n"
    "n_sendKeyEvent:(Landroid/view/KeyEvent;)Z:GetSendKeyEvent_Landroid_view_KeyEvent_Handler\n"
    ;

/* The one method that matters, for the retry below. If any entry in the full
 * descriptor has no managed handler the whole registration throws and nothing
 * binds, which would be a worse outcome than binding only what is needed. */
static const char INPUTCONNECTION_MINIMAL[] =
    "n_commitText:(Ljava/lang/CharSequence;I)Z:GetCommitText_Ljava_lang_CharSequence_IHandler\n"
    ;

#define IC_CLASS \
  "crc646ec5dadb3c3b2bda/AndroidNativeActivity_LawnSurfaceView_LawnInputConnection"

static int register_one(JNIEnv *env, const char *cls_name, const char *members) {
  FakeClass *mp = jni_find_class("net/dot/jni/ManagedPeer");
  FakeClass *target = jni_find_class(cls_name);
  if (!mp || !target) {
    debug_log("[reg] %s: class not registered\n", cls_name);
    return -1;
  }

  jclass mpc = (jclass)jniref_new(&mp->hdr, REF_LOCAL);
  jmethodID mid = (*env)->GetStaticMethodID(
      env, mpc, "registerNativeMembers",
      "(Ljava/lang/Class;Ljava/lang/String;)V");
  if (!mid) { debug_log("[reg] registerNativeMembers not found\n"); return -1; }

  FakeMethod *m = (FakeMethod *)mid;
  if (!m->native_fn) {
    debug_log("[reg] registerNativeMembers has no implementation yet\n");
    return -1;
  }

  jclass  target_ref = (jclass)jniref_new(&target->hdr, REF_LOCAL);
  jstring desc       = (*env)->NewStringUTF(env, members);

  debug_log("[reg] registering natives for %s\n", cls_name);
  (*env)->CallStaticVoidMethod(env, mpc, mid, target_ref, desc);

  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return -1;
  }
  return 0;
}

void lawn_register_natives(JNIEnv *env) {
  register_one(env, "crc646ec5dadb3c3b2bda/AndroidNativeActivity",
               ACTIVITY_NATIVES);
  register_one(env, "crc646ec5dadb3c3b2bda/AndroidNativeActivity_LawnSurfaceView",
               SURFACEVIEW_NATIVES);

  /* Not one of the game's own classes, but it reaches managed code the same
   * way and through the same gap: its <clinit> is the only thing that would
   * have registered it, and we never run one. */
  register_one(env, "mono/java/lang/RunnableImplementor", RUNNABLE_NATIVES);

  /* The InputConnection class is statically registered in android_runtime.c, so
   * unlike the comment on the descriptor above it does exist this early. Try
   * now -- the game can reach the IME before the shim opens a keyboard -- and
   * if the runtime is not ready, the call from lawn_create_input_connection
   * will retry. */
  lawn_register_input_connection(env);
}

void lawn_register_input_connection(JNIEnv *env) {
  static int done;
  if (done) return;
  if (!jni_find_class(IC_CLASS)) return;

  /* `done` is set only on SUCCESS.
   *
   * It used to be set before the attempt, so a registration that failed --
   * because registerNativeMembers was not available yet, which is exactly the
   * situation at the early call site below -- was never retried, and the later
   * call from lawn_create_input_connection would return immediately having done
   * nothing. Marking the work complete before doing it is how a retry path
   * stops being one. */
  int rc = register_one(env, IC_CLASS, INPUTCONNECTION_NATIVES);
  if (rc != 0) {
    debug_log("[reg] the full InputConnection descriptor did not take; "
              "retrying with commitText alone\n");
    rc = register_one(env, IC_CLASS, INPUTCONNECTION_MINIMAL);
  }
  if (rc == 0) done = 1;
}
