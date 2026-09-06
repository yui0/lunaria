/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The host operating system, as this emulator uses it.
 *
 * Everything below is a thing only the host kernel can answer: reserving a
 * range of the process's own address space at a chosen location, handing out
 * an anonymous shared-memory object, turning a descriptor back into the path
 * it was opened from, telling the truth about how much memory the machine
 * has.  The emulator's own logic — the guest's page tables, its file
 * descriptors, its scheduler — is not here and must not move here: those are
 * the same on every host, and the day one of them starts differing per
 * platform is the day the emulator has stopped being one emulator.
 *
 * Implemented once per platform: lunaria_linux.c, lunaria_windows.c,
 * lunaria_mac.c.  The Makefile picks by uname; a build for one host never
 * compiles another's file, so the three are free to include whatever their
 * own platform needs.
 *
 * C, not C++: the emulator is being taken to C, and this is the layer that
 * has to compile first.
 */

#ifndef LUNARIA_OS_H
#define LUNARIA_OS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- virtual memory ----------------------------------------------------
 *
 * The guest's address space is the host's: a guest VA is a host VA, so the
 * emulator has to be able to take a specific range of its own address space
 * and to be told when it cannot.  That last part is what `exact` is for —
 * "give me this address or fail", never "give me something nearby", because
 * something nearby is a guest pointer that no longer means what it says. */

#define LUNA_PROT_NONE   0
#define LUNA_PROT_READ   1
#define LUNA_PROT_WRITE  2
#define LUNA_PROT_EXEC   4

/* Reserve `len` bytes of readable/writable anonymous space.  `want` is the
 * address asked for (NULL: anywhere).  With `exact` set the call fails rather
 * than relocating.  Pages are not committed until touched, which is what
 * makes a multi-gigabyte guest heap cost nothing until the guest uses it.
 * Returns NULL on failure. */
void *luna_os_reserve(void *want, size_t len, int exact);

/* Give a range back.  Safe on a sub-range of an earlier reservation. */
int luna_os_release(void *addr, size_t len);

/* Change protection on a range.  `prot` is a mask of LUNA_PROT_*. */
int luna_os_protect(void *addr, size_t len, int prot);

/* Map `len` bytes of `fd` at `off` — the emulator's file-backed guest
 * mappings, so the page cache is shared with every other reader of the file
 * instead of being copied into the guest's heap.  `fixed` demands `want`. */
void *luna_os_map_file(void *want, size_t len, int prot, int fd,
                       uint64_t off, int fixed);

/* Grow or shrink an existing anonymous mapping in place where the host can,
 * and otherwise report that it cannot (returns NULL, leaving the old mapping
 * untouched) so the caller can copy.  Only Linux has the in-place form. */
void *luna_os_remap(void *addr, size_t old_len, size_t new_len);

size_t luna_os_page_size(void);

/* ---- anonymous shared memory -------------------------------------------
 *
 * ASharedMemory_create: a nameless, resizable, mappable object identified by
 * a descriptor.  The name is a debugging label on every platform, not an
 * identity — two calls with the same name are two different objects. */
int luna_os_shm_create(const char *name, size_t size);

/* ---- descriptors --------------------------------------------------------
 *
 * A guest that has a descriptor and wants to know what it was opened from
 * (Mono's file maps, the guest's own /proc/self/fd emulation).  Writes a NUL
 * terminated path and returns 0, or returns -1 when the host cannot say. */
int luna_os_fd_path(int fd, char *buf, size_t bufsz);

/* Absolute path of the running executable.  Runtime libraries are located
 * relative to this, rather than relative to a process working directory that
 * a launcher or Finder is free to choose. */
int luna_os_executable_path(char *buf, size_t bufsz);

/* True when a dynamic-loader address belongs to executable code.  Guest
 * calls need an ABI bridge; host data symbols must never receive one. */
int luna_os_symbol_is_function(void *address);

/* A descriptor that becomes readable when something signals it — the object
 * behind ALooper_wake and the guest's own eventfd.  Counting semantics:
 * every write adds, a read drains.  Returns -1 on failure. */
int luna_os_event_open(unsigned initval, int nonblock);
int luna_os_event_signal(int fd, uint64_t count);
int luna_os_event_drain(int fd, uint64_t *out);

/* epoll's guest-visible contract, backed by epoll on Linux and kqueue on
 * macOS.  Event bits deliberately use Linux/Android's numeric values because
 * they are copied to and from the guest ABI unchanged. */
typedef struct luna_os_poll_event {
   uint32_t events;
   uint64_t data;
} luna_os_poll_event;

int luna_os_poll_create(int cloexec);
int luna_os_poll_ctl(int pollfd, int op, int fd,
                     const luna_os_poll_event *event);
int luna_os_poll_wait(int pollfd, luna_os_poll_event *events,
                      int max_events, int timeout_ms);

/* signalfd where the host has it.  `mask` is copied into the host's sigset_t;
 * unsupported hosts return -1/ENOSYS, never a descriptor that cannot deliver
 * the records read() promises. */
int luna_os_signal_fd(int fd, const void *mask, size_t mask_size, int flags);

/* ---- threads and CPUs ---------------------------------------------------
 *
 * How many CPUs the *host* has, which is what sizes the engine pool.  What
 * the guest is told it has is a separate number the emulator decides. */
int luna_os_cpu_count(void);

/* Name the calling thread, for the host's own debuggers and profilers. */
void luna_os_thread_name(const char *name);

/* Give up the rest of this thread's slice. */
void luna_os_yield(void);

/* ---- time ---------------------------------------------------------------
 *
 * Monotonic is the emulator's own clock: slices, frame budgets, timeouts.
 * Realtime is the wall clock the guest asks for. */
uint64_t luna_os_monotonic_ns(void);
uint64_t luna_os_realtime_ns(void);

/* ---- what the machine has ----------------------------------------------
 *
 * The guest sizes its caches and its texture pools from these, so a wrong
 * answer here is a guest that either thrashes or reserves memory the host
 * does not have.  Both return 0 on success. */
int luna_os_mem_info(uint64_t *total_bytes, uint64_t *avail_bytes);
int luna_os_disk_info(const char *path, uint64_t *total_bytes,
                      uint64_t *free_bytes, uint64_t *block_size);

/* ---- the window ---------------------------------------------------------
 *
 * The platform handles behind the GLFW window, for the two things GLFW does
 * not cover: the system input method and clipboard round-trips that need the
 * window's own event queue.  `glfw_window` is the GLFWwindow*; both return
 * NULL when the platform has no such handle. */
void *luna_os_native_display(void);
void *luna_os_native_window(void *glfw_window);

/* ---- audio out ----------------------------------------------------------
 *
 * One PCM playback stream, which is what the guest's OpenSL ES / AAudio
 * buffer queue turns into.  16-bit signed interleaved, because that is what
 * an Android audio track carries and what every mixer in the guest already
 * produces.
 *
 * The write is *not* allowed to block: it is called from the SVC that the
 * guest's audio thread is inside, and that thread also holds the emulator's
 * execution lock.  So the platform keeps a small ring and a thread of its own
 * to hand it to the device; a write that finds the ring full drops the
 * newest frames and says how many it took, which is a glitch — the honest
 * outcome when the guest produces faster than the card consumes.
 *
 * Returns: open 0 on success (the stream is silent but harmless otherwise),
 * write the number of frames accepted, queued_frames what is still to play. */
int      luna_os_audio_open(unsigned rate, unsigned channels);
int      luna_os_audio_write(const void *pcm16, unsigned frames);
unsigned luna_os_audio_queued_frames(void);
void     luna_os_audio_close(void);

#ifdef __cplusplus
}
#endif

#endif /* LUNARIA_OS_H */
