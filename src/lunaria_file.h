/* Host file management: process scratch files and persistent guest links. */
#ifndef LUNARIA_FILE_H
#define LUNARIA_FILE_H
#include <sys/stat.h>
#include "lunaria_os.h"
#ifdef __cplusplus
extern "C" {
#endif
int luna_temp_path(char *path,size_t capacity,const char *name);
int luna_temp_mkstemp(char *pattern,size_t capacity);
char *luna_temp_mkdtemp(char *pattern,size_t capacity);

typedef const char *(*luna_symlink_mapper)(const char *,char *,size_t);
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
ptrdiff_t luna_symlink_readlink(const char *path,char *out,size_t capacity);
int luna_symlink_accessat(int dirfd,const char *path,int mode,int flags);
int luna_symlink_access(const char *path,int mode);
char *luna_symlink_realpath(const char *path,char *out,size_t capacity);
luna_directory *luna_symlink_directory_open(const char *path);
luna_directory *luna_symlink_directory_open_fd(int fd);
luna_directory_entry *luna_symlink_directory_read(luna_directory *dir);
int luna_symlink_directory_close(luna_directory *dir);
#ifdef __cplusplus
}
#endif
#endif
