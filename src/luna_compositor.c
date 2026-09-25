/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See luna_compositor.h for what this is and why it exists.
 */

#include "luna_compositor.h"
#include "luna_boot.h"
#include "luna_ime.h"
#include "luna_overlay.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTS 3

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLConfig  g_cfg;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_win = EGL_NO_SURFACE;
static EGLNativeWindowType g_native;

static atomic_bool g_active;
static atomic_ullong g_frames;
static pthread_t g_thread;

/* Frame slots.  A slot is free, queued (the newest submitted frame, not yet
 * taken) or shown (what the compositor samples).
 *
 * A guest context lives in the share group its creator chose — Android's
 * rule — which is usually not the compositor's.  So a frame crosses over as
 * an EGLImage, which belongs to the display rather than to a share group: the
 * guest side keeps a texture per slot in its own group and an image of it,
 * and the compositor binds that image to a texture of its own.  The fences
 * are EGL syncs for the same reason. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv_comp = PTHREAD_COND_INITIALIZER;   /* compositor waits */
static pthread_cond_t  g_cv_guest = PTHREAD_COND_INITIALIZER;  /* submit waits */
static EGLImage g_slot_img[SLOTS];     /* what the slot holds now */
static EGLSync  g_ready_fence[SLOTS];  /* the copy into the slot is done */
static EGLSync  g_release_fence[SLOTS];/* the compositor is done reading it */
static int      g_queued = -1, g_shown = -1;
static int      g_frame_w[SLOTS], g_frame_h[SLOTS];
static bool     g_wake;

/* Guest side: per guest share group (keyed by the presenting context), the
 * slot textures and their images. */
#define SOURCES 8
struct comp_source {
   EGLContext ctx;
   GLuint tex[SLOTS];
   EGLImage img[SLOTS];
   int w, h;
   unsigned long long used;
};
static struct comp_source g_src[SOURCES];
static unsigned long long g_src_clock;

/* Images a resize replaced, destroyed by the compositor once no slot of its
 * own still samples them. */
static EGLImage g_retired[SOURCES * SLOTS];
static int g_nretired;

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

/* Frames that arrive as pixels (a Vulkan swapchain read back to host memory)
 * rather than in a GL context.  One pending buffer, under g_mu; the
 * compositor uploads it to a texture of its own and shows that until the next
 * frame of either kind arrives. */
static uint8_t *g_px_buf;          /* pending frame, RGBA, bottom row first */
static size_t   g_px_cap;
static int      g_px_w, g_px_h;
static bool     g_px_pending;

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- guest side ---------------------------------------------------------- */

/* The source for the current context, (re)allocated to w x h. */
static struct comp_source *source_for(EGLContext ctx, int w, int h)
{
   struct comp_source *src = NULL, *lru = &g_src[0];
   for (int i = 0; i < SOURCES; ++i) {
      if (g_src[i].ctx == ctx) { src = &g_src[i]; break; }
      if (g_src[i].used < lru->used) lru = &g_src[i];
   }
   if (!src) {
      /* A context not seen before takes the least recently used entry.  Its
       * textures belonged to another context and are simply forgotten; the
       * images are retired like a resize's. */
      src = lru;
      pthread_mutex_lock(&g_mu);
      for (int i = 0; i < SLOTS; ++i)
         if (src->img[i] && g_nretired < (int)(sizeof g_retired / sizeof g_retired[0]))
            g_retired[g_nretired++] = src->img[i];
      pthread_mutex_unlock(&g_mu);
      memset(src, 0, sizeof *src);
      src->ctx = ctx;
   }
   src->used = ++g_src_clock;
   if (src->w == w && src->h == h && src->img[0]) return src;

   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < SLOTS; ++i)
      if (src->img[i] && g_nretired < (int)(sizeof g_retired / sizeof g_retired[0]))
         g_retired[g_nretired++] = src->img[i];
   pthread_mutex_unlock(&g_mu);
   for (int i = 0; i < SLOTS; ++i) {
      if (!src->tex[i]) glGenTextures(1, &src->tex[i]);
      glBindTexture(GL_TEXTURE_2D, src->tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      const EGLAttrib attrs[] = { EGL_GL_TEXTURE_LEVEL, 0, EGL_NONE };
      src->img[i] = eglCreateImage(g_dpy, ctx, EGL_GL_TEXTURE_2D,
                                   (EGLClientBuffer)(uintptr_t)src->tex[i], attrs);
      if (src->img[i] == EGL_NO_IMAGE) {
         static int warned;
         if (warned++ < 4)
            fprintf(stderr, "[compositor] eglCreateImage failed (0x%x)\n",
                    eglGetError());
         return NULL;
      }
   }
   src->w = w;
   src->h = h;
   return src;
}

void luna_comp_submit(int w, int h, int swap_interval)
{
   if (!atomic_load_explicit(&g_active, memory_order_acquire) || w <= 0 || h <= 0)
      return;
   EGLContext ctx = eglGetCurrentContext();
   if (ctx == EGL_NO_CONTEXT) return;

   pthread_mutex_lock(&g_mu);
   /* A device's BufferQueue: with vsync on, a frame that is still queued
    * holds the producer until the consumer takes it.  Bounded, so a
    * compositor that has stopped cannot hang the guest. */
   if (swap_interval > 0 && g_queued >= 0) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 100 * 1000 * 1000;
      if (until.tv_nsec >= 1000000000) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
      while (g_queued >= 0 &&
             pthread_cond_timedwait(&g_cv_guest, &g_mu, &until) != ETIMEDOUT) {
      }
   }
   int slot = 0;
   while (slot == g_shown || slot == g_queued) ++slot;
   if (slot >= SLOTS) slot = g_queued;   /* interval 0 and both taken: replace */
   EGLSync release = g_release_fence[slot];
   g_release_fence[slot] = EGL_NO_SYNC;
   EGLSync stale = EGL_NO_SYNC;
   if (slot == g_queued) {
      stale = g_ready_fence[slot];
      g_ready_fence[slot] = EGL_NO_SYNC;
      g_queued = -1;
   }
   pthread_mutex_unlock(&g_mu);

   if (stale != EGL_NO_SYNC) eglDestroySync(g_dpy, stale);
   if (release != EGL_NO_SYNC) {
      eglWaitSync(g_dpy, release, 0);
      eglDestroySync(g_dpy, release);
   }

   GLint prev_tex = 0, prev_read = 0;
   glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
   glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);

   struct comp_source *src = source_for(ctx, w, h);
   EGLSync ready = EGL_NO_SYNC;
   if (src) {
      /* Default framebuffer -> slot.  glCopyTexSubImage2D needs no
       * framebuffer object (those are per context) and, unlike a blit, is
       * not clipped by the guest's scissor. */
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindTexture(GL_TEXTURE_2D, src->tex[slot]);
      glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
      ready = eglCreateSync(g_dpy, EGL_SYNC_FENCE, NULL);
      glFlush();
   }
   glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
   if (!src) return;

   pthread_mutex_lock(&g_mu);
   g_ready_fence[slot] = ready;
   g_slot_img[slot] = src->img[slot];
   g_frame_w[slot] = w;
   g_frame_h[slot] = h;
   g_queued = slot;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
}

void luna_comp_submit_pixels(int w, int h, const void *pixels, int stride,
                             bool bgra, int swap_interval)
{
   if (!atomic_load_explicit(&g_active, memory_order_acquire) || w <= 0 ||
       h <= 0 || !pixels)
      return;
   const size_t row = (size_t)w * 4u, need = row * (size_t)h;
   pthread_mutex_lock(&g_mu);
   if (swap_interval > 0 && g_px_pending) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 100 * 1000 * 1000;
      if (until.tv_nsec >= 1000000000) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
      while (g_px_pending &&
             pthread_cond_timedwait(&g_cv_guest, &g_mu, &until) != ETIMEDOUT) {
      }
   }
   if (g_px_cap < need) {
      uint8_t *nb = realloc(g_px_buf, need);
      if (!nb) { pthread_mutex_unlock(&g_mu); return; }
      g_px_buf = nb;
      g_px_cap = need;
   }
   /* GL's texture rows run bottom-up; the frame's run top-down. */
   for (int y = 0; y < h; ++y) {
      const uint8_t *src = (const uint8_t *)pixels + (size_t)y * (size_t)stride;
      uint8_t *dst = g_px_buf + (size_t)(h - 1 - y) * row;
      if (!bgra) {
         memcpy(dst, src, row);
         continue;
      }
      for (int x = 0; x < w; ++x) {
         dst[4 * x + 0] = src[4 * x + 2];
         dst[4 * x + 1] = src[4 * x + 1];
         dst[4 * x + 2] = src[4 * x + 0];
         dst[4 * x + 3] = src[4 * x + 3];
      }
   }
   g_px_w = w;
   g_px_h = h;
   g_px_pending = true;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
}

void luna_comp_wake(void)
{
   if (!atomic_load_explicit(&g_active, memory_order_acquire)) return;
   pthread_mutex_lock(&g_mu);
   g_wake = true;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
}

bool luna_comp_active(void)
{
   return atomic_load_explicit(&g_active, memory_order_acquire);
}

unsigned long long luna_comp_frames(void)
{
   return atomic_load_explicit(&g_frames, memory_order_relaxed);
}

/* ---- compositor thread ---------------------------------------------------- */

static GLuint g_prog, g_vbo, g_vao;
static GLint  g_u_tex;
static GLuint g_comp_tex[SLOTS];      /* the compositor's view of each slot */
static GLuint g_px_tex;               /* the last frame submitted as pixels */
static int g_px_tex_w, g_px_tex_h;
static bool g_px_shown;               /* g_px_tex is what is on screen */
static EGLImage g_comp_img[SLOTS];    /* the image bound to it */

static GLuint compile(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof log, NULL, log);
      fprintf(stderr, "[compositor] shader: %s\n", log);
   }
   return s;
}

static bool gl_setup(void)
{
   static const char vs[] =
      "attribute vec2 p;\n"
      "varying vec2 uv;\n"
      "void main(){ uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
   static const char fs[] =
      "precision mediump float;\n"
      "varying vec2 uv;\n"
      "uniform sampler2D t;\n"
      "void main(){ gl_FragColor = vec4(texture2D(t, uv).rgb, 1.0); }\n";
   g_prog = glCreateProgram();
   glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
   glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(g_prog, 0, "p");
   glLinkProgram(g_prog);
   GLint ok = 0;
   glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
   if (!ok) return false;
   g_u_tex = glGetUniformLocation(g_prog, "t");
   static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   glGenTextures(SLOTS, g_comp_tex);
   glGenTextures(1, &g_px_tex);
   p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
      eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if (!p_glEGLImageTargetTexture2DOES) return false;
   /* A vertex array of the compositor's own: luna-ui draws in this context
    * too, and attribute state set without one lands in whichever VAO luna-ui
    * last bound. */
   glGenVertexArrays(1, &g_vao);
   glBindVertexArray(g_vao);
   glGenBuffers(1, &g_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glBindVertexArray(0);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   return true;
}

static void draw_texture(GLuint tex, int fw, int fh, int win_w, int win_h)
{
   /* Letterboxed to the frame's aspect ratio: a window resized by the user
    * shows the guest's picture undistorted. */
   int vw = win_w, vh = win_h, vx = 0, vy = 0;
   if (fw > 0 && fh > 0) {
      if ((long long)win_w * fh > (long long)win_h * fw) {
         vw = (int)((long long)win_h * fw / fh);
         vx = (win_w - vw) / 2;
      } else {
         vh = (int)((long long)win_w * fh / fw);
         vy = (win_h - vh) / 2;
      }
   }
   glViewport(vx, vy, vw, vh);
   glUseProgram(g_prog);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex);
   glUniform1i(g_u_tex, 0);
   glBindVertexArray(g_vao);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glBindVertexArray(0);
   glViewport(0, 0, win_w, win_h);
}

/* The state the compositor's own draw expects, reset every frame: luna-ui
 * changes blending, scissor and program freely. */
static void reset_state(int w, int h)
{
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glViewport(0, 0, w, h);
   glDisable(GL_BLEND);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_CULL_FACE);
   glDisable(GL_STENCIL_TEST);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* LUNARIA_COMP_SHOT_S=<seconds>: every that many seconds, write what the
 * compositor is about to put on screen — the guest frame with the emulator's
 * own UI over it — to $LUNARIA_DUMP_DIR (default /tmp) as comp_NNNN.ppm.
 * The guest-side LUNARIA_SCREENSHOT_EVERY sees only the guest's frame; this
 * is the only picture of the boot card, dialogs and the input method as the
 * user sees them. */
static void comp_maybe_dump(int w, int h)
{
   static int probed;
   static uint64_t every_ns, next_ns;
   static unsigned seq;
   if (!probed) {
      probed = 1;
      const char *e = getenv("LUNARIA_COMP_SHOT_S");
      const double s = e ? atof(e) : 0.0;
      every_ns = s > 0.0 ? (uint64_t)(s * 1e9) : 0u;
      next_ns = now_ns() + every_ns;
   }
   if (!every_ns || w <= 0 || h <= 0) return;
   const uint64_t t = now_ns();
   if (t < next_ns) return;
   next_ns = t + every_ns;
   const size_t row = (size_t)w * 4u;
   uint8_t *px = malloc(row * (size_t)h);
   if (!px) return;
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   const char *dir = getenv("LUNARIA_DUMP_DIR");
   char path[1024];
   snprintf(path, sizeof path, "%s/comp_%04u.ppm", dir && *dir ? dir : "/tmp",
            seq++);
   FILE *f = fopen(path, "wb");
   if (f) {
      fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int y = h - 1; y >= 0; --y)
         for (int x = 0; x < w; ++x)
            fwrite(px + (size_t)y * row + (size_t)x * 4u, 1, 3, f);
      fclose(f);
      fprintf(stderr, "[compositor] shot -> %s\n", path);
   }
   free(px);
}

static void *comp_main(void *arg)
{
   (void)arg;
   if (!eglMakeCurrent(g_dpy, g_win, g_win, g_ctx)) {
      fprintf(stderr, "[compositor] eglMakeCurrent failed (0x%x)\n", eglGetError());
      atomic_store(&g_active, false);
      luna_overlay_set_hosted(false);
      return NULL;
   }
   eglSwapInterval(g_dpy, 1);
   if (!gl_setup()) {
      fprintf(stderr, "[compositor] shader setup failed\n");
      atomic_store(&g_active, false);
      luna_overlay_set_hosted(false);
      return NULL;
   }
   luna_overlay_bind_host_thread();
   fprintf(stderr, "[compositor] running (window owned by its own thread)\n");

   bool have_frame = false;
   uint64_t last_draw = 0;
   for (;;) {
      /* How long the picture may stay as it is: the boot card and the input
       * method's caret animate; a static screen needs nothing until the guest
       * or the UI changes it. */
      const bool boot = luna_boot_active() && !have_frame;
      const bool ime = luna_ime_active();
      const uint64_t period = boot ? 16000000ull : ime ? 33000000ull
                             : luna_overlay_active() ? 100000000ull
                             : 1000000000ull;

      pthread_mutex_lock(&g_mu);
      if (g_queued < 0 && !g_wake && !g_px_pending) {
         const uint64_t due = last_draw + period;
         const uint64_t t = now_ns();
         if (due > t) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            uint64_t ns = (uint64_t)until.tv_nsec + (due - t);
            until.tv_sec += (time_t)(ns / 1000000000ull);
            until.tv_nsec = (long)(ns % 1000000000ull);
            (void)pthread_cond_timedwait(&g_cv_comp, &g_mu, &until);
         }
      }
      g_wake = false;
      int take = g_queued;
      EGLSync ready = EGL_NO_SYNC;
      EGLImage take_img = EGL_NO_IMAGE;
      int prev_shown = g_shown;
      if (take >= 0) {
         ready = g_ready_fence[take];
         take_img = g_slot_img[take];
         g_ready_fence[take] = EGL_NO_SYNC;
         g_queued = -1;
         g_shown = take;
         pthread_cond_signal(&g_cv_guest);
      }
      const int shown = g_shown;
      if (g_px_pending) {
         /* Uploaded under the lock: the buffer is the submitter's to refill
          * as soon as it is released. */
         glBindTexture(GL_TEXTURE_2D, g_px_tex);
         if (g_px_tex_w != g_px_w || g_px_tex_h != g_px_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_px_w, g_px_h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, g_px_buf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_px_tex_w = g_px_w;
            g_px_tex_h = g_px_h;
         } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_px_w, g_px_h, GL_RGBA,
                            GL_UNSIGNED_BYTE, g_px_buf);
         }
         g_px_pending = false;
         g_px_shown = true;
         have_frame = true;
         pthread_cond_signal(&g_cv_guest);
      } else if (take >= 0) {
         g_px_shown = false;
      }
      pthread_mutex_unlock(&g_mu);

      if (take >= 0) {
         if (ready != EGL_NO_SYNC) {
            eglWaitSync(g_dpy, ready, 0);
            eglDestroySync(g_dpy, ready);
         }
         if (take_img != g_comp_img[take]) {
            glBindTexture(GL_TEXTURE_2D, g_comp_tex[take]);
            p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)take_img);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_comp_img[take] = take_img;
         }
         have_frame = true;
      }
      /* Images a resize or an evicted context left behind, once none of
       * this side's slots is bound to them. */
      pthread_mutex_lock(&g_mu);
      for (int i = 0; i < g_nretired; ) {
         bool bound = false;
         for (int k = 0; k < SLOTS; ++k)
            bound |= g_comp_img[k] == g_retired[i] || g_slot_img[k] == g_retired[i];
         if (bound) { ++i; continue; }
         eglDestroyImage(g_dpy, g_retired[i]);
         g_retired[i] = g_retired[--g_nretired];
      }
      pthread_mutex_unlock(&g_mu);

      EGLint w = 0, h = 0;
      eglQuerySurface(g_dpy, g_win, EGL_WIDTH, &w);
      eglQuerySurface(g_dpy, g_win, EGL_HEIGHT, &h);
      if (w <= 0 || h <= 0) { last_draw = now_ns(); continue; }

      reset_state(w, h);
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      if (have_frame && g_px_shown)
         draw_texture(g_px_tex, g_px_tex_w, g_px_tex_h, w, h);
      else if (have_frame && shown >= 0)
         draw_texture(g_comp_tex[shown], g_frame_w[shown], g_frame_h[shown], w, h);

      if (luna_boot_active() && !have_frame)
         luna_overlay_present_boot(w, h);
      else
         luna_overlay_present(w, h);

      /* The slot that stopped being shown is free once this frame's reads of
       * it are done — the guest waits on that before copying into it. */
      EGLSync done = eglCreateSync(g_dpy, EGL_SYNC_FENCE, NULL);
      glFlush();
      pthread_mutex_lock(&g_mu);
      const int release_slot = (take >= 0 && prev_shown >= 0 && prev_shown != take)
                               ? prev_shown : -1;
      if (release_slot >= 0 && g_release_fence[release_slot] == EGL_NO_SYNC) {
         g_release_fence[release_slot] = done;
         done = EGL_NO_SYNC;
      }
      pthread_mutex_unlock(&g_mu);
      if (done != EGL_NO_SYNC) eglDestroySync(g_dpy, done);

      comp_maybe_dump(w, h);
      eglSwapBuffers(g_dpy, g_win);
      atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
      last_draw = now_ns();
   }
   return NULL;
}

bool luna_comp_start(void *egl_display, void *egl_config, void *native_window)
{
   if (atomic_load(&g_active)) return true;
   g_dpy = (EGLDisplay)egl_display;
   g_cfg = (EGLConfig)egl_config;
   g_native = (EGLNativeWindowType)(uintptr_t)native_window;

   g_win = eglCreateWindowSurface(g_dpy, g_cfg, g_native, NULL);
   if (g_win == EGL_NO_SURFACE) {
      fprintf(stderr, "[compositor] eglCreateWindowSurface failed (0x%x)\n",
              eglGetError());
      return false;
   }
   static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   /* A share group of its own.  Frames arrive as EGLImages, which belong
    * to the display, so nothing here needs the guest's objects — and the
    * guest must not reach the emulator's: GLES lets an application bind a
    * buffer name it never generated, and Genshin's writes landed in luna-ui's
    * quad buffer, so every dialog and the input method drew nothing once the
    * game was rendering. */
   g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
   if (g_ctx == EGL_NO_CONTEXT) {
      fprintf(stderr, "[compositor] eglCreateContext failed (0x%x)\n", eglGetError());
      eglDestroySurface(g_dpy, g_win);
      g_win = EGL_NO_SURFACE;
      return false;
   }
   luna_overlay_set_wake(luna_comp_wake);
   /* Before the thread starts: from here on only it draws the overlay. */
   luna_overlay_set_hosted(true);
   atomic_store(&g_active, true);
   if (pthread_create(&g_thread, NULL, comp_main, NULL) != 0) {
      atomic_store(&g_active, false);
      luna_overlay_set_hosted(false);
      return false;
   }
   pthread_detach(g_thread);
   return true;
}
