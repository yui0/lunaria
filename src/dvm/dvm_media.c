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
   ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
   if (n > 0) {
      self[n] = '\0';
      char *slash = strrchr(self, '/');
      if (slash) {
         char path[PATH_MAX];
         *slash = '\0';
         if ((size_t)snprintf(path, sizeof path, "%s/runtime/libopenh264.so",
                              self) < sizeof path) {
            void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (h) return h;
         }
      }
   }

   static const char *const fallbacks[] = {
      "/usr/local/lib/lunaria/libopenh264.so",
      "libopenh264.so",
      "libopenh264.so.7",
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

void lm_sink_push(struct lm_sink *s, const uint8_t *rgba, int w, int h,
                  int64_t timestamp_ns)
{
   if (!s || !rgba || w <= 0 || h <= 0) return;
   size_t need = (size_t)w * (size_t)h * 4u;
   if (need > s->cap) {
      uint8_t *p = realloc(s->rgba, need);
      if (!p) return;
      s->rgba = p;
      s->cap = need;
   }
   memcpy(s->rgba, rgba, need);
   s->w = w;
   s->h = h;
   s->ts_ns = timestamp_ns;
   s->pending = true;
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
};

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
       * usually recovers.  Only report it. */
   }
   if (info.iBufferStatus != 1) return true;   /* nothing came out this time */

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

static void i420_to_rgba(const uint8_t *yuv, int w, int h, uint8_t *rgba)
{
   int cw = (w + 1) / 2, ch = (h + 1) / 2;
   const uint8_t *yp = yuv;
   const uint8_t *up = yuv + (size_t)w * h;
   const uint8_t *vp = up + (size_t)cw * ch;

   for (int y = 0; y < h; ++y) {
      const uint8_t *ur = up + (size_t)(y / 2) * cw;
      const uint8_t *vr = vp + (size_t)(y / 2) * cw;
      uint8_t *dst = rgba + (size_t)y * w * 4;
      for (int x = 0; x < w; ++x) {
         int c = yp[(size_t)y * w + x] - 16;
         int d = ur[x / 2] - 128;
         int e = vr[x / 2] - 128;
         int y298 = 298 * c;
         dst[0] = clamp8((y298 + 409 * e + 128) >> 8);
         dst[1] = clamp8((y298 - 100 * d - 208 * e + 128) >> 8);
         dst[2] = clamp8((y298 + 516 * d + 128) >> 8);
         dst[3] = 255;
         dst += 4;
      }
   }
}

void lm_codec_release_output(struct lm_codec *c, int idx, bool render,
                             struct lm_sink *sink)
{
   if (!c || idx < 0 || idx >= LM_OUT_BUFS || !c->out[idx].busy) return;
   struct lm_outbuf *o = &c->out[idx];

   if (render && sink && o->len && o->w > 0 && o->h > 0) {
      size_t need = (size_t)o->w * (size_t)o->h * 4u;
      uint8_t *rgba = malloc(need);
      if (rgba) {
         i420_to_rgba(o->yuv, o->w, o->h, rgba);
         lm_sink_push(sink, rgba, o->w, o->h, o->pts_us * 1000);
         free(rgba);
      }
   }
   if (trace_media())
      fprintf(stderr, "[media] releaseOutput idx=%d render=%d sink=%p\n",
              idx, (int)render, (void *)sink);
   o->busy = false;
   o->ready = false;
}
