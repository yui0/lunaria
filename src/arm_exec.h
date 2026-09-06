/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "jvm/jvm.h"

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- guest RAM profile -------------------------------------------------- *
 *
 * What ActivityManager.getMemoryInfo() and /proc/meminfo tell the app it is
 * running on.  It lived in a header of its own, which bought nothing: the
 * three translation units that read it (arm_exec.cpp, jni_stubs.c,
 * dvm_runtime.c) all include this one already.
 *
 * Env (all optional):
 *   LUNARIA_MEM_TOTAL_MB      — total RAM advertised (default 6144 = 6 GiB)
 *   LUNARIA_MEM_AVAIL_MB      — available RAM (default ~5/6 of total)
 *   LUNARIA_MEM_THRESHOLD_MB  — low-memory threshold (default 48)
 *   LUNARIA_MEM_FREE_MB       — MemFree in /proc/meminfo (default ~2/3 avail)
 *
 * The 32-bit guest VA space is still ~4 GiB, so mmap arenas cannot back a full
 * 6 GiB mapping; the reported size drives Unity's heuristics while
 * LUNARIA_HEAP_MB / the MMAP2 layout control actual allocation. */
static inline long lunaria_env_long(const char *name, long def)
{
    const char *e = getenv(name);
    if (!e || !e[0])
        return def;
    char *end = NULL;
    long v = strtol(e, &end, 0);
    return (end != e) ? v : def;
}

/* Clamp phone-class totals into a sane range for the emulator. */
static inline long lunaria_mem_total_mb(void)
{
    long mb = lunaria_env_long("LUNARIA_MEM_TOTAL_MB", 6144);
    if (mb < 512)
        mb = 512;
    if (mb > 16384)
        mb = 16384;
    return mb;
}

static inline long lunaria_mem_avail_mb(void)
{
    long mb = lunaria_env_long("LUNARIA_MEM_AVAIL_MB", 0);
    if (mb <= 0)
        mb = lunaria_mem_total_mb() * 5 / 6;
    if (mb < 256)
        mb = 256;
    if (mb > lunaria_mem_total_mb())
        mb = lunaria_mem_total_mb();
    return mb;
}

static inline long lunaria_mem_free_mb(void)
{
    long mb = lunaria_env_long("LUNARIA_MEM_FREE_MB", 0);
    if (mb <= 0)
        mb = lunaria_mem_avail_mb() * 2 / 3;
    if (mb < 128)
        mb = 128;
    if (mb > lunaria_mem_avail_mb())
        mb = lunaria_mem_avail_mb();
    return mb;
}

static inline long lunaria_mem_threshold_mb(void)
{
    long mb = lunaria_env_long("LUNARIA_MEM_THRESHOLD_MB", 48);
    if (mb < 8)
        mb = 8;
    return mb;
}

static inline uint64_t lunaria_mem_total_bytes(void)
{
    return (uint64_t)lunaria_mem_total_mb() * 1024ull * 1024ull;
}

static inline uint64_t lunaria_mem_avail_bytes(void)
{
    return (uint64_t)lunaria_mem_avail_mb() * 1024ull * 1024ull;
}

static inline uint64_t lunaria_mem_threshold_bytes(void)
{
    return (uint64_t)lunaria_mem_threshold_mb() * 1024ull * 1024ull;
}

static inline uint64_t lunaria_mem_total_kb(void)
{
    return (uint64_t)lunaria_mem_total_mb() * 1024ull;
}

static inline uint64_t lunaria_mem_avail_kb(void)
{
    return (uint64_t)lunaria_mem_avail_mb() * 1024ull;
}

static inline uint64_t lunaria_mem_free_kb(void)
{
    return (uint64_t)lunaria_mem_free_mb() * 1024ull;
}

/*
 * Detect whether the ELF at `path` is an ARM 32-bit shared object.
 * Returns 1 if ARM, 0 otherwise.
 */
int arm_elf_is_arm32(const char *path);

/*
 * Load and execute an ARM 32-bit JNI library under dynarmic emulation.
 *
 * Loads all PT_LOAD segments, resolves dynamic imports to SVC trampolines,
 * calls JNI_OnLoad, and registers native methods into `jvm`.
 *
 * Returns the JNI version returned by JNI_OnLoad, or -1 on error.
 */
int arm_exec_jni_onload(const char *path, struct jvm *jvm);

/*
 * Call an ARM native method previously registered via RegisterNatives.
 * `fn_va` is the ARM virtual address of the function.
 * `args`  is the jvalue array (A-variant of the JNI call).
 * `ret`   receives the return value as a jvalue.
 *
 * The caller is responsible for marshalling the correct argument types.
 * Returns 0 on success, -1 if no ARM execution context is active.
 */
int arm_exec_call_native(uint32_t fn_va, JNIEnv *env, jobject obj,
                         const jvalue *args, int nargs, jvalue *ret);

/* Look up the ARM virtual address of a native method registered via RegisterNatives.
 * Returns 0 if no ARM context is active or the method is not found. */
uint32_t arm_exec_lookup_native(const char *klass, const char *method);

/* Like arm_exec_lookup_native but also fills `sig_out` (up to sig_max bytes)
 * with the JNI signature string of the registered method. */
uint32_t arm_exec_lookup_native_sig(const char *klass, const char *method,
                                    char *sig_out, int sig_max);

/* Look up a loaded ELF export by name (e.g. "mono_file_map_override").
 * Returns the guest VA from the first library that exported it, or 0. */
uint32_t arm_exec_lookup_export(const char *sym);

/* Call an ARM function with up to 4 register arguments (r0–r3).
 * Returns the value of r0 after the call. */
int arm_exec_call(uint32_t fn_va, uint32_t r0, uint32_t r1,
                  uint32_t r2, uint32_t r3);

/* arm_exec_call + AAPCS スタック渡し引数 2 個 (JNI の 5 個目以降の引数)。
 * nativeResize(IIII) など、レジスタ 4 本に収まらないシグネチャ用。 */
int arm_exec_call6(uint32_t fn_va, uint32_t r0, uint32_t r1,
                   uint32_t r2, uint32_t r3, uint32_t stk0, uint32_t stk1);

/* 任意個数の 32bit 引数で ARM 関数を呼ぶ (AAPCS: r0-r3 + スタック)。
 * nativeSetGlobalActivity(ZZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;)
 * のように引数が 8 個ある JNI ネイティブ用。 */
int arm_exec_calln(uint32_t fn_va, const uint32_t *args, int nargs);

/* arm_exec_lookup_native と同じだが、オーバーロード時の long-form
 * (Java_Cls_method__SIG) 解決のために JNI シグネチャを渡せる。 */
uint32_t arm_exec_lookup_native_isig(const char *klass, const char *method,
                                     const char *sig);

/* Like arm_exec_call but with no tick limit.  Use for calls that MUST run to
 * completion — abandoning them mid-execution (e.g. mid-PlayerLoop) leaves
 * guest state inconsistent.  The caller must ensure the function terminates. */
int arm_exec_call_unlimited(uint32_t fn_va, uint32_t r0, uint32_t r1,
                             uint32_t r2, uint32_t r3);

/* Return the ARM virtual address used as JNIEnv* (ENV_SLOT_BASE),
 * or 0 if no ARM context is active. */
uint32_t arm_exec_env_va(void);

/* Poll GLFW events (call from the render loop). */
void arm_exec_glfw_poll(void);

/* GL_VENDOR / GL_RENDERER / GL_VERSION / GL_SHADING_LANGUAGE_VERSION /
 * GL_EXTENSIONS of the host GL, cached while a context was current so Java-side
 * callers on context-less threads get the same answer a device would give.
 * NULL for anything else. */
const char *arm_exec_gl_string(unsigned name);

/* Host EGL for the Java android.opengl.EGL14 binding (src/dvm/dvm_runtime.c).
 * Android drives one EGL from both its native and its Java API; these are the
 * same operations the guest's EGL SVCs perform, in the same guest handle
 * space, so a context Java makes current is the one the scheduler restores for
 * that thread.  Handles: 0 is EGL_NO_*, everything else is opaque. */
uint32_t arm_exec_egl_get_display(void);
int      arm_exec_egl_initialize(int32_t *major, int32_t *minor);
int      arm_exec_egl_choose_config(const int32_t *attribs, uint32_t *out,
                                    int max, int32_t *num);
uint32_t arm_exec_egl_create_context(uint32_t share, const int32_t *attribs);
uint32_t arm_exec_egl_create_pbuffer_surface(void);
int      arm_exec_egl_make_current(uint32_t draw, uint32_t read, uint32_t ctx);
int      arm_exec_egl_destroy_context(uint32_t ctx);
const char *arm_exec_egl_query_string(int name);
uint32_t arm_exec_egl_get_current_context(void);
uint32_t arm_exec_egl_get_current_display(void);
uint32_t arm_exec_egl_get_current_surface(void);
int      arm_exec_egl_query_context(uint32_t ctx, int attr, int32_t *value);
int      arm_exec_egl_get_config_attrib(uint32_t cfg, int attr, int32_t *value);
int      arm_exec_egl_get_error(void);

/* Overlay (and any other host path) that calls eglMakeCurrent itself must
 * drop the scheduler's binding ledger, or the next slice would restore from
 * a stale handle.  After restoring the guest binding, call
 * arm_exec_egl_note_current instead so the next slice skips eglMakeCurrent. */
void     arm_exec_egl_invalidate_current(void);
void     arm_exec_egl_note_current(void *egl_ctx, void *egl_draw_surf);

/* Upload decoded RGBA into a guest GL_TEXTURE_2D on the current context. */
void     arm_exec_upload_texture_rgba(int tex, const uint8_t *rgba, int w, int h);

/* Read one packaged asset by the name AssetManager.open() takes (relative to
 * assets/, or an absolute path the engine also feeds to AAssetManager).
 * Returns a malloc'd buffer the caller frees, or NULL when there is no such
 * asset.  This is the same lookup the native AAsset path uses — APK, split
 * APKs, OBB and the staged files tree. */
unsigned char *arm_exec_asset_read(const char *name, size_t *len);
/* Read a named member from a guest-visible zip/jar/apk.  Unlike the asset
 * helper this honours the archive argument and does not add assets/ prefixes. */
unsigned char *arm_exec_zip_read(const char *archive, const char *name,
                                 size_t *len);

/* Dump current framebuffer to PPM.  path may be NULL → /tmp/lunaria_NNNN.ppm.
 * Returns 1 on success.  Also: F12 in the window, or `touch /tmp/lunaria-shot`. */
int arm_exec_screenshot(const char *path);

/* Framebuffer / window size (LUNARIA_WIDTH / LUNARIA_HEIGHT, default 1280×720). */
int arm_exec_fb_width(void);
/* Resolves an absolute guest path into the host path it names, applying the
 * guest's filesystem namespace (Android roots, external storage, app data).
 * Returns `path` itself when no rewrite applies, otherwise `buf`.  Callers
 * that touch the host filesystem on the guest's behalf must go through this:
 * without it an Android path such as "/etc" aliases the host's. */
const char *arm_exec_map_guest_path(const char *path, char *buf, size_t bufsz);

/* Reports a guest operation that destroys something under a directory lunaria
 * staged — the extracted expansion, or the app's data.  Those are the two
 * trees the emulator cannot rebuild for free, so their removal is an event to
 * see happen rather than to deduce later from an empty directory. */
void arm_exec_note_destructive(const char *what, const char *host_path,
                               const char *dest);

/* AndroidManifest meta-data for the bytecode VM's ApplicationInfo.metaData
 * Bundle.  Returns 0 when absent, else the value kind ('Z','I','F' in *iv or
 * 'L' in *sv). */
int arm_exec_apk_meta(const char *key, int32_t *iv, const char **sv);
const char *arm_exec_apk_meta_keys(void);
/* The audio output the emulator actually presents, as AudioManager reports it
 * (PROPERTY_OUTPUT_FRAMES_PER_BUFFER / PROPERTY_OUTPUT_SAMPLE_RATE).  An app
 * that asks Java for these must get the same numbers the OpenSL ES pump runs
 * at, or it sizes its mixer for a device that is not here. */
void arm_exec_audio_output_params(int32_t *frames_per_buffer, int32_t *rate);
/* The package version from the <manifest> element (PackageInfo.versionCode /
 * versionName).  Never yields 0 or NULL — see the definition. */
void arm_exec_apk_version(int32_t *code, const char **name);
/* resources.arsc name/value lookup used by android.content.res.Resources. */
uint32_t arm_exec_apk_resource_id(const char *type, const char *name);
int arm_exec_apk_resource_value(uint32_t id, int32_t *iv, const char **sv);
/* Resolve an attribute from a compiled style bag, following app-resource
 * parents.  Returns the Res_value data type, or 0 when absent. */
int arm_exec_apk_style_value(uint32_t style_id, uint32_t attr_id,
                             int32_t *iv, const char **sv);
int arm_exec_fb_height(void);

/* Touch input bridge (GLFW mouse → Android MotionEvent).
 * arm_exec_touch_next() pops the next queued sample into *out (required).
 * Returns 0 when the queue is empty.  arm_exec_touch_* accessors reflect the
 * last popped sample for diagnostics only — MotionEvent JNI getters must use
 * jvm_motion_event_view(), not these. */
typedef struct ArmExecTouchEvent {
   int action;          /* 0=DOWN 1=UP 2=MOVE */
   float x, y;
   long long event_ms;
   long long down_ms;
} ArmExecTouchEvent;

int       arm_exec_touch_next(ArmExecTouchEvent *out);
int       arm_exec_touch_action(void);
float     arm_exec_touch_x(void);
float     arm_exec_touch_y(void);
long long arm_exec_touch_time(void);
long long arm_exec_touch_down_time(void);
void      arm_exec_touch_push(int action, float x, float y);

/* NDK input queue for NativeActivity titles (UE).  Creates the queue's pipe on
 * first call and returns the opaque AInputQueue* the guest will hand back to
 * AInputQueue_*; the caller publishes it to the native_app_glue as
 * android_app::pendingInputQueue and sends APP_CMD_INPUT_CHANGED. */
uint32_t  arm_exec_input_queue_handle(void);

/* The host clipboard, shared by the emulator's input method and the guest's
 * android.content.ClipboardManager — a device has one clipboard, so this is
 * it.  arm_exec_clipboard_get() returns a malloc'd UTF-8 string the caller
 * frees; it is never NULL. */
void  arm_exec_clipboard_set(const char *utf8);
char *arm_exec_clipboard_get(void);

/* Returns 1 if the GLFW window close button was pressed, 0 otherwise. */
int arm_exec_glfw_should_close(void);
void arm_exec_request_quit(void);

/* Stop and join the A64 engine pool.  Call once the guest is finished and
 * before the process returns from main: the workers are host threads owned by
 * a static, and destroying a joinable std::thread calls std::terminate(). */
void arm_exec_shutdown_engines(void);

/* One line naming which guest threads the instructions went to.  Printed
 * beside [perf]: the process-wide rate says how fast the guest is running,
 * this says whether the thread running is the one that should be. */
void arm_exec_ticks_report(void);

/* Run all pthread_create-queued ARM thread functions inline (up to 8 passes). */
void arm_exec_run_pending_threads(void);

/* Guest instructions retired by the cooperative scheduler so far (A32 + A64).
 * The host frame pump compares the value across a scheduler pass to tell "the
 * guest had work to do" from "every guest thread is parked"; see the pump
 * loops in loader.c. */
uint64_t arm_exec_sched_ticks(void);
/* Guest instructions the JIT has translated (not executed).  Compare with
 * arm_exec_sched_ticks(): translating far more than the guest runs means the
 * emulator is recompiling, not working. */
uint64_t arm_exec_translated_insn(void);

/* Pre-create Mono generic JIT trampolines before initJni maps mscorlib. */
void arm_exec_ensure_mono_trampolines(void);

/* Alias: trampolines only (no host mono_jit_init_version). */
void arm_exec_ensure_mono_jit_init(void);

/* Register libunity machine.config + mono etc paths before mjiv. */
void arm_exec_prepare_mono_config(void);

/* Copy mono_get_root_domain into @0x3bd918 when Unity has inited Mono. */
void arm_exec_sync_mono_domain_slot(void);

/* Unity 4 GL thread job queue (ConcurrentLinkedQueue.add → executeGLThreadJobs). */
void arm_exec_gl_job_enqueue(uint32_t runnable_handle);
void arm_exec_drain_gl_thread_jobs(void);

/* Initialize the ARM execution context (JNI tables, stack, heap) without
 * loading any ELF.  Call this before arm_exec_load_library to pre-load
 * dependency libraries so their exports are visible when the main library
 * is patched. */
int arm_exec_context_init(struct jvm *jvm);

/* Load an additional ARM32 ELF shared library at `base_addr` into the current
 * ARM execution context. Exported symbols become available for cross-library
 * resolution. Calls JNI_OnLoad if present.
 * Call arm_exec_context_init first, then load dependencies before the main lib. */
int arm_exec_load_library(const char *path, uint32_t base_addr);

/* Initialize host EGL + GLES2 context and make it current.
 * Must be called before nativeRecreateGfxState so Unity's GL calls use a real context.
 * Returns 1 on success, 0 on failure. */
int arm_exec_host_egl_init(void);

/* Call eglSwapBuffers on the host EGL surface (present the rendered frame). */
void arm_exec_egl_swap(void);

/* Read a 32-bit word from the ARM guest address space.
 * Returns 0 if the address is unmapped or no ARM context is active. */
uint32_t arm_exec_read32(uint32_t va);

/* Write a 32-bit word to the ARM guest address space (no-op if no context). */
void arm_exec_write32(uint32_t va, uint32_t val);

/* Bytes of guest heap consumed by the bump allocator (for leak diagnosis). */
uint32_t arm_exec_heap_used(void);

/* Present the boot / JIT status card once.  Safe during dex load (few SVCs):
 * opens the host window if needed and draws through the overlay's unshared
 * context.  No-op once the guest has presented its first frame. */
int arm_exec_boot_present(void);

/* How many times guest abort() has been called (mono g_assert, etc.). */
uint64_t arm_exec_guest_abort_count(void);

/* Consumes a pending SetDesiredViewSize change: returns 1 once per resize and
 * fills the width/height outputs with the new view size, so the pump can deliver the engine's
 * surfaceChanged notification. */
int arm_exec_take_view_resize(int *w, int *h);

/* Return the directory of the main ARM library (set when arm_exec_jni_onload
 * is first called).  Used by libjvm-java.c findLibrary to return full paths. */
const char *arm_exec_get_main_lib_dir(void);

/* Set the APK process native-library directory without loading a library.
 * Android starts ordinary applications in Java and only loads native code
 * when System.loadLibrary() is reached; that startup path has no "main .so"
 * from which the directory could otherwise be inferred. */
void arm_exec_set_main_lib_dir(const char *dir);

/* Reset saved callee-saved registers (R4-R11) to zero.
 * Call after a run that may have corrupted ARM state (stack overflow / exception).
 * Prevents stale register values from being restored into the next JNI call. */
void arm_exec_reset_saved_regs(void);

/* Allocate `size` bytes in the guest heap.  Returns guest VA, or 0 on OOM. */
uint32_t arm_exec_malloc(uint32_t size);

/* Copy a host C string into guest memory (NUL-terminated).  Returns VA or 0. */
uint32_t arm_exec_strdup(const char *s);

/* Build a guest ANativeActivity (+ callbacks) for UE4 NativeActivity entry.
 * Returns the activity VA, or 0 on failure.  `clazz` is a jobject handle. */
uint32_t arm_exec_native_activity_create(uint32_t clazz_handle);

/* Ensure a fake ANativeWindow exists; return its guest VA (for onNativeWindowCreated). */
uint32_t arm_exec_native_window_va(void);

/* True if ANDROID_APK_FILE — the package path handed to the guest — contains
 * this zip entry.  Answers "is the expansion file inside the APK?" without
 * guessing from which env vars happen to be set. */
int arm_exec_apk_has_entry(const char *name);

/* The host path of that entry when the package was unpacked to a directory,
 * else NULL.  The widget layer resolves a drawable resource to an entry name
 * and needs a file the overlay's image loader can open.  Static buffer, valid
 * until the next call. */
const char *arm_exec_apk_entry_path(const char *name);

/* -------------------------------------------------------------------------
 * ARM64 (AArch64) execution engine — parallel to the ARM32 engine above.
 * All arm64_exec_* functions mirror their arm_exec_* counterparts but
 * operate on the A64 JIT (Dynarmic::A64::Jit) and load Elf64 binaries.
 * ---------------------------------------------------------------------- */

/* Detect whether the ELF at `path` is an AArch64 (arm64-v8a) shared object. */
int arm64_elf_is_arm64(const char *path);

/* Initialize the ARM64 execution context (JNI tables, stack, heap).
 * Must be called before arm64_exec_load_library or arm64_exec_jni_onload. */
int arm64_exec_context_init(struct jvm *jvm);

/* Load an additional ARM64 ELF shared library into the current A64 context.
 * `base_addr` == 0 → auto-place after previously loaded libraries. */
int arm64_exec_load_library(const char *path, uint64_t base_addr);

/* Load an ARM64 ELF and call JNI_OnLoad. Returns JNI version or -1. */
int arm64_exec_jni_onload(const char *path, struct jvm *jvm);

/* Call an ARM64 function with up to 4 register arguments (x0-x3). */
int64_t arm64_exec_call(uint64_t fn_va, uint64_t x0, uint64_t x1,
                        uint64_t x2, uint64_t x3);

/* A64: nativeResize etc. — first 8 args in x0–x7 (no stack needed for 6). */
int64_t arm64_exec_call6(uint64_t fn_va, uint64_t x0, uint64_t x1,
                         uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5);

/* AArch64 call with up to eight integer arguments (x0..x7).  UE4's
 * GameActivity natives take more than the six arm64_exec_call6 carries. */
int64_t arm64_exec_call8(uint64_t fn_va, const uint64_t *x, int n);

/* Like arm64_exec_call but with no tick limit. */
int64_t arm64_exec_call_unlimited(uint64_t fn_va, uint64_t x0, uint64_t x1,
                                  uint64_t x2, uint64_t x3);

/* Look up an exported symbol VA by name. Returns 0 if not found. */
uint64_t arm64_exec_lookup_export(const char *sym);

/* Look up a RegisterNatives-registered native method VA. */
uint64_t arm64_exec_lookup_native(const char *klass, const char *method);
/* Tell the A64 scheduler whether the engine itself runs on guest threads
 * (UE) or only its idle worker pool does (Unity).  See a64_thread_slice(). */
void arm64_exec_threads_run_engine(int on);

/* Like arm64_exec_lookup_native but also fills sig_out. */
uint64_t arm64_exec_lookup_native_sig(const char *klass, const char *method,
                                      char *sig_out, int sig_max);

/* Return the A64 JNIEnv* guest VA (ENV_SLOT64_BASE). */
uint64_t arm64_exec_env_va(void);

/* Build and return an ANativeActivity guest struct for UE NativeActivity. */
uint64_t arm64_exec_native_activity_create(uint64_t clazz_handle);

/* Ensure a fake ANativeWindow exists; return its guest VA. */
uint64_t arm64_exec_native_window_va(void);

/* Read / write guest memory (64-bit addresses). */
uint32_t arm64_exec_read32(uint64_t va);
uint64_t arm64_exec_read64(uint64_t va);
void     arm64_exec_write32(uint64_t va, uint32_t val);
void     arm64_exec_write64(uint64_t va, uint64_t val);

/* Allocate `size` bytes in the A64 guest heap. Returns guest VA or 0. */
uint64_t arm64_exec_malloc(uint64_t size);

/* Copy a host C string into A64 guest memory. Returns VA or 0. */
uint64_t arm64_exec_strdup(const char *s);

/* Initialize host EGL+GLES2 context (shared with A32 path). */
int arm64_exec_host_egl_init(void);

/* Poll GLFW events. */
void arm64_exec_glfw_poll(void);

/* Present host EGL surface. */
void arm64_exec_egl_swap(void);

/* Dump framebuffer to PPM. */
int arm64_exec_screenshot(const char *path);

/* Window dimensions. */
int arm64_exec_fb_width(void);
int arm64_exec_fb_height(void);

/* Touch input. */
void arm64_exec_touch_push(int action, float x, float y);

/* Run queued guest threads (cooperative scheduler). */
void arm64_exec_run_pending_threads(void);

/* GLFW window close requested? */
int arm64_exec_glfw_should_close(void);

/* Dump recent SVC ring for diagnostics. */
void arm64_exec_svc_ring_dump(void);
/* Presents the guest itself drove through the EGL bridge.  The pump loop
 * watches this to notice that the guest has stopped producing frames. */
uint64_t arm_exec_guest_swap_count(void);

#ifdef __cplusplus
}
#endif
