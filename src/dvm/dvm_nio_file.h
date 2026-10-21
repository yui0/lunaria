/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0 */
/* Unix NIO paths and immutable basic metadata, using the shared guest filesystem. */
static dvm_ref nio_invalid_message_ref(struct dvm *vm,dvm_ref self)
{
   union dvm_value input={0},reason={0};dvm_get_field(vm,self,"input","Ljava/lang/String;",&input);dvm_get_field(vm,self,"reason","Ljava/lang/String;",&reason);
   int32_t index=rt_get_int(vm,self,"index");char suffix[48];
   int n=index<0 ? snprintf(suffix,sizeof suffix,": ") : snprintf(suffix,sizeof suffix," at index %d: ",index);
   size_t a=dvm_string_utf8_length(vm,reason.l),b=dvm_string_utf8_length(vm,input.l);
   if(n<0 || a>SIZE_MAX-(size_t)n || b>SIZE_MAX-a-(size_t)n) return 0;
   size_t length=a+(size_t)n+b;char *text=malloc(length ? length : 1);if(!text) return 0;
   memcpy(text,sref(vm,reason.l),a);memcpy(text+a,suffix,(size_t)n);memcpy(text+a+(size_t)n,sref(vm,input.l),b);
   dvm_ref message=dvm_new_string_n(vm,text,length);free(text);return message;
}
static bool nio_invalid_path(struct dvm *vm,dvm_ref input)
{
   dvm__throw(vm,"java/nio/file/InvalidPathException","Nul character not allowed: %s",sref(vm,input));
   dvm_ref error=dvm_exception(vm);
   dvm_set_field(vm,error,"input","Ljava/lang/String;",(union dvm_value){.l=input});
   dvm_set_field(vm,error,"reason","Ljava/lang/String;",(union dvm_value){.l=dvm_new_string(vm,"Nul character not allowed")});
   rt_set_int(vm,error,"index",-1);
   dvm_ref message=nio_invalid_message_ref(vm,error);
   if(message) dvm_set_field(vm,error,"detailMessage","Ljava/lang/String;",(union dvm_value){.l=message});
   return false;
}
static dvm_ref nio_path_from_string(struct dvm *vm,dvm_ref string)
{
   if(!string) {dvm__throw(vm,"java/lang/NullPointerException","path");return 0;}
   const char *text=dvm_string_utf8(vm,string);size_t length=dvm_string_utf8_length(vm,string);
   if(!text) {dvm__throw(vm,"java/lang/ClassCastException","path string");return 0;}
   if(memchr(text,0,length)) {nio_invalid_path(vm,string);return 0;}
   bool normal=!(length>1 && text[length-1]=='/');
   for(size_t i=1;normal && i<length;++i) if(text[i]=='/' && text[i-1]=='/') normal=false;
   /* Normal paths retain the immutable Java String, with no extra byte copy. */
   if(!normal) {
      char *copy=malloc(length+1);if(!copy) {dvm__throw(vm,"java/lang/OutOfMemoryError","path");return 0;}
      size_t used=0;
      for(size_t i=0;i<length;++i) if(text[i]!='/' || !used || copy[used-1]!='/') copy[used++]=text[i];
      if(used>1 && copy[used-1]=='/') --used;
      string=dvm_new_string_n(vm,copy,used);free(copy);if(!string) return 0;
   }
   dvm_pin(vm,string);
   dvm_ref path=dvm_new_object(vm,dvm_find_class(vm,"sun/nio/fs/UnixPath"));
   if(path) dvm_set_field(vm,path,"path","Ljava/lang/String;",(union dvm_value){.l=string});
   dvm_unpin(vm,string);return path;
}
static dvm_ref path_make(struct dvm *vm,const char *text)
{return nio_path_from_string(vm,dvm_new_string(vm,text ? text : ""));}
static bool nio_paths_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   if(!ARG(0).l || !ARG(1).l) {dvm__throw(vm,"java/lang/NullPointerException","path components");return false;}
   struct dvm_object *more=dvm__obj(vm,ARG(1).l);
   if(!more || more->kind!=DVM_OBJ_ARRAY || more->elem_kind!='L') {dvm__throw(vm,"java/lang/ClassCastException","path components");return false;}
   if(!more->length) {out->l=nio_path_from_string(vm,ARG(0).l);return out->l!=0;}
   size_t length=dvm_string_utf8_length(vm,ARG(0).l);
   for(uint32_t i=0;i<more->length;++i) {
      dvm_ref part=((dvm_ref *)more->data)[i];
      if(!part) {dvm__throw(vm,"java/lang/NullPointerException","path component");return false;}
      size_t n=dvm_string_utf8_length(vm,part);
      if(n && (n>=UINT32_MAX || length>UINT32_MAX-n-1)) {dvm__throw(vm,"java/lang/OutOfMemoryError","path length");return false;}
      if(n) length+=n+(length!=0);
   }
   char *joined=malloc(length+1);if(!joined) {dvm__throw(vm,"java/lang/OutOfMemoryError","path");return false;}
   size_t used=dvm_string_utf8_length(vm,ARG(0).l);memcpy(joined,sref(vm,ARG(0).l),used);
   for(uint32_t i=0;i<more->length;++i) {
      dvm_ref part=((dvm_ref *)more->data)[i];size_t n=dvm_string_utf8_length(vm,part);
      if(n) {if(used) joined[used++]='/';memcpy(joined+used,sref(vm,part),n);used+=n;}
   }
   dvm_ref string=dvm_new_string_n(vm,joined,used);free(joined);
   out->l=string ? nio_path_from_string(vm,string) : 0;return out->l!=0;
}
static dvm_ref nio_path_text(struct dvm *vm,dvm_ref self)
{
   if(!self) {dvm__throw(vm,"java/lang/NullPointerException","path");return 0;}
   if(!object_is_a(vm,self,"Lsun/nio/fs/UnixPath;")) {dvm__throw(vm,"java/nio/file/ProviderMismatchException",NULL);return 0;}
   union dvm_value value={0};dvm_get_field(vm,self,"path","Ljava/lang/String;",&value);return value.l;
}
static bool nio_path_string(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;out->l=nio_path_text(vm,self);return out->l!=0;}
static bool nio_path_to_file(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;dvm_ref text=nio_path_text(vm,self);if(!text) return false;
   dvm_ref file=dvm_new_object(vm,dvm_find_class(vm,"java/io/File"));
   if(file) dvm_set_field(vm,file,"path","Ljava/lang/String;",(union dvm_value){.l=text});
   RETL(file);
}
static bool nio_path_absolute(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;dvm_ref string=nio_path_text(vm,self);if(!string) return false;
   const char *text=sref(vm,string);bool absolute=*text=='/';
   if(!strcmp(builtin_name(),"isAbsolute")) RETI(absolute);
   if(absolute) RETL(self);
   size_t n=strlen(text);char *joined=malloc(n+2);if(!joined) {dvm__throw(vm,"java/lang/OutOfMemoryError","absolute path");return false;}
   joined[0]='/';memcpy(joined+1,text,n+1);dvm_ref result=path_make(vm,joined);free(joined);RETL(result);
}
static bool nio_path_equals(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if(!ARG(0).l || !object_is_a(vm,ARG(0).l,"Lsun/nio/fs/UnixPath;")) RETI(0);
   dvm_ref a=nio_path_text(vm,self),b=nio_path_text(vm,ARG(0).l);
   RETI(a && b && !strcmp(sref(vm,a),sref(vm,b)));
}
static bool nio_path_compare(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;dvm_ref left=nio_path_text(vm,self);if(!left) return false;
   if(ARG(0).l && !object_is_a(vm,ARG(0).l,"Lsun/nio/fs/UnixPath;")) {dvm__throw(vm,"java/lang/ClassCastException","UnixPath");return false;}
   dvm_ref right=nio_path_text(vm,ARG(0).l);if(!right) return false;
   const unsigned char *a=(const unsigned char *)sref(vm,left),*b=(const unsigned char *)sref(vm,right);
   size_t x=strlen((const char *)a),y=strlen((const char *)b),n=x<y ? x : y;
   for(size_t i=0;i<n;++i) if(a[i]!=b[i]) RETI((int32_t)a[i]-(int32_t)b[i]);
   RETI((int32_t)((int64_t)x-(int64_t)y));
}
static bool nio_path_hash(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;dvm_ref string=nio_path_text(vm,self);if(!string) return false;
   const unsigned char *p=(const unsigned char *)sref(vm,string);uint32_t hash=0;
   while(*p) hash=hash*31+*p++;
   RETI((int32_t)hash);
}
static bool nio_invalid_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!ARG(0).l || !ARG(1).l) {dvm__throw(vm,"java/lang/NullPointerException","InvalidPathException");return false;}
   int32_t index=nargs>2 ? ARG(2).i : -1;
   if(index< -1) {dvm__throw(vm,"java/lang/IllegalArgumentException","index < -1");return false;}
   dvm_set_field(vm,self,"input","Ljava/lang/String;",ARG(0));dvm_set_field(vm,self,"reason","Ljava/lang/String;",ARG(1));rt_set_int(vm,self,"index",index);
   union dvm_value message={.l=nio_invalid_message_ref(vm,self)};
   if(!message.l) {dvm__throw(vm,"java/lang/OutOfMemoryError","path exception");return false;}
   return th_init(vm,self,&message,1,out);
}
static bool nio_exception_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value file=ARG(0),other={0},reason={0};if(nargs>1) {other=ARG(1);reason=ARG(2);}
   dvm_set_field(vm,self,"file","Ljava/lang/String;",file);dvm_set_field(vm,self,"other","Ljava/lang/String;",other);dvm_set_field(vm,self,"reason","Ljava/lang/String;",reason);
   const char *a=file.l ? sref(vm,file.l) : "",*b=other.l ? sref(vm,other.l) : "",*r=reason.l ? sref(vm,reason.l) : "";
   if(!file.l && !other.l) return th_init(vm,self,&reason,1,out);
   size_t length=strlen(a)+strlen(b)+strlen(r)+7;char *text=malloc(length);
   if(!text) {dvm__throw(vm,"java/lang/OutOfMemoryError","file exception");return false;}
   snprintf(text,length,"%s%s%s%s%s",a,other.l ? " -> " : "",b,reason.l ? ": " : "",r);
   union dvm_value message={.l=dvm_new_string(vm,text)};free(text);return th_init(vm,self,&message,1,out);
}
static bool nio_exception_field(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"getIndex")) RETI(rt_get_int(vm,self,"index"));
   const char *field=!strcmp(name,"getInput") ? "input" : !strcmp(name,"getFile") ? "file" : !strcmp(name,"getOtherFile") ? "other" : "reason";
   return dvm_get_field(vm,self,field,"Ljava/lang/String;",out);
}
static bool nio_io_error(struct dvm *vm,dvm_ref path,int error)
{
   dvm_ref text=nio_path_text(vm,path);if(!text) return false;
   const char *type=error==ENOENT ? "java/nio/file/NoSuchFileException" : error==EACCES ? "java/nio/file/AccessDeniedException" : "java/nio/file/FileSystemException";
   dvm__throw(vm,type,"%s: %s",sref(vm,text),strerror(error));
   dvm_ref exception=dvm_exception(vm);
   dvm_set_field(vm,exception,"file","Ljava/lang/String;",(union dvm_value){.l=text});
   dvm_set_field(vm,exception,"reason","Ljava/lang/String;",(union dvm_value){.l=dvm_new_string(vm,strerror(error))});return false;
}
static bool nio_link_flags(struct dvm *vm,dvm_ref options,int *flags)
{
   *flags=0;if(!options) {dvm__throw(vm,"java/lang/NullPointerException","link options");return false;}
   struct dvm_object *array=dvm__obj(vm,options);
   if(!array || array->kind!=DVM_OBJ_ARRAY || array->elem_kind!='L') {dvm__throw(vm,"java/lang/ClassCastException","link options");return false;}
   union dvm_value nofollow={0};
   if(!dvm_get_static(vm,dvm_find_class(vm,"java/nio/file/LinkOption"),"NOFOLLOW_LINKS","Ljava/nio/file/LinkOption;",&nofollow)) return false;
   for(uint32_t i=0;i<array->length;++i) {
      dvm_ref option=((dvm_ref *)array->data)[i];
      if(!option) {dvm__throw(vm,"java/lang/NullPointerException","link option");return false;}
      if(option!=nofollow.l) {dvm__throw(vm,"java/lang/UnsupportedOperationException","link option");return false;}
      *flags=LUNA_AT_NOFOLLOW;
   }
   return true;
}
static bool nio_stat(struct dvm *vm,dvm_ref path,int flags,luna_file_info *info,int *error)
{
   dvm_ref string=nio_path_text(vm,path);if(!string) return false;
   const char *text=sref(vm,string);char mapped[DVM_PATH_MAX];
   /* Empty UnixPath denotes the Android process working directory, '/'. */
   if(strlen(text)>=sizeof mapped) {*error=ENAMETOOLONG;return true;}
   const char *host=arm_exec_map_guest_path(*text ? text : "/",mapped,sizeof mapped);
   if(!host) {*error=errno ? errno : EINVAL;return true;}
   *error=luna_file_infoat(LUNA_AT_FDCWD,host,info,flags) ? errno : 0;return true;
}
static dvm_ref nio_time_make(struct dvm *vm,luna_file_time value)
{
   dvm_ref time=dvm_new_object(vm,dvm_find_class(vm,"java/nio/file/attribute/FileTime"));
   if(time) {
      dvm_set_field(vm,time,"seconds","J",(union dvm_value){.j=value.seconds});
      rt_set_int(vm,time,"nanos",(int32_t)value.nanoseconds);
   }
   return time;
}
static bool nio_time_from_millis(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;int64_t seconds=ARG(0).j/1000,nanos=(ARG(0).j%1000)*1000000;
   if(nanos<0) {--seconds;nanos+=1000000000;}RETL(nio_time_make(vm,(luna_file_time){seconds,nanos}));
}
static bool nio_time_millis(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value seconds={0};dvm_get_field(vm,self,"seconds","J",&seconds);
   int64_t millis=rt_get_int(vm,self,"nanos")/1000000;
   if(seconds.j>=0) {
      if(seconds.j>INT64_MAX/1000) RETJ(INT64_MAX);
      int64_t base=seconds.j*1000;if(millis>INT64_MAX-base) RETJ(INT64_MAX);RETJ(base+millis);
   }
   if(seconds.j+1<INT64_MIN/1000) RETJ(INT64_MIN);
   int64_t base=(seconds.j+1)*1000,delta=millis-1000;
   if(base<INT64_MIN-delta) RETJ(INT64_MIN);
   RETJ(base+delta);
}
static bool nio_time_compare(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;bool equal=!strcmp(builtin_name(),"equals");
   if(!ARG(0).l) {if(equal) RETI(0);dvm__throw(vm,"java/lang/NullPointerException","FileTime");return false;}
   if(!object_is_a(vm,ARG(0).l,"Ljava/nio/file/attribute/FileTime;")) {
      if(equal) RETI(0);
      dvm__throw(vm,"java/lang/ClassCastException","FileTime");return false;
   }
   union dvm_value a={0},b={0};dvm_get_field(vm,self,"seconds","J",&a);dvm_get_field(vm,ARG(0).l,"seconds","J",&b);
   int32_t x=rt_get_int(vm,self,"nanos"),y=rt_get_int(vm,ARG(0).l,"nanos");
   int order=a.j==b.j ? (x>y)-(x<y) : (a.j>b.j)-(a.j<b.j);RETI(equal ? order==0 : order);
}
static bool nio_time_hash(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value seconds={0};dvm_get_field(vm,self,"seconds","J",&seconds);
   uint64_t value=(uint64_t)seconds.j;RETI((int32_t)((uint32_t)(value^(value>>32))+51u*(uint32_t)rt_get_int(vm,self,"nanos")));
}
static bool nio_attrs_value(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;const char *name=builtin_name();struct dvm_object *obj=dvm__obj(vm,self);
   const luna_file_info *info=obj ? obj->data : NULL;
   if(!info) {dvm__throw(vm,"java/lang/IllegalStateException","file attributes");return false;}
   if(!strcmp(name,"size")) RETJ(info->size);
   unsigned type=info->mode&0170000;
   if(!strcmp(name,"isRegularFile")) RETI(type==0100000);
   if(!strcmp(name,"isDirectory")) RETI(type==0040000);
   if(!strcmp(name,"isSymbolicLink")) RETI(type==0120000);
   if(!strcmp(name,"isOther")) RETI(type!=0100000 && type!=0040000 && type!=0120000);
   union dvm_value cached={0};
   if(!strcmp(name,"fileKey")) {
      dvm_get_field(vm,self,"key","Ljava/lang/Object;",&cached);
      if(!cached.l) {
         dvm_ref key=dvm_new_object(vm,dvm_find_class(vm,"sun/nio/fs/UnixFileKey"));if(!key) return false;
         dvm_set_field(vm,key,"device","J",(union dvm_value){.j=(int64_t)info->device});
         dvm_set_field(vm,key,"inode","J",(union dvm_value){.j=(int64_t)info->inode});cached.l=key;
         dvm_set_field(vm,self,"key","Ljava/lang/Object;",cached);
      }
      RETL(cached.l);
   }
   /* Linux's UnixFileAttributes uses mtime when birth time is unavailable. */
   bool access=!strcmp(name,"lastAccessTime");const char *field=access ? "accessed" : "modified";
   dvm_get_field(vm,self,field,"Ljava/nio/file/attribute/FileTime;",&cached);
   if(!cached.l) {
      cached.l=nio_time_make(vm,access ? info->accessed : info->modified);if(!cached.l) return false;
      dvm_set_field(vm,self,field,"Ljava/nio/file/attribute/FileTime;",cached);
   }
   RETL(cached.l);
}
static bool nio_key_value(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value dev={0},ino={0};dvm_get_field(vm,self,"device","J",&dev);dvm_get_field(vm,self,"inode","J",&ino);
   uint64_t a=(uint64_t)dev.j,b=(uint64_t)ino.j;const char *name=builtin_name();
   if(!strcmp(name,"hashCode")) RETI((int32_t)((uint32_t)(a^(a>>32))+(uint32_t)(b^(b>>32))));
   if(!strcmp(name,"toString")) {char text[96];snprintf(text,sizeof text,"(dev=%llx,ino=%llu)",(unsigned long long)a,(unsigned long long)b);RETL(dvm_new_string(vm,text));}
   if(!ARG(0).l || !object_is_a(vm,ARG(0).l,"Lsun/nio/fs/UnixFileKey;")) RETI(0);
   union dvm_value otherdev={0},otherino={0};dvm_get_field(vm,ARG(0).l,"device","J",&otherdev);dvm_get_field(vm,ARG(0).l,"inode","J",&otherino);
   RETI(dev.j==otherdev.j && ino.j==otherino.j);
}
static bool nio_read_attributes(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   if(!ARG(1).l) {dvm__throw(vm,"java/lang/NullPointerException","attribute type");return false;}
   struct dvm_object *type=dvm__obj(vm,ARG(1).l);
   if(!type || type->kind!=DVM_OBJ_CLASS || type->klass!=dvm_find_class(vm,"java/nio/file/attribute/BasicFileAttributes")) {
      dvm__throw(vm,"java/lang/UnsupportedOperationException","file attribute type");return false;
   }
   int flags=0,error=0;luna_file_info info;
   if(!nio_link_flags(vm,ARG(2).l,&flags) || !nio_stat(vm,ARG(0).l,flags,&info,&error)) return false;
   if(error) return nio_io_error(vm,ARG(0).l,error);
   dvm_ref attributes=dvm_new_object(vm,dvm_find_class(vm,"sun/nio/fs/UnixFileAttributes$UnixAsBasicFileAttributes"));
   if(!attributes) return false;
   luna_file_info *copy=malloc(sizeof *copy);if(!copy) {dvm__throw(vm,"java/lang/OutOfMemoryError","file attributes");return false;}
   *copy=info;dvm__obj(vm,attributes)->data=copy;RETL(attributes);
}
static bool nio_files_test(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;int flags=0,error=0;luna_file_info info;
   if(!nio_link_flags(vm,ARG(1).l,&flags) || !nio_stat(vm,ARG(0).l,flags,&info,&error)) return false;
   if(error) RETI(0);
   const char *name=builtin_name();RETI(!strcmp(name,"exists") || (info.mode&0170000)==(!strcmp(name,"isDirectory") ? 0040000 : 0100000));
}
static const struct rt_field rt_nio_path_fields[]={{"path","Ljava/lang/String;"},F_END};
static const char *const rt_nio_path_ifaces[]={"Ljava/nio/file/Path;",NULL};
static const char *const rt_nio_path_superifaces[]={"Ljava/lang/Comparable;","Ljava/lang/Iterable;","Ljava/nio/file/Watchable;",NULL};
static const struct rt_method rt_nio_path[]={
   M("toString","()Ljava/lang/String;",nio_path_string),M("toFile","()Ljava/io/File;",nio_path_to_file),
   M("equals","(Ljava/lang/Object;)Z",nio_path_equals),M("hashCode","()I",nio_path_hash),
   M("compareTo","(Ljava/nio/file/Path;)I",nio_path_compare),M("compareTo","(Ljava/lang/Object;)I",nio_path_compare),
   M("isAbsolute","()Z",nio_path_absolute),M("toAbsolutePath","()Ljava/nio/file/Path;",nio_path_absolute),M_END};
static const struct rt_method rt_nio_paths[]={SM("get","(Ljava/lang/String;[Ljava/lang/String;)Ljava/nio/file/Path;",nio_paths_get),M_END};
static const struct rt_field rt_nio_invalid_fields[]={{"input","Ljava/lang/String;"},{"reason","Ljava/lang/String;"},{"index","I"},F_END};
static const struct rt_method rt_nio_invalid[]={M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",nio_invalid_init),M("<init>","(Ljava/lang/String;Ljava/lang/String;I)V",nio_invalid_init),M("getInput","()Ljava/lang/String;",nio_exception_field),M("getReason","()Ljava/lang/String;",nio_exception_field),M("getIndex","()I",nio_exception_field),M_END};
static const struct rt_field rt_nio_exception_fields[]={{"file","Ljava/lang/String;"},{"other","Ljava/lang/String;"},{"reason","Ljava/lang/String;"},F_END};
static const struct rt_method rt_nio_exception[]={M("<init>","(Ljava/lang/String;)V",nio_exception_init),M("<init>","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",nio_exception_init),M("getFile","()Ljava/lang/String;",nio_exception_field),M("getOtherFile","()Ljava/lang/String;",nio_exception_field),M("getReason","()Ljava/lang/String;",nio_exception_field),M_END};
static const struct rt_field rt_nio_time_fields[]={{"seconds","J"},{"nanos","I"},F_END};
static const char *const rt_nio_time_ifaces[]={"Ljava/lang/Comparable;",NULL};
static const struct rt_method rt_nio_time[]={
   SM("fromMillis","(J)Ljava/nio/file/attribute/FileTime;",nio_time_from_millis),M("toMillis","()J",nio_time_millis),
   M("compareTo","(Ljava/nio/file/attribute/FileTime;)I",nio_time_compare),M("compareTo","(Ljava/lang/Object;)I",nio_time_compare),
   M("equals","(Ljava/lang/Object;)Z",nio_time_compare),M("hashCode","()I",nio_time_hash),M_END};
static const struct rt_field rt_nio_attrs_fields[]={
   {"modified","Ljava/nio/file/attribute/FileTime;"},{"accessed","Ljava/nio/file/attribute/FileTime;"},{"key","Ljava/lang/Object;"},F_END};
static const char *const rt_nio_attrs_ifaces[]={"Ljava/nio/file/attribute/BasicFileAttributes;",NULL};
#define NIO_ATTR_METHODS(entry,fn) \
   entry("size","()J",fn),entry("isRegularFile","()Z",fn),entry("isDirectory","()Z",fn),entry("isSymbolicLink","()Z",fn),entry("isOther","()Z",fn), \
   entry("lastModifiedTime","()Ljava/nio/file/attribute/FileTime;",fn),entry("lastAccessTime","()Ljava/nio/file/attribute/FileTime;",fn), \
   entry("creationTime","()Ljava/nio/file/attribute/FileTime;",fn),entry("fileKey","()Ljava/lang/Object;",fn)
static const struct rt_method rt_nio_attrs[]={NIO_ATTR_METHODS(M,nio_attrs_value),M_END};
/* Interface dispatch reaches the concrete Unix snapshot's implementation. */
#define NIO_ATTR_ABSTRACT(n,s,f) {n,s,NULL,DEX_ACC_ABSTRACT}
static const struct rt_method rt_nio_attrs_interface[]={NIO_ATTR_METHODS(NIO_ATTR_ABSTRACT,NULL),M_END};
static const struct rt_method rt_nio_path_interface[]={
   NIO_ATTR_ABSTRACT("toString","()Ljava/lang/String;",NULL),NIO_ATTR_ABSTRACT("toFile","()Ljava/io/File;",NULL),
   NIO_ATTR_ABSTRACT("isAbsolute","()Z",NULL),NIO_ATTR_ABSTRACT("toAbsolutePath","()Ljava/nio/file/Path;",NULL),
   NIO_ATTR_ABSTRACT("compareTo","(Ljava/nio/file/Path;)I",NULL),M_END};
#undef NIO_ATTR_ABSTRACT
#undef NIO_ATTR_METHODS
static const struct rt_field rt_nio_key_fields[]={{"device","J"},{"inode","J"},F_END};
static const struct rt_method rt_nio_key[]={M("equals","(Ljava/lang/Object;)Z",nio_key_value),M("hashCode","()I",nio_key_value),M("toString","()Ljava/lang/String;",nio_key_value),M_END};
