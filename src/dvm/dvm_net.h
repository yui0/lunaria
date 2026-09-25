/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * A blocking HTTP/HTTPS client for the built-in java.net classes.  The VM
 * interprets one thread at a time, so a request runs to completion inside the
 * bytecode call that asked for it — the same shape java.net.HttpURLConnection
 * presents to its caller.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dvm_http_header {
   char *name;
   char *value;
};

struct dvm_http_response {
   int status;                      /* -1 when the exchange never completed */
   struct dvm_http_header *headers;
   int nheaders;
   uint8_t *body;                   /* only once slurped; NULL while streaming */
   size_t body_len;
   char error[512];                 /* empty on success */

   /* The exchange stops at the end of the headers and leaves the connection
    * open, so the body is read as the caller consumes it.  Buffering it whole
    * is not an option for this workload: the game downloads its content as
    * pak files of several gigabytes each, and a response that has to fit in
    * memory first stops the emulator dead at the first one. */
   void *stream;                    /* struct stream *, NULL once finished */
   uint8_t *pre;                    /* body bytes read along with the headers */
   size_t pre_len, pre_pos;
   long long content_length;        /* from the header; -1 when absent */
   long long body_read;             /* handed to the caller so far */
   bool chunked;

   /* Read-ahead.  A guest that copies a file reads it in its own small buffer;
    * one socket read per guest read is one interpreter-lock turn per guest
    * read, and the lock turns over once per rendered frame.  That capped the
    * patch download at the guest's buffer times the frame rate (~0.5 MB/s
    * measured).  Filling a big buffer once and serving the guest's reads out
    * of it takes the socket — and the lock hand-off — out of the inner loop. */
   uint8_t *ra;
   size_t ra_len, ra_pos, ra_cap;
};

/* Performs one exchange.  `headers` are sent as given, minus the ones this
 * layer owns (Host, Content-Length, Connection).  Returns false and fills
 * out->error on a transport failure; an HTTP error status is a success here,
 * exactly as it is for HttpURLConnection.getResponseCode(). */
bool dvm_http_perform(const char *method, const char *url,
                      const struct dvm_http_header *headers, int nheaders,
                      const uint8_t *body, size_t body_len,
                      int timeout_ms, bool follow_redirects,
                      struct dvm_http_response *out);

void dvm_http_response_free(struct dvm_http_response *r);

/* Reads the next piece of the body.  Returns 0 at end of body, -1 on a
 * transport error.  A chunked response is decoded whole on the first call
 * (server-side chunking is only used here for small API replies); a
 * Content-Length response streams straight off the socket. */
long dvm_http_read(struct dvm_http_response *r, void *buf, size_t n);

/* Bytes dvm_http_read() can return without touching the socket.  A caller that
 * drops a lock around the read uses this to skip doing so when it would not
 * block. */
size_t dvm_http_avail(const struct dvm_http_response *r);

/* Reads whatever is left of the body into r->body / r->body_len, for the
 * callers that want it as one array (error bodies, small API replies).
 * Returns false only on a transport error. */
bool dvm_http_slurp(struct dvm_http_response *r);

/* ------------------------------------------------------------------------ *
 * Stream sockets for java.net.Socket / javax.net.ssl.SSLSocket
 *
 * The HTTP client above owns its connection from open to close.  A library
 * that speaks HTTP itself (OkHttp, and anything built on Okio) needs the layer
 * underneath instead: a connected TCP socket it reads and writes, and TLS
 * layered on that socket after the fact.  These are that layer.  All of them
 * block; the callers drop the interpreter and execution locks around them.
 * ------------------------------------------------------------------------ */

/* Connects to host:port (a name or a numeric address), IPv4 first, bound
 * first to local_addr:local_port when either is given (NULL / 0 = any).
 * Returns the fd, or -1 with errno set and err filled.  timeout_ms <= 0 waits
 * for as long as the kernel does. */
int dvm_sock_connect(const char *host, int port, const char *local_addr,
                     int local_port, int timeout_ms, char *err, size_t errsz);

/* SO_RCVTIMEO for a Socket's soTimeout; 0 means block forever. */
void dvm_sock_set_timeout(int fd, int timeout_ms);

struct dvm_tls;

#define DVM_TLS_VERIFY_CHAIN 1   /* against the host trust store */
#define DVM_TLS_VERIFY_HOST  2   /* the certificate names `host` */

/* TLS client handshake over a connected fd.  `host` is the server name sent
 * in SNI.  `verify` says what the handshake itself checks: the chain against
 * the host's trust store (what Android's default TrustManagerImpl does against
 * its own) and, only when asked, the name — on a device an SSLSocket leaves
 * the name to the HostnameVerifier unless the endpoint identification
 * algorithm is set.  `alpn` is the ALPN protocol list in wire format (length-
 * prefixed), or NULL.  `min_version`/`max_version` are TLS1_x_VERSION values,
 * 0 for the library default.  `ciphers` are IANA/Java cipher-suite names, or
 * NULL for the default set.  Returns NULL with err filled on failure;
 * *verify_failed says whether it was the certificate that was rejected. */
struct dvm_tls *dvm_tls_connect(int fd, const char *host, int verify,
                                const uint8_t *alpn, size_t alpn_len,
                                int min_version, int max_version,
                                const char *const *ciphers, int nciphers,
                                bool *verify_failed, char *err, size_t errsz);

/* 0 at end of stream, -1 on error, -2 when timeout_ms (> 0) passed with
 * nothing to read.  Safe against a concurrent dvm_tls_write(). */
long dvm_tls_read(struct dvm_tls *t, void *buf, size_t n, int timeout_ms);
/* Bytes written, or -1. */
long dvm_tls_write(struct dvm_tls *t, const void *buf, size_t n);
/* Decrypted bytes available without touching the socket. */
size_t dvm_tls_pending(struct dvm_tls *t);
/* Sends close_notify and frees the session.  Does not close the fd.  No other
 * call may be in progress on it. */
void dvm_tls_free(struct dvm_tls *t);

/* "TLSv1.3", "TLSv1.2", ... — the names SSLSession.getProtocol() reports. */
const char *dvm_tls_protocol(const struct dvm_tls *t);
/* IANA name, e.g. "TLS_AES_128_GCM_SHA256" — SSLSession.getCipherSuite(). */
const char *dvm_tls_cipher(const struct dvm_tls *t);
/* Negotiated ALPN protocol, or "" when none was. */
void dvm_tls_alpn(const struct dvm_tls *t, char *out, size_t outsz);
/* The peer's chain, leaf first, as DER.  Returns the count; the caller frees
 * each (*der)[i] and *der, *len. */
int dvm_tls_peer_chain(const struct dvm_tls *t, uint8_t ***der, int **len);
/* The key-exchange/authentication name a TrustManager is handed as
 * authType ("ECDHE_RSA", "RSA", ..., "GENERIC" for TLS 1.3). */
const char *dvm_tls_auth_type(const struct dvm_tls *t);
/* Session id bytes (may be empty). */
size_t dvm_tls_session_id(const struct dvm_tls *t, uint8_t *out, size_t outsz);

/* IANA names of the cipher suites this TLS library can offer, NULL-terminated
 * and owned by the library. */
const char *const *dvm_tls_supported_ciphers(void);
