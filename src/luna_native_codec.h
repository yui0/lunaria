/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNA_NATIVE_CODEC_H
#define LUNA_NATIVE_CODEC_H
#include "luna_media_format.h"
#ifdef __cplusplus
extern "C" {
#endif
struct lmc_codec;
struct lmc_buffer_info { int32_t offset,size; int64_t time_us; uint32_t flags; };
struct lmc_codec *lmc_encoder(const char *,const struct luna_mf_allocator *);
void lmc_delete(struct lmc_codec *);
int lmc_configure(struct lmc_codec *,AMediaFormat *,unsigned flags);
int lmc_start(struct lmc_codec *);
int lmc_stop(struct lmc_codec *);
int lmc_flush(struct lmc_codec *);
int lmc_dequeue_input(struct lmc_codec *);
bool lmc_input_ready(struct lmc_codec *);
bool lmc_output_ready(struct lmc_codec *);
void *lmc_input(struct lmc_codec *,size_t index,size_t *size,uintptr_t *address);
int lmc_queue_input(struct lmc_codec *,size_t index,size_t offset,size_t bytes,int64_t time_us,unsigned flags);
int lmc_dequeue_output(struct lmc_codec *,struct lmc_buffer_info *);
void *lmc_output(struct lmc_codec *,size_t index,size_t *size,uintptr_t *address);
int lmc_release_output(struct lmc_codec *,size_t index,bool render);
AMediaFormat *lmc_output_format(struct lmc_codec *);
#ifdef __cplusplus
}
#endif
#endif
