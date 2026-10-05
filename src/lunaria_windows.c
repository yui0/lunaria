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
#include <locale.h>
#include <direct.h>
#include <ntsecapi.h>
#include <winternl.h>
#include <winioctl.h>
#include <mstcpip.h>
#include <limits.h>
#include <mmsystem.h>
#include <stdatomic.h>

static wchar_t *luna_file_wide(const char *path);
static SOCKET lookup(int fd);
static SRWLOCK socket_lock = SRWLOCK_INIT;
static void file_description_forget(int fd, HANDLE handle);
static int file_status_register(int fd, int flags);
static int file_status_nonblock(int fd, HANDLE handle, int enabled, int set);

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#define PSAPI_VERSION 2
#include <psapi.h>

/* Invalid guest descriptors are recoverable EBADF errors. UCRT's default
 * invalid-parameter handler aborts instead; change only this thread for the
 * duration of its descriptor lookup and restore the previous handler. */
extern _invalid_parameter_handler __cdecl _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
static void __cdecl descriptor_invalid_parameter(const wchar_t *expression,
   const wchar_t *function, const wchar_t *file, unsigned int line, uintptr_t reserved)
{
   (void)expression; (void)function; (void)file; (void)line; (void)reserved;
}
static intptr_t file_descriptor_handle(int fd)
{
   if (fd < 0) { errno = EBADF; return -1; }
   _invalid_parameter_handler previous =
      _set_thread_local_invalid_parameter_handler(descriptor_invalid_parameter);
   intptr_t result = _get_osfhandle(fd);
   int error = errno;
   _set_thread_local_invalid_parameter_handler(previous);
   if (result == -2) { result = -1; error = EBADF; }
   errno = error;
   return result;
}

int luna_os_setenv(const char *name, const char *value, int overwrite)
{
   if (!name || !*name || strchr(name, '=') || !value) { errno = EINVAL; return -1; }
   if (!overwrite && getenv(name)) return 0;
   int error = _putenv_s(name, value);
   if (error) { errno = error; return -1; }
   return 0;
}
int luna_os_random(void *buffer, size_t length)
{
   if (length && !buffer) { errno = EFAULT; return -1; }
   unsigned char *bytes = buffer;
   while (length) {
      ULONG count = length > ULONG_MAX ? ULONG_MAX : (ULONG)length;
      if (!RtlGenRandom(bytes, count)) { errno = EIO; return -1; }
      bytes += count; length -= count;
   }
   return 0;
}

void *luna_os_library_open(const char *path)
{
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return NULL;
   HMODULE module = LoadLibraryW(wide);
   DWORD error = GetLastError();
   free(wide); SetLastError(error);
   return (void *)module;
}

void *luna_os_library_open_local(const char *path)
{
   return luna_os_library_open(path);
}
const char *luna_os_library_error(void)
{
   static _Thread_local char message[512];
   DWORD error = GetLastError();
   if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, error, 0, message, sizeof message, NULL))
      snprintf(message, sizeof message, "Windows loader error %lu", (unsigned long)error);
   return message;
}

void *luna_os_library_symbol(void *handle, const char *name)
{
   if (handle) return (void *)(uintptr_t)GetProcAddress((HMODULE)handle, name);
   HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,
                                               GetCurrentProcessId());
   if (snapshot == INVALID_HANDLE_VALUE) return NULL;
   MODULEENTRY32 module;
   memset(&module, 0, sizeof module);
   module.dwSize = sizeof module;
   FARPROC symbol = NULL;
   if (Module32First(snapshot, &module)) {
      do {
         symbol = GetProcAddress(module.hModule, name);
         if (symbol) break;
      } while (Module32Next(snapshot, &module));
   }
   CloseHandle(snapshot);
   return (void *)(uintptr_t)symbol;
}

void luna_os_library_close(void *handle)
{
   if (handle) FreeLibrary((HMODULE)handle);
}

int luna_os_backtrace(void **frames, int capacity)
{
   if (!frames || capacity <= 0) return 0;
   if (capacity > 65535) capacity = 65535;
   return (int)CaptureStackBackTrace(1, (DWORD)capacity, frames, NULL);
}

void luna_os_backtrace_print(void *const *frames, int count)
{
   if (!frames) return;
   for (int i = 0; i < count; ++i) fprintf(stderr, "%p\n", frames[i]);
}

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

/* ---- virtual memory ---------------------------------------------------- */

/* Shared views retain a file handle so MS_SYNC remains durable after the
 * guest closes its descriptor. Protect the records against concurrent unmap. */
struct file_view {
   void *base;
   HANDLE file;
   struct file_view *next;
};
static struct file_view *file_views;
static SRWLOCK file_view_lock = SRWLOCK_INIT;


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
   if (!len || (exact && !want)) { errno = EINVAL; return NULL; }
   void *p = VirtualAlloc(exact ? want : NULL, len,
                          MEM_RESERVE | MEM_COMMIT | (exact ? 0 : MEM_TOP_DOWN),
                          PAGE_READWRITE);
   if (!p) {
      errno = exact && GetLastError() == ERROR_INVALID_ADDRESS ? EEXIST : ENOMEM;
      return NULL;
   }
   /* VirtualAlloc with a base either gives that base or fails, so an address
    * that came back different is not something to work around. */
   if (exact && p != want) { VirtualFree(p, 0, MEM_RELEASE); errno = EINVAL; return NULL; }
   return p;
}

int luna_os_release(void *addr, size_t len)
{
   MEMORY_BASIC_INFORMATION mi;
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   if (!addr || !len || (uintptr_t)addr % si.dwPageSize ||
       !VirtualQuery(addr, &mi, sizeof mi) || mi.State == MEM_FREE) {
      errno = EINVAL;
      return -1;
   }
   void *base = mi.AllocationBase;
   uintptr_t end = (uintptr_t)base;
   /* Protections and earlier decommits split a reservation into VirtualQuery
    * regions. Walk the allocation, not just the first region: MEM_RELEASE
    * at the base otherwise destroys still-live pages after a partial unmap. */
   while (VirtualQuery((void *)end, &mi, sizeof mi) &&
          mi.AllocationBase == base) {
      if (mi.RegionSize > UINTPTR_MAX - end) { errno = EINVAL; return -1; }
      end += mi.RegionSize;
   }
   if (len > end - (uintptr_t)addr) { errno = EINVAL; return -1; }
   if (addr == base && len == end - (uintptr_t)base) {
      VirtualQuery(base, &mi, sizeof mi);
      int ok;
      if (mi.Type == MEM_MAPPED) {
         AcquireSRWLockExclusive(&file_view_lock);
         ok = UnmapViewOfFile(base);
         if (ok) {
            struct file_view **at = &file_views;
            while (*at && (*at)->base != base) at = &(*at)->next;
            if (*at) {
               struct file_view *record = *at; *at = record->next;
               if (record->file != INVALID_HANDLE_VALUE) CloseHandle(record->file);
               free(record);
            }
         }
         ReleaseSRWLockExclusive(&file_view_lock);
      } else ok = VirtualFree(base, 0, MEM_RELEASE);
      if (!ok) errno = EINVAL;
      return ok ? 0 : -1;
   }
   VirtualQuery(addr, &mi, sizeof mi);
   if (mi.Type == MEM_MAPPED) { errno = ENOTSUP; return -1; }
   if (!VirtualFree(addr, len, MEM_DECOMMIT)) { errno = EINVAL; return -1; }
   uintptr_t at = (uintptr_t)base;
   int committed = 0;
   while (at < end && VirtualQuery((void *)at, &mi, sizeof mi)) {
      if (mi.State == MEM_COMMIT) { committed = 1; break; }
      at += mi.RegionSize;
   }
   if (!committed) VirtualFree(base, 0, MEM_RELEASE);
   return 0;
}

int luna_os_protect(void *addr, size_t len, int prot)
{
   DWORD old = 0;
   return VirtualProtect(addr, len, luna_prot_to_host(prot), &old) ? 0 : -1;
}

void *luna_os_map_file(void *want, size_t len, int prot, int fd,
                       uint64_t off, int fixed)
{
   return luna_os_map_file_flags(want, len, prot,
      LUNA_MAP_PRIVATE | (fixed ? LUNA_MAP_NOREPLACE : 0), fd, off);
}

void *luna_os_map_anon(void *want, size_t len, int replace)
{
   if (replace && want) {
      MEMORY_BASIC_INFORMATION mi;
      if (VirtualQuery(want, &mi, sizeof mi) && mi.State == MEM_RESERVE)
         return VirtualAlloc(want, len, MEM_COMMIT, PAGE_READWRITE);
   }
   return luna_os_reserve(want, len, replace);
}

void *luna_os_map_file_flags(void *want, size_t len, int prot, int policy,
                            int fd, uint64_t off)
{
   HANDLE h = (HANDLE)file_descriptor_handle(fd);
   if (h == INVALID_HANDLE_VALUE) return NULL;
   const int fixed = (policy & (LUNA_MAP_FIXED | LUNA_MAP_NOREPLACE)) != 0;
   if (fixed && !want) { errno = EINVAL; return NULL; }

   const int writable = (prot & LUNA_PROT_WRITE) != 0;
   const int shared = (policy & LUNA_MAP_SHARED) != 0;
   /* Private maps use copy-on-write; shared writable maps update the file. */
   DWORD page = (prot & LUNA_PROT_EXEC)
                  ? (writable ? (shared ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_WRITECOPY) : PAGE_EXECUTE_READ)
                  : (writable ? (shared ? PAGE_READWRITE : PAGE_WRITECOPY) : PAGE_READONLY);
   DWORD access = (prot & LUNA_PROT_EXEC) ? FILE_MAP_EXECUTE : 0;
   access |= writable ? (shared ? FILE_MAP_WRITE : FILE_MAP_COPY) : FILE_MAP_READ;

   HANDLE m = CreateFileMappingA(h, NULL, page, 0, 0, NULL);
   if (!m) return NULL;
   void *p = NULL;
   if (fixed || sizeof(void *) < 8) {
      p = MapViewOfFileEx(m, access, (DWORD)(off >> 32), (DWORD)off, len,
                          fixed ? want : NULL);
   } else {
      /* Low VAs are aliases of the guest image backing store. An identity
       * mapping must be above that 4 GiB window on a 64-bit host. */
      typedef PVOID (WINAPI *map_view3_fn)(HANDLE, HANDLE, PVOID, ULONG64,
         SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
      HMODULE kernel = GetModuleHandleA("kernelbase.dll");
      map_view3_fn map3 = kernel
         ? (map_view3_fn)(uintptr_t)GetProcAddress(kernel, "MapViewOfFile3") : NULL;
      if (map3) {
         MEM_ADDRESS_REQUIREMENTS requirement;
         MEM_EXTENDED_PARAMETER parameter;
         memset(&requirement, 0, sizeof requirement);
         memset(&parameter, 0, sizeof parameter);
         requirement.LowestStartingAddress = (void *)(uintptr_t)0x100000000ull;
         parameter.Type = MemExtendedParameterAddressRequirements;
         parameter.Pointer = &requirement;
         p = map3(m, GetCurrentProcess(), NULL, off, len, 0, page, &parameter, 1);
      } else {
         /* Older Windows: probe a high free range. A race with another host
          * allocation makes MapViewOfFileEx fail; retry without overwriting. */
         for (int retry = 0; retry < 4 && !p; ++retry) {
            void *candidate = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_TOP_DOWN,
                                             PAGE_NOACCESS);
            if (!candidate) break;
            VirtualFree(candidate, 0, MEM_RELEASE);
            p = MapViewOfFileEx(m, access, (DWORD)(off >> 32), (DWORD)off, len,
                                candidate);
         }
      }
   }
   /* The view keeps the section alive; the handle is not needed past this. */
   CloseHandle(m);
   if (!p) return NULL;
   if (fixed && p != want) { UnmapViewOfFile(p); return NULL; }
   struct file_view *record = calloc(1, sizeof *record);
   if (!record) { UnmapViewOfFile(p); errno = ENOMEM; return NULL; }
   record->base = p; record->file = INVALID_HANDLE_VALUE;
   if (shared && writable && !DuplicateHandle(GetCurrentProcess(), h,
      GetCurrentProcess(), &record->file, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      free(record); UnmapViewOfFile(p); errno = EIO; return NULL;
   }
   AcquireSRWLockExclusive(&file_view_lock);
   record->next = file_views; file_views = record;
   ReleaseSRWLockExclusive(&file_view_lock);
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

int luna_os_memory_advise(void *addr, size_t len, int advice)
{
   switch (advice) {
   case 0: case 1: case 2: case 3: case 4: case 8:
   case 9: case 10: case 11: case 12: case 13: case 14: case 15:
   case 16: case 17: case 18: case 19: case 20: case 21: case 22:
   case 23: case 24: case 25: case 100: case 101: break;
   default: errno = EINVAL; return -1;
   }
   uintptr_t first = (uintptr_t)addr;
   if (first % luna_os_page_size()) { errno = EINVAL; return -1; }
   if (!len) return 0;
   if (len - 1 > UINTPTR_MAX - first) { errno = ENOMEM; return -1; }
   uintptr_t last = first + len - 1;
   for (uintptr_t cursor = first;;) {
      MEMORY_BASIC_INFORMATION info;
      if (!VirtualQuery((void *)cursor, &info, sizeof info) || info.State != MEM_COMMIT) {
         errno = ENOMEM; return -1;
      }
      uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize - 1;
      if (end >= last) break;
      cursor = end + 1;
   }
   /* Access-pattern advice is a hint; Windows keeps its own paging policy. */
   if (advice <= 2) return 0;
   if (advice != 3) {
      /* DiscardVirtualMemory does not promise zero-filled anonymous pages,
       * so it cannot implement Linux MADV_DONTNEED. Preserve the mapping. */
      errno = ENOTSUP; return -1;
   }
   struct memory_range { void *address; SIZE_T size; } range = { addr, len };
   typedef BOOL (WINAPI *prefetch_fn)(HANDLE, ULONG_PTR, struct memory_range *, ULONG);
   prefetch_fn prefetch = (prefetch_fn)(uintptr_t)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory");
   if (!prefetch) { errno = ENOTSUP; return -1; }
   if (!prefetch(GetCurrentProcess(), 1, &range, 0)) {
      DWORD error = GetLastError();
      errno = error == ERROR_CALL_NOT_IMPLEMENTED || error == ERROR_NOT_SUPPORTED ? ENOTSUP :
              error == ERROR_INVALID_PARAMETER ? EINVAL :
              error == ERROR_ACCESS_DENIED ? EACCES : ENOMEM;
      return -1;
   }
   return 0;
}

int luna_os_memory_sync(void *addr, size_t len, int flags)
{
   uintptr_t first = (uintptr_t)addr;
   if (first % luna_os_page_size() ||
       (flags & ~(LUNA_MS_ASYNC | LUNA_MS_INVALIDATE | LUNA_MS_SYNC)) ||
       (flags & LUNA_MS_ASYNC && flags & LUNA_MS_SYNC)) { errno = EINVAL; return -1; }
   if (!len) return 0;
   if (len - 1 > UINTPTR_MAX - first) { errno = ENOMEM; return -1; }
   uintptr_t last = first + len - 1;
   int error = 0;
   AcquireSRWLockShared(&file_view_lock);
   /* Validate the complete range before writing any of its pages. */
   for (uintptr_t cursor = first;;) {
      MEMORY_BASIC_INFORMATION info;
      if (!VirtualQuery((void *)cursor, &info, sizeof info) || info.State != MEM_COMMIT) {
         error = ENOMEM; break;
      }
      uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize - 1;
      if (end >= last) break;
      cursor = end + 1;
   }
   if (!error && flags & LUNA_MS_INVALIDATE) {
      /* Windows views share a coherent cache; no stale copy needs evicting.
       * As on Unix, invalidating a locked page fails with EBUSY. */
      for (uintptr_t cursor = first;;) {
         PSAPI_WORKING_SET_EX_INFORMATION page = { 0 };
         page.VirtualAddress = (void *)cursor;
         if (!QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof page)) { error = EIO; break; }
         if (page.VirtualAttributes.Valid && page.VirtualAttributes.Locked) { error = EBUSY; break; }
         if (last - cursor < luna_os_page_size()) break;
         cursor += luna_os_page_size();
      }
   }
   if (!error && flags & (LUNA_MS_SYNC | LUNA_MS_ASYNC)) {
      for (uintptr_t cursor = first;;) {
         MEMORY_BASIC_INFORMATION info;
         if (!VirtualQuery((void *)cursor, &info, sizeof info) || info.State != MEM_COMMIT) {
            error = ENOMEM; break;
         }
         uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize - 1;
         if (end > last) end = last;
         struct file_view *record = file_views;
         while (record && record->base != info.AllocationBase) record = record->next;
         if (record && record->file != INVALID_HANDLE_VALUE) {
            if (!FlushViewOfFile((void *)cursor, end - cursor + 1) ||
                (flags & LUNA_MS_SYNC && !FlushFileBuffers(record->file))) {
               DWORD native = GetLastError();
               error = native == ERROR_ACCESS_DENIED ? EACCES : EIO; break;
            }
         } else if (info.Type == MEM_MAPPED && !record) {
            error = ENOTSUP; break;
         }
         if (end == last) break;
         cursor = end + 1;
      }
   }
   ReleaseSRWLockShared(&file_view_lock);
   if (error) { errno = error; return -1; }
   return 0;
}

int luna_os_memory_lock(void *addr, size_t len, int unlock)
{
   uintptr_t cursor = (uintptr_t)addr;
   if (!len) return 0;
   if (len - 1 > UINTPTR_MAX - cursor) { errno = ENOMEM; return -1; }
   const uintptr_t last = cursor + len - 1;
   for (;;) {
      MEMORY_BASIC_INFORMATION mapping;
      if (!VirtualQuery((void *)cursor, &mapping, sizeof mapping) ||
          mapping.State != MEM_COMMIT) { errno = ENOMEM; return -1; }
      uintptr_t base = (uintptr_t)mapping.BaseAddress;
      if (!mapping.RegionSize || mapping.RegionSize - 1 > UINTPTR_MAX - base) {
         errno = ENOMEM; return -1;
      }
      uintptr_t end = base + mapping.RegionSize - 1;
      if (end >= last) break;
      cursor = end + 1;
   }
   if (unlock ? VirtualUnlock(addr, len) : VirtualLock(addr, len)) return 0;
   DWORD error = GetLastError();
   /* Unix permits unlocking an already unlocked, mapped range. */
   if (unlock && error == ERROR_NOT_LOCKED) return 0;
   errno = error == ERROR_ACCESS_DENIED ? EPERM :
           error == ERROR_INVALID_PARAMETER ? EINVAL : ENOMEM;
   return -1;
}

int luna_os_residency(void *addr, size_t len, unsigned char *vector)
{
   const size_t page = luna_os_page_size();
   const uintptr_t first = (uintptr_t)addr;
   if (first % page) { errno = EINVAL; return -1; }
   if (!len) return 0;
   if (len - 1 > UINTPTR_MAX - first) { errno = ENOMEM; return -1; }
   if (!vector) { errno = EFAULT; return -1; }
   const size_t pages = len / page + (len % page != 0);
   /* Batches bound scratch space even for the multi-gigabyte guest heap. */
   for (size_t done = 0; done < pages;) {
      PSAPI_WORKING_SET_EX_INFORMATION entries[256] = { 0 };
      const size_t count = pages - done < 256 ? pages - done : 256;
      for (size_t i = 0; i < count; ++i) {
         void *address = (void *)(first + (done + i) * page);
         MEMORY_BASIC_INFORMATION mapping;
         if (!VirtualQuery(address, &mapping, sizeof mapping) || mapping.State != MEM_COMMIT) {
            errno = ENOMEM; return -1;
         }
         entries[i].VirtualAddress = address;
      }
      if (!QueryWorkingSetEx(GetCurrentProcess(), entries, (DWORD)(count * sizeof entries[0]))) {
         errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EIO; return -1;
      }
      for (size_t i = 0; i < count; ++i)
         vector[done + i] = (unsigned char)entries[i].VirtualAttributes.Valid;
      done += count;
   }
   return 0;
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
   h = (HANDLE)file_descriptor_handle(fd);
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
   if (!buf || bufsz < 2 || bufsz > (size_t)UINT32_MAX) {
      errno = EINVAL;
      return -1;
   }
   wchar_t *wide = (wchar_t *)malloc(bufsz * sizeof *wide);
   if (!wide) return -1;
   DWORD n = GetModuleFileNameW(NULL, wide, (DWORD)bufsz);
   if (!n || n >= bufsz) {
      free(wide);
      errno = n ? ENAMETOOLONG : EIO;
      return -1;
   }
   int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
   if (needed <= 0 || (size_t)needed > bufsz) {
      free(wide); errno = needed > 0 ? ENAMETOOLONG : EILSEQ; return -1;
   }
   WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, needed, NULL, NULL);
   free(wide);
   for (char *p = buf; *p; ++p) if (*p == '\\') *p = '/';
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

/* These platforms have no native timerfd. Keep unsupported operations
 * explicit until their descriptor adapter implements timer notifications. */
int luna_os_timer_open(int clock_id, int flags)
{ (void)clock_id; (void)flags; errno = ENOSYS; return -1; }
int luna_os_timer_set(int fd, int flags, const luna_os_timer_spec *value,
                      luna_os_timer_spec *previous)
{ (void)fd; (void)flags; (void)value; (void)previous; errno = ENOSYS; return -1; }
int luna_os_timer_get(int fd, luna_os_timer_spec *value)
{ (void)fd; (void)value; errno = ENOSYS; return -1; }

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

uint64_t luna_os_peak_rss_kb(void)
{
   PROCESS_MEMORY_COUNTERS usage;
   memset(&usage, 0, sizeof usage);
   usage.cb = sizeof usage;
   return GetProcessMemoryInfo(GetCurrentProcess(), &usage, sizeof usage)
      ? (uint64_t)usage.PeakWorkingSetSize / 1024 : 0;
}

int luna_os_mem_info(uint64_t *total_bytes, uint64_t *avail_bytes)
{
   MEMORYSTATUSEX st;
   st.dwLength = sizeof st;
   if (!GlobalMemoryStatusEx(&st)) return -1;
   if (total_bytes) *total_bytes = st.ullTotalPhys;
   if (avail_bytes) *avail_bytes = st.ullAvailPhys;
   return 0;
}

static int luna_win_statfs(const char *path, luna_os_fs_info *out, int verify)
{
   char absolute[32768], volume[32768];
   ULARGE_INTEGER avail, total, freeb;
   DWORD sectors, bytes, free_clusters, clusters, serial, names, flags;
   if (!path || !out) { errno = EINVAL; return -1; }
   if (verify && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) goto fail;
   DWORD n = GetFullPathNameA(path, sizeof absolute, absolute, NULL);
   if (!n) goto fail;
   if (n >= sizeof absolute) { errno = ENAMETOOLONG; return -1; }
   if (!GetVolumePathNameA(absolute, volume, sizeof volume) ||
       !GetDiskFreeSpaceExA(volume, &avail, &total, &freeb) ||
       !GetDiskFreeSpaceA(volume, &sectors, &bytes, &free_clusters, &clusters) ||
       !GetVolumeInformationA(volume, NULL, 0, &serial, &names, &flags, NULL, 0))
      goto fail;
   memset(out, 0, sizeof *out);
   uint64_t cluster_bytes = (uint64_t)sectors * bytes;
   if (!cluster_bytes) { errno = EIO; return -1; }
   out->f_bsize = out->f_frsize = cluster_bytes;
   out->f_blocks = total.QuadPart / cluster_bytes;
   out->f_bfree = freeb.QuadPart / cluster_bytes;
   out->f_bavail = avail.QuadPart / cluster_bytes;
   out->f_fsid = serial;
   out->f_namemax = names;
   /* statvfs ST_RDONLY=1, ST_NOSUID=2. Windows has no Unix setuid bits. */
   out->f_flag = 2 | ((flags & FILE_READ_ONLY_VOLUME) ? 1 : 0);
   return 0;
fail:
   switch (GetLastError()) {
      case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND:
      case ERROR_INVALID_DRIVE: errno = ENOENT; break;
      case ERROR_ACCESS_DENIED: errno = EACCES; break;
      case ERROR_INVALID_HANDLE: errno = EBADF; break;
      case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
      case ERROR_FILENAME_EXCED_RANGE: errno = ENAMETOOLONG; break;
      default: errno = EIO; break;
   }
   return -1;
}

int luna_os_statfs(const char *path, luna_os_fs_info *out)
{
   return luna_win_statfs(path, out, 1);
}

int luna_os_fstatfs(int fd, luna_os_fs_info *out)
{
   char path[32768];
   if (luna_os_fd_path(fd, path, sizeof path) != 0) return -1;
   return luna_win_statfs(path, out, 0);
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

int luna_os_set_window_icon(void *glfw_window, int w, int h,
                            const unsigned char *rgba)
{
   if (!glfw_window || w <= 0 || h <= 0 || !rgba) return -1;
   GLFWimage image = { w, h, (unsigned char *)rgba };
   glfwSetWindowIcon((GLFWwindow *)glfw_window, 1, &image);
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

/* ---- audio output -------------------------------------------------------
 * WinMM owns playback after waveOutWrite; the guest thread never waits for
 * a buffer to finish. Completed headers are reclaimed on the next call. */
static _Atomic float g_win_gain = 1.0f;
static _Atomic int g_win_muted;
static SRWLOCK win_audio_lock = SRWLOCK_INIT;
static char  g_win_audio_dev[128] = "default";
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
      g_win_gain = g;
   }
   m = getenv("LUNARIA_AUDIO_MUTE");
   if (m && *m && strcmp(m, "0") != 0) g_win_muted = 1;
}
void luna_os_audio_set_volume(float gain) { g_win_gain = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain; }
float luna_os_audio_volume(void) { audio_saved_prefs(); return g_win_gain; }
void luna_os_audio_set_muted(int muted) { g_win_muted = muted ? 1 : 0; }
int luna_os_audio_muted(void) { audio_saved_prefs(); return g_win_muted; }
int luna_os_audio_devices(char (*names)[128], char (*descs)[128], int max)
{
   if (max < 1) return 0;
   snprintf(names[0], 128, "default");
   snprintf(descs[0], 128, "System output");
   return 1;
}
int luna_os_audio_select_device(const char *name)
{
   const char *use = !name || !*name ? "default" : name;
   if (!strcmp(use, "none")) use = "off";
   if (strcmp(use, "default") && strcmp(use, "off")) return -1;
   AcquireSRWLockExclusive(&win_audio_lock);
   snprintf(g_win_audio_dev, sizeof g_win_audio_dev, "%s", use);
   ReleaseSRWLockExclusive(&win_audio_lock);
   return 0;
}
const char *luna_os_audio_device(void) { return g_win_audio_dev; }

#define WIN_AUDIO_SLOTS 16
#define WIN_AUDIO_FRAMES 1024
static HWAVEOUT win_audio;
static WAVEHDR win_audio_headers[WIN_AUDIO_SLOTS];
static int16_t *win_audio_pcm;
static unsigned win_audio_channels;
static uint64_t win_audio_played;

static void win_audio_reap(void)
{
   if (!win_audio) return;
   for (unsigned i = 0; i < WIN_AUDIO_SLOTS; ++i) {
      WAVEHDR *h = &win_audio_headers[i];
      if ((h->dwFlags & (WHDR_PREPARED | WHDR_DONE)) != (WHDR_PREPARED | WHDR_DONE)) continue;
      if (waveOutUnprepareHeader(win_audio, h, sizeof *h) != MMSYSERR_NOERROR) continue;
      win_audio_played += h->dwBufferLength / (win_audio_channels * sizeof(int16_t));
      h->dwFlags = 0;
   }
}

int luna_os_audio_open(unsigned rate, unsigned channels)
{
   AcquireSRWLockExclusive(&win_audio_lock);
   audio_saved_prefs();
   if (win_audio) { ReleaseSRWLockExclusive(&win_audio_lock); return 0; }
   if (!rate) rate = 48000;
   if (channels < 1 || channels > 8) channels = 2;
   WAVEFORMATEX format = { 0 };
   format.wFormatTag = WAVE_FORMAT_PCM;
   format.nChannels = (WORD)channels;
   format.nSamplesPerSec = rate;
   format.wBitsPerSample = 16;
   format.nBlockAlign = (WORD)(channels * sizeof(int16_t));
   if (rate > UINT32_MAX / format.nBlockAlign) { ReleaseSRWLockExclusive(&win_audio_lock); return -1; }
   format.nAvgBytesPerSec = rate * format.nBlockAlign;
   win_audio_pcm = calloc(WIN_AUDIO_SLOTS * WIN_AUDIO_FRAMES * channels, sizeof(int16_t));
   if (!win_audio_pcm) { ReleaseSRWLockExclusive(&win_audio_lock); return -1; }
   MMRESULT result = waveOutOpen(&win_audio, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
   if (result != MMSYSERR_NOERROR) {
      free(win_audio_pcm); win_audio_pcm = NULL; win_audio = NULL;
      ReleaseSRWLockExclusive(&win_audio_lock); return -1;
   }
   win_audio_channels = channels;
   win_audio_played = 0;
   memset(win_audio_headers, 0, sizeof win_audio_headers);
   ReleaseSRWLockExclusive(&win_audio_lock);
   return 0;
}

int luna_os_audio_write(const void *pcm16, unsigned frames)
{
   if (!pcm16 || !frames) return 0;
   AcquireSRWLockExclusive(&win_audio_lock);
   if (!win_audio) { ReleaseSRWLockExclusive(&win_audio_lock); return 0; }
   win_audio_reap();
   unsigned accepted = 0;
   const int16_t *source = pcm16;
   const int gain = g_win_muted || !strcmp(g_win_audio_dev, "off") ? 0 : (int)(g_win_gain * 32768.0f);
   for (unsigned i = 0; i < WIN_AUDIO_SLOTS && accepted < frames; ++i) {
      WAVEHDR *h = &win_audio_headers[i];
      if (h->dwFlags & WHDR_PREPARED) continue;
      unsigned count = frames - accepted;
      if (count > WIN_AUDIO_FRAMES) count = WIN_AUDIO_FRAMES;
      int16_t *destination = win_audio_pcm + i * WIN_AUDIO_FRAMES * win_audio_channels;
      for (unsigned j = 0; j < count * win_audio_channels; ++j)
         destination[j] = (int16_t)((int32_t)source[accepted * win_audio_channels + j] * gain / 32768);
      h->lpData = (LPSTR)destination;
      h->dwBufferLength = count * win_audio_channels * sizeof(int16_t);
      h->dwFlags = 0;
      if (waveOutPrepareHeader(win_audio, h, sizeof *h) != MMSYSERR_NOERROR) break;
      if (waveOutWrite(win_audio, h, sizeof *h) != MMSYSERR_NOERROR) {
         waveOutUnprepareHeader(win_audio, h, sizeof *h);
         h->dwFlags = 0;
         break;
      }
      accepted += count;
   }
   ReleaseSRWLockExclusive(&win_audio_lock);
   return (int)accepted;
}

unsigned luna_os_audio_queued_frames(void)
{
   AcquireSRWLockExclusive(&win_audio_lock);
   win_audio_reap();
   unsigned frames = 0;
   if (win_audio) for (unsigned i = 0; i < WIN_AUDIO_SLOTS; ++i)
      if (win_audio_headers[i].dwFlags & WHDR_PREPARED)
         frames += win_audio_headers[i].dwBufferLength / (win_audio_channels * sizeof(int16_t));
   ReleaseSRWLockExclusive(&win_audio_lock);
   return frames;
}

uint64_t luna_os_audio_played_frames(void)
{
   AcquireSRWLockExclusive(&win_audio_lock);
   win_audio_reap();
   uint64_t frames = win_audio_played;
   ReleaseSRWLockExclusive(&win_audio_lock);
   return frames;
}

void luna_os_audio_close(void)
{
   AcquireSRWLockExclusive(&win_audio_lock);
   if (win_audio) {
      waveOutReset(win_audio);
      for (unsigned i = 0; i < WIN_AUDIO_SLOTS; ++i)
         if (win_audio_headers[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(win_audio, &win_audio_headers[i], sizeof(WAVEHDR));
      waveOutClose(win_audio); win_audio = NULL;
   }
   free(win_audio_pcm); win_audio_pcm = NULL;
   memset(win_audio_headers, 0, sizeof win_audio_headers);
   win_audio_played = 0;
   ReleaseSRWLockExclusive(&win_audio_lock);
}

/* Windows file operations. */
static int luna_file_error(DWORD error)
{
   switch (error) {
   case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_DELETE_PENDING: errno = ENOENT; break;
   case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: errno = EEXIST; break;
   case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
   case ERROR_INVALID_HANDLE: errno = EBADF; break;
   case ERROR_INVALID_PARAMETER: case ERROR_INVALID_NAME: errno = EINVAL; break;
   case ERROR_DISK_FULL: errno = ENOSPC; break;
   case ERROR_DIRECTORY: errno = ENOTDIR; break;
   case ERROR_NOT_SAME_DEVICE: errno = EXDEV; break;
   case ERROR_PRIVILEGE_NOT_HELD: errno = EPERM; break;
   case ERROR_DIR_NOT_EMPTY: errno = ENOTEMPTY; break;
   case ERROR_FILENAME_EXCED_RANGE: errno = ENAMETOOLONG; break;
   case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
   default: errno = EIO; break;
   }
   return -1;
}
static wchar_t *luna_file_wide(const char *path)
{
   if (!path) { errno = EINVAL; return NULL; }
   int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
   if (!length) { errno = EILSEQ; return NULL; }
   wchar_t *wide = (wchar_t *)malloc((size_t)length * sizeof *wide);
   if (!wide) return NULL;
   if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length)) {
      free(wide); errno = EILSEQ; return NULL;
   }
   return wide;
}
/* RootDirectory keeps relative opens tied to the directory object even if
 * another thread renames that directory during the operation. */
static HANDLE file_native_open(int dirfd, wchar_t *path, DWORD access,
                               DWORD creation, DWORD attributes)
{
   int absolute = path[0] == L'/' || path[0] == L'\\' ||
      (path[0] && path[1] == L':' && (path[2] == L'/' || path[2] == L'\\'));
   if (dirfd == LUNA_AT_FDCWD || absolute)
      return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, creation, attributes, NULL);
   intptr_t root = file_descriptor_handle(dirfd);
   if (root == -1) { SetLastError(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE; }
   FILE_ATTRIBUTE_TAG_INFO info;
   if (!GetFileInformationByHandleEx((HANDLE)root, FileAttributeTagInfo, &info, sizeof info) ||
       !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      SetLastError(ERROR_DIRECTORY); return INVALID_HANDLE_VALUE;
   }
   size_t length = wcslen(path);
   if (!length) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
   if (length > 32766) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
   for (wchar_t *at = path; *at; ++at) if (*at == L'/') *at = L'\\';
   UNICODE_STRING name = { (USHORT)(length * sizeof *path),
                          (USHORT)((length + 1) * sizeof *path), path };
   OBJECT_ATTRIBUTES object = { 0 };
   object.Length = sizeof object; object.RootDirectory = (HANDLE)root;
   object.ObjectName = &name; object.Attributes = 0x40;
   typedef NTSTATUS (NTAPI *create_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
      PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
   typedef ULONG (WINAPI *error_fn)(NTSTATUS);
   HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
   create_fn create = (create_fn)(void *)GetProcAddress(ntdll, "NtCreateFile");
   error_fn translate = (error_fn)(void *)GetProcAddress(ntdll, "RtlNtStatusToDosError");
   if (!create || !translate) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return INVALID_HANDLE_VALUE; }
   ULONG disposition = creation == CREATE_NEW ? 2 : creation == OPEN_EXISTING ? 1 :
      creation == OPEN_ALWAYS ? 3 : creation == TRUNCATE_EXISTING ? 4 : 5;
   ULONG options = 0x20; /* FILE_SYNCHRONOUS_IO_NONALERT */
   if (attributes & FILE_FLAG_WRITE_THROUGH) options |= 2;
   if (attributes & FILE_FLAG_BACKUP_SEMANTICS) options |= 0x4000;
   if (attributes & FILE_FLAG_OPEN_REPARSE_POINT) options |= 0x200000;
   HANDLE handle = INVALID_HANDLE_VALUE;
   IO_STATUS_BLOCK result;
   NTSTATUS status = create(&handle, access | SYNCHRONIZE, &object, &result, NULL,
      attributes & 0xffff, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      disposition, options, NULL, 0);
   if (status < 0) { SetLastError(translate(status)); return INVALID_HANDLE_VALUE; }
   return handle;
}
int luna_file_openat(int dirfd, const char *path, int flags, unsigned mode)

{
   if ((flags & LUNA_FILE_DIRECTORY) && (flags & (_O_WRONLY | _O_RDWR | _O_CREAT))) {
      errno = (flags & _O_CREAT) ? EINVAL : EISDIR; return -1;
   }
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   DWORD access = (flags & _O_RDWR) ? GENERIC_READ | GENERIC_WRITE :
                  (flags & _O_WRONLY) ? GENERIC_WRITE : GENERIC_READ;
   DWORD creation = (flags & _O_CREAT)
      ? ((flags & _O_EXCL) ? CREATE_NEW : (flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS)
      : (flags & _O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
   /* Inspect a no-follow handle before truncating. CREATE_ALWAYS cannot be
    * combined with OPEN_REPARSE_POINT, and TRUNCATE_EXISTING alters links. */
   if (flags & LUNA_FILE_NOFOLLOW) {
      if (creation == CREATE_ALWAYS) creation = OPEN_ALWAYS;
      if (creation == TRUNCATE_EXISTING) creation = OPEN_EXISTING;
   }
   DWORD attributes = FILE_ATTRIBUTE_NORMAL;
   if ((flags & _O_CREAT) && !(mode & 0200)) attributes = FILE_ATTRIBUTE_READONLY;
   if (flags & LUNA_FILE_SYNC) attributes |= FILE_FLAG_WRITE_THROUGH;
   if (flags & (LUNA_FILE_DIRECTORY | LUNA_FILE_NOFOLLOW)) attributes |= FILE_FLAG_BACKUP_SEMANTICS;
   if (flags & LUNA_FILE_NOFOLLOW) attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
   HANDLE handle = file_native_open(dirfd, wide, access, creation, attributes);
   DWORD error = GetLastError();
   free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   if (flags & (LUNA_FILE_DIRECTORY | LUNA_FILE_NOFOLLOW)) {
      FILE_ATTRIBUTE_TAG_INFO info;
      if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof info)) {
         error = GetLastError(); CloseHandle(handle); return luna_file_error(error);
      }
      if ((flags & LUNA_FILE_NOFOLLOW) && (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
         CloseHandle(handle); errno = ELOOP; return -1;
      }
      if ((flags & LUNA_FILE_DIRECTORY) && !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
         CloseHandle(handle); errno = ENOTDIR; return -1;
      }
   }
   if ((flags & LUNA_FILE_NOFOLLOW) && (flags & _O_TRUNC) && !SetEndOfFile(handle)) {
      error = GetLastError(); CloseHandle(handle); return luna_file_error(error);
   }
   int fd = _open_osfhandle((intptr_t)handle,
      (flags & (_O_RDONLY | _O_WRONLY | _O_RDWR | _O_APPEND)) | _O_BINARY | _O_NOINHERIT);
   if (fd < 0) { int saved = errno; CloseHandle(handle); errno = saved; }
   else if (file_status_register(fd, flags)) {
      int saved = errno; _close(fd); errno = saved; return -1;
   }
   return fd;
}
int luna_file_open(const char *path, int flags, unsigned mode)
{ return luna_file_openat(LUNA_AT_FDCWD, path, flags, mode); }
int luna_file_unlinkat(int dirfd, const char *path, int flags)
{
   if (flags & ~LUNA_AT_REMOVEDIR) { errno = EINVAL; return -1; }
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   HANDLE handle = file_native_open(dirfd, wide, DELETE | FILE_READ_ATTRIBUTES,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS);
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   FILE_ATTRIBUTE_TAG_INFO info;
   if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof info)) {
      error = GetLastError(); CloseHandle(handle); return luna_file_error(error);
   }
   int directory = (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
   int reparse = (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
   if ((flags & LUNA_AT_REMOVEDIR) ? (!directory || reparse) : (directory && !reparse)) {
      CloseHandle(handle); errno = flags & LUNA_AT_REMOVEDIR ? ENOTDIR : EISDIR; return -1;
   }
   FILE_DISPOSITION_INFO_EX disposition_ex = {
      FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
   };
   BOOL ok = SetFileInformationByHandle(handle, FileDispositionInfoEx,
                                        &disposition_ex, sizeof disposition_ex);
   if (!ok && (GetLastError() == ERROR_INVALID_PARAMETER || GetLastError() == ERROR_NOT_SUPPORTED)) {
      FILE_DISPOSITION_INFO disposition = { TRUE };
      ok = SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof disposition);
   }
   error = GetLastError(); CloseHandle(handle);
   return ok ? 0 : luna_file_error(error);
}
int luna_file_accessat(int dirfd, const char *path, int mode, int flags)
{
   if (mode & ~7 || flags & ~(LUNA_AT_NOFOLLOW | LUNA_AT_EACCESS)) { errno = EINVAL; return -1; }
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   DWORD access = FILE_READ_ATTRIBUTES | (mode & 4 ? GENERIC_READ : 0) |
      (mode & 2 ? GENERIC_WRITE : 0) | (mode & 1 ? GENERIC_EXECUTE : 0);
   HANDLE handle = file_native_open(dirfd, wide, access, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | (flags & LUNA_AT_NOFOLLOW ? FILE_FLAG_OPEN_REPARSE_POINT : 0));
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   CloseHandle(handle); return 0;
}

/* Read Microsoft's symlink/junction layouts without importing driver headers.
 * Validate offsets before converting the non-NUL-terminated UTF-16 target. */
static char *file_link_target(HANDLE handle, size_t *length)
{
   union { uint64_t align; unsigned char bytes[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]; } data;
   DWORD received;
   if (!DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0, data.bytes,
                        sizeof data.bytes, &received, NULL)) {
      DWORD error = GetLastError();
      if (error == ERROR_NOT_A_REPARSE_POINT) errno = EINVAL;
      else luna_file_error(error);
      return NULL;
   }
   DWORD tag; USHORT payload, offsets[4];
   if (received < 16) { errno = EIO; return NULL; }
   memcpy(&tag, data.bytes, 4); memcpy(&payload, data.bytes + 4, 2);
   memcpy(offsets, data.bytes + 8, sizeof offsets);
   size_t base = tag == IO_REPARSE_TAG_SYMLINK ? 20 : 16;
   if (tag != IO_REPARSE_TAG_SYMLINK && tag != IO_REPARSE_TAG_MOUNT_POINT) { errno = EINVAL; return NULL; }
   size_t offset = offsets[3] ? offsets[2] : offsets[0];
   size_t bytes = offsets[3] ? offsets[3] : offsets[1];
   size_t end = (size_t)payload + 8;
   if (end > received || base > end || (offset | bytes) & 1 ||
       offset > end - base || bytes > end - base - offset) { errno = EIO; return NULL; }
   const wchar_t *text = (const wchar_t *)(data.bytes + base + offset);
   int count = (int)(bytes / sizeof *text);
   /* Substitute names use the NT namespace; print names normally do not. */
   int unc = 0;
   if (!offsets[3] && count >= 4 && !wcsncmp(text, L"\\??\\", 4)) {
      text += 4; count -= 4;
      if (count >= 4 && !wcsncmp(text, L"UNC\\", 4)) {
         text += 4; count -= 4; unc = 2;
      }
   }
   int needed = count ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                            count, NULL, 0, NULL, NULL) : 0;
   if (count && !needed) { errno = EILSEQ; return NULL; }
   char *result = malloc((size_t)needed + (size_t)unc + 1);
   if (!result) { errno = ENOMEM; return NULL; }
   if (unc) { result[0] = '\\'; result[1] = '\\'; }
   if (needed && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, count,
                                     result + unc, needed, NULL, NULL)) {
      free(result); errno = EILSEQ; return NULL;
   }
   result[needed + unc] = 0; *length = (size_t)needed + (size_t)unc; return result;
}
ptrdiff_t luna_file_readlink(const char *path, char *buffer, size_t capacity)
{
   if (!capacity) { errno = EINVAL; return -1; }
   if (!buffer) { errno = EFAULT; return -1; }
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   HANDLE handle = file_native_open(LUNA_AT_FDCWD, wide, FILE_READ_ATTRIBUTES, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   size_t length;
   char *target = file_link_target(handle, &length);
   int saved = errno; CloseHandle(handle); errno = saved;
   if (!target) return -1;
   if (length > capacity) length = capacity;
   memcpy(buffer, target, length); free(target); return (ptrdiff_t)length;
}

static luna_file_time file_unix_time(LARGE_INTEGER value)
{
   uint64_t ticks = (uint64_t)value.QuadPart;
   luna_file_time time = { (int64_t)(ticks / 10000000) - 11644473600LL,
                           (int64_t)(ticks % 10000000) * 100 };
   return time;
}
static int file_handle_info(HANDLE handle, luna_file_info *info, int nofollow)
{
   SetLastError(ERROR_SUCCESS);
   DWORD kind = GetFileType(handle);
   if (kind == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS)
      return luna_file_error(GetLastError());
   luna_file_info result = { 0 };
   result.block_size = (uint32_t)luna_os_page_size();
   result.links = 1;
   if (kind == FILE_TYPE_PIPE || kind == FILE_TYPE_CHAR) {
      result.mode = (kind == FILE_TYPE_PIPE ? 0010000u : 0020000u) | 0600u;
      *info = result; return 0;
   }
   BY_HANDLE_FILE_INFORMATION identity;
   FILE_BASIC_INFO times;
   FILE_STANDARD_INFO storage;
   if (!GetFileInformationByHandle(handle, &identity) ||
       !GetFileInformationByHandleEx(handle, FileBasicInfo, &times, sizeof times) ||
       !GetFileInformationByHandleEx(handle, FileStandardInfo, &storage, sizeof storage))
      return luna_file_error(GetLastError());
   result.device = identity.dwVolumeSerialNumber;
   result.inode = ((uint64_t)identity.nFileIndexHigh << 32) | identity.nFileIndexLow;
   result.links = storage.NumberOfLinks;
   result.size = storage.EndOfFile.QuadPart;
   result.blocks = ((uint64_t)storage.AllocationSize.QuadPart + 511) / 512;
   if (times.FileAttributes & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED)) {
      FILE_COMPRESSION_INFO physical;
      if (!GetFileInformationByHandleEx(handle, FileCompressionInfo, &physical, sizeof physical))
         return luna_file_error(GetLastError());
      result.blocks = ((uint64_t)physical.CompressedFileSize.QuadPart + 511) / 512;
   }
   result.mode = (storage.Directory ? 0040000u | 0111u : 0100000u) |
      (times.FileAttributes & FILE_ATTRIBUTE_READONLY ? 0444u : 0666u);
   result.accessed = file_unix_time(times.LastAccessTime);
   result.modified = file_unix_time(times.LastWriteTime);
   result.changed = file_unix_time(times.ChangeTime);
   if (nofollow && (times.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      FILE_ATTRIBUTE_TAG_INFO tag;
      if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof tag))
         return luna_file_error(GetLastError());
      if (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
         size_t length;
         char *target = file_link_target(handle, &length);
         if (!target) return -1;
         free(target);
         result.mode = 0120000u | 0777u;
         result.size = (int64_t)length;
      }
   }
   *info = result; return 0;
}
int luna_file_fd_info(int fd, luna_file_info *info)
{
   if (!info) { errno = EFAULT; return -1; }
   if (fd < 0) { errno = EBADF; return -1; }
   if (lookup(fd) != INVALID_SOCKET) {
      luna_file_info result = { 0 };
      result.mode = 0140000u | 0600u; result.links = 1;
      result.block_size = (uint32_t)luna_os_page_size();
      *info = result; return 0;
   }
   intptr_t handle = file_descriptor_handle(fd);
   if (handle == -1) { errno = EBADF; return -1; }
   return file_handle_info((HANDLE)handle, info, 0);
}
int luna_file_infoat(int dirfd, const char *path, luna_file_info *info, int flags)
{
   if (flags & ~(LUNA_AT_NOFOLLOW | LUNA_AT_NO_AUTOMOUNT | LUNA_AT_EMPTY_PATH)) { errno = EINVAL; return -1; }
   if (!path || !info) { errno = EFAULT; return -1; }
   if (!*path && (flags & LUNA_AT_EMPTY_PATH) && dirfd != LUNA_AT_FDCWD)
      return luna_file_fd_info(dirfd, info);
   const char *query = !*path && (flags & LUNA_AT_EMPTY_PATH) ? "." : path;
   wchar_t *wide = luna_file_wide(query);
   if (!wide) return -1;
   HANDLE handle = file_native_open(dirfd, wide, FILE_READ_ATTRIBUTES, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | (flags & LUNA_AT_NOFOLLOW ? FILE_FLAG_OPEN_REPARSE_POINT : 0));
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   int result = file_handle_info(handle, info, flags & LUNA_AT_NOFOLLOW);
   int saved = errno; CloseHandle(handle); errno = saved; return result;
}

struct luna_directory {
   int fd, started, ready, ended;
   size_t position;
   luna_directory_entry entry;
   union { uint64_t alignment; unsigned char bytes[65536]; } batch;
};
luna_directory *luna_directory_open_fd(int fd)
{
   luna_file_info info;
   if (luna_file_fd_info(fd, &info)) return NULL;
   if ((info.mode & 0170000u) != 0040000u) { errno = ENOTDIR; return NULL; }
   luna_directory *directory = calloc(1, sizeof *directory);
   if (!directory) { errno = ENOMEM; return NULL; }
   directory->fd = fd;
   return directory;
}
luna_directory *luna_directory_open(const char *path)
{
   int fd = luna_file_open(path, O_RDONLY | LUNA_FILE_DIRECTORY | LUNA_FILE_CLOEXEC, 0);
   if (fd < 0) return NULL;
   luna_directory *directory = luna_directory_open_fd(fd);
   if (!directory) { int saved = errno; luna_fd_close(fd); errno = saved; }
   return directory;
}
luna_directory_entry *luna_directory_read(luna_directory *directory)
{
   if (!directory) { errno = EBADF; return NULL; }
   if (directory->ended) return NULL;
   if (!directory->ready) {
      memset(directory->batch.bytes, 0, sizeof directory->batch.bytes);
      HANDLE handle = (HANDLE)file_descriptor_handle(directory->fd);
      FILE_INFO_BY_HANDLE_CLASS query = directory->started
         ? FileIdBothDirectoryInfo : FileIdBothDirectoryRestartInfo;
      if (!GetFileInformationByHandleEx(handle, query, directory->batch.bytes,
                                        sizeof directory->batch.bytes)) {
         DWORD error = GetLastError();
         if (error == ERROR_NO_MORE_FILES) directory->ended = 1;
         else luna_file_error(error);
         return NULL;
      }
      directory->started = directory->ready = 1;
      directory->position = 0;
   }
   size_t offset = directory->position;
   const size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
   size_t available = sizeof directory->batch.bytes - offset;
   FILE_ID_BOTH_DIR_INFO *native = (FILE_ID_BOTH_DIR_INFO *)(directory->batch.bytes + offset);
   if (available < header || native->FileNameLength & 1 ||
       native->FileNameLength > available - header ||
       (native->NextEntryOffset && ((native->NextEntryOffset & 7) ||
         native->NextEntryOffset < header + native->FileNameLength ||
         native->NextEntryOffset > available - header))) {
      directory->ended = 1; errno = EIO; return NULL;
   }
   if (native->NextEntryOffset) directory->position += native->NextEntryOffset;
   else directory->ready = 0;
   luna_directory_entry *entry = &directory->entry;
   int count = (int)(native->FileNameLength / sizeof(wchar_t));
   int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native->FileName, count,
                                   entry->d_name, sizeof entry->d_name - 1, NULL, NULL);
   if (!length) { errno = GetLastError() == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : EILSEQ; return NULL; }
   entry->d_name[length] = 0;
   entry->d_ino = (uint64_t)native->FileId.QuadPart;
   entry->d_type = native->FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? LUNA_DT_DIR : LUNA_DT_REG;
   if (native->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      luna_file_info info;
      int saved = errno;
      entry->d_type = luna_file_infoat(directory->fd, entry->d_name, &info, LUNA_AT_NOFOLLOW)
         ? LUNA_DT_UNKNOWN : (unsigned char)((info.mode & 0170000u) >> 12);
      errno = saved;
   }
   return entry;
}
int luna_directory_close(luna_directory *directory)
{
   if (!directory) { errno = EBADF; return -1; }
   int result = luna_fd_close(directory->fd);
   free(directory); return result;
}
void luna_directory_rewind(luna_directory *directory)
{
   if (!directory) { errno = EBADF; return; }
   directory->started = directory->ready = directory->ended = 0;
   directory->position = 0;
}

static int file_native_chmod(HANDLE handle, unsigned mode)
{
   FILE_BASIC_INFO info;
   if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof info))
      return luna_file_error(GetLastError());
   DWORD attributes = info.FileAttributes;
   attributes = mode & 0222 ? attributes & ~FILE_ATTRIBUTE_READONLY : attributes | FILE_ATTRIBUTE_READONLY;
   memset(&info, 0, sizeof info);
   info.FileAttributes = attributes ? attributes : FILE_ATTRIBUTE_NORMAL;
   return SetFileInformationByHandle(handle, FileBasicInfo, &info, sizeof info)
      ? 0 : luna_file_error(GetLastError());
}
long luna_file_pathconf(const char *path, int name)
{
   if (name < 0 || name > LUNA_PC_SYNC_IO) { errno = EINVAL; return -1; }
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   HANDLE handle = file_native_open(LUNA_AT_FDCWD, wide, FILE_READ_ATTRIBUTES,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   long result = -1;
   DWORD length = 0, flags = 0;
   if (name == LUNA_PC_NAME_MAX || name == LUNA_PC_2_SYMLINKS) {
      if (!GetVolumeInformationByHandleW(handle, NULL, 0, NULL, &length, &flags, NULL, 0)) {
         error = GetLastError(); CloseHandle(handle); return luna_file_error(error);
      }
      /* The native limit counts UTF-16 units. A unit takes at most three
       * UTF-8 bytes; a surrogate pair takes four bytes for two units. */
      if (length > (DWORD)LONG_MAX / 3) {
         CloseHandle(handle); errno = EOVERFLOW; return -1;
      }
      result = name == LUNA_PC_NAME_MAX ? (long)length * 3 :
         !!(flags & FILE_SUPPORTS_REPARSE_POINTS);
   } else if (name == LUNA_PC_ALLOC_SIZE_MIN) {
      FILE_FS_SIZE_INFORMATION size;
      IO_STATUS_BLOCK status;
      NTSTATUS native = NtQueryVolumeInformationFile(handle, &status, &size, sizeof size, FileFsSizeInformation);
      if (native < 0) {
         CloseHandle(handle); return luna_file_error(RtlNtStatusToDosError(native));
      }
      uint64_t bytes = (uint64_t)size.SectorsPerAllocationUnit * size.BytesPerSector;
      if (!bytes || bytes > LONG_MAX) { CloseHandle(handle); errno = EOVERFLOW; return -1; }
      result = (long)bytes;
   } else if (name == LUNA_PC_NO_TRUNC) {
      result = 1; /* CreateFile rejects overlong components. */
   }
   CloseHandle(handle);
   /* Windows does not expose the remaining POSIX limits. As pathconf allows,
    * distinguish indeterminate values from actual lookup errors. */
   errno = 0;
   return result;
}

int luna_file_chmodat(int dirfd, const char *path, unsigned mode, int flags)
{
   if (flags & ~LUNA_AT_NOFOLLOW) { errno = EINVAL; return -1; }
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   HANDLE handle = file_native_open(dirfd, wide, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
      (flags & LUNA_AT_NOFOLLOW ? FILE_FLAG_OPEN_REPARSE_POINT : 0));
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   int result = file_native_chmod(handle, mode);
   int saved = errno; CloseHandle(handle); errno = saved;
   return result;
}
int luna_file_fd_chmod(int fd, unsigned mode)
{
   if (fd < 0) { errno = EBADF; return -1; }
   intptr_t original = file_descriptor_handle(fd);
   if (original == -1) { errno = EBADF; return -1; }
   HANDLE handle = ReOpenFile((HANDLE)original, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_BACKUP_SEMANTICS);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(GetLastError());
   int result = file_native_chmod(handle, mode);
   int saved = errno; CloseHandle(handle); errno = saved;
   return result;
}
int luna_file_symlink(const char *target, const char *path)
{
   wchar_t *destination = luna_file_wide(path);
   if (!destination) return -1;
   wchar_t *source = luna_file_wide(target);
   if (!source) { free(destination); return -1; }
   /* Resolve relative targets against the link's parent only to determine
    * its Windows directory flag. The stored target stays relative. */
   wchar_t *query = source;
   if (source[0] != L'/' && source[0] != L'\\' && !(source[0] && source[1] == L':')) {
      size_t parent = 0;
      for (size_t i = 0; destination[i]; ++i)
         if (destination[i] == L'/' || destination[i] == L'\\') parent = i + 1;
      size_t count = wcslen(source) + 1;
      query = malloc((parent + count) * sizeof *query);
      if (!query) { free(source); free(destination); return -1; }
      memcpy(query, destination, parent * sizeof *query);
      memcpy(query + parent, source, count * sizeof *query);
   }
   DWORD attributes = GetFileAttributesW(query);
   DWORD flags = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
   if (query != source) free(query);
   BOOL ok = CreateSymbolicLinkW(destination, source, flags | 2);
   DWORD error = GetLastError();
   if (!ok && error == ERROR_INVALID_PARAMETER) {
      ok = CreateSymbolicLinkW(destination, source, flags);
      error = GetLastError();
   }
   free(source); free(destination);
   return ok ? 0 : luna_file_error(error);
}

int luna_file_link(const char *from, const char *to)
{
   wchar_t *source = luna_file_wide(from);
   if (!source) return -1;
   wchar_t *target = luna_file_wide(to);
   if (!target) { free(source); return -1; }
   BOOL ok = CreateHardLinkW(target, source, NULL);
   DWORD error = GetLastError(); free(source); free(target);
   return ok ? 0 : luna_file_error(error);
}

int luna_file_mkdir(const char *path, unsigned mode)
{
   (void)mode;
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   int result = _wmkdir(wide);
   free(wide); return result;
}
/* Convert once before opening, so an invalid second timestamp cannot leave
 * the first timestamp changed. Both NOW fields use the same clock sample. */
static int file_native_times(const luna_file_time times[2], FILETIME native[2], const FILETIME *values[2])
{
   FILETIME now;
   GetSystemTimeAsFileTime(&now);
   for (int i = 0; i < 2; ++i) {
      values[i] = &native[i];
      if (!times || times[i].nanoseconds == LUNA_TIME_NOW) { native[i] = now; continue; }
      if (times[i].nanoseconds == LUNA_TIME_OMIT) { values[i] = NULL; continue; }
      int64_t seconds = times[i].seconds;
      int64_t ns = times[i].nanoseconds;
      if (ns < 0 || ns >= 1000000000 || seconds < -11644473600LL ||
          seconds > (int64_t)((UINT64_MAX - (uint32_t)ns / 100) / 10000000) - 11644473600LL) {
         errno = EINVAL; return -1;
      }
      uint64_t ticks = (uint64_t)(seconds + 11644473600LL) * 10000000 + (uint32_t)ns / 100;
      /* SetFileTime reserves zero and all-ones as special values. */
      if (!ticks || ticks == UINT64_MAX) { errno = EINVAL; return -1; }
      native[i].dwLowDateTime = (DWORD)ticks;
      native[i].dwHighDateTime = (DWORD)(ticks >> 32);
   }
   return 0;
}
int luna_file_timesat(int dirfd, const char *path, const luna_file_time times[2], int flags)
{
   if (flags & ~LUNA_AT_NOFOLLOW) { errno = EINVAL; return -1; }
   FILETIME native[2]; const FILETIME *values[2];
   if (file_native_times(times, native, values)) return -1;
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   HANDLE handle = file_native_open(dirfd, wide, FILE_WRITE_ATTRIBUTES, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | (flags & LUNA_AT_NOFOLLOW ? FILE_FLAG_OPEN_REPARSE_POINT : 0));
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   BOOL ok = SetFileTime(handle, NULL, values[0], values[1]);
   error = GetLastError(); CloseHandle(handle);
   return ok ? 0 : luna_file_error(error);
}
int luna_file_fd_times(int fd, const luna_file_time times[2])
{
   FILETIME native[2]; const FILETIME *values[2];
   if (file_native_times(times, native, values)) return -1;
   intptr_t original = file_descriptor_handle(fd);
   if (original == -1) { errno = EBADF; return -1; }
   HANDLE handle = ReOpenFile((HANDLE)original, FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_BACKUP_SEMANTICS);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(GetLastError());
   BOOL ok = SetFileTime(handle, NULL, values[0], values[1]);
   DWORD error = GetLastError(); CloseHandle(handle);
   return ok ? 0 : luna_file_error(error);
}
int luna_file_set_modified(const char *path, int64_t ms)
{
   if (ms < 0 || (uint64_t)ms > UINT64_MAX / 10000 - 11644473600000ULL) {
      errno = EINVAL; return -1;
   }
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, NULL);
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(error);
   uint64_t ticks = ((uint64_t)ms + 11644473600000ULL) * 10000;
   FILETIME modified = { (DWORD)ticks, (DWORD)(ticks >> 32) };
   BOOL ok = SetFileTime(handle, NULL, NULL, &modified);
   error = GetLastError(); CloseHandle(handle);
   return ok ? 0 : luna_file_error(error);
}
int luna_file_rename(const char *from, const char *to)
{
   wchar_t *source = luna_file_wide(from), *target = luna_file_wide(to);
   if (!source || !target) { free(source); free(target); return -1; }
   BOOL ok = MoveFileExW(source, target, MOVEFILE_REPLACE_EXISTING);
   DWORD error = GetLastError(); free(source); free(target);
   return ok ? 0 : luna_file_error(error);
}
int luna_file_unlink(const char *path)
{
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   int result = _wunlink(wide); free(wide); return result;
}
int luna_file_temp_directory(char *result, size_t capacity)
{
   wchar_t wide[MAX_PATH];
   DWORD length = GetTempPathW(MAX_PATH, wide);
   if (!length) return luna_file_error(GetLastError());
   if (length >= MAX_PATH) { errno = ENAMETOOLONG; return -1; }
   int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
   if (needed <= 0 || (size_t)needed > capacity) {
      errno = needed > 0 ? ENAMETOOLONG : EILSEQ; return -1;
   }
   WideCharToMultiByte(CP_UTF8, 0, wide, -1, result, needed, NULL, NULL);
   for (char *p = result; *p; ++p) if (*p == '\\') *p = '/';
   return 0;
}
FILE *luna_file_tmpfile(void)
{
   wchar_t directory[MAX_PATH], path[MAX_PATH];
   DWORD length = GetTempPathW(MAX_PATH, directory);
   if (!length) { luna_file_error(GetLastError()); return NULL; }
   if (length >= MAX_PATH) { errno = ENAMETOOLONG; return NULL; }
   if (!GetTempFileNameW(directory, L"lna", 0, path)) {
      luna_file_error(GetLastError()); return NULL;
   }
   HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, TRUNCATE_EXISTING,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
   if (handle == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError(); DeleteFileW(path); luna_file_error(error); return NULL;
   }
   int fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY | _O_NOINHERIT);
   if (fd < 0) { int error = errno; CloseHandle(handle); errno = error; return NULL; }
   if (file_status_register(fd, _O_RDWR)) {
      int saved = errno; _close(fd); errno = saved; return NULL;
   }
   FILE *stream = _fdopen(fd, "w+b");
   if (!stream) { int error = errno; luna_fd_close(fd); errno = error; }
   return stream;
}
FILE *luna_file_memory_reader(const void *buffer, size_t length)
{
   FILE *stream = luna_file_tmpfile();
   if (!stream) return NULL;
   if ((length && fwrite(buffer, 1, length, stream) != length) || fflush(stream) ||
       _fseeki64(stream, 0, SEEK_SET)) { luna_file_fclose(stream); return NULL; }
   int copy = luna_fd_dup(_fileno(stream), 0, 1);
   int error = errno; luna_file_fclose(stream); errno = error;
   if (copy < 0) return NULL;
   FILE *reader = _fdopen(copy, "rb");
   if (!reader) { error = errno; luna_fd_close(copy); errno = error; }
   return reader;
}
char *luna_file_mkdtemp(char *pattern)
{
   size_t length = strlen(pattern);
   if (length < 6 || memcmp(pattern + length - 6, "XXXXXX", 6)) {
      errno = EINVAL; return NULL;
   }
   static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
   for (int attempt = 0; attempt < 128; ++attempt) {
      unsigned char random[6];
      if (!RtlGenRandom(random, sizeof random)) { errno = EIO; return NULL; }
      for (int i = 0; i < 6; ++i) pattern[length - 6 + i] = alphabet[random[i] % 62];
      if (!luna_file_mkdir(pattern, 0700)) return pattern;
      if (errno != EEXIST) return NULL;
   }
   errno = EEXIST; return NULL;
}
int luna_file_mkstemps(char *pattern, int suffix_length)
{
   size_t length = strlen(pattern);
   if (suffix_length < 0 || (size_t)suffix_length > length ||
       length - (size_t)suffix_length < 6) { errno = EINVAL; return -1; }
   char *slot = pattern + length - (size_t)suffix_length - 6;
   if (memcmp(slot, "XXXXXX", 6)) { errno = EINVAL; return -1; }
   static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
   for (int attempt = 0; attempt < 128; ++attempt) {
      unsigned char random[6];
      if (!RtlGenRandom(random, sizeof random)) { errno = EIO; return -1; }
      for (int i = 0; i < 6; ++i) slot[i] = alphabet[random[i] % 62];
      int fd = luna_file_open(pattern, O_RDWR | O_CREAT | O_EXCL, 0600);
      if (fd >= 0 || errno != EEXIST) return fd;
   }
   errno = EEXIST; return -1;
}
char *luna_file_realpath(const char *path, char *result, size_t capacity)
{
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return NULL;
   HANDLE handle = CreateFileW(wide, 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, NULL);
   DWORD error = GetLastError(); free(wide);
   if (handle == INVALID_HANDLE_VALUE) { luna_file_error(error); return NULL; }
   DWORD length = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
   wchar_t *final = length ? (wchar_t *)malloc(((size_t)length + 1) * sizeof *final) : NULL;
   if (!final) { CloseHandle(handle); errno = length ? ENOMEM : EIO; return NULL; }
   DWORD count = GetFinalPathNameByHandleW(handle, final, length + 1, FILE_NAME_NORMALIZED);
   error = GetLastError(); CloseHandle(handle);
   if (!count || count > length) { free(final); luna_file_error(error); return NULL; }
   const wchar_t *text = final;
   if (!wcsncmp(text, L"\\\\?\\UNC\\", 8)) { final[6] = L'\\'; text = final + 6; }
   else if (!wcsncmp(text, L"\\\\?\\", 4)) text += 4;
   int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
   if (needed <= 0 || (size_t)needed > capacity) {
      free(final); errno = needed > 0 ? ENAMETOOLONG : EILSEQ; return NULL;
   }
   WideCharToMultiByte(CP_UTF8, 0, text, -1, result, needed, NULL, NULL);
   free(final);
   for (char *p = result; *p; ++p) if (*p == '\\') *p = '/';
   return result;
}
int luna_file_sync(int fd, int data_only)
{ (void)data_only; return _commit(fd); }
static ptrdiff_t file_positional_io(int fd, void *buffer, size_t length, int64_t offset, int writing)
{
   if (offset < 0) { errno = EINVAL; return -1; }
   intptr_t native = file_descriptor_handle(fd);
   if (native == -1) { errno = EBADF; return -1; }
   if (GetFileType((HANDLE)native) != FILE_TYPE_DISK) { errno = ESPIPE; return -1; }
   typedef NTSTATUS (NTAPI *query_file_fn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                          FILE_INFORMATION_CLASS);
   query_file_fn query = (query_file_fn)(void *)GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
   FILE_ACCESS_INFORMATION access;
   IO_STATUS_BLOCK status;
   if (!query || query((HANDLE)native, &status, &access, sizeof access,
                       FileAccessInformation) < 0) { errno = EIO; return -1; }
   if (!(access.AccessFlags & (writing ? FILE_WRITE_DATA : FILE_READ_DATA))) { errno = EBADF; return -1; }
   /* A separate file handle keeps the caller's file pointer unchanged.
    * DuplicateHandle shares that pointer, so it cannot implement positional I/O. */
   HANDLE handle = ReOpenFile((HANDLE)native, writing ? GENERIC_WRITE : GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
   if (handle == INVALID_HANDLE_VALUE) return luna_file_error(GetLastError());
   OVERLAPPED operation = { 0 };
   operation.Offset = (DWORD)offset;
   operation.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
   operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
   if (!operation.hEvent) {
      DWORD error = GetLastError(); CloseHandle(handle); return luna_file_error(error);
   }
   DWORD count = 0, request = length > INT_MAX ? INT_MAX : (DWORD)length;
   BOOL ok = writing ? WriteFile(handle, buffer, request, &count, &operation)
                     : ReadFile(handle, buffer, request, &count, &operation);
   DWORD error = ok ? ERROR_SUCCESS : GetLastError();
   if (!ok && error == ERROR_IO_PENDING) {
      ok = GetOverlappedResult(handle, &operation, &count, TRUE);
      error = ok ? ERROR_SUCCESS : GetLastError();
   }
   CloseHandle(operation.hEvent); CloseHandle(handle);
   if (ok || (!writing && error == ERROR_HANDLE_EOF)) return count;
   return luna_file_error(error);
}
ptrdiff_t luna_file_pread(int fd, void *buffer, size_t length, int64_t offset)
{ return file_positional_io(fd, buffer, length, offset, 0); }
ptrdiff_t luna_file_pwrite(int fd, const void *buffer, size_t length, int64_t offset)
{ return file_positional_io(fd, (void *)buffer, length, offset, 1); }

int luna_file_lock(int fd, int64_t start, int64_t length,
                                 int shared, int nonblocking, int unlock)
{
   if (start < 0 || length < 0 || length > INT64_MAX - start) {
      errno = EINVAL; return -1;
   }
   intptr_t handle = file_descriptor_handle(fd);
   if (handle == -1) { errno = EBADF; return -1; }
   if (!length) return 0;
   OVERLAPPED region = { 0 };
   region.Offset = (DWORD)start;
   region.OffsetHigh = (DWORD)((uint64_t)start >> 32);
   DWORD flags = (shared ? 0 : LOCKFILE_EXCLUSIVE_LOCK) |
                 (nonblocking ? LOCKFILE_FAIL_IMMEDIATELY : 0);
   BOOL ok = unlock
      ? UnlockFileEx((HANDLE)handle, 0, (DWORD)length, (DWORD)((uint64_t)length >> 32), &region)
      : LockFileEx((HANDLE)handle, flags, 0, (DWORD)length, (DWORD)((uint64_t)length >> 32), &region);
   if (ok) return 0;
   switch (GetLastError()) {
   case ERROR_LOCK_VIOLATION: errno = EAGAIN; break;
   case ERROR_ACCESS_DENIED: errno = EACCES; break;
   case ERROR_INVALID_HANDLE: errno = EBADF; break;
   case ERROR_INVALID_PARAMETER: errno = EINVAL; break;
   default: errno = EIO; break;
   }
   return -1;
}

/* flock belongs to the open file description, including aliases made before
 * the first lock. Keep a private duplicate alive until its final alias closes.
 * A byte beyond the signed 64-bit file offset range is the lock protocol:
 * Windows byte locks otherwise forbid ordinary I/O, unlike advisory flock.
 * Separate opens/processes still contend through the kernel, not this table. */
struct file_description_state {
   unsigned references;
   HANDLE handle;
   SRWLOCK operation_lock;
   int kind, status, status_known;
};
static struct {
   HANDLE handle;
   struct file_description_state *state;
} file_descriptions[8192];
static SRWLOCK file_description_lock = SRWLOCK_INIT;

static void file_description_release_locked(struct file_description_state *state)
{
   if (state && !--state->references) {
      if (state->kind) {
         OVERLAPPED region = { 0 }; region.OffsetHigh = 0x80000000;
         UnlockFileEx(state->handle, 0, 1, 0, &region);
      }
      CloseHandle(state->handle); free(state);
   }
}
static void file_description_release(struct file_description_state *state)
{
   int saved = errno;
   AcquireSRWLockExclusive(&file_description_lock);
   file_description_release_locked(state);
   ReleaseSRWLockExclusive(&file_description_lock);
   errno = saved;
}
static void file_description_forget(int fd, HANDLE handle)
{
   if (fd < 0 || fd >= 8192) return;
   int saved = errno;
   AcquireSRWLockExclusive(&file_description_lock);
   if (file_descriptions[fd].state && (!handle || file_descriptions[fd].handle == handle)) {
      file_description_release_locked(file_descriptions[fd].state);
      file_descriptions[fd].state = NULL;
   }
   ReleaseSRWLockExclusive(&file_description_lock);
   errno = saved;
}
/* Return a temporary reference. The descriptor also keeps its own reference. */
static struct file_description_state *file_description_acquire(int fd, HANDLE handle)
{
   if (fd < 0 || fd >= 8192) { errno = EBADF; return NULL; }
   AcquireSRWLockExclusive(&file_description_lock);
   if (file_descriptions[fd].state && file_descriptions[fd].handle != handle) {
      file_description_release_locked(file_descriptions[fd].state);
      file_descriptions[fd].state = NULL;
   }
   struct file_description_state *state = file_descriptions[fd].state;
   if (!state) {
      state = calloc(1, sizeof *state);
      if (!state) { ReleaseSRWLockExclusive(&file_description_lock); errno = ENOMEM; return NULL; }
      if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                           &state->handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
         DWORD error = GetLastError(); free(state);
         ReleaseSRWLockExclusive(&file_description_lock); luna_file_error(error); return NULL;
      }
      InitializeSRWLock(&state->operation_lock);
      state->references = 1;
      file_descriptions[fd].handle = handle; file_descriptions[fd].state = state;
   }
   ++state->references;
   ReleaseSRWLockExclusive(&file_description_lock);
   return state;
}
static void file_description_duplicate(int target, HANDLE handle, struct file_description_state *state)
{
   AcquireSRWLockExclusive(&file_description_lock);
   file_description_release_locked(file_descriptions[target].state);
   file_descriptions[target].state = state; file_descriptions[target].handle = handle;
   if (state) ++state->references;
   ReleaseSRWLockExclusive(&file_description_lock);
}
static int file_status_register(int fd, int flags)
{
   HANDLE handle = (HANDLE)file_descriptor_handle(fd);
   if (handle == INVALID_HANDLE_VALUE) return -1;
   file_description_forget(fd, NULL);
   struct file_description_state *state = file_description_acquire(fd, handle);
   if (!state) return -1;
   AcquireSRWLockExclusive(&file_description_lock);
   state->status = flags & (3 | _O_APPEND | LUNA_FILE_NONBLOCK |
      LUNA_FILE_SYNC | LUNA_FILE_DIRECTORY | LUNA_FILE_NOFOLLOW);
   state->status_known = 1;
   ReleaseSRWLockExclusive(&file_description_lock);
   file_description_release(state);
   return 0;
}
static int file_status_nonblock(int fd, HANDLE handle, int enabled, int set)
{
   if (fd < 0 || fd >= 8192) { errno = EBADF; return -1; }
   struct file_description_state *lease = set ? file_description_acquire(fd, handle) : NULL;
   if (set && !lease) return -1;
   AcquireSRWLockExclusive(&file_description_lock);
   struct file_description_state *state = file_descriptions[fd].handle == handle
      ? file_descriptions[fd].state : NULL;
   int result = 0;
   if (state) {
      if (set) state->status = enabled ? state->status | LUNA_FILE_NONBLOCK
                                      : state->status & ~LUNA_FILE_NONBLOCK;
      else result = !!(state->status & LUNA_FILE_NONBLOCK);
   }
   ReleaseSRWLockExclusive(&file_description_lock);
   file_description_release(lease);
   return result;
}
/* UCRT has no public F_SETFL. Rebuild each alias from a CRT descriptor with
 * the new append mode while duplicating the same native file object. This
 * preserves the shared position and lets existing FILE streams see the new
 * mode too. Per-descriptor inheritance and all other status flags stay put. */
static int file_status_append_locked(int fd, struct file_description_state *state, int append)
{
   struct { int fd; DWORD inherit; } aliases[8192];
   size_t count = 0, changed = 0;
   for (int i = 0; i < 8192; ++i) {
      if (file_descriptions[i].state != state) continue;
      if ((HANDLE)file_descriptor_handle(i) != file_descriptions[i].handle) continue;
      DWORD flags;
      if (!GetHandleInformation(file_descriptions[i].handle, &flags))
         return luna_file_error(GetLastError());
      aliases[count].fd = i; aliases[count++].inherit = flags & HANDLE_FLAG_INHERIT;
   }
   int backup = _dup(fd);
   if (backup < 0) return -1;
   /* Query the text/binary mode on the backup, never change the caller's. */
   int mode = _setmode(backup, _O_BINARY);
   if (mode < 0) { int saved = errno; _close(backup); errno = saved; return -1; }
   if (_setmode(backup, mode) < 0) {
      int saved = errno; _close(backup); errno = saved; return -1;
   }
   HANDLE native;
   if (!DuplicateHandle(GetCurrentProcess(), state->handle, GetCurrentProcess(),
                        &native, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      DWORD error = GetLastError(); _close(backup); return luna_file_error(error);
   }
   int replacement = _open_osfhandle((intptr_t)native,
      (state->status & 3) | (append ? _O_APPEND : 0) | mode | _O_NOINHERIT);
   if (replacement < 0) {
      int saved = errno; CloseHandle(native); _close(backup); errno = saved; return -1;
   }
   int error = 0;
   for (; changed < count; ++changed) {
      int alias = aliases[changed].fd;
      if (_dup2(replacement, alias)) { error = errno; break; }
      file_descriptions[alias].handle = (HANDLE)file_descriptor_handle(alias);
      if (!SetHandleInformation(file_descriptions[alias].handle, HANDLE_FLAG_INHERIT,
                                aliases[changed].inherit)) {
         error = EIO; ++changed; break;
      }
   }
   if (error) {
      while (changed) {
         --changed;
         int alias = aliases[changed].fd;
         if (_dup2(backup, alias)) { state->status_known = 0; continue; }
         file_descriptions[alias].handle = (HANDLE)file_descriptor_handle(alias);
         if (!SetHandleInformation(file_descriptions[alias].handle, HANDLE_FLAG_INHERIT,
                                   aliases[changed].inherit)) state->status_known = 0;
      }
   }
   _close(replacement); _close(backup);
   if (error) { errno = error; return -1; }
   return 0;
}
FILE *luna_file_fopen(const char *path, const char *mode)
{
   if (!mode || !*mode) { errno = EINVAL; return NULL; }
   int flags;
   switch (*mode) {
   case 'r': flags = _O_RDONLY; break;
   case 'w': flags = _O_WRONLY | _O_CREAT | _O_TRUNC; break;
   case 'a': flags = _O_WRONLY | _O_CREAT | _O_APPEND; break;
   default: errno = EINVAL; return NULL;
   }
   for (const char *p = mode + 1; *p; ++p) {
      switch (*p) {
      case '+': flags = (flags & ~3) | _O_RDWR; break;
      case 'b': break; /* Android streams are byte-oriented. */
      case 'e': flags |= LUNA_FILE_CLOEXEC; break;
      case 'x': flags |= _O_EXCL; break;
      default: errno = EINVAL; return NULL;
      }
   }
   int fd = luna_file_open(path, flags, 0666);
   if (fd < 0) return NULL;
   char native_mode[4] = { *mode, 'b', (flags & 3) == _O_RDWR ? '+' : 0, 0 };
   FILE *stream = _fdopen(fd, native_mode);
   if (!stream) { int saved = errno; luna_fd_close(fd); errno = saved; }
   return stream;
}
int luna_file_flock(int fd, int operation)
{
   int kind = operation & ~LUNA_LOCK_NONBLOCK;
   if (kind != LUNA_LOCK_SHARED && kind != LUNA_LOCK_EXCLUSIVE &&
       kind != LUNA_LOCK_UNLOCK) { errno = EINVAL; return -1; }
   intptr_t native = file_descriptor_handle(fd);
   if (native == -1) { errno = EBADF; return -1; }
   HANDLE handle = (HANDLE)native;
   if (lookup(fd) != INVALID_SOCKET || GetFileType(handle) != FILE_TYPE_DISK) {
      errno = ENOTSUP; return -1;
   }
   struct file_description_state *state = file_description_acquire(fd, handle);
   if (!state) return -1;
   AcquireSRWLockExclusive(&state->operation_lock);
   OVERLAPPED region = { 0 }; region.OffsetHigh = 0x80000000;
   int result = 0;
   if (state->kind && (kind == LUNA_LOCK_UNLOCK || state->kind != kind)) {
      if (!UnlockFileEx(state->handle, 0, 1, 0, &region))
         result = luna_file_error(GetLastError());
      else state->kind = 0;
   }
   /* Conversion removes the old lock before waiting, as flock does. */
   if (!result && kind != LUNA_LOCK_UNLOCK && state->kind != kind) {
      DWORD flags = (kind == LUNA_LOCK_EXCLUSIVE ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
                   (operation & LUNA_LOCK_NONBLOCK ? LOCKFILE_FAIL_IMMEDIATELY : 0);
      if (!LockFileEx(state->handle, flags, 0, 1, 0, &region)) {
         DWORD error = GetLastError();
         if (error == ERROR_LOCK_VIOLATION) { errno = EWOULDBLOCK; result = -1; }
         else result = luna_file_error(error);
      } else state->kind = kind;
   }
   ReleaseSRWLockExclusive(&state->operation_lock);
   file_description_release(state);
   return result;
}
int luna_file_fclose(FILE *stream)
{
   if (!stream) { errno = EINVAL; return EOF; }
   int fd = _fileno(stream);
   intptr_t handle = file_descriptor_handle(fd);
   AcquireSRWLockExclusive(&socket_lock);
   int result = fclose(stream);
   file_description_forget(fd, (HANDLE)handle);
   ReleaseSRWLockExclusive(&socket_lock);
   return result;
}
int luna_file_record_control(int fd, int command, luna_file_record_lock *lock)
{
   if (!lock) { errno = EFAULT; return -1; }
   if (command < LUNA_RECORD_QUERY || command > LUNA_RECORD_WAIT ||
       lock->type < LUNA_RECORD_READ || lock->type > LUNA_RECORD_UNLOCK ||
       (lock->whence != SEEK_SET && lock->whence != SEEK_CUR && lock->whence != SEEK_END)) {
      errno = EINVAL; return -1;
   }
   if (file_descriptor_handle(fd) == -1) { errno = EBADF; return -1; }
   /* LockFileEx is a mandatory, handle-owned lock. It cannot implement
    * F_GETLK's owner/range query or POSIX process locks (merging/splitting and
    * release when any fd for that file closes). Do not report a fictitious
    * unlocked range or silently substitute Java's handle-owned byte locks. */
   errno = ENOTSUP; return -1;
}
FILE *luna_file_memstream_begin(luna_file_memstream *writer)
{
   memset(writer, 0, sizeof *writer);
   writer->stream = luna_file_tmpfile();

   return writer->stream;
}
/* Finish the stream and transfer the allocated, NUL-terminated bytes. */
int luna_file_memstream_finish(luna_file_memstream *writer,
                                             char **data, size_t *length)
{
   if (!writer->stream) { errno = EINVAL; return -1; }
   int error = 0;
   if (ferror(writer->stream)) error = EIO;
   if (!error && (fflush(writer->stream) || _fseeki64(writer->stream, 0, SEEK_END))) error = EIO;
   __int64 size = error ? -1 : _ftelli64(writer->stream);
   if (!error && (size < 0 || (uint64_t)size >= SIZE_MAX)) error = EOVERFLOW;
   if (!error) {
      writer->data = (char *)malloc((size_t)size + 1);
      if (!writer->data) error = ENOMEM;
      else if (_fseeki64(writer->stream, 0, SEEK_SET) ||
               fread(writer->data, 1, (size_t)size, writer->stream) != (size_t)size) error = EIO;
      else { writer->length = (size_t)size; writer->data[size] = 0; }
   }

   if (luna_file_fclose(writer->stream) && !error) error = errno ? errno : EIO;
   writer->stream = NULL;
   if (error) { free(writer->data); writer->data = NULL; errno = error; return -1; }
   *data = writer->data; *length = writer->length; writer->data = NULL;
   return 0;
}
/* Winsock descriptors and I/O. */
struct socket_state { unsigned references; int nonblock, append; };
struct socket_fd { int fd; SOCKET socket; struct socket_state *state; struct socket_fd *next; };
static struct socket_fd *sockets;
static INIT_ONCE startup_once = INIT_ONCE_STATIC_INIT;
static int startup_error;

static int socket_errno(int error)
{
   switch (error) {
   case 0: return 0;
   case WSAEINTR: return EINTR;
   case WSAEBADF: return EBADF;
   case WSAEACCES: return EACCES;
   case WSAEFAULT: return EFAULT;
   case WSAEINVAL: return EINVAL;
   case WSAEMFILE: return EMFILE;
   case WSAEWOULDBLOCK: return EAGAIN;
   case WSAEINPROGRESS: return EINPROGRESS;
   case WSAEALREADY: return EALREADY;
   case WSAENOTSOCK: return ENOTSOCK;
   case WSAEDESTADDRREQ: return EDESTADDRREQ;
   case WSAEMSGSIZE: return EMSGSIZE;
   case WSAEPROTOTYPE: return EPROTOTYPE;
   case WSAENOPROTOOPT: return ENOPROTOOPT;
   case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
   case WSAEOPNOTSUPP: return EOPNOTSUPP;
   case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
   case WSAEADDRINUSE: return EADDRINUSE;
   case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
   case WSAENETDOWN: return ENETDOWN;
   case WSAENETUNREACH: return ENETUNREACH;
   case WSAENETRESET: return ENETRESET;
   case WSAECONNABORTED: return ECONNABORTED;
   case WSAECONNRESET: return ECONNRESET;
   case WSAENOBUFS: return ENOBUFS;
   case WSAEISCONN: return EISCONN;
   case WSAENOTCONN: return ENOTCONN;
   case WSAETIMEDOUT: return ETIMEDOUT;
   case WSAECONNREFUSED: return ECONNREFUSED;
   case WSAEHOSTUNREACH: return EHOSTUNREACH;
   default: return EIO;
   }
}
static int failed(void) { errno = socket_errno(WSAGetLastError()); return -1; }
static BOOL CALLBACK startup(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
   (void)once; (void)parameter; (void)context;
   WSADATA data;
   startup_error = WSAStartup(MAKEWORD(2, 2), &data);
   return TRUE;
}
int luna_socket_startup(void)
{
   InitOnceExecuteOnce(&startup_once, startup, NULL, NULL);
   if (startup_error) { errno = socket_errno(startup_error); return -1; }
   return 0;
}
static SOCKET lookup(int fd)
{
   SOCKET result = INVALID_SOCKET;
   AcquireSRWLockShared(&socket_lock);
   for (struct socket_fd *p = sockets; p; p = p->next)
      if (p->fd == fd) { result = p->socket; break; }
   ReleaseSRWLockShared(&socket_lock);
   return result;
}
intptr_t luna_socket_native(int fd)
{
   SOCKET socket = lookup(fd);
   if (socket == INVALID_SOCKET) { errno = ENOTSOCK; return -1; }
   return (intptr_t)socket;
}
static int wrap(SOCKET socket)
{
   if (socket == INVALID_SOCKET) return failed();
   struct socket_fd *record = calloc(1, sizeof *record);
   if (!record) { closesocket(socket); errno = ENOMEM; return -1; }
   record->state = calloc(1, sizeof *record->state);
   if (!record->state) { free(record); closesocket(socket); errno = ENOMEM; return -1; }
   record->state->references = 1;
   int fd = _open("NUL", _O_RDWR | _O_BINARY | _O_NOINHERIT);
   if (fd < 0) { int error = errno; free(record->state); free(record); closesocket(socket); errno = error; return -1; }
   record->fd = fd; record->socket = socket;
   AcquireSRWLockExclusive(&socket_lock);
   record->next = sockets; sockets = record;
   ReleaseSRWLockExclusive(&socket_lock);
   return fd;
}
int luna_socket_open(int family, int type, int protocol)
{
   if (luna_socket_startup()) return -1;
   return wrap(WSASocketW(family, type, protocol, NULL, 0,
                         WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
}
int luna_socket_pair(int family, int type, int protocol, int pair[2])
{
   if (!pair) { errno = EFAULT; return -1; }
   if (family != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
   if (protocol) { errno = EPROTONOSUPPORT; return -1; }
   if (type != SOCK_STREAM && type != SOCK_DGRAM) { errno = EOPNOTSUPP; return -1; }
   int a = -1, b = -1, listener = -1;
   struct sockaddr_in address = { 0 }, peer = { 0 };
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   socklen_t length = sizeof address;
   if (type == SOCK_STREAM) {
      listener = luna_socket_open(AF_INET, SOCK_STREAM, 0);
      if (listener < 0 || luna_socket_bind(listener, (struct sockaddr *)&address, length) ||
          luna_socket_name(listener, (struct sockaddr *)&address, &length) ||
          luna_socket_listen(listener, 1)) goto fail;
      a = luna_socket_open(AF_INET, SOCK_STREAM, 0);
      if (a < 0 || luna_socket_connect(a, (struct sockaddr *)&address, length)) goto fail;
      b = luna_socket_accept(listener, NULL, NULL);
      if (b < 0) goto fail;
      luna_fd_close(listener);
   } else {
      a = luna_socket_open(AF_INET, SOCK_DGRAM, 0);
      b = luna_socket_open(AF_INET, SOCK_DGRAM, 0);
      peer = address;
      if (a < 0 || b < 0 ||
          luna_socket_bind(a, (struct sockaddr *)&address, length) ||
          luna_socket_bind(b, (struct sockaddr *)&peer, length) ||
          luna_socket_name(a, (struct sockaddr *)&address, &length) ||
          luna_socket_name(b, (struct sockaddr *)&peer, &length) ||
          luna_socket_connect(a, (struct sockaddr *)&peer, length) ||
          luna_socket_connect(b, (struct sockaddr *)&address, length)) goto fail;
   }
   pair[0] = a; pair[1] = b;
   return 0;
fail: {
   int error = errno;
   if (listener >= 0) luna_fd_close(listener);
   if (a >= 0) luna_fd_close(a);
   if (b >= 0) luna_fd_close(b);
   errno = error; return -1;
}
}

int luna_socket_accept(int fd, struct sockaddr *address, socklen_t *length)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   SOCKET client = accept(socket, address, length);
   if (client == INVALID_SOCKET) return failed();
   /* POSIX accept does not inherit the listener's nonblocking flag. */
   u_long zero = 0;
   if (ioctlsocket(client, FIONBIO, &zero)) {
      int error = WSAGetLastError(); closesocket(client);
      errno = socket_errno(error); return -1;
   }
   if (!SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0)) {
      closesocket(client); errno = EIO; return -1;
   }
   return wrap(client);
}
#define SOCKET_CALL(expression) do { \
   SOCKET socket = (SOCKET)luna_socket_native(fd); \
   if (socket == INVALID_SOCKET) return -1; \
   int result = (expression); return result == SOCKET_ERROR ? failed() : result; \
} while (0)
int luna_socket_bind(int fd, const struct sockaddr *a, socklen_t n) { SOCKET_CALL(bind(socket,a,n)); }
int luna_socket_listen(int fd, int backlog) { SOCKET_CALL(listen(socket,backlog)); }
int luna_socket_shutdown(int fd, int how) { SOCKET_CALL(shutdown(socket,how)); }
int luna_socket_name(int fd, struct sockaddr *a, socklen_t *n) { SOCKET_CALL(getsockname(socket,a,n)); }
int luna_socket_peer(int fd, struct sockaddr *a, socklen_t *n) { SOCKET_CALL(getpeername(socket,a,n)); }
int luna_socket_connect(int fd, const struct sockaddr *a, socklen_t n)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   int result = connect(socket, a, n);
   if (result == SOCKET_ERROR) {
      int error = WSAGetLastError();
      errno = error == WSAEWOULDBLOCK ? EINPROGRESS : socket_errno(error);
      return -1;
   }
   return result;
}
int luna_socket_nonblock(int fd, int enabled)
{
   AcquireSRWLockExclusive(&socket_lock);
   struct socket_fd *record = sockets;
   while (record && record->fd != fd) record = record->next;
   if (!record) { ReleaseSRWLockExclusive(&socket_lock); errno = ENOTSOCK; return -1; }
   u_long mode = !!enabled;
   int result = ioctlsocket(record->socket, FIONBIO, &mode);
   int error = result ? WSAGetLastError() : 0;
   if (!result) record->state->nonblock = !!enabled;
   ReleaseSRWLockExclusive(&socket_lock);
   if (result) { errno = socket_errno(error); return -1; }
   return 0;
}
int luna_socket_get_nonblock(int fd)
{
   int result = -1;
   AcquireSRWLockShared(&socket_lock);
   for (struct socket_fd *p = sockets; p; p = p->next)
      if (p->fd == fd) { result = p->state->nonblock; break; }
   ReleaseSRWLockShared(&socket_lock);
   if (result < 0) errno = ENOTSOCK;
   return result;
}
/* File descriptors include CRT files, named pipes and registered sockets. */
int luna_fd_get_nonblock(int fd)
{
   if (lookup(fd) != INVALID_SOCKET) return luna_socket_get_nonblock(fd);
   intptr_t native = file_descriptor_handle(fd);
   if (native == -1) { errno = EBADF; return -1; }
   HANDLE handle = (HANDLE)native;
   if (GetFileType(handle) != FILE_TYPE_PIPE) return file_status_nonblock(fd, handle, 0, 0);
   DWORD mode;
   if (!GetNamedPipeHandleStateW(handle, &mode, NULL, NULL, NULL, NULL, 0))
      return luna_file_error(GetLastError());
   return !!(mode & PIPE_NOWAIT);
}
int luna_fd_nonblock(int fd, int enabled)
{
   if (lookup(fd) != INVALID_SOCKET) return luna_socket_nonblock(fd, enabled);
   intptr_t native = file_descriptor_handle(fd);
   if (native == -1) { errno = EBADF; return -1; }
   HANDLE handle = (HANDLE)native;
   /* Regular files are always ready, as with POSIX O_NONBLOCK. */
   if (GetFileType(handle) != FILE_TYPE_PIPE) return file_status_nonblock(fd, handle, enabled, 1);
   DWORD mode;
   if (!GetNamedPipeHandleStateW(handle, &mode, NULL, NULL, NULL, NULL, 0))
      return luna_file_error(GetLastError());
   mode = enabled ? mode | PIPE_NOWAIT : mode & ~PIPE_NOWAIT;
   return SetNamedPipeHandleState(handle, &mode, NULL, NULL) ? 0 : luna_file_error(GetLastError());
}
/* Status belongs to the open description, shared by duplicated descriptors. */
int luna_fd_get_status(int fd)
{
   AcquireSRWLockShared(&socket_lock);
   for (struct socket_fd *p = sockets; p; p = p->next) {
      if (p->fd != fd) continue;
      int flags = _O_RDWR | (p->state->nonblock ? LUNA_FILE_NONBLOCK : 0) |
                  (p->state->append ? _O_APPEND : 0);
      ReleaseSRWLockShared(&socket_lock);
      return flags;
   }
   HANDLE handle = (HANDLE)file_descriptor_handle(fd);
   int result = -1;
   if (handle == INVALID_HANDLE_VALUE) errno = EBADF;
   else {
      AcquireSRWLockShared(&file_description_lock);
      struct file_description_state *state = fd >= 0 && fd < 8192 &&
         file_descriptions[fd].handle == handle ? file_descriptions[fd].state : NULL;
      if (state && state->status_known) result = state->status;
      else errno = ENOTSUP; /* External CRT descriptors expose no append query. */
      ReleaseSRWLockShared(&file_description_lock);
      if (result >= 0 && GetFileType(handle) == FILE_TYPE_PIPE) {
         DWORD mode;
         if (!GetNamedPipeHandleStateW(handle, &mode, NULL, NULL, NULL, NULL, 0))
            result = luna_file_error(GetLastError());
         else result = (result & ~LUNA_FILE_NONBLOCK) |
                       (mode & PIPE_NOWAIT ? LUNA_FILE_NONBLOCK : 0);
      }
   }
   ReleaseSRWLockShared(&socket_lock);
   return result;
}
int luna_fd_set_status(int fd, int flags)
{
   AcquireSRWLockExclusive(&socket_lock);
   for (struct socket_fd *p = sockets; p; p = p->next) {
      if (p->fd != fd) continue;
      u_long mode = !!(flags & LUNA_FILE_NONBLOCK);
      int result = ioctlsocket(p->socket, FIONBIO, &mode);
      int error = result ? socket_errno(WSAGetLastError()) : 0;
      if (!result) { p->state->nonblock = mode; p->state->append = !!(flags & _O_APPEND); }
      ReleaseSRWLockExclusive(&socket_lock);
      if (error) { errno = error; return -1; }
      return 0;
   }
   HANDLE handle = (HANDLE)file_descriptor_handle(fd);
   if (handle == INVALID_HANDLE_VALUE) {
      ReleaseSRWLockExclusive(&socket_lock); errno = EBADF; return -1;
   }
   AcquireSRWLockExclusive(&file_description_lock);
   struct file_description_state *state = fd >= 0 && fd < 8192 &&
      file_descriptions[fd].handle == handle ? file_descriptions[fd].state : NULL;
   int result = 0;
   if (!state || !state->status_known) { errno = ENOTSUP; result = -1; }
   else if (GetFileType(handle) == FILE_TYPE_PIPE) {
      DWORD mode;
      if (!GetNamedPipeHandleStateW(handle, &mode, NULL, NULL, NULL, NULL, 0))
         result = luna_file_error(GetLastError());
      else {
         mode = flags & LUNA_FILE_NONBLOCK ? mode | PIPE_NOWAIT : mode & ~PIPE_NOWAIT;
         if (!SetNamedPipeHandleState(handle, &mode, NULL, NULL))
            result = luna_file_error(GetLastError());
      }
   } else if ((state->status ^ flags) & _O_APPEND)
      result = file_status_append_locked(fd, state, !!(flags & _O_APPEND));
   if (!result) state->status = (state->status & ~(_O_APPEND | LUNA_FILE_NONBLOCK)) |
                               (flags & (_O_APPEND | LUNA_FILE_NONBLOCK));
   ReleaseSRWLockExclusive(&file_description_lock);
   ReleaseSRWLockExclusive(&socket_lock);
   return result;
}
int luna_fd_control(int fd, int command, int argument)
{
   if (command == 3) return luna_fd_get_status(fd);
   if (command == 4) return luna_fd_set_status(fd, argument);
   intptr_t handle = file_descriptor_handle(fd);
   if (handle == -1) { errno = EBADF; return -1; }
   if (command == 1032) {
      DWORD incoming, outgoing;
      if (!GetNamedPipeInfo((HANDLE)handle, NULL, &outgoing, &incoming, NULL))
         return luna_file_error(GetLastError());
      DWORD capacity = incoming > outgoing ? incoming : outgoing;
      if (capacity > INT_MAX) { errno = EOVERFLOW; return -1; }
      return (int)capacity;
   }
   switch (command) {
   case 8: case 9: case 10: case 11: case 1024: case 1025: case 1026:
   case 1033: case 1034: errno = ENOTSUP; break;
   case 1031: errno = argument > 0 ? ENOTSUP : EINVAL; break;
   default: errno = EINVAL; break;
   }
   return -1;
}
int luna_fd_get_cloexec(int fd)
{
   if (fd < 0) { errno = EBADF; return -1; }
   intptr_t handle = file_descriptor_handle(fd);
   if (handle == -1) { errno = EBADF; return -1; }
   SOCKET socket = lookup(fd);
   if (socket != INVALID_SOCKET) handle = (intptr_t)socket;
   DWORD flags;
   if (!GetHandleInformation((HANDLE)handle, &flags)) return luna_file_error(GetLastError());
   return !(flags & HANDLE_FLAG_INHERIT);
}
int luna_fd_cloexec(int fd, int enabled)
{
   intptr_t native = file_descriptor_handle(fd);
   if (native == -1) { errno = EBADF; return -1; }
   DWORD flags = enabled ? 0 : HANDLE_FLAG_INHERIT;
   if (!SetHandleInformation((HANDLE)native, HANDLE_FLAG_INHERIT, flags))
      return luna_file_error(GetLastError());
   SOCKET socket = lookup(fd);
   if (socket != INVALID_SOCKET && !SetHandleInformation((HANDLE)socket, HANDLE_FLAG_INHERIT, flags))
      return luna_file_error(GetLastError());
   return 0;
}
int luna_socket_set_option(int fd, int level, int option, const void *value, socklen_t length)
{
   DWORD milliseconds;
   if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO) &&
       length == sizeof(struct timeval) && value) {
      const struct timeval *time = value;
      if (time->tv_sec < 0 || time->tv_usec < 0 || time->tv_usec >= 1000000) { errno = EINVAL; return -1; }
      uint64_t ms = (uint64_t)time->tv_sec * 1000 + (time->tv_usec + 999) / 1000;
      milliseconds = ms > MAXDWORD ? MAXDWORD : (DWORD)ms;
      value = &milliseconds; length = sizeof milliseconds;
   }
   SOCKET_CALL(setsockopt(socket,level,option,value,length));
}
int luna_socket_get_option(int fd, int level, int option, void *value, socklen_t *length)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   if (getsockopt(socket,level,option,value,length)) return failed();
   if (level == SOL_SOCKET && option == SO_ERROR && value && length && *length >= (socklen_t)sizeof(int))
      *(int *)value = socket_errno(*(int *)value);
   return 0;
}
int luna_socket_available(int fd, int *bytes)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   u_long count = 0;
   if (ioctlsocket(socket, FIONREAD, &count)) return failed();
   *bytes = count > INT_MAX ? INT_MAX : (int)count;
   return 0;
}
ptrdiff_t luna_socket_recv(int fd, void *buffer, size_t length, int flags)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   int n = length > INT_MAX ? INT_MAX : (int)length;
   int result = recv(socket,buffer,n,flags);
   return result == SOCKET_ERROR ? failed() : result;
}
ptrdiff_t luna_socket_send(int fd, const void *buffer, size_t length, int flags)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   int n = length > INT_MAX ? INT_MAX : (int)length;
   int result = send(socket,buffer,n,flags);
   return result == SOCKET_ERROR ? failed() : result;
}
ptrdiff_t luna_socket_recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   int n = length > INT_MAX ? INT_MAX : (int)length;
   int result = recvfrom(socket,buffer,n,flags,address,address_length);
   return result == SOCKET_ERROR ? failed() : result;
}
ptrdiff_t luna_socket_sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   int n = length > INT_MAX ? INT_MAX : (int)length;
   int result = sendto(socket,buffer,n,flags,address,address_length);
   return result == SOCKET_ERROR ? failed() : result;
}
static WSABUF *message_buffers(const struct msghdr *message)
{
   if (!message || message->msg_iovlen > IOV_MAX ||
       (message->msg_iovlen && !message->msg_iov)) { errno = EINVAL; return NULL; }
   WSABUF *buffers = calloc(message->msg_iovlen ? message->msg_iovlen : 1, sizeof *buffers);
   if (!buffers) { errno = ENOMEM; return NULL; }
   uint64_t total = 0;
   for (size_t i = 0; i < message->msg_iovlen; ++i) {
      size_t length = message->msg_iov[i].iov_len;
      if (length > ULONG_MAX || total + length > ULONG_MAX ||
          (length && !message->msg_iov[i].iov_base)) {
         free(buffers); errno = length && !message->msg_iov[i].iov_base ? EFAULT : EMSGSIZE; return NULL;
      }
      total += length;
      buffers[i].len = (ULONG)length;
      buffers[i].buf = message->msg_iov[i].iov_base;
   }
   return buffers;
}
static ptrdiff_t socket_message(int fd, struct msghdr *message, int flags, int sending)
{
   SOCKET socket = (SOCKET)luna_socket_native(fd);
   if (socket == INVALID_SOCKET) return -1;
   WSABUF *buffers = message_buffers(message);
   if (!buffers) return -1;
   if (message->msg_controllen > ULONG_MAX ||
       (message->msg_controllen && !message->msg_control)) {
      free(buffers); errno = EINVAL; return -1;
   }
   DWORD transferred = 0, count = message->msg_iovlen ? (DWORD)message->msg_iovlen : 1;
   DWORD capacity = 0;
   for (DWORD i = 0; i < count; ++i) capacity += buffers[i].len;
   DWORD native_flags = (DWORD)flags;
   int result, type, type_length = sizeof type;
   if (getsockopt(socket, SOL_SOCKET, SO_TYPE, (char *)&type, &type_length)) {
      int error = WSAGetLastError();
      free(buffers); errno = socket_errno(error); return -1;
   }
   if (message->msg_controllen && type != SOCK_STREAM) {
      WSAMSG native = {0};
      native.name = message->msg_name;
      native.namelen = message->msg_namelen;
      native.lpBuffers = buffers; native.dwBufferCount = count;
      native.Control.buf = message->msg_control;
      native.Control.len = (ULONG)message->msg_controllen;
      native.dwFlags = native_flags;
      DWORD size = 0;
      if (sending) {
         GUID id = WSAID_WSASENDMSG;
         LPFN_WSASENDMSG send_message = NULL;
         result = WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id,
                           &send_message, sizeof send_message, &size, NULL, NULL);
         if (!result) result = send_message(socket, &native, native_flags, &transferred, NULL, NULL);
      } else {
         GUID id = WSAID_WSARECVMSG;
         LPFN_WSARECVMSG receive_message = NULL;
         result = WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id,
                           &receive_message, sizeof receive_message, &size, NULL, NULL);
         if (!result) result = receive_message(socket, &native, &transferred, NULL, NULL);
         message->msg_namelen = native.namelen;
         message->msg_controllen = native.Control.len;
         native_flags = native.dwFlags;
      }
   } else if (sending) {
      if (message->msg_controllen) { free(buffers); errno = EOPNOTSUPP; return -1; }
      result = message->msg_name
         ? WSASendTo(socket, buffers, count, &transferred, native_flags,
                      message->msg_name, message->msg_namelen, NULL, NULL)
         : WSASend(socket, buffers, count, &transferred, native_flags, NULL, NULL);
   } else {
      result = message->msg_name
         ? WSARecvFrom(socket, buffers, count, &transferred, &native_flags,
                        message->msg_name, &message->msg_namelen, NULL, NULL)
         : WSARecv(socket, buffers, count, &transferred, &native_flags, NULL, NULL);
      message->msg_controllen = 0;
      if (type == SOCK_STREAM) message->msg_namelen = 0;
   }
   int error = result ? WSAGetLastError() : 0;
   free(buffers);
   if (!sending) {
      message->msg_flags = (int)(native_flags & (MSG_TRUNC | MSG_CTRUNC | MSG_OOB | MSG_BCAST | MSG_MCAST));
      if (error == WSAEMSGSIZE && (type == SOCK_DGRAM || type == SOCK_RAW)) {
         /* Winsock fills all receive buffers on datagram truncation, but its
          * byte-count output need not be set when it returns SOCKET_ERROR. */
         message->msg_flags |= MSG_TRUNC;
         return capacity;
      }
   }
   if (error) { errno = socket_errno(error); return -1; }
   return transferred;
}
ptrdiff_t luna_socket_sendmsg(int fd, const struct msghdr *message, int flags)
{
   if (!message) { errno = EFAULT; return -1; }
   struct msghdr copy = *message;
   return socket_message(fd, &copy, flags, 1);
}
ptrdiff_t luna_socket_recvmsg(int fd, struct msghdr *message, int flags)
{
   if (!message) { errno = EFAULT; return -1; }
   return socket_message(fd, message, flags, 0);
}
ptrdiff_t luna_fd_writev(int fd, const struct iovec *vectors, int count)
{
   if (count < 0 || count > IOV_MAX || (count && !vectors)) { errno = EINVAL; return -1; }
   if (lookup(fd) != INVALID_SOCKET) {
      struct msghdr message = {0};
      message.msg_iov = (struct iovec *)vectors; message.msg_iovlen = (size_t)count;
      return luna_socket_sendmsg(fd, &message, 0);
   }
   size_t total = 0;
   for (int i = 0; i < count; ++i) {
      if (vectors[i].iov_len && !vectors[i].iov_base) { errno = EFAULT; return -1; }
      if (vectors[i].iov_len > INT_MAX - total) { errno = EINVAL; return -1; }
      total += vectors[i].iov_len;
   }
   char *buffer = malloc(total ? total : 1);
   if (!buffer) { errno = ENOMEM; return -1; }
   size_t position = 0;
   for (int i = 0; i < count; ++i) {
      if (vectors[i].iov_len) memcpy(buffer + position, vectors[i].iov_base, vectors[i].iov_len);
      position += vectors[i].iov_len;
   }
   ptrdiff_t result = luna_fd_write(fd, buffer, total);
   int error = errno; free(buffer); errno = error; return result;
}

struct native_pipe_endpoint {
   HANDLE handle;
   int writing;
   struct native_pipe_endpoint *next;
};
struct native_pipe_pair {
   HANDLE reader, writer;
   DWORD capacity;
   struct native_pipe_endpoint *endpoints;
   struct native_pipe_pair *next;
};
static struct native_pipe_pair *native_pipes;
static SRWLOCK native_pipe_lock = SRWLOCK_INIT;
/* Query the receive endpoint: provider-reported write quota can describe
 * the other direction of a byte pipe. Return -1 once its reader closes. */
static int pipe_capacity(HANDLE writer, DWORD *capacity)
{
   int found = 0;
   AcquireSRWLockShared(&native_pipe_lock);
   for (struct native_pipe_pair *p = native_pipes; p; p = p->next) {
      struct native_pipe_endpoint *endpoint = p->endpoints;
      while (endpoint && (endpoint->handle != writer || !endpoint->writing)) endpoint = endpoint->next;
      if (!endpoint) continue;
      found = 1;
      DWORD used = 0;
      if (p->reader == INVALID_HANDLE_VALUE ||
          !PeekNamedPipe(p->reader, NULL, 0, NULL, &used, NULL)) found = -1;
      else *capacity = used < p->capacity ? p->capacity - used : 0;
      break;
   }
   ReleaseSRWLockShared(&native_pipe_lock);
   return found;
}
static void pipe_forget(HANDLE handle)
{
   AcquireSRWLockExclusive(&native_pipe_lock);
   struct native_pipe_pair **link = &native_pipes;
   while (*link) {
      struct native_pipe_pair *p = *link;
      struct native_pipe_endpoint **at = &p->endpoints;
      while (*at && (*at)->handle != handle) at = &(*at)->next;
      if (*at) {
         struct native_pipe_endpoint *endpoint = *at;
         *at = endpoint->next; free(endpoint);
         p->reader = p->writer = INVALID_HANDLE_VALUE;
         for (endpoint = p->endpoints; endpoint; endpoint = endpoint->next) {
            if (endpoint->writing) p->writer = endpoint->handle;
            else p->reader = endpoint->handle;
         }
      }
      if (!p->endpoints) { *link = p->next; free(p); }
      else link = &p->next;
   }
   ReleaseSRWLockExclusive(&native_pipe_lock);
}
static void pipe_register_duplicate(HANDLE source, HANDLE target, struct native_pipe_endpoint *copy)
{
   AcquireSRWLockExclusive(&native_pipe_lock);
   for (struct native_pipe_pair *p = native_pipes; p; p = p->next) {
      for (struct native_pipe_endpoint *at = p->endpoints; at; at = at->next) {
         if (at->handle != source) continue;
         copy->handle = target; copy->writing = at->writing;
         copy->next = p->endpoints; p->endpoints = copy;
         ReleaseSRWLockExclusive(&native_pipe_lock);
         return;
      }
   }
   ReleaseSRWLockExclusive(&native_pipe_lock);
   free(copy);
}
/* The CRT duplicates file handles, while Winsock needs its own duplicate.
 * Reserve low CRT slots temporarily to implement F_DUPFD's lower bound. */
static int fd_duplicate(int fd, int target, int minimum, int close_on_exec)
{
   if (fd < 0 || target < -1 || target >= 8192) { errno = EBADF; return -1; }
   if (minimum < 0 || minimum >= 8192) { errno = EINVAL; return -1; }
   AcquireSRWLockExclusive(&socket_lock);
   intptr_t source_handle = file_descriptor_handle(fd);
   if (source_handle == -1) {
      ReleaseSRWLockExclusive(&socket_lock); errno = EBADF; return -1;
   }
   if (target == fd) { ReleaseSRWLockExclusive(&socket_lock); return fd; }
   struct socket_fd *source = sockets;
   while (source && source->fd != fd) source = source->next;
   struct socket_fd *copy = NULL;
   struct native_pipe_endpoint *pipe_copy = NULL;
   SOCKET duplicated = INVALID_SOCKET;
   struct file_description_state *file_copy = NULL;
   int result = -1, error = 0;
   if (source) {
      copy = calloc(1, sizeof *copy);
      if (!copy) { error = ENOMEM; goto done; }
      WSAPROTOCOL_INFOW protocol;
      if (WSADuplicateSocketW(source->socket, GetCurrentProcessId(), &protocol)) {
         error = socket_errno(WSAGetLastError()); goto done;
      }
      duplicated = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
         &protocol, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
      if (duplicated == INVALID_SOCKET) { error = socket_errno(WSAGetLastError()); goto done; }
   } else if (GetFileType((HANDLE)source_handle) == FILE_TYPE_PIPE) {
      pipe_copy = calloc(1, sizeof *pipe_copy);
      if (!pipe_copy) { error = ENOMEM; goto done; }
   }
   if (!source && (GetFileType((HANDLE)source_handle) == FILE_TYPE_DISK ||
                   GetFileType((HANDLE)source_handle) == FILE_TYPE_PIPE)) {
      file_copy = file_description_acquire(fd, (HANDLE)source_handle);
      if (!file_copy) { error = errno; goto done; }
   }
   intptr_t replaced_handle = target < 0 ? -1 : file_descriptor_handle(target);
   if (target >= 0) {
      if (_dup2(fd, target)) { error = errno; goto done; }
      result = target;
      struct socket_fd **at = &sockets;
      while (*at && (*at)->fd != target) at = &(*at)->next;
      if (*at) {
         struct socket_fd *old = *at; *at = old->next;
         closesocket(old->socket);
         if (!--old->state->references) free(old->state);
         free(old);
      }
   } else {
      int reserved[8192]; size_t count = 0;
      while ((result = _dup(fd)) >= 0 && result < minimum) reserved[count++] = result;
      if (result < 0) error = errno;
      while (count) _close(reserved[--count]);
      if (result < 0) goto done;
      if (result >= 8192) { _close(result); result = -1; error = EMFILE; goto done; }
   }
   if (pipe_copy) {
      pipe_register_duplicate((HANDLE)source_handle, (HANDLE)file_descriptor_handle(result), pipe_copy);
      pipe_copy = NULL;
   }
   if (replaced_handle != -1) pipe_forget((HANDLE)replaced_handle);
   file_description_duplicate(result, (HANDLE)file_descriptor_handle(result), file_copy);
   DWORD inherit = close_on_exec ? 0 : HANDLE_FLAG_INHERIT;
   if (!SetHandleInformation((HANDLE)file_descriptor_handle(result), HANDLE_FLAG_INHERIT, inherit) ||
       (duplicated != INVALID_SOCKET &&
        !SetHandleInformation((HANDLE)duplicated, HANDLE_FLAG_INHERIT, inherit))) {
      error = EIO;
      file_description_forget(result, (HANDLE)file_descriptor_handle(result));
      pipe_forget((HANDLE)file_descriptor_handle(result)); _close(result); result = -1;
      goto done;
   }
   if (copy) {
      copy->fd = result; copy->socket = duplicated; copy->state = source->state;
      ++copy->state->references;
      copy->next = sockets; sockets = copy;
      copy = NULL; duplicated = INVALID_SOCKET;
   }
done:
   file_description_release(file_copy);
   if (duplicated != INVALID_SOCKET) closesocket(duplicated);
   free(copy); free(pipe_copy);
   ReleaseSRWLockExclusive(&socket_lock);
   if (error) errno = error;
   return result;
}
int luna_fd_dup(int fd, int minimum, int close_on_exec)
{ return fd_duplicate(fd, -1, minimum, close_on_exec); }
int luna_fd_dup_to(int fd, int target, int close_on_exec)
{
   if (target < 0) { errno = EBADF; return -1; }
   return fd_duplicate(fd, target, 0, close_on_exec);
}

static short pipe_poll(HANDLE handle, short events);
static ptrdiff_t pipe_io(HANDLE handle, void *buffer, DWORD length, int writing)
{
   if (writing && length) {
      DWORD mode = 0;
      if (GetNamedPipeHandleStateW(handle, &mode, NULL, NULL, NULL, NULL, 0) && (mode & PIPE_NOWAIT)) {
         typedef NTSTATUS (NTAPI *query_file_fn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                                FILE_INFORMATION_CLASS);
         query_file_fn query = (query_file_fn)(void *)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
         FILE_PIPE_LOCAL_INFORMATION info;
         IO_STATUS_BLOCK status;
         if (!query || query(handle, &status, &info, sizeof info, FilePipeLocalInformation) < 0) {
            errno = EIO; return -1;
         }
         int paired = pipe_capacity(handle, &info.WriteQuotaAvailable);
         if (paired < 0 || info.NamedPipeState != 3) { errno = EPIPE; return -1; }
         if (!info.WriteQuotaAvailable) { errno = EAGAIN; return -1; }
         if (length > info.WriteQuotaAvailable) length = info.WriteQuotaAvailable;
      }
   }
   DWORD bytes = 0;
   BOOL ok = writing ? WriteFile(handle, buffer, length, &bytes, NULL)
                     : ReadFile(handle, buffer, length, &bytes, NULL);
   if (ok && (!writing || bytes || !length)) return bytes;
   DWORD error = ok ? ERROR_NO_DATA : GetLastError();
   if (!writing && error == ERROR_BROKEN_PIPE) return 0;
   if (error == ERROR_NO_DATA) errno = writing && (pipe_poll(handle, 0) & POLLERR) ? EPIPE : EAGAIN;
   else if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) errno = EPIPE;
   else if (error == ERROR_ACCESS_DENIED) errno = EBADF;
   else return luna_file_error(error);
   return -1;
}
static short pipe_poll(HANDLE handle, short events)
{
   typedef NTSTATUS (NTAPI *query_file_fn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                          FILE_INFORMATION_CLASS);
   query_file_fn query = (query_file_fn)(void *)GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
   FILE_PIPE_LOCAL_INFORMATION info;
   FILE_ACCESS_INFORMATION access;
   IO_STATUS_BLOCK status;
   if (!query || query(handle, &status, &info, sizeof info, FilePipeLocalInformation) < 0 ||
       query(handle, &status, &access, sizeof access, FileAccessInformation) < 0) return POLLERR;
   short ready = 0;
   if (access.AccessFlags & FILE_READ_DATA) {
      if (info.ReadDataAvailable) ready |= events & POLLIN;
      if (info.NamedPipeState != 3) ready |= POLLHUP;
   }
   if (access.AccessFlags & FILE_WRITE_DATA) {
      int paired = pipe_capacity(handle, &info.WriteQuotaAvailable);
      if (paired < 0 || info.NamedPipeState != 3) ready |= POLLERR;
      else if (info.WriteQuotaAvailable) ready |= events & POLLOUT;
   }
   return ready;
}
ptrdiff_t luna_fd_read(int fd, void *buffer, size_t length)
{
   SOCKET socket = lookup(fd);
   unsigned int n = length > INT_MAX ? INT_MAX : (unsigned int)length;
   if (socket == INVALID_SOCKET) {
      intptr_t handle = file_descriptor_handle(fd);
      if (handle == -1) { errno = EBADF; return -1; }
      if (GetFileType((HANDLE)handle) == FILE_TYPE_PIPE) return pipe_io((HANDLE)handle, buffer, n, 0);
      return _read(fd, buffer, n);
   }
   int result = recv(socket, buffer, (int)n, 0);
   return result == SOCKET_ERROR ? failed() : result;
}
ptrdiff_t luna_fd_write(int fd, const void *buffer, size_t length)
{
   SOCKET socket = lookup(fd);
   unsigned int n = length > INT_MAX ? INT_MAX : (unsigned int)length;
   if (socket == INVALID_SOCKET) {
      intptr_t handle = file_descriptor_handle(fd);
      if (handle == -1) { errno = EBADF; return -1; }
      if (GetFileType((HANDLE)handle) == FILE_TYPE_PIPE) return pipe_io((HANDLE)handle, (void *)buffer, n, 1);
      return _write(fd, buffer, n);
   }
   int result = send(socket, buffer, (int)n, 0);
   return result == SOCKET_ERROR ? failed() : result;
}
int luna_fd_close(int fd)
{
   AcquireSRWLockExclusive(&socket_lock);
   struct socket_fd **link = &sockets;
   while (*link && (*link)->fd != fd) link = &(*link)->next;
   struct socket_fd *record = *link;
   if (!record) {
      intptr_t handle = file_descriptor_handle(fd);
      if (handle == -1) { ReleaseSRWLockExclusive(&socket_lock); errno = EBADF; return -1; }
      file_description_forget(fd, (HANDLE)handle);
      pipe_forget((HANDLE)handle);
      int result = _close(fd);
      ReleaseSRWLockExclusive(&socket_lock);
      return result;
   }
   if (closesocket(record->socket)) {
      int error = WSAGetLastError();
      ReleaseSRWLockExclusive(&socket_lock);
      errno = socket_errno(error);
      return -1;
   }
   *link = record->next;
   if (!--record->state->references) free(record->state);
   int closed = _close(fd);
   ReleaseSRWLockExclusive(&socket_lock);
   free(record);
   return closed;
}
int luna_socket_dns(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **out)
{
   if (luna_socket_startup()) return EAI_FAIL;
   return getaddrinfo(node, service, hints, out);
}
int luna_socket_poll(struct pollfd *fds, size_t count, int timeout_ms)
{
   if (count > ULONG_MAX || count > SIZE_MAX / sizeof(WSAPOLLFD)) { errno = EINVAL; return -1; }
   if (count && !fds) { errno = EFAULT; return -1; }
   WSAPOLLFD *native = calloc(count ? count : 1, sizeof *native);
   size_t *indices = malloc((count ? count : 1) * sizeof *indices);
   if (!native || !indices) { free(native); free(indices); errno = ENOMEM; return -1; }
   ULONGLONG started = GetTickCount64();
   int has_pipes, ready;
   ULONG used;
rescan:
   has_pipes = ready = 0;
   used = 0;
   for (size_t i = 0; i < count; ++i) {
      fds[i].revents = 0;
      if ((intptr_t)fds[i].fd < 0) continue;
      SOCKET socket = lookup((int)fds[i].fd);
      if (socket != INVALID_SOCKET) {
         indices[used] = i;
         native[used].fd = socket;
         native[used].events = fds[i].events;
         ++used;
         continue;
      }
      intptr_t handle = file_descriptor_handle((int)fds[i].fd);
      if (handle == -1) fds[i].revents = POLLNVAL;
      else {
         DWORD kind = GetFileType((HANDLE)handle);
         if (kind == FILE_TYPE_PIPE) {
            has_pipes = 1;
            fds[i].revents = pipe_poll((HANDLE)handle, fds[i].events);
         } else fds[i].revents = kind == FILE_TYPE_DISK || kind == FILE_TYPE_CHAR
            ? fds[i].events & (POLLIN | POLLOUT) : POLLERR;
      }
      if (fds[i].revents) ++ready;
   }
   /* WSAPoll requires at least one socket; ignored negative entries alone
    * must behave like poll([], timeout), not return WSAEINVAL. */
   int result = used ? WSAPoll(native, used, ready || has_pipes ? 0 : timeout_ms) : 0;
   int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
   if (!used && !ready && !has_pipes && timeout_ms) Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
   if (result != SOCKET_ERROR) {
      for (ULONG i = 0; i < used; ++i) {
         fds[indices[i]].revents = native[i].revents;
         if (native[i].revents) ++ready;
      }
   }
   if (!error && !ready && has_pipes && timeout_ms) {
      ULONGLONG elapsed = GetTickCount64() - started;
      if (timeout_ms < 0 || elapsed < (ULONGLONG)timeout_ms) {
         DWORD delay = timeout_ms < 0 || (ULONGLONG)timeout_ms - elapsed > 2
            ? 2 : (DWORD)((ULONGLONG)timeout_ms - elapsed);
         Sleep(delay);
         goto rescan;
      }
   }
   free(native); free(indices);
   if (error) { errno = socket_errno(error); return -1; }
   return ready;
}

/* IANA identifiers map to Windows' own, year-specific timezone rules. */
#include "unicode/windows_zones.h"
static int luna_zone(const char *zone, DYNAMIC_TIME_ZONE_INFORMATION *tz)
{
   if (!zone || !*zone)
      return GetDynamicTimeZoneInformation(tz) == TIME_ZONE_ID_INVALID ? -1 : 0;
   if ((!strncmp(zone, "GMT", 3) || !strncmp(zone, "UTC", 3)) && (zone[3] == '+' || zone[3] == '-')) {
      const char *p = zone + 4;
      int hour = 0, minute = 0, count = 0;
      while (*p >= '0' && *p <= '9' && count < 4) {
         hour = hour * 10 + *p++ - '0';
         count++;
      }
      if (*p == ':') {
         if (count < 1 || count > 2 || strlen(p) != 3 || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9')
            return -1;
         minute = (p[1] - '0') * 10 + p[2] - '0';
         p += 3;
      } else if (count > 2) {
         minute = hour % 100;
         hour /= 100;
      }
      if (*p || !count || hour > 23 || minute > 59)
         return -1;
      memset(tz, 0, sizeof *tz);
      tz->Bias = (zone[3] == '+' ? -1 : 1) * (hour * 60 + minute);
      MultiByteToWideChar(CP_UTF8, 0, zone, -1, tz->StandardName, 32);
      return 0;
   }
   const wchar_t *key = NULL;
   for (size_t i = 0; i < sizeof luna_windows_zones / sizeof luna_windows_zones[0]; i++)
      if (!strcmp(zone, luna_windows_zones[i].iana)) {
         key = luna_windows_zones[i].windows;
         break;
      }
   wchar_t native[128];
   if (!key) {
      if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, zone, -1, native, 128))
         return -1;
      key = native;
   }
   for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, tz) == ERROR_SUCCESS; i++)
      if (!wcscmp(tz->TimeZoneKeyName, key))
         return 0;
   if (!wcscmp(key, L"UTC")) {
      memset(tz, 0, sizeof *tz);
      wcscpy(tz->StandardName, L"UTC");
      return 0;
   }
   return -1;
}
static BOOL luna_zone_year(WORD year, DYNAMIC_TIME_ZONE_INFORMATION *zone, TIME_ZONE_INFORMATION *out)
{
   if (zone->TimeZoneKeyName[0])
      return GetTimeZoneInformationForYear(year, zone, out);
   out->Bias = zone->Bias;
   out->StandardBias = zone->StandardBias;
   out->DaylightBias = zone->DaylightBias;
   out->StandardDate = zone->StandardDate;
   out->DaylightDate = zone->DaylightDate;
   memcpy(out->StandardName, zone->StandardName, sizeof out->StandardName);
   memcpy(out->DaylightName, zone->DaylightName, sizeof out->DaylightName);
   return TRUE;
}
static int luna_ms_system(int64_t ms, SYSTEMTIME *st)
{
   if (ms < -INT64_C(11644473600000) || ms > INT64_MAX / 10000 - INT64_C(11644473600000))
      return -1;
   ULARGE_INTEGER ticks;
   ticks.QuadPart = (ULONGLONG)(ms + INT64_C(11644473600000)) * 10000;
   FILETIME ft = {ticks.LowPart, ticks.HighPart};
   return FileTimeToSystemTime(&ft, st) ? 0 : -1;
}
static int64_t luna_system_ms(const SYSTEMTIME *st)
{
   FILETIME ft;
   if (!SystemTimeToFileTime(st, &ft))
      return INT64_MIN;
   ULARGE_INTEGER ticks;
   ticks.LowPart = ft.dwLowDateTime;
   ticks.HighPart = ft.dwHighDateTime;
   return (int64_t)(ticks.QuadPart / 10000) - INT64_C(11644473600000);
}
static void luna_zone_name(const wchar_t *source, char *out, int32_t capacity, int offset)
{
   if (capacity <= 0)
      return;
   if (!*source || !WideCharToMultiByte(CP_UTF8, 0, source, -1, out, capacity, NULL, NULL))
      snprintf(out, (size_t)capacity, "GMT%+d:%02d", offset / 3600, abs(offset / 60) % 60);
}
int luna_calendar_break(int64_t ms, const char *zone, struct tm *tm,
                        int32_t *offset, char *name, int32_t name_size)
{
   DYNAMIC_TIME_ZONE_INFORMATION tz;
   SYSTEMTIME utc, local;
   TIME_ZONE_INFORMATION year;
   if (luna_zone(zone, &tz) || luna_ms_system(ms, &utc) ||
       !luna_zone_year(utc.wYear, &tz, &year) ||
       !SystemTimeToTzSpecificLocalTime(&year, &utc, &local))
      return -1;
   int64_t wall = luna_system_ms(&local);
   if (wall == INT64_MIN)
      return -1;
   *offset = (int32_t)((wall - ms) / 1000); /* subsecond millis are preserved by Win32 */
   memset(tm, 0, sizeof *tm);
   tm->tm_year = local.wYear - 1900;
   tm->tm_mon = local.wMonth - 1;
   tm->tm_mday = local.wDay;
   tm->tm_hour = local.wHour;
   tm->tm_min = local.wMinute;
   tm->tm_sec = local.wSecond;
   tm->tm_wday = local.wDayOfWeek;
   SYSTEMTIME jan = local;
   jan.wMonth = 1;
   jan.wDay = 1;
   jan.wHour = jan.wMinute = jan.wSecond = jan.wMilliseconds = 0;
   tm->tm_yday = (int)((wall - luna_system_ms(&jan)) / 86400000);
   tm->tm_isdst = year.DaylightDate.wMonth && *offset != -(year.Bias + year.StandardBias) * 60;
   luna_zone_name(tm->tm_isdst ? year.DaylightName : year.StandardName, name, name_size, *offset);
   return 0;
}
int luna_calendar_timezone(const char *zone, int64_t ms, int32_t *west,
                           int *daylight, char names[2][32])
{
   DYNAMIC_TIME_ZONE_INFORMATION tz;
   SYSTEMTIME st;
   TIME_ZONE_INFORMATION year;
   if (luna_zone(zone, &tz) || luna_ms_system(ms, &st) || !luna_zone_year(st.wYear, &tz, &year))
      return -1;
   *west = (year.Bias + year.StandardBias) * 60;
   *daylight = year.DaylightDate.wMonth != 0;
   luna_zone_name(year.StandardName, names[0], 32, -*west);
   luna_zone_name(year.DaylightName, names[1], 32, -(year.Bias + year.DaylightBias) * 60);
   return 0;
}
/* Proleptic Gregorian day count also normalizes month/day overflow. */
static int64_t luna_civil_days(int64_t y, int64_t m, int64_t d)
{
   y += (m - 1) / 12;
   m = (m - 1) % 12 + 1;
   if (m <= 0) {
      m += 12;
      y--;
   }
   y -= m <= 2;
   int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
   int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
   return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
int luna_calendar_assemble(const struct tm *tm, const char *zone, int64_t sub_ms, int64_t *result)
{
   int64_t days = luna_civil_days((int64_t)tm->tm_year + 1900, (int64_t)tm->tm_mon + 1, tm->tm_mday);
   int64_t wall = ((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60000 + (int64_t)tm->tm_sec * 1000;
   DYNAMIC_TIME_ZONE_INFORMATION tz;
   TIME_ZONE_INFORMATION year;
   SYSTEMTIME local, utc;
   if (luna_zone(zone, &tz) || luna_ms_system(wall, &local) ||
       !luna_zone_year(local.wYear, &tz, &year) ||
       !TzSpecificLocalTimeToSystemTime(&year, &local, &utc))
      return -1;
   int64_t ms = luna_system_ms(&utc);
   if (ms == INT64_MIN)
      return -1;
   *result = ms + sub_ms;
   return 0;
}

/* MSVCRT has no rand48 family. Keep the POSIX sequence and seed width. */
static uint64_t rand48_state = UINT64_C(0x1234abcd330e);
static SRWLOCK rand48_lock = SRWLOCK_INIT;
uint32_t luna_os_lrand48(void)
{
   AcquireSRWLockExclusive(&rand48_lock);
   rand48_state = (rand48_state * UINT64_C(0x5deece66d) + 11) & UINT64_C(0xffffffffffff);
   uint32_t value = (uint32_t)(rand48_state >> 17);
   ReleaseSRWLockExclusive(&rand48_lock);
   return value;
}
void luna_os_srand48(int32_t seed)
{
   AcquireSRWLockExclusive(&rand48_lock);
   rand48_state = ((uint64_t)(uint32_t)seed << 16) | 0x330e;
   ReleaseSRWLockExclusive(&rand48_lock);
}

/* CreateNamedPipe's open mode does not accept FILE_WRITE_ATTRIBUTES.
 * Request it through the native API so a read endpoint can later change
 * its blocking mode without acquiring permission to write pipe data. */
static HANDLE pipe_native_reader(const wchar_t *dos_name, int nonblocking, int inherit)
{
   wchar_t name[128];
   _snwprintf(name, 128, L"\\??\\pipe\\%ls", dos_name + 9);
   USHORT length = (USHORT)(wcslen(name) * sizeof *name);
   UNICODE_STRING text = { length, (USHORT)(length + sizeof *name), name };
   OBJECT_ATTRIBUTES object = { 0 };
   object.Length = sizeof object;
   object.ObjectName = &text;
   object.Attributes = OBJ_CASE_INSENSITIVE | (inherit ? OBJ_INHERIT : 0);
   typedef NTSTATUS (NTAPI *pipe_create_fn)(PHANDLE, ULONG, POBJECT_ATTRIBUTES,
      PIO_STATUS_BLOCK, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG,
      ULONG, ULONG, PLARGE_INTEGER);
   pipe_create_fn create = (pipe_create_fn)(void *)GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "NtCreateNamedPipeFile");
   if (!create) { SetLastError(ERROR_CALL_NOT_IMPLEMENTED); return INVALID_HANDLE_VALUE; }
   IO_STATUS_BLOCK result;
   LARGE_INTEGER timeout; timeout.QuadPart = -500000000LL;
   HANDLE handle = INVALID_HANDLE_VALUE;
   NTSTATUS status = create(&handle, GENERIC_READ | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
      &object, &result, FILE_SHARE_WRITE, FILE_CREATE, FILE_SYNCHRONOUS_IO_NONALERT,
      2 /* byte stream, reject remote clients */, 0 /* byte reads */, !!nonblocking, 1, 65536, 65536, &timeout);
   if (status < 0) { SetLastError(RtlNtStatusToDosError(status)); return INVALID_HANDLE_VALUE; }
   return handle;
}
/* Named byte pipes support PIPE_NOWAIT, unlike CreatePipe's anonymous pipes. */
int luna_os_pipe_open(int fds[2], int nonblocking, int close_on_exec)
{
   if (!fds) { errno = EFAULT; return -1; }
   wchar_t name[128];
   unsigned char random[16];
   if (!RtlGenRandom(random, sizeof random)) { errno = EIO; return -1; }
   wchar_t hex[33];
   for (int i = 0; i < 16; ++i) {
      hex[2*i] = L"0123456789abcdef"[random[i] >> 4];
      hex[2*i+1] = L"0123456789abcdef"[random[i] & 15];
   }
   hex[32] = 0;
   _snwprintf(name, 128, L"\\\\.\\pipe\\lunaria-%lu-%ls", GetCurrentProcessId(), hex);
   SECURITY_ATTRIBUTES security = { sizeof security, NULL, !close_on_exec };
   HANDLE reader = pipe_native_reader(name, nonblocking, !close_on_exec);
   if (reader == INVALID_HANDLE_VALUE) return luna_file_error(GetLastError());
   HANDLE writer = CreateFileW(name, GENERIC_WRITE | FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, 0, &security,
                               OPEN_EXISTING, 0, NULL);
   DWORD error = writer == INVALID_HANDLE_VALUE ? GetLastError() : 0;
   if (!error && !ConnectNamedPipe(reader, NULL)) {
      error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) error = 0;
   }
   DWORD mode = nonblocking ? PIPE_NOWAIT : PIPE_WAIT;
   if (!error && !SetNamedPipeHandleState(writer, &mode, NULL, NULL)) error = GetLastError();
   if (error) {
      CloseHandle(reader);
      if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
      return luna_file_error(error);
   }
   int flags = _O_BINARY | (close_on_exec ? _O_NOINHERIT : 0);
   int read_fd = _open_osfhandle((intptr_t)reader, flags | _O_RDONLY);
   if (read_fd < 0) { int e = errno; CloseHandle(reader); CloseHandle(writer); errno = e; return -1; }
   int write_fd = _open_osfhandle((intptr_t)writer, flags | _O_WRONLY);
   if (write_fd < 0) { int e = errno; _close(read_fd); CloseHandle(writer); errno = e; return -1; }
   struct native_pipe_pair *pair = malloc(sizeof *pair);
   if (!pair) { _close(read_fd); _close(write_fd); errno = ENOMEM; return -1; }
   DWORD capacity = 0;
   if (!GetNamedPipeInfo(reader, NULL, NULL, &capacity, NULL)) {
      DWORD e = GetLastError(); free(pair); _close(read_fd); _close(write_fd); return luna_file_error(e);
   }
   struct native_pipe_endpoint *read_end = calloc(1, sizeof *read_end);
   struct native_pipe_endpoint *write_end = calloc(1, sizeof *write_end);
   if (!read_end || !write_end) {
      free(read_end); free(write_end); free(pair); _close(read_fd); _close(write_fd);
      errno = ENOMEM; return -1;
   }
   if (file_status_register(read_fd, _O_RDONLY | (nonblocking ? LUNA_FILE_NONBLOCK : 0)) ||
       file_status_register(write_fd, _O_WRONLY | (nonblocking ? LUNA_FILE_NONBLOCK : 0))) {
      int saved = errno;
      free(read_end); free(write_end); free(pair);
      luna_fd_close(read_fd); luna_fd_close(write_fd); errno = saved; return -1;
   }
   read_end->handle = reader; read_end->next = write_end;
   write_end->handle = writer; write_end->writing = 1;
   pair->endpoints = read_end;
   pair->reader = reader; pair->writer = writer; pair->capacity = capacity;
   AcquireSRWLockExclusive(&native_pipe_lock);
   pair->next = native_pipes; native_pipes = pair;
   ReleaseSRWLockExclusive(&native_pipe_lock);
   fds[0] = read_fd; fds[1] = write_fd;
   return 0;
}

struct native_locale { _locale_t object; const char *name; };
luna_os_locale luna_os_locale_new(void)
{
   struct native_locale *locale = malloc(sizeof *locale);
   if (!locale) { errno = ENOMEM; return NULL; }
   locale->name = ".UTF8";
   locale->object = _create_locale(LC_CTYPE, locale->name);
   if (!locale->object) {
      locale->name = "C";
      locale->object = _create_locale(LC_CTYPE, locale->name);
   }
   if (!locale->object) { free(locale); return NULL; }
   return locale;
}
luna_os_locale luna_os_locale_clone(luna_os_locale value)
{
   if (!value) { errno = EINVAL; return NULL; }
   const struct native_locale *source = value;
   struct native_locale *locale = malloc(sizeof *locale);
   if (!locale) { errno = ENOMEM; return NULL; }
   locale->name = source->name;
   locale->object = _create_locale(LC_CTYPE, locale->name);
   if (!locale->object) { free(locale); return NULL; }
   return locale;
}
void luna_os_locale_free(luna_os_locale value)
{
   struct native_locale *locale = value;
   if (locale) { _free_locale(locale->object); free(locale); }
}
int luna_os_locale_use(luna_os_locale value)
{
   if (!value) {
      if (_configthreadlocale(_DISABLE_PER_THREAD_LOCALE) != -1) return 0;
      errno = EINVAL; return -1;
   }
   const struct native_locale *locale = value;
   if (_configthreadlocale(_ENABLE_PER_THREAD_LOCALE) == -1 ||
       !setlocale(LC_ALL, "C") || !setlocale(LC_CTYPE, locale->name)) {
      errno = EINVAL; return -1;
   }
   return 0;
}

int luna_os_time_break(int64_t seconds, int local, struct tm *result, int64_t *offset)
{
   if (seconds > INT64_MAX / 1000 || seconds < INT64_MIN / 1000) { errno = EOVERFLOW; return -1; }
   const char *zone = local ? getenv("TZ") : "UTC";
   if (zone && !*zone) zone = "UTC";
   int32_t zone_offset;
   char name[128];
   if (luna_calendar_break(seconds * 1000, zone, result, &zone_offset, name, sizeof name)) {
      errno = EOVERFLOW; return -1;
   }
   *offset = zone_offset;
   return 0;
}
int luna_os_time_make(struct tm *value, int64_t *seconds, int64_t *offset)
{
   const char *zone = getenv("TZ");
   if (zone && !*zone) zone = "UTC";
   int64_t ms;
   if (luna_calendar_assemble(value, zone, 0, &ms)) { errno = EOVERFLOW; return -1; }
   *seconds = ms / 1000;
   return luna_os_time_break(*seconds, 1, value, offset);
}

int luna_os_unsetenv(const char *name)
{
   if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
   int error = _putenv_s(name, "");
   if (error) { errno = error; return -1; }
   return 0;
}
intptr_t luna_os_command_pipe_open(const char *path)
{
   wchar_t *wide = luna_file_wide(path);
   if (!wide) return -1;
   HANDLE pipe = CreateNamedPipeW(wide, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 0, 4096, 0, NULL);
   DWORD error = pipe == INVALID_HANDLE_VALUE ? GetLastError() : 0;
   free(wide);
   if (error) { luna_file_error(error); return -1; }
   return (intptr_t)pipe;
}
ptrdiff_t luna_os_command_pipe_read(intptr_t value, void *buffer, size_t capacity)
{
   HANDLE pipe = (HANDLE)value;
   if (!ConnectNamedPipe(pipe, NULL)) {
      DWORD error = GetLastError();
      if (error == ERROR_PIPE_LISTENING) { errno = EAGAIN; return -1; }
      if (error != ERROR_PIPE_CONNECTED) {
         if (error == ERROR_NO_DATA) { DisconnectNamedPipe(pipe); return 0; }
         return luna_file_error(error);
      }
   }
   DWORD bytes;
   DWORD length = capacity > MAXDWORD ? MAXDWORD : (DWORD)capacity;
   if (ReadFile(pipe, buffer, length, &bytes, NULL)) return bytes;
   DWORD error = GetLastError();
   if (error == ERROR_BROKEN_PIPE) { DisconnectNamedPipe(pipe); return 0; }
   if (error == ERROR_NO_DATA) { errno = EAGAIN; return -1; }
   return luna_file_error(error);
}
void luna_os_command_pipe_close(intptr_t pipe) { CloseHandle((HANDLE)pipe); }

void *luna_os_library_loaded_symbol(const char *module, const char *name)
{
   wchar_t *wide = luna_file_wide(module);
   if (!wide) return NULL;
   HMODULE handle = GetModuleHandleW(wide);
   free(wide);
   return handle ? (void *)GetProcAddress(handle, name) : NULL;
}
