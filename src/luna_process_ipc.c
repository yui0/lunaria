/* MPL-2.0. Process isolation uses exec, never fork of a live threaded VM. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lunaria_os.h"
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
#include <time.h>
#include <unistd.h>
extern char **environ;
#define QUEUE_MAX (8u * LUNA_PROCESS_MESSAGE_MAX)
#define PID_STRIDE 4096u
#define PID_LIMIT 4194304u
#define NAME_MAX_BYTES 511u
struct packet { struct packet *next; size_t size,offset; unsigned char bytes[]; };
struct luna_process_channel {
   pthread_mutex_t lock;
   int fd,error;
   struct packet *head,*tail;
   size_t queued;
   unsigned char header[8];
   size_t header_size,received;
   struct packet *incoming;
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
struct exit_notice { struct exit_notice *next; char *name; uint32_t pid; int status; };
struct luna_process_pool {
   pthread_mutex_t lock;
   char *executable,*mode;
   uint32_t parent_pid,next_pid;
   struct child *children;
   struct exit_notice *exits,*exits_tail;
   int error;
};
static void put32(unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(i*8)); }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void packets_free(struct packet *p)
{ while(p){struct packet *next=p->next;free(p);p=next;} }
struct luna_process_channel *luna_process_channel_open(int fd)
{
   if(fd<0){errno=EBADF;return NULL;}
   int flags=fcntl(fd,F_GETFL),cloexec=fcntl(fd,F_GETFD);
   if(flags<0 || cloexec<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 ||
      fcntl(fd,F_SETFD,cloexec|FD_CLOEXEC)<0){int e=errno;close(fd);errno=e;return NULL;}
   struct luna_process_channel *c=calloc(1,sizeof *c);
   if(!c){close(fd);return NULL;}
   int e=pthread_mutex_init(&c->lock,NULL);
   if(e){close(fd);free(c);errno=e;return NULL;}
   c->fd=fd;return c;
}
static int flush(struct luna_process_channel *c)
{
   while(c->head){
      struct packet *p=c->head;
      ssize_t n=send(c->fd,p->bytes+p->offset,p->size-p->offset,MSG_NOSIGNAL);
      if(n<0 && errno==EINTR)continue;
      if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
      if(n<=0)return -(n<0 ? errno : EPIPE);
      p->offset+=(size_t)n;c->queued-=(size_t)n;
      if(p->offset==p->size){c->head=p->next;if(!c->head)c->tail=NULL;free(p);}
   }
   return 0;
}
int luna_process_channel_send(struct luna_process_channel *c,const void *data,size_t size)
{
   if(!c || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&c->lock);
   int rc=c->error ? -c->error : 0;
   if(!rc && c->queued+size+8>QUEUE_MAX)rc=-ENOBUFS;
   struct packet *p=rc ? NULL : malloc(sizeof *p+size+8);
   if(!rc && !p)rc=-ENOMEM;
   if(!rc){
      p->next=NULL;p->offset=0;p->size=size+8;
      memcpy(p->bytes,"LSP1",4);put32(p->bytes+4,(uint32_t)size);
      if(size)memcpy(p->bytes+8,data,size);
      if(c->tail)c->tail->next=p;else c->head=p;c->tail=p;c->queued+=p->size;
      rc=flush(c);if(rc)c->error=-rc;
   }
   pthread_mutex_unlock(&c->lock);return rc;
}
int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn callback,void *user)
{
   if(!c)return -EINVAL;
   struct packet *head=NULL,*tail=NULL;int count=0;
   pthread_mutex_lock(&c->lock);
   int rc=c->error ? -c->error : flush(c);
   while(!rc && count<32){
      unsigned char *to;size_t left;
      if(c->header_size<8){to=c->header+c->header_size;left=8-c->header_size;}
      else{
         if(!c->incoming){
            uint32_t size=get32(c->header+4);
            if(memcmp(c->header,"LSP1",4) || size>LUNA_PROCESS_MESSAGE_MAX){rc=-EPROTO;break;}
            c->incoming=malloc(sizeof *c->incoming+size);
            if(!c->incoming){rc=-ENOMEM;break;}
            c->incoming->next=NULL;c->incoming->size=size;c->incoming->offset=0;c->received=0;
         }
         to=c->incoming->bytes+c->received;left=c->incoming->size-c->received;
         if(!left){
            if(tail)tail->next=c->incoming;else head=c->incoming;
            tail=c->incoming;c->incoming=NULL;c->header_size=0;++count;continue;
         }
      }
      ssize_t n=recv(c->fd,to,left,0);
      if(n<0 && errno==EINTR)continue;
      if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;
      if(n<=0){rc=n<0 ? -errno : (c->header_size || c->incoming ? -EPROTO : -EPIPE);break;}
      if(c->header_size<8)c->header_size+=(size_t)n;else c->received+=(size_t)n;
   }
   if(rc)c->error=-rc;
   pthread_mutex_unlock(&c->lock);
   for(struct packet *p=head;p;p=p->next)if(callback)callback(user,p->bytes,p->size);
   packets_free(head);return rc ? rc : count;
}
void luna_process_channel_close(struct luna_process_channel *c)
{
   if(!c)return;
   close(c->fd);packets_free(c->head);free(c->incoming);
   pthread_mutex_destroy(&c->lock);free(c);
}
static bool env_key(const char *entry,const char *name)
{ size_t n=strlen(name);return !strncmp(entry,name,n) && entry[n]=='='; }
static char **child_environment(const char *name,uint32_t pid,uint32_t parent)
{
   size_t n=0;while(environ[n])++n;
   char **result=calloc(n+4,sizeof *result);if(!result)return NULL;
   size_t at=0;
   for(size_t i=0;i<n;++i){
      if(env_key(environ[i],"ANDROID_PROCESS_NAME") || env_key(environ[i],"LUNARIA_GUEST_TID_BASE") ||
         env_key(environ[i],"LUNARIA_GUEST_PPID"))continue;
      result[at]=strdup(environ[i]);if(!result[at++])goto fail;
   }
   size_t len=strlen(name)+sizeof "ANDROID_PROCESS_NAME=";
   result[at]=malloc(len);if(!result[at])goto fail;
   snprintf(result[at++],len,"ANDROID_PROCESS_NAME=%s",name);
   char text[64];snprintf(text,sizeof text,"LUNARIA_GUEST_TID_BASE=%u",pid);
   result[at]=strdup(text);if(!result[at++])goto fail;
   snprintf(text,sizeof text,"LUNARIA_GUEST_PPID=%u",parent);
   result[at]=strdup(text);if(!result[at++])goto fail;
   return result;
fail:
   for(size_t i=0;i<at;++i)free(result[i]);
   free(result);return NULL;
}
static void environment_free(char **env)
{ if(env){for(size_t i=0;env[i];++i)free(env[i]);free(env);} }
static int spawn_child(struct luna_process_pool *pool,const char *name,struct child **out)
{
   if(pool->next_pid>PID_LIMIT-PID_STRIDE)return -EAGAIN;
   struct child *child=calloc(1,sizeof *child);if(!child)return -ENOMEM;
   child->name=strdup(name);if(!child->name){free(child);return -ENOMEM;}
   int fds[2];
   if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds)<0){int e=errno;free(child->name);free(child);return -e;}
   /* dup2's source must survive replacement of the protocol's fixed fd 3. */
   int source=fcntl(fds[1],F_DUPFD_CLOEXEC,4);
   if(source<0){int e=errno;close(fds[0]);close(fds[1]);free(child->name);free(child);return -e;}
   posix_spawn_file_actions_t actions;int rc=posix_spawn_file_actions_init(&actions);
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
   char **env=child_environment(name,pool->next_pid,pool->parent_pid);
   if(!env && !rc)rc=ENOMEM;
   char *args[]={pool->executable,pool->mode,"3",NULL};
   if(!rc)rc=posix_spawn(&child->host_pid,pool->executable,&actions,NULL,args,env);
   if(initialized)posix_spawn_file_actions_destroy(&actions);
   environment_free(env);close(source);close(fds[1]);
   if(rc){close(fds[0]);free(child->name);free(child);return -rc;}
   child->guest_pid=pool->next_pid;pool->next_pid+=PID_STRIDE;
   child->channel=luna_process_channel_open(fds[0]);
   if(!child->channel){
      int e=errno;kill(child->host_pid,SIGKILL);while(waitpid(child->host_pid,NULL,0)<0 && errno==EINTR){}
      free(child->name);free(child);return -e;
   }
   child->next=pool->children;pool->children=child;*out=child;return 0;
}
struct luna_process_pool *luna_process_pool_open(const char *exe,const char *mode,uint32_t parent,uint32_t first)
{
   if(!exe || !*exe || !mode || !*mode || !parent || parent>PID_LIMIT-PID_STRIDE || first<parent+PID_STRIDE || first>PID_LIMIT-PID_STRIDE){errno=EINVAL;return NULL;}
   struct luna_process_pool *pool=calloc(1,sizeof *pool);if(!pool)return NULL;
   pool->executable=strdup(exe);pool->mode=strdup(mode);pool->parent_pid=parent;pool->next_pid=first;
   int rc=pthread_mutex_init(&pool->lock,NULL);
   if(!pool->executable || !pool->mode || rc){if(!rc)pthread_mutex_destroy(&pool->lock);free(pool->executable);free(pool->mode);free(pool);errno=rc ? rc : ENOMEM;return NULL;}
   return pool;
}
static void child_poll(struct child *child)
{
   if(child->dead)return;
   int status=0;pid_t result;
   do{result=waitpid(child->host_pid,&status,WNOHANG);}while(result<0 && errno==EINTR);
   if(result==child->host_pid || (result<0 && errno==ECHILD)){
      child->dead=true;child->status=result>0 ? status : -ECHILD;
   }
}
static void reap(struct luna_process_pool *pool)
{
   struct child **link=&pool->children;
   while(*link){
      struct child *child=*link;child_poll(child);
      /* An exited VM can still have final replies in its socket. Keep its
       * old incarnation until those replies and EOF have been consumed. */
      if(child->dead && child->drained){
         struct exit_notice *notice=malloc(sizeof *notice);
         if(notice){
            notice->next=NULL;notice->name=child->name;child->name=NULL;
            notice->pid=child->guest_pid;notice->status=child->status;
            if(pool->exits_tail)pool->exits_tail->next=notice;else pool->exits=notice;
            pool->exits_tail=notice;
         }else pool->error=ENOMEM;
         *link=child->next;luna_process_channel_close(child->channel);free(child->name);free(child);continue;
      }
      link=&child->next;
   }
}
int luna_process_pool_send(struct luna_process_pool *pool,const char *name,const void *data,size_t size,uint32_t *pid)
{
   if(!pool || !name || !*name || strlen(name)>NAME_MAX_BYTES || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&pool->lock);
   struct child *child=pool->children;
   while(child){child_poll(child);if(!child->dead && !strcmp(child->name,name))break;child=child->next;}
   int rc=child ? 0 : spawn_child(pool,name,&child);
   if(!rc){rc=luna_process_channel_send(child->channel,data,size);if(!rc && pid)*pid=child->guest_pid;}
   pthread_mutex_unlock(&pool->lock);return rc;
}
struct reply { struct reply *next; char *name; uint32_t pid; size_t size; unsigned char data[]; };
struct replies { struct reply *head,*tail; struct child *child; int error; };
static void collect_reply(void *user,const void *data,size_t size)
{
   struct replies *r=user;struct reply *p=malloc(sizeof *p+size);
   if(!p){r->error=ENOMEM;return;}
   p->name=strdup(r->child->name);if(!p->name){free(p);r->error=ENOMEM;return;}
   p->next=NULL;p->pid=r->child->guest_pid;p->size=size;if(size)memcpy(p->data,data,size);
   if(r->tail)r->tail->next=p;else r->head=p;r->tail=p;
}
int luna_process_pool_tick(struct luna_process_pool *pool,luna_process_reply_fn callback,void *user)
{
   if(!pool)return -EINVAL;
   struct replies replies={0};int count=0,error=0;
   pthread_mutex_lock(&pool->lock);
   for(struct child *child=pool->children;child;child=child->next){
      replies.child=child;int n=luna_process_channel_tick(child->channel,collect_reply,&replies);
      if(n>0)count+=n;
      if(n<0){child->drained=true;if(!error)error=-n;}
   }
   reap(pool);if(pool->error && !error)error=pool->error;pool->error=0;
   pthread_mutex_unlock(&pool->lock);
   struct reply *p=replies.head;
   while(p){struct reply *next=p->next;if(callback)callback(user,p->name,p->pid,p->data,p->size);free(p->name);free(p);p=next;}
   return replies.error ? -replies.error : error ? -error : count;
}
int luna_process_pool_next_exit(struct luna_process_pool *pool,char *name,size_t capacity,uint32_t *pid,int *status)
{
   if(!pool || !name || !capacity)return -EINVAL;
   pthread_mutex_lock(&pool->lock);struct exit_notice *n=pool->exits;int rc=0;
   if(n){
      if(strlen(n->name)>=capacity)rc=-ENOSPC;
      else{
         memcpy(name,n->name,strlen(n->name)+1);if(pid)*pid=n->pid;if(status)*status=n->status;
         pool->exits=n->next;if(!pool->exits)pool->exits_tail=NULL;free(n->name);free(n);rc=1;
      }
   }
   pthread_mutex_unlock(&pool->lock);return rc;
}
void luna_process_pool_close(struct luna_process_pool *pool)
{
   if(!pool)return;
   for(struct child *c=pool->children;c;c=c->next){luna_process_channel_close(c->channel);c->channel=NULL;c->drained=true;}
   struct timespec delay={0,10000000};
   for(unsigned i=0;i<20 && pool->children;++i){reap(pool);if(pool->children)nanosleep(&delay,NULL);}
   for(struct child *c=pool->children;c;c=c->next)kill(c->host_pid,SIGTERM);
   for(unsigned i=0;i<20 && pool->children;++i){reap(pool);if(pool->children)nanosleep(&delay,NULL);}
   while(pool->children){
      struct child *c=pool->children;pool->children=c->next;
      kill(c->host_pid,SIGKILL);while(waitpid(c->host_pid,NULL,0)<0 && errno==EINTR){}
      free(c->name);free(c);
   }
   while(pool->exits){struct exit_notice *n=pool->exits;pool->exits=n->next;free(n->name);free(n);}
   pthread_mutex_destroy(&pool->lock);free(pool->mode);free(pool->executable);free(pool);
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
{ if(fd>=0)ipc_close(fd);errno=ENOSYS;return NULL; }
int luna_process_channel_send(struct luna_process_channel *c,const void *p,size_t n)
{ (void)c;(void)p;(void)n;return -ENOSYS; }
int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn f,void *u)
{ (void)c;(void)f;(void)u;return -ENOSYS; }
void luna_process_channel_close(struct luna_process_channel *c){(void)c;}
struct luna_process_pool *luna_process_pool_open(const char *e,const char *m,uint32_t p,uint32_t f)
{ (void)e;(void)m;(void)p;(void)f;errno=ENOSYS;return NULL; }
int luna_process_pool_send(struct luna_process_pool *p,const char *n,const void *d,size_t s,uint32_t *g)
{ (void)p;(void)n;(void)d;(void)s;(void)g;return -ENOSYS; }
int luna_process_pool_tick(struct luna_process_pool *p,luna_process_reply_fn f,void *u)
{ (void)p;(void)f;(void)u;return -ENOSYS; }
int luna_process_pool_next_exit(struct luna_process_pool *p,char *n,size_t c,uint32_t *g,int *s)
{ (void)p;(void)n;(void)c;(void)g;(void)s;return -ENOSYS; }
void luna_process_pool_close(struct luna_process_pool *p){(void)p;}
#endif
