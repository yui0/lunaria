/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef LUNA_FILE_READ_CACHE_H
#define LUNA_FILE_READ_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Immutable asset read-ahead. The reader uses positional I/O; this cache
 * changes neither its descriptor's cursor nor the caller's seek position. */
struct luna_read_cache {
  unsigned char *bytes;
  uint64_t offset;
  size_t valid;
};
typedef ptrdiff_t (*luna_read_at_fn)(void *, void *, size_t, uint64_t);
#define LUNA_READ_CACHE_BYTES (64u * 1024u)
static inline void luna_read_cache_clear(struct luna_read_cache *cache)
{
  free(cache->bytes);
  memset(cache, 0, sizeof *cache);
}
static inline ptrdiff_t luna_read_cache_read(struct luna_read_cache *cache,
    luna_read_at_fn reader, void *context, uint64_t offset, uint64_t length,
    void *destination, size_t count)
{
  if (offset >= length || !count) return 0;
  uint64_t remaining = length - offset;
  if (remaining < count) count = (size_t)remaining;
  if (count >= LUNA_READ_CACHE_BYTES)
    return reader(context, destination, count, offset);
  if (!cache->bytes) cache->bytes = (unsigned char *)malloc(LUNA_READ_CACHE_BYTES);
  if (!cache->bytes) return reader(context, destination, count, offset);
  size_t done = 0;
  while (done < count) {
    if (offset < cache->offset || offset - cache->offset >= cache->valid) {
      size_t request = length - offset < LUNA_READ_CACHE_BYTES
                       ? (size_t)(length - offset) : LUNA_READ_CACHE_BYTES;
      ptrdiff_t received = reader(context, cache->bytes, request, offset);
      /* The reader may overwrite the old bytes before returning an error. */
      cache->valid = 0;
      if (received <= 0) return done ? (ptrdiff_t)done : received;
      cache->offset = offset;
      cache->valid = (size_t)received;
    }
    size_t index = (size_t)(offset - cache->offset);
    size_t available = cache->valid - index;
    size_t take = count - done < available ? count - done : available;
    memcpy((unsigned char *)destination + done, cache->bytes + index, take);
    offset += take;
    done += take;
  }
  return (ptrdiff_t)done;
}

#endif
