/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0
 * PrintStream owns the character encoder; the OutputStream retains byte I/O
 * and close ownership. Only IOExceptions are converted into sticky errors.
 */
struct ps_state {
  struct jcs charset;
  struct jcs_encoder *encoder;
  dvm_ref bytes;
  uint16_t pending;
  bool has_pending,closing;
};
static void ps_destroy(void *data)
{
  struct ps_state *s=data;
  if(s) jcs_encoder_free(s->encoder);
  free(s);
}
static bool ps_io_error(struct dvm *vm,dvm_ref self,bool interruptible)
{
  struct dvm_class *error=dvm_object_class(vm,dvm_exception(vm));
  if(!error) return false;
  bool interrupted=interruptible && dvm__class_assignable(vm,error,dvm_find_class(vm,"java/io/InterruptedIOException"));
  if(!interrupted && !dvm__class_assignable(vm,error,dvm_find_class(vm,"java/io/IOException"))) return false;
  dvm_clear_exception(vm);
  if(interrupted) {
    union dvm_value ignored={0};
    return t_interrupt(vm,dvm__current_thread(vm),NULL,0,&ignored);
  }
  rt_set_int(vm,self,"trouble",1);
  return true;
}
static dvm_ref ps_target(struct dvm *vm,dvm_ref self)
{
  union dvm_value value={0};
  (void)(dvm_get_field)(vm,self,"out","Ljava/io/OutputStream;",&value);
  if(!value.l) dvm__throw(vm,"java/io/IOException","Stream closed");
  return value.l;
}
static bool ps_target_flush(struct dvm *vm,dvm_ref self)
{
  dvm_ref target=ps_target(vm,self);
  union dvm_value ignored={0};
  return target && am_vcall(vm,target,"flush","()V",NULL,0,&ignored);
}
static bool ps_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  const char *sig=builtin_sig();
  bool file=strncmp(sig,"(Ljava/io/OutputStream;",22)!=0;
  int encoding=file ? (nargs>1 ? 1 : -1) : (nargs>2 ? 2 : -1);
  if(!file && !ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","Null output stream"); return false; }
  struct jcs charset; jcs_default(&charset);
  if(encoding>=0) {
    if(!ARG(encoding).l) { dvm__throw(vm,"java/lang/NullPointerException","charsetName"); return false; }
    const char *name=dvm_string_utf8(vm,ARG(encoding).l);
    if(!name || !jcs_lookup(name,&charset)) { dvm__throw(vm,"java/io/UnsupportedEncodingException","%s",name ? name : ""); return false; }
  }
  dvm_ref target=ARG(0).l;
  union dvm_value ignored={0};
  if(file) {
    target=dvm_new_object(vm,dvm_find_class(vm,"java/io/FileOutputStream"));
    if(!target) { dvm__throw(vm,"java/lang/OutOfMemoryError","PrintStream file"); return false; }
    const char *ctor=!strncmp(sig,"(Ljava/lang/String;",19) ? "(Ljava/lang/String;)V" : "(Ljava/io/File;)V";
    if(!am_vcall(vm,target,"<init>",ctor,args,1,&ignored)) return false;
  }
  struct ps_state *s=calloc(1,sizeof *s);
  if(!s) { dvm__throw(vm,"java/lang/OutOfMemoryError","PrintStream"); return false; }
  s->charset=charset;
  struct dvm_object *object=dvm__obj(vm,self);
  if(object->data_destroy) object->data_destroy(object->data); else free(object->data);
  object->data=s; object->data_destroy=ps_destroy;
  (void)(dvm_set_field)(vm,self,"out","Ljava/io/OutputStream;",(union dvm_value){.l=target});
  rt_set_int(vm,self,"autoFlush",!file && nargs>1 && ARG(1).i);
  rt_set_int(vm,self,"trouble",0);
  RETV();
}
static bool ps_write(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(!strcmp(builtin_sig(),"([B)V")) {
    if(!ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","buffer"); return false; }
    union dvm_value range[3]={ARG(0),{.i=0},{.i=(int32_t)dvm_array_length(vm,ARG(0).l)}};
    return am_vcall(vm,self,"write","([BII)V",range,3,out);
  }
  if(!dvm__monitor_enter(vm,self)) return false;
  dvm_ref target=ps_target(vm,self);
  union dvm_value ignored={0};
  bool good=target && am_vcall(vm,target,"write",nargs==1 ? "(I)V" : "([BII)V",args,nargs,&ignored);
  if(good && rt_get_int(vm,self,"autoFlush") && (nargs==3 || ARG(0).i=='\n'))
    good=am_vcall(vm,target,"flush","()V",NULL,0,&ignored);
  if(!good) good=ps_io_error(vm,self,true);
  if(!dvm__monitor_exit(vm,self)) good=false;
  return good;
}
static bool ps_send(struct dvm *vm,dvm_ref self,struct ps_state *s,size_t count)
{
  if(!count) return true;
  union dvm_value args[3]={{.l=s->bytes},{.i=0},{.i=(int32_t)count}},ignored={0};
  return am_vcall(vm,self,"write","([BII)V",args,3,&ignored);
}
static bool ps_encode_segment(struct dvm *vm,dvm_ref self,struct ps_state *s,const uint16_t *units,size_t count,bool end)
{
  size_t used=0;
  while(used<count) {
    uint8_t *bytes=dvm_array_data(vm,s->bytes);
    struct jcs_coder_progress step=jcs_encoder_encode(s->encoder,units+used,count-used,bytes,4096,end);
    used+=step.consumed;
    if(!ps_send(vm,self,s,step.produced)) return false;
    if(step.result==JCS_CODER_FAILURE) { dvm__throw(vm,"java/nio/charset/CoderMalfunctionError","character output"); return false; }
    if(step.result==JCS_UNDERFLOW) {
      if(used<count) { s->pending=units[used]; s->has_pending=true; }
      break;
    }
    if(step.result==JCS_OVERFLOW) continue;
    uint16_t replacement=(s->charset.kind==JCS_UTF16 || s->charset.kind==JCS_UTF16BE || s->charset.kind==JCS_UTF16LE) ? 0xfffd : '?';
    size_t length=step.error_length;
    step=jcs_encoder_encode(s->encoder,&replacement,1,dvm_array_data(vm,s->bytes),4096,true);
    if(step.result!=JCS_UNDERFLOW) { dvm__throw(vm,"java/nio/charset/CoderMalfunctionError","replacement output"); return false; }
    if(!ps_send(vm,self,s,step.produced)) return false;
    used+=length;
  }
  return true;
}
static bool ps_emit(struct dvm *vm,dvm_ref self,const uint16_t *units,size_t count,bool end)
{
  if(!ps_target(vm,self)) return false;
  struct ps_state *s=dvm__obj(vm,self)->data;
  if(!s) { dvm__throw(vm,"java/io/IOException","Uninitialized PrintStream"); return false; }
  if(!s->encoder || !s->bytes) {
    if(!s->encoder) s->encoder=jcs_encoder_new(&s->charset);
    if(!s->bytes) s->bytes=dvm_new_array(vm,'B',"B",4096);
    (void)(dvm_set_field)(vm,self,"encodedBytes","[B",(union dvm_value){.l=s->bytes});
    if(!s->encoder || !s->bytes) { dvm__throw(vm,"java/lang/OutOfMemoryError","PrintStream encoder"); return false; }
  }
  if(s->has_pending) {
    if(!count && !end) return true;
    uint16_t pair[2]={s->pending,0};
    size_t n=1;
    if(count && units[0]>=0xdc00 && units[0]<=0xdfff) { pair[n++]=*units++; --count; }
    s->has_pending=false;
    if(!ps_encode_segment(vm,self,s,pair,n,n==1)) return false;
  }
  if(!ps_encode_segment(vm,self,s,units,count,end)) return false;
  if(end) {
    struct jcs_coder_progress step;
    do {
      step=jcs_encoder_flush(s->encoder,dvm_array_data(vm,s->bytes),4096);
      if(!ps_send(vm,self,s,step.produced)) return false;
    } while(step.result==JCS_OVERFLOW);
    if(step.result!=JCS_UNDERFLOW) { dvm__throw(vm,"java/nio/charset/CoderMalfunctionError","encoder flush"); return false; }
  }
  return true;
}
static bool ps_value(struct dvm *vm,const union dvm_value *args,const char *sig,union dvm_value *text)
{
  char value[96];
  switch(sig[1]) {
  case 'Z': text->l=dvm_new_string(vm,args[0].i ? "true" : "false"); return text->l!=0;
  case 'C': return s_valueOf_char(vm,0,args,1,text);
  case 'I': snprintf(value,sizeof value,"%d",args[0].i); break;
  case 'J': snprintf(value,sizeof value,"%lld",(long long)args[0].j); break;
  case 'F': box_fp_str(value,sizeof value,(double)args[0].f,true); break;
  case 'D': box_fp_str(value,sizeof value,args[0].d,false); break;
  case 'L':
    if(!strcmp(sig,"(Ljava/lang/String;)V")) { text->l=args[0].l ? args[0].l : dvm_new_string(vm,"null"); return text->l!=0; }
    return s_valueOf_obj(vm,0,args,1,text);
  default: return false;
  }
  text->l=dvm_new_string(vm,value);
  return text->l!=0;
}
static bool ps_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  const char *sig=builtin_sig();
  bool newline=!strcmp(builtin_name(),"println");
  union dvm_value text={0},ignored={0};
  /* Object conversion is outside the println monitor in Android. */
  if(newline && nargs && !strcmp(sig,"(Ljava/lang/Object;)V")) {
    if(!s_valueOf_obj(vm,0,args,1,&text)) return false;
    args=&text; sig="(Ljava/lang/String;)V";
  }
  if(!newline && sig[1]!='[' && !ps_value(vm,args,sig,&text)) {
    if(!dvm_exception(vm)) dvm__throw(vm,"java/lang/OutOfMemoryError","PrintStream text");
    return false;
  }
  if(!dvm__monitor_enter(vm,self)) return false;
  bool good=true;
  if(newline) {
    if(nargs) good=am_vcall(vm,self,"print",sig,args,1,&ignored);
    if(good) { const uint16_t lf='\n'; good=ps_emit(vm,self,&lf,1,false); }
    if(good && rt_get_int(vm,self,"autoFlush")) good=ps_target_flush(vm,self);
  } else {
    uint16_t *owned=NULL;
    const uint16_t *units=NULL;
    size_t count=0;
    good=ps_target(vm,self)!=0;
    if(good && sig[1]=='[') {
      if(!ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","chars"); good=false; }
      else { count=dvm_array_length(vm,ARG(0).l); units=dvm_array_data(vm,ARG(0).l); }
    } else if(good && !text.l) { dvm__throw(vm,"java/lang/NullPointerException","text"); good=false; }
    else if(good) {
      struct dvm_object *string=dvm__obj(vm,text.l);
      count=jcs_wtf8_to_utf16(string->utf8,string->utf8_len,NULL);
      if(count>SIZE_MAX/sizeof(uint16_t)) { dvm__throw(vm,"java/lang/OutOfMemoryError","PrintStream text"); good=false; }
      else {
        owned=malloc((count ? count : 1)*sizeof(uint16_t));
        if(!owned) { dvm__throw(vm,"java/lang/OutOfMemoryError","PrintStream text"); good=false; }
        else { jcs_wtf8_to_utf16(string->utf8,string->utf8_len,owned); units=owned; }
      }
    }
    if(good) good=ps_emit(vm,self,units,count,false);
    if(good && rt_get_int(vm,self,"autoFlush")) {
      for(size_t i=0; good && i<count; ++i) if(units[i]=='\n') {
        good=ps_target_flush(vm,self);
        if(sig[1]!='[') break; /* String write flushes once; char[] per LF. */
      }
    }
    free(owned);
  }
  if(!good) good=ps_io_error(vm,self,true);
  if(!dvm__monitor_exit(vm,self)) good=false;
  (void)out;
  return good;
}
static bool ps_control(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  const char *name=builtin_name();
  if(!strcmp(name,"setError") || !strcmp(name,"clearError")) {
    rt_set_int(vm,self,"trouble",!strcmp(name,"setError")); RETV();
  }
  if(!strcmp(name,"checkError")) {
    union dvm_value target={0},ignored={0};
    (void)(dvm_get_field)(vm,self,"out","Ljava/io/OutputStream;",&target);
    if(target.l && !am_vcall(vm,self,"flush","()V",NULL,0,&ignored)) return false;
    (void)(dvm_get_field)(vm,self,"out","Ljava/io/OutputStream;",&target);
    if(target.l && dvm__class_assignable(vm,dvm_object_class(vm,target.l),dvm_find_class(vm,"java/io/PrintStream")))
      return am_vcall(vm,target.l,"checkError","()Z",NULL,0,out);
    RETI(rt_get_int(vm,self,"trouble"));
  }
  if(!dvm__monitor_enter(vm,self)) return false;
  bool good=true;
  if(!strcmp(name,"flush")) {
    good=ps_target_flush(vm,self);
    if(!good) good=ps_io_error(vm,self,false);
  } else {
    struct ps_state *s=dvm__obj(vm,self)->data;
    if(!s || !s->closing) {
      if(s) s->closing=true;
      if(s && s->encoder) good=ps_emit(vm,self,NULL,0,true);
      if(good) {
        dvm_ref target=ps_target(vm,self);
        union dvm_value ignored={0};
        good=target && am_vcall(vm,target,"close","()V",NULL,0,&ignored);
      }
      if(!good) good=ps_io_error(vm,self,false);
      if(good) (void)(dvm_set_field)(vm,self,"out","Ljava/io/OutputStream;",(union dvm_value){0});
    }
  }
  if(!dvm__monitor_exit(vm,self)) good=false;
  return good;
}
static bool ps_append(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  union dvm_value value=ARG(0),ignored={0};
  if(builtin_sig()[1]=='C') {
    if(!am_vcall(vm,self,"print","(C)V",args,1,&ignored)) return false;
  } else {
    if(!value.l) value.l=dvm_new_string(vm,"null");
    if(nargs==3) {
      union dvm_value bounds[2]={ARG(1),ARG(2)};
      if(!am_vcall(vm,value.l,"subSequence","(II)Ljava/lang/CharSequence;",bounds,2,&value)) return false;
    }
    union dvm_value text={0};
    if(!am_vcall(vm,value.l,"toString","()Ljava/lang/String;",NULL,0,&text) ||
       !am_vcall(vm,self,"print","(Ljava/lang/String;)V",&text,1,&ignored)) return false;
  }
  RETL(self);
}
static bool ps_format(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  union dvm_value text={0},ignored={0};
  const char *signature=nargs==2 ? "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;" :
    "(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;";
  struct dvm_method *format=dvm_lookup(vm,"java/lang/String","format",signature);
  if(!format || !dvm_call(vm,format,0,args,nargs,&text) ||
     !am_vcall(vm,self,"print","(Ljava/lang/String;)V",&text,1,&ignored)) return false;
  RETL(self);
}
/* Host stdout/stderr are real OutputStreams for System's initial PrintStreams.
 * Closing a console sink retires that Java object, not the process-owned FILE. */
static bool console_write(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(rt_get_int(vm,self,"closed")) { dvm__throw(vm,"java/io/IOException","Console closed"); return false; }
  FILE *file=rt_get_int(vm,self,"stderr") ? stderr : stdout;
  if(!strcmp(builtin_name(),"close")) { rt_set_int(vm,self,"closed",1); RETV(); }
  if(!strcmp(builtin_name(),"flush")) {
    if(fflush(file)) { dvm__throw(vm,"java/io/IOException","Console flush failed"); return false; }
    RETV();
  }
  uint8_t one=(uint8_t)ARG(0).i;
  const uint8_t *bytes=&one;
  size_t length=1;
  if(builtin_sig()[1]=='[') {
    if(!ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","buffer"); return false; }
    uint32_t size=dvm_array_length(vm,ARG(0).l);
    int32_t offset=nargs==3 ? ARG(1).i : 0,n=nargs==3 ? ARG(2).i : (int32_t)size;
    if(offset<0 || n<0 || (uint32_t)offset>size || (uint32_t)n>size-(uint32_t)offset) { dvm__throw(vm,"java/lang/IndexOutOfBoundsException",NULL); return false; }
    bytes=dvm_array_data(vm,ARG(0).l); if(n) bytes+=offset; length=(size_t)n;
  }
  if(length && fwrite(bytes,1,length,file)!=length) { dvm__throw(vm,"java/io/IOException","Console write failed"); return false; }
  RETV();
}
static bool ps_init_console(struct dvm *vm,dvm_ref self,bool is_error)
{
  dvm_ref console=dvm_new_object(vm,dvm_find_class(vm,"lunaria/ConsoleOutputStream"));
  rt_set_int(vm,console,"stderr",is_error);
  /* Initial system streams use the same constructor implementation, not a
   * null-target special case in print/write. */
  struct dvm_method *init=dvm_lookup(vm,"java/io/PrintStream","<init>","(Ljava/io/OutputStream;Z)V");
  union dvm_value args[2]={{.l=console},{.i=1}},ignored={0};
  return console && init && dvm_call(vm,init,self,args,2,&ignored);
}
static const struct rt_field rt_printstream_fields[]={ {"autoFlush","Z"},{"trouble","Z"},{"encodedBytes","[B"},F_END };
static const struct rt_field rt_console_fields[]={ {"stderr","Z"},{"closed","Z"},F_END };
static const struct rt_method rt_console[]={
  M("write","(I)V",console_write),M("write","([B)V",console_write),M("write","([BII)V",console_write),
  M("flush","()V",console_write),M("close","()V",console_write),M_END
};
#define PS_PRINT_PAIR(sig) M("print",sig,ps_text),M("println",sig,ps_text)
static const struct rt_method rt_printstream[]={
  M("<init>","(Ljava/io/OutputStream;)V",ps_init),M("<init>","(Ljava/io/OutputStream;Z)V",ps_init),
  M("<init>","(Ljava/io/OutputStream;ZLjava/lang/String;)V",ps_init),
  M("<init>","(Ljava/lang/String;)V",ps_init),M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",ps_init),
  M("<init>","(Ljava/io/File;)V",ps_init),M("<init>","(Ljava/io/File;Ljava/lang/String;)V",ps_init),
  M("write","(I)V",ps_write),M("write","([B)V",ps_write),M("write","([BII)V",ps_write),
  PS_PRINT_PAIR("(Z)V"),PS_PRINT_PAIR("(C)V"),PS_PRINT_PAIR("(I)V"),PS_PRINT_PAIR("(J)V"),
  PS_PRINT_PAIR("(F)V"),PS_PRINT_PAIR("(D)V"),PS_PRINT_PAIR("([C)V"),
  PS_PRINT_PAIR("(Ljava/lang/String;)V"),PS_PRINT_PAIR("(Ljava/lang/Object;)V"),M("println","()V",ps_text),
  M("flush","()V",ps_control),M("close","()V",ps_control),M("checkError","()Z",ps_control),
  {"setError","()V",ps_control,DEX_ACC_PROTECTED},{"clearError","()V",ps_control,DEX_ACC_PROTECTED},
  M("append","(C)Ljava/io/PrintStream;",ps_append),M("append","(Ljava/lang/CharSequence;)Ljava/io/PrintStream;",ps_append),
  M("append","(Ljava/lang/CharSequence;II)Ljava/io/PrintStream;",ps_append),
  M("append","(C)Ljava/lang/Appendable;",ps_append),M("append","(Ljava/lang/CharSequence;)Ljava/lang/Appendable;",ps_append),
  M("append","(Ljava/lang/CharSequence;II)Ljava/lang/Appendable;",ps_append),
  M("format","(Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;",ps_format),
  M("format","(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;",ps_format),
  M("printf","(Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;",ps_format),
  M("printf","(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;",ps_format),M_END
};
#undef PS_PRINT_PAIR
static const char *const rt_printstream_ifaces[]={"Ljava/lang/Appendable;","Ljava/io/Closeable;",NULL};
