/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * JNI native method stubs for Android/Java/Unity APIs (merged from libjvm-*.c).
 */

#include <assert.h>
#include <dlfcn.h>
#include <err.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "jvm/jni.h"
#include "jvm.h"
#include "arm_exec.h"
#include "dvm/dvm_jni.h"

extern void arm_exec_drain_gl_thread_jobs(void);
const char *arm_exec_get_main_lib_dir(void);

#define JNI_FILE_PATHS_MAX 65536u
static char *g_file_paths[JNI_FILE_PATHS_MAX];
static jobject g_current_activity;

void
jni_set_current_activity(JNIEnv *env, jobject activity)
{
   g_current_activity = activity;
   /* IronSource (and many other Unity plugins) read the public static field
    * UnityPlayer.currentActivity via sget, not the currentActivity() accessor
    * stub.  Mirror the process Activity into that field whenever it is set,
    * the same way UnityPlayer itself does after attach. */
   if (env && activity)
      dvm_jni_bind_unity_activity(env, activity);
}

jobject
jni_get_current_activity(void)
{
   return g_current_activity;
}

void
jni_file_set_path(jobject file, const char *path)
{
   if (!file)
      return;
   uintptr_t idx = (uintptr_t)file;
   if (idx >= JNI_FILE_PATHS_MAX)
      return;
   free(g_file_paths[idx]);
   g_file_paths[idx] = (path && *path) ? strdup(path) : NULL;
}

const char *
jni_file_get_path(jobject file)
{
   if (!file)
      return NULL;
   uintptr_t idx = (uintptr_t)file;
   return (idx < JNI_FILE_PATHS_MAX) ? g_file_paths[idx] : NULL;
}

static void
jni_file_join(char *out, size_t outsz, const char *parent, const char *child)
{
   /* Match java.io.UnixFileSystem.resolve: empty parent → child alone.
    * Inserting '/' here made absolute paths out of relative names. */
   if (!parent || !*parent) {
      snprintf(out, outsz, "%s", child ? child : "");
      return;
   }
   if (!child || !*child) {
      snprintf(out, outsz, "%s", parent);
      return;
   }
   if (child[0] == '/') {
      snprintf(out, outsz, "%s", child);
      return;
   }
   const size_t plen = strlen(parent);
   if (plen > 0 && parent[plen - 1] == '/')
      snprintf(out, outsz, "%s%s", parent, child);
   else
      snprintf(out, outsz, "%s/%s", parent, child);
}

static const char *
jni_file_default_dir(void)
{
   const char *p = getenv("ANDROID_EXTERNAL_FILES_DIR");
   return (p && *p) ? p : "/tmp";
}

static jstring
jni_file_path_string(JNIEnv *env, jobject object)
{
   const char *p = jni_file_get_path(object);
   if (!p || !*p)
      p = jni_file_default_dir();
   return (*env)->NewStringUTF(env, p);
}

void
jni_file_bind_ctor(JNIEnv *env, jobject file, jmethodID ctor, va_list ap)
{
   struct jvm *jvm = jnienv_get_jvm(env);
   if (!jvm || !file || !ctor)
      return;
   struct jvm_object *m = &jvm->objects[(uintptr_t)ctor - 1];
   if (m->type != JVM_OBJECT_METHOD || !m->method.signature.data)
      return;
   const char *sig = m->method.signature.data;
   char path[PATH_MAX];
   if (!strcmp(sig, "(Ljava/lang/String;)V")) {
      jstring js = va_arg(ap, jstring);
      const char *utf = js ? (*env)->GetStringUTFChars(env, js, NULL) : NULL;
      if (utf) {
         jni_file_set_path(file, utf);
         (*env)->ReleaseStringUTFChars(env, js, utf);
      }
   } else if (!strcmp(sig, "(Ljava/io/File;Ljava/lang/String;)V")) {
      jobject parent = va_arg(ap, jobject);
      jstring child = va_arg(ap, jstring);
      const char *base = jni_file_get_path(parent);
      if (!base)
         base = jni_file_default_dir();
      const char *utf = child ? (*env)->GetStringUTFChars(env, child, NULL) : NULL;
      if (utf) {
         jni_file_join(path, sizeof path, base, utf);
         jni_file_set_path(file, path);
         (*env)->ReleaseStringUTFChars(env, child, utf);
      }
   } else if (!strcmp(sig, "(Ljava/lang/String;Ljava/lang/String;)V")) {
      jstring p1 = va_arg(ap, jstring);
      jstring p2 = va_arg(ap, jstring);
      const char *u1 = p1 ? (*env)->GetStringUTFChars(env, p1, NULL) : NULL;
      const char *u2 = p2 ? (*env)->GetStringUTFChars(env, p2, NULL) : NULL;
      if (u1 && u2) {
         jni_file_join(path, sizeof path, u1, u2);
         jni_file_set_path(file, path);
      } else if (u1) {
         jni_file_set_path(file, u1);
      }
      if (u1)
         (*env)->ReleaseStringUTFChars(env, p1, u1);
      if (u2)
         (*env)->ReleaseStringUTFChars(env, p2, u2);
   }
}

void
jni_file_bind_ctor_a(JNIEnv *env, jobject file, jmethodID ctor, const jvalue *args)
{
   struct jvm *jvm = jnienv_get_jvm(env);
   if (!jvm || !file || !ctor || !args)
      return;
   struct jvm_object *m = &jvm->objects[(uintptr_t)ctor - 1];
   if (m->type != JVM_OBJECT_METHOD || !m->method.signature.data)
      return;
   const char *sig = m->method.signature.data;
   char path[PATH_MAX];
   if (!strcmp(sig, "(Ljava/lang/String;)V")) {
      jstring js = (jstring)args[0].l;
      const char *utf = js ? (*env)->GetStringUTFChars(env, js, NULL) : NULL;
      if (utf) {
         jni_file_set_path(file, utf);
         (*env)->ReleaseStringUTFChars(env, js, utf);
      }
   } else if (!strcmp(sig, "(Ljava/io/File;Ljava/lang/String;)V")) {
      jobject parent = (jobject)args[0].l;
      jstring child = (jstring)args[1].l;
      const char *base = jni_file_get_path(parent);
      if (!base)
         base = jni_file_default_dir();
      const char *utf = child ? (*env)->GetStringUTFChars(env, child, NULL) : NULL;
      if (utf) {
         jni_file_join(path, sizeof path, base, utf);
         jni_file_set_path(file, path);
         (*env)->ReleaseStringUTFChars(env, child, utf);
      }
   } else if (!strcmp(sig, "(Ljava/lang/String;Ljava/lang/String;)V")) {
      jstring p1 = (jstring)args[0].l;
      jstring p2 = (jstring)args[1].l;
      const char *u1 = p1 ? (*env)->GetStringUTFChars(env, p1, NULL) : NULL;
      const char *u2 = p2 ? (*env)->GetStringUTFChars(env, p2, NULL) : NULL;
      if (u1 && u2) {
         jni_file_join(path, sizeof path, u1, u2);
         jni_file_set_path(file, path);
      } else if (u1) {
         jni_file_set_path(file, u1);
      }
      if (u1)
         (*env)->ReleaseStringUTFChars(env, p1, u1);
      if (u2)
         (*env)->ReleaseStringUTFChars(env, p2, u2);
   }
}


/* Forward declaration — implemented in arm_exec.cpp */

jstring
java_lang_System_getProperty(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   // The ARM JNI bridge dispatches through CallObjectMethodA with a NULL
   // argument vector, so `args` may be NULL.
   if (!args) return NULL;
   const char *key = (*env)->GetStringUTFChars(env, va_arg(args, jstring), NULL);
   if (!key) return NULL;

   if (!strcmp(key, "java.vm.version"))
       return (*env)->NewStringUTF(env, "1.6");

   union {
      void *ptr;
      int (*fun)(const char*, char*);
   } __system_property_get;

   if (!(__system_property_get.ptr = dlsym(RTLD_DEFAULT, "__system_property_get")))
      return NULL;

   char value[92]; // PROP_VALUE_MAX 92
   __system_property_get.fun(key, value);
   return (*env)->NewStringUTF(env, value);
}

void
java_lang_System_load(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return;
   const char *lib = (*env)->GetStringUTFChars(env, va_arg(args, jstring), NULL);

   struct {
      union {
         void *ptr;
         void* (*fun)(const char*, int);
      } open;

      union {
         void *ptr;
         void* (*fun)(void*, const char*);
      } sym;
   } dl;

   if (!(dl.open.ptr = dlsym(RTLD_DEFAULT, "bionic_dlopen")) || !(dl.sym.ptr = dlsym(RTLD_DEFAULT, "bionic_dlsym"))) {
      dl.open.fun = dlopen;
      dl.sym.fun = dlsym;
   }

   void *handle;
   if (!(handle = dl.open.fun(lib, RTLD_NOW | RTLD_GLOBAL))) {
      warnx("java/lang/System/load: failed to dlopen `%s`", lib);
      return;
   }

   union {
      void *ptr;
      void* (*fun)(void*, void*);
   } JNI_OnLoad;

   if ((JNI_OnLoad.ptr = dl.sym.fun(handle, "JNI_OnLoad"))) {
      JavaVM *vm;
      (*env)->GetJavaVM(env, &vm);
      JNI_OnLoad.fun(vm, NULL);
   }
}

jobject
java_lang_ClassLoader_findLibrary(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return NULL;
   const char *libname = (*env)->GetStringUTFChars(env, va_arg(args, jstring), NULL);
   const char *libdir = arm_exec_get_main_lib_dir();
   char lib[4096];
   lib[0] = '\0';
   if (!libname || !*libname)
      return NULL;
   /* Full path: as-is, then +.so (mono probes without the suffix first). */
   if (strchr(libname, '/')) {
      if (access(libname, F_OK) == 0)
         snprintf(lib, sizeof(lib), "%s", libname);
      else {
         size_t n = strlen(libname);
         if (n + 4 < sizeof(lib) && (n < 3 || strcmp(libname + n - 3, ".so") != 0)) {
            snprintf(lib, sizeof(lib), "%s.so", libname);
            if (access(lib, F_OK) != 0) lib[0] = '\0';
         }
      }
   } else {
      /* Bare name: try Android spellings under the APK lib dir. */
      size_t nl = strlen(libname);
      int has_so = nl > 3 && !strcmp(libname + nl - 3, ".so");
      int has_lib = nl > 3 && !strncmp(libname, "lib", 3);
      char cand[4096];
      int i, ntry = 0;
      const char *cands[4];
      if (libdir && *libdir) {
         if (has_so) {
            snprintf(cand, sizeof(cand), "%s/%s", libdir, libname);
            cands[ntry++] = cand;
         } else if (has_lib) {
            static char a[4096], b[4096];
            snprintf(a, sizeof(a), "%s/%s.so", libdir, libname);
            snprintf(b, sizeof(b), "%s/%s", libdir, libname);
            cands[ntry++] = a; cands[ntry++] = b;
         } else {
            static char a[4096], b[4096], c[4096], d[4096];
            snprintf(a, sizeof(a), "%s/lib%s.so", libdir, libname);
            snprintf(b, sizeof(b), "%s/%s.so", libdir, libname);
            snprintf(c, sizeof(c), "%s/lib%s", libdir, libname);
            snprintf(d, sizeof(d), "%s/%s", libdir, libname);
            cands[ntry++] = a; cands[ntry++] = b;
            cands[ntry++] = c; cands[ntry++] = d;
         }
      } else if (has_so) {
         cands[ntry++] = libname;
      } else if (has_lib) {
         snprintf(cand, sizeof(cand), "%s.so", libname);
         cands[ntry++] = cand;
      } else {
         snprintf(cand, sizeof(cand), "lib%s.so", libname);
         cands[ntry++] = cand;
      }
      for (i = 0; i < ntry; ++i) {
         if (access(cands[i], F_OK) == 0) {
            snprintf(lib, sizeof(lib), "%s", cands[i]);
            break;
         }
      }
   }
   if (!lib[0]) return NULL;
   return (*env)->NewStringUTF(env, lib);
}

jobject
java_lang_ClassLoader_findClass(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return NULL;
   jstring str = va_arg(args, jstring);
   const char *utf = (*env)->GetStringUTFChars(env, str, NULL);
   return (*env)->FindClass(env, utf);
}

/* ClassLoader.loadClass(name) — same as findClass for our stub JVM.
 * libgpg's DexClassLoader.loadClass must not fall through to a NULL wrapper
 * call (host SIGSEGV); map the name onto FindClass so GMS class handles exist. */
jobject
java_lang_ClassLoader_loadClass(JNIEnv *env, jobject object, va_list args)
{
   return java_lang_ClassLoader_findClass(env, object, args);
}

jobject
dalvik_system_DexClassLoader_loadClass(JNIEnv *env, jobject object, va_list args)
{
   return java_lang_ClassLoader_findClass(env, object, args);
}

jobject
dalvik_system_BaseDexClassLoader_loadClass(JNIEnv *env, jobject object, va_list args)
{
   return java_lang_ClassLoader_findClass(env, object, args);
}

jobject
java_lang_Class_getClassLoader(JNIEnv *env, jobject object)
{
   assert(env && object);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/lang/ClassLoader"))));
}

jclass
java_lang_Class_forName(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return NULL;
   jstring str = va_arg(args, jstring);
   const char *utf = (*env)->GetStringUTFChars(env, str, NULL);
   return (*env)->FindClass(env, utf);
}

jstring
java_lang_Class_getName(JNIEnv *env, jobject object)
{
   assert(env && object);

   {
      struct {
         union {
            void *ptr;
            struct jvm* (*fun)(JNIEnv*);
         } jnienv_get_jvm;

         union {
            void *ptr;
            const char* (*fun)(const struct jvm*, jobject);
         } jvm_get_class_name;
      } jvm;

      if ((jvm.jnienv_get_jvm.ptr = dlsym(RTLD_DEFAULT, "jnienv_get_jvm")) && (jvm.jvm_get_class_name.ptr = dlsym(RTLD_DEFAULT, "jvm_get_class_name"))) {
         struct jvm *jvm_ = jvm.jnienv_get_jvm.fun(env);
         return (*env)->NewStringUTF(env, jvm.jvm_get_class_name.fun(jvm_, object));
      }
   }

   warnx("%s: returning NULL, as running in unknown JVM and don't know how to get class name", __func__);
   return NULL;
}

jclass
java_lang_Object_getClass(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->GetObjectClass(env, object);
}

jstring
java_io_File_getPath(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   return jni_file_path_string(env, object);
}

jstring
java_io_File_getParent(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   char path[PATH_MAX];
   const char *p = jni_file_get_path(object);
   if (!p || !*p)
      p = jni_file_default_dir();
   snprintf(path, sizeof path, "%s", p);
   return (*env)->NewStringUTF(env, dirname(path));
}

jboolean
java_lang_String_equals(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return false;
   jstring str = va_arg(args, jstring);
   const char *utf1 = (*env)->GetStringUTFChars(env, object, NULL);
   const char *utf2 = (*env)->GetStringUTFChars(env, str, NULL);
   const jboolean equal = (utf1 == utf2 || (utf1 && utf2 && !strcmp(utf1, utf2)));
   (*env)->ReleaseStringUTFChars(env, object, utf1);
   (*env)->ReleaseStringUTFChars(env, str, utf2);
   return equal;
}

jbyteArray
java_lang_String_getBytes(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   const char *utf = (*env)->GetStringUTFChars(env, object, NULL);
   const size_t len = (utf ? strlen(utf) : 0);
   jbyteArray bytes = (*env)->NewByteArray(env, len);
   (*env)->SetByteArrayRegion(env, bytes, 0, len, (jbyte*)utf);
   return bytes;
}

jobject
java_util_Locale_getDefault(JNIEnv *env, jclass clazz)
{
   assert(env);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/util/Locale"))));
}

jstring
java_util_Locale_getLanguage(JNIEnv *env, jobject object)
{
   assert(env);
   if (!object) return (*env)->NewStringUTF(env, "en");
   return (*env)->NewStringUTF(env, "en");
}

jstring
java_util_Locale_toString(JNIEnv *env, jobject object)
{
   assert(env);
   return (*env)->NewStringUTF(env, "en_US");
}

jstring
java_util_Locale_toLanguageTag(JNIEnv *env, jobject object)
{
   assert(env);
   return (*env)->NewStringUTF(env, "en-US");
}

jstring
java_util_Locale_getCountry(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->NewStringUTF(env, "US");
}

jobject
java_lang_Thread_currentThread(JNIEnv *env, jclass clazz)
{
   assert(env);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/lang/Thread"))));
}

/* Choreographer.getInstance() → singleton stub.  UnityChoreographer's init
 * message (what=0) stores a GlobalRef of this at [this+0x70] and the main
 * thread waits on cond this+0xc until it is non-NULL — returning NULL here
 * deadlocks startup. */
jobject
android_view_Choreographer_getInstance(JNIEnv *env, jclass clazz)
{
   assert(env);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/view/Choreographer"))));
}

/* Long.longValue() — unboxes the frameTimeNanos argument that doFrame(J)
 * receives through the JNIBridge proxy Object[] args. */
jlong
java_lang_Long_longValue(JNIEnv *env, jobject object)
{
   assert(env);
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (jlong)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* Return empty array so Unity's STE construction loop runs 0 iterations */
jobjectArray
java_lang_Thread_getStackTrace(JNIEnv *env, jobject thread)
{
   assert(env);
   return (*env)->NewObjectArray(env, 0, (*env)->FindClass(env, "java/lang/StackTraceElement"), NULL);
}

jobjectArray
java_lang_Throwable_getStackTrace(JNIEnv *env, jobject throwable)
{
   assert(env);
   return (*env)->NewObjectArray(env, 0, (*env)->FindClass(env, "java/lang/StackTraceElement"), NULL);
}

/* no-op: suppress stack-trace capture on exception construction */
jobject
java_lang_Throwable_fillInStackTrace(JNIEnv *env, jobject throwable)
{
   assert(env && throwable);
   return throwable;
}

void
java_lang_Throwable_printStackTrace(JNIEnv *env, jobject throwable)
{
   assert(env && throwable);
   (void)throwable;
}

jstring
java_lang_Throwable_getMessage(JNIEnv *env, jobject throwable)
{
   assert(env && throwable);
   return NULL;
}

/* Scanner.useDelimiter(pattern) → the Scanner itself (fluent API).  Unity reads
 * small text resources via `new Scanner(stream).useDelimiter("\\A").next()`.
 * Returning NULL left the guest calling next() on a null Scanner → vtable load
 * through garbage → wild jump.  Returning `object` keeps the chain on a valid
 * handle; the subsequent hasNext()/next() can then resolve cleanly. */
jobject
java_util_Scanner_useDelimiter(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object); (void)args;
   return object;
}

/* Scanner.hasNext() / Scanner.hasNextLine() → false: the scanner stub has no
 * backing stream, so no further tokens exist. */
jboolean
java_util_Scanner_hasNext(JNIEnv *env, jobject object)
{
   assert(env && object);
   return 0;
}

jboolean
java_util_Scanner_hasNextLine(JNIEnv *env, jobject object)
{
   assert(env && object);
   return 0;
}

/* Scanner.next() / Scanner.nextLine(): Unity calls this after
 * new Scanner(stream).useDelimiter("\\A").next() to read a whole resource
 * file as a string.  With our stub Scanner there is no backing stream, so
 * return an empty string (the safe value for missing/empty resources). */
jstring
java_util_Scanner_next(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->NewStringUTF(env, "");
}

jstring
java_util_Scanner_nextLine(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->NewStringUTF(env, "");
}

/* Scanner.close() — no-op: the stub has no resources to release. */
void
java_util_Scanner_close(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
}

/* Iterator.hasNext() → false: the stub iterator from Set.iterator() is empty. */
jboolean
java_util_Iterator_hasNext(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   return 0;
}

/* Iterator.next() → NULL: called only if hasNext() was wrongly believed true. */
jobject
java_util_Iterator_next(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   return NULL;
}

/* Object.toString() → empty string: avoids wild jumps when Unity stringifies
 * objects whose class has no registered native toString handler. */
jstring
java_lang_Object_toString(JNIEnv *env, jobject object)
{
   (void)object;
   return (*env)->NewStringUTF(env, "");
}

/* Map.entrySet() → a Set stub, Set.iterator() → an Iterator stub.  These back
 * the empty SharedPreferences.getAll() map: the migration loop calls
 * getAll().entrySet().iterator().hasNext(), and hasNext() defaults to false, so
 * a valid (empty) Set/Iterator pair lets it iterate zero times and move on. */
jobject
java_util_Map_entrySet(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object); (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/util/Set"))));
}

jobject
java_util_Set_iterator(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object); (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/util/Iterator"))));
}




/* Rate-limited call trace so we can see which MotionEvent/InputDevice getters
 * Unity actually reads after nativeInjectEvent. */
static void motion_trace(const char *m)
{
   static int n;
   if (n < 60) { fprintf(stderr, "[motion] %s\n", m); ++n; }
}

/* ------------------------------------------------------------------------ *
 * android.util.Log
 *
 * Every Android app logs through this class, and the SDKs a title bundles
 * (Netmarble, AppsFlyer, CrashSight, Epic's own GameActivity) narrate what they
 * are doing there and nowhere else.  With no implementation the calls returned
 * 0 and the text was dropped on the floor, so the Java half of a run was mute.
 * The platform writes to logcat; here stderr is the log, next to the emulator's
 * own trace.
 * ------------------------------------------------------------------------ */

static jint
android_log_write(JNIEnv *env, char level, va_list args)
{
   if (!args) return 0;
   const char *tag = (*env)->GetStringUTFChars(env, va_arg(args, jstring), NULL);
   /* Log.w(String, Throwable) has no message argument; GetStringUTFChars
    * answers NULL for a non-string handle rather than misreading it. */
   const char *msg = (*env)->GetStringUTFChars(env, va_arg(args, jstring), NULL);
   fprintf(stderr, "[java %c/%s] %s\n", level, tag ? tag : "?", msg ? msg : "");
   return (jint)((tag ? strlen(tag) : 0) + (msg ? strlen(msg) : 0) + 2);
}

jint android_util_Log_v(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_log_write(env, 'V', a); }
jint android_util_Log_d(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_log_write(env, 'D', a); }
jint android_util_Log_i(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_log_write(env, 'I', a); }
jint android_util_Log_w(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_log_write(env, 'W', a); }
jint android_util_Log_e(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_log_write(env, 'E', a); }
jint android_util_Log_wtf(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_log_write(env, 'F', a); }

/* println(int priority, String tag, String msg): the priority comes first. */
jint
android_util_Log_println(JNIEnv *env, jobject object, va_list args)
{
   (void)object;
   if (!args) return 0;
   int prio = va_arg(args, jint);
   static const char lv[] = "??VDIWEF";
   return android_log_write(env, lv[(prio >= 2 && prio <= 7) ? prio : 0], args);
}

/* isLoggable(tag, level): the emulator prints every level, so say so. */
jboolean
android_util_Log_isLoggable(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
   return JNI_TRUE;
}

jstring
android_util_Log_getStackTraceString(JNIEnv *env, jobject object, va_list args)
{
   (void)object; (void)args;
   return (*env)->NewStringUTF(env, "");
}

/* ------------------------------------------------------------------------ *
 * android.opengl.GLESxx.glGetString
 *
 * Epic's ElectraTextureSample picks its bitmap-renderer path by reading
 * GL_VERSION from Java and calling String.contains() on it.  Without this the
 * call returned null and Initialize() died in the constructor with an NPE, so
 * video playback never came up at all.  The answer comes from the host GL,
 * cached while a context was current — the Java caller runs on the game thread,
 * which has none.
 * ------------------------------------------------------------------------ */

static jstring
android_gl_get_string(JNIEnv *env, va_list args)
{
   if (!args) return NULL;
   unsigned name = (unsigned)va_arg(args, jint);
   const char *s = arm_exec_gl_string(name);
   return s ? (*env)->NewStringUTF(env, s) : NULL;
}

jstring android_opengl_GLES10_glGetString(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_gl_get_string(env, a); }
jstring android_opengl_GLES20_glGetString(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_gl_get_string(env, a); }
jstring android_opengl_GLES30_glGetString(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_gl_get_string(env, a); }
jstring android_opengl_GLES31_glGetString(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_gl_get_string(env, a); }
jstring android_opengl_GLES32_glGetString(JNIEnv *env, jobject o, va_list a)
{ (void)o; return android_gl_get_string(env, a); }

/* The single answer to "which Android is this?".  See jvm.h for why there is
 * exactly one. */
int
lunaria_sdk_int(void)
{
   static int cached;
   if (!cached) {
      const char *e = getenv("LUNARIA_SDK_INT");
      long v = (e && *e) ? strtol(e, NULL, 10) : 0;
      if (v < 1 || v > 99) v = LUNARIA_SDK_INT_DEFAULT;
      cached = (int)v;
      if (cached != LUNARIA_SDK_INT_DEFAULT)
         fprintf(stderr, "[jvm] Android API level %d (LUNARIA_SDK_INT)%s\n",
                 cached,
                 cached < LUNARIA_SDK_INT_FLOOR
                    ? " — below the supported floor (API 31 / Android 12)" : "");
   }
   return cached;
}

/* Build.VERSION.RELEASE for that level.  Apps parse this string (an integer
 * major version is what every release since 4.4 actually reports), so it has
 * to agree with SDK_INT rather than being a fixed literal. */
const char *
lunaria_android_release(void)
{
   static const char *cached;
   if (cached) return cached;
   const char *e = getenv("LUNARIA_ANDROID_RELEASE");
   if (e && *e) return (cached = e);
   static const struct { int sdk; const char *release; } map[] = {
      { 27, "8.1" }, { 28, "9" },  { 29, "10" }, { 30, "11" }, { 31, "12" },
      { 32, "12" },  { 33, "13" }, { 34, "14" }, { 35, "15" }, { 36, "16" },
   };
   const int sdk = lunaria_sdk_int();
   for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i)
      if (map[i].sdk == sdk) return (cached = map[i].release);
   /* Outside the table: report the level itself rather than a version that
    * contradicts it. */
   static char buf[16];
   snprintf(buf, sizeof buf, "%d", sdk);
   return (cached = buf);
}

jstring
android_os_Build_MANUFACTURER(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->NewStringUTF(env, "berry");
}

jstring
android_os_Build_MODEL(JNIEnv *env, jobject object)
{
   return android_os_Build_MANUFACTURER(env, object);
}

jstring
android_os_Build_PRODUCT(JNIEnv *env, jobject object)
{
   return android_os_Build_MANUFACTURER(env, object);
}

jstring
android_os_Build_ID(JNIEnv *env, jobject object)
{
   return android_os_Build_MANUFACTURER(env, object);
}

jstring
android_os_Build_BRAND(JNIEnv *env, jobject object)
{
   return android_os_Build_MANUFACTURER(env, object);
}

jstring
android_os_Build_VERSION_RELEASE(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->NewStringUTF(env, lunaria_android_release());
}

jint
android_os_Build_VERSION_SDK_INT(JNIEnv *env, jobject object)
{
   assert(env && object);
   return lunaria_sdk_int();
}

jstring
android_os_Build_VERSION_INCREMENTAL(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->NewStringUTF(env, "0"); // XXX: maybe git sha of this repo
}

jstring
android_content_Context_getPackageName(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return (*env)->NewStringUTF(env, getenv("ANDROID_PACKAGE_NAME"));
}

jstring
android_content_Context_getPackageCodePath(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   const char *apk = lunaria_apk_mount_path();
   return (*env)->NewStringUTF(env, (apk && *apk) ? apk : "");
}

jstring
android_content_pm_PackageInfo_versionName(JNIEnv *env, jobject object)
{
   assert(env && object);
   /* The installed package's own version, not a literal: native code and
    * bytecode compare what they read here against the manifest. */
   const char *name = NULL;
   arm_exec_apk_version(NULL, &name);
   return (*env)->NewStringUTF(env, name ? name : "1.0");
}

jobject
android_content_Context_getExternalFilesDir(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   /* Do not va_arg(args): Call*MethodA is invoked with nullptr, and a non-null
    * pointer here is often a host jvalue[] / garbage — not a real va_list. */
   (void)args;
   static jobject sv;
   if (!sv) {
      sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/io/File"));
      const char *p = getenv("ANDROID_EXTERNAL_FILES_DIR");
      jni_file_set_path(sv, (p && *p) ? p : "/tmp");
      fprintf(stderr, "[jni_file] Context.getExternalFilesDir -> h=0x%lx path=%s\n",
              (unsigned long)(uintptr_t)sv, jni_file_get_path(sv));
   }
   return sv;
}

jobject
android_content_Context_getFilesDir(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   static jobject sv;
   if (!sv) {
      sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/io/File"));
      const char *p = getenv("ANDROID_FILES_DIR");
      if (!p || !*p)
         p = getenv("ANDROID_EXTERNAL_FILES_DIR");
      jni_file_set_path(sv, (p && *p) ? p : "/tmp");
      fprintf(stderr, "[jni_file] Context.getFilesDir -> h=0x%lx path=%s\n",
              (unsigned long)(uintptr_t)sv, jni_file_get_path(sv));
   }
   return sv;
}

jobject
android_content_Context_getApplicationInfo(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   /* Return a stub ApplicationInfo so Unity's PlayAssetDelivery path can read
    * its (empty) split-APK fields instead of dereferencing NULL. */
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/content/pm/ApplicationInfo"))));
}

/* ApplicationInfo.splitPublicSourceDirs — accessed as a field (String[]).
 * This is a non-split (mono) APK, so return an empty String array. */
jobjectArray
android_content_pm_ApplicationInfo_splitPublicSourceDirs(JNIEnv *env, jobject object)
{
   assert(env && object);
   return (*env)->NewObjectArray(env, 0, (*env)->FindClass(env, "java/lang/String"), NULL);
}

/* ApplicationInfo.sourceDir — the base APK path. */
jstring
android_content_pm_ApplicationInfo_sourceDir(JNIEnv *env, jobject object)
{
   assert(env && object);
   const char *apk = lunaria_apk_mount_path();
   return (*env)->NewStringUTF(env, (apk && *apk) ? apk : "");
}

jstring
android_content_pm_ApplicationInfo_publicSourceDir(JNIEnv *env, jobject object)
{
   return android_content_pm_ApplicationInfo_sourceDir(env, object);
}

/* ApplicationInfo.dataDir is the package's private data root (the parent of
 * Context.getFilesDir()), never an empty string. */
jstring
android_content_pm_ApplicationInfo_dataDir(JNIEnv *env, jobject object)
{
   assert(env && object);
   const char *files = getenv("ANDROID_FILES_DIR");
   char path[PATH_MAX];
   snprintf(path, sizeof path, "%s", (files && *files) ? files : "/tmp/lunaria-files");
   size_t n = strlen(path);
   if (n >= 6 && !strcmp(path + n - 6, "/files"))
      path[n - 6] = '\0';
   return (*env)->NewStringUTF(env, path);
}

jstring
android_content_pm_ApplicationInfo_nativeLibraryDir(JNIEnv *env, jobject object)
{
   assert(env && object);
   const char *path = getenv("ANDROID_NATIVE_LIB_DIR");
   return (*env)->NewStringUTF(env, (path && *path) ? path : "");
}

jobject
android_content_Context_getCacheDir(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   static jobject sv;
   if (!sv) {
      sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/io/File"));
      /* libgpg extracts an embedded classes jar under cacheDir; a File
       * without a path made "Error emptying previous jar directory". */
      const char *p = getenv("ANDROID_CACHE_DIR");
      jni_file_set_path(sv, (p && *p) ? p : "/tmp/lunaria-cache");
      mkdir("/tmp/lunaria-cache", 0755);
   }
   return sv;
}

jobject
android_content_Context_getExternalCacheDir(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   static jobject sv;
   if (!sv) {
      sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/io/File"));
      const char *p = getenv("ANDROID_EXTERNAL_CACHE_DIR");
      jni_file_set_path(sv, (p && *p) ? p : "/tmp/lunaria-ext-cache");
      mkdir("/tmp/lunaria-ext-cache", 0755);
   }
   return sv;
}

/* Context.getDir(name, mode) → File for app_<name> under the files dir.
 * libgpg uses getDir(".gpg.classloader", MODE_PRIVATE) to extract an embedded
 * classes jar; returning NULL made "Error emptying previous jar directory".
 * Prefer the arm_exec CallObjectMethod special-case (passes name); this stub
 * covers host-side calls that supply a real va_list. */
jobject
android_content_Context_getDir(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   char namebuf[256];
   snprintf(namebuf, sizeof namebuf, "%s", "dir");
   jstring js = NULL;
   if (args) {
      js = va_arg(args, jstring);
      (void)va_arg(args, jint); /* mode */
      if (js) {
         const char *utf = (*env)->GetStringUTFChars(env, js, NULL);
         if (utf && *utf)
            snprintf(namebuf, sizeof namebuf, "%s", utf);
         if (utf)
            (*env)->ReleaseStringUTFChars(env, js, utf);
      }
   }
   const char *base = getenv("ANDROID_FILES_DIR");
   if (!base || !*base)
      base = "/tmp/lunaria-files";
   mkdir(base, 0755);
   char path[PATH_MAX];
   snprintf(path, sizeof path, "%s/app_%s", base, namebuf);
   mkdir(path, 0755);
   jobject file = (*env)->AllocObject(env, (*env)->FindClass(env, "java/io/File"));
   jni_file_set_path(file, path);
   return file;
}

jstring
android_net_Uri_decode(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return NULL;
   jstring str = va_arg(args, jstring);
   (*env)->GetStringUTFChars(env, str, NULL);
   return str;
}

jstring
android_net_Uri_encode(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return NULL;
   jstring str = va_arg(args, jstring);
   (*env)->GetStringUTFChars(env, str, NULL);
   return str;
}

jstring
android_content_SharedPreferences_getString(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return NULL;
   jstring str1 = va_arg(args, jstring);
   jstring str2 = va_arg(args, jstring);
   (*env)->GetStringUTFChars(env, str1, NULL);
   (*env)->GetStringUTFChars(env, str2, NULL);
   return str2;
}

jint
android_content_SharedPreferences_getInt(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return 0;
   va_arg(args, jstring); /* key */
   return va_arg(args, jint); /* defVal */
}

jfloat
android_content_SharedPreferences_getFloat(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return 0.0f;
}

jboolean
android_content_SharedPreferences_getBoolean(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return JNI_FALSE;
}

jlong
android_content_SharedPreferences_getLong(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return 0LL;
}

jboolean
android_content_SharedPreferences_contains(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return JNI_FALSE;
}

jstring
android_os_Bundle_getString(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args) return NULL;
   jstring str1 = va_arg(args, jstring);
   (*env)->GetStringUTFChars(env, str1, NULL);
   return NULL;
}

jintArray
android_view_InputDevice_getDeviceIds(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return (*env)->NewIntArray(env, 1);
}

/* InputDevice.getDevice(int id) — static.  Unity probes the source device of
 * every injected MotionEvent to classify it (touchscreen/keyboard/…); a NULL
 * here makes it drop the event, so return a singleton InputDevice stub. */
jobject
android_view_InputDevice_getDevice(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/view/InputDevice"))));
}

jint
android_view_InputDevice_getSources(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return 0x00001002; // SOURCE_TOUCHSCREEN
}

jint
android_view_InputDevice_getSource(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return 0x00001002; // SOURCE_TOUCHSCREEN
}

jint
android_view_InputDevice_getId(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getId");
   return 0;
}

jstring
android_view_InputDevice_getName(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getName");
   return (*env)->NewStringUTF(env, "lunaria-touchscreen");
}

/* KeyCharacterMap.load(int deviceId) — static; Unity loads it for key events.
 * Return a stub object so the probe succeeds. */
jobject
android_view_KeyCharacterMap_load(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("load");
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/view/KeyCharacterMap"))));
}

jint
android_view_InputEvent_getSource(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return 0x00001002; // SOURCE_TOUCHSCREEN
}

jint
android_view_InputEvent_getDeviceId(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getDeviceId");
   return 0;
}

/* Unity 2023 resolves the InputEvent getters against the MotionEvent class
 * (GetMethodID on the event's own class), which forms these symbol names —
 * without them getSource() fell back to 0 and the touchscreen filter in
 * nativeInjectEvent rejected every injected tap. */
jint
android_view_MotionEvent_getSource(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getSource");
   return 0x00001002; // SOURCE_TOUCHSCREEN
}

/* MotionEvent is a value object: each sample is its own JVM_OBJECT_MOTION with
 * action/coords/times stored in jvm_object::motion.  Getters read that payload
 * only — never a process-global "current touch".  Unity keeps the jobject and
 * consults it again during PlayerLoop, often frames after nativeInjectEvent. */

static const lunaria_touch_event *
motion_payload(JNIEnv *env, jobject object)
{
   static lunaria_touch_event scratch;
   struct jvm *jvm = jnienv_get_jvm(env);
   return jvm && jvm_motion_event_read(jvm, object, &scratch) ? &scratch : NULL;
}

jobject
android_view_MotionEvent_obtain(JNIEnv *env, jclass clazz, va_list args)
{
   assert(env && clazz);
   motion_trace("obtain");
   struct jvm *jvm = jnienv_get_jvm(env);
   jobject src = va_arg(args, jobject);
   lunaria_touch_event ev = {0};
   if (!jvm || !jvm_motion_event_read(jvm, src, &ev))
      return NULL;
   return jvm_new_motion_event(jvm, &ev);
}

void
android_view_MotionEvent_recycle(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("recycle");
   (void)args;
}


jint
android_view_MotionEvent_getAction(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getAction");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? ev->action : 0;
}

jint
android_view_MotionEvent_getActionMasked(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getActionMasked");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? (ev->action & 0xff) : 0;
}

jint
android_view_MotionEvent_getActionIndex(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getActionIndex");
   return 0;
}

jfloat
android_view_MotionEvent_getX(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getX");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? ev->x : 0.f;
}

jfloat
android_view_MotionEvent_getY(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getY");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? ev->y : 0.f;
}

jfloat
android_view_MotionEvent_getRawX(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getRawX");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? ev->x : 0.f;
}

jfloat
android_view_MotionEvent_getRawY(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getRawY");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? ev->y : 0.f;
}

jint
android_view_MotionEvent_getPointerCount(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getPointerCount");
   return 1;
}

jint
android_view_MotionEvent_getPointerId(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getPointerId");
   return 0;
}

jlong
android_view_MotionEvent_getEventTime(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getEventTime");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? (jlong)ev->event_ms : 0;
}

jlong
android_view_MotionEvent_getDownTime(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getDownTime");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return ev ? (jlong)ev->down_ms : 0;
}

jint
android_view_MotionEvent_getMetaState(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getMetaState");
   return 0;
}

jint
android_view_MotionEvent_getDeviceId(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getDeviceId");
   return 0;
}

jfloat
android_view_MotionEvent_getPressure(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getPressure");
   const lunaria_touch_event *ev = motion_payload(env, object);
   return (ev && ev->action != 1) ? 1.0f : 0.0f;
}

jint
android_view_MotionEvent_getToolType(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getToolType");
   return 1; /* TOOL_TYPE_FINGER */
}

jint
android_view_MotionEvent_getHistorySize(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   motion_trace("getHistorySize");
   return 0; /* mouse-injected events carry no historical samples */
}

/* DisplayMetrics fields — Unity reads these to transform touch coordinates
 * into the rendering surface space.  Returning 0 (unimplemented) made every
 * touch collapse to (0,0)/get dropped. */
jint
android_util_DisplayMetrics_widthPixels(JNIEnv *env, jobject object)
{
   assert(env && object);
   return arm_exec_fb_width();
}

jint
android_util_DisplayMetrics_heightPixels(JNIEnv *env, jobject object)
{
   assert(env && object);
   return arm_exec_fb_height();
}

/* ---------------------------------------------------------------------------
 * Application startup / packaging queries.
 *
 * Unity's bootstrap (UnityPlayer.initJni → AndroidJavaClass reflection) probes
 * the launch Intent, the OBB expansion dirs and the package version on every
 * frame until they resolve.  When these were unimplemented the bridge handed
 * back wrong-typed handles, the managed code never finished init, and the
 * Boehm GC expanded its heap by 1 MB blocks without bound → std::bad_alloc.
 * Returning valid (empty) stubs lets the probe complete so init runs once.
 * ------------------------------------------------------------------------- */

/* Activity.getIntent() → a non-null Intent stub (real Activities never return
 * null here, so the caller would otherwise NPE / loop). */
jobject
android_app_Activity_getIntent(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/content/Intent"))));
}

/* Intent.getExtras() → null: this launch carries no extras, so the caller
 * skips the Bundle.containsKey/toString probing that triggered type errors. */
jobject
android_content_Intent_getExtras(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return NULL;
}

/* Context.getObbDir() → a File stub (no OBB, but a valid object). */
jobject
android_content_Context_getObbDir(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/io/File"))));
}

/* Context.getObbDirs() → an empty File[] (this is a mono APK, no expansion
 * files).  The caller expects an array object, not a scalar. */
jobjectArray
android_content_Context_getObbDirs(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   return (*env)->NewObjectArray(env, 0, (*env)->FindClass(env, "java/io/File"), NULL);
}

/* File.getAbsolutePath() → stored path for this File object. */
jstring
java_io_File_getAbsolutePath(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   return jni_file_path_string(env, object);
}

/* Context.getSystemService(name).
 *
 * The JNI A-entry bridge converts jvalue[] into a real va_list before reaching
 * host stubs, so inspect the service name and preserve Android's runtime type.
 * Returning one WindowManager for every name made casts such as
 * (InputManager)getSystemService(INPUT_SERVICE) lose the receiver at the
 * dvm/JNI boundary and fail later with a misleading null-receiver exception. */
jobject
android_content_Context_getSystemService(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (!args)
      return NULL;

   jstring service = va_arg(args, jstring);
   const char *name = service ? (*env)->GetStringUTFChars(env, service, NULL) : NULL;
   if (!name)
      return NULL;

   struct service_entry {
      const char *name;
      const char *class_name;
      jobject instance;
   };
   static struct service_entry services[] = {
      { "window",       "android/view/WindowManager", NULL },
      { "input",        "android/hardware/input/InputManager", NULL },
      { "sensor",       "android/hardware/SensorManager", NULL },
      { "connectivity", "android/net/ConnectivityManager", NULL },
      { "activity",     "android/app/ActivityManager", NULL },
      { "audio",        "android/media/AudioManager", NULL },
      { "wifi",         "android/net/wifi/WifiManager", NULL },
      { "notification", "android/app/NotificationManager", NULL },
      { "display",      "android/hardware/display/DisplayManager", NULL },
      { "power",        "android/os/PowerManager", NULL },
      { "vibrator",     "android/os/Vibrator", NULL },
      { "vibrator_manager", "android/os/VibratorManager", NULL },
      { "input_method", "android/view/inputmethod/InputMethodManager", NULL },
      { "clipboard",    "android/content/ClipboardManager", NULL },
      { "user",         "android/os/UserManager", NULL },
      { "jobscheduler", "android/app/job/JobScheduler", NULL },
   };

   jobject result = NULL;
   for (size_t i = 0; i < sizeof services / sizeof services[0]; ++i) {
      if (strcmp(name, services[i].name))
         continue;
      if (!services[i].instance) {
         jclass cls = (*env)->FindClass(env, services[i].class_name);
         if (cls)
            services[i].instance = (*env)->AllocObject(env, cls);
      }
      result = services[i].instance;
      break;
   }
   (*env)->ReleaseStringUTFChars(env, service, name);
   return result;
}

/* Context.getPackageManager() → a PackageManager stub. */
jobject
android_content_Context_getPackageManager(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/content/pm/PackageManager"))));
}

/* PackageManager.getPackageInfo(name, flags) → a PackageInfo stub whose
 * version fields (versionCode / versionName) are read below. */
jobject
android_content_pm_PackageManager_getPackageInfo(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   if (args) {
      (void)va_arg(args, jstring); /* package name */
      (void)va_arg(args, jint);    /* flags */
   }
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/content/pm/PackageInfo"))));
}

/* PackageInfo.versionCode — read as an int field. */
jint
android_content_pm_PackageInfo_versionCode(JNIEnv *env, jobject object)
{
   assert(env && object);
   int32_t code = 1;
   arm_exec_apk_version(&code, NULL);
   return code;
}

/* android.os.Process.setThreadPriority(int tid, int priority) or (int priority).
 * Just a hint; silently ignore in emulation. */
void
android_os_Process_setThreadPriority(JNIEnv *env, jclass cls, va_list args)
{
   (void)env; (void)cls; (void)args;
}

/* android.os.Environment.getExternalStorageState() → "mounted".
 * Unity checks if external storage is available before writing cache files. */
jstring
android_os_Environment_getExternalStorageState(JNIEnv *env, jclass cls, va_list args)
{
   (void)cls; (void)args;
   return (*env)->NewStringUTF(env, "mounted");
}

/* android.os.Environment.MEDIA_MOUNTED — static String field "mounted". */
jstring
android_os_Environment_MEDIA_MOUNTED(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "mounted");
}

/* ApplicationErrorReport.getErrorReportReceiver(Context, String, ApplicationErrorReport)
 * → the ComponentName of the system crash-report handler.  Unity's bootstrap
 * queries this to decide whether to forward native crashes to the platform error
 * reporter.  In headless emulation there is no such receiver, so return NULL —
 * "no report receiver" — which is the same answer a real device gives when error
 * reporting is disabled.  Returning a valid (NULL) answer stops the engine from
 * re-running the FindClass(ApplicationErrorReport)+probe dance every frame.
 *
 * Reached via GetStaticObjectField, so the bridge invokes us as (env, class); the
 * extra method-style va_list argument (if any) is ignored. */
jobject
android_app_ApplicationErrorReport_getErrorReportReceiver(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   return NULL;
}

/* bitter.jnibridge.JNIBridge.newInterfaceProxy(long ptr, Class[] interfaces).
 * Creates a Java proxy that forwards interface calls to IL2CPP managed code via
 * JNIBridge.invoke().  In headless emulation there are no real Java callers, so
 * a stub object suffices — IL2CPP will hold it as a reference but Java will never
 * actually call back through it. */
jobject
bitter_jnibridge_JNIBridge_newInterfaceProxy(JNIEnv *env, jclass cls, va_list args)
{
   (void)cls; (void)args;
   static jobject sv;
   if (!sv)
      sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/lang/Object"));
   return sv;
}

/* Activity.executeMainThreadJobs() — Unity calls this from its render thread to
 * drain Java callbacks queued on the main thread.  In our headless emulation the
 * ARM game loop IS the "main thread", so all queued work has already run. */
void
android_app_Activity_executeMainThreadJobs(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
}

/* Activity.executeGLThreadJobs() — Unity 4 stores this method ID during initJni
 * and calls it from nativeRender when GfxDevice work is pending on the GL queue.
 * Real Android: UnityPlayerActivity forwards to UnityPlayer, which polls
 * mPendingTasks and runs each Runnable.  We mirror that via arm_exec. */
void
android_app_Activity_executeGLThreadJobs(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
   arm_exec_drain_gl_thread_jobs();
}

/* Activity.kill() — Unity calls this to terminate a background service or process.
 * In emulation there is no real Android process to kill; just return. */
void
android_app_Activity_kill(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
}

/* Looper.getMainLooper() — static call; returns a stub so new Handler(looper)
 * gets a non-null argument and does not abort. */
jobject
android_os_Looper_getMainLooper(JNIEnv *env, jobject object, va_list args)
{
   (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/os/Looper"))));
}

/* Context.getSharedPreferences(String name, int mode) — PlayerPrefs storage.
 * Returns a stub; putInt/apply stubs prevent NPE on editor calls. */
jobject
android_content_Context_getSharedPreferences(JNIEnv *env, jobject object, va_list args)
{
   (void)object; (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/content/SharedPreferences"))));
}

/* Context.getResources() → Resources → Configuration.
 *
 * Both UE (FAndroidMisc locale/orientation queries) and the Netmarble SDK walk
 * Context.getResources().getConfiguration() and then read fields off the
 * result.  With getResources() unimplemented the chain yielded a NULL
 * Configuration and the very next GetObjectField aborted the process, so this
 * has to be a real object, not a missing method. */
jobject
android_content_res_Resources_getConfiguration(JNIEnv *env, jobject object, va_list args)
{
   (void)object; (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/content/res/Configuration"))));
}

void android_view_Display_fillMetrics(JNIEnv *e, jobject out); /* defined below */

jobject
android_content_res_Resources_getDisplayMetrics(JNIEnv *env, jobject object, va_list args)
{
   (void)object; (void)args;
   static jobject sv;
   if (!sv) {
      sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/util/DisplayMetrics"));
      android_view_Display_fillMetrics(env, sv);
   }
   return sv;
}

jobject
android_content_Context_getResources(JNIEnv *env, jobject object, va_list args)
{
   (void)object; (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/content/res/Resources"))));
}

/* Context.getAssets().
 *
 * There was no stub at all, so bytecode asking its own Activity for the asset
 * manager got null — Epic's GameActivity says as much on every start ("No
 * reference to asset manager found!") and every Java-side reader of a packaged
 * asset was dead before it began.  The native side has its own AAssetManager,
 * which is why this went unnoticed; the object handed back here is what the
 * VM's own AssetManager implementation hangs off. */
jobject
android_content_Context_getAssets(JNIEnv *env, jobject object, va_list args)
{
   (void)object; (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/content/res/AssetManager"))));
}

/* Configuration fields.  These are read through GetObjectField/GetIntField,
 * which resolve `<class>_<field>` the same way methods do, so a field reads as
 * a (JNIEnv*, jobject) getter. */
jobject
android_content_res_Configuration_locale(JNIEnv *env, jobject object)
{
   (void)object;
   return java_util_Locale_getDefault(env, NULL);
}

/* Configuration.ORIENTATION_PORTRAIT == 1, ORIENTATION_LANDSCAPE == 2. */
jint
android_content_res_Configuration_orientation(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   return arm_exec_fb_width() >= arm_exec_fb_height() ? 2 : 1;
}

/* SCREENLAYOUT_SIZE_NORMAL | SCREENLAYOUT_LONG_NO | SCREENLAYOUT_LAYOUTDIR_LTR */
jint android_content_res_Configuration_screenLayout(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 0x02 | 0x10 | 0x40; }
/* fillMetrics() reports mdpi, so dp == px. */
jint android_content_res_Configuration_screenWidthDp(JNIEnv *env, jobject o)
{ (void)env; (void)o; return arm_exec_fb_width(); }
jint android_content_res_Configuration_screenHeightDp(JNIEnv *env, jobject o)
{ (void)env; (void)o; return arm_exec_fb_height(); }
jint android_content_res_Configuration_smallestScreenWidthDp(JNIEnv *env, jobject o)
{
   (void)env; (void)o;
   const int w = arm_exec_fb_width(), h = arm_exec_fb_height();
   return w < h ? w : h;
}
jint android_content_res_Configuration_densityDpi(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 160; }
jfloat android_content_res_Configuration_fontScale(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 1.0f; }
/* UI_MODE_TYPE_NORMAL | UI_MODE_NIGHT_NO */
jint android_content_res_Configuration_uiMode(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 0x01 | 0x10; }
/* No SIM: mcc/mnc are 0 on a device without a mobile network. */
jint android_content_res_Configuration_mcc(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 0; }
jint android_content_res_Configuration_mnc(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 0; }
/* KEYBOARD_NOKEYS / TOUCHSCREEN_FINGER / NAVIGATION_NONAV */
jint android_content_res_Configuration_keyboard(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 1; }
jint android_content_res_Configuration_touchscreen(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 3; }
jint android_content_res_Configuration_navigation(JNIEnv *env, jobject o)
{ (void)env; (void)o; return 1; }

jobject
android_content_SharedPreferences_edit(JNIEnv *env, jobject object, va_list args)
{
   (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/content/SharedPreferences_Editor"))));
}

/* Editor.putInt / putBoolean / putString — fluent interface, return self. */
jobject android_content_SharedPreferences_Editor_putInt(JNIEnv *env, jobject object, va_list args)     { (void)args; return object; }
jobject android_content_SharedPreferences_Editor_putBoolean(JNIEnv *env, jobject object, va_list args) { (void)args; return object; }
jobject android_content_SharedPreferences_Editor_putString(JNIEnv *env, jobject object, va_list args)  { (void)args; return object; }
jobject android_content_SharedPreferences_Editor_putFloat(JNIEnv *env, jobject object, va_list args)   { (void)args; return object; }
jobject android_content_SharedPreferences_Editor_putLong(JNIEnv *env, jobject object, va_list args)    { (void)args; return object; }
jobject android_content_SharedPreferences_Editor_remove(JNIEnv *env, jobject object, va_list args)     { (void)args; return object; }

void android_content_SharedPreferences_Editor_apply(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
}

jboolean android_content_SharedPreferences_Editor_commit(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
   return 1;
}

/* Context.MODE_PRIVATE — static int constant (0).  Read via GetStaticIntField;
 * the default (0) is already correct, but provide it explicitly so the symbol
 * resolves and stops the "unimplemented symbol" probe each frame. */
jint
android_content_Context_MODE_PRIVATE(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   return 0;
}

/* SharedPreferences.getAll() → an (empty) Map stub.  Unity's PlayerPrefs
 * migration enumerates getAll().entrySet().iterator(); returning NULL made the
 * guest dispatch entrySet() on a null object → vtable load through a garbage
 * pointer → wild jump (e.g. 0x28000304).  An empty Map whose iterator reports
 * hasNext()==false lets the migration loop run zero iterations and continue. */
jobject
android_content_SharedPreferences_getAll(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object); (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "java/util/Map"))));
}

/* ApplicationInfo.minSdkVersion / targetSdkVersion — int fields read by Unity's
 * startup to log "Min/Target API Level".  When unimplemented they returned 0,
 * which Unity treats as an unconfigured app.  Report levels consistent with
 * the platform version so the engine takes its normal (modern-API) path. */
jint
android_content_pm_ApplicationInfo_minSdkVersion(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   /* Unity 2023's floor, but never above what the platform reports: an app
    * whose minSdk exceeds the device's SDK_INT could not have been installed. */
   const int sdk = lunaria_sdk_int();
   return sdk < 22 ? sdk : 22;
}

jint
android_content_pm_ApplicationInfo_targetSdkVersion(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   /* An app targeting a level the device does not have does not exist, so this
    * tracks Build.VERSION.SDK_INT. */
   return lunaria_sdk_int();
}

/* AlertDialog.Builder fluent setters — each returns the builder itself so the
 * `new Builder(ctx).setTitle(..).setMessage(..).setPositiveButton(..).show()`
 * chain keeps a valid handle instead of a NULL that the guest would then
 * dereference (wild jump).  create() yields an AlertDialog stub; show() is a
 * no-op in headless emulation. */
jobject android_app_AlertDialog_Builder_setTitle(JNIEnv *env, jobject object, va_list args)          { (void)env; (void)args; return object; }
jobject android_app_AlertDialog_Builder_setMessage(JNIEnv *env, jobject object, va_list args)        { (void)env; (void)args; return object; }
jobject android_app_AlertDialog_Builder_setPositiveButton(JNIEnv *env, jobject object, va_list args) { (void)env; (void)args; return object; }
jobject android_app_AlertDialog_Builder_setNegativeButton(JNIEnv *env, jobject object, va_list args) { (void)env; (void)args; return object; }
jobject android_app_AlertDialog_Builder_setNeutralButton(JNIEnv *env, jobject object, va_list args)  { (void)env; (void)args; return object; }
jobject android_app_AlertDialog_Builder_setCancelable(JNIEnv *env, jobject object, va_list args)     { (void)env; (void)args; return object; }
jobject android_app_AlertDialog_Builder_setIcon(JNIEnv *env, jobject object, va_list args)           { (void)env; (void)args; return object; }

jobject
android_app_AlertDialog_Builder_create(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object); (void)args;
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env, (*env)->FindClass(env, "android/app/AlertDialog"))));
}

jobject
android_app_AlertDialog_Builder_show(JNIEnv *env, jobject object, va_list args)
{
   return android_app_AlertDialog_Builder_create(env, object, args);
}

/* ---- android.content.Context static String fields ----
 * Read via GetStaticObjectField; the bridge calls these as (JNIEnv*, jobject).
 * These are service-name keys passed to getSystemService(). */
jstring
android_content_Context_POWER_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "power");
}

jstring
android_content_Context_WINDOW_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "window");
}

jstring
android_content_Context_AUDIO_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "audio");
}

jstring
android_content_Context_SENSOR_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "sensor");
}

jstring
android_content_Context_INPUT_METHOD_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "input_method");
}

jstring
android_content_Context_VIBRATOR_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "vibrator");
}

jstring
android_content_Context_CONNECTIVITY_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "connectivity");
}

jstring
android_content_Context_WIFI_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "wifi");
}

jstring
android_content_Context_ACTIVITY_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "activity");
}

jstring
android_content_Context_CLIPBOARD_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "clipboard");
}

/* ---- android.content.Context static String fields (service name keys) ----
 * Each corresponds to a Context.XYZ_SERVICE constant read via GetStaticObjectField.
 * Without these the bridge logs "FIXME: unimplemented symbol" and returns NULL,
 * causing getSystemService(null) which either NPEs or returns a broken stub. */
jstring
android_content_Context_LOCATION_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "location");
}

jstring
android_content_Context_ALARM_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "alarm");
}

jstring
android_content_Context_NOTIFICATION_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "notification");
}

jstring
android_content_Context_TELEPHONY_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "phone");
}

jstring
android_content_Context_DISPLAY_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "display");
}

jstring
android_content_Context_INPUT_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "input");
}

jstring
android_content_Context_NFC_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "nfc");
}

jstring
android_content_Context_BLUETOOTH_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "bluetooth");
}

jstring
android_content_Context_STORAGE_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "storage");
}

jstring
android_content_Context_CAMERA_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "camera");
}

jstring
android_content_Context_ACCESSIBILITY_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "accessibility");
}

jstring
android_content_Context_KEYGUARD_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "keyguard");
}

jstring
android_content_Context_SEARCH_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "search");
}

jstring
android_content_Context_DOWNLOAD_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "download");
}

jstring
android_content_Context_UI_MODE_SERVICE(JNIEnv *env, jobject obj)
{
   (void)obj;
   return (*env)->NewStringUTF(env, "uimode");
}

/* ---- android.os.PowerManager static int constants ---- */

/* PowerManager.FULL_WAKE_LOCK = 26 (0x1a): keep screen/CPU on at full brightness. */
jint
android_os_PowerManager_FULL_WAKE_LOCK(JNIEnv *env, jobject obj)
{
   (void)env; (void)obj;
   return 26; /* PowerManager.FULL_WAKE_LOCK */
}

/* PowerManager.PARTIAL_WAKE_LOCK = 1: keep CPU on but allow screen to dim. */
jint
android_os_PowerManager_PARTIAL_WAKE_LOCK(JNIEnv *env, jobject obj)
{
   (void)env; (void)obj;
   return 1;
}

/* PowerManager.SCREEN_DIM_WAKE_LOCK = 6 (deprecated). */
jint
android_os_PowerManager_SCREEN_DIM_WAKE_LOCK(JNIEnv *env, jobject obj)
{
   (void)env; (void)obj;
   return 6;
}

/* PowerManager.SCREEN_BRIGHT_WAKE_LOCK = 10 (deprecated). */
jint
android_os_PowerManager_SCREEN_BRIGHT_WAKE_LOCK(JNIEnv *env, jobject obj)
{
   (void)env; (void)obj;
   return 10;
}

/* PowerManager.ON_AFTER_RELEASE = 0x20000000: flag to keep screen on briefly. */
jint
android_os_PowerManager_ON_AFTER_RELEASE(JNIEnv *env, jobject obj)
{
   (void)env; (void)obj;
   return 0x20000000;
}

/* PowerManager.newWakeLock(int levelAndFlags, String tag) → WakeLock stub.
 * Unity acquires "Unity-StartupWakeLock" to prevent the CPU going to sleep.
 * In headless emulation there is no real power management; return a stub that
 * absorbs acquire()/release()/setReferenceCounted() without crashing. */
jobject
android_os_PowerManager_newWakeLock(JNIEnv *env, jobject object, va_list args)
{
   (void)object;
   if (args) {
      (void)va_arg(args, jint);    /* levelAndFlags */
      (void)va_arg(args, jstring); /* tag */
   }
   static jobject sv;
   return (sv ? sv : (sv = (*env)->AllocObject(env,
               (*env)->FindClass(env, "android/os/PowerManager$WakeLock"))));
}

/* ---- android.os.PowerManager$WakeLock methods ---- */
void android_os_PowerManager_WakeLock_acquire(JNIEnv *env, jobject obj, va_list args)          { (void)env; (void)obj; (void)args; }
void android_os_PowerManager_WakeLock_release(JNIEnv *env, jobject obj, va_list args)          { (void)env; (void)obj; (void)args; }
void android_os_PowerManager_WakeLock_setReferenceCounted(JNIEnv *env, jobject obj, va_list a) { (void)env; (void)obj; (void)a; }
jboolean android_os_PowerManager_WakeLock_isHeld(JNIEnv *env, jobject obj, va_list args)       { (void)env; (void)obj; (void)args; return 0; }

/* ---- android.content.pm.PackageManager static int constants ---- */

/* PackageManager.PERMISSION_GRANTED = 0 */
jint
android_content_pm_PackageManager_PERMISSION_GRANTED(JNIEnv *env, jobject obj)
{
   (void)env; (void)obj;
   return 0;
}

/* PackageManager.PERMISSION_DENIED = -1 */
jint
android_content_pm_PackageManager_PERMISSION_DENIED(JNIEnv *env, jobject obj)
{
   (void)env; (void)obj;
   return -1;
}

/* Context.checkCallingOrSelfPermission(String permission) → PERMISSION_GRANTED.
 * In headless emulation all permissions are granted; returning 0 lets the engine
 * proceed without triggering permission-denied error paths. */
jint
android_content_Context_checkCallingOrSelfPermission(JNIEnv *env, jobject obj, va_list args)
{
   (void)env; (void)obj; (void)args;
   return 0; /* PERMISSION_GRANTED */
}

jint
android_content_Context_checkPermission(JNIEnv *env, jobject obj, va_list args)
{
   (void)env; (void)obj; (void)args;
   return 0; /* PERMISSION_GRANTED */
}

jint
android_content_Context_checkSelfPermission(JNIEnv *env, jobject obj, va_list args)
{
   (void)env; (void)obj; (void)args;
   return 0; /* PERMISSION_GRANTED */
}

/* ---- android.app.Activity / java.lang.Object / java.lang.Class aliases ----
 * The JVM bridge resolves method symbols by prepending the runtime class name.
 * Activity extends Context, so getPackageName/getSystemService are inherited;
 * the bridge looks them up as android_app_Activity_* (and as the Object/Class
 * fallbacks when the class hierarchy isn't fully modelled).  Forward each alias
 * to the canonical android_content_Context_* implementation. */

jstring android_app_Activity_getPackageName(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getPackageName(env, obj, args); }
jstring java_lang_Object_getPackageName(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getPackageName(env, obj, args); }
jstring java_lang_Class_getPackageName(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getPackageName(env, obj, args); }

jobject android_app_Activity_getSystemService(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getSystemService(env, obj, args); }
jobject java_lang_Object_getSystemService(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getSystemService(env, obj, args); }
jobject java_lang_Class_getSystemService(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getSystemService(env, obj, args); }

/* getResources() is inherited the same way.  Without these aliases the dex
 * bytecode's `Landroid/app/Activity;->getResources()` resolved to nothing and
 * handed back null, so GameActivity.AndroidThunkJava_GetDeviceOrientation threw
 * an NPE on Resources.getConfiguration and the engine lost its orientation. */
jobject android_app_Activity_getResources(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getResources(env, obj, args); }
jobject java_lang_Object_getResources(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getResources(env, obj, args); }
jobject java_lang_Class_getResources(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getResources(env, obj, args); }

/* …and so is getAssets(): the call site names Activity or NativeActivity. */
jobject android_app_Activity_getAssets(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getAssets(env, obj, args); }
jobject java_lang_Object_getAssets(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getAssets(env, obj, args); }
jobject java_lang_Class_getAssets(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getAssets(env, obj, args); }

/* Context.registerReceiver(BroadcastReceiver, IntentFilter[, …]) → null.
 * UE's Volume/Battery/HeadsetReceiver.startReceiver calls this; without a
 * stub the dvm path reports an unresolved method.  Returning null matches
 * "no sticky broadcast" and is enough for those receivers to finish.
 * A null receiver is tolerated: startReceiver may be invoked before the
 * platform Activity binding is visible to the caller. */
jobject
android_content_Context_registerReceiver(JNIEnv *env, jobject object, va_list args)
{
   (void)args;
   if (!env || !object) return NULL;
   return NULL;
}
jobject android_app_Activity_registerReceiver(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_registerReceiver(env, obj, args); }
jobject java_lang_Object_registerReceiver(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_registerReceiver(env, obj, args); }
jobject java_lang_Class_registerReceiver(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_registerReceiver(env, obj, args); }

jobject android_content_Context_getDir(JNIEnv *env, jobject obj, va_list args);
jobject android_app_Activity_getDir(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getDir(env, obj, args); }
jobject com_unity3d_player_UnityPlayer_getDir(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getDir(env, obj, args); }
jobject java_lang_Object_getDir(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getDir(env, obj, args); }
jobject java_lang_Class_getDir(JNIEnv *env, jobject obj, va_list args)
{ return android_content_Context_getDir(env, obj, args); }

jobject java_lang_Class_getClassLoader(JNIEnv *env, jobject object);
jobject android_content_Context_getClassLoader(JNIEnv *env, jobject obj, va_list args)
{ (void)args; return java_lang_Class_getClassLoader(env, obj); }
jobject android_app_Activity_getClassLoader(JNIEnv *env, jobject obj, va_list args)
{ (void)args; return java_lang_Class_getClassLoader(env, obj); }
jobject com_unity3d_player_UnityPlayer_getClassLoader(JNIEnv *env, jobject obj, va_list args)
{ (void)args; return java_lang_Class_getClassLoader(env, obj); }

/* ActivityManager.MemoryInfo.lowMemory — public boolean field read by Unity to
 * decide whether to trigger aggressive GC.  In headless emulation report false
 * so the engine takes its normal (non-low-memory) code path. */
jboolean android_app_ActivityManager_MemoryInfo_lowMemory(JNIEnv *env, jobject obj)
{ (void)env; (void)obj; return 0; }
jboolean java_lang_Object_lowMemory(JNIEnv *env, jobject obj)
{ (void)env; (void)obj; return 0; }
jboolean java_lang_Class_lowMemory(JNIEnv *env, jobject obj)
{ (void)env; (void)obj; return 0; }

/* ActivityManager.getMemoryInfo(ActivityManager.MemoryInfo outInfo) — void method.
 * Unity queries this to size Dynamic Heap / trim caches.  Profile comes from
 * arm_exec.h (LUNARIA_MEM_TOTAL_MB default 6144 = 6 GiB phone-class), kept in
 * sync with synthetic /proc/meminfo — never the host sysinfo().
 *
 * Field IDs are looked up once and cached. */
void
android_app_ActivityManager_getMemoryInfo(JNIEnv *env, jobject obj, va_list args)
{
   (void)obj;

   jobject out_info = NULL;
   if (args)
      out_info = va_arg(args, jobject);
   if (!out_info)
      return;

   jlong total_bytes = (jlong)lunaria_mem_total_bytes();
   jlong avail_bytes = (jlong)lunaria_mem_avail_bytes();
   jlong threshold   = (jlong)lunaria_mem_threshold_bytes();
   jboolean low = (avail_bytes < threshold) ? JNI_TRUE : JNI_FALSE;

   static jfieldID fid_avail   = NULL;
   static jfieldID fid_total   = NULL;
   static jfieldID fid_low     = NULL;
   static jfieldID fid_thresh  = NULL;
   static int logged = 0;

   if (!fid_avail) {
      jclass cls = (*env)->FindClass(env, "android/app/ActivityManager$MemoryInfo");
      if (!cls) return;
      fid_avail  = (*env)->GetFieldID(env, cls, "availMem",  "J");
      fid_total  = (*env)->GetFieldID(env, cls, "totalMem",  "J");
      fid_low    = (*env)->GetFieldID(env, cls, "lowMemory", "Z");
      fid_thresh = (*env)->GetFieldID(env, cls, "threshold", "J");
      if (!fid_avail || !fid_total || !fid_low || !fid_thresh) return;
   }

   if (!logged) {
      logged = 1;
      fprintf(stderr, "[mem] getMemoryInfo total=%ldMB avail=%ldMB threshold=%ldMB "
              "(LUNARIA_MEM_TOTAL_MB / LUNARIA_MEM_AVAIL_MB)\n",
              (long)(total_bytes >> 20), (long)(avail_bytes >> 20),
              (long)(threshold >> 20));
   }

   (*env)->SetLongField   (env, out_info, fid_avail,  avail_bytes);
   (*env)->SetLongField   (env, out_info, fid_total,  total_bytes);
   (*env)->SetBooleanField(env, out_info, fid_low,    low);
   (*env)->SetLongField   (env, out_info, fid_thresh, threshold);
}

/* Bridge alias: the JVM bridge resolves by prepending the runtime class name.
 * When the receiver is typed as Object or Class (base of the hierarchy) the
 * bridge looks up java_lang_Object_getMemoryInfo / java_lang_Class_getMemoryInfo
 * before the concrete android_app_ActivityManager_* name. */
void java_lang_Object_getMemoryInfo(JNIEnv *env, jobject obj, va_list args)
{ android_app_ActivityManager_getMemoryInfo(env, obj, args); }
void java_lang_Class_getMemoryInfo(JNIEnv *env, jobject obj, va_list args)
{ android_app_ActivityManager_getMemoryInfo(env, obj, args); }

/* java.lang.StringBuilder is implemented in libjvm.so (jvm.c) so that
 * dlsym(RTLD_DEFAULT, …) always resolves within the same DSO. */

/* ---- android.content.pm.ActivityInfo screen orientation constants ---- */
/* Android SDK values: UNSPECIFIED=-1 LANDSCAPE=0 PORTRAIT=1 REVERSE_LANDSCAPE=8
 * REVERSE_PORTRAIT=9 FULL_SENSOR=10 */
#define DEF_ORIENT(name, val) \
   jint android_content_pm_ActivityInfo_##name(JNIEnv *e, jobject o) \
   { (void)e; (void)o; return (val); } \
   jint java_lang_Object_##name(JNIEnv *e, jobject o) \
   { (void)e; (void)o; return (val); } \
   jint java_lang_Class_##name(JNIEnv *e, jobject o) \
   { (void)e; (void)o; return (val); }

DEF_ORIENT(SCREEN_ORIENTATION_UNSPECIFIED,     -1)
DEF_ORIENT(SCREEN_ORIENTATION_LANDSCAPE,        0)
DEF_ORIENT(SCREEN_ORIENTATION_PORTRAIT,         1)
DEF_ORIENT(SCREEN_ORIENTATION_USER,             2)
DEF_ORIENT(SCREEN_ORIENTATION_BEHIND,           3)
DEF_ORIENT(SCREEN_ORIENTATION_SENSOR,           4)
DEF_ORIENT(SCREEN_ORIENTATION_NOSENSOR,         5)
DEF_ORIENT(SCREEN_ORIENTATION_SENSOR_LANDSCAPE, 6)
DEF_ORIENT(SCREEN_ORIENTATION_SENSOR_PORTRAIT,  7)
DEF_ORIENT(SCREEN_ORIENTATION_REVERSE_LANDSCAPE, 8)
DEF_ORIENT(SCREEN_ORIENTATION_REVERSE_PORTRAIT,  9)
DEF_ORIENT(SCREEN_ORIENTATION_FULL_SENSOR,      10)
DEF_ORIENT(SCREEN_ORIENTATION_USER_LANDSCAPE,   11)
DEF_ORIENT(SCREEN_ORIENTATION_USER_PORTRAIT,    12)
DEF_ORIENT(SCREEN_ORIENTATION_FULL_USER,        13)
DEF_ORIENT(SCREEN_ORIENTATION_LOCKED,           14)
#undef DEF_ORIENT

/* Activity orientation is process-local Android framework state.  Seed each
 * Activity from its manifest value, then preserve setRequestedOrientation()
 * calls instead of returning a title-specific constant. */
#define JNI_ACTIVITY_MAX 65536u
static int16_t g_activity_orientation[JNI_ACTIVITY_MAX];
static bool g_activity_orientation_initialized;

static jint
activity_manifest_orientation(void)
{
   const char *value = getenv("ANDROID_SCREEN_ORIENTATION");
   if (value && *value)
      return (jint)strtol(value, NULL, 10);
   return arm_exec_fb_width() >= arm_exec_fb_height()
      ? 0 /* SCREEN_ORIENTATION_LANDSCAPE */
      : 1 /* SCREEN_ORIENTATION_PORTRAIT */;
}

static void
activity_orientation_init(void)
{
   if (g_activity_orientation_initialized)
      return;
   for (size_t i = 0; i < JNI_ACTIVITY_MAX; ++i)
      g_activity_orientation[i] = INT16_MIN;
   g_activity_orientation_initialized = true;
}

static void
activity_set_requested_orientation(jobject activity, jint orientation)
{
   activity_orientation_init();
   const uintptr_t index = (uintptr_t)activity;
   if (index < JNI_ACTIVITY_MAX)
      g_activity_orientation[index] = (int16_t)orientation;
}

static jint
activity_get_requested_orientation(jobject activity)
{
   activity_orientation_init();
   const uintptr_t index = (uintptr_t)activity;
   jint orientation = activity_manifest_orientation();
   if (index < JNI_ACTIVITY_MAX && g_activity_orientation[index] != INT16_MIN)
      orientation = g_activity_orientation[index];
   return orientation;
}

void android_app_Activity_setRequestedOrientation(JNIEnv *e, jobject o, va_list a)
{ (void)e; activity_set_requested_orientation(o, va_arg(a, jint)); }
void java_lang_Object_setRequestedOrientation(JNIEnv *e, jobject o, va_list a)
{ android_app_Activity_setRequestedOrientation(e, o, a); }
void java_lang_Class_setRequestedOrientation(JNIEnv *e, jobject o, va_list a)
{ android_app_Activity_setRequestedOrientation(e, o, a); }

jint android_app_Activity_getRequestedOrientation(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)a; return activity_get_requested_orientation(o); }
jint java_lang_Object_getRequestedOrientation(JNIEnv *e, jobject o, va_list a)
{ return android_app_Activity_getRequestedOrientation(e, o, a); }
jint java_lang_Class_getRequestedOrientation(JNIEnv *e, jobject o, va_list a)
{ return android_app_Activity_getRequestedOrientation(e, o, a); }

/* ---- android.os.Build string fields ---- */
jstring android_os_Build_DEVICE(JNIEnv *e, jobject o)
{ (void)o; return (*e)->NewStringUTF(e, "berry_device"); }
jstring java_lang_Object_DEVICE(JNIEnv *e, jobject o)
{ return android_os_Build_DEVICE(e, o); }
jstring java_lang_Class_DEVICE(JNIEnv *e, jobject o)
{ return android_os_Build_DEVICE(e, o); }

jstring android_os_Build_DISPLAY(JNIEnv *e, jobject o)
{ (void)o; return (*e)->NewStringUTF(e, "berry_display"); }
jstring java_lang_Object_DISPLAY(JNIEnv *e, jobject o)
{ return android_os_Build_DISPLAY(e, o); }
jstring java_lang_Class_DISPLAY(JNIEnv *e, jobject o)
{ return android_os_Build_DISPLAY(e, o); }

jstring android_os_Build_HARDWARE(JNIEnv *e, jobject o)
{ (void)o; return (*e)->NewStringUTF(e, "berry_hw"); }
jstring java_lang_Object_HARDWARE(JNIEnv *e, jobject o)
{ return android_os_Build_HARDWARE(e, o); }
jstring java_lang_Class_HARDWARE(JNIEnv *e, jobject o)
{ return android_os_Build_HARDWARE(e, o); }

jstring android_os_Build_SERIAL(JNIEnv *e, jobject o)
{ (void)o; return (*e)->NewStringUTF(e, "berry000"); }
jstring java_lang_Object_SERIAL(JNIEnv *e, jobject o)
{ return android_os_Build_SERIAL(e, o); }
jstring java_lang_Class_SERIAL(JNIEnv *e, jobject o)
{ return android_os_Build_SERIAL(e, o); }

/* ---- android.provider.Settings.Secure ---- */
/* ANDROID_ID: the constant string key "android_id", not the value itself */
jstring android_provider_Settings_Secure_ANDROID_ID(JNIEnv *e, jobject o)
{ (void)o; return (*e)->NewStringUTF(e, "android_id"); }
jstring java_lang_Object_ANDROID_ID(JNIEnv *e, jobject o)
{ return android_provider_Settings_Secure_ANDROID_ID(e, o); }
jstring java_lang_Class_ANDROID_ID(JNIEnv *e, jobject o)
{ return android_provider_Settings_Secure_ANDROID_ID(e, o); }

/* Settings.Secure.getString(ContentResolver cr, String name) → fake device ID */
jstring
android_provider_Settings_Secure_getString(JNIEnv *e, jobject o, va_list a)
{
   (void)o; (void)a;
   return (*e)->NewStringUTF(e, "berry000000000000");
}

/* ---- android.telephony.TelephonyManager ---- */
jstring
android_telephony_TelephonyManager_getDeviceId(JNIEnv *e, jobject o, va_list a)
{
   (void)o; (void)a;
   return (*e)->NewStringUTF(e, "000000000000000");
}
jstring java_lang_Object_getDeviceId(JNIEnv *e, jobject o, va_list a)
{ return android_telephony_TelephonyManager_getDeviceId(e, o, a); }
jstring java_lang_Class_getDeviceId(JNIEnv *e, jobject o, va_list a)
{ return android_telephony_TelephonyManager_getDeviceId(e, o, a); }

/* Context.getAttributionTag() — API 30+.  Play services reflects for it and
 * calls straight through when the platform level says it exists, so an
 * emulator reporting API 30+ has to answer.  null is what a context with no
 * attribution tag returns, which is every context unless the app sets one. */
jstring
android_content_Context_getAttributionTag(JNIEnv *env, jobject object, va_list args)
{ (void)env; (void)object; (void)args; return NULL; }
jstring java_lang_Object_getAttributionTag(JNIEnv *e, jobject o, va_list a)
{ return android_content_Context_getAttributionTag(e, o, a); }
jstring java_lang_Class_getAttributionTag(JNIEnv *e, jobject o, va_list a)
{ return android_content_Context_getAttributionTag(e, o, a); }
jstring android_app_Activity_getAttributionTag(JNIEnv *e, jobject o, va_list a)
{ return android_content_Context_getAttributionTag(e, o, a); }

/* ---- android.content.Context.getContentResolver() → stub ---- */
jobject
android_content_Context_getContentResolver(JNIEnv *e, jobject o, va_list a)
{
   (void)a;
   static jobject sv;
   return (sv ? sv : (sv = (*e)->AllocObject(e,
      (*e)->FindClass(e, "android/content/ContentResolver"))));
}
jobject java_lang_Object_getContentResolver(JNIEnv *e, jobject o, va_list a)
{ return android_content_Context_getContentResolver(e, o, a); }
jobject java_lang_Class_getContentResolver(JNIEnv *e, jobject o, va_list a)
{ return android_content_Context_getContentResolver(e, o, a); }

/* ---- android.hardware.display.DisplayManager.getDisplay(int) → stub ---- */
jobject
android_hardware_display_DisplayManager_getDisplay(JNIEnv *e, jobject o, va_list a)
{
   (void)a;
   static jobject sv;
   return (sv ? sv : (sv = (*e)->AllocObject(e,
      (*e)->FindClass(e, "android/view/Display"))));
}
jobject java_lang_Object_getDisplay(JNIEnv *e, jobject o, va_list a)
{ return android_hardware_display_DisplayManager_getDisplay(e, o, a); }
jobject java_lang_Class_getDisplay(JNIEnv *e, jobject o, va_list a)
{ return android_hardware_display_DisplayManager_getDisplay(e, o, a); }

/* Activity.getWindowManager() → WindowManager stub.
 * UE's AndroidThunkJava_GetDeviceOrientation does:
 *   getWindowManager().getDefaultDisplay().getRotation()
 * (not getSystemService — that path is a red herring). */
jobject
android_app_Activity_getWindowManager(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   static jobject sv;
   if (!sv) {
      jclass cls = (*env)->FindClass(env, "android/view/WindowManager");
      if (!cls)
         cls = (*env)->FindClass(env, "java/lang/Object");
      if (cls)
         sv = (*env)->AllocObject(env, cls);
   }
   return sv;
}
jobject java_lang_Object_getWindowManager(JNIEnv *env, jobject obj, va_list args)
{ return android_app_Activity_getWindowManager(env, obj, args); }
jobject java_lang_Class_getWindowManager(JNIEnv *env, jobject obj, va_list args)
{ return android_app_Activity_getWindowManager(env, obj, args); }
jobject com_epicgames_ue4_GameActivity_getWindowManager(JNIEnv *env, jobject obj, va_list args)
{ return android_app_Activity_getWindowManager(env, obj, args); }

/* Activity.getWindow() → Window stub.
 * AndroidThunkJava_KeepScreenOn does runOnUiThread(() -> getWindow().addFlags/
 * clearFlags(FLAG_KEEP_SCREEN_ON)).  Without a Window the dex path NPEs inside
 * the anonymous Runnable and again in the thunk itself. */
jobject
android_app_Activity_getWindow(JNIEnv *env, jobject object, va_list args)
{
   assert(env && object);
   (void)args;
   static jobject sv;
   if (!sv) {
      jclass cls = (*env)->FindClass(env, "android/view/Window");
      if (!cls)
         cls = (*env)->FindClass(env, "java/lang/Object");
      if (cls)
         sv = (*env)->AllocObject(env, cls);
   }
   return sv;
}
jobject java_lang_Object_getWindow(JNIEnv *env, jobject obj, va_list args)
{ return android_app_Activity_getWindow(env, obj, args); }
jobject java_lang_Class_getWindow(JNIEnv *env, jobject obj, va_list args)
{ return android_app_Activity_getWindow(env, obj, args); }
jobject com_epicgames_ue4_GameActivity_getWindow(JNIEnv *env, jobject obj, va_list args)
{ return android_app_Activity_getWindow(env, obj, args); }

/* Window.addFlags / clearFlags — KeepScreenOn only; no host window attrs. */
#define DEF_WINDOW_FLAGS(name) \
   void android_view_Window_##name(JNIEnv *e, jobject o, va_list a) \
   { (void)e; (void)o; (void)a; } \
   void java_lang_Object_##name(JNIEnv *e, jobject o, va_list a) \
   { (void)e; (void)o; (void)a; } \
   void java_lang_Class_##name(JNIEnv *e, jobject o, va_list a) \
   { (void)e; (void)o; (void)a; } \
   void android_view_PhoneWindow_##name(JNIEnv *e, jobject o, va_list a) \
   { (void)e; (void)o; (void)a; }
DEF_WINDOW_FLAGS(addFlags)
DEF_WINDOW_FLAGS(clearFlags)
#undef DEF_WINDOW_FLAGS

/* WindowManager.getDefaultDisplay() — UE's AndroidThunkJava_GetDeviceOrientation
 * does getWindowManager().getDefaultDisplay().getRotation().  Without
 * this the dex path NPEs every frame after the first draw. */
jobject
android_view_WindowManager_getDefaultDisplay(JNIEnv *e, jobject o, va_list a)
{
   (void)o; (void)a;
   return android_hardware_display_DisplayManager_getDisplay(e, o, a);
}
jobject java_lang_Object_getDefaultDisplay(JNIEnv *e, jobject o, va_list a)
{ return android_view_WindowManager_getDefaultDisplay(e, o, a); }
jobject java_lang_Class_getDefaultDisplay(JNIEnv *e, jobject o, va_list a)
{ return android_view_WindowManager_getDefaultDisplay(e, o, a); }
jobject android_view_WindowManagerImpl_getDefaultDisplay(JNIEnv *e, jobject o, va_list a)
{ return android_view_WindowManager_getDefaultDisplay(e, o, a); }

/* ---- android.Manifest.permission.READ_PHONE_STATE ---- */
jstring android_Manifest_permission_READ_PHONE_STATE(JNIEnv *e, jobject o)
{ (void)o; return (*e)->NewStringUTF(e, "android.permission.READ_PHONE_STATE"); }
jstring java_lang_Object_READ_PHONE_STATE(JNIEnv *e, jobject o)
{ return android_Manifest_permission_READ_PHONE_STATE(e, o); }
jstring java_lang_Class_READ_PHONE_STATE(JNIEnv *e, jobject o)
{ return android_Manifest_permission_READ_PHONE_STATE(e, o); }

/* ---- java.util.HashMap.put(K,V) → returns null (new key) ---- */
jobject
java_util_HashMap_put(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return NULL; }
jobject java_lang_Object_put(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return NULL; }
jobject java_lang_Class_put(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return NULL; }

/* Activity.isFinishing() — Netmarble cancelNotification / checkActivity.
 * Without a host stub the DVM reports a miss and treats the call as falsey
 * only by accident; return JNI_FALSE so the activity looks alive. */
jboolean android_app_Activity_isFinishing(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return JNI_FALSE; }
jboolean com_epicgames_ue4_GameActivity_isFinishing(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return JNI_FALSE; }
jboolean java_lang_Object_isFinishing(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return JNI_FALSE; }

/* Context.getApplicationContext() — return the Activity/Context itself.
 * Enough for SDK checks that only need a non-null Context; GameActivity is
 * looked up under both the android.app and com.epicgames.ue4 names. */
jobject android_content_Context_getApplicationContext(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)a; return o; }
jobject android_app_Activity_getApplicationContext(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)a; return o; }
jobject com_epicgames_ue4_GameActivity_getApplicationContext(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)a; return o; }
jobject java_lang_Object_getApplicationContext(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)a; return o; }

/* GameActivity.AndroidThunkJava_GetAndroidId() — UE FAndroidMisc::GetDeviceId.
 * Blade & Soul CDN URL (CBCdnConfigManager::SetConfingURL) appends this; without
 * a host stub CallObjectMethod returns null and the request path can stall
 * before curl ever calls getaddrinfo. */
jstring
com_epicgames_ue4_GameActivity_AndroidThunkJava_GetAndroidId(JNIEnv *e, jobject o, va_list a)
{
   (void)o; (void)a;
   static int once;
   if (!once) {
      once = 1;
      fprintf(stderr, "[arm_jni] AndroidThunkJava_GetAndroidId -> berry000000000000\n");
   }
   return (*e)->NewStringUTF(e, "berry000000000000");
}
jstring java_lang_Object_AndroidThunkJava_GetAndroidId(JNIEnv *e, jobject o, va_list a)
{ return com_epicgames_ue4_GameActivity_AndroidThunkJava_GetAndroidId(e, o, a); }

/* GameActivity.AndroidThunkJava_GetNetworkConnectionType() — UE ENetworkConnectionType.
 * WiFi=3 (None=0, Airplane=1, Cell=2, WiFi=3).  Dex bytecode walks
 * ConnectivityManager; stub short-circuits so patch/CDN does not see offline. */
jint
com_epicgames_ue4_GameActivity_AndroidThunkJava_GetNetworkConnectionType(JNIEnv *e, jobject o, va_list a)
{
   (void)e; (void)o; (void)a;
   static int once;
   if (!once) {
      once = 1;
      fprintf(stderr, "[arm_jni] AndroidThunkJava_GetNetworkConnectionType -> WiFi(3)\n");
   }
   return 3; /* ENetworkConnectionType::WiFi */
}
jint java_lang_Object_AndroidThunkJava_GetNetworkConnectionType(JNIEnv *e, jobject o, va_list a)
{ return com_epicgames_ue4_GameActivity_AndroidThunkJava_GetNetworkConnectionType(e, o, a); }

/* ConnectivityManager / NetworkInfo — used if dex GetNetworkConnectionType runs. */
jobject
android_net_ConnectivityManager_getActiveNetworkInfo(JNIEnv *e, jobject o, va_list a)
{
   (void)o; (void)a;
   static jobject sv;
   if (!sv) {
      jclass cls = (*e)->FindClass(e, "android/net/NetworkInfo");
      if (!cls) cls = (*e)->FindClass(e, "java/lang/Object");
      if (cls) sv = (*e)->AllocObject(e, cls);
   }
   return sv;
}
jobject java_lang_Object_getActiveNetworkInfo(JNIEnv *e, jobject o, va_list a)
{ return android_net_ConnectivityManager_getActiveNetworkInfo(e, o, a); }
jobject java_lang_Class_getActiveNetworkInfo(JNIEnv *e, jobject o, va_list a)
{ return android_net_ConnectivityManager_getActiveNetworkInfo(e, o, a); }

jboolean android_net_NetworkInfo_isConnected(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return JNI_TRUE; }

jboolean android_net_NetworkInfo_isAvailable(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return JNI_TRUE; }

/* android.net.ConnectivityManager.TYPE_WIFI == 1 */
jint android_net_NetworkInfo_getType(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 1; }

/* ---- Activity.getSplashMode() → 0 (no splash) ---- */
jint android_app_Activity_getSplashMode(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }
jint java_lang_Object_getSplashMode(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }
jint java_lang_Class_getSplashMode(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }

/* ---- Activity.triggerResizeCall() → no-op ---- */
void android_app_Activity_triggerResizeCall(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; }
void java_lang_Object_triggerResizeCall(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; }
void java_lang_Class_triggerResizeCall(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; }

/* ---- Activity UI stubs ---- */
#define DEF_VOID3(name) \
   void android_app_Activity_##name(JNIEnv *e, jobject o, va_list a) \
   { (void)e; (void)o; (void)a; } \
   void java_lang_Object_##name(JNIEnv *e, jobject o, va_list a) \
   { (void)e; (void)o; (void)a; } \
   void java_lang_Class_##name(JNIEnv *e, jobject o, va_list a) \
   { (void)e; (void)o; (void)a; }
DEF_VOID3(hideSoftInput)
DEF_VOID3(startActivityIndicator)
DEF_VOID3(stopActivityIndicator)
/* runOnUiThread(Runnable): no-op at the stub layer.  When dvm is active the
 * bytecode emulator's hook_call_external runs the Runnable inline instead. */
DEF_VOID3(runOnUiThread)
#undef DEF_VOID3

/* ---- android.view.Display ---- */
jint android_view_Display_getDisplayId(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }
jint java_lang_Object_getDisplayId(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }
jint java_lang_Class_getDisplayId(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }

jint android_view_Display_getWidth(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return arm_exec_fb_width(); }
jint java_lang_Object_getWidth(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return arm_exec_fb_width(); }
jint java_lang_Class_getWidth(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return arm_exec_fb_width(); }

jint android_view_Display_getHeight(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return arm_exec_fb_height(); }
jint java_lang_Object_getHeight(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return arm_exec_fb_height(); }
jint java_lang_Class_getHeight(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return arm_exec_fb_height(); }

jint android_view_Display_getRotation(JNIEnv *e, jobject o, va_list a)
{
   (void)e; (void)o; (void)a;
   /* The emulated device's natural orientation is portrait.  Rotation and
    * DisplayMetrics must describe the same current display: UE polls this
    * every frame and swaps its render dimensions when they disagree. */
   return arm_exec_fb_width() > arm_exec_fb_height()
      ? 1 /* ROTATION_90 */ : 0 /* ROTATION_0 */;
}
jint java_lang_Object_getRotation(JNIEnv *e, jobject o, va_list a)
{ return android_view_Display_getRotation(e, o, a); }
jint java_lang_Class_getRotation(JNIEnv *e, jobject o, va_list a)
{ return android_view_Display_getRotation(e, o, a); }

/* Display refresh timing.  Frame pacers (Google's Swappy, which UE links as
 * libswappy.so) derive the vsync period as 1e9 / getRefreshRate(); with the
 * method unimplemented the rate came back 0, the period became infinite and the
 * pacer never decided a frame was due — the RHI thread parked inside
 * SwappyGL_swap forever.  Report the 60 Hz the emulator actually presents at,
 * with the offsets a typical device reports. */
#define LUNA_REFRESH_HZ 60.0f
#define LUNA_REFRESH_NS 16666667LL

jfloat android_view_Display_getRefreshRate(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return LUNA_REFRESH_HZ; }
jfloat java_lang_Object_getRefreshRate(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return LUNA_REFRESH_HZ; }
jfloat java_lang_Class_getRefreshRate(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return LUNA_REFRESH_HZ; }

/* Time from vsync to when the app's buffer is sampled; 0 is a valid answer and
 * is what devices without a measured offset report. */
jlong android_view_Display_getAppVsyncOffsetNanos(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }
jlong java_lang_Object_getAppVsyncOffsetNanos(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }
jlong java_lang_Class_getAppVsyncOffsetNanos(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; }

/* Latest a buffer may be queued and still make the next vsync: one frame minus
 * the compositor's slack. */
jlong android_view_Display_getPresentationDeadlineNanos(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return LUNA_REFRESH_NS + 1000000LL; }
jlong java_lang_Object_getPresentationDeadlineNanos(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return LUNA_REFRESH_NS + 1000000LL; }
jlong java_lang_Class_getPresentationDeadlineNanos(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return LUNA_REFRESH_NS + 1000000LL; }

/* Display.getRealMetrics(DisplayMetrics outMetrics) — populate w/h/density.
 * Unity reads widthPixels/heightPixels/xdpi/ydpi/density from the outMetrics
 * object to set up its screen metrics.  CallVoidMethod from the ARM bridge
 * often passes a NULL args pointer, so arm_exec also calls this helper with
 * the outMetrics jobject read from guest registers. */
void
android_view_Display_fillMetrics(JNIEnv *e, jobject out)
{
   if (!e || !out) return;
   jclass cls = (*e)->FindClass(e, "android/util/DisplayMetrics");
   if (!cls) return;
   int w = arm_exec_fb_width(), h = arm_exec_fb_height();
   /* mdpi (1.0): Time Locker's world-space START/tutorial quads were vertically
    * crushed at density=2.0 (layout in 360dp while meshes assume full pixels). */
   float density = 1.0f;
   float dpi = 160.0f * density;
   jfieldID fw = (*e)->GetFieldID(e, cls, "widthPixels",   "I");
   jfieldID fh = (*e)->GetFieldID(e, cls, "heightPixels",  "I");
   jfieldID fd = (*e)->GetFieldID(e, cls, "density",       "F");
   jfieldID fs = (*e)->GetFieldID(e, cls, "scaledDensity", "F");
   jfieldID fx = (*e)->GetFieldID(e, cls, "xdpi",          "F");
   jfieldID fy = (*e)->GetFieldID(e, cls, "ydpi",          "F");
   jfieldID fi = (*e)->GetFieldID(e, cls, "densityDpi",    "I");
   if (fw) (*e)->SetIntField(e, out, fw, w);
   if (fh) (*e)->SetIntField(e, out, fh, h);
   if (fd) (*e)->SetFloatField(e, out, fd, density);
   if (fs) (*e)->SetFloatField(e, out, fs, density);
   if (fx) (*e)->SetFloatField(e, out, fx, dpi);
   if (fy) (*e)->SetFloatField(e, out, fy, dpi);
   if (fi) (*e)->SetIntField(e, out, fi, (jint)(dpi + 0.5f));
   fprintf(stderr, "[arm_jni] DisplayMetrics filled %dx%d density=%.1f\n",
           w, h, density);
}

void
android_view_Display_getRealMetrics(JNIEnv *e, jobject o, va_list a)
{
   (void)o;
   if (!a) return;
   android_view_Display_fillMetrics(e, va_arg(a, jobject));
}
void
android_view_Display_getMetrics(JNIEnv *e, jobject o, va_list a)
{
   android_view_Display_getRealMetrics(e, o, a);
}
void java_lang_Object_getRealMetrics(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getRealMetrics(e, o, a); }
void java_lang_Class_getRealMetrics(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getRealMetrics(e, o, a); }
void java_lang_Object_getMetrics(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getMetrics(e, o, a); }
void java_lang_Class_getMetrics(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getMetrics(e, o, a); }

/* Display.getRealSize(Point out) / getSize(Point out).  The Point form is the
 * one most engines use to size their swapchain; it was missing while
 * getRealMetrics was present, so a caller that asked for the size this way got
 * an untouched 0x0 Point.  Same numbers as fillMetrics — one screen. */
static void
android_view_Display_fillPoint(JNIEnv *e, jobject out)
{
   if (!e || !out) return;
   jclass cls = (*e)->FindClass(e, "android/graphics/Point");
   if (!cls) return;
   jfieldID fx = (*e)->GetFieldID(e, cls, "x", "I");
   jfieldID fy = (*e)->GetFieldID(e, cls, "y", "I");
   if (fx) (*e)->SetIntField(e, out, fx, arm_exec_fb_width());
   if (fy) (*e)->SetIntField(e, out, fy, arm_exec_fb_height());
}

void
android_view_Display_getRealSize(JNIEnv *e, jobject o, va_list a)
{
   (void)o;
   if (!a) return;
   android_view_Display_fillPoint(e, va_arg(a, jobject));
}
void android_view_Display_getSize(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getRealSize(e, o, a); }
void java_lang_Object_getRealSize(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getRealSize(e, o, a); }
void java_lang_Class_getRealSize(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getRealSize(e, o, a); }
void java_lang_Object_getSize(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getRealSize(e, o, a); }
void java_lang_Class_getSize(JNIEnv *e, jobject o, va_list a)
{ android_view_Display_getRealSize(e, o, a); }

/* PowerManager thermal API (API 29+).  The emulator never throttles, so the
 * status is permanently THERMAL_STATUS_NONE and a registered listener is never
 * called back — but the calls themselves have to exist, because a game that
 * cannot register its listener dies in its device-capability setup. */
jint android_os_PowerManager_getCurrentThermalStatus(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0; /* THERMAL_STATUS_NONE */ }
void android_os_PowerManager_addThermalStatusListener(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; }
void android_os_PowerManager_removeThermalStatusListener(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; }
jfloat android_os_PowerManager_getThermalHeadroom(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return 0.0f; }

/* Debug.isDebuggerConnected(): nothing is attached to the guest. */
jint android_os_Debug_isDebuggerConnected(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return JNI_FALSE; }
jint android_os_Debug_waitingForDebugger(JNIEnv *e, jobject o, va_list a)
{ (void)e; (void)o; (void)a; return JNI_FALSE; }

/* Dex: (Class,String,String,Z)->Method / Field.  Must take va_list like every
 * other Call* stub — CallStaticObjectMethodA builds a fake va_list over the
 * jvalue array; a jvalue*-taking body read that struct as a pointer and
 * returned null, so Unity's AndroidAgent never got getInstance. */
jobject
com_unity3d_player_ReflectionHelper_getMethodID(JNIEnv *env, jobject object,
                                                va_list args)
{
   assert(env && object);
   jclass clazz = va_arg(args, jclass);
   jstring name = va_arg(args, jstring);
   jstring sig = va_arg(args, jstring);
   jboolean is_static = (jboolean)va_arg(args, int);
   if (!clazz || !name || !sig) return NULL;
   const char *utf1 = (*env)->GetStringUTFChars(env, name, NULL);
   const char *utf2 = (*env)->GetStringUTFChars(env, sig, NULL);
   if (!utf1 || !utf2) {
      if (utf1) (*env)->ReleaseStringUTFChars(env, name, utf1);
      if (utf2) (*env)->ReleaseStringUTFChars(env, sig, utf2);
      return NULL;
   }
   /* Unity immediately FromReflectedMethod()'s the result.  In this JVM a
    * jmethodID is already a Method-shaped object, so returning it matches
    * FromReflectedMethod's identity conversion. */
   jobject mid = is_static
      ? (jobject)(*env)->GetStaticMethodID(env, clazz, utf1, utf2)
      : (jobject)(*env)->GetMethodID(env, clazz, utf1, utf2);
   (*env)->ReleaseStringUTFChars(env, name, utf1);
   (*env)->ReleaseStringUTFChars(env, sig, utf2);
   return mid;
}

jobject
com_unity3d_player_ReflectionHelper_getFieldID(JNIEnv *env, jobject object,
                                               va_list args)
{
   assert(env && object);
   jclass clazz = va_arg(args, jclass);
   jstring name = va_arg(args, jstring);
   jstring sig = va_arg(args, jstring);
   jboolean is_static = (jboolean)va_arg(args, int);
   if (!clazz || !name || !sig) return NULL;
   const char *utf1 = (*env)->GetStringUTFChars(env, name, NULL);
   const char *utf2 = (*env)->GetStringUTFChars(env, sig, NULL);
   if (!utf1 || !utf2) {
      if (utf1) (*env)->ReleaseStringUTFChars(env, name, utf1);
      if (utf2) (*env)->ReleaseStringUTFChars(env, sig, utf2);
      return NULL;
   }
   jobject fid = is_static
      ? (jobject)(*env)->GetStaticFieldID(env, clazz, utf1, utf2)
      : (jobject)(*env)->GetFieldID(env, clazz, utf1, utf2);
   (*env)->ReleaseStringUTFChars(env, name, utf1);
   (*env)->ReleaseStringUTFChars(env, sig, utf2);
   return fid;
}

/* Unity queues callbacks from native threads to run on the Java main thread.
 * In headless emulation the ARM game loop IS the main thread, so jobs have
 * already run by the time this is called — just return without blocking. */
void
com_unity3d_player_UnityPlayer_executeMainThreadJobs(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
}

/* Same method on the 2020+ service class */
void
com_unity3d_player_UnityPlayerForActivityOrService_executeMainThreadJobs(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
}

void
com_unity3d_player_UnityPlayer_executeGLThreadJobs(JNIEnv *env, jobject object, va_list args)
{
   (void)env; (void)object; (void)args;
   arm_exec_drain_gl_thread_jobs();
}

/* UnityPlayer.currentActivity is the Activity instance launched by the process,
 * not an arbitrary instance of android.app.Activity.  Identity matters to
 * window state, lifecycle state, and fields installed by the application's
 * constructor. */
jobject
com_unity3d_player_UnityPlayer_currentActivity(JNIEnv *env, jobject object)
{
   (void)env;
   (void)object;
   return g_current_activity;
}

/* PlayAssetDeliveryUnityWrapper.init(UnityPlayer, Context) — static factory that
 * wraps Google Play Asset Delivery.  In emulation we have no Play Core, so return
 * a stub object; the engine will detect playCoreApiMissing()=true and fall back to
 * loading assets directly from the APK zip. */
jobject
com_unity3d_player_PlayAssetDeliveryUnityWrapper_init(JNIEnv *env, jclass clazz, va_list args)
{
   (void)args;
   static jobject sv;
   if (!sv) sv = (*env)->AllocObject(env,
         (*env)->FindClass(env, "com/unity3d/player/PlayAssetDeliveryUnityWrapper"));
   return sv;
}

/* playCoreApiMissing() — return true so Unity skips Play Core and reads assets
 * straight from the APK (our asset_bridge already handles that path). */
jboolean
com_unity3d_player_PlayAssetDeliveryUnityWrapper_playCoreApiMissing(JNIEnv *env, jobject object)
{
   (void)env; (void)object;
   return 1;
}

/* Unity IAP: GooglePlayPurchasing.instance(IUnityCallback) — no Play Billing in
 * the emulator, but returning null makes AndroidJavaObject throw and a later
 * jproxy Runnable.run blows up mid-nativeRender.  Hand back a singleton stub.
 * Called via CallStaticObjectMethodA (jvalue*); values may be NULL. */
jobject
com_unity_purchasing_googleplay_GooglePlayPurchasing_instance(JNIEnv *env, jclass clazz, jvalue *values)
{
   (void)clazz; (void)values;
   static jobject sv;
   if (!sv)
      sv = (*env)->AllocObject(env,
            (*env)->FindClass(env, "com/unity/purchasing/googleplay/GooglePlayPurchasing"));
   return sv;
}

jstring
com_blizzard_wtcg_hearthstone_DeviceSettings_GetModelNumber(JNIEnv *env, jobject object)
{
   return (*env)->NewStringUTF(env, "0");
}
