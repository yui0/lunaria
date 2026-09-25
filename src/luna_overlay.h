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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draws the overlay into the current context, immediately before the guest's
 * eglSwapBuffers.  A no-op until a window is pushed, so a title that never
 * shows one pays a branch per frame.  `w`/`h` are the surface's pixel size.
 *
 * This is the *application* screen: the guest's own windows with the input
 * method over them, composited onto the frame the guest has just drawn.  It
 * draws nothing while the boot card is up — the card is a screen of its own
 * and the two never share a frame. */
void luna_overlay_present(int w, int h);

/* The boot card's screen, drawn by the card's own presenter.  It is the whole
 * display: it clears first and the guest's frame is not underneath it. */
void luna_overlay_present_boot(int w, int h);

/* True while at least one window is up.  Input routing asks this before
 * handing a touch to the guest. */
bool luna_overlay_active(void);

/* True while a window from the guest's own View layer is up — a dialog, a
 * notification banner — as opposed to the emulator's status card or the IME.
 *
 * The compositor on a device draws such a window whether or not the
 * application is still rendering.  Here the overlay is drawn inside the
 * guest's eglSwapBuffers, so a guest that stops presenting takes its own
 * dialog off the screen with it — and a guest that is *waiting for the answer
 * to that dialog* can then never show it.  The frame pump uses this to put
 * the window up itself in that case. */
bool luna_overlay_guest_window_up(void);

/* Pointer/touch in surface pixels.  Returns true when the overlay consumed the
 * event, which is when the guest must not also see it. */
bool luna_overlay_pointer(double x, double y, int action);

/* Replaces the overlay document.  `css` may be NULL to keep the current sheet.
 * Passing NULL html takes the overlay down.  Safe to call from any thread: the
 * strings are copied and the document is parsed on the thread that presents.
 *
 * Guest widgets (dialogs, etc.) use this path.  While a guest document is up
 * it takes priority over the JIT status card below. */
void luna_overlay_set_document(const char *html, const char *css);

/* The input method's panel (luna_ime.c).  It is a third layer, above both the
 * guest's widgets and the status card, because on a device the IME is its own
 * window and is composited over whatever the app has up — including a dialog.
 * NULL html takes it down. */
void luna_overlay_set_ime(const char *html, const char *css);

/* A Toast: a transient message above the application (and its dialogs) and
 * below the input method.  It never takes pointer input.  NULL html clears
 * it. */
void luna_overlay_set_toast(const char *html, const char *css);

/* The emulator's own menu (right click): the topmost layer, over the boot
 * card too.  While a modal one is up every pointer event goes to it (a
 * non-modal one is a notice that lets touches through).  Its
 * elements' ids start with "luna-menu"; a click on one is handed to the menu
 * handler instead of the guest's click handler.  NULL html takes it down. */
void luna_overlay_set_menu(const char *html, const char *css, bool modal);
bool luna_overlay_menu_showing(void);
typedef void (*luna_overlay_menu_fn)(const char *id);
void luna_overlay_set_menu_handler(luna_overlay_menu_fn fn);

/* The menu itself: screenshots, clipboard, sound, WebView engine/zoom, full
 * screen — built on the layer above, in the style of a macOS context menu. */
/* Opens the menu at a surface position (the window thread's right click).
 * w/h is the surface, to keep the menu on it. */
void luna_menu_open(double x, double y, int w, int h);
void luna_menu_close(void);
/* Once per window-thread poll: expires the notice line. */
void luna_menu_tick(void);


/* Boot / JIT status card.  Shown only when no guest document is up, so a
 * terms-of-service dialog is never covered by "Translating ARM".  Does not
 * steal pointer input.  NULL html clears the card. */
void luna_overlay_set_status(const char *html, const char *css);

/* True while the status card (not a guest document) is what present() would
 * draw.  The mid-compile presenter uses this to decide whether a host swap
 * is showing the progress UI. */
bool luna_overlay_status_showing(void);

/* Called with the DOM id of an element the user clicked, from the thread that
 * presents the overlay.  The emulator's widget layer uses it to find the guest
 * View the element stands for. */
typedef void (*luna_overlay_click_fn)(const char *id);
void luna_overlay_set_click_handler(luna_overlay_click_fn fn);
/* Called on the presenting thread for pointer events within a WebView DOM
 * element. x/y are local image pixels; the receiver queues them for the
 * Android main looper. */
typedef void (*luna_overlay_web_pointer_fn)(const char *id,
                                            int x, int y, int action);
void luna_overlay_set_web_pointer_handler(luna_overlay_web_pointer_fn fn);
typedef void (*luna_overlay_web_char_fn)(const char *id, uint32_t codepoint);
typedef void (*luna_overlay_web_key_fn)(const char *id, int key, int action);
void luna_overlay_set_web_text_handlers(luna_overlay_web_char_fn char_fn,
                                        luna_overlay_web_key_fn key_fn);
/* Host keyboard callbacks use these after a WebView was focused by touch. */
bool luna_overlay_web_char(uint32_t codepoint);
bool luna_overlay_web_key(int key, int action);

/* Called on the presenting thread, with the document parsed and luna-ui's
 * state live, immediately before the frame is drawn.  It is the only place a
 * caller may touch luna-ui's element API.  The boot card updates its text and
 * its progress bar here rather than by republishing the document, which would
 * restart every CSS animation on the page. */
typedef void (*luna_overlay_frame_fn)(void);
void luna_overlay_set_frame_handler(luna_overlay_frame_fn fn);

/* Drawn by the window's compositor on its own thread and context rather than
 * inside a guest swap (see luna_compositor.h). */
void luna_overlay_set_hosted(bool hosted);
/* Called on the compositor's thread before its first frame. */
void luna_overlay_bind_host_thread(void);

/* Called whenever something the overlay draws changes — a document, a
 * pointer event, a keystroke — so the compositor draws a frame for it. */
void luna_overlay_set_wake(void (*fn)(void));
void luna_overlay_wake(void);

/* A producer replaced an image file used by the guest document. The texture
 * cache is invalidated on the presenting thread, where the GL context lives. */
void luna_overlay_image_changed(const char *path);

/* Releases the GL objects and the document.  Safe to call without a context;
 * it only forgets state in that case. */
void luna_overlay_shutdown(void);

/* Decodes a PNG/JPEG/WebP-less image held in memory to 8-bit RGBA with the
 * decoder luna-ui already links; NULL when it cannot.  Free the result with
 * luna_overlay_image_free(). */
unsigned char *luna_overlay_image_decode(const void *data, size_t len,
                                         int *w, int *h);
void luna_overlay_image_free(unsigned char *pixels);

/* The width of one line of `text` at `px`, and the line height at `px`, in
 * the overlay's own type — what an Android TextView has to measure against
 * for the box the overlay then draws it in to fit.  Any thread; serialised
 * with the overlay's render. */
float luna_overlay_text_width(const char *text, float px, bool bold);
float luna_overlay_line_height(float px);



#ifdef __cplusplus
}
#endif
