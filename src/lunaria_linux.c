/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * lunaria_os.h on Linux.  See that header for what each entry point owes its
 * caller; this file only says how Linux provides it.
 */

#define _GNU_SOURCE
#include "lunaria_os.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
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
   /* MAP_NORESERVE is the whole point of the guest heap being this large:
    * the range exists, and a page costs memory only once it is written. */
   int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
   if (exact) {
#ifdef MAP_FIXED_NOREPLACE
      /* Ask for exactly this address and be told when something is already
       * there.  MAP_FIXED would take the range by unmapping whatever it hit,
       * which is how a guest pointer silently starts pointing at rubble. */
      flags |= MAP_FIXED_NOREPLACE;
#endif
      if (!want) return NULL;
   }
   void *p = mmap(want, len, PROT_READ | PROT_WRITE, flags, -1, 0);
   if (p == MAP_FAILED) return NULL;
   /* MAP_FIXED_NOREPLACE fails outright when the range is taken, but a kernel
    * without it treats the address as a hint and answers elsewhere — so check
    * either way, and give back what we did not ask for. */
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
   int flags = MAP_PRIVATE;
   if (fixed) {
#ifdef MAP_FIXED_NOREPLACE
      flags |= MAP_FIXED_NOREPLACE;
#else
      flags |= MAP_FIXED;
#endif
      if (!want) return NULL;
   }
   void *p = mmap(want, len, luna_prot_to_host(prot), flags, fd, (off_t)off);
   if (p == MAP_FAILED) return NULL;
   if (fixed && p != want) { munmap(p, len); return NULL; }
   return p;
}

void *luna_os_remap(void *addr, size_t old_len, size_t new_len)
{
   /* No MREMAP_MAYMOVE: the guest's VA is the host's, so a mapping that moved
    * would take every pointer into it with it. */
   void *p = mremap(addr, old_len, new_len, 0);
   return p == MAP_FAILED ? NULL : p;
}

size_t luna_os_page_size(void)
{
   long v = sysconf(_SC_PAGESIZE);
   return v > 0 ? (size_t)v : 4096u;
}

/* ---- anonymous shared memory ------------------------------------------- */

int luna_os_shm_create(const char *name, size_t size)
{
   /* memfd is the same object ASharedMemory hands out on a device: anonymous,
    * sized once, mappable by anyone holding the descriptor. */
   /* Sealing is allowed because ASharedMemory_setProt exists: a guest that
    * drops write permission on a region it shared expects that to stick. */
   int fd = memfd_create(name && *name ? name : "lunaria-shmem",
                         MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (fd < 0) return -1;
   if (size && ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
   return fd;
}

/* ---- descriptors -------------------------------------------------------- */

int luna_os_fd_path(int fd, char *buf, size_t bufsz)
{
   char proc[64];
   if (fd < 0 || !buf || bufsz < 2) return -1;
   snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
   ssize_t n = readlink(proc, buf, bufsz - 1);
   if (n <= 0) return -1;
   buf[n] = '\0';
   return 0;
}

int luna_os_executable_path(char *buf, size_t bufsz)
{
   if (!buf || bufsz < 2) { errno = EINVAL; return -1; }
   ssize_t n = readlink("/proc/self/exe", buf, bufsz - 1);
   if (n < 0 || (size_t)n >= bufsz - 1) {
      if (n >= 0) errno = ENAMETOOLONG;
      return -1;
   }
   buf[n] = '\0';
   return 0;
}

int luna_os_symbol_is_function(void *address)
{
   Dl_info info;
   ElfW(Sym) *sym = NULL;
   return dladdr1(address, &info, (void **)&sym, RTLD_DL_SYMENT) != 0 &&
          (!sym || ELF64_ST_TYPE(sym->st_info) == STT_FUNC);
}

int luna_os_event_open(unsigned initval, int nonblock)
{
   return eventfd(initval, EFD_CLOEXEC | (nonblock ? EFD_NONBLOCK : 0));
}

int luna_os_event_signal(int fd, uint64_t count)
{
   if (!count) return 0;
   return write(fd, &count, sizeof count) == (ssize_t)sizeof count ? 0 : -1;
}

int luna_os_event_drain(int fd, uint64_t *out)
{
   uint64_t v = 0;
   if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) return -1;
   if (out) *out = v;
   return 0;
}

int luna_os_poll_create(int cloexec)
{
   return epoll_create1(cloexec ? EPOLL_CLOEXEC : 0);
}

int luna_os_poll_ctl(int pollfd, int op, int fd,
                     const luna_os_poll_event *event)
{
   struct epoll_event ev;
   struct epoll_event *p = NULL;
   if (event) {
      memset(&ev, 0, sizeof ev);
      ev.events = event->events;
      ev.data.u64 = event->data;
      p = &ev;
   }
   return epoll_ctl(pollfd, op, fd, p);
}

int luna_os_poll_wait(int pollfd, luna_os_poll_event *events,
                      int max_events, int timeout_ms)
{
   struct epoll_event host[256];
   if (!events || max_events <= 0) { errno = EINVAL; return -1; }
   if (max_events > 256) max_events = 256;
   int n = epoll_wait(pollfd, host, max_events, timeout_ms);
   for (int i = 0; i < n; ++i) {
      events[i].events = host[i].events;
      events[i].data = host[i].data.u64;
   }
   return n;
}

int luna_os_signal_fd(int fd, const void *mask, size_t mask_size, int flags)
{
   sigset_t set;
   memset(&set, 0, sizeof set);
   if (mask) memcpy(&set, mask, mask_size < sizeof set ? mask_size : sizeof set);
   return signalfd(fd, &set, flags);
}

/* ---- threads and CPUs --------------------------------------------------- */

int luna_os_cpu_count(void)
{
   /* The affinity mask, not the machine's core count: a container or a taskset
    * gives fewer, and sizing the engine pool from cores the process may not
    * run on is how a pool ends up oversubscribed on one CPU. */
   cpu_set_t set;
   CPU_ZERO(&set);
   if (sched_getaffinity(0, sizeof set, &set) == 0) {
      int n = CPU_COUNT(&set);
      if (n > 0) return n;
   }
   long v = sysconf(_SC_NPROCESSORS_ONLN);
   return v > 0 ? (int)v : 1;
}

void luna_os_thread_name(const char *name)
{
   char buf[16];  /* the kernel's limit, including the terminator */
   if (!name) return;
   snprintf(buf, sizeof buf, "%s", name);
   prctl(PR_SET_NAME, buf, 0, 0, 0);
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
   /* MemAvailable, not MemFree: the guest wants to know what it could get,
    * and on Linux most of what looks used is reclaimable page cache. */
   FILE *f = fopen("/proc/meminfo", "re");
   uint64_t total = 0, avail = 0;
   char line[256];
   if (!f) return -1;
   while (fgets(line, sizeof line, f)) {
      unsigned long long kb;
      if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) total = kb * 1024ull;
      else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) avail = kb * 1024ull;
   }
   fclose(f);
   if (!total) return -1;
   if (!avail) avail = total / 2u;
   if (total_bytes) *total_bytes = total;
   if (avail_bytes) *avail_bytes = avail;
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
   return (void *)glfwGetX11Display();
}

void *luna_os_native_window(void *glfw_window)
{
   if (!glfw_window) return NULL;
   /* An X11 window id is a number, not a pointer; it is widened here so the
    * one signature serves the platforms whose handle really is a pointer. */
   return (void *)(uintptr_t)glfwGetX11Window((GLFWwindow *)glfw_window);
}
