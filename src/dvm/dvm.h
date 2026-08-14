/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Dalvik bytecode emulator.
 *
 * Lunaria runs an APK's *native* code under a JIT and answers its JNI calls
 * with hand-written host stubs.  That works while the guest only asks the
 * framework for things (a window, a display density, a file path), but an APK
 * also ships its own Java: `com.epicgames.ue4.GameActivity`,
 * `com.unity3d.player.UnityPlayer`, the game's own activity subclass.  When
 * native code calls into those, a stub can only return zero, and zero is a
 * lie the engine acts on.
 *
 * This module executes that Java for real, straight from the APK's
 * classes*.dex.  It is a register machine matching the Dalvik model: 32-bit
 * registers, wide values in adjacent pairs, one frame per call.
 *
 * Classes the APK does not define (android.*, most of java.*) stay with the
 * existing host stubs — the interpreter calls out through `struct dvm_hooks`.
 * So the two halves meet in the middle: bytecode for the app's own code, host
 * stubs for the framework beneath it.
 *
 * Instruction semantics follow the Dalvik bytecode reference:
 * https://source.android.com/docs/core/runtime/dalvik-bytecode
 * The shape of a standalone dex interpreter that answers a host's JNI calls
 * follows the same idea as fatalSec/DaliVM.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dvm/dex.h"

struct dvm;
struct dvm_class;
struct dvm_method;
struct dvm_object;

/* Object handle.  0 is null.  Handles are indices into the VM heap table, so
 * they survive table growth and can be handed to the JNI layer as an
 * integer. */
typedef uint32_t dvm_ref;

union dvm_value {
   int32_t  i;
   uint32_t u;
   int64_t  j;
   uint64_t ju;
   float    f;
   double   d;
   dvm_ref  l;
};

/* --- host hooks ---------------------------------------------------------
 *
 * Everything the interpreter cannot do by itself.  dvm_jni.c fills these in
 * against Lunaria's JNI layer; the unit tests fill in a smaller set.
 */
struct dvm_hooks {
   void *user;

   /* A method on a class with no dex definition (android.*, java.* that the
    * VM does not implement itself).  Return false to make the call throw
    * NoSuchMethodError; return true and leave `out` zeroed for void. */
   bool (*call_external)(void *user, struct dvm *vm, const char *class_name,
                         const char *method, const char *sig, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out);

   /* `new-instance` of a class with no dex definition.  Returns 0 to let the
    * VM allocate a plain opaque object instead. */
   dvm_ref (*new_external)(void *user, struct dvm *vm, const char *class_name);

   /* Read/write a static field of an external class.  Return false when
    * unknown (the VM then treats it as zero / ignores the store). */
   bool (*get_external_static)(void *user, struct dvm *vm, const char *class_name,
                               const char *field, const char *type,
                               union dvm_value *out);

   /* A method declared `native` in the dex: resolve it to the guest's
    * implementation and call it.  Returns false when the guest has not
    * registered or exported it. */
   bool (*call_native)(void *user, struct dvm *vm, const char *class_name,
                       const char *method, const char *sig, bool is_static,
                       dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out);

   /* java.lang.System.loadLibrary().  The bytecode VM owns System, while the
    * guest ELF loader owns the APK ABI directory and JNI symbol namespace. */
   bool (*load_library)(void *user, struct dvm *vm, const char *name);
};

/* --- lifecycle ---------------------------------------------------------- */

struct dvm *dvm_create(const struct dvm_hooks *hooks);
void dvm_destroy(struct dvm *vm);

/* Adds one classes*.dex.  Later files do not override classes already
 * defined, which matches the multidex lookup order. */
bool dvm_add_dex(struct dvm *vm, const char *path);
/* Adds classes.dex, classes2.dex, … from an unpacked APK directory (and from
 * its base/ subdirectory, which is where an App Bundle split keeps them).
 * Returns how many were loaded. */
int dvm_add_apk_dir(struct dvm *vm, const char *dir);

/* --- classes and methods ------------------------------------------------ */

/* Accepts either form: "com/foo/Bar" or "Lcom/foo/Bar;".  Returns NULL when
 * no dex defines it (use dvm_class_is_known() to tell "not defined" from
 * "failed to load"). */
struct dvm_class *dvm_find_class(struct dvm *vm, const char *name);
bool dvm_class_is_known(struct dvm *vm, const char *name);

/* Whether a class by this name would exist on the device at all: defined by
 * one of the APK's dexes, or part of the Android framework.  Anything else is
 * absent, and has to be reported as absent — see the note on the definition.
 * Accepts "com.foo.Bar", "com/foo/Bar" or "Lcom/foo/Bar;". */
bool dvm_class_exists(struct dvm *vm, const char *name);

/* `sig` may be NULL to take the first method with a matching name.  Searches
 * superclasses and interfaces. */
struct dvm_method *dvm_find_method(struct dvm *vm, struct dvm_class *cls,
                                   const char *name, const char *sig);
/* Convenience: class name + method name + signature in one go. */
struct dvm_method *dvm_lookup(struct dvm *vm, const char *class_name,
                              const char *method, const char *sig);

const char *dvm_method_name(const struct dvm_method *m);
const char *dvm_method_sig(const struct dvm_method *m);
const char *dvm_class_name(const struct dvm_class *c);
/* Declared superclass, slash-separated, or NULL for java/lang/Object. */
const char *dvm_class_super_name(const struct dvm_class *c);
bool dvm_method_is_static(const struct dvm_method *m);

/* --- invocation --------------------------------------------------------- */

/* `args` holds one entry per *declared parameter* (not per register slot);
 * wide parameters use one entry.  `self` is ignored for static methods.
 * Returns false when the call ended with an uncaught exception; the exception
 * is then readable with dvm_exception(). */
bool dvm_call(struct dvm *vm, struct dvm_method *m, dvm_ref self,
              const union dvm_value *args, int nargs, union dvm_value *out);

/* Runs <clinit> if it has not run yet. */
bool dvm_init_class(struct dvm *vm, struct dvm_class *cls);

dvm_ref dvm_exception(struct dvm *vm);
void dvm_clear_exception(struct dvm *vm);
/* "java.lang.NullPointerException: message" into a caller buffer. */
void dvm_describe_exception(struct dvm *vm, dvm_ref exc, char *buf, size_t sz);

/* --- heap --------------------------------------------------------------- */

dvm_ref dvm_new_object(struct dvm *vm, struct dvm_class *cls);
dvm_ref dvm_new_string(struct dvm *vm, const char *utf8);
dvm_ref dvm_new_string_n(struct dvm *vm, const char *utf8, size_t len);
/* Element kind is one of Z B C S I J F D L. */
dvm_ref dvm_new_array(struct dvm *vm, char elem, const char *elem_desc, uint32_t length);

struct dvm_class *dvm_object_class(struct dvm *vm, dvm_ref ref);
const char *dvm_string_utf8(struct dvm *vm, dvm_ref ref);
uint32_t dvm_array_length(struct dvm *vm, dvm_ref ref);
void *dvm_array_data(struct dvm *vm, dvm_ref ref);

/* Marks a handle as reachable from outside the VM so it is never recycled. */
void dvm_pin(struct dvm *vm, dvm_ref ref);
void dvm_unpin(struct dvm *vm, dvm_ref ref);

/* An object that only exists to carry a host-side handle (a jobject from the
 * stub layer) through bytecode.  `class_name` is in JNI form. */
dvm_ref dvm_wrap_external(struct dvm *vm, const char *class_name, uint32_t host_handle);
/* Non-zero when `ref` wraps a host handle. */
uint32_t dvm_external_handle(struct dvm *vm, dvm_ref ref);

/* --- fields (for the JNI bridge) ---------------------------------------- */

bool dvm_get_field(struct dvm *vm, dvm_ref obj, const char *name,
                   const char *type, union dvm_value *out);
bool dvm_set_field(struct dvm *vm, dvm_ref obj, const char *name,
                   const char *type, union dvm_value val);
bool dvm_get_static(struct dvm *vm, struct dvm_class *cls, const char *name,
                    const char *type, union dvm_value *out);
bool dvm_set_static(struct dvm *vm, struct dvm_class *cls, const char *name,
                    const char *type, union dvm_value val);

/* --- diagnostics -------------------------------------------------------- */

/* 0 = quiet, 1 = calls in/out, 2 = every instruction. */
void dvm_set_trace(struct dvm *vm, int level);
/* Instructions executed since creation. */
uint64_t dvm_instructions(const struct dvm *vm);
/* Caps runaway bytecode (a spin loop in Java would otherwise hang the pump
 * loop, which is cooperative).  0 disables.  Default 200M. */
void dvm_set_step_limit(struct dvm *vm, uint64_t limit);

/* Number of parameter slots (Dalvik registers) a signature occupies, not
 * counting `this`.  -1 on a malformed signature. */
int dvm_sig_arg_slots(const char *sig);
/* Number of declared parameters. */
int dvm_sig_arg_count(const char *sig);
/* Return-type descriptor character of a signature ('V' when void). */
char dvm_sig_return_kind(const char *sig);
