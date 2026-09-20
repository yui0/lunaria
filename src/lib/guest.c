/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Guest-side platform code: compiled for AArch64 Android and loaded into the
 * emulated process as liblunaria_guest.so (`make guestlib`).
 *
 * Everything here is the part of Android's libc that a device runs as plain
 * code inside the process and that the emulator used to answer with a trap.
 * A trap costs a JIT exit, a dispatcher round trip and the host handler; the
 * function itself is a few dozen instructions.  Called tens of thousands of
 * times a second, the exits were the work.
 *
 * Rules for this file:
 *   - plain C with no system calls on any fast path;
 *   - state that the emulator also reads or writes lives in guest memory, and
 *     the code that touches it is in the shared part below;
 *   - the emulator binds the exports in preference to its own handlers and
 *     publishes the shared state into the two variables at load.
 *
 * The file has two parts.  The first is the guest process's malloc heap,
 * which the emulator's own handlers must also use on the same bytes: the
 * emulator compiles just that part by defining LUNARIA_GUEST_SHARED before
 * including this file (src/arm_exec.cpp).  The second part, the exported
 * libc entry points, is guest-only.
 *
 * ---- heap ---------------------------------------------------------------
 *
 * On a device malloc is code inside the process: a call is a few dozen
 * instructions, never a trap.  Answering it with an SVC made every
 * allocation leave the JIT, and during a UE load that is tens of thousands of
 * exits a second.  So the allocator runs in the guest (this file, built as
 * liblunaria_guest.so), while the emulator's own handlers that must
 * hand the guest memory (NewStringUTF, strdup, …) use the very same functions
 * on the very same bytes.  There is one heap, not two that must never meet.
 *
 * Everything the allocator owns lives in guest memory: the control block
 * below at the start of the heap window, the boundary tags in front of every
 * block and the free-list links inside free blocks.  Addresses are 32-bit
 * offsets into the 4 GiB image window; `lh_heap.mem` says where that window
 * starts in the address space of whoever is running the code (the host's
 * reservation, or the guest's own view of it — the same number under the A64
 * identity mapping).
 *
 * Mutual exclusion is bionic's own mutex protocol on a word in the control
 * block: 0 unlocked, 1 locked, 2 locked with waiters.  A guest waiter sleeps
 * in FUTEX_WAIT on that word and the unlocker wakes one with FUTEX_WAKE, the
 * same system calls a device's libc makes; the emulator's handlers take the
 * same word by compare-and-swap and wake guest waiters through the emulator's
 * futex table.  The holder is recorded beside it so a waiter can ask the
 * scheduler to run the holder.
 */
#ifndef LUNARIA_LIB_GUEST_C
#define LUNARIA_LIB_GUEST_C

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>


#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LH_MAGIC       0x4c484541u   /* "LHEA" */
#define LH_ALIGN       16u
#define LH_HDR         LH_ALIGN
#define LH_MIN         LH_ALIGN
#define LH_ALLOC_MAGIC 0xA110C8EDu
#define LH_FREE_MAGIC  0xF2EEB10Cu
#define LH_SMALL_MAX   4096u                        /* exact bins up to here */
#define LH_SMALL_BINS  (LH_SMALL_MAX / LH_ALIGN + 1u)
#define LH_NBINS       (LH_SMALL_BINS + 32u)
#define LH_MAP_WORDS   ((LH_NBINS + 63u) / 64u)

/* Values for `owner`: 0 is free, LH_OWNER_HOST an emulator handler, and
 * anything else a guest thread's TPIDR_EL0 (unique per thread, readable
 * without a trap). */
#define LH_OWNER_HOST  1u

struct lh_ctl {
   uint32_t magic;
   uint32_t size;                 /* sizeof(struct lh_ctl), checked by both */
   uint32_t state;                /* 0 unlocked, 1 locked, 2 contended */
   uint32_t pad0;
   uint64_t owner;                /* who holds it, for diagnostics and boost */
   uint32_t start;                /* first block header */
   uint32_t ptr;                  /* bump pointer (next header) */
   uint32_t high_water;           /* highest bump ever: above it is zero */
   uint32_t limit;                /* the small-mmap arena comes down to here */
   uint64_t free_blocks;
   uint64_t malloc_calls, malloc_reused, free_calls;
   uint64_t contended;            /* lock waits, for the perf report */
   uint64_t bin_map[LH_MAP_WORDS];
   uint32_t bin[LH_NBINS];
};

struct lh_heap {
   uintptr_t mem;                 /* where offset 0 of the window is */
   struct lh_ctl *ctl;
};

static inline uint32_t lh_r32(const struct lh_heap *h, uint32_t off)
{
   uint32_t v;
   memcpy(&v, (const void *)(h->mem + off), 4);
   return v;
}

static inline void lh_w32(const struct lh_heap *h, uint32_t off, uint32_t v)
{
   memcpy((void *)(h->mem + off), &v, 4);
}

static inline uint32_t lh_align(uint32_t n)
{
   return (n + LH_ALIGN - 1u) & ~(LH_ALIGN - 1u);
}

/* A guest size_t is 64 bits and the window is 32; refuse rather than
 * truncate into a much smaller successful allocation. */
static inline bool lh_size32(uint64_t n, uint32_t *out)
{
   if (n > (uint64_t)UINT32_MAX - (LH_ALIGN - 1u) - LH_HDR) return false;
   *out = (uint32_t)n;
   return true;
}

static inline uint32_t lh_bin_of(uint32_t size)
{
   if (size <= LH_SMALL_MAX) return size / LH_ALIGN;
   uint32_t k = 31u - (uint32_t)__builtin_clz(size);
   uint32_t b = LH_SMALL_BINS + (k - 12u);
   return b < LH_NBINS ? b : LH_NBINS - 1u;
}

static inline void lh_bin_mark(struct lh_ctl *c, uint32_t b, bool used)
{
   if (used) c->bin_map[b / 64u] |= 1ull << (b % 64u);
   else      c->bin_map[b / 64u] &= ~(1ull << (b % 64u));
}

static inline uint32_t lh_bin_next(const struct lh_ctl *c, uint32_t b)
{
   for (uint32_t w = b / 64u; w < LH_MAP_WORDS; ++w) {
      uint64_t bits = c->bin_map[w];
      if (w == b / 64u) bits &= ~0ull << (b % 64u);
      if (bits) return w * 64u + (uint32_t)__builtin_ctzll(bits);
   }
   return LH_NBINS;
}

/* First use of a window: an empty heap starting right after the control
 * block.  `limit` is the top of the window (the small-mmap arena starts
 * there and grows down). */
static inline void lh_init(struct lh_heap *h, uint32_t ctl_off, uint32_t limit)
{
   struct lh_ctl *c = h->ctl;
   memset(c, 0, sizeof *c);
   c->magic = LH_MAGIC;
   c->size = sizeof *c;
   c->start = lh_align(ctl_off + (uint32_t)sizeof *c);
   c->ptr = c->start;
   c->high_water = c->start;
   c->limit = limit;
}

static inline bool lh_free_block_ok(const struct lh_heap *h, uint32_t va)
{
   const struct lh_ctl *c = h->ctl;
   if (va < c->start + LH_HDR || va >= c->ptr ||
       lh_r32(h, va - 4u) != LH_FREE_MAGIC)
      return false;
   const uint32_t size = lh_r32(h, va - LH_HDR);
   return size >= LH_MIN && !(size & (LH_ALIGN - 1u)) &&
          (uint64_t)va + size <= c->ptr &&
          lh_r32(h, va + size - 4u) == size;
}

/* Free-list links are {next,prev} in the first eight payload bytes. */
static inline bool lh_bin_unlink(struct lh_heap *h, uint32_t va, uint32_t size)
{
   struct lh_ctl *c = h->ctl;
   const uint32_t b = lh_bin_of(size);
   const uint32_t next = lh_r32(h, va);
   const uint32_t prev = lh_r32(h, va + 4u);
   if (prev) {
      if (lh_r32(h, prev) != va) return false;
      lh_w32(h, prev, next);
   } else {
      if (c->bin[b] != va) return false;
      c->bin[b] = next;
   }
   if (next) lh_w32(h, next + 4u, prev);
   if (!c->bin[b]) lh_bin_mark(c, b, false);
   if (c->free_blocks) --c->free_blocks;
   return true;
}

static inline uint32_t lh_bin_take(struct lh_heap *h, uint32_t b, uint32_t size)
{
   struct lh_ctl *c = h->ctl;
   uint32_t prev = 0;
   uint32_t va = c->bin[b];
   const int limit = (b < LH_SMALL_BINS) ? 1 : 32;
   for (int steps = 0; va && steps < limit; ++steps) {
      if (!lh_free_block_ok(h, va)) {
         /* Truncate rather than trust a chain through overwritten memory. */
         if (prev) lh_w32(h, prev, 0u);
         else { c->bin[b] = 0u; lh_bin_mark(c, b, false); }
         return 0;
      }
      const uint32_t blk = lh_r32(h, va - LH_HDR);
      const uint32_t next = lh_r32(h, va);
      if (blk >= size) return lh_bin_unlink(h, va, blk) ? va : 0u;
      prev = va;
      va = next;
   }
   return 0;
}

static inline void lh_bin_put(struct lh_heap *h, uint32_t va, uint32_t size)
{
   struct lh_ctl *c = h->ctl;
   const uint32_t b = lh_bin_of(size);
   const uint32_t old = c->bin[b];
   lh_w32(h, va, old);
   lh_w32(h, va + 4u, 0u);
   if (old) lh_w32(h, old + 4u, va);
   c->bin[b] = va;
   lh_bin_mark(c, b, true);
   ++c->free_blocks;
}

/* Header word +4 is the size of the physical left neighbour while that
 * neighbour is free, otherwise zero. */
static inline void lh_set_next_prev_free(struct lh_heap *h, uint32_t va,
                                         uint32_t size, uint32_t prev_size)
{
   const uint32_t next_hdr = va + size;
   if (next_hdr < h->ctl->ptr) lh_w32(h, next_hdr + 4u, prev_size);
}

static inline void lh_mark_allocated(struct lh_heap *h, uint32_t va, uint32_t size)
{
   lh_w32(h, va - LH_HDR, size);
   lh_w32(h, va - 4u, LH_ALLOC_MAGIC);
   lh_set_next_prev_free(h, va, size, 0u);
}

static inline void lh_mark_free(struct lh_heap *h, uint32_t va, uint32_t size)
{
   lh_w32(h, va - LH_HDR, size);
   lh_w32(h, va - 4u, LH_FREE_MAGIC);
   lh_w32(h, va + size - 4u, size);
   lh_set_next_prev_free(h, va, size, size);
}

static inline void lh_release(struct lh_heap *h, uint32_t va, uint32_t size)
{
   struct lh_ctl *c = h->ctl;
   uint32_t hdr = va - LH_HDR;

   if (hdr > c->start) {
      const uint32_t left_size = lh_r32(h, hdr + 4u);
      if (left_size >= LH_MIN && !(left_size & (LH_ALIGN - 1u)) &&
          left_size <= hdr - (c->start + LH_HDR)) {
         const uint32_t left = hdr - left_size;
         if (lh_free_block_ok(h, left) && lh_bin_unlink(h, left, left_size)) {
            va = left;
            size += LH_HDR + left_size;
            hdr = va - LH_HDR;
         }
      }
   }

   const uint32_t right_hdr = va + size;
   if ((uint64_t)right_hdr + LH_HDR < c->ptr) {
      const uint32_t right = right_hdr + LH_HDR;
      if (lh_free_block_ok(h, right)) {
         const uint32_t right_size = lh_r32(h, right - LH_HDR);
         if (lh_bin_unlink(h, right, right_size))
            size += LH_HDR + right_size;
      }
   }

   lh_mark_free(h, va, size);
   if ((uint64_t)va + size == c->ptr) c->ptr = hdr;
   else lh_bin_put(h, va, size);
}

/* `zeroed` reports memory that has never been handed out before. */
static inline uint32_t lh_malloc(struct lh_heap *h, uint32_t size, bool *zeroed)
{
   struct lh_ctl *c = h->ctl;
   if (zeroed) *zeroed = false;
   if (size == 0) size = 1;
   size = lh_align(size);
   ++c->malloc_calls;

   for (uint32_t b = lh_bin_next(c, lh_bin_of(size)); b < LH_NBINS;
        b = lh_bin_next(c, b + 1u)) {
      uint32_t va = lh_bin_take(h, b, size);
      if (!va) continue;
      const uint32_t blk = lh_r32(h, va - LH_HDR);
      ++c->malloc_reused;
      if (blk >= size + LH_HDR + LH_MIN) {
         uint32_t rem_hdr = va + size;
         uint32_t rem_payload = rem_hdr + LH_HDR;
         uint32_t rem_size = blk - size - LH_HDR;
         lh_w32(h, rem_hdr + 4u, 0u);
         lh_mark_free(h, rem_payload, rem_size);
         lh_bin_put(h, rem_payload, rem_size);
         lh_mark_allocated(h, va, size);
      } else {
         lh_mark_allocated(h, va, blk);
      }
      return va;
   }

   if ((uint64_t)c->ptr + LH_HDR + size > c->limit) return 0;   /* OOM */
   uint32_t hdr = c->ptr;
   uint32_t va = hdr + LH_HDR;
   lh_w32(h, hdr + 4u, 0u);
   lh_w32(h, hdr, size);
   lh_w32(h, hdr + LH_HDR - 4u, LH_ALLOC_MAGIC);
   c->ptr = va + size;
   const bool never_used = va >= c->high_water;
   if (c->ptr > c->high_water) c->high_water = c->ptr;
   if (zeroed) *zeroed = never_used;
   return va;
}

static inline bool lh_owns(const struct lh_heap *h, uint32_t va)
{
   const struct lh_ctl *c = h->ctl;
   return va >= c->start + LH_HDR && va < c->ptr &&
          lh_r32(h, va - 4u) == LH_ALLOC_MAGIC;
}

static inline void lh_free(struct lh_heap *h, uint32_t va)
{
   struct lh_ctl *c = h->ctl;
   if (!va || !lh_owns(h, va)) return;              /* foreign / double free */
   const uint32_t size = lh_r32(h, va - LH_HDR);
   if (size < LH_MIN || (size & (LH_ALIGN - 1u)) ||
       (uint64_t)va + size > c->ptr) return;        /* corrupt */
   ++c->free_calls;
   lh_release(h, va, size);
}

static inline void lh_trim(struct lh_heap *h, uint32_t va, uint32_t blk, uint32_t want)
{
   if (blk < want + LH_HDR + LH_MIN) return;
   const uint32_t rem_hdr = va + want;
   const uint32_t rem_size = blk - want - LH_HDR;
   lh_w32(h, rem_hdr + 4u, 0u);
   lh_mark_allocated(h, va, want);
   lh_release(h, rem_hdr + LH_HDR, rem_size);
}

/* 0 with *foreign set: a pointer this heap never returned. */
static inline uint32_t lh_realloc(struct lh_heap *h, uint32_t va, uint32_t newsize,
                                  bool *foreign)
{
   struct lh_ctl *c = h->ctl;
   if (foreign) *foreign = false;
   if (!va) return lh_malloc(h, newsize, NULL);
   if (newsize == 0) { lh_free(h, va); return 0; }
   if (!lh_owns(h, va)) { if (foreign) *foreign = true; return 0; }
   const uint32_t blk = lh_r32(h, va - LH_HDR);
   const uint32_t want = lh_align(newsize);
   if (want <= blk) {
      lh_trim(h, va, blk, want);
      return va;
   }
   if ((uint64_t)va + blk == c->ptr && (uint64_t)va + want <= c->limit) {
      lh_w32(h, va - LH_HDR, want);
      c->ptr = va + want;
      if (c->ptr > c->high_water) c->high_water = c->ptr;
      return va;
   }
   const uint32_t right_hdr = va + blk;
   if ((uint64_t)right_hdr + LH_HDR < c->ptr) {
      const uint32_t right = right_hdr + LH_HDR;
      if (lh_free_block_ok(h, right)) {
         const uint32_t right_size = lh_r32(h, right - LH_HDR);
         const uint32_t combined = blk + LH_HDR + right_size;
         if (combined >= want && lh_bin_unlink(h, right, right_size)) {
            lh_mark_allocated(h, va, combined);
            lh_trim(h, va, combined, want);
            return va;
         }
      }
   }
   uint32_t n = lh_malloc(h, want, NULL);
   if (n) {
      memmove((void *)(h->mem + n), (const void *)(h->mem + va), blk);
      lh_free(h, va);
   }
   return n;
}

static inline uint32_t lh_memalign(struct lh_heap *h, uint32_t align, uint32_t size)
{
   if (align <= LH_HDR || (align & (align - 1u))) return lh_malloc(h, size, NULL);
   if (size == 0) size = 1;
   size = lh_align(size);
   if ((uint64_t)size + align + LH_HDR + LH_HDR + LH_MIN > UINT32_MAX) return 0;
   const uint32_t raw = lh_malloc(h, size + align + LH_HDR + LH_MIN, NULL);
   if (!raw) return 0;
   const uint32_t blk = lh_r32(h, raw - LH_HDR);
   if (!(raw & (align - 1u))) {
      lh_trim(h, raw, blk, size);
      return raw;
   }
   const uint32_t payload =
      (raw + LH_HDR + LH_MIN + align - 1u) & ~(align - 1u);
   const uint32_t prefix = payload - LH_HDR - raw;
   lh_w32(h, payload - LH_HDR, blk - prefix - LH_HDR);
   lh_w32(h, payload - LH_HDR + 4u, 0u);
   lh_w32(h, payload - 4u, LH_ALLOC_MAGIC);
   lh_release(h, raw, prefix);
   lh_trim(h, payload, lh_r32(h, payload - LH_HDR), size);
   return payload;
}

static inline uint32_t lh_usable_size(const struct lh_heap *h, uint32_t va)
{
   return lh_owns(h, va) ? lh_r32(h, va - LH_HDR) : 0u;
}

/* Carve `len` off the top of the window for the small-mmap arena, never
 * below what malloc has bumped to.  Returns the new top, or 0. */
static inline uint32_t lh_take_top(struct lh_heap *h, uint32_t len, uint32_t align_mask)
{
   struct lh_ctl *c = h->ctl;
   if (len > c->limit) return 0;
   const uint32_t next = (c->limit - len) & ~align_mask;
   if (next < c->ptr || next >= c->limit) return 0;
   c->limit = next;
   return next;
}

#ifndef LUNARIA_GUEST_SHARED
/* ---- guest-only: the libc entry points --------------------------------- */

/* Filled in by the emulator when it maps this library (arm64 loader). */
__attribute__((visibility("default"))) struct lh_ctl *lunaria_heap_ctl = 0;
__attribute__((visibility("default"))) uintptr_t lunaria_heap_mem = 0;

extern int *__errno(void);

#define ENOMEM 12
#define EINVAL 22

static inline uint64_t self_owner(void)
{
   return (uint64_t)(uintptr_t)__builtin_thread_pointer();
}

/* futex(addr, op, val, NULL): the kernel's own entry, as bionic calls it. */
long lunaria_raw_futex(uint32_t *addr, long op, uint32_t val);
__asm__(".text\n"
        ".p2align 2\n"
        ".type lunaria_raw_futex, %function\n"
        "lunaria_raw_futex:\n"
        "  mov x3, #0\n"
        "  mov x8, #98\n"
        "  svc #0\n"
        "  ret\n");
#define raw_futex lunaria_raw_futex

#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

/* bionic's normal mutex: fast path one compare-and-swap, a waiter sleeps in
 * the kernel until the holder's unlock wakes it. */
static inline void heap_lock(struct lh_ctl *c)
{
   uint32_t expected = 0;
   if (!__builtin_expect(__atomic_compare_exchange_n(&c->state, &expected, 1u,
                                                     0, 5, 5), 1)) {
      ++c->contended;              /* racy statistic, never read for control */
      while (__atomic_exchange_n(&c->state, 2u, 5) != 0u)
         raw_futex(&c->state, FUTEX_WAIT_PRIVATE, 2u);
   }
   c->owner = self_owner();
}

static inline void heap_unlock(struct lh_ctl *c)
{
   c->owner = 0;
   if (__atomic_exchange_n(&c->state, 0u, 5) == 2u)
      raw_futex(&c->state, FUTEX_WAKE_PRIVATE, 1u);
}

static inline int heap_get(struct lh_heap *h)
{
   h->ctl = lunaria_heap_ctl;
   h->mem = lunaria_heap_mem;
   return h->ctl != 0;
}

static inline void *to_ptr(const struct lh_heap *h, uint32_t off)
{
   return off ? (void *)(h->mem + off) : (void *)0;
}

/* The window offset of a guest pointer, or 0 when it is not in the window. */
static inline uint32_t to_off(const struct lh_heap *h, void *p)
{
   const uintptr_t v = (uintptr_t)p;
   if (!p || v < h->mem || v - h->mem >= 0x100000000ull) return 0;
   return (uint32_t)(v - h->mem);
}

static void set_errno(int e)
{
   int *p = __errno();
   if (p) *p = e;
}

__attribute__((visibility("default")))
void *malloc(size_t n)
{
   struct lh_heap h;
   uint32_t size;
   if (!heap_get(&h) || !lh_size32(n, &size)) { set_errno(ENOMEM); return 0; }
   heap_lock(h.ctl);
   const uint32_t off = lh_malloc(&h, size, (bool *)0);
   heap_unlock(h.ctl);
   if (!off) set_errno(ENOMEM);
   return to_ptr(&h, off);
}

__attribute__((visibility("default")))
void free(void *p)
{
   struct lh_heap h;
   if (!p || !heap_get(&h)) return;
   const uint32_t off = to_off(&h, p);
   if (!off) return;
   heap_lock(h.ctl);
   lh_free(&h, off);
   heap_unlock(h.ctl);
}

__attribute__((visibility("default")))
void *calloc(size_t count, size_t n)
{
   struct lh_heap h;
   uint32_t size;
   if (n && count > (size_t)-1 / n) { set_errno(ENOMEM); return 0; }
   if (!heap_get(&h) || !lh_size32((uint64_t)count * n, &size)) {
      set_errno(ENOMEM);
      return 0;
   }
   bool zeroed = false;
   heap_lock(h.ctl);
   const uint32_t off = lh_malloc(&h, size, &zeroed);
   heap_unlock(h.ctl);
   if (!off) { set_errno(ENOMEM); return 0; }
   if (!zeroed) memset(to_ptr(&h, off), 0, size);
   return to_ptr(&h, off);
}

__attribute__((visibility("default")))
void *realloc(void *p, size_t n)
{
   struct lh_heap h;
   uint32_t size;
   if (!heap_get(&h)) return 0;
   if (!p) return malloc(n);
   if (!lh_size32(n, &size)) { set_errno(ENOMEM); return 0; }
   const uint32_t off = to_off(&h, p);
   if (!off) return 0;
   bool foreign = false;
   heap_lock(h.ctl);
   const uint32_t r = lh_realloc(&h, off, size, &foreign);
   heap_unlock(h.ctl);
   if (!r && n) set_errno(ENOMEM);
   return to_ptr(&h, r);
}

__attribute__((visibility("default")))
void *memalign(size_t align, size_t n)
{
   struct lh_heap h;
   uint32_t a, size;
   if (!heap_get(&h) || !lh_size32(align, &a) || !lh_size32(n, &size)) {
      set_errno(ENOMEM);
      return 0;
   }
   /* bionic rounds a non-power-of-two alignment up to one. */
   if (a & (a - 1u)) {
      uint32_t p2 = 1;
      while (p2 < a && p2) p2 <<= 1;
      a = p2;
   }
   heap_lock(h.ctl);
   const uint32_t off = lh_memalign(&h, a, size);
   heap_unlock(h.ctl);
   if (!off) set_errno(ENOMEM);
   return to_ptr(&h, off);
}

__attribute__((visibility("default")))
int posix_memalign(void **out, size_t align, size_t n)
{
   if (!out || !align || (align & (align - 1u)) || align % sizeof(void *))
      return EINVAL;
   void *p = memalign(align, n);
   if (!p) return ENOMEM;
   *out = p;
   return 0;
}

__attribute__((visibility("default")))
void *aligned_alloc(size_t align, size_t n)
{
   if (!align || (align & (align - 1u))) { set_errno(EINVAL); return 0; }
   return memalign(align, n);
}

__attribute__((visibility("default")))
void *valloc(size_t n)
{
   return memalign(4096u, n);
}

__attribute__((visibility("default")))
void *pvalloc(size_t n)
{
   return memalign(4096u, (n + 4095u) & ~(size_t)4095u);
}

__attribute__((visibility("default")))
size_t malloc_usable_size(const void *p)
{
   struct lh_heap h;
   if (!p || !heap_get(&h)) return 0;
   const uint32_t off = to_off(&h, (void *)(uintptr_t)p);
   if (!off) return 0;
   heap_lock(h.ctl);
   const uint32_t n = lh_usable_size(&h, off);
   heap_unlock(h.ctl);
   return n;
}


/* ---- condition variables ------------------------------------------------
 *
 * bionic's own algorithm, run as guest code for the same reason malloc above
 * is: on a device pthread_cond_signal is a handful of instructions and at most
 * one futex(2); answering it with a trap made every notify leave the JIT.
 *
 * The object is the guest's memory and the protocol is Android's:
 *
 *   +0  state    flags in the low two bits, a wake generation above them
 *   +4  waiters  how many threads are inside a wait
 *
 * The generation counter is what makes the wait race-free without a waiter
 * list.  A waiter reads the state *before* it unlocks the mutex and asks the
 * kernel to sleep only while the word still holds that value; a signal bumps
 * the counter before it wakes anyone.  A signal that lands in the window
 * between the unlock and the sleep therefore cannot be lost — the sleep is
 * refused with EAGAIN because the word has already moved.  Nothing has to
 * remember the event, and no wake can be delivered to a thread that was not
 * waiting for it.
 *
 * `waiters` is the one addition to bionic's layout, in space its
 * pthread_cond_t reserves and nothing else uses.  bionic issues the wake
 * syscall unconditionally; here a syscall is a JIT exit, so a signal with no
 * waiter is answered without one.  The generation is still bumped first, so
 * a waiter that raced the count away from zero sees the state move and does
 * not sleep.
 *
 * The mutex is taken and dropped through pthread_mutex_lock/unlock, which are
 * the guest's own — the wait must leave and re-enter the lock by exactly the
 * protocol the rest of the program uses, and POSIX requires the call to
 * return holding it, timeout or not.
 */
#include <errno.h>
#include <pthread.h>
#include <time.h>

#define COND_SHARED_MASK   0x0001u
#define COND_CLOCK_MASK    0x0002u   /* set: CLOCK_MONOTONIC */
#define COND_COUNTER_STEP  0x0004u
#define COND_FLAGS_MASK    (COND_SHARED_MASK | COND_CLOCK_MASK)

#define FUTEX_WAIT_BITSET_PRIVATE  (9 | 128)
#define FUTEX_CLOCK_REALTIME       256
#define FUTEX_BITSET_MATCH_ANY     0xffffffffu
#define COND_DESTROYED             0xdeadc04du

struct lunaria_cond {
   uint32_t state;
   uint32_t waiters;
};

/* futex(addr, op, val, timeout, uaddr2, val3) — the six-argument form the
 * timed waits need.  The three-argument raw_futex above cannot express an
 * absolute deadline. */
long lunaria_raw_futex6(volatile uint32_t *addr, long op, uint32_t val,
                        const struct timespec *timeout, void *uaddr2,
                        uint32_t val3);
__asm__(".text\n"
        ".p2align 2\n"
        ".type lunaria_raw_futex6, %function\n"
        "lunaria_raw_futex6:\n"
        "  mov x8, #98\n"
        "  svc #0\n"
        "  ret\n");

static inline struct lunaria_cond *cond_of(pthread_cond_t *c)
{
   return (struct lunaria_cond *)(void *)c;
}

/* One wake generation.  The flags live in the low two bits and the step is 4,
 * so the add never disturbs them. */
static inline void cond_bump(struct lunaria_cond *c)
{
   __atomic_fetch_add(&c->state, COND_COUNTER_STEP, __ATOMIC_SEQ_CST);
}

static int cond_wake(struct lunaria_cond *c, uint32_t n)
{
   /* Order matters: the generation moves before the count is read.  A waiter
    * that has already sampled the state but has not yet counted itself in
    * therefore finds the word changed and refuses to sleep. */
   cond_bump(c);
   if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST) != 0u)
      raw_futex(&c->state, FUTEX_WAKE_PRIVATE, n);
   return 0;
}

static int cond_wait_common(pthread_cond_t *cv, pthread_mutex_t *m,
                            const struct timespec *abstime, int clock)
{
   struct lunaria_cond *c = cond_of(cv);
   if (abstime && (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L))
      return EINVAL;
   /* Sampled under the mutex, exactly as the predicate the caller just tested
    * was: everything the wait is racing against has to happen after this. */
   const uint32_t seq = __atomic_load_n(&c->state, __ATOMIC_RELAXED);
   __atomic_fetch_add(&c->waiters, 1u, __ATOMIC_SEQ_CST);
   pthread_mutex_unlock(m);
   const long op = FUTEX_WAIT_BITSET_PRIVATE |
                   (clock == CLOCK_REALTIME ? FUTEX_CLOCK_REALTIME : 0);
   const long rc = lunaria_raw_futex6(&c->state, op, seq, abstime, 0,
                                      FUTEX_BITSET_MATCH_ANY);
   __atomic_fetch_sub(&c->waiters, 1u, __ATOMIC_SEQ_CST);
   /* POSIX: the call returns with the mutex held, however it ended. */
   pthread_mutex_lock(m);
   return rc == -ETIMEDOUT ? ETIMEDOUT : 0;
}

__attribute__((visibility("default")))
int pthread_condattr_init(pthread_condattr_t *attr)
{
   if (!attr) return EINVAL;
   *(uint32_t *)(void *)attr = 0u;
   return 0;
}

__attribute__((visibility("default")))
int pthread_condattr_destroy(pthread_condattr_t *attr)
{
   if (!attr) return EINVAL;
   *(uint32_t *)(void *)attr = COND_DESTROYED;
   return 0;
}

__attribute__((visibility("default")))
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock)
{
   if (!attr) return EINVAL;
   if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return EINVAL;
   uint32_t *p = (uint32_t *)(void *)attr;
   *p = (*p & ~COND_CLOCK_MASK) |
        (clock == CLOCK_MONOTONIC ? COND_CLOCK_MASK : 0u);
   return 0;
}

__attribute__((visibility("default")))
int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock)
{
   if (!attr || !clock) return EINVAL;
   const uint32_t v = *(const uint32_t *)(const void *)attr;
   *clock = (v & COND_CLOCK_MASK) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
   return 0;
}

__attribute__((visibility("default")))
int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
   if (!attr) return EINVAL;
   uint32_t *p = (uint32_t *)(void *)attr;
   *p = (*p & ~COND_SHARED_MASK) | (pshared ? COND_SHARED_MASK : 0u);
   return 0;
}

__attribute__((visibility("default")))
int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared)
{
   if (!attr || !pshared) return EINVAL;
   *pshared = (*(const uint32_t *)(const void *)attr & COND_SHARED_MASK) ? 1 : 0;
   return 0;
}

__attribute__((visibility("default")))
int pthread_cond_init(pthread_cond_t *cv, const pthread_condattr_t *attr)
{
   if (!cv) return EINVAL;
   struct lunaria_cond *c = cond_of(cv);
   const uint32_t flags =
       attr ? (*(const uint32_t *)(const void *)attr & COND_FLAGS_MASK) : 0u;
   __atomic_store_n(&c->waiters, 0u, __ATOMIC_RELAXED);
   __atomic_store_n(&c->state, flags, __ATOMIC_SEQ_CST);
   return 0;
}

__attribute__((visibility("default")))
int pthread_cond_destroy(pthread_cond_t *cv)
{
   if (!cv) return EINVAL;
   __atomic_store_n(&cond_of(cv)->state, COND_DESTROYED, __ATOMIC_SEQ_CST);
   return 0;
}

__attribute__((visibility("default")))
int pthread_cond_signal(pthread_cond_t *cv)
{
   if (!cv) return EINVAL;
   return cond_wake(cond_of(cv), 1u);
}

__attribute__((visibility("default")))
int pthread_cond_broadcast(pthread_cond_t *cv)
{
   if (!cv) return EINVAL;
   return cond_wake(cond_of(cv), 0x7fffffffu);
}

__attribute__((visibility("default")))
int pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *m)
{
   if (!cv || !m) return EINVAL;
   return cond_wait_common(cv, m, 0, CLOCK_MONOTONIC);
}

__attribute__((visibility("default")))
int pthread_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *m,
                           const struct timespec *abstime)
{
   if (!cv || !m) return EINVAL;
   const uint32_t state = __atomic_load_n(&cond_of(cv)->state, __ATOMIC_RELAXED);
   const int clock = (state & COND_CLOCK_MASK) ? CLOCK_MONOTONIC
                                               : CLOCK_REALTIME;
   return cond_wait_common(cv, m, abstime, clock);
}

/* bionic's own extensions.  A guest built against the NDK calls these
 * directly, and UE's FPThreadEvent is one of the callers. */
__attribute__((visibility("default")))
int pthread_cond_clockwait(pthread_cond_t *cv, pthread_mutex_t *m,
                           clockid_t clock, const struct timespec *abstime)
{
   if (!cv || !m) return EINVAL;
   if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return EINVAL;
   return cond_wait_common(cv, m, abstime, clock);
}

__attribute__((visibility("default")))
int pthread_cond_timedwait_monotonic_np(pthread_cond_t *cv, pthread_mutex_t *m,
                                        const struct timespec *abstime)
{
   if (!cv || !m) return EINVAL;
   return cond_wait_common(cv, m, abstime, CLOCK_MONOTONIC);
}

__attribute__((visibility("default")))
int pthread_cond_timedwait_relative_np(pthread_cond_t *cv, pthread_mutex_t *m,
                                       const struct timespec *reltime)
{
   if (!cv || !m) return EINVAL;
   if (!reltime) return cond_wait_common(cv, m, 0, CLOCK_MONOTONIC);
   struct timespec now, abs;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return EINVAL;
   abs.tv_sec = now.tv_sec + reltime->tv_sec;
   abs.tv_nsec = now.tv_nsec + reltime->tv_nsec;
   if (abs.tv_nsec >= 1000000000L) { abs.tv_nsec -= 1000000000L; ++abs.tv_sec; }
   return cond_wait_common(cv, m, &abs, CLOCK_MONOTONIC);
}


/* ---- _FORTIFY_SOURCE ----------------------------------------------------
 *
 * Android builds application code with _FORTIFY_SOURCE, so the calls in the
 * binary are not `strlen`/`memcpy` but `__strlen_chk`/`__memcpy_chk`: the
 * plain arguments plus the destination size the compiler knows, and an abort
 * when the operation would not fit.  They are ordinary in-process code on a
 * device -- the check is one comparison in front of the same routine.
 *
 * As emulator traps they were the single most expensive thing this process
 * did.  On Cross Worlds' post-title load `__strlen_chk` alone was 96% of
 * every SVC, 1.4 million JIT exits every ten seconds -- one every 3,224 guest
 * instructions -- and the SVC round trip came to a third of all run time.
 * The check is a comparison; the exit was the work.
 *
 * So the check lives here, in front of the plain function the emulator
 * already provides as guest code, and only a *failing* check leaves the
 * guest -- through lunaria_fortify_fatal(), which prints and aborts exactly
 * as bionic's __fortify_fatal() does.  The sizes compared are the same ones
 * the emulator's own FORTIFY block compared before.
 *
 * strlen is spelled out rather than imported: it is the hot one, and a local
 * definition cannot be resolved to anything but this code.  The others call
 * the plain entry point by name, as bionic's do. */

/* memcpy/memmove/memset come from <string.h> and bind to the emulator's own
 * guest-side stubs.  This file is built with _FORTIFY_SOURCE off, so a call
 * to one of them here is a call to the plain function and not a recursion
 * back into the wrapper below it. */

/* Reports the violation and ends the process.  Never returns. */
extern void lunaria_fortify_fatal(const char *what, size_t want, size_t have);

/* Word-at-a-time, aligning down first: an 8-aligned load cannot cross into a
 * page the string does not already reach, so the bytes before the start can
 * simply be masked off. */
static size_t lun_strlen(const char *s)
{
   const char *p = s;
   while (((uintptr_t)p & 7u) != 0u) {
      if (*p == '\0') return (size_t)(p - s);
      ++p;
   }
   for (;;) {
      uint64_t v;
      __builtin_memcpy(&v, p, 8);
      const uint64_t z = (v - 0x0101010101010101ull) & ~v &
                         0x8080808080808080ull;
      if (z) return (size_t)(p - s) + (size_t)(__builtin_ctzll(z) >> 3);
      p += 8;
   }
}

__attribute__((visibility("default")))
size_t __strlen_chk(const char *s, size_t s_len)
{
   const size_t n = lun_strlen(s);
   /* The string has to *end* inside the object, so the terminator counts. */
   if (__builtin_expect(n + 1u > s_len, 0))
      lunaria_fortify_fatal("strlen", n + 1u, s_len);
   return n;
}

__attribute__((visibility("default")))
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len)
{
   if (__builtin_expect(n > dst_len, 0))
      lunaria_fortify_fatal("memcpy", n, dst_len);
   return memcpy(dst, src, n);
}

__attribute__((visibility("default")))
void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len)
{
   if (__builtin_expect(n > dst_len, 0))
      lunaria_fortify_fatal("memmove", n, dst_len);
   return memmove(dst, src, n);
}

__attribute__((visibility("default")))
void *__memset_chk(void *dst, int c, size_t n, size_t dst_len)
{
   if (__builtin_expect(n > dst_len, 0))
      lunaria_fortify_fatal("memset", n, dst_len);
   return memset(dst, c, n);
}

__attribute__((visibility("default")))
char *__strcpy_chk(char *dst, const char *src, size_t dst_len)
{
   const size_t n = lun_strlen(src) + 1u;
   if (__builtin_expect(n > dst_len, 0))
      lunaria_fortify_fatal("strcpy", n, dst_len);
   memcpy(dst, src, n);
   return dst;
}

__attribute__((visibility("default")))
char *__stpcpy_chk(char *dst, const char *src, size_t dst_len)
{
   const size_t n = lun_strlen(src) + 1u;
   if (__builtin_expect(n > dst_len, 0))
      lunaria_fortify_fatal("stpcpy", n, dst_len);
   memcpy(dst, src, n);
   return dst + n - 1u;
}

__attribute__((visibility("default")))
char *__strcat_chk(char *dst, const char *src, size_t dst_len)
{
   const size_t have = lun_strlen(dst);
   const size_t n = have + lun_strlen(src) + 1u;
   if (__builtin_expect(n > dst_len, 0))
      lunaria_fortify_fatal("strcat", n, dst_len);
   memcpy(dst + have, src, n - have);
   return dst;
}

/* strncpy pads the destination to `n` bytes, so the operation is `n` bytes
 * wide whatever the source's length is. */
__attribute__((visibility("default")))
char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len)
{
   if (__builtin_expect(n > dst_len, 0))
      lunaria_fortify_fatal("strncpy", n, dst_len);
   size_t i = 0;
   for (; i < n && src[i] != '\0'; ++i) dst[i] = src[i];
   for (; i < n; ++i) dst[i] = '\0';
   return dst;
}

__attribute__((visibility("default")))
char *__strncat_chk(char *dst, const char *src, size_t n, size_t dst_len)
{
   const size_t have = lun_strlen(dst);
   size_t add = 0;
   while (add < n && src[add] != '\0') ++add;
   if (__builtin_expect(have + add + 1u > dst_len, 0))
      lunaria_fortify_fatal("strncat", have + add + 1u, dst_len);
   memcpy(dst + have, src, add);
   dst[have + add] = '\0';
   return dst;
}

#endif /* !LUNARIA_GUEST_SHARED */
#endif /* LUNARIA_LIB_GUEST_C */
