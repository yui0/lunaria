/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Process-wide A64 JIT compile counters exported by dynarmic.  Always
 * updated; the progress hook is what drives the luna-ui status card while a
 * cold start sits in the emitter.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t dynarmic_a64_compile_count(void);
uint64_t dynarmic_a64_compile_ns(void);

/* Called (throttled) after a block is compiled.  NULL clears.  May run on any
 * host thread that owns a JIT — the callback must be re-entrant and cheap, or
 * do its own locking. */
void dynarmic_a64_set_progress_hook(void (*fn)(uint64_t compiles, uint64_t compile_ns));

#ifdef __cplusplus
}
#endif
