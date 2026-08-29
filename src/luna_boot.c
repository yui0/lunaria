/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See luna_boot.h for what this card is and why it exists.
 *
 * The document is published once and then never republished.  That is not an
 * optimisation: luna-ui restarts every @keyframes timeline when a document is
 * parsed, so the old card — which rewrote its whole HTML and CSS on every
 * percent tick — would have reset the moon to the horizon several times a
 * second.  What changes (the two status lines and the width of the bar) is
 * pushed straight into the live DOM from the presenting thread through
 * luna_set_text() and a class swap, which leaves the animations running and
 * costs no reparse at all.
 */

/* The engine is compiled into luna_overlay.c, the one translation unit that
 * defines LUNA_UI_IMPLEMENTATION; this file only calls it. */
#define LUNA_UI_NO_PLATFORM
#include "luna-ui.h"

#include "luna_boot.h"
#include "luna_overlay.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- state ------------------------------------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool     g_up;             /* the document has been published */
static bool     g_done;           /* …and taken down for good */
static bool     g_text_dirty = true;
static char     g_jit_line[160];
static char     g_dex_line[160];
static double   g_jit_pct;        /* asymptotic, from compile counts */
static double   g_dex_pct;        /* real, from bytes of dex parsed */
static int      g_dex_files, g_dex_done;
static uint64_t g_dex_bytes, g_dex_bytes_done;
static uint32_t g_classes, g_methods;

static double boot_now(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool boot_ui_wanted(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("LUNARIA_JIT_UI");
      /* Default on: a blank minute looks broken.  LUNARIA_JIT_UI=0 disables. */
      cached = (!e || !*e || *e != '0') ? 1 : 0;
   }
   return cached != 0;
}

/* ---- the page ---------------------------------------------------------- *
 *
 * A moon, its halo, a star field, the wordmark, and the two progress lines.
 * Every moving part is a CSS animation: luna-ui interpolates opacity,
 * transform scale and transform translate across @keyframes stops, so the moon
 * rises and settles once, the halo breathes, the terminator sweeps the disc
 * for as long as the boot lasts, and the stars twinkle out of phase with one
 * another.
 *
 * The bar's fill is width-by-class rather than an inline style: swapping a
 * class on a live element restyles just that element, where rewriting the
 * document would restart the animations above.  Twenty-one steps is one per
 * five percent, finer than the eye reads on a bar this size.
 */
static const char *boot_html(void)
{
   return
   "<body>"
     "<div class=\"sky\"></div>"
     "<div class=\"stars\">"
       "<div class=\"star a\"></div><div class=\"star b\"></div>"
       "<div class=\"star c\"></div><div class=\"star d\"></div>"
       "<div class=\"star e\"></div><div class=\"star f\"></div>"
       "<div class=\"star g\"></div><div class=\"star h\"></div>"
       "<div class=\"star i\"></div><div class=\"star j\"></div>"
       "<div class=\"star k\"></div><div class=\"star l\"></div>"
     "</div>"
     "<div class=\"cometpath\"><div class=\"comet\"></div></div>"
     "<div class=\"moonwrap\">"
       "<div class=\"pulse w1\"></div>"
       "<div class=\"pulse w2\"></div>"
       "<div class=\"pulse w3\"></div>"
       "<div class=\"halo\"></div>"
       "<div class=\"moon\">"
         "<div class=\"crater k1\"></div>"
         "<div class=\"crater k2\"></div>"
         "<div class=\"crater k3\"></div>"
         "<div class=\"crater k4\"></div>"
         "<div class=\"terminator\"></div>"
       "</div>"
       "<div class=\"sat\"></div>"
     "</div>"
     "<div class=\"wordmark\">LUNARIA</div>"
     "<div class=\"panel\">"
       "<div id=\"jit\" class=\"line\">translating ARM</div>"
       "<div class=\"bar\">"
         "<div id=\"fill\" class=\"fill p0\"></div>"
         "<div class=\"sheen\"></div>"
       "</div>"
       "<div id=\"dex\" class=\"sub\"></div>"
       "<div class=\"hint\">first run compiles · later launches reuse the cache</div>"
     "</div>"
   "</body>";
}

/* luna-ui interpolates four things across @keyframes stops: opacity,
 * transform scale, transform translate, and width/left.  No rotation, and at
 * most eight stops per animation.  Every effect below is built out of just
 * those, and one animation per element — an element cannot carry two, which is
 * why the moon's one-shot entrance and the continuous motion around it live on
 * different elements. */
static const char *boot_css(void)
{
   return
   /* The card owns the whole surface here: there is no guest frame under it
    * yet, so this is the only thing between the moon and the uninitialised
    * back buffer (luna_overlay clears to its ink first for the same reason). */
   "body{margin:0;background:#05060d;color:#e8eefc;font-size:15px;}"
   ".sky{position:fixed;left:-10%;top:-10%;width:120%;height:120%;"
     "background:radial-gradient(circle at 50% 34%, #16204a 0%, "
     "#0a0e22 46%, #05060d 100%);"
     "animation:drift 26s ease-in-out infinite alternate;}"
   /* Slow enough not to be seen moving, large enough that the backdrop is
    * never quite the same twice.  A perfectly still field is what makes a
    * loading screen feel frozen. */
   "@keyframes drift{0%{transform:scale(1.0) translate(0px,0px);}"
     "100%{transform:scale(1.08) translate(-14px,10px);}}"

   /* --- star field ---------------------------------------------------- */
   ".star{position:absolute;width:3px;height:3px;border-radius:3px;"
     "background:#cfe0ff;opacity:0.15;"
     "animation:twinkle 2.6s ease-in-out infinite alternate;}"
   ".stars{position:fixed;left:0;top:0;width:100%;height:100%;}"
   ".star.a{left:14%;top:18%;}"
   ".star.b{left:27%;top:41%;animation-delay:0.4s;}"
   ".star.c{left:38%;top:12%;animation-delay:0.9s;}"
   ".star.d{left:63%;top:22%;animation-delay:1.3s;}"
   ".star.e{left:76%;top:37%;animation-delay:0.2s;}"
   ".star.f{left:86%;top:15%;animation-delay:1.7s;}"
   ".star.g{left:8%;top:57%;animation-delay:2.1s;}"
   ".star.h{left:92%;top:56%;animation-delay:0.7s;}"
   ".star.i{left:20%;top:72%;animation-delay:1.1s;}"
   ".star.j{left:47%;top:8%;animation-delay:2.4s;}"
   ".star.k{left:69%;top:66%;animation-delay:1.5s;}"
   ".star.l{left:57%;top:79%;animation-delay:0.05s;}"
   "@keyframes twinkle{0%{opacity:0.08;transform:scale(0.6);}"
     "100%{opacity:0.9;transform:scale(1.35);}}"

   /* --- a comet, once every eleven seconds ----------------------------- *
    * Off screen and transparent for most of the timeline; the whole crossing
    * is the middle tenth.  A rare event does more for "this is alive" than
    * anything continuous, precisely because it is not expected. */
   ".cometpath{position:fixed;left:6%;top:9%;width:0;height:0;}"
   ".comet{position:absolute;left:0;top:0;width:64px;height:2px;"
     "border-radius:2px;opacity:0;"
     "background:linear-gradient(90deg, rgba(207,224,255,0.0) 0%, "
     "rgba(207,224,255,0.9) 82%, rgba(255,255,255,1.0) 100%);"
     "animation:comet 11s ease-in-out infinite;}"
   "@keyframes comet{0%{opacity:0;transform:translate(0px,0px) scale(0.6);}"
     "70%{opacity:0;transform:translate(0px,0px) scale(0.6);}"
     "74%{opacity:0.95;transform:translate(60px,26px) scale(1.0);}"
     "84%{opacity:0.95;transform:translate(430px,186px) scale(1.0);}"
     "90%{opacity:0;transform:translate(560px,242px) scale(0.8);}"
     "100%{opacity:0;transform:translate(560px,242px) scale(0.8);}}"

   /* --- the moon ------------------------------------------------------- */
   ".moonwrap{position:fixed;left:50%;top:33%;width:0;height:0;}"
   /* Three rings leaving the moon on a stagger — the emulator is reaching
    * outwards, and unlike a spinner it never returns to where it started. */
   ".pulse{position:absolute;left:-70px;top:-70px;width:140px;height:140px;"
     "border-radius:140px;border:2px solid rgba(150,182,255,0.55);"
     "opacity:0;animation:ripple 4.2s ease-out infinite;}"
   ".w2{animation-delay:1.4s;}"
   ".w3{animation-delay:2.8s;}"
   "@keyframes ripple{0%{opacity:0.0;transform:scale(0.80);}"
     "12%{opacity:0.55;transform:scale(0.90);}"
     "100%{opacity:0.0;transform:scale(2.05);}}"
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
     "animation:moonrise 1.9s ease-in-out;}"
   /* Rises from below its resting place, overshoots, and settles — one pass,
    * not a loop: a logo that keeps re-entering reads as a stutter. */
   "@keyframes moonrise{0%{opacity:0;transform:translate(0px,58px) scale(0.60);}"
     "45%{opacity:1;transform:translate(0px,-12px) scale(1.10);}"
     "68%{opacity:1;transform:translate(0px,4px) scale(0.97);}"
     "85%{opacity:1;transform:translate(0px,-2px) scale(1.01);}"
     "100%{opacity:1;transform:translate(0px,0px) scale(1.0);}}"
   ".crater{position:absolute;border-radius:40px;background:#c7c9d6;"
     "opacity:0.55;}"
   ".k1{left:26px;top:30px;width:22px;height:22px;}"
   ".k2{left:60px;top:56px;width:15px;height:15px;}"
   ".k3{left:37px;top:72px;width:10px;height:10px;}"
   ".k4{left:72px;top:26px;width:8px;height:8px;}"
   /* The night side of the disc, sliding across it: the moon goes through its
    * phases for as long as the boot lasts. */
   ".terminator{position:absolute;left:0;top:0;width:112px;height:112px;"
     "border-radius:112px;"
     "background:linear-gradient(90deg, rgba(5,6,13,0.0) 0%, "
     "rgba(5,6,13,0.72) 46%, rgba(5,6,13,0.90) 100%);"
     "animation:phase 9.0s ease-in-out infinite alternate;}"
   "@keyframes phase{0%{transform:translate(-104px,0px);opacity:0.15;}"
     "100%{transform:translate(58px,0px);opacity:0.95;}}"
   /* A satellite going round.  There is no rotation to animate, so the orbit
    * is eight translate stops around the circle; the scale and opacity dip
    * over the far half is what reads as depth rather than a dot on a string. */
   ".sat{position:absolute;left:-4px;top:-4px;width:8px;height:8px;"
     "border-radius:8px;background:#dfe8ff;"
     "box-shadow:0 0 10px rgba(190,214,255,0.9);"
     "animation:orbit 8.5s linear infinite;}"
   "@keyframes orbit{"
     "0%{transform:translate(96px,0px) scale(1.15);opacity:1.0;}"
     "14%{transform:translate(60px,-75px) scale(1.0);opacity:0.9;}"
     "28%{transform:translate(-21px,-94px) scale(0.75);opacity:0.45;}"
     "42%{transform:translate(-87px,-41px) scale(0.62);opacity:0.3;}"
     "57%{transform:translate(-87px,41px) scale(0.62);opacity:0.3;}"
     "71%{transform:translate(-21px,94px) scale(0.75);opacity:0.45;}"
     "85%{transform:translate(60px,75px) scale(1.0);opacity:0.9;}"
     "100%{transform:translate(96px,0px) scale(1.15);opacity:1.0;}}"

   /* --- wordmark, and the light that crosses it ------------------------ */
   /* A light sweeping across the wordmark is deliberately absent.  Given a
    * `position:fixed` parent carrying a margin, luna-ui placed the absolutely
    * positioned highlight against the page instead of against that parent, so
    * it left its clip box and sat in the sky as a grey rectangle.  The
    * progress track below gets the same effect correctly, clipping from a
    * `position:relative` parent — which is the shape to copy if this is ever
    * wanted on the wordmark. */
   /* The entrances are staggered by animation-delay, which does work — an
    * earlier version of this sheet said it did not and dropped the stagger.
    * That was wrong: what actually happened was that the card stopped being
    * presented a second in, so every animation on it froze wherever it had
    * got to, and the two delayed ones happened to freeze before they became
    * visible.  See boot_card_pump() in arm_exec.cpp for the real fault. */
   ".wordmark{position:fixed;left:0;top:51%;width:100%;text-align:center;"
     "font-size:30px;letter-spacing:9px;color:#dfe8ff;opacity:0;"
     "animation:fadein 1.2s ease-in-out;animation-delay:0.6s;}"
   "@keyframes fadein{0%{opacity:0;transform:translate(0px,14px) scale(0.94);}"
     "100%{opacity:1;transform:translate(0px,0px) scale(1.0);}}"

   /* --- the two progress lines ----------------------------------------- */
   ".panel{position:fixed;left:50%;top:63%;width:520px;"
     "margin-left:-260px;opacity:0;"
     "animation:fadein 1.0s ease-in-out;animation-delay:1.0s;}"
   ".line{width:520px;text-align:center;font-size:15px;color:#9fb2d8;"
     "padding-bottom:10px;}"
   ".bar{position:relative;width:520px;height:5px;border-radius:5px;"
     "background:rgba(159,178,216,0.16);overflow:hidden;}"
   ".fill{position:absolute;left:0;top:0;height:5px;border-radius:5px;"
     "background:linear-gradient(90deg, #6f8fe8 0%, #cfe0ff 100%);}"
   /* A streak crossing the track.  The fill only moves when a phase reports;
    * between those the bar would be a still object, and a still progress bar
    * is the thing people read as hung. */
   ".sheen{position:absolute;left:0;top:0;width:120px;height:5px;"
     "background:linear-gradient(90deg, rgba(207,224,255,0.0) 0%, "
     "rgba(207,224,255,0.55) 50%, rgba(207,224,255,0.0) 100%);"
     "animation:slide 2.1s ease-in-out infinite;}"
   "@keyframes slide{0%{transform:translate(-120px,0px);opacity:0.0;}"
     "20%{opacity:1.0;}"
     "80%{opacity:1.0;}"
     "100%{transform:translate(520px,0px);opacity:0.0;}}"
   ".sub{width:520px;text-align:center;padding-top:10px;font-size:13px;"
     "color:#66759a;}"
   ".hint{width:520px;text-align:center;padding-top:16px;font-size:12px;"
     "color:#455168;}"
   /* One rule per five percent; swapped on the live element. */
   ".fill.p0{width:0px;}"    ".fill.p5{width:26px;}"   ".fill.p10{width:52px;}"
   ".fill.p15{width:78px;}"  ".fill.p20{width:104px;}" ".fill.p25{width:130px;}"
   ".fill.p30{width:156px;}" ".fill.p35{width:182px;}" ".fill.p40{width:208px;}"
   ".fill.p45{width:234px;}" ".fill.p50{width:260px;}" ".fill.p55{width:286px;}"
   ".fill.p60{width:312px;}" ".fill.p65{width:338px;}" ".fill.p70{width:364px;}"
   ".fill.p75{width:390px;}" ".fill.p80{width:416px;}" ".fill.p85{width:442px;}"
   ".fill.p90{width:468px;}" ".fill.p95{width:494px;}" ".fill.p100{width:520px;}";
}

/* ---- pushing state into the live document ------------------------------ */

/* Called on the presenting thread by luna_overlay, with the document parsed.
 * Everything here mutates existing elements; nothing reparses. */
static void boot_frame(void)
{
   char jit[160], dex[160];
   int step;
   pthread_mutex_lock(&g_lock);
   if (!g_text_dirty) { pthread_mutex_unlock(&g_lock); return; }
   g_text_dirty = false;
   snprintf(jit, sizeof jit, "%s", g_jit_line);
   snprintf(dex, sizeof dex, "%s", g_dex_line);
   /* The bar is whichever phase has got further.  They overlap — the dex
    * files are compiled while dynarmic is still translating — and a bar that
    * jumped back when the other source reported would read as a fault. */
   double pct = g_jit_pct > g_dex_pct ? g_jit_pct : g_dex_pct;
   pthread_mutex_unlock(&g_lock);
   if (pct < 0.0) pct = 0.0;
   if (pct > 100.0) pct = 100.0;
   step = (int)(pct / 5.0 + 0.5) * 5;

   int i = luna_get_element_by_id("jit");
   if (i >= 0) luna_set_text(i, jit);
   i = luna_get_element_by_id("dex");
   if (i >= 0) luna_set_text(i, dex);

   i = luna_get_element_by_id("fill");
   if (i >= 0) {
      /* The previous step's class has to go, or two width rules compete and
       * the one the sheet lists last wins regardless of which is current. */
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

/* Publishes the document, once.  Everything after this is a live mutation. */
static void boot_show(void)
{
   if (g_up) return;
   g_up = true;
   luna_overlay_set_frame_handler(boot_frame);
   luna_overlay_set_status(boot_html(), boot_css());
}

/* ---- what the emulator calls ------------------------------------------- */

bool luna_boot_jit_update(uint64_t compiles, uint64_t compile_ns)
{
   static uint64_t last_compiles;
   static double last_compile_time;
   static double shown_since;

   if (!boot_ui_wanted() || g_done) return false;

   const double now = boot_now();
   if (compiles != last_compiles) {
      last_compiles = compiles;
      last_compile_time = now;
   }

   /* Ignore tiny bursts (a handful of blocks at a JNI boundary).  Show once
    * translation has clearly become the thing the process is doing. */
   const double compile_s = (double)compile_ns / 1e9;
   const bool busy = (compiles >= 256 && compile_s >= 0.4) ||
                     (compiles >= 64 && now - last_compile_time < 0.35);
   /* The dex phase keeps the card up on its own: the two overlap, and a card
    * that dismissed itself the moment dynarmic paused would flash off in the
    * middle of compiling classes3.dex. */
   pthread_mutex_lock(&g_lock);
   const bool dex_running = g_dex_files > 0 && g_dex_done < g_dex_files;
   pthread_mutex_unlock(&g_lock);
   const bool idle = g_up && !dex_running && (now - last_compile_time) > 1.25;

   if (!busy && !g_up) return false;
   if (idle) {
      luna_boot_finish("translation went idle");
      return false;
   }

   if (!g_up) shown_since = now;
   boot_show();

   /* Asymptotic fill: approaches ~95% as compiles grow, never claims 100%
    * while still translating.  Reads as progress without a fake total. */
   double pct = 100.0 * (1.0 - exp(-(double)compiles / 35000.0));
   if (pct > 95.0) pct = 95.0;
   if (pct < 2.0) pct = 2.0;

   const double elapsed = now - shown_since;
   const double rate = elapsed > 0.05 ? (double)compiles / elapsed : 0.0;

   char line[160];
   snprintf(line, sizeof line, "translating ARM · %llu blocks · %.1f s · %.0f /s",
            (unsigned long long)compiles, compile_s, rate);

   pthread_mutex_lock(&g_lock);
   /* Only wake the frame hook when something a reader would notice moved:
    * the counters tick far faster than the eye, and marking dirty every time
    * would put a text remeasure on every presented frame. */
   if (strcmp(g_jit_line, line) != 0 || fabs(pct - g_jit_pct) >= 1.0) {
      snprintf(g_jit_line, sizeof g_jit_line, "%s", line);
      g_jit_pct = pct;
      g_text_dirty = true;
   }
   pthread_mutex_unlock(&g_lock);
   return true;
}

/* The dex phase is measured in bytes, not files: this title's four dexes
 * differ by 3 MB, and counting files would move the bar in uneven jumps. */
void luna_boot_dex_total(int files, uint64_t bytes)
{
   if (!boot_ui_wanted() || g_done || files <= 0) return;
   pthread_mutex_lock(&g_lock);
   g_dex_files = files;
   g_dex_bytes = bytes;
   g_dex_done = 0;
   g_dex_bytes_done = 0;
   snprintf(g_dex_line, sizeof g_dex_line, "compiling dex — 0 of %d", files);
   g_text_dirty = true;
   pthread_mutex_unlock(&g_lock);
   boot_show();
}

void luna_boot_dex_loaded(const char *path, uint32_t classes,
                          uint32_t methods, uint64_t bytes)
{
   if (!boot_ui_wanted() || g_done) return;
   const char *base = path ? strrchr(path, '/') : NULL;
   base = base ? base + 1 : (path ? path : "dex");

   pthread_mutex_lock(&g_lock);
   ++g_dex_done;
   g_dex_bytes_done += bytes;
   g_classes += classes;
   g_methods += methods;
   if (g_dex_bytes)
      g_dex_pct = 95.0 * ((double)g_dex_bytes_done / (double)g_dex_bytes);
   if (g_dex_files > 0)
      snprintf(g_dex_line, sizeof g_dex_line,
               "compiling %s — %d of %d · %u classes · %u methods · %.1f MB",
               base, g_dex_done, g_dex_files, g_classes, g_methods,
               (double)g_dex_bytes_done / (1024.0 * 1024.0));
   else
      snprintf(g_dex_line, sizeof g_dex_line, "compiling %s", base);
   g_text_dirty = true;
   pthread_mutex_unlock(&g_lock);
   boot_show();
}

bool luna_boot_active(void)
{
   return g_up && !g_done;
}

void luna_boot_finish(const char *why)
{
   if (!g_up || g_done) { g_done = true; return; }
   g_done = true;
   luna_overlay_set_frame_handler(NULL);
   luna_overlay_set_status(NULL, NULL);
   fprintf(stderr, "[boot] card down (%s)\n", why ? why : "done");
}
