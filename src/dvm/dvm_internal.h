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

#include <stdatomic.h>
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
   bool placeholder_warned; /* a no-op scaffold has reported itself once */
   int arg_slots;      /* parameter registers, excluding `this` */
   int arg_count;      /* declared parameters */
   char ret_kind;
};

/* One resolved lookup, cached on the class it was asked of.  `hash` is zero
 * in an unused slot, so the table needs no separate occupancy word. */
struct dvm_mcache_slot {
   uint32_t hash;
   const char *name;
   const char *sig;          /* NULL matches any signature, as in the lookup */
   struct dvm_method *m;
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
   /* Generic metadata for builtins comes from the Android boot class path,
    * which is not a dex in this VM.  DEX classes read their Signature
    * annotation instead. */
   const char *runtime_signature;
   dvm_ref type_parameters; /* cached TypeVariable[] for stable identity */

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
   /* Invented by dvm__class_by_desc()'s last resort: no dex defines it and the
    * emulator does not implement it either.  For the platform's namespaces
    * that is a stub standing in for something a device really has; for the
    * application's own namespace it is a class that does not exist, and
    * bytecode that names it must be told so.  See new-instance in dvm.c. */
   bool synthesized;
   char elem_kind;             /* arrays */
   struct dvm_class *elem;     /* arrays */

   dvm_ref class_object;       /* the java.lang.Class instance, made lazily */
   struct dvm_class *hash_next;

   /* Methods already looked up on this class — see dvm_find_method(). */
   struct dvm_mcache_slot *mcache;
   uint32_t mcache_cap;        /* a power of two, or 0 before the first put */
   uint32_t mcache_len;
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

   /* A materialised DEX annotation.  Its object class is the annotation
    * interface, while these two words identify the encoded element values.
    * Keeping the immutable mapping reference on the object avoids a global
    * side table and needs no lock beyond the VM GIL. */
   struct dex_file *annotation_dex;
   uint32_t annotation_off;

   /* instance fields */
   union dvm_value *slots;

   /* Java object monitor.  VM state is inspected while GIL is held; waits
    * release GIL through dvm_gil_wait_for(), so no host mutex is needed per
    * object.  The owner is a small process-local thread token, not pthread_t,
    * keeping this representation plain C and comparable. */
   uint64_t monitor_owner;
   uint32_t monitor_depth;
   uint64_t monitor_seq;

   /* Free-list link; also marks a dead slot when `cls` is NULL. */
   uint32_t next_free;
   bool live;
};

/* Objects per heap block.  Big enough that growth is rare, small enough that
 * a VM which allocates a handful of objects does not reserve a megabyte. */
#define DVM_HEAP_BLOCK 4096u

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

   /* Objects, in blocks that never move — see heap_slot() in dvm.c. */
   struct dvm_object **heap_blocks;
   uint32_t heap_nblocks;
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

   /* Handler / Looper posts, and Thread.start fallback when a host thread
    * cannot be created (LUNARIA_DVM_THREADS=0 or pthread_create failure).
    * Executor workers go through Thread.start → host threads; they must not
    * live here — that filled a 32-slot queue at Cross Worlds title and made
    * growing it to 256 look like a fix for what was a mis-routed Executor. */
#define DVM_PENDING_MAX 256
   dvm_ref pending_threads[DVM_PENDING_MAX];
   /* Whether each entry came from Thread.start() rather than Handler.post():
    * a posted Runnable runs on the main thread, and app code asserts on that. */
   bool pending_is_thread[DVM_PENDING_MAX];
   /* Monotonic millisecond stamp before which the entry must not run.  A
    * postDelayed() whose delay is dropped turns every "do this unless the fast
    * path beats me to it" timeout into an unconditional one. */
   uint64_t pending_due_ms[DVM_PENDING_MAX];
   /* Handler.post target Looper.  0 = main drain (also Thread.start).  A
    * background Looper.loop() only takes entries tagged with its own ref —
    * otherwise SwappyDisplayManager$LooperThread greedily runs every posted
    * Runnable on the wrong thread and holds the interpreter lock for them. */
   dvm_ref pending_looper[DVM_PENDING_MAX];
   /* The Handler an entry was posted through, and the token it carried.
    * Handler.removeCallbacksAndMessages(token) is defined in terms of both —
    * "this handler's pending posts and messages, keeping only those whose
    * token differs" — so with neither recorded the call had nothing to key on
    * and did nothing.  A Thread.start() entry has no handler and no token. */
   dvm_ref pending_owner[DVM_PENDING_MAX];
   dvm_ref pending_token[DVM_PENDING_MAX];
   int npending;
   /* The Thread the interpreter is currently inside, or 0 for the main one. */
   dvm_ref cur_thread;
   /* How many drains of the pending queue are on the stack.  This used to be a
    * flat re-entrancy lock, which made every blocking wait inside a Runnable
    * unsatisfiable: the work being waited for sits in this same queue, so a
    * drain that refuses to nest can never deliver it.  Volley's
    * NetworkDispatcher could not run while a Netmarble SDK call sat in
    * CountDownLatch.await() for its response, so the wait burned its whole
    * 15 s timeout with the VM frozen and the request "failed".  The entry
    * being executed is taken off the pending list before it runs, so a nested
    * drain cannot re-enter it — only the depth needs a bound. */
   int drain_depth;
   bool quiet_uncaught;  /* a parked worker is not an error to report */
   /* Set when the current thread has parked rather than failed: the unwind
    * runs to the top of the thread instead of stopping at a catch clause.
    * See dvm__park() in dvm_runtime.c. */
   bool parked;
   uint32_t exc_pc;      /* bytecode offset of the throw, innermost frame */
   /* The frame currently interpreting bytecode.  Reconstructing the innermost
    * frame from the unwind mis-attributes a throw whenever an inner frame
    * catches and rethrows, so the throw stamps itself here instead. */
   /* Atomic because the sampling profiler in dvm.c reads it from its own
    * thread while the interpreter writes it; the write is a relaxed store of
    * a pointer, which is what it already was. */
   struct dvm_method *_Atomic cur_method;
   uint32_t cur_pc;
   /* The bytecode methods currently on the interpreter's stack, outermost
    * first.  Frames live on the C stack, so without this there is nothing to
    * build Thread.getStackTrace() from — and SDKs log their own API calls by
    * reading the caller's name out of that trace. */
   struct dvm_method *callstack[128];
   int ncallstack;
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

/* The interpreter state that belongs to a thread rather than to the VM.
 *
 * The fields stay on `struct dvm` where the interpreter and the thousands of
 * built-in methods already read them by name; only one thread interprets at a
 * time, so the lock hands them over by saving the outgoing thread's copy and
 * loading the incoming one's.  That is what keeps this change out of the
 * built-in class library entirely — see dvm_gil_acquire() in dvm.h. */
struct dvm_tstate {
   dvm_ref exception;
   struct dvm_method *exc_trace[16];
   int nexc_trace;
   dvm_ref exc_ref;
   uint32_t exc_pc;
   dvm_ref cur_thread;
   int drain_depth;
   bool quiet_uncaught;
   bool parked;
   struct dvm_method *cur_method;
   uint32_t cur_pc;
   struct dvm_method *callstack[128];
   int ncallstack;
   int depth;
   uint64_t call_steps;
   uint64_t step_limit;
};
void dvm__tstate_save(struct dvm *vm, struct dvm_tstate *t);
void dvm__tstate_load(struct dvm *vm, const struct dvm_tstate *t);
/* A thread that has not interpreted yet starts from the VM's defaults rather
 * than from whatever the previous holder of the lock left behind. */
void dvm__tstate_reset(struct dvm *vm);
/* Marks the calling host thread as one started for bytecode. */
void dvm__mark_bytecode_thread(void);

/* dvm.c */
struct dvm_class *dvm__register_builtin(struct dvm *vm, const char *desc);
struct dvm_class *dvm__define_primitive(struct dvm *vm, const char *desc);
struct dvm_class *dvm__class_by_desc(struct dvm *vm, const char *desc);
bool dvm__class_assignable(struct dvm *vm, struct dvm_class *from,
                           struct dvm_class *to);
struct dvm_object *dvm__obj(struct dvm *vm, dvm_ref ref);
void dvm__throw(struct dvm *vm, const char *class_name, const char *fmt, ...);
void dvm__warn_placeholder(struct dvm *vm);
/* The builtin whose body is running, for the handful of handlers bound to
 * several overloads at once (Field.get/getInt/..., Field.set/setInt/...)
 * whose behaviour differs by which one the caller named. */
struct dvm_method *dvm__builtin_method(void);
bool dvm__monitor_enter(struct dvm *vm, dvm_ref ref);
bool dvm__monitor_try_enter(struct dvm *vm, dvm_ref ref);
bool dvm__monitor_exit(struct dvm *vm, dvm_ref ref);
bool dvm__monitor_wait(struct dvm *vm, dvm_ref ref, uint64_t timeout_ms,
                       bool *notified);
bool dvm__monitor_notify(struct dvm *vm, dvm_ref ref, bool all);
bool dvm__monitor_state(struct dvm *vm, dvm_ref ref, bool current,
                        uint32_t *depth);
dvm_ref dvm__intern(struct dvm *vm, const char *utf8);
char dvm__kind_of(const char *desc);
int dvm__slots_of(char kind);
bool dvm__sig_param(const char *sig, int idx, char *buf, size_t sz);

/* Dispatch to the host stub layer for a class with no dex definition. */
bool dvm__call_out(struct dvm *vm, struct dvm_class *cls, const char *name,
                   const char *sig, bool is_static, bool is_native,
                   dvm_ref self, const uint32_t *slots, int nslots,
                   union dvm_value *out);

/* java.lang.reflect.Proxy: if `self` is a Proxy/$ProxyN instance, forward the
 * call through its InvocationHandler and return true.  Otherwise false. */
bool dvm_proxy_try_invoke(struct dvm *vm, dvm_ref self, struct dvm_class *cls,
                          const char *name, const char *sig,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out);

/* Steps a bytecode-started thread may run before it is considered parked. */
#define DVM_THREAD_SLICE (2u * 1000u * 1000u)

/* Bytes moved is progress, so it resets the slice.
 *
 * The step limit is there to catch code that loops without getting anywhere.
 * A loop that is reading a socket and writing a file is getting somewhere, and
 * the amount of bytecode it runs is set by the size of what it is moving —
 * the game's content download is 13.7 GB, which is orders of magnitude past
 * any fixed step budget.  Aborting that call throws java.lang.Error into
 * AsyncTask.doInBackground() and the app reports a failed download; on a
 * device the task simply runs until it is done.  So an I/O builtin that
 * actually transferred bytes clears the counter, and a spin that transfers
 * nothing still trips it. */
/* How often the interpreter offers the lock to a waiting thread, in bytecode
 * instructions.  Must be a power of two — the check is on the hot path and is
 * written as a mask.  At a few hundred million instructions a second this is a
 * hand-off opportunity roughly every hundred microseconds, which is far below
 * a frame and far above the cost of the hand-off itself. */
#define DVM_GIL_YIELD_STEPS 32768u

/* Longest a blocking primitive waits for something only another thread can
 * produce.  A thread with its own stack may legitimately wait forever, but a
 * wait on something that will never arrive would keep that thread and
 * everything it references alive for the rest of the run, so the wait ends and
 * says so rather than becoming a leak nobody can see. */
#define DVM_BLOCK_MAX_MS 30000u

/* Whether any bytecode thread other than the pending queue could still produce
 * a result.  The blocking primitives used to answer this with "is the pending
 * queue empty", which stopped being the same question once bytecode ran on
 * host threads. */
bool dvm__other_threads_live(void);

/* Bytes one element of a `kind` array occupies, which is how dvm_new_array()
 * lays the payload out.  It is also the scale sun.misc.Unsafe reports for that
 * array class, so an offset a caller computed from arrayIndexScale() divides
 * back to the element index. */
static inline int dvm__elem_width(char kind)
{
   switch (kind) {
      case 'Z': case 'B': return 1;
      case 'C': case 'S': return 2;
      case 'J': case 'D': return 8;
      default: return 4;
   }
}

#define DVM_IO_PROGRESS_BYTES 4096u
static inline void dvm__note_io_progress(struct dvm *vm, size_t bytes) {
   /* Only a bulk transfer counts.  Incidental I/O — a preference file, a log
    * line, a small config read — happens on every other call, and clearing the
    * budget for those would disable the limit everywhere and let one Runnable
    * hold the VM for as long as it likes. */
   if (vm && bytes >= DVM_IO_PROGRESS_BYTES) vm->call_steps = 0;
}

/* Nesting bound for dvm__run_pending_threads().  Each level is a Runnable that
 * blocked waiting for another one, and each costs C stack, so the queue is
 * allowed to unwrap a chain of waits but not an unbounded one. */
#define DVM_DRAIN_MAX_DEPTH 8

/* How many queued Runnables one drain may run before returning to its caller.
 * The old drain ran every due entry up to eight times over; this keeps a
 * comparable budget while taking the entries one at a time. */
#define DVM_DRAIN_MAX_RUNS 64

/* The emulator's own widget presentation layer: dispatches the clicks the
 * overlay collected and republishes the document when the view tree changed.
 * Runs from the pending-queue drain because that is this VM's main looper —
 * the thread every other posted callback already runs on. */
void dvm__ui_tick(struct dvm *vm);

/* Runs threads queued by Thread.start(); called when the VM returns to JNI. */
void dvm__run_pending_threads(struct dvm *vm);
/* Same queue, for a thread that is blocked waiting on another one: this form
 * is allowed to nest, because what it waits for is queued here too. */
/* Run whatever is due on the pending queue and answer how many ran.  A caller
 * that is about to block needs the count: "the queue is not empty" is not the
 * same as "there was work to do", and treating the two as one turns a blocking
 * wait into a spin whenever the queue holds only entries that are not due. */
int dvm__drain_for_wait(struct dvm *vm);
bool dvm__queue_runnable_at(struct dvm *vm, dvm_ref r, bool as_thread,
                            int64_t delay_ms);
/* LUNARIA_DVM_SCHED: scheduler-only tracing (what the pending queue ran, and
 * what a blocked wait was able to make progress on). */
bool dvm__sched_trace(void);
/* Same switch, filtered by class name: LUNARIA_DVM_SCHED=<substring>. */
bool dvm__sched_trace_for(const char *class_name);

/* How long a thread that parked waits before the queue retries it.  It is
 * blocked on something another thread has to produce, so the retry only has to
 * be frequent enough that the producer's result is picked up promptly. */
#define DVM_PARK_RETRY_MS 8
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
