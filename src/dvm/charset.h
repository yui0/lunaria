/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * java.nio.charset for the VM: turning bytes in a named charset into the
 * VM's string storage and back.
 *
 * Strings here are stored as UTF-8, except that a lone surrogate — which a
 * Java String can hold and UTF-8 cannot — is kept as its three-byte form
 * (WTF-8).  Everything below converts between that and the charset's bytes
 * with the rules a device applies:
 *
 *  - decoding never fails: malformed input becomes U+FFFD, one per maximal
 *    ill-formed subsequence (Unicode 3.9, the ICU/Android behaviour);
 *  - encoding never fails: a character the charset cannot represent becomes
 *    the charset's replacement, '?' for every charset this handles;
 *  - "UTF-16" reads a byte-order mark (big-endian without one) and writes a
 *    big-endian one.
 *
 * UTF-8, ISO-8859-1, US-ASCII and the UTF-16 family are done here; every
 * other charset (GBK, Shift_JIS, EUC-KR, Big5, windows-125x …) goes through
 * ICU's converters, which is what Android's own charsets are.
 */
#ifndef LUNARIA_DVM_CHARSET_H
#define LUNARIA_DVM_CHARSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum jcs_kind {
   JCS_UTF8,
   JCS_LATIN1,
   JCS_ASCII,
   JCS_UTF16,      /* BOM-detecting on decode, BOM + big-endian on encode */
   JCS_UTF16BE,
   JCS_UTF16LE,
   JCS_ICU,
};

struct jcs {
   enum jcs_kind kind;
   char name[64];   /* Java's canonical name, what Charset.name() returns */
};

/* Charset.isSupported's name syntax: letters, digits and "-+:_.", starting
 * with a letter or digit.  A name that fails this is an
 * IllegalCharsetNameException rather than an unsupported charset. */
bool jcs_name_legal(const char *name);

/* Resolves a charset name or alias (case-insensitive).  False when no
 * charset of that name exists. */
bool jcs_lookup(const char *name, struct jcs *out);

/* The platform default charset, UTF-8 on Android. */
void jcs_default(struct jcs *out);

/* Bytes in `cs` to WTF-8.  The result is NUL-terminated; *out_len excludes
 * the terminator.  NULL only when memory runs out. */
char *jcs_decode(const struct jcs *cs, const uint8_t *in, size_t n,
                 size_t *out_len);

/* WTF-8 to bytes in `cs`.  NULL only when memory runs out. */
uint8_t *jcs_encode(const struct jcs *cs, const char *wtf8, size_t n,
                    size_t *out_len);

/* UTF-16 code units to WTF-8: surrogate pairs become one four-byte sequence,
 * a lone surrogate its three-byte form.  `out` needs 3 * n bytes; returns the
 * bytes written (no terminator). */
size_t jcs_utf16_to_wtf8(const uint16_t *u, size_t n, char *out);

/* WTF-8 to UTF-16 code units.  `out` needs n units; returns the count. */
size_t jcs_wtf8_to_utf16(const char *s, size_t n, uint16_t *out);

/* A decoder for a byte stream (InputStreamReader): bytes are pulled from
 * the caller one at a time and UTF-16 units come out in order. */
struct jcs_decoder;

struct jcs_decoder *jcs_decoder_new(const struct jcs *cs);
void jcs_decoder_free(struct jcs_decoder *d);

/* The next UTF-16 unit.  `next_byte` returns the next byte (0..255), -1 at
 * the end of the input, or -2 when reading failed; a -2 is passed straight
 * back.  Returns -1 once the input is exhausted and every unit delivered. */
int jcs_decoder_next(struct jcs_decoder *d, int (*next_byte)(void *ctx),
                     void *ctx);

#endif
