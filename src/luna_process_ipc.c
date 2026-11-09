/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Process isolation uses exec, never fork of a live threaded VM.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lunaria_os.h"
#include "luna_google.h"
#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static char state_directory[4096], state_path[4096];
static int state_fd = -1;
static pid_t state_owner;
static bool state_cleanup_registered;
static struct luna_process_info state_current;
static void state_close(void)
{
  if (state_fd >= 0) {
    if (state_owner==getpid()) unlink(state_path);
    close(state_fd);
    state_fd=-1;
  }
}
static void state_cleanup(void)
{
  pthread_mutex_lock(&state_lock);
  state_close();
  pthread_mutex_unlock(&state_lock);
}
int luna_process_state_open(const char *directory)
{
  if (!directory || !*directory || strlen(directory)>=sizeof state_directory) return -EINVAL;
  pthread_mutex_lock(&state_lock);
  state_close();
  snprintf(state_directory,sizeof state_directory,"%s",directory);
  for (char *p=state_directory+1; *p; ++p) if (*p=='/') {
      *p=0;
      int rc=mkdir(state_directory,0700);
      *p='/';
      if (rc && errno!=EEXIST) {
        int error=errno;
        state_directory[0]=0;
        pthread_mutex_unlock(&state_lock);
        return -error;
      }
    }
  int rc=mkdir(state_directory,0700);
  if (rc && errno!=EEXIST) {
    int error=errno;
    state_directory[0]=0;
    pthread_mutex_unlock(&state_lock);
    return -error;
  }
  if (!state_cleanup_registered) {
    atexit(state_cleanup);
    state_cleanup_registered=true;
  }
  memset(&state_current,0,sizeof state_current);
  pthread_mutex_unlock(&state_lock);
  return 0;
}
int luna_process_state_publish(const struct luna_process_info *info)
{
  if (!info || !info->pid || !memchr(info->name,0,sizeof info->name) ||
      !memchr(info->package,0,sizeof info->package)) return -EINVAL;
  pthread_mutex_lock(&state_lock);
  if (!state_directory[0]) {
    pthread_mutex_unlock(&state_lock);
    return -ENOTCONN;
  }
  if (state_fd>=0 && state_owner!=getpid()) state_close();
  if (state_fd>=0 && !memcmp(&state_current,info,sizeof *info)) {
    pthread_mutex_unlock(&state_lock);
    return 0;
  }
  bool created=state_fd<0;
  if (created) {
    /* Publish the directory entry only after the file is locked and filled.
     * Otherwise a concurrent reader could remove the not-yet-locked file. */
    if (snprintf(state_path,sizeof state_path,"%s/.process-XXXXXX",state_directory)>=(int)sizeof state_path) {
      pthread_mutex_unlock(&state_lock);
      return -ENAMETOOLONG;
    }
    state_fd=mkstemp(state_path);
    state_owner=getpid();
    if (state_fd>=0 && (fcntl(state_fd,F_SETFD,FD_CLOEXEC)<0 || flock(state_fd,LOCK_EX|LOCK_NB)<0)) {
      int error=errno;
      state_close();
      pthread_mutex_unlock(&state_lock);
      return -error;
    }
  }
  int rc=state_fd<0 ? -errno : 0;
  if (!rc && pwrite(state_fd,info,sizeof *info,0)!=(ssize_t)sizeof *info) rc=-EIO;
  if (!rc && created) {
    char published[sizeof state_path];
    size_t prefix=strlen(state_directory)+1;
    memcpy(published,state_path,prefix);
    strcpy(published+prefix,state_path+prefix+1);
    if (renameat2(AT_FDCWD,state_path,AT_FDCWD,published,RENAME_NOREPLACE)) rc=-errno;
    else strcpy(state_path,published);
  }
  if (rc && created) state_close();
  if (!rc) state_current=*info;
  pthread_mutex_unlock(&state_lock);
  return rc;
}
int luna_process_state_list(struct luna_process_info **out,size_t *count)
{
  if (!out || !count) return -EINVAL;
  *out=NULL;
  *count=0;
  pthread_mutex_lock(&state_lock);
  DIR *directory=*state_directory ? opendir(state_directory) : NULL;
  if (!directory) {
    pthread_mutex_unlock(&state_lock);
    return -ENOTCONN;
  }
  struct dirent *entry;
  int result=0;
  size_t capacity=0;
  while ((entry=readdir(directory))) {
    if (strncmp(entry->d_name,"process-",8)) continue;
    int fd=openat(dirfd(directory),entry->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) continue;
    struct stat st;
    struct luna_process_info info;
    if (!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==(off_t)sizeof info) {
      if (!flock(fd,LOCK_EX|LOCK_NB)) {
        /* Kernel locks disappear even after a crash. A PID alone cannot
         * establish liveness because the host may have reused it. */
        unlinkat(dirfd(directory),entry->d_name,0);
      } else if ((errno==EWOULDBLOCK || errno==EAGAIN) &&
                 pread(fd,&info,sizeof info,0)==(ssize_t)sizeof info && info.pid &&
                 memchr(info.name,0,sizeof info.name) && memchr(info.package,0,sizeof info.package)) {
        if (*count==capacity) {
          size_t next=capacity ? capacity*2 : 16;
          struct luna_process_info *items=realloc(*out,next*sizeof info);
          if (!items) {
            result=-ENOMEM;
            close(fd);
            break;
          }
          *out=items;
          capacity=next;
        }
        (*out)[(*count)++]=info;
      }
    }
    close(fd);
  }
  closedir(directory);
  pthread_mutex_unlock(&state_lock);
  if (result) {
    free(*out);
    *out=NULL;
    *count=0;
  }
  return result;
}
#define QUEUE_MAX (8u * LUNA_PROCESS_MESSAGE_MAX)
#define PID_STRIDE 4096u
#define PID_LIMIT 4194304u
#define NAME_MAX_BYTES 511u
struct packet {
  struct packet *next;
  size_t size,offset,nfds;
  int *fds;
  unsigned char bytes[];
};
struct luna_process_channel {
  pthread_mutex_t lock;
  int fd,error;
  struct packet *head,*tail;
  size_t queued;
  unsigned char header[8];
  size_t header_size,received;
  struct packet *incoming;
  struct packet *ready_head,*ready_tail;
  int incoming_fds[LUNA_PROCESS_FD_MAX];
  size_t incoming_nfds;
};
struct child {
  struct child *next;
  char *name;
  pid_t host_pid;
  uint32_t guest_pid;
  struct luna_process_channel *channel;
  bool dead,drained;
  int status;
};
struct exit_notice {
  struct exit_notice *next;
  char *name;
  uint32_t pid;
  int status;
};
struct luna_process_pool {
  pthread_mutex_t lock;
  char *executable,*mode,*host_apk,*profile;
  uint32_t parent_pid,next_pid,guest_uid;
  struct child *children;
  struct exit_notice *exits,*exits_tail;
  struct reply *ready_head,*ready_tail;
  int error;
};
static void put32(unsigned char *p,uint32_t v)
{
  for(unsigned i=0; i<4; ++i)p[i]=(unsigned char)(v>>(i*8));
}
static uint32_t get32(const unsigned char *p)
{
  return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
struct fd_scope {
  const int *fds;
  size_t count;
};
static _Thread_local struct fd_scope current_fds;
const int *luna_process_message_fds(size_t *count)
{
  if(count)*count=current_fds.count;
  return current_fds.fds;
}
static void fds_free(int *fds,size_t count)
{
  for(size_t i=0; i<count; ++i)close(fds[i]);
  free(fds);
}
static int fds_copy(const int *fds,size_t count,int **out)
{
  *out=NULL;
  if(count>LUNA_PROCESS_FD_MAX || (count && !fds))return -EINVAL;
  if(!count)return 0;
  int *copy=malloc(count*sizeof *copy);
  if(!copy)return -ENOMEM;
  for(size_t i=0; i<count; ++i) {
    copy[i]=fcntl(fds[i],F_DUPFD_CLOEXEC,0);
    if(copy[i]<0) {
      int error=errno;
      fds_free(copy,i);
      return -error;
    }
  }
  *out=copy;
  return 0;
}
static void packet_free(struct packet *p)
{
  if(p) {
    fds_free(p->fds,p->nfds);
    free(p);
  }
}
static void packets_free(struct packet *p)
{
  while(p) {
    struct packet *next=p->next;
    packet_free(p);
    p=next;
  }
}
struct luna_process_channel *luna_process_channel_open(int fd)
{
  if(fd<0) {
    errno=EBADF;
    return NULL;
  }
  int flags=fcntl(fd,F_GETFL),cloexec=fcntl(fd,F_GETFD);
  if(flags<0 || cloexec<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 ||
      fcntl(fd,F_SETFD,cloexec|FD_CLOEXEC)<0) {
    int e=errno;
    close(fd);
    errno=e;
    return NULL;
  }
  struct luna_process_channel *c=calloc(1,sizeof *c);
  if(!c) {
    close(fd);
    return NULL;
  }
  int e=pthread_mutex_init(&c->lock,NULL);
  if(e) {
    close(fd);
    free(c);
    errno=e;
    return NULL;
  }
  c->fd=fd;
  return c;
}
static int flush(struct luna_process_channel *c)
{
  while(c->head) {
    struct packet *p=c->head;
    struct iovec vec= {p->bytes+p->offset,p->size-p->offset};
    struct msghdr msg= {0};
    msg.msg_iov=&vec;
    msg.msg_iovlen=1;
    union {
      struct cmsghdr align;
      unsigned char bytes[CMSG_SPACE(LUNA_PROCESS_FD_MAX*sizeof(int))];
    } control;
    if(p->nfds) {
      msg.msg_control=control.bytes;
      msg.msg_controllen=CMSG_SPACE(p->nfds*sizeof(int));
      memset(control.bytes,0,msg.msg_controllen);
      struct cmsghdr *h=CMSG_FIRSTHDR(&msg);
      h->cmsg_level=SOL_SOCKET;
      h->cmsg_type=SCM_RIGHTS;
      h->cmsg_len=CMSG_LEN(p->nfds*sizeof(int));
      memcpy(CMSG_DATA(h),p->fds,p->nfds*sizeof(int));
    }
    ssize_t n=sendmsg(c->fd,&msg,MSG_NOSIGNAL);
    if(n<0 && errno==EINTR)continue;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
    if(n<=0)return -(n<0 ? errno : EPIPE);
    /* Rights accompany the first byte only, including a partial send. */
    fds_free(p->fds,p->nfds);
    p->fds=NULL;
    p->nfds=0;
    p->offset+=(size_t)n;
    c->queued-=(size_t)n;
    if(p->offset==p->size) {
      c->head=p->next;
      if(!c->head)c->tail=NULL;
      free(p);
    }
  }
  return 0;
}
int luna_process_channel_send_fds(struct luna_process_channel *c,const void *data,size_t size,const int *fds,size_t nfds)
{
  if(!c || (!data && size) || (nfds && !fds) || nfds>LUNA_PROCESS_FD_MAX)return -EINVAL;
  if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
  pthread_mutex_lock(&c->lock);
  int rc=c->error ? -c->error : 0;
  if(!rc && c->queued+size+8>QUEUE_MAX)rc=-ENOBUFS;
  struct packet *p=rc ? NULL : malloc(sizeof *p+size+8);
  if(!rc && !p)rc=-ENOMEM;
  if(!rc) {
    p->fds=NULL;
    p->nfds=0;
    rc=fds_copy(fds,nfds,&p->fds);
    if(rc) {
      free(p);
      pthread_mutex_unlock(&c->lock);
      return rc;
    }
    p->nfds=nfds;
    p->next=NULL;
    p->offset=0;
    p->size=size+8;
    memcpy(p->bytes,"LSP1",4);
    put32(p->bytes+4,(uint32_t)size);
    if(size)memcpy(p->bytes+8,data,size);
    if(c->tail)c->tail->next=p;
    else c->head=p;
    c->tail=p;
    c->queued+=p->size;
    rc=flush(c);
    if(rc)c->error=-rc;
  }
  pthread_mutex_unlock(&c->lock);
  return rc;
}
int luna_process_channel_send(struct luna_process_channel *c,const void *data,size_t size)
{
  return luna_process_channel_send_fds(c,data,size,NULL,0);
}
int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn callback,void *user)
{
  if(!c)return -EINVAL;
  struct packet *head=NULL,*tail=NULL;
  int count=0;
  pthread_mutex_lock(&c->lock);
  int rc=c->error ? -c->error : flush(c);
  while(!rc && count<32) {
    unsigned char *to;
    size_t left;
    if(c->header_size<8) {
      to=c->header+c->header_size;
      left=8-c->header_size;
    }
    else {
      if(!c->incoming) {
        uint32_t size=get32(c->header+4);
        if(memcmp(c->header,"LSP1",4) || size>LUNA_PROCESS_MESSAGE_MAX) {
          rc=-EPROTO;
          break;
        }
        c->incoming=calloc(1,sizeof *c->incoming+size);
        if(!c->incoming) {
          rc=-ENOMEM;
          break;
        }
        if(c->incoming_nfds) {
          c->incoming->fds=malloc(c->incoming_nfds*sizeof(int));
          if(!c->incoming->fds) {
            rc=-ENOMEM;
            break;
          }
          memcpy(c->incoming->fds,c->incoming_fds,c->incoming_nfds*sizeof(int));
          c->incoming->nfds=c->incoming_nfds;
          c->incoming_nfds=0;
        }
        c->incoming->next=NULL;
        c->incoming->size=size;
        c->incoming->offset=0;
        c->received=0;
      }
      to=c->incoming->bytes+c->received;
      left=c->incoming->size-c->received;
      if(!left) {
        if(tail)tail->next=c->incoming;
        else head=c->incoming;
        tail=c->incoming;
        c->incoming=NULL;
        c->header_size=0;
        ++count;
        continue;
      }
    }
    struct iovec vec= {to,left};
    struct msghdr msg= {0};
    msg.msg_iov=&vec;
    msg.msg_iovlen=1;
    union {
      struct cmsghdr align;
      unsigned char bytes[CMSG_SPACE(LUNA_PROCESS_FD_MAX*sizeof(int))];
    } control;
    msg.msg_control=control.bytes;
    msg.msg_controllen=sizeof control.bytes;
    ssize_t n=recvmsg(c->fd,&msg,MSG_CMSG_CLOEXEC);
    if(n<0 && errno==EINTR)continue;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;
    if(n<=0) {
      rc=n<0 ? -errno : (c->header_size || c->incoming ? -EPROTO : -EPIPE);
      break;
    }
    bool bad=(msg.msg_flags&MSG_CTRUNC)!=0;
    for(struct cmsghdr *h=CMSG_FIRSTHDR(&msg); h; h=CMSG_NXTHDR(&msg,h)) {
      if(h->cmsg_level!=SOL_SOCKET || h->cmsg_type!=SCM_RIGHTS)continue;
      if(h->cmsg_len<CMSG_LEN(0)) {
        bad=true;
        continue;
      }
      size_t bytes=h->cmsg_len-CMSG_LEN(0),count=bytes/sizeof(int);
      if(bytes%sizeof(int))bad=true;
      for(size_t i=0; i<count; ++i) {
        int fd;
        memcpy(&fd,(unsigned char *)CMSG_DATA(h)+i*sizeof fd,sizeof fd);
        if(c->header_size || c->incoming || c->incoming_nfds>=LUNA_PROCESS_FD_MAX) {
          close(fd);
          bad=true;
        }
        else c->incoming_fds[c->incoming_nfds++]=fd;
      }
    }
    if(bad) {
      rc=-EPROTO;
      break;
    }
    if(c->header_size<8)c->header_size+=(size_t)n;
    else c->received+=(size_t)n;
  }
  if(rc)c->error=-rc;
  /* Keep unread callbacks on the channel, not on this stack. A callback
   * may pump this channel again while waiting for a synchronous Binder reply. */
  if(head) {
    if(c->ready_tail)c->ready_tail->next=head;
    else c->ready_head=head;
    c->ready_tail=tail;
  }
  pthread_mutex_unlock(&c->lock);
  for(unsigned delivered=0; delivered<32; ++delivered) {
    pthread_mutex_lock(&c->lock);
    struct packet *p=c->ready_head;
    if(p) {
      c->ready_head=p->next;
      if(!c->ready_head)c->ready_tail=NULL;
    }
    pthread_mutex_unlock(&c->lock);
    if(!p)break;
    struct fd_scope saved=current_fds;
    current_fds=(struct fd_scope) {
      p->fds,p->nfds
    };
    if(callback)callback(user,p->bytes,p->size);
    current_fds=saved;
    packet_free(p);
  }
  return rc ? rc : count;
}
void luna_process_channel_close(struct luna_process_channel *c)
{
  if(!c)return;
  close(c->fd);
  packets_free(c->head);
  packets_free(c->ready_head);
  packet_free(c->incoming);
  for(size_t i=0; i<c->incoming_nfds; ++i)close(c->incoming_fds[i]);
  pthread_mutex_destroy(&c->lock);
  free(c);
}
static bool env_key(const char *entry,const char *name)
{
  size_t n=strlen(name);
  return !strncmp(entry,name,n) && entry[n]=='=';
}
static char **child_environment(const char *name,uint32_t pid,uint32_t parent,uint32_t uid,bool host)
{
  size_t n=0;
  while(environ[n])++n;
  char **result=calloc(n+5,sizeof *result);
  if(!result)return NULL;
  size_t at=0;
  for(size_t i=0; i<n; ++i) {
    if(host && (env_key(environ[i],"LUNARIA_LAUNCH_ACTIVITY") || env_key(environ[i],"LUNARIA_TOUCH_TEST") ||
                env_key(environ[i],"LUNARIA_TOUCH_FIFO") || env_key(environ[i],"LUNARIA_PLAY")))continue;
    if(env_key(environ[i],"ANDROID_PROCESS_NAME") || env_key(environ[i],"LUNARIA_GUEST_TID_BASE") ||
        env_key(environ[i],"LUNARIA_GUEST_PPID") || env_key(environ[i],"LUNARIA_GUEST_UID"))continue;
    result[at]=strdup(environ[i]);
    if(!result[at++])goto fail;
  }
  size_t len=strlen(name)+sizeof "ANDROID_PROCESS_NAME=";
  result[at]=malloc(len);
  if(!result[at])goto fail;
  snprintf(result[at++],len,"ANDROID_PROCESS_NAME=%s",name);
  char text[64];
  snprintf(text,sizeof text,"LUNARIA_GUEST_TID_BASE=%u",pid);
  result[at]=strdup(text);
  if(!result[at++])goto fail;
  snprintf(text,sizeof text,"LUNARIA_GUEST_PPID=%u",parent);
  result[at]=strdup(text);
  if(!result[at++])goto fail;
  if(uid) {
    snprintf(text,sizeof text,"LUNARIA_GUEST_UID=%u",uid);
    result[at]=strdup(text);
    if(!result[at++])goto fail;
  }
  return result;
fail:
  for(size_t i=0; i<at; ++i)free(result[i]);
  free(result);
  return NULL;
}
static void environment_free(char **env)
{
  if(env) {
    for(size_t i=0; env[i]; ++i)free(env[i]);
    free(env);
  }
}
static int spawn_child(struct luna_process_pool *pool,const char *name,struct child **out)
{
  if(pool->next_pid>PID_LIMIT-PID_STRIDE)return -EAGAIN;
  struct child *child=calloc(1,sizeof *child);
  if(!child)return -ENOMEM;
  child->name=strdup(name);
  if(!child->name) {
    free(child);
    return -ENOMEM;
  }
  int fds[2];
  if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds)<0) {
    int e=errno;
    free(child->name);
    free(child);
    return -e;
  }
  /* dup2's source must survive replacement of the protocol's fixed fd 3. */
  int source=fcntl(fds[1],F_DUPFD_CLOEXEC,4);
  if(source<0) {
    int e=errno;
    close(fds[0]);
    close(fds[1]);
    free(child->name);
    free(child);
    return -e;
  }
  posix_spawn_file_actions_t actions;
  int rc=posix_spawn_file_actions_init(&actions);
  bool initialized=rc==0;
  if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,source,3);
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2,34)
#define LUNA_HAVE_SPAWN_CLOSEFROM 1
#endif
#endif
#ifdef LUNA_HAVE_SPAWN_CLOSEFROM
  if(!rc)rc=posix_spawn_file_actions_addclosefrom_np(&actions,4);
#else
  /* This path must not leak the live VM's files/sockets into another VM. */
  if(!rc)rc=ENOTSUP;
#endif
  char **env=child_environment(name,pool->next_pid,pool->parent_pid,pool->guest_uid,pool->host_apk!=NULL);
  if(!env && !rc)rc=ENOMEM;
  const char *owner=getenv("ANDROID_PACKAGE_NAME"),*profile="default";
  /* A peer system package owns its own app data, rather than another game's
   * account profile. This package's own secondary processes still share it. */
  size_t owner_length=owner ? strlen(owner) : 0;
  if(owner_length && !strncmp(name,owner,owner_length) && (!name[owner_length] || name[owner_length]==':')) {
    const char *current=getenv("LUNARIA_PROFILE");
    if(current && *current)profile=current;
  } else if(!strcmp(name,"com.google.android.gms") || !strcmp(name,"com.android.vending") || !strcmp(name,"com.google.android.gsf"))
    profile=luna_google_profile_current();
  if(pool->profile)profile=pool->profile;
  char *module_args[]= {pool->executable,pool->mode,"3",NULL};
  char *host_args[]= {pool->executable,"--service-host","--profile",(char *)profile,pool->host_apk,NULL};
  char **args=pool->host_apk ? host_args : module_args;
  if(!rc)rc=posix_spawn(&child->host_pid,pool->executable,&actions,NULL,args,env);
  if(initialized)posix_spawn_file_actions_destroy(&actions);
  environment_free(env);
  close(source);
  close(fds[1]);
  if(rc) {
    close(fds[0]);
    free(child->name);
    free(child);
    return -rc;
  }
  child->guest_pid=pool->next_pid;
  pool->next_pid+=PID_STRIDE;
  child->channel=luna_process_channel_open(fds[0]);
  if(!child->channel) {
    int e=errno;
    kill(child->host_pid,SIGKILL);
    while(waitpid(child->host_pid,NULL,0)<0 && errno==EINTR) {}
    free(child->name);
    free(child);
    return -e;
  }
  child->next=pool->children;
  pool->children=child;
  *out=child;
  return 0;
}
struct luna_process_pool *luna_process_pool_open(const char *exe,const char *mode,uint32_t parent,uint32_t first)
{
  if(!exe || !*exe || !mode || !*mode || !parent || parent>PID_LIMIT-PID_STRIDE || first<parent+PID_STRIDE || first>PID_LIMIT-PID_STRIDE) {
    errno=EINVAL;
    return NULL;
  }
  struct luna_process_pool *pool=calloc(1,sizeof *pool);
  if(!pool)return NULL;
  pool->executable=strdup(exe);
  pool->mode=strdup(mode);
  pool->parent_pid=parent;
  pool->next_pid=first;
  int rc=pthread_mutex_init(&pool->lock,NULL);
  if(!pool->executable || !pool->mode || rc) {
    if(!rc)pthread_mutex_destroy(&pool->lock);
    free(pool->executable);
    free(pool->mode);
    free(pool);
    errno=rc ? rc : ENOMEM;
    return NULL;
  }
  return pool;
}
struct luna_process_pool *luna_process_pool_open_host(const char *exe,const char *apk,uint32_t uid,uint32_t parent,uint32_t first)
{
  if(!apk || !*apk) {
    errno=EINVAL;
    return NULL;
  }
  struct luna_process_pool *pool=luna_process_pool_open(exe,"--service-host",parent,first);
  if(!pool)return NULL;
  pool->guest_uid=uid;
  pool->host_apk=strdup(apk);
  if(!pool->host_apk) {
    int e=errno;
    luna_process_pool_close(pool);
    errno=e;
    return NULL;
  }
  return pool;
}
int luna_process_pool_set_profile(struct luna_process_pool *pool,const char *profile)
{
  if(!pool || !profile || !*profile)return -EINVAL;
  char *copy=strdup(profile);
  if(!copy)return -ENOMEM;
  pthread_mutex_lock(&pool->lock);
  free(pool->profile);
  pool->profile=copy;
  pthread_mutex_unlock(&pool->lock);
  return 0;
}
static void child_poll(struct child *child)
{
  if(child->dead)return;
  int status=0;
  pid_t result;
  do {
    result=waitpid(child->host_pid,&status,WNOHANG);
  }
  while(result<0 && errno==EINTR);
  if(result==child->host_pid || (result<0 && errno==ECHILD)) {
    child->dead=true;
    child->status=result>0 ? status : -ECHILD;
  }
}
static void reap(struct luna_process_pool *pool)
{
  struct child **link=&pool->children;
  while(*link) {
    struct child *child=*link;
    child_poll(child);
    /* An exited VM can still have final replies in its socket. Keep its
     * old incarnation until those replies and EOF have been consumed. */
    if(child->dead && child->drained) {
      struct exit_notice *notice=malloc(sizeof *notice);
      if(notice) {
        notice->next=NULL;
        notice->name=child->name;
        child->name=NULL;
        notice->pid=child->guest_pid;
        notice->status=child->status;
        if(pool->exits_tail)pool->exits_tail->next=notice;
        else pool->exits=notice;
        pool->exits_tail=notice;
      } else pool->error=ENOMEM;
      *link=child->next;
      luna_process_channel_close(child->channel);
      free(child->name);
      free(child);
      continue;
    }
    link=&child->next;
  }
}
int luna_process_pool_send_fds(struct luna_process_pool *pool,const char *name,const void *data,size_t size,uint32_t *pid,const int *fds,size_t nfds)
{
  if(!pool || !name || !*name || strlen(name)>NAME_MAX_BYTES || (!data && size))return -EINVAL;
  if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
  pthread_mutex_lock(&pool->lock);
  struct child *child=pool->children;
  while(child) {
    child_poll(child);
    if(!child->dead && !strcmp(child->name,name))break;
    child=child->next;
  }
  int rc=child ? 0 : spawn_child(pool,name,&child);
  if(!rc) {
    rc=luna_process_channel_send_fds(child->channel,data,size,fds,nfds);
    if(!rc && pid)*pid=child->guest_pid;
  }
  pthread_mutex_unlock(&pool->lock);
  return rc;
}
int luna_process_pool_send(struct luna_process_pool *pool,const char *name,const void *data,size_t size,uint32_t *pid)
{
  return luna_process_pool_send_fds(pool,name,data,size,pid,NULL,0);
}
int luna_process_pool_send_to_host_fds(struct luna_process_pool *pool,uint32_t host_pid,const void *data,size_t size,const int *fds,size_t nfds)
{
  if(!pool || (!data && size))return -EINVAL;
  if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
  pthread_mutex_lock(&pool->lock);
  int rc=-ESRCH;
  for(struct child *child=pool->children; child; child=child->next) {
    if((uint32_t)child->host_pid!=host_pid)continue;
    child_poll(child);
    if(!child->dead)rc=luna_process_channel_send_fds(child->channel,data,size,fds,nfds);
    break;
  }
  pthread_mutex_unlock(&pool->lock);
  return rc;
}
int luna_process_pool_send_to_host(struct luna_process_pool *pool,uint32_t host_pid,const void *data,size_t size)
{
  return luna_process_pool_send_to_host_fds(pool,host_pid,data,size,NULL,0);
}
int luna_process_pool_running(struct luna_process_pool *pool,const char *name)
{
  if(!pool || !name)return 0;
  pthread_mutex_lock(&pool->lock);
  int running=0;
  for(struct child *child=pool->children; child && !running; child=child->next) {
    child_poll(child);
    running=!child->dead && !strcmp(child->name,name);
  }
  pthread_mutex_unlock(&pool->lock);
  return running;
}
int luna_process_pool_publish_fds(struct luna_process_pool *pool,uint32_t exclude,const void *data,size_t size,const int *fds,size_t nfds)
{
  if(!pool || (!data && size))return -EINVAL;
  if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
  pthread_mutex_lock(&pool->lock);
  int error=0;
  for(struct child *c=pool->children; c; c=c->next) {
    child_poll(c);
    if(c->dead || (uint32_t)c->host_pid==exclude)continue;
    int rc=luna_process_channel_send_fds(c->channel,data,size,fds,nfds);
    /* A receiver can die after child_poll. Its loss is not a failed
     * broadcast to the remaining processes. */
    if(rc<0 && rc!=-EPIPE && rc!=-ECONNRESET && !error)error=rc;
  }
  pthread_mutex_unlock(&pool->lock);
  return error;
}
int luna_process_pool_publish(struct luna_process_pool *pool,uint32_t exclude,const void *data,size_t size)
{
  return luna_process_pool_publish_fds(pool,exclude,data,size,NULL,0);
}
struct reply {
  struct reply *next;
  char *name;
  uint32_t pid;
  size_t size,nfds;
  int *fds;
  unsigned char data[];
};
struct replies {
  struct reply *head,*tail;
  struct child *child;
  int error;
};
static void collect_reply(void *user,const void *data,size_t size)
{
  struct replies *r=user;
  struct reply *p=malloc(sizeof *p+size);
  if(!p) {
    r->error=ENOMEM;
    return;
  }
  p->name=strdup(r->child->name);
  if(!p->name) {
    free(p);
    r->error=ENOMEM;
    return;
  }
  const int *fds=luna_process_message_fds(&p->nfds);
  int rc=fds_copy(fds,p->nfds,&p->fds);
  if(rc) {
    free(p->name);
    free(p);
    r->error=-rc;
    return;
  }
  p->next=NULL;
  p->pid=r->child->guest_pid;
  p->size=size;
  if(size)memcpy(p->data,data,size);
  if(r->tail)r->tail->next=p;
  else r->head=p;
  r->tail=p;
}
int luna_process_pool_tick(struct luna_process_pool *pool,luna_process_reply_fn callback,void *user)
{
  if(!pool)return -EINVAL;
  struct replies replies= {0};
  int count=0,error=0;
  pthread_mutex_lock(&pool->lock);
  for(struct child *child=pool->children; child; child=child->next) {
    replies.child=child;
    int n=luna_process_channel_tick(child->channel,collect_reply,&replies);
    if(n>0)count+=n;
    if(n<0) {
      child->drained=true;
      if(!error)error=-n;
    }
  }
  reap(pool);
  if(pool->error && !error)error=pool->error;
  pool->error=0;
  /* The pool also batches channel callbacks; expose its pending replies to
   * a nested pool tick before invoking any potentially blocking callback. */
  if(replies.head) {
    if(pool->ready_tail)pool->ready_tail->next=replies.head;
    else pool->ready_head=replies.head;
    pool->ready_tail=replies.tail;
  }
  pthread_mutex_unlock(&pool->lock);
  for(;;) {
    pthread_mutex_lock(&pool->lock);
    struct reply *p=pool->ready_head;
    if(p) {
      pool->ready_head=p->next;
      if(!pool->ready_head)pool->ready_tail=NULL;
    }
    pthread_mutex_unlock(&pool->lock);
    if(!p)break;
    struct fd_scope saved=current_fds;
    current_fds=(struct fd_scope) {
      p->fds,p->nfds
    };
    if(callback)callback(user,p->name,p->pid,p->data,p->size);
    current_fds=saved;
    fds_free(p->fds,p->nfds);
    free(p->name);
    free(p);
  }
  return replies.error ? -replies.error : error ? -error : count;
}
int luna_process_pool_next_exit(struct luna_process_pool *pool,char *name,size_t capacity,uint32_t *pid,int *status)
{
  if(!pool || !name || !capacity)return -EINVAL;
  pthread_mutex_lock(&pool->lock);
  struct exit_notice *n=pool->exits;
  int rc=0;
  if(n) {
    if(strlen(n->name)>=capacity)rc=-ENOSPC;
    else {
      memcpy(name,n->name,strlen(n->name)+1);
      if(pid)*pid=n->pid;
      if(status)*status=n->status;
      pool->exits=n->next;
      if(!pool->exits)pool->exits_tail=NULL;
      free(n->name);
      free(n);
      rc=1;
    }
  }
  pthread_mutex_unlock(&pool->lock);
  return rc;
}
void luna_process_pool_close(struct luna_process_pool *pool)
{
  if(!pool)return;
  while(pool->ready_head) {
    struct reply *p=pool->ready_head;
    pool->ready_head=p->next;
    fds_free(p->fds,p->nfds);
    free(p->name);
    free(p);
  }
  for(struct child *c=pool->children; c; c=c->next) {
    luna_process_channel_close(c->channel);
    c->channel=NULL;
    c->drained=true;
  }
  struct timespec delay= {0,10000000};
  for(unsigned i=0; i<20 && pool->children; ++i) {
    reap(pool);
    if(pool->children)nanosleep(&delay,NULL);
  }
  for(struct child *c=pool->children; c; c=c->next)kill(c->host_pid,SIGTERM);
  for(unsigned i=0; i<20 && pool->children; ++i) {
    reap(pool);
    if(pool->children)nanosleep(&delay,NULL);
  }
  while(pool->children) {
    struct child *c=pool->children;
    pool->children=c->next;
    kill(c->host_pid,SIGKILL);
    while(waitpid(c->host_pid,NULL,0)<0 && errno==EINTR) {}
    free(c->name);
    free(c);
  }
  while(pool->exits) {
    struct exit_notice *n=pool->exits;
    pool->exits=n->next;
    free(n->name);
    free(n);
  }
  pthread_mutex_destroy(&pool->lock);
  free(pool->profile);
  free(pool->host_apk);
  free(pool->mode);
  free(pool->executable);
  free(pool);
}

#else
/* No fork-only or shared-address-space substitute on unsupported hosts. */
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#define ipc_close _close
#else
#include <unistd.h>
#define ipc_close close
#endif
struct luna_process_channel *luna_process_channel_open(int fd)
{
  if(fd>=0)ipc_close(fd);
  errno=ENOSYS;
  return NULL;
}
int luna_process_channel_send(struct luna_process_channel *c,const void *p,size_t n)
{
  (void)c;
  (void)p;
  (void)n;
  return -ENOSYS;
}
int luna_process_channel_send_fds(struct luna_process_channel *c,const void *p,size_t n,const int *f,size_t nf)
{
  (void)c;
  (void)p;
  (void)n;
  (void)f;
  (void)nf;
  return -ENOSYS;
}
const int *luna_process_message_fds(size_t *count)
{
  if(count)*count=0;
  return NULL;
}
int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn f,void *u)
{
  (void)c;
  (void)f;
  (void)u;
  return -ENOSYS;
}
void luna_process_channel_close(struct luna_process_channel *c) {
  (void)c;
}
struct luna_process_pool *luna_process_pool_open_host(const char *e,const char *a,uint32_t u,uint32_t p,uint32_t f)
{
  (void)e;
  (void)a;
  (void)u;
  (void)p;
  (void)f;
  errno=ENOSYS;
  return NULL;
}
int luna_process_pool_set_profile(struct luna_process_pool *p,const char *n)
{
  (void)p;
  (void)n;
  return -ENOSYS;
}
struct luna_process_pool *luna_process_pool_open(const char *e,const char *m,uint32_t p,uint32_t f)
{
  (void)e;
  (void)m;
  (void)p;
  (void)f;
  errno=ENOSYS;
  return NULL;
}
int luna_process_pool_running(struct luna_process_pool *p,const char *n)
{
  (void)p;
  (void)n;
  return 0;
}
int luna_process_pool_send_to_host(struct luna_process_pool *p,uint32_t h,const void *d,size_t s)
{
  (void)p;
  (void)h;
  (void)d;
  (void)s;
  return -ENOSYS;
}
int luna_process_pool_send_to_host_fds(struct luna_process_pool *p,uint32_t h,const void *d,size_t s,const int *f,size_t n)
{
  (void)p;
  (void)h;
  (void)d;
  (void)s;
  (void)f;
  (void)n;
  return -ENOSYS;
}
int luna_process_pool_publish(struct luna_process_pool *p,uint32_t h,const void *d,size_t s)
{
  (void)p;
  (void)h;
  (void)d;
  (void)s;
  return -ENOSYS;
}
int luna_process_pool_publish_fds(struct luna_process_pool *p,uint32_t h,const void *d,size_t s,const int *f,size_t n)
{
  (void)p;
  (void)h;
  (void)d;
  (void)s;
  (void)f;
  (void)n;
  return -ENOSYS;
}
int luna_process_pool_send(struct luna_process_pool *p,const char *n,const void *d,size_t s,uint32_t *g)
{
  (void)p;
  (void)n;
  (void)d;
  (void)s;
  (void)g;
  return -ENOSYS;
}
int luna_process_pool_send_fds(struct luna_process_pool *p,const char *n,const void *d,size_t s,uint32_t *g,const int *f,size_t nf)
{
  (void)p;
  (void)n;
  (void)d;
  (void)s;
  (void)g;
  (void)f;
  (void)nf;
  return -ENOSYS;
}
int luna_process_pool_tick(struct luna_process_pool *p,luna_process_reply_fn f,void *u)
{
  (void)p;
  (void)f;
  (void)u;
  return -ENOSYS;
}
int luna_process_pool_next_exit(struct luna_process_pool *p,char *n,size_t c,uint32_t *g,int *s)
{
  (void)p;
  (void)n;
  (void)c;
  (void)g;
  (void)s;
  return -ENOSYS;
}
void luna_process_pool_close(struct luna_process_pool *p) {
  (void)p;
}
int luna_process_state_open(const char *directory) {
  (void)directory;
  return -ENOSYS;
}
int luna_process_state_publish(const struct luna_process_info *info) {
  (void)info;
  return -ENOSYS;
}
int luna_process_state_list(struct luna_process_info **out,size_t *count)
{
  if(out)*out=NULL;
  if(count)*count=0;
  return -ENOSYS;
}
#endif
