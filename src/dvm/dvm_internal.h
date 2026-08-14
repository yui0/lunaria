/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Shared between the interpreter (dvm.c) and the built-in class library
 * (dvm_runtime.c).  Not part of the module's public interface.
 */

#pragma once

#include "dvm/dvm.h"

#include <string.h>

/* A built-in method: implemented in C, but visible to bytecode exactly like a
 * dex method.  `args` excludes `this` (passed separately). */
typedef bool (*dvm_builtin_fn)(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out);

struct dvm_field {
   const char *name;   /* into the dex mapping, or a literal for builtins */
   const char *type;   /* descriptor */
   uint32_t access;
   struct dvm_class *cls;
   uint16_t slot;      /* index into instance or static slot array */
   uint8_t width;      /* 1 or 2 slots */
   char kind;          /* Z B C S I J F D L, or '[' folded into 'L' */
};

struct dvm_method {
   struct dvm_class *cls;
   const char *name;
   char *sig;          /* owned */
   const char *shorty; /* into the dex mapping, or NULL for builtins */
   uint32_t access;
   struct dex_file *dex;
   struct dex_code code;
   bool has_code;
   dvm_builtin_fn builtin;
   int arg_slots;      /* parameter registers, excluding `this` */
   int arg_count;      /* declared parameters */
   char ret_kind;
};

struct dvm_class {
   char *desc;         /* "Lcom/foo/Bar;" */
   char *name;         /* "com/foo/Bar" */
   struct dvm_class *super;
   struct dvm_class **ifaces;
   int nifaces;
   uint32_t access;

   struct dex_file *dex;
   uint32_t class_def_idx;
   uint32_t static_values_off;

   struct dvm_field *ifields;
   int nifields;
   struct dvm_field *sfields;
   int nsfields;
   union dvm_value *sslots;    /* one per static field, wide fits in one */
   int nsslots;

   struct dvm_method *methods;
   int nmethods;

   int islots;                 /* instance slots including the superclass */
   int init_state;             /* 0 unloaded, 1 running <clinit>, 2 done */

   bool is_array;
   bool is_primitive;
   bool is_interface;
   bool external;              /* no dex definition: host stubs own it */
   char elem_kind;             /* arrays */
   struct dvm_class *elem;     /* arrays */

   dvm_ref class_object;       /* the java.lang.Class instance, made lazily */
   struct dvm_class *hash_next;
};

enum dvm_obj_kind {
   DVM_OBJ_PLAIN,
   DVM_OBJ_STRING,
   DVM_OBJ_ARRAY,
   DVM_OBJ_CLASS,      /* a java.lang.Class */
   DVM_OBJ_EXTERNAL,   /* wraps a host jobject */
};

struct dvm_object {
   struct dvm_class *cls;
   enum dvm_obj_kind kind;
   uint32_t pins;
   uint32_t host_handle;   /* jobject, for DVM_OBJ_EXTERNAL and lazily bound */

   /* strings */
   char *utf8;
   uint32_t utf8_len;

   /* arrays */
   uint32_t length;
   char elem_kind;
   void *data;

   /* java.lang.Class */
   struct dvm_class *klass;

   /* instance fields */
   union dvm_value *slots;

   /* Free-list link; also marks a dead slot when `cls` is NULL. */
   uint32_t next_free;
   bool live;
};

#define DVM_MAX_FRAMES 256
#define DVM_CLASS_HASH 1024

struct dvm_dex {
   struct dex_file file;
   /* Per-index resolution caches.  A dex index is only meaningful inside its
    * own file, so the caches live here rather than in the VM. */
   struct dvm_class **type_cache;
   struct dvm_method **method_cache;
   struct dvm_field **field_cache;
   dvm_ref *string_cache;
};

struct dvm {
   struct dvm_hooks hooks;

   /* Each dex is its own allocation.  Methods and classes keep a
    * `struct dex_file *` into it, and dex_of() resolves that pointer back to
    * its dvm_dex, so the storage must never move — an array grown with
    * realloc() left those pointers dangling, and constant-pool indices then
    * resolved against whatever dex happened to land at the old address. */
   struct dvm_dex **dexes;
   int ndexes, dexes_cap;

   struct dvm_class *class_hash[DVM_CLASS_HASH];
   struct dvm_class **classes;    /* every class, for teardown */
   int nclasses, classes_cap;

   struct dvm_object *heap;
   uint32_t heap_size, heap_cap;
   uint32_t free_head;

   dvm_ref exception;
   /* Frames the current exception unwound through, innermost first.  A bare
    * "NullPointerException: String.length" says nothing about which of the
    * app's methods hit it; this makes an uncaught throw locatable without
    * turning on the full call trace. */
   struct dvm_method *exc_trace[16];
   int nexc_trace;
   dvm_ref exc_ref;      /* the object the trace belongs to */

   /* Threads started from bytecode.  A worker's run() is a loop that waits on
    * a queue, so running it the moment start() is called — before anything has
    * been queued — is wrong.  They are held here and run once the call that
    * started them has returned to the JNI boundary, which is the point where
    * the setting-up work is done.  Each then gets a bounded slice: nothing can
    * preempt it, so a worker that runs out of work has to be stopped rather
    * than left spinning. */
   dvm_ref pending_threads[32];
   /* Whether each entry came from Thread.start() rather than Handler.post():
    * a posted Runnable runs on the main thread, and app code asserts on that. */
   bool pending_is_thread[32];
   /* Monotonic millisecond stamp before which the entry must not run.  A
    * postDelayed() whose delay is dropped turns every "do this unless the fast
    * path beats me to it" timeout into an unconditional one. */
   uint64_t pending_due_ms[32];
   int npending;
   /* The Thread the interpreter is currently inside, or 0 for the main one. */
   dvm_ref cur_thread;
   bool draining;
   bool quiet_uncaught;  /* a parked worker is not an error to report */
   /* Set when the current thread has parked rather than failed: the unwind
    * runs to the top of the thread instead of stopping at a catch clause.
    * See dvm__park() in dvm_runtime.c. */
   bool parked;
   uint32_t exc_pc;      /* bytecode offset of the throw, innermost frame */
   /* The frame currently interpreting bytecode.  Reconstructing the innermost
    * frame from the unwind mis-attributes a throw whenever an inner frame
    * catches and rethrows, so the throw stamps itself here instead. */
   struct dvm_method *cur_method;
   uint32_t cur_pc;
   int depth;
   int trace;
   uint64_t steps;       /* since dvm_create, for diagnostics */
   uint64_t call_steps;  /* since the outermost dvm_call, for the step limit */
   uint64_t step_limit;

   /* Interned string literals, keyed by contents. */
   struct dvm_intern *interns;
   int ninterns, interns_cap;

   /* Names of classes we have already reported as missing, so a hot call site
    * does not fill the log. */
   char **missing;
   int nmissing, missing_cap;
};

/* dvm.c */
struct dvm_class *dvm__register_builtin(struct dvm *vm, const char *desc);
struct dvm_class *dvm__define_primitive(struct dvm *vm, const char *desc);
struct dvm_class *dvm__class_by_desc(struct dvm *vm, const char *desc);
bool dvm__class_assignable(struct dvm *vm, struct dvm_class *from,
                           struct dvm_class *to);
struct dvm_object *dvm__obj(struct dvm *vm, dvm_ref ref);
void dvm__throw(struct dvm *vm, const char *class_name, const char *fmt, ...);
dvm_ref dvm__intern(struct dvm *vm, const char *utf8);
char dvm__kind_of(const char *desc);
int dvm__slots_of(char kind);
bool dvm__sig_param(const char *sig, int idx, char *buf, size_t sz);

/* Dispatch to the host stub layer for a class with no dex definition. */
bool dvm__call_out(struct dvm *vm, struct dvm_class *cls, const char *name,
                   const char *sig, bool is_static, bool is_native,
                   dvm_ref self, const uint32_t *slots, int nslots,
                   union dvm_value *out);

/* Steps a bytecode-started thread may run before it is considered parked. */
#define DVM_THREAD_SLICE (2u * 1000u * 1000u)

/* Runs threads queued by Thread.start(); called when the VM returns to JNI. */
void dvm__run_pending_threads(struct dvm *vm);
/* Monotonic milliseconds, for pending_due_ms. */
uint64_t dvm__now_ms(void);

/* The named SharedPreferences file, created on first use.  The store outlives
 * the objects handed to bytecode, so repeated lookups share one map. */
dvm_ref dvm_runtime_shared_prefs(struct dvm *vm, const char *name);

/* Context.getSystemService(): the one name↔manager-class mapping.  Every path
 * that answers getSystemService — bytecode (rt_context) and the JNI bridge —
 * resolves through these, so they cannot disagree about which services exist. */
struct dvm_system_service {
   const char *key;    /* "vibrator_manager" */
   const char *desc;   /* "Landroid/os/VibratorManager;" */
};
const struct dvm_system_service *dvm_runtime_system_services(size_t *count);
/* Matches a service key, a class descriptor or a bare class name. */
const struct dvm_system_service *dvm_runtime_find_system_service(const char *key);
/* The per-service singleton instance, created on first use. */
dvm_ref dvm_runtime_system_service(struct dvm *vm, const char *key);

/* dvm_runtime.c: installs the built-in classes into a fresh VM. */
void dvm_runtime_install(struct dvm *vm);
/* Called by dvm.c when a class has no dex definition, before falling back to
 * the host stubs.  Returns NULL when the runtime has no built-in for it. */
struct dvm_class *dvm_runtime_define(struct dvm *vm, const char *desc);
