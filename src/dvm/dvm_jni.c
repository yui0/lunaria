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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct dvm *g_vm;
static bool g_tried;
static enum dvm_jni_mode g_mode = DVM_JNI_FILL_GAPS;
static JNIEnv *g_env;               /* the env of the call currently in flight */
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
static struct {
   uint32_t host;
   dvm_ref ref;
} g_wrappers[512];
static unsigned g_nwrappers;

static void remember_wrapper(uint32_t host, dvm_ref ref)
{
   if (!host || !ref) return;
   for (unsigned i = 0; i < g_nwrappers; ++i) {
      if (g_wrappers[i].host == host) {
         g_wrappers[i].ref = ref;
         return;
      }
   }
   if (g_nwrappers < sizeof g_wrappers / sizeof g_wrappers[0]) {
      g_wrappers[g_nwrappers].host = host;
      g_wrappers[g_nwrappers].ref = ref;
      ++g_nwrappers;
   }
}

static dvm_ref wrapper_for(struct dvm *vm, const char *class_name, uint32_t host)
{
   if (!host) return 0;
   for (unsigned i = 0; i < g_nwrappers; ++i)
      if (g_wrappers[i].host == host) return g_wrappers[i].ref;

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

static dvm_ref host_array_to_dvm(struct dvm *vm, JNIEnv *env, jobject o)
{
   const char *cls = class_name_of(env, o);
   char kind;
   size_t width;
   if (!host_array_kind(cls, &kind, &width) || kind == 'L') return 0;

   jsize n = (*env)->GetArrayLength(env, o);
   char elem[2] = { kind, 0 };
   dvm_ref r = dvm_new_array(vm, kind, elem, (uint32_t)(n < 0 ? 0 : n));
   void *dst = r ? dvm_array_data(vm, r) : NULL;
   if (dst && n > 0) ARRAY_REGION(env, Get, kind, o, n, dst);
   return r;
}

/* The other direction of the copy above, run after the callee returns. */
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
 * length()/equals() on it; anything else is wrapped so its identity survives
 * the round trip back to the stub layer. */
static dvm_ref from_jobject(struct dvm *vm, JNIEnv *env, jobject o)
{
   if (!o) return 0;
   dvm_ref arr = host_array_to_dvm(vm, env, o);
   if (arr) return arr;
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
      default: return NULL;   /* object arrays keep the wrapper path */
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
   JNIEnv *env = g_env;
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
      dvm_pin(vm, ai);
      out->l = ai;
      return true;
   }

   if (!strcmp(method, "getPackageInfo") && strstr(class_name, "PackageManager")) {
      memset(out, 0, sizeof *out);
      struct dvm_class *pc = dvm_find_class(vm, "android/content/pm/PackageInfo");
      struct dvm_class *ac = dvm_find_class(vm, "android/content/pm/ApplicationInfo");
      dvm_ref pi = pc ? dvm_new_object(vm, pc) : 0;
      dvm_ref ai = ac ? dvm_new_object(vm, ac) : 0;
      if (!pi || !ai) return false;
      union dvm_value v = { .i = lunaria_sdk_int() };
      (void)dvm_set_field(vm, ai, "targetSdkVersion", "I", v);
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
   JNIEnv *env = g_env;
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
   JNIEnv *env = g_env;
   if (!env || !type) return false;
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
   if (!g_guest_native || !g_env) return false;

   jvalue jargs[64];
   memset(jargs, 0, sizeof jargs);
   for (int i = 0; i < nargs && i < 64; ++i) {
      char one[256];
      if (!dvm__sig_param(sig, i, one, sizeof one)) break;
      jargs[i] = dvm_to_jvalue(vm, g_env, dvm__kind_of(one), args[i]);
   }

   jvalue ret;
   memset(&ret, 0, sizeof ret);
   jobject jself = self ? to_jobject(vm, g_env, self) : NULL;
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
      default:  out->l = from_jobject(vm, g_env, ret.l); break;
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

bool dvm_jni_invoke(JNIEnv *env, const char *class_name, const char *method,
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

bool dvm_jni_field(JNIEnv *env, jobject obj, jfieldID field, bool set,
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

const char *dvm_jni_super_name(const char *class_name)
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

bool dvm_jni_class_in_dex(const char *class_name)
{
   struct dvm *vm = vm_get();
   return vm && class_name && dvm_class_is_known(vm, class_name);
}

void dvm_jni_report(void)
{
   if (!g_vm) return;
   fprintf(stderr, "[dvm] %u bytecode calls, %u out to stubs, %u out to guest natives, "
                   "%llu instructions\n",
           g_calls, g_external_calls, g_native_calls,
           (unsigned long long)dvm_instructions(g_vm));
}
