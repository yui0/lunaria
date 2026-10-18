/* VM binding for the C Html converter. Parsed ranges are UTF-16 offsets. */
#include "luna-ui/luna-ui.h"
static bool html_span_value(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const char *cls=dvm__builtin_method()->cls->name;
 const char *field=!strcmp(cls,"android/text/style/StyleSpan")?"style":
  !strcmp(cls,"android/text/style/RelativeSizeSpan")?"proportion":
  !strcmp(cls,"android/text/style/TypefaceSpan")?"family":
  !strcmp(cls,"android/text/style/URLSpan")?"url":
  !strcmp(cls,"android/text/style/BackgroundColorSpan")?"color":"alignment";
 const char *type=!strcmp(field,"proportion")?"F":
  (!strcmp(field,"family")||!strcmp(field,"url"))?"Ljava/lang/String;":
  !strcmp(field,"alignment")?"Landroid/text/Layout$Alignment;":"I";
 if(!strcmp(builtin_name(),"<init>")) {if(!dvm_set_field(vm,self,field,type,ARG(0)))return false;RETV();}
 (void)args;(void)nargs;return dvm_get_field(vm,self,field,type,out);
}
static bool html_span_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;dvm_ref paint=ARG(0).l;
 if(!paint){dvm__throw(vm,"java/lang/NullPointerException","paint");return false;}
 const char *cls=dvm__builtin_method()->cls->name;union dvm_value value={0},current={0};
 if(strstr(cls,"BackgroundColorSpan")){
  if(!dvm_get_field(vm,self,"color","I",&value)||!dvm_set_field(vm,paint,"bgColor","I",value)) {return false;}
  RETV();
 }
 if(strstr(cls,"RelativeSizeSpan")){
  if(!dvm_get_field(vm,self,"proportion","F",&value)||!am_vcall(vm,paint,"getTextSize","()F",NULL,0,&current))return false;
  value.f*=current.f;return am_vcall(vm,paint,"setTextSize","(F)V",&value,1,out);
 }
 if(strstr(cls,"SuperscriptSpan")||strstr(cls,"SubscriptSpan")){
  if(!am_vcall(vm,paint,"ascent","()F",NULL,0,&value)||!dvm_get_field(vm,paint,"baselineShift","I",&current))return false;
  current.i+=(strstr(cls,"SuperscriptSpan")?1:-1)*(int32_t)(value.f/2.f);
  if(!dvm_set_field(vm,paint,"baselineShift","I",current)) {return false;}
  RETV();
 }
 if(strstr(cls,"StyleSpan")||strstr(cls,"TypefaceSpan")){
  union dvm_value old={0},oldstyle={0};
  if(!am_vcall(vm,paint,"getTypeface","()Landroid/graphics/Typeface;",NULL,0,&old))return false;
  if(old.l&&!am_vcall(vm,old.l,"getStyle","()I",NULL,0,&oldstyle))return false;
  bool family=strstr(cls,"TypefaceSpan")!=NULL;
  if(!dvm_get_field(vm,self,family?"family":"style",family?"Ljava/lang/String;":"I",&value))return false;
  int32_t want=family?oldstyle.i:oldstyle.i|value.i;
  union dvm_value create[]={family?value:old,{.i=want}},face={0};
  struct dvm_method *method=dvm_find_method(vm,dvm_find_class(vm,"android/graphics/Typeface"),"create",family?
   "(Ljava/lang/String;I)Landroid/graphics/Typeface;":"(Landroid/graphics/Typeface;I)Landroid/graphics/Typeface;");
  if(!method||!dvm_call(vm,method,0,create,2,&face)||!face.l)return false;
  if(!am_vcall(vm,face.l,"getStyle","()I",NULL,0,&value))return false;
  int32_t fake=want&~value.i;
  if(fake&1){if(!am_vcall(vm,paint,"getFlags","()I",NULL,0,&value))return false;value.i|=32;
   if(!am_vcall(vm,paint,"setFlags","(I)V",&value,1,&current))return false;}
  if(fake&2){value.f=-.25f;if(!am_vcall(vm,paint,"setTextSkewX","(F)V",&value,1,&current))return false;}
  return am_vcall(vm,paint,"setTypeface","(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;",&face,1,&current);
 }
 bool link=strstr(cls,"URLSpan")!=NULL;
 if(link){if(!dvm_get_field(vm,paint,"linkColor","I",&value)||!am_vcall(vm,paint,"setColor","(I)V",&value,1,&current))return false;}
 if(!am_vcall(vm,paint,"getFlags","()I",NULL,0,&value))return false;
 value.i|=strstr(cls,"StrikethroughSpan")?16:8;
 return am_vcall(vm,paint,"setFlags","(I)V",&value,1,out);
}
#define HTML_DRAW M("updateDrawState","(Landroid/text/TextPaint;)V",html_span_draw)
#define HTML_MEASURE M("updateMeasureState","(Landroid/text/TextPaint;)V",html_span_draw)
static const struct rt_method rt_html_metric[]={
 M("<init>","()V",empty_void),
 {"updateMeasureState","(Landroid/text/TextPaint;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},M_END};
static const struct rt_method rt_html_plain[]={M("<init>","()V",empty_void),HTML_DRAW,M_END};
static const struct rt_method rt_html_baseline[]={M("<init>","()V",empty_void),HTML_DRAW,HTML_MEASURE,M_END};
static const struct rt_field rt_html_textpaint_fields[]={{"baselineShift","I"},{"bgColor","I"},{"linkColor","I"},F_END};
#define HTML_VALUE_METHODS(ctor,getter,type) \
 M("<init>","(" ctor ")V",html_span_value),M(getter,"()" type,html_span_value)
static const struct rt_method rt_html_style[]={HTML_VALUE_METHODS("I","getStyle","I"),HTML_DRAW,HTML_MEASURE,M_END};
static const struct rt_method rt_html_size[]={HTML_VALUE_METHODS("F","getSizeChange","F"),HTML_DRAW,HTML_MEASURE,M_END};
static const struct rt_method rt_html_face[]={HTML_VALUE_METHODS("Ljava/lang/String;","getFamily","Ljava/lang/String;"),HTML_DRAW,HTML_MEASURE,M_END};
static const struct rt_method rt_html_url[]={HTML_VALUE_METHODS("Ljava/lang/String;","getURL","Ljava/lang/String;"),HTML_DRAW,M_END};
static const struct rt_method rt_html_background[]={HTML_VALUE_METHODS("I","getBackgroundColor","I"),HTML_DRAW,M_END};
static const struct rt_method rt_html_alignment[]={HTML_VALUE_METHODS("Landroid/text/Layout$Alignment;","getAlignment","Landroid/text/Layout$Alignment;"),M_END};
#undef HTML_VALUE_METHODS
#undef HTML_DRAW
#undef HTML_MEASURE
static const struct rt_field rt_html_size_fields[]={{"proportion","F"},F_END};
static const struct rt_field rt_html_face_fields[]={{"family","Ljava/lang/String;"},F_END};
static const struct rt_field rt_html_url_fields[]={{"url","Ljava/lang/String;"},F_END};
static const struct rt_field rt_html_alignment_fields[]={{"alignment","Landroid/text/Layout$Alignment;"},F_END};
static const char *const rt_iface_alignment[]={"Landroid/text/style/AlignmentSpan;",NULL};
static const char *const rt_iface_paragraph[]={"Landroid/text/style/ParagraphStyle;",NULL};
static const struct rt_field rt_html_alignment_sfields[]={
 {"ALIGN_NORMAL","Landroid/text/Layout$Alignment;"},{"ALIGN_OPPOSITE","Landroid/text/Layout$Alignment;"},
 {"ALIGN_CENTER","Landroid/text/Layout$Alignment;"},{"ALIGN_LEFT","Landroid/text/Layout$Alignment;"},
 {"ALIGN_RIGHT","Landroid/text/Layout$Alignment;"},F_END};
static const struct rt_method rt_html_alignment_enum[]={
 SM("values","()[Landroid/text/Layout$Alignment;",builtin_enum_values),
 SM("valueOf","(Ljava/lang/String;)Landroid/text/Layout$Alignment;",builtin_enum_values),M_END};
static dvm_ref html_alignment(struct dvm *vm,unsigned value)
{
 const char *names[]={"ALIGN_NORMAL","ALIGN_CENTER","ALIGN_OPPOSITE"};
 union dvm_value result={0};
 if(value>2 || !dvm_get_static(vm,dvm_find_class(vm,"android/text/Layout$Alignment"),
    names[value],"Landroid/text/Layout$Alignment;",&result))return 0;
 return result.l;
}
static dvm_ref html_make_span(struct dvm *vm,const struct lh_span *span)
{
 const char *classes[]={"StyleSpan","StyleSpan","UnderlineSpan","StrikethroughSpan","SuperscriptSpan","SubscriptSpan",
  "RelativeSizeSpan","TypefaceSpan","URLSpan","ForegroundColorSpan","BackgroundColorSpan","BulletSpan","QuoteSpan","AlignmentSpan$Standard","ImageSpan"};
 if(span->kind==LH_IMAGE){dvm__throw(vm,"java/lang/UnsupportedOperationException","Html image drawable conversion is not implemented");return 0;}
 char name[96];snprintf(name,sizeof name,"android/text/style/%s",classes[span->kind]);
 dvm_ref result=dvm_new_object(vm,dvm_find_class(vm,name));if(!result)return 0;dvm_pin(vm,result);
 const char *field=NULL,*type="I";union dvm_value value={.u=span->value};
 switch(span->kind){
 case LH_BOLD:case LH_ITALIC:field="style";value.i=span->kind==LH_BOLD?1:2;break;
 case LH_SIZE:field="proportion";type="F";break;
 case LH_FACE:field="family";type="Ljava/lang/String;";break;
 case LH_URL:field="url";type="Ljava/lang/String;";break;
 case LH_FOREGROUND:case LH_BACKGROUND:field="color";break;
 case LH_ALIGNMENT:field="alignment";type="Landroid/text/Layout$Alignment;";value.l=html_alignment(vm,span->value);break;
 default:break;
 }
 if(span->kind==LH_FACE||span->kind==LH_URL)value.l=dvm_new_string(vm,span->text?span->text:"");
 bool good=!dvm_exception(vm)&&(!field||dvm_set_field(vm,result,field,type,value));
 dvm_unpin(vm,result);return good?result:0;
}
static bool html_from(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)self;
 const char *source=dvm_string_utf8(vm,ARG(0).l);
 if(!source){dvm__throw(vm,"java/lang/NullPointerException","source");return false;}
 struct lh_document parsed;
 if(!luna_html_parse(source,nargs>1?(unsigned)ARG(1).i:0,&parsed)){
  dvm__throw(vm,"java/lang/OutOfMemoryError","HTML conversion");return false;
 }
 if(parsed.length>SIZE_MAX/3){luna_html_document_free(&parsed);dvm__throw(vm,"java/lang/OutOfMemoryError","HTML text");return false;}
 char *bytes=malloc(parsed.length?parsed.length*3:1);
 if(!bytes){luna_html_document_free(&parsed);dvm__throw(vm,"java/lang/OutOfMemoryError","HTML text");return false;}
 size_t length=jcs_utf16_to_wtf8(parsed.text,parsed.length,bytes);
 dvm_ref string=dvm_new_string_n(vm,bytes,length);free(bytes);
 if(string)dvm_pin(vm,string);
 dvm_ref result=dvm_new_object(vm,dvm_find_class(vm,"android/text/SpannableStringBuilder"));
 if(!result||!string){if(string)dvm_unpin(vm,string);luna_html_document_free(&parsed);return false;}dvm_pin(vm,result);
 union dvm_value initial={.l=string},ignored={0};bool good=span_init(vm,result,&initial,1,&ignored);dvm_unpin(vm,string);
 for(size_t i=0;i<parsed.count&&good;++i){const struct lh_span *s=&parsed.spans[i];dvm_ref ref=html_make_span(vm,s);
  int flags=s->kind==LH_BULLET||s->kind==LH_QUOTE||s->kind==LH_ALIGNMENT?51:33;
  if(ref)dvm_pin(vm,ref);
  good=ref&&span_add(vm,result,ref,s->start,s->end,flags);
  if(ref)dvm_unpin(vm,ref);
 }
 luna_html_document_free(&parsed);dvm_unpin(vm,result);if(!good)return false;RETL(result);
}
static const struct rt_method rt_html[]={
 SM("fromHtml","(Ljava/lang/String;)Landroid/text/Spanned;",html_from),
 SM("fromHtml","(Ljava/lang/String;I)Landroid/text/Spanned;",html_from),M_END};
