/* MPL-2.0. Apache HTTP parameter contracts used by Android's legacy client.
 * Uses the existing Java Map, boxing and virtual-call implementations.
 * The HTTP transport is separate; these objects never manufacture responses.
 */
static const struct rt_field rt_http_params_fields[]={
 {"parameters","Ljava/util/HashMap;"},F_END
};
static const struct rt_field rt_defaulted_params_fields[]={
 {"local","Lorg/apache/http/params/HttpParams;"},{"defaults","Lorg/apache/http/params/HttpParams;"},F_END
};
static const char *const rt_http_params_iface[]={"Lorg/apache/http/params/HttpParams;",NULL};
static const char *const rt_basic_params_iface[]={"Ljava/io/Serializable;","Ljava/lang/Cloneable;",NULL};
static dvm_ref http_params_field(struct dvm *vm,dvm_ref self,const char *name,const char *type)
{union dvm_value value={0};dvm_get_field(vm,self,name,type,&value);return value.l;}
static bool http_params_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;(void)out;
 dvm_set_field(vm,self,"parameters","Ljava/util/HashMap;",(union dvm_value){0});RETV();
}
static bool http_params_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
 if(!map)RETL(0);
 return map_get(vm,map,args,nargs,out);
}
static bool http_params_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");union dvm_value ignored={0};
 if(!map){
  map=dvm_new_object(vm,dvm_find_class(vm,"java/util/HashMap"));
  if(!map || !map_init(vm,map,NULL,0,&ignored))return false;
  dvm_set_field(vm,self,"parameters","Ljava/util/HashMap;",(union dvm_value){.l=map});
 }
 if(!map_put(vm,map,args,nargs,&ignored))return false;RETL(self);
}
static bool http_params_remove(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");union dvm_value found={0},ignored={0};
 if(!map)RETI(0);
 if(!map_containsKey(vm,map,args,nargs,&found))return false;
 if(found.i && !map_remove(vm,map,args,nargs,&ignored))return false;
 RETI(found.i);
}
static bool http_params_clear(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;(void)out;
 dvm_set_field(vm,self,"parameters","Ljava/util/HashMap;",(union dvm_value){0});RETV();
}
static bool http_params_typed_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;union dvm_value found={0};
 if(!am_vcall(vm,self,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",args,1,&found))return false;
 if(!found.l){*out=ARG(1);return true;}
 const char *name=builtin_name(),*type=strstr(name,"Long") ? "java/lang/Long" : strstr(name,"Double") ? "java/lang/Double" : strstr(name,"Int") ? "java/lang/Integer" : "java/lang/Boolean";
 if(!object_is_a(vm,found.l,type)){dvm__throw(vm,"java/lang/ClassCastException","HTTP parameter is not %s",type);return false;}
 if(strstr(name,"Long"))return box_longValue(vm,found.l,NULL,0,out);
 if(strstr(name,"Double"))return box_doubleValue(vm,found.l,NULL,0,out);
 return box_intValue(vm,found.l,NULL,0,out);
}
static bool http_params_typed_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;const char *name=builtin_name(),*type=strstr(name,"Long") ? "Ljava/lang/Long;" : strstr(name,"Double") ? "Ljava/lang/Double;" : strstr(name,"Int") ? "Ljava/lang/Integer;" : "Ljava/lang/Boolean;";
 union dvm_value pair[2]={ARG(0),{0}},ignored={0};
 if(strstr(name,"Boolean")){
  struct dvm_class *boolean=dvm_find_class(vm,"java/lang/Boolean");
  if(!boolean || !dvm_init_class(vm,boolean) || !dvm_get_static(vm,boolean,ARG(1).i ? "TRUE" : "FALSE","Ljava/lang/Boolean;",&pair[1]))return false;
 }else pair[1].l=box_make(vm,type,ARG(1));
 if(!pair[1].l || !am_vcall(vm,self,"setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",pair,2,&ignored))return false;
 RETL(self);
}
static bool http_params_predicate(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;bool negate=!strcmp(builtin_name(),"isParameterFalse");
 union dvm_value query[2]={ARG(0),{.i=0}},value={0};
 if(!am_vcall(vm,self,"getBooleanParameter","(Ljava/lang/String;Z)Z",query,2,&value))return false;
 RETI(negate ? !value.i : value.i!=0);
}
static bool http_params_is_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 union dvm_value value={0};
 bool good=!strcmp(builtin_name(),"isParameterSetLocally") ? http_params_get(vm,self,args,nargs,&value) : am_vcall(vm,self,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",args,nargs,&value);
 if(!good)return false;RETI(value.l!=0);
}
static bool http_params_set_many(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;
 if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","names");return false;}
 uint32_t count=dvm_array_length(vm,ARG(0).l);
 for(uint32_t i=0;i<count;++i){
  union dvm_value pair[2]={{.l=((dvm_ref *)dvm_array_data(vm,ARG(0).l))[i]},ARG(1)},ignored={0};
  if(!am_vcall(vm,self,"setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",pair,2,&ignored))return false;
 }
 RETV();
}
static bool http_params_copy_into(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
 if(!map)RETV();union dvm_value set={0},iterator={0},entry={0},has={0};
 if(!map_entrySet(vm,map,NULL,0,&set) || !am_vcall(vm,set.l,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
 for(;;){
  if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&has))return false;
  if(!has.i)break;
  if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&entry))return false;
  union dvm_value pair[2]={{0}},ignored={0};
  if(!am_vcall(vm,entry.l,"getKey","()Ljava/lang/Object;",NULL,0,&pair[0]))return false;
  if(!pair[0].l || !object_is_a(vm,pair[0].l,"java/lang/String"))continue;
  if(!am_vcall(vm,entry.l,"getValue","()Ljava/lang/Object;",NULL,0,&pair[1]) ||
     !am_vcall(vm,ARG(0).l,"setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",pair,2,&ignored))return false;
 }
 RETV();
}
static bool http_params_copy(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;dvm_ref copy=dvm_new_object(vm,dvm_object_class(vm,self));if(!copy)return false;
 /* BasicHttpParams.clone is an Object.clone followed by copyParams: the
  * source's lazy HashMap is shallowly shared. copy() starts with a new map. */
 if(!strcmp(builtin_name(),"clone")){
  dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
  dvm_set_field(vm,copy,"parameters","Ljava/util/HashMap;",(union dvm_value){.l=map});
 }
 union dvm_value target={.l=copy},ignored={0};
 if(!http_params_copy_into(vm,self,&target,1,&ignored))return false;RETL(copy);
}
static bool http_defaulted_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;
 if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","HTTP parameters may not be null");return false;}
 dvm_set_field(vm,self,"local","Lorg/apache/http/params/HttpParams;",ARG(0));
 dvm_set_field(vm,self,"defaults","Lorg/apache/http/params/HttpParams;",ARG(1));RETV();
}
static bool http_defaulted_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 dvm_ref local=http_params_field(vm,self,"local","Lorg/apache/http/params/HttpParams;");
 if(!am_vcall(vm,local,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",args,nargs,out))return false;
 dvm_ref defaults=http_params_field(vm,self,"defaults","Lorg/apache/http/params/HttpParams;");
 if(!out->l && defaults)return am_vcall(vm,defaults,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",args,nargs,out);
 return true;
}
static bool http_defaulted_mutate(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 dvm_ref local=http_params_field(vm,self,"local","Lorg/apache/http/params/HttpParams;");
 return am_vcall(vm,local,builtin_name(),builtin_sig(),args,nargs,out);
}
static bool http_defaulted_defaults(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;RETL(http_params_field(vm,self,"defaults","Lorg/apache/http/params/HttpParams;"));}
static bool http_defaulted_copy(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;union dvm_value pair[2]={{0}},ignored={0};
 dvm_ref local=http_params_field(vm,self,"local","Lorg/apache/http/params/HttpParams;");
 if(!am_vcall(vm,local,"copy","()Lorg/apache/http/params/HttpParams;",NULL,0,&pair[0]))return false;
 pair[1].l=http_params_field(vm,self,"defaults","Lorg/apache/http/params/HttpParams;");
 dvm_ref copy=dvm_new_object(vm,dvm_object_class(vm,self));
 if(!copy || !http_defaulted_init(vm,copy,pair,2,&ignored))return false;RETL(copy);
}
static bool http_connection_parameter(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)self;(void)nargs;
 if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","HTTP parameters may not be null");return false;}
 const char *name=builtin_name();bool setter=!strncmp(name,"set",3);
 const struct {const char *name,*key;int value;bool boolean;} fields[]={
  {"SoTimeout","http.socket.timeout",0,false},{"TcpNoDelay","http.tcp.nodelay",1,true},
  {"SocketBufferSize","http.socket.buffer-size",-1,false},{"Linger","http.socket.linger",-1,false},
  {"ConnectionTimeout","http.connection.timeout",0,false},{"StaleCheckingEnabled","http.connection.stalecheck",1,true}
 };
 for(size_t i=0;i<sizeof fields/sizeof fields[0];++i)if(!strcmp(name+3,fields[i].name) || (!strcmp(name,"isStaleCheckingEnabled") && i==5)){
  union dvm_value query[2]={{.l=dvm_new_string(vm,fields[i].key)},setter ? ARG(1) : (union dvm_value){.i=fields[i].value}},result={0};
  const char *method=setter ? fields[i].boolean ? "setBooleanParameter" : "setIntParameter" : fields[i].boolean ? "getBooleanParameter" : "getIntParameter";
  const char *sig=setter ? fields[i].boolean ? "(Ljava/lang/String;Z)Lorg/apache/http/params/HttpParams;" : "(Ljava/lang/String;I)Lorg/apache/http/params/HttpParams;" : fields[i].boolean ? "(Ljava/lang/String;Z)Z" : "(Ljava/lang/String;I)I";
  if(!am_vcall(vm,ARG(0).l,method,sig,query,2,&result))return false;
  if(!setter)*out=result;return true;
 }
 dvm__throw(vm,"java/lang/NoSuchMethodError","%s",name);return false;
}
#define HTTP_PARAM_DECL(n,s) {n,s,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}
static const struct rt_method rt_http_params_decl[]={
 HTTP_PARAM_DECL("getParameter","(Ljava/lang/String;)Ljava/lang/Object;"),HTTP_PARAM_DECL("setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;"),
 HTTP_PARAM_DECL("copy","()Lorg/apache/http/params/HttpParams;"),HTTP_PARAM_DECL("removeParameter","(Ljava/lang/String;)Z"),
 HTTP_PARAM_DECL("getLongParameter","(Ljava/lang/String;J)J"),HTTP_PARAM_DECL("setLongParameter","(Ljava/lang/String;J)Lorg/apache/http/params/HttpParams;"),
 HTTP_PARAM_DECL("getIntParameter","(Ljava/lang/String;I)I"),HTTP_PARAM_DECL("setIntParameter","(Ljava/lang/String;I)Lorg/apache/http/params/HttpParams;"),
 HTTP_PARAM_DECL("getDoubleParameter","(Ljava/lang/String;D)D"),HTTP_PARAM_DECL("setDoubleParameter","(Ljava/lang/String;D)Lorg/apache/http/params/HttpParams;"),
 HTTP_PARAM_DECL("getBooleanParameter","(Ljava/lang/String;Z)Z"),HTTP_PARAM_DECL("setBooleanParameter","(Ljava/lang/String;Z)Lorg/apache/http/params/HttpParams;"),
 HTTP_PARAM_DECL("isParameterTrue","(Ljava/lang/String;)Z"),HTTP_PARAM_DECL("isParameterFalse","(Ljava/lang/String;)Z"),M_END
};
#undef HTTP_PARAM_DECL
static const struct rt_method rt_http_abstract_params[]={
 M("<init>","()V",nop_void),
 M("getLongParameter","(Ljava/lang/String;J)J",http_params_typed_get),M("setLongParameter","(Ljava/lang/String;J)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
 M("getIntParameter","(Ljava/lang/String;I)I",http_params_typed_get),M("setIntParameter","(Ljava/lang/String;I)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
 M("getDoubleParameter","(Ljava/lang/String;D)D",http_params_typed_get),M("setDoubleParameter","(Ljava/lang/String;D)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
 M("getBooleanParameter","(Ljava/lang/String;Z)Z",http_params_typed_get),M("setBooleanParameter","(Ljava/lang/String;Z)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
 M("isParameterTrue","(Ljava/lang/String;)Z",http_params_predicate),M("isParameterFalse","(Ljava/lang/String;)Z",http_params_predicate),M_END
};
static const struct rt_method rt_http_basic_params[]={
 M("<init>","()V",http_params_init),M("getParameter","(Ljava/lang/String;)Ljava/lang/Object;",http_params_get),
 M("setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",http_params_set),M("removeParameter","(Ljava/lang/String;)Z",http_params_remove),
 M("setParameters","([Ljava/lang/String;Ljava/lang/Object;)V",http_params_set_many),M("isParameterSet","(Ljava/lang/String;)Z",http_params_is_set),M("isParameterSetLocally","(Ljava/lang/String;)Z",http_params_is_set),
 M("clear","()V",http_params_clear),M("copy","()Lorg/apache/http/params/HttpParams;",http_params_copy),M("clone","()Ljava/lang/Object;",http_params_copy),
 M("copyParams","(Lorg/apache/http/params/HttpParams;)V",http_params_copy_into),M_END
};
static const struct rt_method rt_http_defaulted_params[]={
 M("<init>","(Lorg/apache/http/params/HttpParams;Lorg/apache/http/params/HttpParams;)V",http_defaulted_init),M("getParameter","(Ljava/lang/String;)Ljava/lang/Object;",http_defaulted_get),
 M("setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",http_defaulted_mutate),M("removeParameter","(Ljava/lang/String;)Z",http_defaulted_mutate),
 M("getDefaults","()Lorg/apache/http/params/HttpParams;",http_defaulted_defaults),M("copy","()Lorg/apache/http/params/HttpParams;",http_defaulted_copy),M_END
};
static const struct rt_method rt_http_connection_params[]={
 SM("getSoTimeout","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setSoTimeout","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
 SM("getTcpNoDelay","(Lorg/apache/http/params/HttpParams;)Z",http_connection_parameter),SM("setTcpNoDelay","(Lorg/apache/http/params/HttpParams;Z)V",http_connection_parameter),
 SM("getSocketBufferSize","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setSocketBufferSize","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
 SM("getLinger","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setLinger","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
 SM("getConnectionTimeout","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setConnectionTimeout","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
 SM("isStaleCheckingEnabled","(Lorg/apache/http/params/HttpParams;)Z",http_connection_parameter),SM("setStaleCheckingEnabled","(Lorg/apache/http/params/HttpParams;Z)V",http_connection_parameter),M_END
};
