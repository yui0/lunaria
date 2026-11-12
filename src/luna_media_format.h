/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNA_MEDIA_FORMAT_H
#define LUNA_MEDIA_FORMAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct AMediaFormat AMediaFormat;
typedef int32_t media_status_t;
/* Payload allocation may expose the same bytes at a guest address. */
struct luna_mf_allocator {
  void *context;
  void *(*allocate)(void *,size_t,uintptr_t *);
  void (*release)(void *,void *,uintptr_t);
};
AMediaFormat *luna_media_format_new(const struct luna_mf_allocator *);
uintptr_t luna_media_format_address(const AMediaFormat *,const void *);
const char *luna_media_format_key(const char *symbol);
AMediaFormat *AMediaFormat_new(void);
media_status_t AMediaFormat_delete(AMediaFormat *);
void AMediaFormat_clear(AMediaFormat *);
media_status_t AMediaFormat_copy(AMediaFormat *,AMediaFormat *);
const char *AMediaFormat_toString(AMediaFormat *);
#define LUNA_MF_SCALAR(NAME,TYPE) \
  bool AMediaFormat_get##NAME(AMediaFormat *,const char *,TYPE *); \
  void AMediaFormat_set##NAME(AMediaFormat *,const char *,TYPE);
LUNA_MF_SCALAR(Int32,int32_t)
LUNA_MF_SCALAR(Int64,int64_t)
LUNA_MF_SCALAR(Float,float)
LUNA_MF_SCALAR(Double,double)
LUNA_MF_SCALAR(Size,size_t)
#undef LUNA_MF_SCALAR
bool AMediaFormat_getRect(AMediaFormat *,const char *,int32_t *,int32_t *,int32_t *,int32_t *);
void AMediaFormat_setRect(AMediaFormat *,const char *,int32_t,int32_t,int32_t,int32_t);
bool AMediaFormat_getString(AMediaFormat *,const char *,const char **);
void AMediaFormat_setString(AMediaFormat *,const char *,const char *);
bool AMediaFormat_getBuffer(AMediaFormat *,const char *,void **,size_t *);
void AMediaFormat_setBuffer(AMediaFormat *,const char *,const void *,size_t);
#include "luna_media_format_keys.h"
#ifdef __cplusplus
}
#endif
#endif
