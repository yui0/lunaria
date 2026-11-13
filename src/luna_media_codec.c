/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
/* Native media codec support, in dependency order: the FDK AAC encoder (loaded
 * with dlopen), the AMediaFormat key/value store with the NDK key names, and
 * the encoder state machine that ties them together. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "luna_aac_encoder.h"
#include "luna_media_format.h"
#include "luna_native_codec.h"
#include "lunaria_os.h"
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- FDK AAC encoder ---------------------------------------------------- */
/* Public FDK AAC C ABI. No private codec structures or versioned field offsets.
 * Definitions: Android external/aac, libAACenc/include/aacenc_lib.h. */
struct aac_info {
  unsigned max_output,max_ancillary,fill,channels,frame_length,delay,core_delay;
  unsigned char config[64];
  unsigned config_size;
};
struct aac_buffers { int count; void **buffers; int *ids,*sizes,*elements; };
struct aac_input { int samples,ancillary; };
struct aac_output { int bytes,samples,ancillary,reservoir; };
static struct {
  void *library;
  int (*open)(void **,unsigned,unsigned);
  int (*close)(void **);
  int (*set)(void *,int,unsigned);
  int (*encode)(void *,const struct aac_buffers *,const struct aac_buffers *,
                const struct aac_input *,struct aac_output *);
  int (*info)(void *,struct aac_info *);
} api;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void load_api(void)
{
  static const char *const names[]={"libfdk-aac.so.2","libfdk-aac.so","libfdk-aac.2.dylib","fdk-aac.dll"};
  void *library=NULL;
  for (size_t i=0;i<sizeof names/sizeof *names && !library;++i)
    library=luna_os_library_open_local(names[i]);
  if (!library) return;
#define LOAD(F,N) do { void *p=luna_os_library_symbol(library,N); \
  if (!p) { luna_os_library_close(library); memset(&api,0,sizeof api); return; } \
  memcpy(&api.F,&p,sizeof p); } while (0)
  LOAD(open,"aacEncOpen"); LOAD(close,"aacEncClose");
  LOAD(set,"aacEncoder_SetParam"); LOAD(encode,"aacEncEncode"); LOAD(info,"aacEncInfo");
#undef LOAD
  api.library=library;
}
bool luna_aac_available(void) { pthread_once(&once,load_api); return api.library!=NULL; }
struct luna_aac { void *handle; struct aac_info info; int channels; bool drained; };
struct luna_aac *luna_aac_new(int rate,int channels,int bitrate,int profile)
{
  if (rate<=0 || channels<1 || channels>2 || bitrate<=0 ||
      (profile!=2 && profile!=5 && profile!=29) || !luna_aac_available()) return NULL;
  struct luna_aac *c=calloc(1,sizeof *c);
  if (!c) return NULL;
  c->channels=channels;
  if (api.open(&c->handle,0,(unsigned)channels) ||
      api.set(c->handle,0x100,(unsigned)profile) ||
      api.set(c->handle,0x103,(unsigned)rate) ||
      api.set(c->handle,0x106,(unsigned)channels) ||
      api.set(c->handle,0x101,(unsigned)bitrate) ||
      api.set(c->handle,0x300,0) || /* TT_MP4_RAW */
      api.encode(c->handle,NULL,NULL,NULL,NULL) || api.info(c->handle,&c->info) ||
      c->info.config_size>sizeof c->info.config || !c->info.frame_length ||
      !c->info.max_output) { luna_aac_free(c); return NULL; }
  return c;
}
void luna_aac_free(struct luna_aac *c)
{
  if (!c) return;
  if (c->handle) api.close(&c->handle);
  free(c);
}
const uint8_t *luna_aac_config(const struct luna_aac *c,size_t *size)
{ if (size) *size=c ? c->info.config_size : 0; return c ? c->info.config : NULL; }
unsigned luna_aac_frame_samples(const struct luna_aac *c) { return c ? c->info.frame_length : 0; }
unsigned luna_aac_delay(const struct luna_aac *c) { return c ? c->info.delay : 0; }
size_t luna_aac_max_output(const struct luna_aac *c) { return c ? c->info.max_output : 0; }
int luna_aac_encode(struct luna_aac *c,const int16_t *pcm,int samples,
                    uint8_t *data,size_t capacity,int *consumed,size_t *written)
{
  if (consumed) *consumed=0;
  if (written) *written=0;
  if (!c || !consumed || !written || !data || capacity>INT_MAX ||
      capacity<c->info.max_output || samples < -1 ||
      (samples>=0 && (samples%c->channels || (samples && !pcm)))) return -1;
  if (c->drained) return 1;
  if (samples>INT_MAX/(int)sizeof *pcm) return -1;
  void *in=(void *)pcm,*out=data;
  int in_id=0,out_id=3,in_size=samples<0 ? 0 : samples*(int)sizeof *pcm;
  int out_size=(int)capacity,in_element=sizeof *pcm,out_element=1;
  struct aac_buffers input={1,&in,&in_id,&in_size,&in_element};
  struct aac_buffers output={1,&out,&out_id,&out_size,&out_element};
  struct aac_input args={samples,0};
  struct aac_output result={0};
  int error=api.encode(c->handle,&input,&output,&args,&result);
  if (error==0x80) { c->drained=true; return 1; }
  if (error || result.samples<0 || result.samples>(samples<0 ? 0 : samples) ||
      result.bytes<0 || (size_t)result.bytes>capacity) return -1;
  *consumed=result.samples; *written=(size_t)result.bytes;
  return 0;
}

/* ---- AMediaFormat ------------------------------------------------------- */
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
/* Android 12 NDK MediaFormat key values. SPDX-License-Identifier: MPL-2.0 */
const char *AMEDIAFORMAT_KEY_AAC_DRC_ATTENUATION_FACTOR="aac-drc-cut-level";
const char *AMEDIAFORMAT_KEY_AAC_DRC_BOOST_FACTOR="aac-drc-boost-level";
const char *AMEDIAFORMAT_KEY_AAC_DRC_HEAVY_COMPRESSION="aac-drc-heavy-compression";
const char *AMEDIAFORMAT_KEY_AAC_DRC_TARGET_REFERENCE_LEVEL="aac-target-ref-level";
const char *AMEDIAFORMAT_KEY_AAC_ENCODED_TARGET_LEVEL="aac-encoded-target-level";
const char *AMEDIAFORMAT_KEY_AAC_MAX_OUTPUT_CHANNEL_COUNT="aac-max-output-channel_count";
const char *AMEDIAFORMAT_KEY_AAC_PROFILE="aac-profile";
const char *AMEDIAFORMAT_KEY_AAC_SBR_MODE="aac-sbr-mode";
const char *AMEDIAFORMAT_KEY_ALBUM="album";
const char *AMEDIAFORMAT_KEY_ALBUMART="albumart";
const char *AMEDIAFORMAT_KEY_ALBUMARTIST="albumartist";
const char *AMEDIAFORMAT_KEY_ARTIST="artist";
const char *AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_INFO="audio-presentation-info";
const char *AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PRESENTATION_ID="audio-presentation-presentation-id";
const char *AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PROGRAM_ID="audio-presentation-program-id";
const char *AMEDIAFORMAT_KEY_AUDIO_SESSION_ID="audio-session-id";
const char *AMEDIAFORMAT_KEY_AUTHOR="author";
const char *AMEDIAFORMAT_KEY_BITRATE_MODE="bitrate-mode";
const char *AMEDIAFORMAT_KEY_BITS_PER_SAMPLE="bits-per-sample";
const char *AMEDIAFORMAT_KEY_BIT_RATE="bitrate";
const char *AMEDIAFORMAT_KEY_CAPTURE_RATE="capture-rate";
const char *AMEDIAFORMAT_KEY_CDTRACKNUMBER="cdtracknum";
const char *AMEDIAFORMAT_KEY_CHANNEL_COUNT="channel-count";
const char *AMEDIAFORMAT_KEY_CHANNEL_MASK="channel-mask";
const char *AMEDIAFORMAT_KEY_COLOR_FORMAT="color-format";
const char *AMEDIAFORMAT_KEY_COLOR_RANGE="color-range";
const char *AMEDIAFORMAT_KEY_COLOR_STANDARD="color-standard";
const char *AMEDIAFORMAT_KEY_COLOR_TRANSFER="color-transfer";
const char *AMEDIAFORMAT_KEY_COMPILATION="compilation";
const char *AMEDIAFORMAT_KEY_COMPLEXITY="complexity";
const char *AMEDIAFORMAT_KEY_COMPOSER="composer";
const char *AMEDIAFORMAT_KEY_CREATE_INPUT_SURFACE_SUSPENDED="create-input-buffers-suspended";
const char *AMEDIAFORMAT_KEY_CRYPTO_DEFAULT_IV_SIZE="crypto-default-iv-size";
const char *AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_BYTE_BLOCK="crypto-encrypted-byte-block";
const char *AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_SIZES="crypto-encrypted-sizes";
const char *AMEDIAFORMAT_KEY_CRYPTO_IV="crypto-iv";
const char *AMEDIAFORMAT_KEY_CRYPTO_KEY="crypto-key";
const char *AMEDIAFORMAT_KEY_CRYPTO_MODE="crypto-mode";
const char *AMEDIAFORMAT_KEY_CRYPTO_PLAIN_SIZES="crypto-plain-sizes";
const char *AMEDIAFORMAT_KEY_CRYPTO_SKIP_BYTE_BLOCK="crypto-skip-byte-block";
const char *AMEDIAFORMAT_KEY_CSD="csd";
const char *AMEDIAFORMAT_KEY_CSD_0="csd-0";
const char *AMEDIAFORMAT_KEY_CSD_1="csd-1";
const char *AMEDIAFORMAT_KEY_CSD_2="csd-2";
const char *AMEDIAFORMAT_KEY_CSD_AVC="csd-avc";
const char *AMEDIAFORMAT_KEY_CSD_HEVC="csd-hevc";
const char *AMEDIAFORMAT_KEY_D263="d263";
const char *AMEDIAFORMAT_KEY_DATE="date";
const char *AMEDIAFORMAT_KEY_DISCNUMBER="discnum";
const char *AMEDIAFORMAT_KEY_DISPLAY_CROP="crop";
const char *AMEDIAFORMAT_KEY_DISPLAY_HEIGHT="display-height";
const char *AMEDIAFORMAT_KEY_DISPLAY_WIDTH="display-width";
const char *AMEDIAFORMAT_KEY_DURATION="durationUs";
const char *AMEDIAFORMAT_KEY_ENCODER_DELAY="encoder-delay";
const char *AMEDIAFORMAT_KEY_ENCODER_PADDING="encoder-padding";
const char *AMEDIAFORMAT_KEY_ESDS="esds";
const char *AMEDIAFORMAT_KEY_EXIF_OFFSET="exif-offset";
const char *AMEDIAFORMAT_KEY_EXIF_SIZE="exif-size";
const char *AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL="flac-compression-level";
const char *AMEDIAFORMAT_KEY_FRAME_COUNT="frame-count";
const char *AMEDIAFORMAT_KEY_FRAME_RATE="frame-rate";
const char *AMEDIAFORMAT_KEY_GENRE="genre";
const char *AMEDIAFORMAT_KEY_GRID_COLUMNS="grid-cols";
const char *AMEDIAFORMAT_KEY_GRID_ROWS="grid-rows";
const char *AMEDIAFORMAT_KEY_HAPTIC_CHANNEL_COUNT="haptic-channel-count";
const char *AMEDIAFORMAT_KEY_HDR10_PLUS_INFO="hdr10-plus-info";
const char *AMEDIAFORMAT_KEY_HDR_STATIC_INFO="hdr-static-info";
const char *AMEDIAFORMAT_KEY_HEIGHT="height";
const char *AMEDIAFORMAT_KEY_ICC_PROFILE="icc-profile";
const char *AMEDIAFORMAT_KEY_INTRA_REFRESH_PERIOD="intra-refresh-period";
const char *AMEDIAFORMAT_KEY_IS_ADTS="is-adts";
const char *AMEDIAFORMAT_KEY_IS_AUTOSELECT="is-autoselect";
const char *AMEDIAFORMAT_KEY_IS_DEFAULT="is-default";
const char *AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE="is-forced-subtitle";
const char *AMEDIAFORMAT_KEY_IS_SYNC_FRAME="is-sync-frame";
const char *AMEDIAFORMAT_KEY_I_FRAME_INTERVAL="i-frame-interval";
const char *AMEDIAFORMAT_KEY_LANGUAGE="language";
const char *AMEDIAFORMAT_KEY_LAST_SAMPLE_INDEX_IN_CHUNK="last-sample-index-in-chunk";
const char *AMEDIAFORMAT_KEY_LATENCY="latency";
const char *AMEDIAFORMAT_KEY_LEVEL="level";
const char *AMEDIAFORMAT_KEY_LOCATION="location";
const char *AMEDIAFORMAT_KEY_LOOP="loop";
const char *AMEDIAFORMAT_KEY_LOW_LATENCY="low-latency";
const char *AMEDIAFORMAT_KEY_LYRICIST="lyricist";
const char *AMEDIAFORMAT_KEY_MANUFACTURER="manufacturer";
const char *AMEDIAFORMAT_KEY_MAX_BIT_RATE="max-bitrate";
const char *AMEDIAFORMAT_KEY_MAX_FPS_TO_ENCODER="max-fps-to-encoder";
const char *AMEDIAFORMAT_KEY_MAX_HEIGHT="max-height";
const char *AMEDIAFORMAT_KEY_MAX_INPUT_SIZE="max-input-size";
const char *AMEDIAFORMAT_KEY_MAX_PTS_GAP_TO_ENCODER="max-pts-gap-to-encoder";
const char *AMEDIAFORMAT_KEY_MAX_WIDTH="max-width";
const char *AMEDIAFORMAT_KEY_MIME="mime";
const char *AMEDIAFORMAT_KEY_MPEG2_STREAM_HEADER="mpeg2-stream-header";
const char *AMEDIAFORMAT_KEY_MPEG_USER_DATA="mpeg-user-data";
const char *AMEDIAFORMAT_KEY_OPERATING_RATE="operating-rate";
const char *AMEDIAFORMAT_KEY_PCM_BIG_ENDIAN="pcm-big-endian";
const char *AMEDIAFORMAT_KEY_PCM_ENCODING="pcm-encoding";
const char *AMEDIAFORMAT_KEY_PRIORITY="priority";
const char *AMEDIAFORMAT_KEY_PROFILE="profile";
const char *AMEDIAFORMAT_KEY_PSSH="pssh";
const char *AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP="push-blank-buffers-on-shutdown";
const char *AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER="repeat-previous-frame-after";
const char *AMEDIAFORMAT_KEY_ROTATION="rotation-degrees";
const char *AMEDIAFORMAT_KEY_SAMPLE_FILE_OFFSET="sample-file-offset";
const char *AMEDIAFORMAT_KEY_SAMPLE_RATE="sample-rate";
const char *AMEDIAFORMAT_KEY_SAMPLE_TIME_BEFORE_APPEND="sample-time-before-append";
const char *AMEDIAFORMAT_KEY_SAR_HEIGHT="sar-height";
const char *AMEDIAFORMAT_KEY_SAR_WIDTH="sar-width";
const char *AMEDIAFORMAT_KEY_SEI="sei";
const char *AMEDIAFORMAT_KEY_SLICE_HEIGHT="slice-height";
const char *AMEDIAFORMAT_KEY_SLOW_MOTION_MARKERS="slow-motion-markers";
const char *AMEDIAFORMAT_KEY_STRIDE="stride";
const char *AMEDIAFORMAT_KEY_TARGET_TIME="target-time";
const char *AMEDIAFORMAT_KEY_TEMPORAL_LAYERING="ts-schema";
const char *AMEDIAFORMAT_KEY_TEMPORAL_LAYER_COUNT="temporal-layer-count";
const char *AMEDIAFORMAT_KEY_TEMPORAL_LAYER_ID="temporal-layer-id";
const char *AMEDIAFORMAT_KEY_TEXT_FORMAT_DATA="text-format-data";
const char *AMEDIAFORMAT_KEY_THUMBNAIL_CSD_AV1C="thumbnail-csd-av1c";
const char *AMEDIAFORMAT_KEY_THUMBNAIL_CSD_HEVC="thumbnail-csd-hevc";
const char *AMEDIAFORMAT_KEY_THUMBNAIL_HEIGHT="thumbnail-height";
const char *AMEDIAFORMAT_KEY_THUMBNAIL_TIME="thumbnail-time";
const char *AMEDIAFORMAT_KEY_THUMBNAIL_WIDTH="thumbnail-width";
const char *AMEDIAFORMAT_KEY_TILE_HEIGHT="tile-height";
const char *AMEDIAFORMAT_KEY_TILE_WIDTH="tile-width";
const char *AMEDIAFORMAT_KEY_TIME_US="timeUs";
const char *AMEDIAFORMAT_KEY_TITLE="title";
const char *AMEDIAFORMAT_KEY_TRACK_ID="track-id";
const char *AMEDIAFORMAT_KEY_TRACK_INDEX="track-index";
const char *AMEDIAFORMAT_KEY_VALID_SAMPLES="valid-samples";
const char *AMEDIAFORMAT_KEY_WIDTH="width";
const char *AMEDIAFORMAT_KEY_XMP_OFFSET="xmp-offset";
const char *AMEDIAFORMAT_KEY_XMP_SIZE="xmp-size";
const char *AMEDIAFORMAT_KEY_YEAR="year";
static const struct { const char *name,*value; } mf_keys[]={
  {"AMEDIAFORMAT_KEY_AAC_DRC_ATTENUATION_FACTOR","aac-drc-cut-level"},
  {"AMEDIAFORMAT_KEY_AAC_DRC_BOOST_FACTOR","aac-drc-boost-level"},
  {"AMEDIAFORMAT_KEY_AAC_DRC_HEAVY_COMPRESSION","aac-drc-heavy-compression"},
  {"AMEDIAFORMAT_KEY_AAC_DRC_TARGET_REFERENCE_LEVEL","aac-target-ref-level"},
  {"AMEDIAFORMAT_KEY_AAC_ENCODED_TARGET_LEVEL","aac-encoded-target-level"},
  {"AMEDIAFORMAT_KEY_AAC_MAX_OUTPUT_CHANNEL_COUNT","aac-max-output-channel_count"},
  {"AMEDIAFORMAT_KEY_AAC_PROFILE","aac-profile"},
  {"AMEDIAFORMAT_KEY_AAC_SBR_MODE","aac-sbr-mode"},
  {"AMEDIAFORMAT_KEY_ALBUM","album"},
  {"AMEDIAFORMAT_KEY_ALBUMART","albumart"},
  {"AMEDIAFORMAT_KEY_ALBUMARTIST","albumartist"},
  {"AMEDIAFORMAT_KEY_ARTIST","artist"},
  {"AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_INFO","audio-presentation-info"},
  {"AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PRESENTATION_ID","audio-presentation-presentation-id"},
  {"AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PROGRAM_ID","audio-presentation-program-id"},
  {"AMEDIAFORMAT_KEY_AUDIO_SESSION_ID","audio-session-id"},
  {"AMEDIAFORMAT_KEY_AUTHOR","author"},
  {"AMEDIAFORMAT_KEY_BITRATE_MODE","bitrate-mode"},
  {"AMEDIAFORMAT_KEY_BITS_PER_SAMPLE","bits-per-sample"},
  {"AMEDIAFORMAT_KEY_BIT_RATE","bitrate"},
  {"AMEDIAFORMAT_KEY_CAPTURE_RATE","capture-rate"},
  {"AMEDIAFORMAT_KEY_CDTRACKNUMBER","cdtracknum"},
  {"AMEDIAFORMAT_KEY_CHANNEL_COUNT","channel-count"},
  {"AMEDIAFORMAT_KEY_CHANNEL_MASK","channel-mask"},
  {"AMEDIAFORMAT_KEY_COLOR_FORMAT","color-format"},
  {"AMEDIAFORMAT_KEY_COLOR_RANGE","color-range"},
  {"AMEDIAFORMAT_KEY_COLOR_STANDARD","color-standard"},
  {"AMEDIAFORMAT_KEY_COLOR_TRANSFER","color-transfer"},
  {"AMEDIAFORMAT_KEY_COMPILATION","compilation"},
  {"AMEDIAFORMAT_KEY_COMPLEXITY","complexity"},
  {"AMEDIAFORMAT_KEY_COMPOSER","composer"},
  {"AMEDIAFORMAT_KEY_CREATE_INPUT_SURFACE_SUSPENDED","create-input-buffers-suspended"},
  {"AMEDIAFORMAT_KEY_CRYPTO_DEFAULT_IV_SIZE","crypto-default-iv-size"},
  {"AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_BYTE_BLOCK","crypto-encrypted-byte-block"},
  {"AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_SIZES","crypto-encrypted-sizes"},
  {"AMEDIAFORMAT_KEY_CRYPTO_IV","crypto-iv"},
  {"AMEDIAFORMAT_KEY_CRYPTO_KEY","crypto-key"},
  {"AMEDIAFORMAT_KEY_CRYPTO_MODE","crypto-mode"},
  {"AMEDIAFORMAT_KEY_CRYPTO_PLAIN_SIZES","crypto-plain-sizes"},
  {"AMEDIAFORMAT_KEY_CRYPTO_SKIP_BYTE_BLOCK","crypto-skip-byte-block"},
  {"AMEDIAFORMAT_KEY_CSD","csd"},
  {"AMEDIAFORMAT_KEY_CSD_0","csd-0"},
  {"AMEDIAFORMAT_KEY_CSD_1","csd-1"},
  {"AMEDIAFORMAT_KEY_CSD_2","csd-2"},
  {"AMEDIAFORMAT_KEY_CSD_AVC","csd-avc"},
  {"AMEDIAFORMAT_KEY_CSD_HEVC","csd-hevc"},
  {"AMEDIAFORMAT_KEY_D263","d263"},
  {"AMEDIAFORMAT_KEY_DATE","date"},
  {"AMEDIAFORMAT_KEY_DISCNUMBER","discnum"},
  {"AMEDIAFORMAT_KEY_DISPLAY_CROP","crop"},
  {"AMEDIAFORMAT_KEY_DISPLAY_HEIGHT","display-height"},
  {"AMEDIAFORMAT_KEY_DISPLAY_WIDTH","display-width"},
  {"AMEDIAFORMAT_KEY_DURATION","durationUs"},
  {"AMEDIAFORMAT_KEY_ENCODER_DELAY","encoder-delay"},
  {"AMEDIAFORMAT_KEY_ENCODER_PADDING","encoder-padding"},
  {"AMEDIAFORMAT_KEY_ESDS","esds"},
  {"AMEDIAFORMAT_KEY_EXIF_OFFSET","exif-offset"},
  {"AMEDIAFORMAT_KEY_EXIF_SIZE","exif-size"},
  {"AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL","flac-compression-level"},
  {"AMEDIAFORMAT_KEY_FRAME_COUNT","frame-count"},
  {"AMEDIAFORMAT_KEY_FRAME_RATE","frame-rate"},
  {"AMEDIAFORMAT_KEY_GENRE","genre"},
  {"AMEDIAFORMAT_KEY_GRID_COLUMNS","grid-cols"},
  {"AMEDIAFORMAT_KEY_GRID_ROWS","grid-rows"},
  {"AMEDIAFORMAT_KEY_HAPTIC_CHANNEL_COUNT","haptic-channel-count"},
  {"AMEDIAFORMAT_KEY_HDR10_PLUS_INFO","hdr10-plus-info"},
  {"AMEDIAFORMAT_KEY_HDR_STATIC_INFO","hdr-static-info"},
  {"AMEDIAFORMAT_KEY_HEIGHT","height"},
  {"AMEDIAFORMAT_KEY_ICC_PROFILE","icc-profile"},
  {"AMEDIAFORMAT_KEY_INTRA_REFRESH_PERIOD","intra-refresh-period"},
  {"AMEDIAFORMAT_KEY_IS_ADTS","is-adts"},
  {"AMEDIAFORMAT_KEY_IS_AUTOSELECT","is-autoselect"},
  {"AMEDIAFORMAT_KEY_IS_DEFAULT","is-default"},
  {"AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE","is-forced-subtitle"},
  {"AMEDIAFORMAT_KEY_IS_SYNC_FRAME","is-sync-frame"},
  {"AMEDIAFORMAT_KEY_I_FRAME_INTERVAL","i-frame-interval"},
  {"AMEDIAFORMAT_KEY_LANGUAGE","language"},
  {"AMEDIAFORMAT_KEY_LAST_SAMPLE_INDEX_IN_CHUNK","last-sample-index-in-chunk"},
  {"AMEDIAFORMAT_KEY_LATENCY","latency"},
  {"AMEDIAFORMAT_KEY_LEVEL","level"},
  {"AMEDIAFORMAT_KEY_LOCATION","location"},
  {"AMEDIAFORMAT_KEY_LOOP","loop"},
  {"AMEDIAFORMAT_KEY_LOW_LATENCY","low-latency"},
  {"AMEDIAFORMAT_KEY_LYRICIST","lyricist"},
  {"AMEDIAFORMAT_KEY_MANUFACTURER","manufacturer"},
  {"AMEDIAFORMAT_KEY_MAX_BIT_RATE","max-bitrate"},
  {"AMEDIAFORMAT_KEY_MAX_FPS_TO_ENCODER","max-fps-to-encoder"},
  {"AMEDIAFORMAT_KEY_MAX_HEIGHT","max-height"},
  {"AMEDIAFORMAT_KEY_MAX_INPUT_SIZE","max-input-size"},
  {"AMEDIAFORMAT_KEY_MAX_PTS_GAP_TO_ENCODER","max-pts-gap-to-encoder"},
  {"AMEDIAFORMAT_KEY_MAX_WIDTH","max-width"},
  {"AMEDIAFORMAT_KEY_MIME","mime"},
  {"AMEDIAFORMAT_KEY_MPEG2_STREAM_HEADER","mpeg2-stream-header"},
  {"AMEDIAFORMAT_KEY_MPEG_USER_DATA","mpeg-user-data"},
  {"AMEDIAFORMAT_KEY_OPERATING_RATE","operating-rate"},
  {"AMEDIAFORMAT_KEY_PCM_BIG_ENDIAN","pcm-big-endian"},
  {"AMEDIAFORMAT_KEY_PCM_ENCODING","pcm-encoding"},
  {"AMEDIAFORMAT_KEY_PRIORITY","priority"},
  {"AMEDIAFORMAT_KEY_PROFILE","profile"},
  {"AMEDIAFORMAT_KEY_PSSH","pssh"},
  {"AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP","push-blank-buffers-on-shutdown"},
  {"AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER","repeat-previous-frame-after"},
  {"AMEDIAFORMAT_KEY_ROTATION","rotation-degrees"},
  {"AMEDIAFORMAT_KEY_SAMPLE_FILE_OFFSET","sample-file-offset"},
  {"AMEDIAFORMAT_KEY_SAMPLE_RATE","sample-rate"},
  {"AMEDIAFORMAT_KEY_SAMPLE_TIME_BEFORE_APPEND","sample-time-before-append"},
  {"AMEDIAFORMAT_KEY_SAR_HEIGHT","sar-height"},
  {"AMEDIAFORMAT_KEY_SAR_WIDTH","sar-width"},
  {"AMEDIAFORMAT_KEY_SEI","sei"},
  {"AMEDIAFORMAT_KEY_SLICE_HEIGHT","slice-height"},
  {"AMEDIAFORMAT_KEY_SLOW_MOTION_MARKERS","slow-motion-markers"},
  {"AMEDIAFORMAT_KEY_STRIDE","stride"},
  {"AMEDIAFORMAT_KEY_TARGET_TIME","target-time"},
  {"AMEDIAFORMAT_KEY_TEMPORAL_LAYERING","ts-schema"},
  {"AMEDIAFORMAT_KEY_TEMPORAL_LAYER_COUNT","temporal-layer-count"},
  {"AMEDIAFORMAT_KEY_TEMPORAL_LAYER_ID","temporal-layer-id"},
  {"AMEDIAFORMAT_KEY_TEXT_FORMAT_DATA","text-format-data"},
  {"AMEDIAFORMAT_KEY_THUMBNAIL_CSD_AV1C","thumbnail-csd-av1c"},
  {"AMEDIAFORMAT_KEY_THUMBNAIL_CSD_HEVC","thumbnail-csd-hevc"},
  {"AMEDIAFORMAT_KEY_THUMBNAIL_HEIGHT","thumbnail-height"},
  {"AMEDIAFORMAT_KEY_THUMBNAIL_TIME","thumbnail-time"},
  {"AMEDIAFORMAT_KEY_THUMBNAIL_WIDTH","thumbnail-width"},
  {"AMEDIAFORMAT_KEY_TILE_HEIGHT","tile-height"},
  {"AMEDIAFORMAT_KEY_TILE_WIDTH","tile-width"},
  {"AMEDIAFORMAT_KEY_TIME_US","timeUs"},
  {"AMEDIAFORMAT_KEY_TITLE","title"},
  {"AMEDIAFORMAT_KEY_TRACK_ID","track-id"},
  {"AMEDIAFORMAT_KEY_TRACK_INDEX","track-index"},
  {"AMEDIAFORMAT_KEY_VALID_SAMPLES","valid-samples"},
  {"AMEDIAFORMAT_KEY_WIDTH","width"},
  {"AMEDIAFORMAT_KEY_XMP_OFFSET","xmp-offset"},
  {"AMEDIAFORMAT_KEY_XMP_SIZE","xmp-size"},
  {"AMEDIAFORMAT_KEY_YEAR","year"},
};
const char *luna_media_format_key(const char *name)
{
  if (!name) return NULL;
  size_t low=0,high=sizeof mf_keys/sizeof mf_keys[0];
  while (low<high) {
    size_t mid=low+(high-low)/2;
    int order=strcmp(name,mf_keys[mid].name);
    if (!order) return mf_keys[mid].value;
    if (order<0) high=mid; else low=mid+1;
  }
  return NULL;
}

/* ---- Encoder state machine ---------------------------------------------- */
enum { OK=0,UNKNOWN=-10000,UNSUPPORTED=-10002,INVALID_OBJECT=-10003,
       INVALID_PARAMETER=-10004,INVALID_OPERATION=-38,CODEC_TRY_AGAIN=-1,FORMAT_CHANGED=-2 };
enum { FREE,DEQUEUED,QUEUED,READY };
enum { SLOTS=4,CONFIG=2,EOS=4 };
struct codec_buffer {
  void *data;
  uintptr_t address;
  size_t capacity,offset,size;
  int state;
  int64_t pts;
  unsigned flags;
};
struct lmc_codec {
  struct luna_mf_allocator memory;
  struct luna_aac *aac;
  AMediaFormat *format;
  int rate,channels,bitrate,profile;
  size_t input_capacity;
  bool configured,started,eos,draining,drained,changed,config_sent,has_time;
  int error;
  int64_t base_time;
  uint64_t output_samples;
  struct codec_buffer input[SLOTS],output[SLOTS];
  unsigned pending[SLOTS],head,count,ready[SLOTS],out_head,out_count;
};
static void *host_alloc(void *ctx,size_t size,uintptr_t *address)
{ (void)ctx; void *p=malloc(size); *address=(uintptr_t)p; return p; }
static void host_free(void *ctx,void *p,uintptr_t address)
{ (void)ctx; (void)address; free(p); }
struct lmc_codec *lmc_encoder(const char *mime,const struct luna_mf_allocator *memory)
{
  if (!mime || strcmp(mime,"audio/mp4a-latm") || !luna_aac_available() ||
      (memory && (!memory->allocate || !memory->release))) return NULL;
  struct lmc_codec *c=calloc(1,sizeof *c);
  const struct luna_mf_allocator host={NULL,host_alloc,host_free};
  if (c) c->memory=memory ? *memory : host;
  return c;
}
static void release_buffers(struct lmc_codec *c)
{
  for (int i=0;i<SLOTS;++i) {
    struct codec_buffer *buffers[]={&c->input[i],&c->output[i]};
    for (unsigned j=0;j<2;++j) {
      struct codec_buffer *b=buffers[j];
      if (b->data) c->memory.release(c->memory.context,b->data,b->address);
      memset(b,0,sizeof *b);
    }
  }
  c->head=c->count=c->out_head=c->out_count=0;
}
void lmc_delete(struct lmc_codec *c)
{
  if (!c) return;
  release_buffers(c); luna_aac_free(c->aac); AMediaFormat_delete(c->format); free(c);
}
int lmc_configure(struct lmc_codec *c,AMediaFormat *format,unsigned flags)
{
  if (!c) return INVALID_OBJECT;
  if (c->started) return INVALID_OPERATION;
  if (!format || flags!=1) return INVALID_PARAMETER;
  int32_t rate,channels,bitrate,profile=2,encoding=2,capacity=4096;
  const char *mime=NULL;
  if (!AMediaFormat_getString(format,"mime",&mime) || strcmp(mime,"audio/mp4a-latm") ||
      !AMediaFormat_getInt32(format,"sample-rate",&rate) ||
      !AMediaFormat_getInt32(format,"channel-count",&channels) ||
      !AMediaFormat_getInt32(format,"bitrate",&bitrate)) return INVALID_PARAMETER;
  AMediaFormat_getInt32(format,"aac-profile",&profile);
  AMediaFormat_getInt32(format,"pcm-encoding",&encoding);
  AMediaFormat_getInt32(format,"max-input-size",&capacity);
  if (encoding!=2) return UNSUPPORTED;
  if (capacity<=0 || capacity>INT_MAX/2 || channels<1 || channels>2) return INVALID_PARAMETER;
  struct luna_aac *aac=luna_aac_new(rate,channels,bitrate,profile);
  if (!aac) return UNSUPPORTED;
  AMediaFormat *copy=luna_media_format_new(&c->memory);
  if (!copy) { luna_aac_free(aac); return UNKNOWN; }
  AMediaFormat_copy(copy,format);
  size_t size=0;
  const uint8_t *config=luna_aac_config(aac,&size);
  AMediaFormat_setBuffer(copy,"csd-0",config,size);
  AMediaFormat_setInt32(copy,"encoder-delay",(int32_t)luna_aac_delay(aac));
  AMediaFormat_setInt32(copy,"pcm-encoding",2);
  luna_aac_free(c->aac); AMediaFormat_delete(c->format);
  c->aac=aac; c->format=copy; c->rate=rate; c->channels=channels;
  c->bitrate=bitrate; c->profile=profile; c->input_capacity=(size_t)capacity;
  c->configured=true;
  return OK;
}
int lmc_start(struct lmc_codec *c)
{
  if (!c) return INVALID_OBJECT;
  if (!c->configured || c->started) return INVALID_OPERATION;
  if (!c->aac) c->aac=luna_aac_new(c->rate,c->channels,c->bitrate,c->profile);
  if (!c->aac) return UNKNOWN;
  for (int i=0;i<SLOTS;++i) {
    struct codec_buffer *buffers[]={&c->input[i],&c->output[i]};
    for (unsigned j=0;j<2;++j) {
      struct codec_buffer *b=buffers[j];
      b->capacity=j ? luna_aac_max_output(c->aac) : c->input_capacity;
      b->data=c->memory.allocate(c->memory.context,b->capacity,&b->address);
      if (!b->data) { release_buffers(c); return UNKNOWN; }
    }
  }
  c->started=true; c->changed=true; c->config_sent=false;
  c->eos=c->draining=c->drained=c->has_time=false;
  c->error=0; c->output_samples=0;
  return OK;
}
int lmc_stop(struct lmc_codec *c)
{
  if (!c) return INVALID_OBJECT;
  if (!c->started) return INVALID_OPERATION;
  release_buffers(c); luna_aac_free(c->aac); c->aac=NULL;
  c->started=false; c->configured=false;
  return OK;
}
/* Android does not define encoder flush support. Reject without losing data. */
int lmc_flush(struct lmc_codec *c) { return c ? INVALID_OPERATION : INVALID_OBJECT; }
static void ready(struct lmc_codec *c,unsigned index,size_t size,int64_t pts,unsigned flags)
{
  struct codec_buffer *out=&c->output[index];
  out->size=size; out->offset=0; out->pts=pts; out->flags=flags; out->state=READY;
  c->ready[(c->out_head+c->out_count)%SLOTS]=index; ++c->out_count;
}
static void pump(struct lmc_codec *c)
{
  while (!c->error && !c->drained) {
    unsigned idx=0;
    while (idx<SLOTS && c->output[idx].state!=FREE) ++idx;
    if (idx==SLOTS) return;
    struct codec_buffer *out=&c->output[idx];
    if (!c->config_sent) {
      size_t size=0; const uint8_t *config=luna_aac_config(c->aac,&size);
      if (size>out->capacity) { c->error=UNKNOWN; return; }
      memcpy(out->data,config,size); ready(c,idx,size,0,CONFIG); c->config_sent=true;
      continue;
    }
    if (!c->count && !c->draining) return;
    struct codec_buffer *in=c->count ? &c->input[c->pending[c->head]] : NULL;
    if (in && !c->has_time) { c->base_time=in->pts; c->has_time=true; }
    int samples=in ? (int)(in->size/sizeof(int16_t)) : -1,used=0;
    size_t written=0;
    if (in && !samples) {
      if (in->flags&EOS) c->draining=true;
      in->state=FREE; c->head=(c->head+1)%SLOTS; --c->count;
      continue;
    }
    int result=luna_aac_encode(c->aac,in ? (const int16_t *)((const uint8_t *)in->data+in->offset) : NULL,
                              samples,out->data,out->capacity,&used,&written);
    if (result<0 || (!result && !used && !written && in)) { c->error=UNKNOWN; return; }
    if (in) {
      in->offset+=(size_t)used*sizeof(int16_t); in->size-=(size_t)used*sizeof(int16_t);
      if (!in->size) {
        if (in->flags&EOS) c->draining=true;
        in->state=FREE; c->head=(c->head+1)%SLOTS; --c->count;
      }
    }
    int64_t pts=c->base_time+(int64_t)(c->output_samples*1000000u/(unsigned)c->rate);
    if (written) {
      ready(c,idx,written,pts,0);
      c->output_samples+=luna_aac_frame_samples(c->aac);
    }
    if (result==1) { ready(c,idx,0,pts,EOS); c->drained=true; }
  }
}
int lmc_dequeue_input(struct lmc_codec *c)
{
  if (!c) return INVALID_OBJECT;
  if (!c->started) return INVALID_OPERATION;
  pump(c);
  if (c->error) return c->error;
  if (!c->eos) for (int i=0;i<SLOTS;++i) if (c->input[i].state==FREE) {
    c->input[i].state=DEQUEUED; return i;
  }
  return CODEC_TRY_AGAIN;
}
bool lmc_input_ready(struct lmc_codec *c)
{
  if (!c || !c->started) return true;
  pump(c);
  if (c->error) return true;
  if (!c->eos) for (int i=0;i<SLOTS;++i) if (c->input[i].state==FREE) return true;
  return false;
}
bool lmc_output_ready(struct lmc_codec *c)
{
  if (!c || !c->started) return true;
  pump(c);
  return c->error || c->changed || c->out_count;
}
void *lmc_input(struct lmc_codec *c,size_t idx,size_t *size,uintptr_t *address)
{
  if (size) *size=0;
  if (address) *address=0;
  if (!c || !c->started || idx>=SLOTS || c->input[idx].state!=DEQUEUED) return NULL;
  if (size) *size=c->input[idx].capacity;
  if (address) *address=c->input[idx].address;
  return c->input[idx].data;
}
int lmc_queue_input(struct lmc_codec *c,size_t idx,size_t offset,size_t bytes,int64_t pts,unsigned flags)
{
  if (!c) return INVALID_OBJECT;
  if (!c->started || c->eos) return INVALID_OPERATION;
  if (idx>=SLOTS || c->input[idx].state!=DEQUEUED || flags&~(unsigned)(EOS|CONFIG) ||
      offset>c->input[idx].capacity || bytes>c->input[idx].capacity-offset ||
      offset%sizeof(int16_t) || bytes%(sizeof(int16_t)*(unsigned)c->channels)) return INVALID_PARAMETER;
  struct codec_buffer *in=&c->input[idx];
  in->offset=offset; in->size=bytes; in->pts=pts; in->flags=flags; in->state=QUEUED;
  c->pending[(c->head+c->count)%SLOTS]=(unsigned)idx; ++c->count;
  if (flags&EOS) c->eos=true;
  pump(c);
  return c->error;
}
int lmc_dequeue_output(struct lmc_codec *c,struct lmc_buffer_info *info)
{
  if (!c) return INVALID_OBJECT;
  if (!c->started) return INVALID_OPERATION;
  if (!info) return INVALID_PARAMETER;
  pump(c);
  if (c->error) return c->error;
  if (c->changed) { c->changed=false; return FORMAT_CHANGED; }
  if (!c->out_count) return CODEC_TRY_AGAIN;
  unsigned idx=c->ready[c->out_head]; c->out_head=(c->out_head+1)%SLOTS; --c->out_count;
  struct codec_buffer *out=&c->output[idx]; out->state=DEQUEUED;
  info->offset=0; info->size=(int32_t)out->size; info->time_us=out->pts; info->flags=out->flags;
  return (int)idx;
}
void *lmc_output(struct lmc_codec *c,size_t idx,size_t *size,uintptr_t *address)
{
  if (size) *size=0;
  if (address) *address=0;
  if (!c || !c->started || idx>=SLOTS || c->output[idx].state!=DEQUEUED) return NULL;
  if (size) *size=c->output[idx].size;
  if (address) *address=c->output[idx].address;
  return c->output[idx].data;
}
int lmc_release_output(struct lmc_codec *c,size_t idx,bool render)
{
  if (!c) return INVALID_OBJECT;
  if (!c->started) return INVALID_OPERATION;
  if (idx>=SLOTS || c->output[idx].state!=DEQUEUED || render) return INVALID_PARAMETER;
  c->output[idx].state=FREE; pump(c);
  return OK;
}
AMediaFormat *lmc_output_format(struct lmc_codec *c)
{
  if (!c || !c->started) return NULL;
  AMediaFormat *copy=luna_media_format_new(&c->memory);
  if (copy) AMediaFormat_copy(copy,c->format);
  return copy;
}
