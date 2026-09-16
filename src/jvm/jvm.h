/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "jni.h"
#include <stdbool.h>
#include <stdlib.h>

/* Android 実機の getPackageCodePath() / ApplicationInfo.sourceDir は APK ファイル
 * パスを返す。lunaria-apk.sh は展開先を ANDROID_PACKAGE_CODE_PATH に置くので、
 * JNI では ANDROID_APK_FILE を優先する。 */
static inline const char *lunaria_apk_mount_path(void)
{
   const char *apk = getenv("ANDROID_APK_FILE");
   if (apk && *apk)
      return apk;
   return getenv("ANDROID_PACKAGE_CODE_PATH");
}

/* The Android platform version this emulator presents.
 *
 * It has to be one number: an app reads it through Build.VERSION.SDK_INT,
 * through the `ro.build.version.sdk` system property from native code, and
 * through ApplicationInfo.targetSdkVersion, and it compares what it gets.  It
 * used to be spelled out separately in four places — 31 in the JNI stubs, 31
 * in the dvm and arm_exec property tables, and 15 in the host libc shim — so a
 * guest that asked twice could be told two different things.
 *
 * LUNARIA_SDK_INT overrides it (LUNARIA_ANDROID_RELEASE overrides the matching
 * release string), which is how you check whether a behaviour is gated on the
 * platform version rather than broken.  The default is Android 12, which is
 * also the oldest level the runtime is built to answer for: below it the
 * platform APIs the stubs implement no longer match what an app of that
 * vintage expects, so a lower setting is a diagnostic, not a configuration. */
#define LUNARIA_SDK_INT_DEFAULT 31   /* Android 12 */
#define LUNARIA_SDK_INT_FLOOR   31   /* Android 12 — supported floor */

int lunaria_sdk_int(void);
int lunaria_app_min_sdk(void);
int lunaria_app_target_sdk(void);
const char *lunaria_android_release(void);

/* The device this emulator reports itself as.
 *
 * A device identity is not a free-form label: `ro.product.model`,
 * `ro.product.device`, `ro.board.platform`, `ro.hardware`, the build id and
 * the fingerprint all have to describe one machine that was actually built and
 * shipped, and the platform level has to be one that machine shipped with.
 * Naming a device that has never existed ("Lunaria" on a board called
 * "lunaria") is internally consistent and still describes nothing -- which is
 * what any library that knows what phones exist is looking for.
 *
 * So the identity is a profile, chosen with LUNARIA_DEVICE and defaulting to a
 * real retail device.  Individual fields can be overridden one at a time; the
 * build id, DISPLAY and the fingerprint are assembled from whatever the
 * profile finally says, so an override cannot make them disagree. */
struct lunaria_device {
   const char *key;             /* LUNARIA_DEVICE selector */
   const char *manufacturer;    /* ro.product.manufacturer / Build.MANUFACTURER */
   const char *brand;           /* ro.product.brand */
   const char *model;           /* ro.product.model */
   const char *name;            /* ro.product.name */
   const char *device;          /* ro.product.device */
   const char *board;           /* ro.product.board */
   const char *platform;        /* ro.board.platform */
   const char *hardware;        /* ro.hardware */
   const char *build_id;        /* ro.build.id */
   const char *incremental;     /* ro.build.version.incremental */
   const char *security_patch;  /* ro.build.version.security_patch */
   const char *sensor_vendor;   /* the IMU's maker, for ASensor_getVendor */
   const char *accel_name;
   const char *gyro_name;
   int         sdk;             /* the level this build actually shipped with */
   /* The panel this machine was built with, in its natural (portrait)
    * orientation, and the density the build declares for it
    * (ro.sf.lcd_density).  A device's screen is a property of the device, so
    * it belongs in the same row as the board and the build id: a profile that
    * claims to be a Pixel 6 while reporting some other display describes a
    * machine nobody shipped, and an app that reads both notices.
    *
    * The emulator does not have to open a window that large — see
    * LUNARIA_SCALE in arm_exec.cpp, which shrinks the panel and its density
    * together, the way `wm size` / `wm density` do on a device. */
   int         screen_w;        /* px, short side  */
   int         screen_h;        /* px, long side   */
   int         density;         /* dpi             */
};

/* The emulated display, after LUNARIA_SCALE / LUNARIA_WIDTH / LUNARIA_HEIGHT /
 * LUNARIA_DPI and the app's requested orientation have been applied.  One
 * answer for the whole process: AConfiguration, DisplayMetrics, the EGL
 * surface and the window are all views of this. */
struct lunaria_screen {
   int width;      /* px, as oriented */
   int height;     /* px, as oriented */
   int density;    /* dpi             */
};
const struct lunaria_screen *lunaria_screen(void);

/* The active profile, after LUNARIA_DEVICE and the per-field overrides. */
const struct lunaria_device *lunaria_device(void);
/* Every built-in profile, for `--devices`-style listing.  NULL-terminated. */
const struct lunaria_device *const *lunaria_device_list(void);

/* Android system properties, and the only place they are decided.
 * android.os.Build is a view of this table (see jni_stubs.c), and so are
 * SystemProperties.get() in the bytecode VM and __system_property_get() in
 * the ARM SVC layer.  Returns "" for a property this device does not have. */
const char *lunaria_android_property(const char *name);

/* Where this install lives, spelled the way a device spells it.
 *
 * Everything the guest is told about its own package has to agree: an app
 * that reads sourceDir, getPackageCodePath() and dl_iterate_phdr() gets three
 * views of one file.  Handing any of them a
 * /tmp/lunaria-cache path describes a process no Android install could
 * produce.  map_guest_path() resolves these back to the staged copies, so the
 * canonical spelling opens as well as the host one did. */
const char *lunaria_android_apk_path(void);        /* .../base.apk */
const char *lunaria_android_apk_dir(void);         /* opaque installd directory */
const char *lunaria_android_native_lib_path(void); /* .../lib/arm64 */
const char *lunaria_android_data_path(void);       /* /data/data/<pkg> */

/* The installed-package registry of the Android device Lunaria presents.
 * PackageManager, ActivityManager and the device shell all answer from this
 * one ledger; two of them disagreeing is itself something an app can detect.
 *
 * The ledger itself is src/jvm/packages.c: what it holds is a property of the
 * installation, so it is configured in lunaria.conf (LUNARIA_PACKAGES,
 * LUNARIA_PACKAGES_EXTRA) rather than derived from the running app. */
enum {
   /* `flags` below is ApplicationInfo.flags exactly as the guest is handed
    * it, so the bits are the framework's.  FLAG_SYSTEM is the one the
    * emulator itself tests (`pm list packages -s|-3`). */
   LUNARIA_APP_FLAG_SYSTEM = 1u
};

struct lunaria_android_package {
   const char *name;
   const char *source_dir;
   const char *data_dir;
   /* Never empty on a device: a platform package's native libraries live in
    * /system/lib64 and an installed app's in its own lib directory.  An empty
    * one is visible to any caller that builds a path out of it -- NMSS opens
    * nativeLibraryDir + "/libnmsssa.so" and greps `pm list packages -f` for
    * the same string. */
   const char *lib_dir;
   int uid;
   int flags;
   int target_sdk;
};

size_t lunaria_android_package_count(void);
int lunaria_android_package_at(size_t index,
                               struct lunaria_android_package *out);
int lunaria_android_package_find(const char *name,
                                 struct lunaria_android_package *out);

/* Package visibility, as Android 11 defines it for PackageManager queries.
 *
 * An app that targets API 30 or later sees only itself, the packages its
 * manifest <queries> names, and the always-queryable platform packages --
 * unless it holds QUERY_ALL_PACKAGES.  The device shell and the registry
 * itself are not filtered; only what an app is allowed to observe is.
 *
 * Returns non-zero when the running app may see `record`. */
int lunaria_android_package_visible(const struct lunaria_android_package *record);

struct jvm_string {
   const char *data;
   size_t size;
   bool heap; // if on heap, then `data` should be freed
};

struct jvm_array {
   void *data;
   size_t element_sz, size; // `size` == in elements, `size * element_sz` for bytes
};

struct jvm_class {
   struct jvm_string name;
};

struct jvm_method {
   jclass klass;
   struct jvm_string name, signature;
};

struct jvm_object {
   jclass this_klass;

   union {
      struct jvm_array array;
      struct jvm_method method;
      struct jvm_class klass;
      struct jvm_string string;
      struct {
         int action;       /* AMOTION_EVENT_ACTION_* */
         float x, y;
         int64_t event_ms; /* CLOCK_MONOTONIC ms when this sample happened */
         int64_t down_ms;  /* down time of the active pointer gesture */
      } motion;
   };

   enum jvm_object_type {
      JVM_OBJECT_NONE,
      JVM_OBJECT_OPAQUE,
      JVM_OBJECT_ARRAY,
      JVM_OBJECT_METHOD,
      JVM_OBJECT_CLASS,
      JVM_OBJECT_STRING,
      JVM_OBJECT_MOTION,
      JVM_OBJECT_LAST,
   } type;

   /* Outstanding references; creation counts as one.  DeleteLocalRef and
    * DeleteGlobalRef were no-ops, so every handle an app made leaked until
    * the table filled and the process aborted.  Only the two types an app
    * allocates without bound — arrays and strings — release their slot when
    * this reaches zero (see jvm_deref_object()).  Classes and methods are
    * interned and must keep their identity for the process lifetime, and
    * opaque objects back the singleton stubs in jni_stubs.c, which cache the
    * jobject in a `static` and would dangle if the slot were recycled. */
   int refs;

   /* JNI MonitorEnter/Exit state.  Access is serialized by the bridge's
    * monitor mutex; the small owner token avoids depending on pthread_t's
    * representation in this public C structure. */
   uint64_t monitor_owner;
   uint32_t monitor_depth;
};

struct jvm_native_method {
   struct jvm_method method;
   void *function;
};

struct jvm {
   // [0] object is created on `jvm_init` and it's a class object for defining the class of a class
   // every class object's `this_class` member points back to [0], causing recursion.
   // Every other object or class definition is created lazily as needed, only [0] is special.
   // `jobject`'s we return through JNI are actually (index+1) to this array, not pointers.
   struct jvm_object objects[65536];

   // Rotating free-slot hint for jvm_add_object(): allocation resumes here
   // instead of rescanning the whole table from index 0 on every call.
   size_t next_object;

   // Native methods registered by the application.
   // Nothing special, but there's no need to access this array either really.
   // You can use `jvm_get_native_method` instead.
   struct jvm_native_method methods[255];

   // These hold the function pointers for our JNI implementation.
   struct JNINativeInterface native;
   struct JNIInvokeInterface invoke;

   // JNI's api is weird.. pointer to a reference of a struct, OK!
   // Developers have to dereference these pointers to call methods from an ... reference.
   // NOTE: These are pointers, and JNI interface passes pointers to these pointers!
   JNIEnv env; // points to native
   JavaVM vm; // points to invoke

   /* The pending JNI exception.  A JNI caller finds out that a call failed by
    * asking ExceptionCheck()/ExceptionOccurred(), not by the return value —
    * ClassLoader.loadClass() answering "null, and no exception" tells the
    * caller the class loaded and *is* null, which is not a thing that can
    * happen on a device.  libswappy reads exactly this to decide whether to
    * fall back to its own embedded dex. */
   jthrowable pending_exception;
   char pending_exception_class[128];
   char pending_exception_msg[256];
   /* jvm_wrap_method() resolves a method to its hand-written stub by forming a
    * symbol name and asking dlsym(RTLD_DEFAULT) for it, walking up the class
    * hierarchy when the derived class has no stub of its own.  That is up to
    * eighteen dlsym misses — each one a search of every loaded object — plus a
    * dvm_jni_super_name() per hop, which takes the interpreter lock and walks
    * the dex.  It used to do all of that on *every* JNI call: this UE title
    * polls MediaPlayer.getCurrentPosition two thousand times a second, and
    * each call spent about 42 us here before the method body even started.
    *
    * The answer depends only on the method object and the class hierarchy,
    * both fixed once the dexes are in, so resolve it once per method id.  NULL
    * is a real answer — "no stub, hand it to the dvm" — so the resolved flag
    * is separate, and the whole table is dropped when a dex arrives.
    *
    * Deliberately last in the struct.  Putting it between objects[] and
    * next_object made the emulator fault at the first bytecode call, which
    * means something writes past the end of objects[] and has been landing on
    * the fields that follow it.  That is worth finding, but it is not this
    * change's to fix; appending leaves every existing offset relationship
    * exactly as it was. */
   void    *wrap_cache[65536];
   bool     wrap_cached[65536];
   unsigned wrap_epoch;
};

/* Raise a JNI exception of `class_name` ("java/lang/ClassNotFoundException")
 * with `msg`, as ThrowNew would.  Safe to call from the JNI stubs. */
void jvm_throw_new(struct jvm *jvm, const char *class_name, const char *msg);

const char*
jvm_get_class_name(struct jvm *jvm, jobject object);

/* Binary name of the class a Class object describes, or NULL if `object` is
 * not a Class.  Distinct from GetObjectClass(instance) → class name. */
const char *
jvm_described_class_name(struct jvm *jvm, jobject object);

/* Whether the stub layer actually implements `method` — i.e. whether a stub
 * symbol resolves for it, here or on one of its superclasses.  Without this a
 * caller cannot tell a stub that legitimately returned 0/null from a method
 * that was never implemented: both come back as zero, and the second is the
 * silent failure this emulator keeps having to hunt down. */
bool
jvm_method_has_stub(JNIEnv *env, jmethodID method);

/* Declaring class, name and descriptor behind a jfieldID.  A field id is a
 * method object here; this is how a caller outside jvm.c can read it. */
bool
jvm_field_info(struct jvm *jvm, jfieldID field, const char **klass,
               const char **name, const char **type);

void*
jvm_get_native_method(struct jvm *jvm, const char *klass, const char *method);

void
jvm_release(struct jvm *jvm);

void
jvm_init(struct jvm *jvm);

/* Immutable Android MotionEvent payload.  Each injected touch gets its own
 * JVM_OBJECT_MOTION instance; getters read only that object, never global
 * state — Unity keeps the jobject and reads it again during PlayerLoop. */
typedef struct lunaria_touch_event {
   int action;
   float x, y;
   long long event_ms;
   long long down_ms;
} lunaria_touch_event;

jobject
jvm_new_motion_event(struct jvm *jvm, const lunaria_touch_event *ev);

/* Copy payload out of a JVM_OBJECT_MOTION handle.  Returns false for other types. */
bool
jvm_motion_event_read(struct jvm *jvm, jobject object, lunaria_touch_event *out);

struct jvm*
jnienv_get_jvm(JNIEnv *env);

/* Per-File path storage (java/io/File). jobject handles index this table. */
void jni_file_set_path(jobject file, const char *path);
const char *jni_file_get_path(jobject file);
void jni_set_current_activity(JNIEnv *env, jobject activity);
jobject jni_get_current_activity(void);
void jni_file_bind_ctor(JNIEnv *env, jobject file, jmethodID ctor, va_list ap);
void jni_file_bind_ctor_a(JNIEnv *env, jobject file, jmethodID ctor, const jvalue *args);

/* Diagnostics for the JNI stub-resolution cache (see jvm::wrap_cache). */
void jvm_wrap_stats(unsigned long long *resolves, unsigned long long *ns,
                    unsigned long long *hits);
