/* Host sockets used by the Android class library.  Linux can set CLOEXEC
 * atomically; POSIX hosts without SOCK_CLOEXEC/accept4 set the descriptor
 * flag immediately after creation and close on failure. */
#ifndef LUNARIA_DVM_HOST_SOCKET_H
#define LUNARIA_DVM_HOST_SOCKET_H

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static inline int host_socket_cloexec(int family, int type, int protocol)
{
#if defined(__linux__)
   return socket(family, type | SOCK_CLOEXEC, protocol);
#else
   int fd = socket(family, type, protocol);
   if (fd < 0) return -1;
   int flags = fcntl(fd, F_GETFD);
   if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
      int e = errno;
      close(fd);
      errno = e;
      return -1;
   }
   return fd;
#endif
}

static inline int host_accept_cloexec(int listener, struct sockaddr *peer,
                                       socklen_t *length)
{
#if defined(__linux__)
   return accept4(listener, peer, length, SOCK_CLOEXEC);
#else
   int fd = accept(listener, peer, length);
   if (fd < 0) return -1;
   int flags = fcntl(fd, F_GETFD);
   if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
      int e = errno;
      close(fd);
      errno = e;
      return -1;
   }
   return fd;
#endif
}

#endif
