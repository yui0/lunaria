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

#ifdef __cplusplus
}
#endif
