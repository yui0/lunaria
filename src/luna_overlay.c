/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See luna_overlay.h for what this layer is and why it exists.
 *
 * luna-ui owns the window in every other program in this tree; here it must
 * not, because the window, the GL context and the input queue all belong to
 * the emulator.  LUNA_UI_NO_PLATFORM builds the engine without any host, which
 * is the configuration meant for exactly this: the caller keeps the context
 * current and calls luna_init/luna_update/luna_render itself.
 */

#define LUNA_UI_NO_PLATFORM
#include <limits.h>
#define LUNA_UI_IMPLEMENTATION
#include "luna-ui.h"

#include "luna_overlay.h"
#include <sys/stat.h>
#include <stdarg.h>
#include "lunaria_os.h"
#include "dvm/dvm.h"
#include "luna_ime.h"
#include "arm_exec.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool  g_ready;          /* luna_init() has run against a live context */
static bool  g_failed;         /* …and failed; do not retry every frame */
static char *g_html;           /* guest widget document (dialogs, …) */
static char *g_css;
static char *g_status_html;    /* JIT / boot status card */
static char *g_status_css;
static char *g_ime_html;       /* the input method's panel, above both */
static char *g_ime_css;
static char *g_toast_html;     /* a Toast: over the app, under the IME */
static char *g_toast_css;
static char *g_menu_html;      /* the emulator's menu: over everything */
static char *g_menu_css;
static luna_overlay_menu_fn g_menu_fn;
static bool g_menu_modal;
static bool  g_doc_dirty;
static char *g_changed_image_path;
static bool  g_from_files;   /* LUNARIA_UI_TEST names an HTML file */
static int   g_w, g_h;
static double g_last_time;

/* The document is published by whichever guest thread runs the widget layer,
 * and consumed by the guest thread that swaps.  Those are different threads —
 * luna-ui has no locking of its own, so the handoff is a copied string under a
 * mutex and every luna_* call stays on the presenting thread. */
static pthread_mutex_t g_doc_lock = PTHREAD_MUTEX_INITIALIZER;

void luna_overlay_image_changed(const char *path)
{
   if (!path || !*path) return;
   const size_t length = strlen(path) + 1;
   char *copy = malloc(length);
   if (!copy) return;
   memcpy(copy, path, length);
   pthread_mutex_lock(&g_doc_lock);
   free(g_changed_image_path);
   g_changed_image_path = copy;
   pthread_mutex_unlock(&g_doc_lock);
   luna_overlay_wake();
}

/* Pointer events arrive on the host's event thread (GLFW polls there) and
 * cannot touch luna-ui's hit-testing state from outside the presenting
 * thread, so they queue here and are replayed at the top of present(). */
struct overlay_pointer_event { double x, y; int action; };
static struct overlay_pointer_event g_ptr_queue[64];
static int g_ptr_count;
static pthread_mutex_t g_ptr_lock = PTHREAD_MUTEX_INITIALIZER;

static luna_overlay_click_fn g_click_fn;
static luna_overlay_web_pointer_fn g_web_pointer_fn;
static luna_overlay_web_char_fn g_web_char_fn;
static luna_overlay_web_key_fn g_web_key_fn;
static char g_web_pointer_capture[32];
static char g_web_focus[32];
static luna_overlay_frame_fn g_frame_fn;

/* Hosted: the overlay is drawn by the window's compositor (luna_compositor.c)
 * into the context current on its own thread.  Nothing is borrowed, so there
 * is no context to switch to and no guest binding to hand back.  wake() tells
 * the compositor that something it draws changed. */
static atomic_bool g_hosted;
static pthread_t   g_host_thread;
static atomic_bool g_host_thread_set;
static void (*g_wake_fn)(void);

/* Hosted mode is declared before the compositor's thread exists, so that no
 * other thread can meanwhile bring luna-ui up in a context of its own: the
 * GL objects it would create there are not names in the compositor's
 * context, and drawing with them there drew one texture in place of another
 * over half the screen (Cross Worlds' notice dialog). */
void luna_overlay_set_hosted(bool hosted) { atomic_store(&g_hosted, hosted); }

/* The compositor's thread, from its first frame on: the only thread that may
 * run the overlay once it is hosted. */
void luna_overlay_bind_host_thread(void)
{
   g_host_thread = pthread_self();
   atomic_store(&g_host_thread_set, true);
}
void luna_overlay_set_wake(void (*fn)(void)) { g_wake_fn = fn; }
void luna_overlay_wake(void) { if (g_wake_fn) g_wake_fn(); }

/* luna-ui's input path asks the app runner to schedule a repaint.  With
 * LUNA_UI_NO_PLATFORM there is no runner, and the symbol is the embedder's to
 * supply: here the overlay is redrawn on the guest's own frame cadence, so
 * there is nothing to schedule. */
void luna_app_request_redraw(void) { }

/* eglGetProcAddress is only required to resolve *extension* entry points; for
 * core GL/GLES functions it may legally return NULL, and on some drivers it
 * does.  luna-ui keeps every entry point it loads in a pointer it null-checks
 * before calling, so a NULL there is silent: glActiveTexture_ was never
 * called, the glyph atlas was bound to whatever texture unit the guest had
 * left active, and the text shader — sampling unit 0 — drew nothing.  Every
 * untextured box still painted, which is why the UI looked complete apart from
 * having no text at all.  The emulator links libGLESv2, so the core names are
 * in the process and dlsym finds them. */
static void *overlay_get_proc(const char *name)
{
   void *p = (void *)eglGetProcAddress(name);
   if (!p) p = dlsym(RTLD_DEFAULT, name);
   if (getenv("LUNARIA_UI_TRACE_GLPROC")) {
      static void *libgles, *libgl;
      static bool opened;
      if (!opened) {
         opened = true;
         libgles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_NOLOAD);
         libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
      }
      void *es = libgles ? dlsym(libgles, name) : NULL;
      void *gl = libgl ? dlsym(libgl, name) : NULL;
      static int n;
      if (n++ < 24)
         fprintf(stderr, "[overlay] proc %-28s egl=%p default=%p "
                 "gles=%p gl=%p%s\n", name, (void *)eglGetProcAddress(name),
                 dlsym(RTLD_DEFAULT, name), es, gl,
                 (es && p != es) ? "  <== NOT the GLES symbol" : "");
   }
   return p;
}

/* Seconds since the overlay came up.  Zero-based rather than raw monotonic: a
 * CSS animation's timeline starts when the document appears, and a float
 * carrying the host's uptime has no precision left to resolve a frame. */
/* Where a present frame's wall clock goes.
 *
 * The emulator UI is composited inside the guest's own swap, on the host
 * thread that also runs the guest's GL work, so every microsecond spent here
 * is a microsecond the guest does not run.  "The overlay is expensive" is not
 * a statement anything can be done with; which of the four stages it is, is.
 * LUNARIA_OVERLAY_PROF=1 reports them. */
static uint64_t overlay_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool overlay_prof_on(void)
{
   static int on = -1;
   if (on < 0) on = getenv("LUNARIA_OVERLAY_PROF") != NULL;
   return on != 0;
}

static struct {
   uint64_t frames, bind_ns, parse_ns, render_ns, ime_ns, restore_ns, total_ns;
} g_prof;

static void overlay_prof_report(void)
{
   if (!g_prof.frames || (g_prof.frames % 300) != 0) return;
   const double f = (double)g_prof.frames;
   fprintf(stderr, "[overlay-prof] %llu presents, %.0f us each: "
           "bind=%.0f parse=%.0f render=%.0f ime=%.0f restore=%.0f\n",
           (unsigned long long)g_prof.frames, (double)g_prof.total_ns / f / 1e3,
           (double)g_prof.bind_ns / f / 1e3, (double)g_prof.parse_ns / f / 1e3,
           (double)g_prof.render_ns / f / 1e3, (double)g_prof.ime_ns / f / 1e3,
           (double)g_prof.restore_ns / f / 1e3);
}

static double overlay_now(void)
{
   static double epoch;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   double t = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
   if (epoch == 0.0) epoch = t;
   return t - epoch;
}

/* LUNARIA_BOOT_DUMP=/path — write a PPM after each boot-card frame (QA only). */
static void overlay_maybe_dump_boot(int w, int h, bool status_only)
{
   static int frame_no;
   const char *dir = getenv("LUNARIA_BOOT_DUMP");
   if (!dir || !*dir || !status_only || w <= 0 || h <= 0) return;
   unsigned char *px = malloc((size_t)w * (size_t)h * 4u);
   if (!px) return;
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   char path[512];
   snprintf(path, sizeof path, "%s/boot_%04d.ppm", dir, frame_no++);
   FILE *f = fopen(path, "wb");
   if (f) {
      fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int y = h - 1; y >= 0; --y)
         for (int x = 0; x < w; ++x) {
            unsigned char *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            fwrite(p, 1, 3, f);
         }
      fclose(f);
   }
   free(px);
}

/* The overlay used to share the guest's context and put its state back
 * afterwards, one binding at a time.  That approach cannot be finished: a
 * modern engine leaves far more set than is practical to enumerate, and the
 * failure it produced was silent and selective — every untextured box painted
 * correctly while the text pass drew nothing, with viewport, program, uniforms,
 * vertex buffer contents and attribute bindings all querying as correct.  The
 * overlay now runs in its own context (see overlay_context_create), so there is
 * no shared state to restore.
 */

/* Errors raised by the overlay's own draws.  They are drained either way, so
 * a mistake here can never surface later as a failure inside the guest. */
static void overlay_report_gl_errors(const char *where)
{
   static int reported;
   GLenum e;
   while ((e = glGetError()) != GL_NO_ERROR) {
      if (reported < 16) {
         fprintf(stderr, "[overlay] GL error 0x%04x at %s\n", (unsigned)e, where);
         ++reported;
      }
   }
}

/* The overlay's own context — not shared with the guest.
 *
 * Sharing put luna-ui's textures and programs into the guest's share group and
 * corrupted guest state in ways that only showed up as missing text.  A second
 * unshared ES2 context keeps the emulator UI completely separate; the same
 * window surface is bound briefly to draw, then handed back. */
static EGLDisplay g_ov_dpy = EGL_NO_DISPLAY;
static EGLContext g_ov_ctx = EGL_NO_CONTEXT;

/* Finds the EGLConfig a context was created with.  eglCreateContext needs one,
 * and the only handle onto it is the config id the context remembers. */
static bool overlay_config_by_id(EGLDisplay dpy, EGLint id, EGLConfig *out)
{
   const EGLint attrs[] = { EGL_CONFIG_ID, id, EGL_NONE };
   EGLint n = 0;
   return eglChooseConfig(dpy, attrs, out, 1, &n) && n == 1;
}

static bool overlay_config_of(EGLDisplay dpy, EGLContext ctx, EGLConfig *out)
{
   EGLint id = 0;
   if (!eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &id)) return false;
   return overlay_config_by_id(dpy, id, out);
}

/* The config of the surface this overlay will actually be made current on.
 *
 * It used to take the config of whatever context happened to be current when
 * the overlay context was first created, which is not the same question: the
 * emulator has several contexts (the guest's, the bootstrap host one, the
 * per-engine ones) and they are not all built for the config the window
 * surface was created with.  When they differ, eglMakeCurrent answers
 * EGL_BAD_MATCH, and the recovery path destroys the context, recreates it and
 * marks the document dirty — a full reparse, and with it the 0x0502 that
 * follows a luna-ui render against names the destroyed context owned.
 *
 * Ask the surface. */
static bool overlay_config_of_surface(EGLDisplay dpy, EGLSurface surf,
                                      EGLConfig *out)
{
   EGLint id = 0;
   if (surf == EGL_NO_SURFACE) return false;
   if (!eglQuerySurface(dpy, surf, EGL_CONFIG_ID, &id)) return false;
   return overlay_config_by_id(dpy, id, out);
}

static bool overlay_context_create(void)
{
   EGLDisplay dpy = eglGetCurrentDisplay();
   EGLContext current = eglGetCurrentContext();
   EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
   if (dpy == EGL_NO_DISPLAY) return false;
   EGLConfig cfg;
   /* The surface first: that is what this context gets made current on, and
    * a context built for a different config cannot be. */
   if (overlay_config_of_surface(dpy, draw, &cfg)) {
      /* got it */
   } else if (current != EGL_NO_CONTEXT) {
      if (!overlay_config_of(dpy, current, &cfg)) return false;
   } else {
      const EGLint cfg_attrs[] = {
         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
         EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
         EGL_NONE
      };
      EGLint n = 0;
      if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1)
         return false;
   }
   /* Match the guest's GLES version when we can.  Preferring ES2 left the
    * overlay on a context that cannot run the ES3 entry points luna-ui and
    * the present path use (separate draw/read framebuffer binds, etc.), and
    * every rebind then logged GL_INVALID_OPERATION (0x0502) on render. */
   EGLint version = 3;
   if (current != EGL_NO_CONTEXT)
      (void)eglQueryContext(dpy, current, EGL_CONTEXT_CLIENT_VERSION, &version);
   if (version < 2) version = 2;
   const EGLint attrs[] = { EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, attrs);
   if (ctx == EGL_NO_CONTEXT && version != 2) {
      const EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
      ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, es2);
   }
   if (ctx == EGL_NO_CONTEXT) return false;
   g_ov_dpy = dpy;
   g_ov_ctx = ctx;
   return true;
}

/* The window surface's EGLConfig can change when the guest recreates its
 * context (boot card → UE GL).  An overlay context built against the old
 * config then fails eglMakeCurrent with EGL_BAD_MATCH (0x3009), so the dialog
 * never paints and queued clicks never drain — an invisible modal.  Rebuild
 * against the context that currently owns the surface. */
static bool overlay_context_rebind(void)
{
   EGLDisplay dpy = eglGetCurrentDisplay();
   if (dpy == EGL_NO_DISPLAY) return false;
   /* Do not luna_shutdown() here: the overlay context is not current (that is
    * why we are rebinding), and deleting its names against the guest's
    * context would free the wrong objects.  Destroying the context drops its
    * share-group resources with it. */
   if (g_ov_ctx != EGL_NO_CONTEXT) {
      eglDestroyContext(dpy, g_ov_ctx);
      g_ov_ctx = EGL_NO_CONTEXT;
   }
   g_ready = false;
   g_w = g_h = 0;
   if (!overlay_context_create()) return false;
   pthread_mutex_lock(&g_doc_lock);
   g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
   fprintf(stderr, "[overlay] rebuilt context for current surface config\n");
   return true;
}

/* The faces the overlay draws with.
 *
 * Left to itself luna-ui scans /usr/share/fonts and scores what it finds; on a
 * host whose only sans family is Liberation that scan comes back with
 * LiberationSans-Italic, and the whole emulator UI — dialogs and boot card
 * alike — renders in italic.  An oblique face is never the right answer for a
 * UI that did not ask for one, so name the faces we want and let the scan
 * answer only when none of them is installed. */
static unsigned char *overlay_read_file(const char *path, size_t *out_size)
{
   FILE *f = fopen(path, "rb");
   if (!f) return NULL;
   if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
   long n = ftell(f);
   if (n <= 0) { fclose(f); return NULL; }
   rewind(f);
   unsigned char *buf = malloc((size_t)n);
   if (!buf) { fclose(f); return NULL; }
   size_t got = fread(buf, 1, (size_t)n, f);
   fclose(f);
   if (got != (size_t)n) { free(buf); return NULL; }
   if (out_size) *out_size = got;
   return buf;
}

static unsigned char *overlay_load_font(int role, size_t *out_size)
{
   static const char *const regular[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      NULL
   };
   static const char *const bold[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      NULL
   };
   static const char *const mono[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      NULL
   };
   const char *const *list = NULL;
   switch (role) {
   case LUNA_FONT_REGULAR: list = regular; break;
   case LUNA_FONT_BOLD:    list = bold;    break;
   case LUNA_FONT_MONO:    list = mono;    break;
   /* CJK, symbols and brands have no sensible fixed path: let the scan answer
    * those, and the engine falls back cleanly when it finds none. */
   default: return NULL;
   }
   for (int i = 0; list[i]; ++i) {
      unsigned char *buf = overlay_read_file(list[i], out_size);
      if (buf) return buf;
   }
   return NULL;
}

/* luna-ui's own clock.
 *
 * With no platform the engine falls back to clock() — process CPU time — and
 * stamps every animation's start with it, while luna_update() is handed the
 * clock the embedder uses.  Those two disagreeing by the host's uptime made
 * every finite @keyframes animation read as already finished on its first
 * frame, and left the infinite ones at an arbitrary phase.  Supplying get_time
 * is what makes both ends of the comparison the same clock. */
static double overlay_platform_time(void) { return overlay_now(); }

/* The clipboard luna-ui edits against.
 *
 * luna-ui already implements Ctrl+C / Ctrl+X / Ctrl+V on a focused <input>;
 * what it needs is somewhere to put the text.  With no platform installed
 * those two hooks were NULL, so the shortcuts ran and did nothing — the
 * emulator's input method could be typed into but never pasted into, which is
 * exactly the case a coupon code or an account name is copied for.  Both ends
 * are the host's own clipboard, the same one the guest's ClipboardManager
 * uses, because a device has one clipboard. */
static void overlay_clipboard_set(const char *utf8)
{
   arm_exec_clipboard_set(utf8 ? utf8 : "");
}

/* luna-ui frees the result with free(), which is what arm_exec_clipboard_get
 * allocates with. */
static char *overlay_clipboard_get(void) { return arm_exec_clipboard_get(); }

static void overlay_set_platform(void)
{
   LunaPlatform platform;
   memset(&platform, 0, sizeof platform);
   platform.get_time = overlay_platform_time;
   platform.get_proc = overlay_get_proc;
   platform.load_font = overlay_load_font;
   platform.set_clipboard = overlay_clipboard_set;
   platform.get_clipboard = overlay_clipboard_get;
   platform.struct_size = (uint32_t)sizeof platform;
   luna_set_platform(&platform);
}

static bool overlay_start(int w, int h)
{
   if (g_ready) return true;
   if (g_failed) return false;

   overlay_set_platform();

   /* The engine's shader set differs between desktop GL and GLES; ask the
    * context which one this is rather than assuming.  Lunaria runs on both:
    * a desktop driver through GLFW, and GLES on the console session. */
   const char *ver = (const char *)glGetString(GL_VERSION);
   bool es = ver && strstr(ver, "OpenGL ES") != NULL;
   luna_set_gles3(es ? 1 : 0);

   LunaInitConfig cfg = {
      .width = (float)w,
      .height = (float)h,
      .get_proc = overlay_get_proc,
      .frameless = 1,
   };
   if (!luna_init(&cfg)) {
      fprintf(stderr, "[overlay] luna_init failed — no emulator UI this run\n");
      g_failed = true;
      return false;
   }
   g_ready = true;
   g_w = w;
   g_h = h;
   g_last_time = overlay_now();
   fprintf(stderr, "[overlay] luna-ui up at %dx%d (%s, gles3=%d)\n", w, h,
           ver ? ver : "?", es ? 1 : 0);
   overlay_report_gl_errors("init");
   return true;
}

/* Which screen this present is drawing.
 *
 * The emulator's boot card and the application's frame are not two layers of
 * one picture: the card is what is on the display *instead of* the game,
 * while the game has nothing to show.  Compositing them together is what made
 * the card flicker against the first frames of the title — each of the two
 * producers swapped its own idea of the buffer, and the guest's document and
 * the card's animation were also being parsed into one luna-ui tree, so a
 * change to either restarted the other.
 *
 * So: one screen at a time, chosen here, and each present says which one it
 * is.  The card's own presenter asks for OV_BOOT; everything that composites
 * over a guest frame asks for OV_APP and is skipped while the card is up. */
enum { OV_BOOT, OV_APP };
static int g_screen = OV_APP;

/* The boot card is published.  Deliberately *not* "and no guest window is
 * up": if the two were decided by different rules the screen could alternate
 * between them from frame to frame, and each flip is a reparse.  While the
 * card is published it is the screen; a guest window that arrives meanwhile
 * is drawn once the card comes down. */
static bool overlay_boot_up(void)
{
   pthread_mutex_lock(&g_doc_lock);
   const bool up = g_status_html != NULL;
   pthread_mutex_unlock(&g_doc_lock);
   return up && !g_failed;
}

/* True when the two strings say the same thing, NULL included. */
static bool overlay_same(const char *a, const char *b)
{
   if (a == b) return true;
   if (!a || !b) return false;
   return strcmp(a, b) == 0;
}

void luna_overlay_set_document(const char *html, const char *css)
{
   pthread_mutex_lock(&g_doc_lock);
   /* Re-emitting the same document is not a change to it.  The Android view
    * layer republishes on every invalidate, and a reparse throws away the
    * live tree: ids, click wiring, @keyframes timelines, the caret and the
    * focus of whatever the user is typing into.  Compare first — a few KB of
    * strcmp against a full parse and a re-layout. */
   if (overlay_same(html, g_html) && (!css || overlay_same(css, g_css))) {
      pthread_mutex_unlock(&g_doc_lock);
      return;
   }
   free(g_html);
   g_html = html ? strdup(html) : NULL;
   if (css) {
      free(g_css);
      g_css = strdup(css);
   }
   g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
   luna_overlay_wake();
}

void luna_overlay_set_status(const char *html, const char *css)
{
   pthread_mutex_lock(&g_doc_lock);
   free(g_status_html);
   g_status_html = html ? strdup(html) : NULL;
   if (css) {
      free(g_status_css);
      g_status_css = strdup(css);
   } else if (!html) {
      free(g_status_css);
      g_status_css = NULL;
   }
   /* Only dirty the parsed document when the card is the screen on display;
    * the application screen does not contain it. */
   if (g_screen == OV_BOOT) g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
   luna_overlay_wake();
}

void luna_overlay_set_ime(const char *html, const char *css)
{
   pthread_mutex_lock(&g_doc_lock);
   free(g_ime_html);
   g_ime_html = html ? strdup(html) : NULL;
   if (css) {
      free(g_ime_css);
      g_ime_css = strdup(css);
   } else if (!html) {
      free(g_ime_css);
      g_ime_css = NULL;
   }
   /* Raising or dropping the panel changes the application screen's markup,
    * so that screen is reparsed; the boot screen never contains it. */
   if (g_screen == OV_APP) g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
   luna_overlay_wake();
}

void luna_overlay_set_toast(const char *html, const char *css)
{
   pthread_mutex_lock(&g_doc_lock);
   free(g_toast_html);
   g_toast_html = html ? strdup(html) : NULL;
   free(g_toast_css);
   g_toast_css = html && css ? strdup(css) : NULL;
   if (g_screen == OV_APP) g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
   luna_overlay_wake();
}

void luna_overlay_set_menu(const char *html, const char *css, bool modal)
{
   pthread_mutex_lock(&g_doc_lock);
   g_menu_modal = html && modal;
   free(g_menu_html);
   g_menu_html = html ? strdup(html) : NULL;
   free(g_menu_css);
   g_menu_css = html && css ? strdup(css) : NULL;
   g_doc_dirty = true;             /* on both screens: the menu is over each */
   pthread_mutex_unlock(&g_doc_lock);
   luna_overlay_wake();
}

bool luna_overlay_menu_showing(void)
{
   pthread_mutex_lock(&g_doc_lock);
   const bool up = g_menu_html != NULL;
   pthread_mutex_unlock(&g_doc_lock);
   return up && !g_failed;
}

void luna_overlay_set_menu_handler(luna_overlay_menu_fn fn) { g_menu_fn = fn; }

bool luna_overlay_status_showing(void)
{
   pthread_mutex_lock(&g_doc_lock);
   bool showing = g_html == NULL && g_status_html != NULL;
   pthread_mutex_unlock(&g_doc_lock);
   return showing && !g_failed;
}

void luna_overlay_set_click_handler(luna_overlay_click_fn fn)
{
   g_click_fn = fn;
}

void luna_overlay_set_web_pointer_handler(luna_overlay_web_pointer_fn fn)
{
   g_web_pointer_fn = fn;
}

void luna_overlay_set_web_text_handlers(luna_overlay_web_char_fn char_fn,
                                        luna_overlay_web_key_fn key_fn)
{
   g_web_char_fn = char_fn;
   g_web_key_fn = key_fn;
}

bool luna_overlay_web_char(uint32_t codepoint)
{
   char id[sizeof g_web_focus];
   pthread_mutex_lock(&g_ptr_lock);
   memcpy(id, g_web_focus, sizeof id);
   pthread_mutex_unlock(&g_ptr_lock);
   if (!id[0] || !g_web_char_fn) return false;
   g_web_char_fn(id, codepoint);
   return true;
}

bool luna_overlay_web_key(int key, int action)
{
   char id[sizeof g_web_focus];
   pthread_mutex_lock(&g_ptr_lock);
   memcpy(id, g_web_focus, sizeof id);
   pthread_mutex_unlock(&g_ptr_lock);
   if (!id[0] || !g_web_key_fn) return false;
   g_web_key_fn(id, key, action);
   return true;
}

void luna_overlay_set_frame_handler(luna_overlay_frame_fn fn)
{
   g_frame_fn = fn;
}

/* luna-ui hands the clicked element back; its DOM id is the only thing the
 * widget layer needs to identify the guest View it stands for. */
static void overlay_element_clicked(LunaElement *e)
{
   if (!e || !e->id[0]) return;
   if (!strncmp(e->id, "luna-menu", 9)) {
      if (g_menu_fn) g_menu_fn(e->id);
      return;
   }
   if (luna_ime_click(e->id)) return;
   if (g_click_fn) g_click_fn(e->id);
}

/* Every element the widget layer named is clickable from luna-ui's side; the
 * widget layer decides whether the View it stands for has a listener. */
static void overlay_wire_clicks(void)
{
   int n = luna_element_count();
   for (int i = 0; i < n; ++i) {
      LunaElement *e = luna_element_at(i);
      if (e && e->id[0]) luna_set_on_click(i, overlay_element_clicked);
   }
}

/* Replays the queued pointer events on the presenting thread. */
static void overlay_drain_pointer(void)
{
   struct overlay_pointer_event batch[64];
   int n;
   pthread_mutex_lock(&g_ptr_lock);
   n = g_ptr_count;
   memcpy(batch, g_ptr_queue, (size_t)n * sizeof batch[0]);
   g_ptr_count = 0;
   pthread_mutex_unlock(&g_ptr_lock);
   for (int i = 0; i < n; ++i) {
      if (g_web_pointer_fn) {
         int hit = g_web_pointer_capture[0]
            ? luna_get_element_by_id(g_web_pointer_capture)
            : luna_element_at_point(batch[i].x, batch[i].y);
         bool web_hit = false;
         for (int parent = hit; parent >= 0;
              parent = luna_element_parent(parent)) {
            LunaElement *e = luna_element_at(parent);
            if (e && e->id[0] == 'v' &&
                strstr(e->class_name, "WebView")) {
               web_hit = true;
               if (batch[i].action == 1)
                  snprintf(g_web_pointer_capture,
                           sizeof g_web_pointer_capture, "%s", e->id);
               if (batch[i].action == 1) {
                  pthread_mutex_lock(&g_ptr_lock);
                  snprintf(g_web_focus, sizeof g_web_focus, "%s", e->id);
                  pthread_mutex_unlock(&g_ptr_lock);
               }
               if (g_web_pointer_capture[0])
                  g_web_pointer_fn(e->id,
                     (int)(batch[i].x - e->x), (int)(batch[i].y - e->y),
                     batch[i].action);
               break;
            }
         }
         if (batch[i].action == 1 && !web_hit) {
            pthread_mutex_lock(&g_ptr_lock);
            g_web_focus[0] = 0;
            pthread_mutex_unlock(&g_ptr_lock);
         }
         if (batch[i].action == 0) g_web_pointer_capture[0] = 0;
      }
      if (batch[i].action < 0) {
         luna_mouse_move(batch[i].x, batch[i].y);
      } else {
         /* A press has to move the pointer first: luna-ui hit-tests against
          * the last known position, and a touchscreen never reported one. */
         luna_mouse_move(batch[i].x, batch[i].y);
         luna_mouse_button(LUNA_MOUSE_BUTTON_LEFT,
                           batch[i].action ? LUNA_PRESS : LUNA_RELEASE, 0,
                           batch[i].x, batch[i].y);
      }
   }
}

/* LUNARIA_UI_TEST=1 puts a card on screen with no guest involvement.  The
 * overlay shares the guest's context, so "does the emulator's UI reach the
 * screen at all, and does the game still render afterwards" is a question
 * worth being able to ask on its own, before any widget code exists. */
static void overlay_maybe_test_card(void)
{
   static bool checked;
   if (checked) return;
   checked = true;
   const char *e = getenv("LUNARIA_UI_TEST");
   if (!e || !*e || !strcmp(e, "0")) return;
   /* A path names a document to load instead of the built-in card, so a known
    * good page can be put through this exact embedding. */
   if (strchr(e, '/')) {
      g_from_files = true;
      luna_overlay_set_document("", NULL);
      return;
   }
   luna_overlay_set_document(
      "<body><div class=\"card\">"
      "<div class=\"title\">Lunaria UI</div>"
      "<div class=\"body\">overlay compositing over the guest frame</div>"
      "</div></body>",
      "body{margin:0;background:transparent;}"
      ".card{position:absolute;left:40px;top:40px;width:380px;"
      "padding:20px;border-radius:14px;background:rgba(16,22,38,0.92);"
      "color:#e8eefc;font-size:18px;}"
      ".title{font-size:22px;padding-bottom:8px;}"
      ".body{font-size:15px;color:#9fb2d8;}");
}

bool luna_overlay_guest_window_up(void)
{
   pthread_mutex_lock(&g_doc_lock);
   bool up = g_html != NULL;
   pthread_mutex_unlock(&g_doc_lock);
   return up && !g_failed;
}

bool luna_overlay_active(void)
{
   overlay_maybe_test_card();
   pthread_mutex_lock(&g_doc_lock);
   bool up = g_html != NULL || g_status_html != NULL || g_ime_html != NULL ||
             g_toast_html != NULL || g_menu_html != NULL;
   pthread_mutex_unlock(&g_doc_lock);
   return up && !g_failed;
}

/* luna-ui keeps its element tree, its layout and its GL objects in plain
 * globals and has no locking of its own, and this overlay is entered from
 * more than one host thread:
 *
 *   - the pump's arm_exec_egl_swap(),
 *   - the guest's own eglSwapBuffers(), on the thread guest GL is pinned to,
 *   - ANativeWindow_unlockAndPost(), from inside that SVC,
 *   - and, during a cold start, whichever dynarmic engine thread is
 *     translating, through the JIT progress hook and the boot card.
 *
 * The boot card's own pump is re-entrancy-guarded, but that guard is not
 * shared with the guest's swap, so a translating engine and the guest's
 * presenting thread could be inside luna_update()/luna_render() at the same
 * time.  The observed shape is a storm of GL_INVALID_OPERATION out of render
 * — after which the emulator's UI and the guest's GL state are both wrong and
 * the guest stops making progress at all (measured: 0.2 Mips, flat).
 *
 * So: one thread inside the emulator's UI at a time, and never a wait.  A
 * frame that arrives while another thread is compositing skips its own
 * composite instead of blocking — the guest's swap must not queue behind a
 * JIT translation thread, and the emulator's UI is a frame late at worst. */
static pthread_mutex_t g_present_lock = PTHREAD_MUTEX_INITIALIZER;

static void overlay_present_locked(int w, int h);

static void overlay_present_screen(int w, int h, int screen)
{
   if (!luna_overlay_active() || w <= 0 || h <= 0) return;
   /* The two screens never share a frame.  While the card is up it is the
    * only thing drawn; a guest swap that arrives meanwhile is presented as
    * the guest drew it, with nothing of the emulator's on top. */
   if (screen == OV_APP && overlay_boot_up()) return;
   /* The emulator's UI has one owner: with a compositor, its thread. */
   if (atomic_load(&g_hosted) &&
       (!atomic_load(&g_host_thread_set) ||
        !pthread_equal(pthread_self(), g_host_thread)))
      return;
   if (screen != g_screen) {
      pthread_mutex_lock(&g_doc_lock);
      g_screen = screen;
      g_doc_dirty = true;   /* a different screen is a different document */
      pthread_mutex_unlock(&g_doc_lock);
   }
   if (pthread_mutex_trylock(&g_present_lock) != 0) {
      static unsigned long skipped;
      if ((skipped++ % 256) == 0)
         fprintf(stderr, "[overlay] another thread is compositing — skipping "
                 "this one (%lu)\n", skipped);
      return;
   }
   overlay_present_locked(w, h);
   pthread_mutex_unlock(&g_present_lock);
}

void luna_overlay_present_boot(int w, int h)
{
   overlay_present_screen(w, h, OV_BOOT);
}

void luna_overlay_present(int w, int h)
{
   overlay_present_screen(w, h, OV_APP);
}

static void overlay_present_locked(int w, int h)
{
   const bool prof = overlay_prof_on();
   const uint64_t t_enter = prof ? overlay_ns() : 0;
   uint64_t t_mark = t_enter;

   EGLDisplay dpy = eglGetCurrentDisplay();
   EGLContext prev_ctx = eglGetCurrentContext();
   EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
   EGLSurface read = eglGetCurrentSurface(EGL_READ);
   if (dpy == EGL_NO_DISPLAY || draw == EGL_NO_SURFACE) return;

   if (g_hosted) goto bound;
   if (g_ov_ctx == EGL_NO_CONTEXT && !g_failed && !overlay_context_create()) {
      fprintf(stderr, "[overlay] no context — emulator UI disabled\n");
      g_failed = true;
      return;
   }
   if (!eglMakeCurrent(dpy, draw, read, g_ov_ctx)) {
      EGLint err = eglGetError();
      /* EGL_BAD_MATCH (0x3009): overlay context was built for a different
       * config than this surface — recreate once and retry. */
      if (err == 0x3009 && overlay_context_rebind() &&
          eglMakeCurrent(dpy, draw, read, g_ov_ctx)) {
         /* ok after rebind */
      } else {
         static unsigned long skipped;
         if ((skipped++ % 120) == 0)
            fprintf(stderr, "[overlay] makeCurrent failed (0x%04x) — skip present "
                    "(%lu)\n", (unsigned)err, skipped);
         return;
      }
   }
   /* Previous binding is gone until we restore below. */
   arm_exec_egl_invalidate_current();
   /* The guest may have left a GL error pending on this share-group / the
    * previous context.  Drain it so a later overlay_report_gl_errors does not
    * blame luna-ui for a guest mistake, and so luna-ui does not start with a
    * sticky INVALID_OPERATION. */
bound:
   while (glGetError() != GL_NO_ERROR) { }
   if (prof) { const uint64_t n = overlay_ns();
               g_prof.bind_ns += n - t_mark; t_mark = n; }

   if (overlay_start(w, h)) {
      luna_invalidate_gl_state();
      if (w != g_w || h != g_h) {
         g_w = w;
         g_h = h;
         luna_resize((float)w, (float)h);
      }
      pthread_mutex_lock(&g_doc_lock);
      /* The boot screen is the card and nothing else; the application screen
       * is the guest's own widgets with the input method over them.  Which
       * one this is was decided in overlay_present_screen(). */
      const bool status_only = g_screen == OV_BOOT && g_status_html != NULL;
      const char *html = status_only ? g_status_html : g_html;
      const char *css  = status_only ? g_status_css  : g_css;
      bool reparsed = g_doc_dirty;
      if (g_doc_dirty) {
         g_doc_dirty = false;
         /* parse_html() appends, so the previous document has to be dropped
          * first or every republish stacks another copy of the page behind the
          * one on screen — with its ids, its click wiring and its animations
          * still live. */
         luna_reset_document();
         /* Styles are resolved while the HTML is parsed, so the sheet has to
          * be in place first. */
         luna_reset_css();
         if (g_from_files && g_html) {
            const char *path = getenv("LUNARIA_UI_TEST");
            char css_path[1024], base[1024];
            snprintf(base, sizeof base, "%s", path);
            char *slash = strrchr(base, '/');
            if (slash) *slash = '\0';
            luna_set_html_base_dir(base);
            snprintf(css_path, sizeof css_path, "%s", path);
            char *dot = strrchr(css_path, '.');
            if (dot) snprintf(dot, sizeof css_path - (size_t)(dot - css_path),
                              ".css");
            if (!luna_load_css_file(css_path))
               fprintf(stderr, "[overlay] no css at %s\n", css_path);
            if (!luna_load_html_file(path))
               fprintf(stderr, "[overlay] failed to load %s\n", path);
         } else if (html) {
            if (css) luna_parse_css(css);
            luna_parse_html(html);
         }
         /* The input method is a window of its own on a device, composited
          * over whatever the application has up — including a dialog.  There
          * is one luna-ui document here, so "over" means last: appended after
          * the layer below it, with its own sheet. */
         /* A toast is a window of its own too: over the application and its
          * dialogs, under the input method, and never touchable. */
         if (g_toast_html && !status_only) {
            if (g_toast_css) luna_parse_css(g_toast_css);
            luna_parse_html(g_toast_html);
         }
         if (g_ime_html && !status_only) {
            if (g_ime_css) luna_parse_css(g_ime_css);
            luna_parse_html(g_ime_html);
         }
         /* The emulator's menu is above every window, the card included. */
         if (g_menu_html) {
            if (g_menu_css) luna_parse_css(g_menu_css);
            luna_parse_html(g_menu_html);
         }
         luna_resize((float)w, (float)h);
         overlay_wire_clicks();
         /* One layout pass so a dump of element boxes is meaningful. */
         luna_update(overlay_now(), 0.0);
         /* A reparse is not a rare event — the input method's panel, a guest
          * dialog and the status card all republish through it, and typing
          * into a field republishes the document it lives in.  Printing
          * thirteen lines every time turns a keystroke into a stderr flush
          * storm on the presenting thread, which is the guest's own swap
          * thread.  The count is cheap and stays; the element dump is a
          * layout diagnostic and goes behind its own flag. */
         {
            static unsigned long parses;
            const bool dump = getenv("LUNARIA_TRACE_OVERLAY") != NULL;
            if (dump || parses < 4 || (parses % 256) == 0)
               fprintf(stderr, "[overlay] parsed guest document: %d elements "
                       "at %dx%d (#%lu)\n", luna_element_count(), w, h,
                       parses);
            if (dump) {
               int n = luna_element_count();
               for (int i = 0; i < n && i < 12; ++i) {
                  LunaElement *e = luna_element_at(i);
                  if (!e) continue;
                  fprintf(stderr, "[overlay]   [%d] id=%s %.0fx%.0f @%.0f,%.0f "
                          "pe_none=%d\n", i, e->id[0] ? e->id : "-",
                          (double)e->w, (double)e->h, (double)e->x,
                          (double)e->y, e->pointer_events_none);
               }
            }
            ++parses;
         }
      }
      if (g_changed_image_path) {
         luna_invalidate_texture(g_changed_image_path);
         free(g_changed_image_path);
         g_changed_image_path = NULL;
      }
      pthread_mutex_unlock(&g_doc_lock);
      if (prof) { const uint64_t n = overlay_ns();
                  g_prof.parse_ns += n - t_mark; t_mark = n; }

      overlay_drain_pointer();
      /* Keystrokes go in and the field's contents come back out here: the
       * document is parsed and luna-ui is live, which is the only state its
       * element API may be called in. */
      luna_ime_frame(reparsed);
      if (prof) { const uint64_t n = overlay_ns();
                  g_prof.ime_ns += n - t_mark; t_mark = n; }

      /* The document is parsed and luna-ui's state is live here, which is the
       * only moment a caller may mutate it.  The boot card pushes its stage
       * line and its progress through this instead of republishing the page:
       * a reparse restarts every @keyframes timeline on it. */
      if (g_frame_fn) g_frame_fn();

      /* Composited over the guest's finished frame: no clear, and the
       * document's own background is whatever its CSS paints.  The status
       * card is the whole frame before the guest has drawn anything, so it
       * clears to its own ink first — otherwise the previous swap's undefined
       * back buffer shows through.
       *
       * Bind with GL_FRAMEBUFFER, not the ES3 DRAW/READ split.  The overlay
       * context is created as ES2 so it does not depend on the guest's ES3
       * context; GL_DRAW_FRAMEBUFFER is not a legal target there and raised
       * GL_INVALID_OPERATION (0x0502) on every present after a rebind. */
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glViewport(0, 0, w, h);
      if (status_only) {
         glDisable(GL_SCISSOR_TEST);
         glClearColor(0.043f, 0.059f, 0.078f, 1.f); /* #0b0f14 */
         glClear(GL_COLOR_BUFFER_BIT);
      }
      /* Everything below is composited onto the guest's finished frame, so
       * luna-ui must not start its pass by clearing the buffer — which it
       * otherwise does, because for a window it owns the previous back buffer
       * is undefined.  The status card is the exception: it *is* the whole
       * frame, and has already cleared to its own ink above. */
      luna_set_preserve_backdrop(!status_only);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_CULL_FACE);
      glDisable(GL_SCISSOR_TEST);
      glDepthMask(GL_FALSE);
      glEnable(GL_BLEND);
      glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                          GL_ONE_MINUS_SRC_ALPHA);

      double now = overlay_now();
      double dt = now - g_last_time;
      g_last_time = now;
      luna_update(now, dt);
      luna_render(w, h);
      overlay_maybe_dump_boot(w, h, status_only);
      overlay_report_gl_errors("render");
      if (prof) { const uint64_t n = overlay_ns();
                  g_prof.render_ns += n - t_mark; t_mark = n; }
   }

   /* Hand the previous binding back. */
   if (g_hosted) {
      /* The compositor's own context stays current; nothing to restore. */
   } else if (!eglMakeCurrent(dpy, draw, read, prev_ctx))
      fprintf(stderr, "[overlay] failed to restore previous context (0x%04x)\n",
              (unsigned)eglGetError());
   else if (prev_ctx != EGL_NO_CONTEXT)
      arm_exec_egl_note_current(prev_ctx, draw);

   if (prof) {
      const uint64_t n = overlay_ns();
      g_prof.restore_ns += n - t_mark;
      g_prof.total_ns += n - t_enter;
      ++g_prof.frames;
      overlay_prof_report();
   }
}

/* A shown Android dialog is modal: the window above takes every touch, and
 * the application below sees none of them.  The JIT status card is not a
 * dialog — it must not eat the touches a title needs once it starts drawing. */
bool luna_overlay_pointer(double x, double y, int action)
{
   pthread_mutex_lock(&g_doc_lock);
   bool guest = g_html != NULL;
   bool ime = g_ime_html != NULL;
   bool menu = g_menu_html != NULL && g_menu_modal;
   pthread_mutex_unlock(&g_doc_lock);
   if (g_failed) return false;
   if (menu) goto consume;    /* the emulator's menu is modal */
   if (guest) goto consume;   /* a dialog is modal: it takes every touch */
   if (!ime) return false;
   /* The input method is not modal — the application below it keeps working,
    * exactly as it does on a device — but the band it occupies is its own
    * window, and a touch there is the keyboard's, not the game's.  Before the
    * overlay has a size there is no band to be inside, so nothing is. */
   if (g_h <= 0 || y < g_h - luna_ime_band_height(g_h)) return false;
consume:
   /* Say so.  Swallowing a touch here is invisible from the guest's side --
    * it is indistinguishable from a button that does not respond -- and the
    * overlay is above the application, so it is the first thing to rule out
    * when a tap stops working.  Rate-limited, and the gate is named. */
   {
      static unsigned long taken;
      if (taken == 0 || (taken % 64) == 0)
         fprintf(stderr, "[overlay] took pointer %.0f,%.0f action=%d "
                 "(%s) — the guest does not see this touch\n", x, y, action,
                 menu ? "the emulator menu is up"
                 : guest ? "a guest dialog is up and is modal"
                       : "inside the input method's band");
      ++taken;
   }
   pthread_mutex_lock(&g_ptr_lock);
   if (g_ptr_count < (int)(sizeof g_ptr_queue / sizeof g_ptr_queue[0])) {
      g_ptr_queue[g_ptr_count].x = x;
      g_ptr_queue[g_ptr_count].y = y;
      g_ptr_queue[g_ptr_count].action = action;
      ++g_ptr_count;
   }
   pthread_mutex_unlock(&g_ptr_lock);
   luna_overlay_wake();
   return true;
}

void luna_overlay_shutdown(void)
{
   /* Blocking, unlike a present: tearing luna-ui down under a thread that is
    * inside it is the race this lock exists for. */
   pthread_mutex_lock(&g_present_lock);
   if (g_ready) luna_shutdown();
   g_ready = false;
   free(g_html);
   free(g_css);
   free(g_status_html);
   free(g_status_css);
   free(g_ime_html);
   free(g_ime_css);
   free(g_toast_html);
   free(g_toast_css);
   free(g_menu_html);
   free(g_menu_css);
   pthread_mutex_lock(&g_doc_lock);
   free(g_changed_image_path);
   g_changed_image_path = NULL;
   pthread_mutex_unlock(&g_doc_lock);
   g_toast_html = g_toast_css = NULL;
   g_menu_html = g_menu_css = NULL;
   g_html = g_css = g_status_html = g_status_css = NULL;
   g_ime_html = g_ime_css = NULL;
   pthread_mutex_unlock(&g_present_lock);
}

unsigned char *luna_overlay_image_decode(const void *data, size_t len,
                                         int *w, int *h)
{
   int ch = 0;
   if (!data || !len || len > (size_t)INT_MAX) return NULL;
   return stbi_load_from_memory((const stbi_uc *)data, (int)len, w, h, &ch, 4);
}

void luna_overlay_image_free(unsigned char *pixels)
{
   stbi_image_free(pixels);
}

float luna_overlay_text_width(const char *text, float px, bool bold)
{
   if (!text || !*text || px <= 0.f) return 0.f;
   pthread_mutex_lock(&g_present_lock);
   const bool ready = g_ready;
   float w = ready ? luna_measure_text(text, px, bold ? 1 : 0) : 0.f;
   pthread_mutex_unlock(&g_present_lock);
   if (ready) return w;
   /* Before luna-ui has its fonts: Latin at about half an em, the rest a
    * full em, which is the shape of the bundled faces. */
   for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
      if ((*p & 0xc0) == 0x80) continue;
      w += *p < 0x80 ? px * 0.55f : px;
   }
   return w;
}

float luna_overlay_line_height(float px)
{
   return luna_line_height(px > 0.f ? px : 14.f);
}

/* ======================================================================== *
 * The emulator's menu (right click)
 * ======================================================================== */

enum { SUB_NONE, SUB_SOUND, SUB_ZOOM, SUB_ENGINE };

#define MENU_W 248
#define ROW_H 24
#define SEP_H 11
#define PAD 5
#define MAX_DEVICES 16

static pthread_mutex_t g_menu_lock = PTHREAD_MUTEX_INITIALIZER;
static bool   g_menu_open;
static double g_menu_x, g_menu_y;
static int    g_menu_w, g_menu_h;
static int    g_menu_sub;
static char   g_menu_dev_name[MAX_DEVICES][128], g_menu_dev_desc[MAX_DEVICES][128];
static int    g_menu_ndev;
static char   g_menu_notice[256];
static double g_menu_notice_until;

/* macOS context-menu look: a translucent vibrancy panel, 13px system text,
 * rounded selection in the accent blue, hairline separators. */
static const char g_menu_style[] =
   "#luna-menu-backdrop{position:absolute;left:0;top:0;right:0;bottom:0;"
   "background:transparent;}"
   ".lm{position:absolute;width:" "248" "px;padding:5px;border-radius:10px;"
   "background:rgba(242,242,246,0.80);backdrop-filter:blur(24px) saturate(1.8);"
   "border:1px solid rgba(0,0,0,0.14);box-shadow:0 12px 36px rgba(0,0,0,0.30);"
   "font-size:13px;color:#1d1d1f;}"
   ".li{position:relative;height:24px;line-height:24px;padding:0 12px 0 24px;"
   "border-radius:5px;white-space:nowrap;overflow:hidden;cursor:pointer;}"
   ".li:hover{background:#0a64d8;color:#ffffff;}"
   ".li.dis{color:#a1a1a6;cursor:default;}"
   ".li.dis:hover{background:transparent;color:#a1a1a6;}"
   /* the checkmark is drawn, not a glyph: every font has a box */
   ".ck{display:block;position:absolute;left:10px;top:6px;width:4px;height:9px;"
   "border-right:2px solid #1d1d1f;border-bottom:2px solid #1d1d1f;transform:rotate(45deg);}"
   ".li:hover .ck{border-right:2px solid #ffffff;border-bottom:2px solid #ffffff;}"
   ".kb{position:absolute;right:12px;top:0;color:#8e8e93;font-size:12px;}"
   ".li:hover .kb{color:#ffffff;}"
   ".sep{height:1px;margin:5px 10px;background:rgba(0,0,0,0.13);}"
   ".hd{height:22px;line-height:22px;padding:0 12px;font-size:11px;"
   "font-weight:bold;color:#8e8e93;}"
   ".hud{position:absolute;left:50%;top:18px;transform:translate(-50%,0);"
   "padding:8px 16px;border-radius:10px;background:rgba(30,30,32,0.78);"
   "backdrop-filter:blur(20px);color:#ffffff;font-size:13px;white-space:nowrap;"
   "pointer-events:none;}";

static double menu_now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct menu_buf { char *p; size_t len, cap; };

static void menu_put(struct menu_buf *b, const char *s)
{
   size_t n = strlen(s);
   if (b->len + n + 1 > b->cap) {
      size_t cap = b->cap ? b->cap * 2 : 4096;
      while (cap < b->len + n + 1) cap *= 2;
      char *g = realloc(b->p, cap);
      if (!g) return;
      b->p = g;
      b->cap = cap;
   }
   memcpy(b->p + b->len, s, n + 1);
   b->len += n;
}

static void menu_putf(struct menu_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void menu_putf(struct menu_buf *b, const char *fmt, ...)
{
   char tmp[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(tmp, sizeof tmp, fmt, ap);
   va_end(ap);
   menu_put(b, tmp);
}

static void menu_put_text(struct menu_buf *b, const char *s)
{
   char one[2] = { 0, 0 };
   for (; *s; ++s) {
      if (*s == '<') menu_put(b, "&lt;");
      else if (*s == '>') menu_put(b, "&gt;");
      else if (*s == '&') menu_put(b, "&amp;");
      else if (*s == '"') menu_put(b, "&quot;");
      else { one[0] = *s; menu_put(b, one); }
   }
}

/* One row.  id NULL: disabled.  check: a ✓ in the gutter.  key: the
 * right-hand hint (a shortcut, a value, or ▸ for a submenu). */
static int menu_item(struct menu_buf *b, const char *id, const char *label, bool check, const char *key)
{
   if (id) menu_putf(b, "<div id=\"luna-menu-%s\" class=\"li\">", id);
   else menu_put(b, "<div class=\"li dis\">");
   if (check) menu_put(b, "<span class=\"ck\"></span>");
   menu_put_text(b, label);
   if (key && *key) { menu_put(b, "<span class=\"kb\">"); menu_put_text(b, key); menu_put(b, "</span>"); }
   menu_put(b, "</div>");
   return ROW_H;
}

static int menu_sep(struct menu_buf *b) { menu_put(b, "<div class=\"sep\"></div>"); return SEP_H; }

static int menu_header(struct menu_buf *b, const char *label)
{
   menu_put(b, "<div class=\"hd\">");
   menu_put_text(b, label);
   menu_put(b, "</div>");
   return 22;
}

static int menu_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The main panel's rows; returns its height and the top of each submenu's
 * parent row. */
static int menu_main_rows(struct menu_buf *b, int *sound_y, int *zoom_y, int *engine_y)
{
   int y = PAD;
   char v[64];
   y += menu_item(b, "shot", "Take Screenshot", false, "F12");
   y += menu_item(b, "paste", "Paste Clipboard as Typing", false, NULL);
   y += menu_sep(b);
   y += menu_item(b, "back", "Back", false, "Esc");
   y += menu_sep(b);
   const int vol = (int)(luna_os_audio_volume() * 100.0f + 0.5f);
   snprintf(v, sizeof v, "%d%%", vol);
   y += menu_item(b, "volup", "Volume Up", false, v);
   y += menu_item(b, "voldown", "Volume Down", false, NULL);
   y += menu_item(b, "mute", "Mute", luna_os_audio_muted() != 0, NULL);
   *sound_y = y;
   y += menu_item(b, "sub-sound", "Sound Output", false, "\xe2\x80\xba");
   y += menu_sep(b);
   const float z = dvm_webview_zoom();
   snprintf(v, sizeof v, "%d%%  \xe2\x80\xba", (int)(z * 100.0f + 0.5f));
   *zoom_y = y;
   y += menu_item(b, "sub-zoom", "WebView Zoom", false, v);
   *engine_y = y;
   y += menu_item(b, "sub-engine", "WebView Engine", false, "\xe2\x80\xba");
   y += menu_item(b, dvm_webview_count() ? "reload" : NULL, "Reload WebView", false, NULL);
   y += menu_sep(b);
   y += menu_item(b, "full", arm_exec_is_fullscreen() ? "Exit Full Screen" : "Enter Full Screen",
             false, NULL);
   y += menu_item(b, "quit", "Quit Lunaria", false, NULL);
   return y + PAD;
}

static int menu_sub_rows(struct menu_buf *b)
{
   int y = PAD;
   char id[32];
   if (g_menu_sub == SUB_SOUND) {
      y += menu_header(b, "SOUND OUTPUT");
      const char *cur = luna_os_audio_device();
      for (int i = 0; i < g_menu_ndev; ++i) {
         snprintf(id, sizeof id, "dev-%d", i);
         y += menu_item(b, id, g_menu_dev_desc[i], cur && !strcmp(cur, g_menu_dev_name[i]), NULL);
      }
      if (!g_menu_ndev) y += menu_item(b, NULL, "No output devices", false, NULL);
   } else if (g_menu_sub == SUB_ZOOM) {
      static const int pct[] = { 75, 100, 125, 150, 175, 200, 250, 300 };
      const int cur = (int)(dvm_webview_zoom() * 100.0f + 0.5f);
      y += menu_header(b, "WEBVIEW ZOOM");
      y += menu_item(b, "zoom-0", "Device Density", false, NULL);
      y += menu_sep(b);
      for (size_t i = 0; i < sizeof pct / sizeof pct[0]; ++i) {
         char label[16];
         snprintf(id, sizeof id, "zoom-%d", pct[i] * 10);
         snprintf(label, sizeof label, "%d%%", pct[i]);
         y += menu_item(b, id, label, cur == pct[i], NULL);
      }
   } else if (g_menu_sub == SUB_ENGINE) {
      static const char *const names[] = { "auto", "chrome", "luna", "off" };
      static const char *const labels[] = { "Automatic", "Chrome / Chromium",
                                            "luna-browser", "Off" };
      const char *cur = dvm_webview_engine();
      y += menu_header(b, "WEBVIEW ENGINE");
      for (int i = 0; i < 4; ++i) {
         snprintf(id, sizeof id, "eng-%s", names[i]);
         y += menu_item(b, id, labels[i], cur && !strcmp(cur, names[i]), NULL);
      }
   }
   return y + PAD;
}

/* Publishes the current state; g_menu_lock held. */
static void menu_publish_locked(void)
{
   if (!g_menu_open) {
      if (g_menu_notice[0]) {
         struct menu_buf b = { 0 };
         menu_put(&b, "<div class=\"hud\">");
         menu_put_text(&b, g_menu_notice);
         menu_put(&b, "</div>");
         luna_overlay_set_menu(b.p, g_menu_style, false);
         free(b.p);
      } else luna_overlay_set_menu(NULL, NULL, false);
      return;
   }
   struct menu_buf rows = { 0 }, sub = { 0 }, doc = { 0 };
   int sound_y = 0, zoom_y = 0, engine_y = 0;
   const int h = menu_main_rows(&rows, &sound_y, &zoom_y, &engine_y);
   const int mx = menu_clampi((int)g_menu_x, 4, g_menu_w - MENU_W - 4 > 4 ? g_menu_w - MENU_W - 4 : 4);
   const int my = menu_clampi((int)g_menu_y, 4, g_menu_h - h - 4 > 4 ? g_menu_h - h - 4 : 4);
   menu_put(&doc, "<div id=\"luna-menu-backdrop\"></div>");
   menu_putf(&doc, "<div class=\"lm\" style=\"left:%dpx;top:%dpx;\">", mx, my);
   menu_put(&doc, rows.p ? rows.p : "");
   menu_put(&doc, "</div>");
   if (g_menu_sub != SUB_NONE) {
      const int parent_y = g_menu_sub == SUB_SOUND ? sound_y : g_menu_sub == SUB_ZOOM ? zoom_y : engine_y;
      const int sh = menu_sub_rows(&sub);
      /* To the right of the menu, the parent row at its first item; to the
       * left when that would leave the surface. */
      int sx = mx + MENU_W - 4;
      if (sx + MENU_W > g_menu_w - 4) sx = mx - MENU_W + 4;
      const int sy = menu_clampi(my + parent_y - PAD - 22, 4, g_menu_h - sh - 4 > 4 ? g_menu_h - sh - 4 : 4);
      menu_putf(&doc, "<div class=\"lm\" style=\"left:%dpx;top:%dpx;\">", sx, sy);
      menu_put(&doc, sub.p ? sub.p : "");
      menu_put(&doc, "</div>");
   }
   if (g_menu_notice[0]) {
      menu_put(&doc, "<div class=\"hud\">");
      menu_put_text(&doc, g_menu_notice);
      menu_put(&doc, "</div>");
   }
   luna_overlay_set_menu(doc.p, g_menu_style, true);
   free(rows.p);
   free(sub.p);
   free(doc.p);
}

static void menu_notice_locked(const char *msg)
{
   snprintf(g_menu_notice, sizeof g_menu_notice, "%s", msg);
   g_menu_notice_until = menu_now() + 2.5;
}

/* The clipboard, typed into whatever has the text focus: the emulator's
 * input method, or a WebView page. */
static int menu_paste_clipboard(void)
{
   char *text = arm_exec_clipboard_get();
   if (!text) return -1;
   const bool ime = luna_ime_active();
   int typed = 0;
   for (const unsigned char *p = (const unsigned char *)text; *p; ) {
      uint32_t cp = *p;
      int n = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
      cp &= n == 3 ? 7u : n == 2 ? 15u : n == 1 ? 31u : 127u;
      ++p;
      while (n-- > 0 && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 63u);
      if (cp == '\r') continue;
      if (ime) { luna_ime_char(cp); ++typed; }
      else if (luna_overlay_web_char(cp)) ++typed;
      else break;
   }
   free(text);
   return typed;
}

static void menu_screenshot_path(char *out, size_t cap)
{
   const char *home = getenv("HOME");
   char dir[512];
   struct stat st;
   snprintf(dir, sizeof dir, "%s/Pictures", home && *home ? home : ".");
   if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
      snprintf(dir, sizeof dir, "%s", home && *home ? home : ".");
   time_t t = time(NULL);
   struct tm tm;
   localtime_r(&t, &tm);
   snprintf(out, cap, "%s/Lunaria %04d-%02d-%02d %02d.%02d.%02d.png", dir,
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void menu_clicked(const char *id)
{
   if (strncmp(id, "luna-menu", 9)) return;
   id += 9;
   if (*id == '-') ++id;
   pthread_mutex_lock(&g_menu_lock);
   bool keep_open = false;
   if (!strcmp(id, "backdrop")) {
      /* a click outside: close (a submenu first) */
      if (g_menu_sub != SUB_NONE) { g_menu_sub = SUB_NONE; keep_open = true; }
   } else if (!strcmp(id, "shot")) {
      char path[600];
      menu_screenshot_path(path, sizeof path);
      arm_exec_request_screenshot(path);
      const char *base = strrchr(path, '/');
      char msg[256];
      snprintf(msg, sizeof msg, "Screenshot saved: %s", base ? base + 1 : path);
      menu_notice_locked(msg);
   } else if (!strcmp(id, "paste")) {
      pthread_mutex_unlock(&g_menu_lock);
      const int typed = menu_paste_clipboard();
      pthread_mutex_lock(&g_menu_lock);
      if (typed <= 0) menu_notice_locked("No text field has the focus");
   } else if (!strcmp(id, "back")) {
      arm_exec_android_key(4 /* AKEYCODE_BACK */);
   } else if (!strcmp(id, "volup") || !strcmp(id, "voldown")) {
      float v = luna_os_audio_volume() + (id[3] == 'u' ? 0.1f : -0.1f);
      luna_os_audio_set_volume(v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v);
      if (luna_os_audio_muted()) luna_os_audio_set_muted(0);
      keep_open = true;
   } else if (!strcmp(id, "mute")) {
      luna_os_audio_set_muted(!luna_os_audio_muted());
      menu_notice_locked(luna_os_audio_muted() ? "Sound muted" : "Sound on");
   } else if (!strncmp(id, "sub-", 4)) {
      const int sub = !strcmp(id + 4, "sound") ? SUB_SOUND : !strcmp(id + 4, "zoom") ? SUB_ZOOM
                    : SUB_ENGINE;
      g_menu_sub = g_menu_sub == sub ? SUB_NONE : sub;
      if (g_menu_sub == SUB_SOUND) g_menu_ndev = luna_os_audio_devices(g_menu_dev_name, g_menu_dev_desc, MAX_DEVICES);
      keep_open = true;
   } else if (!strncmp(id, "dev-", 4)) {
      const int i = atoi(id + 4);
      if (i >= 0 && i < g_menu_ndev) {
         char msg[256];
         if (luna_os_audio_select_device(g_menu_dev_name[i]) == 0)
            snprintf(msg, sizeof msg, "Sound output: %s", g_menu_dev_desc[i]);
         else snprintf(msg, sizeof msg, "Cannot use %s", g_menu_dev_desc[i]);
         menu_notice_locked(msg);
      }
   } else if (!strncmp(id, "zoom-", 5)) {
      dvm_webview_set_zoom((float)atoi(id + 5) / 1000.0f);
      char msg[64];
      snprintf(msg, sizeof msg, "WebView zoom %d%%", (int)(dvm_webview_zoom() * 100.0f + 0.5f));
      menu_notice_locked(msg);
   } else if (!strncmp(id, "eng-", 4)) {
      dvm_webview_set_engine(id + 4);
      char msg[64];
      snprintf(msg, sizeof msg, "WebView engine: %s", id + 4);
      menu_notice_locked(msg);
   } else if (!strcmp(id, "reload")) {
      dvm_webview_reload();
   } else if (!strcmp(id, "full")) {
      arm_exec_toggle_fullscreen();
   } else if (!strcmp(id, "quit")) {
      arm_exec_request_quit();
   } else keep_open = true;
   g_menu_open = keep_open;
   if (!g_menu_open) g_menu_sub = SUB_NONE;
   menu_publish_locked();
   pthread_mutex_unlock(&g_menu_lock);
}

void luna_menu_open(double x, double y, int w, int h)
{
   luna_overlay_set_menu_handler(menu_clicked);
   pthread_mutex_lock(&g_menu_lock);
   g_menu_open = true;
   g_menu_sub = SUB_NONE;
   g_menu_x = x;
   g_menu_y = y;
   g_menu_w = w > 0 ? w : 1024;
   g_menu_h = h > 0 ? h : 576;
   menu_publish_locked();
   pthread_mutex_unlock(&g_menu_lock);
}

void luna_menu_close(void)
{
   pthread_mutex_lock(&g_menu_lock);
   g_menu_open = false;
   g_menu_sub = SUB_NONE;
   menu_publish_locked();
   pthread_mutex_unlock(&g_menu_lock);
}

void luna_menu_tick(void)
{
   pthread_mutex_lock(&g_menu_lock);
   if (g_menu_notice[0] && menu_now() >= g_menu_notice_until) {
      g_menu_notice[0] = '\0';
      menu_publish_locked();
   }
   pthread_mutex_unlock(&g_menu_lock);
}
