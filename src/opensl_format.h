#ifndef LUNARIA_OPENSL_FORMAT_H
#define LUNARIA_OPENSL_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LUNA_SL_DATAFORMAT_PCM = 1u };

/* Validate the ABI-visible prefix of Android's SLDataFormat_PCM.
 * samples_per_sec is expressed in milli-Hz by OpenSL ES. */
static inline int luna_sl_pcm_format(uint32_t format_type,
                                     uint32_t num_channels,
                                     uint32_t samples_per_sec,
                                     uint32_t *rate_hz,
                                     uint32_t *channels)
{
   if (format_type != LUNA_SL_DATAFORMAT_PCM ||
       num_channels < 1u || num_channels > 8u ||
       samples_per_sec < 8000000u || samples_per_sec > 384000000u ||
       samples_per_sec % 1000u != 0u)
      return 0;
   if (rate_hz) *rate_hz = samples_per_sec / 1000u;
   if (channels) *channels = num_channels;
   return 1;
}

#ifdef __cplusplus
}
#endif
#endif
