/* android.os.Parcel: the flat, 4-byte aligned, little-endian container that
 * SafeParcelable, Intent extras and Bundle marshalling are built on.  Layouts
 * follow frameworks/base/core/java/android/os/Parcel.java and BaseBundle.java
 * (Android 12): strings are UTF-16 with a char count, a Bundle is
 * length/BNDL magic/count/(key, typed value)*, a Parcelable is its class name
 * followed by its own writeToParcel().  Included by dvm_runtime.c after the
 * collection helpers it uses. */

enum {
   PARCEL_VAL_NULL = -1, PARCEL_VAL_STRING = 0, PARCEL_VAL_INTEGER = 1,
   PARCEL_VAL_MAP = 2, PARCEL_VAL_BUNDLE = 3, PARCEL_VAL_PARCELABLE = 4,
   PARCEL_VAL_SHORT = 5, PARCEL_VAL_LONG = 6, PARCEL_VAL_FLOAT = 7,
   PARCEL_VAL_DOUBLE = 8, PARCEL_VAL_BOOLEAN = 9, PARCEL_VAL_CHARSEQUENCE = 10,
   PARCEL_VAL_LIST = 11, PARCEL_VAL_BYTEARRAY = 13, PARCEL_VAL_STRINGARRAY = 14,
   PARCEL_VAL_PARCELABLEARRAY = 16, PARCEL_VAL_INTARRAY = 18,
   PARCEL_VAL_LONGARRAY = 19, PARCEL_VAL_BYTE = 20
};
#define PARCEL_BUNDLE_MAGIC 0x4C444E42

struct rt_parcel { uint8_t *buf; uint32_t size, cap, pos; bool ipc, failed; unsigned depth; };

static void parcel_destroy(void *data)
{
   struct rt_parcel *p = data;
   if (!p) return;
   free(p->buf);
   free(p);
}

static struct rt_parcel *parcel_of(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) return NULL;
   if (!o->data) {
      o->data = calloc(1, sizeof(struct rt_parcel));
      o->data_destroy = parcel_destroy;
   }
   return o->data;
}

static bool parcel_obtain(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/os/Parcel;");
   dvm_ref r = c ? dvm_new_object(vm, c) : 0;
   if (!r || !parcel_of(vm, r)) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel");
      return false;
   }
   RETL(r);
}

static bool parcel_recycle(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct rt_parcel *p = parcel_of(vm, self);
   if (p) { free(p->buf); memset(p, 0, sizeof *p); }
   RETV();
}

/* Room for n more bytes at the current position; the data size is the high
 * water mark, not the position, so a patched-in length does not truncate. */
static bool parcel_room(struct dvm *vm, struct rt_parcel *p, size_t n)
{
   size_t need = (size_t)p->pos + n;
   if (need > INT32_MAX || (p->ipc && need > LUNA_PROCESS_MESSAGE_MAX)) goto oom;
   if (need > p->cap) {
      size_t cap = p->cap ? p->cap : 256;
      while (cap < need) cap *= 2;
      uint8_t *b = realloc(p->buf, cap);
      if (!b) goto oom;
      memset(b + p->cap, 0, cap - p->cap);
      p->buf = b;
      p->cap = (uint32_t)cap;
   }
   return true;
oom:
   dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel");
   return false;
}

static bool parcel_put(struct dvm *vm, struct rt_parcel *p, const void *src, size_t n)
{
   size_t padded = (n + 3) & ~(size_t)3;
   if (!parcel_room(vm, p, padded)) return false;
   if (n) memcpy(p->buf + p->pos, src, n);
   if (padded > n) memset(p->buf + p->pos + n, 0, padded - n);
   p->pos += (uint32_t)padded;
   if (p->pos > p->size) p->size = p->pos;
   return true;
}

static bool parcel_put_i32(struct dvm *vm, struct rt_parcel *p, int32_t v)
{ return parcel_put(vm, p, &v, 4); }
static bool parcel_put_i64(struct dvm *vm, struct rt_parcel *p, int64_t v)
{ return parcel_put(vm, p, &v, 8); }

/* Reads past the end yield zero, as the native Parcel does. */
static bool parcel_take(struct rt_parcel *p, void *dst, size_t n)
{
   size_t padded = (n + 3) & ~(size_t)3;
   if (p->pos > p->size || padded > p->size - p->pos) {
      memset(dst, 0, n);
      p->failed = true;
      p->pos = p->size;
      return false;
   }
   memcpy(dst, p->buf + p->pos, n);
   p->pos += (uint32_t)padded;
   return true;
}
static int32_t parcel_take_i32(struct rt_parcel *p)
{ int32_t v; parcel_take(p, &v, 4); return v; }
static int64_t parcel_take_i64(struct rt_parcel *p)
{ int64_t v; parcel_take(p, &v, 8); return v; }

static bool parcel_put_string(struct dvm *vm, struct rt_parcel *p, dvm_ref s)
{
   const char *u = dvm_string_utf8(vm, s);
   if (!s || !u) return parcel_put_i32(vm, p, -1);
   size_t n = dvm_string_utf8_length(vm, s);
   uint16_t *w = malloc((n + 1) * sizeof *w);
   if (!w) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel string"); return false; }
   size_t units = 0;
   for (size_t i = 0; i < n;) {
      uint32_t c = (uint8_t)u[i++];
      unsigned extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
      if (extra) c &= 0x3fu >> extra;
      for (unsigned k = 0; k < extra && i < n; ++k) c = c << 6 | ((uint8_t)u[i++] & 0x3f);
      if (c >= 0x10000) {
         c -= 0x10000;
         w[units++] = (uint16_t)(0xd800 | (c >> 10));
         c = 0xdc00 | (c & 0x3ff);
      }
      w[units++] = (uint16_t)c;
   }
   bool ok = parcel_put_i32(vm, p, (int32_t)units);
   w[units] = 0;
   ok = ok && parcel_put(vm, p, w, (units + 1) * sizeof *w);
   free(w);
   return ok;
}

/* Returns false on an exception; *out is 0 for a null string. */
static bool parcel_take_string(struct dvm *vm, struct rt_parcel *p, dvm_ref *out)
{
   *out = 0;
   int32_t units = parcel_take_i32(p);
   if (units < 0) {if(p->ipc && units != -1)p->failed=true;return true;}
   size_t bytes = ((size_t)units + 1) * 2;
   if (p->pos > p->size || ((bytes + 3) & ~(size_t)3) > p->size - p->pos) {p->failed=true;return true;}
   const uint16_t *w = (const uint16_t *)(p->buf + p->pos);
   char *u = malloc((size_t)units * 3 + 1);
   if (!u) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel string"); return false; }
   size_t n = 0;
   for (int32_t i = 0; i < units; ++i) {
      uint32_t c = w[i];
      if (c >= 0xd800 && c < 0xdc00 && i + 1 < units && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000)
         c = 0x10000 + ((c - 0xd800) << 10) + (w[++i] - 0xdc00);
      if (c < 0x80) u[n++] = (char)c;
      else if (c < 0x800) { u[n++] = (char)(0xc0 | c >> 6); u[n++] = (char)(0x80 | (c & 0x3f)); }
      else if (c < 0x10000) {
         u[n++] = (char)(0xe0 | c >> 12); u[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
         u[n++] = (char)(0x80 | (c & 0x3f));
      } else {
         u[n++] = (char)(0xf0 | c >> 18); u[n++] = (char)(0x80 | ((c >> 12) & 0x3f));
         u[n++] = (char)(0x80 | ((c >> 6) & 0x3f)); u[n++] = (char)(0x80 | (c & 0x3f));
      }
   }
   p->pos += (uint32_t)((bytes + 3) & ~(size_t)3);
   *out = dvm_new_string_n(vm, u, n);
   free(u);
   return *out != 0;
}

static dvm_ref parcel_new_instance(struct dvm *vm, const char *desc)
{
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   return c ? dvm_new_object(vm, c) : 0;
}

/* ---- accessors on primitives ------------------------------------------- */

#define PARCEL_SELF() struct rt_parcel *p = parcel_of(vm, self); (void)args; (void)nargs; \
   if (!p) { dvm__throw(vm, "java/lang/NullPointerException", "Parcel"); return false; }

static bool parcel_dataSize(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETI(p->size); }
static bool parcel_dataPosition(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETI(p->pos); }
static bool parcel_dataAvail(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETI(p->size > p->pos ? p->size - p->pos : 0); }
static bool parcel_dataCapacity(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETI(p->cap); }
static bool parcel_setDataPosition(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t pos = ARG(0).i;
   if (pos < 0 || (uint32_t)pos > p->size) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Position %d out of range", pos);
      return false;
   }
   p->pos = (uint32_t)pos;
   RETV();
}
static bool parcel_setDataSize(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t n = ARG(0).i;
   if (n < 0) { dvm__throw(vm, "java/lang/IllegalArgumentException", "size %d", n); return false; }
   uint32_t keep = p->pos;
   p->pos = 0;
   if (!parcel_room(vm, p, (size_t)n)) { p->pos = keep; return false; }
   p->pos = keep;
   if ((uint32_t)n < p->size) memset(p->buf + n, 0, p->size - (uint32_t)n);
   p->size = (uint32_t)n;
   if (p->pos > p->size) p->pos = p->size;
   RETV();
}
static bool parcel_false(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(0); }
static bool parcel_nop(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; (void)out; RETV(); }

static bool parcel_marshall(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   dvm_ref a = dvm_new_array(vm, 'B', "B", p->size);
   if (!a) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel.marshall"); return false; }
   if (p->size) memcpy(dvm_array_data(vm, a), p->buf, p->size);
   RETL(a);
}

static bool parcel_unmarshall(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   dvm_ref a = ARG(0).l;
   int32_t off = ARG(1).i, n = ARG(2).i;
   uint32_t len = dvm_array_length(vm, a);
   if (!a) { dvm__throw(vm, "java/lang/NullPointerException", "data"); return false; }
   if (off < 0 || n < 0 || (uint32_t)off > len || (uint32_t)n > len - (uint32_t)off) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "offset %d length %d", off, n);
      return false;
   }
   p->size = p->pos = 0;
   if (!parcel_room(vm, p, (size_t)n)) return false;
   if (n) memcpy(p->buf, (uint8_t *)dvm_array_data(vm, a) + off, (size_t)n);
   p->size = p->pos = (uint32_t)n;
   RETV();
}

static bool parcel_appendFrom(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   struct rt_parcel *src = parcel_of(vm, ARG(0).l);
   int32_t off = ARG(1).i, n = ARG(2).i;
   if (!src) { dvm__throw(vm, "java/lang/NullPointerException", "parcel"); return false; }
   if (off < 0 || n < 0 || (uint32_t)off > src->size || (uint32_t)n > src->size - (uint32_t)off) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Bad range %d+%d", off, n);
      return false;
   }
   if (!parcel_put(vm, p, src->buf + off, (size_t)n)) return false;
   (void)out;
   RETV();
}

static bool parcel_writeInt(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_put_i32(vm, p, ARG(0).i); }
static bool parcel_writeLong(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_put_i64(vm, p, ARG(0).j); }
static bool parcel_writeFloat(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; float f = ARG(0).f; return parcel_put(vm, p, &f, 4); }
static bool parcel_writeDouble(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; double d = ARG(0).d; return parcel_put(vm, p, &d, 8); }
static bool parcel_writeString(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_put_string(vm, p, ARG(0).l); }

static bool parcel_readInt(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETI(parcel_take_i32(p)); }
static bool parcel_readByte(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETI((int8_t)parcel_take_i32(p)); }
static bool parcel_readBoolean(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETI(parcel_take_i32(p) != 0); }
static bool parcel_readLong(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() RETJ(parcel_take_i64(p)); }
static bool parcel_readFloat(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() float f; parcel_take(p, &f, 4); RETF(f); }
static bool parcel_readDouble(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() double d; parcel_take(p, &d, 8); RETD(d); }
static bool parcel_readString(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   dvm_ref s;
   if (!parcel_take_string(vm, p, &s) && dvm_exception(vm)) return false;
   RETL(s);
}

/* ---- primitive arrays --------------------------------------------------- */

/* kind: B bytes, Z/C as ints, I, J, F, D.  width is the element's size in
 * the array; the parcel holds Z and C widened to an int. */
static bool parcel_write_prim_array(struct dvm *vm, struct rt_parcel *p, dvm_ref a,
                                    char kind, uint32_t off, uint32_t n, bool whole)
{
   if (!a) return parcel_put_i32(vm, p, -1);
   uint32_t len = dvm_array_length(vm, a);
   if (whole) { off = 0; n = len; }
   if (off > len || n > len - off) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "offset %u length %u", off, n);
      return false;
   }
   const void *d = dvm_array_data(vm, a);
   if (!parcel_put_i32(vm, p, (int32_t)n)) return false;
   switch (kind) {
   case 'B': return parcel_put(vm, p, (const uint8_t *)d + off, n);
   case 'I': case 'F': return parcel_put(vm, p, (const uint8_t *)d + (size_t)off * 4, (size_t)n * 4);
   case 'J': case 'D': return parcel_put(vm, p, (const uint8_t *)d + (size_t)off * 8, (size_t)n * 8);
   case 'Z':
      for (uint32_t i = 0; i < n; ++i)
         if (!parcel_put_i32(vm, p, ((const uint8_t *)d)[off + i] != 0)) return false;
      return true;
   default: /* 'C' */
      for (uint32_t i = 0; i < n; ++i)
         if (!parcel_put_i32(vm, p, ((const uint16_t *)d)[off + i])) return false;
      return true;
   }
}

static const char *parcel_prim_desc(char kind)
{ return kind == 'B' ? "B" : kind == 'I' ? "I" : kind == 'F' ? "F" : kind == 'J' ? "J" :
         kind == 'D' ? "D" : kind == 'Z' ? "Z" : "C"; }

/* Fills `a` (or a fresh array when 0) from the parcel; *length is the count
 * read, -1 for a null array. */
static dvm_ref parcel_read_prim_array(struct dvm *vm, struct rt_parcel *p, char kind,
                                      dvm_ref into, bool *exception)
{
   *exception = false;
   int32_t n = parcel_take_i32(p);
   if (n < 0) return 0;
   size_t unit = kind == 'B' ? 1 : (kind == 'J' || kind == 'D') ? 8 : 4;
   size_t bytes = (size_t)n * unit;
   if (p->pos > p->size || ((bytes + 3) & ~(size_t)3) > p->size - p->pos) { p->failed=true;p->pos = p->size; return 0; }
   dvm_ref a = into;
   if (into) {
      if (dvm_array_length(vm, into) != (uint32_t)n) {
         dvm__throw(vm, "java/lang/RuntimeException", "bad array lengths");
         *exception = true;
         return 0;
      }
   } else {
      a = dvm_new_array(vm, kind, parcel_prim_desc(kind), (uint32_t)n);
      if (!a) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel array"); *exception = true; return 0; }
   }
   uint8_t *d = dvm_array_data(vm, a);
   if (kind == 'Z') {
      for (int32_t i = 0; i < n; ++i) d[i] = parcel_take_i32(p) != 0;
   } else if (kind == 'C') {
      for (int32_t i = 0; i < n; ++i) ((uint16_t *)d)[i] = (uint16_t)parcel_take_i32(p);
   } else {
      if (n) memcpy(d, p->buf + p->pos, bytes);
      p->pos += (uint32_t)((bytes + 3) & ~(size_t)3);
   }
   return a;
}

#define PARCEL_PRIM_ARRAY(suffix, kind) \
   static bool parcel_write##suffix##Array(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                                           int nargs, union dvm_value *out) \
   { PARCEL_SELF() (void)out; return parcel_write_prim_array(vm, p, ARG(0).l, kind, 0, 0, true); } \
   static bool parcel_create##suffix##Array(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                                            int nargs, union dvm_value *out) \
   { PARCEL_SELF() bool exc; dvm_ref a = parcel_read_prim_array(vm, p, kind, 0, &exc); \
     out->l = a; return !exc; } \
   static bool parcel_read##suffix##Array(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                                          int nargs, union dvm_value *out) \
   { PARCEL_SELF() bool exc; if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "val"); return false; } \
     (void)parcel_read_prim_array(vm, p, kind, ARG(0).l, &exc); (void)out; return !exc; }
PARCEL_PRIM_ARRAY(Byte, 'B')
PARCEL_PRIM_ARRAY(Int, 'I')
PARCEL_PRIM_ARRAY(Long, 'J')
PARCEL_PRIM_ARRAY(Float, 'F')
PARCEL_PRIM_ARRAY(Double, 'D')
PARCEL_PRIM_ARRAY(Boolean, 'Z')
PARCEL_PRIM_ARRAY(Char, 'C')
#undef PARCEL_PRIM_ARRAY

static bool parcel_writeByteArrayRange(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "b"); return false; }
   return parcel_write_prim_array(vm, p, ARG(0).l, 'B', (uint32_t)ARG(1).i, (uint32_t)ARG(2).i, false);
}

/* ---- Parcelable and typed containers ------------------------------------ */

static bool parcel_write_parcelable_body(struct dvm *vm, dvm_ref self, dvm_ref v, int32_t flags)
{
   union dvm_value a[2] = { { .l = self }, { .i = flags } }, ignored = { 0 };
   return am_vcall(vm, v, "writeToParcel", "(Landroid/os/Parcel;I)V", a, 2, &ignored);
}

static bool parcel_write_class_name(struct dvm *vm, struct rt_parcel *p, dvm_ref v)
{
   struct dvm_class *c = dvm_object_class(vm, v);
   char name[512];
   snprintf(name, sizeof name, "%s", c && c->name ? c->name : "");
   for (char *q = name; *q; ++q) if (*q == '/') *q = '.';
   dvm_ref s = dvm_new_string(vm, name);
   return s && parcel_put_string(vm, p, s);
}

static bool parcel_writeParcelable(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   dvm_ref v = ARG(0).l;
   if (!v) return parcel_put_string(vm, p, 0);
   return parcel_write_class_name(vm, p, v) && parcel_write_parcelable_body(vm, self, v, ARG(1).i);
}

/* The CREATOR a Parcelable class declares, read after its class name. */
static bool parcel_creator_for(struct dvm *vm, dvm_ref name, dvm_ref *creator)
{
   char internal[512];
   const char *dotted = dvm_string_utf8(vm, name);
   snprintf(internal, sizeof internal, "%s", dotted ? dotted : "");
   for (char *q = internal; *q; ++q) if (*q == '.') *q = '/';
   struct dvm_class *c = dvm_find_class(vm, internal);
   if (!c) {
      dvm__throw(vm, "android/os/BadParcelableException", "ClassNotFoundException when unmarshalling: %s", internal);
      return false;
   }
   union dvm_value v = { 0 };
   if (!dvm_init_class(vm, c)) return false;
   if (!dvm_get_static(vm, c, "CREATOR", "Landroid/os/Parcelable$Creator;", &v) || !v.l) {
      if (!dvm_exception(vm))
         dvm__throw(vm, "android/os/BadParcelableException", "Parcelable protocol requires a Parcelable.Creator object called CREATOR on class %s", internal);
      return false;
   }
   *creator = v.l;
   return true;
}

static bool parcel_create_from(struct dvm *vm, dvm_ref self, dvm_ref creator, dvm_ref *result)
{
   union dvm_value a = { .l = self }, r = { 0 };
   if (!am_vcall(vm, creator, "createFromParcel", "(Landroid/os/Parcel;)Ljava/lang/Object;", &a, 1, &r))
      return false;
   *result = r.l;
   return true;
}

static bool parcel_readParcelable(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   dvm_ref name, creator, r;
   if (!parcel_take_string(vm, p, &name) && dvm_exception(vm)) return false;
   if (!name) RETL(0);
   if (!parcel_creator_for(vm, name, &creator) || !parcel_create_from(vm, self, creator, &r)) return false;
   RETL(r);
}

static bool parcel_writeTypedObject(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   if (!ARG(0).l) return parcel_put_i32(vm, p, 0);
   return parcel_put_i32(vm, p, 1) && parcel_write_parcelable_body(vm, self, ARG(0).l, ARG(1).i);
}

static bool parcel_readTypedObject(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   if (!parcel_take_i32(p)) RETL(0);
   dvm_ref r;
   if (!parcel_create_from(vm, self, ARG(0).l, &r)) return false;
   RETL(r);
}

/* Android writes a typed array/list as its length, then each element as a
 * presence int followed by its writeToParcel(). */
static bool parcel_write_typed_elements(struct dvm *vm, dvm_ref self, struct rt_parcel *p,
                                        dvm_ref (*at)(struct dvm *, dvm_ref, uint32_t),
                                        dvm_ref container, uint32_t n, int32_t flags)
{
   if (!parcel_put_i32(vm, p, (int32_t)n)) return false;
   for (uint32_t i = 0; i < n; ++i) {
      dvm_ref e = at(vm, container, i);
      if (!e) { if (!parcel_put_i32(vm, p, 0)) return false; continue; }
      if (!parcel_put_i32(vm, p, 1) || !parcel_write_parcelable_body(vm, self, e, flags)) return false;
   }
   return true;
}
static dvm_ref parcel_array_at(struct dvm *vm, dvm_ref a, uint32_t i)
{ return ((const uint32_t *)dvm_array_data(vm, a))[i]; }
static dvm_ref parcel_list_at(struct dvm *vm, dvm_ref l, uint32_t i)
{
   union dvm_value a = { .i = (int32_t)i }, r = { 0 };
   return am_vcall(vm, l, "get", "(I)Ljava/lang/Object;", &a, 1, &r) ? r.l : 0;
}
static bool parcel_list_size(struct dvm *vm, dvm_ref l, uint32_t *n)
{
   union dvm_value r = { 0 };
   if (!am_vcall(vm, l, "size", "()I", NULL, 0, &r)) return false;
   *n = (uint32_t)r.i;
   return true;
}

static bool parcel_writeTypedArray(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   if (!ARG(0).l) return parcel_put_i32(vm, p, -1);
   return parcel_write_typed_elements(vm, self, p, parcel_array_at, ARG(0).l,
                                      dvm_array_length(vm, ARG(0).l), ARG(1).i);
}

static bool parcel_writeTypedList(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   uint32_t n;
   if (!ARG(0).l) return parcel_put_i32(vm, p, -1);
   if (!parcel_list_size(vm, ARG(0).l, &n)) return false;
   return parcel_write_typed_elements(vm, self, p, parcel_list_at, ARG(0).l, n, 0);
}

static bool parcel_createTypedArray(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t n = parcel_take_i32(p);
   if (n < 0) RETL(0);
   union dvm_value len = { .i = n }, arr = { 0 };
   if (!am_vcall(vm, ARG(0).l, "newArray", "(I)[Ljava/lang/Object;", &len, 1, &arr)) return false;
   dvm_pin(vm, arr.l);
   for (int32_t i = 0; i < n; ++i) {
      dvm_ref e = 0;
      if (parcel_take_i32(p) && !parcel_create_from(vm, self, ARG(0).l, &e)) { dvm_unpin(vm, arr.l); return false; }
      ((uint32_t *)dvm_array_data(vm, arr.l))[i] = e;
   }
   dvm_unpin(vm, arr.l);
   RETL(arr.l);
}

static bool parcel_createTypedArrayList(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t n = parcel_take_i32(p);
   if (n < 0) RETL(0);
   dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!list) { dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList"); return false; }
   dvm_pin(vm, list);
   for (int32_t i = 0; i < n; ++i) {
      dvm_ref e = 0;
      union dvm_value add = { 0 }, ignored = { 0 };
      if (parcel_take_i32(p) && !parcel_create_from(vm, self, ARG(0).l, &e)) { dvm_unpin(vm, list); return false; }
      add.l = e;
      if (!list_add(vm, list, &add, 1, &ignored)) { dvm_unpin(vm, list); return false; }
   }
   dvm_unpin(vm, list);
   RETL(list);
}

/* ---- strings in containers --------------------------------------------- */

static bool parcel_writeStringArray(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   dvm_ref a = ARG(0).l;
   if (!a) return parcel_put_i32(vm, p, -1);
   uint32_t n = dvm_array_length(vm, a);
   if (!parcel_put_i32(vm, p, (int32_t)n)) return false;
   for (uint32_t i = 0; i < n; ++i)
      if (!parcel_put_string(vm, p, ((const uint32_t *)dvm_array_data(vm, a))[i])) return false;
   return true;
}

static bool parcel_writeStringList(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   uint32_t n;
   if (!ARG(0).l) return parcel_put_i32(vm, p, -1);
   if (!parcel_list_size(vm, ARG(0).l, &n) || !parcel_put_i32(vm, p, (int32_t)n)) return false;
   for (uint32_t i = 0; i < n; ++i) {
      dvm_ref s = parcel_list_at(vm, ARG(0).l, i);
      if (dvm_exception(vm) || !parcel_put_string(vm, p, s)) return false;
   }
   return true;
}

static bool parcel_createStringArray(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t n = parcel_take_i32(p);
   if (n < 0) RETL(0);
   if ((uint32_t)n > (p->size - (p->pos > p->size ? p->size : p->pos)) / 4) { p->failed=true;p->pos = p->size; RETL(0); }
   dvm_ref a = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
   if (!a) { dvm__throw(vm, "java/lang/OutOfMemoryError", "String[]"); return false; }
   dvm_pin(vm, a);
   for (int32_t i = 0; i < n; ++i) {
      dvm_ref s;
      if (!parcel_take_string(vm, p, &s) && dvm_exception(vm)) { dvm_unpin(vm, a); return false; }
      ((uint32_t *)dvm_array_data(vm, a))[i] = s;
   }
   dvm_unpin(vm, a);
   RETL(a);
}

static bool parcel_readStringArray(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   uint32_t at = p->pos;
   int32_t n = parcel_take_i32(p);
   if (n != (int32_t)dvm_array_length(vm, ARG(0).l)) {
      p->pos = at;
      dvm__throw(vm, "java/lang/RuntimeException", "bad array lengths");
      return false;
   }
   for (int32_t i = 0; i < n; ++i) {
      dvm_ref s;
      if (!parcel_take_string(vm, p, &s) && dvm_exception(vm)) return false;
      ((uint32_t *)dvm_array_data(vm, ARG(0).l))[i] = s;
   }
   (void)out;
   RETV();
}

static bool parcel_createStringArrayList(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t n = parcel_take_i32(p);
   if (n < 0) RETL(0);
   dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!list) { dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList"); return false; }
   dvm_pin(vm, list);
   for (int32_t i = 0; i < n; ++i) {
      dvm_ref s;
      union dvm_value add = { 0 }, ignored = { 0 };
      if (!parcel_take_string(vm, p, &s) && dvm_exception(vm)) { dvm_unpin(vm, list); return false; }
      add.l = s;
      if (!list_add(vm, list, &add, 1, &ignored)) { dvm_unpin(vm, list); return false; }
   }
   dvm_unpin(vm, list);
   RETL(list);
}

/* ---- generic values, Map, List, Bundle ---------------------------------- */

static bool parcel_write_value_inner(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref v);
static bool parcel_write_value(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref v)
{
   if(p->ipc && (p->failed || p->depth>=64)){
      p->failed=true;dvm__throw(vm,"android/os/BadParcelableException","IPC Parcel nesting or bounds");return false;
   }
   ++p->depth;
   bool ok=parcel_write_value_inner(vm,self,p,v);
   --p->depth;return ok;
}

static bool parcel_read_value_inner(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out);
static bool parcel_read_value(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out)
{
   if(p->ipc && (p->failed || p->depth>=64)){
      p->failed=true;dvm__throw(vm,"android/os/BadParcelableException","IPC Parcel nesting or bounds");return false;
   }
   ++p->depth;
   bool ok=parcel_read_value_inner(vm,self,p,out);
   --p->depth;return ok;
}

static bool parcel_value_is(struct dvm *vm, dvm_ref v, const char *desc)
{
   struct dvm_class *c = dvm_object_class(vm, v), *t = dvm__class_by_desc(vm, desc);
   return c && t && dvm__class_assignable(vm, c, t);
}

static bool parcel_write_map_body(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref map)
{
   /* A Bundle is keyed by String and is not a java.util.Map. */
   const bool bundle = parcel_value_is(vm, map, "Landroid/os/Bundle;");
   const char *get_sig = bundle ? "(Ljava/lang/String;)Ljava/lang/Object;" : "(Ljava/lang/Object;)Ljava/lang/Object;";
   union dvm_value n = { 0 }, ks = { 0 }, keys = { 0 };
   if (!am_vcall(vm, map, "size", "()I", NULL, 0, &n) ||
       !am_vcall(vm, map, "keySet", "()Ljava/util/Set;", NULL, 0, &ks) ||
       !am_vcall(vm, ks.l, "toArray", "()[Ljava/lang/Object;", NULL, 0, &keys))
      return false;
   dvm_pin(vm, keys.l);
   uint32_t count = dvm_array_length(vm, keys.l);
   bool ok = parcel_put_i32(vm, p, (int32_t)count);
   for (uint32_t i = 0; ok && i < count; ++i) {
      dvm_ref k = ((const uint32_t *)dvm_array_data(vm, keys.l))[i];
      union dvm_value a = { .l = k }, val = { 0 };
      ok = parcel_write_value(vm, self, p, k) &&
           am_vcall(vm, map, "get", get_sig, &a, 1, &val) &&
           parcel_write_value(vm, self, p, val.l);
   }
   dvm_unpin(vm, keys.l);
   return ok;
}

/* BaseBundle.writeToParcelInner: an empty bundle is a single zero. */
static bool parcel_write_bundle(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref b)
{
   if (!b) return parcel_put_i32(vm, p, -1);
   union dvm_value n = { 0 };
   if (!am_vcall(vm, b, "size", "()I", NULL, 0, &n)) return false;
   if (n.i == 0) return parcel_put_i32(vm, p, 0);
   uint32_t length_at = p->pos;
   if (!parcel_put_i32(vm, p, -1) || !parcel_put_i32(vm, p, PARCEL_BUNDLE_MAGIC)) return false;
   uint32_t start = p->pos;
   if (!parcel_write_map_body(vm, self, p, b)) return false;
   uint32_t end = p->pos;
   p->pos = length_at;
   if (!parcel_put_i32(vm, p, (int32_t)(end - start))) return false;
   p->pos = end;
   return true;
}

static bool parcel_put_pair(struct dvm *vm, dvm_ref map, dvm_ref k, dvm_ref v)
{
   union dvm_value kv[2] = { { .l = k }, { .l = v } }, ignored = { 0 };
   return map_put(vm, map, kv, 2, &ignored);
}

static bool parcel_read_map_into(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref map, int32_t n)
{
   if(p->ipc && (n<0 || p->pos>p->size || (uint32_t)n>(p->size-p->pos)/8u)){
      p->failed=true;return false;
   }
   for (int32_t i = 0; i < n; ++i) {
      dvm_ref k, v;
      if (!parcel_read_value(vm, self, p, &k)) return false;
      dvm_pin(vm, k);
      bool ok = parcel_read_value(vm, self, p, &v) && parcel_put_pair(vm, map, k, v);
      dvm_unpin(vm, k);
      if (!ok) return false;
   }
   return true;
}

static bool parcel_read_bundle(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out)
{
   *out = 0;
   int32_t length = parcel_take_i32(p);
   if (length < 0) {if(p->ipc && length != -1)p->failed=true;return true;}
   dvm_ref b = parcel_new_instance(vm, "Landroid/os/Bundle;");
   union dvm_value ignored = { 0 };
   if (!b || !map_init(vm, b, NULL, 0, &ignored)) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "Bundle");
      return false;
   }
   *out = b;
   if (length == 0) return true;
   if (parcel_take_i32(p) != PARCEL_BUNDLE_MAGIC) {
      dvm__throw(vm, "java/lang/IllegalStateException", "Bad magic number for Bundle");
      return false;
   }
   uint32_t start = p->pos;
   if(p->ipc && ((uint32_t)length>p->size-start || length<4)){p->failed=true;return false;}
   int32_t n = parcel_take_i32(p);
   dvm_pin(vm, b);
   bool ok = parcel_read_map_into(vm, self, p, b, n);
   dvm_unpin(vm, b);
   if(p->ipc && p->pos!=start+(uint32_t)length){p->failed=true;return false;}
   p->pos = start + (uint32_t)length > p->size ? p->size : start + (uint32_t)length;
   return ok;
}

static bool parcel_write_value_inner(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref v)
{
   if (!v) return parcel_put_i32(vm, p, PARCEL_VAL_NULL);
   struct dvm_object *o = dvm__obj(vm, v);
   const char *cn = o && o->cls && o->cls->name ? o->cls->name : "";
   if (o && o->kind == DVM_OBJ_STRING)
      return parcel_put_i32(vm, p, PARCEL_VAL_STRING) && parcel_put_string(vm, p, v);
   if (!strcmp(cn, "java/lang/Integer"))
      return parcel_put_i32(vm, p, PARCEL_VAL_INTEGER) && parcel_put_i32(vm, p, box_get(vm, v).i);
   if (!strcmp(cn, "java/lang/Long"))
      return parcel_put_i32(vm, p, PARCEL_VAL_LONG) && parcel_put_i64(vm, p, box_get(vm, v).j);
   if (!strcmp(cn, "java/lang/Short"))
      return parcel_put_i32(vm, p, PARCEL_VAL_SHORT) && parcel_put_i32(vm, p, box_get(vm, v).i);
   if (!strcmp(cn, "java/lang/Byte"))
      return parcel_put_i32(vm, p, PARCEL_VAL_BYTE) && parcel_put_i32(vm, p, box_get(vm, v).i);
   if (!strcmp(cn, "java/lang/Boolean"))
      return parcel_put_i32(vm, p, PARCEL_VAL_BOOLEAN) && parcel_put_i32(vm, p, box_get(vm, v).i != 0);
   if (!strcmp(cn, "java/lang/Float")) {
      float f = box_get(vm, v).f;
      return parcel_put_i32(vm, p, PARCEL_VAL_FLOAT) && parcel_put(vm, p, &f, 4);
   }
   if (!strcmp(cn, "java/lang/Double")) {
      double d = box_get(vm, v).d;
      return parcel_put_i32(vm, p, PARCEL_VAL_DOUBLE) && parcel_put(vm, p, &d, 8);
   }
   if (o && o->kind == DVM_OBJ_ARRAY) {
      switch (o->elem_kind) {
      case 'B': return parcel_put_i32(vm, p, PARCEL_VAL_BYTEARRAY) && parcel_write_prim_array(vm, p, v, 'B', 0, 0, true);
      case 'I': return parcel_put_i32(vm, p, PARCEL_VAL_INTARRAY) && parcel_write_prim_array(vm, p, v, 'I', 0, 0, true);
      case 'J': return parcel_put_i32(vm, p, PARCEL_VAL_LONGARRAY) && parcel_write_prim_array(vm, p, v, 'J', 0, 0, true);
      default: break;
      }
      if (o->cls && o->cls->name && !strcmp(o->cls->name, "[Ljava/lang/String;")) {
         union dvm_value a = { .l = v }, ignored = { 0 };
         return parcel_put_i32(vm, p, PARCEL_VAL_STRINGARRAY) &&
                parcel_writeStringArray(vm, self, &a, 1, &ignored);
      }
   } else {
      if (parcel_value_is(vm, v, "Landroid/os/Bundle;"))
         return parcel_put_i32(vm, p, PARCEL_VAL_BUNDLE) && parcel_write_bundle(vm, self, p, v);
      if (parcel_value_is(vm, v, "Ljava/util/Map;"))
         return parcel_put_i32(vm, p, PARCEL_VAL_MAP) && parcel_write_map_body(vm, self, p, v);
      if (parcel_value_is(vm, v, "Ljava/util/List;")) {
         uint32_t n;
         if (!parcel_put_i32(vm, p, PARCEL_VAL_LIST) || !parcel_list_size(vm, v, &n) ||
             !parcel_put_i32(vm, p, (int32_t)n)) return false;
         for (uint32_t i = 0; i < n; ++i) {
            dvm_ref e = parcel_list_at(vm, v, i);
            if (dvm_exception(vm) || !parcel_write_value(vm, self, p, e)) return false;
         }
         return true;
      }
      if (p->ipc && parcel_value_is(vm,v,"Landroid/os/Parcelable;")) {
         dvm__throw(vm,"java/lang/UnsupportedOperationException","Parcelable IPC transport is not implemented for %s",cn);return false;
      }
      if (parcel_value_is(vm, v, "Landroid/os/Parcelable;"))
         return parcel_put_i32(vm, p, PARCEL_VAL_PARCELABLE) && parcel_write_class_name(vm, p, v) &&
                parcel_write_parcelable_body(vm, self, v, 0);
      if (parcel_value_is(vm, v, "Ljava/lang/CharSequence;")) {
         union dvm_value s = { 0 };
         if (!am_vcall(vm, v, "toString", "()Ljava/lang/String;", NULL, 0, &s)) return false;
         return parcel_put_i32(vm, p, PARCEL_VAL_CHARSEQUENCE) && parcel_put_i32(vm, p, 1) &&
                parcel_put_string(vm, p, s.l);
      }
   }
   dvm__throw(vm, "java/lang/RuntimeException", "Parcel: unable to marshal value %s", cn);
   return false;
}

static bool parcel_read_value_inner(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out)
{
   *out = 0;
   int32_t type = parcel_take_i32(p);
   union dvm_value v = { 0 };
   bool exc;
   switch (type) {
   case PARCEL_VAL_NULL: return true;
   case PARCEL_VAL_STRING: return parcel_take_string(vm, p, out) || !dvm_exception(vm);
   case PARCEL_VAL_INTEGER: v.i = parcel_take_i32(p); *out = box_make(vm, "Ljava/lang/Integer;", v); return true;
   case PARCEL_VAL_SHORT: v.i = (int16_t)parcel_take_i32(p); *out = box_make(vm, "Ljava/lang/Short;", v); return true;
   case PARCEL_VAL_BYTE: v.i = (int8_t)parcel_take_i32(p); *out = box_make(vm, "Ljava/lang/Byte;", v); return true;
   case PARCEL_VAL_BOOLEAN: v.i = parcel_take_i32(p) != 0; *out = box_make(vm, "Ljava/lang/Boolean;", v); return true;
   case PARCEL_VAL_LONG: v.j = parcel_take_i64(p); *out = box_make(vm, "Ljava/lang/Long;", v); return true;
   case PARCEL_VAL_FLOAT: parcel_take(p, &v.f, 4); *out = box_make(vm, "Ljava/lang/Float;", v); return true;
   case PARCEL_VAL_DOUBLE: parcel_take(p, &v.d, 8); *out = box_make(vm, "Ljava/lang/Double;", v); return true;
   case PARCEL_VAL_CHARSEQUENCE: parcel_take_i32(p); return parcel_take_string(vm, p, out) || !dvm_exception(vm);
   case PARCEL_VAL_BUNDLE: return parcel_read_bundle(vm, self, p, out);
   case PARCEL_VAL_BYTEARRAY: *out = parcel_read_prim_array(vm, p, 'B', 0, &exc); return !exc;
   case PARCEL_VAL_INTARRAY: *out = parcel_read_prim_array(vm, p, 'I', 0, &exc); return !exc;
   case PARCEL_VAL_LONGARRAY: *out = parcel_read_prim_array(vm, p, 'J', 0, &exc); return !exc;
   case PARCEL_VAL_STRINGARRAY: {
      union dvm_value r = { 0 };
      if (!parcel_createStringArray(vm, self, NULL, 0, &r)) return false;
      *out = r.l;
      return true;
   }
   case PARCEL_VAL_MAP: {
      int32_t n = parcel_take_i32(p);
      dvm_ref m = coll_new_map(vm);
      if (!m) { dvm__throw(vm, "java/lang/OutOfMemoryError", "HashMap"); return false; }
      dvm_pin(vm, m);
      bool ok = parcel_read_map_into(vm, self, p, m, n);
      dvm_unpin(vm, m);
      *out = m;
      return ok;
   }
   case PARCEL_VAL_LIST: {
      int32_t n = parcel_take_i32(p);
      if(p->ipc && (n<0 || p->pos>p->size || (uint32_t)n>(p->size-p->pos)/4u)){p->failed=true;return false;}
      dvm_ref l = coll_new_list(vm, "Ljava/util/ArrayList;");
      if (!l) { dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList"); return false; }
      dvm_pin(vm, l);
      for (int32_t i = 0; i < n; ++i) {
         dvm_ref e;
         union dvm_value add = { 0 }, ignored = { 0 };
         if (!parcel_read_value(vm, self, p, &e)) { dvm_unpin(vm, l); return false; }
         add.l = e;
         if (!list_add(vm, l, &add, 1, &ignored)) { dvm_unpin(vm, l); return false; }
      }
      dvm_unpin(vm, l);
      *out = l;
      return true;
   }
   case PARCEL_VAL_PARCELABLE: {
      if(p->ipc){p->failed=true;return false;}
      dvm_ref name, creator;
      if (!parcel_take_string(vm, p, &name) && dvm_exception(vm)) return false;
      if (!name) return true;
      return parcel_creator_for(vm, name, &creator) && parcel_create_from(vm, self, creator, out);
   }
   default:
      dvm__throw(vm, "java/lang/RuntimeException", "Parcel: unknown value type %d", type);
      return false;
   }
}

static bool parcel_writeValue(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_write_value(vm, self, p, ARG(0).l); }
static bool parcel_readValue(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   dvm_ref r;
   if (!parcel_read_value(vm, self, p, &r)) return false;
   RETL(r);
}
static bool parcel_writeBundle(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_write_bundle(vm, self, p, ARG(0).l); }
static bool parcel_readBundle(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   dvm_ref b;
   if (!parcel_read_bundle(vm, self, p, &b)) return false;
   RETL(b);
}
static bool parcel_writeMap(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   if (!ARG(0).l) return parcel_put_i32(vm, p, -1);
   return parcel_write_map_body(vm, self, p, ARG(0).l);
}
static bool parcel_writeList(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   uint32_t n;
   if (!ARG(0).l) return parcel_put_i32(vm, p, -1);
   if (!parcel_list_size(vm, ARG(0).l, &n) || !parcel_put_i32(vm, p, (int32_t)n)) return false;
   for (uint32_t i = 0; i < n; ++i) {
      dvm_ref e = parcel_list_at(vm, ARG(0).l, i);
      if (dvm_exception(vm) || !parcel_write_value(vm, self, p, e)) return false;
   }
   return true;
}
static bool parcel_readArrayList(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t n = parcel_take_i32(p);
   if (n < 0) RETL(0);
   dvm_ref l = coll_new_list(vm, "Ljava/util/ArrayList;");
   if (!l) { dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList"); return false; }
   dvm_pin(vm, l);
   for (int32_t i = 0; i < n; ++i) {
      dvm_ref e;
      union dvm_value add = { 0 }, ignored = { 0 };
      if (!parcel_read_value(vm, self, p, &e)) { dvm_unpin(vm, l); return false; }
      add.l = e;
      if (!list_add(vm, l, &add, 1, &ignored)) { dvm_unpin(vm, l); return false; }
   }
   dvm_unpin(vm, l);
   RETL(l);
}
static bool parcel_readHashMap(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   int32_t n = parcel_take_i32(p);
   if (n < 0) RETL(0);
   dvm_ref m = coll_new_map(vm);
   if (!m) { dvm__throw(vm, "java/lang/OutOfMemoryError", "HashMap"); return false; }
   dvm_pin(vm, m);
   bool ok = parcel_read_map_into(vm, self, p, m, n);
   dvm_unpin(vm, m);
   if (!ok) return false;
   RETL(m);
}

/* IPC-only members.  There is no remote process behind a parcel here, so a
 * binder is a null handle and an interface token is just its strict-mode
 * word plus the descriptor, checked on the way back in. */
static bool parcel_writeStrongBinder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_put_i32(vm, p, 0); }
static bool parcel_readStrongBinder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() parcel_take_i32(p); RETL(0); }
static bool parcel_writeInterfaceToken(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_put_i32(vm, p, 0) && parcel_put_string(vm, p, ARG(0).l); }
static bool parcel_enforceInterface(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF()
   dvm_ref token;
   parcel_take_i32(p);
   if (!parcel_take_string(vm, p, &token) && dvm_exception(vm)) return false;
   const char *want = dvm_string_utf8(vm, ARG(0).l), *got = dvm_string_utf8(vm, token);
   if (!want || !got || strcmp(want, got)) {
      dvm__throw(vm, "java/lang/SecurityException", "Binder invocation to an incorrect interface");
      return false;
   }
   (void)out;
   RETV();
}
static bool parcel_writeNoException(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{ PARCEL_SELF() (void)out; return parcel_put_i32(vm, p, 0); }
static bool parcel_readException(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   PARCEL_SELF() (void)out;
   int32_t code = parcel_take_i32(p);
   if (!code) RETV();
   dvm_ref msg;
   if (!parcel_take_string(vm, p, &msg) && dvm_exception(vm)) return false;
   dvm__throw(vm, "java/lang/RuntimeException", "remote exception %d: %s", code,
              dvm_string_utf8(vm, msg) ? dvm_string_utf8(vm, msg) : "");
   return false;
}

/* Bundle.writeToParcel() is Parcel.writeBundle() seen from the Bundle. */
static bool bundle_writeToParcel(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   struct rt_parcel *p = parcel_of(vm, ARG(0).l);
   if (!p) { dvm__throw(vm, "java/lang/NullPointerException", "parcel"); return false; }
   (void)out;
   return parcel_write_bundle(vm, ARG(0).l, p, self);
}

/* Bundle.readFromParcel(): replace this bundle's contents with the parcel's. */
static bool bundle_readFromParcel(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
   struct rt_parcel *p = parcel_of(vm, ARG(0).l);
   if (!p) { dvm__throw(vm, "java/lang/NullPointerException", "parcel"); return false; }
   dvm_ref read;
   if (!parcel_read_bundle(vm, ARG(0).l, p, &read)) return false;
   union dvm_value none = { 0 }, ignored = { 0 };
   (void)am_vcall(vm, self, "clear", "()V", &none, 0, &ignored);
   if (read) {
      struct dvm_object *o = dvm__obj(vm, self), *r = dvm__obj(vm, read);
      struct rt_map *src = r ? r->data : NULL, *dst = o ? o->data : NULL;
      for (uint32_t i = 0; src && dst && i < src->size; ++i)
         if (!parcel_put_pair(vm, self, src->e[i].k, src->e[i].v)) return false;
   }
   (void)out;
   RETV();
}

static const struct rt_method rt_parcel_methods[] = {
   SM("obtain", "()Landroid/os/Parcel;", parcel_obtain),
   M("recycle", "()V", parcel_recycle),
   M("dataSize", "()I", parcel_dataSize),
   M("dataPosition", "()I", parcel_dataPosition),
   M("dataAvail", "()I", parcel_dataAvail),
   M("dataCapacity", "()I", parcel_dataCapacity),
   M("setDataPosition", "(I)V", parcel_setDataPosition),
   M("setDataSize", "(I)V", parcel_setDataSize),
   M("setDataCapacity", "(I)V", parcel_nop),
   M("hasFileDescriptors", "()Z", parcel_false),
   M("marshall", "()[B", parcel_marshall),
   M("unmarshall", "([BII)V", parcel_unmarshall),
   M("appendFrom", "(Landroid/os/Parcel;II)V", parcel_appendFrom),
   M("writeInt", "(I)V", parcel_writeInt),
   M("writeByte", "(B)V", parcel_writeInt),
   M("writeBoolean", "(Z)V", parcel_writeInt),
   M("writeLong", "(J)V", parcel_writeLong),
   M("writeFloat", "(F)V", parcel_writeFloat),
   M("writeDouble", "(D)V", parcel_writeDouble),
   M("writeString", "(Ljava/lang/String;)V", parcel_writeString),
   M("writeString8", "(Ljava/lang/String;)V", parcel_writeString),
   M("writeString16", "(Ljava/lang/String;)V", parcel_writeString),
   M("readInt", "()I", parcel_readInt),
   M("readByte", "()B", parcel_readByte),
   M("readBoolean", "()Z", parcel_readBoolean),
   M("readLong", "()J", parcel_readLong),
   M("readFloat", "()F", parcel_readFloat),
   M("readDouble", "()D", parcel_readDouble),
   M("readString", "()Ljava/lang/String;", parcel_readString),
   M("readString8", "()Ljava/lang/String;", parcel_readString),
   M("readString16", "()Ljava/lang/String;", parcel_readString),
   M("writeByteArray", "([B)V", parcel_writeByteArray),
   M("writeByteArray", "([BII)V", parcel_writeByteArrayRange),
   M("createByteArray", "()[B", parcel_createByteArray),
   M("readByteArray", "([B)V", parcel_readByteArray),
   M("writeIntArray", "([I)V", parcel_writeIntArray),
   M("createIntArray", "()[I", parcel_createIntArray),
   M("readIntArray", "([I)V", parcel_readIntArray),
   M("writeLongArray", "([J)V", parcel_writeLongArray),
   M("createLongArray", "()[J", parcel_createLongArray),
   M("readLongArray", "([J)V", parcel_readLongArray),
   M("writeFloatArray", "([F)V", parcel_writeFloatArray),
   M("createFloatArray", "()[F", parcel_createFloatArray),
   M("readFloatArray", "([F)V", parcel_readFloatArray),
   M("writeDoubleArray", "([D)V", parcel_writeDoubleArray),
   M("createDoubleArray", "()[D", parcel_createDoubleArray),
   M("readDoubleArray", "([D)V", parcel_readDoubleArray),
   M("writeBooleanArray", "([Z)V", parcel_writeBooleanArray),
   M("createBooleanArray", "()[Z", parcel_createBooleanArray),
   M("readBooleanArray", "([Z)V", parcel_readBooleanArray),
   M("writeCharArray", "([C)V", parcel_writeCharArray),
   M("createCharArray", "()[C", parcel_createCharArray),
   M("readCharArray", "([C)V", parcel_readCharArray),
   M("writeStringArray", "([Ljava/lang/String;)V", parcel_writeStringArray),
   M("createStringArray", "()[Ljava/lang/String;", parcel_createStringArray),
   M("readStringArray", "([Ljava/lang/String;)V", parcel_readStringArray),
   M("writeStringList", "(Ljava/util/List;)V", parcel_writeStringList),
   M("createStringArrayList", "()Ljava/util/ArrayList;", parcel_createStringArrayList),
   M("writeParcelable", "(Landroid/os/Parcelable;I)V", parcel_writeParcelable),
   M("readParcelable", "(Ljava/lang/ClassLoader;)Landroid/os/Parcelable;", parcel_readParcelable),
   M("writeTypedObject", "(Landroid/os/Parcelable;I)V", parcel_writeTypedObject),
   M("readTypedObject", "(Landroid/os/Parcelable$Creator;)Ljava/lang/Object;", parcel_readTypedObject),
   M("writeTypedArray", "([Landroid/os/Parcelable;I)V", parcel_writeTypedArray),
   M("writeParcelableArray", "([Landroid/os/Parcelable;I)V", parcel_writeTypedArray),
   M("createTypedArray", "(Landroid/os/Parcelable$Creator;)[Ljava/lang/Object;", parcel_createTypedArray),
   M("writeTypedList", "(Ljava/util/List;)V", parcel_writeTypedList),
   M("createTypedArrayList", "(Landroid/os/Parcelable$Creator;)Ljava/util/ArrayList;", parcel_createTypedArrayList),
   M("writeBundle", "(Landroid/os/Bundle;)V", parcel_writeBundle),
   M("readBundle", "()Landroid/os/Bundle;", parcel_readBundle),
   M("readBundle", "(Ljava/lang/ClassLoader;)Landroid/os/Bundle;", parcel_readBundle),
   M("writeValue", "(Ljava/lang/Object;)V", parcel_writeValue),
   M("readValue", "(Ljava/lang/ClassLoader;)Ljava/lang/Object;", parcel_readValue),
   M("writeMap", "(Ljava/util/Map;)V", parcel_writeMap),
   M("readHashMap", "(Ljava/lang/ClassLoader;)Ljava/util/HashMap;", parcel_readHashMap),
   M("writeList", "(Ljava/util/List;)V", parcel_writeList),
   M("readArrayList", "(Ljava/lang/ClassLoader;)Ljava/util/ArrayList;", parcel_readArrayList),
   M("writeStrongBinder", "(Landroid/os/IBinder;)V", parcel_writeStrongBinder),
   M("readStrongBinder", "()Landroid/os/IBinder;", parcel_readStrongBinder),
   M("writeInterfaceToken", "(Ljava/lang/String;)V", parcel_writeInterfaceToken),
   M("enforceInterface", "(Ljava/lang/String;)V", parcel_enforceInterface),
   M("writeNoException", "()V", parcel_writeNoException),
   M("readException", "()V", parcel_readException),
   M_END
};

/* Internal component transport, version 1. The envelope is not an Android
 * Intent parcel layout; its values use the same Parcel/Bundle implementation
 * as application calls. No VM object references or native pointers cross it. */
int dvm_service_pack(struct dvm *vm,uint32_t intent,int operation,void **bytes,size_t *size)
{
   *bytes=NULL;*size=0;
   if(!intent || (operation!=1 && operation!=2))return -EINVAL;
   union dvm_value parcel={0},value={0},ignored={0};
   if(!parcel_obtain(vm,0,NULL,0,&parcel))return -ENOMEM;
   dvm_pin(vm,parcel.l);dvm_pin(vm,intent);
   struct rt_parcel *p=parcel_of(vm,parcel.l);p->ipc=true;
   bool ok=parcel_put_i32(vm,p,0x3156534c) && parcel_put_i32(vm,p,operation);
   for(const struct rt_field *f=rt_intent_fields;ok && f->name;++f){
      value.l=0;(void)dvm_get_field(vm,intent,f->name,f->type,&value);
      if(!strcmp(f->name,"flags"))ok=parcel_put_i32(vm,p,value.i);
      else if(!strcmp(f->name,"data")){
         union dvm_value text={0};
         ok=!value.l || uri_toString(vm,value.l,NULL,0,&text);
         if(ok)ok=parcel_put_string(vm,p,text.l);
      } else if(!strcmp(f->name,"component")){
         ok=parcel_put_i32(vm,p,value.l!=0);
         const char *fields[]={"pkg","cls"};
         for(unsigned i=0;ok && value.l && i<2;++i){
            union dvm_value text={0};
            (void)dvm_get_field(vm,value.l,fields[i],"Ljava/lang/String;",&text);
            ok=parcel_put_string(vm,p,text.l);
         }
      } else if(!strcmp(f->name,"categories")){
         union dvm_value array={0};
         ok=!value.l || am_vcall(vm,value.l,"toArray","()[Ljava/lang/Object;",NULL,0,&array);
         uint32_t n=array.l ? dvm_array_length(vm,array.l) : 0;
         if(ok)ok=parcel_put_i32(vm,p,value.l ? (int32_t)n : -1);
         for(uint32_t i=0;ok && i<n;++i)
            ok=parcel_put_string(vm,p,((dvm_ref *)dvm_array_data(vm,array.l))[i]);
      } else ok=parcel_put_string(vm,p,value.l);
   }
   if(ok){
      ok=intent_getExtras(vm,intent,NULL,0,&value);
      if(ok){if(value.l)dvm_pin(vm,value.l);ok=parcel_write_bundle(vm,parcel.l,p,value.l);if(value.l)dvm_unpin(vm,value.l);}
   }
   if(ok && !p->failed && !dvm_exception(vm)){
      *bytes=p->buf;*size=p->size;p->buf=NULL;p->size=p->cap=p->pos=0;
   } else ok=false;
   (void)parcel_recycle(vm,parcel.l,NULL,0,&ignored);
   dvm_unpin(vm,intent);dvm_unpin(vm,parcel.l);
   return ok ? 0 : -EINVAL;
}

int dvm_service_unpack(struct dvm *vm,const void *bytes,size_t size,int *operation,uint32_t *intent)
{
   *intent=0;*operation=0;
   if(!bytes || size<8 || size>LUNA_PROCESS_MESSAGE_MAX)return -EINVAL;
   union dvm_value parcel={0},ignored={0};
   if(!parcel_obtain(vm,0,NULL,0,&parcel))return -ENOMEM;
   dvm_pin(vm,parcel.l);
   struct rt_parcel *p=parcel_of(vm,parcel.l);p->ipc=true;
   p->buf=malloc(size);bool ok=p->buf!=NULL;
   if(ok){memcpy(p->buf,bytes,size);p->size=p->cap=(uint32_t)size;}
   int op=0;
   if(ok){ok=parcel_take_i32(p)==0x3156534c;op=parcel_take_i32(p);ok=ok && (op==1 || op==2);}
   dvm_ref result=ok ? dvm_new_object(vm,dvm_find_class(vm,"android/content/Intent")) : 0;
   if(result)dvm_pin(vm,result);else ok=false;
   if(ok)ok=map_init(vm,result,NULL,0,&ignored);
   for(const struct rt_field *f=rt_intent_fields;ok && f->name;++f){
      union dvm_value value={0};
      if(!strcmp(f->name,"flags"))value.i=parcel_take_i32(p);
      else if(!strcmp(f->name,"data")){
         union dvm_value text={0};ok=parcel_take_string(vm,p,&text.l);
         if(ok && text.l)ok=uri_parse(vm,0,&text,1,&value);
      } else if(!strcmp(f->name,"component")){
         int present=parcel_take_i32(p);if(present!=0 && present!=1)ok=false;
         if(present && ok){
            value.l=dvm_new_object(vm,dvm_find_class(vm,"android/content/ComponentName"));
            if(!value.l){ok=false;break;}
            dvm_pin(vm,value.l);
            const char *fields[]={"pkg","cls"};
            for(unsigned i=0;ok && i<2;++i){
               union dvm_value text={0};ok=parcel_take_string(vm,p,&text.l) && text.l;
               if(ok)ok=dvm_set_field(vm,value.l,fields[i],"Ljava/lang/String;",text);
            }
            dvm_unpin(vm,value.l);
         }
      } else if(!strcmp(f->name,"categories")){
         int32_t n=parcel_take_i32(p);
         if(n < -1 || (n>=0 && (uint32_t)n>(p->size-p->pos)/4u)){ok=false;break;}
         if(n>=0){
            value.l=coll_new_list(vm,"Ljava/util/HashSet;");if(!value.l){ok=false;break;}
            dvm_pin(vm,value.l);
            for(int32_t i=0;ok && i<n;++i){
               union dvm_value text={0};ok=parcel_take_string(vm,p,&text.l) && text.l;
               if(ok)ok=list_add(vm,value.l,&text,1,&ignored);
            }
            dvm_unpin(vm,value.l);
         }
      } else ok=parcel_take_string(vm,p,&value.l);
      if(ok)ok=!p->failed && dvm_set_field(vm,result,f->name,f->type,value);
   }
   dvm_ref extras=0;
   if(ok)ok=parcel_read_bundle(vm,parcel.l,p,&extras);
   if(ok && extras){dvm_pin(vm,extras);union dvm_value source={.l=extras};ok=map_putAll(vm,result,&source,1,&ignored);dvm_unpin(vm,extras);}
   ok=ok && !p->failed && p->pos==p->size && !dvm_exception(vm);
   if(ok){*intent=result;*operation=op;}
   if(result)dvm_unpin(vm,result);
   (void)parcel_recycle(vm,parcel.l,NULL,0,&ignored);dvm_unpin(vm,parcel.l);
   return ok ? 0 : -EINVAL;
}

/* Called on the component process's main loop, outside the interpreter lock. */
int dvm_service_dispatch(struct dvm *vm,const void *bytes,size_t size)
{
   if(!vm)return -EINVAL;
   unsigned cookie=dvm_gil_enter_from_guest(vm);
   struct dvm_tstate saved,main_turn={0};dvm__tstate_save(vm,&saved);
   main_turn.cur_thread=vm->main_thread;dvm__tstate_load(vm,&main_turn);
   uint32_t intent=0;int op=0;
   int status=dvm_service_unpack(vm,bytes,size,&op,&intent);
   if(!status){
      dvm_pin(vm,intent);
      union dvm_value args[2]={{.l=intent},{0}},resolved={0},result={0};
      /* Only enabled, declared components in this process may be dispatched. */
      bool found=pm_resolveService(vm,0,args,2,&resolved) && resolved.l &&
                 svc_process_is_current(vm,resolved.l);
      if(found){
         bool ok=op==1 ? ctx_startService(vm,0,args,1,&result) : ctx_stopService(vm,0,args,1,&result);
         status=ok && !dvm_exception(vm) ? (result.l ? 1 : 0) : -EIO;
      } else status=dvm_exception(vm) ? -EIO : -ENOENT;
      dvm_unpin(vm,intent);
   }
   if(dvm_exception(vm)){
      fprintf(stderr,"[service] component command raised a Java exception\n");
      if(status>=0)status=-EIO;
   }
   dvm__tstate_load(vm,&saved);dvm_gil_leave_to_guest(vm,cookie);return status;
}

void dvm_service_set_submit(struct dvm *vm,int (*submit)(void *,const char *,const void *,size_t),void *user)
{
   if(!vm)return;
   unsigned cookie=dvm_gil_enter_from_guest(vm);
   vm->service_submit=submit;vm->service_submit_user=user;
   dvm_gil_leave_to_guest(vm,cookie);
}
