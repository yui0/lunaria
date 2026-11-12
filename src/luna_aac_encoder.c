/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
#include "luna_aac_encoder.h"
#include "lunaria_os.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

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
