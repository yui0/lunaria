/* MPL-2.0. SAX2 callbacks backed by the same XMLReader used for DOM parsing. */
static const struct rt_field rt_sax_fields[]={
   {"namespaceAware","Z"},{"namespacePrefixes","Z"},{"prefixExplicit","Z"},{"validating","Z"},{"parsing","Z"},
   {"reader","Lorg/xml/sax/XMLReader;"},{"contentHandler","Lorg/xml/sax/ContentHandler;"},
   {"errorHandler","Lorg/xml/sax/ErrorHandler;"},{"entityResolver","Lorg/xml/sax/EntityResolver;"},{"dtdHandler","Lorg/xml/sax/DTDHandler;"},F_END
};
static const struct rt_field rt_sax_attributes_fields[]={{"values","Ljava/util/List;"},F_END};
static const struct rt_field rt_sax_locator_fields[]={
   {"lineNumber","I"},{"columnNumber","I"},{"publicId","Ljava/lang/String;"},{"systemId","Ljava/lang/String;"},F_END
};
static bool sax_locator_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"getLineNumber"))RETI(dom_int(vm,self,"lineNumber","I"));
   if(!strcmp(name,"getColumnNumber"))RETI(dom_int(vm,self,"columnNumber","I"));
   RETL(dom_ref(vm,self,!strcmp(name,"getPublicId") ? "publicId" : "systemId","Ljava/lang/String;"));
}
static const struct rt_method rt_sax_locator[]={M("getLineNumber","()I",sax_locator_get),M("getColumnNumber","()I",sax_locator_get),M("getPublicId","()Ljava/lang/String;",sax_locator_get),M("getSystemId","()Ljava/lang/String;",sax_locator_get),M_END};
static const char *const rt_sax_locator_iface[]={"Lorg/xml/sax/Locator;",NULL};
static bool sax_call(struct dvm *vm,dvm_ref reader,const char *method,const char *sig,const union dvm_value *args,int nargs)
{
   dvm_ref handler=dom_ref(vm,reader,"contentHandler","Lorg/xml/sax/ContentHandler;");
   union dvm_value ignored={0};return !handler || am_vcall(vm,handler,method,sig,args,nargs,&ignored);
}
static dvm_ref sax_string(struct dvm *vm,const unsigned char *text,bool intern)
{
   dvm_ref string=dvm_new_string(vm,text ? (const char*)text : "");union dvm_value canonical={0};
   if(intern && string){if(!am_vcall(vm,string,"intern","()Ljava/lang/String;",NULL,0,&canonical))return 0;return canonical.l;}return string;
}
static bool sax_factory_new(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)self;(void)args;(void)nargs;RETL(dvm_new_object(vm,dvm_find_class(vm,"lunaria/xml/SAXParserFactory")));}
static bool sax_factory_parser(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;pthread_once(&dom_xml_once,dom_xml_load);
   if(!dom_xml.prefix || !dom_xml.line || !dom_xml.column || dom_int(vm,self,"validating","Z")){
      dvm__throw(vm,"javax/xml/parsers/ParserConfigurationException",dom_int(vm,self,"validating","Z") ? "Validation is unavailable" : "libxml2 XMLReader is unavailable");return false;}
   dvm_ref parser=dvm_new_object(vm,dvm_find_class(vm,"lunaria/xml/SAXParser")),reader=dvm_new_object(vm,dvm_find_class(vm,"lunaria/xml/XMLReader"));
   if(!parser || !reader)return false;
   int aware=dom_int(vm,self,"namespaceAware","Z");
   int prefixes=dom_int(vm,self,"prefixExplicit","Z") ? dom_int(vm,self,"namespacePrefixes","Z") : !aware;
   (void)dvm_set_field(vm,parser,"reader","Lorg/xml/sax/XMLReader;",(union dvm_value){.l=reader});
   (void)dvm_set_field(vm,parser,"namespaceAware","Z",(union dvm_value){.i=aware});
   (void)dvm_set_field(vm,reader,"namespaceAware","Z",(union dvm_value){.i=aware});
   (void)dvm_set_field(vm,reader,"namespacePrefixes","Z",(union dvm_value){.i=prefixes});RETL(parser);
}
static bool sax_parser_option(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   dvm_ref reader=dom_ref(vm,self,"reader","Lorg/xml/sax/XMLReader;");
   return dom_factory_option(vm,reader ? reader : self,args,nargs,out);
}
static bool sax_get_reader(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;RETL(dom_ref(vm,self,"reader","Lorg/xml/sax/XMLReader;"));}
static bool sax_handler(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=builtin_name(),*field,*type;
   if(strstr(name,"ContentHandler")){field="contentHandler";type="Lorg/xml/sax/ContentHandler;";}
   else if(strstr(name,"ErrorHandler")){field="errorHandler";type="Lorg/xml/sax/ErrorHandler;";}
   else if(strstr(name,"EntityResolver")){field="entityResolver";type="Lorg/xml/sax/EntityResolver;";}
   else {field="dtdHandler";type="Lorg/xml/sax/DTDHandler;";}
   if(nargs){(void)dvm_set_field(vm,self,field,type,ARG(0));RETV();}RETL(dom_ref(vm,self,field,type));
}
static bool sax_feature(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","feature name");return false;}
   const char *name=dvm_string_utf8(vm,ARG(0).l),*field=NULL;int constant=-1;
   if(!strcmp(name,"http://xml.org/sax/features/namespaces"))field="namespaceAware";
   else if(!strcmp(name,"http://xml.org/sax/features/namespace-prefixes"))field="namespacePrefixes";
   else if(!strcmp(name,"http://xml.org/sax/features/validation") || !strcmp(name,"http://xml.org/sax/features/external-general-entities") || !strcmp(name,"http://xml.org/sax/features/external-parameter-entities"))constant=0;
   else if(!strcmp(name,"http://xml.org/sax/features/string-interning"))constant=1;
   else {dvm__throw(vm,"org/xml/sax/SAXNotRecognizedException","Unknown SAX feature");return false;}
   if(nargs>1) {
      if(field){(void)dvm_set_field(vm,self,field,"Z",ARG(1));if(!strcmp(field,"namespacePrefixes"))(void)dvm_set_field(vm,self,"prefixExplicit","Z",(union dvm_value){.i=1});}
      else if(!!ARG(1).i!=constant){dvm__throw(vm,"org/xml/sax/SAXNotSupportedException","Unsupported SAX feature value");return false;}RETV();
   }
   if(field && !strcmp(field,"namespacePrefixes") && !dom_int(vm,self,"prefixExplicit","Z") &&
      object_is_a(vm,self,"Ljavax/xml/parsers/SAXParserFactory;"))RETI(!dom_int(vm,self,"namespaceAware","Z"));
   RETI(field ? dom_int(vm,self,field,"Z") : constant);
}
static bool sax_attributes_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   struct rt_list *values=list_of(vm,dom_ref(vm,self,"values","Ljava/util/List;"));int count=values ? (int)values->size : 0;
   const char *name=builtin_name(),*sig=builtin_sig();if(!nargs)RETI(count);
   int index=-1;bool numeric=sig && sig[1]=='I';
   if(numeric)index=ARG(0).i;
   else for(int i=0;i<count;++i) {
      values=list_of(vm,dom_ref(vm,self,"values","Ljava/util/List;"));dvm_ref record=values->items[i];dvm_ref *entry=dvm_array_data(vm,record);bool equal=false;
      if(!am_equals(vm,ARG(0).l,entry[nargs==2 ? 0 : 2],&equal))return false;
      if(equal && nargs==2){entry=dvm_array_data(vm,record);if(!am_equals(vm,ARG(1).l,entry[1],&equal))return false;}
      if(equal){index=i;break;}
   }
   if(!strcmp(name,"getIndex"))RETI(index);
   if(index<0 || index>=count)RETL(0);
   values=list_of(vm,dom_ref(vm,self,"values","Ljava/util/List;"));dvm_ref *entry=dvm_array_data(vm,values->items[index]);
   int slot=!strcmp(name,"getURI") ? 0 : !strcmp(name,"getLocalName") ? 1 : !strcmp(name,"getQName") ? 2 : !strcmp(name,"getType") ? 3 : 4;RETL(entry[slot]);
}
static bool sax_element(struct dvm *vm,dvm_ref self,void *xml,bool start,bool aware,bool prefixes,dvm_ref *mappings)
{
   union dvm_value names[4]={{0}};names[0].l=sax_string(vm,aware ? dom_xml.uri(xml) : NULL,true);
   names[1].l=sax_string(vm,aware ? dom_xml.local(xml) : NULL,true);names[2].l=sax_string(vm,dom_xml.name(xml),true);
   for(int i=0;i<3;++i)if(names[i].l)dvm_pin(vm,names[i].l);
   if(!start){bool good=sax_call(vm,self,"endElement","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",names,3);
      for(int i=0;i<3;++i)if(names[i].l)dvm_unpin(vm,names[i].l);return good;}
   dvm_ref attributes=dvm_new_object(vm,dvm_find_class(vm,"lunaria/xml/SAXAttributes"));if(!attributes){for(int i=0;i<3;++i)if(names[i].l)dvm_unpin(vm,names[i].l);return false;}dvm_pin(vm,attributes);
   dvm_ref values=coll_new_list(vm,"Ljava/util/ArrayList;");
   (void)dvm_set_field(vm,attributes,"values","Ljava/util/List;",(union dvm_value){.l=values});names[3].l=attributes;
   *mappings=coll_new_list(vm,"Ljava/util/ArrayList;");if(!*mappings){for(int i=0;i<3;++i)if(names[i].l)dvm_unpin(vm,names[i].l);dvm_unpin(vm,attributes);return false;}dvm_pin(vm,*mappings);
   bool good=true;int attr=dom_xml.first_attr(xml);
   while(good && attr==1) {
      const char *raw=(const char*)dom_xml.name(xml);bool declaration=raw && (!strcmp(raw,"xmlns") || !strncmp(raw,"xmlns:",6));
      if(aware && declaration) {
         union dvm_value mapping[2]={{.l=sax_string(vm,(const unsigned char*)(raw[5] ? raw+6 : ""),true)},{.l=sax_string(vm,dom_xml.value(xml),true)}},ignored={0};
         good=list_add(vm,*mappings,mapping,1,&ignored) && sax_call(vm,self,"startPrefixMapping","(Ljava/lang/String;Ljava/lang/String;)V",mapping,2);
      }
      if(good && (!declaration || prefixes)) {
         dvm_ref record=dvm_new_array(vm,'L',"Ljava/lang/String;",5);if(!record){good=false;break;}
         dvm_ref fields[5]={sax_string(vm,aware ? dom_xml.uri(xml) : NULL,true),sax_string(vm,aware ? dom_xml.local(xml) : NULL,true),sax_string(vm,dom_xml.name(xml),true),sax_string(vm,(const unsigned char*)"CDATA",true),sax_string(vm,dom_xml.value(xml),false)};
         memcpy(dvm_array_data(vm,record),fields,sizeof fields);union dvm_value item={.l=record},ignored={0};good=list_add(vm,values,&item,1,&ignored);
      }
      attr=dom_xml.next_attr(xml);
   }
   (void)dom_xml.element(xml);
   good=good && sax_call(vm,self,"startElement","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lorg/xml/sax/Attributes;)V",names,4);
   for(int i=0;i<4;++i)if(names[i].l)dvm_unpin(vm,names[i].l);return good;
}
static bool sax_end_mappings(struct dvm *vm,dvm_ref self,dvm_ref mappings)
{
   struct rt_list *list=list_of(vm,mappings);int n=list ? (int)list->size : 0;bool good=true;
   for(int i=n-1;i>=0 && good;--i){list=list_of(vm,mappings);union dvm_value prefix={.l=list->items[i]};good=sax_call(vm,self,"endPrefixMapping","(Ljava/lang/String;)V",&prefix,1);}
   if(mappings)dvm_unpin(vm,mappings);return good;
}
static bool sax_parse(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;(void)out;
   if(dom_int(vm,self,"parsing","Z")){dvm__throw(vm,"org/xml/sax/SAXException","Parser is already parsing");return false;}
   bool aware=dom_int(vm,self,"namespaceAware","Z"),prefixes=dom_int(vm,self,"namespacePrefixes","Z");
   if(aware && prefixes){dvm__throw(vm,"org/xml/sax/SAXNotSupportedException","namespaces and namespace-prefixes cannot both be enabled");return false;}
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","InputSource");return false;}
   (void)dvm_set_field(vm,self,"parsing","Z",(union dvm_value){.i=1});
   struct rt_bytes bytes={0};bool characters=false,good=dom_read_source(vm,ARG(0).l,&bytes,&characters);void *xml=NULL;
   dvm_ref *mappings=NULL;size_t capacity=0;int result=0;dvm_ref locator=0;
   dvm_ref encoding=dom_ref(vm,ARG(0).l,"encoding","Ljava/lang/String;");
   if(good && bytes.len>INT_MAX){dvm__throw(vm,"org/xml/sax/SAXException","XML input too large");good=false;}
   if(good){xml=dom_xml.create((const char*)bytes.p,(int)bytes.len,NULL,characters ? "UTF-8" : encoding ? dvm_string_utf8(vm,encoding) : NULL,2048);
      if(!xml){dvm__throw(vm,"org/xml/sax/SAXParseException","Cannot create XML reader");good=false;}}
   if(good) {
      locator=dvm_new_object(vm,dvm_find_class(vm,"lunaria/xml/SAXLocator"));if(!locator)good=false;
      else {dvm_pin(vm,locator);
         (void)dvm_set_field(vm,locator,"lineNumber","I",(union dvm_value){.i=-1});
         (void)dvm_set_field(vm,locator,"columnNumber","I",(union dvm_value){.i=-1});
         (void)dvm_set_field(vm,locator,"publicId","Ljava/lang/String;",(union dvm_value){.l=dom_ref(vm,ARG(0).l,"publicId","Ljava/lang/String;")});
         (void)dvm_set_field(vm,locator,"systemId","Ljava/lang/String;",(union dvm_value){.l=dom_ref(vm,ARG(0).l,"systemId","Ljava/lang/String;")});
         union dvm_value call={.l=locator};good=sax_call(vm,self,"setDocumentLocator","(Lorg/xml/sax/Locator;)V",&call,1);}
   }
   if(good)good=sax_call(vm,self,"startDocument","()V",NULL,0);
   while(good && (result=dom_xml.read(xml))==1) {
      (void)dvm_set_field(vm,locator,"lineNumber","I",(union dvm_value){.i=dom_xml.line(xml)});
      (void)dvm_set_field(vm,locator,"columnNumber","I",(union dvm_value){.i=dom_xml.column(xml)});
      int type=dom_xml.type(xml),depth=dom_xml.depth(xml);
      if(type==1 || type==15) {
         if(depth<0){good=false;break;}
         if((size_t)depth>=capacity){size_t next=(size_t)depth+16;dvm_ref *p=realloc(mappings,next*sizeof *p);if(!p){dvm__throw(vm,"java/lang/OutOfMemoryError","SAX depth");good=false;break;}memset(p+capacity,0,(next-capacity)*sizeof *p);mappings=p;capacity=next;}
         if(type==1) {
            bool empty=dom_xml.empty(xml)!=0;
            good=sax_element(vm,self,xml,true,aware,prefixes,&mappings[depth]);
            if(good && empty){good=sax_element(vm,self,xml,false,aware,prefixes,NULL);if(good)good=sax_end_mappings(vm,self,mappings[depth]);else if(mappings[depth])dvm_unpin(vm,mappings[depth]);mappings[depth]=0;}
         } else {
            good=sax_element(vm,self,xml,false,aware,prefixes,NULL);
            if(good)good=sax_end_mappings(vm,self,mappings[depth]);else if(mappings[depth])dvm_unpin(vm,mappings[depth]);mappings[depth]=0;
         }
      } else if(type==3 || type==4 || type==13 || type==14) {
         const char *text=(const char*)dom_xml.value(xml);size_t n=text ? strlen(text) : 0,count=jcs_wtf8_to_utf16(text ? text : "",n,NULL);
         dvm_ref array=dvm_new_array(vm,'C',NULL,(uint32_t)count);if(!array){good=false;break;}
         if(count)(void)jcs_wtf8_to_utf16(text,n,dvm_array_data(vm,array));
         union dvm_value call[3]={{.l=array},{0},{.i=(int32_t)count}};good=sax_call(vm,self,"characters","([CII)V",call,3);
      } else if(type==7) {
         union dvm_value call[2]={{.l=sax_string(vm,dom_xml.name(xml),true)},{.l=sax_string(vm,dom_xml.value(xml),false)}};
         good=sax_call(vm,self,"processingInstruction","(Ljava/lang/String;Ljava/lang/String;)V",call,2);
      } else if(type==5 || type==6 || type==10){dvm__throw(vm,"org/xml/sax/SAXException","DTD and custom entity SAX parsing is not implemented");good=false;}
   }
   if(good && result<0){
      dvm__throw(vm,"org/xml/sax/SAXParseException","Malformed XML document");good=false;
      dvm_ref error=dvm_exception(vm),handler=dom_ref(vm,self,"errorHandler","Lorg/xml/sax/ErrorHandler;");
      (void)dvm_set_field(vm,error,"lineNumber","I",(union dvm_value){.i=dom_xml.line(xml)});
      (void)dvm_set_field(vm,error,"columnNumber","I",(union dvm_value){.i=dom_xml.column(xml)});
      if(handler){dvm_pin(vm,error);dvm_clear_exception(vm);union dvm_value call={.l=error},ignored={0};
         (void)am_vcall(vm,handler,"fatalError","(Lorg/xml/sax/SAXParseException;)V",&call,1,&ignored);
         if(!dvm_exception(vm))vm->exception=error;dvm_unpin(vm,error);}
   }
   if(good)good=sax_call(vm,self,"endDocument","()V",NULL,0);
   for(size_t i=0;i<capacity;++i)if(mappings[i])dvm_unpin(vm,mappings[i]);
   if(locator)dvm_unpin(vm,locator);if(xml)dom_xml.destroy(xml);free(bytes.p);free(mappings);
   (void)dvm_set_field(vm,self,"parsing","Z",(union dvm_value){0});return good;
}
static bool sax_parser_parse(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   dvm_ref reader=dom_ref(vm,self,"reader","Lorg/xml/sax/XMLReader;");
   if(nargs>1 && ARG(1).l) {
      const char *fields[]={"contentHandler","errorHandler","dtdHandler","entityResolver"};
      const char *types[]={"Lorg/xml/sax/ContentHandler;","Lorg/xml/sax/ErrorHandler;","Lorg/xml/sax/DTDHandler;","Lorg/xml/sax/EntityResolver;"};
      for(int i=0;i<4;++i)(void)dvm_set_field(vm,reader,fields[i],types[i],ARG(1));
   }
   return sax_parse(vm,reader,args,1,out);
}
static bool sax_default_fatal(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)self;(void)nargs;(void)out;vm->exception=ARG(0).l;return false;}
static const struct rt_method rt_sax_factory[]={
   SM("newInstance","()Ljavax/xml/parsers/SAXParserFactory;",sax_factory_new),M("newSAXParser","()Ljavax/xml/parsers/SAXParser;",sax_factory_parser),
   M("setNamespaceAware","(Z)V",dom_factory_option),M("isNamespaceAware","()Z",dom_factory_option),M("setValidating","(Z)V",dom_factory_option),M("isValidating","()Z",dom_factory_option),
   M("setFeature","(Ljava/lang/String;Z)V",sax_feature),M("getFeature","(Ljava/lang/String;)Z",sax_feature),M_END
};
static const struct rt_method rt_sax_parser[]={M("getXMLReader","()Lorg/xml/sax/XMLReader;",sax_get_reader),
   M("parse","(Lorg/xml/sax/InputSource;Lorg/xml/sax/helpers/DefaultHandler;)V",sax_parser_parse),
   M("isNamespaceAware","()Z",sax_parser_option),M("isValidating","()Z",sax_parser_option),M_END};
static const struct rt_method rt_sax_reader[]={
   M("setContentHandler","(Lorg/xml/sax/ContentHandler;)V",sax_handler),M("getContentHandler","()Lorg/xml/sax/ContentHandler;",sax_handler),
   M("setErrorHandler","(Lorg/xml/sax/ErrorHandler;)V",sax_handler),M("getErrorHandler","()Lorg/xml/sax/ErrorHandler;",sax_handler),
   M("setEntityResolver","(Lorg/xml/sax/EntityResolver;)V",sax_handler),M("getEntityResolver","()Lorg/xml/sax/EntityResolver;",sax_handler),
   M("setDTDHandler","(Lorg/xml/sax/DTDHandler;)V",sax_handler),M("getDTDHandler","()Lorg/xml/sax/DTDHandler;",sax_handler),
   M("parse","(Lorg/xml/sax/InputSource;)V",sax_parse),M("setFeature","(Ljava/lang/String;Z)V",sax_feature),M("getFeature","(Ljava/lang/String;)Z",sax_feature),M_END
};
static const struct rt_method rt_sax_attributes[]={
   M("getLength","()I",sax_attributes_get),M("getURI","(I)Ljava/lang/String;",sax_attributes_get),M("getLocalName","(I)Ljava/lang/String;",sax_attributes_get),M("getQName","(I)Ljava/lang/String;",sax_attributes_get),
   M("getIndex","(Ljava/lang/String;)I",sax_attributes_get),M("getIndex","(Ljava/lang/String;Ljava/lang/String;)I",sax_attributes_get),
   M("getType","(I)Ljava/lang/String;",sax_attributes_get),M("getType","(Ljava/lang/String;)Ljava/lang/String;",sax_attributes_get),M("getType","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",sax_attributes_get),
   M("getValue","(I)Ljava/lang/String;",sax_attributes_get),M("getValue","(Ljava/lang/String;)Ljava/lang/String;",sax_attributes_get),M("getValue","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",sax_attributes_get),M_END
};
static const struct rt_method rt_sax_default_handler[]={
   M("<init>","()V",empty_void),M("setDocumentLocator","(Lorg/xml/sax/Locator;)V",empty_void),M("startDocument","()V",empty_void),M("endDocument","()V",empty_void),
   M("startElement","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lorg/xml/sax/Attributes;)V",empty_void),M("endElement","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",empty_void),
   M("characters","([CII)V",empty_void),M("ignorableWhitespace","([CII)V",empty_void),M("processingInstruction","(Ljava/lang/String;Ljava/lang/String;)V",empty_void),
   M("startPrefixMapping","(Ljava/lang/String;Ljava/lang/String;)V",empty_void),M("endPrefixMapping","(Ljava/lang/String;)V",empty_void),M("skippedEntity","(Ljava/lang/String;)V",empty_void),
   M("resolveEntity","(Ljava/lang/String;Ljava/lang/String;)Lorg/xml/sax/InputSource;",ret_zero),M("notationDecl","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",empty_void),
   M("unparsedEntityDecl","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",empty_void),
   M("warning","(Lorg/xml/sax/SAXParseException;)V",empty_void),M("error","(Lorg/xml/sax/SAXParseException;)V",empty_void),M("fatalError","(Lorg/xml/sax/SAXParseException;)V",sax_default_fatal),M_END
};
static const char *const rt_sax_reader_iface[]={"Lorg/xml/sax/XMLReader;",NULL};
static const char *const rt_sax_attributes_iface[]={"Lorg/xml/sax/Attributes;",NULL};
static const char *const rt_sax_handler_ifaces[]={"Lorg/xml/sax/ContentHandler;","Lorg/xml/sax/ErrorHandler;","Lorg/xml/sax/DTDHandler;","Lorg/xml/sax/EntityResolver;",NULL};

static const struct rt_method rt_sax_reader_decl[]={
   {"setContentHandler","(Lorg/xml/sax/ContentHandler;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getContentHandler","()Lorg/xml/sax/ContentHandler;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"setErrorHandler","(Lorg/xml/sax/ErrorHandler;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getErrorHandler","()Lorg/xml/sax/ErrorHandler;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"setEntityResolver","(Lorg/xml/sax/EntityResolver;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getEntityResolver","()Lorg/xml/sax/EntityResolver;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"setDTDHandler","(Lorg/xml/sax/DTDHandler;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getDTDHandler","()Lorg/xml/sax/DTDHandler;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"parse","(Lorg/xml/sax/InputSource;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"setFeature","(Ljava/lang/String;Z)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getFeature","(Ljava/lang/String;)Z",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_sax_attributes_decl[]={
   {"getLength","()I",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getURI","(I)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getLocalName","(I)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getQName","(I)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getIndex","(Ljava/lang/String;)I",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getIndex","(Ljava/lang/String;Ljava/lang/String;)I",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getType","(I)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getType","(Ljava/lang/String;)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getType","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getValue","(I)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getValue","(Ljava/lang/String;)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getValue","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_sax_locator_decl[]={
   {"getLineNumber","()I",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getColumnNumber","()I",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getPublicId","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"getSystemId","()Ljava/lang/String;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_sax_content_decl[]={
   {"setDocumentLocator","(Lorg/xml/sax/Locator;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"startDocument","()V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"endDocument","()V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"startElement","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lorg/xml/sax/Attributes;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"endElement","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"characters","([CII)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"ignorableWhitespace","([CII)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"processingInstruction","(Ljava/lang/String;Ljava/lang/String;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"startPrefixMapping","(Ljava/lang/String;Ljava/lang/String;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"endPrefixMapping","(Ljava/lang/String;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"skippedEntity","(Ljava/lang/String;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_sax_error_decl[]={
   {"warning","(Lorg/xml/sax/SAXParseException;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"error","(Lorg/xml/sax/SAXParseException;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"fatalError","(Lorg/xml/sax/SAXParseException;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_sax_dtd_decl[]={
   {"notationDecl","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   {"unparsedEntityDecl","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};

static const struct rt_method rt_sax_entity_decl[]={
   {"resolveEntity","(Ljava/lang/String;Ljava/lang/String;)Lorg/xml/sax/InputSource;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},
   M_END
};
