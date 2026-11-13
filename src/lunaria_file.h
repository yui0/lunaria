/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Host file management: process scratch files and persistent guest links. */
#ifndef LUNARIA_FILE_H
#define LUNARIA_FILE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lunaria_os.h"
#ifdef __cplusplus
extern "C" {
#endif
int luna_temp_path(char *path,size_t capacity,const char *name);
int luna_temp_mkstemp(char *pattern,size_t capacity);
char *luna_temp_mkdtemp(char *pattern,size_t capacity);

typedef const char *(*luna_symlink_mapper)(const char *,char *,size_t);
/* Android java.io.File pathname syntax (independent of host OS and storage
 * mappings). Returned strings are allocated; callers free them. */
char *luna_java_file_normalize(const char *path);
char *luna_java_file_resolve(const char *parent,const char *child);
/* Canonicalize a logical path across mounts, including nonexistent names.
 * Resolve links before '..', then project existing targets into that namespace. */
char *luna_file_canonical_namespace(const char *path,char *out,size_t capacity,
                                    luna_symlink_mapper mapper,luna_symlink_mapper reverse);
/* Return nonzero for modeled inodes, projecting guest uid/gid/mode.
 * Unmanaged host scratch paths retain host semantics. */
typedef int (*luna_file_inode_mapper)(const char *,luna_file_info *);
void luna_file_set_credentials(unsigned uid,unsigned gid,luna_file_inode_mapper mapper);
void luna_symlink_set_mapper(luna_symlink_mapper mapper,luna_symlink_mapper reverse);
int luna_symlink_namespace(const char *path,char *out,size_t capacity,luna_symlink_mapper raw);
int luna_symlink_create(const char *target,const char *path);
int luna_symlink_resolve(const char *path,char *out,size_t capacity,int follow);
int luna_symlink_openat(int dirfd,const char *path,int flags,unsigned mode);
int luna_symlink_open(const char *path,int flags,unsigned mode);
int luna_symlink_mkdir(const char *path,unsigned mode);
int luna_symlink_mkdirat(int dirfd,const char *path,unsigned mode);
int luna_symlink_link(const char *from,const char *to);
int luna_symlink_chmodat(int dirfd,const char *path,unsigned mode,int flags);
int luna_symlink_timesat(int dirfd,const char *path,const luna_file_time times[2],int flags);
int luna_symlink_set_modified(const char *path,int64_t milliseconds);
int luna_file_stat_follow(const char *path,struct stat *info);
int luna_file_stat_nofollow(const char *path,struct stat *info);
FILE *luna_symlink_fopen(const char *path,const char *mode);
int luna_symlink_fd_info(int fd,luna_file_info *info);
int luna_symlink_fd_chmod(int fd,unsigned mode);
int luna_symlink_infoat(int dirfd,const char *path,luna_file_info *info,int flags);
int luna_symlink_unlinkat(int dirfd,const char *path,int flags);
int luna_symlink_unlink(const char *path);
int luna_symlink_rename(const char *from,const char *to);
int luna_symlink_renameat(int fromfd,const char *from,int tofd,const char *to);
ptrdiff_t luna_symlink_readlink(const char *path,char *out,size_t capacity);
int luna_symlink_accessat(int dirfd,const char *path,int mode,int flags);
int luna_symlink_access(const char *path,int mode);
char *luna_symlink_realpath(const char *path,char *out,size_t capacity);
luna_directory *luna_symlink_directory_open(const char *path);
luna_directory *luna_symlink_directory_open_fd(int fd);
luna_directory_entry *luna_symlink_directory_read(luna_directory *dir);
int luna_symlink_directory_close(luna_directory *dir);
/* Device directories created before Android application processes. */
int luna_android_fs_prepare(const char *root);
int luna_android_fs_inode(const char *guest_path, luna_file_info *info);
/* ---- CA certificates (luna_certs.c) ---- */

/* Caller owns the stream. Respects the host OpenSSL CA-file configuration. */
FILE *luna_certs_open_system_bundle(void);
/* Existing directory, PEM input. Both OpenSSL hash generations are indexed;
 * collisions get consecutive suffixes. Returns certificate count or -1. */
int luna_certs_make_hash_cache(FILE *source,const char *directory);

/* ---- Read-ahead for immutable assets ---- */

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
#ifdef __cplusplus
}
#endif
#endif
