/* android.icu.text.UnicodeSet: a set of code points kept as sorted half-open
 * ranges.  Included by dvm_runtime.c after the class helpers.
 *
 * The pattern syntax is ICU's: [abc] [a-z] [^...] nested sets, '&' and '-' between
 * sets, \uXXXX \UXXXXXXXX \xHH escapes, [:prop:] [:^prop:] \p{prop} \P{prop}
 * with prop or name=value.  Properties come from the tables Lunaria already has:
 * General_Category, Block, Grapheme_Cluster_Break, White_Space, Alphabetic,
 * Ideographic, Lowercase, Uppercase, Any, ASCII, Assigned.  A property the tables
 * do not carry (Script, ...) is an IllegalArgumentException, never an empty set.
 * Sets of strings ("{ab}") are not supported and are rejected the same way. */

struct uset {
  int32_t *r;        /* start0, end0, start1, end1, ... ; ends exclusive */
  size_t n, cap;     /* ints in use / allocated */
  bool frozen;
};

#define USET_MAX 0x110000

static struct uset *uset_of(struct dvm *vm, dvm_ref self)
{
  struct dvm_object *o = dvm__obj(vm, self);
  if (!o) return NULL;
  if (!o->data) o->data = calloc(1, sizeof(struct uset));
  return o->data;
}

static bool uset_reserve(struct uset *s, size_t want)
{
  if (want <= s->cap) return true;
  size_t cap = s->cap ? s->cap * 2 : 16;
  while (cap < want) cap *= 2;
  int32_t *p = realloc(s->r, cap * sizeof *p);
  if (!p) return false;
  s->r = p;
  s->cap = cap;
  return true;
}

/* Adds [a, b] inclusive, merging with what is there. */
static bool uset_add_range(struct uset *s, int32_t a, int32_t b)
{
  if (a < 0) a = 0;
  if (b >= USET_MAX) b = USET_MAX - 1;
  if (a > b) return true;
  const int32_t lo = a, hi = b + 1;
  /* The first range that touches or follows the new one, and the last that touches it. */
  size_t i = 0;
  while (i < s->n && s->r[i + 1] < lo) i += 2;
  size_t j = i;
  int32_t nlo = lo, nhi = hi;
  while (j < s->n && s->r[j] <= hi) {
    if (s->r[j] < nlo) nlo = s->r[j];
    if (s->r[j + 1] > nhi) nhi = s->r[j + 1];
    j += 2;
  }
  const size_t removed = j - i;
  if (removed == 0) {
    if (!uset_reserve(s, s->n + 2)) return false;
    memmove(s->r + i + 2, s->r + i, (s->n - i) * sizeof *s->r);
    s->n += 2;
  } else if (removed > 2) {
    memmove(s->r + i + 2, s->r + j, (s->n - j) * sizeof *s->r);
    s->n -= removed - 2;
  }
  s->r[i] = nlo;
  s->r[i + 1] = nhi;
  return true;
}

static bool uset_complement(struct uset *s)
{
  struct uset t = { 0 };
  int32_t at = 0;
  for (size_t i = 0; i < s->n; i += 2) {
    if (s->r[i] > at && !uset_add_range(&t, at, s->r[i] - 1)) goto fail;
    at = s->r[i + 1];
  }
  if (at < USET_MAX && !uset_add_range(&t, at, USET_MAX - 1)) goto fail;
  free(s->r);
  s->r = t.r;
  s->n = t.n;
  s->cap = t.cap;
  return true;
fail:
  free(t.r);
  return false;
}

static bool uset_add_all(struct uset *s, const struct uset *o)
{
  for (size_t i = 0; i < o->n; i += 2)
    if (!uset_add_range(s, o->r[i], o->r[i + 1] - 1)) return false;
  return true;
}

/* s = s & o, or s - o when `minus`. */
static bool uset_combine(struct uset *s, const struct uset *o, bool minus)
{
  struct uset t = { 0 };
  size_t j = 0;
  for (size_t i = 0; i < s->n; i += 2) {
    int32_t a = s->r[i];
    const int32_t b = s->r[i + 1];
    while (j < o->n && o->r[j + 1] <= a) j += 2;
    size_t k = j;
    if (minus) {
      while (a < b) {
        if (k >= o->n || o->r[k] >= b) {
          if (!uset_add_range(&t, a, b - 1)) goto fail;
          break;
        }
        if (o->r[k] > a && !uset_add_range(&t, a, o->r[k] - 1)) goto fail;
        a = o->r[k + 1];
        k += 2;
      }
    } else {
      for (; k < o->n && o->r[k] < b; k += 2) {
        const int32_t lo = o->r[k] > a ? o->r[k] : a, hi = o->r[k + 1] < b ? o->r[k + 1] : b;
        if (lo < hi && !uset_add_range(&t, lo, hi - 1)) goto fail;
      }
    }
  }
  free(s->r);
  s->r = t.r;
  s->n = t.n;
  s->cap = t.cap;
  return true;
fail:
  free(t.r);
  return false;
}

static bool uset_contains(const struct uset *s, int32_t c)
{
  size_t lo = 0, hi = s->n / 2;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (c < s->r[mid * 2]) hi = mid;
    else if (c >= s->r[mid * 2 + 1]) lo = mid + 1;
    else return true;
  }
  return false;
}

static size_t uset_size(const struct uset *s)
{
  size_t n = 0;
  for (size_t i = 0; i < s->n; i += 2) n += (size_t)(s->r[i + 1] - s->r[i]);
  return n;
}

/* ---- properties ----------------------------------------------------------- */

static void uset_loose(const char *in, char *out, size_t cap)
{
  size_t n = 0;
  for (; *in && n + 1 < cap; ++in)
    if (*in != ' ' && *in != '_' && *in != '-') out[n++] = (char)tolower((unsigned char)*in);
  out[n] = 0;
}

static bool uset_white_space(int32_t c)
{
  return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
         c == 0x205f || c == 0x3000;
}

/* General_Category values by the order luna_uc_type() numbers them. */
static const char *const uset_gc_short[] = {
  "Cn", "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Me", "Mc", "Nd", "Nl", "No", "Zs", "Zl", "Zp",
  "Cc", "Cf", "Co", "Cs", "Pd", "Ps", "Pe", "Pc", "Po", "Sm", "Sc", "Sk", "So", "Pi", "Pf",
};
static const struct {
  const char *name, *abbr;
} uset_gc_long[] = {
  { "unassigned", "Cn" }, { "uppercaseletter", "Lu" }, { "lowercaseletter", "Ll" },
  { "titlecaseletter", "Lt" }, { "modifierletter", "Lm" }, { "otherletter", "Lo" },
  { "nonspacingmark", "Mn" }, { "enclosingmark", "Me" }, { "spacingmark", "Mc" },
  { "decimalnumber", "Nd" }, { "letternumber", "Nl" }, { "othernumber", "No" },
  { "spaceseparator", "Zs" }, { "lineseparator", "Zl" }, { "paragraphseparator", "Zp" },
  { "control", "Cc" }, { "format", "Cf" }, { "privateuse", "Co" }, { "surrogate", "Cs" },
  { "dashpunctuation", "Pd" }, { "openpunctuation", "Ps" }, { "closepunctuation", "Pe" },
  { "connectorpunctuation", "Pc" }, { "otherpunctuation", "Po" }, { "mathsymbol", "Sm" },
  { "currencysymbol", "Sc" }, { "modifiersymbol", "Sk" }, { "othersymbol", "So" },
  { "initialpunctuation", "Pi" }, { "finalpunctuation", "Pf" },
  { "letter", "L" }, { "casedletter", "LC" }, { "mark", "M" }, { "number", "N" },
  { "punctuation", "P" }, { "symbol", "S" }, { "separator", "Z" }, { "other", "C" },
};

/* Does `cat` (index into uset_gc_short) belong to the category `want` ("Lu", "L", "LC")? */
static bool uset_gc_match(int cat, const char *want)
{
  if (cat < 0 || cat >= (int)(sizeof uset_gc_short / sizeof *uset_gc_short)) return false;
  const char *have = uset_gc_short[cat];
  if (!strcasecmp(want, "LC")) return !strcmp(have, "Lu") || !strcmp(have, "Ll") || !strcmp(have, "Lt");
  if (strlen(want) == 1) return toupper((unsigned char)want[0]) == have[0];
  return !strcasecmp(want, have);
}

static const char *uset_gc_canonical(const char *value)
{
  char loose[64];
  uset_loose(value, loose, sizeof loose);
  for (size_t i = 0; i < sizeof uset_gc_long / sizeof *uset_gc_long; ++i)
    if (!strcmp(loose, uset_gc_long[i].name)) return uset_gc_long[i].abbr;
  for (size_t i = 0; i < sizeof uset_gc_short / sizeof *uset_gc_short; ++i)
    if (!strcasecmp(value, uset_gc_short[i])) return uset_gc_short[i];
  if (!strcasecmp(value, "LC")) return "LC";
  if (strlen(value) == 1 && strchr("LMNPSZClmnpszc", value[0])) {
    static char one[2];
    one[0] = (char)toupper((unsigned char)value[0]);
    one[1] = 0;
    return one;
  }
  return NULL;
}

/* Fills `s` with the property `name` (=`value`).  False: unknown property. */
static bool uset_property(struct uset *s, const char *name, const char *value)
{
  char n[64], v[64] = "";
  uset_loose(name, n, sizeof n);
  if (value) uset_loose(value, v, sizeof v);
  bool bin = !value;
  if (value && (!strcmp(n, "blk") || !strcmp(n, "block"))) {
    int32_t a, b;
    return luna_uc_block(value, &a, &b) && uset_add_range(s, a, b);
  }
  if (value && (!strcmp(n, "gcb") || !strcmp(n, "graphemeclusterbreak"))) {
    for (int32_t c = 0; c < USET_MAX;) {
      const char *g = luna_uc_grapheme_break(c);
      char gl[32];
      uset_loose(g, gl, sizeof gl);
      int32_t e = c;
      while (e + 1 < USET_MAX && luna_uc_grapheme_break(e + 1) == g) ++e;
      if (!strcmp(gl, v) || (!strcmp(v, "xx") && !strcmp(gl, "other"))) {
        if (!uset_add_range(s, c, e)) return false;
      }
      c = e + 1;
    }
    return true;
  }
  const char *gc = NULL;
  if (value && (!strcmp(n, "gc") || !strcmp(n, "generalcategory") || !strcmp(n, "category")))
    gc = uset_gc_canonical(value);
  else if (bin) gc = uset_gc_canonical(name);
  if (gc) {
    for (int32_t c = 0; c < USET_MAX; ++c)
      if (uset_gc_match(luna_uc_type(c), gc) && !uset_add_range(s, c, c)) return false;
    return true;
  }
  if (bin) {
    int pred = -1;
    if (!strcmp(n, "whitespace") || !strcmp(n, "wspace") || !strcmp(n, "space")) pred = 100;
    else if (!strcmp(n, "alphabetic") || !strcmp(n, "alpha")) pred = LUNA_UC_ALPHABETIC;
    else if (!strcmp(n, "ideographic") || !strcmp(n, "ideo")) pred = LUNA_UC_IDEOGRAPHIC;
    else if (!strcmp(n, "lowercase") || !strcmp(n, "lower")) pred = LUNA_UC_LOWERCASE;
    else if (!strcmp(n, "uppercase") || !strcmp(n, "upper")) pred = LUNA_UC_UPPERCASE;
    else if (!strcmp(n, "any")) return uset_add_range(s, 0, USET_MAX - 1);
    else if (!strcmp(n, "ascii")) return uset_add_range(s, 0, 0x7f);
    else if (!strcmp(n, "assigned")) pred = 101;
    if (pred < 0) return false;
    for (int32_t c = 0; c < USET_MAX; ++c) {
      const bool in = pred == 100 ? uset_white_space(c)
                      : pred == 101 ? luna_uc_type(c) != 0
                      : luna_uc_property(c, pred);
      if (in && !uset_add_range(s, c, c)) return false;
    }
    return true;
  }
  return false;
}

/* ---- pattern parser --------------------------------------------------------- */

struct uset_parse {
  const char *p, *start;
  char err[160];
};

static int32_t uset_next_cp(struct uset_parse *ps)
{
  const unsigned char *u = (const unsigned char *)ps->p;
  int32_t c;
  int len;
  if (u[0] < 0x80) {
    c = u[0];
    len = 1;
  } else if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
    c = ((u[0] & 0x1f) << 6) | (u[1] & 0x3f);
    len = 2;
  } else if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
    c = ((u[0] & 0x0f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f);
    len = 3;
  } else if ((u[0] & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80 && (u[3] & 0xc0) == 0x80) {
    c = ((u[0] & 0x07) << 18) | ((u[1] & 0x3f) << 12) | ((u[2] & 0x3f) << 6) | (u[3] & 0x3f);
    len = 4;
  } else {
    c = u[0];
    len = 1;
  }
  ps->p += len;
  return c;
}

static bool uset_fail(struct uset_parse *ps, const char *why)
{
  snprintf(ps->err, sizeof ps->err, "Error: %s at \"%s\"", why, ps->p);
  return false;
}

static int uset_hex(const char *p, int n)
{
  int v = 0;
  for (int i = 0; i < n; ++i) {
    if (!isxdigit((unsigned char)p[i])) return -1;
    v = v * 16 + (isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10));
  }
  return v;
}

static bool uset_parse_set(struct uset_parse *ps, struct uset *out);

/* \p{prop} / \P{prop} after the backslash and p/P; or [:prop:] after "[:". */
static bool uset_parse_property(struct uset_parse *ps, struct uset *out, bool negate, char close)
{
  char text[96];
  size_t n = 0;
  while (*ps->p && *ps->p != close && n + 1 < sizeof text) text[n++] = *ps->p++;
  text[n] = 0;
  if (close == '}') {
    if (*ps->p != '}') return uset_fail(ps, "missing } in property");
    ++ps->p;
  } else {
    if (ps->p[0] != ':' || ps->p[1] != ']') return uset_fail(ps, "missing :] in property");
    ps->p += 2;
  }
  char *eq = strchr(text, '=');
  if (!eq) eq = strchr(text, ':');   /* \p{Name:Value} */
  char *name = text, *value = NULL;
  if (eq) {
    *eq = 0;
    value = eq + 1;
  }
  if (name[0] == '^') {
    negate = !negate;
    ++name;
  }
  if (!uset_property(out, name, value)) {
    snprintf(ps->err, sizeof ps->err, "Invalid property pattern: %s%s%s", name, value ? "=" : "", value ? value : "");
    return false;
  }
  return !negate || uset_complement(out);
}

/* One element of a set: a code point (after escapes). */
static bool uset_parse_char(struct uset_parse *ps, int32_t *c)
{
  if (*ps->p != '\\') {
    *c = uset_next_cp(ps);
    return true;
  }
  ++ps->p;
  const char e = *ps->p;
  if (!e) return uset_fail(ps, "trailing backslash");
  int v;
  switch (e) {
  case 'u':
    v = uset_hex(ps->p + 1, 4);
    if (v < 0) return uset_fail(ps, "bad \\u escape");
    ps->p += 5;
    if (v >= 0xd800 && v < 0xdc00 && ps->p[0] == '\\' && ps->p[1] == 'u') {
      const int lo = uset_hex(ps->p + 2, 4);
      if (lo >= 0xdc00 && lo < 0xe000) {
        v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00);
        ps->p += 6;
      }
    }
    *c = v;
    return true;
  case 'U':
    v = uset_hex(ps->p + 1, 8);
    if (v < 0 || v >= USET_MAX) return uset_fail(ps, "bad \\U escape");
    ps->p += 9;
    *c = v;
    return true;
  case 'x':
    if (ps->p[1] == '{') {
      const char *close = strchr(ps->p, '}');
      if (!close) return uset_fail(ps, "bad \\x{} escape");
      v = uset_hex(ps->p + 2, (int)(close - ps->p - 2));
      if (v < 0 || v >= USET_MAX) return uset_fail(ps, "bad \\x{} escape");
      ps->p = close + 1;
    } else {
      v = uset_hex(ps->p + 1, 2);
      if (v < 0) return uset_fail(ps, "bad \\x escape");
      ps->p += 3;
    }
    *c = v;
    return true;
  case 't': *c = '\t'; break;
  case 'n': *c = '\n'; break;
  case 'r': *c = '\r'; break;
  case 'f': *c = '\f'; break;
  case 'a': *c = 7; break;
  case 'v': *c = 11; break;
  default:
    ++ps->p;
    ps->p--;
    *c = uset_next_cp(ps);
    return true;
  }
  ++ps->p;
  return true;
}

/* A bracketed set, a property, or nothing: fills `out` and returns true when an operand was read. */
static bool uset_parse_operand(struct uset_parse *ps, struct uset *out, bool *got)
{
  *got = true;
  if (ps->p[0] == '[') {
    if (ps->p[1] == ':') {
      ps->p += 2;
      const bool neg = *ps->p == '^';
      if (neg) ++ps->p;
      return uset_parse_property(ps, out, neg, ':');
    }
    return uset_parse_set(ps, out);
  }
  if (ps->p[0] == '\\' && (ps->p[1] == 'p' || ps->p[1] == 'P') && ps->p[2] == '{') {
    const bool neg = ps->p[1] == 'P';
    ps->p += 3;
    return uset_parse_property(ps, out, neg, '}');
  }
  *got = false;
  return true;
}

static bool uset_parse_set(struct uset_parse *ps, struct uset *out)
{
  if (*ps->p != '[') return uset_fail(ps, "missing [");
  ++ps->p;
  bool negate = false;
  if (*ps->p == '^') {
    negate = true;
    ++ps->p;
  }
  struct uset acc = { 0 };
  char pending_op = 0;   /* '&' or '-' waiting for its right-hand operand */
  bool ok = true;
  while (ok) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ++ps->p;
    if (!*ps->p) {
      ok = uset_fail(ps, "missing ]");
      break;
    }
    if (*ps->p == ']') {
      ++ps->p;
      break;
    }
    if (*ps->p == '|') {   /* an explicit union */
      ++ps->p;
      continue;
    }
    if ((*ps->p == '&' || (*ps->p == '-' && (ps->p[1] == '[' || ps->p[1] == '\\' || ps->p[1] == ':'))) && acc.n) {
      pending_op = *ps->p++;
      continue;
    }
    if (*ps->p == '{') {
      ok = uset_fail(ps, "sets of strings are not supported");
      break;
    }
    struct uset rhs = { 0 };
    bool got;
    if (!uset_parse_operand(ps, &rhs, &got)) {
      free(rhs.r);
      ok = false;
      break;
    }
    if (!got) {
      int32_t a, b;
      if (!uset_parse_char(ps, &a)) {
        free(rhs.r);
        ok = false;
        break;
      }
      b = a;
      if (*ps->p == '-' && ps->p[1] != ']' && ps->p[1] != '[') {
        ++ps->p;
        if (!uset_parse_char(ps, &b)) {
          free(rhs.r);
          ok = false;
          break;
        }
        if (b < a) {
          free(rhs.r);
          ok = uset_fail(ps, "invalid range");
          break;
        }
      }
      ok = uset_add_range(&rhs, a, b);
    }
    if (ok) {
      if (pending_op == '&') ok = uset_combine(&acc, &rhs, false);
      else if (pending_op == '-') ok = uset_combine(&acc, &rhs, true);
      else ok = uset_add_all(&acc, &rhs);
      pending_op = 0;
    }
    free(rhs.r);
  }
  if (ok && negate) ok = uset_complement(&acc);
  if (ok) {
    free(out->r);
    out->r = acc.r;
    out->n = acc.n;
    out->cap = acc.cap;
    /* A nested set unions into its parent through uset_add_all by the caller. */
  } else {
    free(acc.r);
  }
  return ok;
}

/* Replaces the contents of `s` with the set `pattern` describes; throws on error. */
static bool uset_apply_pattern(struct dvm *vm, struct uset *s, const char *pattern)
{
  struct uset_parse ps = { .p = pattern, .start = pattern };
  struct uset t = { 0 };
  while (*ps.p == ' ') ++ps.p;
  bool ok;
  if (ps.p[0] == '[' || (ps.p[0] == '\\' && (ps.p[1] == 'p' || ps.p[1] == 'P'))) {
    bool got;
    ok = uset_parse_operand(&ps, &t, &got);
  } else {
    ok = uset_fail(&ps, "pattern must start with [");
  }
  if (ok) {
    while (*ps.p == ' ') ++ps.p;
    if (*ps.p) ok = uset_fail(&ps, "unexpected trailing characters");
  }
  if (!ok) {
    free(t.r);
    dvm__throw(vm, "java/lang/IllegalArgumentException", "%s", ps.err);
    return false;
  }
  free(s->r);
  *s = (struct uset) { .r = t.r, .n = t.n, .cap = t.cap };
  return true;
}

/* ---- Java surface ------------------------------------------------------------ */

static bool uset_thaw_check(struct dvm *vm, const struct uset *s)
{
  if (!s->frozen) return true;
  dvm__throw(vm, "java/lang/UnsupportedOperationException", "Attempt to modify frozen object");
  return false;
}

static bool x_uset_init(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
  (void)out;
  struct uset *s = uset_of(vm, self);
  if (!s) return false;
  const char *sig = builtin_sig();
  if (!strcmp(sig, "(Ljava/lang/String;)V") || !strcmp(sig, "(Ljava/lang/String;Z)V")) {
    const char *p = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
    if (!p) {
      dvm__throw(vm, "java/lang/NullPointerException", "pattern");
      return false;
    }
    return uset_apply_pattern(vm, s, p);
  }
  if (!strcmp(sig, "(II)V")) {
    if (ARG(0).i < 0 || ARG(1).i > 0x10ffff || ARG(0).i > ARG(1).i) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Invalid range");
      return false;
    }
    if (!uset_add_range(s, ARG(0).i, ARG(1).i)) return false;
  }
  RETV();
}

/* Reads a CharSequence's code points. */
static int32_t *uset_cps(struct dvm *vm, dvm_ref cs, size_t *n)
{
  *n = 0;
  const char *u = cs ? rt_charseq_utf8(vm, cs) : NULL;
  if (!u) return NULL;
  int32_t *cps = malloc((strlen(u) + 1) * sizeof *cps);
  if (!cps) return NULL;
  struct uset_parse ps = { .p = u };
  while (*ps.p) cps[(*n)++] = uset_next_cp(&ps);
  return cps;
}

static bool x_uset_contains(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  struct uset *s = uset_of(vm, self);
  if (!s) return false;
  const char *sig = builtin_sig();
  if (!strcmp(sig, "(I)Z")) RETI(ARG(0).i >= 0 && uset_contains(s, ARG(0).i));
  if (!strcmp(sig, "(II)Z")) {
    /* every code point of [a, b] inside one range */
    const int32_t a = ARG(0).i, b = ARG(1).i;
    for (size_t i = 0; i < s->n; i += 2)
      if (s->r[i] <= a && b < s->r[i + 1]) RETI(1);
    RETI(0);
  }
  /* CharSequence: a set holds no strings, so only a single code point can be in it. */
  size_t n;
  int32_t *cps = uset_cps(vm, ARG(0).l, &n);
  const bool in = cps && n == 1 && uset_contains(s, cps[0]);
  free(cps);
  RETI(in);
}

static bool x_uset_contains_set(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  if (!s) return false;
  const char *name = builtin_name();
  const bool is_set = strstr(builtin_sig(), "UnicodeSet") != NULL;
  struct uset tmp = { 0 }, *o = NULL;
  size_t cn = 0;
  int32_t *cps = NULL;
  if (is_set) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "set");
      return false;
    }
    o = uset_of(vm, ARG(0).l);
  } else {
    cps = uset_cps(vm, ARG(0).l, &cn);
    for (size_t i = 0; i < cn; ++i)
      if (!uset_add_range(&tmp, cps[i], cps[i])) {
        free(cps);
        free(tmp.r);
        return false;
      }
    o = &tmp;
  }
  size_t inside = 0, total = uset_size(o);
  for (size_t i = 0; i < o->n; i += 2)
    for (int32_t c = o->r[i]; c < o->r[i + 1]; ++c)
      if (uset_contains(s, c)) ++inside;
  free(cps);
  free(tmp.r);
  if (!strcmp(name, "containsAll")) RETI(inside == total);
  if (!strcmp(name, "containsNone")) RETI(inside == 0);
  RETI(inside > 0);   /* containsSome */
}

static bool x_uset_mutate(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  if (!s || !uset_thaw_check(vm, s)) return false;
  const char *name = builtin_name(), *sig = builtin_sig();
  bool ok = true;
  const bool two = !strcmp(sig, "(II)Landroid/icu/text/UnicodeSet;");
  const bool one = !strcmp(sig, "(I)Landroid/icu/text/UnicodeSet;");
  const bool set = strstr(sig, "(Landroid/icu/text/UnicodeSet;)") != NULL;
  const bool cs = strstr(sig, "(Ljava/lang/CharSequence;)") != NULL;
  if (!strcmp(name, "complement") && !one && !two && !set && !cs) {
    ok = uset_complement(s);
  } else if (!strcmp(name, "clear")) {
    s->n = 0;
  } else if (!strcmp(name, "set")) {
    s->n = 0;
    ok = uset_add_range(s, ARG(0).i, ARG(1).i);
  } else if (one || two) {
    const int32_t a = ARG(0).i, b = two ? ARG(1).i : ARG(0).i;
    if (a < 0 || b > 0x10ffff || a > b) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Invalid code point or range");
      return false;
    }
    struct uset r = { 0 };
    ok = uset_add_range(&r, a, b);
    if (ok) {
      if (!strcmp(name, "add")) ok = uset_add_all(s, &r);
      else if (!strcmp(name, "remove")) ok = uset_combine(s, &r, true);
      else if (!strcmp(name, "retain")) ok = uset_combine(s, &r, false);
      else if (!strcmp(name, "complement")) {
        /* inside [a,b] what was out comes in and what was in goes out */
        struct uset keep = { 0 }, flip = { 0 };
        ok = uset_add_all(&keep, s) && uset_combine(&keep, &r, true) &&
             uset_add_all(&flip, &r) && uset_combine(&flip, s, true) && uset_add_all(&keep, &flip);
        if (ok) {
          free(s->r);
          s->r = keep.r;
          s->n = keep.n;
          s->cap = keep.cap;
        } else {
          free(keep.r);
        }
        free(flip.r);
      }
    }
    free(r.r);
  } else if (set || cs) {
    struct uset tmp = { 0 }, *o;
    int32_t *cps = NULL;
    if (set) {
      if (!ARG(0).l) {
        dvm__throw(vm, "java/lang/NullPointerException", "set");
        return false;
      }
      o = uset_of(vm, ARG(0).l);
    } else {
      size_t n;
      cps = uset_cps(vm, ARG(0).l, &n);
      for (size_t i = 0; ok && i < n; ++i) ok = uset_add_range(&tmp, cps[i], cps[i]);
      o = &tmp;
    }
    if (ok) {
      if (!strcmp(name, "add") || !strcmp(name, "addAll")) ok = uset_add_all(s, o);
      else if (!strcmp(name, "remove") || !strcmp(name, "removeAll")) ok = uset_combine(s, o, true);
      else if (!strcmp(name, "retain") || !strcmp(name, "retainAll")) ok = uset_combine(s, o, false);
      else if (!strcmp(name, "complementAll")) {
        struct uset common = { 0 };
        ok = uset_add_all(&common, s) && uset_combine(&common, o, false);
        struct uset extra = { 0 };
        ok = ok && uset_add_all(&extra, o) && uset_combine(&extra, &common, true);
        ok = ok && uset_combine(s, o, true) && uset_add_all(s, &extra);
        free(common.r);
        free(extra.r);
      }
    }
    free(cps);
    free(tmp.r);
  }
  if (!ok) return false;
  RETL(self);
}

static bool x_uset_apply(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  if (!s || !uset_thaw_check(vm, s)) return false;
  const char *p = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  if (!p) {
    dvm__throw(vm, "java/lang/NullPointerException", "pattern");
    return false;
  }
  if (!uset_apply_pattern(vm, s, p)) return false;
  RETL(self);
}

static bool x_uset_freeze(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  if (!s) return false;
  if (!strcmp(builtin_name(), "isFrozen")) RETI(s->frozen);
  s->frozen = true;
  RETL(self);
}

static bool x_uset_clone(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  struct dvm_object *o = dvm__obj(vm, self);
  if (!s || !o) return false;
  dvm_ref copy = dvm_new_object(vm, o->cls);
  struct uset *c = copy ? uset_of(vm, copy) : NULL;
  if (!c) return false;
  /* clone() of a frozen set is frozen; cloneAsThawed() is always editable. */
  if (!uset_add_all(c, s)) return false;
  c->frozen = s->frozen && !strcmp(builtin_name(), "clone");
  RETL(copy);
}

static bool x_uset_query(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  if (!s) return false;
  const char *name = builtin_name();
  if (!strcmp(name, "isEmpty")) RETI(s->n == 0);
  if (!strcmp(name, "size")) RETI(uset_size(s));
  if (!strcmp(name, "getRangeCount")) RETI(s->n / 2);
  if (!strcmp(name, "getRangeStart") || !strcmp(name, "getRangeEnd")) {
    const int i = ARG(0).i;
    if (i < 0 || (size_t)i >= s->n / 2) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "%d", i);
      return false;
    }
    RETI(!strcmp(name, "getRangeStart") ? s->r[i * 2] : s->r[i * 2 + 1] - 1);
  }
  if (!strcmp(name, "charAt")) {
    size_t at = (size_t)(ARG(0).i < 0 ? 0 : ARG(0).i);
    if (ARG(0).i < 0) RETI(-1);
    for (size_t i = 0; i < s->n; i += 2) {
      const size_t len = (size_t)(s->r[i + 1] - s->r[i]);
      if (at < len) RETI(s->r[i] + (int32_t)at);
      at -= len;
    }
    RETI(-1);
  }
  if (!strcmp(name, "indexOf")) {
    size_t at = 0;
    for (size_t i = 0; i < s->n; i += 2) {
      if (ARG(0).i >= s->r[i] && ARG(0).i < s->r[i + 1]) RETI((int32_t)(at + (size_t)(ARG(0).i - s->r[i])));
      at += (size_t)(s->r[i + 1] - s->r[i]);
    }
    RETI(-1);
  }
  /* hashCode: ICU folds each range's endpoints. */
  uint32_t h = 1;
  for (size_t i = 0; i < s->n; ++i) h = h * 1000003u + (uint32_t)s->r[i];
  RETI(h);
}

static bool x_uset_equals(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  struct dvm_object *o = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
  struct dvm_object *me = dvm__obj(vm, self);
  if (!s || !o || !me || o->cls != me->cls) RETI(0);
  struct uset *t = uset_of(vm, ARG(0).l);
  RETI(t && t->n == s->n && !memcmp(t->r, s->r, s->n * sizeof *s->r));
}

static bool uset_special(int32_t c)
{
  return c == '[' || c == ']' || c == '\\' || c == '&' || c == '-' || c == '^' || c == '{' ||
         c == '}' || c == ':' || c == '$' || c == '|';
}

/* Appends one code point of a pattern: specials backslash-escaped, controls and
 * surrogates as \uXXXX / \UXXXXXXXX, the rest as UTF-8. */
static char *uset_put_cp(char *buf, size_t *len, size_t *cap, int32_t c)
{
  if (*len + 16 > *cap) {
    *cap = *cap * 2 + 64;
    buf = realloc(buf, *cap);
    if (!buf) return NULL;
  }
  if (uset_special(c)) {
    buf[(*len)++] = '\\';
    buf[(*len)++] = (char)c;
  } else if (c <= ' ' || c == 0x7f || (c >= 0x80 && c < 0xa0) || (c >= 0xd800 && c < 0xe000)) {
    *len += (size_t)snprintf(buf + *len, 16, "\\u%04X", (unsigned)c);
  } else if (c < 0x80) {
    buf[(*len)++] = (char)c;
  } else if (c < 0x800) {
    buf[(*len)++] = (char)(0xc0 | (c >> 6));
    buf[(*len)++] = (char)(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    buf[(*len)++] = (char)(0xe0 | (c >> 12));
    buf[(*len)++] = (char)(0x80 | ((c >> 6) & 0x3f));
    buf[(*len)++] = (char)(0x80 | (c & 0x3f));
  } else {
    buf[(*len)++] = (char)(0xf0 | (c >> 18));
    buf[(*len)++] = (char)(0x80 | ((c >> 12) & 0x3f));
    buf[(*len)++] = (char)(0x80 | ((c >> 6) & 0x3f));
    buf[(*len)++] = (char)(0x80 | (c & 0x3f));
  }
  return buf;
}

static bool x_uset_to_pattern(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  (void)args;
  (void)nargs;
  struct uset *s = uset_of(vm, self);
  if (!s) return false;
  size_t len = 0, cap = 0;
  char *buf = uset_put_cp(NULL, &len, &cap, 'x');
  if (!buf) return false;
  len = 0;
  buf[len++] = '[';
  for (size_t i = 0; i < s->n && buf; i += 2) {
    const int32_t a = s->r[i], b = s->r[i + 1] - 1;
    buf = uset_put_cp(buf, &len, &cap, a);
    if (buf && b > a) {
      if (b > a + 1) buf[len++] = '-';
      buf = uset_put_cp(buf, &len, &cap, b);
    }
  }
  if (!buf) return false;
  buf[len++] = ']';
  buf[len] = 0;
  dvm_ref str = dvm_new_string(vm, buf);
  free(buf);
  RETL(str);
}

#define USET "Landroid/icu/text/UnicodeSet;"
static const struct rt_method rt_uset[] = {
  M("<init>", "()V", x_uset_init),
  M("<init>", "(Ljava/lang/String;)V", x_uset_init),
  M("<init>", "(Ljava/lang/String;Z)V", x_uset_init),
  M("<init>", "(II)V", x_uset_init),
  M("contains", "(I)Z", x_uset_contains),
  M("contains", "(II)Z", x_uset_contains),
  M("contains", "(Ljava/lang/CharSequence;)Z", x_uset_contains),
  M("containsAll", "(" USET ")Z", x_uset_contains_set),
  M("containsAll", "(Ljava/lang/CharSequence;)Z", x_uset_contains_set),
  M("containsNone", "(" USET ")Z", x_uset_contains_set),
  M("containsNone", "(Ljava/lang/CharSequence;)Z", x_uset_contains_set),
  M("containsSome", "(" USET ")Z", x_uset_contains_set),
  M("containsSome", "(Ljava/lang/CharSequence;)Z", x_uset_contains_set),
  M("add", "(I)" USET, x_uset_mutate),
  M("add", "(II)" USET, x_uset_mutate),
  M("add", "(Ljava/lang/CharSequence;)" USET, x_uset_mutate),
  M("addAll", "(" USET ")" USET, x_uset_mutate),
  M("addAll", "(Ljava/lang/CharSequence;)" USET, x_uset_mutate),
  M("remove", "(I)" USET, x_uset_mutate),
  M("remove", "(II)" USET, x_uset_mutate),
  M("remove", "(Ljava/lang/CharSequence;)" USET, x_uset_mutate),
  M("removeAll", "(" USET ")" USET, x_uset_mutate),
  M("removeAll", "(Ljava/lang/CharSequence;)" USET, x_uset_mutate),
  M("retain", "(I)" USET, x_uset_mutate),
  M("retain", "(II)" USET, x_uset_mutate),
  M("retainAll", "(" USET ")" USET, x_uset_mutate),
  M("retainAll", "(Ljava/lang/CharSequence;)" USET, x_uset_mutate),
  M("complement", "()" USET, x_uset_mutate),
  M("complement", "(I)" USET, x_uset_mutate),
  M("complement", "(II)" USET, x_uset_mutate),
  M("complementAll", "(" USET ")" USET, x_uset_mutate),
  M("clear", "()" USET, x_uset_mutate),
  M("set", "(II)" USET, x_uset_mutate),
  M("applyPattern", "(Ljava/lang/String;)" USET, x_uset_apply),
  M("freeze", "()" USET, x_uset_freeze),
  M("isFrozen", "()Z", x_uset_freeze),
  M("clone", "()Ljava/lang/Object;", x_uset_clone),
  M("cloneAsThawed", "()" USET, x_uset_clone),
  M("isEmpty", "()Z", x_uset_query),
  M("size", "()I", x_uset_query),
  M("getRangeCount", "()I", x_uset_query),
  M("getRangeStart", "(I)I", x_uset_query),
  M("getRangeEnd", "(I)I", x_uset_query),
  M("charAt", "(I)I", x_uset_query),
  M("indexOf", "(I)I", x_uset_query),
  M("hashCode", "()I", x_uset_query),
  M("equals", "(Ljava/lang/Object;)Z", x_uset_equals),
  M("toPattern", "(Z)Ljava/lang/String;", x_uset_to_pattern),
  M("toString", "()Ljava/lang/String;", x_uset_to_pattern),
  M_END,
};
#undef USET
