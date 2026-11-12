/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
#include "luna_native_codec.h"
#include "luna_aac_encoder.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
enum { OK=0,UNKNOWN=-10000,UNSUPPORTED=-10002,INVALID_OBJECT=-10003,
       INVALID_PARAMETER=-10004,INVALID_OPERATION=-38,TRY_AGAIN=-1,FORMAT_CHANGED=-2 };
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
  return TRY_AGAIN;
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
  if (!c->out_count) return TRY_AGAIN;
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
