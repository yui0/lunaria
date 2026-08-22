/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The emulator's own UI surface, composited over the guest's frame.
 *
 * Android draws its widgets into windows the system composites above the
 * application's surface; a game that shows a dialog is still rendering its own
 * GL underneath.  Lunaria had no equivalent, so every framework View was
 * bookkeeping that never reached a pixel — which is why a title that gates its
 * boot on an SDK dialog (Netmarble's terms-of-service agreement) simply stopped
 * with nothing on screen.
 *
 * This is that missing layer.  It is backed by luna-ui, the HTML/CSS engine
 * already in this tree: the DOM is the widget tree, CSS is the styling, and its
 * OpenGL renderer draws into whatever context is current — which at present
 * time is the guest's own default framebuffer.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draws the overlay into the current context, immediately before the guest's
 * eglSwapBuffers.  A no-op until a window is pushed, so a title that never
 * shows one pays a branch per frame.  `w`/`h` are the surface's pixel size. */
void luna_overlay_present(int w, int h);

/* True while at least one window is up.  Input routing asks this before
 * handing a touch to the guest. */
bool luna_overlay_active(void);

/* Pointer/touch in surface pixels.  Returns true when the overlay consumed the
 * event, which is when the guest must not also see it. */
bool luna_overlay_pointer(double x, double y, int action);

/* Replaces the overlay document.  `css` may be NULL to keep the current sheet.
 * Passing NULL html takes the overlay down.  Safe to call from any thread: the
 * strings are copied and the document is parsed on the thread that presents. */
void luna_overlay_set_document(const char *html, const char *css);

/* Called with the DOM id of an element the user clicked, from the thread that
 * presents the overlay.  The emulator's widget layer uses it to find the guest
 * View the element stands for. */
typedef void (*luna_overlay_click_fn)(const char *id);
void luna_overlay_set_click_handler(luna_overlay_click_fn fn);

/* Releases the GL objects and the document.  Safe to call without a context;
 * it only forgets state in that case. */
void luna_overlay_shutdown(void);

#ifdef __cplusplus
}
#endif
