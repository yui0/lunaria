/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNA_AAC_ENCODER_H
#define LUNA_AAC_ENCODER_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct luna_aac;
/* Raw AAC access units, interleaved signed 16-bit PCM input. All storage is
 * caller owned. A backend consumes no more than one frame per call. */
bool luna_aac_available(void);
struct luna_aac *luna_aac_new(int rate,int channels,int bitrate,int profile);
void luna_aac_free(struct luna_aac *);
const uint8_t *luna_aac_config(const struct luna_aac *,size_t *);
unsigned luna_aac_frame_samples(const struct luna_aac *);
unsigned luna_aac_delay(const struct luna_aac *);
size_t luna_aac_max_output(const struct luna_aac *);
/* 0 success, 1 fully drained, negative backend error. samples is the total
 * across channels, or -1 to drain. consumed/written are always initialized. */
int luna_aac_encode(struct luna_aac *,const int16_t *,int samples,
                    uint8_t *,size_t capacity,int *consumed,size_t *written);
#ifdef __cplusplus
}
#endif
#endif
