/* MPL-2.0. Account's Parcelable reader, used inside GMS TokenResponse. */
static bool account_init_parcel(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   struct rt_parcel *p=args[0].l?parcel_of(vm,args[0].l):NULL;
   if(!p){dvm__throw(vm,"java/lang/NullPointerException","Parcel");return false;}
   dvm_pin(vm,self);
   union dvm_value values[3]={{0},{0},{0}},ignored={0};
   bool ok=parcel_take_string(vm,p,&values[0].l);
   if(ok)dvm_set_field(vm,self,"name","Ljava/lang/String;",values[0]);
   if(ok)ok=parcel_take_string(vm,p,&values[1].l);
   if(ok)dvm_set_field(vm,self,"type","Ljava/lang/String;",values[1]);
   if(ok)ok=parcel_take_string(vm,p,&values[2].l);
   if(ok){
      dvm_set_field(vm,self,"accessId","Ljava/lang/String;",values[2]);
      ok=account_init(vm,self,values,2,&ignored);
   }
   dvm_unpin(vm,self);
   if(!ok)return false;
   RETV();
}
static bool account_creator_create(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;
   struct dvm_class *c=dvm__class_by_desc(vm,"Landroid/accounts/Account;");
   dvm_ref account=c?dvm_new_object(vm,c):0;
   if(!account)return false;
   union dvm_value ignored={0};
   if(!account_init_parcel(vm,account,args,nargs,&ignored))return false;
   RETL(account);
}
static bool account_creator_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   if(args[0].i<0){dvm__throw(vm,"java/lang/NegativeArraySizeException","Account[]");return false;}
   RETL(dvm_new_array(vm,'L',"Landroid/accounts/Account;",args[0].i));
}
static bool account_get_access_id(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   union dvm_value id={0};dvm_get_field(vm,self,"accessId","Ljava/lang/String;",&id);RETL(id.l);
}
static const struct rt_method rt_account_parcel_more[]={
   M("<init>","(Landroid/os/Parcel;)V",account_init_parcel),
   M("getAccessId","()Ljava/lang/String;",account_get_access_id),M_END
};
static const struct rt_method rt_account_creator[]={
   M("createFromParcel","(Landroid/os/Parcel;)Ljava/lang/Object;",account_creator_create),
   M("newArray","(I)[Ljava/lang/Object;",account_creator_array),M_END
};
