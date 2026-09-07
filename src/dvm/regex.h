/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef LUNARIA_DVM_REGEX_H
#define LUNARIA_DVM_REGEX_H

/* A java.util.regex engine.
 *
 * The VM used to translate Java patterns into POSIX ERE and hand them to the
 * host's regcomp()/regexec().  That is not the same language: POSIX ERE has no
 * reluctant quantifier, no lookaround, no backreference, no \b, no \A/\z, and
 * leftmost-longest instead of Java's leftmost-first semantics.  Every one of
 * those differences is silent — the translation either drops the construct or
 * refuses the pattern, and app code sees a PatternSyntaxException or, worse, a
 * match of the wrong extent.  `(.*?)` — the single most common idiom in
 * hand-written parsers — came out as a greedy `(.*)`.
 *
 * So this is a backtracking matcher written against Java's grammar directly.
 * It works over UTF-8 and reports byte offsets, which is what the VM's string
 * objects are; the callers convert when they have to.
 *
 * The engine is self-contained: it depends on nothing from the VM, so it can
 * be tested on its own (test/regex_test.c). */

#include <stdbool.h>
#include <stddef.h>

/* Pattern flag bits, spelled exactly as java.util.regex.Pattern does, so a
 * guest's Pattern.compile(s, flags) passes its int through untouched. */
enum {
   RX_UNIX_LINES               = 0x01,
   RX_CASE_INSENSITIVE         = 0x02,
   RX_COMMENTS                 = 0x04,
   RX_MULTILINE                = 0x08,
   RX_LITERAL                  = 0x10,
   RX_DOTALL                   = 0x20,
   RX_UNICODE_CASE             = 0x40,
   RX_CANON_EQ                 = 0x80,
   RX_UNICODE_CHARACTER_CLASS  = 0x100,
};

struct rx;

/* Compile `pattern`.  On failure returns NULL and, when `err` is non-NULL,
 * points it at a static-lifetime-free heap string describing the problem in
 * the shape java.util.regex.PatternSyntaxException wants; the caller frees. */
struct rx *rx_compile(const char *pattern, int flags, char **err);
void rx_free(struct rx *re);

/* The pattern text as it was handed to rx_compile(). */
const char *rx_source(const struct rx *re);
int rx_flags(const struct rx *re);
/* Number of capturing groups, not counting group 0. */
int rx_group_count(const struct rx *re);
/* Index of a named group, or -1. */
int rx_group_index(const struct rx *re, const char *name);

/* Search `text[0..len)` for a match starting at or after `from`.
 *
 * `caps` receives 2*(rx_group_count()+1) byte offsets — start/end per group,
 * -1/-1 for a group that did not participate.  `ncaps` is the number of ints
 * `caps` can hold; groups beyond it are simply not reported.
 *
 * `anchor_start` requires the match to begin exactly at `from` (lookingAt);
 * `anchor_end` requires it to end at `len` (matches()).  `from` also fixes
 * where \G matches. */
bool rx_search(const struct rx *re, const char *text, size_t len, size_t from,
               int *caps, int ncaps, bool anchor_start, bool anchor_end);

/* A bounded compiled-pattern cache, so String.matches()/replaceAll() in a loop
 * do not recompile (and, since builtin objects have no finalizer, leak) once
 * per call.  rx_cached() hands out a reference; give it back with
 * rx_cached_release(), which frees the pattern if the table had no room for
 * it.  Never call rx_free() on a pattern from rx_cached(). */
const struct rx *rx_cached(const char *pattern, int flags, char **err);
void rx_cached_release(const struct rx *re);

#endif /* LUNARIA_DVM_REGEX_H */
