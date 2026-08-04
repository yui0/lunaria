/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "dvm/dvm_jni.h"
#include "dvm/dvm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct dvm *g_vm;
static bool g_tried;
static enum dvm_jni_mode g_mode = DVM_JNI_FILL_GAPS;
static JNIEnv *g_env;               /* the env of the call currently in flight */
static dvm_guest_native_fn g_guest_native;
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

static dvm_ref wrapper_for(struct dvm *vm, const char *class_name, uint32_t host)
{
   if (!host) return 0;
   for (unsigned i = 0; i < g_nwrappers; ++i)
      if (g_wrappers[i].host == host) return g_wrappers[i].ref;

   dvm_ref r = dvm_wrap_external(vm, class_name, host);
   if (!r) return 0;
   dvm_pin(vm, r);
   if (g_nwrappers < sizeof g_wrappers / sizeof g_wrappers[0]) {
      g_wrappers[g_nwrappers].host = host;
      g_wrappers[g_nwrappers].ref = r;
      ++g_nwrappers;
   }
   return r;
}

/* jobject → dvm_ref.  A jstring becomes a real VM string so bytecode can call
 * length()/equals() on it; anything else is wrapped so its identity survives
 * the round trip back to the stub layer. */
static dvm_ref from_jobject(struct dvm *vm, JNIEnv *env, jobject o)
{
   if (!o) return 0;
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
   return wrapper_for(vm, "java/lang/Object", (uint32_t)(uintptr_t)o);
}

/* dvm_ref → jobject.  Strings become real jstrings; a wrapper hands back the
 * handle it came in with; anything else becomes an opaque object of the right
 * class, so IsInstanceOf and GetObjectClass on the stub side still work. */
static jobject to_jobject(struct dvm *vm, JNIEnv *env, dvm_ref r)
{
   if (!r) return NULL;
   uint32_t host = dvm_external_handle(vm, r);
   if (host) return (jobject)(uintptr_t)host;

   const char *s = dvm_string_utf8(vm, r);
   if (s) return (jobject)(*env)->NewStringUTF(env, s);

   struct dvm_class *c = dvm_object_class(vm, r);
   jclass cls = (*env)->FindClass(env, c ? c->name : "java/lang/Object");
   jobject o = (*env)->AllocObject(env, cls);
   struct dvm_object *obj = dvm__obj(vm, r);
   if (obj) obj->host_handle = (uint32_t)(uintptr_t)o;
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

   jclass cls = (*env)->FindClass(env, class_name);
   if (!cls) return false;

   bool is_static = (self == 0);
   jmethodID mid = is_static ? (*env)->GetStaticMethodID(env, cls, method, sig)
                             : (*env)->GetMethodID(env, cls, method, sig);
   if (!mid) return false;

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
      struct dvm_class *ga = dvm_find_class(vm, "com/epicgames/ue4/GameActivity");
      union dvm_value cur = { 0 };
      if (ga &&
          dvm_get_static(vm, ga, "_activity",
                         "Lcom/epicgames/ue4/GameActivity;", &cur))
         activity = cur.l;
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
         "com/epicgames/ue4/VolumeReceiver",
         "com/epicgames/ue4/BatteryReceiver",
         "com/epicgames/ue4/HeadsetReceiver",
      };
      for (size_t i = 0; i < sizeof recv / sizeof recv[0]; ++i) {
         struct dvm_method *m = dvm_lookup(
            g_vm, recv[i], "startReceiver", "(Landroid/app/Activity;)V");
         if (m) m->builtin = builtin_ue_start_receiver;
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
   memset(args, 0, sizeof args);
   for (int i = 0; i < nargs; ++i) {
      char one[256];
      if (!dvm__sig_param(msig, i, one, sizeof one)) break;

      jvalue jv;
      if (ap) jv = va_next(ap, one);
      else if (jargs) jv = jargs[i];
      else memset(&jv, 0, sizeof jv);
      args[i] = jvalue_to_dvm(vm, env, one, jv);
   }

   dvm_ref dself = 0;
   if (!is_static && self) {
      /* The receiver is a stub-layer handle.  Give the VM a wrapper carrying
       * that handle so a call back out lands on the same object. */
      dself = wrapper_for(vm, class_name, (uint32_t)(uintptr_t)self);
   } else if (!is_static) {
      dself = dvm_new_object(vm, dvm_find_class(vm, class_name));
   }

   /* Android normally constructs GameActivity and runs its Java onCreate
    * before native startup.  The native-activity loader creates the matching
    * host object directly, so that lifecycle assignment is the one piece of
    * Java state which has not happened: UE's generated activity stores itself
    * in the static `_activity` field and every runOnUiThread thunk reads that
    * field as its receiver.  Bind it to the stable wrapper the first time the
    * real activity crosses JNI.  This models the missing platform lifecycle;
    * it does not special-case or skip the thunk's bytecode. */
   if (dself && !is_static &&
       (!strcmp(class_name, "com/epicgames/ue4/GameActivity") ||
        !strcmp(class_name, "com.epicgames.ue4.GameActivity"))) {
      struct dvm_class *activity_cls = dvm_find_class(vm, class_name);
      union dvm_value current = { 0 };
      if (activity_cls &&
          dvm_get_static(vm, activity_cls, "_activity",
                         "Lcom/epicgames/ue4/GameActivity;", &current) &&
          !current.l) {
         union dvm_value activity = { .l = dself };
         (void)dvm_set_static(vm, activity_cls, "_activity",
                              "Lcom/epicgames/ue4/GameActivity;", activity);
      }
      /* The generated constructor also initializes the dialog state to the
       * enum's None value.  A wrapped platform-created Activity has not run
       * that constructor in the bytecode VM, so leaving it as Java null makes
       * the first visibility check look like an active dialog and its UI
       * Runnable then calls ordinal() on null. */
      union dvm_value dialog = { 0 };
      if (activity_cls &&
          dvm_get_field(vm, dself, "CurrentDialogType",
                        "Lcom/epicgames/ue4/GameActivity$EAlertDialogType;",
                        &dialog) && !dialog.l) {
         struct dvm_class *enum_cls = dvm_find_class(
            vm, "com/epicgames/ue4/GameActivity$EAlertDialogType");
         union dvm_value none = { 0 };
         if (enum_cls &&
             dvm_get_static(vm, enum_cls, "None",
                            "Lcom/epicgames/ue4/GameActivity$EAlertDialogType;",
                            &none) && none.l)
            (void)dvm_set_field(
               vm, dself, "CurrentDialogType",
               "Lcom/epicgames/ue4/GameActivity$EAlertDialogType;", none);
      }
   }

   union dvm_value ret;
   ++g_calls;
   bool ok = dvm_call(vm, m, dself, args, nargs, &ret);
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

void dvm_jni_report(void)
{
   if (!g_vm) return;
   fprintf(stderr, "[dvm] %u bytecode calls, %u out to stubs, %u out to guest natives, "
                   "%llu instructions\n",
           g_calls, g_external_calls, g_native_calls,
           (unsigned long long)dvm_instructions(g_vm));
}
