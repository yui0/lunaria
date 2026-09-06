/* Convert text directly to IEEE-754 binary128 without using the host's long
 * double.  The decimal value is kept as N * 5^k * 2^k (or N / 5^-k * 2^k),
 * then divided at the exact binary place required by the 113-bit significand.
 * Remainders implement round-to-nearest, ties-to-even.
 *
 * This is not macOS-only code even though macOS is why it exists: AArch64's
 * `long double` is binary128 on every host, so strtold/wcstold coming out of
 * the guest need this conversion on Linux and Windows too.  Hosts whose own
 * long double is 80-bit x87 would lose the low bits; hosts where it is a
 * plain double (arm64 macOS) would lose most of the range as well.
 *
 * Plain C, and its own translation unit: nothing here touches the emulator,
 * and keeping it separate is what lets test/binary128_test build it alone.
 */
#include "binary128.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* A magnitude in base 2^32, little-endian.
 *
 * The sizes are decided by the input: 5^k for a decimal exponent near the
 * bottom of the binary128 range is about 38 000 bits, and a caller may hand
 * us a literal with any number of digits.  So the words are heap-allocated
 * and every mutation that can grow checks; an allocation failure sets `bad`,
 * which propagates to a zero result rather than a wrong one. */
typedef struct {
   uint32_t *w;
   size_t    n;     /* words in use */
   size_t    cap;
   bool      bad;   /* out of memory somewhere along the way */
} big;

static void big_free(big *b) { free(b->w); b->w = NULL; b->n = b->cap = 0; }

static bool big_reserve(big *b, size_t need)
{
   if (need <= b->cap) return true;
   size_t cap = b->cap ? b->cap : 8u;
   while (cap < need) cap *= 2u;
   uint32_t *p = (uint32_t *)realloc(b->w, cap * sizeof *p);
   if (!p) { b->bad = true; return false; }
   memset(p + b->cap, 0, (cap - b->cap) * sizeof *p);
   b->w = p;
   b->cap = cap;
   return true;
}

static bool big_resize(big *b, size_t n)
{
   if (!big_reserve(b, n)) return false;
   if (n > b->n) memset(b->w + b->n, 0, (n - b->n) * sizeof *b->w);
   b->n = n;
   return true;
}

static bool big_push(big *b, uint32_t v)
{
   if (!big_reserve(b, b->n + 1u)) return false;
   b->w[b->n++] = v;
   return true;
}

static void big_init(big *b, uint32_t v)
{
   b->w = NULL; b->n = b->cap = 0; b->bad = false;
   if (v) (void)big_push(b, v);
}

static bool big_copy(big *dst, const big *src)
{
   big_init(dst, 0);
   dst->bad = src->bad;
   if (!src->n) return true;
   if (!big_resize(dst, src->n)) return false;
   memcpy(dst->w, src->w, src->n * sizeof *src->w);
   return true;
}

static bool big_zero(const big *b) { return b->n == 0; }

static void big_trim(big *b) { while (b->n && b->w[b->n - 1] == 0) --b->n; }

static int big_bits(const big *b)
{
   if (!b->n) return 0;
   return (int)((b->n - 1) * 32 + 32 - __builtin_clz(b->w[b->n - 1]));
}

static void big_mul_small(big *b, uint32_t m)
{
   uint64_t carry = 0;
   for (size_t i = 0; i < b->n; ++i) {
      uint64_t p = (uint64_t)b->w[i] * m + carry;
      b->w[i] = (uint32_t)p;
      carry = p >> 32;
   }
   if (carry) (void)big_push(b, (uint32_t)carry);
}

static void big_add_small(big *b, uint32_t a)
{
   uint64_t carry = a;
   for (size_t i = 0; carry && i < b->n; ++i) {
      uint64_t s = (uint64_t)b->w[i] + carry;
      b->w[i] = (uint32_t)s;
      carry = s >> 32;
   }
   if (carry) (void)big_push(b, (uint32_t)carry);
}

static void big_shl(big *b, int n)
{
   if (big_zero(b) || n <= 0) return;
   const size_t words = (size_t)n / 32u;
   const int bits = n % 32;
   if (words) {
      if (!big_resize(b, b->n + words)) return;
      memmove(b->w + words, b->w, (b->n - words) * sizeof *b->w);
      memset(b->w, 0, words * sizeof *b->w);
   }
   if (!bits) return;
   uint64_t carry = 0;
   for (size_t i = words; i < b->n; ++i) {
      uint64_t v = ((uint64_t)b->w[i] << bits) | carry;
      b->w[i] = (uint32_t)v;
      carry = v >> 32;
   }
   if (carry) (void)big_push(b, (uint32_t)carry);
}

static void big_shr1(big *b)
{
   uint32_t carry = 0;
   for (size_t i = b->n; i-- > 0;) {
      uint32_t next = b->w[i] & 1u;
      b->w[i] = (b->w[i] >> 1) | (carry << 31);
      carry = next;
   }
   big_trim(b);
}

static int big_cmp(const big *a, const big *b)
{
   if (a->n != b->n) return a->n < b->n ? -1 : 1;
   for (size_t i = a->n; i-- > 0;)
      if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
   return 0;
}

/* precondition: *a >= *b */
static void big_sub(big *a, const big *b)
{
   uint64_t borrow = 0;
   for (size_t i = 0; i < a->n; ++i) {
      uint64_t bv = (uint64_t)(i < b->n ? b->w[i] : 0u) + borrow;
      uint64_t av = a->w[i];
      a->w[i] = (uint32_t)(av - bv);
      borrow = av < bv;
   }
   big_trim(a);
}

static void big_set_bit(big *b, int bit)
{
   const size_t i = (size_t)bit / 32u;
   if (b->n <= i && !big_resize(b, i + 1u)) return;
   b->w[i] |= 1u << (bit % 32);
}

static bool big_odd(const big *b) { return b->n && (b->w[0] & 1u); }

static uint64_t big_word64(const big *b, size_t i)
{
   uint64_t lo = i < b->n ? b->w[i] : 0u;
   uint64_t hi = i + 1u < b->n ? b->w[i + 1u] : 0u;
   return lo | (hi << 32);
}

static void big_pow5(big *out, int n)
{
   big_init(out, 1);
   for (int i = 0; i < n; ++i) big_mul_small(out, 5);
}

/* q = rem / den, rem = rem % den.  `rem` is consumed. */
static void big_divide(big *q, big *rem, const big *den)
{
   big_init(q, 0);
   if (big_zero(den) || big_cmp(rem, den) < 0) return;
   const int shift = big_bits(rem) - big_bits(den);
   big d;
   if (!big_copy(&d, den)) { q->bad = true; return; }
   big_shl(&d, shift);
   for (int i = shift; i >= 0; --i) {
      if (big_cmp(rem, &d) >= 0) { big_sub(rem, &d); big_set_bit(q, i); }
      big_shr1(&d);
   }
   q->bad |= d.bad;
   big_free(&d);
}

static int cmp_scaled(const big *num, const big *den, int shift)
{
   big t;
   int r;
   if (shift >= 0) {
      if (!big_copy(&t, num)) return 0;
      big_shl(&t, shift);
      r = big_cmp(&t, den);
   } else {
      if (!big_copy(&t, den)) return 0;
      big_shl(&t, -shift);
      r = big_cmp(num, &t);
   }
   big_free(&t);
   return r;
}

/* round-to-nearest, ties-to-even quotient of (num << shift) / den.
 * Both inputs are consumed. */
static void rounded_ratio(big *out, big *num, big *den, int shift)
{
   if (shift >= 0) big_shl(num, shift); else big_shl(den, -shift);
   big rem;
   if (!big_copy(&rem, num)) { big_init(out, 0); out->bad = true; return; }
   big_divide(out, &rem, den);
   big_shl(&rem, 1);
   const int half = big_cmp(&rem, den);
   if (half > 0 || (half == 0 && big_odd(out))) big_add_small(out, 1);
   out->bad |= rem.bad;
   big_free(&rem);
}

/* Assemble the value num/den * 2^two_exp.  Both inputs are consumed. */
static luna_binary128 pack(bool neg, big *num, big *den, int two_exp)
{
   luna_binary128 out;
   out.lo = 0;
   out.hi = neg ? 0x8000000000000000ull : 0ull;
   if (big_zero(num) || num->bad || den->bad) return out;

   int e = big_bits(num) - big_bits(den) + two_exp;
   while (cmp_scaled(num, den, two_exp - e) < 0) --e;
   while (cmp_scaled(num, den, two_exp - (e + 1)) >= 0) ++e;
   if (e > 16383) { out.hi |= 0x7fff000000000000ull; return out; }

   big q;
   int exp_field = 0;
   if (e < -16382) {
      big n2, d2;
      if (!big_copy(&n2, num) || !big_copy(&d2, den)) {
         big_free(&n2); big_free(&d2); return out;
      }
      rounded_ratio(&q, &n2, &d2, two_exp + 16494);
      big_free(&n2); big_free(&d2);
      if (big_zero(&q)) { big_free(&q); return out; }
      if (big_bits(&q) > 112) exp_field = 1;  /* rounded to minimum normal */
   } else {
      big n2, d2;
      if (!big_copy(&n2, num) || !big_copy(&d2, den)) {
         big_free(&n2); big_free(&d2); return out;
      }
      rounded_ratio(&q, &n2, &d2, two_exp + 112 - e);
      big_free(&n2); big_free(&d2);
      if (big_bits(&q) > 113) { big_shr1(&q); ++e; }
      if (e > 16383) { big_free(&q); out.hi |= 0x7fff000000000000ull; return out; }
      exp_field = e + 16383;
   }

   out.lo = big_word64(&q, 0);
   /* q's implicit bit is bit 112 and is deliberately discarded here. */
   out.hi |= ((uint64_t)exp_field << 48) |
             (big_word64(&q, 2) & 0x0000ffffffffffffull);
   big_free(&q);
   return out;
}

static bool ci_prefix(const char *p, const char *word)
{
   while (*word) {
      if (tolower((unsigned char)*p++) != *word++) return false;
   }
   return true;
}

static int parse_exp(const char **pp)
{
   const char *p = *pp;
   bool neg = false;
   if (*p == '+' || *p == '-') { neg = *p == '-'; ++p; }
   int v = 0;
   while (isdigit((unsigned char)*p)) {
      if (v < 100000) v = v * 10 + (*p - '0');
      ++p;
   }
   *pp = p;
   return neg ? -v : v;
}

static luna_binary128 make(bool neg, uint64_t hi_bits)
{
   luna_binary128 v;
   v.lo = 0;
   v.hi = (neg ? 0x8000000000000000ull : 0ull) | hi_bits;
   return v;
}

static luna_binary128 parse(const char *s, char **end)
{
   const char *original = s;
   while (isspace((unsigned char)*s)) ++s;
   bool neg = false;
   if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; }

   if (ci_prefix(s, "inf")) {
      s += 3;
      if (ci_prefix(s, "inity")) s += 5;
      if (end) *end = (char *)s;
      return make(neg, 0x7fff000000000000ull);
   }
   if (ci_prefix(s, "nan")) {
      s += 3;
      if (*s == '(') {
         const char *q = s + 1;
         while (isalnum((unsigned char)*q) || *q == '_') ++q;
         if (*q == ')') s = q + 1;
      }
      if (end) *end = (char *)s;
      return make(neg, 0x7fff800000000000ull);
   }

   big n;
   big_init(&n, 0);
   int frac = 0;
   bool any = false;
   const bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
   if (hex) s += 2;
   const uint32_t radix = hex ? 16u : 10u;
   bool dot = false;
   for (;;) {
      int d = -1;
      const unsigned char c = (unsigned char)*s;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
      if (d >= 0 && d < (int)radix) {
         big_mul_small(&n, radix);
         big_add_small(&n, (uint32_t)d);
         any = true;
         if (dot) ++frac;
         ++s;
         continue;
      }
      if (!dot && *s == '.') { dot = true; ++s; continue; }
      break;
   }
   if (!any) {
      big_free(&n);
      if (end) *end = (char *)original;
      return make(false, 0);
   }

   int exponent = 0;
   if ((hex && (*s == 'p' || *s == 'P')) || (!hex && (*s == 'e' || *s == 'E'))) {
      const char *mark = s, *q = s + 1;
      if (*q == '+' || *q == '-') ++q;
      if (isdigit((unsigned char)*q)) { ++s; exponent = parse_exp(&s); }
      else s = mark;
   }
   if (end) *end = (char *)s;

   luna_binary128 result;
   if (hex) {
      big one;
      big_init(&one, 1);
      result = pack(neg, &n, &one, exponent - 4 * frac);
      big_free(&one);
      big_free(&n);
      return result;
   }

   const int dec_exp = exponent - frac;
   /* Do not build 5^100000 merely to discover an obvious infinity or zero.
    * 3.321 < log2(10), so the positive test is a lower bound and the
    * negative test is an upper bound; neither can clamp a finite binary128. */
   const int64_t nbits = big_bits(&n);
   if (dec_exp >= 0 &&
       (nbits - 1) * 1000 + (int64_t)dec_exp * 3321 > 16384LL * 1000) {
      big_free(&n);
      return make(neg, 0x7fff000000000000ull);
   }
   if (dec_exp < 0 &&
       nbits * 1000 + (int64_t)dec_exp * 3321 < -16495LL * 1000) {
      big_free(&n);
      return make(neg, 0);
   }
   if (dec_exp >= 0) {
      big f, product, one;
      big_pow5(&f, dec_exp);
      big_init(&product, 0);
      big_init(&one, 1);
      if (big_resize(&product, n.n + f.n)) {
         /* multiply by 5^k using schoolbook multiplication. */
         for (size_t i = 0; i < n.n; ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < f.n; ++j) {
               uint64_t cur = product.w[i + j] + (uint64_t)n.w[i] * f.w[j] + carry;
               product.w[i + j] = (uint32_t)cur;
               carry = cur >> 32;
            }
            size_t k = i + f.n;
            while (carry) {
               uint64_t cur = product.w[k] + carry;
               product.w[k++] = (uint32_t)cur;
               carry = cur >> 32;
            }
         }
      }
      big_trim(&product);
      product.bad |= n.bad | f.bad;
      result = pack(neg, &product, &one, dec_exp);
      big_free(&f);
      big_free(&product);
      big_free(&one);
      big_free(&n);
      return result;
   }

   big den;
   big_pow5(&den, -dec_exp);
   result = pack(neg, &n, &den, dec_exp);
   big_free(&den);
   big_free(&n);
   return result;
}

luna_binary128 luna_binary128_from_string(const char *s, char **end)
{
   if (!s) { if (end) *end = NULL; return make(false, 0); }
   return parse(s, end);
}

luna_binary128 luna_binary128_from_wstring(const wchar_t *s, wchar_t **end)
{
   if (!s) { if (end) *end = NULL; return make(false, 0); }
   /* Only the ASCII prefix can spell a number, and the caller's end pointer
    * has to land in *its* string, so the offset is what carries across. */
   size_t n = 0;
   while (s[n] && (unsigned long)s[n] < 128ul) ++n;
   char *ascii = (char *)malloc(n + 1u);
   if (!ascii) { if (end) *end = (wchar_t *)s; return make(false, 0); }
   for (size_t i = 0; i < n; ++i) ascii[i] = (char)s[i];
   ascii[n] = '\0';
   char *ae = NULL;
   const luna_binary128 v = luna_binary128_from_string(ascii, &ae);
   if (end) *end = (wchar_t *)(s + (ae ? (size_t)(ae - ascii) : 0u));
   free(ascii);
   return v;
}
