/* SPDX-License-Identifier: MPL-2.0 */
/* Private Windows transport for the existing Android service/Binder broker. */

#define WIN_IPC_HEADER 16u
#define WIN_IPC_PIPE_ENV L"LUNARIA_PRIVATE_CONTROL_PIPE"
#define WIN_IPC_FILE 1u
#define WIN_IPC_SOCKET 2u

struct win_ipc_attachment {
  uint32_t kind, nonblock;
  union {
    uintptr_t file;
    WSAPROTOCOL_INFOW socket;
  } value;
};
#define WIN_IPC_MAX (WIN_IPC_HEADER + LUNA_PROCESS_MESSAGE_MAX + \
                     LUNA_PROCESS_FD_MAX * sizeof(struct win_ipc_attachment))

static struct luna_process_channel *inherited_channel;

static int win_ipc_error(DWORD error)
{
  switch (error) {
  case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: case ERROR_PIPE_NOT_CONNECTED: return EPIPE;
  case ERROR_ACCESS_DENIED: return EACCES;
  case ERROR_INVALID_HANDLE: return EBADF;
  case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return ENOMEM;
  case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
  default: return EIO;
  }
}

static struct luna_process_channel *win_channel_open(HANDLE pipe, HANDLE peer, DWORD pid)
{
  DWORD mode=PIPE_READMODE_MESSAGE|PIPE_NOWAIT;
  struct luna_process_channel *channel=NULL;
  int error=0;
  if(!SetNamedPipeHandleState(pipe,&mode,NULL,NULL))error=win_ipc_error(GetLastError());
  if(!error && !(channel=calloc(1,sizeof *channel)))error=ENOMEM;
  if(!error)error=pthread_mutex_init(&channel->lock,NULL);
  if(error) {
    CloseHandle(pipe);
    CloseHandle(peer);
    free(channel);
    errno=error;
    return NULL;
  }
  channel->pipe=pipe;
  channel->peer=peer;
  channel->peer_pid=pid;
  return channel;
}

int luna_process_win_prepare_child(void)
{
  wchar_t text[2*sizeof(uintptr_t)+1];
  DWORD length=GetEnvironmentVariableW(WIN_IPC_PIPE_ENV,text,(DWORD)(sizeof text/sizeof *text));
  SetEnvironmentVariableW(WIN_IPC_PIPE_ENV,NULL);
  /* Clear the CRT's inherited environment copy as well, before loading guest code. */
  _wputenv_s(WIN_IPC_PIPE_ENV,L"");
  if(!length || length>=sizeof text/sizeof *text || inherited_channel)return -EINVAL;
  wchar_t *end;
  unsigned long long value=wcstoull(text,&end,16);
  if(!value || *end || value>UINTPTR_MAX)return -EINVAL;
  HANDLE pipe=(HANDLE)(uintptr_t)value;
  ULONG owner=0;
  if(!SetHandleInformation(pipe,HANDLE_FLAG_INHERIT,0) ||
      !GetNamedPipeServerProcessId(pipe,&owner) || !owner || owner==GetCurrentProcessId())
    return -win_ipc_error(GetLastError());
  HANDLE peer=OpenProcess(PROCESS_DUP_HANDLE,FALSE,owner);
  if(!peer) {
    int error=win_ipc_error(GetLastError());
    CloseHandle(pipe);
    return -error;
  }
  inherited_channel=win_channel_open(pipe,peer,owner);
  return inherited_channel ? 0 : -errno;
}

struct luna_process_channel *luna_process_channel_open(int fd)
{
  if(fd!=3 || !inherited_channel) {
    errno=EBADF;
    return NULL;
  }
  struct luna_process_channel *channel=inherited_channel;
  inherited_channel=NULL;
  return channel;
}

static int export_fd(struct luna_process_channel *c,int fd,struct win_ipc_attachment *attachment)
{
  memset(attachment,0,sizeof *attachment);
  int nonblock=luna_fd_get_nonblock(fd);
  if(nonblock<0)return -errno;
  attachment->nonblock=nonblock!=0;
  intptr_t socket=luna_socket_native(fd);
  if(socket!=-1) {
    attachment->kind=WIN_IPC_SOCKET;
    if(WSADuplicateSocketW((SOCKET)socket,c->peer_pid,&attachment->value.socket))
      return -EIO;
  } else {
    intptr_t native=_get_osfhandle(fd);
    if(native==-1)return -EBADF;
    DWORD type=GetFileType((HANDLE)native);
    if(type!=FILE_TYPE_DISK && type!=FILE_TYPE_PIPE)return -ENOTSUP;
    /* The receiving broker imports this from the authenticated peer. The
     * queued fd remains open in this process until the receiver acknowledges it. */
    attachment->kind=WIN_IPC_FILE;
    attachment->value.file=(uintptr_t)native;
  }
  return 0;
}

static int import_fd(struct luna_process_channel *c,struct win_ipc_attachment *attachment)
{
  if(attachment->nonblock>1)return -EPROTO;
  if(attachment->kind==WIN_IPC_SOCKET) {
    int fd=luna_win_ipc_import_socket(&attachment->value.socket,attachment->nonblock);
    return fd<0 ? -errno : fd;
  }
  if(attachment->kind!=WIN_IPC_FILE || !attachment->value.file)return -EPROTO;
  HANDLE file=NULL;
  if(!DuplicateHandle(c->peer,(HANDLE)attachment->value.file,GetCurrentProcess(),&file,
                      0,FALSE,DUPLICATE_SAME_ACCESS))return -win_ipc_error(GetLastError());
  DWORD type=GetFileType(file);
  if(type!=FILE_TYPE_PIPE && type!=FILE_TYPE_DISK) {
    CloseHandle(file);
    return -EPROTO;
  }
  int fd=_open_osfhandle((intptr_t)file,_O_BINARY|_O_NOINHERIT|_O_RDWR);
  if(fd<0) {
    int error=errno;
    CloseHandle(file);
    return -error;
  }
  if(attachment->nonblock && luna_fd_nonblock(fd,1)) {
    int error=errno;
    luna_fd_close(fd);
    return -error;
  }
  return fd;
}

static void win_channel_fail(struct luna_process_channel *c,int rc)
{
  if(c->error)return;
  c->error=-rc;
  CloseHandle(c->pipe);
  c->pipe=INVALID_HANDLE_VALUE;
  packets_free(c->head);
  packets_free(c->sent_head);
  c->head=c->tail=c->sent_head=c->sent_tail=NULL;
  c->queued=0;
}

static int win_write_packet(HANDLE pipe,const void *data,size_t size)
{
  DWORD written=0;
  if(!WriteFile(pipe,data,(DWORD)size,&written,NULL))
    return -win_ipc_error(GetLastError());
  /* A nonblocking message-mode pipe writes either the complete frame or zero. */
  if(!written)return 0;
  return written==size ? 1 : -EPROTO;
}

static int win_flush(struct luna_process_channel *c)
{
  if(c->ack) {
    unsigned char frame[WIN_IPC_HEADER]={'L','S','A','2'};
    put32(frame+12,c->ack);
    int rc=win_write_packet(c->pipe,frame,sizeof frame);
    if(rc<=0)return rc;
    c->ack=0;
  }
  while(c->head) {
    struct packet *p=c->head;
    int rc=win_write_packet(c->pipe,p->wire,p->size);
    if(rc<=0)return rc;
    c->head=p->next;
    if(!c->head)c->tail=NULL;
    if(!p->nfds) {
      c->queued-=p->size;
      packet_free(p);
      continue;
    }
    free(p->wire);
    p->wire=NULL;
    p->next=NULL;
    if(c->sent_tail)c->sent_tail->next=p;
    else c->sent_head=p;
    c->sent_tail=p;
  }
  return 0;
}

int luna_process_channel_send_fds(struct luna_process_channel *c,const void *data,size_t size,
                                 const int *fds,size_t nfds)
{
  if(!c || (!data && size) || (nfds && !fds) || nfds>LUNA_PROCESS_FD_MAX)return -EINVAL;
  if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
  size_t wire_size=WIN_IPC_HEADER+nfds*sizeof(struct win_ipc_attachment)+size;
  pthread_mutex_lock(&c->lock);
  int rc=c->error ? -c->error : 0;
  if(!rc && c->queued+wire_size>QUEUE_MAX)rc=-ENOBUFS;
  if(!rc && nfds && c->sequence==UINT32_MAX)rc=-EOVERFLOW;
  struct packet *p=rc ? NULL : calloc(1,sizeof *p);
  if(!rc && (!p || !(p->wire=malloc(wire_size))))rc=-ENOMEM;
  if(!rc)rc=fds_copy(fds,nfds,&p->fds);
  if(!rc) {
    p->nfds=nfds;
    p->size=wire_size;
    p->sequence=nfds ? c->sequence+1 : 0;
    memcpy(p->wire,"LSP2",4);
    put32(p->wire+4,(uint32_t)size);
    put32(p->wire+8,(uint32_t)nfds);
    put32(p->wire+12,p->sequence);
    for(size_t i=0; i<nfds && !rc; ++i) {
      struct win_ipc_attachment attachment;
      rc=export_fd(c,p->fds[i],&attachment);
      if(!rc)memcpy(p->wire+WIN_IPC_HEADER+i*sizeof attachment,&attachment,sizeof attachment);
    }
  }
  if(!rc) {
    if(nfds)c->sequence=p->sequence;
    if(size)memcpy(p->wire+wire_size-size,data,size);
    if(c->tail)c->tail->next=p;
    else c->head=p;
    c->tail=p;
    c->queued+=wire_size;
    rc=win_flush(c);
    if(rc)win_channel_fail(c,rc);
  } else {
    packet_free(p);
  }
  pthread_mutex_unlock(&c->lock);
  return rc;
}

int luna_process_channel_send(struct luna_process_channel *c,const void *data,size_t size)
{
  return luna_process_channel_send_fds(c,data,size,NULL,0);
}

static int win_read_packet(struct luna_process_channel *c)
{
  DWORD available=0,length=0;
  if(!PeekNamedPipe(c->pipe,NULL,0,NULL,&available,&length))
    return -win_ipc_error(GetLastError());
  if(!available)return 0;
  if(length<WIN_IPC_HEADER || length>WIN_IPC_MAX)return -EPROTO;
  unsigned char *frame=malloc(length);
  if(!frame)return -ENOMEM;
  DWORD got=0;
  if(!ReadFile(c->pipe,frame,length,&got,NULL) || got!=length) {
    free(frame);
    return -EPROTO;
  }
  uint32_t size=get32(frame+4),nfds=get32(frame+8),sequence=get32(frame+12);
  int rc=-EPROTO;
  if(!memcmp(frame,"LSA2",4) && length==WIN_IPC_HEADER && !size && !nfds &&
      c->sent_head && sequence>=c->sent_head->sequence && sequence<=c->sent_tail->sequence) {
    while(c->sent_head && c->sent_head->sequence<=sequence) {
      struct packet *p=c->sent_head;
      c->sent_head=p->next;
      c->queued-=p->size;
      packet_free(p);
    }
    if(!c->sent_head)c->sent_tail=NULL;
    rc=1;
  } else if(!memcmp(frame,"LSP2",4) && size<=LUNA_PROCESS_MESSAGE_MAX &&
      nfds<=LUNA_PROCESS_FD_MAX && length==WIN_IPC_HEADER+nfds*sizeof(struct win_ipc_attachment)+size &&
      (nfds ? sequence && sequence==c->received_sequence+1 : !sequence)) {
    struct packet *p=calloc(1,sizeof *p+size);
    rc=-ENOMEM;
    if(p && (!nfds || (p->fds=malloc(nfds*sizeof *p->fds)))) {
      rc=1;
      for(size_t i=0; i<nfds; ++i) {
        struct win_ipc_attachment attachment;
        memcpy(&attachment,frame+WIN_IPC_HEADER+i*sizeof attachment,sizeof attachment);
        int fd=import_fd(c,&attachment);
        if(fd<0) {
          rc=fd;
          break;
        }
        p->fds[p->nfds++]=fd;
      }
    }
    if(rc>0) {
      if(nfds)c->received_sequence=c->ack=sequence;
      p->size=size;
      if(size)memcpy(p->bytes,frame+length-size,size);
      if(c->ready_tail)c->ready_tail->next=p;
      else c->ready_head=p;
      c->ready_tail=p;
    } else {
      packet_free(p);
    }
  }
  free(frame);
  return rc;
}

int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn callback,void *user)
{
  if(!c)return -EINVAL;
  pthread_mutex_lock(&c->lock);
  int rc=c->error ? -c->error : win_flush(c);
  for(unsigned n=0; !rc && n<32; ++n) {
    int incoming=win_read_packet(c);
    if(incoming<=0) {
      rc=incoming;
      break;
    }
  }
  if(!rc)rc=win_flush(c);
  if(rc)win_channel_fail(c,rc);
  pthread_mutex_unlock(&c->lock);

  int count=0;
  while(count<32) {
    pthread_mutex_lock(&c->lock);
    struct packet *p=c->ready_head;
    if(p) {
      c->ready_head=p->next;
      if(!c->ready_head)c->ready_tail=NULL;
    }
    pthread_mutex_unlock(&c->lock);
    if(!p)break;
    struct fd_scope saved=current_fds;
    current_fds=(struct fd_scope){p->fds,p->nfds};
    if(callback)callback(user,p->bytes,p->size);
    current_fds=saved;
    packet_free(p);
    ++count;
  }
  return rc ? rc : count;
}

void luna_process_channel_close(struct luna_process_channel *c)
{
  if(!c)return;
  if(c->pipe!=INVALID_HANDLE_VALUE)CloseHandle(c->pipe);
  CloseHandle(c->peer);
  packets_free(c->head);
  packets_free(c->sent_head);
  packets_free(c->ready_head);
  pthread_mutex_destroy(&c->lock);
  free(c);
}

static int win_pool_job_init(struct luna_process_pool *pool)
{
  pool->job=CreateJobObjectW(NULL,NULL);
  if(!pool->job)return -win_ipc_error(GetLastError());
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  return SetInformationJobObject(pool->job,JobObjectExtendedLimitInformation,&limits,sizeof limits)
    ? 0 : -win_ipc_error(GetLastError());
}

static int win_pipe_pair(HANDLE ends[2])
{
  unsigned char random[16];
  wchar_t hex[33],name[96];
  if(luna_os_random(random,sizeof random))return -EIO;
  for(unsigned i=0; i<sizeof random; ++i) {
    hex[2*i]=L"0123456789abcdef"[random[i]>>4];
    hex[2*i+1]=L"0123456789abcdef"[random[i]&15];
  }
  hex[32]=0;
  swprintf(name,sizeof name/sizeof *name,L"\\\\.\\pipe\\lunaria-guest-%lu-%ls",
           (unsigned long)GetCurrentProcessId(),hex);
  HANDLE server=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
    1,(DWORD)WIN_IPC_MAX,(DWORD)WIN_IPC_MAX,0,NULL);
  if(server==INVALID_HANDLE_VALUE)return -win_ipc_error(GetLastError());
  SECURITY_ATTRIBUTES inherit={sizeof inherit,NULL,TRUE};
  HANDLE client=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,&inherit,OPEN_EXISTING,0,NULL);
  int rc=0;
  if(client==INVALID_HANDLE_VALUE)rc=-win_ipc_error(GetLastError());
  else if(!ConnectNamedPipe(server,NULL) && GetLastError()!=ERROR_PIPE_CONNECTED)
    rc=-win_ipc_error(GetLastError());
  ULONG pid=0;
  if(!rc && (!GetNamedPipeClientProcessId(server,&pid) || pid!=GetCurrentProcessId()))rc=-EACCES;
  if(rc) {
    if(client!=INVALID_HANDLE_VALUE)CloseHandle(client);
    CloseHandle(server);
    return rc;
  }
  ends[0]=server;
  ends[1]=client;
  return 0;
}

static wchar_t *win_ipc_widen(const char *text)
{
  int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0);
  if(!size)return NULL;
  wchar_t *wide=malloc((size_t)size*sizeof *wide);
  if(wide && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,wide,size)) {
    free(wide);
    wide=NULL;
  }
  return wide;
}

static bool win_env_key(const wchar_t *entry,const wchar_t *key)
{
  size_t length=wcslen(key);
  return !_wcsnicmp(entry,key,length) && entry[length]==L'=';
}
static int win_env_order(const void *a,const void *b)
{
  return _wcsicmp(*(const wchar_t *const *)a,*(const wchar_t *const *)b);
}

static wchar_t *win_child_environment(const char *name,uint32_t pid,uint32_t parent,HANDLE control)
{
  wchar_t *process=win_ipc_widen(name);
  if(!process)return NULL;
  wchar_t pname[NAME_MAX_BYTES+32],tid[64],ppid[64],handle[64];
  int named=swprintf(pname,sizeof pname/sizeof *pname,L"ANDROID_PROCESS_NAME=%ls",process);
  free(process);
  if(named<0)return NULL;
  swprintf(tid,sizeof tid/sizeof *tid,L"LUNARIA_GUEST_TID_BASE=%u",pid);
  swprintf(ppid,sizeof ppid/sizeof *ppid,L"LUNARIA_GUEST_PPID=%u",parent);
  swprintf(handle,sizeof handle/sizeof *handle,WIN_IPC_PIPE_ENV L"=%llx",
           (unsigned long long)(uintptr_t)control);
  wchar_t *env=GetEnvironmentStringsW();
  if(!env)return NULL;
  size_t count=0;
  for(wchar_t *entry=env; *entry; entry+=wcslen(entry)+1)++count;
  const wchar_t **entries=malloc((count+4)*sizeof *entries);
  wchar_t *block=NULL;
  if(entries) {
    count=0;
    for(wchar_t *entry=env; *entry; entry+=wcslen(entry)+1) {
      if(win_env_key(entry,L"ANDROID_PROCESS_NAME") || win_env_key(entry,L"LUNARIA_GUEST_TID_BASE") ||
          win_env_key(entry,L"LUNARIA_GUEST_PPID") || win_env_key(entry,WIN_IPC_PIPE_ENV))continue;
      entries[count++]=entry;
    }
    entries[count++]=pname;
    entries[count++]=tid;
    entries[count++]=ppid;
    entries[count++]=handle;
    qsort(entries,count,sizeof *entries,win_env_order);
    size_t length=1;
    for(size_t i=0; i<count; ++i)length+=wcslen(entries[i])+1;
    block=malloc(length*sizeof *block);
    if(block) {
      wchar_t *at=block;
      for(size_t i=0; i<count; ++i) {
        size_t n=wcslen(entries[i])+1;
        memcpy(at,entries[i],n*sizeof *at);
        at+=n;
      }
      *at=0;
    }
    free(entries);
  }
  FreeEnvironmentStringsW(env);
  return block;
}

static HANDLE win_inherit_standard(DWORD id)
{
  HANDLE original=GetStdHandle(id),copy=NULL;
  if(original && original!=INVALID_HANDLE_VALUE &&
      DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&copy,0,TRUE,
                      DUPLICATE_SAME_ACCESS))return copy;
  SECURITY_ATTRIBUTES inherit={sizeof inherit,NULL,TRUE};
  return CreateFileW(L"NUL",id==STD_INPUT_HANDLE ? GENERIC_READ : GENERIC_WRITE,
                     FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,NULL);
}

static int spawn_child(struct luna_process_pool *pool,const char *name,struct child **out)
{
  if(pool->next_pid>PID_LIMIT-PID_STRIDE)return -EAGAIN;
  if(strcmp(pool->mode,"--service-process-arm64") && strcmp(pool->mode,"--service-process-arm32"))
    return -EINVAL;
  struct child *child=calloc(1,sizeof *child);
  if(!child)return -ENOMEM;
  child->name=strdup(name);
  HANDLE ends[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
  int rc=child->name ? win_pipe_pair(ends) : -ENOMEM;
  if(rc)goto fail;

  wchar_t *exe=win_ipc_widen(pool->executable);
  wchar_t *mode=win_ipc_widen(pool->mode);
  wchar_t *env=win_child_environment(name,pool->next_pid,pool->parent_pid,ends[1]);
  size_t length=exe && mode ? wcslen(exe)+wcslen(mode)+8 : 0;
  wchar_t *command=length ? malloc(length*sizeof *command) : NULL;
  if(!env || !command)rc=-ENOMEM;
  if(!rc && swprintf(command,length,L"\"%ls\" %ls 3",exe,mode)<0)rc=-EINVAL;

  HANDLE inherited[]={ends[1],INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
  DWORD standard[]={STD_INPUT_HANDLE,STD_OUTPUT_HANDLE,STD_ERROR_HANDLE};
  for(unsigned i=0; i<3 && !rc; ++i) {
    inherited[i+1]=win_inherit_standard(standard[i]);
    if(inherited[i+1]==INVALID_HANDLE_VALUE)rc=-win_ipc_error(GetLastError());
  }
  STARTUPINFOEXW startup={0};
  startup.StartupInfo.cb=sizeof startup;
  startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput=inherited[1];
  startup.StartupInfo.hStdOutput=inherited[2];
  startup.StartupInfo.hStdError=inherited[3];
  SIZE_T bytes=0;
  InitializeProcThreadAttributeList(NULL,1,0,&bytes);
  LPPROC_THREAD_ATTRIBUTE_LIST attributes=bytes ? malloc(bytes) : NULL;
  bool ready=false;
  if(!rc && !attributes)rc=-ENOMEM;
  if(!rc && !(ready=InitializeProcThreadAttributeList(attributes,1,0,&bytes)!=0))
    rc=-win_ipc_error(GetLastError());
  if(!rc && !UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherited,sizeof inherited,NULL,NULL))
    rc=-win_ipc_error(GetLastError());
  startup.lpAttributeList=attributes;
  PROCESS_INFORMATION process={0};
  if(!rc && !CreateProcessW(exe,command,NULL,NULL,TRUE,
      CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT,
      env,NULL,&startup.StartupInfo,&process))rc=-win_ipc_error(GetLastError());
  if(ready)DeleteProcThreadAttributeList(attributes);
  free(attributes);
  for(unsigned i=1; i<=3; ++i)if(inherited[i]!=INVALID_HANDLE_VALUE)CloseHandle(inherited[i]);
  free(exe);
  free(mode);
  free(command);
  free(env);
  CloseHandle(ends[1]);
  ends[1]=INVALID_HANDLE_VALUE;

  if(!rc && !AssignProcessToJobObject(pool->job,process.hProcess))
    rc=-win_ipc_error(GetLastError());
  if(!rc) {
    HANDLE peer=NULL;
    if(!DuplicateHandle(GetCurrentProcess(),process.hProcess,GetCurrentProcess(),&peer,
                       PROCESS_DUP_HANDLE,FALSE,0))rc=-win_ipc_error(GetLastError());
    else {
      child->channel=win_channel_open(ends[0],peer,process.dwProcessId);
      ends[0]=INVALID_HANDLE_VALUE;
      if(!child->channel)rc=-errno;
    }
  }
  if(!rc && ResumeThread(process.hThread)==(DWORD)-1)rc=-win_ipc_error(GetLastError());
  if(process.hThread)CloseHandle(process.hThread);
  if(rc) {
    if(process.hProcess) {
      TerminateProcess(process.hProcess,1);
      WaitForSingleObject(process.hProcess,2000);
      CloseHandle(process.hProcess);
    }
    goto fail;
  }
  child->handle=process.hProcess;
  child->host_pid=process.dwProcessId;
  child->guest_pid=pool->next_pid;
  pool->next_pid+=PID_STRIDE;
  child->next=pool->children;
  pool->children=child;
  *out=child;
  return 0;
fail:
  if(ends[0]!=INVALID_HANDLE_VALUE)CloseHandle(ends[0]);
  if(ends[1]!=INVALID_HANDLE_VALUE)CloseHandle(ends[1]);
  luna_process_channel_close(child->channel);
  free(child->name);
  free(child);
  return rc;
}

/* The Windows component pool keeps its own child lifecycle records. The
 * Linux system-wide Android process registry is separate from this transport. */
int luna_process_state_open(const char *directory) { (void)directory; return -ENOSYS; }
int luna_process_state_publish(const struct luna_process_info *info) { (void)info; return -ENOSYS; }
int luna_process_state_list(struct luna_process_info **out,size_t *count)
{
  if(out)*out=NULL;
  if(count)*count=0;
  return -ENOSYS;
}
