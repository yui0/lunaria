/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Android-linker symbol scope for AArch64 guest images.  See linker64.h for
 * why the flat "one name -> one address" map this replaces is not equivalent.
 *
 * Everything here is read out of the *mapped* image through the dynamic
 * segment, exactly as the linker does: DT_SYMTAB / DT_STRTAB / DT_HASH /
 * DT_GNU_HASH / DT_VERSYM / DT_VERDEF / DT_VERNEED.  Section headers are not
 * consulted at all -- a stripped library has none, and a library that has them
 * is not obliged to agree with its own dynamic segment.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "linker64.h"

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

#define LN64_VERSYM_HIDDEN 0x8000u
#define LN64_VER_LOCAL     0u
#define LN64_VER_GLOBAL    1u

struct ln64_module {
   char       *path;
   const char *soname;         /* into strtab, or path's basename */

   uint32_t    bias;           /* backing offset the image was mapped at */
   bool        is_global;      /* member of the global group */
   bool        is_main;        /* executable/main object: global-scope head */
   bool        symbolic;       /* DF_SYMBOLIC / DF_1_SYMBOLIC */

   const Elf64_Sym *symtab;
   const char      *strtab;
   uint64_t         strsz;

   /* DT_HASH */
   const uint32_t *bucket, *chain;
   uint32_t        nbucket, nchain;

   /* DT_GNU_HASH */
   const uint32_t *gnu_bucket, *gnu_chain;
   const uint64_t *gnu_bloom;
   uint32_t        gnu_nbucket, gnu_symoffset, gnu_bloom_size, gnu_bloom_shift;

   /* versioning */
   const uint16_t *versym;
   const void     *verdef;
   const void     *verneed;
   uint32_t        verdefnum, verneednum;

   /* DT_NEEDED, as strtab offsets, resolved to modules by ln64_link_deps() */
   uint64_t   *needed_off;
   size_t      needed_n;
   struct ln64_module **deps;
   size_t      deps_n;
};

static ln64_ptr_fn  g_ptr;
static void        *g_ptr_user;

static struct ln64_module **g_mods;
static size_t               g_mods_n, g_mods_cap;

/* Scratch used to walk a local group breadth-first without allocating on the
 * lookup path.  A lookup is not re-entrant and not called from two threads at
 * once (relocation happens on the loading thread), so one buffer is enough. */
static struct ln64_module **g_bfs;
static size_t               g_bfs_cap;

void ln64_init(ln64_ptr_fn ptr, void *user)
{
   g_ptr = ptr;
   g_ptr_user = user;
}

static void *host(uint32_t off, size_t len)
{
   if (!g_ptr || !off) return NULL;
   return g_ptr(off, len, g_ptr_user);
}

/* ---------------------------------------------------------------- hashing */

static uint32_t sysv_hash(const char *name)
{
   const unsigned char *p = (const unsigned char *)name;
   uint32_t h = 0, g;
   while (*p) {
      h = (h << 4) + *p++;
      g = h & 0xf0000000u;
      h ^= g >> 24;
      h &= ~g;
   }
   return h;
}

static uint32_t gnu_hash(const char *name)
{
   const unsigned char *p = (const unsigned char *)name;
   uint32_t h = 5381;
   while (*p) h += (h << 5) + *p++;   /* h * 33 + c */
   return h;
}

/* ------------------------------------------------------------ versioning */

/* The version name a definition at `idx` carries in `m`, or NULL when the
 * image is unversioned.  `*hidden` says whether the definition is a
 * non-default (hidden) version, which only an explicit request may bind to. */
static const char *def_version(const struct ln64_module *m, uint32_t idx,
                               bool *hidden)
{
   *hidden = false;
   if (!m->versym) return NULL;
   uint16_t v = m->versym[idx];
   *hidden = (v & LN64_VERSYM_HIDDEN) != 0;
   v &= (uint16_t)~LN64_VERSYM_HIDDEN;
   if (v <= LN64_VER_GLOBAL) return NULL;   /* local or unversioned-global */
   if (!m->verdef) return NULL;

   const uint8_t *p = (const uint8_t *)m->verdef;
   for (uint32_t i = 0; i < m->verdefnum; ++i) {
      const Elf64_Verdef *vd = (const Elf64_Verdef *)p;
      if (vd->vd_version == 0) break;
      if (vd->vd_ndx == v && vd->vd_cnt > 0) {
         const Elf64_Verdaux *aux =
            (const Elf64_Verdaux *)(p + vd->vd_aux);
         return m->strtab + aux->vda_name;
      }
      if (vd->vd_next == 0) break;
      p += vd->vd_next;
   }
   return NULL;
}

/* The version a *reference* at `idx` in `m` asks for, or NULL for none. */
static const char *ref_version(const struct ln64_module *m, uint32_t idx)
{
   if (!m->versym || idx == LN64_NO_VERSION) return NULL;
   uint16_t v = m->versym[idx] & (uint16_t)~LN64_VERSYM_HIDDEN;
   if (v <= LN64_VER_GLOBAL) return NULL;

   /* An undefined reference names its version through DT_VERNEED. */
   if (m->verneed) {
      const uint8_t *p = (const uint8_t *)m->verneed;
      for (uint32_t i = 0; i < m->verneednum; ++i) {
         const Elf64_Verneed *vn = (const Elf64_Verneed *)p;
         if (vn->vn_version == 0) break;
         const uint8_t *ap = p + vn->vn_aux;
         for (uint32_t j = 0; j < vn->vn_cnt; ++j) {
            const Elf64_Vernaux *va = (const Elf64_Vernaux *)ap;
            if ((va->vna_other & (uint16_t)~LN64_VERSYM_HIDDEN) == v)
               return m->strtab + va->vna_name;
            if (va->vna_next == 0) break;
            ap += va->vna_next;
         }
         if (vn->vn_next == 0) break;
         p += vn->vn_next;
      }
   }
   /* A defined symbol carries its own version through DT_VERDEF; asking for
    * it (a self-reference under -Bsymbolic) is legitimate. */
   bool hidden;
   return def_version(m, idx, &hidden);
}

/* bionic's check_symbol_version, in the terms above: an unversioned request
 * takes any default (non-hidden) definition; a versioned request takes only
 * the matching version, and an unversioned definition satisfies it -- that is
 * what lets a versioned library link against one that has no versions. */
static bool version_ok(const struct ln64_module *m, uint32_t idx,
                       const char *want)
{
   bool hidden;
   const char *have = def_version(m, idx, &hidden);
   if (!want) return !hidden;
   if (!have) return true;
   return strcmp(have, want) == 0;
}

/* ----------------------------------------------------------- per-image find */

static bool sym_usable(const Elf64_Sym *s)
{
   const unsigned char bind = ELF64_ST_BIND(s->st_info);
   /* A definition is only a definition if it has a section.  STB_LOCAL
    * symbols are not exported, however they got into .dynsym -- the old flat
    * map took them, which let one image's private helper answer another
    * image's import. */
   return s->st_shndx != SHN_UNDEF &&
          (bind == STB_GLOBAL || bind == STB_WEAK);
}

static const Elf64_Sym *find_gnu(const struct ln64_module *m, const char *name,
                                 uint32_t h, const char *want, uint32_t *idx_out)
{
   const uint32_t word = (h / 64u) % m->gnu_bloom_size;
   const uint32_t b1 = h % 64u;
   const uint32_t b2 = (h >> m->gnu_bloom_shift) % 64u;
   const uint64_t bits = m->gnu_bloom[word];
   if (!((bits >> b1) & (bits >> b2) & 1u)) return NULL;

   uint32_t n = m->gnu_bucket[h % m->gnu_nbucket];
   if (n == 0) return NULL;
   const uint32_t *chain = m->gnu_chain;
   for (;;) {
      const uint32_t c = chain[n - m->gnu_symoffset];
      if ((c | 1u) == (h | 1u)) {
         const Elf64_Sym *s = &m->symtab[n];
         if (strcmp(m->strtab + s->st_name, name) == 0 &&
             sym_usable(s) && version_ok(m, n, want)) {
            if (idx_out) *idx_out = n;
            return s;
         }
      }
      if (c & 1u) break;   /* end of chain */
      ++n;
   }
   return NULL;
}

static const Elf64_Sym *find_sysv(const struct ln64_module *m, const char *name,
                                  uint32_t h, const char *want, uint32_t *idx_out)
{
   for (uint32_t n = m->bucket[h % m->nbucket]; n != 0 && n < m->nchain;
        n = m->chain[n]) {
      const Elf64_Sym *s = &m->symtab[n];
      if (strcmp(m->strtab + s->st_name, name) != 0) continue;
      if (!sym_usable(s) || !version_ok(m, n, want)) continue;
      if (idx_out) *idx_out = n;
      return s;
   }
   return NULL;
}

static const Elf64_Sym *mod_find(const struct ln64_module *m, const char *name,
                                 uint32_t gh, uint32_t sh, const char *want,
                                 uint32_t *idx_out)
{
   if (!m->symtab || !m->strtab) return NULL;
   /* Prefer DT_GNU_HASH where both exist: it is the table the linker that
    * produced the image intended to be used, and the only one a modern
    * platform library has. */
   if (m->gnu_bucket && m->gnu_nbucket)
      return find_gnu(m, name, gh, want, idx_out);
   if (m->bucket && m->nbucket)
      return find_sysv(m, name, sh, want, idx_out);
   return NULL;
}

/* --------------------------------------------------------------- registry */

static bool mods_push(struct ln64_module *m)
{
   if (g_mods_n == g_mods_cap) {
      size_t cap = g_mods_cap ? g_mods_cap * 2 : 16;
      struct ln64_module **v = realloc(g_mods, cap * sizeof *v);
      if (!v) return false;
      g_mods = v;
      g_mods_cap = cap;
   }
   g_mods[g_mods_n++] = m;
   return true;
}

static const char *basename_of(const char *path)
{
   const char *s = strrchr(path, '/');
   return s ? s + 1 : path;
}

struct ln64_module *ln64_add(const char *path, uint32_t load_bias,
                             uint32_t dynamic_off, bool is_global,
                             bool is_main)
{
   if (!path || !dynamic_off) return NULL;

   /* The dynamic array is NUL-terminated by a DT_NULL entry; ask for a
    * generous window and stop at DT_NULL. */
   const Elf64_Dyn *dyn = host(dynamic_off, sizeof(Elf64_Dyn));
   if (!dyn) return NULL;

   struct ln64_module *m = calloc(1, sizeof *m);
   if (!m) return NULL;
   m->path = strdup(path);
   m->bias = load_bias;
   m->is_global = is_global;
   m->is_main = is_main;

   uint64_t soname_off = 0;
   uint32_t symtab_off = 0, strtab_off = 0, hash_off = 0, gnu_off = 0;
   uint32_t versym_off = 0, verdef_off = 0, verneed_off = 0;
   uint64_t flags = 0;
   size_t   needed_cap = 0;

   for (const Elf64_Dyn *d = dyn; ; ++d) {
      const Elf64_Dyn *e = host(dynamic_off + (uint32_t)((const uint8_t *)d -
                                                         (const uint8_t *)dyn),
                                sizeof *e);
      if (!e) break;
      if (e->d_tag == DT_NULL) break;
      switch (e->d_tag) {
      case DT_SYMTAB:   symtab_off  = (uint32_t)(load_bias + e->d_un.d_ptr); break;
      case DT_STRTAB:   strtab_off  = (uint32_t)(load_bias + e->d_un.d_ptr); break;
      case DT_STRSZ:    m->strsz    = e->d_un.d_val; break;
      case DT_HASH:     hash_off    = (uint32_t)(load_bias + e->d_un.d_ptr); break;
      case DT_GNU_HASH: gnu_off     = (uint32_t)(load_bias + e->d_un.d_ptr); break;
      case DT_VERSYM:   versym_off  = (uint32_t)(load_bias + e->d_un.d_ptr); break;
      case DT_VERDEF:   verdef_off  = (uint32_t)(load_bias + e->d_un.d_ptr); break;
      case DT_VERDEFNUM:  m->verdefnum  = (uint32_t)e->d_un.d_val; break;
      case DT_VERNEED:  verneed_off = (uint32_t)(load_bias + e->d_un.d_ptr); break;
      case DT_VERNEEDNUM: m->verneednum = (uint32_t)e->d_un.d_val; break;
      case DT_SONAME:   soname_off  = e->d_un.d_val; break;
      case DT_FLAGS:    flags       = e->d_un.d_val; break;
      case DT_SYMBOLIC: flags |= DF_SYMBOLIC; break;
      case DT_NEEDED:
         if (m->needed_n == needed_cap) {
            size_t cap = needed_cap ? needed_cap * 2 : 8;
            uint64_t *v = realloc(m->needed_off, cap * sizeof *v);
            if (!v) break;
            m->needed_off = v;
            needed_cap = cap;
         }
         m->needed_off[m->needed_n++] = e->d_un.d_val;
         break;
      default: break;
      }
   }

   m->symbolic = (flags & DF_SYMBOLIC) != 0;
   m->symtab = host(symtab_off, sizeof(Elf64_Sym));
   m->strtab = host(strtab_off, 1);
   m->soname = (soname_off && m->strtab) ? m->strtab + soname_off
                                         : basename_of(m->path);

   if (hash_off) {
      const uint32_t *h = host(hash_off, 8);
      if (h) {
         m->nbucket = h[0];
         m->nchain  = h[1];
         m->bucket  = host(hash_off + 8u, (size_t)m->nbucket * 4u);
         m->chain   = host(hash_off + 8u + m->nbucket * 4u,
                           (size_t)m->nchain * 4u);
      }
   }
   if (gnu_off) {
      const uint32_t *h = host(gnu_off, 16);
      if (h) {
         m->gnu_nbucket     = h[0];
         m->gnu_symoffset   = h[1];
         m->gnu_bloom_size  = h[2];
         m->gnu_bloom_shift = h[3];
         if (m->gnu_nbucket && m->gnu_bloom_size) {
            m->gnu_bloom  = host(gnu_off + 16u,
                                 (size_t)m->gnu_bloom_size * 8u);
            uint32_t bo = gnu_off + 16u + m->gnu_bloom_size * 8u;
            m->gnu_bucket = host(bo, (size_t)m->gnu_nbucket * 4u);
            m->gnu_chain  = host(bo + m->gnu_nbucket * 4u, 4u);
         }
      }
      if (!m->gnu_bloom || !m->gnu_bucket || !m->gnu_chain) {
         m->gnu_bucket = NULL;
         m->gnu_nbucket = 0;
      }
   }
   if (versym_off) m->versym = host(versym_off, 2);
   if (verdef_off) m->verdef = host(verdef_off, sizeof(Elf64_Verdef));
   if (verneed_off) m->verneed = host(verneed_off, sizeof(Elf64_Verneed));
   /* A version table without the definitions it indexes says nothing. */
   if (!m->verdef) m->verdefnum = 0;
   if (!m->verneed) m->verneednum = 0;

   if (!m->symtab || !m->strtab || (!m->bucket && !m->gnu_bucket)) {
      /* Nothing to look symbols up in.  Say so once: an image the emulator
       * cannot index is one whose imports will silently miss. */
      fprintf(stderr, "[ln64] %s: no usable symbol hash "
              "(symtab=%d strtab=%d sysv=%d gnu=%d)\n",
              m->path, m->symtab != NULL, m->strtab != NULL,
              m->bucket != NULL, m->gnu_bucket != NULL);
   }

   if (!mods_push(m)) {
      free(m->needed_off);
      free(m->path);
      free(m);
      return NULL;
   }
   return m;
}

void ln64_link_deps(void)
{
   for (size_t i = 0; i < g_mods_n; ++i) {
      struct ln64_module *m = g_mods[i];
      if (!m->needed_n || !m->strtab) continue;
      free(m->deps);
      m->deps = NULL;
      m->deps = calloc(m->needed_n, sizeof *m->deps);
      if (!m->deps) continue;
      m->deps_n = 0;
      for (size_t k = 0; k < m->needed_n; ++k) {
         const char *want = m->strtab + m->needed_off[k];
         for (size_t j = 0; j < g_mods_n; ++j) {
            if (strcmp(g_mods[j]->soname, want) == 0 ||
                strcmp(basename_of(g_mods[j]->path), want) == 0) {
               m->deps[m->deps_n++] = g_mods[j];
               break;
            }
         }
      }
      /* An unresolved DT_NEEDED is not an error here: the emulator answers
       * the Android platform libraries itself, so they are never mapped. */
      if (m->deps_n == 0) { free(m->deps); m->deps = NULL; }
   }
}

/* ----------------------------------------------------------------- lookup */

static bool take(const struct ln64_module *m, const Elf64_Sym *s,
                 const char *name, struct ln64_sym *out)
{
   out->value = (uint32_t)(m->bias + s->st_value);
   out->size  = (uint32_t)s->st_size;
   out->info  = s->st_info;
   out->other = s->st_other;
   out->name  = name;
   out->owner = (struct ln64_module *)m;
   return true;
}

static bool bfs_reserve(size_t n)
{
   if (g_bfs_cap >= n) return true;
   size_t cap = g_bfs_cap ? g_bfs_cap : 16;
   while (cap < n) cap *= 2;
   struct ln64_module **v = realloc(g_bfs, cap * sizeof *v);
   if (!v) return false;
   g_bfs = v;
   g_bfs_cap = cap;
   return true;
}

bool ln64_lookup(struct ln64_module *ref, const char *name,
                 uint32_t ref_sym_idx, struct ln64_sym *out)
{
   if (!name || !*name || !out) return false;
   const uint32_t gh = gnu_hash(name), sh = sysv_hash(name);
   const char *want = ref ? ref_version(ref, ref_sym_idx) : NULL;
   uint32_t idx;

   /* 1. -Bsymbolic: the object asked to bind its own definitions locally. */
   if (ref && ref->symbolic) {
      const Elf64_Sym *s = mod_find(ref, name, gh, sh, want, &idx);
      if (s) return take(ref, s, name, out);
   }

   /* 2. The global group.  Android places the executable/main object at its
    * head even though its dependencies necessarily had to be mapped first. */
   for (int main_pass = 1; main_pass >= 0; --main_pass)
      for (size_t i = 0; i < g_mods_n; ++i) {
         if (!g_mods[i]->is_global || g_mods[i]->is_main != (main_pass != 0))
            continue;
         const Elf64_Sym *s = mod_find(g_mods[i], name, gh, sh, want, &idx);
         if (s) return take(g_mods[i], s, name, out);
      }

   /* 3. The referring object's local group, breadth-first from itself. */
   if (ref && bfs_reserve(g_mods_n + 1)) {
      size_t head = 0, tail = 0;
      g_bfs[tail++] = ref;
      while (head < tail) {
         struct ln64_module *m = g_bfs[head++];
         if (!m->is_global) {   /* globals were searched above */
            const Elf64_Sym *s = mod_find(m, name, gh, sh, want, &idx);
            if (s) return take(m, s, name, out);
         }
         for (size_t k = 0; k < m->deps_n; ++k) {
            struct ln64_module *d = m->deps[k];
            bool seen = false;
            for (size_t q = 0; q < tail; ++q)
               if (g_bfs[q] == d) { seen = true; break; }
            if (!seen && tail < g_mods_n + 1) g_bfs[tail++] = d;
         }
      }
   }
   return false;
}

bool ln64_lookup_in(struct ln64_module *mod, const char *name,
                    struct ln64_sym *out)
{
   if (!mod || !name || !out) return false;
   const uint32_t gh = gnu_hash(name), sh = sysv_hash(name);
   if (!bfs_reserve(g_mods_n + 1)) return false;
   size_t head = 0, tail = 0;
   g_bfs[tail++] = mod;
   while (head < tail) {
      struct ln64_module *m = g_bfs[head++];
      uint32_t idx;
      const Elf64_Sym *s = mod_find(m, name, gh, sh, NULL, &idx);
      if (s) return take(m, s, name, out);
      for (size_t k = 0; k < m->deps_n; ++k) {
         struct ln64_module *d = m->deps[k];
         bool seen = false;
         for (size_t q = 0; q < tail; ++q)
            if (g_bfs[q] == d) { seen = true; break; }
         if (!seen && tail < g_mods_n + 1) g_bfs[tail++] = d;
      }
   }
   return false;
}

bool ln64_lookup_global(const char *name, struct ln64_sym *out)
{
   if (!name || !out) return false;
   const uint32_t gh = gnu_hash(name), sh = sysv_hash(name);
   uint32_t idx;
   for (int pass = 0; pass < 3; ++pass)
      for (size_t i = 0; i < g_mods_n; ++i) {
         const bool wanted = pass < 2;
         if (g_mods[i]->is_global != wanted) continue;
         if (wanted && g_mods[i]->is_main != (pass == 0)) continue;
         const Elf64_Sym *s = mod_find(g_mods[i], name, gh, sh, NULL, &idx);
         if (s) return take(g_mods[i], s, name, out);
      }
   return false;
}

const char *ln64_module_path(const struct ln64_module *m)
{
   return m ? m->path : NULL;
}
const char *ln64_module_soname(const struct ln64_module *m)
{
   return m ? m->soname : NULL;
}
uint32_t ln64_module_bias(const struct ln64_module *m)
{
   return m ? m->bias : 0u;
}

struct ln64_module *ln64_module_by_path(const char *path)
{
   if (!path) return NULL;
   for (size_t i = 0; i < g_mods_n; ++i)
      if (strcmp(g_mods[i]->path, path) == 0) return g_mods[i];
   return NULL;
}

void ln64_dump(void)
{
   for (size_t i = 0; i < g_mods_n; ++i) {
      const struct ln64_module *m = g_mods[i];
      fprintf(stderr, "[ln64] %-28s base=0x%08x %s hash=%s%s%s ver=%s "
              "needed=%zu\n",
              m->soname, m->bias, m->is_main ? "main  " :
              (m->is_global ? "global" : "local "),
              m->gnu_bucket ? "gnu" : "", (m->gnu_bucket && m->bucket) ? "+" : "",
              m->bucket ? "sysv" : "",
              m->versym ? "yes" : "no", m->needed_n);
   }
}
