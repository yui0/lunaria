/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* ======== merged from dvm_http_params.h ======== */
/* MPL-2.0. Apache HTTP parameter contracts used by Android's legacy client.
 * Uses the existing Java Map, boxing and virtual-call implementations.
 * The HTTP transport is separate; these objects never manufacture responses.
 */
static const struct rt_field rt_http_params_fields[]= {
  {"parameters","Ljava/util/HashMap;"},F_END
};
static const struct rt_field rt_defaulted_params_fields[]= {
  {"local","Lorg/apache/http/params/HttpParams;"},{"defaults","Lorg/apache/http/params/HttpParams;"},F_END
};
static const char *const rt_http_params_iface[]= {"Lorg/apache/http/params/HttpParams;",NULL};
static const char *const rt_basic_params_iface[]= {"Ljava/io/Serializable;","Ljava/lang/Cloneable;",NULL};
static dvm_ref http_params_field(struct dvm *vm,dvm_ref self,const char *name,const char *type)
{
  union dvm_value value= {0};
  dvm_get_field(vm,self,name,type,&value);
  return value.l;
}
static bool http_params_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  (void)out;
  dvm_set_field(vm,self,"parameters","Ljava/util/HashMap;",(union dvm_value) {
    0
  });
  RETV();
}
static bool http_params_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
  if(!map)RETL(0);
  return map_get(vm,map,args,nargs,out);
}
static bool http_params_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
  union dvm_value ignored= {0};
  if(!map) {
    map=dvm_new_object(vm,dvm_find_class(vm,"java/util/HashMap"));
    if(!map || !map_init(vm,map,NULL,0,&ignored))return false;
    dvm_set_field(vm,self,"parameters","Ljava/util/HashMap;",(union dvm_value) {
      .l=map
    });
  }
  if(!map_put(vm,map,args,nargs,&ignored))return false;
  RETL(self);
}
static bool http_params_remove(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
  union dvm_value found= {0},ignored= {0};
  if(!map)RETI(0);
  if(!map_containsKey(vm,map,args,nargs,&found))return false;
  if(found.i && !map_remove(vm,map,args,nargs,&ignored))return false;
  RETI(found.i);
}
static bool http_params_clear(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  (void)out;
  dvm_set_field(vm,self,"parameters","Ljava/util/HashMap;",(union dvm_value) {
    0
  });
  RETV();
}
static bool http_params_typed_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  union dvm_value found= {0};
  if(!am_vcall(vm,self,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",args,1,&found))return false;
  if(!found.l) {
    *out=ARG(1);
    return true;
  }
  const char *name=builtin_name(),*type=strstr(name,"Long") ? "java/lang/Long" : strstr(name,"Double") ? "java/lang/Double" : strstr(name,"Int") ? "java/lang/Integer" : "java/lang/Boolean";
  if(!object_is_a(vm,found.l,type)) {
    dvm__throw(vm,"java/lang/ClassCastException","HTTP parameter is not %s",type);
    return false;
  }
  if(strstr(name,"Long"))return box_longValue(vm,found.l,NULL,0,out);
  if(strstr(name,"Double"))return box_doubleValue(vm,found.l,NULL,0,out);
  return box_intValue(vm,found.l,NULL,0,out);
}
static bool http_params_typed_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  const char *name=builtin_name(),*type=strstr(name,"Long") ? "Ljava/lang/Long;" : strstr(name,"Double") ? "Ljava/lang/Double;" : strstr(name,"Int") ? "Ljava/lang/Integer;" : "Ljava/lang/Boolean;";
  union dvm_value pair[2]= {ARG(0),{0}},ignored= {0};
  if(strstr(name,"Boolean")) {
    struct dvm_class *boolean=dvm_find_class(vm,"java/lang/Boolean");
    if(!boolean || !dvm_init_class(vm,boolean) || !dvm_get_static(vm,boolean,ARG(1).i ? "TRUE" : "FALSE","Ljava/lang/Boolean;",&pair[1]))return false;
  } else pair[1].l=box_make(vm,type,ARG(1));
  if(!pair[1].l || !am_vcall(vm,self,"setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",pair,2,&ignored))return false;
  RETL(self);
}
static bool http_params_predicate(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  bool negate=!strcmp(builtin_name(),"isParameterFalse");
  union dvm_value query[2]= {ARG(0),{.i=0}},value= {0};
  if(!am_vcall(vm,self,"getBooleanParameter","(Ljava/lang/String;Z)Z",query,2,&value))return false;
  RETI(negate ? !value.i : value.i!=0);
}
static bool http_params_is_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  union dvm_value value= {0};
  bool good=!strcmp(builtin_name(),"isParameterSetLocally") ? http_params_get(vm,self,args,nargs,&value) : am_vcall(vm,self,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",args,nargs,&value);
  if(!good)return false;
  RETI(value.l!=0);
}
static bool http_params_set_many(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/NullPointerException","names");
    return false;
  }
  uint32_t count=dvm_array_length(vm,ARG(0).l);
  for(uint32_t i=0; i<count; ++i) {
    union dvm_value pair[2]= {{.l=((dvm_ref *)dvm_array_data(vm,ARG(0).l))[i]},ARG(1)},ignored= {0};
    if(!am_vcall(vm,self,"setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",pair,2,&ignored))return false;
  }
  RETV();
}
static bool http_params_copy_into(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
  if(!map)RETV();
  union dvm_value set= {0},iterator= {0},entry= {0},has= {0};
  if(!map_entrySet(vm,map,NULL,0,&set) || !am_vcall(vm,set.l,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
  for(;;) {
    if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&has))return false;
    if(!has.i)break;
    if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&entry))return false;
    union dvm_value pair[2]= {{0}},ignored= {0};
    if(!am_vcall(vm,entry.l,"getKey","()Ljava/lang/Object;",NULL,0,&pair[0]))return false;
    if(!pair[0].l || !object_is_a(vm,pair[0].l,"java/lang/String"))continue;
    if(!am_vcall(vm,entry.l,"getValue","()Ljava/lang/Object;",NULL,0,&pair[1]) ||
        !am_vcall(vm,ARG(0).l,"setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",pair,2,&ignored))return false;
  }
  RETV();
}
static bool http_params_copy(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  dvm_ref copy=dvm_new_object(vm,dvm_object_class(vm,self));
  if(!copy)return false;
  /* BasicHttpParams.clone is an Object.clone followed by copyParams: the
   * source's lazy HashMap is shallowly shared. copy() starts with a new map. */
  if(!strcmp(builtin_name(),"clone")) {
    dvm_ref map=http_params_field(vm,self,"parameters","Ljava/util/HashMap;");
    dvm_set_field(vm,copy,"parameters","Ljava/util/HashMap;",(union dvm_value) {
      .l=map
    });
  }
  union dvm_value target= {.l=copy},ignored= {0};
  if(!http_params_copy_into(vm,self,&target,1,&ignored))return false;
  RETL(copy);
}
static bool http_defaulted_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","HTTP parameters may not be null");
    return false;
  }
  dvm_set_field(vm,self,"local","Lorg/apache/http/params/HttpParams;",ARG(0));
  dvm_set_field(vm,self,"defaults","Lorg/apache/http/params/HttpParams;",ARG(1));
  RETV();
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
{
  (void)args;
  (void)nargs;
  RETL(http_params_field(vm,self,"defaults","Lorg/apache/http/params/HttpParams;"));
}
static bool http_defaulted_copy(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value pair[2]= {{0}},ignored= {0};
  dvm_ref local=http_params_field(vm,self,"local","Lorg/apache/http/params/HttpParams;");
  if(!am_vcall(vm,local,"copy","()Lorg/apache/http/params/HttpParams;",NULL,0,&pair[0]))return false;
  pair[1].l=http_params_field(vm,self,"defaults","Lorg/apache/http/params/HttpParams;");
  dvm_ref copy=dvm_new_object(vm,dvm_object_class(vm,self));
  if(!copy || !http_defaulted_init(vm,copy,pair,2,&ignored))return false;
  RETL(copy);
}
static bool http_connection_parameter(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","HTTP parameters may not be null");
    return false;
  }
  const char *name=builtin_name();
  bool setter=!strncmp(name,"set",3);
  const struct {
    const char *name,*key;
    int value;
    bool boolean;
  } fields[]= {
    {"SoTimeout","http.socket.timeout",0,false},{"TcpNoDelay","http.tcp.nodelay",1,true},
    {"SocketBufferSize","http.socket.buffer-size",-1,false},{"Linger","http.socket.linger",-1,false},
    {"ConnectionTimeout","http.connection.timeout",0,false},{"StaleCheckingEnabled","http.connection.stalecheck",1,true}
  };
  for(size_t i=0; i<sizeof fields/sizeof fields[0]; ++i)if(!strcmp(name+3,fields[i].name) || (!strcmp(name,"isStaleCheckingEnabled") && i==5)) {
      union dvm_value query[2]= {{.l=dvm_new_string(vm,fields[i].key)},setter ? ARG(1) : (union dvm_value) {
          .i=fields[i].value
        }
      },result= {0};
      const char *method=setter ? fields[i].boolean ? "setBooleanParameter" : "setIntParameter" : fields[i].boolean ? "getBooleanParameter" : "getIntParameter";
      const char *sig=setter ? fields[i].boolean ? "(Ljava/lang/String;Z)Lorg/apache/http/params/HttpParams;" : "(Ljava/lang/String;I)Lorg/apache/http/params/HttpParams;" : fields[i].boolean ? "(Ljava/lang/String;Z)Z" : "(Ljava/lang/String;I)I";
      if(!am_vcall(vm,ARG(0).l,method,sig,query,2,&result))return false;
      if(!setter)*out=result;
      return true;
    }
  dvm__throw(vm,"java/lang/NoSuchMethodError","%s",name);
  return false;
}
#define HTTP_PARAM_DECL(n,s) {n,s,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}
static const struct rt_method rt_http_params_decl[]= {
  HTTP_PARAM_DECL("getParameter","(Ljava/lang/String;)Ljava/lang/Object;"),HTTP_PARAM_DECL("setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;"),
  HTTP_PARAM_DECL("copy","()Lorg/apache/http/params/HttpParams;"),HTTP_PARAM_DECL("removeParameter","(Ljava/lang/String;)Z"),
  HTTP_PARAM_DECL("getLongParameter","(Ljava/lang/String;J)J"),HTTP_PARAM_DECL("setLongParameter","(Ljava/lang/String;J)Lorg/apache/http/params/HttpParams;"),
  HTTP_PARAM_DECL("getIntParameter","(Ljava/lang/String;I)I"),HTTP_PARAM_DECL("setIntParameter","(Ljava/lang/String;I)Lorg/apache/http/params/HttpParams;"),
  HTTP_PARAM_DECL("getDoubleParameter","(Ljava/lang/String;D)D"),HTTP_PARAM_DECL("setDoubleParameter","(Ljava/lang/String;D)Lorg/apache/http/params/HttpParams;"),
  HTTP_PARAM_DECL("getBooleanParameter","(Ljava/lang/String;Z)Z"),HTTP_PARAM_DECL("setBooleanParameter","(Ljava/lang/String;Z)Lorg/apache/http/params/HttpParams;"),
  HTTP_PARAM_DECL("isParameterTrue","(Ljava/lang/String;)Z"),HTTP_PARAM_DECL("isParameterFalse","(Ljava/lang/String;)Z"),M_END
};
#undef HTTP_PARAM_DECL
static const struct rt_method rt_http_abstract_params[]= {
  M("<init>","()V",nop_void),
  M("getLongParameter","(Ljava/lang/String;J)J",http_params_typed_get),M("setLongParameter","(Ljava/lang/String;J)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
  M("getIntParameter","(Ljava/lang/String;I)I",http_params_typed_get),M("setIntParameter","(Ljava/lang/String;I)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
  M("getDoubleParameter","(Ljava/lang/String;D)D",http_params_typed_get),M("setDoubleParameter","(Ljava/lang/String;D)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
  M("getBooleanParameter","(Ljava/lang/String;Z)Z",http_params_typed_get),M("setBooleanParameter","(Ljava/lang/String;Z)Lorg/apache/http/params/HttpParams;",http_params_typed_set),
  M("isParameterTrue","(Ljava/lang/String;)Z",http_params_predicate),M("isParameterFalse","(Ljava/lang/String;)Z",http_params_predicate),M_END
};
static const struct rt_method rt_http_basic_params[]= {
  M("<init>","()V",http_params_init),M("getParameter","(Ljava/lang/String;)Ljava/lang/Object;",http_params_get),
  M("setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",http_params_set),M("removeParameter","(Ljava/lang/String;)Z",http_params_remove),
  M("setParameters","([Ljava/lang/String;Ljava/lang/Object;)V",http_params_set_many),M("isParameterSet","(Ljava/lang/String;)Z",http_params_is_set),M("isParameterSetLocally","(Ljava/lang/String;)Z",http_params_is_set),
  M("clear","()V",http_params_clear),M("copy","()Lorg/apache/http/params/HttpParams;",http_params_copy),M("clone","()Ljava/lang/Object;",http_params_copy),
  M("copyParams","(Lorg/apache/http/params/HttpParams;)V",http_params_copy_into),M_END
};
static const struct rt_method rt_http_defaulted_params[]= {
  M("<init>","(Lorg/apache/http/params/HttpParams;Lorg/apache/http/params/HttpParams;)V",http_defaulted_init),M("getParameter","(Ljava/lang/String;)Ljava/lang/Object;",http_defaulted_get),
  M("setParameter","(Ljava/lang/String;Ljava/lang/Object;)Lorg/apache/http/params/HttpParams;",http_defaulted_mutate),M("removeParameter","(Ljava/lang/String;)Z",http_defaulted_mutate),
  M("getDefaults","()Lorg/apache/http/params/HttpParams;",http_defaulted_defaults),M("copy","()Lorg/apache/http/params/HttpParams;",http_defaulted_copy),M_END
};
static const struct rt_method rt_http_connection_params[]= {
  SM("getSoTimeout","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setSoTimeout","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
  SM("getTcpNoDelay","(Lorg/apache/http/params/HttpParams;)Z",http_connection_parameter),SM("setTcpNoDelay","(Lorg/apache/http/params/HttpParams;Z)V",http_connection_parameter),
  SM("getSocketBufferSize","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setSocketBufferSize","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
  SM("getLinger","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setLinger","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
  SM("getConnectionTimeout","(Lorg/apache/http/params/HttpParams;)I",http_connection_parameter),SM("setConnectionTimeout","(Lorg/apache/http/params/HttpParams;I)V",http_connection_parameter),
  SM("isStaleCheckingEnabled","(Lorg/apache/http/params/HttpParams;)Z",http_connection_parameter),SM("setStaleCheckingEnabled","(Lorg/apache/http/params/HttpParams;Z)V",http_connection_parameter),M_END
};

/* ======== merged from dvm_http_entity.h ======== */
/* MPL-2.0. Android legacy HTTP headers, name/value pairs and entity bodies.
 * Bodies use existing Java byte arrays and streams; streaming sources remain
 * streaming and their exceptions are propagated without synthetic responses.
 */
static const struct rt_field rt_http_pair_fields[]= {
  {"name","Ljava/lang/String;"},{"value","Ljava/lang/String;"},F_END
};
static const char *const rt_http_pair_iface[]= {"Lorg/apache/http/NameValuePair;","Ljava/lang/Cloneable;",NULL};
static const char *const rt_http_header_iface[]= {"Lorg/apache/http/Header;","Ljava/lang/Cloneable;",NULL};
static bool http_pair_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Name may not be null");
    return false;
  }
  dvm_set_field(vm,self,"name","Ljava/lang/String;",ARG(0));
  dvm_set_field(vm,self,"value","Ljava/lang/String;",ARG(1));
  RETV();
}
static bool http_pair_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  return dvm_get_field(vm,self,!strcmp(builtin_name(),"getName") ? "name" : "value","Ljava/lang/String;",out);
}
static bool http_pair_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  dvm_ref name=http_params_field(vm,self,"name","Ljava/lang/String;"),value=http_params_field(vm,self,"value","Ljava/lang/String;");
  bool header=object_is_a(vm,self,"org/apache/http/Header");
  size_t n=0;
  const char *text=sdata(vm,name,&n);
  struct rt_bytes buffer= {0};
  bool good=bytes_append(&buffer,text,n);
  if(header || value)good=good && bytes_append(&buffer,header ? ": " : "=",header ? 2 : 1);
  if(value) {
    text=sdata(vm,value,&n);
    good=good && bytes_append(&buffer,text,n);
  }
  if(good)out->l=dvm_new_string_n(vm,(const char *)buffer.p,buffer.len);
  free(buffer.p);
  if(!good) {
    dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP pair");
    return false;
  }
  return out->l!=0;
}
static bool http_pair_equals(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  if(self==ARG(0).l)RETI(1);
  if(!ARG(0).l || !object_is_a(vm,ARG(0).l,"org/apache/http/NameValuePair"))RETI(0);
  /* Android's BasicNameValuePair checks the interface, then casts to its own
   * concrete type; another NameValuePair implementation throws here. */
  if(!object_is_a(vm,ARG(0).l,"org/apache/http/message/BasicNameValuePair")) {
    dvm__throw(vm,"java/lang/ClassCastException","BasicNameValuePair");
    return false;
  }
  dvm_ref a=http_params_field(vm,self,"name","Ljava/lang/String;"),b=http_params_field(vm,ARG(0).l,"name","Ljava/lang/String;");
  if(!key_eq(vm,a,b))RETI(0);
  a=http_params_field(vm,self,"value","Ljava/lang/String;");
  b=http_params_field(vm,ARG(0).l,"value","Ljava/lang/String;");
  RETI(key_eq(vm,a,b));
}
static bool http_pair_hash(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  uint32_t hash=17;
  const char *fields[]= {"name","value"};
  for(unsigned i=0; i<2; ++i) {
    dvm_ref value=http_params_field(vm,self,fields[i],"Ljava/lang/String;");
    union dvm_value r= {0};
    if(value && !am_vcall(vm,value,"hashCode","()I",NULL,0,&r))return false;
    hash=hash*37u+r.u;
  }
  RETI(hash);
}
static bool http_header_elements(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  (void)out;
  dvm__throw(vm,"java/lang/UnsupportedOperationException","HTTP header element parser is not implemented");
  return false;
}
static dvm_ref http_header_new(struct dvm *vm,const char *name,dvm_ref value)
{
  dvm_ref header=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicHeader"));
  union dvm_value a[2]= {{.l=dvm_new_string(vm,name)},{.l=value}},ignored= {0};
  return header && http_pair_init(vm,header,a,2,&ignored) ? header : 0;
}
static const struct rt_field rt_http_entity_fields[]= {
  {"contentType","Lorg/apache/http/Header;"},{"contentEncoding","Lorg/apache/http/Header;"},{"chunked","Z"},F_END
};
static const struct rt_field rt_http_buffer_entity_fields[]= {{"content","[B"},F_END};
static const struct rt_field rt_http_basic_entity_fields[]= {
  {"content","Ljava/io/InputStream;"},{"contentObtained","Z"},{"length","J"},F_END
};
static const char *const rt_http_entity_iface[]= {"Lorg/apache/http/HttpEntity;",NULL};
static const char *const rt_http_buffer_entity_iface[]= {"Ljava/lang/Cloneable;",NULL};
static bool http_entity_header_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  return dvm_get_field(vm,self,!strcmp(builtin_name(),"getContentType") ? "contentType" : "contentEncoding","Lorg/apache/http/Header;",out);
}
static bool http_entity_header_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  const char *method=builtin_name();
  bool type=!strcmp(method,"setContentType");
  if(strstr(builtin_sig(),"Ljava/lang/String;")) {
    union dvm_value header= {.l=ARG(0).l ? http_header_new(vm,type ? "Content-Type" : "Content-Encoding",ARG(0).l) : 0},ignored= {0};
    if(ARG(0).l && !header.l)return false;
    return am_vcall(vm,self,method,"(Lorg/apache/http/Header;)V",&header,1,&ignored);
  }
  dvm_set_field(vm,self,type ? "contentType" : "contentEncoding","Lorg/apache/http/Header;",ARG(0));
  RETV();
}
static bool http_entity_chunked(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(nargs) {
    dvm_set_field(vm,self,"chunked","Z",ARG(0));
    RETV();
  }
  return dvm_get_field(vm,self,"chunked","Z",out);
}
static bool http_entity_basic_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  (void)out;
  dvm_set_field(vm,self,"length","J",(union dvm_value) {
    .j=-1
  });
  RETV();
}
static bool http_entity_byte_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Source byte array may not be null");
    return false;
  }
  dvm_set_field(vm,self,"content","[B",ARG(0));
  RETV();
}
static bool http_entity_string_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)out;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Source string may not be null");
    return false;
  }
  dvm_ref encoding=nargs>1 && ARG(1).l ? ARG(1).l : dvm_new_string(vm,"ISO-8859-1");
  struct jcs charset;
  if(!url_form_charset(vm,encoding,&charset))return false;
  size_t length=0,count=0;
  const char *text=sdata(vm,ARG(0).l,&length);
  uint8_t *bytes=jcs_encode(&charset,text,length,&count);
  if(!bytes || count>UINT32_MAX) {
    free(bytes);
    dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP entity encoding");
    return false;
  }
  dvm_ref array=dvm_new_array(vm,'B',"B",(uint32_t)count);
  if(array && count)memcpy(dvm_array_data(vm,array),bytes,count);
  free(bytes);
  if(!array)return false;
  dvm_set_field(vm,self,"content","[B",(union dvm_value) {
    .l=array
  });
  struct rt_bytes type= {0};
  size_t encoding_length=0;
  const char *name=sdata(vm,encoding,&encoding_length);
  bool good=bytes_append(&type,"text/plain; charset=",20) && bytes_append(&type,name,encoding_length);
  union dvm_value a= {.l=good ? dvm_new_string_n(vm,(const char *)type.p,type.len) : 0},ignored= {0};
  free(type.p);
  if(!good) {
    dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP entity type");
    return false;
  }
  return a.l && am_vcall(vm,self,"setContentType","(Ljava/lang/String;)V",&a,1,&ignored);
}
static bool http_entity_buffered(struct dvm *vm,dvm_ref self)
{
  return object_is_a(vm,self,"org/apache/http/entity/StringEntity") || object_is_a(vm,self,"org/apache/http/entity/ByteArrayEntity");
}
static bool http_entity_length(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if(http_entity_buffered(vm,self)) {
    dvm_ref array=http_params_field(vm,self,"content","[B");
    RETJ(dvm_array_length(vm,array));
  }
  return dvm_get_field(vm,self,"length","J",out);
}
static bool http_entity_set_length(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  dvm_set_field(vm,self,"length","J",ARG(0));
  RETV();
}
static bool http_entity_get_content(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if(http_entity_buffered(vm,self)) {
    union dvm_value array= {.l=http_params_field(vm,self,"content","[B")},ignored= {0};
    dvm_ref stream=dvm_new_object(vm,dvm_find_class(vm,"java/io/ByteArrayInputStream"));
    if(!stream || !am_vcall(vm,stream,"<init>","([B)V",&array,1,&ignored))return false;
    RETL(stream);
  }
  dvm_ref content=http_params_field(vm,self,"content","Ljava/io/InputStream;");
  union dvm_value obtained= {0};
  dvm_get_field(vm,self,"contentObtained","Z",&obtained);
  if(!content || obtained.i) {
    dvm__throw(vm,"java/lang/IllegalStateException",!content ? "Content has not been provided" : "Content has been consumed");
    return false;
  }
  dvm_set_field(vm,self,"contentObtained","Z",(union dvm_value) {
    .i=1
  });
  RETL(content);
}
static bool http_entity_set_content(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  dvm_set_field(vm,self,"content","Ljava/io/InputStream;",ARG(0));
  dvm_set_field(vm,self,"contentObtained","Z",(union dvm_value) {
    0
  });
  RETV();
}
static bool http_entity_repeatable(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  RETI(http_entity_buffered(vm,self));
}
static bool http_entity_streaming(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if(http_entity_buffered(vm,self))RETI(0);
  union dvm_value obtained= {0};
  dvm_get_field(vm,self,"contentObtained","Z",&obtained);
  RETI(!obtained.i && http_params_field(vm,self,"content","Ljava/io/InputStream;")!=0);
}
static bool http_entity_write(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  if(!ARG(0).l) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Output stream may not be null");
    return false;
  }
  union dvm_value ignored= {0};
  if(http_entity_buffered(vm,self)) {
    union dvm_value a= {.l=http_params_field(vm,self,"content","[B")};
    return am_vcall(vm,ARG(0).l,"write","([B)V",&a,1,&ignored) && am_vcall(vm,ARG(0).l,"flush","()V",NULL,0,&ignored);
  }
  union dvm_value source= {0};
  if(!am_vcall(vm,self,"getContent","()Ljava/io/InputStream;",NULL,0,&source))return false;
  union dvm_value buffer= {.l=dvm_new_array(vm,'B',"B",2048)};
  if(!buffer.l)return false;
  for(;;) {
    union dvm_value count= {0};
    if(!am_vcall(vm,source.l,"read","([B)I",&buffer,1,&count))return false;
    if(count.i<0)break;
    if(count.i>2048) {
      dvm__throw(vm,"java/io/IOException","Invalid stream read count");
      return false;
    }
    union dvm_value write[3]= {buffer,{.i=0},count};
    if(!am_vcall(vm,ARG(0).l,"write","([BII)V",write,3,&ignored))return false;
  }
  RETV();
}
static bool http_entity_consume(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  (void)out;
  if(object_is_a(vm,self,"org/apache/http/entity/BasicHttpEntity")) {
    dvm_ref content=http_params_field(vm,self,"content","Ljava/io/InputStream;");
    union dvm_value ignored= {0};
    if(content)return am_vcall(vm,content,"close","()V",NULL,0,&ignored);
    RETV();
  }
  union dvm_value streaming= {0};
  if(!am_vcall(vm,self,"isStreaming","()Z",NULL,0,&streaming))return false;
  if(streaming.i) {
    dvm__throw(vm,"java/lang/UnsupportedOperationException","Streaming entity does not implement consumeContent");
    return false;
  }
  RETV();
}
static bool http_form_encode_piece(struct dvm *vm,dvm_ref value,dvm_ref encoding,union dvm_value *out)
{
  union dvm_value a[2]= {{.l=value},{.l=encoding}};
  struct dvm_method *encode=dvm_lookup(vm,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  if(encode && dvm_call(vm,encode,0,a,2,out))return true;
  dvm_ref failure=dvm_exception(vm);
  if(object_is_a(vm,failure,"java/io/UnsupportedEncodingException")) {
    dvm_clear_exception(vm);
    dvm__throw(vm,"java/lang/IllegalArgumentException","Unsupported form encoding");
    dvm_set_field(vm,dvm_exception(vm),"cause","Ljava/lang/Throwable;",(union dvm_value) {
      .l=failure
    });
  }
  return false;
}
static bool http_form_format(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)self;
  (void)nargs;
  union dvm_value iterator= {0};
  if(!am_vcall(vm,ARG(0).l,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
  dvm_ref encoding=ARG(1).l ? ARG(1).l : dvm_new_string(vm,"ISO-8859-1");
  if(!encoding)return false;
  struct rt_bytes result= {0};
  bool good=true;
  for(;;) {
    union dvm_value has= {0},entry= {0},name= {0},value= {0},encoded= {0};
    if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&has)) {
      good=false;
      break;
    }
    if(!has.i)break;
    if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&entry) ||
        !am_vcall(vm,entry.l,"getName","()Ljava/lang/String;",NULL,0,&name) || !http_form_encode_piece(vm,name.l,encoding,&encoded)) {
      good=false;
      break;
    }
    size_t length=0;
    const char *text=sdata(vm,encoded.l,&length);
    if((result.len && !bytes_append(&result,"&",1)) || !bytes_append(&result,text,length) || !bytes_append(&result,"=",1)) {
      good=false;
      break;
    }
    if(!am_vcall(vm,entry.l,"getValue","()Ljava/lang/String;",NULL,0,&value)) {
      good=false;
      break;
    }
    if(value.l) {
      if(!http_form_encode_piece(vm,value.l,encoding,&encoded)) {
        good=false;
        break;
      }
      text=sdata(vm,encoded.l,&length);
      if(!bytes_append(&result,text,length)) {
        good=false;
        break;
      }
    }
  }
  if(good)out->l=dvm_new_string_n(vm,(const char *)result.p,result.len);
  free(result.p);
  if(!good && !dvm_exception(vm))dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP form");
  return good && out->l!=0;
}
static bool http_form_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  union dvm_value query[2]= {ARG(0),{.l=nargs>1 ? ARG(1).l : 0}},formatted= {0};
  if(!http_form_format(vm,0,query,2,&formatted))return false;
  query[0]=formatted;
  struct dvm_method *init=dvm_lookup(vm,"org/apache/http/entity/StringEntity","<init>","(Ljava/lang/String;Ljava/lang/String;)V");
  if(!init || !dvm_call(vm,init,self,query,2,out))return false;
  union dvm_value type= {.l=dvm_new_string(vm,"application/x-www-form-urlencoded")};
  return type.l && am_vcall(vm,self,"setContentType","(Ljava/lang/String;)V",&type,1,out);
}
#define HTTP_ENTITY_DECL(n,s) {n,s,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}
static const struct rt_method rt_http_pair_decl[]= {HTTP_ENTITY_DECL("getName","()Ljava/lang/String;"),HTTP_ENTITY_DECL("getValue","()Ljava/lang/String;"),M_END};
static const struct rt_method rt_http_header_decl[]= {HTTP_ENTITY_DECL("getName","()Ljava/lang/String;"),HTTP_ENTITY_DECL("getValue","()Ljava/lang/String;"),HTTP_ENTITY_DECL("getElements","()[Lorg/apache/http/HeaderElement;"),M_END};
static const struct rt_method rt_http_entity_decl[]= {
  HTTP_ENTITY_DECL("isRepeatable","()Z"),HTTP_ENTITY_DECL("isChunked","()Z"),HTTP_ENTITY_DECL("isStreaming","()Z"),HTTP_ENTITY_DECL("getContentLength","()J"),
  HTTP_ENTITY_DECL("getContentType","()Lorg/apache/http/Header;"),HTTP_ENTITY_DECL("getContentEncoding","()Lorg/apache/http/Header;"),HTTP_ENTITY_DECL("getContent","()Ljava/io/InputStream;"),
  HTTP_ENTITY_DECL("writeTo","(Ljava/io/OutputStream;)V"),HTTP_ENTITY_DECL("consumeContent","()V"),M_END
};
#undef HTTP_ENTITY_DECL
static const struct rt_method rt_http_pair[]= {
  M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",http_pair_init),M("getName","()Ljava/lang/String;",http_pair_get),M("getValue","()Ljava/lang/String;",http_pair_get),
  M("toString","()Ljava/lang/String;",http_pair_text),M("equals","(Ljava/lang/Object;)Z",http_pair_equals),M("hashCode","()I",http_pair_hash),M("clone","()Ljava/lang/Object;",o_clone),M_END
};
static const struct rt_method rt_http_header[]= {
  M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",http_pair_init),M("getName","()Ljava/lang/String;",http_pair_get),M("getValue","()Ljava/lang/String;",http_pair_get),
  M("toString","()Ljava/lang/String;",http_pair_text),M("getElements","()[Lorg/apache/http/HeaderElement;",http_header_elements),M("clone","()Ljava/lang/Object;",o_clone),M_END
};
static const struct rt_method rt_http_abstract_entity[]= {
  M("<init>","()V",nop_void),M("getContentType","()Lorg/apache/http/Header;",http_entity_header_get),M("getContentEncoding","()Lorg/apache/http/Header;",http_entity_header_get),
  M("setContentType","(Lorg/apache/http/Header;)V",http_entity_header_set),M("setContentType","(Ljava/lang/String;)V",http_entity_header_set),
  M("setContentEncoding","(Lorg/apache/http/Header;)V",http_entity_header_set),M("setContentEncoding","(Ljava/lang/String;)V",http_entity_header_set),
  M("isChunked","()Z",http_entity_chunked),M("setChunked","(Z)V",http_entity_chunked),M("consumeContent","()V",http_entity_consume),M_END
};
#define HTTP_ENTITY_BODY_METHODS \
 M("getContentLength","()J",http_entity_length),M("getContent","()Ljava/io/InputStream;",http_entity_get_content), \
 M("isRepeatable","()Z",http_entity_repeatable),M("isStreaming","()Z",http_entity_streaming),M("writeTo","(Ljava/io/OutputStream;)V",http_entity_write)
static const struct rt_method rt_http_string_entity[]= {
  M("<init>","(Ljava/lang/String;)V",http_entity_string_init),M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",http_entity_string_init),
  HTTP_ENTITY_BODY_METHODS,M("clone","()Ljava/lang/Object;",o_clone),M_END
};
static const struct rt_method rt_http_byte_entity[]= {M("<init>","([B)V",http_entity_byte_init),HTTP_ENTITY_BODY_METHODS,M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const struct rt_method rt_http_basic_entity[]= {
  M("<init>","()V",http_entity_basic_init),HTTP_ENTITY_BODY_METHODS,M("setContent","(Ljava/io/InputStream;)V",http_entity_set_content),
  M("setContentLength","(J)V",http_entity_set_length),M("consumeContent","()V",http_entity_consume),M_END
};
#undef HTTP_ENTITY_BODY_METHODS
static const struct rt_method rt_http_form_entity[]= {M("<init>","(Ljava/util/List;)V",http_form_init),M("<init>","(Ljava/util/List;Ljava/lang/String;)V",http_form_init),M_END};
static const struct rt_method rt_http_urlencoded_utils[]= {SM("format","(Ljava/util/List;Ljava/lang/String;)Ljava/lang/String;",http_form_format),M_END};

/* ======== merged from (original dvm_http_message.h) ======== */
/* MPL-2.0. Legacy Apache HTTP messages and URI requests. */
static const struct rt_field rt_http_version_fields[]= {
  {"protocol","Ljava/lang/String;"},{"major","I"},{"minor","I"},F_END
};
static bool http_version_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)out;
  int index=nargs==2 ? 0 : 1;
  dvm_ref protocol=nargs==2 ? dvm_new_string(vm,"HTTP") : ARG(0).l;
  if(!protocol || ARG(index).i<0 || ARG(index+1).i<0) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid protocol version");
    return false;
  }
  dvm_set_field(vm,self,"protocol","Ljava/lang/String;",(union dvm_value) {
    .l=protocol
  });
  dvm_set_field(vm,self,"major","I",ARG(index));
  dvm_set_field(vm,self,"minor","I",ARG(index+1));
  RETV();
}
static dvm_ref http_version_new(struct dvm *vm,int major,int minor)
{
  dvm_ref version=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/HttpVersion"));
  union dvm_value a[2]= {{.i=major},{.i=minor}},r= {0};
  return version && http_version_init(vm,version,a,2,&r) ? version : 0;
}
static bool http_version_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const char *name=builtin_name();
  return dvm_get_field(vm,self,!strcmp(name,"getProtocol") ? "protocol" : !strcmp(name,"getMajor") ? "major" : "minor",!strcmp(name,"getProtocol") ? "Ljava/lang/String;" : "I",out);
}
static bool http_version_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value major= {0},minor= {0};
  dvm_get_field(vm,self,"major","I",&major);
  dvm_get_field(vm,self,"minor","I",&minor);
  dvm_ref protocol=http_params_field(vm,self,"protocol","Ljava/lang/String;");
  size_t length=0;
  const char *text=sdata(vm,protocol,&length);
  struct rt_bytes result= {0};
  bool good=bytes_append(&result,text,length) && bytes_printf(&result,"/%d.%d",major.i,minor.i);
  if(good)out->l=dvm_new_string_n(vm,(char *)result.p,result.len);
  free(result.p);
  if(!good) {
    dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP protocol");
    return false;
  }
  return out->l!=0;
}
static const struct rt_field rt_http_line_fields[]= {
  {"version","Lorg/apache/http/ProtocolVersion;"},{"method","Ljava/lang/String;"},{"uri","Ljava/lang/String;"},{"statusCode","I"},{"reasonPhrase","Ljava/lang/String;"},F_END
};
static bool http_line_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  (void)out;
  bool status=object_is_a(vm,self,"org/apache/http/StatusLine");
  if((status && (!ARG(0).l || ARG(1).i<0)) || (!status && (!ARG(0).l || !ARG(1).l || !ARG(2).l))) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid HTTP line");
    return false;
  }
  if(status) {
    dvm_set_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;",ARG(0));
    dvm_set_field(vm,self,"statusCode","I",ARG(1));
    dvm_set_field(vm,self,"reasonPhrase","Ljava/lang/String;",ARG(2));
  }
  else {
    dvm_set_field(vm,self,"method","Ljava/lang/String;",ARG(0));
    dvm_set_field(vm,self,"uri","Ljava/lang/String;",ARG(1));
    dvm_set_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;",ARG(2));
  }
  RETV();
}
static bool http_line_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const char *name=builtin_name();
  if(!strcmp(name,"getProtocolVersion"))return dvm_get_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;",out);
  if(!strcmp(name,"getStatusCode"))return dvm_get_field(vm,self,"statusCode","I",out);
  return dvm_get_field(vm,self,!strcmp(name,"getMethod") ? "method" : !strcmp(name,"getUri") ? "uri" : "reasonPhrase","Ljava/lang/String;",out);
}
static bool http_line_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value r= {0};
  dvm_ref version=http_params_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;");
  if(!am_vcall(vm,version,"toString","()Ljava/lang/String;",NULL,0,&r))return false;
  struct rt_bytes result= {0};
  bool good=true;
  size_t length=0;
  const char *text=NULL;
  if(object_is_a(vm,self,"org/apache/http/StatusLine")) {
    text=sdata(vm,r.l,&length);
    union dvm_value code= {0};
    dvm_get_field(vm,self,"statusCode","I",&code);
    good=bytes_append(&result,text,length) && bytes_printf(&result," %d ",code.i);
    dvm_ref reason=http_params_field(vm,self,"reasonPhrase","Ljava/lang/String;");
    if(reason) {
      text=sdata(vm,reason,&length);
      good=good && bytes_append(&result,text,length);
    }
  } else {
    dvm_ref method=http_params_field(vm,self,"method","Ljava/lang/String;"),uri=http_params_field(vm,self,"uri","Ljava/lang/String;");
    text=sdata(vm,method,&length);
    good=bytes_append(&result,text,length) && bytes_append(&result," ",1);
    text=sdata(vm,uri,&length);
    good=good && bytes_append(&result,text,length) && bytes_append(&result," ",1);
    text=sdata(vm,r.l,&length);
    good=good && bytes_append(&result,text,length);
  }
  if(good)out->l=dvm_new_string_n(vm,(char *)result.p,result.len);
  free(result.p);
  if(!good) {
    dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP line");
    return false;
  }
  return out->l!=0;
}
static const struct rt_field rt_http_message_fields[]= {
  {"headers","Ljava/util/ArrayList;"},{"params","Lorg/apache/http/params/HttpParams;"},F_END
};
static dvm_ref http_message_headers(struct dvm *vm,dvm_ref self)
{
  dvm_ref list=http_params_field(vm,self,"headers","Ljava/util/ArrayList;");
  if(!list) {
    list=coll_new_list(vm,"Ljava/util/ArrayList;");
    if(list)dvm_set_field(vm,self,"headers","Ljava/util/ArrayList;",(union dvm_value) {
      .l=list
    });
  }
  return list;
}
static bool http_message_params(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(nargs) {
    if(!ARG(0).l) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","HTTP parameters may not be null");
      return false;
    }
    dvm_set_field(vm,self,"params","Lorg/apache/http/params/HttpParams;",ARG(0));
    RETV();
  }
  dvm_ref params=http_params_field(vm,self,"params","Lorg/apache/http/params/HttpParams;");
  if(!params) {
    params=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/params/BasicHttpParams"));
    if(params)dvm_set_field(vm,self,"params","Lorg/apache/http/params/HttpParams;",(union dvm_value) {
      .l=params
    });
  }
  RETL(params);
}
static bool http_header_matches(struct dvm *vm,dvm_ref header,dvm_ref name,bool *matches)
{
  union dvm_value actual= {0},query= {.l=name},r= {0};
  if(!am_vcall(vm,header,"getName","()Ljava/lang/String;",NULL,0,&actual) || !am_vcall(vm,actual.l,"equalsIgnoreCase","(Ljava/lang/String;)Z",&query,1,&r))return false;
  *matches=r.i!=0;
  return true;
}
static bool http_message_get_headers(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  dvm_ref list=http_message_headers(vm,self),selected=coll_new_list(vm,"Ljava/util/ArrayList;");
  if(!list || !selected)return false;
  union dvm_value iterator= {0},r= {0};
  if(!am_vcall(vm,list,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
  for(;;) {
    if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&r))return false;
    if(!r.i)break;
    if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&r))return false;
    bool match=true;
    if(nargs && !http_header_matches(vm,r.l,ARG(0).l,&match))return false;
    union dvm_value ignored= {0};
    if(match && !list_add(vm,selected,&r,1,&ignored))return false;
  }
  struct rt_list *values=list_of(vm,selected);
  const char *name=builtin_name();
  if(!strcmp(name,"containsHeader"))RETI(values && values->size!=0);
  if(!strcmp(name,"getFirstHeader") || !strcmp(name,"getLastHeader"))RETL(values && values->size ? values->items[!strcmp(name,"getFirstHeader") ? 0 : values->size-1] : 0);
  dvm_ref array=dvm_new_array(vm,'L',"Lorg/apache/http/Header;",values ? values->size : 0);
  values=list_of(vm,selected);
  if(array && values && values->size)memcpy(dvm_array_data(vm,array),values->items,values->size*sizeof(dvm_ref));
  RETL(array);
}
static bool http_message_mutate_headers(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)out;
  dvm_ref list=http_message_headers(vm,self);
  if(!list)return false;
  const char *name=builtin_name();
  union dvm_value ignored= {0};
  if(!strcmp(name,"setHeaders")) {
    if(!list_clear(vm,list,NULL,0,&ignored))return false;
    for(uint32_t i=0; ARG(0).l && i<dvm_array_length(vm,ARG(0).l); ++i) {
      union dvm_value header= {.l=((dvm_ref *)dvm_array_data(vm,ARG(0).l))[i]};
      if(!list_add(vm,list,&header,1,&ignored))return false;
    }
    RETV();
  }
  if(!strcmp(name,"removeHeader"))return am_vcall(vm,list,"remove","(Ljava/lang/Object;)Z",args,1,&ignored);
  if(!strcmp(name,"removeHeaders")) {
    if(!ARG(0).l)RETV();
    for(uint32_t i=0; list_of(vm,list) && i<list_of(vm,list)->size;) {
      bool match=false;
      dvm_ref header=list_of(vm,list)->items[i];
      if(!http_header_matches(vm,header,ARG(0).l,&match))return false;
      if(match) {
        union dvm_value index= {.i=(int32_t)i};
        if(!am_vcall(vm,list,"remove","(I)Ljava/lang/Object;",&index,1,&ignored))return false;
      }
      else ++i;
    }
    RETV();
  }
  union dvm_value header= {.l=ARG(0).l};
  if(nargs==2) {
    header.l=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicHeader"));
    if(!header.l || !http_pair_init(vm,header.l,args,2,&ignored))return false;
  }
  if(!header.l)RETV();
  if(!strcmp(name,"setHeader")) {
    union dvm_value key= {0};
    if(!am_vcall(vm,header.l,"getName","()Ljava/lang/String;",NULL,0,&key))return false;
    for(uint32_t i=0; list_of(vm,list) && i<list_of(vm,list)->size; ++i) {
      bool match=false;
      if(!http_header_matches(vm,list_of(vm,list)->items[i],key.l,&match))return false;
      if(match) {
        union dvm_value replace[2]= {{.i=(int32_t)i},header};
        return am_vcall(vm,list,"set","(ILjava/lang/Object;)Ljava/lang/Object;",replace,2,&ignored);
      }
    }
  }
  return list_add(vm,list,&header,1,&ignored);
}
static const struct rt_field rt_http_request_fields[]= {
  {"uri","Ljava/net/URI;"},{"entity","Lorg/apache/http/HttpEntity;"},{"aborted","Z"},
  {"activeClient","Lorg/apache/http/impl/client/AbstractHttpClient;"},{"activeConnection","Ljava/net/HttpURLConnection;"},F_END
};
static const struct rt_field rt_http_response_fields[]= {
  {"statusLine","Lorg/apache/http/StatusLine;"},{"entity","Lorg/apache/http/HttpEntity;"},F_END
};
static bool http_message_entity(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(nargs) {
    dvm_set_field(vm,self,"entity","Lorg/apache/http/HttpEntity;",ARG(0));
    RETV();
  }
  return dvm_get_field(vm,self,"entity","Lorg/apache/http/HttpEntity;",out);
}
static bool http_request_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)out;
  if(nargs) {
    union dvm_value uri=ARG(0);
    if(strstr(builtin_sig(),"Ljava/lang/String;")) {
      if(!juri_create(vm,0,args,1,&uri))return false;
    }
    dvm_set_field(vm,self,"uri","Ljava/net/URI;",uri);
  }
  RETV();
}
static bool http_request_uri(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(nargs) {
    dvm_set_field(vm,self,"uri","Ljava/net/URI;",ARG(0));
    RETV();
  }
  return dvm_get_field(vm,self,"uri","Ljava/net/URI;",out);
}
static bool http_request_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const struct {
    const char *type,*method;
  } methods[]= {{"HttpGet","GET"},{"HttpPost","POST"},{"HttpPut","PUT"},{"HttpDelete","DELETE"},{"HttpHead","HEAD"},{"HttpOptions","OPTIONS"},{"HttpTrace","TRACE"}};
  for(struct dvm_class *c=dvm_object_class(vm,self); c; c=c->super)for(unsigned i=0; i<sizeof methods/sizeof methods[0]; ++i) {
      const char *end=strrchr(c->name,'/');
      if(end && !strcmp(end+1,methods[i].type))RETL(dvm_new_string(vm,methods[i].method));
    }
  dvm__throw(vm,"java/lang/AbstractMethodError","getMethod");
  return false;
}
static bool http_request_version(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  union dvm_value params= {0},key= {.l=dvm_new_string(vm,"http.protocol.version")};
  if(!http_message_params(vm,self,args,0,&params) || !am_vcall(vm,params.l,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",&key,1,out))return false;
  if(!out->l)out->l=http_version_new(vm,1,1);
  if(out->l && !object_is_a(vm,out->l,"org/apache/http/ProtocolVersion")) {
    dvm__throw(vm,"java/lang/ClassCastException","ProtocolVersion");
    return false;
  }
  return out->l!=0;
}
static bool http_request_line(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value a[3]= {{0}};
  if(!am_vcall(vm,self,"getMethod","()Ljava/lang/String;",NULL,0,&a[0]) || !am_vcall(vm,self,"getURI","()Ljava/net/URI;",NULL,0,&a[1]))return false;
  if(a[1].l) {
    union dvm_value uri=a[1];
    if(!am_vcall(vm,uri.l,"toASCIIString","()Ljava/lang/String;",NULL,0,&a[1]))return false;
  }
  if(!a[1].l || !dvm_string_utf8_length(vm,a[1].l))a[1].l=dvm_new_string(vm,"/");
  if(!am_vcall(vm,self,"getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",NULL,0,&a[2]))return false;
  dvm_ref line=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicRequestLine"));
  union dvm_value ignored= {0};
  if(!line || !http_line_init(vm,line,a,3,&ignored))return false;
  RETL(line);
}
static bool http_client_abort_request(struct dvm *vm,dvm_ref request);
static bool http_request_aborted(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if(!strcmp(builtin_name(),"abort")) {
    dvm_set_field(vm,self,"aborted","Z",(union dvm_value) {
      .i=1
    });
    return http_client_abort_request(vm,self);
  }
  return dvm_get_field(vm,self,"aborted","Z",out);
}
static bool http_response_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)out;
  dvm_ref line=0;
  if(nargs==1)line=ARG(0).l;
  else {
    line=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicStatusLine"));
    union dvm_value ignored= {0};
    if(!line || !http_line_init(vm,line,args,3,&ignored))return false;
  }
  if(!line) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","Status line may not be null");
    return false;
  }
  dvm_set_field(vm,self,"statusLine","Lorg/apache/http/StatusLine;",(union dvm_value) {
    .l=line
  });
  RETV();
}
static bool http_response_status(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(nargs)return http_response_init(vm,self,args,nargs,out);
  return dvm_get_field(vm,self,"statusLine","Lorg/apache/http/StatusLine;",out);
}
static bool http_response_version(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args;
  (void)nargs;
  return am_vcall(vm,http_params_field(vm,self,"statusLine","Lorg/apache/http/StatusLine;"),"getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",NULL,0,out);
}
#define HTTP_MESSAGE_METHODS \
 M("getParams","()Lorg/apache/http/params/HttpParams;",http_message_params),M("setParams","(Lorg/apache/http/params/HttpParams;)V",http_message_params), \
 M("getAllHeaders","()[Lorg/apache/http/Header;",http_message_get_headers),M("getHeaders","(Ljava/lang/String;)[Lorg/apache/http/Header;",http_message_get_headers), \
 M("getFirstHeader","(Ljava/lang/String;)Lorg/apache/http/Header;",http_message_get_headers),M("getLastHeader","(Ljava/lang/String;)Lorg/apache/http/Header;",http_message_get_headers),M("containsHeader","(Ljava/lang/String;)Z",http_message_get_headers), \
 M("addHeader","(Lorg/apache/http/Header;)V",http_message_mutate_headers),M("addHeader","(Ljava/lang/String;Ljava/lang/String;)V",http_message_mutate_headers), \
 M("setHeader","(Lorg/apache/http/Header;)V",http_message_mutate_headers),M("setHeader","(Ljava/lang/String;Ljava/lang/String;)V",http_message_mutate_headers), \
 M("removeHeader","(Lorg/apache/http/Header;)V",http_message_mutate_headers),M("removeHeaders","(Ljava/lang/String;)V",http_message_mutate_headers),M("setHeaders","([Lorg/apache/http/Header;)V",http_message_mutate_headers)
static const struct rt_method rt_http_message[]= {M("<init>","()V",nop_void),HTTP_MESSAGE_METHODS,M_END};
#undef HTTP_MESSAGE_METHODS
static const struct rt_method rt_http_protocol_version[]= {M("<init>","(Ljava/lang/String;II)V",http_version_init),M("getProtocol","()Ljava/lang/String;",http_version_get),M("getMajor","()I",http_version_get),M("getMinor","()I",http_version_get),M("toString","()Ljava/lang/String;",http_version_text),M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const struct rt_method rt_http_version[]= {M("<init>","(II)V",http_version_init),M_END};
static const char *const rt_http_request_line_iface[]= {"Lorg/apache/http/RequestLine;","Ljava/lang/Cloneable;",NULL};
static const char *const rt_http_status_line_iface[]= {"Lorg/apache/http/StatusLine;","Ljava/lang/Cloneable;",NULL};
static const struct rt_method rt_http_request_line[]= {M("<init>","(Ljava/lang/String;Ljava/lang/String;Lorg/apache/http/ProtocolVersion;)V",http_line_init),M("getMethod","()Ljava/lang/String;",http_line_get),M("getUri","()Ljava/lang/String;",http_line_get),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_line_get),M("toString","()Ljava/lang/String;",http_line_text),M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const struct rt_method rt_http_status_line[]= {M("<init>","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V",http_line_init),M("getStatusCode","()I",http_line_get),M("getReasonPhrase","()Ljava/lang/String;",http_line_get),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_line_get),M("toString","()Ljava/lang/String;",http_line_text),M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const char *const rt_http_request_iface[]= {"Lorg/apache/http/client/methods/HttpUriRequest;",NULL};
static const char *const rt_http_enclosing_iface[]= {"Lorg/apache/http/HttpEntityEnclosingRequest;",NULL};
static const char *const rt_http_response_iface[]= {"Lorg/apache/http/HttpResponse;",NULL};
static const char *const rt_http_request_parent[]= {"Lorg/apache/http/HttpRequest;",NULL};
static const char *const rt_http_message_parent[]= {"Lorg/apache/http/HttpMessage;",NULL};
static const struct rt_method rt_http_request_base[]= {M("<init>","()V",http_request_init),M("getURI","()Ljava/net/URI;",http_request_uri),M("setURI","(Ljava/net/URI;)V",http_request_uri),M("getRequestLine","()Lorg/apache/http/RequestLine;",http_request_line),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_request_version),M("abort","()V",http_request_aborted),M("isAborted","()Z",http_request_aborted),M_END};
static const struct rt_method rt_http_request_enclosing[]= {M("<init>","()V",http_request_init),M("getEntity","()Lorg/apache/http/HttpEntity;",http_message_entity),M("setEntity","(Lorg/apache/http/HttpEntity;)V",http_message_entity),M_END};
static const struct rt_method rt_http_request_concrete[]= {M("<init>","()V",http_request_init),M("<init>","(Ljava/lang/String;)V",http_request_init),M("<init>","(Ljava/net/URI;)V",http_request_init),M("getMethod","()Ljava/lang/String;",http_request_method),M_END};
static const struct rt_method rt_http_response[]= {M("<init>","(Lorg/apache/http/StatusLine;)V",http_response_init),M("<init>","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V",http_response_init),M("getStatusLine","()Lorg/apache/http/StatusLine;",http_response_status),M("setStatusLine","(Lorg/apache/http/StatusLine;)V",http_response_status),M("setStatusLine","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V",http_response_status),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_response_version),M("getEntity","()Lorg/apache/http/HttpEntity;",http_message_entity),M("setEntity","(Lorg/apache/http/HttpEntity;)V",http_message_entity),M_END};
#define HTTP_DECL(n,s) {n,s,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}
static const struct rt_method rt_http_message_decl[]= {HTTP_DECL("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;"),HTTP_DECL("getParams","()Lorg/apache/http/params/HttpParams;"),HTTP_DECL("setParams","(Lorg/apache/http/params/HttpParams;)V"),HTTP_DECL("getAllHeaders","()[Lorg/apache/http/Header;"),HTTP_DECL("getFirstHeader","(Ljava/lang/String;)Lorg/apache/http/Header;"),HTTP_DECL("getLastHeader","(Ljava/lang/String;)Lorg/apache/http/Header;"),HTTP_DECL("getHeaders","(Ljava/lang/String;)[Lorg/apache/http/Header;"),HTTP_DECL("containsHeader","(Ljava/lang/String;)Z"),HTTP_DECL("addHeader","(Lorg/apache/http/Header;)V"),HTTP_DECL("addHeader","(Ljava/lang/String;Ljava/lang/String;)V"),HTTP_DECL("setHeader","(Lorg/apache/http/Header;)V"),HTTP_DECL("setHeader","(Ljava/lang/String;Ljava/lang/String;)V"),HTTP_DECL("removeHeader","(Lorg/apache/http/Header;)V"),HTTP_DECL("removeHeaders","(Ljava/lang/String;)V"),HTTP_DECL("setHeaders","([Lorg/apache/http/Header;)V"),M_END};
static const struct rt_method rt_http_request_decl[]= {HTTP_DECL("getRequestLine","()Lorg/apache/http/RequestLine;"),M_END};
static const struct rt_method rt_http_uri_request_decl[]= {HTTP_DECL("getMethod","()Ljava/lang/String;"),HTTP_DECL("getURI","()Ljava/net/URI;"),HTTP_DECL("abort","()V"),HTTP_DECL("isAborted","()Z"),M_END};
static const struct rt_method rt_http_response_decl[]= {HTTP_DECL("getStatusLine","()Lorg/apache/http/StatusLine;"),HTTP_DECL("setStatusLine","(Lorg/apache/http/StatusLine;)V"),HTTP_DECL("setStatusLine","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V"),HTTP_DECL("getEntity","()Lorg/apache/http/HttpEntity;"),HTTP_DECL("setEntity","(Lorg/apache/http/HttpEntity;)V"),M_END};
static const struct rt_method rt_http_enclosing_decl[]= {HTTP_DECL("getEntity","()Lorg/apache/http/HttpEntity;"),HTTP_DECL("setEntity","(Lorg/apache/http/HttpEntity;)V"),M_END};
static const struct rt_method rt_http_status_decl[]= {HTTP_DECL("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;"),HTTP_DECL("getStatusCode","()I"),HTTP_DECL("getReasonPhrase","()Ljava/lang/String;"),M_END};
static const struct rt_method rt_http_request_line_decl[]= {HTTP_DECL("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;"),HTTP_DECL("getMethod","()Ljava/lang/String;"),HTTP_DECL("getUri","()Ljava/lang/String;"),M_END};
static const struct rt_method rt_http_client_decl[]= {HTTP_DECL("getParams","()Lorg/apache/http/params/HttpParams;"),HTTP_DECL("getConnectionManager","()Lorg/apache/http/conn/ClientConnectionManager;"),HTTP_DECL("execute","(Lorg/apache/http/client/methods/HttpUriRequest;)Lorg/apache/http/HttpResponse;"),HTTP_DECL("execute","(Lorg/apache/http/client/methods/HttpUriRequest;Lorg/apache/http/protocol/HttpContext;)Lorg/apache/http/HttpResponse;"),M_END};
static const struct rt_method rt_http_manager_decl[]= {HTTP_DECL("shutdown","()V"),M_END};
#undef HTTP_DECL
