/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * lunaria_os.h on macOS.  See that header for what each entry point owes its
 * caller; this file only says how macOS provides it.
 *
 * Two places where macOS is not Linux and the difference is visible to the
 * rest of the emulator, so they are stated here rather than hidden:
 *
 *   - There is no MAP_FIXED_NOREPLACE.  MAP_FIXED exists but takes the range
 *     by destroying whatever was already there, which for an emulator whose
 *     guest VA is the host VA is the worst possible outcome.  So an exact
 *     reservation asks without MAP_FIXED and checks what it was given: the
 *     kernel treats the address as a hint and answers elsewhere when the
 *     range is taken, and "elsewhere" is what tells us to fail.
 *
 *   - There is no mremap.  luna_os_remap always reports that it cannot grow
 *     in place, and the caller copies.
 */

#include "lunaria_os.h"

#include <locale.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_host.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <execinfo.h>

int luna_os_setenv(const char *name, const char *value, int overwrite)
{
   if (!name || !*name || strchr(name, '=') || !value) { errno = EINVAL; return -1; }
   return setenv(name, value, overwrite);
}
int luna_os_random(void *buffer, size_t length)
{
   if (length && !buffer) { errno = EFAULT; return -1; }
   if (length) arc4random_buf(buffer, length);
   return 0;
}

void *luna_os_library_open(const char *path)
{
   return dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
}

void *luna_os_library_open_local(const char *path)
{
   return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
const char *luna_os_library_error(void)
{
   const char *error = dlerror();
   return error ? error : "unknown loader error";
}

void *luna_os_library_symbol(void *handle, const char *name)
{
   return dlsym(handle ? handle : RTLD_DEFAULT, name);
}

void luna_os_library_close(void *handle)
{
   if (handle) dlclose(handle);
}

int luna_os_backtrace(void **frames, int capacity)
{
   return frames && capacity > 0 ? backtrace(frames, capacity) : 0;
}

void luna_os_backtrace_print(void *const *frames, int count)
{
   if (frames && count > 0) backtrace_symbols_fd(frames, count, 2);
}
#include <sys/mman.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

/* Linux reports a broken pipe writer as ERR, rather than Darwin's HUP.
 * Inspect only exceptional descriptors; the usual ready path stays native. */
int luna_socket_poll(struct pollfd *fds, size_t n, int ms)
{
   if (n > UINT32_MAX || n > SIZE_MAX / sizeof(struct pollfd)) {
      errno = EINVAL; return -1;
   }
   if (n && !fds) { errno = EFAULT; return -1; }
   struct pollfd small[64];
   struct pollfd *host = n <= 64 ? small : malloc(n * sizeof *host);
   if (!host) { errno = ENOMEM; return -1; }
   for (size_t i = 0; i < n; ++i) {
      host[i] = fds[i];
      /* Darwin needs HUP requested even for events=0. Linux always reports it. */
      host[i].events |= POLLHUP;
   }
   int result = poll(host, n, ms);
   const int saved_errno = errno;
   if (result >= 0) for (size_t i = 0; i < n; ++i) {
      short events = host[i].revents;
      if (events & POLLHUP) {
         struct stat info;
         int mode = fcntl(host[i].fd, F_GETFL);
         if (mode >= 0 && (mode & O_ACCMODE) == O_WRONLY &&
             !fstat(host[i].fd, &info) && S_ISFIFO(info.st_mode))
            events = (events & ~POLLHUP) | POLLERR;
      }
      fds[i].revents = events;
   }
   if (host != small) free(host);
   errno = saved_errno;
   return result;
}

/* Linux drops the old flock when a conversion conflicts. Darwin keeps it.
 * Probe nonblocking first so an upgrade never sleeps holding its shared lock.
 * Repeating an existing lock succeeds without dropping it or opening a race. */
int luna_file_flock(int fd, int operation)
{
   int kind = operation & ~LUNA_LOCK_NONBLOCK;
   if (kind != LUNA_LOCK_SHARED && kind != LUNA_LOCK_EXCLUSIVE &&
       kind != LUNA_LOCK_UNLOCK) { errno = EINVAL; return -1; }
   int native = kind == LUNA_LOCK_SHARED ? LOCK_SH :
                kind == LUNA_LOCK_EXCLUSIVE ? LOCK_EX : LOCK_UN;
   if (kind == LUNA_LOCK_UNLOCK) return flock(fd, LOCK_UN);
   int result = flock(fd, native | LOCK_NB);
   if (!result || (errno != EWOULDBLOCK && errno != EAGAIN)) return result;
   const int error = errno;
   if (flock(fd, LOCK_UN)) return -1;
   if (operation & LUNA_LOCK_NONBLOCK) { errno = error; return -1; }
   return flock(fd, native);
}

/* ---- virtual memory ---------------------------------------------------- */

static int luna_prot_to_host(int prot)
{
   int p = 0;
   if (prot & LUNA_PROT_READ)  p |= PROT_READ;
   if (prot & LUNA_PROT_WRITE) p |= PROT_WRITE;
   if (prot & LUNA_PROT_EXEC)  p |= PROT_EXEC;
   return p ? p : PROT_NONE;
}

void *luna_os_reserve(void *want, size_t len, int exact)
{
   int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
   flags |= MAP_NORESERVE;   /* accepted and ignored here; harmless */
#endif
   if (exact && !want) return NULL;
   void *p = mmap(want, len, PROT_READ | PROT_WRITE, flags, -1, 0);
   if (p == MAP_FAILED) return NULL;
   if (exact && p != want) { munmap(p, len); return NULL; }
   return p;
}

int luna_os_release(void *addr, size_t len)
{
   return munmap(addr, len);
}

void *luna_os_map_anon(void *want, size_t len, int replace)
{
   if (replace && !want) { errno = EINVAL; return NULL; }
   int flags = MAP_PRIVATE | MAP_ANON;
   if (replace) flags |= MAP_FIXED;
   void *p = mmap(want, len, PROT_READ | PROT_WRITE, flags, -1, 0);
   return p == MAP_FAILED ? NULL : p;
}

void *luna_os_map_file_flags(void *want, size_t len, int prot, int policy,
                            int fd, uint64_t off)
{
   int flags = (policy & LUNA_MAP_SHARED) ? MAP_SHARED : MAP_PRIVATE;
   if (policy & LUNA_MAP_FIXED) flags |= MAP_FIXED;
   if ((policy & (LUNA_MAP_FIXED | LUNA_MAP_NOREPLACE)) && !want) {
      errno = EINVAL;
      return NULL;
   }
   void *p = mmap(want, len, luna_prot_to_host(prot), flags, fd, (off_t)off);
   if (p == MAP_FAILED) return NULL;
   if ((policy & LUNA_MAP_NOREPLACE) && p != want) {
      munmap(p, len);
      errno = EEXIST;
      return NULL;
   }
   return p;
}

int luna_os_protect(void *addr, size_t len, int prot)
{
   return mprotect(addr, len, luna_prot_to_host(prot));
}

void *luna_os_map_file(void *want, size_t len, int prot, int fd,
                       uint64_t off, int fixed)
{
   if (fixed && !want) return NULL;
   void *p = mmap(want, len, luna_prot_to_host(prot), MAP_PRIVATE, fd,
                  (off_t)off);
   if (p == MAP_FAILED) return NULL;
   if (fixed && p != want) { munmap(p, len); return NULL; }
   return p;
}

void *luna_os_remap(void *addr, size_t old_len, size_t new_len)
{
   (void)addr; (void)old_len; (void)new_len;
   return NULL;  /* no in-place resize on macOS; the caller copies */
}

size_t luna_os_page_size(void)
{
   long v = sysconf(_SC_PAGESIZE);
   return v > 0 ? (size_t)v : 4096u;
}

/* Validate mapped host ranges using Mach rather than a shadow mapping table. */
static int luna_memory_range(void *addr, size_t len, int reject_locked)
{
   const size_t page = luna_os_page_size();
   const uintptr_t start = (uintptr_t)addr;
   if (start % page) { errno = EINVAL; return -1; }
   if (len > UINTPTR_MAX - (page - 1)) { errno = ENOMEM; return -1; }
   const size_t rounded = (len + page - 1) & ~(page - 1);
   if (rounded > UINTPTR_MAX - start) { errno = ENOMEM; return -1; }
   const uintptr_t end = start + rounded;
   /* Query the actual mappings, including locks created outside this wrapper.
    * Darwin does not enforce Linux's INVALIDATE/locked-range exclusion. */
   for (uintptr_t cursor = start; cursor < end;) {
      mach_vm_address_t region = cursor;
      mach_vm_size_t size = 0;
      vm_region_basic_info_data_64_t info;
      mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t object = MACH_PORT_NULL;
      kern_return_t kr = mach_vm_region(mach_task_self(), &region, &size,
          VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
      if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
      if (kr != KERN_SUCCESS || region > cursor || !size) {
         errno = ENOMEM; return -1;
      }
      if (reject_locked && info.user_wired_count) {
         errno = EBUSY; return -1;
      }
      if (size >= end - region) break;
      cursor = (uintptr_t)(region + size);
   }
   return 0;
}

int luna_os_memory_advise(void *addr, size_t len, int advice)
{
   /* Darwin rounds unaligned addresses; the Android API rejects them. */
   if ((uintptr_t)addr % luna_os_page_size()) { errno = EINVAL; return -1; }
   int native;
   switch (advice) {
   case 0: native = MADV_NORMAL; break;
   case 1: native = MADV_RANDOM; break;
   case 2: native = MADV_SEQUENTIAL; break;
   case 3: native = MADV_WILLNEED; break;
   case 4: native = MADV_DONTNEED; break;
   case 8: native = MADV_FREE; break;
   default: errno = EINVAL; return -1;
   }
   int result = madvise(addr, len, native);
   if (result < 0 && errno == EINVAL) {
      /* XNU maps KERN_INVALID_ADDRESS to EINVAL; Linux uses ENOMEM. */
      if (luna_memory_range(addr, len, 0)) return -1;
      errno = EINVAL;
   }
   return result;
}

/* Linux MADV_DONTNEED on private anonymous memory leaves zero pages behind;
 * Darwin's keeps the old bytes (MADV_FREE as well), and allocators such as
 * Scudo and the Mono/il2cpp GC take the zeros for granted when they reuse a
 * range.  The guest sees 4 KiB pages on 16 KiB host pages, so the range need
 * not be host-page aligned: whole host pages inside it are swapped for fresh
 * anonymous pages with the protection they had (which also gives the memory
 * back), and the partial pages at the ends, shared with neighbouring guest
 * pages, are cleared in place.  Only for anonymous guest memory. */
int luna_os_memory_zero(void *addr, size_t len)
{
   if (!len) return 0;
   const uintptr_t page = luna_os_page_size();
   const uintptr_t lo = (uintptr_t)addr;
   if (len > UINTPTR_MAX - lo) { errno = ENOMEM; return -1; }
   const uintptr_t hi = lo + len;
   /* Every host page the range touches must be mapped. */
   const uintptr_t span = lo & ~(page - 1);
   if (luna_memory_range((void *)span, (size_t)(hi - span), 0)) return -1;
   const uintptr_t first = (lo + page - 1) & ~(page - 1);
   const uintptr_t last = hi & ~(page - 1);
   if (first >= last) { memset(addr, 0, len); return 0; }
   if (lo < first) memset((void *)lo, 0, first - lo);
   if (hi > last) memset((void *)last, 0, hi - last);
   for (uintptr_t cursor = first; cursor < last;) {
      mach_vm_address_t region = cursor;
      mach_vm_size_t size = 0;
      vm_region_basic_info_data_64_t info;
      mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t object = MACH_PORT_NULL;
      kern_return_t kr = mach_vm_region(mach_task_self(), &region, &size,
          VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
      if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
      if (kr != KERN_SUCCESS || region > cursor || !size) { errno = ENOMEM; return -1; }
      uintptr_t stop = (uintptr_t)(region + size);
      if (stop > last) stop = last;
      int prot = (info.protection & VM_PROT_READ ? PROT_READ : 0) |
                 (info.protection & VM_PROT_WRITE ? PROT_WRITE : 0);
      if (!(info.protection & VM_PROT_WRITE)) {
         /* Zero pages are what the caller asked for; a read-only range can
          * only be replaced, which keeps the protection it had. */
      }
      if (mmap((void *)cursor, stop - cursor, prot,
               MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) == MAP_FAILED)
         return -1;
      cursor = stop;
   }
   return 0;
}

int luna_os_memory_sync(void *addr, size_t len, int flags)
{
   if ((flags & ~(LUNA_MS_ASYNC | LUNA_MS_INVALIDATE | LUNA_MS_SYNC)) ||
       (flags & LUNA_MS_ASYNC && flags & LUNA_MS_SYNC)) { errno = EINVAL; return -1; }
   if (luna_memory_range(addr, len, flags & LUNA_MS_INVALIDATE)) return -1;
   if (!len) return 0;
   int native = (flags & LUNA_MS_ASYNC ? MS_ASYNC : 0) |
                (flags & LUNA_MS_INVALIDATE ? MS_INVALIDATE : 0) |
                (flags & LUNA_MS_SYNC ? MS_SYNC : 0);
   return msync(addr, len, native);
}

int luna_os_memory_lock(void *addr, size_t len, int unlock)
{
   return unlock ? munlock(addr, len) : mlock(addr, len);
}

int luna_os_residency(void *addr, size_t len, unsigned char *vector)
{
   if (luna_memory_range(addr, len, 0)) return -1;
   int result = mincore((const void *)addr, len, (char *)vector);
   if (!result) {
      const size_t page = luna_os_page_size();
      size_t count = len / page + (len % page != 0);
      for (size_t i = 0; i < count; ++i) vector[i] &= 1;
   }
   return result;
}

/* ---- anonymous shared memory ------------------------------------------- */

int luna_os_shm_create(const char *name, size_t size)
{
   /* POSIX shared memory is the closest thing to a memfd here.  The name is
    * unlinked as soon as it is open, so what is left is exactly what
    * ASharedMemory hands out: an object with no name, reachable only through
    * the descriptor. */
   char path[64];
   static unsigned seq;
   int fd;
   snprintf(path, sizeof path, "/lunaria-%s-%u-%u",
            name && *name ? name : "shmem", (unsigned)getpid(), seq++);
   fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
   if (fd < 0) return -1;
   shm_unlink(path);
   if (size && ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
   return fd;
}

/* ---- descriptors -------------------------------------------------------- */

int luna_os_fd_path(int fd, char *buf, size_t bufsz)
{
   char full[PATH_MAX];
   if (fd < 0 || !buf || bufsz < 2) return -1;
   if (fcntl(fd, F_GETPATH, full) != 0) return -1;
   if (strlen(full) >= bufsz) return -1;
   strcpy(buf, full);
   return 0;
}

int luna_os_executable_path(char *buf, size_t bufsz)
{
   if (!buf || bufsz < 2) { errno = EINVAL; return -1; }
   uint32_t n = (uint32_t)bufsz;
   if (_NSGetExecutablePath(buf, &n) != 0) {
      errno = ENAMETOOLONG;
      return -1;
   }
   /* _NSGetExecutablePath may contain symlinks or relative components.  The
    * decoder directory belongs beside the real executable, not beside an
    * alias through which Finder happened to launch it. */
   char resolved[PATH_MAX];
   if (!realpath(buf, resolved)) return -1;
   size_t len = strlen(resolved);
   if (len >= bufsz) { errno = ENAMETOOLONG; return -1; }
   memcpy(buf, resolved, len + 1);
   return 0;
}

int luna_os_symbol_is_function(void *address)
{
   Dl_info di;
   if (!address || dladdr(address, &di) == 0 || !di.dli_fbase) return 0;
   const struct mach_header_64 *mh = (const struct mach_header_64 *)di.dli_fbase;
   if (mh->magic != MH_MAGIC_64) return 0;

   const struct symtab_command *symtab = NULL;
   const struct segment_command_64 *linkedit = NULL;
   const uint8_t *cmdp = (const uint8_t *)(mh + 1);
   intptr_t slide = 0;
   for (uint32_t i = 0; i < mh->ncmds; ++i) {
      const struct load_command *lc = (const struct load_command *)cmdp;
      if (lc->cmd == LC_SYMTAB) symtab = (const struct symtab_command *)lc;
      if (lc->cmd == LC_SEGMENT_64) {
         const struct segment_command_64 *seg =
            (const struct segment_command_64 *)lc;
         if (!strcmp(seg->segname, SEG_TEXT))
            slide = (intptr_t)mh - (intptr_t)seg->vmaddr;
         if (!strcmp(seg->segname, SEG_LINKEDIT)) linkedit = seg;
      }
      cmdp += lc->cmdsize;
   }
   if (!symtab || !linkedit) return 0;
   const uint8_t *base = (const uint8_t *)(slide + linkedit->vmaddr -
                                           linkedit->fileoff);
   const struct nlist_64 *symbols =
      (const struct nlist_64 *)(base + symtab->symoff);
   uintptr_t wanted = (uintptr_t)address;
   for (uint32_t i = 0; i < symtab->nsyms; ++i) {
      const struct nlist_64 *n = &symbols[i];
      if ((n->n_type & N_TYPE) != N_SECT ||
          (uintptr_t)(n->n_value + slide) != wanted || !n->n_sect)
         continue;
      uint8_t sect_index = 1;
      cmdp = (const uint8_t *)(mh + 1);
      for (uint32_t c = 0; c < mh->ncmds; ++c) {
         const struct load_command *lc = (const struct load_command *)cmdp;
         if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg =
               (const struct segment_command_64 *)lc;
            const struct section_64 *sec = (const struct section_64 *)(seg + 1);
            for (uint32_t j = 0; j < seg->nsects; ++j, ++sect_index)
               if (sect_index == n->n_sect)
                  return (sec[j].flags & (S_ATTR_PURE_INSTRUCTIONS |
                                           S_ATTR_SOME_INSTRUCTIONS)) != 0;
         }
         cmdp += lc->cmdsize;
      }
   }
   return 0;
}

/* An eventfd stands in as a pipe.  The pipe makes the descriptor readable;
 * the count lives beside it, so a drain reports the total the way eventfd
 * does rather than one byte per signal. */
#define LUNA_EVENT_MAX 64
static struct {
   _Atomic int rd, wr;
   _Atomic uint64_t count;
} g_events[LUNA_EVENT_MAX];

static _Atomic int g_event_live;   /* open event/timer descriptors */

static int luna_event_slot(int fd)
{
   if (fd < 0 || !atomic_load_explicit(&g_event_live, memory_order_acquire))
      return -1;
   for (int i = 0; i < LUNA_EVENT_MAX; ++i)
      if (atomic_load_explicit(&g_events[i].rd, memory_order_acquire) == fd)
         return i;
   return -1;
}

int luna_os_event_open(unsigned initval, int nonblock)
{
   int fds[2], slot = -1;
   for (int i = 0; i < LUNA_EVENT_MAX; ++i) {
      int free_slot = 0;
      if (atomic_compare_exchange_strong_explicit(
             &g_events[i].rd, &free_slot, -1,
             memory_order_acq_rel, memory_order_relaxed)) {
         slot = i;
         break;
      }
   }
   if (slot < 0) return -1;
   if (pipe(fds) != 0) {
      atomic_store_explicit(&g_events[slot].rd, 0, memory_order_release);
      return -1;
   }
   fcntl(fds[0], F_SETFD, FD_CLOEXEC);
   fcntl(fds[1], F_SETFD, FD_CLOEXEC);
   if (nonblock) fcntl(fds[0], F_SETFL, O_NONBLOCK);
   fcntl(fds[1], F_SETFL, O_NONBLOCK);
   atomic_store_explicit(&g_events[slot].wr, fds[1], memory_order_relaxed);
   atomic_store_explicit(&g_events[slot].count, 0, memory_order_relaxed);
   atomic_store_explicit(&g_events[slot].rd, fds[0], memory_order_release);
   atomic_fetch_add_explicit(&g_event_live, 1, memory_order_acq_rel);
   if (initval) luna_os_event_signal(fds[0], initval);
   return fds[0];
}

int luna_os_event_signal(int fd, uint64_t count)
{
   int i = luna_event_slot(fd);
   char b = 1;
   if (i < 0 || !count) return i < 0 ? -1 : 0;
   atomic_fetch_add_explicit(&g_events[i].count, count, memory_order_release);
   const int wr = atomic_load_explicit(&g_events[i].wr, memory_order_acquire);
   if (write(wr, &b, 1) == 1 || errno == EAGAIN) return 0;
   /* Keep the count on a hard pipe error.  A concurrent drain may already
    * have observed it, so rolling it back here can underflow the shared
    * counter.  The broken wake descriptor is itself the error; eventfd's
    * accumulated value must still remain monotonic. */
   return -1;
}

int luna_os_event_drain(int fd, uint64_t *out)
{
   int i = luna_event_slot(fd);
   char b[64];
   if (i < 0) return -1;
   /* A blocking pipe would sleep in read() once emptied; only read what the
    * descriptor says is there. */
   for (struct pollfd pfd = { fd, POLLIN, 0 };
        poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);)
      if (read(fd, b, sizeof b) <= 0) break;
   const uint64_t count = atomic_exchange_explicit(
      &g_events[i].count, 0, memory_order_acq_rel);
   if (!count) { errno = EAGAIN; return -1; }
   if (out) *out = count;
   return 0;
}


/* eventfd and timerfd read(): an 8-byte native-endian counter, never the
 * pipe's wake bytes.  Returns the byte count, or -1 with errno set; a
 * descriptor this layer does not own is read as an ordinary file.  A blocking
 * descriptor waits for the counter to become non-zero. */
ptrdiff_t luna_fd_read(int fd, void *buffer, size_t length)
{
   if (luna_event_slot(fd) < 0) return read(fd, buffer, length);
   if (length < sizeof(uint64_t)) { errno = EINVAL; return -1; }
   const int mode = fcntl(fd, F_GETFL);
   for (;;) {
      uint64_t count = 0;
      if (!luna_os_event_drain(fd, &count)) {
         memcpy(buffer, &count, sizeof count);
         return (ptrdiff_t)sizeof count;
      }
      if (errno != EAGAIN || (mode >= 0 && (mode & O_NONBLOCK))) return -1;
      struct pollfd pfd = { fd, POLLIN, 0 };
      if (poll(&pfd, 1, -1) < 0 && errno != EINTR) return -1;
   }
}

/* ---- timerfd -------------------------------------------------------------
 *
 * Darwin has none.  A timer is an event descriptor whose counter a single
 * lazily started thread advances: that gives the guest the descriptor
 * Linux does (readable on expiry, an expiration count, poll/epoll-able)
 * without a thread per timer. */
enum { LUNA_TFD_ABSTIME = 1, LUNA_TFD_NONBLOCK = 0x800 };
static struct luna_timer {
   int armed, clock;               /* clock: 0 realtime, else monotonic */
   int64_t next_ns, interval_ns;   /* next_ns is on that clock */
} g_timers[LUNA_EVENT_MAX];
static unsigned char g_is_timer[LUNA_EVENT_MAX];
static pthread_mutex_t g_timer_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_timer_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_timer_once = PTHREAD_ONCE_INIT;

static int64_t luna_timer_now(int clock)
{
   struct timespec ts;
   clock_gettime(clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void *luna_timer_thread(void *unused)
{
   (void)unused;
   pthread_mutex_lock(&g_timer_mu);
   for (;;) {
      int64_t wait_ns = INT64_MAX;
      for (int i = 0; i < LUNA_EVENT_MAX; ++i) {
         struct luna_timer *t = &g_timers[i];
         if (!t->armed) continue;
         const int64_t now = luna_timer_now(t->clock);
         int64_t due = t->next_ns - now;
         if (due <= 0) {
            uint64_t n = 1;
            if (t->interval_ns > 0) {
               n += (uint64_t)(-due / t->interval_ns);
               t->next_ns += (int64_t)n * t->interval_ns;
               due = t->next_ns - now;
            } else {
               t->armed = 0;
               due = INT64_MAX;
            }
            (void)luna_os_event_signal(
               atomic_load_explicit(&g_events[i].rd, memory_order_acquire), n);
         }
         if (t->armed && due < wait_ns) wait_ns = due;
      }
      if (wait_ns == INT64_MAX) {
         pthread_cond_wait(&g_timer_cv, &g_timer_mu);
      } else {
         struct timespec rel = { (time_t)(wait_ns / 1000000000ll),
                                 (long)(wait_ns % 1000000000ll) };
         pthread_cond_timedwait_relative_np(&g_timer_cv, &g_timer_mu, &rel);
      }
   }
   return NULL;
}

static void luna_timer_start(void)
{
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_create(&thread, &attr, luna_timer_thread, NULL);
   pthread_attr_destroy(&attr);
}

int luna_os_timer_open(int clock_id, int flags)
{
   /* CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME (Linux numbering) */
   if (clock_id != 0 && clock_id != 1 && clock_id != 7) { errno = EINVAL; return -1; }
   if (flags & ~(LUNA_TFD_NONBLOCK | 0x80000)) { errno = EINVAL; return -1; }
   const int fd = luna_os_event_open(0, (flags & LUNA_TFD_NONBLOCK) != 0);
   if (fd < 0) return -1;
   const int slot = luna_event_slot(fd);
   pthread_mutex_lock(&g_timer_mu);
   g_timers[slot] = (struct luna_timer){ 0, clock_id == 0 ? 0 : 1, 0, 0 };
   g_is_timer[slot] = 1;
   pthread_mutex_unlock(&g_timer_mu);
   return fd;
}

static int luna_timer_slot(int fd)
{
   const int i = luna_event_slot(fd);
   if (i < 0 || !g_is_timer[i]) { errno = EINVAL; return -1; }
   return i;
}

static void luna_timer_remaining(const struct luna_timer *t,
                                 luna_os_timer_spec *out)
{
   int64_t left = 0;
   if (t->armed) {
      left = t->next_ns - luna_timer_now(t->clock);
      if (left < 1) left = 1;      /* armed but already due: still pending */
   }
   out->value_sec = left / 1000000000ll;
   out->value_nsec = left % 1000000000ll;
   out->interval_sec = t->interval_ns / 1000000000ll;
   out->interval_nsec = t->interval_ns % 1000000000ll;
}

static int luna_timer_ns(int64_t sec, int64_t nsec, int64_t *out)
{
   if (sec < 0 || nsec < 0 || nsec >= 1000000000ll) return -1;
   *out = sec > INT64_MAX / 2000000000ll ? INT64_MAX / 2
                                         : sec * 1000000000ll + nsec;
   return 0;
}

int luna_os_timer_set(int fd, int flags, const luna_os_timer_spec *value,
                      luna_os_timer_spec *previous)
{
   if (!value || (flags & ~LUNA_TFD_ABSTIME)) { errno = EINVAL; return -1; }
   const int i = luna_timer_slot(fd);
   if (i < 0) return -1;
   int64_t first, interval;
   if (luna_timer_ns(value->value_sec, value->value_nsec, &first) ||
       luna_timer_ns(value->interval_sec, value->interval_nsec, &interval)) {
      errno = EINVAL; return -1;
   }
   pthread_once(&g_timer_once, luna_timer_start);
   pthread_mutex_lock(&g_timer_mu);
   struct luna_timer *t = &g_timers[i];
   if (previous) luna_timer_remaining(t, previous);
   t->armed = first != 0;
   t->interval_ns = interval;
   t->next_ns = (flags & LUNA_TFD_ABSTIME) ? first
                                           : luna_timer_now(t->clock) + first;
   /* Re-arming forgets expirations nobody has read yet, as Linux does. */
   (void)luna_os_event_drain(fd, NULL);
   pthread_cond_signal(&g_timer_cv);
   pthread_mutex_unlock(&g_timer_mu);
   return 0;
}

int luna_os_timer_get(int fd, luna_os_timer_spec *value)
{
   if (!value) { errno = EINVAL; return -1; }
   const int i = luna_timer_slot(fd);
   if (i < 0) return -1;
   pthread_mutex_lock(&g_timer_mu);
   luna_timer_remaining(&g_timers[i], value);
   pthread_mutex_unlock(&g_timer_mu);
   return 0;
}

/* close() for descriptors that may be an event or timer: give the slot back
 * only once both pipe ends are closed, so a reused fd number is never taken
 * for ours. */
int luna_fd_close(int fd)
{
   const int i = luna_event_slot(fd);
   if (i < 0) return close(fd);
   pthread_mutex_lock(&g_timer_mu);
   g_timers[i].armed = 0;
   g_is_timer[i] = 0;
   pthread_mutex_unlock(&g_timer_mu);
   const int wr = atomic_load_explicit(&g_events[i].wr, memory_order_acquire);
   atomic_store_explicit(&g_events[i].rd, -1, memory_order_release);
   if (wr >= 0) close(wr);
   const int result = close(fd);
   atomic_store_explicit(&g_events[i].rd, 0, memory_order_release);
   atomic_fetch_sub_explicit(&g_event_live, 1, memory_order_acq_rel);
   return result;
}

/* Android epoll bits and operations.  Keeping them local avoids importing a
 * Linux header into a macOS translation unit while preserving the guest ABI. */
enum {
   LUNA_EPOLLIN = 0x001, LUNA_EPOLLOUT = 0x004,
   LUNA_EPOLLERR = 0x008, LUNA_EPOLLHUP = 0x010,
   LUNA_EPOLLRDHUP = 0x2000,
   LUNA_EPOLLONESHOT = (int)0x40000000u,
   LUNA_EPOLLET = (int)0x80000000u,
   LUNA_EPOLL_CTL_ADD = 1, LUNA_EPOLL_CTL_DEL = 2,
   LUNA_EPOLL_CTL_MOD = 3
};

int luna_os_poll_create(int cloexec)
{
   int fd = kqueue();
   if (fd >= 0 && cloexec) fcntl(fd, F_SETFD, FD_CLOEXEC);
   return fd;
}

static int luna_kqueue_delete(int pollfd, int fd, int16_t filter)
{
   struct kevent change;
   EV_SET(&change, (uintptr_t)fd, filter, EV_DELETE, 0, 0, NULL);
   if (kevent(pollfd, &change, 1, NULL, 0, NULL) == 0) return 0;
   return errno == ENOENT ? 0 : -1;
}

static int luna_kqueue_add(int pollfd, int fd, int16_t filter,
                           uint32_t events, uint64_t data)
{
   uint16_t flags = EV_ADD | EV_ENABLE;
   if (events & (uint32_t)LUNA_EPOLLET) flags |= EV_CLEAR;
   if (events & (uint32_t)LUNA_EPOLLONESHOT) flags |= EV_ONESHOT;
   struct kevent change;
   EV_SET(&change, (uintptr_t)fd, filter, flags, 0, 0,
          (void *)(uintptr_t)data);
   return kevent(pollfd, &change, 1, NULL, 0, NULL);
}

int luna_os_poll_ctl(int pollfd, int op, int fd,
                     const luna_os_poll_event *event)
{
   if (op == LUNA_EPOLL_CTL_DEL) {
      int a = luna_kqueue_delete(pollfd, fd, EVFILT_READ);
      int b = luna_kqueue_delete(pollfd, fd, EVFILT_WRITE);
      return a == 0 && b == 0 ? 0 : -1;
   }
   if (!event || (op != LUNA_EPOLL_CTL_ADD && op != LUNA_EPOLL_CTL_MOD)) {
      errno = EINVAL;
      return -1;
   }
   if (op == LUNA_EPOLL_CTL_MOD) {
      (void)luna_kqueue_delete(pollfd, fd, EVFILT_READ);
      (void)luna_kqueue_delete(pollfd, fd, EVFILT_WRITE);
   }
   int rc = 0;
   if (event->events & (LUNA_EPOLLIN | LUNA_EPOLLRDHUP))
      rc = luna_kqueue_add(pollfd, fd, EVFILT_READ,
                           event->events, event->data);
   if (rc == 0 && (event->events & LUNA_EPOLLOUT))
      rc = luna_kqueue_add(pollfd, fd, EVFILT_WRITE,
                           event->events, event->data);
   return rc;
}

int luna_os_poll_wait(int pollfd, luna_os_poll_event *events,
                      int max_events, int timeout_ms)
{
   struct kevent host[256];
   struct timespec ts, *tsp = NULL;
   if (!events || max_events <= 0) { errno = EINVAL; return -1; }
   if (max_events > 256) max_events = 256;
   if (timeout_ms >= 0) {
      ts.tv_sec = timeout_ms / 1000;
      ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
      tsp = &ts;
   }
   int n = kevent(pollfd, NULL, 0, host, max_events, tsp);
   for (int i = 0; i < n; ++i) {
      uint32_t bits = host[i].filter == EVFILT_WRITE
                         ? LUNA_EPOLLOUT : LUNA_EPOLLIN;
      if (host[i].flags & EV_EOF) {
         bits |= LUNA_EPOLLHUP;
         if (host[i].filter == EVFILT_READ) bits |= LUNA_EPOLLRDHUP;
      }
      if (host[i].flags & EV_ERROR) bits |= LUNA_EPOLLERR;
      events[i].events = bits;
      events[i].data = (uint64_t)(uintptr_t)host[i].udata;
   }
   return n;
}

int luna_os_signal_fd(int fd, const void *mask, size_t mask_size, int flags)
{
   (void)fd; (void)mask; (void)mask_size; (void)flags;
   errno = ENOSYS;
   return -1;
}

/* ---- threads and CPUs --------------------------------------------------- */

int luna_os_cpu_count(void)
{
   int n = 0;
   size_t len = sizeof n;
   /* The logical count, matching what sched_getaffinity reports on Linux;
    * macOS has no affinity mask to narrow it. */
   if (sysctlbyname("hw.logicalcpu", &n, &len, NULL, 0) == 0 && n > 0)
      return n;
   long v = sysconf(_SC_NPROCESSORS_ONLN);
   return v > 0 ? (int)v : 1;
}

void luna_os_thread_name(const char *name)
{
   /* macOS names only the calling thread, which is what every caller here
    * wants anyway. */
   if (name) pthread_setname_np(name);
}

void luna_os_yield(void)
{
   sched_yield();
}

/* ---- time --------------------------------------------------------------- */

uint64_t luna_os_monotonic_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t luna_os_realtime_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- what the machine has ----------------------------------------------- */

uint64_t luna_os_peak_rss_kb(void)
{
   struct rusage usage;
   return getrusage(RUSAGE_SELF, &usage) == 0 ? (uint64_t)usage.ru_maxrss / 1024 : 0;
}

int luna_os_mem_info(uint64_t *total_bytes, uint64_t *avail_bytes)
{
   uint64_t total = 0;
   size_t len = sizeof total;
   vm_statistics64_data_t vm;
   mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
   vm_size_t page = 0;

   if (sysctlbyname("hw.memsize", &total, &len, NULL, 0) != 0 || !total)
      return -1;
   if (total_bytes) *total_bytes = total;
   if (!avail_bytes) return 0;

   if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) page = 4096;
   if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                         (host_info64_t)&vm, &count) != KERN_SUCCESS) {
      *avail_bytes = total / 2u;
      return 0;
   }
   /* Free plus what the pager can reclaim without pushing anything out —
    * the same intent as Linux's MemAvailable. */
   *avail_bytes = ((uint64_t)vm.free_count + (uint64_t)vm.inactive_count +
                   (uint64_t)vm.purgeable_count) * (uint64_t)page;
   return 0;
}

static void luna_fs_copy(luna_os_fs_info *out, const struct statvfs *st)
{
   out->f_bsize = st->f_bsize;
   out->f_frsize = st->f_frsize;
   out->f_blocks = st->f_blocks;
   out->f_bfree = st->f_bfree;
   out->f_bavail = st->f_bavail;
   out->f_files = st->f_files;
   out->f_ffree = st->f_ffree;
   out->f_favail = st->f_favail;
   out->f_fsid = st->f_fsid;
   out->f_flag = st->f_flag;
   out->f_namemax = st->f_namemax;
}

int luna_os_statfs(const char *path, luna_os_fs_info *out)
{
   if (!path || !out) { errno = EINVAL; return -1; }
   struct statvfs st;
   if (statvfs(path, &st) != 0) return -1;
   luna_fs_copy(out, &st);
   return 0;
}

int luna_os_fstatfs(int fd, luna_os_fs_info *out)
{
   if (!out) { errno = EINVAL; return -1; }
   struct statvfs st;
   if (fstatvfs(fd, &st) != 0) return -1;
   luna_fs_copy(out, &st);
   return 0;
}

int luna_os_disk_info(const char *path, uint64_t *total_bytes,
                      uint64_t *free_bytes, uint64_t *block_size)
{
   struct statvfs st;
   if (!path || statvfs(path, &st) != 0) return -1;
   uint64_t frag = st.f_frsize ? st.f_frsize : st.f_bsize;
   if (total_bytes) *total_bytes = (uint64_t)st.f_blocks * frag;
   if (free_bytes)  *free_bytes  = (uint64_t)st.f_bavail * frag;
   if (block_size)  *block_size  = st.f_bsize;
   return 0;
}

/* ---- the window --------------------------------------------------------- */

void *luna_os_native_display(void)
{
   return NULL;  /* Cocoa has no display connection to hand out */
}

void *luna_os_native_window(void *glfw_window)
{
   if (!glfw_window) return NULL;
   /* ANGLE's Metal WindowSurface takes the backing CALayer, not NSWindow.
    * Keep this file C by using the Objective-C runtime directly. */
   id window = (id)glfwGetCocoaWindow((GLFWwindow *)glfw_window);
   SEL content_view = sel_registerName("contentView");
   SEL set_wants_layer = sel_registerName("setWantsLayer:");
   SEL layer_sel = sel_registerName("layer");
   id view = ((id (*)(id, SEL))objc_msgSend)(window, content_view);
   if (!view) return NULL;
   ((void (*)(id, SEL, int))objc_msgSend)(view, set_wants_layer, 1);
   return (void *)((id (*)(id, SEL))objc_msgSend)(view, layer_sel);
}

/* GLFW's Cocoa backend ignores glfwSetWindowIcon: the icon is the
 * application's, set through NSApplication.  Build an NSImage from the pixels
 * with the Objective-C runtime, as luna_os_native_window does, to stay C. */
int luna_os_set_window_icon(void *glfw_window, int w, int h,
                            const unsigned char *rgba)
{
   (void)glfw_window;
   if (w <= 0 || h <= 0 || !rgba) return -1;
   if ((size_t)w > SIZE_MAX / 4 / (size_t)h) return -1;
   Class rep_class = objc_getClass("NSBitmapImageRep");
   Class image_class = objc_getClass("NSImage");
   Class app_class = objc_getClass("NSApplication");
   if (!rep_class || !image_class || !app_class) return -1;

   SEL alloc_sel = sel_registerName("alloc");
   SEL release_sel = sel_registerName("release");
   id rep = ((id (*)(id, SEL))objc_msgSend)((id)rep_class, alloc_sel);
   rep = ((id (*)(id, SEL, unsigned char **, long, long, long, long, BOOL, BOOL,
                  id, unsigned long, long, long))objc_msgSend)(
      rep, sel_registerName("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:"
                            "bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:"
                            "colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:"),
      NULL, w, h, 8, 4, YES, NO,
      *(id *)dlsym(RTLD_DEFAULT, "NSDeviceRGBColorSpace"),
      /* NSBitmapFormatAlphaNonpremultiplied */ 2UL, (long)w * 4, 32);
   if (!rep) return -1;
   unsigned char *dst = ((unsigned char *(*)(id, SEL))objc_msgSend)(
      rep, sel_registerName("bitmapData"));
   if (!dst) {
      ((void (*)(id, SEL))objc_msgSend)(rep, release_sel);
      return -1;
   }
   memcpy(dst, rgba, (size_t)w * (size_t)h * 4);

   id image = ((id (*)(id, SEL))objc_msgSend)((id)image_class, alloc_sel);
   typedef struct { double width, height; } icon_size;   /* NSSize == CGSize */
   const icon_size size = { (double)w, (double)h };
   image = ((id (*)(id, SEL, icon_size))objc_msgSend)(
      image, sel_registerName("initWithSize:"), size);
   ((void (*)(id, SEL, id))objc_msgSend)(
      image, sel_registerName("addRepresentation:"), rep);
   ((void (*)(id, SEL))objc_msgSend)(rep, release_sel);

   id app = ((id (*)(id, SEL))objc_msgSend)((id)app_class,
                                             sel_registerName("sharedApplication"));
   ((void (*)(id, SEL, id))objc_msgSend)(
      app, sel_registerName("setApplicationIconImage:"), image);
   ((void (*)(id, SEL))objc_msgSend)(image, release_sel);   /* app retains */
   return 0;
}

void *luna_os_offscreen_window(void *glfw_window, int w, int h)
{
   (void)glfw_window; (void)w; (void)h;
   return NULL;
}

void luna_os_offscreen_resize(void *native_window, int w, int h)
{
   (void)native_window; (void)w; (void)h;
}

/* ---- audio out --------------------------------------------------------- *
 *
 * CoreAudio's AudioQueue, the macOS counterpart of the ALSA path in
 * lunaria_linux.c.  The queue owns a callback thread that CoreAudio drives;
 * the caller is a guest thread inside an SVC and must not wait on it, so the
 * two are decoupled by the same plain ring of interleaved S16 frames: one
 * producer (the guest audio thread) and one consumer (the queue callback),
 * synchronised by a pair of atomic indices and nothing else — no lock on the
 * path the guest is on.  The ring holds about a third of a second. */
#include <stdatomic.h>
#include <stdlib.h>
#include <AudioToolbox/AudioToolbox.h>

#define LUNA_MAC_AUDIO_RING_FRAMES 16384u   /* 341 ms at 48 kHz */
#define LUNA_MAC_AUDIO_BUFS        3
#define LUNA_MAC_AUDIO_BUF_FRAMES  512u

static AudioQueueRef     g_aq;
static AudioQueueBufferRef g_aq_buf[LUNA_MAC_AUDIO_BUFS];
static int               g_aq_open;
static unsigned          g_aq_ch = 2u;
static int16_t          *g_aq_ring;
static _Atomic unsigned  g_aq_head;   /* producer writes here */
static _Atomic unsigned  g_aq_tail;   /* consumer reads here */
static _Atomic uint64_t  g_aq_played; /* source frames actually consumed */

static unsigned luna_aq_used(void)
{
   const unsigned h = atomic_load_explicit(&g_aq_head, memory_order_acquire);
   const unsigned t = atomic_load_explicit(&g_aq_tail, memory_order_acquire);
   return (h - t) % LUNA_MAC_AUDIO_RING_FRAMES;
}

static void luna_aq_callback(void *user, AudioQueueRef q, AudioQueueBufferRef b)
{
   (void)user;
   const unsigned want = LUNA_MAC_AUDIO_BUF_FRAMES;
   int16_t *out = (int16_t *)b->mAudioData;
   unsigned t = atomic_load_explicit(&g_aq_tail, memory_order_relaxed);
   const unsigned have = luna_aq_used();
   const unsigned n = have < want ? have : want;
   for (unsigned i = 0; i < n; ++i) {
      const unsigned src = ((t + i) % LUNA_MAC_AUDIO_RING_FRAMES) * g_aq_ch;
      for (unsigned c = 0; c < g_aq_ch; ++c)
         out[i * g_aq_ch + c] = g_aq_ring[src + c];
   }
   /* AudioQueue permits a buffer to carry fewer bytes than it was allocated
    * with.  Do not turn a producer delay into a whole 512-frame silence
    * block: OpenSL's buffer callback may become runnable just after this
    * callback sampled the ring, and padding the allocation made the device
    * commit to another 10.7 ms of silence before it could look again.  Submit
    * all real frames we have.  When completely dry submit a small silence
    * quantum so the device clock keeps running while recovery latency stays
    * bounded.  This is the same non-blocking underrun behaviour used by an
    * Android AudioTrack fast mixer; the real-time callback never waits. */
   const unsigned submit = n ? n : 128u;
   if (!n)
      memset(out, 0, (size_t)submit * g_aq_ch * sizeof *out);
   atomic_store_explicit(&g_aq_tail, (t + n) % LUNA_MAC_AUDIO_RING_FRAMES,
                         memory_order_release);
   /* The device's playback position, silence quantum included -- see the
    * same counter in lunaria_linux.c.  Counting only the frames the producer
    * supplied makes an underrun permanent: the OpenSL buffer queue retires a
    * buffer when this counter passes its end mark, and the guest enqueues the
    * next buffer from inside that retirement, so a stalled counter means the
    * guest is never asked for audio again. */
   atomic_fetch_add_explicit(&g_aq_played, submit, memory_order_release);
   b->mAudioDataByteSize = submit * g_aq_ch * sizeof(int16_t);
   AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void audio_saved_prefs(void);

int luna_os_audio_open(unsigned rate, unsigned channels)
{
   if (g_aq_open) return 0;
   if (!rate) rate = 48000u;
   if (channels < 1u || channels > 8u) channels = 2u;

   AudioStreamBasicDescription fmt;
   memset(&fmt, 0, sizeof fmt);
   fmt.mSampleRate       = (double)rate;
   fmt.mFormatID         = kAudioFormatLinearPCM;
   fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger |
                           kAudioFormatFlagIsPacked;
   fmt.mBitsPerChannel   = 16;
   fmt.mChannelsPerFrame = channels;
   fmt.mFramesPerPacket  = 1;
   fmt.mBytesPerFrame    = channels * (UInt32)sizeof(int16_t);
   fmt.mBytesPerPacket   = fmt.mBytesPerFrame;

   g_aq_ring = (int16_t *)calloc((size_t)LUNA_MAC_AUDIO_RING_FRAMES * channels,
                                 sizeof *g_aq_ring);
   if (!g_aq_ring) return -1;
   g_aq_ch = channels;
   atomic_store_explicit(&g_aq_head, 0u, memory_order_relaxed);
   atomic_store_explicit(&g_aq_tail, 0u, memory_order_relaxed);
   atomic_store_explicit(&g_aq_played, 0u, memory_order_relaxed);

   OSStatus rc = AudioQueueNewOutput(&fmt, luna_aq_callback, NULL, NULL, NULL,
                                     0, &g_aq);
   if (rc != noErr) {
      fprintf(stderr, "[audio] AudioQueueNewOutput failed (%d)\n", (int)rc);
      free(g_aq_ring); g_aq_ring = NULL;
      return -1;
   }
   const UInt32 bytes = LUNA_MAC_AUDIO_BUF_FRAMES * fmt.mBytesPerFrame;
   for (int i = 0; i < LUNA_MAC_AUDIO_BUFS; ++i) {
      if (AudioQueueAllocateBuffer(g_aq, bytes, &g_aq_buf[i]) != noErr) {
         AudioQueueDispose(g_aq, true); g_aq = NULL;
         free(g_aq_ring); g_aq_ring = NULL;
         return -1;
      }
      luna_aq_callback(NULL, g_aq, g_aq_buf[i]);   /* prime with silence */
   }
   if (AudioQueueStart(g_aq, NULL) != noErr) {
      AudioQueueDispose(g_aq, true); g_aq = NULL;
      free(g_aq_ring); g_aq_ring = NULL;
      return -1;
   }
   g_aq_open = 1;
   fprintf(stderr, "[audio] AudioQueue %u Hz %u ch\n", rate, channels);
   audio_saved_prefs();
   return 0;
}

static _Atomic int g_aq_gain_q15 = 32768;
static _Atomic int g_aq_muted;

static void audio_saved_prefs(void)
{
   static int done;
   const char *v, *m;
   if (done) return;
   done = 1;
   v = getenv("LUNARIA_AUDIO_VOLUME");
   if (v && *v) {
      float g = (float)atof(v);
      if (g < 0.0f) g = 0.0f;
      if (g > 1.0f) g = 1.0f;
      atomic_store_explicit(&g_aq_gain_q15, (int)(g * 32768.0f + 0.5f), memory_order_relaxed);
   }
   m = getenv("LUNARIA_AUDIO_MUTE");
   if (m && *m && strcmp(m, "0") != 0)
      atomic_store_explicit(&g_aq_muted, 1, memory_order_relaxed);
}

void luna_os_audio_set_volume(float gain)
{
   if (gain < 0.0f) gain = 0.0f;
   if (gain > 1.0f) gain = 1.0f;
   atomic_store_explicit(&g_aq_gain_q15, (int)(gain * 32768.0f + 0.5f), memory_order_relaxed);
}
float luna_os_audio_volume(void) { audio_saved_prefs(); return (float)atomic_load(&g_aq_gain_q15) / 32768.0f; }
void luna_os_audio_set_muted(int muted) { atomic_store(&g_aq_muted, muted ? 1 : 0); }
int luna_os_audio_muted(void) { audio_saved_prefs(); return atomic_load(&g_aq_muted); }
/* AudioQueue plays on the system's current output; choosing one is the
 * system's Sound settings. */
int luna_os_audio_devices(char (*names)[128], char (*descs)[128], int max)
{
   if (max < 1) return 0;
   snprintf(names[0], 128, "default");
   snprintf(descs[0], 128, "System output");
   return 1;
}
static char g_mac_audio_dev[128] = "default";
int luna_os_audio_select_device(const char *name)
{
   const char *use = !name || !*name ? "default" : name;
   if (!strcmp(use, "none")) use = "off";
   if (strcmp(use, "default") && strcmp(use, "off")) return -1;
   snprintf(g_mac_audio_dev, sizeof g_mac_audio_dev, "%s", use);
   return 0;
}
const char *luna_os_audio_device(void) { return g_mac_audio_dev; }

int luna_os_audio_write(const void *pcm16, unsigned frames)
{
   if (!g_aq_open || !pcm16 || !frames) return 0;
   const int gain = atomic_load_explicit(&g_aq_muted, memory_order_relaxed)
      ? 0 : atomic_load_explicit(&g_aq_gain_q15, memory_order_relaxed);
   const int16_t *src = (const int16_t *)pcm16;
   const unsigned h = atomic_load_explicit(&g_aq_head, memory_order_relaxed);
   const unsigned free_frames =
      LUNA_MAC_AUDIO_RING_FRAMES - 1u - luna_aq_used();
   if (frames > free_frames) frames = free_frames;
   for (unsigned i = 0; i < frames; ++i) {
      const unsigned dst = ((h + i) % LUNA_MAC_AUDIO_RING_FRAMES) * g_aq_ch;
      for (unsigned c = 0; c < g_aq_ch; ++c)
         g_aq_ring[dst + c] = (int16_t)(((int32_t)src[i * g_aq_ch + c] * gain) >> 15);
   }
   atomic_store_explicit(&g_aq_head, (h + frames) % LUNA_MAC_AUDIO_RING_FRAMES,
                         memory_order_release);
   return (int)frames;
}

unsigned luna_os_audio_queued_frames(void)
{
   return g_aq_open ? luna_aq_used() : 0u;
}

uint64_t luna_os_audio_played_frames(void)
{
   return g_aq_open
      ? atomic_load_explicit(&g_aq_played, memory_order_acquire) : 0u;
}

void luna_os_audio_close(void)
{
   if (!g_aq_open) return;
   g_aq_open = 0;
   AudioQueueStop(g_aq, true);
   AudioQueueDispose(g_aq, true);
   g_aq = NULL;
   free(g_aq_ring);
   g_aq_ring = NULL;
}

uint32_t luna_os_lrand48(void) { return (uint32_t)lrand48(); }
void luna_os_srand48(int32_t seed) { srand48((long)seed); }

int luna_os_pipe_open(int fds[2], int nonblocking, int close_on_exec)
{
   if (pipe(fds)) return -1;
   for (int i = 0; i < 2; ++i) {
      int flags = fcntl(fds[i], F_GETFL);
      if (flags < 0 || (nonblocking && fcntl(fds[i], F_SETFL, flags | O_NONBLOCK)) ||
          (close_on_exec && fcntl(fds[i], F_SETFD, FD_CLOEXEC))) {
         int error = errno; close(fds[0]); close(fds[1]); errno = error; return -1;
      }
   }
   return 0;
}

luna_os_locale luna_os_locale_new(void)
{
   locale_t locale = newlocale(LC_ALL_MASK, "C.UTF-8", (locale_t)0);
   if (!locale) locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
   return (luna_os_locale)locale;
}
luna_os_locale luna_os_locale_clone(luna_os_locale locale)
{
   if (!locale) { errno = EINVAL; return NULL; }
   return (luna_os_locale)duplocale((locale_t)locale);
}
void luna_os_locale_free(luna_os_locale locale)
{ if (locale) freelocale((locale_t)locale); }
int luna_os_locale_use(luna_os_locale locale)
{ return uselocale(locale ? (locale_t)locale : LC_GLOBAL_LOCALE) ? 0 : -1; }

int luna_os_time_break(int64_t seconds, int local, struct tm *result, int64_t *offset)
{
   time_t value = (time_t)seconds;
   if ((int64_t)value != seconds) { errno = EOVERFLOW; return -1; }
   if (!(local ? localtime_r(&value, result) : gmtime_r(&value, result))) return -1;
   *offset = (int64_t)result->tm_gmtoff;
   return 0;
}
int luna_os_time_make(struct tm *value, int64_t *seconds, int64_t *offset)
{
   int saved = errno;
   errno = 0;
   time_t result = mktime(value);
   if (result == (time_t)-1 && errno) return -1;
   *seconds = (int64_t)result;
   *offset = (int64_t)value->tm_gmtoff;
   errno = saved;
   return 0;
}

int luna_os_unsetenv(const char *name) { return unsetenv(name); }
intptr_t luna_os_command_pipe_open(const char *path)
{
   if (mkfifo(path, 0600) && errno != EEXIST) return -1;
   return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}
ptrdiff_t luna_os_command_pipe_read(intptr_t pipe, void *buffer, size_t capacity)
{
   return read((int)pipe, buffer, capacity);
}
void luna_os_command_pipe_close(intptr_t pipe) { close((int)pipe); }

void *luna_os_library_loaded_symbol(const char *module, const char *name)
{
   void *handle = dlopen(module, RTLD_LAZY | RTLD_NOLOAD);
   if (!handle) return NULL;
   void *symbol = dlsym(handle, name);
   dlclose(handle);
   return symbol;
}
