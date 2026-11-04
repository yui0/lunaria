/* CharsetEncoder convenience encoding. Policies apply independently to
 * malformed UTF-16 and characters absent from the selected charset. */
static bool cs_new_encoder(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   dvm_ref encoder=dvm_new_object(vm,dvm__class_by_desc(vm,"Ljava/nio/charset/CharsetEncoder;"));
   if(!encoder)return sb_fail(vm,"java/lang/OutOfMemoryError");
   (void)dvm_set_field(vm,encoder,"charset","Ljava/nio/charset/Charset;",(union dvm_value){.l=self});
   RETL(encoder);
}
static bool ce_charset(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value cs={0};
   (void)dvm_get_field(vm,self,"charset","Ljava/nio/charset/Charset;",&cs);RETL(cs.l);
}
static bool ce_policy(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *field=(strstr(builtin_name(),"Malformed") || strstr(builtin_name(),"malformed")) ? "malformed" : "unmappable";
   union dvm_value policy={0};
   if(nargs){
      if(!args[0].l)return sb_fail(vm,"java/lang/IllegalArgumentException");
      union dvm_value name={0};(void)dvm_get_field(vm,args[0].l,"name","Ljava/lang/String;",&name);
      const char *text=name.l ? dvm_string_utf8(vm,name.l) : "";
      policy.i=!strcmp(text,"IGNORE") ? 1 : !strcmp(text,"REPLACE") ? 2 : 0;
      (void)dvm_set_field(vm,self,field,"I",policy);RETL(self);
   }
   (void)dvm_get_field(vm,self,field,"I",&policy);
   struct dvm_class *c=dvm__class_by_desc(vm,"Ljava/nio/charset/CodingErrorAction;");
   union dvm_value action={0};
   (void)dvm_get_static(vm,c,policy.i==1 ? "IGNORE" : policy.i==2 ? "REPLACE" : "REPORT","Ljava/nio/charset/CodingErrorAction;",&action);
   RETL(action.l);
}
static bool ce_reset(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)vm;(void)args;(void)nargs;RETL(self);}

static bool ce_convert(struct dvm *vm,dvm_ref self,const uint16_t *units,size_t count,
                        bool check,dvm_ref input,int32_t start,union dvm_value *out)
{
   union dvm_value charset={0},name={0},malformed={0},unmappable={0};
   (void)ce_charset(vm,self,NULL,0,&charset);(void)cs_name(vm,charset.l,NULL,0,&name);
   struct jcs cs;if(!name.l || !jcs_lookup(dvm_string_utf8(vm,name.l),&cs))return sb_fail(vm,"java/nio/charset/UnsupportedCharsetException");
   (void)dvm_get_field(vm,self,"malformed","I",&malformed);
   (void)dvm_get_field(vm,self,"unmappable","I",&unmappable);
   char *filtered=malloc(count*3+1);size_t used=0;
   if(!filtered)return sb_fail(vm,"java/lang/OutOfMemoryError");
   for(size_t i=0;i<count;){
      size_t width=1;uint16_t u=units[i];
      bool bad=u>=0xd800 && u<=0xdfff;
      if(u>=0xd800 && u<=0xdbff && i+1<count && units[i+1]>=0xdc00 && units[i+1]<=0xdfff){width=2;bad=false;}
      char scalar[6];size_t n=jcs_utf16_to_wtf8(units+i,width,scalar);
      bool missing=false;
      if(!bad){
         size_t bytes=0,decoded=0;uint8_t *encoded=jcs_encode(&cs,scalar,n,&bytes);
         char *roundtrip=encoded ? jcs_decode(&cs,encoded,bytes,&decoded) : NULL;
         if(!roundtrip){free(encoded);free(filtered);return sb_fail(vm,"java/lang/OutOfMemoryError");}
         missing=decoded!=n || memcmp(roundtrip,scalar,n);
         free(encoded);free(roundtrip);
      }
      if(bad || missing){
         int policy=check ? 0 : bad ? malformed.i : unmappable.i;
         if(!policy){
            free(filtered);
            if(check)RETI(0);
            if(input)stream_set_field(vm,input,"pos",start+(int32_t)i);
            dvm__throw(vm,bad ? "java/nio/charset/MalformedInputException" : "java/nio/charset/UnmappableCharacterException","Input length = %zu",width);
            return false;
         }
         if(policy==2){
            if(cs.kind==JCS_UTF16 || cs.kind==JCS_UTF16BE || cs.kind==JCS_UTF16LE){
               memcpy(filtered+used,"\xef\xbf\xbd",3);used+=3;
            }else filtered[used++]='?';
         }
      }else{memcpy(filtered+used,scalar,n);used+=n;}
      i+=width;
   }
   if(check){free(filtered);RETI(1);}
   size_t bytes=0;uint8_t *encoded=jcs_encode(&cs,filtered,used,&bytes);free(filtered);
   if(!encoded || bytes>INT32_MAX){free(encoded);return sb_fail(vm,"java/lang/OutOfMemoryError");}
   dvm_ref array=dvm_new_array(vm,'B',"B",(uint32_t)bytes);
   if(!array){free(encoded);return sb_fail(vm,"java/lang/OutOfMemoryError");}
   if(bytes)memcpy(dvm_array_data(vm,array),encoded,bytes);free(encoded);
   union dvm_value a={.l=array};
   if(!bb_wrap(vm,0,&a,1,out))return false;
   if(input)stream_set_field(vm,input,"pos",start+(int32_t)count);
   return true;
}
static bool ce_encode(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;dvm_ref input=args[0].l;
   if(!input)return sb_fail(vm,"java/lang/NullPointerException");
   int32_t pos=sb_field(vm,input,"pos"),limit=sb_field(vm,input,"count");
   return ce_convert(vm,self,limit>pos ? cb_data(vm,input)+pos : NULL,(size_t)(limit-pos),false,input,pos,out);
}
static bool ce_can_encode(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if(!strcmp(builtin_sig(),"(C)Z")){uint16_t u=(uint16_t)args[0].i;return ce_convert(vm,self,&u,1,true,0,0,out);}
   if(!args[0].l)return sb_fail(vm,"java/lang/NullPointerException");
   union dvm_value text={0};if(!am_vcall(vm,args[0].l,"toString","()Ljava/lang/String;",NULL,0,&text))return false;
   struct dvm_object *s=dvm__obj(vm,text.l);if(!s)return sb_fail(vm,"java/lang/NullPointerException");
   size_t n=jcs_wtf8_to_utf16(s->utf8,s->utf8_len,NULL);
   uint16_t *units=malloc((n+1)*sizeof *units);if(!units)return sb_fail(vm,"java/lang/OutOfMemoryError");
   jcs_wtf8_to_utf16(s->utf8,s->utf8_len,units);
   bool ok=ce_convert(vm,self,units,n,true,0,0,out);free(units);return ok;
}
static const struct rt_field rt_charset_encoder_fields[]={
   {"charset","Ljava/nio/charset/Charset;"},{"malformed","I"},{"unmappable","I"},F_END
};
static const struct rt_method rt_charset_encoder[]={
   M("charset","()Ljava/nio/charset/Charset;",ce_charset),
   M("onMalformedInput","(Ljava/nio/charset/CodingErrorAction;)Ljava/nio/charset/CharsetEncoder;",ce_policy),
   M("onUnmappableCharacter","(Ljava/nio/charset/CodingErrorAction;)Ljava/nio/charset/CharsetEncoder;",ce_policy),
   M("malformedInputAction","()Ljava/nio/charset/CodingErrorAction;",ce_policy),
   M("unmappableCharacterAction","()Ljava/nio/charset/CodingErrorAction;",ce_policy),
   M("reset","()Ljava/nio/charset/CharsetEncoder;",ce_reset),
   M("encode","(Ljava/nio/CharBuffer;)Ljava/nio/ByteBuffer;",ce_encode),
   M("canEncode","(C)Z",ce_can_encode),M("canEncode","(Ljava/lang/CharSequence;)Z",ce_can_encode),M_END
};
static const struct rt_field rt_coding_action_fields[]={{"name","Ljava/lang/String;"},F_END};
static const struct rt_field rt_coding_action_sfields[]={
   {"REPORT","Ljava/nio/charset/CodingErrorAction;"},{"IGNORE","Ljava/nio/charset/CodingErrorAction;"},{"REPLACE","Ljava/nio/charset/CodingErrorAction;"},F_END
};
static const struct rt_method rt_coding_action[]={M("toString","()Ljava/lang/String;",cs_name),M_END};
