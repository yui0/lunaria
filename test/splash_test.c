/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Renders the boot screen on its own.
 *
 * The screen it draws is otherwise only visible inside the half-minute a real
 * title takes to boot, on a surface the guest is about to take — which is a
 * poor place to find out that a CSS rule did not parse.  This drives it
 * headlessly instead: a surfaceless EGL context, the same luna_splash calls
 * the dex loader makes, and a PPM per frame, so the moon's rise and the
 * terminator's sweep can be looked at frame by frame.
 *
 *   ./test/test_splash [frames] [out-dir]
 */

#include "luna_overlay.h"
#include "luna_splash.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int W = 1280, H = 720;
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLSurface surf = EGL_NO_SURFACE;
static const char *out_dir = "/tmp/lunaria-splash";
static int frame_no;

static void write_ppm(void)
{
   unsigned char *px = malloc((size_t)W * H * 4);
   if (!px) return;
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   char path[512];
   snprintf(path, sizeof path, "%s/splash_%04d.ppm", out_dir, frame_no++);
   FILE *f = fopen(path, "wb");
   if (f) {
      fprintf(f, "P6\n%d %d\n255\n", W, H);
      /* glReadPixels is bottom-up; PPM is top-down. */
      for (int y = H - 1; y >= 0; --y)
         for (int x = 0; x < W; ++x)
            fwrite(px + ((size_t)y * W + x) * 4, 1, 3, f);
      fclose(f);
   }
   free(px);
}

static void present(void)
{
   luna_overlay_present_ex(W, H, /*clear=*/true);
   write_ppm();
   eglSwapBuffers(dpy, surf);
}

int main(int argc, char **argv)
{
   int frames = argc > 1 ? atoi(argv[1]) : 12;
   if (argc > 2) out_dir = argv[2];

   PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   if (get_dpy)
      dpy = get_dpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major = 0, minor = 0;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
      fprintf(stderr, "splash-test: no EGL display\n");
      return 1;
   }
   static const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE
   };
   EGLConfig cfg;
   EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n != 1) {
      fprintf(stderr, "splash-test: eglChooseConfig failed\n");
      return 1;
   }
   const EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
   surf = eglCreatePbufferSurface(dpy, cfg, pb);
   const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
       !eglMakeCurrent(dpy, surf, surf, ctx)) {
      fprintf(stderr, "splash-test: no context\n");
      return 1;
   }
   fprintf(stderr, "splash-test: EGL %d.%d, %s, %dx%d → %s\n", major, minor,
           (const char *)glGetString(GL_VERSION), W, H, out_dir);

   luna_splash_set_presenter(present);
   luna_splash_begin();

   /* The same sequence a real boot reports, compressed: link, static
    * initialisers, then four dex files of the sizes this title ships. */
   static const struct { const char *name; unsigned long long bytes;
                         unsigned classes, methods; } dexes[] = {
      { "classes.dex",  8457460ull, 8465, 65517 },
      { "classes2.dex", 7374744ull, 8364, 65532 },
      { "classes3.dex", 7665316ull, 5979, 63502 },
      { "classes4.dex", 5839684ull, 4577, 40720 },
   };
   luna_splash_stage("linking libUE4.so");
   luna_splash_dex_total(4, 8457460ull + 7374744ull + 7665316ull + 5839684ull);

   for (int i = 0; i < frames; ++i) {
      if (i && i % 3 == 0) {
         unsigned d = (unsigned)(i / 3) - 1u;
         if (d < sizeof dexes / sizeof dexes[0])
            luna_splash_dex_loaded(dexes[d].name, dexes[d].classes,
                                   dexes[d].methods, dexes[d].bytes);
      }
      /* One frame per 1/15 s of animation time, so a short run still shows
       * the moon rising and the terminator moving. */
      struct timespec ts = { 0, 66 * 1000 * 1000 };
      nanosleep(&ts, NULL);
      present();
   }
   fprintf(stderr, "splash-test: wrote %d frames\n", frame_no);
   luna_splash_end("test done");
   luna_overlay_shutdown();
   return 0;
}
