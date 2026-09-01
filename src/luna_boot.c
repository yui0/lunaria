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
 * parsed.  What changes (the two status lines, percentage, and progress arc)
 * is pushed straight into the live DOM from the presenting thread through
 * luna_set_text() and luna_css_set_variables().  The snowfall keeps running
 * and a progress update costs no stylesheet parse.
 */

/* The engine is compiled into luna_overlay.c, the one translation unit that
 * defines LUNA_UI_IMPLEMENTATION; this file only calls it. */
#define LUNA_UI_NO_PLATFORM
#include "luna-ui.h"

#include "luna_boot.h"
#include "luna_overlay.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- state ------------------------------------------------------------- *
 *
 * Dex updates arrive on the loader thread; JIT updates on a dynarmic engine
 * thread; the frame hook reads on the presenter.  A pthread mutex was the
 * previous answer and kept showing up in deadlock hunts after every lock
 * change elsewhere.  Each progress line has its own seqlock (writer bumps
 * odd→write→even; reader retries on a torn snapshot).  The two lines never
 * share a writer, so they need no cross-lock. */

static atomic_bool g_up;             /* the document has been published */
static atomic_bool g_done;           /* …and taken down for good */
static atomic_bool g_text_dirty = true;

static atomic_uint g_jit_seq;
static char         g_jit_line[160];
static atomic_uint  g_jit_pct100;    /* 0..10000 = 0.00%..100.00% */

static atomic_uint g_dex_seq;
static char         g_dex_line[160];
static atomic_uint  g_dex_pct100;

static atomic_int    g_dex_files, g_dex_done;
static atomic_ullong g_dex_bytes, g_dex_bytes_done;
static atomic_uint   g_classes, g_methods;

static int           g_ring_pct = -1; /* last --progress pushed into CSS */

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

static void boot_write_begin(atomic_uint *seq)
{
   atomic_fetch_add_explicit(seq, 1u, memory_order_relaxed); /* odd */
   atomic_thread_fence(memory_order_release);
}

static void boot_write_end(atomic_uint *seq)
{
   atomic_thread_fence(memory_order_release);
   atomic_fetch_add_explicit(seq, 1u, memory_order_release); /* even */
   atomic_store_explicit(&g_text_dirty, true, memory_order_release);
}

static void boot_read_line(atomic_uint *seq, const char *src, char *dst,
                           size_t dstsz, atomic_uint *pct_src, unsigned *pct100)
{
   for (;;) {
      unsigned s = atomic_load_explicit(seq, memory_order_acquire);
      if (s & 1u) continue;
      snprintf(dst, dstsz, "%s", src);
      if (pct100 && pct_src)
         *pct100 = atomic_load_explicit(pct_src, memory_order_relaxed);
      atomic_thread_fence(memory_order_acquire);
      if (s == atomic_load_explicit(seq, memory_order_acquire)) return;
   }
}

/* ---- the page ---------------------------------------------------------- *
 *
 * A CSS-only snowfall splash (splash/snow2.html), a determinate circular
 * progress indicator, the wordmark, and two progress lines.  No image assets
 * are loaded at runtime.
 *
 * The ring's conic fill is driven by the live custom property --progress so
 * the arc can advance every frame without republishing the document or
 * swapping twenty-one progress classes.
 */
static const char *boot_html(void)
{
   return
   "<body>"
     "<div class=\"snow\">"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
       "<div class=\"flake\"></div><div class=\"flake\"></div>"
     "</div>"
     "<div class=\"hero\">"
       "<div id=\"ring\" class=\"ring\">"
         "<div class=\"ring-core\">"
           "<div id=\"pct\" class=\"pct\">0%</div>"
           "<div class=\"ring-label\">LOADING</div>"
         "</div>"
       "</div>"
     "</div>"
     "<div class=\"copy\">"
       "<div class=\"wordmark\">LUNARIA</div>"
       "<div id=\"jit\" class=\"line\">starting</div>"
       "<div id=\"dex\" class=\"sub\"></div>"
     "</div>"
   "</body>";
}

/* The card is deliberately quiet.  Falling snow says the process is alive;
 * --progress says how far it has progressed.  RGB tokens are used because
 * luna-ui's compact CSS parser deliberately implements sRGB/rgba rather than
 * OKLCH. */
static const char *boot_css(void)
{
   return
   "/* Hallmark · component: boot splash · genre: atmospheric · "
     "theme: snow2 winter sky\n"
   " * pre-emit critique: P5 H5 E4 S5 R5 V5 */"
   ":root{--progress:0%;--arc:#ffffff;--track:rgba(255,255,255,0.30);"
     "--sky-top:#b7e7fc;--sky-bot:#00a3ef;--ink:#ffffff;"
     "--muted:rgba(255,255,255,0.88);--quiet:rgba(255,255,255,0.62);"
     "--core:rgba(0,120,210,0.55);}"
   "body{margin:0;overflow:hidden;color:var(--ink);font-size:15px;"
     "background:linear-gradient(to top,var(--sky-top) 0%,var(--sky-bot) 100%);}"

   ".snow{position:fixed;left:0;top:0;width:100%;height:100%;"
     "pointer-events:none;overflow:hidden;}"
   ".flake{position:absolute;border-radius:50%;"
     "background-image:"
     "linear-gradient(180deg,rgba(255,255,255,0) 30%,#fff 50%,#fff 60%,"
     "rgba(255,255,255,0) 60%),"
     "linear-gradient(90deg,rgba(255,255,255,0) 30%,#fff 50%,#fff 60%,"
     "rgba(255,255,255,0) 60%),"
     "linear-gradient(45deg,rgba(255,255,255,0) 33%,#fff 53%,#fff 57%,"
     "rgba(255,255,255,0) 65%),"
     "linear-gradient(135deg,rgba(255,255,255,0) 33%,#fff 53%,#fff 57%,"
     "rgba(255,255,255,0) 65%);}"
   "@keyframes flakes{100%{transform:translateY(1000px) rotateX(48deg) "
     "rotateY(15deg);opacity:0;}}"

   ".flake:nth-child(1){width:19px;height:19px;top:-610px;left:6%;opacity:0.65;"
     "filter:blur(3px);animation:55s flakes linear infinite;}"
   ".flake:nth-child(2){width:11px;height:11px;top:-634px;left:50%;opacity:0.6;"
     "filter:blur(4px);animation:42s flakes linear infinite;}"
   ".flake:nth-child(3){width:10px;height:10px;top:-602px;left:32%;opacity:0.6;"
     "filter:blur(3px);animation:18s flakes linear infinite;}"
   ".flake:nth-child(4){width:8px;height:8px;top:-409px;left:68%;opacity:0.97;"
     "filter:blur(3px);animation:38s flakes linear infinite;}"
   ".flake:nth-child(5){width:10px;height:10px;top:-70px;left:17%;opacity:0.95;"
     "filter:blur(3px);animation:45s flakes linear infinite;}"
   ".flake:nth-child(6){width:10px;height:10px;top:-591px;left:10%;opacity:0.87;"
     "filter:blur(3px);animation:67s flakes linear infinite;}"
   ".flake:nth-child(7){width:20px;height:20px;top:-123px;left:10%;opacity:0.92;"
     "filter:blur(3px);animation:67s flakes linear infinite;}"
   ".flake:nth-child(8){width:19px;height:19px;top:-361px;left:56%;opacity:0.55;"
     "filter:blur(3px);animation:44s flakes linear infinite;}"
   ".flake:nth-child(9){width:18px;height:18px;top:-6px;left:71%;opacity:0.78;"
     "filter:blur(3px);animation:42s flakes linear infinite;}"
   ".flake:nth-child(10){width:13px;height:13px;top:-619px;left:40%;opacity:0.79;"
     "filter:blur(3px);animation:28s flakes linear infinite;}"
   ".flake:nth-child(11){width:8px;height:8px;top:-25px;left:34%;opacity:0.73;"
     "filter:blur(3px);animation:32s flakes linear infinite;}"
   ".flake:nth-child(12){width:19px;height:19px;top:-555px;left:23%;opacity:0.65;"
     "filter:blur(4px);animation:25s flakes linear infinite;}"
   ".flake:nth-child(13){width:17px;height:17px;top:-235px;left:25%;opacity:0.97;"
     "filter:blur(3px);animation:31s flakes linear infinite;}"
   ".flake:nth-child(14){width:14px;height:14px;top:-681px;left:7%;opacity:0.59;"
     "filter:blur(3px);animation:39s flakes linear infinite;}"
   ".flake:nth-child(15){width:17px;height:17px;top:-124px;left:90%;opacity:0.97;"
     "filter:blur(4px);animation:32s flakes linear infinite;}"
   ".flake:nth-child(16){width:11px;height:11px;top:-480px;left:51%;opacity:0.7;"
     "filter:blur(4px);animation:38s flakes linear infinite;}"
   ".flake:nth-child(17){width:20px;height:20px;top:-350px;left:66%;opacity:0.97;"
     "filter:blur(3px);animation:47s flakes linear infinite;}"
   ".flake:nth-child(18){width:16px;height:16px;top:-607px;left:62%;opacity:0.9;"
     "filter:blur(4px);animation:39s flakes linear infinite;}"
   ".flake:nth-child(19){width:12px;height:12px;top:-187px;left:90%;opacity:0.79;"
     "filter:blur(4px);animation:29s flakes linear infinite;}"
   ".flake:nth-child(20){width:15px;height:15px;top:-486px;left:26%;opacity:0.78;"
     "filter:blur(4px);animation:37s flakes linear infinite;}"
   ".flake:nth-child(21){width:20px;height:20px;top:-323px;left:38%;opacity:0.72;"
     "filter:blur(3px);animation:45s flakes linear infinite;}"
   ".flake:nth-child(22){width:16px;height:16px;top:-672px;left:31%;opacity:0.64;"
     "filter:blur(3px);animation:26s flakes linear infinite;}"
   ".flake:nth-child(23){width:11px;height:11px;top:-652px;left:81%;opacity:0.79;"
     "filter:blur(4px);animation:63s flakes linear infinite;}"
   ".flake:nth-child(24){width:13px;height:13px;top:-167px;left:37%;opacity:0.8;"
     "filter:blur(4px);animation:41s flakes linear infinite;}"

   ".hero{position:fixed;left:50%;top:38%;width:140px;height:140px;"
     "margin-left:-70px;margin-top:-70px;}"
   ".ring{position:absolute;left:0;top:0;width:140px;height:140px;"
     "border-radius:999px;"
     "background:conic-gradient(from -90deg at 50% 50%,"
     "var(--arc) 0%,var(--arc) var(--progress),"
     "var(--track) var(--progress),var(--track) 100%);"
     "box-shadow:0 14px 42px rgba(0,48,96,0.28);}"
   ".ring-core{position:absolute;left:11px;top:11px;width:118px;height:118px;"
     "border-radius:999px;background:var(--core);display:flex;"
     "flex-direction:column;align-items:center;justify-content:center;"
     "box-shadow:inset 0 0 0 1px rgba(255,255,255,0.18);}"
   ".pct{font-size:28px;font-weight:700;color:#fff;letter-spacing:-1px;"
     "text-shadow:0 1px 8px rgba(0,48,96,0.35);}"
   ".ring-label{padding-top:4px;font-size:9px;font-weight:700;"
     "letter-spacing:2px;color:var(--quiet);}"

   ".copy{position:fixed;left:50%;top:64%;width:420px;margin-left:-210px;"
     "text-align:center;animation:reveal 0.8s ease-in-out;}"
   "@keyframes reveal{0%{opacity:0;}100%{opacity:1;}}"
   ".wordmark{font-size:25px;font-weight:700;letter-spacing:10px;"
     "color:var(--ink);padding-left:10px;"
     "text-shadow:0 2px 16px rgba(0,48,96,0.25);}"
   ".line{padding-top:19px;font-size:13px;color:var(--muted);"
     "text-shadow:0 1px 8px rgba(0,48,96,0.22);}"
   ".sub{padding-top:7px;font-size:12px;color:var(--quiet);"
     "text-shadow:0 1px 8px rgba(0,48,96,0.22);}"

   "@media (max-width:560px){.hero{transform:scale(0.82);}"
     ".copy{width:300px;margin-left:-150px;}.wordmark{font-size:21px;"
     "letter-spacing:8px;padding-left:8px;}}"
   "@media (prefers-reduced-motion:reduce){.flake,.copy{animation:none;}}";
}

/* ---- pushing state into the live document ------------------------------ */

static void boot_frame(void)
{
   char jit[160], dex[160];
   unsigned jit_p = 0, dex_p = 0;
   int pct_i;
   static char last_jit[160], last_dex[160], last_pct[16];

   /* Snowfall keeps moving from luna_update() alone; gating this handler on
    * g_text_dirty left the ring and status lines frozen while the flakes still
    * fell — exactly the “progress bar stuck at 0%” report on real APK boots. */
   boot_read_line(&g_jit_seq, g_jit_line, jit, sizeof jit, &g_jit_pct100, &jit_p);
   boot_read_line(&g_dex_seq, g_dex_line, dex, sizeof dex, &g_dex_pct100, &dex_p);
   atomic_store_explicit(&g_text_dirty, false, memory_order_relaxed);

   double pct = (jit_p > dex_p ? jit_p : dex_p) / 100.0;
   if (pct < 0.0) pct = 0.0;
   if (pct > 100.0) pct = 100.0;
   pct_i = (int)(pct + 0.5);

   char percent[16];
   snprintf(percent, sizeof percent, "%u%%", (unsigned)pct_i);

   if (strcmp(jit, last_jit) != 0) {
      int i = luna_get_element_by_id("jit");
      if (i >= 0) luna_set_text(i, jit);
      snprintf(last_jit, sizeof last_jit, "%s", jit);
   }
   if (strcmp(dex, last_dex) != 0) {
      int i = luna_get_element_by_id("dex");
      if (i >= 0) luna_set_text(i, dex);
      snprintf(last_dex, sizeof last_dex, "%s", dex);
   }
   if (strcmp(percent, last_pct) != 0) {
      int i = luna_get_element_by_id("pct");
      if (i >= 0) luna_set_text(i, percent);
      snprintf(last_pct, sizeof last_pct, "%s", percent);
   }

   if (g_ring_pct != pct_i) {
      char prog[8];
      LunaCssVariable vars[1];
      int ring = luna_get_element_by_id("ring");
      snprintf(prog, sizeof prog, "%d%%", pct_i);
      vars[0].name = "--progress";
      vars[0].value = prog;
      luna_css_set_variables(vars, 1);
      if (ring >= 0) luna_mark_visual_dirty(ring);
      g_ring_pct = pct_i;
   }
}

static void boot_show(void)
{
   bool was = atomic_exchange_explicit(&g_up, true, memory_order_acq_rel);
   if (was) return;
   g_ring_pct = -1;
   luna_overlay_set_frame_handler(boot_frame);
   luna_overlay_set_status(boot_html(), boot_css());
}

/* ---- what the emulator calls ------------------------------------------- */

bool luna_boot_jit_update(uint64_t compiles, uint64_t compile_ns)
{
   static uint64_t last_compiles;
   static double last_compile_time;
   static double shown_since;

   if (!boot_ui_wanted() || atomic_load_explicit(&g_done, memory_order_acquire))
      return false;

   const double now = boot_now();
   if (compiles != last_compiles) {
      last_compiles = compiles;
      last_compile_time = now;
   }

   const double compile_s = (double)compile_ns / 1e9;
   const bool busy = (compiles >= 256 && compile_s >= 0.4) ||
                     (compiles >= 64 && now - last_compile_time < 0.35);
   const int files = atomic_load_explicit(&g_dex_files, memory_order_relaxed);
   const int done  = atomic_load_explicit(&g_dex_done, memory_order_relaxed);
   const bool dex_running = files > 0 && done < files;
   const bool up = atomic_load_explicit(&g_up, memory_order_acquire);
   const bool idle = up && !dex_running && (now - last_compile_time) > 1.25;

   if (!busy && !up) return false;
   if (idle) {
      luna_boot_finish("translation went idle");
      return false;
   }

   if (!up) shown_since = now;
   boot_show();

   double pct = 100.0 * (1.0 - exp(-(double)compiles / 35000.0));
   if (pct > 95.0) pct = 95.0;
   if (pct < 2.0) pct = 2.0;

   const double elapsed = now - shown_since;
   const double rate = elapsed > 0.05 ? (double)compiles / elapsed : 0.0;

   char line[160];
   snprintf(line, sizeof line, "translating ARM · %llu blocks · %.0f/s",
            (unsigned long long)compiles, rate);
   (void)compile_s;

   boot_write_begin(&g_jit_seq);
   snprintf(g_jit_line, sizeof g_jit_line, "%s", line);
   atomic_store_explicit(&g_jit_pct100, (unsigned)(pct * 100.0 + 0.5),
                         memory_order_relaxed);
   boot_write_end(&g_jit_seq);
   return true;
}

void luna_boot_dex_total(int files, uint64_t bytes)
{
   if (!boot_ui_wanted() || atomic_load_explicit(&g_done, memory_order_acquire)
       || files <= 0)
      return;
   atomic_store_explicit(&g_dex_files, files, memory_order_relaxed);
   atomic_store_explicit(&g_dex_bytes, bytes, memory_order_relaxed);
   atomic_store_explicit(&g_dex_done, 0, memory_order_relaxed);
   atomic_store_explicit(&g_dex_bytes_done, 0, memory_order_relaxed);
   boot_write_begin(&g_dex_seq);
   snprintf(g_dex_line, sizeof g_dex_line, "compiling dex — 0 of %d", files);
   boot_write_end(&g_dex_seq);
   boot_show();
}

void luna_boot_dex_loaded(const char *path, uint32_t classes,
                          uint32_t methods, uint64_t bytes)
{
   if (!boot_ui_wanted() || atomic_load_explicit(&g_done, memory_order_acquire))
      return;
   const char *base = path ? strrchr(path, '/') : NULL;
   base = base ? base + 1 : (path ? path : "dex");

   int done = atomic_fetch_add_explicit(&g_dex_done, 1, memory_order_relaxed) + 1;
   uint64_t bytes_done =
      atomic_fetch_add_explicit(&g_dex_bytes_done, bytes, memory_order_relaxed)
      + bytes;
   unsigned cls = atomic_fetch_add_explicit(&g_classes, classes,
                                            memory_order_relaxed) + classes;
   unsigned meth = atomic_fetch_add_explicit(&g_methods, methods,
                                             memory_order_relaxed) + methods;
   uint64_t total = atomic_load_explicit(&g_dex_bytes, memory_order_relaxed);
   int files = atomic_load_explicit(&g_dex_files, memory_order_relaxed);
   unsigned pct100 = 0;
   if (total)
      pct100 = (unsigned)(9500.0 * ((double)bytes_done / (double)total) + 0.5);

   boot_write_begin(&g_dex_seq);
   if (files > 0)
      snprintf(g_dex_line, sizeof g_dex_line,
               "%s · %d of %d · %u classes · %.1f MB",
               base, done, files, cls,
               (double)bytes_done / (1024.0 * 1024.0));
   else
      snprintf(g_dex_line, sizeof g_dex_line, "compiling %s", base);
   (void)meth;   /* counted for the log; the card has no room for it */
   atomic_store_explicit(&g_dex_pct100, pct100, memory_order_relaxed);
   boot_write_end(&g_dex_seq);
   boot_show();
}

bool luna_boot_active(void)
{
   return atomic_load_explicit(&g_up, memory_order_acquire)
       && !atomic_load_explicit(&g_done, memory_order_acquire);
}

void luna_boot_finish(const char *why)
{
   if (!atomic_load_explicit(&g_up, memory_order_acquire)
       || atomic_load_explicit(&g_done, memory_order_acquire)) {
      atomic_store_explicit(&g_done, true, memory_order_release);
      return;
   }
   atomic_store_explicit(&g_done, true, memory_order_release);
   luna_overlay_set_frame_handler(NULL);
   luna_overlay_set_status(NULL, NULL);
   fprintf(stderr, "[boot] card down (%s)\n", why ? why : "done");
}
