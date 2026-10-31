/* MPL-2.0. Android legacy HTTP headers, name/value pairs and entity bodies.
 * Bodies use existing Java byte arrays and streams; streaming sources remain
 * streaming and their exceptions are propagated without synthetic responses.
 */
static const struct rt_field rt_http_pair_fields[]={
 {"name","Ljava/lang/String;"},{"value","Ljava/lang/String;"},F_END
};
static const char *const rt_http_pair_iface[]={"Lorg/apache/http/NameValuePair;","Ljava/lang/Cloneable;",NULL};
static const char *const rt_http_header_iface[]={"Lorg/apache/http/Header;","Ljava/lang/Cloneable;",NULL};
static bool http_pair_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;
 if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","Name may not be null");return false;}
 dvm_set_field(vm,self,"name","Ljava/lang/String;",ARG(0));dvm_set_field(vm,self,"value","Ljava/lang/String;",ARG(1));RETV();
}
static bool http_pair_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;return dvm_get_field(vm,self,!strcmp(builtin_name(),"getName") ? "name" : "value","Ljava/lang/String;",out);}
static bool http_pair_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;dvm_ref name=http_params_field(vm,self,"name","Ljava/lang/String;"),value=http_params_field(vm,self,"value","Ljava/lang/String;");
 bool header=object_is_a(vm,self,"org/apache/http/Header");size_t n=0;const char *text=sdata(vm,name,&n);struct rt_bytes buffer={0};
 bool good=bytes_append(&buffer,text,n);
 if(header || value)good=good && bytes_append(&buffer,header ? ": " : "=",header ? 2 : 1);
 if(value){text=sdata(vm,value,&n);good=good && bytes_append(&buffer,text,n);}
 if(good)out->l=dvm_new_string_n(vm,(const char *)buffer.p,buffer.len);free(buffer.p);
 if(!good){dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP pair");return false;}return out->l!=0;
}
static bool http_pair_equals(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;if(self==ARG(0).l)RETI(1);
 if(!ARG(0).l || !object_is_a(vm,ARG(0).l,"org/apache/http/NameValuePair"))RETI(0);
 /* Android's BasicNameValuePair checks the interface, then casts to its own
  * concrete type; another NameValuePair implementation throws here. */
 if(!object_is_a(vm,ARG(0).l,"org/apache/http/message/BasicNameValuePair")){dvm__throw(vm,"java/lang/ClassCastException","BasicNameValuePair");return false;}
 dvm_ref a=http_params_field(vm,self,"name","Ljava/lang/String;"),b=http_params_field(vm,ARG(0).l,"name","Ljava/lang/String;");
 if(!key_eq(vm,a,b))RETI(0);
 a=http_params_field(vm,self,"value","Ljava/lang/String;");b=http_params_field(vm,ARG(0).l,"value","Ljava/lang/String;");RETI(key_eq(vm,a,b));
}
static bool http_pair_hash(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;uint32_t hash=17;
 const char *fields[]={"name","value"};
 for(unsigned i=0;i<2;++i){dvm_ref value=http_params_field(vm,self,fields[i],"Ljava/lang/String;");union dvm_value r={0};
  if(value && !am_vcall(vm,value,"hashCode","()I",NULL,0,&r))return false;hash=hash*37u+r.u;}
 RETI(hash);
}
static bool http_header_elements(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)self;(void)args;(void)nargs;(void)out;dvm__throw(vm,"java/lang/UnsupportedOperationException","HTTP header element parser is not implemented");return false;}
static dvm_ref http_header_new(struct dvm *vm,const char *name,dvm_ref value)
{
 dvm_ref header=dvm_new_object(vm,dvm_find_class(vm,"org/apache/http/message/BasicHeader"));union dvm_value a[2]={{.l=dvm_new_string(vm,name)},{.l=value}},ignored={0};
 return header && http_pair_init(vm,header,a,2,&ignored) ? header : 0;
}
static const struct rt_field rt_http_entity_fields[]={
 {"contentType","Lorg/apache/http/Header;"},{"contentEncoding","Lorg/apache/http/Header;"},{"chunked","Z"},F_END
};
static const struct rt_field rt_http_buffer_entity_fields[]={{"content","[B"},F_END};
static const struct rt_field rt_http_basic_entity_fields[]={
 {"content","Ljava/io/InputStream;"},{"contentObtained","Z"},{"length","J"},F_END
};
static const char *const rt_http_entity_iface[]={"Lorg/apache/http/HttpEntity;",NULL};
static const char *const rt_http_buffer_entity_iface[]={"Ljava/lang/Cloneable;",NULL};
static bool http_entity_header_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;return dvm_get_field(vm,self,!strcmp(builtin_name(),"getContentType") ? "contentType" : "contentEncoding","Lorg/apache/http/Header;",out);}
static bool http_entity_header_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;const char *method=builtin_name();bool type=!strcmp(method,"setContentType");
 if(strstr(builtin_sig(),"Ljava/lang/String;")){
  union dvm_value header={.l=ARG(0).l ? http_header_new(vm,type ? "Content-Type" : "Content-Encoding",ARG(0).l) : 0},ignored={0};
  if(ARG(0).l && !header.l)return false;
  return am_vcall(vm,self,method,"(Lorg/apache/http/Header;)V",&header,1,&ignored);
 }
 dvm_set_field(vm,self,type ? "contentType" : "contentEncoding","Lorg/apache/http/Header;",ARG(0));RETV();
}
static bool http_entity_chunked(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 if(nargs){dvm_set_field(vm,self,"chunked","Z",ARG(0));RETV();}
 return dvm_get_field(vm,self,"chunked","Z",out);
}
static bool http_entity_basic_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;(void)out;dvm_set_field(vm,self,"length","J",(union dvm_value){.j=-1});RETV();}
static bool http_entity_byte_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","Source byte array may not be null");return false;}
 dvm_set_field(vm,self,"content","[B",ARG(0));RETV();
}
static bool http_entity_string_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)out;if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","Source string may not be null");return false;}
 dvm_ref encoding=nargs>1 && ARG(1).l ? ARG(1).l : dvm_new_string(vm,"ISO-8859-1");struct jcs charset;
 if(!url_form_charset(vm,encoding,&charset))return false;
 size_t length=0,count=0;const char *text=sdata(vm,ARG(0).l,&length);uint8_t *bytes=jcs_encode(&charset,text,length,&count);
 if(!bytes || count>UINT32_MAX){free(bytes);dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP entity encoding");return false;}
 dvm_ref array=dvm_new_array(vm,'B',"B",(uint32_t)count);if(array && count)memcpy(dvm_array_data(vm,array),bytes,count);free(bytes);if(!array)return false;
 dvm_set_field(vm,self,"content","[B",(union dvm_value){.l=array});
 struct rt_bytes type={0};size_t encoding_length=0;const char *name=sdata(vm,encoding,&encoding_length);
 bool good=bytes_append(&type,"text/plain; charset=",20) && bytes_append(&type,name,encoding_length);
 union dvm_value a={.l=good ? dvm_new_string_n(vm,(const char *)type.p,type.len) : 0},ignored={0};free(type.p);
 if(!good){dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP entity type");return false;}
 return a.l && am_vcall(vm,self,"setContentType","(Ljava/lang/String;)V",&a,1,&ignored);
}
static bool http_entity_buffered(struct dvm *vm,dvm_ref self)
{return object_is_a(vm,self,"org/apache/http/entity/StringEntity") || object_is_a(vm,self,"org/apache/http/entity/ByteArrayEntity");}
static bool http_entity_length(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;
 if(http_entity_buffered(vm,self)){dvm_ref array=http_params_field(vm,self,"content","[B");RETJ(dvm_array_length(vm,array));}
 return dvm_get_field(vm,self,"length","J",out);
}
static bool http_entity_set_length(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;(void)out;dvm_set_field(vm,self,"length","J",ARG(0));RETV();}
static bool http_entity_get_content(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;
 if(http_entity_buffered(vm,self)){
  union dvm_value array={.l=http_params_field(vm,self,"content","[B")},ignored={0};
  dvm_ref stream=dvm_new_object(vm,dvm_find_class(vm,"java/io/ByteArrayInputStream"));
  if(!stream || !am_vcall(vm,stream,"<init>","([B)V",&array,1,&ignored))return false;RETL(stream);
 }
 dvm_ref content=http_params_field(vm,self,"content","Ljava/io/InputStream;");union dvm_value obtained={0};dvm_get_field(vm,self,"contentObtained","Z",&obtained);
 if(!content || obtained.i){dvm__throw(vm,"java/lang/IllegalStateException",!content ? "Content has not been provided" : "Content has been consumed");return false;}
 dvm_set_field(vm,self,"contentObtained","Z",(union dvm_value){.i=1});RETL(content);
}
static bool http_entity_set_content(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;(void)out;dvm_set_field(vm,self,"content","Ljava/io/InputStream;",ARG(0));dvm_set_field(vm,self,"contentObtained","Z",(union dvm_value){0});RETV();}
static bool http_entity_repeatable(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;RETI(http_entity_buffered(vm,self));}
static bool http_entity_streaming(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;if(http_entity_buffered(vm,self))RETI(0);
 union dvm_value obtained={0};dvm_get_field(vm,self,"contentObtained","Z",&obtained);
 RETI(!obtained.i && http_params_field(vm,self,"content","Ljava/io/InputStream;")!=0);
}
static bool http_entity_write(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;(void)out;
 if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","Output stream may not be null");return false;}
 union dvm_value ignored={0};
 if(http_entity_buffered(vm,self)){
  union dvm_value a={.l=http_params_field(vm,self,"content","[B")};
  return am_vcall(vm,ARG(0).l,"write","([B)V",&a,1,&ignored) && am_vcall(vm,ARG(0).l,"flush","()V",NULL,0,&ignored);
 }
 union dvm_value source={0};if(!am_vcall(vm,self,"getContent","()Ljava/io/InputStream;",NULL,0,&source))return false;
 union dvm_value buffer={.l=dvm_new_array(vm,'B',"B",2048)};if(!buffer.l)return false;
 for(;;){
  union dvm_value count={0};if(!am_vcall(vm,source.l,"read","([B)I",&buffer,1,&count))return false;
  if(count.i<0)break;
  if(count.i>2048){dvm__throw(vm,"java/io/IOException","Invalid stream read count");return false;}
  union dvm_value write[3]={buffer,{.i=0},count};
  if(!am_vcall(vm,ARG(0).l,"write","([BII)V",write,3,&ignored))return false;
 }
 RETV();
}
static bool http_entity_consume(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;(void)out;
 if(object_is_a(vm,self,"org/apache/http/entity/BasicHttpEntity")){
  dvm_ref content=http_params_field(vm,self,"content","Ljava/io/InputStream;");union dvm_value ignored={0};
  if(content)return am_vcall(vm,content,"close","()V",NULL,0,&ignored);RETV();
 }
 union dvm_value streaming={0};if(!am_vcall(vm,self,"isStreaming","()Z",NULL,0,&streaming))return false;
 if(streaming.i){dvm__throw(vm,"java/lang/UnsupportedOperationException","Streaming entity does not implement consumeContent");return false;}RETV();
}
static bool http_form_encode_piece(struct dvm *vm,dvm_ref value,dvm_ref encoding,union dvm_value *out)
{
 union dvm_value a[2]={{.l=value},{.l=encoding}};
 struct dvm_method *encode=dvm_lookup(vm,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
 if(encode && dvm_call(vm,encode,0,a,2,out))return true;
 dvm_ref failure=dvm_exception(vm);
 if(object_is_a(vm,failure,"java/io/UnsupportedEncodingException")){
  dvm_clear_exception(vm);dvm__throw(vm,"java/lang/IllegalArgumentException","Unsupported form encoding");dvm_set_field(vm,dvm_exception(vm),"cause","Ljava/lang/Throwable;",(union dvm_value){.l=failure});
 }
 return false;
}
static bool http_form_format(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)self;(void)nargs;union dvm_value iterator={0};
 if(!am_vcall(vm,ARG(0).l,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
 dvm_ref encoding=ARG(1).l ? ARG(1).l : dvm_new_string(vm,"ISO-8859-1");if(!encoding)return false;
 struct rt_bytes result={0};bool good=true;
 for(;;){
  union dvm_value has={0},entry={0},name={0},value={0},encoded={0};
  if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&has)){good=false;break;}
  if(!has.i)break;
  if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&entry) ||
     !am_vcall(vm,entry.l,"getName","()Ljava/lang/String;",NULL,0,&name) || !http_form_encode_piece(vm,name.l,encoding,&encoded)){good=false;break;}
  size_t length=0;const char *text=sdata(vm,encoded.l,&length);
  if((result.len && !bytes_append(&result,"&",1)) || !bytes_append(&result,text,length) || !bytes_append(&result,"=",1)){good=false;break;}
  if(!am_vcall(vm,entry.l,"getValue","()Ljava/lang/String;",NULL,0,&value)){good=false;break;}
  if(value.l){
   if(!http_form_encode_piece(vm,value.l,encoding,&encoded)){good=false;break;}
   text=sdata(vm,encoded.l,&length);if(!bytes_append(&result,text,length)){good=false;break;}
  }
 }
 if(good)out->l=dvm_new_string_n(vm,(const char *)result.p,result.len);free(result.p);
 if(!good && !dvm_exception(vm))dvm__throw(vm,"java/lang/OutOfMemoryError","HTTP form");return good && out->l!=0;
}
static bool http_form_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 union dvm_value query[2]={ARG(0),{.l=nargs>1 ? ARG(1).l : 0}},formatted={0};
 if(!http_form_format(vm,0,query,2,&formatted))return false;
 query[0]=formatted;struct dvm_method *init=dvm_lookup(vm,"org/apache/http/entity/StringEntity","<init>","(Ljava/lang/String;Ljava/lang/String;)V");
 if(!init || !dvm_call(vm,init,self,query,2,out))return false;
 union dvm_value type={.l=dvm_new_string(vm,"application/x-www-form-urlencoded")};
 return type.l && am_vcall(vm,self,"setContentType","(Ljava/lang/String;)V",&type,1,out);
}
#define HTTP_ENTITY_DECL(n,s) {n,s,NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}
static const struct rt_method rt_http_pair_decl[]={HTTP_ENTITY_DECL("getName","()Ljava/lang/String;"),HTTP_ENTITY_DECL("getValue","()Ljava/lang/String;"),M_END};
static const struct rt_method rt_http_header_decl[]={HTTP_ENTITY_DECL("getName","()Ljava/lang/String;"),HTTP_ENTITY_DECL("getValue","()Ljava/lang/String;"),HTTP_ENTITY_DECL("getElements","()[Lorg/apache/http/HeaderElement;"),M_END};
static const struct rt_method rt_http_entity_decl[]={
 HTTP_ENTITY_DECL("isRepeatable","()Z"),HTTP_ENTITY_DECL("isChunked","()Z"),HTTP_ENTITY_DECL("isStreaming","()Z"),HTTP_ENTITY_DECL("getContentLength","()J"),
 HTTP_ENTITY_DECL("getContentType","()Lorg/apache/http/Header;"),HTTP_ENTITY_DECL("getContentEncoding","()Lorg/apache/http/Header;"),HTTP_ENTITY_DECL("getContent","()Ljava/io/InputStream;"),
 HTTP_ENTITY_DECL("writeTo","(Ljava/io/OutputStream;)V"),HTTP_ENTITY_DECL("consumeContent","()V"),M_END
};
#undef HTTP_ENTITY_DECL
static const struct rt_method rt_http_pair[]={
 M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",http_pair_init),M("getName","()Ljava/lang/String;",http_pair_get),M("getValue","()Ljava/lang/String;",http_pair_get),
 M("toString","()Ljava/lang/String;",http_pair_text),M("equals","(Ljava/lang/Object;)Z",http_pair_equals),M("hashCode","()I",http_pair_hash),M("clone","()Ljava/lang/Object;",o_clone),M_END
};
static const struct rt_method rt_http_header[]={
 M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",http_pair_init),M("getName","()Ljava/lang/String;",http_pair_get),M("getValue","()Ljava/lang/String;",http_pair_get),
 M("toString","()Ljava/lang/String;",http_pair_text),M("getElements","()[Lorg/apache/http/HeaderElement;",http_header_elements),M("clone","()Ljava/lang/Object;",o_clone),M_END
};
static const struct rt_method rt_http_abstract_entity[]={
 M("<init>","()V",nop_void),M("getContentType","()Lorg/apache/http/Header;",http_entity_header_get),M("getContentEncoding","()Lorg/apache/http/Header;",http_entity_header_get),
 M("setContentType","(Lorg/apache/http/Header;)V",http_entity_header_set),M("setContentType","(Ljava/lang/String;)V",http_entity_header_set),
 M("setContentEncoding","(Lorg/apache/http/Header;)V",http_entity_header_set),M("setContentEncoding","(Ljava/lang/String;)V",http_entity_header_set),
 M("isChunked","()Z",http_entity_chunked),M("setChunked","(Z)V",http_entity_chunked),M("consumeContent","()V",http_entity_consume),M_END
};
#define HTTP_ENTITY_BODY_METHODS \
 M("getContentLength","()J",http_entity_length),M("getContent","()Ljava/io/InputStream;",http_entity_get_content), \
 M("isRepeatable","()Z",http_entity_repeatable),M("isStreaming","()Z",http_entity_streaming),M("writeTo","(Ljava/io/OutputStream;)V",http_entity_write)
static const struct rt_method rt_http_string_entity[]={
 M("<init>","(Ljava/lang/String;)V",http_entity_string_init),M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",http_entity_string_init),
 HTTP_ENTITY_BODY_METHODS,M("clone","()Ljava/lang/Object;",o_clone),M_END
};
static const struct rt_method rt_http_byte_entity[]={M("<init>","([B)V",http_entity_byte_init),HTTP_ENTITY_BODY_METHODS,M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const struct rt_method rt_http_basic_entity[]={
 M("<init>","()V",http_entity_basic_init),HTTP_ENTITY_BODY_METHODS,M("setContent","(Ljava/io/InputStream;)V",http_entity_set_content),
 M("setContentLength","(J)V",http_entity_set_length),M("consumeContent","()V",http_entity_consume),M_END
};
#undef HTTP_ENTITY_BODY_METHODS
static const struct rt_method rt_http_form_entity[]={M("<init>","(Ljava/util/List;)V",http_form_init),M("<init>","(Ljava/util/List;Ljava/lang/String;)V",http_form_init),M_END};
static const struct rt_method rt_http_urlencoded_utils[]={SM("format","(Ljava/util/List;Ljava/lang/String;)Ljava/lang/String;",http_form_format),M_END};
