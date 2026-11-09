/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

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

struct parcel_fds {
  int *fds;
  size_t count;
};
static void parcel_fds_clear(struct parcel_fds *f)
{
  for(size_t i=0; i<f->count; ++i)luna_fd_close(f->fds[i]);
  free(f->fds);
  f->fds=NULL;
  f->count=0;
}
static int parcel_fds_copy(struct parcel_fds *out,const int *fds,size_t count)
{
  if(count>LUNA_PROCESS_FD_MAX || (count && !fds))return -EINVAL;
  struct parcel_fds copy= {0};
  if(count) {
    copy.fds=malloc(count*sizeof *copy.fds);
    if(!copy.fds)return -ENOMEM;
  }
  for(size_t i=0; i<count; ++i) {
    int fd=luna_fd_dup(fds[i],0,1);
    if(fd<0) {
      int error=errno;
      parcel_fds_clear(&copy);
      return -error;
    }
    copy.fds[copy.count++]=fd;
  }
  parcel_fds_clear(out);
  *out=copy;
  return 0;
}
static _Thread_local struct parcel_fds binder_incoming_fds;
struct rt_parcel {
  uint8_t *buf;
  uint32_t size, cap, pos;
  bool ipc, failed;
  unsigned depth;
  struct parcel_fds descriptors;
};

static void parcel_destroy(void *data)
{
  struct rt_parcel *p = data;
  if (!p) return;
  free(p->buf);
  parcel_fds_clear(&p->descriptors);
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
  (void)self;
  (void)args;
  (void)nargs;
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
  (void)args;
  (void)nargs;
  (void)out;
  struct rt_parcel *p = parcel_of(vm, self);
  if (p) {
    free(p->buf);
    parcel_fds_clear(&p->descriptors);
    memset(p, 0, sizeof *p);
  }
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
{
  return parcel_put(vm, p, &v, 4);
}
static bool parcel_put_i64(struct dvm *vm, struct rt_parcel *p, int64_t v)
{
  return parcel_put(vm, p, &v, 8);
}

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
{
  int32_t v;
  parcel_take(p, &v, 4);
  return v;
}
static int64_t parcel_take_i64(struct rt_parcel *p)
{
  int64_t v;
  parcel_take(p, &v, 8);
  return v;
}

/* FD records contain an attachment index, never a descriptor number from
 * another process. The transport duplicates the associated open file description. */
static bool parcel_put_fd(struct dvm *vm,struct rt_parcel *p,int fd)
{
  if(fd<0 || p->descriptors.count>=LUNA_PROCESS_FD_MAX) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid file descriptor");
    return false;
  }
  int copy=luna_fd_dup(fd,0,1);
  if(copy<0) {
    dvm__throw(vm,"java/io/IOException","dup: %s",strerror(errno));
    return false;
  }
  size_t index=p->descriptors.count;
  int *fds=realloc(p->descriptors.fds,(index+1)*sizeof *fds);
  if(!fds) {
    luna_fd_close(copy);
    dvm__throw(vm,"java/lang/OutOfMemoryError","Parcel descriptors");
    return false;
  }
  p->descriptors.fds=fds;
  fds[index]=copy;
  p->descriptors.count++;
  return parcel_put_i32(vm,p,0x66642a85) && parcel_put_i32(vm,p,0) &&
         parcel_put_i64(vm,p,(int64_t)index) && parcel_put_i64(vm,p,0);
}
static int parcel_take_fd(struct dvm *vm,struct rt_parcel *p)
{
  uint32_t type=(uint32_t)parcel_take_i32(p);
  (void)parcel_take_i32(p);
  int64_t index=parcel_take_i64(p);
  (void)parcel_take_i64(p);
  if(p->failed || type!=0x66642a85 || index<0 || (uint64_t)index>=p->descriptors.count) {
    dvm__throw(vm,"android/os/BadParcelableException","Invalid file descriptor attachment");
    return -1;
  }
  int fd=luna_fd_dup(p->descriptors.fds[index],0,1);
  if(fd<0)dvm__throw(vm,"java/io/IOException","dup: %s",strerror(errno));
  return fd;
}
static bool parcel_fd_write(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  struct rt_parcel *p=parcel_of(vm,ARG(0).l);
  if(!p) {
    dvm__throw(vm,"java/lang/NullPointerException","destination");
    return false;
  }
  union dvm_value value= {.i=-1};
  dvm_get_field(vm,self,"fd","I",&value);
  struct dvm_object *o=dvm__obj(vm,self);
  struct parcel_fd_owner *owner=o && o->data_destroy==parcel_fd_destroy ? o->data : NULL;
  int comm=owner ? owner->comm_fd : -1;
  if(!parcel_put_i32(vm,p,comm>=0) || !parcel_put_fd(vm,p,value.i) || (comm>=0 && !parcel_put_fd(vm,p,comm)))return false;
  if(ARG(1).i & 1)return parcel_fd_close(vm,self,NULL,0,out);
  RETV();
}
static bool parcel_fd_create(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self;
  (void)nargs;
  struct rt_parcel *p=parcel_of(vm,ARG(0).l);
  if(!p) {
    dvm__throw(vm,"java/lang/NullPointerException","source");
    return false;
  }
  int comm=parcel_take_i32(p);
  int fd=parcel_take_fd(vm,p);
  if(fd<0)return false;
  int extra=comm ? parcel_take_fd(vm,p) : -1;
  if(comm && extra<0) {
    luna_fd_close(fd);
    return false;
  }
  dvm_ref result=parcel_fd_adopt(vm,fd);
  if(!result) {
    if(extra>=0)luna_fd_close(extra);
    return false;
  }
  ((struct parcel_fd_owner *)dvm__obj(vm,result)->data)->comm_fd=extra;
  RETL(result);
}
static bool parcel_fd_new_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self;
  (void)nargs;
  RETL(dvm_new_array(vm,'L',"Landroid/os/ParcelFileDescriptor;",ARG(0).i));
}
static const struct rt_method rt_parcel_fd_creator[]= {
  M("createFromParcel","(Landroid/os/Parcel;)Ljava/lang/Object;",parcel_fd_create),
  M("newArray","(I)[Ljava/lang/Object;",parcel_fd_new_array),M_END
};

static bool parcel_put_string(struct dvm *vm, struct rt_parcel *p, dvm_ref s)
{
  const char *u = dvm_string_utf8(vm, s);
  if (!s || !u) return parcel_put_i32(vm, p, -1);
  size_t n = dvm_string_utf8_length(vm, s);
  uint16_t *w = malloc((n + 1) * sizeof *w);
  if (!w) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel string");
    return false;
  }
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
  if (units < 0) {
    if(p->ipc && units != -1)p->failed=true;
    return true;
  }
  size_t bytes = ((size_t)units + 1) * 2;
  if (p->pos > p->size || ((bytes + 3) & ~(size_t)3) > p->size - p->pos) {
    p->failed=true;
    return true;
  }
  const uint16_t *w = (const uint16_t *)(p->buf + p->pos);
  char *u = malloc((size_t)units * 3 + 1);
  if (!u) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel string");
    return false;
  }
  size_t n = 0;
  for (int32_t i = 0; i < units; ++i) {
    uint32_t c = w[i];
    if (c >= 0xd800 && c < 0xdc00 && i + 1 < units && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000)
      c = 0x10000 + ((c - 0xd800) << 10) + (w[++i] - 0xdc00);
    if (c < 0x80) u[n++] = (char)c;
    else if (c < 0x800) {
      u[n++] = (char)(0xc0 | c >> 6);
      u[n++] = (char)(0x80 | (c & 0x3f));
    }
    else if (c < 0x10000) {
      u[n++] = (char)(0xe0 | c >> 12);
      u[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
      u[n++] = (char)(0x80 | (c & 0x3f));
    } else {
      u[n++] = (char)(0xf0 | c >> 18);
      u[n++] = (char)(0x80 | ((c >> 12) & 0x3f));
      u[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
      u[n++] = (char)(0x80 | (c & 0x3f));
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
{
  PARCEL_SELF() RETI(p->size);
}
static bool parcel_dataPosition(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() RETI(p->pos);
}
static bool parcel_dataAvail(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() RETI(p->size > p->pos ? p->size - p->pos : 0);
}
static bool parcel_dataCapacity(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() RETI(p->cap);
}
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
  if (n < 0) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "size %d", n);
    return false;
  }
  uint32_t keep = p->pos;
  p->pos = 0;
  if (!parcel_room(vm, p, (size_t)n)) {
    p->pos = keep;
    return false;
  }
  p->pos = keep;
  if ((uint32_t)n < p->size) memset(p->buf + n, 0, p->size - (uint32_t)n);
  p->size = (uint32_t)n;
  if (p->pos > p->size) p->pos = p->size;
  RETV();
}
static bool parcel_false(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}
static bool parcel_nop(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  (void)out;
  RETV();
}

static bool parcel_marshall(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF()
  if(p->descriptors.count) {
    dvm__throw(vm,"java/lang/RuntimeException","Tried to marshall a Parcel that contains FileDescriptors");
    return false;
  }
  dvm_ref a = dvm_new_array(vm, 'B', "B", p->size);
  if (!a) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel.marshall");
    return false;
  }
  if (p->size) memcpy(dvm_array_data(vm, a), p->buf, p->size);
  RETL(a);
}

static bool parcel_unmarshall(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF()
  dvm_ref a = ARG(0).l;
  int32_t off = ARG(1).i, n = ARG(2).i;
  uint32_t len = dvm_array_length(vm, a);
  if (!a) {
    dvm__throw(vm, "java/lang/NullPointerException", "data");
    return false;
  }
  if (off < 0 || n < 0 || (uint32_t)off > len || (uint32_t)n > len - (uint32_t)off) {
    dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "offset %d length %d", off, n);
    return false;
  }
  parcel_fds_clear(&p->descriptors);
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
  if (!src) {
    dvm__throw(vm, "java/lang/NullPointerException", "parcel");
    return false;
  }
  if (off < 0 || n < 0 || (uint32_t)off > src->size || (uint32_t)n > src->size - (uint32_t)off) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "Bad range %d+%d", off, n);
    return false;
  }
  if(src->descriptors.count) {
    dvm__throw(vm,"java/lang/UnsupportedOperationException","appendFrom with file descriptors");
    return false;
  }
  if (!parcel_put(vm, p, src->buf + off, (size_t)n)) return false;
  (void)out;
  RETV();
}

static bool parcel_writeInt(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  return parcel_put_i32(vm, p, ARG(0).i);
}
static bool parcel_writeLong(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  return parcel_put_i64(vm, p, ARG(0).j);
}
static bool parcel_writeFloat(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  float f = ARG(0).f;
  return parcel_put(vm, p, &f, 4);
}
static bool parcel_writeDouble(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  double d = ARG(0).d;
  return parcel_put(vm, p, &d, 8);
}
static bool parcel_writeString(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  return parcel_put_string(vm, p, ARG(0).l);
}

static bool parcel_readInt(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() RETI(parcel_take_i32(p));
}
static bool parcel_readByte(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() RETI((int8_t)parcel_take_i32(p));
}
static bool parcel_readBoolean(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() RETI(parcel_take_i32(p) != 0);
}
static bool parcel_readLong(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() RETJ(parcel_take_i64(p));
}
static bool parcel_readFloat(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() float f;
  parcel_take(p, &f, 4);
  RETF(f);
}
static bool parcel_readDouble(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() double d;
  parcel_take(p, &d, 8);
  RETD(d);
}
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
  if (whole) {
    off = 0;
    n = len;
  }
  if (off > len || n > len - off) {
    dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "offset %u length %u", off, n);
    return false;
  }
  const void *d = dvm_array_data(vm, a);
  if (!parcel_put_i32(vm, p, (int32_t)n)) return false;
  switch (kind) {
  case 'B':
    return parcel_put(vm, p, (const uint8_t *)d + off, n);
  case 'I':
  case 'F':
    return parcel_put(vm, p, (const uint8_t *)d + (size_t)off * 4, (size_t)n * 4);
  case 'J':
  case 'D':
    return parcel_put(vm, p, (const uint8_t *)d + (size_t)off * 8, (size_t)n * 8);
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
         kind == 'D' ? "D" : kind == 'Z' ? "Z" : "C";
}

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
  if (p->pos > p->size || ((bytes + 3) & ~(size_t)3) > p->size - p->pos) {
    p->failed=true;
    p->pos = p->size;
    return 0;
  }
  dvm_ref a = into;
  if (into) {
    if (dvm_array_length(vm, into) != (uint32_t)n) {
      dvm__throw(vm, "java/lang/RuntimeException", "bad array lengths");
      *exception = true;
      return 0;
    }
  } else {
    a = dvm_new_array(vm, kind, parcel_prim_desc(kind), (uint32_t)n);
    if (!a) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "Parcel array");
      *exception = true;
      return 0;
    }
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
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "b");
    return false;
  }
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
    if (!e) {
      if (!parcel_put_i32(vm, p, 0)) return false;
      continue;
    }
    if (!parcel_put_i32(vm, p, 1) || !parcel_write_parcelable_body(vm, self, e, flags)) return false;
  }
  return true;
}
static dvm_ref parcel_array_at(struct dvm *vm, dvm_ref a, uint32_t i)
{
  return ((const uint32_t *)dvm_array_data(vm, a))[i];
}
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
    if (parcel_take_i32(p) && !parcel_create_from(vm, self, ARG(0).l, &e)) {
      dvm_unpin(vm, arr.l);
      return false;
    }
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
  if (!list) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList");
    return false;
  }
  dvm_pin(vm, list);
  for (int32_t i = 0; i < n; ++i) {
    dvm_ref e = 0;
    union dvm_value add = { 0 }, ignored = { 0 };
    if (parcel_take_i32(p) && !parcel_create_from(vm, self, ARG(0).l, &e)) {
      dvm_unpin(vm, list);
      return false;
    }
    add.l = e;
    if (!list_add(vm, list, &add, 1, &ignored)) {
      dvm_unpin(vm, list);
      return false;
    }
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
  if ((uint32_t)n > (p->size - (p->pos > p->size ? p->size : p->pos)) / 4) {
    p->failed=true;
    p->pos = p->size;
    RETL(0);
  }
  dvm_ref a = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
  if (!a) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "String[]");
    return false;
  }
  dvm_pin(vm, a);
  for (int32_t i = 0; i < n; ++i) {
    dvm_ref s;
    if (!parcel_take_string(vm, p, &s) && dvm_exception(vm)) {
      dvm_unpin(vm, a);
      return false;
    }
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
  if (!list) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList");
    return false;
  }
  dvm_pin(vm, list);
  for (int32_t i = 0; i < n; ++i) {
    dvm_ref s;
    union dvm_value add = { 0 }, ignored = { 0 };
    if (!parcel_take_string(vm, p, &s) && dvm_exception(vm)) {
      dvm_unpin(vm, list);
      return false;
    }
    add.l = s;
    if (!list_add(vm, list, &add, 1, &ignored)) {
      dvm_unpin(vm, list);
      return false;
    }
  }
  dvm_unpin(vm, list);
  RETL(list);
}

/* ---- generic values, Map, List, Bundle ---------------------------------- */

static bool parcel_write_value_inner(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref v);
static bool parcel_write_value(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref v)
{
  if(p->ipc && (p->failed || p->depth>=64)) {
    p->failed=true;
    dvm__throw(vm,"android/os/BadParcelableException","IPC Parcel nesting or bounds");
    return false;
  }
  ++p->depth;
  bool ok=parcel_write_value_inner(vm,self,p,v);
  --p->depth;
  return ok;
}

static bool parcel_read_value_inner(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out);
static bool parcel_read_value(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out)
{
  if(p->ipc && (p->failed || p->depth>=64)) {
    p->failed=true;
    dvm__throw(vm,"android/os/BadParcelableException","IPC Parcel nesting or bounds");
    return false;
  }
  ++p->depth;
  bool ok=parcel_read_value_inner(vm,self,p,out);
  --p->depth;
  return ok;
}

static bool parcel_value_is(struct dvm *vm, dvm_ref v, const char *desc)
{
  struct dvm_class *c = dvm_object_class(vm, v), *t = dvm__class_by_desc(vm, desc);
  return c && t && dvm__class_assignable(vm, c, t);
}

static bool parcel_write_map_body(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref map)
{
  /* A Bundle and a ContentValues are keyed by String and are not java.util.Maps. */
  const bool bundle = parcel_value_is(vm, map, "Landroid/os/Bundle;") ||
                      parcel_value_is(vm, map, "Landroid/content/ContentValues;");
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
  if(p->ipc && (n<0 || p->pos>p->size || (uint32_t)n>(p->size-p->pos)/8u)) {
    p->failed=true;
    return false;
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

/* A bundle's bytes read into a new instance of `desc`: Bundle and ContentValues
 * share one wire form. */
static bool parcel_read_bundle_as(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out, const char *desc)
{
  *out = 0;
  int32_t length = parcel_take_i32(p);
  if (length < 0) {
    if(p->ipc && length != -1)p->failed=true;
    return true;
  }
  dvm_ref b = parcel_new_instance(vm, desc);
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
  if(p->ipc && ((uint32_t)length>p->size-start || length<4)) {
    p->failed=true;
    return false;
  }
  int32_t n = parcel_take_i32(p);
  dvm_pin(vm, b);
  bool ok = parcel_read_map_into(vm, self, p, b, n);
  dvm_unpin(vm, b);
  if(p->ipc && p->pos!=start+(uint32_t)length) {
    p->failed=true;
    return false;
  }
  p->pos = start + (uint32_t)length > p->size ? p->size : start + (uint32_t)length;
  return ok;
}

static bool parcel_read_bundle(struct dvm *vm, dvm_ref self, struct rt_parcel *p, dvm_ref *out)
{
  return parcel_read_bundle_as(vm, self, p, out, "Landroid/os/Bundle;");
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
    case 'B':
      return parcel_put_i32(vm, p, PARCEL_VAL_BYTEARRAY) && parcel_write_prim_array(vm, p, v, 'B', 0, 0, true);
    case 'I':
      return parcel_put_i32(vm, p, PARCEL_VAL_INTARRAY) && parcel_write_prim_array(vm, p, v, 'I', 0, 0, true);
    case 'J':
      return parcel_put_i32(vm, p, PARCEL_VAL_LONGARRAY) && parcel_write_prim_array(vm, p, v, 'J', 0, 0, true);
    default:
      break;
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
  case PARCEL_VAL_NULL:
    return true;
  case PARCEL_VAL_STRING:
    return parcel_take_string(vm, p, out) || !dvm_exception(vm);
  case PARCEL_VAL_INTEGER:
    v.i = parcel_take_i32(p);
    *out = box_make(vm, "Ljava/lang/Integer;", v);
    return true;
  case PARCEL_VAL_SHORT:
    v.i = (int16_t)parcel_take_i32(p);
    *out = box_make(vm, "Ljava/lang/Short;", v);
    return true;
  case PARCEL_VAL_BYTE:
    v.i = (int8_t)parcel_take_i32(p);
    *out = box_make(vm, "Ljava/lang/Byte;", v);
    return true;
  case PARCEL_VAL_BOOLEAN:
    v.i = parcel_take_i32(p) != 0;
    *out = box_make(vm, "Ljava/lang/Boolean;", v);
    return true;
  case PARCEL_VAL_LONG:
    v.j = parcel_take_i64(p);
    *out = box_make(vm, "Ljava/lang/Long;", v);
    return true;
  case PARCEL_VAL_FLOAT:
    parcel_take(p, &v.f, 4);
    *out = box_make(vm, "Ljava/lang/Float;", v);
    return true;
  case PARCEL_VAL_DOUBLE:
    parcel_take(p, &v.d, 8);
    *out = box_make(vm, "Ljava/lang/Double;", v);
    return true;
  case PARCEL_VAL_CHARSEQUENCE:
    parcel_take_i32(p);
    return parcel_take_string(vm, p, out) || !dvm_exception(vm);
  case PARCEL_VAL_BUNDLE:
    return parcel_read_bundle(vm, self, p, out);
  case PARCEL_VAL_BYTEARRAY:
    *out = parcel_read_prim_array(vm, p, 'B', 0, &exc);
    return !exc;
  case PARCEL_VAL_INTARRAY:
    *out = parcel_read_prim_array(vm, p, 'I', 0, &exc);
    return !exc;
  case PARCEL_VAL_LONGARRAY:
    *out = parcel_read_prim_array(vm, p, 'J', 0, &exc);
    return !exc;
  case PARCEL_VAL_STRINGARRAY: {
    union dvm_value r = { 0 };
    if (!parcel_createStringArray(vm, self, NULL, 0, &r)) return false;
    *out = r.l;
    return true;
  }
  case PARCEL_VAL_MAP: {
    int32_t n = parcel_take_i32(p);
    dvm_ref m = coll_new_map(vm);
    if (!m) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "HashMap");
      return false;
    }
    dvm_pin(vm, m);
    bool ok = parcel_read_map_into(vm, self, p, m, n);
    dvm_unpin(vm, m);
    *out = m;
    return ok;
  }
  case PARCEL_VAL_LIST: {
    int32_t n = parcel_take_i32(p);
    if(p->ipc && (n<0 || p->pos>p->size || (uint32_t)n>(p->size-p->pos)/4u)) {
      p->failed=true;
      return false;
    }
    dvm_ref l = coll_new_list(vm, "Ljava/util/ArrayList;");
    if (!l) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList");
      return false;
    }
    dvm_pin(vm, l);
    for (int32_t i = 0; i < n; ++i) {
      dvm_ref e;
      union dvm_value add = { 0 }, ignored = { 0 };
      if (!parcel_read_value(vm, self, p, &e)) {
        dvm_unpin(vm, l);
        return false;
      }
      add.l = e;
      if (!list_add(vm, l, &add, 1, &ignored)) {
        dvm_unpin(vm, l);
        return false;
      }
    }
    dvm_unpin(vm, l);
    *out = l;
    return true;
  }
  case PARCEL_VAL_PARCELABLE: {
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
{
  PARCEL_SELF() (void)out;
  return parcel_write_value(vm, self, p, ARG(0).l);
}
static bool parcel_readValue(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF()
  dvm_ref r;
  if (!parcel_read_value(vm, self, p, &r)) return false;
  RETL(r);
}
static bool parcel_writeBundle(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  return parcel_write_bundle(vm, self, p, ARG(0).l);
}
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
  if (!l) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "ArrayList");
    return false;
  }
  dvm_pin(vm, l);
  for (int32_t i = 0; i < n; ++i) {
    dvm_ref e;
    union dvm_value add = { 0 }, ignored = { 0 };
    if (!parcel_read_value(vm, self, p, &e)) {
      dvm_unpin(vm, l);
      return false;
    }
    add.l = e;
    if (!list_add(vm, l, &add, 1, &ignored)) {
      dvm_unpin(vm, l);
      return false;
    }
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
  if (!m) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "HashMap");
    return false;
  }
  dvm_pin(vm, m);
  bool ok = parcel_read_map_into(vm, self, p, m, n);
  dvm_unpin(vm, m);
  if (!ok) return false;
  RETL(m);
}

/* ======================================================================== *
 * Cross-process Binder
 *
 * Two component processes talk over the process channel.  Every message is
 *     magic[4] | sender host pid u32 | seq u32 | ...
 * and is one of
 *     LBB1  bind a Service of the receiving package:  intent parcel
 *     LBT1  transact:  handle i32 | code u32 | flags u32 | data length u32 | data
 *     LBR1  reply:     status i32 | handle i32 | payload length u32 | payload
 * A strong binder inside a data parcel is (tag, handle, owner pid); a handle
 * is meaningful only to its owner, so the other side holds a BinderProxy that
 * names it.  Transactions flagged FLAG_ONEWAY return at once -- the whole of
 * the Play asset-module protocol is one-way both ways -- and a synchronous one
 * waits for its reply, with the interpreter lock released, for a bounded time.
 * Synchronous calls are made only from this process towards its peer's
 * handles; a reply to one is matched by sequence number.
 * ======================================================================== */
#define BINDER_FLAG_ONEWAY 1
#define BINDER_WAIT_MS 15000

static int (*g_binder_send)(void *user, const char *peer, const void *bytes, size_t size);
static void *g_binder_send_user;
static int (*g_binder_fd_send)(void *,const char *,const void *,size_t,const int *,size_t);
static void *g_binder_fd_user;
void dvm_binder_set_fd_transport(int (*send)(void *,const char *,const void *,size_t,const int *,size_t),void *user)
{
  g_binder_fd_send=send;
  g_binder_fd_user=user;
}
static int binder_send_message(const char *peer,const void *bytes,size_t size,const struct parcel_fds *fds)
{
  if(fds && fds->count)return g_binder_fd_send ? g_binder_fd_send(g_binder_fd_user,peer,bytes,size,fds->fds,fds->count) : -ENOSYS;
  return g_binder_send ? g_binder_send(g_binder_send_user,peer,bytes,size) : -ENOSYS;
}
void dvm_binder_set_transport(int (*send)(void *, const char *, const void *, size_t), void *user)
{
  g_binder_send = send;
  g_binder_send_user = user;
}

static uint32_t binder_self(void)
{
  static uint32_t pid;
  if (!pid) pid = (uint32_t)getpid();
  return pid;
}

/* Who is calling, for the duration of a transaction this thread is serving:
 * Binder.getCallingUid()/getCallingPid() answer these, and are the process's own
 * uid and pid outside one.  Zero means "not inside a transaction". */
static __thread uint32_t g_binder_caller_uid, g_binder_caller_pid;

/* Where to send to a peer process: the parent addresses a child by package
 * name; a child's only peer is its parent, addressed by NULL. */
struct binder_link {
  uint32_t peer;
  char *name;
  struct binder_link *next;
};
static struct binder_link *g_binder_links;

static void binder_link_learn(uint32_t peer, const char *name)
{
  for (struct binder_link *l = g_binder_links; l; l = l->next)
    if (l->peer == peer) return;
  struct binder_link *l = calloc(1, sizeof *l);
  if (!l) return;
  l->peer = peer;
  if (name) l->name = strdup(name);
  l->next = g_binder_links;
  g_binder_links = l;
}

static int binder_send_peer_fds(uint32_t peer,const void *bytes,size_t size,const struct parcel_fds *fds)
{
  if (!g_binder_send && !g_binder_fd_send) return -ENOSYS;
  for (struct binder_link *l = g_binder_links; l; l = l->next)
    if (l->peer == peer) {
      /* A package can have both a foreground process and component hosts. */
      uint8_t *direct = malloc(size + 8);
      if (!direct) return -ENOMEM;
      memcpy(direct, "LBD1", 4);
      for (unsigned i = 0; i < 4; ++i) direct[4+i] = (uint8_t)(peer >> (8*i));
      memcpy(direct + 8, bytes, size);
      int rc = binder_send_message(l->name,direct,size+8,fds);
      free(direct);
      return rc;
    }
  return -ENOENT;
}
static int binder_send_peer(uint32_t peer,const void *bytes,size_t size)
{
  return binder_send_peer_fds(peer,bytes,size,NULL);
}

static void bput(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static uint32_t bget(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Every message names the sender: process, uid, package, and where the
 * package's installed APK is.  The receiver needs them to answer "who is
 * calling": the Play Store checks that a package belongs to the caller's uid
 * and reads its signing certificate from its APK. */
#define BINDER_PACKAGE_MAX 200
#define BINDER_PATH_MAX 1024
static uint8_t *binder_message(const char *magic, uint32_t seq, size_t body, uint8_t **at, size_t *total)
{
  const char *package = getenv("ANDROID_PACKAGE_NAME"), *apk = getenv("ANDROID_APK_FILE");
  const size_t pn = package ? strnlen(package, BINDER_PACKAGE_MAX) : 0;
  const size_t an = apk ? strnlen(apk, BINDER_PATH_MAX) : 0;
  const size_t header = 24 + pn + an;
  uint8_t *m = malloc(header + body);
  if (!m) return NULL;
  memcpy(m, magic, 4);
  bput(m + 4, binder_self());
  bput(m + 8, seq);
  bput(m + 12, (uint32_t)arm_exec_guest_uid());
  bput(m + 16, (uint32_t)pn);
  if (pn) memcpy(m + 20, package, pn);
  bput(m + 20 + pn, (uint32_t)an);
  if (an) memcpy(m + 24 + pn, apk, an);
  *at = m + header;
  *total = header + body;
  return m;
}

/* Calls that wait for an answer, and binds waiting to deliver a connection. */
struct binder_pending {
  uint32_t seq;
  bool done;
  int32_t status, handle;
  uint8_t *payload;
  size_t payload_size;
  struct parcel_fds descriptors;
  dvm_ref connection;                     /* a bind: the ServiceConnection */
  char *package;                          /* a bind: the package asked */
  uint32_t peer;
  struct binder_pending *next;
};
static pthread_mutex_t g_binder_lock = PTHREAD_MUTEX_INITIALIZER;
static struct binder_pending *g_binder_pending;
static uint32_t g_binder_seq;

static struct binder_pending *binder_pending_new(void)
{
  struct binder_pending *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  pthread_mutex_lock(&g_binder_lock);
  e->seq = ++g_binder_seq;
  e->next = g_binder_pending;
  g_binder_pending = e;
  pthread_mutex_unlock(&g_binder_lock);
  return e;
}

static void binder_pending_drop(struct binder_pending *e)
{
  pthread_mutex_lock(&g_binder_lock);
  for (struct binder_pending **pp = &g_binder_pending; *pp; pp = &(*pp)->next)
    if (*pp == e) {
      *pp = e->next;
      break;
    }
  pthread_mutex_unlock(&g_binder_lock);
  parcel_fds_clear(&e->descriptors);
  free(e->payload);
  free(e->package);
  free(e);
}

/* ---- BinderProxy ---- */
struct binder_proxy_rec {
  uint32_t peer;
  int32_t handle;
  dvm_ref proxy;
};
#define BINDER_PROXY_MAX 1024
static struct binder_proxy_rec g_binder_proxies[BINDER_PROXY_MAX];
static int g_nbinder_proxies;

static bool binder_proxy_identity(struct dvm *vm, dvm_ref obj, uint32_t *peer, int32_t *handle)
{
  if (!obj) return false;
  struct dvm_class *c = dvm_object_class(vm, obj);
  if (!c || !c->name || strcmp(c->name, "android/os/BinderProxy")) return false;
  union dvm_value p = {0}, h = {0};
  (void)dvm_get_field(vm, obj, "peer", "I", &p);
  (void)dvm_get_field(vm, obj, "handle", "I", &h);
  *peer = (uint32_t)p.i;
  *handle = h.i;
  return true;
}

static dvm_ref binder_proxy_get(struct dvm *vm, uint32_t peer, int32_t handle)
{
  for (int i = 0; i < g_nbinder_proxies; ++i)
    if (g_binder_proxies[i].peer == peer && g_binder_proxies[i].handle == handle)
      return g_binder_proxies[i].proxy;
  if (g_nbinder_proxies >= BINDER_PROXY_MAX) return 0;
  struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/os/BinderProxy;");
  dvm_ref proxy = c ? dvm_new_object(vm, c) : 0;
  if (!proxy) return 0;
  dvm_pin(vm, proxy);
  (void)dvm_set_field(vm, proxy, "peer", "I", (union dvm_value) {
    .i = (int32_t)peer
  });
  (void)dvm_set_field(vm, proxy, "handle", "I", (union dvm_value) {
    .i = handle
  });
  g_binder_proxies[g_nbinder_proxies++] = (struct binder_proxy_rec) {
    peer, handle, proxy
  };
  return proxy;
}

/* Parcel <-> bytes. */
static dvm_ref binder_parcel_from(struct dvm *vm, const uint8_t *bytes, size_t size)
{
  union dvm_value parcel = {0};
  if (!parcel_obtain(vm, 0, NULL, 0, &parcel) || !parcel.l) return 0;
  struct rt_parcel *p = parcel_of(vm, parcel.l);
  if (!p) return 0;
  if (size) {
    p->buf = malloc(size);
    if (!p->buf) return 0;
    memcpy(p->buf, bytes, size);
  }
  p->size = p->cap = (uint32_t)size;
  p->pos = 0;
  if(parcel_fds_copy(&p->descriptors,binder_incoming_fds.fds,binder_incoming_fds.count)<0) {
    dvm__throw(vm,"java/io/IOException","Unable to duplicate incoming Parcel descriptors");
    return 0;
  }
  return parcel.l;
}

/* What a thread that drives the process's own message pump does while it waits
 * for an answer: the answer is delivered by that pump, so waiting without
 * running it would never end.  Installed by the launcher. */
static void (*g_binder_pump)(void);
void dvm_binder_set_pump(void (*pump)(void)) {
  g_binder_pump = pump;
}

/* Any thread that waits for an answer drives the pump, one at a time: the main
 * thread may itself be parked on a Future that depends on the very answer.  The
 * lock is recursive so a handler the pump runs can wait on a nested request. */
#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP /* glibc spelling; winpthreads only has the C11 one. */
static pthread_mutex_t g_binder_pump_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t g_binder_pump_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#endif
static void binder_pump_once(void)
{
  if (!g_binder_pump || pthread_mutex_trylock(&g_binder_pump_lock)) return;
  g_binder_pump();
  pthread_mutex_unlock(&g_binder_pump_lock);
}

/* The transaction half of a BinderProxy: build LBT1 and, unless one-way, wait. */
bool dvm_binder_proxy_transact(struct dvm *vm, dvm_ref proxy, int32_t code, dvm_ref data,
                               dvm_ref reply, int32_t flags, bool *result)
{
  uint32_t peer = 0;
  int32_t handle = 0;
  *result = false;
  if (!binder_proxy_identity(vm, proxy, &peer, &handle)) return true;
  struct rt_parcel *d = data ? parcel_of(vm, data) : NULL;
  const uint32_t dsize = d ? d->size : 0;
  const bool oneway = (flags & BINDER_FLAG_ONEWAY) != 0;
  struct binder_pending *wait = oneway ? NULL : binder_pending_new();
  if (!oneway && !wait) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Binder call");
    return false;
  }
  uint8_t *at = NULL;
  size_t total = 0;
  uint8_t *m = binder_message("LBT1", wait ? wait->seq : 0, 16 + dsize, &at, &total);
  if (!m) {
    if (wait) binder_pending_drop(wait);
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Binder call");
    return false;
  }
  bput(at, (uint32_t)handle);
  bput(at + 4, (uint32_t)code);
  bput(at + 8, (uint32_t)flags);
  bput(at + 12, dsize);
  if (dsize) memcpy(at + 16, d->buf, dsize);
  int rc = binder_send_peer_fds(peer,m,total,d ? &d->descriptors : NULL);
  free(m);
  if (rc < 0) {
    if (wait) binder_pending_drop(wait);
    dvm__throw(vm, "android/os/DeadObjectException", "Binder peer %u: %s", peer, strerror(-rc));
    return false;
  }
  if (oneway) {
    *result = true;
    return true;
  }
  const unsigned depth = dvm_gil_unlock_all(vm);
  bool done = false;
  for (int waited = 0; waited < BINDER_WAIT_MS && !done; ++waited) {
    binder_pump_once();
    pthread_mutex_lock(&g_binder_lock);
    done = wait->done;
    pthread_mutex_unlock(&g_binder_lock);
    if (!done) usleep(1000);
  }
  dvm_gil_relock(vm, depth);
  if (!done) {
    binder_pending_drop(wait);
    dvm__throw(vm, "android/os/DeadObjectException", "Binder transaction timed out");
    return false;
  }
  if (wait->status >= 0 && reply && wait->payload_size) {
    struct rt_parcel *r = parcel_of(vm, reply);
    if (r) {
      free(r->buf);
      parcel_fds_clear(&r->descriptors);
      r->buf = malloc(wait->payload_size);
      if (r->buf) {
        memcpy(r->buf, wait->payload, wait->payload_size);
        r->size = r->cap = (uint32_t)wait->payload_size;
        r->pos = 0;
      }
      r->descriptors=wait->descriptors;
      wait->descriptors=(struct parcel_fds) {
        0
      };
    }
  }
  *result = wait->status > 0;
  const bool failed = wait->status < 0;
  binder_pending_drop(wait);
  if (failed) {
    dvm__throw(vm, "android/os/RemoteException", "Binder transaction failed");
    return false;
  }
  return true;
}

static void binder_reply_fds(uint32_t peer,uint32_t seq,int32_t status,int32_t handle,
                             const uint8_t *payload,size_t size,const struct parcel_fds *fds)
{
  uint8_t *at = NULL;
  size_t total = 0;
  uint8_t *m = binder_message("LBR1", seq, 12 + size, &at, &total);
  if (!m) return;
  bput(at, (uint32_t)status);
  bput(at + 4, (uint32_t)handle);
  bput(at + 8, (uint32_t)size);
  if (size) memcpy(at + 12, payload, size);
  (void)binder_send_peer_fds(peer,m,total,fds);
  free(m);
}
static void binder_reply(uint32_t peer,uint32_t seq,int32_t status,int32_t handle,const uint8_t *payload,size_t size)
{
  binder_reply_fds(peer,seq,status,handle,payload,size,NULL);
}


/* Provider operations are served by a handler the Android layer installs
 * (dvm_extras_android.h); a message nobody can serve is answered "unsupported". */
static int (*g_binder_provider_serve)(struct dvm *vm, uint32_t sender, uint32_t seq,
                                      const uint8_t *body, size_t size);
void dvm_binder_set_provider_server(int (*serve)(struct dvm *, uint32_t, uint32_t, const uint8_t *, size_t))
{
  g_binder_provider_serve = serve;
}

/* A synchronous request to `package`'s process: sends `magic` with `body` and
 * waits (interpreter lock released) for its reply.  0 on a reply, with the peer's
 * status and a malloc'd payload; negative errno when the peer cannot be reached
 * or does not answer in time. */
int dvm_binder_request(struct dvm *vm, const char *package, const char *magic,
                       const void *body, size_t body_size,
                       int32_t *status, uint8_t **payload, size_t *payload_size)
{
  *status = 0;
  *payload = NULL;
  *payload_size = 0;
  struct binder_pending *wait = binder_pending_new();
  if (!wait) return -ENOMEM;
  uint8_t *at = NULL;
  size_t total = 0;
  uint8_t *m = binder_message(magic, wait->seq, body_size, &at, &total);
  if (!m) {
    binder_pending_drop(wait);
    return -ENOMEM;
  }
  if (body_size) memcpy(at, body, body_size);
  int rc = g_binder_send ? g_binder_send(g_binder_send_user, package, m, total) : -ENOSYS;
  free(m);
  if (rc < 0) {
    binder_pending_drop(wait);
    return rc;
  }
  const unsigned depth = dvm_gil_unlock_all(vm);
  bool done = false;
  for (int waited = 0; waited < BINDER_WAIT_MS && !done; ++waited) {
    binder_pump_once();
    pthread_mutex_lock(&g_binder_lock);
    done = wait->done;
    pthread_mutex_unlock(&g_binder_lock);
    if (!done) usleep(1000);
  }
  dvm_gil_relock(vm, depth);
  if (!done) {
    binder_pending_drop(wait);
    return -ETIMEDOUT;
  }
  if(wait->descriptors.count) {
    binder_pending_drop(wait);
    return -ENOTSUP;
  }
  *status = wait->status;
  *payload = wait->payload;
  *payload_size = wait->payload_size;
  wait->payload = NULL;
  binder_pending_drop(wait);
  return 0;
}

/* Binder objects in a Parcel.
 *
 * A strong binder is written as a flat object -- a tag and a handle -- and read
 * back as the object that handle names.  Handle 0 is null.  Within a process the
 * handle table maps straight back to the very Binder that was written, which is
 * what Parcel.readStrongBinder returns for a local binder on Android (the
 * original object, not a proxy).  The table pins what it holds, so a binder
 * handed to another component stays alive for as long as that component may use
 * it; a binder is entered once, however many parcels carry it. */
#define PARCEL_BINDER_TAG 0x85422a73u
#define BINDER_HANDLE_MAX 4096
static dvm_ref g_binder_handles[BINDER_HANDLE_MAX];   /* handle n lives at [n-1] */
static int g_nbinder_handles;

static int32_t binder_handle_for(struct dvm *vm, dvm_ref binder)
{
  if (!binder) return 0;
  for (int i = 0; i < g_nbinder_handles; ++i)
    if (g_binder_handles[i] == binder) return i + 1;
  if (g_nbinder_handles >= BINDER_HANDLE_MAX) return -1;
  dvm_pin(vm, binder);
  g_binder_handles[g_nbinder_handles] = binder;
  return ++g_nbinder_handles;
}

static dvm_ref binder_from_handle(int32_t handle)
{
  return handle > 0 && handle <= g_nbinder_handles ? g_binder_handles[handle - 1] : 0;
}

/* A flat binder object is tag, handle and the process that owns the handle.  A
 * local binder is owned by this process; a BinderProxy is written back as the
 * remote handle it stands for, so the owner can recognise its own object. */
static bool parcel_writeStrongBinder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  dvm_ref binder = nargs > 0 ? ARG(0).l : 0;
  uint32_t owner = binder_self();
  int32_t handle = 0;
  if (!binder_proxy_identity(vm, binder, &owner, &handle)) handle = binder_handle_for(vm, binder);
  if (handle < 0) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Binder handle table is full");
    return false;
  }
  return parcel_put_i32(vm, p, (int32_t)PARCEL_BINDER_TAG) && parcel_put_i32(vm, p, handle) &&
         parcel_put_i32(vm, p, (int32_t)owner);
}
static bool parcel_readStrongBinder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF()
  if ((uint32_t)parcel_take_i32(p) != PARCEL_BINDER_TAG) {
    p->failed = true;
    RETL(0);
  }
  const int32_t handle = parcel_take_i32(p);
  const uint32_t owner = (uint32_t)parcel_take_i32(p);
  if (handle <= 0) RETL(0);
  if (owner == binder_self()) RETL(binder_from_handle(handle));
  RETL(binder_proxy_get(vm, owner, handle));
}
/* Bundle.CREATOR, the Parcelable.Creator every reader of a typed Bundle in a
 * Parcel reaches for (Parcel.createTypedArrayList(Bundle.CREATOR), the AIDL
 * stubs of the Play asset-module service). */
static bool bundle_creator_create(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  struct rt_parcel *p = ARG(0).l ? parcel_of(vm, ARG(0).l) : NULL;
  if (!p) {
    dvm__throw(vm, "java/lang/NullPointerException", "Parcel");
    return false;
  }
  dvm_ref bundle = 0;
  if (!parcel_read_bundle(vm, ARG(0).l, p, &bundle)) return false;
  RETL(bundle);
}
static bool bundle_creator_new_array(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if (ARG(0).i < 0) {
    dvm__throw(vm, "java/lang/NegativeArraySizeException", "%d", (int)ARG(0).i);
    return false;
  }
  RETL(dvm_new_array(vm, 'L', "Landroid/os/Bundle;", (uint32_t)ARG(0).i));
}
static const struct rt_method rt_bundle_creator[] = {
  M("createFromParcel", "(Landroid/os/Parcel;)Ljava/lang/Object;", bundle_creator_create),
  M("newArray", "(I)[Ljava/lang/Object;", bundle_creator_new_array),
  M_END,
};
static const char *const rt_iface_bundle_creator[] = { "Landroid/os/Parcelable$Creator;", NULL };
static const struct rt_field rt_bundle_sfields[] = {
  { "CREATOR", "Landroid/os/Parcelable$Creator;" }, { "EMPTY", "Landroid/os/Bundle;" }, F_END
};
static const struct rt_field rt_persistable_bundle_sfields[] = { { "EMPTY", "Landroid/os/PersistableBundle;" }, F_END };

static bool parcel_writeInterfaceToken(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  PARCEL_SELF() (void)out;
  return parcel_put_i32(vm, p, 0) && parcel_put_string(vm, p, ARG(0).l);
}
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
{
  PARCEL_SELF() (void)out;
  return parcel_put_i32(vm, p, 0);
}
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
  if (!p) {
    dvm__throw(vm, "java/lang/NullPointerException", "parcel");
    return false;
  }
  (void)out;
  return parcel_write_bundle(vm, ARG(0).l, p, self);
}

/* Bundle.readFromParcel(): replace this bundle's contents with the parcel's. */
static bool bundle_readFromParcel(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  struct rt_parcel *p = parcel_of(vm, ARG(0).l);
  if (!p) {
    dvm__throw(vm, "java/lang/NullPointerException", "parcel");
    return false;
  }
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
/* An Intent as Parcelable bytes: its fields, then its extras Bundle. */
static bool intent_write_body(struct dvm *vm, dvm_ref parcel, struct rt_parcel *p, dvm_ref intent)
{
  union dvm_value value= {0};
  bool ok=true;
  for(const struct rt_field *f=rt_intent_fields; ok && f->name; ++f) {
    value.l=0;
    (void)dvm_get_field(vm,intent,f->name,f->type,&value);
    if(!strcmp(f->name,"flags"))ok=parcel_put_i32(vm,p,value.i);
    else if(!strcmp(f->name,"data")) {
      union dvm_value text= {0};
      ok=!value.l || uri_toString(vm,value.l,NULL,0,&text);
      if(ok)ok=parcel_put_string(vm,p,text.l);
    } else if(!strcmp(f->name,"component")) {
      ok=parcel_put_i32(vm,p,value.l!=0);
      const char *fields[]= {"pkg","cls"};
      for(unsigned i=0; ok && value.l && i<2; ++i) {
        union dvm_value text= {0};
        (void)dvm_get_field(vm,value.l,fields[i],"Ljava/lang/String;",&text);
        ok=parcel_put_string(vm,p,text.l);
      }
    } else if(!strcmp(f->name,"categories")) {
      union dvm_value array= {0};
      ok=!value.l || am_vcall(vm,value.l,"toArray","()[Ljava/lang/Object;",NULL,0,&array);
      uint32_t n=array.l ? dvm_array_length(vm,array.l) : 0;
      if(ok)ok=parcel_put_i32(vm,p,value.l ? (int32_t)n : -1);
      for(uint32_t i=0; ok && i<n; ++i)
        ok=parcel_put_string(vm,p,((dvm_ref *)dvm_array_data(vm,array.l))[i]);
    } else ok=parcel_put_string(vm,p,value.l);
  }
  if(ok) {
    ok=intent_getExtras(vm,intent,NULL,0,&value);
    if(ok) {
      if(value.l)dvm_pin(vm,value.l);
      ok=parcel_write_bundle(vm,parcel,p,value.l);
      if(value.l)dvm_unpin(vm,value.l);
    }
  }
  return ok;
}

int dvm_service_pack(struct dvm *vm,uint32_t intent,int operation,void **bytes,size_t *size)
{
  *bytes=NULL;
  *size=0;
  if(!intent || (operation!=1 && operation!=2 && operation!=3))return -EINVAL;
  union dvm_value parcel= {0},value= {0},ignored= {0};
  if(!parcel_obtain(vm,0,NULL,0,&parcel))return -ENOMEM;
  dvm_pin(vm,parcel.l);
  dvm_pin(vm,intent);
  struct rt_parcel *p=parcel_of(vm,parcel.l);
  p->ipc=true;
  bool ok=parcel_put_i32(vm,p,0x3156534c) && parcel_put_i32(vm,p,operation);
  if(ok)ok=intent_write_body(vm,parcel.l,p,intent);
  if(ok && !p->failed && !p->descriptors.count && !dvm_exception(vm)) {
    *bytes=p->buf;
    *size=p->size;
    p->buf=NULL;
    p->size=p->cap=p->pos=0;
  } else ok=false;
  (void)parcel_recycle(vm,parcel.l,NULL,0,&ignored);
  dvm_unpin(vm,intent);
  dvm_unpin(vm,parcel.l);
  return ok ? 0 : -EINVAL;
}

/* The reverse: the Intent a body written by intent_write_body holds. */
static bool intent_read_fields(struct dvm *vm,struct rt_parcel *p,dvm_ref result)
{
  union dvm_value ignored= {0};
  bool ok=true;
  for(const struct rt_field *f=rt_intent_fields; ok && f->name; ++f) {
    union dvm_value value= {0};
    if(!strcmp(f->name,"flags"))value.i=parcel_take_i32(p);
    else if(!strcmp(f->name,"data")) {
      union dvm_value text= {0};
      ok=parcel_take_string(vm,p,&text.l);
      if(ok && text.l)ok=uri_parse(vm,0,&text,1,&value);
    } else if(!strcmp(f->name,"component")) {
      int present=parcel_take_i32(p);
      if(present!=0 && present!=1)ok=false;
      if(present && ok) {
        value.l=dvm_new_object(vm,dvm_find_class(vm,"android/content/ComponentName"));
        if(!value.l) {
          ok=false;
          break;
        }
        dvm_pin(vm,value.l);
        const char *fields[]= {"pkg","cls"};
        for(unsigned i=0; ok && i<2; ++i) {
          union dvm_value text= {0};
          ok=parcel_take_string(vm,p,&text.l) && text.l;
          if(ok)ok=dvm_set_field(vm,value.l,fields[i],"Ljava/lang/String;",text);
        }
        dvm_unpin(vm,value.l);
      }
    } else if(!strcmp(f->name,"categories")) {
      int32_t n=parcel_take_i32(p);
      if(n < -1 || (n>=0 && (uint32_t)n>(p->size-p->pos)/4u)) {
        ok=false;
        break;
      }
      if(n>=0) {
        value.l=coll_new_list(vm,"Ljava/util/HashSet;");
        if(!value.l) {
          ok=false;
          break;
        }
        dvm_pin(vm,value.l);
        for(int32_t i=0; ok && i<n; ++i) {
          union dvm_value text= {0};
          ok=parcel_take_string(vm,p,&text.l) && text.l;
          if(ok)ok=list_add(vm,value.l,&text,1,&ignored);
        }
        dvm_unpin(vm,value.l);
      }
    } else ok=parcel_take_string(vm,p,&value.l);
    if(ok)ok=!p->failed && dvm_set_field(vm,result,f->name,f->type,value);
  }
  return ok;
}
static bool intent_read_extras(struct dvm *vm,dvm_ref parcel,struct rt_parcel *p,dvm_ref result)
{
  dvm_ref extras=0;
  bool ok=parcel_read_bundle(vm,parcel,p,&extras);
  if(ok && extras) {
    dvm_pin(vm,extras);
    union dvm_value source= {.l=extras},ignored= {0};
    ok=map_putAll(vm,result,&source,1,&ignored);
    dvm_unpin(vm,extras);
  }
  return ok;
}
static bool intent_read_body(struct dvm *vm,dvm_ref parcel,struct rt_parcel *p,dvm_ref result)
{
  return intent_read_fields(vm,p,result) && intent_read_extras(vm,parcel,p,result);
}

int dvm_service_unpack(struct dvm *vm,const void *bytes,size_t size,int *operation,uint32_t *intent)
{
  *intent=0;
  *operation=0;
  if(!bytes || size<8 || size>LUNA_PROCESS_MESSAGE_MAX)return -EINVAL;
  union dvm_value parcel= {0},ignored= {0};
  if(!parcel_obtain(vm,0,NULL,0,&parcel))return -ENOMEM;
  dvm_pin(vm,parcel.l);
  struct rt_parcel *p=parcel_of(vm,parcel.l);
  p->ipc=true;
  p->buf=malloc(size);
  bool ok=p->buf!=NULL;
  if(ok) {
    memcpy(p->buf,bytes,size);
    p->size=p->cap=(uint32_t)size;
  }
  int op=0;
  if(ok) {
    ok=parcel_take_i32(p)==0x3156534c;
    op=parcel_take_i32(p);
    ok=ok && (op==1 || op==2 || op==3);
  }
  dvm_ref result=ok ? dvm_new_object(vm,dvm_find_class(vm,"android/content/Intent")) : 0;
  if(result)dvm_pin(vm,result);
  else ok=false;
  if(ok)ok=map_init(vm,result,NULL,0,&ignored);
  if(ok)ok=intent_read_body(vm,parcel.l,p,result);
  ok=ok && !p->failed && p->pos==p->size && !dvm_exception(vm);
  if(ok) {
    *intent=result;
    *operation=op;
  }
  if(result)dvm_unpin(vm,result);
  (void)parcel_recycle(vm,parcel.l,NULL,0,&ignored);
  dvm_unpin(vm,parcel.l);
  return ok ? 0 : -EINVAL;
}

/* Intent as a Parcelable: the bytes of intent_write_body, and the Creator that
 * reads them back. */
static bool intent_writeToParcel(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)nargs;
  struct rt_parcel *p = args[0].l ? parcel_of(vm, args[0].l) : NULL;
  if (!p) {
    dvm__throw(vm, "java/lang/NullPointerException", "Parcel");
    return false;
  }
  if (!intent_write_body(vm, args[0].l, p, self)) {
    if (!dvm_exception(vm)) dvm__throw(vm, "java/lang/RuntimeException", "Parcel: unable to marshal an Intent");
    return false;
  }
  RETV();
}
static bool intent_describeContents(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);
}
static const struct rt_method rt_x_intent_parcel[] = {
  M("writeToParcel", "(Landroid/os/Parcel;I)V", intent_writeToParcel),
  M("describeContents", "()I", intent_describeContents),
  /* The loader that resolves the Parcelables of the extras; classes are found by name through the VM's own loader. */
  M("setExtrasClassLoader", "(Ljava/lang/ClassLoader;)V", nop_void),
  M_END,
};
static bool intent_creator_create(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  struct rt_parcel *p = args[0].l ? parcel_of(vm, args[0].l) : NULL;
  if (!p) {
    dvm__throw(vm, "java/lang/NullPointerException", "Parcel");
    return false;
  }
  dvm_ref intent = dvm_new_object(vm, dvm_find_class(vm, "android/content/Intent"));
  union dvm_value ignored = {0};
  if (!intent) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "Intent");
    return false;
  }
  dvm_pin(vm, intent);
  bool ok = map_init(vm, intent, NULL, 0, &ignored) && intent_read_body(vm, args[0].l, p, intent) && !p->failed;
  dvm_unpin(vm, intent);
  if (!ok) {
    if (!dvm_exception(vm)) dvm__throw(vm, "android/os/BadParcelableException", "Parcelable Intent is malformed");
    return false;
  }
  RETL(intent);
}
static bool intent_creator_new_array(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                     int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if (args[0].i < 0) {
    dvm__throw(vm, "java/lang/NegativeArraySizeException", "%d", (int)args[0].i);
    return false;
  }
  RETL(dvm_new_array(vm, 'L', "Landroid/content/Intent;", (uint32_t)args[0].i));
}
static const struct rt_method rt_intent_creator[] = {
  M("createFromParcel", "(Landroid/os/Parcel;)Ljava/lang/Object;", intent_creator_create),
  M("newArray", "(I)[Ljava/lang/Object;", intent_creator_new_array),
  M_END,
};
static const struct rt_field rt_intent_sfields[] = { { "CREATOR", "Landroid/os/Parcelable$Creator;" }, F_END };

/* Called on the component process's main loop, outside the interpreter lock. */
int dvm_service_dispatch(struct dvm *vm,const void *bytes,size_t size)
{
  if(!vm)return -EINVAL;
  unsigned cookie=dvm_gil_enter_from_guest(vm);
  struct dvm_tstate saved,main_turn= {0};
  dvm__tstate_save(vm,&saved);
  main_turn.cur_thread=vm->main_thread;
  dvm__tstate_load(vm,&main_turn);
  uint32_t intent=0;
  int op=0;
  int status=dvm_service_unpack(vm,bytes,size,&op,&intent);
  if(!status) {
    dvm_pin(vm,intent);
    union dvm_value args[2]= {{.l=intent},{0}},resolved= {0},result= {0};
    /* Only enabled, declared components in this process may be dispatched. */
    bool found=pm_resolveService(vm,0,args,2,&resolved) && resolved.l &&
               svc_process_is_current(vm,resolved.l);
    if(found) {
      bool ok=op==1 ? ctx_startService(vm,0,args,1,&result) : ctx_stopService(vm,0,args,1,&result);
      status=ok && !dvm_exception(vm) ? (result.l ? 1 : 0) : -EIO;
    } else status=dvm_exception(vm) ? -EIO : -ENOENT;
    dvm_unpin(vm,intent);
  }
  if(dvm_exception(vm)) {
    fprintf(stderr,"[service] component command raised a Java exception\n");
    if(status>=0)status=-EIO;
  }
  dvm__tstate_load(vm,&saved);
  dvm_gil_leave_to_guest(vm,cookie);
  return status;
}

void dvm_service_set_submit(struct dvm *vm,int (*submit)(void *,const char *,const void *,size_t),void *user)
{
  if(!vm)return;
  unsigned cookie=dvm_gil_enter_from_guest(vm);
  vm->service_submit=submit;
  vm->service_submit_user=user;
  dvm_gil_leave_to_guest(vm,cookie);
}

/* Parent side of a bind that has been answered: connect the ServiceConnection. */
static void binder_bind_connected(struct dvm *vm, struct binder_pending *e, const uint8_t *payload, size_t size)
{
  char cls[256] = "", desc[256] = "";
  if (size >= 4) {
    uint32_t n = bget(payload);
    if (n < sizeof cls && 4 + n <= size) {
      memcpy(cls, payload + 4, n);
      cls[n] = 0;
      if (4 + n + 4 <= size) {
        uint32_t m = bget(payload + 4 + n);
        if (m < sizeof desc && 8 + n + m <= size) {
          memcpy(desc, payload + 8 + n, m);
          desc[m] = 0;
        }
      }
    }
  }
  struct dvm_class *cc = dvm__class_by_desc(vm, "Landroid/content/ComponentName;");
  dvm_ref name = cc ? dvm_new_object(vm, cc) : 0;
  if (!name || !e->connection) return;
  dvm_pin(vm, name);
  (void)dvm_set_field(vm, name, "pkg", "Ljava/lang/String;", (union dvm_value) {
    .l = dvm_new_string(vm, e->package ? e->package : "")
  });
  (void)dvm_set_field(vm, name, "cls", "Ljava/lang/String;", (union dvm_value) {
    .l = dvm_new_string(vm, cls)
  });
  dvm_ref proxy = e->peer && e->status >= 0 && e->handle > 0 ? binder_proxy_get(vm, e->peer, e->handle) : 0;
  if (proxy) {
    (void)dvm_set_field(vm, proxy, "descriptor", "Ljava/lang/String;", (union dvm_value) {
      .l = dvm_new_string(vm, desc)
    });
    union dvm_value a[2] = { { .l = name }, { .l = proxy } };
    dvm_ref conn = e->connection;
    struct dvm_class *c = dvm_object_class(vm, conn);
    struct dvm_method *m = c ? dvm_find_method(vm, c, "onServiceConnected",
                           "(Landroid/content/ComponentName;Landroid/os/IBinder;)V") : NULL;
    union dvm_value ignored = {0};
    if (m) (void)dvm_call(vm, m, conn, a, 2, &ignored);
    fprintf(stderr, "[binder] bound %s in process %u (handle %d)\n", cls, e->peer, e->handle);
  } else {
    union dvm_value a = { .l = name }, ignored = {0};
    struct dvm_class *c = dvm_object_class(vm, e->connection);
    struct dvm_method *m = c ? dvm_find_method(vm, c, "onNullBinding", "(Landroid/content/ComponentName;)V") : NULL;
    if (m) (void)dvm_call(vm, m, e->connection, &a, 1, &ignored);
    fprintf(stderr, "[binder] bind of %s returned no binder (status %d)\n", e->package ? e->package : "?", e->status);
  }
  dvm_unpin(vm, name);
}

/* One transaction: run the target Binder's transact() and, unless one-way,
 * answer.  Runs on the Binder thread (see binder_dispatch_transact). */
static bool binder_connection_run(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                  int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value seq= {0};
  dvm_get_field(vm,self,"seq","I",&seq);
  pthread_mutex_lock(&g_binder_lock);
  struct binder_pending *e=g_binder_pending;
  while(e && e->seq!=(uint32_t)seq.i)e=e->next;
  pthread_mutex_unlock(&g_binder_lock);
  if(e && e->connection && e->done) {
    binder_bind_connected(vm,e,e->payload,e->payload_size);
    dvm_unpin(vm,e->connection);
    binder_pending_drop(e);
  }
  RETV();
}
static const struct rt_method rt_binder_connection[]= {M("run","()V",binder_connection_run),M_END};
static const struct rt_field rt_binder_connection_fields[]= {{"seq","I"},F_END};

static void binder_serve_transact(struct dvm *vm, uint32_t sender, uint32_t seq, uint32_t caller_uid,
                                  const uint8_t *body, size_t blen)
{
  g_binder_caller_uid = caller_uid;
  g_binder_caller_pid = sender;
  int32_t status = 1;
  uint8_t *out = NULL;
  size_t osize = 0;
  if (blen < 16 || bget(body + 12) > blen - 16) status = -EINVAL;
  else {
    const int32_t handle = (int32_t)bget(body);
    const int32_t code = (int32_t)bget(body + 4), flags = (int32_t)bget(body + 8);
    const uint32_t dsize = bget(body + 12);
    dvm_ref binder = binder_from_handle(handle);
    if (getenv("LUNARIA_TRACE_BINDER"))
      fprintf(stderr, "[binder] transact code %d on handle %d -> %s\n", code, handle,
              binder ? dvm_object_class(vm, binder)->name : "(no such binder)");
    dvm_ref data = binder ? binder_parcel_from(vm, body + 16, dsize) : 0;
    union dvm_value rv = {0};
    dvm_ref rep = 0;
    if (data) {
      dvm_pin(vm, data);
      if (parcel_obtain(vm, 0, NULL, 0, &rv)) rep = rv.l;
    }
    if (rep) dvm_pin(vm, rep);
    if (!binder || !data || !rep) status = -ENOENT;
    else {
      union dvm_value call[4] = { { .i = code }, { .l = data }, { .l = rep }, { .i = flags } }, handled = {0};
      if (am_vcall(vm, binder, "transact", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z", call, 4, &handled) && !dvm_exception(vm)) {
        status = handled.i ? 1 : 0;
        struct rt_parcel *r = parcel_of(vm, rep);
        if (r && r->size && !(flags & BINDER_FLAG_ONEWAY)) {
          out = r->buf;
          osize = r->size;
        }
      } else status = -EIO;
      if (dvm_exception(vm)) {
        char what[256] = "";
        dvm_describe_exception(vm, dvm_exception(vm), what, sizeof what);
        fprintf(stderr, "[binder] transaction %d on handle %d raised %s\n", code, handle, what[0] ? what : "a Java exception");
        dvm_clear_exception(vm);
      }
    }
    if (!(flags & BINDER_FLAG_ONEWAY)) binder_reply_fds(sender,seq,status,0,out,osize,rep ? &parcel_of(vm,rep)->descriptors : NULL);
    union dvm_value ignored= {0};
    if (rep) {
      (void)parcel_recycle(vm,rep,NULL,0,&ignored);
      dvm_unpin(vm,rep);
    }
    if (data) {
      (void)parcel_recycle(vm,data,NULL,0,&ignored);
      dvm_unpin(vm,data);
    }
    status = 1;
  }
  if (status < 0) binder_reply(sender, seq, status, 0, NULL, 0);
  g_binder_caller_uid = g_binder_caller_pid = 0;
}

/* Transactions arrive on a Binder thread, never the application's main thread:
 * a handler may block (BlockingServiceConnection refuses the main thread), and
 * the main looper must stay free to deliver what it waits for.  One thread
 * keeps the order of one-way calls to a Binder. */
struct binder_job {
  uint32_t sender, seq, uid;
  int worker;
  bool provider;
  size_t size;
  struct parcel_fds descriptors;
  uint8_t body[];
};
static const struct rt_field rt_binder_job_fields[] = { { "job", "J" }, F_END };

/* A pool of Binder threads, as a process has (up to 15).  A synchronous
 * transaction may block on a reply that only another transaction delivers, so
 * one thread for all of them deadlocks; one-way calls keep to thread 0, which
 * is what keeps their order. */
#define BINDER_THREADS 8
static dvm_ref g_binder_workers[BINDER_THREADS];
static atomic_int g_binder_load[BINDER_THREADS];
static int binder_pick_worker(bool oneway)
{
  if (oneway) return 0;
  int best = 1;
  for (int i = 2; i < BINDER_THREADS; ++i)
    if (atomic_load(&g_binder_load[i]) < atomic_load(&g_binder_load[best])) best = i;
  return best;
}
static dvm_ref binder_worker(struct dvm *vm, int index)
{
  char name[16];
  snprintf(name, sizeof name, "Binder:%d", index + 1);
  return worker_handler(vm, name, &g_binder_workers[index]);
}

static void binder_serve_request(struct dvm *vm, uint32_t sender, uint32_t seq, uint32_t uid,
                                 const uint8_t *body, size_t size, bool provider)
{
  if (!provider) {
    binder_serve_transact(vm, sender, seq, uid, body, size);
    return;
  }
  uint32_t saved_uid = g_binder_caller_uid, saved_pid = g_binder_caller_pid;
  g_binder_caller_uid = uid;
  g_binder_caller_pid = sender;
  int rc = g_binder_provider_serve ? g_binder_provider_serve(vm, sender, seq, body, size) : -ENOSYS;
  if (rc < 0) binder_reply(sender, seq, rc, 0, NULL, 0);
  if (dvm_exception(vm)) dvm_clear_exception(vm);
  g_binder_caller_uid = saved_uid;
  g_binder_caller_pid = saved_pid;
}

static bool binder_job_run(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value ptr = { 0 };
  (void)dvm_get_field(vm, self, "job", "J", &ptr);
  struct binder_job *j = (struct binder_job *)(uintptr_t)ptr.j;
  if (j) {
    struct parcel_fds saved=binder_incoming_fds;
    binder_incoming_fds=j->descriptors;
    if (getenv("LUNARIA_TRACE_BINDER")) fprintf(stderr, "[binder] serving seq %u\n", j->seq);
    binder_serve_request(vm, j->sender, j->seq, j->uid, j->body, j->size, j->provider);
    binder_incoming_fds=saved;
    parcel_fds_clear(&j->descriptors);
    atomic_fetch_sub(&g_binder_load[j->worker], 1);
    free(j);
  }
  RETV();
}
static const struct rt_method rt_binder_job[] = { M("run", "()V", binder_job_run), M_END };

static void binder_dispatch_request(struct dvm *vm, uint32_t sender, uint32_t seq, uint32_t uid,
                                    const uint8_t *body, size_t blen, bool provider)
{
  if (getenv("LUNARIA_TRACE_BINDER")) fprintf(stderr, "[binder] transaction from %u seq %u (%zu bytes)\n", sender, seq, blen);
  const bool oneway = !provider && blen >= 16 && (bget(body + 8) & BINDER_FLAG_ONEWAY);
  const int worker = binder_pick_worker(oneway);
  dvm_ref handler = binder_worker(vm, worker);
  struct dvm_class *jc = dvm__class_by_desc(vm, "Llunaria/BinderJob;");
  dvm_ref job = handler && jc ? dvm_new_object(vm, jc) : 0;
  struct binder_job *j = job ? malloc(sizeof *j + blen) : NULL;
  if (!j) {
    if (getenv("LUNARIA_TRACE_BINDER")) fprintf(stderr, "[binder] no Binder thread (handler=%u job=%u): serving inline\n", (unsigned)handler, (unsigned)job);
    binder_serve_request(vm, sender, seq, uid, body, blen, provider);
    return;
  }
  j->sender = sender;
  j->seq = seq;
  j->uid = uid;
  j->size = blen;
  j->worker = worker;
  j->provider = provider;
  j->descriptors=(struct parcel_fds) {
    0
  };
  if(parcel_fds_copy(&j->descriptors,binder_incoming_fds.fds,binder_incoming_fds.count)<0) {
    free(j);
    binder_reply(sender,seq,-EMFILE,0,NULL,0);
    return;
  }
  memcpy(j->body, body, blen);
  atomic_fetch_add(&g_binder_load[worker], 1);
  union dvm_value ptr = { .j = (int64_t)(uintptr_t)j }, post = { .l = job }, posted = { 0 };
  (void)dvm_set_field(vm, job, "job", "J", ptr);
  if (!am_vcall(vm, handler, "post", "(Ljava/lang/Runnable;)Z", &post, 1, &posted) || !posted.i) {
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    (void)dvm_set_field(vm, job, "job", "J", (union dvm_value) {
      .j = 0
    });
    atomic_fetch_sub(&g_binder_load[worker], 1);
    parcel_fds_clear(&j->descriptors);
    free(j);
    binder_serve_request(vm, sender, seq, uid, body, blen, provider);
  }
}

static void binder_receive_broadcast(struct dvm *,uint32_t,const char *,const uint8_t *,size_t);
int dvm_binder_broadcast(struct dvm *vm,dvm_ref intent,dvm_ref permission)
{
  union dvm_value parcel= {0},ignored= {0};
  if(!parcel_obtain(vm,0,NULL,0,&parcel))return -ENOMEM;
  dvm_pin(vm,parcel.l);
  struct rt_parcel *p=parcel_of(vm,parcel.l);
  p->ipc=true;
  bool ok=parcel_put_string(vm,p,permission) && intent_write_body(vm,parcel.l,p,intent);
  if(getenv("LUNARIA_TRACE_BROADCAST"))
    fprintf(stderr,"[broadcast] pid=%ld encode ok=%d failed=%d bytes=%u exception=%u\n",
            (long)getpid(),ok,p->failed,p->size,dvm_exception(vm));
  int rc=-EINVAL;
  if(ok && !p->failed && !dvm_exception(vm)) {
    const char *self=getenv("ANDROID_PACKAGE_NAME");
    /* Even an in-process receiver sees the Binder snapshot, not an Intent
     * the sender can mutate again after sendBroadcast returns. */
    struct parcel_fds saved=binder_incoming_fds;
    binder_incoming_fds=p->descriptors;
    binder_receive_broadcast(vm,(uint32_t)arm_exec_guest_uid(),self,p->buf,p->size);
    binder_incoming_fds=saved;
    if(!g_binder_send && !g_binder_fd_send) {
      rc=0;
      goto done;
    }
    uint8_t *at=NULL;
    size_t size=0;
    uint8_t *message=binder_message("LBC1",0,p->size,&at,&size);
    if(message) {
      memcpy(at,p->buf,p->size);
      rc=binder_send_message(self ? self : "",message,size,&p->descriptors);
      free(message);
    } else rc=-ENOMEM;
  }
done:
  (void)parcel_recycle(vm,parcel.l,NULL,0,&ignored);
  dvm_unpin(vm,parcel.l);
  return rc;
}
static void binder_receive_broadcast(struct dvm *vm,uint32_t uid,const char *package,
                                     const uint8_t *body,size_t size)
{
  dvm_ref parcel=binder_parcel_from(vm,body,size);
  if(!parcel)return;
  dvm_pin(vm,parcel);
  struct rt_parcel *p=parcel_of(vm,parcel);
  p->ipc=true;
  dvm_ref permission=0,intent=0;
  bool ok=parcel_take_string(vm,p,&permission);
  if(permission)dvm_pin(vm,permission);
  if(ok)intent=dvm_new_object(vm,dvm_find_class(vm,"android/content/Intent"));
  if(intent) {
    dvm_pin(vm,intent);
    union dvm_value ignored= {0};
    ok=map_init(vm,intent,NULL,0,&ignored) && intent_read_fields(vm,p,intent);
    /* Resolve recipients using the plain Intent fields before invoking any
     * application CREATOR. Unaddressed or denied broadcasts must not load
     * another app's classes or execute its deserialization code. */
    if(ok) {
      if(!dvm_runtime_broadcast_wanted(vm,intent,uid,package,permission)) {
        dvm_unpin(vm,intent);
        goto done;
      }
      ok=intent_read_extras(vm,parcel,p,intent);
    }
    if(getenv("LUNARIA_TRACE_BROADCAST"))
      fprintf(stderr,"[broadcast] pid=%ld decode from=%s ok=%d failed=%d consumed=%u/%u exception=%u\n",
              (long)getpid(),package ? package : "",ok,p->failed,p->pos,p->size,dvm_exception(vm));
    if(ok && !p->failed && p->pos==p->size && !dvm_exception(vm))
      (void)dvm_runtime_deliver_broadcast(vm,intent,uid,package,permission);
    dvm_unpin(vm,intent);
  }
done:
  if(permission)dvm_unpin(vm,permission);
  union dvm_value ignored= {0};
  (void)parcel_recycle(vm,parcel,NULL,0,&ignored);
  dvm_unpin(vm,parcel);
}

/* One received message.  `name` is the package/process it came from (a parent
 * hearing from a child) or NULL (a child hearing from its parent).  Returns 1
 * when the message was ours. */
static int binder_receive(struct dvm *vm, const char *name, const void *bytes, size_t size)
{
  const uint8_t *m = bytes;
  if (!vm || !m || size < 24 || memcmp(m, "LB", 2)) return 0;
  const bool bind = !memcmp(m, "LBB1", 4), transact = !memcmp(m, "LBT1", 4), reply = !memcmp(m, "LBR1", 4);
  const bool provider_op = !memcmp(m, "LBQ1", 4);
  const bool broadcast = !memcmp(m, "LBC1", 4);
  if (!bind && !transact && !reply && !provider_op && !broadcast) return 0;
  const uint32_t sender = bget(m + 4), seq = bget(m + 8), caller_uid = bget(m + 12), pkglen = bget(m + 16);
  if (pkglen > BINDER_PACKAGE_MAX || size < 24 + (size_t)pkglen) return 1;
  const uint32_t apklen = bget(m + 20 + pkglen);
  if (apklen > BINDER_PATH_MAX || size < 24 + (size_t)pkglen + apklen) return 1;
  const size_t header = 24 + (size_t)pkglen + apklen;
  const uint8_t *body = m + header;
  const size_t blen = size - header;
  char from[BINDER_PACKAGE_MAX + 1];
  memcpy(from, m + 20, pkglen);
  from[pkglen] = 0;
  binder_link_learn(sender, name ? name : pkglen ? from : NULL);
  if(broadcast) {
    binder_receive_broadcast(vm,caller_uid,from,body,blen);
    return 1;
  }
  if (pkglen && !reply) {
    /* The caller is an installed package of this device: make it known here,
     * with its APK where its own sourceDir says it is. */
    char caller[BINDER_PACKAGE_MAX + 1], apk[BINDER_PATH_MAX + 1];
    memcpy(caller, m + 20, pkglen);
    caller[pkglen] = 0;
    memcpy(apk, m + 24 + pkglen, apklen);
    apk[apklen] = 0;
    (void)lunaria_android_package_install(caller, caller_uid, apklen ? apk : NULL);
  }

  if (reply) {
    if (blen < 12 || bget(body + 8) > blen - 12) return 1;
    struct binder_pending *e = NULL;
    pthread_mutex_lock(&g_binder_lock);
    for (e = g_binder_pending; e && e->seq != seq; e = e->next) {}
    pthread_mutex_unlock(&g_binder_lock);
    if (!e) return 1;
    if(e->done)return 1;
    const int32_t status = (int32_t)bget(body), handle = (int32_t)bget(body + 4);
    const uint32_t n = bget(body + 8);
    if (e->connection) {
      e->status = status;
      e->handle = handle;
      e->peer = sender;
      unsigned cookie = dvm_gil_enter_from_guest(vm);
      /* A reply may be pumped by a background thread inside a synchronous
       * provider call while it holds application monitors. ServiceDispatcher
       * posts its callback; borrowing the main identity here can deadlock
       * against those monitors and block the real main Looper too. */
      e->payload=n ? malloc(n) : NULL;
      e->payload_size=n;
      dvm_ref job=(!n || e->payload) ? dvm_new_object(vm,
                  dvm__class_by_desc(vm,"Llunaria/BinderConnection;")) : 0;
      if(n && e->payload)memcpy(e->payload,body+12,n);
      e->done=true;
      bool queued=job && dvm_set_field(vm,job,"seq","I",(union dvm_value) {
        .i=(int32_t)e->seq
      })
      && dvm__queue_runnable_at(vm,job,false,0);
      if(!queued) {
        dvm_unpin(vm,e->connection);
        binder_pending_drop(e);
      }
      dvm_gil_leave_to_guest(vm, cookie);
      return 1;
    }
    uint8_t *copy = n ? malloc(n) : NULL;
    if (n) {
      if (copy) memcpy(copy, body + 12, n);
    }
    struct parcel_fds descriptors= {0};
    int fd_status=parcel_fds_copy(&descriptors,binder_incoming_fds.fds,binder_incoming_fds.count);
    pthread_mutex_lock(&g_binder_lock);
    e->status = fd_status<0 ? fd_status : (n && !copy) ? -ENOMEM : status;
    e->handle = handle;
    e->payload = copy;
    e->payload_size = copy ? n : 0;
    e->descriptors=descriptors;
    e->done = true;
    pthread_mutex_unlock(&g_binder_lock);
    return 1;
  }

  unsigned cookie = dvm_gil_enter_from_guest(vm);
  struct dvm_tstate saved, main_turn = {0};
  dvm__tstate_save(vm, &saved);
  main_turn.cur_thread = vm->main_thread;
  dvm__tstate_load(vm, &main_turn);
  g_binder_caller_uid = caller_uid;
  g_binder_caller_pid = sender;
  if (provider_op) {
    binder_dispatch_request(vm, sender, seq, caller_uid, body, blen, true);
    g_binder_caller_uid = g_binder_caller_pid = 0;
    dvm__tstate_load(vm, &saved);
    dvm_gil_leave_to_guest(vm, cookie);
    return 1;
  }
  if (bind) {
    int op = 0;
    uint32_t intent = 0;
    dvm_ref binder = 0;
    char cls[256] = "";
    int rc = dvm_service_unpack(vm, body, blen, &op, &intent);
    int32_t status = rc, handle = 0;
    uint8_t payload[600];
    size_t psize = 0;
    if (!rc && op == 3) {
      dvm_pin(vm, intent);
      if (svc_bind_for_peer(vm, intent, &binder, cls, sizeof cls) && !dvm_exception(vm)) {
        handle = binder ? binder_handle_for(vm, binder) : 0;
        status = 0;
        const char *desc = "";
        if (binder) {
          union dvm_value d = {0};
          if (am_vcall(vm, binder, "getInterfaceDescriptor", "()Ljava/lang/String;", NULL, 0, &d) && d.l)
            desc = dvm_string_utf8(vm, d.l);
        }
        const size_t cn = strlen(cls), dn = strnlen(desc, 250);
        bput(payload, (uint32_t)cn);
        memcpy(payload + 4, cls, cn);
        bput(payload + 4 + cn, (uint32_t)dn);
        memcpy(payload + 8 + cn, desc, dn);
        psize = 8 + cn + dn;
      } else status = -ENOENT;
      dvm_unpin(vm, intent);
    } else if (!rc) status = -EINVAL;
    if (dvm_exception(vm)) {
      dvm_clear_exception(vm);
      if (status >= 0) status = -EIO;
    }
    binder_reply(sender, seq, status, handle, payload, psize);
  } else {   /* transact */
    binder_dispatch_request(vm, sender, seq, caller_uid, body, blen, false);
  }
  g_binder_caller_uid = g_binder_caller_pid = 0;
  dvm__tstate_load(vm, &saved);
  dvm_gil_leave_to_guest(vm, cookie);
  return 1;
}

/* Parent side: ask `package`'s process to bind the Service `intent` names, and
 * connect `connection` when it answers. */
/* Transport readers can run while the main Looper is blocked. Package and
 * VM state still require the GIL; lifecycle callbacks stay on the main queue. */
int dvm_binder_receive_fds(struct dvm *vm,const char *name,const void *bytes,size_t size,const int *fds,size_t count)
{
  if (!vm || count>LUNA_PROCESS_FD_MAX || (count && !fds)) return 0;
  unsigned cookie = dvm_gil_enter_from_guest(vm);
  struct parcel_fds saved=binder_incoming_fds;
  binder_incoming_fds=(struct parcel_fds) {
    (int *)fds,count
  };
  int handled = binder_receive(vm, name, bytes, size);
  binder_incoming_fds=saved;
  dvm_gil_leave_to_guest(vm, cookie);
  return handled;
}
int dvm_binder_receive(struct dvm *vm,const char *name,const void *bytes,size_t size)
{
  size_t count=0;
  const int *fds=luna_process_message_fds(&count);
  return dvm_binder_receive_fds(vm,name,bytes,size,fds,count);
}
int dvm_binder_receive_io(struct dvm *vm, const void *bytes, size_t size)
{
  const uint8_t *m = bytes;
  if (!vm || !m || size < 24) return 0;
  bool request = !memcmp(m, "LBT1", 4) || !memcmp(m, "LBQ1", 4);
  bool reply = !memcmp(m, "LBR1", 4);
  /* The system delivers registered broadcasts through the process's Binder
   * receiver endpoint. Enqueue onReceive on its Handler/Looper here; do not
   * wait for a lifecycle turn on a possibly busy main thread. */
  bool broadcast = !memcmp(m, "LBC1", 4);
  if (!request && !reply && !broadcast) return 0;
  unsigned cookie = dvm_gil_enter_from_guest(vm);
  if (reply) {
    uint32_t seq = bget(m + 8);
    pthread_mutex_lock(&g_binder_lock);
    struct binder_pending *e = g_binder_pending;
    while (e && e->seq != seq) e = e->next;
    bool connection = e && e->connection;
    pthread_mutex_unlock(&g_binder_lock);
    if (connection) {
      dvm_gil_leave_to_guest(vm, cookie);
      return 0;
    }
  }
  size_t count=0;
  const int *fds=luna_process_message_fds(&count);
  int handled = dvm_binder_receive_fds(vm,NULL,bytes,size,fds,count);
  dvm_gil_leave_to_guest(vm, cookie);
  return handled;
}

int dvm_binder_remote_bind(struct dvm *vm, const char *package, dvm_ref intent, dvm_ref connection)
{
  void *packed = NULL;
  size_t psize = 0;
  int rc = dvm_service_pack(vm, intent, 3, &packed, &psize);
  if (rc) return rc;
  struct binder_pending *e = binder_pending_new();
  if (!e) {
    free(packed);
    return -ENOMEM;
  }
  e->package = strdup(package);
  e->connection = connection;
  dvm_pin(vm, connection);
  uint8_t *at = NULL;
  size_t total = 0;
  uint8_t *m = binder_message("LBB1", e->seq, psize, &at, &total);
  if (!m) {
    free(packed);
    dvm_unpin(vm, connection);
    binder_pending_drop(e);
    return -ENOMEM;
  }
  memcpy(at, packed, psize);
  free(packed);
  rc = g_binder_send ? g_binder_send(g_binder_send_user, package, m, total) : -ENOSYS;
  if (getenv("LUNARIA_TRACE_BINDER")) fprintf(stderr, "[binder] remote bind of %s sent: rc=%d\n", package, rc);
  free(m);
  if (rc < 0) {
    dvm_unpin(vm, connection);
    binder_pending_drop(e);
  }
  return rc;
}
