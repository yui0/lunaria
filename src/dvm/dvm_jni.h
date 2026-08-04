/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Where the bytecode emulator meets Lunaria's JNI layer.
 *
 * Two directions cross here:
 *
 *   guest native code --JNI--> jvm.c --> [this] --> bytecode
 *   bytecode --> [this] --> jni_stubs.c  (android.*, java.* we do not run)
 *   bytecode --> [this] --> guest native code   (methods declared `native`)
 *
 * The third one needs the ARM JIT, which lives in the lunaria binary rather
 * than libjvm.so, so arm_exec registers a caller through
 * dvm_jni_set_guest_native_caller().
 */

#pragma once

#include "jvm/jni.h"

#include <stdarg.h>
#include <stdbool.h>

/* LUNARIA_DVM: 0 disables the emulator, 1 (default) runs dex bytecode only
 * where no host stub exists, 2 prefers dex bytecode over host stubs. */
enum dvm_jni_mode { DVM_JNI_OFF = 0, DVM_JNI_FILL_GAPS = 1, DVM_JNI_PREFER = 2 };

enum dvm_jni_mode dvm_jni_mode(void);

/* Runs the method.  Exactly one of `ap` and `jargs` may be non-NULL; both NULL
 * means a no-argument call.  Returns false when the method is not in any dex
 * (the caller then falls back to its stub). */
bool dvm_jni_invoke(JNIEnv *env, const char *class_name, const char *method,
                    const char *sig, jobject self, bool is_static,
                    va_list *ap, const jvalue *jargs, jvalue *out);

/* Called by arm_exec once the guest's libraries are loaded, so bytecode can
 * reach methods the guest declared `native`. */
typedef bool (*dvm_guest_native_fn)(const char *class_name, const char *method,
                                    const char *sig, bool is_static, jobject self,
                                    const jvalue *args, int nargs, jvalue *out);
void dvm_jni_set_guest_native_caller(dvm_guest_native_fn fn);

/* Diagnostics for the loader's summary line. */
void dvm_jni_report(void);
