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
#include "jvm/jni.h"
#include <stdarg.h>


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
    * registered or exported it. Called with the interpreter lock held for
    * VM argument/result conversion; release it around actual guest execution. */
   bool (*call_native)(void *user, struct dvm *vm, const char *class_name,
                       const char *method, const char *sig, bool is_static,
                       dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out);

   /* java.lang.System.loadLibrary().  The bytecode VM owns System, while the
    * guest ELF loader owns the APK ABI directory and JNI symbol namespace. */
   bool (*load_library)(void *user, struct dvm *vm, const char *name);

   /* A native method that has just returned.  JNI says a native which returns
    * with an exception pending raises it in its caller; the host side keeps
    * that state, so ask it.  Returns the Throwable (and clears the host's
    * pending state), or 0 when there is none. */
   dvm_ref (*take_pending_exception)(void *user, struct dvm *vm);

   /* android.app.NativeActivity lifecycle (DVM_NATIVE_ACTIVITY_*).  The
    * framework half of a NativeActivity — loading the app's native library,
    * calling ANativeActivity_onCreate and delivering the activity's native
    * callbacks — lives with the guest, not in bytecode.  Returns false when
    * the process has no native activity entry point (a pure-Java app). */
   bool (*native_activity)(void *user, struct dvm *vm, int stage,
                           dvm_ref activity);
};

enum {
   DVM_NATIVE_ACTIVITY_CREATE = 0,   /* NativeActivity.onCreate  */
   DVM_NATIVE_ACTIVITY_START  = 1,   /* NativeActivity.onStart   */
   DVM_NATIVE_ACTIVITY_RESUME = 2,   /* NativeActivity.onResume  */
};

/* Device/system-UI state, independent of app-owned Java objects. A new
 * virtual device has no enrolled credential or displayed lock screen. The
 * device owner publishes changes; this API never verifies credentials. */
enum {
   DVM_KEYGUARD_SHOWING=1u,
   DVM_KEYGUARD_DEVICE_SECURE=2u,
   DVM_KEYGUARD_SIM_LOCKED=4u,
   DVM_KEYGUARD_DEVICE_LOCKED=8u,
};
void dvm_set_keyguard_state(struct dvm *vm,unsigned state);

/* --- lifecycle ---------------------------------------------------------- */

struct dvm *dvm_create(const struct dvm_hooks *hooks);
void dvm_destroy(struct dvm *vm);
void dvm_glsurface_tick(struct dvm *vm);
void dvm_glsurface_finish(struct dvm *vm);
struct lm_sink;
/* Native producers and Java MediaCodec must share one BufferQueue. */
dvm_ref dvm_surface_from_sink(struct dvm *vm, struct lm_sink *sink);
/* Returns an owned sink reference, or NULL for a released/invalid Surface. */
struct lm_sink *dvm_surface_acquire_sink(struct dvm *vm, dvm_ref surface);
/* Bind ContextImpl/LoadedApk before app construction; publish Application
 * after attachBaseContext and before providers/onCreate. */
bool dvm_bind_application(struct dvm *vm, dvm_ref base, dvm_ref application);
/* Process teardown: stop admission and wake platform waits, then join Java
 * host threads before releasing the JNI table or execution engines. */
bool dvm_is_stopping(struct dvm *vm);
void dvm_request_stop(struct dvm *vm);
/* Explicit Java exit/halt, distinct from forced host teardown. */
bool dvm_process_exited(struct dvm *vm, int *status);
void dvm_exit_process(struct dvm *vm, int status);
void dvm_threads_finish(struct dvm *vm);

/* Adds one classes*.dex.  Later files do not override classes already
 * defined, which matches the multidex lookup order. */
bool dvm_add_dex(struct dvm *vm, const char *path);
/* Adds a dex the app handed to a ClassLoader at runtime rather than one that
 * came out of the APK: dalvik.system.InMemoryDexClassLoader over a direct
 * ByteBuffer, or DexClassLoader/PathClassLoader over a file it just wrote.
 * A native library that ships its own Java (libswappy.so carries the
 * com.google.androidgamesdk classes as a dex inside the .so) reaches its
 * bytecode only this way.  The bytes are copied; `name` is for diagnostics. */
bool dvm_add_dex_memory(struct dvm *vm, const void *data, size_t len,
                        const char *name);
/* Adds classes.dex, classes2.dex, … from an unpacked APK directory (and from
 * its base/ subdirectory, which is where an App Bundle split keeps them).
 * Returns how many were loaded. */
int dvm_add_apk_dir(struct dvm *vm, const char *dir);
int dvm_add_apk_archive(struct dvm *vm, const char *host_path, const char *guest_path);
dvm_ref dvm_get_class_loader(struct dvm *vm, dvm_ref class_object);

/* --- classes and methods ------------------------------------------------ */

/* Accepts either form: "com/foo/Bar" or "Lcom/foo/Bar;".  Returns NULL when
 * no dex defines it (use dvm_class_is_known() to tell "not defined" from
 * "failed to load"). */
struct dvm_class *dvm_find_class(struct dvm *vm, const char *name);
/* Same lookup, but also answers for a class only the host stub layer
 * implements — the VM's placeholder for it, whose methods route back out.
 * A jclass arriving from native names a type whether or not this VM defines
 * it, and the Class object handed to bytecode has to name that type. */
struct dvm_class *dvm_find_class_or_external(struct dvm *vm, const char *name);
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
/* Like dvm_find_method(), but the return type is not part of the match: only
 * the name and the parameter list are.  For callers that name a framework
 * method whose return type is a class an obfuscated app has renamed. */
struct dvm_method *dvm_find_method_by_params(struct dvm *vm, struct dvm_class *cls,
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
/* The java.lang.Class instance that describes `cls` (made once, pinned). */
dvm_ref dvm_class_object(struct dvm *vm, struct dvm_class *cls);
dvm_ref dvm_new_string(struct dvm *vm, const char *utf8);
dvm_ref dvm_new_string_n(struct dvm *vm, const char *utf8, size_t len);
/* Framework MotionEvent snapshots and Activity/View input delivery. */
struct luna_touch_event;
dvm_ref dvm_motion_event_new(struct dvm *vm, const struct luna_touch_event *sample);
bool dvm_motion_event_read(struct dvm *vm, dvm_ref event, struct luna_touch_event *out);
bool dvm_ui_dispatch_touch(struct dvm *vm, const struct luna_touch_event *sample);

/* Element kind is one of Z B C S I J F D L. */
dvm_ref dvm_new_array(struct dvm *vm, char elem, const char *elem_desc, uint32_t length);

struct dvm_class *dvm_object_class(struct dvm *vm, dvm_ref ref);
const char *dvm_string_utf8(struct dvm *vm, dvm_ref ref);
uint32_t dvm_string_utf8_length(struct dvm *vm, dvm_ref ref);
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

/* Write out any preferences an apply() left pending.  The frame pump calls
 * this, which is where a device's asynchronous write would land. */
void dvm_prefs_flush(struct dvm *vm);
void dvm_prefs_finish(struct dvm *vm);
/* One frame of the emulator's input method: what the host typed goes to the
 * connected View, and a pending layout pass runs.  Called from the frame pump
 * without the interpreter lock. */
void dvm_ime_frame(struct dvm *vm);

/* WebView controls for the emulator's menu; any thread.  Applied on the main
 * looper's next turn.  zoom 0 = the device density; engine is one of auto,
 * chrome, luna, off. */
float dvm_webview_zoom(void);
void dvm_webview_set_zoom(float zoom);
void dvm_webview_reload(void);
const char *dvm_webview_engine(void);
void dvm_webview_set_engine(const char *name);
int dvm_webview_count(void);
/* One turn of the main thread's Looper — see dvm.c.  The frame pump calls it
 * so a Handler.postDelayed() callback runs near its due time instead of
 * waiting for the guest to call into Java. */
void dvm_main_looper_tick(struct dvm *vm);

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

/* --- threads ------------------------------------------------------------ *
 *
 * Bytecode threads run on host threads, one at a time, under a global
 * interpreter lock.
 *
 * Until now the VM interpreted a single thread: Thread.start() queued the
 * Runnable and a later drain ran it to completion, or to a step budget.  That
 * is workable for a worker that services a queue and returns, and wrong for
 * one that blocks — the frames of a bytecode call live on the host C stack, so
 * a thread sitting inside a socket read cannot be set aside, and it holds not
 * just the VM but the whole emulator.  The game's patcher downloads 13.7 GB in
 * one doInBackground(); every second of that was a second the engine did not
 * render, and the step budget that kept the freeze bounded aborted the copy
 * instead ("step limit reached in DownloadAsyncTask.doInBackground").
 *
 * A host thread per bytecode thread gives each its own C stack, so blocking is
 * something a thread does rather than something the VM does.  The lock keeps
 * exactly one of them interpreting at a time, which is what lets the heap, the
 * class table and the emulator's own single-threaded guest scheduler stay as
 * they are: this buys concurrency across blocking calls, not parallelism.
 *
 * Whoever executes VM or guest code holds the lock.  A blocking host call —
 * a socket read, a file write, a sleep — drops it around the part that blocks
 * and takes it back afterwards, and so does the frame pump between frames.
 * Anything read out of the VM before a release has to be read again after it:
 * another thread may have moved the heap in between.
 */

/* Recursive: a native that releases the lock around one blocking call may be
 * nested inside another that already holds it. */
void dvm_gil_acquire(struct dvm *vm);
void dvm_gil_release(struct dvm *vm);
/* True when this host thread holds the lock. */
bool dvm_gil_held(struct dvm *vm);

/* Drops the lock for the duration of a blocking host call.  `depth` receives
 * the recursion count so the matching re-acquire restores it exactly.  Between
 * the two, this thread must not touch the VM at all. */
unsigned dvm_gil_unlock_all(struct dvm *vm);
void dvm_gil_relock(struct dvm *vm, unsigned depth);

/* Hands the lock to a thread that is waiting for it, if there is one, and
 * takes it back.  This is what makes the lock fair: a thread interpreting a
 * long loop, and a guest thread spinning in the emulator's own scheduler, both
 * hold the lock for as long as they run, so each has to offer it up.  Callers
 * must have nothing live across it that points into the heap — another thread
 * may allocate, and the heap moves when it grows. */
void dvm_gil_yield(struct dvm *vm);

/* Total wall time, across every thread, spent blocked in dvm_gil_acquire().
 * The interpreter lock is the emulator's other global serialisation point (the
 * ARM execution lock is the first), and a JNI call from guest code has to take
 * it; without a number for it, "the guest is slow" and "the guest is queued
 * behind eight Java threads" look the same. */
unsigned long long dvm_gil_wait_ns(void);
/* Wall time during which some thread held the interpreter lock, and the worst
 * single wait since the last read.  The summed wait above counts idle threads
 * correctly blocked on empty queues; these two say whether the lock is
 * saturated and whether anybody was starved by it. */
unsigned long long dvm_gil_held_ns(void);
unsigned long long dvm_gil_max_wait_ns(void);

/* Block until another thread calls dvm_gil_notify() or `ms` elapses, without
 * holding the interpreter lock; the lock is taken again before returning.
 * Callers must re-check their own condition — the timeout is a backstop, not
 * the mechanism. */
void dvm_gil_wait(struct dvm *vm, unsigned ms);
/* As above, but wake only for changes to `channel`.  Channels are opaque
 * non-zero values; a queue's dvm_ref is a convenient stable identifier. */
void dvm_gil_wait_for(struct dvm *vm, uintptr_t channel, unsigned ms);
/* UINT64_MAX waits until notification without a timeout. */
void dvm_gil_wait_for_ns(struct dvm *vm, uintptr_t channel, uint64_t ns);

/* Enter/leave the VM from guest code that holds the ARM execution lock; see
 * the definition for the lock-order argument.  The cookie is opaque and must
 * be handed back to the matching leave. */
unsigned dvm_gil_enter_from_guest(struct dvm *vm);
void     dvm_gil_leave_to_guest(struct dvm *vm, unsigned cookie);
void dvm_gil_notify(void);
void dvm_gil_notify_for(uintptr_t channel);
void dvm_gil_notify_one_for(uintptr_t channel);

/* The VM this process created, for callers that have to offer the lock up
 * without one to hand (the frame pump, the guest scheduler).  NULL before
 * dvm_create(). */
struct dvm *dvm_current(void);

/* Populate PackageInfo.signatures/signingInfo from the certificate carried by
 * the installed APK.  This is shared by the bytecode PackageManager paths in
 * dvm_runtime.c and dvm_jni.c. */
bool dvm_package_info_add_signatures(struct dvm *vm, dvm_ref package_info,
                                     uint32_t flags);

/* True on a host thread started for bytecode, false on the one that drives the
 * frame pump and the guest CPU.  A wait on the latter has to stay short. */
bool dvm_on_bytecode_thread(void);

/* True in Android's main-thread context. Guest native threads sharing the
 * frame-pump host thread retain their own Android thread identity. */
bool dvm_on_main_thread(void);

struct dvm;

/* Internal IPC envelope. Pack/unpack require the VM lock; dispatch enters a
 * main-looper transaction and takes the lock itself. operation: 1=start, 2=stop.
 * No VM handles are present in the serialized result. Caller frees bytes. */
int dvm_service_pack(struct dvm *,uint32_t intent,int operation,void **bytes,size_t *size);
int dvm_service_unpack(struct dvm *,const void *bytes,size_t size,int *operation,uint32_t *intent);
int dvm_service_dispatch(struct dvm *,const void *bytes,size_t size);

/* The ActivityManager-side transport accepts a serialized start request.
 * Returning 0 means accepted; negative errno means no request was accepted. */
void dvm_service_set_submit(struct dvm *,int (*submit)(void *,const char *,const void *,size_t),void *user);

/* ---- DEX / APK container readers (dex.c, apk_signers.c) ---- */
/* Access flags (dex, class/field/method share the space). */
#define DEX_ACC_PUBLIC       0x00001
#define DEX_ACC_PRIVATE      0x00002
#define DEX_ACC_PROTECTED    0x00004
#define DEX_ACC_STATIC       0x00008
#define DEX_ACC_FINAL        0x00010
#define DEX_ACC_SYNCHRONIZED 0x00020
#define DEX_ACC_VOLATILE     0x00040
#define DEX_ACC_BRIDGE       0x00040
#define DEX_ACC_TRANSIENT    0x00080
#define DEX_ACC_VARARGS      0x00080
#define DEX_ACC_NATIVE       0x00100
#define DEX_ACC_INTERFACE    0x00200
#define DEX_ACC_ABSTRACT     0x00400
#define DEX_ACC_STRICT       0x00800
#define DEX_ACC_SYNTHETIC    0x01000
#define DEX_ACC_ANNOTATION   0x02000
#define DEX_ACC_ENUM         0x04000
#define DEX_ACC_CONSTRUCTOR  0x10000

#define DEX_NO_INDEX 0xffffffffu

struct dex_file {
   const uint8_t *p;
   size_t len;
   char *path;      /* owned; for diagnostics */
   uint8_t *owned;  /* owned mapping when dex_open() read the file */

   uint32_t string_ids_size, string_ids_off;
   uint32_t type_ids_size, type_ids_off;
   uint32_t proto_ids_size, proto_ids_off;
   uint32_t field_ids_size, field_ids_off;
   uint32_t method_ids_size, method_ids_off;
   uint32_t class_defs_size, class_defs_off;

   /* Decoded string pool: one pointer per string_id, filled lazily.  The
    * pointers alias the mapping (MUTF-8 is NUL-terminated in the file), except
    * where a string contains an encoded surrogate pair or an embedded NUL, in
    * which case the entry owns a converted copy (see `fixups`). */
   const char **strings;
   char **fixups;

   /* descriptor → class_def index, built on the first lookup.  Without it
    * dex_find_class() is a strcmp against every class in the file, and
    * dvm_class_is_known() runs that over every dex — which the JNI bridge
    * does on each Call*Method.  With three dexes and 16,500 classes that was
    * the single largest non-guest cost in a profile of world loading. */
   uint32_t *cls_hash;      /* open addressing; 0 empty, else index + 1 */
   uint32_t  cls_hash_cap;  /* power of two */
};

struct dex_method_id {
   uint16_t class_idx;
   uint16_t proto_idx;
   uint32_t name_idx;
};

struct dex_field_id {
   uint16_t class_idx;
   uint16_t type_idx;
   uint32_t name_idx;
};

struct dex_class_def {
   uint32_t class_idx;
   uint32_t access_flags;
   uint32_t superclass_idx;
   uint32_t interfaces_off;
   uint32_t source_file_idx;
   uint32_t annotations_off;
   uint32_t class_data_off;
   uint32_t static_values_off;
};

struct dex_code {
   uint16_t registers_size;
   uint16_t ins_size;
   uint16_t outs_size;
   uint16_t tries_size;
   uint32_t insns_size;      /* in 16-bit code units */
   const uint16_t *insns;
   const uint8_t *tries;     /* try_item[tries_size], or NULL */
   const uint8_t *handlers;  /* encoded_catch_handler_list, or NULL */
};

/* One decoded entry of a class_data_item. */
struct dex_encoded_field {
   uint32_t field_idx;
   uint32_t access_flags;
};

struct dex_encoded_method {
   uint32_t method_idx;
   uint32_t access_flags;
   uint32_t code_off;
};

struct dex_class_data {
   struct dex_encoded_field *static_fields;
   struct dex_encoded_field *instance_fields;
   struct dex_encoded_method *direct_methods;
   struct dex_encoded_method *virtual_methods;
   uint32_t static_fields_size, instance_fields_size;
   uint32_t direct_methods_size, virtual_methods_size;
};

/* An encoded_value, as found in a static_values array. */
enum dex_value_type {
   DEX_VALUE_BYTE = 0x00, DEX_VALUE_SHORT = 0x02, DEX_VALUE_CHAR = 0x03,
   DEX_VALUE_INT = 0x04, DEX_VALUE_LONG = 0x06, DEX_VALUE_FLOAT = 0x10,
   DEX_VALUE_DOUBLE = 0x11, DEX_VALUE_METHOD_TYPE = 0x15,
   DEX_VALUE_METHOD_HANDLE = 0x16, DEX_VALUE_STRING = 0x17,
   DEX_VALUE_TYPE = 0x18, DEX_VALUE_FIELD = 0x19, DEX_VALUE_METHOD = 0x1a,
   DEX_VALUE_ENUM = 0x1b, DEX_VALUE_ARRAY = 0x1c,
   DEX_VALUE_ANNOTATION = 0x1d, DEX_VALUE_NULL = 0x1e,
   DEX_VALUE_BOOLEAN = 0x1f,
};

struct dex_value {
   enum dex_value_type type;
   uint64_t bits;   /* primitive payload, or the pool index for string/type/… */
};

/* --- container ---------------------------------------------------------- */

/* Takes ownership of nothing; `data` must outlive the view. */
bool dex_open_memory(struct dex_file *d, const uint8_t *data, size_t len,
                     const char *path);
/* Reads the whole file; dex_close() frees it. */
bool dex_open(struct dex_file *d, const char *path);
void dex_close(struct dex_file *d);

/* --- pools -------------------------------------------------------------- */

/* NULL when the index is out of range.  Never NULL for a valid index. */
const char *dex_string(struct dex_file *d, uint32_t idx);
/* Type descriptor, e.g. "Ljava/lang/String;" or "[I" or "I". */
const char *dex_type(struct dex_file *d, uint32_t idx);
bool dex_method_id(const struct dex_file *d, uint32_t idx, struct dex_method_id *out);
bool dex_field_id(const struct dex_file *d, uint32_t idx, struct dex_field_id *out);
bool dex_class_def(const struct dex_file *d, uint32_t idx, struct dex_class_def *out);

/* Concatenates the string fragments in a class' dalvik.annotation.Signature
 * annotation.  The annotation is how the generic reflection API obtains
 * information erased from the ordinary superclass/type pools. */
bool dex_class_signature(struct dex_file *d, uint32_t class_def_idx,
                         char *out, size_t out_sz);
/* Generic signature of a declared field, from its system-visible
 * dalvik.annotation.Signature annotation. */
bool dex_field_signature(struct dex_file *d, uint32_t class_def_idx,
                         const char *field_name, char *out, size_t out_sz);
/* Declaring/enclosing class recorded by EnclosingClass or EnclosingMethod. */
const char *dex_class_enclosing_type(struct dex_file *d,
                                     uint32_t class_def_idx);
/* EnclosingMethod's method_id, including constructors and class initializers. */
bool dex_class_enclosing_method(struct dex_file *d, uint32_t class_def_idx,
                                 uint32_t *method_idx);

/* Member classes declare EnclosingClass; local/anonymous classes use EnclosingMethod. */
bool dex_class_is_member(struct dex_file *d, uint32_t class_def_idx);

/* InnerClass.name: true when present, with NULL for an anonymous class. */
bool dex_class_inner_name(struct dex_file *d, uint32_t class_def_idx,
                           const char **name);

/* Source-level access flags retained in the InnerClass system annotation. */
bool dex_class_inner_flags(struct dex_file *d, uint32_t class_def_idx,
                            uint32_t *flags);

/* MemberClasses system metadata: total count, or -1 for malformed data.
 * Fill up to max descriptors; NULL/0 queries the count. */
int dex_class_member_types(struct dex_file *d, uint32_t class_def_idx,
                            const char **types, int max);

/* Runtime-visible class annotations.  `encoded_off` points at the
 * encoded_annotation (its type_idx, not the preceding visibility byte).
 * AnnotationDefault is kept on the annotation interface itself and is used
 * when an instance omits an element with a Java default. */
bool dex_class_annotation(struct dex_file *d, uint32_t class_def_idx,
                          const char *annotation_desc,
                          uint32_t *encoded_off);
/* Fill `encoded_offs`/`types` with every runtime-visible annotation on the
 * class.  Returns the total count (may exceed `max`; only the first `max`
 * slots are written).  `types` entries point into the dex string pool. */
int dex_class_annotations(struct dex_file *d, uint32_t class_def_idx,
                          uint32_t *encoded_offs, const char **types, int max);
bool dex_field_annotation(struct dex_file *d, uint32_t class_def_idx,
                          const char *field_name, const char *annotation_desc,
                          uint32_t *encoded_off);
/* Runtime-visible annotations on the named field; same count/capacity
 * convention as dex_class_annotations. */
int dex_field_annotations(struct dex_file *d, uint32_t class_def_idx,
                           const char *field_name, uint32_t *encoded_offs,
                           const char **types, int max);
/* A method's annotations, named by its name and signature
 * ("(Ljava/lang/String;I)V").  Same conventions as the class forms. */
int dex_method_annotations(struct dex_file *d, uint32_t class_def_idx,
                           const char *name, const char *sig,
                           uint32_t *encoded_offs, const char **types, int max);
bool dex_method_annotation(struct dex_file *d, uint32_t class_def_idx,
                           const char *name, const char *sig,
                           const char *annotation_desc, uint32_t *encoded_off);
/* How many parameter annotation sets the method has (the dex list's size),
 * or -1 when it has none recorded. */
int dex_parameter_annotation_lists(struct dex_file *d, uint32_t class_def_idx,
                                   const char *name, const char *sig);
/* The runtime-visible annotations of parameter `param`. */
int dex_parameter_annotations(struct dex_file *d, uint32_t class_def_idx,
                              const char *name, const char *sig, int param,
                              uint32_t *encoded_offs, const char **types,
                              int max);
/* The method's generic signature, from its dalvik.annotation.Signature. */
bool dex_method_signature(struct dex_file *d, uint32_t class_def_idx,
                          const char *name, const char *sig,
                          char *out, size_t out_sz);
const char *dex_annotation_type(struct dex_file *d, uint32_t encoded_off);
bool dex_annotation_element(struct dex_file *d, uint32_t encoded_off,
                            const char *name, struct dex_value *out);
bool dex_annotation_default(struct dex_file *d, uint32_t class_def_idx,
                            const char *name, struct dex_value *out);

/* Shorty of a proto: return type first, then one char per parameter. */
const char *dex_proto_shorty(struct dex_file *d, uint32_t proto_idx);
const char *dex_proto_return(struct dex_file *d, uint32_t proto_idx);
/* Parameter type descriptors.  Returns the count and fills `out` (at most
 * `max`).  Returns -1 when the proto index is invalid. */
int dex_proto_params(struct dex_file *d, uint32_t proto_idx,
                     const char **out, int max);

/* Builds "(Ljava/lang/String;I)V" into `buf`.  Returns false when it does not
 * fit or the proto is invalid. */
/* Allocated full descriptor; caller owns the returned string. */
char *dex_proto_signature_alloc(struct dex_file *d, uint32_t proto_idx);
bool dex_proto_signature(struct dex_file *d, uint32_t proto_idx,
                         char *buf, size_t buf_sz);

/* Index of the class_def for a type descriptor, or -1. */
int dex_find_class(struct dex_file *d, const char *descriptor);

/* --- class data --------------------------------------------------------- */

/* Allocates the four arrays; free with dex_class_data_release(). */
bool dex_class_data(const struct dex_file *d, uint32_t off, struct dex_class_data *out);
void dex_class_data_release(struct dex_class_data *cd);

bool dex_code(const struct dex_file *d, uint32_t off, struct dex_code *out);

/* Interface type indices of a class_def.  Returns the count, fills at most
 * `max` entries. */
int dex_interfaces(const struct dex_file *d, uint32_t interfaces_off,
                   uint16_t *out, int max);

/* --- encoded values ----------------------------------------------------- */

/* Decodes the encoded_array at `off` (a static_values_off).  Returns the
 * element count and fills at most `max`.  Returns -1 when `off` is 0. */
int dex_static_values(const struct dex_file *d, uint32_t off,
                      struct dex_value *out, int max);
/* Decodes the array payload referenced by a DEX_VALUE_ARRAY. */
int dex_value_array(const struct dex_file *d, const struct dex_value *array,
                    struct dex_value *out, int max);

/* --- low level (exposed for the interpreter's payload decoding) ---------- */

static inline uint32_t dex_u32_at(const struct dex_file *d, size_t off)
{
   if (off + 4 > d->len) return 0;
   uint32_t v;
   __builtin_memcpy(&v, d->p + off, 4);
   return v;
}

static inline uint16_t dex_u16_at(const struct dex_file *d, size_t off)
{
   if (off + 2 > d->len) return 0;
   uint16_t v;
   __builtin_memcpy(&v, d->p + off, 2);
   return v;
}

size_t dex_uleb(const struct dex_file *d, size_t off, uint32_t *out);
size_t dex_sleb(const struct dex_file *d, size_t off, int32_t *out);

/* ---- APK signing certificates (apk_signers.c; needs <openssl/x509.h>) ---- */
struct stack_st_X509;
/* MPL-2.0. APK signing certificate metadata, using the existing OpenSSL. */

/* 1: certificates read; 0: no supported scheme; -1: malformed or unreadable.
 * Returned stacks own their X509 objects. History is oldest-first, including
 * the current certificate, and is NULL unless v3 records key rotation.
 * This reads installed-package metadata; it is not an APK installation verifier. */
int dvm_apk_signers_read(const char *path, unsigned sdk,
                         struct stack_st_X509 **current, struct stack_st_X509 **history);

/* ---- JNI bridge to the guest (dvm_jni.c) ---- */
void dvm_jni_shutdown(void);

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
bool dvm_jni_invoke_nonvirtual(JNIEnv *env, const char *class_name, const char *method,
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

/* android.app.NativeActivity's framework half, run by the guest: stage is a
 * DVM_NATIVE_ACTIVITY_* value and activity the Activity object. */
typedef bool (*dvm_guest_activity_fn)(int stage, jobject activity);
void dvm_jni_set_guest_native_activity(dvm_guest_activity_fn fn);

/* True when the APK's dex declares this class (slash-separated name).  A guest
 * uses ClassLoader.loadClass() to feature-detect optional Java components; the
 * emulator must answer "absent" for a class the APK does not ship, or the guest
 * commits to a code path whose Java half can never run. */
bool dvm_jni_class_in_dex(const char *class_name);

/* PackageManager.getPackageInfo(name, flags) for native callers: the VM's own
 * PackageInfo, as a host handle.  NULL with NameNotFoundException pending when
 * the package is not installed. */
jobject dvm_jni_package_info(JNIEnv *env, jstring name, jint flags);
jobject dvm_jni_application_info(JNIEnv *env, jstring name, jint flags);

/* True when the device we present has the class: the APK's dex or the
 * platform (see dvm_class_exists()).  What FindClass answers from. */
bool dvm_jni_class_exists(const char *class_name);

/* True when the APK's dex carries bytecode for this method (`sig` may be NULL
 * to match on the name alone).  A canned answer in the stub layer must never
 * take precedence over Java the APK actually ships: the app's own
 * implementation is the one the rest of the app agrees with.  Ask this first,
 * and keep the stub only for titles that do not ship the method. */
bool dvm_jni_method_in_dex(const char *class_name, const char *method,
                           const char *sig);

/* Register a dex the guest loaded at runtime (InMemoryDexClassLoader over a
 * direct ByteBuffer, or a DexClassLoader over a file).  See
 * dvm_add_dex_memory(). */
bool dvm_jni_add_dex_memory(const void *data, size_t len, const char *name);
/* Bumped whenever a dex arrives; anything memoised from the class hierarchy
 * has to be dropped when this changes. */
unsigned dvm_jni_dex_epoch(void);

/* Instance field of an object whose class the APK's dex defines, reached from
 * native through Get/SetXxxField.  The bytecode VM owns that object's fields,
 * so a write from native has to land there and not in the stub layer's own
 * side table — otherwise the two halves disagree about the same field.  `bits`
 * is the raw value, the way the JNI accessor macros carry it.  False when the
 * field is not one the VM owns; the caller then keeps its own handling. */
bool dvm_jni_field(JNIEnv *env, jobject obj, jfieldID field, bool set,
                   uint64_t *bits);

/* The same for a *static* field.  The dex owns these too: GameActivity's
 * <clinit> fills ANDROID_BUILD_VERSION from Build.VERSION.SDK_INT, and native
 * code reads it back with GetStaticIntField.  Answering from the stub layer's
 * side table instead reported 0 — an Android older than 1.0 — and the engine
 * then disabled every feature gated on the OS version. */
bool dvm_jni_static_field(JNIEnv *env, jclass cls, jfieldID field, bool set,
                          uint64_t *bits);

/* Superclass of a dex-defined class, slash-separated, or NULL when the APK
 * does not define the class (the framework hierarchy is not in the dex).
 * The stub layer needs it to resolve an inherited framework method called on
 * an app's own subclass. */
const char *dvm_jni_super_name(const char *class_name);

/* Assignability as the dex declares it, interfaces included (`sub instanceof
 * sup`).  Both names are slash-separated.  False when either class is not one
 * the dex defines — the caller then continues through the framework
 * hierarchy, which no dex declares. */
bool dvm_jni_class_assignable(const char *sub, const char *sup);

/* Attach the launcher Activity to the framework window/Activity stack after
 * its lifecycle returns. Used for dex apps whose UI is owned by the framework. */
struct dvm;
struct luna_touch_event;
bool dvm_ui_dispatch_touch(struct dvm *vm, const struct luna_touch_event *sample);
void dvm_jni_show_activity(JNIEnv *env, jobject activity);
struct lm_sink;
jobject dvm_jni_surface_from_sink(JNIEnv *env, struct lm_sink *sink);
struct lm_sink *dvm_jni_surface_acquire_sink(JNIEnv *env, jobject surface);
void dvm_jni_main_transaction(JNIEnv *env, bool begin);
void dvm_jni_activity_dispatch(JNIEnv *env, jobject activity, const char *step,
                               int post);
bool dvm_jni_activity_finishing(JNIEnv *env, jobject activity);

/* Diagnostics for the loader's summary line. */
void dvm_jni_report(void);

/* Publish the process framework graph into the VM before application code. */
bool dvm_jni_bind_application(JNIEnv *env, jobject base, jobject application);
jobject dvm_jni_get_class_loader(JNIEnv *env, jobject object);
/* Publish the process Activity into UnityPlayer.currentActivity (static field
 * in the APK dex).  Plugins such as IronSource read that field via sget. */
void dvm_jni_bind_unity_activity(JNIEnv *env, jobject activity);

struct dvm;
struct dvm *dvm_jni_vm(void);
/* Process status must not initialize bytecode from a native SVC poll. */
bool dvm_jni_process_exited(int *status);
void dvm_jni_exit_process(int status);
/* Write out any preferences an apply() left pending; declared in dvm.h too,
 * and repeated here because the frame pump calls it and does not (and cannot)
 * include the interpreter's own header. */
void dvm_prefs_flush(struct dvm *vm);
/* Declared in dvm.h too; repeated here for the loader's frame pump, which
 * reaches the VM through this header. */
void dvm_ime_frame(struct dvm *vm);
/* One turn of the main thread's Looper — see dvm.c.  The frame pump calls it
 * so a Handler.postDelayed() callback runs near its due time instead of
 * waiting for the guest to call into Java. */
void dvm_main_looper_tick(struct dvm *vm);

/* The interpreter lock, for callers that cannot include dvm.h.
 *
 * dvm/dex.h declares dex_uleb() and dex_string(); loader.c has its own
 * functions by those names for the manifest reader it runs before any VM
 * exists, so pulling dvm.h in there is a redeclaration.  These two are the
 * whole of what the frame pump needs — see the commentary on dvm_gil_acquire()
 * in dvm.h for what the lock is and when to give it up. */
struct dvm;
struct dvm *dvm_current(void);
void dvm_gil_yield(struct dvm *vm);

void dvm_glsurface_tick(struct dvm *vm);

struct dvm;

/* Internal IPC envelope. Pack/unpack require the VM lock; dispatch enters a
 * main-looper transaction and takes the lock itself. operation: 1=start, 2=stop.
 * No VM handles are present in the serialized result. Caller frees bytes. */
int dvm_service_pack(struct dvm *,uint32_t intent,int operation,void **bytes,size_t *size);
int dvm_service_unpack(struct dvm *,const void *bytes,size_t size,int *operation,uint32_t *intent);
int dvm_service_dispatch(struct dvm *,const void *bytes,size_t size);

/* The ActivityManager-side transport accepts a serialized start request.
 * Returning 0 means accepted; negative errno means no request was accepted. */
void dvm_service_set_submit(struct dvm *,int (*submit)(void *,const char *,const void *,size_t),void *user);
