/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The emulator's boot card.
 *
 * Cold starts spend a long stretch with nothing on screen: dynarmic
 * translating, and — for a title that ships its Java as multidex — the
 * bytecode emulator compiling classes*.dex.  Blade & Soul Revolution is
 * 28 MB of dex across four files on top of a 200 MB library's static
 * initialisers.  A blank window through all of that is indistinguishable
 * from a hang, and the one thing a person watching wants to know (is it
 * still making progress, and on what) was only ever in the log.
 *
 * This is the card that covers it: a moon drawn and animated in CSS through
 * luna-ui, with the two progress sources under it.  It publishes through
 * luna_overlay_set_status(), so a guest dialog always takes priority over it,
 * and it comes down once the guest has a frame of its own.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
int luna_launcher_choose(const char *package, const char *const *profiles,
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
   char value[1024];
} luna_launcher_setting;

/* Started with no application: asks which to open and how this installation
 * is set up (path field, luna-ui's file dialog for Browse, drag-and-drop).
 * 0 with the application in `path` and `settings` as edited; -1 when
 * cancelled or there is no display to ask on. */
int luna_launcher_pick(char *path, size_t capacity,
                       luna_launcher_setting *settings, size_t count);
void luna_launcher_end(int keep_window);
void *luna_launcher_take_window(void);

#ifdef __cplusplus
}
#endif

/* The running app's profile: what exists, which is in use, a fresh name, and
 * starting over under another (returns only when it could not). */
const char *luna_apk_profile_current(void);
int luna_apk_profile_list(char (*names)[65], int max);
int luna_apk_profile_new_name(char *out, size_t cap);
int luna_apk_restart(const char *profile);
