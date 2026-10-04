/* MPL-2.0. Unicode normalization (UAX #15) and IDNA2003 (RFC 3490-3492).
 * Unicode tables are compiled into this file; no external Unicode library. */
#include "luna_unicode.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include "unicode/character_data.h"
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
static const struct uc_range *property(int32_t cp)
{
   size_t lo = 0, hi = COUNT(uc_ranges);
   if (cp < 0 || cp > 0x10ffff)
      return NULL;
   if (cp < 128) return uc_ascii + cp;
   while (lo < hi) {
      size_t m = lo + (hi - lo) / 2;
      if (uc_ranges[m].last < (uint32_t)cp)
         lo = m + 1;
      else
         hi = m;
   }
   return lo < COUNT(uc_ranges) && uc_ranges[lo].first <= (uint32_t)cp ? uc_ranges + lo : NULL;
}
int luna_uc_type(int32_t cp)
{
   const struct uc_range *r = property(cp);
   return r ? r->category : 0;
}
static int combining(uint32_t cp)
{
   const struct uc_range *r = property((int32_t)cp);
   return r ? r->ccc : 0;
}
static int32_t change_case(int32_t cp, bool upper)
{
   if (cp >= 0 && cp < 128)
      return upper ? (cp >= 'a' && cp <= 'z' ? cp - 32 : cp) : (cp >= 'A' && cp <= 'Z' ? cp + 32 : cp);
   size_t lo = 0, hi = COUNT(uc_cases);
   while (lo < hi) {
      size_t m = lo + (hi - lo) / 2;
      if (uc_cases[m].cp < (uint32_t)cp)
         lo = m + 1;
      else
         hi = m;
   }
   return lo < COUNT(uc_cases) && uc_cases[lo].cp == (uint32_t)cp ? (int32_t)(upper ? uc_cases[lo].upper : uc_cases[lo].lower) : cp;
}
int32_t luna_uc_upper(int32_t cp)
{
   return change_case(cp, true);
}
int32_t luna_uc_lower(int32_t cp)
{
   return change_case(cp, false);
}
bool luna_uc_property(int32_t cp, int p)
{
   const struct uc_range *r = property(cp);
   return p == LUNA_UC_ALNUM ? (luna_uc_property(cp, LUNA_UC_ALPHABETIC) || luna_uc_digit(cp)) : r && p >= 0 && p < 4 && (r->flags & (1u << p));
}
bool luna_uc_uppercase(int32_t cp)
{
   return luna_uc_type(cp) == 1;
}
bool luna_uc_lowercase(int32_t cp)
{
   return luna_uc_type(cp) == 2;
}
bool luna_uc_digit(int32_t cp)
{
   return luna_uc_type(cp) == 9;
}
bool luna_uc_alpha(int32_t cp)
{
   int t = luna_uc_type(cp);
   return t >= 1 && t <= 5;
}
bool luna_uc_alnum(int32_t cp)
{
   return luna_uc_alpha(cp) || luna_uc_digit(cp);
}
bool luna_uc_defined(int32_t cp)
{
   return luna_uc_type(cp) != 0;
}
bool luna_uc_spacechar(int32_t cp)
{
   int t = luna_uc_type(cp);
   return t >= 12 && t <= 14;
}
bool luna_uc_space(int32_t cp)
{
   return luna_uc_spacechar(cp) || (cp >= 9 && cp <= 13) || cp == 0x85;
}
bool luna_uc_whitespace(int32_t cp)
{
   return (luna_uc_spacechar(cp) && cp != 0xa0 && cp != 0x2007 && cp != 0x202f) || (cp >= 9 && cp <= 13) || (cp >= 0x1c && cp <= 0x1f);
}
bool luna_uc_blank(int32_t cp)
{
   return cp == 9 || luna_uc_type(cp) == 12;
}
bool luna_uc_control(int32_t cp)
{
   int t = luna_uc_type(cp);
   return t == 15 || t == 16 || t == 13 || t == 14;
}
bool luna_uc_graph(int32_t cp)
{
   int t = luna_uc_type(cp);
   return t && !(t >= 12 && t <= 16) && t != 18;
}
bool luna_uc_print(int32_t cp)
{
   int t = luna_uc_type(cp);
   return t && t != 15 && t != 16 && t != 18;
}
bool luna_uc_punct(int32_t cp)
{
   int t = luna_uc_type(cp);
   return (t >= 19 && t <= 23) || t == 28 || t == 29;
}
bool luna_uc_xdigit(int32_t cp)
{
   return luna_uc_digit(cp) || (cp >= 'a' && cp <= 'f') || (cp >= 'A' && cp <= 'F')||(cp>=0xff21 && cp <= 0xff26) || (cp >= 0xff41 && cp <= 0xff46);
}
struct points {
   uint32_t *p;
   size_t n, cap;
};
static bool push(struct points *v, uint32_t cp)
{
   if (v->n == v->cap) {
      size_t cap = v->cap ? v->cap * 2 : 32;
      if (cap < v->cap || cap > SIZE_MAX / sizeof(*v->p))
         return false;
      void *p = realloc(v->p, cap * sizeof(*v->p));
      if (!p)
         return false;
      v->p = p;
      v->cap = cap;
   }
   v->p[v->n++] = cp;
   return true;
}
static bool decode(const char *s, struct points *v)
{
   const unsigned char *p = (const unsigned char *)s;
   while (*p) {
      uint32_t cp = *p++;
      unsigned n = 0;
      uint32_t min = 0;
      if (cp >= 0xc2 && cp <= 0xdf) {
         cp &= 31;
         n = 1;
         min = 0x80;
      } else if (cp >= 0xe0 && cp <= 0xef) {
         cp &= 15;
         n = 2;
         min = 0x800;
      } else if (cp >= 0xf0 && cp <= 0xf4) {
         cp &= 7;
         n = 3;
         min = 0x10000;
      } else if (cp >= 0x80)
         return false;
      for (unsigned k = 0; k < n; k++) {
         if ((*p & 0xc0) != 0x80)
            return false;
         cp = (cp << 6) | (*p++ & 63);
      }
      /* WTF-8 lone surrogates are preserved for Java strings. */
      if (cp < min || cp > 0x10ffff || !push(v, cp))
         return false;
   }
   return true;
}
static char *encode(const struct points *v)
{
   if (v->n > (SIZE_MAX - 1) / 4)
      return NULL;
   char *s = malloc(v->n * 4 + 1);
   if (!s)
      return NULL;
   size_t w = 0;
   for (size_t i = 0; i < v->n; i++) {
      uint32_t cp = v->p[i];
      if (cp < 0x80)
         s[w++] = (char)cp;
      else if (cp < 0x800) {
         s[w++] = (char)(0xc0 | (cp >> 6));
         s[w++] = (char)(0x80 | (cp & 63));
      } else if (cp < 0x10000) {
         s[w++] = (char)(0xe0 | (cp >> 12));
         s[w++] = (char)(0x80 | ((cp >> 6) & 63));
         s[w++] = (char)(0x80 | (cp & 63));
      } else {
         s[w++] = (char)(0xf0 | (cp >> 18));
         s[w++] = (char)(0x80 | ((cp >> 12) & 63));
         s[w++] = (char)(0x80 | ((cp >> 6) & 63));
         s[w++] = (char)(0x80 | (cp & 63));
      }
   }
   s[w] = 0;
   return s;
}
static unsigned prep_flags(uint32_t cp);
static bool decompose(struct points *v, uint32_t cp, bool compat, bool legacy)
{
   if (legacy) {
      if (prep_flags(cp) & 1)
         return push(v, cp);
      static const struct {
         uint32_t cp, old;
      } corrections[] = {
         {0x2f868, 0x2136a}, {0x2f874, 0x5f33}, {0x2f91f, 0x43ab},
         {0x2f95f, 0x7aae}, {0x2f9bf, 0x4d57},
      };
      for (size_t i = 0; i < COUNT(corrections); i++)
         if (cp == corrections[i].cp)
            return push(v, corrections[i].old);
   }
   if (cp >= 0xac00 && cp < 0xd7a4) {
      uint32_t s = cp - 0xac00;
      return push(v, 0x1100+s / 588) && push(v, 0x1161+(s % 588) / 28) && (!(s % 28) || push(v, 0x11a7+s % 28));
   }
   size_t lo = 0, hi = COUNT(uc_decomps);
   while (lo < hi) {
      size_t m = lo + (hi - lo) / 2;
      if (uc_decomps[m].cp < cp)
         lo = m + 1;
      else
         hi = m;
   }
   if (lo < COUNT(uc_decomps) && uc_decomps[lo].cp == cp && (compat || !uc_decomps[lo].compat)) {
      const struct uc_decomp *d = uc_decomps + lo;
      for (unsigned i = 0; i < d->length; i++)
         if (!decompose(v, uc_decomp_data[d->offset + i], compat, legacy))
            return false;
      return true;
   }
   return push(v, cp);
}
int32_t luna_uc_compose(int32_t a, int32_t b)
{
   if (a >= 0x1100 && a < 0x1113 && b >= 0x1161 && b < 0x1176)
      return 0xac00+(a - 0x1100) * 588 + (b - 0x1161) * 28;
   if (a >= 0xac00 && a < 0xd7a4 && (a - 0xac00) % 28 == 0 && b > 0x11a7 && b < 0x11c3)
      return a + b - 0x11a7;
   size_t lo = 0, hi = COUNT(uc_pairs);
   while (lo < hi) {
      size_t m = lo + (hi - lo) / 2;
      const struct uc_pair *p = uc_pairs + m;
      if (p->first < (uint32_t)a || (p->first == (uint32_t)a && p->second < (uint32_t)b))
         lo = m + 1;
      else
         hi = m;
   }
   return lo < COUNT(uc_pairs) && uc_pairs[lo].first == (uint32_t)a && uc_pairs[lo].second == (uint32_t)b ? (int32_t)uc_pairs[lo].composed : -1;
}
static bool normalize_points(struct points *v, int form, bool legacy)
{
   struct points d = {0};
   for (size_t i = 0; i < v->n; i++)
      if (!decompose(&d, v->p[i], form >= 2, legacy)) {
         free(d.p);
         return false;
      }
   for (size_t i = 1; i < d.n; i++) {
      int c = combining(d.p[i]);
      if (!c)
         continue;
      uint32_t cp = d.p[i];
      size_t j = i;
      while (j && combining(d.p[j - 1]) > c) {
         d.p[j] = d.p[j - 1];
         j--;
      }
      d.p[j] = cp;
   }
   if (form == 1 || form == 3) {
      size_t w = 0, start = 0;
      int last = 0;
      for (size_t i = 0; i < d.n; i++) {
         uint32_t cp = d.p[i];
         int c = combining(cp);
         int32_t composed = w && (!last || last < c) ? luna_uc_compose((int32_t)d.p[start], (int32_t)cp) : -1;
         if (composed >= 0)
            d.p[start] = (uint32_t)composed;
         else {
            if (!c)
               start = w;
            d.p[w++] = cp;
            last = c;
         }
      }
      d.n = w;
   }
   free(v->p);
   *v = d;
   return true;
}
char *luna_unicode_normalize(const char *s, int form)
{
   if (s && form >= 0 && form <= 3) {
      const unsigned char *p = (const unsigned char *)s;
      while (*p && *p < 128) p++;
      if (!*p) return strdup(s);
   }
   struct points v = {0};
   char *r = NULL;
   if (s && form >= 0 && form <= 3 && decode(s, &v) && normalize_points(&v, form, false))
      r = encode(&v);
   free(v.p);
   return r;
}
static unsigned prep_flags(uint32_t cp)
{
   size_t lo = 0, hi = COUNT(prep_ranges);
   while (lo < hi) {
      size_t m = lo + (hi - lo) / 2;
      if (prep_ranges[m].last < cp)
         lo = m + 1;
      else
         hi = m;
   }
   return lo < COUNT(prep_ranges) && prep_ranges[lo].first <= cp ? prep_ranges[lo].flags : 0;
}
static bool nameprep(struct points *v, unsigned flags)
{
   struct points d = {0};
   for (size_t i = 0; i < v->n; i++) {
      uint32_t cp = v->p[i];
      size_t lo = 0, hi = COUNT(prep_maps);
      while (lo < hi) {
         size_t m = lo + (hi - lo) / 2;
         if (prep_maps[m].cp < cp)
            lo = m + 1;
         else
            hi = m;
      }
      if (lo < COUNT(prep_maps) && prep_maps[lo].cp == cp) {
         const struct prep_map *m = prep_maps + lo;
         for (unsigned k = 0; k < m->length; k++)
            if (!push(&d, prep_data[m->offset + k]))
               goto fail;
      } else if (!push(&d, cp))
         goto fail;
   }
   if (!normalize_points(&d, 3, true) || !d.n)
      goto fail;
   bool randal = false, left = false;
   for (size_t i = 0; i < d.n; i++) {
      unsigned f = prep_flags(d.p[i]);
      if ((f & 2) || ((f & 1) && !(flags & 1)))
         goto fail;
      randal |= !!(f & 4);
      left |= !!(f & 8);
   }
   if (randal && (left || !(prep_flags(d.p[0]) & 4) || !(prep_flags(d.p[d.n - 1]) & 4)))
      goto fail;
   free(v->p);
   *v = d;
   return true;
fail:
   free(d.p);
   return false;
}
static unsigned adapt(uint64_t delta, unsigned count, bool first)
{
   delta = first ? delta / 700 : delta / 2;
   delta += delta / count;
   unsigned k = 0;
   while (delta > 455) {
      delta /= 35;
      k += 36;
   }
   return k + (unsigned)(36 * delta / (delta + 38));
}
static char digit(unsigned d)
{
   return (char)(d < 26 ? 'a' + d : '0' + d - 26);
}
static int undigit(unsigned c)
{
   return c >= 'a' && c <= 'z' ? (int)(c - 'a') : c >= 'A' && c <= 'Z' ? (int)(c - 'A') : c >= '0' && c <= '9' ? (int)(c - '0' + 26) : -1;
}
static char *puny_encode(const struct points *v)
{
   char out[64];
   size_t w = 4;
   memcpy(out, "xn--", 4);
   unsigned basic = 0;
   for (size_t i = 0; i < v->n; i++)
      if (v->p[i] < 128) {
         if (w == 63)
            return NULL;
         out[w++] = (char)v->p[i];
         basic++;
      }
   unsigned h = basic, bias = 72;
   uint64_t n = 128, delta = 0;
   if (basic) {
      if (w == 63)
         return NULL;
      out[w++] = '-';
   }
   while (h < v->n) {
      uint64_t m = UINT32_MAX;
      for (size_t i = 0; i < v->n; i++)
         if (v->p[i] >= n && v->p[i] < m)
            m = v->p[i];
      delta += (m - n) * (h + 1);
      if (delta > UINT32_MAX)
         return NULL;
      n = m;
      for (size_t i = 0; i < v->n; i++) {
         if (v->p[i] < n && ++delta > UINT32_MAX)
            return NULL;
         if (v->p[i] == n) {
            uint64_t q = delta;
            for (unsigned k = 36;; k += 36) {
               unsigned t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
               if (q < t)
                  break;
               if (w == 63)
                  return NULL;
               out[w++] = digit(t + (unsigned)((q - t) % (36 - t)));
               q = (q - t) / (36 - t);
            }
            if (w == 63)
               return NULL;
            out[w++] = digit((unsigned)q);
            bias = adapt(delta, h + 1, h == basic);
            delta = 0;
            h++;
         }
      }
      delta++;
      n++;
   }
   out[w] = 0;
   return strdup(out);
}
static bool puny_decode(const char *s, struct points *v)
{
   size_t len = strlen(s), start = 0;
   const char *dash = strrchr(s, '-');
   if (dash) {
      start = (size_t)(dash - s) +1;
      for (size_t k = 0; k < start - 1; k++)
         if ((unsigned char)s[k] >= 128 || !push(v, (unsigned char)s[k]))
            return false;
   }
   uint64_t n = 128, i = 0;
   unsigned bias = 72;
   while (start < len) {
      uint64_t old = i, weight = 1;
      for (unsigned k = 36;; k += 36) {
         if (start == len)
            return false;
         int d = undigit((unsigned char)s[start++]);
         if (d < 0 || weight > (UINT32_MAX - i) / (unsigned)(d ? d : 1))
            return false;
         i += (unsigned)d * weight;
         unsigned t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
         if ((unsigned)d < t)
            break;
         if (weight > UINT32_MAX / (36 - t))
            return false;
         weight *= 36 - t;
      }
      bias = adapt(i - old, (unsigned)v->n + 1, old == 0);
      n += i / (v->n + 1);
      i %= v->n + 1;
      if (n > 0x10ffff || (n >= 0xd800 && n <= 0xdfff) || !push(v, 0))
         return false;
      memmove(v->p + i + 1, v->p + i, (v->n - (size_t)i - 1)*sizeof(*v->p));
      v->p[i] = (uint32_t)n;
      i++;
   }
   return true;
}
static char *label_ascii(const char *label, unsigned flags)
{
   struct points v = {0};
   char *r = NULL;
   bool nonascii = false;
   if (!decode(label, &v) || !v.n)
      goto done;
   for (size_t i = 0; i < v.n; i++)
      nonascii |= v.p[i] >= 128;
   if (nonascii && !nameprep(&v, flags))
      goto done;
   nonascii = false;
   for (size_t i = 0; i < v.n; i++) {
      uint32_t c = v.p[i];
      nonascii |= c >= 128;
      if ((flags & 2) && c < 128 && !((c >= 'a' && c <= 'z')||(c>='A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
         goto done;
   }
   if ((flags & 2) && (v.p[0] == '-' || v.p[v.n - 1] == '-'))
      goto done;
   if (nonascii) {
      if (v.n >= 4 && v.p[0] == 'x' && v.p[1] == 'n' && v.p[2] == '-' && v.p[3] == '-')
         goto done;
      r = puny_encode(&v);
   } else
      r = encode(&v);
   if (r && (strlen(r) < 1 || strlen(r) > 63)) {
      free(r);
      r = NULL;
   }
done:
   free(v.p);
   return r;
}
char *luna_idn_convert(const char *input, unsigned flags, bool ascii)
{
   if (!input || flags & ~3u)
      return NULL;
   if (!*input)
      return strdup("");
   if (!strcmp(input, ".") || !strcmp(input, "\xe3\x80\x82") || !strcmp(input, "\xef\xbc\x8e") || !strcmp(input, "\xef\xbd\xa1"))
      return strdup(".");
   struct points all = {0};
   if (!decode(input, &all)) {
      free(all.p);
      return NULL;
   }
   for (size_t i = 0; i < all.n; i++)
      if (all.p[i] == 0x3002 || all.p[i] == 0xff0e || all.p[i] == 0xff61)
         all.p[i] = '.';
   char *s = encode(&all);
   free(all.p);
   if (!s)
      return NULL;
   size_t length = strlen(s);
   if (length > (SIZE_MAX - 256) / 4) { free(s); return NULL; }
   size_t cap = length * 4 + 256, w = 0;
   char *out = malloc(cap);
   if (!out) {
      free(s);
      return NULL;
   }
   char *p = s;
   for (;;) {
      char *dot = strchr(p, '.');
      if (dot)
         * dot = 0;
      char *label = NULL;
      if (ascii)
         label = label_ascii(p, flags);
      else if (strlen(p) > 4 && !strncasecmp(p, "xn--", 4)) {
         struct points v = {0};
         if (puny_decode(p + 4, &v)) {
            char *decoded = encode(&v);
            char *check = decoded ? label_ascii(decoded, flags) : NULL;
            if (check && !strcasecmp(check, p))
               label = decoded;
            else
               free(decoded);
            free(check);
         }
         free(v.p);
         if (!label)
            label = strdup(p);
      } else
         label = strdup(p);
      if (!label) {
         free(s);
         free(out);
         return NULL;
      }
      size_t n = strlen(label);
      if (w + n + 2 > cap) {
         size_t next = w + n + 256;
         char *grown = realloc(out, next);
         if (!grown) {
            free(label);
            free(s);
            free(out);
            return NULL;
         }
         out = grown;
         cap = next;
      }
      memcpy(out + w, label, n);
      w += n;
      free(label);
      if (!dot)
         break;
      out[w++] = '.';
      p = dot + 1;
      if (!*p)
         break;
   }
   out[w] = 0;
   free(s);
   return out;
}
