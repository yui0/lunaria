/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The emulator's input method.
 *
 * Android has no keyboard of its own.  When an app wants text it asks the
 * *system* to put an input method on screen, and everything it sees afterwards
 * — the characters, the editor action, the window becoming shorter to make
 * room — comes from that separate window.  Lunaria had no such window, so
 * InputMethodManager.showSoftInput() had nothing to raise, no host keystroke
 * had anywhere to go, and a title that gates progress on a name or a coupon
 * code stopped there.  The Java side of the field was complete; the input
 * method was the piece that did not exist.
 *
 * This is that input method.  It is a real one in the sense that matters here:
 *
 *  - It owns a band at the bottom of the surface and *reports* that band
 *    (luna_ime_band_height), which is what View.getWindowVisibleDisplayFrame
 *    answers with.  UE4's GameActivity decides whether a keyboard is up by
 *    measuring how much shorter the window got — an input method that lied
 *    about its size is one the app would never believe in.
 *  - The field is a luna-ui <input>: luna-ui already implements the caret,
 *    the selection, backspace, the arrow keys and password masking, so the
 *    editing is a real editor's rather than a second, poorer copy of one.
 *  - It draws in the overlay's own GL context, never the guest's.
 *
 * The prefix is `luna_ime_*` for the emulator's input method as a whole;
 * luna-ui's own `luna_ime_preedit` / `luna_ime_set_preedit` are a different
 * thing (the host platform's composition state) and are not used here.
 *
 * Threading.  Three threads touch this:
 *   host event thread  — luna_ime_key / luna_ime_char, queue only.
 *   presenting thread  — luna_ime_frame(), the only place luna-ui is touched.
 *   dvm main looper    — luna_ime_show/hide/pump, which talk to the guest.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What luna_ime_pump() reports, as a bitmask. */
enum {
   LUNA_IME_EV_TEXT   = 1 << 0,   /* the buffer changed */
   LUNA_IME_EV_ACCEPT = 1 << 1,   /* the editor action fired (Enter / OK) */
   LUNA_IME_EV_CANCEL = 1 << 2,   /* dismissed without accepting (Esc) */
};

/* Raises the input method over `initial`, with the Android input type the
 * field asked for (TYPE_* bits — the password variation is honoured) and the
 * field's own single/multi-line setting. */
void luna_ime_show(const char *initial, int input_type, bool multiline);
void luna_ime_hide(void);
bool luna_ime_active(void);

/* Height of the band the input method occupies on a surface `surface_h` tall,
 * in surface pixels; zero while it is down. */
int luna_ime_band_height(int surface_h);

/* Applies what the host queued and reports it.  `text_out` receives the
 * current contents (always NUL-terminated when cap > 0).  Returns a mask of
 * LUNA_IME_EV_*.  dvm main-looper thread. */
int luna_ime_pump(char *text_out, size_t cap);

/* Host event thread.  `key`/`action`/`mods` use luna-ui's numbering, which is
 * GLFW's, so the host callback can pass its arguments straight through. */
void luna_ime_key(int key, int scancode, int action, int mods);
void luna_ime_char(uint32_t codepoint);

/* Presenting thread, from luna_overlay_present() with the document parsed and
 * luna-ui live.  Drains the host queue into the <input> and reads it back.
 *
 * `document_reparsed` is true on the first frame after the overlay rebuilt its
 * DOM: the <input> is a new element then, holding the value the published
 * markup carried rather than what has been typed since, so that is the frame
 * on which the field has to be found, refilled and focused again. */
void luna_ime_frame(bool document_reparsed);

/* True when the overlay element `id` belongs to the input method — it has been
 * acted on, and the widget layer must not also look for a guest View behind
 * it.  Presenting thread. */
bool luna_ime_click(const char *id);

#ifdef __cplusplus
}
#endif
