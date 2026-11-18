/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* The platform frames below an application's own on a thread's stack.
 *
 * On a device the app is never the bottom of a stack.  Its onCreate() is called by
 * Activity.performCreate(), which Instrumentation, ActivityThread, the transaction
 * executor and the main Looper call in turn, down to ZygoteInit.main; a Thread's run() is
 * called by java.lang.Thread.run.  Throwable.getStackTrace() and Thread.getStackTrace()
 * report all of it, and code that inspects its own call chain (anti-tamper does, class by
 * class) reads those frames.  The interpreter's stack holds only bytecode methods, so the
 * frames that the host drives on the platform's behalf are listed here, per entry point, as
 * Android 12 shows them (innermost first).  File names are the platform's; line numbers
 * differ between builds and are not claimed.
 *
 * A host thread that enters Java names its entry with dvm_platform_stack() and clears it
 * afterwards; with none named, the main thread is a Looper callback and any other bytecode
 * thread is a plain Thread.
 */

struct platform_frame {
  const char *cls, *method, *file;
  bool native;
};
#define PF(c, m, f) { c, m, f, false }
#define PF_NATIVE(c, m, f) { c, m, f, true }

/* What the main Looper's callers look like from Handler.dispatchMessage down. */
static const struct platform_frame pf_looper_tail[] = {
  PF("android/os/Handler", "dispatchMessage", "Handler.java"),
  PF("android/os/Looper", "loopOnce", "Looper.java"),
  PF("android/os/Looper", "loop", "Looper.java"),
  PF("android/app/ActivityThread", "main", "ActivityThread.java"),
  PF_NATIVE("java/lang/reflect/Method", "invoke", "Method.java"),
  PF("com/android/internal/os/RuntimeInit$MethodAndArgsCaller", "run", "RuntimeInit.java"),
  PF("com/android/internal/os/ZygoteInit", "main", "ZygoteInit.java"),
};

#define PF_H PF("android/app/ActivityThread$H", "handleMessage", "ActivityThread.java")
#define PF_BIND \
  PF("android/app/ActivityThread", "handleBindApplication", "ActivityThread.java"), \
  PF("android/app/ActivityThread", "access$1300", "ActivityThread.java"), \
  PF_H
#define PF_EXEC \
  PF("android/app/servertransaction/TransactionExecutor", "execute", "TransactionExecutor.java")
#define PF_CYCLE \
  PF("android/app/servertransaction/TransactionExecutor", "performLifecycleSequence", "TransactionExecutor.java"), \
  PF("android/app/servertransaction/TransactionExecutor", "cycleToPath", "TransactionExecutor.java"), \
  PF("android/app/servertransaction/TransactionExecutor", "cycleToPath", "TransactionExecutor.java"), \
  PF("android/app/servertransaction/TransactionExecutor", "executeLifecycleState", "TransactionExecutor.java"), \
  PF_EXEC, PF_H
#define PF_RESUME \
  PF("android/app/ActivityThread", "performResumeActivity", "ActivityThread.java"), \
  PF("android/app/ActivityThread", "handleResumeActivity", "ActivityThread.java"), \
  PF("android/app/servertransaction/ResumeActivityItem", "execute", "ResumeActivityItem.java"), \
  PF("android/app/servertransaction/TransactionExecutor", "executeLifecycleState", "TransactionExecutor.java"), \
  PF_EXEC, PF_H

static const struct platform_frame pf_application_attach[] = {
  PF("android/app/Application", "attach", "Application.java"),
  PF("android/app/Instrumentation", "newApplication", "Instrumentation.java"),
  PF("android/app/Instrumentation", "newApplication", "Instrumentation.java"),
  PF("android/app/LoadedApk", "makeApplication", "LoadedApk.java"),
  PF_BIND,
};
static const struct platform_frame pf_application_create[] = {
  PF("android/app/Instrumentation", "callApplicationOnCreate", "Instrumentation.java"),
  PF_BIND,
};
static const struct platform_frame pf_provider_attach[] = {
  PF("android/content/ContentProvider", "attachInfo", "ContentProvider.java"),
  PF("android/content/ContentProvider", "attachInfo", "ContentProvider.java"),
  PF("android/app/ActivityThread", "installProvider", "ActivityThread.java"),
  PF("android/app/ActivityThread", "installContentProviders", "ActivityThread.java"),
  PF_BIND,
};
static const struct platform_frame pf_activity_create[] = {
  PF("android/app/Activity", "performCreate", "Activity.java"),
  PF("android/app/Activity", "performCreate", "Activity.java"),
  PF("android/app/Instrumentation", "callActivityOnCreate", "Instrumentation.java"),
  PF("android/app/ActivityThread", "performLaunchActivity", "ActivityThread.java"),
  PF("android/app/ActivityThread", "handleLaunchActivity", "ActivityThread.java"),
  PF("android/app/servertransaction/LaunchActivityItem", "execute", "LaunchActivityItem.java"),
  PF("android/app/servertransaction/TransactionExecutor", "executeCallbacks", "TransactionExecutor.java"),
  PF_EXEC, PF_H,
};
static const struct platform_frame pf_activity_start[] = {
  PF("android/app/Instrumentation", "callActivityOnStart", "Instrumentation.java"),
  PF("android/app/Activity", "performStart", "Activity.java"),
  PF("android/app/ActivityThread", "handleStartActivity", "ActivityThread.java"),
  PF_CYCLE,
};
static const struct platform_frame pf_activity_post_create[] = {
  PF("android/app/Instrumentation", "callActivityOnPostCreate", "Instrumentation.java"),
  PF("android/app/ActivityThread", "handleStartActivity", "ActivityThread.java"),
  PF_CYCLE,
};
static const struct platform_frame pf_activity_resume[] = {
  PF("android/app/Instrumentation", "callActivityOnResume", "Instrumentation.java"),
  PF("android/app/Activity", "performResume", "Activity.java"),
  PF_RESUME,
};
static const struct platform_frame pf_activity_post_resume[] = {
  PF("android/app/Activity", "performResume", "Activity.java"),
  PF_RESUME,
};
static const struct platform_frame pf_looper_callback[] = {
  PF("android/os/Handler", "handleCallback", "Handler.java"),
};
static const struct platform_frame pf_thread[] = {
  PF("java/lang/Thread", "run", "Thread.java"),
};

struct platform_stack {
  const char *name;
  const struct platform_frame *head;
  unsigned nhead;
  bool looper;   /* the head is followed by the main Looper's callers */
};
#define PS(n, a, l) { n, a, sizeof a / sizeof a[0], l }
static const struct platform_stack platform_stacks[] = {
  PS("application.attach", pf_application_attach, true),
  PS("application.onCreate", pf_application_create, true),
  PS("provider.attach", pf_provider_attach, true),
  PS("activity.onCreate", pf_activity_create, true),
  PS("activity.onStart", pf_activity_start, true),
  PS("activity.onPostCreate", pf_activity_post_create, true),
  PS("activity.onResume", pf_activity_resume, true),
  PS("activity.onPostResume", pf_activity_post_resume, true),
  PS("looper.callback", pf_looper_callback, true),
  PS("thread", pf_thread, false),
};

static _Thread_local const struct platform_stack *g_platform_stack;

void dvm_platform_stack(const char *entry)
{
  g_platform_stack = NULL;
  for (size_t i = 0; entry && i < sizeof platform_stacks / sizeof platform_stacks[0]; ++i)
    if (!strcmp(platform_stacks[i].name, entry)) {
      g_platform_stack = &platform_stacks[i];
      return;
    }
}

/* The frames below the interpreter's, innermost first.  `index` runs over head then tail. */
static const struct platform_stack *platform_stack_current(void)
{
  if (g_platform_stack) return g_platform_stack;
  return &platform_stacks[dvm_on_main_thread() ? 8 : 9];
}
static unsigned platform_frame_count(const struct platform_stack *s)
{
  return s->nhead + (s->looper ? (unsigned)(sizeof pf_looper_tail / sizeof pf_looper_tail[0]) : 0u);
}
static const struct platform_frame *platform_frame_at(const struct platform_stack *s, unsigned index)
{
  return index < s->nhead ? &s->head[index] : &pf_looper_tail[index - s->nhead];
}
