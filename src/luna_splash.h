/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The emulator's boot screen.
 *
 * Between the launcher's last message and the game's first frame there is a
 * long stretch — for Blade & Soul Revolution about half a minute — during
 * which the surface holds nothing at all: ELF relocation, 1610 static
 * constructors, 29 MB of dex across four files, then the engine's own RHI and
 * shader setup.  On a device this gap is covered by the launcher icon and then
 * by the app's splash Activity.  Lunaria showed a black rectangle, which is
 * indistinguishable from a hang, and the one thing a person watching wants to
 * know — is it still making progress, and on what — was only in the log.
 *
 * This is that screen: a moon drawn and animated entirely in CSS through
 * luna-ui (the same engine luna_overlay composites dialogs with), with the
 * dex loader's progress underneath it.  It is the emulator's own UI, not the
 * guest's, so it comes down the moment the guest takes the surface.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Puts the boot screen up.  Called once the host EGL surface exists, which is
 * the first moment anything can be drawn at all. */
void luna_splash_begin(void);

/* What the emulator is doing now, in the words a person watching would use.
 * Replaces the previous stage line. */
void luna_splash_stage(const char *fmt, ...)
#ifdef __GNUC__
   __attribute__((format(printf, 1, 2)))
#endif
   ;

/* Dex loading, which is the one phase with a countable total: the loader knows
 * how many classes*.dex the package has before it opens the first. */
void luna_splash_dex_total(int files, uint64_t bytes);
void luna_splash_dex_loaded(const char *path, uint32_t classes,
                            uint32_t methods, uint64_t bytes);

/* Fraction of the whole boot that is done, 0..1, for phases with no count of
 * their own (ELF ctors, engine init).  Never moves backwards, so the phases
 * have to claim their ranges in the order they run; luna_splash.c holds the
 * budget they are divided into. */
void luna_splash_progress(float fraction);
#define LUNA_SPLASH_LINK_FLOOR 0.02f
#define LUNA_SPLASH_LINK_CEIL  0.30f
#define LUNA_SPLASH_JNI_MARK   0.32f
#define LUNA_SPLASH_WAIT_MARK  0.90f

/* Takes the boot screen down.  `why` is logged. */
void luna_splash_end(const char *why);

/* True while the boot screen should still be drawn. */
bool luna_splash_active(void);

/* The presenter draws a frame: it makes the host surface current, asks the
 * overlay to render, and swaps.  arm_exec owns the EGL objects, so it
 * registers the function here and the splash calls it from wherever progress
 * happens — including from inside the dex loader, which runs with no guest
 * instructions executing and so would otherwise freeze the animation. */
typedef void (*luna_splash_present_fn)(void);
void luna_splash_set_presenter(luna_splash_present_fn fn);

/* Asks for a frame, at most once per frame interval.  Cheap to call often. */
void luna_splash_pump(void);

#ifdef __cplusplus
}
#endif
