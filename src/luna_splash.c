/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See luna_splash.h for what this screen is and why it exists.
 *
 * The document is published once and then never reparsed.  That is not an
 * optimisation: luna-ui restarts every @keyframes timeline when a document is
 * parsed, so rebuilding the page each time a dex finished would have reset the
 * moon to the horizon eight times during boot.  What changes — the stage line,
 * the counters, the width of the bar — is pushed straight into the live DOM
 * through luna_set_text() and a class swap, which leaves the animations
 * running.
 */

/* The engine itself is compiled into luna_overlay.c (the one translation unit
 * that defines LUNA_UI_IMPLEMENTATION); this file only calls it. */
#define LUNA_UI_NO_PLATFORM
#include "luna-ui.h"

#include "luna_splash.h"
#include "luna_overlay.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Everything a frame needs to draw, and the lock the guest threads that
 * update it share with the thread that presents. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool  g_up;
static bool  g_finished;
static char  g_stage[160] = "starting";
static char  g_counts[160] = "";
static int   g_dex_files, g_dex_done;
static uint64_t g_dex_bytes, g_dex_bytes_done;
static uint32_t g_classes, g_methods;
static float g_fraction;
static unsigned long g_frames;
static bool  g_text_dirty = true;
static luna_splash_present_fn g_present;
/* Held while a frame is on its way to the surface.  luna_splash_end() takes it
 * too: the guest calls eglMakeCurrent on its own thread, and taking down the
 * document while another thread is inside luna_render() would race it. */
static pthread_mutex_t g_pump_lock = PTHREAD_MUTEX_INITIALIZER;

static double splash_now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- the page ---------------------------------------------------------- *
 *
 * A moon, its halo, a drifting star field, the wordmark, and the loader's
 * progress.  Every moving part is a CSS animation: luna-ui interpolates
 * opacity, transform scale and transform translate across @keyframes stops,
 * so the moon rises and settles, the halo breathes, the terminator sweeps the
 * disc, and the stars twinkle out of phase with one another.
 *
 * The bar's fill is width-by-class rather than an inline style: swapping a
 * class on a live element restyles it, where rewriting the document would
 * restart the animations above.  Twenty-one steps is one per five percent,
 * which is finer than the eye reads on a bar this size.
 */
static const char *splash_html(void)
{
   return
   "<body>"
     "<div class=\"stage-bg\"></div>"
     "<div class=\"stars\">"
       "<div class=\"star a\"></div><div class=\"star b\"></div>"
       "<div class=\"star c\"></div><div class=\"star d\"></div>"
       "<div class=\"star e\"></div><div class=\"star f\"></div>"
       "<div class=\"star g\"></div><div class=\"star h\"></div>"
     "</div>"
     "<div class=\"moonwrap\">"
       "<div class=\"halo\"></div>"
       "<div class=\"moon\">"
         "<div class=\"crater k1\"></div>"
         "<div class=\"crater k2\"></div>"
         "<div class=\"crater k3\"></div>"
         "<div class=\"terminator\"></div>"
       "</div>"
     "</div>"
     "<div class=\"wordmark\">LUNARIA</div>"
     "<div class=\"panel\">"
       "<div id=\"stage\" class=\"stage\">starting</div>"
       "<div class=\"bar\"><div id=\"fill\" class=\"fill p0\"></div></div>"
       "<div id=\"counts\" class=\"counts\"></div>"
     "</div>"
   "</body>";
}

static const char *splash_css(void)
{
   return
   /* The boot screen owns the whole surface: there is no guest frame under it
    * yet, so this background is the only thing between the moon and the
    * uninitialised buffer. */
   "body{margin:0;background:#05060d;color:#e8eefc;font-size:15px;}"
   ".stage-bg{position:fixed;left:0;top:0;width:100%;height:100%;"
     "background:radial-gradient(circle at 50% 34%, #16204a 0%, "
     "#0a0e22 46%, #05060d 100%);}"

   /* --- star field --------------------------------------------------- */
   ".stars{position:fixed;left:0;top:0;width:100%;height:100%;}"
   ".star{position:absolute;width:3px;height:3px;border-radius:3px;"
     "background:#cfe0ff;opacity:0.15;"
     "animation:twinkle 2.6s ease-in-out infinite alternate;}"
   ".star.a{left:14%;top:18%;}"
   ".star.b{left:27%;top:41%;animation-delay:0.4s;}"
   ".star.c{left:38%;top:12%;animation-delay:0.9s;}"
   ".star.d{left:63%;top:22%;animation-delay:1.3s;}"
   ".star.e{left:76%;top:37%;animation-delay:0.2s;}"
   ".star.f{left:86%;top:15%;animation-delay:1.7s;}"
   ".star.g{left:8%;top:57%;animation-delay:2.1s;}"
   ".star.h{left:92%;top:56%;animation-delay:0.7s;}"
   "@keyframes twinkle{0%{opacity:0.10;transform:scale(0.7);}"
     "100%{opacity:0.85;transform:scale(1.25);}}"

   /* --- the moon ------------------------------------------------------ *
    * The disc rises into place once; the halo behind it keeps breathing, so
    * a boot that takes half a minute still looks alive rather than frozen on
    * a static logo. */
   ".moonwrap{position:fixed;left:50%;top:34%;width:0;height:0;}"
   ".halo{position:absolute;left:-118px;top:-118px;width:236px;height:236px;"
     "border-radius:236px;background:radial-gradient(circle at 50% 50%, "
     "rgba(150,182,255,0.34) 0%, rgba(90,120,220,0.14) 52%, "
     "rgba(10,14,34,0.0) 72%);"
     "animation:breathe 3.4s ease-in-out infinite alternate;}"
   "@keyframes breathe{0%{transform:scale(0.86);opacity:0.55;}"
     "100%{transform:scale(1.14);opacity:1.0;}}"
   ".moon{position:absolute;left:-56px;top:-56px;width:112px;height:112px;"
     "border-radius:112px;"
     "background:radial-gradient(circle at 36% 32%, #fdfbf2 0%, "
     "#e9e6d8 42%, #bfc3d2 78%, #8a90a8 100%);"
     "box-shadow:0 0 42px rgba(176,200,255,0.45);"
     "animation:moonrise 1.6s ease-in-out;}"
   /* Rises from below its resting place and settles — one pass, not a loop:
    * a logo that keeps re-entering reads as a stutter. */
   "@keyframes moonrise{0%{opacity:0;transform:translate(0px,46px) scale(0.72);}"
     "60%{opacity:1;transform:translate(0px,-6px) scale(1.05);}"
     "100%{opacity:1;transform:translate(0px,0px) scale(1.0);}}"
   ".crater{position:absolute;border-radius:40px;background:#c7c9d6;"
     "opacity:0.55;}"
   ".k1{left:26px;top:30px;width:22px;height:22px;}"
   ".k2{left:60px;top:56px;width:15px;height:15px;}"
   ".k3{left:37px;top:72px;width:10px;height:10px;}"
   /* The night side of the disc, sliding across it: the moon goes through its
    * phases for as long as the boot lasts. */
   ".terminator{position:absolute;left:0;top:0;width:112px;height:112px;"
     "border-radius:112px;"
     "background:linear-gradient(90deg, rgba(5,6,13,0.0) 0%, "
     "rgba(5,6,13,0.72) 46%, rgba(5,6,13,0.90) 100%);"
     "animation:phase 6.0s ease-in-out infinite alternate;}"
   "@keyframes phase{0%{transform:translate(-104px,0px);opacity:0.15;}"
     "100%{transform:translate(58px,0px);opacity:0.95;}}"

   /* --- wordmark and the loader read-out ------------------------------ */
   ".wordmark{position:fixed;left:0;top:52%;width:100%;text-align:center;"
     "font-size:30px;letter-spacing:9px;color:#dfe8ff;opacity:0;"
     "animation:fadein 1.1s ease-in-out;animation-delay:0.5s;}"
   "@keyframes fadein{0%{opacity:0;transform:translate(0px,10px);}"
     "100%{opacity:1;transform:translate(0px,0px);}}"
   ".panel{position:fixed;left:50%;top:64%;width:520px;"
     "margin-left:-260px;text-align:center;opacity:0;"
     "animation:fadein 0.9s ease-in-out;animation-delay:0.9s;}"
   ".stage{font-size:15px;color:#9fb2d8;padding-bottom:10px;width:520px;"
     "text-align:center;}"
   ".bar{position:relative;width:520px;height:5px;border-radius:5px;"
     "background:rgba(159,178,216,0.16);}"
   ".fill{position:absolute;left:0;top:0;height:5px;border-radius:5px;"
     "background:linear-gradient(90deg, #6f8fe8 0%, #cfe0ff 100%);}"
   ".counts{padding-top:10px;font-size:13px;color:#66759a;width:520px;"
     "text-align:center;}"
   /* One rule per five percent: the class is swapped on the live element, so
    * the bar moves without the document being reparsed. */
   ".fill.p0{width:0px;}"    ".fill.p5{width:26px;}"   ".fill.p10{width:52px;}"
   ".fill.p15{width:78px;}"  ".fill.p20{width:104px;}" ".fill.p25{width:130px;}"
   ".fill.p30{width:156px;}" ".fill.p35{width:182px;}" ".fill.p40{width:208px;}"
   ".fill.p45{width:234px;}" ".fill.p50{width:260px;}" ".fill.p55{width:286px;}"
   ".fill.p60{width:312px;}" ".fill.p65{width:338px;}" ".fill.p70{width:364px;}"
   ".fill.p75{width:390px;}" ".fill.p80{width:416px;}" ".fill.p85{width:442px;}"
   ".fill.p90{width:468px;}" ".fill.p95{width:494px;}" ".fill.p100{width:520px;}";
}

/* ---- pushing state into the live document ------------------------------ */

/* luna_get_element_by_id() is the engine's own lookup; LunaElement is opaque
 * outside the translation unit that defines LUNA_UI_IMPLEMENTATION, so this
 * file never dereferences one. */
static int splash_find(const char *id) { return luna_get_element_by_id(id); }

/* Called on the presenting thread, with the document parsed and luna-ui's
 * state live.  Everything here is a mutation of existing elements. */
static void splash_frame(void)
{
   char stage[160], counts[160];
   int step;
   pthread_mutex_lock(&g_lock);
   if (!g_text_dirty) { pthread_mutex_unlock(&g_lock); return; }
   g_text_dirty = false;
   snprintf(stage, sizeof stage, "%s", g_stage);
   snprintf(counts, sizeof counts, "%s", g_counts);
   float f = g_fraction < 0.0f ? 0.0f : (g_fraction > 1.0f ? 1.0f : g_fraction);
   step = (int)(f * 20.0f + 0.5f) * 5;
   pthread_mutex_unlock(&g_lock);

   int i = splash_find("stage");
   if (i >= 0) luna_set_text(i, stage);
   i = splash_find("counts");
   if (i >= 0) luna_set_text(i, counts);

   i = splash_find("fill");
   if (i >= 0) {
      /* The previous step's class has to go, or the two width rules compete
       * and the more specific — which is whichever the sheet lists last —
       * wins regardless of which one is current. */
      static int applied = -1;
      if (applied != step) {
         char cls[8];
         if (applied >= 0) {
            snprintf(cls, sizeof cls, "p%d", applied);
            luna_remove_class(i, cls);
         }
         snprintf(cls, sizeof cls, "p%d", step);
         luna_add_class(i, cls);
         applied = step;
         luna_mark_layout_dirty();
      }
   }
}

/* ---- the public surface ------------------------------------------------ */

void luna_splash_set_presenter(luna_splash_present_fn fn) { g_present = fn; }

bool luna_splash_active(void)
{
   pthread_mutex_lock(&g_lock);
   bool up = g_up && !g_finished;
   pthread_mutex_unlock(&g_lock);
   return up;
}

void luna_splash_begin(void)
{
   pthread_mutex_lock(&g_lock);
   if (g_up || g_finished) { pthread_mutex_unlock(&g_lock); return; }
   g_up = true;
   g_text_dirty = true;
   pthread_mutex_unlock(&g_lock);

   luna_overlay_set_frame_handler(splash_frame);
   luna_overlay_set_document(splash_html(), splash_css());
   fprintf(stderr, "[splash] boot screen up\n");
   luna_splash_pump();
}

void luna_splash_stage(const char *fmt, ...)
{
   if (!luna_splash_active()) return;
   char buf[160];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof buf, fmt, ap);
   va_end(ap);
   pthread_mutex_lock(&g_lock);
   snprintf(g_stage, sizeof g_stage, "%s", buf);
   g_text_dirty = true;
   pthread_mutex_unlock(&g_lock);
   luna_splash_pump();
}

/* The dex phase gets its own share of the bar.  It is the one part of boot
 * with a real denominator, so it is measured in bytes rather than in files:
 * classes.dex here is 8.4 MB and classes4.dex 5.8 MB, and counting files
 * would make the bar jump unevenly. */
/* The bar is a budget split across the phases in the order they run: ELF link
 * and static initialisers, JNI_OnLoad, the dex files, then the wait for the
 * engine's first frame.  The fraction never moves backwards, so a later phase
 * claiming a range below an earlier one would simply freeze the bar — which is
 * what a dex range ending below JNI_OnLoad's mark did. */
#define SPLASH_LINK_FLOOR 0.02f
#define SPLASH_LINK_CEIL  0.30f
#define SPLASH_JNI_MARK   0.32f
#define SPLASH_DEX_FLOOR  0.35f
#define SPLASH_DEX_CEIL   0.80f
#define SPLASH_WAIT_MARK  0.90f

void luna_splash_dex_total(int files, uint64_t bytes)
{
   pthread_mutex_lock(&g_lock);
   g_dex_files = files;
   g_dex_bytes = bytes;
   g_dex_done = 0;
   g_dex_bytes_done = 0;
   g_text_dirty = true;
   pthread_mutex_unlock(&g_lock);
   if (files > 0)
      luna_splash_stage("compiling dex — 0 of %d", files);
}

void luna_splash_dex_loaded(const char *path, uint32_t classes,
                            uint32_t methods, uint64_t bytes)
{
   const char *base = path ? strrchr(path, '/') : NULL;
   base = base ? base + 1 : (path ? path : "dex");

   pthread_mutex_lock(&g_lock);
   ++g_dex_done;
   g_dex_bytes_done += bytes;
   g_classes += classes;
   g_methods += methods;
   int done = g_dex_done, total = g_dex_files;
   uint32_t all_classes = g_classes, all_methods = g_methods;
   float f = SPLASH_DEX_FLOOR;
   if (g_dex_bytes)
      f += (SPLASH_DEX_CEIL - SPLASH_DEX_FLOOR) *
           ((float)g_dex_bytes_done / (float)g_dex_bytes);
   if (f > g_fraction) g_fraction = f;
   snprintf(g_counts, sizeof g_counts,
            "%u classes · %u methods · %.1f MB of bytecode",
            all_classes, all_methods, (double)g_dex_bytes_done / (1024.0 * 1024.0));
   g_text_dirty = true;
   pthread_mutex_unlock(&g_lock);

   if (total > 0)
      luna_splash_stage("compiling %s — %d of %d", base, done, total);
   else
      luna_splash_stage("compiling %s", base);
}

void luna_splash_progress(float fraction)
{
   pthread_mutex_lock(&g_lock);
   if (fraction > g_fraction) { g_fraction = fraction; g_text_dirty = true; }
   pthread_mutex_unlock(&g_lock);
   luna_splash_pump();
}

void luna_splash_end(const char *why)
{
   pthread_mutex_lock(&g_lock);
   if (!g_up || g_finished) { pthread_mutex_unlock(&g_lock); return; }
   /* Set first, so a pump that is about to start sees the screen is over and
    * returns without presenting; the lock below then only has to wait for one
    * that had already begun. */
   g_finished = true;
   pthread_mutex_unlock(&g_lock);
   pthread_mutex_lock(&g_pump_lock);
   pthread_mutex_unlock(&g_pump_lock);
   luna_overlay_set_frame_handler(NULL);
   luna_overlay_set_document(NULL, NULL);
   fprintf(stderr, "[splash] boot screen down after %lu frames (%s)\n",
           g_frames, why ? why : "done");
}

void luna_splash_pump(void)
{
   if (!g_present || !luna_splash_active()) return;
   /* At most one frame per ~16 ms.  The dex loader calls this between files
    * and the SVC dispatch calls it constantly; without the gate the second
    * would spend the boot rendering the splash instead of running the guest. */
   static double last;
   if (pthread_mutex_trylock(&g_pump_lock) != 0) return;
   double now = splash_now();
   /* Re-check under the lock: luna_splash_end() may have run between the test
    * above and here, and it is waiting on exactly this lock. */
   if (!luna_splash_active() || now - last < 0.016) {
      pthread_mutex_unlock(&g_pump_lock);
      return;
   }
   last = now;
   ++g_frames;
   g_present();
   pthread_mutex_unlock(&g_pump_lock);
}
