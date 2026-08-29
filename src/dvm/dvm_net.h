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
