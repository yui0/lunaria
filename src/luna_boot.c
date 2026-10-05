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
 * luna_set_text() and luna_update_classes().  The snowfall keeps running
 * and a progress update costs no stylesheet parse.
 */

/* The engine is compiled into luna_overlay.c, the one translation unit that
 * defines LUNA_UI_IMPLEMENTATION; this file only calls it. */
#define GLFW_INCLUDE_NONE
#define LUNA_UI_NO_PLATFORM
#include "luna-ui.h"
#include "luna-window.h"

#include "luna_boot.h"
#include "luna_overlay.h"
#include "lunaria_os.h"
#include "jvm/jvm.h"
#include <GLFW/glfw3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

static int           g_ring_pct = -1; /* last ring.pN class pushed */
static double        g_last_compile_time; /* last dynarmic block compiled */
static double        g_shown_since;   /* boot_show() timestamp */
static unsigned      g_dex_pct0;      /* dex % when the card first appeared */
static atomic_uint   g_guest_swaps;   /* eglSwapBuffers since the card went up */
static double        g_displayed_pct; /* monotonic % shown on the ring */
static bool          g_installing;
static char          g_last_jit[160], g_last_dex[160], g_last_pct[16];

static void boot_document_reset(void)
{
   g_ring_pct = -1;
   g_last_jit[0] = g_last_dex[0] = g_last_pct[0] = 0;
}

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
 * The ring's conic fill is driven by ring.p0 … ring.p100 classes so the arc
 * advances every frame without republishing the document.  (Live --progress
 * custom properties did not repaint reliably through the overlay's GLES path.)
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
       "<div id=\"ring\" class=\"ring p0\">"
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
 * the ring arc says how far it has progressed.  RGB tokens are used because
 * luna-ui's compact CSS parser deliberately implements sRGB/rgba rather than
 * OKLCH. */
static const char boot_css_base[] =
   "/* Hallmark · component: boot splash · genre: atmospheric · "
     "theme: snow2 winter sky\n"
   " * pre-emit critique: P5 H5 E4 S5 R5 V5 */"
   ":root{--sky-top:#b7e7fc;--sky-bot:#00a3ef;--ink:#ffffff;"
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

   ".hero{position:fixed;left:50%;top:41%;width:140px;height:140px;"
     "margin-left:-70px;margin-top:-70px;}"
   ".ring{position:absolute;left:0;top:0;width:140px;height:140px;"
     "border-radius:999px;"
     "background:conic-gradient(from -90deg at 50% 50%,"
     "rgba(255,255,255,0.22) 0%,rgba(255,255,255,0.22) 100%);"
     "box-shadow:0 14px 42px rgba(0,48,96,0.28);}"
   ".ring-core{position:absolute;left:11px;top:11px;width:118px;height:118px;"
     "border-radius:999px;background:var(--core);display:flex;"
     "flex-direction:column;align-items:center;justify-content:center;"
     "box-shadow:inset 0 0 0 1px rgba(255,255,255,0.18);}"
   ".pct{font-size:28px;font-weight:700;color:#fff;letter-spacing:-1px;"
     "text-shadow:0 1px 8px rgba(0,48,96,0.35);}"
   ".ring-label{padding-top:4px;font-size:9px;font-weight:700;"
     "letter-spacing:2px;color:var(--quiet);}"

   ".copy{position:fixed;left:50%;top:54%;width:460px;margin-left:-230px;"
     "text-align:center;opacity:1;display:flex;flex-direction:column;"
     "align-items:center;}"
   ".wordmark{width:100%;font-size:25px;font-weight:700;letter-spacing:10px;"
     "color:var(--ink);text-align:center;"
     "text-shadow:0 2px 16px rgba(0,48,96,0.25);}"
   ".line{padding-top:14px;width:100%;font-size:13px;color:var(--muted);"
     "text-align:center;line-height:1.45;"
     "text-shadow:0 1px 8px rgba(0,48,96,0.22);}"
   ".sub{padding-top:5px;width:100%;font-size:12px;color:var(--quiet);"
     "text-align:center;line-height:1.45;"
     "text-shadow:0 1px 8px rgba(0,48,96,0.22);}"

   "@media (max-width:560px){.hero{top:40%;transform:scale(0.82);}"
     ".copy{top:53%;width:320px;margin-left:-160px;}.wordmark{font-size:21px;"
     "letter-spacing:8px;}.line{font-size:12px;}.sub{font-size:11px;}}"
   "@media (prefers-reduced-motion:reduce){.flake{animation:none;}}";

static char g_boot_css[32 * 1024];

static const char *boot_css(void)
{
   static int built;
   if (built) return g_boot_css;

   int n = snprintf(g_boot_css, sizeof g_boot_css, "%s", boot_css_base);
   for (int step = 0; step <= 100 && n < (int)sizeof g_boot_css - 160; ++step) {
      n += snprintf(g_boot_css + n, sizeof g_boot_css - (size_t)n,
                    ".ring.p%d{background:conic-gradient(from -90deg at 50%% 50%%,"
                    "#ffffff 0%%,#ffffff %d%%,rgba(255,255,255,0.22) %d%%,"
                    "rgba(255,255,255,0.22) 100%%);}",
                    step, step, step);
   }
   built = 1;
   return g_boot_css;
}

static void boot_set_ring_pct(int pct_i)
{
   if (pct_i < 0) pct_i = 0;
   if (pct_i > 100) pct_i = 100;

   int ring = luna_get_element_by_id("ring");
   if (ring < 0) return;

   /* HTML ships with p0; the first push must still remove it when advancing. */
   if (g_ring_pct < 0) g_ring_pct = 0;
   if (g_ring_pct == pct_i) return;

   char rem[8], add[8];
   snprintf(add, sizeof add, "p%d", pct_i);
   snprintf(rem, sizeof rem, "p%d", g_ring_pct);

   luna_update_classes(ring, rem, add);
   luna_mark_visual_dirty(ring);
   g_ring_pct = pct_i;
}

/* ---- pushing state into the live document ------------------------------ */

/* Dex fills the first half of the bar; dynarmic the second.  Progress is
 * measured from the snapshot taken when the card first appears — dex that
 * finished before the overlay was up must not jump the ring to 95% on frame
 * one.  A slow time floor keeps the arc moving during long libUE4 JIT even when
 * the compile counters plateau. */
static double boot_progress_pct(unsigned jit_p, unsigned dex_p)
{
   const double elapsed = boot_now() - g_shown_since;
   double pct = 3.0 + elapsed * (22.0 / 60.0);

   unsigned dex_rel = dex_p > g_dex_pct0 ? dex_p - g_dex_pct0 : 0u;
   if (dex_rel > 0) {
      double denom = 9500.0 - (double)g_dex_pct0;
      if (denom < 1.0) denom = 1.0;
      const double dex = 8.0 + ((double)dex_rel / denom) * 32.0;
      if (dex > pct) pct = dex;
   }

   if (jit_p > 50) {
      const double jit = 35.0 + ((jit_p / 100.0) - 0.5) * (52.0 / 94.5);
      if (jit > pct) pct = jit;
   }

   if (pct < 3.0) pct = 3.0;
   if (pct > 90.0) pct = 90.0;

   /* Never move backwards, and keep crawling so a long libUE4 JIT stretch
    * cannot look frozen at one value while blocks/s climb in the line below. */
   if (pct > g_displayed_pct)
      g_displayed_pct = pct;
   else
      g_displayed_pct += 0.04;
   if (g_displayed_pct > 90.0) g_displayed_pct = 90.0;
   if (g_displayed_pct < 3.0) g_displayed_pct = 3.0;
   return g_displayed_pct;
}

static void boot_snapshot_progress(void)
{
   g_dex_pct0 = atomic_load_explicit(&g_dex_pct100, memory_order_relaxed);
}

static void boot_frame(void)
{
   char jit[160], dex[160];
   unsigned jit_p = 0, dex_p = 0;
   int pct_i;

   /* Snowfall keeps moving from luna_update() alone; gating this handler on
    * g_text_dirty left the ring and status lines frozen while the flakes still
    * fell — exactly the “progress bar stuck at 0%” report on real APK boots. */
   boot_read_line(&g_jit_seq, g_jit_line, jit, sizeof jit, &g_jit_pct100, &jit_p);
   boot_read_line(&g_dex_seq, g_dex_line, dex, sizeof dex, &g_dex_pct100, &dex_p);
   atomic_store_explicit(&g_text_dirty, false, memory_order_relaxed);

   double pct = g_installing ? jit_p / 100.0 : boot_progress_pct(jit_p, dex_p);
   pct_i = (int)(pct + 0.5);

   char percent[16];
   snprintf(percent, sizeof percent, "%u%%", (unsigned)pct_i);

   if (strcmp(jit, g_last_jit) != 0) {
      int i = luna_get_element_by_id("jit");
      if (i >= 0) luna_set_text(i, jit);
      snprintf(g_last_jit, sizeof g_last_jit, "%s", jit);
   }
   if (strcmp(dex, g_last_dex) != 0) {
      int i = luna_get_element_by_id("dex");
      if (i >= 0) luna_set_text(i, dex);
      snprintf(g_last_dex, sizeof g_last_dex, "%s", dex);
   }
   if (strcmp(percent, g_last_pct) != 0) {
      int i = luna_get_element_by_id("pct");
      if (i >= 0) luna_set_text(i, percent);
      snprintf(g_last_pct, sizeof g_last_pct, "%s", percent);
   }

   boot_set_ring_pct(pct_i);
}

static void boot_show(void)
{
   bool was = atomic_exchange_explicit(&g_up, true, memory_order_acq_rel);
   if (was) return;
   boot_document_reset();
   g_shown_since = boot_now();
   g_last_compile_time = g_shown_since;
   g_displayed_pct = 3.0;
   atomic_store_explicit(&g_guest_swaps, 0u, memory_order_relaxed);
   boot_snapshot_progress();
   luna_overlay_set_frame_handler(boot_frame);
   luna_overlay_set_status(boot_html(), boot_css());
}

/* ---- what the emulator calls ------------------------------------------- */

bool luna_boot_jit_update(uint64_t compiles, uint64_t compile_ns)
{
   static uint64_t last_compiles;

   if (!boot_ui_wanted() || atomic_load_explicit(&g_done, memory_order_acquire))
      return false;

   const double now = boot_now();
   if (compiles != last_compiles) {
      last_compiles = compiles;
      g_last_compile_time = now;
   }

   const double compile_s = (double)compile_ns / 1e9;
   const bool busy = (compiles >= 256 && compile_s >= 0.4) ||
                     (compiles >= 64 && now - g_last_compile_time < 0.35);
   const bool up = atomic_load_explicit(&g_up, memory_order_acquire);

   if (!busy && !up) return false;

   if (!up) g_shown_since = now;
   boot_show();

   double pct = 100.0 * (1.0 - exp(-(double)compiles / 35000.0));
   if (pct > 95.0) pct = 95.0;
   if (pct < 2.0) pct = 2.0;

   const double elapsed = now - g_shown_since;
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
   atomic_store_explicit(&g_dex_pct100, 0, memory_order_relaxed);
   boot_show();
   boot_write_begin(&g_dex_seq);
   snprintf(g_dex_line, sizeof g_dex_line, "compiling dex — 0 of %d", files);
   atomic_store_explicit(&g_dex_pct100, 300, memory_order_relaxed);
   boot_write_end(&g_dex_seq);
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

void luna_boot_guest_presented(void)
{
   if (!atomic_load_explicit(&g_up, memory_order_acquire)
       || atomic_load_explicit(&g_done, memory_order_acquire))
      return;
   const double now = boot_now();
   const unsigned swaps =
      atomic_fetch_add_explicit(&g_guest_swaps, 1u, memory_order_relaxed) + 1u;
   const int files = atomic_load_explicit(&g_dex_files, memory_order_relaxed);
   const int done  = atomic_load_explicit(&g_dex_done, memory_order_relaxed);
   if (files > 0 && done < files) return;
   if (now - g_shown_since < 0.75) return;
   /* UE4's first swap is often a cleared back buffer; the logo movie is the
    * second or third.  Do not wait for JIT to go idle — that never happens on
    * a cold libUE4 start and left the card clearing every guest frame. */
   if (swaps < 2) return;
   luna_boot_finish("guest presented its first frame");
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

/* Installer: render on the main thread and hand the window to the guest. */
static GLFWwindow *window;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static int ready, cancelled, choosing;
static uint64_t last_frame;

static void pointer_position(GLFWwindow *w, double *x, double *y)
{
    int ww,wh,fw,fh;
    glfwGetWindowSize(w,&ww,&wh); glfwGetFramebufferSize(w,&fw,&fh);
    if (ww>0 && wh>0) { *x *= (double)fw/ww; *y *= (double)fh/wh; }
}
static void cursor(GLFWwindow *w, double x, double y)
{ pointer_position(w,&x,&y); luna_mouse_move(x, y); }
static void mouse(GLFWwindow *w, int button, int action, int mods)
{ double x,y; glfwGetCursorPos(w,&x,&y); pointer_position(w,&x,&y); luna_mouse_button(button, action, mods, x,y); }
static int pick_select_for = -1;
static void key(GLFWwindow *w, int k, int scan, int action, int mods)
{
    (void)w;
    /* An open file dialog owns the keyboard, Escape included. */
    if (luna_file_dialog_handle_key(k, scan, action, mods)) return;
    if (k == GLFW_KEY_ESCAPE && action == GLFW_PRESS && pick_select_for < 0) cancelled = 1;
    luna_key(k, scan, action, mods);
}
static void character(GLFWwindow *w, unsigned int c)
{ (void)w; luna_char(c); }
static void scroll(GLFWwindow *w, double x, double y)
{ (void)w; luna_scroll(x, y); }

/* A file dropped on the window is the same answer as typing its path. */
static char dropped[4096];
static void drop(GLFWwindow *w, int count, const char **paths)
{
    (void)w;
    if (count > 0 && paths[0] && strlen(paths[0]) < sizeof dropped)
        snprintf(dropped, sizeof dropped, "%s", paths[0]);
}

static void frame(int force)
{
    if (!ready) return;
    uint64_t now = luna_os_monotonic_ns();
    if (!force && now - last_frame < UINT64_C(33000000)) return;
    glfwPollEvents();
    if (glfwWindowShouldClose(window)) cancelled = 1;
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    if (w > 0 && h > 0) {
        /* Only when the size changed: a resize re-lays the page out, and a
         * page that is laid out again starts its keyframes over -- the snow
         * would sit at its first frame for as long as this ran every pass. */
        static int last_w, last_h;
        if (w != last_w || h != last_h) {
            luna_resize((float)w, (float)h);
            last_w = w; last_h = h;
        }
        glViewport(0, 0, w, h);
        glClearColor(.95f, .96f, .97f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!choosing) boot_frame();
        const double dt = last_frame ? (double)(now-last_frame)/1e9 : 0;
        luna_file_dialog_tick(dt);
        luna_update(luna_platform_time(), dt);
        luna_render(w, h);
        luna_file_dialog_render(w, h);
        /* LUNARIA_LAUNCHER_SHOT=file.png: save the launcher's frame, once, a few
         * frames in (the first ones are still laying out). */
        static int frames;
        const char *shot = getenv("LUNARIA_LAUNCHER_SHOT");
        if (shot && *shot) {
            ++frames;
            char later[4200];
            snprintf(later, sizeof later, "%s.later.png", shot);
            if (frames == 8) luna_overlay_screenshot(shot);
            else if (frames == 200) luna_overlay_screenshot(later);
        }
        eglSwapBuffers(display, surface);
    }
    last_frame = now;
}
static void progress_document(void)
{
    luna_reset_document();
    luna_reset_css();
    luna_parse_css(boot_css());
    luna_parse_html(boot_html());
    boot_document_reset();
}

/* The launcher's window, EGL surface and luna-ui instance.  Shared by the
 * "open an application" screen and the installer, so one window serves the
 * whole run up to the guest taking it over.  Returns 0 when there is no
 * display to open it on. */
static int launcher_window_open(int width, int height)
{
    if (ready) return 1;
    const char *pbuffer = getenv("LUNARIA_PBUFFER");
    if (pbuffer && *pbuffer && *pbuffer != '0') return 0;
    if (!glfwInit()) return 0;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (width <= 0 || height <= 0) {
        const struct lunaria_screen *screen = lunaria_screen();
        width = screen->width; height = screen->height;
    }
    window = glfwCreateWindow(width, height, "Lunaria — Preparing application", NULL, NULL);
    if (!window) return 0;
    display = eglGetDisplay((EGLNativeDisplayType)luna_os_native_display());
    EGLint major, minor, n;
    EGLConfig config;
    const EGLint config_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(display, config_attrs, &config, 1, &n) || !n) goto fail;
    surface = eglCreateWindowSurface(display, config,
        (EGLNativeWindowType)(uintptr_t)luna_os_native_window(window), NULL);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) goto fail;
    luna_overlay_prepare_platform();
    luna_set_gles3(1);
    LunaInitConfig cfg = {.width=width, .height=height, .frameless=1,
                          .get_proc=luna_platform_get_proc()};
    if (!luna_init(&cfg)) goto fail;
    ready = 1;
    g_installing = true;
    glfwSetCursorPosCallback(window, cursor);
    glfwSetMouseButtonCallback(window, mouse);
    glfwSetKeyCallback(window, key);
    glfwSetCharCallback(window, character);
    glfwSetScrollCallback(window, scroll);
    glfwSetDropCallback(window, drop);
    return 1;
fail:
    fprintf(stderr, "[apk-ui] cannot initialize launch UI (EGL 0x%x)\n", eglGetError());
    luna_launcher_end(0);
    return 0;
}

int luna_launcher_begin(const char *path)
{
    if (!launcher_window_open(0, 0)) return 0;
    const struct lunaria_screen *screen = lunaria_screen();
    /* Resolve the app size after launcher edits and manifest/config loading.
     * The setup form must not freeze the guest display before these inputs. */
    int ww, wh;
    glfwGetWindowSize(window, &ww, &wh);
    if (ww != screen->width || wh != screen->height)
        glfwSetWindowSize(window, screen->width, screen->height);
    progress_document();
    luna_launcher_progress("Opening archive", path, 0, 0);
    frame(1);
    fprintf(stderr, "[apk-ui] window shown before archive extraction (%dx%d)\n",
            screen->width, screen->height);
    return 1;
}

void luna_launcher_progress(const char *stage, const char *file, uint64_t bytes, uint64_t total)
{
    if (!ready || choosing) return;
    if (!stage && luna_os_monotonic_ns() - last_frame < UINT64_C(33000000)) return;
    boot_write_begin(&g_jit_seq);
    if (stage) snprintf(g_jit_line, sizeof g_jit_line, "%s", stage);
    unsigned pct = total ? (unsigned)(10000.0L * bytes / total) : 0;
    atomic_store_explicit(&g_jit_pct100, pct > 10000 ? 10000 : pct, memory_order_relaxed);
    boot_write_end(&g_jit_seq);
    boot_write_begin(&g_dex_seq);
    if (total) snprintf(g_dex_line, sizeof g_dex_line, "%.1f / %.1f MiB · %.100s",
        (double)bytes/1048576, (double)total/1048576, file ? file : "");
    else snprintf(g_dex_line, sizeof g_dex_line, "%s", file ? file : "");
    boot_write_end(&g_dex_seq);
    frame(0);
}
int luna_launcher_cancelled(void) { return cancelled; }

/* ---- the launcher screen --------------------------------------------------
 *
 * Started with no application (a double-clicked .app, a desktop launcher) the
 * emulator asks which one to run, and where this installation keeps its data:
 * the one setting that cannot be recreated, because a later run pointed
 * elsewhere does not find what an earlier one downloaded.  The application is
 * a path field, Browse (luna-ui's own file dialog, so it is the same on every
 * host OS) and drag-and-drop. */
#define PICK_FIELDS 16
#ifndef LUNARIA_VERSION
#define LUNARIA_VERSION ""
#endif
static const char *pick_version = LUNARIA_VERSION;
static luna_launcher_setting *pick_settings;
static size_t pick_count;
static int pick_chosen;
static char pick_folder[4096];
static int pick_folder_for = -1;       /* which field the folder dialog answers */

static void pick_select_label(size_t i)
{
    char id[32], label[1100];
    snprintf(id, sizeof id, "set%zu", i);
    snprintf(label, sizeof label, "%s v", *pick_settings[i].value ?
             pick_settings[i].value : "Default (pixel6)");
    luna_set_text(luna_get_element_by_id(id), label);
}

static void pick_select_close(int panel)
{
    luna_add_class(panel, "hidden");
    luna_update_element_style(panel);
    luna_pop_focus_trap(panel);
    if (pick_select_for >= 0) {
        char id[32]; snprintf(id, sizeof id, "set%d", pick_select_for);
        luna_dom_set_attr(luna_get_element_by_id(id), "aria-expanded", "false");
    }
    pick_select_for = -1;
}

static void pick_select_toggle(LunaElement *element)
{
    for (size_t i = 0; i < pick_count; ++i) {
        char id[32]; snprintf(id, sizeof id, "set%zu", i);
        if (luna_element_at(luna_get_element_by_id(id)) != element) continue;
        int previous = pick_select_for;
        if (previous >= 0) {
            snprintf(id, sizeof id, "select%d", previous);
            pick_select_close(luna_get_element_by_id(id));
        }
        if (previous == (int)i) return;
        pick_select_for = (int)i;
        snprintf(id, sizeof id, "select%zu", i);
        int panel = luna_get_element_by_id(id);
        luna_remove_class(panel, "hidden");
        luna_update_element_style(panel);
        snprintf(id, sizeof id, "set%zu", i);
        luna_dom_set_attr(luna_get_element_by_id(id), "aria-expanded", "true");
        luna_push_focus_trap(panel, pick_select_close, 0);
        for (size_t k = 0; pick_settings[i].choices[k]; ++k) {
            char option_id[48]; snprintf(option_id, sizeof option_id, "option%zu_%zu", i, k);
            int selected = !strcmp(pick_settings[i].value, pick_settings[i].choices[k]);
            luna_dom_set_attr(luna_get_element_by_id(option_id), "aria-selected", selected ? "true" : "false");
            if (selected) luna_focus_element(luna_get_element_by_id(option_id));
        }
        return;
    }
}

static void pick_select_option(LunaElement *element)
{
    if (pick_select_for < 0) return;
    size_t i = (size_t)pick_select_for;
    for (size_t k = 0; pick_settings[i].choices[k]; ++k) {
        char id[48]; snprintf(id, sizeof id, "option%zu_%zu", i, k);
        if (luna_element_at(luna_get_element_by_id(id)) != element) continue;
        snprintf(pick_settings[i].value, sizeof pick_settings[i].value,
                 "%s", pick_settings[i].choices[k]);
        pick_select_label(i);
        snprintf(id, sizeof id, "select%zu", i);
        pick_select_close(luna_get_element_by_id(id));
        return;
    }
}

static void pick_continue(LunaElement *element) { (void)element; pick_chosen = 1; }

static void pick_dialog_done(const LunaFileDialogResult *result, void *userdata)
{
    (void)userdata;
    if (!result || !result->accepted || result->count < 1 || !result->paths[0] ||
        strlen(result->paths[0]) >= sizeof dropped) return;
    if (pick_folder_for >= 0) snprintf(pick_folder, sizeof pick_folder, "%s", result->paths[0]);
    else snprintf(dropped, sizeof dropped, "%s", result->paths[0]);
}

static void pick_open_dialog(int field)
{
    char start[4096] = "";
    const char *home = getenv("HOME");
    if (home) {
        luna_file_info info;
        snprintf(start, sizeof start, "%s/Downloads", home);
        if (luna_file_infoat(LUNA_AT_FDCWD, start, &info, 0))
            snprintf(start, sizeof start, "%s", home);
    }
    pick_folder_for = field;
    LunaFileDialogConfig config = {0};
    if (field >= 0) {
        char id[32]; snprintf(id, sizeof id, "set%d", field);
        const char *now = luna_get_value(luna_get_element_by_id(id));
        config.mode = LUNA_FILE_DIALOG_SELECT_FOLDER;
        config.title = pick_settings[field].label;
        config.initial_path = now && now[0] ? now : (start[0] ? start : NULL);
    } else {
        config.mode = LUNA_FILE_DIALOG_OPEN_FILE;
        config.title = "Open an application";
        config.initial_path = start[0] ? start : NULL;
        config.filter_name = "Android packages";
        config.filter_patterns = "*.apk *.xapk *.apks *.aab *.so";
    }
    if (luna_file_dialog_show(&config, pick_dialog_done, NULL) < 0)
        luna_set_text(luna_get_element_by_id("error"),
                      "No file browser here: type the path or drop the file on this window.");
}

static void pick_browse(LunaElement *element) { (void)element; pick_open_dialog(-1); }
static void pick_browse_field(LunaElement *element)
{
    for (size_t i = 0; i < pick_count; ++i) {
        char id[32]; snprintf(id, sizeof id, "browse%zu", i);
        if (luna_element_at(luna_get_element_by_id(id)) == element) { pick_open_dialog((int)i); return; }
    }
}

/* What a person types or drags is not always a bare path: quotes from a shell,
 * a file:// URL from a browser, a leading ~. */
static int pick_clean(const char *in, char *out, size_t capacity)
{
    while (*in == ' ' || *in == '\t') ++in;
    size_t len = strlen(in);
    while (len && (in[len-1] == ' ' || in[len-1] == '\t' || in[len-1] == '\n' ||
                   in[len-1] == '\r')) --len;
    if (len >= 2 && (in[0] == '"' || in[0] == '\'') && in[len-1] == in[0]) { ++in; len -= 2; }
    if (len >= 7 && !strncmp(in, "file://", 7)) { in += 7; len -= 7; }
    const char *home = getenv("HOME");
    char text[4096];
    if (in[0] == '~' && (len == 1 || in[1] == '/') && home)
        snprintf(text, sizeof text, "%s%.*s", home, (int)(len - 1), in + 1);
    else
        snprintf(text, sizeof text, "%.*s", (int)len, in);
    if (!text[0] || strlen(text) >= capacity) return -1;
    memcpy(out, text, strlen(text) + 1);
    return 0;
}

static int pick_acceptable(const char *path)
{
    static const char *const kinds[] = { ".apk", ".xapk", ".apks", ".aab", ".so" };
    const size_t len = strlen(path);
    luna_file_info info;
    if (luna_file_infoat(LUNA_AT_FDCWD, path, &info, 0)) return 0;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
        const size_t k = strlen(kinds[i]);
        if (len > k && !strcasecmp(path + len - k, kinds[i])) return 1;
    }
    return 0;
}

/* A folder is accepted when files can really be made in it. */
static int pick_folder_usable(const char *dir)
{
    char probe[4200];
    for (const char *p = dir + 1; (p = strchr(p, '/')); ++p) {
        char part[4096];
        snprintf(part, sizeof part, "%.*s", (int)(p - dir), dir);
        luna_file_mkdir(part, 0700);
    }
    luna_file_mkdir(dir, 0700);
    snprintf(probe, sizeof probe, "%s/.lunaria-write-test", dir);
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

static void pick_set_field(size_t i, const char *value)
{
    char id[32]; snprintf(id, sizeof id, "set%zu", i);
    if (pick_settings[i].choices)
        pick_select_label(i);
    else luna_set_value(luna_get_element_by_id(id), value);
}

static int pick_read_fields(void)
{
    for (size_t i = 0; i < pick_count; ++i) {
        if (pick_settings[i].choices) continue;
        char id[32]; snprintf(id, sizeof id, "set%zu", i);
        const char *v = luna_get_value(luna_get_element_by_id(id));
        char cleaned[1024];
        if (!v || !*v) { pick_settings[i].value[0] = 0; continue; }
        if (pick_settings[i].folder) {
            if (pick_clean(v, cleaned, sizeof cleaned) || cleaned[0] != '/'
#ifdef _WIN32
                && !(cleaned[1] == ':')
#endif
               ) {
                char msg[160];
                snprintf(msg, sizeof msg, "%s must be a full path.", pick_settings[i].label);
                luna_set_text(luna_get_element_by_id("error"), msg);
                return -1;
            }
            if (!pick_folder_usable(cleaned)) {
                char msg[200];
                snprintf(msg, sizeof msg, "Cannot write to %s: %.100s", pick_settings[i].label, cleaned);
                luna_set_text(luna_get_element_by_id("error"), msg);
                return -1;
            }
            snprintf(pick_settings[i].value, sizeof pick_settings[i].value, "%s", cleaned);
        } else {
            snprintf(pick_settings[i].value, sizeof pick_settings[i].value, "%.*s",
                     (int)sizeof pick_settings[i].value - 1, v);
        }
    }
    return 0;
}

/* The launcher wears the boot card's clothes -- the same winter sky, snow and
 * wordmark -- so the first screen and the loading screen read as one thing.
 * Only what a form needs is added on top of boot_css_base. */
static const char pick_css_extra[] =
    /* Flakes start just above the window and fall in ~10-20 s: the boot card's own
     * minute-long drops begin hundreds of pixels up, and here most of the sky
     * is behind the card. */
    ".flake:nth-child(1){width:12px;height:12px;top:-212px;left:21%;opacity:0.62;animation:19s flakes linear infinite;}"
    ".flake:nth-child(2){width:17px;height:17px;top:-197px;left:14%;opacity:0.62;animation:18s flakes linear infinite;}"
    ".flake:nth-child(3){width:17px;height:17px;top:-29px;left:29%;opacity:0.75;animation:10s flakes linear infinite;}"
    ".flake:nth-child(4){width:8px;height:8px;top:-56px;left:32%;opacity:0.75;animation:17s flakes linear infinite;}"
    ".flake:nth-child(5){width:17px;height:17px;top:-124px;left:17%;opacity:0.82;animation:19s flakes linear infinite;}"
    ".flake:nth-child(6){width:8px;height:8px;top:-309px;left:75%;opacity:0.62;animation:15s flakes linear infinite;}"
    ".flake:nth-child(7){width:10px;height:10px;top:-295px;left:7%;opacity:0.7;animation:11s flakes linear infinite;}"
    ".flake:nth-child(8){width:10px;height:10px;top:-70px;left:71%;opacity:0.71;animation:18s flakes linear infinite;}"
    ".flake:nth-child(9){width:20px;height:20px;top:-62px;left:25%;opacity:0.8;animation:18s flakes linear infinite;}"
    ".flake:nth-child(10){width:10px;height:10px;top:-59px;left:49%;opacity:0.85;animation:17s flakes linear infinite;}"
    ".flake:nth-child(11){width:17px;height:17px;top:-326px;left:9%;opacity:0.77;animation:12s flakes linear infinite;}"
    ".flake:nth-child(12){width:17px;height:17px;top:-407px;left:56%;opacity:0.76;animation:14s flakes linear infinite;}"
    ".flake:nth-child(13){width:14px;height:14px;top:-163px;left:48%;opacity:0.88;animation:12s flakes linear infinite;}"
    ".flake:nth-child(14){width:20px;height:20px;top:-51px;left:33%;opacity:0.71;animation:18s flakes linear infinite;}"
    ".flake:nth-child(15){width:14px;height:14px;top:-383px;left:45%;opacity:0.7;animation:16s flakes linear infinite;}"
    ".flake:nth-child(16){width:8px;height:8px;top:-272px;left:17%;opacity:0.66;animation:15s flakes linear infinite;}"
    ".flake:nth-child(17){width:12px;height:12px;top:-260px;left:21%;opacity:0.61;animation:15s flakes linear infinite;}"
    ".flake:nth-child(18){width:20px;height:20px;top:-401px;left:11%;opacity:0.8;animation:17s flakes linear infinite;}"
    ".flake:nth-child(19){width:12px;height:12px;top:-365px;left:45%;opacity:0.81;animation:14s flakes linear infinite;}"
    ".flake:nth-child(20){width:17px;height:17px;top:-45px;left:60%;opacity:0.93;animation:10s flakes linear infinite;}"
    ".flake:nth-child(21){width:14px;height:14px;top:-350px;left:91%;opacity:0.62;animation:10s flakes linear infinite;}"
    ".flake:nth-child(22){width:20px;height:20px;top:-341px;left:41%;opacity:0.95;animation:18s flakes linear infinite;}"
    ".flake:nth-child(23){width:14px;height:14px;top:-376px;left:38%;opacity:0.91;animation:15s flakes linear infinite;}"
    ".flake:nth-child(24){width:12px;height:12px;top:-246px;left:4%;opacity:0.66;animation:14s flakes linear infinite;}"
    ".mark{position:absolute;left:0;right:0;top:20px;text-align:center;font-size:22px;"
    "font-weight:700;letter-spacing:10px;color:#ffffff;text-shadow:0 2px 16px rgba(0,48,96,0.25);}"
    "#launcher-card{position:absolute;left:120px;right:120px;top:68px;bottom:28px;"
    "background:rgba(255,255,255,0.84);border-radius:16px;box-shadow:0 14px 42px rgba(0,48,96,0.28);}"
    "#launcher-card .main{position:absolute;left:0;right:0;top:0;bottom:68px;padding:24px 32px 8px 32px;overflow-y:auto;color:#12324d;}"
    "#launcher-card h1{font-size:21px;margin:0 0 4px 0;color:#12324d;}"
    "#launcher-card h2{font-size:12px;margin:22px 0 2px 0;color:#5c7a92;letter-spacing:2px;}"
    "#launcher-card .sub{margin:0 0 12px 0;color:#5c7a92;font-size:13px;text-align:left;padding:0;}"
    "#launcher-card .drop{padding:14px;border:2px dashed #9cc7e4;border-radius:10px;background:#eef7fd;"
    "text-align:center;color:#3f7fa8;margin-bottom:10px;}"
    "#launcher-card .row{position:relative;height:42px;margin:0 0 4px 0;}"
    "#launcher-card .row input{position:absolute;left:0;right:104px;top:0;height:40px;width:auto;padding:0 12px;"
    "border:1px solid #bcd6e8;border-radius:8px;background:#ffffff;color:#12324d;}"
    "#launcher-card .row button{position:absolute;right:0;top:0;width:96px;height:42px;margin:0;padding:0;}"
    "#launcher-card button{background:#e3f1fb;color:#12324d;border:1px solid #bcd6e8;border-radius:8px;}"
    "#launcher-card .lbl{margin:12px 0 5px 0;font-weight:600;font-size:14px;}"
    "#launcher-card .trio{position:relative;height:72px;margin:0 0 4px 0;}"
    "#launcher-card .cell{position:absolute;top:0;height:70px;}"
    "#launcher-card .cell input{position:absolute;left:0;right:0;top:30px;height:40px;padding:0 12px;border:1px solid #bcd6e8;border-radius:8px;background:#ffffff;color:#12324d;}"
    "#launcher-card .cell .select-box{box-sizing:border-box;position:absolute;left:0;right:0;top:30px;height:40px;padding:10px 12px;border:1px solid #bcd6e8;border-radius:8px;background:#ffffff;color:#12324d;cursor:pointer;}"
    "#launcher-card .select-panel{position:absolute;left:0;right:0;bottom:42px;z-index:100;background:#ffffff;border:1px solid #bcd6e8;border-radius:8px;padding:4px;}"
    "#launcher-card .select_option{box-sizing:border-box;height:34px;padding:8px;cursor:pointer;}"
    "#launcher-card .select_option:hover,#launcher-card .select_option:focus{background:#e3f1fb;}"
    "#launcher-card .select-panel.hidden{display:none;}"
    "#launcher-card .hint{margin:0 0 2px 0;font-size:12px;color:#6f8ca2;}"
    "#launcher-card #error{color:#c0392b;margin:10px 0 0 0;min-height:18px;font-size:13px;}"
    "#launcher-card .bar{position:absolute;left:0;right:0;bottom:0;height:68px;"
    "border-top:1px solid #d6e6f2;border-radius:0 0 16px 16px;background:rgba(255,255,255,0.97);}"
    "#launcher-card .bar .note{position:absolute;left:32px;top:25px;font-size:12px;color:#6f8ca2;}"
    "#launcher-card #continue{position:absolute;right:32px;top:13px;width:150px;height:42px;margin:0;"
    "background:#0b8fe0;color:#ffffff;border:1px solid #0b8fe0;font-weight:700;}";

static int launcher_form(char *path, size_t capacity,
                         luna_launcher_setting *settings, size_t count,
                         const char *package)
{
    if (!launcher_window_open(960, 680)) return -1;
    if (count > PICK_FIELDS) count = PICK_FIELDS;
    pick_settings = settings; pick_count = count;
    glfwSetWindowTitle(window, "Lunaria");
    choosing = 1; pick_chosen = 0; dropped[0] = 0; pick_folder[0] = 0; pick_folder_for = -1; pick_select_for = -1;

    /* luna-ui's file dialog is part of the document, hidden until Browse. */
    const char *dialog_html = luna_file_dialog_overlay_html();
    const char *dialog_css = luna_file_dialog_css();
    size_t html_size = 6144 + count * 768 + strlen(dialog_html);
    for (size_t i = 0; i < count; ++i) {
        if (!settings[i].choices) continue;
        for (size_t k = 0; settings[i].choices[k]; ++k) {
            size_t extra = 160 + strlen(settings[i].choices[k]);
            if (extra > SIZE_MAX - html_size) return -1;
            html_size += extra;
        }
    }
    char *html = malloc(html_size);
    char *css = malloc(sizeof boot_css_base + sizeof pick_css_extra + strlen(dialog_css) + 1);
    if (!html || !css) { free(html); free(css); return -1; }
    size_t at = (size_t)snprintf(html, html_size,
        "<body><div class=\"snow\">"
        "<div class=\"flake\"></div><div class=\"flake\"></div><div class=\"flake\"></div>"
        "<div class=\"flake\"></div><div class=\"flake\"></div><div class=\"flake\"></div>"
        "<div class=\"flake\"></div><div class=\"flake\"></div><div class=\"flake\"></div>"
        "<div class=\"flake\"></div><div class=\"flake\"></div><div class=\"flake\"></div>"
        "<div class=\"flake\"></div><div class=\"flake\"></div><div class=\"flake\"></div>"
        "<div class=\"flake\"></div><div class=\"flake\"></div><div class=\"flake\"></div>"
        "</div><div class=\"mark\">LUNARIA</div>"
        "<div class=\"card\" id=\"launcher-card\"><div class=\"main\"><h1 id=\"launcher-title\"></h1>"
        "<p class=\"sub\" id=\"launcher-sub\"></p>");
    if (!package) at += (size_t)snprintf(html + at, html_size - at,
        "<div class=\"drop\" id=\"drop\">Drop a package file here</div>"
        "<div class=\"row\"><input id=\"path\" value=\"\">"
        "<button id=\"browse\">Browse…</button></div>");
    at += (size_t)snprintf(html + at, html_size - at, "<h2>%s</h2>", package ? "PROFILE" : "SETTINGS");
    for (size_t i = 0; i < count; ++i) {
        if (settings[i].compact) {
            /* a run of compact settings is one row of equal columns */
            size_t run = 0;
            while (i + run < count && settings[i + run].compact) ++run;
            at += (size_t)snprintf(html + at, html_size - at, "<div class=\"trio\">");
            for (size_t k = 0; k < run; ++k) {
                at += (size_t)snprintf(html + at, html_size - at,
                    "<div class=\"cell\" style=\"left:%zu%%;width:%zu%%;\">"
                    "<div class=\"lbl\">%s</div>",
                    k * 100 / run, 100 / run - 2, settings[i + k].label);
                size_t field = i + k;
                if (settings[field].choices) {
                    at += (size_t)snprintf(html + at, html_size - at,
                        "<div id=\"set%zu\" class=\"select-box\" role=\"combobox\" tabindex=\"0\" aria-expanded=\"false\"></div>"
                        "<div id=\"select%zu\" class=\"select-panel hidden\" role=\"listbox\">", field, field);
                    for (size_t option = 0; settings[field].choices[option]; ++option)
                        at += (size_t)snprintf(html + at, html_size - at,
                            "<div id=\"option%zu_%zu\" class=\"select_option\" role=\"option\" tabindex=\"0\">%s</div>",
                            field, option, *settings[field].choices[option] ? settings[field].choices[option] : "Default (pixel6)");
                    at += (size_t)snprintf(html + at, html_size - at, "</div>");
                } else at += (size_t)snprintf(html + at, html_size - at,
                            "<input id=\"set%zu\" value=\"\">", field);
                at += (size_t)snprintf(html + at, html_size - at, "</div>");
            }
            at += (size_t)snprintf(html + at, html_size - at, "</div>");
            i += run - 1;
            continue;
        }
        at += (size_t)snprintf(html + at, html_size - at,
            "<div class=\"lbl\">%s</div><div class=\"row\"><input id=\"set%zu\" value=\"\">",
            settings[i].label, i);
        if (settings[i].folder)
            at += (size_t)snprintf(html + at, html_size - at,
                "<button id=\"browse%zu\">Browse…</button>", i);
        at += (size_t)snprintf(html + at, html_size - at, "</div>");
        if (settings[i].hint && *settings[i].hint)
            at += (size_t)snprintf(html + at, html_size - at, "<div class=\"hint\">%s</div>", settings[i].hint);
    }
    snprintf(html + at, html_size - at,
        "<div id=\"error\"></div></div>"
        "<div class=\"bar\"><div class=\"note\">%s %s</div>"
        "<button id=\"continue\">Open</button></div></div>%s</body>",
        package ? "Each profile keeps its own app data." : "Changes are saved to lunaria.conf.", pick_version, dialog_html);
    snprintf(css, sizeof boot_css_base + sizeof pick_css_extra + strlen(dialog_css) + 1,
             "%s%s%s", boot_css_base, pick_css_extra, dialog_css);
    luna_reset_document();
    luna_reset_css();
    luna_parse_css(css);
    luna_parse_html(html);
    free(html); free(css);
    luna_set_text(luna_get_element_by_id("launcher-title"), package ? "Open a profile" : "Open an application");
    luna_set_text(luna_get_element_by_id("launcher-sub"), package ? package : "Choose an APK, XAPK, APKS or AAB package.");
    luna_set_on_click(luna_get_element_by_id("browse"), pick_browse);
    luna_set_on_click(luna_get_element_by_id("continue"), pick_continue);
    for (size_t i = 0; i < count; ++i) {
        char id[32]; snprintf(id, sizeof id, "browse%zu", i);
        if (settings[i].folder) luna_set_on_click(luna_get_element_by_id(id), pick_browse_field);
        if (settings[i].choices) {
            snprintf(id, sizeof id, "set%zu", i);
            luna_set_on_click(luna_get_element_by_id(id), pick_select_toggle);
            for (size_t k = 0; settings[i].choices[k]; ++k) {
                char option_id[48]; snprintf(option_id, sizeof option_id, "option%zu_%zu", i, k);
                luna_set_on_click(luna_get_element_by_id(option_id), pick_select_option);
            }
        }
        pick_set_field(i, settings[i].value);
    }
    luna_file_dialog_prepare();
    if (package) fprintf(stderr, "[apk-ui] profile selection shown for %s (shared launcher form)\n", package);
    else fprintf(stderr, "[apk-ui] no application given: asking which to open\n");

    int result = -1;
    while (!cancelled) {
        frame(1);
        if (pick_folder[0] && pick_folder_for >= 0) {
            pick_set_field((size_t)pick_folder_for, pick_folder);
            pick_folder[0] = 0; pick_folder_for = -1;
        }
        if (dropped[0]) {
            luna_set_value(luna_get_element_by_id("path"), dropped);
            dropped[0] = 0;
            pick_chosen = 1;
        }
        if (pick_chosen) {
            pick_chosen = 0;
            if (package) {
                if (pick_read_fields()) continue;
                const char *value = settings[1].value[0] ? settings[1].value : settings[0].value;
                size_t len = strlen(value);
                int valid = len && len <= 64 && len < capacity;
                for (size_t i = 0; valid && i < len; ++i) {
                    unsigned char c = (unsigned char)value[i];
                    int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                    valid = alnum || c == '_' || (i && (c == '-' || c == '.'));
                }
                if (valid) { memcpy(path, value, len + 1); result = 0; break; }
                luna_set_text(luna_get_element_by_id("error"), "Use 1–64 letters, digits, dots, hyphens or underscores.");
                continue;
            }
            const char *typed = luna_get_value(luna_get_element_by_id("path"));
            char cleaned[4096];
            if (!typed || pick_clean(typed, cleaned, sizeof cleaned) ||
                !pick_acceptable(cleaned) || strlen(cleaned) >= capacity) {
                luna_set_text(luna_get_element_by_id("error"),
                              "That is not an existing .apk, .xapk, .apks or .aab file.");
            } else if (!pick_read_fields()) {
                memcpy(path, cleaned, strlen(cleaned) + 1);
                result = 0;
                break;
            }
        }
        glfwWaitEventsTimeout(.02);
    }
    choosing = 0;
    if (pick_select_for >= 0) {
        char id[32]; snprintf(id, sizeof id, "select%d", pick_select_for);
        pick_select_close(luna_get_element_by_id(id));
    }
    pick_settings = NULL; pick_count = 0;
    return result;
}

int luna_launcher_pick(char *path, size_t capacity,
                       luna_launcher_setting *settings, size_t count)
{
    return launcher_form(path, capacity, settings, count, NULL);
}

int luna_launcher_choose(const char *package, const char *const *profiles,
                         size_t count, char *choice, size_t capacity)
{
    if (!ready || !count || count >= SIZE_MAX / sizeof(char *)) return -1;
    const char **values = calloc(count + 1, sizeof *values);
    if (!values) return -1;
    for (size_t i = 0; i < count; ++i) values[i] = profiles[i];
    luna_launcher_setting settings[2] = {
        {.label="Profile", .compact=1, .choices=values},
        {.label="New profile (optional)", .compact=1}
    };
    const char *selected = getenv("LUNARIA_PROFILE");
    snprintf(settings[0].value, sizeof settings[0].value, "%s", selected && *selected ? selected : profiles[0]);
    int result = launcher_form(choice, capacity, settings, 2, package);
    free(values);
    progress_document(); frame(1);
    return result;
}

void luna_launcher_end(int keep_window)
{
    g_installing = false;
    if (keep_window && ready) {
        boot_write_begin(&g_jit_seq);
        snprintf(g_jit_line, sizeof g_jit_line, "Starting application");
        atomic_store_explicit(&g_jit_pct100, 0, memory_order_relaxed);
        boot_write_end(&g_jit_seq);
        boot_write_begin(&g_dex_seq);
        g_dex_line[0] = 0;
        boot_write_end(&g_dex_seq);
        if (boot_ui_wanted()) boot_show();
    }
    if (window) {
        glfwSetCursorPosCallback(window, NULL);
        glfwSetMouseButtonCallback(window, NULL);
        glfwSetKeyCallback(window, NULL);
        glfwSetCharCallback(window, NULL);
        glfwSetScrollCallback(window, NULL);
    }
    if (ready) { luna_shutdown(); ready=0; }
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    display=EGL_NO_DISPLAY; context=EGL_NO_CONTEXT; surface=EGL_NO_SURFACE;
    if (!keep_window && window) { glfwDestroyWindow(window); window=NULL; }
}
void *luna_launcher_take_window(void)
{ GLFWwindow *result=window; window=NULL; return result; }
