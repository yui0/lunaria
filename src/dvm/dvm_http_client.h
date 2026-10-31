/* MPL-2.0. Legacy HTTP client transport, using existing URL/TLS streams.
 * This implementation does not turn status failures into transport failures.
 * A response holds the single connection lease until EOF or stream close.
 */
static const struct rt_field rt_http_client_fields[]={
 {"params","Lorg/apache/http/params/HttpParams;"},{"manager","Lorg/apache/http/conn/ClientConnectionManager;"},
 {"connection","Ljava/net/HttpURLConnection;"},{"shutdown","Z"},{"executing","Z"},{"readers","I"},{"releasePending","Z"},
 {"socketFactory","Ljavax/net/ssl/SSLSocketFactory;"},{"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;"},F_END
};
static const struct rt_field rt_http_manager_fields[]={{"client","Lorg/apache/http/impl/client/AbstractHttpClient;"},F_END};
static const struct rt_field rt_http_managed_stream_fields[]={
 {"client","Lorg/apache/http/impl/client/AbstractHttpClient;"},{"connection","Ljava/net/HttpURLConnection;"},
 {"request","Lorg/apache/http/client/methods/HttpUriRequest;"},
 {"delegate","Ljava/io/InputStream;"},{"closed","Z"},{"eof","Z"},F_END
};
static const char *const rt_http_client_iface[]={"Lorg/apache/http/client/HttpClient;",NULL};
static const char *const rt_http_manager_iface[]={"Lorg/apache/http/conn/ClientConnectionManager;",NULL};
static bool http_client_flag(struct dvm *vm,dvm_ref self,const char *name)
{union dvm_value r={0};dvm_get_field(vm,self,name,"Z",&r);return r.i!=0;}
static bool http_client_unsupported(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)self;(void)args;(void)nargs;(void)out;dvm__throw(vm,"java/lang/UnsupportedOperationException","Legacy HTTP API not implemented: %s",builtin_name());return false;}
static bool http_client_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)out;dvm_ref params=0;
 if(nargs==2 && ARG(0).l){dvm__throw(vm,"java/lang/UnsupportedOperationException","Custom HTTP connection managers are not implemented");return false;}
 if(nargs)params=ARG(nargs-1).l;
 if(!params)params=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/params/BasicHttpParams"));
 if(!params)return false;dvm_set_field(vm,self,"params","Lorg/apache/http/params/HttpParams;",(union dvm_value){.l=params});RETV();
}
static bool http_client_params(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;dvm_ref params=http_params_field(vm,self,"params","Lorg/apache/http/params/HttpParams;");
 if(!params){union dvm_value ignored={0};if(!http_client_init(vm,self,NULL,0,&ignored))return false;params=http_params_field(vm,self,"params","Lorg/apache/http/params/HttpParams;");}RETL(params);
}
static bool http_client_manager(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;dvm_ref manager=http_params_field(vm,self,"manager","Lorg/apache/http/conn/ClientConnectionManager;");
 if(!manager){manager=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/impl/conn/SingleClientConnManager"));if(!manager)return false;
  dvm_set_field(vm,manager,"client","Lorg/apache/http/impl/client/AbstractHttpClient;",(union dvm_value){.l=self});dvm_set_field(vm,self,"manager","Lorg/apache/http/conn/ClientConnectionManager;",(union dvm_value){.l=manager});}RETL(manager);
}
static bool http_client_release(struct dvm *vm,dvm_ref client,dvm_ref connection)
{
 if(http_params_field(vm,client,"connection","Ljava/net/HttpURLConnection;")!=connection){union dvm_value ignored={0};return !connection || conn_disconnect(vm,connection,NULL,0,&ignored);}
 union dvm_value readers={0};dvm_get_field(vm,client,"readers","I",&readers);
 if(readers.i){dvm_set_field(vm,client,"releasePending","Z",(union dvm_value){.i=1});return true;}
 dvm_set_field(vm,client,"releasePending","Z",(union dvm_value){0});
 if(http_params_field(vm,client,"connection","Ljava/net/HttpURLConnection;")==connection)
  dvm_set_field(vm,client,"connection","Ljava/net/HttpURLConnection;",(union dvm_value){0});
 union dvm_value ignored={0};return !connection || conn_disconnect(vm,connection,NULL,0,&ignored);
}
static bool http_client_abort_request(struct dvm *vm,dvm_ref request)
{
 dvm_ref client=http_params_field(vm,request,"activeClient","Lorg/apache/http/impl/client/AbstractHttpClient;"),connection=http_params_field(vm,request,"activeConnection","Ljava/net/HttpURLConnection;");
 return !client || !connection || http_client_release(vm,client,connection);
}
static bool http_client_shutdown(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;(void)out;dvm_ref client=http_params_field(vm,self,"client","Lorg/apache/http/impl/client/AbstractHttpClient;");
 if(client){dvm_set_field(vm,client,"shutdown","Z",(union dvm_value){.i=1});if(!http_client_release(vm,client,http_params_field(vm,client,"connection","Ljava/net/HttpURLConnection;")))return false;}RETV();
}
static bool http_client_stream_close(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;(void)out;if(http_client_flag(vm,self,"closed"))RETV();
 dvm_set_field(vm,self,"closed","Z",(union dvm_value){.i=1});
 dvm_ref delegate=http_params_field(vm,self,"delegate","Ljava/io/InputStream;"),client=http_params_field(vm,self,"client","Lorg/apache/http/impl/client/AbstractHttpClient;"),connection=http_params_field(vm,self,"connection","Ljava/net/HttpURLConnection;");
 union dvm_value readers={0};dvm_get_field(vm,client,"readers","I",&readers);
 if(readers.i)return http_client_release(vm,client,connection);
 union dvm_value ignored={0};bool good=am_vcall(vm,delegate,"close","()V",NULL,0,&ignored);dvm_ref failure=dvm_exception(vm);if(failure)dvm_clear_exception(vm);
 bool released=http_client_release(vm,client,connection);if(failure)vm->exception=failure;return good && released;
}
static bool http_client_stream_read(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 if(http_client_flag(vm,self,"closed")){dvm__throw(vm,"java/io/IOException","Attempted read from closed HTTP stream");return false;}
 if(http_client_flag(vm,self,"eof")){
  if(!strcmp(builtin_name(),"read")){
   if(nargs){if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","buffer");return false;}
    uint32_t size=dvm_array_length(vm,ARG(0).l);int32_t offset=nargs==3 ? ARG(1).i : 0,count=nargs==3 ? ARG(2).i : (int32_t)size;
    if(offset<0 || count<0 || (uint32_t)offset>size || (uint32_t)count>size-(uint32_t)offset){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","buffer");return false;}RETI(count ? -1 : 0);}
   RETI(-1);
  }
  if(!strcmp(builtin_name(),"skip"))RETJ(0);RETI(0);
 }
 dvm_ref client=http_params_field(vm,self,"client","Lorg/apache/http/impl/client/AbstractHttpClient;");
 dvm_ref request=http_params_field(vm,self,"request","Lorg/apache/http/client/methods/HttpUriRequest;");union dvm_value aborted={0};
 if(request && !am_vcall(vm,request,"isAborted","()Z",NULL,0,&aborted))return false;
 if(aborted.i){union dvm_value ignored={0};if(!http_client_stream_close(vm,self,NULL,0,&ignored))return false;dvm__throw(vm,"java/io/InterruptedIOException","Request aborted");return false;}
 if(http_client_flag(vm,client,"shutdown")){dvm__throw(vm,"java/io/IOException","Connection manager shut down");return false;}
 dvm_ref delegate=http_params_field(vm,self,"delegate","Ljava/io/InputStream;");
 union dvm_value readers={0};dvm_get_field(vm,client,"readers","I",&readers);++readers.i;dvm_set_field(vm,client,"readers","I",readers);
 bool good=am_vcall(vm,delegate,builtin_name(),builtin_sig(),args,nargs,out);dvm_ref failure=dvm_exception(vm);
 if(good && request && !am_vcall(vm,request,"isAborted","()Z",NULL,0,&aborted)){good=false;failure=dvm_exception(vm);}
 dvm_get_field(vm,client,"readers","I",&readers);--readers.i;dvm_set_field(vm,client,"readers","I",readers);
 if(good && !strcmp(builtin_name(),"read") && out->i<0){dvm_set_field(vm,self,"eof","Z",(union dvm_value){.i=1});dvm_set_field(vm,client,"releasePending","Z",(union dvm_value){.i=1});}
 if(aborted.i)dvm_set_field(vm,client,"releasePending","Z",(union dvm_value){.i=1});
 if(!readers.i && (http_client_flag(vm,client,"shutdown") || http_client_flag(vm,client,"releasePending"))){
  if(failure)dvm_clear_exception(vm);dvm_ref connection=http_params_field(vm,self,"connection","Ljava/net/HttpURLConnection;");
  bool released=http_client_release(vm,client,connection);if(failure)vm->exception=failure;good=good && released;
 }
 if(good && http_client_flag(vm,client,"shutdown")){dvm__throw(vm,"java/io/IOException","Connection manager shut down");return false;}
 if(good && aborted.i){dvm__throw(vm,"java/io/InterruptedIOException","Request aborted");return false;}
 return good;
}
static bool http_client_setting(struct dvm *vm,dvm_ref request,dvm_ref client,const char *key,int32_t fallback,bool boolean,int32_t *value)
{
 union dvm_value params={0},local={0},query[2]={{.l=dvm_new_string(vm,key)},{.i=fallback}};
 if(!am_vcall(vm,request,"getParams","()Lorg/apache/http/params/HttpParams;",NULL,0,&params) || !am_vcall(vm,params.l,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",query,1,&local))return false;
 if(!local.l && !http_client_params(vm,client,NULL,0,&params))return false;
 union dvm_value result={0};if(!am_vcall(vm,params.l,boolean ? "getBooleanParameter" : "getIntParameter",boolean ? "(Ljava/lang/String;Z)Z" : "(Ljava/lang/String;I)I",query,2,&result))return false;
 *value=result.i;return true;
}
static bool http_client_object_setting(struct dvm *vm,dvm_ref request,dvm_ref client,const char *key,union dvm_value *out)
{
 union dvm_value params={0},query={.l=dvm_new_string(vm,key)};
 if(!am_vcall(vm,request,"getParams","()Lorg/apache/http/params/HttpParams;",NULL,0,&params) || !am_vcall(vm,params.l,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",&query,1,out))return false;
 if(!out->l){if(!http_client_params(vm,client,NULL,0,&params))return false;return am_vcall(vm,params.l,"getParameter","(Ljava/lang/String;)Ljava/lang/Object;",&query,1,out);}return true;
}
static bool http_client_same_origin(struct dvm *vm,dvm_ref a,dvm_ref b,bool *same)
{
 *same=true;const char *methods[]={"getScheme","getHost"};
 for(unsigned i=0;i<2;++i){union dvm_value x={0},y={0};if(!am_vcall(vm,a,methods[i],"()Ljava/lang/String;",NULL,0,&x) || !am_vcall(vm,b,methods[i],"()Ljava/lang/String;",NULL,0,&y))return false;
  if(!x.l || !y.l || strcasecmp(dvm_string_utf8(vm,x.l),dvm_string_utf8(vm,y.l)))*same=false;}
 union dvm_value x={0},y={0};if(!am_vcall(vm,a,"getPort","()I",NULL,0,&x) || !am_vcall(vm,b,"getPort","()I",NULL,0,&y))return false;
 dvm_ref objects[]={a,b};union dvm_value *ports[]={&x,&y};
 for(unsigned i=0;i<2;++i)if(ports[i]->i<0){union dvm_value scheme={0};if(!am_vcall(vm,objects[i],"getScheme","()Ljava/lang/String;",NULL,0,&scheme))return false;
  const char *s=scheme.l ? dvm_string_utf8(vm,scheme.l) : "";ports[i]->i=!strcasecmp(s,"https") ? 443 : !strcasecmp(s,"http") ? 80 : -1;}
 if(x.i!=y.i)*same=false;return true;
}
static bool http_client_prepare_tls(struct dvm *vm,dvm_ref self,dvm_ref connection)
{
 if(!object_is_a(vm,connection,"javax/net/ssl/HttpsURLConnection"))return true;
 dvm_ref factory=http_params_field(vm,self,"socketFactory","Ljavax/net/ssl/SSLSocketFactory;");union dvm_value r={0};
 if(!factory){
  union dvm_value protocol={.l=dvm_new_string(vm,"TLS")};struct dvm_method *get=dvm_lookup(vm,"javax/net/ssl/SSLContext","getInstance","(Ljava/lang/String;)Ljavax/net/ssl/SSLContext;");
  if(!get || !dvm_call(vm,get,0,&protocol,1,&r))return false;dvm_ref context=r.l;union dvm_value init[3]={{0}};
  if(!am_vcall(vm,context,"init","([Ljavax/net/ssl/KeyManager;[Ljavax/net/ssl/TrustManager;Ljava/security/SecureRandom;)V",init,3,&r) || !am_vcall(vm,context,"getSocketFactory","()Ljavax/net/ssl/SSLSocketFactory;",NULL,0,&r))return false;
  factory=r.l;dvm_set_field(vm,self,"socketFactory","Ljavax/net/ssl/SSLSocketFactory;",r);
 }
 dvm_ref verifier=http_params_field(vm,self,"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;");
 if(!verifier){verifier=dvm_new_object(vm,dvm_find_class(vm,"lunaria/net/HostnameVerifier"));if(!verifier)return false;dvm_set_field(vm,self,"hostnameVerifier","Ljavax/net/ssl/HostnameVerifier;",(union dvm_value){.l=verifier});}
 union dvm_value value={.l=factory};if(!am_vcall(vm,connection,"setSSLSocketFactory","(Ljavax/net/ssl/SSLSocketFactory;)V",&value,1,&r))return false;
 value.l=verifier;return am_vcall(vm,connection,"setHostnameVerifier","(Ljavax/net/ssl/HostnameVerifier;)V",&value,1,&r);
}
static bool http_client_execute_impl(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 dvm_ref request=ARG(0).l;
 if(nargs>1 && ARG(1).l){dvm__throw(vm,"java/lang/UnsupportedOperationException","Custom HTTP contexts are not implemented");return false;}
 if(!request){dvm__throw(vm,"java/lang/IllegalArgumentException","HTTP request may not be null");return false;}
 if(http_client_flag(vm,self,"shutdown")){dvm__throw(vm,"java/lang/IllegalStateException","Connection manager has been shut down");return false;}
 if(http_params_field(vm,self,"connection","Ljava/net/HttpURLConnection;")){dvm__throw(vm,"java/lang/IllegalStateException","Connection still allocated; consume the previous response");return false;}
 union dvm_value aborted={0},uri={0},method={0},entity={0},r={0};
 if(!am_vcall(vm,request,"isAborted","()Z",NULL,0,&aborted))return false;
 if(aborted.i){dvm__throw(vm,"java/io/InterruptedIOException","Request aborted");return false;}
 union dvm_value configured_version={0};
 if(!http_client_object_setting(vm,request,self,"http.protocol.version",&configured_version))return false;
 if(configured_version.l){union dvm_value protocol={0},major={0},minor={0};
  if(!object_is_a(vm,configured_version.l,"org/apache/http/ProtocolVersion")){dvm__throw(vm,"java/lang/ClassCastException","ProtocolVersion");return false;}
  if(!am_vcall(vm,configured_version.l,"getProtocol","()Ljava/lang/String;",NULL,0,&protocol) || !am_vcall(vm,configured_version.l,"getMajor","()I",NULL,0,&major) || !am_vcall(vm,configured_version.l,"getMinor","()I",NULL,0,&minor))return false;
  if(!protocol.l || strcmp(dvm_string_utf8(vm,protocol.l),"HTTP") || major.i!=1 || minor.i!=1){dvm__throw(vm,"java/lang/UnsupportedOperationException","Only HTTP/1.1 requests are implemented");return false;}
 }
 if(!am_vcall(vm,request,"getURI","()Ljava/net/URI;",NULL,0,&uri) || !uri.l){
  if(!dvm_exception(vm))dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Target host is not specified");return false;
 }
 int32_t redirecting=1,max_redirects=100,circular=0,reject_relative=0;
 if(!http_client_setting(vm,request,self,"http.protocol.handle-redirects",1,true,&redirecting) || !http_client_setting(vm,request,self,"http.protocol.max-redirects",100,false,&max_redirects) ||
    !http_client_setting(vm,request,self,"http.protocol.allow-circular-redirects",0,true,&circular) || !http_client_setting(vm,request,self,"http.protocol.reject-relative-redirect",0,true,&reject_relative))return false;
 unsigned redirects=0;bool redirected=false,strip_sensitive=false;dvm_ref locations=coll_new_list(vm,"Ljava/util/ArrayList;");if(!locations)return false;
next_hop: ;
 if(!am_vcall(vm,uri.l,"getScheme","()Ljava/lang/String;",NULL,0,&r))return false;
 const char *scheme=r.l ? dvm_string_utf8(vm,r.l) : "";
 if(strcasecmp(scheme,"http") && strcasecmp(scheme,"https")){dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Unsupported HTTP scheme");return false;}
 if(!am_vcall(vm,uri.l,"getHost","()Ljava/lang/String;",NULL,0,&r))return false;
 if(!r.l || !dvm_string_utf8_length(vm,r.l)){dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Target host is not specified");return false;}
 if(!am_vcall(vm,uri.l,"toASCIIString","()Ljava/lang/String;",NULL,0,&r))return false;
 dvm_ref url=dvm_new_object(vm,dvm_find_class(vm,"java/net/URL"));union dvm_value spec=r;
 if(!url || !url_init(vm,url,&spec,1,&r) || !url_openConnection(vm,url,NULL,0,&r))return false;dvm_ref connection=r.l;
 struct rt_conn *c=conn_of(vm,connection);if(!c){dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Not an HTTP URL");return false;}
 if(!http_client_prepare_tls(vm,self,connection))return false;
 c->independent_client=true;c->follow=false;
 if(!redirected){if(!am_vcall(vm,request,"getMethod","()Ljava/lang/String;",NULL,0,&method))return false;}else method.l=dvm_new_string(vm,"GET");
 if(!conn_setRequestMethod(vm,connection,&method,1,&r))return false;
 int32_t connect_timeout=0,read_timeout=0;
 if(!http_client_setting(vm,request,self,"http.connection.timeout",0,false,&connect_timeout) || !http_client_setting(vm,request,self,"http.socket.timeout",0,false,&read_timeout))return false;
 union dvm_value timeout={.i=connect_timeout};if(!conn_setConnectTimeout(vm,connection,&timeout,1,&r))return false;
 timeout.i=read_timeout;if(!conn_setReadTimeout(vm,connection,&timeout,1,&r))return false;
 if(!am_vcall(vm,request,"getAllHeaders","()[Lorg/apache/http/Header;",NULL,0,&r))return false;dvm_ref headers=r.l;
 for(uint32_t i=0;i<dvm_array_length(vm,headers);++i){dvm_ref header=((dvm_ref *)dvm_array_data(vm,headers))[i];union dvm_value pair[2]={{0}};
  if(!am_vcall(vm,header,"getName","()Ljava/lang/String;",NULL,0,&pair[0]) || !am_vcall(vm,header,"getValue","()Ljava/lang/String;",NULL,0,&pair[1]))return false;
  const char *name=pair[0].l ? dvm_string_utf8(vm,pair[0].l) : "";
  if(strip_sensitive && (!strcasecmp(name,"Authorization") || !strcasecmp(name,"Proxy-Authorization") || !strcasecmp(name,"Cookie")))continue;
  if(!conn_addRequestProperty(vm,connection,pair,2,&r))return false;
 }
 union dvm_value agent={0};if(!http_client_object_setting(vm,request,self,"http.useragent",&agent))return false;
 if(agent.l){
  if(!object_is_a(vm,agent.l,"java/lang/String")){dvm__throw(vm,"java/lang/ClassCastException","User-Agent string");return false;}
  bool present=false;for(int j=0;j<c->nhdrs;++j)present|=!strcasecmp(c->hdrs[j].name,"User-Agent");
  if(!present){union dvm_value pair[2]={{.l=dvm_new_string(vm,"User-Agent")},agent};if(!conn_addRequestProperty(vm,connection,pair,2,&r))return false;}
 }
 if(!redirected && object_is_a(vm,request,"org/apache/http/HttpEntityEnclosingRequest")){
  if(!am_vcall(vm,request,"getEntity","()Lorg/apache/http/HttpEntity;",NULL,0,&entity))return false;
  if(entity.l){
   const char *getters[]={"getContentType","getContentEncoding"};
   for(unsigned i=0;i<2;++i){union dvm_value header={0},pair[2]={{0}};
    if(!am_vcall(vm,entity.l,getters[i],"()Lorg/apache/http/Header;",NULL,0,&header))return false;
    if(header.l){if(!am_vcall(vm,header.l,"getName","()Ljava/lang/String;",NULL,0,&pair[0]) || !am_vcall(vm,header.l,"getValue","()Ljava/lang/String;",NULL,0,&pair[1]))return false;
     const char *name=dvm_string_utf8(vm,pair[0].l);bool present=false;for(int j=0;j<c->nhdrs;++j)present|=!strcasecmp(c->hdrs[j].name,name);
     if(!present && !conn_addRequestProperty(vm,connection,pair,2,&r))return false;}
   }
   union dvm_value enabled={.i=1};if(!conn_setDoOutput(vm,connection,&enabled,1,&r) || !conn_getOutputStream(vm,connection,NULL,0,&r))return false;
   union dvm_value output=r;if(!am_vcall(vm,entity.l,"writeTo","(Ljava/io/OutputStream;)V",&output,1,&r))return false;
  }
 }
 if(!am_vcall(vm,request,"isAborted","()Z",NULL,0,&aborted))return false;
 if(aborted.i || http_client_flag(vm,self,"shutdown")){conn_disconnect(vm,connection,NULL,0,&r);dvm__throw(vm,"java/io/InterruptedIOException","Request aborted");return false;}
 if(!conn_getResponseCode(vm,connection,NULL,0,&r))return false;
 int code=r.i;
 if(redirecting && (code==301 || code==302 || code==303 || code==307)){
  if(max_redirects<0 || redirects>=(unsigned)max_redirects){conn_disconnect(vm,connection,NULL,0,&r);dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Maximum redirects exceeded");return false;}
  const char *location=NULL;for(int i=0;i<c->resp.nheaders;++i)if(!strcasecmp(c->resp.headers[i].name,"Location")){location=c->resp.headers[i].value;break;}
  if(!location){conn_disconnect(vm,connection,NULL,0,&r);dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Redirect response has no Location");return false;}
  union dvm_value target={.l=dvm_new_string(vm,location)},child={0};
  dvm_ref candidate=dvm_new_object(vm,dvm_find_class(vm,"java/net/URI"));
  bool good=candidate && juri_init(vm,candidate,&target,1,&child);union dvm_value absolute={0};
  good=good && am_vcall(vm,candidate,"isAbsolute","()Z",NULL,0,&absolute);
  if(good && reject_relative && !absolute.i){dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Relative redirects are disabled");good=false;}
  if(good){target.l=candidate;good=am_vcall(vm,uri.l,"resolve","(Ljava/net/URI;)Ljava/net/URI;",&target,1,&child);}
  dvm_ref failure=dvm_exception(vm);if(failure)dvm_clear_exception(vm);
  conn_disconnect(vm,connection,NULL,0,&r);
  if(!good){if(failure){dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Invalid redirect URI");dvm_set_field(vm,dvm_exception(vm),"cause","Ljava/lang/Throwable;",(union dvm_value){.l=failure});}return false;}
  union dvm_value found={0};if(!am_vcall(vm,locations,"contains","(Ljava/lang/Object;)Z",&child,1,&found))return false;
  if(!circular && found.i){dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Circular redirect");return false;}
  if(!am_vcall(vm,locations,"add","(Ljava/lang/Object;)Z",&child,1,&r))return false;
  bool same=false;if(!http_client_same_origin(vm,uri.l,child.l,&same))return false;strip_sensitive|=!same;
  uri=child;redirected=true;++redirects;goto next_hop;
 }
 if(!am_vcall(vm,request,"isAborted","()Z",NULL,0,&aborted))return false;
 if(aborted.i || http_client_flag(vm,self,"shutdown")){conn_disconnect(vm,connection,NULL,0,&r);dvm__throw(vm,"java/io/InterruptedIOException","Request aborted");return false;}
 /* Preserve the actual wire protocol and reason, including nonstandard
  * reason phrases. No status-to-reason lookup is substituted. */
 int major=0,minor=0,status=0,consumed=0;
 if(!c->resp.status_line || sscanf(c->resp.status_line,"HTTP/%d.%d %d%n",&major,&minor,&status,&consumed)!=3 || major<0 || minor<0 || status<100 || status>999){conn_disconnect(vm,connection,NULL,0,&r);dvm__throw(vm,"org/apache/http/client/ClientProtocolException","Invalid HTTP status line");return false;}
 const char *reason=c->resp.status_line+consumed;while(*reason==' ')++reason;
 union dvm_value line[3]={{.l=http_version_new(vm,major,minor)},{.i=status},{.l=dvm_new_string(vm,reason)}};
 dvm_ref response=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicHttpResponse"));if(!response || !http_response_init(vm,response,line,3,&r))return false;
 for(int i=0;i<c->resp.nheaders;++i){union dvm_value pair[2]={{.l=dvm_new_string(vm,c->resp.headers[i].name)},{.l=dvm_new_string(vm,c->resp.headers[i].value)}};
  if(!am_vcall(vm,response,"addHeader","(Ljava/lang/String;Ljava/lang/String;)V",pair,2,&r))return false;}
 bool bodyless=!strcmp(dvm_string_utf8(vm,method.l),"HEAD") || status==204 || status==304 || status<200;
 if(bodyless){if(!conn_disconnect(vm,connection,NULL,0,&r))return false;RETL(response);}
 dvm_ref basic=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/entity/BasicHttpEntity"));if(!basic || !http_entity_basic_init(vm,basic,NULL,0,&r))return false;
 union dvm_value stream={0};bool good=status>=400 ? conn_getErrorStream(vm,connection,NULL,0,&stream) : conn_getInputStream(vm,connection,NULL,0,&stream);
 if(!good)return false;
 if(!stream.l){dvm_ref empty=dvm_new_array(vm,'B',"B",0);stream.l=dvm_new_object(vm,dvm_find_class(vm,"java/io/ByteArrayInputStream"));union dvm_value a={.l=empty};if(!stream.l || !am_vcall(vm,stream.l,"<init>","([B)V",&a,1,&r))return false;}
 dvm_ref managed=dvm_new_object(vm,dvm_find_class(vm,"lunaria/http/ManagedEntityStream"));if(!managed)return false;
 dvm_set_field(vm,managed,"delegate","Ljava/io/InputStream;",stream);dvm_set_field(vm,managed,"client","Lorg/apache/http/impl/client/AbstractHttpClient;",(union dvm_value){.l=self});dvm_set_field(vm,managed,"connection","Ljava/net/HttpURLConnection;",(union dvm_value){.l=connection});
 dvm_set_field(vm,managed,"request","Lorg/apache/http/client/methods/HttpUriRequest;",(union dvm_value){.l=request});
 stream.l=managed;if(!http_entity_set_content(vm,basic,&stream,1,&r))return false;
 union dvm_value length={.j=c->resp.content_length};if(!http_entity_set_length(vm,basic,&length,1,&r))return false;
 dvm_set_field(vm,basic,"chunked","Z",(union dvm_value){.i=c->resp.chunked});
 const char *names[]={"Content-Type","Content-Encoding"},*setters[]={"setContentType","setContentEncoding"};
 for(unsigned i=0;i<2;++i)for(int j=0;j<c->resp.nheaders;++j)if(!strcasecmp(c->resp.headers[j].name,names[i])){union dvm_value value={.l=dvm_new_string(vm,c->resp.headers[j].value)};if(!am_vcall(vm,basic,setters[i],"(Ljava/lang/String;)V",&value,1,&r))return false;break;}
 dvm_set_field(vm,response,"entity","Lorg/apache/http/HttpEntity;",(union dvm_value){.l=basic});
 if(!am_vcall(vm,request,"isAborted","()Z",NULL,0,&aborted))return false;
 if(aborted.i || http_client_flag(vm,self,"shutdown")){conn_disconnect(vm,connection,NULL,0,&r);dvm__throw(vm,"java/io/InterruptedIOException","Request aborted");return false;}
 dvm_set_field(vm,self,"connection","Ljava/net/HttpURLConnection;",(union dvm_value){.l=connection});
 if(object_is_a(vm,request,"org/apache/http/client/methods/HttpRequestBase")){
  dvm_set_field(vm,request,"activeClient","Lorg/apache/http/impl/client/AbstractHttpClient;",(union dvm_value){.l=self});dvm_set_field(vm,request,"activeConnection","Ljava/net/HttpURLConnection;",(union dvm_value){.l=connection});
 }
 if(c->resp.content_length==0 && !http_client_release(vm,self,connection))return false;RETL(response);
}
static bool http_client_execute(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 if(http_client_flag(vm,self,"executing")){dvm__throw(vm,"java/lang/IllegalStateException","HTTP request already executing");return false;}
 dvm_set_field(vm,self,"executing","Z",(union dvm_value){.i=1});
 bool good=http_client_execute_impl(vm,self,args,nargs,out);
 dvm_set_field(vm,self,"executing","Z",(union dvm_value){0});return good;
}
static const struct rt_method rt_http_client[]={
 M("<init>","()V",http_client_init),M("<init>","(Lorg/apache/http/params/HttpParams;)V",http_client_init),M("<init>","(Lorg/apache/http/conn/ClientConnectionManager;Lorg/apache/http/params/HttpParams;)V",http_client_init),
 M("getParams","()Lorg/apache/http/params/HttpParams;",http_client_params),M("getConnectionManager","()Lorg/apache/http/conn/ClientConnectionManager;",http_client_manager),
 M("execute","(Lorg/apache/http/client/methods/HttpUriRequest;)Lorg/apache/http/HttpResponse;",http_client_execute),M("execute","(Lorg/apache/http/client/methods/HttpUriRequest;Lorg/apache/http/protocol/HttpContext;)Lorg/apache/http/HttpResponse;",http_client_execute),
 M("getCookieStore","()Lorg/apache/http/client/CookieStore;",http_client_unsupported),M("getCredentialsProvider","()Lorg/apache/http/client/CredentialsProvider;",http_client_unsupported),M_END
};
static const struct rt_method rt_http_manager[]={M("shutdown","()V",http_client_shutdown),M_END};
static const struct rt_method rt_http_managed_stream[]={M("read","()I",http_client_stream_read),M("read","([B)I",http_client_stream_read),M("read","([BII)I",http_client_stream_read),M("available","()I",http_client_stream_read),M("skip","(J)J",http_client_stream_read),M("close","()V",http_client_stream_close),M_END};
