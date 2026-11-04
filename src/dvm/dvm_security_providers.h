/* MPL-2.0. Per-VM JCA provider order, retained by Security's static field. */
static const struct rt_field rt_security_sfields[]={
   {"providers","[Ljava/security/Provider;"},{"properties","Ljava/util/Map;"},F_END
};
static dvm_ref security_provider_list(struct dvm *vm)
{
   struct dvm_class *c=dvm__class_by_desc(vm,"Ljava/security/Security;");
   union dvm_value list={0};
   if(!c)return 0;
   dvm_get_static(vm,c,"providers","[Ljava/security/Provider;",&list);
   if(!list.l){
      dvm_ref builtin=security_builtin_provider(vm);
      list.l=dvm_new_array(vm,'L',"Ljava/security/Provider;",1);
      if(!list.l)return 0;
      ((dvm_ref*)dvm_array_data(vm,list.l))[0]=builtin;
      dvm_set_static(vm,c,"providers","[Ljava/security/Provider;",list);
   }
   return list.l;
}
static const char *security_provider_name(struct dvm *vm,dvm_ref provider)
{
   union dvm_value name={0};
   dvm_get_field(vm,provider,"providerName","Ljava/lang/String;",&name);
   return name.l?dvm_string_utf8(vm,name.l):"AndroidOpenSSL";
}
static bool security_registered_provider(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   const char *name=args[0].l?dvm_string_utf8(vm,args[0].l):NULL;
   if(!name)RETL(0);
   dvm_ref list=security_provider_list(vm);
   if(!list)return false;
   for(uint32_t i=0;i<dvm_array_length(vm,list);++i){
      dvm_ref provider=((dvm_ref*)dvm_array_data(vm,list))[i];
      if(!strcmp(name,security_provider_name(vm,provider)))RETL(provider);
   }
   RETL(0);
}
static bool security_registered_providers(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;
   dvm_ref list=security_provider_list(vm);
   if(!list)return false;
   uint32_t n=dvm_array_length(vm,list);
   dvm_ref copy=dvm_new_array(vm,'L',"Ljava/security/Provider;",n);
   if(!copy)return false;
   memcpy(dvm_array_data(vm,copy),dvm_array_data(vm,list),n*sizeof(dvm_ref));
   RETL(copy);
}
static bool security_insert_provider(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;
   if(!args[0].l){dvm__throw(vm,"java/lang/NullPointerException","provider");return false;}
   dvm_ref list=security_provider_list(vm);
   if(!list)return false;
   uint32_t n=dvm_array_length(vm,list);
   const char *name=security_provider_name(vm,args[0].l);
   for(uint32_t i=0;i<n;++i){
      dvm_ref provider=((dvm_ref*)dvm_array_data(vm,list))[i];
      if(!strcmp(name,security_provider_name(vm,provider)))RETI(-1);
   }
   int pos=nargs>1?args[1].i:0;
   uint32_t at=pos>0 && (uint32_t)pos<=n+1?(uint32_t)pos-1:n;
   dvm_ref next=dvm_new_array(vm,'L',"Ljava/security/Provider;",n+1);
   if(!next)return false;
   dvm_ref *from=dvm_array_data(vm,list),*to=dvm_array_data(vm,next);
   memcpy(to,from,at*sizeof(dvm_ref));to[at]=args[0].l;
   memcpy(to+at+1,from+at,(n-at)*sizeof(dvm_ref));
   dvm_set_static(vm,dvm__class_by_desc(vm,"Ljava/security/Security;"),"providers","[Ljava/security/Provider;",(union dvm_value){.l=next});
   RETI(at+1);
}
static bool security_remove_provider(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   if(!args[0].l){dvm__throw(vm,"java/lang/NullPointerException","name");return false;}
   dvm_ref list=security_provider_list(vm);
   if(!list)return false;
   const char *name=dvm_string_utf8(vm,args[0].l);
   uint32_t n=dvm_array_length(vm,list),at=n;
   for(uint32_t i=0;i<n;++i){
      dvm_ref provider=((dvm_ref*)dvm_array_data(vm,list))[i];
      if(!strcmp(name,security_provider_name(vm,provider))){at=i;break;}
   }
   if(at==n)RETV();
   dvm_ref next=dvm_new_array(vm,'L',"Ljava/security/Provider;",n-1);
   if(!next)return false;
   dvm_ref *from=dvm_array_data(vm,list),*to=dvm_array_data(vm,next);
   memcpy(to,from,at*sizeof(dvm_ref));
   memcpy(to+at,from+at+1,(n-at-1)*sizeof(dvm_ref));
   dvm_set_static(vm,dvm__class_by_desc(vm,"Ljava/security/Security;"),"providers","[Ljava/security/Provider;",(union dvm_value){.l=next});
   RETV();
}

static bool security_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;
   if(!args[0].l || (nargs>1 && !args[1].l)){
      dvm__throw(vm,"java/lang/NullPointerException","security property");return false;
   }
   struct dvm_class *c=dvm__class_by_desc(vm,"Ljava/security/Security;");
   union dvm_value properties={0};
   dvm_get_static(vm,c,"properties","Ljava/util/Map;",&properties);
   if(!properties.l){
      struct dvm_class *mc=dvm__class_by_desc(vm,"Ljava/util/HashMap;");
      properties.l=mc?dvm_new_object(vm,mc):0;
      if(!properties.l)return false;
      dvm_pin(vm,properties.l);
      union dvm_value ignored={0};
      bool ok=map_init(vm,properties.l,NULL,0,&ignored);
      if(ok)dvm_set_static(vm,c,"properties","Ljava/util/Map;",properties);
      dvm_unpin(vm,properties.l);
      if(!ok)return false;
   }
   if(nargs>1){
      union dvm_value ignored={0};
      if(!map_put(vm,properties.l,args,2,&ignored))return false;
      RETV();
   }
   union dvm_value value={0};
   if(!map_get(vm,properties.l,args,1,&value))return false;
   if(!value.l)RETL(0);
   /* Security.getProperty trims ASCII whitespace as String.trim does. */
   const char *text=dvm_string_utf8(vm,value.l),*start=text;
   while(*start && (unsigned char)*start<=32)++start;
   size_t length=strlen(start);
   while(length && (unsigned char)start[length-1]<=32)--length;
   if(start==text && !start[length])RETL(value.l);
   char *trimmed=luna_strndup(start,length);
   if(!trimmed){dvm__throw(vm,"java/lang/OutOfMemoryError","security property");return false;}
   dvm_ref result=dvm_new_string(vm,trimmed);free(trimmed);RETL(result);
}
