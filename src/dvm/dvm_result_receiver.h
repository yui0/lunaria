/* MPL-2.0. ResultReceiver delivers completion locally or through its Binder. */
#define RR_DESCRIPTOR "com.android.internal.os.IResultReceiver"
static const struct rt_field rt_result_receiver_fields[]={
   {"handler","Landroid/os/Handler;"},{"binder","Landroid/os/IBinder;"},{"local","Z"},F_END
};
static const struct rt_field rt_result_receiver_task_fields[]={
   {"receiver","Landroid/os/ResultReceiver;"},{"code","I"},{"data","Landroid/os/Bundle;"},F_END
};
static const struct rt_field rt_result_receiver_binder_fields[]={
   {"receiver","Landroid/os/ResultReceiver;"},F_END
};
static bool rr_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_set_field(vm,self,"handler","Landroid/os/Handler;",args[0]);
   dvm_set_field(vm,self,"local","Z",(union dvm_value){.i=1});
   RETV();
}
static bool rr_deliver(struct dvm *vm,dvm_ref self,const union dvm_value *args)
{
   dvm_ref handler=aa_ref(vm,self,"handler","Landroid/os/Handler;");
   union dvm_value ignored={0};
   if(!handler)return am_vcall(vm,self,"onReceiveResult","(ILandroid/os/Bundle;)V",args,2,&ignored);
   struct dvm_class *c=dvm__class_by_desc(vm,"Llunaria/ResultReceiverTask;");
   dvm_ref task=c?dvm_new_object(vm,c):0;
   if(!task)return false;
   dvm_set_field(vm,task,"receiver","Landroid/os/ResultReceiver;",(union dvm_value){.l=self});
   dvm_set_field(vm,task,"code","I",args[0]);
   dvm_set_field(vm,task,"data","Landroid/os/Bundle;",args[1]);
   union dvm_value a={.l=task};
   return h_post(vm,handler,&a,1,&ignored);
}
static bool rr_task_run(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   union dvm_value a[2]={{0},{0}};
   dvm_get_field(vm,self,"code","I",&a[0]);
   dvm_get_field(vm,self,"data","Landroid/os/Bundle;",&a[1]);
   dvm_ref receiver=aa_ref(vm,self,"receiver","Landroid/os/ResultReceiver;");
   return am_vcall(vm,receiver,"onReceiveResult","(ILandroid/os/Bundle;)V",a,2,out);
}
static bool rr_send(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   union dvm_value local={0};dvm_get_field(vm,self,"local","Z",&local);
   if(local.i){if(!rr_deliver(vm,self,args))return false;RETV();}
   dvm_ref binder=aa_ref(vm,self,"binder","Landroid/os/IBinder;");
   if(!binder)RETV();
   dvm_ref data=aa_parcel_new(vm,RR_DESCRIPTOR);
   if(!data)return false;
   dvm_pin(vm,data);
   struct rt_parcel *p=parcel_of(vm,data);
   bool ok=p && parcel_put_i32(vm,p,args[0].i) && aa_parcel_put_bundle(vm,data,args[1].l);
   if(ok)ok=aa_transact(vm,binder,1,data);
   else {union dvm_value ignored={0};parcel_recycle(vm,data,NULL,0,&ignored);}
   dvm_unpin(vm,data);
   /* Android silently drops a result whose remote process has died. */
   if(!ok && vm->exception && dvm__class_assignable(vm,dvm_object_class(vm,vm->exception),dvm__class_by_desc(vm,"Landroid/os/RemoteException;"))){
      dvm_clear_exception(vm);RETV();
   }
   if(!ok)return false;
   RETV();
}
static bool rr_binder_transact(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(args[0].i!=1)return binder_on_transact(vm,self,args,nargs,out);
   if(!aa_parcel_enforce(vm,args[1].l,RR_DESCRIPTOR)){
      dvm__throw(vm,"java/lang/SecurityException","ResultReceiver interface token");return false;
   }
   struct rt_parcel *p=parcel_of(vm,args[1].l);
   union dvm_value a[2]={{.i=parcel_take_i32(p)},{0}};
   if(!aa_parcel_get_bundle(vm,args[1].l,&a[1].l))return false;
   dvm_ref receiver=aa_ref(vm,self,"receiver","Landroid/os/ResultReceiver;");
   if(!rr_deliver(vm,receiver,a))return false;
   RETI(1);
}
static bool rr_write(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   union dvm_value binder={.l=aa_ref(vm,self,"binder","Landroid/os/IBinder;")};
   if(!binder.l){
      union dvm_value local={0};dvm_get_field(vm,self,"local","Z",&local);
      if(local.i){
         struct dvm_class *c=dvm__class_by_desc(vm,"Llunaria/ResultReceiverBinder;");
         binder.l=c?dvm_new_object(vm,c):0;
         if(!binder.l)return false;
         dvm_set_field(vm,binder.l,"receiver","Landroid/os/ResultReceiver;",(union dvm_value){.l=self});
         dvm_set_field(vm,self,"binder","Landroid/os/IBinder;",binder);
      }
   }
   return parcel_writeStrongBinder(vm,args[0].l,&binder,1,out);
}
static bool rr_create(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   union dvm_value binder={0};
   if(!parcel_readStrongBinder(vm,args[0].l,NULL,0,&binder))return false;
   struct dvm_class *c=dvm__class_by_desc(vm,"Landroid/os/ResultReceiver;");
   dvm_ref receiver=c?dvm_new_object(vm,c):0;
   if(receiver)dvm_set_field(vm,receiver,"binder","Landroid/os/IBinder;",binder);
   RETL(receiver);
}
static bool rr_new_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   if(args[0].i<0){dvm__throw(vm,"java/lang/NegativeArraySizeException","ResultReceiver[]");return false;}
   RETL(dvm_new_array(vm,'L',"Landroid/os/ResultReceiver;",args[0].i));
}
static const struct rt_method rt_result_receiver[]={
   M("<init>","(Landroid/os/Handler;)V",rr_init),
   M("send","(ILandroid/os/Bundle;)V",rr_send),
   M("onReceiveResult","(ILandroid/os/Bundle;)V",nop_void),
   M("writeToParcel","(Landroid/os/Parcel;I)V",rr_write),
   M("describeContents","()I",aa_response_describe),M_END
};
static const struct rt_method rt_result_receiver_task[]={M("run","()V",rr_task_run),M_END};
static const struct rt_method rt_result_receiver_binder[]={
   M("onTransact","(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z",rr_binder_transact),M_END
};
static const struct rt_method rt_result_receiver_creator[]={
   M("createFromParcel","(Landroid/os/Parcel;)Ljava/lang/Object;",rr_create),
   M("newArray","(I)[Ljava/lang/Object;",rr_new_array),M_END
};
