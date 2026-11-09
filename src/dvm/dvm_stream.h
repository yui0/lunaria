/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* java.util.stream (eager), java.util.Optional.
 *
 * A Stream here is a snapshot: every intermediate operation runs to
 * completion over an ArrayList and returns a new Stream holding the result.
 * That is observably the same as the lazy JDK pipeline for finite sources
 * without side effects that depend on interleaving, which is what the
 * application code that reaches for stream() does (map/filter/collect).
 * Included from dvm_runtime.c before rt_list_methods. */

#define ST_LIST_DESC "Ljava/util/ArrayList;"

static struct rt_list *st_items(struct dvm *vm, dvm_ref self)
{
  union dvm_value v = { 0 };
  if (!dvm_get_field(vm, self, "items", "Ljava/util/ArrayList;", &v) || !v.l)
    return NULL;
  return list_of(vm, v.l);
}

static dvm_ref st_new(struct dvm *vm, const char *desc, dvm_ref list)
{
  struct dvm_class *c = dvm__class_by_desc(vm, desc);
  dvm_ref s = c ? dvm_new_object(vm, c) : 0;
  if (!s) return 0;
  union dvm_value v = { .l = list };
  (void)dvm_set_field(vm, s, "items", "Ljava/util/ArrayList;", v);
  return s;
}

static bool st_add(struct dvm *vm, dvm_ref list, dvm_ref item)
{
  union dvm_value a = { .l = item }, ignored = { 0 };
  return list_add(vm, list, &a, 1, &ignored);
}

/* Items of any Collection / Iterable, copied into a fresh ArrayList. */
static dvm_ref st_snapshot(struct dvm *vm, dvm_ref source)
{
  dvm_ref list = coll_new_list(vm, ST_LIST_DESC);
  union dvm_value it = { 0 };
  if (!list) return 0;
  if (!am_vcall(vm, source, "iterator", "()Ljava/util/Iterator;", NULL, 0, &it))
    return 0;
  for (;;) {
    union dvm_value has = { 0 }, next = { 0 };
    if (!am_vcall(vm, it.l, "hasNext", "()Z", NULL, 0, &has)) return 0;
    if (!has.i) break;
    if (!am_vcall(vm, it.l, "next", "()Ljava/lang/Object;", NULL, 0, &next))
      return 0;
    if (!st_add(vm, list, next.l)) return 0;
  }
  return list;
}

static bool coll_stream(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  dvm_ref list = st_snapshot(vm, self);
  if (!list) return false;
  RETL(st_new(vm, "Ljava/util/stream/Stream;", list));
}

static bool st_apply1(struct dvm *vm, dvm_ref fn, const char *name,
                      const char *sig, dvm_ref arg, union dvm_value *res)
{
  union dvm_value a = { .l = arg };
  return am_vcall(vm, fn, name, sig, &a, 1, res);
}

static const char *st_text(struct dvm *vm, dvm_ref s)
{
  return s ? dvm_string_utf8(vm, s) : NULL;
}

/* ---- Optional ---------------------------------------------------------- */

static dvm_ref opt_make(struct dvm *vm, dvm_ref value)
{
  struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/util/Optional;");
  dvm_ref o = c ? dvm_new_object(vm, c) : 0;
  if (!o) return 0;
  union dvm_value v = { .l = value };
  (void)dvm_set_field(vm, o, "value", "Ljava/lang/Object;", v);
  return o;
}

static dvm_ref opt_value(struct dvm *vm, dvm_ref self)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "value", "Ljava/lang/Object;", &v);
  return v.l;
}

static bool opt_of(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
  (void)self;
  if (!strcmp(builtin_name(), "of") && !ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "value");
    return false;
  }
  RETL(opt_make(vm, ARG(0).l));
}

static bool opt_empty(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  RETL(opt_make(vm, 0));
}

static bool opt_isPresent(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  bool present = opt_value(vm, self) != 0;
  RETI(!strcmp(builtin_name(), "isEmpty") ? !present : present);
}

static bool opt_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
  dvm_ref v = opt_value(vm, self);
  if (v) RETL(v);
  if (nargs > 0 && ARG(0).l) {
    union dvm_value ex = { 0 };
    if (!am_vcall(vm, ARG(0).l, "get", "()Ljava/lang/Object;", NULL, 0, &ex))
      return false;
    vm->exception = ex.l;
    return false;
  }
  dvm__throw(vm, "java/util/NoSuchElementException", "No value present");
  return false;
}

static bool opt_orElse(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
  (void)nargs;
  dvm_ref v = opt_value(vm, self);
  if (v) RETL(v);
  if (!strcmp(builtin_name(), "orElseGet")) {
    union dvm_value r = { 0 };
    if (!am_vcall(vm, ARG(0).l, "get", "()Ljava/lang/Object;", NULL, 0, &r))
      return false;
    RETL(r.l);
  }
  RETL(ARG(0).l);
}

static bool opt_ifPresent(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  dvm_ref v = opt_value(vm, self);
  union dvm_value ignored = { 0 };
  if (v && !st_apply1(vm, ARG(0).l, "accept", "(Ljava/lang/Object;)V", v, &ignored))
    return false;
  RETV();
}

static bool opt_map(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
  (void)nargs;
  dvm_ref v = opt_value(vm, self);
  union dvm_value r = { 0 };
  if (!v) RETL(opt_make(vm, 0));
  if (!strcmp(builtin_name(), "filter")) {
    if (!st_apply1(vm, ARG(0).l, "test", "(Ljava/lang/Object;)Z", v, &r)) return false;
    RETL(r.i ? self : opt_make(vm, 0));
  }
  if (!st_apply1(vm, ARG(0).l, "apply", "(Ljava/lang/Object;)Ljava/lang/Object;", v, &r))
    return false;
  if (!strcmp(builtin_name(), "flatMap")) RETL(r.l);
  RETL(opt_make(vm, r.l));
}

static bool opt_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  dvm_ref v = opt_value(vm, self);
  if (!v) RETL(dvm_new_string(vm, "Optional.empty"));
  union dvm_value s = { 0 };
  if (!am_vcall(vm, v, "toString", "()Ljava/lang/String;", NULL, 0, &s)) return false;
  char buf[512];
  snprintf(buf, sizeof buf, "Optional[%s]", st_text(vm, s.l) ? st_text(vm, s.l) : "null");
  RETL(dvm_new_string(vm, buf));
}

static const struct rt_method rt_optional[] = {
  SM("of", "(Ljava/lang/Object;)Ljava/util/Optional;", opt_of),
  SM("ofNullable", "(Ljava/lang/Object;)Ljava/util/Optional;", opt_of),
  SM("empty", "()Ljava/util/Optional;", opt_empty),
  M("isPresent", "()Z", opt_isPresent),
  M("isEmpty", "()Z", opt_isPresent),
  M("get", "()Ljava/lang/Object;", opt_get),
  M("orElseThrow", "()Ljava/lang/Object;", opt_get),
  M("orElseThrow", "(Ljava/util/function/Supplier;)Ljava/lang/Object;", opt_get),
  M("orElse", "(Ljava/lang/Object;)Ljava/lang/Object;", opt_orElse),
  M("orElseGet", "(Ljava/util/function/Supplier;)Ljava/lang/Object;", opt_orElse),
  M("ifPresent", "(Ljava/util/function/Consumer;)V", opt_ifPresent),
  M("map", "(Ljava/util/function/Function;)Ljava/util/Optional;", opt_map),
  M("flatMap", "(Ljava/util/function/Function;)Ljava/util/Optional;", opt_map),
  M("filter", "(Ljava/util/function/Predicate;)Ljava/util/Optional;", opt_map),
  M("toString", "()Ljava/lang/String;", opt_toString),
  M_END,
};
static const struct rt_field rt_optional_fields[] = {
  { "value", "Ljava/lang/Object;" }, F_END
};

/* ---- Stream ------------------------------------------------------------ */

#define ST_FN "(Ljava/lang/Object;)Ljava/lang/Object;"

static bool st_filter_map(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  bool filter = !strcmp(name, "filter"), flat = !strcmp(name, "flatMap"),
       peek = !strcmp(name, "peek"), to_int = !strcmp(name, "mapToInt");
  struct rt_list *src = st_items(vm, self);
  dvm_ref list = coll_new_list(vm, ST_LIST_DESC);
  if (!src || !list || !ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "%s", name);
    return false;
  }
  for (uint32_t i = 0; i < src->size; ++i) {
    dvm_ref item = src->items[i];
    union dvm_value r = { 0 };
    if (filter) {
      if (!st_apply1(vm, ARG(0).l, "test", "(Ljava/lang/Object;)Z", item, &r)) return false;
      if (r.i && !st_add(vm, list, item)) return false;
    } else if (peek) {
      if (!st_apply1(vm, ARG(0).l, "accept", "(Ljava/lang/Object;)V", item, &r) ||
          !st_add(vm, list, item)) return false;
    } else if (to_int) {
      if (!st_apply1(vm, ARG(0).l, "applyAsInt", "(Ljava/lang/Object;)I", item, &r))
        return false;
      if (!st_add(vm, list, box_make(vm, "Ljava/lang/Integer;", r))) return false;
    } else {
      if (!st_apply1(vm, ARG(0).l, "apply", ST_FN, item, &r)) return false;
      if (flat) {
        struct rt_list *inner = r.l ? st_items(vm, r.l) : NULL;
        for (uint32_t j = 0; inner && j < inner->size; ++j)
          if (!st_add(vm, list, inner->items[j])) return false;
      } else if (!st_add(vm, list, r.l)) return false;
    }
  }
  RETL(st_new(vm, to_int ? "Ljava/util/stream/IntStream;" : "Ljava/util/stream/Stream;", list));
}

static bool st_for_each(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  struct rt_list *src = st_items(vm, self);
  union dvm_value ignored = { 0 };
  for (uint32_t i = 0; src && i < src->size; ++i)
    if (!st_apply1(vm, ARG(0).l, "accept", "(Ljava/lang/Object;)V", src->items[i], &ignored))
      return false;
  RETV();
}

static bool st_match(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  bool any = !strcmp(name, "anyMatch"), all = !strcmp(name, "allMatch");
  struct rt_list *src = st_items(vm, self);
  for (uint32_t i = 0; src && i < src->size; ++i) {
    union dvm_value r = { 0 };
    if (!st_apply1(vm, ARG(0).l, "test", "(Ljava/lang/Object;)Z", src->items[i], &r))
      return false;
    if (any && r.i) RETI(1);
    if (all && !r.i) RETI(0);
    if (!any && !all && r.i) RETI(0); /* noneMatch */
  }
  RETI(any ? 0 : 1);
}

static bool st_find_first(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct rt_list *src = st_items(vm, self);
  RETL(opt_make(vm, src && src->size ? src->items[0] : 0));
}

static bool st_count(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct rt_list *src = st_items(vm, self);
  RETJ(src ? src->size : 0);
}

static bool st_self(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
  (void)vm; (void)args; (void)nargs;
  RETL(self);
}

static bool st_limit_skip(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)nargs;
  bool limit = !strcmp(builtin_name(), "limit");
  struct rt_list *src = st_items(vm, self);
  int64_t n = ARG(0).j;
  if (n < 0) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "%lld", (long long)n);
    return false;
  }
  dvm_ref list = coll_new_list(vm, ST_LIST_DESC);
  uint32_t size = src ? src->size : 0;
  uint32_t from = limit ? 0 : (n > size ? size : (uint32_t)n);
  uint32_t to = limit ? (n > size ? size : (uint32_t)n) : size;
  for (uint32_t i = from; list && i < to; ++i)
    if (!st_add(vm, list, src->items[i])) return false;
  RETL(st_new(vm, "Ljava/util/stream/Stream;", list));
}

/* Objects.equals(a, b) through the receiver's own equals(). */
static bool st_equals(struct dvm *vm, dvm_ref a, dvm_ref b, bool *eq)
{
  if (a == b || !a || !b) { *eq = a == b; return true; }
  union dvm_value arg = { .l = b }, r = { 0 };
  if (!am_vcall(vm, a, "equals", "(Ljava/lang/Object;)Z", &arg, 1, &r)) return false;
  *eq = r.i != 0;
  return true;
}

static bool st_distinct(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct rt_list *src = st_items(vm, self);
  dvm_ref list = coll_new_list(vm, ST_LIST_DESC);
  for (uint32_t i = 0; list && src && i < src->size; ++i) {
    struct rt_list *dst = list_of(vm, list);
    bool seen = false;
    for (uint32_t j = 0; dst && j < dst->size && !seen; ++j)
      if (!st_equals(vm, dst->items[j], src->items[i], &seen)) return false;
    if (!seen && !st_add(vm, list, src->items[i])) return false;
  }
  RETL(st_new(vm, "Ljava/util/stream/Stream;", list));
}

/* Stable merge sort; `cmp` is a Comparator or NULL for Comparable order. */
static bool st_compare(struct dvm *vm, dvm_ref cmp, dvm_ref a, dvm_ref b, int *order)
{
  union dvm_value args[2] = { { .l = a }, { .l = b } }, r = { 0 };
  bool ok = cmp
    ? am_vcall(vm, cmp, "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", args, 2, &r)
    : am_vcall(vm, a, "compareTo", "(Ljava/lang/Object;)I", &args[1], 1, &r);
  *order = r.i;
  return ok;
}

static bool st_msort(struct dvm *vm, dvm_ref cmp, dvm_ref *a, dvm_ref *tmp, uint32_t n)
{
  if (n < 2) return true;
  uint32_t mid = n / 2, i = 0, j = mid, k = 0;
  if (!st_msort(vm, cmp, a, tmp, mid) || !st_msort(vm, cmp, a + mid, tmp, n - mid))
    return false;
  while (i < mid && j < n) {
    int order;
    if (!st_compare(vm, cmp, a[j], a[i], &order)) return false;
    tmp[k++] = order < 0 ? a[j++] : a[i++];
  }
  while (i < mid) tmp[k++] = a[i++];
  while (j < n) tmp[k++] = a[j++];
  memcpy(a, tmp, n * sizeof *a);
  return true;
}

static bool st_sorted(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
  struct rt_list *src = st_items(vm, self);
  dvm_ref list = coll_new_list(vm, ST_LIST_DESC);
  uint32_t n = src ? src->size : 0;
  dvm_ref *buf = malloc((n ? n : 1) * 2 * sizeof *buf);
  if (!buf || !list) {
    free(buf);
    dvm__throw(vm, "java/lang/OutOfMemoryError", "sorted");
    return false;
  }
  if (n) memcpy(buf, src->items, n * sizeof *buf);
  bool ok = st_msort(vm, nargs > 0 ? ARG(0).l : 0, buf, buf + n, n);
  for (uint32_t i = 0; ok && i < n; ++i) ok = st_add(vm, list, buf[i]);
  free(buf);
  if (!ok) return false;
  RETL(st_new(vm, "Ljava/util/stream/Stream;", list));
}

static bool st_min_max(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
  (void)nargs;
  bool max = !strcmp(builtin_name(), "max");
  struct rt_list *src = st_items(vm, self);
  dvm_ref best = 0;
  for (uint32_t i = 0; src && i < src->size; ++i) {
    int order = 0;
    if (i && !st_compare(vm, ARG(0).l, src->items[i], best, &order)) return false;
    if (!i || (max ? order > 0 : order < 0)) best = src->items[i];
  }
  RETL(opt_make(vm, best));
}

static bool st_reduce(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
  struct rt_list *src = st_items(vm, self);
  dvm_ref acc = nargs > 1 ? ARG(0).l : 0;
  dvm_ref op = nargs > 1 ? ARG(1).l : ARG(0).l;
  for (uint32_t i = 0; src && i < src->size; ++i) {
    if (nargs == 1 && i == 0) { acc = src->items[0]; continue; }
    union dvm_value ab[2] = { { .l = acc }, { .l = src->items[i] } }, r = { 0 };
    if (!am_vcall(vm, op, "apply", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                  ab, 2, &r)) return false;
    acc = r.l;
  }
  RETL(nargs > 1 ? acc : opt_make(vm, acc));
}

static bool st_to_array(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct rt_list *src = st_items(vm, self);
  uint32_t n = src ? src->size : 0;
  dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/Object;", n);
  for (uint32_t i = 0; arr && i < n; ++i)
    ((dvm_ref *)dvm_array_data(vm, arr))[i] = src->items[i];
  RETL(arr);
}

static bool st_to_list(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct rt_list *src = st_items(vm, self);
  dvm_ref list = coll_new_list(vm, ST_LIST_DESC);
  for (uint32_t i = 0; list && src && i < src->size; ++i)
    if (!st_add(vm, list, src->items[i])) return false;
  RETL(list);
}

static bool st_iterator(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  union dvm_value list = { 0 };
  if (!st_to_list(vm, self, args, nargs, &list) || !list.l) return false;
  return am_vcall(vm, list.l, "iterator", "()Ljava/util/Iterator;", NULL, 0, out);
}

static bool st_of(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                  int nargs, union dvm_value *out)
{
  (void)self;
  dvm_ref list = coll_new_list(vm, ST_LIST_DESC);
  if (nargs == 1 && ARG(0).l && dvm_object_class(vm, ARG(0).l) &&
      dvm_object_class(vm, ARG(0).l)->is_array) {
    uint32_t n = dvm_array_length(vm, ARG(0).l);
    for (uint32_t i = 0; list && i < n; ++i)
      if (!st_add(vm, list, ((dvm_ref *)dvm_array_data(vm, ARG(0).l))[i])) return false;
  } else if (nargs == 1 && list && !st_add(vm, list, ARG(0).l)) return false;
  RETL(st_new(vm, "Ljava/util/stream/Stream;", list));
}

/* ---- Collector(s) ------------------------------------------------------ */

enum { CK_LIST = 1, CK_SET, CK_JOIN, CK_COLLECTION, CK_MAP };

static dvm_ref cl_make(struct dvm *vm, int kind, dvm_ref a, dvm_ref b, dvm_ref c, dvm_ref d)
{
  struct dvm_class *k = dvm__class_by_desc(vm, "Ljava/util/stream/Collector;");
  dvm_ref o = k ? dvm_new_object(vm, k) : 0;
  if (!o) return 0;
  union dvm_value v = { .i = kind };
  (void)dvm_set_field(vm, o, "kind", "I", v);
  v.l = a; (void)dvm_set_field(vm, o, "a", "Ljava/lang/Object;", v);
  v.l = b; (void)dvm_set_field(vm, o, "b", "Ljava/lang/Object;", v);
  v.l = c; (void)dvm_set_field(vm, o, "c", "Ljava/lang/Object;", v);
  v.l = d; (void)dvm_set_field(vm, o, "d", "Ljava/lang/Object;", v);
  return o;
}

static bool cls_make(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
  (void)self;
  const char *name = builtin_name();
  if (!strcmp(name, "toList")) RETL(cl_make(vm, CK_LIST, 0, 0, 0, 0));
  if (!strcmp(name, "toSet")) RETL(cl_make(vm, CK_SET, 0, 0, 0, 0));
  if (!strcmp(name, "toCollection")) RETL(cl_make(vm, CK_COLLECTION, ARG(0).l, 0, 0, 0));
  if (!strcmp(name, "joining"))
    RETL(cl_make(vm, CK_JOIN, nargs > 0 ? ARG(0).l : 0, nargs > 1 ? ARG(1).l : 0,
                 nargs > 2 ? ARG(2).l : 0, 0));
  RETL(cl_make(vm, CK_MAP, ARG(0).l, ARG(1).l, nargs > 2 ? ARG(2).l : 0, 0));
}

static dvm_ref cl_field(struct dvm *vm, dvm_ref o, const char *name)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, o, name, "Ljava/lang/Object;", &v);
  return v.l;
}

static bool st_run_collector(struct dvm *vm, dvm_ref collector, struct rt_list *src,
                             union dvm_value *out)
{
  union dvm_value kind = { 0 };
  (void)dvm_get_field(vm, collector, "kind", "I", &kind);
  dvm_ref a = cl_field(vm, collector, "a"), b = cl_field(vm, collector, "b"),
          c = cl_field(vm, collector, "c");
  uint32_t n = src ? src->size : 0;
  if (kind.i == CK_LIST || kind.i == CK_SET || kind.i == CK_COLLECTION) {
    dvm_ref target;
    if (kind.i == CK_COLLECTION) {
      union dvm_value r = { 0 };
      if (!am_vcall(vm, a, "get", "()Ljava/lang/Object;", NULL, 0, &r)) return false;
      target = r.l;
    } else {
      target = coll_new_list(vm, kind.i == CK_LIST ? ST_LIST_DESC
                                                   : "Ljava/util/HashSet;");
    }
    for (uint32_t i = 0; target && i < n; ++i) {
      union dvm_value arg = { .l = src->items[i] }, ignored = { 0 };
      if (!am_vcall(vm, target, "add", "(Ljava/lang/Object;)Z", &arg, 1, &ignored))
        return false;
    }
    RETL(target);
  }
  if (kind.i == CK_JOIN) {
    const char *delim = a ? st_text(vm, a) : "", *prefix = b ? st_text(vm, b) : "",
               *suffix = c ? st_text(vm, c) : "";
    if (!delim) delim = "";
    if (!prefix) prefix = "";
    if (!suffix) suffix = "";
    size_t cap = strlen(prefix) + strlen(suffix) + 1, len = 0;
    char *buf = malloc(cap);
    if (!buf) { dvm__throw(vm, "java/lang/OutOfMemoryError", "joining"); return false; }
    len = snprintf(buf, cap, "%s", prefix);
    for (uint32_t i = 0; i < n; ++i) {
      union dvm_value s = { 0 };
      if (!am_vcall(vm, src->items[i], "toString", "()Ljava/lang/String;", NULL, 0, &s)) {
        free(buf);
        return false;
      }
      const char *t = st_text(vm, s.l) ? st_text(vm, s.l) : "null";
      size_t add = strlen(t) + (i ? strlen(delim) : 0);
      char *grown = realloc(buf, cap + add);
      if (!grown) { free(buf); dvm__throw(vm, "java/lang/OutOfMemoryError", "joining"); return false; }
      buf = grown;
      cap += add;
      if (i) { memcpy(buf + len, delim, strlen(delim)); len += strlen(delim); }
      memcpy(buf + len, t, strlen(t));
      len += strlen(t);
    }
    memcpy(buf + len, suffix, strlen(suffix) + 1);
    dvm_ref s = dvm_new_string(vm, buf);
    free(buf);
    RETL(s);
  }
  /* CK_MAP: toMap(keyFn, valueFn[, merge]) */
  dvm_ref map = coll_new_map(vm);
  for (uint32_t i = 0; map && i < n; ++i) {
    union dvm_value k = { 0 }, v = { 0 }, has = { 0 }, ignored = { 0 };
    if (!st_apply1(vm, a, "apply", ST_FN, src->items[i], &k) ||
        !st_apply1(vm, b, "apply", ST_FN, src->items[i], &v)) return false;
    union dvm_value key = { .l = k.l };
    if (!am_vcall(vm, map, "containsKey", "(Ljava/lang/Object;)Z", &key, 1, &has))
      return false;
    if (has.i) {
      if (!c) {
        dvm__throw(vm, "java/lang/IllegalStateException", "Duplicate key");
        return false;
      }
      union dvm_value old = { 0 }, ab[2];
      if (!am_vcall(vm, map, "get", ST_FN, &key, 1, &old)) return false;
      ab[0].l = old.l; ab[1].l = v.l;
      if (!am_vcall(vm, c, "apply", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                    ab, 2, &v)) return false;
    }
    union dvm_value kv[2] = { { .l = k.l }, { .l = v.l } };
    if (!map_put(vm, map, kv, 2, &ignored)) return false;
  }
  RETL(map);
}

static bool st_collect(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
  struct rt_list *src = st_items(vm, self);
  if (nargs == 1) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "collector");
      return false;
    }
    return st_run_collector(vm, ARG(0).l, src, out);
  }
  /* collect(Supplier, BiConsumer accumulator, BiConsumer combiner) */
  union dvm_value container = { 0 };
  if (!am_vcall(vm, ARG(0).l, "get", "()Ljava/lang/Object;", NULL, 0, &container))
    return false;
  for (uint32_t i = 0; src && i < src->size; ++i) {
    union dvm_value ab[2] = { { .l = container.l }, { .l = src->items[i] } }, ignored = { 0 };
    if (!am_vcall(vm, ARG(1).l, "accept", "(Ljava/lang/Object;Ljava/lang/Object;)V",
                  ab, 2, &ignored)) return false;
  }
  RETL(container.l);
}

/* ---- IntStream (just what the sum/boxing callers need) ------------------ */

static bool ist_sum(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct rt_list *src = st_items(vm, self);
  int32_t sum = 0;
  for (uint32_t i = 0; src && i < src->size; ++i) {
    union dvm_value v = { 0 };
    if (!am_vcall(vm, src->items[i], "intValue", "()I", NULL, 0, &v)) return false;
    sum += v.i;
  }
  RETI(sum);
}

static bool ist_boxed(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value list = { 0 };
  if (!st_to_list(vm, self, args, nargs, &list)) return false;
  RETL(st_new(vm, "Ljava/util/stream/Stream;", list.l));
}

static bool ist_to_array(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct rt_list *src = st_items(vm, self);
  uint32_t n = src ? src->size : 0;
  dvm_ref arr = dvm_new_array(vm, 'I', NULL, n);
  for (uint32_t i = 0; arr && i < n; ++i) {
    union dvm_value v = { 0 };
    if (!am_vcall(vm, src->items[i], "intValue", "()I", NULL, 0, &v)) return false;
    ((int32_t *)dvm_array_data(vm, arr))[i] = v.i;
  }
  RETL(arr);
}

static const struct rt_method rt_stream[] = {
  M("filter", "(Ljava/util/function/Predicate;)Ljava/util/stream/Stream;", st_filter_map),
  M("map", "(Ljava/util/function/Function;)Ljava/util/stream/Stream;", st_filter_map),
  M("flatMap", "(Ljava/util/function/Function;)Ljava/util/stream/Stream;", st_filter_map),
  M("peek", "(Ljava/util/function/Consumer;)Ljava/util/stream/Stream;", st_filter_map),
  M("mapToInt", "(Ljava/util/function/ToIntFunction;)Ljava/util/stream/IntStream;", st_filter_map),
  M("forEach", "(Ljava/util/function/Consumer;)V", st_for_each),
  M("forEachOrdered", "(Ljava/util/function/Consumer;)V", st_for_each),
  M("anyMatch", "(Ljava/util/function/Predicate;)Z", st_match),
  M("allMatch", "(Ljava/util/function/Predicate;)Z", st_match),
  M("noneMatch", "(Ljava/util/function/Predicate;)Z", st_match),
  M("findFirst", "()Ljava/util/Optional;", st_find_first),
  M("findAny", "()Ljava/util/Optional;", st_find_first),
  M("count", "()J", st_count),
  M("limit", "(J)Ljava/util/stream/Stream;", st_limit_skip),
  M("skip", "(J)Ljava/util/stream/Stream;", st_limit_skip),
  M("distinct", "()Ljava/util/stream/Stream;", st_distinct),
  M("sorted", "()Ljava/util/stream/Stream;", st_sorted),
  M("sorted", "(Ljava/util/Comparator;)Ljava/util/stream/Stream;", st_sorted),
  M("min", "(Ljava/util/Comparator;)Ljava/util/Optional;", st_min_max),
  M("max", "(Ljava/util/Comparator;)Ljava/util/Optional;", st_min_max),
  M("reduce", "(Ljava/lang/Object;Ljava/util/function/BinaryOperator;)Ljava/lang/Object;", st_reduce),
  M("reduce", "(Ljava/util/function/BinaryOperator;)Ljava/util/Optional;", st_reduce),
  M("toArray", "()[Ljava/lang/Object;", st_to_array),
  M("toList", "()Ljava/util/List;", st_to_list),
  M("iterator", "()Ljava/util/Iterator;", st_iterator),
  M("collect", "(Ljava/util/stream/Collector;)Ljava/lang/Object;", st_collect),
  M("collect", "(Ljava/util/function/Supplier;Ljava/util/function/BiConsumer;"
               "Ljava/util/function/BiConsumer;)Ljava/lang/Object;", st_collect),
  M("parallel", "()Ljava/util/stream/Stream;", st_self),
  M("sequential", "()Ljava/util/stream/Stream;", st_self),
  M("unordered", "()Ljava/util/stream/Stream;", st_self),
  M("onClose", "(Ljava/lang/Runnable;)Ljava/util/stream/Stream;", st_self),
  M("close", "()V", nop_void),
  SM("of", "([Ljava/lang/Object;)Ljava/util/stream/Stream;", st_of),
  SM("of", "(Ljava/lang/Object;)Ljava/util/stream/Stream;", st_of),
  SM("empty", "()Ljava/util/stream/Stream;", st_of),
  M_END,
};

static const struct rt_method rt_int_stream[] = {
  M("sum", "()I", ist_sum),
  M("count", "()J", st_count),
  M("toArray", "()[I", ist_to_array),
  M("boxed", "()Ljava/util/stream/Stream;", ist_boxed),
  M_END,
};

static const struct rt_field rt_stream_fields[] = {
  { "items", "Ljava/util/ArrayList;" }, F_END
};

static const struct rt_method rt_collectors[] = {
  SM("toList", "()Ljava/util/stream/Collector;", cls_make),
  SM("toSet", "()Ljava/util/stream/Collector;", cls_make),
  SM("toCollection", "(Ljava/util/function/Supplier;)Ljava/util/stream/Collector;", cls_make),
  SM("joining", "()Ljava/util/stream/Collector;", cls_make),
  SM("joining", "(Ljava/lang/CharSequence;)Ljava/util/stream/Collector;", cls_make),
  SM("joining", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;Ljava/lang/CharSequence;)"
                "Ljava/util/stream/Collector;", cls_make),
  SM("toMap", "(Ljava/util/function/Function;Ljava/util/function/Function;)"
              "Ljava/util/stream/Collector;", cls_make),
  SM("toMap", "(Ljava/util/function/Function;Ljava/util/function/Function;"
              "Ljava/util/function/BinaryOperator;)Ljava/util/stream/Collector;", cls_make),
  M_END,
};

static const struct rt_field rt_collector_fields[] = {
  { "kind", "I" }, { "a", "Ljava/lang/Object;" }, { "b", "Ljava/lang/Object;" },
  { "c", "Ljava/lang/Object;" }, { "d", "Ljava/lang/Object;" }, F_END
};
