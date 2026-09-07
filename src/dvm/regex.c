/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* A java.util.regex engine — see regex.h for why the host's POSIX ERE is not
 * one.
 *
 * The shape is the ordinary one for a language with reluctant quantifiers,
 * lookaround and backreferences: parse to a tree, then match by backtracking
 * with an explicit continuation.  Java's semantics are leftmost-first (the
 * first match the backtracker finds wins, not the longest), which is exactly
 * what falls out of that, and is the one thing a POSIX engine cannot be made
 * to do.
 *
 * Positions are byte offsets into UTF-8, because that is how the VM stores a
 * String; quantifiers and classes still work on whole code points.
 */

#include "regex.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Java's own limit is far higher, but a pattern with a three-digit group
 * count does not exist in app code, and a fixed bound keeps the group vectors
 * off the heap on every backtracking frame that has to save them. */
#define RX_MAX_GROUPS 64
/* Work budget for one rx_search().  A backtracker is exponential on patterns
 * built to be (`(a+)+b` against a long run of a's); rather than hang the VM,
 * give up and report "no match", the same answer a timeout would produce. */
#define RX_STEP_MAX   20000000L
/* Recursion budget.  Simple quantifiers (a literal, `.`, a class) are matched
 * iteratively below and cost one frame however long they run, so this only
 * bounds nesting of groups and lookaround. */
#define RX_DEPTH_MAX  4000
/* How far a lookbehind scans back.  Java only allows a bounded-width
 * lookbehind; this is the bound, in code points. */
#define RX_LOOKBEHIND_MAX 1000

/* strdup() is POSIX, not C11, and this file is compiled with -std=c11 in the
 * standalone test build as well as inside the VM. */
static char *rx_strdup(const char *s)
{
   size_t n = strlen(s) + 1;
   char *p = malloc(n);
   if (p) memcpy(p, s, n);
   return p;
}

/* ------------------------------------------------------------------ UTF-8 */

/* Decode the code point at *i, advancing it.  A byte that is not part of a
 * well-formed sequence decodes as itself, so a pattern or subject holding
 * Latin-1 or truncated UTF-8 still matches byte-for-byte instead of throwing
 * the rest of the string away. */
static uint32_t rx_u8(const char *s, size_t len, size_t *i)
{
   const unsigned char *p = (const unsigned char *)s;
   unsigned char c = p[*i];
   size_t avail = len - *i;
   if (c < 0x80u) { ++*i; return c; }
   unsigned need, cp;
   if ((c & 0xE0u) == 0xC0u)      { need = 1; cp = c & 0x1Fu; }
   else if ((c & 0xF0u) == 0xE0u) { need = 2; cp = c & 0x0Fu; }
   else if ((c & 0xF8u) == 0xF0u) { need = 3; cp = c & 0x07u; }
   else                           { ++*i; return c; }
   if (avail <= need) { ++*i; return c; }
   for (unsigned k = 1; k <= need; ++k) {
      if ((p[*i + k] & 0xC0u) != 0x80u) { ++*i; return c; }
      cp = (cp << 6) | (p[*i + k] & 0x3Fu);
   }
   *i += need + 1;
   return cp;
}

/* Step back one code point from `i`. */
static size_t rx_u8_prev(const char *s, size_t i)
{
   const unsigned char *p = (const unsigned char *)s;
   size_t j = i;
   while (j > 0) {
      --j;
      if ((p[j] & 0xC0u) != 0x80u) break;
      if (i - j >= 4) break;      /* not a continuation run: one byte back */
   }
   return j;
}

/* ------------------------------------------------------------- case folding
 *
 * The blocks whose upper/lower halves are a fixed distance apart.  Java folds
 * by the full Unicode tables; this covers ASCII, Latin-1, Latin Extended-A,
 * Greek and Cyrillic, which is every alphabet an app's patterns fold in
 * practice, and leaves everything else alone (folding a code point to itself
 * is never wrong, only incomplete). */
static uint32_t rx_lower(uint32_t c)
{
   if (c < 128u) return (c >= 'A' && c <= 'Z') ? c + 32u : c;
   if ((c >= 0xC0u && c <= 0xDEu) && c != 0xD7u) return c + 32u;
   if (c >= 0x100u && c <= 0x177u) return (c & 1u) ? c : c + 1u;
   if (c >= 0x391u && c <= 0x3ABu) return c + 32u;
   if (c >= 0x410u && c <= 0x42Fu) return c + 32u;
   if (c >= 0x400u && c <= 0x40Fu) return c + 80u;
   return c;
}

static uint32_t rx_upper(uint32_t c)
{
   if (c < 128u) return (c >= 'a' && c <= 'z') ? c - 32u : c;
   if ((c >= 0xE0u && c <= 0xFEu) && c != 0xF7u) return c - 32u;
   if (c >= 0x100u && c <= 0x177u) return (c & 1u) ? c - 1u : c;
   if (c >= 0x3B1u && c <= 0x3CBu) return c - 32u;
   if (c >= 0x430u && c <= 0x44Fu) return c - 32u;
   if (c >= 0x450u && c <= 0x45Fu) return c - 80u;
   return c;
}

static bool rx_cp_eq(uint32_t a, uint32_t b, bool icase)
{
   if (a == b) return true;
   if (!icase) return false;
   return rx_lower(a) == rx_lower(b) || rx_upper(a) == rx_upper(b);
}

/* -------------------------------------------------------- character classes */

struct rx_range { uint32_t lo, hi; };

struct rx_cls {
   struct rx_range *r;
   int n, cap;
   unsigned char negate;
};

#define RX_CP_MAX 0x10FFFFu

static bool cls_add(struct rx_cls *c, uint32_t lo, uint32_t hi)
{
   if (lo > hi) { uint32_t t = lo; lo = hi; hi = t; }
   if (c->n == c->cap) {
      int cap = c->cap ? c->cap * 2 : 8;
      struct rx_range *r = realloc(c->r, (size_t)cap * sizeof *r);
      if (!r) return false;
      c->r = r; c->cap = cap;
   }
   c->r[c->n].lo = lo; c->r[c->n].hi = hi; ++c->n;
   return true;
}

static int cls_cmp(const void *a, const void *b)
{
   const struct rx_range *x = a, *y = b;
   return x->lo < y->lo ? -1 : x->lo > y->lo ? 1 : 0;
}

static void cls_normalize(struct rx_cls *c)
{
   if (c->n < 2) return;
   qsort(c->r, (size_t)c->n, sizeof *c->r, cls_cmp);
   int w = 0;
   for (int i = 1; i < c->n; ++i) {
      if (c->r[i].lo <= c->r[w].hi + 1u) {
         if (c->r[i].hi > c->r[w].hi) c->r[w].hi = c->r[i].hi;
      } else {
         c->r[++w] = c->r[i];
      }
   }
   c->n = w + 1;
}

/* Replace the set with its complement over [0, RX_CP_MAX]. */
static bool cls_complement(struct rx_cls *c)
{
   cls_normalize(c);
   struct rx_cls out = { 0 };
   uint32_t next = 0;
   for (int i = 0; i < c->n; ++i) {
      if (c->r[i].lo > next && !cls_add(&out, next, c->r[i].lo - 1u))
         { free(out.r); return false; }
      if (c->r[i].hi >= RX_CP_MAX) { next = RX_CP_MAX; goto done; }
      if (c->r[i].hi + 1u > next) next = c->r[i].hi + 1u;
   }
   if (next <= RX_CP_MAX && !cls_add(&out, next, RX_CP_MAX))
      { free(out.r); return false; }
done:
   free(c->r);
   c->r = out.r; c->n = out.n; c->cap = out.cap;
   return true;
}

/* dst |= src, honouring src's own negation. */
static bool cls_union(struct rx_cls *dst, struct rx_cls *src)
{
   if (src->negate) {
      if (!cls_complement(src)) return false;
      src->negate = 0;
   }
   for (int i = 0; i < src->n; ++i)
      if (!cls_add(dst, src->r[i].lo, src->r[i].hi)) return false;
   return true;
}

/* dst &= src — Java's [a-z&&[^bc]]. */
static bool cls_intersect(struct rx_cls *dst, struct rx_cls *src)
{
   if (src->negate) {
      if (!cls_complement(src)) return false;
      src->negate = 0;
   }
   cls_normalize(dst);
   cls_normalize(src);
   struct rx_cls out = { 0 };
   int i = 0, j = 0;
   while (i < dst->n && j < src->n) {
      uint32_t lo = dst->r[i].lo > src->r[j].lo ? dst->r[i].lo : src->r[j].lo;
      uint32_t hi = dst->r[i].hi < src->r[j].hi ? dst->r[i].hi : src->r[j].hi;
      if (lo <= hi && !cls_add(&out, lo, hi)) { free(out.r); return false; }
      if (dst->r[i].hi < src->r[j].hi) ++i; else ++j;
   }
   free(dst->r);
   dst->r = out.r; dst->n = out.n; dst->cap = out.cap;
   return true;
}

static bool cls_has(const struct rx_cls *c, uint32_t cp)
{
   for (int i = 0; i < c->n; ++i)
      if (cp >= c->r[i].lo && cp <= c->r[i].hi) return true;
   return false;
}

static bool cls_test(const struct rx_cls *c, uint32_t cp, bool icase)
{
   bool in = cls_has(c, cp);
   if (!in && icase)
      in = cls_has(c, rx_lower(cp)) || cls_has(c, rx_upper(cp));
   return c->negate ? !in : in;
}

/* ------------------------------------------------- predefined class bodies
 *
 * Above ASCII these are approximations, and deliberately so: the VM has no
 * Unicode character database and one would be a megabyte of tables for a set
 * of patterns that, in the apps this runs, are \d, \s, \w, \p{C} and the
 * POSIX names.  What each approximation commits to is written next to it.
 * The rule followed throughout: be exact on ASCII, and above it prefer to
 * classify a code point as a letter, because that is what the overwhelming
 * majority of assigned code points are. */

enum {
   PC_LOWER, PC_UPPER, PC_ALPHA, PC_DIGIT, PC_ALNUM, PC_PUNCT, PC_GRAPH,
   PC_PRINT, PC_BLANK, PC_CNTRL, PC_XDIGIT, PC_SPACE, PC_ASCII,
   PC_WORD, PC_LETTER, PC_MARK, PC_NUMBER, PC_SYMBOL, PC_SEPARATOR, PC_OTHER,
};

/* Code points at or above 0x80 that are *not* letters.  Everything else above
 * ASCII is treated as a letter (PC_LETTER / PC_ALPHA). */
static const struct rx_range kNonLetterHigh[] = {
   { 0x0080u, 0x00A9u }, { 0x00ABu, 0x00B4u }, { 0x00B6u, 0x00B9u },
   { 0x00BBu, 0x00BFu }, { 0x00D7u, 0x00D7u }, { 0x00F7u, 0x00F7u },
   { 0x02B0u, 0x036Fu }, { 0x2000u, 0x2BFFu }, { 0x3000u, 0x303Fu },
   { 0xFE10u, 0xFE6Fu }, { 0xFF01u, 0xFF20u }, { 0xFF3Bu, 0xFF40u },
   { 0xFF5Bu, 0xFF65u }, { 0xFFF0u, 0xFFFFu },
};

static bool cls_add_predef(struct rx_cls *c, int which)
{
   bool ok = true;
   switch (which) {
   case PC_LOWER:  ok = cls_add(c, 'a', 'z'); break;
   case PC_UPPER:  ok = cls_add(c, 'A', 'Z'); break;
   case PC_DIGIT:  ok = cls_add(c, '0', '9'); break;
   case PC_XDIGIT: ok = cls_add(c, '0', '9') && cls_add(c, 'a', 'f') &&
                        cls_add(c, 'A', 'F'); break;
   case PC_BLANK:  ok = cls_add(c, ' ', ' ') && cls_add(c, '\t', '\t'); break;
   case PC_ASCII:  ok = cls_add(c, 0u, 0x7Fu); break;
   case PC_ALPHA:
   case PC_LETTER: {
      /* ASCII letters, then everything above ASCII minus the ranges above. */
      struct rx_cls high = { 0 };
      for (size_t i = 0; i < sizeof kNonLetterHigh / sizeof kNonLetterHigh[0]; ++i)
         if (!cls_add(&high, kNonLetterHigh[i].lo, kNonLetterHigh[i].hi))
            { free(high.r); return false; }
      if (!cls_add(&high, 0u, 0x7Fu)) { free(high.r); return false; }
      if (!cls_complement(&high)) { free(high.r); return false; }
      ok = cls_add(c, 'a', 'z') && cls_add(c, 'A', 'Z') && cls_union(c, &high);
      free(high.r);
      break;
   }
   case PC_ALNUM:
      ok = cls_add_predef(c, PC_ALPHA) && cls_add_predef(c, PC_DIGIT);
      break;
   case PC_WORD:
      ok = cls_add(c, 'a', 'z') && cls_add(c, 'A', 'Z') &&
           cls_add(c, '0', '9') && cls_add(c, '_', '_');
      break;
   case PC_NUMBER:
      /* Decimal digits of the scripts an app is likely to see. */
      ok = cls_add(c, '0', '9') && cls_add(c, 0x0660u, 0x0669u) &&
           cls_add(c, 0x06F0u, 0x06F9u) && cls_add(c, 0x0966u, 0x096Fu) &&
           cls_add(c, 0xFF10u, 0xFF19u);
      break;
   case PC_SPACE:
      ok = cls_add(c, '\t', '\r') && cls_add(c, ' ', ' ') &&
           cls_add(c, 0x00A0u, 0x00A0u) && cls_add(c, 0x1680u, 0x1680u) &&
           cls_add(c, 0x2000u, 0x200Au) && cls_add(c, 0x2028u, 0x2029u) &&
           cls_add(c, 0x202Fu, 0x202Fu) && cls_add(c, 0x205Fu, 0x205Fu) &&
           cls_add(c, 0x3000u, 0x3000u);
      break;
   case PC_SEPARATOR:
      ok = cls_add(c, ' ', ' ') && cls_add(c, 0x00A0u, 0x00A0u) &&
           cls_add(c, 0x1680u, 0x1680u) && cls_add(c, 0x2000u, 0x200Au) &&
           cls_add(c, 0x2028u, 0x2029u) && cls_add(c, 0x202Fu, 0x202Fu) &&
           cls_add(c, 0x205Fu, 0x205Fu) && cls_add(c, 0x3000u, 0x3000u);
      break;
   case PC_CNTRL:
   case PC_OTHER:
      /* \p{C} is how a payload gets stripped of non-printing characters
       * (AppsFlyer does exactly replaceAll("\\p{C}", "")), so this has to
       * cover the format characters as well as the two control blocks —
       * and nothing else, or the strip eats the message. */
      ok = cls_add(c, 0u, 0x1Fu) && cls_add(c, 0x7Fu, 0x9Fu) &&
           cls_add(c, 0x00ADu, 0x00ADu) && cls_add(c, 0x200Bu, 0x200Fu) &&
           cls_add(c, 0x202Au, 0x202Eu) && cls_add(c, 0x2060u, 0x2064u) &&
           cls_add(c, 0x206Au, 0x206Fu) && cls_add(c, 0xFEFFu, 0xFEFFu) &&
           cls_add(c, 0xFFF9u, 0xFFFBu);
      break;
   case PC_PUNCT:
      ok = cls_add(c, '!', '/') && cls_add(c, ':', '@') &&
           cls_add(c, '[', '`') && cls_add(c, '{', '~') &&
           cls_add(c, 0x2010u, 0x205Eu) && cls_add(c, 0x3001u, 0x303Fu) &&
           cls_add(c, 0xFF01u, 0xFF20u) && cls_add(c, 0xFF3Bu, 0xFF40u) &&
           cls_add(c, 0xFF5Bu, 0xFF65u);
      break;
   case PC_SYMBOL:
      ok = cls_add(c, '$', '$') && cls_add(c, '+', '+') &&
           cls_add(c, '<', '>') && cls_add(c, '^', '^') &&
           cls_add(c, '`', '`') && cls_add(c, '|', '|') &&
           cls_add(c, '~', '~') && cls_add(c, 0x00A2u, 0x00A6u) &&
           cls_add(c, 0x20A0u, 0x20CFu) && cls_add(c, 0x2100u, 0x2BFFu);
      break;
   case PC_MARK:
      ok = cls_add(c, 0x0300u, 0x036Fu) && cls_add(c, 0x1AB0u, 0x1AFFu) &&
           cls_add(c, 0x20D0u, 0x20FFu) && cls_add(c, 0xFE20u, 0xFE2Fu);
      break;
   case PC_GRAPH:
      ok = cls_add(c, 0x21u, 0x7Eu) && cls_add_predef(c, PC_ALPHA);
      break;
   case PC_PRINT:
      ok = cls_add(c, 0x20u, 0x7Eu) && cls_add_predef(c, PC_ALPHA);
      break;
   default: ok = false; break;
   }
   return ok;
}

/* Java's class names, POSIX and Unicode-property alike, mapped onto the above.
 * The one- and two-letter Unicode general categories collapse onto the closest
 * approximation the table can express. */
struct rx_named { const char *name; int which; };
static const struct rx_named kNamed[] = {
   { "Lower", PC_LOWER }, { "Upper", PC_UPPER }, { "Alpha", PC_ALPHA },
   { "Digit", PC_DIGIT }, { "Alnum", PC_ALNUM }, { "Punct", PC_PUNCT },
   { "Graph", PC_GRAPH }, { "Print", PC_PRINT }, { "Blank", PC_BLANK },
   { "Cntrl", PC_CNTRL }, { "XDigit", PC_XDIGIT }, { "Space", PC_SPACE },
   { "ASCII", PC_ASCII }, { "javaWhitespace", PC_SPACE },
   { "javaLetter", PC_LETTER }, { "javaLetterOrDigit", PC_ALNUM },
   { "javaDigit", PC_DIGIT }, { "javaLowerCase", PC_LOWER },
   { "javaUpperCase", PC_UPPER },
   { "L", PC_LETTER }, { "Lu", PC_UPPER }, { "Ll", PC_LOWER },
   { "Lt", PC_UPPER }, { "Lm", PC_LETTER }, { "Lo", PC_LETTER },
   { "M", PC_MARK }, { "Mn", PC_MARK }, { "Mc", PC_MARK }, { "Me", PC_MARK },
   { "N", PC_NUMBER }, { "Nd", PC_NUMBER }, { "Nl", PC_NUMBER },
   { "No", PC_NUMBER },
   { "P", PC_PUNCT }, { "Pc", PC_PUNCT }, { "Pd", PC_PUNCT },
   { "Ps", PC_PUNCT }, { "Pe", PC_PUNCT }, { "Pi", PC_PUNCT },
   { "Pf", PC_PUNCT }, { "Po", PC_PUNCT },
   { "S", PC_SYMBOL }, { "Sm", PC_SYMBOL }, { "Sc", PC_SYMBOL },
   { "Sk", PC_SYMBOL }, { "So", PC_SYMBOL },
   { "Z", PC_SEPARATOR }, { "Zs", PC_SEPARATOR }, { "Zl", PC_SEPARATOR },
   { "Zp", PC_SEPARATOR },
   { "C", PC_OTHER }, { "Cc", PC_CNTRL }, { "Cf", PC_OTHER },
   { "Cn", PC_OTHER }, { "Co", PC_OTHER }, { "Cs", PC_OTHER },
};

static int named_class(const char *name, size_t len)
{
   for (size_t i = 0; i < sizeof kNamed / sizeof kNamed[0]; ++i)
      if (strlen(kNamed[i].name) == len && !strncmp(kNamed[i].name, name, len))
         return kNamed[i].which;
   return -1;
}

/* --------------------------------------------------------------- the tree */

enum {
   OP_CHAR, OP_ANY, OP_CLASS, OP_GROUP, OP_ATOMIC, OP_BACKREF,
   OP_BOL, OP_EOL, OP_BOS, OP_EOS, OP_EOS_FINAL, OP_GPOS,
   OP_WORDB, OP_NWORDB, OP_LOOKAHEAD, OP_LOOKBEHIND,
};

enum { Q_GREEDY, Q_RELUCTANT, Q_POSSESSIVE };

struct rx_node {
   unsigned char op, quant, negate;
   unsigned char icase, dotall, multiline, unixlines;
   int min, max;                 /* max < 0: unbounded */
   uint32_t ch;                  /* OP_CHAR */
   struct rx_cls *cls;           /* OP_CLASS */
   struct rx_node **alts;        /* OP_GROUP / OP_ATOMIC / lookaround */
   int nalts;
   int group;                    /* >0: capturing index */
   int ref;                      /* OP_BACKREF */
   struct rx_node *next;
};

struct rx_name { char *name; int idx; };

struct rx {
   struct rx_node **alts;
   int nalts;
   int ngroups;
   int flags;
   char *src;
   struct rx_name *names;
   int nnames;
   /* Everything the parser allocated, so rx_free() is a walk of two arrays
    * instead of a tree traversal that has to agree with the parser about
    * ownership. */
   struct rx_node **pool;  int npool, poolcap;
   struct rx_cls **clspool; int ncls, clscap;
   struct rx_node ***altpool; int naltp, altpcap;
};

static struct rx_node *node_new(struct rx *re, int op)
{
   struct rx_node *n = calloc(1, sizeof *n);
   if (!n) return NULL;
   if (re->npool == re->poolcap) {
      int cap = re->poolcap ? re->poolcap * 2 : 32;
      struct rx_node **p = realloc(re->pool, (size_t)cap * sizeof *p);
      if (!p) { free(n); return NULL; }
      re->pool = p; re->poolcap = cap;
   }
   re->pool[re->npool++] = n;
   n->op = (unsigned char)op;
   n->min = n->max = 1;
   n->quant = Q_GREEDY;
   return n;
}

static struct rx_cls *cls_new(struct rx *re)
{
   struct rx_cls *c = calloc(1, sizeof *c);
   if (!c) return NULL;
   if (re->ncls == re->clscap) {
      int cap = re->clscap ? re->clscap * 2 : 16;
      struct rx_cls **p = realloc(re->clspool, (size_t)cap * sizeof *p);
      if (!p) { free(c); return NULL; }
      re->clspool = p; re->clscap = cap;
   }
   re->clspool[re->ncls++] = c;
   return c;
}

static bool alts_track(struct rx *re, struct rx_node **a)
{
   if (re->naltp == re->altpcap) {
      int cap = re->altpcap ? re->altpcap * 2 : 16;
      struct rx_node ***p = realloc(re->altpool, (size_t)cap * sizeof *p);
      if (!p) return false;
      re->altpool = p; re->altpcap = cap;
   }
   re->altpool[re->naltp++] = a;
   return true;
}

void rx_free(struct rx *re)
{
   if (!re) return;
   for (int i = 0; i < re->npool; ++i) free(re->pool[i]);
   for (int i = 0; i < re->ncls; ++i) { free(re->clspool[i]->r); free(re->clspool[i]); }
   for (int i = 0; i < re->naltp; ++i) free(re->altpool[i]);
   for (int i = 0; i < re->nnames; ++i) free(re->names[i].name);
   free(re->pool); free(re->clspool); free(re->altpool);
   free(re->names); free(re->src); free(re);
}

/* ------------------------------------------------------------- the parser */

struct rxp {
   const char *p, *e;
   struct rx *re;
   int flags;
   char err[192];
   bool failed;
   int depth;
};

static void fail(struct rxp *ps, const char *what)
{
   if (ps->failed) return;
   ps->failed = true;
   snprintf(ps->err, sizeof ps->err, "%s near index %d",
            what, (int)(ps->p - (ps->re->src ? ps->re->src : ps->p)));
}

static bool at_end(const struct rxp *ps) { return ps->p >= ps->e; }
static char peek(const struct rxp *ps)   { return at_end(ps) ? '\0' : *ps->p; }
static char peek2(const struct rxp *ps)  { return ps->p + 1 < ps->e ? ps->p[1] : '\0'; }

static struct rx_node **parse_alt(struct rxp *ps, int *nalts);

/* Skip whitespace and #-comments when COMMENTS is on. */
static void skip_x(struct rxp *ps)
{
   if (!(ps->flags & RX_COMMENTS)) return;
   for (;;) {
      while (!at_end(ps) && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
                             *ps->p == '\r' || *ps->p == '\f')) ++ps->p;
      if (!at_end(ps) && *ps->p == '#') {
         while (!at_end(ps) && *ps->p != '\n') ++ps->p;
         continue;
      }
      return;
   }
}

static int hexval(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

/* The escape sequences that stand for one code point.  Called with ps->p on
 * the character *after* the backslash; consumes it and any argument. */
static uint32_t esc_char(struct rxp *ps)
{
   char c = peek(ps);
   ++ps->p;
   switch (c) {
   case 'n': return '\n';
   case 'r': return '\r';
   case 't': return '\t';
   case 'f': return '\f';
   case 'a': return 0x07u;
   case 'e': return 0x1Bu;
   case '0': {
      /* \0n, \0nn, \0mnn — octal, one to three digits. */
      uint32_t v = 0; int k = 0;
      while (k < 3 && !at_end(ps) && *ps->p >= '0' && *ps->p <= '7') {
         v = v * 8u + (uint32_t)(*ps->p++ - '0'); ++k;
      }
      if (!k) fail(ps, "Illegal octal escape sequence");
      return v;
   }
   case 'x': {
      if (peek(ps) == '{') {
         ++ps->p;
         uint32_t v = 0; int k = 0;
         while (!at_end(ps) && *ps->p != '}') {
            int h = hexval(*ps->p++);
            if (h < 0) { fail(ps, "Illegal hexadecimal escape sequence"); return 0; }
            v = v * 16u + (uint32_t)h; ++k;
         }
         if (peek(ps) != '}') { fail(ps, "Unclosed hexadecimal escape sequence"); return 0; }
         ++ps->p;
         if (!k) fail(ps, "Illegal hexadecimal escape sequence");
         return v;
      }
      uint32_t v = 0;
      for (int k = 0; k < 2; ++k) {
         int h = at_end(ps) ? -1 : hexval(*ps->p);
         if (h < 0) { fail(ps, "Illegal hexadecimal escape sequence"); return 0; }
         v = v * 16u + (uint32_t)h; ++ps->p;
      }
      return v;
   }
   case 'u': {
      uint32_t v = 0;
      for (int k = 0; k < 4; ++k) {
         int h = at_end(ps) ? -1 : hexval(*ps->p);
         if (h < 0) { fail(ps, "Illegal Unicode escape sequence"); return 0; }
         v = v * 16u + (uint32_t)h; ++ps->p;
      }
      /* A surrogate pair spelled as two \u escapes is one code point. */
      if (v >= 0xD800u && v <= 0xDBFFu && ps->p + 1 < ps->e &&
          ps->p[0] == '\\' && ps->p[1] == 'u') {
         const char *save = ps->p;
         ps->p += 2;
         uint32_t lo = 0; bool okpair = true;
         for (int k = 0; k < 4; ++k) {
            int h = at_end(ps) ? -1 : hexval(*ps->p);
            if (h < 0) { okpair = false; break; }
            lo = lo * 16u + (uint32_t)h; ++ps->p;
         }
         if (okpair && lo >= 0xDC00u && lo <= 0xDFFFu)
            return 0x10000u + ((v - 0xD800u) << 10) + (lo - 0xDC00u);
         ps->p = save;
      }
      return v;
   }
   case 'c': {
      if (at_end(ps)) { fail(ps, "Illegal control escape sequence"); return 0; }
      return (uint32_t)(*ps->p++ & 0x1Fu);
   }
   default: {
      /* Anything else stands for itself, which is how a pattern quotes a
       * metacharacter.  Back up one so a multi-byte character is decoded
       * whole rather than one byte at a time. */
      size_t i = 0;
      const char *s = ps->p - 1;
      size_t avail = (size_t)(ps->e - s);
      uint32_t cp = rx_u8(s, avail, &i);
      ps->p = s + i;
      return cp;
   }
   }
}

/* Build the class an escape stands for, if it is one of the class escapes.
 * Returns NULL and leaves ps->p untouched when the escape is not a class. */
static struct rx_cls *esc_class(struct rxp *ps)
{
   char c = peek(ps);
   int which = -1;
   bool neg = false;
   const char *save = ps->p;

   switch (c) {
   case 'd': which = PC_DIGIT; break;
   case 'D': which = PC_DIGIT; neg = true; break;
   case 's': which = PC_SPACE; break;
   case 'S': which = PC_SPACE; neg = true; break;
   case 'w': which = PC_WORD;  break;
   case 'W': which = PC_WORD;  neg = true; break;
   case 'h': which = PC_BLANK; break;
   case 'H': which = PC_BLANK; neg = true; break;
   case 'v': case 'V': break;                 /* handled below */
   case 'p': case 'P': break;
   default: return NULL;
   }

   struct rx_cls *cls = cls_new(ps->re);
   if (!cls) { fail(ps, "out of memory"); return NULL; }

   if (c == 'v' || c == 'V') {
      ++ps->p;
      if (!cls_add(cls, '\n', '\r') || !cls_add(cls, 0x0085u, 0x0085u) ||
          !cls_add(cls, 0x2028u, 0x2029u)) { fail(ps, "out of memory"); return NULL; }
      cls->negate = (c == 'V');
      return cls;
   }
   if (c == 'p' || c == 'P') {
      neg = (c == 'P');
      ++ps->p;                                /* the p/P */
      const char *name; size_t len;
      if (peek(ps) == '{') {
         ++ps->p;
         name = ps->p;
         while (!at_end(ps) && *ps->p != '}') ++ps->p;
         if (at_end(ps)) { ps->p = save; fail(ps, "Unclosed character family"); return NULL; }
         len = (size_t)(ps->p - name);
         ++ps->p;                             /* the } */
      } else if (!at_end(ps)) {
         name = ps->p; len = 1; ++ps->p;
      } else {
         ps->p = save; fail(ps, "Unclosed character family"); return NULL;
      }
      /* Java allows an Is/In prefix: \p{IsAlpha}, \p{InGreek}. */
      int w = named_class(name, len);
      if (w < 0 && len > 2 && (!strncmp(name, "Is", 2) || !strncmp(name, "In", 2)))
         w = named_class(name + 2, len - 2);
      if (w < 0) {
         /* An unknown family — a script name, say.  Matching nothing is the
          * honest answer and keeps the rest of the pattern usable; refusing
          * it would throw away a pattern whose other branches are fine. */
         cls->negate = neg ? 1 : 0;
         return cls;
      }
      if (!cls_add_predef(cls, w)) { fail(ps, "out of memory"); return NULL; }
      cls->negate = neg ? 1 : 0;
      return cls;
   }

   ++ps->p;
   if (!cls_add_predef(cls, which)) { fail(ps, "out of memory"); return NULL; }
   cls->negate = neg ? 1 : 0;
   return cls;
}

static struct rx_cls *parse_class(struct rxp *ps);

/* One union term of a class body: everything up to `]` or `&&`. */
static struct rx_cls *parse_class_union(struct rxp *ps)
{
   struct rx_cls *cls = cls_new(ps->re);
   if (!cls) { fail(ps, "out of memory"); return NULL; }
   bool first = true;
   for (;;) {
      if (ps->flags & RX_COMMENTS) skip_x(ps);
      if (at_end(ps)) { fail(ps, "Unclosed character class"); return NULL; }
      if (*ps->p == ']' && !first) return cls;
      if (*ps->p == '&' && peek2(ps) == '&') return cls;
      first = false;

      uint32_t lo;
      if (*ps->p == '[') {
         struct rx_cls *sub = parse_class(ps);
         if (!sub) return NULL;
         if (!cls_union(cls, sub)) { fail(ps, "out of memory"); return NULL; }
         continue;
      }
      if (*ps->p == '\\') {
         ++ps->p;
         struct rx_cls *sub = esc_class(ps);
         if (ps->failed) return NULL;
         if (sub) {
            if (!cls_union(cls, sub)) { fail(ps, "out of memory"); return NULL; }
            continue;
         }
         lo = esc_char(ps);
         if (ps->failed) return NULL;
      } else {
         size_t i = 0;
         size_t avail = (size_t)(ps->e - ps->p);
         lo = rx_u8(ps->p, avail, &i);
         ps->p += i;
      }

      /* A '-' only forms a range when a term follows it; `[a-]` and `[-a]`
       * are literal dashes, which is where the ERE translation used to trip
       * (POSIX reads `[0-9-_]` as a descending range and refuses it). */
      if (peek(ps) == '-' && peek2(ps) != ']' && peek2(ps) != '\0' &&
          !(peek2(ps) == '&' && ps->p + 2 < ps->e && ps->p[2] == '&')) {
         ++ps->p;
         uint32_t hi;
         if (peek(ps) == '\\') {
            ++ps->p;
            const char *before = ps->p;
            struct rx_cls *sub = esc_class(ps);
            if (ps->failed) return NULL;
            if (sub) {
               /* `[a-\d]` is not a range; Java rejects it.  Treat the dash as
                * a literal and the class escape as its own term. */
               ps->p = before;
               if (!cls_add(cls, lo, lo) || !cls_add(cls, '-', '-'))
                  { fail(ps, "out of memory"); return NULL; }
               continue;
            }
            hi = esc_char(ps);
            if (ps->failed) return NULL;
         } else {
            size_t i = 0;
            size_t avail = (size_t)(ps->e - ps->p);
            hi = rx_u8(ps->p, avail, &i);
            ps->p += i;
         }
         if (hi < lo) { fail(ps, "Illegal character range"); return NULL; }
         if (!cls_add(cls, lo, hi)) { fail(ps, "out of memory"); return NULL; }
         continue;
      }
      if (!cls_add(cls, lo, lo)) { fail(ps, "out of memory"); return NULL; }
   }
}

/* `[` ... `]`, including Java's `&&` intersection and nested classes. */
static struct rx_cls *parse_class(struct rxp *ps)
{
   ++ps->p;                                   /* the '[' */
   bool neg = false;
   if (peek(ps) == '^') { neg = true; ++ps->p; }
   struct rx_cls *cls = parse_class_union(ps);
   if (!cls) return NULL;
   while (peek(ps) == '&' && peek2(ps) == '&') {
      ps->p += 2;
      struct rx_cls *rhs;
      if (peek(ps) == '[') rhs = parse_class(ps);
      else                 rhs = parse_class_union(ps);
      if (!rhs) return NULL;
      if (!cls_intersect(cls, rhs)) { fail(ps, "out of memory"); return NULL; }
   }
   if (peek(ps) != ']') { fail(ps, "Unclosed character class"); return NULL; }
   ++ps->p;
   cls_normalize(cls);
   cls->negate = neg ? 1 : 0;
   return cls;
}

static void node_stamp(struct rxp *ps, struct rx_node *n)
{
   n->icase     = (ps->flags & RX_CASE_INSENSITIVE) ? 1 : 0;
   n->dotall    = (ps->flags & RX_DOTALL) ? 1 : 0;
   n->multiline = (ps->flags & RX_MULTILINE) ? 1 : 0;
   n->unixlines = (ps->flags & RX_UNIX_LINES) ? 1 : 0;
}

/* Parse the flag letters of `(?idmsux-idmsux...`, returning the new set. */
static int parse_flag_letters(struct rxp *ps, int base)
{
   int on = base, sign = 1;
   for (;;) {
      char c = peek(ps);
      int bit;
      switch (c) {
      case 'i': bit = RX_CASE_INSENSITIVE; break;
      case 'd': bit = RX_UNIX_LINES; break;
      case 'm': bit = RX_MULTILINE; break;
      case 's': bit = RX_DOTALL; break;
      case 'u': bit = RX_UNICODE_CASE; break;
      case 'x': bit = RX_COMMENTS; break;
      case 'U': bit = RX_UNICODE_CHARACTER_CLASS; break;
      case '-': sign = 0; ++ps->p; continue;
      default: return on;
      }
      ++ps->p;
      if (sign) on |= bit; else on &= ~bit;
   }
}

/* A `(?<name>` group name; returns a heap copy or NULL. */
static char *parse_group_name(struct rxp *ps)
{
   const char *s = ps->p;
   while (!at_end(ps) && *ps->p != '>') ++ps->p;
   if (at_end(ps)) { fail(ps, "Unclosed group name"); return NULL; }
   size_t n = (size_t)(ps->p - s);
   ++ps->p;                                   /* the '>' */
   char *name = malloc(n + 1);
   if (!name) { fail(ps, "out of memory"); return NULL; }
   memcpy(name, s, n); name[n] = '\0';
   return name;
}

static bool record_name(struct rx *re, char *name, int idx)
{
   struct rx_name *p = realloc(re->names, (size_t)(re->nnames + 1) * sizeof *p);
   if (!p) { free(name); return false; }
   re->names = p;
   re->names[re->nnames].name = name;
   re->names[re->nnames].idx = idx;
   ++re->nnames;
   return true;
}

/* One atom, without its quantifier.  Returns NULL with ps->failed clear when
 * the construct produced no node (an inline flag group). */
static struct rx_node *parse_atom(struct rxp *ps)
{
   if (ps->depth > RX_DEPTH_MAX) { fail(ps, "Pattern nested too deeply"); return NULL; }
   char c = peek(ps);

   if (c == '(') {
      ++ps->p;
      int op = OP_GROUP, group = 0;
      bool negate = false;
      char *name = NULL;
      const int saved_flags = ps->flags;

      if (peek(ps) == '?') {
         ++ps->p;
         char k = peek(ps);
         if (k == ':') { ++ps->p; }
         else if (k == '=') { ++ps->p; op = OP_LOOKAHEAD; }
         else if (k == '!') { ++ps->p; op = OP_LOOKAHEAD; negate = true; }
         else if (k == '>') { ++ps->p; op = OP_ATOMIC; }
         else if (k == '<' && (peek2(ps) == '=' || peek2(ps) == '!')) {
            negate = (peek2(ps) == '!');
            ps->p += 2;
            op = OP_LOOKBEHIND;
         } else if (k == '<') {
            ++ps->p;
            name = parse_group_name(ps);
            if (!name) return NULL;
            group = ++ps->re->ngroups;
         } else {
            /* `(?flags)` or `(?flags:...)` */
            int nf = parse_flag_letters(ps, ps->flags);
            if (peek(ps) == ')') {
               ++ps->p;
               ps->flags = nf;               /* in force to the end of the group */
               return NULL;                   /* no node */
            }
            if (peek(ps) != ':') { fail(ps, "Unknown inline modifier"); return NULL; }
            ++ps->p;
            ps->flags = nf;
         }
      } else {
         group = ++ps->re->ngroups;
      }
      if (group > RX_MAX_GROUPS) { fail(ps, "Too many capturing groups"); return NULL; }
      if (name && !record_name(ps->re, name, group)) { fail(ps, "out of memory"); return NULL; }

      struct rx_node *n = node_new(ps->re, op);
      if (!n) { fail(ps, "out of memory"); return NULL; }
      node_stamp(ps, n);
      n->group = group;
      n->negate = negate ? 1 : 0;

      ++ps->depth;
      n->alts = parse_alt(ps, &n->nalts);
      --ps->depth;
      if (ps->failed) return NULL;
      if (peek(ps) != ')') { fail(ps, "Unclosed group"); return NULL; }
      ++ps->p;
      ps->flags = saved_flags;               /* flags do not escape their group */
      return n;
   }

   if (c == '[') {
      struct rx_cls *cls = parse_class(ps);
      if (!cls) return NULL;
      struct rx_node *n = node_new(ps->re, OP_CLASS);
      if (!n) { fail(ps, "out of memory"); return NULL; }
      node_stamp(ps, n);
      n->cls = cls;
      return n;
   }

   if (c == '.') {
      ++ps->p;
      struct rx_node *n = node_new(ps->re, OP_ANY);
      if (!n) { fail(ps, "out of memory"); return NULL; }
      node_stamp(ps, n);
      return n;
   }

   if (c == '^' || c == '$') {
      ++ps->p;
      struct rx_node *n = node_new(ps->re, c == '^' ? OP_BOL : OP_EOL);
      if (!n) { fail(ps, "out of memory"); return NULL; }
      node_stamp(ps, n);
      return n;
   }

   if (c == '\\') {
      ++ps->p;
      char k = peek(ps);
      int op = -1;
      switch (k) {
      case 'b': op = OP_WORDB; break;
      case 'B': op = OP_NWORDB; break;
      case 'A': op = OP_BOS; break;
      case 'z': op = OP_EOS; break;
      case 'Z': op = OP_EOS_FINAL; break;
      case 'G': op = OP_GPOS; break;
      default: break;
      }
      if (op >= 0) {
         ++ps->p;
         struct rx_node *n = node_new(ps->re, op);
         if (!n) { fail(ps, "out of memory"); return NULL; }
         node_stamp(ps, n);
         return n;
      }
      if (k == 'k' && peek2(ps) == '<') {
         ps->p += 2;
         char *nm = parse_group_name(ps);
         if (!nm) return NULL;
         int idx = rx_group_index(ps->re, nm);
         free(nm);
         if (idx < 0) { fail(ps, "Unknown group name in backreference"); return NULL; }
         struct rx_node *n = node_new(ps->re, OP_BACKREF);
         if (!n) { fail(ps, "out of memory"); return NULL; }
         node_stamp(ps, n);
         n->ref = idx;
         return n;
      }
      if (k >= '1' && k <= '9') {
         /* Java takes as many digits as name a group that exists. */
         int idx = 0;
         const char *q = ps->p;
         while (q < ps->e && *q >= '0' && *q <= '9') {
            int cand = idx * 10 + (*q - '0');
            if (cand > ps->re->ngroups) break;
            idx = cand; ++q;
         }
         if (idx == 0) { fail(ps, "Illegal backreference"); return NULL; }
         ps->p = q;
         struct rx_node *n = node_new(ps->re, OP_BACKREF);
         if (!n) { fail(ps, "out of memory"); return NULL; }
         node_stamp(ps, n);
         n->ref = idx;
         return n;
      }
      struct rx_cls *cls = esc_class(ps);
      if (ps->failed) return NULL;
      if (cls) {
         struct rx_node *n = node_new(ps->re, OP_CLASS);
         if (!n) { fail(ps, "out of memory"); return NULL; }
         node_stamp(ps, n);
         n->cls = cls;
         return n;
      }
      uint32_t cp = esc_char(ps);
      if (ps->failed) return NULL;
      struct rx_node *n = node_new(ps->re, OP_CHAR);
      if (!n) { fail(ps, "out of memory"); return NULL; }
      node_stamp(ps, n);
      n->ch = cp;
      return n;
   }

   if (c == ')' || c == '|' || c == '\0') { fail(ps, "Unexpected token"); return NULL; }
   if (c == '*' || c == '+' || c == '?') { fail(ps, "Dangling meta character"); return NULL; }

   {
      size_t i = 0;
      size_t avail = (size_t)(ps->e - ps->p);
      uint32_t cp = rx_u8(ps->p, avail, &i);
      ps->p += i;
      struct rx_node *n = node_new(ps->re, OP_CHAR);
      if (!n) { fail(ps, "out of memory"); return NULL; }
      node_stamp(ps, n);
      n->ch = cp;
      return n;
   }
}

/* `\Q ... \E` — everything between is literal.  Returns a chain of OP_CHAR. */
static struct rx_node *parse_quoted(struct rxp *ps, struct rx_node **tail)
{
   struct rx_node *head = NULL, *last = NULL;
   while (!at_end(ps)) {
      if (*ps->p == '\\' && ps->p + 1 < ps->e && ps->p[1] == 'E') { ps->p += 2; break; }
      size_t i = 0;
      size_t avail = (size_t)(ps->e - ps->p);
      uint32_t cp = rx_u8(ps->p, avail, &i);
      ps->p += i;
      struct rx_node *n = node_new(ps->re, OP_CHAR);
      if (!n) { fail(ps, "out of memory"); return NULL; }
      node_stamp(ps, n);
      n->ch = cp;
      if (last) last->next = n; else head = n;
      last = n;
   }
   *tail = last;
   return head;
}

static bool parse_quant(struct rxp *ps, struct rx_node *n)
{
   char c = peek(ps);
   int min, max;
   if (c == '*')      { min = 0; max = -1; ++ps->p; }
   else if (c == '+') { min = 1; max = -1; ++ps->p; }
   else if (c == '?') { min = 0; max =  1; ++ps->p; }
   else if (c == '{') {
      const char *save = ps->p;
      ++ps->p;
      if (!(peek(ps) >= '0' && peek(ps) <= '9')) { ps->p = save; return true; }
      min = 0;
      while (peek(ps) >= '0' && peek(ps) <= '9') min = min * 10 + (*ps->p++ - '0');
      if (peek(ps) == ',') {
         ++ps->p;
         if (peek(ps) == '}') max = -1;
         else {
            max = 0;
            while (peek(ps) >= '0' && peek(ps) <= '9') max = max * 10 + (*ps->p++ - '0');
         }
      } else {
         max = min;
      }
      if (peek(ps) != '}') { fail(ps, "Unclosed counted closure"); return false; }
      ++ps->p;
      if (max >= 0 && max < min) { fail(ps, "Illegal repetition range"); return false; }
   } else {
      return true;
   }
   n->min = min; n->max = max;
   if (peek(ps) == '?')      { n->quant = Q_RELUCTANT;  ++ps->p; }
   else if (peek(ps) == '+') { n->quant = Q_POSSESSIVE; ++ps->p; }
   else                        n->quant = Q_GREEDY;
   return true;
}

static struct rx_node *parse_seq(struct rxp *ps)
{
   struct rx_node *head = NULL, *last = NULL;
   for (;;) {
      skip_x(ps);
      if (at_end(ps) || *ps->p == '|' || *ps->p == ')') break;

      if (*ps->p == '\\' && ps->p + 1 < ps->e && ps->p[1] == 'Q') {
         ps->p += 2;
         struct rx_node *tail = NULL;
         struct rx_node *q = parse_quoted(ps, &tail);
         if (ps->failed) return NULL;
         if (!q) continue;
         if (last) last->next = q; else head = q;
         last = tail;
         /* A quantifier after \Q..\E applies to the last literal only. */
         if (last && !parse_quant(ps, last)) return NULL;
         continue;
      }

      struct rx_node *n = parse_atom(ps);
      if (ps->failed) return NULL;
      if (!n) continue;                       /* inline-flag group */
      if (!parse_quant(ps, n)) return NULL;
      if (last) last->next = n; else head = n;
      last = n;
   }
   return head;
}

static struct rx_node **parse_alt(struct rxp *ps, int *nalts)
{
   struct rx_node **alts = NULL;
   int n = 0, cap = 0;
   for (;;) {
      struct rx_node *seq = parse_seq(ps);
      if (ps->failed) { free(alts); return NULL; }
      if (n == cap) {
         int c2 = cap ? cap * 2 : 4;
         struct rx_node **p = realloc(alts, (size_t)c2 * sizeof *p);
         if (!p) { free(alts); fail(ps, "out of memory"); return NULL; }
         alts = p; cap = c2;
      }
      alts[n++] = seq;                        /* NULL is the empty alternative */
      if (peek(ps) != '|') break;
      ++ps->p;
   }
   if (!alts_track(ps->re, alts)) { free(alts); fail(ps, "out of memory"); return NULL; }
   *nalts = n;
   return alts;
}

struct rx *rx_compile(const char *pattern, int flags, char **err)
{
   if (err) *err = NULL;
   if (!pattern) pattern = "";

   struct rx *re = calloc(1, sizeof *re);
   if (!re) return NULL;
   re->src = rx_strdup(pattern);
   re->flags = flags;
   if (!re->src) { rx_free(re); return NULL; }

   if (flags & RX_LITERAL) {
      /* Pattern.LITERAL: the whole string is one literal run. */
      struct rx_node **alts = calloc(1, sizeof *alts);
      if (!alts || !alts_track(re, alts)) { free(alts); rx_free(re); return NULL; }
      struct rx_node *head = NULL, *last = NULL;
      size_t len = strlen(pattern), i = 0;
      while (i < len) {
         uint32_t cp = rx_u8(pattern, len, &i);
         struct rx_node *n = node_new(re, OP_CHAR);
         if (!n) { rx_free(re); return NULL; }
         n->ch = cp;
         n->icase = (flags & RX_CASE_INSENSITIVE) ? 1 : 0;
         if (last) last->next = n; else head = n;
         last = n;
      }
      alts[0] = head;
      re->alts = alts; re->nalts = 1;
      return re;
   }

   struct rxp ps = { 0 };
   ps.p = re->src;
   ps.e = re->src + strlen(re->src);
   ps.re = re;
   ps.flags = flags;

   re->alts = parse_alt(&ps, &re->nalts);
   if (!ps.failed && !at_end(&ps)) fail(&ps, "Unmatched closing ')'");
   if (ps.failed) {
      if (err) {
         size_t n = strlen(ps.err) + strlen(pattern) + 8;
         char *m = malloc(n);
         if (m) snprintf(m, n, "%s: %s", ps.err, pattern);
         *err = m;
      }
      rx_free(re);
      return NULL;
   }
   return re;
}

const char *rx_source(const struct rx *re) { return re ? re->src : NULL; }
int rx_flags(const struct rx *re)          { return re ? re->flags : 0; }
int rx_group_count(const struct rx *re)    { return re ? re->ngroups : 0; }

int rx_group_index(const struct rx *re, const char *name)
{
   if (!re || !name) return -1;
   for (int i = 0; i < re->nnames; ++i)
      if (!strcmp(re->names[i].name, name)) return re->names[i].idx;
   return -1;
}

/* ------------------------------------------------------------- the matcher */

enum { K_ACCEPT, K_NODE, K_GEND, K_REP, K_STOP, K_ENDAT };

struct rx_cont {
   int kind;
   struct rx_node *node;      /* K_NODE: what to match next; K_REP: the atom */
   int group;                 /* K_GEND */
   int count;                 /* K_REP: iterations completed */
   size_t at;                 /* K_REP: where this iteration started
                                 K_ENDAT: the position the body must reach */
   size_t *out;               /* K_STOP */
   struct rx_cont *k;
};

struct rx_ctx {
   const struct rx *re;
   const char *in;
   size_t len;
   size_t from;                       /* region start; where \G matches */
   int gs[RX_MAX_GROUPS + 1], ge[RX_MAX_GROUPS + 1];
   int ngroups;
   bool anchor_end;
   size_t end;                        /* where the accepted match ended */
   long steps;
   int depth;
};

static bool m_cont(struct rx_ctx *c, struct rx_cont *k, size_t pos);
static bool m_node(struct rx_ctx *c, struct rx_node *n, struct rx_cont *k, size_t pos);
static bool m_atom(struct rx_ctx *c, struct rx_node *n, struct rx_cont *k, size_t pos);
static bool m_rep(struct rx_ctx *c, struct rx_node *n, int count,
                  struct rx_cont *k, size_t pos);

static bool is_word(uint32_t cp)
{
   return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
          (cp >= '0' && cp <= '9') || cp == '_';
}

static bool is_term(uint32_t cp, bool unix_lines)
{
   if (unix_lines) return cp == '\n';
   return cp == '\n' || cp == '\r' || cp == 0x0085u ||
          cp == 0x2028u || cp == 0x2029u;
}

static uint32_t cp_at(const struct rx_ctx *c, size_t pos)
{
   size_t i = pos;
   return rx_u8(c->in, c->len, &i);
}

static uint32_t cp_before(const struct rx_ctx *c, size_t pos)
{
   size_t j = rx_u8_prev(c->in, pos);
   size_t i = j;
   return rx_u8(c->in, c->len, &i);
}

/* Match exactly one instance of an atom that consumes a single code point. */
static bool atom_step(struct rx_ctx *c, const struct rx_node *n, size_t pos,
                      size_t *out)
{
   if (pos >= c->len) return false;
   size_t i = pos;
   uint32_t cp = rx_u8(c->in, c->len, &i);
   switch (n->op) {
   case OP_CHAR:
      if (!rx_cp_eq(cp, n->ch, n->icase)) return false;
      break;
   case OP_ANY:
      if (!n->dotall && is_term(cp, n->unixlines)) return false;
      break;
   case OP_CLASS:
      if (!cls_test(n->cls, cp, n->icase)) return false;
      break;
   default:
      return false;
   }
   *out = i;
   return true;
}

static bool atom_is_simple(const struct rx_node *n)
{
   return n->op == OP_CHAR || n->op == OP_ANY || n->op == OP_CLASS;
}

/* A quantified single-code-point atom, matched iteratively: one stack frame
 * however long the run is, which is what keeps `.*` over a large string from
 * being a recursion as deep as the string. */
static bool m_rep_simple(struct rx_ctx *c, struct rx_node *n,
                         struct rx_cont *k, size_t pos)
{
   size_t small[64];
   size_t *stops = small;
   int cap = (int)(sizeof small / sizeof small[0]);
   int cnt = 0;
   stops[0] = pos;
   size_t cur = pos;
   while (n->max < 0 || cnt < n->max) {
      size_t nx;
      if (!atom_step(c, n, cur, &nx)) break;
      if (cnt + 2 > cap) {
         int c2 = cap * 2;
         size_t *p = stops == small ? malloc((size_t)c2 * sizeof *p)
                                    : realloc(stops, (size_t)c2 * sizeof *p);
         if (!p) break;
         if (stops == small) memcpy(p, small, (size_t)cap * sizeof *p);
         stops = p; cap = c2;
      }
      ++cnt;
      stops[cnt] = nx;
      cur = nx;
   }
   bool r = false;
   if (cnt >= n->min) {
      if (n->quant == Q_RELUCTANT) {
         for (int i = n->min; i <= cnt; ++i)
            if (m_cont(c, k, stops[i])) { r = true; break; }
      } else if (n->quant == Q_POSSESSIVE) {
         r = m_cont(c, k, stops[cnt]);
      } else {
         for (int i = cnt; i >= n->min; --i)
            if (m_cont(c, k, stops[i])) { r = true; break; }
      }
   }
   if (stops != small) free(stops);
   return r;
}

static bool m_rep(struct rx_ctx *c, struct rx_node *n, int count,
                  struct rx_cont *k, size_t pos)
{
   if (count == 0 && atom_is_simple(n)) return m_rep_simple(c, n, k, pos);

   const bool can_more = n->max < 0 || count < n->max;
   const bool can_stop = count >= n->min;
   struct rx_cont kk = { .kind = K_REP, .node = n, .count = count + 1,
                         .at = pos, .k = k };

   if (n->quant == Q_RELUCTANT) {
      if (can_stop && m_cont(c, k, pos)) return true;
      if (can_more) return m_atom(c, n, &kk, pos);
      return false;
   }
   if (n->quant == Q_POSSESSIVE) {
      /* Take as many iterations as the atom will give and never give one
       * back — the whole point of `x*+`. */
      size_t cur = pos;
      int cnt = count;
      while (n->max < 0 || cnt < n->max) {
         size_t got = cur;
         struct rx_cont stop = { .kind = K_STOP, .out = &got };
         if (!m_atom(c, n, &stop, cur)) break;
         if (got == cur) break;               /* an empty iteration repeats forever */
         cur = got;
         ++cnt;
      }
      if (cnt < n->min) return false;
      return m_cont(c, k, cur);
   }
   if (can_more && m_atom(c, n, &kk, pos)) return true;
   if (can_stop) return m_cont(c, k, pos);
   return false;
}

static bool m_cont(struct rx_ctx *c, struct rx_cont *k, size_t pos)
{
   if (++c->steps > RX_STEP_MAX) return false;
   switch (k->kind) {
   case K_ACCEPT:
      if (c->anchor_end && pos != c->len) return false;
      c->end = pos;
      return true;
   case K_NODE:
      return m_node(c, k->node, k->k, pos);
   case K_GEND: {
      const int g = k->group;
      const int save = c->ge[g];
      c->ge[g] = (int)pos;
      if (m_cont(c, k->k, pos)) return true;
      c->ge[g] = save;
      return false;
   }
   case K_REP:
      /* An iteration that consumed nothing would repeat for ever; take the
       * continuation instead, which is also what Java does with `(a?)*`. */
      if (pos == k->at) return m_cont(c, k->k, pos);
      return m_rep(c, k->node, k->count, k->k, pos);
   case K_STOP:
      *k->out = pos;
      return true;
   case K_ENDAT:
      return pos == k->at;
   default:
      return false;
   }
}

static bool m_node(struct rx_ctx *c, struct rx_node *n, struct rx_cont *k,
                   size_t pos)
{
   if (!n) return m_cont(c, k, pos);
   if (++c->depth > RX_DEPTH_MAX) { --c->depth; return false; }
   struct rx_cont kk = { .kind = K_NODE, .node = n->next, .k = k };
   bool r = (n->min == 1 && n->max == 1) ? m_atom(c, n, &kk, pos)
                                         : m_rep(c, n, 0, &kk, pos);
   --c->depth;
   return r;
}

static bool m_atom(struct rx_ctx *c, struct rx_node *n, struct rx_cont *k,
                   size_t pos)
{
   if (++c->steps > RX_STEP_MAX) return false;

   switch (n->op) {
   case OP_CHAR: case OP_ANY: case OP_CLASS: {
      size_t nx;
      if (!atom_step(c, n, pos, &nx)) return false;
      return m_cont(c, k, nx);
   }

   case OP_BOL:
      if (pos == 0) return m_cont(c, k, pos);
      if (!n->multiline) return false;
      /* After a line terminator, but not between the CR and LF of a CRLF. */
      {
         uint32_t prev = cp_before(c, pos);
         if (!is_term(prev, n->unixlines)) return false;
         if (prev == '\r' && pos < c->len && c->in[pos] == '\n') return false;
      }
      return m_cont(c, k, pos);

   case OP_EOL:
      if (n->multiline) {
         if (pos == c->len) return m_cont(c, k, pos);
         if (!is_term(cp_at(c, pos), n->unixlines)) return false;
         return m_cont(c, k, pos);
      }
      /* Fall through to \Z: end of input, or before a final terminator. */
      /* fallthrough */
   case OP_EOS_FINAL: {
      if (pos == c->len) return m_cont(c, k, pos);
      size_t i = pos;
      uint32_t cp = rx_u8(c->in, c->len, &i);
      if (!is_term(cp, n->unixlines)) return false;
      if (cp == '\r' && i < c->len && c->in[i] == '\n') ++i;
      if (i != c->len) return false;
      return m_cont(c, k, pos);
   }

   case OP_BOS:  return pos == 0 ? m_cont(c, k, pos) : false;
   case OP_EOS:  return pos == c->len ? m_cont(c, k, pos) : false;
   case OP_GPOS: return pos == c->from ? m_cont(c, k, pos) : false;

   case OP_WORDB: case OP_NWORDB: {
      bool before = pos > 0 && is_word(cp_before(c, pos));
      bool after  = pos < c->len && is_word(cp_at(c, pos));
      bool b = before != after;
      if ((n->op == OP_WORDB) != b) return false;
      return m_cont(c, k, pos);
   }

   case OP_BACKREF: {
      const int g = n->ref;
      if (g > c->ngroups || c->gs[g] < 0 || c->ge[g] < 0) return false;
      size_t bl = (size_t)(c->ge[g] - c->gs[g]);
      if (pos + bl > c->len) return false;
      if (n->icase) {
         size_t a = (size_t)c->gs[g], b = pos;
         size_t ae = (size_t)c->ge[g], be = pos + bl;
         while (a < ae && b < be) {
            uint32_t x = rx_u8(c->in, ae, &a);
            uint32_t y = rx_u8(c->in, be, &b);
            if (!rx_cp_eq(x, y, true)) return false;
         }
         if (a != ae || b != be) return false;
      } else if (memcmp(c->in + c->gs[g], c->in + pos, bl) != 0) {
         return false;
      }
      return m_cont(c, k, pos + bl);
   }

   case OP_GROUP: {
      const int g = n->group;
      const int save_s = g ? c->gs[g] : 0, save_e = g ? c->ge[g] : 0;
      struct rx_cont kg = { .kind = K_GEND, .group = g, .k = k };
      struct rx_cont *body_k = g ? &kg : k;
      if (g) c->gs[g] = (int)pos;
      for (int i = 0; i < n->nalts; ++i)
         if (m_node(c, n->alts[i], body_k, pos)) return true;
      if (g) { c->gs[g] = save_s; c->ge[g] = save_e; }
      return false;
   }

   case OP_ATOMIC: {
      size_t end = pos;
      struct rx_cont stop = { .kind = K_STOP, .out = &end };
      for (int i = 0; i < n->nalts; ++i)
         if (m_node(c, n->alts[i], &stop, pos))
            return m_cont(c, k, end);
      return false;
   }

   case OP_LOOKAHEAD: {
      int save_s[RX_MAX_GROUPS + 1], save_e[RX_MAX_GROUPS + 1];
      memcpy(save_s, c->gs, sizeof save_s);
      memcpy(save_e, c->ge, sizeof save_e);
      size_t end = pos;
      struct rx_cont stop = { .kind = K_STOP, .out = &end };
      bool hit = false;
      for (int i = 0; i < n->nalts && !hit; ++i)
         hit = m_node(c, n->alts[i], &stop, pos);
      if (n->negate) {
         memcpy(c->gs, save_s, sizeof save_s);
         memcpy(c->ge, save_e, sizeof save_e);
         if (hit) return false;
         return m_cont(c, k, pos);
      }
      if (!hit) {
         memcpy(c->gs, save_s, sizeof save_s);
         memcpy(c->ge, save_e, sizeof save_e);
         return false;
      }
      if (m_cont(c, k, pos)) return true;
      memcpy(c->gs, save_s, sizeof save_s);
      memcpy(c->ge, save_e, sizeof save_e);
      return false;
   }

   case OP_LOOKBEHIND: {
      int save_s[RX_MAX_GROUPS + 1], save_e[RX_MAX_GROUPS + 1];
      memcpy(save_s, c->gs, sizeof save_s);
      memcpy(save_e, c->ge, sizeof save_e);
      struct rx_cont endat = { .kind = K_ENDAT, .at = pos };
      bool hit = false;
      size_t s = pos;
      for (int steps = 0; steps <= RX_LOOKBEHIND_MAX; ++steps) {
         for (int i = 0; i < n->nalts && !hit; ++i)
            hit = m_node(c, n->alts[i], &endat, s);
         if (hit || s == 0) break;
         s = rx_u8_prev(c->in, s);
      }
      if (n->negate) {
         memcpy(c->gs, save_s, sizeof save_s);
         memcpy(c->ge, save_e, sizeof save_e);
         if (hit) return false;
         return m_cont(c, k, pos);
      }
      if (!hit) {
         memcpy(c->gs, save_s, sizeof save_s);
         memcpy(c->ge, save_e, sizeof save_e);
         return false;
      }
      if (m_cont(c, k, pos)) return true;
      memcpy(c->gs, save_s, sizeof save_s);
      memcpy(c->ge, save_e, sizeof save_e);
      return false;
   }

   default:
      return false;
   }
}

bool rx_search(const struct rx *re, const char *text, size_t len, size_t from,
               int *caps, int ncaps, bool anchor_start, bool anchor_end)
{
   if (!re || !text || from > len) return false;

   struct rx_ctx c;
   memset(&c, 0, sizeof c);
   c.re = re;
   c.in = text;
   c.len = len;
   c.from = from;
   c.ngroups = re->ngroups;
   c.anchor_end = anchor_end;

   struct rx_cont accept = { .kind = K_ACCEPT };

   for (size_t start = from;;) {
      for (int i = 0; i <= re->ngroups; ++i) { c.gs[i] = -1; c.ge[i] = -1; }
      c.depth = 0;
      bool hit = false;
      for (int a = 0; a < re->nalts && !hit; ++a)
         hit = m_node(&c, re->alts[a], &accept, start);
      if (hit) {
         c.gs[0] = (int)start;
         c.ge[0] = (int)c.end;
         for (int i = 0; i < ncaps; ++i) caps[i] = -1;
         for (int g = 0; g <= re->ngroups && 2 * g + 1 < ncaps; ++g) {
            caps[2 * g]     = c.gs[g];
            caps[2 * g + 1] = c.ge[g];
         }
         return true;
      }
      if (anchor_start || start >= len || c.steps > RX_STEP_MAX) return false;
      size_t nxt = start;
      (void)rx_u8(text, len, &nxt);
      start = nxt;
   }
}

/* ------------------------------------------------------- the pattern cache
 *
 * Builtin objects have no finalizer, so a Pattern compiled per call would leak
 * per call; more to the point, String.matches() inside a loop recompiled the
 * same pattern thousands of times.  A small keyed table fixes both.
 *
 * Entries are reference counted so an eviction can never free a pattern a
 * matcher is still walking: rx_cached() hands out a reference, the caller
 * gives it back with rx_cached_release(), and only an entry at zero can be
 * evicted.  A pattern the table had no room for is released by the same call —
 * release frees whatever it cannot find in the table — so callers do not have
 * to know whether their pattern was cached. */

#define RX_CACHE_MAX 128

static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static struct rx_cache_ent {
   char *pat;
   int flags;
   struct rx *re;
   unsigned long used;
   int refs;
} g_cache[RX_CACHE_MAX];
static int g_cache_n;
static unsigned long g_cache_clock;

const struct rx *rx_cached(const char *pattern, int flags, char **err)
{
   if (err) *err = NULL;
   if (!pattern) return NULL;

   pthread_mutex_lock(&g_cache_mu);
   for (int i = 0; i < g_cache_n; ++i) {
      if (g_cache[i].flags == flags && !strcmp(g_cache[i].pat, pattern)) {
         g_cache[i].used = ++g_cache_clock;
         ++g_cache[i].refs;
         struct rx *hit = g_cache[i].re;
         pthread_mutex_unlock(&g_cache_mu);
         return hit;
      }
   }
   pthread_mutex_unlock(&g_cache_mu);

   struct rx *re = rx_compile(pattern, flags, err);
   if (!re) return NULL;

   char *key = rx_strdup(pattern);
   if (!key) return re;                 /* uncached; release() will free it */

   pthread_mutex_lock(&g_cache_mu);
   /* Another thread may have compiled the same pattern meanwhile. */
   for (int i = 0; i < g_cache_n; ++i) {
      if (g_cache[i].flags == flags && !strcmp(g_cache[i].pat, pattern)) {
         g_cache[i].used = ++g_cache_clock;
         ++g_cache[i].refs;
         struct rx *hit = g_cache[i].re;
         pthread_mutex_unlock(&g_cache_mu);
         free(key);
         rx_free(re);
         return hit;
      }
   }
   int slot = -1;
   if (g_cache_n < RX_CACHE_MAX) {
      slot = g_cache_n++;
   } else {
      for (int i = 0; i < RX_CACHE_MAX; ++i) {
         if (g_cache[i].refs) continue;
         if (slot < 0 || g_cache[i].used < g_cache[slot].used) slot = i;
      }
      if (slot >= 0) { free(g_cache[slot].pat); rx_free(g_cache[slot].re); }
   }
   if (slot < 0) {                      /* every entry is in use right now */
      pthread_mutex_unlock(&g_cache_mu);
      free(key);
      return re;                        /* uncached; release() will free it */
   }
   g_cache[slot].pat = key;
   g_cache[slot].flags = flags;
   g_cache[slot].re = re;
   g_cache[slot].used = ++g_cache_clock;
   g_cache[slot].refs = 1;
   pthread_mutex_unlock(&g_cache_mu);
   return re;
}

void rx_cached_release(const struct rx *re)
{
   if (!re) return;
   pthread_mutex_lock(&g_cache_mu);
   for (int i = 0; i < g_cache_n; ++i) {
      if (g_cache[i].re == re) {
         if (g_cache[i].refs > 0) --g_cache[i].refs;
         pthread_mutex_unlock(&g_cache_mu);
         return;
      }
   }
   pthread_mutex_unlock(&g_cache_mu);
   rx_free((struct rx *)re);            /* it never made it into the table */
}
