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

typedef bool (*dvm_guest_library_fn)(const char *name);
void dvm_jni_set_guest_library_loader(dvm_guest_library_fn fn);

/* True when the APK's dex declares this class (slash-separated name).  A guest
 * uses ClassLoader.loadClass() to feature-detect optional Java components; the
 * emulator must answer "absent" for a class the APK does not ship, or the guest
 * commits to a code path whose Java half can never run. */
bool dvm_jni_class_in_dex(const char *class_name);

/* Instance field of an object whose class the APK's dex defines, reached from
 * native through Get/SetXxxField.  The bytecode VM owns that object's fields,
 * so a write from native has to land there and not in the stub layer's own
 * side table — otherwise the two halves disagree about the same field.  `bits`
 * is the raw value, the way the JNI accessor macros carry it.  False when the
 * field is not one the VM owns; the caller then keeps its own handling. */
bool dvm_jni_field(JNIEnv *env, jobject obj, jfieldID field, bool set,
                   uint64_t *bits);

/* Superclass of a dex-defined class, slash-separated, or NULL when the APK
 * does not define the class (the framework hierarchy is not in the dex).
 * The stub layer needs it to resolve an inherited framework method called on
 * an app's own subclass. */
const char *dvm_jni_super_name(const char *class_name);

/* Diagnostics for the loader's summary line. */
void dvm_jni_report(void);
