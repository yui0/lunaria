/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Reader for the Dalvik executable container (classes*.dex).
 *
 * This is a random-access view over a whole dex image held in memory: the
 * index pools are decoded on demand, nothing is copied, and every accessor
 * bounds-checks against the mapping.  A malformed or truncated dex must make
 * the accessor fail, never read out of bounds — the files come from
 * third-party APKs.
 *
 * Layout reference: https://source.android.com/docs/core/runtime/dex-format
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Access flags (dex, class/field/method share the space). */
#define DEX_ACC_PUBLIC       0x00001
#define DEX_ACC_PRIVATE      0x00002
#define DEX_ACC_PROTECTED    0x00004
#define DEX_ACC_STATIC       0x00008
#define DEX_ACC_FINAL        0x00010
#define DEX_ACC_SYNCHRONIZED 0x00020
#define DEX_ACC_VOLATILE     0x00040
#define DEX_ACC_BRIDGE       0x00040
#define DEX_ACC_TRANSIENT    0x00080
#define DEX_ACC_VARARGS      0x00080
#define DEX_ACC_NATIVE       0x00100
#define DEX_ACC_INTERFACE    0x00200
#define DEX_ACC_ABSTRACT     0x00400
#define DEX_ACC_STRICT       0x00800
#define DEX_ACC_SYNTHETIC    0x01000
#define DEX_ACC_ANNOTATION   0x02000
#define DEX_ACC_ENUM         0x04000
#define DEX_ACC_CONSTRUCTOR  0x10000

#define DEX_NO_INDEX 0xffffffffu

struct dex_file {
   const uint8_t *p;
   size_t len;
   char *path;      /* owned; for diagnostics */
   uint8_t *owned;  /* owned mapping when dex_open() read the file */

   uint32_t string_ids_size, string_ids_off;
   uint32_t type_ids_size, type_ids_off;
   uint32_t proto_ids_size, proto_ids_off;
   uint32_t field_ids_size, field_ids_off;
   uint32_t method_ids_size, method_ids_off;
   uint32_t class_defs_size, class_defs_off;

   /* Decoded string pool: one pointer per string_id, filled lazily.  The
    * pointers alias the mapping (MUTF-8 is NUL-terminated in the file), except
    * where a string contains an encoded surrogate pair or an embedded NUL, in
    * which case the entry owns a converted copy (see `fixups`). */
   const char **strings;
   char **fixups;

   /* descriptor → class_def index, built on the first lookup.  Without it
    * dex_find_class() is a strcmp against every class in the file, and
    * dvm_class_is_known() runs that over every dex — which the JNI bridge
    * does on each Call*Method.  With three dexes and 16,500 classes that was
    * the single largest non-guest cost in a profile of world loading. */
   uint32_t *cls_hash;      /* open addressing; 0 empty, else index + 1 */
   uint32_t  cls_hash_cap;  /* power of two */
};

struct dex_method_id {
   uint16_t class_idx;
   uint16_t proto_idx;
   uint32_t name_idx;
};

struct dex_field_id {
   uint16_t class_idx;
   uint16_t type_idx;
   uint32_t name_idx;
};

struct dex_class_def {
   uint32_t class_idx;
   uint32_t access_flags;
   uint32_t superclass_idx;
   uint32_t interfaces_off;
   uint32_t source_file_idx;
   uint32_t annotations_off;
   uint32_t class_data_off;
   uint32_t static_values_off;
};

struct dex_code {
   uint16_t registers_size;
   uint16_t ins_size;
   uint16_t outs_size;
   uint16_t tries_size;
   uint32_t insns_size;      /* in 16-bit code units */
   const uint16_t *insns;
   const uint8_t *tries;     /* try_item[tries_size], or NULL */
   const uint8_t *handlers;  /* encoded_catch_handler_list, or NULL */
};

/* One decoded entry of a class_data_item. */
struct dex_encoded_field {
   uint32_t field_idx;
   uint32_t access_flags;
};

struct dex_encoded_method {
   uint32_t method_idx;
   uint32_t access_flags;
   uint32_t code_off;
};

struct dex_class_data {
   struct dex_encoded_field *static_fields;
   struct dex_encoded_field *instance_fields;
   struct dex_encoded_method *direct_methods;
   struct dex_encoded_method *virtual_methods;
   uint32_t static_fields_size, instance_fields_size;
   uint32_t direct_methods_size, virtual_methods_size;
};

/* An encoded_value, as found in a static_values array. */
enum dex_value_type {
   DEX_VALUE_BYTE = 0x00, DEX_VALUE_SHORT = 0x02, DEX_VALUE_CHAR = 0x03,
   DEX_VALUE_INT = 0x04, DEX_VALUE_LONG = 0x06, DEX_VALUE_FLOAT = 0x10,
   DEX_VALUE_DOUBLE = 0x11, DEX_VALUE_METHOD_TYPE = 0x15,
   DEX_VALUE_METHOD_HANDLE = 0x16, DEX_VALUE_STRING = 0x17,
   DEX_VALUE_TYPE = 0x18, DEX_VALUE_FIELD = 0x19, DEX_VALUE_METHOD = 0x1a,
   DEX_VALUE_ENUM = 0x1b, DEX_VALUE_ARRAY = 0x1c,
   DEX_VALUE_ANNOTATION = 0x1d, DEX_VALUE_NULL = 0x1e,
   DEX_VALUE_BOOLEAN = 0x1f,
};

struct dex_value {
   enum dex_value_type type;
   uint64_t bits;   /* primitive payload, or the pool index for string/type/… */
};

/* --- container ---------------------------------------------------------- */

/* Takes ownership of nothing; `data` must outlive the view. */
bool dex_open_memory(struct dex_file *d, const uint8_t *data, size_t len,
                     const char *path);
/* Reads the whole file; dex_close() frees it. */
bool dex_open(struct dex_file *d, const char *path);
void dex_close(struct dex_file *d);

/* --- pools -------------------------------------------------------------- */

/* NULL when the index is out of range.  Never NULL for a valid index. */
const char *dex_string(struct dex_file *d, uint32_t idx);
/* Type descriptor, e.g. "Ljava/lang/String;" or "[I" or "I". */
const char *dex_type(struct dex_file *d, uint32_t idx);
bool dex_method_id(const struct dex_file *d, uint32_t idx, struct dex_method_id *out);
bool dex_field_id(const struct dex_file *d, uint32_t idx, struct dex_field_id *out);
bool dex_class_def(const struct dex_file *d, uint32_t idx, struct dex_class_def *out);

/* Concatenates the string fragments in a class' dalvik.annotation.Signature
 * annotation.  The annotation is how the generic reflection API obtains
 * information erased from the ordinary superclass/type pools. */
bool dex_class_signature(struct dex_file *d, uint32_t class_def_idx,
                         char *out, size_t out_sz);
/* Generic signature of a declared field, from its system-visible
 * dalvik.annotation.Signature annotation. */
bool dex_field_signature(struct dex_file *d, uint32_t class_def_idx,
                         const char *field_name, char *out, size_t out_sz);
/* Declaring/enclosing class recorded by EnclosingClass or EnclosingMethod. */
const char *dex_class_enclosing_type(struct dex_file *d,
                                     uint32_t class_def_idx);

/* Runtime-visible class annotations.  `encoded_off` points at the
 * encoded_annotation (its type_idx, not the preceding visibility byte).
 * AnnotationDefault is kept on the annotation interface itself and is used
 * when an instance omits an element with a Java default. */
bool dex_class_annotation(struct dex_file *d, uint32_t class_def_idx,
                          const char *annotation_desc,
                          uint32_t *encoded_off);
bool dex_field_annotation(struct dex_file *d, uint32_t class_def_idx,
                          const char *field_name, const char *annotation_desc,
                          uint32_t *encoded_off);
const char *dex_annotation_type(struct dex_file *d, uint32_t encoded_off);
bool dex_annotation_element(struct dex_file *d, uint32_t encoded_off,
                            const char *name, struct dex_value *out);
bool dex_annotation_default(struct dex_file *d, uint32_t class_def_idx,
                            const char *name, struct dex_value *out);

/* Shorty of a proto: return type first, then one char per parameter. */
const char *dex_proto_shorty(struct dex_file *d, uint32_t proto_idx);
const char *dex_proto_return(struct dex_file *d, uint32_t proto_idx);
/* Parameter type descriptors.  Returns the count and fills `out` (at most
 * `max`).  Returns -1 when the proto index is invalid. */
int dex_proto_params(struct dex_file *d, uint32_t proto_idx,
                     const char **out, int max);

/* Builds "(Ljava/lang/String;I)V" into `buf`.  Returns false when it does not
 * fit or the proto is invalid. */
bool dex_proto_signature(struct dex_file *d, uint32_t proto_idx,
                         char *buf, size_t buf_sz);

/* Index of the class_def for a type descriptor, or -1. */
int dex_find_class(struct dex_file *d, const char *descriptor);

/* --- class data --------------------------------------------------------- */

/* Allocates the four arrays; free with dex_class_data_release(). */
bool dex_class_data(const struct dex_file *d, uint32_t off, struct dex_class_data *out);
void dex_class_data_release(struct dex_class_data *cd);

bool dex_code(const struct dex_file *d, uint32_t off, struct dex_code *out);

/* Interface type indices of a class_def.  Returns the count, fills at most
 * `max` entries. */
int dex_interfaces(const struct dex_file *d, uint32_t interfaces_off,
                   uint16_t *out, int max);

/* --- encoded values ----------------------------------------------------- */

/* Decodes the encoded_array at `off` (a static_values_off).  Returns the
 * element count and fills at most `max`.  Returns -1 when `off` is 0. */
int dex_static_values(const struct dex_file *d, uint32_t off,
                      struct dex_value *out, int max);
/* Decodes the array payload referenced by a DEX_VALUE_ARRAY. */
int dex_value_array(const struct dex_file *d, const struct dex_value *array,
                    struct dex_value *out, int max);

/* --- low level (exposed for the interpreter's payload decoding) ---------- */

static inline uint32_t dex_u32_at(const struct dex_file *d, size_t off)
{
   if (off + 4 > d->len) return 0;
   uint32_t v;
   __builtin_memcpy(&v, d->p + off, 4);
   return v;
}

static inline uint16_t dex_u16_at(const struct dex_file *d, size_t off)
{
   if (off + 2 > d->len) return 0;
   uint16_t v;
   __builtin_memcpy(&v, d->p + off, 2);
   return v;
}

size_t dex_uleb(const struct dex_file *d, size_t off, uint32_t *out);
size_t dex_sleb(const struct dex_file *d, size_t off, int32_t *out);
