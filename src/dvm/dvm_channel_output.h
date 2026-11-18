/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0 */
static bool channels_new_output(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self; (void)nargs;
  if(!ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","\"ch\" is null!"); return false; }
  dvm_ref stream=dvm_new_object(vm,dvm_find_class(vm,"java/nio/channels/Channels$1"));
  if(!stream) { dvm__throw(vm,"java/lang/OutOfMemoryError","channel output stream"); return false; }
  (void)dvm_set_field(vm,stream,"ch","Ljava/nio/channels/WritableByteChannel;",ARG(0));
  RETL(stream);
}
static bool channel_output_fully(struct dvm *vm,dvm_ref channel,dvm_ref buffer)
{
  union dvm_value ignored={0},arg={.l=buffer};
  for(;;) {
    int32_t pos=0,limit=0;
    (void)stream_field(vm,buffer,"pos",&pos);
    (void)stream_field(vm,buffer,"count",&limit);
    if(pos>=limit) return true;
    if(!am_vcall(vm,channel,"write","(Ljava/nio/ByteBuffer;)I",&arg,1,&ignored)) return false;
    if(ignored.i<=0) { dvm__throw(vm,"java/lang/RuntimeException","no bytes written"); return false; }
  }
  return true;
}
static bool channel_output_write(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(!dvm__monitor_enter(vm,self)) return false;
  union dvm_value array={0},buffer={0},channel={0},old={0};
  bool good=true;
  int32_t offset=0,length=0;
  if(!strcmp(builtin_sig(),"(I)V")) {
    (void)dvm_get_field(vm,self,"b1","[B",&array);
    if(!array.l) {
      array.l=dvm_new_array(vm,'B',"B",1);
      (void)dvm_set_field(vm,self,"b1","[B",array);
    }
    if(!array.l) { dvm__throw(vm,"java/lang/OutOfMemoryError","channel byte buffer"); good=false; }
    else { ((uint8_t *)dvm_array_data(vm,array.l))[0]=(uint8_t)ARG(0).i; length=1; }
  } else {
    array=ARG(0);
    if(!array.l) { dvm__throw(vm,"java/lang/NullPointerException","buffer"); good=false; }
    else {
      uint32_t count=dvm_array_length(vm,array.l);
      offset=nargs==3 ? ARG(1).i : 0;
      length=nargs==3 ? ARG(2).i : (int32_t)count;
      if(offset<0 || length<0 || (uint32_t)offset>count || (uint32_t)length>count-(uint32_t)offset) {
        dvm__throw(vm,"java/lang/IndexOutOfBoundsException",NULL); good=false;
      }
    }
  }
  if(good && length) {
    (void)dvm_get_field(vm,self,"ch","Ljava/nio/channels/WritableByteChannel;",&channel);
    (void)dvm_get_field(vm,self,"bs","[B",&old);
    if(old.l==array.l) (void)dvm_get_field(vm,self,"bb","Ljava/nio/ByteBuffer;",&buffer);
    else {
      good=bb_wrap(vm,0,&array,1,&buffer);
      if(good) {
        (void)dvm_set_field(vm,self,"bs","[B",array);
        (void)dvm_set_field(vm,self,"bb","Ljava/nio/ByteBuffer;",buffer);
      }
    }
    if(good && !buffer.l) { dvm__throw(vm,"java/lang/OutOfMemoryError","channel ByteBuffer"); good=false; }
    if(good) {
      stream_set_field(vm,buffer.l,"count",offset+length);
      stream_set_field(vm,buffer.l,"pos",offset);
      struct dvm_class *selectable=dvm_find_class(vm,"java/nio/channels/SelectableChannel");
      if(selectable && dvm__class_assignable(vm,dvm_object_class(vm,channel.l),selectable)) {
        union dvm_value lock={0},blocking={0};
        good=am_vcall(vm,channel.l,"blockingLock","()Ljava/lang/Object;",NULL,0,&lock);
        if(good) good=dvm__monitor_enter(vm,lock.l);
        if(good) {
          good=am_vcall(vm,channel.l,"isBlocking","()Z",NULL,0,&blocking);
          if(good && !blocking.i) { dvm__throw(vm,"java/nio/channels/IllegalBlockingModeException",NULL); good=false; }
          if(good) good=channel_output_fully(vm,channel.l,buffer.l);
          if(!dvm__monitor_exit(vm,lock.l)) good=false;
        }
      } else good=channel_output_fully(vm,channel.l,buffer.l);
    }
  }
  if(!dvm__monitor_exit(vm,self)) good=false;
  (void)out;
  return good;
}
static bool channel_output_close(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value channel={0};
  (void)dvm_get_field(vm,self,"ch","Ljava/nio/channels/WritableByteChannel;",&channel);
  return am_vcall(vm,channel.l,"close","()V",NULL,0,out);
}
static const struct rt_field rt_channel_output_fields[]={
  {"ch","Ljava/nio/channels/WritableByteChannel;"},{"bs","[B"},
  {"bb","Ljava/nio/ByteBuffer;"},{"b1","[B"},F_END
};
static const struct rt_method rt_channel_output[]={
  M("write","(I)V",channel_output_write),M("write","([B)V",channel_output_write),
  M("write","([BII)V",channel_output_write),M("close","()V",channel_output_close),M_END
};
