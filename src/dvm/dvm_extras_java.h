/* java.lang / java.util / java.io additions.
 *
 * Methods a class gains here are merged into its builtin table when the class
 * is defined (see rt_extras and dvm_runtime_define), so each lives next to the
 * behaviour it needs rather than inside a table that is far from its helpers.
 * Included from dvm_runtime.c; everything it uses is defined above. */

#ifndef DVM_EXTRAS_JAVA_H
#define DVM_EXTRAS_JAVA_H

/* A static call on a platform class by name. */
static bool x_call_static(struct dvm *vm, const char *cls, const char *name,
                          const char *sig, const union dvm_value *a, int n,
                          union dvm_value *out)
{
   struct dvm_class *c = dvm_find_class(vm, cls);
   struct dvm_method *m = c ? dvm_find_method(vm, c, name, sig) : NULL;
   if (!m) {
      dvm__throw(vm, "java/lang/NoSuchMethodError", "%s.%s%s", cls, name, sig);
      return false;
   }
   return dvm_call(vm, m, 0, a, n, out);
}

/* ------------------------------------------------------------------ Long */

static bool x_long_compare(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI(ARG(0).j < ARG(1).j ? -1 : ARG(0).j > ARG(1).j ? 1 : 0);
}

static const struct rt_method rt_x_long[] = {
   SM("compare", "(JJ)I", x_long_compare),
   M_END,
};

/* --------------------------------------------------------------- BitSet */

static struct rt_bitset *x_bits(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (o && !o->data) {
      union dvm_value ignored = { 0 };
      (void)bitset_init(vm, self, NULL, 0, &ignored);
   }
   return o ? o->data : NULL;
}

static bool x_bits_grow(struct dvm *vm, struct rt_bitset *b, uint32_t need)
{
   if (need <= b->nbits) return true;
   uint64_t cap = (uint64_t)b->nbits * 2;
   const uint64_t required = ((uint64_t)need + 63u) / 64u * 64u;
   if (cap < required) cap = required;
   if (cap > UINT64_C(2147483648)) cap = UINT64_C(2147483648);
   uint8_t *grown = realloc(b->bits, (size_t)cap / 8u);
   if (!grown) { dvm__throw(vm, "java/lang/OutOfMemoryError", "BitSet growth"); return false; }
   memset(grown + b->nbits / 8u, 0, (size_t)cap / 8u - b->nbits / 8u);
   b->bits = grown;
   b->nbits = (uint32_t)cap;
   return true;
}

static inline bool x_bit_get(const struct rt_bitset *b, uint32_t i)
{
   return i < b->nbits && (b->bits[i / 8u] & (1u << (i % 8u))) != 0;
}

static inline void x_bit_put(struct rt_bitset *b, uint32_t i, bool v)
{
   if (v) b->bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
   else   b->bits[i / 8u] &= (uint8_t)~(1u << (i % 8u));
}

/* Number of bits up to and including the highest set one. */
static uint32_t x_bits_length(const struct rt_bitset *b)
{
   for (uint32_t i = b->nbits; i > 0; --i)
      if (x_bit_get(b, i - 1u)) return i;
   return 0;
}

/* set / clear / flip over one index or a range, with an optional value. */
static bool x_bitset_modify(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)out;
   const char *name = builtin_name(), *sig = builtin_sig();
   struct rt_bitset *b = x_bits(vm, self);
   if (!b) RETV();
   const bool is_set = !strcmp(name, "set"), is_flip = !strcmp(name, "flip");
   int32_t from = nargs > 0 ? ARG(0).i : 0, to;
   bool value = !strcmp(name, "set");
   if (!strcmp(sig, "()V")) { from = 0; to = (int32_t)x_bits_length(b); }
   else if (!strcmp(sig, "(I)V") || !strcmp(sig, "(IZ)V")) {
      to = from + 1;
      if (!strcmp(sig, "(IZ)V")) value = ARG(1).i != 0;
   } else {
      to = ARG(1).i;
      if (!strcmp(sig, "(IIZ)V")) value = ARG(2).i != 0;
   }
   if (from < 0) { dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "fromIndex < 0: %d", from); return false; }
   if (to < 0)   { dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "toIndex < 0: %d", to); return false; }
   if (from > to){ dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "fromIndex: %d > toIndex: %d", from, to); return false; }
   if (is_set || is_flip) {
      if (!x_bits_grow(vm, b, (uint32_t)to)) return false;
   } else if ((uint32_t)to > b->nbits) to = (int32_t)b->nbits;
   for (int32_t i = from; i < to; ++i)
      x_bit_put(b, (uint32_t)i, is_flip ? !x_bit_get(b, (uint32_t)i) : value);
   RETV();
}

static bool x_bitset_cardinality(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct rt_bitset *b = x_bits(vm, self);
   int32_t n = 0;
   for (uint32_t i = 0; b && i < b->nbits / 8u; ++i) n += __builtin_popcount(b->bits[i]);
   RETI(n);
}

static bool x_bitset_length(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct rt_bitset *b = x_bits(vm, self);
   RETI(b ? (int32_t)x_bits_length(b) : 0);
}

static bool x_bitset_isEmpty(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct rt_bitset *b = x_bits(vm, self);
   RETI(!b || x_bits_length(b) == 0);
}

/* nextSetBit / nextClearBit / previousSetBit / previousClearBit. */
static bool x_bitset_scan(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   const struct rt_bitset *b = x_bits(vm, self);
   const int32_t from = ARG(0).i;
   const bool forward = !strncmp(name, "next", 4), want = strstr(name, "Set") != NULL;
   if (forward && from < 0) { dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "fromIndex < 0: %d", from); return false; }
   if (!forward && from < -1) { dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "fromIndex < -1: %d", from); return false; }
   if (!b) RETI(forward && !want ? from : -1);
   if (forward) {
      for (int64_t i = from; i < (int64_t)b->nbits; ++i)
         if (x_bit_get(b, (uint32_t)i) == want) RETI((int32_t)i);
      RETI(want ? -1 : from > (int32_t)b->nbits ? from : (int32_t)b->nbits);
   }
   for (int64_t i = from; i >= 0; --i)
      if (x_bit_get(b, (uint32_t)i) == want) RETI((int32_t)i);
   RETI(-1);
}

/* and / or / xor / andNot with another BitSet. */
static bool x_bitset_combine(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *name = builtin_name();
   struct rt_bitset *a = x_bits(vm, self);
   struct rt_bitset *o = ARG(0).l ? x_bits(vm, ARG(0).l) : NULL;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "set"); return false; }
   if (!a || !o) RETV();
   if (!x_bits_grow(vm, a, o->nbits)) return false;
   for (uint32_t i = 0; i < a->nbits / 8u; ++i) {
      const uint8_t ob = i < o->nbits / 8u ? o->bits[i] : 0u;
      if (!strcmp(name, "and")) a->bits[i] &= ob;
      else if (!strcmp(name, "or")) a->bits[i] |= ob;
      else if (!strcmp(name, "xor")) a->bits[i] ^= ob;
      else a->bits[i] &= (uint8_t)~ob;
   }
   RETV();
}

static bool x_bitset_intersects(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)nargs;
   const struct rt_bitset *a = x_bits(vm, self), *o = ARG(0).l ? x_bits(vm, ARG(0).l) : NULL;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "set"); return false; }
   for (uint32_t i = 0; a && o && i < a->nbits / 8u && i < o->nbits / 8u; ++i)
      if (a->bits[i] & o->bits[i]) RETI(1);
   RETI(0);
}

static bool x_bitset_equals(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l || !dvm__class_assignable(vm, dvm_object_class(vm, ARG(0).l),
                                           dvm_find_class(vm, "java/util/BitSet"))) RETI(0);
   const struct rt_bitset *a = x_bits(vm, self), *o = x_bits(vm, ARG(0).l);
   if (!a || !o) RETI(a == o);
   const uint32_t n = a->nbits > o->nbits ? a->nbits : o->nbits;
   for (uint32_t i = 0; i < n; ++i)
      if (x_bit_get(a, i) != x_bit_get(o, i)) RETI(0);
   RETI(1);
}

/* The JDK's hash: xor the 64-bit words, shifted by (length + 1). */
static bool x_bitset_hashCode(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct rt_bitset *b = x_bits(vm, self);
   uint64_t h = 1234;
   const uint32_t words = b ? (x_bits_length(b) + 63u) / 64u : 0;
   for (uint32_t i = words; i-- > 0;) {
      uint64_t w = 0;
      for (uint32_t k = 0; k < 8u; ++k) w |= (uint64_t)b->bits[i * 8u + k] << (8u * k);
      h ^= w * (i + 1u);
   }
   RETI((int32_t)((h >> 32) ^ h));
}

static bool x_bitset_get_range(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)nargs;
   const int32_t from = ARG(0).i, to = ARG(1).i;
   if (from < 0 || to < 0 || from > to) {
      dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "fromIndex: %d, toIndex: %d", from, to);
      return false;
   }
   const struct rt_bitset *b = x_bits(vm, self);
   dvm_ref copy = dvm_new_object(vm, dvm_find_class(vm, "java/util/BitSet"));
   if (!copy) return false;
   union dvm_value ignored = { 0 };
   if (!bitset_init(vm, copy, NULL, 0, &ignored)) return false;
   struct rt_bitset *c = x_bits(vm, copy);
   for (int32_t i = from; c && b && i < to; ++i) {
      if (!x_bit_get(b, (uint32_t)i)) continue;
      if (!x_bits_grow(vm, c, (uint32_t)(i - from) + 1u)) return false;
      x_bit_put(c, (uint32_t)(i - from), true);
   }
   RETL(copy);
}

static bool x_bitset_clone(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   const union dvm_value range[2] = { { .i = 0 }, { .i = (int32_t)(x_bits(vm, self) ? x_bits_length(x_bits(vm, self)) : 0) } };
   (void)args; (void)nargs;
   return x_bitset_get_range(vm, self, range, 2, out);
}

static bool x_bitset_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct rt_bitset *b = x_bits(vm, self);
   struct rt_bytes buf = { 0 };
   bytes_append(&buf, "{", 1);
   bool first = true;
   for (uint32_t i = 0; b && i < b->nbits; ++i) {
      if (!x_bit_get(b, i)) continue;
      char num[16];
      const int n = snprintf(num, sizeof num, "%s%u", first ? "" : ", ", i);
      bytes_append(&buf, num, (size_t)n);
      first = false;
   }
   bytes_append(&buf, "}", 1);
   dvm_ref s = dvm_new_string_n(vm, (const char *)buf.p, buf.len);
   free(buf.p);
   RETL(s);
}

static bool x_bitset_toByteArray(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct rt_bitset *b = x_bits(vm, self);
   const uint32_t bytes = b ? (x_bits_length(b) + 7u) / 8u : 0;
   dvm_ref arr = dvm_new_array(vm, 'B', "B", bytes);
   if (arr && bytes) memcpy(dvm_array_data(vm, arr), b->bits, bytes);
   RETL(arr);
}

static bool x_bitset_valueOf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_object *src = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   if (!src || src->kind != DVM_OBJ_ARRAY) { dvm__throw(vm, "java/lang/NullPointerException", "bytes"); return false; }
   dvm_ref set = dvm_new_object(vm, dvm_find_class(vm, "java/util/BitSet"));
   union dvm_value ignored = { 0 };
   if (!set || !bitset_init(vm, set, NULL, 0, &ignored)) return false;
   struct rt_bitset *b = x_bits(vm, set);
   src = dvm__obj(vm, ARG(0).l);
   const size_t width = src->elem_kind == 'J' ? 8u : 1u;
   if (b && src->length && !x_bits_grow(vm, b, (uint32_t)(src->length * width * 8u))) return false;
   if (b && src->length) memcpy(b->bits, src->data, (size_t)src->length * width);   /* little-endian words */
   RETL(set);
}

static const struct rt_method rt_x_bitset[] = {
   M("set", "(IZ)V", x_bitset_modify), M("set", "(II)V", x_bitset_modify), M("set", "(IIZ)V", x_bitset_modify),
   M("clear", "()V", x_bitset_modify), M("clear", "(I)V", x_bitset_modify), M("clear", "(II)V", x_bitset_modify),
   M("flip", "(I)V", x_bitset_modify), M("flip", "(II)V", x_bitset_modify),
   M("get", "(II)Ljava/util/BitSet;", x_bitset_get_range),
   M("cardinality", "()I", x_bitset_cardinality), M("length", "()I", x_bitset_length),
   M("isEmpty", "()Z", x_bitset_isEmpty),
   M("nextSetBit", "(I)I", x_bitset_scan), M("nextClearBit", "(I)I", x_bitset_scan),
   M("previousSetBit", "(I)I", x_bitset_scan), M("previousClearBit", "(I)I", x_bitset_scan),
   M("and", "(Ljava/util/BitSet;)V", x_bitset_combine), M("or", "(Ljava/util/BitSet;)V", x_bitset_combine),
   M("xor", "(Ljava/util/BitSet;)V", x_bitset_combine), M("andNot", "(Ljava/util/BitSet;)V", x_bitset_combine),
   M("intersects", "(Ljava/util/BitSet;)Z", x_bitset_intersects),
   M("equals", "(Ljava/lang/Object;)Z", x_bitset_equals), M("hashCode", "()I", x_bitset_hashCode),
   M("clone", "()Ljava/lang/Object;", x_bitset_clone), M("toString", "()Ljava/lang/String;", x_bitset_toString),
   M("toByteArray", "()[B", x_bitset_toByteArray),
   SM("valueOf", "([B)Ljava/util/BitSet;", x_bitset_valueOf), SM("valueOf", "([J)Ljava/util/BitSet;", x_bitset_valueOf),
   M_END,
};

/* ------------------------------------------------------------------ Date */

/* The deprecated field getters read the civil date in the default zone, which
 * is what a Calendar over the same instant says. */
static bool x_date_field(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value millis = { 0 }, cal = { 0 }, arg = { 0 };
   (void)dvm_get_field(vm, self, "millis", "J", &millis);
   if (!x_call_static(vm, "java/util/Calendar", "getInstance", "()Ljava/util/Calendar;", NULL, 0, &cal)) return false;
   arg.j = millis.j;
   union dvm_value ignored = { 0 };
   if (!am_vcall(vm, cal.l, "setTimeInMillis", "(J)V", &arg, 1, &ignored)) return false;
   const char *name = builtin_name();
   static const struct { const char *name; int field, bias; } map[] = {
      { "getYear", 1, -1900 }, { "getMonth", 2, 0 }, { "getDate", 5, 0 }, { "getDay", 7, -1 },
      { "getHours", 11, 0 }, { "getMinutes", 12, 0 }, { "getSeconds", 13, 0 },
   };
   for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) {
      if (strcmp(name, map[i].name)) continue;
      arg.i = map[i].field;
      if (!am_vcall(vm, cal.l, "get", "(I)I", &arg, 1, &ignored)) return false;
      RETI(ignored.i + map[i].bias);
   }
   RETI(0);
}

static const struct rt_method rt_x_date[] = {
   M("getYear", "()I", x_date_field), M("getMonth", "()I", x_date_field),
   M("getDate", "()I", x_date_field), M("getDay", "()I", x_date_field),
   M("getHours", "()I", x_date_field), M("getMinutes", "()I", x_date_field),
   M("getSeconds", "()I", x_date_field),
   M_END,
};

/* ------------------------------------------------------------- TimeZone */

/* getDisplayName(daylight, style, locale): the zone's name for the locale.
 * The locale only chooses the language of the name; the table behind the
 * two-argument form answers in the default one. */
static bool x_tz_displayName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   return tz_getDisplayName(vm, self, args, 2, out);
}

static const struct rt_method rt_x_timezone[] = {
   M("getDisplayName", "(ZILjava/util/Locale;)Ljava/lang/String;", x_tz_displayName),
   M_END,
};

/* ----------------------------------------------------------- Properties */

static void x_prop_unescape_append(struct rt_bytes *b, const char *s, size_t n)
{
   for (size_t i = 0; i < n; ++i) {
      if (s[i] != '\\' || i + 1 >= n) { bytes_append(b, &s[i], 1); continue; }
      const char c = s[++i];
      switch (c) {
      case 't': bytes_append(b, "\t", 1); break;
      case 'n': bytes_append(b, "\n", 1); break;
      case 'r': bytes_append(b, "\r", 1); break;
      case 'f': bytes_append(b, "\f", 1); break;
      case 'u': {
         uint32_t cp = 0;
         size_t k = 0;
         for (; k < 4 && i + 1 + k < n && isxdigit((unsigned char)s[i + 1 + k]); ++k) {
            const char d = s[i + 1 + k];
            cp = cp * 16u + (uint32_t)(d <= '9' ? d - '0' : (d | 32) - 'a' + 10);
         }
         i += k;
         char u[4];
         size_t un;
         if (cp < 0x80) { u[0] = (char)cp; un = 1; }
         else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); un = 2; }
         else { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); un = 3; }
         bytes_append(b, u, un);
         break;
      }
      default: bytes_append(b, &c, 1); break;
      }
   }
}

/* Parses the java.util.Properties text format and puts each pair into `self`. */
static bool x_prop_parse(struct dvm *vm, dvm_ref self, const char *text, size_t len)
{
   size_t pos = 0;
   while (pos < len) {
      /* One logical line: physical lines joined across an odd run of backslashes. */
      struct rt_bytes line = { 0 };
      bool first_physical = true, skip = false;
      for (;;) {
         size_t end = pos;
         while (end < len && text[end] != '\n' && text[end] != '\r') ++end;
         size_t start = pos;
         while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\f')) ++start;
         if (first_physical && (start == end || text[start] == '#' || text[start] == '!')) { skip = true; }
         size_t slashes = 0;
         for (size_t k = end; k > start && text[k - 1] == '\\'; --k) ++slashes;
         const bool continued = !skip && (slashes & 1u) && end < len;
         bytes_append(&line, text + start, end - start - (continued ? 1u : 0u));
         if (end < len && text[end] == '\r' && end + 1 < len && text[end + 1] == '\n') ++end;
         pos = end < len ? end + 1 : end;
         if (!continued) break;
         first_physical = false;
      }
      if (!skip && line.len) {
         const char *s = (const char *)line.p;
         size_t k = 0;
         while (k < line.len && s[k] != '=' && s[k] != ':' && s[k] != ' ' && s[k] != '\t' && s[k] != '\f') {
            if (s[k] == '\\') ++k;
            ++k;
         }
         if (k > line.len) k = line.len;
         size_t v = k;
         while (v < line.len && (s[v] == ' ' || s[v] == '\t' || s[v] == '\f')) ++v;
         if (v < line.len && (s[v] == '=' || s[v] == ':')) ++v;
         while (v < line.len && (s[v] == ' ' || s[v] == '\t' || s[v] == '\f')) ++v;
         struct rt_bytes key = { 0 }, value = { 0 };
         x_prop_unescape_append(&key, s, k);
         x_prop_unescape_append(&value, s + v, line.len - v);
         union dvm_value kv[2] = { { .l = dvm_new_string_n(vm, key.p ? (const char *)key.p : "", key.len) },
                                   { .l = dvm_new_string_n(vm, value.p ? (const char *)value.p : "", value.len) } };
         union dvm_value ignored = { 0 };
         const bool ok = kv[0].l && kv[1].l && props_put_virtual(vm, self, kv, 2, &ignored);
         free(key.p); free(value.p);
         if (!ok) { free(line.p); return false; }
      }
      free(line.p);
   }
   return true;
}

/* All the bytes (or UTF-16 units as UTF-8) a stream or reader yields. */
static bool x_read_all(struct dvm *vm, dvm_ref source, bool chars, struct rt_bytes *all)
{
   dvm_ref chunk = dvm_new_array(vm, chars ? 'C' : 'B', chars ? "C" : "B", 4096);
   if (!chunk) return false;
   dvm_pin(vm, chunk);
   bool good = true;
   for (;;) {
      union dvm_value arg = { .l = chunk }, n = { 0 };
      if (!am_vcall(vm, source, "read", chars ? "([C)I" : "([B)I", &arg, 1, &n)) { good = false; break; }
      if (n.i <= 0) break;
      const void *data = dvm_array_data(vm, chunk);
      if (!chars) bytes_append(all, data, (size_t)n.i);
      else {
         const uint16_t *u = data;
         for (int32_t i = 0; i < n.i; ++i) {
            char enc[3];
            size_t en;
            if (u[i] < 0x80) { enc[0] = (char)u[i]; en = 1; }
            else if (u[i] < 0x800) { enc[0] = (char)(0xC0 | (u[i] >> 6)); enc[1] = (char)(0x80 | (u[i] & 0x3F)); en = 2; }
            else { enc[0] = (char)(0xE0 | (u[i] >> 12)); enc[1] = (char)(0x80 | ((u[i] >> 6) & 0x3F)); enc[2] = (char)(0x80 | (u[i] & 0x3F)); en = 3; }
            bytes_append(all, enc, en);
         }
      }
   }
   dvm_unpin(vm, chunk);
   return good;
}

/* load(InputStream) reads ISO-8859-1; load(Reader) reads characters. */
static bool x_prop_load(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "inStream"); return false; }
   const bool reader = strstr(builtin_sig(), "Reader") != NULL;
   struct rt_bytes all = { 0 };
   bool good = x_read_all(vm, ARG(0).l, reader, &all);
   if (good && !reader) {
      /* ISO-8859-1 to UTF-8, which the parser and the VM's strings use. */
      struct rt_bytes utf8 = { 0 };
      for (size_t i = 0; i < all.len; ++i) {
         const unsigned char c = all.p[i];
         if (c < 0x80) bytes_append(&utf8, &c, 1);
         else { const char two[2] = { (char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F)) }; bytes_append(&utf8, two, 2); }
      }
      free(all.p);
      all = utf8;
   }
   if (good) good = x_prop_parse(vm, self, all.p ? (const char *)all.p : "", all.len);
   free(all.p);
   return good;
}

static void x_prop_escape(struct rt_bytes *b, const char *s, size_t n, bool key, bool ascii_only)
{
   for (size_t i = 0; i < n;) {
      const unsigned char c = (unsigned char)s[i];
      if (c >= 0x80) {
         uint32_t cp = 0;
         size_t len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
         if (i + len > n) len = 1;
         cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
         for (size_t k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
         if (ascii_only) {
            char esc[16];
            int en;
            if (cp >= 0x10000) {
               const uint32_t v = cp - 0x10000;
               en = snprintf(esc, sizeof esc, "\\u%04X\\u%04X", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            } else en = snprintf(esc, sizeof esc, "\\u%04X", cp);
            bytes_append(b, esc, (size_t)en);
         } else bytes_append(b, s + i, len);
         i += len;
         continue;
      }
      switch (c) {
      case ' ': if (key || i == 0) bytes_append(b, "\\ ", 2); else bytes_append(b, " ", 1); break;
      case '\t': bytes_append(b, "\\t", 2); break;
      case '\n': bytes_append(b, "\\n", 2); break;
      case '\r': bytes_append(b, "\\r", 2); break;
      case '\f': bytes_append(b, "\\f", 2); break;
      case '=': case ':': case '#': case '!': case '\\': bytes_append(b, "\\", 1); bytes_append(b, &c, 1); break;
      default:
         if (c < 0x20 || c > 0x7E) { char esc[8]; const int en = snprintf(esc, sizeof esc, "\\u%04X", c); bytes_append(b, esc, (size_t)en); }
         else bytes_append(b, &c, 1);
         break;
      }
      ++i;
   }
}

/* store(OutputStream|Writer, comments): the file Properties.load reads back. */
static bool x_prop_store(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "out"); return false; }
   const bool writer = strstr(builtin_sig(), "Writer") != NULL;
   struct rt_bytes text = { 0 };
   const char *comments = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL;
   if (comments) { bytes_append(&text, "#", 1); bytes_append(&text, comments, strlen(comments)); bytes_append(&text, "\n", 1); }
   {
      const time_t now = time(NULL);
      struct tm tm;
      char stamp[64];
      if (luna_localtime(&now, &tm)) {
         const size_t n = strftime(stamp, sizeof stamp, "#%a %b %d %H:%M:%S %Z %Y\n", &tm);
         bytes_append(&text, stamp, n);
      }
   }
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   for (uint32_t i = 0; mp && i < mp->size; ++i) {
      const char *k = dvm_string_utf8(vm, mp->e[i].k), *v = dvm_string_utf8(vm, mp->e[i].v);
      if (!k || !v) continue;
      x_prop_escape(&text, k, strlen(k), true, !writer);
      bytes_append(&text, "=", 1);
      x_prop_escape(&text, v, strlen(v), false, !writer);
      bytes_append(&text, "\n", 1);
      o = dvm__obj(vm, self);
      mp = o ? o->data : NULL;
   }
   bool good;
   if (writer) {
      union dvm_value s = { .l = dvm_new_string_n(vm, text.p ? (const char *)text.p : "", text.len) }, ignored = { 0 };
      good = s.l && am_vcall(vm, ARG(0).l, "write", "(Ljava/lang/String;)V", &s, 1, &ignored);
   } else {
      dvm_ref bytes = dvm_new_array(vm, 'B', "B", (uint32_t)text.len);
      union dvm_value a = { .l = bytes }, ignored = { 0 };
      if (bytes && text.len) memcpy(dvm_array_data(vm, bytes), text.p, text.len);
      good = bytes && am_vcall(vm, ARG(0).l, "write", "([B)V", &a, 1, &ignored);
   }
   free(text.p);
   return good;
}

static const struct rt_method rt_x_properties[] = {
   M("load", "(Ljava/io/InputStream;)V", x_prop_load), M("load", "(Ljava/io/Reader;)V", x_prop_load),
   M("store", "(Ljava/io/OutputStream;Ljava/lang/String;)V", x_prop_store),
   M("store", "(Ljava/io/Writer;Ljava/lang/String;)V", x_prop_store),
   M_END,
};

/* ----------------------------------------------------------- Collections */

static bool x_collections_list(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "e"); return false; }
   dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!list) return false;
   dvm_pin(vm, list);
   bool good = true;
   for (;;) {
      union dvm_value more = { 0 }, item = { 0 }, ignored = { 0 };
      good = am_vcall(vm, ARG(0).l, "hasMoreElements", "()Z", NULL, 0, &more);
      if (!good || !more.i) break;
      good = am_vcall(vm, ARG(0).l, "nextElement", "()Ljava/lang/Object;", NULL, 0, &item) &&
             list_add(vm, list, &item, 1, &ignored);
      if (!good) break;
   }
   dvm_unpin(vm, list);
   if (!good) return false;
   RETL(list);
}

/* shuffle(list[, random]): Fisher-Yates from the end.  Swaps go through the
 * List's own get/set, so any List implementation works. */
static bool x_collections_shuffle(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)self; (void)out;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "list"); return false; }
   union dvm_value size = { 0 };
   if (!am_vcall(vm, ARG(0).l, "size", "()I", NULL, 0, &size)) return false;
   static _Atomic uint64_t seed = 0x9E3779B97F4A7C15ull;
   for (int32_t i = size.i; i > 1; --i) {
      int32_t j;
      if (nargs > 1 && ARG(1).l) {
         union dvm_value bound = { .i = i }, r = { 0 };
         if (!am_vcall(vm, ARG(1).l, "nextInt", "(I)I", &bound, 1, &r)) return false;
         j = r.i;
      } else {
         uint64_t x = atomic_load(&seed) ^ (uint64_t)luna_os_uptime_ns();
         x ^= x << 13; x ^= x >> 7; x ^= x << 17;
         atomic_store(&seed, x);
         j = (int32_t)(x % (uint64_t)i);
      }
      union dvm_value gi = { .i = i - 1 }, gj = { .i = j }, a = { 0 }, b = { 0 }, ignored = { 0 };
      if (!am_vcall(vm, ARG(0).l, "get", "(I)Ljava/lang/Object;", &gi, 1, &a)) return false;
      dvm_pin(vm, a.l);
      bool good = am_vcall(vm, ARG(0).l, "get", "(I)Ljava/lang/Object;", &gj, 1, &b);
      if (good) {
         const union dvm_value set_i[2] = { gi, b }, set_j[2] = { gj, a };
         good = am_vcall(vm, ARG(0).l, "set", "(ILjava/lang/Object;)Ljava/lang/Object;", set_i, 2, &ignored) &&
                am_vcall(vm, ARG(0).l, "set", "(ILjava/lang/Object;)Ljava/lang/Object;", set_j, 2, &ignored);
      }
      dvm_unpin(vm, a.l);
      if (!good) return false;
   }
   RETV();
}

static const struct rt_method rt_x_collections[] = {
   SM("list", "(Ljava/util/Enumeration;)Ljava/util/ArrayList;", x_collections_list),
   SM("shuffle", "(Ljava/util/List;)V", x_collections_shuffle),
   SM("shuffle", "(Ljava/util/List;Ljava/util/Random;)V", x_collections_shuffle),
   M_END,
};

/* ------------------------------------------------------------- Executors */

/* purge() drops cancelled tasks still waiting in the queue.  Tasks here run as
 * they are submitted and nothing waits, so there is no cancelled entry to drop. */
static const struct rt_method rt_x_executor[] = {
   M("purge", "()V", empty_void),
   M_END,
};

/* ---------------------------------------------------------------- System */

static const struct rt_method rt_x_system[] = {
   /* Android never installs a SecurityManager. */
   SM("getSecurityManager", "()Ljava/lang/SecurityManager;", ret_null),
   M_END,
};

/* ---------------------------------------------------------- ProcessBuilder */

static const struct rt_field rt_x_process_builder_fields[] = {
   { "command", "Ljava/util/List;" }, F_END
};

static bool x_pb_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)out;
   dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!list) return false;
   union dvm_value ignored = { 0 };
   struct dvm_object *src = nargs && ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   if (src && src->kind == DVM_OBJ_ARRAY) {
      for (uint32_t i = 0; i < src->length; ++i) {
         union dvm_value item = { .l = ((const dvm_ref *)src->data)[i] };
         if (!list_add(vm, list, &item, 1, &ignored)) return false;
      }
   } else if (src) {
      union dvm_value all = { .l = ARG(0).l };
      if (!list_addAll(vm, list, &all, 1, &ignored)) return false;
   }
   (void)dvm_set_field(vm, self, "command", "Ljava/util/List;", (union dvm_value){ .l = list });
   RETV();
}

static bool x_pb_command(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   return dvm_get_field(vm, self, "command", "Ljava/util/List;", out);
}

/* start() runs the command exactly as Runtime.exec() would. */
static bool x_pb_start(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value command = { 0 };
   (void)dvm_get_field(vm, self, "command", "Ljava/util/List;", &command);
   struct rt_list *l = command.l ? list_of(vm, command.l) : NULL;
   if (!l || !l->size) { dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "Index: 0, Size: 0"); return false; }
   dvm_ref argv = dvm_new_array(vm, 'L', "Ljava/lang/String;", l->size);
   if (!argv) return false;
   l = list_of(vm, command.l);
   for (uint32_t i = 0; i < l->size; ++i) ((dvm_ref *)dvm_array_data(vm, argv))[i] = l->items[i];
   const union dvm_value one = { .l = argv };
   return runtime_exec(vm, 0, &one, 1, out);
}

static const struct rt_method rt_x_process_builder[] = {
   M("<init>", "([Ljava/lang/String;)V", x_pb_init), M("<init>", "(Ljava/util/List;)V", x_pb_init),
   M("command", "()Ljava/util/List;", x_pb_command),
   M("start", "()Ljava/lang/Process;", x_pb_start),
   M_END,
};


/* ------------------------------------------------------- NetworkInterface */

/* java.net.NetworkInterface over the host's interfaces (luna_os_net_list).  An
 * app that fingerprints the device (a game SDK collecting its network
 * parameters) lists them and asks each for its addresses.  Like Android 11+,
 * getHardwareAddress() answers null: the MAC is withheld from apps. */
#define NI_UP 1
#define NI_LOOPBACK 2
#define NI_MULTICAST 4
#define NI_POINT_TO_POINT 8

static const struct rt_field rt_netif_fields[] = {
   { "name", "Ljava/lang/String;" }, { "index", "I" }, { "mtu", "I" }, { "flags", "I" },
   { "addresses", "Ljava/util/List;" }, { "interfaceAddresses", "Ljava/util/List;" }, F_END
};
static const struct rt_field rt_interface_address_fields[] = {
   { "address", "Ljava/net/InetAddress;" }, { "prefix", "I" }, F_END
};

static dvm_ref ni_enumeration(struct dvm *vm, dvm_ref collection)
{
   dvm_ref e = dvm_new_object(vm, dvm_find_class(vm, "lunaria/util/ResourceKeys"));
   if (e) (void)dvm_set_field(vm, e, "target", "Ljava/lang/Object;", (union dvm_value){ .l = collection });
   return e;
}

static dvm_ref ni_make(struct dvm *vm, const struct luna_os_netif *host)
{
   dvm_ref ni = dvm_new_object(vm, dvm_find_class(vm, "java/net/NetworkInterface"));
   dvm_ref addresses = coll_new_list(vm, "Ljava/util/ArrayList;");
   dvm_ref ifaddresses = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!ni || !addresses || !ifaddresses) return 0;
   (void)dvm_set_field(vm, ni, "name", "Ljava/lang/String;", (union dvm_value){ .l = dvm_new_string(vm, host->name) });
   rt_set_int(vm, ni, "index", (int32_t)host->index);
   rt_set_int(vm, ni, "mtu", host->mtu ? (int32_t)host->mtu : -1);
   rt_set_int(vm, ni, "flags", (host->up ? NI_UP : 0) | (host->loopback ? NI_LOOPBACK : 0) |
              (host->multicast ? NI_MULTICAST : 0) | (host->point_to_point ? NI_POINT_TO_POINT : 0));
   for (int i = 0; i < host->naddr; ++i) {
      const size_t n = host->addr[i].family == 4 ? 4 : 16;
      dvm_ref bytes = dvm_new_array(vm, 'B', "B", (uint32_t)n);
      if (!bytes) return 0;
      memcpy(dvm_array_data(vm, bytes), host->addr[i].bytes, n);
      union dvm_value arg = { .l = bytes }, address = { 0 }, ignored = { 0 };
      if (!x_call_static(vm, "java/net/InetAddress", "getByAddress", "([B)Ljava/net/InetAddress;", &arg, 1, &address))
         return 0;
      union dvm_value item = { .l = address.l };
      (void)list_add(vm, addresses, &item, 1, &ignored);
      dvm_ref ia = dvm_new_object(vm, dvm_find_class(vm, "java/net/InterfaceAddress"));
      if (!ia) return 0;
      (void)dvm_set_field(vm, ia, "address", "Ljava/net/InetAddress;", address);
      rt_set_int(vm, ia, "prefix", host->addr[i].prefix);
      item.l = ia;
      (void)list_add(vm, ifaddresses, &item, 1, &ignored);
   }
   (void)dvm_set_field(vm, ni, "addresses", "Ljava/util/List;", (union dvm_value){ .l = addresses });
   (void)dvm_set_field(vm, ni, "interfaceAddresses", "Ljava/util/List;", (union dvm_value){ .l = ifaddresses });
   return ni;
}

/* Every host interface, as a list of NetworkInterface objects. */
static dvm_ref ni_all(struct dvm *vm)
{
   struct luna_os_netif host[32];
   const int n = luna_os_net_list(host, 32);
   dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
   for (int i = 0; list && i < n; ++i) {
      union dvm_value item = { .l = ni_make(vm, &host[i]) }, ignored = { 0 };
      if (!item.l) return 0;
      (void)list_add(vm, list, &item, 1, &ignored);
   }
   return list;
}

static bool ni_getNetworkInterfaces(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   dvm_ref all = ni_all(vm);
   RETL(all ? ni_enumeration(vm, all) : 0);
}

static bool ni_find(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *what = builtin_name(), *sig = builtin_sig();
   const bool by_name = !strcmp(what, "getByName"), by_index = !strcmp(what, "getByIndex");
   const char *name = by_name && args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
   if (by_name && !name) { dvm__throw(vm, "java/lang/NullPointerException", "name is null"); return false; }
   if (by_index && args[0].i < 0) { dvm__throw(vm, "java/lang/IllegalArgumentException", "Interface index can't be negative"); return false; }
   if (!by_name && !by_index && !args[0].l) { dvm__throw(vm, "java/lang/NullPointerException", "addr is null"); return false; }
   uint8_t want[16] = { 0 };
   const int want_len = !by_name && !by_index ? inet_bytes(vm, args[0].l, want) : 0;
   (void)sig;
   dvm_ref all = ni_all(vm);
   struct rt_list *l = all ? list_of(vm, all) : NULL;
   for (uint32_t i = 0; l && i < l->size; ++i) {
      dvm_ref ni = l->items[i];
      if (by_name) {
         union dvm_value n = { 0 };
         (void)dvm_get_field(vm, ni, "name", "Ljava/lang/String;", &n);
         if (n.l && !strcmp(dvm_string_utf8(vm, n.l), name)) RETL(ni);
      } else if (by_index) {
         if (rt_get_int(vm, ni, "index") == args[0].i) RETL(ni);
      } else {
         union dvm_value addresses = { 0 };
         (void)dvm_get_field(vm, ni, "addresses", "Ljava/util/List;", &addresses);
         struct rt_list *al = addresses.l ? list_of(vm, addresses.l) : NULL;
         for (uint32_t k = 0; al && k < al->size; ++k) {
            uint8_t have[16] = { 0 };
            if (inet_bytes(vm, al->items[k], have) == want_len && !memcmp(have, want, (size_t)want_len)) RETL(ni);
         }
      }
   }
   RETL(0);
}

static bool ni_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *n = builtin_name();
   union dvm_value v = { 0 };
   if (!strcmp(n, "getName") || !strcmp(n, "getDisplayName")) {
      (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &v);
      RETL(v.l);
   }
   if (!strcmp(n, "getIndex")) RETI(rt_get_int(vm, self, "index"));
   if (!strcmp(n, "getMTU")) RETI(rt_get_int(vm, self, "mtu"));
   const int flags = rt_get_int(vm, self, "flags");
   if (!strcmp(n, "isUp")) RETI(flags & NI_UP);
   if (!strcmp(n, "isLoopback")) RETI(flags & NI_LOOPBACK);
   if (!strcmp(n, "supportsMulticast")) RETI(flags & NI_MULTICAST);
   if (!strcmp(n, "isPointToPoint")) RETI(flags & NI_POINT_TO_POINT);
   if (!strcmp(n, "getInetAddresses")) {
      (void)dvm_get_field(vm, self, "addresses", "Ljava/util/List;", &v);
      RETL(v.l ? ni_enumeration(vm, v.l) : 0);
   }
   if (!strcmp(n, "getInterfaceAddresses")) {
      (void)dvm_get_field(vm, self, "interfaceAddresses", "Ljava/util/List;", &v);
      RETL(v.l);
   }
   if (!strcmp(n, "getSubInterfaces")) RETL(ni_enumeration(vm, coll_new_list(vm, "Ljava/util/ArrayList;")));
   RETL(0);                    /* getParent, getHardwareAddress: none, as on Android 11+ */
}

static bool ni_equals(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)nargs;
   if (args[0].l == self) RETI(1);
   if (!args[0].l || dvm_object_class(vm, args[0].l) != dvm_object_class(vm, self)) RETI(0);
   union dvm_value a = { 0 }, b = { 0 };
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &a);
   (void)dvm_get_field(vm, args[0].l, "name", "Ljava/lang/String;", &b);
   RETI(a.l && b.l && !strcmp(dvm_string_utf8(vm, a.l), dvm_string_utf8(vm, b.l)));
}

static bool ni_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value n = { 0 };
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &n);
   const char *name = n.l ? dvm_string_utf8(vm, n.l) : "";
   char text[160];
   snprintf(text, sizeof text, "name:%s (%s)", name, name);
   RETL(dvm_new_string(vm, text));
}

static bool ni_hashCode(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value n = { 0 };
   uint32_t h = 0;
   if (dvm_get_field(vm, self, "name", "Ljava/lang/String;", &n) && n.l)
      for (const char *p = dvm_string_utf8(vm, n.l); p && *p; ++p) h = h * 31u + (unsigned char)*p;
   RETI((int32_t)h);
}

static bool ia_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   if (!strcmp(builtin_name(), "getAddress")) {
      (void)dvm_get_field(vm, self, "address", "Ljava/net/InetAddress;", &v);
      RETL(v.l);
   }
   if (!strcmp(builtin_name(), "getNetworkPrefixLength")) RETI(rt_get_int(vm, self, "prefix"));
   RETL(0);                    /* getBroadcast: the host does not say */
}

static const struct rt_method rt_netif[] = {
   SM("getNetworkInterfaces", "()Ljava/util/Enumeration;", ni_getNetworkInterfaces),
   SM("getByName", "(Ljava/lang/String;)Ljava/net/NetworkInterface;", ni_find),
   SM("getByIndex", "(I)Ljava/net/NetworkInterface;", ni_find),
   SM("getByInetAddress", "(Ljava/net/InetAddress;)Ljava/net/NetworkInterface;", ni_find),
   M("getName", "()Ljava/lang/String;", ni_get), M("getDisplayName", "()Ljava/lang/String;", ni_get),
   M("getIndex", "()I", ni_get), M("getMTU", "()I", ni_get),
   M("isUp", "()Z", ni_get), M("isLoopback", "()Z", ni_get),
   M("supportsMulticast", "()Z", ni_get), M("isPointToPoint", "()Z", ni_get),
   M("isVirtual", "()Z", ret_false),
   M("getInetAddresses", "()Ljava/util/Enumeration;", ni_get),
   M("getInterfaceAddresses", "()Ljava/util/List;", ni_get),
   M("getSubInterfaces", "()Ljava/util/Enumeration;", ni_get),
   M("getParent", "()Ljava/net/NetworkInterface;", ni_get),
   M("getHardwareAddress", "()[B", ni_get),
   M("equals", "(Ljava/lang/Object;)Z", ni_equals),
   M("hashCode", "()I", ni_hashCode),
   M("toString", "()Ljava/lang/String;", ni_toString),
   M_END,
};

static const struct rt_method rt_interface_address[] = {
   M("getAddress", "()Ljava/net/InetAddress;", ia_get),
   M("getNetworkPrefixLength", "()S", ia_get),
   M("getBroadcast", "()Ljava/net/InetAddress;", ia_get),
   M_END,
};

/* ------------------------------------------------------------------ URI */

/* URI.toURL(): only an absolute URI names a location a URL can open. */
static bool juri_to_url(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *text = uri_text(vm, self);
   const char *colon = text ? strchr(text, ':') : NULL;
   if (!colon || colon == text) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "URI is not absolute");
      return false;
   }
   struct dvm_class *uc = dvm__class_by_desc(vm, "Ljava/net/URL;");
   dvm_ref url = uc ? dvm_new_object(vm, uc) : 0;
   union dvm_value spec = { .l = dvm_new_string(vm, text) }, ignored = { 0 };
   if (!url || !am_vcall(vm, url, "<init>", "(Ljava/lang/String;)V", &spec, 1, &ignored))
      return false;
   RETL(url);
}

static const struct rt_method rt_x_java_uri[] = {
   M("toURL", "()Ljava/net/URL;", juri_to_url),
   M_END,
};

#endif

/* MPL-2.0. HttpsURLConnection owns Java TLS policy and Java socket I/O. */
static const struct rt_field rt_https_fields[]={
   {"sslSocketFactory","Ljavax/net/ssl/SSLSocketFactory;"},
   {"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;"},{"sslSession","Ljavax/net/ssl/SSLSession;"},F_END
};
static const char *const rt_hostname_verifier_iface[]={"Ljavax/net/ssl/HostnameVerifier;",NULL};
static bool https_verify_hostname(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;if(!ARG(0).l || !ARG(1).l)RETI(0);
   union dvm_value certificates={0},encoded={0};
   if(!am_vcall(vm,ARG(1).l,"getPeerCertificates","()[Ljava/security/cert/Certificate;",NULL,0,&certificates)) {
      if(object_is_a(vm,dvm_exception(vm),"Ljavax/net/ssl/SSLException;")){dvm_clear_exception(vm);RETI(0);}return false;
   }
   if(!certificates.l || !dvm_array_length(vm,certificates.l))RETI(0);
   dvm_ref *chain=dvm_array_data(vm,certificates.l);
   if(!am_vcall(vm,chain[0],"getEncoded","()[B",NULL,0,&encoded))return false;
   const unsigned char *p=dvm_array_data(vm,encoded.l);long length=(long)dvm_array_length(vm,encoded.l);
   X509 *certificate=p ? d2i_X509(NULL,&p,length) : NULL;if(!certificate)RETI(0);
   const char *host=dvm_string_utf8(vm,ARG(0).l);size_t n=dvm_string_utf8_length(vm,ARG(0).l);
   bool valid=false;
   if(host && n && !memchr(host,0,n)) {
      char *plain=malloc(n+1);
      if(plain){memcpy(plain,host,n);plain[n]=0;if(n>1 && plain[0]=='[' && plain[n-1]==']'){memmove(plain,plain+1,n-2);plain[n-2]=0;}
         unsigned char ip[16];bool address=inet_pton(AF_INET,plain,ip)==1 || inet_pton(AF_INET6,plain,ip)==1;
         valid=address ? X509_check_ip_asc(certificate,plain,0)==1 :
            X509_check_host(certificate,plain,strlen(plain),X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS|X509_CHECK_FLAG_NEVER_CHECK_SUBJECT,NULL)==1;
         free(plain);}
   }
   X509_free(certificate);RETI(valid);
}
static dvm_ref https_defaults(struct dvm *vm)
{
   static dvm_ref key;dvm_ref defaults=runtime_singleton(vm,"Llunaria/net/HttpsDefaults;",&key);
   union dvm_value factory={0},verifier={0},ignored={0};
   (void)dvm_get_field(vm,defaults,"sslSocketFactory","Ljavax/net/ssl/SSLSocketFactory;",&factory);
   if(!factory.l){if(!sslsf_getDefault(vm,0,NULL,0,&factory))return 0;(void)dvm_set_field(vm,defaults,"sslSocketFactory","Ljavax/net/ssl/SSLSocketFactory;",factory);}
   (void)dvm_get_field(vm,defaults,"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;",&verifier);
   if(!verifier.l){verifier.l=dvm_new_object(vm,dvm_find_class(vm,"lunaria/net/HostnameVerifier"));(void)dvm_set_field(vm,defaults,"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;",verifier);}
   (void)ignored;return defaults;
}
static bool https_initialize(struct dvm *vm,dvm_ref self)
{
   if(!object_is_a(vm,self,"Ljavax/net/ssl/HttpsURLConnection;"))return true;
   dvm_ref defaults=https_defaults(vm);if(!defaults)return false;union dvm_value value={0};
   (void)dvm_get_field(vm,defaults,"sslSocketFactory","Ljavax/net/ssl/SSLSocketFactory;",&value);
   (void)dvm_set_field(vm,self,"sslSocketFactory","Ljavax/net/ssl/SSLSocketFactory;",value);
   (void)dvm_get_field(vm,defaults,"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;",&value);
   (void)dvm_set_field(vm,self,"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;",value);return true;
}
static bool https_configuration(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=builtin_name();bool global=strstr(name,"Default")!=NULL,factory=strstr(name,"SocketFactory")!=NULL;
   dvm_ref target=global ? https_defaults(vm) : self;if(!target)return false;
   const char *field=factory ? "sslSocketFactory" : "hostnameVerifier",*type=factory ? "Ljavax/net/ssl/SSLSocketFactory;" : "Ljavax/net/ssl/HostnameVerifier;";
   if(nargs){if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","TLS configuration is null");return false;}
      (void)dvm_set_field(vm,target,field,type,ARG(0));RETV();}
   union dvm_value value={0};(void)dvm_get_field(vm,target,field,type,&value);RETL(value.l);
}
struct https_stream {struct dvm *vm;dvm_ref socket,input,output,input_buffer,output_buffer;};
static long https_stream_read(void *context,void *buffer,size_t length)
{
   struct https_stream *s=context;struct dvm *vm=s->vm;dvm_gil_acquire(vm);
   size_t count=length>65536 ? 65536 : length;dvm_ref bytes=s->input_buffer;
   union dvm_value args[3]={{.l=bytes},{0},{.i=(int32_t)count}},result={0};
   bool good=bytes && am_vcall(vm,s->input,"read","([BII)I",args,3,&result);
   long got=-1;if(good){if(result.i==-1)got=0;else if(result.i>0 && (size_t)result.i<=count){got=result.i;memcpy(buffer,dvm_array_data(vm,bytes),(size_t)got);}
      else {dvm__throw(vm,"java/io/IOException","Invalid TLS stream read count");}}
   dvm_gil_release(vm);if(got<0)errno=EIO;return got;
}
static long https_stream_write(void *context,const void *buffer,size_t length)
{
   struct https_stream *s=context;struct dvm *vm=s->vm;dvm_gil_acquire(vm);
   size_t count=length>65536 ? 65536 : length;dvm_ref bytes=s->output_buffer;if(bytes && count)memcpy(dvm_array_data(vm,bytes),buffer,count);
   union dvm_value args[3]={{.l=bytes},{0},{.i=(int32_t)count}},ignored={0};
   bool good=bytes && am_vcall(vm,s->output,"write","([BII)V",args,3,&ignored);
   dvm_gil_release(vm);if(!good)errno=EIO;return good ? (long)count : -1;
}
static void https_stream_close(void *context)
{
   struct https_stream *s=context;struct dvm *vm=s->vm;
   /* Teardown destroys every socket separately. Re-entering Java after heap
    * destruction has begun would access objects already torn down. */
   dvm_gil_acquire(vm);
   if(!dvm_is_stopping(vm)) {
      dvm_ref error=dvm_exception(vm);if(error){dvm_pin(vm,error);dvm_clear_exception(vm);}
      union dvm_value ignored={0};(void)am_vcall(vm,s->socket,"close","()V",NULL,0,&ignored);
      if(error){vm->exception=error;dvm_unpin(vm,error);}
   }
   dvm_unpin(vm,s->socket);dvm_unpin(vm,s->input);dvm_unpin(vm,s->output);dvm_unpin(vm,s->input_buffer);dvm_unpin(vm,s->output_buffer);dvm_gil_release(vm);free(s);
}
static void *https_open_transport(void *context,const char *url,int timeout,char *error,size_t error_size)
{
   struct https_request *request=context;struct dvm *vm=request->vm;
   if(strncasecmp(url,"https://",8))return dvm_http_connect(url,timeout,error,error_size);
   dvm_gil_acquire(vm);union dvm_value value={.l=dvm_new_string(vm,url)},ignored={0},host={0},port={0},factory={0},verifier={0},socket={0},session={0},input={0},output={0};
   dvm_ref parsed=dvm_new_object(vm,dvm_find_class(vm,"java/net/URL"));
   bool good=parsed && am_vcall(vm,parsed,"<init>","(Ljava/lang/String;)V",&value,1,&ignored) &&
      am_vcall(vm,parsed,"getHost","()Ljava/lang/String;",NULL,0,&host) && am_vcall(vm,parsed,"getPort","()I",NULL,0,&port);
   if(good && port.i<0)port.i=443;
   dvm_ref raw=good ? dvm_new_object(vm,dvm_find_class(vm,"java/net/Socket")) : 0;
   if(raw)dvm_pin(vm,raw);
   good=good && raw && am_vcall(vm,raw,"<init>","()V",NULL,0,&ignored);
   dvm_ref address=good ? dvm_new_object(vm,dvm_find_class(vm,"java/net/InetSocketAddress")) : 0;
   union dvm_value address_args[2]={host,port};
   good=good && address && am_vcall(vm,address,"<init>","(Ljava/lang/String;I)V",address_args,2,&ignored);
   union dvm_value connect[2]={{.l=address},{.i=timeout}};
   good=good && am_vcall(vm,raw,"connect","(Ljava/net/SocketAddress;I)V",connect,2,&ignored);
   (void)dvm_get_field(vm,request->connection,"sslSocketFactory","Ljavax/net/ssl/SSLSocketFactory;",&factory);
   (void)dvm_get_field(vm,request->connection,"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;",&verifier);
   union dvm_value wrap[4]={{.l=raw},host,port,{.i=1}};
   good=good && am_vcall(vm,factory.l,"createSocket","(Ljava/net/Socket;Ljava/lang/String;IZ)Ljava/net/Socket;",wrap,4,&socket) && socket.l;
   if(socket.l)dvm_pin(vm,socket.l);
   struct rt_conn *connection=conn_of(vm,request->connection);union dvm_value read_timeout={.i=connection ? connection->read_timeout : 0};
   good=good && am_vcall(vm,socket.l,"setSoTimeout","(I)V",&read_timeout,1,&ignored) &&
      am_vcall(vm,socket.l,"startHandshake","()V",NULL,0,&ignored) && am_vcall(vm,socket.l,"getSession","()Ljavax/net/ssl/SSLSession;",NULL,0,&session);
   union dvm_value verify[2]={host,session},verified={0};
   good=good && am_vcall(vm,verifier.l,"verify","(Ljava/lang/String;Ljavax/net/ssl/SSLSession;)Z",verify,2,&verified);
   if(good && !verified.i){dvm__throw(vm,"javax/net/ssl/SSLPeerUnverifiedException","Hostname verification failed");good=false;}
   good=good && am_vcall(vm,socket.l,"getInputStream","()Ljava/io/InputStream;",NULL,0,&input) && am_vcall(vm,socket.l,"getOutputStream","()Ljava/io/OutputStream;",NULL,0,&output);
   void *transport=NULL;
   if(good){struct https_stream *stream=calloc(1,sizeof *stream);
      if(stream){*stream=(struct https_stream){.vm=vm,.socket=socket.l,.input=input.l,.output=output.l};dvm_pin(vm,input.l);dvm_pin(vm,output.l);
         stream->input_buffer=dvm_new_array(vm,'B',NULL,65536);stream->output_buffer=dvm_new_array(vm,'B',NULL,65536);
         if(stream->input_buffer)dvm_pin(vm,stream->input_buffer);if(stream->output_buffer)dvm_pin(vm,stream->output_buffer);
         struct dvm_http_io io={stream,https_stream_read,https_stream_write,https_stream_close};
         if(stream->input_buffer && stream->output_buffer)transport=dvm_http_transport_from_io(&io);
         if(!transport){dvm_unpin(vm,input.l);dvm_unpin(vm,output.l);dvm_unpin(vm,stream->input_buffer);dvm_unpin(vm,stream->output_buffer);free(stream);}}
      if(!transport){dvm__throw(vm,"java/lang/OutOfMemoryError","HTTPS transport");good=false;}
   }
   if(good)(void)dvm_set_field(vm,request->connection,"sslSession","Ljavax/net/ssl/SSLSession;",session);
   else {
      dvm_ref exception=dvm_exception(vm);if(exception){dvm_pin(vm,exception);dvm_clear_exception(vm);}
      dvm_ref close=socket.l ? socket.l : raw;if(close)(void)am_vcall(vm,close,"close","()V",NULL,0,&ignored);
      if(exception){vm->exception=exception;dvm_unpin(vm,exception);}
      if(socket.l)dvm_unpin(vm,socket.l);snprintf(error,error_size,"Java TLS connection failed");
   }
   if(raw)dvm_unpin(vm,raw);dvm_gil_release(vm);return transport;
}
static const struct rt_method rt_https_connection[]={
   M("<init>","(Ljava/net/URL;)V",conn_init),
   M("setSSLSocketFactory","(Ljavax/net/ssl/SSLSocketFactory;)V",https_configuration),M("getSSLSocketFactory","()Ljavax/net/ssl/SSLSocketFactory;",https_configuration),
   SM("setDefaultSSLSocketFactory","(Ljavax/net/ssl/SSLSocketFactory;)V",https_configuration),SM("getDefaultSSLSocketFactory","()Ljavax/net/ssl/SSLSocketFactory;",https_configuration),
   M("setHostnameVerifier","(Ljavax/net/ssl/HostnameVerifier;)V",https_configuration),M("getHostnameVerifier","()Ljavax/net/ssl/HostnameVerifier;",https_configuration),
   SM("setDefaultHostnameVerifier","(Ljavax/net/ssl/HostnameVerifier;)V",https_configuration),SM("getDefaultHostnameVerifier","()Ljavax/net/ssl/HostnameVerifier;",https_configuration),M_END
};
static const struct rt_method rt_hostname_verifier[]={M("verify","(Ljava/lang/String;Ljavax/net/ssl/SSLSession;)Z",https_verify_hostname),M_END};
static const struct rt_method rt_hostname_verifier_decl[]={{"verify","(Ljava/lang/String;Ljavax/net/ssl/SSLSession;)Z",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},M_END};
