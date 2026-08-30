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
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <dlfcn.h>
#include <elf.h>
#include <err.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include "linker/dlfcn.h"
#include "linker/linker.h"
#include "jvm/jvm.h"
#include "arm_exec.h"
#include "dvm/dvm_jni.h"
#include "dvm/dvm_media.h"
#include <link.h>

/* Exposed from arm_exec.cpp for diagnostic dumps */
extern void arm_exec_svc_ring_dump(void);
extern void arm64_exec_svc_ring_dump(void);

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

/* LUNARIA_TOUCH_TEST replay, shared by the pump loops.
 *
 * x,y[;x,y...] taps, one after another: LUNARIA_TOUCH_FRAME is the first
 * tap's DOWN frame, LUNARIA_TOUCH_HOLD its length, LUNARIA_TOUCH_GAP the wait
 * before the next.  Injection goes through arm_exec_touch_push(), which routes
 * through the emulator's own window layer first, so these taps can press a
 * dialog the emulator is showing as well as reach the guest.
 *
 * More than one point because a dialog is more than one press: a terms gate
 * wants a checkbox and then Confirm, in that order. */
static void touch_test_tick(int frame_count)
{
#define TT_MAX 8
   static float tt_x[TT_MAX], tt_y[TT_MAX];
   static int tt_n = -1, tt_frame = 60, tt_hold = 10, tt_gap = 60;
   if (tt_n < 0) {
      tt_n = 0;
      const char *tt = getenv("LUNARIA_TOUCH_TEST");
      for (const char *p = tt; p && *p && tt_n < TT_MAX; ) {
         float x = -1, y = -1;
         if (sscanf(p, "%f,%f", &x, &y) == 2 && x >= 0 && y >= 0) {
            tt_x[tt_n] = x; tt_y[tt_n] = y; ++tt_n;
         }
         const char *semi = strchr(p, ';');
         if (!semi) break;
         p = semi + 1;
      }
      const char *tf = getenv("LUNARIA_TOUCH_FRAME");
      if (tf) { int v = atoi(tf); if (v > 0) tt_frame = v; }
      const char *th = getenv("LUNARIA_TOUCH_HOLD");
      if (th) { int v = atoi(th); if (v > 0) tt_hold = v; }
      const char *tg = getenv("LUNARIA_TOUCH_GAP");
      if (tg) { int v = atoi(tg); if (v > 0) tt_gap = v; }
   }
   if (tt_n <= 0) return;
   const int stride = tt_hold + tt_gap;
   for (int t = 0; t < tt_n; ++t) {
      const int down = tt_frame + t * stride;
      if (frame_count == down) {
         arm_exec_touch_push(0, tt_x[t], tt_y[t]);  /* ACTION_DOWN */
         fprintf(stderr, "[loader] TOUCH_TEST[%d] DOWN (%.0f,%.0f) frame %d\n",
                 t, tt_x[t], tt_y[t], frame_count);
      }
      /* ACTION_MOVE every frame while held: Unity keeps the touch active */
      if (frame_count > down && frame_count < down + tt_hold)
         arm_exec_touch_push(2, tt_x[t], tt_y[t]);
      if (frame_count == down + tt_hold) {
         arm_exec_touch_push(1, tt_x[t], tt_y[t]);  /* ACTION_UP */
         fprintf(stderr, "[loader] TOUCH_TEST[%d] UP (%.0f,%.0f) frame %d (hold=%d)\n",
                 t, tt_x[t], tt_y[t], frame_count, tt_hold);
      }
   }
#undef TT_MAX
}

/* Set by the AArch64 run paths so the stall dump reports the A64 JIT state
 * instead of the (idle) A32 one. */
static int g_dump_arm64;

/* Throughput, once every LUNARIA_PERF_S seconds (0 disables).
 *
 * "Slower than a phone" is the whole of the report most of the time, and
 * answering it needs three numbers side by side: how often the host pump
 * turns, how often the guest actually presents, and how many guest
 * instructions were retired to get there.  Which of the three moved tells the
 * difference between the emulator starving the guest, the guest starving
 * itself on a lock, and the host GL being the bottleneck — and it costs two
 * counter reads a frame, so it is on by default rather than behind a trace
 * flag nobody sets until the run has already been thrown away. */
/* Where a pump frame's wall clock goes.
 *
 * "20 frames a second" is the symptom; it says nothing about whether the
 * emulator was running guest code, presenting, or waiting.  Each stage of the
 * frame adds its own time here and the totals are reported next to [perf], so
 * a frame that costs 50 ms can be read as "16 ms of guest, 30 ms of swap"
 * rather than guessed at. */
enum frame_stage {
   FRAME_STAGE_SCHED,      /* run_threads(): guest code and its SVCs */
   FRAME_STAGE_IDLE,       /* every guest thread parked: the frame's own sleep */
   FRAME_STAGE_MEDIA,      /* the media clock and the pending-preferences write */
   FRAME_STAGE_SWAP,       /* eglSwapBuffers on the host */
   FRAME_STAGE_INPUT,      /* window system events */
   FRAME_STAGE_LAST
};
static uint64_t g_frame_stage_ns[FRAME_STAGE_LAST];
static const char *const g_frame_stage_name[FRAME_STAGE_LAST] = {
   "sched", "idle", "media", "swap", "input"
};

static uint64_t frame_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void frame_stage_add(enum frame_stage st, uint64_t t0)
{
   g_frame_stage_ns[st] += frame_now_ns() - t0;
}

static void frame_stage_report(double window, unsigned long long frames)
{
   uint64_t tot = 0;
   for (int i = 0; i < FRAME_STAGE_LAST; ++i) tot += g_frame_stage_ns[i];
   if (!tot || !frames) return;
   fprintf(stderr, "[frame]   %.1f ms each, of which", window * 1e3 / (double)frames);
   for (int i = 0; i < FRAME_STAGE_LAST; ++i)
      fprintf(stderr, " %s=%.1f", g_frame_stage_name[i],
              (double)g_frame_stage_ns[i] / 1e6 / (double)frames);
   fprintf(stderr, " ms (%.0f%% of the frame accounted for)\n",
           100.0 * (double)tot / (window * 1e9));
   for (int i = 0; i < FRAME_STAGE_LAST; ++i) g_frame_stage_ns[i] = 0;
}

static void perf_tick(void)
{
   static double every = -1.0;      /* -1: not configured, 0: disabled */
   static double t0, last;
   static uint64_t last_frames, last_presents, last_ticks, last_xlat;
   static uint64_t frames;

   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   const double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;

   ++frames;
   if (every < 0.0) {
      const char *e = getenv("LUNARIA_PERF_S");
      every = e && *e ? atof(e) : 10.0;
      if (every < 0.0) every = 0.0;
      t0 = last = now;
   }
   if (every == 0.0 || now - last < every) return;

   const uint64_t presents = arm_exec_guest_swap_count();
   const uint64_t gticks   = arm_exec_sched_ticks();
   const uint64_t xlat     = arm_exec_translated_insn();
   const double   dt       = now - last;

   fprintf(stderr,
           "[perf] t=%.0fs pump=%llu (%.1f/s) present=%llu (%.1f/s) "
           "guest=%.0fM insn (%.1f Mips) xlat=%.1fM (%.1f M/s)\n",
           now - t0,
           (unsigned long long)frames, (double)(frames - last_frames) / dt,
           (unsigned long long)presents, (double)(presents - last_presents) / dt,
           (double)gticks / 1e6, (double)(gticks - last_ticks) / dt / 1e6,
           (double)xlat / 1e6, (double)(xlat - last_xlat) / dt / 1e6);

   frame_stage_report(dt, frames - last_frames);

   last = now;
   last_frames = frames;
   last_presents = presents;
   last_ticks = gticks;
   last_xlat = xlat;
}

/* A guest that stops presenting is the shape every "it just sits there" bug
 * takes, and it is invisible from the outside: the pump loop keeps spinning
 * at full speed with nothing behind it.  Watch the guest's own present count
 * and, when it stops moving, dump the guest thread state from the pump loop
 * itself.  The same dump used to be reachable only through SIGUSR1, which
 * runs the (decidedly not async-signal-safe) dumper on whatever thread the
 * signal lands on and takes the process down with it.
 *
 * LUNARIA_STALL_S seconds with no present triggers a report; 0 disables. */
static void stall_watch_tick(void)
{
   static double next_s = -1.0;   /* -1: not configured yet, 0: disabled */
   static uint64_t last_swaps;
   static double last_change;
   static int reports;

   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   const double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;

   if (next_s < 0.0) {
      const char *e = getenv("LUNARIA_STALL_S");
      next_s = e && *e ? atof(e) : 10.0;
      if (next_s < 0.0) next_s = 0.0;
      last_change = now;
   }
   if (next_s == 0.0) return;

   const uint64_t swaps = arm_exec_guest_swap_count();
   if (swaps != last_swaps) {
      last_swaps = swaps;
      last_change = now;
      reports = 0;
      return;
   }
   /* Nothing to say before the guest has presented at all: startup legitimately
    * takes tens of seconds, and the pump loop is the only thing running. */
   if (swaps == 0 || now - last_change < next_s) return;
   if (reports >= 8) return;

   fprintf(stderr, "[stall] guest has not presented for %.1fs "
           "(%llu presents so far) — dumping guest threads #%d "
           "(LUNARIA_STALL_S=0 to silence)\n",
           now - last_change, (unsigned long long)swaps, reports);
   ++reports;
   arm64_exec_svc_ring_dump();
   last_change = now;   /* re-arm, so a long stall reports periodically */
}

/* The other half of the same diagnostic: a guest that is presenting happily
 * while the game itself has stopped getting anywhere never trips the stall
 * watch, and that is exactly the shape of "the engine renders but the title
 * waits forever".  Touch /tmp/lunaria-threads and the next frame says what
 * every guest thread is doing.  Polled from the pump rather than raised by a
 * signal because the dump is not async-signal-safe — the handler route took
 * the process down instead of printing. */
static void thread_dump_request_tick(void)
{
   static int every;
   if (++every < 30) return;   /* ~twice a second at 60 fps */
   every = 0;
   if (access("/tmp/lunaria-threads", F_OK) != 0) return;
   unlink("/tmp/lunaria-threads");
   fprintf(stderr, "[threads] dump requested\n");
   arm64_exec_svc_ring_dump();
}

static void svc_dump_handler(int sig) {
    (void)sig;
    if (g_dump_arm64)
        arm64_exec_svc_ring_dump();
    else
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

/* Parameter types of a GameActivity native.  Returns the count, or -1 when the
 * method is unknown (then the caller falls back to the layout it knows).
 *
 * The signature handed to RegisterNatives comes from the engine binary that is
 * about to be called, so it is the only description of the frame the callee
 * actually reads; take it whenever the library registered the method.  The dex
 * is a fallback for implicitly-bound natives (no RegisterNatives entry) and it
 * cannot be trusted on its own: a title may still ship a stale
 * com.epicgames.ue4.GameActivity next to the UE5 com.epicgames.unreal one, and
 * its older declaration then shifts every argument by a slot — Blade & Soul
 * Masia declares nativeSetGlobalActivity(Z,L,L,Z,L) in the dex while libUnreal
 * registers (Z,Z,L,L,Z,L), so APKFilename fell off the end and the engine
 * mounted no expansion at all. */
static int
ue_native_params(const char *method, char *types, int max)
{
   static const char *const classes[] = { "com.epicgames.unreal.GameActivity",
                                          "com.epicgames.ue4.GameActivity" };
   char sig[512];
   for (size_t i = 0; i < sizeof classes / sizeof classes[0]; ++i) {
      sig[0] = '\0';
      if ((arm64_exec_lookup_native_sig(classes[i], method, sig, (int)sizeof sig) ||
           arm_exec_lookup_native_sig(classes[i], method, sig, (int)sizeof sig)) &&
          sig[0] == '(') {
         fprintf(stderr, "[loader] %s.%s%s (registered)\n", classes[i], method, sig);
         return jni_sig_params(sig, types, max);
      }
   }
   for (size_t i = 0; i < sizeof classes / sizeof classes[0]; ++i)
      if (apk_method_signature(classes[i], method, sig, sizeof sig))
         return jni_sig_params(sig, types, max);
   return -1;
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

/* FAndroidPlatformFile roots the loose project tree at
 *   <externalFilesDir>/UE4Game/<Project>/     (UE4 and earlier)
 *   <externalFilesDir>/UnrealGame/<Project>/  (UE5 on)
 * The engine picks one; the emulator has to recognise whichever the staging
 * step produced, so keep both in one table. */
static const char *const ue_files_roots[] = { "UE4Game", "UnrealGame", NULL };

/* UE's ProjectName — the "TSProject" in <EngineDir>/TSProject/TSProject.uproject.
 * nativeSetObbInfo hands it to the engine, which then builds GFilePathBase-
 * relative content paths and the OBB names from it.  It is *not* derivable
 * from the package id: Blade & Soul ships com.netmarble.bnsmasia around a
 * project called TSProject, so the last package component sends every
 * subsequent lookup to UE4Game/bnsmasia/… while the expansion staged the tree
 * under UE4Game/TSProject/.
 *
 * UnrealBuildTool bakes the name into GameActivity.java as the ProjectName
 * field, but it is also right there in the package: assets/UE4CommandLine.txt
 * is the engine's own command line and starts with the .uproject path.  Read
 * the name from the same place the engine does, and only fall back to the
 * package id when a package ships no command line at all. */
static const char *
ue_project_name(const char *pkg)
{
   static char name[128];
   if (*name) return name;

   const char *dir = getenv("ANDROID_PACKAGE_CODE_PATH");
   char line[512];
   line[0] = '\0';
   if (dir && *dir) {
      /* UE4 stages assets/UE4CommandLine.txt; UE5 renamed it to
       * assets/UECommandLine.txt.  Look for both — otherwise a UE5 package
       * falls through to the staged-tree scan (or the package id) and the
       * engine is handed the wrong ProjectName. */
      static const char *const cmdline_names[] = {
         "UE4CommandLine.txt", "UECommandLine.txt", NULL
      };
      for (int i = 0; cmdline_names[i] && !line[0]; ++i) {
         char path[PATH_MAX];
         snprintf(path, sizeof path, "%s/assets/%s", dir, cmdline_names[i]);
         FILE *f = fopen(path, "rb");
         if (!f) continue;
         if (!fgets(line, sizeof line, f)) line[0] = '\0';
         fclose(f);
      }
   }
   /* "../../../TSProject/TSProject.uproject Map_Start -faketouches …" — take
    * the first whitespace-delimited token and keep the .uproject basename. */
   char *sp = line;
   while (*sp && !isspace((unsigned char)*sp)) ++sp;
   *sp = '\0';
   const char *ext = strstr(line, ".uproject");
   if (ext) {
      const char *base = line;
      for (const char *c = line; c < ext; ++c)
         if (*c == '/' || *c == '\\') base = c + 1;
      size_t n = (size_t)(ext - base);
      if (n > 0 && n < sizeof name) {
         memcpy(name, base, n);
         name[n] = '\0';
         fprintf(stderr, "[loader] UE project name %s (from UE4CommandLine.txt)\n", name);
         return name;
      }
   }

   /* No command line in the package: the staged expansion tree names the
    * project too (<EngineDir>/<P>/<P>/Content/Paks). */
   const char *ext_files = getenv("ANDROID_EXTERNAL_FILES_DIR");
   for (int r = 0; ext_files && *ext_files && ue_files_roots[r]; ++r) {
      char ue[PATH_MAX];
      snprintf(ue, sizeof ue, "%s/%s", ext_files, ue_files_roots[r]);
      DIR *d = opendir(ue);
      if (!d) continue;
      struct dirent *de;
      while ((de = readdir(d))) {
         if (de->d_name[0] == '.') continue;
         char paks[PATH_MAX];
         struct stat sb;
         snprintf(paks, sizeof paks, "%s/%s/%s/Content/Paks",
                  ue, de->d_name, de->d_name);
         if (stat(paks, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            snprintf(name, sizeof name, "%s", de->d_name);
            closedir(d);
            fprintf(stderr, "[loader] UE project name %s (from staged %s)\n",
                    name, ue_files_roots[r]);
            return name;
         }
      }
      closedir(d);
   }

   const char *last = pkg ? strrchr(pkg, '.') : NULL;
   snprintf(name, sizeof name, "%s", last ? last + 1 : (pkg ? pkg : "Game"));
   fprintf(stderr, "[loader] UE project name %s (guessed from package id)\n", name);
   return name;
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

/* --- host frame pump pacing ----------------------------------------------
 *
 * The pump has two jobs: present at a steady rate, and service host window
 * events.  Running the guest is the cooperative scheduler's job.  The loops
 * below used to conflate the two — one scheduler pass, then a flat
 * usleep(16000) — and a pass ends as soon as every runnable thread has had its
 * slice, which during loading is a fraction of a millisecond.  Measured on a
 * Cross Worlds startup, guest code ran 114 ms out of 6.7 s of wall time: the
 * emulator was idle 98% of the time and everything before the first frame took
 * roughly fifty times longer than the work in it warrants.
 *
 * A device runs the engine threads continuously between vsyncs, so do the
 * same: keep scheduling until the frame budget is spent, and sleep out the
 * remainder only when a pass executed no guest code at all — that is, when
 * every guest thread is parked on a cond/futex/mutex/fd and there is genuinely
 * nothing to run.
 */
static uint64_t
pump_now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void
pump_run_frame(void (*run_threads)(void))
{
   static uint64_t budget_us = 0;
   static uint64_t va_get_clock = 0;
   static uint64_t va_tick_fetch = 0;
   static uint64_t va_tick_render = 0;
   static uint64_t va_tick_pre_engine = 0;
   static uint64_t va_tick_post_engine = 0;
   static uint64_t va_tick_input = 0;
   static uint64_t va_tick_output = 0;
   static int ue_media_tick_dbg = 0;
   static int ue_media_tick_inited = 0;
   static int ue_media_tick_use_a64 = 0;

   if (!ue_media_tick_inited) {
      /* UE4 video playback uses FMediaClock/FJavaAndroidMediaPlayer to
       * eventually call MediaPlayer14.updateVideoFrame/getVideoLastFrame.
       * On Lunaria bring-up the media clock path can miss frames, leaving
       * the movie texture white. Tick the clock explicitly to re-enter the
       * native→JNI→SurfaceTexture consumer pipeline. */
      ue_media_tick_dbg = getenv("LUNARIA_TRACE_MEDIA") ? 1 : 0;

      /* Resolve for the currently active guest arch. */
      va_get_clock   = arm_exec_lookup_export("_ZN12FMediaModule8GetClockEv");
      va_tick_fetch  = arm_exec_lookup_export("_ZN11FMediaClock9TickFetchEv");
      va_tick_render = arm_exec_lookup_export("_ZN11FMediaClock10TickRenderEv");
      va_tick_pre_engine =
         arm_exec_lookup_export("_ZN12FMediaModule13TickPreEngineEv");
      va_tick_post_engine =
         arm_exec_lookup_export("_ZN12FMediaModule14TickPostEngineEv");
      va_tick_input = arm_exec_lookup_export("_ZN11FMediaClock9TickInputEv");
      va_tick_output = arm_exec_lookup_export("_ZN11FMediaClock10TickOutputEv");

      if (!va_get_clock && !va_tick_fetch && !va_tick_render &&
          !va_tick_pre_engine && !va_tick_post_engine &&
          !va_tick_input && !va_tick_output) {
         /* arm64: arm_exec_lookup_export reads the 32-bit dynsym only. */
         va_get_clock   = arm64_exec_lookup_export("_ZN12FMediaModule8GetClockEv");
         va_tick_fetch  = arm64_exec_lookup_export("_ZN11FMediaClock9TickFetchEv");
         va_tick_render = arm64_exec_lookup_export("_ZN11FMediaClock10TickRenderEv");
         va_tick_pre_engine =
            arm64_exec_lookup_export("_ZN12FMediaModule13TickPreEngineEv");
         va_tick_post_engine =
            arm64_exec_lookup_export("_ZN12FMediaModule14TickPostEngineEv");
         va_tick_input = arm64_exec_lookup_export("_ZN11FMediaClock9TickInputEv");
         va_tick_output = arm64_exec_lookup_export("_ZN11FMediaClock10TickOutputEv");
         ue_media_tick_use_a64 = 1;
      }

      ue_media_tick_inited = 1;
      if (ue_media_tick_dbg) {
         fprintf(stderr,
                 "[media] UE clock symbols: getClock=0x%llx tickFetch=0x%llx tickRender=0x%llx (a64=%d)\n",
                 (unsigned long long)va_get_clock,
                 (unsigned long long)va_tick_fetch,
                 (unsigned long long)va_tick_render,
                 ue_media_tick_use_a64);
      }
   }
   if (!budget_us) {
      const char *e = getenv("LUNARIA_FRAME_US");
      long v = (e && *e) ? atol(e) : 16000;
      budget_us = v > 0 ? (uint64_t)v : 16000;
   }
   const uint64_t deadline = pump_now_us() + budget_us;
   for (;;) {
      const uint64_t before = arm_exec_sched_ticks();
      const uint64_t t_sched = frame_now_ns();
      run_threads();
      frame_stage_add(FRAME_STAGE_SCHED, t_sched);
      const uint64_t now = pump_now_us();
      if (now >= deadline)
         break;
      if (arm_exec_sched_ticks() != before)
         continue;   /* the guest is working — let it have the whole frame */
      /* Everything is parked.  Wait in small steps so that a wakeup which
       * becomes visible mid-frame is not held back until the next one. */
      const uint64_t t_idle = frame_now_ns();
      usleep((deadline - now > 1000ull) ? 1000u : (useconds_t)(deadline - now));
      frame_stage_add(FRAME_STAGE_IDLE, t_idle);
   }
   const uint64_t t_media = frame_now_ns();
   {
      struct dvm *vm = dvm_jni_vm();
      if (vm) dvm_media_pump_active(vm);
      /* Preferences an apply() left pending: on a device the framework writes
       * them behind the caller's back, and this is that writer. */
      if (vm) dvm_prefs_flush(vm);
      /* The input method: a game blocked on the text the player is typing
       * calls no Java of its own, so the keystrokes need a way in that does
       * not depend on the guest doing anything. */
      if (vm) dvm_ime_frame(vm);

      /* Drive UE's media clock even when the guest happens to be waiting
       * on other task graph work; this is the root of "updateVideoFrame
       * isn't called during playback". */
      if (va_get_clock) {
         uint64_t clk = ue_media_tick_use_a64
            ? (uint64_t)arm64_exec_call(va_get_clock, 0, 0, 0, 0)
            : (uint64_t)arm_exec_call((uint32_t)va_get_clock, 0, 0, 0, 0);

         if (clk) {
            /* Also tick the media module stage that usually prepares clocks
             * and sinks; on Lunaria the engine's pre-engine stage can miss
             * the media task graph. */
            if (va_tick_pre_engine) {
               if (ue_media_tick_use_a64)
                  (void)arm64_exec_call(va_tick_pre_engine, 0, 0, 0, 0);
               else
                  (void)arm_exec_call((uint32_t)va_tick_pre_engine, 0, 0, 0, 0);
            }
            if (va_tick_post_engine) {
               if (ue_media_tick_use_a64)
                  (void)arm64_exec_call(va_tick_post_engine, 0, 0, 0, 0);
               else
                  (void)arm_exec_call((uint32_t)va_tick_post_engine, 0, 0, 0, 0);
            }
         }

         if (clk) {
            if (ue_media_tick_use_a64) {
               if (va_tick_input)  (void)arm64_exec_call(va_tick_input, clk, 0, 0, 0);
               if (va_tick_fetch)  (void)arm64_exec_call(va_tick_fetch, clk, 0, 0, 0);
               if (va_tick_output) (void)arm64_exec_call(va_tick_output, clk, 0, 0, 0);
               if (va_tick_render) (void)arm64_exec_call(va_tick_render, clk, 0, 0, 0);
            } else {
               if (va_tick_input)  (void)arm_exec_call((uint32_t)va_tick_input, (uint32_t)clk, 0, 0, 0);
               if (va_tick_fetch)  (void)arm_exec_call((uint32_t)va_tick_fetch, (uint32_t)clk, 0, 0, 0);
               if (va_tick_output) (void)arm_exec_call((uint32_t)va_tick_output, (uint32_t)clk, 0, 0, 0);
               if (va_tick_render) (void)arm_exec_call((uint32_t)va_tick_render, (uint32_t)clk, 0, 0, 0);
            }
         } else if (ue_media_tick_dbg) {
            fprintf(stderr, "[media] UE clock pointer is 0\n");
            /* Disable printing after first failure. */
            ue_media_tick_dbg = 0;
         }
      }
   }
   frame_stage_add(FRAME_STAGE_MEDIA, t_media);
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
   uint32_t va_config_rules = arm_exec_lookup_native(
         "com.epicgames.ue4.GameActivity", "nativeSetConfigRulesVariables");
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
   if (obb_in_apk)
      fprintf(stderr, "[loader] expansion is in the APK (assets/main.obb.png)\n");

   if (va_config_rules) {
      /* (env, thiz, String[] KeyValuePairs) — see the arm64 path: FAndroidMisc
       * parks every thread that needs a config-rules variable until this
       * arrives, so omitting it deadlocks UE before PreInit. */
      jclass str_cls = jvm->native.FindClass(&jvm->env, "java/lang/String");
      jobjectArray kv = jvm->native.NewObjectArray(&jvm->env, 0, str_cls, NULL);
      uint32_t a[3] = { env, ctx, (uint32_t)(uintptr_t)kv };
      fprintf(stderr, "[loader] UE4 nativeSetConfigRulesVariables (0 pairs)\n");
      arm_exec_calln(va_config_rules, a, 3);
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
      const char *proj = ue_project_name(pkg);
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
         /* The input queue reaches the glue the same way the window does:
          * pendingInputQueue (msgread+0x34 on ILP32) plus APP_CMD_INPUT_CHANGED,
          * which makes the glue attach it to its looper.  Without it the engine
          * has no touchscreen at all — AInputQueue_getEvent is the only path a
          * NativeActivity has for touches. */
         uint32_t inq = arm_exec_input_queue_handle();
         if (inq && pipe_off) {
            arm_exec_write32(instance + pipe_off + 0x34u, inq);
            fprintf(stderr, "[loader] UE4 pendingInputQueue@+0x%x = 0x%08x\n",
                    pipe_off + 0x34u, inq);
         }
         if (msgwrite >= 0) {
            /* APP_CMD_INPUT_CHANGED=0, START=10, RESUME=11, INIT_WINDOW=1,
             * GAINED_FOCUS=6 */
            static const int8_t cmds[] = { 0, 10, 11, 1, 6 };
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
      pump_run_frame(arm_exec_run_pending_threads);
      arm_exec_egl_swap();
      arm_exec_glfw_poll();
      perf_tick();
      if (frame < 5 || frame % 50 == 0)
         fprintf(stderr, "[loader] UE4 pump frame %d\n", frame);
      if (arm_exec_glfw_should_close()) break;
   }
   return EXIT_SUCCESS;
}

/* -------------------------------------------------------------------------
 * ARM64 UE NativeActivity pump
 * ---------------------------------------------------------------------- */
/* UE ships the same GameActivity under two packages: com.epicgames.ue4 for
 * UE4 and com.epicgames.unreal for UE5.  Look a native method up in both. */
/* --- dex-driven startup -------------------------------------------------
 *
 * The per-engine startup sequences below exist because there was no bytecode
 * VM: the loader called, by hand, the `native` methods that the app's own
 * Activity.onCreate() would have called — in the order we believed they came
 * in, with arguments reconstructed from the environment.  That is a
 * transcription of one engine's Java, and it only covers the calls we knew to
 * transcribe.
 *
 * With a dex interpreter the app can make those calls itself, which is both
 * shorter and engine-agnostic: whatever Activity the APK ships runs its real
 * onCreate.  This is the default; LUNARIA_DEX_START=0 selects the legacy
 * hand-written sequence for comparison.  Everything Android itself would
 * do — creating the ANativeActivity, the window, the APP_CMD_* deliveries —
 * stays with the loader either way; that is the platform's job, not the app's.
 */
static int
dex_start_requested(void)
{
   const char *e = getenv("LUNARIA_DEX_START");
   return !e || !*e || strcmp(e, "0") != 0;
}

/* First of `cands` that the APK's dex actually defines, or NULL. */
static const char *
dex_first_defined_class(const char *const *cands)
{
   for (int i = 0; cands[i]; ++i)
      if (dvm_jni_class_in_dex(cands[i]))
         return cands[i];
   return NULL;
}

/* Epic renamed the package at UE5 (com.epicgames.ue4 → com.epicgames.unreal). */
static const char *
ue_activity_dex_class(void)
{
   static const char *const cands[] = {
      "com/epicgames/unreal/GameActivity",
      "com/epicgames/ue4/GameActivity",
      NULL
   };
   return dex_first_defined_class(cands);
}

static int dex_call_lifecycle(struct jvm *jvm, jobject self, const char *cls,
                              const char *method, const char *sig,
                              int takes_null_arg);
static int start_manifest_providers(struct jvm *jvm, jobject context);

static jobject
ue_start_dex_application(struct jvm *jvm)
{
   static const char *const cands[] = {
      "com/epicgames/unreal/GameApplication",
      "com/epicgames/ue4/GameApplication",
      NULL
   };
   const char *cls = dex_first_defined_class(cands);
   if (!cls) return NULL;

   jclass k = jvm->native.FindClass(&jvm->env, cls);
   jmethodID ctor = k ? jvm->native.GetMethodID(&jvm->env, k, "<init>", "()V")
                      : NULL;
   jobject app = (k && ctor)
      ? jvm->native.NewObjectA(&jvm->env, k, ctor, NULL) : NULL;
   if (!app) {
      fprintf(stderr, "[loader] dex startup: cannot construct %s\n", cls);
      return NULL;
   }

   /* ActivityThread attaches an application Context before onCreate.  The
    * framework Context is intentionally plain; the Application wrapper keeps
    * it as its base and obtains package services through it. */
   jclass ck = jvm->native.FindClass(&jvm->env, "android/content/Context");
   jobject context = ck ? jvm->native.AllocObject(&jvm->env, ck) : NULL;
   jmethodID attach = jvm->native.GetMethodID(
      &jvm->env, k, "attachBaseContext", "(Landroid/content/Context;)V");
   if (attach && context) {
      jvalue a = { .l = context };
      fprintf(stderr, "[loader] dex startup: %s.attachBaseContext(Context)\n",
              cls);
      jvm->native.CallVoidMethodA(&jvm->env, app, attach, &a);
   }
   /* ActivityThread installs ContentProviders after attachBaseContext and
    * before Application.onCreate.  FirebaseInitProvider.initializeApp lives
    * here — skipping it left FirebaseApp.getInstance() throwing later. */
   start_manifest_providers(jvm, app);
   (void)dex_call_lifecycle(jvm, app, cls, "onCreate", "()V", 0);
   return app;
}

/* Calls one no-argument (or single-null-argument) lifecycle method through the
 * ordinary JNI entry points, which route into the bytecode VM whenever the dex
 * defines the method.  Returns 0 when there is no such method. */
static int
dex_call_lifecycle(struct jvm *jvm, jobject self, const char *cls,
                   const char *method, const char *sig, int takes_null_arg)
{
   jclass k = jvm->native.FindClass(&jvm->env, cls);
   jmethodID m = k ? jvm->native.GetMethodID(&jvm->env, k, method, sig) : NULL;
   if (!m)
      return 0;
   fprintf(stderr, "[loader] dex startup: %s.%s%s\n", cls, method, sig);
   if (takes_null_arg) {
      jvalue arg = { .l = NULL }; /* a fresh launch has no savedInstanceState */
      jvm->native.CallVoidMethodA(&jvm->env, self, m, &arg);
   } else {
      jvm->native.CallVoidMethodA(&jvm->env, self, m, NULL);
   }
   return 1;
}

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

   const char *dex_cls = dex_start_requested() ? ue_activity_dex_class() : NULL;
   jobject activity = NULL;
   jobject application = NULL;
   if (dex_cls) {
      /* Android creates and starts the manifest Application before it
       * instantiates the first Activity. */
      application = ue_start_dex_application(jvm);
      /* ActivityThread instantiates an Activity by running its no-argument
       * constructor before delivering onCreate.  AllocObject alone skips all
       * instance field initialisers and produces an object Android can never
       * produce; in UE that leaves ProcessSystemInfoLock null. */
      jclass activity_class = jvm->native.FindClass(&jvm->env, dex_cls);
      jmethodID ctor = activity_class
         ? jvm->native.GetMethodID(&jvm->env, activity_class, "<init>", "()V")
         : NULL;
      if (activity_class && ctor)
         activity = jvm->native.NewObjectA(&jvm->env, activity_class, ctor, NULL);
      if (!activity)
         errx(EXIT_FAILURE, "dex startup: cannot construct %s", dex_cls);
      if (application) {
         jclass ac = jvm->native.FindClass(&jvm->env, "android/app/Activity");
         jfieldID af = ac ? jvm->native.GetFieldID(
            &jvm->env, ac, "application", "Landroid/app/Application;") : NULL;
         if (af)
            jvm->native.SetObjectField(&jvm->env, activity, af, application);
      }
   }
   if (!activity)
      activity = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "com/epicgames/ue4/GameActivity"));
   if (!activity)
      activity = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "com/epicgames/unreal/GameActivity"));
   if (!activity)
      activity = jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "android/app/NativeActivity"));
   jni_set_current_activity(&jvm->env, activity);
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
   uint64_t va_config_rules   = ue4_native64("nativeSetConfigRulesVariables");
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
   /* LUNARIA_DEX_START: let GameActivity.onCreate() issue these calls itself,
    * from the APK's own bytecode, instead of the transcription below.  The
    * transcription only runs for the entry points onCreate did not reach. */
   if (dex_start_requested()) {
      if (!dex_cls) {
         fprintf(stderr, "[loader] dex startup: no GameActivity in the dex — "
                         "using the built-in startup sequence\n");
      } else if (dex_call_lifecycle(jvm, activity, dex_cls, "onCreate",
                                    "(Landroid/os/Bundle;)V", 1)) {
         va_config_rules = va_set_global = va_set_ver = 0;
         va_set_obb = va_set_obb_paths = 0;
         arm64_exec_run_pending_threads();
      }
   }

   uint64_t obb_in_apk = ue_obb_in_apk();
   if (obb_in_apk)
      fprintf(stderr, "[loader] expansion is in the APK (assets/main.obb.png)\n");

   if (va_config_rules) {
      /* (env, thiz, String[] KeyValuePairs) — GameActivity evaluates
       * configrules.txt in onCreate and hands the result to the engine.
       * FAndroidMisc blocks every thread that needs a config-rules variable
       * until this lands ("thread waiting for configrules to be set"), so
       * skipping it deadlocks UE before PreInit.  A device whose APK has no
       * matching rules passes an empty array; do the same. */
      jclass str_cls = jvm->native.FindClass(&jvm->env, "java/lang/String");
      jobjectArray kv = jvm->native.NewObjectArray(&jvm->env, 0, str_cls, NULL);
      uint64_t a[3] = { env, ctx, (uint64_t)(uintptr_t)kv };
      fprintf(stderr, "[loader] UE arm64 nativeSetConfigRulesVariables (0 pairs)\n");
      arm64_exec_call8(va_config_rules, a, 3);
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
      const char *proj = ue_project_name(pkg);
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
         /* Same for the input queue: pendingInputQueue (msgread+0x50 on LP64)
          * plus APP_CMD_INPUT_CHANGED makes the glue attach it to its looper.
          * A NativeActivity has no other path for touches. */
         uint32_t inq = arm_exec_input_queue_handle();
         if (inq && pipe_off) {
            arm64_exec_write64(instance_va + pipe_off + 0x50u, (uint64_t)inq);
            fprintf(stderr, "[loader] UE arm64 pendingInputQueue@+0x%x = 0x%08x\n",
                    pipe_off + 0x50u, inq);
         }
         if (msgwrite >= 0) {
            /* APP_CMD_INPUT_CHANGED=0, START=10, RESUME=11, INIT_WINDOW=1,
             * GAINED_FOCUS=6 */
            static const int8_t cmds[] = { 0, 10, 11, 1, 6 };
            for (size_t i = 0; i < sizeof cmds; ++i) {
               int8_t c = cmds[i];
               if (write(msgwrite, &c, 1) != 1)
                  fprintf(stderr, "[loader] UE arm64 write APP_CMD %d failed\n", c);
               arm64_exec_run_pending_threads();
            }
         }
      }
   }

   /* NativeActivity's real superclass methods deliver the native START and
    * RESUME callbacks while the Java lifecycle call is on the stack, and its
    * SurfaceView can deliver INIT_WINDOW before GameActivity.onResume reaches
    * nativeResumeMainInit().  Our bytecode-side NativeActivity methods are
    * framework stubs, so the command block above is that missing superclass /
    * window-manager work and must precede the app's lifecycle continuation.
    *
    * Doing this afterwards creates a circular wait which no Android device
    * has: Cross Worlds' nativeResumeMainInit sets GResumeMainInit and waits
    * for AndroidMain to finish its first phase, while AndroidMain is waiting
    * for INIT_WINDOW which the loader planned to send only after onResume
    * returned.  The callback's 120-second safety limit eventually broke the
    * cycle, making every title-to-game transition look like slow emulation.
    * Preserve the causal order instead of shortening or bypassing the wait. */
   if (dex_cls) {
      dex_call_lifecycle(jvm, activity, dex_cls, "onStart",  "()V", 0);
      dex_call_lifecycle(jvm, activity, dex_cls, "onResume", "()V", 0);
      arm64_exec_run_pending_threads();
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

   g_dump_arm64 = 1;
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
      touch_test_tick(frame);
      /* SetDesiredViewSize resized the surface.  Deliver the surfaceChanged
       * the engine would have got on a device, or it keeps mapping input
       * against the size it was told at startup. */
      {
         int nw = 0, nh = 0;
         if (arm_exec_take_view_resize(&nw, &nh) && nw > 0 && nh > 0) {
            if (va_set_win)
               arm64_exec_call(va_set_win, env, ctx, (nh > nw) ? 1u : 0u, 0);
            if (va_set_surf) {
               fprintf(stderr, "[loader] UE arm64 nativeSetSurfaceViewInfo "
                       "%dx%d (view resized)\n", nw, nh);
               arm64_exec_call(va_set_surf, env, ctx, (uint64_t)nw,
                               (uint64_t)nh);
            }
         }
      }
      pump_run_frame(arm64_exec_run_pending_threads);
      /* The engine renders on its own thread and swaps through the EGL
       * bridge; present here too so a frame reaches the window even when the
       * guest's swap goes through the Java surface path. */
      {
         const uint64_t t_swap = frame_now_ns();
         arm64_exec_egl_swap();
         frame_stage_add(FRAME_STAGE_SWAP, t_swap);
         const uint64_t t_input = frame_now_ns();
         arm64_exec_glfw_poll();
         frame_stage_add(FRAME_STAGE_INPUT, t_input);
      }
      /* The frame is presented and nothing is half-done: hand the interpreter
       * lock to any bytecode thread waiting for it before starting the next
       * one.  This is the pump's share of keeping the lock fair. */
      dvm_gil_yield(dvm_current());
      stall_watch_tick();
      perf_tick();
      thread_dump_request_tick();
      if (frame < 5 || frame % 50 == 0)
         fprintf(stderr, "[loader] UE arm64 pump frame %d\n", frame);
      if (arm64_exec_glfw_should_close()) break;
   }
   return EXIT_SUCCESS;
}

static uint64_t
arm64_lookup_native_two(const char *primary, const char *fallback,
                        const char *name)
{
   uint64_t va = arm64_exec_lookup_native(primary, name);
   return va ? va : arm64_exec_lookup_native(fallback, name);
}

static uint64_t
arm64_lookup_native_sig_two(const char *primary, const char *fallback,
                            const char *name, char *sig, size_t sig_size)
{
   uint64_t va = arm64_exec_lookup_native_sig(primary, name, sig, sig_size);
   return va ? va : arm64_exec_lookup_native_sig(fallback, name, sig, sig_size);
}

static uint32_t
arm_lookup_native_two(const char *primary, const char *fallback,
                      const char *name)
{
   uint32_t va = arm_exec_lookup_native(primary, name);
   return va ? va : arm_exec_lookup_native(fallback, name);
}

static uint32_t
arm_lookup_native_sig_two(const char *primary, const char *fallback,
                          const char *name, char *sig, size_t sig_size)
{
   uint32_t va = arm_exec_lookup_native_sig(primary, name, sig, sig_size);
   return va ? va : arm_exec_lookup_native_sig(fallback, name, sig, sig_size);
}

static int
run_unity_game_arm64(struct jvm *jvm, jobject existing_context,
                     int call_init_jni)
{
   static const char *cls     = "com.unity3d.player.UnityPlayer";
   static const char *cls_svc = "com.unity3d.player.UnityPlayerForActivityOrService";

#define LOOKUP2(name) arm64_lookup_native_two(cls, cls_svc, name)
#define LOOKUP2_SIG(name, sig_buf) \
   arm64_lookup_native_sig_two(cls, cls_svc, name, sig_buf, sizeof(sig_buf))

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
   const jobject context = existing_context ? existing_context
      : jvm->native.AllocObject(&jvm->env,
            jvm->native.FindClass(&jvm->env, "android/app/Activity"));
   uint64_t ctx = (uint64_t)(uintptr_t)context;
   jni_set_current_activity(&jvm->env, context);

   if (va_file) {
      const char *apk = lunaria_apk_mount_path();
      if (apk && *apk) {
         fprintf(stderr, "[loader] arm64 calling nativeFile (%s)...\n", apk);
         jobject str = jvm->native.NewStringUTF(&jvm->env, apk);
         arm64_exec_call(va_file, env, ctx, (uint64_t)(uintptr_t)str, 0);
         arm64_exec_run_pending_threads();
      }
   }

   if (call_init_jni) {
      fprintf(stderr, "[loader] arm64 calling initJni (va=0x%llx)...\n",
              (unsigned long long)va_init_jni);
      arm64_exec_call(va_init_jni, env, ctx, ctx, 0);
      arm64_exec_run_pending_threads();
      fprintf(stderr, "[loader] arm64 initJni done\n");
   } else {
      fprintf(stderr, "[loader] arm64 Unity was initialized by the Activity\n");
   }

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
   g_dump_arm64 = 1;
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

/* Start the package the way Android does: construct the launcher Activity and
 * run its lifecycle out of the dex.  `ANDROID_LAUNCH_ACTIVITY` comes from
 * lunaria-apk.sh, which already parses the APK's AndroidManifest.
 *
 * This is the engine-agnostic path.  It carries no knowledge of Unity or
 * Unreal: whatever the APK's Activity does in onCreate — loading its native
 * libraries, building its player object, calling its own JNI methods — is its
 * own bytecode running.  What it does not do is drive rendering: an app that
 * draws through a GLSurfaceView is driven by callbacks from a real Android
 * framework, which lunaria does not yet deliver.  So this gets the package
 * started and pumped; frames only appear for apps whose native side presents
 * through the EGL bridge on its own thread.
 */
static int
start_manifest_providers(struct jvm *jvm, jobject context)
{
   const char *encoded = getenv("ANDROID_CONTENT_PROVIDERS");
   if (!encoded || !*encoded) return 0;
   char *providers = strdup(encoded);
   if (!providers) return 0;
   int started = 0;
   char *save = NULL;
   for (char *entry = strtok_r(providers, ";", &save); entry;
        entry = strtok_r(NULL, ";", &save)) {
      char *authority = strchr(entry, '|');
      if (authority) *authority++ = '\0';
      char *metadata = authority ? strchr(authority, '|') : NULL;
      if (metadata) *metadata++ = '\0';
      char cls[256];
      size_t n = 0;
      for (const char *p = entry; *p && n + 1 < sizeof cls; ++p)
         cls[n++] = (*p == '.') ? '/' : *p;
      cls[n] = '\0';
      if (!*cls || !dvm_jni_class_in_dex(cls)) continue;

      jclass provider_class = jvm->native.FindClass(&jvm->env, cls);
      jmethodID ctor = provider_class ? jvm->native.GetMethodID(
         &jvm->env, provider_class, "<init>", "()V") : NULL;
      jobject provider = (provider_class && ctor) ? jvm->native.NewObjectA(
         &jvm->env, provider_class, ctor, NULL) : NULL;
      if (!provider) {
         fprintf(stderr, "[loader] content provider: cannot construct %s\n", cls);
         continue;
      }

      jclass info_class = jvm->native.FindClass(&jvm->env,
                                                 "android/content/pm/ProviderInfo");
      jobject info = info_class
         ? jvm->native.AllocObject(&jvm->env, info_class) : NULL;
      if (info && authority) {
         jfieldID field = jvm->native.GetFieldID(&jvm->env, info_class,
                                                  "authority",
                                                  "Ljava/lang/String;");
         jstring value = jvm->native.NewStringUTF(&jvm->env, authority);
         if (field && value)
            jvm->native.SetObjectField(&jvm->env, info, field, value);
      }
      if (info && metadata && *metadata) {
         jclass bundle_class = jvm->native.FindClass(&jvm->env,
                                                       "android/os/Bundle");
         jmethodID bundle_ctor = bundle_class ? jvm->native.GetMethodID(
            &jvm->env, bundle_class, "<init>", "()V") : NULL;
         jobject bundle = (bundle_class && bundle_ctor)
            ? jvm->native.NewObjectA(&jvm->env, bundle_class, bundle_ctor, NULL)
            : NULL;
         if (bundle) {
            char *meta_save = NULL;
            for (char *pair = strtok_r(metadata, ",", &meta_save); pair;
                 pair = strtok_r(NULL, ",", &meta_save)) {
               char *value = strchr(pair, '~');
               if (!value) continue;
               *value++ = '\0';
               char *end = NULL;
               long iv = strtol(value, &end, 0);
               if (end && *end == '\0') {
                  jmethodID put = jvm->native.GetMethodID(
                     &jvm->env, bundle_class, "putInt",
                     "(Ljava/lang/String;I)V");
                  if (put) {
                     jvalue a[2] = {
                        { .l = jvm->native.NewStringUTF(&jvm->env, pair) },
                        { .i = (jint)iv }
                     };
                     jvm->native.CallVoidMethodA(&jvm->env, bundle, put, a);
                  }
               } else {
                  jmethodID put = jvm->native.GetMethodID(
                     &jvm->env, bundle_class, "putString",
                     "(Ljava/lang/String;Ljava/lang/String;)V");
                  if (put) {
                     jvalue a[2] = {
                        { .l = jvm->native.NewStringUTF(&jvm->env, pair) },
                        { .l = jvm->native.NewStringUTF(&jvm->env, value) }
                     };
                     jvm->native.CallVoidMethodA(&jvm->env, bundle, put, a);
                  }
               }
            }
            jfieldID field = jvm->native.GetFieldID(
               &jvm->env, info_class, "metaData", "Landroid/os/Bundle;");
            if (field)
               jvm->native.SetObjectField(&jvm->env, info, field, bundle);
         }
      }
      if (info) {
         jfieldID grant = jvm->native.GetFieldID(&jvm->env, info_class,
                                                  "grantUriPermissions", "Z");
         if (grant)
            jvm->native.SetBooleanField(&jvm->env, info, grant, JNI_TRUE);
      }
      jmethodID attach = jvm->native.GetMethodID(
         &jvm->env, provider_class, "attachInfo",
         "(Landroid/content/Context;Landroid/content/pm/ProviderInfo;)V");
      if (attach) {
         jvalue args[2] = { { .l = context }, { .l = info } };
         jvm->native.CallVoidMethodA(&jvm->env, provider, attach, args);
      }
      if (dex_call_lifecycle(jvm, provider, cls, "onCreate", "()Z", 0)) {
         fprintf(stderr, "[loader] content provider: started %s\n", cls);
         ++started;
      }
   }
   free(providers);
   return started;
}

static int
run_dex_activity_arm64(struct jvm *jvm)
{
   const char *act = getenv("ANDROID_LAUNCH_ACTIVITY");
   if (!act || !*act) {
      fprintf(stderr, "[loader] dex startup: ANDROID_LAUNCH_ACTIVITY unset\n");
      return EXIT_FAILURE;
   }
   /* The manifest gives a dotted name; the VM and JNI want slashes. */
   char cls[256];
   size_t n = 0;
   for (const char *p = act; *p && n + 1 < sizeof cls; ++p)
      cls[n++] = (*p == '.') ? '/' : *p;
   cls[n] = '\0';

   if (!dvm_jni_class_in_dex(cls)) {
      fprintf(stderr, "[loader] dex startup: %s is not in any dex\n", cls);
      return EXIT_FAILURE;
   }

   if (!arm64_exec_host_egl_init())
      fprintf(stderr, "[loader] arm64 host EGL init failed\n");

   /* ActivityThread first creates the process Application and attaches a base
    * Context.  Activity.getApplication() is therefore non-null even when the
    * manifest uses the default android.app.Application. */
   jclass context_class = jvm->native.FindClass(&jvm->env,
                                                 "android/content/Context");
   jobject base_context = context_class
      ? jvm->native.AllocObject(&jvm->env, context_class) : NULL;
   const char *app_env = getenv("ANDROID_APPLICATION_CLASS");
   char app_cls[256] = "android/app/Application";
   if (app_env && *app_env) {
      size_t an = 0;
      for (const char *p = app_env; *p && an + 1 < sizeof app_cls; ++p)
         app_cls[an++] = (*p == '.') ? '/' : *p;
      app_cls[an] = '\0';
   }
   jclass application_class = jvm->native.FindClass(&jvm->env, app_cls);
   jobject application = NULL;
   if (application_class && dvm_jni_class_in_dex(app_cls)) {
      jmethodID app_ctor = jvm->native.GetMethodID(&jvm->env,
                                      application_class, "<init>", "()V");
      if (app_ctor)
         application = jvm->native.NewObjectA(&jvm->env, application_class,
                                               app_ctor, NULL);
   }
   if (!application) {
      application_class = jvm->native.FindClass(&jvm->env,
                                                 "android/app/Application");
      application = application_class
         ? jvm->native.AllocObject(&jvm->env, application_class) : NULL;
   }
   if (application && base_context) {
      jmethodID attach = jvm->native.GetMethodID(
         &jvm->env, application_class, "attachBaseContext",
         "(Landroid/content/Context;)V");
      if (attach) {
         jvalue arg = { .l = base_context };
         jvm->native.CallVoidMethodA(&jvm->env, application, attach, &arg);
      }
   }
   /* ActivityThread installs all manifest providers before delivering the
    * process Application.onCreate().  Firebase and AndroidX Startup rely on
    * this ordering for process-wide initialisation. */
   start_manifest_providers(jvm, application ? application : base_context);

   if (application && app_env && *app_env && dvm_jni_class_in_dex(app_cls))
      (void)dex_call_lifecycle(jvm, application, app_cls, "onCreate", "()V", 0);

   /* ActivityThread uses Instrumentation.newActivity(), which invokes the
    * launcher's no-argument constructor before attach()/onCreate().  Using
    * AllocObject here created an object Android can never create: every Java
    * field initializer in ComponentActivity, FragmentActivity, AppCompat and
    * the application class was skipped.  The first super.onCreate() then saw
    * null SavedStateRegistryController/FragmentController fields. */
   jclass activity_class = jvm->native.FindClass(&jvm->env, cls);
   jmethodID ctor = activity_class
      ? jvm->native.GetMethodID(&jvm->env, activity_class, "<init>", "()V")
      : NULL;
   jobject activity = (activity_class && ctor)
      ? jvm->native.NewObjectA(&jvm->env, activity_class, ctor, NULL) : NULL;
   if (!activity) {
      fprintf(stderr, "[loader] dex startup: cannot construct %s\n", cls);
      return EXIT_FAILURE;
   }
   jni_set_current_activity(&jvm->env, activity);

   /* Activity.attach() establishes both ContextWrapper.mBase and the process
    * Application before any lifecycle callback.  The host framework exposes
    * those two observable pieces directly. */
   if (base_context) {
      jmethodID attach = jvm->native.GetMethodID(
         &jvm->env, activity_class, "attachBaseContext",
         "(Landroid/content/Context;)V");
      if (attach) {
         jvalue arg = { .l = base_context };
         jvm->native.CallVoidMethodA(&jvm->env, activity, attach, &arg);
      }
   }
   if (application) {
      jclass platform_activity = jvm->native.FindClass(&jvm->env,
                                                        "android/app/Activity");
      jfieldID app_field = platform_activity ? jvm->native.GetFieldID(
         &jvm->env, platform_activity, "application",
         "Landroid/app/Application;") : NULL;
      if (app_field)
         jvm->native.SetObjectField(&jvm->env, activity, app_field, application);
   }

   if (!dex_call_lifecycle(jvm, activity, cls, "onCreate",
                           "(Landroid/os/Bundle;)V", 1)) {
      fprintf(stderr, "[loader] dex startup: %s has no onCreate\n", cls);
      return EXIT_FAILURE;
   }
   arm64_exec_run_pending_threads();
   /* onStart/onResume are what make an Activity visible and running; an app
    * that starts its render thread in onResume never starts without them. */
   dex_call_lifecycle(jvm, activity, cls, "onStart",  "()V", 0);
   dex_call_lifecycle(jvm, activity, cls, "onResume", "()V", 0);
   arm64_exec_run_pending_threads();

   /* A GLSurfaceView receives surfaceCreated/surfaceChanged from Android's
    * window manager after onResume.  The host has no framework compositor to
    * emit those callbacks, so connect the already-initialised Unity player to
    * the host Surface here.  The Activity has already called initJni through
    * its own bytecode; doing that a second time corrupts Unity global state. */
   if (arm64_exec_lookup_native("com.unity3d.player.UnityPlayer",
                                "nativeRender"))
      return run_unity_game_arm64(jvm, activity, 0);

   int max_frames = 0;
   { const char *mf = getenv("LUNARIA_MAX_FRAMES"); if (mf && *mf) max_frames = atoi(mf); }
   fprintf(stderr, "[loader] dex startup: entering pump loop (max_frames=%d)\n",
           max_frames);
   for (int frame = 0; max_frames <= 0 || frame < max_frames; ++frame) {
      if (arm_exec_guest_abort_count() > 0) {
         fprintf(stderr, "[loader] guest abort — stopping dex loop (frame %d)\n", frame);
         break;
      }
      pump_run_frame(arm64_exec_run_pending_threads);
      arm64_exec_egl_swap();
      arm64_exec_glfw_poll();
      perf_tick();
      if (frame < 5 || frame % 50 == 0)
         fprintf(stderr, "[loader] dex pump frame %d\n", frame);
      if (arm64_exec_glfw_should_close()) break;
   }
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
      return run_unity_game_arm64(jvm, NULL, 1);
   /* Neither engine's native entry point is exported.  An ordinary Android
    * app has none: its entry point is the launcher Activity, in the dex.  With
    * a bytecode VM that is runnable, so start it the way Android does instead
    * of giving up on the package. */
   if (run_dex_activity_arm64(jvm) == EXIT_SUCCESS)
      return EXIT_SUCCESS;
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
#define LOOKUP2(name) arm_lookup_native_two(cls, cls_svc, name)
#define LOOKUP2_SIG(name, sig_buf) \
   arm_lookup_native_sig_two(cls, cls_svc, name, sig_buf, sizeof(sig_buf))

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
   jni_set_current_activity(&jvm->env, context);
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
      /* フォーカス再送: 実機では surfaceChanged 後に focus が届く。
       * ループ前の nativeFocusChanged はエンジン初期化で上書きされる
       * 疑いがあるため、タップ前に再送して入力ゲートを開く */
      {
         static int tt_frame = 60, tt_parsed = 0;
         if (!tt_parsed) {
            tt_parsed = 1;
            const char *tf = getenv("LUNARIA_TOUCH_FRAME");
            if (tf) { int v = atoi(tf); if (v > 0) tt_frame = v; }
         }
         if (getenv("LUNARIA_TOUCH_TEST") && frame_count == tt_frame - 10 &&
             tt_frame > 10 && va_focus) {
            arm_exec_call(va_focus, env, ctx, 1, 0);
            fprintf(stderr, "[loader] nativeFocusChanged(1) re-sent (frame %d)\n",
                    frame_count);
         }
      }
      touch_test_tick(frame_count);
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

   /* An ordinary Android application has no native process entry point.
    * ActivityThread starts its Application/Activity bytecode first, and the
    * app loads each JNI library itself with System.loadLibrary().  Requiring
    * an arbitrary .so here inverted that order and also rejected APKs whose
    * alphabetically first helper library had no JNI_OnLoad. */
   if (!strcmp(argv[1], "--apk-process-arm64") ||
       !strcmp(argv[1], "--apk-process-arm32")) {
      const int is_a64 = !strcmp(argv[1], "--apk-process-arm64");
      const char *libdir = getenv("ANDROID_NATIVE_LIB_DIR");
      if (!libdir || !*libdir)
         errx(EXIT_FAILURE, "ANDROID_NATIVE_LIB_DIR is required for APK process startup");

      static struct jvm jvm;
      jvm_init(&jvm);
      int init_ok = is_a64 ? arm64_exec_context_init(&jvm)
                           : arm_exec_context_init(&jvm);
      if (init_ok < 0)
         errx(EXIT_FAILURE, "%s context init failed",
              is_a64 ? "arm64" : "arm32");
      arm_exec_set_main_lib_dir(libdir);
      if (!arm_exec_host_egl_init())
         fprintf(stderr, "[loader] early APK-process host EGL init failed\n");

      int ret = is_a64 ? run_dex_activity_arm64(&jvm)
                       : EXIT_FAILURE;
      if (!is_a64)
         fprintf(stderr, "[loader] ARM32 APK-process startup is not implemented yet\n");
      jvm_release(&jvm);
      printf("exiting\n");
      return ret;
   }

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
