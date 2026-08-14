/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "dvm/dex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- primitives --------------------------------------------------------- */

size_t dex_uleb(const struct dex_file *d, size_t off, uint32_t *out)
{
   uint32_t v = 0;
   int shift = 0;
   for (int i = 0; i < 5; ++i) {
      if (off >= d->len) { *out = 0; return off; }
      uint8_t b = d->p[off++];
      v |= (uint32_t)(b & 0x7f) << shift;
      shift += 7;
      if (!(b & 0x80)) break;
   }
   *out = v;
   return off;
}

size_t dex_sleb(const struct dex_file *d, size_t off, int32_t *out)
{
   int32_t v = 0;
   int shift = 0;
   uint8_t b = 0;
   for (int i = 0; i < 5; ++i) {
      if (off >= d->len) { *out = 0; return off; }
      b = d->p[off++];
      v |= (int32_t)(uint32_t)(b & 0x7f) << shift;
      shift += 7;
      if (!(b & 0x80)) break;
   }
   if (shift < 32 && (b & 0x40))
      v |= -(int32_t)((uint32_t)1 << shift);
   *out = v;
   return off;
}

/* uleb128p1: the encoded value is the real one plus one, so 0 means -1. */
static size_t dex_ulebp1(const struct dex_file *d, size_t off, int32_t *out)
{
   uint32_t v;
   off = dex_uleb(d, off, &v);
   *out = (int32_t)v - 1;
   return off;
}

/* --- container ---------------------------------------------------------- */

static bool dex_parse_header(struct dex_file *d)
{
   if (d->len < 112) return false;
   if (memcmp(d->p, "dex\n", 4) != 0) return false;
   /* "035" is the baseline; 037/038/039/040 add opcodes but keep the header. */
   if (d->p[7] != 0) return false;

   uint32_t endian = dex_u32_at(d, 40);
   if (endian != 0x12345678u) return false;  /* byte-swapped dex is not produced by d8 */

   d->string_ids_size = dex_u32_at(d, 56);
   d->string_ids_off  = dex_u32_at(d, 60);
   d->type_ids_size   = dex_u32_at(d, 64);
   d->type_ids_off    = dex_u32_at(d, 68);
   d->proto_ids_size  = dex_u32_at(d, 72);
   d->proto_ids_off   = dex_u32_at(d, 76);
   d->field_ids_size  = dex_u32_at(d, 80);
   d->field_ids_off   = dex_u32_at(d, 84);
   d->method_ids_size = dex_u32_at(d, 88);
   d->method_ids_off  = dex_u32_at(d, 92);
   d->class_defs_size = dex_u32_at(d, 96);
   d->class_defs_off  = dex_u32_at(d, 100);

   /* Every pool must lie inside the mapping. */
   struct { uint64_t off, size, stride; } pools[] = {
      { d->string_ids_off, d->string_ids_size, 4 },
      { d->type_ids_off,   d->type_ids_size,   4 },
      { d->proto_ids_off,  d->proto_ids_size,  12 },
      { d->field_ids_off,  d->field_ids_size,  8 },
      { d->method_ids_off, d->method_ids_size, 8 },
      { d->class_defs_off, d->class_defs_size, 32 },
   };
   for (size_t i = 0; i < sizeof pools / sizeof pools[0]; ++i) {
      if (!pools[i].size) continue;
      if (pools[i].off + pools[i].size * pools[i].stride > d->len)
         return false;
   }
   return true;
}

bool dex_open_memory(struct dex_file *d, const uint8_t *data, size_t len,
                     const char *path)
{
   memset(d, 0, sizeof *d);
   d->p = data;
   d->len = len;
   d->path = path ? strdup(path) : NULL;
   if (!dex_parse_header(d)) {
      free(d->path);
      memset(d, 0, sizeof *d);
      return false;
   }
   d->strings = calloc(d->string_ids_size ? d->string_ids_size : 1, sizeof *d->strings);
   d->fixups  = calloc(d->string_ids_size ? d->string_ids_size : 1, sizeof *d->fixups);
   if (!d->strings || !d->fixups) {
      dex_close(d);
      return false;
   }
   return true;
}

bool dex_open(struct dex_file *d, const char *path)
{
   memset(d, 0, sizeof *d);
   FILE *f = fopen(path, "rb");
   if (!f) return false;
   if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
   long size = ftell(f);
   if (size <= 0) { fclose(f); return false; }
   rewind(f);
   uint8_t *buf = malloc((size_t)size);
   if (!buf) { fclose(f); return false; }
   size_t got = fread(buf, 1, (size_t)size, f);
   fclose(f);
   if (got != (size_t)size) { free(buf); return false; }
   if (!dex_open_memory(d, buf, got, path)) { free(buf); return false; }
   d->owned = buf;
   return true;
}

void dex_close(struct dex_file *d)
{
   if (!d) return;
   if (d->fixups) {
      for (uint32_t i = 0; i < d->string_ids_size; ++i)
         free(d->fixups[i]);
      free(d->fixups);
   }
   free(d->strings);
   free(d->path);
   free(d->owned);
   memset(d, 0, sizeof *d);
}

/* --- strings ------------------------------------------------------------ */

/* MUTF-8 differs from UTF-8 in two ways: NUL is encoded as C0 80, and code
 * points above the BMP are encoded as a surrogate pair of three-byte
 * sequences.  Both forms are rejected by anything expecting plain UTF-8, so we
 * normalise them into a private copy the first time such a string is read. */
static bool mutf8_needs_fixup(const uint8_t *s)
{
   /* c0 80 (the MUTF-8 spelling of U+0000) is left alone: it is already safe
    * to carry in a C string, and it keeps the character count right.  Only
    * surrogate pairs have to be folded into real UTF-8. */
   for (; *s; ++s)
      if (s[0] == 0xed && (s[1] & 0xf0) == 0xa0) return true;  /* high surrogate */
   return false;
}

static char *mutf8_to_utf8(const uint8_t *s)
{
   size_t n = strlen((const char *)s);
   char *out = malloc(n + 1);
   if (!out) return NULL;
   char *w = out;
   while (*s) {
      /* c0 80 falls through untouched: dropping it silently shortened the
       * string by a character, which broke length checks on binary payloads
       * carried as strings (Play services stores its signing certificates
       * that way). */
      if (s[0] == 0xed && (s[1] & 0xf0) == 0xa0 &&
          s[3] == 0xed && (s[4] & 0xf0) == 0xb0) {
         uint32_t hi = 0xd800u | (uint32_t)(s[1] & 0x0f) << 6 | (s[2] & 0x3f);
         uint32_t lo = 0xdc00u | (uint32_t)(s[4] & 0x0f) << 6 | (s[5] & 0x3f);
         uint32_t cp = 0x10000u + ((hi - 0xd800u) << 10) + (lo - 0xdc00u);
         *w++ = (char)(0xf0 | (cp >> 18));
         *w++ = (char)(0x80 | ((cp >> 12) & 0x3f));
         *w++ = (char)(0x80 | ((cp >> 6) & 0x3f));
         *w++ = (char)(0x80 | (cp & 0x3f));
         s += 6;
         continue;
      }
      *w++ = (char)*s++;
   }
   *w = '\0';
   return out;
}

const char *dex_string(struct dex_file *d, uint32_t idx)
{
   if (idx >= d->string_ids_size) return NULL;
   if (d->strings[idx]) return d->strings[idx];

   uint32_t off = dex_u32_at(d, d->string_ids_off + (size_t)idx * 4u);
   if (off >= d->len) return NULL;
   uint32_t utf16_len;
   size_t p = dex_uleb(d, off, &utf16_len);
   if (p >= d->len) return NULL;

   /* The data must be NUL-terminated inside the mapping. */
   if (!memchr(d->p + p, 0, d->len - p)) return NULL;

   const uint8_t *raw = d->p + p;
   if (mutf8_needs_fixup(raw)) {
      char *fixed = mutf8_to_utf8(raw);
      if (!fixed) return NULL;
      d->fixups[idx] = fixed;
      d->strings[idx] = fixed;
   } else {
      d->strings[idx] = (const char *)raw;
   }
   return d->strings[idx];
}

const char *dex_type(struct dex_file *d, uint32_t idx)
{
   if (idx >= d->type_ids_size) return NULL;
   return dex_string(d, dex_u32_at(d, d->type_ids_off + (size_t)idx * 4u));
}

/* --- id pools ----------------------------------------------------------- */

bool dex_method_id(const struct dex_file *d, uint32_t idx, struct dex_method_id *out)
{
   if (idx >= d->method_ids_size) return false;
   size_t e = d->method_ids_off + (size_t)idx * 8u;
   out->class_idx = dex_u16_at(d, e);
   out->proto_idx = dex_u16_at(d, e + 2);
   out->name_idx  = dex_u32_at(d, e + 4);
   return true;
}

bool dex_field_id(const struct dex_file *d, uint32_t idx, struct dex_field_id *out)
{
   if (idx >= d->field_ids_size) return false;
   size_t e = d->field_ids_off + (size_t)idx * 8u;
   out->class_idx = dex_u16_at(d, e);
   out->type_idx  = dex_u16_at(d, e + 2);
   out->name_idx  = dex_u32_at(d, e + 4);
   return true;
}

bool dex_class_def(const struct dex_file *d, uint32_t idx, struct dex_class_def *out)
{
   if (idx >= d->class_defs_size) return false;
   size_t e = d->class_defs_off + (size_t)idx * 32u;
   out->class_idx        = dex_u32_at(d, e);
   out->access_flags     = dex_u32_at(d, e + 4);
   out->superclass_idx   = dex_u32_at(d, e + 8);
   out->interfaces_off   = dex_u32_at(d, e + 12);
   out->source_file_idx  = dex_u32_at(d, e + 16);
   out->annotations_off  = dex_u32_at(d, e + 20);
   out->class_data_off   = dex_u32_at(d, e + 24);
   out->static_values_off = dex_u32_at(d, e + 28);
   return true;
}

const char *dex_proto_shorty(struct dex_file *d, uint32_t proto_idx)
{
   if (proto_idx >= d->proto_ids_size) return NULL;
   return dex_string(d, dex_u32_at(d, d->proto_ids_off + (size_t)proto_idx * 12u));
}

const char *dex_proto_return(struct dex_file *d, uint32_t proto_idx)
{
   if (proto_idx >= d->proto_ids_size) return NULL;
   return dex_type(d, dex_u32_at(d, d->proto_ids_off + (size_t)proto_idx * 12u + 4u));
}

int dex_proto_params(struct dex_file *d, uint32_t proto_idx,
                     const char **out, int max)
{
   if (proto_idx >= d->proto_ids_size) return -1;
   uint32_t off = dex_u32_at(d, d->proto_ids_off + (size_t)proto_idx * 12u + 8u);
   if (!off) return 0;
   if (off + 4 > d->len) return -1;
   uint32_t n = dex_u32_at(d, off);
   if (off + 4u + (uint64_t)n * 2u > d->len) return -1;
   for (uint32_t i = 0; i < n && (int)i < max; ++i)
      out[i] = dex_type(d, dex_u16_at(d, off + 4u + (size_t)i * 2u));
   return (int)n;
}

bool dex_proto_signature(struct dex_file *d, uint32_t proto_idx,
                         char *buf, size_t buf_sz)
{
   const char *params[256];
   int n = dex_proto_params(d, proto_idx, params, 256);
   if (n < 0 || n > 256) return false;
   const char *ret = dex_proto_return(d, proto_idx);
   if (!ret) return false;

   size_t used = 0;
   if (buf_sz < 3) return false;
   buf[used++] = '(';
   for (int i = 0; i < n; ++i) {
      if (!params[i]) return false;
      size_t l = strlen(params[i]);
      if (used + l + 2 + strlen(ret) >= buf_sz) return false;
      memcpy(buf + used, params[i], l);
      used += l;
   }
   buf[used++] = ')';
   size_t rl = strlen(ret);
   if (used + rl + 1 > buf_sz) return false;
   memcpy(buf + used, ret, rl);
   buf[used + rl] = '\0';
   return true;
}

int dex_find_class(struct dex_file *d, const char *descriptor)
{
   for (uint32_t i = 0; i < d->class_defs_size; ++i) {
      uint32_t ti = dex_u32_at(d, d->class_defs_off + (size_t)i * 32u);
      const char *t = dex_type(d, ti);
      if (t && !strcmp(t, descriptor))
         return (int)i;
   }
   return -1;
}

/* --- class data --------------------------------------------------------- */

static bool read_fields(const struct dex_file *d, size_t *pp, uint32_t n,
                        struct dex_encoded_field **out)
{
   *out = n ? calloc(n, sizeof **out) : NULL;
   if (n && !*out) return false;
   uint32_t idx = 0;
   for (uint32_t i = 0; i < n; ++i) {
      uint32_t diff, acc;
      *pp = dex_uleb(d, *pp, &diff);
      *pp = dex_uleb(d, *pp, &acc);
      idx += diff;
      (*out)[i].field_idx = idx;
      (*out)[i].access_flags = acc;
   }
   return true;
}

static bool read_methods(const struct dex_file *d, size_t *pp, uint32_t n,
                         struct dex_encoded_method **out)
{
   *out = n ? calloc(n, sizeof **out) : NULL;
   if (n && !*out) return false;
   uint32_t idx = 0;
   for (uint32_t i = 0; i < n; ++i) {
      uint32_t diff, acc, code;
      *pp = dex_uleb(d, *pp, &diff);
      *pp = dex_uleb(d, *pp, &acc);
      *pp = dex_uleb(d, *pp, &code);
      idx += diff;
      (*out)[i].method_idx = idx;
      (*out)[i].access_flags = acc;
      (*out)[i].code_off = code;
   }
   return true;
}

bool dex_class_data(const struct dex_file *d, uint32_t off, struct dex_class_data *out)
{
   memset(out, 0, sizeof *out);
   if (!off || off >= d->len) return false;

   size_t p = off;
   p = dex_uleb(d, p, &out->static_fields_size);
   p = dex_uleb(d, p, &out->instance_fields_size);
   p = dex_uleb(d, p, &out->direct_methods_size);
   p = dex_uleb(d, p, &out->virtual_methods_size);

   /* Each entry is at least two bytes, so a count larger than the remaining
    * mapping is a corrupt file, not a huge class. */
   uint64_t total = (uint64_t)out->static_fields_size + out->instance_fields_size +
                    out->direct_methods_size + out->virtual_methods_size;
   if (total * 2u > d->len - p) {
      memset(out, 0, sizeof *out);
      return false;
   }

   if (!read_fields(d, &p, out->static_fields_size, &out->static_fields) ||
       !read_fields(d, &p, out->instance_fields_size, &out->instance_fields) ||
       !read_methods(d, &p, out->direct_methods_size, &out->direct_methods) ||
       !read_methods(d, &p, out->virtual_methods_size, &out->virtual_methods)) {
      dex_class_data_release(out);
      return false;
   }
   return true;
}

void dex_class_data_release(struct dex_class_data *cd)
{
   free(cd->static_fields);
   free(cd->instance_fields);
   free(cd->direct_methods);
   free(cd->virtual_methods);
   memset(cd, 0, sizeof *cd);
}

bool dex_code(const struct dex_file *d, uint32_t off, struct dex_code *out)
{
   memset(out, 0, sizeof *out);
   if (!off || (uint64_t)off + 16u > d->len) return false;

   out->registers_size = dex_u16_at(d, off);
   out->ins_size       = dex_u16_at(d, off + 2);
   out->outs_size      = dex_u16_at(d, off + 4);
   out->tries_size     = dex_u16_at(d, off + 6);
   out->insns_size     = dex_u32_at(d, off + 12);

   uint64_t insns_off = (uint64_t)off + 16u;
   if (insns_off + (uint64_t)out->insns_size * 2u > d->len) return false;
   out->insns = (const uint16_t *)(const void *)(d->p + insns_off);

   if (out->tries_size) {
      uint64_t t = insns_off + (uint64_t)out->insns_size * 2u;
      if (out->insns_size & 1u) t += 2;  /* padding to a 4-byte boundary */
      if (t + (uint64_t)out->tries_size * 8u > d->len) {
         out->tries_size = 0;
         return true;   /* code is usable; the handler table is not */
      }
      out->tries = d->p + t;
      out->handlers = d->p + t + (size_t)out->tries_size * 8u;
   }
   return true;
}

int dex_interfaces(const struct dex_file *d, uint32_t interfaces_off,
                   uint16_t *out, int max)
{
   if (!interfaces_off || (uint64_t)interfaces_off + 4u > d->len) return 0;
   uint32_t n = dex_u32_at(d, interfaces_off);
   if ((uint64_t)interfaces_off + 4u + (uint64_t)n * 2u > d->len) return 0;
   for (uint32_t i = 0; i < n && (int)i < max; ++i)
      out[i] = dex_u16_at(d, interfaces_off + 4u + (size_t)i * 2u);
   return (int)n;
}

/* --- encoded values ----------------------------------------------------- */

/* Reads one encoded_value.  Sign- or zero-extension depends on the type:
 * integers are sign-extended, float/double are zero-extended to the *left*
 * (the encoded bytes are the high-order end of the value). */
static size_t dex_read_value(const struct dex_file *d, size_t p, struct dex_value *v)
{
   if (p >= d->len) { v->type = DEX_VALUE_NULL; v->bits = 0; return p; }
   uint8_t hdr = d->p[p++];
   uint32_t type = hdr & 0x1f;
   uint32_t size = (uint32_t)(hdr >> 5) + 1u;

   v->type = (enum dex_value_type)type;
   v->bits = 0;

   switch (type) {
      case DEX_VALUE_NULL:
         return p;
      case DEX_VALUE_BOOLEAN:
         v->bits = (hdr >> 5) & 1u;
         return p;
      case DEX_VALUE_ARRAY: {
         /* Nested arrays are skipped: we record the offset so the caller can
          * decode them if it cares. */
         v->bits = p;
         uint32_t n;
         size_t q = dex_uleb(d, p, &n);
         struct dex_value tmp;
         for (uint32_t i = 0; i < n; ++i)
            q = dex_read_value(d, q, &tmp);
         return q;
      }
      case DEX_VALUE_ANNOTATION: {
         /* type_idx, size, then (name_idx, value)* */
         uint32_t ti, n;
         size_t q = dex_uleb(d, p, &ti);
         q = dex_uleb(d, q, &n);
         struct dex_value tmp;
         for (uint32_t i = 0; i < n; ++i) {
            uint32_t ni;
            q = dex_uleb(d, q, &ni);
            q = dex_read_value(d, q, &tmp);
         }
         return q;
      }
      default:
         break;
   }

   if (size > 8 || p + size > d->len) { v->type = DEX_VALUE_NULL; return d->len; }

   uint64_t raw = 0;
   for (uint32_t i = 0; i < size; ++i)
      raw |= (uint64_t)d->p[p + i] << (8u * i);
   p += size;

   switch (type) {
      case DEX_VALUE_BYTE: case DEX_VALUE_SHORT: case DEX_VALUE_INT:
      case DEX_VALUE_LONG: {
         uint32_t bits = size * 8u;
         if (bits < 64 && (raw >> (bits - 1)) & 1u)
            raw |= ~((uint64_t)0) << bits;   /* sign-extend */
         v->bits = raw;
         break;
      }
      case DEX_VALUE_CHAR:
         v->bits = raw;                      /* zero-extended */
         break;
      case DEX_VALUE_FLOAT:
         v->bits = raw << (32u - size * 8u); /* right-zero-extended to 32 bits */
         break;
      case DEX_VALUE_DOUBLE:
         v->bits = raw << (64u - size * 8u);
         break;
      default:
         v->bits = raw;                      /* pool index */
         break;
   }
   return p;
}

int dex_static_values(const struct dex_file *d, uint32_t off,
                      struct dex_value *out, int max)
{
   if (!off || off >= d->len) return -1;
   uint32_t n;
   size_t p = dex_uleb(d, off, &n);
   for (uint32_t i = 0; i < n; ++i) {
      struct dex_value tmp;
      p = dex_read_value(d, p, (int)i < max ? &out[i] : &tmp);
   }
   return (int)n;
}

/* Silence "defined but not used" for the p1 helper: it is part of the format
 * and used by the debug_info decoder that lives in the interpreter. */
size_t dex_ulebp1_(const struct dex_file *d, size_t off, int32_t *out);
size_t dex_ulebp1_(const struct dex_file *d, size_t off, int32_t *out)
{
   return dex_ulebp1(d, off, out);
}
