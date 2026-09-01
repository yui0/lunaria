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

#pragma once

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
