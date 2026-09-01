/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * lunaria_os.h on Windows.  See that header for what each entry point owes
 * its caller; this file only says how Windows provides it.
 *
 * Two places where Windows is not Linux and the difference is visible to the
 * rest of the emulator, so they are stated here rather than hidden:
 *
 *   - There is no MAP_NORESERVE.  A reserved-but-uncommitted range faults on
 *     first touch and Windows delivers that as an exception, not as a page
 *     the kernel quietly supplies.  So a reservation is reserved *and*
 *     committed, and the commit is charged against the pagefile immediately:
 *     the guest heap size (LUNARIA_HEAP_MB) is a real cost here, where on
 *     Linux it is only an address-space size.
 *
 *   - There is no mremap.  luna_os_remap always reports that it cannot grow
 *     in place, and the caller copies — which is the same path Linux takes
 *     when the range ahead is occupied.
 */

#include "lunaria_os.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

/* ---- virtual memory ---------------------------------------------------- */

static DWORD luna_prot_to_host(int prot)
{
   const int rw = LUNA_PROT_READ | LUNA_PROT_WRITE;
   if (prot & LUNA_PROT_EXEC)
      return (prot & LUNA_PROT_WRITE) ? PAGE_EXECUTE_READWRITE
           : (prot & LUNA_PROT_READ)  ? PAGE_EXECUTE_READ
                                      : PAGE_EXECUTE;
   if ((prot & rw) == rw)          return PAGE_READWRITE;
   if (prot & LUNA_PROT_WRITE)     return PAGE_READWRITE;  /* no write-only */
   if (prot & LUNA_PROT_READ)      return PAGE_READONLY;
   return PAGE_NOACCESS;
}

void *luna_os_reserve(void *want, size_t len, int exact)
{
   if (exact && !want) return NULL;
   void *p = VirtualAlloc(exact ? want : NULL, len,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
   if (!p) return NULL;
   /* VirtualAlloc with a base either gives that base or fails, so an address
    * that came back different is not something to work around. */
   if (exact && p != want) { VirtualFree(p, 0, MEM_RELEASE); return NULL; }
   return p;
}

int luna_os_release(void *addr, size_t len)
{
   /* MEM_RELEASE frees a whole reservation and rejects a length; a sub-range
    * of a live reservation can only be decommitted.  Try the whole-range form
    * first, which is what a matching reserve/release pair wants. */
   if (VirtualFree(addr, 0, MEM_RELEASE)) return 0;
   return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : -1;
}

int luna_os_protect(void *addr, size_t len, int prot)
{
   DWORD old = 0;
   return VirtualProtect(addr, len, luna_prot_to_host(prot), &old) ? 0 : -1;
}

void *luna_os_map_file(void *want, size_t len, int prot, int fd,
                       uint64_t off, int fixed)
{
   HANDLE h = (HANDLE)_get_osfhandle(fd);
   if (h == INVALID_HANDLE_VALUE) return NULL;
   if (fixed && !want) return NULL;

   const int writable = (prot & LUNA_PROT_WRITE) != 0;
   /* Private (copy-on-write) to match the emulator's file-backed guest
    * mappings: the guest may write to a mapped pak page without the change
    * reaching the file. */
   DWORD page = (prot & LUNA_PROT_EXEC)
                  ? (writable ? PAGE_EXECUTE_WRITECOPY : PAGE_EXECUTE_READ)
                  : (writable ? PAGE_WRITECOPY : PAGE_READONLY);
   DWORD access = (prot & LUNA_PROT_EXEC) ? FILE_MAP_EXECUTE : 0;
   access |= writable ? FILE_MAP_COPY : FILE_MAP_READ;

   HANDLE m = CreateFileMappingA(h, NULL, page, 0, 0, NULL);
   if (!m) return NULL;
   void *p = MapViewOfFileEx(m, access, (DWORD)(off >> 32), (DWORD)off, len,
                             fixed ? want : NULL);
   /* The view keeps the section alive; the handle is not needed past this. */
   CloseHandle(m);
   if (!p) return NULL;
   if (fixed && p != want) { UnmapViewOfFile(p); return NULL; }
   return p;
}

void *luna_os_remap(void *addr, size_t old_len, size_t new_len)
{
   (void)addr; (void)old_len; (void)new_len;
   return NULL;  /* no in-place resize on Windows; the caller copies */
}

size_t luna_os_page_size(void)
{
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   return si.dwPageSize ? si.dwPageSize : 4096u;
}

/* ---- anonymous shared memory ------------------------------------------- */

int luna_os_shm_create(const char *name, size_t size)
{
   /* A pagefile-backed section is the anonymous object; wrapping it in a CRT
    * descriptor is what lets the rest of the emulator keep treating it as the
    * fd that ASharedMemory hands out. */
   (void)name;
   HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                 (DWORD)((uint64_t)size >> 32),
                                 (DWORD)size, NULL);
   if (!m) return -1;
   int fd = _open_osfhandle((intptr_t)m, 0);
   if (fd < 0) { CloseHandle(m); return -1; }
   return fd;
}

/* ---- descriptors -------------------------------------------------------- */

int luna_os_fd_path(int fd, char *buf, size_t bufsz)
{
   HANDLE h;
   DWORD n;
   if (fd < 0 || !buf || bufsz < 2) return -1;
   h = (HANDLE)_get_osfhandle(fd);
   if (h == INVALID_HANDLE_VALUE) return -1;
   n = GetFinalPathNameByHandleA(h, buf, (DWORD)bufsz - 1, FILE_NAME_OPENED);
   if (n == 0 || n >= bufsz) return -1;
   buf[n] = '\0';
   /* Strip the \\?\ prefix Windows prepends: callers compare these against
    * paths the guest handed in, which never carry it. */
   if (!strncmp(buf, "\\\\?\\", 4)) memmove(buf, buf + 4, strlen(buf + 4) + 1);
   return 0;
}

int luna_os_executable_path(char *buf, size_t bufsz)
{
   if (!buf || bufsz < 2 || bufsz > (size_t)DWORD_MAX) {
      errno = EINVAL;
      return -1;
   }
   DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bufsz);
   if (!n || n >= bufsz) {
      errno = n ? ENAMETOOLONG : EIO;
      return -1;
   }
   return 0;
}

int luna_os_symbol_is_function(void *address)
{
   MEMORY_BASIC_INFORMATION mi;
   if (!address || !VirtualQuery(address, &mi, sizeof mi)) return 0;
   DWORD p = mi.Protect & 0xffu;
   return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
          p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

/* An eventfd stands in as a loopback socket pair: the emulator selects on
 * these alongside real sockets, and Winsock's select accepts nothing else.
 * The counter lives on the reader side, so a drain reports the total the way
 * eventfd does rather than one byte per signal. */
#define LUNA_EVENT_MAX 64
static struct { SOCKET rd, wr; uint64_t count; } g_events[LUNA_EVENT_MAX];

static int luna_event_slot(int fd)
{
   for (int i = 0; i < LUNA_EVENT_MAX; ++i)
      if (g_events[i].rd && (int)g_events[i].rd == fd) return i;
   return -1;
}

int luna_os_event_open(unsigned initval, int nonblock)
{
   SOCKET listener = INVALID_SOCKET, rd = INVALID_SOCKET, wr = INVALID_SOCKET;
   struct sockaddr_in addr;
   int len = (int)sizeof addr;
   int slot = -1;

   for (int i = 0; i < LUNA_EVENT_MAX; ++i)
      if (!g_events[i].rd) { slot = i; break; }
   if (slot < 0) return -1;

   listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (listener == INVALID_SOCKET) return -1;
   memset(&addr, 0, sizeof addr);
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   if (bind(listener, (struct sockaddr *)&addr, sizeof addr) ||
       listen(listener, 1) ||
       getsockname(listener, (struct sockaddr *)&addr, &len))
      goto fail;
   wr = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (wr == INVALID_SOCKET) goto fail;
   if (connect(wr, (struct sockaddr *)&addr, sizeof addr)) goto fail;
   rd = accept(listener, NULL, NULL);
   if (rd == INVALID_SOCKET) goto fail;
   closesocket(listener);

   if (nonblock) {
      u_long on = 1;
      ioctlsocket(rd, FIONBIO, &on);
   }
   g_events[slot].rd = rd;
   g_events[slot].wr = wr;
   g_events[slot].count = 0;
   if (initval) luna_os_event_signal((int)rd, initval);
   return (int)rd;

fail:
   if (listener != INVALID_SOCKET) closesocket(listener);
   if (wr != INVALID_SOCKET) closesocket(wr);
   if (rd != INVALID_SOCKET) closesocket(rd);
   return -1;
}

int luna_os_event_signal(int fd, uint64_t count)
{
   int i = luna_event_slot(fd);
   char b = 1;
   if (i < 0 || !count) return i < 0 ? -1 : 0;
   g_events[i].count += count;
   /* One byte is enough to make the descriptor readable; the amount lives in
    * the counter, exactly as eventfd keeps it in the kernel object. */
   return send(g_events[i].wr, &b, 1, 0) == 1 ? 0 : -1;
}

int luna_os_event_drain(int fd, uint64_t *out)
{
   int i = luna_event_slot(fd);
   char b[64];
   if (i < 0) return -1;
   while (recv(g_events[i].rd, b, (int)sizeof b, 0) > 0) { }
   if (!g_events[i].count) { WSASetLastError(WSAEWOULDBLOCK); return -1; }
   if (out) *out = g_events[i].count;
   g_events[i].count = 0;
   return 0;
}

int luna_os_poll_create(int cloexec)
{
   (void)cloexec;
   WSASetLastError(WSAEOPNOTSUPP);
   return -1;
}

int luna_os_poll_ctl(int pollfd, int op, int fd,
                     const luna_os_poll_event *event)
{
   (void)pollfd; (void)op; (void)fd; (void)event;
   WSASetLastError(WSAEOPNOTSUPP);
   return -1;
}

int luna_os_poll_wait(int pollfd, luna_os_poll_event *events,
                      int max_events, int timeout_ms)
{
   (void)pollfd; (void)events; (void)max_events; (void)timeout_ms;
   WSASetLastError(WSAEOPNOTSUPP);
   return -1;
}

int luna_os_signal_fd(int fd, const void *mask, size_t mask_size, int flags)
{
   (void)fd; (void)mask; (void)mask_size; (void)flags;
   WSASetLastError(WSAEOPNOTSUPP);
   return -1;
}

/* ---- threads and CPUs --------------------------------------------------- */

int luna_os_cpu_count(void)
{
   DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
   return n ? (int)n : 1;
}

void luna_os_thread_name(const char *name)
{
   wchar_t w[64];
   if (!name) return;
   MultiByteToWideChar(CP_UTF8, 0, name, -1, w, (int)(sizeof w / sizeof *w));
   SetThreadDescription(GetCurrentThread(), w);
}

void luna_os_yield(void)
{
   SwitchToThread();
}

/* ---- time --------------------------------------------------------------- */

uint64_t luna_os_monotonic_ns(void)
{
   static LARGE_INTEGER freq;
   LARGE_INTEGER now;
   if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&now);
   /* Split so a machine with a 10 MHz counter does not overflow the multiply
    * after a few hours of uptime. */
   return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000000ull +
          (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000000ull /
              (uint64_t)freq.QuadPart;
}

uint64_t luna_os_realtime_ns(void)
{
   FILETIME ft;
   ULARGE_INTEGER u;
   GetSystemTimeAsFileTime(&ft);
   u.LowPart = ft.dwLowDateTime;
   u.HighPart = ft.dwHighDateTime;
   /* FILETIME counts 100 ns ticks from 1601-01-01; the guest counts from
    * 1970-01-01. */
   return (u.QuadPart - 116444736000000000ull) * 100ull;
}

/* ---- what the machine has ----------------------------------------------- */

int luna_os_mem_info(uint64_t *total_bytes, uint64_t *avail_bytes)
{
   MEMORYSTATUSEX st;
   st.dwLength = sizeof st;
   if (!GlobalMemoryStatusEx(&st)) return -1;
   if (total_bytes) *total_bytes = st.ullTotalPhys;
   if (avail_bytes) *avail_bytes = st.ullAvailPhys;
   return 0;
}

int luna_os_disk_info(const char *path, uint64_t *total_bytes,
                      uint64_t *free_bytes, uint64_t *block_size)
{
   ULARGE_INTEGER avail, total, freeb;
   if (!path || !GetDiskFreeSpaceExA(path, &avail, &total, &freeb)) return -1;
   if (total_bytes) *total_bytes = total.QuadPart;
   /* The quota-aware figure, which is what the caller can actually write. */
   if (free_bytes)  *free_bytes  = avail.QuadPart;
   if (block_size)  *block_size  = luna_os_page_size();
   return 0;
}

/* ---- the window --------------------------------------------------------- */

void *luna_os_native_display(void)
{
   return NULL;  /* Windows has no display connection to hand out */
}

void *luna_os_native_window(void *glfw_window)
{
   if (!glfw_window) return NULL;
   return (void *)glfwGetWin32Window((GLFWwindow *)glfw_window);
}
