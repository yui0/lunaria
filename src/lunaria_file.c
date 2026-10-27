/* Host file management; plain C, shared by the VM and native engine.
 * Owns process scratch files and the .sym. guest-link representation. */
#define LUNARIA_SYMLINK_NATIVE
#include "lunaria_os.h"
#include "lunaria_file.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t temp_lock = PTHREAD_MUTEX_INITIALIZER;
static char temp_root[4096];
static long temp_owner;
static int temp_registered;

/* Never follow a link out of our private namespace. Only this process's
 * mkdtemp directory is visited, never a glob of other Lunaria executions. */
static void temp_remove(const char *path)
{
   luna_file_info info;
   if (luna_file_infoat(LUNA_AT_FDCWD, path, &info, LUNA_AT_NOFOLLOW)) return;
   if ((info.mode & 0170000) != 0040000) {
      (void)luna_file_unlink(path);
      return;
   }
   (void)luna_file_chmodat(LUNA_AT_FDCWD, path, 0700, 0);
   luna_directory *dir = luna_directory_open(path);
   if (!dir) return;
   luna_directory_entry *entry;
   while ((entry = luna_directory_read(dir))) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      size_t length = strlen(path) + strlen(entry->d_name) + 2;
      char *child = malloc(length);
      if (!child) continue;
      snprintf(child, length, "%s/%s", path, entry->d_name);
      temp_remove(child);
      free(child);
   }
   luna_directory_close(dir);
   (void)luna_file_unlinkat(LUNA_AT_FDCWD, path, LUNA_AT_REMOVEDIR);
}

static void temp_finish(void)
{
   /* fork children inherit atexit handlers but do not own the parent's files. */
   if (temp_owner != (long)getpid()) return;
   pthread_mutex_lock(&temp_lock);
   if (temp_root[0]) temp_remove(temp_root);
   temp_root[0] = 0;
   pthread_mutex_unlock(&temp_lock);
}

int luna_temp_path(char *path, size_t capacity, const char *name)
{
   if (!name || !*name || strchr(name, '/') || strchr(name, '\\') ||
       !strcmp(name, ".") || !strcmp(name, "..")) { errno = EINVAL; return -1; }
   pthread_mutex_lock(&temp_lock);
   if (temp_owner != (long)getpid()) temp_root[0] = 0;
   if (!temp_root[0]) {
      char base[4096];
      if (luna_file_temp_directory(base, sizeof base) ||
          (size_t)snprintf(temp_root, sizeof temp_root, "%s/lunaria-XXXXXX", base) >= sizeof temp_root ||
          !luna_file_mkdtemp(temp_root)) {
         temp_root[0] = 0;
         pthread_mutex_unlock(&temp_lock);
         return -1;
      }
      temp_owner = (long)getpid();
      if (!temp_registered) {
         if (atexit(temp_finish)) {
            temp_remove(temp_root); temp_root[0] = 0;
            pthread_mutex_unlock(&temp_lock);
            errno = ENOMEM; return -1;
         }
         temp_registered = 1;
      }
   }
   int result = snprintf(path, capacity, "%s/%s", temp_root, name);
   pthread_mutex_unlock(&temp_lock);
   if (result < 0 || (size_t)result >= capacity) { errno = ENAMETOOLONG; return -1; }
   return 0;
}

static int temp_rebase(char *pattern, size_t capacity)
{
   const char *leaf = strrchr(pattern, '/');
   leaf = leaf ? leaf + 1 : pattern;
   char *name = strdup(leaf);
   if (!name) { errno = ENOMEM; return -1; }
   int result = luna_temp_path(pattern, capacity, name);
   free(name);
   return result;
}

int luna_temp_mkstemp(char *pattern, size_t capacity)
{
   return temp_rebase(pattern, capacity) ? -1 : luna_file_mkstemp(pattern);
}

char *luna_temp_mkdtemp(char *pattern, size_t capacity)
{
   return temp_rebase(pattern, capacity) ? NULL : luna_file_mkdtemp(pattern);
}

/* C filesystem bridge. The metadata is ordinary UTF-8 text on every host;
 * it is never substituted by copying the target's data. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#define SYM_PATH 8192
static const char marker[]="#!/symlink\n";
static luna_symlink_mapper path_mapper;
static luna_symlink_mapper reverse_mapper;
static pthread_mutex_t sym_lock;
static pthread_once_t sym_once=PTHREAD_ONCE_INIT;
static void sym_init(void)
{
   pthread_mutexattr_t attr;
   pthread_mutexattr_init(&attr);
   pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&sym_lock,&attr);
   pthread_mutexattr_destroy(&attr);
}
static void sym_enter(void) {pthread_once(&sym_once,sym_init);pthread_mutex_lock(&sym_lock);}
static void sym_leave(void) {int e=errno;pthread_mutex_unlock(&sym_lock);errno=e;}
void luna_symlink_set_mapper(luna_symlink_mapper mapper,luna_symlink_mapper reverse)
{sym_enter();path_mapper=mapper;reverse_mapper=reverse;sym_leave();}
static luna_file_inode_mapper inode_mapper;
static unsigned file_uid,file_gid;
void luna_file_set_credentials(unsigned uid,unsigned gid,luna_file_inode_mapper mapper)
{sym_enter();file_uid=uid;file_gid=gid;inode_mapper=mapper;sym_leave();}
/* Ownership follows the inode, including paths containing parent components.
 * Search checks still run on the original components before normalization. */
static int sym_inode(const char *path,luna_file_info *info)
{
   if (!inode_mapper) return 0;
   size_t n=strlen(path);char actual[SYM_PATH];
   if (strstr(path,"/../") || strstr(path,"/./") ||
       (n>=3 && !strcmp(path+n-3,"/..")) || (n>=2 && !strcmp(path+n-2,"/."))) {
      if (!luna_file_realpath(path,actual,sizeof actual)) return 0;
      path=actual;
   }
   return inode_mapper(path,info);
}
/* Evaluate DAC using guest credentials even when the host process is root. */
static int sym_permission(const char *path,int mode,int directory)
{
   if (!inode_mapper) return 0;
   luna_file_info info;
   if (luna_file_infoat(LUNA_AT_FDCWD,path,&info,0)) return -1;
   if (directory && (info.mode&0170000)!=0040000) {errno=ENOTDIR;return -1;}
   if (!sym_inode(path,&info)) return 0;
   unsigned bits=info.mode>>(file_uid==info.uid ? 6 : file_gid==info.gid ? 3 : 0);
   if (((unsigned)mode&bits)==(unsigned)mode) return 0;
   errno=EACCES;return -1;
}
static int sym_parent_permission(const char *path,int mode)
{
   if (!inode_mapper) return 0;
   char parent[SYM_PATH];size_t n=strlen(path);
   if (n>=sizeof parent) {errno=ENAMETOOLONG;return -1;}
   memcpy(parent,path,n+1);
   while (n>1 && parent[n-1]=='/') parent[--n]=0;
   char *slash=strrchr(parent,'/');
   if (!slash) return sym_permission(".",mode,1);
   if (slash==parent) slash[1]=0;else *slash=0;
   return sym_permission(parent,mode,1);
}
static int sym_open_permission(const char *path,int flags)
{
   if (!inode_mapper) return 0;
   luna_file_info info;
   if (luna_file_infoat(LUNA_AT_FDCWD,path,&info,0)) {
      if (errno==ENOENT && (flags&O_CREAT)) return sym_parent_permission(path,3);
      return -1;
   }
   if ((flags&O_CREAT) && (flags&O_EXCL)) {errno=EEXIST;return -1;}
   #ifdef O_PATH
   if (flags&O_PATH) return 0;
#endif
   int access=flags&O_ACCMODE;
   return sym_permission(path,(access==O_WRONLY ? 0 : 4) |
                              (access==O_RDONLY && !(flags&O_TRUNC) ? 0 : 2),0);
}
static int sym_owner_permission(const char *path)
{
   if (!inode_mapper) return 0;
   luna_file_info info;
   if (luna_file_infoat(LUNA_AT_FDCWD,path,&info,0)) return -1;
   if (!sym_inode(path,&info) || info.uid==file_uid) return 0;
   errno=EPERM;return -1;
}
static int sym_copy(char *out,size_t cap,const char *text)
{
   size_t n=strlen(text);
   if (n>=cap) {errno=ENAMETOOLONG;return -1;}
   memcpy(out,text,n+1);return 0;
}
static int sym_name(const char *path,char *out,size_t cap)
{
   const char *leaf=strrchr(path,'/');leaf=leaf ? leaf+1 : path;
   if (!*leaf || !strcmp(leaf,".") || !strcmp(leaf,"..")) {errno=EINVAL;return -1;}
   size_t parent=(size_t)(leaf-path),len=strlen(leaf);
   if (parent+5+len>=cap) {errno=ENAMETOOLONG;return -1;}
   memcpy(out,path,parent);memcpy(out+parent,".sym.",5);memcpy(out+parent+5,leaf,len+1);return 0;
}
/* 1 means valid metadata; 0 means an ordinary file or no file. Errors other
 * than absence propagate rather than making an inaccessible link disappear. */
static int sym_text(const char *metadata,char *target,size_t cap)
{
   int fd=luna_file_open(metadata,O_RDONLY|LUNA_FILE_CLOEXEC|LUNA_FILE_NOFOLLOW,0);
   if (fd<0) return errno==ENOENT || errno==ENOTDIR || errno==ELOOP ? 0 : -1;
   char header[sizeof marker-1];size_t have=0;
   while (have<sizeof header) {
      ptrdiff_t n=luna_fd_read(fd,header+have,sizeof header-have);
      if (n<0 && errno==EINTR) continue;
      if (n<=0) {luna_fd_close(fd);return n<0 ? -1 : 0;}
      have+=(size_t)n;
   }
   if (memcmp(header,marker,sizeof header)) {luna_fd_close(fd);return 0;}
   have=0;
   while (have<cap) {
      ptrdiff_t n=luna_fd_read(fd,target+have,cap-have);
      if (n<0 && errno==EINTR) continue;
      if (n<0) {int e=errno;luna_fd_close(fd);errno=e;return -1;}
      if (!n) break;
      have+=(size_t)n;
   }
   luna_fd_close(fd);
   if (!have || have>=cap || memchr(target,0,have)) return 0;
   target[have]=0;return 1;
}
static int sym_probe(const char *path,char *metadata,char *target)
{
   luna_file_info info;
   if (!luna_file_infoat(LUNA_AT_FDCWD,path,&info,LUNA_AT_NOFOLLOW)) return 0;
   if (errno!=ENOENT && errno!=ENOTDIR) return -1;
   if (sym_name(path,metadata,SYM_PATH)) return 0;
   return sym_text(metadata,target,SYM_PATH);
}
static int sym_hidden(const char *path)
{
   const char *leaf=strrchr(path,'/');leaf=leaf ? leaf+1 : path;
   if (strncmp(leaf,".sym.",5) && strncmp(leaf,".sym-stage-",11)) return 0;
   char target[SYM_PATH];int text=sym_text(path,target,sizeof target);
   if (text!=1) return text;
   if (!strncmp(leaf,".sym-stage-",11)) return 1;
   char logical[SYM_PATH];size_t parent=(size_t)(leaf-path);
   if (parent+strlen(leaf+5)>=sizeof logical) return 0;
   memcpy(logical,path,parent);strcpy(logical+parent,leaf+5);
   luna_file_info info;
   return luna_file_infoat(LUNA_AT_FDCWD,logical,&info,LUNA_AT_NOFOLLOW) && errno==ENOENT;
}

/* Expand directory links in the guest namespace before lexical '..' removal
 * or host mapping. Doing those in the opposite order changes link/../file. */
int luna_symlink_namespace(const char *path,char *out,size_t cap,luna_symlink_mapper raw)
{
   if (!path || path[0]!='/') {errno=EINVAL;return -1;}
   sym_enter();char work[SYM_PATH],prefix[SYM_PATH];int result=-1;unsigned hops=0;
   if (sym_copy(work,sizeof work,path)) goto done;
restart_namespace:
   strcpy(prefix,"/");size_t used=1;
   for (size_t pos=1;;) {
      while (work[pos]=='/') ++pos;
      if (!work[pos]) break;
      size_t start=pos;while (work[pos] && work[pos]!='/') ++pos;
      size_t length=pos-start;
      char host[SYM_PATH];
      if ((length==1 && work[start]=='.') || (length==2 && work[start]=='.' && work[start+1]=='.')) {
         const char *physical=raw(prefix,host,sizeof host);luna_file_info info;
         if (!physical || luna_file_infoat(LUNA_AT_FDCWD,physical,&info,0)) goto done;
         if ((info.mode&0170000)!=0040000) {errno=ENOTDIR;goto done;}
         if (sym_permission(physical,1,1)) goto done;
         if (length==2 && used>1) {
            while (used>1 && prefix[used-1]!='/') --used;
            if (used>1) --used;
            prefix[used]=0;
         }
         continue;
      }
      if (used+length+1>=sizeof prefix) {errno=ENAMETOOLONG;goto done;}
      if (used>1) prefix[used++]='/';
      memcpy(prefix+used,work+start,length);used+=length;prefix[used]=0;
      if (!work[pos]) break; /* final link belongs to open/lstat/unlink */
      const char *physical=raw(prefix,host,sizeof host);
      if (!physical) goto done;
      char metadata[SYM_PATH],target[SYM_PATH];int link=sym_probe(physical,metadata,target);
      if (link<0) goto done;
      if (link) {
         if (++hops>40) {errno=ELOOP;goto done;}
         char next[SYM_PATH];int n;
         if (target[0]=='/') n=snprintf(next,sizeof next,"%s%s",target,work+pos);
         else {
            const char *slash=strrchr(prefix,'/');size_t parent=(size_t)(slash-prefix+1);
            n=snprintf(next,sizeof next,"%.*s%s%s",(int)parent,prefix,target,work+pos);
         }
         if (n<0 || (size_t)n>=sizeof next) {errno=ENAMETOOLONG;goto done;}
         strcpy(work,next);goto restart_namespace;
      }
   }
   if (work[strlen(work)-1]=='/' && used>1) {prefix[used++]='/';prefix[used]=0;}
   result=sym_copy(out,cap,prefix);
done:sym_leave();return result;
}
static int sym_resolve(const char *path,char *out,size_t cap,int follow)
{
   if (!path) {errno=EFAULT;return -1;}
   if (!*path) {errno=ENOENT;return -1;}
   /* Ordinary existing paths need no component walk or metadata file reads. */
   luna_file_info existing;
   if (!strstr(path,".sym.") && !strstr(path,".sym-stage-") &&
       !luna_file_infoat(LUNA_AT_FDCWD,path,&existing,LUNA_AT_NOFOLLOW)) {
      if (inode_mapper) {
         char prefix[SYM_PATH];if (sym_copy(prefix,sizeof prefix,path)) return -1;
         for (size_t i=1;prefix[i];++i) if (prefix[i]=='/' && prefix[i-1]!='/') {
            prefix[i]=0;int r=sym_permission(prefix,1,1);prefix[i]='/';if (r) return -1;
         }
      }
      return sym_copy(out,cap,path);
   }
   char work[SYM_PATH];if (sym_copy(work,sizeof work,path)) return -1;
   unsigned hops=0;
restart:
   for (size_t i=0;;++i) {
      if (work[i] && work[i]!='/') continue;
      if (!i || (i && work[i-1]=='/')) {if (!work[i]) break;continue;}
      char prefix[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];
      memcpy(prefix,work,i);prefix[i]=0;
      int hidden=sym_hidden(prefix);
      if (hidden<0) return -1;
      if (hidden) {errno=ENOENT;return -1;}
      int link=sym_probe(prefix,metadata,target);
      if (link<0) return -1;
      if (!link && work[i] && sym_permission(prefix,1,1)) return -1;
      if (link && (follow || work[i])) {
         if (++hops>40) {errno=ELOOP;return -1;}
         char next[SYM_PATH],mapped[SYM_PATH];const char *dest=target;
         int absolute=target[0]=='/' || (target[0] && target[1]==':');
         if (absolute && path_mapper) {dest=path_mapper(target,mapped,sizeof mapped);if (!dest) return -1;}
         int n;
         if (absolute) n=snprintf(next,sizeof next,"%s%s",dest,work+i);
         else {
            char guest[SYM_PATH],logical[SYM_PATH];const char *anchor=prefix;
            if (reverse_mapper) {anchor=reverse_mapper(prefix,guest,sizeof guest);if (!anchor) return -1;}
            const char *slash=strrchr(anchor,'/');size_t parent=slash ? (size_t)(slash-anchor+1) : 0;
            n=snprintf(logical,sizeof logical,"%.*s%s%s",(int)parent,anchor,target,work+i);
            if (n<0 || (size_t)n>=sizeof logical) {errno=ENAMETOOLONG;return -1;}
            if (path_mapper && anchor!=prefix) {
               dest=path_mapper(logical,mapped,sizeof mapped);if (!dest) return -1;
               n=snprintf(next,sizeof next,"%s",dest);
            } else n=snprintf(next,sizeof next,"%s",logical);
         }
         if (n<0 || (size_t)n>=sizeof next) {errno=ENAMETOOLONG;return -1;}
         strcpy(work,next);goto restart;
      }
      if (!work[i]) break;
   }
   return sym_copy(out,cap,work);
}
int luna_symlink_resolve(const char *path,char *out,size_t cap,int follow)
{sym_enter();int r=sym_resolve(path,out,cap,follow);sym_leave();return r;}

static int sym_fd_path(int fd,char *out,size_t cap)
{
#ifdef _WIN32
   wchar_t wide[SYM_PATH];
   intptr_t h=_get_osfhandle(fd);if (h==-1) {errno=EBADF;return -1;}
   DWORD n=GetFinalPathNameByHandleW((HANDLE)h,wide,SYM_PATH,FILE_NAME_NORMALIZED);
   if (!n || n>=SYM_PATH) {errno=EBADF;return -1;}
   const wchar_t *p=wide;
   if (!wcsncmp(p,L"\\\\?\\UNC\\",8)) {p+=6;wide[6]=L'\\';}
   else if (!wcsncmp(p,L"\\\\?\\",4)) p+=4;
   if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,p,-1,out,(int)cap,NULL,NULL)) {errno=ENAMETOOLONG;return -1;}
   for (char *s=out;*s;++s) if (*s=='\\') *s='/';
   return 0;
#elif defined(__APPLE__)
   char name[PATH_MAX];if (fcntl(fd,F_GETPATH,name)) return -1;
   return sym_copy(out,cap,name);
#else
   char name[64];snprintf(name,sizeof name,"/proc/self/fd/%d",fd);
   ptrdiff_t n=luna_file_readlink(name,out,cap-1);if (n<0) return -1;
   if ((size_t)n>=cap-1) {errno=ENAMETOOLONG;return -1;}
   out[n]=0;return 0;
#endif
}
static int sym_at(int fd,const char *path,char *out)
{
   if (!path) {errno=EFAULT;return -1;}
   if (fd==LUNA_AT_FDCWD || path[0]=='/' || (path[0] && path[1]==':')) return sym_copy(out,SYM_PATH,path);
   char parent[SYM_PATH];if (sym_fd_path(fd,parent,sizeof parent)) return -1;
   int n=snprintf(out,SYM_PATH,"%s/%s",parent,path);
   if (n<0 || n>=SYM_PATH) {errno=ENAMETOOLONG;return -1;}return 0;
}
static int sym_publish(const char *from,const char *to)
{
#ifdef _WIN32
   wchar_t a[SYM_PATH],b[SYM_PATH];
   if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,from,-1,a,SYM_PATH) ||
       !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,to,-1,b,SYM_PATH)) {errno=EINVAL;return -1;}
   if (MoveFileExW(a,b,0)) return 0;
   DWORD e=GetLastError();errno=e==ERROR_ALREADY_EXISTS || e==ERROR_FILE_EXISTS ? EEXIST :
      e==ERROR_ACCESS_DENIED ? EACCES : e==ERROR_PATH_NOT_FOUND ? ENOENT : EIO;return -1;
#elif defined(__linux__) && defined(SYS_renameat2)
   if (!syscall(SYS_renameat2,AT_FDCWD,from,AT_FDCWD,to,1)) return 0;
   if (errno!=ENOSYS) return -1;
   if (luna_file_link(from,to)) return -1;
   return luna_file_unlink(from);
#elif defined(__APPLE__)
   return renamex_np(from,to,RENAME_EXCL);
#else
   if (luna_file_link(from,to)) return -1;
   return luna_file_unlink(from);
#endif
}
int luna_symlink_create(const char *target,const char *path)
{
   if (!target || !path) {errno=EFAULT;return -1;}
   if (!*target) {errno=ENOENT;return -1;}
   if (strlen(target)>=SYM_PATH) {errno=ENAMETOOLONG;return -1;}
   sym_enter();char actual[SYM_PATH],metadata[SYM_PATH],stage[SYM_PATH];int r=-1;
   if (sym_resolve(path,actual,sizeof actual,0) || sym_name(actual,metadata,sizeof metadata)) goto done;
   if (sym_parent_permission(actual,3)) goto done;
   luna_file_info info;
   if (!luna_file_infoat(LUNA_AT_FDCWD,actual,&info,LUNA_AT_NOFOLLOW) ||
       !luna_file_infoat(LUNA_AT_FDCWD,metadata,&info,LUNA_AT_NOFOLLOW)) {errno=EEXIST;goto done;}
   if (errno!=ENOENT) goto done;
   const char *leaf=strrchr(metadata,'/');size_t parent=leaf ? (size_t)(leaf-metadata+1) : 0;
   int length=snprintf(stage,sizeof stage,"%.*s.sym-stage-XXXXXX",(int)parent,metadata);
   if (length<0 || (size_t)length>=sizeof stage) {errno=ENAMETOOLONG;goto done;}
   int fd=luna_file_mkstemp(stage);if (fd<0) goto done;
   const char *parts[]={marker,target};size_t lengths[]={sizeof marker-1,strlen(target)};
   int good=1;
   for (int i=0;i<2 && good;++i) {
      size_t sent=0;
      while (sent<lengths[i]) {
         ptrdiff_t n=luna_fd_write(fd,parts[i]+sent,lengths[i]-sent);
         if (n<0 && errno==EINTR) continue;
         if (n<=0) {if (!n) errno=EIO;good=0;break;}sent+=(size_t)n;
      }
   }
   int error=errno;if (luna_fd_close(fd) && good) {good=0;error=errno;}
   if (good) {r=sym_publish(stage,metadata);error=errno;}
   (void)luna_file_unlink(stage);errno=error;
done: sym_leave();return r;
}

ptrdiff_t luna_symlink_readlink(const char *path,char *out,size_t cap)
{
   if (!cap) {errno=EINVAL;return -1;}
   sym_enter();char actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];ptrdiff_t r=-1;
   if (!sym_resolve(path,actual,sizeof actual,0)) {
      int link=sym_probe(actual,metadata,target);
      if (link==1) {size_t n=strlen(target);if (n>cap) n=cap;memcpy(out,target,n);r=(ptrdiff_t)n;}
      else if (!link) r=luna_file_readlink(actual,out,cap);
   }
   sym_leave();return r;
}
int luna_symlink_openat(int fd,const char *path,int flags,unsigned mode)
{
   sym_enter();char absolute[SYM_PATH],actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   int nofollow=(flags&LUNA_FILE_NOFOLLOW) || ((flags&O_CREAT)&&(flags&O_EXCL));
   if (!sym_at(fd,path,absolute) && !sym_resolve(absolute,actual,sizeof actual,!nofollow)) {
      int link=nofollow ? sym_probe(actual,metadata,target) : 0;
      if (link==1) errno=(flags&O_CREAT)&&(flags&O_EXCL) ? EEXIST : ELOOP;
      else if (!link && !sym_open_permission(actual,flags)) r=luna_file_open(actual,flags,mode);
   }
   sym_leave();return r;
}
int luna_symlink_open(const char *p,int flags,unsigned mode)
{return luna_symlink_openat(LUNA_AT_FDCWD,p,flags,mode);}
FILE *luna_symlink_fopen(const char *path,const char *mode)
{
   sym_enter();char actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];FILE *r=NULL;
   int exclusive=mode && strchr(mode,'x');
   if (!sym_resolve(path,actual,sizeof actual,!exclusive)) {
      int link=exclusive ? sym_probe(actual,metadata,target) : 0;
      int flags=mode && *mode=='r' ? O_RDONLY : O_WRONLY|O_CREAT;
      if (mode && strchr(mode,'+')) flags=(flags&~O_ACCMODE)|O_RDWR;
      if (mode && *mode=='w') flags|=O_TRUNC;
      if (exclusive) flags|=O_EXCL;
      if (link==1) errno=EEXIST;else if (!link && !sym_open_permission(actual,flags)) r=luna_file_fopen(actual,mode);
   }
   sym_leave();return r;
}
int luna_symlink_fd_info(int fd,luna_file_info *info)
{
   sym_enter();int r=luna_file_fd_info(fd,info);
   if (!r && inode_mapper) {
      char path[SYM_PATH];int saved=errno;
      if (!sym_fd_path(fd,path,sizeof path)) sym_inode(path,info);
      errno=saved;
   }
   sym_leave();return r;
}
int luna_symlink_fd_chmod(int fd,unsigned mode)
{
   sym_enter();luna_file_info info;int r=luna_symlink_fd_info(fd,&info);
   if (!r) {
      char path[SYM_PATH];
      if (inode_mapper && !sym_fd_path(fd,path,sizeof path) && sym_inode(path,&info) && info.uid!=file_uid) {
         errno=EPERM;r=-1;
      } else r=luna_file_fd_chmod(fd,mode);
   }
   sym_leave();return r;
}
int luna_symlink_infoat(int fd,const char *path,luna_file_info *info,int flags)
{
   if (flags&~(LUNA_AT_NOFOLLOW|LUNA_AT_NO_AUTOMOUNT|LUNA_AT_EMPTY_PATH)) {errno=EINVAL;return -1;}
   if (path && !*path && (flags&LUNA_AT_EMPTY_PATH) && fd!=LUNA_AT_FDCWD) return luna_symlink_fd_info(fd,info);
   sym_enter();char absolute[SYM_PATH],actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   if (!sym_at(fd,path,absolute) && !sym_resolve(absolute,actual,sizeof actual,!(flags&LUNA_AT_NOFOLLOW))) {
      int link=flags&LUNA_AT_NOFOLLOW ? sym_probe(actual,metadata,target) : 0;
      if (link==1) {
         r=luna_file_infoat(LUNA_AT_FDCWD,metadata,info,LUNA_AT_NOFOLLOW);
         if (!r) {info->mode=0120000|0777;info->size=(int64_t)strlen(target);}
      } else if (!link) r=luna_file_infoat(LUNA_AT_FDCWD,actual,info,flags);
   }
   if (!r && inode_mapper) sym_inode(actual,info);
   sym_leave();return r;
}
int luna_symlink_unlinkat(int fd,const char *path,int flags)
{
   if (flags&~LUNA_AT_REMOVEDIR) {errno=EINVAL;return -1;}
   sym_enter();char absolute[SYM_PATH],actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   if (!sym_at(fd,path,absolute) && !sym_resolve(absolute,actual,sizeof actual,0)) {
      int link=sym_probe(actual,metadata,target);
      if (sym_parent_permission(actual,3)) link=-1;
      if (link==1) {
         if (flags&LUNA_AT_REMOVEDIR) errno=ENOTDIR;else r=luna_file_unlink(metadata);
      } else if (!link) r=luna_file_unlinkat(LUNA_AT_FDCWD,actual,flags);
   }
   sym_leave();return r;
}
int luna_symlink_unlink(const char *p) {return luna_symlink_unlinkat(LUNA_AT_FDCWD,p,0);}
int luna_symlink_accessat(int fd,const char *path,int mode,int flags)
{
   if ((mode&~7) || (flags&~(LUNA_AT_NOFOLLOW|LUNA_AT_EACCESS))) {errno=EINVAL;return -1;}
   sym_enter();char absolute[SYM_PATH],actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   if (!sym_at(fd,path,absolute) && !sym_resolve(absolute,actual,sizeof actual,!(flags&LUNA_AT_NOFOLLOW))) {
      int link=flags&LUNA_AT_NOFOLLOW ? sym_probe(actual,metadata,target) : 0;
      if (link==1) r=0;else if (!link && !sym_permission(actual,mode,0)) r=luna_file_accessat(LUNA_AT_FDCWD,actual,mode,flags);
   }
   sym_leave();return r;
}
int luna_symlink_access(const char *p,int mode) {return luna_symlink_accessat(LUNA_AT_FDCWD,p,mode,0);}

int luna_symlink_mkdir(const char *path,unsigned mode)
{
   sym_enter();char actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   char name[SYM_PATH];
   if (!path) {errno=EFAULT;goto done;}
   if (sym_copy(name,sizeof name,path)) goto done;
   size_t n=strlen(name);
   while (n>1 && name[n-1]=='/' && !(n==3 && name[1]==':')) name[--n]=0;
   if (!sym_resolve(name,actual,sizeof actual,0)) {
      int link=sym_probe(actual,metadata,target);
      luna_file_info existing;
      if (link==1 || !luna_file_infoat(LUNA_AT_FDCWD,actual,&existing,LUNA_AT_NOFOLLOW)) errno=EEXIST;
      else if (!link && !sym_parent_permission(actual,3)) r=luna_file_mkdir(actual,mode);
   }
done:
   sym_leave();return r;
}
int luna_symlink_chmodat(int fd,const char *path,unsigned mode,int flags)
{
   sym_enter();char absolute[SYM_PATH],actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   if (!sym_at(fd,path,absolute) && !sym_resolve(absolute,actual,sizeof actual,!(flags&LUNA_AT_NOFOLLOW))) {
      int link=flags&LUNA_AT_NOFOLLOW ? sym_probe(actual,metadata,target) : 0;
      if (link==1) errno=EOPNOTSUPP;else if (!link && !sym_owner_permission(actual)) r=luna_file_chmodat(LUNA_AT_FDCWD,actual,mode,flags);
   }
   sym_leave();return r;
}
int luna_symlink_timesat(int fd,const char *path,const luna_file_time times[2],int flags)
{
   if (path && !*path) return luna_file_timesat(fd,path,times,flags);
   sym_enter();char absolute[SYM_PATH],actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   if (!sym_at(fd,path,absolute) && !sym_resolve(absolute,actual,sizeof actual,!(flags&LUNA_AT_NOFOLLOW))) {
      int link=flags&LUNA_AT_NOFOLLOW ? sym_probe(actual,metadata,target) : 0;
      if (link>=0 && !sym_owner_permission(link ? metadata : actual)) r=luna_file_timesat(LUNA_AT_FDCWD,link ? metadata : actual,times,flags);
   }
   sym_leave();return r;
}
int luna_symlink_set_modified(const char *path,int64_t ms)
{
   sym_enter();char actual[SYM_PATH];int r=-1;
   if (!sym_resolve(path,actual,sizeof actual,1) && !sym_owner_permission(actual)) r=luna_file_set_modified(actual,ms);
   sym_leave();return r;
}
int luna_symlink_link(const char *from,const char *to)
{
   sym_enter();char a[SYM_PATH],b[SYM_PATH],ma[SYM_PATH],mb[SYM_PATH],target[SYM_PATH];int r=-1;
   if (!sym_resolve(from,a,sizeof a,0) && !sym_resolve(to,b,sizeof b,0) && !sym_parent_permission(b,3)) {
      int sa=sym_probe(a,ma,target),sb=sym_probe(b,mb,target);
      if (sb==1) errno=EEXIST;
      else if (sa>=0 && !sb) {
         if (sa) {
            luna_file_info info;
            if (!luna_file_infoat(LUNA_AT_FDCWD,b,&info,LUNA_AT_NOFOLLOW)) errno=EEXIST;
            else if (!sym_name(b,mb,sizeof mb)) r=luna_file_link(ma,mb);
         } else r=luna_file_link(a,b);
      }
   }
   sym_leave();return r;
}
static int sym_stat(const char *path,struct stat *info,int follow)
{
   sym_enter();char actual[SYM_PATH],metadata[SYM_PATH],target[SYM_PATH];int r=-1;
   if (!sym_resolve(path,actual,sizeof actual,follow)) {
      int link=!follow ? sym_probe(actual,metadata,target) : 0;
      if (link==1) {
         r=stat(metadata,info);
         if (!r) {info->st_mode=0120000|0777;info->st_size=(off_t)strlen(target);}
      } else if (!link) {
#ifdef _WIN32
         r=stat(actual,info);
#else
         r=follow ? stat(actual,info) : lstat(actual,info);
#endif
      }
   }
   if (!r && inode_mapper) {
      luna_file_info projected={0};projected.uid=info->st_uid;projected.gid=info->st_gid;projected.mode=info->st_mode;
      if (sym_inode(actual,&projected)) {
         info->st_uid=projected.uid;info->st_gid=projected.gid;info->st_mode=projected.mode;
      }
   }
   sym_leave();return r;
}
int luna_file_stat_follow(const char *p,struct stat *info) {return sym_stat(p,info,1);}
int luna_file_stat_nofollow(const char *p,struct stat *info) {return sym_stat(p,info,0);}
char *luna_symlink_realpath(const char *path,char *out,size_t cap)
{
   sym_enter();char actual[SYM_PATH];char *r=NULL;
   if (!sym_resolve(path,actual,sizeof actual,1)) r=luna_file_realpath(actual,out,cap);
   sym_leave();return r;
}
int luna_symlink_rename(const char *from,const char *to)
{
   sym_enter();char a[SYM_PATH],b[SYM_PATH],ma[SYM_PATH],mb[SYM_PATH],target[SYM_PATH];int r=-1;
   if (sym_resolve(from,a,sizeof a,0) || sym_resolve(to,b,sizeof b,0)) goto done;
   if (sym_parent_permission(a,3) || sym_parent_permission(b,3)) goto done;
   int sa=sym_probe(a,ma,target),sb=sym_probe(b,mb,target);
   if (sa<0 || sb<0) goto done;
   luna_file_info ai,bi;int have_b=!luna_file_infoat(LUNA_AT_FDCWD,sb ? mb : b,&bi,LUNA_AT_NOFOLLOW);
   if (luna_file_infoat(LUNA_AT_FDCWD,sa ? ma : a,&ai,LUNA_AT_NOFOLLOW)) goto done;
   if (have_b && ai.device==bi.device && ai.inode==bi.inode) {r=0;goto done;}
   if (have_b && ((ai.mode&0170000)==0040000) != ((bi.mode&0170000)==0040000)) {
      errno=(ai.mode&0170000)==0040000 ? ENOTDIR : EISDIR;goto done;
   }
   if (sa==sb) {r=luna_file_rename(sa ? ma : a,sb ? mb : b);goto done;}
   if (!sa) {
      r=luna_file_rename(a,b);if (!r) r=luna_file_unlink(mb);goto done;
   }
   if (sym_name(b,mb,sizeof mb)) goto done;
   if (!luna_file_infoat(LUNA_AT_FDCWD,mb,&bi,LUNA_AT_NOFOLLOW)) {errno=EEXIST;goto done;}
   r=sym_publish(ma,mb);
   if (!r && have_b && luna_file_unlink(b)) {int e=errno;(void)luna_file_rename(mb,ma);errno=e;r=-1;}
done: sym_leave();return r;
}

struct sym_directory {luna_directory *native;char *path;luna_directory_entry entry;struct sym_directory *next;};
static struct sym_directory *directories;
static luna_directory *sym_register_dir(luna_directory *dir,const char *path)
{
   if (!dir) return NULL;
   struct sym_directory *record=calloc(1,sizeof *record);
   if (!record || !(record->path=strdup(path))) {
      free(record);luna_directory_close(dir);errno=ENOMEM;return NULL;
   }
   record->native=dir;record->next=directories;directories=record;return dir;
}
luna_directory *luna_symlink_directory_open(const char *path)
{
   sym_enter();char actual[SYM_PATH];luna_directory *r=NULL;
   if (!sym_resolve(path,actual,sizeof actual,1) && !sym_permission(actual,4,1)) r=sym_register_dir(luna_directory_open(actual),actual);
   sym_leave();return r;
}
luna_directory *luna_symlink_directory_open_fd(int fd)
{
   sym_enter();char path[SYM_PATH];luna_directory *r=NULL;
   if (!sym_fd_path(fd,path,sizeof path) && !sym_permission(path,4,1)) r=sym_register_dir(luna_directory_open_fd(fd),path);
   sym_leave();return r;
}
luna_directory_entry *luna_symlink_directory_read(luna_directory *dir)
{
   sym_enter();struct sym_directory *record=directories;
   while (record && record->native!=dir) record=record->next;
   luna_directory_entry *entry;
   while ((entry=luna_directory_read(dir)) && record) {
      if (strncmp(entry->d_name,".sym.",5) && strncmp(entry->d_name,".sym-stage-",11)) break;
      char full[SYM_PATH],target[SYM_PATH];
      int n=snprintf(full,sizeof full,"%s/%s",record->path,entry->d_name);
      if (n<0 || (size_t)n>=sizeof full || sym_text(full,target,sizeof target)!=1) break;
      if (!strncmp(entry->d_name,".sym-stage-",11)) continue;
      char logical[SYM_PATH];snprintf(logical,sizeof logical,"%s/%s",record->path,entry->d_name+5);
      luna_file_info info;if (!luna_file_infoat(LUNA_AT_FDCWD,logical,&info,LUNA_AT_NOFOLLOW)) break;
      record->entry=*entry;
      memmove(record->entry.d_name,entry->d_name+5,strlen(entry->d_name+5)+1);
      record->entry.d_type=LUNA_DT_LNK;entry=&record->entry;break;
   }
   sym_leave();return entry;
}
int luna_symlink_directory_close(luna_directory *dir)
{
   sym_enter();struct sym_directory **at=&directories;
   while (*at && (*at)->native!=dir) at=&(*at)->next;
   if (*at) {struct sym_directory *old=*at;*at=old->next;free(old->path);free(old);}
   int r=luna_directory_close(dir);sym_leave();return r;
}
