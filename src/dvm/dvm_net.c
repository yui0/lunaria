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
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
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

/* OpenSSL writes to the host socket without MSG_NOSIGNAL.  A peer closing an
 * HTTP/2 connection must be reported to Java as an I/O error, not terminate
 * the emulator with the host's default SIGPIPE action.  Guest signal state is
 * maintained separately by the ARM runtime. */
static pthread_once_t host_sigpipe_once = PTHREAD_ONCE_INIT;
static void ignore_host_sigpipe(void)
{
   struct sigaction action = { 0 };
   action.sa_handler = SIG_IGN;
   sigemptyset(&action.sa_mask);
   (void)sigaction(SIGPIPE, &action, NULL);
}

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

/* How long a body read may stall in total before it is called a failure.  The
 * socket carries the caller's own SO_RCVTIMEO, so each attempt returns after
 * that; this bounds how many of those in a row are tolerated. */
#define HTTP_READ_STALL_MS 120000

static ssize_t stream_read(struct stream *s, void *buf, size_t len)
{
   long waited_ms = 0;
   for (;;) {
      ssize_t n;
      if (s->ssl) {
         ERR_clear_error();
         errno = 0;
         n = SSL_read(s->ssl, buf, (int)len);
         if (n > 0) return n;
         int e = SSL_get_error(s->ssl, (int)n);
         /* A read that timed out has not ended anything: the body is still
          * owed bytes.  Reporting it as end of stream is how a multi-gigabyte
          * download came back truncated and then failed its MD5 check — the
          * transfer looked like it had finished successfully.  The timeout
          * arrives as WANT_READ, or as SYSCALL with EAGAIN under the socket's
          * SO_RCVTIMEO, so both go back around. */
         if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE ||
             (e == SSL_ERROR_SYSCALL &&
              (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
            if (waited_ms >= HTTP_READ_STALL_MS) { errno = ETIMEDOUT; return -1; }
            waited_ms += 1000;
            continue;
         }
         /* Only a genuine end of connection is end of body: close_notify, or a
          * peer that just went away (SYSCALL with nothing to report).  Anything
          * else is an error and has to be reported as one. */
         if (e == SSL_ERROR_ZERO_RETURN) return 0;
         if (e == SSL_ERROR_SYSCALL && errno == 0) return 0;
         return -1;
      }
      n = read(s->fd, buf, len);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
         if (waited_ms >= HTTP_READ_STALL_MS) { errno = ETIMEDOUT; return -1; }
         waited_ms += 1000;
         continue;
      }
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
   /* Graph API / Play URLs with application meta-data in the query routinely
    * exceed 4 KiB; keep room for those legitimate Android request lines. */
   char path[16384];
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

   /* Relative to the base's directory.  Same capacity as url.path: a redirect
    * whose base path already fills the URL must not be truncated here. */
   char dir[sizeof base->path];
   if (strlen(base->path) >= sizeof dir) return false;
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
   /* Try IPv4 first when both families are present — same rationale as the
    * guest getaddrinfo reorder in arm_exec (avoid blackhole AAAA stalls). */
   for (int pass = 0; pass < 2 && fd < 0; ++pass) {
      for (struct addrinfo *a = res; a; a = a->ai_next) {
         const bool is_v4 = a->ai_family == AF_INET;
         if (pass == 0 && !is_v4) continue;
         if (pass == 1 && is_v4) continue;
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

/* Decode the chunked transfer coding.  stop_at identifies the byte at which
 * framing is incomplete or invalid.
 *
 * The size line is parsed here rather than with strtoul(): `src` is a length-
 * counted buffer with no NUL, which strtoul() may read past, and its answer
 * for a line that holds no digits at all is 0 — the same answer as the
 * last-chunk marker.  A body whose framing went wrong therefore used to decode
 * as a short but perfectly valid one, with nothing said about it. */
/* 1: complete body; 0: need another read; -1: invalid framing. */
static int dechunk(const uint8_t *src, size_t len, struct buf *out,
                   size_t *stop_at)
{
   size_t p = 0;
   for (;;) {
      /* chunk-size [;ext] CRLF */
      size_t line = p;
      while (p < len && src[p] != '\r') ++p;
      if (p >= len) { *stop_at = line; return 0; }
      if (p + 1 >= len) { *stop_at = line; return 0; }
      if (src[p + 1] != '\n') { *stop_at = p; return -1; }
      size_t sz = 0;
      int digits = 0;
      bool extension = false;
      for (size_t q = line; q < p; ++q) {
         unsigned c = src[q], d;
         if (extension) continue;
         if (c >= '0' && c <= '9')      d = c - '0';
         else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
         else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
         else if (digits && (c == ';' || c == ' ' || c == '\t')) {
            extension = true;
            continue;
         } else { *stop_at = q; return -1; }
         if (sz > (SIZE_MAX - d) / 16) { *stop_at = line; return -1; }
         sz = sz * 16 + d;
         ++digits;
      }
      if (!digits) { *stop_at = line; return -1; }
      p += 2;
      if (!sz) {
         for (;;) {
            size_t trailer = p;
            while (p < len && src[p] != '\r') ++p;
            if (p + 1 >= len) { *stop_at = trailer; return 0; }
            if (src[p + 1] != '\n') { *stop_at = p; return -1; }
            p += 2;
            if (p == trailer + 2) { *stop_at = p; return 1; }
         }
      }
      if (sz > len - p) { *stop_at = p; return 0; }
      if (!buf_add(out, src + p, sz)) { *stop_at = p; return -1; }
      p += sz;
      if (p + 1 >= len) { *stop_at = p; return 0; }
      if (src[p] != '\r' || src[p + 1] != '\n') {
         *stop_at = p;
         return -1;
      }
      p += 2;
   }
}

/* ------------------------------------------------------------------------ *
 * One exchange, no redirect handling
 * ------------------------------------------------------------------------ */

static void http_stream_done(struct dvm_http_response *r);

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

   /* Read only as far as the end of the headers.  Everything after that is
    * the caller's to pull, one buffer at a time, through dvm_http_read(). */
   struct buf raw = { 0 };
   uint8_t chunk[16384];
   const char *hdr_end = NULL;
   size_t sep = 4;
   for (;;) {
      ssize_t n = stream_read(&s, chunk, sizeof chunk);
      if (n < 0) {
         snprintf(out->error, sizeof out->error, "read from %s failed: %s",
                  u->host, strerror(errno));
         free(raw.p);
         stream_close(&s);
         return false;
      }
      if (n > 0 && !buf_add(&raw, chunk, (size_t)n)) {
         snprintf(out->error, sizeof out->error, "out of memory");
         free(raw.p);
         stream_close(&s);
         return false;
      }
      /* buf_add keeps the buffer NUL-terminated, so the searches below stay
       * inside it even before the whole response has arrived. */
      if (raw.p) {
         hdr_end = strstr((const char *)raw.p, "\r\n\r\n");
         sep = 4;
         if (!hdr_end) {
            hdr_end = strstr((const char *)raw.p, "\n\n");
            sep = 2;
         }
      }
      if (hdr_end || n == 0) break;
   }

   /* Status line */
   const char *p = (const char *)raw.p;
   if (!hdr_end) {
      snprintf(out->error, sizeof out->error, "%s: truncated response",
               u->host);
      free(raw.p);
      stream_close(&s);
      return false;
   }
   const char *eol = strchr(p, '\n');
   if (!eol || strncmp(p, "HTTP/", 5)) {
      snprintf(out->error, sizeof out->error, "%s: not an HTTP response",
               u->host);
      free(raw.p);
      stream_close(&s);
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

   /* Hand the leftover bytes and the still-open connection to the reader. */
   const uint8_t *bstart = (const uint8_t *)hdr_end + sep;
   size_t blen = raw.len - (size_t)(bstart - raw.p);
   out->pre = malloc(blen + 1);
   if (!out->pre) {
      snprintf(out->error, sizeof out->error, "out of memory");
      free(raw.p);
      stream_close(&s);
      return false;
   }
   memcpy(out->pre, bstart, blen);
   out->pre[blen] = '\0';
   out->pre_len = blen;
   out->pre_pos = 0;
   free(raw.p);

   const char *te = header_get(out, "Transfer-Encoding");
   out->chunked = te && strcasestr(te, "chunked");
   const char *cl = header_get(out, "Content-Length");
   out->content_length = cl ? strtoll(cl, NULL, 10) : -1;
   if (out->chunked) out->content_length = -1;

   struct stream *held = malloc(sizeof *held);
   if (!held) {
      snprintf(out->error, sizeof out->error, "out of memory");
      stream_close(&s);
      return false;
   }
   *held = s;
   out->stream = held;
   /* A body that is already complete needs no more of the connection. */
   if (out->content_length >= 0 && (long long)out->pre_len >= out->content_length) {
      out->pre_len = (size_t)out->content_length;
      http_stream_done(out);
   }
   return true;
}

/* Closes the connection behind a response and forgets it.  Reads after this
 * are served from whatever is still buffered, then report end of body. */
static void http_stream_done(struct dvm_http_response *r)
{
   if (!r || !r->stream) return;
   stream_close((struct stream *)r->stream);
   free(r->stream);
   r->stream = NULL;
}

size_t dvm_http_avail(const struct dvm_http_response *r)
{
   if (!r) return 0;
   if (r->chunked)
      return r->body && r->body_len > (size_t)r->body_read
                ? r->body_len - (size_t)r->body_read : 0;
   size_t n = 0;
   if (r->pre_pos < r->pre_len) n += r->pre_len - r->pre_pos;
   if (r->ra_pos < r->ra_len)   n += r->ra_len - r->ra_pos;
   return n;
}

long dvm_http_read(struct dvm_http_response *r, void *buf, size_t n)
{
   if (!r || !buf || !n) return 0;

   /* Chunked framing is only used here by the small JSON APIs, and decoding it
    * incrementally would buy nothing: decode it once and serve from there. */
   if (r->chunked) {
      if (!r->body && !dvm_http_slurp(r)) return -1;
      size_t left = r->body_len > (size_t)r->body_read
                        ? r->body_len - (size_t)r->body_read : 0;
      if (!left) return 0;
      if (n > left) n = left;
      memcpy(buf, r->body + r->body_read, n);
      r->body_read += (long long)n;
      return (long)n;
   }

   size_t done = 0;
   /* Bytes that arrived in the same read as the headers come first. */
   if (r->pre_pos < r->pre_len) {
      size_t take = r->pre_len - r->pre_pos;
      if (take > n) take = n;
      memcpy(buf, r->pre + r->pre_pos, take);
      r->pre_pos += take;
      done += take;
   }
   /* Then whatever the last socket read pulled ahead of what was asked for. */
   if (done < n && r->ra_pos < r->ra_len) {
      size_t take = r->ra_len - r->ra_pos;
      if (take > n - done) take = n - done;
      memcpy((uint8_t *)buf + done, r->ra + r->ra_pos, take);
      r->ra_pos += take;
      done += take;
   }
   while (done < n && r->stream) {
      if (r->content_length >= 0 &&
          r->body_read + (long long)done >= r->content_length)
         break;
      size_t want = n - done;
      if (r->content_length >= 0) {
         long long left = r->content_length - (r->body_read + (long long)done);
         if ((long long)want > left) want = (size_t)left;
      }
      /* Read into the read-ahead buffer whenever the caller's request is
       * smaller than it, so one socket read covers many of them.  A big
       * request goes straight into the caller's buffer — nothing to gain by
       * copying it twice. */
      if (!r->ra_cap) {
         r->ra_cap = 8u << 20;
         r->ra = malloc(r->ra_cap);
         if (!r->ra) r->ra_cap = 0;
      }
      if (r->ra_cap && want < r->ra_cap && r->content_length >= 0) {
         size_t fill = r->ra_cap;
         long long left = r->content_length - (r->body_read + (long long)done);
         if ((long long)fill > left) fill = (size_t)left;
         /* Fill it, rather than stopping at whatever one read returned: a TLS
          * stream hands back a record at a time (16 KiB), and one guest read
          * per record is most of the hand-off cost back again.  Bounded by
          * Content-Length, so this never waits for bytes the body does not
          * have — which is also why a close-terminated body does not come
          * here at all. */
         r->ra_len = 0;
         r->ra_pos = 0;
         bool closed = false;
         while (r->ra_len < fill) {
            ssize_t got = stream_read((struct stream *)r->stream,
                                      r->ra + r->ra_len, fill - r->ra_len);
            if (got < 0) {
               snprintf(r->error, sizeof r->error, "read failed: %s",
                        strerror(errno));
               http_stream_done(r);
               return done ? (long)done : -1;
            }
            if (got == 0) { closed = true; break; }
            r->ra_len += (size_t)got;
         }
         if (!r->ra_len) { if (closed) http_stream_done(r); break; }
         size_t take = r->ra_len < want ? r->ra_len : want;
         memcpy((uint8_t *)buf + done, r->ra, take);
         r->ra_pos = take;
         done += take;
         if (closed) http_stream_done(r);
         continue;
      }
      ssize_t got = stream_read((struct stream *)r->stream,
                                (uint8_t *)buf + done, want);
      if (got < 0) {
         snprintf(r->error, sizeof r->error, "read failed: %s",
                  strerror(errno));
         http_stream_done(r);
         return done ? (long)done : -1;
      }
      if (got == 0) {           /* peer closed */
         http_stream_done(r);
         break;
      }
      done += (size_t)got;
   }
   r->body_read += (long long)done;
   if (r->content_length >= 0 && r->body_read >= r->content_length)
      http_stream_done(r);
   /* Short of what the headers promised, with the connection gone: the body is
    * truncated.  Saying "end of stream" here would hand the caller a partial
    * file it believes is complete — which is what a download does with it. */
   if (!done && !r->stream && r->ra_pos >= r->ra_len &&
       r->content_length >= 0 && r->body_read < r->content_length) {
      snprintf(r->error, sizeof r->error,
               "body truncated: %lld of %lld bytes",
               r->body_read, r->content_length);
      return -1;
   }
   return (long)done;
}

bool dvm_http_slurp(struct dvm_http_response *r)
{
   if (!r) return false;
   if (r->body) return true;

   struct buf all = { 0 };
   /* Chunked framing is collected until its final chunk, independent of when
    * the server decides to close the HTTP/1.1 connection. */
   if (r->pre_pos < r->pre_len &&
       !buf_add(&all, r->pre + r->pre_pos, r->pre_len - r->pre_pos)) {
      free(all.p);
      return false;
   }
   r->pre_pos = r->pre_len;
   uint8_t chunk[16384];
   bool complete = false;
   bool eof = false;
   while (r->stream) {
      if (r->chunked) {
         struct buf decoded = { 0 };
         size_t stop = 0;
         int state = dechunk(all.p, all.len, &decoded, &stop);
         if (state == 1) {
            if (!decoded.p) {
               decoded.p = malloc(1);
               if (!decoded.p) {
                  snprintf(r->error, sizeof r->error, "out of memory");
                  http_stream_done(r);
                  free(all.p);
                  return false;
               }
               decoded.p[0] = '\0';
            }
            r->body = decoded.p;
            r->body_len = decoded.len;
            complete = true;
            break;
         }
         free(decoded.p);
         if (state < 0) {
            snprintf(r->error, sizeof r->error,
                     "malformed chunked body at byte %zu", stop);
            http_stream_done(r);
            free(all.p);
            return false;
         }
      }
      if (!r->chunked && r->content_length >= 0 &&
          (long long)all.len >= r->content_length)
         break;
      ssize_t got = stream_read((struct stream *)r->stream, chunk, sizeof chunk);
      if (got < 0) {
         snprintf(r->error, sizeof r->error, "read failed: %s",
                  strerror(errno));
         http_stream_done(r);
         free(all.p);
         return false;
      }
      if (got == 0) { eof = true; http_stream_done(r); break; }
      if (!buf_add(&all, chunk, (size_t)got)) {
         http_stream_done(r);
         free(all.p);
         return false;
      }
   }
   http_stream_done(r);

   if (r->chunked) {
      free(all.p);
      if (!complete) {
         snprintf(r->error, sizeof r->error,
                  "%s before final HTTP chunk", eof ? "EOF" : "read ended");
         return false;
      }
   } else {
      if (r->content_length >= 0 && (long long)all.len > r->content_length)
         all.len = (size_t)r->content_length;
      r->body = all.p;
      r->body_len = all.len;
   }
   if (!r->body) {
      r->body = malloc(1);
      if (r->body) r->body[0] = '\0';
      r->body_len = 0;
   }
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
   (void)pthread_once(&host_sigpipe_once, ignore_host_sigpipe);
   memset(out, 0, sizeof *out);
   out->status = -1;

   char cur[16896];
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
   http_stream_done(r);
   free(r->pre);
   r->pre = NULL;
   r->pre_len = r->pre_pos = 0;
   free(r->ra);
   r->ra = NULL;
   r->ra_len = r->ra_pos = r->ra_cap = 0;
   headers_free(r->headers, r->nheaders);
   r->headers = NULL;
   r->nheaders = 0;
   free(r->body);
   r->body = NULL;
   r->body_len = 0;
   r->body_read = 0;
   r->content_length = -1;
   r->chunked = false;
}

/* ------------------------------------------------------------------------ *
 * Stream sockets (java.net.Socket / SSLSocket)
 * ------------------------------------------------------------------------ */

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/x509v3.h>

/* A connect that honours Socket.connect(address, timeout): non-blocking
 * connect, then poll for writability for at most timeout_ms. */
static int bind_local(int fd, int family, const char *local_addr,
                      int local_port)
{
   if ((!local_addr || !*local_addr) && local_port <= 0) return 0;
   struct sockaddr_storage ss;
   memset(&ss, 0, sizeof ss);
   socklen_t sl;
   if (family == AF_INET6) {
      struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
      a6->sin6_family = AF_INET6;
      a6->sin6_port = htons((uint16_t)local_port);
      a6->sin6_addr = in6addr_any;
      if (local_addr && *local_addr &&
          inet_pton(AF_INET6, local_addr, &a6->sin6_addr) != 1) {
         errno = EADDRNOTAVAIL;
         return -1;
      }
      sl = sizeof *a6;
   } else {
      struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
      a4->sin_family = AF_INET;
      a4->sin_port = htons((uint16_t)local_port);
      a4->sin_addr.s_addr = htonl(INADDR_ANY);
      if (local_addr && *local_addr &&
          inet_pton(AF_INET, local_addr, &a4->sin_addr) != 1) {
         errno = EADDRNOTAVAIL;
         return -1;
      }
      sl = sizeof *a4;
   }
   return bind(fd, (struct sockaddr *)&ss, sl);
}

static int connect_one(const struct addrinfo *a, const char *local_addr,
                       int local_port, int timeout_ms)
{
   int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
   if (fd < 0) return -1;
   if (bind_local(fd, a->ai_family, local_addr, local_port) < 0) {
      int e = errno;
      close(fd);
      errno = e;
      return -1;
   }
   if (timeout_ms <= 0) {
      if (!connect(fd, a->ai_addr, a->ai_addrlen)) return fd;
      int e = errno;
      close(fd);
      errno = e;
      return -1;
   }
   int fl = fcntl(fd, F_GETFL, 0);
   fcntl(fd, F_SETFL, fl | O_NONBLOCK);
   int rc = connect(fd, a->ai_addr, a->ai_addrlen);
   if (rc < 0 && errno == EINPROGRESS) {
      struct pollfd p = { .fd = fd, .events = POLLOUT };
      int pr;
      do pr = poll(&p, 1, timeout_ms); while (pr < 0 && errno == EINTR);
      if (pr == 0) {
         close(fd);
         errno = ETIMEDOUT;
         return -1;
      }
      int soerr = 0;
      socklen_t sl = sizeof soerr;
      if (pr < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 ||
          soerr) {
         close(fd);
         errno = soerr ? soerr : errno;
         return -1;
      }
      rc = 0;
   }
   if (rc < 0) {
      int e = errno;
      close(fd);
      errno = e;
      return -1;
   }
   fcntl(fd, F_SETFL, fl);
   return fd;
}

int dvm_sock_connect(const char *host, int port, const char *local_addr,
                     int local_port, int timeout_ms, char *err, size_t errsz)
{
   char portstr[8];
   snprintf(portstr, sizeof portstr, "%d", port);
   struct addrinfo hints = { 0 }, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   int rc = getaddrinfo(host, portstr, &hints, &res);
   if (rc || !res) {
      snprintf(err, errsz, "Unable to resolve host \"%s\": %s", host,
               gai_strerror(rc));
      errno = EHOSTUNREACH;
      return -1;
   }
   int fd = -1, last = ECONNREFUSED;
   for (int pass = 0; pass < 2 && fd < 0; ++pass)
      for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
         const bool v4 = a->ai_family == AF_INET;
         if (v4 != (pass == 0)) continue;
         fd = connect_one(a, local_addr, local_port, timeout_ms);
         if (fd < 0) last = errno;
      }
   freeaddrinfo(res);
   if (fd < 0) {
      snprintf(err, errsz, "failed to connect to %s (port %d): %s", host, port,
               strerror(last));
      errno = last;
   }
   return fd;
}

void dvm_sock_set_timeout(int fd, int timeout_ms)
{
   if (fd < 0) return;
   struct timeval tv = {
      .tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 0,
      .tv_usec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0,
   };
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* One TLS session.  HTTP/2 reads a connection on one thread while others
 * write it, and an SSL object must not be entered by two threads at once, so
 * every SSL_* call on it takes `mu`.  Waiting for the socket happens outside
 * the lock, in poll(): after the handshake the fd is non-blocking, and a
 * reader that would block releases the session before it sleeps. */
struct dvm_tls {
   SSL_CTX *ctx;
   SSL *ssl;
   int fd;
   pthread_mutex_t mu;
};

/* Every suite the library offers, by IANA name.  Built once from a default
 * client context: that is what SSLSocket.getSupportedCipherSuites() means. */
static const char **g_tls_ciphers;

const char *const *dvm_tls_supported_ciphers(void)
{
   if (g_tls_ciphers) return g_tls_ciphers;
   SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
   SSL *ssl = ctx ? SSL_new(ctx) : NULL;
   STACK_OF(SSL_CIPHER) *sk = ssl ? SSL_get1_supported_ciphers(ssl) : NULL;
   int n = sk ? sk_SSL_CIPHER_num(sk) : 0;
   const char **list = calloc((size_t)n + 1, sizeof *list);
   int k = 0;
   for (int i = 0; list && i < n; ++i) {
      const char *std = SSL_CIPHER_standard_name(sk_SSL_CIPHER_value(sk, i));
      if (std) list[k++] = strdup(std);
   }
   if (sk) sk_SSL_CIPHER_free(sk);
   if (ssl) SSL_free(ssl);
   if (ctx) SSL_CTX_free(ctx);
   static const char *empty[] = { NULL };
   g_tls_ciphers = list ? list : empty;
   return g_tls_ciphers;
}

/* Maps the caller's IANA names onto what OpenSSL configures: TLS 1.3 suites
 * go through SSL_set_ciphersuites() by their (identical) names, older ones
 * through SSL_set_cipher_list() by OpenSSL's own names. */
static void tls_apply_ciphers(SSL *ssl, const char *const *want, int nwant)
{
   STACK_OF(SSL_CIPHER) *sk = SSL_get_ciphers(ssl);
   if (!sk || !want || nwant <= 0) return;
   char l12[4096] = "", l13[1024] = "";
   for (int i = 0; i < sk_SSL_CIPHER_num(sk); ++i) {
      const SSL_CIPHER *c = sk_SSL_CIPHER_value(sk, i);
      const char *std = SSL_CIPHER_standard_name(c);
      if (!std) continue;
      bool wanted = false;
      for (int j = 0; j < nwant && !wanted; ++j)
         wanted = want[j] && !strcmp(want[j], std);
      if (!wanted) continue;
      /* TLS 1.3 suites negotiate no key exchange of their own. */
      const bool v13 = SSL_CIPHER_get_kx_nid(c) == NID_kx_any;
      char *dst = v13 ? l13 : l12;
      size_t cap = v13 ? sizeof l13 : sizeof l12;
      size_t used = strlen(dst);
      snprintf(dst + used, cap - used, "%s%s", used ? ":" : "",
               v13 ? std : SSL_CIPHER_get_name(c));
   }
   if (l12[0]) SSL_set_cipher_list(ssl, l12);
   if (l13[0]) SSL_set_ciphersuites(ssl, l13);
}

struct dvm_tls *dvm_tls_connect(int fd, const char *host, int verify,
                                const uint8_t *alpn, size_t alpn_len,
                                int min_version, int max_version,
                                const char *const *ciphers, int nciphers,
                                bool *verify_failed, char *err, size_t errsz)
{
   (void)pthread_once(&host_sigpipe_once, ignore_host_sigpipe);
   if (verify_failed) *verify_failed = false;
   struct dvm_tls *t = calloc(1, sizeof *t);
   if (!t) { snprintf(err, errsz, "out of memory"); return NULL; }
   t->fd = fd;
   pthread_mutex_init(&t->mu, NULL);
   t->ctx = SSL_CTX_new(TLS_client_method());
   if (!t->ctx) {
      snprintf(err, errsz, "SSL_CTX_new failed");
      free(t);
      return NULL;
   }
   SSL_CTX_set_options(t->ctx, SSL_OP_NO_SSLv3);
   if (verify & DVM_TLS_VERIFY_CHAIN) {
      SSL_CTX_set_default_verify_paths(t->ctx);
      SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
   } else {
      SSL_CTX_set_verify(t->ctx, SSL_VERIFY_NONE, NULL);
   }
   if (min_version) SSL_CTX_set_min_proto_version(t->ctx, min_version);
   if (max_version) SSL_CTX_set_max_proto_version(t->ctx, max_version);
   t->ssl = SSL_new(t->ctx);
   if (!t->ssl) {
      snprintf(err, errsz, "SSL_new failed");
      dvm_tls_free(t);
      return NULL;
   }
   tls_apply_ciphers(t->ssl, ciphers, nciphers);
   if (alpn && alpn_len) SSL_set_alpn_protos(t->ssl, alpn, (unsigned)alpn_len);
   SSL_set_fd(t->ssl, fd);
   if (host && *host) {
      /* SNI is for names only (RFC 6066 §3); an address goes in the IP SAN
       * check instead. */
      unsigned char probe[16];
      const bool is_ip = inet_pton(AF_INET, host, probe) == 1 ||
                         inet_pton(AF_INET6, host, probe) == 1;
      if (!is_ip) SSL_set_tlsext_host_name(t->ssl, host);
      if (verify & DVM_TLS_VERIFY_HOST) {
         if (!is_ip) SSL_set1_host(t->ssl, host);
         else X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->ssl), host);
      }
   }
   ERR_clear_error();
   if (SSL_connect(t->ssl) != 1) {
      unsigned long e = ERR_get_error();
      char ebuf[160] = "";
      if (e) ERR_error_string_n(e, ebuf, sizeof ebuf);
      long v = SSL_get_verify_result(t->ssl);
      if (verify_failed) *verify_failed = v != X509_V_OK;
      snprintf(err, errsz, "TLS handshake with %s failed: %s%s%s",
               host ? host : "?", ebuf[0] ? ebuf : "connection closed",
               v != X509_V_OK ? " / " : "",
               v != X509_V_OK ? X509_verify_cert_error_string(v) : "");
      dvm_tls_free(t);
      return NULL;
   }
   SSL_set_mode(t->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE |
                        SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
   return t;
}

/* Sleeps until the fd can do what the session asked for.  0 = ready,
 * -2 = timed out, -1 = error. */
static int tls_wait(int fd, int want, int timeout_ms)
{
   struct pollfd p = {
      .fd = fd,
      .events = (short)(want == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN),
   };
   int r;
   do r = poll(&p, 1, timeout_ms > 0 ? timeout_ms : -1);
   while (r < 0 && errno == EINTR);
   if (r == 0) return -2;
   if (r < 0) return -1;
   return 0;   /* POLLHUP/POLLERR: the next SSL call reports it */
}

long dvm_tls_read(struct dvm_tls *t, void *buf, size_t n, int timeout_ms)
{
   if (!t || !t->ssl) return -1;
   const int want_n = (int)(n > 0x7fffffff ? 0x7fffffff : n);
   for (;;) {
      pthread_mutex_lock(&t->mu);
      ERR_clear_error();
      errno = 0;
      int r = SSL_read(t->ssl, buf, want_n);
      int e = r > 0 ? SSL_ERROR_NONE : SSL_get_error(t->ssl, r);
      const int err = errno;
      pthread_mutex_unlock(&t->mu);
      if (r > 0) return r;
      if (e == SSL_ERROR_ZERO_RETURN) return 0;
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
         int w = tls_wait(t->fd, e, timeout_ms);
         if (w) return w;
         continue;
      }
      if (e == SSL_ERROR_SYSCALL && err == EINTR) continue;
      /* A peer that just went away without close_notify: end of stream, as
       * the plain socket reports it. */
      if (e == SSL_ERROR_SYSCALL && err == 0) return 0;
      return -1;
   }
}

long dvm_tls_write(struct dvm_tls *t, const void *buf, size_t n)
{
   if (!t || !t->ssl) return -1;
   size_t done = 0;
   while (done < n) {
      pthread_mutex_lock(&t->mu);
      ERR_clear_error();
      errno = 0;
      int w = SSL_write(t->ssl, (const char *)buf + done,
                        (int)((n - done) > 0x7fffffff ? 0x7fffffff : n - done));
      int e = w > 0 ? SSL_ERROR_NONE : SSL_get_error(t->ssl, w);
      const int err = errno;
      pthread_mutex_unlock(&t->mu);
      if (w > 0) { done += (size_t)w; continue; }
      if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
         if (tls_wait(t->fd, e, 0) < 0) return -1;
         continue;
      }
      if (e == SSL_ERROR_SYSCALL && err == EINTR) continue;
      return -1;
   }
   return (long)done;
}

size_t dvm_tls_pending(struct dvm_tls *t)
{
   if (!t || !t->ssl) return 0;
   pthread_mutex_lock(&t->mu);
   size_t n = (size_t)SSL_pending(t->ssl);
   pthread_mutex_unlock(&t->mu);
   return n;
}

void dvm_tls_free(struct dvm_tls *t)
{
   if (!t) return;
   if (t->ssl) {
      SSL_shutdown(t->ssl);
      SSL_free(t->ssl);
   }
   if (t->ctx) SSL_CTX_free(t->ctx);
   pthread_mutex_destroy(&t->mu);
   free(t);
}

const char *dvm_tls_protocol(const struct dvm_tls *t)
{
   return t && t->ssl ? SSL_get_version(t->ssl) : "NONE";
}

const char *dvm_tls_cipher(const struct dvm_tls *t)
{
   const SSL_CIPHER *c = t && t->ssl ? SSL_get_current_cipher(t->ssl) : NULL;
   const char *std = c ? SSL_CIPHER_standard_name(c) : NULL;
   return std ? std : "SSL_NULL_WITH_NULL_NULL";
}

void dvm_tls_alpn(const struct dvm_tls *t, char *out, size_t outsz)
{
   const unsigned char *p = NULL;
   unsigned len = 0;
   if (t && t->ssl) SSL_get0_alpn_selected(t->ssl, &p, &len);
   if (!outsz) return;
   if (len >= outsz) len = (unsigned)outsz - 1;
   if (p && len) memcpy(out, p, len);
   out[len] = '\0';
}

int dvm_tls_peer_chain(const struct dvm_tls *t, uint8_t ***der, int **len)
{
   *der = NULL;
   *len = NULL;
   STACK_OF(X509) *sk = t && t->ssl ? SSL_get_peer_cert_chain(t->ssl) : NULL;
   int n = sk ? sk_X509_num(sk) : 0;
   if (n <= 0) return 0;
   *der = calloc((size_t)n, sizeof **der);
   *len = calloc((size_t)n, sizeof **len);
   if (!*der || !*len) { free(*der); free(*len); *der = NULL; *len = NULL; return 0; }
   int k = 0;
   for (int i = 0; i < n; ++i) {
      X509 *x = sk_X509_value(sk, i);
      int l = i2d_X509(x, NULL);
      if (l <= 0) continue;
      uint8_t *buf = malloc((size_t)l), *p = buf;
      if (!buf) continue;
      if (i2d_X509(x, &p) != l) { free(buf); continue; }
      (*der)[k] = buf;
      (*len)[k] = l;
      ++k;
   }
   return k;
}

const char *dvm_tls_auth_type(const struct dvm_tls *t)
{
   const SSL_CIPHER *c = t && t->ssl ? SSL_get_current_cipher(t->ssl) : NULL;
   if (!c) return "UNKNOWN";
   const int kx = SSL_CIPHER_get_kx_nid(c), au = SSL_CIPHER_get_auth_nid(c);
   if (kx == NID_kx_any) return "GENERIC";
   const bool ecdhe = kx == NID_kx_ecdhe, dhe = kx == NID_kx_dhe;
   if (au == NID_auth_ecdsa) return ecdhe ? "ECDHE_ECDSA" : "ECDSA";
   if (au == NID_auth_rsa) return ecdhe ? "ECDHE_RSA" : dhe ? "DHE_RSA" : "RSA";
   if (au == NID_auth_psk) return ecdhe ? "ECDHE_PSK" : "PSK";
   return "UNKNOWN";
}

size_t dvm_tls_session_id(const struct dvm_tls *t, uint8_t *out, size_t outsz)
{
   SSL_SESSION *s = t && t->ssl ? SSL_get_session(t->ssl) : NULL;
   unsigned len = 0;
   const unsigned char *id = s ? SSL_SESSION_get_id(s, &len) : NULL;
   if (!id) return 0;
   if (len > outsz) len = (unsigned)outsz;
   memcpy(out, id, len);
   return len;
}
