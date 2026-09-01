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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

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
static struct { int rd, wr; uint64_t count; } g_events[LUNA_EVENT_MAX];

static int luna_event_slot(int fd)
{
   for (int i = 0; i < LUNA_EVENT_MAX; ++i)
      if (g_events[i].rd > 0 && g_events[i].rd == fd) return i;
   return -1;
}

int luna_os_event_open(unsigned initval, int nonblock)
{
   int fds[2], slot = -1;
   for (int i = 0; i < LUNA_EVENT_MAX; ++i)
      if (!g_events[i].rd) { slot = i; break; }
   if (slot < 0 || pipe(fds) != 0) return -1;
   fcntl(fds[0], F_SETFD, FD_CLOEXEC);
   fcntl(fds[1], F_SETFD, FD_CLOEXEC);
   if (nonblock) fcntl(fds[0], F_SETFL, O_NONBLOCK);
   fcntl(fds[1], F_SETFL, O_NONBLOCK);
   g_events[slot].rd = fds[0];
   g_events[slot].wr = fds[1];
   g_events[slot].count = 0;
   if (initval) luna_os_event_signal(fds[0], initval);
   return fds[0];
}

int luna_os_event_signal(int fd, uint64_t count)
{
   int i = luna_event_slot(fd);
   char b = 1;
   if (i < 0 || !count) return i < 0 ? -1 : 0;
   g_events[i].count += count;
   return write(g_events[i].wr, &b, 1) == 1 ? 0 : -1;
}

int luna_os_event_drain(int fd, uint64_t *out)
{
   int i = luna_event_slot(fd);
   char b[64];
   if (i < 0) return -1;
   while (read(g_events[i].rd, b, sizeof b) > 0) { }
   if (!g_events[i].count) { errno = EAGAIN; return -1; }
   if (out) *out = g_events[i].count;
   g_events[i].count = 0;
   return 0;
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
