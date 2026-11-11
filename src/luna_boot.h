/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * * SPDX-License-Identifier: MPL-2.0
 * * Host presentation, input, boot card and launcher interfaces.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Boot card and launcher */
/* dynarmic's compile counters, from the GetBlock hook.  Returns true while the
 * card should stay on screen — the caller starts its presenter thread on the
 * first true.  Safe from any thread. */
bool luna_boot_jit_update(uint64_t compiles, uint64_t compile_ns);

/* Dex compilation.  The loader knows how many classes*.dex the package has,
 * and how large they are, before it opens the first — the one phase of boot
 * with a real denominator. */
void luna_boot_dex_total(int files, uint64_t bytes);
void luna_boot_dex_loaded(const char *path, uint32_t classes,
                          uint32_t methods, uint64_t bytes);

/* True while the card is published and has not been taken down.  The presenter
 * that draws it uses this to decide whether to run: the card has to be redrawn
 * for as long as it is on screen, not only while the JIT hook happens to be
 * firing.  Before this existed the card was presented about forty times in
 * total and then stood still for the rest of the boot — an animation nobody
 * ever saw finish. */
bool luna_boot_active(void);

/* Guest eglSwapBuffers.  Dismisses the card once dex is finished and the guest
 * has presented a couple of frames — the first is often blank while libUE4 is
 * still wiring up its renderer. */
void luna_boot_guest_presented(void);

/* Takes the card down for good. */
void luna_boot_finish(const char *why);

/* Installation/profile UI and native-window handoff. */
int luna_launcher_begin(const char *path);
void luna_launcher_progress(const char *stage, const char *file, uint64_t bytes, uint64_t total);
int luna_launcher_cancelled(void);
int luna_launcher_choose(const char *package, const char *const *profiles, const char *const *labels,
                         size_t count, char *choice, size_t capacity);
/* One setting the launcher screen offers next to the application: a key of
 * lunaria.conf, what to call it, and its value (filled in by the caller,
 * edited by the person, read back).  `folder` adds a Browse button that picks
 * a directory. */
typedef struct luna_launcher_setting {
  const char *key, *label, *hint;
  int folder;
  int compact;      /* shares a row with the neighbouring compact settings */
  const char *const *choices; /* NULL-terminated values, or NULL for text */
  const char *const *choice_labels; /* optional display names for choices */
  char value[1024];
} luna_launcher_setting;

/* Started with no application: asks which to open and how this installation
 * is set up (path field, luna-ui's file dialog for Browse, drag-and-drop).
 * 0 with the application in `path` and `settings` as edited; -1 when
 * cancelled or there is no display to ask on. */
int luna_launcher_pick(char *path, size_t capacity,
                       luna_launcher_setting *settings, size_t count);
int luna_launcher_pick_named(char *path, size_t capacity, const char *title,
                             const char *note);
void luna_launcher_end(int keep_window);
void *luna_launcher_take_window(void);

/* APKPure discovery and streaming downloads, using the host HTTP client. */
#define APKPURE_RESULTS 8
struct apkpure_result {
  char package[256], title[256];
};
/* Return nonzero from progress to cancel. Called on the worker, never the UI. */
typedef int (*apkpure_progress)(void *, uint64_t, uint64_t);
int apkpure_package_valid(const char *name);
int apkpure_search_url(const char *query, char *url, size_t capacity);
int apkpure_parse_results(const char *html, struct apkpure_result *out, size_t capacity);
int apkpure_search(const char *query, struct apkpure_result *out, size_t capacity,
                   char *error, size_t error_size, apkpure_progress progress, void *context);
int apkpure_download(const char *package, const char *directory, char *path, size_t capacity,
                     char *error, size_t error_size, apkpure_progress progress, void *context);


/* The running app's profile: what exists, which is in use, a fresh name, and
 * starting over under another (returns only when it could not). */
const char *luna_apk_profile_current(void);
int luna_apk_profile_list(char (*names)[65], int max);
int luna_apk_profile_new_name(char *out, size_t cap);
int luna_apk_restart(const char *profile);
/* Starts over with no APK, showing the launcher screen. */
int luna_apk_restart_launcher(void);
/* Optional Google packages and their account settings share the current
 * installation's data root. Windows are opened in a separate process. */
int luna_apk_google_services_installed(void);
int luna_apk_google_package_installed(const char *package);
int luna_apk_google_services_busy(void);
int luna_apk_google_services_open(int setup, const char *profile);
int luna_apk_google_profile_list(char (*names)[65], int max);
int luna_apk_google_profile_accounts(const char *profile);
int luna_apk_google_profile_create(char *out, size_t cap);
/* Retains the original archive, including splits, for component hosts. */
int luna_apk_system_archive(const char *package, char *out, size_t cap);
/* Caller owns the returned manifest service table. */
int luna_apk_service_manifest(const char *apk, char **services, char **providers, char *process, size_t cap);

/* Touch input, keymaps and recording */
#define LUNA_TOUCH_MAX 10
typedef struct luna_touch_point {
  int id;
  float x, y;
} luna_touch_point;
typedef struct luna_touch_event {
  int action;
  float x, y; /* pointer zero, also used by legacy single-pointer producers */
  long long event_ms, down_ms;
  int pointer_count;
  luna_touch_point pointers[LUNA_TOUCH_MAX];
} luna_touch_event;
typedef struct luna_touch_state {
  int count;
  long long down_ms;
  luna_touch_point points[LUNA_TOUCH_MAX];
} luna_touch_state;
/* action: DOWN=0, UP=1, MOVE=2, CANCEL=3; id identifies a stable contact.
 * Returns a complete immutable Android event, including the lifting pointer
 * on POINTER_UP.  Pointer indices can change; pointer IDs do not. */
int luna_touch_update(luna_touch_state *, int id, int action, float x, float y,
                      long long now_ms, luna_touch_event *out);
int luna_event_count(const luna_touch_event *);
int luna_event_id(const luna_touch_event *, int index);
float luna_event_x(const luna_touch_event *, int index);
float luna_event_y(const luna_touch_event *, int index);
#define LUNA_KEYMAP_BUTTONS 8
#define LUNA_KEY_POINTS 4      /* contacts one key can press at the same time */
#define LUNA_KEYMAP_MACROS 8
#define LUNA_MACRO_STEPS 24
#define LUNA_TIMED_MAX 256
/* button KEY x y [x y ...] [repeat=MS] [hold=MS]
 *   Every point is pressed together (a simultaneous tap).  With repeat= the
 *   points are tapped again every MS milliseconds while the key is held, each
 *   tap lasting hold= milliseconds.  x/y mirror the first point. */
typedef struct luna_key_binding {
  int key, held;
  float x, y;
  int npoints, id, repeat_ms, hold_ms, phase;
  float px[LUNA_KEY_POINTS], py[LUNA_KEY_POINTS];
  long long cycle_ms;
} luna_key_binding;
/* macro KEY step... [loop]
 *   A step is "x,y[|x,y...][/hold_ms]" (taps those points together) or
 *   "@ms" (waits).  The sequence runs once per press; "loop" keeps running it
 *   while the key is held. */
typedef struct luna_macro_step {
  int npoints, hold_ms, wait_ms;
  float x[LUNA_KEY_POINTS], y[LUNA_KEY_POINTS];
} luna_macro_step;
typedef struct luna_macro {
  int key, held, loop, nsteps, id, pending;
  unsigned down;                 /* contacts currently pressed, by offset */
  luna_macro_step steps[LUNA_MACRO_STEPS];
} luna_macro;
typedef struct luna_timed {
  long long at_ms;
  int id, action, macro;
  float x, y;
} luna_timed;
typedef struct luna_keymap {
  int enabled, direction[4], stick_down, count, nmacros, nqueue;
  float stick_x, stick_y, radius;
  int walk_key, walk_held;
  float walk_radius;
  luna_key_binding buttons[LUNA_KEYMAP_BUTTONS];
  luna_macro macros[LUNA_KEYMAP_MACROS];
  luna_timed queue[LUNA_TIMED_MAX];
} luna_keymap;
typedef void (*luna_keymap_emit)(void *, int id, int action, float x, float y);
/* Selection can come from the menu thread. The input pump takes a validated
 * replacement and releases old contacts before applying it. */
int luna_keymap_select(const char *path);
void luna_keymap_current(char *out, size_t capacity);
int luna_keymap_take(luna_keymap *out);
/* Profiles are ordinary normalized touch layouts; no guest-specific behavior. */
int luna_keymap_load(luna_keymap *, const char *profile);
int luna_keymap_key(luna_keymap *, int key, int pressed, float width, float height,
                    luna_keymap_emit, void *);
void luna_keymap_release(luna_keymap *, float width, float height, luna_keymap_emit, void *);

/* Everything that happens on a clock rather than a key event: repeat taps,
 * macro steps and the replay of a recording.  Call it regularly from the
 * thread that delivers keys (it emits through the same callback). */
void luna_input_tick(luna_keymap *, float width, float height, luna_keymap_emit, void *);
/* Tests substitute the clock (milliseconds, monotonic). */
void luna_input_set_clock(long long (*now_ms)(void));

/* Recording keeps every touch contact, whatever produced it, as text lines
 * "ms id action x y" with x,y as fractions of the view, so a recording replays
 * at any resolution.  Replay goes through the emit callback and is not itself
 * recorded.  luna_input_record_event is safe from any thread. */
int luna_record_start(const char *path);
void luna_record_stop(void);
int luna_record_active(void);
void luna_record_event(int id, int action, float nx, float ny);
int luna_play_start(const char *path, int loop);
void luna_play_stop(void);
int luna_play_active(void);
void luna_record_last_path(char *out, size_t capacity);

/* Framework windows and overlay */
/* Draws the overlay into the current context, immediately before the guest's
 * eglSwapBuffers.  A no-op until a window is pushed, so a title that never
 * shows one pays a branch per frame.  `w`/`h` are the surface's pixel size.
 *
 * This is the *application* screen: the guest's own windows with the input
 * method over them, composited onto the frame the guest has just drawn.  It
 * draws nothing while the boot card is up — the card is a screen of its own
 * and the two never share a frame. */
void luna_overlay_present(int w, int h);
/* False means contention skipped the required overlay. The compositor must
 * not swap its cleared/incomplete back buffer in that case. */
bool luna_overlay_present_checked(int w,int h,bool boot);

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

/* Native game key mappings belong only to the focused surface window. */
void luna_overlay_set_native_input_focus(bool focused);
bool luna_overlay_guest_keys_captured(void);

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
/* Moves one element of the published document (absolute left/top in px) without
 * republishing it; false when the document is not up or is about to be replaced. */
bool luna_overlay_move(const char *id, float left, float top);

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
/* True for the modal context menu; a transient notice does not take input. */
bool luna_overlay_menu_showing(void);
typedef void (*luna_overlay_menu_fn)(const char *id);
void luna_overlay_set_menu_handler(luna_overlay_menu_fn fn);

/* The menu itself: screenshots, clipboard, sound, WebView engine/zoom, full
 * screen — built on the layer above, in the style of a macOS context menu. */
/* Opens the menu at a surface position (the window thread's right click).
 * w/h is the surface, to keep the menu on it. */
void luna_menu_open(double x, double y, int w, int h);
int luna_overlay_screenshot(const char *path);
void luna_menu_close(void);
/* Session-only execution control. The host compositor keeps running. */
bool luna_guest_paused(void);
void luna_guest_set_paused(bool paused);
/* Once per window-thread poll: expires the notice line. */
void luna_menu_tick(void);
/* F9 / F10: start or stop recording touch input; replay the last recording. */
void luna_input_record_toggle(void);
void luna_input_play_toggle(void);


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
/* Called on the presenting thread while a press is dragged past the touch slop:
 * (x, y) is where the finger is and (dx, dy) how far the *content* should move
 * (the opposite of the finger), since the last call.  The press then never
 * becomes a click.  The widget layer scrolls whatever scrollable view is under
 * the finger. */
typedef void (*luna_overlay_scroll_fn)(double x, double y, double dx, double dy);
void luna_overlay_set_scroll_handler(luna_overlay_scroll_fn fn);
/* Called on the presenting thread for pointer events within a WebView DOM
 * element. x/y are local image pixels; the receiver queues them for the
 * Android main looper. */
typedef void (*luna_overlay_web_pointer_fn)(const char *id,
    int x, int y, int action, long long event_ms);
void luna_overlay_set_web_pointer_handler(luna_overlay_web_pointer_fn fn);
typedef void (*luna_overlay_web_char_fn)(const char *id, uint32_t codepoint);
typedef void (*luna_overlay_web_key_fn)(const char *id, int key, int action, int mods);
typedef void (*luna_overlay_web_text_fn)(const char *id, const char *utf8);
void luna_overlay_set_web_text_handlers(luna_overlay_web_char_fn char_fn,
                                        luna_overlay_web_key_fn key_fn,
                                        luna_overlay_web_text_fn text_fn);
/* Host keyboard callbacks use these after a WebView was focused by touch. */
bool luna_overlay_web_char(uint32_t codepoint);
bool luna_overlay_host_key(int key, int action, int mods);
bool luna_overlay_host_char(uint32_t codepoint);
bool luna_overlay_web_key(int key, int action, int mods);
bool luna_overlay_web_text(const char *utf8);

/* Called on the presenting thread, with the document parsed and luna-ui's
 * state live, immediately before the frame is drawn.  It is the only place a
 * caller may touch luna-ui's element API.  The boot card updates its text and
 * its progress bar here rather than by republishing the document, which would
 * restart every CSS animation on the page. */
typedef void (*luna_overlay_frame_fn)(void);
void luna_overlay_set_frame_handler(luna_overlay_frame_fn fn);

/* Drawn by the window's compositor on its own thread and context rather than
 * inside a guest swap (see luna_boot.h). */
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
/* Install the shared font, clock and GL callbacks for a pre-guest luna-ui host. */
void luna_overlay_prepare_platform(void);

/* Decodes a PNG/JPEG/WebP-less image held in memory to 8-bit RGBA with the
 * decoder luna-ui already links; NULL when it cannot.  Free the result with
 * luna_overlay_image_free(). */
unsigned char *luna_overlay_image_decode(const void *data, size_t len,
    int *w, int *h);
void luna_overlay_image_free(unsigned char *pixels);
/* Copies one ARGB Bitmap generation. Pending updates of the same key coalesce
 * and are uploaded by the presenting thread; no image file or codec. */
bool luna_overlay_image_argb(const char *key,int width,int height,const uint32_t *pixels);
bool luna_overlay_image_rgba(const char *key,int width,int height,const uint8_t *rgba_top_down);
bool luna_overlay_image_argb_rect(const char *key,int width,int height,const uint32_t *pixels,
                                  int x0,int y0,int x1,int y1);
bool luna_overlay_image_size(const char *key,int *width,int *height);
void luna_overlay_image_forget(const char *key);
/* Native image consumers run on the compositor, without a VM lock. A successful
 * registration owns user until replacement/removal; callbacks must not use Java. */
typedef void (*luna_overlay_image_pull_fn)(void *user,const char *key,int64_t present_ns);
bool luna_overlay_image_source(const char *key,void *user,luna_overlay_image_pull_fn pull,
                               void (*release)(void *));
void luna_overlay_image_sources_remove(void *user);
bool luna_overlay_has_image_sources(void);
void luna_overlay_poll_image_sources(int64_t present_ns);


/* The width of one line of `text` at `px`, and the line height at `px`, in
 * the overlay's own type — what an Android TextView has to measure against
 * for the box the overlay then draws it in to fit.  Any thread; serialised
 * with the overlay's render. */
/* CPU text runs shared by Android measurement and Bitmap Canvas drawing. */
struct luna_text_glyph {
  const void *font,*font_data;
  uint32_t glyph,cluster;
  float x,y,advance;
};
struct luna_text_run {
  struct luna_text_glyph *glyphs;
  size_t count;
  float width;
  /* Unrounded baseline metrics of the faces actually used for this run. */
  float ascent,descent;
  bool missing;
};
struct luna_text_bitmap {
  unsigned char *coverage;
  int width,height,left,top;
  void *cache; /* owner entry while the coverage is shared; release with luna_overlay_free_glyph */
};
/* Immutable copied font data, cached until host shutdown. Zero is the system
 * face. Handles remain valid across platform-font reloads and glyph runs. */
uint32_t luna_overlay_font_load(const void *data,size_t size,int index);
/* Borrow the selected host system faces (regular, bold, CJK, mono). */
bool luna_overlay_system_font(unsigned role,const void **data,size_t *size);
/* direction: 0=automatic, 1=LTR, 2=RTL. */
bool luna_overlay_shape_font_direction(uint32_t font,const uint16_t *text,size_t length,float px,bool bold,int direction,struct luna_text_run *run);
/* Shape only [start,start+count), retaining the surrounding UTF-16 context.
 * Returned cluster offsets are relative to start. */
bool luna_overlay_shape_font_context(uint32_t font,const uint16_t *text,size_t length,size_t start,size_t count,float px,bool bold,int direction,struct luna_text_run *run);
bool luna_overlay_shape_font(uint32_t font,const uint16_t *text,size_t length,float px,bool bold,struct luna_text_run *run);
bool luna_overlay_shape_text(const uint16_t *text,size_t length,float px,bool bold,struct luna_text_run *run);
bool luna_overlay_raster_glyph(const struct luna_text_glyph *glyph,float px,struct luna_text_bitmap *bitmap);
void luna_overlay_free_glyph(struct luna_text_bitmap *bitmap);
/* At each UTF-16 boundary: bit 0 line opportunity, bit 1 grapheme boundary,
 * bit 2 mandatory line break. Caller supplies length+1 bytes. */
bool luna_overlay_text_breaks(const uint16_t *text,size_t length,const char *locale,unsigned char *breaks);
float luna_overlay_text_width(const char *text, float px, bool bold);
float luna_overlay_line_height(float px);
struct luna_overlay_font_metrics {
  float top, ascent, descent, bottom, leading;
};
/* Face metrics in Android's baseline coordinate system, without GL work. */
bool luna_overlay_font_metrics(uint32_t font,float px,bool bold,
                               struct luna_overlay_font_metrics *metrics);
bool luna_overlay_get_font_metrics(float px, bool bold,
                                   struct luna_overlay_font_metrics *metrics);

/* Input method */
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

/* Host window compositor */
/* Starts the compositor on `native_window`.  The compositor creates its own
 * context — in a share group of its own, apart from the guest's — and its own
 * window surface; the caller must not make that window current anywhere.
 * False when it could not start — the caller then presents the way it did
 * without one. */
bool luna_comp_start(void *egl_display, void *egl_config, void *native_window);
/* Stop and join the display thread before VM or driver teardown. */
void luna_comp_stop(void);

/* True once started and running. */
bool luna_comp_active(void);

/* Saves the next frame the compositor puts on screen to `path` (a .png): the
 * window as the user sees it, emulator UI included.  Any thread; the file is
 * written on the compositor's next present.  False when not running. */
bool luna_comp_shot(const char *path);

/* An 8-bit RGB PNG from `w` x `h` RGBA pixels; `bottom_up` for glReadPixels
 * order.  False on any failure. */
bool luna_png_write_rgba(const char *path, int w, int h, const uint8_t *rgba, bool bottom_up);

/* Guest side, on the thread and context that just finished the frame, with
 * the frame in the default framebuffer of the current draw surface (w x h).
 * Copies it into a free slot and queues it.  `swap_interval` is the guest's
 * eglSwapInterval. */
void luna_comp_submit(int w, int h, int swap_interval);

/* A frame that is already in host memory: `w` x `h` pixels of four bytes,
 * rows `stride` bytes apart, top row first (Vulkan's order).  `bgra` says the
 * bytes are B,G,R,A rather than R,G,B,A.  Any thread; the pixels are copied
 * before this returns.  With `swap_interval` 1 or more it waits, bounded,
 * while the previous frame has not been taken yet. */
void luna_comp_submit_pixels(int w, int h, const void *pixels, int stride,
                             bool bgra, int swap_interval);

/* The same, for rows bottom row first (what glReadPixels returns): the pixels
 * are already in the compositor's texture order, so nothing is flipped. */
void luna_comp_submit_pixels_bottom_up(int w, int h, const void *pixels,
                                       int stride, bool bgra, int swap_interval);

/* Something the compositor draws besides the guest's frame changed (a
 * document, a keystroke, a pointer event): draw a frame soon. */
void luna_comp_wake(void);

/* Frames the compositor has put on screen. */
unsigned long long luna_comp_frames(void);

#ifdef __cplusplus
}
#endif
