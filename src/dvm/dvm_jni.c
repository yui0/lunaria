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

/* The env to make outward calls through: this thread's, or the process's. */
static JNIEnv *current_env(void) { return g_env ? g_env : g_env_any; }
static dvm_guest_native_fn g_guest_native;
static dvm_guest_library_fn g_guest_library;
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

static bool hook_load_library(void *user, struct dvm *vm, const char *name)
{
   (void)user; (void)vm;
   return g_guest_library && g_guest_library(name);
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
   return slot && slot->host == host ? slot->ref : 0;
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
}

static dvm_ref wrapper_for(struct dvm *vm, const char *class_name, uint32_t host)
{
   if (!host) return 0;
   dvm_ref old = find_wrapper(host);
   if (old) return old;

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
      }
      struct dvm_object *ao = r ? dvm__obj(vm, r) : NULL;
      if (ao) ao->host_handle = (uint32_t)(uintptr_t)o;
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

/* jobject → dvm_ref.  A jstring becomes a real VM string so bytecode can call
 * length()/equals() on it; a Class becomes the VM's Class for that type so
 * getDeclaredMethods()/getMethod() see the dex methods of the class the
 * handle names, not java.lang.Class's own table; anything else is wrapped so
 * its identity survives the round trip back to the stub layer. */
static dvm_ref from_jobject(struct dvm *vm, JNIEnv *env, jobject o)
{
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
         struct dvm_class *c = dvm_find_class(vm, described);
         if (c) {
            dvm_ref r = dvm_class_object(vm, c);
            if (r) dvm_pin(vm, r);
            return r;
         }
      }
   }
   const char *utf = NULL;
   /* GetStringUTFChars asserts on a non-string, so probe the class first. */
   jclass sc = (*env)->GetObjectClass(env, o);
   if (sc) {
      jclass strc = (*env)->FindClass(env, "java/lang/String");
      if (strc && (*env)->IsInstanceOf(env, o, strc))
         utf = (*env)->GetStringUTFChars(env, (jstring)o, NULL);
   }
   if (utf) {
      dvm_ref r = dvm_new_string(vm, utf);
      (*env)->ReleaseStringUTFChars(env, (jstring)o, utf);
      return r;
   }
   /* Wrap it as what it actually is.  Naming every incoming object
    * "java/lang/Object" threw away the one piece of type information the
    * stub layer had: bytecode could not dispatch a virtual call on it, an
    * instanceof against its real class answered false, and any class the VM
    * implements itself (AssetManager, File, …) was unreachable through an
    * object that arrived this way. */
   const char *cn = class_name_of(env, o);
   return wrapper_for(vm, cn && *cn ? cn : "java/lang/Object",
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
   return h;
}

static jobject to_jobject(struct dvm *vm, JNIEnv *env, dvm_ref r)
{
   if (!r) return NULL;
   uint32_t host = dvm_external_handle(vm, r);
   if (host) return (jobject)(uintptr_t)host;

   jobject arr = dvm_array_to_host(vm, env, r);
   if (arr) return arr;

   const char *s = dvm_string_utf8(vm, r);
   if (s) return (jobject)(*env)->NewStringUTF(env, s);

   struct dvm_class *c = dvm_object_class(vm, r);
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
   struct dvm_object *obj = dvm__obj(vm, r);
   if (obj) {
      obj->host_handle = (uint32_t)(uintptr_t)o;
      /* The object can immediately cross back through a native callback.
       * Record the reverse edge now; otherwise from_jobject() wraps the host
       * handle as a fresh VM object and loses every instance field. */
      remember_wrapper(obj->host_handle, r);
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
   /* Off by default, and not because reporting them is wrong — it is the
    * truthful answer, and the launcher now knows it.  It used to be off
    * because Cross Worlds stopped at AndroidThunkJava_GooglePAD_Available once
    * Play Core could see the splits; that is no longer true (the title now
    * reaches the same render loop either way, and further).  What is still
    * missing is the other half: Play Core resolves an install-time pack by
    * walking these arrays and then asking AssetPackStorage for its directory,
    * which the emulator does not answer — "Pack not found with pack name" is
    * what the app gets.  LUNARIA_REPORT_SPLITS=1 turns reporting on for work
    * on that half. */
   const char *on = getenv("LUNARIA_REPORT_SPLITS");
   if (!on || !*on || !strcmp(on, "0")) return 0;
   const char *splits = getenv("ANDROID_SPLIT_APKS");
   size_t n = 0;
   for (const char *p = splits; p && *p; ) {
      const char *end = strchr(p, ';');
      if (!end) end = p + strlen(p);
      if (strchr(p, '|') && strchr(p, '|') < end) ++n;
      p = (*end == ';') ? end + 1 : end;
   }
   /* A package installed from a single APK genuinely has no split arrays;
    * only describe them when the launcher actually installed splits. */
   if (!n) return 0;
   dvm_ref names = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
   dvm_ref dirs  = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
   dvm_ref *nslot = names ? dvm_array_data(vm, names) : NULL;
   dvm_ref *dslot = dirs  ? dvm_array_data(vm, dirs)  : NULL;
   if (!nslot || !dslot) return 0;
   /* Collect first, then sort by name.  The framework keeps splitNames sorted
    * and Play Core depends on it: it locates a pack with
    * Arrays.binarySearch(splitNames, pack) and indexes splitSourceDirs with
    * whatever comes back, so the two arrays must agree index for index and be
    * in the order a binary search expects.  The launcher lists the splits in
    * install order, which is not that order. */
   struct split_ent { const char *name; size_t nlen; const char *dir; size_t dlen; };
   struct split_ent e[64];
   size_t i = 0;
   for (const char *p = splits; *p && i < n && i < 64; ) {
      const char *bar = strchr(p, '|');
      const char *end = strchr(p, ';');
      if (!end) end = p + strlen(p);
      if (bar && bar < end) {
         e[i].name = p;       e[i].nlen = (size_t)(bar - p);
         e[i].dir  = bar + 1; e[i].dlen = (size_t)(end - bar - 1);
         ++i;
      }
      p = (*end == ';') ? end + 1 : end;
   }
   n = i;
   for (size_t a = 1; a < n; ++a) {           /* insertion sort; n is tiny */
      for (size_t b = a; b > 0; --b) {
         size_t la = e[b - 1].nlen, lb = e[b].nlen, m = la < lb ? la : lb;
         int c = strncmp(e[b - 1].name, e[b].name, m);
         if (c == 0) c = la < lb ? -1 : la > lb ? 1 : 0;
         if (c <= 0) break;
         struct split_ent t = e[b - 1]; e[b - 1] = e[b]; e[b] = t;
      }
   }
   for (i = 0; i < n; ++i) {
      nslot[i] = dvm_new_string_n(vm, e[i].name, e[i].nlen);
      dslot[i] = dvm_new_string_n(vm, e[i].dir,  e[i].dlen);
   }
   *out_names = names;
   *out_dirs  = dirs;
   return n;
}

static void dvm_fill_application_info_paths(struct dvm *vm, dvm_ref ai)
{
   union dvm_value v;
   const char *source = lunaria_apk_mount_path();
   v.l = dvm_new_string(vm, source ? source : "");
   (void)dvm_set_field(vm, ai, "sourceDir", "Ljava/lang/String;", v);
   (void)dvm_set_field(vm, ai, "publicSourceDir", "Ljava/lang/String;", v);

   const char *files = getenv("ANDROID_FILES_DIR");
   char data_dir[PATH_MAX];
   snprintf(data_dir, sizeof data_dir, "%s",
            (files && *files) ? files : "/tmp/lunaria-files");
   size_t data_len = strlen(data_dir);
   if (data_len >= 6 && !strcmp(data_dir + data_len - 6, "/files"))
      data_dir[data_len - 6] = '\0';
   v.l = dvm_new_string(vm, data_dir);
   (void)dvm_set_field(vm, ai, "dataDir", "Ljava/lang/String;", v);

   const char *lib_dir = getenv("ANDROID_NATIVE_LIB_DIR");
   v.l = dvm_new_string(vm, lib_dir ? lib_dir : "");
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
static bool pm_query_is_self(struct dvm *vm, const union dvm_value *args,
                             int nargs)
{
   if (nargs < 1 || !args[0].l) return true;   /* null name: the caller's own */
   const char *want = dvm_string_utf8(vm, args[0].l);
   if (!want || !*want) return true;
   const char *self = getenv("ANDROID_PACKAGE_NAME");
   return self && !strcmp(want, self);
}

static bool hook_call_external(void *user, struct dvm *vm, const char *class_name,
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
         if (dvm_on_bytecode_thread()) {
            (void)dvm__queue_runnable_at(vm, args[0].l, false, 0);
            return true;
         }
         struct dvm_class *rc = dvm_object_class(vm, args[0].l);
         struct dvm_method *run = rc ? dvm_find_method(vm, rc, "run", "()V") : NULL;
         if (run) {
            union dvm_value ignored = { 0 };
            (void)dvm_call(vm, run, args[0].l, NULL, 0, &ignored);
         }
      }
      /* Always consume the call: a failed Runnable must not look like a
       * missing Activity.runOnUiThread stub (note_missing). */
      return true;
   }
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
      if (!pm_query_is_self(vm, args, nargs)) {
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
      struct dvm_class *bc = dvm_find_class(vm, "android/os/Bundle");
      union dvm_value v = { .l = bc ? dvm_new_object(vm, bc) : 0 };
      (void)dvm_set_field(vm, ai, "metaData", "Landroid/os/Bundle;", v);
      const char *pkg = getenv("ANDROID_PACKAGE_NAME");
      v.l = pkg ? dvm_new_string(vm, pkg) : 0;
      (void)dvm_set_field(vm, ai, "packageName", "Ljava/lang/String;", v);
      /* An app cannot target a level the device does not have. */
      v.i = lunaria_sdk_int();
      (void)dvm_set_field(vm, ai, "targetSdkVersion", "I", v);
      dvm_fill_application_info_paths(vm, ai);
      dvm_pin(vm, ai);
      out->l = ai;
      return true;
   }

   if (!strcmp(method, "getPackageInfo") && strstr(class_name, "PackageManager")) {
      memset(out, 0, sizeof *out);
      if (!pm_query_is_self(vm, args, nargs)) {
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
      dvm_fill_application_info_paths(vm, ai);
      union dvm_value v = { .i = lunaria_sdk_int() };
      (void)dvm_set_field(vm, ai, "targetSdkVersion", "I", v);
      /* Same ApplicationInfo the getApplicationInfo path builds: manifest
       * meta-data is read straight off pi.applicationInfo.metaData. */
      {
         struct dvm_class *bc = dvm_find_class(vm, "android/os/Bundle");
         union dvm_value bv = { .l = bc ? dvm_new_object(vm, bc) : 0 };
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
      const char *pkg = getenv("ANDROID_PACKAGE_NAME");
      v.l = pkg ? dvm_new_string(vm, pkg) : 0;
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
      arm_exec_apk_version(&vcode, &vname);
      v.i = vcode;
      (void)dvm_set_field(vm, pi, "versionCode", "I", v);
      v.l = vname ? dvm_new_string(vm, vname) : 0;
      (void)dvm_set_field(vm, pi, "versionName", "Ljava/lang/String;", v);
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
      dvm_ref service = dvm_runtime_system_service(vm, want);
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
    * unresolved call there reads as "this device has no display service". */
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
   return wrapper_for(vm, class_name, (uint32_t)(uintptr_t)o);
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

static bool hook_call_native(void *user, struct dvm *vm, const char *class_name,
                             const char *method, const char *sig, bool is_static,
                             dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)user;
   if (!g_guest_native || !current_env()) return false;

   jvalue jargs[64];
   memset(jargs, 0, sizeof jargs);
   for (int i = 0; i < nargs && i < 64; ++i) {
      char one[256];
      if (!dvm__sig_param(sig, i, one, sizeof one)) break;
      jargs[i] = dvm_to_jvalue(vm, current_env(), dvm__kind_of(one), args[i]);
   }

   jvalue ret;
   memset(&ret, 0, sizeof ret);
   jobject jself = self ? to_jobject(vm, current_env(), self) : NULL;
   if (!g_guest_native(class_name, method, sig, is_static, jself, jargs, nargs, &ret))
      return false;

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
      default:  out->l = from_jobject(vm, current_env(), ret.l); break;
   }
   return true;
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
      .load_library = hook_load_library,
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
   if (!m || !(m->has_code || m->builtin)) {
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

   dvm_ref dself = 0;
   if (!is_static && self) {
      /* The receiver is a stub-layer handle.  Give the VM a wrapper carrying
       * that handle so a call back out lands on the same object. */
      dself = wrapper_for(vm, class_name, (uint32_t)(uintptr_t)self);
   } else if (!is_static) {
      dself = dvm_new_object(vm, dvm_find_class(vm, class_name));
   }

   union dvm_value ret;
   ++g_calls;
   bool ok = dvm_call(vm, m, dself, args, nargs, &ret);
   for (int i = 0; i < nargs; ++i)
      if (host_arrays[i]) array_sync_back(vm, env, host_arrays[i], args[i].l);
   if (!ok) {
      char buf[512];
      dvm_describe_exception(vm, dvm_exception(vm), buf, sizeof buf);
      fprintf(stderr, "[dvm] %s.%s%s threw %s\n", class_name, method, msig, buf);
      dvm_clear_exception(vm);
      memset(&ret, 0, sizeof ret);
   }

   if (out) *out = dvm_to_jvalue(vm, env, dvm_sig_return_kind(msig), ret);
   g_env = saved;
   return true;
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

   dvm_ref self = wrapper_for(vm, cls, (uint32_t)(uintptr_t)obj);
   if (!self) return false;

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
