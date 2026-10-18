/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The decoding half of android.media.MediaCodec.
 *
 * dvm_runtime.c owns the Java objects (MediaCodec, MediaFormat, Surface,
 * SurfaceTexture) and the GL calls; this file owns the bitstream.  The split
 * keeps the codec free of any dvm heap knowledge, so it can be exercised on
 * its own.
 *
 * H.264 goes to Cisco's openh264, opened with dlopen() at run time — see the
 * fetch-openh264 rule in the Makefile for why it is a download and not a
 * dependency.  When it is absent the codec reports that it cannot be created,
 * which is what a device without that decoder does.
 */

#ifndef LUNARIA_DVM_MEDIA_H
#define LUNARIA_DVM_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- the frame sink behind android.view.Surface -------------------------- *
 *
 * A MediaCodec renders into a Surface; a SurfaceTexture takes the frames back
 * out.  On a device that is a BufferQueue.  Here one frame is enough: the
 * consumer (updateTexImage) always wants the newest, and a dropped frame is
 * exactly what a device does when the consumer falls behind. */
struct lm_sink;

struct lm_sink *lm_sink_new(void);
/* Retain while an asynchronous producer owns a destination. free releases one
 * reference; a detached window need not keep historical buffers alive. */
void lm_sink_ref(struct lm_sink *s);
void lm_sink_free(struct lm_sink *s);

/* Producer side.  `rgba` is w*h*4 bytes, first row = top of the picture. */
void lm_sink_push(struct lm_sink *s, const uint8_t *rgba, int w, int h,
                  int64_t timestamp_ns);

/* Producer side, without the handover copy.  A decoded frame is 3.6 MB at
 * 720p, and lm_sink_push() copies into a buffer the sink already owns: the
 * producer used to convert into a malloc'd scratch buffer and then copy that
 * in, which costs one 3.6 MB allocation (fresh pages, so a fault per 4 KB
 * written), one full write and one full read+write on every video frame --
 * all of it on the thread the guest is blocked on.  begin() hands back the
 * sink's own buffer to convert into, commit() publishes it. */
uint8_t *lm_sink_begin(struct lm_sink *s, int w, int h);
void lm_sink_commit(struct lm_sink *s, int w, int h, int64_t timestamp_ns);

/* Consumer side.  False when no frame has arrived since the last take; the
 * pointer stays valid until the next push. */
bool lm_sink_take(struct lm_sink *s, const uint8_t **rgba, int *w, int *h);

/* Last frame still in the sink (even after take() cleared pending). */
bool lm_sink_peek(const struct lm_sink *s, const uint8_t **rgba, int *w, int *h);

/* Presentation time of the frame the last take() handed out, in nanoseconds —
 * SurfaceTexture.getTimestamp(). */
int64_t lm_sink_timestamp(const struct lm_sink *s);

/* --- the codec ----------------------------------------------------------- */

struct lm_codec;

/* MediaCodec.dequeueOutputBuffer() results, as the platform defines them. */
enum {
   LM_INFO_TRY_AGAIN_LATER      = -1,
   LM_INFO_OUTPUT_FORMAT_CHANGED = -2,
   LM_INFO_OUTPUT_BUFFERS_CHANGED = -3,
};

/* MediaCodec buffer flags. */
enum {
   LM_BUFFER_FLAG_CODEC_CONFIG = 2,
   LM_BUFFER_FLAG_END_OF_STREAM = 4,
};

/* Whether a decoder for this mime type can be built at all.  MediaCodecList
 * answers from this, so the list never advertises a codec that createByCodecName
 * would then fail to produce. */
bool lm_codec_supported(const char *mime);

/* NULL when the mime type has no decoder here.  `mime` is kept by value. */
struct lm_codec *lm_codec_new(const char *mime);
void lm_codec_free(struct lm_codec *c);

/* csd-0 / csd-1 out of the MediaFormat handed to configure().  The platform
 * feeds these to the decoder at start(); so does lm_codec_start(). */
void lm_codec_add_csd(struct lm_codec *c, const uint8_t *data, size_t len);

bool lm_codec_start(struct lm_codec *c);
void lm_codec_stop(struct lm_codec *c);
void lm_codec_flush(struct lm_codec *c);

/* -1 when every input buffer is still queued. */
int lm_codec_dequeue_input(struct lm_codec *c);
/* The bytes behind input buffer `idx`, and its capacity. */
uint8_t *lm_codec_input_buffer(struct lm_codec *c, int idx, size_t *cap);
/* Hands the buffer to the decoder.  Decoding happens here, so that
 * dequeueOutputBuffer() only has to look at what is already done. */
bool lm_codec_queue_input(struct lm_codec *c, int idx, int offset, int size,
                          int64_t pts_us, int flags);

/* >= 0 is an output buffer index; otherwise one of the LM_INFO_* codes. */
int lm_codec_dequeue_output(struct lm_codec *c, int64_t *pts_us, int *size,
                            int *flags);
/* Decoded picture size, valid once the first frame has been decoded. */
void lm_codec_output_size(const struct lm_codec *c, int *w, int *h);
/* Raw output bytes for a codec configured without a surface. */
const uint8_t *lm_codec_output_buffer(struct lm_codec *c, int idx, size_t *len);
/* Frees the buffer, and when `render` is set pushes it to `sink` (which may be
 * NULL — a codec configured without a surface just drops it). */
void lm_codec_release_output(struct lm_codec *c, int idx, bool render,
                             struct lm_sink *sink);

/* LUNARIA_TRACE_MEDIA: whether to log the codec state machine.  Shared so the
 * Java side of MediaCodec can log on the same switch. */
bool lm_media_trace(void);

struct dvm;
/* Advance every MediaPlayer that is currently playing.  Called from the host
 * pump loop so intro movies decode between native isPlaying polls. */
void dvm_media_pump_active(struct dvm *vm);

/* Audio decoders answer these instead of a picture size. */
void lm_codec_audio_format(const struct lm_codec *c, int *rate, int *channels);
bool lm_codec_is_video(const struct lm_codec *c);
/* "video/avc", "video/x-vnd.on2.vp9", … or NULL when this is not a video codec. */
const char *lm_codec_mime(const struct lm_codec *c);

/* True once the decoder has refused enough of this stream, with nothing ever
 * decoded, that it will not read it at all — a Main/High profile movie handed
 * to openh264's Constrained Baseline decoder, in practice.  A player must turn
 * this into an error for its listener rather than go on "playing" a movie that
 * will never produce a frame. */
bool lm_codec_undecodable(const struct lm_codec *c);

/* --- MP4 demuxing -------------------------------------------------------- *
 *
 * android.media.MediaExtractor and android.media.MediaPlayer are both handed a
 * file descriptor and expected to know what is in it: how many tracks, their
 * formats, and where every sample starts.  That is a container question, not a
 * codec question, so it lives here next to the bitstream code and knows
 * nothing about the dvm heap.
 *
 * Only the ISO base media format (MP4/M4A/MOV) is read — the one UE4 ships its
 * movies in.  A file this cannot parse opens as zero tracks, which is what a
 * device reports for a container it does not support. */
struct lm_mp4;

/* `fd` is borrowed for the lifetime of the demuxer (it is dup()ed inside).
 * `length` <= 0 means "to the end of the file". */
struct lm_mp4 *lm_mp4_open(int fd, int64_t offset, int64_t length);
void lm_mp4_free(struct lm_mp4 *m);

int lm_mp4_tracks(const struct lm_mp4 *m);
/* "video/avc", "audio/mp4a-latm", … — the MediaFormat KEY_MIME of track `t`. */
const char *lm_mp4_mime(const struct lm_mp4 *m, int t);
int64_t lm_mp4_duration_us(const struct lm_mp4 *m, int t);
void lm_mp4_video_size(const struct lm_mp4 *m, int t, int *w, int *h);
void lm_mp4_audio_format(const struct lm_mp4 *m, int t, int *rate, int *channels);
/* csd-0: Annex-B SPS/PPS for AVC, the AudioSpecificConfig for AAC. */
const uint8_t *lm_mp4_csd(const struct lm_mp4 *m, int t, size_t *len);
int lm_mp4_samples(const struct lm_mp4 *m, int t);

/* Sample `i` of track `t`, read from the file into the demuxer's own buffer
 * (valid until the next call).  AVC samples come back as Annex-B so a decoder
 * fed straight from here needs no further framing. */
const uint8_t *lm_mp4_sample(struct lm_mp4 *m, int t, int i, size_t *len,
                             int64_t *pts_us, bool *sync);
/* Index of the last sync sample at or before `us` — where a seek lands. */
int lm_mp4_sync_sample_at(const struct lm_mp4 *m, int t, int64_t us);

/* Owned PCM queues shared by Java AudioTrack and the output mixer. */
struct luna_audio_track;
struct luna_audio_track *luna_at_create(unsigned rate, unsigned channels, unsigned capacity);
void luna_at_destroy(void *track);
void luna_at_play(struct luna_audio_track *track, int playing);
void luna_at_flush(struct luna_audio_track *track);
void luna_at_volume(struct luna_audio_track *track, float volume);
unsigned luna_at_write(struct luna_audio_track *track, const int16_t *pcm, unsigned frames);
void luna_at_wait_space(struct luna_audio_track *track);
unsigned luna_at_space(struct luna_audio_track *track);
uint64_t luna_at_played(struct luna_audio_track *track);
int luna_at_active(void);
unsigned luna_at_mix(int32_t *mix, unsigned frames, unsigned rate, unsigned channels);
void luna_at_advance(unsigned frames, unsigned rate, unsigned channels);

#endif /* LUNARIA_DVM_MEDIA_H */

/* Included by dvm_runtime.c after its object and collection helpers. */
#ifdef LUNARIA_DVM_MEDIA_ROUTER_IMPLEMENTATION
/* MPL-2.0. Process-local Android MediaRouter state and host audio route. */
#define MR_USER 0x800000
#define MR_ANY (MR_USER|7)
#define MR_ROUTE "Landroid/media/MediaRouter$RouteInfo;"
#define MR_ROUTER "Landroid/media/MediaRouter;"
#define MR_CATEGORY "Landroid/media/MediaRouter$RouteCategory;"
static dvm_ref mr_obj(struct dvm *vm,dvm_ref o,const char *field,const char *type)
{union dvm_value v={0};dvm_get_field(vm,o,field,type,&v);return v.l;}
static void mr_put(struct dvm *vm,dvm_ref o,const char *field,const char *type,dvm_ref value)
{dvm_set_field(vm,o,field,type,(union dvm_value){.l=value});}
static dvm_ref mr_list(struct dvm *vm,dvm_ref o,const char *field)
{return mr_obj(vm,o,field,"Ljava/util/List;");}
static bool mr_contains(struct dvm *vm,dvm_ref list,dvm_ref item)
{struct rt_list *l=list_of(vm,list);for(uint32_t i=0;l && i<l->size;++i)if(l->items[i]==item)return true;return false;}
static bool mr_append(struct dvm *vm,dvm_ref list,dvm_ref item)
{union dvm_value a={.l=item},r={0};return list_add(vm,list,&a,1,&r);}
static bool mr_delete(struct dvm *vm,dvm_ref list,dvm_ref item)
{
 struct rt_list *l=list_of(vm,list);for(uint32_t i=0;l && i<l->size;++i)if(l->items[i]==item){memmove(l->items+i,l->items+i+1,(l->size-i-1)*sizeof *l->items);l->items[--l->size]=0;++l->mod_count;return true;}return false;
}
static dvm_ref *mr_snapshot(struct dvm *vm,dvm_ref list,uint32_t *count)
{
 struct rt_list *l=list_of(vm,list);*count=l ? l->size : 0;
 dvm_ref *copy=*count ? malloc(*count*sizeof *copy) : NULL;
 if(*count && !copy){dvm__throw(vm,"java/lang/OutOfMemoryError","MediaRouter notification");*count=0;return NULL;}
 for(uint32_t i=0;i<*count;++i){copy[i]=l->items[i];dvm_pin(vm,copy[i]);}return copy;
}
static void mr_release(struct dvm *vm,dvm_ref *copy,uint32_t count)
{for(uint32_t i=0;i<count;++i)dvm_unpin(vm,copy[i]);free(copy);}
static bool mr_call(struct dvm *vm,dvm_ref callback,const char *name,const char *sig,union dvm_value *args,int count)
{
 struct dvm_method *m=dvm_find_method(vm,dvm_object_class(vm,callback),name,sig);union dvm_value out={0};
 if(!m){dvm__throw(vm,"java/lang/AbstractMethodError","%s",name);return false;}return dvm_call(vm,m,callback,args,count,&out);
}
static bool mr_notify(struct dvm *vm,dvm_ref router,dvm_ref route,const char *event,int types,dvm_ref group,int index)
{
 uint32_t count=0;dvm_ref list=mr_list(vm,router,"callbacks"),*records=mr_snapshot(vm,list,&count);bool good=!dvm_exception(vm);
 for(uint32_t i=0;good && i<count;++i){
  if(!mr_contains(vm,list,records[i]))continue;
  int mask=rt_get_int(vm,records[i],"types"),flags=rt_get_int(vm,records[i],"flags");
  if(!(flags&2) && !(mask&rt_get_int(vm,route,"types")))continue;
  dvm_ref cb=mr_obj(vm,records[i],"callback","Landroid/media/MediaRouter$Callback;");
  union dvm_value args[4]={{.l=router},{.l=route},{.l=group},{.i=index}};
  const char *sig="(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;)V";int n=2;
  if(!strcmp(event,"onRouteSelected") || !strcmp(event,"onRouteUnselected")){args[1].i=types;args[2].l=route;n=3;sig="(Landroid/media/MediaRouter;ILandroid/media/MediaRouter$RouteInfo;)V";}
  else if(group){n=!strcmp(event,"onRouteGrouped") ? 4 : 3;sig=n==4 ? "(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;Landroid/media/MediaRouter$RouteGroup;I)V" : "(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;Landroid/media/MediaRouter$RouteGroup;)V";}
  good=mr_call(vm,cb,event,sig,args,n);
 }
 mr_release(vm,records,count);return good;
}
static dvm_ref mr_new_route(struct dvm *vm,dvm_ref router,dvm_ref category,const char *cls)
{
 dvm_ref route=dvm_new_object(vm,dvm_find_class(vm,cls));if(!route)return 0;
 mr_put(vm,route,"router",MR_ROUTER,router);mr_put(vm,route,"category",MR_CATEGORY,category);
 rt_set_int(vm,route,"enabled",1);rt_set_int(vm,route,"stream",3);return route;
}
static bool mr_changed(struct dvm *vm,dvm_ref router,dvm_ref route,int old_types)
{
 uint32_t n=0;dvm_ref list=mr_list(vm,router,"callbacks"),*records=mr_snapshot(vm,list,&n);bool good=!dvm_exception(vm);int now=rt_get_int(vm,route,"types");
 const char *plain="(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;)V",*selection="(Landroid/media/MediaRouter;ILandroid/media/MediaRouter$RouteInfo;)V";
 for(uint32_t i=0;good && i<n;++i){
  if(!mr_contains(vm,list,records[i]))continue;
  int mask=rt_get_int(vm,records[i],"types"),flags=rt_get_int(vm,records[i],"flags");bool before=(flags&2)||(old_types&mask),after=(flags&2)||(now&mask);
  dvm_ref cb=mr_obj(vm,records[i],"callback","Landroid/media/MediaRouter$Callback;");union dvm_value a[]={{.l=router},{.l=route}},selected[]={{.l=router},{.i=now},{.l=route}};
  if(!before && after){good=mr_call(vm,cb,"onRouteAdded",plain,a,2);if(good && mr_obj(vm,router,"selectedRoute",MR_ROUTE)==route)good=mr_call(vm,cb,"onRouteSelected",selection,selected,3);}
  if(good && (before || after))good=mr_call(vm,cb,"onRouteChanged",plain,a,2);
  if(good && before && !after){selected[1].i=old_types;if(mr_obj(vm,router,"selectedRoute",MR_ROUTE)==route)good=mr_call(vm,cb,"onRouteUnselected",selection,selected,3);if(good)good=mr_call(vm,cb,"onRouteRemoved",plain,a,2);}
 }
 mr_release(vm,records,n);return good;
}
bool dvm__media_router_update_types(struct dvm *vm,dvm_ref route,int types)
{
 dvm_ref router=mr_obj(vm,route,"router",MR_ROUTER);if(!router){dvm__throw(vm,"java/lang/IllegalArgumentException","route");return false;}
 int old=rt_get_int(vm,route,"types");if(old==types)return true;rt_set_int(vm,route,"types",types);return mr_changed(vm,router,route,old);
}
static bool mr_ensure(struct dvm *vm,dvm_ref router,dvm_ref context)
{
 if(mr_obj(vm,router,"defaultRoute",MR_ROUTE)){
  if(context && !mr_obj(vm,router,"resources","Landroid/content/res/Resources;")){union dvm_value resources={0};if(!am_vcall(vm,context,"getResources","()Landroid/content/res/Resources;",NULL,0,&resources))return false;mr_put(vm,router,"resources","Landroid/content/res/Resources;",resources.l);}return true;
 }
 const char *lists[]={"routes","categories","callbacks"};
 for(unsigned i=0;i<3;++i){dvm_ref list=coll_new_list(vm,"Ljava/util/ArrayList;");if(!list)return false;mr_put(vm,router,lists[i],"Ljava/util/List;",list);}
 dvm_ref category=dvm_new_object(vm,dvm_find_class(vm,"android/media/MediaRouter$RouteCategory"));if(!category)return false;
 mr_put(vm,category,"router",MR_ROUTER,router);mr_put(vm,category,"name","Ljava/lang/CharSequence;",dvm_new_string(vm,"Device"));
 rt_set_int(vm,category,"types",3);rt_set_int(vm,category,"system",1);
 if(!mr_append(vm,mr_list(vm,router,"categories"),category))return false;
 dvm_ref route=mr_new_route(vm,router,category,"android/media/MediaRouter$RouteInfo");if(!route)return false;
 mr_put(vm,route,"name","Ljava/lang/CharSequence;",dvm_new_string(vm,"Device"));rt_set_int(vm,route,"types",3);rt_set_int(vm,route,"volumeHandling",1);
 mr_put(vm,router,"defaultRoute",MR_ROUTE,route);mr_put(vm,router,"selectedRoute",MR_ROUTE,route);
 if(!mr_append(vm,mr_list(vm,router,"routes"),route))return false;
 /* Initialization loads the existing host preferences before a volume write. */
 (void)luna_os_audio_volume();
 if(context){union dvm_value resources={0};if(!am_vcall(vm,context,"getResources","()Landroid/content/res/Resources;",NULL,0,&resources))return false;mr_put(vm,router,"resources","Landroid/content/res/Resources;",resources.l);}
 return true;
}
static bool mr_select(struct dvm *vm,dvm_ref router,dvm_ref route,int types)
{
 if(!route){dvm__throw(vm,"java/lang/IllegalArgumentException","Route cannot be null");return false;}
 if(!(rt_get_int(vm,route,"types")&types))return true;
 if(mr_obj(vm,route,"router",MR_ROUTER)!=router){dvm__throw(vm,"java/lang/IllegalArgumentException","Foreign route");return false;}
 dvm_ref old=mr_obj(vm,router,"selectedRoute",MR_ROUTE);if(old==route)return true;
 mr_put(vm,router,"selectedRoute",MR_ROUTE,route);
 return (!old || mr_notify(vm,router,old,"onRouteUnselected",types&rt_get_int(vm,old,"types"),0,0)) && mr_notify(vm,router,route,"onRouteSelected",types&rt_get_int(vm,route,"types"),0,0);
}
static bool mr_remove(struct dvm *,dvm_ref,dvm_ref);
static bool mr_group_refresh(struct dvm *vm,dvm_ref group)
{
 struct rt_list *l=list_of(vm,mr_list(vm,group,"members"));int types=0,max=0,volume=0,remote=0,variable=0,old=rt_get_int(vm,group,"types");
 for(uint32_t i=0;l && i<l->size;++i){dvm_ref r=l->items[i];types|=rt_get_int(vm,r,"types");int local=rt_get_int(vm,r,"playbackType")==0,m=local ? 15 : rt_get_int(vm,r,"volumeMax"),v=local ? (int)lroundf(luna_os_audio_volume()*15) : rt_get_int(vm,r,"volume");if(m>max)max=m;if(v>volume)volume=v;remote|=!local;variable|=rt_get_int(vm,r,"volumeHandling");}
 rt_set_int(vm,group,"types",types);rt_set_int(vm,group,"volumeMax",max);rt_set_int(vm,group,"volume",volume);rt_set_int(vm,group,"playbackType",remote);rt_set_int(vm,group,"volumeHandling",variable);
 mr_put(vm,group,"icon","Landroid/graphics/drawable/Drawable;",l && l->size==1 ? mr_obj(vm,l->items[0],"icon","Landroid/graphics/drawable/Drawable;") : 0);
 return mr_changed(vm,mr_obj(vm,group,"router",MR_ROUTER),group,old);
}
static bool mr_group_add(struct dvm *vm,dvm_ref group,dvm_ref route,int index)
{
 if(mr_obj(vm,route,"group","Landroid/media/MediaRouter$RouteGroup;")){dvm__throw(vm,"java/lang/IllegalStateException","Route already grouped");return false;}
 if(mr_obj(vm,group,"category",MR_CATEGORY)!=mr_obj(vm,route,"category",MR_CATEGORY)){dvm__throw(vm,"java/lang/IllegalArgumentException","Different route category");return false;}
 dvm_ref list=mr_list(vm,group,"members");struct rt_list *l=list_of(vm,list);uint32_t n=l ? l->size : 0;
 if(index<0 || (uint32_t)index>n){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","group index");return false;}
 if(!mr_append(vm,list,route))return false;l=list_of(vm,list);memmove(l->items+index+1,l->items+index,(n-(uint32_t)index)*sizeof *l->items);l->items[index]=route;
 mr_put(vm,route,"group","Landroid/media/MediaRouter$RouteGroup;",group);
 return mr_group_refresh(vm,group) && mr_notify(vm,mr_obj(vm,group,"router",MR_ROUTER),route,"onRouteGrouped",0,group,index);
}
static bool mr_add(struct dvm *vm,dvm_ref router,dvm_ref route)
{
 if(!route){dvm__throw(vm,"java/lang/NullPointerException","route");return false;}
 if(mr_contains(vm,mr_list(vm,router,"routes"),route) || mr_obj(vm,route,"group","Landroid/media/MediaRouter$RouteGroup;"))return true;
 dvm_ref category=mr_obj(vm,route,"category",MR_CATEGORY);
 if(mr_obj(vm,category,"router",MR_ROUTER)!=router){dvm__throw(vm,"java/lang/IllegalArgumentException","Foreign route category");return false;}
 if(!mr_contains(vm,mr_list(vm,router,"categories"),category) && !mr_append(vm,mr_list(vm,router,"categories"),category))return false;
 if(rt_get_int(vm,category,"groupable")){
  dvm_ref group=mr_new_route(vm,router,category,"android/media/MediaRouter$RouteGroup");if(!group)return false;
  mr_put(vm,group,"group","Landroid/media/MediaRouter$RouteGroup;",group);mr_put(vm,group,"members","Ljava/util/List;",coll_new_list(vm,"Ljava/util/ArrayList;"));rt_set_int(vm,group,"types",rt_get_int(vm,route,"types"));
  if(!mr_append(vm,mr_list(vm,router,"routes"),group) || !mr_notify(vm,router,group,"onRouteAdded",0,0,0))return false;
  return mr_group_add(vm,group,route,0);
 }
 return mr_append(vm,mr_list(vm,router,"routes"),route) && mr_notify(vm,router,route,"onRouteAdded",0,0,0);
}
static bool mr_remove(struct dvm *vm,dvm_ref router,dvm_ref route)
{
 if(route==mr_obj(vm,router,"defaultRoute",MR_ROUTE))return true;
 dvm_ref group=mr_obj(vm,route,"group","Landroid/media/MediaRouter$RouteGroup;");
 if(group && group!=route){
  if(!mr_delete(vm,mr_list(vm,group,"members"),route))return true;mr_put(vm,route,"group","Landroid/media/MediaRouter$RouteGroup;",0);
  if(!mr_notify(vm,router,route,"onRouteUngrouped",0,group,0))return false;
  struct rt_list *members=list_of(vm,mr_list(vm,group,"members"));return members && members->size ? mr_group_refresh(vm,group) : mr_remove(vm,router,group);
 }
 if(!mr_delete(vm,mr_list(vm,router,"routes"),route))return true;
 if(mr_obj(vm,router,"selectedRoute",MR_ROUTE)==route && !mr_select(vm,router,mr_obj(vm,router,"defaultRoute",MR_ROUTE),MR_ANY))return false;
 dvm_ref category=mr_obj(vm,route,"category",MR_CATEGORY);struct rt_list *routes=list_of(vm,mr_list(vm,router,"routes"));bool used=false;
 for(uint32_t i=0;routes && i<routes->size;++i)used |= mr_obj(vm,routes->items[i],"category",MR_CATEGORY)==category;
 if(!used)mr_delete(vm,mr_list(vm,router,"categories"),category);
 return mr_notify(vm,router,route,"onRouteRemoved",0,0,0);
}
static bool mr_manager(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 if(!mr_ensure(vm,self,0))return false;const char *name=builtin_name();
 if(!strcmp(name,"getDefaultRoute"))RETL(mr_obj(vm,self,"defaultRoute",MR_ROUTE));
 if(!strcmp(name,"getSelectedRoute")){int types=nargs ? ARG(0).i : MR_ANY;dvm_ref route=mr_obj(vm,self,"selectedRoute",MR_ROUTE);if(rt_get_int(vm,route,"types")&types)RETL(route);RETL(types==MR_USER ? 0 : mr_obj(vm,self,"defaultRoute",MR_ROUTE));}
 if(!strcmp(name,"getRouteCount") || !strcmp(name,"getCategoryCount")){struct rt_list *l=list_of(vm,mr_list(vm,self,name[3]=='R' ? "routes" : "categories"));RETI(l ? l->size : 0);}
 if(!strcmp(name,"getRouteAt") || !strcmp(name,"getCategoryAt")){struct rt_list *l=list_of(vm,mr_list(vm,self,name[3]=='R' ? "routes" : "categories"));if(!l || ARG(0).i<0 || (uint32_t)ARG(0).i>=l->size){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","MediaRouter index");return false;}RETL(l->items[ARG(0).i]);}
 if(!strcmp(name,"isRouteAvailable")){struct rt_list *l=list_of(vm,mr_list(vm,self,"routes"));dvm_ref def=mr_obj(vm,self,"defaultRoute",MR_ROUTE);for(uint32_t i=0;l && i<l->size;++i)if((rt_get_int(vm,l->items[i],"types")&ARG(0).i) && (!(ARG(1).i&1) || l->items[i]!=def))RETI(1);RETI(0);}
 if(!strcmp(name,"selectRoute") || !strcmp(name,"selectRouteInt")){if(!mr_select(vm,self,ARG(1).l,ARG(0).i))return false;RETV();}
 if(!strcmp(name,"addUserRoute")){if(!mr_add(vm,self,ARG(0).l))return false;RETV();}
 if(!strcmp(name,"removeUserRoute")){if(!mr_remove(vm,self,ARG(0).l))return false;RETV();}
 if(!strcmp(name,"clearUserRoutes")){uint32_t n=0;dvm_ref *routes=mr_snapshot(vm,mr_list(vm,self,"routes"),&n);bool good=true;for(uint32_t i=0;good && i<n;++i)good=mr_remove(vm,self,routes[i]);mr_release(vm,routes,n);if(!good)return false;RETV();}
 if(!strcmp(name,"createUserRoute")){dvm_ref category=ARG(0).l;if(!category || mr_obj(vm,category,"router",MR_ROUTER)!=self){dvm__throw(vm,"java/lang/IllegalArgumentException","category");return false;}dvm_ref route=mr_new_route(vm,self,category,"android/media/MediaRouter$UserRouteInfo");if(!route)return false;rt_set_int(vm,route,"types",MR_USER);rt_set_int(vm,route,"playbackType",1);RETL(route);}
 if(!strcmp(name,"createRouteCategory")){dvm_ref category=dvm_new_object(vm,dvm_find_class(vm,"android/media/MediaRouter$RouteCategory"));if(!category)return false;mr_put(vm,category,"router",MR_ROUTER,self);rt_set_int(vm,category,"types",MR_USER);rt_set_int(vm,category,"groupable",ARG(1).i!=0);if(builtin_sig()[1]=='I')rt_set_int(vm,category,"nameRes",ARG(0).i);else mr_put(vm,category,"name","Ljava/lang/CharSequence;",ARG(0).l);RETL(category);}
 dvm_ref callback=!strcmp(name,"removeCallback") ? ARG(0).l : ARG(1).l;
 if(!callback){dvm__throw(vm,"java/lang/NullPointerException","callback");return false;}
 dvm_ref list=mr_list(vm,self,"callbacks"),record=0;struct rt_list *l=list_of(vm,list);
 for(uint32_t i=0;l && i<l->size;++i)if(mr_obj(vm,l->items[i],"callback","Landroid/media/MediaRouter$Callback;")==callback){record=l->items[i];break;}
 if(!strcmp(name,"removeCallback")){if(record)mr_delete(vm,list,record);RETV();}
 if(!record){record=dvm_new_object(vm,dvm_find_class(vm,"lunaria/MediaRouteCallback"));if(!record)return false;mr_put(vm,record,"callback","Landroid/media/MediaRouter$Callback;",callback);if(!mr_append(vm,list,record))return false;}
 rt_set_int(vm,record,"types",rt_get_int(vm,record,"types")|ARG(0).i);rt_set_int(vm,record,"flags",rt_get_int(vm,record,"flags")|(nargs>2 ? ARG(2).i : 0));RETV();
}
static bool mr_name(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 int res=rt_get_int(vm,self,"nameRes");if(!res)RETL(mr_obj(vm,self,"name","Ljava/lang/CharSequence;"));
 union dvm_value resources={0},id={.i=res};if(nargs && ARG(0).l){if(!am_vcall(vm,ARG(0).l,"getResources","()Landroid/content/res/Resources;",NULL,0,&resources))return false;}else resources.l=mr_obj(vm,mr_obj(vm,self,"router",MR_ROUTER),"resources","Landroid/content/res/Resources;");
 return am_vcall(vm,resources.l,"getText","(I)Ljava/lang/CharSequence;",&id,1,out);
}
static bool mr_category(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const char *name=builtin_name();if(!strcmp(name,"getName"))return mr_name(vm,self,args,nargs,out);
 if(!strcmp(name,"isGroupable"))RETI(rt_get_int(vm,self,"groupable"));if(!strcmp(name,"isSystem"))RETI(rt_get_int(vm,self,"system"));if(!strcmp(name,"getSupportedTypes"))RETI(rt_get_int(vm,self,"types"));
 dvm_ref result=ARG(0).l ? ARG(0).l : coll_new_list(vm,"Ljava/util/ArrayList;");if(!result)return false;union dvm_value ignored={0};if(!am_vcall(vm,result,"clear","()V",NULL,0,&ignored))return false;
 uint32_t n=0;dvm_ref *routes=mr_snapshot(vm,mr_list(vm,mr_obj(vm,self,"router",MR_ROUTER),"routes"),&n);bool good=!dvm_exception(vm);
 for(uint32_t i=0;good && i<n;++i)if(mr_obj(vm,routes[i],"category",MR_CATEGORY)==self){union dvm_value a={.l=routes[i]};good=am_vcall(vm,result,"add","(Ljava/lang/Object;)Z",&a,1,&ignored);}
 mr_release(vm,routes,n);if(!good)return false;RETL(result);
}
static bool mr_group_name(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 uint32_t n=0;dvm_ref *members=mr_snapshot(vm,mr_list(vm,self,"members"),&n);char *text=NULL;size_t length=0;bool good=!dvm_exception(vm);
 for(uint32_t i=0;good && i<n;++i){union dvm_value value={0};good=mr_name(vm,members[i],args,nargs,&value);if(!good)break;
  const char *part=dvm_string_utf8(vm,value.l);
  if(!part && value.l){union dvm_value string={0};good=am_vcall(vm,value.l,"toString","()Ljava/lang/String;",NULL,0,&string);if(!good)break;part=dvm_string_utf8(vm,string.l);}
  if(!part)part="";size_t size=strlen(part),separator=i ? 2 : 0;char *next=realloc(text,length+separator+size+1);
  if(!next){dvm__throw(vm,"java/lang/OutOfMemoryError","route group name");good=false;break;}text=next;if(separator){text[length++]=',';text[length++]=' ';}memcpy(text+length,part,size+1);length+=size;
 }
 mr_release(vm,members,n);dvm_ref result=good ? dvm_new_string(vm,text ? text : "") : 0;free(text);if(!good || !result)return false;RETL(result);
}
static bool mr_group_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const char *name=builtin_name();dvm_ref list=mr_list(vm,self,"members"),router=mr_obj(vm,self,"router",MR_ROUTER);struct rt_list *l=list_of(vm,list);
 if(!strcmp(name,"getRouteCount"))RETI(l ? l->size : 0);
 if(!strcmp(name,"getRouteAt")){if(!l || ARG(0).i<0 || (uint32_t)ARG(0).i>=l->size){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","route group index");return false;}RETL(l->items[ARG(0).i]);}
 if(!strcmp(name,"addRoute")){if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","route");return false;}if(!mr_group_add(vm,self,ARG(0).l,nargs>1 ? ARG(1).i : (int)(l ? l->size : 0)))return false;RETV();}
 if(!strcmp(name,"removeRoute")){dvm_ref route=ARG(0).l;if(builtin_sig()[1]=='I'){if(!l || ARG(0).i<0 || (uint32_t)ARG(0).i>=l->size){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","route group index");return false;}route=l->items[ARG(0).i];}
  if(mr_obj(vm,route,"group","Landroid/media/MediaRouter$RouteGroup;")!=self){dvm__throw(vm,"java/lang/IllegalArgumentException","Route not in group");return false;}if(!mr_remove(vm,router,route))return false;RETV();}
 /* Group requests use each member's volume range; no remote callback is
  * invoked as if it belonged to the group itself. */
 uint32_t n=0;dvm_ref *members=mr_snapshot(vm,list,&n);bool good=!dvm_exception(vm);int max=rt_get_int(vm,self,"volumeMax");
 for(uint32_t i=0;good && i<n;++i){union dvm_value value=ARG(0),ignored={0};if(!strcmp(name,"requestSetVolume")){int member_max=rt_get_int(vm,members[i],"playbackType")==0 ? 15 : rt_get_int(vm,members[i],"volumeMax");value.i=max>0 ? (int)lround((double)ARG(0).i*member_max/max) : 0;}
  good=am_vcall(vm,members[i],name,"(I)V",&value,1,&ignored);
 }
 mr_release(vm,members,n);if(!good)return false;RETV();
}
static bool mr_route(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const char *name=builtin_name();dvm_ref router=mr_obj(vm,self,"router",MR_ROUTER);
 if(!strcmp(name,"getName"))return mr_name(vm,self,args,nargs,out);
 if(!strcmp(name,"isSelected"))RETI(self==mr_obj(vm,router,"selectedRoute",MR_ROUTE));
 if(!strcmp(name,"isDefault"))RETI(self==mr_obj(vm,router,"defaultRoute",MR_ROUTE));
 if(!strcmp(name,"select")){if(!mr_select(vm,router,self,MR_ANY))return false;RETV();}
 if(!strcmp(name,"matchesTypes"))RETI((rt_get_int(vm,self,"types")&ARG(0).i)!=0);
 if(!strcmp(name,"getVolume") || !strcmp(name,"getVolumeMax")){if(rt_get_int(vm,self,"playbackType")==0)RETI(!strcmp(name,"getVolume") ? (int)lroundf(luna_os_audio_volume()*15) : 15);RETI(rt_get_int(vm,self,!strcmp(name,"getVolume") ? "volume" : "volumeMax"));}
 if(!strcmp(name,"requestSetVolume") || !strcmp(name,"requestUpdateVolume")){
  bool set=!strcmp(name,"requestSetVolume");if(self==mr_obj(vm,router,"defaultRoute",MR_ROUTE)){int old=(int)lroundf(luna_os_audio_volume()*15),v=set ? ARG(0).i : (int32_t)((uint32_t)old+(uint32_t)ARG(0).i);if(v<0)v=0;if(v>15)v=15;luna_os_audio_set_volume(v/15.f);if(old!=v && !mr_notify(vm,router,self,"onRouteVolumeChanged",0,0,0))return false;RETV();}
  dvm_ref cb=mr_obj(vm,self,"volumeCallback","Landroid/media/MediaRouter$VolumeCallback;");if(rt_get_int(vm,self,"volumeHandling")==1 && cb){union dvm_value a[]={{.l=self},ARG(0)};if(!mr_call(vm,cb,set ? "onVolumeSetRequest" : "onVolumeUpdateRequest","(Landroid/media/MediaRouter$RouteInfo;I)V",a,2))return false;}RETV();
 }
 struct {const char *method,*field,*type;} const properties[]={
 {"Description","description","Ljava/lang/CharSequence;"},{"Status","status","Ljava/lang/CharSequence;"},{"SupportedTypes","types","I"},{"PlaybackType","playbackType","I"},{"PlaybackStream","stream","I"},{"VolumeHandling","volumeHandling","I"},{"VolumeMax","volumeMax","I"},{"Volume","volume","I"},{"DeviceType","deviceType","I"},{"StatusCode","statusCode","I"},{"Category","category",MR_CATEGORY},{"Group","group","Landroid/media/MediaRouter$RouteGroup;"},{"Tag","tag","Ljava/lang/Object;"},{"IconDrawable","icon","Landroid/graphics/drawable/Drawable;"},{"VolumeCallback","volumeCallback","Landroid/media/MediaRouter$VolumeCallback;"},{"RemoteControlClient","remoteControlClient","Landroid/media/RemoteControlClient;"}};
 if(!strcmp(name,"isEnabled"))RETI(rt_get_int(vm,self,"enabled"));
 if(!strcmp(name,"isConnecting") || !strcmp(name,"isBluetooth"))RETI(0);
 if(!strcmp(name,"getPresentationDisplay") || !strcmp(name,"getDeviceAddress"))RETL(0);
 if(!strcmp(name,"setIconResource")){union dvm_value icon={0};dvm_ref resources=mr_obj(vm,router,"resources","Landroid/content/res/Resources;");if(!am_vcall(vm,resources,"getDrawable","(I)Landroid/graphics/drawable/Drawable;",args,1,&icon))return false;mr_put(vm,self,"icon","Landroid/graphics/drawable/Drawable;",icon.l);RETV();}
 if(!strcmp(name,"setName")){if(builtin_sig()[1]=='I'){rt_set_int(vm,self,"nameRes",ARG(0).i);mr_put(vm,self,"name","Ljava/lang/CharSequence;",0);}else{rt_set_int(vm,self,"nameRes",0);mr_put(vm,self,"name","Ljava/lang/CharSequence;",ARG(0).l);}if(!mr_notify(vm,router,self,"onRouteChanged",0,0,0))return false;RETV();}
 bool set=name[0]=='s';for(unsigned i=0;i<sizeof properties/sizeof *properties;++i)if(!strcmp(name+3,properties[i].method)){
  union dvm_value v={0};if(set){v=ARG(0);if(!strcmp(properties[i].field,"volume")){if(v.i<0)v.i=0;int max=rt_get_int(vm,self,"volumeMax");if(v.i>max)v.i=max;}int old=rt_get_int(vm,self,"types");dvm_set_field(vm,self,properties[i].field,properties[i].type,v);
   if(!strcmp(properties[i].field,"types") && old!=v.i){if(!mr_changed(vm,router,self,old))return false;}
   else if(strcmp(properties[i].field,"tag") && strcmp(properties[i].field,"volumeCallback") && strcmp(properties[i].field,"remoteControlClient") && !mr_notify(vm,router,self,!strcmp(properties[i].field,"volume") ? "onRouteVolumeChanged" : "onRouteChanged",0,0,0))return false;
   dvm_ref group=mr_obj(vm,self,"group","Landroid/media/MediaRouter$RouteGroup;");if(group && group!=self && !mr_group_refresh(vm,group))return false;RETV();}
  dvm_get_field(vm,self,properties[i].field,properties[i].type,&v);if(properties[i].type[0]=='I')RETI(v.i);RETL(v.l);
 }
 dvm__throw(vm,"java/lang/UnsupportedOperationException","MediaRouter route method %s",name);return false;
}
static const struct rt_field rt_mr_fields[]={{"routes","Ljava/util/List;"},{"categories","Ljava/util/List;"},{"callbacks","Ljava/util/List;"},{"defaultRoute",MR_ROUTE},{"selectedRoute",MR_ROUTE},{"resources","Landroid/content/res/Resources;"},F_END};
static const struct rt_field rt_mr_category_fields[]={{"router",MR_ROUTER},{"name","Ljava/lang/CharSequence;"},{"nameRes","I"},{"types","I"},{"groupable","I"},{"system","I"},F_END};
static const struct rt_field rt_mr_route_fields[]={{"router",MR_ROUTER},{"category",MR_CATEGORY},{"group","Landroid/media/MediaRouter$RouteGroup;"},{"members","Ljava/util/List;"},{"name","Ljava/lang/CharSequence;"},{"nameRes","I"},{"description","Ljava/lang/CharSequence;"},{"status","Ljava/lang/CharSequence;"},{"types","I"},{"playbackType","I"},{"stream","I"},{"volume","I"},{"volumeMax","I"},{"volumeHandling","I"},{"enabled","I"},{"deviceType","I"},{"statusCode","I"},{"tag","Ljava/lang/Object;"},{"icon","Landroid/graphics/drawable/Drawable;"},{"volumeCallback","Landroid/media/MediaRouter$VolumeCallback;"},{"remoteControlClient","Landroid/media/RemoteControlClient;"},F_END};
static const struct rt_field rt_mr_callback_fields[]={{"callback","Landroid/media/MediaRouter$Callback;"},{"types","I"},{"flags","I"},F_END};
#define MM(n,s) M(n,s,mr_manager)
static const struct rt_method rt_media_router[]={
 MM("getDefaultRoute","()" MR_ROUTE),MM("getSelectedRoute","()" MR_ROUTE),MM("getSelectedRoute","(I)" MR_ROUTE),MM("getRouteCount","()I"),MM("getRouteAt","(I)" MR_ROUTE),MM("getCategoryCount","()I"),MM("getCategoryAt","(I)" MR_CATEGORY),MM("isRouteAvailable","(II)Z"),MM("selectRoute","(I" MR_ROUTE ")V"),MM("selectRouteInt","(I" MR_ROUTE "Z)V"),MM("createRouteCategory","(Ljava/lang/CharSequence;Z)" MR_CATEGORY),MM("createRouteCategory","(IZ)" MR_CATEGORY),MM("createUserRoute","(" MR_CATEGORY ")Landroid/media/MediaRouter$UserRouteInfo;"),MM("addUserRoute","(Landroid/media/MediaRouter$UserRouteInfo;)V"),MM("removeUserRoute","(Landroid/media/MediaRouter$UserRouteInfo;)V"),MM("clearUserRoutes","()V"),MM("addCallback","(ILandroid/media/MediaRouter$Callback;)V"),MM("addCallback","(ILandroid/media/MediaRouter$Callback;I)V"),MM("removeCallback","(Landroid/media/MediaRouter$Callback;)V"),M_END};
#undef MM
#define MC(n,s) M(n,s,mr_category)
static const struct rt_method rt_mr_category[]={M("getName","()Ljava/lang/CharSequence;",mr_name),M("getName","(Landroid/content/Context;)Ljava/lang/CharSequence;",mr_name),MC("getRoutes","(Ljava/util/List;)Ljava/util/List;"),MC("getSupportedTypes","()I"),MC("isGroupable","()Z"),MC("isSystem","()Z"),M_END};
#undef MC
#define RI(n,s) M(n,s,mr_route)
static const struct rt_method rt_mr_route[]={
 RI("getName","()Ljava/lang/CharSequence;"),RI("getName","(Landroid/content/Context;)Ljava/lang/CharSequence;"),RI("getDescription","()Ljava/lang/CharSequence;"),RI("getStatus","()Ljava/lang/CharSequence;"),RI("getStatusCode","()I"),RI("getSupportedTypes","()I"),RI("getDeviceType","()I"),RI("matchesTypes","(I)Z"),RI("getCategory","()" MR_CATEGORY),RI("getGroup","()Landroid/media/MediaRouter$RouteGroup;"),RI("getTag","()Ljava/lang/Object;"),RI("setTag","(Ljava/lang/Object;)V"),RI("getIconDrawable","()Landroid/graphics/drawable/Drawable;"),RI("getPlaybackType","()I"),RI("getPlaybackStream","()I"),RI("getVolume","()I"),RI("getVolumeMax","()I"),RI("getVolumeHandling","()I"),RI("requestSetVolume","(I)V"),RI("requestUpdateVolume","(I)V"),RI("getPresentationDisplay","()Landroid/view/Display;"),RI("getDeviceAddress","()Ljava/lang/String;"),RI("isEnabled","()Z"),RI("isConnecting","()Z"),RI("isSelected","()Z"),RI("isDefault","()Z"),RI("isBluetooth","()Z"),RI("select","()V"),M_END};
static const struct rt_method rt_mr_user_route[]={RI("setName","(Ljava/lang/CharSequence;)V"),RI("setName","(I)V"),RI("setDescription","(Ljava/lang/CharSequence;)V"),RI("setStatus","(Ljava/lang/CharSequence;)V"),RI("setPlaybackType","(I)V"),RI("setPlaybackStream","(I)V"),RI("setVolume","(I)V"),RI("setVolumeMax","(I)V"),RI("setVolumeHandling","(I)V"),RI("setVolumeCallback","(Landroid/media/MediaRouter$VolumeCallback;)V"),RI("setIconDrawable","(Landroid/graphics/drawable/Drawable;)V"),RI("setIconResource","(I)V"),RI("setRemoteControlClient","(Landroid/media/RemoteControlClient;)V"),RI("getRemoteControlClient","()Landroid/media/RemoteControlClient;"),M_END};
#undef RI
#define MG(n,s) M(n,s,mr_group_method)
static const struct rt_method rt_mr_group[]={M("getName","()Ljava/lang/CharSequence;",mr_group_name),M("getName","(Landroid/content/Context;)Ljava/lang/CharSequence;",mr_group_name),MG("addRoute","(" MR_ROUTE ")V"),MG("addRoute","(" MR_ROUTE "I)V"),MG("removeRoute","(" MR_ROUTE ")V"),MG("removeRoute","(I)V"),MG("getRouteCount","()I"),MG("getRouteAt","(I)" MR_ROUTE),MG("requestSetVolume","(I)V"),MG("requestUpdateVolume","(I)V"),M_END};
#undef MG
#define MR_CALLBACK_METHODS(F) \
 F("onRouteSelected","(Landroid/media/MediaRouter;ILandroid/media/MediaRouter$RouteInfo;)V") \
 F("onRouteUnselected","(Landroid/media/MediaRouter;ILandroid/media/MediaRouter$RouteInfo;)V") \
 F("onRouteAdded","(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;)V") \
 F("onRouteRemoved","(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;)V") \
 F("onRouteChanged","(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;)V") \
 F("onRouteGrouped","(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;Landroid/media/MediaRouter$RouteGroup;I)V") \
 F("onRouteUngrouped","(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;Landroid/media/MediaRouter$RouteGroup;)V") \
 F("onRouteVolumeChanged","(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;)V")
#define MR_ABSTRACT(n,s) {n,s,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
static const struct rt_method rt_mr_callback[]={MR_CALLBACK_METHODS(MR_ABSTRACT) M("onRoutePresentationDisplayChanged","(Landroid/media/MediaRouter;Landroid/media/MediaRouter$RouteInfo;)V",empty_void),M("<init>","()V",empty_void),M_END};
static const struct rt_method rt_mr_volume_callback[]={MR_ABSTRACT("onVolumeSetRequest","(Landroid/media/MediaRouter$RouteInfo;I)V") MR_ABSTRACT("onVolumeUpdateRequest","(Landroid/media/MediaRouter$RouteInfo;I)V") M("<init>","()V",empty_void),M_END};
#undef MR_ABSTRACT
#define MR_EMPTY(n,s) M(n,s,empty_void),
static const struct rt_method rt_mr_simple_callback[]={MR_CALLBACK_METHODS(MR_EMPTY) M("<init>","()V",empty_void),M_END};
#undef MR_EMPTY
#undef MR_CALLBACK_METHODS

#endif /* LUNARIA_DVM_MEDIA_ROUTER_IMPLEMENTATION */
