/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The declarations this build actually calls out of stb_vorbis.c (Sean
 * Barrett's public-domain/MIT single-file decoder, vendored verbatim in that
 * file).  stb_vorbis.c is its own translation unit — see the Makefile — so
 * this header exists only so a caller does not need to pull in the whole
 * 5500-line implementation to get the four function signatures it needs.
 * Kept in sync with the subset of stb_vorbis.c's public API this file
 * declares; the rest of that API is unused here.
 */
#ifndef LUNARIA_STB_VORBIS_H
#define LUNARIA_STB_VORBIS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct stb_vorbis stb_vorbis;

typedef struct {
   unsigned int sample_rate;
   int channels;
   unsigned int setup_memory_required;
   unsigned int setup_temp_memory_required;
   unsigned int temp_memory_required;
   int max_frame_size;
} stb_vorbis_info;

typedef struct {
   char *alloc_buffer;
   int alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;

stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len,
                                   int *error, const stb_vorbis_alloc *alloc_buffer);
stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
void stb_vorbis_close(stb_vorbis *f);
int stb_vorbis_seek_start(stb_vorbis *f);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels,
                                             short *buffer, int num_shorts);
int stb_vorbis_get_error(stb_vorbis *f);

#ifdef __cplusplus
}
#endif

#endif
