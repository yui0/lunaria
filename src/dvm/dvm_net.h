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
   uint8_t *body;
   size_t body_len;
   char error[512];                 /* empty on success */
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
