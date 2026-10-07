/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
/* Bounded C11 reader for ResTable_map_entry. No allocation or aligned reads. */
#ifndef LUNARIA_ARSC_BAG_H
#define LUNARIA_ARSC_BAG_H
#include <stddef.h>
#include <stdint.h>
static uint16_t arsc_u16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static uint32_t arsc_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
struct arsc_bag_view { const uint8_t *maps; uint32_t count, parent; };
typedef int (*arsc_score_fn)(const uint8_t *, size_t, uint8_t);
static int arsc_find_bag(const uint8_t *b, size_t n, uint32_t id,
                         uint8_t orientation, arsc_score_fn score_config,
                         struct arsc_bag_view *out)
{
   if (!b || n < 12 || arsc_u16(b) != 2) return 0;
   uint32_t table_size = arsc_u32(b + 4);
   uint16_t table_header = arsc_u16(b + 2);
   if (table_size > n || table_size < 12 || table_header < 12 || table_header > table_size) return 0;
   n = table_size;
   int best = -1, found = 0;
   for (size_t i = table_header; n - i >= 8;) {
      uint32_t size = arsc_u32(b+i+4);
      uint16_t header = arsc_u16(b+i+2);
      if (size < 8 || size > n-i || header < 8 || header > size) return 0;
      if (arsc_u16(b+i) == 0x200 && header >= 284 && arsc_u32(b+i+8) == (id >> 24)) {
         uint32_t type_offset = header >= 288 ? arsc_u32(b+i+284) : 0;
         size_t end = i + size;
         for (size_t j = i+header; end-j >= 8;) {
            uint32_t ts = arsc_u32(b+j+4);
            uint16_t th = arsc_u16(b+j+2);
            if (ts < 8 || ts > end-j || th < 8 || th > ts) return 0;
            if (arsc_u16(b+j) == 0x201 && th >= 20 &&
                (uint32_t)b[j+8] + type_offset == ((id >> 16) & 255)) {
               uint32_t count = arsc_u32(b+j+12), start = arsc_u32(b+j+16);
               uint32_t entry = id & 65535, off = UINT32_MAX;
               uint8_t flags = b[j+9];
               size_t width = (flags & 2) ? 2 : 4;
               if (start < th || start > ts || count > (start-th)/width) return 0;
               const uint8_t *indices = b+j+th;
               if (flags & 1) {
                  if (flags & 2) return 0;
                  /* Sparse indices are sorted; binary search keeps lookup bounded. */
                  uint32_t lo=0, hi=count;
                  while (lo < hi) {
                     uint32_t mid=lo+(hi-lo)/2;
                     if (arsc_u16(indices+mid*4) < entry) lo=mid+1; else hi=mid;
                  }
                  if (lo < count && arsc_u16(indices+lo*4) == entry)
                     off=(uint32_t)arsc_u16(indices+lo*4+2)*4;
               } else if (entry < count) {
                  if (flags & 2) {
                     uint16_t v=arsc_u16(indices+entry*2);
                     if (v != 65535) off=(uint32_t)v*4;
                  } else off=arsc_u32(indices+entry*4);
               }
               if (off != UINT32_MAX) {
                  if (off > ts-start || ts-start-off < 8) return 0;
                  const uint8_t *e=b+j+start+off;
                  size_t available=ts-start-off;
                  uint16_t eh=arsc_u16(e), ef=arsc_u16(e+2);
                  if (eh < 8 || eh > available) return 0;
                  int score=score_config ? score_config(b+j+20,th-20,orientation) : 0;
                  if (score >= 0 && score >= best) {
                     best=score; found=0;
                     if ((ef & 1) && eh >= 16) {
                        uint32_t maps=arsc_u32(e+12);
                        if (maps > (available-eh)/12) return 0;
                        for (uint32_t k=0;k<maps;++k)
                           if (arsc_u16(e+eh+k*12+4) != 8) return 0;
                        out->maps=e+eh; out->count=maps; out->parent=arsc_u32(e+8);
                        found=1;
                     }
                  }
               }
            }
            j+=ts;
         }
      }
      i+=size;
   }
   return found;
}
#endif
