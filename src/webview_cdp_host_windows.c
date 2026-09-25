#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "webview_cdp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t *utf8_to_wide(const char *s)
{
   int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
   if (!n) return NULL;
   wchar_t *wide = malloc((size_t)n * sizeof *wide);
   if (!wide || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     s, -1, wide, n)) {
      free(wide);
      return NULL;
   }
   return wide;
}

static ptrdiff_t host_read(void *context, void *dst, size_t size)
{
   struct lunaria_cdp_host *host = context;
   DWORD got = 0;
   if (host->nonblocking) {
      DWORD available = 0;
      if (!PeekNamedPipe((HANDLE)host->input_handle, NULL, 0, NULL,
                         &available, NULL)) return -1;
      if (!available) return -2;
      if (size > available) size = available;
   }
   if (size > 0x7fffffffU) size = 0x7fffffffU;
   return ReadFile((HANDLE)host->input_handle, dst, (DWORD)size, &got, NULL)
      ? (ptrdiff_t)got : -1;
}

int lunaria_cdp_host_set_nonblocking(struct lunaria_cdp_host *host)
{
   if (!host || !host->input_handle) return -1;
   host->nonblocking = 1;
   return 0;
}

static ptrdiff_t host_write(void *context, const void *src, size_t size)
{
   struct lunaria_cdp_host *host = context;
   DWORD sent = 0;
   if (size > 0x7fffffffU) size = 0x7fffffffU;
   return WriteFile((HANDLE)host->output_handle, src, (DWORD)size,
                    &sent, NULL) ? (ptrdiff_t)sent : -1;
}

int lunaria_cdp_host_open(struct lunaria_cdp_host *host,
                          const char *browser_binary,
                          const char *profile_dir)
{
   if (!host || !browser_binary || !*browser_binary ||
       !profile_dir || !*profile_dir) return -1;
   memset(host, 0, sizeof *host);
   wchar_t *browser = utf8_to_wide(browser_binary);
   wchar_t *profile = utf8_to_wide(profile_dir);
   if (!browser || !profile) goto fail;
   if (!CreateDirectoryW(profile, NULL) &&
       GetLastError() != ERROR_ALREADY_EXISTS) goto fail;

   SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
   HANDLE child_read = NULL, parent_write = NULL;
   HANDLE parent_read = NULL, child_write = NULL;
   if (!CreatePipe(&child_read, &parent_write, &sa, 0)) goto fail;
   if (!CreatePipe(&parent_read, &child_write, &sa, 0)) goto pipe_fail;
   if (!SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0) ||
       !SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0))
      goto pipe_fail;

   /* Chromium adopts these two inherited handles as its CDP input/output. */
   const size_t command_chars = wcslen(browser) + wcslen(profile) + 256;
   wchar_t *command = malloc(command_chars * sizeof *command);
   if (!command) goto pipe_fail;
   int written = swprintf(command, command_chars,
      L"\"%ls\" --headless --no-first-run --no-default-browser-check "
      L"--remote-debugging-pipe "
      L"--remote-debugging-io-pipes=%lu,%lu "
      L"--user-data-dir=\"%ls\" about:blank",
      browser, (unsigned long)(uintptr_t)child_read,
      (unsigned long)(uintptr_t)child_write, profile);
   if (written < 0 || (size_t)written >= command_chars) {
      free(command);
      goto pipe_fail;
   }

   SIZE_T attribute_bytes = 0;
   (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_bytes);
   LPPROC_THREAD_ATTRIBUTE_LIST attributes = malloc(attribute_bytes);
   if (!attributes) { free(command); goto pipe_fail; }
   if (!InitializeProcThreadAttributeList(attributes, 1, 0,
                                          &attribute_bytes)) {
      free(attributes); free(command); goto pipe_fail;
   }
   HANDLE inherited[2] = { child_read, child_write };
   BOOL ready = UpdateProcThreadAttribute(attributes, 0,
      PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited,
      NULL, NULL);
   STARTUPINFOEXW startup = { 0 };
   startup.StartupInfo.cb = sizeof startup;
   startup.lpAttributeList = attributes;
   PROCESS_INFORMATION process = { 0 };
   BOOL launched = ready && CreateProcessW(browser, command, NULL, NULL,
      TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL,
      &startup.StartupInfo, &process);
   DeleteProcThreadAttributeList(attributes);
   free(attributes);
   free(command);
   if (!launched) goto pipe_fail;
   CloseHandle(process.hThread);
   CloseHandle(child_read);
   CloseHandle(child_write);
   free(browser);
   free(profile);
   host->input_handle = parent_read;
   host->output_handle = parent_write;
   host->process_handle = process.hProcess;
   host->cdp.context = host;
   host->cdp.read = host_read;
   host->cdp.write = host_write;
   return 0;

pipe_fail:
   if (child_read) CloseHandle(child_read);
   if (child_write) CloseHandle(child_write);
   if (parent_read) CloseHandle(parent_read);
   if (parent_write) CloseHandle(parent_write);
fail:
   free(browser);
   free(profile);
   return -1;
}

void lunaria_cdp_host_close(struct lunaria_cdp_host *host)
{
   if (!host) return;
   lunaria_cdp_close(&host->cdp);
   if (host->output_handle) CloseHandle((HANDLE)host->output_handle);
   if (host->input_handle) CloseHandle((HANDLE)host->input_handle);
   if (host->process_handle) {
      HANDLE process = (HANDLE)host->process_handle;
      if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) {
         TerminateProcess(process, 1);
         WaitForSingleObject(process, INFINITE);
      }
      CloseHandle(process);
   }
   host->input_handle = host->output_handle = host->process_handle = NULL;
}
#endif
