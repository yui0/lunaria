/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef LUNARIA_LINKER64_H
#define LUNARIA_LINKER64_H

/* Symbol scope for AArch64 guest images, as the Android dynamic linker
 * defines it.
 *
 * The emulator used to answer every relocation from one flat name -> address
 * map, with "the main library wins, otherwise whoever loaded first".  That is
 * not what a device does, and the difference is not academic:
 *
 *   - A definition is only visible to an object that has it *in scope*.  A
 *     flat map makes every definition in the process visible to everybody, so
 *     two libraries that each carry their own copy of a helper silently share
 *     one -- and which one they share depends on load order.
 *   - Interposition has a direction.  The global group is searched before the
 *     local group, so the executable and its dependencies interpose the
 *     libraries loaded later; a library's own definition does *not* win merely
 *     because it is its own, unless it was linked -Bsymbolic (DF_SYMBOLIC).
 *   - Symbol versioning decides which definition answers a versioned
 *     reference.  Without it, a real bionic libc.so (whose symbols are all
 *     versioned, and which carries several versions of the same name) cannot
 *     be linked against at all.
 *   - A modern platform library may ship DT_GNU_HASH only.  Looking symbols up
 *     through DT_HASH alone finds nothing in it.
 *
 * This module owns that: the set of loaded images, the scope each one sees,
 * and the lookup itself.  It does not map memory or apply relocations -- the
 * caller does that and asks here for the address a name resolves to.
 *
 * C11, no C++ in the interface: the loader is being moved to C.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ln64_module;

/* Guest memory access.  `off` is the 32-bit backing offset the emulator uses
 * for an AArch64 guest address (guest VA == 0x700000000000 | off); the call
 * returns a host pointer to at least `len` readable bytes, or NULL.  Every
 * table this module walks lives inside a mapped image, exactly as it does on
 * a device -- nothing is read out of the on-disk file. */
typedef void *(*ln64_ptr_fn)(uint32_t off, size_t len, void *user);

void ln64_init(ln64_ptr_fn ptr, void *user);

/* Register an image the loader has just mapped.  `dynamic_off` is the backing
 * offset of its PT_DYNAMIC contents (load_bias + p_vaddr).  `is_global` puts
 * the image in the global group -- true for the executable and everything
 * reachable from it, as the linker does for the main namespace.
 *
 * Returns NULL only on allocation failure or a dynamic section it cannot
 * parse; the caller can then fall back to its previous behaviour. */
struct ln64_module *ln64_add(const char *path, uint32_t load_bias,
                             uint32_t dynamic_off, bool is_global,
                             bool is_main);

/* Resolve the DT_NEEDED names of every registered image against the images
 * registered so far, building the dependency graph the local groups are
 * walked over.  Cheap and idempotent; call it after each load. */
void ln64_link_deps(void);

/* The result of a lookup. */
struct ln64_sym {
   uint32_t            value;    /* backing offset of the definition */
   uint32_t            size;
   uint8_t             info;     /* ELF64_ST_INFO */
   uint8_t             other;
   const char         *name;
   struct ln64_module *owner;
};

/* Resolve `name` as `ref` sees it: DF_SYMBOLIC first if set, then the global
 * group in load order, then `ref`'s local group breadth-first.  `ref_sym_idx`
 * is the index of the *reference* in `ref`'s own .dynsym, which is what
 * carries its version requirement; pass LN64_NO_VERSION when there is none to
 * express (a dlsym() by name, say).
 *
 * Returns true and fills `*out` when a global or weak definition is found. */
#define LN64_NO_VERSION 0xffffffffu
bool ln64_lookup(struct ln64_module *ref, const char *name,
                 uint32_t ref_sym_idx, struct ln64_sym *out);

/* Lookup restricted to one image and its dependencies, which is what
 * dlsym(handle, name) does. */
bool ln64_lookup_in(struct ln64_module *mod, const char *name,
                    struct ln64_sym *out);

/* Whole-process lookup with no reference object: the global group in load
 * order, then every other image in load order.  This is RTLD_DEFAULT. */
bool ln64_lookup_global(const char *name, struct ln64_sym *out);

const char *ln64_module_path(const struct ln64_module *m);
const char *ln64_module_soname(const struct ln64_module *m);
uint32_t    ln64_module_bias(const struct ln64_module *m);
struct ln64_module *ln64_module_by_path(const char *path);

/* Diagnostics: one line per image with what was found in it. */
void ln64_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* LUNARIA_LINKER64_H */
