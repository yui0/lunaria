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

/* Which caller the lock was held *for*.
 *
 * "held 100% of the last 5s" says the emulator is serialised but not by what,
 * and the two candidates — a long SVC handler and the scheduler pass — are
 * fixed in completely different places.  A hold is labelled by the thread that
 * takes it (0 = unlabelled) and the time is charged to that label when the
 * outermost release happens, so the report can name the handler rather than
 * the lock.  One thread-local store per acquire and one relaxed add per
 * release: cheap enough to leave on. */
#define ARM_LOCK_TAG_MAX 4160u
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
inline uint32_t           g_heap_size   = 0x10000000u; /* guest_layout_init */
// A64 fallback mmap arena: upper host-service heap, grows downward.
inline uint32_t           g_small_mmap_top = 0;
/* How far malloc has bumped; the small-mmap fallback stops there. */
inline uint32_t           g_heap_bump_top = 0;
// Mutable: A64 may relocate tramp/stack and grow primary to 1 GiB.
inline uint32_t THREAD_STACK_BASE = 0x48000000u;
constexpr uint32_t THREAD_STACK_SIZE = 0x00100000u;
constexpr uint32_t MMAP_BASE      = 0x10000000u;
inline uint32_t           MMAP_END       = 0x41000000u;
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
// A64: request image window at its guest VA before ArmMemory::init().
inline bool g_a64_identity_arena = false;
inline bool g_a64_arena_identity = false;

inline bool a64_is_guest_va(GuestVA va) {
    return va >= A64_GUEST_BASE && va < A64_GUEST_BASE + A64_GUEST_SIZE;
}
inline GuestVA a64_guest_va(BackingOffset backing) {
    return A64_GUEST_BASE + (GuestVA)backing;
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

/* Single-entry lookup cache, per thread: a shared global one is itself a race
 * (two readers write the pair non-atomically and a third can see a torn lo/hi
 * that spans an unmapped hole). */
inline thread_local GuestVA t_a64_cache_lo = 1, t_a64_cache_hi = 0;
inline thread_local uint64_t t_a64_cache_gen = 0;

// Caller holds g_a64_maps_mu (either way).
inline const A64Mapping *a64_find_locked(GuestVA va) {
    auto it = std::upper_bound(g_a64_maps.begin(), g_a64_maps.end(), va,
                               [](GuestVA v, const A64Mapping &m) { return v < m.lo; });
    if (it == g_a64_maps.begin()) return nullptr;
    --it;
    return (va >= it->lo && va < it->hi) ? &*it : nullptr;
}

inline bool a64_mapped(GuestVA va) {
    const uint64_t gen = g_a64_maps_gen.load(std::memory_order_acquire);
    if (gen == t_a64_cache_gen && va >= t_a64_cache_lo && va < t_a64_cache_hi)
        return true;
    std::shared_lock<std::shared_mutex> lk(g_a64_maps_mu);
    const A64Mapping *m = a64_find_locked(va);
    if (!m) return false;
    t_a64_cache_lo  = m->lo;
    t_a64_cache_hi  = m->hi;
    t_a64_cache_gen = gen;
    return true;
}

// Bytes mapped contiguously from `va`, or 0 when `va` itself is unmapped.
inline size_t a64_mapped_span(GuestVA va) {
    std::shared_lock<std::shared_mutex> lk(g_a64_maps_mu);
    const A64Mapping *m = a64_find_locked(va);
    return m ? (size_t)(m->hi - va) : 0u;
}

inline void a64_map_insert(GuestVA lo, GuestVA hi, uint32_t prot, bool owned) {
    std::unique_lock<std::shared_mutex> lk(g_a64_maps_mu);
    auto it = std::lower_bound(g_a64_maps.begin(), g_a64_maps.end(), lo,
                               [](const A64Mapping &m, GuestVA v) { return m.lo < v; });
    g_a64_maps.insert(it, A64Mapping{lo, hi, prot, owned});
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
inline GuestVA a64_va_map(GuestVA hint, uint64_t len, uint32_t prot) {
    len = (len + 4095ull) & ~4095ull;
    if (!len) return 0;
    void *want = nullptr;
    if (hint && !a64_is_guest_va(hint) && hint >= 0x10000ull &&
        !(hint & 4095ull) && hint + len <= 0x0010000000000000ull)
        want = (void *)hint;
    void *p = ::mmap(want, (size_t)len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (!a64_va_usable(p, len)) {
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
struct LoadedRegion { uint32_t lo, hi; uint32_t flags; std::string path; };
inline std::vector<LoadedRegion> g_loaded_regions;
// Process-wide ABI selector used by synthetic /proc files.

// Program-header metadata exposed through dl_iterate_phdr.
struct ModulePhdr {
    GuestVA load_bias;
    GuestVA name_va;
    GuestVA phdr_va;
    uint16_t phnum;
    bool is_64;
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
        // wherever malloc has bumped to: the two share the window.
        uint32_t floor = g_heap_bump_top ? g_heap_bump_top
                                         : HEAP_BASE + HEAP_SIZE / 2u;
        uint32_t next = (g_small_mmap_top - len) & ~(MMAP_ALIGN - 1u);
        if (next >= floor && next < g_small_mmap_top) {
            g_small_mmap_top = next;
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
inline uint32_t misc_tzname(void)     { return MISC_DATA + 0x100u; }
inline uint32_t misc_tzvars(void)     { return MISC_DATA + 0x180u; }
inline uint32_t misc_stdio(void)      { return MISC_DATA + 0x200u; }
// glGetString/eglQueryString ring (8 × 8 KiB) — see stash_gl_c_string().
inline uint32_t GL_STR_RING_BASE = 0x41020000u;
constexpr uint32_t GL_STR_RING_SLOTS = 8u;
constexpr uint32_t GL_STR_RING_SLOT  = 8192u; /* GL_EXTENSIONS can exceed 4 KiB */
constexpr uint32_t GL_STR_RING_SIZE  = GL_STR_RING_SLOTS * GL_STR_RING_SLOT;
// Per-thread guest TLS pages (tpidr_el0), one 4 KiB page per guest tid.
inline uint32_t TLS_WINDOW_BASE = 0x41030000u;
inline uint32_t TLS_WINDOW_END  = 0x41040000u;
inline uint32_t STACK_BASE    = 0x42000000u;
inline uint32_t STACK_SIZE    = 0x05000000u;  /* 80MB */
inline uint32_t SENTINEL_ADDR = 0x43000000u;
inline uint32_t CB_STACK_BASE = 0x47700000u;
/* 512 KiB is enough for the callbacks this emulator hosts (JNI bridges,
 * NDK choreographer, audio).  Halving the old 1 MiB window doubles the
 * number of concurrent stacks that fit in the same VA range. */
constexpr uint32_t CB_STACK_SIZE = 0x00080000u;
constexpr int      CB_MAX_DEPTH  = 2;            /* outer + one nest */
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
 * Exclusive-monitor ids are permanent per host thread (LL/SC identity) and
 * come from a larger pool, because they are cheap and the JIT that embeds
 * them is thread-local. */
constexpr int      CB_MAX_SLOTS  = 16;           /* concurrent stack windows */
constexpr int      CB_MAX_EXCL   = 64;           /* host threads that callback */
/* How many of those slots the guest address space actually has stack windows
 * for; the layout functions below set it from the room they have. */
inline int         g_cb_slots    = 4;
inline uint32_t cb_stack_base(int slot, int depth) {
    return CB_STACK_BASE +
           (uint32_t)(slot * CB_MAX_DEPTH + depth) * CB_STACK_SIZE;
}

// Exclusive-monitor processor ids.
enum : size_t {
    EXCL_ID_MAIN   = 0,
    EXCL_ID_AUX    = 1,
    /* + excl * CB_MAX_DEPTH + nest depth */
    EXCL_ID_A32_CB = 2,
    EXCL_ID_A64_CB = EXCL_ID_A32_CB + CB_MAX_DEPTH * CB_MAX_EXCL,
    /* One slot per A64 engine (LUNARIA_A64_ENGINES): engines can hold an LL/SC
     * reservation at the same time, so they may not share a slot. */
    EXCL_ID_A64_ENG = EXCL_ID_A64_CB + CB_MAX_DEPTH * CB_MAX_EXCL,
    A64_ENGINE_MAX  = 8,
    EXCL_ID_COUNT  = EXCL_ID_A64_ENG + A64_ENGINE_MAX,
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
    /* CB_MAX_SLOTS concurrent stacks x CB_MAX_DEPTH nest levels x 512 KiB
     * = 16 MiB, same footprint the old 8×1 MiB layout used. */
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


/* JNINativeInterface has 233 entries (4 reserved + 229 functions), and the
 * guest indexes it by the offsets in <jni.h>.  Lunaria used to build only 229
 * slots and dispatch slot n to SVC n, which silently dropped the four
 * "critical" accessors (GetPrimitiveArrayCritical, ReleasePrimitiveArrayCritical,
 * GetStringCritical, ReleaseStringCritical) and shifted everything after them
 * by four: a guest calling ExceptionCheck (228) landed on a stub that always
 * answered "no exception", GetPrimitiveArrayCritical returned a jobject where
 * a raw element pointer was due, and NewDirectByteBuffer (229) and its
 * companions fell off the end of the table entirely.
 *
 * libswappy walks exactly that path — loadClass, ExceptionCheck, then
 * InMemoryDexClassLoader over a direct ByteBuffer holding its own dex — so it
 * never saw the failure and ran on with a null SwappyDisplayManager class.
 * See jni_vtable_svc() for the slot→SVC mapping. */
constexpr uint32_t JNI_VTABLE_COUNT = 233u; /* indices 0–232 */
constexpr uint32_t SVC_JVM_GETENV   = 229u;
constexpr uint32_t SVC_JVM_ATTACH   = 230u;
constexpr uint32_t SVC_JVM_DESTROY  = 231u;
constexpr uint32_t SVC_LOG_PRINT    = 232u;
constexpr uint32_t SVC_LOG_WRITE    = 233u;
constexpr uint32_t SVC_DLOPEN       = 234u;
constexpr uint32_t SVC_DLSYM        = 235u;
constexpr uint32_t SVC_DLCLOSE      = 236u;

constexpr uint32_t SVC_MALLOC       = 237u;
constexpr uint32_t SVC_FREE         = 238u;
constexpr uint32_t SVC_CALLOC       = 239u;
constexpr uint32_t SVC_REALLOC      = 240u;
constexpr uint32_t SVC_MEMCPY       = 241u;
constexpr uint32_t SVC_MEMMOVE      = 242u;
constexpr uint32_t SVC_MEMSET       = 243u;
constexpr uint32_t SVC_STRLEN       = 244u;
constexpr uint32_t SVC_STRCPY       = 245u;
constexpr uint32_t SVC_STRNCPY      = 246u;
constexpr uint32_t SVC_STRCMP       = 247u;
constexpr uint32_t SVC_STRNCMP      = 248u;
constexpr uint32_t SVC_STRDUP       = 249u;
constexpr uint32_t SVC_STRNDUP      = 250u;
constexpr uint32_t SVC_STRCAT       = 251u;
constexpr uint32_t SVC_STRNCAT      = 252u;
constexpr uint32_t SVC_ABORT          = 253u;
constexpr uint32_t SVC_PTHREAD_KEY    = 254u; /* pthread_key_create/set/get/once/mutex/cond */
constexpr uint32_t SVC_PTHREAD_CREATE = 255u; /* pthread_create — queues fn for deferred run */

constexpr uint32_t SVC_ANW_FROM_SURFACE   = 256u;
constexpr uint32_t SVC_ANW_ACQUIRE        = 257u;
constexpr uint32_t SVC_ANW_RELEASE        = 258u;
constexpr uint32_t SVC_ANW_GETWIDTH       = 259u;
constexpr uint32_t SVC_ANW_GETHEIGHT      = 260u;
constexpr uint32_t SVC_ANW_SETBUFGEO      = 261u;
constexpr uint32_t SVC_ANW_TOSURFACE      = 262u;

constexpr uint32_t SVC_EGL_GETDISPLAY     = 263u;
constexpr uint32_t SVC_EGL_INITIALIZE     = 264u;
constexpr uint32_t SVC_EGL_CHOOSECONFIG   = 265u;
constexpr uint32_t SVC_EGL_CREATEWSURF    = 266u;
constexpr uint32_t SVC_EGL_CREATEPBUF     = 267u;
constexpr uint32_t SVC_EGL_CREATECTX      = 268u;
constexpr uint32_t SVC_EGL_MAKECURRENT    = 269u;
constexpr uint32_t SVC_EGL_SWAPBUF        = 270u;
constexpr uint32_t SVC_EGL_DESTROYSURF    = 271u;
constexpr uint32_t SVC_EGL_DESTROYCTX     = 272u;
constexpr uint32_t SVC_EGL_TERMINATE      = 273u;
constexpr uint32_t SVC_EGL_GETPROC        = 274u;
constexpr uint32_t SVC_EGL_QUERYSURF      = 275u;
constexpr uint32_t SVC_EGL_GETERROR       = 276u;
constexpr uint32_t SVC_EGL_GETCFGATTRIB   = 277u;
constexpr uint32_t SVC_EGL_QUERYSTR       = 278u;
constexpr uint32_t SVC_EGL_SURFACEATTRIB  = 279u;
constexpr uint32_t SVC_EGL_SWAPINTERVAL   = 280u;
constexpr uint32_t SVC_EGL_GETCURCTX      = 281u;
constexpr uint32_t SVC_EGL_GETCURSURF     = 282u;
constexpr uint32_t SVC_DL_UNWIND_EXIDX   = 283u;


constexpr uint32_t SVC_GL_BASE            = 284u;
constexpr uint32_t SVC_GL_Viewport              = 284u;
constexpr uint32_t SVC_GL_Clear                 = 285u;
constexpr uint32_t SVC_GL_ClearColor            = 286u;
constexpr uint32_t SVC_GL_ClearDepthf           = 287u;
constexpr uint32_t SVC_GL_ClearStencil          = 288u;
constexpr uint32_t SVC_GL_Enable                = 289u;
constexpr uint32_t SVC_GL_Disable               = 290u;
constexpr uint32_t SVC_GL_DepthFunc             = 291u;
constexpr uint32_t SVC_GL_DepthMask             = 292u;
constexpr uint32_t SVC_GL_ColorMask             = 293u;
constexpr uint32_t SVC_GL_Scissor               = 294u;
constexpr uint32_t SVC_GL_FrontFace             = 295u;
constexpr uint32_t SVC_GL_CullFace              = 296u;
constexpr uint32_t SVC_GL_BlendFuncSeparate     = 297u;
constexpr uint32_t SVC_GL_BlendEquationSeparate = 298u;
constexpr uint32_t SVC_GL_GetError              = 299u;
constexpr uint32_t SVC_GL_GetString             = 300u;
constexpr uint32_t SVC_GL_GetIntegerv           = 301u;
constexpr uint32_t SVC_GL_PixelStorei           = 302u;
constexpr uint32_t SVC_GL_ReadPixels            = 303u;
constexpr uint32_t SVC_GL_Flush                 = 304u;
constexpr uint32_t SVC_GL_Finish                = 305u;

constexpr uint32_t SVC_GL_GenBuffers            = 306u;
constexpr uint32_t SVC_GL_BindBuffer            = 307u;
constexpr uint32_t SVC_GL_BufferData            = 308u;
constexpr uint32_t SVC_GL_BufferSubData         = 309u;
constexpr uint32_t SVC_GL_DeleteBuffers         = 310u;

constexpr uint32_t SVC_GL_GenTextures           = 311u;
constexpr uint32_t SVC_GL_BindTexture           = 312u;
constexpr uint32_t SVC_GL_ActiveTexture         = 313u;
constexpr uint32_t SVC_GL_DeleteTextures        = 314u;
constexpr uint32_t SVC_GL_TexParameteri         = 315u;
constexpr uint32_t SVC_GL_TexImage2D            = 316u;
constexpr uint32_t SVC_GL_TexSubImage2D         = 317u;
constexpr uint32_t SVC_GL_CopyTexSubImage2D     = 318u;
constexpr uint32_t SVC_GL_CompressedTexImage2D  = 319u;
constexpr uint32_t SVC_GL_CompressedTexSubImage2D = 320u;
constexpr uint32_t SVC_GL_GenerateMipmap        = 321u;

constexpr uint32_t SVC_GL_GenFramebuffers       = 322u;
constexpr uint32_t SVC_GL_BindFramebuffer       = 323u;
constexpr uint32_t SVC_GL_DeleteFramebuffers    = 324u;
constexpr uint32_t SVC_GL_CheckFramebufferStatus = 325u;
constexpr uint32_t SVC_GL_FramebufferTexture2D  = 326u;
constexpr uint32_t SVC_GL_FramebufferRenderbuffer = 327u;
constexpr uint32_t SVC_GL_GetFramebufferAttachmentParameteriv = 328u;

constexpr uint32_t SVC_GL_GenRenderbuffers      = 329u;
constexpr uint32_t SVC_GL_BindRenderbuffer      = 330u;
constexpr uint32_t SVC_GL_DeleteRenderbuffers   = 331u;
constexpr uint32_t SVC_GL_RenderbufferStorage   = 332u;

constexpr uint32_t SVC_GL_CreateShader          = 333u;
constexpr uint32_t SVC_GL_ShaderSource          = 334u;
constexpr uint32_t SVC_GL_CompileShader         = 335u;
constexpr uint32_t SVC_GL_DeleteShader          = 336u;
constexpr uint32_t SVC_GL_GetShaderiv           = 337u;
constexpr uint32_t SVC_GL_GetShaderInfoLog      = 338u;
constexpr uint32_t SVC_GL_GetShaderSource       = 339u;

constexpr uint32_t SVC_GL_CreateProgram         = 340u;
constexpr uint32_t SVC_GL_AttachShader          = 341u;
constexpr uint32_t SVC_GL_LinkProgram           = 342u;
constexpr uint32_t SVC_GL_UseProgram            = 343u;
constexpr uint32_t SVC_GL_DeleteProgram         = 344u;
constexpr uint32_t SVC_GL_GetProgramiv          = 345u;
constexpr uint32_t SVC_GL_GetProgramInfoLog     = 346u;
constexpr uint32_t SVC_GL_GetAttribLocation     = 347u;
constexpr uint32_t SVC_GL_GetUniformLocation    = 348u;
constexpr uint32_t SVC_GL_GetActiveAttrib       = 349u;
constexpr uint32_t SVC_GL_GetActiveUniform      = 350u;
constexpr uint32_t SVC_GL_BindAttribLocation    = 351u;

constexpr uint32_t SVC_GL_Uniform1i             = 352u;
constexpr uint32_t SVC_GL_Uniform1iv            = 353u;
constexpr uint32_t SVC_GL_Uniform2iv            = 354u;
constexpr uint32_t SVC_GL_Uniform3iv            = 355u;
constexpr uint32_t SVC_GL_Uniform4iv            = 356u;
constexpr uint32_t SVC_GL_Uniform1fv            = 357u;
constexpr uint32_t SVC_GL_Uniform2fv            = 358u;
constexpr uint32_t SVC_GL_Uniform3fv            = 359u;
constexpr uint32_t SVC_GL_Uniform4fv            = 360u;
constexpr uint32_t SVC_GL_UniformMatrix3fv      = 361u;
constexpr uint32_t SVC_GL_UniformMatrix4fv      = 362u;

constexpr uint32_t SVC_GL_EnableVertexAttribArray  = 363u;
constexpr uint32_t SVC_GL_DisableVertexAttribArray = 364u;
constexpr uint32_t SVC_GL_VertexAttribPointer   = 365u;
constexpr uint32_t SVC_GL_GetVertexAttribiv     = 366u;
constexpr uint32_t SVC_GL_GetVertexAttribPointerv = 367u;

constexpr uint32_t SVC_GL_DrawArrays            = 368u;
constexpr uint32_t SVC_GL_DrawElements          = 369u;

constexpr uint32_t SVC_GL_StencilFunc           = 370u;
constexpr uint32_t SVC_GL_StencilFuncSeparate   = 371u;
constexpr uint32_t SVC_GL_StencilMask           = 372u;
constexpr uint32_t SVC_GL_StencilOp             = 373u;
constexpr uint32_t SVC_GL_StencilOpSeparate     = 374u;

constexpr uint32_t SVC_GL_BlendFunc             = 375u;
constexpr uint32_t SVC_GL_TexParameterf         = 376u;
constexpr uint32_t SVC_GL_DepthRangef           = 377u;
constexpr uint32_t SVC_GL_PolygonOffset         = 378u;
constexpr uint32_t SVC_GL_LineWidth             = 379u;
constexpr uint32_t SVC_GL_SampleCoverage        = 380u;

constexpr uint32_t SVC_GL_Uniform1f             = 381u;
constexpr uint32_t SVC_GL_Uniform2f             = 382u;
constexpr uint32_t SVC_GL_Uniform3f             = 383u;
constexpr uint32_t SVC_GL_Uniform4f             = 384u;

constexpr uint32_t SVC_GL_VertexAttrib1f        = 385u;
constexpr uint32_t SVC_GL_VertexAttrib2f        = 386u;
constexpr uint32_t SVC_GL_VertexAttrib3f        = 387u;
constexpr uint32_t SVC_GL_VertexAttrib4f        = 388u;
constexpr uint32_t SVC_GL_VertexAttrib4fv       = 389u;

constexpr uint32_t SVC_GL_GetFloatv             = 390u;
constexpr uint32_t SVC_GL_GetBooleanv           = 391u;
constexpr uint32_t SVC_GL_IsEnabled             = 392u;
constexpr uint32_t SVC_GL_IsProgram             = 393u;
constexpr uint32_t SVC_GL_IsShader              = 394u;
constexpr uint32_t SVC_GL_IsTexture             = 395u;
constexpr uint32_t SVC_GL_IsBuffer              = 396u;
constexpr uint32_t SVC_GL_IsFramebuffer         = 397u;
constexpr uint32_t SVC_GL_IsRenderbuffer        = 398u;

constexpr uint32_t SVC_GL_BlendEquation         = 399u;
constexpr uint32_t SVC_GL_BlendColor            = 400u;
constexpr uint32_t SVC_GL_ReleaseShaderCompiler = 401u;
constexpr uint32_t SVC_GL_GetShaderPrecisionFormat = 402u;
constexpr uint32_t SVC_GL_UniformMatrix2fv      = 403u;
constexpr uint32_t SVC_GL_VertexAttrib1fv       = 404u;
constexpr uint32_t SVC_GL_VertexAttrib2fv       = 405u;
constexpr uint32_t SVC_GL_VertexAttrib3fv       = 406u;

constexpr uint32_t SVC_GL_GetTexParameteriv    = 541u;

constexpr uint32_t SVC_AEABI_UIDIV       = 407u;
constexpr uint32_t SVC_AEABI_UIDIVMOD    = 408u;
constexpr uint32_t SVC_AEABI_IDIV        = 409u;
constexpr uint32_t SVC_AEABI_LDIVMOD     = 410u;
constexpr uint32_t SVC_AEABI_ULDIVMOD    = 411u; /* unsigned 64-bit division */
constexpr uint32_t SVC_LIBC_OPEN         = 412u;
constexpr uint32_t SVC_LIBC_CLOSE        = 413u;
constexpr uint32_t SVC_LIBC_READ         = 414u;
constexpr uint32_t SVC_LIBC_WRITE        = 415u;
constexpr uint32_t SVC_LIBC_LSEEK        = 416u;
constexpr uint32_t SVC_LIBC_FOPEN        = 417u;
constexpr uint32_t SVC_LIBC_FCLOSE       = 418u;
constexpr uint32_t SVC_LIBC_FREAD        = 419u;
constexpr uint32_t SVC_LIBC_FWRITE       = 420u;
constexpr uint32_t SVC_LIBC_FSEEK        = 421u;
constexpr uint32_t SVC_LIBC_FTELL        = 422u;
constexpr uint32_t SVC_LIBC_STAT         = 423u;
constexpr uint32_t SVC_LIBC_FSTAT        = 424u;
constexpr uint32_t SVC_LIBC_MMAP         = 425u;
constexpr uint32_t SVC_LIBC_MUNMAP       = 426u;


constexpr uint32_t SVC_CLOCK_GETTIME     = 427u;
constexpr uint32_t SVC_GETTIMEOFDAY      = 428u;
constexpr uint32_t SVC_TIME              = 429u;
constexpr uint32_t SVC_NANOSLEEP         = 430u;
constexpr uint32_t SVC_USLEEP            = 431u;
constexpr uint32_t SVC_GETENV            = 432u;
constexpr uint32_t SVC_GETPID            = 433u;
constexpr uint32_t SVC_GETTID            = 434u;
constexpr uint32_t SVC_SCHED_YIELD       = 435u;
constexpr uint32_t SVC_GETPAGESIZE       = 436u;
constexpr uint32_t SVC_SYSCONF           = 437u;
constexpr uint32_t SVC_RET0              = 438u; /* generic success stub */
constexpr uint32_t SVC_ERRNO_ADDR        = 439u; /* __errno */
constexpr uint32_t SVC_SYSPROP_GET       = 440u; /* __system_property_get */

constexpr uint32_t SVC_PTHREAD_SELF        = 441u;
constexpr uint32_t SVC_PTHREAD_KEY_CREATE  = 442u;
constexpr uint32_t SVC_PTHREAD_KEY_DELETE  = 443u;
constexpr uint32_t SVC_PTHREAD_SETSPECIFIC = 444u;
constexpr uint32_t SVC_PTHREAD_GETSPECIFIC = 445u;

constexpr uint32_t SVC_Z_INFLATEINIT2    = 446u;
constexpr uint32_t SVC_Z_INFLATE         = 447u;
constexpr uint32_t SVC_Z_INFLATEEND      = 448u;
constexpr uint32_t SVC_Z_INFLATERESET    = 449u;
constexpr uint32_t SVC_Z_CRC32           = 450u;
constexpr uint32_t SVC_Z_ADLER32         = 451u;

constexpr uint32_t SVC_Z_INFLATEINIT     = 452u; /* inflateInit_(z, ver, size) */
constexpr uint32_t SVC_Z_DEFLATEINIT2    = 453u; /* deflateInit2_(z,lvl,method,wbits,mem,strategy,ver,sz) */
constexpr uint32_t SVC_Z_DEFLATE         = 454u; /* deflate(z, flush) */
constexpr uint32_t SVC_Z_DEFLATEEND      = 455u; /* deflateEnd(z) */
constexpr uint32_t SVC_Z_DEFLATERESET    = 456u; /* deflateReset(z) */

constexpr uint32_t SVC_ATOI              = 457u;
constexpr uint32_t SVC_ATOL              = 458u;
constexpr uint32_t SVC_STRTOL            = 459u;
constexpr uint32_t SVC_STRTOUL           = 460u;
constexpr uint32_t SVC_STRTOD            = 461u;
constexpr uint32_t SVC_STRTOF            = 462u;


// Math passthrough block.
constexpr uint32_t SVC_MATH_F1_BASE      = 542u; /* float  fn(float) */
constexpr uint32_t SVC_MATH_F1_COUNT     = 27u;
constexpr uint32_t SVC_MATH_F2_BASE      = SVC_MATH_F1_BASE + SVC_MATH_F1_COUNT;       /* float  fn(float,float) */
constexpr uint32_t SVC_MATH_F2_COUNT     = 9u;
constexpr uint32_t SVC_MATH_D1_BASE      = SVC_MATH_F2_BASE + SVC_MATH_F2_COUNT;       /* double fn(double) */
constexpr uint32_t SVC_MATH_D1_COUNT     = 25u;
constexpr uint32_t SVC_MATH_D2_BASE      = SVC_MATH_D1_BASE + SVC_MATH_D1_COUNT;       /* double fn(double,double) */
constexpr uint32_t SVC_MATH_D2_COUNT     = 7u;

static_assert(SVC_MATH_F1_BASE > SVC_GL_GetTexParameteriv,
              "math SVC block overlaps GL (cosf would hit glGetTexParameteriv)");

// Extended libc passthrough.

constexpr uint32_t SVC_EXT_BASE        = SVC_MATH_D2_BASE + SVC_MATH_D2_COUNT;
constexpr uint32_t SVC_MEMALIGN        = SVC_EXT_BASE + 0u;
constexpr uint32_t SVC_POSIX_MEMALIGN  = SVC_EXT_BASE + 1u;
constexpr uint32_t SVC_MEMCMP          = SVC_EXT_BASE + 2u;
constexpr uint32_t SVC_MEMCHR          = SVC_EXT_BASE + 3u;
constexpr uint32_t SVC_MEMRCHR         = SVC_EXT_BASE + 4u;
constexpr uint32_t SVC_MEMMEM          = SVC_EXT_BASE + 5u;
constexpr uint32_t SVC_STRCHR          = SVC_EXT_BASE + 6u;
constexpr uint32_t SVC_STRRCHR         = SVC_EXT_BASE + 7u;
constexpr uint32_t SVC_STRSTR          = SVC_EXT_BASE + 8u;
constexpr uint32_t SVC_STRNLEN         = SVC_EXT_BASE + 9u;
constexpr uint32_t SVC_STRCASECMP      = SVC_EXT_BASE + 10u;
constexpr uint32_t SVC_STRCSPN         = SVC_EXT_BASE + 11u;
constexpr uint32_t SVC_STRSPN          = SVC_EXT_BASE + 12u;
constexpr uint32_t SVC_STRTOK_R        = SVC_EXT_BASE + 13u;
constexpr uint32_t SVC_PTHREAD_EQUAL   = SVC_EXT_BASE + 14u;
constexpr uint32_t SVC_SNPRINTF        = SVC_EXT_BASE + 15u;
constexpr uint32_t SVC_SPRINTF         = SVC_EXT_BASE + 16u;
constexpr uint32_t SVC_VSNPRINTF       = SVC_EXT_BASE + 17u;
constexpr uint32_t SVC_VASPRINTF       = SVC_EXT_BASE + 18u;
constexpr uint32_t SVC_PRINTF          = SVC_EXT_BASE + 19u;
constexpr uint32_t SVC_FPRINTF         = SVC_EXT_BASE + 20u;
constexpr uint32_t SVC_VPRINTF         = SVC_EXT_BASE + 21u;
constexpr uint32_t SVC_VFPRINTF        = SVC_EXT_BASE + 22u;
constexpr uint32_t SVC_PUTS            = SVC_EXT_BASE + 23u;
constexpr uint32_t SVC_FPUTS           = SVC_EXT_BASE + 24u;
constexpr uint32_t SVC_FPUTC           = SVC_EXT_BASE + 25u;
constexpr uint32_t SVC_ASSERT2         = SVC_EXT_BASE + 26u;
constexpr uint32_t SVC_LOG_VPRINT      = SVC_EXT_BASE + 27u;
constexpr uint32_t SVC_SINCOS          = SVC_EXT_BASE + 28u;
constexpr uint32_t SVC_SINCOSF         = SVC_EXT_BASE + 29u;
constexpr uint32_t SVC_LDEXP           = SVC_EXT_BASE + 30u;
constexpr uint32_t SVC_LDEXPF          = SVC_EXT_BASE + 31u;
constexpr uint32_t SVC_MODF            = SVC_EXT_BASE + 32u;
constexpr uint32_t SVC_MODFF           = SVC_EXT_BASE + 33u;
constexpr uint32_t SVC_STRTOLL         = SVC_EXT_BASE + 34u;
constexpr uint32_t SVC_STRTOULL        = SVC_EXT_BASE + 35u;
constexpr uint32_t SVC_ACCESS          = SVC_EXT_BASE + 36u;
constexpr uint32_t SVC_REALPATH        = SVC_EXT_BASE + 37u;
constexpr uint32_t SVC_PREAD           = SVC_EXT_BASE + 38u;
constexpr uint32_t SVC_PWRITE          = SVC_EXT_BASE + 39u;
constexpr uint32_t SVC_OPENDIR         = SVC_EXT_BASE + 40u;
constexpr uint32_t SVC_READDIR         = SVC_EXT_BASE + 41u;
constexpr uint32_t SVC_CLOSEDIR        = SVC_EXT_BASE + 42u;
constexpr uint32_t SVC_WCSLEN          = SVC_EXT_BASE + 43u;
constexpr uint32_t SVC_WMEMCPY         = SVC_EXT_BASE + 44u;
constexpr uint32_t SVC_WMEMMOVE        = SVC_EXT_BASE + 45u;
constexpr uint32_t SVC_WMEMSET         = SVC_EXT_BASE + 46u;
constexpr uint32_t SVC_ISSPACE         = SVC_EXT_BASE + 47u;
constexpr uint32_t SVC_FGETS           = SVC_EXT_BASE + 48u;
constexpr uint32_t SVC_FILENO          = SVC_EXT_BASE + 49u;
constexpr uint32_t SVC_FEOF            = SVC_EXT_BASE + 50u;
constexpr uint32_t SVC_BASENAME        = SVC_EXT_BASE + 51u;
constexpr uint32_t SVC_EXIT            = SVC_EXT_BASE + 52u;
// __aeabi_* / fortify (_chk) / additional libc stubs
constexpr uint32_t SVC_AEABI_MEMSET    = SVC_EXT_BASE + 53u; /* (dst, n, c) — note argument order */
constexpr uint32_t SVC_AEABI_MEMCLR    = SVC_EXT_BASE + 54u; /* (dst, n) */
constexpr uint32_t SVC_STRLCPY         = SVC_EXT_BASE + 55u;
constexpr uint32_t SVC_STRNCASECMP     = SVC_EXT_BASE + 56u;
constexpr uint32_t SVC_TOLOWER         = SVC_EXT_BASE + 57u;
constexpr uint32_t SVC_ISALPHA         = SVC_EXT_BASE + 58u;
constexpr uint32_t SVC_ISDIGIT         = SVC_EXT_BASE + 59u;
constexpr uint32_t SVC_ISALNUM         = SVC_EXT_BASE + 60u;
constexpr uint32_t SVC_ISXDIGIT        = SVC_EXT_BASE + 61u;
constexpr uint32_t SVC_MKDIR           = SVC_EXT_BASE + 62u;
constexpr uint32_t SVC_GETCWD          = SVC_EXT_BASE + 63u;
constexpr uint32_t SVC_UNLINK          = SVC_EXT_BASE + 64u;
constexpr uint32_t SVC_RENAME          = SVC_EXT_BASE + 65u;
constexpr uint32_t SVC_FTRUNCATE       = SVC_EXT_BASE + 66u;
constexpr uint32_t SVC_READLINK        = SVC_EXT_BASE + 67u;
constexpr uint32_t SVC_CLOCK           = SVC_EXT_BASE + 68u;
constexpr uint32_t SVC_LOCALTIME_R     = SVC_EXT_BASE + 69u;
constexpr uint32_t SVC_GMTIME_R        = SVC_EXT_BASE + 70u;
constexpr uint32_t SVC_LOCALTIME       = SVC_EXT_BASE + 71u;
constexpr uint32_t SVC_GMTIME          = SVC_EXT_BASE + 72u;
constexpr uint32_t SVC_MKTIME          = SVC_EXT_BASE + 73u;
constexpr uint32_t SVC_DIFFTIME        = SVC_EXT_BASE + 74u;
constexpr uint32_t SVC_STRFTIME        = SVC_EXT_BASE + 75u;
constexpr uint32_t SVC_UNAME           = SVC_EXT_BASE + 76u;
constexpr uint32_t SVC_GETRLIMIT       = SVC_EXT_BASE + 77u;
constexpr uint32_t SVC_MREMAP          = SVC_EXT_BASE + 78u;
constexpr uint32_t SVC_WRITEV          = SVC_EXT_BASE + 79u;
constexpr uint32_t SVC_STRERROR        = SVC_EXT_BASE + 80u;
constexpr uint32_t SVC_SETLOCALE       = SVC_EXT_BASE + 81u;
constexpr uint32_t SVC_RETM1           = SVC_EXT_BASE + 82u; /* stub returning -1 on failure */
constexpr uint32_t SVC_PTHREAD_GETATTR_NP      = SVC_EXT_BASE + 83u;
constexpr uint32_t SVC_PTHREAD_ATTR_GETSTACK   = SVC_EXT_BASE + 84u;
constexpr uint32_t SVC_PTHREAD_ATTR_GETSTACKSZ = SVC_EXT_BASE + 85u;
constexpr uint32_t SVC_VSNPRINTF_CHK   = SVC_EXT_BASE + 86u;
constexpr uint32_t SVC_VSPRINTF_CHK    = SVC_EXT_BASE + 87u;
constexpr uint32_t SVC_ABS             = SVC_EXT_BASE + 88u;
constexpr uint32_t SVC_SSCANF          = SVC_EXT_BASE + 89u;
constexpr uint32_t SVC_VSSCANF         = SVC_EXT_BASE + 90u;
constexpr uint32_t SVC_ISASCII         = SVC_EXT_BASE + 91u;
constexpr uint32_t SVC_LIBC_MMAP2      = SVC_EXT_BASE + 92u; /* __mmap2: offset is in pages */
constexpr uint32_t SVC_WAIT            = SVC_EXT_BASE + 93u; /* cond_wait/join: yield slice each call */
// Semaphores (real counters) — required for Boehm GC thread registration
constexpr uint32_t SVC_SEM_INIT        = SVC_EXT_BASE + 94u;
constexpr uint32_t SVC_SEM_POST        = SVC_EXT_BASE + 95u;
constexpr uint32_t SVC_SEM_WAIT        = SVC_EXT_BASE + 96u;
constexpr uint32_t SVC_SEM_TRYWAIT     = SVC_EXT_BASE + 97u;
constexpr uint32_t SVC_SEM_TIMEDWAIT   = SVC_EXT_BASE + 98u;
constexpr uint32_t SVC_SEM_DESTROY     = SVC_EXT_BASE + 99u;
constexpr uint32_t SVC_SEM_GETVALUE    = SVC_EXT_BASE + 100u;
// setjmp/longjmp: save/restore context in jmp_buf.
constexpr uint32_t SVC_SETJMP          = SVC_EXT_BASE + 101u;
constexpr uint32_t SVC_LONGJMP         = SVC_EXT_BASE + 102u;
// ARM Linux kuser helpers (called via BLX 0xffff0fa0 / 0xffff0fc0 / 0xffff0fe0)
constexpr uint32_t SVC_KUSER_CMPXCHG   = SVC_EXT_BASE + 103u;
constexpr uint32_t SVC_KUSER_GET_TLS   = SVC_EXT_BASE + 104u;
constexpr uint32_t SVC_SYSCALL         = SVC_EXT_BASE + 105u; /* syscall() shim (futex/gettid) */
constexpr uint32_t SVC_SBRK           = SVC_EXT_BASE + 106u;
constexpr uint32_t SVC_BRK            = SVC_EXT_BASE + 107u;

constexpr uint32_t SVC_ALOOPER_FORTHREAD = SVC_EXT_BASE + 108u;
constexpr uint32_t SVC_ALOOPER_PREPARE   = SVC_EXT_BASE + 109u;
constexpr uint32_t SVC_ALOOPER_POLLONCE  = SVC_EXT_BASE + 110u;
constexpr uint32_t SVC_ALOOPER_POLLALL   = SVC_EXT_BASE + 111u;
constexpr uint32_t SVC_ALOOPER_WAKE      = SVC_EXT_BASE + 112u;
constexpr uint32_t SVC_STATFS            = SVC_EXT_BASE + 113u;
constexpr uint32_t SVC_STATVFS           = SVC_EXT_BASE + 114u;
constexpr uint32_t SVC_CHDIR             = SVC_EXT_BASE + 115u;
// reliable exception-identification logging SVCs.
constexpr uint32_t SVC_EXC_FROM_NAME     = SVC_EXT_BASE + 116u;
constexpr uint32_t SVC_EXC_RAISE         = SVC_EXT_BASE + 117u;
// getdtablesize() — required by mono's io-layer _wapi_handle_init to size _wapi_fd_reserve.
constexpr uint32_t SVC_GETDTABLESIZE     = SVC_EXT_BASE + 118u;
// bsearch with a guest comparator callback.  libmono imports.
constexpr uint32_t SVC_BSEARCH           = SVC_EXT_BASE + 119u;
// inline-detour logging SVCs (LUNARIA_TRACE_EXC).
constexpr uint32_t SVC_DETOUR_BASE       = SVC_EXT_BASE + 120u;
constexpr uint32_t NUM_DETOURS           = 20u;

// Itanium/ARM C++ ABI one-time static-initialisation guards.
constexpr uint32_t SVC_CXA_GUARD_ACQUIRE = SVC_DETOUR_BASE + NUM_DETOURS + 0u;
constexpr uint32_t SVC_CXA_GUARD_RELEASE = SVC_DETOUR_BASE + NUM_DETOURS + 1u;
constexpr uint32_t SVC_CXA_GUARD_ABORT   = SVC_DETOUR_BASE + NUM_DETOURS + 2u;
constexpr uint32_t SVC_CXA_PURE_VIRTUAL  = SVC_DETOUR_BASE + NUM_DETOURS + 3u;
constexpr uint32_t SVC_AEABI_ATEXIT      = SVC_DETOUR_BASE + NUM_DETOURS + 4u;


constexpr uint32_t SVC_BTOWC             = SVC_DETOUR_BASE + NUM_DETOURS + 5u;
constexpr uint32_t SVC_WCTOB             = SVC_DETOUR_BASE + NUM_DETOURS + 6u;
constexpr uint32_t SVC_TOWLOWER          = SVC_DETOUR_BASE + NUM_DETOURS + 7u;
constexpr uint32_t SVC_TOWUPPER          = SVC_DETOUR_BASE + NUM_DETOURS + 8u;
constexpr uint32_t SVC_ISWCTYPE          = SVC_DETOUR_BASE + NUM_DETOURS + 9u;
constexpr uint32_t SVC_WCTYPE            = SVC_DETOUR_BASE + NUM_DETOURS + 10u;
constexpr uint32_t SVC_MBRTOWC           = SVC_DETOUR_BASE + NUM_DETOURS + 11u;
constexpr uint32_t SVC_WCRTOMB           = SVC_DETOUR_BASE + NUM_DETOURS + 12u;
constexpr uint32_t SVC_WMEMCHR           = SVC_DETOUR_BASE + NUM_DETOURS + 13u;
constexpr uint32_t SVC_STRCOLL           = SVC_DETOUR_BASE + NUM_DETOURS + 14u;
constexpr uint32_t SVC_STRXFRM           = SVC_DETOUR_BASE + NUM_DETOURS + 15u;
constexpr uint32_t SVC_STRCASESTR        = SVC_DETOUR_BASE + NUM_DETOURS + 16u;
constexpr uint32_t SVC_STRSEP            = SVC_DETOUR_BASE + NUM_DETOURS + 17u;

// fp classification, fenv, wide-char classification/conversion
constexpr uint32_t SVC_ISNAN             = SVC_DETOUR_BASE + NUM_DETOURS + 18u;
constexpr uint32_t SVC_ISINF             = SVC_DETOUR_BASE + NUM_DETOURS + 19u;
constexpr uint32_t SVC_ISFINITE          = SVC_DETOUR_BASE + NUM_DETOURS + 20u;
constexpr uint32_t SVC_SIGNBIT           = SVC_DETOUR_BASE + NUM_DETOURS + 21u;
constexpr uint32_t SVC_FEGETROUND        = SVC_DETOUR_BASE + NUM_DETOURS + 22u;
constexpr uint32_t SVC_FESETROUND        = SVC_DETOUR_BASE + NUM_DETOURS + 23u;
constexpr uint32_t SVC_FECLEAREXCEPT     = SVC_DETOUR_BASE + NUM_DETOURS + 24u;
constexpr uint32_t SVC_FERAISEEXCEPT     = SVC_DETOUR_BASE + NUM_DETOURS + 25u;
constexpr uint32_t SVC_FETESTEXCEPT      = SVC_DETOUR_BASE + NUM_DETOURS + 26u;
constexpr uint32_t SVC_ISWSPACE          = SVC_DETOUR_BASE + NUM_DETOURS + 27u;
constexpr uint32_t SVC_ISWDIGIT          = SVC_DETOUR_BASE + NUM_DETOURS + 28u;
constexpr uint32_t SVC_ISWALPHA          = SVC_DETOUR_BASE + NUM_DETOURS + 29u;
constexpr uint32_t SVC_ISWUPPER          = SVC_DETOUR_BASE + NUM_DETOURS + 30u;
constexpr uint32_t SVC_ISWLOWER          = SVC_DETOUR_BASE + NUM_DETOURS + 31u;
constexpr uint32_t SVC_ISWPRINT          = SVC_DETOUR_BASE + NUM_DETOURS + 32u;
constexpr uint32_t SVC_ISWPUNCT          = SVC_DETOUR_BASE + NUM_DETOURS + 33u;
constexpr uint32_t SVC_ISWGRAPH          = SVC_DETOUR_BASE + NUM_DETOURS + 34u;
constexpr uint32_t SVC_ISWALNUM          = SVC_DETOUR_BASE + NUM_DETOURS + 35u;
constexpr uint32_t SVC_ISWBLANK          = SVC_DETOUR_BASE + NUM_DETOURS + 36u;
constexpr uint32_t SVC_ISWCNTRL          = SVC_DETOUR_BASE + NUM_DETOURS + 37u;
constexpr uint32_t SVC_WCTRANS           = SVC_DETOUR_BASE + NUM_DETOURS + 38u;
constexpr uint32_t SVC_TOWCTRANS         = SVC_DETOUR_BASE + NUM_DETOURS + 39u;
constexpr uint32_t SVC_STRTOLD           = SVC_DETOUR_BASE + NUM_DETOURS + 40u;
constexpr uint32_t SVC_WCSTOD            = SVC_DETOUR_BASE + NUM_DETOURS + 41u;
constexpr uint32_t SVC_WCSTOL            = SVC_DETOUR_BASE + NUM_DETOURS + 42u;
constexpr uint32_t SVC_WCSTOUL           = SVC_DETOUR_BASE + NUM_DETOURS + 43u;
constexpr uint32_t SVC_WCSTOLL           = SVC_DETOUR_BASE + NUM_DETOURS + 44u;
constexpr uint32_t SVC_WCSTOULL          = SVC_DETOUR_BASE + NUM_DETOURS + 45u;
constexpr uint32_t SVC_STRTOIMAX         = SVC_DETOUR_BASE + NUM_DETOURS + 46u;
constexpr uint32_t SVC_STRTOUMAX         = SVC_DETOUR_BASE + NUM_DETOURS + 47u;
// SVC_WCSLEN already defined at SVC_EXT_BASE+43 — skip duplicate
constexpr uint32_t SVC_WCSNCMP           = SVC_DETOUR_BASE + NUM_DETOURS + 49u;
constexpr uint32_t SVC_WCSCMP            = SVC_DETOUR_BASE + NUM_DETOURS + 50u;
constexpr uint32_t SVC_WCSCPY            = SVC_DETOUR_BASE + NUM_DETOURS + 51u;
constexpr uint32_t SVC_WCSCAT            = SVC_DETOUR_BASE + NUM_DETOURS + 52u;
constexpr uint32_t SVC_DIV               = SVC_DETOUR_BASE + NUM_DETOURS + 53u;
constexpr uint32_t SVC_LDIV              = SVC_DETOUR_BASE + NUM_DETOURS + 54u;
// Unresolved-symbol stubs (instead of tramp(0)) for distinct bind vs runtime logs
constexpr uint32_t SVC_UNKNOWN_CALL      = SVC_DETOUR_BASE + NUM_DETOURS + 55u;

constexpr uint32_t SVC_FREXP             = SVC_DETOUR_BASE + NUM_DETOURS + 56u;
constexpr uint32_t SVC_RINT              = SVC_DETOUR_BASE + NUM_DETOURS + 57u;
constexpr uint32_t SVC_LRAND48           = SVC_DETOUR_BASE + NUM_DETOURS + 58u;
constexpr uint32_t SVC_SRAND48           = SVC_DETOUR_BASE + NUM_DETOURS + 59u;
constexpr uint32_t SVC_STRPBRK           = SVC_DETOUR_BASE + NUM_DETOURS + 60u;
constexpr uint32_t SVC_STRTOK            = SVC_DETOUR_BASE + NUM_DETOURS + 61u;
constexpr uint32_t SVC_DUP2              = SVC_DETOUR_BASE + NUM_DETOURS + 62u;
constexpr uint32_t SVC_CLOCK_GETRES      = SVC_DETOUR_BASE + NUM_DETOURS + 63u;
constexpr uint32_t SVC_GETHOSTNAME       = SVC_DETOUR_BASE + NUM_DETOURS + 64u;
constexpr uint32_t SVC_GETRUSAGE         = SVC_DETOUR_BASE + NUM_DETOURS + 65u;
constexpr uint32_t SVC_VSPRINTF2         = SVC_DETOUR_BASE + NUM_DETOURS + 66u;
constexpr uint32_t SVC_GETC              = SVC_DETOUR_BASE + NUM_DETOURS + 67u;
constexpr uint32_t SVC_PUTCHAR           = SVC_DETOUR_BASE + NUM_DETOURS + 68u;
constexpr uint32_t SVC_FPCLASSIFYF       = SVC_DETOUR_BASE + NUM_DETOURS + 69u;
constexpr uint32_t SVC_INET_ADDR         = SVC_DETOUR_BASE + NUM_DETOURS + 70u;
constexpr uint32_t SVC_FCNTL2            = SVC_DETOUR_BASE + NUM_DETOURS + 71u;
constexpr uint32_t SVC_MONO_PATH_NORM    = SVC_DETOUR_BASE + NUM_DETOURS + 72u;
constexpr uint32_t SVC_G_FILENAME_URI    = SVC_DETOUR_BASE + NUM_DETOURS + 73u;
constexpr uint32_t SVC_MONO_FILE_MAP_OPEN = SVC_DETOUR_BASE + NUM_DETOURS + 102u;
constexpr uint32_t SVC_MONO_FILE_MAP_SIZE = SVC_DETOUR_BASE + NUM_DETOURS + 103u;
constexpr uint32_t SVC_MONO_FILE_MAP_FD   = SVC_DETOUR_BASE + NUM_DETOURS + 104u;
constexpr uint32_t SVC_MONO_FILE_MAP      = SVC_DETOUR_BASE + NUM_DETOURS + 105u;
constexpr uint32_t SVC_G_FILENAME_FROM_URI = SVC_DETOUR_BASE + NUM_DETOURS + 106u;
constexpr uint32_t SVC_MONO_FILE_MAP_CLOSE = SVC_DETOUR_BASE + NUM_DETOURS + 107u;
constexpr uint32_t SVC_KUSER_DMB           = SVC_DETOUR_BASE + NUM_DETOURS + 108u; /* 0xffff0fa0 */
// Wrap mono_add_internal_call to probe Time/Transform icalls (LUNARIA_TRACE_ICALL).
constexpr uint32_t SVC_MONO_ADD_ICALL     = SVC_DETOUR_BASE + NUM_DETOURS + 109u;
// Host AES-ECB for FAES::DecryptData — UE pak indexes in this title need it.
/* Host implementations of UE's own functions, reached by an inline detour that
 * overwrites the first instruction with `svc #N` — not by a symbol binding, so
 * these numbers never appear in kSymbolSvcMap and nothing in the build checks
 * them against it.  They used to be carved out of the same run as the SVC29
 * bank and collided with it exactly: FAES::DecryptData shared a number with
 * mbrlen, FSHA1::HashBuffer with mbsrtowcs, CityHash64 with logb,
 * DES_ncbc_encrypt with lrintf, and so on down the block.  The hook is tested
 * with an `if` before dispatch_svc's switch and returns, so a guest calling
 * mbrlen ran AES-256 over its own string buffer and got the pointer back as
 * the answer.  Give them a range of their own, above everything else, and
 * keep SVC_TRAMP_TOTAL derived from its end. */
constexpr uint32_t SVC_UE_HOOK_BASE = 1400u;
constexpr uint32_t SVC_FAES_DECRYPT = SVC_UE_HOOK_BASE + 0u;
// Host SHA-1 for FSHA1::HashBuffer — startup profiler showed 27% of load time.
constexpr uint32_t SVC_FSHA1_HASHBUFFER = SVC_UE_HOOK_BASE + 1u;
// Host CityHash64 — FName interning showed 11% of load time.
constexpr uint32_t SVC_CITYHASH64 = SVC_UE_HOOK_BASE + 2u;
/* Host OpenSSL DES-CBC.  This title decrypts its content with single DES and
 * the guest's own OpenSSL was 78% of every instruction the emulator executed
 * — 39.7 billion of them in 140 s, one thread, no SVCs, all of it inside
 * DES_ncbc_encrypt.  Same trade as FAES/FSHA1/CityHash above. */
constexpr uint32_t SVC_DES_NCBC = SVC_UE_HOOK_BASE + 3u;
constexpr uint32_t SVC_DES_EDE3_CBC = SVC_UE_HOOK_BASE + 4u;
/* Host FGenericPlatformStricmp::Stricmp.  UE compares FNames and paths with
 * it a character at a time; it was 2.7% of every guest instruction on the load
 * screen.  One SVC for all the width combinations — which one a call is comes
 * from the address the SVC was taken at. */
constexpr uint32_t SVC_UE_STRICMP = SVC_UE_HOOK_BASE + 5u;
/* Host FGenericPlatformStricmp::Strnicmp — the same function with a count.
 * UE reaches for it wherever it compares a prefix, and mounting this title's
 * patch paks (570k filenames, each turned into a package name) spends 10% of
 * every guest instruction in it. */
constexpr uint32_t SVC_UE_STRNICMP = SVC_UE_HOOK_BASE + 6u;
/* Host FString::ReplaceInline.  Mounting this title's patch paks turns every
 * one of 570k pak entries into a package name, and each conversion normalises
 * the filename — which is a ReplaceInline of "\\" by "/".  That is 12% of
 * every guest instruction on the load screen, and it is the one shape of the
 * function that needs no allocation at all: search and replacement are the
 * same length, so the characters are overwritten in place.  The handler takes
 * only that shape and hands every other call back to the guest's own code
 * through a resume stub, so the growing path keeps its own semantics. */
constexpr uint32_t SVC_UE_REPLACE_INLINE = SVC_UE_HOOK_BASE + 7u;
/* Host TStringViewImpl<T>::FindChar.  A one-character scan over a path, 9.5%
 * of the load screen: the loop is four instructions, so the guest pays for
 * fetch and decode rather than for the comparison.  Whole function, no
 * fallback — there is nothing in it to fall back to. */
constexpr uint32_t SVC_UE_FINDCHAR = SVC_UE_HOOK_BASE + 8u;
constexpr uint32_t SVC_UE_HOOK_LAST = SVC_UE_FINDCHAR;


constexpr uint32_t SVC_HONEST_BASE          = 1354u; /* first free id */
/* Entry points that used to be bound to the generic "returns 0" / "returns
 * -1" templates and turned out to be called for real.  A template answer is a
 * guess about what the caller wanted; these are the answers the caller can
 * actually act on. */
constexpr uint32_t SVC_SCHED_SETAFFINITY       = SVC_HONEST_BASE + 0u;
constexpr uint32_t SVC_CXA_ATEXIT              = SVC_HONEST_BASE + 1u;
constexpr uint32_t SVC_CXA_FINALIZE            = SVC_HONEST_BASE + 2u;
constexpr uint32_t SVC_ATEXIT                  = SVC_HONEST_BASE + 3u;
constexpr uint32_t SVC_SETRLIMIT               = SVC_HONEST_BASE + 4u;
constexpr uint32_t SVC_CHMOD                   = SVC_HONEST_BASE + 5u;
constexpr uint32_t SVC_FCHMOD                  = SVC_HONEST_BASE + 6u;
constexpr uint32_t SVC_SYSTEM                  = SVC_HONEST_BASE + 7u;
constexpr uint32_t SVC_FORK                    = SVC_HONEST_BASE + 8u;
constexpr uint32_t SVC_ANA_SET_WINDOW_FORMAT   = SVC_HONEST_BASE + 9u;
constexpr uint32_t SVC_TRUNCATE                = SVC_HONEST_BASE + 10u;
constexpr uint32_t SVC_SYMLINK                 = SVC_HONEST_BASE + 11u;
constexpr uint32_t SVC_LINK                    = SVC_HONEST_BASE + 12u;
constexpr uint32_t SVC_FDATASYNC               = SVC_HONEST_BASE + 13u;
constexpr uint32_t SVC_UTIMENSAT               = SVC_HONEST_BASE + 14u;
constexpr uint32_t SVC_FCHMODAT                = SVC_HONEST_BASE + 15u;
constexpr uint32_t SVC_FNMATCH                 = SVC_HONEST_BASE + 16u;
constexpr uint32_t SVC_LLDIV                   = SVC_HONEST_BASE + 17u;
constexpr uint32_t SVC_PATHCONF                = SVC_HONEST_BASE + 18u;
constexpr uint32_t SVC_GETNAMEINFO             = SVC_HONEST_BASE + 19u;
constexpr uint32_t SVC_SETVBUF                 = SVC_HONEST_BASE + 20u;
constexpr uint32_t SVC_ANW_GETFORMAT           = SVC_HONEST_BASE + 21u;
constexpr uint32_t SVC_ALOOPER_ACQUIRE         = SVC_HONEST_BASE + 22u;
constexpr uint32_t SVC_ALOOPER_RELEASE         = SVC_HONEST_BASE + 23u;
constexpr uint32_t SVC_PTHREAD_ATFORK          = SVC_HONEST_BASE + 24u;
constexpr uint32_t SVC_MLOCK                   = SVC_HONEST_BASE + 25u;
constexpr uint32_t SVC_MUNLOCK                 = SVC_HONEST_BASE + 26u;
constexpr uint32_t SVC_GETPWUID_R              = SVC_HONEST_BASE + 27u;
constexpr uint32_t SVC_CXA_THREAD_ATEXIT       = SVC_HONEST_BASE + 28u;
/* pthread_condattr_setclock/getclock: a condvar may be created on
 * CLOCK_MONOTONIC, and its timedwait deadlines are then on that clock. */
constexpr uint32_t SVC_PTHREAD_CONDATTR_SETCLOCK = SVC_HONEST_BASE + 29u;
constexpr uint32_t SVC_PTHREAD_CONDATTR_GETCLOCK = SVC_HONEST_BASE + 30u;
/* pthread_mutexattr_settype/gettype: NORMAL, RECURSIVE and ERRORCHECK are
 * three different contracts and a mutex has to know which one it was made
 * with. */
constexpr uint32_t SVC_PTHREAD_MUTEXATTR_SETTYPE = SVC_HONEST_BASE + 31u;
constexpr uint32_t SVC_PTHREAD_MUTEXATTR_GETTYPE = SVC_HONEST_BASE + 32u;
/* pthread_attr_t is bionic's plain struct in guest memory, so the setters
 * write the same fields the getters above already read. */
constexpr uint32_t SVC_PTHREAD_ATTR_INIT         = SVC_HONEST_BASE + 33u;
constexpr uint32_t SVC_PTHREAD_ATTR_SETSTACKSZ   = SVC_HONEST_BASE + 34u;
constexpr uint32_t SVC_PTHREAD_ATTR_SETDETACH    = SVC_HONEST_BASE + 35u;
constexpr uint32_t SVC_PTHREAD_ATTR_GETDETACH    = SVC_HONEST_BASE + 36u;
/* __pthread_cleanup_push/pop: the handler stack a thread unwinds through when
 * it is cancelled or exits. */
constexpr uint32_t SVC_PTHREAD_CLEANUP_PUSH      = SVC_HONEST_BASE + 37u;
constexpr uint32_t SVC_PTHREAD_CLEANUP_POP       = SVC_HONEST_BASE + 38u;
/* Destroying an attribute has to leave it *invalid*, not untouched. */
constexpr uint32_t SVC_PTHREAD_MUTEXATTR_DESTROY = SVC_HONEST_BASE + 39u;
/* The last number in the block above.  SVC_TRAMP_TOTAL is derived from this
 * rather than from whichever SVC happened to be written last: a number past
 * that bound gets no trampoline built, and the unknown-symbol pool — which
 * starts at the bound — hands its address out to a dlsym'd name instead, so
 * two unrelated symbols end up sharing one stub.  Adding to the block above
 * means moving this line down with it. */
constexpr uint32_t SVC_HONEST_LAST             = SVC_CXA_THREAD_ATEXIT;
static_assert(SVC_HONEST_LAST < SVC_UE_HOOK_BASE,
              "the honest block has grown into the UE hook block");
constexpr uint32_t NUM_ICALL_PROBES        = 16u;
constexpr uint32_t SVC_ICALL_PROBE_BASE    = SVC_DETOUR_BASE + NUM_DETOURS + 110u;
constexpr uint32_t ICALL_PROBE_STUB_BASE   = 0x4100e800u;
inline uint32_t g_icall_probe_next = ICALL_PROBE_STUB_BASE;
inline uint32_t g_icall_probe_count = 0;
inline const char *g_icall_probe_names[NUM_ICALL_PROBES] = {};
inline uint8_t g_icall_probe_kind[NUM_ICALL_PROBES] = {};
inline uint32_t g_mono_add_icall_real = 0;
// Real INTERNAL_set_localRotation — optional workaround (LUNARIA_FIX_EULER=1).
inline uint32_t g_set_local_rotation_fn = 0;
inline uint32_t g_set_local_euler_fn = 0; /* real INTERNAL_set_localEulerAngles */
constexpr uint32_t ICALL_QUAT_SCRATCH = 0x4100e7c0u; /* 4 floats */

constexpr uint32_t SVC_LIBC_LSEEK64      = SVC_DETOUR_BASE + NUM_DETOURS + 74u; /* lseek64(fd, r1:r2, whence_r3) */

constexpr uint32_t SVC_PTHREAD_MUTEX_INIT    = SVC_DETOUR_BASE + NUM_DETOURS + 75u;
constexpr uint32_t SVC_PTHREAD_MUTEX_LOCK    = SVC_DETOUR_BASE + NUM_DETOURS + 76u;
constexpr uint32_t SVC_PTHREAD_MUTEX_TRYLOCK = SVC_DETOUR_BASE + NUM_DETOURS + 77u;
constexpr uint32_t SVC_PTHREAD_MUTEX_UNLOCK  = SVC_DETOUR_BASE + NUM_DETOURS + 78u;
constexpr uint32_t SVC_PTHREAD_MUTEX_DESTROY = SVC_DETOUR_BASE + NUM_DETOURS + 79u;
constexpr uint32_t SVC_PTHREAD_COND_INIT     = SVC_DETOUR_BASE + NUM_DETOURS + 80u;
constexpr uint32_t SVC_PTHREAD_COND_DESTROY  = SVC_DETOUR_BASE + NUM_DETOURS + 81u;
constexpr uint32_t SVC_PTHREAD_COND_SIGNAL   = SVC_DETOUR_BASE + NUM_DETOURS + 82u;
constexpr uint32_t SVC_PTHREAD_COND_BROADCAST= SVC_DETOUR_BASE + NUM_DETOURS + 83u;
constexpr uint32_t SVC_PTHREAD_EXIT          = SVC_DETOUR_BASE + NUM_DETOURS + 84u;
constexpr uint32_t SVC_PTHREAD_ATTR_NOOP     = SVC_DETOUR_BASE + NUM_DETOURS + 85u; /* init/destroy/setstacksize etc */
constexpr uint32_t SVC_PTHREAD_ATTR_GETGUARD = SVC_DETOUR_BASE + NUM_DETOURS + 48u; /* getguardsize */
constexpr uint32_t SVC_PTHREAD_MUTEXATTR_NOOP= SVC_DETOUR_BASE + NUM_DETOURS + 86u; /* mutexattr_init/settype/destroy */
constexpr uint32_t SVC_PTHREAD_CONDATTR_NOOP = SVC_DETOUR_BASE + NUM_DETOURS + 87u; /* condattr_init/setclock/destroy */
// pthread_rwlock: no real blocking under cooperative scheduling; track state for EBUSY
constexpr uint32_t SVC_PTHREAD_RWLOCK_INIT     = SVC_DETOUR_BASE + NUM_DETOURS + 88u;
constexpr uint32_t SVC_PTHREAD_RWLOCK_RDLOCK   = SVC_DETOUR_BASE + NUM_DETOURS + 89u;
constexpr uint32_t SVC_PTHREAD_RWLOCK_WRLOCK   = SVC_DETOUR_BASE + NUM_DETOURS + 90u;
constexpr uint32_t SVC_PTHREAD_RWLOCK_UNLOCK   = SVC_DETOUR_BASE + NUM_DETOURS + 91u;
constexpr uint32_t SVC_PTHREAD_RWLOCK_DESTROY  = SVC_DETOUR_BASE + NUM_DETOURS + 92u;
constexpr uint32_t SVC_PTHREAD_RWLOCK_TRYRDLOCK= SVC_DETOUR_BASE + NUM_DETOURS + 93u;
constexpr uint32_t SVC_PTHREAD_RWLOCK_TRYWRLOCK= SVC_DETOUR_BASE + NUM_DETOURS + 94u;
// pthread_join: wait for target thread finished flag
constexpr uint32_t SVC_PTHREAD_JOIN            = SVC_DETOUR_BASE + NUM_DETOURS + 95u;
// pthread_detach: mark thread detached
constexpr uint32_t SVC_PTHREAD_DETACH          = SVC_DETOUR_BASE + NUM_DETOURS + 96u;
// pthread_cond_wait/timedwait: check cond and schedule
constexpr uint32_t SVC_PTHREAD_COND_WAIT       = SVC_DETOUR_BASE + NUM_DETOURS + 97u;
constexpr uint32_t SVC_PTHREAD_COND_TIMEDWAIT  = SVC_DETOUR_BASE + NUM_DETOURS + 98u;
// qsort: invoke guest comparator via call_guest_cb
constexpr uint32_t SVC_QSORT                   = SVC_DETOUR_BASE + NUM_DETOURS + 99u;
// fdopen: register fd in g_file_tab, return guest shim
constexpr uint32_t SVC_FDOPEN                  = SVC_DETOUR_BASE + NUM_DETOURS + 100u;
// strerror_r: write error string into buffer
constexpr uint32_t SVC_STRERROR_R              = SVC_DETOUR_BASE + NUM_DETOURS + 101u;
// pread64/pwrite64: LP32 bionic passes the 64-bit offset as an 8-byte-aligned value.
constexpr uint32_t SVC_PREAD64                = SVC_ICALL_PROBE_BASE + NUM_ICALL_PROBES + 0u;
constexpr uint32_t SVC_PWRITE64               = SVC_ICALL_PROBE_BASE + NUM_ICALL_PROBES + 1u;
/* fstatfs/fstatvfs take a descriptor, not a path: they cannot share the SVC
 * with their path-taking siblings once the answer depends on which filesystem
 * was named. */
constexpr uint32_t SVC_FSTATFS               = SVC_ICALL_PROBE_BASE + NUM_ICALL_PROBES + 2u;
constexpr uint32_t SVC_FSTATVFS              = SVC_ICALL_PROBE_BASE + NUM_ICALL_PROBES + 3u;
constexpr uint32_t SVC_TOTAL              = SVC_ICALL_PROBE_BASE + NUM_ICALL_PROBES + 4u;


constexpr uint32_t SVC29_BASE             = SVC_TOTAL;

constexpr uint32_t SVC_FMA                = SVC29_BASE + 0u;  /* fma(double,double,double) */
constexpr uint32_t SVC_FMAF               = SVC29_BASE + 1u;  /* fmaf(float,float,float) */
constexpr uint32_t SVC_SCALBN             = SVC29_BASE + 2u;  /* scalbn(double,int) */
constexpr uint32_t SVC_SCALBNF            = SVC29_BASE + 3u;  /* scalbnf(float,int) */
constexpr uint32_t SVC_ILOGB              = SVC29_BASE + 4u;  /* ilogb(double)->int */
constexpr uint32_t SVC_ILOGBF             = SVC29_BASE + 5u;  /* ilogbf(float)->int */

constexpr uint32_t SVC_DUP                = SVC29_BASE + 6u;  /* dup(fd) */
constexpr uint32_t SVC_FERROR             = SVC29_BASE + 7u;  /* ferror(FILE*) */
constexpr uint32_t SVC_REWINDDIR          = SVC29_BASE + 8u;  /* rewinddir(DIR*) */
constexpr uint32_t SVC_MBTOWC             = SVC29_BASE + 9u;  /* mbtowc(pwc,s,n) */
constexpr uint32_t SVC_MBRLEN             = SVC29_BASE + 10u; /* mbrlen(s,n,ps) */
constexpr uint32_t SVC_MBSRTOWCS          = SVC29_BASE + 11u; /* mbsrtowcs(dst,src,n,ps) */

constexpr uint32_t SVC_LOGB               = SVC29_BASE + 12u; /* logb(double)->double */
constexpr uint32_t SVC_LRINTF             = SVC29_BASE + 13u; /* lrintf(float)->int */
constexpr uint32_t SVC_EXPM1F             = SVC29_BASE + 14u; /* expm1f(float)->float */
constexpr uint32_t SVC_NANF               = SVC29_BASE + 15u; /* nanf(const char*)->float */
constexpr uint32_t SVC_WMEMCMP            = SVC29_BASE + 16u; /* wmemcmp(s1,s2,n)->int */
constexpr uint32_t SVC_SWPRINTF           = SVC29_BASE + 17u; /* swprintf(buf,n,fmt,...)->int */
constexpr uint32_t SVC_LOCALECONV         = SVC29_BASE + 18u; /* localeconv()->struct lconv* */
constexpr uint32_t SVC_SOCKETPAIR         = SVC29_BASE + 19u; /* socketpair(dom,type,prot,sv) */
/* iswxdigit accepts a-f/A-F as well as 0-9; iswdigit does not, so the two are
 * not interchangeable — see the symbol table entry for why that mattered. */
constexpr uint32_t SVC_ACFG_SDKVER        = SVC29_BASE + 20u; /* AConfiguration_getSdkVersion */
constexpr uint32_t SVC_ACHOREOGRAPHER_GET = SVC29_BASE + 21u; /* AChoreographer_getInstance */
constexpr uint32_t SVC_AASSETMGR_FROMJAVA = SVC29_BASE + 22u; /* AAssetManager_fromJava */
constexpr uint32_t SVC_AASSETMGR_OPEN     = SVC29_BASE + 23u; /* AAssetManager_open */
constexpr uint32_t SVC_AASSET_GETBUFFER   = SVC29_BASE + 24u; /* AAsset_getBuffer */
constexpr uint32_t SVC_AASSET_GETLENGTH   = SVC29_BASE + 25u; /* AAsset_getLength */
constexpr uint32_t SVC_ACHOREOGRAPHER_POST      = SVC29_BASE + 26u; /* postFrameCallback */
constexpr uint32_t SVC_ACHOREOGRAPHER_POST64    = SVC29_BASE + 27u; /* postFrameCallback64 */
constexpr uint32_t SVC_ACHOREOGRAPHER_POSTDELAY = SVC29_BASE + 28u; /* postFrameCallbackDelayed */
constexpr uint32_t SVC_MONO_PREP            = SVC29_BASE + 29u; /* mono config before jit init */
constexpr uint32_t SVC_ANW_LOCK             = SVC29_BASE + 30u;
constexpr uint32_t SVC_ANW_UNLOCK           = SVC29_BASE + 31u;
constexpr uint32_t SVC_AEABI_IDIV0          = SVC29_BASE + 32u;
constexpr uint32_t SVC_AEABI_LDIV0          = SVC29_BASE + 33u;
constexpr uint32_t SVC_AEABI_LLSL           = SVC29_BASE + 34u;
constexpr uint32_t SVC_AEABI_LLSR           = SVC29_BASE + 35u;
constexpr uint32_t SVC_ISFINITEF            = SVC29_BASE + 36u;
constexpr uint32_t SVC_WPRINTF              = SVC29_BASE + 37u;
constexpr uint32_t SVC_SWSCANF              = SVC29_BASE + 38u;
constexpr uint32_t SVC_LRINT                = SVC29_BASE + 39u; /* lrint(double)->long */
/* iswxdigit accepts a-f/A-F as well as 0-9; iswdigit does not, so the two are
 * not interchangeable — see the symbol table entry for why that mattered. */
constexpr uint32_t SVC_ISWXDIGIT            = SVC29_BASE + 40u;
constexpr uint32_t SVC29_TOTAL            = SVC29_BASE + 41u;

// ---- GC signal / sigaction ----
constexpr uint32_t SVC31_BASE             = SVC29_TOTAL;
constexpr uint32_t SVC_SIGACTION          = SVC31_BASE + 0u; /* sigaction(signum,new,old) */
constexpr uint32_t SVC_PTHREAD_KILL       = SVC31_BASE + 1u; /* pthread_kill/tkill/kill(tid,sig) */
constexpr uint32_t SVC_BSD_SIGNAL         = SVC31_BASE + 2u; /* bsd_signal(signum,handler) */
constexpr uint32_t SVC_EGL_SYSTIME_FREQ   = SVC31_BASE + 3u; /* eglGetSystemTimeFrequencyNV() → u64 ticks/s */
constexpr uint32_t SVC_EGL_SYSTIME        = SVC31_BASE + 4u; /* eglGetSystemTimeNV() → u64 ticks */
constexpr uint32_t SVC_SIGSUSPEND         = SVC31_BASE + 5u; /* sigsuspend(mask): GC suspend loop */

// pipe/pipe2: host-backed pipes (fds live in the same table as open() fds)
constexpr uint32_t SVC_PIPE               = SVC31_BASE + 6u;
constexpr uint32_t SVC_PIPE2              = SVC31_BASE + 7u;
constexpr uint32_t SVC_ALOOPER_ADDFD      = SVC31_BASE + 8u; /* ALooper_addFd → 1 on success */
constexpr uint32_t SVC_ALOOPER_REMOVEFD   = SVC31_BASE + 8u; /* aliased — see handler */

// ASensor* — Unity Input.
constexpr uint32_t SVC_ASENSOR_MGR_INSTANCE = SVC31_BASE + 90u;
constexpr uint32_t SVC_ASENSOR_MGR_DEFAULT  = SVC31_BASE + 91u;
constexpr uint32_t SVC_ASENSOR_MGR_LIST     = SVC31_BASE + 92u;
constexpr uint32_t SVC_ASENSOR_MGR_CREATEQ  = SVC31_BASE + 93u;
constexpr uint32_t SVC_ASENSOR_MGR_DESTROYQ = SVC31_BASE + 94u;
constexpr uint32_t SVC_ASENSOR_Q_ENABLE     = SVC31_BASE + 95u;
constexpr uint32_t SVC_ASENSOR_Q_DISABLE    = SVC31_BASE + 96u;
constexpr uint32_t SVC_ASENSOR_Q_SETRATE    = SVC31_BASE + 97u;
constexpr uint32_t SVC_ASENSOR_Q_HASEVENTS  = SVC31_BASE + 98u;
constexpr uint32_t SVC_ASENSOR_Q_GETEVENTS  = SVC31_BASE + 99u;
constexpr uint32_t SVC_ASENSOR_GETTYPE      = SVC31_BASE + 100u;
constexpr uint32_t SVC_ASENSOR_GETNAME      = SVC31_BASE + 101u;
constexpr uint32_t SVC_ASENSOR_GETVENDOR    = SVC31_BASE + 102u;
constexpr uint32_t SVC_ASENSOR_GETRES       = SVC31_BASE + 103u;
constexpr uint32_t SVC_ASENSOR_GETMINDELAY  = SVC31_BASE + 104u;
inline bool g_asensor_enabled = false;

// Extra libc / EGL / zlib / GLES3 symbols needed by UE arm64 (libUnreal).
constexpr uint32_t SVC_EGL_BIND_API         = SVC31_BASE + 105u; /* eglBindAPI → EGL_TRUE */
constexpr uint32_t SVC_MALLOC_USABLE_SIZE   = SVC31_BASE + 106u;
constexpr uint32_t SVC_ZLIB_VERSION         = SVC31_BASE + 107u;
constexpr uint32_t SVC_DL_ITERATE_PHDR      = SVC31_BASE + 108u;
constexpr uint32_t SVC_ANDROID_ABORT_MSG    = SVC31_BASE + 109u;
constexpr uint32_t SVC_PAUSE                = SVC31_BASE + 110u;
constexpr uint32_t SVC_MINCORE              = SVC31_BASE + 111u;
constexpr uint32_t SVC_SCHED_GETSCHEDULER   = SVC31_BASE + 112u;
constexpr uint32_t SVC_TZSET                = SVC31_BASE + 113u;
constexpr uint32_t SVC_STRFTIME_L           = SVC31_BASE + 114u;
constexpr uint32_t SVC_WCSCHR               = SVC31_BASE + 115u;
constexpr uint32_t SVC_SL_CREATE_ENGINE     = SVC31_BASE + 116u;
constexpr uint32_t SVC_GL_BLIT_FRAMEBUFFER  = SVC31_BASE + 117u;
constexpr uint32_t SVC_GL_TEX_IMAGE_3D      = SVC31_BASE + 118u;
constexpr uint32_t SVC_GL_DRAW_INSTANCED    = SVC31_BASE + 119u; /* Arrays/Elements Instanced */
constexpr uint32_t SVC_GL_HINT              = SVC31_BASE + 120u;
constexpr uint32_t SVC_GL_READ_BUFFER       = SVC31_BASE + 121u;
constexpr uint32_t SVC_GL_GEN_QUERIES       = SVC31_BASE + 122u;
constexpr uint32_t SVC_GL_QUERY_OPS         = SVC31_BASE + 123u; /* Begin/End/GetQueryObjectuiv */
constexpr uint32_t SVC_GL_SAMPLER_OPS       = SVC31_BASE + 124u; /* Gen/Delete/Parameteri */
constexpr uint32_t SVC_GL_MISC3_NOP         = SVC31_BASE + 125u; /* safe no-op GL3 */
// Kept as the highest real SVC number so SVC_TRAMP_TOTAL (= +1) bounds the whole known-SVC range.
constexpr uint32_t SVC_MPROTECT            = SVC31_BASE + 129u;

// zlib size helpers.
constexpr uint32_t SVC_Z_COMPRESSBOUND     = SVC31_BASE + 130u;
constexpr uint32_t SVC_Z_DEFLATEBOUND      = SVC31_BASE + 131u;

// GLES 3.
constexpr uint32_t SVC_GLX_TexBuffer          = SVC31_BASE + 132u;
constexpr uint32_t SVC_GLX_TexBufferRange     = SVC31_BASE + 133u;
constexpr uint32_t SVC_GLX_CopyImageSubData   = SVC31_BASE + 134u;
constexpr uint32_t SVC_GLX_Enablei            = SVC31_BASE + 135u;
constexpr uint32_t SVC_GLX_Disablei           = SVC31_BASE + 136u;
constexpr uint32_t SVC_GLX_ColorMaski         = SVC31_BASE + 137u;
constexpr uint32_t SVC_GLX_BlendEquationi     = SVC31_BASE + 138u;
constexpr uint32_t SVC_GLX_BlendEquationSepi  = SVC31_BASE + 139u;
constexpr uint32_t SVC_GLX_BlendFunci         = SVC31_BASE + 140u;
constexpr uint32_t SVC_GLX_BlendFuncSepi      = SVC31_BASE + 141u;
constexpr uint32_t SVC_GLX_GetPointerv        = SVC31_BASE + 142u;

// OpenSL ES object model.
constexpr uint32_t SVC_SL_OBJ_REALIZE         = SVC31_BASE + 143u;
constexpr uint32_t SVC_SL_OBJ_GETSTATE        = SVC31_BASE + 144u;
constexpr uint32_t SVC_SL_OBJ_GETINTERFACE    = SVC31_BASE + 145u;
constexpr uint32_t SVC_SL_ENG_CREATE_OUTMIX   = SVC31_BASE + 146u;
constexpr uint32_t SVC_SL_ENG_CREATE_PLAYER   = SVC31_BASE + 147u;
constexpr uint32_t SVC_SL_BQ_REGISTER         = SVC31_BASE + 148u;
constexpr uint32_t SVC_SL_BQ_ENQUEUE          = SVC31_BASE + 149u;
constexpr uint32_t SVC_SL_BQ_GETSTATE         = SVC31_BASE + 150u;
// sched_getaffinity(pid, setsize, cpu_set_t*).
constexpr uint32_t SVC_SCHED_GETAFFINITY      = SVC31_BASE + 151u;

// GLES 3.
constexpr uint32_t SVC_GL3_ClearBufferfv      = SVC31_BASE + 152u;
constexpr uint32_t SVC_GL3_ClearBufferiv      = SVC31_BASE + 153u;
constexpr uint32_t SVC_GL3_ClearBufferuiv     = SVC31_BASE + 154u;
constexpr uint32_t SVC_GL3_ClearBufferfi      = SVC31_BASE + 155u;
constexpr uint32_t SVC_GL3_GetUniformBlockIndex   = SVC31_BASE + 156u;
constexpr uint32_t SVC_GL3_UniformBlockBinding    = SVC31_BASE + 157u;
constexpr uint32_t SVC_GL3_GetActiveUniformBlockiv= SVC31_BASE + 158u;
constexpr uint32_t SVC_GL3_GetUniformIndices      = SVC31_BASE + 159u;
constexpr uint32_t SVC_GL3_GetActiveUniformsiv    = SVC31_BASE + 160u;
constexpr uint32_t SVC_GL3_FramebufferTextureLayer= SVC31_BASE + 161u;
constexpr uint32_t SVC_GL3_CopyBufferSubData      = SVC31_BASE + 162u;
constexpr uint32_t SVC_GL3_RenderbufferStorageMS  = SVC31_BASE + 163u;
constexpr uint32_t SVC_GL3_BindImageTexture       = SVC31_BASE + 164u;
constexpr uint32_t SVC_GL3_MemoryBarrier          = SVC31_BASE + 165u;
constexpr uint32_t SVC_GL3_DispatchCompute        = SVC31_BASE + 166u;
constexpr uint32_t SVC_GL3_BindVertexBuffer       = SVC31_BASE + 167u;
constexpr uint32_t SVC_GL3_VertexAttribFormat     = SVC31_BASE + 168u;
constexpr uint32_t SVC_GL3_VertexAttribIFormat    = SVC31_BASE + 169u;
constexpr uint32_t SVC_GL3_VertexAttribBinding    = SVC31_BASE + 170u;
constexpr uint32_t SVC_GL3_VertexBindingDivisor   = SVC31_BASE + 171u;
constexpr uint32_t SVC_GL3_TexStorage2DMS         = SVC31_BASE + 172u;
constexpr uint32_t SVC_GL3_Uniform4uiv            = SVC31_BASE + 173u;
constexpr uint32_t SVC_GL3_GetProgramResourceIndex= SVC31_BASE + 174u;
constexpr uint32_t SVC_GL3_FramebufferTexture     = SVC31_BASE + 175u;
constexpr uint32_t SVC_GL3_FramebufferTexture3D   = SVC31_BASE + 176u;
// GLES2 leftovers + GLES3.
constexpr uint32_t SVC_GL3_CopyTexImage2D            = SVC31_BASE + 177u;
constexpr uint32_t SVC_GL3_GetRenderbufferParameteriv= SVC31_BASE + 178u;
constexpr uint32_t SVC_GL3_ValidateProgram           = SVC31_BASE + 179u;
constexpr uint32_t SVC_GL3_GetTexLevelParameterfv    = SVC31_BASE + 180u;
constexpr uint32_t SVC_GL3_GetTexLevelParameteriv    = SVC31_BASE + 181u;
constexpr uint32_t SVC_GL3_GetUniformiv              = SVC31_BASE + 182u;
constexpr uint32_t SVC_GL3_TexImage2DMultisample     = SVC31_BASE + 183u;
constexpr uint32_t SVC_GL3_TexParameteriv            = SVC31_BASE + 184u;
constexpr uint32_t SVC_GL3_Uniform1uiv               = SVC31_BASE + 185u;
constexpr uint32_t SVC_GL3_Uniform2uiv               = SVC31_BASE + 186u;
constexpr uint32_t SVC_GL3_Uniform3uiv               = SVC31_BASE + 187u;
constexpr uint32_t SVC_GL3_DeleteQueries             = SVC31_BASE + 188u;
constexpr uint32_t SVC_GL3_GetQueryiv                = SVC31_BASE + 189u;
constexpr uint32_t SVC_GL3_CompressedTexImage3D      = SVC31_BASE + 190u;
constexpr uint32_t SVC_GL3_GetActiveUniformBlockName = SVC31_BASE + 191u;
constexpr uint32_t SVC_GL3_VertexAttribIPointer      = SVC31_BASE + 192u;
constexpr uint32_t SVC_GL3_ProgramUniform1fv         = SVC31_BASE + 193u;
constexpr uint32_t SVC_GL3_ProgramUniform1iv         = SVC31_BASE + 194u;
constexpr uint32_t SVC_GL3_ProgramUniform2fv         = SVC31_BASE + 195u;
constexpr uint32_t SVC_GL3_ProgramUniform2iv         = SVC31_BASE + 196u;
constexpr uint32_t SVC_GL3_ProgramUniform3fv         = SVC31_BASE + 197u;
constexpr uint32_t SVC_GL3_ProgramUniform3iv         = SVC31_BASE + 198u;
constexpr uint32_t SVC_GL3_ProgramUniform4fv         = SVC31_BASE + 199u;
constexpr uint32_t SVC_GL3_ProgramUniform4iv         = SVC31_BASE + 200u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix2fv   = SVC31_BASE + 201u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix3fv   = SVC31_BASE + 202u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix4fv   = SVC31_BASE + 203u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix2x3fv = SVC31_BASE + 204u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix3x2fv = SVC31_BASE + 205u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix2x4fv = SVC31_BASE + 206u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix4x2fv = SVC31_BASE + 207u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix3x4fv = SVC31_BASE + 208u;
constexpr uint32_t SVC_GL3_ProgramUniformMatrix4x3fv = SVC31_BASE + 209u;
constexpr uint32_t SVC_GL3_ProgramUniform1uiv        = SVC31_BASE + 210u;
constexpr uint32_t SVC_GL3_ProgramUniform2uiv        = SVC31_BASE + 211u;
constexpr uint32_t SVC_GL3_ProgramUniform3uiv        = SVC31_BASE + 212u;
constexpr uint32_t SVC_GL3_ProgramUniform4uiv        = SVC31_BASE + 213u;
constexpr uint32_t SVC_GL3_PatchParameteri           = SVC31_BASE + 214u;
constexpr uint32_t SVC_GL3_TexStorage3DMultisample   = SVC31_BASE + 215u;
// Android app processes have no controlling terminal.
constexpr uint32_t SVC_TCGETATTR                     = SVC31_BASE + 216u;
constexpr uint32_t SVC_TCSETATTR                     = SVC31_BASE + 217u;
constexpr uint32_t SVC_TCFLUSH                       = SVC31_BASE + 218u;
constexpr uint32_t SVC_GL3_BeginTransformFeedback    = SVC31_BASE + 219u;
constexpr uint32_t SVC_GL3_EndTransformFeedback      = SVC31_BASE + 220u;
constexpr uint32_t SVC_GL3_TransformFeedbackVaryings = SVC31_BASE + 221u;
constexpr uint32_t SVC_GL3_BindTransformFeedback     = SVC31_BASE + 222u;
constexpr uint32_t SVC_GL3_DeleteTransformFeedbacks  = SVC31_BASE + 223u;
constexpr uint32_t SVC_GL3_GenTransformFeedbacks     = SVC31_BASE + 224u;

// AAudio (FMOD output/recorder path; API 26+).
constexpr uint32_t SVC_AAUDIO_CREATE_BUILDER = SVC31_BASE + 9u;  /* AAudio_createStreamBuilder(**b) */
constexpr uint32_t SVC_AAUDIO_OPEN_STREAM    = SVC31_BASE + 10u; /* AAudioStreamBuilder_openStream(b,**s) */
constexpr uint32_t SVC_AAUDIO_GET_FPB        = SVC31_BASE + 11u; /* AAudioStream_getFramesPerBurst */
constexpr uint32_t SVC_AAUDIO_GET_BUFSIZE    = SVC31_BASE + 12u; /* AAudioStream_getBufferSizeInFrames */
constexpr uint32_t SVC_AAUDIO_SET_BUFSIZE    = SVC31_BASE + 13u; /* AAudioStream_setBufferSizeInFrames */
constexpr uint32_t SVC_AAUDIO_GET_BUFCAP     = SVC31_BASE + 14u; /* AAudioStream_getBufferCapacityInFrames */
constexpr uint32_t SVC_AAUDIO_WAIT_STATE     = SVC31_BASE + 15u; /* AAudioStream_waitForStateChange */

// getauxval(type): FMOD dlopen()s libc.
constexpr uint32_t SVC_GETAUXVAL             = SVC31_BASE + 16u;

// AAudio builder setters that must record state for callback pumping
constexpr uint32_t SVC_AAUDIO_SET_DIRECTION  = SVC31_BASE + 17u;
constexpr uint32_t SVC_AAUDIO_SET_DATA_CB    = SVC31_BASE + 18u;
constexpr uint32_t SVC_AAUDIO_SET_FORMAT     = SVC31_BASE + 19u;
constexpr uint32_t SVC_AAUDIO_SET_CHANNELS   = SVC31_BASE + 20u;
constexpr uint32_t SVC_AAUDIO_SET_RATE       = SVC31_BASE + 21u;
constexpr uint32_t SVC_AAUDIO_START          = SVC31_BASE + 22u;
constexpr uint32_t SVC_AAUDIO_STOP           = SVC31_BASE + 23u; /* stop + close */

// Shared SVC behind per-symbol stub trampolines for dlsym'd-but-unimplemented functions.
constexpr uint32_t SVC_UNKNOWN_SYM           = SVC31_BASE + 24u;

// GLES 3.
constexpr uint32_t SVC_GL3_GetStringi             = SVC31_BASE + 25u;
constexpr uint32_t SVC_GL3_GetIntegeri_v          = SVC31_BASE + 26u;
constexpr uint32_t SVC_GL3_GetInternalformativ    = SVC31_BASE + 27u;
constexpr uint32_t SVC_GL3_GetProgramInterfaceiv  = SVC31_BASE + 28u;
constexpr uint32_t SVC_GL3_GetProgramResourceiv   = SVC31_BASE + 29u;
constexpr uint32_t SVC_GL3_GetProgramResourceName = SVC31_BASE + 30u;
constexpr uint32_t SVC_GL3_GenVertexArrays        = SVC31_BASE + 31u;
constexpr uint32_t SVC_GL3_BindVertexArray        = SVC31_BASE + 32u;
constexpr uint32_t SVC_GL3_DeleteVertexArrays     = SVC31_BASE + 33u;
constexpr uint32_t SVC_GL3_IsVertexArray          = SVC31_BASE + 34u;
constexpr uint32_t SVC_GL3_BindSampler            = SVC31_BASE + 35u;
constexpr uint32_t SVC_GL3_BindBufferBase         = SVC31_BASE + 36u;
constexpr uint32_t SVC_GL3_BindBufferRange        = SVC31_BASE + 37u;
constexpr uint32_t SVC_GL3_MapBufferRange         = SVC31_BASE + 38u;
constexpr uint32_t SVC_GL3_UnmapBuffer            = SVC31_BASE + 39u;
constexpr uint32_t SVC_GL3_FlushMappedBufferRange = SVC31_BASE + 40u;
constexpr uint32_t SVC_GL3_TexStorage2D           = SVC31_BASE + 41u;
constexpr uint32_t SVC_GL3_TexStorage3D           = SVC31_BASE + 42u;
constexpr uint32_t SVC_GL3_TexSubImage3D          = SVC31_BASE + 43u;
constexpr uint32_t SVC_GL3_ProgramParameteri      = SVC31_BASE + 44u;
constexpr uint32_t SVC_GL3_GetProgramBinary       = SVC31_BASE + 45u;
constexpr uint32_t SVC_GL3_ProgramBinary          = SVC31_BASE + 46u;
constexpr uint32_t SVC_GL3_FenceSync              = SVC31_BASE + 47u;
constexpr uint32_t SVC_GL3_ClientWaitSync         = SVC31_BASE + 48u;
constexpr uint32_t SVC_GL3_DeleteSync             = SVC31_BASE + 49u;
// Appended after the packed SVC31 block (see SVC_MPROTECT below).
constexpr uint32_t SVC_GL3_IsSync                 = SVC31_BASE + 127u;
constexpr uint32_t SVC_GL_TexParameterfv          = SVC31_BASE + 128u;
constexpr uint32_t SVC_GL3_InvalidateFramebuffer  = SVC31_BASE + 50u;
constexpr uint32_t SVC_GL3_DetachShader           = SVC31_BASE + 51u;
constexpr uint32_t SVC_GL3_DrawBuffers            = SVC31_BASE + 52u;
constexpr uint32_t SVC_GL3_DrawElementsBaseVertex = SVC31_BASE + 53u;

// cxa_throw logging (always on): identifies which managed exception IL2CPP throws.
constexpr uint32_t SVC_EXC_CXA_THROW              = SVC31_BASE + 54u;

// GL extension entry points the host may or may not back.
constexpr uint32_t SVC_GLX_DebugMessageControl    = SVC31_BASE + 55u;
constexpr uint32_t SVC_GLX_DebugMessageCallback   = SVC31_BASE + 56u;
constexpr uint32_t SVC_GLX_DebugMessageInsert     = SVC31_BASE + 57u;
constexpr uint32_t SVC_GLX_ObjectLabel            = SVC31_BASE + 58u;
constexpr uint32_t SVC_GLX_GetObjectLabel         = SVC31_BASE + 59u;
constexpr uint32_t SVC_GLX_PushDebugGroup         = SVC31_BASE + 60u;
constexpr uint32_t SVC_GLX_PopDebugGroup          = SVC31_BASE + 61u;
constexpr uint32_t SVC_GLX_MarkerNop              = SVC31_BASE + 62u; /* EXT_debug_marker/label */
constexpr uint32_t SVC_GLX_BufferStorage          = SVC31_BASE + 63u;
constexpr uint32_t SVC_GLX_QueryCounter           = SVC31_BASE + 64u;
constexpr uint32_t SVC_GLX_GetQueryObjectui64v    = SVC31_BASE + 65u;
constexpr uint32_t SVC_GLX_DrawElemInstBaseVertex = SVC31_BASE + 66u;
constexpr uint32_t SVC_GLX_BlendBarrier           = SVC31_BASE + 67u;

// UE4 NativeActivity / AssetManager APIs not covered above
constexpr uint32_t SVC_AASSETMGR_OPENDIR          = SVC31_BASE + 68u;
constexpr uint32_t SVC_AASSETDIR_NEXT             = SVC31_BASE + 69u;
constexpr uint32_t SVC_AASSETDIR_CLOSE            = SVC31_BASE + 70u;
constexpr uint32_t SVC_AASSET_OPENFD              = SVC31_BASE + 71u; /* openFileDescriptor / 64 */
constexpr uint32_t SVC_ACFG_NEW                   = SVC31_BASE + 72u; /* AConfiguration_new */
constexpr uint32_t SVC_ACFG_GETLANG               = SVC31_BASE + 73u; /* getLanguage → write 2 chars */
constexpr uint32_t SVC_ACFG_GETCOUNTRY            = SVC31_BASE + 74u; /* getCountry → write 2 chars */
constexpr uint32_t SVC_ACFG_FROM_AM               = SVC31_BASE + 75u; /* fromAssetManager */
constexpr uint32_t SVC_ATOF                       = SVC31_BASE + 76u;
constexpr uint32_t SVC_FREXPF                     = SVC31_BASE + 77u;
constexpr uint32_t SVC_RAND                       = SVC31_BASE + 78u;
constexpr uint32_t SVC_SRAND                      = SVC31_BASE + 79u;
constexpr uint32_t SVC_GETENTROPY                 = SVC31_BASE + 80u;
constexpr uint32_t SVC_SYSINFO                    = SVC31_BASE + 81u;
constexpr uint32_t SVC_COMPRESS2                  = SVC31_BASE + 82u;
constexpr uint32_t SVC_ISLOWER                    = SVC31_BASE + 83u;
constexpr uint32_t SVC_ISUPPER                    = SVC31_BASE + 84u;
constexpr uint32_t SVC_ISBLANK                    = SVC31_BASE + 85u;
constexpr uint32_t SVC_TOUPPER                    = SVC31_BASE + 86u;

/* eventfd(2) — host-backed, like pipe(): guest fds are host fds.  UE's
 * FHttpManager and the task-graph use one as a wakeup handle; without it the
 * dlsym stub handed back 0, which is a perfectly usable fd number, so the
 * engine wrote its wakeups into stdin forever and never woke. */
constexpr uint32_t SVC_EVENTFD                    = SVC31_BASE + 87u;
constexpr uint32_t SVC_EVENTFD_READ               = SVC31_BASE + 88u;
constexpr uint32_t SVC_EVENTFD_WRITE              = SVC31_BASE + 89u;



// AConfiguration_getXxx() integer getters.
enum : uint32_t {
    ACFG_I_MCC = 0, ACFG_I_MNC, ACFG_I_ORIENTATION, ACFG_I_TOUCHSCREEN,
    ACFG_I_DENSITY, ACFG_I_KEYBOARD, ACFG_I_NAVIGATION, ACFG_I_KEYSHIDDEN,
    ACFG_I_NAVHIDDEN, ACFG_I_SCREENSIZE, ACFG_I_SCREENLONG, ACFG_I_UIMODETYPE,
    ACFG_I_UIMODENIGHT, ACFG_I_LAYOUTDIR, ACFG_I_SCREENWIDTHDP,
    ACFG_I_SCREENHEIGHTDP, ACFG_I_SMALLESTSCREENWIDTHDP, ACFG_I_COUNT
};
constexpr uint32_t SVC_ACFG_INT_BASE              = SVC31_BASE + 225u;
constexpr uint32_t SVC_ACFG_INT_END               = SVC_ACFG_INT_BASE + ACFG_I_COUNT - 1u;

// stdio / zlib / math entry points that libgnustl_shared.
constexpr uint32_t SVC_LIBC_REWIND                = SVC31_BASE + 243u;
constexpr uint32_t SVC_LIBC_FREOPEN               = SVC31_BASE + 244u;
constexpr uint32_t SVC_LIBC_TMPFILE               = SVC31_BASE + 245u;
constexpr uint32_t SVC_LIBC_TMPNAM                = SVC31_BASE + 246u;
constexpr uint32_t SVC_COMPRESS                   = SVC31_BASE + 247u;
constexpr uint32_t SVC_FREXPL                     = SVC31_BASE + 248u;
constexpr uint32_t SVC_ACFG_MATCH                 = SVC31_BASE + 249u;
// GL_OES_mapbuffer / GL_EXT_discard_framebuffer.
constexpr uint32_t SVC_GL_MapBufferOES            = SVC31_BASE + 250u;
constexpr uint32_t SVC_GL_UnmapBufferOES          = SVC31_BASE + 251u;
constexpr uint32_t SVC_GL_DiscardFramebufferEXT   = SVC31_BASE + 252u;

// BSD sockets.
constexpr uint32_t SVC_NET_SOCKET                 = SVC31_BASE + 253u;
constexpr uint32_t SVC_NET_SOCKETPAIR             = SVC31_BASE + 254u;
constexpr uint32_t SVC_NET_CONNECT                = SVC31_BASE + 255u;
constexpr uint32_t SVC_NET_BIND                   = SVC31_BASE + 256u;
constexpr uint32_t SVC_NET_LISTEN                 = SVC31_BASE + 257u;
constexpr uint32_t SVC_NET_ACCEPT                 = SVC31_BASE + 258u;
constexpr uint32_t SVC_NET_ACCEPT4                = SVC31_BASE + 259u;
constexpr uint32_t SVC_NET_SEND                   = SVC31_BASE + 260u;
constexpr uint32_t SVC_NET_SENDTO                 = SVC31_BASE + 261u;
constexpr uint32_t SVC_NET_RECV                   = SVC31_BASE + 262u;
constexpr uint32_t SVC_NET_RECVFROM               = SVC31_BASE + 263u;
constexpr uint32_t SVC_NET_SENDMSG                = SVC31_BASE + 264u;
constexpr uint32_t SVC_NET_RECVMSG                = SVC31_BASE + 265u;
constexpr uint32_t SVC_NET_SHUTDOWN               = SVC31_BASE + 266u;
constexpr uint32_t SVC_NET_SETSOCKOPT             = SVC31_BASE + 267u;
constexpr uint32_t SVC_NET_GETSOCKOPT             = SVC31_BASE + 268u;
constexpr uint32_t SVC_NET_GETSOCKNAME            = SVC31_BASE + 269u;
constexpr uint32_t SVC_NET_GETPEERNAME            = SVC31_BASE + 270u;
constexpr uint32_t SVC_NET_SELECT                 = SVC31_BASE + 271u;
constexpr uint32_t SVC_NET_POLL                   = SVC31_BASE + 272u;
constexpr uint32_t SVC_NET_GETADDRINFO            = SVC31_BASE + 273u;
constexpr uint32_t SVC_NET_FREEADDRINFO           = SVC31_BASE + 274u;
constexpr uint32_t SVC_NET_GAI_STRERROR           = SVC31_BASE + 275u;
constexpr uint32_t SVC_NET_GETHOSTBYNAME          = SVC31_BASE + 276u;
constexpr uint32_t SVC_NET_INET_NTOP              = SVC31_BASE + 277u;
constexpr uint32_t SVC_NET_INET_PTON              = SVC31_BASE + 278u;
constexpr uint32_t SVC_NET_INET_ATON              = SVC31_BASE + 279u;
constexpr uint32_t SVC_NET_INET_NTOA              = SVC31_BASE + 280u;
constexpr uint32_t SVC_NET_EPOLL_CREATE           = SVC31_BASE + 281u;
constexpr uint32_t SVC_NET_EPOLL_CTL              = SVC31_BASE + 282u;
constexpr uint32_t SVC_NET_EPOLL_WAIT             = SVC31_BASE + 283u;
constexpr uint32_t SVC_NET_IOCTL                  = SVC31_BASE + 284u;
constexpr uint32_t SVC_NET_IF_NAMETOINDEX         = SVC31_BASE + 285u;
constexpr uint32_t SVC_NET_IF_INDEXTONAME         = SVC31_BASE + 286u;
// bionic exports htons/htonl/ntohs/ntohl as real functions.
constexpr uint32_t SVC_NET_BSWAP16               = SVC31_BASE + 287u;
constexpr uint32_t SVC_NET_BSWAP32               = SVC31_BASE + 288u;

// EGL_KHR_fence_sync / EGL 1.
constexpr uint32_t SVC_EGL_CREATE_SYNC            = SVC31_BASE + 289u;
constexpr uint32_t SVC_EGL_DESTROY_SYNC           = SVC31_BASE + 290u;
constexpr uint32_t SVC_EGL_CLIENT_WAIT_SYNC       = SVC31_BASE + 291u;
constexpr uint32_t SVC_EGL_GET_SYNC_ATTRIB        = SVC31_BASE + 292u;
constexpr uint32_t SVC_EGL_WAIT_SYNC              = SVC31_BASE + 293u;
// glVertexAttribDivisor is ES 3.
constexpr uint32_t SVC_GL3_VertexAttribDivisor    = SVC31_BASE + 294u;
constexpr uint32_t SVC_GL3_IsQuery                = SVC31_BASE + 295u;
// pthread_setname_np was a no-op, so every diagnostic that lists guest threads could only show numbers.
constexpr uint32_t SVC_PTHREAD_SETNAME            = SVC31_BASE + 296u;
constexpr uint32_t SVC_ATOLL                       = SVC31_BASE + 297u;
/* Entry points the loader previously left as "unresolved → stub", i.e. calls
 * that silently returned 0 and left their out-parameters untouched. */
constexpr uint32_t SVC_EGL_GETCURDPY              = SVC31_BASE + 298u;
constexpr uint32_t SVC_GL_GETINTEGER64V           = SVC31_BASE + 299u;
constexpr uint32_t SVC_ARC4RANDOM_BUF             = SVC31_BASE + 300u;
/* POSIX regex.  libCrashSight matches thread names and library paths with
 * these; stubbed out, every match failed and its filters selected nothing. */
constexpr uint32_t SVC_REGCOMP                    = SVC31_BASE + 301u;
constexpr uint32_t SVC_REGEXEC                    = SVC31_BASE + 302u;
constexpr uint32_t SVC_REGFREE                    = SVC31_BASE + 303u;
// scandir(dir, &namelist, filter, compar) — enumeration with guest callbacks.
constexpr uint32_t SVC_SCANDIR                    = SVC31_BASE + 304u;
/* process_vm_readv: read guest memory without risking a fault, which is the
 * whole reason a crash handler reaches for it. */
constexpr uint32_t SVC_PROCESS_VM_READV           = SVC31_BASE + 305u;
/* Scheduling policy.  These were unresolved imports, i.e. stubs returning 0 —
 * "your SCHED_FIFO request was granted" — and sched_get_priority_max/min sat
 * at SVC_RET0, so the whole usable priority band read back as [0,0]. */
constexpr uint32_t SVC_SCHED_SETSCHEDULER         = SVC31_BASE + 306u;
constexpr uint32_t SVC_SCHED_SETPARAM             = SVC31_BASE + 307u;
constexpr uint32_t SVC_SCHED_GETPARAM             = SVC31_BASE + 308u;
constexpr uint32_t SVC_SCHED_PRIO_MAX             = SVC31_BASE + 309u;
constexpr uint32_t SVC_SCHED_PRIO_MIN             = SVC31_BASE + 310u;
/* ASharedMemory_* (libandroid, API 26+).  A stub returned 0, which is a
 * perfectly valid fd number — the guest then mmap()ed and ftruncate()d stdin. */
constexpr uint32_t SVC_ASHMEM_CREATE              = SVC31_BASE + 311u;
constexpr uint32_t SVC_ASHMEM_GETSIZE             = SVC31_BASE + 312u;
constexpr uint32_t SVC_ASHMEM_SETPROT             = SVC31_BASE + 313u;
/* Wide-char stdio.  putwc/fputwc sat at SVC_RET0: the call reported success
 * (0 is not WEOF) while the character went nowhere. */
constexpr uint32_t SVC_FPUTWC                     = SVC31_BASE + 314u;
constexpr uint32_t SVC_FPUTWS                     = SVC31_BASE + 315u;
// ANativeWindow_setBuffersTransform — dlsym'd out of libnativewindow.so.
constexpr uint32_t SVC_ANW_SETBUFTRANSFORM        = SVC31_BASE + 316u;
/* EGLImage / GL_OES_EGL_image.  UE resolves these with eglGetProcAddress and
 * later calls through the saved pointers unconditionally — a NULL entry is a
 * NoExecuteFault, not a skipped optional path.  There is no dma-buf import
 * here, so an EGLImage is the 2D texture it wraps (same contract as mapping
 * GL_TEXTURE_EXTERNAL_OES → GL_TEXTURE_2D). */
constexpr uint32_t SVC_EGL_CREATE_IMAGE           = SVC31_BASE + 317u;
constexpr uint32_t SVC_EGL_DESTROY_IMAGE          = SVC31_BASE + 318u;
constexpr uint32_t SVC_GL_EGLImageTargetTexture2DOES = SVC31_BASE + 319u;
constexpr uint32_t SVC_GL_EGLImageTargetTexStorageEXT = SVC31_BASE + 320u;
constexpr uint32_t SVC_EGL_GET_NATIVE_CLIENT_BUFFER = SVC31_BASE + 321u;
/* EGL extensions the host string advertises but which had no trampoline —
 * UE/Mesa probe them via eglGetProcAddress; NULL means a later crash, not a
 * skipped optional path.  Each handler below matches the extension's contract
 * on this host (no dma-buf plane to export, fences are GL syncs, …). */
constexpr uint32_t SVC_EGL_SET_BLOB_CACHE         = SVC31_BASE + 322u;
constexpr uint32_t SVC_EGL_DUP_NATIVE_FENCE       = SVC31_BASE + 323u;
constexpr uint32_t SVC_EGL_GET_MSC_RATE           = SVC31_BASE + 324u;
constexpr uint32_t SVC_EGL_QUERY_DMABUF_FORMATS   = SVC31_BASE + 325u;
constexpr uint32_t SVC_EGL_SWAP_DAMAGE            = SVC31_BASE + 326u;
constexpr uint32_t SVC_EGL_EXPORT_DMABUF          = SVC31_BASE + 327u;
constexpr uint32_t SVC_EGL_GET_DRIVER_NAME        = SVC31_BASE + 328u;
constexpr uint32_t SVC_EGL_PRESENTATION_TIME      = SVC31_BASE + 329u;

/* AChoreographer refresh-rate callbacks (API 30+) — see
 * post_refresh_rate_callback() for why answering these with a stub is not a
 * harmless omission. */
constexpr uint32_t SVC_ACHOREOGRAPHER_REG_RR      = SVC31_BASE + 330u;
constexpr uint32_t SVC_ACHOREOGRAPHER_UNREG_RR    = SVC31_BASE + 331u;

/* NDK input queue.  A NativeActivity gets every touch through this path — the
 * native_app_glue that UE links reads it in process_input() — so answering
 * AInputQueue_getEvent with "no events" forever is not a missing extra: it is
 * an emulator with no touchscreen. */
constexpr uint32_t SVC_AINPUTQ_ATTACH             = SVC31_BASE + 332u;
constexpr uint32_t SVC_AINPUTQ_DETACH             = SVC31_BASE + 333u;
constexpr uint32_t SVC_AINPUTQ_HASEVENTS          = SVC31_BASE + 334u;
constexpr uint32_t SVC_AINPUTQ_GETEVENT           = SVC31_BASE + 335u;
constexpr uint32_t SVC_AINPUTQ_PREDISPATCH        = SVC31_BASE + 336u;
constexpr uint32_t SVC_AINPUTQ_FINISH             = SVC31_BASE + 337u;
constexpr uint32_t SVC_AINPUTEV_TYPE              = SVC31_BASE + 338u;
constexpr uint32_t SVC_AINPUTEV_SOURCE            = SVC31_BASE + 339u;
constexpr uint32_t SVC_AINPUTEV_DEVICEID          = SVC31_BASE + 340u;
constexpr uint32_t SVC_AMOTION_ACTION             = SVC31_BASE + 341u;
constexpr uint32_t SVC_AMOTION_POINTERCOUNT       = SVC31_BASE + 342u;
constexpr uint32_t SVC_AMOTION_POINTERID          = SVC31_BASE + 343u;
constexpr uint32_t SVC_AMOTION_X                  = SVC31_BASE + 344u;
constexpr uint32_t SVC_AMOTION_Y                  = SVC31_BASE + 345u;
constexpr uint32_t SVC_AMOTION_EVENTTIME          = SVC31_BASE + 346u;
constexpr uint32_t SVC_AMOTION_DOWNTIME           = SVC31_BASE + 347u;
constexpr uint32_t SVC_AMOTION_PRESSURE           = SVC31_BASE + 348u;
constexpr uint32_t SVC_AMOTION_SIZE               = SVC31_BASE + 349u;
constexpr uint32_t SVC_AMOTION_TOOLTYPE           = SVC31_BASE + 350u;
constexpr uint32_t SVC_AMOTION_AXISVALUE          = SVC31_BASE + 351u;

/* AAsset stream reads.  AAssetManager_open already materialises the whole
 * entry, so the stream API is a cursor over that buffer — the NDK contract
 * every non-mmap reader (`AAsset_read` loops until it returns 0) relies on. */
constexpr uint32_t SVC_AASSET_READ                = SVC31_BASE + 352u;
constexpr uint32_t SVC_AASSET_SEEK                = SVC31_BASE + 353u;
constexpr uint32_t SVC_AASSET_SEEK64              = SVC31_BASE + 354u;
constexpr uint32_t SVC_AASSET_GETLENGTH64         = SVC31_BASE + 355u;
constexpr uint32_t SVC_AASSET_GETREMAINING        = SVC31_BASE + 356u;
constexpr uint32_t SVC_AASSET_GETREMAINING64      = SVC31_BASE + 357u;
constexpr uint32_t SVC_AASSET_ISALLOCATED         = SVC31_BASE + 358u;
constexpr uint32_t SVC_AASSET_CLOSE               = SVC31_BASE + 359u;

/* JNIEnv slots 222–232.  The layout of the vtable is fixed by <jni.h>; the SVC
 * numbers behind it are Lunaria's own, and the low ones were handed out before
 * these entries were modelled at all.  Rather than renumber every SVC in the
 * file, the tail of the table maps explicitly — see jni_vtable_svc(). */
constexpr uint32_t SVC_JNI_GET_PRIM_CRITICAL      = SVC31_BASE + 360u;
constexpr uint32_t SVC_JNI_REL_PRIM_CRITICAL      = SVC31_BASE + 361u;
constexpr uint32_t SVC_JNI_GET_STR_CRITICAL       = SVC31_BASE + 362u;
constexpr uint32_t SVC_JNI_REL_STR_CRITICAL       = SVC31_BASE + 363u;
constexpr uint32_t SVC_JNI_NEW_DIRECT_BB          = SVC31_BASE + 364u;
constexpr uint32_t SVC_JNI_DIRECT_BB_ADDR         = SVC31_BASE + 365u;
constexpr uint32_t SVC_JNI_DIRECT_BB_CAP          = SVC31_BASE + 366u;
constexpr uint32_t SVC_JNI_OBJECT_REF_TYPE        = SVC31_BASE + 367u;
constexpr uint32_t SVC_OPENAT                     = SVC31_BASE + 368u;
constexpr uint32_t SVC_FDOPENDIR                  = SVC31_BASE + 369u;
constexpr uint32_t SVC_UNLINKAT                   = SVC31_BASE + 370u;
constexpr uint32_t SVC_SIGISMEMBER                = SVC31_BASE + 371u;
constexpr uint32_t SVC_SIGEMPTYSET                = SVC31_BASE + 372u;
constexpr uint32_t SVC_SIGFILLSET                 = SVC31_BASE + 373u;
constexpr uint32_t SVC_SIGADDSET                  = SVC31_BASE + 374u;
constexpr uint32_t SVC_SIGDELSET                  = SVC31_BASE + 375u;
constexpr uint32_t SVC_CFI_SLOWPATH               = SVC31_BASE + 376u;
constexpr uint32_t SVC_SIGPROCMASK                = SVC31_BASE + 377u;
/* Kept so trampoline numbers after it stay put.  SwappyGL_swap itself is
 * no longer patched: the guest runs it, and the EGL timestamp entry points
 * below are what its swap path actually calls. */
constexpr uint32_t SVC_SWAPPY_GL_SWAP             = SVC31_BASE + 378u;
/* alarm(2): arms a one-shot SIGALRM and answers what was left on the previous
 * one.  Stubbed to zero it always claimed "no alarm was pending", so a caller
 * that arms a watchdog and later cancels it reads back a lie. */
constexpr uint32_t SVC_ALARM                      = SVC31_BASE + 379u;
/* eglGetSyncValuesCHROMIUM: the counters a frame pacer reads to line its
 * submissions up with the display.  Without an entry point the whole
 * EGL_CHROMIUM_sync_control extension had to be stripped from the string. */
constexpr uint32_t SVC_EGL_GET_SYNC_VALUES        = SVC31_BASE + 381u;
/* unshare(2): needs privileges Android apps do not have.  The stub's implicit
 * success told the guest it had its own namespace when nothing had changed;
 * EPERM is what the call really returns to an app. */
constexpr uint32_t SVC_UNSHARE                    = SVC31_BASE + 380u;
/* glDrawElementsInstanced.  It used to share SVC_GL_DRAW_INSTANCED with
 * glDrawArraysInstanced, and the handler could not tell them apart: every
 * indexed instanced draw was executed as glDrawArraysInstanced(mode, count,
 * type, indices) — first = the index count, count = the *type enum* (0x1403 =
 * 5123 vertices), instancecount = the index offset.  Whatever that submits, it
 * is not the geometry the guest asked for. */
constexpr uint32_t SVC_GL_DRAW_ELEM_INSTANCED     = SVC31_BASE + 382u;
/* JavaVM::DetachCurrentThread.  The slot had no SVC of its own, so it kept the
 * default fill below — trampoline index 0, which on A64 is the raw-syscall
 * entry, not a stub that returns.  Every worker thread that attached, did its
 * JNI work and detached therefore ended its life on ENOSYS from a syscall it
 * never made. */
constexpr uint32_t SVC_JVM_DETACH                 = SVC31_BASE + 383u;
/* EGL_ANDROID_get_frame_timestamps.  Frame pacers (Swappy) look these up
 * with eglGetProcAddress and, when they are missing, either disable
 * themselves or wait forever for a present that can never be observed. */
constexpr uint32_t SVC_EGL_GET_NEXT_FRAME_ID      = SVC31_BASE + 384u;
constexpr uint32_t SVC_EGL_GET_FRAME_TIMESTAMPS   = SVC31_BASE + 385u;
constexpr uint32_t SVC_EGL_FRAME_TS_SUPPORTED     = SVC31_BASE + 386u;
constexpr uint32_t SVC_EGL_GET_COMPOSITOR_TIMING  = SVC31_BASE + 387u;
constexpr uint32_t SVC_EGL_COMPOSITOR_TIMING_SUP  = SVC31_BASE + 388u;
/* Previously fell through to the unknown-symbol stub (silent 0 / untouched
 * out-params).  arc4random fills entropy; mallinfo reports heap shape;
 * signalfd is the crash-handler wake path. */
constexpr uint32_t SVC_ARC4RANDOM                 = SVC31_BASE + 389u;
constexpr uint32_t SVC_MALLINFO                   = SVC31_BASE + 390u;
constexpr uint32_t SVC_SIGNALFD                   = SVC31_BASE + 391u;

/* Occlusion queries.  These three used to share SVC_GL_QUERY_OPS, which was a
 * plain no-op — including glGetQueryObjectuiv, whose whole job is to write the
 * out-param.  UE4's RHI thread polls GL_QUERY_RESULT_AVAILABLE in a
 * sched_yield loop, so an untouched out-param that happens to hold 0 is an
 * RHI thread that never presents again.  (The 64-bit sibling
 * SVC_GLX_GetQueryObjectui64v always answered "available", which is why only
 * the 32-bit path hung.) */
constexpr uint32_t SVC_GL_BEGIN_QUERY             = SVC31_BASE + 392u;
constexpr uint32_t SVC_GL_END_QUERY               = SVC31_BASE + 393u;
constexpr uint32_t SVC_GL_GET_QUERY_OBJECT_UIV    = SVC31_BASE + 394u;
/* clock_nanosleep(clkid, flags, req, rem) — its own entry, not an alias of
 * nanosleep(req, rem): the two put the timespec in different argument slots,
 * and reading the clock id as a pointer made every clock_nanosleep ask to
 * sleep for zero and spin instead. */
constexpr uint32_t SVC_CLOCK_NANOSLEEP            = SVC31_BASE + 395u;
/* fflush(3).  It was bound to the generic "returns 0" stub, which is a lie the
 * guest cannot see through: the emulator keeps a real host FILE* per guest
 * stream, so an unflushed write is still sitting in the host's buffer when the
 * guest goes on to read the file back. */
constexpr uint32_t SVC_LIBC_FFLUSH                = SVC31_BASE + 396u;
/* ANativeWindow::query for the fake native window.  Keeping the policy in the
 * host avoids baking an incomplete, version-specific switch into guest code. */
constexpr uint32_t SVC_ANW_QUERY                  = SVC31_BASE + 397u;

/* stdio pushback and the wide-character read side.  Both sat at SVC_RET0.
 * ungetc() returning 0 is indistinguishable from success for a caller that
 * only checks against EOF, so a parser that peeks one byte and pushes it back
 * silently lost it — the byte was never put anywhere, and the next getc()
 * returned the one after.  getwc() answering 0 is worse: 0 is L'\0', a
 * perfectly good wide character, so a read loop that stops at WEOF never
 * stops. */
constexpr uint32_t SVC_UNGETC                     = SVC31_BASE + 398u;
constexpr uint32_t SVC_UNGETWC                    = SVC31_BASE + 399u;
constexpr uint32_t SVC_GETWC                      = SVC31_BASE + 400u;
/* Per-object locales (POSIX 2008).  newlocale() returning NULL is the "out of
 * memory / unsupported locale" answer, and libc++'s std::locale constructor
 * turns that into a runtime_error; uselocale() returning NULL is not even a
 * legal locale_t.  Android has exactly one locale — C.UTF-8, under several
 * names — so these are cheap to answer truthfully. */
constexpr uint32_t SVC_NEWLOCALE                  = SVC31_BASE + 401u;
constexpr uint32_t SVC_USELOCALE                  = SVC31_BASE + 402u;
constexpr uint32_t SVC_FREELOCALE                 = SVC31_BASE + 403u;
constexpr uint32_t SVC_DUPLOCALE                  = SVC31_BASE + 404u;
/* wcstold(): the wide-character long-double parse.  See SVC_STRTOLD for the
 * return width — on A64 a long double is a 128-bit quad in q0, not a double. */
constexpr uint32_t SVC_WCSTOLD                    = SVC31_BASE + 405u;
/* Wide-string collation.  Returning 0 from wcscoll means "these two strings
 * are equal", which turns every sort that uses it into a no-op and every
 * lookup keyed on it into a false hit. */
constexpr uint32_t SVC_WCSCOLL                    = SVC31_BASE + 406u;
constexpr uint32_t SVC_WCSXFRM                    = SVC31_BASE + 407u;
/* wcsnrtombs(): the wide->multibyte direction of SVC_MBSRTOWCS. */
constexpr uint32_t SVC_WCSNRTOMBS                 = SVC31_BASE + 408u;
constexpr uint32_t SVC_WCSRTOMBS                  = SVC31_BASE + 409u;
/* mbsnrtowcs() is not mbsrtowcs() with an extra argument: it takes the source
 * limit *before* the destination limit, so sharing one handler read the wrong
 * register as "how many wide characters fit" and wrote past the caller's
 * buffer whenever the two differed. */
constexpr uint32_t SVC_MBSNRTOWCS                 = SVC31_BASE + 410u;
/* rmdir(2).  It was bound to the "returns -1" template, so every attempt to
 * remove a directory failed — with no errno set, so the guest could not even
 * tell why.  A game that cleans up its own cache directory tree leaves it
 * behind and, worse, may treat the failure as "the directory is in use". */
constexpr uint32_t SVC_RMDIR                      = SVC31_BASE + 411u;
/* pthread_getschedparam / pthread_setschedparam.  The getter returning 0
 * without writing its two out-parameters is the dangerous one: the caller
 * reads an uninitialised policy and priority off its own stack and then hands
 * them straight back to the setter. */
constexpr uint32_t SVC_PTHREAD_GETSCHEDPARAM      = SVC31_BASE + 412u;
constexpr uint32_t SVC_PTHREAD_SETSCHEDPARAM      = SVC31_BASE + 413u;
/* __sched_cpucount() is what CPU_COUNT() expands to.  Answering 0 tells the
 * caller its affinity mask contains no CPUs at all, which is how a worker-pool
 * size computed from "how many cores may I use" comes out as zero. */
constexpr uint32_t SVC_SCHED_CPUCOUNT             = SVC31_BASE + 414u;
/* libc calls that were bound to the shared "return 0" template and then showed
 * up as `[stub] CALLED … nothing was done`.  A zero answer is often a lie the
 * guest acts on (getuid()=0 is root; dladdr()=0 means "no module"; setenv()=0
 * looks like success without writing).  Each gets its own trampoline. */
constexpr uint32_t SVC_GETUID                     = SVC31_BASE + 415u;
constexpr uint32_t SVC_GETEUID                    = SVC31_BASE + 416u;
constexpr uint32_t SVC_GETGID                     = SVC31_BASE + 417u;
constexpr uint32_t SVC_GETEGID                    = SVC31_BASE + 418u;
constexpr uint32_t SVC_PRCTL                      = SVC31_BASE + 419u;
constexpr uint32_t SVC_SETPRIORITY                = SVC31_BASE + 420u;
constexpr uint32_t SVC_GETPRIORITY                = SVC31_BASE + 421u;
constexpr uint32_t SVC_MADVISE                    = SVC31_BASE + 422u;
constexpr uint32_t SVC_MSYNC                      = SVC31_BASE + 423u;
constexpr uint32_t SVC_SETENV                     = SVC31_BASE + 424u;
constexpr uint32_t SVC_UNSETENV                   = SVC31_BASE + 425u;
constexpr uint32_t SVC_PTHREAD_SIGMASK            = SVC31_BASE + 426u;
constexpr uint32_t SVC_DLADDR                     = SVC31_BASE + 427u;
constexpr uint32_t SVC_DLERROR                    = SVC31_BASE + 428u;
constexpr uint32_t SVC_FSCANF                     = SVC31_BASE + 429u;
constexpr uint32_t SVC_FSYNC                      = SVC31_BASE + 430u;
constexpr uint32_t SVC_FLOCK                      = SVC31_BASE + 431u;

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

/* Cover ASENSOR + UE extras (must be ≥ highest SVC31_* used as trampoline).
 * Every SVC in the table needs its trampoline built by build_jni_tables(), and
 * that loop stops at SVC_TRAMP_TOTAL; anything past it gets a zero-filled slot
 * that the unknown-symbol pool then hands out to somebody else. */
static_assert(SVC_PTHREAD_SETNAME > SVC_GL3_GenTransformFeedbacks,
              "keep SVC_TRAMP_TOTAL above every trampolined SVC");
/* The bound must sit above the *highest* SVC, not above whichever one happened
 * to be last when the constant was written: SVC_SWAPPY_GL_SWAP and the four
 * numbers after it live past SVC_SIGPROCMASK, so their trampolines were never
 * built and the unknown-symbol pool — which starts here — handed the same
 * addresses out to dlsym'd names it did not implement. */
constexpr uint32_t SVC_TRAMP_TOTAL        = SVC_UE_HOOK_LAST + 1u;
static_assert(SVC_TRAMP_TOTAL > SVC_PROCESS_VM_READV &&
              SVC_TRAMP_TOTAL > SVC_ACFG_INT_END &&
              SVC_TRAMP_TOTAL > SVC_GL3_GenTransformFeedbacks &&
              SVC_TRAMP_TOTAL > SVC_SWAPPY_GL_SWAP &&
              SVC_TRAMP_TOTAL > SVC_ALARM &&
              SVC_TRAMP_TOTAL > SVC_UNSHARE &&
              SVC_TRAMP_TOTAL > SVC_EGL_GET_SYNC_VALUES &&
              SVC_TRAMP_TOTAL > SVC_GL_DRAW_ELEM_INSTANCED &&
              SVC_TRAMP_TOTAL > SVC_EGL_GET_NEXT_FRAME_ID &&
              SVC_TRAMP_TOTAL > SVC_EGL_GET_FRAME_TIMESTAMPS &&
              SVC_TRAMP_TOTAL > SVC_EGL_GET_COMPOSITOR_TIMING &&
              SVC_TRAMP_TOTAL > SVC_ARC4RANDOM &&
              SVC_TRAMP_TOTAL > SVC_MALLINFO &&
              SVC_TRAMP_TOTAL > SVC_SIGNALFD &&
              SVC_TRAMP_TOTAL > SVC_SIGPROCMASK &&
              SVC_TRAMP_TOTAL > SVC_GL_GET_QUERY_OBJECT_UIV &&
              SVC_TRAMP_TOTAL > SVC_FLOCK &&
              SVC_TRAMP_TOTAL > SVC_GETPWUID_R &&
              SVC_TRAMP_TOTAL > SVC_HONEST_LAST &&
              SVC_TRAMP_TOTAL > SVC_UE_HOOK_LAST,
              "SVC_TRAMP_TOTAL must bound every trampolined SVC");

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
