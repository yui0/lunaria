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

#include <dlfcn.h>
#include <pthread.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool  g_ready;          /* luna_init() has run against a live context */
static bool  g_failed;         /* …and failed; do not retry every frame */
static char *g_html;
static char *g_css;
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

static double overlay_now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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

/* The overlay's own context, sharing objects with the guest's.
 *
 * Saving and restoring individual pieces of the guest's GL state was never
 * going to be complete: a modern engine leaves dozens of bindings set, and the
 * text pass in particular kept coming out wrong while every untextured box
 * painted correctly, with every state query reporting exactly what it should.
 * A second context in the same share group removes the question — luna-ui gets
 * a clean state vector of its own, while textures, buffers and programs stay
 * shared, so nothing has to be uploaded twice. */
static EGLDisplay g_ov_dpy = EGL_NO_DISPLAY;
static EGLContext g_ov_ctx = EGL_NO_CONTEXT;

/* Finds the EGLConfig a context was created with.  eglCreateContext needs one,
 * and the only handle onto it is the config id the context remembers. */
static bool overlay_config_of(EGLDisplay dpy, EGLContext ctx, EGLConfig *out)
{
   EGLint id = 0;
   if (!eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &id)) return false;
   const EGLint attrs[] = { EGL_CONFIG_ID, id, EGL_NONE };
   EGLint n = 0;
   return eglChooseConfig(dpy, attrs, out, 1, &n) && n == 1;
}

static bool overlay_context_create(void)
{
   EGLDisplay dpy = eglGetCurrentDisplay();
   EGLContext guest = eglGetCurrentContext();
   if (dpy == EGL_NO_DISPLAY || guest == EGL_NO_CONTEXT) return false;
   EGLConfig cfg;
   if (!overlay_config_of(dpy, guest, &cfg)) return false;
   EGLint version = 3;
   (void)eglQueryContext(dpy, guest, EGL_CONTEXT_CLIENT_VERSION, &version);
   const EGLint attrs[] = { EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, guest, attrs);
   if (ctx == EGL_NO_CONTEXT) return false;
   g_ov_dpy = dpy;
   g_ov_ctx = ctx;
   return true;
}

static bool overlay_start(int w, int h)
{
   if (g_ready) return true;
   if (g_failed) return false;

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

void luna_overlay_set_click_handler(luna_overlay_click_fn fn)
{
   g_click_fn = fn;
}

/* luna-ui hands the clicked element back; its DOM id is the only thing the
 * widget layer needs to identify the guest View it stands for. */
static void overlay_element_clicked(LunaElement *e)
{
   if (g_click_fn && e && e->id[0]) g_click_fn(e->id);
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

bool luna_overlay_active(void)
{
   overlay_maybe_test_card();
   pthread_mutex_lock(&g_doc_lock);
   bool up = g_html != NULL;
   pthread_mutex_unlock(&g_doc_lock);
   return up && !g_failed;
}

void luna_overlay_present(int w, int h)
{
   if (!luna_overlay_active() || w <= 0 || h <= 0) return;

   EGLDisplay dpy = eglGetCurrentDisplay();
   EGLContext guest_ctx = eglGetCurrentContext();
   EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
   EGLSurface read = eglGetCurrentSurface(EGL_READ);
   if (dpy == EGL_NO_DISPLAY || guest_ctx == EGL_NO_CONTEXT) return;

   if (g_ov_ctx == EGL_NO_CONTEXT && !g_failed && !overlay_context_create()) {
      fprintf(stderr, "[overlay] no shared context — emulator UI disabled\n");
      g_failed = true;
      return;
   }
   if (!eglMakeCurrent(dpy, draw, read, g_ov_ctx)) {
      fprintf(stderr, "[overlay] eglMakeCurrent failed (0x%04x)\n",
              (unsigned)eglGetError());
      g_failed = true;
      return;
   }

   if (overlay_start(w, h)) {
      luna_invalidate_gl_state();
      if (w != g_w || h != g_h) {
         g_w = w;
         g_h = h;
         luna_resize((float)w, (float)h);
      }
      pthread_mutex_lock(&g_doc_lock);
      if (g_doc_dirty) {
         g_doc_dirty = false;
         /* Styles are resolved while the HTML is parsed, so the sheet has to
          * be in place first. */
         luna_reset_css();
         if (g_from_files) {
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
         } else {
            if (g_css) luna_parse_css(g_css);
            luna_parse_html(g_html);
         }
         luna_resize((float)w, (float)h);
         overlay_wire_clicks();
      }
      pthread_mutex_unlock(&g_doc_lock);

      overlay_drain_pointer();

      /* Composited over the guest's finished frame: no clear, and the
       * document's own background is whatever its CSS paints. */
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glViewport(0, 0, w, h);
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
      overlay_report_gl_errors("render");
   }

   /* Hand the guest back exactly the binding it had. */
   if (!eglMakeCurrent(dpy, draw, read, guest_ctx))
      fprintf(stderr, "[overlay] failed to restore the guest context (0x%04x)\n",
              (unsigned)eglGetError());
}

/* A shown Android dialog is modal: the window above takes every touch, and
 * the application below sees none of them.  So while a document is up the
 * overlay consumes the event unconditionally — there is no hit test to do,
 * and doing one here would read luna-ui's layout from the wrong thread. */
bool luna_overlay_pointer(double x, double y, int action)
{
   if (!luna_overlay_active()) return false;
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
   if (g_ready) luna_shutdown();
   g_ready = false;
   free(g_html);
   free(g_css);
   g_html = g_css = NULL;
}
