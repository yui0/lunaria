/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0
 * Channel contracts needed by stream adapters. Selection/provider operations
 * remain abstract; concrete channels must supply their actual behavior. */
static bool nio_channel_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  dvm_ref lock=dvm_new_object(vm,dvm_find_class(vm,"java/lang/Object"));
  if(!lock) { dvm__throw(vm,"java/lang/OutOfMemoryError","channel close lock"); return false; }
  (void)dvm_set_field(vm,self,"closeLock","Ljava/lang/Object;",(union dvm_value){.l=lock});
  (void)dvm_set_field(vm,self,"closed","Z",(union dvm_value){0});
  RETV();
}
static bool nio_channel_open(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value closed={0};
  (void)dvm_get_field(vm,self,"closed","Z",&closed);
  RETI(!closed.i);
}
static bool nio_channel_close(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value lock={0},closed={0};
  (void)dvm_get_field(vm,self,"closeLock","Ljava/lang/Object;",&lock);
  if(!lock.l) {
    union dvm_value ignored={0};
    if(!nio_channel_init(vm,self,NULL,0,&ignored)) return false;
    (void)dvm_get_field(vm,self,"closeLock","Ljava/lang/Object;",&lock);
  }
  if(!dvm__monitor_enter(vm,lock.l)) return false;
  (void)dvm_get_field(vm,self,"closed","Z",&closed);
  bool good=true;
  if(!closed.i) {
    (void)dvm_set_field(vm,self,"closed","Z",(union dvm_value){.i=1});
    good=am_vcall(vm,self,"implCloseChannel","()V",NULL,0,out);
  }
  if(!dvm__monitor_exit(vm,lock.l)) good=false;
  return good;
}
static bool nio_channel_register(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  union dvm_value expanded[3]={ARG(0),ARG(1),{0}};
  return am_vcall(vm,self,"register","(Ljava/nio/channels/Selector;ILjava/lang/Object;)Ljava/nio/channels/SelectionKey;",expanded,3,out);
}
#define NIO_ABSTRACT(name,sig) {name,sig,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}
static const struct rt_method rt_channel_contract[]={ NIO_ABSTRACT("close","()V"),NIO_ABSTRACT("isOpen","()Z"),M_END };
static const char *const rt_channel_ifaces[]={"Ljava/io/Closeable;",NULL};
static const char *const rt_interruptible_ifaces[]={"Ljava/nio/channels/Channel;",NULL};
static const char *const rt_interruptible_base_ifaces[]={"Ljava/nio/channels/Channel;","Ljava/nio/channels/InterruptibleChannel;",NULL};
static const struct rt_field rt_interruptible_channel_fields[]={ {"closed","Z"},{"closeLock","Ljava/lang/Object;"},F_END };
static const struct rt_method rt_interruptible_channel[]={
  {"<init>","()V",nio_channel_init,DEX_ACC_PROTECTED},
  {"close","()V",nio_channel_close,DEX_ACC_PUBLIC|DEX_ACC_FINAL},
  {"isOpen","()Z",nio_channel_open,DEX_ACC_PUBLIC|DEX_ACC_FINAL},
  {"implCloseChannel","()V",NULL,DEX_ACC_PROTECTED|DEX_ACC_ABSTRACT},M_END
};
static const struct rt_method rt_selectable_channel[]={
  {"<init>","()V",nio_channel_init,DEX_ACC_PROTECTED},
  NIO_ABSTRACT("provider","()Ljava/nio/channels/spi/SelectorProvider;"),NIO_ABSTRACT("validOps","()I"),
  NIO_ABSTRACT("isRegistered","()Z"),NIO_ABSTRACT("keyFor","(Ljava/nio/channels/Selector;)Ljava/nio/channels/SelectionKey;"),
  NIO_ABSTRACT("register","(Ljava/nio/channels/Selector;ILjava/lang/Object;)Ljava/nio/channels/SelectionKey;"),
  {"register","(Ljava/nio/channels/Selector;I)Ljava/nio/channels/SelectionKey;",nio_channel_register,DEX_ACC_PUBLIC|DEX_ACC_FINAL},
  NIO_ABSTRACT("configureBlocking","(Z)Ljava/nio/channels/SelectableChannel;"),
  NIO_ABSTRACT("isBlocking","()Z"),NIO_ABSTRACT("blockingLock","()Ljava/lang/Object;"),M_END
};
#undef NIO_ABSTRACT
