/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * HTTP/1.1 over a host socket, with TLS through OpenSSL.  This backs the
 * built-in java.net.HttpURLConnection: the guest's own networking goes through
 * the emulated syscall layer, but a connection opened by bytecode has no guest
 * socket behind it, so it is served here.
 */

#include "dvm/dvm_net.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#define HTTP_MAX_REDIRECTS 5

/* ------------------------------------------------------------------------ *
 * A stream that is either a bare fd or an SSL session over one
 * ------------------------------------------------------------------------ */

struct stream {
   int fd;
   SSL_CTX *ctx;
   SSL *ssl;
};

static void stream_close(struct stream *s)
{
   if (s->ssl) {
      SSL_shutdown(s->ssl);
      SSL_free(s->ssl);
      s->ssl = NULL;
   }
   if (s->ctx) {
      SSL_CTX_free(s->ctx);
      s->ctx = NULL;
   }
   if (s->fd >= 0) {
      close(s->fd);
      s->fd = -1;
   }
}

static ssize_t stream_write(struct stream *s, const void *buf, size_t len)
{
   size_t done = 0;
   while (done < len) {
      ssize_t n;
      if (s->ssl)
         n = SSL_write(s->ssl, (const char *)buf + done, (int)(len - done));
      else
         n = write(s->fd, (const char *)buf + done, len - done);
      if (n <= 0) {
         if (!s->ssl && n < 0 && errno == EINTR) continue;
         return -1;
      }
      done += (size_t)n;
   }
   return (ssize_t)done;
}

static ssize_t stream_read(struct stream *s, void *buf, size_t len)
{
   for (;;) {
      ssize_t n;
      if (s->ssl) {
         n = SSL_read(s->ssl, buf, (int)len);
         if (n <= 0) {
            int e = SSL_get_error(s->ssl, (int)n);
            /* A server that closes without close_notify is common enough that
             * treating it as a read error would truncate valid replies. */
            if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) return 0;
            return -1;
         }
         return n;
      }
      n = read(s->fd, buf, len);
      if (n < 0 && errno == EINTR) continue;
      return n;
   }
}

/* ------------------------------------------------------------------------ *
 * URL
 * ------------------------------------------------------------------------ */

struct url {
   bool tls;
   char host[256];
   char port[8];
   char path[4096];
};

static bool url_parse(const char *s, struct url *u)
{
   memset(u, 0, sizeof *u);
   const char *p = s;
   if (!strncasecmp(p, "https://", 8)) {
      u->tls = true;
      p += 8;
      snprintf(u->port, sizeof u->port, "443");
   } else if (!strncasecmp(p, "http://", 7)) {
      p += 7;
      snprintf(u->port, sizeof u->port, "80");
   } else {
      return false;
   }

   /* userinfo is not something the callers use; a '@' would be part of the
    * host otherwise, so reject rather than mis-resolve. */
   const char *end = p + strcspn(p, "/?#");
   const char *colon = memchr(p, ':', (size_t)(end - p));
   size_t hlen = (size_t)((colon ? colon : end) - p);
   if (!hlen || hlen >= sizeof u->host) return false;
   memcpy(u->host, p, hlen);
   u->host[hlen] = '\0';
   if (colon) {
      size_t plen = (size_t)(end - colon - 1);
      if (!plen || plen >= sizeof u->port) return false;
      memcpy(u->port, colon + 1, plen);
      u->port[plen] = '\0';
   }

   /* The fragment is client-side only and must not be sent. */
   const char *frag = strchr(end, '#');
   size_t plen = frag ? (size_t)(frag - end) : strlen(end);
   if (plen >= sizeof u->path) return false;
   if (!plen) {
      snprintf(u->path, sizeof u->path, "/");
   } else {
      memcpy(u->path, end, plen);
      u->path[plen] = '\0';
   }
   return true;
}

/* Resolves a Location header, which may be absolute or a path. */
static bool url_resolve(const struct url *base, const char *loc, char *out,
                        size_t outsz)
{
   if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) {
      if (strlen(loc) >= outsz) return false;
      snprintf(out, outsz, "%s", loc);
      return true;
   }
   const char *scheme = base->tls ? "https" : "http";
   bool default_port = !strcmp(base->port, base->tls ? "443" : "80");
   char hostport[288];
   if (default_port)
      snprintf(hostport, sizeof hostport, "%s", base->host);
   else
      snprintf(hostport, sizeof hostport, "%s:%s", base->host, base->port);

   if (loc[0] == '/')
      return (size_t)snprintf(out, outsz, "%s://%s%s", scheme, hostport,
                              loc) < outsz;

   /* Relative to the base's directory. */
   char dir[4096];
   snprintf(dir, sizeof dir, "%s", base->path);
   char *slash = strrchr(dir, '/');
   if (slash) slash[1] = '\0';
   else snprintf(dir, sizeof dir, "/");
   return (size_t)snprintf(out, outsz, "%s://%s%s%s", scheme, hostport, dir,
                           loc) < outsz;
}

/* ------------------------------------------------------------------------ *
 * A growable byte buffer
 * ------------------------------------------------------------------------ */

struct buf {
   uint8_t *p;
   size_t len, cap;
};

static bool buf_add(struct buf *b, const void *data, size_t n)
{
   if (b->len + n + 1 > b->cap) {
      size_t cap = b->cap ? b->cap : 4096;
      while (cap < b->len + n + 1) cap *= 2;
      uint8_t *np = realloc(b->p, cap);
      if (!np) return false;
      b->p = np;
      b->cap = cap;
   }
   memcpy(b->p + b->len, data, n);
   b->len += n;
   b->p[b->len] = '\0';
   return true;
}

static bool buf_str(struct buf *b, const char *s)
{
   return buf_add(b, s, strlen(s));
}

/* ------------------------------------------------------------------------ *
 * Connect
 * ------------------------------------------------------------------------ */

static bool connect_to(const struct url *u, int timeout_ms, struct stream *s,
                       char *err, size_t errsz)
{
   s->fd = -1;
   s->ctx = NULL;
   s->ssl = NULL;

   struct addrinfo hints = { 0 }, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   int rc = getaddrinfo(u->host, u->port, &hints, &res);
   if (rc || !res) {
      snprintf(err, errsz, "%s: %s", u->host, gai_strerror(rc));
      return false;
   }

   int fd = -1;
   for (struct addrinfo *a = res; a; a = a->ai_next) {
      fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (fd < 0) continue;
      struct timeval tv = {
         .tv_sec = timeout_ms / 1000,
         .tv_usec = (timeout_ms % 1000) * 1000,
      };
      if (timeout_ms > 0) {
         setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      }
      if (!connect(fd, a->ai_addr, a->ai_addrlen)) break;
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   if (fd < 0) {
      snprintf(err, errsz, "connect %s:%s: %s", u->host, u->port,
               strerror(errno));
      return false;
   }
   s->fd = fd;
   if (!u->tls) return true;

   s->ctx = SSL_CTX_new(TLS_client_method());
   if (!s->ctx) {
      snprintf(err, errsz, "SSL_CTX_new failed");
      stream_close(s);
      return false;
   }
   SSL_CTX_set_options(s->ctx, SSL_OP_NO_SSLv3);
   /* Verify against the host's trust store.  A silent no-verify fallback would
    * make every MITM look like a working connection. */
   SSL_CTX_set_default_verify_paths(s->ctx);
   SSL_CTX_set_verify(s->ctx, SSL_VERIFY_PEER, NULL);

   s->ssl = SSL_new(s->ctx);
   if (!s->ssl) {
      snprintf(err, errsz, "SSL_new failed");
      stream_close(s);
      return false;
   }
   SSL_set_fd(s->ssl, fd);
   SSL_set_tlsext_host_name(s->ssl, u->host);
   SSL_set1_host(s->ssl, u->host);
   if (SSL_connect(s->ssl) != 1) {
      unsigned long e = ERR_get_error();
      char ebuf[128] = "";
      if (e) ERR_error_string_n(e, ebuf, sizeof ebuf);
      long v = SSL_get_verify_result(s->ssl);
      snprintf(err, errsz, "TLS handshake with %s failed: %s%s%s", u->host,
               ebuf[0] ? ebuf : "connection closed",
               v != X509_V_OK ? " / " : "",
               v != X509_V_OK ? X509_verify_cert_error_string(v) : "");
      stream_close(s);
      return false;
   }
   return true;
}

/* ------------------------------------------------------------------------ *
 * Response parsing
 * ------------------------------------------------------------------------ */

static void headers_free(struct dvm_http_header *h, int n)
{
   for (int i = 0; i < n; ++i) {
      free(h[i].name);
      free(h[i].value);
   }
   free(h);
}

static bool headers_add(struct dvm_http_response *r, const char *name,
                        size_t nlen, const char *value, size_t vlen)
{
   struct dvm_http_header *n =
      realloc(r->headers, (size_t)(r->nheaders + 1) * sizeof *n);
   if (!n) return false;
   r->headers = n;
   struct dvm_http_header *h = &r->headers[r->nheaders];
   h->name = malloc(nlen + 1);
   h->value = malloc(vlen + 1);
   if (!h->name || !h->value) {
      free(h->name);
      free(h->value);
      return false;
   }
   memcpy(h->name, name, nlen);
   h->name[nlen] = '\0';
   memcpy(h->value, value, vlen);
   h->value[vlen] = '\0';
   ++r->nheaders;
   return true;
}

static const char *header_get(const struct dvm_http_response *r,
                              const char *name)
{
   for (int i = 0; i < r->nheaders; ++i)
      if (!strcasecmp(r->headers[i].name, name)) return r->headers[i].value;
   return NULL;
}

/* Decodes a chunked body in place of `src`, appending to `out`. */
static bool dechunk(const uint8_t *src, size_t len, struct buf *out)
{
   size_t p = 0;
   for (;;) {
      /* chunk-size [;ext] CRLF */
      size_t line = p;
      while (p < len && src[p] != '\n') ++p;
      if (p >= len) return false;
      size_t sz = strtoul((const char *)src + line, NULL, 16);
      ++p;
      if (!sz) return true;
      if (p + sz > len) return false;
      if (!buf_add(out, src + p, sz)) return false;
      p += sz;
      /* trailing CRLF */
      while (p < len && (src[p] == '\r' || src[p] == '\n')) ++p;
   }
}

/* ------------------------------------------------------------------------ *
 * One exchange, no redirect handling
 * ------------------------------------------------------------------------ */

static bool exchange(const char *method, const struct url *u,
                     const struct dvm_http_header *headers, int nheaders,
                     const uint8_t *body, size_t body_len, int timeout_ms,
                     struct dvm_http_response *out)
{
   struct stream s;
   if (!connect_to(u, timeout_ms, &s, out->error, sizeof out->error))
      return false;

   struct buf req = { 0 };
   char line[4352];
   snprintf(line, sizeof line, "%s %s HTTP/1.1\r\n", method, u->path);
   buf_str(&req, line);
   snprintf(line, sizeof line, "Host: %s%s%s\r\n", u->host,
            strcmp(u->port, u->tls ? "443" : "80") ? ":" : "",
            strcmp(u->port, u->tls ? "443" : "80") ? u->port : "");
   buf_str(&req, line);
   buf_str(&req, "Connection: close\r\n");
   /* No transparent decompression here, so do not let the server pick one. */
   bool have_accept_encoding = false, have_ua = false;
   for (int i = 0; i < nheaders; ++i) {
      const char *n = headers[i].name;
      if (!n || !headers[i].value) continue;
      if (!strcasecmp(n, "Host") || !strcasecmp(n, "Connection") ||
          !strcasecmp(n, "Content-Length") ||
          !strcasecmp(n, "Transfer-Encoding"))
         continue;
      if (!strcasecmp(n, "Accept-Encoding")) have_accept_encoding = true;
      if (!strcasecmp(n, "User-Agent")) have_ua = true;
      snprintf(line, sizeof line, "%s: %s\r\n", n, headers[i].value);
      buf_str(&req, line);
   }
   if (!have_accept_encoding) buf_str(&req, "Accept-Encoding: identity\r\n");
   if (!have_ua) buf_str(&req, "User-Agent: Dalvik/2.1.0 (Linux; Android 11)\r\n");
   if (body && body_len) {
      snprintf(line, sizeof line, "Content-Length: %zu\r\n", body_len);
      buf_str(&req, line);
   }
   buf_str(&req, "\r\n");
   if (body && body_len) buf_add(&req, body, body_len);

   bool ok = stream_write(&s, req.p, req.len) >= 0;
   free(req.p);
   if (!ok) {
      snprintf(out->error, sizeof out->error, "send to %s failed: %s", u->host,
               strerror(errno));
      stream_close(&s);
      return false;
   }

   struct buf raw = { 0 };
   uint8_t chunk[16384];
   for (;;) {
      ssize_t n = stream_read(&s, chunk, sizeof chunk);
      if (n == 0) break;
      if (n < 0) {
         snprintf(out->error, sizeof out->error, "read from %s failed: %s",
                  u->host, strerror(errno));
         free(raw.p);
         stream_close(&s);
         return false;
      }
      if (!buf_add(&raw, chunk, (size_t)n)) {
         snprintf(out->error, sizeof out->error, "out of memory");
         free(raw.p);
         stream_close(&s);
         return false;
      }
   }
   stream_close(&s);

   /* Status line */
   const char *p = (const char *)raw.p;
   const char *hdr_end = raw.p ? strstr(p, "\r\n\r\n") : NULL;
   size_t sep = 4;
   if (!hdr_end && raw.p) {
      hdr_end = strstr(p, "\n\n");
      sep = 2;
   }
   if (!hdr_end) {
      snprintf(out->error, sizeof out->error, "%s: truncated response",
               u->host);
      free(raw.p);
      return false;
   }
   const char *eol = strchr(p, '\n');
   if (!eol || strncmp(p, "HTTP/", 5)) {
      snprintf(out->error, sizeof out->error, "%s: not an HTTP response",
               u->host);
      free(raw.p);
      return false;
   }
   const char *sp = memchr(p, ' ', (size_t)(eol - p));
   out->status = sp ? atoi(sp + 1) : 0;

   /* Headers */
   const char *hp = eol + 1;
   while (hp < hdr_end) {
      const char *e = memchr(hp, '\n', (size_t)(hdr_end - hp));
      if (!e) e = hdr_end;
      const char *c = memchr(hp, ':', (size_t)(e - hp));
      if (c) {
         const char *ve = e;
         while (ve > c + 1 && (ve[-1] == '\r' || ve[-1] == ' ')) --ve;
         const char *vs = c + 1;
         while (vs < ve && *vs == ' ') ++vs;
         headers_add(out, hp, (size_t)(c - hp), vs, (size_t)(ve - vs));
      }
      hp = e + 1;
   }

   const uint8_t *bstart = (const uint8_t *)hdr_end + sep;
   size_t blen = raw.len - (size_t)(bstart - raw.p);
   const char *te = header_get(out, "Transfer-Encoding");
   if (te && strcasestr(te, "chunked")) {
      struct buf dec = { 0 };
      if (!dechunk(bstart, blen, &dec)) {
         /* A truncated chunked stream still carries usable bytes. */
         fprintf(stderr, "[http] %s: malformed chunked body\n", u->host);
      }
      out->body = dec.p;
      out->body_len = dec.len;
   } else {
      const char *cl = header_get(out, "Content-Length");
      if (cl) {
         size_t want = strtoul(cl, NULL, 10);
         if (want < blen) blen = want;
      }
      out->body = malloc(blen + 1);
      if (out->body) {
         memcpy(out->body, bstart, blen);
         out->body[blen] = '\0';
         out->body_len = blen;
      }
   }
   free(raw.p);
   return true;
}

/* ------------------------------------------------------------------------ *
 * Entry point
 * ------------------------------------------------------------------------ */

bool dvm_http_perform(const char *method, const char *url,
                      const struct dvm_http_header *headers, int nheaders,
                      const uint8_t *body, size_t body_len, int timeout_ms,
                      bool follow_redirects, struct dvm_http_response *out)
{
   memset(out, 0, sizeof *out);
   out->status = -1;

   char cur[4608];
   if (strlen(url) >= sizeof cur) {
      snprintf(out->error, sizeof out->error, "URL too long");
      return false;
   }
   snprintf(cur, sizeof cur, "%s", url);
   char cur_method[16];
   snprintf(cur_method, sizeof cur_method, "%s", method && *method ? method : "GET");

   for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; ++hop) {
      struct url u;
      if (!url_parse(cur, &u)) {
         snprintf(out->error, sizeof out->error, "unsupported URL: %.400s",
                  cur);
         return false;
      }
      struct dvm_http_response r;
      memset(&r, 0, sizeof r);
      r.status = -1;
      if (!exchange(cur_method, &u, headers, nheaders, body, body_len,
                    timeout_ms, &r)) {
         snprintf(out->error, sizeof out->error, "%s", r.error);
         dvm_http_response_free(&r);
         return false;
      }

      bool redirect = follow_redirects &&
                      (r.status == 301 || r.status == 302 || r.status == 303 ||
                       r.status == 307 || r.status == 308);
      const char *loc = redirect ? header_get(&r, "Location") : NULL;
      if (!loc) {
         *out = r;
         return true;
      }

      char next[4608];
      if (!url_resolve(&u, loc, next, sizeof next)) {
         *out = r;
         return true;
      }
      /* 303, and historically 301/302, turn a POST into a GET. */
      if (r.status == 303 || ((r.status == 301 || r.status == 302) &&
                              strcmp(cur_method, "GET") &&
                              strcmp(cur_method, "HEAD"))) {
         snprintf(cur_method, sizeof cur_method, "GET");
         body = NULL;
         body_len = 0;
      }
      dvm_http_response_free(&r);
      snprintf(cur, sizeof cur, "%s", next);
   }

   snprintf(out->error, sizeof out->error, "too many redirects");
   return false;
}

void dvm_http_response_free(struct dvm_http_response *r)
{
   if (!r) return;
   headers_free(r->headers, r->nheaders);
   r->headers = NULL;
   r->nheaders = 0;
   free(r->body);
   r->body = NULL;
   r->body_len = 0;
}
