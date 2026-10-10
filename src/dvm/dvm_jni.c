/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "dvm/dvm_jni.h"
#include "dvm/dvm_internal.h"
#include "jvm/jvm.h"
#include "arm_exec.h"
#include "arm.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct dvm *g_vm;
static bool g_tried;
static enum dvm_jni_mode g_mode = DVM_JNI_FILL_GAPS;
/* The env of the call currently in flight *on this thread*.
 *
 * A framework call from bytecode goes back out through this env, and it used
 * to be one global set for the duration of a guest→VM call.  That held while
 * the VM only ever ran underneath such a call.  Bytecode on a host thread of
 * its own does not start from one: the global was NULL there, and every
 * framework call it made was reported as an unresolved method — Log.d(),
 * Context.getAssets(), Activity.registerReceiver().  The SDK initialisation
 * that made those calls then failed for want of a JNIEnv rather than for want
 * of an implementation.
 *
 * There is one env in the process, so remember it and let a thread with no
 * call of its own use it. */
static _Thread_local JNIEnv *g_env;
static JNIEnv *g_env_any;

/* The env to make outward calls through: this thread's, or the process's.
 *
 * An env whose function table is NULL is not an env.  Callers test the result
 * for NULL and then go straight to (*env)->FindClass, which is table slot 6 —
 * so handing one back segfaults at address 0x30, inside the callee, with
 * nothing in the report to say the env was the problem.  Cross Worlds died
 * exactly there: a bytecode thread running a HandlerThread's Looper has no env
 * of its own, took the process-wide one, and that one's table read as NULL.
 *
 * Say so once and decline.  A caller that cannot get an env reports a missing
 * implementation, which is recoverable and visible; a caller that gets a
 * broken one takes the process down. */
static JNIEnv *current_env(void)
{
   JNIEnv *e = g_env ? g_env : g_env_any;
   if (e && !*e) {
      static bool said;
      if (!said) {
         said = true;
         fprintf(stderr, "[dvm] JNIEnv %p has a NULL function table "
                 "(thread-local=%p process=%p) — declining outward calls "
                 "from this thread\n", (void *)e, (void *)g_env,
                 (void *)g_env_any);
      }
      return NULL;
   }
   return e;
}
static dvm_guest_native_fn g_guest_native;
static dvm_guest_library_fn g_guest_library;
static dvm_guest_activity_fn g_guest_activity;
static unsigned g_calls, g_native_calls, g_external_calls;

/* Read on first use, not in vm_get(): the mode decides whether a call site
 * even asks the emulator, so waiting until the VM is built would make
 * LUNARIA_DVM=2 unreachable — every call with a host stub would take the stub
 * and never create the VM. */
enum dvm_jni_mode dvm_jni_mode(void)
{
   static bool read;
   if (!read) {
      read = true;
      const char *m = getenv("LUNARIA_DVM");
      if (m) g_mode = (enum dvm_jni_mode)atoi(m);
   }
   return g_mode;
}

void dvm_jni_set_guest_native_caller(dvm_guest_native_fn fn) { g_guest_native = fn; }
void dvm_jni_set_guest_library_loader(dvm_guest_library_fn fn) { g_guest_library = fn; }
void dvm_jni_set_guest_native_activity(dvm_guest_activity_fn fn) { g_guest_activity = fn; }

static bool hook_load_library(void *user, struct dvm *vm, const char *name)
{
   (void)user;
   if (!g_guest_library) return false;
   /* ELF loading and constructors execute native code, just like ordinary
    * JNI calls. Keep their ARM state serialized without stopping unrelated
    * bytecode threads. JNI callbacks acquire GIL through the normal bridge.
    * Copy the VM string before allowing other threads to access its heap. */
   char *library=strdup(name);
   if (!library) return false;
   unsigned gil=dvm_gil_unlock_all(vm);
   arm_lock_acquire();
   bool loaded=g_guest_library(library);
   arm_lock_release();
   dvm_gil_relock(vm,gil);
   free(library);
   return loaded;
}

/* ------------------------------------------------------------------------ *
 * Value bridging
 * ------------------------------------------------------------------------ */

/* A host jobject must map to the *same* VM object every time it crosses over.
 * An activity keeps its state in instance fields, and the native side calls it
 * once per lifecycle event: a fresh wrapper per call would reset those fields
 * between calls, which looks exactly like the silent-zero behaviour this
 * module exists to remove. */
struct dvm_wrapper {
   uint32_t host;
   dvm_ref ref;
   uint32_t epoch;   /* the handle slot's epoch when it was bound */
};

/* Host handles are integer keys allocated throughout the process lifetime.
 * A fixed, linearly searched array made every bridge crossing O(n), then lost
 * identity completely after 8192 objects.  Keep an open-addressed table: it
 * has no per-entry allocation or lock (all access is under the DVM GIL), and
 * grows before probing becomes expensive. */
static struct dvm_wrapper *g_wrappers;
static size_t g_wrapper_cap, g_nwrappers;

static size_t wrapper_hash(uint32_t host)
{
   uint32_t x = host;
   x ^= x >> 16;
   x *= UINT32_C(0x7feb352d);
   x ^= x >> 15;
   x *= UINT32_C(0x846ca68b);
   x ^= x >> 16;
   return (size_t)x;
}

static struct dvm_wrapper *wrapper_slot(struct dvm_wrapper *table, size_t cap,
                                        uint32_t host)
{
   if (!table || !cap) return NULL;
   size_t i = wrapper_hash(host) & (cap - 1);
   while (table[i].host && table[i].host != host)
      i = (i + 1) & (cap - 1);
   return &table[i];
}

static bool wrapper_reserve(void)
{
   if (g_wrapper_cap && (g_nwrappers + 1) * 10 < g_wrapper_cap * 7)
      return true;
   size_t cap = g_wrapper_cap ? g_wrapper_cap * 2 : 1024;
   struct dvm_wrapper *table = calloc(cap, sizeof *table);
   if (!table) return false;
   for (size_t i = 0; i < g_wrapper_cap; ++i) {
      if (!g_wrappers[i].host) continue;
      struct dvm_wrapper *slot = wrapper_slot(table, cap, g_wrappers[i].host);
      *slot = g_wrappers[i];
   }
   free(g_wrappers);
   g_wrappers = table;
   g_wrapper_cap = cap;
   return true;
}

static dvm_ref find_wrapper(uint32_t host)
{
   struct dvm_wrapper *slot = wrapper_slot(g_wrappers, g_wrapper_cap, host);
   if (!slot || slot->host != host) return 0;
   /* A wrapper from before the slot was recycled belongs to nothing now. */
   JNIEnv *env = current_env();
   if (env && slot->epoch != jvm_handle_epoch(jnienv_get_jvm(env), (jobject)(uintptr_t)host))
      return 0;
   return slot->ref;
}

static void remember_wrapper(uint32_t host, dvm_ref ref)
{
   if (!host || !ref) return;
   if (!wrapper_reserve()) {
      static int warned;
      if (warned++ < 4) fprintf(stderr,
         "[dvm] cannot grow wrapper table (%zu entries) — host 0x%x loses "
         "instance state across JNI\n", g_nwrappers, host);
      return;
   }
   struct dvm_wrapper *slot = wrapper_slot(g_wrappers, g_wrapper_cap, host);
   if (!slot->host) { slot->host = host; ++g_nwrappers; }
   slot->ref = ref;
   JNIEnv *env = current_env();
   slot->epoch = env ? jvm_handle_epoch(jnienv_get_jvm(env), (jobject)(uintptr_t)host) : 0;
}

/* Does `host` still name `ref`?  A released handle slot is recycled for the
 * next object, and a wrapper left behind would then answer for it.  An object
 * bound without a wrapper has nothing to compare and is taken as current. */
static bool wrapper_current(JNIEnv *env, uint32_t host, dvm_ref ref)
{
   struct dvm_wrapper *slot = wrapper_slot(g_wrappers, g_wrapper_cap, host);
   if (!slot || slot->host != host) return true;
   return slot->ref == ref &&
          slot->epoch == jvm_handle_epoch(jnienv_get_jvm(env), (jobject)(uintptr_t)host);
}

/* Remove an entry without breaking the probe chain following it. */
static void forget_wrapper(uint32_t host, dvm_ref ref)
{
   struct dvm_wrapper *slot = wrapper_slot(g_wrappers, g_wrapper_cap, host);
   if (!slot || slot->host != host || slot->ref != ref) return;
   size_t i = (size_t)(slot - g_wrappers);
   memset(slot, 0, sizeof *slot);
   --g_nwrappers;
   for (size_t j = (i + 1) & (g_wrapper_cap - 1); g_wrappers[j].host;
        j = (j + 1) & (g_wrapper_cap - 1)) {
      struct dvm_wrapper moved = g_wrappers[j];
      memset(&g_wrappers[j], 0, sizeof g_wrappers[j]);
      *wrapper_slot(g_wrappers, g_wrapper_cap, moved.host) = moved;
   }
}

/* JNI locals made while converting bytecode arguments belong to the native
 * call, not to the DVM heap object they came from.  Track only fresh handles;
 * already bound objects keep their established identity. */
extern bool arm_exec_release_guest_direct_buffer(uint32_t handle);
struct bridge_local { jobject handle; dvm_ref source; bool direct; };
struct bridge_frame {
   struct bridge_frame *prev;
   struct bridge_local *locals;
   size_t count, capacity;
};
static _Thread_local struct bridge_frame *g_bridge_frame;

static void bridge_record(jobject handle, dvm_ref source, bool direct)
{
   struct bridge_frame *f = g_bridge_frame;
   if (!f || !handle) return;
   JNIEnv *env = current_env();
   if (env) jvm_mark_bridge_local(jnienv_get_jvm(env), handle);
   if (f->count == f->capacity) {
      size_t cap = f->capacity ? f->capacity * 2 : 16;
      struct bridge_local *p = realloc(f->locals, cap * sizeof *p);
      if (!p) return;
      f->locals = p;
      f->capacity = cap;
   }
   f->locals[f->count++] = (struct bridge_local){ handle, source, direct };
}

static bool bridge_release_one(struct dvm *vm, JNIEnv *env, struct jvm *jvm,
                               const struct bridge_local *local)
{
   if (!jvm_bridge_take_pending(jvm, local->handle)) {
      /* Native code deleted the incoming local itself.  Only drop the DVM
       * binding if that was the last reference and the slot is gone. */
      if (jvm_bridge_ref_count(jvm, local->handle) == 0) {
         struct dvm_object *o = local->source ? dvm__obj(vm, local->source) : NULL;
         if (o && o->host_handle == (uint32_t)(uintptr_t)local->handle) {
            o->host_handle = 0;
            forget_wrapper((uint32_t)(uintptr_t)local->handle, local->source);
         }
      }
      return false;
   }
   const int refs = jvm_bridge_ref_count(jvm, local->handle);
   if (refs > 1) {
      (*env)->DeleteLocalRef(env, local->handle);
      return false;  /* a global reference still owns this handle */
   }
   struct dvm_object *o = local->source ? dvm__obj(vm, local->source) : NULL;
   if (o && o->host_handle == (uint32_t)(uintptr_t)local->handle) {
      o->host_handle = 0;
      forget_wrapper((uint32_t)(uintptr_t)local->handle, local->source);
   }
   const bool released = jvm_release_bridge_local(jvm, local->handle);
   if (local->direct && released)
      arm_exec_release_guest_direct_buffer((uint32_t)(uintptr_t)local->handle);
   return released;
}

static void bridge_end(struct dvm *vm, JNIEnv *env, struct bridge_frame *f)
{
   struct jvm *jvm = jnienv_get_jvm(env);
   size_t released = 0;
   /* Every local the frame made is released, newest first.  An object array keeps
    * its own strong edge to each element (see jvm_array_release_refs), so an
    * element's local reference is not what keeps it alive, and a handle that
    * something else still references (a global reference native code took, an
    * interned object another array holds) only loses this frame's reference
    * (bridge_release_one).  This used to keep the whole frame whenever any one
    * handle had more than one reference — which happens for every Long that
    * interns onto an existing object — and so leaked every handle of such a
    * call: an Object[]{Long} passed to a native a few hundred thousand times
    * filled the 65,536-entry object table and the process aborted. */
   for (size_t i = f->count; i > 0; --i)
      if (bridge_release_one(vm, env, jvm, &f->locals[i - 1])) ++released;
   if (released) {
      static unsigned long long total;
      unsigned long long before = total;
      total += released;
      if (before / 10000 != total / 10000)
         fprintf(stderr, "[dvm-jni] released %llu bridge locals\n", total);
   }
   g_bridge_frame = f->prev;
   free(f->locals);
}

static dvm_ref wrapper_for(struct dvm *vm, JNIEnv *env,
                           const char *class_name, uint32_t host)
{
   if (!host) return 0;
   dvm_ref old = find_wrapper(host);
   if (old) return old;

   /* A DVM wrapper is pinned and may outlive the native frame that supplied
    * `host`.  Keeping only that local-reference number made its lifetime
    * depend on an unrelated DeleteLocalRef: the JVM slot was released while
    * SharedPreferences (and ordinary Java fields) still held the DVM object,
    * and a later round trip returned a non-null jstring whose payload had
    * already gone.  ART's heap objects do not depend on JNI local-reference
    * lifetime.  Give the wrapper its own global reference before publishing
    * it, which is the JNI representation of that ownership. */
   jobject owned = env
      ? (*env)->NewGlobalRef(env, (jobject)(uintptr_t)host)
      : (jobject)(uintptr_t)host;
   if (!owned) return 0;
   host = (uint32_t)(uintptr_t)owned;

   /* A miss means this host handle was never the far side of a VM object, so
    * whatever instance state the VM holds for it is about to be invisible. */
   if (getenv("LUNARIA_TRACE_FIELDS")) {
      static int n;
      if (n++ < 64)
         fprintf(stderr, "[dvm] no VM object behind host handle 0x%x (%s) — "
                 "wrapping it empty\n", host, class_name ? class_name : "?");
   }
   dvm_ref r = dvm_wrap_external(vm, class_name, host);
   if (!r) return 0;
   dvm_pin(vm, r);
   remember_wrapper(host, r);
   return r;
}

/* Element descriptor and width of a host array class name ("[B" → 'B', 1). */
static bool host_array_kind(const char *cls, char *kind, size_t *width)
{
   if (!cls || cls[0] != '[') return false;
   char k = cls[1];
   size_t w;
   switch (k) {
      case 'Z': case 'B': w = 1; break;
      case 'C': case 'S': w = 2; break;
      case 'I': case 'F': w = 4; break;
      case 'J': case 'D': w = 8; break;
      case 'L': case '[': k = 'L'; w = sizeof(void *); break;
      default: return false;
   }
   *kind = k;
   *width = w;
   return true;
}

/* A JNI array argument has to arrive in bytecode as a real array.  Wrapping it
 * as an opaque handle instead made `array-length` on it throw — Epic's
 * ElectraDecoderVideoH264.QueueInputBuffer(int, long, byte[]) reads
 * `data.length`, so every sample it was handed looked like null.
 *
 * Primitive arrays are copied; jvm_array owns its storage and a dvm array owns
 * its own, so the two cannot share one buffer.  Anything a callee writes is
 * copied back by array_sync_back() once the call returns, which is the same
 * bargain GetArrayElements(..., isCopy=true) makes. */
#define ARRAY_REGION(env, dir, kind, o, n, p)                                 \
   do {                                                                       \
      switch (kind) {                                                         \
         case 'Z': (*(env))->dir##BooleanArrayRegion(env, o, 0, n, p); break; \
         case 'B': (*(env))->dir##ByteArrayRegion(env, o, 0, n, p); break;    \
         case 'C': (*(env))->dir##CharArrayRegion(env, o, 0, n, p); break;    \
         case 'S': (*(env))->dir##ShortArrayRegion(env, o, 0, n, p); break;   \
         case 'I': (*(env))->dir##IntArrayRegion(env, o, 0, n, p); break;     \
         case 'J': (*(env))->dir##LongArrayRegion(env, o, 0, n, p); break;    \
         case 'F': (*(env))->dir##FloatArrayRegion(env, o, 0, n, p); break;   \
         case 'D': (*(env))->dir##DoubleArrayRegion(env, o, 0, n, p); break;  \
         default: break;                                                      \
      }                                                                       \
   } while (0)

/* jvm_get_class_name() takes a *class* handle, not an instance — asking it
 * about an object gets the zeroed dummy and a NULL name.  Go through
 * GetObjectClass, which is what holds the instance's class. */
static const char *class_name_of(JNIEnv *env, jobject o)
{
   if (!o) return NULL;
   jclass c = (*env)->GetObjectClass(env, o);
   return c ? jvm_get_class_name(jnienv_get_jvm(env), c) : NULL;
}

static dvm_ref from_jobject(struct dvm *vm, JNIEnv *env, jobject o);

static dvm_ref host_array_to_dvm(struct dvm *vm, JNIEnv *env, jobject o)
{
   const char *cls = class_name_of(env, o);
   char kind;
   size_t width;
   if (!host_array_kind(cls, &kind, &width)) return 0;

   jsize n = (*env)->GetArrayLength(env, o);
   if (n < 0) n = 0;

   if (kind == 'L') {
      /* Object arrays used to fall through to an opaque wrapper, so
       * array-length threw NPE (IronSource AndroidBridge.init(String,String[])).
       * Element descriptor is the host class name with the leading '[' removed,
       * dots normalised to slashes for dvm__class_by_desc. */
      char ed[256];
      const char *rest = cls + 1;
      size_t i = 0;
      for (; rest[i] && i + 1 < sizeof ed; ++i)
         ed[i] = (rest[i] == '.') ? '/' : rest[i];
      ed[i] = '\0';
      dvm_ref r = dvm_new_array(vm, 'L', ed[0] ? ed : "Ljava/lang/Object;",
                                (uint32_t)n);
      dvm_ref *slots = r ? dvm_array_data(vm, r) : NULL;
      for (jsize ei = 0; slots && ei < n; ++ei) {
         jobject el = (*env)->GetObjectArrayElement(env, (jobjectArray)o, ei);
         slots[ei] = from_jobject(vm, env, el);
         if (slots[ei]) dvm_pin(vm, slots[ei]);
         if (el) (*env)->DeleteLocalRef(env, el);
      }
      struct dvm_object *ao = r ? dvm__obj(vm, r) : NULL;
      if (ao) {
         /* The identity-bound VM array outlives the native return local. */
         ao->host_handle = (uint32_t)(uintptr_t)(*env)->NewGlobalRef(env,o);
      }
      return r;
   }

   char elem[2] = { kind, 0 };
   dvm_ref r = dvm_new_array(vm, kind, elem, (uint32_t)n);
   void *dst = r ? dvm_array_data(vm, r) : NULL;
   if (dst && n > 0) ARRAY_REGION(env, Get, kind, o, n, dst);
   return r;
}

/* The other direction of the copy above, run after the callee returns.
 * (Object arrays are identity-bound via host_handle; no bulk copy-back.) */
static void array_sync_back(struct dvm *vm, JNIEnv *env, jobject o, dvm_ref r)
{
   if (!o || !r) return;
   char kind;
   size_t width;
   if (!host_array_kind(class_name_of(env, o), &kind, &width) || kind == 'L')
      return;
   void *src = dvm_array_data(vm, r);
   jsize n = (jsize)dvm_array_length(vm, r);
   if (src && n > 0 && n == (*env)->GetArrayLength(env, o))
      ARRAY_REGION(env, Set, kind, o, n, src);
}

/* --- direct ByteBuffers ---------------------------------------------------
 *
 * ByteBuffer.allocateDirect() promises memory JNI can address.  The VM's
 * array lives in host memory the guest cannot name, so the bridge gives the
 * buffer a region of guest memory as well and copies the array through it
 * around a native call.  Without an address, GetDirectBufferAddress() answers
 * 0 and a native does not fail politely: Unity's UnityWebRequest upload loop
 * takes its "how many bytes are there in total" branch instead of its "fill
 * this buffer" one, so the loop's exit condition is never reached and it
 * calls the native for ever. */
extern uint32_t arm_exec_new_guest_direct_buffer(uint64_t cap,
                                                 uint64_t *addr_out);
extern void *arm_exec_direct_buffer_host(uint32_t handle, uint64_t *cap_out);

static bool dvm_buffer_is_direct(struct dvm *vm, dvm_ref r)
{
   if (!r) return false;
   /* The class check is not belt-and-braces: `direct` is an ordinary field
    * name and a dex class of the app's own may well have one. */
   struct dvm_class *c = dvm_object_class(vm, r);
   if (!c || !c->name || strcmp(c->name, "java/nio/ByteBuffer")) return false;
   union dvm_value d = { 0 };
   return dvm_get_field(vm, r, "direct", "Z", &d) && d.i != 0;
}

/* The buffer's bytes and their length, or NULL. */
static uint8_t *dvm_buffer_bytes(struct dvm *vm, dvm_ref r, uint32_t *len)
{
   union dvm_value bufv = { 0 };
   *len = 0;
   if (!dvm_get_field(vm, r, "buf", "[B", &bufv) || !bufv.l) return NULL;
   *len = dvm_array_length(vm, bufv.l);
   return dvm_array_data(vm, bufv.l);
}

/* The guest-backed handle for this buffer, allocated once and remembered on
 * the object, with the VM's bytes copied into it. */
static uint32_t dvm_direct_buffer_to_host(struct dvm *vm, dvm_ref r)
{
   uint32_t len = 0;
   uint8_t *src = dvm_buffer_bytes(vm, r, &len);
   if (!src || !len) return 0;

   uint32_t handle = dvm_external_handle(vm, r);
   if (!handle) {
      uint64_t addr = 0;
      handle = arm_exec_new_guest_direct_buffer(len, &addr);
      if (!handle) return 0;
      struct dvm_object *o = dvm__obj(vm, r);
      if (!o) return 0;
      o->host_handle = handle;
      remember_wrapper(handle, r);
      bridge_record((jobject)(uintptr_t)handle, r, true);
   }
   uint64_t cap = 0;
   void *dst = arm_exec_direct_buffer_host(handle, &cap);
   if (dst && cap) memcpy(dst, src, cap < len ? (size_t)cap : (size_t)len);
   return handle;
}

/* The other direction, once the native has written into the region. */
static void dvm_direct_buffer_from_host(struct dvm *vm, dvm_ref r)
{
   uint32_t len = 0;
   uint8_t *dst = dvm_buffer_bytes(vm, r, &len);
   const uint32_t handle = dvm_external_handle(vm, r);
   if (!dst || !len || !handle) return;
   uint64_t cap = 0;
   const void *src = arm_exec_direct_buffer_host(handle, &cap);
   if (src && cap) memcpy(dst, src, cap < len ? (size_t)cap : (size_t)len);
}

/* jobject → dvm_ref.  A jstring becomes a real VM string so bytecode can call
 * length()/equals() on it; a Class becomes the VM's Class for that type so
 * getDeclaredMethods()/getMethod() see the dex methods of the class the
 * handle names, not java.lang.Class's own table; anything else is wrapped so
 * its identity survives the round trip back to the stub layer. */
static dvm_ref from_jobject(struct dvm *vm, JNIEnv *env, jobject o)
{
   /* Wrapper identity belongs to the object, not to a weak handle naming it.
    * A cleared weak reference crosses the Java boundary as null. */
   o = jvm_resolve_reference(jnienv_get_jvm(env), o);
   if (!o) return 0;
   dvm_ref arr = host_array_to_dvm(vm, env, o);
   if (arr) return arr;
   /* Class arguments (FindClass / GetObjectClass results) must stay Class
    * objects in the VM.  Wrapping them as instances of java.lang.Class made
    * ReflectionHelper.getMethodID walk Class's own methods and miss every
    * getInstance()/setConsent() on the type Unity actually asked about. */
   {
      struct jvm *jvm = jnienv_get_jvm(env);
      const char *described = jvm_described_class_name(jvm, o);
      if (described && *described) {
         /* Only a class the VM defines becomes a VM Class object.  A class
          * that exists solely in the host stub layer has no methods or fields
          * here, so a Class object for it would answer an empty
          * getConstructors()/getDeclaredMethods() — reflection over it would
          * fail rather than reach the stub that can service it.  Such a
          * handle stays a wrapper, which is what carries it back out to the
          * layer that owns it.  (Its getName() is wrong here; fixing that
          * means giving external classes real reflective members, not
          * renaming the wrapper — see PROGRESS.md.) */
         struct dvm_class *c = dvm_find_class(vm, described);
         if (c) {
            dvm_ref r = dvm_class_object(vm, c);
            if (r) dvm_pin(vm, r);
            return r;
         }
      }
   }
   /* Both VM interiors use WTF-8 with an explicit byte length. Avoid JNI's
    * modified UTF-8 conversion here, retaining NUL and surrogate pairs. */
   jclass sc = (*env)->GetObjectClass(env, o);
   if (sc) {
      jclass strc = (*env)->FindClass(env, "java/lang/String");
      if (strc && (*env)->IsInstanceOf(env, o, strc)) {
         size_t bytes;
         const char *utf = jvm_string_wtf8(jnienv_get_jvm(env), (jstring)o, &bytes);
         return dvm_new_string_n(vm, utf, bytes);
      }
   }
   /* Wrap it as what it actually is.  Naming every incoming object
    * "java/lang/Object" threw away the one piece of type information the
    * stub layer had: bytecode could not dispatch a virtual call on it, an
    * instanceof against its real class answered false, and any class the VM
    * implements itself (AssetManager, File, …) was unreachable through an
    * object that arrived this way. */
   const char *cn = class_name_of(env, o);
   /* A ProviderInfo allocated by the framework bridge has its fields in the
    * host JVM.  Read them before registering the DVM wrapper: once registered,
    * JNI field access correctly resolves to the DVM object instead. */
   if (cn && (!strcmp(cn, "android/content/pm/ProviderInfo") ||
              !strcmp(cn, "android.content.pm.ProviderInfo"))) {
      jclass pc = (*env)->GetObjectClass(env, o);
      jfieldID grant = (*env)->GetFieldID(env, pc, "grantUriPermissions", "Z");
      jfieldID authority = (*env)->GetFieldID(env, pc, "authority",
                                             "Ljava/lang/String;");
      jboolean grant_value = grant ? (*env)->GetBooleanField(env, o, grant) : JNI_FALSE;
      jobject authority_value = authority ? (*env)->GetObjectField(env, o, authority) : NULL;
      if (getenv("LUNARIA_TRACE_FIELDS"))
         fprintf(stderr, "[dvm] ProviderInfo import host=%p grant=%d authority=%p\n",
                 (void *)o, (int)grant_value, (void *)authority_value);
      dvm_ref r = wrapper_for(vm, env, cn, (uint32_t)(uintptr_t)o);
      union dvm_value value = { .i = grant_value ? 1 : 0 };
      (void)dvm_set_field(vm, r, "grantUriPermissions", "Z", value);
      value.l = authority_value ? from_jobject(vm, env, authority_value) : 0;
      (void)dvm_set_field(vm, r, "authority", "Ljava/lang/String;", value);
      return r;
   }
   return wrapper_for(vm, env, cn && *cn ? cn : "java/lang/Object",
                      (uint32_t)(uintptr_t)o);
}

/* dvm_ref → jobject.  Strings become real jstrings; a wrapper hands back the
 * handle it came in with; anything else becomes an opaque object of the right
 * class, so IsInstanceOf and GetObjectClass on the stub side still work. */
/* A VM array going back to native, e.g. the byte[] Epic's decoder hands out of
 * GetOutputBuffer().  Without this it became an opaque object and the caller
 * read nothing out of it. */
static jobject to_jobject(struct dvm *vm, JNIEnv *env, dvm_ref r);

static jobject dvm_array_to_host(struct dvm *vm, JNIEnv *env, dvm_ref r)
{
   struct dvm_object *o = dvm__obj(vm, r);
   if (!o || o->kind != DVM_OBJ_ARRAY) return NULL;

   jsize n = (jsize)o->length;
   char kind = o->elem_kind;
   jobject h = NULL;
   switch (kind) {
      case 'Z': h = (*env)->NewBooleanArray(env, n); break;
      case 'B': h = (*env)->NewByteArray(env, n);    break;
      case 'C': h = (*env)->NewCharArray(env, n);    break;
      case 'S': h = (*env)->NewShortArray(env, n);   break;
      case 'I': h = (*env)->NewIntArray(env, n);     break;
      case 'J': h = (*env)->NewLongArray(env, n);    break;
      case 'F': h = (*env)->NewFloatArray(env, n);   break;
      case 'D': h = (*env)->NewDoubleArray(env, n);  break;
      /* Object arrays.  Falling through to the plain-object wrapper turned an
       * array into a bare java.lang.Object: GetArrayLength() then answered 0
       * and the caller concluded the method had returned nothing.  UE's
       * FJavaAndroidMediaPlayer::GetVideoTracks() is exactly that shape — it
       * reads MediaPlayer14.GetVideoTracks()'s VideoTrackInfo[] through
       * GetArrayLength/GetObjectArrayElement — so every movie came back with
       * zero video tracks, SelectedVideoTrack stayed INDEX_NONE and
       * FAndroidMediaPlayer::TickFetch never fetched a frame. */
      default: {
         const char *elem = (o->cls && o->cls->elem && o->cls->elem->name)
            ? o->cls->elem->name : "java/lang/Object";
         jclass ec = (*env)->FindClass(env, elem);
         if (!ec) ec = (*env)->FindClass(env, "java/lang/Object");
         h = (*env)->NewObjectArray(env, n, ec, NULL);
         if (!h) return NULL;
         /* Bind the handle before converting the elements: an element that
          * refers back to this array then finds it instead of building a
          * second one. */
         o->host_handle = (uint32_t)(uintptr_t)h;
         bridge_record(h, r, false);
         const dvm_ref *items = (const dvm_ref *)o->data;
         for (jsize i = 0; items && i < n; ++i)
            (*env)->SetObjectArrayElement(env, (jobjectArray)h, i,
                                          to_jobject(vm, env, items[i]));
         return h;
      }
   }
   if (!h) return NULL;
   if (n > 0 && o->data) ARRAY_REGION(env, Set, kind, h, n, o->data);
   o->host_handle = (uint32_t)(uintptr_t)h;
   bridge_record(h, r, false);
   return h;
}

static jobject to_jobject(struct dvm *vm, JNIEnv *env, dvm_ref r)
{
   if (!r) return NULL;
   /* Before the handle shortcut: a direct buffer's bytes have to be in the
    * guest region on every crossing, not only the first. */
   if (dvm_buffer_is_direct(vm, r)) {
      uint32_t bb = dvm_direct_buffer_to_host(vm, r);
      if (bb) return (jobject)(uintptr_t)bb;
   }
   /* Each time a Java object crosses into native code the native side gets a
    * reference of its own (JNI: every returned or passed object is a new local
    * reference).  Handing back the existing handle as is let one thread's
    * DeleteLocalRef free the slot another thread still held.  A handle whose
    * slot has gone is not reused. */
   uint32_t host = dvm_external_handle(vm, r);
   /*TMP*/{ struct dvm_class *tc = dvm_object_class(vm, r); if (tc && tc->name && strstr(tc->name, "payment/Payment") && !strstr(tc->name, "$")) fprintf(stderr, "[TMP] to_jobject Payment r=0x%x host=0x%x refs=%d cur=%d jclass=%s\n", r, host, host ? jvm_bridge_ref_count(jnienv_get_jvm(env), (jobject)(uintptr_t)host) : -1, host ? (int)wrapper_current(env, host, r) : -1, host ? class_name_of(env, (jobject)(uintptr_t)host) : "-"); }
   if (host) {
      if (jvm_bridge_ref_count(jnienv_get_jvm(env), (jobject)(uintptr_t)host) > 0 &&
          wrapper_current(env, host, r))
         return (*env)->NewLocalRef(env, (jobject)(uintptr_t)host);
      struct dvm_object *stale = dvm__obj(vm, r);
      if (stale) stale->host_handle = 0;
      forget_wrapper(host, r);
   }

   jobject arr = dvm_array_to_host(vm, env, r);
   if (arr) return arr;

   const char *s = dvm_string_utf8(vm, r);
   if (s) {
      size_t bytes = dvm_string_utf8_length(vm, r);
      jobject h = (jobject)jvm_new_string_wtf8(jnienv_get_jvm(env), s, bytes);
      bridge_record(h, 0, false);
      return h;
   }

   struct dvm_class *c = dvm_object_class(vm, r);
   if (getenv("LUNARIA_TRACE_JNI")) {
      struct dvm_object *raw = dvm__obj(vm, r);
      fprintf(stderr, "[dvm-jni] non-payload object ref=0x%x kind=%d class=%s\n",
              r, raw ? (int)raw->kind : -1,
              (c && c->name) ? c->name : "(none)");
   }

   /* Preserve the immutable event when a Java View forwards host input to
    * native code. An opaque MotionEvent has no samples for JNI getters. */
   if (c && c->name && !strcmp(c->name, "android/view/MotionEvent")) {
      lunaria_touch_event event = {0};
      if (!dvm_motion_event_read(vm, r, &event)) return NULL;
      jobject handle = jvm_new_motion_event(jnienv_get_jvm(env), &event);
      struct dvm_object *object = dvm__obj(vm, r);
      if (object) object->host_handle = (uint32_t)(uintptr_t)handle;
      remember_wrapper((uint32_t)(uintptr_t)handle, r);
      bridge_record(handle, r, false);
      return handle;
   }

   /* A java.lang.String may never cross JNI as a bare AllocObject(String).
    * Such an object has no String payload in the host VM: it is non-null, but
    * GetStringUTFChars returns NULL.  UE's ordinary JNI_String conversion then
    * quite correctly treats the non-null jstring as a String and calls
    * strlen() on the promised character pointer.  A few framework-created
    * optional strings arrive as a typed DVM object with no utf8 payload (the
    * GameActivity AppType is one); Android represents their usable empty
    * value as "", never as a payload-less String instance. */
   if (c && c->name && (!strcmp(c->name, "java/lang/String") ||
                        !strcmp(c->name, "Ljava/lang/String;")))
      return (jobject)(*env)->NewStringUTF(env, "");

   /* A java.lang.Class must cross as the host jclass for the class it
    * *represents*, not as an instance of java.lang.Class.
    *
    * Without this it fell through to the AllocObject path below, whose
    * FindClass argument is dvm_object_class() — "java/lang/Class" — so every
    * Class the VM handed to native code came out as one anonymous, empty
    * java.lang.Class instance that names nothing.  Native code does not read
    * fields off a Class; it passes it to FindClass-shaped APIs
    * (GetObjectClass results, NewObjectArray's element class,
    * IsInstanceOf/IsAssignableFrom, Class.getName, reflection), and those
    * cannot work on an opaque that has no class behind it.
    *
    * klass->name is already the internal form FindClass wants, including for
    * array classes ("[I", "[Ljava/lang/String;").  A Class object with no
    * klass — a primitive class the VM models differently — is left to the
    * generic path rather than guessed at. */
   if (c && c->name && !strcmp(c->name, "java/lang/Class")) {
      struct dvm_object *co = dvm__obj(vm, r);
      if (co && co->kind == DVM_OBJ_CLASS && co->klass && co->klass->name) {
         jclass jc = (*env)->FindClass(env, co->klass->name);
         if (jc) {
            co->host_handle = (uint32_t)(uintptr_t)jc;
            remember_wrapper(co->host_handle, r);
            return (jobject)jc;
         }
      }
   }

   /* reflect.Method / Field must become real jmethodID / jfieldID objects.
    * AllocObject left an empty opaque; FromReflectedMethod then handed that
    * opaque to Call*Method, which rejected it (not JVM_OBJECT_METHOD). */
   if (c && c->name &&
       (!strcmp(c->name, "java/lang/reflect/Method") ||
        !strcmp(c->name, "java/lang/reflect/Constructor") ||
        !strcmp(c->name, "java/lang/reflect/Field"))) {
      union dvm_value ownerv = { 0 }, namev = { 0 }, sigv = { 0 }, accv = { 0 };
      (void)dvm_get_field(vm, r, "owner", "Ljava/lang/Class;", &ownerv);
      (void)dvm_get_field(vm, r, "name", "Ljava/lang/String;", &namev);
      (void)dvm_get_field(vm, r, "sig", "Ljava/lang/String;", &sigv);
      (void)dvm_get_field(vm, r, "access", "I", &accv);
      struct dvm_object *oo = dvm__obj(vm, ownerv.l);
      const char *cname = (oo && oo->kind == DVM_OBJ_CLASS && oo->klass)
                             ? oo->klass->name
                             : NULL;
      const char *nm = dvm_string_utf8(vm, namev.l);
      const char *sg = dvm_string_utf8(vm, sigv.l);
      if (cname && nm && sg) {
         jclass jc = (*env)->FindClass(env, cname);
         if (jc) {
            const bool is_static = (accv.i & 0x0008) != 0;
            jobject id;
            if (!strcmp(c->name, "java/lang/reflect/Field"))
               id = is_static
                  ? (jobject)(*env)->GetStaticFieldID(env, jc, nm, sg)
                  : (jobject)(*env)->GetFieldID(env, jc, nm, sg);
            else if (nm[0] == '<' && !strcmp(nm, "<init>"))
               id = (jobject)(*env)->GetMethodID(env, jc, nm, sg);
            else
               id = is_static
                  ? (jobject)(*env)->GetStaticMethodID(env, jc, nm, sg)
                  : (jobject)(*env)->GetMethodID(env, jc, nm, sg);
            if (id) {
               struct dvm_object *obj = dvm__obj(vm, r);
               if (obj) {
                  obj->host_handle = (uint32_t)(uintptr_t)id;
                  remember_wrapper(obj->host_handle, r);
               }
               return id;
            }
         }
      }
   }

   jclass cls = (*env)->FindClass(env, c ? c->name : "java/lang/Object");
   jobject o = (*env)->AllocObject(env, cls);
   if (getenv("LUNARIA_TRACE_FIELDS")) {
      static int n;
      if (n++ < 96)
         fprintf(stderr, "[dvm] to_jobject @%x (%s) -> host 0x%x\n", r,
                 c && c->name ? c->name : "?", (unsigned)(uintptr_t)o);
   }
   struct dvm_object *obj = dvm__obj(vm, r);
   if (obj) {
      obj->host_handle = (uint32_t)(uintptr_t)o;
      /* The object can immediately cross back through a native callback.
       * Record the reverse edge now; otherwise from_jobject() wraps the host
       * handle as a fresh VM object and loses every instance field. */
      remember_wrapper(obj->host_handle, r);
      bridge_record(o, r, false);
   }
   return o;
}

/* Fills one dvm value from a JNI argument of the given descriptor. */
static union dvm_value jvalue_to_dvm(struct dvm *vm, JNIEnv *env,
                                     const char *desc, jvalue v)
{
   union dvm_value o = { 0 };
   switch (desc[0]) {
      case 'Z': o.i = v.z ? 1 : 0; break;
      case 'B': o.i = v.b; break;
      case 'C': o.i = (uint16_t)v.c; break;
      case 'S': o.i = v.s; break;
      case 'I': o.i = v.i; break;
      case 'J': o.j = v.j; break;
      case 'F': o.f = v.f; break;
      case 'D': o.d = v.d; break;
      default:  o.l = from_jobject(vm, env, v.l); break;
   }
   return o;
}

static jvalue dvm_to_jvalue(struct dvm *vm, JNIEnv *env, char kind, union dvm_value v)
{
   jvalue o;
   memset(&o, 0, sizeof o);
   switch (kind) {
      case 'Z': o.z = v.i ? 1 : 0; break;
      case 'B': o.b = (jbyte)v.i; break;
      case 'C': o.c = (jchar)v.i; break;
      case 'S': o.s = (jshort)v.i; break;
      case 'I': o.i = v.i; break;
      case 'J': o.j = v.j; break;
      case 'F': o.f = v.f; break;
      case 'D': o.d = v.d; break;
      case 'V': break;
      default:  o.l = to_jobject(vm, env, v.l); break;
   }
   return o;
}

/* Pulls one argument out of a varargs list.  Small integer types are promoted
 * to int and float to double by the C calling convention, so they have to be
 * read at the promoted width. */
static jvalue va_next(va_list *ap, const char *desc)
{
   jvalue v;
   memset(&v, 0, sizeof v);
   switch (desc[0]) {
      case 'Z': v.z = (jboolean)va_arg(*ap, int); break;
      case 'B': v.b = (jbyte)va_arg(*ap, int); break;
      case 'C': v.c = (jchar)va_arg(*ap, int); break;
      case 'S': v.s = (jshort)va_arg(*ap, int); break;
      case 'I': v.i = va_arg(*ap, jint); break;
      case 'J': v.j = va_arg(*ap, jlong); break;
      case 'F': v.f = (jfloat)va_arg(*ap, double); break;
      case 'D': v.d = va_arg(*ap, double); break;
      default:  v.l = va_arg(*ap, jobject); break;
   }
   return v;
}

/* ------------------------------------------------------------------------ *
 * Hooks: bytecode calling out
 * ------------------------------------------------------------------------ */

/* ApplicationInfo's on-disk layout: where the installer put the base APK and,
 * for an app shipped as an App Bundle, each split it also wrote.
 *
 * An install-time asset pack is nothing but one of those splits, and Play Core
 * resolves a pack by walking splitNames/splitSourceDirs — with both fields null
 * it reports "No splits are found or app cannot be found in package manager"
 * and then "Pack not found with pack name: <pack>".  The launcher hands the set
 * over in ANDROID_SPLIT_APKS as "name|path;name|path". */
/* Builds the two parallel String[]s the framework exposes for installed
 * splits.  Returns how many there are; 0 leaves both refs untouched. */
static size_t dvm_build_split_arrays(struct dvm *vm, dvm_ref *out_names,
                                     dvm_ref *out_dirs)
{
   const char *splits = getenv("ANDROID_SPLIT_APKS");
   size_t n = 0;
   for (const char *p = splits; p && *p; ) {
      const char *end = strchr(p, ';');
      if (!end) end = p + strlen(p);
      const char *bar = memchr(p, '|', (size_t)(end - p));
      if (bar && bar != p) ++n;
      p = (*end == ';') ? end + 1 : end;
   }
   /* A package installed from a single APK genuinely has no split arrays;
    * only describe them when the launcher actually installed splits. */
   if (!n) return 0;
   /* Collect first, then sort by name.  The framework keeps splitNames sorted
    * and Play Core depends on it: it locates a pack with
    * Arrays.binarySearch(splitNames, pack) and indexes splitSourceDirs with
    * whatever comes back, so the two arrays must agree index for index and be
    * in the order a binary search expects.  The launcher lists the splits in
    * install order, which is not that order. */
   struct split_ent { const char *name; size_t nlen; };
   struct split_ent *e = calloc(n, sizeof *e);
   if (!e) return 0;
   size_t i = 0;
   for (const char *p = splits; *p && i < n; ) {
      const char *end = strchr(p, ';');
      if (!end) end = p + strlen(p);
      const char *bar = memchr(p, '|', (size_t)(end - p));
      if (bar && bar != p) {
         e[i].name = p;       e[i].nlen = (size_t)(bar - p);
         ++i;
      }
      p = (*end == ';') ? end + 1 : end;
   }
   n = i;
   if (n > UINT32_MAX) { free(e); return 0; }
   dvm_ref names = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
   dvm_ref dirs  = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
   dvm_ref *nslot = names ? dvm_array_data(vm, names) : NULL;
   dvm_ref *dslot = dirs  ? dvm_array_data(vm, dirs)  : NULL;
   if (!nslot || !dslot) { free(e); return 0; }
   for (size_t a = 1; a < n; ++a) {           /* insertion sort; n is tiny */
      for (size_t b = a; b > 0; --b) {
         size_t la = e[b - 1].nlen, lb = e[b].nlen, m = la < lb ? la : lb;
         int c = strncmp(e[b - 1].name, e[b].name, m);
         if (c == 0) c = la < lb ? -1 : la > lb ? 1 : 0;
         if (c <= 0) break;
         struct split_ent t = e[b - 1]; e[b - 1] = e[b]; e[b] = t;
      }
   }
   /* PackageManager publishes device paths, not the launcher's staging
    * files.  The filesystem bridge maps split_<name>.apk back to the staged
    * APK when the guest opens it.  Exposing the host path here also lets an
    * app derive a bogus native-library directory beside the XAPK container. */
   const char *install_dir = lunaria_android_apk_dir();
   for (i = 0; i < n; ++i) {
      char path[PATH_MAX];
      int written = snprintf(path, sizeof path, "%s/split_%.*s.apk",
                             install_dir, (int)e[i].nlen, e[i].name);
      if (written < 0 || (size_t)written >= sizeof path) {
         free(e); return 0;
      }
      nslot[i] = dvm_new_string_n(vm, e[i].name, e[i].nlen);
      dslot[i] = dvm_new_string(vm, path);
   }
   free(e);
   *out_names = names;
   *out_dirs  = dirs;
   return n;
}

static void dvm_fill_application_info_paths(struct dvm *vm, dvm_ref ai)
{
   union dvm_value v;
   v.l = dvm_new_string(vm, lunaria_android_apk_path());
   (void)dvm_set_field(vm, ai, "sourceDir", "Ljava/lang/String;", v);
   (void)dvm_set_field(vm, ai, "publicSourceDir", "Ljava/lang/String;", v);
   v.l = dvm_new_string(vm, lunaria_android_data_path());
   (void)dvm_set_field(vm, ai, "dataDir", "Ljava/lang/String;", v);
   v.l = dvm_new_string(vm, lunaria_android_native_lib_path());
   (void)dvm_set_field(vm, ai, "nativeLibraryDir", "Ljava/lang/String;", v);

   dvm_ref names = 0, dirs = 0;
   if (!dvm_build_split_arrays(vm, &names, &dirs)) return;
   v.l = names;
   (void)dvm_set_field(vm, ai, "splitNames", "[Ljava/lang/String;", v);
   v.l = dirs;
   (void)dvm_set_field(vm, ai, "splitSourceDirs", "[Ljava/lang/String;", v);
   (void)dvm_set_field(vm, ai, "splitPublicSourceDirs", "[Ljava/lang/String;", v);
}

/* Is this PackageManager query about the app that is running?
 *
 * A device answers getPackageInfo()/getApplicationInfo() for a package it does
 * not have with NameNotFoundException.  The emulator used to answer *every*
 * name with the running app's own record, so a library probing for another
 * package found one — with the wrong signature and the wrong version.  That is
 * a worse answer than "not installed": Google's client library reads it as
 * SERVICE_INVALID ("requires Google Play services, but their signature is
 * invalid") instead of SERVICE_MISSING, and an SDK that has a documented
 * no-Play-services path never takes it. */
static bool pm_query_package(struct dvm *vm, const union dvm_value *args,
                             int nargs, struct lunaria_android_package *record)
{
   const char *want = NULL;
   if (nargs >= 1 && args[0].l) want = dvm_string_utf8(vm, args[0].l);
   if (!want || !*want) want = getenv("ANDROID_PACKAGE_NAME");
   if (!want || !lunaria_android_package_find(want, record)) return false;
   /* A package the caller cannot see is, to that caller, not installed --
      the same NameNotFoundException a device raises. */
   return lunaria_android_package_visible(record) != 0;
}

static void dvm_fill_registered_application_info(
   struct dvm *vm, dvm_ref ai, const struct lunaria_android_package *record)
{
   union dvm_value v = { .l = dvm_new_string(vm, record->name) };
   (void)dvm_set_field(vm, ai, "packageName", "Ljava/lang/String;", v);
   v.i = record->target_sdk;
   (void)dvm_set_field(vm, ai, "targetSdkVersion", "I", v);
   v.i = record->uid;
   (void)dvm_set_field(vm, ai, "uid", "I", v);
   v.i = record->flags;
   (void)dvm_set_field(vm, ai, "flags", "I", v);
   v.l = dvm_new_string(vm, record->source_dir);
   (void)dvm_set_field(vm, ai, "sourceDir", "Ljava/lang/String;", v);
   (void)dvm_set_field(vm, ai, "publicSourceDir", "Ljava/lang/String;", v);
   v.l = dvm_new_string(vm, record->data_dir);
   (void)dvm_set_field(vm, ai, "dataDir", "Ljava/lang/String;", v);
   v.l = dvm_new_string(vm, record->lib_dir ? record->lib_dir : "");
   (void)dvm_set_field(vm, ai, "nativeLibraryDir", "Ljava/lang/String;", v);
}

static bool hook_call_external_inner(void *user, struct dvm *vm, const char *class_name,
                                     const char *method, const char *sig, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out);

/* A call from bytecode into the host's Java stub layer converts every argument
 * into a host handle (an Object[] becomes a host array with a handle per element,
 * a boxed Long a host Long).  Those handles are locals of this call: nothing
 * released them, so a loop that logs or formats a value (Long in an Object[]) filled
 * the 65,536-entry object table in a few minutes of play and the process aborted
 * ("jvm object limit reached").  Collect them in a bridge frame and release them when the call
 * returns, exactly as a call into guest native code does (hook_call_native). A stub
 * that keeps an argument beyond the call has to hold its own global reference, which
 * bridge_end() honours. */
static bool hook_call_external(void *user, struct dvm *vm, const char *class_name,
                               const char *method, const char *sig, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   JNIEnv *env = current_env();
   if (!env)
      return hook_call_external_inner(user, vm, class_name, method, sig, self,
                                      args, nargs, out);
   struct bridge_frame frame = { .prev = g_bridge_frame };
   g_bridge_frame = &frame;
   const bool ok = hook_call_external_inner(user, vm, class_name, method, sig,
                                            self, args, nargs, out);
   bridge_end(vm, env, &frame);
   return ok;
}

static bool hook_call_external_inner(void *user, struct dvm *vm, const char *class_name,
                                     const char *method, const char *sig, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)user;
   /* Activity.runOnUiThread(Runnable) posts to Android's main Looper.  Lunaria
    * has one cooperative host thread, so running the bytecode callback inline
    * is the faithful ordering point and preserves the actual anonymous
    * Runnable object (converting it to an opaque JNI stub loses its run()
    * method).  A null Runnable is a documented no-op — still consume the call
    * so call_out does not log it as unresolved. */
   if (!strcmp(method, "runOnUiThread")) {
      memset(out, 0, sizeof *out);
      if (nargs > 0 && args[0].l) {
         /* From a thread of its own, this has to become a post: the whole
          * point of the call is "not on my thread, on the UI one", and an SDK
          * that uses it to put a dialog up checks that it ended up there.
          * Running it inline was right while the VM had a single thread and
          * every caller already was the main one; with bytecode on host
          * threads it would run the UI work on the caller's thread instead. */
         if (!dvm_on_main_thread()) {
            (void)dvm__queue_runnable_at(vm, args[0].l, false, 0);
            return true;
         }
         struct dvm_class *rc = dvm_object_class(vm, args[0].l);
         struct dvm_method *run = rc ? dvm_find_method(vm, rc, "run", "()V") : NULL;
         if (run) {
            union dvm_value ignored = { 0 };
            (void)dvm_call(vm, run, args[0].l, NULL, 0, &ignored);
         } else {
            union dvm_value ignored = { 0 };
            struct dvm_class *iface =
               dvm__class_by_desc(vm, "Ljava/lang/Runnable;");
            (void)dvm_proxy_try_invoke(vm, args[0].l, iface, "run", "()V",
                                       NULL, 0, &ignored);
         }
      }
      /* Always consume the call: a failed Runnable must not look like a
       * missing Activity.runOnUiThread stub (note_missing). */
      return true;
   }
   /* Broadcast receivers registered at run time are the VM's own objects,
    * and delivery calls back into bytecode; the registry lives with them. */
   if (dvm_runtime_context_broadcast(vm, method, sig, self, args, nargs, out))
      return true;

   JNIEnv *env = current_env();
   if (!env) return false;

   /* Display.getMode() / getSupportedModes().  Android guarantees at least the
    * mode the display is currently in; the stub layer models a Display as an
    * opaque object, so both came back null and UE's
    * AndroidThunkJava_GetSupportedNativeDisplayRefreshRates NPE'd on
    * modes.length — the engine then believed the device supports no refresh
    * rate at all.  Build the one mode the emulator presents, taking its
    * geometry and rate from the very stubs that answer getWidth/getHeight/
    * getRefreshRate on the same Display. */
   /* PackageManager.getApplicationInfo(pkg, flags).  Callers reach the
    * manifest's meta-data through the returned ApplicationInfo's `metaData`
    * field, which only works if the object is one the VM owns — a host stub
    * handle has no dvm fields to read.  Build it here so the iget lands on a
    * real Bundle. */
   if (!strcmp(method, "getApplicationInfo") &&
       (strstr(class_name, "PackageManager") || strstr(class_name, "Context"))) {
      memset(out, 0, sizeof *out);
      struct lunaria_android_package record;
      if (!pm_query_package(vm, args, nargs, &record)) {
         /* Handled — the pending exception is the answer, so do not fall
          * through to the "no such external method" path. */
         dvm__throw(vm, "android/content/pm/PackageManager$NameNotFoundException",
                    "%s", args[0].l ? dvm_string_utf8(vm, args[0].l) : "");
         return true;
      }
      struct dvm_class *ac =
         dvm_find_class(vm, "android/content/pm/ApplicationInfo");
      dvm_ref ai = ac ? dvm_new_object(vm, ac) : 0;
      if (!ai) return false;
      const char *meta_pkg = getenv("ANDROID_PACKAGE_NAME");
      union dvm_value v = { .l = meta_pkg && !strcmp(meta_pkg, record.name)
                               ? dvm_runtime_manifest_bundle(vm) : 0 };
      (void)dvm_set_field(vm, ai, "metaData", "Landroid/os/Bundle;", v);
      dvm_fill_registered_application_info(vm, ai, &record);
      const char *pkg = getenv("ANDROID_PACKAGE_NAME");
      if (pkg && !strcmp(pkg, record.name)) dvm_fill_application_info_paths(vm, ai);
      dvm_pin(vm, ai);
      out->l = ai;
      return true;
   }

   if (!strcmp(method, "getPackageInfo") && strstr(class_name, "PackageManager")) {
      memset(out, 0, sizeof *out);
      struct lunaria_android_package record;
      if (!pm_query_package(vm, args, nargs, &record)) {
         /* Handled — the pending exception is the answer, so do not fall
          * through to the "no such external method" path. */
         dvm__throw(vm, "android/content/pm/PackageManager$NameNotFoundException",
                    "%s", args[0].l ? dvm_string_utf8(vm, args[0].l) : "");
         return true;
      }
      struct dvm_class *pc = dvm_find_class(vm, "android/content/pm/PackageInfo");
      struct dvm_class *ac = dvm_find_class(vm, "android/content/pm/ApplicationInfo");
      dvm_ref pi = pc ? dvm_new_object(vm, pc) : 0;
      dvm_ref ai = ac ? dvm_new_object(vm, ac) : 0;
      if (!pi || !ai) return false;
      const char *self_pkg = getenv("ANDROID_PACKAGE_NAME");
      if (self_pkg && !strcmp(self_pkg, record.name))
         dvm_fill_application_info_paths(vm, ai);
      dvm_fill_registered_application_info(vm, ai, &record);
      union dvm_value v = { 0 };
      /* Same ApplicationInfo the getApplicationInfo path builds: manifest
       * meta-data is read straight off pi.applicationInfo.metaData. */
      {
         union dvm_value bv = { .l = self_pkg && !strcmp(self_pkg, record.name)
                                   ? dvm_runtime_manifest_bundle(vm) : 0 };
         if (bv.l) (void)dvm_set_field(vm, ai, "metaData", "Landroid/os/Bundle;", bv);
      }
      /* Play Core's SplitInstallInfoProvider reads the installed split set off
       * PackageInfo, not off ApplicationInfo. */
      {
         dvm_ref sn = 0, sd = 0;
         if (dvm_build_split_arrays(vm, &sn, &sd)) {
            union dvm_value sv = { .l = sn };
            (void)dvm_set_field(vm, pi, "splitNames", "[Ljava/lang/String;", sv);
         }
      }
      v.l = dvm_new_string(vm, record.name);
      (void)dvm_set_field(vm, ai, "packageName", "Ljava/lang/String;", v);
      (void)dvm_set_field(vm, pi, "packageName", "Ljava/lang/String;", v);
      v.l = ai;
      (void)dvm_set_field(vm, pi, "applicationInfo",
                          "Landroid/content/pm/ApplicationInfo;", v);
      /* Straight from the manifest.  versionName was left unset, which reads
       * back as null — a value the framework never produces, so callers push it
       * on unchecked (UE's processSystemInfo() puts it in a map and later
       * CRCs every value, and died on String.getBytes of null). */
      int32_t vcode = 1;
      const char *vname = NULL;
      if (self_pkg && !strcmp(self_pkg, record.name))
         arm_exec_apk_version(&vcode, &vname);
      v.i = vcode;
      (void)dvm_set_field(vm, pi, "versionCode", "I", v);
      v.l = vname ? dvm_new_string(vm, vname) : 0;
      (void)dvm_set_field(vm, pi, "versionName", "Ljava/lang/String;", v);
      if (self_pkg && !strcmp(self_pkg, record.name))
         (void)dvm_package_info_add_signatures(vm, pi,
                                               nargs > 1 ? (uint32_t)args[1].i : 0);
      dvm_pin(vm, pi);
      out->l = pi;
      return true;
   }

   /* Context.getSystemService(name).  Services used by bytecode must be VM
    * objects: keeping them as opaque JNI wrappers makes iput/iget state and
    * virtual dispatch disappear when the object is stored in an app field. */
   if (!strcmp(method, "getSystemService") && nargs >= 1 && args[0].l) {
      struct dvm_object *class_arg = dvm__obj(vm, args[0].l);
      const char *want = (class_arg && class_arg->kind == DVM_OBJ_CLASS &&
                          class_arg->klass) ? class_arg->klass->name
                                            : dvm_string_utf8(vm, args[0].l);
      dvm_ref service = dvm_runtime_system_service(vm, want, self);
      if (service) {
         memset(out, 0, sizeof *out);
         out->l = service;
         return true;
      }
   }

   /* Context.getSharedPreferences(name, mode).  The host stub answered with a
    * handle that has no storage behind it, so every read came back null and
    * the SDKs rebuilt their state from nothing on each call.  The VM owns the
    * store, keyed by file name, so two lookups of the same name see each
    * other's writes. */
   if (!strcmp(method, "getSharedPreferences") && nargs >= 1) {
      const char *name = args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
      memset(out, 0, sizeof *out);
      out->l = dvm_runtime_shared_prefs(vm, name ? name : "");
      if (out->l) dvm_pin(vm, out->l);
      return out->l != 0;
   }

   /* PreferenceManager.getDefaultSharedPreferences(context) names its file
    * after the package, which is the one convention app code relies on. */
   if (!strcmp(method, "getDefaultSharedPreferences")) {
      const char *pkg = getenv("ANDROID_PACKAGE_NAME");
      char name[256];
      snprintf(name, sizeof name, "%s_preferences", pkg ? pkg : "app");
      memset(out, 0, sizeof *out);
      out->l = dvm_runtime_shared_prefs(vm, name);
      if (out->l) dvm_pin(vm, out->l);
      return out->l != 0;
   }

   /* InputDevice.getDeviceIds() is never null on Android — it is an int[] of
    * the currently attached devices.  Returning null made
    * AndroidThunkJava_IsGamepadAttached NPE on the array length once per frame.
    * Lunaria models no InputDevice at all (touch is injected directly), so the
    * truthful answer is an empty list rather than a fabricated device. */
   if (!strcmp(class_name, "android/view/InputDevice") &&
       !strcmp(method, "getDeviceIds")) {
      memset(out, 0, sizeof *out);
      out->l = dvm_new_array(vm, 'I', "I", 0);
      return true;
   }

   /* DisplayManager.register/unregisterDisplayListener.  The emulator presents
    * a single display whose one mode (see getSupportedModes below) never
    * changes, and a display that never appears, disappears or reconfigures
    * itself delivers no callbacks — so accepting the registration is the whole
    * of the work here.  Reporting the method as missing instead is what is
    * wrong: SwappyDisplayManager calls it from startListening(), and an
    * unresolved call there reads as "this device has no display service".
    *
    * Delivering one onDisplayChanged from here — which is what a device does
    * during startup, and what SwappyDisplayManager's only path to the display's
    * *current* refresh period needs — was tried and taken back out: this title
    * never reaches startListening() at all (its Swappy takes the NDK
    * choreographer path on SDK 31), so it bought nothing, and running guest
    * bytecode synchronously from inside a JNI stub gave two runs that hung in
    * the first forty seconds with the pump holding a guest mutex.  If it is
    * needed again it belongs on the Handler the caller passes, posted, not
    * called inline. */
   if (!strcmp(class_name, "android/hardware/display/DisplayManager") &&
       (!strcmp(method, "registerDisplayListener") ||
        !strcmp(method, "unregisterDisplayListener"))) {
      memset(out, 0, sizeof *out);
      return true;
   }

   if (!strcmp(class_name, "android/view/Display") &&
       (!strcmp(method, "getMode") || !strcmp(method, "getSupportedModes"))) {
      memset(out, 0, sizeof *out);
      struct dvm_class *mc = dvm_find_class(vm, "android/view/Display$Mode");
      dvm_ref mode = mc ? dvm_new_object(vm, mc) : 0;
      if (!mode) return false;

      jobject disp = to_jobject(vm, env, self);
      jclass dc = (*env)->FindClass(env, class_name);
      struct { const char *jm, *js, *field, *ft; } probe[] = {
         { "getWidth",       "()I", "physicalWidth",  "I" },
         { "getHeight",      "()I", "physicalHeight", "I" },
         { "getRefreshRate", "()F", "refreshRate",    "F" },
      };
      for (size_t i = 0; i < sizeof probe / sizeof probe[0]; ++i) {
         jmethodID mid = dc ? (*env)->GetMethodID(env, dc, probe[i].jm,
                                                 probe[i].js) : NULL;
         union dvm_value v = { 0 };
         if (mid) {
            if (probe[i].ft[0] == 'F')
               v.f = (*env)->CallFloatMethod(env, disp, mid);
            else
               v.i = (*env)->CallIntMethod(env, disp, mid);
         }
         (void)dvm_set_field(vm, mode, probe[i].field, probe[i].ft, v);
      }
      union dvm_value id = { .i = 1 };   /* mode ids are 1-based on Android */
      (void)dvm_set_field(vm, mode, "modeId", "I", id);

      if (!strcmp(method, "getMode")) {
         out->l = mode;
         return true;
      }
      dvm_ref arr = dvm_new_array(vm, 'L', "Landroid/view/Display$Mode;", 1);
      dvm_ref *slots = arr ? dvm_array_data(vm, arr) : NULL;
      if (slots) slots[0] = mode;
      out->l = arr;
      return true;
   }

   jclass cls = (*env)->FindClass(env, class_name);
   if (!cls) return false;

   bool is_static = (self == 0);
   jmethodID mid = is_static ? (*env)->GetStaticMethodID(env, cls, method, sig)
                             : (*env)->GetMethodID(env, cls, method, sig);
   if (!mid) return false;

   /* GetMethodID answers for any name at all — the stub layer builds a method
    * id from the strings, it does not look anything up.  Calling through one
    * that has no stub behind it returns zero and reports success, which is
    * exactly the silent failure this module exists to remove: the caller
    * cannot tell it from a real zero, and nothing says the method is missing.
    * Decline instead, so dvm__call_out names it in the unresolved list (and
    * still yields zero, as it always did). */
   if (!jvm_method_has_stub(env, mid)) return false;

   jvalue jargs[64];
   memset(jargs, 0, sizeof jargs);
   for (int i = 0; i < nargs && i < 64; ++i) {
      char one[256];
      if (!dvm__sig_param(sig, i, one, sizeof one)) break;
      jargs[i] = dvm_to_jvalue(vm, env, dvm__kind_of(one), args[i]);
   }

   jobject jself = self ? to_jobject(vm, env, self) : NULL;
   char ret = dvm_sig_return_kind(sig);
   ++g_external_calls;

   memset(out, 0, sizeof *out);
   switch (ret) {
      case 'V':
         if (is_static) (*env)->CallStaticVoidMethodA(env, cls, mid, jargs);
         else (*env)->CallVoidMethodA(env, jself, mid, jargs);
         break;
      case 'Z': case 'B': case 'C': case 'S': case 'I':
         out->i = is_static ? (*env)->CallStaticIntMethodA(env, cls, mid, jargs)
                            : (*env)->CallIntMethodA(env, jself, mid, jargs);
         break;
      case 'J':
         out->j = is_static ? (*env)->CallStaticLongMethodA(env, cls, mid, jargs)
                            : (*env)->CallLongMethodA(env, jself, mid, jargs);
         break;
      case 'F':
         out->f = is_static ? (*env)->CallStaticFloatMethodA(env, cls, mid, jargs)
                            : (*env)->CallFloatMethodA(env, jself, mid, jargs);
         break;
      case 'D':
         out->d = is_static ? (*env)->CallStaticDoubleMethodA(env, cls, mid, jargs)
                            : (*env)->CallDoubleMethodA(env, jself, mid, jargs);
         break;
      default: {
         jobject r = is_static ? (*env)->CallStaticObjectMethodA(env, cls, mid, jargs)
                               : (*env)->CallObjectMethodA(env, jself, mid, jargs);
         out->l = from_jobject(vm, env, r);
         break;
      }
   }
   return true;
}

static dvm_ref hook_new_external(void *user, struct dvm *vm, const char *class_name)
{
   (void)user;
   JNIEnv *env = current_env();
   if (!env) return 0;
   jclass cls = (*env)->FindClass(env, class_name);
   if (!cls) return 0;
   jobject o = (*env)->AllocObject(env, cls);
   if (!o) return 0;
   return wrapper_for(vm, env, class_name, (uint32_t)(uintptr_t)o);
}

static bool hook_get_external_static(void *user, struct dvm *vm, const char *class_name,
                                     const char *field, const char *type,
                                     union dvm_value *out)
{
   (void)user;
   JNIEnv *env = current_env();
   if (!env || !type) return false;
   /* UnityPlayer.currentActivity is a dex field, but the host publishes the
    * process Activity through jni_set_current_activity().  Prefer that over
    * a null sslot / recursive GetStaticObjectField. */
   if (field && class_name && !strcmp(field, "currentActivity") &&
       strstr(class_name, "UnityPlayer")) {
      jobject a = jni_get_current_activity();
      if (!a) return false;
      memset(out, 0, sizeof *out);
      out->l = from_jobject(vm, env, a);
      return out->l != 0;
   }
   /* A static field of a class the device does not have is not a null, it is
    * a NoClassDefFoundError — the stub layer would answer any name at all.
    * dvm.c raises it when this hook declines. */
   if (!dvm_class_exists(vm, class_name)) return false;
   jclass cls = (*env)->FindClass(env, class_name);
   if (!cls) return false;
   jfieldID fid = (*env)->GetStaticFieldID(env, cls, field, type);
   if (!fid) return false;

   memset(out, 0, sizeof *out);
   switch (dvm__kind_of(type)) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
         out->i = (*env)->GetStaticIntField(env, cls, fid);
         break;
      case 'J': out->j = (*env)->GetStaticLongField(env, cls, fid); break;
      case 'F': out->f = (*env)->GetStaticFloatField(env, cls, fid); break;
      case 'D': out->d = (*env)->GetStaticDoubleField(env, cls, fid); break;
      default:
         out->l = from_jobject(vm, env, (*env)->GetStaticObjectField(env, cls, fid));
         break;
   }
   return true;
}

/* NativeActivity.onCreate/onStart/onResume.  The guest runs without the
 * interpreter lock and re-enters Java through the ordinary JNI path, exactly
 * as for any other native call. */
static bool hook_native_activity(void *user, struct dvm *vm, int stage,
                                 dvm_ref activity)
{
   (void)user;
   JNIEnv *env = current_env();
   if (!g_guest_activity || !env) return false;
   struct bridge_frame frame = { .prev = g_bridge_frame };
   g_bridge_frame = &frame;
   jobject self = to_jobject(vm, env, activity);
   unsigned gil = dvm_gil_unlock_all(vm);
   bool done = g_guest_activity(stage, self);
   dvm_gil_relock(vm, gil);
   bridge_end(vm, env, &frame);
   return done;
}

static bool hook_call_native(void *user, struct dvm *vm, const char *class_name,
                             const char *method, const char *sig, bool is_static,
                             dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)user;
   if (!g_guest_native || !current_env()) return false;

   JNIEnv *env = current_env();
   struct bridge_frame frame = { .prev = g_bridge_frame };
   g_bridge_frame = &frame;

   jvalue jargs[64];
   memset(jargs, 0, sizeof jargs);
   for (int i = 0; i < nargs && i < 64; ++i) {
      char one[256];
      if (!dvm__sig_param(sig, i, one, sizeof one)) break;
      jargs[i] = dvm_to_jvalue(vm, env, dvm__kind_of(one), args[i]);
   }

   jvalue ret;
   memset(&ret, 0, sizeof ret);
   jobject jself = self ? to_jobject(vm, env, self) : NULL;
   if (getenv("LUNARIA_TOUCH_DIAG") && !strcmp(method, "nativeInjectEvent") && nargs) {
      lunaria_touch_event event = {0};
      bool valid = jvm_motion_event_read(jnienv_get_jvm(env), jargs[0].l, &event);
      fprintf(stderr, "[motion-jni] arg=%p valid=%d action=%d xy=%.1f,%.1f time=%lld down=%lld\n",
              jargs[0].l, valid, event.action, event.x, event.y,
              event.event_ms, event.down_ms);
   }
   /* VM heap and wrapper tables are protected by GIL. Guest code executes
    * without it and re-enters Java through the ordinary JNI acquisition path.
    * Restore it before reading back buffers, converting the result or releasing
    * bridge references; other Java threads can mutate those tables meanwhile. */
   unsigned gil = dvm_gil_unlock_all(vm);
   bool called = g_guest_native(class_name, method, sig, is_static, jself,
                                 jargs, nargs, &ret);
   dvm_gil_relock(vm, gil);
   if (!called) {
      bridge_end(vm, env, &frame);
      return false;
   }
   if (getenv("LUNARIA_TOUCH_DIAG") && !strcmp(method, "nativeInjectEvent"))
      fprintf(stderr, "[motion-jni] nativeInjectEvent returned %d\n", ret.z);
   /* The native may have written into a direct buffer's guest region; the
    * bytes have to be back in the VM's array before bytecode reads them
    * again (UnityWebRequest's upload loop reads array() on the next line). */
   for (int i = 0; i < nargs && i < 64; ++i)
      if (args[i].l && dvm_buffer_is_direct(vm, args[i].l))
         dvm_direct_buffer_from_host(vm, args[i].l);


   ++g_native_calls;
   memset(out, 0, sizeof *out);
   switch (dvm_sig_return_kind(sig)) {
      case 'V': break;
      case 'Z': out->i = ret.z ? 1 : 0; break;
      case 'B': out->i = ret.b; break;
      case 'C': out->i = (uint16_t)ret.c; break;
      case 'S': out->i = ret.s; break;
      case 'I': out->i = ret.i; break;
      case 'J': out->j = ret.j; break;
      case 'F': out->f = ret.f; break;
      case 'D': out->d = ret.d; break;
      default:
         out->l = from_jobject(vm, env, ret.l);
         /* Native return promotion gives this bridge one local reference.
          * Wrappers retain their own global; strings/primitive arrays copy. */
         if (ret.l) (*env)->DeleteLocalRef(env,ret.l);
         break;
   }
   bridge_end(vm, env, &frame);
   return true;
}

/* A native method returned with an exception pending: JNI raises it in the
 * caller.  An exception that began as a VM object (native code caught what a
 * Java call threw and handed it back with Throw) is that same object; one the
 * native made itself (ThrowNew) is built here from its class and message. */
static dvm_ref hook_take_pending_exception(void *user, struct dvm *vm)
{
   (void)user;
   JNIEnv *env = current_env();
   if (!env) return 0;
   jthrowable object = NULL;
   char cls[128], msg[256];
   if (!jvm_take_pending_exception(jnienv_get_jvm(env), &object, cls, sizeof cls,
                                   msg, sizeof msg))
      return 0;
   dvm_ref ex = object ? find_wrapper((uint32_t)(uintptr_t)object) : 0;
   if (object) (*env)->DeleteLocalRef(env,object);
   if (ex) return ex;
   dvm__throw(vm, cls[0] ? cls : "java/lang/RuntimeException", "%s", msg);
   ex = vm->exception;
   dvm_clear_exception(vm);
   return ex;
}

/* ------------------------------------------------------------------------ *
 * UE Java package
 * ------------------------------------------------------------------------ */

/* Epic ships the same GameActivity.java under two packages — com.epicgames.ue4
 * in UE4 and com.epicgames.unreal from UE5 on — and the class is unchanged
 * apart from its package.  Every place below that needs the activity must find
 * whichever one the APK actually carries; pinning one package silently skips
 * the binding on the other and the thunks then run with a null receiver. */
static const char *const g_ue_pkgs[] = {
   "com/epicgames/unreal",
   "com/epicgames/ue4",
};

/* JNI-form class name of the loaded GameActivity, or NULL when the APK has
 * neither (non-UE title).  Filled in on first use; the dex set is fixed once
 * the VM exists. */
static const char *ue_activity_class(struct dvm *vm)
{
   static const char *cached;
   static bool looked;
   if (looked) return cached;
   looked = true;
   for (size_t i = 0; i < sizeof g_ue_pkgs / sizeof g_ue_pkgs[0]; ++i) {
      static char buf[64];
      snprintf(buf, sizeof buf, "%s/GameActivity", g_ue_pkgs[i]);
      if (dvm_find_class(vm, buf)) {
         cached = buf;
         break;
      }
   }
   return cached;
}

/* Field descriptor for `class_name` (dotted or JNI form), optionally for one of
 * its nested classes: ("com.epicgames.unreal.GameActivity", "EAlertDialogType")
 * → "Lcom/epicgames/unreal/GameActivity$EAlertDialogType;". */
static void ue_descriptor(const char *class_name, const char *inner,
                          char *out, size_t cap)
{
   size_t o = 0;
   if (cap) out[o++] = 'L';
   for (const char *p = class_name; *p && o + 1 < cap; ++p)
      out[o++] = (*p == '.') ? '/' : *p;
   if (inner) {
      if (o + 1 < cap) out[o++] = '$';
      for (const char *p = inner; *p && o + 1 < cap; ++p) out[o++] = *p;
   }
   if (o + 1 < cap) out[o++] = ';';
   out[o < cap ? o : cap - 1] = '\0';
}

/* ------------------------------------------------------------------------ *
 * Set-up
 * ------------------------------------------------------------------------ */

/* Volume/Battery/HeadsetReceiver.startReceiver(Activity) ends in
 * activity.registerReceiver(...).  The native-activity loader never runs the
 * real Activity lifecycle that would guarantee a non-null Activity at every
 * call site, so a null argument is an emulator gap.  Prefer
 * GameActivity._activity; otherwise no-op (host has no broadcast delivery). */
static bool
builtin_ue_start_receiver(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self;
   memset(out, 0, sizeof *out);
   dvm_ref activity = (nargs > 0) ? args[0].l : 0;
   if (!activity) {
      const char *aname = ue_activity_class(vm);
      struct dvm_class *ga = aname ? dvm_find_class(vm, aname) : NULL;
      union dvm_value cur = { 0 };
      char desc[80];
      if (ga) {
         ue_descriptor(aname, NULL, desc, sizeof desc);
         if (dvm_get_static(vm, ga, "_activity", desc, &cur))
            activity = cur.l;
      }
   }
   if (!activity)
      return true;
   /* Framework registerReceiver is host-stubbed; invoke it so the dex path
    * still exercises the same call shape as a real device. */
   struct dvm_class *acls = dvm_object_class(vm, activity);
   struct dvm_method *reg = acls
      ? dvm_find_method(vm, acls, "registerReceiver", NULL) : NULL;
   if (!reg && acls && acls->super)
      reg = dvm_find_method(vm, acls->super, "registerReceiver", NULL);
   if (!reg)
      return true;
   union dvm_value rr_args[2] = { { .l = 0 }, { .l = 0 } };
   union dvm_value ignored = { 0 };
   (void)dvm_call(vm, reg, activity, rr_args, 2, &ignored);
   dvm_clear_exception(vm);
   return true;
}

static struct dvm *vm_get(void)
{
   if (g_tried) return g_vm;
   g_tried = true;

   if (dvm_jni_mode() == DVM_JNI_OFF) return NULL;

   const char *dir = getenv("ANDROID_PACKAGE_CODE_PATH");
   if (!dir || !*dir) {
      fprintf(stderr, "[dvm] no ANDROID_PACKAGE_CODE_PATH; bytecode emulation is off\n");
      return NULL;
   }

   struct dvm_hooks hooks = {
      .call_external = hook_call_external,
      .new_external = hook_new_external,
      .get_external_static = hook_get_external_static,
      .call_native = hook_call_native,
      .take_pending_exception = hook_take_pending_exception,
      .load_library = hook_load_library,
      .native_activity = hook_native_activity,
   };
   g_vm = dvm_create(&hooks);
   if (!g_vm) return NULL;

   int n = dvm_add_apk_dir(g_vm, dir);
   if (!n) {
      fprintf(stderr, "[dvm] no classes*.dex under %s; bytecode emulation is off\n", dir);
      dvm_destroy(g_vm);
      g_vm = NULL;
      return NULL;
   }
   {
      static const char *recv[] = {
         "VolumeReceiver", "BatteryReceiver", "HeadsetReceiver",
      };
      for (size_t p = 0; p < sizeof g_ue_pkgs / sizeof g_ue_pkgs[0]; ++p) {
         for (size_t i = 0; i < sizeof recv / sizeof recv[0]; ++i) {
            char cls[80];
            snprintf(cls, sizeof cls, "%s/%s", g_ue_pkgs[p], recv[i]);
            struct dvm_method *m = dvm_lookup(
               g_vm, cls, "startReceiver", "(Landroid/app/Activity;)V");
            if (m) m->builtin = builtin_ue_start_receiver;
         }
      }
   }
   fprintf(stderr, "[dvm] bytecode emulator ready (%d dex, mode %d)\n", n, (int)g_mode);
   return g_vm;
}

/* ------------------------------------------------------------------------ *
 * Entry points used by jvm.c
 * ------------------------------------------------------------------------ */

static struct dvm_method *find(struct dvm *vm, const char *class_name,
                               const char *method, const char *sig)
{
   struct dvm_method *m = dvm_lookup(vm, class_name, method, sig);
   if (m) return m;
   /* The stub layer forms method IDs without always keeping the signature
    * (jvm.c's symbol form drops it), so fall back to the name alone. */
   return dvm_lookup(vm, class_name, method, NULL);
}

bool dvm_jni_invoke_locked(JNIEnv *env, const char *class_name, const char *method,
                    const char *sig, jobject self, bool is_static,
                    va_list *ap, const jvalue *jargs, jvalue *out)
{
   struct dvm *vm = vm_get();
   if (!vm || !class_name || !method) return false;

   struct dvm_method *m = find(vm, class_name, method, sig);
   /* A `native` method the dex declares is still the dex's method: a JNI call
    * to it initialises its class first (JNI spec, Call<Type>Method* and
    * GetStaticMethodID) and then runs whatever RegisterNatives or the
    * Java_… symbol bound.  dvm_call() does both.  Handing it to the stub layer
    * skipped <clinit> — a class whose initialiser is
    * `System.loadLibrary(...)` then never loaded its library, and the native
    * never ran. */
   if (!m || !(m->has_code || m->builtin || (m->access & DEX_ACC_NATIVE))) {
      /* Not in any dex: the caller falls back to its stub.  Worth seeing when
       * a title is not behaving, because it says exactly which Java the
       * emulator is *not* running. */
      if (getenv("LUNARIA_DVM_TRACE"))
         fprintf(stderr, "[dvm] miss %s.%s%s\n", class_name, method,
                 sig ? sig : "");
      return false;
   }

   const char *msig = m->sig ? m->sig : sig;
   int nargs = dvm_sig_arg_count(msig);
   if (nargs < 0) nargs = 0;
   if (nargs > 64) nargs = 64;

   JNIEnv *saved = g_env;
   g_env = env;
   if (env) g_env_any = env;

   union dvm_value args[64];
   jobject host_arrays[64];
   memset(args, 0, sizeof args);
   memset(host_arrays, 0, sizeof host_arrays);
   for (int i = 0; i < nargs; ++i) {
      char one[256];
      if (!dvm__sig_param(msig, i, one, sizeof one)) break;

      jvalue jv;
      if (ap) jv = va_next(ap, one);
      else if (jargs) jv = jargs[i];
      else memset(&jv, 0, sizeof jv);
      args[i] = jvalue_to_dvm(vm, env, one, jv);
      /* Remember array arguments so what the callee writes gets back to the
       * caller's buffer (see host_array_to_dvm). */
      if (one[0] == '[' && jv.l && args[i].l) host_arrays[i] = jv.l;
      /* An array parameter that arrives as null, or as something the VM could
       * not see as an array, makes the callee throw on its first .length —
       * far from here, and with nothing to say why. */
      if (one[0] == '[' && !host_arrays[i]) {
         static int arr_diag;
         if (arr_diag++ < 16)
            fprintf(stderr, "[dvm] %s.%s%s: array arg %d is %s (host 0x%x, class %s)\n",
                    class_name, method, msig, i, jv.l ? "unconvertible" : "null",
                    (unsigned)(uintptr_t)jv.l,
                    jv.l ? (class_name_of(env, jv.l) ?: "?") : "-");
      }
   }

   /* What a reflective helper was actually handed.  The signature string it
    * parses comes from native code across the stub layer's handle table, and
    * a wrong one there produces a failure inside the helper's own parser —
    * far from the call that supplied it. */
   if (getenv("LUNARIA_TRACE_CLASS_FLOW") &&
       strstr(class_name, "ReflectionHelper")) {
      fprintf(stderr, "[classflow] -> %s.%s%s", class_name, method, msig ? msig : "");
      for (int i = 0; i < nargs; ++i) {
         char one[256];
         if (!dvm__sig_param(msig, i, one, sizeof one)) break;
         if (!strcmp(one, "Ljava/lang/String;"))
            fprintf(stderr, " arg%d=\"%s\"", i,
                    args[i].l ? (dvm_string_utf8(vm, args[i].l) ?: "?") : "(null)");
         else if (!strcmp(one, "Ljava/lang/Class;")) {
            jvalue jv = jargs ? jargs[i] : (jvalue){ 0 };
            const char *described = jv.l
               ? jvm_described_class_name(jnienv_get_jvm(env), jv.l) : NULL;
            struct dvm_object *co = args[i].l ? dvm__obj(vm, args[i].l) : NULL;
            fprintf(stderr, " arg%d=class(%s of %s, host=%s)", i,
                    co && co->klass && co->klass->name
                       ? co->klass->name : "(no klass)",
                    co && co->cls && co->cls->name ? co->cls->name : "?",
                    described ? described : "(not a host class)");
         } else
            fprintf(stderr, " arg%d=%llx", i, (unsigned long long)args[i].j);
      }
      fprintf(stderr, "\n");
   }

   dvm_ref dself = 0;
   if (!is_static && self) {
      /* Receivers need the same conversion as arguments: strings and arrays
       * have VM payloads, while ordinary objects keep their host identity. */
      dself = from_jobject(vm, env, self);
   } else if (!is_static) {
      dself = dvm_new_object(vm, dvm_find_class(vm, class_name));
   }

   union dvm_value ret;
   ++g_calls;
   bool ok = dvm_call(vm, m, dself, args, nargs, &ret);
   if (ok && self && !strcmp(method, "<init>") &&
       (!strcmp(class_name, "java/lang/String") || !strcmp(class_name, "java.lang.String"))) {
      size_t bytes = dvm_string_utf8_length(vm, dself);
      const char *text = dvm_string_utf8(vm, dself);
      if (text) jvm_init_string_wtf8(jnienv_get_jvm(env), self, text, bytes);
   }
   for (int i = 0; i < nargs; ++i)
      if (host_arrays[i]) array_sync_back(vm, env, host_arrays[i], args[i].l);
   if (!ok) {
      char buf[512];
      dvm_describe_exception(vm, dvm_exception(vm), buf, sizeof buf);
      fprintf(stderr, "[dvm] %s.%s%s threw %s\n", class_name, method, msig, buf);
      jthrowable exception = (jthrowable)to_jobject(vm, env, dvm_exception(vm));
      dvm_clear_exception(vm);
      if (exception) (*env)->Throw(env, exception);
      memset(&ret, 0, sizeof ret);
   }

   if (out) *out = dvm_to_jvalue(vm, env, dvm_sig_return_kind(msig), ret);
   g_env = saved;
   return true;
}

void dvm_jni_show_activity(JNIEnv *env, jobject activity)
{
   struct dvm *vm = dvm_jni_vm();
   if (!vm || !env || !activity) return;
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   dvm_ref ref = from_jobject(vm, env, activity);
   if (ref) dvm__activity_visible(vm, ref);
   dvm_gil_leave_to_guest(vm, cookie);
}

/* Activity.finish() has been called on it (the activity stack tears it down
 * on the main thread; the flag is set at the call). */
bool dvm_jni_activity_finishing(JNIEnv *env, jobject activity)
{
   struct dvm *vm = dvm_jni_vm();
   if (!vm || !env || !activity) return false;
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   dvm_ref ref = from_jobject(vm, env, activity);
   union dvm_value v = { 0 };
   const bool r = ref && dvm_get_field(vm, ref, "finishing", "Z", &v) && v.i;
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

bool dvm_jni_invoke(JNIEnv *env, const char *class_name, const char *method,
                    const char *sig, jobject self, bool is_static,
                    va_list *ap, const jvalue *jargs, jvalue *out)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_jni_invoke_locked(env, class_name, method, sig, self, is_static, ap, jargs, out);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

bool dvm_jni_field_locked(JNIEnv *env, jobject obj, jfieldID field, bool set,
                   uint64_t *bits)
{
   struct dvm *vm = vm_get();
   if (!vm || dvm_jni_mode() == DVM_JNI_OFF || !env || !obj || !field || !bits)
      return false;

   const char *cls = NULL, *name = NULL, *type = NULL;
   if (!jvm_field_info(jnienv_get_jvm(env), field, &cls, &name, &type))
      return false;
   if (getenv("LUNARIA_TRACE_FIELDS")) {
      static int n;
      if (n++ < 200)
         fprintf(stderr, "[dvm] %s field %s.%s:%s (dex=%d)\n",
                 set ? "set" : "get", cls, name, type,
                 dvm_find_class(vm, cls) ? 1 : 0);
   }
   /* Only classes the dex defines: everything else keeps the stub layer's
    * own field storage. */
   if (!dvm_find_class(vm, cls)) return false;

   /* A field belongs to the DVM only when this host handle was produced from
    * a DVM object earlier.  Merely finding a dex/runtime class with the same
    * name is not ownership: Context.getApplicationInfo(), for example, is a
    * JVM-stub object whose fields are implemented by jni_stubs.c.  Wrapping
    * that unknown handle here manufactured an empty DVM ApplicationInfo and
    * changed a valid nativeLibraryDir into null before the stub layer could
    * answer. */
   dvm_ref self = find_wrapper((uint32_t)(uintptr_t)obj);
   if (!self) return false;
   if (getenv("LUNARIA_TRACE_FIELDS")) {
      static int n;
      if (n++ < 96)
         fprintf(stderr, "[dvm] field host 0x%x -> @%x\n",
                 (unsigned)(uintptr_t)obj, self);
   }

   if (set) {
      union dvm_value v = { 0 };
      switch (type[0]) {
         case 'Z': v.i = (*bits & 0xffu) ? 1 : 0; break;
         case 'B': v.i = (int8_t)*bits; break;
         case 'C': v.i = (uint16_t)*bits; break;
         case 'S': v.i = (int16_t)*bits; break;
         case 'I': v.i = (int32_t)*bits; break;
         case 'J': v.j = (int64_t)*bits; break;
         case 'F': { uint32_t u = (uint32_t)*bits; memcpy(&v.f, &u, 4); break; }
         case 'D': { uint64_t u = *bits; memcpy(&v.d, &u, 8); break; }
         default:  v.l = from_jobject(vm, env, (jobject)(uintptr_t)*bits); break;
      }
      return dvm_set_field(vm, self, name, type, v);
   }

   union dvm_value v = { 0 };
   if (!dvm_get_field(vm, self, name, type, &v)) return false;
   if (getenv("LUNARIA_TRACE_FIELDS") && type[0] == 'L') {
      static int n;
      if (n++ < 128)
         fprintf(stderr, "[dvm]   %s.%s on @%x = \"%s\"\n", cls, name, self,
                 v.l ? (dvm_string_utf8(vm, v.l) ?: "<obj>") : "<null>");
   }
   *bits = 0;
   switch (type[0]) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
         *bits = (uint32_t)v.i; break;
      case 'J': *bits = (uint64_t)v.j; break;
      case 'F': { uint32_t u; memcpy(&u, &v.f, 4); *bits = u; break; }
      case 'D': { uint64_t u; memcpy(&u, &v.d, 8); *bits = u; break; }
      default:  *bits = (uint64_t)(uintptr_t)to_jobject(vm, env, v.l); break;
   }
   return true;
}

bool dvm_jni_field(JNIEnv *env, jobject obj, jfieldID field, bool set,
                   uint64_t *bits)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_jni_field_locked(env, obj, field, set, bits);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

bool dvm_jni_static_field_locked(JNIEnv *env, jclass cls_ref, jfieldID field, bool set,
                          uint64_t *bits)
{
   struct dvm *vm = vm_get();
   if (!vm || dvm_jni_mode() == DVM_JNI_OFF || !env || !field || !bits)
      return false;

   const char *cls = NULL, *name = NULL, *type = NULL;
   if (!jvm_field_info(jnienv_get_jvm(env), field, &cls, &name, &type))
      return false;
   (void)cls_ref; /* the field id already names the class it belongs to */
   if (getenv("LUNARIA_TRACE_FIELDS")) {
      static int n;
      if (n++ < 200)
         fprintf(stderr, "[dvm] %s static field %s.%s:%s (dex=%d)\n",
                 set ? "set" : "get", cls, name, type,
                 dvm_find_class(vm, cls) ? 1 : 0);
   }
   struct dvm_class *k = dvm_find_class(vm, cls);
   if (!k) return false;   /* not ours: the stub layer keeps its own storage */

   if (set) {
      union dvm_value v = { 0 };
      switch (type[0]) {
         case 'Z': v.i = (*bits & 0xffu) ? 1 : 0; break;
         case 'B': v.i = (int8_t)*bits; break;
         case 'C': v.i = (uint16_t)*bits; break;
         case 'S': v.i = (int16_t)*bits; break;
         case 'I': v.i = (int32_t)*bits; break;
         case 'J': v.j = (int64_t)*bits; break;
         case 'F': { uint32_t u = (uint32_t)*bits; memcpy(&v.f, &u, 4); break; }
         case 'D': { uint64_t u = *bits; memcpy(&v.d, &u, 8); break; }
         default:  v.l = from_jobject(vm, env, (jobject)(uintptr_t)*bits); break;
      }
      return dvm_set_static(vm, k, name, type, v);
   }

   union dvm_value v = { 0 };
   if (!dvm_get_static(vm, k, name, type, &v)) return false;
   *bits = 0;
   switch (type[0]) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
         *bits = (uint32_t)v.i; break;
      case 'J': *bits = (uint64_t)v.j; break;
      case 'F': { uint32_t u; memcpy(&u, &v.f, 4); *bits = u; break; }
      case 'D': { uint64_t u; memcpy(&u, &v.d, 8); *bits = u; break; }
      default:  *bits = (uint64_t)(uintptr_t)to_jobject(vm, env, v.l); break;
   }
   return true;
}

bool dvm_jni_static_field(JNIEnv *env, jclass cls_ref, jfieldID field, bool set,
                          uint64_t *bits)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_jni_static_field_locked(env, cls_ref, field, set, bits);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

const char * dvm_jni_super_name_locked(const char *class_name)
{
   struct dvm *vm = vm_get();
   if (!vm || !class_name)
      return NULL;
   /* Only ask about classes the dex defines: for anything else the VM
    * synthesises an external class whose super is java/lang/Object, which is
    * not what the Android framework's hierarchy says. */
   if (!dvm_class_is_known(vm, class_name))
      return NULL;
   struct dvm_class *cls = dvm_find_class(vm, class_name);
   return cls ? dvm_class_super_name(cls) : NULL;
}

const char * dvm_jni_super_name(const char *class_name)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   const char * r = dvm_jni_super_name_locked(class_name);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

bool dvm_jni_class_assignable_locked(const char *sub, const char *sup)
{
   struct dvm *vm = vm_get();
   if (!vm || !sub || !sup)
      return false;
   /* Only answer for classes a dex actually defines.  For anything else the
    * VM synthesises an external class whose super is java/lang/Object and
    * whose interface list is empty, and class_assignable() would then report
    * "not assignable" for a relationship the framework really has. */
   if (!dvm_class_is_known(vm, sub) || !dvm_class_is_known(vm, sup))
      return false;
   struct dvm_class *a = dvm_find_class(vm, sub);
   struct dvm_class *b = dvm_find_class(vm, sup);
   if (!a || !b)
      return false;
   return dvm__class_assignable(vm, a, b);
}

bool dvm_jni_class_assignable(const char *sub, const char *sup)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_jni_class_assignable_locked(sub, sup);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

bool dvm_jni_class_in_dex_locked(const char *class_name)
{
   struct dvm *vm = vm_get();
   return vm && class_name && dvm_class_is_known(vm, class_name);
}

bool dvm_jni_class_in_dex(const char *class_name)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_jni_class_in_dex_locked(class_name);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

/* PackageManager.getPackageInfo() asked from native code.  The VM builds the
 * PackageInfo a device answers with — signing certificates, versions, splits,
 * permissions — and native code reads those off the handle with the same
 * Get<Type>Field calls bytecode would use as plain field reads.  Returns NULL
 * with NameNotFoundException pending when the package is not one this device
 * has, as the platform does. */
jobject dvm_jni_package_info(JNIEnv *env, jstring name, jint flags)
{
   struct dvm *vm = vm_get();
   if (!vm || !env) return NULL;
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   JNIEnv *saved = g_env;
   g_env = env;
   union dvm_value args[2] = { { .l = from_jobject(vm, env, name) },
                               { .i = flags } };
   union dvm_value out = { 0 };
   jobject result = NULL;
   /* No bridge frame: the handle returned is the caller's local reference, and
    * a frame would release it on the way out. */
   const bool handled = hook_call_external_inner(
      NULL, vm, "android/content/pm/PackageManager", "getPackageInfo",
      "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", 0, args, 2, &out);
   if (handled && !vm->exception && out.l)
      result = to_jobject(vm, env, out.l);
   if (vm->exception) {
      const char *what = args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
      char message[256];
      snprintf(message, sizeof message, "%s", what ? what : "");
      dvm_clear_exception(vm);
      jvm_throw_new(jnienv_get_jvm(env),
                    "android/content/pm/PackageManager$NameNotFoundException",
                    message);
   }
   g_env = saved;
   dvm_gil_leave_to_guest(vm, cookie);
   return result;
}

bool dvm_jni_class_exists(const char *class_name)
{
   struct dvm *vm = vm_get();
   if (!class_name) return false;
   /* Native-only modules have no DEX VM and use the host JNI class table.
    * Once an APK VM exists, its class loader remains authoritative. */
   if (!vm) return true;
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_class_exists(vm, class_name);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

void dvm_jni_shutdown(void)
{
   /* Do not initialize a VM just to shut the process down. All callbacks must
    * finish while their JNI table and guest engines are still alive. */
   if (!g_vm) return;
   dvm_request_stop(g_vm);
   dvm_threads_finish(g_vm);
   dvm_glsurface_finish(g_vm);
   dvm_nsd_finish(g_vm);
   dvm_prefs_finish(g_vm);
}

bool dvm_jni_method_in_dex_locked(const char *class_name, const char *method,
                           const char *sig)
{
   struct dvm *vm = vm_get();
   if (dvm_jni_mode() == DVM_JNI_OFF || !vm || !class_name || !method)
      return false;
   struct dvm_method *m = find(vm, class_name, method, sig);
   return m && m->has_code;
}

bool dvm_jni_method_in_dex(const char *class_name, const char *method,
                           const char *sig)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_jni_method_in_dex_locked(class_name, method, sig);
   dvm_gil_leave_to_guest(vm, cookie);
   return r;
}

bool dvm_jni_add_dex_memory_locked(const void *data, size_t len, const char *name)
{
   struct dvm *vm = vm_get();
   return vm && dvm_add_dex_memory(vm, data, len, name);
}

/* Bumped whenever a dex arrives.  Callers that memoise anything derived from
 * the class hierarchy — jvm_wrap_method()'s stub resolution walks it — compare
 * this and drop what they cached. */
static unsigned g_dex_epoch;
unsigned dvm_jni_dex_epoch(void) { return g_dex_epoch; }

bool dvm_jni_add_dex_memory(const void *data, size_t len, const char *name)
{
   struct dvm *vm = vm_get();
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   bool r = dvm_jni_add_dex_memory_locked(data, len, name);
   dvm_gil_leave_to_guest(vm, cookie);
   if (r) ++g_dex_epoch;
   return r;
}

void dvm_jni_report(void)
{
   if (!g_vm) return;
   fprintf(stderr, "[dvm] %u bytecode calls, %u out to stubs, %u out to guest natives, "
                   "%llu instructions\n",
           g_calls, g_external_calls, g_native_calls,
           (unsigned long long)dvm_instructions(g_vm));
}

struct dvm *dvm_jni_vm(void)
{
   return vm_get();
}

void dvm_jni_bind_unity_activity(JNIEnv *env, jobject activity)
{
   struct dvm *vm = vm_get();
   if (!vm || !env || !activity) return;
   unsigned cookie = dvm_gil_enter_from_guest(vm);
   struct dvm_class *up =
      dvm_find_class(vm, "com/unity3d/player/UnityPlayer");
   if (!up) {
      dvm_gil_leave_to_guest(vm, cookie);
      return;
   }
   dvm_ref act = from_jobject(vm, env, activity);
   if (!act) {
      dvm_gil_leave_to_guest(vm, cookie);
      return;
   }
   dvm_pin(vm, act);
   union dvm_value v = { .l = act };
   bool ok = dvm_set_static(vm, up, "currentActivity",
                            "Landroid/app/Activity;", v);
   static int once;
   if (once++ < 4)
      fprintf(stderr,
              "[dvm] UnityPlayer.currentActivity := host 0x%x → @%x (%s)\n",
              (unsigned)(uintptr_t)activity, act, ok ? "ok" : "no-field");
   dvm_gil_leave_to_guest(vm, cookie);
}
