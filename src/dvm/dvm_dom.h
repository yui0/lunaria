/* MPL-2.0. XMLReader-backed DOM parsing; Java references own the resulting tree. */
typedef const unsigned char *(*dom_xml_text_fn)(void*);
struct dom_xml_api {
   void *handle;
   void *(*create)(const char*,int,const char*,const char*,int);
   void (*destroy)(void*);
   int (*read)(void*),(*type)(void*),(*depth)(void*),(*empty)(void*),(*first_attr)(void*),(*next_attr)(void*),(*element)(void*),(*line)(void*),(*column)(void*);
   dom_xml_text_fn name,value,local,uri,prefix;
};
static struct dom_xml_api dom_xml;
static pthread_once_t dom_xml_once=PTHREAD_ONCE_INIT;
static void dom_xml_load(void)
{
   const char *names[]={"libxml2.2.dylib","libxml2.so.2","libxml2-2.dll"};
   for(size_t i=0;i<sizeof names/sizeof names[0] && !dom_xml.handle;++i)dom_xml.handle=luna_os_library_open_local(names[i]);
   if(!dom_xml.handle)return;
#define DOM_SYMBOL(field,symbol) *(void**)&dom_xml.field=luna_os_library_symbol(dom_xml.handle,symbol);if(!dom_xml.field)return
   DOM_SYMBOL(create,"xmlReaderForMemory");DOM_SYMBOL(destroy,"xmlFreeTextReader");DOM_SYMBOL(read,"xmlTextReaderRead");
   DOM_SYMBOL(type,"xmlTextReaderNodeType");DOM_SYMBOL(depth,"xmlTextReaderDepth");DOM_SYMBOL(empty,"xmlTextReaderIsEmptyElement");
   DOM_SYMBOL(first_attr,"xmlTextReaderMoveToFirstAttribute");DOM_SYMBOL(next_attr,"xmlTextReaderMoveToNextAttribute");DOM_SYMBOL(element,"xmlTextReaderMoveToElement");
   DOM_SYMBOL(name,"xmlTextReaderConstName");DOM_SYMBOL(value,"xmlTextReaderConstValue");DOM_SYMBOL(local,"xmlTextReaderConstLocalName");
   DOM_SYMBOL(uri,"xmlTextReaderConstNamespaceUri");DOM_SYMBOL(prefix,"xmlTextReaderConstPrefix");
   DOM_SYMBOL(line,"xmlTextReaderGetParserLineNumber");DOM_SYMBOL(column,"xmlTextReaderGetParserColumnNumber");
#undef DOM_SYMBOL
}
static const struct rt_field rt_dom_source_fields[]={
   {"characterStream","Ljava/io/Reader;"},{"byteStream","Ljava/io/InputStream;"},
   {"systemId","Ljava/lang/String;"},{"encoding","Ljava/lang/String;"},{"publicId","Ljava/lang/String;"},F_END
};
static const struct rt_field rt_dom_factory_fields[]={{"namespaceAware","Z"},{"validating","Z"},F_END};
static const struct rt_field rt_dom_node_fields[]={
   {"nodeType","I"},{"nodeName","Ljava/lang/String;"},{"nodeValue","Ljava/lang/String;"},
   {"namespaceURI","Ljava/lang/String;"},{"localName","Ljava/lang/String;"},{"prefix","Ljava/lang/String;"},
   {"parent","Lorg/w3c/dom/Node;"},{"ownerDocument","Lorg/w3c/dom/Document;"},
   {"children","Ljava/util/List;"},{"attributes","Ljava/util/List;"},F_END
};
static const struct rt_field rt_dom_list_fields[]={
   {"root","Lorg/w3c/dom/Node;"},{"query","Ljava/lang/String;"},{"attributes","Z"},F_END
};
static dvm_ref dom_ref(struct dvm *vm,dvm_ref self,const char *field,const char *type)
{union dvm_value v={0};(void)dvm_get_field(vm,self,field,type,&v);return v.l;}
static int dom_int(struct dvm *vm,dvm_ref self,const char *field,const char *type)
{union dvm_value v={0};(void)dvm_get_field(vm,self,field,type,&v);return v.i;}
static void dom_string(struct dvm *vm,dvm_ref self,const char *field,const unsigned char *text)
{(void)dvm_set_field(vm,self,field,"Ljava/lang/String;",(union dvm_value){.l=text ? dvm_new_string(vm,(const char*)text) : 0});}
static bool dom_source(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=builtin_name(),*field=NULL,*type=NULL;
   if(!strcmp(name,"<init>")) {
      if(!nargs)RETV();const char *sig=builtin_sig();
      field=strstr(sig,"Reader;") ? "characterStream" : strstr(sig,"InputStream;") ? "byteStream" : "systemId";
   } else if(strstr(name,"CharacterStream"))field="characterStream";
   else if(strstr(name,"ByteStream"))field="byteStream";
   else if(strstr(name,"SystemId"))field="systemId";
   else if(strstr(name,"PublicId"))field="publicId";
   else field="encoding";
   type=!strcmp(field,"characterStream") ? "Ljava/io/Reader;" : !strcmp(field,"byteStream") ? "Ljava/io/InputStream;" : "Ljava/lang/String;";
   if(nargs){(void)dvm_set_field(vm,self,field,type,ARG(0));RETV();}RETL(dom_ref(vm,self,field,type));
}
static bool dom_factory_new(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)self;(void)args;(void)nargs;RETL(dvm_new_object(vm,dvm_find_class(vm,"lunaria/xml/DocumentBuilderFactory")));}
static bool dom_factory_option(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *field=strstr(builtin_name(),"NamespaceAware") ? "namespaceAware" : "validating";
   if(nargs){(void)dvm_set_field(vm,self,field,"Z",ARG(0));RETV();}RETI(dom_int(vm,self,field,"Z"));
}
static bool dom_factory_builder(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;pthread_once(&dom_xml_once,dom_xml_load);
   if(!dom_xml.prefix || dom_int(vm,self,"validating","Z")) {
      dvm__throw(vm,"javax/xml/parsers/ParserConfigurationException",dom_xml.prefix ? "Validating DOM is not available" : "libxml2 XMLReader is unavailable");return false;
   }
   dvm_ref builder=dvm_new_object(vm,dvm_find_class(vm,"lunaria/xml/DocumentBuilder"));
   (void)dvm_set_field(vm,builder,"namespaceAware","Z",(union dvm_value){.i=dom_int(vm,self,"namespaceAware","Z")});RETL(builder);
}
static dvm_ref dom_new_node(struct dvm *vm,int type,const unsigned char *name,const unsigned char *value,dvm_ref document)
{
   const char *cls=type==9 ? "lunaria/xml/Document" : type==1 ? "lunaria/xml/Element" : type==2 ? "lunaria/xml/Attr" :
      type==3 ? "lunaria/xml/Text" : type==4 ? "lunaria/xml/CDATA" : type==8 ? "lunaria/xml/Comment" : "lunaria/xml/Node";
   dvm_ref node=dvm_new_object(vm,dvm_find_class(vm,cls));if(!node)return 0;
   (void)dvm_set_field(vm,node,"nodeType","I",(union dvm_value){.i=type});
   dom_string(vm,node,"nodeName",name);dom_string(vm,node,"nodeValue",value);
   (void)dvm_set_field(vm,node,"ownerDocument","Lorg/w3c/dom/Document;",(union dvm_value){.l=document});
   dvm_ref children=coll_new_list(vm,"Ljava/util/ArrayList;"),attrs=coll_new_list(vm,"Ljava/util/ArrayList;");
   (void)dvm_set_field(vm,node,"children","Ljava/util/List;",(union dvm_value){.l=children});
   (void)dvm_set_field(vm,node,"attributes","Ljava/util/List;",(union dvm_value){.l=attrs});return node;
}
static bool dom_read_source(struct dvm *vm,dvm_ref source,struct rt_bytes *bytes,bool *characters)
{
   dvm_ref stream=dom_ref(vm,source,"characterStream","Ljava/io/Reader;");*characters=stream!=0;
   if(!stream)stream=dom_ref(vm,source,"byteStream","Ljava/io/InputStream;");
   if(!stream){dvm__throw(vm,"org/xml/sax/SAXException","InputSource requires a byte or character stream");return false;}
   dvm_ref buffer=dvm_new_array(vm,*characters ? 'C' : 'B',NULL,4096);if(!buffer)return false;dvm_pin(vm,buffer);
   bool good=true;
   for(;;) {
      union dvm_value a[3]={{.l=buffer},{0},{.i=4096}},r={0};
      if(!am_vcall(vm,stream,"read",*characters ? "([CII)I" : "([BII)I",a,3,&r)){good=false;break;}
      if(r.i<0)break;
      if(r.i>4096 || !r.i){dvm__throw(vm,"java/io/IOException","Invalid stream read count");good=false;break;}
      if(!bytes_append(bytes,dvm_array_data(vm,buffer),(size_t)r.i*(*characters ? 2 : 1))){dvm__throw(vm,"java/lang/OutOfMemoryError","XML input");good=false;break;}
   }
   dvm_unpin(vm,buffer);
   if(good && *characters) {
      size_t units=bytes->len/2;
      if(units>(SIZE_MAX-1)/3){dvm__throw(vm,"java/lang/OutOfMemoryError","XML characters");return false;}
      char *utf8=malloc(units*3+1);if(!utf8){dvm__throw(vm,"java/lang/OutOfMemoryError","XML characters");return false;}
      size_t length=jcs_utf16_to_wtf8((const uint16_t*)bytes->p,units,utf8);free(bytes->p);bytes->p=(uint8_t*)utf8;bytes->len=length;bytes->cap=units*3+1;
   }
   return good;
}
static bool dom_parse(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;dvm_ref source=ARG(0).l;
   if(!source){dvm__throw(vm,"java/lang/IllegalArgumentException","InputSource is null");return false;}
   struct rt_bytes bytes={0};bool characters=false;
   if(!dom_read_source(vm,source,&bytes,&characters)){free(bytes.p);return false;}
   if(bytes.len>INT_MAX){free(bytes.p);dvm__throw(vm,"org/xml/sax/SAXException","XML input too large");return false;}
   dvm_ref encoding=dom_ref(vm,source,"encoding","Ljava/lang/String;");
   void *reader=dom_xml.create((const char*)bytes.p,(int)bytes.len,NULL,characters ? "UTF-8" : encoding ? dvm_string_utf8(vm,encoding) : NULL,2048);
   if(!reader){free(bytes.p);dvm__throw(vm,"org/xml/sax/SAXParseException","Cannot create XML reader");return false;}
   dvm_ref document=dom_new_node(vm,9,(const unsigned char*)"#document",NULL,0);if(document)dvm_pin(vm,document);
   dvm_ref *stack=NULL;size_t cap=0;int result=0;bool good=document!=0,aware=dom_int(vm,self,"namespaceAware","Z")!=0;
   while(good && (result=dom_xml.read(reader))==1) {
      int type=dom_xml.type(reader),depth=dom_xml.depth(reader);
      if(type==15 || type==17)continue;
      if(type==5 || type==6 || type==10){dvm__throw(vm,"org/xml/sax/SAXException","Entity and DTD DOM nodes are not implemented");good=false;break;}
      int node_type=type==1 ? 1 : type==3 || type==13 || type==14 ? 3 : type==4 ? 4 : type==8 ? 8 : type==7 ? 7 : 0;
      if(!node_type)continue;
      if(depth<0){good=false;break;}
      if((size_t)depth>=cap){size_t next=(size_t)depth+16;dvm_ref *p=realloc(stack,next*sizeof *p);if(!p){dvm__throw(vm,"java/lang/OutOfMemoryError","XML depth");good=false;break;}memset(p+cap,0,(next-cap)*sizeof *p);stack=p;cap=next;}
      const unsigned char *name=node_type==3 ? (const unsigned char*)"#text" : node_type==4 ? (const unsigned char*)"#cdata-section" : node_type==8 ? (const unsigned char*)"#comment" : dom_xml.name(reader);
      dvm_ref node=dom_new_node(vm,node_type,name,node_type==1 ? NULL : dom_xml.value(reader),document);
      if(!node){good=false;break;}
      dvm_ref parent=depth ? stack[depth-1] : document;
      (void)dvm_set_field(vm,node,"parent","Lorg/w3c/dom/Node;",(union dvm_value){.l=parent});
      union dvm_value child={.l=node},ignored={0};
      good=list_add(vm,dom_ref(vm,parent,"children","Ljava/util/List;"),&child,1,&ignored);
      if(node_type==1) {
         stack[depth]=node;
         if(aware){dom_string(vm,node,"localName",dom_xml.local(reader));dom_string(vm,node,"namespaceURI",dom_xml.uri(reader));dom_string(vm,node,"prefix",dom_xml.prefix(reader));}
         int attr=dom_xml.first_attr(reader);
         while(good && attr==1) {
            dvm_ref attribute=dom_new_node(vm,2,dom_xml.name(reader),dom_xml.value(reader),document);if(!attribute){good=false;break;}
            if(aware){dom_string(vm,attribute,"localName",dom_xml.local(reader));dom_string(vm,attribute,"namespaceURI",dom_xml.uri(reader));dom_string(vm,attribute,"prefix",dom_xml.prefix(reader));}
            child.l=attribute;good=list_add(vm,dom_ref(vm,node,"attributes","Ljava/util/List;"),&child,1,&ignored);attr=dom_xml.next_attr(reader);
         }
         (void)dom_xml.element(reader);
      }
   }
   if(good && result<0){dvm__throw(vm,"org/xml/sax/SAXParseException","Malformed XML document");good=false;}
   if(good && !document){good=false;}
   dom_xml.destroy(reader);free(bytes.p);free(stack);if(document)dvm_unpin(vm,document);
   if(!good)return false;RETL(document);
}
static bool dom_node_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"getNodeType"))RETI(dom_int(vm,self,"nodeType","I"));
   const char *field=!strcmp(name,"getNodeName") || !strcmp(name,"getTagName") || !strcmp(name,"getName") ? "nodeName" :
      !strcmp(name,"getNamespaceURI") ? "namespaceURI" : !strcmp(name,"getLocalName") ? "localName" : !strcmp(name,"getPrefix") ? "prefix" : "nodeValue";
   RETL(dom_ref(vm,self,field,"Ljava/lang/String;"));
}
static bool dom_node_relation(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"getParentNode"))RETL(dom_ref(vm,self,"parent","Lorg/w3c/dom/Node;"));
   if(!strcmp(name,"getOwnerDocument"))RETL(dom_ref(vm,self,"ownerDocument","Lorg/w3c/dom/Document;"));
   struct rt_list *children=list_of(vm,dom_ref(vm,self,"children","Ljava/util/List;"));
   if(!strcmp(name,"hasChildNodes"))RETI(children && children->size);
   if(!strcmp(name,"getDocumentElement")){for(uint32_t i=0;children && i<children->size;++i)if(dom_int(vm,children->items[i],"nodeType","I")==1)RETL(children->items[i]);RETL(0);}
   if(!strcmp(name,"getFirstChild") || !strcmp(name,"getLastChild"))RETL(children && children->size ? children->items[!strcmp(name,"getFirstChild") ? 0 : children->size-1] : 0);
   dvm_ref parent=dom_ref(vm,self,"parent","Lorg/w3c/dom/Node;");children=list_of(vm,dom_ref(vm,parent,"children","Ljava/util/List;"));
   for(uint32_t i=0;children && i<children->size;++i)if(children->items[i]==self){
      if(!strcmp(name,"getNextSibling"))RETL(i+1<children->size ? children->items[i+1] : 0);RETL(i ? children->items[i-1] : 0);}
   RETL(0);
}
static dvm_ref dom_list(struct dvm *vm,dvm_ref root,dvm_ref query,bool attributes)
{
   dvm_ref list=dvm_new_object(vm,dvm_find_class(vm,attributes ? "lunaria/xml/NamedNodeMap" : "lunaria/xml/NodeList"));
   (void)dvm_set_field(vm,list,"root","Lorg/w3c/dom/Node;",(union dvm_value){.l=root});
   (void)dvm_set_field(vm,list,"query","Ljava/lang/String;",(union dvm_value){.l=query});
   (void)dvm_set_field(vm,list,"attributes","Z",(union dvm_value){.i=attributes});return list;
}
static bool dom_node_list(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   bool attrs=!strcmp(builtin_name(),"getAttributes");
   if(attrs && dom_int(vm,self,"nodeType","I")!=1)RETL(0);
   RETL(dom_list(vm,self,nargs ? ARG(0).l : 0,attrs));
}
static bool dom_collect(struct dvm *vm,dvm_ref root,dvm_ref query,dvm_ref result)
{
   struct rt_list *children=list_of(vm,dom_ref(vm,root,"children","Ljava/util/List;"));uint32_t n=children ? children->size : 0;
   for(uint32_t i=0;i<n;++i){children=list_of(vm,dom_ref(vm,root,"children","Ljava/util/List;"));dvm_ref child=children->items[i];
      if(dom_int(vm,child,"nodeType","I")==1){dvm_ref name=dom_ref(vm,child,"nodeName","Ljava/lang/String;");bool equal=false;
         const char *pattern=dvm_string_utf8(vm,query);bool wildcard=pattern && dvm_string_utf8_length(vm,query)==1 && *pattern=='*';
         if(!wildcard && !am_equals(vm,query,name,&equal))return false;
         if(wildcard || equal){union dvm_value a={.l=child},ignored={0};if(!list_add(vm,result,&a,1,&ignored))return false;}}
      if(!dom_collect(vm,child,query,result))return false;
   }
   return true;
}
static dvm_ref dom_list_items(struct dvm *vm,dvm_ref self)
{
   dvm_ref root=dom_ref(vm,self,"root","Lorg/w3c/dom/Node;"),query=dom_ref(vm,self,"query","Ljava/lang/String;");
   if(!query)return dom_ref(vm,root,dom_int(vm,self,"attributes","Z") ? "attributes" : "children","Ljava/util/List;");
   dvm_ref result=coll_new_list(vm,"Ljava/util/ArrayList;");if(!dom_collect(vm,root,query,result))return 0;return result;
}
static bool dom_list_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   struct rt_list *items=list_of(vm,dom_list_items(vm,self));if(dvm_exception(vm))return false;
   if(!nargs)RETI(items ? items->size : 0);
   if(!strcmp(builtin_name(),"getNamedItem")) {
      uint32_t n=items ? items->size : 0;
      for(uint32_t i=0;i<n;++i){dvm_ref node=items->items[i];bool equal=false;
         if(!am_equals(vm,ARG(0).l,dom_ref(vm,node,"nodeName","Ljava/lang/String;"),&equal))return false;
         if(equal)RETL(node);items=list_of(vm,dom_list_items(vm,self));}
      RETL(0);
   }
   RETL(items && ARG(0).i>=0 && (uint32_t)ARG(0).i<items->size ? items->items[ARG(0).i] : 0);
}
static bool dom_attribute(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct rt_list *attrs=list_of(vm,dom_ref(vm,self,"attributes","Ljava/util/List;"));uint32_t n=attrs ? attrs->size : 0;
   for(uint32_t i=0;i<n;++i){attrs=list_of(vm,dom_ref(vm,self,"attributes","Ljava/util/List;"));dvm_ref node=attrs->items[i];bool equal=false;
      if(!am_equals(vm,ARG(0).l,dom_ref(vm,node,"nodeName","Ljava/lang/String;"),&equal))return false;
      if(equal){if(!strcmp(builtin_name(),"hasAttribute"))RETI(1);RETL(dom_ref(vm,node,"nodeValue","Ljava/lang/String;"));}}
   if(!strcmp(builtin_name(),"hasAttribute"))RETI(0);RETL(dvm_new_string(vm,""));
}
static bool dom_text_append(struct dvm *vm,dvm_ref node,struct rt_bytes *text)
{
   int type=dom_int(vm,node,"nodeType","I");
   if(type==3 || type==4 || type==2){dvm_ref value=dom_ref(vm,node,"nodeValue","Ljava/lang/String;");return !value || bytes_append(text,dvm_string_utf8(vm,value),dvm_string_utf8_length(vm,value));}
   if(type==8 || type==7)return true;
   struct rt_list *children=list_of(vm,dom_ref(vm,node,"children","Ljava/util/List;"));uint32_t n=children ? children->size : 0;
   for(uint32_t i=0;i<n;++i){children=list_of(vm,dom_ref(vm,node,"children","Ljava/util/List;"));if(!dom_text_append(vm,children->items[i],text))return false;}return true;
}
static bool dom_text_content(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;if(dom_int(vm,self,"nodeType","I")==9)RETL(0);
   int type=dom_int(vm,self,"nodeType","I");if(type==7 || type==8)RETL(dom_ref(vm,self,"nodeValue","Ljava/lang/String;"));
   struct rt_bytes text={0};if(!dom_text_append(vm,self,&text)){free(text.p);dvm__throw(vm,"java/lang/OutOfMemoryError","DOM text");return false;}
   dvm_ref value=dvm_new_string_n(vm,text.p ? (const char*)text.p : "",text.len);free(text.p);RETL(value);
}
static const struct rt_method rt_dom_source[]={
   M("<init>","()V",dom_source),M("<init>","(Ljava/io/Reader;)V",dom_source),M("<init>","(Ljava/io/InputStream;)V",dom_source),M("<init>","(Ljava/lang/String;)V",dom_source),
   M("setCharacterStream","(Ljava/io/Reader;)V",dom_source),M("getCharacterStream","()Ljava/io/Reader;",dom_source),
   M("setByteStream","(Ljava/io/InputStream;)V",dom_source),M("getByteStream","()Ljava/io/InputStream;",dom_source),
   M("setSystemId","(Ljava/lang/String;)V",dom_source),M("getSystemId","()Ljava/lang/String;",dom_source),
   M("setPublicId","(Ljava/lang/String;)V",dom_source),M("getPublicId","()Ljava/lang/String;",dom_source),
   M("setEncoding","(Ljava/lang/String;)V",dom_source),M("getEncoding","()Ljava/lang/String;",dom_source),M_END
};
static const struct rt_method rt_dom_factory[]={
   SM("newInstance","()Ljavax/xml/parsers/DocumentBuilderFactory;",dom_factory_new),
   M("newDocumentBuilder","()Ljavax/xml/parsers/DocumentBuilder;",dom_factory_builder),
   M("setNamespaceAware","(Z)V",dom_factory_option),M("isNamespaceAware","()Z",dom_factory_option),
   M("setValidating","(Z)V",dom_factory_option),M("isValidating","()Z",dom_factory_option),M_END
};
static const struct rt_method rt_dom_builder[]={
   M("parse","(Lorg/xml/sax/InputSource;)Lorg/w3c/dom/Document;",dom_parse),
   M("isNamespaceAware","()Z",dom_factory_option),M("isValidating","()Z",dom_factory_option),M_END
};
static const struct rt_method rt_dom_node[]={
   M("getNodeType","()S",dom_node_get),M("getNodeName","()Ljava/lang/String;",dom_node_get),M("getNodeValue","()Ljava/lang/String;",dom_node_get),
   M("getTagName","()Ljava/lang/String;",dom_node_get),M("getName","()Ljava/lang/String;",dom_node_get),M("getValue","()Ljava/lang/String;",dom_node_get),
   M("getNamespaceURI","()Ljava/lang/String;",dom_node_get),M("getLocalName","()Ljava/lang/String;",dom_node_get),M("getPrefix","()Ljava/lang/String;",dom_node_get),
   M("getParentNode","()Lorg/w3c/dom/Node;",dom_node_relation),M("getOwnerDocument","()Lorg/w3c/dom/Document;",dom_node_relation),
   M("getDocumentElement","()Lorg/w3c/dom/Element;",dom_node_relation),
   M("getFirstChild","()Lorg/w3c/dom/Node;",dom_node_relation),M("getLastChild","()Lorg/w3c/dom/Node;",dom_node_relation),
   M("getNextSibling","()Lorg/w3c/dom/Node;",dom_node_relation),M("getPreviousSibling","()Lorg/w3c/dom/Node;",dom_node_relation),M("hasChildNodes","()Z",dom_node_relation),
   M("getChildNodes","()Lorg/w3c/dom/NodeList;",dom_node_list),M("getElementsByTagName","(Ljava/lang/String;)Lorg/w3c/dom/NodeList;",dom_node_list),
   M("getAttributes","()Lorg/w3c/dom/NamedNodeMap;",dom_node_list),M("getAttribute","(Ljava/lang/String;)Ljava/lang/String;",dom_attribute),M("hasAttribute","(Ljava/lang/String;)Z",dom_attribute),
   M("getTextContent","()Ljava/lang/String;",dom_text_content),M_END
};
static const struct rt_method rt_dom_list[]={M("getLength","()I",dom_list_get),M("item","(I)Lorg/w3c/dom/Node;",dom_list_get),M("getNamedItem","(Ljava/lang/String;)Lorg/w3c/dom/Node;",dom_list_get),M_END};
static const char *const rt_dom_document_iface[]={"Lorg/w3c/dom/Document;",NULL};
static const char *const rt_dom_element_iface[]={"Lorg/w3c/dom/Element;",NULL};
static const char *const rt_dom_attr_iface[]={"Lorg/w3c/dom/Attr;",NULL};
static const char *const rt_dom_text_iface[]={"Lorg/w3c/dom/Text;",NULL};
static const char *const rt_dom_cdata_iface[]={"Lorg/w3c/dom/CDATASection;",NULL};
static const char *const rt_dom_comment_iface[]={"Lorg/w3c/dom/Comment;",NULL};
static const char *const rt_dom_node_iface[]={"Lorg/w3c/dom/Node;",NULL};
static const char *const rt_dom_character_iface[]={"Lorg/w3c/dom/CharacterData;",NULL};
static const char *const rt_dom_nodelist_iface[]={"Lorg/w3c/dom/NodeList;",NULL};
static const char *const rt_dom_namedmap_iface[]={"Lorg/w3c/dom/NamedNodeMap;",NULL};

static const struct rt_method rt_dom_node_decl[]={
   {"getNodeType","()S",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getNodeName","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getNodeValue","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getNamespaceURI","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getLocalName","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getPrefix","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getParentNode","()Lorg/w3c/dom/Node;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getOwnerDocument","()Lorg/w3c/dom/Document;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getFirstChild","()Lorg/w3c/dom/Node;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getLastChild","()Lorg/w3c/dom/Node;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getNextSibling","()Lorg/w3c/dom/Node;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getPreviousSibling","()Lorg/w3c/dom/Node;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"hasChildNodes","()Z",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getChildNodes","()Lorg/w3c/dom/NodeList;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getAttributes","()Lorg/w3c/dom/NamedNodeMap;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getTextContent","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_dom_document_decl[]={
   {"getDocumentElement","()Lorg/w3c/dom/Element;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getElementsByTagName","(Ljava/lang/String;)Lorg/w3c/dom/NodeList;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_dom_element_decl[]={
   {"getTagName","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getAttribute","(Ljava/lang/String;)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"hasAttribute","(Ljava/lang/String;)Z",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getElementsByTagName","(Ljava/lang/String;)Lorg/w3c/dom/NodeList;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_dom_attr_decl[]={
   {"getName","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getValue","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_dom_character_decl[]={
   M_END
};

static const struct rt_method rt_dom_text_decl[]={
   M_END
};

static const struct rt_method rt_dom_list_decl[]={
   {"getLength","()I",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"item","(I)Lorg/w3c/dom/Node;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getNamedItem","(Ljava/lang/String;)Lorg/w3c/dom/Node;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};
