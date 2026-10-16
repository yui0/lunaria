/* MPL-2.0. java.text number, choice and message formatting.
 * Implementation section of dvm_runtime.c; shared helpers remain in that
 * translation unit. Keep registration tables in dvm_runtime.c. */

#include <locale.h>
#include <fenv.h>

static const char *const decsym_interfaces[]={"Ljava/lang/Cloneable;","Ljava/io/Serializable;",NULL};

/* Number formatting uses instance-local symbols, never the host LC_NUMERIC.
 * The locale groups below cover the numeric conventions implemented here;
 * unknown locale identifiers inherit the root decimal convention. */
static const struct rt_field rt_decimal_symbols_fields[] = {
   {"zeroDigit","C"},{"decimalSeparator","C"},{"groupingSeparator","C"},
   {"minusSign","C"},{"primaryGroup","I"},{"secondaryGroup","I"},F_END
};
static bool decsym_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value locale={0};
   if(nargs) {
      locale=args[0];
      if(!locale.l) {dvm__throw(vm,"java/lang/NullPointerException","locale");return false;}
   } else if(!loc_getDefault(vm,0,NULL,0,&locale)) return false;
   const char *language=loc_field_utf8(vm,locale.l,"language");
   const char *country=loc_field_utf8(vm,locale.l,"country");
   uint16_t zero='0',decimal='.',group=',';
   int secondary=3;
   static const char *const comma_languages[]={"de","da","el","es","id","is","it","nl","pt","ro","tr","vi",NULL};
   static const char *const space_languages[]={"bg","cs","et","fi","hr","hu","lt","lv","nb","nn","pl","ru","sk","sl","sq","sv","uk",NULL};
   for(size_t i=0;comma_languages[i];++i) if(!strcmp(language,comma_languages[i])) {decimal=',';group='.';break;}
   for(size_t i=0;space_languages[i];++i) if(!strcmp(language,space_languages[i])) {decimal=',';group=0x00a0;break;}
   if(!strcmp(language,"fr")) {decimal=',';group=!strcmp(country,"CA") ? 0x00a0 : 0x202f;}
   if(!strcmp(language,"de") && (!strcmp(country,"CH") || !strcmp(country,"LI"))) {decimal='.';group=0x2019;}
   if(!strcmp(language,"ar") && strcmp(country,"DZ") && strcmp(country,"MA") && strcmp(country,"TN") && strcmp(country,"LY")) {
      zero=0x0660;decimal=0x066b;group=0x066c;
   }
   if(!strcmp(language,"fa")) {zero=0x06f0;decimal=0x066b;group=0x066c;}
   if(!strcmp(language,"bn")) zero=0x09e6;
   if(!strcmp(language,"mr") || !strcmp(language,"ne")) zero=0x0966;
   if(!strcmp(language,"as")) zero=0x09e6;
   if(!strcmp(language,"bn") || !strcmp(language,"as") || !strcmp(language,"hi") ||
      !strcmp(language,"mr") || !strcmp(language,"ne") ||
      ((!strcmp(language,"en") || !strcmp(language,"ta") || !strcmp(language,"te") || !strcmp(language,"kn") || !strcmp(language,"ml")) && !strcmp(country,"IN"))) secondary=2;
   (void)dvm_set_field(vm,self,"zeroDigit","C",(union dvm_value){.i=zero});
   (void)dvm_set_field(vm,self,"decimalSeparator","C",(union dvm_value){.i=decimal});
   (void)dvm_set_field(vm,self,"groupingSeparator","C",(union dvm_value){.i=group});
   (void)dvm_set_field(vm,self,"minusSign","C",(union dvm_value){.i='-'});
   rt_set_int(vm,self,"primaryGroup",3);rt_set_int(vm,self,"secondaryGroup",secondary);
   RETV();
}
static dvm_ref decsym_copy(struct dvm *vm,dvm_ref source)
{
   dvm_ref copy=dvm_new_object(vm,dvm_find_class(vm,"java/text/DecimalFormatSymbols"));
   if(!copy) return 0;
   for(const struct rt_field *f=rt_decimal_symbols_fields;f->name;++f) {
      union dvm_value value={0};
      (void)dvm_get_field(vm,source,f->name,f->type,&value);
      (void)dvm_set_field(vm,copy,f->name,f->type,value);
   }
   return copy;
}
static bool decsym_factory(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;
   dvm_ref symbols=dvm_new_object(vm,dvm_find_class(vm,"java/text/DecimalFormatSymbols"));
   if(!symbols) return false;
   dvm_pin(vm,symbols);union dvm_value ignored={0};
   bool good=decsym_init(vm,symbols,args,nargs,&ignored);dvm_unpin(vm,symbols);
   if(!good) return false;
   RETL(symbols);
}
static bool decsym_clone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;RETL(decsym_copy(vm,self));
}
static bool decsym_char(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=builtin_name();
   const char *field=strstr(name,"ZeroDigit") ? "zeroDigit" : strstr(name,"DecimalSeparator") ? "decimalSeparator" :
      strstr(name,"GroupingSeparator") ? "groupingSeparator" : "minusSign";
   union dvm_value value={0};
   if(nargs) {(void)dvm_set_field(vm,self,field,"C",args[0]);RETV();}
   (void)dvm_get_field(vm,self,field,"C",&value);*out=value;return true;
}
static const struct rt_method rt_decimal_symbols[]={
   M("<init>","()V",decsym_init),M("<init>","(Ljava/util/Locale;)V",decsym_init),
   SM("getInstance","()Ljava/text/DecimalFormatSymbols;",decsym_factory),
   SM("getInstance","(Ljava/util/Locale;)Ljava/text/DecimalFormatSymbols;",decsym_factory),
   M("clone","()Ljava/lang/Object;",decsym_clone),
   M("getZeroDigit","()C",decsym_char),M("setZeroDigit","(C)V",decsym_char),
   M("getDecimalSeparator","()C",decsym_char),M("setDecimalSeparator","(C)V",decsym_char),
   M("getGroupingSeparator","()C",decsym_char),M("setGroupingSeparator","(C)V",decsym_char),
   M("getMinusSign","()C",decsym_char),M("setMinusSign","(C)V",decsym_char),M_END
};
static int decfmt_symbol(struct dvm *vm,dvm_ref self,const char *name,int fallback)
{
   union dvm_value symbols={0},value={0};
   (void)dvm_get_field(vm,self,"symbols","Ljava/text/DecimalFormatSymbols;",&symbols);
   if(!symbols.l || !dvm_get_field(vm,symbols.l,name,"C",&value)) return fallback;
   return value.i;
}

/* java.text.DecimalFormat.  Only the pattern features that change the output
 * are modelled: the fraction-digit range and grouping. */
static const struct rt_field rt_decimal_format_fields[] = {
   { "pattern", "Ljava/lang/String;" },
   { "minFrac", "I" }, { "maxFrac", "I" }, { "minInt", "I" }, { "maxInt", "I" },
   { "primaryGroup", "I" }, { "secondaryGroup", "I" },
   { "grouping", "Z" }, { "parseIntegerOnly", "Z" }, { "symbols", "Ljava/text/DecimalFormatSymbols;" }, F_END
};

static void decfmt_apply(struct dvm *vm,dvm_ref self,const char *pattern)
{
   int min=0,max=0,min_integer=0,digits=0,last=-1,previous=-1;
   bool frac=false,quote=false;
   for(const char *p=pattern;p && *p;++p) {
      if(*p=='\'') {quote=!quote;continue;}
      if(quote) continue;
      if(*p==';' || *p=='E') break;
      if(*p=='.') {frac=true;continue;}
      if(*p==',' && !frac) {previous=last;last=digits;continue;}
      if(*p=='0' || *p=='#') {
         if(frac) {++max;if(*p=='0') ++min;}
         else {++digits;if(*p=='0') ++min_integer;}
      }
   }
   (void)dvm_set_field(vm,self,"pattern","Ljava/lang/String;",(union dvm_value){.l=dvm_new_string(vm,pattern ? pattern : "")});
   rt_set_int(vm,self,"minFrac",min);rt_set_int(vm,self,"maxFrac",max);
   rt_set_int(vm,self,"minInt",min_integer);rt_set_int(vm,self,"maxInt",309);
   rt_set_int(vm,self,"primaryGroup",last<0 ? 3 : digits-last);
   rt_set_int(vm,self,"secondaryGroup",previous<0 ? digits-last : last-previous);
   (void)dvm_set_field(vm,self,"grouping","Z",(union dvm_value){.i=last>=0});
}

static bool decfmt_init(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)out;
   if (nargs && !ARG(0).l) {
      dvm__throw(vm,"java/lang/NullPointerException","pattern"); return false;
   }
   const char *pattern = nargs ? dvm_string_utf8(vm, ARG(0).l) : "#,##0.###";
   union dvm_value symbols={0};
   if(nargs>1) {
      if(!args[1].l) {dvm__throw(vm,"java/lang/NullPointerException","symbols");return false;}
      symbols.l=decsym_copy(vm,args[1].l);
   } else if(!decsym_factory(vm,0,NULL,0,&symbols)) return false;
   if(!symbols.l) return false;
   (void)dvm_set_field(vm,self,"symbols","Ljava/text/DecimalFormatSymbols;",symbols);
   rt_set_int(vm,self,"minInt",1);rt_set_int(vm,self,"maxInt",309);
   if(!nargs && rt_get_int(vm,symbols.l,"secondaryGroup")==2) pattern="#,##,##0.###";
   decfmt_apply(vm, self, pattern);
   RETV();
}

static bool decfmt_applyPattern(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)out;
   if (!nargs || !ARG(0).l) {
      dvm__throw(vm,"java/lang/NullPointerException","pattern"); return false;
   }
   decfmt_apply(vm, self, dvm_string_utf8(vm, ARG(0).l));
   RETV();
}

static dvm_ref text_from_utf16(struct dvm *vm,const uint16_t *units,size_t length);
static bool decfmt_ascii(struct dvm *vm,dvm_ref self,const char *ascii,union dvm_value *out)
{
   uint16_t units[2048];size_t used=0;
   if(*ascii=='-') {units[used++]=(uint16_t)decfmt_symbol(vm,self,"minusSign",'-');++ascii;}
   uint16_t zero=(uint16_t)decfmt_symbol(vm,self,"zeroDigit",'0');
   uint16_t decimal=(uint16_t)decfmt_symbol(vm,self,"decimalSeparator",'.');
   uint16_t group=(uint16_t)decfmt_symbol(vm,self,"groupingSeparator",',');
   const char *dot=strchr(ascii,'.');size_t digits=dot ? (size_t)(dot-ascii) : strlen(ascii);
   int min=rt_get_int(vm,self,"minInt"),max=rt_get_int(vm,self,"maxInt");
   if(min<0) min=0;if(min>309) min=309;
   if(max<min) max=min;if(max>309) max=309;
   if(digits>(size_t)max) {ascii+=digits-(size_t)max;digits=(size_t)max;}
   size_t padding=(size_t)min>digits ? (size_t)min-digits : 0,total=padding+digits;
   if(!min && digits==1 && ascii[0]=='0' && dot && dot[1]) total=digits=0;
   union dvm_value grouping={0};(void)dvm_get_field(vm,self,"grouping","Z",&grouping);
   int primary=rt_get_int(vm,self,"primaryGroup"),secondary=rt_get_int(vm,self,"secondaryGroup");
   if(primary<1) primary=3;if(secondary<1) secondary=primary;
   for(size_t i=0;i<total;++i) {
      size_t remaining=total-i;
      if(i && grouping.i && (remaining==(size_t)primary ||
         (remaining>(size_t)primary && (remaining-(size_t)primary)%(size_t)secondary==0))) units[used++]=group;
      units[used++]=(uint16_t)(zero+(i<padding ? 0 : ascii[i-padding]-'0'));
   }
   if(dot && dot[1]) {
      units[used++]=decimal;
      for(const char *p=dot+1;*p;++p) units[used++]=(uint16_t)(zero+*p-'0');
   }
   dvm_ref text=text_from_utf16(vm,units,used);
   if(!text) return false;
   RETL(text);
}
static bool decfmt_render(struct dvm *vm,dvm_ref self,double value,union dvm_value *out)
{
   if(isnan(value)) RETL(dvm_new_string(vm,"NaN"));
   if(isinf(value)) {
      uint16_t units[2]={(uint16_t)decfmt_symbol(vm,self,"minusSign",'-'),0x221e};
      RETL(text_from_utf16(vm,units+(value>0),value>0 ? 1 : 2));
   }
   int min=rt_get_int(vm,self,"minFrac"),max=rt_get_int(vm,self,"maxFrac");
   if(min<0) min=0;if(min>340) min=340;
   if(max<min) max=min;if(max>340) max=340;
   /* A finite double needs at most309 integer digits. Space for340 fraction
    * digits avoids the old fixed64-byte output truncation and15-digit cap. */
   char ascii[768];int rounding=fegetround();
   if(rounding!=FE_TONEAREST) (void)fesetround(FE_TONEAREST);
   int length=snprintf(ascii,sizeof ascii,"%.*f",max,value);
   if(rounding!=-1 && rounding!=FE_TONEAREST) (void)fesetround(rounding);
   if(length<0 || (size_t)length>=sizeof ascii) {dvm__throw(vm,"java/lang/OutOfMemoryError","formatted double");return false;}
   const char *host_decimal=localeconv()->decimal_point;
   if(host_decimal && *host_decimal && strcmp(host_decimal,".")) {
      char *point=strstr(ascii,host_decimal);
      if(point) {size_t n=strlen(host_decimal);memmove(point+1,point+n,strlen(point+n)+1);*point='.';}
   }
   char *dot=strchr(ascii,'.');
   if(dot) {
      char *end=ascii+strlen(ascii),*keep=dot+1+min;
      while(end>keep && end[-1]=='0') --end;
      if(end==dot+1) --end;
      *end=0;
   }
   return decfmt_ascii(vm,self,ascii,out);
}
static bool decfmt_format_d(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   return decfmt_render(vm,self,ARG(0).d,out);
}
static bool decfmt_format_j(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   int64_t value=ARG(0).j;
   uint64_t magnitude=value<0 ? UINT64_C(0)-(uint64_t)value : (uint64_t)value;
   char digits[20],ascii[384];size_t count=0,used=0;
   do {digits[count++]=(char)('0'+magnitude%10);magnitude/=10;} while(magnitude);
   if(value<0) ascii[used++]='-';
   while(count) ascii[used++]=digits[--count];
   int min=rt_get_int(vm,self,"minFrac");if(min<0) min=0;if(min>340) min=340;
   if(min) {ascii[used++]='.';memset(ascii+used,'0',(size_t)min);used+=(size_t)min;}
   ascii[used]=0;return decfmt_ascii(vm,self,ascii,out);
}

static bool decfmt_set_max_frac(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)out;
   int digits=ARG(0).i<0 ? 0 : ARG(0).i;
   rt_set_int(vm, self, "maxFrac", digits);
   if (rt_get_int(vm, self, "minFrac") > digits)
      rt_set_int(vm, self, "minFrac", digits);
   RETV();
}

static bool decfmt_set_min_frac(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)out;
   int digits=ARG(0).i<0 ? 0 : ARG(0).i;
   rt_set_int(vm, self, "minFrac", digits);
   if (rt_get_int(vm, self, "maxFrac") < digits)
      rt_set_int(vm, self, "maxFrac", digits);
   RETV();
}

static bool decfmt_set_grouping(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .i = ARG(0).i };
   (void)dvm_set_field(vm, self, "grouping", "Z", v);
   RETV();
}

static bool decfmt_toPattern(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "pattern", "Ljava/lang/String;", &v);
   RETL(v.l ? v.l : dvm_new_string(vm, ""));
}

/* MPL-2.0. FieldPosition state shared by Format subclasses. */
static const struct rt_field rt_field_position_fields[]={
   {"field","I"},{"attribute","Ljava/text/Format$Field;"},{"beginIndex","I"},{"endIndex","I"},F_END
};
static bool field_position_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   bool attribute=dvm__builtin_method()->sig[1]=='L';
   rt_set_int(vm,self,"field",attribute ? nargs>1 ? args[1].i : -1 : args[0].i);
   dvm_set_field(vm,self,"attribute","Ljava/text/Format$Field;",(union dvm_value){.l=attribute ? args[0].l : 0});
   rt_set_int(vm,self,"beginIndex",0);rt_set_int(vm,self,"endIndex",0);RETV();
}
static bool field_position_access(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=dvm__builtin_method()->name;
   if (!strcmp(name,"getFieldAttribute")) {
      union dvm_value value={0};dvm_get_field(vm,self,"attribute","Ljava/text/Format$Field;",&value);RETL(value.l);
   }
   const char *field=strstr(name,"Begin") ? "beginIndex" : strstr(name,"End") ? "endIndex" : "field";
   if (nargs) {rt_set_int(vm,self,field,args[0].i);RETV();}
   RETI(rt_get_int(vm,self,field));
}
static bool field_position_equals(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct dvm_class *other=dvm_object_class(vm,args[0].l);
   if (!other || !dvm__class_assignable(vm,other,dvm_find_class(vm,"java/text/FieldPosition"))) RETI(0);
   union dvm_value a={0},b={0},equal={0};
   dvm_get_field(vm,self,"attribute","Ljava/text/Format$Field;",&a);dvm_get_field(vm,args[0].l,"attribute","Ljava/text/Format$Field;",&b);
   if (a.l!=b.l) {
      if (!a.l || !b.l) RETI(0);
      struct dvm_method *method=dvm_find_method(vm,dvm_object_class(vm,a.l),"equals","(Ljava/lang/Object;)Z");
      if (!method || !dvm_call(vm,method,a.l,&b,1,&equal)) return false;
      if (!equal.i) RETI(0);
   }
   RETI(rt_get_int(vm,self,"field")==rt_get_int(vm,args[0].l,"field") &&
      rt_get_int(vm,self,"beginIndex")==rt_get_int(vm,args[0].l,"beginIndex") && rt_get_int(vm,self,"endIndex")==rt_get_int(vm,args[0].l,"endIndex"));
}
static bool field_position_hash(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   RETI((int32_t)(((uint32_t)rt_get_int(vm,self,"field")<<24)|((uint32_t)rt_get_int(vm,self,"beginIndex")<<16)|(uint32_t)rt_get_int(vm,self,"endIndex")));
}
static const struct rt_method rt_field_position[]={
   M("<init>","(I)V",field_position_init),M("<init>","(Ljava/text/Format$Field;)V",field_position_init),
   M("<init>","(Ljava/text/Format$Field;I)V",field_position_init),
   M("getField","()I",field_position_access),M("getFieldAttribute","()Ljava/text/Format$Field;",field_position_access),
   M("getBeginIndex","()I",field_position_access),M("getEndIndex","()I",field_position_access),
   M("setBeginIndex","(I)V",field_position_access),M("setEndIndex","(I)V",field_position_access),
   M("equals","(Ljava/lang/Object;)Z",field_position_equals),M("hashCode","()I",field_position_hash),M_END
};

static bool numfmt_string(struct dvm *vm,dvm_ref self,union dvm_value number,bool integral,union dvm_value *out)
{
   dvm_ref buffer=dvm_new_object(vm,dvm_find_class(vm,"java/lang/StringBuffer"));
   dvm_ref position=dvm_new_object(vm,dvm_find_class(vm,"java/text/FieldPosition"));
   union dvm_value ignored={0};
   if (!buffer || !position || !sb_init(vm,buffer,NULL,0,&ignored)) return false;
   union dvm_value arguments[3]={number,{.l=buffer},{.l=position}};
   struct dvm_method *format=dvm_find_method(vm,dvm_object_class(vm,self),"format",integral ?
      "(JLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;" :
      "(DLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;");
   if (!format || !dvm_call(vm,format,self,arguments,3,&ignored)) return false;
   return sb_toString(vm,buffer,NULL,0,out);
}
static bool numfmt_primitive(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   return numfmt_string(vm,self,args[0],dvm__builtin_method()->sig[1]=='J',out);
}
static bool decfmt_buffer(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   bool integral=dvm__builtin_method()->sig[1]=='J'; union dvm_value text={0},length={0};
   if (!args[1].l) {dvm__throw(vm,"java/lang/NullPointerException","toAppendTo");return false;}
   if (!(integral ? decfmt_format_j(vm,self,args,1,&text) : decfmt_format_d(vm,self,args,1,&text))) return false;
   if (!sb_length(vm,args[1].l,NULL,0,&length)) return false;
   if (args[2].l) {
      size_t n=0;const char *s=sdata(vm,text.l,&n);size_t first=n && s[0]=='-' ? 1 : 0,end=first;
      while(end<n && (s[end]==',' || (s[end]>='0' && s[end]<='9'))) ++end;
      int field=rt_get_int(vm,args[2].l,"field");
      rt_set_int(vm,args[2].l,"beginIndex",0);rt_set_int(vm,args[2].l,"endIndex",0);
      if (field==0 && end>first) {
         rt_set_int(vm,args[2].l,"beginIndex",length.i+(int32_t)first);rt_set_int(vm,args[2].l,"endIndex",length.i+(int32_t)end);
      } else if (field==1 && end<n && s[end]=='.' && end+1<n) {
         rt_set_int(vm,args[2].l,"beginIndex",length.i+(int32_t)end+1);rt_set_int(vm,args[2].l,"endIndex",length.i+(int32_t)n);
      }
   }
   return sb_append_str(vm,args[1].l,&text,1,out);
}
/* Number subclasses can implement conversion without using boxed slots. */
static bool numfmt_format_object(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   struct dvm_class *cls=dvm_object_class(vm,args[0].l);
   if (!cls || !dvm__class_assignable(vm,cls,dvm_find_class(vm,"java/lang/Number"))) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","Cannot format given Object as a Number"); return false;
   }
   bool integral=!strcmp(cls->name,"java/lang/Long") || !strcmp(cls->name,"java/lang/Integer") ||
      !strcmp(cls->name,"java/lang/Short") || !strcmp(cls->name,"java/lang/Byte") ||
      !strcmp(cls->name,"java/util/concurrent/atomic/AtomicInteger") || !strcmp(cls->name,"java/util/concurrent/atomic/AtomicLong");
   if (dvm__class_assignable(vm,cls,dvm_find_class(vm,"java/math/BigInteger"))) {
      struct dvm_method *bits=dvm_find_method(vm,cls,"bitLength","()I"); union dvm_value length={0};
      if (!bits || !dvm_call(vm,bits,args[0].l,NULL,0,&length)) return false;
      integral=length.i<64;
   }
   struct dvm_method *convert=dvm_find_method(vm,cls,integral ? "longValue" : "doubleValue",integral ? "()J" : "()D");
   union dvm_value number={0};
   if (!convert || !dvm_call(vm,convert,args[0].l,NULL,0,&number)) return false;
   if (nargs==1) return numfmt_string(vm,self,number,integral,out);
   struct dvm_method *format=dvm_find_method(vm,dvm_object_class(vm,self),"format",integral ?
      "(JLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;" :
      "(DLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;");
   union dvm_value arguments[3]={number,args[1],args[2]};
   return format && dvm_call(vm,format,self,arguments,3,out);
}
/* Each factory result owns its symbols and digit settings. */
static bool numfmt_factory(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;
   union dvm_value symbols={0};
   if(!decsym_factory(vm,0,args,nargs,&symbols)) return false;
   dvm_pin(vm,symbols.l);
   dvm_ref format=dvm_new_object(vm,dvm_find_class(vm,"java/text/DecimalFormat"));
   if(!format) {dvm_unpin(vm,symbols.l);return false;}
   dvm_pin(vm,format);
   bool good=dvm_set_field(vm,format,"symbols","Ljava/text/DecimalFormatSymbols;",symbols);
   if(good) decfmt_apply(vm,format,rt_get_int(vm,symbols.l,"secondaryGroup")==2 ? "#,##,##0.###" : "#,##0.###");
   dvm_unpin(vm,format);dvm_unpin(vm,symbols.l);
   if(!good) return false;
   RETL(format);
}
static const struct rt_method rt_decimal_format[] = {
   SM("getInstance","()Ljava/text/NumberFormat;",numfmt_factory),
   SM("getInstance","(Ljava/util/Locale;)Ljava/text/NumberFormat;",numfmt_factory),
   SM("getNumberInstance","()Ljava/text/NumberFormat;",numfmt_factory),
   SM("getNumberInstance","(Ljava/util/Locale;)Ljava/text/NumberFormat;",numfmt_factory),
   M("<init>", "()V", decfmt_init),
   M("<init>", "(Ljava/lang/String;)V", decfmt_init),
   M("<init>", "(Ljava/lang/String;Ljava/text/DecimalFormatSymbols;)V", decfmt_init),
   M("applyPattern", "(Ljava/lang/String;)V", decfmt_applyPattern),
   M("toPattern", "()Ljava/lang/String;", decfmt_toPattern),
   M("format", "(D)Ljava/lang/String;", numfmt_primitive),
   M("format", "(J)Ljava/lang/String;", numfmt_primitive),
   M("format", "(DLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;", decfmt_buffer),
   M("format", "(JLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;", decfmt_buffer),
   M("format", "(Ljava/lang/Object;)Ljava/lang/String;", numfmt_format_object),
   M("format", "(Ljava/lang/Object;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;", numfmt_format_object),
   M("setMaximumFractionDigits", "(I)V", decfmt_set_max_frac),
   M("setMinimumFractionDigits", "(I)V", decfmt_set_min_frac),
   M("setGroupingUsed", "(Z)V", decfmt_set_grouping),
   M_END,
};

/* MPL-2.0. Included by dvm_runtime.c after NumberFormat.
 * ChoiceFormat intervals and quoting follow Android libcore. */
static dvm_ref text_from_utf16(struct dvm *vm,const uint16_t *units,size_t length)
{
   if (length>SIZE_MAX/3) { dvm__throw(vm,"java/lang/OutOfMemoryError","UTF-16 text"); return 0; }
   char *text=malloc(length ? length*3 : 1);
   if (!text) { dvm__throw(vm,"java/lang/OutOfMemoryError","UTF-16 text"); return 0; }
   size_t bytes=jcs_utf16_to_wtf8(units,length,text);
   dvm_ref result=dvm_new_string_n(vm,text,bytes); free(text); return result;
}
static uint16_t *text_to_utf16(struct dvm *vm,dvm_ref string,size_t *length)
{
   size_t bytes=0; const char *text=sdata(vm,string,&bytes);
   if (!text) { dvm__throw(vm,"java/lang/NullPointerException","text"); return NULL; }
   *length=jcs_wtf8_to_utf16(text,bytes,NULL);
   if (*length>SIZE_MAX/sizeof(uint16_t)) { dvm__throw(vm,"java/lang/OutOfMemoryError","UTF-16 text"); return NULL; }
   uint16_t *units=malloc(*length ? *length*sizeof(*units) : 1);
   if (!units) { dvm__throw(vm,"java/lang/OutOfMemoryError","UTF-16 text"); return NULL; }
   jcs_wtf8_to_utf16(text,bytes,units); return units;
}
static const struct rt_field rt_choice_fields[]={
   {"choiceLimits","[D"},{"choiceFormats","[Ljava/lang/String;"},F_END
};
static bool choice_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if (!args[0].l || !args[1].l) { dvm__throw(vm,"java/lang/NullPointerException","choices"); return false; }
   if (dvm_array_length(vm,args[0].l)!=dvm_array_length(vm,args[1].l)) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","Array and limit arrays must be of the same length"); return false;
   }
   union dvm_value limits={0},formats={0};
   if (!o_clone(vm,args[0].l,NULL,0,&limits) || !limits.l || !o_clone(vm,args[1].l,NULL,0,&formats) || !formats.l) return false;
   dvm_set_field(vm,self,"choiceLimits","[D",limits);
   dvm_set_field(vm,self,"choiceFormats","[Ljava/lang/String;",formats); RETV();
}
static bool choice_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   bool limits=!strcmp(dvm__builtin_method()->name,"getLimits"); union dvm_value field={0};
   dvm_get_field(vm,self,limits ? "choiceLimits" : "choiceFormats",limits ? "[D" : "[Ljava/lang/String;",&field);
   if (limits) return o_clone(vm,field.l,NULL,0,out);
   uint32_t length=dvm_array_length(vm,field.l);
   dvm_ref copy=dvm_new_array(vm,'L',"Ljava/lang/Object;",length);
   if (!copy) return false;
   if (length) memcpy(dvm_array_data(vm,copy),dvm_array_data(vm,field.l),(size_t)length*sizeof(dvm_ref));
   RETL(copy);
}
struct choice_entry { double limit; dvm_ref format; };
static bool choice_push(struct dvm *vm,struct choice_entry **entries,size_t *count,size_t *capacity,double limit,struct rt_bytes *text)
{
   if (*count==*capacity) {
      size_t cap=*capacity ? *capacity*2 : 8;
      if (cap<*capacity || cap>SIZE_MAX/sizeof(**entries)) goto memory;
      struct choice_entry *next=realloc(*entries,cap*sizeof(*next));
      if (!next) goto memory;
      *entries=next; *capacity=cap;
   }
   dvm_ref format=text_from_utf16(vm,(const uint16_t *)text->p,text->len/2);
   if (!format) return false;
   (*entries)[(*count)++]=(struct choice_entry){limit,format}; text->len=0; return true;
memory:
   dvm__throw(vm,"java/lang/OutOfMemoryError","choice entries"); return false;
}
static bool choice_apply(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs; (void)out;
   size_t length=0; uint16_t *pattern=text_to_utf16(vm,args[0].l,&length);
   if (!pattern) return false;
   struct rt_bytes segments[2]={{0}}; struct choice_entry *entries=NULL;
   size_t count=0,capacity=0; int part=0;
   double limit=0,previous=NAN; bool quoted=false,good=false;
   for (size_t i=0;i<length;++i) {
      uint16_t ch=pattern[i];
      if (ch=='\'') {
         if (i+1<length && pattern[i+1]==ch) ++i;
         else { quoted=!quoted; continue; }
      } else if (!quoted && (ch=='<' || ch=='#' || ch==0x2264)) {
         const uint16_t *units=(const uint16_t *)segments[0].p; size_t size=segments[0].len/2;
         if (!size) goto invalid;
         if (size==1 && units[0]==0x221e) limit=INFINITY;
         else if (size==2 && units[0]=='-' && units[1]==0x221e) limit=-INFINITY;
         else {
            union dvm_value parsed={0}; dvm_ref number=text_from_utf16(vm,units,size);
            if (!number) goto done;
            if (!fp_parse_string(vm,number,false,&parsed)) {
               struct dvm_class *exception=dvm_object_class(vm,dvm_exception(vm));
               if (!exception || strcmp(exception->name,"java/lang/NumberFormatException")) goto done;
               dvm_clear_exception(vm); goto invalid;
            }
            limit=parsed.d;
         }
         if (ch=='<' && isfinite(limit)) limit=nextafter(limit,INFINITY);
         if (limit<=previous) goto invalid;
         segments[0].len=0; part=1; continue;
      } else if (!quoted && ch=='|') {
         if (!choice_push(vm,&entries,&count,&capacity,limit,segments+1)) goto done;
         previous=limit; part=0; continue;
      }
      if (!bytes_append(segments+part,&ch,sizeof(ch))) goto memory;
   }
   if (part && !choice_push(vm,&entries,&count,&capacity,limit,segments+1)) goto done;
   if (count>UINT32_MAX) goto memory;
   dvm_ref limits=dvm_new_array(vm,'D',"D",(uint32_t)count),formats=dvm_new_array(vm,'L',"Ljava/lang/String;",(uint32_t)count);
   if (!limits || !formats) goto done;
   for (size_t i=0;i<count;++i) {
      ((double *)dvm_array_data(vm,limits))[i]=entries[i].limit;
      ((dvm_ref *)dvm_array_data(vm,formats))[i]=entries[i].format;
   }
   dvm_set_field(vm,self,"choiceLimits","[D",(union dvm_value){.l=limits});
   dvm_set_field(vm,self,"choiceFormats","[Ljava/lang/String;",(union dvm_value){.l=formats}); good=true; goto done;
memory:
   dvm__throw(vm,"java/lang/OutOfMemoryError","choice pattern"); goto done;
invalid:
   dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid ChoiceFormat intervals");
done:
   free(entries); free(pattern); free(segments[0].p); free(segments[1].p); return good;
}
static bool choice_format(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   double value=dvm__builtin_method()->sig[1]=='J' ? (double)args[0].j : args[0].d;
   union dvm_value limits={0},formats={0};
   dvm_get_field(vm,self,"choiceLimits","[D",&limits); dvm_get_field(vm,self,"choiceFormats","[Ljava/lang/String;",&formats);
   uint32_t count=dvm_array_length(vm,limits.l),index=0; const double *values=dvm_array_data(vm,limits.l);
   while (index<count && value>=values[index]) ++index;
   if (index) --index;
   if (!count) { dvm__throw(vm,"java/lang/ArrayIndexOutOfBoundsException","empty choices"); return false; }
   union dvm_value text={.l=((dvm_ref *)dvm_array_data(vm,formats.l))[index]};
   if (nargs==1) { if (!text.l) text.l=dvm__intern(vm,"null"); RETL(text.l); }
   if (!args[1].l) { dvm__throw(vm,"java/lang/NullPointerException","toAppendTo"); return false; }
   return sb_append_str(vm,args[1].l,&text,1,out);
}
static bool choice_next(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self; bool up=strcmp(dvm__builtin_method()->name,"previousDouble")!=0;
   if (nargs>1) up=args[1].i!=0;
   RETD(isnan(args[0].d) ? args[0].d : nextafter(args[0].d,up ? INFINITY : -INFINITY));
}
static bool choice_pattern(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value limits={0},formats={0};
   dvm_get_field(vm,self,"choiceLimits","[D",&limits); dvm_get_field(vm,self,"choiceFormats","[Ljava/lang/String;",&formats);
   struct rt_bytes buffer={0}; bool good=true; uint32_t count=dvm_array_length(vm,limits.l);
   for (uint32_t i=0;i<count && good;++i) {
      double limit=((double *)dvm_array_data(vm,limits.l))[i],less=nextafter(limit,-INFINITY);
      bool inclusive=fabs(remainder(limit,1))<fabs(remainder(less,1)); char number[64];
      if (!inclusive && isinf(limit)) strcpy(number,limit<0 ? "-\xe2\x88\x9e" : "\xe2\x88\x9e");
      else box_fp_str(number,sizeof number,inclusive ? limit : less,false);
      good=(!i || bytes_append(&buffer,"|",1)) && bytes_append(&buffer,number,strlen(number)) && bytes_append(&buffer,inclusive ? "#" : "<",1);
      dvm_ref text=((dvm_ref *)dvm_array_data(vm,formats.l))[i]; size_t length=0; const char *s=sdata(vm,text,&length);
      if (!s) { dvm__throw(vm,"java/lang/NullPointerException","choice text"); good=false; break; }
      bool quote=memchr(s,'<',length) || memchr(s,'#',length) || memchr(s,'|',length);
      for (size_t k=0;k+2<length && !quote;++k) quote=!memcmp(s+k,"\xe2\x89\xa4",3);
      if (quote) good=good && bytes_append(&buffer,"'",1);
      for (size_t k=0;k<length && good;++k) {
         good=bytes_append(&buffer,s+k,1);
         if (s[k]=='\'') good=good && bytes_append(&buffer,"'",1);
      }
      if (quote) good=good && bytes_append(&buffer,"'",1);
   }
   dvm_ref result=good ? dvm_new_string_n(vm,(const char *)buffer.p,buffer.len) : 0; free(buffer.p);
   if (!good && !dvm_exception(vm)) dvm__throw(vm,"java/lang/OutOfMemoryError","choice pattern");
   if (!result) return false;
   RETL(result);
}
static bool choice_clone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value clone={0},fields[2]={{0}},ignored={0};
   if (!o_clone(vm,self,args,nargs,&clone) || !clone.l) return false;
   dvm_get_field(vm,self,"choiceLimits","[D",fields); dvm_get_field(vm,self,"choiceFormats","[Ljava/lang/String;",fields+1);
   if (!choice_set(vm,clone.l,fields,2,&ignored)) return false;
   RETL(clone.l);
}
static const struct rt_field rt_parse_position_fields[]={
   {"index","I"},{"errorIndex","I"},F_END
};
static bool parse_position_field(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=dvm__builtin_method()->name;
   if (!strcmp(name,"<init>")) {
      rt_set_int(vm,self,"index",args[0].i); rt_set_int(vm,self,"errorIndex",-1); RETV();
   }
   const char *field=strstr(name,"Error") ? "errorIndex" : "index";
   if (nargs) { rt_set_int(vm,self,field,args[0].i); RETV(); }
   RETI(rt_get_int(vm,self,field));
}
static bool parse_position_equals(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   struct dvm_class *other=dvm_object_class(vm,args[0].l);
   RETI(other && dvm__class_assignable(vm,other,dvm_find_class(vm,"java/text/ParsePosition")) &&
         rt_get_int(vm,self,"index")==rt_get_int(vm,args[0].l,"index") &&
         rt_get_int(vm,self,"errorIndex")==rt_get_int(vm,args[0].l,"errorIndex"));
}
static bool parse_position_hash(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI((int32_t)(((uint32_t)rt_get_int(vm,self,"errorIndex")<<16)|(uint32_t)rt_get_int(vm,self,"index")));
}
static const struct rt_method rt_parse_position[]={
   M("<init>","(I)V",parse_position_field),M("getIndex","()I",parse_position_field),M("setIndex","(I)V",parse_position_field),
   M("getErrorIndex","()I",parse_position_field),M("setErrorIndex","(I)V",parse_position_field),
   M("equals","(Ljava/lang/Object;)Z",parse_position_equals),M("hashCode","()I",parse_position_hash),M_END
};
static bool choice_parse(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   dvm_ref position=nargs>1 ? args[1].l : dvm_new_object(vm,dvm_find_class(vm,"java/text/ParsePosition"));
   if (!position) { dvm__throw(vm,"java/lang/NullPointerException","position"); return false; }
   if (nargs==1) rt_set_int(vm,position,"errorIndex",-1);
   int32_t start=rt_get_int(vm,position,"index"),furthest=start;
   double best=NAN;
   union dvm_value formats={0},limits={0};
   dvm_get_field(vm,self,"choiceFormats","[Ljava/lang/String;",&formats); dvm_get_field(vm,self,"choiceLimits","[D",&limits);
   uint32_t count=dvm_array_length(vm,formats.l);
   struct dvm_object *source=dvm__obj(vm,args[0].l);
   if (count && !source) { dvm__throw(vm,"java/lang/NullPointerException","text"); return false; }
   if (count && !string_index(vm,source)) return false;
   for (uint32_t i=0;i<count;++i) {
      dvm_ref format=((dvm_ref *)dvm_array_data(vm,formats.l))[i];
      struct dvm_object *text=dvm__obj(vm,format);
      if (!text) { dvm__throw(vm,"java/lang/NullPointerException","choice text"); return false; }
      if (!string_index(vm,text)) return false;
      size_t n=text->string_length;
      bool match=start>=0 && (uint32_t)start<=source->string_length && n<=source->string_length-(uint32_t)start;
      for (size_t k=0;match && k<n;++k) {
         uint16_t a=source->string_units ? source->string_units[start+k] : (unsigned char)source->utf8[start+k];
         uint16_t b=text->string_units ? text->string_units[k] : (unsigned char)text->utf8[k];
         match=a==b;
      }
      if (match && (size_t)start+n>(size_t)furthest) {
         furthest=start+(int32_t)n; best=((double *)dvm_array_data(vm,limits.l))[i];
         if ((uint32_t)furthest==source->string_length) break;
      }
   }
   rt_set_int(vm,position,"index",furthest);
   if (furthest==start) rt_set_int(vm,position,"errorIndex",furthest);
   if (nargs==1 && !furthest) { dvm__throw(vm,"java/text/ParseException","Unparseable number"); return false; }
   RETL(box_make(vm,"Ljava/lang/Double;",(union dvm_value){.d=best}));
}
static bool choice_equals(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if (self==args[0].l) RETI(1);
   if (dvm_object_class(vm,self)!=dvm_object_class(vm,args[0].l)) RETI(0);
   union dvm_value limits[2]={{0}},formats[2]={{0}};
   for (int k=0;k<2;++k) {
      dvm_ref ref=k ? args[0].l : self;
      dvm_get_field(vm,ref,"choiceLimits","[D",limits+k); dvm_get_field(vm,ref,"choiceFormats","[Ljava/lang/String;",formats+k);
   }
   uint32_t count=dvm_array_length(vm,limits[0].l);
   if (count!=dvm_array_length(vm,limits[1].l)) RETI(0);
   for (uint32_t i=0;i<count;++i) {
      if (java_d2bits(((double *)dvm_array_data(vm,limits[0].l))[i],false)!=
          java_d2bits(((double *)dvm_array_data(vm,limits[1].l))[i],false)) RETI(0);
      dvm_ref a=((dvm_ref *)dvm_array_data(vm,formats[0].l))[i],b=((dvm_ref *)dvm_array_data(vm,formats[1].l))[i];
      if (a==b) continue;
      if (!a || !b) RETI(0);
      union dvm_value arg={.l=b},equal={0};
      struct dvm_method *method=dvm_find_method(vm,dvm_object_class(vm,a),"equals","(Ljava/lang/Object;)Z");
      if (!method || !dvm_call(vm,method,a,&arg,1,&equal)) return false;
      if (!equal.i) RETI(0);
   }
   RETI(1);
}
static bool choice_hash(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args; (void)nargs; union dvm_value formats={0};
   dvm_get_field(vm,self,"choiceFormats","[Ljava/lang/String;",&formats);
   uint32_t count=dvm_array_length(vm,formats.l); union dvm_value hash={0};
   if (count) {
      dvm_ref last=((dvm_ref *)dvm_array_data(vm,formats.l))[count-1];
      if (!last) { dvm__throw(vm,"java/lang/NullPointerException","choice text"); return false; }
      struct dvm_method *method=dvm_find_method(vm,dvm_object_class(vm,last),"hashCode","()I");
      if (!method || !dvm_call(vm,method,last,NULL,0,&hash)) return false;
   }
   RETI((int32_t)(count^(uint32_t)hash.i));
}
static const struct rt_method rt_choice[]={
   M("<init>","(Ljava/lang/String;)V",choice_apply),M("<init>","([D[Ljava/lang/String;)V",choice_set),
   M("applyPattern","(Ljava/lang/String;)V",choice_apply),M("toPattern","()Ljava/lang/String;",choice_pattern),
   M("setChoices","([D[Ljava/lang/String;)V",choice_set),M("getLimits","()[D",choice_get),M("getFormats","()[Ljava/lang/Object;",choice_get),
   M("format","(D)Ljava/lang/String;",numfmt_primitive),M("format","(J)Ljava/lang/String;",numfmt_primitive),
   M("format","(DLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;",choice_format),
   M("format","(JLjava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;",choice_format),
   M("clone","()Ljava/lang/Object;",choice_clone),
   M("equals","(Ljava/lang/Object;)Z",choice_equals),M("hashCode","()I",choice_hash),
   M("parse","(Ljava/lang/String;Ljava/text/ParsePosition;)Ljava/lang/Number;",choice_parse),
   M("parse","(Ljava/lang/String;)Ljava/lang/Number;",choice_parse),
   M("parseObject","(Ljava/lang/String;Ljava/text/ParsePosition;)Ljava/lang/Object;",choice_parse),
   M("parseObject","(Ljava/lang/String;)Ljava/lang/Object;",choice_parse),
   SM("nextDouble","(D)D",choice_next),SM("nextDouble","(DZ)D",choice_next),SM("previousDouble","(D)D",choice_next),M_END
};
/* MPL-2.0. MessageFormat's literal/index/type/style state machine.
 * Contract: Android libcore java/text/MessageFormat.applyPattern/makeFormat.
 * Formats are compiled once; repeated formatting need not parse the pattern. */

struct segment { uint16_t *data; size_t length, capacity; };
static bool append(struct segment *s, uint16_t ch)
{
   if (s->length == s->capacity) {
      size_t capacity=s->capacity ? s->capacity*2 : 32;
      if (capacity<s->capacity || capacity>SIZE_MAX/sizeof(*s->data)) return false;
      uint16_t *data=realloc(s->data,capacity*sizeof(*data));
      if (!data) return false;
      s->data=data; s->capacity=capacity;
   }
   s->data[s->length++]=ch;
   return true;
}
void luna_message_free(struct luna_message_pattern *p)
{
   for (size_t i=0;i<p->count;++i) free(p->elements[i].style);
   free(p->elements); free(p->literal);
   memset(p,0,sizeof(*p));
}
static bool argument_index(const struct segment *s, int32_t *out)
{
   size_t i=0;
   bool negative=false;
   if (s->length && (s->data[0]=='+' || s->data[0]=='-')) {
      negative=s->data[0]=='-'; ++i;
   }
   if (i==s->length) return false;
   int32_t value=0;
   for (;i<s->length;++i) {
      /* Integer.parseInt reads UTF-16 chars, including non-ASCII decimal
       * digits. Supplementary digits consist of surrogates and are rejected. */
      uint16_t ch=s->data[i];
      int digit=ch>='0' && ch<='9' ? ch-'0' :
         luna_uc_digit(ch) ? luna_uc_numeric_value(ch) : -1;
      if (digit<0 || digit>9 || value>(INT32_MAX-digit)/10) return false;
      value=value*10+digit;
   }
   if (negative && value) return false;
   *out=value; return true;
}
static int format_type(const struct segment *s)
{
   static const char *const types[]={"","number","date","time","choice"};
   size_t start=0,end=s->length;
   /* Java String.trim(), not Unicode whitespace. */
   while (start<end && s->data[start]<=0x20) ++start;
   while (end>start && s->data[end-1]<=0x20) --end;
   for (int type=0;type<5;++type) {
      if (strlen(types[type])!=end-start) continue;
      bool equal=true;
      for (size_t i=start;i<end;++i) {
         uint16_t ch=s->data[i];
         if (ch>='A' && ch<='Z') ch+='a'-'A';
         if (ch!=(unsigned char)types[type][i-start]) { equal=false; break; }
      }
      if (equal) return type;
   }
   return -1;
}
static enum luna_message_error add_element(struct luna_message_pattern *p,
                                            size_t *capacity, struct segment s[4])
{
   int32_t argument;
   if (!argument_index(s+1,&argument)) return LUNA_MESSAGE_ARGUMENT;
   int type=format_type(s+2);
   if (type<0) return LUNA_MESSAGE_TYPE;
   if (p->count==*capacity) {
      size_t cap=*capacity ? *capacity*2 : 8;
      if (cap<*capacity || cap>SIZE_MAX/sizeof(*p->elements)) return LUNA_MESSAGE_MEMORY;
      struct luna_message_element *elements=realloc(p->elements,cap*sizeof(*elements));
      if (!elements) return LUNA_MESSAGE_MEMORY;
      p->elements=elements; *capacity=cap;
   }
   p->elements[p->count++]=(struct luna_message_element){
      .offset=s[0].length,.argument=argument,.type=(enum luna_message_type)type,
      .style=s[3].data,.style_length=s[3].length
   };
   s[3]=(struct segment){0}; /* transfer ownership */
   s[1].length=s[2].length=0;
   return LUNA_MESSAGE_OK;
}
enum luna_message_error luna_message_compile(const uint16_t *text, size_t length,
                                             struct luna_message_pattern *out)
{
   if (!text) return LUNA_MESSAGE_NULL;
   struct luna_message_pattern compiled={0};
   struct segment segments[4]={{0}};
   size_t part=0,depth=0,capacity=0;
   bool quoted=false;
   enum luna_message_error error=LUNA_MESSAGE_OK;
   for (size_t i=0;i<length;++i) {
      uint16_t ch=text[i];
      if (!part) {
         if (ch=='\'') {
            if (i+1<length && text[i+1]=='\'') ++i;
            else { quoted=!quoted; continue; }
         } else if (ch=='{' && !quoted) { part=1; continue; }
      } else if (quoted) {
         if (ch=='\'') quoted=false;
      } else {
         if (ch==',') {
            if (part<3) { ++part; continue; }
         } else if (ch=='{') {
            ++depth;
         } else if (ch=='}') {
            if (!depth) {
               error=add_element(&compiled,&capacity,segments);
               if (error) goto done;
               part=0; continue;
            }
            --depth;
         } else if (ch==' ' && part==2 && !segments[2].length) {
            continue;
         } else if (ch=='\'') {
            quoted=true;
         }
      }
      if (!append(segments+part,ch)) { error=LUNA_MESSAGE_MEMORY; goto done; }
   }
   /* Match Android's state machine: an unclosed quote in literal text closes
    * implicitly; a simple unclosed format element is an error. Android also
    * discards an unfinished element with unmatched nested opening braces. */
   if (!depth && part) { error=LUNA_MESSAGE_BRACES; goto done; }
   compiled.literal=segments[0].data;
   compiled.literal_length=segments[0].length;
   segments[0]=(struct segment){0};
   luna_message_free(out);
   *out=compiled;
   compiled=(struct luna_message_pattern){0};
done:
   for (size_t i=0;i<4;++i) free(segments[i].data);
   luna_message_free(&compiled);
   return error;
}

/* MPL-2.0. MessageFormat runtime, included after the primitive Format classes.
 * Compiled syntax lives in ordinary VM Strings/arrays; no untraced C handles. */
static bool message_call(struct dvm *vm,dvm_ref self,const char *owner,const char *name,
                          const char *sig,const union dvm_value *args,int nargs,union dvm_value *out)
{
   struct dvm_class *cls=self ? dvm_object_class(vm,self) : dvm_find_class(vm,owner);
   struct dvm_method *method=cls ? dvm_find_method(vm,cls,name,sig) : NULL;
   if (!method) {dvm__throw(vm,"java/lang/NoSuchMethodError","%s.%s%s",cls ? cls->name : owner,name,sig);return false;}
   return dvm_call(vm,method,self,args,nargs,out);
}
static bool format_object_string(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref buffer=dvm_new_object(vm,dvm_find_class(vm,"java/lang/StringBuffer"));
   dvm_ref position=dvm_new_object(vm,dvm_find_class(vm,"java/text/FieldPosition"));
   union dvm_value ignored={0},rendered={0};
   if (!buffer || !position || !sb_init(vm,buffer,NULL,0,&ignored)) return false;
   union dvm_value a[3]={args[0],{.l=buffer},{.l=position}};
   if (!message_call(vm,self,NULL,"format","(Ljava/lang/Object;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;",a,3,&rendered)) return false;
   if (!rendered.l) {dvm__throw(vm,"java/lang/NullPointerException","format result");return false;}
   return message_call(vm,rendered.l,NULL,"toString","()Ljava/lang/String;",NULL,0,out);
}
static const struct rt_field rt_message_format_fields[]={
   {"pattern","Ljava/lang/String;"},{"offsets","[I"},{"argumentNumbers","[I"},
   {"formats","[Ljava/text/Format;"},{"locale","Ljava/util/Locale;"},F_END
};
static const char *const rt_format_ifaces[]={"Ljava/io/Serializable;","Ljava/lang/Cloneable;",NULL};
static int message_keyword(const uint16_t *style,size_t n,const char *const *keywords,size_t count)
{
   size_t begin=0;while(begin<n && style[begin]<=32) ++begin;
   while(n>begin && style[n-1]<=32) --n;
   for(size_t i=0;i<count;++i) {
      if(strlen(keywords[i])!=n-begin) continue;
      size_t k=begin;
      for(;k<n;++k) {
         uint16_t c=style[k];if(c>='A' && c<='Z') c+='a'-'A';
         if(c!=(unsigned char)keywords[i][k-begin]) break;
      }
      if(k==n) return (int)i;
   }
   return -1;
}
static bool message_subformat(struct dvm *vm,const struct luna_message_element *element,dvm_ref locale,union dvm_value *out)
{
   out->l=0;
   if(element->type==LUNA_MESSAGE_OBJECT) return true;
   const char *owner=NULL,*signature=NULL,*factory=NULL;
   union dvm_value a[2]={{0}};int nargs=0;
   if(element->type==LUNA_MESSAGE_NUMBER) {
      static const char *const styles[]={"","currency","percent","integer"};
      static const char *const factories[]={"getInstance","getCurrencyInstance","getPercentInstance","getIntegerInstance"};
      int style=message_keyword(element->style,element->style_length,styles,4);
      if(style>=0) {
         a[0].l=locale;
         return message_call(vm,0,"java/text/NumberFormat",factories[style],"(Ljava/util/Locale;)Ljava/text/NumberFormat;",a,1,out);
      }
      union dvm_value symbols={0};a[0].l=locale;
      if(!message_call(vm,0,"java/text/DecimalFormatSymbols","getInstance","(Ljava/util/Locale;)Ljava/text/DecimalFormatSymbols;",a,1,&symbols)) return false;
      owner="java/text/DecimalFormat";signature="(Ljava/lang/String;Ljava/text/DecimalFormatSymbols;)V";
      a[1]=symbols;nargs=2;
   } else if(element->type==LUNA_MESSAGE_DATE || element->type==LUNA_MESSAGE_TIME) {
      static const char *const styles[]={"","short","medium","long","full"};
      static const int values[]={2,3,2,1,0};
      int style=message_keyword(element->style,element->style_length,styles,5);
      if(style>=0) {
         factory=element->type==LUNA_MESSAGE_DATE ? "getDateInstance" : "getTimeInstance";
         a[0].i=values[style];a[1].l=locale;
         return message_call(vm,0,"java/text/DateFormat",factory,"(ILjava/util/Locale;)Ljava/text/DateFormat;",a,2,out);
      }
      owner="java/text/SimpleDateFormat";signature="(Ljava/lang/String;Ljava/util/Locale;)V";a[1].l=locale;nargs=2;
   } else {
      owner="java/text/ChoiceFormat";signature="(Ljava/lang/String;)V";nargs=1;
   }
   a[0].l=text_from_utf16(vm,element->style,element->style_length);
   dvm_ref object=dvm_new_object(vm,dvm_find_class(vm,owner));union dvm_value ignored={0};
   if(!a[0].l || !object || !message_call(vm,object,NULL,"<init>",signature,a,nargs,&ignored)) return false;
   out->l=object;return true;
}
static bool message_apply(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;(void)out;
   size_t length=0;uint16_t *input=text_to_utf16(vm,args[0].l,&length);
   if(!input) return false;
   struct luna_message_pattern pattern={0};
   enum luna_message_error error=luna_message_compile(input,length,&pattern);free(input);
   if(error) {
      dvm__throw(vm,error==LUNA_MESSAGE_MEMORY ? "java/lang/OutOfMemoryError" : "java/lang/IllegalArgumentException","MessageFormat pattern error %d",error);
      return false;
   }
   bool good=false;union dvm_value locale={0};
   if(pattern.count>INT32_MAX || pattern.literal_length>INT32_MAX) {dvm__throw(vm,"java/lang/OutOfMemoryError","MessageFormat pattern");goto done;}
   dvm_get_field(vm,self,"locale","Ljava/util/Locale;",&locale);
   dvm_ref literal=text_from_utf16(vm,pattern.literal,pattern.literal_length);
   dvm_ref offsets=dvm_new_array(vm,'I',"I",(uint32_t)pattern.count);
   dvm_ref arguments=dvm_new_array(vm,'I',"I",(uint32_t)pattern.count);
   dvm_ref formats=dvm_new_array(vm,'L',"Ljava/text/Format;",(uint32_t)pattern.count);
   if(!literal || !offsets || !arguments || !formats) goto done;
   for(size_t i=0;i<pattern.count;++i) {
      union dvm_value format={0};
      if(!message_subformat(vm,pattern.elements+i,locale.l,&format)) goto done;
      ((int32_t *)dvm_array_data(vm,offsets))[i]=(int32_t)pattern.elements[i].offset;
      ((int32_t *)dvm_array_data(vm,arguments))[i]=pattern.elements[i].argument;
      ((dvm_ref *)dvm_array_data(vm,formats))[i]=format.l;
   }
   dvm_set_field(vm,self,"pattern","Ljava/lang/String;",(union dvm_value){.l=literal});
   dvm_set_field(vm,self,"offsets","[I",(union dvm_value){.l=offsets});
   dvm_set_field(vm,self,"argumentNumbers","[I",(union dvm_value){.l=arguments});
   dvm_set_field(vm,self,"formats","[Ljava/text/Format;",(union dvm_value){.l=formats});good=true;
done:
   luna_message_free(&pattern);return good;
}
static bool message_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value locale={0};
   if(nargs>1) locale=args[1];else if(!loc_getDefault(vm,0,NULL,0,&locale)) return false;
   dvm_set_field(vm,self,"locale","Ljava/util/Locale;",locale);
   return message_apply(vm,self,args,1,out);
}
static bool message_locale(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(nargs) {dvm_set_field(vm,self,"locale","Ljava/util/Locale;",args[0]);RETV();}
   union dvm_value locale={0};dvm_get_field(vm,self,"locale","Ljava/util/Locale;",&locale);RETL(locale.l);
}
static bool message_formats(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=dvm__builtin_method()->name;
   bool by_argument=strstr(name,"ByArgumentIndex")!=NULL;
   union dvm_value formats={0},indices={0};
   dvm_get_field(vm,self,"formats","[Ljava/text/Format;",&formats);dvm_get_field(vm,self,"argumentNumbers","[I",&indices);
   uint32_t count=dvm_array_length(vm,formats.l);
   if(!nargs) {
      uint32_t length=count;
      if(by_argument) {
         int32_t maximum=-1;const int32_t *numbers=dvm_array_data(vm,indices.l);
         for(uint32_t i=0;i<count;++i) if(numbers[i]>maximum) maximum=numbers[i];
         if(maximum==INT32_MAX) {dvm__throw(vm,"java/lang/NegativeArraySizeException","MessageFormat argument indices");return false;}
         length=(uint32_t)(maximum+1);
      }
      dvm_ref copy=dvm_new_array(vm,'L',"Ljava/text/Format;",length);if(!copy) return false;
      for(uint32_t i=0;i<count;++i) {
         uint32_t index=by_argument ? (uint32_t)((int32_t *)dvm_array_data(vm,indices.l))[i] : i;
         ((dvm_ref *)dvm_array_data(vm,copy))[index]=((dvm_ref *)dvm_array_data(vm,formats.l))[i];
      }
      RETL(copy);
   }
   bool single=!strcmp(name,"setFormat") || !strcmp(name,"setFormatByArgumentIndex");
   if(single && !by_argument && (args[0].i<0 || (uint32_t)args[0].i>=count)) {
      dvm__throw(vm,"java/lang/ArrayIndexOutOfBoundsException","MessageFormat element index %d",args[0].i);return false;
   }
   if(!single && !args[0].l) {dvm__throw(vm,"java/lang/NullPointerException","formats");return false;}
   uint32_t length=single ? 0 : dvm_array_length(vm,args[0].l);
   for(uint32_t i=0;i<count;++i) {
      int32_t index=by_argument ? ((int32_t *)dvm_array_data(vm,indices.l))[i] : (int32_t)i;
      if(single) {if(index==args[0].i) ((dvm_ref *)dvm_array_data(vm,formats.l))[i]=args[1].l;}
      else if((uint32_t)index<length) ((dvm_ref *)dvm_array_data(vm,formats.l))[i]=((dvm_ref *)dvm_array_data(vm,args[0].l))[index];
   }
   RETV();
}
static bool message_buffer(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if(!args[1].l) {dvm__throw(vm,"java/lang/NullPointerException","result");return false;}
   struct dvm_object *array=dvm__obj(vm,args[0].l);
   if(array && (array->kind!=DVM_OBJ_ARRAY || array->elem_kind!='L')) {dvm__throw(vm,"java/lang/ClassCastException","MessageFormat requires Object[]");return false;}
   union dvm_value fields[5]={{0}};
   const char *const names[]={"pattern","offsets","argumentNumbers","formats","locale"};
   const char *const types[]={"Ljava/lang/String;","[I","[I","[Ljava/text/Format;","Ljava/util/Locale;"};
   for(int i=0;i<5;++i) {dvm_get_field(vm,self,names[i],types[i],fields+i);dvm_pin(vm,fields[i].l);}
   size_t length=0;uint16_t *literal=text_to_utf16(vm,fields[0].l,&length);
   char *scratch=literal && length<=SIZE_MAX/3 ? malloc(length ? length*3 : 1) : NULL;
   bool good=false;
   if(!literal || !scratch) {if(!dvm_exception(vm)) dvm__throw(vm,"java/lang/OutOfMemoryError","MessageFormat literal");goto done;}
   uint32_t count=dvm_array_length(vm,fields[1].l);size_t previous=0;
   for(uint32_t i=0;i<count;++i) {
      size_t offset=(uint32_t)((int32_t *)dvm_array_data(vm,fields[1].l))[i];
      size_t n=jcs_utf16_to_wtf8(literal+previous,offset-previous,scratch);
      sb_append(vm,args[1].l,scratch,n);if(dvm_exception(vm)) goto done;previous=offset;
      int32_t index=((int32_t *)dvm_array_data(vm,fields[2].l))[i];
      if(!args[0].l || (uint32_t)index>=dvm_array_length(vm,args[0].l)) {
         char missing[32];int bytes=snprintf(missing,sizeof missing,"{%d}",index);sb_append(vm,args[1].l,missing,(size_t)bytes);
         if(dvm_exception(vm)) goto done;
         continue;
      }
      dvm_ref item=((dvm_ref *)dvm_array_data(vm,args[0].l))[index];
      union dvm_value text={0};dvm_ref format=((dvm_ref *)dvm_array_data(vm,fields[3].l))[i];
      if(!item) text.l=dvm__intern(vm,"null");
      else {
         struct dvm_class *cls=dvm_object_class(vm,item);
         if(!format && dvm__class_assignable(vm,cls,dvm_find_class(vm,"java/lang/Number"))) {
            union dvm_value locale=fields[4],made={0};
            if(!message_call(vm,0,"java/text/NumberFormat","getInstance","(Ljava/util/Locale;)Ljava/text/NumberFormat;",&locale,1,&made)) goto done;
            format=made.l;
         } else if(!format && dvm__class_assignable(vm,cls,dvm_find_class(vm,"java/util/Date"))) {
            union dvm_value a[3]={{.i=3},{.i=3},fields[4]},made={0};
            if(!message_call(vm,0,"java/text/DateFormat","getDateTimeInstance","(IILjava/util/Locale;)Ljava/text/DateFormat;",a,3,&made)) goto done;
            format=made.l;
         }
         if(format) {
            union dvm_value one={.l=item};
            if(!message_call(vm,format,NULL,"format","(Ljava/lang/Object;)Ljava/lang/String;",&one,1,&text)) goto done;
            struct dvm_class *fc=dvm_object_class(vm,format);
            size_t bytes=0;const char *selected=sdata(vm,text.l,&bytes);
            if(dvm__class_assignable(vm,fc,dvm_find_class(vm,"java/text/ChoiceFormat")) && selected && memchr(selected,'{',bytes)) {
               dvm_ref nested=dvm_new_object(vm,dvm_find_class(vm,"java/text/MessageFormat"));
               union dvm_value init[2]={text,fields[4]},ignored={0};
               if(!nested || !message_call(vm,nested,NULL,"<init>","(Ljava/lang/String;Ljava/util/Locale;)V",init,2,&ignored)) goto done;
               union dvm_value nested_args[3]={args[0],{.l=args[1].l},{.l=0}};
               if(!message_call(vm,nested,NULL,"format","([Ljava/lang/Object;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;",nested_args,3,&ignored)) goto done;
               continue;
            }
         } else {
            union dvm_value one={.l=item};if(!s_valueOf_obj(vm,0,&one,1,&text)) goto done;
         }
      }
      if(!text.l) text.l=dvm__intern(vm,"null");
      if(!sb_append_str(vm,args[1].l,&text,1,&(union dvm_value){0})) goto done;
   }
   sb_append(vm,args[1].l,scratch,jcs_utf16_to_wtf8(literal+previous,length-previous,scratch));
   good=!dvm_exception(vm);
done:
   free(scratch);free(literal);for(int i=0;i<5;++i) dvm_unpin(vm,fields[i].l);
   if(!good) return false;
   RETL(args[1].l);
}
static bool message_static_format(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   dvm_ref format=dvm_new_object(vm,dvm_find_class(vm,"java/text/MessageFormat"));union dvm_value ignored={0};
   if(!format || !message_init(vm,format,args,1,&ignored)) return false;
   return format_object_string(vm,format,args+1,1,out);
}
static bool message_clone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value copy={0};if(!o_clone(vm,self,args,nargs,&copy) || !copy.l) return false;
   const char *const fields[]={"offsets","argumentNumbers","formats"};const char *const types[]={"[I","[I","[Ljava/text/Format;"};
   for(int k=0;k<3;++k) {
      union dvm_value source={0},array={0};dvm_get_field(vm,self,fields[k],types[k],&source);
      if(!o_clone(vm,source.l,NULL,0,&array) || !array.l) return false;
      dvm_set_field(vm,copy.l,fields[k],types[k],array);
      if(k==2) for(uint32_t i=0;i<dvm_array_length(vm,array.l);++i) {
         dvm_ref item=((dvm_ref *)dvm_array_data(vm,array.l))[i];if(!item) continue;
         union dvm_value cloned={0};if(!message_call(vm,item,NULL,"clone","()Ljava/lang/Object;",NULL,0,&cloned)) return false;
         ((dvm_ref *)dvm_array_data(vm,array.l))[i]=cloned.l;
      }
   }
   RETL(copy.l);
}
static const struct rt_method rt_message_format[]={
   M("<init>","(Ljava/lang/String;)V",message_init),M("<init>","(Ljava/lang/String;Ljava/util/Locale;)V",message_init),
   M("applyPattern","(Ljava/lang/String;)V",message_apply),M("getLocale","()Ljava/util/Locale;",message_locale),M("setLocale","(Ljava/util/Locale;)V",message_locale),
   M("getFormats","()[Ljava/text/Format;",message_formats),M("getFormatsByArgumentIndex","()[Ljava/text/Format;",message_formats),
   M("setFormats","([Ljava/text/Format;)V",message_formats),M("setFormatsByArgumentIndex","([Ljava/text/Format;)V",message_formats),
   M("setFormat","(ILjava/text/Format;)V",message_formats),M("setFormatByArgumentIndex","(ILjava/text/Format;)V",message_formats),
   M("format","([Ljava/lang/Object;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;",message_buffer),
   M("format","(Ljava/lang/Object;Ljava/lang/StringBuffer;Ljava/text/FieldPosition;)Ljava/lang/StringBuffer;",message_buffer),
   SM("format","(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;",message_static_format),
   M("clone","()Ljava/lang/Object;",message_clone),M_END
};

