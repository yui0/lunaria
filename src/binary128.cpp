/* Convert text directly to IEEE-754 binary128 without using the host's long
 * double.  The decimal value is kept as N * 5^k * 2^k (or N / 5^-k * 2^k),
 * then divided at the exact binary place required by the 113-bit significand.
 * Remainders implement round-to-nearest, ties-to-even. */
#include "binary128.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

class BigUInt {
public:
   std::vector<uint32_t> w;              // little-endian, base 2^32

   explicit BigUInt(uint32_t v = 0) { if (v) w.push_back(v); }
   bool zero() const { return w.empty(); }
   void trim() { while (!w.empty() && w.back() == 0) w.pop_back(); }
   int bits() const {
      if (w.empty()) return 0;
      return (int)((w.size() - 1) * 32 + 32 - __builtin_clz(w.back()));
   }
   void mul_small(uint32_t m) {
      uint64_t carry = 0;
      for (uint32_t &v : w) {
         uint64_t p = (uint64_t)v * m + carry;
         v = (uint32_t)p; carry = p >> 32;
      }
      if (carry) w.push_back((uint32_t)carry);
   }
   void add_small(uint32_t a) {
      uint64_t carry = a;
      for (size_t i = 0; carry && i < w.size(); ++i) {
         uint64_t s = (uint64_t)w[i] + carry;
         w[i] = (uint32_t)s; carry = s >> 32;
      }
      if (carry) w.push_back((uint32_t)carry);
   }
   void shl(int n) {
      if (zero() || n <= 0) return;
      int words = n / 32, bits = n % 32;
      w.insert(w.begin(), (size_t)words, 0);
      if (!bits) return;
      uint64_t carry = 0;
      for (size_t i = (size_t)words; i < w.size(); ++i) {
         uint64_t v = ((uint64_t)w[i] << bits) | carry;
         w[i] = (uint32_t)v; carry = v >> 32;
      }
      if (carry) w.push_back((uint32_t)carry);
   }
   void shr1() {
      uint32_t carry = 0;
      for (size_t i = w.size(); i-- > 0;) {
         uint32_t next = w[i] & 1u;
         w[i] = (w[i] >> 1) | (carry << 31);
         carry = next;
      }
      trim();
   }
   int cmp(const BigUInt &b) const {
      if (w.size() != b.w.size()) return w.size() < b.w.size() ? -1 : 1;
      for (size_t i = w.size(); i-- > 0;)
         if (w[i] != b.w[i]) return w[i] < b.w[i] ? -1 : 1;
      return 0;
   }
   void sub(const BigUInt &b) {           // precondition: *this >= b
      uint64_t borrow = 0;
      for (size_t i = 0; i < w.size(); ++i) {
         uint64_t bv = (i < b.w.size() ? b.w[i] : 0u) + borrow;
         uint64_t av = w[i];
         w[i] = (uint32_t)(av - bv);
         borrow = av < bv;
      }
      trim();
   }
   void set_bit(int bit) {
      size_t i = (size_t)bit / 32;
      if (w.size() <= i) w.resize(i + 1);
      w[i] |= 1u << (bit % 32);
   }
   bool odd() const { return !w.empty() && (w[0] & 1u); }
   uint64_t word64(size_t i) const {
      uint64_t lo = i < w.size() ? w[i] : 0;
      uint64_t hi = i + 1 < w.size() ? w[i + 1] : 0;
      return lo | (hi << 32);
   }
};

static BigUInt pow5(int n) {
   BigUInt v(1);
   for (int i = 0; i < n; ++i) v.mul_small(5);
   return v;
}

static BigUInt divide(BigUInt rem, const BigUInt &den, BigUInt *rem_out) {
   BigUInt q;
   if (den.zero() || rem.cmp(den) < 0) { if (rem_out) *rem_out = rem; return q; }
   int shift = rem.bits() - den.bits();
   BigUInt d = den; d.shl(shift);
   for (int i = shift; i >= 0; --i) {
      if (rem.cmp(d) >= 0) { rem.sub(d); q.set_bit(i); }
      d.shr1();
   }
   if (rem_out) *rem_out = rem;
   return q;
}

static int cmp_scaled(const BigUInt &num, const BigUInt &den, int shift) {
   if (shift >= 0) { BigUInt a = num; a.shl(shift); return a.cmp(den); }
   BigUInt b = den; b.shl(-shift); return num.cmp(b);
}

static BigUInt rounded_ratio(BigUInt num, BigUInt den, int shift) {
   if (shift >= 0) num.shl(shift); else den.shl(-shift);
   BigUInt rem;
   BigUInt q = divide(num, den, &rem);
   rem.shl(1);
   int half = rem.cmp(den);
   if (half > 0 || (half == 0 && q.odd())) q.add_small(1);
   return q;
}

static luna_binary128 pack(bool neg, BigUInt num, BigUInt den, int two_exp) {
   luna_binary128 out{0, neg ? 0x8000000000000000ull : 0};
   if (num.zero()) return out;

   int e = num.bits() - den.bits() + two_exp;
   while (cmp_scaled(num, den, two_exp - e) < 0) --e;
   while (cmp_scaled(num, den, two_exp - (e + 1)) >= 0) ++e;
   if (e > 16383) { out.hi |= 0x7fff000000000000ull; return out; }

   BigUInt q;
   int exp_field = 0;
   if (e < -16382) {
      q = rounded_ratio(num, den, two_exp + 16494);
      if (q.zero()) return out;
      if (q.bits() > 112) exp_field = 1;  // rounded to minimum normal
   } else {
      q = rounded_ratio(num, den, two_exp + 112 - e);
      if (q.bits() > 113) { q.shr1(); ++e; }
      if (e > 16383) { out.hi |= 0x7fff000000000000ull; return out; }
      exp_field = e + 16383;
   }

   out.lo = q.word64(0);
   /* q's implicit bit is bit 112 and is deliberately discarded here. */
   out.hi |= ((uint64_t)exp_field << 48) | (q.word64(2) & 0x0000ffffffffffffull);
   return out;
}

static bool ci_prefix(const char *p, const char *word) {
   while (*word) {
      if (std::tolower((unsigned char)*p++) != *word++) return false;
   }
   return true;
}

static int parse_exp(const char *&p) {
   bool neg = false;
   if (*p == '+' || *p == '-') { neg = *p == '-'; ++p; }
   int v = 0;
   while (std::isdigit((unsigned char)*p)) {
      if (v < 100000) v = v * 10 + (*p - '0');
      ++p;
   }
   return neg ? -v : v;
}

static luna_binary128 parse(const char *s, char **end) {
   const char *original = s;
   while (std::isspace((unsigned char)*s)) ++s;
   bool neg = false;
   if (*s == '+' || *s == '-') { neg = *s == '-'; ++s; }

   if (ci_prefix(s, "inf")) {
      s += 3; if (ci_prefix(s, "inity")) s += 5;
      if (end) *end = const_cast<char *>(s);
      return {0, (neg ? 0x8000000000000000ull : 0) | 0x7fff000000000000ull};
   }
   if (ci_prefix(s, "nan")) {
      s += 3;
      if (*s == '(') { const char *q = s + 1; while (std::isalnum((unsigned char)*q) || *q == '_') ++q; if (*q == ')') s = q + 1; }
      if (end) *end = const_cast<char *>(s);
      return {0, (neg ? 0x8000000000000000ull : 0) | 0x7fff800000000000ull};
   }

   BigUInt n;
   int frac = 0;
   bool any = false;
   bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
   if (hex) s += 2;
   const uint32_t radix = hex ? 16u : 10u;
   bool dot = false;
   for (;;) {
      int d = -1;
      unsigned char c = (unsigned char)*s;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
      if (d >= 0 && d < (int)radix) {
         n.mul_small(radix); n.add_small((uint32_t)d);
         any = true; if (dot) ++frac; ++s; continue;
      }
      if (!dot && *s == '.') { dot = true; ++s; continue; }
      break;
   }
   if (!any) {
      if (end) *end = const_cast<char *>(original);
      return {0, 0};
   }

   int exponent = 0;
   if ((hex && (*s == 'p' || *s == 'P')) || (!hex && (*s == 'e' || *s == 'E'))) {
      const char *mark = s, *q = s + 1;
      if (*q == '+' || *q == '-') ++q;
      if (std::isdigit((unsigned char)*q)) { ++s; exponent = parse_exp(s); }
      else s = mark;
   }
   if (end) *end = const_cast<char *>(s);

   if (hex) return pack(neg, n, BigUInt(1), exponent - 4 * frac);
   int dec_exp = exponent - frac;
   /* Do not build 5^100000 merely to discover an obvious infinity or zero.
    * 3.321 < log2(10), so the positive test is a lower bound and the
    * negative test is an upper bound; neither can clamp a finite binary128. */
   const int64_t nbits = n.bits();
   if (dec_exp >= 0 &&
       (nbits - 1) * 1000 + (int64_t)dec_exp * 3321 > 16384LL * 1000)
      return {0, (neg ? 0x8000000000000000ull : 0) | 0x7fff000000000000ull};
   if (dec_exp < 0 &&
       nbits * 1000 + (int64_t)dec_exp * 3321 < -16495LL * 1000)
      return {0, neg ? 0x8000000000000000ull : 0};
   if (dec_exp >= 0) {
      BigUInt f = pow5(dec_exp);
      /* multiply by 5^k using schoolbook multiplication. */
      BigUInt product;
      product.w.assign(n.w.size() + f.w.size(), 0);
      for (size_t i = 0; i < n.w.size(); ++i) {
         uint64_t carry = 0;
         for (size_t j = 0; j < f.w.size(); ++j) {
            uint64_t cur = product.w[i + j] + (uint64_t)n.w[i] * f.w[j] + carry;
            product.w[i + j] = (uint32_t)cur; carry = cur >> 32;
         }
         size_t k = i + f.w.size();
         while (carry) {
            uint64_t cur = product.w[k] + carry;
            product.w[k++] = (uint32_t)cur; carry = cur >> 32;
         }
      }
      product.trim();
      return pack(neg, product, BigUInt(1), dec_exp);
   }
   return pack(neg, n, pow5(-dec_exp), dec_exp);
}

} // namespace

extern "C" luna_binary128 luna_binary128_from_string(const char *s, char **end) {
   if (!s) { if (end) *end = nullptr; return {0, 0}; }
   return parse(s, end);
}

extern "C" luna_binary128 luna_binary128_from_wstring(const wchar_t *s, wchar_t **end) {
   if (!s) { if (end) *end = nullptr; return {0, 0}; }
   std::string ascii;
   const wchar_t *p = s;
   while (*p && (unsigned)*p < 128u) { ascii.push_back((char)*p); ++p; }
   ascii.push_back('\0');
   char *ae = nullptr;
   luna_binary128 v = parse(ascii.c_str(), &ae);
   if (end) *end = const_cast<wchar_t *>(s + (ae - ascii.c_str()));
   return v;
}
