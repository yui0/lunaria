/* MPL-2.0. Legacy Apache HTTP messages and URI requests. */
static const struct rt_field rt_http_version_fields[]={
 {"protocol","Ljava/lang/String;"},{"major","I"},{"minor","I"},F_END
};
static bool http_version_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)out;int index=nargs==2 ? 0 : 1;dvm_ref protocol=nargs==2 ? dvm_new_string(vm,"HTTP") : ARG(0).l;
 if(!protocol || ARG(index).i<0 || ARG(index+1).i<0){dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid protocol version");return false;}
 dvm_set_field(vm,self,"protocol","Ljava/lang/String;",(union dvm_value){.l=protocol});dvm_set_field(vm,self,"major","I",ARG(index));dvm_set_field(vm,self,"minor","I",ARG(index+1));RETV();
}
static dvm_ref http_version_new(struct dvm *vm,int major,int minor)
{
 dvm_ref version=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/HttpVersion"));union dvm_value a[2]={{.i=major},{.i=minor}},r={0};
 return version && http_version_init(vm,version,a,2,&r) ? version : 0;
}
static bool http_version_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;const char *name=builtin_name();
 return dvm_get_field(vm,self,!strcmp(name,"getProtocol") ? "protocol" : !strcmp(name,"getMajor") ? "major" : "minor",!strcmp(name,"getProtocol") ? "Ljava/lang/String;" : "I",out);
}
static bool http_version_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;union dvm_value major={0},minor={0};dvm_get_field(vm,self,"major","I",&major);dvm_get_field(vm,self,"minor","I",&minor);
 dvm_ref protocol=http_params_field(vm,self,"protocol","Ljava/lang/String;");size_t length=0;const char *text=sdata(vm,protocol,&length);struct rt_bytes result={0};
 bool good=bytes_append(&result,text,length) && bytes_printf(&result,"/%d.%d",major.i,minor.i);
 if(good)out->l=dvm_new_string_n(vm,(char *)result.p,result.len);free(result.p);
 if(!good){dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP protocol");return false;}return out->l!=0;
}
static const struct rt_field rt_http_line_fields[]={
 {"version","Lorg/apache/http/ProtocolVersion;"},{"method","Ljava/lang/String;"},{"uri","Ljava/lang/String;"},{"statusCode","I"},{"reasonPhrase","Ljava/lang/String;"},F_END
};
static bool http_line_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;
 bool status=object_is_a(vm,self,"org/apache/http/StatusLine");
 if((status && (!ARG(0).l || ARG(1).i<0)) || (!status && (!ARG(0).l || !ARG(1).l || !ARG(2).l))){dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid HTTP line");return false;}
 if(status){dvm_set_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;",ARG(0));dvm_set_field(vm,self,"statusCode","I",ARG(1));dvm_set_field(vm,self,"reasonPhrase","Ljava/lang/String;",ARG(2));}
 else{dvm_set_field(vm,self,"method","Ljava/lang/String;",ARG(0));dvm_set_field(vm,self,"uri","Ljava/lang/String;",ARG(1));dvm_set_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;",ARG(2));}
 RETV();
}
static bool http_line_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;const char *name=builtin_name();
 if(!strcmp(name,"getProtocolVersion"))return dvm_get_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;",out);
 if(!strcmp(name,"getStatusCode"))return dvm_get_field(vm,self,"statusCode","I",out);
 return dvm_get_field(vm,self,!strcmp(name,"getMethod") ? "method" : !strcmp(name,"getUri") ? "uri" : "reasonPhrase","Ljava/lang/String;",out);
}
static bool http_line_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;union dvm_value r={0};dvm_ref version=http_params_field(vm,self,"version","Lorg/apache/http/ProtocolVersion;");
 if(!am_vcall(vm,version,"toString","()Ljava/lang/String;",NULL,0,&r))return false;
 struct rt_bytes result={0};bool good=true;size_t length=0;const char *text=NULL;
 if(object_is_a(vm,self,"org/apache/http/StatusLine")){
  text=sdata(vm,r.l,&length);union dvm_value code={0};dvm_get_field(vm,self,"statusCode","I",&code);
  good=bytes_append(&result,text,length) && bytes_printf(&result," %d ",code.i);
  dvm_ref reason=http_params_field(vm,self,"reasonPhrase","Ljava/lang/String;");if(reason){text=sdata(vm,reason,&length);good=good && bytes_append(&result,text,length);}
 }else{
  dvm_ref method=http_params_field(vm,self,"method","Ljava/lang/String;"),uri=http_params_field(vm,self,"uri","Ljava/lang/String;");
  text=sdata(vm,method,&length);good=bytes_append(&result,text,length) && bytes_append(&result," ",1);
  text=sdata(vm,uri,&length);good=good && bytes_append(&result,text,length) && bytes_append(&result," ",1);
  text=sdata(vm,r.l,&length);good=good && bytes_append(&result,text,length);
 }
 if(good)out->l=dvm_new_string_n(vm,(char *)result.p,result.len);free(result.p);
 if(!good){dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP line");return false;}return out->l!=0;
}
static const struct rt_field rt_http_message_fields[]={
 {"headers","Ljava/util/ArrayList;"},{"params","Lorg/apache/http/params/HttpParams;"},F_END
};
static dvm_ref http_message_headers(struct dvm *vm,dvm_ref self)
{
 dvm_ref list=http_params_field(vm,self,"headers","Ljava/util/ArrayList;");
 if(!list){list=coll_new_list(vm,"Ljava/util/ArrayList;");if(list)dvm_set_field(vm,self,"headers","Ljava/util/ArrayList;",(union dvm_value){.l=list});}
 return list;
}
static bool http_message_params(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 if(nargs){if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","HTTP parameters may not be null");return false;}dvm_set_field(vm,self,"params","Lorg/apache/http/params/HttpParams;",ARG(0));RETV();}
 dvm_ref params=http_params_field(vm,self,"params","Lorg/apache/http/params/HttpParams;");
 if(!params){params=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/params/BasicHttpParams"));if(params)dvm_set_field(vm,self,"params","Lorg/apache/http/params/HttpParams;",(union dvm_value){.l=params});}
 RETL(params);
}
static bool http_header_matches(struct dvm *vm,dvm_ref header,dvm_ref name,bool *matches)
{
 union dvm_value actual={0},query={.l=name},r={0};
 if(!am_vcall(vm,header,"getName","()Ljava/lang/String;",NULL,0,&actual) || !am_vcall(vm,actual.l,"equalsIgnoreCase","(Ljava/lang/String;)Z",&query,1,&r))return false;
 *matches=r.i!=0;return true;
}
static bool http_message_get_headers(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 dvm_ref list=http_message_headers(vm,self),selected=coll_new_list(vm,"Ljava/util/ArrayList;");if(!list || !selected)return false;
 union dvm_value iterator={0},r={0};if(!am_vcall(vm,list,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
 for(;;){
  if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&r))return false;if(!r.i)break;
  if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&r))return false;bool match=true;
  if(nargs && !http_header_matches(vm,r.l,ARG(0).l,&match))return false;
  union dvm_value ignored={0};if(match && !list_add(vm,selected,&r,1,&ignored))return false;
 }
 struct rt_list *values=list_of(vm,selected);const char *name=builtin_name();
 if(!strcmp(name,"containsHeader"))RETI(values && values->size!=0);
 if(!strcmp(name,"getFirstHeader") || !strcmp(name,"getLastHeader"))RETL(values && values->size ? values->items[!strcmp(name,"getFirstHeader") ? 0 : values->size-1] : 0);
 dvm_ref array=dvm_new_array(vm,'L',"Lorg/apache/http/Header;",values ? values->size : 0);values=list_of(vm,selected);
 if(array && values && values->size)memcpy(dvm_array_data(vm,array),values->items,values->size*sizeof(dvm_ref));RETL(array);
}
static bool http_message_mutate_headers(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)out;dvm_ref list=http_message_headers(vm,self);if(!list)return false;const char *name=builtin_name();union dvm_value ignored={0};
 if(!strcmp(name,"setHeaders")){
  if(!list_clear(vm,list,NULL,0,&ignored))return false;
  for(uint32_t i=0;ARG(0).l && i<dvm_array_length(vm,ARG(0).l);++i){union dvm_value header={.l=((dvm_ref *)dvm_array_data(vm,ARG(0).l))[i]};if(!list_add(vm,list,&header,1,&ignored))return false;}RETV();
 }
 if(!strcmp(name,"removeHeader"))return am_vcall(vm,list,"remove","(Ljava/lang/Object;)Z",args,1,&ignored);
 if(!strcmp(name,"removeHeaders")){
  if(!ARG(0).l)RETV();
  for(uint32_t i=0;list_of(vm,list) && i<list_of(vm,list)->size;){bool match=false;dvm_ref header=list_of(vm,list)->items[i];if(!http_header_matches(vm,header,ARG(0).l,&match))return false;
   if(match){union dvm_value index={.i=(int32_t)i};if(!am_vcall(vm,list,"remove","(I)Ljava/lang/Object;",&index,1,&ignored))return false;}else ++i;}RETV();
 }
 union dvm_value header={.l=ARG(0).l};
 if(nargs==2){header.l=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicHeader"));if(!header.l || !http_pair_init(vm,header.l,args,2,&ignored))return false;}
 if(!header.l)RETV();
 if(!strcmp(name,"setHeader")){
  union dvm_value key={0};if(!am_vcall(vm,header.l,"getName","()Ljava/lang/String;",NULL,0,&key))return false;
  for(uint32_t i=0;list_of(vm,list) && i<list_of(vm,list)->size;++i){bool match=false;if(!http_header_matches(vm,list_of(vm,list)->items[i],key.l,&match))return false;
   if(match){union dvm_value replace[2]={{.i=(int32_t)i},header};return am_vcall(vm,list,"set","(ILjava/lang/Object;)Ljava/lang/Object;",replace,2,&ignored);}}
 }
 return list_add(vm,list,&header,1,&ignored);
}
static const struct rt_field rt_http_request_fields[]={
 {"uri","Ljava/net/URI;"},{"entity","Lorg/apache/http/HttpEntity;"},{"aborted","Z"},
 {"activeClient","Lorg/apache/http/impl/client/AbstractHttpClient;"},{"activeConnection","Ljava/net/HttpURLConnection;"},F_END
};
static const struct rt_field rt_http_response_fields[]={
 {"statusLine","Lorg/apache/http/StatusLine;"},{"entity","Lorg/apache/http/HttpEntity;"},F_END
};
static bool http_message_entity(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{if(nargs){dvm_set_field(vm,self,"entity","Lorg/apache/http/HttpEntity;",ARG(0));RETV();}return dvm_get_field(vm,self,"entity","Lorg/apache/http/HttpEntity;",out);}
static bool http_request_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)out;if(nargs){union dvm_value uri=ARG(0);
  if(strstr(builtin_sig(),"Ljava/lang/String;")){if(!juri_create(vm,0,args,1,&uri))return false;}
  dvm_set_field(vm,self,"uri","Ljava/net/URI;",uri);}
 RETV();
}
static bool http_request_uri(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{if(nargs){dvm_set_field(vm,self,"uri","Ljava/net/URI;",ARG(0));RETV();}return dvm_get_field(vm,self,"uri","Ljava/net/URI;",out);}
static bool http_request_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;
 const struct {const char *type,*method;} methods[]={{"HttpGet","GET"},{"HttpPost","POST"},{"HttpPut","PUT"},{"HttpDelete","DELETE"},{"HttpHead","HEAD"},{"HttpOptions","OPTIONS"},{"HttpTrace","TRACE"}};
 for(struct dvm_class *c=dvm_object_class(vm,self);c;c=c->super)for(unsigned i=0;i<sizeof methods/sizeof methods[0];++i){const char *end=strrchr(c->name,'/');if(end && !strcmp(end+1,methods[i].type))RETL(dvm_new_string(vm,methods[i].method));}
 dvm__throw(vm,"java/lang/AbstractMethodError","getMethod");return false;
}
static bool http_request_version(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 union dvm_value params={0},key={.l=dvm_new_string(vm,"http.protocol.version")};
 if(!http_message_params(vm,self,args,0,&params) || !am_vcall(vm,params.l,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",&key,1,out))return false;
 if(!out->l)out->l=http_version_new(vm,1,1);
 if(out->l && !object_is_a(vm,out->l,"org/apache/http/ProtocolVersion")){dvm__throw(vm,"java/lang/ClassCastException","ProtocolVersion");return false;}return out->l!=0;
}
static bool http_request_line(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;union dvm_value a[3]={{0}};
 if(!am_vcall(vm,self,"getMethod","()Ljava/lang/String;",NULL,0,&a[0]) || !am_vcall(vm,self,"getURI","()Ljava/net/URI;",NULL,0,&a[1]))return false;
 if(a[1].l){union dvm_value uri=a[1];if(!am_vcall(vm,uri.l,"toASCIIString","()Ljava/lang/String;",NULL,0,&a[1]))return false;}
 if(!a[1].l || !dvm_string_utf8_length(vm,a[1].l))a[1].l=dvm_new_string(vm,"/");
 if(!am_vcall(vm,self,"getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",NULL,0,&a[2]))return false;
 dvm_ref line=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicRequestLine"));union dvm_value ignored={0};
 if(!line || !http_line_init(vm,line,a,3,&ignored))return false;RETL(line);
}
static bool http_client_abort_request(struct dvm *vm,dvm_ref request);
static bool http_request_aborted(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;
 if(!strcmp(builtin_name(),"abort")){dvm_set_field(vm,self,"aborted","Z",(union dvm_value){.i=1});return http_client_abort_request(vm,self);}
 return dvm_get_field(vm,self,"aborted","Z",out);
}
static bool http_response_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)out;dvm_ref line=0;
 if(nargs==1)line=ARG(0).l;
 else{line=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicStatusLine"));union dvm_value ignored={0};if(!line || !http_line_init(vm,line,args,3,&ignored))return false;}
 if(!line){dvm__throw(vm,"java/lang/IllegalArgumentException","Status line may not be null");return false;}
 dvm_set_field(vm,self,"statusLine","Lorg/apache/http/StatusLine;",(union dvm_value){.l=line});RETV();
}
static bool http_response_status(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 if(nargs)return http_response_init(vm,self,args,nargs,out);
 return dvm_get_field(vm,self,"statusLine","Lorg/apache/http/StatusLine;",out);
}
static bool http_response_version(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;return am_vcall(vm,http_params_field(vm,self,"statusLine","Lorg/apache/http/StatusLine;"),"getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",NULL,0,out);}
#define HTTP_MESSAGE_METHODS \
 M("getParams","()Lorg/apache/http/params/HttpParams;",http_message_params),M("setParams","(Lorg/apache/http/params/HttpParams;)V",http_message_params), \
 M("getAllHeaders","()[Lorg/apache/http/Header;",http_message_get_headers),M("getHeaders","(Ljava/lang/String;)[Lorg/apache/http/Header;",http_message_get_headers), \
 M("getFirstHeader","(Ljava/lang/String;)Lorg/apache/http/Header;",http_message_get_headers),M("getLastHeader","(Ljava/lang/String;)Lorg/apache/http/Header;",http_message_get_headers),M("containsHeader","(Ljava/lang/String;)Z",http_message_get_headers), \
 M("addHeader","(Lorg/apache/http/Header;)V",http_message_mutate_headers),M("addHeader","(Ljava/lang/String;Ljava/lang/String;)V",http_message_mutate_headers), \
 M("setHeader","(Lorg/apache/http/Header;)V",http_message_mutate_headers),M("setHeader","(Ljava/lang/String;Ljava/lang/String;)V",http_message_mutate_headers), \
 M("removeHeader","(Lorg/apache/http/Header;)V",http_message_mutate_headers),M("removeHeaders","(Ljava/lang/String;)V",http_message_mutate_headers),M("setHeaders","([Lorg/apache/http/Header;)V",http_message_mutate_headers)
static const struct rt_method rt_http_message[]={M("<init>","()V",nop_void),HTTP_MESSAGE_METHODS,M_END};
#undef HTTP_MESSAGE_METHODS
static const struct rt_method rt_http_protocol_version[]={M("<init>","(Ljava/lang/String;II)V",http_version_init),M("getProtocol","()Ljava/lang/String;",http_version_get),M("getMajor","()I",http_version_get),M("getMinor","()I",http_version_get),M("toString","()Ljava/lang/String;",http_version_text),M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const struct rt_method rt_http_version[]={M("<init>","(II)V",http_version_init),M_END};
static const char *const rt_http_request_line_iface[]={"Lorg/apache/http/RequestLine;","Ljava/lang/Cloneable;",NULL};
static const char *const rt_http_status_line_iface[]={"Lorg/apache/http/StatusLine;","Ljava/lang/Cloneable;",NULL};
static const struct rt_method rt_http_request_line[]={M("<init>","(Ljava/lang/String;Ljava/lang/String;Lorg/apache/http/ProtocolVersion;)V",http_line_init),M("getMethod","()Ljava/lang/String;",http_line_get),M("getUri","()Ljava/lang/String;",http_line_get),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_line_get),M("toString","()Ljava/lang/String;",http_line_text),M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const struct rt_method rt_http_status_line[]={M("<init>","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V",http_line_init),M("getStatusCode","()I",http_line_get),M("getReasonPhrase","()Ljava/lang/String;",http_line_get),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_line_get),M("toString","()Ljava/lang/String;",http_line_text),M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const char *const rt_http_request_iface[]={"Lorg/apache/http/client/methods/HttpUriRequest;",NULL};
static const char *const rt_http_enclosing_iface[]={"Lorg/apache/http/HttpEntityEnclosingRequest;",NULL};
static const char *const rt_http_response_iface[]={"Lorg/apache/http/HttpResponse;",NULL};
static const char *const rt_http_request_parent[]={"Lorg/apache/http/HttpRequest;",NULL};
static const char *const rt_http_message_parent[]={"Lorg/apache/http/HttpMessage;",NULL};
static const struct rt_method rt_http_request_base[]={M("<init>","()V",http_request_init),M("getURI","()Ljava/net/URI;",http_request_uri),M("setURI","(Ljava/net/URI;)V",http_request_uri),M("getRequestLine","()Lorg/apache/http/RequestLine;",http_request_line),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_request_version),M("abort","()V",http_request_aborted),M("isAborted","()Z",http_request_aborted),M_END};
static const struct rt_method rt_http_request_enclosing[]={M("<init>","()V",http_request_init),M("getEntity","()Lorg/apache/http/HttpEntity;",http_message_entity),M("setEntity","(Lorg/apache/http/HttpEntity;)V",http_message_entity),M_END};
static const struct rt_method rt_http_request_concrete[]={M("<init>","()V",http_request_init),M("<init>","(Ljava/lang/String;)V",http_request_init),M("<init>","(Ljava/net/URI;)V",http_request_init),M("getMethod","()Ljava/lang/String;",http_request_method),M_END};
static const struct rt_method rt_http_response[]={M("<init>","(Lorg/apache/http/StatusLine;)V",http_response_init),M("<init>","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V",http_response_init),M("getStatusLine","()Lorg/apache/http/StatusLine;",http_response_status),M("setStatusLine","(Lorg/apache/http/StatusLine;)V",http_response_status),M("setStatusLine","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V",http_response_status),M("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;",http_response_version),M("getEntity","()Lorg/apache/http/HttpEntity;",http_message_entity),M("setEntity","(Lorg/apache/http/HttpEntity;)V",http_message_entity),M_END};
#define HTTP_DECL(n,s) {n,s,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}
static const struct rt_method rt_http_message_decl[]={HTTP_DECL("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;"),HTTP_DECL("getParams","()Lorg/apache/http/params/HttpParams;"),HTTP_DECL("setParams","(Lorg/apache/http/params/HttpParams;)V"),HTTP_DECL("getAllHeaders","()[Lorg/apache/http/Header;"),HTTP_DECL("getFirstHeader","(Ljava/lang/String;)Lorg/apache/http/Header;"),HTTP_DECL("getLastHeader","(Ljava/lang/String;)Lorg/apache/http/Header;"),HTTP_DECL("getHeaders","(Ljava/lang/String;)[Lorg/apache/http/Header;"),HTTP_DECL("containsHeader","(Ljava/lang/String;)Z"),HTTP_DECL("addHeader","(Lorg/apache/http/Header;)V"),HTTP_DECL("addHeader","(Ljava/lang/String;Ljava/lang/String;)V"),HTTP_DECL("setHeader","(Lorg/apache/http/Header;)V"),HTTP_DECL("setHeader","(Ljava/lang/String;Ljava/lang/String;)V"),HTTP_DECL("removeHeader","(Lorg/apache/http/Header;)V"),HTTP_DECL("removeHeaders","(Ljava/lang/String;)V"),HTTP_DECL("setHeaders","([Lorg/apache/http/Header;)V"),M_END};
static const struct rt_method rt_http_request_decl[]={HTTP_DECL("getRequestLine","()Lorg/apache/http/RequestLine;"),M_END};
static const struct rt_method rt_http_uri_request_decl[]={HTTP_DECL("getMethod","()Ljava/lang/String;"),HTTP_DECL("getURI","()Ljava/net/URI;"),HTTP_DECL("abort","()V"),HTTP_DECL("isAborted","()Z"),M_END};
static const struct rt_method rt_http_response_decl[]={HTTP_DECL("getStatusLine","()Lorg/apache/http/StatusLine;"),HTTP_DECL("setStatusLine","(Lorg/apache/http/StatusLine;)V"),HTTP_DECL("setStatusLine","(Lorg/apache/http/ProtocolVersion;ILjava/lang/String;)V"),HTTP_DECL("getEntity","()Lorg/apache/http/HttpEntity;"),HTTP_DECL("setEntity","(Lorg/apache/http/HttpEntity;)V"),M_END};
static const struct rt_method rt_http_enclosing_decl[]={HTTP_DECL("getEntity","()Lorg/apache/http/HttpEntity;"),HTTP_DECL("setEntity","(Lorg/apache/http/HttpEntity;)V"),M_END};
static const struct rt_method rt_http_status_decl[]={HTTP_DECL("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;"),HTTP_DECL("getStatusCode","()I"),HTTP_DECL("getReasonPhrase","()Ljava/lang/String;"),M_END};
static const struct rt_method rt_http_request_line_decl[]={HTTP_DECL("getProtocolVersion","()Lorg/apache/http/ProtocolVersion;"),HTTP_DECL("getMethod","()Ljava/lang/String;"),HTTP_DECL("getUri","()Ljava/lang/String;"),M_END};
static const struct rt_method rt_http_client_decl[]={HTTP_DECL("getParams","()Lorg/apache/http/params/HttpParams;"),HTTP_DECL("getConnectionManager","()Lorg/apache/http/conn/ClientConnectionManager;"),HTTP_DECL("execute","(Lorg/apache/http/client/methods/HttpUriRequest;)Lorg/apache/http/HttpResponse;"),HTTP_DECL("execute","(Lorg/apache/http/client/methods/HttpUriRequest;Lorg/apache/http/protocol/HttpContext;)Lorg/apache/http/HttpResponse;"),M_END};
static const struct rt_method rt_http_manager_decl[]={HTTP_DECL("shutdown","()V"),M_END};
#undef HTTP_DECL
