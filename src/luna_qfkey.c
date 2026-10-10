/* SPDX-License-Identifier: MPL-2.0 */
#include "luna_qfkey.h"
#include "arm_exec.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#endif

#define QK_MAGIC 0x4e574b51u /* "QKWN" */
#define QK_MAX_W 3840
#define QK_MAX_H 2160
#define QK_HDR_SIZE 128u
#define QK_PLANE_SIZE ((size_t)QK_MAX_W * QK_MAX_H * 4u)

/* Shared header (little endian); keep in sync with lunaria_bridge.rs. */
typedef struct qk_hdr {
   uint32_t magic, version;
   uint32_t width, height, stride;
   uint32_t bgra;                 /* 0 = RGBA, 1 = BGRA */
   _Atomic uint32_t front;        /* plane index holding the newest frame */
   uint32_t pad0;
   _Atomic uint64_t seq;          /* bumped after each published frame */
   _Atomic uint32_t subscribed;   /* qfkey sets/clears via control channel; mirrored here */
   uint32_t pad1[11];
} qk_hdr;

enum { M_HELLO = 1, M_RESIZE = 2,
       M_SUBSCRIBE = 10, M_UNSUBSCRIBE = 11, M_TOUCH = 12, M_KEY = 13 };

static qk_hdr *g_hdr;
static uint8_t *g_planes;
static char g_shm_name[64];
static char g_title[96] = "lunaria";
static int g_w, g_h;
static _Atomic int g_sub;
static _Atomic int g_started;
static _Atomic int g_connected;

bool luna_qfkey_wants_frame(void) { return atomic_load_explicit(&g_sub, memory_order_relaxed) != 0; }

/* ---- platform layer ------------------------------------------------------
 * shm_create()   map the frame segment, filling g_hdr/g_planes/g_shm_name
 * io_connect()   open the control channel to qfkey (0 on success)
 * io_read_full() / io_write_all()  blocking, usable from different threads
 * io_close()     drop the channel
 * sleep_ms(), start_thread(), get_pid()
 */
#ifdef _WIN32

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_wr_cs;
static INIT_ONCE g_cs_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK cs_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_wr_cs); return TRUE; }

static unsigned get_pid(void) { return (unsigned)GetCurrentProcessId(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }

static int shm_create(void)
{
   /* "Local\" = the current logon session, which is where qfkey's user
    * session and lunaria run together. */
   snprintf(g_shm_name, sizeof g_shm_name, "Local\\qfkey-lunaria-%u", get_pid());
   const size_t total = QK_HDR_SIZE + 2 * QK_PLANE_SIZE;
   HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                 (DWORD)((uint64_t)total >> 32), (DWORD)(total & 0xffffffffu),
                                 g_shm_name);
   if (!m) return -1;
   void *v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, total);
   /* The view keeps the section alive; the name goes away with the process. */
   if (!v) { CloseHandle(m); return -1; }
   g_hdr = (qk_hdr *)v;
   g_planes = (uint8_t *)v + QK_HDR_SIZE;
   return 0;
}

static const char *pipe_name(char *buf, size_t n)
{
   const char *e = getenv("LUNARIA_QFKEY");
   if (e && !strncmp(e, "\\\\.\\pipe\\", 9)) { snprintf(buf, n, "%s", e); return buf; }
   return "\\\\.\\pipe\\qfkey-lunaria";
}

static int io_connect(void)
{
   char nb[128];
   const char *name = pipe_name(nb, sizeof nb);
   InitOnceExecuteOnce(&g_cs_once, cs_init, NULL, NULL);
   for (int i = 0; i < 2; ++i) {
      /* Overlapped, so a blocked read does not stall writes from other threads. */
      HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, NULL);
      if (h != INVALID_HANDLE_VALUE) { g_pipe = h; return 0; }
      if (GetLastError() != ERROR_PIPE_BUSY) return -1;
      WaitNamedPipeA(name, 1000);
   }
   return -1;
}

static void io_close(void)
{
   HANDLE h = g_pipe;
   g_pipe = INVALID_HANDLE_VALUE;
   if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

/* One overlapped transfer to completion; returns bytes moved or -1. */
static int ov_io(int is_write, void *buf, DWORD n)
{
   HANDLE h = g_pipe;
   if (h == INVALID_HANDLE_VALUE) return -1;
   OVERLAPPED ov;
   memset(&ov, 0, sizeof ov);
   ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
   if (!ov.hEvent) return -1;
   DWORD got = 0;
   BOOL ok = is_write ? WriteFile(h, buf, n, NULL, &ov) : ReadFile(h, buf, n, NULL, &ov);
   if (!ok && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return -1; }
   ok = GetOverlappedResult(h, &ov, &got, TRUE);
   CloseHandle(ov.hEvent);
   return ok ? (int)got : -1;
}

static int io_read_full(void *buf, size_t n)
{
   uint8_t *p = buf;
   while (n) {
      int r = ov_io(0, p, (DWORD)n);
      if (r <= 0) return -1;
      p += r; n -= (size_t)r;
   }
   return 0;
}

static int io_write_all(const void *buf, size_t n)
{
   const uint8_t *p = buf;
   EnterCriticalSection(&g_wr_cs);
   int bad = 0;
   while (n && !bad) {
      int r = ov_io(1, (void *)p, (DWORD)n);
      if (r <= 0) bad = 1; else { p += r; n -= (size_t)r; }
   }
   LeaveCriticalSection(&g_wr_cs);
   return bad ? -1 : 0;
}

static DWORD WINAPI connector(LPVOID arg);
static int start_thread(void)
{
   HANDLE t = CreateThread(NULL, 0, connector, NULL, 0, NULL);
   if (!t) return -1;
   CloseHandle(t);
   return 0;
}

#else /* POSIX */

static int g_sock = -1;
static pthread_mutex_t g_wr_mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned get_pid(void) { return (unsigned)getpid(); }
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }

static int shm_create(void)
{
   snprintf(g_shm_name, sizeof g_shm_name, "/qfkey-lunaria-%u", get_pid());
   int fd = shm_open(g_shm_name, O_CREAT | O_RDWR, 0600);
   if (fd < 0) return -1;
   const size_t total = QK_HDR_SIZE + 2 * QK_PLANE_SIZE;
   if (ftruncate(fd, (off_t)total) != 0) { close(fd); shm_unlink(g_shm_name); return -1; }
   void *m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   close(fd);
   if (m == MAP_FAILED) { shm_unlink(g_shm_name); return -1; }
   g_hdr = (qk_hdr *)m;
   g_planes = (uint8_t *)m + QK_HDR_SIZE;
   return 0;
}

static void shm_cleanup(void) { if (g_shm_name[0]) shm_unlink(g_shm_name); }

/* A lunaria that was killed (SIGKILL, timeout) cannot unlink its segment;
 * remove those whose owner no longer exists.  Linux only: that is the one
 * place POSIX shm is a listable directory. */
static void shm_reap_stale(void)
{
#ifdef __linux__
   DIR *d = opendir("/dev/shm");
   if (!d) return;
   struct dirent *e;
   while ((e = readdir(d)) != NULL) {
      unsigned pid;
      if (sscanf(e->d_name, "qfkey-lunaria-%u", &pid) != 1 || pid == get_pid()) continue;
      if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
         char n[300];
         snprintf(n, sizeof n, "/%s", e->d_name);
         shm_unlink(n);
      }
   }
   closedir(d);
#endif
}

static const char *sock_path(char *buf, size_t n)
{
   const char *e = getenv("LUNARIA_QFKEY");
   if (e && e[0] == '/') return e;
   const char *rt = getenv("XDG_RUNTIME_DIR");
   if (rt && rt[0]) { snprintf(buf, n, "%s/qfkey-lunaria.sock", rt); return buf; }
   return "/tmp/qfkey-lunaria.sock";
}

static int io_connect(void)
{
   char pb[128];
   const char *path = sock_path(pb, sizeof pb);
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un sa;
   memset(&sa, 0, sizeof sa);
   sa.sun_family = AF_UNIX;
   strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
   if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
      if (fd >= 0) close(fd);
      return -1;
   }
   g_sock = fd;
   return 0;
}

static void io_close(void)
{
   int fd = g_sock;
   g_sock = -1;
   if (fd >= 0) close(fd);
}

static int io_read_full(void *buf, size_t n)
{
   uint8_t *p = buf;
   while (n) {
      ssize_t r = read(g_sock, p, n);
      if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
      p += r; n -= (size_t)r;
   }
   return 0;
}

static int io_write_all(const void *buf, size_t n)
{
   const uint8_t *p = buf;
   int bad = 0;
   pthread_mutex_lock(&g_wr_mu);
   while (n && !bad) {
      ssize_t r = write(g_sock, p, n);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) bad = 1; else { p += r; n -= (size_t)r; }
   }
   pthread_mutex_unlock(&g_wr_mu);
   return bad ? -1 : 0;
}

static void *connector(void *arg);
static int start_thread(void)
{
   pthread_t t;
   if (pthread_create(&t, NULL, connector, NULL) != 0) return -1;
   pthread_detach(t);
   return 0;
}

#endif

/* ---- protocol (platform independent) ------------------------------------ */

static int send_msg(uint8_t type, const void *payload, uint16_t len)
{
   uint8_t msg[3 + 256];
   if (len > 256) return -1;
   msg[0] = type; msg[1] = (uint8_t)(len & 0xff); msg[2] = (uint8_t)(len >> 8);
   if (len) memcpy(msg + 3, payload, len);
   return io_write_all(msg, 3u + len); /* one write: header and body stay together */
}

static void send_hello(void)
{
   uint8_t b[256]; size_t o = 0;
   uint32_t pid = get_pid(), w = (uint32_t)g_w, h = (uint32_t)g_h;
   memcpy(b + o, &pid, 4); o += 4;
   memcpy(b + o, &w, 4); o += 4;
   memcpy(b + o, &h, 4); o += 4;
   size_t nl = strlen(g_shm_name), tl = strlen(g_title);
   b[o++] = (uint8_t)nl; memcpy(b + o, g_shm_name, nl); o += nl;
   b[o++] = (uint8_t)tl; memcpy(b + o, g_title, tl); o += tl;
   send_msg(M_HELLO, b, (uint16_t)o);
}

/* Input arrives on the connector thread, but the emulator's input queues are
 * only touched from its pump thread (arm_exec_glfw_poll).  A single-producer /
 * single-consumer ring hands events across; luna_qfkey_pump_input() drains it
 * on the pump thread.  Full ring = drop (a stalled guest must not block us). */
#define QK_RING 128
typedef struct qk_in { uint8_t kind; int a; float x, y; } qk_in; /* kind 1 touch(a=action), 2 key(a=android code) */
static qk_in g_ring[QK_RING];
static _Atomic unsigned g_head, g_tail;

static void ring_push(qk_in e)
{
   unsigned h = atomic_load_explicit(&g_head, memory_order_relaxed);
   if (h - atomic_load_explicit(&g_tail, memory_order_acquire) >= QK_RING) return;
   g_ring[h % QK_RING] = e;
   atomic_store_explicit(&g_head, h + 1, memory_order_release);
}

void luna_qfkey_pump_input(void)
{
   unsigned t = atomic_load_explicit(&g_tail, memory_order_relaxed);
   while (t != atomic_load_explicit(&g_head, memory_order_acquire)) {
      qk_in e = g_ring[t % QK_RING];
      ++t;
      atomic_store_explicit(&g_tail, t, memory_order_release);
      if (getenv("LUNARIA_QFKEY_DEBUG"))
         fprintf(stderr, "[qfkey] input kind=%d a=%d x=%.1f y=%.1f\n", e.kind, e.a, e.x, e.y);
      if (e.kind == 1) arm_exec_touch_push(e.a, e.x, e.y);
      else if (e.kind == 2) arm_exec_android_key(e.a);
   }
}

static void on_touch(const uint8_t *p, uint16_t len)
{
   if (len < 9) return;
   qk_in e = { 1, p[0], 0, 0 }; /* action: 0 down, 1 up, 2 move */
   memcpy(&e.x, p + 1, 4); memcpy(&e.y, p + 5, 4);
   if (e.a <= 2) ring_push(e);
}

static void on_key(const uint8_t *p, uint16_t len)
{
   if (len < 2 || !p[1]) return;
   int code = 0;
   switch (p[0]) { /* HID usage -> Android keycode, only the basics for now */
   case 0x29: code = 4; break;    /* Esc -> BACK */
   case 0x28: code = 66; break;   /* Enter */
   case 0x2a: code = 67; break;   /* Backspace -> DEL */
   default: break;
   }
   if (code) { qk_in e = { 2, code, 0, 0 }; ring_push(e); }
}

static void session(void)
{
   send_hello();
   atomic_store(&g_connected, 1);
   fprintf(stderr, "[qfkey] connected as %s\n", g_shm_name);
   for (;;) {
      uint8_t head[3], pay[64];
      if (io_read_full(head, 3) != 0) break;
      uint16_t len = (uint16_t)(head[1] | (head[2] << 8));
      if (len > sizeof pay) {  /* drain unknown/oversized */
         uint8_t tmp[256]; uint16_t left = len; int bad = 0;
         while (left && !bad) { uint16_t c = left > sizeof tmp ? (uint16_t)sizeof tmp : left; bad = io_read_full(tmp, c); left = (uint16_t)(left - c); }
         if (bad) break;
         continue;
      }
      if (len && io_read_full(pay, len) != 0) break;
      switch (head[0]) {
      case M_SUBSCRIBE:   atomic_store(&g_sub, 1); atomic_store(&g_hdr->subscribed, 1); break;
      case M_UNSUBSCRIBE: atomic_store(&g_sub, 0); atomic_store(&g_hdr->subscribed, 0); break;
      case M_TOUCH:       on_touch(pay, len); break;
      case M_KEY:         on_key(pay, len); break;
      default: break;
      }
   }
   atomic_store(&g_connected, 0);
   atomic_store(&g_sub, 0);
   atomic_store(&g_hdr->subscribed, 0);
   io_close();
   fprintf(stderr, "[qfkey] disconnected, retrying\n");
}

#ifdef _WIN32
static DWORD WINAPI connector(LPVOID arg)
#else
static void *connector(void *arg)
#endif
{
   (void)arg;
   for (;;) {
      if (io_connect() != 0) { sleep_ms(2000); continue; }
      session();
   }
   return 0;
}

void luna_qfkey_start(const char *title, int width, int height)
{
   const char *e = getenv("LUNARIA_QFKEY");
   if (e && (!strcmp(e, "off") || !strcmp(e, "0"))) return;
   int expected = 0;
   if (!atomic_compare_exchange_strong(&g_started, &expected, 1)) return;
   if (title && title[0]) snprintf(g_title, sizeof g_title, "%s", title);
   g_w = width > 0 ? width : 1280; g_h = height > 0 ? height : 720;
#ifndef _WIN32
   shm_reap_stale();
#endif
   if (shm_create() != 0) { fprintf(stderr, "[qfkey] shared memory unavailable\n"); return; }
   memset(g_hdr, 0, sizeof *g_hdr);
   g_hdr->magic = QK_MAGIC;
   g_hdr->version = 1;
   g_hdr->width = (uint32_t)g_w; g_hdr->height = (uint32_t)g_h; g_hdr->stride = (uint32_t)g_w * 4u;
#ifndef _WIN32
   atexit(shm_cleanup);
#endif
   if (start_thread() != 0) fprintf(stderr, "[qfkey] cannot start connector thread\n");
}

void luna_qfkey_publish(const void *pixels, int w, int h, int stride, bool bgra)
{
   if (!g_hdr || !luna_qfkey_wants_frame() || !pixels || w <= 0 || h <= 0 ||
       w > QK_MAX_W || h > QK_MAX_H)
      return;
   uint32_t back = atomic_load_explicit(&g_hdr->front, memory_order_relaxed) ^ 1u;
   uint8_t *dst = g_planes + (size_t)back * QK_PLANE_SIZE;
   const size_t row = (size_t)w * 4u;
   for (int y = 0; y < h; ++y)
      memcpy(dst + (size_t)y * row, (const uint8_t *)pixels + (size_t)y * (size_t)stride, row);
   g_hdr->width = (uint32_t)w; g_hdr->height = (uint32_t)h;
   g_hdr->stride = (uint32_t)row; g_hdr->bgra = bgra ? 1u : 0u;
   atomic_store_explicit(&g_hdr->front, back, memory_order_release);
   atomic_fetch_add_explicit(&g_hdr->seq, 1, memory_order_release);
   if (w != g_w || h != g_h) {
      g_w = w; g_h = h;
      if (atomic_load(&g_connected)) { uint32_t d[2] = { (uint32_t)w, (uint32_t)h }; send_msg(M_RESIZE, d, 8); }
   }
}
