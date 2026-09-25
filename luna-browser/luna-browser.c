/*
 * luna-browser — a small web engine: luna-ui lays out and paints the
 * document, QuickJS runs its scripts.  C11.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 *
 * One process is one page.  It is driven three ways:
 *
 *   luna-browser --remote-debugging-pipe [--user-data-dir=DIR] [URL]
 *       Headless, speaking the Chrome DevTools Protocol subset an embedder
 *       needs (targets, navigation, screenshots, input, Runtime.evaluate,
 *       bindings) as NUL-terminated JSON on file descriptors 3 (in) and
 *       4 (out) — the same pipe Chromium's --remote-debugging-pipe uses, so
 *       an embedder that drives Chrome drives this unchanged.
 *   luna-browser --screenshot[=FILE] [--window-size=W,H] URL
 *       Headless: load, let timers settle, write a PNG, exit.
 *   luna-browser [--window-size=W,H] URL          (built with LB_WINDOW=1)
 *       An interactive window through luna-ui's native host.
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

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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
#define LB_LOG(...) do { if (g_verbose) fprintf(stderr, "[luna-browser] " __VA_ARGS__); } while (0)

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
#ifndef LB_NO_TLS
   SSL *ssl;
#endif
};

#ifndef LB_NO_TLS
static SSL_CTX *g_ssl_ctx;
static SSL_CTX *tls_context(void)
{
   if (g_ssl_ctx) return g_ssl_ctx;
   g_ssl_ctx = SSL_CTX_new(TLS_client_method());
   if (!g_ssl_ctx) return NULL;
   SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
   SSL_CTX_set_default_verify_paths(g_ssl_ctx);
   SSL_CTX_set_verify(g_ssl_ctx, getenv("LUNA_BROWSER_INSECURE") ? SSL_VERIFY_NONE
                                                                 : SSL_VERIFY_PEER, NULL);
   return g_ssl_ctx;
}
#endif

static ptrdiff_t conn_write(struct conn *c, const void *p, size_t n)
{
#ifndef LB_NO_TLS
   if (c->ssl) return SSL_write(c->ssl, p, (int)n);
#endif
   return send(c->fd, p, n, MSG_NOSIGNAL);
}

static ptrdiff_t conn_read(struct conn *c, void *p, size_t n)
{
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
   if (c->fd >= 0) close(c->fd);
   c->fd = -1;
}

static int conn_open(struct conn *c, const struct url *u, char **error)
{
   c->fd = -1;
#ifndef LB_NO_TLS
   c->ssl = NULL;
#endif
   int tls = !strcmp(u->scheme, "https");
   char port[16];
   snprintf(port, sizeof port, "%d", u->port >= 0 ? u->port : tls ? 443 : 80);
   char host[256];
   snprintf(host, sizeof host, "%s", u->host);
   if (host[0] == '[') { memmove(host, host + 1, strlen(host)); host[strcspn(host, "]")] = 0; }
   struct addrinfo hints = { 0 }, *res = NULL;
   hints.ai_socktype = SOCK_STREAM;
   int rc = getaddrinfo(host, port, &hints, &res);
   if (rc != 0) { *error = lb_strdup(gai_strerror(rc)); return -1; }
   for (struct addrinfo *a = res; a; a = a->ai_next) {
      c->fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
      if (c->fd < 0) continue;
      struct timeval tv = { 30, 0 };
      setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      if (connect(c->fd, a->ai_addr, a->ai_addrlen) == 0) break;
      close(c->fd);
      c->fd = -1;
   }
   freeaddrinfo(res);
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
   SSL_set_tlsext_host_name(c->ssl, host);
   if (!getenv("LUNA_BROWSER_INSECURE")) SSL_set1_host(c->ssl, host);
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

/* One request/response exchange, no redirects. */
static int http_exchange(const char *url, const char *method, const char *extra_headers,
                         const unsigned char *body, size_t body_len, struct response *r)
{
   struct url u;
   if (url_parse(url, &u) != 0 || !u.host) { url_free(&u); r->error = lb_strdup("bad URL"); return -1; }
   struct conn c;
   if (conn_open(&c, &u, &r->error) != 0) { url_free(&u); return -1; }
   char *cookies = cookie_header(url);
   struct buf req = { 0 };
   buf_printf(&req, "%s %s%s%s HTTP/1.1\r\nHost: %s", method, u.path && u.path[0] ? u.path : "/",
              u.query ? "?" : "", u.query ? u.query : "", u.host);
   if (u.port >= 0) buf_printf(&req, ":%d", u.port);
   buf_printf(&req, "\r\nUser-Agent: %s\r\nAccept-Encoding: identity\r\n"
              "Connection: close\r\n", g_user_agent);
   if (!extra_headers || !strcasestr(extra_headers, "accept:"))
      buf_puts(&req, "Accept: */*\r\n");
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
      ptrdiff_t n = conn_write(&c, req.p + sent, req.len - (size_t)sent);
      if (n <= 0) break;
      sent += n;
   }
   free(req.p);
   struct buf in = { 0 };
   char chunk[16384];
   for (;;) {
      ptrdiff_t n = conn_read(&c, chunk, sizeof chunk);
      if (n <= 0) break;
      buf_put(&in, chunk, (size_t)n);
   }
   conn_close(&c);
   url_free(&u);
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
   return 0;
}

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

/* GET or any method; follows redirects.  Always fills r (r->error on
 * failure). */
static int lb_fetch(const char *url, const char *method, const char *headers,
                    const unsigned char *body, size_t body_len, struct response *r)
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

/* luna-ui's resource reader: every relative reference in the document is a
 * URL relative to the page.  Local font files are the engine's own and are
 * left to its plain file loader. */
static unsigned char *lb_read_resource(const char *path, size_t *out_size)
{
   *out_size = 0;
   if (!path || !path[0] || !g_page.url) return NULL;
   if (path[0] == '/' && path[1] != '/' && (is_font_path(path) ||
       !strncmp(g_page.url, "file:", 5) || !strncmp(g_page.url, "about:", 6)))
      return NULL;
   char *abs = url_resolve(g_page.url, path);
   if (!abs) return NULL;
   for (int i = 0; i < g_page.ncache; ++i) {
      if (strcmp(g_page.cache[i].url, abs)) continue;
      free(abs);
      unsigned char *copy = malloc(g_page.cache[i].len + 1);
      if (!copy) return NULL;
      memcpy(copy, g_page.cache[i].data, g_page.cache[i].len);
      copy[g_page.cache[i].len] = 0;
      *out_size = g_page.cache[i].len;
      return copy;
   }
   struct response r;
   int ok = lb_fetch(abs, "GET", NULL, NULL, 0, &r) == 0 && r.status >= 200 && r.status < 300;
   LB_LOG("resource %s -> %d %s\n", abs, r.status, r.error ? r.error : "");
   unsigned char *data = NULL;
   if (ok) {
      void *grown = realloc(g_page.cache, sizeof *g_page.cache * (size_t)(g_page.ncache + 1));
      if (grown) {
         g_page.cache = grown;
         g_page.cache[g_page.ncache].url = abs;
         g_page.cache[g_page.ncache].data = r.body;
         g_page.cache[g_page.ncache].len = r.length;
         g_page.ncache++;
         abs = NULL;
         data = malloc(r.length + 1);
         if (data) { memcpy(data, r.body, r.length); data[r.length] = 0; *out_size = r.length; }
         r.body = NULL;
      }
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
   PS_NTH = 512, PS_NTH_LAST = 1024, PS_NEVER = 2048,
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
   const char *own = elements[i].is_input ? "" : elements[i].text;
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
   const char *own = e->is_input && strcasecmp(e->type, "textarea") ? "" : e->text;
   if (!kids || e->direct_text_before_children) html_escape(b, own, 0);
   for (int j = i + 1; j < elem_count; ++j)
      if (elements[j].parent_idx == i && el_node(j)) html_of(j, b, 1);
   if (kids && !e->direct_text_before_children) html_escape(b, own, 0);
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
   }
}

static int js_eval(const char *code, size_t len, const char *filename, int module)
{
   if (!g_page.ctx) return -1;
   js_arm();
   JSValue v = JS_Eval(g_page.ctx, code, len, filename,
                       module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
   int ok = !JS_IsException(v);
   if (!ok) js_report(g_page.ctx, filename);
   JS_FreeValue(g_page.ctx, v);
   js_run_jobs();
   return ok ? 0 : -1;
}

/* Fetch and run a classic script (or module) by URL. */
static int js_eval_url(const char *rel, int module)
{
   char *url = url_resolve(g_page.url, rel);
   struct response r;
   int ok = lb_fetch(url, "GET", NULL, NULL, 0, &r) == 0 && r.status >= 200 && r.status < 300;
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
   free(j->method); free(j->url); free(j->headers); free(j->body);
   response_free(&j->r);
   free(j);
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
      if (j->gen == g_page_gen && g_page.ctx) {
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
   JS_CFUNC_DEF("title", 1, lb_title),
   JS_CFUNC_DEF("css", 1, lb_css),
   JS_CFUNC_DEF("http", 5, lb_http),
   JS_CFUNC_DEF("httpAsync", 5, lb_http_async),
   JS_CFUNC_DEF("evalURL", 2, lb_eval_url),
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

static int js_open(void)
{
   js_close();
   g_page.rt = JS_NewRuntime();
   if (!g_page.rt) return -1;
   JS_SetMaxStackSize(g_page.rt, 4u << 20);
   JS_SetInterruptHandler(g_page.rt, js_interrupt, NULL);
   JS_SetHostPromiseRejectionTracker(g_page.rt, js_rejection, NULL);
   g_page.ctx = JS_NewContext(g_page.rt);
   if (!g_page.ctx) return -1;
   JSValue global = JS_GetGlobalObject(g_page.ctx);
   JSValue lb = JS_NewObject(g_page.ctx);
   JS_SetPropertyFunctionList(g_page.ctx, lb, lb_funcs, (int)(sizeof lb_funcs / sizeof lb_funcs[0]));
   JS_SetPropertyStr(g_page.ctx, lb, "userAgent", JS_NewString(g_page.ctx, g_user_agent));
   JS_SetPropertyStr(g_page.ctx, global, "__lb", lb);
   JS_FreeValue(g_page.ctx, global);
   if (js_eval(lb_prelude, sizeof lb_prelude - 1, "luna-browser.js", 0) != 0) return -1;
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

struct script_tag { char *src, *code; int module; };

/* The document's scripts in source order.  luna-ui's parser skips <script>;
 * they are run here once the tree exists. */
static int scan_scripts(const char *html, struct script_tag **out)
{
   int n = 0;
   *out = NULL;
   for (const char *p = html; (p = strchr(p, '<')) != NULL; ) {
      if (!strncmp(p, "<!--", 4)) {
         const char *e = strstr(p, "-->");
         p = e ? e + 3 : p + strlen(p);
         continue;
      }
      if (strncasecmp(p, "<script", 7) || (p[7] != '>' && !isspace((unsigned char)p[7]))) { p++; continue; }
      const char *gt = strchr(p, '>');
      if (!gt) break;
      char *attrs = lb_strndup(p + 7, (size_t)(gt - p - 7));
      const char *close = strcasestr(gt + 1, "</script");
      const char *end = close ? close : gt + 1 + strlen(gt + 1);
      struct script_tag t = { 0 };
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
   "html,head,script,style,template,[hidden]{display:none}"
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
   "table{border-spacing:2px}td,th{padding:1px}";

static void page_teardown(void)
{
   g_page_gen++;              /* requests still in flight belong to the old page */
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

static void ready_state(int stage)
{
   g_page.ready = stage;
   JSValue a = JS_NewInt32(g_page.ctx, stage);
   JS_FreeValue(g_page.ctx, js_hook("__lb_ready", 1, &a));
}

static void page_load_html(const char *html, const char *url)
{
   page_teardown();
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
   luna_inject_body_background();
   luna_resize((float)g_page.w, (float)g_page.h);
   struct script_tag *scripts = NULL;
   int n = scan_scripts(html, &scripts);
   for (int i = 0; i < n; ++i) {
      if (scripts[i].src) js_eval_url(scripts[i].src, scripts[i].module);
      else if (scripts[i].code) js_eval(scripts[i].code, strlen(scripts[i].code), g_page.url, scripts[i].module);
      free(scripts[i].src);
      free(scripts[i].code);
   }
   free(scripts);
   ready_state(1);
   page_emit("Page.domContentEventFired", NULL);
   ready_state(2);
   g_page.dirty = 1;
}

static int page_navigate(const char *url)
{
   struct response r;
   char *abs = url_resolve(g_page.url, url);
   int rc = lb_fetch(abs, "GET", NULL, NULL, 0, &r);
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

/* action: 1 down, 0 up, -1 move. */
static void page_pointer(double x, double y, int action, int touch)
{
   luna_mouse_move(x, y);
   if (action < 0) {
      if (g_page.pressed && g_page.ctx) {
         JSValue a[7] = { JS_NewInt32(g_page.ctx, 3), JS_NewUint32(g_page.ctx, node_uid_at(luna_element_at_point(x, y))),
                          JS_NewFloat64(g_page.ctx, x), JS_NewFloat64(g_page.ctx, y),
                          JS_NewInt32(g_page.ctx, 0), JS_FALSE, JS_NewBool(g_page.ctx, touch) };
         JS_FreeValue(g_page.ctx, js_hook("__lb_pointer", 7, a));
      }
      g_page.dirty = 1;
      return;
   }
   g_page.pressed = action == 1;
   luna_mouse_button(LUNA_MOUSE_BUTTON_LEFT, action ? LUNA_PRESS : LUNA_RELEASE, 0, x, y);
   flush_pointer_events(x, y, touch);
   g_page.dirty = 1;
}

static void page_wheel(double x, double y, double dx, double dy)
{
   luna_mouse_move(x, y);
   /* CDP deltas are pixels; luna-ui's wheel unit is a notch. */
   luna_scroll(-dx / 100.0, -dy / 100.0);
   if (g_page.ctx) {
      JSValue a[5] = { JS_NewUint32(g_page.ctx, node_uid_at(luna_element_at_point(x, y))),
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
      luna_char(cp);
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
   { "Delete", LUNA_KEY_DELETE, 46 }, { NULL, 0, 0 },
};

/* One key transition.  The page's keydown can cancel the default action,
 * which is luna-ui's editing of the focused field. */
static void page_key(const char *key, int vk, int down, const char *text)
{
   int lkey = LUNA_KEY_UNKNOWN;
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
      if (lkey != LUNA_KEY_UNKNOWN) luna_key(lkey, 0, down ? LUNA_PRESS : LUNA_RELEASE, 0);
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
static double page_pump(void)
{
   if (g_page.nav_url) {
      char *u = g_page.nav_url;
      g_page.nav_url = NULL;
      page_emit("Page.frameStartedLoading", NULL);
      page_navigate(u);
      free(u);
      page_emit("Page.loadEventFired", NULL);
      page_emit("Page.frameStoppedLoading", NULL);
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

/* The current frame as PNG bytes (owned). */
static unsigned char *page_capture(size_t *len)
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
   stbi_write_png_to_func(png_sink, &b, w, h, 3, rgb, w * 3);
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
      /* PNG whatever the format asked for: the engine has no JPEG encoder. */
      size_t len = 0;
      unsigned char *img = page_capture(&len);
      if (!img) cdp_error(id, in_session, -32000, "Unable to capture screenshot");
      else {
         buf_puts(&r, "{\"data\":\"");
         buf_base64(&r, img, len);
         buf_puts(&r, "\"}");
         cdp_reply(id, in_session, r.p);
         free(img);
      }
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
      if (type && !strcmp(type, "mousePressed")) page_pointer(x, y, 1, 0);
      else if (type && !strcmp(type, "mouseReleased")) page_pointer(x, y, 0, 0);
      else if (type && !strcmp(type, "mouseMoved")) page_pointer(x, y, -1, 0);
      else if (type && !strcmp(type, "mouseWheel"))
         page_wheel(x, y, jnum(params, "deltaX", 0), jnum(params, "deltaY", 0));
   } else if (!strcmp(method, "Input.dispatchTouchEvent")) {
      const char *type = jstr(params, "type");
      const struct jv *pts = jget(params, "touchPoints");
      const struct jv *p0 = pts && pts->type == J_ARR ? pts->kid : NULL;
      static double tx, ty;
      if (p0) { tx = jnum(p0, "x", tx); ty = jnum(p0, "y", ty); }
      cdp_reply(id, in_session, "{}");
      if (type && !strcmp(type, "touchStart")) page_pointer(tx, ty, 1, 1);
      else if (type && !strcmp(type, "touchMove")) page_pointer(tx, ty, -1, 1);
      else if (type && (!strcmp(type, "touchEnd") || !strcmp(type, "touchCancel")))
         page_pointer(tx, ty, 0, 1);
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
         page_key(key, vk, 1, !strcmp(type, "keyDown") ? text : NULL);
      else if (type && !strcmp(type, "keyUp")) page_key(key, vk, 0, NULL);
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
         "  --remote-debugging-pipe   serve CDP on fds 3/4 (headless)\n"
         "  --screenshot[=FILE]       load URL, save a PNG (default screenshot.png), exit\n"
         "  --dump-dom                load URL, print the document, exit\n"
         "  --window-size=W,H         viewport (default 1024,576)\n"
         "  --virtual-time-budget=MS  time timers get before --screenshot/--dump-dom\n"
         "  --user-data-dir=DIR       profile (localStorage)\n"
         "  --user-agent=UA\n"
         "  -v                        log\n", stderr);
}

static void setup_luna_platform(void)
{
   static LunaPlatform platform;
   memset(&platform, 0, sizeof platform);
   platform.get_time = lb_platform_time;
   platform.get_proc = gl_proc;
   platform.read_resource = lb_read_resource;
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
   luna_set_mouse_press_hook(hook_press);
   luna_set_mouse_release_hook(hook_release);
   luna_set_web_compat(1);
   return 0;
}

static void settle(double budget_ms)
{
   double end = lb_now_ms() + budget_ms;
   while (lb_now_ms() < end) {
      double next = page_pump();
      double left = end - lb_now_ms();
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
static char *g_window_url;

static void win_init(void *user)
{
   (void)user;
   g_luna_platform.read_resource = lb_read_resource;
   luna_set_web_compat(1);
   luna_set_mouse_press_hook(hook_press);
   luna_set_mouse_release_hook(hook_release);
   page_navigate(g_window_url);
   luna_platform_set_title(luna_doc_title[0] ? luna_doc_title : g_window_url);
}

static void win_frame(double dt, void *user)
{
   (void)dt; (void)user;
   double x, y;
   luna_get_pointer(&x, &y);
   if (g_page.nev) flush_pointer_events(x, y, 0);
   page_pump();
   luna_app_request_redraw();
}

static int win_key(int key, int scancode, int action, int mods, void *user)
{
   (void)scancode; (void)mods; (void)user;
   if (action == LUNA_REPEAT) action = LUNA_PRESS;
   const char *name = NULL;
   for (int i = 0; g_keys[i].name; ++i) if (g_keys[i].key == key) name = g_keys[i].name;
   if (!g_page.ctx) return 0;
   JSValue a[3] = { JS_NewString(g_page.ctx, action == LUNA_PRESS ? "keydown" : "keyup"),
                    JS_NewString(g_page.ctx, name ? name : ""), JS_NewInt32(g_page.ctx, key) };
   JSValue r = js_hook("__lb_key", 3, a);
   int prevented = JS_ToBool(g_page.ctx, r) > 0;
   JS_FreeValue(g_page.ctx, r);
   for (int i = 0; i < 3; ++i) JS_FreeValue(g_page.ctx, a[i]);
   return prevented;
}

static void win_render(int fbw, int fbh, void *user)
{
   (void)user;
   luna_render(fbw, fbh);
   sync_focus_and_value();
}

static void win_resize_check(void)
{
   int x, y, w, h;
   luna_platform_get_window_rect(&x, &y, &w, &h);
   if (w > 0 && h > 0) set_viewport(w, h, g_page.scale);
}

static int window_main(const char *url)
{
   g_window_url = lb_strdup(url);
   LunaAppConfig cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.title = "Luna Browser";
   cfg.width = g_page.w;
   cfg.height = g_page.h;
   cfg.resizable = 1;
   cfg.vsync = 1;
   cfg.html = "<body></body>";
   cfg.css = "body{background:#ffffff;}";
   cfg.on_init = win_init;
   cfg.on_frame = win_frame;
   cfg.on_key = win_key;
   cfg.frame_interval = 1.0 / 60.0;
   (void)win_render;
   (void)win_resize_check;
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
      unsigned char *png = page_capture(&len);
      FILE *f = png ? fopen(shot, "wb") : NULL;
      if (f && fwrite(png, 1, len, f) == len) fprintf(stderr, "luna-browser: wrote %s (%dx%d)\n", shot, g_page.w, g_page.h);
      else { fprintf(stderr, "luna-browser: screenshot failed\n"); rc = 1; }
      if (f) fclose(f);
      free(png);
   }
   page_teardown();
   return rc;
}
