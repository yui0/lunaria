/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
#include "luna_media_format.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum mf_kind { MF_NONE,MF_I32,MF_I64,MF_FLOAT,MF_DOUBLE,MF_SIZE,MF_RECT,MF_STRING,MF_BUFFER };
struct mf_bytes { void *data; size_t size; uintptr_t address; };
struct mf_buffer {
  unsigned refs;
  struct luna_mf_allocator allocator;
  struct mf_bytes bytes;
};
union mf_value {
  int32_t i32;
  int64_t i64;
  float f;
  double d;
  size_t size;
  int32_t rect[4];
  char *string;
  struct mf_buffer *buffer;
};
struct mf_entry {
  char *name;
  enum mf_kind kind;
  union mf_value value;
  /* Android getString caches a separate result until its next lookup. */
  struct mf_bytes cache;
};
struct AMediaFormat {
  struct luna_mf_allocator allocator;
  struct mf_entry *entries;
  size_t count,capacity;
  struct mf_bytes debug;
};
static void *host_allocate(void *unused,size_t size,uintptr_t *address)
{
  (void)unused;
  void *p=malloc(size ? size : 1);
  *address=(uintptr_t)p;
  return p;
}
static void host_release(void *unused,void *p,uintptr_t address)
{
  (void)unused; (void)address; free(p);
}
static char *mf_string(const char *s)
{
  if (!s) return NULL;
  size_t n=strlen(s)+1;
  char *p=malloc(n);
  if (p) memcpy(p,s,n);
  return p;
}
static struct mf_bytes mf_allocate(const struct luna_mf_allocator *a,size_t size)
{
  struct mf_bytes bytes={0};
  bytes.data=a->allocate(a->context,size ? size : 1,&bytes.address);
  if (bytes.data) bytes.size=size;
  return bytes;
}
static void mf_release(const struct luna_mf_allocator *a,struct mf_bytes *bytes)
{
  if (bytes->data) a->release(a->context,bytes->data,bytes->address);
  memset(bytes,0,sizeof *bytes);
}
static void mf_value_clear(struct mf_entry *e)
{
  if (e->kind==MF_STRING) free(e->value.string);
  if (e->kind==MF_BUFFER && !--e->value.buffer->refs) {
    struct mf_buffer *buffer=e->value.buffer;
    mf_release(&buffer->allocator,&buffer->bytes);
    free(buffer);
  }
  memset(&e->value,0,sizeof e->value);
  e->kind=MF_NONE;
}
static struct mf_entry *mf_find(AMediaFormat *f,const char *name,bool create)
{
  if (!f || !name) return NULL;
  for (size_t i=0;i<f->count;++i)
    if (!strcmp(f->entries[i].name,name)) return &f->entries[i];
  if (!create) return NULL;
  if (f->count==f->capacity) {
    size_t capacity=f->capacity ? f->capacity*2 : 8;
    if (capacity<f->capacity || capacity>SIZE_MAX/sizeof *f->entries) return NULL;
    struct mf_entry *entries=realloc(f->entries,capacity*sizeof *entries);
    if (!entries) return NULL;
    f->entries=entries; f->capacity=capacity;
  }
  char *key=mf_string(name);
  if (!key) return NULL;
  struct mf_entry *e=&f->entries[f->count++];
  memset(e,0,sizeof *e); e->name=key;
  return e;
}
AMediaFormat *luna_media_format_new(const struct luna_mf_allocator *allocator)
{
  if (allocator && (!allocator->allocate || !allocator->release)) return NULL;
  AMediaFormat *f=calloc(1,sizeof *f);
  if (f) {
    const struct luna_mf_allocator host={NULL,host_allocate,host_release};
    f->allocator=allocator ? *allocator : host;
  }
  return f;
}
AMediaFormat *AMediaFormat_new(void) { return luna_media_format_new(NULL); }
void AMediaFormat_clear(AMediaFormat *f)
{
  if (f) for (size_t i=0;i<f->count;++i) mf_value_clear(&f->entries[i]);
}
media_status_t AMediaFormat_delete(AMediaFormat *f)
{
  if (!f) return 0;
  AMediaFormat_clear(f);
  for (size_t i=0;i<f->count;++i) {
    free(f->entries[i].name);
    mf_release(&f->allocator,&f->entries[i].cache);
  }
  free(f->entries);
  mf_release(&f->allocator,&f->debug);
  free(f);
  return 0;
}
#define MF_SCALAR(NAME,TYPE,KIND,MEMBER) \
bool AMediaFormat_get##NAME(AMediaFormat *f,const char *key,TYPE *out) { \
  struct mf_entry *e=mf_find(f,key,false); \
  if (!e || e->kind!=KIND || !out) return false; \
  *out=e->value.MEMBER; return true; \
} \
void AMediaFormat_set##NAME(AMediaFormat *f,const char *key,TYPE value) { \
  struct mf_entry *e=mf_find(f,key,true); \
  if (e) { mf_value_clear(e); e->kind=KIND; e->value.MEMBER=value; } \
}
MF_SCALAR(Int32,int32_t,MF_I32,i32)
MF_SCALAR(Int64,int64_t,MF_I64,i64)
MF_SCALAR(Float,float,MF_FLOAT,f)
MF_SCALAR(Double,double,MF_DOUBLE,d)
MF_SCALAR(Size,size_t,MF_SIZE,size)
#undef MF_SCALAR
void AMediaFormat_setRect(AMediaFormat *f,const char *key,int32_t l,int32_t t,int32_t r,int32_t b)
{
  struct mf_entry *e=mf_find(f,key,true);
  if (!e) return;
  mf_value_clear(e); e->kind=MF_RECT;
  e->value.rect[0]=l; e->value.rect[1]=t; e->value.rect[2]=r; e->value.rect[3]=b;
}
bool AMediaFormat_getRect(AMediaFormat *f,const char *key,int32_t *l,int32_t *t,int32_t *r,int32_t *b)
{
  struct mf_entry *e=mf_find(f,key,false);
  if (!e || e->kind!=MF_RECT || !l || !t || !r || !b) return false;
  *l=e->value.rect[0]; *t=e->value.rect[1]; *r=e->value.rect[2]; *b=e->value.rect[3]; return true;
}
void AMediaFormat_setString(AMediaFormat *f,const char *key,const char *value)
{
  char *copy=mf_string(value);
  if (!copy) return;
  struct mf_entry *e=mf_find(f,key,true);
  if (!e) { free(copy); return; }
  mf_value_clear(e); e->kind=MF_STRING; e->value.string=copy;
}
bool AMediaFormat_getString(AMediaFormat *f,const char *key,const char **out)
{
  struct mf_entry *e=mf_find(f,key,false);
  if (!e) return false;
  mf_release(&f->allocator,&e->cache);
  if (e->kind!=MF_STRING || !out) return false;
  size_t size=strlen(e->value.string)+1;
  e->cache=mf_allocate(&f->allocator,size);
  if (!e->cache.data) return false;
  memcpy(e->cache.data,e->value.string,size);
  *out=e->cache.data;
  return true;
}
void AMediaFormat_setBuffer(AMediaFormat *f,const char *key,const void *data,size_t size)
{
  if (!f || (!data && size)) return;
  struct mf_buffer *buffer=calloc(1,sizeof *buffer);
  if (!buffer) return;
  buffer->allocator=f->allocator;
  buffer->bytes=mf_allocate(&buffer->allocator,size);
  if (!buffer->bytes.data) { free(buffer); return; }
  if (size) memcpy(buffer->bytes.data,data,size);
  buffer->refs=1;
  struct mf_entry *e=mf_find(f,key,true);
  if (!e) { mf_release(&buffer->allocator,&buffer->bytes); free(buffer); return; }
  mf_value_clear(e); e->kind=MF_BUFFER; e->value.buffer=buffer;
}
bool AMediaFormat_getBuffer(AMediaFormat *f,const char *key,void **data,size_t *size)
{
  struct mf_entry *e=mf_find(f,key,false);
  if (!e || e->kind!=MF_BUFFER || !data || !size) return false;
  *data=e->value.buffer->bytes.data; *size=e->value.buffer->bytes.size; return true;
}
media_status_t AMediaFormat_copy(AMediaFormat *to,AMediaFormat *from)
{
  if (!to || !from) return -10004;
  AMediaFormat_clear(to);
  for (size_t i=0;i<from->count;++i) {
    const struct mf_entry *src=&from->entries[i];
    if (src->kind==MF_NONE) continue;
    struct mf_entry *dst=mf_find(to,src->name,true);
    if (!dst) return -10000;
    if (src->kind==MF_STRING) {
      char *copy=mf_string(src->value.string);
      if (!copy) return -10000;
      dst->value.string=copy;
    } else {
      dst->value=src->value;
      if (src->kind==MF_BUFFER) ++dst->value.buffer->refs;
    }
    dst->kind=src->kind;
  }
  return 0;
}
struct mf_text { char *p; size_t length; };
static bool mf_append(struct mf_text *text,const char *format,...)
{
  va_list args,copy;
  va_start(args,format); va_copy(copy,args);
  int count=vsnprintf(NULL,0,format,copy); va_end(copy);
  if (count<0 || (size_t)count>SIZE_MAX-text->length-1) { va_end(args); return false; }
  size_t size=text->length+(size_t)count+1;
  char *p=realloc(text->p,size);
  if (!p) { va_end(args); return false; }
  text->p=p;
  vsnprintf(p+text->length,(size_t)count+1,format,args); va_end(args);
  text->length+=(size_t)count;
  return true;
}
const char *AMediaFormat_toString(AMediaFormat *f)
{
  if (!f) return NULL;
  struct mf_text text={0};
  bool ok=true,first=true;
  for (size_t i=0;ok && i<f->count;++i) {
    struct mf_entry *e=&f->entries[i];
    if (e->kind==MF_NONE) continue;
    ok=mf_append(&text,"%s%s: ",first ? "" : ", ",e->name); first=false;
    if (!ok) break;
    switch (e->kind) {
    case MF_I32: ok=mf_append(&text,"int32(%" PRId32 ")",e->value.i32); break;
    case MF_I64: ok=mf_append(&text,"int64(%" PRId64 ")",e->value.i64); break;
    case MF_FLOAT: ok=mf_append(&text,"float(%f)",(double)e->value.f); break;
    case MF_DOUBLE: ok=mf_append(&text,"double(%f)",e->value.d); break;
    case MF_SIZE: ok=mf_append(&text,"size_t(%zu)",e->value.size); break;
    case MF_RECT: ok=mf_append(&text,"Rect(%" PRId32 ", %" PRId32 ", %" PRId32 ", %" PRId32 ")",e->value.rect[0],e->value.rect[1],e->value.rect[2],e->value.rect[3]); break;
    case MF_STRING: ok=mf_append(&text,"string(%s)",e->value.string); break;
    case MF_BUFFER: ok=mf_append(&text,"data"); break;
    default: break;
    }
  }
  if (ok) ok=mf_append(&text,"}");
  if (ok) {
    struct mf_bytes bytes=mf_allocate(&f->allocator,text.length+1);
    if (bytes.data) {
      memcpy(bytes.data,text.p,text.length+1);
      mf_release(&f->allocator,&f->debug); f->debug=bytes;
    } else ok=false;
  }
  free(text.p);
  return ok ? f->debug.data : NULL;
}
uintptr_t luna_media_format_address(const AMediaFormat *f,const void *p)
{
  if (!f || !p) return 0;
  if (f->debug.data==p) return f->debug.address;
  for (size_t i=0;i<f->count;++i) {
    const struct mf_entry *e=&f->entries[i];
    if (e->cache.data==p) return e->cache.address;
    if (e->kind==MF_BUFFER && e->value.buffer->bytes.data==p) return e->value.buffer->bytes.address;
  }
  return 0;
}
#include "luna_media_format_keys.inc"
