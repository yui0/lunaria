/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef ARM_H
#define ARM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ARM execution lock.
 *
 * Guest code itself needs no lock: memory is identity-mapped and each guest
 * thread has its own registers, so two of them can run in the JIT at the same
 * time on two host cores.  What cannot run at the same time is the emulator
 * around them — the SVC layer keeps its socket table, thread table, file
 * table, GL state and several hundred other file-scope variables without any
 * synchronisation, because until now exactly one host thread ever touched
 * them.
 *
 * So the rule is: hold this across everything that leaves guest code, and only
 * that.  It is the same shape as the bytecode VM's interpreter lock (see
 * dvm_gil_acquire), including the part that matters most — a handler that is
 * about to block gives the lock up first, or one thread waiting on a socket
 * would stop every other guest thread.
 *
 * Recursive: a handler that releases the lock around a blocking call may be
 * nested inside another that already holds it.
 */
void arm_lock_acquire(void);
void arm_lock_release(void);

/* True when this host thread holds the lock. */
bool arm_lock_held(void);

/* Drop the lock for the duration of a blocking host call.  The return value is
 * the recursion count, which the matching relock restores exactly.  Between
 * the two, this thread must not touch any emulator state. */
unsigned arm_lock_unlock_all(void);
void     arm_lock_relock(unsigned depth);

/* Hand the lock to a thread that is waiting for it, if there is one, and take
 * it back.  For a handler that runs long without blocking. */
void arm_lock_yield(void);

/* How many host threads are executing guest code right now, lock or no lock.
 * Diagnostics only. */
unsigned arm_lock_waiters(void);

/* Contention on the execution lock, for the [slice] report: wall time with an
 * owner, summed wait over all threads, and the worst single wait (which is
 * read-and-cleared). */
unsigned long long arm_lock_held_ns(void);
unsigned long long arm_lock_wait_ns(void);
unsigned long long arm_lock_max_wait_ns(void);
/* The longest single hold since the last call, and the label of whoever held
 * it (an SVC number, or one of the reserved ARM_LOCK_TAG_* ids). */
unsigned long long arm_lock_max_hold_ns(unsigned *tag_out);

/* Which caller the lock was held *for*.
 *
 * "held 100% of the last 5s" says the emulator is serialised but not by what,
 * and the two candidates — a long SVC handler and the scheduler pass — are
 * fixed in completely different places.  A hold is labelled by the thread that
 * takes it (0 = unlabelled) and the time is charged to that label when the
 * outermost release happens, so the report can name the handler rather than
 * the lock.  One thread-local store per acquire and one relaxed add per
 * release: cheap enough to leave on. */
/* SVC labels occupy 1..4096, the named holders 4097..4100, and a raw syscall
 * gets its own label above those: every `svc #0` shares SVC number 0, so
 * without this the report can only say "svc0 held the lock for 8.7 ms" and
 * never which syscall that was. */
#define ARM_LOCK_TAG_RAW(n) (4160u + ((unsigned)(n) & 511u))
#define ARM_LOCK_TAG_MAX 4672u
/* SVC numbers occupy 0..4095 (see SVC_TIME_MAX), so they are labelled at +1 and
 * 0 stays "unlabelled".  The named holders live above that range. */
#define ARM_LOCK_TAG_SVC(n) ((unsigned)(n) + 1u)
#define ARM_LOCK_TAG_SCHED  4097u   /* a scheduler pass */
#define ARM_LOCK_TAG_SLICE  4098u   /* an engine's slice prologue/epilogue */
#define ARM_LOCK_TAG_CLAIM  4099u   /* a worker looking for a thread to run */
#define ARM_LOCK_TAG_CB     4100u   /* a host-initiated guest callback */
/* Returns the label that was in force, so a nested holder can put it back. */
unsigned arm_lock_tag(unsigned tag);
/* Read-and-clear one label's charged time, for a windowed report. */
unsigned long long arm_lock_tag_take_ns(unsigned tag);

/* True once more than one A64 engine runs guest threads at the same time.
 *
 * The emulator outside guest code is written as a single thread that may be
 * re-entered, not as one that runs concurrently: an SVC handler waits by
 * spinning the scheduler, and the dvm hands its interpreter lock around inside
 * those waits.  Both hold while exactly one engine executes.  Once several do,
 * the same waits have to give the execution lock up instead of spinning while
 * holding it, and that is what this gates — the parallel path, not a fix for
 * anything the sequential one gets wrong.  Set by the ARM side at startup. */
bool arm_parallel_engines(void);
void arm_set_parallel_engines(bool on);

#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
/* ---------------------------------------------------------------------------
 * Shared with the execution paths: the guest virtual-address layout, the SVC
 * numbering, and the handful of helpers that hang off them.  None of it is
 * specific to ARM32 or ARM64 — both dispatch through the same SVC space — so
 * it lives here rather than being duplicated or reached across a translation
 * unit.  C++ because the guest-VA helpers use the standard containers the
 * emulator keeps its maps in; the C part of this header stays C.
 * ------------------------------------------------------------------------ */
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

/* Cached getenv — the literal's address is the cache key.
 *
 * Every engine calls this from inside SVC handlers, so it is on the hot path
 * of the whole emulator: an unlocked unordered_map raced its buckets (the host
 * allocator reported "corrupted size vs. prev_size"), and the shared_mutex
 * that replaced it turned every lookup into a contended atomic on one cache
 * line shared by four engines.
 *
 * A fixed open-addressed table needs neither.  The only mutation is publishing
 * a slot that was empty, the key is a pointer the caller already owns, and two
 * threads racing to insert the same literal both compute the same value from
 * getenv — so a released store of the value before the key, and an acquired
 * load of the key before the value, is the whole of the synchronisation.  It
 * is also plain C apart from the atomics. */
#define LUNARIA_ENV_CACHE_SLOTS 512u   /* power of two, ~40 distinct literals */
inline std::atomic<const void *> g_env_key[LUNARIA_ENV_CACHE_SLOTS];
inline std::atomic<const char *> g_env_val[LUNARIA_ENV_CACHE_SLOTS];
/* Bumped by lunaria_env_invalidate() so a cached miss cannot outlive our own
 * setenv().  Compared, not cleared: clearing would race the readers. */
inline std::atomic<unsigned> g_env_generation{1};
inline std::atomic<unsigned> g_env_slot_gen[LUNARIA_ENV_CACHE_SLOTS];
/* Scope guard for the ARM execution lock (src/arm.c).  The lock itself is
 * common C; this is only the C++ convenience for holding it across a block. */
namespace {
struct ArmLockGuard {
    ArmLockGuard()  { arm_lock_acquire(); }
    ~ArmLockGuard() { arm_lock_release(); }
    ArmLockGuard(const ArmLockGuard &) = delete;
    ArmLockGuard &operator=(const ArmLockGuard &) = delete;
};

/* Same, but only when the caller says the lock is needed.  The scheduler holds
 * the lock across its bookkeeping only when guest threads really do run in
 * parallel; with a single engine nothing else is executing and taking it would
 * change the order the emulator has always run in. */
struct ArmLockGuardIf {
    const bool on;
    explicit ArmLockGuardIf(bool want) : on(want) { if (on) arm_lock_acquire(); }
    ~ArmLockGuardIf() { if (on) arm_lock_release(); }
    ArmLockGuardIf(const ArmLockGuardIf &) = delete;
    ArmLockGuardIf &operator=(const ArmLockGuardIf &) = delete;
};

/* The inverse, for the one thing an SVC handler must never do while holding
 * the execution lock: block in a host call.
 *
 * A blocking read(2), poll(2) or connect(2) with the lock held stops every
 * other engine at its next SVC — including the guest thread that was going to
 * write to the pipe this one is reading.  That is a whole-emulator deadlock,
 * not a slow path, and it has to be spelled out at each such call rather than
 * left to the caller to remember. */
struct ArmLockDropped {
    const unsigned depth;
    ArmLockDropped() : depth(arm_lock_unlock_all()) {}
    ~ArmLockDropped() { arm_lock_relock(depth); }
    ArmLockDropped(const ArmLockDropped &) = delete;
    ArmLockDropped &operator=(const ArmLockDropped &) = delete;
};
}  // namespace

inline const char *lunaria_env(const char *lit) {
    const unsigned gen = g_env_generation.load(std::memory_order_relaxed);
    /* The key is an address, and the addresses of distinct literals differ in
     * their low bits far more than in their high ones. */
    unsigned h = (unsigned)(((uintptr_t)lit >> 4) * 2654435761u);
    for (unsigned probe = 0; probe < LUNARIA_ENV_CACHE_SLOTS; ++probe) {
        const unsigned i = (h + probe) & (LUNARIA_ENV_CACHE_SLOTS - 1u);
        const void *k = g_env_key[i].load(std::memory_order_acquire);
        if (k && k != (const void *)lit) continue;   /* another literal's slot */
        if (!k) {
            /* Claim the slot before filling it: filling first would let the
             * thread that lost the race overwrite the winner's value. */
            const void *expected = nullptr;
            if (!g_env_key[i].compare_exchange_strong(
                    expected, (const void *)lit,
                    std::memory_order_release, std::memory_order_acquire) &&
                expected != (const void *)lit)
                continue;                           /* somebody else took it */
        }
        /* The slot is ours (or already was).  A generation that does not match
         * means "not filled yet" and "filled before an invalidate" alike, and
         * both are answered the same way. */
        if (g_env_slot_gen[i].load(std::memory_order_acquire) == gen)
            return g_env_val[i].load(std::memory_order_relaxed);
        const char *v = getenv(lit);
        g_env_val[i].store(v, std::memory_order_relaxed);
        g_env_slot_gen[i].store(gen, std::memory_order_release);
        return v;
    }
    return getenv(lit);                             /* table full: still right */
}
// Call after any setenv() we perform ourselves.
inline void lunaria_env_invalidate(void) {
    g_env_generation.fetch_add(1u, std::memory_order_relaxed);
}

// Virtual address layout (32-bit guest VA; A64 relocates tramp/stack).
constexpr uint32_t HEAP_BASE     = 0x50000000u;
inline uint32_t    g_heap_size   = 0x10000000u; /* guest_layout_init */
// A64 fallback mmap arena: upper host-service heap, grows downward.
inline uint32_t    g_small_mmap_top = 0;
/* Carve the small-mmap fallback off the top of the malloc window (arm_exec). */
uint32_t arm_heap_take_top(uint32_t len, uint32_t align_mask);
// Mutable: A64 may relocate tramp/stack and grow primary to 1 GiB.
inline uint32_t THREAD_STACK_BASE = 0x48000000u;
constexpr uint32_t THREAD_STACK_SIZE = 0x00100000u;
constexpr uint32_t MMAP_BASE      = 0x10000000u;
inline uint32_t    MMAP_END       = 0x41000000u;
// Where AArch64 ELF images may live inside the image window.
inline uint32_t A64_IMAGE_BASE = 0x10000u;
inline uint32_t A64_IMAGE_END  = 0x04000000u;
// Unity Dynamic Heap asks for ~512 MiB slabs; success ⇒ exact length.
constexpr uint32_t MMAP_MAX_SINGLE= 0x20001000u; /* 512 MiB + 4K */
inline uint32_t g_mmap_max_single = MMAP_MAX_SINGLE;
constexpr uint32_t MMAP_ALIGN     = 0x10000u;
inline uint32_t g_mmap_next = MMAP_BASE;
inline uint32_t g_mmap2_base = 0x60000000u;
inline uint32_t g_mmap2_end  = 0; /* 0 → exclusive end at 4 GiB */
inline uint32_t g_mmap2_next = 0;
inline bool     g_mmap2_active = false;
using GuestVA = uint64_t;
using BackingOffset = uint32_t;
// A64 guest ptrs: 48-bit lower-half VA; backing store is still 4 GiB.
constexpr GuestVA A64_GUEST_BASE = 0x0000700000000000ull;
constexpr GuestVA A64_GUEST_SIZE = 0x0000000100000000ull;
/* The kernel's own two mappings at the top of the user address space.  Every
 * Linux process has them; a process whose /proc/<pid>/maps has no [vdso] was
 * not built by a kernel.  Nothing executes here -- the guest reaches the
 * clock through its libc, which this emulator answers -- so the addresses
 * only have to be where a 48-bit-VA arm64 kernel puts them. */
constexpr GuestVA A64_VVAR_BASE = 0x00007fffffff9000ull;
constexpr GuestVA A64_VDSO_BASE = 0x00007fffffffd000ull;
// A64: request image window at its guest VA before ArmMemory::init().
inline bool g_a64_identity_arena = false;
inline bool g_a64_arena_identity = false;

inline bool a64_is_guest_va(GuestVA va) {
    return va >= A64_GUEST_BASE && va < A64_GUEST_BASE + A64_GUEST_SIZE;
}
inline GuestVA a64_guest_va(BackingOffset backing) {
    return A64_GUEST_BASE + (GuestVA)backing;
}
/* Dynarmic's callbacks may report an image-window access as its 32-bit
 * backing offset even though the architectural A64 pointer uses the identity
 * arena VA.  Both spellings name the same guest page; VMA lookups must use
 * the architectural spelling stored in g_a64_maps. */
inline GuestVA a64_mapping_va(GuestVA va) {
    return va < A64_GUEST_SIZE ? a64_guest_va((BackingOffset)va) : va;
}
// A64: real 64-bit guest address space; outside image window, guest VA == host VA.
struct A64Mapping {
    GuestVA  lo = 0, hi = 0; /* guest VA range, page aligned; host VA == guest VA */
    uint32_t prot = 0;       /* last guest PROT_* (bookkeeping only) */
    bool     owned = false;  /* we host-mmap'd it and must munmap on release */
};
// Sorted by lo; disjoint.
inline std::vector<A64Mapping> g_a64_maps;
/* Every guest thread reads this table: the JIT's memory callbacks resolve a
 * high VA through it while guest code runs, which is deliberately outside the
 * ARM execution lock.  A std::vector cannot be walked while another thread
 * inserts or erases — the elements move under the reader.  Writers therefore
 * take this exclusively and readers take it shared; the table is written only
 * by guest mmap/munmap/mprotect, so the shared side is what has to be cheap. */
inline std::shared_mutex g_a64_maps_mu;
/* Bumped on every change so a thread that cached a range which has since been
 * unmapped stops answering "mapped" for it. */
inline std::atomic<uint64_t> g_a64_maps_gen{1};
inline void a64_maps_bump() { g_a64_maps_gen.fetch_add(1, std::memory_order_release); }
/* Bumped only when address space is taken *away*.  A cached answer "[lo,hi)
 * is mapped" stays true across an insert or a protection change (neither
 * unmaps a byte), so validating the cache against g_a64_maps_gen threw it
 * away on every mmap and mprotect -- and UE's allocator makes those all
 * through a load, which left every host-side access to a high address paying
 * the reader lock and a binary search. */
inline std::atomic<uint64_t> g_a64_unmap_gen{1};

/* Lookup cache, per thread: a shared global one is itself a race (two readers
 * write the pair non-atomically and a third can see a torn lo/hi that spans
 * an unmapped hole).  Several entries, because a copy has a source and a
 * destination and a single entry ping-pongs between them. */
struct A64RangeCache { GuestVA lo = 1, hi = 0; uint64_t gen = 0; };
inline thread_local A64RangeCache t_a64_cache[8];

// Caller holds g_a64_maps_mu (either way).
inline const A64Mapping *a64_find_locked(GuestVA va) {
    auto it = std::upper_bound(g_a64_maps.begin(), g_a64_maps.end(), va,
                               [](GuestVA v, const A64Mapping &m) { return v < m.lo; });
    if (it == g_a64_maps.begin()) return nullptr;
    --it;
    return (va >= it->lo && va < it->hi) ? &*it : nullptr;
}

inline bool a64_mapped(GuestVA va) {
    va = a64_mapping_va(va);
    const uint64_t gen = g_a64_unmap_gen.load(std::memory_order_acquire);
    A64RangeCache &c = t_a64_cache[(va >> 21) & 7u];
    if (gen == c.gen && va >= c.lo && va < c.hi)
        return true;
    for (const A64RangeCache &o : t_a64_cache)
        if (gen == o.gen && va >= o.lo && va < o.hi) {
            c = o;
            return true;
        }
    std::shared_lock<std::shared_mutex> lk(g_a64_maps_mu);
    const A64Mapping *m = a64_find_locked(va);
    if (!m) return false;
    c.lo  = m->lo;
    c.hi  = m->hi;
    c.gen = gen;
    return true;
}

/* Instruction fetch permission is distinct from address validity.  The old
 * high-VA path used a64_mapped() and consequently executed ordinary RW heap
 * pages.  Linux raises an instruction abort for those pages; feeding their
 * bytes to the decoder turns zero-filled data into an endless undefined-
 * instruction fallback instead. */
inline bool a64_executable(GuestVA va) {
    va = a64_mapping_va(va);
    std::shared_lock<std::shared_mutex> lk(g_a64_maps_mu);
    const A64Mapping *m = a64_find_locked(va);
    return m && (m->prot & 4u /* PROT_EXEC */) != 0;
}

// Bytes mapped contiguously from `va`, or 0 when `va` itself is unmapped.
inline size_t a64_mapped_span(GuestVA va) {
    va = a64_mapping_va(va);
    std::shared_lock<std::shared_mutex> lk(g_a64_maps_mu);
    const A64Mapping *m = a64_find_locked(va);
    return m ? (size_t)(m->hi - va) : 0u;
}

/* True when every byte in [va, va+len) has the requested guest permission.
 * The host reserves the whole A64 arena, so host pointer validity is not a
 * substitute for the guest's mmap table.  Walk adjacent entries as Linux
 * does for an access spanning a page/mapping boundary. */
inline bool a64_accessible(GuestVA va, size_t len, bool write) {
    if (!len || va + len < va) return false;
    va = a64_mapping_va(va);
    const GuestVA end = va + len;
    std::shared_lock<std::shared_mutex> lk(g_a64_maps_mu);
    while (va < end) {
        const A64Mapping *m = a64_find_locked(va);
        if (!m || !(m->prot & 1u /* PROT_READ */) ||
            (write && !(m->prot & 2u /* PROT_WRITE */)))
            return false;
        va = m->hi < end ? m->hi : end;
    }
    return true;
}

inline void a64_map_insert(GuestVA lo, GuestVA hi, uint32_t prot, bool owned) {
    std::unique_lock<std::shared_mutex> lk(g_a64_maps_mu);
    auto it = std::lower_bound(g_a64_maps.begin(), g_a64_maps.end(), lo,
                               [](const A64Mapping &m, GuestVA v) { return m.lo < v; });
    g_a64_maps.insert(it, A64Mapping{lo, hi, prot, owned});
    a64_maps_bump();
}

/* Declare pages in the 4 GiB image/backing window as part of the guest
 * process address space.
 *
 * ArmMemory::map() is the low-VA equivalent of mmap/brk: callers use it for
 * the libc heap, stacks, TLS, linker images and runtime data.  The host arena
 * happens to be reserved in one piece, but that reservation is not the guest
 * page table.  Keep these declarations in the same table as ordinary A64
 * mmap() ranges so permission checks and signal delivery see one coherent
 * Android address space.
 *
 * Small runtime maps commonly sit inside a range declared during process
 * setup.  Add only holes and coalesce adjacent declarations; blindly
 * inserting them would violate g_a64_maps' sorted/disjoint invariant. */
inline void a64_map_declare_backing(BackingOffset base, uint64_t len,
                                    uint32_t prot = 3u /* R|W */) {
    if (!len) return;
    uint64_t off_lo = (uint64_t)base & ~4095ull;
    uint64_t off_hi = ((uint64_t)base + len + 4095ull) & ~4095ull;
    if (off_hi <= off_lo || off_hi > A64_GUEST_SIZE) return;
    GuestVA lo = a64_guest_va((BackingOffset)off_lo);
    const GuestVA hi = A64_GUEST_BASE + off_hi;

    std::unique_lock<std::shared_mutex> lk(g_a64_maps_mu);
    while (lo < hi) {
        auto it = std::lower_bound(g_a64_maps.begin(), g_a64_maps.end(), lo,
                                   [](const A64Mapping &m, GuestVA v) {
                                       return m.lo < v;
                                   });
        if (it != g_a64_maps.begin()) {
            const A64Mapping &prev = *std::prev(it);
            if (prev.hi > lo) { lo = std::min(prev.hi, hi); continue; }
        }
        if (it != g_a64_maps.end() && it->lo <= lo) {
            lo = std::min(it->hi, hi);
            continue;
        }
        GuestVA gap_hi = it == g_a64_maps.end() ? hi : std::min(hi, it->lo);
        it = g_a64_maps.insert(it, A64Mapping{lo, gap_hi, prot, false});
        lo = gap_hi;
    }
    for (size_t i = 1; i < g_a64_maps.size();) {
        A64Mapping &a = g_a64_maps[i - 1];
        const A64Mapping &b = g_a64_maps[i];
        if (a.hi == b.lo && a.prot == b.prot && a.owned == b.owned) {
            a.hi = b.hi;
            g_a64_maps.erase(g_a64_maps.begin() + (long)i);
        } else {
            ++i;
        }
    }
    a64_maps_bump();
}

// Drop [lo,hi) from the table, splitting/trimming entries as needed.
inline void a64_map_remove(GuestVA lo, GuestVA hi, bool release) {
    std::unique_lock<std::shared_mutex> lk(g_a64_maps_mu);
    for (size_t i = 0; i < g_a64_maps.size();) {
        A64Mapping &m = g_a64_maps[i];
        if (m.hi <= lo || m.lo >= hi) { ++i; continue; }
        GuestVA clo = std::max(m.lo, lo), chi = std::min(m.hi, hi);
        if (release && m.owned)
            ::munmap((void *)clo, (size_t)(chi - clo));
        bool head = m.lo < clo, tail = m.hi > chi;
        if (head && tail) {
            A64Mapping right{chi, m.hi, m.prot, m.owned};
            m.hi = clo;
            g_a64_maps.insert(g_a64_maps.begin() + (long)i + 1, right);
            i += 2;
        } else if (head) {
            m.hi = clo; ++i;
        } else if (tail) {
            m.lo = chi; ++i;
        } else {
            g_a64_maps.erase(g_a64_maps.begin() + (long)i);
        }
    }
    g_a64_unmap_gen.fetch_add(1, std::memory_order_release);
    a64_maps_bump();
}

// Record a guest mprotect over [lo,hi).
inline void a64_map_set_prot(GuestVA lo, GuestVA hi, uint32_t prot) {
    std::unique_lock<std::shared_mutex> lk(g_a64_maps_mu);
    for (size_t i = 0; i < g_a64_maps.size(); ++i) {
        A64Mapping &m = g_a64_maps[i];
        if (m.hi <= lo || m.lo >= hi || m.prot == prot) continue;
        GuestVA clo = std::max(m.lo, lo), chi = std::min(m.hi, hi);
        if (m.lo == clo && m.hi == chi) { m.prot = prot; continue; }
        A64Mapping head{m.lo, clo, m.prot, m.owned};
        A64Mapping tail{chi, m.hi, m.prot, m.owned};
        m.lo = clo; m.hi = chi; m.prot = prot;
        if (tail.hi > tail.lo) g_a64_maps.insert(g_a64_maps.begin() + (long)i + 1, tail);
        if (head.hi > head.lo) { g_a64_maps.insert(g_a64_maps.begin() + (long)i, head); ++i; }
    }
    a64_maps_bump();
}

// Guest VA is host VA, so a mapping must never land inside the image window.
inline bool a64_va_usable(void *p, uint64_t len) {
    if (p == MAP_FAILED) return false;
    uint64_t lo = (uint64_t)p, hi = lo + len;
    if (lo < A64_GUEST_BASE + A64_GUEST_SIZE && hi > A64_GUEST_BASE) return false;
    // Must stay inside the AArch64 lower-half canonical range (52-bit VA).
    if (hi > 0x0010000000000000ull) return false;
    return true;
}

// Reserve `len` bytes of 64-bit guest VA backed by real host pages.
inline GuestVA a64_va_map(GuestVA hint, uint64_t len, uint32_t prot,
                          bool fixed = false) {
    len = (len + 4095ull) & ~4095ull;
    if (!len) return 0;
    void *want = nullptr;
    if (hint && !a64_is_guest_va(hint) && hint >= 0x10000ull &&
        !(hint & 4095ull) && hint + len <= 0x0010000000000000ull)
        want = (void *)hint;
    if (fixed && !want) { errno = EINVAL; return 0; }
    /* Android exposes 4 KiB pages while Apple Silicon uses 16 KiB host
     * pages.  Scudo reserves a large PROT_NONE span and commits 4 KiB-aligned
     * subranges with MAP_FIXED; asking Darwin to mmap such a subrange fails
     * with EINVAL when it is not host-page aligned.  Our anonymous reserve is
     * deliberately host-RW already, so replacing a wholly mapped guest range
     * is represented by zeroing it and changing the guest VMA metadata. */
    if (fixed && a64_mapped_span((GuestVA)want) >= len) {
        memset(want, 0, (size_t)len);
        a64_map_remove((GuestVA)want, (GuestVA)want + len, /*release=*/false);
        a64_map_insert((GuestVA)want, (GuestVA)want + len, prot, /*owned=*/true);
        return (GuestVA)want;
    }
    if (fixed)
        a64_map_remove((GuestVA)want, (GuestVA)want + len, /*release=*/false);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    if (fixed) flags |= MAP_FIXED;
    void *p = ::mmap(want, (size_t)len, PROT_READ | PROT_WRITE,
                     flags, -1, 0);
    if (fixed && p != want) {
        if (p != MAP_FAILED) ::munmap(p, (size_t)len);
        return 0;
    }
    if (!a64_va_usable(p, len)) {
        if (fixed) {
            if (p != MAP_FAILED) ::munmap(p, (size_t)len);
            return 0;
        }
        // Landed in the image window (or an unusable hint).
        void *bad = p;
        p = (want || p != MAP_FAILED)
                ? ::mmap(nullptr, (size_t)len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)
                : MAP_FAILED;
        if (bad != MAP_FAILED) ::munmap(bad, (size_t)len);
        if (!a64_va_usable(p, len)) {
            if (p != MAP_FAILED) ::munmap(p, (size_t)len);
            return 0;
        }
    }
    a64_map_insert((GuestVA)p, (GuestVA)p + len, prot, /*owned=*/true);
    return (GuestVA)p;
}

/* File-backed mmap: guest VA == host VA, so this is a real host mmap of the
 * file, not an anonymous reservation that is then filled with pread.
 *
 * The copy path was the emulator pretending to have mmap.  A UE title maps
 * its pak files this way; copying hundreds of megabytes (or gigabytes) inside
 * the SVC — while holding the execution lock — is why the title→world
 * transition crawled while a phone, which only installs a VMA, is instant. */
inline GuestVA a64_va_mmap_file(GuestVA hint, uint64_t len, uint32_t prot,
                                int host_flags, int fd, int64_t off) {
    len = (len + 4095ull) & ~4095ull;
    if (!len || fd < 0) return 0;
    int host_prot = (int)(prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
    if (!host_prot) host_prot = PROT_NONE;
    if (!(host_flags & (MAP_SHARED | MAP_PRIVATE)))
        host_flags |= MAP_PRIVATE;
    host_flags &= ~MAP_ANONYMOUS;
    host_flags |= MAP_NORESERVE;

    bool fixed = (host_flags & MAP_FIXED) != 0;
    void *want = nullptr;
    if (hint && !a64_is_guest_va(hint) && hint >= 0x10000ull &&
        !(hint & 4095ull) && hint + len <= 0x0010000000000000ull)
        want = (void *)hint;
    if (fixed && want)
        a64_map_remove((GuestVA)want, (GuestVA)want + len, /*release=*/true);

    void *p = ::mmap(want, (size_t)len, host_prot, host_flags, fd, (off_t)off);
    if (!a64_va_usable(p, len) || (fixed && want && p != want)) {
        void *bad = p;
        if (fixed) {
            if (bad != MAP_FAILED) ::munmap(bad, (size_t)len);
            return 0;
        }
        int retry = host_flags & ~(int)MAP_FIXED;
#ifdef MAP_FIXED_NOREPLACE
        retry &= ~(int)MAP_FIXED_NOREPLACE;
#endif
        p = ::mmap(nullptr, (size_t)len, host_prot, retry, fd, (off_t)off);
        if (bad != MAP_FAILED && bad != p) ::munmap(bad, (size_t)len);
        if (!a64_va_usable(p, len)) {
            if (p != MAP_FAILED) ::munmap(p, (size_t)len);
            return 0;
        }
    }
    a64_map_insert((GuestVA)p, (GuestVA)p + len, prot, /*owned=*/true);
    return (GuestVA)p;
}

/* The SVC register file is 64 bits wide on both JITs, so an A64 pointer is
 * handed to a handler as itself.  (It used to be squeezed into a 32-bit slot
 * through an interned 64 KiB "pointer token", which truncated every jlong and
 * broke any handler that did arithmetic across a segment boundary.) */

// Successful anon mmap slabs [lo,hi).
struct MmapSlab { uint32_t lo, hi; };
inline std::vector<MmapSlab> g_mmap_slabs;
// PROT_NONE anon bytes reserved.
inline uint64_t g_mmap_prot_none_bytes = 0;

inline bool mmap_prot_none_budget_ok(uint32_t len) {
    // PROT_NONE consumes guest VA, not committed RAM.
    uint64_t mmap2_end = g_mmap2_end ? (uint64_t)g_mmap2_end : 0x100000000ull;
    uint64_t budget = (uint64_t)(MMAP_END - MMAP_BASE) +
                      (mmap2_end - (uint64_t)g_mmap2_base);
    if (g_mmap_prot_none_bytes + (uint64_t)len <= budget)
        return true;
    static int n = 0;
    if (n++ < 8)
        fprintf(stderr, "[mmap] MAP_FAILED PROT_NONE len=%u: reserved=%lluMB budget=%lluMB "
                "(VA reserve cap)\n",
                len, (unsigned long long)(g_mmap_prot_none_bytes >> 20),
                (unsigned long long)(budget >> 20));
    return false;
}

#define HEAP_SIZE  (g_heap_size)
#define MMAP2_BASE (g_mmap2_base)
#define MMAP2_END  (g_mmap2_end)
inline uint64_t mmap2_end_excl(void) {
    return g_mmap2_end ? (uint64_t)g_mmap2_end : 0x100000000ull;
}

/* True once the process is running a 64-bit guest.  Set before
 * guest_layout_init(), which sizes the heap differently for the two. */
inline bool g_guest_proc_arm64 = false;

inline void guest_layout_init(void) {
    static bool once = false;
    if (once) return;
    once = true;
    long m2b = lunaria_env_long("LUNARIA_MMAP2_BASE", 0);
    long m2e = lunaria_env_long("LUNARIA_MMAP2_END", 0);

    /* How much of the window malloc and the small-mmap fallback share.
     *
     * A 32-bit guest has one 4 GiB window holding images, thread stacks, mmap
     * and heap, so 256 MiB of malloc arena there is a deliberate share of it.
     * A 64-bit guest puts its mmaps in the real 64-bit address space
     * (guest_va_layout_arm64), which leaves everything from HEAP_BASE up to
     * MMAP2_BASE with nothing in it but this arena — and 256 MiB of that is
     * not a budget, it is a leftover.  Genshin's Unity allocator reported
     * "System out of memory! Trying to allocate 262160B" with 142 MiB in use,
     * on a device this emulator tells the guest has 6 GiB of RAM.  Default to
     * the whole window instead; LUNARIA_HEAP_MB still overrides it. */
    const uint32_t heap_window_top = m2b ? (uint32_t)m2b : 0xF0000000u;
    const long window_mb = (long)((heap_window_top - HEAP_BASE) >> 20);
    long max_mb = g_guest_proc_arm64 ? window_mb
                                     : (window_mb < 1024 ? window_mb : 1024);
    long heap_mb = lunaria_env_long("LUNARIA_HEAP_MB",
                                    g_guest_proc_arm64 ? window_mb : 256);
    if (heap_mb < 64) heap_mb = 64;
    if (heap_mb > max_mb) heap_mb = max_mb;
    g_heap_size = (uint32_t)heap_mb * 1024u * 1024u;
    uint32_t heap_end = HEAP_BASE + g_heap_size;
    g_small_mmap_top = heap_end;
    g_mmap2_base = m2b ? (uint32_t)m2b
                       : ((heap_end + 0xfffffu) & ~0xfffffu);
    g_mmap2_end  = m2e ? (uint32_t)m2e : 0u;
    if (g_mmap2_base < heap_end + 0x1000u)
        g_mmap2_base = (heap_end + 0xfffffu) & ~0xfffffu;
    fprintf(stderr, "[mem] layout heap=[%08x,%08x) mmap2=[%08x,%llx) "
            "report total=%ldMB avail=%ldMB "
            "(LUNARIA_MEM_TOTAL_MB / LUNARIA_HEAP_MB / LUNARIA_MMAP2_*)\n",
            HEAP_BASE, HEAP_BASE + g_heap_size, g_mmap2_base,
            (unsigned long long)mmap2_end_excl(),
            lunaria_mem_total_mb(), lunaria_mem_avail_mb());
}

// Image-window offset of a guest VA, for the code paths that still reason in 32-bit backing offsets (RX guards.
inline uint32_t a64_canon_va(uint64_t va) {
    if (a64_is_guest_va(va)) return (uint32_t)(va - A64_GUEST_BASE);
    if (va < A64_GUEST_SIZE) return (uint32_t)va; /* bare offset from A32 paths */
    return 0;
}

// size_t / count args: A64 W-ops zero-extend.
inline size_t a64_buf_len(uint64_t x) {
    return (x >> 32) ? (size_t)(uint32_t)x : (size_t)x;
}

// Guest library regions, recorded by load_elf.
/* One PT_LOAD of a mapped image, as /proc/<pid>/maps has to describe it.
 * `file_off` is the page-aligned file offset the region maps: a device's maps
 * names it per line, and every segment of a library claiming offset 0 is not
 * a state the kernel can produce. */
struct LoadedRegion {
   uint32_t lo, hi;
   uint32_t flags;
   uint64_t file_off;
   std::string path;
   /* Captured while the loader still owns the fd.  A mapped ELF may be
    * unlinked later; Linux keeps its inode in maps and suffixes the name with
    * " (deleted)" rather than turning it into a named 00:00/inode-0 map. */
   uint64_t backing_inode = 0;
   /* Linux mappings belong to an mm/process.  The emulator has one backing
    * arena, but procfs must never expose an exec child image in its parent. */
   uint32_t process_pid = 1000u; /* initial Android app process */
};
inline std::vector<LoadedRegion> g_loaded_regions;
// Process-wide ABI selector used by synthetic /proc files.

// Program-header metadata exposed through dl_iterate_phdr.
struct ModulePhdr {
    GuestVA load_bias;
    GuestVA name_va;
    GuestVA phdr_va;
    uint16_t phnum;
    bool is_64;
    uint32_t process_pid = 1000u;
};
inline std::vector<ModulePhdr> g_module_phdrs;

inline void warn_unknown_reloc(const char *abi, uint32_t type,
                               const char *symbol) {
    static std::map<std::string, unsigned> counts;
    std::string key = std::string(abi ? abi : "?") + ":" +
                      std::to_string(type) + ":" +
                      (symbol && *symbol ? symbol : "<none>");
    unsigned n = ++counts[key];
    if (n <= 4)
        fprintf(stderr, "[%s] unknown reloc type=%u symbol=%s occurrence=%u\n",
                abi ? abi : "reloc", type,
                symbol && *symbol ? symbol : "<none>", n);
}

// Mono rejects corlib unless the path sits under .../Managed/mono/2.0/.
inline void mono_rewrite_corlib_path(std::string &path) {
    const char *needle = "/Managed/mscorlib.dll";
    size_t pos = path.find(needle);
    if (pos != std::string::npos && path.find("/mono/") == std::string::npos)
        path.replace(pos, strlen(needle), "/Managed/mono/2.0/mscorlib.dll");
}

// Bump-allocate a guest mmap region of `raw_len` bytes (before page-rounding).
// Freed guest VA ranges, kept sorted, coalesced and MMAP_ALIGN-aligned.
inline std::vector<MmapSlab> g_mmap_freelist;

inline void mmap_free_insert(uint32_t lo, uint32_t hi) {
    // Only whole aligned blocks go back: mmap_bump hands out MMAP_ALIGN-aligned addresses and callers rely on that.
    lo = (uint32_t)(((uint64_t)lo + MMAP_ALIGN - 1u) & ~(uint64_t)(MMAP_ALIGN - 1u));
    hi &= ~(MMAP_ALIGN - 1u);
    if (hi <= lo) return;
    auto it = std::lower_bound(g_mmap_freelist.begin(), g_mmap_freelist.end(), lo,
                               [](const MmapSlab &s, uint32_t v) { return s.lo < v; });
    it = g_mmap_freelist.insert(it, MmapSlab{lo, hi});
    if (it != g_mmap_freelist.begin()) {
        auto prev = std::prev(it);
        if (prev->hi >= it->lo) {
            prev->hi = std::max(prev->hi, it->hi);
            it = std::prev(g_mmap_freelist.erase(it));
        }
    }
    for (auto nxt = std::next(it);
         nxt != g_mmap_freelist.end() && nxt->lo <= it->hi;
         nxt = std::next(it)) {
        it->hi = std::max(it->hi, nxt->hi);
        g_mmap_freelist.erase(nxt);
    }
}

// First fit.
inline uint32_t mmap_free_take(uint32_t len) {
    for (auto it = g_mmap_freelist.begin(); it != g_mmap_freelist.end(); ++it) {
        if ((uint64_t)it->hi - it->lo < len) continue;
        uint32_t addr = it->lo;
        uint64_t next = ((uint64_t)addr + len + MMAP_ALIGN - 1u) &
                        ~(uint64_t)(MMAP_ALIGN - 1u);
        if (next >= it->hi) g_mmap_freelist.erase(it);
        else it->lo = (uint32_t)next;
        if (lunaria_env("LUNARIA_TRACE_MMAP"))
            fprintf(stderr, "[mmap] reuse %08x len=%u (%zu free ranges left)\n",
                    addr, len, g_mmap_freelist.size());
        return addr;
    }
    return ~0u;
}

inline uint32_t mmap_bump(uint32_t raw_len) {
    uint32_t len = (raw_len + 4095u) & ~4095u;
    if (len == 0 || len > g_mmap_max_single) {
        static int cap = 0;
        if (cap++ < 4)
            fprintf(stderr, "[mmap] reject len=%u > cap=%u\n", len, g_mmap_max_single);
        return ~0u;
    }

    // Recycled space first — otherwise the bump pointers walk off the end of both arenas and never come back.
    if (uint32_t reuse = mmap_free_take(len); reuse != ~0u) return reuse;

    auto bump_in = [&](uint32_t &next, uint64_t arena_end) -> uint32_t {
        for (int spins = 0; spins < 64; ++spins) {
            if ((uint64_t)next + len > arena_end) return ~0u;
            uint32_t addr = next;
            uint64_t end64 = (uint64_t)addr + len;
            bool hit = false;
            for (const auto &r : g_loaded_regions) {
                if (addr < r.hi && end64 > r.lo) {
                    uint32_t skip = (r.hi + MMAP_ALIGN - 1u) & ~(MMAP_ALIGN - 1u);
                    if (skip <= next) return ~0u;
                    static int logged = 0;
                    if (logged++ < 8)
                        fprintf(stderr, "[mmap] skip lib overlap [%08x,%llx) ∩ [%08x,%08x) → next=%08x\n",
                                addr, (unsigned long long)end64, r.lo, r.hi, skip);
                    next = skip;
                    hit = true;
                    break;
                }
            }
            if (hit) continue;
            // Advance with uint64 math — uint32 (addr+len) wraps at 4 GiB and used to reset next to 0.
            uint64_t n64 = (end64 + MMAP_ALIGN - 1u) & ~(uint64_t)(MMAP_ALIGN - 1u);
            if (n64 >= arena_end || n64 > 0xffffffffull)
                next = 0xffffffffu;
            else
                next = (uint32_t)n64;
            return addr;
        }
        return ~0u;
    };

    uint32_t addr = bump_in(g_mmap_next, MMAP_END);
    if (addr != ~0u) return addr;

    if (!g_mmap2_active) {
        g_mmap2_active = true;
        g_mmap2_next = MMAP2_BASE;
        fprintf(stderr, "[mmap] primary arena exhausted (next=%08x); activating "
                "secondary [%08x,%llx)\n", g_mmap_next, MMAP2_BASE,
                (unsigned long long)mmap2_end_excl());
    } else if (g_mmap2_next == 0) {
        // Exhausted / corrupted bump ptr — do not rewind to BASE.
        static int rewind = 0;
        if (rewind++ < 4)
            fprintf(stderr, "[mmap] secondary bump exhausted (next=0), refusing rewind\n");
        return ~0u;
    }
    addr = bump_in(g_mmap2_next, mmap2_end_excl());
    if (addr == ~0u && len <= 0x01000000u && g_small_mmap_top) {
        // PROT_NONE slabs consume the normal mmap arenas on A64.  The floor is
        // wherever malloc has bumped to: the two share the window, and the
        // heap's own lock decides between them (src/lib/guest.c).
        uint32_t next = arm_heap_take_top(len, MMAP_ALIGN - 1u);
        if (next) {
            static int fb = 0;
            if (fb++ < 16)
                fprintf(stderr, "[mmap] small A64 fallback len=%u -> %08x\n", len, next);
            return next;
        }
    }
    if (addr == ~0u) {
        static int oom = 0;
        if (oom++ < 8)
            fprintf(stderr, "[mmap] MAP_FAILED len=%u primary_next=%08x secondary_next=%08x\n",
                    len, g_mmap_next, g_mmap2_next);
    }
    return addr;
}

// Linux mmap: prefer exact `len`.
// allow_split=false: return ~0u if exact size unavailable (overflow mmap).
inline uint32_t mmap_bump_exact(uint32_t raw_len, uint32_t *out_len = nullptr,
                                bool allow_split = true) {
    uint32_t want = (raw_len + 4095u) & ~4095u;
    if (want == 0) return ~0u;
    if (want > g_mmap_max_single) {
        static int cap = 0;
        if (cap++ < 8)
            fprintf(stderr, "[mmap] MAP_FAILED len=%u > max_single=%u\n",
                    want, g_mmap_max_single);
        return ~0u;
    }
    uint32_t addr = mmap_bump(want);
    if (addr != ~0u) {
        if (out_len) *out_len = want;
        g_mmap_slabs.push_back({addr, addr + want});
        return addr;
    }
    // Never short-map a large reservation.
    if (!allow_split || want > 0x10000000u)
        return ~0u;
    uint32_t try_len = want;
    for (int i = 0; i < 16 && try_len >= 0x10000u; ++i) {
        try_len >>= 1;
        try_len &= ~4095u;
        if (!try_len) break;
        addr = mmap_bump(try_len);
        if (addr != ~0u) {
            // Report requested size to caller bookkeeping that updates r1.
            if (out_len) *out_len = want;
            g_mmap_slabs.push_back({addr, addr + try_len});
            static int split = 0;
            if (split++ < 8)
                fprintf(stderr, "[mmap] split request %u → %u at %08x\n",
                        want, try_len, addr);
            return addr;
        }
    }
    return ~0u;
}

// Union bounds of the RX segments, maintained by guest_rx_bounds_refresh().
inline uint32_t g_rx_lo = ~0u, g_rx_hi = 0u;
inline void guest_rx_bounds_refresh(void) {
    g_rx_lo = ~0u; g_rx_hi = 0u;
    for (const auto &r : g_loaded_regions) {
        if (!(r.flags & 1u) || (r.flags & 2u)) continue; /* need X, not W */
        if (r.lo < g_rx_lo) g_rx_lo = r.lo;
        if (r.hi > g_rx_hi) g_rx_hi = r.hi;
    }
}

// True if [va, va+n) overlaps any loaded RX (PF_X and not PF_W) segment.
inline bool guest_range_hits_rx(uint32_t va, uint32_t n) {
    if (!n) return false;
    uint64_t end = (uint64_t)va + n;
    // Fast reject.
    if (va >= g_rx_hi || end <= (uint64_t)g_rx_lo) return false;
    if (end > 0x100000000ull) end = 0x100000000ull;
    for (const auto &r : g_loaded_regions) {
        if (!(r.flags & 1u) || (r.flags & 2u)) continue; /* need X, not W */
        if (va < r.hi && end > r.lo) return true;
    }
    return false;
}

// True if [va, va+n) overlaps any recorded PT_LOAD (code or data).
inline bool guest_range_hits_loaded(uint32_t va, uint32_t n) {
    if (!n) return false;
    uint64_t end = (uint64_t)va + n;
    if (end > 0x100000000ull) end = 0x100000000ull;
    for (const auto &r : g_loaded_regions) {
        if (va < r.hi && end > r.lo) return true;
    }
    return false;
}
// sbrk / brk arena for Boehm GC's GC_scratch_alloc().
constexpr uint32_t BRK_BASE = 0x04000000u;
inline uint32_t           BRK_END  = 0x10000000u;
inline uint32_t g_brk = BRK_BASE;

inline uint32_t TRAMP_BASE    = 0x41000000u;
inline uint32_t JNI_TBL_BASE  = 0x41010000u; /* JNINativeInterface[229] */
inline uint32_t JVM_TBL_BASE  = 0x41011000u; /* JNIInvokeInterface[8] (A32) */
inline uint32_t ENV_SLOT_BASE = 0x41012000u; /* holds JNI_TBL_BASE (A32) */
inline uint32_t VM_SLOT_BASE  = 0x41012008u; /* holds JVM_TBL_BASE (A32) */
// A64: JNI table still at JNI_TBL_BASE but with 8-byte slots; JVM table moves to ENV_SLOT VA.
inline uint32_t JNI_TBL64_BASE  = 0x41010000u;
inline uint32_t JVM_TBL64_BASE  = 0x41012000u;
inline uint32_t ENV_SLOT64_BASE = 0x41012100u;
inline uint32_t VM_SLOT64_BASE  = 0x41012108u;
inline uint32_t STR_SCRATCH   = 0x41013000u; /* 4KB scratch for strings */
inline uint32_t LIBC_DATA     = 0x41014000u; /* 4KB libc data globals */
inline uint32_t LIBC_PAGE_SIZE  = 0x41014000u; /* = 4096 word */
inline uint32_t LIBC_PAGE_SHIFT = 0x41014004u;
inline uint32_t LIBC_PAGE_MASK  = 0x41014008u;
inline uint32_t MJIV_CACHE      = 0x41014020u;
inline uint32_t MJIV_ENTRY_MARK = 0x41014024u;
inline uint32_t MJIV_RUNTIME_VER = 0x41014028u;
inline uint32_t MONO_EMPTY_STR     = 0x4101402cu;
inline uint32_t LIBC_CTYPE_TAB     = 0x41014300u;
inline uint32_t LIBC_TOLOWER_TAB   = 0x41014400u;
inline uint32_t g_reloc_data_start = 0; /* per-library .data start for data_start/__data_start */

// tiny ARM stub that does nothing and returns r0=0.
inline uint32_t NOOP_RET0      = 0x41014100u;
// Fixed guest data the loader synthesises for data-only imports.
inline uint32_t MISC_DATA     = 0x41014000u;
inline uint32_t SL_PAGE_BASE  = 0x41016000u;
inline uint32_t misc_sl_iid(void)     { return MISC_DATA + 0x000u; }
inline uint32_t misc_sl_iid_end(void) { return MISC_DATA + 0x100u; }
/* tzname is `char *tzname[2]`, and the two strings have to live somewhere the
 * guest can read: the array itself at +0x100, the abbreviations after it.
 * `timezone` (long) and `daylight` (int) are separate objects in bionic, so
 * they get separate words -- one address for both meant a write to either
 * changed the other. */
inline uint32_t misc_tzname(void)     { return MISC_DATA + 0x100u; }
inline uint32_t misc_tzname_str(int i){ return MISC_DATA + 0x120u + (uint32_t)i * 0x20u; }
inline uint32_t misc_tzvars(void)     { return MISC_DATA + 0x180u; }
inline uint32_t misc_daylight(void)   { return MISC_DATA + 0x190u; }
inline uint32_t misc_stdio(void)      { return MISC_DATA + 0x200u; }
inline uint32_t misc_environ(void)    { return MISC_DATA + 0x300u; }
inline uint32_t misc_env_array(void)  { return MISC_DATA + 0x320u; }
inline uint32_t misc_env_strings(void){ return MISC_DATA + 0x380u; }
/* getopt(3) process state.  Between daylight and the stack-guard cookie.
 * optarg is a pointer; the ints follow.  Defaults match bionic: optind=1,
 * opterr=1, optopt=0, optarg=NULL. */
inline uint32_t misc_optarg(void)     { return MISC_DATA + 0x1A0u; }
inline uint32_t misc_optind(void)     { return MISC_DATA + 0x1A8u; }
inline uint32_t misc_opterr(void)     { return MISC_DATA + 0x1ACu; }
inline uint32_t misc_optopt(void)     { return MISC_DATA + 0x1B0u; }
/* __stack_chk_guard is a *variable* the compiled guest reads directly (the
 * prologue copies it onto the stack, the epilogue compares).  Binding it to a
 * code trampoline handed out the address of an instruction as the guard
 * value; it happened to compare equal, but the whole point of the cookie is
 * that it is unpredictable.  Give it a word of real entropy instead. */
inline uint32_t misc_stack_guard(void) { return MISC_DATA + 0x1C0u; }
/* The AThermalManager AThermal_acquireManager() hands out.  The object is
 * opaque to the guest — the thermal calls only pass the pointer back — but it
 * has to be an address in guest memory, not an arbitrary token, so a caller
 * that stores it beside other pointers or compares it with one is not looking
 * at something that could never be mapped. */
inline uint32_t misc_athermal(void)    { return MISC_DATA + 0x1E0u; }
// glGetString/eglQueryString ring (8 × 8 KiB) — see stash_gl_c_string().
inline uint32_t GL_STR_RING_BASE = 0x41020000u;
constexpr uint32_t GL_STR_RING_SLOTS = 8u;
constexpr uint32_t GL_STR_RING_SLOT  = 8192u; /* GL_EXTENSIONS can exceed 4 KiB */
constexpr uint32_t GL_STR_RING_SIZE  = GL_STR_RING_SLOTS * GL_STR_RING_SLOT;
/* Storage for a direct java.nio.ByteBuffer the bytecode VM allocated.
 *
 * A direct buffer's whole contract is that JNI can take its address:
 * GetDirectBufferAddress() has to answer with a pointer the guest can
 * dereference.  The VM's own arrays live in host memory the guest cannot
 * name, so a buffer it allocates is backed from this guest region instead.
 * A native that is handed 0 does not fail politely — Unity's UnityWebRequest
 * upload path takes its "how much is there in total" branch instead of its
 * "fill this buffer" one and loops for ever. */
/* 0x41100000..0x41102000 is executable runtime code (FAST_SYNC64_PAGE in
 * arm_exec.cpp).  Direct-buffer payload is writable data and must never
 * overlap it: UnityWebRequest writes the response into this pool, and the old
 * overlap replaced the pthread fast stubs with response bytes.  A worker then
 * returned through those bytes as A64 instructions. */
inline uint32_t DIRECT_BB_BASE = 0x41200000u;
constexpr uint32_t DIRECT_BB_SIZE = 0x00800000u;  /* 8 MiB */
// Per-thread guest TLS pages (tpidr_el0), one 4 KiB page per guest tid.
inline uint32_t TLS_WINDOW_BASE = 0x41030000u;
inline uint32_t TLS_WINDOW_END  = 0x41040000u;
inline uint32_t STACK_BASE    = 0x42000000u;
inline uint32_t STACK_SIZE    = 0x05000000u;  /* 80MB */
inline uint32_t SENTINEL_ADDR = 0x43000000u;
inline uint32_t CB_STACK_BASE = 0x47700000u;
/* One window per concurrent outermost callback — the stack of the thread the
 * host borrowed to make the call.  A callback nested inside it (native ->
 * Java -> native, the ordinary JNI shape) runs on that same stack below the
 * caller's SP, exactly as it does on a device; it has no window of its own.
 * So the depth is bounded by stack and by CB_MAX_DEPTH (one callback JIT per
 * level, since a dynarmic Jit cannot be re-entered), not by address layout.
 * The old layout gave every level its own 512 KiB window and allowed two, and
 * the third level of an everyday JNI chain was dropped with its result left
 * at zero. */
constexpr uint32_t CB_STACK_SIZE = 0x00100000u;
constexpr int      CB_MAX_DEPTH  = 16;
/* A guest callback needs a JIT, an exclusive-monitor id and a guest stack,
 * and none of the three may be shared with a callback running at the same
 * time.  Nest depth alone was enough while one host thread ran all guest code;
 * with the engine pool and the dvm's own Java threads several host threads
 * reach call_guest_abi() at once, and sharing by depth then puts two of them
 * inside one dynarmic Jit (which asserts !is_executing) on one guest stack.
 *
 * Stack windows are a freelist of CB_MAX_SLOTS entries: claimed for the
 * duration of an outermost callback and released on return.  Holding them
 * for the life of every host thread that ever called once exhausted the
 * pool (8) under Cross Worlds — further callbacks were skipped, and the
 * anti-tamper thread that lost its signal sat in a Java retry loop that
 * pinned the interpreter lock for seconds at a time.
 *
 * Exclusive-monitor ids are per host thread, not per callback JIT; see
 * EXCL_ID_PE below. */
constexpr int      CB_MAX_SLOTS  = 16;           /* concurrent stack windows */
/* How many of those slots the guest address space actually has stack windows
 * for; the layout functions below set it from the room they have. */
inline int         g_cb_slots    = 4;
inline uint32_t cb_stack_base(int slot) {
    return CB_STACK_BASE + (uint32_t)slot * CB_STACK_SIZE;
}

/* Exclusive-monitor processor ids.
 *
 * A reservation belongs to the processing element that took it, and this
 * emulator's processing elements are the host threads that run guest code:
 * an engine, or any host thread that enters a guest callback.  Several JITs
 * live on one such thread (the engine's, plus one callback JIT per nest depth
 * for A32 and A64), but only one of them executes at a time, and a nested one
 * clobbering the outer one's reservation is precisely what a real PE does
 * when an exception handler uses LL/SC: the outer STXR fails and the guest's
 * retry loop takes it from there.  So the id belongs to the host thread, not
 * to the JIT object.
 *
 * The count is not just memory.  dynarmic emits one inline compare-and-clear
 * per *other* processor at every store-exclusive, and its out-of-line path
 * scans the whole table under one global spin lock.  Giving every possible
 * callback thread two ids per nest depth sized this monitor at 266 — 265
 * compares over 33 cache lines on every STXR, and UE4 takes a reservation on
 * every refcount.  One id per host thread that can run guest code is both the
 * architecturally right model and two orders of magnitude cheaper. */
constexpr int      A64_ENGINE_MAX = 8;
constexpr int      EXCL_PE_MAX    = 14;  /* host threads that run guest code */
enum : size_t {
    EXCL_ID_MAIN  = 0,
    EXCL_ID_AUX   = 1,
    EXCL_ID_PE    = 2,
    EXCL_ID_COUNT = EXCL_ID_PE + EXCL_PE_MAX,
};

// A64: tramp/stack below MMAP_BASE so primary holds 2×512 MiB exact slabs and secondary 5× → 7 exact 512 MiB (A64 Unity.
inline void guest_va_layout_arm64(void) {
    // Ask ArmMemory::init() for the image window at A64_GUEST_BASE.
    g_a64_identity_arena = true;
    BRK_END           = 0x0A000000u;
    TRAMP_BASE        = 0x0A000000u;
    JNI_TBL_BASE      = 0x0A010000u;
    JVM_TBL_BASE      = 0x0A011000u;
    ENV_SLOT_BASE     = 0x0A012000u;
    VM_SLOT_BASE      = 0x0A012008u;
    JNI_TBL64_BASE    = JNI_TBL_BASE;
    JVM_TBL64_BASE    = 0x0A012000u;
    ENV_SLOT64_BASE   = 0x0A012100u;
    VM_SLOT64_BASE    = 0x0A012108u;
    STR_SCRATCH       = 0x0A013000u;
    LIBC_DATA         = 0x0A014000u;
    LIBC_PAGE_SIZE    = LIBC_DATA + 0x00u;
    LIBC_PAGE_SHIFT   = LIBC_DATA + 0x04u;
    LIBC_PAGE_MASK    = LIBC_DATA + 0x08u;
    MJIV_CACHE        = LIBC_DATA + 0x20u;
    MJIV_ENTRY_MARK   = LIBC_DATA + 0x24u;
    MJIV_RUNTIME_VER  = LIBC_DATA + 0x28u;
    MONO_EMPTY_STR    = LIBC_DATA + 0x2cu;
    LIBC_CTYPE_TAB    = LIBC_DATA + 0x300u;
    LIBC_TOLOWER_TAB  = LIBC_DATA + 0x400u;
    NOOP_RET0         = LIBC_DATA + 0x100u;
    // The SL_IID/tzname/stdio page and the OpenSL ES vtable page keep their A32 addresses (0x41014000 / 0x41016000) by.
    MISC_DATA         = 0x0A015000u;
    SL_PAGE_BASE      = 0x0A016000u; /* 8 KiB: 64-bit vtables + instances */
    /* DETOUR64_STUB_BASE occupies 0x0A018000.  The next two windows must not
     * overlap each other: the GL string ring is written by host code on every
     * glGetString, so any thread whose TLS page landed inside it had its
     * stack-guard word (tpidr_el0+0x28) overwritten with extension-string
     * bytes — the next __stack_chk_fail aborted the process. */
    GL_STR_RING_BASE  = 0x0A020000u;              /* .. 0x0A030000 */
    TLS_WINDOW_BASE   = 0x0A030000u;              /* .. STACK_BASE  */
    TLS_WINDOW_END    = 0x0A100000u;
    STACK_BASE        = 0x0A100000u;
    STACK_SIZE        = 0x03000000u; /* 48 MiB */
    /* CB_MAX_SLOTS concurrent stacks x 1 MiB = 16 MiB. */
    CB_STACK_BASE     = 0x0D100000u; /* .. 0x0E100000 */
    g_cb_slots        = CB_MAX_SLOTS;
    SENTINEL_ADDR     = 0x0E200000u; /* must not alias CB nest stacks */
    // Guest thread stacks: [0x40000000, HEAP_BASE) — 256 MiB, i.
    THREAD_STACK_BASE = 0x40000000u;
    // ELF images: [0x10000000,0x50000000).
    A64_IMAGE_BASE    = MMAP_BASE;         /* 0x10000000 */
    A64_IMAGE_END     = THREAD_STACK_BASE; /* 0x40000000 — 768 MiB of images */
    MMAP_END          = MMAP_BASE;   /* primary arena disabled (see above) */
    g_mmap_next       = MMAP_BASE;
    // Guest mmap now lives in the real 64-bit address space, so the image window only has to hold code.
    setenv("LUNARIA_MMAP2_BASE", "0xF0000000", 0);
    lunaria_env_invalidate();
    fprintf(stderr, "[mem] a64 VA: tramp=%08x stack=%08x mmap=[%08x,%08x) "
            "heap=%08x\n",
            TRAMP_BASE, STACK_BASE, MMAP_BASE, MMAP_END, HEAP_BASE);
}

// MMU-ish: shrink a libc write to stay out of RX .
inline uint32_t guest_clamp_write_n(uint32_t dst, uint32_t n) {
    if (!n) return 0;
    if ((uint64_t)dst + n > 0x100000000ull)
        n = (uint32_t)(0x100000000ull - dst);
    if (!n) return 0;
    uint64_t end = (uint64_t)dst + n;
    for (const auto &r : g_loaded_regions) {
        if (!(r.flags & 1u) || (r.flags & 2u)) continue; /* X and not W */
        if (dst >= r.lo && dst < r.hi)
            return 0; /* starts in RX */
        if (dst < r.lo && end > r.lo) {
            n = r.lo - dst;
            end = (uint64_t)dst + n;
        }
    }
    // Tramp/JNI only — main/thread stacks are at STACK_BASE+.
    if (dst < TRAMP_BASE && end > TRAMP_BASE) {
        n = TRAMP_BASE - dst;
        end = (uint64_t)dst + n;
    }
    if (dst >= TRAMP_BASE && dst < STACK_BASE)
        return 0;
    // Do NOT clamp libc writes to mmap slab size (Unity Dynamic Heap bookkeeping).
    (void)g_mmap_slabs;
    return n;
}

// A clean return lands the guest PC on the (non-executable) SENTINEL page.
inline bool pc_in_sentinel(uint32_t pc) {
    uint32_t p = pc & ~1u;
    return p >= SENTINEL_ADDR && p < SENTINEL_ADDR + 0x1000u;
}
inline bool pc_in_sentinel(GuestVA pc) {
    return pc_in_sentinel(a64_canon_va(pc));
}


#include "svc_ids.h"

constexpr uint32_t ICALL_PROBE_STUB_BASE   = 0x4100e800u;
inline uint32_t g_icall_probe_next = ICALL_PROBE_STUB_BASE;
inline uint32_t g_icall_probe_count = 0;
inline const char *g_icall_probe_names[NUM_ICALL_PROBES] = {};
inline uint8_t g_icall_probe_kind[NUM_ICALL_PROBES] = {};
inline uint32_t g_mono_add_icall_real = 0;

inline bool g_asensor_enabled = false;

/* Which SVC a JNINativeInterface slot dispatches to.  Identity up to 221;
 * beyond that the historical numbering is four short, so name every slot. */
constexpr uint32_t jni_vtable_svc(uint32_t slot) {
    switch (slot) {
    case 222: return SVC_JNI_GET_PRIM_CRITICAL;
    case 223: return SVC_JNI_REL_PRIM_CRITICAL;
    case 224: return SVC_JNI_GET_STR_CRITICAL;
    case 225: return SVC_JNI_REL_STR_CRITICAL;
    case 226: return 222u;   /* NewWeakGlobalRef    */
    case 227: return 223u;   /* DeleteWeakGlobalRef */
    case 228: return 224u;   /* ExceptionCheck      */
    case 229: return SVC_JNI_NEW_DIRECT_BB;
    case 230: return SVC_JNI_DIRECT_BB_ADDR;
    case 231: return SVC_JNI_DIRECT_BB_CAP;
    case 232: return SVC_JNI_OBJECT_REF_TYPE;
    default:  return slot;
    }
}

// OpenSL ES fake object page.
// Everything here is measured in *guest pointer words*, not bytes.
constexpr uint32_t SL_VT_OBJECT     = 0u;   /* slot index; 16 slots each */
constexpr uint32_t SL_VT_ENGINE     = 16u;
constexpr uint32_t SL_VT_PLAY       = 32u;
constexpr uint32_t SL_VT_BUFQ       = 48u;
constexpr uint32_t SL_VT_VOLUME     = 56u;
constexpr uint32_t SL_VT_ANDROIDCFG = 72u;
constexpr uint32_t SL_VT_SLOTS      = 128u; /* reserved before instances */
constexpr uint32_t SL_INST_WORDS    = 8u;   /* per instance */
// Instance words: [0]=vtable [1]=kind [2]=bufq callback [3]=callback context [4]=owning object.
enum : uint32_t { SL_KIND_ENGINE = 1, SL_KIND_OUTMIX, SL_KIND_PLAYER };
inline uint32_t g_sl_inst_next = 0;
// SL_IID_* data symbol address → interface name (filled as they are resolved).
inline std::map<uint32_t, std::string> g_sl_iid_names;

constexpr uint32_t TRAMP_STRIDE     = 8u; /* ARM32: SVC #n + BX LR */

// dlsym'd-but-unimplemented symbols: slot i lives at trampoline index SVC_TRAMP_TOTAL + i and executes svc.
inline std::vector<std::string> g_unknown_sym_names;
inline std::map<std::string, uint32_t> g_unknown_sym_slot;
/* What that slot's stub answers, and whether it is a shared "return 0/-1"
 * template rather than a symbol nobody has heard of.  Both are per slot so the
 * stub can name itself when it is *called*: which functions were bound is a
 * property of the binary, which ones the run actually reached is a property of
 * the run, and only the second explains a wrong answer the guest acted on. */
inline std::vector<int32_t> g_unknown_sym_ret;
inline std::vector<uint8_t> g_unknown_sym_is_template;

constexpr uint32_t JVM_SLOT_RESERVED0  = 0;
constexpr uint32_t JVM_SLOT_RESERVED1  = 1;
constexpr uint32_t JVM_SLOT_RESERVED2  = 2;
constexpr uint32_t JVM_SLOT_DESTROY    = 3;
constexpr uint32_t JVM_SLOT_ATTACH     = 4;
constexpr uint32_t JVM_SLOT_DETACH     = 5;
constexpr uint32_t JVM_SLOT_GETENV     = 6;
constexpr uint32_t JVM_SLOT_ATTACH_DA  = 7;
constexpr uint32_t JVM_SLOT_COUNT      = 8;
#endif /* __cplusplus */

#endif /* ARM_H */
