/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See dvm_media.h.
 */

#include "dvm/dvm_media.h"
#include "lunaria_os.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ *
 * openh264, through its C binding
 *
 * codec_api.h offers the vtable form for callers that are not C++; that is
 * what we declare here, so the emulator needs neither the headers nor a link
 * against the library.  The field order is the one openh264 has published
 * since 1.8 and the layout is checked against the decoder's own answers before
 * anything is read out of it (see decode_au).
 * ------------------------------------------------------------------------ */

typedef const struct ISVCDecoderVtbl *ISVCDecoder;

struct SSysMEMBuffer {
   int iWidth, iHeight, iFormat, iStride[2];
};

struct SBufferInfo {
   int iBufferStatus;
   unsigned long long uiInBsTimeStamp;
   unsigned long long uiOutYuvTimeStamp;
   union { struct SSysMEMBuffer sSystemBuffer; } UsrData;
   unsigned char *pDst[3];
};

struct SVideoProperty {
   unsigned int size;
   int eVideoBsType;
};

struct SDecodingParam {
   char *pFileNameRestructed;
   unsigned int uiCpuLoad;
   unsigned char uiTargetDqLayer;
   int eEcActiveIdc;
   bool bParseOnly;
   struct SVideoProperty sVideoProperty;
};

struct ISVCDecoderVtbl {
   long (*Initialize)(ISVCDecoder *, const struct SDecodingParam *);
   long (*Uninitialize)(ISVCDecoder *);
   int (*DecodeFrame)(ISVCDecoder *, const unsigned char *, int,
                      unsigned char **, int *, int *, int *);
   int (*DecodeFrameNoDelay)(ISVCDecoder *, const unsigned char *, int,
                             unsigned char **, struct SBufferInfo *);
   int (*DecodeFrame2)(ISVCDecoder *, const unsigned char *, int,
                       unsigned char **, struct SBufferInfo *);
   int (*FlushFrame)(ISVCDecoder *, unsigned char **, struct SBufferInfo *);
   int (*DecodeParser)(ISVCDecoder *, const unsigned char *, int, void *);
   int (*DecodeFrameEx)(ISVCDecoder *, const unsigned char *, int,
                        unsigned char *, int, int *, int *, int *, int *);
   long (*SetOption)(ISVCDecoder *, int, void *);
   long (*GetOption)(ISVCDecoder *, int, void *);
};

/* ERROR_CON_IDC: conceal a damaged slice from the frame before it rather than
 * dropping the picture — a stream that lost a packet keeps playing. */
#define LM_ERROR_CON_SLICE_COPY 2

static int (*lm_WelsCreateDecoder)(ISVCDecoder **);
static void (*lm_WelsDestroyDecoder)(ISVCDecoder *);

/* Where the openh264 binary can be: an explicit override, next to the running
 * lunaria (`make fetch-openh264` puts it in runtime/), the install directory,
 * or the system loader's own search. */
static void *openh264_dlopen(void)
{
   const char *env = getenv("LUNARIA_OPENH264");
   if (env && *env) return dlopen(env, RTLD_NOW | RTLD_LOCAL);

   char self[PATH_MAX];
   if (luna_os_executable_path(self, sizeof self) == 0) {
      char *slash = strrchr(self, '/');
      if (slash) {
         char path[PATH_MAX];
         *slash = '\0';
         if ((size_t)snprintf(path, sizeof path, "%s/runtime/%s",
                              self,
#ifdef __APPLE__
                              "libopenh264.dylib"
#else
                              "libopenh264.so"
#endif
                              ) < sizeof path) {
            void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (h) return h;
         }
      }
   }

   static const char *const fallbacks[] = {
#ifdef __APPLE__
      "/usr/local/lib/lunaria/libopenh264.dylib",
      "libopenh264.dylib",
#else
      "/usr/local/lib/lunaria/libopenh264.so",
      "libopenh264.so",
      "libopenh264.so.7",
#endif
   };
   for (size_t i = 0; i < sizeof fallbacks / sizeof *fallbacks; ++i) {
      void *h = dlopen(fallbacks[i], RTLD_NOW | RTLD_LOCAL);
      if (h) return h;
   }
   return NULL;
}

/* True once the library is loaded and its two entry points resolved.  Reported
 * exactly once, so a run without it says so instead of going quiet. */
static bool openh264_ready(void)
{
   static int state = -1;
   if (state >= 0) return state == 1;

   void *h = openh264_dlopen();
   if (!h) {
      fprintf(stderr, "[media] no openh264: %s\n"
              "[media] H.264 decoding is unavailable — run `make fetch-openh264`"
              " or set LUNARIA_OPENH264\n", dlerror());
      state = 0;
      return false;
   }
   *(void **)&lm_WelsCreateDecoder = dlsym(h, "WelsCreateDecoder");
   *(void **)&lm_WelsDestroyDecoder = dlsym(h, "WelsDestroyDecoder");
   if (!lm_WelsCreateDecoder || !lm_WelsDestroyDecoder) {
      fprintf(stderr, "[media] openh264 loaded but WelsCreateDecoder is missing\n");
      state = 0;
      return false;
   }
   unsigned (*ver)(void) = NULL;
   *(void **)&ver = dlsym(h, "WelsGetCodecVersion");
   fprintf(stderr, "[media] openh264 ready%s\n", ver ? "" : " (version unknown)");
   state = 1;
   return true;
}

/* ------------------------------------------------------------------------ *
 * AAC, through libavcodec
 *
 * openh264 is a video codec; the AAC half of a stream needs its own decoder.
 * libavcodec's is native code under the LGPL, so it is opened with dlopen()
 * the same way openh264 is — no link-time dependency, and an emulator without
 * it reports that it has no AAC decoder instead of inventing silence.
 *
 * Only the entry points are declared here, not the structures: AVPacket and
 * AVFrame are ABI-versioned, so the two field offsets that are actually needed
 * are checked against what the library itself writes before anything is read
 * through them (avcodec_probe_layout / decode_aac).
 * ------------------------------------------------------------------------ */

/* AVPacket (libavcodec 55): buf, pts, dts, data, size, … */
#define AVPKT_PTS_OFF   8
#define AVPKT_DTS_OFF   16
#define AVPKT_DATA_OFF  24
#define AVPKT_SIZE_OFF  32
#define AVPKT_BYTES     256      /* comfortably larger than the real struct */

/* AVFrame (libavutil 52): data[8], linesize[8], extended_data, w, h, … */
#define AVFRAME_DATA_OFF        0
#define AVFRAME_NB_SAMPLES_OFF  112
#define AVFRAME_FORMAT_OFF      116

enum { AV_SAMPLE_FMT_S16 = 1, AV_SAMPLE_FMT_FLT = 3,
       AV_SAMPLE_FMT_S16P = 6, AV_SAMPLE_FMT_FLTP = 8 };

static struct {
   void (*register_all)(void);
   void *(*find_decoder_by_name)(const char *);
   void *(*alloc_context3)(void *);
   int (*open2)(void *, void *, void **);
   int (*decode_audio4)(void *, void *, int *, const void *);
   int (*close)(void *);
   void (*init_packet)(void *);
   void *(*frame_alloc)(void);
   void (*frame_free)(void **);
   int (*frame_channels)(const void *);
   int (*frame_sample_rate)(const void *);
} av;

static void *dlopen_any(const char *const *names, size_t n)
{
   for (size_t i = 0; i < n; ++i) {
      void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
      if (h) return h;
   }
   return NULL;
}

/* av_init_packet() writes AV_NOPTS_VALUE into pts and dts and clears buf.
 * Seeing all three land at the offsets above is what says this libavcodec has
 * the layout assumed here; anything else and the decoder stays unavailable
 * rather than reading a struct it does not understand.  (data/size are left
 * alone by av_init_packet — the caller is expected to set them, which is what
 * decode_aac does.) */
static bool avcodec_probe_layout(void)
{
   unsigned char pkt[AVPKT_BYTES];
   memset(pkt, 0xA5, sizeof pkt);
   av.init_packet(pkt);
   int64_t pts, dts;
   void *buf;
   memcpy(&buf, pkt, sizeof buf);
   memcpy(&pts, pkt + AVPKT_PTS_OFF, 8);
   memcpy(&dts, pkt + AVPKT_DTS_OFF, 8);
   return buf == NULL && pts == INT64_MIN && dts == INT64_MIN;
}

static bool avcodec_ready(void)
{
   static int state = -1;
   if (state >= 0) return state == 1;
   state = 0;

   static const char *const codec_names[] = {
      "libavcodec.so.55", "libavcodec.so",
   };
   static const char *const util_names[] = {
      "libavutil.so.52", "libavutil.so",
   };
   const char *env = getenv("LUNARIA_LIBAVCODEC");
   void *c = env && *env ? dlopen(env, RTLD_NOW | RTLD_LOCAL)
                         : dlopen_any(codec_names, 2);
   void *u = dlopen_any(util_names, 2);
   if (!c || !u) {
      fprintf(stderr, "[media] no libavcodec/libavutil: %s\n"
              "[media] AAC decoding is unavailable\n", dlerror());
      return false;
   }
#define AV_SYM(dst, lib, name) \
   do { *(void **)&av.dst = dlsym(lib, name); if (!av.dst) { \
        fprintf(stderr, "[media] libav: %s missing\n", name); return false; } } while (0)
   AV_SYM(register_all, c, "avcodec_register_all");
   AV_SYM(find_decoder_by_name, c, "avcodec_find_decoder_by_name");
   AV_SYM(alloc_context3, c, "avcodec_alloc_context3");
   AV_SYM(open2, c, "avcodec_open2");
   AV_SYM(decode_audio4, c, "avcodec_decode_audio4");
   AV_SYM(close, c, "avcodec_close");
   AV_SYM(init_packet, c, "av_init_packet");
   AV_SYM(frame_alloc, u, "av_frame_alloc");
   AV_SYM(frame_free, u, "av_frame_free");
   AV_SYM(frame_channels, u, "av_frame_get_channels");
   AV_SYM(frame_sample_rate, u, "av_frame_get_sample_rate");
#undef AV_SYM

   if (!avcodec_probe_layout()) {
      fprintf(stderr, "[media] libavcodec AVPacket layout is not the one this "
              "build understands — AAC decoding disabled\n");
      return false;
   }
   av.register_all();
   if (!av.find_decoder_by_name("aac")) {
      fprintf(stderr, "[media] libavcodec has no \"aac\" decoder\n");
      return false;
   }
   fprintf(stderr, "[media] libavcodec AAC ready\n");
   state = 1;
   return true;
}

/* ------------------------------------------------------------------------ *
 * Frame sink
 * ------------------------------------------------------------------------ */

struct lm_sink {
   uint8_t *rgba;
   size_t cap;
   int w, h;
   int64_t ts_ns;
   bool pending;
};

struct lm_sink *lm_sink_new(void)
{
   return calloc(1, sizeof(struct lm_sink));
}

void lm_sink_free(struct lm_sink *s)
{
   if (!s) return;
   free(s->rgba);
   free(s);
}

uint8_t *lm_sink_begin(struct lm_sink *s, int w, int h)
{
   if (!s || w <= 0 || h <= 0) return NULL;
   size_t need = (size_t)w * (size_t)h * 4u;
   if (need > s->cap) {
      uint8_t *p = realloc(s->rgba, need);
      if (!p) return NULL;
      s->rgba = p;
      s->cap = need;
   }
   return s->rgba;
}

void lm_sink_commit(struct lm_sink *s, int w, int h, int64_t timestamp_ns)
{
   if (!s || !s->rgba || w <= 0 || h <= 0) return;
   s->w = w;
   s->h = h;
   s->ts_ns = timestamp_ns;
   s->pending = true;
}

void lm_sink_push(struct lm_sink *s, const uint8_t *rgba, int w, int h,
                  int64_t timestamp_ns)
{
   uint8_t *dst;
   if (!rgba) return;
   dst = lm_sink_begin(s, w, h);
   if (!dst) return;
   memcpy(dst, rgba, (size_t)w * (size_t)h * 4u);
   lm_sink_commit(s, w, h, timestamp_ns);
}

bool lm_sink_take(struct lm_sink *s, const uint8_t **rgba, int *w, int *h)
{
   if (!s || !s->pending || !s->rgba) return false;
   s->pending = false;
   if (rgba) *rgba = s->rgba;
   if (w) *w = s->w;
   if (h) *h = s->h;
   return true;
}

bool lm_sink_peek(const struct lm_sink *s, const uint8_t **rgba, int *w, int *h)
{
   if (!s || !s->rgba || s->w <= 0 || s->h <= 0) return false;
   if (rgba) *rgba = s->rgba;
   if (w) *w = s->w;
   if (h) *h = s->h;
   return true;
}

int64_t lm_sink_timestamp(const struct lm_sink *s)
{
   return s ? s->ts_ns : 0;
}

/* ------------------------------------------------------------------------ *
 * Codec
 * ------------------------------------------------------------------------ */

#define LM_IN_BUFS  6
#define LM_OUT_BUFS 6
#define LM_IN_CAP   (1u << 20)   /* one access unit; grown on demand */
#define LM_MAX_CSD  4

struct lm_inbuf {
   uint8_t *p;
   size_t cap;
   bool queued;
};

struct lm_outbuf {
   uint8_t *yuv;            /* I420, planes packed w*h + 2*(w/2 * h/2) */
   size_t cap, len;
   int w, h;
   int64_t pts_us;
   int flags;
   bool busy;               /* handed to the caller, not yet released */
   bool ready;              /* holds a decoded picture */
};

struct lm_codec {
   bool video;
   bool started;
   ISVCDecoder *dec;

   /* audio */
   void *actx, *aframe;
   int rate, channels;
   int aac_profile, aac_freq_idx, aac_chan_cfg;   /* from csd-0 */
   bool aac_config;

   struct lm_inbuf in[LM_IN_BUFS];
   struct lm_outbuf out[LM_OUT_BUFS];

   struct { uint8_t *p; size_t len; } csd[LM_MAX_CSD];
   int ncsd;

   int width, height;
   bool announced_format;   /* the one INFO_OUTPUT_FORMAT_CHANGED was handed out */
   bool got_eos;            /* an input buffer carried END_OF_STREAM */
   bool sent_eos;

   /* A decoder that cannot decode this stream at all.
    *
    * openh264 decodes Constrained Baseline; a title's startup movie is
    * routinely Main or High with CABAC and B-frames, and the avcC's
    * profile_compatibility byte cannot be trusted to say so (Blade & Soul's
    * splash claims constraint_set1 and still carries B-frames).  There is no
    * honest static test, so the test is the decode itself: every access unit
    * comes back a bitstream error and no picture is ever produced.
    *
    * Reporting that matters more than it looks.  The old behaviour was to log
    * each error and carry on, so the player kept "playing" a movie that would
    * never show a frame — and an engine waiting on that movie waits for ever.
    * That is the whole of Blade & Soul Revolution's boot: a white screen at a
    * steady 21 fps, with nothing wrong anywhere the log was looking. */
   int  dec_errors;         /* consecutive errors with nothing decoded yet */
   bool produced_picture;   /* …until the first picture comes out */
   bool undecodable;        /* give up: this decoder cannot read this stream */
};

/* How many access units to let fail before calling it.  A stream that starts
 * on a non-IDR frame legitimately errors until the first keyframe, so this has
 * to be past any plausible run of those, and still well inside the fraction of
 * a second an engine spends waiting before it notices a stall. */
#define LM_DECODE_GIVE_UP 16

static bool is_aac_mime(const char *mime)
{
   return !strcasecmp(mime, "audio/mp4a-latm") || !strcasecmp(mime, "audio/aac");
}

bool lm_codec_supported(const char *mime)
{
   if (!mime) return false;
   if (!strcasecmp(mime, "video/avc")) return openh264_ready();
   if (is_aac_mime(mime)) return avcodec_ready();
   return false;
}

static struct lm_codec *aac_codec_new(void)
{
   struct lm_codec *c = calloc(1, sizeof *c);
   if (!c) return NULL;
   void *codec = av.find_decoder_by_name("aac");
   c->actx = codec ? av.alloc_context3(codec) : NULL;
   c->aframe = c->actx ? av.frame_alloc() : NULL;
   if (!c->aframe || av.open2(c->actx, codec, NULL) < 0) {
      fprintf(stderr, "[media] libavcodec could not open the AAC decoder\n");
      if (c->aframe) av.frame_free(&c->aframe);
      free(c);
      return NULL;
   }
   return c;
}

struct lm_codec *lm_codec_new(const char *mime)
{
   if (!lm_codec_supported(mime)) return NULL;
   if (is_aac_mime(mime)) return aac_codec_new();

   struct lm_codec *c = calloc(1, sizeof *c);
   if (!c) return NULL;
   c->video = true;

   if (lm_WelsCreateDecoder(&c->dec) != 0 || !c->dec) {
      fprintf(stderr, "[media] WelsCreateDecoder failed\n");
      free(c);
      return NULL;
   }
   struct SDecodingParam param;
   memset(&param, 0, sizeof param);
   param.eEcActiveIdc = LM_ERROR_CON_SLICE_COPY;
   param.sVideoProperty.size = sizeof param.sVideoProperty;
   if ((*c->dec)->Initialize(c->dec, &param) != 0) {
      fprintf(stderr, "[media] openh264 Initialize failed\n");
      lm_WelsDestroyDecoder(c->dec);
      free(c);
      return NULL;
   }
   return c;
}

void lm_codec_free(struct lm_codec *c)
{
   if (!c) return;
   if (c->dec) {
      (*c->dec)->Uninitialize(c->dec);
      lm_WelsDestroyDecoder(c->dec);
   }
   if (c->actx) av.close(c->actx);
   if (c->aframe) av.frame_free(&c->aframe);
   for (int i = 0; i < LM_IN_BUFS; ++i) free(c->in[i].p);
   for (int i = 0; i < LM_OUT_BUFS; ++i) free(c->out[i].yuv);
   for (int i = 0; i < c->ncsd; ++i) free(c->csd[i].p);
   free(c);
}

void lm_codec_add_csd(struct lm_codec *c, const uint8_t *data, size_t len)
{
   if (!c || !data || !len || c->ncsd >= LM_MAX_CSD) return;
   uint8_t *p = malloc(len);
   if (!p) return;
   memcpy(p, data, len);
   c->csd[c->ncsd].p = p;
   c->csd[c->ncsd].len = len;
   c->ncsd++;
}

/* LUNARIA_TRACE_MEDIA=1 logs the codec's own state machine — which buffers the
 * caller took, what came back — because a stalled player looks exactly like a
 * quiet one from outside. */
bool lm_media_trace(void)
{
   static int on = -1;
   if (on < 0) on = getenv("LUNARIA_TRACE_MEDIA") != NULL;
   return on == 1;
}
#define trace_media lm_media_trace

/* Feeds one access unit and keeps whatever picture comes out.  Returns false
 * only when the decoder itself refused the data. */
static bool decode_au(struct lm_codec *c, const uint8_t *data, size_t len,
                      int64_t pts_us)
{
   unsigned char *planes[3] = { NULL, NULL, NULL };
   struct SBufferInfo info;
   memset(&info, 0, sizeof info);
   info.uiInBsTimeStamp = (unsigned long long)pts_us;

   int st = (*c->dec)->DecodeFrameNoDelay(c->dec, data, (int)len, planes, &info);
   if (st != 0) {
      static int warned;
      if (warned++ < 8)
         fprintf(stderr, "[media] openh264 decode state 0x%x (%zu bytes)\n",
                 st, len);
      /* A concealed or dropped picture is not a codec failure; the next AU
       * usually recovers.  Only report it — unless nothing has ever decoded,
       * in which case this is not a dropped frame, it is a stream the decoder
       * cannot read.  See lm_codec::undecodable. */
      if (!c->produced_picture && !c->undecodable &&
          ++c->dec_errors >= LM_DECODE_GIVE_UP) {
         c->undecodable = true;
         fprintf(stderr,
                 "[media] this H.264 stream is not decodable here: %d access "
                 "units, no picture.  openh264 decodes Constrained Baseline, "
                 "and Main/High with CABAC or B-frames is not that — reporting "
                 "the stream unsupported so the caller stops waiting on it.\n",
                 c->dec_errors);
      }
   }
   if (info.iBufferStatus != 1) return true;   /* nothing came out this time */
   c->produced_picture = true;
   c->dec_errors = 0;

   const struct SSysMEMBuffer *b = &info.UsrData.sSystemBuffer;
   int w = b->iWidth, h = b->iHeight;
   if (w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
       b->iStride[0] < w || b->iStride[1] < w / 2 ||
       !planes[0] || !planes[1] || !planes[2]) {
      fprintf(stderr, "[media] openh264 handed back an unusable picture "
              "(%dx%d stride %d/%d) — refusing it\n",
              w, h, b->iStride[0], b->iStride[1]);
      return false;
   }

   struct lm_outbuf *o = NULL;
   for (int i = 0; i < LM_OUT_BUFS; ++i)
      if (!c->out[i].busy && !c->out[i].ready) { o = &c->out[i]; break; }
   if (!o) {
      /* dequeue_input() keeps this from happening; if it ever does, dropping
       * the newest frame is what a full BufferQueue does. */
      fprintf(stderr, "[media] output buffers full — dropping a frame\n");
      return true;
   }

   int cw = (w + 1) / 2, ch = (h + 1) / 2;
   size_t need = (size_t)w * (size_t)h + 2u * (size_t)cw * (size_t)ch;
   if (need > o->cap) {
      uint8_t *p = realloc(o->yuv, need);
      if (!p) return false;
      o->yuv = p;
      o->cap = need;
   }
   for (int y = 0; y < h; ++y)
      memcpy(o->yuv + (size_t)y * w, planes[0] + (size_t)y * b->iStride[0], (size_t)w);
   uint8_t *u = o->yuv + (size_t)w * h;
   uint8_t *v = u + (size_t)cw * ch;
   for (int y = 0; y < ch; ++y) {
      memcpy(u + (size_t)y * cw, planes[1] + (size_t)y * b->iStride[1], (size_t)cw);
      memcpy(v + (size_t)y * cw, planes[2] + (size_t)y * b->iStride[1], (size_t)cw);
   }
   if (c->width != w || c->height != h)
      fprintf(stderr, "[media] H.264 picture %dx%d\n", w, h);
   o->len = need;
   o->w = w;
   o->h = h;
   o->pts_us = (int64_t)info.uiOutYuvTimeStamp;
   o->flags = 0;
   o->ready = true;

   c->width = w;
   c->height = h;
   return true;
}

/* AAC frames arrive raw, with the configuration in csd-0 (an
 * AudioSpecificConfig).  libavcodec's decoder reads that configuration out of
 * an ADTS header, which the same three fields describe, so each frame is given
 * one instead of teaching the emulator where AVCodecContext keeps extradata. */
static void aac_take_config(struct lm_codec *c, const uint8_t *asc, size_t len)
{
   if (len < 2) return;
   c->aac_profile = (asc[0] >> 3) & 0x1f;              /* audioObjectType */
   c->aac_freq_idx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);
   c->aac_chan_cfg = (asc[1] >> 3) & 0x0f;
   c->aac_config = c->aac_profile >= 1 && c->aac_profile <= 4 &&
                   c->aac_freq_idx <= 12 && c->aac_chan_cfg >= 1;
   if (!c->aac_config)
      fprintf(stderr, "[media] AAC csd-0 is not a usable AudioSpecificConfig "
              "(objectType %d, freq idx %d, channels %d)\n",
              c->aac_profile, c->aac_freq_idx, c->aac_chan_cfg);
}

static void aac_write_adts(const struct lm_codec *c, uint8_t *h, size_t payload)
{
   size_t total = payload + 7;
   h[0] = 0xff;
   h[1] = 0xf1;                                   /* MPEG-4, no CRC */
   h[2] = (uint8_t)(((c->aac_profile - 1) << 6) | (c->aac_freq_idx << 2) |
                    ((c->aac_chan_cfg >> 2) & 1));
   h[3] = (uint8_t)(((c->aac_chan_cfg & 3) << 6) | ((total >> 11) & 0x03));
   h[4] = (uint8_t)((total >> 3) & 0xff);
   h[5] = (uint8_t)(((total & 7) << 5) | 0x1f);
   h[6] = 0xfc;
}

/* Interleaved signed 16-bit, which is what MediaCodec hands back by default
 * and what the caller asked for with pcm-encoding = ENCODING_PCM_16BIT. */
static size_t aac_pack_s16(int fmt, int channels, int nb, uint8_t *const *planes,
                           int16_t *dst)
{
   if (nb <= 0 || channels <= 0) return 0;
   switch (fmt) {
      case AV_SAMPLE_FMT_S16:
         memcpy(dst, planes[0], (size_t)nb * channels * 2);
         return (size_t)nb * channels * 2;
      case AV_SAMPLE_FMT_S16P:
         for (int i = 0; i < nb; ++i)
            for (int ch = 0; ch < channels; ++ch)
               dst[i * channels + ch] = ((const int16_t *)planes[ch])[i];
         return (size_t)nb * channels * 2;
      case AV_SAMPLE_FMT_FLT:
         for (int i = 0; i < nb * channels; ++i) {
            float v = ((const float *)planes[0])[i] * 32767.0f;
            dst[i] = (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
         }
         return (size_t)nb * channels * 2;
      case AV_SAMPLE_FMT_FLTP:
         for (int i = 0; i < nb; ++i)
            for (int ch = 0; ch < channels; ++ch) {
               float v = ((const float *)planes[ch])[i] * 32767.0f;
               dst[i * channels + ch] =
                  (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
            }
         return (size_t)nb * channels * 2;
      default:
         return 0;
   }
}

static bool decode_aac(struct lm_codec *c, const uint8_t *data, size_t len,
                       int64_t pts_us)
{
   if (!c->aac_config || !len) return true;

   /* One ADTS frame: header + payload, in a buffer libavcodec may read a
    * little past (it wants FF_INPUT_BUFFER_PADDING_SIZE of slack). */
   size_t total = len + 7;
   uint8_t *buf = calloc(total + 64, 1);
   if (!buf) return false;
   aac_write_adts(c, buf, len);
   memcpy(buf + 7, data, len);

   unsigned char pkt[AVPKT_BYTES];
   memset(pkt, 0, sizeof pkt);
   av.init_packet(pkt);
   uint8_t *pdata = buf;
   int32_t psize = (int32_t)total;
   memcpy(pkt + AVPKT_DATA_OFF, &pdata, sizeof pdata);
   memcpy(pkt + AVPKT_SIZE_OFF, &psize, 4);

   int got = 0;
   int used = av.decode_audio4(c->actx, c->aframe, &got, pkt);
   if (used < 0 || !got) {
      static int warned;
      if (used < 0 && warned++ < 8)
         fprintf(stderr, "[media] AAC decode failed (%d) on %zu bytes\n", used, len);
      free(buf);
      return true;
   }

   uint8_t *planes[8];
   int32_t nb, fmt;
   memcpy(planes, (const uint8_t *)c->aframe + AVFRAME_DATA_OFF, sizeof planes);
   memcpy(&nb, (const uint8_t *)c->aframe + AVFRAME_NB_SAMPLES_OFF, 4);
   memcpy(&fmt, (const uint8_t *)c->aframe + AVFRAME_FORMAT_OFF, 4);
   int ch = av.frame_channels(c->aframe);
   int rate = av.frame_sample_rate(c->aframe);
   if (nb <= 0 || nb > 65536 || ch <= 0 || ch > 8 || rate <= 0 || !planes[0]) {
      fprintf(stderr, "[media] libavcodec AVFrame does not match the layout this "
              "build assumes (%d samples, %d ch, %d Hz) — dropping audio\n",
              nb, ch, rate);
      free(buf);
      return false;
   }

   struct lm_outbuf *o = NULL;
   for (int i = 0; i < LM_OUT_BUFS; ++i)
      if (!c->out[i].busy && !c->out[i].ready) { o = &c->out[i]; break; }
   if (!o) { free(buf); return true; }

   size_t need = (size_t)nb * ch * 2;
   if (need > o->cap) {
      uint8_t *p = realloc(o->yuv, need);
      if (!p) { free(buf); return false; }
      o->yuv = p;
      o->cap = need;
   }
   o->len = aac_pack_s16(fmt, ch, nb, planes, (int16_t *)o->yuv);
   if (!o->len) {
      static int warned;
      if (warned++ < 4)
         fprintf(stderr, "[media] AAC sample format %d is not one this build "
                 "converts\n", fmt);
      free(buf);
      return true;
   }
   o->w = o->h = 0;
   o->pts_us = pts_us;
   o->flags = 0;
   o->ready = true;
   if (c->rate != rate || c->channels != ch)
      fprintf(stderr, "[media] AAC %d Hz, %d channel(s)\n", rate, ch);
   c->rate = rate;
   c->channels = ch;
   free(buf);
   return true;
}

void lm_codec_audio_format(const struct lm_codec *c, int *rate, int *channels)
{
   if (rate) *rate = c ? c->rate : 0;
   if (channels) *channels = c ? c->channels : 0;
}

bool lm_codec_undecodable(const struct lm_codec *c)
{
   return c && c->undecodable;
}

bool lm_codec_is_video(const struct lm_codec *c)
{
   return c && c->video;
}

bool lm_codec_start(struct lm_codec *c)
{
   if (!c) return false;
   c->started = true;
   /* configure() collected csd-0 / csd-1; the platform feeds them at start(),
    * before any sample.  Doing it here means a caller that never queues its
    * own codec-config buffer still gets a configured decoder. */
   for (int i = 0; i < c->ncsd; ++i) {
      if (c->video) (void)decode_au(c, c->csd[i].p, c->csd[i].len, 0);
      else if (i == 0) aac_take_config(c, c->csd[i].p, c->csd[i].len);
   }
   return true;
}

void lm_codec_stop(struct lm_codec *c)
{
   if (!c) return;
   c->started = false;
   lm_codec_flush(c);
}

void lm_codec_flush(struct lm_codec *c)
{
   if (!c) return;
   for (int i = 0; i < LM_IN_BUFS; ++i) c->in[i].queued = false;
   for (int i = 0; i < LM_OUT_BUFS; ++i) {
      c->out[i].busy = false;
      c->out[i].ready = false;
   }
   c->got_eos = c->sent_eos = false;
}

int lm_codec_dequeue_input(struct lm_codec *c)
{
   if (!c || !c->started) return -1;
   /* Back-pressure: with every output slot spoken for there is nowhere to put
    * the next picture, so the input side has to wait. */
   int free_out = 0;
   for (int i = 0; i < LM_OUT_BUFS; ++i)
      if (!c->out[i].busy && !c->out[i].ready) free_out++;
   if (!free_out) return -1;

   for (int i = 0; i < LM_IN_BUFS; ++i) {
      if (c->in[i].queued) continue;
      if (!c->in[i].p) {
         c->in[i].p = malloc(LM_IN_CAP);
         if (!c->in[i].p) return -1;
         c->in[i].cap = LM_IN_CAP;
      }
      c->in[i].queued = true;   /* held by the caller until queueInputBuffer */
      return i;
   }
   return -1;
}

uint8_t *lm_codec_input_buffer(struct lm_codec *c, int idx, size_t *cap)
{
   if (!c || idx < 0 || idx >= LM_IN_BUFS || !c->in[idx].p) return NULL;
   if (cap) *cap = c->in[idx].cap;
   return c->in[idx].p;
}

bool lm_codec_queue_input(struct lm_codec *c, int idx, int offset, int size,
                          int64_t pts_us, int flags)
{
   if (!c || idx < 0 || idx >= LM_IN_BUFS || !c->in[idx].p) return false;
   if (offset < 0 || size < 0 ||
       (size_t)offset + (size_t)size > c->in[idx].cap) return false;

   c->in[idx].queued = false;
   if (flags & LM_BUFFER_FLAG_END_OF_STREAM) c->got_eos = true;
   if (trace_media())
      fprintf(stderr, "[media] queueInput idx=%d size=%d pts=%lld flags=0x%x\n",
              idx, size, (long long)pts_us, flags);
   if (size > 0) {
      if (c->video)
         (void)decode_au(c, c->in[idx].p + offset, (size_t)size, pts_us);
      else if (flags & LM_BUFFER_FLAG_CODEC_CONFIG)
         aac_take_config(c, c->in[idx].p + offset, (size_t)size);
      else
         (void)decode_aac(c, c->in[idx].p + offset, (size_t)size, pts_us);
   }
   return true;
}

int lm_codec_dequeue_output(struct lm_codec *c, int64_t *pts_us, int *size,
                            int *flags)
{
   if (pts_us) *pts_us = 0;
   if (size) *size = 0;
   if (flags) *flags = 0;
   if (!c || !c->started) return LM_INFO_TRY_AGAIN_LATER;

   /* The format is known once the first picture has been decoded, and the
    * platform reports it before handing that picture over. */
   if ((c->video ? c->width : c->rate) > 0 && !c->announced_format) {
      c->announced_format = true;
      if (trace_media())
         fprintf(stderr, "[media] dequeueOutput -> format changed %dx%d/%dHz\n",
                 c->width, c->height, c->rate);
      return LM_INFO_OUTPUT_FORMAT_CHANGED;
   }

   for (int i = 0; i < LM_OUT_BUFS; ++i) {
      if (!c->out[i].ready || c->out[i].busy) continue;
      c->out[i].busy = true;
      c->out[i].ready = false;
      if (pts_us) *pts_us = c->out[i].pts_us;
      if (size) *size = (int)c->out[i].len;
      if (flags) *flags = c->out[i].flags;
      if (trace_media())
         fprintf(stderr, "[media] dequeueOutput idx=%d size=%d pts=%lld\n",
                 i, (int)c->out[i].len, (long long)c->out[i].pts_us);
      return i;
   }

   /* Everything queued has come out; only then does the stream end. */
   if (c->got_eos && !c->sent_eos) {
      c->sent_eos = true;
      for (int i = 0; i < LM_OUT_BUFS; ++i) {
         if (c->out[i].busy) continue;
         c->out[i].busy = true;
         c->out[i].len = 0;
         c->out[i].flags = LM_BUFFER_FLAG_END_OF_STREAM;
         if (flags) *flags = LM_BUFFER_FLAG_END_OF_STREAM;
         return i;
      }
   }
   return LM_INFO_TRY_AGAIN_LATER;
}

void lm_codec_output_size(const struct lm_codec *c, int *w, int *h)
{
   if (w) *w = c ? c->width : 0;
   if (h) *h = c ? c->height : 0;
}

const uint8_t *lm_codec_output_buffer(struct lm_codec *c, int idx, size_t *len)
{
   if (!c || idx < 0 || idx >= LM_OUT_BUFS || !c->out[idx].busy) return NULL;
   if (len) *len = c->out[idx].len;
   return c->out[idx].yuv;
}

/* BT.601 limited range, the matrix H.264 streams default to. */
static inline uint8_t clamp8(int v)
{
   return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Chroma is shared by a 2x2 block, so its three products are computed once per
 * pair of pixels rather than once per pixel, and the luma row is walked with a
 * pointer instead of being re-indexed from the base on every sample.  At 720p
 * this runs 921,600 times a frame, at video rate, on the thread the guest is
 * waiting on -- the arithmetic per pixel is the whole cost. */
static void i420_to_rgba(const uint8_t *yuv, int w, int h, uint8_t *rgba)
{
   const int cw = (w + 1) / 2, ch = (h + 1) / 2;
   const uint8_t *yp = yuv;
   const uint8_t *up = yuv + (size_t)w * h;
   const uint8_t *vp = up + (size_t)cw * ch;
   int y;

   for (y = 0; y < h; ++y) {
      const uint8_t *yr = yp + (size_t)y * w;
      const uint8_t *ur = up + (size_t)(y / 2) * cw;
      const uint8_t *vr = vp + (size_t)(y / 2) * cw;
      uint8_t *dst = rgba + (size_t)y * w * 4;
      int x = 0;
      for (; x + 1 < w; x += 2) {
         const int d = *ur++ - 128;
         const int e = *vr++ - 128;
         const int r = 409 * e + 128;
         const int g = -100 * d - 208 * e + 128;
         const int b = 516 * d + 128;
         int y298 = 298 * (*yr++ - 16);
         dst[0] = clamp8((y298 + r) >> 8);
         dst[1] = clamp8((y298 + g) >> 8);
         dst[2] = clamp8((y298 + b) >> 8);
         dst[3] = 255;
         y298 = 298 * (*yr++ - 16);
         dst[4] = clamp8((y298 + r) >> 8);
         dst[5] = clamp8((y298 + g) >> 8);
         dst[6] = clamp8((y298 + b) >> 8);
         dst[7] = 255;
         dst += 8;
      }
      if (x < w) {                      /* odd width: one trailing pixel */
         const int d = *ur - 128, e = *vr - 128;
         const int y298 = 298 * (*yr - 16);
         dst[0] = clamp8((y298 + 409 * e + 128) >> 8);
         dst[1] = clamp8((y298 - 100 * d - 208 * e + 128) >> 8);
         dst[2] = clamp8((y298 + 516 * d + 128) >> 8);
         dst[3] = 255;
      }
   }
}

void lm_codec_release_output(struct lm_codec *c, int idx, bool render,
                             struct lm_sink *sink)
{
   if (!c || idx < 0 || idx >= LM_OUT_BUFS || !c->out[idx].busy) return;
   struct lm_outbuf *o = &c->out[idx];

   if (render && sink && o->len && o->w > 0 && o->h > 0) {
      uint8_t *rgba = lm_sink_begin(sink, o->w, o->h);
      if (rgba) {
         i420_to_rgba(o->yuv, o->w, o->h, rgba);
         lm_sink_commit(sink, o->w, o->h, o->pts_us * 1000);
      }
   }
   if (trace_media())
      fprintf(stderr, "[media] releaseOutput idx=%d render=%d sink=%p\n",
              idx, (int)render, (void *)sink);
   o->busy = false;
   o->ready = false;
}

/* ------------------------------------------------------------------------ *
 * MP4 demuxing
 *
 * The moov box is read into memory whole (it is the index, tens of KB); the
 * media data stays on disk and samples are pread() on demand.  Everything the
 * players need is derived once at open() into a flat per-track sample table,
 * because both MediaExtractor and MediaPlayer walk samples in order and asking
 * the chunk tables per sample would be quadratic.
 * ------------------------------------------------------------------------ */

#define LM_MP4_MAX_TRACKS 8

struct lm_mp4_sample {
   int64_t  offset;
   uint32_t size;
   int64_t  pts_us;
   bool     sync;
};

struct lm_mp4_track {
   char     mime[32];
   uint32_t timescale;
   int64_t  duration_us;
   int      width, height;
   int      rate, channels;
   uint8_t *csd;
   size_t   csd_len;
   int      nal_len;                 /* AVC length-prefix width, 0 = not AVC */
   struct lm_mp4_sample *s;
   int      ns;
};

struct lm_mp4 {
   int      fd;                      /* our own dup() */
   int64_t  base;
   struct lm_mp4_track t[LM_MP4_MAX_TRACKS];
   int      nt;
   uint8_t *buf;                     /* the sample handed to the last caller */
   size_t   buf_cap;
};

static uint32_t rd32(const uint8_t *p) {
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
          ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd16(const uint8_t *p) {
   return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}
static uint64_t rd64(const uint8_t *p) {
   return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

/* Walks the children of one box body, calling `fn` per child. */
struct lm_box_walk {
   void (*fn)(void *ctx, const char *type, const uint8_t *body, size_t len);
   void *ctx;
};

static void lm_box_children(const uint8_t *p, size_t len,
                            const struct lm_box_walk *w)
{
   size_t off = 0;
   while (off + 8 <= len) {
      uint64_t size = rd32(p + off);
      char type[5] = { (char)p[off + 4], (char)p[off + 5],
                       (char)p[off + 6], (char)p[off + 7], 0 };
      size_t hdr = 8;
      if (size == 1) {
         if (off + 16 > len) return;
         size = rd64(p + off + 8);
         hdr = 16;
      } else if (size == 0) {
         size = len - off;
      }
      if (size < hdr || off + size > len) return;
      w->fn(w->ctx, type, p + off + hdr, (size_t)size - hdr);
      off += (size_t)size;
   }
}

/* --- one track's sample tables, as read out of stbl ---------------------- */
struct lm_stbl {
   struct lm_mp4_track *tr;
   /* stts */
   const uint8_t *stts; uint32_t stts_n;
   /* ctts */
   const uint8_t *ctts; uint32_t ctts_n; int ctts_ver;
   /* stss */
   const uint8_t *stss; uint32_t stss_n;
   /* stsz */
   const uint8_t *stsz; uint32_t stsz_n; uint32_t stsz_fixed;
   /* stsc */
   const uint8_t *stsc; uint32_t stsc_n;
   /* stco / co64 */
   const uint8_t *stco; uint32_t stco_n; bool co64;
};

static void lm_avcc_to_csd(struct lm_mp4_track *tr, const uint8_t *p, size_t len)
{
   if (len < 7) return;
   tr->nal_len = (p[4] & 3) + 1;
   size_t off = 5;
   uint8_t *out = NULL;
   size_t used = 0;
   int nsets = p[off++] & 0x1f;                    /* SPS */
   for (int round = 0; round < 2; ++round) {
      for (int i = 0; i < nsets && off + 2 <= len; ++i) {
         size_t n = rd16(p + off);
         off += 2;
         if (off + n > len) return;
         uint8_t *grown = realloc(out, used + 4 + n);
         if (!grown) { free(out); return; }
         out = grown;
         out[used] = 0; out[used + 1] = 0; out[used + 2] = 0; out[used + 3] = 1;
         memcpy(out + used + 4, p + off, n);
         used += 4 + n;
         off += n;
      }
      if (round == 0) {
         if (off >= len) break;
         nsets = p[off++];                          /* PPS */
      }
   }
   free(tr->csd);
   tr->csd = out;
   tr->csd_len = used;
}

/* esds → AudioSpecificConfig (the DecoderSpecificInfo, tag 0x05). */
static void lm_esds_to_csd(struct lm_mp4_track *tr, const uint8_t *p, size_t len)
{
   size_t off = 4;                                 /* version + flags */
   while (off + 2 <= len) {
      uint8_t tag = p[off++];
      size_t size = 0;
      for (int i = 0; i < 4 && off < len; ++i) {
         uint8_t b = p[off++];
         size = (size << 7) | (b & 0x7f);
         if (!(b & 0x80)) break;
      }
      if (off + size > len) return;
      if (tag == 0x03) {                           /* ES_Descriptor */
         off += 3;                                 /* ES_ID + flags */
         continue;
      }
      if (tag == 0x04) {                           /* DecoderConfigDescriptor */
         off += 13;
         continue;
      }
      if (tag == 0x05) {                           /* DecoderSpecificInfo */
         uint8_t *csd = malloc(size ? size : 1);
         if (!csd) return;
         memcpy(csd, p + off, size);
         free(tr->csd);
         tr->csd = csd;
         tr->csd_len = size;
         return;
      }
      off += size;
   }
}

static void lm_stsd_child(void *ctx, const char *type, const uint8_t *body,
                          size_t len)
{
   struct lm_mp4_track *tr = ctx;
   if (!strcmp(type, "avcC")) lm_avcc_to_csd(tr, body, len);
   else if (!strcmp(type, "esds")) lm_esds_to_csd(tr, body, len);
}

static void lm_stsd_entry(void *ctx, const char *type, const uint8_t *body,
                          size_t len)
{
   struct lm_mp4_track *tr = ctx;
   struct lm_box_walk w = { lm_stsd_child, tr };
   if (!strcmp(type, "avc1") || !strcmp(type, "avc3")) {
      snprintf(tr->mime, sizeof tr->mime, "video/avc");
      if (len >= 32) {
         tr->width  = rd16(body + 24);
         tr->height = rd16(body + 26);
      }
      if (len > 78) lm_box_children(body + 78, len - 78, &w);
   } else if (!strcmp(type, "hvc1") || !strcmp(type, "hev1")) {
      snprintf(tr->mime, sizeof tr->mime, "video/hevc");
      if (len >= 32) {
         tr->width  = rd16(body + 24);
         tr->height = rd16(body + 26);
      }
   } else if (!strcmp(type, "mp4a")) {
      snprintf(tr->mime, sizeof tr->mime, "audio/mp4a-latm");
      if (len >= 28) {
         tr->channels = rd16(body + 16);
         tr->rate     = rd16(body + 24);            /* 16.16, integer part */
      }
      if (len > 28) lm_box_children(body + 28, len - 28, &w);
   }
}

static void lm_stbl_child(void *ctx, const char *type, const uint8_t *body,
                          size_t len)
{
   struct lm_stbl *st = ctx;
   if (!strcmp(type, "stsd")) {
      if (len < 8) return;
      struct lm_box_walk w = { lm_stsd_entry, st->tr };
      lm_box_children(body + 8, len - 8, &w);
   } else if (!strcmp(type, "stts") && len >= 8) {
      st->stts = body + 8; st->stts_n = rd32(body + 4);
      if (st->stts_n > (len - 8) / 8) st->stts_n = (uint32_t)(len - 8) / 8;
   } else if (!strcmp(type, "ctts") && len >= 8) {
      st->ctts = body + 8; st->ctts_n = rd32(body + 4); st->ctts_ver = body[0];
      if (st->ctts_n > (len - 8) / 8) st->ctts_n = (uint32_t)(len - 8) / 8;
   } else if (!strcmp(type, "stss") && len >= 8) {
      st->stss = body + 8; st->stss_n = rd32(body + 4);
      if (st->stss_n > (len - 8) / 4) st->stss_n = (uint32_t)(len - 8) / 4;
   } else if (!strcmp(type, "stsz") && len >= 12) {
      st->stsz_fixed = rd32(body + 4);
      st->stsz_n = rd32(body + 8);
      st->stsz = body + 12;
      if (!st->stsz_fixed && st->stsz_n > (len - 12) / 4)
         st->stsz_n = (uint32_t)(len - 12) / 4;
   } else if (!strcmp(type, "stsc") && len >= 8) {
      st->stsc = body + 8; st->stsc_n = rd32(body + 4);
      if (st->stsc_n > (len - 8) / 12) st->stsc_n = (uint32_t)(len - 8) / 12;
   } else if (!strcmp(type, "stco") && len >= 8) {
      st->stco = body + 8; st->stco_n = rd32(body + 4); st->co64 = false;
      if (st->stco_n > (len - 8) / 4) st->stco_n = (uint32_t)(len - 8) / 4;
   } else if (!strcmp(type, "co64") && len >= 8) {
      st->stco = body + 8; st->stco_n = rd32(body + 4); st->co64 = true;
      if (st->stco_n > (len - 8) / 8) st->stco_n = (uint32_t)(len - 8) / 8;
   }
}

/* Flattens the chunk/size/time tables into one entry per sample. */
static void lm_stbl_build(struct lm_stbl *st)
{
   struct lm_mp4_track *tr = st->tr;
   uint32_t n = st->stsz_fixed ? 0 : st->stsz_n;
   if (st->stsz_fixed) {
      /* Every sample the same size: the count comes from the time table. */
      for (uint32_t i = 0; i < st->stts_n; ++i)
         n += rd32(st->stts + i * 8);
   }
   if (!n || !st->stco_n || !st->stsc_n) return;
   struct lm_mp4_sample *s = calloc(n, sizeof *s);
   if (!s) return;

   /* offsets: walk chunks, taking samples-per-chunk from stsc */
   uint32_t sample = 0, entry = 0;
   for (uint32_t chunk = 0; chunk < st->stco_n && sample < n; ++chunk) {
      while (entry + 1 < st->stsc_n &&
             rd32(st->stsc + (entry + 1) * 12) <= chunk + 1)
         ++entry;
      uint32_t per = rd32(st->stsc + entry * 12 + 4);
      int64_t off = st->co64 ? (int64_t)rd64(st->stco + chunk * 8)
                            : (int64_t)rd32(st->stco + chunk * 4);
      for (uint32_t k = 0; k < per && sample < n; ++k) {
         uint32_t size = st->stsz_fixed ? st->stsz_fixed
                                        : rd32(st->stsz + sample * 4);
         s[sample].offset = off;
         s[sample].size   = size;
         off += size;
         ++sample;
      }
   }
   uint32_t count = sample;

   /* decode times from stts, shifted by the ctts composition offset */
   uint32_t ts = tr->timescale ? tr->timescale : 1000;
   int64_t dts = 0;
   sample = 0;
   for (uint32_t i = 0; i < st->stts_n && sample < count; ++i) {
      uint32_t cnt = rd32(st->stts + i * 8);
      uint32_t delta = rd32(st->stts + i * 8 + 4);
      for (uint32_t k = 0; k < cnt && sample < count; ++k) {
         s[sample].pts_us = (int64_t)((double)dts * 1000000.0 / (double)ts);
         dts += delta;
         ++sample;
      }
   }
   if (st->ctts) {
      sample = 0;
      for (uint32_t i = 0; i < st->ctts_n && sample < count; ++i) {
         uint32_t cnt = rd32(st->ctts + i * 8);
         int64_t coff = st->ctts_ver ? (int32_t)rd32(st->ctts + i * 8 + 4)
                                     : (int64_t)rd32(st->ctts + i * 8 + 4);
         for (uint32_t k = 0; k < cnt && sample < count; ++k) {
            s[sample].pts_us += (int64_t)((double)coff * 1000000.0 / (double)ts);
            ++sample;
         }
      }
   }

   /* stss lists the sync samples; without it every sample is one */
   if (st->stss) {
      for (uint32_t i = 0; i < st->stss_n; ++i) {
         uint32_t idx = rd32(st->stss + i * 4);
         if (idx >= 1 && idx <= count) s[idx - 1].sync = true;
      }
   } else {
      for (uint32_t i = 0; i < count; ++i) s[i].sync = true;
   }

   tr->s = s;
   tr->ns = (int)count;
}

struct lm_trak_ctx {
   struct lm_mp4_track *tr;
   bool wanted;                       /* a handler type we can decode */
};

static void lm_minf_child(void *ctx, const char *type, const uint8_t *body,
                          size_t len)
{
   struct lm_trak_ctx *tc = ctx;
   if (strcmp(type, "stbl")) return;
   struct lm_stbl st = { .tr = tc->tr };
   struct lm_box_walk w = { lm_stbl_child, &st };
   lm_box_children(body, len, &w);
   lm_stbl_build(&st);
}

static void lm_mdia_child(void *ctx, const char *type, const uint8_t *body,
                          size_t len)
{
   struct lm_trak_ctx *tc = ctx;
   if (!strcmp(type, "mdhd") && len >= 20) {
      if (body[0] == 1 && len >= 32) {
         tc->tr->timescale = rd32(body + 20);
         uint64_t dur = rd64(body + 24);
         if (tc->tr->timescale)
            tc->tr->duration_us =
               (int64_t)((double)dur * 1000000.0 / (double)tc->tr->timescale);
      } else {
         tc->tr->timescale = rd32(body + 12);
         uint32_t dur = rd32(body + 16);
         if (tc->tr->timescale)
            tc->tr->duration_us =
               (int64_t)((double)dur * 1000000.0 / (double)tc->tr->timescale);
      }
   } else if (!strcmp(type, "hdlr") && len >= 12) {
      tc->wanted = !memcmp(body + 8, "vide", 4) || !memcmp(body + 8, "soun", 4);
   } else if (!strcmp(type, "minf")) {
      struct lm_box_walk w = { lm_minf_child, tc };
      lm_box_children(body, len, &w);
   }
}

static void lm_trak_child(void *ctx, const char *type, const uint8_t *body,
                          size_t len)
{
   struct lm_trak_ctx *tc = ctx;
   if (!strcmp(type, "mdia")) {
      struct lm_box_walk w = { lm_mdia_child, tc };
      lm_box_children(body, len, &w);
   }
}

static void lm_moov_child(void *ctx, const char *type, const uint8_t *body,
                          size_t len)
{
   struct lm_mp4 *m = ctx;
   if (strcmp(type, "trak") || m->nt >= LM_MP4_MAX_TRACKS) return;
   struct lm_trak_ctx tc = { .tr = &m->t[m->nt] };
   struct lm_box_walk w = { lm_trak_child, &tc };
   lm_box_children(body, len, &w);
   if (tc.wanted && tc.tr->mime[0] && tc.tr->ns > 0) {
      ++m->nt;
   } else {
      free(tc.tr->csd);
      free(tc.tr->s);
      memset(tc.tr, 0, sizeof *tc.tr);
   }
}

static bool lm_read_at(int fd, int64_t off, void *dst, size_t len)
{
   size_t done = 0;
   while (done < len) {
      ssize_t n = pread(fd, (char *)dst + done, len - done, (off_t)(off + (int64_t)done));
      if (n <= 0) return false;
      done += (size_t)n;
   }
   return true;
}

struct lm_mp4 *lm_mp4_open(int fd, int64_t offset, int64_t length)
{
   if (fd < 0) return NULL;
   int64_t end = length > 0 ? offset + length : INT64_MAX;

   /* Find moov by walking the top-level boxes; mdat is skipped, not read. */
   uint8_t *moov = NULL;
   size_t moov_len = 0;
   for (int64_t pos = offset; pos + 8 <= end; ) {
      uint8_t hdr[16];
      if (!lm_read_at(fd, pos, hdr, 8)) break;
      uint64_t size = rd32(hdr);
      size_t hlen = 8;
      if (size == 1) {
         if (!lm_read_at(fd, pos, hdr, 16)) break;
         size = rd64(hdr + 8);
         hlen = 16;
      }
      if (size < hlen) break;
      if (!memcmp(hdr + 4, "moov", 4)) {
         moov_len = (size_t)(size - hlen);
         if (moov_len > 64u * 1024u * 1024u) break;
         moov = malloc(moov_len ? moov_len : 1);
         if (!moov) break;
         if (!lm_read_at(fd, pos + (int64_t)hlen, moov, moov_len)) {
            free(moov);
            moov = NULL;
         }
         break;
      }
      pos += (int64_t)size;
   }
   if (!moov) {
      if (trace_media()) fprintf(stderr, "[media] mp4: no moov box\n");
      return NULL;
   }

   struct lm_mp4 *m = calloc(1, sizeof *m);
   if (!m) { free(moov); return NULL; }
   m->fd = dup(fd);
   m->base = offset;
   struct lm_box_walk w = { lm_moov_child, m };
   lm_box_children(moov, moov_len, &w);
   free(moov);

   if (!m->nt) { lm_mp4_free(m); return NULL; }
   if (trace_media()) {
      fprintf(stderr, "[media] mp4: %d track(s)\n", m->nt);
      for (int i = 0; i < m->nt; ++i)
         fprintf(stderr, "[media]   #%d %s %dx%d %d samples %lldus csd=%zu\n",
                 i, m->t[i].mime, m->t[i].width, m->t[i].height, m->t[i].ns,
                 (long long)m->t[i].duration_us, m->t[i].csd_len);
   }
   return m;
}

void lm_mp4_free(struct lm_mp4 *m)
{
   if (!m) return;
   for (int i = 0; i < LM_MP4_MAX_TRACKS; ++i) {
      free(m->t[i].csd);
      free(m->t[i].s);
   }
   if (m->fd >= 0) close(m->fd);
   free(m->buf);
   free(m);
}

int lm_mp4_tracks(const struct lm_mp4 *m) { return m ? m->nt : 0; }

static const struct lm_mp4_track *track_of(const struct lm_mp4 *m, int t)
{
   if (!m || t < 0 || t >= m->nt) return NULL;
   return &m->t[t];
}

const char *lm_mp4_mime(const struct lm_mp4 *m, int t)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   return tr ? tr->mime : NULL;
}

int64_t lm_mp4_duration_us(const struct lm_mp4 *m, int t)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   return tr ? tr->duration_us : 0;
}

void lm_mp4_video_size(const struct lm_mp4 *m, int t, int *w, int *h)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   if (w) *w = tr ? tr->width : 0;
   if (h) *h = tr ? tr->height : 0;
}

void lm_mp4_audio_format(const struct lm_mp4 *m, int t, int *rate, int *channels)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   if (rate) *rate = tr ? tr->rate : 0;
   if (channels) *channels = tr ? tr->channels : 0;
}

const uint8_t *lm_mp4_csd(const struct lm_mp4 *m, int t, size_t *len)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   if (len) *len = tr ? tr->csd_len : 0;
   return tr ? tr->csd : NULL;
}

int lm_mp4_samples(const struct lm_mp4 *m, int t)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   return tr ? tr->ns : 0;
}

const uint8_t *lm_mp4_sample(struct lm_mp4 *m, int t, int i, size_t *len,
                             int64_t *pts_us, bool *sync)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   if (!tr || i < 0 || i >= tr->ns) return NULL;
   const struct lm_mp4_sample *s = &tr->s[i];
   if (pts_us) *pts_us = s->pts_us;
   if (sync) *sync = s->sync;

   size_t need = s->size;
   if (need > m->buf_cap) {
      uint8_t *grown = realloc(m->buf, need);
      if (!grown) return NULL;
      m->buf = grown;
      m->buf_cap = need;
   }
   if (!lm_read_at(m->fd, s->offset, m->buf, need)) return NULL;

   /* AVC in MP4 prefixes each NAL with its length; a decoder wants Annex-B
    * start codes.  The 4-byte case is a rewrite in place; narrower prefixes
    * change the size, so those are expanded into the same buffer only when it
    * is big enough to hold the growth. */
   if (tr->nal_len > 0 && tr->nal_len <= 4) {
      int nl = tr->nal_len;
      if (nl == 4) {
         size_t off = 0;
         while (off + 4 <= need) {
            uint32_t n = rd32(m->buf + off);
            m->buf[off] = 0; m->buf[off + 1] = 0;
            m->buf[off + 2] = 0; m->buf[off + 3] = 1;
            if (n > need - off - 4) break;
            off += 4 + n;
         }
      } else {
         /* count NALs to size the expansion */
         size_t off = 0, nnal = 0;
         while (off + (size_t)nl <= need) {
            uint32_t n = 0;
            for (int k = 0; k < nl; ++k) n = (n << 8) | m->buf[off + k];
            ++nnal;
            if (n > need - off - (size_t)nl) break;
            off += (size_t)nl + n;
         }
         size_t grow = need + nnal * (size_t)(4 - nl);
         if (grow > m->buf_cap) {
            uint8_t *grown = realloc(m->buf, grow);
            if (!grown) return NULL;
            m->buf = grown;
            m->buf_cap = grow;
         }
         /* rewrite from the back so the moves do not overlap destructively */
         size_t src = need, dst = grow;
         /* walk forward once to record the NAL starts */
         size_t starts[512];
         size_t sizes[512];
         size_t count = 0;
         off = 0;
         while (off + (size_t)nl <= need && count < 512) {
            uint32_t n = 0;
            for (int k = 0; k < nl; ++k) n = (n << 8) | m->buf[off + k];
            if (n > need - off - (size_t)nl) break;
            starts[count] = off + (size_t)nl;
            sizes[count] = n;
            ++count;
            off += (size_t)nl + n;
         }
         for (size_t k = count; k-- > 0; ) {
            dst -= sizes[k];
            memmove(m->buf + dst, m->buf + starts[k], sizes[k]);
            dst -= 4;
            m->buf[dst] = 0; m->buf[dst + 1] = 0;
            m->buf[dst + 2] = 0; m->buf[dst + 3] = 1;
         }
         (void)src;
         need = grow - dst;
         if (dst) memmove(m->buf, m->buf + dst, need);
      }
   }
   if (len) *len = need;
   return m->buf;
}

int lm_mp4_sync_sample_at(const struct lm_mp4 *m, int t, int64_t us)
{
   const struct lm_mp4_track *tr = track_of(m, t);
   if (!tr || tr->ns <= 0) return 0;
   int best = 0;
   for (int i = 0; i < tr->ns; ++i) {
      if (!tr->s[i].sync) continue;
      if (tr->s[i].pts_us <= us) best = i;
      else break;
   }
   return best;
}
