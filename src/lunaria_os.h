/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The host operating system, as this emulator uses it.
 *
 * Platform services and shared host helpers used by the emulator. The
 * inline helpers cover guest ABI conversion, files, sockets, strings and
 * the run queue; operations requiring a host implementation are declared
 * here and provided by the platform source below.
 *
 * Implemented once per platform: lunaria_linux.c, lunaria_windows.c,
 * lunaria_mac.c.  The Makefile picks by uname; a build for one host never
 * compiles another's file, so the three are free to include whatever their
 * own platform needs.
 *
 * C, not C++: the emulator is being taken to C, and this is the layer that
 * has to compile first.
 */

#ifndef LUNARIA_OS_H
#define LUNARIA_OS_H

#include <stddef.h>
#include <stdint.h>

/* ---- Filename patterns (Android/Bionic flag values) ---- */
#define LUNA_FNM_PATHNAME 1
#define LUNA_FNM_NOESCAPE 2
#define LUNA_FNM_PERIOD 4
#define LUNA_FNM_LEADING_DIR 8
#define LUNA_FNM_CASEFOLD 16
#define LUNA_FNM_NOMATCH 1
#ifdef __cplusplus
extern "C" {
#endif
int luna_fnmatch(const char *pattern, const char *string, int flags);
#ifdef __cplusplus
}
#endif

/* ---- Socket interfaces ---- */
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <mswsock.h>
struct iovec { void *iov_base; size_t iov_len; };
struct msghdr {
   void *msg_name;
   socklen_t msg_namelen;
   struct iovec *msg_iov;
   size_t msg_iovlen;
   void *msg_control;
   size_t msg_controllen;
   int msg_flags;
};
typedef WSACMSGHDR luna_cmsghdr;
#define LUNA_CMSG_ALIGN(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define LUNA_CMSG_SPACE(n) (LUNA_CMSG_ALIGN(sizeof(luna_cmsghdr)) + LUNA_CMSG_ALIGN(n))
#define LUNA_CMSG_LEN(n) (LUNA_CMSG_ALIGN(sizeof(luna_cmsghdr)) + (n))
#define LUNA_CMSG_DATA(c) ((unsigned char *)(c) + LUNA_CMSG_ALIGN(sizeof(luna_cmsghdr)))
#define LUNA_CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(luna_cmsghdr) ? (luna_cmsghdr *)(m)->msg_control : NULL)
static inline luna_cmsghdr *luna_cmsg_next(const struct msghdr *message, const luna_cmsghdr *current)
{
   uintptr_t start = (uintptr_t)message->msg_control, at = (uintptr_t)current;
   if (message->msg_controllen > UINTPTR_MAX - start) return NULL;
   uintptr_t end = start + message->msg_controllen;
   if (at < start || at > end || end - at < sizeof *current ||
       current->cmsg_len < sizeof *current || current->cmsg_len > end - at) return NULL;
   size_t step = LUNA_CMSG_ALIGN(current->cmsg_len);
   if (step < current->cmsg_len || step > end - at || end - at - step < sizeof *current) return NULL;
   return (luna_cmsghdr *)(at + step);
}
#define LUNA_CMSG_NXTHDR(m,c) luna_cmsg_next((m),(c))
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
typedef struct cmsghdr luna_cmsghdr;
#define LUNA_CMSG_SPACE(n) CMSG_SPACE(n)
#define LUNA_CMSG_LEN(n) CMSG_LEN(n)
#define LUNA_CMSG_DATA(c) CMSG_DATA(c)
#define LUNA_CMSG_FIRSTHDR(m) CMSG_FIRSTHDR(m)
#define LUNA_CMSG_NXTHDR(m,c) CMSG_NXTHDR(m,c)
#endif
#ifdef __cplusplus
extern "C" {
#endif
#ifdef MSG_NOSIGNAL
#define LUNA_SEND_NOSIGNAL MSG_NOSIGNAL
#else
#define LUNA_SEND_NOSIGNAL 0
#endif
#ifdef _WIN32
/* Sockets occupy CRT descriptor slots, backed by separate Winsock handles.
 * File and socket descriptors consequently cannot collide. Unwrap only for
 * external socket APIs such as OpenSSL's socket BIO. */
int luna_socket_startup(void);
int luna_socket_inet_aton(const char *text, struct in_addr *address);
intptr_t luna_socket_native(int fd);
int luna_socket_open(int family, int type, int protocol);
int luna_socket_pair(int family, int type, int protocol, int pair[2]);
int luna_socket_accept(int fd, struct sockaddr *address, socklen_t *length);
int luna_socket_bind(int fd, const struct sockaddr *address, socklen_t length);
int luna_socket_connect(int fd, const struct sockaddr *address, socklen_t length);
int luna_socket_listen(int fd, int backlog);
int luna_socket_shutdown(int fd, int how);
int luna_socket_name(int fd, struct sockaddr *address, socklen_t *length);
int luna_socket_peer(int fd, struct sockaddr *address, socklen_t *length);
int luna_socket_set_option(int fd, int level, int option, const void *value, socklen_t length);
int luna_socket_get_option(int fd, int level, int option, void *value, socklen_t *length);
int luna_socket_nonblock(int fd, int enabled);
int luna_socket_get_nonblock(int fd);
int luna_socket_poll(struct pollfd *fds, size_t count, int timeout_ms);
int luna_socket_dns(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **out);
int luna_socket_available(int fd, int *bytes);
ptrdiff_t luna_socket_recv(int fd, void *buffer, size_t length, int flags);
ptrdiff_t luna_socket_send(int fd, const void *buffer, size_t length, int flags);
ptrdiff_t luna_socket_recvfrom(int fd, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
ptrdiff_t luna_socket_sendto(int fd, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
ptrdiff_t luna_socket_sendmsg(int fd, const struct msghdr *message, int flags);
ptrdiff_t luna_socket_recvmsg(int fd, struct msghdr *message, int flags);
ptrdiff_t luna_fd_writev(int fd, const struct iovec *vectors, int count);
ptrdiff_t luna_fd_read(int fd, void *buffer, size_t length);
ptrdiff_t luna_fd_write(int fd, const void *buffer, size_t length);
int luna_fd_close(int fd);
int luna_fd_get_nonblock(int fd);
int luna_fd_nonblock(int fd, int enabled);
int luna_fd_cloexec(int fd, int enabled);
int luna_fd_get_cloexec(int fd);
int luna_fd_dup(int fd, int minimum, int close_on_exec);
int luna_fd_dup_to(int fd, int target, int close_on_exec);
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
#else
static inline int luna_socket_startup(void) { return 0; }
static inline int luna_socket_inet_aton(const char *text, struct in_addr *address)
{ return inet_aton(text, address); }
static inline intptr_t luna_socket_native(int fd) { return fd; }
static inline int luna_socket_pair(int family, int type, int protocol, int pair[2])
{ return socketpair(family,type,protocol,pair); }
static inline int luna_socket_open(int family, int type, int protocol) {
#ifdef __linux__
   return socket(family, type | SOCK_CLOEXEC, protocol);
#else
   int fd = socket(family, type, protocol);
   if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
      int error = errno; close(fd); errno = error; return -1;
   }
   return fd;
#endif
}
static inline int luna_socket_accept(int fd, struct sockaddr *address, socklen_t *length) {
#ifdef __linux__
   return accept4(fd, address, length, SOCK_CLOEXEC);
#else
   int client = accept(fd, address, length);
   if (client >= 0 && fcntl(client, F_SETFD, FD_CLOEXEC) < 0) {
      int error = errno; close(client); errno = error; return -1;
   }
   return client;
#endif
}
static inline int luna_socket_bind(int fd, const struct sockaddr *a, socklen_t n) { return bind(fd,a,n); }
static inline int luna_socket_connect(int fd, const struct sockaddr *a, socklen_t n) { return connect(fd,a,n); }
static inline int luna_socket_listen(int fd, int backlog) { return listen(fd,backlog); }
static inline int luna_socket_shutdown(int fd, int how) { return shutdown(fd,how); }
static inline int luna_socket_name(int fd, struct sockaddr *a, socklen_t *n) { return getsockname(fd,a,n); }
static inline int luna_socket_peer(int fd, struct sockaddr *a, socklen_t *n) { return getpeername(fd,a,n); }
static inline int luna_socket_set_option(int fd, int level, int option, const void *v, socklen_t n) { return setsockopt(fd,level,option,v,n); }
static inline int luna_socket_get_option(int fd, int level, int option, void *v, socklen_t *n) { return getsockopt(fd,level,option,v,n); }
static inline int luna_socket_get_nonblock(int fd) {
   int flags = fcntl(fd, F_GETFL, 0); return flags < 0 ? -1 : !!(flags & O_NONBLOCK);
}
static inline int luna_socket_nonblock(int fd, int enabled) {
   int flags = fcntl(fd, F_GETFL, 0);
   return flags < 0 ? -1 : fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
static inline int luna_socket_poll(struct pollfd *fds, size_t n, int ms) { return poll(fds,n,ms); }
static inline int luna_socket_dns(const char *node, const char *service, const struct addrinfo *h, struct addrinfo **out) { return getaddrinfo(node,service,h,out); }
static inline int luna_socket_available(int fd, int *bytes) { return ioctl(fd,FIONREAD,bytes); }
static inline ptrdiff_t luna_socket_recv(int fd, void *p, size_t n, int flags) { return recv(fd,p,n,flags); }
static inline ptrdiff_t luna_socket_send(int fd, const void *p, size_t n, int flags) { return send(fd,p,n,flags); }
static inline ptrdiff_t luna_socket_recvfrom(int fd, void *p, size_t n, int flags, struct sockaddr *a, socklen_t *length) { return recvfrom(fd,p,n,flags,a,length); }
static inline ptrdiff_t luna_socket_sendto(int fd, const void *p, size_t n, int flags, const struct sockaddr *a, socklen_t length) { return sendto(fd,p,n,flags,a,length); }
static inline ptrdiff_t luna_socket_sendmsg(int fd, const struct msghdr *m, int flags) { return sendmsg(fd,m,flags); }
static inline ptrdiff_t luna_socket_recvmsg(int fd, struct msghdr *m, int flags) { return recvmsg(fd,m,flags); }
static inline ptrdiff_t luna_fd_writev(int fd, const struct iovec *vectors, int count) { return writev(fd,vectors,count); }
static inline ptrdiff_t luna_fd_read(int fd, void *p, size_t n) { return read(fd,p,n); }
static inline ptrdiff_t luna_fd_write(int fd, const void *p, size_t n) { return write(fd,p,n); }
static inline int luna_fd_close(int fd) { return close(fd); }
static inline int luna_fd_get_nonblock(int fd) { return luna_socket_get_nonblock(fd); }
static inline int luna_fd_nonblock(int fd, int enabled) { return luna_socket_nonblock(fd, enabled); }
static inline int luna_fd_get_cloexec(int fd)
{
   int flags = fcntl(fd, F_GETFD);
   return flags < 0 ? -1 : !!(flags & FD_CLOEXEC);
}
static inline int luna_fd_dup(int fd, int minimum, int close_on_exec)
{
#ifdef F_DUPFD_CLOEXEC
   return fcntl(fd, close_on_exec ? F_DUPFD_CLOEXEC : F_DUPFD, minimum);
#else
   int result = fcntl(fd, F_DUPFD, minimum);
   if (result >= 0 && close_on_exec && fcntl(result, F_SETFD, FD_CLOEXEC) < 0) {
      int error = errno; close(result); errno = error; return -1;
   }
   return result;
#endif
}
static inline int luna_fd_dup_to(int fd, int target, int close_on_exec)
{
   if (fd == target) return fcntl(fd, F_GETFD) < 0 ? -1 : fd;
#ifdef __linux__
   return dup3(fd, target, close_on_exec ? O_CLOEXEC : 0);
#else
   int result = dup2(fd, target);
   if (result >= 0 && close_on_exec && fcntl(result, F_SETFD, FD_CLOEXEC) < 0) {
      int error = errno; close(result); errno = error; return -1;
   }
   return result;
#endif
}
static inline int luna_fd_cloexec(int fd, int enabled) {
   int flags = fcntl(fd, F_GETFD);
   return flags < 0 ? -1 : fcntl(fd, F_SETFD, enabled ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC);
}
#endif
#ifdef __cplusplus
}
#endif

/* ---- Android errno conversion ---- */
#include <errno.h>
static inline int luna_errno_to_android(int error)
{
#ifdef _WIN32
   /* Winsock errors have already been translated into CRT/POSIX names.
    * MinGW's numeric values still differ from Android/Linux. */
#define LUNA_ERRNO(name, number) if (error == name) return number
   LUNA_ERRNO(EAGAIN, 11); LUNA_ERRNO(EWOULDBLOCK, 11);
   LUNA_ERRNO(EDEADLK, 35); LUNA_ERRNO(ENAMETOOLONG, 36);
   LUNA_ERRNO(ENOLCK, 37); LUNA_ERRNO(ENOSYS, 38); LUNA_ERRNO(ENOTEMPTY, 39);
   LUNA_ERRNO(ELOOP, 40);
   LUNA_ERRNO(ENOMSG, 42); LUNA_ERRNO(EIDRM, 43);
   LUNA_ERRNO(ENOSTR, 60); LUNA_ERRNO(ENODATA, 61); LUNA_ERRNO(ETIME, 62);
   LUNA_ERRNO(ENOSR, 63); LUNA_ERRNO(ENOLINK, 67); LUNA_ERRNO(EPROTO, 71);
   LUNA_ERRNO(EBADMSG, 74); LUNA_ERRNO(EOVERFLOW, 75); LUNA_ERRNO(EILSEQ, 84);
   LUNA_ERRNO(ENOTSOCK, 88); LUNA_ERRNO(EDESTADDRREQ, 89); LUNA_ERRNO(EMSGSIZE, 90);
   LUNA_ERRNO(EPROTOTYPE, 91); LUNA_ERRNO(ENOPROTOOPT, 92); LUNA_ERRNO(EPROTONOSUPPORT, 93);
   LUNA_ERRNO(ENOTSUP, 95); LUNA_ERRNO(EOPNOTSUPP, 95); LUNA_ERRNO(EAFNOSUPPORT, 97);
   LUNA_ERRNO(EADDRINUSE, 98); LUNA_ERRNO(EADDRNOTAVAIL, 99); LUNA_ERRNO(ENETDOWN, 100);
   LUNA_ERRNO(ENETUNREACH, 101); LUNA_ERRNO(ENETRESET, 102); LUNA_ERRNO(ECONNABORTED, 103);
   LUNA_ERRNO(ECONNRESET, 104); LUNA_ERRNO(ENOBUFS, 105); LUNA_ERRNO(EISCONN, 106);
   LUNA_ERRNO(ENOTCONN, 107); LUNA_ERRNO(ETIMEDOUT, 110); LUNA_ERRNO(ECONNREFUSED, 111);
   LUNA_ERRNO(EHOSTUNREACH, 113); LUNA_ERRNO(EALREADY, 114); LUNA_ERRNO(EINPROGRESS, 115);
   LUNA_ERRNO(ECANCELED, 125); LUNA_ERRNO(EOWNERDEAD, 130); LUNA_ERRNO(ENOTRECOVERABLE, 131);
   LUNA_ERRNO(ETXTBSY, 26);
#undef LUNA_ERRNO
#endif
   return error;
}

/* Android/POSIX *at ABI; wrappers translate these on the host. */
enum { LUNA_AT_FDCWD = -100, LUNA_AT_NOFOLLOW = 0x100,
       LUNA_AT_REMOVEDIR = 0x200, LUNA_AT_EACCESS = 0x200,
       LUNA_AT_NO_AUTOMOUNT = 0x800, LUNA_AT_EMPTY_PATH = 0x1000 };
/* ---- File interfaces ---- */
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <limits.h>
/* Android timestamp sentinels, independent of the host's UTIME_* values. */
enum { LUNA_TIME_NOW = 0x3fffffff, LUNA_TIME_OMIT = 0x3ffffffe };
typedef struct luna_file_time {
   int64_t seconds;
   int64_t nanoseconds;
} luna_file_time;
/* Host metadata with wide sizes and timestamps; no platform stat layout is
 * exposed to the Android ABI writer. Blocks always count 512-byte units. */
typedef struct luna_file_info {
   uint64_t device, inode, rdevice, blocks;
   int64_t size;
   uint32_t mode, links, uid, gid, block_size;
   luna_file_time accessed, modified, changed;
} luna_file_info;
typedef struct luna_file_memstream {
   FILE *stream;
   char *data;
   size_t length;
} luna_file_memstream;
/* Android flock flags. These are independent of host locking APIs. */
enum { LUNA_LOCK_SHARED=1, LUNA_LOCK_EXCLUSIVE=2,
       LUNA_LOCK_NONBLOCK=4, LUNA_LOCK_UNLOCK=8 };
/* Process record locks use a neutral layout and Android lock type values.
 * They are different from the open-description locks used by flock/Java. */
enum { LUNA_RECORD_READ=0, LUNA_RECORD_WRITE=1, LUNA_RECORD_UNLOCK=2 };
enum { LUNA_RECORD_QUERY=0, LUNA_RECORD_SET=1, LUNA_RECORD_WAIT=2 };
typedef struct luna_file_record_lock {
   int type, whence;
   int64_t start, length;
   int32_t pid;
} luna_file_record_lock;
/* pathconf names are the Android ABI, not the host's _PC_* numbers. */
enum {
   LUNA_PC_FILESIZEBITS=0, LUNA_PC_LINK_MAX=1, LUNA_PC_MAX_CANON=2,
   LUNA_PC_MAX_INPUT=3, LUNA_PC_NAME_MAX=4, LUNA_PC_PATH_MAX=5,
   LUNA_PC_PIPE_BUF=6, LUNA_PC_2_SYMLINKS=7, LUNA_PC_ALLOC_SIZE_MIN=8,
   LUNA_PC_REC_INCR_XFER_SIZE=9, LUNA_PC_REC_MAX_XFER_SIZE=10,
   LUNA_PC_REC_MIN_XFER_SIZE=11, LUNA_PC_REC_XFER_ALIGN=12,
   LUNA_PC_SYMLINK_MAX=13, LUNA_PC_CHOWN_RESTRICTED=14, LUNA_PC_NO_TRUNC=15,
   LUNA_PC_VDISABLE=16, LUNA_PC_ASYNC_IO=17, LUNA_PC_PRIO_IO=18, LUNA_PC_SYNC_IO=19
};
#ifdef _WIN32
#define LUNA_FILE_CLOEXEC _O_NOINHERIT
#define LUNA_FILE_SYNC 0x10000000
#define LUNA_FILE_DSYNC LUNA_FILE_SYNC
#define LUNA_FILE_DIRECTORY 0x20000000
#define LUNA_FILE_NOFOLLOW 0x40000000
#define LUNA_FILE_NONBLOCK 0x08000000
#define LUNA_FILE_NOCTTY 0
#ifdef __cplusplus
extern "C" {
#endif
int luna_file_open(const char *path, int flags, unsigned mode);
int luna_file_openat(int dirfd, const char *path, int flags, unsigned mode);
int luna_file_unlinkat(int dirfd, const char *path, int flags);
int luna_file_accessat(int dirfd, const char *path, int mode, int flags);
int luna_file_mkdir(const char *path, unsigned mode);
long luna_file_pathconf(const char *path, int name);
int luna_file_chmodat(int dirfd, const char *path, unsigned mode, int flags);
int luna_file_fd_chmod(int fd, unsigned mode);
int luna_file_link(const char *from, const char *to);
int luna_file_symlink(const char *target, const char *path);
int luna_file_set_modified(const char *path, int64_t ms);
int luna_file_timesat(int dirfd, const char *path, const luna_file_time times[2], int flags);
int luna_file_fd_times(int fd, const luna_file_time times[2]);
int luna_file_infoat(int dirfd, const char *path, luna_file_info *info, int flags);
int luna_file_fd_info(int fd, luna_file_info *info);
ptrdiff_t luna_file_readlink(const char *path, char *buffer, size_t capacity);
int luna_file_rename(const char *from, const char *to);
int luna_file_unlink(const char *path);
int luna_file_temp_directory(char *result, size_t capacity);
FILE *luna_file_tmpfile(void);
FILE *luna_file_memory_reader(const void *buffer, size_t length);
char *luna_file_mkdtemp(char *pattern);
int luna_file_mkstemps(char *pattern, int suffix_length);
char *luna_file_realpath(const char *path, char *result, size_t capacity);
int luna_file_sync(int fd, int data_only);
ptrdiff_t luna_file_pread(int fd, void *buffer, size_t length, int64_t offset);
ptrdiff_t luna_file_pwrite(int fd, const void *buffer, size_t length, int64_t offset);
int luna_file_lock(int fd, int64_t start, int64_t length,
                                 int shared, int nonblocking, int unlock);
int luna_file_flock(int fd, int operation);
int luna_file_record_control(int fd, int command, luna_file_record_lock *lock);
int luna_file_fclose(FILE *stream);
FILE *luna_file_fopen(const char *path, const char *mode);
int luna_fd_get_status(int fd);
int luna_fd_set_status(int fd, int flags);
/* command is Android's integer fcntl command; status words use host flags. */
int luna_fd_control(int fd, int command, int argument);
FILE *luna_file_memstream_begin(luna_file_memstream *writer);
int luna_file_memstream_finish(luna_file_memstream *writer, char **data, size_t *length);
#ifdef __cplusplus
}
#endif

#else
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <time.h>
#define LUNA_FILE_CLOEXEC O_CLOEXEC
#define LUNA_FILE_SYNC O_SYNC
#define LUNA_FILE_DIRECTORY O_DIRECTORY
#define LUNA_FILE_NOFOLLOW O_NOFOLLOW
#define LUNA_FILE_NONBLOCK O_NONBLOCK
#define LUNA_FILE_NOCTTY O_NOCTTY
#ifdef O_DSYNC
#define LUNA_FILE_DSYNC O_DSYNC
#else
#define LUNA_FILE_DSYNC O_SYNC
#endif
static inline void luna_file_native_info(const struct stat *native, luna_file_info *info)
{
   memset(info, 0, sizeof *info);
   info->device = native->st_dev; info->inode = native->st_ino;
   info->rdevice = native->st_rdev; info->size = native->st_size;
   info->blocks = native->st_blocks; info->block_size = native->st_blksize;
   info->mode = native->st_mode; info->links = native->st_nlink;
   info->uid = native->st_uid; info->gid = native->st_gid;
#ifdef __APPLE__
   info->accessed.seconds = native->st_atimespec.tv_sec;
   info->accessed.nanoseconds = native->st_atimespec.tv_nsec;
   info->modified.seconds = native->st_mtimespec.tv_sec;
   info->modified.nanoseconds = native->st_mtimespec.tv_nsec;
   info->changed.seconds = native->st_ctimespec.tv_sec;
   info->changed.nanoseconds = native->st_ctimespec.tv_nsec;
#else
   info->accessed.seconds = native->st_atim.tv_sec;
   info->accessed.nanoseconds = native->st_atim.tv_nsec;
   info->modified.seconds = native->st_mtim.tv_sec;
   info->modified.nanoseconds = native->st_mtim.tv_nsec;
   info->changed.seconds = native->st_ctim.tv_sec;
   info->changed.nanoseconds = native->st_ctim.tv_nsec;
#endif
}
static inline int luna_file_fd_info(int fd, luna_file_info *info)
{
   if (!info) { errno = EFAULT; return -1; }
   struct stat native;
   if (fstat(fd, &native)) return -1;
   luna_file_native_info(&native, info); return 0;
}
static inline int luna_file_infoat(int dirfd, const char *path, luna_file_info *info, int flags)
{
   if (flags & ~(LUNA_AT_NOFOLLOW | LUNA_AT_NO_AUTOMOUNT | LUNA_AT_EMPTY_PATH)) { errno = EINVAL; return -1; }
   if (!path || !info) { errno = EFAULT; return -1; }
   if (!*path && (flags & LUNA_AT_EMPTY_PATH) && dirfd != LUNA_AT_FDCWD)
      return luna_file_fd_info(dirfd, info);
   struct stat native;
   const char *query = !*path && (flags & LUNA_AT_EMPTY_PATH) ? "." : path;
   int native_flags = flags & LUNA_AT_NOFOLLOW ? AT_SYMLINK_NOFOLLOW : 0;
#ifdef AT_NO_AUTOMOUNT
   if (flags & LUNA_AT_NO_AUTOMOUNT) native_flags |= AT_NO_AUTOMOUNT;
#endif
   if (fstatat(dirfd == LUNA_AT_FDCWD ? AT_FDCWD : dirfd, query, &native, native_flags)) return -1;
   luna_file_native_info(&native, info); return 0;
}
static inline ptrdiff_t luna_file_readlink(const char *path, char *buffer, size_t capacity)
{ return readlink(path, buffer, capacity); }
static inline int luna_file_open(const char *path, int flags, unsigned mode)
{ return open(path, flags, (mode_t)mode); }
static inline int luna_file_openat(int dirfd, const char *path, int flags, unsigned mode)
{ return openat(dirfd == LUNA_AT_FDCWD ? AT_FDCWD : dirfd, path, flags, (mode_t)mode); }
static inline int luna_file_unlinkat(int dirfd, const char *path, int flags)
{
   if (flags & ~LUNA_AT_REMOVEDIR) { errno = EINVAL; return -1; }
   return unlinkat(dirfd == LUNA_AT_FDCWD ? AT_FDCWD : dirfd, path,
                   flags & LUNA_AT_REMOVEDIR ? AT_REMOVEDIR : 0);
}
static inline int luna_file_accessat(int dirfd, const char *path, int mode, int flags)
{
   if (flags & ~(LUNA_AT_NOFOLLOW | LUNA_AT_EACCESS)) { errno = EINVAL; return -1; }
   int native = (flags & LUNA_AT_NOFOLLOW ? AT_SYMLINK_NOFOLLOW : 0) |
                (flags & LUNA_AT_EACCESS ? AT_EACCESS : 0);
   return faccessat(dirfd == LUNA_AT_FDCWD ? AT_FDCWD : dirfd, path, mode, native);
}
static inline int luna_file_rename(const char *from, const char *to)
{ return rename(from, to); }
static inline int luna_file_unlink(const char *path)
{ return unlink(path); }
static inline int luna_file_temp_directory(char *result, size_t capacity)
{
   const char *directory = getenv("TMPDIR");
   if (!directory || !*directory) directory = "/tmp";
   size_t length = strlen(directory) + 1;
   if (length > capacity) { errno = ENAMETOOLONG; return -1; }
   memcpy(result, directory, length); return 0;
}
static inline FILE *luna_file_tmpfile(void) { return tmpfile(); }
static inline FILE *luna_file_memory_reader(const void *buffer, size_t length)
{ return fmemopen((void *)buffer, length, "r"); }
static inline char *luna_file_mkdtemp(char *pattern) { return mkdtemp(pattern); }
static inline int luna_file_mkstemps(char *pattern, int suffix_length)
{ return mkstemps(pattern, suffix_length); }
static inline char *luna_file_realpath(const char *path, char *result, size_t capacity)
{
   char *resolved = realpath(path, NULL);
   if (!resolved) return NULL;
   size_t length = strlen(resolved) + 1;
   if (length > capacity) { free(resolved); errno = ENAMETOOLONG; return NULL; }
   memcpy(result, resolved, length); free(resolved); return result;
}
static inline int luna_file_native_times(const luna_file_time times[2], struct timespec native[2])
{
   for (int i = 0; i < 2; ++i) {
      int64_t ns = times[i].nanoseconds;
      if (ns == LUNA_TIME_NOW || ns == LUNA_TIME_OMIT) {
         native[i].tv_sec = 0;
         native[i].tv_nsec = ns == LUNA_TIME_NOW ? UTIME_NOW : UTIME_OMIT;
      } else {
         if (ns < 0 || ns >= 1000000000) { errno = EINVAL; return -1; }
         native[i].tv_sec = (time_t)times[i].seconds;
         if ((int64_t)native[i].tv_sec != times[i].seconds) { errno = EOVERFLOW; return -1; }
         native[i].tv_nsec = ns;
      }
   }
   return 0;
}
static inline int luna_file_timesat(int dirfd, const char *path, const luna_file_time times[2], int flags)
{
   if (flags & ~LUNA_AT_NOFOLLOW) { errno = EINVAL; return -1; }
   struct timespec native[2];
   if (times && luna_file_native_times(times, native)) return -1;
   return utimensat(dirfd == LUNA_AT_FDCWD ? AT_FDCWD : dirfd, path,
                    times ? native : NULL, flags & LUNA_AT_NOFOLLOW ? AT_SYMLINK_NOFOLLOW : 0);
}
static inline int luna_file_fd_times(int fd, const luna_file_time times[2])
{
   struct timespec native[2];
   if (times && luna_file_native_times(times, native)) return -1;
   return futimens(fd, times ? native : NULL);
}
static inline int luna_file_set_modified(const char *path, int64_t ms)
{
   if (ms < 0) { errno = EINVAL; return -1; }
   struct timespec times[2] = { { 0, UTIME_OMIT },
      { (time_t)(ms / 1000), (long)((ms % 1000) * 1000000) } };
   return utimensat(AT_FDCWD, path, times, 0);
}
static inline int luna_file_chmodat(int dirfd, const char *path, unsigned mode, int flags)
{
   if (flags & ~LUNA_AT_NOFOLLOW) { errno = EINVAL; return -1; }
   return fchmodat(dirfd == LUNA_AT_FDCWD ? AT_FDCWD : dirfd, path, (mode_t)mode,
                   flags & LUNA_AT_NOFOLLOW ? AT_SYMLINK_NOFOLLOW : 0);
}
static inline int luna_file_fd_chmod(int fd, unsigned mode)
{ return fchmod(fd, (mode_t)mode); }
static inline int luna_file_link(const char *from, const char *to)
{ return link(from, to); }
static inline int luna_file_symlink(const char *target, const char *path)
{ return symlink(target, path); }
static inline long luna_file_pathconf(const char *path, int name)
{
   if (!path) { errno = EINVAL; return -1; }
   if (name < 0 || name > LUNA_PC_SYNC_IO) { errno = EINVAL; return -1; }
   int native = -1;
   switch (name) {
#ifdef _PC_FILESIZEBITS
   case LUNA_PC_FILESIZEBITS: native = _PC_FILESIZEBITS; break;
#endif
#ifdef _PC_LINK_MAX
   case LUNA_PC_LINK_MAX: native = _PC_LINK_MAX; break;
#endif
#ifdef _PC_MAX_CANON
   case LUNA_PC_MAX_CANON: native = _PC_MAX_CANON; break;
#endif
#ifdef _PC_MAX_INPUT
   case LUNA_PC_MAX_INPUT: native = _PC_MAX_INPUT; break;
#endif
#ifdef _PC_NAME_MAX
   case LUNA_PC_NAME_MAX: native = _PC_NAME_MAX; break;
#endif
#ifdef _PC_PATH_MAX
   case LUNA_PC_PATH_MAX: native = _PC_PATH_MAX; break;
#endif
#ifdef _PC_PIPE_BUF
   case LUNA_PC_PIPE_BUF: native = _PC_PIPE_BUF; break;
#endif
#ifdef _PC_2_SYMLINKS
   case LUNA_PC_2_SYMLINKS: native = _PC_2_SYMLINKS; break;
#endif
#ifdef _PC_ALLOC_SIZE_MIN
   case LUNA_PC_ALLOC_SIZE_MIN: native = _PC_ALLOC_SIZE_MIN; break;
#endif
#ifdef _PC_REC_INCR_XFER_SIZE
   case LUNA_PC_REC_INCR_XFER_SIZE: native = _PC_REC_INCR_XFER_SIZE; break;
#endif
#ifdef _PC_REC_MAX_XFER_SIZE
   case LUNA_PC_REC_MAX_XFER_SIZE: native = _PC_REC_MAX_XFER_SIZE; break;
#endif
#ifdef _PC_REC_MIN_XFER_SIZE
   case LUNA_PC_REC_MIN_XFER_SIZE: native = _PC_REC_MIN_XFER_SIZE; break;
#endif
#ifdef _PC_REC_XFER_ALIGN
   case LUNA_PC_REC_XFER_ALIGN: native = _PC_REC_XFER_ALIGN; break;
#endif
#ifdef _PC_SYMLINK_MAX
   case LUNA_PC_SYMLINK_MAX: native = _PC_SYMLINK_MAX; break;
#endif
#ifdef _PC_CHOWN_RESTRICTED
   case LUNA_PC_CHOWN_RESTRICTED: native = _PC_CHOWN_RESTRICTED; break;
#endif
#ifdef _PC_NO_TRUNC
   case LUNA_PC_NO_TRUNC: native = _PC_NO_TRUNC; break;
#endif
#ifdef _PC_VDISABLE
   case LUNA_PC_VDISABLE: native = _PC_VDISABLE; break;
#endif
#ifdef _PC_ASYNC_IO
   case LUNA_PC_ASYNC_IO: native = _PC_ASYNC_IO; break;
#endif
#ifdef _PC_PRIO_IO
   case LUNA_PC_PRIO_IO: native = _PC_PRIO_IO; break;
#endif
#ifdef _PC_SYNC_IO
   case LUNA_PC_SYNC_IO: native = _PC_SYNC_IO; break;
#endif
   default: break;
   }
   if (native >= 0) return pathconf(path, native);
   luna_file_info info;
   if (luna_file_infoat(LUNA_AT_FDCWD, path, &info, 0)) return -1;
   errno = 0; return -1; /* An indeterminate limit, not an invalid query. */
}
static inline int luna_file_mkdir(const char *path, unsigned mode)
{ return mkdir(path, (mode_t)mode); }
static inline int luna_file_sync(int fd, int data_only)
{
#ifdef __APPLE__
   (void)data_only; return fsync(fd);
#else
   return data_only ? fdatasync(fd) : fsync(fd);
#endif
}
static inline ptrdiff_t luna_file_pread(int fd, void *buffer, size_t length, int64_t offset)
{ return pread(fd, buffer, length, (off_t)offset); }
static inline ptrdiff_t luna_file_pwrite(int fd, const void *buffer, size_t length, int64_t offset)
{ return pwrite(fd, buffer, length, (off_t)offset); }
static inline int luna_file_lock(int fd, int64_t start, int64_t length,
                                 int shared, int nonblocking, int unlock)
{
   if (start < 0 || length < 0 || length > INT64_MAX - start) {
      errno = EINVAL; return -1;
   }
   if (!length) return 0;
   struct flock region = { 0 };
   region.l_type = unlock ? F_UNLCK : shared ? F_RDLCK : F_WRLCK;
   region.l_whence = SEEK_SET;
   region.l_start = (off_t)start;
   region.l_len = (off_t)length;
#ifdef F_OFD_SETLK
   int command = nonblocking || unlock ? F_OFD_SETLK : F_OFD_SETLKW;
#else
   int command = nonblocking || unlock ? F_SETLK : F_SETLKW;
#endif
   return fcntl(fd, command, &region);
}
static inline int luna_file_flock(int fd, int operation)
{
   int kind = operation & ~LUNA_LOCK_NONBLOCK;
   if (kind != LUNA_LOCK_SHARED && kind != LUNA_LOCK_EXCLUSIVE &&
       kind != LUNA_LOCK_UNLOCK) { errno = EINVAL; return -1; }
   int native = kind == LUNA_LOCK_SHARED ? LOCK_SH :
                kind == LUNA_LOCK_EXCLUSIVE ? LOCK_EX : LOCK_UN;
   return flock(fd, native | (operation & LUNA_LOCK_NONBLOCK ? LOCK_NB : 0));
}
static inline int luna_file_fclose(FILE *stream) { return fclose(stream); }
static inline FILE *luna_file_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static inline int luna_fd_get_status(int fd) { return fcntl(fd, F_GETFL); }
static inline int luna_fd_set_status(int fd, int flags) { return fcntl(fd, F_SETFL, flags); }
static inline int luna_fd_control(int fd, int command, int argument)
{
   int native;
   switch (command) {
   case 3: return luna_fd_get_status(fd);
   case 4: return luna_fd_set_status(fd, argument);
   case 8: native = F_SETOWN; break;
   case 9: return fcntl(fd, F_GETOWN);
#ifdef F_SETSIG
   case 10: native = F_SETSIG; break;
#endif
#ifdef F_GETSIG
   case 11: return fcntl(fd, F_GETSIG);
#endif
#ifdef F_SETLEASE
   case 1024: native = F_SETLEASE; break;
#endif
#ifdef F_GETLEASE
   case 1025: return fcntl(fd, F_GETLEASE);
#endif
#ifdef F_NOTIFY
   case 1026: native = F_NOTIFY; break;
#endif
#ifdef F_SETPIPE_SZ
   case 1031: native = F_SETPIPE_SZ; break;
#endif
#ifdef F_GETPIPE_SZ
   case 1032: return fcntl(fd, F_GETPIPE_SZ);
#endif
#ifdef F_ADD_SEALS
   case 1033: native = F_ADD_SEALS; break;
#endif
#ifdef F_GET_SEALS
   case 1034: return fcntl(fd, F_GET_SEALS);
#endif
   default: errno = EINVAL; return -1;
   }
   return fcntl(fd, native, argument);
}
static inline int luna_file_record_control(int fd, int command, luna_file_record_lock *lock)
{
   if (!lock) { errno = EFAULT; return -1; }
   if (command < LUNA_RECORD_QUERY || command > LUNA_RECORD_WAIT ||
       lock->type < LUNA_RECORD_READ || lock->type > LUNA_RECORD_UNLOCK ||
       (lock->whence != SEEK_SET && lock->whence != SEEK_CUR && lock->whence != SEEK_END)) {
      errno = EINVAL; return -1;
   }
   struct flock native = { 0 };
   native.l_type = lock->type == LUNA_RECORD_READ ? F_RDLCK :
                   lock->type == LUNA_RECORD_WRITE ? F_WRLCK : F_UNLCK;
   native.l_whence = (short)lock->whence;
   native.l_start = (off_t)lock->start;
   native.l_len = (off_t)lock->length;
   if ((int64_t)native.l_start != lock->start || (int64_t)native.l_len != lock->length) {
      errno = EOVERFLOW; return -1;
   }
   int op = command == LUNA_RECORD_QUERY ? F_GETLK :
            command == LUNA_RECORD_SET ? F_SETLK : F_SETLKW;
   int result = fcntl(fd, op, &native);
   if (!result && command == LUNA_RECORD_QUERY) {
      lock->type = native.l_type == F_RDLCK ? LUNA_RECORD_READ :
                   native.l_type == F_WRLCK ? LUNA_RECORD_WRITE : LUNA_RECORD_UNLOCK;
      lock->whence = native.l_whence;
      lock->start = native.l_start; lock->length = native.l_len;
      lock->pid = (int32_t)native.l_pid;
   }
   return result;
}
#endif
static inline int luna_file_mkstemp(char *pattern)
{ return luna_file_mkstemps(pattern, 0); }
#ifndef _WIN32
static inline FILE *luna_file_memstream_begin(luna_file_memstream *writer)
{
   memset(writer, 0, sizeof *writer);
   writer->stream = open_memstream(&writer->data, &writer->length);

   return writer->stream;
}
/* Finish the stream and transfer the allocated, NUL-terminated bytes. */
static inline int luna_file_memstream_finish(luna_file_memstream *writer,
                                             char **data, size_t *length)
{
   if (!writer->stream) { errno = EINVAL; return -1; }
   int error = 0;
   if (ferror(writer->stream)) error = EIO;

   if (fclose(writer->stream) && !error) error = errno ? errno : EIO;
   writer->stream = NULL;
   if (error) { free(writer->data); writer->data = NULL; errno = error; return -1; }
   *data = writer->data; *length = writer->length; writer->data = NULL;
   return 0;
}
#endif

/* ---- Directory streams ---- */
enum { LUNA_DT_UNKNOWN = 0, LUNA_DT_FIFO = 1, LUNA_DT_CHR = 2,
       LUNA_DT_DIR = 4, LUNA_DT_BLK = 6, LUNA_DT_REG = 8,
       LUNA_DT_LNK = 10, LUNA_DT_SOCK = 12 };
#ifdef _WIN32
typedef struct luna_directory luna_directory;
typedef struct luna_directory_entry {
   uint64_t d_ino;
   unsigned char d_type;
   char d_name[1024];
} luna_directory_entry;
#ifdef __cplusplus
extern "C" {
#endif
luna_directory *luna_directory_open(const char *path);
/* Takes ownership of fd only on success, matching fdopendir. */
luna_directory *luna_directory_open_fd(int fd);
luna_directory_entry *luna_directory_read(luna_directory *directory);
int luna_directory_close(luna_directory *directory);
void luna_directory_rewind(luna_directory *directory);
#ifdef __cplusplus
}
#endif
#else
#include <dirent.h>
typedef DIR luna_directory;
typedef struct dirent luna_directory_entry;
static inline luna_directory *luna_directory_open(const char *path) { return opendir(path); }
static inline luna_directory *luna_directory_open_fd(int fd) { return fdopendir(fd); }
static inline luna_directory_entry *luna_directory_read(luna_directory *directory) { return readdir(directory); }
static inline int luna_directory_close(luna_directory *directory) { return closedir(directory); }
static inline void luna_directory_rewind(luna_directory *directory) { rewinddir(directory); }
#endif

/* ---- Android socket ABI ---- */
#include <errno.h>
#include <string.h>

enum { LUNA_ANDROID_MSG_CTRUNC = 8, LUNA_ANDROID_MSG_DONTWAIT = 0x40,
       LUNA_ANDROID_MSG_ERRQUEUE = 0x2000, LUNA_ANDROID_MSG_NOSIGNAL = 0x4000 };
/* Scheduler-owned guest sockets are nonblocking on the host. DONTWAIT is
 * handled by the guest scheduler rather than a Windows message flag. */
static inline int luna_message_flags_from_android(int flags, int *native)
{
#ifdef _WIN32
   if (flags & LUNA_ANDROID_MSG_ERRQUEUE) { errno = EAGAIN; return -1; }
   if (flags & ~0x4147) { errno = EOPNOTSUPP; return -1; }
   *native = 0;
   if (flags & 1) *native |= MSG_OOB;
   if (flags & 2) *native |= MSG_PEEK;
   if (flags & 4) *native |= MSG_DONTROUTE;
   if (flags & 0x100) *native |= MSG_WAITALL;
#else
   *native = flags;
#endif
   return 0;
}
static inline int luna_message_flags_to_android(int native)
{
#ifdef _WIN32
   int flags = 0;
   if (native & MSG_OOB) flags |= 1;
   if (native & MSG_CTRUNC) flags |= 8;
   if (native & MSG_TRUNC) flags |= 32;
   return flags;
#else
   return native;
#endif
}
static inline ptrdiff_t luna_android_socket_send(int fd, const void *buffer, size_t length, int flags)
{
   int native; if (luna_message_flags_from_android(flags, &native)) return -1;
   return luna_socket_send(fd, buffer, length, native);
}
static inline ptrdiff_t luna_android_socket_recv(int fd, void *buffer, size_t length, int flags)
{
   int native; if (luna_message_flags_from_android(flags, &native)) return -1;
   return luna_socket_recv(fd, buffer, length, native);
}
static inline ptrdiff_t luna_android_socket_sendto(int fd, const void *buffer, size_t length,
                                                  int flags, const struct sockaddr *address, socklen_t size)
{
   int native; if (luna_message_flags_from_android(flags, &native)) return -1;
   return luna_socket_sendto(fd, buffer, length, native, address, size);
}
static inline ptrdiff_t luna_android_socket_recvfrom(int fd, void *buffer, size_t length,
                                                    int flags, struct sockaddr *address, socklen_t *size)
{
   int native; if (luna_message_flags_from_android(flags, &native)) return -1;
   return luna_socket_recvfrom(fd, buffer, length, native, address, size);
}
static inline ptrdiff_t luna_android_socket_sendmsg(int fd, const struct msghdr *message, int flags)
{
   int native; if (luna_message_flags_from_android(flags, &native)) return -1;
   return luna_socket_sendmsg(fd, message, native);
}
static inline ptrdiff_t luna_android_socket_recvmsg(int fd, struct msghdr *message, int flags)
{
   int native; if (luna_message_flags_from_android(flags, &native)) return -1;
   ptrdiff_t count = luna_socket_recvmsg(fd, message, native);
   if (count >= 0) message->msg_flags = luna_message_flags_to_android(message->msg_flags);
   return count;
}

/* Android pollfd is always { int32 fd; int16 events; int16 revents; }.
 * Winsock uses a pointer-sized SOCKET and different event bits. */
static inline short luna_poll_events_from_android(uint16_t events)
{
   short result = 0;
   if (events & 0x0001) result |= POLLIN;
   if (events & 0x0002) result |= POLLPRI;
   if (events & 0x0004) result |= POLLOUT;
#ifdef POLLRDNORM
   if (events & 0x0040) result |= POLLRDNORM;
#endif
#ifdef POLLRDBAND
   if (events & 0x0080) result |= POLLRDBAND;
#endif
#ifdef POLLWRNORM
   if (events & 0x0100) result |= POLLWRNORM;
#endif
#ifdef POLLWRBAND
   if (events & 0x0200) result |= POLLWRBAND;
#endif
#ifdef POLLRDHUP
   if (events & 0x2000) result |= POLLRDHUP;
#endif
   return result;
}
static inline uint16_t luna_poll_events_to_android(short events, uint16_t requested)
{
   uint16_t result = 0;
   if (events & POLLIN) result |= 0x0001;
   if (events & POLLPRI) result |= 0x0002;
   if (events & POLLOUT) result |= 0x0004;
   if (events & POLLERR) result |= 0x0008;
   if (events & POLLHUP) result |= 0x0010;
   if (events & POLLNVAL) result |= 0x0020;
#ifdef POLLRDNORM
   if (events & POLLRDNORM) result |= 0x0040;
#endif
#ifdef POLLRDBAND
   if (events & POLLRDBAND) result |= 0x0080;
#endif
#ifdef POLLWRNORM
   if (events & POLLWRNORM) result |= 0x0100;
#endif
#ifdef POLLWRBAND
   if (events & POLLWRBAND) result |= 0x0200;
#endif
#ifdef POLLRDHUP
   if (events & POLLRDHUP) result |= 0x2000;
#endif
   return result & (uint16_t)(requested | 0x0038);
}
static inline void luna_poll_load_android(struct pollfd *host, const void *guest, size_t count,
                                          uint16_t *requests)
{
   const unsigned char *bytes = (const unsigned char *)guest;
   for (size_t i = 0; i < count; ++i) {
      int32_t fd; uint16_t events;
      memcpy(&fd, bytes + i * 8, sizeof fd);
      memcpy(&events, bytes + i * 8 + 4, sizeof events);
      requests[i] = events;
      host[i].fd = (intptr_t)fd;
      host[i].events = luna_poll_events_from_android(events);
      host[i].revents = 0;
   }
}
static inline void luna_poll_store_android(void *guest, const struct pollfd *host, size_t count,
                                           const uint16_t *requests)
{
   unsigned char *bytes = (unsigned char *)guest;
   for (size_t i = 0; i < count; ++i) {
      uint16_t result = luna_poll_events_to_android(host[i].revents, requests[i]);
      memcpy(bytes + i * 8 + 6, &result, sizeof result);
   }
}

/* Host local time without a shared static result buffer. */
#include <time.h>
static inline struct tm *luna_localtime(const time_t *value, struct tm *result)
{
#ifdef _WIN32
   return localtime_s(result, value) ? NULL : result;
#else
   return localtime_r(value, result);
#endif
}

/* ---- String helpers ---- */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Split in place, preserving empty fields like BSD strsep. */
static inline char *luna_strsep(char **cursor, const char *delimiters)
{
   char *field = *cursor;
   if (!field) return NULL;
   char *end = field + strcspn(field, delimiters);
   if (*end) { *end = 0; *cursor = end + 1; }
   else *cursor = NULL;
   return field;
}

static inline const void *luna_memmem(const void *buffer, size_t length,
                                    const void *pattern, size_t pattern_length)
{
   const unsigned char *bytes = (const unsigned char *)buffer;
   if (!pattern_length) return buffer;
   if (pattern_length > length) return NULL;
   for (size_t i = 0; i <= length - pattern_length; ++i)
      if (!memcmp(bytes + i, pattern, pattern_length)) return bytes + i;
   return NULL;
}

static inline const char *luna_strcasestr(const char *text, const char *pattern)
{
   if (!*pattern) return text;
   for (; *text; ++text) {
      size_t i = 0;
      while (pattern[i] && text[i] &&
             tolower((unsigned char)pattern[i]) == tolower((unsigned char)text[i])) ++i;
      if (!pattern[i]) return text;
   }
   return NULL;
}

static inline char *luna_strndup(const char *text, size_t limit)
{
   size_t length = 0;
   while (length < limit && text[length]) ++length;
   if (length == (size_t)-1) return NULL;
   char *copy = (char *)malloc(length + 1);
   if (copy) { memcpy(copy, text, length); copy[length] = 0; }
   return copy;
}

/* ---- Guest run queue ---- */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One entry per guest tid (1..255). External scheduler lock protects this
 * queue. Value copies are independent snapshots, including membership. */
struct luna_run_queue {
    uint8_t next[256], prev[256], present[256];
    uint8_t head, tail;
    size_t count;
};
static inline void luna_run_queue_clear(struct luna_run_queue *q)
{ memset(q, 0, sizeof *q); }
static inline void luna_run_queue_remove(struct luna_run_queue *q, uint8_t tid)
{
    if (!tid || !q->present[tid]) return;
    uint8_t before = q->prev[tid], after = q->next[tid];
    if (before) q->next[before] = after; else q->head = after;
    if (after) q->prev[after] = before; else q->tail = before;
    q->present[tid] = q->next[tid] = q->prev[tid] = 0;
    --q->count;
}
static inline void luna_run_queue_push(struct luna_run_queue *q, uint8_t tid, int urgent)
{
    if (!tid) return;
    luna_run_queue_remove(q, tid);
    q->present[tid] = 1;
    if (urgent) {
        q->next[tid] = q->head;
        if (q->head) q->prev[q->head] = tid; else q->tail = tid;
        q->head = tid;
    } else {
        q->prev[tid] = q->tail;
        if (q->tail) q->next[q->tail] = tid; else q->head = tid;
        q->tail = tid;
    }
    ++q->count;
}
static inline uint8_t luna_run_queue_pop(struct luna_run_queue *q)
{
    uint8_t tid = q->head;
    luna_run_queue_remove(q, tid);
    return tid;
}
static inline size_t luna_run_queue_copy(const struct luna_run_queue *q, uint8_t out[256])
{
    size_t n = 0;
    uint8_t tid;
    for (tid = q->head; tid; tid = q->next[tid]) out[n++] = tid;
    return n;
}

#ifdef __cplusplus
extern "C" {
#endif

struct tm;
/* Local conversions use the installed TZ; UTC conversions ignore it. */
int luna_os_time_break(int64_t seconds, int local, struct tm *result, int64_t *offset);
int luna_os_time_make(struct tm *value, int64_t *seconds, int64_t *offset);

/* Android's single C.UTF-8 locale, with C as a host fallback.
 * A NULL use() argument restores the host global locale. */
typedef void *luna_os_locale;
luna_os_locale luna_os_locale_new(void);
luna_os_locale luna_os_locale_clone(luna_os_locale locale);
void luna_os_locale_free(luna_os_locale locale);
int luna_os_locale_use(luna_os_locale locale);

/* A byte pipe; fds[0] reads and fds[1] writes. */
int luna_os_pipe_open(int fds[2], int nonblocking, int close_on_exec);

/* POSIX 48-bit generator used by the guest libc. */
uint32_t luna_os_lrand48(void);
void luna_os_srand48(int32_t seed);

/* ---- virtual memory ----------------------------------------------------
 *
 * The guest's address space is the host's: a guest VA is a host VA, so the
 * emulator has to be able to take a specific range of its own address space
 * and to be told when it cannot.  That last part is what `exact` is for —
 * "give me this address or fail", never "give me something nearby", because
 * something nearby is a guest pointer that no longer means what it says. */

#define LUNA_PROT_NONE   0
#define LUNA_PROT_READ   1
#define LUNA_PROT_WRITE  2
#define LUNA_PROT_EXEC   4

/* Reserve `len` bytes of readable/writable anonymous space.  `want` is the
 * address asked for (NULL: anywhere).  With `exact` set the call fails rather
 * than relocating.  Pages are not committed until touched, which is what
 * makes a multi-gigabyte guest heap cost nothing until the guest uses it.
 * Returns NULL on failure. */
void *luna_os_reserve(void *want, size_t len, int exact);

/* Give a range back.  Safe on a sub-range of an earlier reservation. */
int luna_os_release(void *addr, size_t len);

/* Change protection on a range.  `prot` is a mask of LUNA_PROT_*. */
int luna_os_protect(void *addr, size_t len, int prot);

/* Map `len` bytes of `fd` at `off` — the emulator's file-backed guest
 * mappings, so the page cache is shared with every other reader of the file
 * instead of being copied into the guest's heap.  `fixed` demands `want`. */
void *luna_os_map_file(void *want, size_t len, int prot, int fd,
                       uint64_t off, int fixed);

/* Guest mapping policy, independent of the host's mmap constants.  Values
 * follow Android so a syscall can pass its policy without host headers. */
#define LUNA_MAP_SHARED     0x000001
#define LUNA_MAP_PRIVATE    0x000002
#define LUNA_MAP_FIXED      0x000010
#define LUNA_MAP_NORESERVE  0x004000
#define LUNA_MAP_POPULATE   0x008000
#define LUNA_MAP_STACK      0x020000
#define LUNA_MAP_NOREPLACE  0x100000
/* Anonymous guest pages stay host-RW; guest protections live in the VMA
 * table. replace requires the requested address, replacing an old mapping
 * where supported. Neither function returns a MAP_FAILED sentinel. */
void *luna_os_map_anon(void *want, size_t len, int replace);
void *luna_os_map_file_flags(void *want, size_t len, int prot, int flags,
                            int fd, uint64_t off);

/* Grow or shrink an existing anonymous mapping in place where the host can,
 * and otherwise report that it cannot (returns NULL, leaving the old mapping
 * untouched) so the caller can copy.  Only Linux has the in-place form. */
void *luna_os_remap(void *addr, size_t old_len, size_t new_len);

size_t luna_os_page_size(void);
/* One low-bit residency result per host page in [addr, addr + len).
 * addr must be host-page aligned; an unmapped page fails with ENOMEM. */
int luna_os_residency(void *addr, size_t len, unsigned char *vector);
/* Lock committed pages, or unlock them when unlock is nonzero. */
int luna_os_memory_lock(void *addr, size_t len, int unlock);
/* Advice uses Android/Linux MADV_* values, regardless of host constants. */
int luna_os_memory_advise(void *addr, size_t len, int advice);
#define LUNA_MS_ASYNC 1
#define LUNA_MS_INVALIDATE 2
#define LUNA_MS_SYNC 4
int luna_os_memory_sync(void *addr, size_t len, int flags);

/* ---- anonymous shared memory -------------------------------------------
 *
 * ASharedMemory_create: a nameless, resizable, mappable object identified by
 * a descriptor.  The name is a debugging label on every platform, not an
 * identity — two calls with the same name are two different objects. */
int luna_os_shm_create(const char *name, size_t size);

/* ---- descriptors --------------------------------------------------------
 *
 * A guest that has a descriptor and wants to know what it was opened from
 * (Mono's file maps, the guest's own /proc/self/fd emulation).  Writes a NUL
 * terminated path and returns 0, or returns -1 when the host cannot say. */
int luna_os_fd_path(int fd, char *buf, size_t bufsz);

/* Absolute path of the running executable.  Runtime libraries are located
 * relative to this, rather than relative to a process working directory that
 * a launcher or Finder is free to choose. */
int luna_os_executable_path(char *buf, size_t bufsz);

/* True when a dynamic-loader address belongs to executable code.  Guest
 * calls need an ABI bridge; host data symbols must never receive one. */
int luna_os_symbol_is_function(void *address);

/* Host libraries only; guest ELF loading stays in the emulator.  A NULL
 * handle in library_symbol searches the loaded host modules.  Open returns
 * NULL on failure; close accepts only handles returned by open. */
void *luna_os_library_open(const char *path);
/* Eager loading with private symbol scope for optional native codecs. */
void *luna_os_library_open_local(const char *path);
const char *luna_os_library_error(void);
/* Host launch configuration. Windows follows its CRT convention: setting
 * an empty value removes that variable. Guest environ is managed separately. */
int luna_os_setenv(const char *name, const char *value, int overwrite);
/* Remove a host launch variable without changing guest environ. */
int luna_os_unsetenv(const char *name);
/* Nonblocking external command input: POSIX FIFO or Windows named pipe.
 * Windows paths use \\.\pipe\NAME. Open returns -1 on failure. */
intptr_t luna_os_command_pipe_open(const char *path);
ptrdiff_t luna_os_command_pipe_read(intptr_t pipe, void *buffer, size_t capacity);
void luna_os_command_pipe_close(intptr_t pipe);

/* Fill with bytes from the host's cryptographic random source. */
int luna_os_random(void *buffer, size_t length);
void *luna_os_library_symbol(void *handle, const char *name);
/* Search one already-loaded host module without loading a new GL driver. */
void *luna_os_library_loaded_symbol(const char *module, const char *name);

void luna_os_library_close(void *handle);

/* Fatal-error diagnostics for the host stack.  Capture returns the number
 * of addresses written; print writes them to stderr (symbolized where the
 * host supports it) without allocating a returned string array. */
int luna_os_backtrace(void **frames, int capacity);
void luna_os_backtrace_print(void *const *frames, int count);

/* A descriptor that becomes readable when something signals it — the object
 * behind ALooper_wake and the guest's own eventfd.  Counting semantics:
 * every write adds, a read drains.  Returns -1 on failure. */
int luna_os_event_open(unsigned initval, int nonblock);
int luna_os_event_signal(int fd, uint64_t count);
int luna_os_event_drain(int fd, uint64_t *out);

/* epoll's guest-visible contract, backed by epoll on Linux and kqueue on
 * macOS.  Event bits deliberately use Linux/Android's numeric values because
 * they are copied to and from the guest ABI unchanged. */
typedef struct luna_os_poll_event {
   uint32_t events;
   uint64_t data;
} luna_os_poll_event;

int luna_os_poll_create(int cloexec);
int luna_os_poll_ctl(int pollfd, int op, int fd,
                     const luna_os_poll_event *event);
int luna_os_poll_wait(int pollfd, luna_os_poll_event *events,
                      int max_events, int timeout_ms);

/* signalfd where the host has it.  `mask` is copied into the host's sigset_t;
 * unsupported hosts return -1/ENOSYS, never a descriptor that cannot deliver
 * the records read() promises. */
int luna_os_signal_fd(int fd, const void *mask, size_t mask_size, int flags);

/* ---- threads and CPUs ---------------------------------------------------
 *
 * How many CPUs the *host* has, which is what sizes the engine pool.  What
 * the guest is told it has is a separate number the emulator decides. */
int luna_os_cpu_count(void);

/* Name the calling thread, for the host's own debuggers and profilers. */
void luna_os_thread_name(const char *name);

/* Give up the rest of this thread's slice. */
void luna_os_yield(void);

/* ---- time ---------------------------------------------------------------
 *
 * Monotonic is the emulator's own clock: slices, frame budgets, timeouts.
 * Realtime is the wall clock the guest asks for. */
uint64_t luna_os_monotonic_ns(void);
uint64_t luna_os_realtime_ns(void);

/* ---- what the machine has ----------------------------------------------
 *
 * The guest sizes its caches and its texture pools from these, so a wrong
 * answer here is a guest that either thrashes or reserves memory the host
 * does not have.  Both return 0 on success. */
int luna_os_mem_info(uint64_t *total_bytes, uint64_t *avail_bytes);
/* Peak host resident memory, in Android/Linux's KiB units. */
uint64_t luna_os_peak_rss_kb(void);
int luna_os_disk_info(const char *path, uint64_t *total_bytes,
                      uint64_t *free_bytes, uint64_t *block_size);

/* Host filesystem counters, copied into the guest's own ABI by the core.
 * A host without inode quotas reports zero for the inode counters. */
typedef struct luna_os_fs_info {
   uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
   uint64_t f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
} luna_os_fs_info;
int luna_os_statfs(const char *path, luna_os_fs_info *info);
int luna_os_fstatfs(int fd, luna_os_fs_info *info);

/* ---- the window ---------------------------------------------------------
 *
 * The platform handles behind the GLFW window, for the two things GLFW does
 * not cover: the system input method and clipboard round-trips that need the
 * window's own event queue.  `glfw_window` is the GLFWwindow*; both return
 * NULL when the platform has no such handle. */
void *luna_os_native_display(void);
void *luna_os_native_window(void *glfw_window);

/* A second native window of the same kind as `glfw_window`'s, `w` x `h`,
 * that is never shown.  The compositor owns the visible window; the guest's
 * EGL window surface is created on this one, so the frame it finishes is a
 * window back buffer the compositor can copy out (a pbuffer is not readable
 * on every host EGL).  NULL where the platform has no such thing; the caller
 * then falls back to a pbuffer. */
void *luna_os_offscreen_window(void *glfw_window, int w, int h);
/* Resize an existing native offscreen drawable, on the window pump thread. */
void luna_os_offscreen_resize(void *native_window, int w, int h);

/* ---- audio out ----------------------------------------------------------
 *
 * One PCM playback stream, which is what the guest's OpenSL ES / AAudio
 * buffer queue turns into.  16-bit signed interleaved, because that is what
 * an Android audio track carries and what every mixer in the guest already
 * produces.
 *
 * The write is *not* allowed to block: it is called from the SVC that the
 * guest's audio thread is inside, and that thread also holds the emulator's
 * execution lock.  So the platform keeps a small ring and a thread of its own
 * to hand it to the device; a write that finds the ring full drops the
 * newest frames and says how many it took, which is a glitch — the honest
 * outcome when the guest produces faster than the card consumes.
 *
 * Returns: open 0 on success (the stream is silent but harmless otherwise),
 * write the number of frames accepted, queued_frames what is still to play. */
int      luna_os_audio_open(unsigned rate, unsigned channels);
int      luna_os_audio_write(const void *pcm16, unsigned frames);
unsigned luna_os_audio_queued_frames(void);
uint64_t luna_os_audio_played_frames(void);
void     luna_os_audio_close(void);

/* Output controls for the emulator's menu.  Volume is a linear gain 0..1
 * applied as the stream is handed to the device; mute silences it without
 * stopping it (the guest's buffer queue keeps its clock).  Devices are the
 * host's playback endpoints: *names* are what select_device takes, *descs*
 * what a person reads.  Selecting reopens the stream on that device, from the
 * stream's own thread; NULL or "" is the platform default.  Returns the count
 * written / 0 on success. */
void     luna_os_audio_set_volume(float gain);
float    luna_os_audio_volume(void);
void     luna_os_audio_set_muted(int muted);
int      luna_os_audio_muted(void);
int      luna_os_audio_devices(char (*names)[128], char (*descs)[128], int max);
int      luna_os_audio_select_device(const char *name);
const char *luna_os_audio_device(void);

#ifdef _WIN32
struct tm;
int luna_calendar_break(int64_t ms, const char *zone, struct tm *tm,
                        int32_t *offset, char *name, int32_t name_size);
int luna_calendar_assemble(const struct tm *tm, const char *zone,
                           int64_t sub_ms, int64_t *result);
int luna_calendar_timezone(const char *zone, int64_t ms, int32_t *west,
                           int *daylight, char names[2][32]);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LUNARIA_OS_H */
