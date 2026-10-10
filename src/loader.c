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
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <elf.h>
#ifndef _WIN32
#include <err.h>
#else
#include <stdarg.h>
#endif
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include "lunaria_os.h"
#ifdef _WIN32
#include <pthread.h>
#endif
#include "linker/dlfcn.h"
#include "linker/linker.h"
#include "jvm/jvm.h"
#include "arm_exec.h"
#include "luna_compositor.h"
#include "dvm/dvm_jni.h"

#ifdef _WIN32
/* This call targets the Android ELF loader, using Android flag values. */
#define RTLD_LOCAL 0
#define RTLD_NOW 2
static void warnx(const char *format, ...) {
   va_list args;
   va_start(args, format); vfprintf(stderr, format, args); va_end(args);
   fputc('\n', stderr);
}
static void errx(int status, const char *format, ...) {
   va_list args;
   va_start(args, format); vfprintf(stderr, format, args); va_end(args);
   fputc('\n', stderr); exit(status);
}
static void err(int status, const char *format, ...) {
   int saved = errno;
   va_list args;
   va_start(args, format); vfprintf(stderr, format, args); va_end(args);
   fprintf(stderr, ": %s\n", strerror(saved)); exit(status);
}
#endif

/* Android library leaves are bounded independently of the host filesystem. */
#define LUNA_GUEST_NAME_MAX 255

static lunaria_touch_event
touch_to_lunaria(const ArmExecTouchEvent *te)
{
   return *te;
}
#include "dvm/dvm_media.h"

/* Exposed from arm_exec.cpp for diagnostic dumps */
extern void arm_exec_svc_ring_dump(void);
extern void arm64_exec_svc_ring_dump(void);

/* Load APK-private AArch64 DT_NEEDED libraries before their consumer.  The
 * old A64 path named a handful of Unity/UE libraries explicitly, so a normal
 * dependency such as libunity.so -> libmain.so was silently left unresolved.
 * Android system libraries are provided by Lunaria's SVC/runtime bridge and
 * therefore deliberately have no guest ELF beside the APK libraries. */
static int a64_dep_seen(const char *name, char seen[][LUNA_GUEST_NAME_MAX + 1], size_t n)
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
                              char names[][LUNA_GUEST_NAME_MAX + 1], size_t cap)
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
      while (len < LUNA_GUEST_NAME_MAX && (ch = fgetc(f)) != EOF && ch != '\0')
         names[count][len++] = (char)ch;
      names[count][len] = '\0';
      if (len && (ch == '\0' || len == LUNA_GUEST_NAME_MAX)) ++count;
   }
   free(dyn);
out:
   free(ph);
   if (f) fclose(f);
   return count;
}

/* A directory of real AArch64 Android platform libraries.
 *
 * Everything the emulator answers with an SVC thunk costs the guest a JIT
 * exit, and the measurements put libm alone — pow and sincosf — at 40% of
 * every exit Cross Worlds makes.  Those are pure computation with no business
 * leaving the JIT at all; the right fix is the same code a device runs, as
 * guest code.  Point LUNARIA_SYSLIB_DIR at a directory holding them (an
 * Android NDK ships an arm64 libm.so under
 * build-tools/<v>/renderscript/lib/intermediates/arm64-v8a/), and any
 * DT_NEEDED that is not beside the APK's own libraries is looked up there
 * before being left to the SVC bridge.  Unset, nothing changes. */
static const char *a64_syslib_dir(void)
{
   static const char *dir;
   static int probed;
   if (!probed) {
      probed = 1;
      dir = getenv("LUNARIA_SYSLIB_DIR");
      if (dir && !*dir) dir = NULL;
   }
   return dir;
}

/* Platform libraries every Android process sees before any app DSO loads.
 *
 * The ELF-on-the-command-line path used to do this; the APK-process path
 * (`--apk-process-arm64`) did not, so the first System.loadLibrary that
 * imported a bionic data symbol (optarg, for Crashlytics) failed with
 * "cannot locate symbol" even though syslib-arm64 held the real image.
 * A device does not have two linkers — preload once, for every entry. */
static void a64_preload_platform_libs(void)
{
   const char *sys = a64_syslib_dir();
   char libpath[PATH_MAX];
   struct stat stbuf;

   if (!sys) return;

   /* libc as a pure-code provider only — see the comment at the ELF-path
    * call site.  Full ctor/TLS bootstrap is still the emulator's job. */
   {
      int have_pure = 0;
      if ((size_t)snprintf(libpath, sizeof libpath,
                           "%s/libc-pure.so", sys) < sizeof libpath &&
          stat(libpath, &stbuf) == 0 && arm64_elf_is_arm64(libpath))
         have_pure = 1;
      else if ((size_t)snprintf(libpath, sizeof libpath,
                                "%s/libc.so", sys) < sizeof libpath &&
               stat(libpath, &stbuf) == 0 && arm64_elf_is_arm64(libpath))
         have_pure = 1; /* legacy syslib layout */
      if (have_pure) {
         printf("preloading arm64 pure-code library: %s\n", libpath);
         if (arm64_exec_load_library(libpath, 0) < 0)
            warnx("failed to load pure-code library %s", libpath);
      }
   }

   /* Guest-side malloc family as in-process code instead of traps. */
   {
      const char *off = getenv("LUNARIA_GUEST_LIBC");
      if (!(off && !strcmp(off, "0")) &&
          (size_t)snprintf(libpath, sizeof libpath,
                           "%s/liblunaria_guest.so", sys) < sizeof libpath &&
          stat(libpath, &stbuf) == 0 && arm64_elf_is_arm64(libpath)) {
         printf("preloading arm64 guest libc: %s\n", libpath);
         if (arm64_exec_load_library(libpath, 0) < 0)
            warnx("failed to load guest libc %s", libpath);
      }
   }
}

static void a64_preload_needed(const char *path, const char *dir,
                               char seen[][LUNA_GUEST_NAME_MAX + 1], size_t *seen_n)
{
   char needed[64][LUNA_GUEST_NAME_MAX + 1];
   size_t n = a64_read_needed(path, needed, 64);
   for (size_t i = 0; i < n; ++i) {
      char dep_path[PATH_MAX];
      struct stat st;
      if (a64_dep_seen(needed[i], seen, *seen_n)) continue;
      if (*seen_n < 128) {
         memcpy(seen[*seen_n], needed[i], LUNA_GUEST_NAME_MAX + 1);
         seen[*seen_n][LUNA_GUEST_NAME_MAX] = '\0';
         ++*seen_n;
      }
      size_t dir_len = strlen(dir), name_len = strnlen(needed[i], LUNA_GUEST_NAME_MAX + 1);
      if (dir_len + name_len + 1 > sizeof dep_path) {
         warnx("AArch64 dependency path too long: %s", needed[i]);
         continue;
      }
      memcpy(dep_path, dir, dir_len);
      memcpy(dep_path + dir_len, needed[i], name_len + 1);
      if (stat(dep_path, &st) != 0 || !arm64_elf_is_arm64(dep_path)) {
         /* Not beside the APK's libraries.  Before handing it to the SVC
          * bridge, look for the real thing among the platform libraries. */
         const char *sys = a64_syslib_dir();
         int found = 0;
         if (sys) {
            char cand[PATH_MAX];
            /* libc is process state, not merely a code DSO.  The copy in the
             * syslib directory is loaded separately below as a deliberately
             * filtered pure-code image.  Loading it here would run bionic's
             * constructors and split pthread/allocator/errno ownership
             * between bionic and Lunaria's Android ABI implementation. */
            if (strcmp(needed[i], "libc.so") != 0 &&
                (size_t)snprintf(cand, sizeof cand, "%s/%s", sys, needed[i])
                < sizeof cand &&
                stat(cand, &st) == 0 && arm64_elf_is_arm64(cand)) {
               printf("preloading arm64 platform library: %s\n", cand);
               if (arm64_exec_load_library(cand, 0) < 0)
                  warnx("failed to load platform library %s", cand);
               else
                  found = 1;
            }
         }
         if (!found)
            continue; /* Android platform library: handled by the emulator. */
         continue;    /* loaded from the platform directory, not from `dir` */
      }
      a64_preload_needed(dep_path, dir, seen, seen_n);
      printf("preloading arm64 DT_NEEDED: %s\n", dep_path);
      if (arm64_exec_load_library(dep_path, 0) < 0)
         warnx("failed to preload AArch64 dependency %s", dep_path);
   }
}

void arm64_loader_load_needed(const char *path)
{
   char dir[PATH_MAX];
   char seen[128][LUNA_GUEST_NAME_MAX + 1];
   size_t seen_n = 0;
   const char *slash = path ? strrchr(path, '/') : NULL;
   size_t n = slash ? (size_t)(slash - path) + 1u : 0u;
   if (!path || n >= sizeof dir) return;
   memcpy(dir, path, n);
   dir[n] = '\0';
   a64_preload_needed(path, dir, seen, &seen_n);
}

/* LUNARIA_TOUCH_FIFO — taps arriving while the emulator is already running.
 *
 * LUNARIA_TOUCH_TEST can only describe a script written before launch, keyed
 * on a pump frame number.  Anything that waits on the network — a title
 * screen that asks to be tapped once the session is up — lands on a different
 * frame every run, so a fixed script either fires too early or not at all.
 *
 * This is the same injection path, driven from outside instead: a FIFO whose
 * lines are "x,y" or "x,y,hold", the coordinates being percentages of the
 * framebuffer by default (see touch_parse_coord below).  Reads are
 * non-blocking and happen once per pump frame, so an empty FIFO costs one
 * read(2) that returns EAGAIN.  Held taps finish on a later frame, which is
 * what a guest expects — a DOWN and an UP in the same frame is not a tap.
 */
#define TF_MAX 16
static struct { float x, y; int up_frame; } g_tf_hold[TF_MAX];
static int g_tf_nhold;
static intptr_t g_tf_fd = -2;
static char g_tf_buf[256];
static size_t g_tf_len;

/* One coordinate of a tap, for every LUNARIA_TOUCH_* spelling.
 *
 * A percentage of the framebuffer is the default reading of a bare number.
 * The window size is a launch option here (LUNARIA_WIDTH / LUNARIA_HEIGHT)
 * and a device property everywhere else, but a game's layout is the same
 * layout at every size: "50,92" presses the button at the bottom middle on
 * any screen, while the pixel pair that hit it at 1280x720 presses empty
 * background at 1920x1080.  Coordinates written down once and replayed
 * against a differently-sized run were exactly the trap the Cross Worlds
 * notes record ("the old 512,505 now lands on Sign in with X").
 *
 * "50%" says the same thing explicitly.  An exact guest pixel is still
 * reachable as "640px" -- needed when a coordinate came from a screenshot
 * or a guest-side dump rather than from looking at the layout.
 *
 * Returns 0 and leaves *out untouched when the text is not a coordinate.
 * *end, when given, receives the first character not consumed.
 */
static int touch_parse_coord(const char *s, int extent, float *out,
                             const char **end)
{
   char *stop = NULL;
   double v;
   if (!s) return 0;
   while (*s == ' ' || *s == '\t') ++s;
   v = strtod(s, &stop);
   if (stop == s) return 0;
   while (*stop == ' ' || *stop == '\t') ++stop;
   if (stop[0] == 'p' && stop[1] == 'x') {
      stop += 2;                      /* guest pixels, verbatim */
   } else {
      if (stop[0] == '%') ++stop;     /* "50%" and "50" mean the same thing */
      v = v * (double)extent / 100.0;
   }
   if (v < 0.0) return 0;
   if (extent > 0 && v > (double)(extent - 1)) v = (double)(extent - 1);
   *out = (float)v;
   if (end) *end = stop;
   return 1;
}

/* "x,y" in either spelling, as one pair. */
static int touch_parse_xy(const char *s, float *x, float *y, const char **end)
{
   const char *p = NULL;
   const int w = arm_exec_fb_width(), h = arm_exec_fb_height();
   if (!touch_parse_coord(s, w, x, &p)) return 0;
   while (*p == ' ' || *p == '\t') ++p;
   if (*p != ',') return 0;
   if (!touch_parse_coord(p + 1, h, y, &p)) return 0;
   if (end) *end = p;
   return 1;
}

static void touch_fifo_line(const char *line, int frame_count)
{
   float x = -1, y = -1;
   int hold = 6;
   const char *rest = NULL;
   if (!touch_parse_xy(line, &x, &y, &rest)) return;
   while (*rest == ' ' || *rest == '\t') ++rest;
   if (*rest == ',') hold = atoi(rest + 1);
   if (hold < 1) hold = 1;
   arm_exec_touch_push(0, x, y);
   fprintf(stderr, "[loader] TOUCH_FIFO DOWN (%.0f,%.0f) frame %d hold=%d\n",
           x, y, frame_count, hold);
   if (g_tf_nhold < TF_MAX) {
      g_tf_hold[g_tf_nhold].x = x;
      g_tf_hold[g_tf_nhold].y = y;
      g_tf_hold[g_tf_nhold].up_frame = frame_count + hold;
      ++g_tf_nhold;
   } else {
      arm_exec_touch_push(1, x, y);
   }
}

static void touch_fifo_tick(int frame_count)
{
   /* Finish the taps whose hold has run out, and keep the rest pressed. */
   for (int i = 0; i < g_tf_nhold; ) {
      if (frame_count >= g_tf_hold[i].up_frame) {
         arm_exec_touch_push(1, g_tf_hold[i].x, g_tf_hold[i].y);
         fprintf(stderr, "[loader] TOUCH_FIFO UP (%.0f,%.0f) frame %d\n",
                 g_tf_hold[i].x, g_tf_hold[i].y, frame_count);
         g_tf_hold[i] = g_tf_hold[--g_tf_nhold];
      } else {
         arm_exec_touch_push(2, g_tf_hold[i].x, g_tf_hold[i].y);
         ++i;
      }
   }

   if (g_tf_fd == -2) {
      const char *path = getenv("LUNARIA_TOUCH_FIFO");
      g_tf_fd = -1;
      if (path && *path) {
         g_tf_fd = luna_os_command_pipe_open(path);
         if (g_tf_fd < 0)
            fprintf(stderr, "[loader] TOUCH_FIFO open(%s): %s\n",
                    path, strerror(errno));
         else
            fprintf(stderr, "[loader] TOUCH_FIFO listening on %s "
                    "(echo 'x,y[,hold]' > %s; x and y are percentages of "
                    "the %dx%d framebuffer unless written as '640px')\n",
                    path, path, arm_exec_fb_width(), arm_exec_fb_height());
      }
   }
   if (g_tf_fd < 0) return;

   for (;;) {
      char chunk[128];
      ptrdiff_t n = luna_os_command_pipe_read(g_tf_fd, chunk, sizeof chunk);
      if (n <= 0) break;
      for (ptrdiff_t i = 0; i < n; ++i) {
         if (chunk[i] == '\n' || chunk[i] == ';') {
            g_tf_buf[g_tf_len] = '\0';
            if (g_tf_len) touch_fifo_line(g_tf_buf, frame_count);
            g_tf_len = 0;
         } else if (g_tf_len + 1 < sizeof g_tf_buf) {
            g_tf_buf[g_tf_len++] = chunk[i];
         }
      }
   }
}
#undef TF_MAX

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
   touch_fifo_tick(frame_count);
#define TT_MAX 8
   static float tt_x[TT_MAX], tt_y[TT_MAX];
   static int tt_n = -1, tt_frame = 60, tt_hold = 10, tt_gap = 60;
   if (tt_n < 0) {
      tt_n = 0;
      const char *tt = getenv("LUNARIA_TOUCH_TEST");
      for (const char *p = tt; p && *p && tt_n < TT_MAX; ) {
         float x = -1, y = -1;
         if (touch_parse_xy(p, &x, &y, NULL)) {
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
   static uint64_t last_frames, last_presents, last_composites, last_ticks, last_xlat;
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
   if (!(every > 0.0) || now - last < every) return;

   const uint64_t presents = arm_exec_guest_swap_count();
   const uint64_t composites = luna_comp_frames();
   const uint64_t gticks   = arm_exec_sched_ticks();
   const uint64_t xlat     = arm_exec_translated_insn();
   const double   dt       = now - last;

   fprintf(stderr,
           "[perf] t=%.0fs pump=%llu (%.1f/s) present=%llu (%.1f/s) composite=%llu (%.1f/s) "
           "guest=%.0fM insn (%.1f Mips) xlat=%.1fM (%.1f M/s)\n",
           now - t0,
           (unsigned long long)frames, (double)(frames - last_frames) / dt,
           (unsigned long long)presents, (double)(presents - last_presents) / dt,
           (unsigned long long)composites, (double)(composites - last_composites) / dt,
           (double)gticks / 1e6, (double)(gticks - last_ticks) / dt / 1e6,
           (double)xlat / 1e6, (double)(xlat - last_xlat) / dt / 1e6);

   arm_exec_ticks_report();
   frame_stage_report(dt, frames - last_frames);

   last = now;
   last_frames = frames;
   last_presents = presents;
   last_composites = composites;
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
   if (!(next_s > 0.0)) return;

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
   /* /tmp/lunaria-threads.<pid> names one process when several run; the
    * bare name is whichever process polls it first. */
   char own[64];
   snprintf(own, sizeof own, "/tmp/lunaria-threads.%d", (int)getpid());
   if (access(own, F_OK) == 0) unlink(own);
   else if (access("/tmp/lunaria-threads", F_OK) == 0) unlink("/tmp/lunaria-threads");
   else return;
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

/* A signal handler cannot safely walk the scheduler, allocate C++ containers
 * or print the resulting thread dump.  In particular, firing SIGALRM during
 * UE startup used to stop every guest thread at the 30 second mark while the
 * diagnostic traversed mutable state.  Keep the historical alarm available
 * for explicit debugging, but never inject it into a normal Android run.
 * The normal-runtime diagnostic is /tmp/lunaria-threads, consumed by
 * thread_dump_request_tick() above. */
#ifdef _WIN32
static void *unsafe_alarm_thread(void *value)
{
   uintptr_t seconds = (uintptr_t)value;
   while (seconds--) Sleep(1000);
   svc_dump_handler(0);
   return NULL;
}
#endif
static void schedule_unsafe_alarm_dump(void)
{
   const char *value = getenv("LUNARIA_UNSAFE_ALARM_DUMP_S");
   char *end = NULL;
   long seconds;

   if (!value || !*value) return;
   errno = 0;
   seconds = strtol(value, &end, 10);
   if (errno || end == value || *end || seconds <= 0 || seconds > UINT_MAX) {
      fprintf(stderr, "[loader] ignoring invalid "
              "LUNARIA_UNSAFE_ALARM_DUMP_S=%s\n", value);
      return;
   }
#ifdef _WIN32
   pthread_t worker;
   int error = pthread_create(&worker, NULL, unsafe_alarm_thread, (void *)(uintptr_t)seconds);
   if (error) fprintf(stderr, "[loader] diagnostic timer: %s\n", strerror(error));
   else pthread_detach(worker);
#else
   signal(SIGALRM, svc_dump_handler);
   alarm((unsigned int)seconds);
#endif
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

   if (!unity.native_init_jni.ptr) {
      warnx("not a unity jni lib");
      return EXIT_FAILURE;
   }

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

/* The Android side of one turn of the main loop: what the framework does on
 * the main thread every frame regardless of which engine the app runs.  Every
 * pump loop calls it once per frame, the engine-specific ones included — the
 * Unity loops used to skip it, so Handler/Choreographer callbacks ran only
 * when Unity happened to call into Java, apply()'d preferences never reached
 * the disk (the account a login stored was gone next launch), and typed text
 * waited for the guest to call Java before the input method saw it. */
static void
pump_java_frame(void)
{
   thread_dump_request_tick();
   const uint64_t t_media = frame_now_ns();
   struct dvm *vm = dvm_jni_vm();
   if (vm) {
      /* The main thread's Looper.  A device runs it every turn of the main
       * loop, so a Handler.postDelayed() callback lands near its due time;
       * without this it only ran when the guest itself called into Java, which
       * left armed callbacks seconds late (see dvm_main_looper_tick). */
      dvm_main_looper_tick(vm);
      dvm_media_pump_active(vm);
      dvm_glsurface_tick(vm);
      /* Preferences an apply() left pending: on a device the framework writes
       * them behind the caller's back, and this is that writer. */
      dvm_prefs_flush(vm);
      /* The input method: a game blocked on the text the player is typing
       * calls no Java of its own, so the keystrokes need a way in that does
       * not depend on the guest doing anything. */
      dvm_ime_frame(vm);
   }
   frame_stage_add(FRAME_STAGE_MEDIA, t_media);
}

static void
pump_run_frame(void (*run_threads)(void))
{
   static uint64_t budget_us = 0;
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
   pump_java_frame();
}

/* --- dex startup --------------------------------------------------------
 *
 * An Android process starts at the manifest's Application and launcher
 * Activity and runs their real bytecode; whatever the app needs from the
 * platform (an engine's NativeActivity glue, its window, its lifecycle) is the
 * framework's job and is provided behind the framework classes, not here. */
static int dex_call_lifecycle(struct jvm *jvm, jobject self, const char *cls,
                              const char *method, const char *sig,
                              int takes_null_arg);
static int start_manifest_providers(struct jvm *jvm, jobject context);

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

/* Activity.attach() gives every Activity a base Context before any native
 * code can ask it for a system service.  A bare AllocObject() Activity has
 * none, so getSystemService() failed with "unattached ContextWrapper" and the
 * engine went on to dereference the null result. */
static jobject
alloc_attached_activity(struct jvm *jvm)
{
   jobject activity = jvm->native.AllocObject(&jvm->env,
      jvm->native.FindClass(&jvm->env, "android/app/Activity"));
   jclass context_class = jvm->native.FindClass(&jvm->env,
                                                "android/content/Context");
   jobject base = context_class
      ? jvm->native.AllocObject(&jvm->env, context_class) : NULL;
   jclass ac = jvm->native.FindClass(&jvm->env, "android/app/Activity");
   jmethodID attach = ac ? jvm->native.GetMethodID(&jvm->env, ac,
      "attachBaseContext", "(Landroid/content/Context;)V") : NULL;
   if (activity && base && attach) {
      jvalue arg = { .l = base };
      jvm->native.CallVoidMethodA(&jvm->env, activity, attach, &arg);
   }
   return activity;
}

/* Start the package the way Android does: construct the launcher Activity and
 * run its lifecycle out of the dex.  `ANDROID_LAUNCH_ACTIVITY` comes from
 * the native APK launcher, which already parses the APK's AndroidManifest.
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
run_dex_activity(struct jvm *jvm, int is_a64)
{
   void (*run_threads)(void) = is_a64 ? arm64_exec_run_pending_threads : arm_exec_run_pending_threads;
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

   if (!(is_a64 ? arm64_exec_host_egl_init() : arm_exec_host_egl_init()))
      fprintf(stderr, "[loader] host EGL init failed\n");

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
   run_threads();
   /* onStart/onResume are what make an Activity visible and running; an app
    * that starts its render thread in onResume never starts without them. */
   /* A launcher that called finish() from onCreate (a splash that starts the
    * real activity) goes straight to onDestroy, which the activity stack has
    * already delivered.  Starting, resuming or showing it again would put its
    * window back on top of the activity that replaced it, and that window
    * would take the input focus. */
   const int launcher_finished = dvm_jni_activity_finishing(&jvm->env, activity);
   if (!launcher_finished) {
      dex_call_lifecycle(jvm, activity, cls, "onStart",  "()V", 0);
      dex_call_lifecycle(jvm, activity, cls, "onResume", "()V", 0);
      run_threads();

      /* Attach the resumed Activity to its window before running its renderer.
       * SurfaceView dispatches its lifecycle callbacks through the main Looper;
       * the app's callback owns connecting its native renderer to that Surface. */
      dvm_jni_show_activity(&jvm->env, activity);
   }
   pump_java_frame();

   int max_frames = 0;
   { const char *mf = getenv("LUNARIA_MAX_FRAMES"); if (mf && *mf) max_frames = atoi(mf); }
   fprintf(stderr, "[loader] dex startup: entering pump loop (max_frames=%d)\n",
           max_frames);
   for (int frame = 0; max_frames <= 0 || frame < max_frames; ++frame) {
      if (arm_exec_guest_abort_count() > 0) {
         fprintf(stderr, "[loader] guest abort — stopping dex loop (frame %d)\n", frame);
         break;
      }
      pump_run_frame(run_threads);
      if (is_a64) arm64_exec_egl_swap(); else arm_exec_egl_swap();
      arm_exec_glfw_poll();
      /* LUNARIA_TOUCH_FIFO / LUNARIA_TOUCH_TEST: the other pump loops replay them;
       * this one drives Unity titles whose entry point is a dex Activity. */
      touch_test_tick(frame);
      /* The framework input queue feeds Activity/View dispatch, independently
       * of the engine's Java-owned renderer. NativeActivity retains its NDK
       * input queue; Unity's Java path must not bypass its View callbacks. */
      ArmExecTouchEvent touch;
      struct dvm *input_vm=dvm_jni_vm();
      for (int n=0;input_vm && n<64 && arm_exec_touch_next(&touch);++n)
         (void)dvm_ui_dispatch_touch(input_vm,&touch);
      stall_watch_tick();
      perf_tick();
      if (frame < 5 || frame % 50 == 0)
         fprintf(stderr, "[loader] dex pump frame %d\n", frame);
      if (arm_exec_glfw_should_close()) break;
   }
   return EXIT_SUCCESS;
}

static int
run_jni_game_arm64(struct jvm *jvm)
{
   /* Neither engine's native entry point is exported.  An ordinary Android
    * app has none: its entry point is the launcher Activity, in the dex.  With
    * a bytecode VM that is runnable, so start it the way Android does instead
    * of giving up on the package. */
   if (run_dex_activity(jvm, 1) == EXIT_SUCCESS)
      return EXIT_SUCCESS;
   fprintf(stderr, "[loader] arm64: no known entry point\n");
   return EXIT_FAILURE;
}

static int
run_jni_game_arm(struct jvm *jvm)
{

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

   if (!va_init_jni || !va_render) {
      warnx("not a unity jni lib");
      return EXIT_FAILURE;
   }

   uint32_t env = arm_exec_env_va();
   const jobject context = alloc_attached_activity(jvm);
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
#ifndef _WIN32
   signal(SIGUSR1, svc_dump_handler);
#endif
   schedule_unsafe_alarm_dump();

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
      /* GLFW mouse -> one MotionEvent per queued sample.  Each object carries
       * its own immutable payload (JVM_OBJECT_MOTION); Unity keeps the jobject
       * and reads it again during PlayerLoop.  One inject per frame. */
      ArmExecTouchEvent te;
      if (va_inject && arm_exec_touch_next(&te)) {
         /* Re-clear in case Java/meta-data path set the flag during startup. */
         if (fwd_flag_va) {
            uint32_t word = arm_exec_read32(fwd_flag_va & ~3u);
            unsigned sh = (fwd_flag_va & 3u) * 8u;
            if ((word >> sh) & 0xffu)
               arm_exec_write32(fwd_flag_va & ~3u, word & ~(0xffu << sh));
         }
         if (va_fwd_dalv)
            arm_exec_call(va_fwd_dalv, env, ctx, 0, 0);
         lunaria_touch_event lev = touch_to_lunaria(&te);
         jobject motion_ev = jvm_new_motion_event(jvm, &lev);
         int handled = arm_exec_call(va_inject, env, ctx,
                                     (uint32_t)(uintptr_t)motion_ev, 0);
         static int inj_log = 0;
         if (inj_log < 100) {
            fprintf(stderr, "[loader] injectEvent action=%d x=%.0f y=%.0f -> %d\n",
                    te.action, te.x, te.y, handled);
            ++inj_log;
         }
      }
      /* UnityPlayer GL thread loop: executeGLThreadJobs() then nativeRender().
       * nativeRender MUST run to completion: abandoning it mid-PlayerLoop leaves
       * Unity's reentrancy guard set, making every subsequent frame bail with
       * "PlayerLoop called recursively!".  Use the unlimited variant. */
      arm_exec_drain_gl_thread_jobs();
      int ok = arm_exec_call_unlimited(va_render, env, ctx, 0, 0);
      /* A guest process termination ends its GL thread as well.  Preserve
       * guest memory: nativeRender's return value does not identify a fault
       * and cannot authorize resetting engine state at a fixed address. */
      if (arm_exec_guest_exit_count() > 0 || arm_exec_guest_abort_count() > 0) {
         fprintf(stderr, "[loader] guest process terminated during nativeRender "
                         "(frame %d)\n", frame_count);
         break;
      }
      /* Android では surfaceChanged -> nativeResize がエンジン初期化後にも
       * 届く。ループ前の nativeResize はエンジン未初期化で無視されるため
       * (画面が 128x128 の既定値のままになる)、初回フレーム完了後に再送する。 */
      int resized_w = 0, resized_h = 0;
      if (arm_exec_take_view_resize(&resized_w, &resized_h) && va_resize) {
         arm_exec_call6(va_resize, env, ctx, (uint32_t)resized_w, (uint32_t)resized_h,
                          (uint32_t)resized_w, (uint32_t)resized_h);
         fprintf(stderr, "[loader] nativeResize(%d,%d) — view resized\n",
                 resized_w, resized_h);
      }
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
      pump_java_frame();
      /* Unity はeglSwapBuffersをJava側に任せる場合があるのでここで呼ぶ */
      arm_exec_egl_swap();
      ++frame_count;
      if (ok != last_ok || (frame_count <= 5) || (frame_count % 100 == 0)) {
         fprintf(stderr, "[loader] nativeRender -> %d (frame %d)\n", ok, frame_count);
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

int luna_apk_prepare(int *argc, const char ***argv);

#ifndef _WIN32
/* A heap check that fails inside libc ("realloc(): invalid next size") aborts with
 * one line of text and nothing about who was running.  The thread that detects the
 * corruption is rarely the one that caused it, but its stack still names the
 * allocation that tripped, which is the first thing needed to find the writer. */
static void
host_abort_report(int sig)
{
   void *frames[48];
   int n = luna_os_backtrace(frames, 48);
   fprintf(stderr, "\n[host-abort] signal %d on a host thread; stack:\n", sig);
   luna_os_backtrace_print(frames, n);
   signal(sig, SIG_DFL);
   raise(sig);
}
#endif

#ifdef _WIN32
static int
lunaria_main(int argc, const char *argv[])
#else
int
main(int argc, const char *argv[])
#endif
{
   /* Keep loader milestones in chronological order when stdout and stderr are
    * redirected to one startup log.  Fully buffered stdout otherwise leaves
    * "loading module" and dependency messages at the end of the file. */
   setvbuf(stdout, NULL, _IOLBF, 0);
#ifndef _WIN32
   signal(SIGABRT, host_abort_report);
#endif

   /* Descriptors 0, 1 and 2 are always open in an Android process: zygote
    * hands every app /dev/null on stdin and the logger on stdout/stderr, and
    * nothing the app allocates afterwards can land there.  The guest's file
    * descriptors *are* this process's file descriptors, so a closed stdin here
    * is a free descriptor there, and the first pipe() the guest makes comes
    * back as fd 0.
    *
    * That is not a theoretical tidiness argument.  android_native_app_glue
    * keeps its command pipe in android_app, and the loader finds that pipe by
    * scanning the struct for a pair of descriptors that are both FIFOs; a
    * guest whose msgread is fd 0 was skipped, so APP_CMD_INIT_WINDOW never
    * reached android_main, the engine never learned it had a window, and the
    * title ran its pump loop for ever without presenting a frame.  Anything
    * the guest writes to its own stderr would also have gone into that pipe.
    *
    * So open /dev/null over whatever is missing before any of it exists. */
   for (int fd = 0; fd <= 2; ++fd) {
      if (luna_fd_get_cloexec(fd) != -1 || errno != EBADF)
         continue;
#ifdef _WIN32
      const char *null_path = "NUL";
#else
      const char *null_path = "/dev/null";
#endif
      int nul = luna_file_open(null_path, fd == 0 ? O_RDONLY : O_WRONLY, 0);
      if (nul < 0)
         break;
      if (nul != fd) {
         int copied = luna_fd_dup_to(nul, fd, 0);
         luna_fd_close(nul);
         if (copied < 0) break;
      }
      fprintf(stderr, "[loader] fd %d was closed — opened %s on it "
              "(a guest descriptor must never land on stdin/stdout/stderr)\n",
              fd, null_path);
   }

   int launch = luna_apk_prepare(&argc, &argv);
   if (launch) return launch < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

   printf("loading module: %s\n", argv[1]);

   /* A bare module loaded from the command line — every test under test/ —
    * runs its work on guest pthreads exactly as a game does, so it must be
    * scheduled the same way.  Without this the tests measured the flat-slice
    * path while every real title measured the adaptive one, which made them
    * quietly unrepresentative: test/heap_test.c reported 20,001 instructions
    * a slice, a number no game ever sees.  LUNARIA_THREADS_RUN_ENGINE=0 puts
    * a test back on the flat path to compare the two. */
   {
      const char *e = getenv("LUNARIA_THREADS_RUN_ENGINE");
      arm64_exec_threads_run_engine(!(e && e[0] == '0'));
   }

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
      /* Same platform images a device maps before zygote forks the app. */
      if (is_a64)
         a64_preload_platform_libs();

      int ret = run_dex_activity(&jvm, is_a64);
      arm_exec_begin_shutdown();
      dvm_jni_shutdown();
      arm_exec_shutdown_engines();
      jvm_release(&jvm);
      printf("exiting\n");
      return ret;
   }

   /* ARM64 ELF: use A64 dynarmic emulation path */
   if (arm64_elf_is_arm64(argv[1])) {
      printf("detected ARM64 ELF — using A64 dynarmic emulation\n");
      luna_os_setenv("GC_DONT_GC", "1", 0);
      luna_os_setenv("GC_MAXIMUM_HEAP_SIZE", "268435456", 0);
      luna_os_setenv("GC_INITIAL_HEAP_SIZE", "67108864",  0);
      static struct jvm jvm;
      jvm_init(&jvm);

      if (arm64_exec_context_init(&jvm) < 0)
         errx(EXIT_FAILURE, "arm64_exec_context_init failed");

      /* Pre-load companion libraries from the same directory */
      {
         char dir[4096], libpath[4096];
         char dep_seen[128][LUNA_GUEST_NAME_MAX + 1] = {{0}};
         size_t dep_seen_n = 0;
         struct stat stbuf;
         snprintf(dir, sizeof(dir), "%s", argv[1]);
         char *slash = strrchr(dir, '/');
         if (slash) *(slash + 1) = '\0'; else dir[0] = '\0';

         /* Map Android libc's code image only as a pure-code provider.  A
          * process libc is not an ordinary dlopen dependency: bionic's linker
          * bootstraps its main-thread TLS, auxv, allocator and libc globals as
          * one operation before running application constructors.  Running
          * libc.so's constructors here without that bootstrap produced a
          * half-initialised libc (Cross Worlds reached a NULL JavaVM call in
          * UE's fourth constructor).  arm_exec therefore exposes only the
          * audited stateless routines from this image until that process
          * bootstrap exists.  Shared with the APK-process entry. */
         a64_preload_platform_libs();

         /* Relocate dependencies before their consumer, as the Android
          * dynamic linker does.  Loading libc++ directly used to bind its
          * pthread imports to emulator SVC thunks because the platform libc
          * had not entered the ELF global scope yet.  Later DSOs then bound
          * the same names to bionic, producing two incompatible pthread
          * implementations in one process. */
         snprintf(libpath, sizeof(libpath), "%s%s", dir, "libc++_shared.so");
         if (stat(libpath, &stbuf) == 0 && arm64_elf_is_arm64(libpath)) {
            a64_preload_needed(libpath, dir, dep_seen, &dep_seen_n);
            printf("preloading arm64 libc++_shared after dependencies: %s\n",
                   libpath);
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
      if (jni_ver < 0) warnx("arm64_exec_jni_onload failed");
      int ret = jni_ver < 0 ? EXIT_FAILURE : run_jni_game_arm64(&jvm);
      arm_exec_begin_shutdown();
      dvm_jni_shutdown();
      arm_exec_shutdown_engines();
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
         luna_os_unsetenv("GC_DONT_GC");
      else
         luna_os_setenv("GC_DONT_GC", "1", 0);
      /* Boehm GC computes max_heap_size from the 32-bit address space (~4 GB),
       * producing requests of ~3.7 GB which our mmap bump allocator must reject.
       * With zero heap the GC calls GC_scratch_alloc(0) -> ABORT("Bad GET_MEM arg").
       * Cap the heap to 256 MB so the GC gets usable memory without flooding. */
      luna_os_setenv("GC_MAXIMUM_HEAP_SIZE", "268435456", 0); /* 256 MB */
      luna_os_setenv("GC_INITIAL_HEAP_SIZE", "67108864",  0); /* 64 MB */
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

         /* libc++_shared.so -> libil2cpp.so の順 (IL2CPP ゲーム対応) */
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
      if (jni_ver < 0) warnx("arm_exec_jni_onload failed");
      int ret = jni_ver < 0 ? EXIT_FAILURE : run_jni_game_arm(&jvm);
      arm_exec_begin_shutdown();
      dvm_jni_shutdown();
      arm_exec_shutdown_engines();
      jvm_release(&jvm);
      printf("exiting\n");
      return ret;
   }

   {
      char abs[PATH_MAX], paths[4096];
      if (!luna_file_realpath(argv[1], abs, sizeof abs))
         snprintf(abs, sizeof abs, "%s", argv[1]);
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

      if (fread(elf.bytes, 1, sizeof(elf.bytes), f) != sizeof(elf.bytes)) {
         fclose(f);
         err(EXIT_FAILURE, "fread(%s)", argv[1]);
      }
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
      static struct jvm jvm;
      jvm_init(&jvm);
      entry.JNI_OnLoad.fun(&jvm.vm, NULL);
      ret = run_jni_game(&jvm);
      arm_exec_begin_shutdown();
      dvm_jni_shutdown();
      arm_exec_shutdown_engines();
      jvm_release(&jvm);
   } else {
      warnx("no entrypoint found in %s", argv[1]);
   }

   dvm_jni_report();
   /* The guest is done; stop the engines before anything static goes away. */
   arm_exec_shutdown_engines();
   printf("unloading module: %s\n", argv[1]);
   bionic_dlclose(handle);
   printf("exiting\n");
   return ret;
}

#ifdef _WIN32
/* Decode Windows arguments losslessly, including drag-and-drop Unicode paths. */
int wmain(int argc, wchar_t *wide_argv[])
{
   char **argv = calloc((size_t)argc + 1, sizeof *argv);
   if (!argv) return EXIT_FAILURE;
   for (int i = 0; i < argc; i++) {
      int n = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, NULL, 0, NULL, NULL);
      if (!n || !(argv[i] = malloc((size_t)n)) ||
          !WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, argv[i], n, NULL, NULL)) {
         for (int j = 0; j <= i; j++) free(argv[j]);
         free(argv); return EXIT_FAILURE;
      }
   }
   int result = lunaria_main(argc, (const char **)argv);
   for (int i = 0; i < argc; i++) free(argv[i]);
   free(argv); return result;
}
#endif
