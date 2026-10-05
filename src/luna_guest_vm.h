/* MPL-2.0. Host-granule operations for a sorted, disjoint guest page table. */
#ifndef LUNA_GUEST_VM_H
#define LUNA_GUEST_VM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct luna_guest_mapping {
    uint64_t lo, hi;
    uint32_t prot;
    bool owned, file_backed;
};
typedef int (*luna_vm_host_operation)(void *, size_t, int);
typedef int (*luna_vm_host_release)(void *, size_t);

/* Linux siginfo fault codes: 0 succeeds, 1 means an unmapped address,
 * 2 means a mapped address without the requested permissions. */
static inline int luna_vm_access_fault(const struct luna_guest_mapping *maps,
    size_t count, uint64_t lo, size_t length, uint32_t required, uint64_t *fault)
{
    uint64_t end = lo + length;
    size_t left = 0, right = count;
    *fault = lo;
    if (!length || end < lo) return 1;
    while (left < right) {
        size_t middle = left + (right - left) / 2;
        if (maps[middle].hi <= lo) left = middle + 1;
        else right = middle;
    }
    while (lo < end) {
        const struct luna_guest_mapping *m;
        *fault = lo;
        if (left == count || maps[left].lo > lo) return 1;
        m = maps + left;
        if ((m->prot & required) != required) return 2;
        lo = m->hi < end ? m->hi : end;
        ++left;
    }
    return 0;
}

/* Anonymous MAP_FIXED may reuse backing only when every byte belongs to
 * anonymous memory. Zeroing a shared file mapping would alter the file. */
static inline bool luna_vm_anonymous_span(const struct luna_guest_mapping *maps,
    size_t count, uint64_t lo, uint64_t hi)
{
    size_t i;
    if (hi <= lo) return false;
    for (i = 0; i < count && lo < hi; ++i) {
        const struct luna_guest_mapping *m = maps + i;
        if (m->hi <= lo) continue;
        if (m->lo > lo || m->file_backed) return false;
        lo = m->hi < hi ? m->hi : hi;
    }
    return lo == hi;
}

static inline uint32_t luna_vm_edge_prot(const struct luna_guest_mapping *maps,
    size_t count, uint64_t page, uint64_t page_size,
    uint64_t lo, uint64_t hi, uint32_t prot)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        const struct luna_guest_mapping *m = maps + i;
        if (m->hi <= page) continue;
        if (m->lo >= page + page_size) break;
        if ((m->lo < lo && m->hi > page) ||
            (m->hi > hi && m->lo < page + page_size)) prot |= m->prot;
    }
    return prot;
}

static inline int luna_vm_protect_span(const struct luna_guest_mapping *maps,
    size_t count, uint64_t begin, uint64_t end, uint32_t prot,
    uint64_t page_size, luna_vm_host_operation protect)
{
    /* Guest instructions are data to the translator, so EXEC needs host READ,
     * rather than native executable memory. Anonymous backing remains RW for
     * the memory manager's MAP_FIXED commit/zeroing below guest permissions. */
    int host_prot = (prot & 5u) ? 1 : 0;
    size_t i;
    if (prot & 2u) host_prot |= 2;
    if (page_size > 4096u) {
        for (i = 0; i < count; ++i) {
            const struct luna_guest_mapping *m = maps + i;
            if (m->hi <= begin) continue;
            if (m->lo >= end) break;
            if (!m->file_backed) { host_prot |= 3; break; }
        }
    }
    return protect((void *)(uintptr_t)begin, (size_t)(end - begin), host_prot);
}

/* Enforce the union of guest subpage rights on each host page. At most three
 * native calls cover the two edge granules and the complete interior span. */
static inline int luna_vm_protect_host(const struct luna_guest_mapping *maps,
    size_t count, uint64_t lo, uint64_t hi, uint32_t prot,
    uint64_t page_size, luna_vm_host_operation protect)
{
    uint64_t first = lo & ~(page_size - 1);
    uint64_t end = (hi + page_size - 1) & ~(page_size - 1);
    uint32_t edge = luna_vm_edge_prot(maps, count, first, page_size, lo, hi, prot);
    if (end - first == page_size)
        return luna_vm_protect_span(maps, count, first, end, edge, page_size, protect);
    if (luna_vm_protect_span(maps, count, first, first + page_size,
                             edge, page_size, protect)) return -1;
    if (end - first > 2 * page_size &&
        luna_vm_protect_span(maps, count, first + page_size, end - page_size,
                             prot, page_size, protect)) return -1;
    edge = luna_vm_edge_prot(maps, count, end - page_size, page_size, lo, hi, prot);
    return luna_vm_protect_span(maps, count, end - page_size, end,
                                edge, page_size, protect);
}

/* The caller has already removed the guest ranges. A physical page survives
 * until no remaining guest mapping intersects it; release empty spans in bulk. */
static inline void luna_vm_release_empty(const struct luna_guest_mapping *maps,
    size_t count, uint64_t begin, uint64_t end, uint64_t page_size,
    luna_vm_host_release release)
{
    while (begin < end) {
        size_t left = 0, right = count;
        while (left < right) {
            size_t middle = left + (right - left) / 2;
            if (maps[middle].hi <= begin) left = middle + 1;
            else right = middle;
        }
        if (left == count || maps[left].lo >= end) {
            release((void *)(uintptr_t)begin, (size_t)(end - begin));
            return;
        }
        {
            uint64_t stop = maps[left].lo & ~(page_size - 1);
            if (stop > begin)
                release((void *)(uintptr_t)begin, (size_t)(stop - begin));
            begin = (maps[left].hi + page_size - 1) & ~(page_size - 1);
        }
    }
}
#endif
