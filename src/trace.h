/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef VERBOSE_FUNCTIONS
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

/* One line per call, prefixed by the host thread that made it.  The lock is
 * per translation unit: a line is formatted into a local buffer and written
 * with one fprintf. */
static inline void verbose_log(const char *fmt, ...)
{
   static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof buf, fmt, ap);
   va_end(ap);
   pthread_mutex_lock(&mutex);
   fprintf(stderr, "%lu: %s\n", (unsigned long)pthread_self(), buf);
   pthread_mutex_unlock(&mutex);
}
#  define verbose verbose_log
#else
#  define verbose(...) ((void)0)
#endif

/* Identity passthrough — no mmap/asm trampolines in the default build. */
static inline void *
wrapper_create(const char *symbol, void *function)
{
   (void)symbol;
   return function;
}

static inline void
wrapper_set_cpp_demangler(void *function)
{
   (void)function;
}

/* AHardwareBuffer_Format sizes for ANativeWindow CPU buffers.  Zero means
 * this build cannot hand out a buffer for that format. */
static inline unsigned android_pixel_bytes(unsigned format)
{
    switch (format) {
    case 1: case 2: return 4; /* RGBA_8888, RGBX_8888 */
    case 3: return 3; /* RGB_888 */
    case 4: return 2; /* RGB_565 */
    default: return 0;
    }
}

/* Widen `count` pixels to the RGBA8888 the host texture upload wants. */
static inline void android_pixels_rgba(uint8_t *out, const uint8_t *in,
                                       size_t count, unsigned format)
{
    size_t i;
    unsigned bytes = android_pixel_bytes(format);
    for (i = 0; i < count; ++i, in += bytes, out += 4) {
        if (format == 4) {
            unsigned pixel = in[0] | ((unsigned)in[1] << 8);
            unsigned r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
            out[0] = (uint8_t)((r << 3) | (r >> 2));
            out[1] = (uint8_t)((g << 2) | (g >> 4));
            out[2] = (uint8_t)((b << 3) | (b >> 2));
        } else {
            out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
        }
        out[3] = format == 1 ? in[3] : 255;
    }
}
