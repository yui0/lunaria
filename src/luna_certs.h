/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNA_CERTS_H
#define LUNA_CERTS_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller owns the stream. Respects the host OpenSSL CA-file configuration. */
FILE *luna_certs_open_system_bundle(void);
/* Existing directory, PEM input. Both OpenSSL hash generations are indexed;
 * collisions get consecutive suffixes. Returns certificate count or -1. */
int luna_certs_make_hash_cache(FILE *source,const char *directory);
#ifdef __cplusplus
}
#endif
#endif
