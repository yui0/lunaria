/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The part of the class library the interpreter has to own.
 *
 * An APK ships its own classes, not the platform's, so `java/lang/String` and
 * friends have no bytecode anywhere in the dex.  Routing those to the JNI stub
 * layer would not work either: `StringBuilder.append` is what every string
 * concatenation in the app compiles to, and it has to return the *same*
 * builder so the next append chains.  So the VM implements them directly.
 *
 * Scope is set by what app glue code actually uses: strings and builders,
 * boxing, Math, System, the exception hierarchy, and small collections.
 * Anything else stays an external class and goes to the host stubs.
 */

#include "dvm/dvm_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARG(i) (((i) < nargs) ? args[i] : (union dvm_value){ 0 })
#define RETI(v) do { out->i = (int32_t)(v); return true; } while (0)
#define RETJ(v) do { out->j = (int64_t)(v); return true; } while (0)
#define RETF(v) do { out->f = (float)(v); return true; } while (0)
#define RETD(v) do { out->d = (double)(v); return true; } while (0)
#define RETL(v) do { out->l = (dvm_ref)(v); return true; } while (0)
#define RETV() do { return true; } while (0)

static const char *sref(struct dvm *vm, dvm_ref r)
{
   const char *s = dvm_string_utf8(vm, r);
   return s ? s : "null";
}

/* ------------------------------------------------------------------------ *
 * java.lang.Object
 * ------------------------------------------------------------------------ */

static bool o_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs; (void)out;
   RETV();
}

static bool o_hashCode(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *s = dvm_string_utf8(vm, self);
   if (s) {
      /* String.hashCode is specified, and app code stores it in tables. */
      int32_t h = 0;
      for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
         h = (int32_t)((uint32_t)h * 31u + *p);
      RETI(h);
   }
   RETI((int32_t)self);
}

static bool o_equals(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   dvm_ref other = ARG(0).l;
   const char *a = dvm_string_utf8(vm, self), *b = dvm_string_utf8(vm, other);
   if (a && b) RETI(!strcmp(a, b) ? 1 : 0);
   RETI(self == other ? 1 : 0);
}

static bool o_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   if (dvm_string_utf8(vm, self)) RETL(self);
   struct dvm_class *c = dvm_object_class(vm, self);
   char buf[256];
   snprintf(buf, sizeof buf, "%s@%x", c ? c->name : "java/lang/Object", self);
   RETL(dvm_new_string(vm, buf));
}

static dvm_ref class_object_for(struct dvm *vm, struct dvm_class *cls);

static bool o_getClass(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(class_object_for(vm, dvm_object_class(vm, self)));
}

static bool nop_void(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs; (void)out;
   RETV();
}

/* ------------------------------------------------------------------------ *
 * java.lang.String
 * ------------------------------------------------------------------------ */

static bool s_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETV();
   o->kind = DVM_OBJ_STRING;
   free(o->utf8);
   o->utf8 = NULL;
   o->utf8_len = 0;

   if (nargs >= 1) {
      const char *src = dvm_string_utf8(vm, ARG(0).l);
      if (src) {
         o->utf8 = strdup(src);
         o->utf8_len = (uint32_t)strlen(src);
      } else {
         /* new String(byte[]) / new String(char[]) — the two forms app code
          * uses to turn native data into text. */
         struct dvm_object *a = dvm__obj(vm, ARG(0).l);
         if (a && a->kind == DVM_OBJ_ARRAY) {
            uint32_t off = 0, len = a->length;
            if (nargs >= 3) { off = ARG(1).u; len = ARG(2).u; }
            if (off > a->length) off = a->length;
            if (off + len > a->length) len = a->length - off;
            if (a->elem_kind == 'C') {
               o->utf8 = malloc((size_t)len * 4u + 1u);
               char *w = o->utf8;
               const uint16_t *cs = (const uint16_t *)a->data;
               for (uint32_t i = 0; w && i < len; ++i) {
                  uint16_t c = cs[off + i];
                  if (c < 0x80) *w++ = (char)c;
                  else if (c < 0x800) { *w++ = (char)(0xc0 | (c >> 6)); *w++ = (char)(0x80 | (c & 0x3f)); }
                  else { *w++ = (char)(0xe0 | (c >> 12)); *w++ = (char)(0x80 | ((c >> 6) & 0x3f)); *w++ = (char)(0x80 | (c & 0x3f)); }
               }
               if (w) { *w = '\0'; o->utf8_len = (uint32_t)(w - o->utf8); }
            } else {
               o->utf8 = malloc((size_t)len + 1u);
               if (o->utf8) {
                  memcpy(o->utf8, (const uint8_t *)a->data + off, len);
                  o->utf8[len] = '\0';
                  o->utf8_len = len;
               }
            }
         }
      }
   }
   if (!o->utf8) { o->utf8 = strdup(""); o->utf8_len = 0; }
   RETV();
}

static bool s_length(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   /* Java counts UTF-16 units.  Everything an APK's glue code passes around is
    * ASCII in practice, but count properly so an accented path name does not
    * make a substring() land mid-character. */
   const char *s = dvm_string_utf8(vm, self);
   if (!s) RETI(0);
   int32_t n = 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; ) {
      if (*p < 0x80) { ++p; ++n; }
      else if ((*p & 0xe0) == 0xc0) { p += 2; ++n; }
      else if ((*p & 0xf0) == 0xe0) { p += 3; ++n; }
      else { p += 4; n += 2; }
      if (!p[-1] && p[-1] != *s) break;
   }
   RETI(n);
}

/* Byte offset of UTF-16 index `idx`. */
static size_t utf16_to_byte(const char *s, int32_t idx)
{
   const unsigned char *p = (const unsigned char *)s;
   int32_t n = 0;
   while (*p && n < idx) {
      if (*p < 0x80) ++p;
      else if ((*p & 0xe0) == 0xc0) p += 2;
      else if ((*p & 0xf0) == 0xe0) p += 3;
      else { p += 4; ++n; }
      ++n;
   }
   return (size_t)((const char *)p - s);
}

static bool s_charAt(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   const char *s = dvm_string_utf8(vm, self);
   int32_t i = ARG(0).i;
   if (!s || i < 0) {
      dvm__throw(vm, "java/lang/StringIndexOutOfBoundsException", "index %d", i);
      return false;
   }
   size_t b = utf16_to_byte(s, i);
   if (b >= strlen(s)) {
      dvm__throw(vm, "java/lang/StringIndexOutOfBoundsException", "index %d", i);
      return false;
   }
   const unsigned char *p = (const unsigned char *)s + b;
   uint32_t cp;
   if (*p < 0x80) cp = *p;
   else if ((*p & 0xe0) == 0xc0) cp = (uint32_t)(*p & 0x1f) << 6 | (p[1] & 0x3f);
   else cp = (uint32_t)(*p & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
   RETI(cp & 0xffff);
}

static bool s_isEmpty(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *s = dvm_string_utf8(vm, self);
   RETI(!s || !*s);
}

static bool s_equalsIgnoreCase(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   const char *a = dvm_string_utf8(vm, self), *b = dvm_string_utf8(vm, ARG(0).l);
   if (!a || !b) RETI(0);
   while (*a && *b) {
      int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
      int cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
      if (ca != cb) RETI(0);
      ++a; ++b;
   }
   RETI(!*a && !*b);
}

static bool s_compareTo(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   const char *a = dvm_string_utf8(vm, self), *b = dvm_string_utf8(vm, ARG(0).l);
   RETI(strcmp(a ? a : "", b ? b : ""));
}

static bool s_indexOf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   const char *s = dvm_string_utf8(vm, self);
   if (!s) RETI(-1);
   char one[2] = { 0, 0 };
   const char *needle = dvm_string_utf8(vm, ARG(0).l);
   if (!needle) { one[0] = (char)ARG(0).i; needle = one; }
   size_t from = (nargs >= 2 && ARG(1).i > 0) ? utf16_to_byte(s, ARG(1).i) : 0;
   if (from > strlen(s)) RETI(-1);
   const char *hit = strstr(s + from, needle);
   if (!hit) RETI(-1);
   /* Convert the byte offset back to a UTF-16 index. */
   int32_t idx = 0;
   for (const unsigned char *p = (const unsigned char *)s;
        (const char *)p < hit; ) {
      if (*p < 0x80) ++p;
      else if ((*p & 0xe0) == 0xc0) p += 2;
      else if ((*p & 0xf0) == 0xe0) p += 3;
      else { p += 4; ++idx; }
      ++idx;
   }
   RETI(idx);
}

static bool s_lastIndexOf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   const char *s = dvm_string_utf8(vm, self);
   if (!s) RETI(-1);
   char one[2] = { 0, 0 };
   const char *needle = dvm_string_utf8(vm, ARG(0).l);
   if (!needle) { one[0] = (char)ARG(0).i; needle = one; }
   size_t nl = strlen(needle);
   if (!nl) RETI((int32_t)strlen(s));
   const char *best = NULL;
   for (const char *p = s; (p = strstr(p, needle)); ++p) best = p;
   RETI(best ? (int32_t)(best - s) : -1);
}

static bool s_substring(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   const char *s = dvm_string_utf8(vm, self);
   if (!s) RETL(dvm_new_string(vm, ""));
   size_t len = strlen(s);
   int32_t b = ARG(0).i;
   size_t bb = utf16_to_byte(s, b < 0 ? 0 : b);
   size_t eb = len;
   if (nargs >= 2) eb = utf16_to_byte(s, ARG(1).i);
   if (bb > len || eb > len || bb > eb) {
      dvm__throw(vm, "java/lang/StringIndexOutOfBoundsException",
                 "begin %d end %d length %zu", ARG(0).i,
                 nargs >= 2 ? ARG(1).i : (int32_t)len, len);
      return false;
   }
   RETL(dvm_new_string_n(vm, s + bb, eb - bb));
}

static bool s_concat(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   const char *a = sref(vm, self), *b = sref(vm, ARG(0).l);
   size_t la = strlen(a), lb = strlen(b);
   char *buf = malloc(la + lb + 1);
   if (!buf) RETL(self);
   memcpy(buf, a, la);
   memcpy(buf + la, b, lb);
   buf[la + lb] = '\0';
   dvm_ref r = dvm_new_string_n(vm, buf, la + lb);
   free(buf);
   RETL(r);
}

static bool s_trim(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *s = dvm_string_utf8(vm, self);
   if (!s) RETL(self);
   const char *b = s, *e = s + strlen(s);
   while (b < e && (unsigned char)*b <= ' ') ++b;
   while (e > b && (unsigned char)e[-1] <= ' ') --e;
   RETL(dvm_new_string_n(vm, b, (size_t)(e - b)));
}

static bool s_case(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out, bool upper)
{
   (void)args; (void)nargs;
   const char *s = dvm_string_utf8(vm, self);
   if (!s) RETL(self);
   size_t n = strlen(s);
   char *buf = malloc(n + 1);
   if (!buf) RETL(self);
   for (size_t i = 0; i < n; ++i) {
      char c = s[i];
      if (upper && c >= 'a' && c <= 'z') c = (char)(c - 32);
      else if (!upper && c >= 'A' && c <= 'Z') c = (char)(c + 32);
      buf[i] = c;
   }
   buf[n] = '\0';
   dvm_ref r = dvm_new_string_n(vm, buf, n);
   free(buf);
   RETL(r);
}

static bool s_toLowerCase(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   return s_case(vm, self, args, nargs, out, false);
}

static bool s_toUpperCase(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   return s_case(vm, self, args, nargs, out, true);
}

static bool s_startsWith(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *s = sref(vm, self), *p = sref(vm, ARG(0).l);
   RETI(!strncmp(s, p, strlen(p)));
}

static bool s_endsWith(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *s = sref(vm, self), *p = sref(vm, ARG(0).l);
   size_t ls = strlen(s), lp = strlen(p);
   RETI(lp <= ls && !memcmp(s + ls - lp, p, lp));
}

static bool s_contains(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)nargs;
   RETI(strstr(sref(vm, self), sref(vm, ARG(0).l)) != NULL);
}

static bool s_replace(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   const char *s = dvm_string_utf8(vm, self);
   if (!s) RETL(self);
   const char *from = dvm_string_utf8(vm, ARG(0).l);
   const char *to = dvm_string_utf8(vm, ARG(1).l);
   char fb[2] = { (char)ARG(0).i, 0 }, tb[2] = { (char)ARG(1).i, 0 };
   if (!from) from = fb;
   if (!to) to = tb;
   size_t lf = strlen(from), lt = strlen(to);
   if (!lf) RETL(self);

   size_t cap = strlen(s) + 1, len = 0;
   char *buf = malloc(cap);
   if (!buf) RETL(self);
   for (const char *p = s; *p; ) {
      const char *hit = strstr(p, from);
      size_t chunk = hit ? (size_t)(hit - p) : strlen(p);
      size_t need = len + chunk + (hit ? lt : 0) + 1;
      if (need > cap) {
         cap = need * 2;
         char *nb = realloc(buf, cap);
         if (!nb) { free(buf); RETL(self); }
         buf = nb;
      }
      memcpy(buf + len, p, chunk);
      len += chunk;
      if (!hit) break;
      memcpy(buf + len, to, lt);
      len += lt;
      p = hit + lf;
   }
   buf[len] = '\0';
   dvm_ref r = dvm_new_string_n(vm, buf, len);
   free(buf);
   RETL(r);
}

static bool s_getBytes(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *s = sref(vm, self);
   size_t n = strlen(s);
   dvm_ref a = dvm_new_array(vm, 'B', "B", (uint32_t)n);
   void *d = dvm_array_data(vm, a);
   if (d) memcpy(d, s, n);
   RETL(a);
}

static bool s_toCharArray(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *s = sref(vm, self);
   size_t n = strlen(s);
   dvm_ref a = dvm_new_array(vm, 'C', "C", (uint32_t)n);
   uint16_t *d = dvm_array_data(vm, a);
   if (d) for (size_t i = 0; i < n; ++i) d[i] = (uint8_t)s[i];
   RETL(a);
}

static bool s_intern(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(dvm__intern(vm, sref(vm, self)));
}

/* String.valueOf — one entry per primitive, plus the Object form which has to
 * go through the argument's own toString() so an app class renders itself. */
static bool s_valueOf_obj(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)self;
   if (nargs < 1 || !ARG(0).l) RETL(dvm__intern(vm, "null"));
   dvm_ref o = ARG(0).l;
   if (dvm_string_utf8(vm, o)) RETL(o);
   struct dvm_class *c = dvm_object_class(vm, o);
   struct dvm_method *ts = c ? dvm_find_method(vm, c, "toString", "()Ljava/lang/String;") : NULL;
   if (ts) {
      union dvm_value ret;
      if (dvm_call(vm, ts, o, NULL, 0, &ret)) RETL(ret.l);
   }
   RETL(dvm__intern(vm, "null"));
}

#define S_VALUEOF(name, fmt, expr) \
   static bool name(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                    int nargs, union dvm_value *out) \
   { \
      (void)self; (void)nargs; \
      char buf[64]; \
      snprintf(buf, sizeof buf, fmt, expr); \
      RETL(dvm_new_string(vm, buf)); \
   }

S_VALUEOF(s_valueOf_int, "%d", ARG(0).i)
S_VALUEOF(s_valueOf_long, "%lld", (long long)ARG(0).j)
S_VALUEOF(s_valueOf_float, "%g", (double)ARG(0).f)
S_VALUEOF(s_valueOf_double, "%g", ARG(0).d)

static bool s_valueOf_bool(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   RETL(dvm__intern(vm, ARG(0).i ? "true" : "false"));
}

static bool s_valueOf_char(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   char buf[2] = { (char)(ARG(0).u & 0x7f), 0 };
   RETL(dvm_new_string(vm, buf));
}

/* ------------------------------------------------------------------------ *
 * java.lang.StringBuilder / StringBuffer
 *
 * Content lives in the object's own utf8 buffer, so teardown is one free().
 * ------------------------------------------------------------------------ */

static void sb_append(struct dvm *vm, dvm_ref self, const char *text, size_t n)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) return;
   size_t have = o->utf8 ? o->utf8_len : 0;
   char *nb = realloc(o->utf8, have + n + 1);
   if (!nb) return;
   memcpy(nb + have, text, n);
   nb[have + n] = '\0';
   o->utf8 = nb;
   o->utf8_len = (uint32_t)(have + n);
}

static bool sb_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETV();
   free(o->utf8);
   o->utf8 = strdup("");
   o->utf8_len = 0;
   if (nargs >= 1) {
      const char *s = dvm_string_utf8(vm, ARG(0).l);
      if (s) sb_append(vm, self, s, strlen(s));
   }
   RETV();
}

#define SB_APPEND_FN(name, body) \
   static bool name(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                    int nargs, union dvm_value *out) \
   { char buf[64]; (void)buf; (void)nargs; body; RETL(self); }

SB_APPEND_FN(sb_append_str, {
   const char *s = dvm_string_utf8(vm, ARG(0).l);
   if (!s) {
      /* append(Object) on something that is not a String: use its toString. */
      struct dvm_object *o = dvm__obj(vm, ARG(0).l);
      if (!ARG(0).l) s = "null";
      else if (o && o->utf8) s = o->utf8;
      else { snprintf(buf, sizeof buf, "%s@%x",
                      o && o->cls ? o->cls->name : "Object", ARG(0).l); s = buf; }
   }
   sb_append(vm, self, s, strlen(s));
})
SB_APPEND_FN(sb_append_int, {
   snprintf(buf, sizeof buf, "%d", ARG(0).i);
   sb_append(vm, self, buf, strlen(buf));
})
SB_APPEND_FN(sb_append_long, {
   snprintf(buf, sizeof buf, "%lld", (long long)ARG(0).j);
   sb_append(vm, self, buf, strlen(buf));
})
SB_APPEND_FN(sb_append_bool, {
   const char *s = ARG(0).i ? "true" : "false";
   sb_append(vm, self, s, strlen(s));
})
SB_APPEND_FN(sb_append_char, {
   uint32_t c = ARG(0).u & 0xffff;
   if (c < 0x80) { buf[0] = (char)c; sb_append(vm, self, buf, 1); }
   else if (c < 0x800) { buf[0] = (char)(0xc0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3f)); sb_append(vm, self, buf, 2); }
   else { buf[0] = (char)(0xe0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 0x3f)); buf[2] = (char)(0x80 | (c & 0x3f)); sb_append(vm, self, buf, 3); }
})
SB_APPEND_FN(sb_append_float, {
   snprintf(buf, sizeof buf, "%g", (double)ARG(0).f);
   sb_append(vm, self, buf, strlen(buf));
})
SB_APPEND_FN(sb_append_double, {
   snprintf(buf, sizeof buf, "%g", ARG(0).d);
   sb_append(vm, self, buf, strlen(buf));
})

static bool sb_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   RETL(dvm_new_string_n(vm, o && o->utf8 ? o->utf8 : "", o ? o->utf8_len : 0));
}

static bool sb_length(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   RETI(o ? (int32_t)o->utf8_len : 0);
}

static bool sb_setLength(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   int32_t n = ARG(0).i;
   if (o && o->utf8 && n >= 0 && (uint32_t)n <= o->utf8_len) {
      o->utf8[n] = '\0';
      o->utf8_len = (uint32_t)n;
   }
   RETV();
}

static bool sb_reverse(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   if (o && o->utf8)
      for (uint32_t i = 0, j = o->utf8_len ? o->utf8_len - 1 : 0; i < j; ++i, --j) {
         char t = o->utf8[i]; o->utf8[i] = o->utf8[j]; o->utf8[j] = t;
      }
   RETL(self);
}

/* ------------------------------------------------------------------------ *
 * Boxed primitives
 *
 * The value lives in the object's first instance slot, declared as a real
 * field so `iget` on it works too.
 * ------------------------------------------------------------------------ */

static bool box_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)out; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   if (o && o->slots) o->slots[0] = ARG(0);
   RETV();
}

static union dvm_value box_get(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   return (o && o->slots) ? o->slots[0] : (union dvm_value){ 0 };
}

static dvm_ref box_make(struct dvm *vm, const char *desc, union dvm_value v)
{
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   dvm_ref r = dvm_new_object(vm, c);
   struct dvm_object *o = dvm__obj(vm, r);
   if (o && o->slots) o->slots[0] = v;
   return r;
}

#define BOX_VALUEOF(name, desc) \
   static bool name(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                    int nargs, union dvm_value *out) \
   { (void)self; (void)nargs; RETL(box_make(vm, desc, ARG(0))); }

BOX_VALUEOF(i_valueOf, "Ljava/lang/Integer;")
BOX_VALUEOF(l_valueOf, "Ljava/lang/Long;")
BOX_VALUEOF(f_valueOf, "Ljava/lang/Float;")
BOX_VALUEOF(d_valueOf, "Ljava/lang/Double;")
BOX_VALUEOF(b_valueOf, "Ljava/lang/Boolean;")
BOX_VALUEOF(c_valueOf, "Ljava/lang/Character;")
BOX_VALUEOF(sh_valueOf, "Ljava/lang/Short;")
BOX_VALUEOF(by_valueOf, "Ljava/lang/Byte;")

static bool box_intValue(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(box_get(vm, self).i);
}
static bool box_longValue(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETJ(box_get(vm, self).j);
}
static bool box_floatValue(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETF(box_get(vm, self).f);
}
static bool box_doubleValue(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETD(box_get(vm, self).d);
}

static bool i_parseInt(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)self;
   const char *s = dvm_string_utf8(vm, ARG(0).l);
   int base = (nargs >= 2) ? ARG(1).i : 10;
   if (!s) { dvm__throw(vm, "java/lang/NumberFormatException", "null"); return false; }
   char *end = NULL;
   long v = strtol(s, &end, base);
   if (end == s) { dvm__throw(vm, "java/lang/NumberFormatException", "%s", s); return false; }
   RETI((int32_t)v);
}

static bool l_parseLong(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)self;
   const char *s = dvm_string_utf8(vm, ARG(0).l);
   int base = (nargs >= 2) ? ARG(1).i : 10;
   if (!s) { dvm__throw(vm, "java/lang/NumberFormatException", "null"); return false; }
   char *end = NULL;
   long long v = strtoll(s, &end, base);
   if (end == s) { dvm__throw(vm, "java/lang/NumberFormatException", "%s", s); return false; }
   RETJ(v);
}

static bool f_parseFloat(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *s = dvm_string_utf8(vm, ARG(0).l);
   RETF(s ? strtof(s, NULL) : 0.0f);
}

static bool d_parseDouble(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *s = dvm_string_utf8(vm, ARG(0).l);
   RETD(s ? strtod(s, NULL) : 0.0);
}

static bool i_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   char buf[32];
   if (nargs >= 1) snprintf(buf, sizeof buf, "%d", ARG(0).i);
   else snprintf(buf, sizeof buf, "%d", box_get(vm, self).i);
   RETL(dvm_new_string(vm, buf));
}

static bool l_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   char buf[32];
   if (nargs >= 1) snprintf(buf, sizeof buf, "%lld", (long long)ARG(0).j);
   else snprintf(buf, sizeof buf, "%lld", (long long)box_get(vm, self).j);
   RETL(dvm_new_string(vm, buf));
}

static bool i_toHexString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   char buf[32];
   snprintf(buf, sizeof buf, "%x", ARG(0).u);
   RETL(dvm_new_string(vm, buf));
}

static bool b_booleanValue(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(box_get(vm, self).i ? 1 : 0);
}

static bool c_charValue(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(box_get(vm, self).u & 0xffff);
}

/* ------------------------------------------------------------------------ *
 * java.lang.Math
 * ------------------------------------------------------------------------ */

#define MATH1D(name, expr) \
   static bool name(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                    int nargs, union dvm_value *out) \
   { (void)vm; (void)self; (void)nargs; double x = ARG(0).d; RETD(expr); }

MATH1D(m_sqrt, sqrt(x))
MATH1D(m_floor, floor(x))
MATH1D(m_ceil, ceil(x))
MATH1D(m_sin, sin(x))
MATH1D(m_cos, cos(x))
MATH1D(m_tan, tan(x))
MATH1D(m_asin, asin(x))
MATH1D(m_acos, acos(x))
MATH1D(m_atan, atan(x))
MATH1D(m_log, log(x))
MATH1D(m_log10, log10(x))
MATH1D(m_exp, exp(x))
MATH1D(m_cbrt, cbrt(x))
MATH1D(m_absd, fabs(x))

static bool m_pow(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                  int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETD(pow(ARG(0).d, ARG(1).d));
}
static bool m_atan2(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETD(atan2(ARG(0).d, ARG(1).d));
}
static bool m_hypot(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETD(hypot(ARG(0).d, ARG(1).d));
}
static bool m_absi(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   int32_t v = ARG(0).i;
   RETI(v < 0 ? -v : v);
}
static bool m_absl(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   int64_t v = ARG(0).j;
   RETJ(v < 0 ? -v : v);
}
static bool m_absf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETF(fabsf(ARG(0).f));
}
static bool m_mini(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI(ARG(0).i < ARG(1).i ? ARG(0).i : ARG(1).i);
}
static bool m_maxi(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI(ARG(0).i > ARG(1).i ? ARG(0).i : ARG(1).i);
}
static bool m_minl(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETJ(ARG(0).j < ARG(1).j ? ARG(0).j : ARG(1).j);
}
static bool m_maxl(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETJ(ARG(0).j > ARG(1).j ? ARG(0).j : ARG(1).j);
}
static bool m_minf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETF(fminf(ARG(0).f, ARG(1).f));
}
static bool m_maxf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETF(fmaxf(ARG(0).f, ARG(1).f));
}
static bool m_mind(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETD(fmin(ARG(0).d, ARG(1).d));
}
static bool m_maxd(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETD(fmax(ARG(0).d, ARG(1).d));
}
static bool m_roundf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI((int32_t)floorf(ARG(0).f + 0.5f));
}
static bool m_roundd(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETJ((int64_t)floor(ARG(0).d + 0.5));
}
static bool m_random(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETD((double)rand() / ((double)RAND_MAX + 1.0));
}

/* ------------------------------------------------------------------------ *
 * java.lang.System
 * ------------------------------------------------------------------------ */

static bool sys_currentTimeMillis(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   RETJ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static bool sys_nanoTime(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   RETJ((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
}

static bool sys_arraycopy(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)self; (void)nargs; (void)out;
   struct dvm_object *src = dvm__obj(vm, ARG(0).l);
   struct dvm_object *dst = dvm__obj(vm, ARG(2).l);
   int32_t sp = ARG(1).i, dp = ARG(3).i, n = ARG(4).i;
   if (!src || !dst || src->kind != DVM_OBJ_ARRAY || dst->kind != DVM_OBJ_ARRAY) {
      dvm__throw(vm, "java/lang/NullPointerException", "arraycopy");
      return false;
   }
   if (n < 0 || sp < 0 || dp < 0 ||
       (uint32_t)(sp + n) > src->length || (uint32_t)(dp + n) > dst->length) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException",
                 "arraycopy src=%u dst=%u srcPos=%d dstPos=%d length=%d",
                 src->length, dst->length, sp, dp, n);
      return false;
   }
   int w;
   switch (src->elem_kind) {
      case 'Z': case 'B': w = 1; break;
      case 'C': case 'S': w = 2; break;
      case 'J': case 'D': w = 8; break;
      default: w = 4; break;
   }
   memmove((uint8_t *)dst->data + (size_t)dp * w,
           (const uint8_t *)src->data + (size_t)sp * w, (size_t)n * w);
   RETV();
}

static bool sys_identityHashCode(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   RETI((int32_t)ARG(0).l);
}

static bool sys_getProperty(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self;
   const char *k = dvm_string_utf8(vm, ARG(0).l);
   /* Only the handful an APK's glue code branches on.  Anything else falls
    * through to the default argument, or null. */
   static const struct { const char *k, *v; } props[] = {
      { "line.separator", "\n" },
      { "file.separator", "/" },
      { "path.separator", ":" },
      { "java.vm.name", "Lunaria DVM" },
      { "java.vendor", "Project Lunaria" },
      { "os.name", "Linux" },
      { "os.arch", "aarch64" },
   };
   if (k) for (size_t i = 0; i < sizeof props / sizeof props[0]; ++i)
      if (!strcmp(k, props[i].k)) RETL(dvm_new_string(vm, props[i].v));
   RETL(nargs >= 2 ? ARG(1).l : 0);
}

/* ------------------------------------------------------------------------ *
 * java.io.PrintStream — System.out / System.err
 * ------------------------------------------------------------------------ */

static bool ps_println(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)self; (void)out;
   if (nargs < 1) { fprintf(stderr, "[dvm:out]\n"); RETV(); }
   const char *s = dvm_string_utf8(vm, ARG(0).l);
   if (s) fprintf(stderr, "[dvm:out] %s\n", s);
   else fprintf(stderr, "[dvm:out] %d\n", ARG(0).i);
   RETV();
}

/* ------------------------------------------------------------------------ *
 * java.lang.Class
 * ------------------------------------------------------------------------ */

static dvm_ref class_object_for(struct dvm *vm, struct dvm_class *cls)
{
   if (!cls) return 0;
   if (cls->class_object) return cls->class_object;
   dvm_ref r = dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/lang/Class;"));
   struct dvm_object *o = dvm__obj(vm, r);
   if (o) { o->kind = DVM_OBJ_CLASS; o->klass = cls; ++o->pins; }
   cls->class_object = r;
   return r;
}

static bool cl_getName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   const char *n = (o && o->klass) ? o->klass->name : "java.lang.Object";
   char buf[512];
   snprintf(buf, sizeof buf, "%s", n);
   for (char *p = buf; *p; ++p) if (*p == '/') *p = '.';
   RETL(dvm_new_string(vm, buf));
}

static bool cl_getSimpleName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   const char *n = (o && o->klass) ? o->klass->name : "Object";
   const char *slash = strrchr(n, '/');
   RETL(dvm_new_string(vm, slash ? slash + 1 : n));
}

static bool cl_forName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *n = dvm_string_utf8(vm, ARG(0).l);
   if (!n) { dvm__throw(vm, "java/lang/ClassNotFoundException", "null"); return false; }
   char desc[512];
   snprintf(desc, sizeof desc, "L%s;", n);
   for (char *p = desc; *p; ++p) if (*p == '.') *p = '/';
   RETL(class_object_for(vm, dvm__class_by_desc(vm, desc)));
}

/* ------------------------------------------------------------------------ *
 * java.lang.Enum
 *
 * Every enum in the APK is a dex class that extends this one, and its
 * generated constructor calls `super(name, ordinal)` before anything else.
 * With no definition here that super call went to the host stub layer, which
 * has nowhere to put the two values — so every constant ended up nameless
 * with ordinal 0, and name()/ordinal()/compareTo()/valueOf() all agreed on
 * the wrong answer.  The state is per-instance, so it has to live in the VM
 * for the same reason StringBuilder does.
 * ------------------------------------------------------------------------ */

static bool en_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)out;
   union dvm_value nm = { .l = ARG(0).l }, ord = { .i = ARG(1).i };
   (void)dvm_set_field(vm, self, "name", "Ljava/lang/String;", nm);
   (void)dvm_set_field(vm, self, "ordinal", "I", ord);
   /* toString() on an enum is its name, and the VM prints objects through
    * the same utf8 field String uses. */
   const char *s = dvm_string_utf8(vm, nm.l);
   struct dvm_object *o = dvm__obj(vm, self);
   if (s && o) { free(o->utf8); o->utf8 = strdup(s); o->utf8_len = (uint32_t)strlen(s); }
   RETV();
}

static bool en_name(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &v);
   RETL(v.l);
}

static bool en_ordinal(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "ordinal", "I", &v);
   RETI(v.i);
}

static bool en_compareTo(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   union dvm_value a = { 0 }, b = { 0 };
   (void)dvm_get_field(vm, self, "ordinal", "I", &a);
   (void)dvm_get_field(vm, ARG(0).l, "ordinal", "I", &b);
   RETI(a.i - b.i);
}

static bool en_equals(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)vm;
   RETI(self == ARG(0).l ? 1 : 0);
}

static bool en_hashCode(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)vm; (void)args; (void)nargs;
   RETI((int32_t)self);
}

static bool en_getDeclaringClass(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(class_object_for(vm, dvm_object_class(vm, self)));
}

/* Enum.valueOf(Class, String) is what the compiler generates for the enum's
 * own valueOf(String).  The constants are the class's static fields of its
 * own type, so the lookup is a scan of those. */
static bool en_valueOf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)self;
   struct dvm_object *co = dvm__obj(vm, ARG(0).l);
   struct dvm_class *cls = co ? co->klass : NULL;
   const char *want = dvm_string_utf8(vm, ARG(1).l);
   if (cls && want) {
      for (int i = 0; i < cls->nsfields; ++i) {
         if (cls->sfields[i].kind != 'L' ||
             cls->sfields[i].slot >= (uint16_t)cls->nsslots)
            continue;
         dvm_ref c = cls->sslots[cls->sfields[i].slot].l;
         if (!c || dvm_object_class(vm, c) != cls) continue;
         union dvm_value nm = { 0 };
         if (!dvm_get_field(vm, c, "name", "Ljava/lang/String;", &nm)) continue;
         const char *have = dvm_string_utf8(vm, nm.l);
         if (have && !strcmp(have, want)) RETL(c);
      }
   }
   dvm__throw(vm, "java/lang/IllegalArgumentException",
              want ? want : "enum constant");
   return false;
}

/* ------------------------------------------------------------------------ *
 * java.lang.Throwable
 * ------------------------------------------------------------------------ */

static bool th_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)out;
   if (nargs >= 1) {
      const char *s = dvm_string_utf8(vm, ARG(0).l);
      if (s) {
         union dvm_value v = { .l = ARG(0).l };
         (void)dvm_set_field(vm, self, "detailMessage", "Ljava/lang/String;", v);
         struct dvm_object *o = dvm__obj(vm, self);
         if (o) { free(o->utf8); o->utf8 = strdup(s); }
      }
   }
   RETV();
}

static bool th_getMessage(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "detailMessage", "Ljava/lang/String;", &v);
   RETL(v.l);
}

static bool th_toString(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   char buf[600];
   dvm_describe_exception(vm, self, buf, sizeof buf);
   RETL(dvm_new_string(vm, buf));
}

static bool th_printStackTrace(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   char buf[600];
   dvm_describe_exception(vm, self, buf, sizeof buf);
   fprintf(stderr, "[dvm] %s\n", buf);
   RETV();
}

static bool th_fillInStackTrace(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)vm; (void)args; (void)nargs;
   RETL(self);
}

static bool th_getStackTrace(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(dvm_new_array(vm, 'L', "Ljava/lang/StackTraceElement;", 0));
}

/* ------------------------------------------------------------------------ *
 * java.lang.Thread
 * ------------------------------------------------------------------------ */

static bool t_currentThread(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   static dvm_ref main_thread;
   if (!main_thread) {
      main_thread = dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/lang/Thread;"));
      dvm_pin(vm, main_thread);
   }
   RETL(main_thread);
}

static bool t_getName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(dvm_new_string(vm, "main"));
}

static bool t_getId(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETJ(1);
}

/* Thread.sleep and Thread.start: the VM runs on whichever guest thread made
 * the JNI call, and Lunaria's scheduler is cooperative — actually sleeping
 * would stall the pump loop, and spawning a host thread would let bytecode
 * run concurrently with the interpreter's single frame stack.  Both become
 * no-ops; a Runnable handed to start() is run inline instead. */
static bool t_start(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_class *c = dvm_object_class(vm, self);
   struct dvm_method *run = c ? dvm_find_method(vm, c, "run", "()V") : NULL;
   if (run && run->has_code) {
      union dvm_value ret;
      (void)dvm_call(vm, run, self, NULL, 0, &ret);
   }
   RETV();
}

/* ------------------------------------------------------------------------ *
 * java.util.ArrayList / java.util.HashMap
 *
 * One allocation each, so the object's single `data` pointer owns everything
 * and teardown stays a plain free().
 * ------------------------------------------------------------------------ */

struct rt_list { uint32_t cap, size; dvm_ref items[]; };

static struct rt_list *list_of(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   return o ? o->data : NULL;
}

static bool list_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETV();
   free(o->data);
   o->data = calloc(1, sizeof(struct rt_list) + 8 * sizeof(dvm_ref));
   if (o->data) ((struct rt_list *)o->data)->cap = 8;
   RETV();
}

static bool list_add(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETI(0);
   struct rt_list *l = o->data;
   if (!l) { list_init(vm, self, NULL, 0, out); l = o->data; }
   if (!l) RETI(0);
   if (l->size == l->cap) {
      uint32_t cap = l->cap ? l->cap * 2 : 8;
      struct rt_list *n = realloc(l, sizeof *n + (size_t)cap * sizeof(dvm_ref));
      if (!n) RETI(0);
      n->cap = cap;
      o->data = l = n;
   }
   l->items[l->size++] = ARG(0).l;
   dvm_pin(vm, ARG(0).l);
   RETI(1);
}

static bool list_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   int32_t i = ARG(0).i;
   if (!l || i < 0 || (uint32_t)i >= l->size) {
      dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "index %d size %u",
                 i, l ? l->size : 0);
      return false;
   }
   RETL(l->items[i]);
}

static bool list_size(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_list *l = list_of(vm, self);
   RETI(l ? (int32_t)l->size : 0);
}

static bool list_isEmpty(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_list *l = list_of(vm, self);
   RETI(!l || !l->size);
}

static bool list_clear(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct rt_list *l = list_of(vm, self);
   if (l) l->size = 0;
   RETV();
}

static bool list_contains(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   const char *needle = dvm_string_utf8(vm, ARG(0).l);
   for (uint32_t i = 0; l && i < l->size; ++i) {
      if (l->items[i] == ARG(0).l) RETI(1);
      const char *s = dvm_string_utf8(vm, l->items[i]);
      if (needle && s && !strcmp(s, needle)) RETI(1);
   }
   RETI(0);
}

static bool list_remove(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, self);
   int32_t i = ARG(0).i;
   if (!l || i < 0 || (uint32_t)i >= l->size) RETL(0);
   dvm_ref old = l->items[i];
   memmove(&l->items[i], &l->items[i + 1], (l->size - (uint32_t)i - 1) * sizeof(dvm_ref));
   --l->size;
   RETL(old);
}

struct rt_map { uint32_t cap, size; struct { dvm_ref k, v; } e[]; };

static bool map_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETV();
   free(o->data);
   o->data = calloc(1, sizeof(struct rt_map) + 8 * sizeof(*((struct rt_map *)0)->e));
   if (o->data) ((struct rt_map *)o->data)->cap = 8;
   RETV();
}

static bool key_eq(struct dvm *vm, dvm_ref a, dvm_ref b)
{
   if (a == b) return true;
   const char *x = dvm_string_utf8(vm, a), *y = dvm_string_utf8(vm, b);
   return x && y && !strcmp(x, y);
}

static bool map_put(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) RETL(0);
   struct rt_map *mp = o->data;
   if (!mp) { map_init(vm, self, NULL, 0, out); mp = o->data; }
   if (!mp) RETL(0);
   for (uint32_t i = 0; i < mp->size; ++i) {
      if (!key_eq(vm, mp->e[i].k, ARG(0).l)) continue;
      dvm_ref old = mp->e[i].v;
      mp->e[i].v = ARG(1).l;
      dvm_pin(vm, ARG(1).l);
      RETL(old);
   }
   if (mp->size == mp->cap) {
      uint32_t cap = mp->cap ? mp->cap * 2 : 8;
      struct rt_map *n = realloc(mp, sizeof *n + (size_t)cap * sizeof n->e[0]);
      if (!n) RETL(0);
      n->cap = cap;
      o->data = mp = n;
   }
   mp->e[mp->size].k = ARG(0).l;
   mp->e[mp->size].v = ARG(1).l;
   ++mp->size;
   dvm_pin(vm, ARG(0).l);
   dvm_pin(vm, ARG(1).l);
   RETL(0);
}

static bool map_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   for (uint32_t i = 0; mp && i < mp->size; ++i)
      if (key_eq(vm, mp->e[i].k, ARG(0).l)) RETL(mp->e[i].v);
   RETL(0);
}

static bool map_containsKey(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   for (uint32_t i = 0; mp && i < mp->size; ++i)
      if (key_eq(vm, mp->e[i].k, ARG(0).l)) RETI(1);
   RETI(0);
}

static bool map_remove(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   for (uint32_t i = 0; mp && i < mp->size; ++i) {
      if (!key_eq(vm, mp->e[i].k, ARG(0).l)) continue;
      dvm_ref old = mp->e[i].v;
      memmove(&mp->e[i], &mp->e[i + 1], (mp->size - i - 1) * sizeof mp->e[0]);
      --mp->size;
      RETL(old);
   }
   RETL(0);
}

static bool map_size(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   RETI(mp ? (int32_t)mp->size : 0);
}

static bool map_clear(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_object *o = dvm__obj(vm, self);
   struct rt_map *mp = o ? o->data : NULL;
   if (mp) mp->size = 0;
   RETV();
}

/* ------------------------------------------------------------------------ *
 * The table
 * ------------------------------------------------------------------------ */

struct rt_method {
   const char *name, *sig;
   dvm_builtin_fn fn;
   uint32_t access;
};

struct rt_field {
   const char *name, *type;
};

struct rt_class {
   const char *desc;
   const char *super;
   const struct rt_method *methods;
   const struct rt_field *fields;
};

#define M(n, s, f)   { n, s, f, 0 }
#define SM(n, s, f)  { n, s, f, DEX_ACC_STATIC }
#define M_END        { NULL, NULL, NULL, 0 }
#define F_END        { NULL, NULL }

static const struct rt_method rt_object[] = {
   M("<init>", "()V", o_init),
   M("hashCode", "()I", o_hashCode),
   M("equals", "(Ljava/lang/Object;)Z", o_equals),
   M("toString", "()Ljava/lang/String;", o_toString),
   M("getClass", "()Ljava/lang/Class;", o_getClass),
   M("notify", "()V", nop_void),
   M("notifyAll", "()V", nop_void),
   M("wait", "()V", nop_void),
   M("wait", "(J)V", nop_void),
   M("finalize", "()V", nop_void),
   M_END,
};

static const struct rt_method rt_string[] = {
   M("<init>", "()V", s_init),
   M("<init>", "(Ljava/lang/String;)V", s_init),
   M("<init>", "([B)V", s_init),
   M("<init>", "([BII)V", s_init),
   M("<init>", "([C)V", s_init),
   M("<init>", "([CII)V", s_init),
   M("<init>", "([BLjava/lang/String;)V", s_init),
   M("length", "()I", s_length),
   M("charAt", "(I)C", s_charAt),
   M("isEmpty", "()Z", s_isEmpty),
   M("equals", "(Ljava/lang/Object;)Z", o_equals),
   M("equalsIgnoreCase", "(Ljava/lang/String;)Z", s_equalsIgnoreCase),
   M("hashCode", "()I", o_hashCode),
   M("toString", "()Ljava/lang/String;", o_toString),
   M("compareTo", "(Ljava/lang/String;)I", s_compareTo),
   M("indexOf", "(I)I", s_indexOf),
   M("indexOf", "(II)I", s_indexOf),
   M("indexOf", "(Ljava/lang/String;)I", s_indexOf),
   M("indexOf", "(Ljava/lang/String;I)I", s_indexOf),
   M("lastIndexOf", "(I)I", s_lastIndexOf),
   M("lastIndexOf", "(Ljava/lang/String;)I", s_lastIndexOf),
   M("substring", "(I)Ljava/lang/String;", s_substring),
   M("substring", "(II)Ljava/lang/String;", s_substring),
   M("concat", "(Ljava/lang/String;)Ljava/lang/String;", s_concat),
   M("trim", "()Ljava/lang/String;", s_trim),
   M("toLowerCase", "()Ljava/lang/String;", s_toLowerCase),
   M("toUpperCase", "()Ljava/lang/String;", s_toUpperCase),
   M("startsWith", "(Ljava/lang/String;)Z", s_startsWith),
   M("endsWith", "(Ljava/lang/String;)Z", s_endsWith),
   M("contains", "(Ljava/lang/CharSequence;)Z", s_contains),
   M("replace", "(CC)Ljava/lang/String;", s_replace),
   M("replace", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;", s_replace),
   M("getBytes", "()[B", s_getBytes),
   M("getBytes", "(Ljava/lang/String;)[B", s_getBytes),
   M("toCharArray", "()[C", s_toCharArray),
   M("intern", "()Ljava/lang/String;", s_intern),
   SM("valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", s_valueOf_obj),
   SM("valueOf", "(I)Ljava/lang/String;", s_valueOf_int),
   SM("valueOf", "(J)Ljava/lang/String;", s_valueOf_long),
   SM("valueOf", "(F)Ljava/lang/String;", s_valueOf_float),
   SM("valueOf", "(D)Ljava/lang/String;", s_valueOf_double),
   SM("valueOf", "(Z)Ljava/lang/String;", s_valueOf_bool),
   SM("valueOf", "(C)Ljava/lang/String;", s_valueOf_char),
   M_END,
};

static const struct rt_method rt_sb[] = {
   M("<init>", "()V", sb_init),
   M("<init>", "(I)V", sb_init),
   M("<init>", "(Ljava/lang/String;)V", sb_init),
   M("append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", sb_append_str),
   M("append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;", sb_append_str),
   M("append", "(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;", sb_append_str),
   M("append", "(I)Ljava/lang/StringBuilder;", sb_append_int),
   M("append", "(J)Ljava/lang/StringBuilder;", sb_append_long),
   M("append", "(Z)Ljava/lang/StringBuilder;", sb_append_bool),
   M("append", "(C)Ljava/lang/StringBuilder;", sb_append_char),
   M("append", "(F)Ljava/lang/StringBuilder;", sb_append_float),
   M("append", "(D)Ljava/lang/StringBuilder;", sb_append_double),
   M("append", "([C)Ljava/lang/StringBuilder;", sb_append_str),
   M("toString", "()Ljava/lang/String;", sb_toString),
   M("length", "()I", sb_length),
   M("setLength", "(I)V", sb_setLength),
   M("reverse", "()Ljava/lang/StringBuilder;", sb_reverse),
   M_END,
};

static const struct rt_method rt_sbuf[] = {
   M("<init>", "()V", sb_init),
   M("<init>", "(I)V", sb_init),
   M("<init>", "(Ljava/lang/String;)V", sb_init),
   M("append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;", sb_append_str),
   M("append", "(Ljava/lang/Object;)Ljava/lang/StringBuffer;", sb_append_str),
   M("append", "(I)Ljava/lang/StringBuffer;", sb_append_int),
   M("append", "(J)Ljava/lang/StringBuffer;", sb_append_long),
   M("append", "(Z)Ljava/lang/StringBuffer;", sb_append_bool),
   M("append", "(C)Ljava/lang/StringBuffer;", sb_append_char),
   M("append", "(F)Ljava/lang/StringBuffer;", sb_append_float),
   M("append", "(D)Ljava/lang/StringBuffer;", sb_append_double),
   M("toString", "()Ljava/lang/String;", sb_toString),
   M("length", "()I", sb_length),
   M("setLength", "(I)V", sb_setLength),
   M_END,
};

static const struct rt_field rt_box_fields[] = { { "value", "I" }, F_END };

static const struct rt_method rt_integer[] = {
   M("<init>", "(I)V", box_init),
   SM("valueOf", "(I)Ljava/lang/Integer;", i_valueOf),
   SM("parseInt", "(Ljava/lang/String;)I", i_parseInt),
   SM("parseInt", "(Ljava/lang/String;I)I", i_parseInt),
   SM("toString", "(I)Ljava/lang/String;", i_toString),
   SM("toHexString", "(I)Ljava/lang/String;", i_toHexString),
   SM("valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;", i_valueOf),
   M("intValue", "()I", box_intValue),
   M("longValue", "()J", box_longValue),
   M("floatValue", "()F", box_floatValue),
   M("doubleValue", "()D", box_doubleValue),
   M("toString", "()Ljava/lang/String;", i_toString),
   M("hashCode", "()I", box_intValue),
   M_END,
};

static const struct rt_method rt_long[] = {
   M("<init>", "(J)V", box_init),
   SM("valueOf", "(J)Ljava/lang/Long;", l_valueOf),
   SM("parseLong", "(Ljava/lang/String;)J", l_parseLong),
   SM("toString", "(J)Ljava/lang/String;", l_toString),
   M("longValue", "()J", box_longValue),
   M("intValue", "()I", box_intValue),
   M("toString", "()Ljava/lang/String;", l_toString),
   M_END,
};

static const struct rt_method rt_float[] = {
   M("<init>", "(F)V", box_init),
   SM("valueOf", "(F)Ljava/lang/Float;", f_valueOf),
   SM("parseFloat", "(Ljava/lang/String;)F", f_parseFloat),
   M("floatValue", "()F", box_floatValue),
   M("doubleValue", "()D", box_doubleValue),
   M("intValue", "()I", box_intValue),
   M_END,
};

static const struct rt_method rt_double[] = {
   M("<init>", "(D)V", box_init),
   SM("valueOf", "(D)Ljava/lang/Double;", d_valueOf),
   SM("parseDouble", "(Ljava/lang/String;)D", d_parseDouble),
   M("doubleValue", "()D", box_doubleValue),
   M("floatValue", "()F", box_floatValue),
   M("intValue", "()I", box_intValue),
   M_END,
};

static const struct rt_method rt_boolean[] = {
   M("<init>", "(Z)V", box_init),
   SM("valueOf", "(Z)Ljava/lang/Boolean;", b_valueOf),
   M("booleanValue", "()Z", b_booleanValue),
   M_END,
};

static const struct rt_method rt_character[] = {
   M("<init>", "(C)V", box_init),
   SM("valueOf", "(C)Ljava/lang/Character;", c_valueOf),
   M("charValue", "()C", c_charValue),
   M_END,
};

static const struct rt_method rt_short[] = {
   M("<init>", "(S)V", box_init),
   SM("valueOf", "(S)Ljava/lang/Short;", sh_valueOf),
   M("shortValue", "()S", box_intValue),
   M("intValue", "()I", box_intValue),
   M_END,
};

static const struct rt_method rt_byte[] = {
   M("<init>", "(B)V", box_init),
   SM("valueOf", "(B)Ljava/lang/Byte;", by_valueOf),
   M("byteValue", "()B", box_intValue),
   M("intValue", "()I", box_intValue),
   M_END,
};

static const struct rt_method rt_math[] = {
   SM("abs", "(I)I", m_absi),
   SM("abs", "(J)J", m_absl),
   SM("abs", "(F)F", m_absf),
   SM("abs", "(D)D", m_absd),
   SM("min", "(II)I", m_mini),
   SM("max", "(II)I", m_maxi),
   SM("min", "(JJ)J", m_minl),
   SM("max", "(JJ)J", m_maxl),
   SM("min", "(FF)F", m_minf),
   SM("max", "(FF)F", m_maxf),
   SM("min", "(DD)D", m_mind),
   SM("max", "(DD)D", m_maxd),
   SM("sqrt", "(D)D", m_sqrt),
   SM("floor", "(D)D", m_floor),
   SM("ceil", "(D)D", m_ceil),
   SM("sin", "(D)D", m_sin),
   SM("cos", "(D)D", m_cos),
   SM("tan", "(D)D", m_tan),
   SM("asin", "(D)D", m_asin),
   SM("acos", "(D)D", m_acos),
   SM("atan", "(D)D", m_atan),
   SM("atan2", "(DD)D", m_atan2),
   SM("log", "(D)D", m_log),
   SM("log10", "(D)D", m_log10),
   SM("exp", "(D)D", m_exp),
   SM("cbrt", "(D)D", m_cbrt),
   SM("pow", "(DD)D", m_pow),
   SM("hypot", "(DD)D", m_hypot),
   SM("round", "(F)I", m_roundf),
   SM("round", "(D)J", m_roundd),
   SM("random", "()D", m_random),
   M_END,
};

static const struct rt_method rt_system[] = {
   SM("currentTimeMillis", "()J", sys_currentTimeMillis),
   SM("nanoTime", "()J", sys_nanoTime),
   SM("arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", sys_arraycopy),
   SM("identityHashCode", "(Ljava/lang/Object;)I", sys_identityHashCode),
   SM("getProperty", "(Ljava/lang/String;)Ljava/lang/String;", sys_getProperty),
   SM("getProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", sys_getProperty),
   SM("gc", "()V", nop_void),
   SM("exit", "(I)V", nop_void),
   M_END,
};

static const struct rt_field rt_system_fields[] = {
   { "out", "Ljava/io/PrintStream;" },
   { "err", "Ljava/io/PrintStream;" },
   F_END,
};

static const struct rt_method rt_printstream[] = {
   M("println", "()V", ps_println),
   M("println", "(Ljava/lang/String;)V", ps_println),
   M("println", "(Ljava/lang/Object;)V", ps_println),
   M("println", "(I)V", ps_println),
   M("print", "(Ljava/lang/String;)V", ps_println),
   M("flush", "()V", nop_void),
   M_END,
};

static const struct rt_method rt_class[] = {
   M("getName", "()Ljava/lang/String;", cl_getName),
   M("getCanonicalName", "()Ljava/lang/String;", cl_getName),
   M("getSimpleName", "()Ljava/lang/String;", cl_getSimpleName),
   SM("forName", "(Ljava/lang/String;)Ljava/lang/Class;", cl_forName),
   M("toString", "()Ljava/lang/String;", cl_getName),
   M_END,
};

static const struct rt_method rt_throwable[] = {
   M("<init>", "()V", th_init),
   M("<init>", "(Ljava/lang/String;)V", th_init),
   M("<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V", th_init),
   M("<init>", "(Ljava/lang/Throwable;)V", th_init),
   M("getMessage", "()Ljava/lang/String;", th_getMessage),
   M("getLocalizedMessage", "()Ljava/lang/String;", th_getMessage),
   M("toString", "()Ljava/lang/String;", th_toString),
   M("printStackTrace", "()V", th_printStackTrace),
   M("fillInStackTrace", "()Ljava/lang/Throwable;", th_fillInStackTrace),
   M("getStackTrace", "()[Ljava/lang/StackTraceElement;", th_getStackTrace),
   M("getCause", "()Ljava/lang/Throwable;", th_fillInStackTrace),
   M_END,
};

static const struct rt_method rt_enum[] = {
   M("<init>", "(Ljava/lang/String;I)V", en_init),
   M("name", "()Ljava/lang/String;", en_name),
   M("toString", "()Ljava/lang/String;", en_name),
   M("ordinal", "()I", en_ordinal),
   M("compareTo", "(Ljava/lang/Object;)I", en_compareTo),
   M("equals", "(Ljava/lang/Object;)Z", en_equals),
   M("hashCode", "()I", en_hashCode),
   M("getDeclaringClass", "()Ljava/lang/Class;", en_getDeclaringClass),
   SM("valueOf", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;", en_valueOf),
   M_END,
};

static const struct rt_field rt_enum_fields[] = {
   { "name", "Ljava/lang/String;" },
   { "ordinal", "I" },
   F_END,
};

static const struct rt_field rt_throwable_fields[] = {
   { "detailMessage", "Ljava/lang/String;" },
   F_END,
};

static const struct rt_method rt_thread[] = {
   M("<init>", "()V", o_init),
   M("<init>", "(Ljava/lang/Runnable;)V", o_init),
   SM("currentThread", "()Ljava/lang/Thread;", t_currentThread),
   SM("sleep", "(J)V", nop_void),
   M("getName", "()Ljava/lang/String;", t_getName),
   M("getId", "()J", t_getId),
   M("start", "()V", t_start),
   M("run", "()V", nop_void),
   M("join", "()V", nop_void),
   M("setName", "(Ljava/lang/String;)V", nop_void),
   M("setPriority", "(I)V", nop_void),
   M("setDaemon", "(Z)V", nop_void),
   M("interrupt", "()V", nop_void),
   M_END,
};

static const struct rt_method rt_list_methods[] = {
   M("<init>", "()V", list_init),
   M("<init>", "(I)V", list_init),
   M("add", "(Ljava/lang/Object;)Z", list_add),
   M("get", "(I)Ljava/lang/Object;", list_get),
   M("size", "()I", list_size),
   M("isEmpty", "()Z", list_isEmpty),
   M("clear", "()V", list_clear),
   M("contains", "(Ljava/lang/Object;)Z", list_contains),
   M("remove", "(I)Ljava/lang/Object;", list_remove),
   M_END,
};

static const struct rt_method rt_map_methods[] = {
   M("<init>", "()V", map_init),
   M("<init>", "(I)V", map_init),
   M("put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", map_put),
   M("get", "(Ljava/lang/Object;)Ljava/lang/Object;", map_get),
   M("containsKey", "(Ljava/lang/Object;)Z", map_containsKey),
   M("remove", "(Ljava/lang/Object;)Ljava/lang/Object;", map_remove),
   M("size", "()I", map_size),
   M("clear", "()V", map_clear),
   M_END,
};

static const struct rt_class rt_classes[] = {
   { "Ljava/lang/Object;", NULL, rt_object, NULL },
   { "Ljava/lang/String;", "Ljava/lang/Object;", rt_string, NULL },
   { "Ljava/lang/CharSequence;", "Ljava/lang/Object;", NULL, NULL },
   { "Ljava/lang/StringBuilder;", "Ljava/lang/Object;", rt_sb, NULL },
   { "Ljava/lang/StringBuffer;", "Ljava/lang/Object;", rt_sbuf, NULL },
   { "Ljava/lang/Number;", "Ljava/lang/Object;", NULL, NULL },
   { "Ljava/lang/Integer;", "Ljava/lang/Number;", rt_integer, rt_box_fields },
   { "Ljava/lang/Long;", "Ljava/lang/Number;", rt_long, rt_box_fields },
   { "Ljava/lang/Float;", "Ljava/lang/Number;", rt_float, rt_box_fields },
   { "Ljava/lang/Double;", "Ljava/lang/Number;", rt_double, rt_box_fields },
   { "Ljava/lang/Short;", "Ljava/lang/Number;", rt_short, rt_box_fields },
   { "Ljava/lang/Byte;", "Ljava/lang/Number;", rt_byte, rt_box_fields },
   { "Ljava/lang/Boolean;", "Ljava/lang/Object;", rt_boolean, rt_box_fields },
   { "Ljava/lang/Character;", "Ljava/lang/Object;", rt_character, rt_box_fields },
   { "Ljava/lang/Math;", "Ljava/lang/Object;", rt_math, NULL },
   { "Ljava/lang/System;", "Ljava/lang/Object;", rt_system, rt_system_fields },
   { "Ljava/io/PrintStream;", "Ljava/lang/Object;", rt_printstream, NULL },
   { "Ljava/lang/Class;", "Ljava/lang/Object;", rt_class, NULL },
   { "Ljava/lang/Thread;", "Ljava/lang/Object;", rt_thread, NULL },
   { "Ljava/lang/Enum;", "Ljava/lang/Object;", rt_enum, rt_enum_fields },
   { "Ljava/util/ArrayList;", "Ljava/lang/Object;", rt_list_methods, NULL },
   { "Ljava/util/List;", "Ljava/lang/Object;", rt_list_methods, NULL },
   { "Ljava/util/HashMap;", "Ljava/lang/Object;", rt_map_methods, NULL },
   { "Ljava/util/Map;", "Ljava/lang/Object;", rt_map_methods, NULL },

   /* The exception hierarchy.  Every one of these inherits Throwable's
    * methods, so only the names have to be listed. */
   { "Ljava/lang/Throwable;", "Ljava/lang/Object;", rt_throwable, rt_throwable_fields },
   { "Ljava/lang/Exception;", "Ljava/lang/Throwable;", NULL, NULL },
   { "Ljava/lang/Error;", "Ljava/lang/Throwable;", NULL, NULL },
   { "Ljava/lang/RuntimeException;", "Ljava/lang/Exception;", NULL, NULL },
   { "Ljava/lang/NullPointerException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/ArithmeticException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/ClassCastException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/IllegalArgumentException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/IllegalStateException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/NumberFormatException;", "Ljava/lang/IllegalArgumentException;", NULL, NULL },
   { "Ljava/lang/IndexOutOfBoundsException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/ArrayIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;", NULL, NULL },
   { "Ljava/lang/StringIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;", NULL, NULL },
   { "Ljava/lang/UnsupportedOperationException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/NegativeArraySizeException;", "Ljava/lang/RuntimeException;", NULL, NULL },
   { "Ljava/lang/ClassNotFoundException;", "Ljava/lang/Exception;", NULL, NULL },
   { "Ljava/lang/NoSuchMethodError;", "Ljava/lang/Error;", NULL, NULL },
   { "Ljava/lang/NoClassDefFoundError;", "Ljava/lang/Error;", NULL, NULL },
   { "Ljava/lang/UnsatisfiedLinkError;", "Ljava/lang/Error;", NULL, NULL },
   { "Ljava/lang/StackOverflowError;", "Ljava/lang/Error;", NULL, NULL },
   { "Ljava/lang/OutOfMemoryError;", "Ljava/lang/Error;", NULL, NULL },
   { "Ljava/lang/VerifyError;", "Ljava/lang/Error;", NULL, NULL },
   { "Ljava/io/IOException;", "Ljava/lang/Exception;", NULL, NULL },
   { "Ljava/io/FileNotFoundException;", "Ljava/io/IOException;", NULL, NULL },
   { "Ljava/lang/InterruptedException;", "Ljava/lang/Exception;", NULL, NULL },
};

/* dvm.c owns class_register(); the runtime reaches it through this shim so the
 * registry stays in one place. */
struct dvm_class *dvm__register_builtin(struct dvm *vm, const char *desc);

struct dvm_class *dvm_runtime_define(struct dvm *vm, const char *desc)
{
   const struct rt_class *rc = NULL;
   for (size_t i = 0; i < sizeof rt_classes / sizeof rt_classes[0]; ++i)
      if (!strcmp(rt_classes[i].desc, desc)) { rc = &rt_classes[i]; break; }
   if (!rc) return NULL;

   struct dvm_class *c = dvm__register_builtin(vm, desc);
   if (!c) return NULL;
   c->init_state = 2;   /* nothing to run: the tables are already populated */

   /* Registered before resolving the super so a cycle cannot recurse. */
   if (rc->super) c->super = dvm__class_by_desc(vm, rc->super);

   int nf = 0;
   if (rc->fields) while (rc->fields[nf].name) ++nf;
   c->nifields = nf;
   if (nf) {
      c->ifields = calloc((size_t)nf, sizeof *c->ifields);
      int base = c->super ? c->super->islots : 0;
      for (int i = 0; i < nf && c->ifields; ++i) {
         c->ifields[i].cls = c;
         c->ifields[i].name = rc->fields[i].name;
         c->ifields[i].type = rc->fields[i].type;
         c->ifields[i].kind = dvm__kind_of(rc->fields[i].type);
         c->ifields[i].width = 1;
         c->ifields[i].slot = (uint16_t)(base + i);
      }
      c->islots = base + nf;
   } else {
      c->islots = c->super ? c->super->islots : 0;
   }

   int nm = 0;
   if (rc->methods) while (rc->methods[nm].name) ++nm;
   c->nmethods = nm;
   if (nm) {
      c->methods = calloc((size_t)nm, sizeof *c->methods);
      for (int i = 0; i < nm && c->methods; ++i) {
         struct dvm_method *m = &c->methods[i];
         m->cls = c;
         m->name = rc->methods[i].name;
         m->sig = strdup(rc->methods[i].sig);
         m->access = rc->methods[i].access;
         m->builtin = rc->methods[i].fn;
         m->arg_slots = dvm_sig_arg_slots(m->sig);
         m->arg_count = dvm_sig_arg_count(m->sig);
         m->ret_kind = dvm_sig_return_kind(m->sig);
      }
   }

   /* System.out / System.err need real objects to receive println(). */
   if (!strcmp(desc, "Ljava/lang/System;")) {
      c->nsfields = nf;
      c->sfields = c->ifields;
      c->ifields = NULL;
      c->nifields = 0;
      c->islots = c->super ? c->super->islots : 0;
      for (int i = 0; i < c->nsfields; ++i) c->sfields[i].slot = (uint16_t)i;
      c->nsslots = c->nsfields;
      c->sslots = calloc((size_t)(c->nsslots ? c->nsslots : 1), sizeof *c->sslots);
      struct dvm_class *ps = dvm__class_by_desc(vm, "Ljava/io/PrintStream;");
      for (int i = 0; i < c->nsslots; ++i)
         c->sslots[i].l = dvm_new_object(vm, ps);
   }

   return c;
}

void dvm_runtime_install(struct dvm *vm)
{
   /* Object first: everything else names it as its super. */
   (void)dvm__class_by_desc(vm, "Ljava/lang/Object;");
   (void)dvm__class_by_desc(vm, "Ljava/lang/String;");
   (void)dvm__class_by_desc(vm, "Ljava/lang/Class;");
   (void)dvm__class_by_desc(vm, "Ljava/lang/Throwable;");
}
