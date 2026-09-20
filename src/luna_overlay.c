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
#define LUNA_UI_IMPLEMENTATION
#include "luna-ui.h"

#include "luna_overlay.h"
#include "luna_ime.h"
#include "arm_exec.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
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
static bool  g_doc_dirty;
static bool  g_from_files;   /* LUNARIA_UI_TEST names an HTML file */
static int   g_w, g_h;
static double g_last_time;

/* The document is published by whichever guest thread runs the widget layer,
 * and consumed by the guest thread that swaps.  Those are different threads —
 * luna-ui has no locking of its own, so the handoff is a copied string under a
 * mutex and every luna_* call stays on the presenting thread. */
static pthread_mutex_t g_doc_lock = PTHREAD_MUTEX_INITIALIZER;

/* Pointer events arrive on the host's event thread (GLFW polls there) and
 * cannot touch luna-ui's hit-testing state from outside the presenting
 * thread, so they queue here and are replayed at the top of present(). */
struct overlay_pointer_event { double x, y; int action; };
static struct overlay_pointer_event g_ptr_queue[64];
static int g_ptr_count;
static pthread_mutex_t g_ptr_lock = PTHREAD_MUTEX_INITIALIZER;

static luna_overlay_click_fn g_click_fn;
static luna_overlay_frame_fn g_frame_fn;

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
   /* Prefer ES2 so luna-ui does not depend on the guest's ES3 context. */
   const EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, es2);
   if (ctx == EGL_NO_CONTEXT && current != EGL_NO_CONTEXT) {
      EGLint version = 2;
      (void)eglQueryContext(dpy, current, EGL_CONTEXT_CLIENT_VERSION, &version);
      const EGLint attrs[] = { EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE };
      ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, attrs);
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

void luna_overlay_set_document(const char *html, const char *css)
{
   pthread_mutex_lock(&g_doc_lock);
   free(g_html);
   g_html = html ? strdup(html) : NULL;
   if (css) {
      free(g_css);
      g_css = strdup(css);
   }
   g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
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
   /* Only dirty the parsed document when the status card is what we would
    * show — a guest dialog owns the parsed tree and must not be rebuilt from
    * a progress-bar rewrite. */
   if (!g_html) g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
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
   /* The panel is composited into the same parsed document, so raising or
    * dropping it is a reparse either way. */
   g_doc_dirty = true;
   pthread_mutex_unlock(&g_doc_lock);
}

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

void luna_overlay_set_frame_handler(luna_overlay_frame_fn fn)
{
   g_frame_fn = fn;
}

/* luna-ui hands the clicked element back; its DOM id is the only thing the
 * widget layer needs to identify the guest View it stands for. */
static void overlay_element_clicked(LunaElement *e)
{
   if (!e || !e->id[0]) return;
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
   bool up = g_html != NULL || g_status_html != NULL || g_ime_html != NULL;
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

void luna_overlay_present(int w, int h)
{
   if (!luna_overlay_active() || w <= 0 || h <= 0) return;
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
      /* Guest widgets win.  The status card is only the document when nothing
       * from the Android View layer is up. */
      const char *html = g_html ? g_html : g_status_html;
      const char *css  = g_html ? g_css  : g_status_css;
      const bool status_only =
         g_html == NULL && g_status_html != NULL && g_ime_html == NULL;
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
         if (g_ime_html) {
            if (g_ime_css) luna_parse_css(g_ime_css);
            luna_parse_html(g_ime_html);
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
       * back buffer shows through. */
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
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
   if (!eglMakeCurrent(dpy, draw, read, prev_ctx))
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
   pthread_mutex_unlock(&g_doc_lock);
   if (g_failed) return false;
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
                 guest ? "a guest dialog is up and is modal"
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
   g_html = g_css = g_status_html = g_status_css = NULL;
   g_ime_html = g_ime_css = NULL;
   pthread_mutex_unlock(&g_present_lock);
}
