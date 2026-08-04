/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <dlfcn.h>
#include <elf.h>
#include <err.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include "linker/dlfcn.h"
#include "linker/linker.h"
#include "jvm/jvm.h"
#include "arm_exec.h"
#include "dvm/dvm_jni.h"
#include <link.h>

/* Exposed from arm_exec.cpp for diagnostic dumps */
extern void arm_exec_svc_ring_dump(void);

/* Load APK-private AArch64 DT_NEEDED libraries before their consumer.  The
 * old A64 path named a handful of Unity/UE libraries explicitly, so a normal
 * dependency such as libunity.so -> libmain.so was silently left unresolved.
 * Android system libraries are provided by Lunaria's SVC/runtime bridge and
 * therefore deliberately have no guest ELF beside the APK libraries. */
static int a64_dep_seen(const char *name, char seen[][NAME_MAX + 1], size_t n)
{
   for (size_t i = 0; i < n; ++i)
      if (!strcmp(name, seen[i])) return 1;
   return 0;
}

static int a64_vaddr_to_offset(const Elf64_Phdr *ph, size_t n,
                               Elf64_Addr va, Elf64_Off *off)
{
   for (size_t i = 0; i < n; ++i) {
      if (ph[i].p_type != PT_LOAD) continue;
      if (va >= ph[i].p_vaddr && va < ph[i].p_vaddr + ph[i].p_filesz) {
         *off = ph[i].p_offset + (va - ph[i].p_vaddr);
         return 1;
      }
   }
   return 0;
}

static size_t a64_read_needed(const char *path,
                              char names[][NAME_MAX + 1], size_t cap)
{
   FILE *f = fopen(path, "rb");
   Elf64_Ehdr eh;
   Elf64_Phdr *ph = NULL;
   size_t count = 0;
   if (!f || fread(&eh, sizeof eh, 1, f) != 1 ||
       memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
       eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_machine != EM_AARCH64 ||
       !eh.e_phnum || eh.e_phentsize != sizeof(Elf64_Phdr))
      goto out;
   ph = calloc(eh.e_phnum, sizeof *ph);
   if (!ph || fseeko(f, (off_t)eh.e_phoff, SEEK_SET) ||
       fread(ph, sizeof *ph, eh.e_phnum, f) != eh.e_phnum)
      goto out;

   const Elf64_Phdr *dynamic = NULL;
   for (size_t i = 0; i < eh.e_phnum; ++i)
      if (ph[i].p_type == PT_DYNAMIC) { dynamic = &ph[i]; break; }
   if (!dynamic || !dynamic->p_filesz) goto out;

   size_t ndyn = dynamic->p_filesz / sizeof(Elf64_Dyn);
   Elf64_Dyn *dyn = calloc(ndyn, sizeof *dyn);
   if (!dyn || fseeko(f, (off_t)dynamic->p_offset, SEEK_SET) ||
       fread(dyn, sizeof *dyn, ndyn, f) != ndyn) {
      free(dyn);
      goto out;
   }
   Elf64_Addr strtab_va = 0;
   for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; ++i)
      if (dyn[i].d_tag == DT_STRTAB) strtab_va = dyn[i].d_un.d_ptr;
   Elf64_Off strtab_off;
   if (!strtab_va || !a64_vaddr_to_offset(ph, eh.e_phnum,
                                           strtab_va, &strtab_off)) {
      free(dyn);
      goto out;
   }
   for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL && count < cap; ++i) {
      if (dyn[i].d_tag != DT_NEEDED) continue;
      if (fseeko(f, (off_t)(strtab_off + dyn[i].d_un.d_val), SEEK_SET)) continue;
      size_t len = 0;
      int ch;
      while (len < NAME_MAX && (ch = fgetc(f)) != EOF && ch != '\0')
         names[count][len++] = (char)ch;
      names[count][len] = '\0';
      if (len && (ch == '\0' || len == NAME_MAX)) ++count;
   }
   free(dyn);
out:
   free(ph);
   if (f) fclose(f);
   return count;
}

static void a64_preload_needed(const char *path, const char *dir,
                               char seen[][NAME_MAX + 1], size_t *seen_n)
{
   char needed[64][NAME_MAX + 1];
   size_t n = a64_read_needed(path, needed, 64);
   for (size_t i = 0; i < n; ++i) {
      char dep_path[PATH_MAX];
      struct stat st;
      if (a64_dep_seen(needed[i], seen, *seen_n)) continue;
      if (*seen_n < 128) {
         memcpy(seen[*seen_n], needed[i], NAME_MAX + 1);
         seen[*seen_n][NAME_MAX] = '\0';
         ++*seen_n;
      }
      size_t dir_len = strlen(dir), name_len = strnlen(needed[i], NAME_MAX + 1);
      if (dir_len + name_len + 1 > sizeof dep_path) {
         warnx("AArch64 dependency path too long: %s", needed[i]);
         continue;
      }
      memcpy(dep_path, dir, dir_len);
      memcpy(dep_path + dir_len, needed[i], name_len + 1);
      if (stat(dep_path, &st) != 0 || !arm64_elf_is_arm64(dep_path))
         continue; /* Android platform library: handled by the emulator. */
      a64_preload_needed(dep_path, dir, seen, seen_n);
      printf("preloading arm64 DT_NEEDED: %s\n", dep_path);
      if (arm64_exec_load_library(dep_path, 0) < 0)
         warnx("failed to preload AArch64 dependency %s", dep_path);
   }
}

static void svc_dump_handler(int sig) {
    (void)sig;
    arm_exec_svc_ring_dump();
}

/* libmono.so @ 0x20000000: mono_defaults struct and key fields */
static uint32_t mono_export_call(const char *sym)
{
   uint32_t va = arm_exec_lookup_export(sym);
   return va ? (uint32_t)arm_exec_call(va, 0, 0, 0, 0) : 0u;
}

static void dump_mono_defaults(const char *when)
{
   if (!getenv("LUNARIA_TRACE_MONO")) return;
   uint32_t base = arm_exec_lookup_export("mono_defaults");
   uint32_t corlib_fn = mono_export_call("mono_get_corlib");
   uint32_t object_fn = mono_export_call("mono_get_object_class");
   uint32_t root_fn  = mono_export_call("mono_get_root_domain");
   uint32_t corlib_asm = mono_export_call("mono_unity_assembly_get_mscorlib");
   if (base) {
      fprintf(stderr,
              "[mono] %s: defaults@%#x corlib=%#x object=%#x void=%#x byte=%#x int32=%#x string=%#x\n",
              when, base,
              arm_exec_read32(base + 0x00u),
              arm_exec_read32(base + 0x04u),
              arm_exec_read32(base + 0x0cu),
              arm_exec_read32(base + 0x08u),
              arm_exec_read32(base + 0x20u),
              arm_exec_read32(base + 0x44u));
   } else {
      fprintf(stderr, "[mono] %s: mono_defaults symbol not found\n", when);
   }
   fprintf(stderr, "[mono] %s: mono_get_corlib()=%#x mono_get_object_class()=%#x mono_get_root_domain()=%#x mono_unity_assembly_get_mscorlib()=%#x\n",
           when, corlib_fn, object_fn, root_fn, corlib_asm);
}

/* -------------------------------------------------------------------------
 * Minimal .dex reader: look up a method's JNI signature
 *
 * UE's GameActivity natives are not registered through RegisterNatives, so the
 * bridge binds them by their exported Java_… name and learns nothing about
 * their parameters.  Their signatures do change across engine versions —
 * nativeSetGlobalActivity is (ZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V
 * in UE4.20 and (ZZ…) from UE4.21 on, and nativeSetAndroidVersionInformation
 * gained a TargetSDK int and a build-number string along the way.  Calling one
 * version's layout on another shifts every later argument: the engine reads an
 * empty APK path, opens "" for the in-APK OBB and stops at "Failed to open
 * descriptor file <project>.uproject".
 *
 * The APK is already extracted for the AssetManager bridge, so read the real
 * signature out of classes*.dex and marshal against that instead of hardcoding
 * one engine version.
 * ---------------------------------------------------------------------- */

struct dex_view {
   const uint8_t *p;
   size_t         len;
};

static uint32_t dex_u32(const struct dex_view *d, size_t off)
{
   uint32_t v = 0;
   if (off + 4 <= d->len) memcpy(&v, d->p + off, 4);
   return v;
}

static uint16_t dex_u16(const struct dex_view *d, size_t off)
{
   uint16_t v = 0;
   if (off + 2 <= d->len) memcpy(&v, d->p + off, 2);
   return v;
}

static size_t dex_uleb(const struct dex_view *d, size_t off, uint32_t *out)
{
   uint32_t r = 0;
   int shift = 0;
   while (off < d->len && shift <= 28) {
      uint8_t b = d->p[off++];
      r |= (uint32_t)(b & 0x7f) << shift;
      if (!(b & 0x80)) break;
      shift += 7;
   }
   *out = r;
   return off;
}

/* MUTF-8 string by string_id index.  Returns a pointer into the mapping (the
 * bytes are NUL-terminated inside the file) or NULL. */
static const char *dex_string(const struct dex_view *d, uint32_t ids_off,
                              uint32_t ids_size, uint32_t idx)
{
   if (idx >= ids_size) return NULL;
   uint32_t off = dex_u32(d, ids_off + idx * 4u);
   uint32_t utf16_len;
   size_t p = dex_uleb(d, off, &utf16_len);
   if (p >= d->len) return NULL;
   if (!memchr(d->p + p, '\0', d->len - p)) return NULL;
   return (const char *)(d->p + p);
}

static int dex_find_sig(const struct dex_view *d, const char *class_desc,
                        const char *method, char *out, size_t out_sz)
{
   if (d->len < 112 || memcmp(d->p, "dex\n", 4) != 0) return 0;
   uint32_t str_size = dex_u32(d, 56), str_off = dex_u32(d, 60);
   uint32_t type_size = dex_u32(d, 64), type_off = dex_u32(d, 68);
   uint32_t proto_size = dex_u32(d, 72), proto_off = dex_u32(d, 76);
   uint32_t meth_size = dex_u32(d, 88), meth_off = dex_u32(d, 92);

#define DEX_TYPE(i) (((i) < type_size) \
      ? dex_string(d, str_off, str_size, dex_u32(d, type_off + (i) * 4u)) : NULL)

   for (uint32_t i = 0; i < meth_size; ++i) {
      size_t e = meth_off + (size_t)i * 8u;
      uint16_t cls_idx = dex_u16(d, e);
      uint16_t proto_idx = dex_u16(d, e + 2);
      uint32_t name_idx = dex_u32(d, e + 4);
      const char *name = dex_string(d, str_off, str_size, name_idx);
      if (!name || strcmp(name, method) != 0) continue;
      const char *cls = DEX_TYPE(cls_idx);
      if (!cls || strcmp(cls, class_desc) != 0) continue;
      if (proto_idx >= proto_size) return 0;

      size_t pe = proto_off + (size_t)proto_idx * 12u;
      uint32_t ret_idx = dex_u32(d, pe + 4);
      uint32_t params_off = dex_u32(d, pe + 8);
      size_t n = 0;
      if (n + 1 >= out_sz) return 0;
      out[n++] = '(';
      if (params_off) {
         uint32_t cnt = dex_u32(d, params_off);
         for (uint32_t k = 0; k < cnt; ++k) {
            const char *t = DEX_TYPE(dex_u16(d, params_off + 4u + k * 2u));
            if (!t) return 0;
            size_t tl = strlen(t);
            if (n + tl + 2 >= out_sz) return 0;
            memcpy(out + n, t, tl);
            n += tl;
         }
      }
      out[n++] = ')';
      const char *rt = DEX_TYPE(ret_idx);
      if (!rt) return 0;
      size_t rl = strlen(rt);
      if (n + rl + 1 >= out_sz) return 0;
      memcpy(out + n, rt, rl);
      n += rl;
      out[n] = '\0';
      return 1;
   }
#undef DEX_TYPE
   return 0;
}

/* Look a method up in every classes*.dex of the installed-package view.
 * `klass` is dotted or slashed ("com.epicgames.ue4.GameActivity"). */
static int
apk_method_signature(const char *klass, const char *method,
                     char *out, size_t out_sz)
{
   const char *dir = getenv("ANDROID_PACKAGE_CODE_PATH");
   if (!dir || !*dir || !klass || !method) return 0;

   char desc[256];
   size_t n = 0;
   desc[n++] = 'L';
   for (const char *c = klass; *c && n + 3 < sizeof desc; ++c)
      desc[n++] = (*c == '.') ? '/' : *c;
   desc[n++] = ';';
   desc[n] = '\0';

   /* Standard APK: <root>/classes*.dex.  App Bundle split (XAPK): the base
    * module's dex lives under base/. */
   static const char *const dex_dirs[] = { "", "base/" };
   for (size_t d = 0; d < sizeof dex_dirs / sizeof dex_dirs[0]; ++d)
   for (int i = 0; i < 32; ++i) {
      char path[PATH_MAX];
      if (i == 0) snprintf(path, sizeof path, "%s/%sclasses.dex", dir, dex_dirs[d]);
      else        snprintf(path, sizeof path, "%s/%sclasses%d.dex", dir,
                           dex_dirs[d], i + 1);
      FILE *f = fopen(path, "rb");
      if (!f) {
         if (i == 0) continue; /* the first file may be classes2.dex */
         break;
      }
      struct stat st;
      if (fstat(fileno(f), &st) != 0 || st.st_size < 112) { fclose(f); continue; }
      uint8_t *buf = malloc((size_t)st.st_size);
      if (!buf) { fclose(f); continue; }
      size_t got = fread(buf, 1, (size_t)st.st_size, f);
      fclose(f);
      struct dex_view d = { buf, got };
      int ok = dex_find_sig(&d, desc, method, out, out_sz);
      free(buf);
      if (ok) {
         fprintf(stderr, "[dex] %s.%s %s\n", klass, method, out);
         return 1;
      }
   }
   return 0;
}

/* Write one type letter per parameter of a JNI signature into `types`.
 * Returns the parameter count, or -1 if the signature is malformed. */
static int
jni_sig_params(const char *sig, char *types, int max)
{
   if (!sig || *sig != '(') return -1;
   int n = 0;
   for (const char *p = sig + 1; *p && *p != ')'; ) {
      if (n >= max) return -1;
      char t = *p;
      if (t == 'L') {
         const char *semi = strchr(p, ';');
         if (!semi) return -1;
         p = semi + 1;
      } else if (t == '[') {
         ++p;
         continue; /* array of the following type — same slot */
      } else {
         ++p;
      }
      types[n++] = t;
   }
   return n;
}

/* Parameter types of a GameActivity native, from the APK's dex.  Returns the
 * count, or -1 when the method is not in any dex (then the caller falls back
 * to the layout it knows). */
static int
ue_native_params(const char *method, char *types, int max)
{
   char sig[512];
   if (!apk_method_signature("com.epicgames.ue4.GameActivity", method,
                             sig, sizeof sig) &&
       !apk_method_signature("com.epicgames.unreal.GameActivity", method,
                             sig, sizeof sig))
      return -1;
   return jni_sig_params(sig, types, max);
}

/* UE mounts its expansion file one of two ways, and bOBBinAPK is what selects
 * between them:
 *   1 — "package data inside APK": the OBB zip is stored (uncompressed, hence
 *       the .png suffix) as the APK entry assets/main.obb.png, and the engine
 *       opens APKFilename itself to mount it;
 *   0 — a standalone expansion file: the engine looks for
 *       <obbdir>/main.<version>.<package>.obb, or uses the absolute paths
 *       given to nativeSetObbFilePaths.
 * FAndroidPlatformFile::Initialize takes one branch and never falls back to
 * the other — it logs "OBB not found in APK" (or finds no .obb) and mounts no
 * content at all, and the game dies on the first missing .uproject.  So this
 * has to be an observation of the package we hand the guest, not an inference
 * from which env vars the launcher happened to set: a title whose expansion
 * lives in the APK still has ANDROID_OBB_MAIN pointing at the extracted copy,
 * and an XAPK's base APK does not contain the split's OBB. */
static uint32_t
ue_obb_in_apk(void)
{
   return arm_exec_apk_has_entry("assets/main.obb.png") ? 1u : 0u;
}

/* Fill `out` with (env, thiz, …) for one of the UE startup natives, matching
 * whatever parameter list this engine version declares.  Ordered value lists
 * are bound per type: the position of a parameter among the parameters of its
 * own type is stable even when a version adds or drops one. */
static int
ue_build_startup_args(const char *method, const char *types, int np,
                      uint64_t env, uint64_t ctx,
                      const uint64_t *strs, int nstr_avail,
                      const uint64_t *ints, int nint_avail,
                      uint64_t obb_in_apk,
                      uint64_t *out, int max)
{
   int n = 0;
   if (max < 2 + np) return -1;
   out[n++] = env;
   out[n++] = ctx;

   int tb = 0, ts = 0;
   for (int i = 0; i < np; ++i) {
      if (types[i] == 'Z') ++tb;
      else if (types[i] == 'L') ++ts;
   }
   int nb = 0, ns = 0, ni = 0;
   for (int i = 0; i < np; ++i) {
      switch (types[i]) {
      case 'Z':
         /* bOBBinAPK is the last boolean, and only exists in the versions
          * that also take an APKFilename (the third string).  Everything
          * else here — bUseExternalFilesDir, bPublicLogFiles — is true. */
         if (!strcmp(method, "nativeSetGlobalActivity") && ts >= 3 && nb == tb - 1)
            out[n++] = obb_in_apk;
         else
            out[n++] = 1u;
         ++nb;
         break;
      case 'L':
         out[n++] = ns < nstr_avail ? strs[ns] : 0u;
         ++ns;
         break;
      default: /* I, J, F, D — integers in declaration order */
         out[n++] = ni < nint_avail ? ints[ni] : 0u;
         ++ni;
         break;
      }
   }
   return n;
}

static int
run_jni_game(struct jvm *jvm)
{
   // Works only with unity libs for now
   // XXX: What this basically is that, we port the Java bits to C
   // XXX: This will become unneccessary as we make dalvik interpreter

   struct {
      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jobject);
      } native_init_jni;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject);
      } native_done;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jstring);
      } native_file;

      union {
         void *ptr;
         jboolean (*fun)(JNIEnv*, jobject);
      } native_pause;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jint, jobject);
      } native_recreate_gfx_state;

      union {
         void *ptr;
         jboolean (*fun)(JNIEnv*, jobject);
      } native_render;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject);
      } native_resume;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jboolean);
      } native_focus_changed;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jstring);
      } native_set_input_string;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject);
      } native_soft_input_closed;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jboolean);
      } native_set_input_canceled;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jobject);
      } native_init_www;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jobject);
      } native_init_web_request;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jlong);
      } native_add_vsync_time;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jboolean);
      } native_forward_events_to_dalvik;

      union {
         void *ptr;
         void (*fun)(JNIEnv*, jobject, jobject);
      } native_inject_event;
   } unity;

   static const char *unity_player_class = "com.unity3d.player.UnityPlayer";
   unity.native_init_jni.ptr = jvm_get_native_method(jvm, unity_player_class, "initJni");
   unity.native_done.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeDone");
   unity.native_file.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeFile");
   unity.native_pause.ptr = jvm_get_native_method(jvm, unity_player_class, "nativePause");
   unity.native_recreate_gfx_state.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeRecreateGfxState");
   unity.native_render.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeRender");
   unity.native_resume.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeResume");
   unity.native_focus_changed.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeFocusChanged");
   unity.native_set_input_string.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeSetInputString");
   unity.native_soft_input_closed.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeSoftInputClosed");
   unity.native_set_input_canceled.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeSetInputCanceled");
   unity.native_init_www.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeInitWWW");
   unity.native_init_web_request.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeInitWebRequest");
   unity.native_add_vsync_time.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeAddVSyncTime");
   unity.native_forward_events_to_dalvik.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeForwardEventsToDalvik");
   unity.native_inject_event.ptr = jvm_get_native_method(jvm, unity_player_class, "nativeInjectEvent");

   if (!unity.native_init_jni.ptr)
      errx(EXIT_FAILURE, "not a unity jni lib");

   const jobject context = jvm->native.AllocObject(&jvm->env, jvm->native.FindClass(&jvm->env, "android/app/Activity"));

   if (unity.native_file.ptr) {
      const char *apk = lunaria_apk_mount_path();
      if (apk && *apk)
         unity.native_file.fun(&jvm->env, context, jvm->env->NewStringUTF(&jvm->env, apk));

      DIR *dir;
      const char *obb_dir = getenv("ANDROID_EXTERNAL_OBB_DIR");
      if (obb_dir && (dir = opendir(obb_dir))) {
         for (struct dirent *d; (d = readdir(dir));) {
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
               continue;

            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", obb_dir, d->d_name);
            unity.native_file.fun(&jvm->env, context, jvm->env->NewStringUTF(&jvm->env, path));
         }
      }
   }

   unity.native_init_jni.fun(&jvm->env, context, context);

   // unity.native_forward_events_to_dalvik.fun(&jvm->env, context, true);
   if (unity.native_init_www.ptr)
      unity.native_init_www.fun(&jvm->env, context, jvm->env->FindClass(&jvm->env, "com/unity3d/player/WWW"));
   if (unity.native_init_web_request.ptr)
      unity.native_init_web_request.fun(&jvm->env, context, jvm->env->FindClass(&jvm->env, "com/unity3d/player/UnityWebRequest"));
   unity.native_recreate_gfx_state.fun(&jvm->env, context, 0, context);
   unity.native_focus_changed.fun(&jvm->env, context, true);
   unity.native_resume.fun(&jvm->env, context);
   unity.native_done.fun(&jvm->env, context);
   // unity.native_add_vsync_time.fun(&jvm->env, context, 0);

   while (unity.native_render.fun(&jvm->env, context)) {
      static int i = 0;
      if (++i >= 10) {
         unity.native_inject_event.fun(&jvm->env, context, jvm->native.AllocObject(&jvm->env, jvm->native.FindClass(&jvm->env, "android/view/MotionEvent")));
         i = 0;
      }
   }

   return EXIT_SUCCESS;
}

static int
run_ue4_game_arm(struct jvm *jvm)
{
   /* UE4 GameActivity is a NativeActivity: entry is ANativeActivity_onCreate,
    * then the engine's android_main / looper thread drives the frame loop. */
   uint32_t va_oncreate = arm_exec_lookup_export("ANativeActivity_onCreate");
   if (!va_oncreate)
      errx(EXIT_FAILURE, "UE4: ANativeActivity_onCreate not found");

   if (!arm_exec_host_egl_init())
      fprintf(stderr, "[loader] host EGL re-init failed\n");

   jobject activity = jvm->native.AllocObject(&jvm->env,
         jvm->native.FindClass(&jvm->env, "com/epicgames/ue4/GameActivity"));
   if (!activity)
      activity = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "android/app/NativeActivity"));
   uint32_t act_va = arm_exec_native_activity_create(
         (uint32_t)(uintptr_t)activity);
   if (!act_va)
      errx(EXIT_FAILURE, "UE4: failed to allocate ANativeActivity");

   /* JNI hooks GameActivity.java calls before/during native startup.  These
    * are declared `native` in Java and exported from libUE4.so under the JNI
    * implicit-binding name (Java_com_epicgames_ue4_GameActivity_nativeXxx) —
    * they never go through RegisterNatives, so arm_exec_lookup_native must
    * fall back to the mangled export (it does).  Signatures come straight
    * from classes2.dex:
    *   nativeSetGlobalActivity (ZZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V
    *   nativeSetAndroidVersionInformation (Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V
    *   nativeSetObbInfo (Ljava/lang/String;Ljava/lang/String;IILjava/lang/String;)V
    *   nativeSetWindowInfo (ZI)V          <- (bIsPortrait, DepthBufferPreference)
    *   nativeSetSurfaceViewInfo (II)V     <- (width, height)
    *   nativeSetAndroidStartupState (Z)V
    *   nativeResumeMainInit ()V
    * AndroidMain() spins on `while (!GResumeMainInit) Sleep(0.01f)` right after
    * "Controller interface supported"; without nativeResumeMainInit the engine
    * never initialises and every frame presents an empty surface. */
   uint32_t va_set_global = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetGlobalActivity");
   uint32_t va_set_ver = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetAndroidVersionInformation");
   uint32_t va_set_obb = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetObbInfo");
   uint32_t va_set_obb_paths = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetObbFilePaths");
   uint32_t va_set_win = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetWindowInfo");
   uint32_t va_set_surf = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetSurfaceViewInfo");
   uint32_t va_startup_state = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetAndroidStartupState");
   uint32_t va_resume_init = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeResumeMainInit");
   uint32_t env = arm_exec_env_va();
   uint32_t ctx = (uint32_t)(uintptr_t)activity;

   const char *apk = getenv("ANDROID_APK_FILE");
   const char *pkg = getenv("ANDROID_PACKAGE_NAME");
   const char *ext = getenv("ANDROID_EXTERNAL_FILES_DIR");
   const char *obb_main = getenv("ANDROID_OBB_MAIN");
   const char *obb_patch = getenv("ANDROID_OBB_PATCH");
   if (!apk) apk = "";
   if (!pkg) pkg = "com.lunaria.app";
   if (!ext) ext = "/tmp";
   if (!obb_main) obb_main = "";
   if (!obb_patch) obb_patch = "";
   uint32_t obb_in_apk = ue_obb_in_apk();
   /* Prefer staged loose OBB over nested zip-in-APK (see arm64 path). */
   if (*obb_main && access(obb_main, R_OK) == 0) {
      if (obb_in_apk)
         fprintf(stderr, "[loader] prefer loose OBB %s over obbInAPK\n",
                 obb_main);
      obb_in_apk = 0;
   }

   if (va_set_global) {
      /* (env, thiz, bUseExternalFilesDir, bPublicLogFiles,
       *  internalFilePath, externalFilePath, bOBBinAPK, APKFilename) */
      jobject s_int = jvm->native.NewStringUTF(&jvm->env, ext);
      jobject s_ext = jvm->native.NewStringUTF(&jvm->env, ext);
      jobject s_apk = jvm->native.NewStringUTF(&jvm->env, apk);
      uint32_t a[8] = { env, ctx, 1u, 1u,
                        (uint32_t)(uintptr_t)s_int, (uint32_t)(uintptr_t)s_ext,
                        obb_in_apk, (uint32_t)(uintptr_t)s_apk };
      fprintf(stderr, "[loader] UE4 nativeSetGlobalActivity apk=%s files=%s obbInAPK=%u\n",
              apk, ext, obb_in_apk);
      arm_exec_calln(va_set_global, a, 8);
   }
   if (va_set_obb_paths && *obb_main && !obb_in_apk) {
      /* (env, thiz, OBBMainFilePath, OBBPatchFilePath,
       *  OBBOverflow1FilePath, OBBOverflow2FilePath) — absolute paths, which
       * take priority over the /sdcard/Android/obb/<pkg> search.  Skipped
       * when the expansion is in the APK: the engine mounts that itself and
       * these paths would send it looking for a file that is not there. */
      jobject s_main  = jvm->native.NewStringUTF(&jvm->env, obb_main);
      jobject s_patch = jvm->native.NewStringUTF(&jvm->env, obb_patch);
      jobject s_none  = jvm->native.NewStringUTF(&jvm->env, "");
      uint32_t a[6] = { env, ctx, (uint32_t)(uintptr_t)s_main,
                        (uint32_t)(uintptr_t)s_patch,
                        (uint32_t)(uintptr_t)s_none,
                        (uint32_t)(uintptr_t)s_none };
      fprintf(stderr, "[loader] UE4 nativeSetObbFilePaths main=%s\n", obb_main);
      arm_exec_calln(va_set_obb_paths, a, 6);
   }
   if (va_set_ver) {
      /* (env, thiz, AndroidVersion, TargetSDKversion, PhoneMake, PhoneModel,
       *  PhoneBuildNumber, OSLanguage) */
      jobject s_rel   = jvm->native.NewStringUTF(&jvm->env, "12");
      jobject s_make  = jvm->native.NewStringUTF(&jvm->env, "Lunaria");
      jobject s_model = jvm->native.NewStringUTF(&jvm->env, "Lunaria Emulator");
      jobject s_build = jvm->native.NewStringUTF(&jvm->env, "lunaria-1");
      jobject s_lang  = jvm->native.NewStringUTF(&jvm->env, "en");
      uint32_t a[8] = { env, ctx, (uint32_t)(uintptr_t)s_rel, 31u,
                        (uint32_t)(uintptr_t)s_make, (uint32_t)(uintptr_t)s_model,
                        (uint32_t)(uintptr_t)s_build, (uint32_t)(uintptr_t)s_lang };
      fprintf(stderr, "[loader] UE4 nativeSetAndroidVersionInformation\n");
      arm_exec_calln(va_set_ver, a, 8);
   }
   if (va_set_obb) {
      /* (env, thiz, ProjectName, PackageName, Version, PatchVersion, AppType) */
      const char *proj = strrchr(pkg, '.');
      proj = proj ? proj + 1 : pkg;
      jobject s_proj = jvm->native.NewStringUTF(&jvm->env, proj);
      jobject s_pkg  = jvm->native.NewStringUTF(&jvm->env, pkg);
      jobject s_type = jvm->native.NewStringUTF(&jvm->env, "");
      uint32_t a[7] = { env, ctx, (uint32_t)(uintptr_t)s_proj,
                        (uint32_t)(uintptr_t)s_pkg, 1u, 0u,
                        (uint32_t)(uintptr_t)s_type };
      fprintf(stderr, "[loader] UE4 nativeSetObbInfo project=%s package=%s\n", proj, pkg);
      arm_exec_calln(va_set_obb, a, 7);
   }

   fprintf(stderr, "[loader] UE4 ANativeActivity_onCreate @0x%08x act=0x%08x\n",
           va_oncreate, act_va);
   arm_exec_call_unlimited(va_oncreate, act_va, 0, 0, 0);
   arm_exec_run_pending_threads();
   fprintf(stderr, "[loader] UE4 onCreate returned\n");

   /* Do NOT call activity->callbacks->onStart/onResume/onNativeWindowCreated
    * synchronously: the NDK glue's onNativeWindowCreated waits on a cond until
    * android_main processes APP_CMD_INIT_WINDOW, which deadlocks if we hold the
    * main JIT.  Instead write APP_CMD_* into the android_app command pipe and
    * let the event thread drain them while we pump. */
   {
      uint32_t instance = arm_exec_read32(act_va + 28); /* ANativeActivity.instance */
      uint32_t win = arm_exec_native_window_va();
      fprintf(stderr, "[loader] UE4 android_app instance=0x%08x win=0x%08x\n",
              instance, win);
      if (instance) {
         /* Heuristic: scan android_app for a writable pipe fd pair.
          * Observed NDK layout (32-bit bionic): msgread @+0x48. */
         int msgwrite = -1;
         uint32_t pipe_off = 0;
         for (uint32_t off = 64; off < 256; off += 4) {
            int a = (int)arm_exec_read32(instance + off);
            int b = (int)arm_exec_read32(instance + off + 4);
            if (a > 2 && b > 2 && a < 1024 && b < 1024 && a != b) {
               struct stat sa, sb;
               if (fstat(a, &sa) == 0 && fstat(b, &sb) == 0 &&
                   S_ISFIFO(sa.st_mode) && S_ISFIFO(sb.st_mode)) {
                  msgwrite = b;
                  pipe_off = off;
                  fprintf(stderr, "[loader] UE4 cmd pipe @+0x%x read=%d write=%d\n",
                          off, a, b);
                  break;
               }
            }
         }
         if (win) {
            /* Public window @36; pendingWindow sits after mutex/cond/pipe/
             * thread/poll_sources/flags — typically msgread+0x38 (=0x80 when
             * msgread is 0x48).  process_cmd copies pendingWindow → window. */
            arm_exec_write32(instance + 36, win);
            uint32_t pend = pipe_off ? pipe_off + 0x38u : 0x80u;
            arm_exec_write32(instance + pend, win);
            fprintf(stderr, "[loader] UE4 set window@36 pendingWindow@+0x%x = 0x%08x\n",
                    pend, win);
         }
         if (msgwrite >= 0) {
            /* APP_CMD_START=10, RESUME=11, INIT_WINDOW=1, GAINED_FOCUS=6 */
            static const int8_t cmds[] = { 10, 11, 1, 6 };
            for (size_t i = 0; i < sizeof cmds; ++i) {
               int8_t c = cmds[i];
               if (write(msgwrite, &c, 1) != 1)
                  fprintf(stderr, "[loader] UE4 write APP_CMD %d failed\n", c);
               else
                  fprintf(stderr, "[loader] UE4 wrote APP_CMD %d\n", c);
               arm_exec_run_pending_threads();
            }
         }
      }
   }

   int w = arm_exec_fb_width(), h = arm_exec_fb_height();
   if (va_set_win) {
      /* (env, thiz, jboolean bIsPortrait, jint DepthBufferPreference).
       * NOT (width, height): passing the width here made every landscape
       * window report "portrait" and fed the height in as a depth-buffer
       * preference enum. */
      uint32_t portrait = (h > w) ? 1u : 0u;
      fprintf(stderr, "[loader] UE4 nativeSetWindowInfo portrait=%u depth=0\n", portrait);
      arm_exec_call(va_set_win, env, ctx, portrait, 0);
   }
   if (va_set_surf) {
      fprintf(stderr, "[loader] UE4 nativeSetSurfaceViewInfo %dx%d\n", w, h);
      arm_exec_call(va_set_surf, env, ctx, (uint32_t)w, (uint32_t)h);
   }
   if (va_startup_state) {
      /* (env, thiz, jboolean bDebuggerAttached) */
      fprintf(stderr, "[loader] UE4 nativeSetAndroidStartupState\n");
      arm_exec_call(va_startup_state, env, ctx, 0, 0);
   }
   if (va_resume_init) {
      /* Releases AndroidMain()'s `while (!GResumeMainInit)` spin so
       * FEngineLoop::PreInit + the game thread finally start. */
      fprintf(stderr, "[loader] UE4 nativeResumeMainInit\n");
      arm_exec_call(va_resume_init, env, ctx, 0, 0);
      arm_exec_run_pending_threads();
   }

   signal(SIGUSR1, svc_dump_handler);
   signal(SIGALRM, svc_dump_handler);
   alarm(30);

   int max_frames = 0;
   {
      const char *mf = getenv("LUNARIA_MAX_FRAMES");
      if (mf && *mf) max_frames = atoi(mf);
   }
   /* No default cap.  300 frames was a bring-up aid — about five seconds,
    * which UE spends mounting content and starting the task graph, so the
    * engine looked permanently stuck before its first frame.  The loop still
    * exits on guest abort and on the window close button, and
    * LUNARIA_MAX_FRAMES caps it for scripted runs. */

   fprintf(stderr, "[loader] UE4 entering pump loop (max_frames=%d)\n", max_frames);
   for (int frame = 0; max_frames <= 0 || frame < max_frames; ++frame) {
      if (arm_exec_guest_abort_count() > 0) {
         fprintf(stderr, "[loader] guest abort — stopping UE4 loop (frame %d)\n", frame);
         break;
      }
      arm_exec_run_pending_threads();
      arm_exec_egl_swap();
      arm_exec_glfw_poll();
      if (frame < 5 || frame % 50 == 0)
         fprintf(stderr, "[loader] UE4 pump frame %d\n", frame);
      if (arm_exec_glfw_should_close()) break;
      usleep(16000);
   }
   return EXIT_SUCCESS;
}

/* -------------------------------------------------------------------------
 * ARM64 UE NativeActivity pump
 * ---------------------------------------------------------------------- */
/* UE ships the same GameActivity under two packages: com.epicgames.ue4 for
 * UE4 and com.epicgames.unreal for UE5.  Look a native method up in both. */
static uint64_t
ue4_native64(const char *method)
{
   uint64_t va = arm64_exec_lookup_native("com.epicgames.ue4.GameActivity", method);
   if (!va)
      va = arm64_exec_lookup_native("com.epicgames.unreal.GameActivity", method);
   return va;
}

static int
run_ue4_game_arm64(struct jvm *jvm)
{
   uint64_t va_oncreate = arm64_exec_lookup_export("ANativeActivity_onCreate");
   if (!va_oncreate)
      errx(EXIT_FAILURE, "UE arm64: ANativeActivity_onCreate not found");

   /* android_native_app_glue runs AndroidMain — and therefore FEngineLoop and
    * the whole game — on a guest pthread, so the scheduler's slices are the
    * engine's entire CPU budget rather than background maintenance. */
   arm64_exec_threads_run_engine(1);

   if (!arm64_exec_host_egl_init())
      fprintf(stderr, "[loader] arm64 host EGL init failed\n");

   jobject activity = jvm->native.AllocObject(&jvm->env,
         jvm->native.FindClass(&jvm->env, "com/epicgames/ue4/GameActivity"));
   if (!activity)
      activity = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "com/epicgames/unreal/GameActivity"));
   if (!activity)
      activity = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "android/app/NativeActivity"));
   uint64_t act_va = arm64_exec_native_activity_create((uint64_t)(uintptr_t)activity);
   if (!act_va)
      errx(EXIT_FAILURE, "UE arm64: failed to allocate ANativeActivity");

   /* GameActivity.java calls these `native` methods before and around native
    * startup.  They are exported under the JNI implicit-binding name rather
    * than registered through RegisterNatives, which arm64_exec_lookup_native
    * already falls back to.  Their parameters, whose *presence* varies by
    * engine version (the actual list comes from the APK's dex — see
    * ue_native_params):
    *   nativeSetGlobalActivity   bUseExternalFilesDir, [bPublicLogFiles],
    *                             internalFilePath, externalFilePath,
    *                             bOBBinAPK, APKFilename
    *   nativeSetAndroidVersionInformation
    *                             AndroidVersion, [TargetSDKversion],
    *                             PhoneMake, PhoneModel, [PhoneBuildNumber],
    *                             OSLanguage
    *   nativeSetObbInfo          ProjectName, PackageName, Version,
    *                             PatchVersion, AppType
    *   nativeSetObbFilePaths     main, patch, overflow1, overflow2
    *   nativeSetWindowInfo       bIsPortrait, DepthBufferPreference
    *   nativeSetSurfaceViewInfo  width, height
    *   nativeSetAndroidStartupState  bDebuggerAttached
    *   nativeResumeMainInit      ()
    * AndroidMain() spins on `while (!GResumeMainInit) Sleep(0.01f)` right
    * after "Controller interface supported"; without nativeResumeMainInit
    * FEngineLoop::PreInit never runs and every frame presents an empty
    * surface.  The A32 path has always done this — the A64 path went straight
    * from onCreate to the pump loop, so the engine never initialised. */
   uint64_t va_set_global     = ue4_native64("nativeSetGlobalActivity");
   uint64_t va_set_ver        = ue4_native64("nativeSetAndroidVersionInformation");
   uint64_t va_set_obb        = ue4_native64("nativeSetObbInfo");
   uint64_t va_set_obb_paths  = ue4_native64("nativeSetObbFilePaths");
   uint64_t va_set_win        = ue4_native64("nativeSetWindowInfo");
   uint64_t va_set_surf       = ue4_native64("nativeSetSurfaceViewInfo");
   uint64_t va_startup_state  = ue4_native64("nativeSetAndroidStartupState");
   uint64_t va_resume_init    = ue4_native64("nativeResumeMainInit");
   uint64_t env = arm64_exec_env_va();
   uint64_t ctx = (uint64_t)(uintptr_t)activity;

   const char *apk = getenv("ANDROID_APK_FILE");
   const char *pkg = getenv("ANDROID_PACKAGE_NAME");
   const char *ext = getenv("ANDROID_EXTERNAL_FILES_DIR");
   const char *obb_main = getenv("ANDROID_OBB_MAIN");
   const char *obb_patch = getenv("ANDROID_OBB_PATCH");
   if (!apk) apk = "";
   if (!pkg) pkg = "com.lunaria.app";
   if (!ext) ext = "/tmp";
   if (!obb_main) obb_main = "";
   if (!obb_patch) obb_patch = "";
   uint64_t obb_in_apk = ue_obb_in_apk();
   /* Prefer a staged loose OBB (lunaria-apk.sh exports ANDROID_OBB_MAIN to
    * the extracted assets/main.obb.png) over nested zip-in-APK mounting.
    * With obbInAPK=1 alone, UE's in-APK OBB reader has left Content/Paks
    * unresolved (host access → -1) and the scene empty — only post-process
    * of black RTs.  Passing the real file via nativeSetObbFilePaths mounts
    * the expansion the same way a Play Store OBB would. */
   if (*obb_main && access(obb_main, R_OK) == 0) {
      if (obb_in_apk)
         fprintf(stderr, "[loader] prefer loose OBB %s over obbInAPK\n",
                 obb_main);
      obb_in_apk = 0;
   }

   if (va_set_global) {
      /* (env, thiz, [bUseExternalFilesDir], [bPublicLogFiles],
       *  internalFilePath, externalFilePath, [bOBBinAPK], [APKFilename]). */
      uint64_t strs[3] = {
         (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, ext),
         (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, ext),
         (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, apk),
      };
      char types[16] = "ZZLLZL"; /* UE4.21+ default when there is no dex */
      int np = ue_native_params("nativeSetGlobalActivity", types, (int)sizeof types);
      if (np < 0) np = 6;
      uint64_t a[16];
      int na = ue_build_startup_args("nativeSetGlobalActivity", types, np,
                                     env, ctx, strs, 3, NULL, 0, obb_in_apk,
                                     a, (int)(sizeof a / sizeof a[0]));
      fprintf(stderr, "[loader] UE arm64 nativeSetGlobalActivity apk=%s files=%s obbInAPK=%llu (%d args)\n",
              apk, ext, (unsigned long long)obb_in_apk, na);
      if (na > 0) arm64_exec_call8(va_set_global, a, na);
   }
   if (va_set_obb_paths && *obb_main && !obb_in_apk) {
      /* (env, thiz, OBBMainFilePath, OBBPatchFilePath,
       *  OBBOverflow1FilePath, OBBOverflow2FilePath) — absolute paths, which
       * take priority over the /sdcard/Android/obb/<pkg> search.  Skipped
       * when the expansion is in the APK: the engine mounts that itself and
       * these paths would send it looking for a file that is not there. */
      jobject s_main  = jvm->native.NewStringUTF(&jvm->env, obb_main);
      jobject s_patch = jvm->native.NewStringUTF(&jvm->env, obb_patch);
      jobject s_none  = jvm->native.NewStringUTF(&jvm->env, "");
      uint64_t a[6] = { env, ctx, (uint64_t)(uintptr_t)s_main,
                        (uint64_t)(uintptr_t)s_patch,
                        (uint64_t)(uintptr_t)s_none,
                        (uint64_t)(uintptr_t)s_none };
      fprintf(stderr, "[loader] UE arm64 nativeSetObbFilePaths main=%s\n", obb_main);
      arm64_exec_call8(va_set_obb_paths, a, 6);
   }
   if (va_set_ver) {
      /* (env, thiz, AndroidVersion, [TargetSDKversion], PhoneMake, PhoneModel,
       *  [PhoneBuildNumber], OSLanguage) — the SDK int and the build-number
       * string only exist from UE4.25 on. */
      char types[16] = "LILLLL"; /* UE4.25+ default when there is no dex */
      int np = ue_native_params("nativeSetAndroidVersionInformation",
                                types, (int)sizeof types);
      if (np < 0) np = 6;
      int nstr = 0;
      for (int i = 0; i < np; ++i) if (types[i] == 'L') ++nstr;
      uint64_t strs[5];
      int k = 0;
      strs[k++] = (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, "12");
      strs[k++] = (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, "Lunaria");
      strs[k++] = (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, "Lunaria Emulator");
      if (nstr >= 5) /* PhoneBuildNumber only in the longer form */
         strs[k++] = (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, "lunaria-1");
      strs[k++] = (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, "en");
      uint64_t ints[1] = { 31u }; /* TargetSDKversion, matches Build.VERSION */
      uint64_t a[16];
      int na = ue_build_startup_args("nativeSetAndroidVersionInformation", types, np,
                                     env, ctx, strs, k, ints, 1, 0, a,
                                     (int)(sizeof a / sizeof a[0]));
      fprintf(stderr, "[loader] UE arm64 nativeSetAndroidVersionInformation (%d args)\n", na);
      if (na > 0) arm64_exec_call8(va_set_ver, a, na);
   }
   if (va_set_obb) {
      /* (env, thiz, ProjectName, PackageName, Version, PatchVersion, AppType) */
      const char *proj = strrchr(pkg, '.');
      proj = proj ? proj + 1 : pkg;
      uint64_t strs[3] = {
         (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, proj),
         (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, pkg),
         (uint64_t)(uintptr_t)jvm->native.NewStringUTF(&jvm->env, ""),
      };
      uint64_t ints[2] = { 1u, 0u }; /* Version, PatchVersion */
      char types[16] = "LLIIL";
      int np = ue_native_params("nativeSetObbInfo", types, (int)sizeof types);
      if (np < 0) np = 5;
      uint64_t a[16];
      int na = ue_build_startup_args("nativeSetObbInfo", types, np,
                                     env, ctx, strs, 3, ints, 2, 0, a,
                                     (int)(sizeof a / sizeof a[0]));
      fprintf(stderr, "[loader] UE arm64 nativeSetObbInfo project=%s package=%s (%d args)\n",
              proj, pkg, na);
      if (na > 0) arm64_exec_call8(va_set_obb, a, na);
   }

   fprintf(stderr, "[loader] UE arm64 ANativeActivity_onCreate @0x%llx act=0x%llx\n",
           (unsigned long long)va_oncreate, (unsigned long long)act_va);
   arm64_exec_call_unlimited(va_oncreate, act_va, 0, 0, 0);
   arm64_exec_run_pending_threads();
   fprintf(stderr, "[loader] UE arm64 onCreate returned\n");

   /* Deliver APP_CMD_* via the android_app command pipe.  Calling
    * activity->callbacks->onNativeWindowCreated directly would block on the
    * glue's condition variable until android_main drains the command, which
    * cannot happen while we hold the JIT. */
   {
      uint64_t instance_va = arm64_exec_read64(act_va + 56); /* ANativeActivity.instance */
      uint64_t win_va      = arm64_exec_native_window_va();
      fprintf(stderr, "[loader] UE arm64 android_app instance=0x%llx win=0x%llx\n",
              (unsigned long long)instance_va, (unsigned long long)win_va);
      if (instance_va) {
         int msgwrite = -1;
         uint32_t pipe_off = 0;
         /* Scan android_app for the command pipe's fd pair.  On LP64 bionic
          * (pthread_mutex_t 40 B, pthread_cond_t 48 B) msgread lands at
          * +0xC0; the scan keeps this working if the glue struct shifts. */
         for (uint32_t off = 64; off < 512; off += 4) {
            int a = (int)arm64_exec_read32(instance_va + off);
            int b = (int)arm64_exec_read32(instance_va + off + 4);
            if (a > 2 && b > 2 && a < 1024 && b < 1024 && a != b) {
               struct stat sa, sb;
               if (fstat(a, &sa) == 0 && fstat(b, &sb) == 0 &&
                   S_ISFIFO(sa.st_mode) && S_ISFIFO(sb.st_mode)) {
                  msgwrite = b; pipe_off = off;
                  fprintf(stderr, "[loader] UE arm64 cmd pipe @+0x%x write=%d\n", off, b);
                  break;
               }
            }
         }
         if (win_va) {
            /* LP64 android_app: window @+0x48 (it precedes the mutex, so its
             * offset does not depend on the pthread type sizes), and
             * pendingWindow @ msgread+0x58:
             *   msgread +0x00, msgwrite +0x04, thread +0x08,
             *   inputPollSource +0x10 (24 B), cmdPollSource +0x28 (24 B),
             *   running/stateSaved/destroyed/redrawNeeded +0x40..+0x4C,
             *   pendingInputQueue +0x50, pendingWindow +0x58.
             * The A32 numbers (window@36, pendingWindow@msgread+0x38) were
             * carried over unchanged and wrote the window pointer over
             * savedState/savedStateSize instead. */
            arm64_exec_write64(instance_va + 0x48, win_va);
            uint64_t pend = pipe_off ? (uint64_t)pipe_off + 0x58u : 0x118u;
            arm64_exec_write64(instance_va + pend, win_va);
            fprintf(stderr, "[loader] UE arm64 window@+0x48 pendingWindow@+0x%llx = 0x%llx\n",
                    (unsigned long long)pend, (unsigned long long)win_va);
         }
         if (msgwrite >= 0) {
            /* APP_CMD_START=10, RESUME=11, INIT_WINDOW=1, GAINED_FOCUS=6 */
            static const int8_t cmds[] = { 10, 11, 1, 6 };
            for (size_t i = 0; i < sizeof cmds; ++i) {
               int8_t c = cmds[i];
               if (write(msgwrite, &c, 1) != 1)
                  fprintf(stderr, "[loader] UE arm64 write APP_CMD %d failed\n", c);
               arm64_exec_run_pending_threads();
            }
         }
      }
   }

   int w = arm64_exec_fb_width(), h = arm64_exec_fb_height();
   if (va_set_win) {
      /* (env, thiz, jboolean bIsPortrait, jint DepthBufferPreference).
       * NOT (width, height): passing the width here makes every landscape
       * window report "portrait" and feeds the height in as a depth-buffer
       * preference enum. */
      uint64_t portrait = (h > w) ? 1u : 0u;
      fprintf(stderr, "[loader] UE arm64 nativeSetWindowInfo portrait=%llu depth=0\n",
              (unsigned long long)portrait);
      arm64_exec_call(va_set_win, env, ctx, portrait, 0);
   }
   if (va_set_surf) {
      fprintf(stderr, "[loader] UE arm64 nativeSetSurfaceViewInfo %dx%d\n", w, h);
      arm64_exec_call(va_set_surf, env, ctx, (uint64_t)w, (uint64_t)h);
   }
   if (va_startup_state) {
      /* (env, thiz, jboolean bDebuggerAttached) */
      fprintf(stderr, "[loader] UE arm64 nativeSetAndroidStartupState\n");
      arm64_exec_call(va_startup_state, env, ctx, 0, 0);
   }
   if (va_resume_init) {
      /* Releases AndroidMain()'s `while (!GResumeMainInit)` spin so
       * FEngineLoop::PreInit + the game thread finally start. */
      fprintf(stderr, "[loader] UE arm64 nativeResumeMainInit\n");
      arm64_exec_call(va_resume_init, env, ctx, 0, 0);
      arm64_exec_run_pending_threads();
   }

   signal(SIGUSR1, svc_dump_handler);
   signal(SIGALRM, svc_dump_handler);
   alarm(30);

   int max_frames = 0;
   { const char *mf = getenv("LUNARIA_MAX_FRAMES"); if (mf && *mf) max_frames = atoi(mf); }
   /* No default cap — see the A32 pump loop. */

   fprintf(stderr, "[loader] UE arm64 entering pump loop (max_frames=%d)\n", max_frames);
   for (int frame = 0; max_frames <= 0 || frame < max_frames; ++frame) {
      if (arm_exec_guest_abort_count() > 0) {
         fprintf(stderr, "[loader] guest abort — stopping UE arm64 loop (frame %d)\n", frame);
         break;
      }
      arm64_exec_run_pending_threads();
      /* The engine renders on its own thread and swaps through the EGL
       * bridge; present here too so a frame reaches the window even when the
       * guest's swap goes through the Java surface path. */
      arm64_exec_egl_swap();
      arm64_exec_glfw_poll();
      if (frame < 5 || frame % 50 == 0)
         fprintf(stderr, "[loader] UE arm64 pump frame %d\n", frame);
      if (arm64_exec_glfw_should_close()) break;
      usleep(16000);
   }
   return EXIT_SUCCESS;
}

static int
run_unity_game_arm64(struct jvm *jvm)
{
   static const char *cls     = "com.unity3d.player.UnityPlayer";
   static const char *cls_svc = "com.unity3d.player.UnityPlayerForActivityOrService";

#define LOOKUP2(name) \
   (arm64_exec_lookup_native(cls, name) ?: arm64_exec_lookup_native(cls_svc, name))
#define LOOKUP2_SIG(name, sig_buf) \
   (arm64_exec_lookup_native_sig(cls, name, sig_buf, sizeof(sig_buf)) ?: \
    arm64_exec_lookup_native_sig(cls_svc, name, sig_buf, sizeof(sig_buf)))

   uint64_t va_init_jni = arm64_exec_lookup_native(cls, "initJni");
   uint64_t va_done     = LOOKUP2("nativeDone");
   uint64_t va_render   = LOOKUP2("nativeRender");
   uint64_t va_resume   = LOOKUP2("nativeResume");
   uint64_t va_focus    = LOOKUP2("nativeFocusChanged");
   char recreate_sig[128] = {0};
   uint64_t va_recreate = LOOKUP2_SIG("nativeRecreateGfxState", recreate_sig);
   uint64_t va_inject   = arm64_exec_lookup_native(cls, "nativeInjectEvent");
   uint64_t va_file     = arm64_exec_lookup_native(cls, "nativeFile");
   uint64_t va_resize   = LOOKUP2("nativeResize");
   uint64_t va_fwd_dalv = LOOKUP2("nativeForwardEventsToDalvik");

#undef LOOKUP2
#undef LOOKUP2_SIG

   if (!va_init_jni || !va_render)
      errx(EXIT_FAILURE, "not a unity jni lib (arm64)");

   uint64_t env = arm64_exec_env_va();
   const jobject context = jvm->native.AllocObject(&jvm->env,
         jvm->native.FindClass(&jvm->env, "android/app/Activity"));
   uint64_t ctx = (uint64_t)(uintptr_t)context;

   if (va_file) {
      const char *apk = lunaria_apk_mount_path();
      if (apk && *apk) {
         fprintf(stderr, "[loader] arm64 calling nativeFile (%s)...\n", apk);
         jobject str = jvm->native.NewStringUTF(&jvm->env, apk);
         arm64_exec_call(va_file, env, ctx, (uint64_t)(uintptr_t)str, 0);
         arm64_exec_run_pending_threads();
      }
   }

   fprintf(stderr, "[loader] arm64 calling initJni (va=0x%llx)...\n",
           (unsigned long long)va_init_jni);
   arm64_exec_call(va_init_jni, env, ctx, ctx, 0);
   arm64_exec_run_pending_threads();
   fprintf(stderr, "[loader] arm64 initJni done\n");

   if (!arm64_exec_host_egl_init())
      fprintf(stderr, "[loader] arm64 host EGL re-init failed\n");

   if (va_recreate) {
      const jobject fake_surf = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "android/view/Surface"));
      int unity4_sig = (recreate_sig[0] == '(' && recreate_sig[1] == 'L');
      if (unity4_sig) {
         fprintf(stderr, "[loader] arm64 nativeRecreateGfxState (Unity4)...\n");
         arm64_exec_call(va_recreate, env, ctx, (uint64_t)(uintptr_t)fake_surf, 0);
      } else {
         fprintf(stderr, "[loader] arm64 nativeRecreateGfxState (displayId=0)...\n");
         arm64_exec_call(va_recreate, env, ctx, 0, (uint64_t)(uintptr_t)fake_surf);
      }
      arm64_exec_run_pending_threads();
   }

   if (va_resize) {
      int w = arm64_exec_fb_width(), h = arm64_exec_fb_height();
      fprintf(stderr, "[loader] arm64 nativeResize(%d,%d)...\n", w, h);
      arm64_exec_call6(va_resize, env, ctx, (uint64_t)w, (uint64_t)h,
                       (uint64_t)w, (uint64_t)h);
   }
   if (va_focus)
      arm64_exec_call(va_focus, env, ctx, 1, 0);
   if (va_fwd_dalv)
      arm64_exec_call(va_fwd_dalv, env, ctx, 0, 0);
   if (va_resume)
      arm64_exec_call(va_resume, env, ctx, 0, 0);
   arm64_exec_run_pending_threads();

   fprintf(stderr, "[loader] arm64 entering Unity render loop\n");
   signal(SIGUSR1, svc_dump_handler);
   signal(SIGALRM, svc_dump_handler);
   alarm(30);

   int frame_count = 0, fail_streak = 0, last_ok = -1, resized_after_init = 0;
   int max_frames = 0;
   {
      const char *mf = getenv("LUNARIA_MAX_FRAMES");
      if (mf && *mf) max_frames = atoi(mf);
   }
   /* No default cap — same as the A32 render loop below.  The 300-frame limit
    * here was a bring-up aid, but Unity's splash runs on real time and the
    * game only reaches its first scene well past that, so it made arm64 look
    * permanently stuck on the splash screen. */

   for (;;) {
      if (max_frames > 0 && frame_count >= max_frames) {
         fprintf(stderr, "[loader] LUNARIA_MAX_FRAMES=%d reached\n", max_frames);
         break;
      }
      if (va_inject && arm_exec_touch_next()) {
         static jobject motion_ev;
         if (!motion_ev)
            motion_ev = jvm->native.AllocObject(&jvm->env,
                  jvm->native.FindClass(&jvm->env, "android/view/MotionEvent"));
         if (va_fwd_dalv)
            arm64_exec_call(va_fwd_dalv, env, ctx, 0, 0);
         arm64_exec_call(va_inject, env, ctx, (uint64_t)(uintptr_t)motion_ev, 0);
      }

      arm_exec_drain_gl_thread_jobs();
      int ok = (int)arm64_exec_call_unlimited(va_render, env, ctx, 0, 0);

      if (!resized_after_init && frame_count >= 1 && va_resize) {
         int w = arm64_exec_fb_width(), h = arm64_exec_fb_height();
         arm64_exec_call6(va_resize, env, ctx, (uint64_t)w, (uint64_t)h,
                          (uint64_t)w, (uint64_t)h);
         resized_after_init = 1;
      }

      arm64_exec_run_pending_threads();
      arm64_exec_egl_swap();
      arm64_exec_glfw_poll();
      ++frame_count;
      if (ok != last_ok || frame_count <= 5 || (frame_count % 50 == 0)) {
         fprintf(stderr, "[loader] arm64 nativeRender → %d (frame %d)\n",
                 ok, frame_count);
         last_ok = ok;
      }
      fail_streak = ok ? 0 : fail_streak + 1;
      if (fail_streak > 3)
         usleep(16000);
      if (arm64_exec_glfw_should_close()) break;
   }

   if (va_done)
      arm64_exec_call(va_done, env, ctx, 0, 0);
   return EXIT_SUCCESS;
}

static int
run_jni_game_arm64(struct jvm *jvm)
{
   /* UE NativeActivity path */
   if (arm64_exec_lookup_export("ANativeActivity_onCreate"))
      return run_ue4_game_arm64(jvm);
   /* UnityPlayer.initJni path (IL2CPP / Mono) */
   if (arm64_exec_lookup_native("com.unity3d.player.UnityPlayer", "initJni"))
      return run_unity_game_arm64(jvm);
   fprintf(stderr, "[loader] arm64: no known entry point\n");
   return EXIT_FAILURE;
}

static int
run_jni_game_arm(struct jvm *jvm)
{
   /* UE4 NativeActivity path (no UnityPlayer.initJni) */
   if (arm_exec_lookup_export("ANativeActivity_onCreate") &&
       !arm_exec_lookup_native("com.unity3d.player.UnityPlayer", "initJni"))
      return run_ue4_game_arm(jvm);

   /* Unity <= 2019: all methods on UnityPlayer.
    * Unity 2020+:  render/lifecycle moved to UnityPlayerForActivityOrService. */
   static const char *cls      = "com.unity3d.player.UnityPlayer";
   static const char *cls_svc  = "com.unity3d.player.UnityPlayerForActivityOrService";

   /* Helper: look up from primary class, fall back to the service class */
#define LOOKUP2(name) \
   (arm_exec_lookup_native(cls, name) ?: arm_exec_lookup_native(cls_svc, name))
#define LOOKUP2_SIG(name, sig_buf) \
   (arm_exec_lookup_native_sig(cls, name, sig_buf, sizeof(sig_buf)) ?: \
    arm_exec_lookup_native_sig(cls_svc, name, sig_buf, sizeof(sig_buf)))

   uint32_t va_init_jni = arm_exec_lookup_native(cls, "initJni");
   uint32_t va_done     = LOOKUP2("nativeDone");
   uint32_t va_render   = LOOKUP2("nativeRender");
   uint32_t va_resume   = LOOKUP2("nativeResume");
   uint32_t va_focus    = LOOKUP2("nativeFocusChanged");
   char recreate_sig[128] = {0};
   uint32_t va_recreate = LOOKUP2_SIG("nativeRecreateGfxState", recreate_sig);
   uint32_t va_inject   = arm_exec_lookup_native(cls, "nativeInjectEvent");
   uint32_t va_file     = arm_exec_lookup_native(cls, "nativeFile");
   uint32_t va_resize   = LOOKUP2("nativeResize");
   uint32_t va_fwd_dalv = LOOKUP2("nativeForwardEventsToDalvik");
   uint32_t fwd_flag_va = 0; /* Unity 5.3 ForwardEventsToDalvik BSS byte */

#undef LOOKUP2
#undef LOOKUP2_SIG

   if (!va_init_jni || !va_render)
      errx(EXIT_FAILURE, "not a unity jni lib");

   uint32_t env = arm_exec_env_va();
   const jobject context = jvm->native.AllocObject(&jvm->env, jvm->native.FindClass(&jvm->env, "android/app/Activity"));
   uint32_t ctx = (uint32_t)(uintptr_t)context;
   uint32_t mono_root = mono_export_call("mono_get_root_domain");

   /* JIT trampolines must exist before initJni — Unity 4.x maps mscorlib inside
    * initJni, and mono branches into uninitialized codeman slots without mini_init. */
   arm_exec_ensure_mono_trampolines();
   dump_mono_defaults("after mono trampolines");

   /* Mount the APK before initJni.  On a real device UnityPlayer passes
    * getPackageCodePath() (the .apk file) via nativeFile during construction,
    * before initJni reads assets/bin/Data.  Calling nativeFile after initJni
    * leaves ArchiveFileSystem unmounted and splash upload runs with unset
    * texture callbacks. */
   if (va_file) {
      const char *apk = lunaria_apk_mount_path();
      if (apk && *apk) {
         fprintf(stderr, "[loader] calling nativeFile (%s)...\n", apk);
         jobject str = jvm->native.NewStringUTF(&jvm->env, apk);
         arm_exec_call(va_file, env, ctx, (uint32_t)(uintptr_t)str, 0);
         arm_exec_run_pending_threads();
      }
   }

   fprintf(stderr, "[loader] calling initJni (va=0x%08x) mono_root_domain=0x%08x...\n",
           va_init_jni, mono_root);

   arm_exec_call(va_init_jni, env, ctx, ctx, 0);
   arm_exec_run_pending_threads();
   fprintf(stderr, "[loader] initJni done: mono_root_domain=0x%08x\n",
           mono_export_call("mono_get_root_domain"));
   dump_mono_defaults("after initJni");

   /* Unity registers paths in initJni; register embedded machine.config before
    * nativeRender calls mono_jit_init_version (gmisc-unix.c:69 otherwise). */
   arm_exec_prepare_mono_config();
   arm_exec_sync_mono_domain_slot();

   /* mono_file_map_open/… are hooked via SVC; leave Unity's override from initJni
    * unless it blocks our hooks (cleared on libunity load if needed). */

   /* EGL は main() の arm_exec_jni_onload より前に初期化済み。
    * 念のため再度 make-current を試みる（arm_exec_host_egl_init は冪等）。 */
   if (!arm_exec_host_egl_init())
      fprintf(stderr, "[loader] host EGL re-init failed — GL calls may be no-ops\n");

   /* initJni/threads may overflow the ARM stack if Mono recurses deeply.
    * Reset saved R4-R11 so nativeRecreateGfxState starts with a clean register state. */
   arm_exec_reset_saved_regs();

   if (va_recreate) {
      const jobject fake_surf = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "android/view/Surface"));
      /* Unity 4.x: nativeRecreateGfxState(Landroid/view/Surface;)V  — Surface only
       * Unity 5+ : nativeRecreateGfxState(ILandroid/view/Surface;)V — displayId + Surface
       * Java 側は updateGLDisplay(0, surface) で主ディスプレイ=0 を渡す。
       * 1 を渡すと Surface が G[1] に格納され、メインスレッドが待つ G[0]
       * (libunity 0x02e601a0 相当) が永遠に 0 のまま cond_wait でデッドロック
       * する (Unity 2023 IL2CPP で確認)。 */
      int unity4_sig = (recreate_sig[0] == '(' && recreate_sig[1] == 'L');
      if (unity4_sig) {
         fprintf(stderr, "[loader] calling nativeRecreateGfxState (Unity4 surface-only)...\n");
         arm_exec_call(va_recreate, env, ctx, (uint32_t)(uintptr_t)fake_surf, 0);
      } else {
         fprintf(stderr, "[loader] calling nativeRecreateGfxState (displayId=0)...\n");
         arm_exec_call(va_recreate, env, ctx, 0, (uint32_t)(uintptr_t)fake_surf);
      }
      arm_exec_run_pending_threads();
      fprintf(stderr, "[loader] nativeRecreateGfxState done\n");
   }
   dump_mono_defaults("after nativeRecreateGfxState");
   /* ウィンドウサイズを通知。nativeResize は (IIII)V = (w, h, texW, texH):
    * libunity は touch スケールを xscale=w/texW, yscale=h/texH で計算する
    * (libunity+0x39c7d0)。texW/texH は AAPCS でスタック渡しのため
    * arm_exec_call (レジスタ 4 本のみ) だと 0 を読み scale=inf になり、
    * 以後の全タッチ座標が inf に化ける (GUI.Button が反応しない真因)。 */
   fprintf(stderr, "[loader] calling nativeResize...\n");
   if (va_resize) {
      int w = arm_exec_fb_width(), h = arm_exec_fb_height();
      arm_exec_call6(va_resize, env, ctx, (uint32_t)w, (uint32_t)h,
                     (uint32_t)w, (uint32_t)h);
   }
   fprintf(stderr, "[loader] calling nativeFocusChanged...\n");
   if (va_focus)
      arm_exec_call(va_focus, env, ctx, 1, 0);
   /* APK meta-data unityplayer.ForwardNativeEventsToDalvik=true makes
    * Unity 5.x nativeInjectEvent skip the native queue (return 0) when the
    * forward-to-Dalvik flag byte is set.  We have no Dalvik touch dispatch —
    * inject is our only path — so force the flag clear.  The JNI call itself
    * can no-op if Unity's Scoped* TLS (+0x104) is busy; poke the BSS byte
    * when the Unity 5.3 strb pattern matches. */
   if (va_fwd_dalv) {
      fprintf(stderr, "[loader] nativeForwardEventsToDalvik(false)\n");
      arm_exec_call(va_fwd_dalv, env, ctx, 0, 0);
      /* Unity 5.3.3: strb r7,[r0,#0xc] at fwd+0x118; literals at +0x154/+0x158 */
      if (arm_exec_read32(va_fwd_dalv + 0x118u) == 0xe5c0700cu) {
         uint32_t lit0 = arm_exec_read32(va_fwd_dalv + 0x154u);
         uint32_t lit1 = arm_exec_read32(va_fwd_dalv + 0x158u);
         fwd_flag_va = (va_fwd_dalv + 0x118u) + lit0 + lit1 + 0xcu;
         uint32_t word = arm_exec_read32(fwd_flag_va & ~3u);
         unsigned sh = (fwd_flag_va & 3u) * 8u;
         unsigned cur = (word >> sh) & 0xffu;
         if (cur) {
            arm_exec_write32(fwd_flag_va & ~3u, word & ~(0xffu << sh));
            fprintf(stderr, "[loader] cleared ForwardEventsToDalvik flag "
                    "@ 0x%08x (was %u)\n", fwd_flag_va, cur);
         } else {
            fprintf(stderr, "[loader] ForwardEventsToDalvik flag @ 0x%08x "
                    "already 0\n", fwd_flag_va);
         }
      }
   }
   fprintf(stderr, "[loader] calling nativeResume...\n");
   if (va_resume)
      arm_exec_call(va_resume, env, ctx, 0, 0);
   fprintf(stderr, "[loader] nativeResume done, running pending threads...\n");
   arm_exec_run_pending_threads();
   fprintf(stderr, "[loader] entering render loop\n");
   /* SIGUSR1: dump SVC ring buffer on demand (kill -USR1 <pid>) */
   signal(SIGUSR1, svc_dump_handler);
   /* SIGALRM: auto-dump after 15s to diagnose first-frame hang */
   signal(SIGALRM, svc_dump_handler);
   alarm(15);

   /* 注意: nativeDone() はここでは呼ばない。Unity 5+ では nativeDone() は
    * UnityPlayer.destroy() からの終了処理であり、レンダーループ前に呼ぶと
    * エンジンが quit 状態になり nativeRender が即 return する。 */

   /* limp mode: nativeRender が失敗(0)を返してもループを続ける。
    * スタブ未実装による一過性の失敗後も他のサブシステムは前進し得る。 */
   int frame_count = 0, fail_streak = 0, last_ok = -1, resized_after_init = 0;
   int max_frames = 0;
   {
      const char *mf = getenv("LUNARIA_MAX_FRAMES");
      if (mf && *mf) max_frames = atoi(mf);
   }
   for (;;) {
      if (max_frames > 0 && frame_count >= max_frames) {
         fprintf(stderr, "[loader] LUNARIA_MAX_FRAMES=%d reached — exiting render loop\n",
                 max_frames);
         break;
      }
      /* LUNARIA_TOUCH_TEST=x,y: auto DOWN/UP (default frames 60/70).
       * LUNARIA_TOUCH_FRAME=N  : DOWN frame (UP = N + hold).
       * LUNARIA_TOUCH_HOLD=N   : hold duration in frames (default 10).
       *                          Each frame during the hold injects ACTION_MOVE
       *                          so Unity sees a continuous touch. */
      {
         static float tt_x = -1, tt_y = -1; static int tt_parsed = 0;
         static int tt_frame = 60, tt_hold = 10;
         if (!tt_parsed) {
            tt_parsed = 1;
            const char *tt = getenv("LUNARIA_TOUCH_TEST");
            if (tt) sscanf(tt, "%f,%f", &tt_x, &tt_y);
            const char *tf = getenv("LUNARIA_TOUCH_FRAME");
            if (tf) { int v = atoi(tf); if (v > 0) tt_frame = v; }
            const char *th = getenv("LUNARIA_TOUCH_HOLD");
            if (th) { int v = atoi(th); if (v > 0) tt_hold = v; }
         }
         if (tt_x >= 0) {
            /* フォーカス再送: 実機では surfaceChanged 後に focus が届く。
             * ループ前の nativeFocusChanged はエンジン初期化で上書きされる
             * 疑いがあるため、タップ前に再送して入力ゲートを開く */
            if (frame_count == tt_frame - 10 && tt_frame > 10 && va_focus) {
               arm_exec_call(va_focus, env, ctx, 1, 0);
               fprintf(stderr, "[loader] nativeFocusChanged(1) re-sent (frame %d)\n",
                       frame_count);
            }
            if (frame_count == tt_frame) {
               arm_exec_touch_push(0, tt_x, tt_y);  /* ACTION_DOWN */
               fprintf(stderr, "[loader] TOUCH_TEST DOWN (%.0f,%.0f) frame %d\n",
                       tt_x, tt_y, frame_count);
            }
            /* ACTION_MOVE: send every frame while held so Unity keeps the touch active */
            if (frame_count > tt_frame && frame_count < tt_frame + tt_hold) {
               arm_exec_touch_push(2, tt_x, tt_y);
            }
            if (frame_count == tt_frame + tt_hold) {
               arm_exec_touch_push(1, tt_x, tt_y);  /* ACTION_UP */
               fprintf(stderr, "[loader] TOUCH_TEST UP (%.0f,%.0f) frame %d (hold=%d)\n",
                       tt_x, tt_y, frame_count, tt_hold);
            }
         }
      }
      /* GLFW マウス → MotionEvent 注入 (UnityPlayer.onTouchEvent 相当)。
       * MotionEvent の中身 (action/x/y) は libjvm-android.c の JNI getter が
       * arm_exec_touch_* アクセサ経由で読む。1 フレーム 1 イベント: 実機の
       * タップは DOWN と UP が別フレームに届く。同一フレームに両方入れると
       * Unity の Input 集計でタップと認識されないことがある。 */
      if (va_inject && arm_exec_touch_next()) {
         /* Re-clear in case Java/meta-data path set the flag during startup. */
         if (fwd_flag_va) {
            uint32_t word = arm_exec_read32(fwd_flag_va & ~3u);
            unsigned sh = (fwd_flag_va & 3u) * 8u;
            if ((word >> sh) & 0xffu)
               arm_exec_write32(fwd_flag_va & ~3u, word & ~(0xffu << sh));
         }
         if (va_fwd_dalv)
            arm_exec_call(va_fwd_dalv, env, ctx, 0, 0);
         static jobject motion_ev;
         if (!motion_ev)
            motion_ev = jvm->native.AllocObject(&jvm->env,
                  jvm->native.FindClass(&jvm->env, "android/view/MotionEvent"));
         int handled = arm_exec_call(va_inject, env, ctx,
                                     (uint32_t)(uintptr_t)motion_ev, 0);
         static int inj_log = 0;
         if (inj_log < 100) {
            fprintf(stderr, "[loader] injectEvent action=%d x=%.0f y=%.0f → %d\n",
                    arm_exec_touch_action(), arm_exec_touch_x(),
                    arm_exec_touch_y(), handled);
            ++inj_log;
         }
      }
      /* UnityPlayer GL thread loop: executeGLThreadJobs() then nativeRender().
       * nativeRender MUST run to completion: abandoning it mid-PlayerLoop leaves
       * Unity's reentrancy guard set, making every subsequent frame bail with
       * "PlayerLoop called recursively!".  Use the unlimited variant. */
      arm_exec_drain_gl_thread_jobs();
      int ok = arm_exec_call_unlimited(va_render, env, ctx, 0, 0);
      /* Guest abort() (e.g. mono g_assert after mmap OOM) leaves PlayerLoop
       * inconsistent; clearing a hardcoded guard VA then re-entering floods
       * "PlayerLoop called recursively".  Stop the loop after the first abort. */
      if (arm_exec_guest_abort_count() > 0) {
         fprintf(stderr, "[loader] guest abort seen — stopping render loop (frame %d)\n",
                 frame_count);
         break;
      }
      /* If nativeRender was cut short by a guest fault (NULL call, NoExecuteFault),
       * PlayerLoop's re-entry guard byte at 0x20f1ac90 may still be set to 1,
       * causing every subsequent frame to bail with "PlayerLoop called recursively!".
       * Reset it so the next frame can enter PlayerLoop normally. */
      if (!ok) {
         static const uint32_t PLAYERLOOP_GUARD_VA = 0x20f1ac90u;
         uint32_t guard_word = arm_exec_read32(PLAYERLOOP_GUARD_VA & ~3u);
         if (guard_word & 0xffu) {
            arm_exec_write32(PLAYERLOOP_GUARD_VA & ~3u,
                             guard_word & ~0xffu);
            fprintf(stderr, "[loader] nativeRender fault: cleared PlayerLoop guard (frame %d)\n",
                    frame_count);
         }
      }
      /* Android では surfaceChanged → nativeResize がエンジン初期化後にも
       * 届く。ループ前の nativeResize はエンジン未初期化で無視されるため
       * (画面が 128x128 の既定値のままになる)、初回フレーム完了後に再送する。 */
      if (!resized_after_init && frame_count >= 1 && va_resize) {
         int w = arm_exec_fb_width(), h = arm_exec_fb_height();
         arm_exec_call6(va_resize, env, ctx, (uint32_t)w, (uint32_t)h,
                        (uint32_t)w, (uint32_t)h);
         resized_after_init = 1;
         fprintf(stderr, "[loader] nativeResize(%d,%d,%d,%d) re-sent after first frame\n",
                 w, h, w, h);
      }
      /* LUNARIA_TOUCH_DIAG=cntSyncVA,cntPhaseVA,getTouchVA:
       * 毎フレーム libunity のタッチカウント関数をゲスト呼び出しして
       * 「C# スクリプトが見る値」を直接観測する (診断用)。 */
      {
         static uint32_t dg_cnt_sync, dg_cnt_phase, dg_get; static int dg_parsed;
         if (!dg_parsed) {
            dg_parsed = 1;
            const char *d = getenv("LUNARIA_TOUCH_DIAG");
            if (d) sscanf(d, "%x,%x,%x", &dg_cnt_sync, &dg_cnt_phase, &dg_get);
         }
         if (dg_cnt_sync) {
            int cs = arm_exec_call(dg_cnt_sync, 0, 0, 0, 0);
            int cp = dg_cnt_phase ? arm_exec_call(dg_cnt_phase, 0, 0, 0, 0) : -1;
            static int last_cs = -1, last_cp = -1;
            if (cs != last_cs || cp != last_cp) {
               fprintf(stderr, "[diag] frame=%d touchCount sync=%d phase=%d\n",
                       frame_count, cs, cp);
               last_cs = cs; last_cp = cp;
            }
            if (cs > 0 && dg_get) {
               const uint32_t out = 0x41013800; /* STR_SCRATCH 後半 */
               int ok2 = arm_exec_call(dg_get, 0, out, 0, 0);
               fprintf(stderr, "[diag]   GetTouch(0)=%d id=%d x=%f y=%f "
                       "phase=%u f34=%u f38=%u f3c=%u tap=%u\n", ok2,
                       (int)arm_exec_read32(out),
                       (double)*(float *)&(uint32_t){arm_exec_read32(out + 4)},
                       (double)*(float *)&(uint32_t){arm_exec_read32(out + 8)},
                       arm_exec_read32(out + 0x24), arm_exec_read32(out + 0x34),
                       arm_exec_read32(out + 0x38), arm_exec_read32(out + 0x3c),
                       arm_exec_read32(out + 0x20));
            }
         }
      }
      /* UnityMain などのゲストスレッドにも実行時間を与える */
      arm_exec_run_pending_threads();
      /* Unity はeglSwapBuffersをJava側に任せる場合があるのでここで呼ぶ */
      arm_exec_egl_swap();
      ++frame_count;
      if (ok != last_ok || (frame_count <= 5) || (frame_count % 100 == 0)) {
         fprintf(stderr, "[loader] nativeRender → %d (frame %d)\n", ok, frame_count);
         last_ok = ok;
      }
      if (getenv("LUNARIA_TRACE_HEAP"))
         fprintf(stderr, "[loader] heap used = %u MB (frame %d)\n",
                 arm_exec_heap_used() >> 20, frame_count);
      if (getenv("LUNARIA_TRACE_MONO") &&
          (frame_count == 1 || frame_count == 10 || frame_count == 100))
         dump_mono_defaults("render loop");
      fail_streak = ok ? 0 : fail_streak + 1;
      /* 失敗が続いたらフレームペーシングして CPU/スワップ暴走を防ぐ */
      if (fail_streak > 3)
         usleep(16000);
      if (arm_exec_glfw_should_close()) break;
   }

   /* 終了処理: nativeDone() は UnityPlayer.destroy() 相当 */
   if (va_done)
      arm_exec_call(va_done, env, ctx, 0, 0);

   return EXIT_SUCCESS;
}

__attribute__((optimize(0))) static void
raw_start(void *entry, int argc, const char *argv[])
{
   // XXX: make this part of the linker when it's rewritten
#if ANDROID_X86_LINKER && defined(__i386__)
   __asm__("mov 2*4(%ebp),%eax"); /* entry */
   __asm__("mov 3*4(%ebp),%ecx"); /* argc */
   __asm__("mov 4*4(%ebp),%edx"); /* argv */
   __asm__("mov %edx,%esp"); /* trim stack. */
   __asm__("push %edx"); /* push argv */
   __asm__("push %ecx"); /* push argc */
   __asm__("sub %edx,%edx"); /* no rtld_fini function */
   __asm__("jmp *%eax"); /* goto entry */
#else
   warnx("raw_start not implemented for this asm platform, can't execute binaries.");
#endif
}

int
main(int argc, const char *argv[])
{
   /* Keep loader milestones in chronological order when stdout and stderr are
    * redirected to one startup log.  Fully buffered stdout otherwise leaves
    * "loading module" and dependency messages at the end of the file. */
   setvbuf(stdout, NULL, _IOLBF, 0);

   if (argc < 2)
      errx(EXIT_FAILURE, "usage: <elf file or jni library>");

   printf("loading module: %s\n", argv[1]);

   /* ARM64 ELF: use A64 dynarmic emulation path */
   if (arm64_elf_is_arm64(argv[1])) {
      printf("detected ARM64 ELF — using A64 dynarmic emulation\n");
      setenv("GC_DONT_GC", "1", 0);
      setenv("GC_MAXIMUM_HEAP_SIZE", "268435456", 0);
      setenv("GC_INITIAL_HEAP_SIZE", "67108864",  0);
      static struct jvm jvm;
      jvm_init(&jvm);

      if (arm64_exec_context_init(&jvm) < 0)
         errx(EXIT_FAILURE, "arm64_exec_context_init failed");

      /* Pre-load companion libraries from the same directory */
      {
         char dir[4096], libpath[4096];
         char dep_seen[128][NAME_MAX + 1] = {{0}};
         size_t dep_seen_n = 0;
         struct stat stbuf;
         snprintf(dir, sizeof(dir), "%s", argv[1]);
         char *slash = strrchr(dir, '/');
         if (slash) *(slash + 1) = '\0'; else dir[0] = '\0';

         /* libc++_shared.so first */
         snprintf(libpath, sizeof(libpath), "%s%s", dir, "libc++_shared.so");
         if (stat(libpath, &stbuf) == 0 && arm64_elf_is_arm64(libpath)) {
            printf("preloading arm64 libc++_shared: %s\n", libpath);
            arm64_exec_load_library(libpath, 0);
         }
         /* Unity IL2CPP + Frame Pacing (must precede libunity PLT bind) */
         static const char *unity_deps[] = {
            "libil2cpp.so", "libswappywrapper.so", NULL
         };
         for (int k = 0; unity_deps[k]; k++) {
            snprintf(libpath, sizeof(libpath), "%s%s", dir, unity_deps[k]);
            if (stat(libpath, &stbuf) == 0 && arm64_elf_is_arm64(libpath)) {
               printf("preloading arm64 dep: %s\n", libpath);
               arm64_exec_load_library(libpath, 0);
            }
         }
         /* libpsoservice.so and other UE companion libs */
         static const char *ue_deps[] = {
            "libpsoservice.so", "libhwcpipe.so", NULL
         };
         for (int k = 0; ue_deps[k]; k++) {
            snprintf(libpath, sizeof(libpath), "%s%s", dir, ue_deps[k]);
            if (stat(libpath, &stbuf) == 0 && arm64_elf_is_arm64(libpath)) {
               printf("preloading arm64 dep: %s\n", libpath);
               arm64_exec_load_library(libpath, 0);
            }
         }

         /* Finally follow the main ELF's actual dependency graph.  This
          * catches APK-private libraries without title-specific name lists. */
         a64_preload_needed(argv[1], dir, dep_seen, &dep_seen_n);
      }

      if (!arm64_exec_host_egl_init())
         fprintf(stderr, "[loader] early arm64 host EGL init failed\n");

      int jni_ver = arm64_exec_jni_onload(argv[1], &jvm);
      if (jni_ver < 0)
         errx(EXIT_FAILURE, "arm64_exec_jni_onload failed");
      int ret = run_jni_game_arm64(&jvm);
      jvm_release(&jvm);
      printf("exiting\n");
      return ret;
   }

   /* ARM 32-bit ELF: use dynarmic emulation path */
   if (arm_elf_is_arm32(argv[1])) {
      printf("detected ARM32 ELF — using dynarmic emulation\n");
      /* Boehm GC の stop-the-world はシグナルでスレッドを止めるが、協調
       * スレッドモデルではシグナル配送がなく suspend ack を永遠に待って
       * ハングする。bdwgc が GC_init で参照する GC_DONT_GC で抑止する
       * (環境変数で明示指定されていれば尊重する)。
       * LUNARIA_GC_ENABLE=1 のときは回収を有効化する（リーク抑止の実験/本対応）。
       * 注意: bdwgc は GC_DONT_GC の「存在」で判定するため、有効化時は
       * setenv せず unsetenv しておく。 */
      if (getenv("LUNARIA_GC_ENABLE"))
         unsetenv("GC_DONT_GC");
      else
         setenv("GC_DONT_GC", "1", 0);
      /* Boehm GC computes max_heap_size from the 32-bit address space (~4 GB),
       * producing requests of ~3.7 GB which our mmap bump allocator must reject.
       * With zero heap the GC calls GC_scratch_alloc(0) → ABORT("Bad GET_MEM arg").
       * Cap the heap to 256 MB so the GC gets usable memory without flooding. */
      setenv("GC_MAXIMUM_HEAP_SIZE", "268435456", 0); /* 256 MB */
      setenv("GC_INITIAL_HEAP_SIZE", "67108864",  0); /* 64 MB */
      static struct jvm jvm;
      jvm_init(&jvm);

      /* ARM context を先に初期化して依存ライブラリをプリロードする。
       * libmono.so のエクスポートシンボルを libunity.so のパッチより先に収集する。 */
      if (arm_exec_context_init(&jvm) < 0)
         errx(EXIT_FAILURE, "arm_exec_context_init failed");

      /* 依存ライブラリを引数より同一ディレクトリから探してロードする */
      {
         char dir[4096], libpath[4096];
         struct stat stbuf;
         snprintf(dir, sizeof(dir), "%s", argv[1]);
         /* dirname相当 (末尾スラッシュまで) */
         char *slash = strrchr(dir, '/');
         if (slash) *(slash + 1) = '\0';
         else dir[0] = '\0';

         /* libmono.so または libmonobdwgc-2.0.so を探してロード */
         static const char *mono_candidates[] = {
            "libmono.so", "libmonobdwgc-2.0.so", NULL
         };
         for (int k = 0; mono_candidates[k]; k++) {
            snprintf(libpath, sizeof(libpath), "%s%s", dir, mono_candidates[k]);
            if (stat(libpath, &stbuf) == 0 && arm_elf_is_arm32(libpath)) {
               printf("preloading mono: %s at base 0x20000000\n", libpath);
               arm_exec_load_library(libpath, 0x20000000u);
               break;
            }
         }
         /* libmain.so はロードしない: 中身は Java の NativeLoader 経由で
          * libunity.so を dlopen するだけのスタブで、エミュレーション環境では
          * NativeLoader 待ちでハングする */

         /* libc++_shared.so → libil2cpp.so の順 (IL2CPP ゲーム対応) */
         snprintf(libpath, sizeof(libpath), "%s%s", dir, "libc++_shared.so");
         if (stat(libpath, &stbuf) == 0 && arm_elf_is_arm32(libpath)) {
            printf("preloading libc++_shared: %s\n", libpath);
            arm_exec_load_library(libpath, 0);
         }

         /* libil2cpp.so: libunity.so より先にシンボルテーブルへ登録 */
         snprintf(libpath, sizeof(libpath), "%s%s", dir, "libil2cpp.so");
         if (stat(libpath, &stbuf) == 0 && arm_elf_is_arm32(libpath)) {
            printf("preloading il2cpp: %s\n", libpath);
            arm_exec_load_library(libpath, 0);
         }

         /* libswappywrapper.so (Android Frame Pacing): libunity.so が SwappyGL_* を
          * 呼ぶため、先にロードして PLT を解決しておかないと NULL 呼び出しになる */
         snprintf(libpath, sizeof(libpath), "%s%s", dir, "libswappywrapper.so");
         if (stat(libpath, &stbuf) == 0 && arm_elf_is_arm32(libpath)) {
            printf("preloading swappywrapper: %s\n", libpath);
            arm_exec_load_library(libpath, 0);
         }

         /* UE4 companion libs (DT_NEEDED of libUE4.so / optional plugins) */
         static const char *ue4_deps[] = {
            "libplaycore.so", "libhwcpipe.so", "libtry-alloc-lib.so",
            "libOVRPlugin.so", "libvrapi.so", NULL
         };
         for (int k = 0; ue4_deps[k]; k++) {
            snprintf(libpath, sizeof(libpath), "%s%s", dir, ue4_deps[k]);
            if (stat(libpath, &stbuf) == 0 && arm_elf_is_arm32(libpath)) {
               printf("preloading UE4 dep: %s\n", libpath);
               arm_exec_load_library(libpath, 0);
            }
         }
      }

      /* ホスト側 EGL/GLES2 コンテキストを libunity.so の INIT_ARRAY / JNI_OnLoad よりも
       * 前に作成する。Unity の INIT_ARRAY コンストラクタが eglGetCurrentContext() を
       * チェックして EGL 準備済みなら eglGetProcAddress() で GL 関数ポインタを取得する
       * ため、ここで初期化しておく必要がある。 */
      if (!arm_exec_host_egl_init())
         fprintf(stderr, "[loader] early host EGL init failed\n");

      /* メインライブラリ (libunity.so) をロード & JNI_OnLoad 実行 */
      int jni_ver = arm_exec_jni_onload(argv[1], &jvm);
      if (jni_ver < 0)
         errx(EXIT_FAILURE, "arm_exec_jni_onload failed");
      int ret = run_jni_game_arm(&jvm);
      jvm_release(&jvm);
      printf("exiting\n");
      return ret;
   }

   {
      char abs[PATH_MAX], paths[4096];
      realpath(argv[1], abs);
      snprintf(paths, sizeof(paths), "%s", dirname(abs));
      dl_parse_library_path(paths, ":");
   }

   void *handle;
   if (!(handle = bionic_dlopen(argv[1], RTLD_LOCAL | RTLD_NOW)))
      errx(EXIT_FAILURE, "dlopen failed: %s", bionic_dlerror());

   struct {
      union {
         void *ptr;
         jint (*fun)(void*, void*);
      } JNI_OnLoad;

      union {
         void *ptr;
      } start;
   } entry = {0};

   {
      union {
         char bytes[sizeof(Elf32_Ehdr)];
         Elf32_Ehdr hdr;
      } elf;

      FILE *f;
      if (!(f = fopen(argv[1], "rb")))
         err(EXIT_FAILURE, "fopen(%s)", argv[1]);

      fread(elf.bytes, 1, sizeof(elf.bytes), f);
      fclose(f);

      struct soinfo *si = handle;
      if (elf.hdr.e_entry)
         entry.start.ptr = (void*)(intptr_t)(si->base + elf.hdr.e_entry);
   }

   int ret = EXIT_FAILURE;
   if (entry.start.ptr) {
      printf("jumping to %p\n", entry.start.ptr);
      raw_start(entry.start.ptr, argc - 1, &argv[1]);
   } else if ((entry.JNI_OnLoad.ptr = bionic_dlsym(handle, "JNI_OnLoad"))) {
      struct jvm jvm;
      jvm_init(&jvm);
      entry.JNI_OnLoad.fun(&jvm.vm, NULL);
      ret = run_jni_game(&jvm);
      jvm_release(&jvm);
   } else {
      warnx("no entrypoint found in %s", argv[1]);
   }

   dvm_jni_report();
   printf("unloading module: %s\n", argv[1]);
   bionic_dlclose(handle);
   printf("exiting\n");
   return ret;
}
