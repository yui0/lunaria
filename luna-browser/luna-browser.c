/*
 * luna-browser — a small web engine: luna-ui lays out and paints the
 * document, QuickJS runs its scripts.  C11.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 *
 * One process is one page.  It is driven three ways, plus the window:
 *
 *   luna-browser --remote-debugging-pipe [--user-data-dir=DIR] [URL]
 *       Headless, speaking the Chrome DevTools Protocol subset an embedder
 *       needs (targets, navigation, screenshots, input, Runtime.evaluate,
 *       bindings) as NUL-terminated JSON on file descriptors 3 (in) and
 *       4 (out) — the same pipe Chromium's --remote-debugging-pipe uses, so
 *       an embedder that drives Chrome drives this unchanged.
 *   luna-browser --screenshot[=FILE] [--window-size=W,H] URL
 *       Headless: load, let timers settle, write a PNG, exit.
 *   luna-browser [--window-size=W,H] [URL]        (built with GLFW)
 *       A browser window: address bar, back/forward/reload.  The page runs
 *       in a child in the first mode; this process draws the toolbar and
 *       the frames the child streams (Page.startScreencast).
 *       Keys: Ctrl+L / F6 address bar, Ctrl+R / F5 reload, Alt+Left/Right.
 *
 * The document *is* luna-ui's element tree: scripts mutate it through the
 * luna_dom_* API, so there is no second DOM to keep in sync and no reparse.
 * The DOM/event/timer/network surface scripts see is luna-browser.js, run by
 * QuickJS on top of the small native API installed below as `__lb`.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef LB_WINDOW
#define LUNA_UI_NO_PLATFORM
#endif
#define LUNA_UI_NANOSVG
#define LUNA_UI_IMPLEMENTATION
#include "luna-ui.h"

#include "quickjs.h"
#include <dlfcn.h>
#include <pthread.h>

/* WebP, which stb_image does not read (Bing's picture of the day, among others). */
#define SIMPLEWEBP_DISABLE_STDIO
#define SIMPLEWEBP_IMPLEMENTATION
#include "simplewebp.h"

static int is_webp(const unsigned char *b, size_t n)
{
   return n > 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4);
}

static pthread_once_t g_webp_once = PTHREAD_ONCE_INIT;
static int (*g_webp_info)(const unsigned char *, size_t, int *, int *);
static unsigned char *(*g_webp_decode)(const unsigned char *, size_t, unsigned char *, size_t, int);

static void webp_decoder_init(void)
{
   if (getenv("LUNA_BROWSER_SOFTWARE_WEBP")) return;
   static const char *names[] = { "libwebp.so.7", "libwebp.so", NULL };
   for (int i = 0; names[i]; ++i) {
      void *lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
      if (!lib) continue;
      g_webp_info = dlsym(lib, "WebPGetInfo");
      g_webp_decode = dlsym(lib, "WebPDecodeRGBAInto");
      if (g_webp_info && g_webp_decode) return; /* keep the library loaded */
      g_webp_info = NULL; g_webp_decode = NULL;
      dlclose(lib);
   }
}

static unsigned char *lb_decode_webp(const unsigned char *bytes, size_t len, int *w, int *h)
{
   if (!is_webp(bytes, len)) return NULL;
   /* Use the system's SIMD decoder when installed, retaining the bundled
    * decoder for systems without libwebp. Decode into malloc-owned memory so
    * the renderer keeps its existing free() ownership contract. */
   pthread_once(&g_webp_once, webp_decoder_init);
   int nw = 0, nh = 0;
   if (g_webp_info && g_webp_info(bytes, len, &nw, &nh) && nw > 0 && nh > 0 &&
       nw <= 16384 && nh <= 16384 && (size_t)nw * nh <= ((size_t)1 << 27)) {
      size_t size = (size_t)nw * nh * 4;
      unsigned char *pixels = malloc(size);
      if (pixels && g_webp_decode(bytes, len, pixels, size, nw * 4)) {
         *w = nw; *h = nh;
         return pixels;
      }
      free(pixels);
   }
   simplewebp *dec = NULL;
   if (simplewebp_load_from_memory((void *)bytes, len, NULL, &dec) != SIMPLEWEBP_NO_ERROR) return NULL;
   size_t pw = 0, ph = 0;
   simplewebp_get_dimensions(dec, &pw, &ph);
   unsigned char *rgba = NULL;
   if (pw && ph && pw <= 16384 && ph <= 16384 && pw * ph <= ((size_t)1 << 27)) {
      rgba = malloc(pw * ph * 4);
      if (rgba && simplewebp_decode(dec, rgba, NULL) != SIMPLEWEBP_NO_ERROR) { free(rgba); rgba = NULL; }
   }
   simplewebp_unload(dec);
   if (rgba) { *w = (int)pw; *h = (int)ph; }
   return rgba;
}

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef LB_NO_ZLIB
#include <zlib.h>
#endif

/* Incremental HTTP/1.1 response framing.  A return value of 1 means that the
 * response is complete, 0 requests more bytes, and -1 rejects invalid framing.
 * No socket close is needed for length-delimited or chunked responses. */
struct http_frame {
   size_t start, body, cursor, length;
   int headers, chunked, trailers, close_delimited, no_body, complete;
};

static int http_decimal(const char *p, const char *end, size_t *out)
{
   int found = 0;
   size_t first = 0;
   for (;;) {
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if (p == end || !isdigit((unsigned char)*p)) return -1;
      size_t n = 0;
      while (p < end && isdigit((unsigned char)*p)) {
         unsigned d = (unsigned)(*p++ - '0');
         if (n > (SIZE_MAX - d) / 10) return -1;
         n = n * 10 + d;
      }
      if (found && n != first) return -1;
      first = n; found = 1;
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if (p == end) { *out = first; return 0; }
      if (*p++ != ',') return -1;
   }
}

static int http_frame_feed(struct http_frame *f, const char *data, size_t n,
                           const char *method)
{
   if (f->complete) return 1;
   while (!f->headers) {
      const char *begin = data + f->start;
      const char *end = memmem(begin, n - f->start, "\r\n\r\n", 4);
      if (!end) return 0;
      const char *line = memmem(begin, (size_t)(end - begin), "\r\n", 2);
      if (!line) line = end;
      const char *space = memchr(begin, ' ', (size_t)(line - begin));
      if (!space || line - space < 4 || !isdigit((unsigned char)space[1]) ||
          !isdigit((unsigned char)space[2]) || !isdigit((unsigned char)space[3])) return -1;
      int status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
      size_t body = (size_t)(end - data) + 4;
      if (status >= 100 && status < 200 && status != 101) {
         f->start = body;
         if (body == n) return 0;
         continue;
      }
      int have_length = 0, have_transfer = 0;
      for (const char *p = line + 2; p < end; ) {
         const char *next = memmem(p, (size_t)(end + 2 - p), "\r\n", 2);
         if (!next) return -1;
         const char *colon = memchr(p, ':', (size_t)(next - p));
         if (!colon) return -1;
         const char *value = colon + 1;
         while (value < next && (*value == ' ' || *value == '\t')) value++;
         const char *tail = next;
         while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) tail--;
         if (colon - p == 14 && !strncasecmp(p, "content-length", 14)) {
            size_t length;
            if (http_decimal(value, tail, &length) < 0 ||
                (have_length && f->length != length)) return -1;
            f->length = length; have_length = 1;
         } else if (colon - p == 17 && !strncasecmp(p, "transfer-encoding", 17)) {
            if (have_transfer || tail - value != 7 || strncasecmp(value, "chunked", 7)) return -1;
            f->chunked = 1; have_transfer = 1;
         }
         p = next + 2;
      }
      f->body = f->cursor = body;
      f->headers = 1;
      f->no_body = !strcasecmp(method, "HEAD") || status == 101 || status == 204 || status == 304;
      if (f->no_body) { f->complete = 1; return 1; }
      if (have_transfer && have_length) return -1;
      f->close_delimited = !have_transfer && !have_length;
   }
   if (f->close_delimited) return 0;
   if (!f->chunked) {
      if (n - f->body >= f->length) { f->complete = 1; return 1; }
      return 0;
   }
   for (;;) {
      if (f->length) {
         if (n - f->cursor < f->length || n - f->cursor - f->length < 2) return 0;
         f->cursor += f->length;
         if (data[f->cursor] != '\r' || data[f->cursor + 1] != '\n') return -1;
         f->cursor += 2; f->length = 0;
      }
      const char *line = data + f->cursor;
      const char *end = memmem(line, n - f->cursor, "\r\n", 2);
      if (!end) return 0;
      if (f->trailers) {
         f->cursor = (size_t)(end - data) + 2;
         if (end == line) { f->complete = 1; return 1; }
         if (!memchr(line, ':', (size_t)(end - line))) return -1;
         continue;
      }
      size_t length = 0;
      const char *p = line;
      if (p == end || !isxdigit((unsigned char)*p)) return -1;
      while (p < end && isxdigit((unsigned char)*p)) {
         unsigned c = (unsigned char)*p++;
         unsigned digit = c <= '9' ? c - '0' : (c | 32u) - 'a' + 10u;
         if (length > (SIZE_MAX - digit) / 16) return -1;
         length = length * 16 + digit;
      }
      while (p < end && (*p == ' ' || *p == '\t')) p++;
      if (p < end && *p != ';') return -1;
      f->cursor = (size_t)(end - data) + 2;
      f->length = length;
      if (!length) f->trailers = 1;
   }
}

#ifndef LB_NO_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#define LB_VERSION "0.1"
#define LB_PRODUCT "LunaBrowser/" LB_VERSION

static const char lb_prelude[] =
#include "luna-browser.js.h"
;

/* ======================================================================== *
 * Small utilities
 * ======================================================================== */

static int g_verbose;
static double g_t0;
static double lb_now_ms(void);
#define LB_LOG(...) do { if (g_verbose) { fprintf(stderr, "[luna-browser %.0f] ", lb_now_ms() - g_t0); fprintf(stderr, __VA_ARGS__); } } while (0)

static double lb_now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static char *lb_strndup(const char *s, size_t n)
{
   char *p = malloc(n + 1);
   if (!p) return NULL;
   memcpy(p, s, n);
   p[n] = '\0';
   return p;
}

static char *lb_strdup(const char *s) { return s ? lb_strndup(s, strlen(s)) : NULL; }

struct buf { char *p; size_t len, cap; };

static void buf_put(struct buf *b, const void *s, size_t n)
{
   if (b->len + n + 1 > b->cap) {
      size_t cap = b->cap ? b->cap : 256;
      while (cap < b->len + n + 1) cap *= 2;
      char *g = realloc(b->p, cap);
      if (!g) return;
      b->p = g;
      b->cap = cap;
   }
   memcpy(b->p + b->len, s, n);
   b->len += n;
   b->p[b->len] = '\0';
}

static void buf_puts(struct buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static void buf_printf(struct buf *b, const char *fmt, ...)
{
   char tmp[512];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
   va_end(ap);
   if (n < 0) return;
   if ((size_t)n < sizeof tmp) { buf_put(b, tmp, (size_t)n); return; }
   char *big = malloc((size_t)n + 1);
   if (!big) return;
   va_start(ap, fmt);
   vsnprintf(big, (size_t)n + 1, fmt, ap);
   va_end(ap);
   buf_put(b, big, (size_t)n);
   free(big);
}

static char *buf_take(struct buf *b)
{
   char *p = b->p ? b->p : lb_strdup("");
   b->p = NULL;
   b->len = b->cap = 0;
   return p;
}

/* JSON string literal of UTF-8 text. */
static void buf_json_str(struct buf *b, const char *s)
{
   buf_put(b, "\"", 1);
   for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
      switch (*p) {
      case '"': buf_puts(b, "\\\""); break;
      case '\\': buf_puts(b, "\\\\"); break;
      case '\n': buf_puts(b, "\\n"); break;
      case '\r': buf_puts(b, "\\r"); break;
      case '\t': buf_puts(b, "\\t"); break;
      default:
         if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
         else buf_put(b, p, 1);
      }
   }
   buf_put(b, "\"", 1);
}

static const char b64_table[] =
   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void buf_base64(struct buf *b, const unsigned char *d, size_t n)
{
   for (size_t i = 0; i < n; i += 3) {
      unsigned v = (unsigned)d[i] << 16;
      if (i + 1 < n) v |= (unsigned)d[i + 1] << 8;
      if (i + 2 < n) v |= d[i + 2];
      char q[4] = { b64_table[(v >> 18) & 63], b64_table[(v >> 12) & 63],
                    i + 1 < n ? b64_table[(v >> 6) & 63] : '=',
                    i + 2 < n ? b64_table[v & 63] : '=' };
      buf_put(b, q, 4);
   }
}

static unsigned char *base64_decode(const char *s, size_t n, size_t *out_len)
{
   unsigned char *out = malloc(n / 4 * 3 + 4);
   if (!out) return NULL;
   size_t o = 0;
   unsigned v = 0;
   int bits = 0;
   for (size_t i = 0; i < n; ++i) {
      const char *q = strchr(b64_table, s[i]);
      int c = s[i] == '-' ? 62 : s[i] == '_' ? 63 : (q && s[i]) ? (int)(q - b64_table) : -1;
      if (c < 0) continue;
      v = (v << 6) | (unsigned)c;
      bits += 6;
      if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(v >> bits); }
   }
   *out_len = o;
   return out;
}

/* ======================================================================== *
 * JSON (CDP messages)
 * ======================================================================== */

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

struct jv {
   int type;
   double num;
   char *str;              /* J_STR value */
   char *key;              /* member name inside an object */
   struct jv *kid, *next;  /* children (J_ARR/J_OBJ), sibling */
};

static void jv_free(struct jv *v)
{
   while (v) {
      struct jv *next = v->next;
      jv_free(v->kid);
      free(v->str);
      free(v->key);
      free(v);
      v = next;
   }
}

static const char *json_ws(const char *p)
{
   while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
   return p;
}

static void utf8_put(struct buf *b, unsigned cp)
{
   char u[4];
   int n = 0;
   if (cp < 0x80) u[n++] = (char)cp;
   else if (cp < 0x800) { u[n++] = (char)(0xc0 | cp >> 6); u[n++] = (char)(0x80 | (cp & 63)); }
   else if (cp < 0x10000) {
      u[n++] = (char)(0xe0 | cp >> 12); u[n++] = (char)(0x80 | ((cp >> 6) & 63));
      u[n++] = (char)(0x80 | (cp & 63));
   } else {
      u[n++] = (char)(0xf0 | cp >> 18); u[n++] = (char)(0x80 | ((cp >> 12) & 63));
      u[n++] = (char)(0x80 | ((cp >> 6) & 63)); u[n++] = (char)(0x80 | (cp & 63));
   }
   buf_put(b, u, (size_t)n);
}

static const char *json_string(const char *p, char **out)
{
   struct buf b = { 0 };
   if (*p != '"') return NULL;
   for (++p; *p && *p != '"'; ++p) {
      if (*p != '\\') { buf_put(&b, p, 1); continue; }
      ++p;
      switch (*p) {
      case 'n': buf_puts(&b, "\n"); break;
      case 'r': buf_puts(&b, "\r"); break;
      case 't': buf_puts(&b, "\t"); break;
      case 'b': buf_puts(&b, "\b"); break;
      case 'f': buf_puts(&b, "\f"); break;
      case 'u': {
         unsigned cp = 0;
         for (int i = 1; i <= 4; ++i) {
            if (!isxdigit((unsigned char)p[i])) { free(b.p); return NULL; }
            cp = cp * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0'
                                      : (tolower((unsigned char)p[i]) - 'a' + 10));
         }
         p += 4;
         if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
            unsigned lo = (unsigned)strtoul((char[5]){ p[3], p[4], p[5], p[6], 0 }, NULL, 16);
            if (lo >= 0xdc00 && lo < 0xe000) {
               cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
               p += 6;
            }
         }
         utf8_put(&b, cp);
         break;
      }
      case '\0': free(b.p); return NULL;
      default: buf_put(&b, p, 1);
      }
   }
   if (*p != '"') { free(b.p); return NULL; }
   *out = buf_take(&b);
   return p + 1;
}

static const char *json_value(const char *p, struct jv **out, int depth)
{
   p = json_ws(p);
   if (depth > 64) return NULL;
   struct jv *v = calloc(1, sizeof *v);
   if (!v) return NULL;
   *out = v;
   if (*p == '{' || *p == '[') {
      const char close = *p == '{' ? '}' : ']';
      v->type = *p == '{' ? J_OBJ : J_ARR;
      struct jv **tail = &v->kid;
      p = json_ws(p + 1);
      if (*p == close) return p + 1;
      for (;;) {
         char *key = NULL;
         if (v->type == J_OBJ) {
            p = json_string(json_ws(p), &key);
            if (!p) return NULL;
            p = json_ws(p);
            if (*p != ':') { free(key); return NULL; }
            ++p;
         }
         struct jv *kid = NULL;
         p = json_value(p, &kid, depth + 1);
         if (!p) { free(key); jv_free(kid); return NULL; }
         kid->key = key;
         *tail = kid;
         tail = &kid->next;
         p = json_ws(p);
         if (*p == ',') { ++p; continue; }
         if (*p == close) return p + 1;
         return NULL;
      }
   }
   if (*p == '"') { v->type = J_STR; return json_string(p, &v->str); }
   if (!strncmp(p, "true", 4)) { v->type = J_TRUE; return p + 4; }
   if (!strncmp(p, "false", 5)) { v->type = J_FALSE; return p + 5; }
   if (!strncmp(p, "null", 4)) { v->type = J_NULL; return p + 4; }
   char *end;
   v->num = strtod(p, &end);
   if (end == p) return NULL;
   v->type = J_NUM;
   return end;
}

static struct jv *json_parse(const char *text)
{
   struct jv *v = NULL;
   const char *end = json_value(text, &v, 0);
   if (!end) { jv_free(v); return NULL; }
   return v;
}

static const struct jv *jget(const struct jv *o, const char *key)
{
   if (!o || o->type != J_OBJ) return NULL;
   for (const struct jv *k = o->kid; k; k = k->next)
      if (k->key && !strcmp(k->key, key)) return k;
   return NULL;
}

static const char *jstr(const struct jv *o, const char *key)
{
   const struct jv *v = jget(o, key);
   return v && v->type == J_STR ? v->str : NULL;
}

static double jnum(const struct jv *o, const char *key, double fallback)
{
   const struct jv *v = jget(o, key);
   return v && v->type == J_NUM ? v->num : fallback;
}

static int jbool(const struct jv *o, const char *key)
{
   const struct jv *v = jget(o, key);
   return v && v->type == J_TRUE;
}

/* ======================================================================== *
 * URLs (RFC 3986 reference resolution)
 * ======================================================================== */

struct url {
   char scheme[16];
   char *host;          /* NULL for no authority */
   int port;            /* -1: default */
   char *path, *query, *fragment;   /* query/fragment without ?/# or NULL */
};

static void url_free(struct url *u)
{
   free(u->host); free(u->path); free(u->query); free(u->fragment);
   memset(u, 0, sizeof *u);
}

static int url_parse(const char *s, struct url *u)
{
   memset(u, 0, sizeof *u);
   u->port = -1;
   const char *p = s;
   while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
   if (*p != ':' || p == s || (size_t)(p - s) >= sizeof u->scheme) return -1;
   for (size_t i = 0; i < (size_t)(p - s); ++i) u->scheme[i] = (char)tolower((unsigned char)s[i]);
   p++;
   if (p[0] == '/' && p[1] == '/') {
      p += 2;
      const char *end = p + strcspn(p, "/?#");
      const char *at = memchr(p, '@', (size_t)(end - p));
      if (at) p = at + 1;                       /* userinfo is dropped */
      const char *colon = NULL;
      if (*p == '[') {
         const char *rb = memchr(p, ']', (size_t)(end - p));
         if (rb && rb + 1 < end && rb[1] == ':') colon = rb + 1;
      } else colon = memchr(p, ':', (size_t)(end - p));
      const char *hend = colon ? colon : end;
      u->host = lb_strndup(p, (size_t)(hend - p));
      for (char *h = u->host; h && *h; ++h) *h = (char)tolower((unsigned char)*h);
      if (colon && colon + 1 < end) u->port = atoi(colon + 1);
      p = end;
   }
   size_t plen = strcspn(p, "?#");
   u->path = lb_strndup(p, plen);
   p += plen;
   if (*p == '?') {
      size_t q = strcspn(p + 1, "#");
      u->query = lb_strndup(p + 1, q);
      p += 1 + q;
   }
   if (*p == '#') u->fragment = lb_strdup(p + 1);
   return 0;
}

static char *url_format(const struct url *u)
{
   struct buf b = { 0 };
   buf_puts(&b, u->scheme);
   buf_puts(&b, ":");
   if (u->host) {
      buf_puts(&b, "//");
      buf_puts(&b, u->host);
      if (u->port >= 0) buf_printf(&b, ":%d", u->port);
   }
   buf_puts(&b, u->path ? u->path : "");
   if (u->query) { buf_puts(&b, "?"); buf_puts(&b, u->query); }
   if (u->fragment) { buf_puts(&b, "#"); buf_puts(&b, u->fragment); }
   return buf_take(&b);
}

/* RFC 3986 5.2.4 */
static char *remove_dot_segments(const char *path)
{
   size_t n = strlen(path);
   char *out = malloc(n + 2);
   if (!out) return NULL;
   size_t o = 0;
   const char *in = path;
   while (*in) {
      if (!strncmp(in, "../", 3)) in += 3;
      else if (!strncmp(in, "./", 2)) in += 2;
      else if (!strncmp(in, "/./", 3)) in += 2;
      else if (!strcmp(in, "/.")) { in += 1; out[o++] = '/'; break; }
      else if (!strncmp(in, "/../", 4) || !strcmp(in, "/..")) {
         in += 3;
         while (o > 0 && out[o - 1] != '/') o--;
         if (o > 0) o--;
         if (!*in) { out[o++] = '/'; break; }
      } else if (!strcmp(in, ".") || !strcmp(in, "..")) break;
      else {
         const char *seg = in;
         if (*seg == '/') seg++;
         const char *e = strchr(seg, '/');
         size_t len = e ? (size_t)(e - in) : strlen(in);
         memcpy(out + o, in, len);
         o += len;
         in += len;
      }
   }
   out[o] = '\0';
   return out;
}

static char *url_resolve(const char *base, const char *ref)
{
   if (!ref) return NULL;
   while (*ref == ' ' || *ref == '\t' || *ref == '\n') ref++;
   struct url r;
   if (url_parse(ref, &r) == 0) {
      char *dots = remove_dot_segments(r.path);
      free(r.path);
      r.path = dots;
      char *s = url_format(&r);
      url_free(&r);
      return s;
   }
   struct url b;
   if (!base || url_parse(base, &b) != 0) return lb_strdup(ref);
   struct url t = { 0 };
   t.port = -1;
   memcpy(t.scheme, b.scheme, sizeof t.scheme);
   const char *q = strpbrk(ref, "?#");
   size_t plen = q ? (size_t)(q - ref) : strlen(ref);
   char *rpath = lb_strndup(ref, plen);
   char *rquery = NULL, *rfrag = NULL;
   if (q && *q == '?') {
      size_t ql = strcspn(q + 1, "#");
      rquery = lb_strndup(q + 1, ql);
      q += 1 + ql;
   }
   if (q && *q == '#') rfrag = lb_strdup(q + 1);
   if (ref[0] == '/' && ref[1] == '/') {
      char *tmp = malloc(strlen(b.scheme) + strlen(ref) + 2);
      sprintf(tmp, "%s:%s", b.scheme, ref);
      char *s = url_resolve(NULL, tmp);
      free(tmp); free(rpath); free(rquery); free(rfrag); url_free(&b);
      return s;
   }
   t.host = lb_strdup(b.host);
   t.port = b.port;
   if (!rpath[0]) {
      t.path = lb_strdup(b.path);
      t.query = rquery ? rquery : lb_strdup(b.query);
      if (!rquery) rquery = NULL;
   } else {
      if (rpath[0] == '/') t.path = remove_dot_segments(rpath);
      else {
         const char *bp = b.path ? b.path : "";
         const char *slash = strrchr(bp, '/');
         struct buf m = { 0 };
         if (b.host && !bp[0]) buf_puts(&m, "/");
         else if (slash) buf_put(&m, bp, (size_t)(slash - bp + 1));
         buf_puts(&m, rpath);
         char *merged = buf_take(&m);
         t.path = remove_dot_segments(merged);
         free(merged);
      }
      t.query = rquery;
   }
   t.fragment = rfrag;
   free(rpath);
   char *s = url_format(&t);
   url_free(&t);
   url_free(&b);
   return s;
}

static char *percent_decode(const char *s, size_t n)
{
   char *out = malloc(n + 1);
   if (!out) return NULL;
   size_t o = 0;
   for (size_t i = 0; i < n; ++i) {
      if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) &&
          isxdigit((unsigned char)s[i + 2])) {
         char hx[3] = { s[i + 1], s[i + 2], 0 };
         out[o++] = (char)strtol(hx, NULL, 16);
         i += 2;
      } else out[o++] = s[i];
   }
   out[o] = '\0';
   return out;
}

static char *url_origin(const char *s)
{
   struct url u;
   if (url_parse(s, &u) != 0 || !u.host) { url_free(&u); return lb_strdup("null"); }
   struct buf b = { 0 };
   buf_printf(&b, "%s://%s", u.scheme, u.host);
   if (u.port >= 0) buf_printf(&b, ":%d", u.port);
   url_free(&u);
   return buf_take(&b);
}

/* ======================================================================== *
 * Network: file:, data:, about:, http:, https:
 * ======================================================================== */

struct response {
   int status;
   char *url;              /* final URL after redirects */
   char *headers;          /* "Name: value\n" lines, lower-case names */
   unsigned char *body;
   size_t length;
   char *error;            /* set when no response was obtained */
};

static void response_free(struct response *r)
{
   free(r->url); free(r->headers); free(r->body); free(r->error);
   memset(r, 0, sizeof *r);
}

static void body_decode(struct response *r);
static char *header_get(const char *headers, const char *name)
{
   size_t n = strlen(name);
   for (const char *p = headers; p && *p; ) {
      const char *eol = strchr(p, '\n');
      if (!eol) eol = p + strlen(p);
      if ((size_t)(eol - p) > n && !strncasecmp(p, name, n) && p[n] == ':') {
         const char *v = p + n + 1;
         while (*v == ' ') v++;
         return lb_strndup(v, (size_t)(eol - v));
      }
      p = *eol ? eol + 1 : eol;
   }
   return NULL;
}

static char g_user_agent[512] =
   "Mozilla/5.0 (Linux; Android 13; Lunaria) AppleWebKit/537.36 "
   "(KHTML, like Gecko) " LB_PRODUCT " Mobile Safari/537.36";

/* Cookies: one jar per process, keyed by host (domain cookies match
 * subdomains).  Enough for session cookies a login page sets. */
struct cookie { char *domain, *path, *name, *value; int host_only, secure; };
static struct cookie *g_cookies;
static int g_ncookies;
/* Requests run on worker threads; the jar is shared with them. */
static pthread_mutex_t g_cookie_lock = PTHREAD_MUTEX_INITIALIZER;

static int domain_match(const char *host, const struct cookie *c)
{
   if (!strcasecmp(host, c->domain)) return 1;
   if (c->host_only) return 0;
   size_t h = strlen(host), d = strlen(c->domain);
   return h > d && !strcasecmp(host + h - d, c->domain) && host[h - d - 1] == '.';
}

static void cookie_set_locked(const char *url, const char *line, int from_script);
static void cookie_set(const char *url, const char *line, int from_script)
{
   pthread_mutex_lock(&g_cookie_lock);
   cookie_set_locked(url, line, from_script);
   pthread_mutex_unlock(&g_cookie_lock);
}

static void cookie_set_locked(const char *url, const char *line, int from_script)
{
   struct url u;
   if (url_parse(url, &u) != 0 || !u.host) { url_free(&u); return; }
   const char *semi = strchr(line, ';');
   size_t pair = semi ? (size_t)(semi - line) : strlen(line);
   const char *eq = memchr(line, '=', pair);
   if (!eq) { url_free(&u); return; }
   char *name = lb_strndup(line, (size_t)(eq - line));
   char *value = lb_strndup(eq + 1, pair - (size_t)(eq - line) - 1);
   char *domain = lb_strdup(u.host), *path = lb_strdup("/");
   int host_only = 1, secure = 0, expired = 0;
   for (const char *a = semi; a && *a; ) {
      a++;
      while (*a == ' ') a++;
      const char *e = strchr(a, ';');
      size_t len = e ? (size_t)(e - a) : strlen(a);
      if (!strncasecmp(a, "domain=", 7)) {
         const char *d = a + 7;
         if (*d == '.') d++;
         free(domain);
         domain = lb_strndup(d, len - (size_t)(d - a));
         host_only = 0;
      } else if (!strncasecmp(a, "path=", 5)) {
         free(path);
         path = lb_strndup(a + 5, len - 5);
      } else if (!strncasecmp(a, "secure", 6)) secure = 1;
      else if (!strncasecmp(a, "max-age=", 8)) expired = atol(a + 8) <= 0;
      else if (!strncasecmp(a, "httponly", 8) && from_script) { expired = 1; }
      a = e;
   }
   for (char *s = name; *s == ' '; ) memmove(s, s + 1, strlen(s));
   for (int i = 0; i < g_ncookies; ++i) {
      struct cookie *c = &g_cookies[i];
      if (!strcmp(c->name, name) && !strcasecmp(c->domain, domain) && !strcmp(c->path, path)) {
         free(c->name); free(c->value); free(c->domain); free(c->path);
         g_cookies[i] = g_cookies[--g_ncookies];
         break;
      }
   }
   if (!expired) {
      struct cookie *g = realloc(g_cookies, sizeof *g * (size_t)(g_ncookies + 1));
      if (g) {
         g_cookies = g;
         g_cookies[g_ncookies++] = (struct cookie){ domain, path, name, value, host_only, secure };
         url_free(&u);
         return;
      }
   }
   free(name); free(value); free(domain); free(path);
   url_free(&u);
}

static char *cookie_header(const char *url)
{
   pthread_mutex_lock(&g_cookie_lock);
   struct url u;
   if (url_parse(url, &u) != 0 || !u.host) {
      url_free(&u);
      pthread_mutex_unlock(&g_cookie_lock);
      return lb_strdup("");
   }
   struct buf b = { 0 };
   const char *path = u.path && u.path[0] ? u.path : "/";
   for (int i = 0; i < g_ncookies; ++i) {
      const struct cookie *c = &g_cookies[i];
      if (!domain_match(u.host, c) || strncmp(path, c->path, strlen(c->path))) continue;
      if (c->secure && strcmp(u.scheme, "https")) continue;
      if (b.len) buf_puts(&b, "; ");
      buf_printf(&b, "%s=%s", c->name, c->value);
   }
   url_free(&u);
   pthread_mutex_unlock(&g_cookie_lock);
   return buf_take(&b);
}

struct conn {
   int fd;
   struct conn *network_next;
   struct conn *idle_next;           /* the keep-alive pool */
   char key[300];                    /* scheme://host:port the connection is to */
   double idle_since;
#ifndef LB_NO_TLS
   SSL *ssl;
#endif
};

static _Atomic int g_network_online = 1;
static pthread_mutex_t g_network_lock = PTHREAD_MUTEX_INITIALIZER;
static struct conn *g_network_connections;

static void network_set_online(int online)
{
   pthread_mutex_lock(&g_network_lock);
   atomic_store(&g_network_online, online);
   if (!online)
      for (struct conn *c = g_network_connections; c; c = c->network_next)
         shutdown(c->fd, SHUT_RDWR);
   pthread_mutex_unlock(&g_network_lock);
}

static void conn_close_fd(struct conn *c)
{
   pthread_mutex_lock(&g_network_lock);
   struct conn **at = &g_network_connections;
   while (*at && *at != c) at = &(*at)->network_next;
   if (*at) *at = c->network_next;
   if (c->fd >= 0) close(c->fd);
   c->fd = -1;
   pthread_mutex_unlock(&g_network_lock);
}

#ifndef LB_NO_TLS
static SSL_CTX *g_ssl_ctx;

/* The last session per host: a new connection to a host seen before resumes
 * it and skips the full handshake. */
static struct { char host[256]; SSL_SESSION *sess; } g_tls_sessions[32];
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;

static int tls_new_session(SSL *ssl, SSL_SESSION *sess)
{
   const char *host = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
   if (!host || !*host) return 0;
   pthread_mutex_lock(&g_tls_lock);
   int slot = -1;
   static int next;
   for (int i = 0; i < 32; ++i) if (g_tls_sessions[i].sess && !strcmp(g_tls_sessions[i].host, host)) slot = i;
   if (slot < 0) slot = next++ & 31;
   if (g_tls_sessions[slot].sess) SSL_SESSION_free(g_tls_sessions[slot].sess);
   snprintf(g_tls_sessions[slot].host, sizeof g_tls_sessions[slot].host, "%s", host);
   g_tls_sessions[slot].sess = sess;
   pthread_mutex_unlock(&g_tls_lock);
   return 1;                                 /* the cache owns the session now */
}

static void tls_resume(SSL *ssl, const char *host)
{
   pthread_mutex_lock(&g_tls_lock);
   for (int i = 0; i < 32; ++i)
      if (g_tls_sessions[i].sess && !strcmp(g_tls_sessions[i].host, host)) {
         SSL_set_session(ssl, g_tls_sessions[i].sess);
         break;
      }
   pthread_mutex_unlock(&g_tls_lock);
}

static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

static void tls_context_init(void)
{
   g_ssl_ctx = SSL_CTX_new(TLS_client_method());
   if (!g_ssl_ctx) return;
   SSL_CTX_set_session_cache_mode(g_ssl_ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
   SSL_CTX_sess_set_new_cb(g_ssl_ctx, tls_new_session);
   SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
   SSL_CTX_set_default_verify_paths(g_ssl_ctx);
   SSL_CTX_set_verify(g_ssl_ctx, getenv("LUNA_BROWSER_INSECURE") ? SSL_VERIFY_NONE
                                                                 : SSL_VERIFY_PEER, NULL);
}

static SSL_CTX *tls_context(void)
{
   pthread_once(&g_tls_once, tls_context_init);
   return g_ssl_ctx;
}
#endif

static ptrdiff_t conn_write(struct conn *c, const void *p, size_t n)
{
   if (!atomic_load(&g_network_online)) { errno = ENETUNREACH; return -1; }
#ifndef LB_NO_TLS
   if (c->ssl) return SSL_write(c->ssl, p, (int)n);
#endif
   return send(c->fd, p, n, MSG_NOSIGNAL);
}

static ptrdiff_t conn_read(struct conn *c, void *p, size_t n)
{
   if (!atomic_load(&g_network_online)) { errno = ENETUNREACH; return -1; }
#ifndef LB_NO_TLS
   if (c->ssl) {
      int r = SSL_read(c->ssl, p, (int)n);
      return r > 0 ? r : 0;
   }
#endif
   ptrdiff_t r;
   do { r = recv(c->fd, p, n, 0); } while (r < 0 && errno == EINTR);
   return r;
}

static void conn_close(struct conn *c)
{
#ifndef LB_NO_TLS
   if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); c->ssl = NULL; }
#endif
   conn_close_fd(c);
}

/* Resolved addresses are kept for a minute: a page's dozens of requests to the
 * same few hosts must not each wait for the resolver. */
struct dns_addr { int family, proto; socklen_t len; struct sockaddr_storage addr; };
struct dns_entry { char host[256]; char port[16]; double when; int n; struct dns_addr a[8]; };
static struct dns_entry g_dns[32];
static pthread_mutex_t g_dns_lock = PTHREAD_MUTEX_INITIALIZER;

static int dns_resolve(const char *host, const char *port, struct dns_entry *out)
{
   double now = lb_now_ms();
   pthread_mutex_lock(&g_dns_lock);
   for (int i = 0; i < 32; ++i)
      if (g_dns[i].n && now - g_dns[i].when < 60000 && !strcmp(g_dns[i].host, host) && !strcmp(g_dns[i].port, port)) {
         *out = g_dns[i];
         pthread_mutex_unlock(&g_dns_lock);
         return 0;
      }
   pthread_mutex_unlock(&g_dns_lock);
   struct addrinfo hints = { 0 }, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   int rc = getaddrinfo(host, port, &hints, &res);
   if (rc != 0) return rc;
   memset(out, 0, sizeof *out);
   snprintf(out->host, sizeof out->host, "%s", host);
   snprintf(out->port, sizeof out->port, "%s", port);
   out->when = now;
   /* Interleave before imposing the cache limit. A resolver can return more
    * than eight IPv6 addresses ahead of its first IPv4 address (or vice versa). */
   struct addrinfo *v4 = res, *v6 = res;
   int family = res && res->ai_family == AF_INET6 ? AF_INET6 : AF_INET;
   while (out->n < 8) {
      while (v4 && (v4->ai_family != AF_INET || v4->ai_addrlen > sizeof out->a[0].addr)) v4 = v4->ai_next;
      while (v6 && (v6->ai_family != AF_INET6 || v6->ai_addrlen > sizeof out->a[0].addr)) v6 = v6->ai_next;
      struct addrinfo *a = family == AF_INET6 ? (v6 ? v6 : v4) : (v4 ? v4 : v6);
      if (!a) break;
      if (a == v4) v4 = v4->ai_next;
      else v6 = v6->ai_next;
      struct dns_addr *d = &out->a[out->n++];
      d->family = a->ai_family; d->proto = a->ai_protocol; d->len = a->ai_addrlen;
      memcpy(&d->addr, a->ai_addr, a->ai_addrlen);
      family = a->ai_family == AF_INET6 ? AF_INET : AF_INET6;
   }
   freeaddrinfo(res);
   if (!out->n) return EAI_NONAME;
   pthread_mutex_lock(&g_dns_lock);
   static int next;
   g_dns[next++ & 31] = *out;
   pthread_mutex_unlock(&g_dns_lock);
   return 0;
}

static void dns_forget(const char *host, const char *port)
{
   pthread_mutex_lock(&g_dns_lock);
   for (int i = 0; i < 32; ++i)
      if (g_dns[i].n && !strcmp(g_dns[i].host, host) && !strcmp(g_dns[i].port, port)) g_dns[i].n = 0;
   pthread_mutex_unlock(&g_dns_lock);
}

static int conn_open(struct conn *c, const struct url *u, char **error)
{
   c->fd = -1;
   c->network_next = NULL;
#ifndef LB_NO_TLS
   c->ssl = NULL;
#endif
   if (!atomic_load(&g_network_online)) {
      *error = lb_strdup("Network is offline"); errno = ENETUNREACH; return -1;
   }
   int tls = !strcmp(u->scheme, "https");
   char port[16];
   snprintf(port, sizeof port, "%d", u->port >= 0 ? u->port : tls ? 443 : 80);
   char host[256];
   snprintf(host, sizeof host, "%s", u->host);
   if (host[0] == '[') { memmove(host, host + 1, strlen(host)); host[strcspn(host, "]")] = 0; }
   struct dns_entry addrs;
   int rc = dns_resolve(host, port, &addrs);
   if (rc != 0) { *error = lb_strdup(gai_strerror(rc)); return -1; }
   /* Stagger nonblocking attempts, alternating families. A blackholed IPv6
    * route must not hold IPv4 (or another address) behind a connect timeout. */
   struct conn attempts[8] = { 0 };
   struct pollfd pf[8];
   int order[8], used[8] = { 0 }, count = 0;
   int family = addrs.a[0].family;
   for (int n = 0; n < addrs.n; ++n) {
      int pick = -1;
      for (int i = 0; i < addrs.n; ++i)
         if (!used[i] && addrs.a[i].family == family) { pick = i; break; }
      if (pick < 0) for (int i = 0; i < addrs.n; ++i)
         if (!used[i]) { pick = i; break; }
      order[n] = pick; used[pick] = 1;
      family = addrs.a[pick].family == AF_INET6 ? AF_INET : AF_INET6;
      attempts[n].fd = pf[n].fd = -1;
      pf[n].events = POLLOUT;
   }
   double deadline = lb_now_ms() + 4000, launch = 0;
   int winner = -1;
   while (lb_now_ms() < deadline && atomic_load(&g_network_online)) {
      double now = lb_now_ms();
      int active = 0;
      for (int i = 0; i < count; ++i) active += pf[i].fd >= 0;
      if (count < addrs.n && (!active || now >= launch)) {
         int i = count++;
         const struct dns_addr *a = &addrs.a[order[i]];
         struct conn *candidate = &attempts[i];
         candidate->fd = socket(a->family, SOCK_STREAM, a->proto);
         if (candidate->fd < 0) continue;
         fcntl(candidate->fd, F_SETFD, FD_CLOEXEC);
         fcntl(candidate->fd, F_SETFL, O_NONBLOCK);
         pthread_mutex_lock(&g_network_lock);
         candidate->network_next = g_network_connections;
         g_network_connections = candidate;
         if (!atomic_load(&g_network_online)) shutdown(candidate->fd, SHUT_RDWR);
         pthread_mutex_unlock(&g_network_lock);
         int result = connect(candidate->fd, (const struct sockaddr *)&a->addr, a->len);
         if (result == 0) { winner = i; break; }
         if (errno != EINPROGRESS) { conn_close_fd(candidate); continue; }
         pf[i].fd = candidate->fd;
         launch = now + 250;
      }
      active = 0;
      for (int i = 0; i < count; ++i) active += pf[i].fd >= 0;
      if (!active && count == addrs.n) break;
      now = lb_now_ms();
      double until = count < addrs.n && launch < deadline ? launch : deadline;
      int wait = until > now ? (int)ceil(until - now) : 0;
      int result = poll(pf, (nfds_t)count, wait);
      if (result < 0 && errno != EINTR) break;
      for (int i = 0; result > 0 && i < count; ++i) {
         if (pf[i].fd < 0 || !pf[i].revents) continue;
         int err = 0; socklen_t len = sizeof err;
         if (getsockopt(pf[i].fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && !err) {
            winner = i; break;
         }
         conn_close_fd(&attempts[i]); pf[i].fd = -1;
      }
      if (winner >= 0) break;
   }
   if (winner >= 0) {
      pthread_mutex_lock(&g_network_lock);
      struct conn **at = &g_network_connections;
      while (*at && *at != &attempts[winner]) at = &(*at)->network_next;
      c->fd = attempts[winner].fd;
      c->network_next = attempts[winner].network_next;
      if (*at) *at = c;
      attempts[winner].fd = -1;
      pthread_mutex_unlock(&g_network_lock);
      fcntl(c->fd, F_SETFL, 0);
      struct timeval tv = { 30, 0 };
      setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      int one = 1;
      setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
   }
   for (int i = 0; i < count; ++i)
      if (attempts[i].fd >= 0) conn_close_fd(&attempts[i]);
   if (winner < 0) dns_forget(host, port);
   if (c->fd < 0) { *error = lb_strdup("connection failed"); return -1; }
   if (!tls) return 0;
#ifdef LB_NO_TLS
   *error = lb_strdup("https is not built in (LB_NO_TLS)");
   conn_close(c);
   return -1;
#else
   SSL_CTX *ctx = tls_context();
   c->ssl = ctx ? SSL_new(ctx) : NULL;
   if (!c->ssl) { *error = lb_strdup("TLS unavailable"); conn_close(c); return -1; }
   SSL_set_fd(c->ssl, c->fd);
   unsigned char ip[16];
   int numeric = inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1;
   if (!numeric) SSL_set_tlsext_host_name(c->ssl, host);
   tls_resume(c->ssl, host);
   if (!getenv("LUNA_BROWSER_INSECURE")) {
      if (numeric) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c->ssl), host);
      else SSL_set1_host(c->ssl, host);
   }
   if (SSL_connect(c->ssl) != 1) {
      char msg[256];
      ERR_error_string_n(ERR_get_error(), msg, sizeof msg);
      *error = lb_strdup(msg);
      conn_close(c);
      return -1;
   }
   return 0;
#endif
}

/* Set while the page's own document is being fetched: a navigation carries the
 * headers a browser sends for one (some sites tell scripted clients apart by them). */
static __thread int t_fetch_navigation;

/* Idle keep-alive connections, per scheme://host:port.  A connection is taken
 * out of the pool while a request uses it and put back if the response left it
 * clean; one the server closed meanwhile is noticed on use and retried. */
#define POOL_MAX 24
#define POOL_IDLE_MS 20000
static struct conn *g_idle;
static int g_idle_n;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static void conn_destroy(struct conn *c)
{
   conn_close(c);
   free(c);
}

static struct conn *pool_take(const char *key)
{
   double now = lb_now_ms();
   struct conn *got = NULL, *stale = NULL;
   pthread_mutex_lock(&g_pool_lock);
   for (struct conn **at = &g_idle; *at; ) {
      struct conn *c = *at;
      if (now - c->idle_since > POOL_IDLE_MS) { *at = c->idle_next; g_idle_n--; c->idle_next = stale; stale = c; continue; }
      if (!got && !strcmp(c->key, key)) { *at = c->idle_next; g_idle_n--; got = c; continue; }
      at = &c->idle_next;
   }
   pthread_mutex_unlock(&g_pool_lock);
   while (stale) { struct conn *n = stale->idle_next; conn_destroy(stale); stale = n; }
   if (got) got->idle_next = NULL;
   return got;
}

static void pool_put(struct conn *c)
{
   struct conn *drop = NULL;
   c->idle_since = lb_now_ms();
   pthread_mutex_lock(&g_pool_lock);
   c->idle_next = g_idle; g_idle = c; g_idle_n++;
   if (g_idle_n > POOL_MAX) {                       /* the oldest goes */
      struct conn **at = &g_idle;
      while ((*at)->idle_next) at = &(*at)->idle_next;
      drop = *at; *at = NULL; g_idle_n--;
   }
   pthread_mutex_unlock(&g_pool_lock);
   if (drop) conn_destroy(drop);
}

/* One request/response exchange, no redirects. */
static int http_exchange(const char *url, const char *method, const char *extra_headers,
                         const unsigned char *body, size_t body_len, struct response *r)
{
   struct url u;
   if (url_parse(url, &u) != 0 || !u.host) { url_free(&u); r->error = lb_strdup("bad URL"); return -1; }
   char key[300];
   snprintf(key, sizeof key, "%s://%s:%d", u.scheme, u.host, u.port >= 0 ? u.port : !strcmp(u.scheme, "https") ? 443 : 80);
   int reused = 0;
   struct conn *c = pool_take(key);
   if (c) reused = 1;
retry:
   if (!c) {
      c = calloc(1, sizeof *c);
      if (!c || conn_open(c, &u, &r->error) != 0) { free(c); url_free(&u); return -1; }
      snprintf(c->key, sizeof c->key, "%s", key);
   }
   char *cookies = cookie_header(url);
   struct buf req = { 0 };
   buf_printf(&req, "%s %s%s%s HTTP/1.1\r\nHost: %s", method, u.path && u.path[0] ? u.path : "/",
              u.query ? "?" : "", u.query ? u.query : "", u.host);
   if (u.port >= 0) buf_printf(&req, ":%d", u.port);
   buf_printf(&req, "\r\nUser-Agent: %s\r\nAccept-Encoding: %s\r\n"
              "Connection: keep-alive\r\n", g_user_agent,
#ifndef LB_NO_ZLIB
              "gzip, deflate"
#else
              "identity"
#endif
              );
   if (!extra_headers || !strcasestr(extra_headers, "accept:"))
      buf_puts(&req, t_fetch_navigation
                     ? "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8\r\n"
                       "Upgrade-Insecure-Requests: 1\r\nSec-Fetch-Dest: document\r\nSec-Fetch-Mode: navigate\r\n"
                       "Sec-Fetch-Site: none\r\nSec-Fetch-User: ?1\r\n"
                     : "Accept: */*\r\n");
   if (!extra_headers || !strcasestr(extra_headers, "accept-language:"))
      buf_puts(&req, "Accept-Language: ja-JP,ja;q=0.9,en-US;q=0.8,en;q=0.7\r\n");
   if (cookies[0]) buf_printf(&req, "Cookie: %s\r\n", cookies);
   free(cookies);
   if (extra_headers) buf_puts(&req, extra_headers);
   if (body || strcmp(method, "GET")) buf_printf(&req, "Content-Length: %zu\r\n", body_len);
   buf_puts(&req, "\r\n");
   if (body && body_len) buf_put(&req, body, body_len);
   ptrdiff_t sent = 0;
   while ((size_t)sent < req.len) {
      ptrdiff_t n = conn_write(c, req.p + sent, req.len - (size_t)sent);
      if (n <= 0) break;
      sent += n;
   }
   free(req.p);
   if ((size_t)sent < req.len) {
      conn_destroy(c); c = NULL;
      if (reused) { reused = 0; goto retry; }       /* the server had closed it */
      url_free(&u);
      r->error = lb_strdup("HTTP request send failed");
      return -1;
   }
   struct buf in = { 0 };
   struct http_frame frame = { 0 };
   int framing = 0, received_eof = 0;
   char chunk[16384];
   for (;;) {
      ptrdiff_t n = conn_read(c, chunk, sizeof chunk);
      if (n <= 0) { received_eof = n == 0; break; }
      buf_put(&in, chunk, (size_t)n);
      framing = http_frame_feed(&frame, in.p, in.len, method);
      if (framing) break;
   }
   if (!in.len && reused) {                         /* closed before it answered: try a fresh one */
      conn_destroy(c); c = NULL; reused = 0;
      free(in.p);
      goto retry;
   }
   /* Kept for the next request when the response ended by its own framing
    * and neither side asked to close. */
   int keep = framing > 0 && frame.complete && !frame.close_delimited && in.len >= 12 &&
              !strncmp(in.p, "HTTP/1.1", 8);
   if (keep) {
      const char *he = memmem(in.p, in.len, "\r\n\r\n", 4);
      for (const char *l = in.p; he && l < he && keep; l = strstr(l, "\r\n") + 2) {
         if (!strncasecmp(l, "connection:", 11)) {
            const char *eol = strstr(l, "\r\n");
            for (const char *q = l + 11; q + 5 <= eol; ++q) if (!strncasecmp(q, "close", 5)) { keep = 0; break; }
         }
         if (!strstr(l, "\r\n")) break;
      }
   }
   if (keep && atomic_load(&g_network_online)) pool_put(c); else conn_destroy(c);
   c = NULL;
   url_free(&u);
   if (framing < 0 || (!frame.complete &&
       !(received_eof && frame.headers && frame.close_delimited))) {
      free(in.p);
      r->error = lb_strdup(framing < 0 ? "invalid HTTP response framing" : "incomplete HTTP response");
      return -1;
   }
   /* Informational responses precede the final response, without a body. */
   if (frame.start) {
      in.len -= frame.start;
      memmove(in.p, in.p + frame.start, in.len);
      in.p[in.len] = 0;
   }
   char *head_end = in.p ? strstr(in.p, "\r\n\r\n") : NULL;
   if (!head_end) { free(in.p); r->error = lb_strdup("no HTTP response"); return -1; }
   r->status = atoi(in.p + strcspn(in.p, " "));
   struct buf h = { 0 };
   for (char *line = strstr(in.p, "\r\n") + 2; line < head_end; ) {
      char *eol = strstr(line, "\r\n");
      char *colon = memchr(line, ':', (size_t)(eol - line));
      if (colon) {
         for (char *k = line; k < colon; ++k) *k = (char)tolower((unsigned char)*k);
         buf_put(&h, line, (size_t)(eol - line));
         buf_puts(&h, "\n");
         if (!strncmp(line, "set-cookie:", 11)) {
            char *v = lb_strndup(colon + 1, (size_t)(eol - colon - 1));
            char *vv = v;
            while (*vv == ' ') vv++;
            cookie_set(url, vv, 0);
            free(v);
         }
      }
      line = eol + 2;
   }
   r->headers = buf_take(&h);
   const unsigned char *b = (unsigned char *)head_end + 4;
   size_t blen = in.len - (size_t)((char *)b - in.p);
   if (frame.no_body) blen = 0;
   char *te = header_get(r->headers, "transfer-encoding");
   if (te && strcasestr(te, "chunked")) {
      struct buf out = { 0 };
      size_t i = 0;
      while (i < blen) {
         size_t size = strtoul((const char *)b + i, NULL, 16);
         const unsigned char *crlf = memmem(b + i, blen - i, "\r\n", 2);
         if (!crlf || size == 0) break;
         i = (size_t)(crlf - b) + 2;
         if (i + size > blen) size = blen - i;
         buf_put(&out, b + i, size);
         i += size + 2;
      }
      r->length = out.len;
      r->body = (unsigned char *)buf_take(&out);
   } else {
      char *cl = header_get(r->headers, "content-length");
      if (cl && (size_t)atol(cl) < blen) blen = (size_t)atol(cl);
      free(cl);
      r->body = malloc(blen + 1);
      if (r->body) { memcpy(r->body, b, blen); r->body[blen] = 0; r->length = blen; }
   }
   free(te);
   free(in.p);
   body_decode(r);
   return 0;
}

#ifndef LB_NO_ZLIB
/* Servers may gzip/deflate a body even when asked for identity. */
static void body_decode(struct response *r)
{
   char *ce = header_get(r->headers, "content-encoding");
   int enc = ce && (strcasestr(ce, "gzip") || strcasestr(ce, "deflate"));
   free(ce);
   if (!enc || !r->body || !r->length) return;
   z_stream z;
   memset(&z, 0, sizeof z);
   if (inflateInit2(&z, 15 + 32) != Z_OK) return;
   size_t cap = r->length * 4 + 1024, len = 0;
   unsigned char *out = malloc(cap + 1);
   int rc = Z_OK;
   z.next_in = r->body; z.avail_in = (uInt)r->length;
   while (out && rc == Z_OK) {
      if (len == cap) {
         if (cap > ((size_t)1 << 30)) { rc = Z_MEM_ERROR; break; }
         cap *= 2;
         unsigned char *n = realloc(out, cap + 1);
         if (!n) { rc = Z_MEM_ERROR; break; }
         out = n;
      }
      z.next_out = out + len; z.avail_out = (uInt)(cap - len);
      rc = inflate(&z, Z_NO_FLUSH);
      len = cap - z.avail_out;
   }
   inflateEnd(&z);
   if (!out || (rc != Z_STREAM_END && !len)) { free(out); return; }
   out[len] = 0;
   free(r->body);
   r->body = out; r->length = len;
}
#else
static void body_decode(struct response *r) { (void)r; }
#endif

static int is_font_path(const char *p)
{
   static const char *dirs[] = { "/usr/share/fonts", "/usr/local/share/fonts",
                                 "/System/Library/Fonts", "/Library/Fonts", NULL };
   for (int i = 0; dirs[i]; ++i) if (!strncmp(p, dirs[i], strlen(dirs[i]))) return 1;
   return strstr(p, "/.fonts/") || strstr(p, "/.local/share/fonts/") || strstr(p, "/fonts/");
}

static const char *mime_of_path(const char *path)
{
   const char *dot = strrchr(path, '.');
   if (!dot) return "application/octet-stream";
   static const char *map[][2] = {
      { ".html", "text/html" }, { ".htm", "text/html" }, { ".css", "text/css" },
      { ".js", "text/javascript" }, { ".mjs", "text/javascript" },
      { ".json", "application/json" }, { ".png", "image/png" }, { ".jpg", "image/jpeg" },
      { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".svg", "image/svg+xml" },
      { ".webp", "image/webp" }, { ".txt", "text/plain" }, { NULL, NULL } };
   for (int i = 0; map[i][0]; ++i) if (!strcasecmp(dot, map[i][0])) return map[i][1];
   return "application/octet-stream";
}

/* Reuse explicitly fresh public subresources across document lifetimes.
 * Documents and requests with custom headers bypass this cache. Cookie and UA
 * are part of the key; redirects, Set-Cookie and arbitrary Vary are excluded. */
#define HTTP_CACHE_SLOTS 128
#define HTTP_CACHE_BYTES (32u * 1024u * 1024u)
struct http_cached {
   char *key;
   double expires, used;
   struct response r;
};
static struct http_cached g_http_cache[HTTP_CACHE_SLOTS];
static size_t g_http_cache_bytes;
static pthread_mutex_t g_http_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static int response_copy(struct response *to, const struct response *from)
{
   memset(to, 0, sizeof *to);
   to->status = from->status;
   to->url = lb_strdup(from->url);
   to->headers = lb_strdup(from->headers);
   to->body = (unsigned char *)lb_strndup((const char *)from->body, from->length);
   to->length = from->length;
   if (to->url && to->headers && to->body) return 1;
   response_free(to);
   return 0;
}

static char *http_cache_key(const char *url)
{
   char *cookies = cookie_header(url);
   struct buf key = { 0 };
   buf_printf(&key, "%s\n%s\n%s", url, g_user_agent, cookies ? cookies : "");
   free(cookies);
   return buf_take(&key);
}

static void http_cache_drop(int i)
{
   g_http_cache_bytes -= g_http_cache[i].r.length;
   free(g_http_cache[i].key);
   response_free(&g_http_cache[i].r);
   memset(&g_http_cache[i], 0, sizeof g_http_cache[i]);
}

static int http_cache_get(const char *key, struct response *r)
{
   int found = 0;
   double now = lb_now_ms();
   pthread_mutex_lock(&g_http_cache_lock);
   for (int i = 0; i < HTTP_CACHE_SLOTS; ++i) {
      if (!g_http_cache[i].key) continue;
      if (g_http_cache[i].expires <= now) { http_cache_drop(i); continue; }
      if (!strcmp(g_http_cache[i].key, key)) {
         found = response_copy(r, &g_http_cache[i].r);
         g_http_cache[i].used = now;
         break;
      }
   }
   pthread_mutex_unlock(&g_http_cache_lock);
   return found;
}

static void http_cache_put(const char *key, const char *url, const struct response *r)
{
   if (r->status != 200 || !r->body || r->length > HTTP_CACHE_BYTES / 4 ||
       !r->url || strcmp(url, r->url)) return;
   char *cc = header_get(r->headers, "cache-control");
   char *vary = header_get(r->headers, "vary");
   char *cookie = header_get(r->headers, "set-cookie");
   char *age = header_get(r->headers, "age");
   const char *ma = cc ? strcasestr(cc, "max-age=") : NULL;
   double ttl = ma ? strtod(ma + 8, NULL) : 0;
   if (age) ttl -= strtod(age, NULL);
   int usable = cc && strcasestr(cc, "public") && !strcasestr(cc, "no-store") &&
      !strcasestr(cc, "no-cache") && !strcasestr(cc, "private") && !cookie &&
      (!vary || !strcasecmp(vary, "accept-encoding")) && ttl > 0;
   free(cc); free(vary); free(cookie); free(age);
   if (!usable) return;
   struct response copy;
   if (!response_copy(&copy, r)) return;
   char *saved_key = lb_strdup(key);
   if (!saved_key) { response_free(&copy); return; }
   pthread_mutex_lock(&g_http_cache_lock);
   for (int i = 0; i < HTTP_CACHE_SLOTS; ++i)
      if (g_http_cache[i].key && !strcmp(g_http_cache[i].key, key)) http_cache_drop(i);
   int slot;
   for (;;) {
      slot = -1;
      int oldest = -1;
      for (int i = 0; i < HTTP_CACHE_SLOTS; ++i) {
         if (!g_http_cache[i].key) { slot = i; continue; }
         if (oldest < 0 || g_http_cache[i].used < g_http_cache[oldest].used) oldest = i;
      }
      if (slot >= 0 && g_http_cache_bytes + r->length <= HTTP_CACHE_BYTES) break;
      if (oldest < 0) break;
      http_cache_drop(oldest);
   }
   g_http_cache[slot] = (struct http_cached){ saved_key, lb_now_ms() + fmin(ttl, 86400) * 1000, lb_now_ms(), copy };
   g_http_cache_bytes += copy.length;
   pthread_mutex_unlock(&g_http_cache_lock);
}

static int lb_fetch_hops(const char *url, const char *method, const char *headers,
                         const unsigned char *body, size_t body_len, struct response *r,
                         char **frag);

/* GET or any method; follows redirects.  Always fills r (r->error on
 * failure).  r->url keeps the fragment: the request's, or the one a
 * redirect's Location gave (fragments never go on the wire). */
static int lb_fetch(const char *url, const char *method, const char *headers,
                    const unsigned char *body, size_t body_len, struct response *r)
{
   int cacheable = !t_fetch_navigation && (!method || !strcmp(method, "GET")) &&
      (!headers || !*headers) && !body_len && !strchr(url, '#') &&
      (!strncmp(url, "https://", 8) || !strncmp(url, "http://", 7));
   char *key = cacheable ? http_cache_key(url) : NULL;
   if (key && atomic_load(&g_network_online) && http_cache_get(key, r)) {
      LB_LOG("HTTP cache %s\n", url);
      free(key);
      return 0;
   }
   const char *hash = strchr(url, '#');
   char *frag = hash ? lb_strdup(hash) : NULL;
   int rc = lb_fetch_hops(url, method, headers, body, body_len, r, &frag);
   if (frag && r->url) {
      struct buf b = { 0 };
      buf_puts(&b, r->url);
      buf_puts(&b, frag);
      free(r->url);
      r->url = buf_take(&b);
   }
   free(frag);
   if (rc == 0 && key) http_cache_put(key, url, r);
   free(key);
   return rc;
}

static int lb_fetch_hops(const char *url, const char *method, const char *headers,
                         const unsigned char *body, size_t body_len, struct response *r,
                         char **frag)
{
   memset(r, 0, sizeof *r);
   char *cur = lb_strdup(url);
   char *hash = strchr(cur, '#');
   if (hash) *hash = '\0';
   if (!method) method = "GET";
   for (int hop = 0; hop < 10; ++hop) {
      struct url u;
      if (url_parse(cur, &u) != 0) {
         r->error = lb_strdup("unsupported URL");
         r->url = cur;
         return -1;
      }
      if (!strcmp(u.scheme, "about")) {
         r->status = 200;
         r->headers = lb_strdup("content-type: text/html\n");
         r->body = (unsigned char *)lb_strdup("");
         url_free(&u);
         r->url = cur;
         return 0;
      }
      if (!strcmp(u.scheme, "data")) {
         const char *comma = strchr(cur + 5, ',');
         if (!comma) { url_free(&u); r->error = lb_strdup("bad data URL"); r->url = cur; return -1; }
         char *meta = lb_strndup(cur + 5, (size_t)(comma - cur - 5));
         int b64 = strstr(meta, ";base64") != NULL;
         char *semi = strchr(meta, ';');
         if (semi) *semi = '\0';
         struct buf h = { 0 };
         buf_printf(&h, "content-type: %s\n", meta[0] ? meta : "text/plain");
         r->headers = buf_take(&h);
         char *dec = percent_decode(comma + 1, strlen(comma + 1));
         if (b64) r->body = base64_decode(dec, strlen(dec), &r->length), free(dec);
         else { r->body = (unsigned char *)dec; r->length = strlen(dec); }
         free(meta);
         r->status = 200;
         url_free(&u);
         r->url = cur;
         return 0;
      }
      if (!strcmp(u.scheme, "file")) {
         char *path = percent_decode(u.path, strlen(u.path));
         FILE *f = fopen(path, "rb");
         if (f) {
            struct buf b = { 0 };
            char tmp[65536];
            size_t n;
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_put(&b, tmp, n);
            fclose(f);
            r->status = 200;
            r->length = b.len;
            r->body = (unsigned char *)buf_take(&b);
            struct buf h = { 0 };
            buf_printf(&h, "content-type: %s\n", mime_of_path(path));
            r->headers = buf_take(&h);
         } else {
            r->status = 404;
            r->error = lb_strdup(strerror(errno));
         }
         free(path);
         url_free(&u);
         r->url = cur;
         return r->error ? -1 : 0;
      }
      if (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")) {
         url_free(&u);
         r->error = lb_strdup("unsupported scheme");
         r->url = cur;
         return -1;
      }
      url_free(&u);
      if (http_exchange(cur, method, headers, body, body_len, r) != 0) { r->url = cur; return -1; }
      if (r->status >= 300 && r->status < 400 && r->status != 304) {
         char *loc = header_get(r->headers, "location");
         if (loc) {
            char *next = url_resolve(cur, loc);
            free(loc);
            free(cur);
            cur = next;
            char *h = strchr(cur, '#');
            if (h) { free(*frag); *frag = lb_strdup(h); *h = '\0'; }
            if (r->status != 307 && r->status != 308) { method = "GET"; body = NULL; body_len = 0; }
            free(r->headers); free(r->body);
            r->headers = NULL; r->body = NULL; r->length = 0;
            continue;
         }
      }
      r->url = cur;
      return 0;
   }
   r->error = lb_strdup("too many redirects");
   r->url = cur;
   return -1;
}

/* ======================================================================== *
 * Headless OpenGL: an EGL pbuffer is the page's framebuffer 0
 * ======================================================================== */

typedef void (*gl_viewport_fn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*gl_clear_color_fn)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*gl_clear_fn)(GLbitfield);
typedef void (*gl_read_pixels_fn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (*gl_finish_fn)(void);
typedef const GLubyte *(*gl_get_string_fn)(GLenum);
typedef void (*gl_pixel_storei_fn)(GLenum, GLint);

static struct {
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLContext ctx;
   EGLSurface surf;
   int w, h, es;
   gl_viewport_fn viewport;
   gl_clear_color_fn clear_color;
   gl_clear_fn clear;
   gl_read_pixels_fn read_pixels;
   gl_finish_fn finish;
   gl_pixel_storei_fn pixel_storei;
} g_gl;

/* eglGetProcAddress may legally return NULL for core entry points; the GL
 * library the context came from has them. */
static void *gl_proc(const char *name)
{
   void *p = (void *)eglGetProcAddress(name);
   if (p) return p;
   static void *libs[3];
   static int opened;
   if (!opened) {
      opened = 1;
      libs[0] = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
      libs[1] = dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_GLOBAL);
      libs[2] = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_GLOBAL);
   }
   for (int i = 0; i < 3; ++i) if (libs[i] && (p = dlsym(libs[i], name))) return p;
   return dlsym(RTLD_DEFAULT, name);
}

static int gl_surface(int w, int h)
{
   if (g_gl.surf != EGL_NO_SURFACE && g_gl.w == w && g_gl.h == h) return 0;
   const EGLint attrs[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
   EGLSurface s = eglCreatePbufferSurface(g_gl.dpy, g_gl.cfg, attrs);
   if (s == EGL_NO_SURFACE) return -1;
   if (!eglMakeCurrent(g_gl.dpy, s, s, g_gl.ctx)) { eglDestroySurface(g_gl.dpy, s); return -1; }
   if (g_gl.surf != EGL_NO_SURFACE) eglDestroySurface(g_gl.dpy, g_gl.surf);
   g_gl.surf = s;
   g_gl.w = w;
   g_gl.h = h;
   return 0;
}

static int gl_open(int w, int h)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
   g_gl.dpy = EGL_NO_DISPLAY;
   g_gl.surf = EGL_NO_SURFACE;
   /* Surfaceless first: it needs no display server, which is what a
    * pipe-driven browser under an emulator or CI has. */
   if (get_platform_display && client && strstr(client, "EGL_MESA_platform_surfaceless"))
      g_gl.dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint major, minor;
   if (g_gl.dpy == EGL_NO_DISPLAY || !eglInitialize(g_gl.dpy, &major, &minor)) {
      g_gl.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
      if (g_gl.dpy == EGL_NO_DISPLAY || !eglInitialize(g_gl.dpy, &major, &minor)) {
         fprintf(stderr, "luna-browser: no EGL display\n");
         return -1;
      }
   }
   for (int es = 0; es < 2 && !g_gl.ctx; ++es) {
      if (!eglBindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API)) continue;
      const EGLint cattrs[] = {
         EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
         EGL_RENDERABLE_TYPE, es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
         EGL_NONE };
      EGLint n = 0;
      if (!eglChooseConfig(g_gl.dpy, cattrs, &g_gl.cfg, 1, &n) || n < 1) continue;
      const EGLint gl_attrs[] = {
         EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
         EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
      const EGLint es_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
      g_gl.ctx = eglCreateContext(g_gl.dpy, g_gl.cfg, EGL_NO_CONTEXT, es ? es_attrs : gl_attrs);
      if (g_gl.ctx == EGL_NO_CONTEXT) g_gl.ctx = NULL;
      else g_gl.es = es;
   }
   if (!g_gl.ctx) { fprintf(stderr, "luna-browser: no GL 3.3 / GLES 3 context\n"); return -1; }
   if (gl_surface(w, h) != 0) { fprintf(stderr, "luna-browser: pbuffer failed\n"); return -1; }
   g_gl.viewport = (gl_viewport_fn)gl_proc("glViewport");
   g_gl.clear_color = (gl_clear_color_fn)gl_proc("glClearColor");
   g_gl.clear = (gl_clear_fn)gl_proc("glClear");
   g_gl.read_pixels = (gl_read_pixels_fn)gl_proc("glReadPixels");
   g_gl.finish = (gl_finish_fn)gl_proc("glFinish");
   g_gl.pixel_storei = (gl_pixel_storei_fn)gl_proc("glPixelStorei");
   gl_get_string_fn get_string = (gl_get_string_fn)gl_proc("glGetString");
   LB_LOG("GL %s (%s)\n", get_string ? (const char *)get_string(0x1F02 /* GL_VERSION */) : "?",
          g_gl.es ? "ES" : "desktop");
   return g_gl.viewport && g_gl.read_pixels && g_gl.clear ? 0 : -1;
}

/* ======================================================================== *
 * Page state
 * ======================================================================== */

struct lb_event {        /* pointer events recorded inside luna's input path */
   int kind;             /* 1 press, 2 release */
   int hit, button, drag_moved;
};

static struct {
   JSRuntime *rt;
   JSContext *ctx;
   char *url;
   int w, h;                    /* viewport, CSS pixels */
   float scale;                 /* device pixels per CSS pixel */
   int ready;                   /* 0 loading, 1 interactive, 2 complete */
   double last_frame;
   char *nav_url;               /* a navigation requested by the page */
   char *profile;               /* --user-data-dir */
   char **boot_scripts;         /* Page.addScriptToEvaluateOnNewDocument */
   int nboot;
   char **bindings;             /* Runtime.addBinding */
   int nbindings;
   struct lb_event ev[16];
   int nev;
   unsigned press_uid;
   double press_x, press_y;
   int pressed;
   int dirty;
   /* Resources fetched for the renderer, by absolute URL. */
   struct { char *url; unsigned char *data; size_t len; } *cache;
   int ncache;
   /* Events for the embedder (CDP), JSON objects without the envelope. */
   void (*emit)(const char *method, const char *params_json);
} g_page;

static void page_emit(const char *method, const char *params)
{
   if (g_page.emit) g_page.emit(method, params ? params : "{}");
}

static int cache_find(const char *abs)
{
   for (int i = 0; i < g_page.ncache; ++i)
      if (!strcmp(g_page.cache[i].url, abs)) return i;
   return -1;
}

/* Takes ownership of url and body. */
static void cache_add(char *url, unsigned char *body, size_t len)
{
   void *grown = realloc(g_page.cache, sizeof *g_page.cache * (size_t)(g_page.ncache + 1));
   if (!grown) { free(url); free(body); return; }
   g_page.cache = grown;
   g_page.cache[g_page.ncache].url = url;
   g_page.cache[g_page.ncache].data = body;
   g_page.cache[g_page.ncache].len = len;
   g_page.ncache++;
}

/* Subresources named by the markup are fetched in parallel before parsing, so
 * a page costs the slowest request rather than the sum of all of them.
 * lb_read_resource and script loading then find them in the cache. */
#define LB_PF_MAX  256
#define PF_THREADS 12
struct pf_job { char *url; struct response r; };
struct pf_pool { struct pf_job *jobs; int n, next; };

static void *pf_worker(void *arg)
{
   struct pf_pool *pool = arg;
   for (;;) {
      int i = __atomic_fetch_add(&pool->next, 1, __ATOMIC_RELAXED);
      if (i >= pool->n) return NULL;
      lb_fetch(pool->jobs[i].url, "GET", NULL, NULL, 0, &pool->jobs[i].r);
   }
}

static int tag_attr(const char *tag, const char *end, const char *name, char *out, size_t outsz)
{
   size_t nl = strlen(name);
   for (const char *p = tag; p + nl < end; ++p) {
      if (p == tag || !isspace((unsigned char)p[-1])) continue;
      if (strncasecmp(p, name, nl)) continue;
      const char *a = p + nl;
      while (a < end && isspace((unsigned char)*a)) a++;
      if (a >= end || *a != '=') continue;
      a++;
      while (a < end && isspace((unsigned char)*a)) a++;
      char q = (*a == '"' || *a == '\'') ? *a++ : 0;
      size_t k = 0;
      while (a < end && (q ? *a != q : (!isspace((unsigned char)*a) && *a != '>')) && k + 1 < outsz) out[k++] = *a++;
      out[k] = 0;
      return 1;
   }
   return 0;
}

static void page_prefetch(const char *html)
{
   if (!strncmp(g_page.url, "file:", 5) || !strncmp(g_page.url, "about:", 6)) return;
   struct pf_pool pool = { calloc(LB_PF_MAX, sizeof(struct pf_job)), 0, 0 };
   if (!pool.jobs) return;
   for (const char *p = html; (p = strchr(p, '<')) != NULL && pool.n < LB_PF_MAX; ) {
      if (!strncmp(p, "<!--", 4)) { const char *e = strstr(p, "-->"); if (!e) break; p = e + 3; continue; }
      int kind = !strncasecmp(p, "<link", 5) ? 1 : !strncasecmp(p, "<script", 7) ? 2 :
                 0;
      if (!kind) { p++; continue; }
      const char *end = p + 1;
      for (char q = 0; *end && (q || *end != '>'); ++end)
         if (q) { if (*end == q) q = 0; } else if (*end == '"' || *end == '\'') q = *end;
      char val[1024], rel[128];
      const char *attr = kind == 1 ? "href" : "src";
      int ok = tag_attr(p + 1, end, attr, val, sizeof val) && val[0];
      if (ok && kind == 1)       /* stylesheets, and modules the page says it will import */
         ok = tag_attr(p + 1, end, "rel", rel, sizeof rel) &&
              (strcasestr(rel, "stylesheet") || strcasestr(rel, "modulepreload"));
      if (ok && !strncasecmp(val, "data:", 5)) ok = 0;
      if (ok) {
         decode_html_entities(val);
         char *abs = url_resolve(g_page.url, val);
         char *hash = abs ? strchr(abs, '#') : NULL;
         if (hash) *hash = 0;
         int dup = !abs || cache_find(abs) >= 0;
         for (int i = 0; !dup && i < pool.n; ++i) dup = !strcmp(pool.jobs[i].url, abs);
         if (dup) free(abs); else pool.jobs[pool.n++].url = abs;
      }
      if (!*end) break;
      p = end;
   }
   if (pool.n > 1) {
      pthread_t th[PF_THREADS];
      int nt = pool.n < PF_THREADS ? pool.n : PF_THREADS, started = 0;
      for (int i = 0; i < nt; ++i)
         if (pthread_create(&th[started], NULL, pf_worker, &pool) == 0) started++;
      if (!started) pf_worker(&pool);
      for (int i = 0; i < started; ++i) pthread_join(th[i], NULL);
   }
   for (int i = 0; i < pool.n; ++i) {
      struct pf_job *j = &pool.jobs[i];
      if (pool.n == 1) lb_fetch(j->url, "GET", NULL, NULL, 0, &j->r);
      LB_LOG("prefetch %s -> %d %s\n", j->url, j->r.status, j->r.error ? j->r.error : "");
      if (j->r.body && j->r.status >= 200 && j->r.status < 300) {
         cache_add(j->url, j->r.body, j->r.length);
         j->r.body = NULL; j->url = NULL;
      } else {
         /* Parsing must not retry every failed preload synchronously. */
         cache_add(j->url, NULL, 0);
         j->url = NULL;
      }
      free(j->url);
      response_free(&j->r);
   }
   free(pool.jobs);
}

/* luna-ui's resource reader: every relative reference in the document is a
 * URL relative to the page.  Local font files are the engine's own and are
 * left to its plain file loader. */
static int resource_fetch_async(const char *abs, const char *path);

/* A picture reached the cache: <img> elements that wait for its size are
 * styled again, which sizes them and flows the page around them. */
static void images_arrived(const char *abs)
{
   for (int i = 0; i < elem_count; ++i) {
      if (!elements[i].has_bg_image || strcmp(elements[i].type, "img")) continue;
      char *u = url_resolve(g_page.url, luna_expand_path(elements[i].bg_image_path));
      int same = u && !strcmp(u, abs);
      free(u);
      if (same) { LB_LOG("image arrived for element %d (%s)\n", i, elements[i].type); luna_image_arrived(i); LB_LOG("  css_w=%g css_h=%g has_w=%d has_h=%d pct_w=%d ar=%g tex=%u hasbg=%d\n", elements[i].css_width, elements[i].css_height, elements[i].has_css_width, elements[i].has_css_height, elements[i].pct_w, elements[i].aspect_ratio, (unsigned)elements[i].bg_image_tex, elements[i].has_bg_image); }
   }
}

static unsigned char *lb_read_resource(const char *path, size_t *out_size)
{
   *out_size = 0;
   if (!path || !path[0] || !g_page.url) return NULL;
   if (path[0] == '/' && path[1] != '/' && (is_font_path(path) ||
       !strncmp(g_page.url, "file:", 5) || !strncmp(g_page.url, "about:", 6)))
      return NULL;
   char *abs = url_resolve(g_page.url, path);
   if (!abs) return NULL;
   int hit = cache_find(abs);
   if (hit >= 0) {
      free(abs);
      if (!g_page.cache[hit].len) return NULL;           /* fetched and failed */
      unsigned char *copy = malloc(g_page.cache[hit].len + 1);
      if (!copy) return NULL;
      memcpy(copy, g_page.cache[hit].data, g_page.cache[hit].len);
      copy[g_page.cache[hit].len] = 0;
      *out_size = g_page.cache[hit].len;
      return copy;
   }
   /* A picture is read in the background and painted when it arrives; style,
    * script and font text are needed at once. */
   if (resource_fetch_async(abs, path)) { free(abs); return NULL; }
   struct response r;
   int ok = lb_fetch(abs, "GET", NULL, NULL, 0, &r) == 0 && r.status >= 200 && r.status < 300;
   LB_LOG("resource %s -> %d %s\n", abs, r.status, r.error ? r.error : "");
   unsigned char *data = NULL;
   if (ok && r.body) {
      data = malloc(r.length + 1);
      if (data) { memcpy(data, r.body, r.length); data[r.length] = 0; *out_size = r.length; }
      cache_add(abs, r.body, r.length);
      abs = NULL;
      r.body = NULL;
   } else {
      cache_add(abs, NULL, 0);
      abs = NULL;
   }
   free(abs);
   response_free(&r);
   return data;
}

static double lb_platform_time(void) { return lb_now_ms() / 1000.0; }

#ifndef LB_WINDOW
void luna_app_request_redraw(void) { g_page.dirty = 1; }
#endif

/* ======================================================================== *
 * Selectors over the luna-ui document
 * ======================================================================== */

enum {
   PS_FIRST = 1, PS_LAST = 2, PS_ONLY = 4, PS_EMPTY = 8, PS_ROOT = 16,
   PS_CHECKED = 32, PS_DISABLED = 64, PS_ENABLED = 128, PS_FOCUS = 256,
   PS_NTH = 512, PS_NTH_LAST = 1024, PS_NEVER = 2048, PS_TARGET = 4096,
};

struct sel_attr { char name[64]; char op; char val[192]; };

struct sel_simple {
   char tag[32], id[64];
   char cls[6][64];
   int ncls;
   struct sel_attr at[4];
   int nat;
   unsigned ps;
   int nth_a, nth_b;
   struct sel_simple *neg;     /* :not(compound) */
   char comb;                  /* combinator on the left: ' ', '>', '+', '~' */
};

struct sel_complex { struct sel_simple *c; int n; };
struct selector { struct sel_complex *g; int n; };

static void sel_free(struct selector *s)
{
   if (!s) return;
   for (int i = 0; i < s->n; ++i) {
      for (int k = 0; k < s->g[i].n; ++k) {
         struct sel_simple *neg = s->g[i].c[k].neg;
         free(neg);
      }
      free(s->g[i].c);
   }
   free(s->g);
   free(s);
}

static int sel_ident(const char **pp, char *out, size_t cap)
{
   const char *p = *pp;
   size_t n = 0;
   while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' ||
                 (unsigned char)*p >= 0x80 || *p == '\\')) {
      if (*p == '\\' && p[1]) p++;
      if (n + 1 < cap) out[n++] = *p;
      p++;
   }
   out[n] = '\0';
   *pp = p;
   return n > 0;
}

static int sel_nth(const char *s, int *a, int *b)
{
   while (*s == ' ') s++;
   if (!strncmp(s, "odd", 3)) { *a = 2; *b = 1; return 0; }
   if (!strncmp(s, "even", 4)) { *a = 2; *b = 0; return 0; }
   const char *n = strchr(s, 'n');
   if (!n) { *a = 0; *b = atoi(s); return 0; }
   if (n == s || (n == s + 1 && *s == '+')) *a = 1;
   else if (n == s + 1 && *s == '-') *a = -1;
   else *a = atoi(s);
   const char *r = n + 1;
   while (*r == ' ') r++;
   *b = (*r == '+' || *r == '-') ? (int)strtol(r, NULL, 10) : 0;
   if (*r == '+' || *r == '-') {
      int sign = *r == '-' ? -1 : 1;
      r++;
      while (*r == ' ') r++;
      *b = sign * atoi(r);
   }
   return 0;
}

static int sel_compound(const char **pp, struct sel_simple *s)
{
   const char *p = *pp;
   int any = 0;
   if (*p == '*') { p++; any = 1; }
   else if (isalpha((unsigned char)*p)) {
      sel_ident(&p, s->tag, sizeof s->tag);
      for (char *t = s->tag; *t; ++t) *t = (char)tolower((unsigned char)*t);
      any = 1;
   }
   for (;;) {
      if (*p == '#') {
         p++;
         if (!sel_ident(&p, s->id, sizeof s->id)) return -1;
      } else if (*p == '.') {
         p++;
         if (s->ncls >= 6) return -1;
         if (!sel_ident(&p, s->cls[s->ncls++], sizeof s->cls[0])) return -1;
      } else if (*p == '[') {
         if (s->nat >= 4) return -1;
         struct sel_attr *a = &s->at[s->nat++];
         p++;
         while (*p == ' ') p++;
         if (!sel_ident(&p, a->name, sizeof a->name)) return -1;
         for (char *t = a->name; *t; ++t) *t = (char)tolower((unsigned char)*t);
         while (*p == ' ') p++;
         if (*p == ']') { p++; any = 1; continue; }
         if (strchr("~|^$*", *p) && p[1] == '=') { a->op = *p; p += 2; }
         else if (*p == '=') { a->op = '='; p++; }
         else return -1;
         while (*p == ' ') p++;
         if (*p == '"' || *p == '\'') {
            char q = *p++;
            size_t n = 0;
            while (*p && *p != q) { if (n + 1 < sizeof a->val) a->val[n++] = *p; p++; }
            a->val[n] = '\0';
            if (*p != q) return -1;
            p++;
         } else sel_ident(&p, a->val, sizeof a->val);
         while (*p == ' ' || *p == 'i') p++;
         if (*p != ']') return -1;
         p++;
      } else if (*p == ':') {
         p++;
         if (*p == ':') {                  /* pseudo-elements never match nodes */
            p++;
            char tmp[32];
            sel_ident(&p, tmp, sizeof tmp);
            s->ps |= PS_NEVER;
            any = 1;
            continue;
         }
         char name[32];
         if (!sel_ident(&p, name, sizeof name)) return -1;
         const char *arg = NULL;
         size_t arglen = 0;
         if (*p == '(') {
            int depth = 1;
            arg = ++p;
            while (*p && depth) { if (*p == '(') depth++; else if (*p == ')') depth--; p++; }
            if (depth) return -1;
            arglen = (size_t)(p - arg - 1);
         }
         if (!strcmp(name, "first-child")) s->ps |= PS_FIRST;
         else if (!strcmp(name, "last-child")) s->ps |= PS_LAST;
         else if (!strcmp(name, "only-child")) s->ps |= PS_ONLY;
         else if (!strcmp(name, "empty")) s->ps |= PS_EMPTY;
         else if (!strcmp(name, "root")) s->ps |= PS_ROOT;
         else if (!strcmp(name, "target")) s->ps |= PS_TARGET;
         else if (!strcmp(name, "checked")) s->ps |= PS_CHECKED;
         else if (!strcmp(name, "disabled")) s->ps |= PS_DISABLED;
         else if (!strcmp(name, "enabled")) s->ps |= PS_ENABLED;
         else if (!strcmp(name, "focus") || !strcmp(name, "focus-within") ||
                  !strcmp(name, "focus-visible")) s->ps |= PS_FOCUS;
         else if ((!strcmp(name, "nth-child") || !strcmp(name, "nth-last-child")) && arg) {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%.*s", (int)arglen, arg);
            sel_nth(tmp, &s->nth_a, &s->nth_b);
            s->ps |= name[4] == 'l' ? PS_NTH_LAST : PS_NTH;
         } else if (!strcmp(name, "not") && arg && !s->neg) {
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%.*s", (int)arglen, arg);
            s->neg = calloc(1, sizeof *s->neg);
            const char *q = tmp;
            while (*q == ' ') q++;
            if (!s->neg || sel_compound(&q, s->neg) != 0) return -1;
         } else if (!strcmp(name, "hover") || !strcmp(name, "active") ||
                    !strcmp(name, "visited")) {
            s->ps |= PS_NEVER;
         } else if (!strcmp(name, "link") || !strcmp(name, "any-link") ||
                    !strcmp(name, "is") || !strcmp(name, "where")) {
            /* :link is every <a href>; :is()/:where() are approximated by
             * their argument's first compound. */
            if (arg && (!strcmp(name, "is") || !strcmp(name, "where"))) {
               char tmp[256];
               snprintf(tmp, sizeof tmp, "%.*s", (int)arglen, arg);
               const char *q = tmp;
               while (*q == ' ') q++;
               struct sel_simple inner = { 0 };
               if (sel_compound(&q, &inner) != 0) return -1;
               if (!s->tag[0]) memcpy(s->tag, inner.tag, sizeof s->tag);
               for (int i = 0; i < inner.ncls && s->ncls < 6; ++i)
                  memcpy(s->cls[s->ncls++], inner.cls[i], sizeof s->cls[0]);
               free(inner.neg);
            } else if (s->nat < 4) {
               strcpy(s->at[s->nat++].name, "href");
            }
         } else return -1;
      } else break;
      any = 1;
   }
   *pp = p;
   return any ? 0 : -1;
}

static struct selector *sel_parse(const char *text)
{
   struct selector *s = calloc(1, sizeof *s);
   if (!s) return NULL;
   const char *p = text;
   for (;;) {
      while (*p == ' ' || *p == '\t' || *p == '\n') p++;
      struct sel_complex cx = { 0 };
      char comb = 0;
      for (;;) {
         struct sel_simple one = { 0 };
         if (sel_compound(&p, &one) != 0) { free(one.neg); free(cx.c); sel_free(s); return NULL; }
         one.comb = comb;
         struct sel_simple *g = realloc(cx.c, sizeof *g * (size_t)(cx.n + 1));
         if (!g) { free(cx.c); sel_free(s); return NULL; }
         cx.c = g;
         cx.c[cx.n++] = one;
         int ws = 0;
         while (*p == ' ' || *p == '\t' || *p == '\n') { p++; ws = 1; }
         if (*p == '>' || *p == '+' || *p == '~') {
            comb = *p++;
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
         } else if (*p == ',' || !*p) break;
         else if (ws) comb = ' ';
         else { free(cx.c); sel_free(s); return NULL; }
      }
      struct sel_complex *g = realloc(s->g, sizeof *g * (size_t)(s->n + 1));
      if (!g) { free(cx.c); sel_free(s); return NULL; }
      s->g = g;
      s->g[s->n++] = cx;
      if (*p == ',') { p++; continue; }
      break;
   }
   return s;
}

static int el_node(int i)
{
   return i >= 0 && i < elem_count && !elements[i].luna_internal &&
          !elements[i].generated_pseudo && elements[i].dom_uid;
}

static int el_parent(int i)
{
   if (i < 0 || i >= elem_count || elements[i].dom_detached) return -1;
   return elements[i].parent_idx;
}

/* Element siblings (text runs are not elements to selectors). */
static int el_prev(int i)
{
   int p = elements[i].parent_idx;
   for (int j = i - 1; j >= 0; --j)
      if (elements[j].parent_idx == p && el_node(j) && !elements[j].anon_text &&
          !(p == -1 && elements[j].dom_detached))
         return j;
   return -1;
}

static int el_next(int i)
{
   int p = elements[i].parent_idx;
   for (int j = i + 1; j < elem_count; ++j)
      if (elements[j].parent_idx == p && el_node(j) && !elements[j].anon_text &&
          !(p == -1 && elements[j].dom_detached))
         return j;
   return -1;
}

static int el_has_class(int i, const char *cls)
{
   const char *s = elements[i].class_name;
   size_t n = strlen(cls);
   while (*s) {
      while (*s == ' ') s++;
      const char *e = s;
      while (*e && *e != ' ') e++;
      if ((size_t)(e - s) == n && !strncmp(s, cls, n)) return 1;
      s = e;
   }
   return 0;
}

static int nth_ok(int a, int b, int pos)
{
   if (!a) return pos == b;
   return (pos - b) % a == 0 && (pos - b) / a >= 0;
}

static int sel_simple_match(const struct sel_simple *s, int i)
{
   const LunaElement *e = &elements[i];
   if (s->ps & PS_NEVER) return 0;
   if (s->tag[0] && strcasecmp(s->tag, e->type)) return 0;
   if (s->id[0] && strcmp(s->id, e->id)) return 0;
   for (int k = 0; k < s->ncls; ++k) if (!el_has_class(i, s->cls[k])) return 0;
   for (int k = 0; k < s->nat; ++k) {
      char v[512];
      const struct sel_attr *a = &s->at[k];
      if (!luna_dom_get_attr(i, a->name, v, sizeof v)) return 0;
      size_t vl = strlen(v), al = strlen(a->val);
      switch (a->op) {
      case '=': if (strcmp(v, a->val)) return 0; break;
      case '^': if (!al || strncmp(v, a->val, al)) return 0; break;
      case '$': if (!al || vl < al || strcmp(v + vl - al, a->val)) return 0; break;
      case '*': if (!al || !strstr(v, a->val)) return 0; break;
      case '|': if (strcmp(v, a->val) && (strncmp(v, a->val, al) || v[al] != '-')) return 0; break;
      case '~': {
         int found = 0;
         for (char *t = strtok(v, " \t\n"); t && !found; t = strtok(NULL, " \t\n"))
            found = !strcmp(t, a->val);
         if (!found) return 0;
         break;
      }
      default: break;
      }
   }
   if (s->ps) {
      if ((s->ps & (PS_FIRST | PS_ONLY)) && el_prev(i) >= 0) return 0;
      if ((s->ps & (PS_LAST | PS_ONLY)) && el_next(i) >= 0) return 0;
      if ((s->ps & PS_ROOT) && (e->parent_idx != -1 || e->dom_detached)) return 0;
      if (s->ps & PS_EMPTY) {
         if (e->text[0] && !e->is_input) return 0;
         for (int j = i + 1; j < elem_count; ++j) if (elements[j].parent_idx == i && el_node(j)) return 0;
      }
      if (s->ps & PS_CHECKED) {
         char v[8];
         if (!luna_dom_get_attr(i, "checked", v, sizeof v) && !luna_dom_get_attr(i, "selected", v, sizeof v))
            return 0;
      }
      if (s->ps & (PS_DISABLED | PS_ENABLED)) {
         char v[8];
         int dis = luna_dom_get_attr(i, "disabled", v, sizeof v);
         if ((s->ps & PS_DISABLED) && !dis) return 0;
         if ((s->ps & PS_ENABLED) && dis) return 0;
      }
      if (s->ps & PS_TARGET) {
         const char *hash = g_page.url ? strchr(g_page.url, '#') : NULL;
         if (!hash || !hash[1] || !luna_dom_is_connected(i)) return 0;
         char *target = percent_decode(hash + 1, strlen(hash + 1));
         int matches = target && !strcmp(e->id, target);
         free(target);
         if (!matches) return 0;
      }
      if ((s->ps & PS_FOCUS) && luna_focused_element() != i) return 0;
      if (s->ps & (PS_NTH | PS_NTH_LAST)) {
         int pos = 1;
         for (int j = i; (j = (s->ps & PS_NTH) ? el_prev(j) : el_next(j)) >= 0; ) pos++;
         if (!nth_ok(s->nth_a, s->nth_b, pos)) return 0;
      }
   }
   if (s->neg && sel_simple_match(s->neg, i)) return 0;
   return 1;
}

static int sel_match_from(const struct sel_complex *cx, int k, int i)
{
   if (!sel_simple_match(&cx->c[k], i)) return 0;
   if (k == 0) return 1;
   switch (cx->c[k].comb) {
   case '>': { int p = el_parent(i); return p >= 0 && sel_match_from(cx, k - 1, p); }
   case '+': { int s = el_prev(i); return s >= 0 && sel_match_from(cx, k - 1, s); }
   case '~':
      for (int s = el_prev(i); s >= 0; s = el_prev(s)) if (sel_match_from(cx, k - 1, s)) return 1;
      return 0;
   default:
      for (int p = el_parent(i); p >= 0; p = el_parent(p)) if (sel_match_from(cx, k - 1, p)) return 1;
      return 0;
   }
}

static int sel_match(const struct selector *s, int i)
{
   if (!el_node(i) || elements[i].anon_text) return 0;
   for (int g = 0; g < s->n; ++g) if (sel_match_from(&s->g[g], s->g[g].n - 1, i)) return 1;
   return 0;
}

/* Is i inside root (root -1: connected to the document)? */
static int el_within(int i, int root)
{
   if (root < 0) return luna_dom_is_connected(i);
   for (int p = el_parent(i); p >= 0; p = el_parent(p)) if (p == root) return 1;
   return 0;
}

/* ======================================================================== *
 * Script engine: the native half of the DOM (`__lb`)
 * ======================================================================== */

static int layout_now(void)
{
   if (g_layout_dirty) luna_update(lb_now_ms() / 1000.0, 0.0);
   return 0;
}

static int uid_idx(JSContext *ctx, JSValueConst v)
{
   uint32_t uid = 0;
   if (JS_ToUint32(ctx, &uid, v) != 0 || !uid) return -1;
   return luna_dom_find(uid);
}

static JSValue js_uid(JSContext *ctx, int idx)
{
   return JS_NewUint32(ctx, idx >= 0 ? luna_dom_uid(idx) : 0);
}

static JSValue lb_body(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   for (int i = 0; i < elem_count; ++i)
      if (elements[i].parent_idx == -1 && !elements[i].dom_detached &&
          !strcmp(elements[i].type, "body") && elements[i].dom_uid)
         return js_uid(ctx, i);
   return JS_NewUint32(ctx, 0);
}

static JSValue lb_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *id = JS_ToCString(ctx, argv[0]);
   if (!id) return JS_EXCEPTION;
   int i = luna_get_element_by_id(id);
   if (i < 0 || !el_node(i) || !luna_dom_is_connected(i) || strcmp(elements[i].id, id)) {
      /* The id map keeps the first element per id; a detached or replaced
       * first one hides a connected duplicate. */
      i = -1;
      for (int j = 0; j < elem_count; ++j)
         if (el_node(j) && !strcmp(elements[j].id, id) && luna_dom_is_connected(j)) { i = j; break; }
   }
   JS_FreeCString(ctx, id);
   return js_uid(ctx, i);
}

static JSValue lb_query(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   uint32_t root_uid = 0;
   JS_ToUint32(ctx, &root_uid, argv[0]);
   int root = root_uid ? luna_dom_find(root_uid) : -1;
   if (root_uid && root < 0) return JS_NULL;
   const char *text = JS_ToCString(ctx, argv[1]);
   if (!text) return JS_EXCEPTION;
   struct selector *sel = sel_parse(text);
   if (!sel) {
      JSValue err = JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", text);
      JS_FreeCString(ctx, text);
      return err;
   }
   JS_FreeCString(ctx, text);
   int all = JS_ToBool(ctx, argv[2]);
   JSValue out = all ? JS_NewArray(ctx) : JS_NewUint32(ctx, 0);
   uint32_t n = 0;
   for (int i = 0; i < elem_count; ++i) {
      if (!el_within(i, root) || !sel_match(sel, i)) continue;
      if (!all) { out = js_uid(ctx, i); break; }
      JS_SetPropertyUint32(ctx, out, n++, js_uid(ctx, i));
   }
   sel_free(sel);
   return out;
}

static JSValue lb_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   const char *text = JS_ToCString(ctx, argv[1]);
   if (!text) return JS_EXCEPTION;
   struct selector *sel = sel_parse(text);
   JS_FreeCString(ctx, text);
   if (!sel) return JS_ThrowSyntaxError(ctx, "invalid selector");
   int m = i >= 0 && sel_match(sel, i);
   sel_free(sel);
   return JS_NewBool(ctx, m);
}

static JSValue lb_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   const char *tag = JS_ToCString(ctx, argv[0]);
   if (!tag) return JS_EXCEPTION;
   const char *attrs = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
   char lower[32];
   snprintf(lower, sizeof lower, "%s", tag);
   for (char *t = lower; *t; ++t) *t = (char)tolower((unsigned char)*t);
   int i = luna_dom_create(lower, attrs);
   JS_FreeCString(ctx, tag);
   if (attrs) JS_FreeCString(ctx, attrs);
   g_page.dirty = 1;
   return js_uid(ctx, i);
}

static JSValue lb_create_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *t = JS_ToCString(ctx, argv[0]);
   if (!t) return JS_EXCEPTION;
   int i = luna_dom_create_text(t);
   JS_FreeCString(ctx, t);
   return js_uid(ctx, i);
}

static JSValue lb_insert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   uint32_t pu = 0, bu = 0;
   JS_ToUint32(ctx, &pu, argv[0]);
   JS_ToUint32(ctx, &bu, argv[2]);
   int parent = pu ? luna_dom_find(pu) : -1;
   int child = uid_idx(ctx, argv[1]);
   int before = bu ? luna_dom_find(bu) : -1;
   if ((pu && parent < 0) || child < 0 || (bu && before < 0)) return JS_FALSE;
   g_page.dirty = 1;
   return JS_NewBool(ctx, luna_dom_insert(parent, child, before) >= 0);
}

static JSValue lb_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   g_page.dirty = 1;
   return JS_NewBool(ctx, i >= 0 && luna_dom_remove(i) == 0);
}

static JSValue lb_set_checked(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   if (i >= 0) luna_dom_set_checked(i, JS_ToBool(ctx, argv[1]) > 0);
   g_page.dirty = 1;
   return JS_UNDEFINED;
}

static JSValue lb_selection(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   char *t = luna_selection_text();
   JSValue v = JS_NewString(ctx, t ? t : "");
   free(t);
   return v;
}

/* __lb.imageSize(url) -> [width, height], or null when it cannot be read as a picture. */
static JSValue lb_image_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *u = JS_ToCString(ctx, argv[0]);
   if (!u) return JS_NULL;
   size_t n = 0;
   if (argc > 1 && JS_ToBool(ctx, argv[1])) {         /* only what is already cached */
      char *abs = url_resolve(g_page.url, u);
      int hit = abs && strncmp(abs, "data:", 5) ? cache_find(abs) : 0;
      free(abs);
      if (hit < 0) { JS_FreeCString(ctx, u); return JS_UNDEFINED; }
   }
   unsigned char *data = lb_read_resource(u, &n);
   JS_FreeCString(ctx, u);
   if (!data) return JS_NULL;
   int w = 0, h = 0, comp = 0;
   int ok = stbi_info_from_memory(data, (int)n, &w, &h, &comp);
   if (!ok && is_webp(data, n)) {
      simplewebp *dec = NULL;
      size_t pw = 0, ph = 0;
      if (simplewebp_load_from_memory((void *)data, n, NULL, &dec) == SIMPLEWEBP_NO_ERROR) {
         simplewebp_get_dimensions(dec, &pw, &ph);
         simplewebp_unload(dec);
         if (pw && ph) { w = (int)pw; h = (int)ph; ok = 1; }
      }
   }
   if (!ok && n > 4 && (memmem(data, n < 512 ? n : 512, "<svg", 4) != NULL)) { w = 300; h = 150; ok = 1; }
   free(data);
   if (!ok) return JS_NULL;
   JSValue a = JS_NewArray(ctx);
   JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, w));
   JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, h));
   return a;
}

/* __lb.cachePut(url, bytes): a picture fetched asynchronously, for the renderer. */
static JSValue lb_cache_put(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *u = JS_ToCString(ctx, argv[0]);
   if (!u) return JS_EXCEPTION;
   size_t n = 0;
   uint8_t *src = JS_GetArrayBuffer(ctx, &n, argv[1]);
   char *abs = src ? url_resolve(g_page.url, u) : NULL;
   JS_FreeCString(ctx, u);
   if (abs && cache_find(abs) < 0) {
      unsigned char *copy = malloc(n + 1);
      if (copy) { memcpy(copy, src, n); copy[n] = 0; char *key = lb_strdup(abs); cache_add(abs, copy, n); abs = NULL; if (key) images_arrived(key); free(key); }
   }
   free(abs);
   return JS_UNDEFINED;
}

static JSValue lb_parent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   return js_uid(ctx, i >= 0 ? el_parent(i) : -1);
}

static JSValue lb_children(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   JSValue out = JS_NewArray(ctx);
   if (i < 0) return out;
   uint32_t n = 0;
   for (int j = i + 1; j < elem_count; ++j)
      if (elements[j].parent_idx == i && el_node(j))
         JS_SetPropertyUint32(ctx, out, n++, js_uid(ctx, j));
   return out;
}

static JSValue lb_tag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   return i >= 0 ? JS_NewString(ctx, elements[i].type) : JS_NULL;
}

static JSValue lb_connected(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   return JS_NewBool(ctx, i >= 0 && luna_dom_is_connected(i));
}

static JSValue lb_get_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   const char *name = JS_ToCString(ctx, argv[1]);
   if (!name) return JS_EXCEPTION;
   JSValue r = JS_NULL;
   if (i >= 0) {
      const char *attrs = luna_dom_attrs(i);
      size_t cap = strlen(attrs) + 1;
      char *v = malloc(cap);
      if (v && luna_dom_get_attr(i, name, v, (int)cap)) r = JS_NewString(ctx, v);
      free(v);
   }
   JS_FreeCString(ctx, name);
   return r;
}

static JSValue lb_set_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   const char *name = JS_ToCString(ctx, argv[1]);
   const char *value = JS_ToCString(ctx, argv[2]);
   if (i >= 0 && name && value) {
      char lower[64];
      snprintf(lower, sizeof lower, "%s", name);
      for (char *t = lower; *t; ++t) *t = (char)tolower((unsigned char)*t);
      luna_dom_set_attr(i, lower, value);
      g_page.dirty = 1;
   }
   if (name) JS_FreeCString(ctx, name);
   if (value) JS_FreeCString(ctx, value);
   return JS_UNDEFINED;
}

static JSValue lb_remove_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   const char *name = JS_ToCString(ctx, argv[1]);
   if (i >= 0 && name) { luna_dom_remove_attr(i, name); g_page.dirty = 1; }
   if (name) JS_FreeCString(ctx, name);
   return JS_UNDEFINED;
}

static JSValue lb_attr_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   JSValue out = JS_NewArray(ctx);
   if (i < 0) return out;
   const char *s = luna_dom_attrs(i);
   uint32_t n = 0;
   char name[64];
   const char *vb, *ve;
   while ((s = dom_attr_next(s, name, sizeof name, &vb, &ve)) != NULL) {
      if (!name[0]) { if (*s) s++; continue; }
      JS_SetPropertyUint32(ctx, out, n++, JS_NewString(ctx, name));
   }
   return out;
}

static void text_of(int i, struct buf *b)
{
   int kids = 0;
   for (int j = i + 1; j < elem_count; ++j) if (elements[j].parent_idx == i && el_node(j)) { kids = 1; break; }
   const char *own = elements[i].is_input ? "" :
      elements[i].dom_raw_text ? elements[i].dom_raw_text : elements[i].text;
   if (!kids || elements[i].direct_text_before_children) buf_puts(b, own);
   for (int j = i + 1; j < elem_count; ++j)
      if (elements[j].parent_idx == i && el_node(j)) text_of(j, b);
   if (kids && !elements[i].direct_text_before_children) buf_puts(b, own);
}

static JSValue lb_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   if (i < 0) return JS_NewString(ctx, "");
   struct buf b = { 0 };
   text_of(i, &b);
   JSValue r = JS_NewString(ctx, b.p ? b.p : "");
   free(b.p);
   return r;
}

/* textContent = s: the element's children go and its own text becomes s. */
static JSValue lb_set_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   const char *s = JS_ToCString(ctx, argv[1]);
   if (!s) return JS_EXCEPTION;
   if (i >= 0) {
      int kids = 0;
      for (int j = i + 1; j < elem_count; ++j) if (elements[j].parent_idx == i && el_node(j)) { kids = 1; break; }
      if (kids) {
         luna_dom_set_inner_html(i, "");
         i = uid_idx(ctx, argv[0]);
      }
      if (i >= 0) {
         if (elements[i].dom_raw_text) {
            char *raw = lb_strdup(s);
            if (!raw) { JS_FreeCString(ctx, s); return JS_ThrowOutOfMemory(ctx); }
            free(elements[i].dom_raw_text);
            elements[i].dom_raw_text = raw;
         }
         luna_set_text(i, s);
         elements[i].direct_text_before_children = s[0] != '\0';
      }
      g_page.dirty = 1;
   }
   JS_FreeCString(ctx, s);
   return JS_UNDEFINED;
}

static void html_escape(struct buf *b, const char *s, int attr)
{
   for (; *s; ++s) {
      if (*s == '&') buf_puts(b, "&amp;");
      else if (*s == '<' && !attr) buf_puts(b, "&lt;");
      else if (*s == '>' && !attr) buf_puts(b, "&gt;");
      else if (*s == '"' && attr) buf_puts(b, "&quot;");
      else buf_put(b, s, 1);
   }
}

static int is_void_tag(const char *t)
{
   static const char *v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input",
                              "link", "meta", "param", "source", "track", "wbr", NULL };
   for (int i = 0; v[i]; ++i) if (!strcasecmp(t, v[i])) return 1;
   return 0;
}

static void html_of(int i, struct buf *b, int outer)
{
   const LunaElement *e = &elements[i];
   if (e->anon_text) {
      if (e->ws_before) buf_puts(b, " ");
      html_escape(b, e->text, 0);
      return;
   }
   if (outer) {
      if (e->ws_before) buf_puts(b, " ");
      buf_printf(b, "<%s%s>", e->type, luna_dom_attrs(i));
      if (is_void_tag(e->type)) return;
   }
   int kids = 0;
   for (int j = i + 1; j < elem_count; ++j) if (elements[j].parent_idx == i && el_node(j)) { kids = 1; break; }
   const char *own = e->is_input && strcasecmp(e->type, "textarea") ? "" :
      e->dom_raw_text ? e->dom_raw_text : e->text;
   const int raw = !strcasecmp(e->type, "script") || !strcasecmp(e->type, "style");
   if (!kids || e->direct_text_before_children) {
      if (raw) buf_puts(b, own); else html_escape(b, own, 0);
   }
   for (int j = i + 1; j < elem_count; ++j)
      if (elements[j].parent_idx == i && el_node(j)) html_of(j, b, 1);
   if (kids && !e->direct_text_before_children) {
      if (raw) buf_puts(b, own); else html_escape(b, own, 0);
   }
   if (outer) buf_printf(b, "</%s>", e->type);
}

static JSValue lb_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   if (i < 0) return JS_NewString(ctx, "");
   struct buf b = { 0 };
   html_of(i, &b, JS_ToBool(ctx, argv[1]));
   JSValue r = JS_NewString(ctx, b.p ? b.p : "");
   free(b.p);
   return r;
}

static JSValue lb_set_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   const char *s = JS_ToCString(ctx, argv[1]);
   if (!s) return JS_EXCEPTION;
   if (i >= 0 && elements[i].dom_raw_text) {
      JS_FreeCString(ctx, s);
      return lb_set_text(ctx, this_val, argc, argv);
   }
   if (i >= 0) { luna_dom_set_inner_html(i, s); g_page.dirty = 1; }
   JS_FreeCString(ctx, s);
   return JS_UNDEFINED;
}

static JSValue lb_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   int i = uid_idx(ctx, argv[0]);
   JSValue out = JS_NewArray(ctx);
   layout_now();
   i = uid_idx(ctx, argv[0]);
   float v[4] = { 0, 0, 0, 0 };
   if (i >= 0 && is_visible(i)) {
      v[0] = elements[i].x; v[1] = elements[i].y; v[2] = elements[i].w; v[3] = elements[i].h;
   }
   for (int k = 0; k < 4; ++k) JS_SetPropertyUint32(ctx, out, (uint32_t)k, JS_NewFloat64(ctx, v[k]));
   return out;
}

static JSValue lb_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   int i = uid_idx(ctx, argv[0]);
   if (i < 0) return JS_NewString(ctx, "");
   if (argc > 1 && !JS_IsUndefined(argv[1])) {
      const char *s = JS_ToCString(ctx, argv[1]);
      if (!s) return JS_EXCEPTION;
      luna_set_value(i, s);
      JS_FreeCString(ctx, s);
      g_page.dirty = 1;
   }
   const char *v = luna_get_value(i);
   return JS_NewString(ctx, v ? v : "");
}

static JSValue lb_focus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc > 0 && !JS_IsUndefined(argv[0])) {
      int i = uid_idx(ctx, argv[0]);
      luna_focus_element(i >= 0 && luna_dom_is_connected(i) ? i : -1);
      g_page.dirty = 1;
   }
   return js_uid(ctx, luna_focused_element());
}

static JSValue lb_scroll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   int i = uid_idx(ctx, argv[0]);
   JSValue out = JS_NewArray(ctx);
   if (i < 0) return out;
   LunaElement *e = &elements[i];
   double v;
   if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &v, argv[1]) == 0) {
      e->scroll_top = e->scroll_dest_top = (float)v;
      g_page.dirty = 1;
   }
   if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToFloat64(ctx, &v, argv[2]) == 0) {
      e->scroll_left = e->scroll_dest_left = (float)v;
      g_page.dirty = 1;
   }
   layout_now();
   e = &elements[i];
   JS_SetPropertyUint32(ctx, out, 0, JS_NewFloat64(ctx, e->scroll_top));
   JS_SetPropertyUint32(ctx, out, 1, JS_NewFloat64(ctx, e->scroll_left));
   JS_SetPropertyUint32(ctx, out, 2, JS_NewFloat64(ctx, e->scroll_content_h));
   JS_SetPropertyUint32(ctx, out, 3, JS_NewFloat64(ctx, e->scroll_content_w));
   return out;
}

static JSValue lb_offset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   layout_now();
   int i = uid_idx(ctx, argv[0]), parent = -1;
   JSValue out = JS_NewArray(ctx);
   float x = 0, y = 0;
   if (i >= 0) {
      LunaElement *e = &elements[i];
      x = e->x; y = e->y;
      if (is_visible(i) && !e->position_fixed && strcmp(e->type, "body") && strcmp(e->type, "html")) {
         for (int p = e->parent_idx; p >= 0; p = elements[p].parent_idx) {
            LunaElement *a = &elements[p];
            if (is_positioned_element(a) || !strcmp(a->type, "body") ||
                !strcmp(a->type, "table") || !strcmp(a->type, "td") || !strcmp(a->type, "th")) {
               parent = p; break;
            }
         }
      }
      if (parent >= 0) {
         LunaElement *a = &elements[parent];
         x -= a->x + a->border_width; y -= a->y + a->border_width;
      }
   }
   JS_SetPropertyUint32(ctx, out, 0, JS_NewFloat64(ctx, x));
   JS_SetPropertyUint32(ctx, out, 1, JS_NewFloat64(ctx, y));
   JS_SetPropertyUint32(ctx, out, 2, js_uid(ctx, parent));
   return out;
}

static JSValue lb_document_scroll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   layout_now();
   float x, y, w, h;
   luna_document_scroll_state(&x, &y, &w, &h);
   double v;
   if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToFloat64(ctx, &v, argv[0]) == 0) x = v;
   if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &v, argv[1]) == 0) y = v;
   if (argc) { luna_document_scroll(x, y); g_page.dirty = 1; layout_now(); }
   luna_document_scroll_state(&x, &y, &w, &h);
   JSValue out = JS_NewArray(ctx);
   float values[] = {x, y, w, h};
   for (int i = 0; i < 4; ++i) JS_SetPropertyUint32(ctx, out, i, JS_NewFloat64(ctx, values[i]));
   return out;
}

static JSValue lb_title(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (argc > 0 && JS_IsString(argv[0])) {
      const char *s = JS_ToCString(ctx, argv[0]);
      if (s) { snprintf(luna_doc_title, sizeof luna_doc_title, "%s", s); JS_FreeCString(ctx, s); }
   }
   return JS_NewString(ctx, luna_doc_title);
}

static JSValue lb_css(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *s = JS_ToCString(ctx, argv[0]);
   if (!s) return JS_EXCEPTION;
   LB_LOG("css (%zu bytes): %.160s\n", strlen(s), s);
   luna_parse_css(s);
   JS_FreeCString(ctx, s);
   g_page.dirty = 1;
   return JS_UNDEFINED;
}

/* __lb.http(method, url, headers "Name: v\r\n"..., body, binary) ->
 * { status, url, headers, body, error } */
static JSValue lb_http(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   const char *method = JS_ToCString(ctx, argv[0]);
   const char *rel = JS_ToCString(ctx, argv[1]);
   const char *headers = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
   size_t body_len = 0;
   const char *body = argc > 3 && JS_IsString(argv[3]) ? JS_ToCStringLen(ctx, &body_len, argv[3]) : NULL;
   int binary = argc > 4 && JS_ToBool(ctx, argv[4]);
   JSValue out = JS_NewObject(ctx);
   if (method && rel) {
      char *url = url_resolve(g_page.url, rel);
      struct response r;
      lb_fetch(url, method, headers, (const unsigned char *)body, body_len, &r);
      LB_LOG("%s %s -> %d %s\n", method, url, r.status, r.error ? r.error : "");
      JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, r.status));
      JS_SetPropertyStr(ctx, out, "url", JS_NewString(ctx, r.url ? r.url : url));
      JS_SetPropertyStr(ctx, out, "headers", JS_NewString(ctx, r.headers ? r.headers : ""));
      if (binary) JS_SetPropertyStr(ctx, out, "body", JS_NewArrayBufferCopy(ctx, r.body ? r.body : (const uint8_t *)"", r.length));
      else JS_SetPropertyStr(ctx, out, "body", JS_NewStringLen(ctx, r.body ? (const char *)r.body : "", r.length));
      if (r.error) JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, r.error));
      response_free(&r);
      free(url);
   }
   if (method) JS_FreeCString(ctx, method);
   if (rel) JS_FreeCString(ctx, rel);
   if (headers) JS_FreeCString(ctx, headers);
   if (body) JS_FreeCString(ctx, body);
   return out;
}

static void js_report(JSContext *ctx, const char *where)
{
   JSValue ex = JS_GetException(ctx);
   const char *msg = JS_ToCString(ctx, ex);
   JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
   const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
   fprintf(stderr, "[luna-browser] uncaught (%s): %s\n%s", where, msg ? msg : "?", st ? st : "");
   struct buf p = { 0 };
   buf_puts(&p, "{\"exceptionDetails\":{\"text\":");
   buf_json_str(&p, msg ? msg : "error");
   buf_puts(&p, ",\"url\":");
   buf_json_str(&p, where);
   buf_puts(&p, "}}");
   page_emit("Runtime.exceptionThrown", p.p);
   free(p.p);
   if (msg) JS_FreeCString(ctx, msg);
   if (st) JS_FreeCString(ctx, st);
   JS_FreeValue(ctx, stack);
   JS_FreeValue(ctx, ex);
}

/* A script that runs too long is stopped, as a browser stops an unresponsive
 * page: the event loop, input and the embedder must keep going. */
static double g_js_deadline;

static int js_interrupt(JSRuntime *rt, void *opaque)
{
   (void)rt; (void)opaque;
   return g_js_deadline > 0 && lb_now_ms() > g_js_deadline;
}

static void js_arm(void)
{
   const char *e = getenv("LUNA_BROWSER_SCRIPT_MS");
   const double ms = e && *e ? atof(e) : 10000.0;
   g_js_deadline = ms > 0 ? lb_now_ms() + ms : 0;
}

static void js_run_jobs(void)
{
   JSContext *jc;
   for (int i = 0; i < 10000; ++i) {
      int r = JS_ExecutePendingJob(g_page.rt, &jc);
      if (r <= 0) { if (r < 0) js_report(jc, "promise job"); break; }
      if (i == 9999) LB_LOG("microtask queue not draining\n");
   }
}

static void module_prefetch(const char *root_url, const unsigned char *root_src, size_t root_len);

static int js_eval(const char *code, size_t len, const char *filename, int module)
{
   if (!g_page.ctx) return -1;
   if (module) module_prefetch(filename, (const unsigned char *)code, len);
   js_arm();
   double t0 = lb_now_ms();
   JSValue v = JS_Eval(g_page.ctx, code, len, filename,
                       module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
   int ok = !JS_IsException(v);
   if (!ok) js_report(g_page.ctx, filename);
   JS_FreeValue(g_page.ctx, v);
   js_run_jobs();
   if (g_verbose) LB_LOG("js %.0fms %zuB %.80s\n", lb_now_ms() - t0, len, filename);
   return ok ? 0 : -1;
}

/* Fetch and run a classic script (or module) by URL. */
static int js_eval_url(const char *rel, int module)
{
   char *url = url_resolve(g_page.url, rel);
   struct response r;
   int hit = cache_find(url), ok;
   if (hit >= 0) {
      memset(&r, 0, sizeof r);
      r.body = g_page.cache[hit].data ? (unsigned char *)lb_strndup((const char *)g_page.cache[hit].data, g_page.cache[hit].len) : NULL;
      r.length = g_page.cache[hit].len;
      ok = r.body != NULL;
   } else ok = lb_fetch(url, "GET", NULL, NULL, 0, &r) == 0 && r.status >= 200 && r.status < 300;
   LB_LOG("script %s -> %d\n", url, r.status);
   if (ok) ok = js_eval((const char *)r.body, r.length, r.url ? r.url : url, module) == 0;
   else fprintf(stderr, "[luna-browser] script %s failed: %d %s\n", url, r.status,
                r.error ? r.error : "");
   response_free(&r);
   free(url);
   return ok ? 0 : -1;
}

static JSValue lb_eval_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   const char *u = JS_ToCString(ctx, argv[0]);
   if (!u) return JS_EXCEPTION;
   char *copy = lb_strdup(u);
   JS_FreeCString(ctx, u);
   int ok = js_eval_url(copy, argc > 1 && JS_ToBool(ctx, argv[1])) == 0;
   free(copy);
   return JS_NewBool(ctx, ok);
}

/* __lb.isCached(url): is the script/stylesheet already in the page cache? */
static JSValue lb_is_cached(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *u = JS_ToCString(ctx, argv[0]);
   if (!u) return JS_EXCEPTION;
   char *abs = url_resolve(g_page.url, u);
   JS_FreeCString(ctx, u);
   int hit = abs && cache_find(abs) >= 0;
   free(abs);
   return JS_NewBool(ctx, hit);
}

/* __lb.evalCode(source, url, module): run source fetched elsewhere, as script `url`. */
static JSValue lb_eval_code(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   size_t n = 0;
   const char *code = JS_ToCStringLen(ctx, &n, argv[0]);
   const char *u = JS_ToCString(ctx, argv[1]);
   if (!code || !u) { if (code) JS_FreeCString(ctx, code); if (u) JS_FreeCString(ctx, u); return JS_EXCEPTION; }
   char *src = lb_strndup(code, n);
   char *name = url_resolve(g_page.url, u);
   JS_FreeCString(ctx, code);
   JS_FreeCString(ctx, u);
   int ok = src && js_eval(src, n, name ? name : "script", argc > 2 && JS_ToBool(ctx, argv[2])) == 0;
   free(src); free(name);
   return JS_NewBool(ctx, ok);
}

static JSValue lb_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   return JS_NewFloat64(ctx, lb_now_ms());
}

static JSValue lb_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *level = JS_ToCString(ctx, argv[0]);
   const char *text = JS_ToCString(ctx, argv[1]);
   if (level && text) {
      fprintf(stderr, "[console.%s] %s\n", level, text);
      struct buf p = { 0 };
      buf_puts(&p, "{\"type\":");
      buf_json_str(&p, !strcmp(level, "warn") ? "warning" : level);
      buf_puts(&p, ",\"args\":[{\"type\":\"string\",\"value\":");
      buf_json_str(&p, text);
      buf_printf(&p, "}],\"executionContextId\":1,\"timestamp\":%.3f}", lb_now_ms());
      page_emit("Runtime.consoleAPICalled", p.p);
      free(p.p);
   }
   if (level) JS_FreeCString(ctx, level);
   if (text) JS_FreeCString(ctx, text);
   return JS_UNDEFINED;
}

static JSValue lb_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   return JS_NewString(ctx, g_page.url ? g_page.url : "about:blank");
}

static JSValue lb_navigate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *u = JS_ToCString(ctx, argv[0]);
   if (!u) return JS_EXCEPTION;
   free(g_page.nav_url);
   g_page.nav_url = url_resolve(g_page.url, u);
   JS_FreeCString(ctx, u);
   return JS_UNDEFINED;
}

static JSValue lb_viewport(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc; (void)argv;
   JSValue out = JS_NewArray(ctx);
   JS_SetPropertyUint32(ctx, out, 0, JS_NewInt32(ctx, g_page.w));
   JS_SetPropertyUint32(ctx, out, 1, JS_NewInt32(ctx, g_page.h));
   JS_SetPropertyUint32(ctx, out, 2, JS_NewFloat64(ctx, g_page.scale));
   return out;
}

/* localStorage persistence: one JSON file per origin in the profile. */
static char *storage_path(void)
{
   if (!g_page.profile || !g_page.url) return NULL;
   char *origin = url_origin(g_page.url);
   struct buf b = { 0 };
   buf_printf(&b, "%s/storage-", g_page.profile);
   for (const char *s = origin; *s; ++s)
      buf_printf(&b, isalnum((unsigned char)*s) || *s == '.' ? "%c" : "_", *s);
   buf_puts(&b, ".json");
   free(origin);
   return buf_take(&b);
}

static JSValue lb_storage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   char *path = storage_path();
   JSValue r = JS_NULL;
   if (argc > 0 && JS_IsString(argv[0]) && path) {
      size_t n;
      const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
      FILE *f = s ? fopen(path, "wb") : NULL;
      if (f) { fwrite(s, 1, n, f); fclose(f); }
      if (s) JS_FreeCString(ctx, s);
   } else if (path) {
      struct response resp;
      char *url = malloc(strlen(path) + 8);
      sprintf(url, "file://%s", path);
      if (lb_fetch(url, "GET", NULL, NULL, 0, &resp) == 0)
         r = JS_NewStringLen(ctx, (const char *)resp.body, resp.length);
      response_free(&resp);
      free(url);
   }
   free(path);
   return r;
}

static JSValue lb_cookie(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   if (!g_page.url) return JS_NewString(ctx, "");
   if (argc > 0 && JS_IsString(argv[0])) {
      const char *s = JS_ToCString(ctx, argv[0]);
      if (s) { cookie_set(g_page.url, s, 1); JS_FreeCString(ctx, s); }
   }
   char *c = cookie_header(g_page.url);
   JSValue r = JS_NewString(ctx, c);
   free(c);
   return r;
}

static JSValue lb_binding(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *name = JS_ToCString(ctx, argv[0]);
   const char *payload = JS_ToCString(ctx, argv[1]);
   if (name && payload) {
      struct buf p = { 0 };
      buf_puts(&p, "{\"name\":");
      buf_json_str(&p, name);
      buf_puts(&p, ",\"payload\":");
      buf_json_str(&p, payload);
      buf_puts(&p, ",\"executionContextId\":1}");
      page_emit("Runtime.bindingCalled", p.p);
      free(p.p);
   }
   if (name) JS_FreeCString(ctx, name);
   if (payload) JS_FreeCString(ctx, payload);
   return JS_UNDEFINED;
}

static JSValue lb_redraw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)ctx; (void)this_val; (void)argc; (void)argv;
   g_page.dirty = 1;
   return JS_UNDEFINED;
}

static JSValue lb_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   const char *base = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
   const char *rel = JS_ToCString(ctx, argv[0]);
   JSValue r = JS_NULL;
   if (rel) {
      struct url probe;
      const char *b = base ? base : g_page.url;
      /* An absolute reference, or a relative one against a valid base. */
      if (url_parse(rel, &probe) == 0 || (b && url_parse(b, &probe) == 0)) {
         char *s = url_resolve(url_parse(rel, &probe) == 0 ? NULL : b, rel);
         r = JS_NewString(ctx, s ? s : "");
         free(s);
      }
      url_free(&probe);
      JS_FreeCString(ctx, rel);
   }
   if (base) JS_FreeCString(ctx, base);
   return r;
}

static JSValue lb_random(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   uint32_t n = 0;
   JS_ToUint32(ctx, &n, argv[0]);
   if (n > 65536) n = 65536;
   unsigned char *b = malloc(n ? n : 1);
   if (!b) return JS_EXCEPTION;
   int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
   ssize_t got = fd >= 0 ? read(fd, b, n) : -1;
   if (fd >= 0) close(fd);
   if (got != (ssize_t)n) for (uint32_t i = 0; i < n; ++i) b[i] = (unsigned char)rand();
   JSValue r = JS_NewArrayBufferCopy(ctx, b, n);
   free(b);
   return r;
}

/* ---- asynchronous requests ---------------------------------------------
 * fetch() and XMLHttpRequest run on worker threads, as a browser's network
 * stack does, so a slow server never stalls timers, input or the embedder.
 * A finished request waits in g_done until the page's event loop takes it. */
struct http_job {
   int id;
   unsigned gen;                 /* the page it belongs to */
   char *method, *url, *headers, *body;
   size_t body_len;
   int binary;
   int kind;                     /* 1: a picture for the renderer, kept in the page cache */
   char *path;                   /* ... as the renderer named it */
   struct response r;
   struct http_job *next;
};
static pthread_mutex_t g_done_lock = PTHREAD_MUTEX_INITIALIZER;
static struct http_job *g_done, *g_done_tail;
static int g_wake[2] = { -1, -1 };
static unsigned g_page_gen = 1;
static int g_jobs_out;

static JSValue js_hook(const char *name, int argc, JSValueConst *argv);

static void *http_worker(void *arg)
{
   struct http_job *j = arg;
   lb_fetch(j->url, j->method, j->headers, (const unsigned char *)j->body, j->body_len, &j->r);
   pthread_mutex_lock(&g_done_lock);
   if (g_done_tail) g_done_tail->next = j; else g_done = j;
   g_done_tail = j;
   pthread_mutex_unlock(&g_done_lock);
   if (g_wake[1] >= 0) { ssize_t w = write(g_wake[1], "x", 1); (void)w; }
   return NULL;
}

static void http_job_free(struct http_job *j)
{
   free(j->method); free(j->url); free(j->headers); free(j->body); free(j->path);
   response_free(&j->r);
   free(j);
}

static char **g_inflight;
static int g_ninflight;

static int inflight_has(const char *url)
{
   for (int i = 0; i < g_ninflight; ++i) if (!strcmp(g_inflight[i], url)) return 1;
   return 0;
}

static void inflight_remove(const char *url)
{
   for (int i = 0; i < g_ninflight; ++i)
      if (!strcmp(g_inflight[i], url)) { free(g_inflight[i]); g_inflight[i] = g_inflight[--g_ninflight]; return; }
}

/* Start fetching `abs` for the renderer.  Returns 0 when the resource is one
 * to read at once (not http, or style/script/font), else 1. */
static int resource_fetch_async(const char *abs, const char *path)
{
   if (strncmp(abs, "http://", 7) && strncmp(abs, "https://", 8)) return 0;
   if (!g_page.ctx || g_wake[1] < 0) return 0;
   const char *q = strpbrk(abs, "?#");
   size_t n = q ? (size_t)(q - abs) : strlen(abs);
   static const char *sync_ext[] = { ".css", ".js", ".mjs", ".json", ".woff", ".woff2", ".ttf", ".otf", ".html", NULL };
   for (int i = 0; sync_ext[i]; ++i) {
      size_t l = strlen(sync_ext[i]);
      if (n >= l && !strncasecmp(abs + n - l, sync_ext[i], l)) return 0;
   }
   if (inflight_has(abs)) return 1;
   struct http_job *j = calloc(1, sizeof *j);
   if (!j) return 0;
   j->id = 0;
   j->kind = 1;
   j->gen = g_page_gen;
   j->method = lb_strdup("GET");
   j->url = lb_strdup(abs);
   j->path = lb_strdup(path);
   char **grown = realloc(g_inflight, sizeof *g_inflight * (size_t)(g_ninflight + 1));
   if (!grown) { http_job_free(j); return 0; }
   g_inflight = grown;
   g_inflight[g_ninflight++] = lb_strdup(abs);
   pthread_t t;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   if (pthread_create(&t, &attr, http_worker, j) != 0) {
      inflight_remove(abs);
      http_job_free(j);
      pthread_attr_destroy(&attr);
      return 0;
   }
   g_jobs_out++;
   pthread_attr_destroy(&attr);
   return 1;
}

/* __lb.httpAsync(method, url, headers, body, binary) -> request id */
static JSValue lb_http_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val;
   static int seq;
   struct http_job *j = calloc(1, sizeof *j);
   if (!j) return JS_EXCEPTION;
   const char *method = JS_ToCString(ctx, argv[0]);
   const char *rel = JS_ToCString(ctx, argv[1]);
   const char *headers = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
   size_t body_len = 0;
   const char *body = argc > 3 && JS_IsString(argv[3]) ? JS_ToCStringLen(ctx, &body_len, argv[3]) : NULL;
   j->id = ++seq;
   j->gen = g_page_gen;
   j->method = lb_strdup(method ? method : "GET");
   j->url = url_resolve(g_page.url, rel ? rel : "");
   j->headers = headers ? lb_strdup(headers) : NULL;
   j->body = body ? lb_strndup(body, body_len) : NULL;
   j->body_len = body_len;
   j->binary = argc > 4 && JS_ToBool(ctx, argv[4]);
   if (method) JS_FreeCString(ctx, method);
   if (rel) JS_FreeCString(ctx, rel);
   if (headers) JS_FreeCString(ctx, headers);
   if (body) JS_FreeCString(ctx, body);
   pthread_t t;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   if (pthread_create(&t, &attr, http_worker, j) != 0) {
      j->r.error = lb_strdup("no thread");
      http_worker(j);
   } else g_jobs_out++;
   pthread_attr_destroy(&attr);
   return JS_NewInt32(ctx, j->id);
}

/* Hands finished requests to the page (main thread). */
static void http_drain(void)
{
   if (g_wake[0] >= 0) { char tmp[64]; while (read(g_wake[0], tmp, sizeof tmp) > 0) { } }
   pthread_mutex_lock(&g_done_lock);
   struct http_job *j = g_done;
   g_done = g_done_tail = NULL;
   pthread_mutex_unlock(&g_done_lock);
   while (j) {
      struct http_job *next = j->next;
      if (g_jobs_out > 0) g_jobs_out--;
      LB_LOG("%s %s -> %d %s\n", j->method, j->url, j->r.status, j->r.error ? j->r.error : "");
      if (j->kind == 1) {
         if (j->gen == g_page_gen) {
            inflight_remove(j->url);
            int ok = j->r.body && j->r.length && j->r.status >= 200 && j->r.status < 300;
            char *abs = lb_strdup(j->url);
            if (ok) { cache_add(j->url, j->r.body, j->r.length); j->url = NULL; j->r.body = NULL; }
            else cache_add(lb_strdup(j->url), NULL, 0);          /* do not ask again */
            luna_invalidate_texture(j->path);
            if (ok && abs) images_arrived(abs);
            free(abs);
            g_page.dirty = 1;
         }
      } else if (j->gen == g_page_gen && g_page.ctx) {
         JSContext *ctx = g_page.ctx;
         JSValue o = JS_NewObject(ctx);
         JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, j->r.status));
         JS_SetPropertyStr(ctx, o, "url", JS_NewString(ctx, j->r.url ? j->r.url : j->url));
         JS_SetPropertyStr(ctx, o, "headers", JS_NewString(ctx, j->r.headers ? j->r.headers : ""));
         if (j->binary)
            JS_SetPropertyStr(ctx, o, "body", JS_NewArrayBufferCopy(ctx, j->r.body ? j->r.body : (const uint8_t *)"", j->r.length));
         else
            JS_SetPropertyStr(ctx, o, "body", JS_NewStringLen(ctx, j->r.body ? (const char *)j->r.body : "", j->r.length));
         if (j->r.error) JS_SetPropertyStr(ctx, o, "error", JS_NewString(ctx, j->r.error));
         JSValue a[2] = { JS_NewInt32(ctx, j->id), o };
         JS_FreeValue(ctx, js_hook("__lb_http_done", 2, a));
         JS_FreeValue(ctx, o);
      }
      http_job_free(j);
      j = next;
   }
}

static JSValue lb_set_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
   (void)this_val; (void)argc;
   const char *u = JS_ToCString(ctx, argv[0]);
   if (!u) return JS_EXCEPTION;
   free(g_page.url);
   g_page.url = lb_strdup(u);
   JS_FreeCString(ctx, u);
   return JS_UNDEFINED;
}

static const JSCFunctionListEntry lb_funcs[] = {
   JS_CFUNC_DEF("body", 0, lb_body),
   JS_CFUNC_DEF("byId", 1, lb_by_id),
   JS_CFUNC_DEF("query", 3, lb_query),
   JS_CFUNC_DEF("matches", 2, lb_matches),
   JS_CFUNC_DEF("create", 2, lb_create),
   JS_CFUNC_DEF("createText", 1, lb_create_text),
   JS_CFUNC_DEF("insert", 3, lb_insert),
   JS_CFUNC_DEF("remove", 1, lb_remove),
   JS_CFUNC_DEF("setChecked", 2, lb_set_checked),
   JS_CFUNC_DEF("imageSize", 2, lb_image_size),
   JS_CFUNC_DEF("cachePut", 2, lb_cache_put),
   JS_CFUNC_DEF("selection", 0, lb_selection),
   JS_CFUNC_DEF("parent", 1, lb_parent),
   JS_CFUNC_DEF("children", 1, lb_children),
   JS_CFUNC_DEF("tag", 1, lb_tag),
   JS_CFUNC_DEF("connected", 1, lb_connected),
   JS_CFUNC_DEF("getAttr", 2, lb_get_attr),
   JS_CFUNC_DEF("setAttr", 3, lb_set_attr),
   JS_CFUNC_DEF("removeAttr", 2, lb_remove_attr),
   JS_CFUNC_DEF("attrNames", 1, lb_attr_names),
   JS_CFUNC_DEF("text", 1, lb_text),
   JS_CFUNC_DEF("setText", 2, lb_set_text),
   JS_CFUNC_DEF("html", 2, lb_html),
   JS_CFUNC_DEF("setHTML", 2, lb_set_html),
   JS_CFUNC_DEF("rect", 1, lb_rect),
   JS_CFUNC_DEF("value", 2, lb_value),
   JS_CFUNC_DEF("focus", 1, lb_focus),
   JS_CFUNC_DEF("scroll", 3, lb_scroll),
   JS_CFUNC_DEF("documentScroll", 2, lb_document_scroll),
   JS_CFUNC_DEF("offset", 1, lb_offset),
   JS_CFUNC_DEF("title", 1, lb_title),
   JS_CFUNC_DEF("css", 1, lb_css),
   JS_CFUNC_DEF("http", 5, lb_http),
   JS_CFUNC_DEF("httpAsync", 5, lb_http_async),
   JS_CFUNC_DEF("evalURL", 2, lb_eval_url),
   JS_CFUNC_DEF("evalCode", 3, lb_eval_code),
   JS_CFUNC_DEF("isCached", 1, lb_is_cached),
   JS_CFUNC_DEF("now", 0, lb_now),
   JS_CFUNC_DEF("log", 2, lb_log),
   JS_CFUNC_DEF("url", 0, lb_url),
   JS_CFUNC_DEF("navigate", 1, lb_navigate),
   JS_CFUNC_DEF("viewport", 0, lb_viewport),
   JS_CFUNC_DEF("storage", 1, lb_storage),
   JS_CFUNC_DEF("cookie", 1, lb_cookie),
   JS_CFUNC_DEF("binding", 2, lb_binding),
   JS_CFUNC_DEF("redraw", 0, lb_redraw),
   JS_CFUNC_DEF("resolve", 2, lb_resolve),
   JS_CFUNC_DEF("random", 1, lb_random),
   JS_CFUNC_DEF("setURL", 1, lb_set_url),
};

/* Calls a prelude hook: globalThis[name](args...).  Returns the result
 * (caller frees) or JS_UNDEFINED. */
static JSValue js_hook(const char *name, int argc, JSValueConst *argv)
{
   if (!g_page.ctx) return JS_UNDEFINED;
   JSValue global = JS_GetGlobalObject(g_page.ctx);
   JSValue fn = JS_GetPropertyStr(g_page.ctx, global, name);
   JSValue r = JS_UNDEFINED;
   if (JS_IsFunction(g_page.ctx, fn)) {
      js_arm();
      r = JS_Call(g_page.ctx, fn, global, argc, argv);
      if (JS_IsException(r)) { js_report(g_page.ctx, name); r = JS_UNDEFINED; }
   }
   JS_FreeValue(g_page.ctx, fn);
   JS_FreeValue(g_page.ctx, global);
   js_run_jobs();
   return r;
}

static void js_close(void)
{
   if (g_page.ctx) JS_FreeContext(g_page.ctx);
   if (g_page.rt) JS_FreeRuntime(g_page.rt);
   g_page.ctx = NULL;
   g_page.rt = NULL;
}

/* "Uncaught (in promise)", as a browser console reports it. */
static void js_rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                         JS_BOOL is_handled, void *opaque)
{
   (void)promise; (void)opaque;
   if (is_handled) return;
   const char *msg = JS_ToCString(ctx, reason);
   JSValue stack = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED;
   const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : NULL;
   fprintf(stderr, "[luna-browser] uncaught (in promise): %s\n%s", msg ? msg : "?", st ? st : "");
   if (msg) JS_FreeCString(ctx, msg);
   if (st) JS_FreeCString(ctx, st);
   JS_FreeValue(ctx, stack);
}

/* ES modules: specifiers resolve against the importing module's URL or the page's
 * import map; the source is fetched when first imported. */
struct imap_entry { char *spec, *url; };
static struct imap_entry *g_imap;
static int g_nimap;

static void importmap_clear(void)
{
   for (int i = 0; i < g_nimap; ++i) { free(g_imap[i].spec); free(g_imap[i].url); }
   free(g_imap);
   g_imap = NULL; g_nimap = 0;
}

/* <script type="importmap">{"imports":{"name":"url", "prefix/":"url/"}}</script> */
static void importmap_add(const char *json)
{
   struct jv *root = json_parse(json);
   const struct jv *imports = jget(root, "imports");
   if (imports && imports->type == J_OBJ)
      for (const struct jv *k = imports->kid; k; k = k->next) {
         if (!k->key || k->type != J_STR) continue;
         struct imap_entry *g = realloc(g_imap, sizeof *g * (size_t)(g_nimap + 1));
         if (!g) break;
         g_imap = g;
         g_imap[g_nimap].spec = lb_strdup(k->key);
         g_imap[g_nimap].url = url_resolve(g_page.url, k->str);
         g_nimap++;
      }
   jv_free(root);
}

static char *lb_mod_normalize(JSContext *ctx, const char *base, const char *name, void *opaque)
{
   (void)opaque;
   char *out = NULL;
   if (strstr(name, "://") || name[0] == '/' || !strncmp(name, "./", 2) || !strncmp(name, "../", 3)) {
      out = url_resolve(strstr(base, "://") ? base : g_page.url, name);
   } else {
      for (int i = 0; i < g_nimap && !out; ++i)
         if (!strcmp(g_imap[i].spec, name)) out = lb_strdup(g_imap[i].url);
      for (int i = 0; i < g_nimap && !out; ++i) {
         size_t l = strlen(g_imap[i].spec);
         if (l && g_imap[i].spec[l - 1] == '/' && !strncmp(name, g_imap[i].spec, l)) {
            struct buf b = { 0 };
            buf_puts(&b, g_imap[i].url);
            buf_puts(&b, name + l);
            out = buf_take(&b);
         }
      }
   }
   if (!out) out = lb_strdup(name);
   char *r = js_strdup(ctx, out);
   free(out);
   return r;
}

/* A module's static imports are found in its source and fetched together, level
 * by level, into the page cache: a graph of dozens of modules costs its depth in
 * round trips, not its size. */
static char **g_modscan;
static int g_nmodscan;

static int modscan_seen(const char *url)
{
   for (int i = 0; i < g_nmodscan; ++i) if (!strcmp(g_modscan[i], url)) return 1;
   char **g = realloc(g_modscan, sizeof *g * (size_t)(g_nmodscan + 1));
   if (!g) return 1;
   g_modscan = g;
   g_modscan[g_nmodscan++] = lb_strdup(url);
   return 0;
}

static void modscan_clear(void)
{
   for (int i = 0; i < g_nmodscan; ++i) free(g_modscan[i]);
   free(g_modscan);
   g_modscan = NULL; g_nmodscan = 0;
}

/* The literal specifiers of `from "x"` and `import "x"` in source. */
static void module_specs(const char *src, size_t n, const char *base, char ***out, int *count)
{
   for (size_t i = 0; i + 6 < n; ++i) {
      /* Import-like text inside comments or strings is not a dependency. */
      if (src[i] == '/' && src[i + 1] == '/') {
         while (i < n && src[i] != '\n') i++;
         continue;
      }
      if (src[i] == '/' && src[i + 1] == '*') {
         i += 2;
         while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
         i++;
         continue;
      }
      if (src[i] == '\'' || src[i] == '"' || src[i] == '`') {
         char quote = src[i++];
         while (i < n && src[i] != quote) { if (src[i] == '\\' && i + 1 < n) i++; i++; }
         continue;
      }
      size_t k;
      if (!strncmp(src + i, "from", 4) && (i == 0 || !isalnum((unsigned char)src[i - 1]))) k = i + 4;
      else if (!strncmp(src + i, "import", 6) && (i == 0 || !isalnum((unsigned char)src[i - 1]))) k = i + 6;
      else continue;
      while (k < n && isspace((unsigned char)src[k])) k++;
      if (k >= n || (src[k] != '"' && src[k] != '\'')) continue;
      char q = src[k++];
      size_t e = k;
      while (e < n && src[e] != q && src[e] != '\n' && e - k < 1000) e++;
      if (e >= n || src[e] != q || e == k) continue;
      char *spec = lb_strndup(src + k, e - k);
      char *abs = spec ? lb_mod_normalize(g_page.ctx, base, spec, NULL) : NULL;
      free(spec);
      if (!abs) continue;
      if (strstr(abs, "://")) {
         char **g = realloc(*out, sizeof **out * (size_t)(*count + 1));
         if (g) { *out = g; (*out)[(*count)++] = lb_strdup(abs); }
      }
      js_free(g_page.ctx, abs);
      i = e;
   }
}

static void module_prefetch(const char *root_url, const unsigned char *root_src, size_t root_len)
{
   char **queue = NULL;
   int nq = 0;
   if (modscan_seen(root_url)) return;
   module_specs((const char *)root_src, root_len, root_url, &queue, &nq);
   LB_LOG("module imports of %s: %d\n", root_url, nq);
   while (nq > 0) {
      struct pf_job *jobs = calloc((size_t)nq, sizeof *jobs);
      if (!jobs) {
         for (int i = 0; i < nq; ++i) free(queue[i]);
         free(queue);
         return;  /* the module loader can still fetch imports on demand */
      }
      int nj = 0;
      for (int i = 0; i < nq; ++i) {
         if (cache_find(queue[i]) >= 0) { /* known: still walk its imports below */ }
         else {
            int duplicate = 0;
            for (int j = 0; j < nj; ++j) if (!strcmp(jobs[j].url, queue[i])) { duplicate = 1; break; }
            if (!duplicate) { jobs[nj].url = lb_strdup(queue[i]); if (jobs[nj].url) nj++; }
         }
      }
      /* A slow import must not stop the next batch of otherwise ready work.
       * Refill each worker from the queue as soon as its request finishes. */
      struct pf_pool pool = { jobs, nj, 0 };
      pthread_t th[PF_THREADS];
      int started = 0;
      for (int i = 1; i < nj && i < PF_THREADS; ++i)
         if (pthread_create(&th[started], NULL, pf_worker, &pool) == 0) started++;
      pf_worker(&pool);
      for (int i = 0; i < started; ++i) pthread_join(th[i], NULL);
      for (int i = 0; i < nj; ++i) {
         if (jobs[i].r.body && jobs[i].r.status >= 200 && jobs[i].r.status < 300) {
            cache_add(jobs[i].url, jobs[i].r.body, jobs[i].r.length);
            jobs[i].url = NULL; jobs[i].r.body = NULL;
         } else {
            cache_add(jobs[i].url, NULL, 0);
            jobs[i].url = NULL;
         }
         free(jobs[i].url);
         response_free(&jobs[i].r);
      }
      free(jobs);
      char **next = NULL;
      int nn = 0;
      for (int i = 0; i < nq; ++i) {
         int hit = modscan_seen(queue[i]) ? -1 : cache_find(queue[i]);
         if (hit >= 0 && g_page.cache[hit].data)
            module_specs((const char *)g_page.cache[hit].data, g_page.cache[hit].len, queue[i], &next, &nn);
         free(queue[i]);
      }
      free(queue);
      queue = next; nq = nn;
   }
   free(queue);
}

static JSModuleDef *lb_mod_load(JSContext *ctx, const char *name, void *opaque)
{
   (void)opaque;
   if (!strstr(name, "://")) {
      JS_ThrowReferenceError(ctx, "cannot resolve module '%s'", name);
      return NULL;
   }
   struct response r;
   int hit = cache_find(name), ok;
   if (hit >= 0) {
      memset(&r, 0, sizeof r);
      r.body = g_page.cache[hit].data ? (unsigned char *)lb_strndup((const char *)g_page.cache[hit].data, g_page.cache[hit].len) : NULL;
      r.length = g_page.cache[hit].len;
      ok = r.body != NULL;
   } else ok = lb_fetch(name, "GET", NULL, NULL, 0, &r) == 0 && r.status >= 200 && r.status < 300;
   LB_LOG("module %s -> %d\n", name, r.status);
   JSModuleDef *m = NULL;
   if (ok) module_prefetch(name, r.body, r.length);
   if (!ok) JS_ThrowReferenceError(ctx, "could not load module '%s' (%d %s)", name, r.status, r.error ? r.error : "");
   else {
      JSValue f = JS_Eval(ctx, (const char *)r.body, r.length, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
      if (!JS_IsException(f)) {
         m = JS_VALUE_GET_PTR(f);
         JSValue meta = JS_GetImportMeta(ctx, m);
         JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, name), JS_PROP_C_W_E);
         JS_FreeValue(ctx, meta);
      }
      JS_FreeValue(ctx, f);
   }
   response_free(&r);
   return m;
}

static int js_open(void)
{
   js_close();
   g_page.rt = JS_NewRuntime();
   if (!g_page.rt) return -1;
   JS_SetMaxStackSize(g_page.rt, 4u << 20);
   JS_SetInterruptHandler(g_page.rt, js_interrupt, NULL);
   JS_SetModuleLoaderFunc(g_page.rt, lb_mod_normalize, lb_mod_load, NULL);
   importmap_clear();
   modscan_clear();
   JS_SetHostPromiseRejectionTracker(g_page.rt, js_rejection, NULL);
   g_page.ctx = JS_NewContext(g_page.rt);
   if (!g_page.ctx) return -1;
   JSValue global = JS_GetGlobalObject(g_page.ctx);
   JSValue lb = JS_NewObject(g_page.ctx);
   JS_SetPropertyFunctionList(g_page.ctx, lb, lb_funcs, (int)(sizeof lb_funcs / sizeof lb_funcs[0]));
   JS_SetPropertyStr(g_page.ctx, lb, "userAgent", JS_NewString(g_page.ctx, g_user_agent));
   JS_SetPropertyStr(g_page.ctx, lb, "verbose", JS_NewBool(g_page.ctx, g_verbose));
   JS_SetPropertyStr(g_page.ctx, global, "__lb", lb);
   JS_FreeValue(g_page.ctx, global);
   if (js_eval(lb_prelude, sizeof lb_prelude - 1, "luna-browser.js", 0) != 0) return -1;
   if (!atomic_load(&g_network_online)) {
      const char *code = "Object.defineProperty(navigator,'onLine',{value:false,configurable:true});";
      if (js_eval(code, strlen(code), "network", 0) != 0) return -1;
   }
   for (int i = 0; i < g_page.nbindings; ++i) {
      JSValue a = JS_NewString(g_page.ctx, g_page.bindings[i]);
      JS_FreeValue(g_page.ctx, js_hook("__lb_add_binding", 1, &a));
      JS_FreeValue(g_page.ctx, a);
   }
   return 0;
}

/* ======================================================================== *
 * Loading a document
 * ======================================================================== */

struct script_tag { char *src, *code; int module, ordinal; };

/* Execute the document's scripts once in source order, after its DOM exists. */
static int scan_scripts(const char *html, struct script_tag **out)
{
   int n = 0, ordinal = 0;
   *out = NULL;
   for (const char *p = html; (p = strchr(p, '<')) != NULL; ) {
      if (!strncmp(p, "<!--", 4)) {
         const char *e = strstr(p, "-->");
         p = e ? e + 3 : p + strlen(p);
         continue;
      }
      const char *raw_tags[] = {"style", "title", "textarea", "iframe", "noscript"};
      int skipped = 0;
      for (size_t k = 0; k < sizeof(raw_tags) / sizeof(raw_tags[0]); ++k) {
         size_t len = strlen(raw_tags[k]);
         if (strncasecmp(p + 1, raw_tags[k], len) ||
             (p[len + 1] != '>' && !isspace((unsigned char)p[len + 1]))) continue;
         char closing[32]; snprintf(closing, sizeof closing, "</%s", raw_tags[k]);
         const char *e = strcasestr(p + len + 1, closing);
         while (e && e[len + 2] != '>' && !isspace((unsigned char)e[len + 2])) e = strcasestr(e + 2, closing);
         p = e ? e + len + 2 : p + strlen(p);
         skipped = 1; break;
      }
      if (skipped) continue;
      if (strncasecmp(p, "<script", 7) || (p[7] != '>' && !isspace((unsigned char)p[7]))) { p++; continue; }
      const char *gt = strchr(p, '>');
      if (!gt) break;
      char *attrs = lb_strndup(p + 7, (size_t)(gt - p - 7));
      const char *close = strcasestr(gt + 1, "</script");
      while (close && close[8] != '>' && !isspace((unsigned char)close[8]))
         close = strcasestr(close + 2, "</script");
      const char *end = close ? close : gt + 1 + strlen(gt + 1);
      struct script_tag t = { 0 };
      t.ordinal = ordinal++;
      char type[64] = "", src[1024] = "";
      /* A tiny attribute read, same rules as luna-ui's (quoted values). */
      const char *a;
      if ((a = strcasestr(attrs, "type=")) && (a == attrs || isspace((unsigned char)a[-1]))) {
         a += 5;
         char q = (*a == '"' || *a == '\'') ? *a++ : ' ';
         size_t k = 0;
         while (*a && *a != q && *a != '>' && k + 1 < sizeof type) type[k++] = *a++;
         type[k] = 0;
      }
      if ((a = strcasestr(attrs, "src=")) && (a == attrs || isspace((unsigned char)a[-1]))) {
         a += 4;
         char q = (*a == '"' || *a == '\'') ? *a++ : ' ';
         size_t k = 0;
         while (*a && *a != q && *a != '>' && k + 1 < sizeof src) src[k++] = *a++;
         src[k] = 0;
      }
      free(attrs);
      if (!strcasecmp(type, "importmap")) {            /* before any module runs */
         char *map = lb_strndup(gt + 1, (size_t)(end - gt - 1));
         if (map) { importmap_add(map); free(map); }
         p = close ? close + 8 : end;
         continue;
      }
      int js = !type[0] || !strcasecmp(type, "text/javascript") ||
               !strcasecmp(type, "application/javascript") || !strcasecmp(type, "module") ||
               !strcasecmp(type, "text/ecmascript") || !strcasecmp(type, "application/x-javascript");
      if (js) {
         t.module = !strcasecmp(type, "module");
         if (src[0]) {
            decode_html_entities(src);
            t.src = lb_strdup(src);
         } else t.code = lb_strndup(gt + 1, (size_t)(end - gt - 1));
         struct script_tag *g = realloc(*out, sizeof *g * (size_t)(n + 1));
         if (g) { *out = g; (*out)[n++] = t; }
         else { free(t.src); free(t.code); }
      }
      p = close ? close + 8 : end;
   }
   return n;
}

/* The part of the HTML UA stylesheet luna-ui does not build in. */
static const char lb_ua_css[] =
   "head,script,style,template,noscript,input[type=hidden],[hidden]{display:none}"
   "input,textarea,select{border:1px solid #767676;border-radius:2px;padding:1px 2px;"
   "background:#ffffff;color:#000000;font-size:13.33px}"
   "button{border:1px solid #767676;border-radius:3px;padding:1px 6px;background:#efefef;"
   "color:#000000;font-size:13.33px}"
   "a{color:#0000ee;text-decoration:underline;cursor:pointer}"
   "b,strong,th,h4,h5,h6{font-weight:bold}"
   "i,em,cite,var,dfn{font-style:italic}"
   "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
   "h4{margin:21px 0}h5{font-size:13.28px;margin:22px 0}h6{font-size:10.72px;margin:24px 0}"
   "ul,ol{margin:16px 0;padding-left:40px}"
   "blockquote{margin:16px 40px}pre{white-space:pre;margin:13px 0}"
   "code,kbd,samp,pre,tt{font-family:monospace}"
   "small{font-size:13.33px}sub,sup{font-size:12px}"
   "hr{border-top:1px solid #9a9a9a;margin:8px 0}"
   "table{border-spacing:2px}";

static void page_teardown(void)
{
   g_page_gen++;              /* requests still in flight belong to the old page */
   for (int i = 0; i < g_ninflight; ++i) free(g_inflight[i]);
   g_ninflight = 0;
   js_close();
   luna_reset_document();
   luna_reset_css();
   for (int i = 0; i < g_page.ncache; ++i) { free(g_page.cache[i].url); free(g_page.cache[i].data); }
   free(g_page.cache);
   g_page.cache = NULL;
   g_page.ncache = 0;
   g_page.nev = 0;
   g_page.pressed = 0;
   g_page.ready = 0;
}

static double page_pump(void);
static int g_page_loading;

static void ready_state(int stage)
{
   g_page.ready = stage;
   JSValue a = JS_NewInt32(g_page.ctx, stage);
   JS_FreeValue(g_page.ctx, js_hook("__lb_ready", 1, &a));
}

static void page_load_html(const char *html, const char *url)
{
   double stage = lb_now_ms(), total = stage;
   g_page_loading++;
   page_teardown();
   free(g_page.nav_url);
   g_page.nav_url = NULL;
   free(g_page.url);
   g_page.url = lb_strdup(url && url[0] ? url : "about:blank");
   /* luna-ui joins relative references onto this base; lb_read_resource then
    * resolves the result against the page URL. */
   if (!strncmp(g_page.url, "file://", 7)) {
      struct url u;
      if (url_parse(g_page.url, &u) == 0) {
         char *path = percent_decode(u.path, strlen(u.path));
         luna_set_html_base_dir(path);
         free(path);
      }
      url_free(&u);
   } else luna_set_html_base_dir(g_page.url);
   snprintf(luna_doc_title, sizeof luna_doc_title, "%s", "");
   page_prefetch(html);
   LB_LOG("load prefetch %.0fms\n", lb_now_ms() - stage); stage = lb_now_ms();
   js_open();
   luna_parse_css(lb_ua_css);
   for (int i = 0; i < g_page.nboot; ++i) js_eval(g_page.boot_scripts[i], strlen(g_page.boot_scripts[i]), "boot", 0);
   /* HTML always has a body: content outside one belongs to it. */
   if (!strcasestr(html, "<body")) {
      struct buf wrapped = { 0 };
      buf_puts(&wrapped, "<body>");
      buf_puts(&wrapped, html);
      buf_puts(&wrapped, "</body>");
      luna_parse_html(wrapped.p ? wrapped.p : "<body></body>");
      free(wrapped.p);
   } else luna_parse_html(html);
   LB_LOG("load parse %.0fms (%d elements, %d rules)\n", lb_now_ms() - stage, elem_count, rule_count); stage = lb_now_ms();
   luna_inject_body_background();
   luna_resize((float)g_page.w, (float)g_page.h);
   struct script_tag *scripts = NULL;
   int n = scan_scripts(html, &scripts);
   JS_FreeValue(g_page.ctx, js_hook("__lb_parser_scripts", 0, NULL));
   for (int i = 0; i < n; ++i) {
      JSValue index = JS_NewInt32(g_page.ctx, scripts[i].module ? -1 : scripts[i].ordinal);
      JS_FreeValue(g_page.ctx, js_hook("__lb_current_script", 1, &index));
      JS_FreeValue(g_page.ctx, index);
      if (scripts[i].src) js_eval_url(scripts[i].src, scripts[i].module);
      else if (scripts[i].code) js_eval(scripts[i].code, strlen(scripts[i].code), g_page.url, scripts[i].module);
      free(scripts[i].src);
      free(scripts[i].code);
   }
   JSValue no_script = JS_NewInt32(g_page.ctx, -1);
   JS_FreeValue(g_page.ctx, js_hook("__lb_current_script", 1, &no_script));
   JS_FreeValue(g_page.ctx, no_script);
   free(scripts);
   ready_state(1);
   LB_LOG("load scripts %.0fms\n", lb_now_ms() - stage); stage = lb_now_ms();
   page_emit("Page.domContentEventFired", NULL);
   /* A browser paints a frame (animation callbacks, zero-delay timers) before the
    * load event, which waits for subresources.  Scripts such as Bing's bot check
    * schedule work for that frame and expect `load` to come after it. */
   for (int frame = 0; frame < 2; ++frame) {
      page_pump();
      struct timespec ts = { 0, 20 * 1000000L };
      nanosleep(&ts, NULL);
   }
   ready_state(2);
   g_page.dirty = 1;
   g_page_loading--;
   LB_LOG("load events %.0fms, total %.0fms\n", lb_now_ms() - stage, lb_now_ms() - total);
}

static int page_navigate(const char *url)
{
   struct response r;
   char *abs = url_resolve(g_page.url, url);
   t_fetch_navigation = 1;
   int rc = lb_fetch(abs, "GET", NULL, NULL, 0, &r);
   t_fetch_navigation = 0;
   LB_LOG("navigate %s -> %d %s\n", abs, r.status, r.error ? r.error : "");
   if (rc != 0 || !r.body) {
      struct buf b = { 0 };
      buf_puts(&b, "<body style=\"font-family:sans-serif;padding:24px\"><h2>This page could not be loaded</h2><p>");
      html_escape(&b, abs, 0);
      buf_puts(&b, "</p><p>");
      html_escape(&b, r.error ? r.error : "error", 0);
      buf_puts(&b, "</p></body>");
      page_load_html(b.p, abs);
      free(b.p);
   } else {
      char *ctype = header_get(r.headers, "content-type");
      if (ctype && !strcasestr(ctype, "html") && !strncasecmp(ctype, "text/", 5)) {
         struct buf b = { 0 };
         buf_puts(&b, "<body><pre>");
         html_escape(&b, (const char *)r.body, 0);
         buf_puts(&b, "</pre></body>");
         page_load_html(b.p, r.url ? r.url : abs);
         free(b.p);
      } else if (ctype && !strncasecmp(ctype, "image/", 6)) {
         struct buf b = { 0 };
         buf_puts(&b, "<body style=\"margin:0;background:#0e0e0e\"><img src=\"");
         html_escape(&b, r.url ? r.url : abs, 1);
         buf_puts(&b, "\"></body>");
         page_load_html(b.p, r.url ? r.url : abs);
         free(b.p);
      } else page_load_html((const char *)r.body, r.url ? r.url : abs);
      free(ctype);
   }
   response_free(&r);
   free(abs);
   return rc;
}

/* ======================================================================== *
 * Input: luna-ui handles the widget behaviour (focus, carets, scrolling),
 * the page's scripts see DOM events for the same input
 * ======================================================================== */

static void hook_press(int hit, int button, int mods)
{
   (void)mods;
   if (g_page.nev < 16) g_page.ev[g_page.nev++] = (struct lb_event){ 1, hit, button, 0 };
}

static void hook_release(int hit, int drag_moved)
{
   if (g_page.nev < 16) g_page.ev[g_page.nev++] = (struct lb_event){ 2, hit, 0, drag_moved };
}

static unsigned node_uid_at(int idx)
{
   while (idx >= 0 && (!el_node(idx) || elements[idx].anon_text)) idx = elements[idx].parent_idx;
   return idx >= 0 ? elements[idx].dom_uid : 0u;
}

static unsigned g_focus_uid;
static char g_focus_value[LUNA_UI_TEXT_CAP];

/* Focus and value changes luna-ui made while handling input become
 * focus/blur and input events. */
static void sync_focus_and_value(void)
{
   if (!g_page.ctx) return;
   unsigned now = node_uid_at(luna_focused_element());
   if (now != g_focus_uid) {
      JSValue a[2] = { JS_NewUint32(g_page.ctx, g_focus_uid), JS_NewUint32(g_page.ctx, now) };
      g_focus_uid = now;
      const char *v = luna_get_value(luna_focused_element());
      snprintf(g_focus_value, sizeof g_focus_value, "%s", v ? v : "");
      JS_FreeValue(g_page.ctx, js_hook("__lb_focus", 2, a));
      return;
   }
   if (!now) return;
   const char *v = luna_get_value(luna_dom_find(now));
   if (v && strcmp(v, g_focus_value)) {
      snprintf(g_focus_value, sizeof g_focus_value, "%s", v);
      JSValue a = JS_NewUint32(g_page.ctx, now);
      JS_FreeValue(g_page.ctx, js_hook("__lb_input", 1, &a));
      g_page.dirty = 1;
   }
}

static void flush_pointer_events(double x, double y, int touch)
{
   int n = g_page.nev;
   struct lb_event ev[16];
   memcpy(ev, g_page.ev, sizeof ev[0] * (size_t)n);
   g_page.nev = 0;
   for (int i = 0; i < n && g_page.ctx; ++i) {
      JSValue a[7] = {
         JS_NewInt32(g_page.ctx, ev[i].kind), JS_NewUint32(g_page.ctx, node_uid_at(ev[i].hit)),
         JS_NewFloat64(g_page.ctx, x), JS_NewFloat64(g_page.ctx, y),
         JS_NewInt32(g_page.ctx, ev[i].button), JS_NewBool(g_page.ctx, ev[i].drag_moved),
         JS_NewBool(g_page.ctx, touch) };
      JS_FreeValue(g_page.ctx, js_hook("__lb_pointer", 7, a));
   }
   sync_focus_and_value();
}

static void cdp_emit(const char *method, const char *params);
static char *g_page_clip;            /* the clipboard as the page process knows it */

/* CDP modifiers (alt 1, ctrl 2, meta 4, shift 8) as luna-ui's. */
static int cdp_mods(int m)
{
   return ((m & 1) ? LUNA_MOD_ALT : 0) | ((m & 2) ? LUNA_MOD_CONTROL : 0) |
          ((m & 4) ? LUNA_MOD_SUPER : 0) | ((m & 8) ? LUNA_MOD_SHIFT : 0);
}

/* action: 1 down, 0 up, -1 move. */
static int g_page_cursor;
static void page_set_cursor(int type) { g_page_cursor = type; }

static void page_pointer(double x, double y, int action, int touch, int button, int mods)
{
   luna_mouse_move(x, y);
   if (action < 0) {
      if (g_page.pressed && g_page.ctx) {
         JSValue a[7] = { JS_NewInt32(g_page.ctx, 3), JS_NewUint32(g_page.ctx, node_uid_at((luna_dom_flush_restyle(), luna_element_at_point(x, y)))),
                          JS_NewFloat64(g_page.ctx, x), JS_NewFloat64(g_page.ctx, y),
                          JS_NewInt32(g_page.ctx, 0), JS_FALSE, JS_NewBool(g_page.ctx, touch) };
         JS_FreeValue(g_page.ctx, js_hook("__lb_pointer", 7, a));
      }
      /* The link under the pointer, for the window's status line. */
      {
         static char *last;
         char *href = (luna_dom_flush_restyle(), luna_link_at_point(x, y));
         if ((href ? 1 : 0) != (last ? 1 : 0) || (href && strcmp(href, last))) {
            struct buf b = { 0 };
            buf_puts(&b, "{\"url\":");
            buf_json_str(&b, href ? href : "");
            buf_puts(&b, "}");
            cdp_emit("Lunaria.hover", b.p);
            free(b.p);
            free(last);
            last = href;
         } else free(href);
      }
      /* The cursor the page under the pointer asks for.  Sent while it is not the
       * arrow too: the window's own hover handling resets it on re-entry. */
      {
         static int sent;
         if (g_page_cursor != sent || g_page_cursor) {
            char c[40];
            snprintf(c, sizeof c, "{\"type\":%d}", g_page_cursor);
            cdp_emit("Lunaria.cursor", c);
            sent = g_page_cursor;
         }
      }
      g_page.dirty = 1;
      return;
   }
   if (button == LUNA_MOUSE_BUTTON_LEFT) g_page.pressed = action == 1;
   luna_mouse_button(button, action ? LUNA_PRESS : LUNA_RELEASE, mods, x, y);
   flush_pointer_events(x, y, touch);
   g_page.dirty = 1;
}

static void page_wheel(double x, double y, double dx, double dy)
{
   luna_mouse_move(x, y);
   /* CDP deltas are pixels; luna-ui's wheel unit is a notch. */
   luna_scroll(-dx / 18.0, -dy / 18.0);
   if (g_page.ctx) {
      JSValue a[5] = { JS_NewUint32(g_page.ctx, node_uid_at((luna_dom_flush_restyle(), luna_element_at_point(x, y)))),
                       JS_NewFloat64(g_page.ctx, x), JS_NewFloat64(g_page.ctx, y),
                       JS_NewFloat64(g_page.ctx, dx), JS_NewFloat64(g_page.ctx, dy) };
      JS_FreeValue(g_page.ctx, js_hook("__lb_wheel", 5, a));
   }
   g_page.dirty = 1;
}

static void page_text(const char *utf8)
{
   const unsigned char *p = (const unsigned char *)utf8;
   while (*p) {
      unsigned cp = *p, n = 0;
      if (cp >= 0xf0) { cp &= 7; n = 3; } else if (cp >= 0xe0) { cp &= 15; n = 2; }
      else if (cp >= 0xc0) { cp &= 31; n = 1; }
      p++;
      while (n-- && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 63u);
      char one[5] = { 0 };
      int ol = 0;
      if (cp < 0x80) one[ol++] = (char)cp;
      else if (cp < 0x800) { one[ol++] = (char)(0xc0 | cp >> 6); one[ol++] = (char)(0x80 | (cp & 63)); }
      else if (cp < 0x10000) { one[ol++] = (char)(0xe0 | cp >> 12); one[ol++] = (char)(0x80 | ((cp >> 6) & 63)); one[ol++] = (char)(0x80 | (cp & 63)); }
      else { one[ol++] = (char)(0xf0 | cp >> 18); one[ol++] = (char)(0x80 | ((cp >> 12) & 63)); one[ol++] = (char)(0x80 | ((cp >> 6) & 63)); one[ol++] = (char)(0x80 | (cp & 63)); }
      int cancelled = 0;
      if (g_page.ctx && cp > 0x20) {         /* the key events around a typed character */
         JSValue a[2] = { JS_NewString(g_page.ctx, one), JS_NewInt32(g_page.ctx, 0) };
         JSValue r = js_hook("__lb_typed", 2, a);
         cancelled = JS_ToBool(g_page.ctx, r);
         JS_FreeValue(g_page.ctx, r); JS_FreeValue(g_page.ctx, a[0]);
      }
      if (!cancelled) luna_char(cp);
      sync_focus_and_value();
      if (g_page.ctx && cp > 0x20) {
         JSValue a[2] = { JS_NewString(g_page.ctx, one), JS_NewInt32(g_page.ctx, 1) };
         JS_FreeValue(g_page.ctx, js_hook("__lb_typed", 2, a));
         JS_FreeValue(g_page.ctx, a[0]);
      }
   }
   sync_focus_and_value();
   g_page.dirty = 1;
}

static const struct { const char *name; int key; int vk; } g_keys[] = {
   { "Backspace", LUNA_KEY_BACKSPACE, 8 }, { "Tab", LUNA_KEY_TAB, 9 },
   { "Enter", LUNA_KEY_ENTER, 13 }, { "Escape", LUNA_KEY_ESCAPE, 27 },
   { "PageUp", LUNA_KEY_PAGE_UP, 33 }, { "PageDown", LUNA_KEY_PAGE_DOWN, 34 },
   { "End", LUNA_KEY_END, 35 }, { "Home", LUNA_KEY_HOME, 36 },
   { "ArrowLeft", LUNA_KEY_LEFT, 37 }, { "ArrowUp", LUNA_KEY_UP, 38 },
   { "ArrowRight", LUNA_KEY_RIGHT, 39 }, { "ArrowDown", LUNA_KEY_DOWN, 40 },
   { "Delete", LUNA_KEY_DELETE, 46 }, { "Insert", LUNA_KEY_INSERT, 45 }, { " ", LUNA_KEY_SPACE, 32 }, { NULL, 0, 0 },
};

/* One key transition.  The page's keydown can cancel the default action,
 * which is luna-ui's editing of the focused field. */
static void page_key(const char *key, int vk, int down, const char *text, int mods)
{
   int lkey = LUNA_KEY_UNKNOWN;
   /* Shortcuts (Ctrl+A, Ctrl+C ...): the letter itself is the key. */
   if ((mods & (LUNA_MOD_CONTROL | LUNA_MOD_SUPER)) && key && key[0] && !key[1] && isalpha((unsigned char)key[0]))
      lkey = toupper((unsigned char)key[0]);
   for (int i = 0; g_keys[i].name; ++i)
      if ((key && !strcmp(key, g_keys[i].name)) || (vk && vk == g_keys[i].vk)) {
         lkey = g_keys[i].key;
         if (!key || !key[0]) key = g_keys[i].name;
         if (!vk) vk = g_keys[i].vk;
         break;
      }
   int prevented = 0;
   if (g_page.ctx) {
      JSValue a[3] = { JS_NewString(g_page.ctx, down ? "keydown" : "keyup"),
                       JS_NewString(g_page.ctx, key ? key : ""), JS_NewInt32(g_page.ctx, vk) };
      JSValue r = js_hook("__lb_key", 3, a);
      prevented = JS_ToBool(g_page.ctx, r) > 0;
      JS_FreeValue(g_page.ctx, r);
      for (int i = 0; i < 3; ++i) JS_FreeValue(g_page.ctx, a[i]);
   }
   if (!prevented) {
      if (lkey != LUNA_KEY_UNKNOWN) luna_key(lkey, 0, down ? LUNA_PRESS : LUNA_RELEASE, mods);
      else if (down && text && text[0]) page_text(text);
   }
   sync_focus_and_value();
   g_page.dirty = 1;
}

/* ======================================================================== *
 * Frames
 * ======================================================================== */

/* Runs due timers and animation callbacks and any navigation the page asked
 * for.  Returns milliseconds until the next timer (-1: none). */
static void cdp_navigate_now(const char *url);

static double page_pump(void)
{
   if (!g_page_loading && g_page.nav_url) {             /* the page's own navigation (a link, location) */
      char *u = g_page.nav_url;
      g_page.nav_url = NULL;
      cdp_navigate_now(u);
      free(u);
   }
   http_drain();
   if (!g_page.ctx) return -1;
   JSValue a = JS_NewFloat64(g_page.ctx, lb_now_ms());
   JSValue r = js_hook("__lb_tick", 1, &a);
   double next = -1;
   JS_ToFloat64(g_page.ctx, &next, r);
   JS_FreeValue(g_page.ctx, r);
   sync_focus_and_value();
   return next;
}

static int fb_w(void) { return (int)((float)g_page.w * g_page.scale + 0.5f); }
static int fb_h(void) { return (int)((float)g_page.h * g_page.scale + 0.5f); }

static void page_render(void)
{
   if (gl_surface(fb_w(), fb_h()) != 0) return;
   luna_resize((float)g_page.w, (float)g_page.h);
   double now = lb_now_ms() / 1000.0;
   double dt = g_page.last_frame > 0 ? now - g_page.last_frame : 0.0;
   g_page.last_frame = now;
   luna_update(now, dt);
   g_gl.viewport(0, 0, fb_w(), fb_h());
   g_gl.clear_color(1.0f, 1.0f, 1.0f, 1.0f);
   g_gl.clear(GL_COLOR_BUFFER_BIT);
   luna_set_preserve_backdrop(1);
   luna_render(fb_w(), fb_h());
   /* LUNA_BROWSER_DUMP_BOXES=1: every painted box, for layout debugging. */
   if (getenv("LUNA_BROWSER_DUMP_BOXES")) {
      for (int i = 0; i < elem_count; ++i) {
         const LunaElement *e = &elements[i];
         if (!is_visible(i)) continue;
         fprintf(stderr, "[box] %d %s#%s.%s @%.0f,%.0f %.0fx%.0f bg=%.2f,%.2f,%.2f,%.2f z=%d text='%.30s'\n",
                 i, e->type, e->id, e->class_name, e->x, e->y, e->w, e->h,
                 e->cur_r, e->cur_g, e->cur_b, e->cur_a, e->z_index, e->text);
      }
   }
   if (g_gl.finish) g_gl.finish();
   g_page.dirty = 0;
}

static void png_sink(void *ctx, void *data, int size) { buf_put(ctx, data, (size_t)size); }

#define CAST_MS 16          /* ~60 frames a second while things move */
#define CAST_CARET_MS 500   /* a focused field's caret alone */
static struct { int on, acked, quality, seq; double last_ms; } g_cast;

/* A screenshot/screencast "quality" (JPEG), 80 when not given. */
static int cast_quality(const struct jv *params)
{
   double q = params ? jnum(params, "quality", 80) : 80;
   return q < 1 ? 1 : q > 100 ? 100 : (int)q;
}

/* The current frame as PNG, or JPEG when quality > 0 (owned bytes). */
static unsigned char *page_capture(size_t *len, int jpeg_quality)
{
   *len = 0;
   page_render();
   int w = fb_w(), h = fb_h();
   unsigned char *rgba = malloc((size_t)w * (size_t)h * 4);
   unsigned char *rgb = malloc((size_t)w * (size_t)h * 3);
   if (!rgba || !rgb) { free(rgba); free(rgb); return NULL; }
   if (g_gl.pixel_storei) g_gl.pixel_storei(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
   g_gl.read_pixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
   for (int y = 0; y < h; ++y) {
      const unsigned char *src = rgba + (size_t)(h - 1 - y) * (size_t)w * 4;
      unsigned char *dst = rgb + (size_t)y * (size_t)w * 3;
      for (int x = 0; x < w; ++x) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; src += 4; dst += 3; }
   }
   free(rgba);
   struct buf b = { 0 };
   if (jpeg_quality > 0) stbi_write_jpg_to_func(png_sink, &b, w, h, 3, rgb, jpeg_quality);
   else stbi_write_png_to_func(png_sink, &b, w, h, 3, rgb, w * 3);
   free(rgb);
   *len = b.len;
   return (unsigned char *)b.p;
}

/* ======================================================================== *
 * Chrome DevTools Protocol over the pipe
 * ======================================================================== */

#define TARGET_ID "5A1E9A2F0D7B4C3E9F1A2B3C4D5E6F70"
#define SESSION_ID "L1B2S3E4S5S6I7O8N9A0B1C2D3E4F5A6"
#define FRAME_ID TARGET_ID

static int g_cdp_in = 3, g_cdp_out = 4;
static int g_attached;
static unsigned g_loader_seq;

static void cdp_write(const char *json)
{
   size_t n = strlen(json) + 1, off = 0;   /* the NUL is the frame delimiter */
   while (off < n) {
      ssize_t w = write(g_cdp_out, json + off, n - off);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) { fprintf(stderr, "luna-browser: pipe closed\n"); exit(0); }
      off += (size_t)w;
   }
}

static void cdp_event_session(const char *method, const char *params, int session)
{
   struct buf b = { 0 };
   buf_printf(&b, "{\"method\":\"%s\",\"params\":%s", method, params);
   if (session) buf_puts(&b, ",\"sessionId\":\"" SESSION_ID "\"");
   buf_puts(&b, "}");
   cdp_write(b.p);
   free(b.p);
}

static void cdp_emit(const char *method, const char *params)
{
   if (!g_attached) return;
   char tmp[128];
   if (!strcmp(params, "{}") && !strncmp(method, "Page.", 5)) {
      snprintf(tmp, sizeof tmp, "{\"timestamp\":%.6f,\"frameId\":\"" FRAME_ID "\"}", lb_now_ms() / 1000.0);
      params = tmp;
   }
   cdp_event_session(method, params, 1);
}

static void cdp_reply(double id, int session, const char *result)
{
   struct buf b = { 0 };
   buf_printf(&b, "{\"id\":%.0f,\"result\":%s", id, result ? result : "{}");
   if (session) buf_puts(&b, ",\"sessionId\":\"" SESSION_ID "\"");
   buf_puts(&b, "}");
   cdp_write(b.p);
   free(b.p);
}

static void cdp_error(double id, int session, int code, const char *message)
{
   struct buf b = { 0 };
   buf_printf(&b, "{\"id\":%.0f,\"error\":{\"code\":%d,\"message\":", id, code);
   buf_json_str(&b, message);
   buf_puts(&b, "}");
   if (session) buf_puts(&b, ",\"sessionId\":\"" SESSION_ID "\"");
   buf_puts(&b, "}");
   cdp_write(b.p);
   free(b.p);
}

static void target_info(struct buf *b)
{
   buf_puts(b, "{\"targetId\":\"" TARGET_ID "\",\"type\":\"page\",\"title\":");
   buf_json_str(b, luna_doc_title);
   buf_puts(b, ",\"url\":");
   buf_json_str(b, g_page.url ? g_page.url : "about:blank");
   buf_printf(b, ",\"attached\":%s,\"canAccessOpener\":false,\"browserContextId\":\"LB-CONTEXT\"}",
              g_attached ? "true" : "false");
}

static void frame_info(struct buf *b)
{
   char *origin = url_origin(g_page.url ? g_page.url : "about:blank");
   buf_printf(b, "{\"id\":\"" FRAME_ID "\",\"loaderId\":\"LB-LOADER-%u\",\"url\":", g_loader_seq);
   buf_json_str(b, g_page.url ? g_page.url : "about:blank");
   buf_puts(b, ",\"securityOrigin\":");
   buf_json_str(b, origin);
   buf_puts(b, ",\"mimeType\":\"text/html\",\"domainAndRegistry\":\"\",\"secureContextType\":\"Secure\","
               "\"crossOriginIsolatedContextType\":\"NotIsolated\",\"gatedAPIFeatures\":[]}");
   free(origin);
}

static void cdp_navigated(void)
{
   struct buf b = { 0 };
   buf_puts(&b, "{\"frame\":");
   frame_info(&b);
   buf_puts(&b, ",\"type\":\"Navigation\"}");
   cdp_emit("Page.frameNavigated", b.p);
   free(b.p);
}

/* Runtime.evaluate: the prelude turns the completion value into a
 * RemoteObject. */
static char *cdp_evaluate(const char *expr, int by_value, int await_promise)
{
   JSContext *ctx = g_page.ctx;
   if (!ctx) return lb_strdup("{\"result\":{\"type\":\"undefined\"}}");
   JSValue v = JS_Eval(ctx, expr, strlen(expr), "<evaluate>", JS_EVAL_TYPE_GLOBAL);
   int threw = JS_IsException(v);
   if (threw) v = JS_GetException(ctx);
   if (!threw && await_promise && JS_IsObject(v) && (int)JS_PromiseState(ctx, v) >= 0) {
      double deadline = lb_now_ms() + 10000;
      while (JS_PromiseState(ctx, v) == JS_PROMISE_PENDING && lb_now_ms() < deadline) {
         js_run_jobs();
         double next = page_pump();
         if (JS_PromiseState(ctx, v) != JS_PROMISE_PENDING) break;
         struct timespec ts = { 0, (long)((next >= 0 && next < 5 ? next : 5) * 1e6) };
         nanosleep(&ts, NULL);
      }
      JSPromiseStateEnum st = JS_PromiseState(ctx, v);
      JSValue res = JS_PromiseResult(ctx, v);
      JS_FreeValue(ctx, v);
      v = res;
      threw = st == JS_PROMISE_REJECTED;
   }
   js_run_jobs();
   JSValue a[3] = { v, JS_NewBool(ctx, by_value), JS_NewBool(ctx, threw) };
   JSValue r = js_hook("__lb_remote", 3, a);
   JS_FreeValue(ctx, v);
   const char *s = JS_ToCString(ctx, r);
   char *out = lb_strdup(s ? s : "{\"result\":{\"type\":\"undefined\"}}");
   if (s) JS_FreeCString(ctx, s);
   JS_FreeValue(ctx, r);
   return out;
}

static void set_viewport(int w, int h, float scale)
{
   if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
   if (!(scale > 0.0f) || scale > 8.0f) scale = 1.0f;
   if (w == g_page.w && h == g_page.h && scale == g_page.scale) return;
   g_page.w = w;
   g_page.h = h;
   g_page.scale = scale;
   luna_resize((float)w, (float)h);
   if (g_page.ctx) JS_FreeValue(g_page.ctx, js_hook("__lb_resize", 0, NULL));
   g_page.dirty = 1;
}

static void cdp_navigate_now(const char *url)
{
   g_loader_seq++;
   cdp_emit("Page.frameStartedLoading", "{}");
   page_navigate(url);
   cdp_navigated();
   cdp_emit("Page.loadEventFired", "{}");
   cdp_emit("Page.frameStoppedLoading", "{}");
}

/* One CDP Network.CookieParam into the jar. */
static void cdp_set_cookie(const struct jv *c)
{
   const char *name = jstr(c, "name"), *value = jstr(c, "value");
   if (!name || !value) return;
   const char *url = jstr(c, "url"), *domain = jstr(c, "domain"), *path = jstr(c, "path");
   struct buf line = { 0 }, where = { 0 };
   buf_printf(&line, "%s=%s", name, value);
   if (domain && domain[0]) buf_printf(&line, "; Domain=%s", domain);
   buf_printf(&line, "; Path=%s", path && path[0] ? path : "/");
   if (jbool(c, "secure")) buf_puts(&line, "; Secure");
   if (url) buf_puts(&where, url);
   else buf_printf(&where, "https://%s/", domain ? (domain[0] == '.' ? domain + 1 : domain) : "localhost");
   cookie_set(where.p, line.p, 0);
   free(line.p);
   free(where.p);
}

static void cdp_cookie_list(struct buf *b, const struct jv *urls)
{
   pthread_mutex_lock(&g_cookie_lock);
   buf_puts(b, "{\"cookies\":[");
   int first = 1;
   for (int i = 0; i < g_ncookies; ++i) {
      const struct cookie *c = &g_cookies[i];
      int want = !urls || urls->type != J_ARR;
      for (const struct jv *u = urls && urls->type == J_ARR ? urls->kid : NULL; u && !want; u = u->next) {
         struct url pu;
         if (u->type == J_STR && url_parse(u->str, &pu) == 0 && pu.host && domain_match(pu.host, c)) want = 1;
         url_free(&pu);
      }
      if (!want) continue;
      if (!first) buf_puts(b, ",");
      first = 0;
      buf_puts(b, "{\"name\":");
      buf_json_str(b, c->name);
      buf_puts(b, ",\"value\":");
      buf_json_str(b, c->value);
      buf_puts(b, ",\"domain\":");
      if (c->host_only) buf_json_str(b, c->domain);
      else { struct buf d = { 0 }; buf_printf(&d, ".%s", c->domain); buf_json_str(b, d.p); free(d.p); }
      buf_puts(b, ",\"path\":");
      buf_json_str(b, c->path);
      buf_printf(b, ",\"expires\":-1,\"size\":%zu,\"httpOnly\":false,\"secure\":%s,"
                 "\"session\":true,\"priority\":\"Medium\",\"sameParty\":false,"
                 "\"sourceScheme\":\"Secure\",\"sourcePort\":443}",
                 strlen(c->name) + strlen(c->value), c->secure ? "true" : "false");
   }
   buf_puts(b, "]}");
   pthread_mutex_unlock(&g_cookie_lock);
}

static void cdp_delete_cookies(const char *name, const char *domain)
{
   pthread_mutex_lock(&g_cookie_lock);
   for (int i = 0; i < g_ncookies; ) {
      struct cookie *c = &g_cookies[i];
      const char *d = domain && domain[0] == '.' ? domain + 1 : domain;
      if ((!name || !strcmp(c->name, name)) && (!d || !strcasecmp(c->domain, d))) {
         free(c->name); free(c->value); free(c->domain); free(c->path);
         g_cookies[i] = g_cookies[--g_ncookies];
      } else ++i;
   }
   pthread_mutex_unlock(&g_cookie_lock);
}

static void cdp_handle(const char *text)
{
   struct jv *msg = json_parse(text);
   if (!msg) return;
   double id = jnum(msg, "id", -1);
   const char *method = jstr(msg, "method");
   const char *session = jstr(msg, "sessionId");
   const struct jv *params = jget(msg, "params");
   int in_session = session != NULL;
   if (!method) { jv_free(msg); return; }
   if (session && strcmp(session, SESSION_ID)) {
      cdp_error(id, 0, -32001, "Session with given id not found.");
      jv_free(msg);
      return;
   }
   LB_LOG("cdp %s\n", method);
   struct buf r = { 0 };
   if (!strcmp(method, "Browser.getVersion")) {
      buf_puts(&r, "{\"protocolVersion\":\"1.3\",\"product\":\"" LB_PRODUCT "\",\"revision\":\"\",\"userAgent\":");
      buf_json_str(&r, g_user_agent);
      buf_puts(&r, ",\"jsVersion\":\"QuickJS\"}");
      cdp_reply(id, in_session, r.p);
   } else if (!strcmp(method, "Browser.close")) {
      cdp_reply(id, in_session, "{}");
      exit(0);
   } else if (!strcmp(method, "Target.createTarget")) {
      cdp_reply(id, in_session, "{\"targetId\":\"" TARGET_ID "\"}");
      buf_puts(&r, "{\"targetInfo\":");
      target_info(&r);
      buf_puts(&r, "}");
      cdp_event_session("Target.targetCreated", r.p, 0);
      const char *url = jstr(params, "url");
      if (url && strcmp(url, "about:blank")) {
         free(g_page.nav_url);
         g_page.nav_url = lb_strdup(url);
      }
   } else if (!strcmp(method, "Target.attachToTarget")) {
      const char *tid = jstr(params, "targetId");
      if (tid && strcmp(tid, TARGET_ID)) cdp_error(id, in_session, -32602, "No target with given id found");
      else {
         g_attached = 1;
         cdp_reply(id, in_session, "{\"sessionId\":\"" SESSION_ID "\"}");
         buf_puts(&r, "{\"sessionId\":\"" SESSION_ID "\",\"targetInfo\":");
         target_info(&r);
         buf_puts(&r, ",\"waitingForDebugger\":false}");
         cdp_event_session("Target.attachedToTarget", r.p, 0);
      }
   } else if (!strcmp(method, "Target.getTargets")) {
      buf_puts(&r, "{\"targetInfos\":[");
      target_info(&r);
      buf_puts(&r, "]}");
      cdp_reply(id, in_session, r.p);
   } else if (!strcmp(method, "Target.closeTarget")) {
      cdp_reply(id, in_session, "{\"success\":true}");
      exit(0);
   } else if (!strcmp(method, "Page.navigate")) {
      const char *url = jstr(params, "url");
      if (!url) cdp_error(id, in_session, -32602, "Invalid parameters");
      else {
         buf_printf(&r, "{\"frameId\":\"" FRAME_ID "\",\"loaderId\":\"LB-LOADER-%u\"}", g_loader_seq + 1);
         cdp_reply(id, in_session, r.p);
         cdp_navigate_now(url);
      }
   } else if (!strcmp(method, "Page.reload")) {
      cdp_reply(id, in_session, "{}");
      char *u = lb_strdup(g_page.url ? g_page.url : "about:blank");
      cdp_navigate_now(u);
      free(u);
   } else if (!strcmp(method, "Page.setDocumentContent")) {
      const char *html = jstr(params, "html");
      cdp_reply(id, in_session, "{}");
      char *u = lb_strdup(g_page.url ? g_page.url : "about:blank");
      page_load_html(html ? html : "", u);
      free(u);
      cdp_emit("Page.loadEventFired", "{}");
   } else if (!strcmp(method, "Page.getFrameTree")) {
      buf_puts(&r, "{\"frameTree\":{\"frame\":");
      frame_info(&r);
      buf_puts(&r, "}}");
      cdp_reply(id, in_session, r.p);
   } else if (!strcmp(method, "Page.captureScreenshot")) {
      const char *fmt = jstr(params, "format");
      size_t len = 0;
      unsigned char *img = page_capture(&len, fmt && !strcmp(fmt, "jpeg") ? cast_quality(params) : 0);
      if (!img) cdp_error(id, in_session, -32000, "Unable to capture screenshot");
      else {
         buf_puts(&r, "{\"data\":\"");
         buf_base64(&r, img, len);
         buf_puts(&r, "\"}");
         cdp_reply(id, in_session, r.p);
         free(img);
      }
   } else if (!strcmp(method, "Page.startScreencast")) {
      const char *fmt = jstr(params, "format");
      g_cast.quality = fmt && !strcmp(fmt, "png") ? 0 : cast_quality(params);
      g_cast.on = 1;
      g_cast.acked = 1;
      g_page.dirty = 1;               /* the first frame right away */
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Page.stopScreencast")) {
      g_cast.on = 0;
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Page.screencastFrameAck")) {
      g_cast.acked = 1;
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Page.addScriptToEvaluateOnNewDocument")) {
      const char *src = jstr(params, "source");
      char **g = realloc(g_page.boot_scripts, sizeof *g * (size_t)(g_page.nboot + 1));
      if (g && src) { g_page.boot_scripts = g; g[g_page.nboot++] = lb_strdup(src); }
      buf_printf(&r, "{\"identifier\":\"%d\"}", g_page.nboot);
      cdp_reply(id, in_session, r.p);
   } else if (!strcmp(method, "Page.getNavigationHistory")) {
      buf_puts(&r, "{\"currentIndex\":0,\"entries\":[{\"id\":0,\"url\":");
      buf_json_str(&r, g_page.url ? g_page.url : "about:blank");
      buf_puts(&r, ",\"userTypedURL\":\"\",\"title\":");
      buf_json_str(&r, luna_doc_title);
      buf_puts(&r, ",\"transitionType\":\"typed\"}]}");
      cdp_reply(id, in_session, r.p);
   } else if (!strcmp(method, "Runtime.evaluate") || !strcmp(method, "Runtime.callFunctionOn")) {
      const char *expr = jstr(params, "expression");
      const char *decl = jstr(params, "functionDeclaration");
      char *code = NULL;
      if (!expr && decl) {
         code = malloc(strlen(decl) + 8);
         sprintf(code, "(%s)()", decl);
         expr = code;
      }
      char *res = cdp_evaluate(expr ? expr : "undefined", jbool(params, "returnByValue"),
                               jbool(params, "awaitPromise"));
      cdp_reply(id, in_session, res);
      free(res);
      free(code);
   } else if (!strcmp(method, "Network.emulateNetworkConditions")) {
      const int online = !jbool(params, "offline");
      network_set_online(online);
      if (g_page.ctx) {
         const char *code = online
            ? "Object.defineProperty(navigator,'onLine',{value:true,configurable:true});window.dispatchEvent(new Event('online'));"
            : "Object.defineProperty(navigator,'onLine',{value:false,configurable:true});window.dispatchEvent(new Event('offline'));";
         (void)js_eval(code, strlen(code), "network", 0);
      }
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Network.setCookie")) {
      cdp_set_cookie(params);
      cdp_reply(id, in_session, "{\"success\":true}");
   } else if (!strcmp(method, "Network.setCookies") || !strcmp(method, "Storage.setCookies")) {
      const struct jv *list = jget(params, "cookies");
      for (const struct jv *c = list && list->type == J_ARR ? list->kid : NULL; c; c = c->next)
         cdp_set_cookie(c);
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Network.getAllCookies") || !strcmp(method, "Storage.getCookies") ||
              !strcmp(method, "Network.getCookies")) {
      const struct jv *urls = jget(params, "urls");
      if (!urls && !strcmp(method, "Network.getCookies") && g_page.url) {
         /* no urls: the current page's */
         struct jv fake = { 0 }, one = { 0 };
         fake.type = J_ARR; fake.kid = &one; one.type = J_STR; one.str = g_page.url;
         cdp_cookie_list(&r, &fake);
      } else cdp_cookie_list(&r, urls);
      cdp_reply(id, in_session, r.p);
   } else if (!strcmp(method, "Network.deleteCookies")) {
      const char *dom = jstr(params, "domain");
      struct url pu = { 0 };
      const char *u = jstr(params, "url");
      if (!dom && u && url_parse(u, &pu) == 0) dom = pu.host;
      cdp_delete_cookies(jstr(params, "name"), dom);
      url_free(&pu);
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Network.clearBrowserCookies") || !strcmp(method, "Storage.clearCookies")) {
      cdp_delete_cookies(NULL, NULL);
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Runtime.addBinding")) {
      const char *name = jstr(params, "name");
      if (name) {
         char **g = realloc(g_page.bindings, sizeof *g * (size_t)(g_page.nbindings + 1));
         if (g) { g_page.bindings = g; g[g_page.nbindings++] = lb_strdup(name); }
         if (g_page.ctx) {
            JSValue a = JS_NewString(g_page.ctx, name);
            JS_FreeValue(g_page.ctx, js_hook("__lb_add_binding", 1, &a));
            JS_FreeValue(g_page.ctx, a);
         }
      }
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Runtime.enable")) {
      cdp_reply(id, in_session, "{}");
      cdp_emit("Runtime.executionContextCreated",
               "{\"context\":{\"id\":1,\"origin\":\"\",\"name\":\"\",\"uniqueId\":\"LB-CTX-1\","
               "\"auxData\":{\"isDefault\":true,\"type\":\"default\",\"frameId\":\"" FRAME_ID "\"}}}");
   } else if (!strcmp(method, "Emulation.setDeviceMetricsOverride")) {
      set_viewport((int)jnum(params, "width", g_page.w), (int)jnum(params, "height", g_page.h),
                   (float)jnum(params, "deviceScaleFactor", 1.0));
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Emulation.setUserAgentOverride") ||
              !strcmp(method, "Network.setUserAgentOverride")) {
      const char *ua = jstr(params, "userAgent");
      if (ua) snprintf(g_user_agent, sizeof g_user_agent, "%s", ua);
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Input.dispatchMouseEvent")) {
      const char *type = jstr(params, "type");
      double x = jnum(params, "x", 0), y = jnum(params, "y", 0);
      cdp_reply(id, in_session, "{}");
      const char *bname = jstr(params, "button");
      int button = bname && !strcmp(bname, "right") ? LUNA_MOUSE_BUTTON_RIGHT :
                   bname && !strcmp(bname, "middle") ? 2 : LUNA_MOUSE_BUTTON_LEFT;
      int mods = cdp_mods((int)jnum(params, "modifiers", 0));
      if (type && !strcmp(type, "mousePressed")) page_pointer(x, y, 1, 0, button, mods);
      else if (type && !strcmp(type, "mouseReleased")) page_pointer(x, y, 0, 0, button, mods);
      else if (type && !strcmp(type, "mouseMoved")) page_pointer(x, y, -1, 0, 0, mods);
      else if (type && !strcmp(type, "mouseWheel"))
         page_wheel(x, y, jnum(params, "deltaX", 0), jnum(params, "deltaY", 0));
   } else if (!strcmp(method, "Lunaria.contextInfo")) {
      /* What is under the pointer, for the window's right-click menu. */
      double x = jnum(params, "x", 0), y = jnum(params, "y", 0);
      luna_dom_flush_restyle();
      luna_update(lb_now_ms() / 1000.0, 0.0);
      char *link = luna_link_at_point(x, y);
      char *sel = luna_selection_text();
      char *image = NULL;
      for (int i = luna_element_at_point(x, y), g = 0; i >= 0 && i < elem_count && g < 64; i = elements[i].parent_idx, ++g) {
         if (strcmp(elements[i].type, "img") || !elements[i].has_bg_image) continue;
         const char *p = luna_expand_path(elements[i].bg_image_path);
         if (p && strncmp(p, "data:", 5)) image = url_resolve(g_page.url, p);
         break;
      }
      int f = luna_focused_element();
      int editable = f >= 0 && f < elem_count && elements[f].is_input;
      struct buf b = { 0 };
      buf_puts(&b, "{\"link\":");
      buf_json_str(&b, link ? link : "");
      buf_puts(&b, ",\"image\":");
      buf_json_str(&b, image ? image : "");
      buf_puts(&b, ",\"selection\":");
      buf_json_str(&b, sel ? sel : "");
      buf_printf(&b, ",\"editable\":%d}", editable);
      cdp_reply(id, in_session, b.p);
      free(b.p); free(link); free(sel); free(image);
   } else if (!strcmp(method, "Lunaria.find")) {
      const char *text = jstr(params, "text");
      int forward = jnum(params, "forward", 1) != 0;
      if (jnum(params, "restart", 0) != 0) luna_selection_clear();
      int found = text && text[0] ? luna_find_text(text, forward) : (luna_selection_clear(), 0);
      g_page.dirty = 1;
      cdp_reply(id, in_session, "{}");
      char r[40];
      snprintf(r, sizeof r, "{\"found\":%d}", found);
      cdp_emit("Lunaria.findResult", r);
   } else if (!strcmp(method, "Lunaria.setClipboard")) {
      const char *text = jstr(params, "text");
      free(g_page_clip);
      g_page_clip = lb_strdup(text ? text : "");
      cdp_reply(id, in_session, "{}");
   } else if (!strcmp(method, "Input.dispatchTouchEvent")) {
      const char *type = jstr(params, "type");
      const struct jv *pts = jget(params, "touchPoints");
      const struct jv *p0 = pts && pts->type == J_ARR ? pts->kid : NULL;
      static double tx, ty;
      if (p0) { tx = jnum(p0, "x", tx); ty = jnum(p0, "y", ty); }
      cdp_reply(id, in_session, "{}");
      if (type && !strcmp(type, "touchStart")) page_pointer(tx, ty, 1, 1, LUNA_MOUSE_BUTTON_LEFT, 0);
      else if (type && !strcmp(type, "touchMove")) page_pointer(tx, ty, -1, 1, 0, 0);
      else if (type && (!strcmp(type, "touchEnd") || !strcmp(type, "touchCancel")))
         page_pointer(tx, ty, 0, 1, LUNA_MOUSE_BUTTON_LEFT, 0);
   } else if (!strcmp(method, "Input.insertText") || !strcmp(method, "Input.imeSetComposition")) {
      const char *text = jstr(params, "text");
      cdp_reply(id, in_session, "{}");
      if (text && method[6] == 'i') page_text(text);
   } else if (!strcmp(method, "Input.dispatchKeyEvent")) {
      const char *type = jstr(params, "type");
      const char *key = jstr(params, "key");
      const char *text = jstr(params, "text");
      int vk = (int)jnum(params, "windowsVirtualKeyCode", 0);
      cdp_reply(id, in_session, "{}");
      if (type && (!strcmp(type, "keyDown") || !strcmp(type, "rawKeyDown")))
         page_key(key, vk, 1, !strcmp(type, "keyDown") ? text : NULL, cdp_mods((int)jnum(params, "modifiers", 0)));
      else if (type && !strcmp(type, "keyUp")) page_key(key, vk, 0, NULL, cdp_mods((int)jnum(params, "modifiers", 0)));
      else if (type && !strcmp(type, "char") && text) page_text(text);
   } else if (strstr(method, ".enable") || strstr(method, ".disable") ||
              !strcmp(method, "Target.setDiscoverTargets") || !strcmp(method, "Target.setAutoAttach") ||
              !strcmp(method, "Page.setLifecycleEventsEnabled") || !strcmp(method, "Page.bringToFront") ||
              !strcmp(method, "Page.stopLoading") || !strcmp(method, "Network.setCacheDisabled") ||
              !strcmp(method, "Network.setExtraHTTPHeaders") || !strcmp(method, "Emulation.setTouchEmulationEnabled") ||
              !strcmp(method, "Emulation.setEmitTouchEventsForMouse") ||
              !strcmp(method, "Emulation.clearDeviceMetricsOverride") ||
              !strcmp(method, "Runtime.runIfWaitingForDebugger") ||
              !strcmp(method, "Page.setBypassCSP") || !strcmp(method, "Emulation.setFocusEmulationEnabled")) {
      cdp_reply(id, in_session, "{}");
   } else {
      char m[256];
      snprintf(m, sizeof m, "'%s' wasn't found", method);
      cdp_error(id, in_session, -32601, m);
   }
   free(r.p);
   jv_free(msg);
}

/* Reads what has arrived on the pipe and handles every complete message.
 * Returns -1 when the embedder closed it. */
static int cdp_read(struct buf *rx)
{
   for (;;) {
      char chunk[65536];
      ssize_t n = read(g_cdp_in, chunk, sizeof chunk);
      if (n == 0) return -1;
      if (n < 0) {
         if (errno == EINTR) continue;
         if (errno == EAGAIN || errno == EWOULDBLOCK) break;
         return -1;
      }
      buf_put(rx, chunk, (size_t)n);
      if ((size_t)n < sizeof chunk) break;
   }
   size_t start = 0;
   for (size_t i = 0; i < rx->len; ++i) {
      if (rx->p[i]) continue;
      cdp_handle(rx->p + start);
      start = i + 1;
   }
   if (start) {
      memmove(rx->p, rx->p + start, rx->len - start);
      rx->len -= start;
      rx->p[rx->len] = '\0';
   }
   return 0;
}

/* Page.startScreencast: a frame goes out when the page has repainted and the
 * previous one was acknowledged, at most every CAST_MS -- no polling, and
 * nothing sent while the page is still. */

/* Milliseconds until the next frame is due (-1: none pending). */
static int cast_pump(void)
{
   if (!g_cast.on || !g_cast.acked) return -1;
   double now = lb_now_ms();
   double gap = CAST_MS;
   if (!g_page.dirty) {
      int m = luna_needs_redraw(now / 1000.0, 0.0);
      if (!m) return -1;
      if (!(m & (LUNA_REDRAW_LAYOUT | LUNA_REDRAW_ANIM))) gap = CAST_CARET_MS;
   }
   if (now - g_cast.last_ms < gap) return (int)ceil(gap - (now - g_cast.last_ms));
   size_t len = 0;
   unsigned char *img = page_capture(&len, g_cast.quality);
   if (!img) return -1;
   struct buf b = { 0 };
   buf_puts(&b, "{\"data\":\"");
   buf_base64(&b, img, len);
   buf_printf(&b, "\",\"metadata\":{\"offsetTop\":0,\"pageScaleFactor\":%g,"
              "\"deviceWidth\":%d,\"deviceHeight\":%d,\"scrollOffsetX\":0,"
              "\"scrollOffsetY\":0,\"timestamp\":%.6f},\"sessionId\":%d}",
              (double)g_page.scale, g_page.w, g_page.h, now / 1000.0, ++g_cast.seq);
   cdp_emit("Page.screencastFrame", b.p);
   free(b.p);
   free(img);
   g_cast.acked = 0;
   g_cast.last_ms = now;
   return -1;
}

static int cdp_loop(void)
{
   g_page.emit = cdp_emit;
   struct buf rx = { 0 };
   fcntl(g_cdp_in, F_SETFD, FD_CLOEXEC);
   fcntl(g_cdp_out, F_SETFD, FD_CLOEXEC);
   fcntl(g_cdp_in, F_SETFL, fcntl(g_cdp_in, F_GETFL) | O_NONBLOCK);
   for (;;) {
      /* Tasks run in arrival order: messages that came in while a script or
       * a load was running go before the timers that fell due meanwhile. */
      if (cdp_read(&rx) < 0) return 0;
      double next = page_pump();
      int timeout = next < 0 ? 1000 : next > 1000 ? 1000 : (int)ceil(next);
      int cast = cast_pump();
      if (cast >= 0 && cast < timeout) timeout = cast;
      struct pollfd pfd[2] = { { g_cdp_in, POLLIN, 0 }, { g_wake[0], POLLIN, 0 } };
      int pr = poll(pfd, g_wake[0] >= 0 ? 2 : 1, timeout);
      if (pr < 0 && errno != EINTR) return 1;
   }
}

/* ======================================================================== *
 * Command line
 * ======================================================================== */

static void usage(void)
{
   fputs("usage: luna-browser [options] [URL]\n"
         "  (no mode)                 open a browser window (GLFW builds)\n"
         "  --remote-debugging-pipe   serve CDP on fds 3/4 (headless)\n"
         "  --screenshot[=FILE]       load URL, save a PNG (default screenshot.png), exit\n"
         "  --dump-dom                load URL, print the document, exit\n"
         "  --window-size=W,H         viewport (default 1024,576)\n"
         "  --virtual-time-budget=MS  time timers get before --screenshot/--dump-dom\n"
         "  --user-data-dir=DIR       profile (localStorage)\n"
         "  --user-agent=UA\n"
         "  -v                        log\n", stderr);
}

/* The page process has no system clipboard: a copy is reported to the window
 * (Lunaria.clipboard), which owns the real one, and the window pushes its
 * contents back (Lunaria.setClipboard) before a paste can need them. */
static void lb_set_clipboard(const char *utf8)
{
   free(g_page_clip);
   g_page_clip = lb_strdup(utf8 ? utf8 : "");
   struct buf b = { 0 };
   buf_puts(&b, "{\"text\":");
   buf_json_str(&b, g_page_clip);
   buf_puts(&b, "}");
   cdp_emit("Lunaria.clipboard", b.p);
   free(b.p);
}

static char *lb_get_clipboard(void) { return g_page_clip ? lb_strdup(g_page_clip) : NULL; }

static char *lb_resolve_href(const char *href) { return url_resolve(g_page.url, href); }

static void setup_luna_platform(void)
{
   static LunaPlatform platform;
   memset(&platform, 0, sizeof platform);
   platform.get_time = lb_platform_time;
   platform.get_proc = gl_proc;
   platform.read_resource = lb_read_resource;
   platform.set_clipboard = lb_set_clipboard;
   platform.get_clipboard = lb_get_clipboard;
   platform.struct_size = (uint32_t)sizeof platform;
   luna_set_platform(&platform);
}

static int headless_init(void)
{
   if (gl_open(g_page.w, g_page.h) != 0) return -1;
   setup_luna_platform();
   luna_set_gles3(g_gl.es);
   LunaInitConfig cfg = { (float)g_page.w, (float)g_page.h, gl_proc, 1 };
   if (!luna_init(&cfg)) { fprintf(stderr, "luna-browser: luna_init failed\n"); return -1; }
   luna_set_url_resolver(lb_resolve_href);
   g_luna_platform.set_cursor = page_set_cursor;
   luna_set_mouse_press_hook(hook_press);
   luna_set_mouse_release_hook(hook_release);
   luna_set_web_compat(1);
   luna_dom_defer_restyle(1);
   luna_set_image_decoder(lb_decode_webp);
   return 0;
}

static void settle(double budget_ms)
{
   /* Pictures and data still arriving are waited for, up to a limit. */
   const double hard_end = lb_now_ms() + budget_ms + 4000;
   double end = lb_now_ms() + budget_ms;
   while (lb_now_ms() < end || (g_jobs_out > 0 && lb_now_ms() < hard_end)) {
      double next = page_pump();
      double left = end - lb_now_ms();
      if (g_jobs_out > 0 && left < 20) left = 20;
      if (next < 0 || next > left) next = left;
      if (next <= 0) break;
      if (next > 50) next = 50;
      struct timespec ts = { 0, (long)(next * 1e6) };
      nanosleep(&ts, NULL);
   }
   page_pump();
}

static char *arg_url(const char *a)
{
   if (strstr(a, "://") || !strncmp(a, "about:", 6) || !strncmp(a, "data:", 5)) return lb_strdup(a);
   char *real = realpath(a, NULL);
   struct buf b = { 0 };
   buf_puts(&b, "file://");
   buf_puts(&b, real ? real : a);
   free(real);
   return buf_take(&b);
}

#ifdef LB_WINDOW
/* ======================================================================== *
 * The browser window
 *
 * luna-ui keeps one document per process, so, as in Chrome, the window and
 * the page are two processes: this one draws the toolbar with luna-ui and
 * shows the frames a child `luna-browser --remote-debugging-pipe` pushes
 * (Page.startScreencast), and hands it input over the same protocol.
 * ======================================================================== */

#define UI_BAR (UI_TAB_H + UI_BAR_H)
#define UI_FRAME "lb-frame"          /* the page picture, served from memory */
#define UI_CAT "lb-cat"              /* the running cat shown while a page loads */

static const unsigned char ui_cat_png[] = {
#include "run_cat.png.h"
};

static const char ui_html[] =
   "<body><div id=\"tabs\"></div><div id=\"bar\">"
   "<button id=\"back\" title=\"Back\">\xe2\x80\xb9</button>"
   "<button id=\"fwd\" title=\"Forward\">\xe2\x80\xba</button>"
   "<button id=\"reload\" title=\"Reload\">\xe2\x86\xbb</button>"
   "<input id=\"url\" type=\"text\" spellcheck=\"false\">"
   "<div id=\"spin\"></div>"
   "<button id=\"pc\" title=\"Request desktop site\">PC</button>"
   "</div><div id=\"view\"></div>"
   "<div id=\"cat\"><div class=\"cf\" id=\"cf\"></div>"
   "<div id=\"cattext\">Loading as hard as I can...</div></div>"
   "<div id=\"findbox\"><input id=\"find\" type=\"text\" placeholder=\"Find in page\"><span id=\"fcnt\"></span></div>"
   "<div id=\"ctx\"></div><div id=\"status\"></div></body>";
static const char ui_css[] =
   "body{margin:0;background:#ffffff;font-family:sans-serif;}"
   "#tabs{position:fixed;left:0;top:0;right:0;height:34px;box-sizing:border-box;display:flex;"
   "align-items:flex-end;gap:2px;padding:4px 8px 0 8px;background:#dee1e6;}"
   ".tab{display:flex;align-items:center;flex:0 1 200px;min-width:60px;height:30px;box-sizing:border-box;"
   "padding:0 6px 0 12px;border-radius:8px 8px 0 0;background:#cfd3d9;color:#3c4043;font-size:12px;}"
   ".tab:hover{background:#e8eaed;}"
   ".tab.on{background:#f1f3f4;color:#202124;}"
   ".tt{flex:1 1 auto;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;}"
   ".tx{flex:0 0 auto;width:20px;height:20px;line-height:20px;text-align:center;border-radius:10px;font-size:14px;}"
   ".tx:hover{background:#c4c7cc;}"
   "#newtab{flex:0 0 auto;width:28px;height:28px;line-height:28px;text-align:center;border-radius:14px;"
   "font-size:18px;color:#3c4043;margin-left:4px;margin-bottom:1px;}"
   "#newtab:hover{background:#c4c7cc;}"
   "#bar{position:fixed;left:0;top:34px;right:0;height:44px;box-sizing:border-box;display:flex;"
   "align-items:center;gap:4px;padding:0 8px;background:#f1f3f4;border-bottom:1px solid #dadce0;}"
   "#bar button{width:32px;height:32px;border:0;border-radius:16px;background:transparent;"
   "color:#3c4043;font-size:20px;padding:0;flex:0 0 auto;}"
   "#bar button:hover{background:#e3e5e8;}"
   "#bar button.off{color:#b8bbbf;}"
   "#url{flex:1 1 auto;height:30px;box-sizing:border-box;border:1px solid #dadce0;border-radius:15px;"
   "padding:0 14px;margin-left:4px;font-size:14px;background:#ffffff;color:#202124;}"
   "@keyframes lbspin{from{transform:rotate(0deg);}to{transform:rotate(360deg);}}"
   "#spin,.sp{box-sizing:border-box;border:2px solid #c6dafc;border-top-color:#1a73e8;border-radius:50%;"
   "animation:lbspin 0.8s linear infinite;}"
   "#spin{display:none;flex:0 0 auto;width:20px;height:20px;margin-left:8px;}"
   "#spin.on{display:block;}"
   ".sp{flex:0 0 auto;width:12px;height:12px;margin-right:6px;}"
   "#bar #pc{width:34px;font-size:12px;font-weight:bold;margin-left:4px;}"
   "#bar #pc.on{background:#d2e3fc;color:#174ea6;}"
   "#view{position:fixed;left:0;top:78px;right:0;bottom:0;background-size:100% 100%;"
   "background-repeat:no-repeat;}"
   "#cat{position:fixed;left:50%;top:50%;width:128px;height:128px;margin:-25px 0 0 -64px;display:none;}"
   "#cat.on{display:block;}"
   "#cattext{position:absolute;left:-86px;top:134px;width:300px;text-align:center;font-size:15px;color:#5f6368;}"
   ".cf{position:absolute;left:0;top:0;width:128px;height:128px;background-image:url(lb-cat);"
   "background-repeat:no-repeat;background-size:512px 128px;background-position:0px 0px;}"
   "#ctx{position:fixed;left:0;top:0;display:none;min-width:210px;padding:4px 0;background:#ffffff;"
   "border:1px solid #dadce0;border-radius:6px;}"
   "#ctx.on{display:block;}"
   ".mi{padding:6px 16px;font-size:13px;color:#202124;}"
   ".mi:hover{background:#e8f0fe;}"
   ".mi.off{color:#9aa0a6;}"
   ".msep{height:1px;margin:4px 0;background:#e0e3e7;}"
   "#status{position:fixed;left:0;bottom:0;padding:2px 8px;font-size:12px;color:#5f6368;"
   "background:#f1f3f4;border-top-right-radius:4px;display:none;}"
   "#status.on{display:block;}"
   "#findbox{position:fixed;right:16px;top:86px;display:none;align-items:center;gap:8px;padding:6px 10px;"
   "background:#ffffff;border:1px solid #dadce0;border-radius:8px;}"
   "#findbox.on{display:flex;}"
   "#find{width:200px;height:26px;box-sizing:border-box;border:1px solid #dadce0;border-radius:4px;padding:0 8px;"
   "font-size:14px;background:#ffffff;color:#202124;}"
   "#fcnt{font-size:12px;color:#5f6368;}";

#define UI_BAR_H 44                  /* the toolbar */
#define UI_TAB_H 34                  /* the tab strip above it */
#define UI_MAX_TABS 16

/* One tab is one page process, as in Chrome. */
struct ui_tab {
   pid_t pid;
   int to, from;                     /* the child's fds 3 and 4, from here */
   struct buf rx;
   unsigned next_id, title_id;
   unsigned char *frame;             /* the newest picture (JPEG) */
   size_t frame_len;
   char **hist;                      /* back/forward list and where we are */
   int nhist, cur, going;            /* going: index a back/forward is loading */
   int sent_w, sent_h;
   int loading, committed, desktop;             /* desktop: asked for the PC version of sites */
   char hover[1024];
   char title[256];
   char *start;                      /* loaded when the page process attaches */
};

static struct ui_tab *g_tabs[UI_MAX_TABS];
static int g_ntabs, g_active;

static struct {
   struct ui_tab *t;                 /* the tab being acted on (the active one) */
   int shown;
   float scale;
   int pressed;                      /* a button went down over the page */
   float zoom;                       /* page zoom (Ctrl +/-) */
   int find_on, url_click;
   char find_last[256];
   double mx, my;
   char *start;                      /* the first tab's address */
   int action, action_arg;           /* a click, run from the frame loop */
   int tabs_dirty;                   /* the tab strip needs redrawing */
   unsigned load_mask;               /* which tabs showed a spinner last */
   int cat_on, cat_frame;
   unsigned ctx_id;                  /* the contextInfo request being waited for */
   double ctx_x, ctx_y;              /* where the menu opens (window px) */
   int ctx_open, ctx_n;
   int ctx_cmd[16];
   char *ctx_arg[16];
   double wheel_x, wheel_y;          /* wheel distance not yet sent (px): sent a share per frame */
   double wheel_at_x, wheel_at_y;
   unsigned shot_id;                 /* the captureScreenshot being waited for */
   char shot_path[1024];
   unsigned frames;                  /* ui_frame calls, for LUNA_BROWSER_SCREENSHOT_EVERY */
   int shot_seq;
} g_ui = { .scale = 1.0f, .zoom = 1.0f };

enum { UA_NONE, UA_SWITCH, UA_CLOSE, UA_NEW, UA_DESKTOP, UA_MENU };

static int ui_is_active(void) { return g_ui.t == g_tabs[g_active]; }

static int ui_el(const char *id) { return luna_get_element_by_id(id); }
static void ui_ctx_show(const char *link, const char *image, const char *sel, int editable);
static void ui_menu_do(int i);
static void ui_ctx_close(void);

/* A protocol command to the page's session (or the browser, session 0). */
static unsigned ui_send(const char *method, const char *params, int session)
{
   if (g_ui.t->to < 0) return 0;
   unsigned id = ++g_ui.t->next_id;
   struct buf b = { 0 };
   buf_printf(&b, "{\"id\":%u,\"method\":\"%s\",\"params\":%s%s}", id, method,
              params ? params : "{}", session ? ",\"sessionId\":\"" SESSION_ID "\"" : "");
   buf_put(&b, "", 1);               /* the NUL that ends a message */
   const char *p = b.p;
   size_t n = b.len;
   while (n > 0) {
      ssize_t w = write(g_ui.t->to, p, n);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) break;
      p += w; n -= (size_t)w;
   }
   free(b.p);
   return id;
}

static unsigned char *ui_read_resource(const char *path, size_t *out_size)
{
   *out_size = 0;
   if (path && !strcmp(path, UI_CAT)) {
      unsigned char *cat = malloc(sizeof ui_cat_png);
      if (!cat) return NULL;
      memcpy(cat, ui_cat_png, sizeof ui_cat_png);
      *out_size = sizeof ui_cat_png;
      return cat;
   }
   struct ui_tab *t = g_ntabs ? g_tabs[g_active] : NULL;
   if (!path || strcmp(path, UI_FRAME) || !t || !t->frame) return NULL;
   unsigned char *copy = malloc(t->frame_len);
   if (!copy) return NULL;
   memcpy(copy, t->frame, t->frame_len);
   *out_size = t->frame_len;
   return copy;
}

static void ui_status(const char *text)
{
   if (!ui_is_active()) return;
   int i = ui_el("status");
   if (i < 0) return;
   luna_set_text(i, text ? text : "");
   if (text && *text) luna_add_class(i, "on"); else luna_remove_class(i, "on");
}

/* The status line: the link under the pointer, else the loading note. */
static void ui_show_hover(const char *url)
{
   snprintf(g_ui.t->hover, sizeof g_ui.t->hover, "%s", url ? url : "");
   if (!ui_is_active()) return;
   ui_status(g_ui.t->hover[0] ? g_ui.t->hover : g_ui.t->loading ? "Loading\xe2\x80\xa6" : NULL);
}

static void ui_buttons(void)
{
   if (!ui_is_active()) return;
   int b = ui_el("back"), f = ui_el("fwd");
   int pc = ui_el("pc");
   if (pc >= 0) { if (g_ui.t->desktop) luna_add_class(pc, "on"); else luna_remove_class(pc, "on"); }
   if (b >= 0) { if (g_ui.t->cur > 0) luna_remove_class(b, "off"); else luna_add_class(b, "off"); }
   if (f >= 0) { if (g_ui.t->cur + 1 < g_ui.t->nhist) luna_remove_class(f, "off"); else luna_add_class(f, "off"); }
}

/* The page a new tab shows: a search box and the sites people go to most. */
#define UI_HOME_PREFIX "data:text/html;charset=utf-8,"
static const char ui_home_html[] =
   "<!doctype html><html><head><meta charset=\"utf-8\"><title>New tab</title><style>\n"
   "html,body{margin:0;height:100%;overflow:hidden;}\n"
   "body{font-family:sans-serif;color:#eef3ff;text-align:center;background:linear-gradient(180deg,#070b1f 0%,#14224a 55%,#2b3f72 100%);}\n"
   "#snow{position:absolute;left:0;top:0;right:0;bottom:0;overflow:hidden;}\n"
   ".f{position:absolute;left:0;top:0;background:#fff;border-radius:50%;}\n"
   "#hill{position:absolute;left:0;right:0;bottom:0;height:56px;background:linear-gradient(180deg,#2b3f72,#aebfe8 60%,#f2f6ff);}\n"
   "#main{position:relative;z-index:2;width:100%;}\n"
   "#time{margin:64px 0 0;font-size:84px;font-weight:200;letter-spacing:4px;}\n"
   "#date{margin:2px 0 26px;font-size:17px;color:#aab8e0;}\n"
   "form{margin:0 auto 30px;width:560px;max-width:90%;}\n"
   "input{box-sizing:border-box;width:100%;height:48px;padding:0 22px;font-size:17px;color:#fff;\n"
   "border:1px solid #5a6fa8;border-radius:24px;background:#1c2a55;text-align:center;}\n"
   ".g{margin:0 auto;width:640px;max-width:94%;}\n"
   ".g a{display:inline-block;box-sizing:border-box;width:140px;margin:7px;padding:16px 6px 12px;\n"
   "background:#1c2a55;border:1px solid #34478a;border-radius:12px;text-decoration:none;color:#e6ecff;font-size:14px;}\n"
   ".g b{display:block;margin:0 auto 8px;width:40px;height:40px;line-height:40px;border-radius:20px;color:#fff;font-size:20px;}\n"
   "</style></head><body><div id=\"snow\"></div><div id=\"hill\"></div>\n"
   "<div id=\"main\"><div id=\"time\"></div><div id=\"date\"></div>\n"
   "<form action=\"https://www.bing.com/search\" method=\"get\"><input name=\"q\" type=\"text\" placeholder=\"Search or type a URL\"></form>\n"
   "<div class=\"g\">\n"
   "<a href=\"https://www.bing.com/\"><b style=\"background:#0078d4\">B</b>Bing</a>\n"
   "<a href=\"https://www.yahoo.co.jp/\"><b style=\"background:#ff0033\">Y</b>Yahoo! JAPAN</a>\n"
   "<a href=\"https://www.google.com/\"><b style=\"background:#4285f4\">G</b>Google</a>\n"
   "<a href=\"https://en.wikipedia.org/\"><b style=\"background:#636466\">W</b>Wikipedia</a>\n"
   "<a href=\"https://news.ycombinator.com/\"><b style=\"background:#ff6600\">H</b>Hacker News</a>\n"
   "<a href=\"https://github.com/\"><b style=\"background:#24292f\">G</b>GitHub</a>\n"
   "</div></div>\n"
   "<script>\n"
   "(function(){\n"
   "var s=document.getElementById('snow'),r=7,F=[],W=window.innerWidth||1000,H=window.innerHeight||640;\n"
   "function rnd(){r=(r*1103515245+12345)&0x7fffffff;return r/0x7fffffff;}\n"
   "for(var i=0;i<50;i++){\n"
   " var d=document.createElement('div'),z=2+Math.floor(rnd()*5);\n"
   " d.className='f';\n"
   " d.style.cssText='width:'+z+'px;height:'+z+'px;opacity:'+(0.45+rnd()*0.5).toFixed(2);\n"
   " s.appendChild(d);\n"
   " F.push({e:d,x:rnd()*W,y:rnd()*H,v:20+z*9+rnd()*20,a:6+rnd()*14,p:rnd()*6.28,w:0.5+rnd()});\n"
   "}\n"
   "var last=0;\n"
   "function frame(t){\n"
   " var dt=last?Math.min((t-last)/1000,0.1):0;last=t;\n"
   " W=window.innerWidth||W;H=window.innerHeight||H;\n"
   " for(var i=0;i<F.length;i++){var f=F[i];\n"
   "  f.y+=f.v*dt;f.p+=f.w*dt;\n"
   "  if(f.y>H+10){f.y=-10;f.x=rnd()*W;}\n"
   "  f.e.style.transform='translate('+(f.x+Math.sin(f.p)*f.a).toFixed(1)+'px,'+f.y.toFixed(1)+'px)';\n"
   " }\n"
   " requestAnimationFrame(frame);\n"
   "}\n"
   "requestAnimationFrame(frame);\n"
   "var D=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'],M=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];\n"
   "function tick(){var n=new Date(),p=function(v){return(v<10?'0':'')+v;};\n"
   " document.getElementById('time').textContent=p(n.getHours())+':'+p(n.getMinutes());\n"
   " document.getElementById('date').textContent=D[n.getDay()]+', '+M[n.getMonth()]+' '+n.getDate();}\n"
   "tick();setInterval(tick,10000);\n"
   "})();\n"
   "</script></body></html>\n";

static char *ui_home_url(void)
{
   struct buf b = { 0 };
   buf_puts(&b, UI_HOME_PREFIX);
   for (const unsigned char *p = (const unsigned char *)ui_home_html; *p; ++p) {
      if (isalnum(*p) || strchr("-_.~", *p)) buf_put(&b, (const char *)p, 1);
      else buf_printf(&b, "%%%02X", *p);
   }
   return buf_take(&b);
}

/* A search for text, on the engine in LUNA_BROWSER_SEARCH (a URL ending in the
 * query parameter) or Bing. */
static char *ui_search_url(const char *t)
{
   const char *engine = getenv("LUNA_BROWSER_SEARCH");
   struct buf b = { 0 };
   buf_puts(&b, engine && strstr(engine, "://") ? engine : "https://www.bing.com/search?q=");
   for (const unsigned char *p = (const unsigned char *)t; *p; ++p) {
      if (isalnum(*p) || strchr("-_.~", *p)) buf_put(&b, (const char *)p, 1);
      else buf_printf(&b, *p == ' ' ? "+" : "%%%02X", *p);
   }
   return buf_take(&b);
}

/* A host that is this machine or a bare address: plain http, not https. */
static int ui_is_local_host(const char *t)
{
   size_t n = strcspn(t, "/:?#");
   if (n >= 9 && !strncmp(t, "localhost", 9)) return 1;
   int dots = 0, digits = 1;
   for (size_t i = 0; i < n; ++i) {
      if (t[i] == '.') ++dots; else if (!isdigit((unsigned char)t[i])) digits = 0;
   }
   return digits && dots == 3;
}

/* What was typed: a URL, a file, or else a search. */
static char *ui_typed_url(const char *t)
{
   while (*t == ' ') t++;
   if (!*t) return NULL;
   if (strstr(t, "://") || !strncmp(t, "about:", 6) || !strncmp(t, "data:", 5)) return lb_strdup(t);
   if (t[0] == '/' || t[0] == '.' || t[0] == '~') return arg_url(t);
   if (!strncmp(t, "www.", 4) || (!strchr(t, ' ') && (strchr(t, '.') || ui_is_local_host(t)))) {
      struct buf b = { 0 };
      buf_puts(&b, ui_is_local_host(t) ? "http://" : "https://");
      buf_puts(&b, t);
      return buf_take(&b);
   }
   return ui_search_url(t);
}

static void ui_navigate(const char *url)
{
   if (!url) return;
   struct buf b = { 0 };
   buf_puts(&b, "{\"url\":");
   buf_json_str(&b, url);
   buf_puts(&b, "}");
   ui_send("Page.navigate", b.p, 1);
   free(b.p);
   g_ui.t->loading = 1; g_ui.t->committed = 0;
   ui_status("Loading\xe2\x80\xa6");
}

static void ui_go(int delta)
{
   int to = g_ui.t->cur + delta;
   if (to < 0 || to >= g_ui.t->nhist) return;
   g_ui.t->going = to;
   ui_navigate(g_ui.t->hist[to]);
}

static void ui_on_back(LunaElement *e) { (void)e; ui_go(-1); }
static void ui_on_fwd(LunaElement *e) { (void)e; ui_go(1); }
static void ui_on_reload(LunaElement *e) { (void)e; ui_send("Page.reload", NULL, 1); g_ui.t->loading = 1; g_ui.t->committed = 0; ui_status("Loading\xe2\x80\xa6"); }

/* A page committed: the address bar, the history, then its title. */
static void ui_committed(const char *url)
{
   int u = ui_el("url");
   g_ui.t->committed = 1;
   if (ui_is_active() && u >= 0 && luna_focused_element() != u)
      luna_set_value(u, strncmp(url, UI_HOME_PREFIX, sizeof UI_HOME_PREFIX - 1) ? url : "");
   if (g_ui.t->going >= 0) {
      g_ui.t->cur = g_ui.t->going;
      g_ui.t->going = -1;
   } else if (!(g_ui.t->cur < g_ui.t->nhist && !strcmp(g_ui.t->hist[g_ui.t->cur], url))) {
      for (int i = g_ui.t->cur + 1; i < g_ui.t->nhist; ++i) free(g_ui.t->hist[i]);
      g_ui.t->nhist = g_ui.t->nhist ? g_ui.t->cur + 1 : 0;
      char **h = realloc(g_ui.t->hist, sizeof *h * (size_t)(g_ui.t->nhist + 1));
      if (h) { g_ui.t->hist = h; h[g_ui.t->nhist] = lb_strdup(url); g_ui.t->cur = g_ui.t->nhist++; }
   }
   ui_buttons();
}

static void ui_message(const char *text)
{
   struct jv *m = json_parse(text);
   if (!m) return;
   const char *method = jstr(m, "method");
   const struct jv *params = jget(m, "params");
   if (method && !strcmp(method, "Page.screencastFrame")) {
      const char *data = jstr(params, "data");
      size_t n = 0;
      unsigned char *img = data ? base64_decode(data, strlen(data), &n) : NULL;
      if (img) {
         free(g_ui.t->frame);
         g_ui.t->frame = img;
         g_ui.t->frame_len = n;
         if (ui_is_active()) {
            luna_invalidate_texture(UI_FRAME);
            if (!g_ui.shown && ui_el("view") >= 0) {
               luna_set_background_image(ui_el("view"), UI_FRAME);
               g_ui.shown = 1;
            }
            luna_app_request_redraw();
         }
      }
      char ack[48];
      snprintf(ack, sizeof ack, "{\"sessionId\":%d}", (int)jnum(params, "sessionId", 0));
      ui_send("Page.screencastFrameAck", ack, 1);
   } else if (method && !strcmp(method, "Lunaria.clipboard")) {
      const char *text = jstr(params, "text");
      if (text) luna_clipboard_set(text);
   } else if (method && !strcmp(method, "Page.frameStartedLoading")) {
      g_ui.t->loading = 1;
      g_ui.t->committed = 0;
      if (ui_is_active()) ui_status("Loading\xe2\x80\xa6");
   } else if (method && !strcmp(method, "Lunaria.cursor")) {
      if (ui_is_active() && g_luna_platform.set_cursor) g_luna_platform.set_cursor((int)jnum(params, "type", 0));
   } else if (method && !strcmp(method, "Page.frameNavigated")) {
      const char *url = jstr(jget(params, "frame"), "url");
      if (url) ui_committed(url);
   } else if (method && !strcmp(method, "Lunaria.hover")) {
      ui_show_hover(jstr(params, "url"));
   } else if (method && !strcmp(method, "Lunaria.findResult")) {
      int c = ui_is_active() ? ui_el("fcnt") : -1;
      if (c >= 0) luna_set_text(c, jnum(params, "found", 0) ? "" : (g_ui.find_last[0] ? "No matches" : ""));
   } else if (method && !strcmp(method, "Page.loadEventFired")) {
      g_ui.t->loading = 0;
      ui_show_hover(g_ui.t->hover);
      g_ui.t->title_id = ui_send("Runtime.evaluate",
                              "{\"expression\":\"document.title\",\"returnByValue\":true}", 1);
   } else if (!method && g_ui.t->title_id && (unsigned)jnum(m, "id", 0) == g_ui.t->title_id) {
      const char *t = jstr(jget(jget(m, "result"), "result"), "value");
      int u = ui_el("url");
      const char *url = g_ui.t->hist && g_ui.t->cur < g_ui.t->nhist ? g_ui.t->hist[g_ui.t->cur] : (u >= 0 ? luna_get_value(u) : "");
      snprintf(g_ui.t->title, sizeof g_ui.t->title, "%s", t && *t ? t : url);
      if (ui_is_active()) luna_platform_set_title(g_ui.t->title);
      g_ui.t->title_id = 0;
      g_ui.tabs_dirty = 1;
   } else if (!method && g_ui.ctx_id && (unsigned)jnum(m, "id", 0) == g_ui.ctx_id) {
      const struct jv *r = jget(m, "result");
      g_ui.ctx_id = 0;
      if (ui_is_active() && r)
         ui_ctx_show(jstr(r, "link"), jstr(r, "image"), jstr(r, "selection"), (int)jnum(r, "editable", 0));
   } else if (!method && g_ui.shot_id && (unsigned)jnum(m, "id", 0) == g_ui.shot_id) {
      const char *data = jstr(jget(m, "result"), "data");
      size_t n = 0;
      unsigned char *png = data ? base64_decode(data, strlen(data), &n) : NULL;
      FILE *f = png ? fopen(g_ui.shot_path, "wb") : NULL;
      if (f && fwrite(png, 1, n, f) == n) fprintf(stderr, "luna-browser: wrote %s\n", g_ui.shot_path);
      else fprintf(stderr, "luna-browser: screenshot failed (%s)\n", g_ui.shot_path);
      if (f) fclose(f);
      free(png);
      g_ui.shot_id = 0;
   } else if (method && !strcmp(method, "Target.attachedToTarget")) {
      ui_send("Page.enable", NULL, 1);
      ui_send("Runtime.enable", NULL, 1);
      ui_send("Page.startScreencast", "{\"format\":\"jpeg\",\"quality\":80}", 1);
      if (g_ui.t->start) {
         char *home = strcmp(g_ui.t->start, "about:blank") ? NULL : ui_home_url();
         ui_navigate(home ? home : g_ui.t->start);
         free(home);
         free(g_ui.t->start);
         g_ui.t->start = NULL;
      }
   }
   jv_free(m);
}

static void ui_pump_one(void)
{
   if (g_ui.t->from < 0) return;
   for (;;) {
      char chunk[65536];
      ssize_t n = read(g_ui.t->from, chunk, sizeof chunk);
      if (n > 0) { buf_put(&g_ui.t->rx, chunk, (size_t)n); continue; }
      if (n == 0) { close(g_ui.t->from); g_ui.t->from = -1; ui_status("The page process ended."); }
      break;
   }
   size_t start = 0;
   for (size_t i = 0; i < g_ui.t->rx.len; ++i) {
      if (g_ui.t->rx.p[i]) continue;
      ui_message(g_ui.t->rx.p + start);
      start = i + 1;
   }
   if (start) {
      memmove(g_ui.t->rx.p, g_ui.t->rx.p + start, g_ui.t->rx.len - start);
      g_ui.t->rx.len -= start;
   }
}

/* Every tab's process is read, so a background page keeps loading and its
 * frames are acknowledged; only the active tab draws. */
static void ui_pump(void)
{
   struct ui_tab *active = g_ui.t;
   for (int i = 0; i < g_ntabs; ++i) {
      g_ui.t = g_tabs[i];
      ui_pump_one();
   }
   g_ui.t = active;
}

/* The page area's size in CSS pixels, and the device pixels behind them. */
static void ui_viewport(void)
{
   int x, y, w, h;
   luna_platform_get_window_rect(&x, &y, &w, &h);
   h -= UI_BAR;
   static float sent_zoom;
   if (w <= 0 || h <= 0 || (w == g_ui.t->sent_w && h == g_ui.t->sent_h && sent_zoom == g_ui.zoom)) return;
   char p[160];
   /* Zoom shrinks the page's CSS viewport and magnifies it by the same factor. */
   snprintf(p, sizeof p, "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%g,\"mobile\":false}",
            (int)((float)w / g_ui.zoom + 0.5f), (int)((float)h / g_ui.zoom + 0.5f), (double)(g_ui.scale * g_ui.zoom));
   ui_send("Emulation.setDeviceMetricsOverride", p, 1);
   g_ui.t->sent_w = w;
   g_ui.t->sent_h = h;
   sent_zoom = g_ui.zoom;
}

static int ui_spawn(void)
{
   int in[2], out[2];
   if (pipe(in) != 0 || pipe(out) != 0) return -1;
   char size[64];
   snprintf(size, sizeof size, "--window-size=%d,%d", g_page.w, g_page.h - UI_BAR);
   char profile[1024] = "";
   if (g_page.profile) snprintf(profile, sizeof profile, "--user-data-dir=%s", g_page.profile);
   else if (getenv("HOME")) {
      snprintf(profile, sizeof profile, "%s/.config", getenv("HOME"));
      mkdir(profile, 0700);
      snprintf(profile, sizeof profile, "--user-data-dir=%s/.config/luna-browser", getenv("HOME"));
   }
   char ua[600] = "";
   if (g_user_agent[0]) snprintf(ua, sizeof ua, "--user-agent=%s", g_user_agent);
   pid_t pid = fork();
   if (pid < 0) return -1;
   if (pid == 0) {
      dup2(in[0], 3);
      dup2(out[1], 4);
      for (int fd = 5; fd < 256; ++fd) close(fd);
      const char *argv[8] = { "luna-browser", "--remote-debugging-pipe", size };
      int n = 3;
      if (profile[0]) argv[n++] = profile;
      if (ua[0]) argv[n++] = ua;
      if (g_verbose) argv[n++] = "-v";
      argv[n] = NULL;
      execv("/proc/self/exe", (char *const *)argv);
      _exit(127);
   }
   close(in[0]);
   close(out[1]);
   g_ui.t->pid = pid;
   g_ui.t->to = in[1];
   g_ui.t->from = out[0];
   fcntl(g_ui.t->to, F_SETFD, FD_CLOEXEC);
   fcntl(g_ui.t->from, F_SETFD, FD_CLOEXEC);
   fcntl(g_ui.t->from, F_SETFL, O_NONBLOCK);
   ui_send("Target.attachToTarget", "{\"targetId\":\"" TARGET_ID "\",\"flatten\":true}", 0);
   return 0;
}

static const char ui_desktop_ua[] =
   "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
   "Chrome/124.0.0.0 Safari/537.36 " LB_PRODUCT;

static char *ui_html_escape(const char *s)
{
   struct buf b = { 0 };
   for (; *s; ++s) {
      if (*s == '<') buf_puts(&b, "&lt;");
      else if (*s == '>') buf_puts(&b, "&gt;");
      else if (*s == '&') buf_puts(&b, "&amp;");
      else if (*s == '"') buf_puts(&b, "&quot;");
      else buf_put(&b, s, 1);
   }
   return buf_take(&b);
}

static void ui_tab_click(LunaElement *e);
static void ui_tab_x_click(LunaElement *e);
static void ui_newtab_click(LunaElement *e);

/* Redraw the tab strip from g_tabs. */
static void ui_tabs_refresh(void)
{
   int host = ui_el("tabs");
   if (host < 0) return;
   struct buf b = { 0 };
   for (int i = 0; i < g_ntabs; ++i) {
      const struct ui_tab *t = g_tabs[i];
      const char *label = t->title[0] ? t->title : (t->hist && t->cur < t->nhist ? t->hist[t->cur] : "New Tab");
      char *esc = ui_html_escape(label);
      buf_printf(&b, "<div class=\"tab%s\" id=\"tab%d\">%s<span class=\"tt\">%s</span>"
                     "<span class=\"tx\" id=\"tx%d\">\xc3\x97</span></div>",
                 i == g_active ? " on" : "", i, t->loading ? "<span class=\"sp\"></span>" : "", esc, i);
      free(esc);
   }
   buf_puts(&b, "<div id=\"newtab\">+</div>");
   luna_dom_set_inner_html(host, b.p ? b.p : "");
   free(b.p);
   for (int i = 0; i < g_ntabs; ++i) {
      char id[16];
      snprintf(id, sizeof id, "tab%d", i);
      luna_set_on_click(ui_el(id), ui_tab_click);
      snprintf(id, sizeof id, "tx%d", i);
      luna_set_on_click(ui_el(id), ui_tab_x_click);
   }
   luna_set_on_click(ui_el("newtab"), ui_newtab_click);
   g_ui.tabs_dirty = 0;
   /* the restyle that follows set_inner_html drops the picture set by hand */
   if (g_ui.shown && ui_el("view") >= 0) luna_set_background_image(ui_el("view"), UI_FRAME);
   luna_invalidate_texture(UI_FRAME);
}

/* The tab whose element `e` is (or contains), by the ids ui_tabs_refresh gave. */
static int ui_tab_of(const LunaElement *e, const char *prefix)
{
   int idx = (int)(e - elements);
   for (int i = 0; i < g_ntabs; ++i) {
      char id[16];
      snprintf(id, sizeof id, "%s%d", prefix, i);
      if (ui_el(id) == idx) return i;
   }
   return -1;
}

static void ui_tab_click(LunaElement *e)
{
   int i = ui_tab_of(e, "tab");
   if (i >= 0 && g_ui.action == UA_NONE) { g_ui.action = UA_SWITCH; g_ui.action_arg = i; }
}
static void ui_tab_x_click(LunaElement *e)
{
   int i = ui_tab_of(e, "tx");
   if (i >= 0) { g_ui.action = UA_CLOSE; g_ui.action_arg = i; }
}
static void ui_newtab_click(LunaElement *e) { (void)e; g_ui.action = UA_NEW; }

/* Show tab i: the address bar, title, buttons and the page picture. */
static void ui_tab_switch(int i)
{
   if (i < 0 || i >= g_ntabs) return;
   struct ui_tab *old = g_ui.t, *t = g_tabs[i];
   if (old && old != t) {
      ui_send("Page.stopScreencast", NULL, 1);       /* a hidden page need not stream */
      g_ui.t = t;
      ui_send("Page.startScreencast", "{\"format\":\"jpeg\",\"quality\":80}", 1);
   }
   g_active = i;
   g_ui.t = t;
   t->sent_w = 0;                                    /* the viewport is sent again */
   int u = ui_el("url");
   if (u >= 0) luna_set_value(u, t->hist && t->cur < t->nhist ? t->hist[t->cur] : (t->start ? t->start : ""));
   luna_platform_set_title(t->title[0] ? t->title : "Luna Browser");
   ui_buttons();
   ui_show_hover(t->hover);
   luna_invalidate_texture(UI_FRAME);
   if (!g_ui.shown && t->frame && ui_el("view") >= 0) {
      luna_set_background_image(ui_el("view"), UI_FRAME);
      g_ui.shown = 1;
   }
   luna_app_request_redraw();
   ui_tabs_refresh();
}

static void ui_tab_open(const char *url)
{
   if (g_ntabs >= UI_MAX_TABS) return;
   struct ui_tab *t = calloc(1, sizeof *t);
   if (!t) return;
   t->pid = -1; t->to = -1; t->from = -1; t->going = -1;
   t->start = lb_strdup(url ? url : "about:blank");
   struct ui_tab *old = g_ui.t;
   g_tabs[g_ntabs++] = t;
   g_ui.t = t;
   if (ui_spawn() != 0) { ui_status("Could not start the page process."); }
   if (old && old != t) {                            /* hide the tab we came from */
      g_ui.t = old;
      ui_send("Page.stopScreencast", NULL, 1);
      g_ui.t = t;
   }
   g_active = g_ntabs - 1;
   t->sent_w = 0;
   int u = ui_el("url");
   if (u >= 0) luna_set_value(u, t->start && strcmp(t->start, "about:blank") ? t->start : "");
   luna_platform_set_title("Luna Browser");
   ui_buttons();
   ui_status(NULL);
   luna_invalidate_texture(UI_FRAME);
   luna_app_request_redraw();
   ui_tabs_refresh();
}

static void ui_tab_free(struct ui_tab *t)
{
   if (t->to >= 0) close(t->to);
   if (t->from >= 0) close(t->from);
   if (t->pid > 0) { kill(t->pid, SIGTERM); waitpid(t->pid, NULL, 0); }
   for (int i = 0; i < t->nhist; ++i) free(t->hist[i]);
   free(t->hist); free(t->frame); free(t->rx.p); free(t->start);
   free(t);
}

static void ui_tab_close(int i)
{
   if (i < 0 || i >= g_ntabs) return;
   int was_active = i == g_active;
   struct ui_tab *t = g_tabs[i];
   if (was_active) g_ui.t = NULL;
   ui_tab_free(t);
   for (int k = i; k + 1 < g_ntabs; ++k) g_tabs[k] = g_tabs[k + 1];
   g_tabs[--g_ntabs] = NULL;
   if (g_ntabs == 0) { g_active = 0; ui_tab_open("about:blank"); luna_focus_element(ui_el("url")); return; }
   if (!was_active) {
      if (i < g_active) g_active--;
      ui_tabs_refresh();
      return;
   }
   int next = i < g_ntabs ? i : g_ntabs - 1;
   g_active = next;
   struct ui_tab *n = g_tabs[next];
   g_ui.t = n;
   ui_send("Page.startScreencast", "{\"format\":\"jpeg\",\"quality\":80}", 1);
   ui_tab_switch(next);
}

/* The PC / mobile toggle: change the user agent, then load the page again. */
static void ui_toggle_desktop(void)
{
   struct ui_tab *t = g_ui.t;
   t->desktop = !t->desktop;
   struct buf b = { 0 };
   buf_puts(&b, "{\"userAgent\":");
   buf_json_str(&b, t->desktop ? ui_desktop_ua : g_user_agent);
   buf_puts(&b, "}");
   ui_send("Emulation.setUserAgentOverride", b.p, 1);
   free(b.p);
   ui_send("Page.reload", NULL, 1);
   t->loading = 1; t->committed = 0;
   ui_status("Loading\xe2\x80\xa6");
   ui_buttons();
}

static void ui_on_pc(LunaElement *e) { (void)e; g_ui.action = UA_DESKTOP; }

static void ui_run_action(void)
{
   int a = g_ui.action, arg = g_ui.action_arg;
   g_ui.action = UA_NONE;
   if (a == UA_SWITCH) ui_tab_switch(arg);
   else if (a == UA_CLOSE) ui_tab_close(arg);
   else if (a == UA_NEW) { ui_tab_open("about:blank"); luna_focus_element(ui_el("url")); }
   else if (a == UA_DESKTOP) ui_toggle_desktop();
   else if (a == UA_MENU) ui_menu_do(arg);
}

/* Screenshots, as lunaria takes them: LUNA_BROWSER_DUMP_DIR with
 * LUNA_BROWSER_SCREENSHOT_EVERY=N (every N frames), a trigger file
 * (LUNA_BROWSER_SHOT_TRIGGER, default /tmp/luna-browser-shot) that dumps one
 * more and is removed, and F12 (~/Pictures).  The page is captured at its
 * own size, as PNG. */
static void ui_shot(const char *path)
{
   if (g_ui.shot_id || !g_ntabs) return;
   snprintf(g_ui.shot_path, sizeof g_ui.shot_path, "%s", path);
   g_ui.shot_id = ui_send("Page.captureScreenshot", "{\"format\":\"png\"}", 1);
}

static void ui_shot_dump(void)
{
   const char *dir = getenv("LUNA_BROWSER_DUMP_DIR");
   if (!dir || !*dir) dir = "/tmp";
   mkdir(dir, 0755);
   char path[1024];
   snprintf(path, sizeof path, "%s/luna-browser_%04d.png", dir, g_ui.shot_seq);
   if (g_ui.shot_id) return;
   g_ui.shot_seq++;
   ui_shot(path);
}

static void ui_shot_poll(void)
{
   g_ui.frames++;
   const char *every = getenv("LUNA_BROWSER_SCREENSHOT_EVERY");
   if (every && atoi(every) > 0 && g_ui.frames % (unsigned)atoi(every) == 0) ui_shot_dump();
   if (g_ui.frames % 10) return;
   const char *trig = getenv("LUNA_BROWSER_SHOT_TRIGGER");
   if (!trig || !*trig) trig = "/tmp/luna-browser-shot";
   if (access(trig, F_OK) == 0) { unlink(trig); ui_shot_dump(); }
}

static void ui_shot_pictures(void)
{
   const char *home = getenv("HOME");
   if (!home) return;
   char dir[512], path[1100];
   snprintf(dir, sizeof dir, "%s/Pictures", home);
   mkdir(dir, 0755);
   time_t t = time(NULL);
   struct tm tm;
   localtime_r(&t, &tm);
   char stamp[64];
   strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tm);
   snprintf(path, sizeof path, "%s/Luna Browser %s.png", dir, stamp);
   ui_shot(path);
}

static void ui_wheel_pump(void);

/* The cat runs on the white page while the active tab loads. */
static void ui_cat(void)
{
   int busy = g_ntabs && g_tabs[g_active]->loading;
   int want = busy && !g_tabs[g_active]->committed;
   int cat = ui_el("cat");
   if (cat < 0) return;
   if (want != g_ui.cat_on) {
      g_ui.cat_on = want;
      if (want) luna_add_class(cat, "on"); else luna_remove_class(cat, "on");
   }
   if (want) {                                      /* the next picture of the cat, one change of one box */
      int frame = (int)(lb_now_ms() / 110.0) & 3;
      if (frame != g_ui.cat_frame) {
         char st[48];
         snprintf(st, sizeof st, "background-position:-%dpx 0px", frame * 128);
         luna_dom_set_attr(ui_el("cf"), "style", st);
         g_ui.cat_frame = frame;
      }
   } else g_ui.cat_frame = -1;
   if (busy) luna_app_request_redraw();            /* the spinners turn smoothly */
}

static void ui_init(void *user)
{
   (void)user;
   g_luna_platform.read_resource = ui_read_resource;
   luna_set_on_click(ui_el("back"), ui_on_back);
   luna_set_on_click(ui_el("fwd"), ui_on_fwd);
   luna_set_on_click(ui_el("reload"), ui_on_reload);
   luna_set_on_click(ui_el("pc"), ui_on_pc);
   const int blank = g_ui.start && !strcmp(g_ui.start, "about:blank");
   ui_tab_open(g_ui.start);
   free(g_ui.start);
   g_ui.start = NULL;
   if (blank) luna_focus_element(ui_el("url"));
}

static void ui_frame(double dt, void *user)
{
   (void)dt; (void)user;
   ui_pump();
   if (g_ui.action) ui_run_action();
   unsigned mask = 0;
   for (int i = 0; i < g_ntabs; ++i) if (g_tabs[i]->loading) mask |= 1u << i;
   if (mask != g_ui.load_mask) {
      g_ui.load_mask = mask;
      g_ui.tabs_dirty = 1;
      int sp = ui_el("spin");
      if (sp >= 0 && g_ntabs) { if (g_tabs[g_active]->loading) luna_add_class(sp, "on"); else luna_remove_class(sp, "on"); }
   }
   if (g_ui.tabs_dirty) ui_tabs_refresh();
   ui_cat();
   ui_wheel_pump();
   ui_shot_poll();
   ui_viewport();
   if (g_ui.find_on) {                                   /* find as you type */
      const char *t = luna_get_value(ui_el("find"));
      if (t && strcmp(t, g_ui.find_last)) {
         snprintf(g_ui.find_last, sizeof g_ui.find_last, "%s", t);
         struct buf b = { 0 };
         buf_puts(&b, "{\"text\":");
         buf_json_str(&b, t);
         buf_puts(&b, ",\"forward\":1,\"restart\":1}");
         ui_send("Lunaria.find", b.p, 1);
         free(b.p);
      }
   }
}

static void ui_render(int fbw, int fbh, void *user)
{
   (void)user;
   int x, y, w, h;
   luna_platform_get_window_rect(&x, &y, &w, &h);
   float s = w > 0 ? (float)fbw / (float)w : 1.0f;
   (void)fbh;
   if (s > 0.5f && fabsf(s - g_ui.scale) > 0.01f) { g_ui.scale = s; g_ui.t->sent_w = 0; }
}

static int ui_in_view(double y) { return y >= UI_BAR; }

/* luna-ui's modifier mask as CDP's (alt 1, ctrl 2, meta 4, shift 8). */
static int ui_cdp_mods(int mods)
{
   return ((mods & LUNA_MOD_ALT) ? 1 : 0) | ((mods & LUNA_MOD_CONTROL) ? 2 : 0) |
          ((mods & LUNA_MOD_SUPER) ? 4 : 0) | ((mods & LUNA_MOD_SHIFT) ? 8 : 0);
}

/* Hands the system clipboard to the page, for a paste it may be asked for. */
static void ui_push_clipboard(void)
{
   char *c = luna_clipboard_get();
   struct buf b = { 0 };
   buf_puts(&b, "{\"text\":");
   buf_json_str(&b, c ? c : "");
   buf_puts(&b, "}");
   ui_send("Lunaria.setClipboard", b.p, 1);
   free(b.p);
   if (c) luna_clipboard_free(c);
}

/* ---------------------------------------------------------------- the page's right-click menu
 * The page process says what is under the pointer (link, picture, selection,
 * an editable field); the window draws the menu and carries out the choice. */
enum { MC_NONE, MC_BACK, MC_FWD, MC_RELOAD, MC_LINK_TAB, MC_LINK_COPY, MC_IMG_TAB, MC_IMG_COPY,
       MC_SEL_COPY, MC_SEL_SEARCH, MC_CUT, MC_COPY, MC_PASTE, MC_SELALL, MC_DESKTOP };

static void ui_menu_click(LunaElement *e);

static void ui_ctx_close(void)
{
   int host = ui_el("ctx");
   if (host >= 0) luna_remove_class(host, "on");
   for (int i = 0; i < 16; ++i) { free(g_ui.ctx_arg[i]); g_ui.ctx_arg[i] = NULL; }
   g_ui.ctx_open = 0; g_ui.ctx_n = 0; g_ui.ctx_id = 0;
}

static void ui_ctx_add(struct buf *html, int cmd, const char *label, const char *arg, int on)
{
   if (g_ui.ctx_n >= 16) return;
   int i = g_ui.ctx_n++;
   g_ui.ctx_cmd[i] = on ? cmd : MC_NONE;
   g_ui.ctx_arg[i] = arg ? lb_strdup(arg) : NULL;
   char *esc = ui_html_escape(label);
   buf_printf(html, "<div class=\"mi%s\" id=\"mi%d\">%s</div>", on ? "" : " off", i, esc);
   free(esc);
}

static void ui_ctx_sep(struct buf *html) { buf_puts(html, "<div class=\"msep\"></div>"); }

/* Open the menu for what the page reported. */
static void ui_ctx_show(const char *link, const char *image, const char *sel, int editable)
{
   int host = ui_el("ctx");
   if (host < 0) return;
   for (int i = 0; i < 16; ++i) { free(g_ui.ctx_arg[i]); g_ui.ctx_arg[i] = NULL; }
   g_ui.ctx_n = 0;
   struct buf h = { 0 };
   if (link && *link) {
      ui_ctx_add(&h, MC_LINK_TAB, "リンクを新しいタブで開く", link, 1);
      ui_ctx_add(&h, MC_LINK_COPY, "リンクのアドレスをコピー", link, 1);
      ui_ctx_sep(&h);
   }
   if (image && *image) {
      ui_ctx_add(&h, MC_IMG_TAB, "画像を新しいタブで開く", image, 1);
      ui_ctx_add(&h, MC_IMG_COPY, "画像のアドレスをコピー", image, 1);
      ui_ctx_sep(&h);
   }
   if (editable) {
      ui_ctx_add(&h, MC_CUT, "切り取り", NULL, 1);
      ui_ctx_add(&h, MC_COPY, "コピー", NULL, 1);
      ui_ctx_add(&h, MC_PASTE, "貼り付け", NULL, 1);
      ui_ctx_add(&h, MC_SELALL, "すべて選択", NULL, 1);
      ui_ctx_sep(&h);
   } else if (sel && *sel) {
      ui_ctx_add(&h, MC_SEL_COPY, "コピー", sel, 1);
      ui_ctx_add(&h, MC_SEL_SEARCH, "選択範囲を検索", sel, 1);
      ui_ctx_sep(&h);
   }
   struct ui_tab *t = g_ui.t;
   ui_ctx_add(&h, MC_BACK, "戻る", NULL, t->cur > 0);
   ui_ctx_add(&h, MC_FWD, "進む", NULL, t->cur + 1 < t->nhist);
   ui_ctx_add(&h, MC_RELOAD, "再読み込み", NULL, 1);
   ui_ctx_add(&h, MC_DESKTOP, t->desktop ? "モバイル版サイトに切り替え" : "PC版サイトに切り替え", NULL, 1);
   luna_dom_set_inner_html(host, h.p ? h.p : "");
   free(h.p);
   int x, y, w, ht;
   luna_platform_get_window_rect(&x, &y, &w, &ht);
   int mw = 240, mh = g_ui.ctx_n * 30 + 20;
   int px = (int)g_ui.ctx_x, py = (int)g_ui.ctx_y;
   if (px + mw > w) px = w - mw;
   if (py + mh > ht) py = ht - mh;
   if (px < 0) px = 0;
   if (py < 0) py = 0;
   char st[64];
   snprintf(st, sizeof st, "left:%dpx;top:%dpx;", px, py);
   luna_dom_set_attr(host, "style", st);
   for (int i = 0; i < g_ui.ctx_n; ++i) {
      char id[16];
      snprintf(id, sizeof id, "mi%d", i);
      luna_set_on_click(ui_el(id), ui_menu_click);
   }
   luna_add_class(host, "on");
   g_ui.ctx_open = 1;
   luna_app_request_redraw();
}

static void ui_menu_click(LunaElement *e)
{
   int idx = (int)(e - elements);
   for (int i = 0; i < g_ui.ctx_n; ++i) {
      char id[16];
      snprintf(id, sizeof id, "mi%d", i);
      if (ui_el(id) == idx) { g_ui.action = UA_MENU; g_ui.action_arg = i; return; }
   }
}

static void ui_key_chord(const char *key, int vk)
{
   char p[200];
   snprintf(p, sizeof p, "{\"type\":\"rawKeyDown\",\"key\":\"%s\",\"windowsVirtualKeyCode\":%d,\"modifiers\":2}", key, vk);
   ui_send("Input.dispatchKeyEvent", p, 1);
   snprintf(p, sizeof p, "{\"type\":\"keyUp\",\"key\":\"%s\",\"windowsVirtualKeyCode\":%d,\"modifiers\":2}", key, vk);
   ui_send("Input.dispatchKeyEvent", p, 1);
}

static void ui_menu_do(int i)
{
   if (i < 0 || i >= g_ui.ctx_n) return;
   int cmd = g_ui.ctx_cmd[i];
   char *arg = g_ui.ctx_arg[i] ? lb_strdup(g_ui.ctx_arg[i]) : NULL;
   ui_ctx_close();
   switch (cmd) {
   case MC_BACK: ui_go(-1); break;
   case MC_FWD: ui_go(1); break;
   case MC_RELOAD: ui_on_reload(NULL); break;
   case MC_DESKTOP: ui_toggle_desktop(); break;
   case MC_LINK_TAB: case MC_IMG_TAB: if (arg) ui_tab_open(arg); break;
   case MC_LINK_COPY: case MC_IMG_COPY: case MC_SEL_COPY: if (arg) luna_clipboard_set(arg); break;
   case MC_SEL_SEARCH:
      if (arg) {
         char *u = ui_search_url(arg);
         ui_tab_open(u);
         free(u);
      }
      break;
   case MC_CUT: ui_key_chord("x", 88); break;
   case MC_COPY: ui_key_chord("c", 67); break;
   case MC_SELALL: ui_key_chord("a", 65); break;
   case MC_PASTE: {
      char *clip = luna_clipboard_get();
      if (clip && *clip) {
         struct buf b = { 0 };
         buf_puts(&b, "{\"text\":");
         buf_json_str(&b, clip);
         buf_puts(&b, "}");
         ui_send("Input.insertText", b.p, 1);
         free(b.p);
      }
      if (clip) luna_clipboard_free(clip);
      break;
   }
   default: break;
   }
   free(arg);
}

/* Ask the page what is under the pointer; the menu opens when it answers. */
static void ui_ctx_request(double wx, double wy)
{
   ui_ctx_close();
   char p[120];
   snprintf(p, sizeof p, "{\"x\":%.1f,\"y\":%.1f}", wx / g_ui.zoom, (wy - UI_BAR) / g_ui.zoom);
   g_ui.ctx_x = wx; g_ui.ctx_y = wy;
   g_ui.ctx_id = ui_send("Lunaria.contextInfo", p, 1);
}

static int ui_in_ctx_menu(double x, double y)
{
   int host = ui_el("ctx");
   if (host < 0) return 0;
   luna_dom_flush_restyle();
   for (int i = luna_element_at_point(x, y), g = 0; i >= 0 && g < 16; i = elements[i].parent_idx, ++g)
      if (i == host) return 1;
   return 0;
}

static void ui_mouse(const char *type, double x, double y, int button, int clicks, int mods)
{
   char p[240];
   static const char *names[] = { "left", "right", "middle" };
   snprintf(p, sizeof p, "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"button\":\"%s\",\"clickCount\":%d,\"modifiers\":%d}",
            type, x / g_ui.zoom, (y - UI_BAR) / g_ui.zoom, button >= 0 && button < 3 ? names[button] : "none", clicks, ui_cdp_mods(mods));
   ui_send("Input.dispatchMouseEvent", p, 1);
}

static int ui_mouse_button(int button, int action, int mods, double x, double y, void *user)
{
   (void)user;
   if (button == LUNA_MOUSE_BUTTON_LEFT && !ui_in_view(y)) {
      int u = ui_el("url");
      if (action == LUNA_PRESS) g_ui.url_click = u >= 0 && luna_focused_element() != u && (luna_dom_flush_restyle(), luna_element_at_point(x, y)) == u;
      else if (action == LUNA_RELEASE && g_ui.url_click) { g_ui.url_click = 0; luna_input_select_all(u); }
   }
   if (g_ui.ctx_open && action == LUNA_PRESS) {
      if (ui_in_ctx_menu(x, y)) return 0;                  /* a menu item takes the click */
      ui_ctx_close();
      return 1;
   }
   if (g_ui.ctx_open && ui_in_ctx_menu(x, y)) return 0;
   if (button == LUNA_MOUSE_BUTTON_RIGHT && ui_in_view(y)) {
      if (action == LUNA_PRESS) { ui_push_clipboard(); ui_ctx_request(x, y); }
      return 1;                                           /* the menu is ours, not the page's */
   }
   if (action == LUNA_PRESS && ui_in_view(y)) {
      luna_focus_element(-1);          /* the page has the keyboard now */
      g_ui.pressed = 1;
      if (button == LUNA_MOUSE_BUTTON_RIGHT) ui_push_clipboard();
      ui_mouse("mousePressed", x, y, button, 1, mods);
      return 1;
   }
   if (action == LUNA_RELEASE && g_ui.pressed) {
      g_ui.pressed = 0;
      ui_mouse("mouseReleased", x, y, button, 1, mods);
      return 1;
   }
   return 0;
}

static int ui_mouse_move(double x, double y, void *user)
{
   (void)user;
   g_ui.mx = x; g_ui.my = y;
   if (ui_in_view(y) || g_ui.pressed) ui_mouse("mouseMoved", x, y, g_ui.pressed ? 0 : -1, 0, 0);
   return 0;
}

static int ui_scroll(double dx, double dy, void *user)
{
   (void)user;
   if (!ui_in_view(g_ui.my)) return 0;
   /* A notch is about 90 px; it is eased out over the next frames rather than
    * jumped, so scrolling looks continuous. */
   g_ui.wheel_x += -dx * 90.0;
   g_ui.wheel_y += -dy * 90.0;
   g_ui.wheel_at_x = g_ui.mx; g_ui.wheel_at_y = g_ui.my;
   return 1;
}

/* Called every frame: sends a share of the pending wheel distance. */
static void ui_wheel_pump(void)
{
   if (fabs(g_ui.wheel_x) < 0.5 && fabs(g_ui.wheel_y) < 0.5) { g_ui.wheel_x = g_ui.wheel_y = 0.0; return; }
   double fx = g_ui.wheel_x * 0.35, fy = g_ui.wheel_y * 0.35;
   if (fabs(g_ui.wheel_x) < 6.0) fx = g_ui.wheel_x;        /* finish small remainders */
   if (fabs(g_ui.wheel_y) < 6.0) fy = g_ui.wheel_y;
   g_ui.wheel_x -= fx; g_ui.wheel_y -= fy;
   char p[200];
   snprintf(p, sizeof p, "{\"type\":\"mouseWheel\",\"x\":%.1f,\"y\":%.1f,\"deltaX\":%.2f,\"deltaY\":%.2f}",
            g_ui.wheel_at_x / g_ui.zoom, (g_ui.wheel_at_y - UI_BAR) / g_ui.zoom, fx / g_ui.zoom, fy / g_ui.zoom);
   ui_send("Input.dispatchMouseEvent", p, 1);
}

static int ui_page_has_keys(void)
{
   int f = luna_focused_element();
   return f != ui_el("url") && f != ui_el("find");
}

static int ui_key(int key, int scancode, int action, int mods, void *user)
{
   (void)scancode; (void)user;
   const int down = action != LUNA_RELEASE;
   const int ctrl = mods & (LUNA_MOD_CONTROL | LUNA_MOD_SUPER);
   if (down && key == LUNA_KEY_F12) { ui_shot_pictures(); return 1; }
   if (down && g_ui.ctx_open && key == LUNA_KEY_ESCAPE) { ui_ctx_close(); return 1; }
   /* tabs */
   if (down && ctrl && key == LUNA_KEY_T) { ui_tab_open("about:blank"); luna_focus_element(ui_el("url")); return 1; }
   if (down && ctrl && key == LUNA_KEY_W) { ui_tab_close(g_active); return 1; }
   if (down && ctrl && g_ntabs > 1 &&
       (key == LUNA_KEY_TAB || key == LUNA_KEY_PAGE_DOWN || key == LUNA_KEY_PAGE_UP)) {
      int back = key == LUNA_KEY_PAGE_UP || (key == LUNA_KEY_TAB && (mods & LUNA_MOD_SHIFT));
      ui_tab_switch((g_active + (back ? g_ntabs - 1 : 1)) % g_ntabs);
      return 1;
   }
   if (down && ctrl && key >= LUNA_KEY_1 && key <= LUNA_KEY_9) {
      int n = key - LUNA_KEY_1;
      ui_tab_switch(key == LUNA_KEY_9 || n >= g_ntabs ? g_ntabs - 1 : n);
      return 1;
   }
   /* the browser's own keys first */
   if (down && ((ctrl && key == LUNA_KEY_L) || key == LUNA_KEY_F6)) {
      int u = ui_el("url");
      luna_focus_element(u);
      luna_input_select_all(u);
      return 1;
   }
   if (down && ctrl && key == LUNA_KEY_F) {                       /* find in page */
      int f = ui_el("find"), box = ui_el("findbox");
      if (box >= 0) luna_add_class(box, "on");
      g_ui.find_on = 1;
      if (f >= 0) { luna_focus_element(f); luna_input_select_all(f); }
      return 1;
   }
   if (down && ctrl && (key == LUNA_KEY_EQUAL || key == LUNA_KEY_KP_ADD || key == LUNA_KEY_MINUS ||
                        key == LUNA_KEY_KP_SUBTRACT || key == LUNA_KEY_0 || key == LUNA_KEY_KP_0)) {   /* zoom */
      static const float steps[] = { 0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f };
      int n = (int)(sizeof steps / sizeof steps[0]), cur = 0;
      for (int i = 0; i < n; ++i) if (steps[i] <= g_ui.zoom + 0.001f) cur = i;
      if (key == LUNA_KEY_0 || key == LUNA_KEY_KP_0) g_ui.zoom = 1.0f;
      else if (key == LUNA_KEY_EQUAL || key == LUNA_KEY_KP_ADD) g_ui.zoom = steps[cur + 1 < n ? cur + 1 : cur];
      else g_ui.zoom = steps[cur > 0 ? cur - 1 : 0];
      char z[48];
      snprintf(z, sizeof z, "%d%%", (int)(g_ui.zoom * 100.0f + 0.5f));
      ui_status(g_ui.zoom == 1.0f ? NULL : z);
      return 1;
   }
   if (down && ((ctrl && key == LUNA_KEY_R) || key == LUNA_KEY_F5)) { ui_on_reload(NULL); return 1; }
   if (down && (mods & LUNA_MOD_ALT) && key == LUNA_KEY_LEFT) { ui_go(-1); return 1; }
   if (down && (mods & LUNA_MOD_ALT) && key == LUNA_KEY_RIGHT) { ui_go(1); return 1; }
   if (!ui_page_has_keys()) {
      if (luna_focused_element() == ui_el("find")) {
         if (down && key == LUNA_KEY_ENTER) {
            const char *t = luna_get_value(ui_el("find"));
            if (t && *t) {
               struct buf b = { 0 };
               buf_puts(&b, "{\"text\":");
               buf_json_str(&b, t);
               buf_printf(&b, ",\"forward\":%d}", (mods & LUNA_MOD_SHIFT) ? 0 : 1);
               ui_send("Lunaria.find", b.p, 1);
               free(b.p);
            }
            return 1;
         }
         if (down && key == LUNA_KEY_ESCAPE) {
            g_ui.find_on = 0;
            luna_remove_class(ui_el("findbox"), "on");
            ui_send("Lunaria.find", "{\"text\":\"\"}", 1);
            luna_focus_element(-1);
            return 1;
         }
         return 0;
      }
      if (down && key == LUNA_KEY_ENTER) {
         char *url = ui_typed_url(luna_get_value(ui_el("url")));
         if (url) { ui_navigate(url); free(url); }
         luna_focus_element(-1);
         return 1;
      }
      if (down && key == LUNA_KEY_ESCAPE) {
         if (g_ui.t->cur < g_ui.t->nhist) luna_set_value(ui_el("url"), g_ui.t->hist[g_ui.t->cur]);
         luna_focus_element(-1);
         return 1;
      }
      return 0;                        /* typing in the address bar */
   }
   if (ctrl && key == LUNA_KEY_V) {      /* paste: the system clipboard, as typed text */
      if (down) {
         char *c = luna_clipboard_get();
         if (c && *c) {
            struct buf b = { 0 };
            buf_puts(&b, "{\"text\":");
            buf_json_str(&b, c);
            buf_puts(&b, "}");
            ui_send("Input.insertText", b.p, 1);
            free(b.p);
         }
         if (c) luna_clipboard_free(c);
      }
      return 1;
   }
   if (ctrl && key >= LUNA_KEY_A && key <= LUNA_KEY_Z) {      /* Ctrl+A, Ctrl+C, Ctrl+X ... */
      char p[200];
      snprintf(p, sizeof p, "{\"type\":\"%s\",\"key\":\"%c\",\"windowsVirtualKeyCode\":%d,\"modifiers\":%d}",
               down ? "rawKeyDown" : "keyUp", tolower(key), key, ui_cdp_mods(mods));
      ui_send("Input.dispatchKeyEvent", p, 1);
      return 1;
   }
   for (int i = 0; g_keys[i].name; ++i) {
      if (g_keys[i].key != key) continue;
      char p[200];
      snprintf(p, sizeof p, "{\"type\":\"%s\",\"key\":\"%s\",\"windowsVirtualKeyCode\":%d,\"modifiers\":%d}",
               down ? "rawKeyDown" : "keyUp", g_keys[i].name, g_keys[i].vk, ui_cdp_mods(mods));
      ui_send("Input.dispatchKeyEvent", p, 1);
      return 1;
   }
   return 0;
}

static int ui_char(unsigned int cp, void *user)
{
   (void)user;
   if (!ui_page_has_keys() || cp < 0x20) return 0;
   char utf8[5] = { 0 };
   if (cp < 0x80) utf8[0] = (char)cp;
   else if (cp < 0x800) { utf8[0] = (char)(0xc0 | cp >> 6); utf8[1] = (char)(0x80 | (cp & 63)); }
   else if (cp < 0x10000) { utf8[0] = (char)(0xe0 | cp >> 12); utf8[1] = (char)(0x80 | ((cp >> 6) & 63)); utf8[2] = (char)(0x80 | (cp & 63)); }
   else { utf8[0] = (char)(0xf0 | cp >> 18); utf8[1] = (char)(0x80 | ((cp >> 12) & 63)); utf8[2] = (char)(0x80 | ((cp >> 6) & 63)); utf8[3] = (char)(0x80 | (cp & 63)); }
   struct buf b = { 0 };
   buf_puts(&b, "{\"text\":");
   buf_json_str(&b, utf8);
   buf_puts(&b, "}");
   ui_send("Input.insertText", b.p, 1);
   free(b.p);
   return 1;
}

static void ui_shutdown(void *user)
{
   (void)user;
   for (int i = 0; i < g_ntabs; ++i) { ui_tab_free(g_tabs[i]); g_tabs[i] = NULL; }
   g_ntabs = 0;
   g_ui.t = NULL;
}

static int window_main(const char *url)
{
   g_ui.start = lb_strdup(url);
   LunaAppConfig cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.title = "Luna Browser";
   cfg.width = g_page.w;
   cfg.height = g_page.h;
   cfg.resizable = 1;
   cfg.vsync = 1;
   cfg.html = ui_html;
   cfg.css = ui_css;
   cfg.on_init = ui_init;
   cfg.on_frame = ui_frame;
   cfg.on_render = ui_render;
   cfg.on_shutdown = ui_shutdown;
   cfg.on_mouse_button = ui_mouse_button;
   cfg.on_mouse_move = ui_mouse_move;
   cfg.on_scroll = ui_scroll;
   cfg.on_key = ui_key;
   cfg.on_char = ui_char;
   cfg.frame_interval = 1.0 / 60.0;
   return luna_app_run(&cfg);
}
#endif

int main(int argc, char **argv)
{
   signal(SIGPIPE, SIG_IGN);
   g_page.w = 1024;
   g_page.h = 576;
   g_page.scale = 1.0f;
   int pipe_mode = 0, dump = 0;
   const char *shot = NULL, *url = NULL;
   double budget = 500;
   for (int i = 1; i < argc; ++i) {
      const char *a = argv[i];
      if (!strcmp(a, "--remote-debugging-pipe")) pipe_mode = 1;
      else if (!strcmp(a, "--screenshot")) shot = "screenshot.png";
      else if (!strncmp(a, "--screenshot=", 13)) shot = a + 13;
      else if (!strcmp(a, "--dump-dom")) dump = 1;
      else if (!strncmp(a, "--window-size=", 14)) sscanf(a + 14, "%d,%d", &g_page.w, &g_page.h);
      else if (!strncmp(a, "--force-device-scale-factor=", 28)) g_page.scale = (float)atof(a + 28);
      else if (!strncmp(a, "--virtual-time-budget=", 22)) budget = atof(a + 22);
      else if (!strncmp(a, "--user-data-dir=", 16)) {
         g_page.profile = lb_strdup(a + 16);
         mkdir(g_page.profile, 0700);
      } else if (!strncmp(a, "--user-agent=", 13)) snprintf(g_user_agent, sizeof g_user_agent, "%s", a + 13);
      else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) g_verbose = 1;
      else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
      else if (!strncmp(a, "--", 2)) LB_LOG("ignoring %s\n", a);   /* Chrome switches */
      else url = a;
   }
   if (getenv("LUNA_BROWSER_VERBOSE")) g_verbose = 1;
   g_t0 = lb_now_ms();
   if (pipe(g_wake) == 0) {
      fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
      fcntl(g_wake[1], F_SETFL, O_NONBLOCK);
      fcntl(g_wake[0], F_SETFD, FD_CLOEXEC);
      fcntl(g_wake[1], F_SETFD, FD_CLOEXEC);
   }
#ifndef LB_NO_TLS
   (void)tls_context();       /* before any worker thread can race to it */
#endif
   char *start = arg_url(url ? url : "about:blank");
#ifdef LB_WINDOW
   if (!pipe_mode && !shot && !dump) return window_main(start);
#endif
   if (!pipe_mode && !shot && !dump) {
#ifndef LB_WINDOW
      fputs("luna-browser: built without a window (LB_WINDOW=1); use --screenshot, "
            "--dump-dom or --remote-debugging-pipe\n", stderr);
#endif
      usage();
      return 2;
   }
   if (headless_init() != 0) return 1;
   if (pipe_mode) {
      struct stat st;
      if (fstat(g_cdp_in, &st) != 0 || fstat(g_cdp_out, &st) != 0) {
         fputs("luna-browser: --remote-debugging-pipe needs fds 3 and 4\n", stderr);
         return 2;
      }
      page_load_html("", "about:blank");
      if (strcmp(start, "about:blank")) { g_page.nav_url = start; start = NULL; }
      free(start);
      return cdp_loop();
   }
   page_navigate(start);
   free(start);
   settle(budget);
   int rc = 0;
   if (dump) {
      struct buf b = { 0 };
      for (int i = 0; i < elem_count; ++i)
         if (el_node(i) && elements[i].parent_idx == -1 && !elements[i].dom_detached) html_of(i, &b, 1);
      printf("%s\n", b.p ? b.p : "");
      free(b.p);
   }
   if (shot) {
      size_t len = 0;
      /* Background pictures are only asked for when first painted: paint once,
       * wait for them, then take the picture. */
      for (int pass = 0; pass < 5; ++pass) {      /* a paint can start more pictures: until none are left */
         page_render();
         if (g_jobs_out == 0) break;
         settle(1500);
      }
      unsigned char *png = page_capture(&len, 0);
      FILE *f = png ? fopen(shot, "wb") : NULL;
      if (f && fwrite(png, 1, len, f) == len) fprintf(stderr, "luna-browser: wrote %s (%dx%d)\n", shot, g_page.w, g_page.h);
      else { fprintf(stderr, "luna-browser: screenshot failed\n"); rc = 1; }
      if (f) fclose(f);
      free(png);
   }
   page_teardown();
   return rc;
}
