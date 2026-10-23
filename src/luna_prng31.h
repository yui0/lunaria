/* MPL-2.0. Bionic's default random/srandom generator, with caller-owned state.
 * Algorithm: Android 12 libc/upstream-netbsd/common/lib/libc/stdlib/random.c
 * (TYPE_3: 31 words, separation 3, Park-Miller initialization, 310 warmups).
 * https://android.googlesource.com/platform/bionic/+/android-12.0.0_r1/libc/upstream-netbsd/common/lib/libc/stdlib/random.c
 * Callers serialize access to a shared state, as Bionic's random_mutex does.
 * This must never share the host libc PRNG or Java's Random state. */
#ifndef LUNA_PRNG31_H
#define LUNA_PRNG31_H

#include <stdbool.h>
#include <stdint.h>

struct luna_prng31 {
   uint32_t words[31];
   unsigned char front, rear;
   bool seeded;
};

static inline uint32_t luna_prng31_step(struct luna_prng31 *s)
{
   uint32_t sum = s->words[s->front] + s->words[s->rear];
   s->words[s->front] = sum;
   if (++s->front == 31) s->front = 0;
   if (++s->rear == 31) s->rear = 0;
   return sum >> 1;
}

static inline void luna_prng31_seed(struct luna_prng31 *s, uint32_t seed)
{
   /* Bionic interprets the seed as a signed 32-bit word during expansion.
    * Widen before multiplication; neither signed overflow nor a host's
    * implementation-defined conversion of large unsigned seeds is needed. */
   int64_t word = seed;
   if (word > INT32_MAX) word -= INT64_C(4294967296);
   s->words[0] = seed;
   for (unsigned i = 1; i < 31; ++i) {
      word = (word * 16807) % INT64_C(2147483647);
      if (word <= 0) word += INT64_C(2147483647);
      s->words[i] = (uint32_t)word;
   }
   s->front = 3;
   s->rear = 0;
   s->seeded = true;
   for (unsigned i = 0; i < 310; ++i) (void)luna_prng31_step(s);
}

static inline uint32_t luna_prng31_next(struct luna_prng31 *s)
{
   if (!s->seeded) luna_prng31_seed(s, 1);
   return luna_prng31_step(s);
}
#endif
