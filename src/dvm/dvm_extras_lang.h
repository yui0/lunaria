/* java.lang numeric statics: compare/sum/exact/unsigned and the rest of the
 * java.lang.Math, Integer, Long, Short, Byte, Character and Boolean surface that
 * libraries reach for without a second thought (Guava's ComparisonChain sorts
 * HLS tracks with Integer.compare; a missing one answered 0, so every pair
 * compared equal).
 *
 * Merged into the builtin tables through rt_extras.  Included from
 * dvm_runtime.c; everything it uses is defined above. */

#ifndef DVM_EXTRAS_LANG_H
#define DVM_EXTRAS_LANG_H

#include <math.h>

static bool lang_overflow(struct dvm *vm, const char *what)
{
   dvm__throw(vm, "java/lang/ArithmeticException", "%s overflow", what);
   return false;
}

static int lang_cmp_i64(int64_t a, int64_t b) { return a < b ? -1 : a > b ? 1 : 0; }
static int lang_cmp_u64(uint64_t a, uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; }

/* ---- compare / hashCode on the small boxes --------------------------------- */

static bool lang_compare(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI(lang_cmp_i64(args[0].i, args[1].i));       /* (II) (SS) (BB) (CC) (ZZ) */
}

static bool lang_compare_char(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI((int)(uint16_t)args[0].i - (int)(uint16_t)args[1].i);   /* Character.compare is x - y */
}

static bool lang_compare_small(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI(args[0].i - args[1].i);                    /* Short.compare, Byte.compare are x - y */
}

static bool lang_hash_i(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI(args[0].i);
}

static bool lang_hash_bool(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI(args[0].i ? 1231 : 1237);
}

static bool lang_bool_op(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   const char *n = builtin_name();
   const bool a = args[0].i != 0, b = args[1].i != 0;
   RETI(!strcmp(n, "logicalAnd") ? a && b : !strcmp(n, "logicalOr") ? a || b : a != b);
}

static bool lang_unsigned_int(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   const char *n = builtin_name(), *sig = builtin_sig();
   if (!strcmp(n, "toUnsignedInt"))                /* Short / Byte */
      RETI(sig[1] == 'S' ? (uint16_t)args[0].i : (uint8_t)args[0].i);
   RETI(0);
}

/* ---- Integer / Long ------------------------------------------------------- */

static bool lang_int_arith(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *n = builtin_name();
   const int32_t a = args[0].i, b = args[1].i;
   if (!strcmp(n, "sum")) RETI((int32_t)((uint32_t)a + (uint32_t)b));
   if (!strcmp(n, "max")) RETI(a > b ? a : b);
   if (!strcmp(n, "min")) RETI(a < b ? a : b);
   if (!strcmp(n, "divideUnsigned") || !strcmp(n, "remainderUnsigned")) {
      if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "/ by zero"); return false; }
      RETI(!strcmp(n, "divideUnsigned") ? (uint32_t)a / (uint32_t)b : (uint32_t)a % (uint32_t)b);
   }
   RETI(0);
}

static bool lang_long_arith(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *n = builtin_name();
   const int64_t a = args[0].j, b = args[1].j;
   if (!strcmp(n, "sum")) RETJ((int64_t)((uint64_t)a + (uint64_t)b));
   if (!strcmp(n, "max")) RETJ(a > b ? a : b);
   if (!strcmp(n, "min")) RETJ(a < b ? a : b);
   if (!strcmp(n, "divideUnsigned") || !strcmp(n, "remainderUnsigned")) {
      if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "/ by zero"); return false; }
      RETJ(!strcmp(n, "divideUnsigned") ? (uint64_t)a / (uint64_t)b : (uint64_t)a % (uint64_t)b);
   }
   RETJ(0);
}

static bool lang_to_unsigned_long(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETJ((uint64_t)(uint32_t)args[0].i);
}

static bool lang_unsigned_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)self;
   const bool wide = builtin_sig()[1] == 'J';
   const uint64_t v = wide ? (uint64_t)args[0].j : (uint32_t)args[0].i;
   int radix = nargs > 1 ? args[1].i : 10;
   if (radix < 2 || radix > 36) radix = 10;
   char buf[72];
   int at = (int)sizeof buf - 1;
   uint64_t x = v;
   buf[at] = 0;
   do { const unsigned d = (unsigned)(x % (uint64_t)radix); buf[--at] = (char)(d < 10 ? '0' + d : 'a' + d - 10); x /= (uint64_t)radix; } while (x);
   RETL(dvm_new_string(vm, buf + at));
}

static bool lang_parse_unsigned(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)self;
   const bool wide = builtin_sig()[strlen(builtin_sig()) - 1] == 'J';
   const char *s = args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
   const int radix = nargs > 1 ? args[1].i : 10;
   char *end = NULL;
   errno = 0;
   unsigned long long v = s && *s && *s != '-' && *s != '+' && radix >= 2 && radix <= 36
                          ? strtoull(s, &end, radix) : 0;
   if (!s || !end || *end || errno || (!wide && v > 0xffffffffull)) {
      dvm__throw(vm, "java/lang/NumberFormatException", "For input string: \"%s\"", s ? s : "null");
      return false;
   }
   if (wide) RETJ((int64_t)v);
   RETI((int32_t)(uint32_t)v);
}

/* ---- Math ----------------------------------------------------------------- */

static bool m_copy_sign(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   if (builtin_sig()[1] == 'F') RETF(copysignf(args[0].f, args[1].f));
   RETD(copysign(args[0].d, args[1].d));
}

static bool m_signum_d(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   const double d = args[0].d;
   RETD(d > 0 ? 1.0 : d < 0 ? -1.0 : d);          /* zeros and NaN come back as they are */
}

static bool m_hyper(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   const char *n = builtin_name();
   const double x = args[0].d;
   RETD(!strcmp(n, "sinh") ? sinh(x) : !strcmp(n, "cosh") ? cosh(x) : !strcmp(n, "tanh") ? tanh(x)
        : !strcmp(n, "log1p") ? log1p(x) : expm1(x));
}

static bool m_ulp(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                  int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   if (builtin_sig()[1] == 'F') {
      const float f = fabsf(args[0].f);
      RETF(isfinite(f) ? nextafterf(f, INFINITY) - f : f);
   }
   const double d = fabs(args[0].d);
   RETD(isfinite(d) ? nextafter(d, INFINITY) - d : d);
}

static bool m_next(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   const bool up = !strcmp(builtin_name(), "nextUp");
   if (builtin_sig()[1] == 'F') RETF(nextafterf(args[0].f, up ? INFINITY : -INFINITY));
   RETD(nextafter(args[0].d, up ? INFINITY : -INFINITY));
}

static bool m_ieee_remainder(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETD(remainder(args[0].d, args[1].d));
}

static bool m_get_exponent(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   const bool f = builtin_sig()[1] == 'F';
   const double x = f ? (double)args[0].f : args[0].d;
   if (!isfinite(x)) RETI(f ? 128 : 1024);
   if (x == 0) RETI(f ? -127 : -1023);
   int e;
   (void)frexp(x, &e);
   RETI(e - 1);
}

static bool m_fma(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                  int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   if (builtin_sig()[1] == 'F') RETF(fmaf(args[0].f, args[1].f, args[2].f));
   RETD(fma(args[0].d, args[1].d, args[2].d));
}

static int64_t lang_floor_div(int64_t a, int64_t b)
{
   int64_t q = a / b;
   return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static bool m_floor_div_mod(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *n = builtin_name(), *sig = builtin_sig();
   const bool mod = !strcmp(n, "floorMod");
   const bool wide_a = sig[1] == 'J';
   const bool wide_b = sig[2] == 'J';
   const int64_t a = wide_a ? args[0].j : args[0].i;
   const int64_t b = wide_b ? args[1].j : args[1].i;
   if (!b) { dvm__throw(vm, "java/lang/ArithmeticException", "/ by zero"); return false; }
   if (!mod && b == -1) {                          /* MIN / -1 wraps, as in Java */
      if (wide_a) RETJ((int64_t)(0 - (uint64_t)a));
      RETI((int32_t)(0 - (uint32_t)a));
   }
   const int64_t q = (b == -1) ? 0 : lang_floor_div(a, b);
   const int64_t r = (b == -1) ? 0 : a - q * b;
   if (mod) {
      if (wide_b) RETJ(r);
      RETI((int32_t)r);                            /* floorMod(long,int) is an int */
   }
   if (wide_a || wide_b) RETJ(q);
   RETI((int32_t)q);
}

static bool m_exact(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)self;
   const char *n = builtin_name(), *sig = builtin_sig();
   const bool wide = sig[1] == 'J';
   if (!strcmp(n, "toIntExact")) {
      if (args[0].j < INT32_MIN || args[0].j > INT32_MAX) return lang_overflow(vm, "integer");
      RETI((int32_t)args[0].j);
   }
   const int64_t a = wide ? args[0].j : args[0].i;
   const int64_t b = nargs > 1 ? (wide ? args[1].j : args[1].i) : 0;
   int64_t r = 0;
   bool over;
   const int64_t lo = wide ? INT64_MIN : INT32_MIN, hi = wide ? INT64_MAX : INT32_MAX;
   if (!strcmp(n, "addExact")) over = __builtin_add_overflow(a, b, &r);
   else if (!strcmp(n, "subtractExact")) over = __builtin_sub_overflow(a, b, &r);
   else if (!strcmp(n, "multiplyExact")) over = __builtin_mul_overflow(a, b, &r);
   else if (!strcmp(n, "negateExact")) over = __builtin_sub_overflow((int64_t)0, a, &r);
   else if (!strcmp(n, "incrementExact")) over = __builtin_add_overflow(a, (int64_t)1, &r);
   else over = __builtin_sub_overflow(a, (int64_t)1, &r);           /* decrementExact */
   if (over || r < lo || r > hi) return lang_overflow(vm, wide ? "long" : "integer");
   if (wide) RETJ(r);
   RETI((int32_t)r);
}

#define XM(n, s, f) SM(n, s, f)

static const struct rt_method rt_x_math[] = {
   XM("copySign", "(DD)D", m_copy_sign), XM("copySign", "(FF)F", m_copy_sign),
   XM("signum", "(D)D", m_signum_d),
   XM("sinh", "(D)D", m_hyper), XM("cosh", "(D)D", m_hyper), XM("tanh", "(D)D", m_hyper),
   XM("log1p", "(D)D", m_hyper), XM("expm1", "(D)D", m_hyper),
   XM("ulp", "(D)D", m_ulp), XM("ulp", "(F)F", m_ulp),
   XM("nextUp", "(D)D", m_next), XM("nextUp", "(F)F", m_next),
   XM("nextDown", "(D)D", m_next), XM("nextDown", "(F)F", m_next),
   XM("IEEEremainder", "(DD)D", m_ieee_remainder),
   XM("getExponent", "(D)I", m_get_exponent), XM("getExponent", "(F)I", m_get_exponent),
   XM("fma", "(DDD)D", m_fma), XM("fma", "(FFF)F", m_fma),
   XM("floorDiv", "(II)I", m_floor_div_mod), XM("floorDiv", "(JI)J", m_floor_div_mod),
   XM("floorDiv", "(JJ)J", m_floor_div_mod),
   XM("floorMod", "(II)I", m_floor_div_mod), XM("floorMod", "(JI)I", m_floor_div_mod),
   XM("floorMod", "(JJ)J", m_floor_div_mod),
   XM("addExact", "(II)I", m_exact), XM("addExact", "(JJ)J", m_exact),
   XM("subtractExact", "(II)I", m_exact), XM("subtractExact", "(JJ)J", m_exact),
   XM("multiplyExact", "(II)I", m_exact), XM("multiplyExact", "(JJ)J", m_exact),
   XM("negateExact", "(I)I", m_exact), XM("negateExact", "(J)J", m_exact),
   XM("incrementExact", "(I)I", m_exact), XM("incrementExact", "(J)J", m_exact),
   XM("decrementExact", "(I)I", m_exact), XM("decrementExact", "(J)J", m_exact),
   XM("toIntExact", "(J)I", m_exact),
   M_END,
};

static const struct rt_method rt_x_integer[] = {
   XM("compare", "(II)I", lang_compare),
   XM("sum", "(II)I", lang_int_arith), XM("max", "(II)I", lang_int_arith), XM("min", "(II)I", lang_int_arith),
   XM("divideUnsigned", "(II)I", lang_int_arith), XM("remainderUnsigned", "(II)I", lang_int_arith),
   XM("toUnsignedLong", "(I)J", lang_to_unsigned_long),
   XM("toUnsignedString", "(I)Ljava/lang/String;", lang_unsigned_string),
   XM("toUnsignedString", "(II)Ljava/lang/String;", lang_unsigned_string),
   XM("parseUnsignedInt", "(Ljava/lang/String;)I", lang_parse_unsigned),
   XM("parseUnsignedInt", "(Ljava/lang/String;I)I", lang_parse_unsigned),
   M_END,
};

static const struct rt_method rt_x_long_more[] = {
   XM("sum", "(JJ)J", lang_long_arith), XM("max", "(JJ)J", lang_long_arith), XM("min", "(JJ)J", lang_long_arith),
   XM("divideUnsigned", "(JJ)J", lang_long_arith), XM("remainderUnsigned", "(JJ)J", lang_long_arith),
   XM("toUnsignedString", "(J)Ljava/lang/String;", lang_unsigned_string),
   XM("toUnsignedString", "(JI)Ljava/lang/String;", lang_unsigned_string),
   XM("parseUnsignedLong", "(Ljava/lang/String;)J", lang_parse_unsigned),
   XM("parseUnsignedLong", "(Ljava/lang/String;I)J", lang_parse_unsigned),
   M_END,
};

static const struct rt_method rt_x_short[] = {
   XM("compare", "(SS)I", lang_compare_small), XM("hashCode", "(S)I", lang_hash_i),
   XM("toUnsignedInt", "(S)I", lang_unsigned_int),
   M_END,
};

static const struct rt_method rt_x_byte[] = {
   XM("compare", "(BB)I", lang_compare_small), XM("hashCode", "(B)I", lang_hash_i),
   XM("toUnsignedInt", "(B)I", lang_unsigned_int),
   M_END,
};

static const struct rt_method rt_x_character[] = {
   XM("compare", "(CC)I", lang_compare_char), XM("hashCode", "(C)I", lang_hash_i),
   M_END,
};

static const struct rt_method rt_x_boolean[] = {
   XM("compare", "(ZZ)I", lang_compare), XM("hashCode", "(Z)I", lang_hash_bool),
   XM("logicalAnd", "(ZZ)Z", lang_bool_op), XM("logicalOr", "(ZZ)Z", lang_bool_op),
   XM("logicalXor", "(ZZ)Z", lang_bool_op),
   M_END,
};

#undef XM
#endif
