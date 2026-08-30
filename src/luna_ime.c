/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See luna_ime.h for what this is and why it exists.
 */

#define LUNA_UI_NO_PLATFORM
#include "luna-ui.h"

#include "luna_ime.h"
#include "luna_overlay.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Android InputType bits this layer acts on.  Named rather than spelled out at
 * the use site: 0x80 is only "password" if you already know that. */
#define IME_TYPE_CLASS_MASK                  0x0000000f
#define IME_TYPE_CLASS_NUMBER                0x00000002
#define IME_TYPE_CLASS_PHONE                 0x00000003
#define IME_TYPE_MASK_VARIATION              0x00000ff0
#define IME_TYPE_TEXT_VARIATION_PASSWORD     0x00000080
#define IME_TYPE_TEXT_VARIATION_WEB_PASSWORD 0x000000e0
#define IME_TYPE_NUMBER_VARIATION_PASSWORD   0x00000010

#define IME_FIELD_ID "luna-ime-field"
#define IME_TEXT_MAX 512   /* luna-ui's own LunaElement::text is this size */

/* Shared between the dvm looper (show/hide/pump) and the presenting thread
 * (frame).  Everything under g_lock. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_active;
static int  g_input_type;
static bool g_multiline;
static char g_text[IME_TEXT_MAX];   /* authoritative contents */
static int  g_events;               /* LUNA_IME_EV_*, drained by pump() */

/* Raw host events.  The host's event thread must not touch luna-ui: the
 * presenting thread is inside it. */
enum { IME_Q_CHAR = 1, IME_Q_KEY = 2 };
struct ime_queued { int kind, key, scancode, action, mods; uint32_t codepoint; };
static struct ime_queued g_queue[128];
static int g_queue_count;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;

/* Presenting thread only. */
static int g_field_idx = -1;

/* ------------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------------ */

/* The panel's height, and therefore the band.  These are one number on
 * purpose: what the guest is told about its window has to be what is actually
 * covering it, so the CSS below fixes the height rather than letting the
 * content decide it.  240px is about a phone IME's share of a 16:9 screen and
 * is comfortably over the 200px UE4's GameActivity uses as its "is a keyboard
 * up" threshold — below that an app would be right to conclude nothing came
 * up. */
#define IME_BAND_H 240

int luna_ime_band_height(int surface_h)
{
   if (!g_active) return 0;
   return (surface_h > 0 && surface_h < IME_BAND_H) ? surface_h : IME_BAND_H;
}

/* ------------------------------------------------------------------------ *
 * The panel
 * ------------------------------------------------------------------------ */

static bool ime_is_password(int input_type)
{
   int variation = input_type & IME_TYPE_MASK_VARIATION;
   int cls = input_type & IME_TYPE_CLASS_MASK;
   if (variation == IME_TYPE_TEXT_VARIATION_PASSWORD ||
       variation == IME_TYPE_TEXT_VARIATION_WEB_PASSWORD)
      return true;
   return cls == IME_TYPE_CLASS_NUMBER &&
          variation == IME_TYPE_NUMBER_VARIATION_PASSWORD;
}

static const char *ime_kind_label(int input_type, bool multiline)
{
   int cls = input_type & IME_TYPE_CLASS_MASK;
   if (ime_is_password(input_type)) return "password";
   if (cls == IME_TYPE_CLASS_NUMBER) return "number";
   if (cls == IME_TYPE_CLASS_PHONE)  return "phone";
   return multiline ? "text, multi-line" : "text";
}

/* HTML-escapes into `out`; returns `out`. */
static char *ime_escape(const char *s, char *out, size_t cap)
{
   size_t o = 0;
   for (size_t i = 0; s && s[i] && o + 8 < cap; ++i) {
      const char *rep = NULL;
      switch (s[i]) {
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '&':  rep = "&amp;";  break;
      case '"':  rep = "&quot;"; break;
      default: break;
      }
      if (rep) { size_t n = strlen(rep); memcpy(out + o, rep, n); o += n; }
      else out[o++] = s[i];
   }
   out[o] = '\0';
   return out;
}

/* Publishes the panel.  Called only when it comes up or goes down: the text
 * itself lives in the <input> from then on, so typing does not reparse the
 * document — which would restart every animation on it and drop the focus the
 * field needs to keep. */
static void ime_publish(bool up, int input_type, bool multiline,
                        const char *initial)
{
   if (!up) {
      luna_overlay_set_ime(NULL, NULL);
      return;
   }

   char value[IME_TEXT_MAX * 6 + 1];
   char html[IME_TEXT_MAX * 6 + 1024];
   ime_escape(initial, value, sizeof value);

   snprintf(html, sizeof html,
      "<div id=\"luna-ime\"><div class=\"luna-ime-inner\">"
      "<div class=\"luna-ime-head\">Lunaria input method — %s</div>"
      "<%s id=\"" IME_FIELD_ID "\" class=\"luna-ime-field\"%s value=\"%s\"></%s>"
      "<div class=\"luna-ime-row\">"
      "<span id=\"luna-ime-ok\" class=\"luna-ime-btn luna-ime-ok\">OK (Enter)</span>"
      "<span id=\"luna-ime-cancel\" class=\"luna-ime-btn\">Cancel (Esc)</span>"
      "<span class=\"luna-ime-hint\">type on the host keyboard — "
      "Ctrl+C / Ctrl+V share the desktop clipboard</span>"
      "</div></div></div>",
      ime_kind_label(input_type, multiline),
      multiline ? "textarea" : "input",
      ime_is_password(input_type) ? " type=\"password\"" : "",
      value,
      multiline ? "textarea" : "input");

   /* Anchored to the bottom of the surface, the same edge the band reported by
    * luna_ime_band_height() is measured from, and exactly that tall.
    *
    * The transparent body is not decoration: luna-ui gives every document a
    * full-surface backdrop element, and an opaque one there covers the frame
    * the guest has just drawn — the whole game, behind a keyboard that is only
    * supposed to take a third of the screen. */
   static const char css[] =
      "body{margin:0;background:transparent;}"
      /* Exactly the band, with nothing on the box itself that could change
         its height; the padding lives on the wrapper inside it. */
      "#luna-ime{position:absolute;left:0;right:0;bottom:0;height:240px;"
      "background:rgba(10,14,22,0.95);border-top:2px solid #3d6ea8;"
      "color:#e8eefc;}"
      ".luna-ime-inner{padding:16px 24px;}"
      ".luna-ime-head{font-size:14px;color:#7f96c0;padding-bottom:10px;}"
      ".luna-ime-field{display:block;width:100%;height:52px;"
      "background:#141c2c;border:1px solid #33456a;border-radius:8px;"
      "padding:8px 14px;font-size:26px;color:#ffffff;}"
      ".luna-ime-row{display:flex;align-items:center;padding-top:18px;}"
      ".luna-ime-btn{padding:9px 20px;margin-right:12px;border-radius:8px;"
      "background:#243350;color:#cfe0ff;font-size:16px;}"
      ".luna-ime-ok{background:#2d5c96;color:#ffffff;}"
      ".luna-ime-hint{color:#6b7fa6;font-size:14px;}";

   luna_overlay_set_ime(html, css);
}

/* ------------------------------------------------------------------------ *
 * dvm main-looper side
 * ------------------------------------------------------------------------ */

void luna_ime_show(const char *initial, int input_type, bool multiline)
{
   pthread_mutex_lock(&g_lock);
   g_input_type = input_type;
   g_multiline = multiline;
   snprintf(g_text, sizeof g_text, "%s", initial ? initial : "");
   g_events = 0;
   g_active = true;
   pthread_mutex_unlock(&g_lock);

   /* Keys pressed before the field existed are not its text. */
   pthread_mutex_lock(&g_queue_lock);
   g_queue_count = 0;
   pthread_mutex_unlock(&g_queue_lock);

   fprintf(stderr, "[ime] up: inputType=0x%x%s, seeded \"%s\"\n",
           input_type, multiline ? ", multi-line" : "", g_text);
   ime_publish(true, input_type, multiline, g_text);
}

void luna_ime_hide(void)
{
   pthread_mutex_lock(&g_lock);
   bool was = g_active;
   g_active = false;
   pthread_mutex_unlock(&g_lock);
   if (!was) return;
   fprintf(stderr, "[ime] down\n");
   ime_publish(false, 0, false, NULL);
}

bool luna_ime_active(void)
{
   pthread_mutex_lock(&g_lock);
   bool up = g_active;
   pthread_mutex_unlock(&g_lock);
   return up;
}

int luna_ime_pump(char *text_out, size_t cap)
{
   pthread_mutex_lock(&g_lock);
   int events = g_events;
   g_events = 0;
   if (text_out && cap) snprintf(text_out, cap, "%s", g_text);
   pthread_mutex_unlock(&g_lock);
   return events;
}

/* ------------------------------------------------------------------------ *
 * Host event thread
 * ------------------------------------------------------------------------ */

static void ime_enqueue(const struct ime_queued *ev)
{
   if (!luna_ime_active()) return;
   pthread_mutex_lock(&g_queue_lock);
   if (g_queue_count < (int)(sizeof g_queue / sizeof g_queue[0]))
      g_queue[g_queue_count++] = *ev;
   pthread_mutex_unlock(&g_queue_lock);
}

void luna_ime_key(int key, int scancode, int action, int mods)
{
   struct ime_queued ev = { .kind = IME_Q_KEY, .key = key,
                            .scancode = scancode, .action = action,
                            .mods = mods };
   ime_enqueue(&ev);
}

void luna_ime_char(uint32_t codepoint)
{
   struct ime_queued ev = { .kind = IME_Q_CHAR, .codepoint = codepoint };
   ime_enqueue(&ev);
}

/* ------------------------------------------------------------------------ *
 * Presenting thread
 * ------------------------------------------------------------------------ */

bool luna_ime_click(const char *id)
{
   if (!id || strncmp(id, "luna-ime", 8)) return false;
   int ev = 0;
   if (!strcmp(id, "luna-ime-ok"))
      ev = LUNA_IME_EV_ACCEPT;
   else if (!strcmp(id, "luna-ime-cancel"))
      ev = LUNA_IME_EV_CANCEL;
   if (ev) {
      pthread_mutex_lock(&g_lock);
      if (g_active) g_events |= ev;
      pthread_mutex_unlock(&g_lock);
   }
   return true;
}

void luna_ime_frame(bool document_reparsed)
{
   pthread_mutex_lock(&g_lock);
   bool up = g_active, ml = g_multiline;
   char text[IME_TEXT_MAX];
   memcpy(text, g_text, sizeof text);
   pthread_mutex_unlock(&g_lock);
   if (!up) { g_field_idx = -1; return; }

   if (document_reparsed || g_field_idx < 0) {
      g_field_idx = luna_get_element_by_id(IME_FIELD_ID);
      if (g_field_idx >= 0) {
         /* The markup that was just parsed carries the value the field had
          * when it was published, not what has been typed into it since. */
         luna_set_value(g_field_idx, text);
         luna_focus_element(g_field_idx);
      }
   }
   if (g_field_idx < 0) return;

   /* The panel is the only thing that can hold focus while it is up; a click
    * elsewhere in the overlay would otherwise leave the field unable to type
    * with nothing on screen saying so. */
   if (luna_focused_element() != g_field_idx) luna_focus_element(g_field_idx);

   struct ime_queued batch[128];
   int n;
   pthread_mutex_lock(&g_queue_lock);
   n = g_queue_count;
   memcpy(batch, g_queue, (size_t)n * sizeof batch[0]);
   g_queue_count = 0;
   pthread_mutex_unlock(&g_queue_lock);

   bool cancelled = false, accepted = false;
   for (int i = 0; i < n; ++i) {
      if (batch[i].kind == IME_Q_CHAR) {
         luna_char(batch[i].codepoint);
         continue;
      }
      /* Escape is the input method's own dismissal, not the editor's: a device
       * has the Back key for it and luna-ui has no meaning for it at all. */
      if (batch[i].key == LUNA_KEY_ESCAPE) {
         if (batch[i].action == LUNA_PRESS) cancelled = true;
         continue;
      }
      /* Enter on a single-line field is the editor action.  Handled here and
       * not through the field's click handler, which luna-ui also fires when
       * the field is clicked — and clicking a text box to put the caret in it
       * must not submit it. */
      if ((batch[i].key == LUNA_KEY_ENTER || batch[i].key == LUNA_KEY_KP_ENTER) &&
          !ml) {
         if (batch[i].action == LUNA_PRESS) accepted = true;
         continue;
      }
      luna_key(batch[i].key, batch[i].scancode, batch[i].action, batch[i].mods);
   }

   const char *now = luna_get_value(g_field_idx);
   bool changed = now && strcmp(now, text) != 0;

   if (changed || cancelled || accepted) {
      pthread_mutex_lock(&g_lock);
      if (changed) {
         snprintf(g_text, sizeof g_text, "%s", now);
         g_events |= LUNA_IME_EV_TEXT;
      }
      if (accepted)  g_events |= LUNA_IME_EV_ACCEPT;
      if (cancelled) g_events |= LUNA_IME_EV_CANCEL;
      pthread_mutex_unlock(&g_lock);
   }
}
