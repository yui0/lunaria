/* MPL-2.0. Android widgets, Context/Activity services, layout and UI presentation.
 * Implementation section of dvm_runtime.c; shared helpers remain in that
 * translation unit. Keep registration tables in dvm_runtime.c. */

/* ------------------------------------------------------------------------ *
 * The clipboard
 * ------------------------------------------------------------------------ *
 *
 * android.content.ClipboardManager was named in the getSystemService table but
 * the class itself did not exist, so `getSystemService(CLIPBOARD_SERVICE)`
 * handed back an object with no methods on it — the same shape of hole
 * InputMethodManager used to have.  Copy and paste inside the app therefore
 * did nothing, and nothing could travel between the app and the desktop.
 *
 * The clipboard is system state, not manager state: on a device every app
 * reads and writes the one the system owns.  Here that one is the *host's*
 * clipboard (arm_exec_clipboard_set/get), which is also what the emulator's
 * own input method edits against — so text copied in the game can be pasted
 * into a host window and back.  getPrimaryClip() therefore builds a fresh
 * ClipData from what is on the clipboard *now* rather than returning something
 * this process remembers setting.
 *
 * The label is the one piece of a clip that the host clipboard cannot carry.
 * It is kept alongside the text it was set with, and offered only while the
 * clipboard still holds that text; once something else has been copied, the
 * clip genuinely is not the one that was labelled.
 */

/* A CharSequence as UTF-8.  A String answers directly; anything else — a
 * SpannableString, a StringBuilder, an app's own CharSequence — answers
 * through its own toString(), which is what coerceToText() does on a device. */
static const char *rt_charseq_utf8(struct dvm *vm, dvm_ref cs)
{
   if (!cs) return NULL;
   const char *s = dvm_string_utf8(vm, cs);
   if (s) return s;
   struct dvm_class *c = dvm_object_class(vm, cs);
   struct dvm_method *ts =
      c ? dvm_find_method(vm, c, "toString", "()Ljava/lang/String;") : NULL;
   union dvm_value ret;
   if (ts && dvm_call(vm, ts, cs, NULL, 0, &ret))
      return dvm_string_utf8(vm, ret.l);
   return NULL;
}

/* Text and spans stay in traced VM fields. Offsets are Java UTF-16 units. */
struct span_builder_buffer {
   uint16_t *chars;
   size_t length,capacity;
   unsigned watcher_depth;
   bool dirty;
};
static void span_builder_destroy(void *data)
{
   struct span_builder_buffer *buffer=data;
   if(buffer) {free(buffer->chars);free(buffer);}
}
static dvm_ref span_text(struct dvm *vm,dvm_ref self)
{
   struct dvm_object *object=dvm__obj(vm,self);
   struct span_builder_buffer *buffer=object && object->data_destroy==span_builder_destroy ? object->data : NULL;
   if(buffer && buffer->dirty) {
      if(buffer->length>SIZE_MAX/3) {dvm__throw(vm,"java/lang/OutOfMemoryError","Editable text");return 0;}
      char *bytes=malloc(buffer->length*3+1);
      if(!bytes) {dvm__throw(vm,"java/lang/OutOfMemoryError","Editable text");return 0;}
      size_t size=jcs_utf16_to_wtf8(buffer->chars,buffer->length,bytes);
      dvm_ref text=dvm_new_string_n(vm,bytes,size);free(bytes);
      if(!text) return 0;
      dvm_set_field(vm,self,"text","Ljava/lang/String;",(union dvm_value){.l=text});
      buffer->dirty=false;
   }
   union dvm_value value={0};
   (void)dvm_get_field(vm,self,"text","Ljava/lang/String;",&value); return value.l;
}
static void span_arrays(struct dvm *vm,dvm_ref self,dvm_ref **objects,int32_t **data,int32_t *count)
{
   union dvm_value refs={0},ranges={0},n={0};
   (void)dvm_get_field(vm,self,"spans","[Ljava/lang/Object;",&refs);
   (void)dvm_get_field(vm,self,"spanData","[I",&ranges);
   (void)dvm_get_field(vm,self,"spanCount","I",&n);
   *objects=dvm_array_data(vm,refs.l); *data=dvm_array_data(vm,ranges.l); *count=n.i;
}
static bool span_intersects(int32_t first,int32_t last,int32_t start,int32_t end)
{ return first<=end && last>=start && (first==last || start==end || (first!=end && last!=start)); }
static bool span_add(struct dvm *vm,dvm_ref self,dvm_ref what,int32_t start,int32_t end,int32_t flags)
{
   union dvm_value length={0};
   if (!s_length(vm,span_text(vm,self),NULL,0,&length)) return false;
   if (start<0 || end<start || end>length.i) {
      dvm__throw(vm,"java/lang/IndexOutOfBoundsException","span %d..%d length %d",start,end,length.i); return false;
   }
   if ((flags&0x33)==0x33) {
      int32_t indices[]={start,end};
      for(int k=0;k<2;++k) if(indices[k]>0 && indices[k]<length.i) {
         union dvm_value index={.i=indices[k]-1},ch={0};
         if(!s_charAt(vm,span_text(vm,self),&index,1,&ch)) return false;
         if(ch.i!='\n') { dvm__throw(vm,"java/lang/RuntimeException","paragraph span boundary"); return false; }
      }
   }
   struct dvm_class *cls=dvm_object_class(vm,self);
   if(start==end && (flags&0x33)==0x21 && cls
      && dvm__class_assignable(vm,cls,dvm_find_class(vm,"android/text/SpannableStringBuilder"))) return true;
   dvm_ref *objects; int32_t *data,n; span_arrays(vm,self,&objects,&data,&n);
   for(int i=0;i<n;++i) if(objects[i]==what) { data[i*3]=start;data[i*3+1]=end;data[i*3+2]=flags;return true; }
   union dvm_value refs={0}; (void)dvm_get_field(vm,self,"spans","[Ljava/lang/Object;",&refs);
   uint32_t capacity=dvm_array_length(vm,refs.l);
   if((uint32_t)n==capacity) {
      uint32_t next=capacity ? capacity*2 : 4;
      if(next<capacity || next>INT32_MAX/3) { dvm__throw(vm,"java/lang/OutOfMemoryError","spans");return false; }
      dvm_ref array=dvm_new_array(vm,'L',"Ljava/lang/Object;",next),ranges=dvm_new_array(vm,'I',"I",next*3);
      if(!array || !ranges) return false;
      if(n) { memcpy(dvm_array_data(vm,array),objects,(size_t)n*sizeof *objects);memcpy(dvm_array_data(vm,ranges),data,(size_t)n*3*sizeof *data); }
      (void)dvm_set_field(vm,self,"spans","[Ljava/lang/Object;",(union dvm_value){.l=array});
      (void)dvm_set_field(vm,self,"spanData","[I",(union dvm_value){.l=ranges});
      span_arrays(vm,self,&objects,&data,&n);
   }
   objects[n]=what;data[n*3]=start;data[n*3+1]=end;data[n*3+2]=flags;
   (void)dvm_set_field(vm,self,"spanCount","I",(union dvm_value){.i=n+1}); return true;
}
static bool span_query(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs; dvm_ref *objects;int32_t *data,n;span_arrays(vm,self,&objects,&data,&n);
   const char *method=builtin_name();
   if(!strcmp(method,"getSpanStart") || !strcmp(method,"getSpanEnd") || !strcmp(method,"getSpanFlags")) {
      int column=!strcmp(method,"getSpanStart") ? 0 : !strcmp(method,"getSpanEnd") ? 1 : 2;
      for(int i=n;i-->0;) if(objects[i]==ARG(0).l) RETI(data[i*3+column]);
      RETI(column==2 ? 0 : -1);
   }
   struct dvm_object *type=ARG(2).l ? dvm__obj(vm,ARG(2).l) : NULL;
   struct dvm_class *kind=type && type->kind==DVM_OBJ_CLASS ? type->klass : NULL;
   int32_t start=ARG(0).i,end=ARG(1).i;
   bool transition=!strcmp(method,"nextSpanTransition");
   uint32_t count=0;
   for(int i=0;i<n;++i) {
      if(kind && !dvm__class_assignable(vm,dvm_object_class(vm,objects[i]),kind)) continue;
      if(transition) {
         if(data[i*3]>start && data[i*3]<end) end=data[i*3];
         if(data[i*3+1]>start && data[i*3+1]<end) end=data[i*3+1];
      } else if(span_intersects(data[i*3],data[i*3+1],start,end)) ++count;
   }
   if(transition) RETI(end);
   dvm_ref result=dvm_new_array(vm,'L',kind ? kind->desc : "Ljava/lang/Object;",count);
   dvm_ref *dest=dvm_array_data(vm,result);
   if(!result) return false;
   uint32_t used=0;
   for(int i=0;i<n;++i) {
      if(!span_intersects(data[i*3],data[i*3+1],start,end) ||
         (kind && !dvm__class_assignable(vm,dvm_object_class(vm,objects[i]),kind))) continue;
      int priority=data[i*3+2]&0xff0000;
      uint32_t pos=used;
      for(uint32_t j=0;j<used;++j) {
         int previous=0;
         for(int k=0;k<n;++k) if(objects[k]==dest[j]) {previous=data[k*3+2]&0xff0000;break;}
         if(priority>previous) {pos=j;break;}
      }
      memmove(dest+pos+1,dest+pos,(used-pos)*sizeof *dest);dest[pos]=objects[i];++used;
   }
   RETL(result);
}
static bool span_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)out;
   dvm_ref source=nargs ? ARG(0).l : dvm_new_string(vm,"");
   if(!source) {dvm__throw(vm,"java/lang/NullPointerException","text");return false;}
   /* Strings are immutable: keep their complete storage, including NUL and
    * unpaired UTF-16 surrogates, instead of round-tripping through strlen. */
   dvm_ref text=source;
   struct dvm_object *string=dvm__obj(vm,text);
   if(!string || string->kind!=DVM_OBJ_STRING) {
      union dvm_value value={0};
      if(!am_vcall(vm,source,"toString","()Ljava/lang/String;",NULL,0,&value)) return false;
      text=value.l;string=dvm__obj(vm,text);
      if(!string || string->kind!=DVM_OBJ_STRING) {
         dvm__throw(vm,"java/lang/NullPointerException","CharSequence.toString");return false;
      }
   }
   union dvm_value length={0};
   if(!s_length(vm,text,NULL,0,&length)) return false;
   int32_t start=nargs==3 ? ARG(1).i : 0,end=nargs==3 ? ARG(2).i : length.i;
   if(start<0 || end<start || end>length.i) {
      dvm__throw(vm,"java/lang/IndexOutOfBoundsException","text %d..%d length %d",start,end,length.i);return false;
   }
   struct dvm_object *receiver=dvm__obj(vm,self);
   if(receiver && receiver->data_destroy==span_builder_destroy) {
      span_builder_destroy(receiver->data);receiver->data=NULL;receiver->data_destroy=NULL;
   }
   (void)dvm_set_field(vm,self,"spanCount","I",(union dvm_value){.i=0});
   union dvm_value range[]={{.i=start},{.i=end}},substring={0};
   if(!s_substring(vm,text,range,2,&substring)) return false;
   (void)dvm_set_field(vm,self,"text","Ljava/lang/String;",substring);
   struct dvm_class *spanned=dvm__class_by_desc(vm,"Landroid/text/Spanned;");
   struct dvm_class *cls=dvm_object_class(vm,source);
   if(cls && dvm__class_assignable(vm,cls,spanned)) {
      struct dvm_class *object=dvm__class_by_desc(vm,"Ljava/lang/Object;");
      dvm_ref classref=class_object_for(vm,object);
      union dvm_value query[]={{.i=start},{.i=end},{.l=classref}},items={0};
      struct dvm_method *get=dvm_find_method(vm,cls,"getSpans","(IILjava/lang/Class;)[Ljava/lang/Object;");
      if(!get || !dvm_call(vm,get,source,query,3,&items)) return false;
      uint32_t count=dvm_array_length(vm,items.l);
      for(uint32_t i=0;i<count;++i) {
         dvm_ref span=((dvm_ref *)dvm_array_data(vm,items.l))[i];
         if(dvm__class_assignable(vm,dvm_object_class(vm,self),dvm_find_class(vm,"android/text/SpannableStringBuilder"))
            && dvm__class_assignable(vm,dvm_object_class(vm,span),dvm_find_class(vm,"android/text/NoCopySpan"))) continue;
         const char *names[]={"getSpanStart","getSpanEnd","getSpanFlags"};int32_t data[3];
         for(int k=0;k<3;++k) {
            struct dvm_method *method=dvm_find_method(vm,cls,names[k],"(Ljava/lang/Object;)I");
            union dvm_value arg={.l=span},ret={0};
            if(!method || !dvm_call(vm,method,source,&arg,1,&ret)) return false;data[k]=ret.i;
         }
         int32_t first=data[0]<start ? start : data[0],last=data[1]>end ? end : data[1];
         if(!span_add(vm,self,span,first-start,last-start,data[2])) return false;
      }
   }
   RETV();
}
static bool span_value_of(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   struct dvm_class *kind=dvm__builtin_method()->cls;
   struct dvm_class *source=dvm_object_class(vm,ARG(0).l);
   if(source && dvm__class_assignable(vm,source,kind)) RETL(ARG(0).l);
   dvm_ref copy=dvm_new_object(vm,kind);union dvm_value ignored={0};
   if(!copy || !span_init(vm,copy,args,1,&ignored)) return false;
   RETL(copy);
}
static bool span_chars(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *method=builtin_name();
   struct dvm_object *object=dvm__obj(vm,self);
   struct span_builder_buffer *buffer=object && object->data_destroy==span_builder_destroy ? object->data : NULL;
   if(buffer && !strcmp(method,"length")) RETI((int32_t)buffer->length);
   if(buffer && !strcmp(method,"charAt")) {
      if(ARG(0).i<0 || (size_t)ARG(0).i>=buffer->length) {
         dvm__throw(vm,"java/lang/IndexOutOfBoundsException","charAt %d",ARG(0).i);return false;
      }
      RETI(buffer->chars[ARG(0).i]);
   }
   dvm_ref text=span_text(vm,self);if(!text) return false;
   if(!strcmp(method,"toString")) RETL(text);
   if(!strcmp(method,"length")) return s_length(vm,text,args,nargs,out);
   if(!strcmp(method,"charAt")) return s_charAt(vm,text,args,nargs,out);
   if(!strcmp(method,"getChars")) return s_getChars(vm,text,args,nargs,out);
   struct dvm_class *cls=dvm_object_class(vm,self);
   dvm_ref result=dvm_new_object(vm,cls);
   union dvm_value range[]={{.l=self},ARG(0),ARG(1)},ignored={0};
   if(!span_init(vm,result,range,3,&ignored)) return false;RETL(result);
}
static bool span_notify(struct dvm *,dvm_ref,dvm_ref,int,int,int,int);
static bool span_mutate(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref *objects;int32_t *data,n;span_arrays(vm,self,&objects,&data,&n);
   if(!strcmp(builtin_name(),"setSpan")) {
      int first=-1,last=-1;
      for(int i=0;i<n;++i) if(objects[i]==args[0].l) {first=data[i*3];last=data[i*3+1];break;}
      if(!span_add(vm,self,ARG(0).l,ARG(1).i,ARG(2).i,ARG(3).i)) return false;
      if(ARG(1).i==ARG(2).i && (ARG(3).i&0x33)==0x21
         && dvm__class_assignable(vm,dvm_object_class(vm,self),dvm_find_class(vm,"android/text/SpannableStringBuilder"))) RETV();
      span_arrays(vm,self,&objects,&data,&n);
      for(int i=0;i<n;++i) if(objects[i]==args[0].l) {
         if(!span_notify(vm,self,args[0].l,first,last,data[i*3],data[i*3+1])) return false;
         break;
      }
      RETV();
   }
   if(!strcmp(builtin_name(),"clearSpans")) {
      while(n>0) {
         dvm_ref what=objects[n-1];int first=data[(n-1)*3],last=data[(n-1)*3+1];
         objects[--n]=0;dvm_set_field(vm,self,"spanCount","I",(union dvm_value){.i=n});
         if(!span_notify(vm,self,what,first,last,-1,-1)) return false;
         span_arrays(vm,self,&objects,&data,&n);
      }
      RETV();
   }
   for(int i=0;i<n;++i) if(objects[i]==ARG(0).l) {
      int first=data[i*3],last=data[i*3+1];
      memmove(objects+i,objects+i+1,(size_t)(n-i-1)*sizeof *objects);
      memmove(data+i*3,data+(i+1)*3,(size_t)(n-i-1)*3*sizeof *data);--n;objects[n]=0;
      dvm_set_field(vm,self,"spanCount","I",(union dvm_value){.i=n});
      if(!span_notify(vm,self,ARG(0).l,first,last,-1,-1)) return false;
      RETV();
   }
   (void)dvm_set_field(vm,self,"spanCount","I",(union dvm_value){.i=n});RETV();
}
static bool annotation_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;(void)dvm_set_field(vm,self,"key","Ljava/lang/String;",ARG(0));
   (void)dvm_set_field(vm,self,"value","Ljava/lang/String;",ARG(1));RETV();
}
static bool annotation_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value value={0};
   (void)dvm_get_field(vm,self,!strcmp(builtin_name(),"getKey") ? "key" : "value","Ljava/lang/String;",&value);RETL(value.l);
}
/* Mutable UTF-16 backing and append transactions share the span storage above. */
static struct span_builder_buffer *span_builder_buffer(struct dvm *vm,dvm_ref self)
{
   struct dvm_object *object=dvm__obj(vm,self);
   if(!object) return NULL;
   if(object->data_destroy==span_builder_destroy) return object->data;
   dvm_ref text=span_text(vm,self);
   struct dvm_object *string=dvm__obj(vm,text);
   if(!string || !string_index(vm,string)) return NULL;
   struct span_builder_buffer *buffer=calloc(1,sizeof *buffer);
   if(buffer) {
      buffer->length=string->string_length;
      buffer->capacity=buffer->length<16 ? 16 : buffer->length;
      buffer->chars=malloc(buffer->capacity*sizeof *buffer->chars);
   }
   if(!buffer || !buffer->chars) {
      span_builder_destroy(buffer);dvm__throw(vm,"java/lang/OutOfMemoryError","Editable buffer");return NULL;
   }
   if(string->string_units) memcpy(buffer->chars,string->string_units,buffer->length*sizeof *buffer->chars);
   else {
      const unsigned char *bytes=(const unsigned char *)dvm_string_utf8(vm,text);
      for(size_t i=0;i<buffer->length;++i) buffer->chars[i]=bytes[i];
   }
   object->data=buffer;object->data_destroy=span_builder_destroy;
   return buffer;
}
static bool span_recipients(struct dvm *vm,dvm_ref self,int start,int end,const char *type,dvm_ref *recipients)
{
   dvm_ref klass=class_object_for(vm,dvm_find_class(vm,type));
   union dvm_value args[]={{.i=start},{.i=end},{.l=klass}},result={0};
   if(!klass || !am_vcall(vm,self,"getSpans","(IILjava/lang/Class;)[Ljava/lang/Object;",args,3,&result)) return false;
   *recipients=result.l;return true;
}
static bool span_text_notify(struct dvm *vm,dvm_ref self,dvm_ref watchers,int phase,int start,int count)
{
   struct span_builder_buffer *buffer=span_builder_buffer(vm,self);if(!buffer) return false;
   const char *names[]={"beforeTextChanged","onTextChanged","afterTextChanged"};
   const char *sig=phase==2 ? "(Landroid/text/Editable;)V" : "(Ljava/lang/CharSequence;III)V";
   union dvm_value args[]={{.l=self},{.i=start},{.i=0},{.i=count}},ignored={0};
   ++buffer->watcher_depth;
   bool good=true;
   for(uint32_t i=0;good && i<dvm_array_length(vm,watchers);++i) {
      dvm_ref watcher=((dvm_ref *)dvm_array_data(vm,watchers))[i];
      good=am_vcall(vm,watcher,names[phase],sig,args,phase==2 ? 1 : 4,&ignored);
   }
   --buffer->watcher_depth;return good;
}
static bool span_notify(struct dvm *vm,dvm_ref self,dvm_ref what,int oldstart,int oldend,int start,int end)
{
   bool added=oldstart<0,removed=start<0;
   int first=removed ? oldstart : added || start<oldstart ? start : oldstart;
   int last=removed ? oldend : added || end>oldend ? end : oldend;
   dvm_ref watchers=0;
   if(!span_recipients(vm,self,first,last,"android/text/SpanWatcher",&watchers)) return false;
   union dvm_value args[]={{.l=self},{.l=what},{.i=added ? start : oldstart},{.i=added ? end : oldend},{.i=start},{.i=end}},ignored={0};
   const char *name=added ? "onSpanAdded" : removed ? "onSpanRemoved" : "onSpanChanged";
   const char *sig=added || removed ? "(Landroid/text/Spannable;Ljava/lang/Object;II)V" : "(Landroid/text/Spannable;Ljava/lang/Object;IIII)V";
   for(uint32_t i=0;i<dvm_array_length(vm,watchers);++i) {
      dvm_ref watcher=((dvm_ref *)dvm_array_data(vm,watchers))[i];
      if(!am_vcall(vm,watcher,name,sig,args,added || removed ? 4 : 6,&ignored)) return false;
   }
   return true;
}
static bool span_builder_filters(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value value={0};
   if(!strcmp(builtin_name(),"setFilters")) {
      if(!args[0].l) {dvm__throw(vm,"java/lang/IllegalArgumentException","filters must not be null");return false;}
      dvm_set_field(vm,self,"filters","[Landroid/text/InputFilter;",args[0]);RETV();
   }
   dvm_get_field(vm,self,"filters","[Landroid/text/InputFilter;",&value);
   if(!value.l) {
      value.l=dvm_new_array(vm,'L',"Landroid/text/InputFilter;",0);if(!value.l) return false;
      dvm_set_field(vm,self,"filters","[Landroid/text/InputFilter;",value);
   }
   RETL(value.l);
}
static bool span_builder_depth(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;struct span_builder_buffer *buffer=span_builder_buffer(vm,self);
   if(!buffer) return false;RETI(buffer->watcher_depth);
}
struct span_append_notice {dvm_ref what;int start,end;};
static bool span_builder_reserve(struct dvm *vm,struct span_builder_buffer *buffer,size_t count)
{
   if(count>(size_t)INT32_MAX-buffer->length) {dvm__throw(vm,"java/lang/OutOfMemoryError","Editable length");return false;}
   if(buffer->length+count<=buffer->capacity) return true;
   size_t capacity=buffer->capacity;
   while(capacity<buffer->length+count) capacity=capacity>INT32_MAX/2 ? INT32_MAX : capacity*2;
   uint16_t *grown=realloc(buffer->chars,capacity*sizeof *grown);
   if(!grown) {dvm__throw(vm,"java/lang/OutOfMemoryError","Editable growth");return false;}
   buffer->chars=grown;buffer->capacity=capacity;return true;
}
static bool span_builder_append(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *sig=builtin_sig();
   bool character=sig[1]=='C',with_span=strstr(sig,";Ljava/lang/Object;")!=NULL;
   /* Without spans or filters, this exact framework class has no callbacks to
    * dispatch. Repeated subtitle characters then need no Java allocations. */
   if(character && !strcmp(dvm_object_class(vm,self)->name,"android/text/SpannableStringBuilder")) {
      union dvm_value spans={0},filters={0};
      dvm_get_field(vm,self,"spanCount","I",&spans);
      dvm_get_field(vm,self,"filters","[Landroid/text/InputFilter;",&filters);
      if(!spans.i && !dvm_array_length(vm,filters.l)) {
         struct span_builder_buffer *buffer=span_builder_buffer(vm,self);
         if(!buffer || !span_builder_reserve(vm,buffer,1)) return false;
         buffer->chars[buffer->length++]=(uint16_t)args[0].i;buffer->dirty=true;RETL(self);
      }
   }
   dvm_ref input=character ? 0 : args[0].l;
   if(character) {
      uint16_t unit=(uint16_t)args[0].i;char bytes[3];
      size_t n=jcs_utf16_to_wtf8(&unit,1,bytes);input=dvm_new_string_n(vm,bytes,n);
   }
   if(!input) {if(!dvm_exception(vm)) dvm__throw(vm,"java/lang/NullPointerException","text");return false;}
   union dvm_value length={0};
   if(!am_vcall(vm,input,"length","()I",NULL,0,&length)) return false;
   int first=nargs==3 && !with_span ? args[1].i : 0,last=nargs==3 && !with_span ? args[2].i : length.i;
   if(first<0 || last<first || last>length.i) {dvm__throw(vm,"java/lang/IndexOutOfBoundsException","append range");return false;}
   struct span_builder_buffer *buffer=span_builder_buffer(vm,self);if(!buffer) return false;
   int start=(int)buffer->length;
   union dvm_value filters={0};dvm_get_field(vm,self,"filters","[Landroid/text/InputFilter;",&filters);
   for(uint32_t i=0;i<dvm_array_length(vm,filters.l);++i) {
      dvm_ref filter=((dvm_ref *)dvm_array_data(vm,filters.l))[i];
      union dvm_value fa[]={{.l=input},{.i=first},{.i=last},{.l=self},{.i=start},{.i=start}},replacement={0};
      if(!am_vcall(vm,filter,"filter","(Ljava/lang/CharSequence;IILandroid/text/Spanned;II)Ljava/lang/CharSequence;",fa,6,&replacement)) return false;
      if(replacement.l) {
         input=replacement.l;first=0;
         if(!am_vcall(vm,input,"length","()I",NULL,0,&length)) return false;
         last=length.i;
      }
   }
   /* Snapshot input before changing self: appending self or its subsequence
    * must never read from a moving/reallocated destination. */
   dvm_ref source=0,text=0;
   if(dvm__class_assignable(vm,dvm_object_class(vm,input),dvm_find_class(vm,"android/text/Spanned"))) {
      source=dvm_new_object(vm,dvm_find_class(vm,"android/text/SpannedString"));
      union dvm_value slice[]={{.l=input},{.i=first},{.i=last}},ignored={0};
      if(!source || !span_init(vm,source,slice,3,&ignored)) return false;
      text=span_text(vm,source);
   } else {
      union dvm_value whole={0},slice[]={{.i=first},{.i=last}},part={0};
      if(!am_vcall(vm,input,"toString","()Ljava/lang/String;",NULL,0,&whole)
         || !s_substring(vm,whole.l,slice,2,&part)) return false;
      text=part.l;
   }
   struct dvm_object *string=dvm__obj(vm,text);
   if(!string || !string_index(vm,string)) return false;
   size_t count=string->string_length;
   dvm_ref *source_spans=NULL;int32_t *source_data=NULL,source_count=0;
   if(source) span_arrays(vm,source,&source_spans,&source_data,&source_count);
   bool adds_span=false;
   for(int i=0;i<source_count;++i) if((source_data[i*3+2]&0x33)!=0x21 || source_data[i*3]!=source_data[i*3+1]) adds_span=true;
   if(!count && !adds_span) {
      if(with_span) {
         union dvm_value span[]={{.l=args[1].l},{.i=start},{.i=start},args[2]},ignored={0};
         if(!am_vcall(vm,self,"setSpan","(Ljava/lang/Object;III)V",span,4,&ignored)) return false;
      }
      RETL(self);
   }
   dvm_ref watchers=0;
   if(!span_recipients(vm,self,start,start,"android/text/TextWatcher",&watchers)
      || !span_text_notify(vm,self,watchers,0,start,(int)count)) return false;
   buffer=span_builder_buffer(vm,self);if(!buffer) return false;
   if((size_t)start>buffer->length) {dvm__throw(vm,"java/lang/IndexOutOfBoundsException","append after watcher");return false;}
   if(count>(size_t)INT32_MAX-buffer->length) {dvm__throw(vm,"java/lang/OutOfMemoryError","Editable length");return false;}
   dvm_ref *objects;int32_t *data,n;span_arrays(vm,self,&objects,&data,&n);
   size_t notice_capacity=(size_t)n+(size_t)source_count;
   struct span_append_notice *notices=notice_capacity ? calloc(notice_capacity,sizeof *notices) : NULL;
   if(notice_capacity && !notices) {dvm__throw(vm,"java/lang/OutOfMemoryError","span notifications");return false;}
   for(int i=0;i<n;++i) notices[i]=(struct span_append_notice){objects[i],data[i*3],data[i*3+1]};
   int notices_count=n;
   if(!span_builder_reserve(vm,buffer,count)) {free(notices);return false;}
   memmove(buffer->chars+start+count,buffer->chars+start,(buffer->length-(size_t)start)*sizeof *buffer->chars);
   if(string->string_units) memcpy(buffer->chars+start,string->string_units,count*sizeof *buffer->chars);
   else {
      const unsigned char *bytes=(const unsigned char *)dvm_string_utf8(vm,text);
      for(size_t i=0;i<count;++i) buffer->chars[start+i]=bytes[i];
   }
   bool at_end=(size_t)start==buffer->length;
   buffer->length+=count;buffer->dirty=true;
   for(int i=0;i<n;++i) for(int k=0;k<2;++k) {
      int flag=k ? data[i*3+2]&15 : (data[i*3+2]>>4)&15;
      int *position=&data[i*3+k];
      if(*position>start || (*position==start && (flag==2 || (flag==3 && at_end)))) *position+=(int)count;
   }
   bool good=true;
   for(int i=0;good && i<source_count;++i) {
      span_arrays(vm,self,&objects,&data,&n);
      bool exists=false;for(int j=0;j<n;++j) if(objects[j]==source_spans[i]) {exists=true;break;}
      if(exists || (source_data[i*3]==source_data[i*3+1] && (source_data[i*3+2]&0x33)==0x21)) continue;
      if((source_data[i*3+2]&0x33)==0x33) {
         int a=start+source_data[i*3],b=start+source_data[i*3+1];
         if((a>0 && (size_t)a<buffer->length && buffer->chars[a-1]!='\n')
            || (b>0 && (size_t)b<buffer->length && buffer->chars[b-1]!='\n')) continue;
      }
      good=span_add(vm,self,source_spans[i],start+source_data[i*3],start+source_data[i*3+1],source_data[i*3+2]);
      if(good) notices[notices_count++]=(struct span_append_notice){source_spans[i],-1,-1};
   }
   if(good) good=span_text_notify(vm,self,watchers,1,start,(int)count)
      && span_text_notify(vm,self,watchers,2,start,(int)count);
   for(int i=0;good && i<notices_count;++i) {
      span_arrays(vm,self,&objects,&data,&n);
      for(int j=0;j<n;++j) if(objects[j]==notices[i].what) {
         if(data[j*3]!=notices[i].start || data[j*3+1]!=notices[i].end)
            good=span_notify(vm,self,objects[j],notices[i].start,notices[i].end,data[j*3],data[j*3+1]);
         break;
      }
   }
   free(notices);if(!good) return false;
   if(with_span) {
      union dvm_value span[]={{.l=args[1].l},{.i=start},{.i=(int)buffer->length},args[2]},ignored={0};
      if(!am_vcall(vm,self,"setSpan","(Ljava/lang/Object;III)V",span,4,&ignored)) return false;
   }
   RETL(self);
}
#define SPAN_APPEND_METHODS(result) \
 M("append","(C)" result,span_builder_append), \
 M("append","(Ljava/lang/CharSequence;)" result,span_builder_append), \
 M("append","(Ljava/lang/CharSequence;II)" result,span_builder_append)
#define SPAN_BUILDER_METHODS \
 SPAN_APPEND_METHODS("Landroid/text/SpannableStringBuilder;"), \
 SPAN_APPEND_METHODS("Landroid/text/Editable;"), \
 SPAN_APPEND_METHODS("Ljava/lang/Appendable;"), \
 M("append","(Ljava/lang/CharSequence;Ljava/lang/Object;I)Landroid/text/SpannableStringBuilder;",span_builder_append), \
 M("setFilters","([Landroid/text/InputFilter;)V",span_builder_filters), \
 M("getFilters","()[Landroid/text/InputFilter;",span_builder_filters), \
 M("getTextWatcherDepth","()I",span_builder_depth)
#define SPANNED_METHODS \
   M("getChars","(II[CI)V",span_chars), \
   M("length","()I",span_chars), M("charAt","(I)C",span_chars), \
   M("toString","()Ljava/lang/String;",span_chars), M("subSequence","(II)Ljava/lang/CharSequence;",span_chars), \
   M("getSpans","(IILjava/lang/Class;)[Ljava/lang/Object;",span_query), \
   M("getSpanStart","(Ljava/lang/Object;)I",span_query),M("getSpanEnd","(Ljava/lang/Object;)I",span_query), \
   M("getSpanFlags","(Ljava/lang/Object;)I",span_query),M("nextSpanTransition","(IILjava/lang/Class;)I",span_query)
static const struct rt_method rt_spanned[]={
   SM("valueOf","(Ljava/lang/CharSequence;)Landroid/text/SpannedString;",span_value_of),
   M("<init>","(Ljava/lang/CharSequence;)V",span_init),SPANNED_METHODS,M_END
};
#define SPANNABLE_METHODS \
   M("<init>","()V",span_init),M("<init>","(Ljava/lang/CharSequence;)V",span_init), \
   M("<init>","(Ljava/lang/CharSequence;II)V",span_init),SPANNED_METHODS, \
   M("setSpan","(Ljava/lang/Object;III)V",span_mutate),M("removeSpan","(Ljava/lang/Object;)V",span_mutate), \
   M("clearSpans","()V",span_mutate)
static const struct rt_method rt_spannable[]={
   SM("valueOf","(Ljava/lang/CharSequence;)Landroid/text/SpannableString;",span_value_of),
   SPANNABLE_METHODS,M_END
};
static const struct rt_method rt_spannable_builder[]={SPANNABLE_METHODS,SPAN_BUILDER_METHODS,M_END};
#undef SPAN_BUILDER_METHODS
#undef SPAN_APPEND_METHODS
#undef SPANNABLE_METHODS
#undef SPANNED_METHODS
static const struct rt_field rt_spanned_fields[]={
   {"text","Ljava/lang/String;"},{"spans","[Ljava/lang/Object;"},{"spanData","[I"},{"spanCount","I"},
   {"filters","[Landroid/text/InputFilter;"},F_END
};
static const struct rt_method rt_annotation[]={
   M("<init>","(Ljava/lang/String;Ljava/lang/String;)V",annotation_init),
   M("getKey","()Ljava/lang/String;",annotation_get),M("getValue","()Ljava/lang/String;",annotation_get),M_END
};
/* CharacterStyle and ForegroundColorSpan keep Java identity and color bits;
 * applying the span dispatches through TextPaint just as framework code does. */
static bool character_style_underlying(struct dvm *vm,dvm_ref self,
   const union dvm_value *args,int nargs,union dvm_value *out)
{ (void)vm;(void)args;(void)nargs;RETL(self); }
static bool foreground_span_init(struct dvm *vm,dvm_ref self,
   const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value color=ARG(0);
   if (builtin_sig()[1]=='L' && !am_vcall(vm,ARG(0).l,"readInt","()I",NULL,0,&color)) return false;
   if(!dvm_set_field(vm,self,"color","I",color)) return false;RETV();
}
static bool foreground_span_method(struct dvm *vm,dvm_ref self,
   const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"describeContents")) RETI(0);
   if(!strcmp(name,"getSpanTypeId") || !strcmp(name,"getSpanTypeIdInternal")) RETI(2);
   union dvm_value color={0};
   if(!dvm_get_field(vm,self,"color","I",&color)) return false;
   if(!strcmp(name,"getForegroundColor")) { *out=color;return true; }
   return am_vcall(vm,ARG(0).l,!strcmp(name,"updateDrawState") ? "setColor" : "writeInt",
                  "(I)V",&color,1,out);
}
static const struct rt_method rt_character_style[]={
   M("<init>","()V",empty_void),
   M("getUnderlying","()Landroid/text/style/CharacterStyle;",character_style_underlying),
   {"updateDrawState","(Landroid/text/TextPaint;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},M_END
};
static const struct rt_method rt_foreground_span[]={
   M("<init>","(I)V",foreground_span_init),M("<init>","(Landroid/os/Parcel;)V",foreground_span_init),
   M("getForegroundColor","()I",foreground_span_method),
   M("getSpanTypeId","()I",foreground_span_method),M("getSpanTypeIdInternal","()I",foreground_span_method),
   M("describeContents","()I",foreground_span_method),
   M("updateDrawState","(Landroid/text/TextPaint;)V",foreground_span_method),
   M("writeToParcel","(Landroid/os/Parcel;I)V",foreground_span_method),
   M("writeToParcelInternal","(Landroid/os/Parcel;I)V",foreground_span_method),M_END
};
static const struct rt_field rt_foreground_span_fields[]={{"color","I"},F_END};
static const char *const rt_iface_foreground_span[]={"Landroid/text/style/UpdateAppearance;","Landroid/text/ParcelableSpan;",NULL};
static const char *const rt_iface_parcelable_span[]={"Landroid/os/Parcelable;",NULL};

static const struct rt_field rt_span_style_fields[]={{"style","I"},F_END};
static const struct rt_field rt_annotation_fields[]={{"key","Ljava/lang/String;"},{"value","Ljava/lang/String;"},F_END};
static const char *const rt_iface_spanned[]={"Landroid/text/Spanned;",NULL};
static const char *const rt_iface_spannable[]={"Landroid/text/Spannable;",NULL};
static const char *const rt_iface_editable[]={"Landroid/text/Spannable;","Ljava/lang/Appendable;",NULL};
static const char *const rt_iface_builder[]={"Landroid/text/Editable;","Landroid/text/GetChars;",NULL};
static const char *const rt_iface_no_copy[]={"Landroid/text/NoCopySpan;",NULL};
static const char *const rt_iface_text_charseq[]={"Ljava/lang/CharSequence;",NULL};

static dvm_ref resource_styled_text(struct dvm *vm,uint32_t id,const char *text)
{
   const char *tag=NULL;uint32_t first=0,last=0;
   int status=arm_exec_apk_resource_span(id,0,&tag,&first,&last);
   if(status==0) return dvm_new_string(vm,text);
   dvm_ref string=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/text/SpannedString;"));
   union dvm_value arg={.l=dvm_new_string(vm,text)},ignored={0};
   if(!span_init(vm,string,&arg,1,&ignored)) return 0;
   for(size_t i=0;status==1;++i) {
      dvm_ref span=0;
      if(!strncmp(tag,"annotation;",11)) {
         const char *key=tag+11;
         while(*key) {
            const char *end=strchr(key,';');if(!end) end=key+strlen(key);
            const char *equals=memchr(key,'=',(size_t)(end-key));
            if(equals) {
               span=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/text/Annotation;"));
               union dvm_value values[]={{.l=dvm_new_string_n(vm,key,(size_t)(equals-key))},
                  {.l=dvm_new_string_n(vm,equals+1,(size_t)(end-equals-1))}};
               if(!annotation_init(vm,span,values,2,&ignored) || !span_add(vm,string,span,(int32_t)first,(int32_t)last+1,33)) return 0;
            }
            key=*end ? end+1 : end;
         }
      } else {
         const char *type=!strcmp(tag,"b") || !strcmp(tag,"i") ? "Landroid/text/style/StyleSpan;" :
            !strcmp(tag,"u") ? "Landroid/text/style/UnderlineSpan;" : NULL;
         if(!type) {dvm__throw(vm,"java/lang/UnsupportedOperationException","resource style %s",tag);return 0;}
         span=dvm_new_object(vm,dvm__class_by_desc(vm,type));
         if(!strcmp(tag,"b") || !strcmp(tag,"i")) (void)dvm_set_field(vm,span,"style","I",(union dvm_value){.i=!strcmp(tag,"b") ? 1 : 2});
         if(!span_add(vm,string,span,(int32_t)first,(int32_t)last+1,33)) return 0;
      }
      status=arm_exec_apk_resource_span(id,i+1,&tag,&first,&last);
   }
   if(status<0) {dvm__throw(vm,"android/content/res/Resources$NotFoundException","invalid string styles #0x%08x",id);return 0;}
   return string;
}

static char *g_clip_label;        /* label of the clip this process last set */
static char *g_clip_label_text;   /* the text it was set with */

#define RT_MAX_CLIP_LISTENERS 8
static dvm_ref g_clip_listeners[RT_MAX_CLIP_LISTENERS];
static int     g_nclip_listeners;

static void clip_remember_label(const char *label, const char *text)
{
   free(g_clip_label);
   free(g_clip_label_text);
   g_clip_label = label ? strdup(label) : NULL;
   g_clip_label_text = text ? strdup(text) : NULL;
}

/* The label that belongs to what is on the clipboard right now, or NULL. */
static const char *clip_label_for(const char *text)
{
   if (!g_clip_label_text || !text) return NULL;
   return strcmp(g_clip_label_text, text) == 0 ? g_clip_label : NULL;
}

static dvm_ref clip_new_item(struct dvm *vm, const char *text)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/content/ClipData$Item;");
   dvm_ref item = c ? dvm_new_object(vm, c) : 0;
   if (item) {
      union dvm_value t = { .l = dvm_new_string(vm, text ? text : "") };
      (void)dvm_set_field(vm, item, "text", "Ljava/lang/CharSequence;", t);
   }
   return item;
}

static dvm_ref clip_new_description(struct dvm *vm, const char *label)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/content/ClipDescription;");
   dvm_ref d = c ? dvm_new_object(vm, c) : 0;
   if (d && label) {
      union dvm_value l = { .l = dvm_new_string(vm, label) };
      (void)dvm_set_field(vm, d, "label", "Ljava/lang/CharSequence;", l);
   }
   return d;
}

/* A one-item plain-text ClipData, the shape newPlainText() produces. */
static dvm_ref clip_new_plain(struct dvm *vm, const char *label, const char *text)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/content/ClipData;");
   dvm_ref clip = c ? dvm_new_object(vm, c) : 0;
   if (!clip) return 0;
   union dvm_value v = { .l = clip_new_description(vm, label) };
   (void)dvm_set_field(vm, clip, "description", "Landroid/content/ClipDescription;", v);
   v.l = coll_new_list(vm, "Ljava/util/ArrayList;");
   (void)dvm_set_field(vm, clip, "items", "Ljava/util/List;", v);
   dvm_ref item = clip_new_item(vm, text);
   if (v.l && item) {
      union dvm_value a = { .l = item }, ignored = { 0 };
      (void)list_add(vm, v.l, &a, 1, &ignored);
   }
   return clip;
}

static bool clipitem_getText(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value t = { 0 };
   (void)dvm_get_field(vm, self, "text", "Ljava/lang/CharSequence;", &t);
   RETL(t.l);
}

/* coerceToText() differs from getText() only for the item kinds this clipboard
 * cannot hold (a Uri to resolve, an Intent to render), so on a text item they
 * are the same answer — but it must never be null: callers use it precisely
 * because it promises a CharSequence. */
static bool clipitem_coerceToText(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value t = { 0 };
   (void)dvm_get_field(vm, self, "text", "Ljava/lang/CharSequence;", &t);
   RETL(t.l ? t.l : dvm_new_string(vm, ""));
}

static bool clipitem_init(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   union dvm_value t = { .l = nargs > 0 ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "text", "Ljava/lang/CharSequence;", t);
   RETV();
}

static bool clipitem_toString(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value t = { 0 };
   (void)dvm_get_field(vm, self, "text", "Ljava/lang/CharSequence;", &t);
   char buf[256];
   snprintf(buf, sizeof buf, "ClipData.Item { T:%s }",
            t.l ? sref(vm, t.l) : "null");
   RETL(dvm_new_string(vm, buf));
}

static const struct rt_field rt_clip_item_fields[] = {
   { "text", "Ljava/lang/CharSequence;" }, F_END
};

static const struct rt_method rt_clip_item[] = {
   M("<init>", "(Ljava/lang/CharSequence;)V", clipitem_init),
   M("getText", "()Ljava/lang/CharSequence;", clipitem_getText),
   M("coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;",
     clipitem_coerceToText),
   M("coerceToStyledText", "(Landroid/content/Context;)Ljava/lang/CharSequence;",
     clipitem_coerceToText),
   M("coerceToHtmlText", "(Landroid/content/Context;)Ljava/lang/String;",
     clipitem_coerceToText),
   /* This clipboard carries text.  A Uri or an Intent item is not something
    * the emulator drops on the floor — it is something no clip here ever
    * holds, and null is the answer the API defines for that. */
   M("getUri", "()Landroid/net/Uri;", ret_null),
   M("getIntent", "()Landroid/content/Intent;", ret_null),
   M("getHtmlText", "()Ljava/lang/String;", ret_null),
   M("toString", "()Ljava/lang/String;", clipitem_toString),
   M_END,
};

static bool clipdesc_getLabel(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value l = { 0 };
   (void)dvm_get_field(vm, self, "label", "Ljava/lang/CharSequence;", &l);
   RETL(l.l);
}

/* Every clip here is plain text, so the answer is decidable rather than
 * guessed: "text/plain" and the wildcards match, anything else does not. */
static bool clipdesc_hasMimeType(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *want = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!want) RETI(0);
   RETI(!strcmp(want, "text/plain") || !strcmp(want, "text/*") ||
        !strcmp(want, "*/*"));
}

static bool clipdesc_getMimeType(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(dvm_new_string(vm, "text/plain"));
}

static const struct rt_field rt_clip_desc_fields[] = {
   { "label", "Ljava/lang/CharSequence;" }, F_END
};

static const struct rt_method rt_clip_description[] = {
   M("getLabel", "()Ljava/lang/CharSequence;", clipdesc_getLabel),
   M("hasMimeType", "(Ljava/lang/String;)Z", clipdesc_hasMimeType),
   M("getMimeType", "(I)Ljava/lang/String;", clipdesc_getMimeType),
   M("getMimeTypeCount", "()I", ret_true),   /* one type: text/plain */
   M("toString", "()Ljava/lang/String;", clipdesc_getLabel),
   M_END,
};

static bool clipdata_newPlainText(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *label = rt_charseq_utf8(vm, ARG(0).l);
   const char *text = rt_charseq_utf8(vm, ARG(1).l);
   RETL(clip_new_plain(vm, label, text));
}

static dvm_ref clipdata_items(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "items", "Ljava/util/List;", &v);
   return v.l;
}

static bool clipdata_getItemCount(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref items = clipdata_items(vm, self);
   struct rt_list *l = items ? list_of(vm, items) : NULL;
   RETI(l ? (int32_t)l->size : 0);
}

static bool clipdata_getItemAt(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)nargs;
   dvm_ref items = clipdata_items(vm, self);
   struct rt_list *l = items ? list_of(vm, items) : NULL;
   int32_t i = ARG(0).i;
   if (!l || i < 0 || (uint32_t)i >= l->size) {
      dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "item %d", i);
      return false;
   }
   RETL(l->items[i]);
}

static bool clipdata_addItem(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   /* addItem(Item) and addItem(ContentResolver, Item): the item is the last
    * argument in both. */
   dvm_ref item = nargs > 0 ? ARG(nargs - 1).l : 0;
   dvm_ref items = clipdata_items(vm, self);
   if (items && item) {
      union dvm_value a = { .l = item }, ignored = { 0 };
      (void)list_add(vm, items, &a, 1, &ignored);
   }
   RETV();
}

static bool clipdata_getDescription(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value d = { 0 };
   (void)dvm_get_field(vm, self, "description",
                       "Landroid/content/ClipDescription;", &d);
   RETL(d.l);
}

static const struct rt_field rt_clip_data_fields[] = {
   { "description", "Landroid/content/ClipDescription;" },
   { "items", "Ljava/util/List;" },
   F_END
};

static const struct rt_method rt_clip_data[] = {
   SM("newPlainText",
      "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;",
      clipdata_newPlainText),
   M("getItemCount", "()I", clipdata_getItemCount),
   M("getItemAt", "(I)Landroid/content/ClipData$Item;", clipdata_getItemAt),
   M("addItem", "(Landroid/content/ClipData$Item;)V", clipdata_addItem),
   M("addItem", "(Landroid/content/ContentResolver;Landroid/content/ClipData$Item;)V",
     clipdata_addItem),
   M("getDescription", "()Landroid/content/ClipDescription;",
     clipdata_getDescription),
   M_END,
};

/* The text on the clipboard right now, as a fresh guest String. */
static dvm_ref clip_host_text(struct dvm *vm)
{
   char *text = arm_exec_clipboard_get();
   dvm_ref s = dvm_new_string(vm, text ? text : "");
   free(text);
   return s;
}

static void clip_notify_listeners(struct dvm *vm)
{
   for (int i = 0; i < g_nclip_listeners; ++i) {
      dvm_ref l = g_clip_listeners[i];
      struct dvm_class *c = l ? dvm_object_class(vm, l) : NULL;
      struct dvm_method *m =
         c ? dvm_find_method(vm, c, "onPrimaryClipChanged", "()V") : NULL;
      union dvm_value ret;
      if (m) (void)dvm_call(vm, m, l, NULL, 0, &ret);
   }
}

/* setPrimaryClip(ClipData) — the whole clip goes on the clipboard.  Multiple
 * items are joined with newlines, which is how coerceToText() renders a
 * multi-item clip on a device. */
static bool clipboard_setPrimaryClip(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref clip = ARG(0).l;
   if (!clip) { RETV(); }
   char joined[4096];
   size_t used = 0;
   joined[0] = '\0';
   dvm_ref items = clipdata_items(vm, clip);
   struct rt_list *l = items ? list_of(vm, items) : NULL;
   for (uint32_t i = 0; l && i < l->size; ++i) {
      union dvm_value t = { 0 };
      (void)dvm_get_field(vm, l->items[i], "text", "Ljava/lang/CharSequence;", &t);
      const char *s = rt_charseq_utf8(vm, t.l);
      if (!s) continue;
      int n = snprintf(joined + used, sizeof joined - used, "%s%s",
                       used ? "\n" : "", s);
      if (n < 0) break;
      used += (size_t)n;
      if (used >= sizeof joined) { used = sizeof joined - 1; break; }
   }
   union dvm_value d = { 0 };
   (void)dvm_get_field(vm, clip, "description",
                       "Landroid/content/ClipDescription;", &d);
   union dvm_value lab = { 0 };
   if (d.l)
      (void)dvm_get_field(vm, d.l, "label", "Ljava/lang/CharSequence;", &lab);
   clip_remember_label(lab.l ? rt_charseq_utf8(vm, lab.l) : NULL, joined);
   arm_exec_clipboard_set(joined);
   fprintf(stderr, "[clipboard] app copied %zu bytes\n", used);
   clip_notify_listeners(vm);
   RETV();
}

static bool clipboard_getPrimaryClip(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   char *text = arm_exec_clipboard_get();
   /* An empty clipboard has no primary clip — null is the documented answer
    * and the one callers test for. */
   if (!text || !*text) { free(text); RETL(0); }
   dvm_ref clip = clip_new_plain(vm, clip_label_for(text), text);
   free(text);
   RETL(clip);
}

static bool clipboard_getPrimaryClipDescription(struct dvm *vm, dvm_ref self,
                                                const union dvm_value *args,
                                                int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   char *text = arm_exec_clipboard_get();
   if (!text || !*text) { free(text); RETL(0); }
   dvm_ref d = clip_new_description(vm, clip_label_for(text));
   free(text);
   RETL(d);
}

static bool clipboard_hasPrimaryClip(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   char *text = arm_exec_clipboard_get();
   bool any = text && *text;
   free(text);
   RETI(any ? 1 : 0);
}

static bool clipboard_clear(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   arm_exec_clipboard_set("");
   clip_remember_label(NULL, NULL);
   clip_notify_listeners(vm);
   RETV();
}

/* The pre-API-11 text API.  It is the same clipboard, so these are the same
 * calls with the label left off. */
static bool clipboard_setText(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *text = rt_charseq_utf8(vm, ARG(0).l);
   clip_remember_label(NULL, text);
   arm_exec_clipboard_set(text ? text : "");
   fprintf(stderr, "[clipboard] app copied %zu bytes (setText)\n",
           text ? strlen(text) : 0u);
   clip_notify_listeners(vm);
   RETV();
}

static bool clipboard_getText(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(clip_host_text(vm));
}

static bool clipboard_addListener(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref l = ARG(0).l;
   if (!l) { RETV(); }
   for (int i = 0; i < g_nclip_listeners; ++i)
      if (g_clip_listeners[i] == l) { RETV(); }
   if (g_nclip_listeners < RT_MAX_CLIP_LISTENERS) {
      dvm_pin(vm, l);
      g_clip_listeners[g_nclip_listeners++] = l;
   }
   RETV();
}

static bool clipboard_removeListener(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref l = ARG(0).l;
   for (int i = 0; i < g_nclip_listeners; ++i) {
      if (g_clip_listeners[i] != l) continue;
      dvm_unpin(vm, l);
      for (int k = i; k + 1 < g_nclip_listeners; ++k)
         g_clip_listeners[k] = g_clip_listeners[k + 1];
      --g_nclip_listeners;
      break;
   }
   RETV();
}

static const struct rt_method rt_clipboard_manager[] = {
   M("setPrimaryClip", "(Landroid/content/ClipData;)V", clipboard_setPrimaryClip),
   M("getPrimaryClip", "()Landroid/content/ClipData;", clipboard_getPrimaryClip),
   M("getPrimaryClipDescription", "()Landroid/content/ClipDescription;",
     clipboard_getPrimaryClipDescription),
   M("hasPrimaryClip", "()Z", clipboard_hasPrimaryClip),
   M("clearPrimaryClip", "()V", clipboard_clear),
   M("addPrimaryClipChangedListener",
     "(Landroid/content/ClipboardManager$OnPrimaryClipChangedListener;)V",
     clipboard_addListener),
   M("removePrimaryClipChangedListener",
     "(Landroid/content/ClipboardManager$OnPrimaryClipChangedListener;)V",
     clipboard_removeListener),
   /* android.text.ClipboardManager, which this class extends. */
   M("setText", "(Ljava/lang/CharSequence;)V", clipboard_setText),
   M("getText", "()Ljava/lang/CharSequence;", clipboard_getText),
   M("hasText", "()Z", clipboard_hasPrimaryClip),
   M_END,
};

static const struct rt_method rt_accessibility_manager[] = {
   M("isEnabled", "()Z", ret_false),
   M("isTouchExplorationEnabled", "()Z", ret_false),
   M("sendAccessibilityEvent", "(Landroid/view/accessibility/AccessibilityEvent;)V",
     nop_void),
   M_END,
};

static bool telephony_jp(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{ (void)self; (void)args; (void)nargs; RETL(dvm_new_string(vm, "jp")); }

static bool telephony_empty(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{ (void)self; (void)args; (void)nargs; RETL(dvm_new_string(vm, "")); }

static bool telephony_lte(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(13); }

static bool telephony_ready(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(5); }

static const struct rt_method rt_telephony_manager[] = {
   M("getNetworkCountryIso", "()Ljava/lang/String;", telephony_jp),
   M("getSimCountryIso", "()Ljava/lang/String;", telephony_jp),
   M("getNetworkOperator", "()Ljava/lang/String;", telephony_empty),
   M("getNetworkOperatorName", "()Ljava/lang/String;", telephony_empty),
   M("getSimOperator", "()Ljava/lang/String;", telephony_empty),
   M("getSimOperatorName", "()Ljava/lang/String;", telephony_empty),
   M("getSimCountryIso", "()Ljava/lang/String;", telephony_empty),
   M("getNetworkType", "()I", telephony_lte),
   M("getDataNetworkType", "()I", telephony_lte),
   M("getSimState", "()I", telephony_ready),
   M("getPhoneType", "()I", ret_true),
   M_END,
};

static bool view_config_get(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   static dvm_ref instance;
   (void)self; (void)args; (void)nargs;
   RETL(runtime_singleton(vm, "Landroid/view/ViewConfiguration;", &instance));
}

static bool view_config_8(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(8); }

static bool view_config_50(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(50); }

static bool view_config_8000(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(8000); }

static bool view_config_factor(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETF(50.0f); }

/* The pre-density static getters.  They are deprecated but not gone, and a
 * library that computes a slop once at class-init time still reads them. */
static bool view_config_12(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(12); }

static bool view_config_16(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(16); }

static bool view_config_500_ms(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETJ(500); }

static bool view_config_10(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(10); }

static bool view_config_250(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(250); }

static bool view_config_300_ms(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(300); }

static bool view_config_3000_ms(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETJ(3000); }

static bool view_config_500_i(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(500); }

/* AOSP: two screens' worth of ARGB_8888 at the display size. */
/* The rest of ViewConfiguration's static getters, at AOSP's values.
 *
 * These are not decoration.  AppsFlyer's string obfuscator folds platform
 * constants — ViewConfiguration's timeouts, KeyEvent.getMaxKeyCode() — into
 * the key it decodes its own strings with, so a constant that is merely
 * plausible rather than right yields a wrong string, the SDK's own lookups
 * miss, and it retries: on the Cross Worlds loading screen it held the
 * interpreter at 95%% busy for the whole load.  Values from AOSP's
 * ViewConfiguration (TAP_TIMEOUT, DEFAULT_LONG_PRESS_TIMEOUT,
 * PRESSED_STATE_DURATION, JUMP_TAP_TIMEOUT, HOVER_TAP_TIMEOUT/SLOP). */
static bool view_config_64(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(64); }

static bool view_config_100(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(100); }

static bool view_config_150(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(150); }

static bool view_config_400(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(400); }

static bool view_config_20(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(20); }

static bool view_config_2000(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(2000); }

static bool view_config_friction(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETF(0.015f); }

static bool view_config_cache_size(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETI(2 * 1920 * 1080 * 4);
}

static const struct rt_method rt_view_configuration[] = {
   SM("get", "(Landroid/content/Context;)Landroid/view/ViewConfiguration;",
      view_config_get),
   SM("getTouchSlop", "()I", view_config_8),
   SM("getWindowTouchSlop", "()I", view_config_16),
   SM("getFadingEdgeLength", "()I", view_config_12),
   SM("getMinimumFlingVelocity", "()I", view_config_50),
   SM("getMaximumFlingVelocity", "()I", view_config_8000),
   SM("getGlobalActionKeyTimeout", "()J", view_config_500_ms),
   SM("getZoomControlsTimeout", "()J", view_config_3000_ms),
   SM("getMaximumDrawingCacheSize", "()I", view_config_cache_size),
   SM("getDoubleTapTimeout", "()I", view_config_300_ms),
   SM("getScrollBarSize", "()I", view_config_10),
   SM("getScrollBarFadeDuration", "()I", view_config_250),
   SM("getScrollDefaultDelay", "()I", view_config_300_ms),
   SM("getEdgeSlop", "()I", view_config_12),
   SM("getPressedStateDuration", "()I", view_config_64),
   /* AOSP's getKeyRepeatTimeout() is getLongPressTimeout(). */
   SM("getKeyRepeatTimeout", "()I", view_config_400),
   SM("getKeyRepeatDelay", "()I", view_config_50),
   M("getScaledTouchSlop", "()I", view_config_8),
   M("getScaledPagingTouchSlop", "()I", view_config_8),
   M("getScaledMinimumFlingVelocity", "()I", view_config_50),
   M("getScaledMaximumFlingVelocity", "()I", view_config_8000),
   M("getScaledHorizontalScrollFactor", "()F", view_config_factor),
   M("getScaledVerticalScrollFactor", "()F", view_config_factor),
   M("hasPermanentMenuKey", "()Z", ret_false),
   SM("getTapTimeout", "()I", view_config_100),
   SM("getLongPressTimeout", "()I", view_config_400),
   SM("getJumpTapTimeout", "()I", view_config_500_i),
   SM("getHoverTapTimeout", "()I", view_config_150),
   SM("getHoverTapSlop", "()I", view_config_20),
   SM("getScrollFriction", "()F", view_config_friction),
   SM("getDefaultActionModeHideDuration", "()I", view_config_2000),
   SM("getScrollBarDefaultDelayBeforeFade", "()I", view_config_300_ms),
   M_END,
};

static dvm_ref context_path_file(struct dvm *vm, const char *env_name,
                                 const char *fallback, const char *child)
{
   const char *base = getenv(env_name);
   if (!base || !*base) base = fallback;
   char guest[DVM_PATH_MAX];
   base=arm_exec_unmap_host_path(base,guest,sizeof guest);
   if(!base){dvm__throw(vm,"java/io/IOException","Path exceeds namespace limit");return 0;}
   char *path=child ? luna_java_file_resolve(base,child) : luna_java_file_normalize(base);
   if(!path){dvm__throw(vm,"java/lang/OutOfMemoryError","context path");return 0;}
   struct dvm_class *fc = dvm__class_by_desc(vm, "Ljava/io/File;");
   dvm_ref f = fc ? dvm_new_object(vm, fc) : 0;
   if (f) {
      union dvm_value v = { .l = dvm_new_string(vm, path) };
      (void)dvm_set_field(vm, f, "path", "Ljava/lang/String;", v);
      /* The system creates an app's own directories before handing them out
       * (getCacheDir, getExternalCacheDir, ...): callers StatFs them and write
       * into them without a mkdirs() of their own. */
      if (!child || !*child) {
         union dvm_value made = { 0 };
         (void)file_mkdir_impl(vm, f, true, &made);
      }
   }
   free(path);return f;
}

static bool context_get_database_path(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
   (void)self;
   const char *name = nargs > 0 ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   /* An absolute name is the path itself, as Context.getDatabasePath() has it. */
   if (name && name[0] == '/') {
      struct dvm_class *fc = dvm__class_by_desc(vm, "Ljava/io/File;");
      dvm_ref f = fc ? dvm_new_object(vm, fc) : 0;
      if (f) {
         union dvm_value v = { .l = dvm_new_string(vm, name) };
         (void)dvm_set_field(vm, f, "path", "Ljava/lang/String;", v);
      }
      RETL(f);
   }
   RETL(context_path_file(vm, "ANDROID_DATABASES_DIR",
                          "/tmp/lunaria-databases", name));
}

static bool context_get_files_dir(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(context_path_file(vm, "ANDROID_FILES_DIR", "/tmp/lunaria-files", NULL));
}

static bool context_get_data_dir(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   struct lunaria_android_package record;
   if (!lunaria_android_package_find(getenv("ANDROID_PACKAGE_NAME"), &record))
      RETL(0);
   struct dvm_class *fc = dvm__class_by_desc(vm, "Ljava/io/File;");
   dvm_ref file = fc ? dvm_new_object(vm, fc) : 0;
   if (file) {
      union dvm_value path = { .l = dvm_new_string(vm, record.data_dir) };
      (void)dvm_set_field(vm, file, "path", "Ljava/lang/String;", path);
   }
   RETL(file);
}

static bool context_get_cache_dir(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(context_path_file(vm, "ANDROID_CACHE_DIR", "/tmp/lunaria-cache", NULL));
}

static bool context_get_code_cache_dir(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(context_path_file(vm, "ANDROID_CODE_CACHE_DIR",
                          "/tmp/lunaria-code-cache", NULL));
}

static bool context_get_no_backup_dir(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(context_path_file(vm, "ANDROID_NO_BACKUP_DIR",
                          "/tmp/lunaria-no-backup", NULL));
}

/* A File[] holding exactly one entry: the shape getExternalFilesDirs(),
 * getExternalCacheDirs() and getObbDirs() answer with when there is a single
 * storage volume.  dvm_new_array() prefixes the '[' itself. */
static dvm_ref context_file_array(struct dvm *vm, dvm_ref file)
{
   dvm_ref a = dvm_new_array(vm, 'L', "Ljava/io/File;", 1);
   struct dvm_object *o = a ? dvm__obj(vm, a) : NULL;
   if (o) ((dvm_ref *)o->data)[0] = file;
   return a;
}

/* External (app-private shared) storage.  These were missing from the bytecode
 * Context entirely, so getExternalFilesDir(null) answered with the *internal*
 * files dir.  That is the directory UE hands the engine as GFilePathBase when
 * the manifest says UseExternalFilesDir, and it is where the launcher stages
 * the expansion — pointing the engine at the wrong root meant it found no
 * paks and no .uproject and gave up in PreInit.
 *
 * getExternalFilesDir(type) appends the media type when one is named, which is
 * the whole difference between the null and non-null forms on a device. */
static bool context_get_external_files_dir(struct dvm *vm, dvm_ref self,
                                           const union dvm_value *args,
                                           int nargs, union dvm_value *out)
{
   (void)self;
   const char *type = nargs > 0 && ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   RETL(context_path_file(vm, "ANDROID_EXTERNAL_FILES_DIR",
                          "/tmp/lunaria-external-files", type));
}

static bool context_get_external_files_dirs(struct dvm *vm, dvm_ref self,
                                            const union dvm_value *args,
                                            int nargs, union dvm_value *out)
{
   union dvm_value one = { 0 };
   if (!context_get_external_files_dir(vm, self, args, nargs, &one)) return false;
   RETL(context_file_array(vm, one.l));
}

static bool context_get_external_cache_dir(struct dvm *vm, dvm_ref self,
                                           const union dvm_value *args,
                                           int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(context_path_file(vm, "ANDROID_EXTERNAL_CACHE_DIR",
                          "/tmp/lunaria-ext-cache", NULL));
}

static bool context_get_external_cache_dirs(struct dvm *vm, dvm_ref self,
                                            const union dvm_value *args,
                                            int nargs, union dvm_value *out)
{
   union dvm_value one = { 0 };
   if (!context_get_external_cache_dir(vm, self, args, nargs, &one)) return false;
   RETL(context_file_array(vm, one.l));
}

/* Context.fileList(): the names in the files dir, as a String[] (never null —
 * an empty dir is an empty array). */
static bool context_file_list(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   const char *base = getenv("ANDROID_FILES_DIR");
   if (!base || !*base) base = "/tmp/lunaria-files";
   const char *names[1024];
   char *own[1024];
   uint32_t n = 0;
   luna_directory *d = luna_directory_open(base);
   luna_directory_entry *e;
   while (d && (e = luna_directory_read(d)) && n < 1024) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      own[n] = strdup(e->d_name);
      names[n] = own[n];
      ++n;
   }
   if (d) luna_directory_close(d);
   dvm_ref a = dvm_new_array(vm, 'L', "Ljava/lang/String;", n);
   struct dvm_object *o = a ? dvm__obj(vm, a) : NULL;
   for (uint32_t i = 0; i < n; ++i) {
      if (o) ((dvm_ref *)o->data)[i] = dvm_new_string(vm, names[i]);
      free(own[i]);
   }
   RETL(a);
}

static bool context_get_obb_dir(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(context_path_file(vm, "ANDROID_EXTERNAL_OBB_DIR",
                          "/tmp/lunaria-obb", NULL));
}

static bool context_get_obb_dirs(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   union dvm_value one = { 0 };
   if (!context_get_obb_dir(vm, self, args, nargs, &one)) return false;
   RETL(context_file_array(vm, one.l));
}

static bool context_get_package_resource_path(struct dvm *vm, dvm_ref self,
                                              const union dvm_value *args,
                                              int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   /* The APK the package was installed from — UE passes it to the engine so
    * the engine can open the expansion that lives inside it.  Same answer the
    * JNI side gives, so both views of the package agree. */
   RETL(dvm_new_string(vm, lunaria_android_apk_path()));
}

/* The process Application.  ActivityThread builds exactly one per process and
 * remembers it the moment its base Context is attached; cw_attachBaseContext()
 * below records it at that same point. */
static dvm_ref g_process_application;

/* Context.getApplicationContext() — the process Application, never the
 * receiver.  Every Context in a process hands back that one object, and callers
 * cast the result: Play Games' zzby.zzc() does
 * `(Application) context.getApplicationContext()`, so returning the Activity
 * threw ClassCastException and took PlayGamesSdk.initialize() (and with it the
 * whole Netmarble plugin init) down with it. */
static bool context_get_application_context(struct dvm *vm, dvm_ref self,
                                            const union dvm_value *args,
                                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *app = dvm__class_by_desc(vm, "Landroid/app/Application;");
   if (o && app && dvm__class_assignable(vm, o->cls, app)) RETL(self);
   if (g_process_application) RETL(g_process_application);
   /* An Activity carries the process Application even before one is attached. */
   union dvm_value v = { 0 };
   if (dvm_get_field(vm, self, "application", "Landroid/app/Application;", &v)
       && v.l)
      RETL(v.l);
   RETL(self);
}

static bool context_get_application_info(struct dvm *vm, dvm_ref self,
                                         const union dvm_value *args, int nargs,
                                         union dvm_value *out)
{
   (void)args; (void)nargs;
   /* An attached ContextImpl reads its LoadedApk; wrappers delegate to mBase.
    * Preserve the installed object's identity after process binding. */
   dvm_ref context = self;
   for (int depth = 0; context && depth < 16; ++depth) {
      union dvm_value loaded = {0}, info = {0}, base = {0};
      if (dvm_get_field(vm, context, "mPackageInfo", "Landroid/app/LoadedApk;", &loaded)
          && loaded.l && dvm_get_field(vm, loaded.l, "mApplicationInfo",
                                       "Landroid/content/pm/ApplicationInfo;", &info)
          && info.l) RETL(info.l);
      if (!dvm_get_field(vm, context, "mBase", "Landroid/content/Context;", &base)
          || base.l == context) break;
      context = base.l;
   }
   /* Android hands back the very object PackageManager holds for this
    * package, so Context.getApplicationInfo() and
    * PackageManager.getApplicationInfo(getPackageName(), 0) describe the same
    * install down to every field.  Building a second, thinner object here
    * left sourceDir, publicSourceDir, dataDir, nativeLibraryDir and flags
    * null while PackageManager filled them — and a native SDK that reads
    * sourceDir off this object (NMSS greps `pm list packages -f` for it)
    * then searched for an empty path. */
   struct lunaria_android_package record;
   if (lunaria_android_package_find(getenv("ANDROID_PACKAGE_NAME"), &record)) {
      dvm_ref pi = pm_package_info(vm, &record, 0);
      union dvm_value v = { 0 };
      if (pi && dvm_get_field(vm, pi, "applicationInfo",
                              "Landroid/content/pm/ApplicationInfo;", &v)
          && v.l)
         RETL(v.l);
   }
   struct dvm_class *ac = dvm__class_by_desc(vm,
      "Landroid/content/pm/ApplicationInfo;");
   dvm_ref ai = ac ? dvm_new_object(vm, ac) : 0;
   if (!ai) RETL(0);
   union dvm_value v = { .l = dvm_new_string(vm,
      getenv("ANDROID_PACKAGE_NAME") ?: "") };
   (void)dvm_set_field(vm, ai, "packageName", "Ljava/lang/String;", v);
   (void)dvm_set_field(vm, ai, "processName", "Ljava/lang/String;", v);
   v.i = arm_exec_guest_uid();
   (void)dvm_set_field(vm, ai, "uid", "I", v);
   v.i = lunaria_app_target_sdk();
   (void)dvm_set_field(vm, ai, "targetSdkVersion", "I", v);
   /* The application's own <meta-data>, as the installed record carries it. */
   v.l = dvm_runtime_manifest_bundle(vm);
   (void)dvm_set_field(vm, ai, "metaData", "Landroid/os/Bundle;", v);
   RETL(ai);
}

static bool context_get_attribution_tag(struct dvm *vm, dvm_ref self,
                                        const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value value = { 0 };
   (void)dvm_get_field(vm, self, "mBase", "Landroid/content/Context;", &value);
   if (value.l && value.l != self) {
      struct dvm_class *c = dvm_object_class(vm, value.l);
      struct dvm_method *m = c ? dvm_find_method(vm, c, "getAttributionTag",
                                                "()Ljava/lang/String;") : NULL;
      return m && dvm_call(vm, m, value.l, NULL, 0, out);
   }
   value.l = 0;
   (void)dvm_get_field(vm, self, "attributionTag", "Ljava/lang/String;", &value);
   RETL(value.l);
}

static bool context_get_attribution_source(struct dvm *vm, dvm_ref self,
                                           const union dvm_value *args, int nargs,
                                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value base = { 0 }, value = { 0 };
   (void)dvm_get_field(vm, self, "mBase", "Landroid/content/Context;", &base);
   if (base.l && base.l != self) {
      struct dvm_class *c = dvm_object_class(vm, base.l);
      struct dvm_method *m = c ? dvm_find_method(vm, c, "getAttributionSource",
                         "()Landroid/content/AttributionSource;") : NULL;
      return m && dvm_call(vm, m, base.l, NULL, 0, out);
   }
   (void)dvm_get_field(vm, self, "attributionSource",
                       "Landroid/content/AttributionSource;", &value);
   if (value.l) RETL(value.l);
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/content/AttributionSource;");
   dvm_ref source = c ? dvm_new_object(vm, c) : 0;
   if (!source) return false;
   value.i = arm_exec_guest_uid();
   (void)dvm_set_field(vm, source, "uid", "I", value);
   value.i = arm_exec_guest_pid();
   (void)dvm_set_field(vm, source, "pid", "I", value);
   struct dvm_class *binder = dvm__class_by_desc(vm, "Landroid/os/Binder;");
   union dvm_value cached_token = { 0 };
   (void)dvm_get_static(vm, c, "defaultToken", "Landroid/os/IBinder;", &cached_token);
   dvm_ref token = cached_token.l;
   if (!token) {
      token = binder ? dvm_new_object(vm, binder) : 0;
      struct dvm_method *init = binder ? dvm_find_method(vm, binder, "<init>", "(Ljava/lang/String;)V") : NULL;
      union dvm_value descriptor = { .l = dvm_new_string(vm, "android.content.AttributionSource") };
      if (!token || !init || !dvm_call(vm, init, token, &descriptor, 1, &value)) return false;
      cached_token.l = token;
      (void)dvm_set_static(vm, c, "defaultToken", "Landroid/os/IBinder;", cached_token);
   }
   value.l = token;
   (void)dvm_set_field(vm, source, "token", "Landroid/os/IBinder;", value);
   struct dvm_class *context = dvm_object_class(vm, self);
   const char *methods[] = { "getPackageName", "getAttributionTag" };
   const char *fields[] = { "packageName", "attributionTag" };
   for (int i = 0; i < 2; ++i) {
      struct dvm_method *m = context ? dvm_find_method(vm, context, methods[i],
                                      "()Ljava/lang/String;") : NULL;
      if (!m || !dvm_call(vm, m, self, NULL, 0, &value)) return false;
      (void)dvm_set_field(vm, source, fields[i], "Ljava/lang/String;", value);
   }
   value.l = source;
   (void)dvm_set_field(vm, self, "attributionSource",
                       "Landroid/content/AttributionSource;", value);
   RETL(source);
}

#define ATTR_GETTER(fn, field, type, ret, member) \
static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
                int nargs, union dvm_value *out) \
{ \
   (void)args; (void)nargs; \
   union dvm_value value = { 0 }; \
   (void)dvm_get_field(vm, self, field, type, &value); \
   ret(value.member); \
}
ATTR_GETTER(attribution_uid, "uid", "I", RETI, i)
ATTR_GETTER(attribution_pid, "pid", "I", RETI, i)
ATTR_GETTER(attribution_package, "packageName", "Ljava/lang/String;", RETL, l)
ATTR_GETTER(attribution_tag, "attributionTag", "Ljava/lang/String;", RETL, l)
ATTR_GETTER(attribution_next, "next", "Landroid/content/AttributionSource;", RETL, l)
ATTR_GETTER(attribution_token, "token", "Landroid/os/IBinder;", RETL, l)
#undef ATTR_GETTER

static const struct rt_field rt_attribution_fields[] = {
   { "uid", "I" }, { "pid", "I" },
   { "packageName", "Ljava/lang/String;" },
   { "attributionTag", "Ljava/lang/String;" },
   { "next", "Landroid/content/AttributionSource;" },
   { "token", "Landroid/os/IBinder;" }, F_END
};
static const struct rt_field rt_attribution_sfields[] = {
   { "defaultToken", "Landroid/os/IBinder;" }, F_END
};
static const struct rt_method rt_attribution[] = {
   M("getUid", "()I", attribution_uid),
   M("getPid", "()I", attribution_pid),
   M("getPackageName", "()Ljava/lang/String;", attribution_package),
   M("getAttributionTag", "()Ljava/lang/String;", attribution_tag),
   M("getNext", "()Landroid/content/AttributionSource;", attribution_next),
   M("getToken", "()Landroid/os/IBinder;", attribution_token),
   M_END
};
static const struct rt_field rt_context_fields[] = {
   { "keyguardManager", "Landroid/app/KeyguardManager;" },
   { "autofillManager", "Landroid/view/autofill/AutofillManager;" },
   { "contextResources", "Landroid/content/res/Resources;" },
   { "themeObject", "Landroid/content/res/Resources$Theme;" },
   { "layoutInflater", "Landroid/view/LayoutInflater;" },
   { "attributionSource", "Landroid/content/AttributionSource;" },
   { "attributionTag", "Ljava/lang/String;" }, F_END
};

static bool context_get_package_name(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   const char *pkg = getenv("ANDROID_PACKAGE_NAME");
   dvm_ref r = dvm_new_string(vm, pkg && *pkg ? pkg : "com.lunaria.app");
   if (dvm__env("LUNARIA_DVM_TRACE"))
      fprintf(stderr, "[dvm] Context.getPackageName: @%x \\\"%s\\\"\n", r,
              r ? dvm_string_utf8(vm, r) : "");
   RETL(r);
}

/* Context.checkSelfPermission / checkCallingOrSelfPermission /
 * checkCallingPermission.
 *
 * These were unresolved, which answers 0 — and 0 is PERMISSION_GRANTED, so
 * every permission the manifest never asked for read as held.  The install
 * decides what an app has, and the native APK launcher already publishes that list as
 * ANDROID_REQUESTED_PERMISSIONS, so answer from it. */
static bool manifest_has_permission(const char *want)
{
   const char *list = getenv("ANDROID_REQUESTED_PERMISSIONS");
   if (!want || !*want || !list) return false;
   const size_t n = strlen(want);
   for (const char *p = list; *p;) {
      const char *end = strchr(p, ',');
      const size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len == n && !memcmp(p, want, n)) return true;
      if (!end) break;
      p = end + 1;
   }
   return false;
}

static bool ctx_check_permission(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *want = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   RETI(manifest_has_permission(want) ? 0 : -1);
}

static bool pm_can_request_installs(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   if (lunaria_app_target_sdk() < 26) RETI(0);
   if (!manifest_has_permission("android.permission.REQUEST_INSTALL_PACKAGES")) {
      dvm__throw(vm, "java/lang/SecurityException",
                 "Need to declare android.permission.REQUEST_INSTALL_PACKAGES to call this api");
      return false;
   }
   const char *package_name = getenv("ANDROID_PACKAGE_NAME");
   RETI(lunaria_android_install_source_allowed(package_name));
}

/* PackageManager.checkPermission(permission, packageName) first resolves the
 * package.  A package that is not installed cannot hold a permission.  The
 * old blanket GRANTED answer made Firebase believe the absent GmsCore package
 * was installed, then wait 30 seconds for a TOKEN_REQUEST receiver which the
 * same PackageManager correctly said did not exist. */
static bool pm_check_permission(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)self;
   if (nargs < 2) RETI(-1);
   const char *package_name = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL;
   const char *self_name = getenv("ANDROID_PACKAGE_NAME");
   if (!package_name || !self_name || strcmp(package_name, self_name))
      RETI(-1); /* PERMISSION_DENIED: package is not installed */
   return ctx_check_permission(vm, self, args, 1, out);
}

static bool ctx_bindService(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out);
static bool ctx_startService(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out);
static bool ctx_unbindService(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out);
static bool ctx_stopService(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out);

static bool act_startActivity(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out);

static bool ctx_get_color(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)nargs;
   union dvm_value resources={0}, values[2]={ARG(0),{0}};
   if (!am_vcall(vm,self,"getResources","()Landroid/content/res/Resources;",NULL,0,&resources)) return false;
   dvm_pin(vm,resources.l);
   bool good=am_vcall(vm,self,"getTheme","()Landroid/content/res/Resources$Theme;",NULL,0,&values[1]);
   if (good) good=am_vcall(vm,resources.l,"getColor","(ILandroid/content/res/Resources$Theme;)I",values,2,out);
   dvm_unpin(vm,resources.l);
   return good;
}

static const struct rt_method rt_context[] = {
   M("getAttributionSource", "()Landroid/content/AttributionSource;",
     context_get_attribution_source),
   M("getAttributionTag", "()Ljava/lang/String;", context_get_attribution_tag),
   M("getColor", "(I)I", ctx_get_color),
   /* Declared on Context, not Activity: a Service is a Context too, and
    * DownloadService starts itself. */
   M("bindService",
     "(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z",
     ctx_bindService),
   M("unbindService", "(Landroid/content/ServiceConnection;)V",
     ctx_unbindService),
   M("startService", "(Landroid/content/Intent;)Landroid/content/ComponentName;",
     ctx_startService),
   M("startForegroundService",
     "(Landroid/content/Intent;)Landroid/content/ComponentName;",
     ctx_startService),
   M("stopService", "(Landroid/content/Intent;)Z", ctx_stopService),
   M("startActivity", "(Landroid/content/Intent;)V", act_startActivity),
   M("startActivity", "(Landroid/content/Intent;Landroid/os/Bundle;)V",
     act_startActivity),
   M("getApplicationContext", "()Landroid/content/Context;",
     context_get_application_context),
   M("getClassLoader", "()Ljava/lang/ClassLoader;", cl_getClassLoader),
   M("getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;",
     context_get_application_info),
   M("getPackageName", "()Ljava/lang/String;", context_get_package_name),
   /* A restricted context is the one a sandboxed host (an unlocked-credential
    * preview, a lock-screen widget) hands out; an app's own is never. */
   M("isRestricted", "()Z", ret_false),
   M("checkSelfPermission", "(Ljava/lang/String;)I", ctx_check_permission),
   M("checkCallingOrSelfPermission", "(Ljava/lang/String;)I",
     ctx_check_permission),
   M("checkCallingPermission", "(Ljava/lang/String;)I", ctx_check_permission),
   M("checkPermission", "(Ljava/lang/String;II)I", ctx_check_permission),
   /* FirebaseOptions.fromResource → StringResourceValueReader walks
    * Context.getResources().getIdentifier("google_app_id", …).  The JNI stub
    * covers native callers; bytecode needs the same object on the host class. */
   M("getResources", "()Landroid/content/res/Resources;", pm_resources),
   M("getText", "(I)Ljava/lang/CharSequence;", context_get_text),
   M("getString", "(I)Ljava/lang/String;", context_get_string),
   M("getString", "(I[Ljava/lang/Object;)Ljava/lang/String;", context_get_string),
   M("getDrawable", "(I)Landroid/graphics/drawable/Drawable;", context_get_drawable),
   M("getDrawable", "(ILandroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;", res_get_drawable),
   M("getTheme", "()Landroid/content/res/Resources$Theme;", context_get_theme),
   M("setTheme", "(I)V", context_set_theme),
   M("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
     context_get_system_service),
   M("getSystemService", "(Ljava/lang/Class;)Ljava/lang/Object;",
     context_get_system_service),
   M("getSystemServiceName", "(Ljava/lang/Class;)Ljava/lang/String;",
     context_get_system_service_name),
   /* Context.getMainLooper() is Looper.getMainLooper() — the same object.
    * Without it every `new Handler(context.getMainLooper())` got null and the
    * SDKs posted their work to nowhere. */
   M("getMainLooper", "()Landroid/os/Looper;", looper_get),
   M("createDeviceProtectedStorageContext", "()Landroid/content/Context;",
     return_self),
   M("getSharedPreferences",
     "(Ljava/lang/String;I)Landroid/content/SharedPreferences;",
     context_get_shared_preferences),
   M("getFilesDir", "()Ljava/io/File;", context_get_files_dir),
   M("getDataDir", "()Ljava/io/File;", context_get_data_dir),
   M("getCacheDir", "()Ljava/io/File;", context_get_cache_dir),
   M("getCodeCacheDir", "()Ljava/io/File;", context_get_code_cache_dir),
   M("getNoBackupFilesDir", "()Ljava/io/File;", context_get_no_backup_dir),
   M("getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;",
     context_get_external_files_dir),
   M("getExternalFilesDirs", "(Ljava/lang/String;)[Ljava/io/File;",
     context_get_external_files_dirs),
   M("getExternalCacheDir", "()Ljava/io/File;", context_get_external_cache_dir),
   M("getExternalCacheDirs", "()[Ljava/io/File;", context_get_external_cache_dirs),
   M("fileList", "()[Ljava/lang/String;", context_file_list),
   M("getObbDir", "()Ljava/io/File;", context_get_obb_dir),
   M("getObbDirs", "()[Ljava/io/File;", context_get_obb_dirs),
   M("getFileStreamPath", "(Ljava/lang/String;)Ljava/io/File;",
     context_get_file_stream_path),
   M("openFileInput", "(Ljava/lang/String;)Ljava/io/FileInputStream;",
     context_open_file_input),
   M("openFileOutput", "(Ljava/lang/String;I)Ljava/io/FileOutputStream;",
     context_open_file_output),
   M("deleteFile", "(Ljava/lang/String;)Z", context_delete_file),
   M("getMainExecutor", "()Ljava/util/concurrent/Executor;",
     context_get_main_executor),
   M("getPackageResourcePath", "()Ljava/lang/String;",
     context_get_package_resource_path),
   M("getPackageCodePath", "()Ljava/lang/String;",
     context_get_package_resource_path),
   M("getDatabasePath", "(Ljava/lang/String;)Ljava/io/File;",
     context_get_database_path),
   /* Context.openOrCreateDatabase(name, mode, factory) is SQLiteOpenHelper's
      getWritableDatabase() without the helper: the same object, named the
      same way, so both routes see one database. */
   M("openOrCreateDatabase",
     "(Ljava/lang/String;ILandroid/database/sqlite/SQLiteDatabase$CursorFactory;)"
     "Landroid/database/sqlite/SQLiteDatabase;",
     context_open_database),
   M("openOrCreateDatabase",
     "(Ljava/lang/String;ILandroid/database/sqlite/SQLiteDatabase$CursorFactory;"
     "Landroid/database/DatabaseErrorHandler;)"
     "Landroid/database/sqlite/SQLiteDatabase;",
     context_open_database),
   M("deleteDatabase", "(Ljava/lang/String;)Z", context_delete_database),
   M("isDeviceProtectedStorage", "()Z", ret_false),
   M("createConfigurationContext",
     "(Landroid/content/res/Configuration;)Landroid/content/Context;",
     context_create_configuration),
   M("createPackageContext", "(Ljava/lang/String;I)Landroid/content/Context;",
     context_create_package),
   M("obtainStyledAttributes", "([I)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M("obtainStyledAttributes", "(I[I)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M("obtainStyledAttributes",
     "(Landroid/util/AttributeSet;[III)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M("obtainStyledAttributes",
     "(Landroid/util/AttributeSet;[I)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M("obtainStyledAttributes",
     "(Landroid/util/AttributeSet;[III)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M_END,
};

static bool text_get_filters(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "filters", "[Landroid/text/InputFilter;", &v);
   RETL(v.l ? v.l : dvm_new_array(vm, 'L', "Landroid/text/InputFilter;", 0));
}

static bool text_set_filters(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   union dvm_value v = { .l = nargs ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "filters", "[Landroid/text/InputFilter;", v);
   RETV();
}

static bool text_get_transformation(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "transformationMethod",
                       "Landroid/text/method/TransformationMethod;", &v);
   RETL(v.l);
}

static bool text_set_transformation(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   union dvm_value v = { .l = nargs ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "transformationMethod",
                       "Landroid/text/method/TransformationMethod;", v);
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

/* InputFilter.LengthFilter counts UTF-16 code units, as CharSequence does.
 * Calling the source's own subSequence also preserves spans when it is a
 * SpannedString rather than a plain String. */
static bool length_filter_init(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   union dvm_value v = { .i = nargs ? ARG(0).i : 0 };
   (void)dvm_set_field(vm, self, "mMax", "I", v);
   RETV();
}

static bool length_filter_get_max(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "mMax", "I", &v);
   RETI(v.i);
}

static bool charseq_length(struct dvm *vm, dvm_ref seq, int32_t *len)
{
   struct dvm_class *c = dvm_object_class(vm, seq);
   struct dvm_method *m = c ? dvm_find_method(vm, c, "length", "()I") : NULL;
   union dvm_value result = { 0 };
   if (!m || !dvm_call(vm, m, seq, NULL, 0, &result)) return false;
   *len = result.i;
   return true;
}

static bool length_filter_filter(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   if (nargs < 6 || !ARG(0).l || !ARG(3).l) RETL(0);
   int32_t dest_len = 0;
   if (!charseq_length(vm, ARG(3).l, &dest_len)) return false;
   union dvm_value max = { 0 };
   (void)dvm_get_field(vm, self, "mMax", "I", &max);
   const int32_t start = ARG(1).i, end = ARG(2).i;
   int32_t keep = max.i - (dest_len - (ARG(5).i - ARG(4).i));
   if (keep <= 0) RETL(dvm_new_string(vm, ""));
   if (keep >= end - start) RETL(0); /* null means leave source unchanged */
   keep += start;
   struct dvm_class *c = dvm_object_class(vm, ARG(0).l);
   struct dvm_method *char_at = c ? dvm_find_method(vm, c, "charAt", "(I)C") : NULL;
   struct dvm_method *sub = c ? dvm_find_method(vm, c, "subSequence",
                         "(II)Ljava/lang/CharSequence;") : NULL;
   if (!char_at || !sub) return false;
   union dvm_value index = { .i = keep - 1 }, ch = { 0 };
   if (!dvm_call(vm, char_at, ARG(0).l, &index, 1, &ch)) return false;
   if (ch.i >= 0xd800 && ch.i <= 0xdbff) --keep;
   union dvm_value range[2] = { { .i = start }, { .i = keep } };
   union dvm_value result = { 0 };
   if (!dvm_call(vm, sub, ARG(0).l, range, 2, &result)) return false;
   RETL(result.l);
}

static const struct rt_field rt_length_filter_fields[] = {
   { "mMax", "I" }, F_END
};
static const struct rt_method rt_length_filter[] = {
   M("<init>", "(I)V", length_filter_init),
   M("getMax", "()I", length_filter_get_max),
   M("filter", "(Ljava/lang/CharSequence;IILandroid/text/Spanned;II)Ljava/lang/CharSequence;",
     length_filter_filter),
   M_END,
};

static bool password_transformation_instance(struct dvm *vm, dvm_ref self,
                                              const union dvm_value *args,
                                              int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   static struct dvm *owner;
   static dvm_ref instance;
   if (owner != vm || !instance) {
      struct dvm_class *c = dvm__class_by_desc(
         vm, "Landroid/text/method/PasswordTransformationMethod;");
      instance = c ? dvm_new_object(vm, c) : 0;
      owner = vm;
   }
   RETL(instance);
}

static const struct rt_method rt_password_transformation[] = {
   M("<init>", "()V", nop_void),
   SM("getInstance", "()Landroid/text/method/PasswordTransformationMethod;",
      password_transformation_instance),
   M_END,
};

static const char *const rt_iface_input_filter[] = {
   "Landroid/text/InputFilter;", NULL
};

static bool text_get_text(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value text = { 0 };
   (void)dvm_get_field(vm, self, "text", "Ljava/lang/CharSequence;", &text);
   RETL(text.l ? text.l : dvm_new_string(vm, ""));
}

static bool text_set_text(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   union dvm_value text = { .l = nargs ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "text", "Ljava/lang/CharSequence;", text);
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool text_set_text_resource(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)nargs;
   union dvm_value text = {0}, context = {0};
   if (!view_getContext(vm, self, NULL, 0, &context)) return false;
   if (!context.l) {
      dvm__throw(vm, "java/lang/NullPointerException", "TextView context");
      return false;
   }
   if (!context_get_text(vm, context.l, args, 1, &text)) return false;
   return text_set_text(vm, self, &text, 1, out);
}

/* The colour of the first row whose spec the state set satisfies
 * (StateSet.stateSetMatches: every positive state present, every negated
 * one absent; an empty spec matches anything), else `fallback`. */
static int32_t color_state_lookup(struct dvm *vm, dvm_ref self,
                                  const int32_t *set, uint32_t nset,
                                  int32_t fallback)
{
   union dvm_value specs = { 0 }, colors = { 0 };
   (void)dvm_get_field(vm, self, "stateSpecs", "[[I", &specs);
   (void)dvm_get_field(vm, self, "colors", "[I", &colors);
   struct dvm_object *rows = specs.l ? dvm__obj(vm, specs.l) : NULL;
   struct dvm_object *vals = colors.l ? dvm__obj(vm, colors.l) : NULL;
   if (!rows || !vals || !rows->data || !vals->data) return fallback;
   const uint32_t n = rows->length < vals->length ? rows->length : vals->length;
   for (uint32_t i = 0; i < n; ++i) {
      struct dvm_object *row = dvm__obj(vm, ((dvm_ref *)rows->data)[i]);
      bool match = true;
      for (uint32_t k = 0; row && row->data && k < row->length && match; ++k) {
         const int32_t st = ((int32_t *)row->data)[k];
         bool present = false;
         for (uint32_t j = 0; j < nset; ++j)
            if (set[j] == (st > 0 ? st : -st)) { present = true; break; }
         match = st > 0 ? present : !present;
      }
      if (match) return ((int32_t *)vals->data)[i];
   }
   return fallback;
}

/* setTextColor(ColorStateList): the list replaces the text colour; what is
 * drawn is its colour for the view's state — at rest, {state_enabled}. */
static bool text_set_color_state(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   union dvm_value color = { .l = nargs ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "textColors",
                       "Landroid/content/res/ColorStateList;", color);
   if (color.l) {
      static const int32_t enabled[1] = { 0x0101009e /* state_enabled */ };
      union dvm_value def = { 0 };
      (void)dvm_get_field(vm, color.l, "color", "I", &def);
      union dvm_value v = { .i = color_state_lookup(vm, color.l, enabled, 1, def.i) };
      (void)dvm_set_field(vm, self, "textColor", "I", v);
      ui_invalidate();
   }
   RETV();
}

static bool image_set_drawable(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   union dvm_value drawable = { .l = nargs ? ARG(0).l : 0 };
   union dvm_value previous = {0};
   (void)dvm_get_field(vm,self,"drawable","Landroid/graphics/drawable/Drawable;",&previous);
   union dvm_value resource = {0};
   (void)dvm_get_field(vm,self,"imageResource","I",&resource);
   if (previous.l == drawable.l && !resource.i) RETV();
   int32_t dimensions[2][2]={{-1,-1},{-1,-1}};
   dvm_ref refs[]={previous.l,drawable.l};
   const char *methods[]={"getIntrinsicWidth","getIntrinsicHeight"};
   for (int i=0;i<2;++i) for (int k=0;refs[i] && k<2;++k) {
      struct dvm_class *cls=dvm_object_class(vm,refs[i]);
      struct dvm_method *method=cls ? dvm_find_method(vm,cls,methods[k],"()I") : NULL;
      union dvm_value value={0};
      if (method) {
         if (!dvm_call(vm,method,refs[i],NULL,0,&value)) return false;
         dimensions[i][k]=value.i;
      }
   }
   if(!view_replace_drawable_callback(vm,self,previous.l,drawable.l)) return false;
   (void)dvm_set_field(vm,self,"imageResource","I",(union dvm_value){.i=0});
   (void)dvm_set_field(vm, self, "drawable",
                       "Landroid/graphics/drawable/Drawable;", drawable);
   if (resource.i || dimensions[0][0] != dimensions[1][0] || dimensions[0][1] != dimensions[1][1])
      (void)view_request_layout(vm,self,NULL,0,out);
   ui_invalidate();
   if (dvm__env("LUNARIA_TRACE_DRAWABLE"))
      fprintf(stderr,"[image] view=%x drawable=%x intrinsic=%dx%d\n",self,drawable.l,
         dimensions[1][0],dimensions[1][1]);
   RETV();
}

static bool image_resource_drawable(struct dvm *vm, dvm_ref self, int32_t id,
                                     union dvm_value *drawable)
{
   drawable->l = 0;
   if (!id) return true;
   union dvm_value context={0},resources={0},input[2]={{.i=id},{0}};
   return am_vcall(vm,self,"getContext","()Landroid/content/Context;",NULL,0,&context) &&
      am_vcall(vm,context.l,"getTheme","()Landroid/content/res/Resources$Theme;",NULL,0,&input[1]) &&
      am_vcall(vm,self,"getResources","()Landroid/content/res/Resources;",NULL,0,&resources) &&
      am_vcall(vm,resources.l,"getDrawable","(ILandroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;",input,2,drawable);
}

static bool image_get_drawable(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value drawable = { 0 };
   (void)dvm_get_field(vm, self, "drawable",
                       "Landroid/graphics/drawable/Drawable;", &drawable);
   int32_t resource=rt_get_int(vm,self,"imageResource");
   if (!drawable.l && resource) {
      if (!image_resource_drawable(vm,self,resource,&drawable) ||
          !image_set_drawable(vm,self,&drawable,1,out)) return false;
      rt_set_int(vm,self,"imageResource",resource);
   }
   RETL(drawable.l);
}

/* LinearLayout.setOrientation / setGravity.  Both used to write one shared
 * placeholder field, so a layout built in code kept neither: every such
 * LinearLayout laid out horizontally with top|start gravity, and a header
 * above a form became a header beside it. */
static bool linear_set_orientation(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   union dvm_value value = { .i = nargs ? ARG(0).i : 0 };
   (void)dvm_set_field(vm, self, "orientation", "I", value);
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool linear_get_orientation(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "orientation", "I", &v);
   RETI(v.i);
}

/* setGravity(): Gravity.NO_GRAVITY (0) means START|TOP, as on a device. */
static bool linear_set_gravity(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   int32_t g = nargs ? ARG(0).i : 0;
   if (!(g & 0x07)) g |= 0x03;
   if (!(g & 0x70)) g |= 0x30;
   union dvm_value value = { .i = g };
   (void)dvm_set_field(vm, self, "gravity", "I", value);
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool linear_get_gravity(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "gravity", "I", &v);
   RETI(v.i);
}

/* Compound drawable accessors always return a fresh four-element array. */
static const char *text_drawable_fields[] = {
   "drawableLeft", "drawableTop", "drawableRight", "drawableBottom",
   "drawableStart", "drawableEnd"
};
static bool text_get_compound_drawables(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   bool relative = strstr(builtin_name(), "Relative") != NULL;
   dvm_ref array = dvm_new_array(vm, 'L', "Landroid/graphics/drawable/Drawable;", 4);
   if (!array) RETL(0);
   dvm_ref values[6] = {0};
   for (int i = 0; i < 6; ++i) {
      union dvm_value v = {0};
      (void)dvm_get_field(vm, self, text_drawable_fields[i], "Landroid/graphics/drawable/Drawable;", &v);
      values[i] = v.l;
   }
   union dvm_value override = {0}, direction = {0};
   (void)dvm_get_field(vm, self, "drawableRelative", "Z", &override);
   (void)dvm_get_field(vm, self, "layoutDirection", "I", &direction);
   dvm_ref *elements = dvm_array_data(vm, array);
   elements[1] = values[1]; elements[3] = values[3];
   if (relative) { elements[0] = values[4]; elements[2] = values[5]; }
   else if (override.i) {
      elements[0] = values[direction.i == 1 ? 5 : 4];
      elements[2] = values[direction.i == 1 ? 4 : 5];
   } else { elements[0] = values[0]; elements[2] = values[2]; }
   RETL(array);
}
static bool text_set_compound_drawables(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)out;
   bool relative = strstr(builtin_name(), "Relative") != NULL;
   union dvm_value drawables[4] = {{0}};
   bool intrinsic = strstr(builtin_name(), "WithIntrinsicBounds") != NULL;
   bool resources = !strcmp(builtin_sig(), "(IIII)V");
   for (int i = 0; i < 4 && i < nargs; ++i) {
      drawables[i] = args[i];
      if (resources) {
         drawables[i].l = 0;
         if (args[i].i && !res_get_drawable(vm, 0, &args[i], 1, &drawables[i])) return false;
      }
      if (intrinsic && drawables[i].l) {
         struct dvm_class *dc = dvm_object_class(vm, drawables[i].l);
         struct dvm_method *w = dvm_find_method(vm, dc, "getIntrinsicWidth", "()I");
         struct dvm_method *h = dvm_find_method(vm, dc, "getIntrinsicHeight", "()I");
         struct dvm_method *bounds = dvm_find_method(vm, dc, "setBounds", "(IIII)V");
         union dvm_value a[4] = {{0}}, ignored = {0};
         if (w && !dvm_call(vm, w, drawables[i].l, NULL, 0, &a[2])) return false;
         if (h && !dvm_call(vm, h, drawables[i].l, NULL, 0, &a[3])) return false;
         if (bounds && !dvm_call(vm, bounds, drawables[i].l, a, 4, &ignored)) return false;
      }
   }
   if (intrinsic) {
      struct dvm_method *setter = dvm_find_method(vm, dvm_object_class(vm, self),
         relative ? "setCompoundDrawablesRelative" : "setCompoundDrawables",
         "(Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;)V");
      if (setter) return dvm_call(vm, setter, self, drawables, 4, out);
   }
   for (int i = 0; i < 6; ++i) {
      int arg = i == 1 ? 1 : i == 3 ? 3 :
         relative ? (i == 4 ? 0 : i == 5 ? 2 : -1) : (i == 0 ? 0 : i == 2 ? 2 : -1);
      union dvm_value v = { .l = arg >= 0 && arg < nargs ? drawables[arg].l : 0 };
      (void)dvm_set_field(vm, self, text_drawable_fields[i], "Landroid/graphics/drawable/Drawable;", v);
   }
   union dvm_value mode = { .i = relative };
   (void)dvm_set_field(vm, self, "drawableRelative", "Z", mode);
   ui_invalidate();
   RETV();
}

static const struct rt_field rt_text_view_fields[] = {
   { "paintFlags", "I" },
   { "textPaint", "Landroid/text/TextPaint;" },
   { "drawablePadding", "I" },
   { "drawableLeft", "Landroid/graphics/drawable/Drawable;" },
   { "drawableTop", "Landroid/graphics/drawable/Drawable;" },
   { "drawableRight", "Landroid/graphics/drawable/Drawable;" },
   { "drawableBottom", "Landroid/graphics/drawable/Drawable;" },
   { "drawableStart", "Landroid/graphics/drawable/Drawable;" },
   { "drawableEnd", "Landroid/graphics/drawable/Drawable;" },
   { "drawableRelative", "Z" },
   { "text", "Ljava/lang/CharSequence;" },
   { "textColors", "Landroid/content/res/ColorStateList;" },
   /* Editor state.  The keyboard classes read back what they configure —
    * GameActivity's virtual keyboard sets an input type and then decides how
    * to drive the IME from getInputType() — so these are stored, not
    * discarded. */
   { "textColor", "I" },
   { "hint", "Ljava/lang/CharSequence;" },
   { "hintTextColor", "I" },
   { "textLayout", "Landroid/text/Layout;" },
   { "textNaturalLayout", "Landroid/text/Layout;" },
   { "includeFontPadding", "Z" }, { "includeFontPaddingSet", "Z" },
   { "textSize", "F" }, { "textStyle", "I" }, { "maxLines", "I" },
   { "inputType", "I" }, { "imeOptions", "I" }, { "singleLine", "Z" },
   { "selectionStart", "I" }, { "selectionEnd", "I" },
   { "textWatchers", "Ljava/util/List;" },
   { "editorActionListener", "Landroid/widget/TextView$OnEditorActionListener;" },
   { "keyListener", "Landroid/text/method/KeyListener;" },
   { "highlightColor", "I" },
   { "movementMethod", "Landroid/text/method/MovementMethod;" },
   { "editableFactory", "Landroid/text/Editable$Factory;" },
   { "filters", "[Landroid/text/InputFilter;" },
   { "transformationMethod", "Landroid/text/method/TransformationMethod;" },
   { "allCaps", "Z" }, { "capsFrom", "Ljava/lang/CharSequence;" }, { "capsTo", "Ljava/lang/CharSequence;" },
   { "ellipsize", "Landroid/text/TextUtils$TruncateAt;" },
   F_END
};

#define TEXT_SET_INT(fn, field)                                              \
   static bool fn(struct dvm *vm, dvm_ref self,                              \
                  const union dvm_value *args, int nargs,                    \
                  union dvm_value *out)                                      \
   {                                                                         \
      (void)out;                                                             \
      rt_set_int(vm, self, field, nargs > 0 ? ARG(0).i : 0);                 \
      RETV();                                                                \
   }

TEXT_SET_INT(text_set_text_color, "textColor")
TEXT_SET_INT(text_set_hint_text_color, "hintTextColor")
TEXT_SET_INT(text_set_bg_color,   "backgroundColor")
TEXT_SET_INT(text_set_input_type, "inputType")
TEXT_SET_INT(text_set_ime_options, "imeOptions")
TEXT_SET_INT(text_set_max_lines,  "maxLines")
#undef TEXT_SET_INT

static bool text_set_hint(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .l = nargs ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "hint", "Ljava/lang/CharSequence;", v);
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool text_get_hint(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "hint", "Ljava/lang/CharSequence;", &v);
   RETL(v.l);
}

/* TextView stores the pixel size.  Android's TypedValue units are px, dp,
 * sp, pt, in and mm in that order; this device uses one font scale and the
 * panel DPI reported through ro.sf.lcd_density. */
static bool paint_init(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out);

static float text_size_px(struct dvm *vm, dvm_ref self)
{
   union dvm_value paint = {0}, size = {0};
   (void)dvm_get_field(vm, self, "textPaint", "Landroid/text/TextPaint;", &paint);
   if (paint.l) {
      (void)dvm_get_field(vm, paint.l, "textSize", "F", &size);
      return size.f;
   }
   (void)dvm_get_field(vm, self, "textSize", "F", &size);
   return size.f != 0.f ? size.f : 14.f * rt_density();
}

/* TextView owns its TextPaint. Keep the identity and settings across lookups;
 * subclasses consult this object while measuring and laying out their text. */
static dvm_ref text_paint(struct dvm *vm, dvm_ref self)
{
   union dvm_value paint = {0};
   (void)dvm_get_field(vm, self, "textPaint", "Landroid/text/TextPaint;", &paint);
   if (paint.l) return paint.l;
   paint.l = dvm_new_object(vm, dvm_find_class(vm, "android/text/TextPaint"));
   if (!paint.l) return 0;
   union dvm_value flags = {.i = 1}, value = {0}; /* Paint.ANTI_ALIAS_FLAG */
   (void)paint_init(vm, paint.l, &flags, 1, NULL);
   value.f = text_size_px(vm, self);
   (void)dvm_set_field(vm, paint.l, "textSize", "F", value);
   value.l = 0;
   (void)dvm_get_field(vm, self, "typeface", "Landroid/graphics/Typeface;", &value);
   (void)dvm_set_field(vm, paint.l, "typeface", "Landroid/graphics/Typeface;", value);
   (void)dvm_set_field(vm, self, "textPaint", "Landroid/text/TextPaint;", paint);
   return paint.l;
}

static bool text_get_paint(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(text_paint(vm, self));
}

static void text_store_size(struct dvm *vm, dvm_ref self, float size)
{
   dvm_ref paint = text_paint(vm, self);
   union dvm_value value = {.f = size};
   (void)dvm_set_field(vm, self, "textSize", "F", value);
   if (paint) (void)dvm_set_field(vm, paint, "textSize", "F", value);
}

static bool text_set_size(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)out;
   int unit = nargs > 1 ? ARG(0).i : 2; /* one-argument overload is sp */
   float size = nargs > 1 ? ARG(1).f : ARG(0).f;
   const int dpi = device_density_dpi();
   float scale = (float)dpi / 160.0f;
   switch (unit) {
   case 0: break;                 /* COMPLEX_UNIT_PX */
   case 1: case 2: size *= scale; break; /* DIP / SP */
   case 3: size *= (float)dpi / 72.0f; break; /* PT */
   case 4: size *= (float)dpi; break;         /* IN */
   case 5: size *= (float)dpi / 25.4f; break; /* MM */
   default: RETV();
   }
   text_store_size(vm, self, size);
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool text_get_size(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   v.f = text_size_px(vm, self);
   *out = v;
   return true;
}

/* setRawInputType() differs from setInputType() only in that it leaves the
 * key listener alone; the stored type is the same one getInputType() answers,
 * and the virtual-keyboard code that calls it reads that back. */

static bool text_get_max_lines(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "maxLines"));
}

static bool text_get_input_type(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "inputType"));
}

static bool text_get_ime_options(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "imeOptions"));
}

static bool text_set_single_line(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .i = nargs > 0 ? ARG(0).i : 1 };
   (void)dvm_set_field(vm, self, "singleLine", "Z", v);
   RETV();
}

static bool text_add_watcher(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   union dvm_value v = { 0 };
   if (!dvm_get_field(vm, self, "textWatchers", "Ljava/util/List;", &v) || !v.l) {
      v.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      (void)dvm_set_field(vm, self, "textWatchers", "Ljava/util/List;", v);
   }
   if (!v.l) RETV();
   return list_add(vm, v.l, args, nargs, out);
}

#define TEXT_SET_REF(fn, field, desc)                                        \
   static bool fn(struct dvm *vm, dvm_ref self,                              \
                  const union dvm_value *args, int nargs,                    \
                  union dvm_value *out)                                      \
   {                                                                         \
      (void)out;                                                             \
      union dvm_value v = { .l = nargs > 0 ? ARG(0).l : 0 };                 \
      (void)dvm_set_field(vm, self, field, desc, v);                         \
      RETV();                                                                \
   }

/* The last field that registered an editor-action listener.  A field asking to
 * be told when the user is done is a field meant to be typed into, and
 * LUNARIA_IME_TEST uses it to raise the input method on the app's real widget
 * — with the app's own watchers and listener behind it — without having to
 * navigate a game to the screen that would. */
static dvm_ref g_ime_last_field;

static bool text_set_editor_action(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .l = nargs > 0 ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "editorActionListener",
                       "Landroid/widget/TextView$OnEditorActionListener;", v);
   if (v.l && g_ime_last_field != self) {
      if (g_ime_last_field) dvm_unpin(vm, g_ime_last_field);
      dvm_pin(vm, self);
      g_ime_last_field = self;
   }
   RETV();
}
TEXT_SET_REF(text_set_key_listener, "keyListener",
             "Landroid/text/method/KeyListener;")
TEXT_SET_REF(text_set_editable_factory, "editableFactory",
             "Landroid/text/Editable$Factory;")
#undef TEXT_SET_REF

static bool text_get_key_listener(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "keyListener",
                       "Landroid/text/method/KeyListener;", &v);
   RETL(v.l);
}

/* TextView.setTypeface(tf[, style]): the style is what the text shows. */
static bool text_set_typeface(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)out;
   int32_t style = ARG(0).l ? rt_get_int(vm, ARG(0).l, "style") : 0;
   if (nargs >= 2) style = ARG(1).i;
   (void)dvm_set_field(vm, self, "typeface", "Landroid/graphics/Typeface;", ARG(0));
   union dvm_value paint = {0};
   (void)dvm_get_field(vm, self, "textPaint", "Landroid/text/TextPaint;", &paint);
   if (paint.l)
      (void)dvm_set_field(vm, paint.l, "typeface", "Landroid/graphics/Typeface;", ARG(0));
   rt_set_int(vm, self, "textStyle", style & 3);
   ui_layout_changed();
   RETV();
}

static bool text_set_gravity(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs; (void)out;
   int32_t g = ARG(0).i;
   /* No horizontal part means START, no vertical part TOP (TextView). */
   if (!(g & 0x07) && !(g & 0x800007)) g |= 0x800003;
   if (!(g & 0x70)) g |= 0x30;
   rt_set_int(vm, self, "gravity", g);
   ui_layout_changed();
   ui_invalidate();
   RETV();
}

static bool text_get_typeface(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "typeface", "Landroid/graphics/Typeface;", &v);
   RETL(v.l);
}

static bool text_get_gravity(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   const int32_t g = rt_get_int(vm, self, "gravity");
   RETI(g ? g : 0x800033);   /* START | TOP */
}


/* The MovementMethod singletons: getInstance() answers the same object every
 * time, as the framework's do. */
static bool movement_get_instance(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   const struct dvm_method *m = dvm__builtin_method();
   static struct { struct dvm_class *cls; dvm_ref instance; } cache[8];
   struct dvm_class *c = m ? m->cls : NULL;
   for (size_t i = 0; c && i < sizeof cache / sizeof cache[0]; ++i) {
      if (cache[i].cls == c) RETL(cache[i].instance);
      if (!cache[i].cls) {
         dvm_ref o = dvm_new_object(vm, c);
         if (o) dvm_pin(vm, o);
         cache[i].cls = c;
         cache[i].instance = o;
         RETL(o);
      }
   }
   RETL(c ? dvm_new_object(vm, c) : 0);
}

static const struct rt_method rt_movement_method[] = {
   SM("getInstance", "()Landroid/text/method/MovementMethod;", movement_get_instance),
   M_END,
};
static const struct rt_method rt_link_movement_method[] = {
   SM("getInstance", "()Landroid/text/method/MovementMethod;", movement_get_instance),
   M_END,
};

/* DigitsKeyListener.getInstance(accepted | sign, decimal): a KeyListener that
 * accepts only the listed characters; its input type is TYPE_CLASS_NUMBER. */
static bool digits_get_instance(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)self;
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/text/method/DigitsKeyListener;");
   dvm_ref l = c ? dvm_new_object(vm, c) : 0;
   if (l) {
      union dvm_value accepted = { .l = nargs == 1 ? ARG(0).l : 0 };
      (void)dvm_set_field(vm, l, "accepted", "Ljava/lang/String;", accepted);
   }
   RETL(l);
}

static bool digits_get_input_type(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETI(2);   /* InputType.TYPE_CLASS_NUMBER */
}

static const struct rt_method rt_digits_key_listener[] = {
   SM("getInstance", "()Landroid/text/method/DigitsKeyListener;", digits_get_instance),
   SM("getInstance", "(Ljava/lang/String;)Landroid/text/method/DigitsKeyListener;",
      digits_get_instance),
   SM("getInstance", "(ZZ)Landroid/text/method/DigitsKeyListener;", digits_get_instance),
   M("getInputType", "()I", digits_get_input_type),
   M_END,
};
static const struct rt_field rt_digits_key_listener_fields[] = {
   { "accepted", "Ljava/lang/String;" }, F_END
};

/* AccessibilityNodeInfo.AccessibilityAction(id, label) */
static bool a11y_action_init(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)out;
   (void)dvm_set_field(vm, self, "id", "I", (union dvm_value){ .i = ARG(0).i });
   (void)dvm_set_field(vm, self, "label", "Ljava/lang/CharSequence;", ARG(1));
   RETV();
}

static bool a11y_action_id(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "id", "I", &v);
   RETI(v.i);
}

static bool a11y_action_label(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "label", "Ljava/lang/CharSequence;", &v);
   RETL(v.l);
}

static const struct rt_method rt_a11y_action[] = {
   M("<init>", "(ILjava/lang/CharSequence;)V", a11y_action_init),
   M("getId", "()I", a11y_action_id),
   M("getLabel", "()Ljava/lang/CharSequence;", a11y_action_label),
   M_END,
};
static const struct rt_field rt_a11y_action_fields[] = {
   { "id", "I" }, { "label", "Ljava/lang/CharSequence;" }, F_END
};

/* android.widget.AutoCompleteTextView: an EditText that suggests from an
 * adapter.  The threshold, adapter and listeners are kept and read back; the
 * suggestion popup itself is not drawn (isPopupShowing() is false). */
static bool act_popup_noop(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; (void)out; return true; }
static bool act_popup_showing(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(0); }

static const struct rt_method rt_auto_complete_text_view[] = {
   M("<init>", "(Landroid/content/Context;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;I)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;II)V", view_init_context),
   M("setThreshold", "(I)V", act_set_threshold),
   M("getThreshold", "()I", act_get_threshold),
   M("setAdapter", "(Landroid/widget/ListAdapter;)V", act_set_adapter),
   M("getAdapter", "()Landroid/widget/ListAdapter;", act_get_adapter),
   M("setOnItemClickListener", "(Landroid/widget/AdapterView$OnItemClickListener;)V", act_set_item_click),
   M("showDropDown", "()V", act_popup_noop),
   M("dismissDropDown", "()V", act_popup_noop),
   M("isPopupShowing", "()Z", act_popup_showing),
   M("onFinishInflate", "()V", empty_void),
   M_END,
};
static const struct rt_field rt_auto_complete_text_view_fields[] = {
   { "threshold", "I" }, { "adapter", "Landroid/widget/ListAdapter;" },
   { "itemClickListener", "Landroid/widget/AdapterView$OnItemClickListener;" }, F_END
};

static dvm_ref ui_obj(struct dvm *vm, dvm_ref o, const char *name,
                      const char *type)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, o, name, type, &v);
   return v.l;
}

static const char *ui_text_of(struct dvm *vm, dvm_ref view)
{
   dvm_ref t = ui_obj(vm, view, "text", "Ljava/lang/CharSequence;");
   if (!t) return NULL;
   const char *s = dvm_string_utf8(vm, t);
   if (s) return s;
   struct dvm_class *c = dvm_object_class(vm, t);
   struct dvm_method *m =
      c ? dvm_find_method(vm, c, "toString", "()Ljava/lang/String;") : NULL;
   union dvm_value out = { 0 };
   if (m && dvm_call(vm, m, t, NULL, 0, &out) && out.l)
      return dvm_string_utf8(vm, out.l);
   dvm_clear_exception(vm);
   return NULL;
}
static bool view_minimum(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out);

/* TextView.length(): the characters of the text, in UTF-16 units. */
static bool text_length(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *s = ui_text_of(vm, self);
   int32_t n = 0;
   for (; s && *s; ++s) {
      const unsigned char c = (unsigned char)*s;
      if ((c & 0xc0) != 0x80) n += c >= 0xf0 ? 2 : 1;   /* a 4-byte sequence is a surrogate pair */
   }
   RETI(n);
}

static bool text_set_paint_flags(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   rt_set_int(vm, self, "paintFlags", ARG(0).i);
   (void)dvm_set_field(vm,text_paint(vm,self),"flags","I",ARG(0));
   RETV();
}

static bool text_font_padding(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!nargs) RETI(!rt_get_int(vm,self,"includeFontPaddingSet") || rt_get_int(vm,self,"includeFontPadding"));
   (void)dvm_set_field(vm,self,"includeFontPaddingSet","Z",(union dvm_value){.i=1});
   (void)dvm_set_field(vm,self,"includeFontPadding","Z",(union dvm_value){.i=ARG(0).i!=0});
   return view_request_layout(vm,self,NULL,0,out);
}

static const struct rt_method rt_text_view[] = {
   M("onTouchEvent","(Landroid/view/MotionEvent;)Z",text_on_touch),
   M("onDraw","(Landroid/graphics/Canvas;)V",text_on_draw),
   M("setIncludeFontPadding","(Z)V",text_font_padding),
   M("getIncludeFontPadding","()Z",text_font_padding),
   M("length", "()I", text_length),
   M("setPaintFlags", "(I)V", text_set_paint_flags),
   M("setMinWidth", "(I)V", view_minimum),
   M("setMinHeight", "(I)V", view_minimum),
   M("getLineCount", "()I", text_get_line_count),
   M("getLayout", "()Landroid/text/Layout;", text_get_layout),
   M("setHighlightColor", "(I)V", text_set_highlight_color),
   M("getHighlightColor", "()I", text_get_highlight_color),
   M("setMovementMethod", "(Landroid/text/method/MovementMethod;)V", text_set_movement_method),
   M("getMovementMethod", "()Landroid/text/method/MovementMethod;", text_get_movement_method),
   M("getPaint", "()Landroid/text/TextPaint;", text_get_paint),
   M("onMeasure", "(II)V", widget_on_measure),
   M("setCompoundDrawablesWithIntrinsicBounds", "(Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;)V", text_set_compound_drawables),
   M("setCompoundDrawablesWithIntrinsicBounds", "(IIII)V", text_set_compound_drawables),
   M("setCompoundDrawablesRelativeWithIntrinsicBounds", "(Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;)V", text_set_compound_drawables),
   M("setCompoundDrawablesRelativeWithIntrinsicBounds", "(IIII)V", text_set_compound_drawables),
   M("getCompoundDrawables", "()[Landroid/graphics/drawable/Drawable;", text_get_compound_drawables),
   M("getCompoundDrawablesRelative", "()[Landroid/graphics/drawable/Drawable;", text_get_compound_drawables),
   M("setCompoundDrawables", "(Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;)V", text_set_compound_drawables),
   M("setCompoundDrawablesRelative", "(Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;Landroid/graphics/drawable/Drawable;)V", text_set_compound_drawables),
   M("<init>", "(Landroid/content/Context;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V",
     view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;I)V",
     view_init_context),
   M("getContext", "()Landroid/content/Context;", view_getContext),
   M("getTransformationMethod", "()Landroid/text/method/TransformationMethod;",
     text_get_transformation),
   M("getFilters", "()[Landroid/text/InputFilter;", text_get_filters),
   M("setFilters", "([Landroid/text/InputFilter;)V", text_set_filters),
   M("getText", "()Ljava/lang/CharSequence;", text_get_text),
   /* EditText re-declares getText() with the covariant return type Editable.
    * The dex call site is resolved by descriptor, so the wider signature has
    * to be present too or it lands on the unresolved stub and the caller gets
    * null where it expects the edit buffer. */
   M("getText", "()Landroid/text/Editable;", text_get_text),
   M("setText", "(Ljava/lang/CharSequence;)V", text_set_text),
   M("setText", "(I)V", text_set_text_resource),
   M("setHint", "(Ljava/lang/CharSequence;)V", text_set_hint),
   M("getHint", "()Ljava/lang/CharSequence;", text_get_hint),
   M("setHintTextColor", "(I)V", text_set_hint_text_color),
   M("setTextSize", "(F)V", text_set_size),
   M("setTextSize", "(IF)V", text_set_size),
   M("getTextSize", "()F", text_get_size),
   M("setTypeface", "(Landroid/graphics/Typeface;)V", text_set_typeface),
   M("setTypeface", "(Landroid/graphics/Typeface;I)V", text_set_typeface),
   M("getTypeface", "()Landroid/graphics/Typeface;", text_get_typeface),
   M("setGravity", "(I)V", text_set_gravity),
   M("getGravity", "()I", text_get_gravity),
   M("setTransformationMethod", "(Landroid/text/method/TransformationMethod;)V",
     text_set_transformation),
   M("setEnabled", "(Z)V", view_set_enabled),
   M("getMaxLines", "()I", text_get_max_lines),
   M("setMaxLines", "(I)V", text_set_max_lines),
   M("setTextAppearance", "(I)V", nop_void),
   M("setTextColor", "(Landroid/content/res/ColorStateList;)V",
     text_set_color_state),
   M("setTextColor", "(I)V", text_set_text_color),
   M("setBackgroundColor", "(I)V", text_set_bg_color),
   M("setInputType", "(I)V", text_set_input_type),
   M("setRawInputType", "(I)V", text_set_input_type),
   M("getInputType", "()I", text_get_input_type),
   M("setImeOptions", "(I)V", text_set_ime_options),
   M("getImeOptions", "()I", text_get_ime_options),
   M("setSingleLine", "()V", text_set_single_line),
   M("setSingleLine", "(Z)V", text_set_single_line),
   M("setPadding", "(IIII)V", view_set_padding),
   M("setWidth", "(I)V", text_set_width),
   M("setHeight", "(I)V", nop_void),
   M("setSelection", "(I)V", text_set_selection),
   M("setSelection", "(II)V", text_set_selection),
   M("addTextChangedListener", "(Landroid/text/TextWatcher;)V",
     text_add_watcher),
   M("removeTextChangedListener", "(Landroid/text/TextWatcher;)V", nop_void),
   M("setOnEditorActionListener",
     "(Landroid/widget/TextView$OnEditorActionListener;)V",
     text_set_editor_action),
   M("setKeyListener", "(Landroid/text/method/KeyListener;)V",
     text_set_key_listener),
   M("getKeyListener", "()Landroid/text/method/KeyListener;",
     text_get_key_listener),
   M("setEditableFactory", "(Landroid/text/Editable$Factory;)V",
     text_set_editable_factory),
   M("isClickable", "()Z", ret_true),
   M("isFocusable", "()Z", ret_true),
   M("isLongClickable", "()Z", ret_true),
   M_END,
};

static const struct rt_field rt_image_view_fields[] = {
   { "drawable", "Landroid/graphics/drawable/Drawable;" },
   { "imageResource", "I" }, { "imageMatrix", "Landroid/graphics/Matrix;" },
   { "cropToPadding", "Z" },
   { "imageAlpha", "I" }, { "imageAlphaSet", "Z" },
   { "imageTintList", "Landroid/content/res/ColorStateList;" }, { "imageTintMode", "Landroid/graphics/PorterDuff$Mode;" },
   F_END
};
static bool image_set_resource(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   int32_t resource=nargs > 0 ? ARG(0).i : 0;
   union dvm_value drawable={0};
   if (!image_resource_drawable(vm,self,resource,&drawable) ||
       !image_set_drawable(vm,self,&drawable,1,out)) return false;
   rt_set_int(vm, self, "imageResource", resource);
   RETV();
}

/* Stored inverted, because a View is enabled unless something says otherwise
 * and an untouched field reads as zero.  TextView.setEnabled() used to be a
 * no-op, which mattered: Netmarble's terms dialog ships its Confirm button
 * disabled and enables it once every required box is ticked, so dropping the
 * state made the button live from the moment it appeared. */
static bool view_set_enabled(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .i = (nargs > 0 ? ARG(0).i : 1) ? 0 : 1 };
   union dvm_value old = { 0 };
   (void)dvm_get_field(vm, self, "disabled", "Z", &old);
   if ((old.i != 0) == (v.i != 0)) RETV();
   (void)dvm_set_field(vm, self, "disabled", "Z", v);
   ui_invalidate();
   return view_refresh_drawable_state(vm, self, NULL, 0, out);
}

static bool view_is_enabled(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "disabled", "Z", &v);
   RETI(!v.i);
}

static bool image_set_bitmap(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out);

/* ImageView.setScaleType: kept as the enum's ordinal (MATRIX, FIT_XY,
 * FIT_START, FIT_CENTER, FIT_END, CENTER, CENTER_CROP, CENTER_INSIDE); the
 * view layer maps it to how the image fills the box. */
static bool image_set_scale_type(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "scaleType");
      return false;
   }
   union dvm_value o = { 0 };
   (void)dvm_get_field(vm, ARG(0).l, "ordinal", "I", &o);
   if (rt_get_int(vm,self,"scaleType")==o.i+1) RETV();
   rt_set_int(vm, self, "scaleType", o.i + 1);   /* 0 = never set */
   if (!view_request_layout(vm,self,NULL,0,out)) return false;
   ui_invalidate();
   RETV();
}

static bool image_get_scale_type(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   int32_t o = rt_get_int(vm, self, "scaleType") - 1;
   if (o < 0) o = 3;                              /* FIT_CENTER */
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/widget/ImageView$ScaleType;");
   RETL(c && o < c->nsslots ? c->sslots[o].l : 0);
}
static bool image_matrix_storage(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "imageMatrix", "Landroid/graphics/Matrix;", &v);
   if (!v.l) {
      struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/graphics/Matrix;");
      v.l = c ? dvm_new_object(vm, c) : 0;
      (void)dvm_set_field(vm, self, "imageMatrix", "Landroid/graphics/Matrix;", v);
   }
   RETL(v.l);
}
static bool image_get_matrix(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool image_on_draw(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);

/* ImageView owns a copy; later changes to the argument must not move it. */
static bool image_set_matrix(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   union dvm_value matrix={0};
   if (!image_matrix_storage(vm,self,NULL,0,&matrix)) return false;
   if (ARG(0).l) {
      if (!am_vcall(vm,matrix.l,"set","(Landroid/graphics/Matrix;)V",args,1,out)) return false;
   } else if (!am_vcall(vm,matrix.l,"reset","()V",NULL,0,out)) return false;
   ui_invalidate();
   RETV();
}

static bool image_crop_padding(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                 int nargs,union dvm_value *out)
{
   (void)nargs;
   if (builtin_name()[0]=='g') RETI(rt_get_int(vm,self,"cropToPadding"));
   if (!!rt_get_int(vm,self,"cropToPadding")==!!ARG(0).i) RETV();
   (void)dvm_set_field(vm,self,"cropToPadding","Z",ARG(0));
   if (!view_request_layout(vm,self,NULL,0,out)) return false;
   ui_invalidate(); RETV();
}

static bool image_verify_drawable(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value drawable={0};dvm_get_field(vm,self,"drawable","Landroid/graphics/drawable/Drawable;",&drawable);
   if(drawable.l==ARG(0).l) RETI(1);
   return view_verify_drawable(vm,self,args,nargs,out);
}
static bool image_invalidate_drawable(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value drawable={0};dvm_get_field(vm,self,"drawable","Landroid/graphics/drawable/Drawable;",&drawable);
   if(drawable.l==ARG(0).l) return am_vcall(vm,self,"invalidate","()V",NULL,0,out);
   return view_drawable_callback(vm,self,args,nargs,out);
}
static const struct rt_method rt_image_view[] = {
   M("verifyDrawable","(Landroid/graphics/drawable/Drawable;)Z",image_verify_drawable),
   M("invalidateDrawable","(Landroid/graphics/drawable/Drawable;)V",image_invalidate_drawable),
   M("onDraw", "(Landroid/graphics/Canvas;)V", image_on_draw),
   M("getCropToPadding", "()Z", image_crop_padding),
   M("setCropToPadding", "(Z)V", image_crop_padding),
   M("onMeasure", "(II)V", widget_on_measure),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;I)V", view_init_context),
   M("setImageMatrix", "(Landroid/graphics/Matrix;)V", image_set_matrix),
   M("getImageMatrix", "()Landroid/graphics/Matrix;", image_get_matrix),
   M("setScaleType", "(Landroid/widget/ImageView$ScaleType;)V",
     image_set_scale_type),
   M("getScaleType", "()Landroid/widget/ImageView$ScaleType;",
     image_get_scale_type),
   M("<init>", "(Landroid/content/Context;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V",
     view_init_context),
   M("setImageBitmap", "(Landroid/graphics/Bitmap;)V", image_set_bitmap),
   M("setImageDrawable", "(Landroid/graphics/drawable/Drawable;)V", image_set_drawable),
   M("getDrawable", "()Landroid/graphics/drawable/Drawable;", image_get_drawable),
   M("setImageResource", "(I)V", image_set_resource),
   M("setEnabled", "(Z)V", view_set_enabled),
   M("isEnabled", "()Z", view_is_enabled),
   M_END,
};
/* --- android.widget.CompoundButton and its family ------------------------ *
 *
 * CheckBox, RadioButton, Switch and ToggleButton were not registered at all,
 * so `<CheckBox/>` in a layout produced a bare object with no View underneath
 * it: no id, no text, no size — every attribute the inflater set was dropped
 * on the floor, and setChecked()/isChecked() had nowhere to go.  Netmarble's
 * terms dialog is a list of exactly these, and it will not enable its Confirm
 * button until each required one reports itself checked.
 */
static bool radio_group_child_changed(struct dvm *,dvm_ref,dvm_ref);
static bool compound_set_checked(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out);

static bool compound_is_checked(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "checked", "Z", &v);
   RETI(v.i != 0);
}

/* The listener runs whenever the state actually changes, which is the
 * contract app code relies on to avoid re-entering its own updates. */
static bool compound_notify(struct dvm *vm, dvm_ref self, bool checked)
{
   union dvm_value l = { 0 };
   if (!dvm_get_field(vm, self, "checkedListener",
                      "Landroid/widget/CompoundButton$OnCheckedChangeListener;",
                      &l) || !l.l)
      return true;
   struct dvm_class *c = dvm_object_class(vm, l.l);
   struct dvm_method *m =
      c ? dvm_find_method(vm, c, "onCheckedChanged",
                          "(Landroid/widget/CompoundButton;Z)V") : NULL;
   if (!m) return true;
   union dvm_value a[2] = { { .l = self }, { .i = checked } }, ignored = { 0 };
   return dvm_call(vm, m, l.l, a, 2, &ignored);
}

static bool compound_set_checked(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)out;
   int32_t want = nargs > 0 && ARG(0).i ? 1 : 0;
   union dvm_value old = { 0 };
   (void)dvm_get_field(vm, self, "checked", "Z", &old);
   if ((old.i != 0) == (want != 0)) RETV();
   union dvm_value v = { .i = want };
   if (!dvm_set_field(vm, self, "checked", "Z", v))
      fprintf(stderr, "[ui] setChecked: no `checked` field on %s\n",
              dvm_class_name(dvm_object_class(vm, self)));
   ui_invalidate();
   if (!view_refresh_drawable_state(vm, self, NULL, 0, out)) return false;
   if(rt_get_int(vm,self,"broadcastingChecked")) RETV();
   (void)dvm_set_field(vm,self,"broadcastingChecked","Z",(union dvm_value){.i=1});
   bool good=compound_notify(vm,self,want!=0);
   union dvm_value group={0};
   (void)dvm_get_field(vm,self,"radioGroup","Landroid/widget/RadioGroup;",&group);
   if(good && group.l) good=radio_group_child_changed(vm,group.l,self);
   (void)dvm_set_field(vm,self,"broadcastingChecked","Z",(union dvm_value){.i=0});
   return good;
}

static bool compound_toggle(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value cur = { 0 };
   (void)dvm_get_field(vm, self, "checked", "Z", &cur);
   union dvm_value flip = { .i = cur.i ? 0 : 1 };
   return am_vcall(vm,self,"setChecked","(Z)V",&flip,1,out);
}

static bool radio_toggle(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                          int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   union dvm_value checked={0};
   if (!am_vcall(vm,self,"isChecked","()Z",NULL,0,&checked)) return false;
   if (checked.i) RETV();
   return compound_toggle(vm,self,NULL,0,out);
}

/* Each concrete widget declares its own constructors. Java never inherits
 * <init>, even though these adapters share View context/resource setup. */
#define COMPOUND_WIDGET_CTORS \
   M("<init>","(Landroid/content/Context;)V",view_init_context), \
   M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;)V",view_init_context), \
   M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;I)V",view_init_context), \
   M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;II)V",view_init_context),
static const struct rt_method rt_compound_widget_constructors[]={
   COMPOUND_WIDGET_CTORS
   M_END,
};
static const struct rt_method rt_radio_button[]={
   COMPOUND_WIDGET_CTORS
   M("toggle","()V",radio_toggle),
   M_END,
};
#undef COMPOUND_WIDGET_CTORS

static bool radio_group_set_id(struct dvm *vm,dvm_ref group,int32_t id)
{
   rt_set_int(vm,group,"checkedId",id);
   union dvm_value listener={0},ignored={0},a[]={{.l=group},{.i=id}};
   (void)dvm_get_field(vm,group,"groupCheckedListener","Landroid/widget/RadioGroup$OnCheckedChangeListener;",&listener);
   return !listener.l || am_vcall(vm,listener.l,"onCheckedChanged","(Landroid/widget/RadioGroup;I)V",a,2,&ignored);
}
static bool radio_group_set_button(struct dvm *vm,dvm_ref group,int32_t id,bool checked)
{
   union dvm_value a={.i=id},button={0},ignored={0};
   if(!view_find_by_id(vm,group,&a,1,&button)) return false;
   if(button.l && ui_is_a(vm,button.l,"android/widget/RadioButton")) {
      a.i=checked;return am_vcall(vm,button.l,"setChecked","(Z)V",&a,1,&ignored);
   }
   return true;
}
static bool radio_group_child_changed(struct dvm *vm,dvm_ref group,dvm_ref child)
{
   if(rt_get_int(vm,group,"protectChecked")) return true;
   (void)dvm_set_field(vm,group,"protectChecked","Z",(union dvm_value){.i=1});
   int32_t old=rt_get_int(vm,group,"checkedId");
   bool good=old==-1 || radio_group_set_button(vm,group,old,false);
   (void)dvm_set_field(vm,group,"protectChecked","Z",(union dvm_value){.i=0});
   return good && radio_group_set_id(vm,group,rt_get_int(vm,child,"id"));
}
static bool radio_group_before_add(struct dvm *vm,dvm_ref group,dvm_ref child)
{
   if(!ui_is_a(vm,group,"android/widget/RadioGroup") || !ui_is_a(vm,child,"android/widget/RadioButton") ||
      !rt_get_int(vm,child,"checked")) return true;
   return radio_group_child_changed(vm,group,child);
}
static bool radio_group_hierarchy(struct dvm *vm,dvm_ref group,dvm_ref child,bool added)
{
   if(!ui_is_a(vm,group,"android/widget/RadioGroup") || !ui_is_a(vm,child,"android/widget/RadioButton")) return true;
   if(added && rt_get_int(vm,child,"id")==-1) rt_set_int(vm,child,"id",view_new_id());
   return dvm_set_field(vm,child,"radioGroup","Landroid/widget/RadioGroup;",(union dvm_value){.l=added ? group : 0});
}
static bool radio_group_check(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   int32_t id=nargs ? ARG(0).i : -1,old=rt_get_int(vm,self,"checkedId");
   if(id!=-1 && id==old) RETV();
   if(old!=-1 && !radio_group_set_button(vm,self,old,false)) return false;
   if(id!=-1 && !radio_group_set_button(vm,self,id,true)) return false;
   return radio_group_set_id(vm,self,id);
}
static bool radio_group_get_checked(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{ (void)args;(void)nargs;RETI(rt_get_int(vm,self,"checkedId")); }
static bool radio_group_listener(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;(void)out;
   return dvm_set_field(vm,self,"groupCheckedListener","Landroid/widget/RadioGroup$OnCheckedChangeListener;",ARG(0));
}
static bool radio_group_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   if (!view_init_context(vm,self,args,nargs,out)) return false;
   (void)dvm_set_field(vm,self,"orientation","I",(union dvm_value){.i=1});
   rt_set_int(vm,self,"checkedId",-1);
   RETV();
}
static const struct rt_field rt_radio_group_fields[]={
   {"checkedId","I"},{"protectChecked","Z"},
   {"groupCheckedListener","Landroid/widget/RadioGroup$OnCheckedChangeListener;"},F_END,
};
static const struct rt_method rt_radio_group_constructors[]={
   M("check","(I)V",radio_group_check),M("clearCheck","()V",radio_group_check),
   M("getCheckedRadioButtonId","()I",radio_group_get_checked),
   M("setOnCheckedChangeListener","(Landroid/widget/RadioGroup$OnCheckedChangeListener;)V",radio_group_listener),
   M("<init>","(Landroid/content/Context;)V",radio_group_init),
   M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;)V",radio_group_init),
   M_END,
};

static bool compound_set_listener(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .l = nargs > 0 ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "checkedListener",
                       "Landroid/widget/CompoundButton$OnCheckedChangeListener;",
                       v);
   RETV();
}

/* CompoundButton.setButtonDrawable / setButtonTintList / getButtonDrawable.
 * The button's picture is what a check box or radio button shows beside its
 * label; text_compound_get() lays it out as the leading drawable and
 * text_on_draw() paints it in the view's current state. */
static bool compound_apply_tint(struct dvm *vm, dvm_ref self)
{
   union dvm_value drawable = { 0 }, tint = { 0 }, ignored = { 0 };
   (void)dvm_get_field(vm, self, "buttonDrawable", "Landroid/graphics/drawable/Drawable;", &drawable);
   (void)dvm_get_field(vm, self, "buttonTint", "Landroid/content/res/ColorStateList;", &tint);
   if (!drawable.l || !tint.l) return true;
   return am_vcall(vm, drawable.l, "setTintList", "(Landroid/content/res/ColorStateList;)V", &tint, 1, &ignored);
}

static bool compound_set_button_drawable(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                         int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   union dvm_value drawable = { .l = 0 };
   if (!strcmp(builtin_sig(), "(I)V")) {
      union dvm_value context = { 0 };
      (void)dvm_get_field(vm, self, "context", "Landroid/content/Context;", &context);
      if (ARG(0).i && context.l &&
          !am_vcall(vm, context.l, "getDrawable", "(I)Landroid/graphics/drawable/Drawable;", args, 1, &drawable))
         return false;
   } else {
      drawable.l = ARG(0).l;
   }
   (void)dvm_set_field(vm, self, "buttonDrawable", "Landroid/graphics/drawable/Drawable;", drawable);
   if (!compound_apply_tint(vm, self)) return false;
   ui_view_layout_changed(vm, self);
   ui_invalidate();
   RETV();
}

static bool compound_get_button_drawable(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "buttonDrawable", "Landroid/graphics/drawable/Drawable;", &v);
   RETL(v.l);
}

static bool compound_set_button_tint(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                     int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "buttonTint", "Landroid/content/res/ColorStateList;", args[0]);
   if (!compound_apply_tint(vm, self)) return false;
   ui_invalidate();
   RETV();
}

static const struct rt_field rt_compound_fields[] = {
   { "buttonDrawable", "Landroid/graphics/drawable/Drawable;" },
   { "buttonTint", "Landroid/content/res/ColorStateList;" },
   { "broadcastingChecked", "Z" },
   { "radioGroup", "Landroid/widget/RadioGroup;" },
   { "checked", "Z" },
   { "checkedListener",
     "Landroid/widget/CompoundButton$OnCheckedChangeListener;" },
   F_END,
};

static const struct rt_method rt_compound_button[] = {
   M("onCreateDrawableState", "(I)[I", compound_create_drawable_state),
   M("<init>", "(Landroid/content/Context;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;I)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;II)V", view_init_context),
   M("setChecked", "(Z)V", compound_set_checked),
   M("isChecked", "()Z", compound_is_checked),
   M("toggle", "()V", compound_toggle),
   M("setOnCheckedChangeListener",
     "(Landroid/widget/CompoundButton$OnCheckedChangeListener;)V",
     compound_set_listener),
   M("setButtonDrawable", "(Landroid/graphics/drawable/Drawable;)V", compound_set_button_drawable),
   M("setButtonDrawable", "(I)V", compound_set_button_drawable),
   M("getButtonDrawable", "()Landroid/graphics/drawable/Drawable;", compound_get_button_drawable),
   M("setButtonTintList", "(Landroid/content/res/ColorStateList;)V", compound_set_button_tint),
   M_END,
};

static bool linear_generate_params(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   union dvm_value a[2] = {{.i = -2}, {.i = -2}}, ignored = {0};
   int count = 2;
   if (!nargs) {
      union dvm_value orientation = {0};
      (void)dvm_get_field(vm, self, "orientation", "I", &orientation);
      if (orientation.i == 1) a[0].i = -1;
      else if (orientation.i != 0) RETL(0);
   } else if (strstr(builtin_sig(), "AttributeSet;")) {
      (void)dvm_get_field(vm, self, "context", "Landroid/content/Context;", &a[0]);
      a[1] = ARG(0);
   } else { a[0] = ARG(0); count = 1; }
   dvm_ref params = dvm_new_object(vm,
      dvm_find_class(vm, "android/widget/LinearLayout$LayoutParams"));
   if (!linear_params_init(vm, params, a, count, &ignored)) return false;
   RETL(params);
}

static bool linear_check_params(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)self;
   struct dvm_class *c = nargs ? dvm_object_class(vm, ARG(0).l) : NULL;
   for (; c; c = c->super)
      if (!strcmp(c->name, "android/widget/LinearLayout$LayoutParams")) RETI(1);
   RETI(0);
}

static const struct rt_field rt_linear_fields[] = { F_END };
static const struct rt_method rt_linear_layout[] = {
   M("onLayout", "(ZIIII)V", widget_on_layout),
   M("onMeasure", "(II)V", widget_on_measure),
   M("generateDefaultLayoutParams", "()Landroid/view/ViewGroup$LayoutParams;", linear_generate_params),
   M("generateLayoutParams", "(Landroid/util/AttributeSet;)Landroid/view/ViewGroup$LayoutParams;", linear_generate_params),
   M("generateLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)Landroid/view/ViewGroup$LayoutParams;", linear_generate_params),
   M("checkLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)Z", linear_check_params),
   M("<init>", "(Landroid/content/Context;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V",
     view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;I)V",
     view_init_context),
   M("setGravity", "(I)V", linear_set_gravity),
   M("getGravity", "()I", linear_get_gravity),
   M("setOrientation", "(I)V", linear_set_orientation),
   M("getOrientation", "()I", linear_get_orientation),
   M("setBackground", "(Landroid/graphics/drawable/Drawable;)V", view_set_background),
   M_END,
};

static bool observable_init(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/util/ArrayList;");
   union dvm_value list = { .l = c ? dvm_new_object(vm, c) : 0 };
   union dvm_value ignored = { 0 };
   if (list.l) (void)list_init(vm, list.l, NULL, 0, &ignored);
   (void)dvm_set_field(vm, self, "mObservers", "Ljava/util/ArrayList;", list);
   RETV();
}

static dvm_ref observable_list(struct dvm *vm,dvm_ref self)
{
   union dvm_value list={0},ignored={0};
   (void)dvm_get_field(vm,self,"mObservers","Ljava/util/ArrayList;",&list);
   if(!list.l) {
      if(!observable_init(vm,self,NULL,0,&ignored)) return 0;
      (void)dvm_get_field(vm,self,"mObservers","Ljava/util/ArrayList;",&list);
   }
   return list.l;
}
static bool observable_mutate(struct dvm *vm,dvm_ref self,
                              const union dvm_value *args,int nargs,
                              union dvm_value *out)
{
   const char *name=builtin_name();
   bool clear=!strcmp(name,"unregisterAll"),add=!strcmp(name,"registerObserver");
   if(!clear && (!nargs || !ARG(0).l)) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","The observer is null");return false;
   }
   dvm_ref list=observable_list(vm,self);
   if(!list || !dvm__monitor_enter(vm,list)) return false;
   bool good=true;union dvm_value index={0};
   if(clear) good=list_clear(vm,list,NULL,0,out);
   else {
      index.i=-1;
      for(uint32_t i=0;good;++i) {
         struct rt_list *entries=list_of(vm,list);
         if(!entries || i>=entries->size)break;
         dvm_ref candidate=entries->items[i];bool equal=false;
         good=am_equals(vm,ARG(0).l,candidate,&equal);
         if(good && equal){index.i=(int32_t)i;break;}
      }
      if(good && ((add && index.i>=0) || (!add && index.i<0))) {
         dvm__throw(vm,"java/lang/IllegalStateException",add ?
            "Observer is already registered" : "Observer was not registered");good=false;
      }
      if(good) good=add ? list_add(vm,list,args,1,out) : list_remove(vm,list,&index,1,out);
   }
   dvm__monitor_exit(vm,list);return good;
}

static const struct rt_field rt_db_observable_fields[] = {
   { "mObservers", "Ljava/util/ArrayList;" }, F_END
};

static const struct rt_method rt_db_observable[] = {
   M("<init>", "()V", observable_init),
   M("registerObserver", "(Ljava/lang/Object;)V", observable_mutate),
   M("unregisterObserver", "(Ljava/lang/Object;)V", observable_mutate),
   M("unregisterAll", "()V", observable_mutate),
   M_END,
};

/* Android dispatches under the observer-list monitor, in reverse order.
 * Read each live slot so an observer can unregister itself during notification. */
static bool dso_notify(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                       int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;(void)out;
   bool invalidated=!strcmp(builtin_name(),"notifyInvalidated");
   dvm_ref list=observable_list(vm,self);
   if(!list || !dvm__monitor_enter(vm,list)) return false;
   struct rt_list *l=list_of(vm,list);
   int64_t i=l ? (int64_t)l->size-1 : -1;
   bool good=true;
   for(;good && i>=0;--i) {
      union dvm_value index={.i=(int32_t)i},observer={0},ignored={0};
      struct rt_list *current=list_of(vm,list);
      if(!current || (uint32_t)index.i>=current->size) {
         dvm__throw(vm,"java/lang/IndexOutOfBoundsException","Observer index %d",index.i);good=false;
      } else good=list_get(vm,list,&index,1,&observer);
      if(good) good=am_vcall(vm,observer.l,invalidated ? "onInvalidated" : "onChanged",
                             "()V",NULL,0,&ignored);
   }
   dvm__monitor_exit(vm,list);return good;
}
static const struct rt_method rt_data_set_observable[] = {
   M("<init>", "()V", observable_init),
   M("notifyChanged", "()V", dso_notify),
   M("notifyInvalidated", "()V", dso_notify),
   M_END,
};
static const struct rt_method rt_data_set_observer[] = {
   M("<init>", "()V", empty_void),
   M("onChanged", "()V", empty_void),
   M("onInvalidated", "()V", empty_void),
   M_END,
};

static const struct rt_method rt_typed_array[] = {
   M("getLayoutDimension", "(II)I", typed_array_layout_dimension),
   M("getLayoutDimension", "(ILjava/lang/String;)I", typed_array_layout_dimension),
   M("getResources","()Landroid/content/res/Resources;",view_get_resources),
   M("length", "()I", typed_array_length),
   M("getIndexCount", "()I", typed_array_get_index),
   M("getIndex", "(I)I", typed_array_get_index),
   M("hasValue", "(I)Z", typed_array_has_value),
   M("getBoolean", "(IZ)Z", typed_array_default_bool),
   M("getInt", "(II)I", typed_array_default_int),
   M("getInteger", "(II)I", typed_array_default_int),
   M("getFloat", "(IF)F", typed_array_default_float),
   M("getDimension", "(IF)F", typed_array_dimension),
   M("getResourceId", "(II)I", typed_array_resource_id),
   M("getColor", "(II)I", typed_array_color),
   M("getDimensionPixelSize", "(II)I", typed_array_dimension_size),
   M("getDimensionPixelOffset", "(II)I", typed_array_dimension_offset),
   M("getDrawable", "(I)Landroid/graphics/drawable/Drawable;",
     typed_array_get_drawable),
   M("getValue", "(ILandroid/util/TypedValue;)Z", typed_array_get_value),
   M("peekValue", "(I)Landroid/util/TypedValue;", typed_array_peek_value),
   M("getText", "(I)Ljava/lang/CharSequence;", typed_array_get_text),
   M("getString", "(I)Ljava/lang/String;", typed_array_get_text),
   M("getColorStateList", "(I)Landroid/content/res/ColorStateList;",
     typed_array_color),
   /* recycle() returns the array to a pool this runtime does not keep: a
      TypedArray here is an ordinary object the collector owns. */
   M("recycle", "()V", empty_void),
   M_END,
};

static const struct rt_field rt_typed_array_fields[] = {
   { "resources", "Landroid/content/res/Resources;" },
   {"xmlStyle","I"}, {"defaultStyle","I"},
   { "attrs", "[I" }, { "style", "I" }, { "themeSnapshot", "Landroid/content/res/Resources$Theme;" }, { "indices", "[I" },
   { "xmlNames", "[I" }, { "xmlTypes", "[I" }, { "xmlData", "[I" },
   { "xmlStrings", "[Ljava/lang/String;" }, F_END
};
static const struct rt_field rt_theme_fields[] = { { "resources", "Landroid/content/res/Resources;" }, { "style", "I" }, { "styleApplications", "[I" }, F_END };

static const struct rt_method rt_package_info[] = {
   M("getLongVersionCode", "()J", pi_getLongVersionCode),
   M_END,
};

static bool res_get_text(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self;
   const char *value = luna_framework_string_value((uint32_t)ARG(0).i);
   int32_t iv = 0;
   if (value || (ARG(0).i &&
       arm_exec_apk_resource_value((uint32_t)ARG(0).i, &iv, &value) == 'L' && value)) {
      dvm_ref text=resource_styled_text(vm,(uint32_t)ARG(0).i,value);
      if (dvm_exception(vm)) return false;
      RETL(text);
   }
   if (nargs > 1) RETL(ARG(1).l);
   dvm__throw(vm, "android/content/res/Resources$NotFoundException",
              "Text resource #0x%08x", (unsigned)ARG(0).i);
   return false;
}

static bool res_getString(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   /* Unlike getText(id, default), getString has no missing-resource fallback. */
   (void)nargs;
   union dvm_value text={0};
   if (!res_get_text(vm,self,args,1,&text)) return false;
   const char *value=rt_charseq_utf8(vm,text.l);
   if (!value) return false;
   RETL(dvm_new_string(vm,value));
}

/* BEGIN GENERATED INTEGER PLURALS */
/* Generated by test/generate_integer_plurals.py; do not edit.
 * Unicode CLDR 40 cardinal data: test/data/plurals-cldr40.xml.
 * Unicode data license: https://www.unicode.org/license.txt
 * Only integer quantities: i=n, v=w=f=t=c=e=0. */
static uint32_t integer_plural_attr(const char *locale, int32_t quantity)
{
   static const struct { const char *locale; unsigned char group; } table[] = {
      {"af",7},
      {"ak",5},
      {"am",1},
      {"an",7},
      {"ar",37},
      {"ars",37},
      {"as",1},
      {"asa",7},
      {"ast",3},
      {"az",7},
      {"bal",7},
      {"be",29},
      {"bem",7},
      {"bez",7},
      {"bg",7},
      {"bho",5},
      {"bm",0},
      {"bn",1},
      {"bo",0},
      {"br",33},
      {"brx",7},
      {"bs",18},
      {"ca",3},
      {"ce",7},
      {"ceb",11},
      {"cgg",7},
      {"chr",7},
      {"ckb",7},
      {"cs",27},
      {"cy",38},
      {"da",8},
      {"de",3},
      {"doi",1},
      {"dsb",25},
      {"dv",7},
      {"dz",0},
      {"ee",7},
      {"el",7},
      {"en",3},
      {"eo",7},
      {"es",22},
      {"et",3},
      {"eu",7},
      {"fa",1},
      {"ff",2},
      {"fi",3},
      {"fil",11},
      {"fo",7},
      {"fr",19},
      {"fur",7},
      {"fy",3},
      {"ga",34},
      {"gd",23},
      {"gl",3},
      {"gsw",7},
      {"gu",1},
      {"guw",5},
      {"gv",35},
      {"ha",7},
      {"haw",7},
      {"he",26},
      {"hi",1},
      {"hnj",0},
      {"hr",18},
      {"hsb",25},
      {"hu",7},
      {"hy",2},
      {"ia",3},
      {"id",0},
      {"ig",0},
      {"ii",0},
      {"in",0},
      {"io",3},
      {"is",9},
      {"it",21},
      {"iu",15},
      {"iw",26},
      {"ja",0},
      {"jbo",0},
      {"jgo",7},
      {"ji",3},
      {"jmc",7},
      {"jv",0},
      {"jw",0},
      {"ka",7},
      {"kab",2},
      {"kaj",7},
      {"kcg",7},
      {"kde",0},
      {"kea",0},
      {"kk",7},
      {"kkj",7},
      {"kl",7},
      {"km",0},
      {"kn",1},
      {"ko",0},
      {"ks",7},
      {"ksb",7},
      {"ksh",14},
      {"ku",7},
      {"kw",36},
      {"ky",7},
      {"lag",13},
      {"lb",7},
      {"lg",7},
      {"lij",3},
      {"lkt",0},
      {"ln",5},
      {"lo",0},
      {"lt",30},
      {"lv",12},
      {"mas",7},
      {"mg",5},
      {"mgo",7},
      {"mk",10},
      {"ml",7},
      {"mn",7},
      {"mo",17},
      {"mr",7},
      {"ms",0},
      {"mt",31},
      {"my",0},
      {"nah",7},
      {"naq",15},
      {"nb",7},
      {"nd",7},
      {"ne",7},
      {"nl",3},
      {"nn",7},
      {"nnh",7},
      {"no",7},
      {"nqo",0},
      {"nr",7},
      {"nso",5},
      {"ny",7},
      {"nyn",7},
      {"om",7},
      {"or",7},
      {"os",7},
      {"osa",0},
      {"pa",5},
      {"pap",7},
      {"pcm",1},
      {"pl",28},
      {"prg",12},
      {"ps",7},
      {"pt",20},
      {"pt_PT",21},
      {"rm",7},
      {"ro",17},
      {"rof",7},
      {"root",0},
      {"ru",32},
      {"rwk",7},
      {"sah",0},
      {"saq",7},
      {"sat",15},
      {"sc",3},
      {"scn",3},
      {"sd",7},
      {"sdh",7},
      {"se",15},
      {"seh",7},
      {"ses",0},
      {"sg",0},
      {"sh",18},
      {"shi",16},
      {"si",4},
      {"sk",27},
      {"sl",24},
      {"sma",15},
      {"smi",15},
      {"smj",15},
      {"smn",15},
      {"sms",15},
      {"sn",7},
      {"so",7},
      {"sq",7},
      {"sr",18},
      {"ss",7},
      {"ssy",7},
      {"st",7},
      {"su",0},
      {"sv",3},
      {"sw",3},
      {"syr",7},
      {"ta",7},
      {"te",7},
      {"teo",7},
      {"th",0},
      {"ti",5},
      {"tig",7},
      {"tk",7},
      {"tl",11},
      {"tn",7},
      {"to",0},
      {"tpi",0},
      {"tr",7},
      {"ts",7},
      {"tzm",6},
      {"ug",7},
      {"uk",32},
      {"ur",3},
      {"uz",7},
      {"ve",7},
      {"vi",0},
      {"vo",7},
      {"vun",7},
      {"wa",5},
      {"wae",7},
      {"wo",0},
      {"xh",7},
      {"xog",7},
      {"yi",3},
      {"yo",0},
      {"yue",0},
      {"zh",0},
      {"zu",1},
   };
   char name[96]; snprintf(name,sizeof name,"%s",locale ? locale : "root");
   for(char *p=name; *p; ++p) if(*p=='-') *p='_';
   unsigned group=0;
   for(;;) {
      size_t low=0,high=sizeof table/sizeof table[0];
      while(low<high) {size_t mid=low+(high-low)/2; int c=strcmp(name,table[mid].locale);
         if(c>0)low=mid+1;else high=mid;}
      if(low<sizeof table/sizeof table[0] && !strcmp(name,table[low].locale)) {group=table[low].group;break;}
      char *last=strrchr(name,'_'); if(!last)break; *last=0;
   }
   uint64_t n=quantity<0 ? (uint64_t)(-(int64_t)quantity) : (uint64_t)quantity;
   switch(group) {
   case 1:
      if ((((n == 0))) || (((n == 1)))) return 0x01000006u;
      break;
   case 2:
      if ((((n == 0) || (n == 1)))) return 0x01000006u;
      break;
   case 3:
      if ((((n == 1)) && ((0 == 0)))) return 0x01000006u;
      break;
   case 4:
      if ((((n == 0) || (n == 1))) || (((n == 0)) && ((0 == 1)))) return 0x01000006u;
      break;
   case 5:
      if ((((n >= 0 && n <= 1)))) return 0x01000006u;
      break;
   case 6:
      if ((((n >= 0 && n <= 1))) || (((n >= 11 && n <= 99)))) return 0x01000006u;
      break;
   case 7:
      if ((((n == 1)))) return 0x01000006u;
      break;
   case 8:
      if ((((n == 1))) || (!((0 == 0)) && ((n == 0) || (n == 1)))) return 0x01000006u;
      break;
   case 9:
      if ((((0 == 0)) && (((n % 10) == 1)) && !(((n % 100) == 11))) || (!((0 == 0)))) return 0x01000006u;
      break;
   case 10:
      if ((((0 == 0)) && (((n % 10) == 1)) && !(((n % 100) == 11))) || ((((0 % 10) == 1)) && !(((0 % 100) == 11)))) return 0x01000006u;
      break;
   case 11:
      if ((((0 == 0)) && ((n == 1) || (n == 2) || (n == 3))) || (((0 == 0)) && !(((n % 10) == 4) || ((n % 10) == 6) || ((n % 10) == 9))) || (!((0 == 0)) && !(((0 % 10) == 4) || ((0 % 10) == 6) || ((0 % 10) == 9)))) return 0x01000006u;
      break;
   case 12:
      if (((((n % 10) == 0))) || ((((n % 100) >= 11 && (n % 100) <= 19))) || (((0 == 2)) && (((0 % 100) >= 11 && (0 % 100) <= 19)))) return 0x01000005u;
      if (((((n % 10) == 1)) && !(((n % 100) == 11))) || (((0 == 2)) && (((0 % 10) == 1)) && !(((0 % 100) == 11))) || (!((0 == 2)) && (((0 % 10) == 1)))) return 0x01000006u;
      break;
   case 13:
      if ((((n == 0)))) return 0x01000005u;
      if ((((n == 0) || (n == 1)) && !((n == 0)))) return 0x01000006u;
      break;
   case 14:
      if ((((n == 0)))) return 0x01000005u;
      if ((((n == 1)))) return 0x01000006u;
      break;
   case 15:
      if ((((n == 1)))) return 0x01000006u;
      if ((((n == 2)))) return 0x01000007u;
      break;
   case 16:
      if ((((n == 0))) || (((n == 1)))) return 0x01000006u;
      if ((((n >= 2 && n <= 10)))) return 0x01000008u;
      break;
   case 17:
      if ((((n == 1)) && ((0 == 0)))) return 0x01000006u;
      if ((!((0 == 0))) || (((n == 0))) || ((((n % 100) >= 2 && (n % 100) <= 19)))) return 0x01000008u;
      break;
   case 18:
      if ((((0 == 0)) && (((n % 10) == 1)) && !(((n % 100) == 11))) || ((((0 % 10) == 1)) && !(((0 % 100) == 11)))) return 0x01000006u;
      if ((((0 == 0)) && (((n % 10) >= 2 && (n % 10) <= 4)) && !(((n % 100) >= 12 && (n % 100) <= 14))) || ((((0 % 10) >= 2 && (0 % 10) <= 4)) && !(((0 % 100) >= 12 && (0 % 100) <= 14)))) return 0x01000008u;
      break;
   case 19:
      if ((((n == 0) || (n == 1)))) return 0x01000006u;
      if ((((0 == 0)) && !((n == 0)) && (((n % 1000000) == 0)) && ((0 == 0))) || (!((0 >= 0 && 0 <= 5)))) return 0x01000009u;
      break;
   case 20:
      if ((((n >= 0 && n <= 1)))) return 0x01000006u;
      if ((((0 == 0)) && !((n == 0)) && (((n % 1000000) == 0)) && ((0 == 0))) || (!((0 >= 0 && 0 <= 5)))) return 0x01000009u;
      break;
   case 21:
      if ((((n == 1)) && ((0 == 0)))) return 0x01000006u;
      if ((((0 == 0)) && !((n == 0)) && (((n % 1000000) == 0)) && ((0 == 0))) || (!((0 >= 0 && 0 <= 5)))) return 0x01000009u;
      break;
   case 22:
      if ((((n == 1)))) return 0x01000006u;
      if ((((0 == 0)) && !((n == 0)) && (((n % 1000000) == 0)) && ((0 == 0))) || (!((0 >= 0 && 0 <= 5)))) return 0x01000009u;
      break;
   case 23:
      if ((((n == 1) || (n == 11)))) return 0x01000006u;
      if ((((n == 2) || (n == 12)))) return 0x01000007u;
      if ((((n >= 3 && n <= 10) || (n >= 13 && n <= 19)))) return 0x01000008u;
      break;
   case 24:
      if ((((0 == 0)) && (((n % 100) == 1)))) return 0x01000006u;
      if ((((0 == 0)) && (((n % 100) == 2)))) return 0x01000007u;
      if ((((0 == 0)) && (((n % 100) >= 3 && (n % 100) <= 4))) || (!((0 == 0)))) return 0x01000008u;
      break;
   case 25:
      if ((((0 == 0)) && (((n % 100) == 1))) || ((((0 % 100) == 1)))) return 0x01000006u;
      if ((((0 == 0)) && (((n % 100) == 2))) || ((((0 % 100) == 2)))) return 0x01000007u;
      if ((((0 == 0)) && (((n % 100) >= 3 && (n % 100) <= 4))) || ((((0 % 100) >= 3 && (0 % 100) <= 4)))) return 0x01000008u;
      break;
   case 26:
      if ((((n == 1)) && ((0 == 0)))) return 0x01000006u;
      if ((((n == 2)) && ((0 == 0)))) return 0x01000007u;
      if ((((0 == 0)) && !((n >= 0 && n <= 10)) && (((n % 10) == 0)))) return 0x01000009u;
      break;
   case 27:
      if ((((n == 1)) && ((0 == 0)))) return 0x01000006u;
      if ((((n >= 2 && n <= 4)) && ((0 == 0)))) return 0x01000008u;
      if ((!((0 == 0)))) return 0x01000009u;
      break;
   case 28:
      if ((((n == 1)) && ((0 == 0)))) return 0x01000006u;
      if ((((0 == 0)) && (((n % 10) >= 2 && (n % 10) <= 4)) && !(((n % 100) >= 12 && (n % 100) <= 14)))) return 0x01000008u;
      if ((((0 == 0)) && !((n == 1)) && (((n % 10) >= 0 && (n % 10) <= 1))) || (((0 == 0)) && (((n % 10) >= 5 && (n % 10) <= 9))) || (((0 == 0)) && (((n % 100) >= 12 && (n % 100) <= 14)))) return 0x01000009u;
      break;
   case 29:
      if (((((n % 10) == 1)) && !(((n % 100) == 11)))) return 0x01000006u;
      if (((((n % 10) >= 2 && (n % 10) <= 4)) && !(((n % 100) >= 12 && (n % 100) <= 14)))) return 0x01000008u;
      if (((((n % 10) == 0))) || ((((n % 10) >= 5 && (n % 10) <= 9))) || ((((n % 100) >= 11 && (n % 100) <= 14)))) return 0x01000009u;
      break;
   case 30:
      if (((((n % 10) == 1)) && !(((n % 100) >= 11 && (n % 100) <= 19)))) return 0x01000006u;
      if (((((n % 10) >= 2 && (n % 10) <= 9)) && !(((n % 100) >= 11 && (n % 100) <= 19)))) return 0x01000008u;
      if ((!((0 == 0)))) return 0x01000009u;
      break;
   case 31:
      if ((((n == 1)))) return 0x01000006u;
      if ((((n == 0))) || ((((n % 100) >= 2 && (n % 100) <= 10)))) return 0x01000008u;
      if (((((n % 100) >= 11 && (n % 100) <= 19)))) return 0x01000009u;
      break;
   case 32:
      if ((((0 == 0)) && (((n % 10) == 1)) && !(((n % 100) == 11)))) return 0x01000006u;
      if ((((0 == 0)) && (((n % 10) >= 2 && (n % 10) <= 4)) && !(((n % 100) >= 12 && (n % 100) <= 14)))) return 0x01000008u;
      if ((((0 == 0)) && (((n % 10) == 0))) || (((0 == 0)) && (((n % 10) >= 5 && (n % 10) <= 9))) || (((0 == 0)) && (((n % 100) >= 11 && (n % 100) <= 14)))) return 0x01000009u;
      break;
   case 33:
      if (((((n % 10) == 1)) && !(((n % 100) == 11) || ((n % 100) == 71) || ((n % 100) == 91)))) return 0x01000006u;
      if (((((n % 10) == 2)) && !(((n % 100) == 12) || ((n % 100) == 72) || ((n % 100) == 92)))) return 0x01000007u;
      if (((((n % 10) >= 3 && (n % 10) <= 4) || ((n % 10) == 9)) && !(((n % 100) >= 10 && (n % 100) <= 19) || ((n % 100) >= 70 && (n % 100) <= 79) || ((n % 100) >= 90 && (n % 100) <= 99)))) return 0x01000008u;
      if ((!((n == 0)) && (((n % 1000000) == 0)))) return 0x01000009u;
      break;
   case 34:
      if ((((n == 1)))) return 0x01000006u;
      if ((((n == 2)))) return 0x01000007u;
      if ((((n >= 3 && n <= 6)))) return 0x01000008u;
      if ((((n >= 7 && n <= 10)))) return 0x01000009u;
      break;
   case 35:
      if ((((0 == 0)) && (((n % 10) == 1)))) return 0x01000006u;
      if ((((0 == 0)) && (((n % 10) == 2)))) return 0x01000007u;
      if ((((0 == 0)) && (((n % 100) == 0) || ((n % 100) == 20) || ((n % 100) == 40) || ((n % 100) == 60) || ((n % 100) == 80)))) return 0x01000008u;
      if ((!((0 == 0)))) return 0x01000009u;
      break;
   case 36:
      if ((((n == 0)))) return 0x01000005u;
      if ((((n == 1)))) return 0x01000006u;
      if (((((n % 100) == 2) || ((n % 100) == 22) || ((n % 100) == 42) || ((n % 100) == 62) || ((n % 100) == 82))) || ((((n % 1000) == 0)) && (((n % 100000) >= 1000 && (n % 100000) <= 20000) || ((n % 100000) == 40000) || ((n % 100000) == 60000) || ((n % 100000) == 80000))) || (!((n == 0)) && (((n % 1000000) == 100000)))) return 0x01000007u;
      if (((((n % 100) == 3) || ((n % 100) == 23) || ((n % 100) == 43) || ((n % 100) == 63) || ((n % 100) == 83)))) return 0x01000008u;
      if ((!((n == 1)) && (((n % 100) == 1) || ((n % 100) == 21) || ((n % 100) == 41) || ((n % 100) == 61) || ((n % 100) == 81)))) return 0x01000009u;
      break;
   case 37:
      if ((((n == 0)))) return 0x01000005u;
      if ((((n == 1)))) return 0x01000006u;
      if ((((n == 2)))) return 0x01000007u;
      if (((((n % 100) >= 3 && (n % 100) <= 10)))) return 0x01000008u;
      if (((((n % 100) >= 11 && (n % 100) <= 99)))) return 0x01000009u;
      break;
   case 38:
      if ((((n == 0)))) return 0x01000005u;
      if ((((n == 1)))) return 0x01000006u;
      if ((((n == 2)))) return 0x01000007u;
      if ((((n == 3)))) return 0x01000008u;
      if ((((n == 6)))) return 0x01000009u;
      break;
   default: break;
   }
   return 0x01000004u;
}
/* END GENERATED INTEGER PLURALS */

/* Resources uses the configuration locale, not Locale.getDefault(), to pick
 * cardinal bag entries. Integer rules are generated from Unicode CLDR. */
static bool res_quantity_locale(struct dvm *vm,dvm_ref resources,union dvm_value *locale)
{
   union dvm_value config={0},locales={0},index={.i=0};
   return am_vcall(vm,resources,"getConfiguration","()Landroid/content/res/Configuration;",NULL,0,&config) &&
      am_vcall(vm,config.l,"getLocales","()Landroid/os/LocaleList;",NULL,0,&locales) &&
      am_vcall(vm,locales.l,"get","(I)Ljava/util/Locale;",&index,1,locale);
}
static bool res_quantity_text(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   (void)nargs;
   union dvm_value locale={0},name={0};
   if (!res_quantity_locale(vm,self,&locale) ||
       !am_vcall(vm,locale.l,"toString","()Ljava/lang/String;",NULL,0,&name)) return false;
   uint32_t attr=integer_plural_attr(dvm_string_utf8(vm,name.l),ARG(1).i);
   ArmExecResourceValue *entries=NULL;size_t count=0;
   int status=arm_exec_apk_resource_bag(ARG(0).u,&entries,&count);
   if (status<0) {free(entries);dvm__throw(vm,"java/lang/OutOfMemoryError","Plural resource");return false;}
   const char *text=NULL; uint32_t text_id=0;
   for(unsigned pass=0;status==1 && pass<2 && !text;++pass) {
      uint32_t wanted=pass ? 0x01000004u : attr;
      for(size_t i=0;i<count;++i) if(entries[i].key==wanted) {
         if(entries[i].type=='L') text=entries[i].text;
         else if(entries[i].type==1) {
            int32_t ignored=0;
            if(arm_exec_apk_resource_value(entries[i].data,&ignored,&text)=='L') text_id=entries[i].data;
            else text=NULL;
         }
         break;
      }
   }
   dvm_ref result=text ? (text_id ? resource_styled_text(vm,text_id,text) : dvm_new_string(vm,text)) : 0;
   free(entries);
   if(!text) {dvm__throw(vm,"android/content/res/Resources$NotFoundException",
      "Plural resource #0x%08x quantity=%d",ARG(0).u,ARG(1).i);return false;}
   if(dvm_exception(vm))return false;
   RETL(result);
}
static bool res_quantity_string(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                int nargs,union dvm_value *out)
{
   union dvm_value text={0};
   if(!res_quantity_text(vm,self,args,2,&text))return false;
   union dvm_value format={.l=dvm_new_string(vm,rt_charseq_utf8(vm,text.l))};
   if(nargs<3)RETL(format.l);
   union dvm_value locale={0};
   if(!res_quantity_locale(vm,self,&locale))return false;
   union dvm_value values[]={locale,format,ARG(2)};
   return s_format(vm,0,values,3,out);
}

/* Expand the compiled bag; a scalar or absent resource is NotFoundException. */
static bool res_obtain_typed_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                    int nargs,union dvm_value *out)
{
   (void)self; (void)nargs;
   ArmExecResourceValue *entries=NULL; size_t count=0;
   int status=arm_exec_apk_resource_bag(ARG(0).u,&entries,&count);
   if (status!=1 || count>INT32_MAX) {
      free(entries);
      dvm__throw(vm,status<0 || count>INT32_MAX ? "java/lang/OutOfMemoryError" :
         "android/content/res/Resources$NotFoundException","Array resource #0x%08x",ARG(0).u);
      return false;
   }
   dvm_ref ta=dvm_new_object(vm,dvm_find_class(vm,"android/content/res/TypedArray"));
   if (!ta) {free(entries); return false;}
   dvm_pin(vm,ta);
   const char *fields[]={"attrs","xmlNames","xmlTypes","xmlData","xmlStrings"};
   dvm_ref arrays[5]={0}; bool good=true;
   for(int i=0;i<5;++i) {
      arrays[i]=dvm_new_array(vm,i==4 ? 'L' : 'I',i==4 ? "Ljava/lang/String;" : "I",(uint32_t)count);
      if (!arrays[i]) {good=false; break;}
      (void)dvm_set_field(vm,ta,fields[i],i==4 ? "[Ljava/lang/String;" : "[I",(union dvm_value){.l=arrays[i]});
   }
   if (good) {
      uint32_t *attrs=dvm_array_data(vm,arrays[0]),*names=dvm_array_data(vm,arrays[1]),*data=dvm_array_data(vm,arrays[3]);
      int32_t *types=dvm_array_data(vm,arrays[2]); dvm_ref *texts=dvm_array_data(vm,arrays[4]);
      for(size_t i=0;i<count;++i) {
         attrs[i]=names[i]=entries[i].key; types[i]=entries[i].type; data[i]=entries[i].data;
         if (entries[i].text) texts[i]=dvm_new_string(vm,entries[i].text);
      }
   }
   free(entries); dvm_unpin(vm,ta);
   if (!good || dvm_exception(vm)) return false;
   RETL(ta);
}

static bool res_get_string_array(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)nargs;
   const char **values = NULL;
   size_t n = 0;
   int status = arm_exec_apk_string_array((uint32_t)ARG(0).i, &values, &n);
   if (status != 1 || n > INT32_MAX) {
      free(values);
      dvm__throw(vm, status < 0 || n > INT32_MAX ? "java/lang/OutOfMemoryError"
         : "android/content/res/Resources$NotFoundException",
         "String array resource #0x%08x", (unsigned)ARG(0).i);
      return false;
   }
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
   dvm_ref *slots = arr ? dvm_array_data(vm, arr) : NULL;
   if (slots) for (size_t i = 0; i < n; ++i)
      if (values[i]) slots[i] = dvm_new_string(vm, values[i]);
   free(values);
   RETL(arr);
}

static bool res_getIdentifier(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self;
   const char *name = nargs > 0 ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!name) {
      dvm__throw(vm,"java/lang/NullPointerException","resource name");
      return false;
   }
   /* Android accepts decimal IDs before interpreting package:type/name. */
   char *end=NULL;
   long numeric=strtol(name,&end,10);
   if (*name && end && !*end && numeric>=INT32_MIN && numeric<=INT32_MAX)
      RETI((int32_t)numeric);
   const char *type = nargs > 1 ? dvm_string_utf8(vm, ARG(1).l) : NULL;
   const char *pkg = nargs > 2 ? dvm_string_utf8(vm, ARG(2).l) : NULL;
   char *qualified=strdup(name);
   if (!qualified) {
      dvm__throw(vm,"java/lang/OutOfMemoryError","resource name");
      return false;
   }
   char *entry=qualified, *colon=strchr(entry,':');
   if (colon) { *colon=0; pkg=entry; entry=colon+1; }
   char *slash=strchr(entry,'/');
   if (slash) { *slash=0; type=entry; entry=slash+1; }
   uint32_t id=0;
   const char *app=getenv("ANDROID_PACKAGE_NAME");
   if (pkg && !strcmp(pkg,"android")) {
      if (type && !strcmp(type,"string")) id=luna_framework_string_id(entry);
      else if(type && !strcmp(type,"color")) id=luna_framework_color_id(entry);
   } else if (!pkg || !*pkg || (app && !strcmp(pkg,app)))
      id=arm_exec_apk_resource_id(type,entry);
   free(qualified);
   RETI((int32_t)id);
}

/* getResourceEntryName / getResourceTypeName / getResourceName(id): the name
 * parts of an id, or Resources.NotFoundException when the id has none. */
static bool res_name_part(struct dvm *vm, const union dvm_value *args, int nargs,
                          union dvm_value *out, int part)
{
   char type[64], entry[256];
   const char *framework=nargs ? luna_framework_color_name((uint32_t)args[0].i) : NULL;
   if(framework) {snprintf(type,sizeof type,"color");snprintf(entry,sizeof entry,"%s",framework);}
   if (!nargs || (!framework && !arm_exec_apk_resource_name((uint32_t)args[0].i, type, sizeof type,
                                             entry, sizeof entry))) {
      dvm__throw(vm, "android/content/res/Resources$NotFoundException",
                 "Unable to find resource ID #0x%x", nargs ? (unsigned)args[0].i : 0u);
      return false;
   }
   if (part == 0) { out->l = dvm_new_string(vm, entry); return true; }
   if (part == 1) { out->l = dvm_new_string(vm, type); return true; }
   const char *pkg = getenv("ANDROID_PACKAGE_NAME");
   char full[400];
   snprintf(full, sizeof full, "%s:%s/%s", ((uint32_t)args[0].i>>24)==1 ? "android" : pkg && *pkg ? pkg : "com.lunaria.app", type, entry);
   out->l = dvm_new_string(vm, full);
   return true;
}

static bool res_getResourceEntryName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                     int nargs, union dvm_value *out)
{ (void)self; return res_name_part(vm, args, nargs, out, 0); }

static bool res_getResourceTypeName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{ (void)self; return res_name_part(vm, args, nargs, out, 1); }

static bool res_getResourceName(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{ (void)self; return res_name_part(vm, args, nargs, out, 2); }

static bool res_getResourcePackageName(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   if(nargs && ((uint32_t)ARG(0).i>>24)==1) RETL(dvm_new_string(vm,"android"));
   const char *pkg = getenv("ANDROID_PACKAGE_NAME");
   RETL(dvm_new_string(vm, pkg && *pkg ? pkg : "com.lunaria.app"));
}

/* A resource's value with references followed: a dimension that is an alias of
 * another one (@dimen/a = @dimen/b) reads as the dimension it names. */
static int res_value_resolved(uint32_t id,int32_t *value,const char **string)
{
   for(unsigned hop=0;hop<32;++hop) {
      if(luna_framework_color_value(id,value)) {if(string) *string=NULL;return 0x1c;}
      int kind=arm_exec_apk_resource_value(id,value,string);
      if(kind!=1) return kind;
      uint32_t next=(uint32_t)*value;if(next==id) return 0;id=next;
   }
   return 0;
}

static bool res_getBoolean(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   int32_t value = 0;
   const char *unused = NULL;
   (void)res_value_resolved((uint32_t)ARG(0).i, &value, &unused);
   RETI(value != 0);
}

/* A packed TypedValue complex, as TypedValue.complexToDimension() reads it.
 *
 * Two things here were the wrong way round, and both turned a real length into
 * approximately nothing:
 *
 *  - The radix multipliers were reversed.  Android's RADIX_MULTS apply to the
 *    mantissa *in place* (with its 8-bit shift still on), so once the mantissa
 *    has been shifted down they are {1, 1/128, 1/32768, 1/8388608}.  Radix 0
 *    is the common case — every whole-number dip in every layout — and it was
 *    being divided by 8388608, so `android:layout_height="300dip"` came out as
 *    0.0000358 and rounded to nothing.  A view with no height is not a view
 *    with a wrong height: it does not appear at all.
 *  - The unit codes were off by one from case 2 on, so sp was scaled as pt,
 *    pt as in, and in as mm.
 *
 * Units: 0 px, 1 dip, 2 sp, 3 pt, 4 in, 5 mm.  dip and sp scale with the
 * density of the screen (see DisplayMetrics); the font scale is 1. */
static float android_complex_dimension(int32_t complex)
{
   static const float radix[] = {
      1.0f, 1.0f / 128.0f, 1.0f / 32768.0f, 1.0f / 8388608.0f
   };
   int32_t mantissa = complex >> 8;
   unsigned r = ((uint32_t)complex >> 4) & 3u;
   float value = mantissa * radix[r];
   const float dpi = (float)device_density_dpi();
   switch ((uint32_t)complex & 15u) {
   case 0: return value;                            /* px */
   case 1: case 2: return value * (dpi / 160.0f);   /* dip, sp (font scale 1) */
   case 3: return value * (dpi / 72.0f);            /* pt */
   case 4: return value * dpi;                      /* in */
   case 5: return value * (dpi / 25.4f);            /* mm */
   default: return value;
   }
}

static bool res_get_dimension(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   int32_t value = 0; const char *string = NULL;
   (void)res_value_resolved((uint32_t)ARG(0).i, &value, &string);
   RETF(android_complex_dimension(value));
}

static bool res_get_dimension_pixels(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   union dvm_value dimension = { 0 };
   if (!res_get_dimension(vm, self, args, nargs, &dimension)) return false;
   float f = dimension.f;
   int32_t rounded = dimension_pixel_int(f >= 0 ? f + .5f : f - .5f);
   RETI(rounded ? rounded : (f > 0 ? 1 : f < 0 ? -1 : 0));
}

static bool res_get_integer(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   int32_t value = 0; const char *string = NULL;
   (void)res_value_resolved((uint32_t)ARG(0).i, &value, &string);
   RETI(value);
}

static bool res_get_value(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self;
   int32_t value = 0; const char *string = NULL;
   int kind = arm_exec_apk_resource_value((uint32_t)ARG(0).i, &value, &string);
   if (nargs > 1) typed_value_assign(vm, ARG(1).l, kind, value, string);
   RETV();
}

static bool res_open_raw_resource(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)self;
   uint32_t id = nargs ? (uint32_t)ARG(0).i : 0;
   int32_t data_value = 0; const char *path = NULL;
   int kind = arm_exec_apk_resource_value(id, &data_value, &path);
   if (nargs > 1 && ARG(1).l) {
      typed_value_assign(vm, ARG(1).l, kind, data_value, path);
      union dvm_value resource = {.i = (int32_t)id};
      (void)dvm_set_field(vm, ARG(1).l, "resourceId", "I", resource);
   }
   size_t length = 0;
   unsigned char *bytes = kind == 'L' && path ? arm_exec_asset_read(path, &length) : NULL;
   if (!bytes) {
      dvm__throw(vm, "android/content/res/Resources$NotFoundException", "Raw resource 0x%08x", id);
      return false;
   }
   if (length > INT32_MAX) {
      free(bytes);
      dvm__throw(vm, "java/lang/OutOfMemoryError", "raw resource exceeds Java array size");
      return false;
   }
   dvm_ref array = dvm_new_array(vm, 'B', "B", (uint32_t)length);
   if (!array) { free(bytes); return false; }
   if (length) memcpy(dvm_array_data(vm, array), bytes, length);
   free(bytes);
   RETL(bytes_input_stream(vm, array));
}

static bool res_get_xml(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm,
      "Landroid/content/res/XmlResourceParser;");
   dvm_ref parser = c ? dvm_new_object(vm, c) : 0;
   if (parser) {
      union dvm_value id = { .i = ARG(0).i };
      (void)dvm_set_field(vm, parser, "resourceId", "I", id);
   }
   RETL(parser);
}

static bool res_get_color_state_list(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self;
   dvm_ref colors=color_value_list(vm,1,ARG(0).i,NULL,nargs>1 ? ARG(1).l : 0,0);
   if (dvm__env("LUNARIA_TRACE_TA"))
      fprintf(stderr,"[ta-color] Resources self=%u id=0x%08x theme=%u -> ref=%u exception=%u\n",
              self,ARG(0).u,nargs>1 ? ARG(1).l : 0,colors,dvm_exception(vm));
   if (!colors) return false;
   RETL(colors);
}

/* Resources.getColor(int) — the packed ARGB of a colour resource.  Left
 * unresolved it answered 0, transparent black, so a widget that asks the
 * theme for its own colour drew invisible. */
static bool res_get_color(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self;
   return color_value_argb(vm,1,ARG(0).i,NULL,nargs>1 ? ARG(1).l : 0,0,out);
}

/* Resources.getDrawable(id): the class a device builds for this resource.  What
 * the file is decides it: <vector> is a VectorDrawable, <selector> a
 * StateListDrawable, a PNG a BitmapDrawable, and so on.  AppCompat checks exactly
 * that (`drawable.getClass().getName()` against android.graphics.drawable.
 * VectorDrawable) to decide whether the build is configured for vector drawables,
 * and an untyped Drawable made it refuse to start MainTabActivity.  Painting is the
 * view layer's business; what the app can observe here is the type. */
static const char *drawable_class_of_xml(const unsigned char *bytes, size_t n)
{
   uint32_t nstrings = 0;
   char **strings = (n >= 8 && ax_u16(bytes) == 3) ? ax_string_pool(bytes, n, &nstrings) : NULL;
   const char *root = NULL;
   size_t pos = n >= 8 ? ax_u16(bytes + 2) : n;
   while (strings && pos + 8 <= n) {
      uint16_t type = ax_u16(bytes + pos);
      uint32_t size = ax_u32(bytes + pos + 4);
      if (size < 8 || pos + size > n) break;
      if (type == 0x0102 && pos + 24 <= n) {
         uint32_t name_index = ax_u32(bytes + pos + 20);
         if (name_index < nstrings) root = strings[name_index];
         break;
      }
      pos += size;
   }
   static const struct { const char *tag, *cls; } map[] = {
      { "vector", "Landroid/graphics/drawable/VectorDrawable;" },
      { "animated-vector", "Landroid/graphics/drawable/AnimatedVectorDrawable;" },
      { "shape", "Landroid/graphics/drawable/GradientDrawable;" },
      { "selector", "Landroid/graphics/drawable/StateListDrawable;" },
      { "layer-list", "Landroid/graphics/drawable/LayerDrawable;" },
      { "clip", "Landroid/graphics/drawable/ClipDrawable;" },
      { "ripple", "Landroid/graphics/drawable/RippleDrawable;" },
      { "inset", "Landroid/graphics/drawable/InsetDrawable;" },
      { "rotate", "Landroid/graphics/drawable/RotateDrawable;" },
      { "animated-rotate", "Landroid/graphics/drawable/AnimatedRotateDrawable;" },
      { "level-list", "Landroid/graphics/drawable/LevelListDrawable;" },
      { "animation-list", "Landroid/graphics/drawable/AnimationDrawable;" },
      { "color", "Landroid/graphics/drawable/ColorDrawable;" },
      { "bitmap", "Landroid/graphics/drawable/BitmapDrawable;" },
      { "nine-patch", "Landroid/graphics/drawable/NinePatchDrawable;" },
   };
   const char *cls = "Landroid/graphics/drawable/Drawable;";
   for (size_t i = 0; root && i < sizeof map / sizeof map[0]; ++i)
      if (!strcmp(root, map[i].tag)) { cls = map[i].cls; break; }
   for (uint32_t i = 0; strings && i < nstrings; ++i) free(strings[i]);
   free(strings);
   return cls;
}

static dvm_ref drawable_from_resource(struct dvm *vm, uint32_t id, unsigned depth, dvm_ref theme);

static bool res_get_drawable(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   union dvm_value theme = {0};
   if (nargs > 1) theme = ARG(1);
   else if (object_is_a(vm, self, "Landroid/content/Context;")) {
      if (!am_vcall(vm, self, "getTheme", "()Landroid/content/res/Resources$Theme;", NULL, 0, &theme)) return false;
   }
   uint32_t resolved_id=nargs>0 ? (uint32_t)ARG(0).i : 0;
   for (unsigned depth=0;resolved_id;++depth) {
      int32_t reference=0; const char *text=NULL;
      int kind=arm_exec_apk_resource_value(resolved_id,&reference,&text);
      if (kind!=0x01 /* TYPE_REFERENCE */) break;
      if (depth>=32 || (uint32_t)reference==resolved_id) {
         dvm__throw(vm,"android/content/res/Resources$NotFoundException",
                     "Cyclic drawable resource reference 0x%08x",(unsigned)ARG(0).i);
         return false;
      }
      resolved_id=(uint32_t)reference;
   }
   if (nargs > 0) {
      dvm_ref parsed = drawable_from_resource(vm, resolved_id, 0, theme.l);
      if (dvm_exception(vm)) {
         dvm_ref cause = dvm_exception(vm);
         dvm_pin(vm, cause);
         dvm_clear_exception(vm);
         dvm__throw(vm, "android/content/res/Resources$NotFoundException",
                     "Error inflating drawable resource 0x%08x", (unsigned)ARG(0).i);
         (void)dvm_set_field(vm, dvm_exception(vm), "cause", "Ljava/lang/Throwable;",
                            (union dvm_value){.l = cause});
         dvm_unpin(vm, cause);
         return false;
      }
      if (parsed) RETL(parsed);
   }
   const char *cls = "Landroid/graphics/drawable/Drawable;";
   int32_t iv = 0; const char *path = NULL;
   int resource_kind=nargs>0 ? res_value_resolved(resolved_id,&iv,&path) : 0;
   if (resource_kind>=0x1c && resource_kind<=0x1f) {
      struct dvm_class *color=dvm__class_by_desc(vm,"Landroid/graphics/drawable/ColorDrawable;");
      dvm_ref drawable=color ? dvm_new_object(vm,color) : 0;
      if (!drawable) return false;
      dvm_pin(vm,drawable);
      union dvm_value value={.i=iv},ignored={0};
      bool good=am_vcall(vm,drawable,"<init>","(I)V",&value,1,&ignored);
      dvm_unpin(vm,drawable);
      if (!good) return false;
      RETL(drawable);
   }
   if (resource_kind == 'L' && path) {
      const size_t len = strlen(path);
      if (len > 4 && !strcasecmp(path + len - 4, ".xml")) {
         size_t n = 0;
         unsigned char *bytes = arm_exec_asset_read(path, &n);
         if (bytes) { cls = drawable_class_of_xml(bytes, n); free(bytes); }
      } else if (len > 6 && !strcasecmp(path + len - 6, ".9.png")) {
         cls = "Landroid/graphics/drawable/NinePatchDrawable;";
      } else if (len > 4) {
         cls = "Landroid/graphics/drawable/BitmapDrawable;";
      }
   }
   struct dvm_class *c = dvm__class_by_desc(vm, cls);
   if (!c) c = dvm__class_by_desc(vm, "Landroid/graphics/drawable/Drawable;");
   if (!strcmp(cls, "Landroid/graphics/drawable/BitmapDrawable;")) {
      struct dvm_class *factory = dvm_find_class(vm, "android/graphics/BitmapFactory");
      struct dvm_method *decode = dvm_find_method(vm, factory, "decodeResource",
         "(Landroid/content/res/Resources;I)Landroid/graphics/Bitmap;");
      union dvm_value input[] = {{.l = self}, {.i=(int32_t)resolved_id}}, bitmap = {0};
      if (!decode || !dvm_call(vm, decode, 0, input, 2, &bitmap)) return false;
      if (!bitmap.l) {
         dvm__throw(vm, "android/content/res/Resources$NotFoundException",
                     "Cannot decode drawable resource 0x%08x", (unsigned)ARG(0).i);
         return false;
      }
      dvm_pin(vm, bitmap.l);
      dvm_ref drawable = c ? dvm_new_object(vm, c) : 0;
      if (drawable) {
         dvm_pin(vm, drawable);
         struct dvm_method *init = dvm_find_method(vm, c, "<init>",
            "(Landroid/content/res/Resources;Landroid/graphics/Bitmap;)V");
         union dvm_value constructor[] = {{.l = self}, bitmap}, ignored = {0};
         bool initialized = init && dvm_call(vm, init, drawable, constructor, 2, &ignored);
         dvm_unpin(vm, drawable);
         dvm_unpin(vm, bitmap.l);
         if (!initialized) return false;
         RETL(drawable);
      }
      dvm_unpin(vm, bitmap.l);
      return false;
   }
   RETL(c ? dvm_new_object(vm, c) : 0);
}

static bool res_getConfiguration(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value stored = { 0 };
   (void)dvm_get_field(vm, self, "configuration",
                       "Landroid/content/res/Configuration;", &stored);
   dvm_ref configuration = stored.l;
   if (!configuration) {
      struct dvm_class *c =
         dvm__class_by_desc(vm, "Landroid/content/res/Configuration;");
      configuration = c ? dvm_new_object(vm, c) : 0;
      if (configuration) {
         dvm_set_field(vm,configuration,"fontScale","F",(union dvm_value){.f=1});
         configuration_ensure_locale(vm, configuration);
         stored.l = configuration;
         (void)dvm_set_field(vm, self, "configuration",
                             "Landroid/content/res/Configuration;", stored);
         dvm_pin(vm, configuration);
      }
   } else {
      configuration_ensure_locale(vm, configuration);
   }
   RETL(configuration);
}

static void resources_scale_metrics(struct dvm *vm,dvm_ref configuration,dvm_ref metrics)
{
   union dvm_value scale={0},density={0};
   dvm_get_field(vm,configuration,"fontScale","F",&scale);
   dvm_get_field(vm,metrics,"density","F",&density);
   density.f*=scale.f!=0 ? scale.f : 1;
   dvm_set_field(vm,metrics,"scaledDensity","F",density);
}

static bool res_get_display_metrics(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value dm = { 0 };
   if (self)
      (void)dvm_get_field(vm, self, "displayMetrics",
                          "Landroid/util/DisplayMetrics;", &dm);
   if (!dm.l) {
      struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/util/DisplayMetrics;");
      dm.l = c ? dvm_new_object(vm, c) : 0;
      if (dm.l && self)
         (void)dvm_set_field(vm, self, "displayMetrics",
                             "Landroid/util/DisplayMetrics;", dm);
      if (dm.l) {
         rt_fill_display_metrics(vm, dm.l);
         union dvm_value config={0};
         if(!res_getConfiguration(vm,self,NULL,0,&config)) return false;
         resources_scale_metrics(vm,config.l,dm.l);
      }
   }
   RETL(dm.l);
}

static bool res_update_configuration(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   if (nargs && ARG(0).l) {
      union dvm_value current = { 0 };
      (void)res_getConfiguration(vm, self, NULL, 0, &current);
      if (current.l) {
         union dvm_value previous_scale={0},new_scale={0};
         dvm_get_field(vm,current.l,"fontScale","F",&previous_scale);
         dvm_get_field(vm,ARG(0).l,"fontScale","F",&new_scale);
         union dvm_value source = { .l = ARG(0).l };
         (void)cfg_set_to(vm, current.l, &source, 1, out);
         /* Undefined override fields do not erase the effective value. */
         if(!(new_scale.f>0)) dvm_set_field(vm,current.l,"fontScale","F",previous_scale);
         union dvm_value metrics={0};
         if(!res_get_display_metrics(vm,self,NULL,0,&metrics)) return false;
         resources_scale_metrics(vm,current.l,metrics.l);
      }
   }
   RETV();
}

static bool res_getSystem(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   /* Same process-wide Resources the Context path already serves. */
   static dvm_ref system_resources;
   RETL(runtime_singleton(vm, "Landroid/content/res/Resources;",
                          &system_resources));
}

static const struct rt_method rt_resources[] = {
   M("openRawResource", "(I)Ljava/io/InputStream;", res_open_raw_resource),
   M("openRawResource", "(ILandroid/util/TypedValue;)Ljava/io/InputStream;", res_open_raw_resource),
   M("getString", "(I)Ljava/lang/String;", res_getString),
   M("getString", "(I[Ljava/lang/Object;)Ljava/lang/String;", context_get_string),
   M("getQuantityText", "(II)Ljava/lang/CharSequence;", res_quantity_text),
   M("getQuantityString", "(II)Ljava/lang/String;", res_quantity_string),
   M("getQuantityString", "(II[Ljava/lang/Object;)Ljava/lang/String;", res_quantity_string),
   M("getText", "(I)Ljava/lang/CharSequence;", res_get_text),
   M("getText", "(ILjava/lang/CharSequence;)Ljava/lang/CharSequence;", res_get_text),
   M("getResourceEntryName", "(I)Ljava/lang/String;", res_getResourceEntryName),
   M("getResourceTypeName", "(I)Ljava/lang/String;", res_getResourceTypeName),
   M("getResourceName", "(I)Ljava/lang/String;", res_getResourceName),
   M("getIdentifier",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
     res_getIdentifier),
   M("getResourcePackageName", "(I)Ljava/lang/String;",
     res_getResourcePackageName),
   M("getBoolean", "(I)Z", res_getBoolean),
   M("getDimension", "(I)F", res_get_dimension),
   M("getDimensionPixelSize", "(I)I", res_get_dimension_pixels),
   M("getInteger", "(I)I", res_get_integer),
   M("getValue", "(ILandroid/util/TypedValue;Z)V", res_get_value),
   M("getXml", "(I)Landroid/content/res/XmlResourceParser;", res_get_xml),
   /* getLayout()/getAnimation() are the same call for another resource type;
    * menu inflation and animation loading read them through here. */
   M("getLayout", "(I)Landroid/content/res/XmlResourceParser;", res_get_xml),
   M("getAnimation", "(I)Landroid/content/res/XmlResourceParser;", res_get_xml),
   M("getColorStateList","(I)Landroid/content/res/ColorStateList;",res_get_color_state_list),
   M("getColorStateList",
     "(ILandroid/content/res/Resources$Theme;)Landroid/content/res/ColorStateList;",
     res_get_color_state_list),
   /* getColor() is the packed ARGB of a colour resource.  Unresolved it
      answered 0 — transparent black — so a widget that asks the theme for its
      own colour drew invisible. */
   M("getColor", "(I)I", res_get_color),
   M("getColor", "(ILandroid/content/res/Resources$Theme;)I", res_get_color),
   M("getStringArray", "(I)[Ljava/lang/String;", res_get_string_array),
   M("getTextArray", "(I)[Ljava/lang/CharSequence;", res_get_string_array),
   M("obtainTypedArray", "(I)Landroid/content/res/TypedArray;",res_obtain_typed_array),
   M("getDrawable", "(I)Landroid/graphics/drawable/Drawable;", res_get_drawable),
   M("getDrawable", "(ILandroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;", res_get_drawable),
   M("getConfiguration", "()Landroid/content/res/Configuration;",
     res_getConfiguration),
   M("getDisplayMetrics", "()Landroid/util/DisplayMetrics;",
     res_get_display_metrics),
   SM("getSystem", "()Landroid/content/res/Resources;", res_getSystem),
   M("updateConfiguration",
     "(Landroid/content/res/Configuration;Landroid/util/DisplayMetrics;)V",
     res_update_configuration),
   M("newTheme", "()Landroid/content/res/Resources$Theme;", resources_new_theme),
   M_END,
};

static const struct rt_field rt_resources_fields[] = {
   { "configuration", "Landroid/content/res/Configuration;" },
   { "displayMetrics", "Landroid/util/DisplayMetrics;" },
   F_END
};

static bool theme_apply_style(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs;
   if (dvm__env("LUNARIA_TRACE_TA"))
      fprintf(stderr, "[theme] apply theme=%x style=%08x force=%d\n",
              self, (unsigned)ARG(0).i, ARG(1).i != 0);
   if (!ARG(0).i) RETV();
   union dvm_value history = {0}, previous = {0};
   (void)dvm_get_field(vm, self, "styleApplications", "[I", &history);
   (void)dvm_get_field(vm, self, "style", "I", &previous);
   uint32_t n = dvm_array_length(vm, history.l);
   uint32_t base = !n && previous.i ? 2 : 0;
   if (n > INT32_MAX - 4) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "theme styles"); return false;
   }
   dvm_ref next = dvm_new_array(vm, 'I', "I", n + base + 2);
   int32_t *pairs = dvm_array_data(vm, next);
   if (!pairs) return false;
   if (n) memcpy(pairs, dvm_array_data(vm, history.l), n * sizeof *pairs);
   if (base) { pairs[0] = previous.i; pairs[1] = 1; }
   pairs[n+base] = ARG(0).i; pairs[n+base+1] = ARG(1).i != 0;
   (void)dvm_set_field(vm, self, "styleApplications", "[I", (union dvm_value){ .l = next });
   (void)dvm_set_field(vm, self, "style", "I", ARG(0));
   RETV();
}

static bool theme_set_to(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "null source theme"); return false;
   }
   union dvm_value value = {0};
   (void)dvm_get_field(vm, ARG(0).l, "style", "I", &value);
   (void)dvm_set_field(vm, self, "style", "I", value);
   (void)dvm_get_field(vm, ARG(0).l, "styleApplications", "[I", &value);
   (void)dvm_set_field(vm, self, "styleApplications", "[I", value);
   RETV();
}

static bool theme_resolve_attribute(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   int32_t value = 0; const char *string = NULL;
   int kind = theme_style_value(vm, self, (uint32_t)ARG(0).i, &value, &string);
   /* Theme attribute aliases are followed even when resource resolution is
    * disabled. resolveRefs controls the subsequent resource lookup. */
   uint32_t visited[64]; unsigned count = 0;
   while (kind == 2) {
      uint32_t attr = (uint32_t)value;
      if (count == 64) { kind = 0; break; }
      for (unsigned i = 0; i < count; ++i)
         if (visited[i] == attr) { kind = 0; break; }
      if (!kind) break;
      visited[count++] = attr;
      kind = theme_style_value(vm, self, attr, &value, &string);
   }
   uint32_t resource_id = 0;
   if (nargs > 2 && ARG(2).i) {
      count = 0;
      while (kind == 1 && value) {
         uint32_t id = (uint32_t)value;
         if (count == 64) { kind = 0; break; }
         for (unsigned i = 0; i < count; ++i)
            if (visited[i] == id) { kind = 0; break; }
         if (!kind) break;
         visited[count++] = id;
         resource_id = id;
         string = NULL;
         if (luna_framework_color_value(id, &value)) kind = 0x1c;
         else {
            kind = arm_exec_apk_resource_value(id, &value, &string);
            if (!kind) {
               /* AssetManager resolves a complex resource to a reference
                * to its bag. A style is not a missing scalar resource. */
               if (arm_exec_apk_resource_is_bag(id)) {
                  kind = 1; value = (int32_t)id; break;
               }
            }
         }
      }
   }
   if (dvm__env("LUNARIA_TRACE_TA"))
      fprintf(stderr, "[theme] resolve theme=%x attr=%08x kind=%x value=%08x\n",
              self, (unsigned)ARG(0).i, kind, (unsigned)value);
   if (kind && nargs > 1) {
      typed_value_assign(vm, ARG(1).l, kind, value, string);
      if (resource_id)
         (void)dvm_set_field(vm, ARG(1).l, "resourceId", "I",
                            (union dvm_value){.u = resource_id});
   }
   RETI(kind != 0);
}

static bool xml_as_attribute_set(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{ (void)vm; (void)self; RETL(nargs ? ARG(0).l : 0); }
static bool xml_new_pull(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)self;(void)args;(void)nargs;
 dvm_ref parser=dvm_new_object(vm,dvm_find_class(vm,"org/kxml2/io/KXmlParser"));if(!parser)return false;
 dvm_set_field(vm,parser,"namespaceAware","Z",(union dvm_value){.i=1});
 dvm_set_field(vm,parser,"processDocDecl","Z",(union dvm_value){.i=1});rt_set_int(vm,parser,"attributeCount",-1);RETL(parser);
}
static const struct rt_method rt_xml[] = {
   SM("newPullParser","()Lorg/xmlpull/v1/XmlPullParser;",xml_new_pull),
   SM("asAttributeSet",
      "(Lorg/xmlpull/v1/XmlPullParser;)Landroid/util/AttributeSet;",
      xml_as_attribute_set), M_END,
};

static bool state_set_trim(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self;
   struct dvm_object *source = nargs ? dvm__obj(vm, ARG(0).l) : NULL;
   int32_t count = nargs > 1 ? ARG(1).i : 0;
   if (!source || source->kind != DVM_OBJ_ARRAY || count < 0 ||
       (uint32_t)count > source->length) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "state set size");
      return false;
   }
   if ((uint32_t)count == source->length) RETL(ARG(0).l);
   dvm_ref result = dvm_new_array(vm, 'I', "I", (uint32_t)count);
   struct dvm_object *array = result ? dvm__obj(vm, result) : NULL;
   if (array && array->data && count) memcpy(array->data, source->data,
                                             (size_t)count * sizeof(int32_t));
   RETL(result);
}
static const struct rt_method rt_state_set[] = {
   SM("trimStateSet", "([II)[I", state_set_trim), M_END,
};

/* TypedValue.complexToFloat(): the packed dimension/fraction encoding used by
 * every attribute the resource compiler stored as a complex value.  Bits 0-3
 * are the unit, 4-5 the radix, 8-31 a signed mantissa. */
static bool typed_value_complexToFloat(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)vm; (void)self;
   int32_t complex = nargs > 0 ? ARG(0).i : 0;
   static const float mult = 1.0f / (1 << 8);
   static const float radix[] = {
      1.0f * mult, 1.0f / (1 << 7) * mult,
      1.0f / (1 << 15) * mult, 1.0f / (1 << 23) * mult,
   };
   RETF((float)(complex & (int32_t)0xffffff00) * radix[(complex >> 4) & 3]);
}

static bool typed_value_getFloat(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value data = { 0 };
   (void)dvm_get_field(vm, self, "data", "I", &data);
   union dvm_value bits = { .i = data.i };
   RETF(bits.f);
}

/* TypedValue.applyDimension(unit, value, metrics): COMPLEX_UNIT_PX, DIP, SP,
 * PT, IN, MM in pixels. */
static bool typed_value_apply_dimension(struct dvm *vm, dvm_ref self,
                                        const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
   (void)self; (void)nargs;
   const int unit = ARG(0).i;
   const float value = ARG(1).f;
   union dvm_value density = { 0 }, scaled = { 0 }, xdpi = { 0 };
   if (ARG(2).l) {
      (void)dvm_get_field(vm, ARG(2).l, "density", "F", &density);
      (void)dvm_get_field(vm, ARG(2).l, "scaledDensity", "F", &scaled);
      (void)dvm_get_field(vm, ARG(2).l, "xdpi", "F", &xdpi);
   }
   switch (unit) {
   case 0: RETF(value);                          /* PX */
   case 1: RETF(value * density.f);              /* DIP */
   case 2: RETF(value * scaled.f);               /* SP */
   case 3: RETF(value * xdpi.f * (1.0f / 72.0f));      /* PT */
   case 4: RETF(value * xdpi.f);                 /* IN */
   case 5: RETF(value * xdpi.f * (1.0f / 25.4f));      /* MM */
   default:
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Invalid unit: %d", unit);
      return false;
   }
}

static const struct rt_method rt_typed_value[] = {
   M("<init>", "()V", nop_void),
   SM("applyDimension", "(IFLandroid/util/DisplayMetrics;)F", typed_value_apply_dimension),
   SM("complexToFloat", "(I)F", typed_value_complexToFloat),
   SM("complexToFraction", "(IFF)F", typed_value_complexToFloat),
   M("getFloat", "()F", typed_value_getFloat),
   M_END,
};

static const struct rt_field rt_typed_value_fields[] = {
   { "type", "I" }, { "data", "I" }, { "resourceId", "I" },
   { "string", "Ljava/lang/CharSequence;" }, F_END
};

static const struct rt_field rt_xml_parser_fields[] = {
   {"encoding","Ljava/lang/String;"}, { "input", "Ljava/lang/Object;" }, {"reader","I"}, {"namespaceAware","Z"}, {"processDocDecl","Z"}, {"validating","Z"}, {"empty","I"},
   {"namespace","Ljava/lang/String;"}, {"prefix","Ljava/lang/String;"},
   {"attributeNamespaces","[Ljava/lang/String;"}, {"attributePrefixes","[Ljava/lang/String;"},
   { "resourceId", "I" }, { "eventIndex", "I" }, { "eventType", "I" },
   { "depth", "I" }, { "attributeCount", "I" },
   { "attributeNames", "[I" }, { "attributeTypes", "[I" },
   { "attributeData", "[I" },
   /* An attribute carries both a resource id and the name it was written
    * under; XmlPullParser.getAttributeName() asks for the latter and the
    * resource map cannot answer it, so the string-pool text is kept too. */
   { "attributeNameStrings", "[Ljava/lang/String;" },
   { "attributeValueStrings", "[Ljava/lang/String;" },
   { "name", "Ljava/lang/String;" },
   /* The character data of a TEXT event — <gameCode>enn</gameCode> puts the
    * value there, not in an attribute. */
   { "text", "Ljava/lang/String;" }, F_END
};

/* getAttributeValue() is defined to hand back the *string* form of whatever
 * the attribute holds, the way TypedValue.coerceToString() writes it.  Only an
 * attribute authored as text keeps a string-pool index; aapt compiles
 * location="50001" to a typed integer with no pool entry, and answering null
 * there tells the caller the attribute is absent.  Netmarble's plugin loader
 * reads exactly such an attribute out of res/xml/nmplugin.xml and refused
 * every UIView plugin with "Location is null". */
static dvm_ref ax_value_string(struct dvm *vm, int32_t type, int32_t data)
{
   char text[32];
   switch (type) {
   case 0x00: return 0;                                   /* TYPE_NULL */
   case 0x01: snprintf(text, sizeof text, "@%d", data); break;   /* REFERENCE */
   case 0x02: snprintf(text, sizeof text, "?%d", data); break;   /* ATTRIBUTE */
   case 0x04: {                                           /* TYPE_FLOAT */
      float f;
      memcpy(&f, &data, sizeof f);
      snprintf(text, sizeof text, "%g", (double)f);
      break;
   }
   case 0x11: snprintf(text, sizeof text, "0x%x", (unsigned)data); break;
   case 0x12: snprintf(text, sizeof text, "%s", data ? "true" : "false"); break;
   default:
      /* TYPE_INT_DEC and the colour/int family all print as decimals. */
      if (type < 0x10) return 0;
      snprintf(text, sizeof text, "%d", data);
      break;
   }
   return dvm_new_string(vm, text);
}

/* XML 1.0 pull events. The parser owns UTF-8 tokens, never pointers into VM
 * storage. No host library or DOM: names, attributes and coalesced text only. */
struct pull_event {int type,depth;char *name,*text;char **attrs;size_t count;bool empty;};
struct pull_binding {struct pull_binding *next;char *prefix,*uri;int depth;};
struct pull_entity {struct pull_entity *next;char *name,*value;bool active;};
struct pull_default {struct pull_default *next;char *element,*name,*value;};
struct pull_source {struct pull_source *next;char *bytes;size_t length,pos;int depth;struct pull_entity *entity;};
struct pull_state {
 struct pull_event *events;size_t count,capacity,index;
 char *bytes;size_t length,pos;
 char **stack;size_t stack_cap;int depth;
 struct pull_binding *bindings;
 struct pull_entity *entities;
 struct pull_default *defaults;
 struct pull_source *sources;
 const char *error;bool oom,parsed,namespaces,root,docdecl,doctype,external;

};
static void pull_event_free(struct pull_event *e)
{
 free(e->name);free(e->text);for(size_t i=0;i<e->count*2;++i)free(e->attrs[i]);free(e->attrs);memset(e,0,sizeof *e);
}
static void pull_bindings_pop(struct pull_state *s,int depth)
{
 while(s->bindings && s->bindings->depth>=depth){struct pull_binding *b=s->bindings;s->bindings=b->next;free(b->prefix);free(b->uri);free(b);}
}
static void pull_destroy(void *data)
{
 struct pull_state *s=data;if(!s)return;
 for(size_t i=0;i<s->count;++i)pull_event_free(&s->events[i]);
 for(int i=0;i<s->depth;++i)free(s->stack[i]);
 while(s->sources){struct pull_source *p=s->sources;s->sources=p->next;s->bytes=p->bytes;free(p);}
 while(s->entities){struct pull_entity *p=s->entities;s->entities=p->next;free(p->name);free(p->value);free(p);}
 while(s->defaults){struct pull_default *p=s->defaults;s->defaults=p->next;free(p->element);free(p->name);free(p->value);free(p);}
 pull_bindings_pop(s,0);free(s->stack);free(s->events);free(s->bytes);free(s);
}
static struct pull_event *pull_event_add(struct pull_state *s,int type)
{
 if(s->oom || s->error)return NULL;
 if(s->count==s->capacity){size_t n=s->capacity ? s->capacity*2 : 16;struct pull_event *p=realloc(s->events,n*sizeof *p);if(!p){s->oom=true;return NULL;}s->events=p;s->capacity=n;}
 struct pull_event *e=&s->events[s->count++];memset(e,0,sizeof *e);e->type=type;e->depth=s->depth;return e;
}
static bool pull_space(unsigned char c){return c==' ' || c=='\t' || c=='\r' || c=='\n';}
static void pull_skip_space(struct pull_state *s){while(s->pos<s->length && pull_space((unsigned char)s->bytes[s->pos]))++s->pos;}
static bool pull_at(struct pull_state *s,const char *text)
{size_t n=strlen(text);return n<=s->length-s->pos && !memcmp(s->bytes+s->pos,text,n);}
static bool pull_xml_char(uint32_t c)
{return c==9 || c==10 || c==13 || (c>=0x20 && c<=0xd7ff) || (c>=0xe000 && c<=0xfffd) || (c>=0x10000 && c<=0x10ffff);}
static bool pull_utf8(const char *text,size_t length,size_t *at,uint32_t *cp)
{
 if(*at>=length)return false;unsigned char a=(unsigned char)text[(*at)++];
 if(a<128){*cp=a;return true;}
 int n=a>=0xc2 && a<=0xdf ? 1 : a>=0xe0 && a<=0xef ? 2 : a>=0xf0 && a<=0xf4 ? 3 : -1;
 if(n<0 || (size_t)n>length-*at)return false;uint32_t value=a & ((1u<<(6-n))-1u);
 for(int i=0;i<n;++i){unsigned char b=(unsigned char)text[(*at)++];if((b&0xc0)!=0x80)return false;value=(value<<6)|(b&63);}
 if(value<(n==1 ? 0x80u : n==2 ? 0x800u : 0x10000u) || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;*cp=value;return true;
}
static bool pull_name_start(uint32_t c)
{
 return c==':' || c=='_' || (c>='A' && c<='Z') || (c>='a' && c<='z') ||
  (c>=0xc0 && c<=0xd6) || (c>=0xd8 && c<=0xf6) || (c>=0xf8 && c<=0x2ff) ||
  (c>=0x370 && c<=0x37d) || (c>=0x37f && c<=0x1fff) || (c>=0x200c && c<=0x200d) ||
  (c>=0x2070 && c<=0x218f) || (c>=0x2c00 && c<=0x2fef) || (c>=0x3001 && c<=0xd7ff) ||
  (c>=0xf900 && c<=0xfdcf) || (c>=0xfdf0 && c<=0xfffd) || (c>=0x10000 && c<=0xeffff);
}
static char *pull_name(struct pull_state *s)
{
 size_t start=s->pos,at=start;uint32_t cp;
 if(!pull_utf8(s->bytes,s->length,&at,&cp) || !pull_name_start(cp)){s->error="Invalid XML name";return NULL;}s->pos=at;
 while(s->pos<s->length){at=s->pos;if(!pull_utf8(s->bytes,s->length,&at,&cp)){s->error="Invalid UTF-8";return NULL;}
  if(!pull_name_start(cp) && !(cp=='-' || cp=='.' || (cp>='0' && cp<='9') || cp==0xb7 || (cp>=0x300 && cp<=0x36f) || (cp>=0x203f && cp<=0x2040)))break;s->pos=at;}
 char *name=luna_strndup(s->bytes+start,s->pos-start);if(!name)s->oom=true;return name;
}
static bool pull_append(struct pull_state *s,char **text,size_t *length,const char *bytes,size_t n)
{
 if(n>SIZE_MAX-*length-1){s->oom=true;return false;}char *p=realloc(*text,*length+n+1);if(!p){s->oom=true;return false;}memcpy(p+*length,bytes,n);*length+=n;p[*length]=0;*text=p;return true;
}
static size_t pull_encode(char *b,uint32_t cp)
{
 size_t n=0;
 if(cp<128)b[n++]=(char)cp;
 else if(cp<0x800){b[n++]=(char)(0xc0|(cp>>6));b[n++]=(char)(0x80|(cp&63));}
 else if(cp<0x10000){b[n++]=(char)(0xe0|(cp>>12));b[n++]=(char)(0x80|((cp>>6)&63));b[n++]=(char)(0x80|(cp&63));}
 else {b[n++]=(char)(0xf0|(cp>>18));b[n++]=(char)(0x80|((cp>>12)&63));b[n++]=(char)(0x80|((cp>>6)&63));b[n++]=(char)(0x80|(cp&63));}
 return n;
}
static bool pull_codepoint(struct pull_state *s,char **text,size_t *length,uint32_t cp)
{
 if(!pull_xml_char(cp)){s->error="Invalid XML character reference";return false;}
 char bytes[4];size_t n=pull_encode(bytes,cp);return pull_append(s,text,length,bytes,n);
}
static struct pull_entity *pull_entity_find(struct pull_state *s,const char *name,size_t length)
{
 for(struct pull_entity *e=s->entities;e;e=e->next)if(strlen(e->name)==length && !memcmp(e->name,name,length))return e;
 return NULL;
}
static bool pull_entity_store(struct pull_state *s,char *name,char *value)
{
 if(!name || !value){free(name);free(value);return false;}
 struct pull_entity *e=pull_entity_find(s,name,strlen(name));
 if(e){free(name);free(e->value);e->value=value;return true;}
 e=calloc(1,sizeof *e);if(!e){free(name);free(value);s->oom=true;return false;}
 e->name=name;e->value=value;e->next=s->entities;s->entities=e;return true;
}
static char *pull_decode_mode(struct pull_state *s,const char *bytes,size_t n,bool attribute,bool entities,bool declaration)
{
 /* Most input shrinks when decoded. Grow only for declared replacements. */
 char *text=malloc(n+1);size_t length=0,capacity=n+1;if(!text){s->oom=true;return NULL;}
 for(size_t i=0;i<n && !s->error && !s->oom;){
  const char *replacement=NULL;char *owned=NULL;size_t count=0;char encoded[4];uint32_t cp=0;
  if(bytes[i]=='&' && entities){size_t end=i+1;while(end<n && bytes[end]!=';')++end;if(end==n){s->error="Unterminated XML entity";break;}
   const char *name=bytes+i+1;size_t size=end-i-1;
   if(declaration && (!size || name[0]!='#')){replacement=bytes+i;count=end-i+1;}
   else if(size==2 && !memcmp(name,"lt",2))cp='<';else if(size==2 && !memcmp(name,"gt",2))cp='>';
   else if(size==3 && !memcmp(name,"amp",3))cp='&';else if(size==4 && !memcmp(name,"quot",4))cp='"';else if(size==4 && !memcmp(name,"apos",4))cp='\'';
   else if(size && name[0]=='#'){size_t at=1;unsigned base=10;if(at<size && name[at]=='x'){base=16;++at;}bool digit=false;
    for(;at<size;++at){unsigned char c=(unsigned char)name[at];unsigned v=c>='0' && c<='9' ? c-'0' : base==16 && c>='a' && c<='f' ? c-'a'+10 : base==16 && c>='A' && c<='F' ? c-'A'+10 : 16;
     if(v>=base || cp>(0x10ffff-v)/base){s->error="Invalid XML character reference";break;}cp=cp*base+v;digit=true;}if(!digit)s->error="Empty XML character reference";}
   else {struct pull_entity *e=pull_entity_find(s,name,size);
    if(e){
     if(e->active){s->error="Recursive XML entity";break;}
     if(s->docdecl){e->active=true;owned=pull_decode_mode(s,e->value,strlen(e->value),attribute,true,false);e->active=false;replacement=owned;}
     else replacement=e->value;
     if(replacement)count=strlen(replacement);
    }else if(s->external){replacement="";}else s->error="Undefined XML entity";
   }
   i=end+1;
  }else {size_t at=i;if(!pull_utf8(bytes,n,&at,&cp) || !pull_xml_char(cp)){s->error="Invalid XML character";break;}
   if(attribute && cp=='<'){s->error="Less-than in XML attribute";break;}
   if(declaration && cp=='%'){s->error="Parameter entity references are unsupported";break;}
   if(cp=='\r'){if(at<n && bytes[at]=='\n')++at;cp='\n';}
   if(attribute && cp<128 && pull_space((unsigned char)cp))cp=' ';
   i=at;
  }
  if(!replacement && !s->error && !s->oom){if(!pull_xml_char(cp))s->error="Invalid XML character reference";else {count=pull_encode(encoded,cp);replacement=encoded;}}
  if(!s->error && !s->oom && replacement){
   if(count>SIZE_MAX-length-1)s->oom=true;
   else if(length+count+1>capacity){size_t need=length+count+1,cap=capacity<=SIZE_MAX/2 ? capacity*2 : need;if(cap<need)cap=need;char *q=realloc(text,cap);if(q){text=q;capacity=cap;}else s->oom=true;}
   if(!s->oom){memcpy(text+length,replacement,count);length+=count;}
  }
  free(owned);
 }
 text[length]=0;return text;
}
static char *pull_decode(struct pull_state *s,const char *bytes,size_t n,bool attribute,bool entities)
{return pull_decode_mode(s,bytes,n,attribute,entities,false);}

static bool pull_bind(struct pull_state *s,const char *prefix,const char *uri)
{
 if(!strcmp(prefix,"xmlns") || !strcmp(uri,"http://www.w3.org/2000/xmlns/") ||
    (!strcmp(prefix,"xml") != !strcmp(uri,"http://www.w3.org/XML/1998/namespace")) || (*prefix && !*uri)){s->error="Invalid XML namespace binding";return false;}
 struct pull_binding *b=calloc(1,sizeof *b);if(!b){s->oom=true;return false;}b->prefix=strdup(prefix);b->uri=strdup(uri);b->depth=s->depth;
 if(!b->prefix || !b->uri){free(b->prefix);free(b->uri);free(b);s->oom=true;return false;}b->next=s->bindings;s->bindings=b;return true;
}
static char *pull_expand(struct pull_state *s,const char *name,bool attribute)
{
 if(!s->namespaces)return strdup(name);
 const char *colon=strchr(name,':'),*local=colon ? colon+1 : name,*uri="";size_t prefix=colon ? (size_t)(colon-name) : 0;
 if(colon && (!prefix || !*local || strchr(local,':'))){s->error="Invalid namespace-qualified XML name";return NULL;}
 if(prefix==3 && !memcmp(name,"xml",3))uri="http://www.w3.org/XML/1998/namespace";
 else if(colon || !attribute){bool found=false;for(struct pull_binding *b=s->bindings;b;b=b->next)if(strlen(b->prefix)==prefix && !memcmp(b->prefix,name,prefix)){uri=b->uri;found=true;break;}
  if(colon && !found){s->error="Unbound XML namespace prefix";return NULL;}}
 if(!*uri)return strdup(local);
 size_t n=strlen(uri)+strlen(local)+prefix+3;char *expanded=malloc(n);if(!expanded){s->oom=true;return NULL;}
 if(prefix)snprintf(expanded,n,"%s\x1f%s\x1f%.*s",uri,local,(int)prefix,name);else snprintf(expanded,n,"%s\x1f%s",uri,local);return expanded;
}
static bool pull_skip_markup(struct pull_state *s,const char *end,bool comment)
{
 const char *p=strstr(s->bytes+s->pos,end);if(!p){s->error="Unterminated XML markup";return false;}
 size_t n=(size_t)(p-(s->bytes+s->pos));
 if(comment){const char *dash=strstr(s->bytes+s->pos,"--");if(dash && dash<p){s->error="Double hyphen in XML comment";return false;}}
 char *checked=pull_decode(s,s->bytes+s->pos,n,false,false);free(checked);s->pos=(size_t)(p-s->bytes)+strlen(end);return !s->error && !s->oom;
}
/* Non-validating DTD parsing: Android keeps internal general entities and
 * attribute defaults, skips external content, and rejects parameter references. */
static bool pull_take(struct pull_state *s,const char *token)
{
 if(!pull_at(s,token)){s->error="Unexpected XML declaration token";return false;}
 s->pos+=strlen(token);return true;
}
static char *pull_quoted(struct pull_state *s,int mode)
{
 if(s->pos>=s->length || (s->bytes[s->pos]!='\'' && s->bytes[s->pos]!='"')){s->error="Expected quoted XML declaration value";return NULL;}
 char quote=s->bytes[s->pos++];size_t start=s->pos;
 while(s->pos<s->length && s->bytes[s->pos]!=quote)++s->pos;
 if(s->pos==s->length){s->error="Unterminated XML declaration value";return NULL;}
 char *value=pull_decode_mode(s,s->bytes+start,s->pos-start,mode==1,mode!=0,mode==2);++s->pos;return value;
}
static bool pull_external_id(struct pull_state *s,bool system_required)
{
 bool public_id=pull_at(s,"PUBLIC");
 if(!public_id && !pull_at(s,"SYSTEM"))return false;
 s->pos+=6;pull_skip_space(s);
 char *id=pull_quoted(s,0);free(id);if(s->error || s->oom)return false;
 if(public_id){pull_skip_space(s);
  if(system_required || pull_at(s,"\'") || pull_at(s,"\"")){id=pull_quoted(s,0);free(id);}
 }
 return !s->error && !s->oom;
}
static void pull_default_store(struct pull_state *s,const char *element,char *name,char *value)
{
 if(!name || !value){free(name);free(value);return;}
 struct pull_default *d=s->defaults;
 for(;d;d=d->next)if(!strcmp(d->element,element) && !strcmp(d->name,name))break;
 if(d){free(name);free(d->value);d->value=value;return;}
 d=calloc(1,sizeof *d);if(d)d->element=strdup(element);
 if(!d || !d->element){free(d);free(name);free(value);s->oom=true;return;}
 d->name=name;d->value=value;d->next=s->defaults;s->defaults=d;
}
static void pull_dtd_entity(struct pull_state *s)
{
 pull_skip_space(s);bool parameter=pull_at(s,"%");if(parameter){++s->pos;pull_skip_space(s);}
 char *name=pull_name(s);pull_skip_space(s);char *value=NULL;
 if(pull_at(s,"\'") || pull_at(s,"\""))value=pull_quoted(s,2);
 else if(pull_external_id(s,true)){
  value=strdup("");if(!value)s->oom=true;pull_skip_space(s);
  if(pull_at(s,"NDATA")){s->pos+=5;pull_skip_space(s);char *notation=pull_name(s);free(notation);}
 }else if(!s->error)s->error="Expected XML entity value";
 pull_skip_space(s);if(!s->error)pull_take(s,">");
 if(!s->error && !s->oom && !parameter && s->docdecl)pull_entity_store(s,name,value);
 else {free(name);free(value);}
}
static void pull_dtd_attributes(struct pull_state *s)
{
 pull_skip_space(s);char *element=pull_name(s);
 while(element && !s->error && !s->oom){
  pull_skip_space(s);if(pull_at(s,">")){++s->pos;break;}
  char *name=pull_name(s);pull_skip_space(s);
  if(pull_at(s,"NOTATION")){s->pos+=8;pull_skip_space(s);}
  if(pull_at(s,"(")){
   ++s->pos;for(;;){pull_skip_space(s);char *item=pull_name(s);free(item);pull_skip_space(s);
    if(s->error || s->oom)break;if(pull_at(s,")")){++s->pos;break;}if(!pull_take(s,"|"))break;}
  }else {char *type=pull_name(s);free(type);}
  pull_skip_space(s);bool required=false;
  if(pull_at(s,"#")){++s->pos;
   if(pull_at(s,"REQUIRED")){s->pos+=8;required=true;}else if(pull_at(s,"IMPLIED")){s->pos+=7;required=true;}else pull_take(s,"FIXED");pull_skip_space(s);}
  char *value=NULL;
  if(!s->error && !s->oom && !required)value=pull_quoted(s,1);
  if(value && !s->error && !s->oom)pull_default_store(s,element,name,value);else {free(name);free(value);}
 }
 free(element);
}
static void pull_dtd_element(struct pull_state *s)
{
 pull_skip_space(s);char *name=pull_name(s);free(name);pull_skip_space(s);
 if(pull_at(s,"(")){
  int nesting=0;do {char c=s->bytes[s->pos++];if(c=='(')++nesting;else if(c==')')--nesting;}while(s->pos<s->length && nesting);
  if(nesting){s->error="Unterminated XML content specification";return;}
  if(pull_at(s,"?") || pull_at(s,"*") || pull_at(s,"+"))++s->pos;
 }else if(pull_at(s,"EMPTY"))s->pos+=5;else pull_take(s,"ANY");
 pull_skip_space(s);if(!s->error)pull_take(s,">");
}
static void pull_doctype(struct pull_state *s)
{
 if(s->root || s->doctype){s->error="Invalid XML document declaration";return;}s->doctype=true;
 s->pos+=9;pull_skip_space(s);char *name=pull_name(s);free(name);pull_skip_space(s);
 s->external=pull_external_id(s,true);pull_skip_space(s);
 if(pull_at(s,"[")){
  ++s->pos;
  while(!s->error && !s->oom){pull_skip_space(s);
   if(pull_at(s,"]")){++s->pos;break;}
   if(pull_at(s,"<!--")){s->pos+=4;pull_skip_markup(s,"-->",true);}
   else if(pull_at(s,"<?")){s->pos+=2;pull_skip_markup(s,"?>",false);}
   else if(pull_at(s,"<!ENTITY")){s->pos+=8;pull_dtd_entity(s);}
   else if(pull_at(s,"<!ATTLIST")){s->pos+=9;pull_dtd_attributes(s);}
   else if(pull_at(s,"<!ELEMENT")){s->pos+=9;pull_dtd_element(s);}
   else if(pull_at(s,"<!NOTATION")){s->pos+=10;pull_skip_space(s);char *notation=pull_name(s);free(notation);pull_skip_space(s);
    if(!pull_external_id(s,false) && !s->error)s->error="Expected XML notation identifier";pull_skip_space(s);if(!s->error)pull_take(s,">");}
   else {s->error=pull_at(s,"%") ? "Parameter entity references are unsupported" : "Invalid XML internal subset";}
  }
 }
 pull_skip_space(s);if(!s->error)pull_take(s,">");
}
static struct pull_entity *pull_text_entity(struct pull_state *s,size_t pos,size_t *end)
{
 if(!s->docdecl || s->bytes[pos]!='&')return NULL;
 size_t at=pos+1;while(at<s->length && s->bytes[at]!=';' && s->bytes[at]!='<' && !pull_space((unsigned char)s->bytes[at]))++at;
 if(at==s->length || s->bytes[at]!=';')return NULL;
 *end=at+1;
 /* Predefined entities are always literal characters, even if redeclared. */
 const char *name=s->bytes+pos+1;size_t n=at-pos-1;
 if((n==2 && (!memcmp(name,"lt",2) || !memcmp(name,"gt",2))) || (n==3 && !memcmp(name,"amp",3)) || (n==4 && (!memcmp(name,"quot",4) || !memcmp(name,"apos",4))))return NULL;
 return pull_entity_find(s,name,n);
}
static bool pull_source_push(struct pull_state *s,struct pull_entity *entity)
{
 if(entity->active){s->error="Recursive XML entity";return false;}
 struct pull_source *p=malloc(sizeof *p);if(!p){s->oom=true;return false;}
 *p=(struct pull_source){.next=s->sources,.bytes=s->bytes,.length=s->length,.pos=s->pos,.depth=s->depth,.entity=entity};
 s->sources=p;s->bytes=entity->value;s->length=strlen(s->bytes);s->pos=0;entity->active=true;return true;
}
static bool pull_source_pop(struct pull_state *s)
{
 struct pull_source *p=s->sources;
 if(!p)return false;
 if(s->depth!=p->depth){s->error="Unbalanced XML entity replacement";return false;}
 p->entity->active=false;s->bytes=p->bytes;s->length=p->length;s->pos=p->pos;s->sources=p->next;free(p);return true;
}
static void pull_default_attributes(struct pull_state *s,struct pull_event *e)
{
 const char *name=strchr(e->name,'\x1f');name=name ? name+1 : e->name;
 size_t length=strcspn(name,"\x1f");
 for(struct pull_default *d=s->defaults;d && !s->oom;d=d->next){
  if(strlen(d->element)!=length || memcmp(d->element,name,length))continue;
  bool exists=false;for(size_t i=0;i<e->count;++i)if(!strcmp(e->attrs[i*2],d->name)){exists=true;break;}
  if(exists)continue;
  char *key=strdup(d->name),*value=strdup(d->value);
  char **attrs=realloc(e->attrs,(e->count+1)*2*sizeof *attrs);
  if(!key || !value || !attrs){free(key);free(value);if(attrs)e->attrs=attrs;s->oom=true;break;}
  e->attrs=attrs;e->attrs[2*e->count]=key;e->attrs[2*e->count+1]=value;++e->count;
 }
}

static void pull_parse_document(struct pull_state *s)
{
 size_t valid_count=0;
 if(s->length>=3 && !memcmp(s->bytes,"\xef\xbb\xbf",3))s->pos=3;
 while(!s->error && !s->oom){
  if(s->pos==s->length){if(pull_source_pop(s))continue;break;}
  valid_count=s->count;
  size_t reference_end=0;struct pull_entity *entity=pull_text_entity(s,s->pos,&reference_end);
  if(entity){s->pos=reference_end;if(!pull_source_push(s,entity))break;continue;}
  if(pull_at(s,"<!--")){s->pos+=4;pull_skip_markup(s,"-->",true);continue;}
  if(pull_at(s,"<?")){s->pos+=2;pull_skip_markup(s,"?>",false);continue;}
  if(pull_at(s,"<!DOCTYPE")){pull_doctype(s);continue;}
  if(pull_at(s,"<![CDATA[") || s->bytes[s->pos]!='<'){
   bool cdata=pull_at(s,"<![CDATA[");if(cdata)s->pos+=9;size_t start=s->pos,end=start;
   if(cdata){const char *p=strstr(s->bytes+start,"]]>");if(!p){s->error="Unterminated CDATA";break;}end=(size_t)(p-s->bytes);s->pos=end+3;}
   else {size_t ignored=0;while(end<s->length && s->bytes[end]!='<' && !pull_text_entity(s,end,&ignored))++end;s->pos=end;}
   char *text=pull_decode(s,s->bytes+start,end-start,false,!cdata);if(!text)break;
   if(!s->depth){for(char *p=text;*p;++p)if(!pull_space((unsigned char)*p)){s->error="Text outside XML root";break;}if(cdata)s->error="CDATA outside XML root";free(text);continue;}
   if(!cdata && strstr(s->bytes+start,"]]>") && (size_t)(strstr(s->bytes+start,"]]>")-s->bytes)<end){free(text);s->error="CDATA close in text";break;}
   struct pull_event *e=s->count && s->events[s->count-1].type==4 ? &s->events[s->count-1] : pull_event_add(s,4);
   if(e){size_t n=e->text ? strlen(e->text) : 0;pull_append(s,&e->text,&n,text,strlen(text));}free(text);continue;
  }
  ++s->pos;
  if(pull_at(s,"/")){
   if(s->sources && s->depth<=s->sources->depth){s->error="XML entity closes an enclosing element";break;}
   ++s->pos;char *name=pull_name(s);pull_skip_space(s);
   if(!name || !s->depth || strcmp(s->stack[s->depth-1],name) || s->pos>=s->length || s->bytes[s->pos]!='>'){free(name);s->error="Mismatched XML end tag";break;}++s->pos;
   struct pull_event *e=pull_event_add(s,3);if(e)e->name=pull_expand(s,name,false);free(name);free(s->stack[--s->depth]);pull_bindings_pop(s,s->depth+1);continue;
  }
  char *name=pull_name(s);if(!name)break;
  if(!s->depth && s->root){free(name);s->error="Multiple XML root elements";break;}s->root=true;
  if((size_t)s->depth==s->stack_cap){size_t cap=s->stack_cap ? s->stack_cap*2 : 16;char **p=realloc(s->stack,cap*sizeof *p);if(!p){free(name);s->oom=true;break;}s->stack=p;s->stack_cap=cap;}
  s->stack[s->depth++]=name;struct pull_event *e=pull_event_add(s,2);if(!e)break;
  while(!s->error && !s->oom){size_t before=s->pos;pull_skip_space(s);
   if(pull_at(s,"/>") || pull_at(s,">"))break;
   if(before==s->pos){s->error="Missing XML attribute separator";break;}
   char *key=pull_name(s);pull_skip_space(s);if(!key)break;
   if(!pull_at(s,"=")){free(key);s->error="Missing XML attribute equals";break;}++s->pos;pull_skip_space(s);
   char quote=s->pos<s->length ? s->bytes[s->pos] : 0;if(quote!='\'' && quote!='"'){free(key);s->error="Unquoted XML attribute";break;}size_t start=++s->pos;while(s->pos<s->length && s->bytes[s->pos]!=quote)++s->pos;
   if(s->pos==s->length){free(key);s->error="Unterminated XML attribute";break;}
   char *value=pull_decode(s,s->bytes+start,s->pos-start,true,true);++s->pos;if(!value){free(key);break;}
   for(size_t i=0;i<e->count;++i)if(!strcmp(e->attrs[i*2],key))s->error="Duplicate XML attribute";
   char **p=realloc(e->attrs,(e->count+1)*2*sizeof *p);if(!p){free(key);free(value);s->oom=true;break;}e->attrs=p;e->attrs[e->count*2]=key;e->attrs[e->count*2+1]=value;++e->count;
  }
  if(s->error || s->oom)break;
  if(s->namespaces){for(size_t i=0;i<e->count;++i){const char *key=e->attrs[i*2];if(!strcmp(key,"xmlns"))pull_bind(s,"",e->attrs[i*2+1]);else if(!strncmp(key,"xmlns:",6))pull_bind(s,key+6,e->attrs[i*2+1]);}
   size_t keep=0;for(size_t i=0;i<e->count;++i){char *key=e->attrs[i*2],*value=e->attrs[i*2+1];if(!strcmp(key,"xmlns") || !strncmp(key,"xmlns:",6)){free(key);free(value);continue;}
    char *expanded=pull_expand(s,key,true);free(key);e->attrs[keep*2]=expanded;e->attrs[keep*2+1]=value;++keep;}e->count=keep;
   for(size_t i=0;i<e->count && !s->error && !s->oom;++i)for(size_t j=0;j<i;++j){const char *a=e->attrs[i*2],*b=e->attrs[j*2];const char *as=strchr(a,'\x1f'),*bs=strchr(b,'\x1f');size_t an=as && strchr(as+1,'\x1f') ? (size_t)(strchr(as+1,'\x1f')-a) : strlen(a),bn=bs && strchr(bs+1,'\x1f') ? (size_t)(strchr(bs+1,'\x1f')-b) : strlen(b);if(an==bn && !memcmp(a,b,an))s->error="Duplicate expanded XML attribute";}
  }
  if(s->error || s->oom)break;e->name=pull_expand(s,name,false);if(!e->name)break;pull_default_attributes(s,e);if(s->oom)break;
  bool empty=pull_at(s,"/>");e->empty=empty;s->pos+=empty ? 2 : 1;
  if(empty){struct pull_event *end=pull_event_add(s,3);if(end)end->name=pull_expand(s,name,false);free(s->stack[--s->depth]);pull_bindings_pop(s,s->depth+1);}
 }
 if(s->error)while(s->count>valid_count)pull_event_free(&s->events[--s->count]);
 if(!s->error && (!s->root || s->depth))s->error="Incomplete XML document";
}

static bool pull_feature(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const char *name=dvm_string_utf8(vm,ARG(0).l);
 if(!name){dvm__throw(vm,"java/lang/IllegalArgumentException","null XML feature");return false;}
 const char *field=!strcmp(name,"http://xmlpull.org/v1/doc/features.html#process-namespaces") ? "namespaceAware" :
    !strcmp(name,"http://xmlpull.org/v1/doc/features.html#process-docdecl") ? "processDocDecl" : NULL;
 if(!field && nargs>1 && ARG(1).i){dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","Unsupported XML feature: %s",name);return false;}
 if(nargs>1){if(field){struct dvm_object *o=dvm__obj(vm,self);struct pull_state *s=o && o->data_destroy==pull_destroy ? o->data : NULL;
   if(s && s->parsed){dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","Cannot change XML features during parsing");return false;}
   dvm_set_field(vm,self,field,"Z",ARG(1));if(s){if(!strcmp(field,"namespaceAware"))s->namespaces=ARG(1).i!=0;else s->docdecl=ARG(1).i!=0;}}
  RETV();}
 union dvm_value v={0};if(field)dvm_get_field(vm,self,field,"Z",&v);RETI(v.i);
}
static bool pull_factory_flag(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const char *field=strstr(builtin_name(),"Validating") ? "validating" : "namespaceAware";
 if(nargs){if(!strcmp(field,"validating") && ARG(0).i){dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","XML validation is unavailable");return false;}dvm_set_field(vm,self,field,"Z",ARG(0));RETV();}
 union dvm_value v={0};dvm_get_field(vm,self,field,"Z",&v);RETI(v.i);
}
static bool pull_factory_new(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;bool factory=!strcmp(builtin_name(),"newInstance");
 dvm_ref ref=dvm_new_object(vm,dvm_find_class(vm,factory ? "org/xmlpull/v1/XmlPullParserFactory" : "org/kxml2/io/KXmlParser"));
 if(!ref)return false;
 if(!factory){union dvm_value ns={0};dvm_get_field(vm,self,"namespaceAware","Z",&ns);dvm_set_field(vm,ref,"namespaceAware","Z",ns);rt_set_int(vm,ref,"attributeCount",-1);}
 RETL(ref);
}
static bool pull_set_input(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 if(!ARG(0).l){dvm__throw(vm,"java/lang/IllegalArgumentException","null XML input");return false;}
 struct pull_state *s=calloc(1,sizeof *s);if(!s){dvm__throw(vm,"java/lang/OutOfMemoryError","XML parser");return false;}
 union dvm_value ns={0};dvm_get_field(vm,self,"namespaceAware","Z",&ns);s->namespaces=ns.i!=0;dvm_get_field(vm,self,"processDocDecl","Z",&ns);s->docdecl=ns.i!=0;
 dvm_set_field(vm,self,"encoding","Ljava/lang/String;",nargs>1 ? ARG(1) : (union dvm_value){0});
 struct dvm_object *o=dvm__obj(vm,self);if(o->data_destroy)o->data_destroy(o->data);o->data=s;o->data_destroy=pull_destroy;
 dvm_set_field(vm,self,"input","Ljava/lang/Object;",ARG(0));rt_set_int(vm,self,"reader",nargs==1);rt_set_int(vm,self,"eventType",0);rt_set_int(vm,self,"depth",0);rt_set_int(vm,self,"attributeCount",-1);
 dvm_set_field(vm,self,"name","Ljava/lang/String;",(union dvm_value){0});dvm_set_field(vm,self,"text","Ljava/lang/String;",(union dvm_value){0});RETV();
}
static dvm_ref pull_name_part(struct dvm *vm,const char *name,int part)
{
 if(!name)return 0;const char *sep=strchr(name,'\x1f');
 if(!sep)return part==0 ? dvm_new_string(vm,name) : part==1 ? dvm_new_string(vm,"") : 0;
 if(part==1)return dvm_new_string_n(vm,name,(size_t)(sep-name));
 const char *local=sep+1,*prefix=strchr(local,'\x1f');
 if(part==2)return prefix ? dvm_new_string(vm,prefix+1) : 0;
 return prefix ? dvm_new_string_n(vm,local,(size_t)(prefix-local)) : dvm_new_string(vm,local);
}
static bool pull_read_document(struct dvm *vm,dvm_ref self,struct pull_state *s)
{
 union dvm_value input={0},reader={0},n={0};dvm_get_field(vm,self,"input","Ljava/lang/Object;",&input);dvm_get_field(vm,self,"reader","I",&reader);
 dvm_ref buffer=dvm_new_array(vm,reader.i ? 'C' : 'B',reader.i ? "C" : "B",4096);if(!buffer)return false;dvm_pin(vm,buffer);
 size_t capacity=0;bool good=true;
 for(;;){union dvm_value a[3]={{.l=buffer},{.i=0},{.i=4096}};
  if(!am_vcall(vm,input.l,"read",reader.i ? "([CII)I" : "([BII)I",a,3,&n)){good=false;break;}
  if(n.i<0)break;
  if(!n.i || n.i>4096){dvm__throw(vm,"java/io/IOException","Invalid XML read size");good=false;break;}
  size_t count=(size_t)n.i*(reader.i ? 2 : 1);
  if(count>SIZE_MAX-s->length-1){s->oom=true;break;}
  if(s->length+count+1>capacity){size_t cap=capacity ? capacity*2 : 8192;if(cap<s->length+count+1)cap=s->length+count+1;char *p=realloc(s->bytes,cap);if(!p){s->oom=true;break;}s->bytes=p;capacity=cap;}
  memcpy(s->bytes+s->length,dvm_array_data(vm,buffer),count);s->length+=count;s->bytes[s->length]=0;
 }
 dvm_unpin(vm,buffer);if(!good || s->oom)return good;
 if(!s->bytes){s->bytes=strdup("");if(!s->bytes){s->oom=true;return true;}}
 bool utf16=reader.i,little=false;size_t skip=0;
 union dvm_value encoding={0};dvm_get_field(vm,self,"encoding","Ljava/lang/String;",&encoding);const char *enc=dvm_string_utf8(vm,encoding.l);
 if(!reader.i){
  if(s->length>=2 && (unsigned char)s->bytes[0]==0xff && (unsigned char)s->bytes[1]==0xfe){utf16=little=true;skip=2;}
  else if(s->length>=2 && (unsigned char)s->bytes[0]==0xfe && (unsigned char)s->bytes[1]==0xff){utf16=true;skip=2;}
  else if(enc && (!strcasecmp(enc,"UTF-16LE") || !strcasecmp(enc,"UTF-16BE") || !strcasecmp(enc,"UTF-16"))){utf16=true;little=!strcasecmp(enc,"UTF-16LE");}
  else if(enc && strcasecmp(enc,"UTF-8") && strcasecmp(enc,"UTF8") && strcasecmp(enc,"US-ASCII")){s->error="Unsupported XML input encoding";return true;}
  else if(s->length>=4 && s->bytes[0]=='<' && !s->bytes[1] && s->bytes[2]=='?' && !s->bytes[3])utf16=little=true;
  else if(s->length>=4 && !s->bytes[0] && s->bytes[1]=='<' && !s->bytes[2] && s->bytes[3]=='?')utf16=true;
 }
 if(utf16){size_t count=(s->length-skip)/2;if((s->length-skip)&1){s->error="Odd UTF-16 XML input length";return true;}
  char *text=NULL;size_t bytes=0;
  for(size_t i=0;i<count && !s->error && !s->oom;++i){uint16_t u;const unsigned char *p=(const unsigned char *)s->bytes+skip+i*2;
   if(reader.i)memcpy(&u,p,2);else u=little ? (uint16_t)(p[0]|p[1]<<8) : (uint16_t)(p[0]<<8|p[1]);uint32_t cp=u;
   if(u>=0xd800 && u<=0xdbff){if(++i>=count){s->error="Unpaired XML surrogate";break;}p=(const unsigned char *)s->bytes+skip+i*2;uint16_t lo;if(reader.i)memcpy(&lo,p,2);else lo=little ? (uint16_t)(p[0]|p[1]<<8) : (uint16_t)(p[0]<<8|p[1]);if(lo<0xdc00 || lo>0xdfff){s->error="Unpaired XML surrogate";break;}cp=0x10000+((u-0xd800)<<10)+(lo-0xdc00);}
   pull_codepoint(s,&text,&bytes,cp);
  }
  free(s->bytes);s->bytes=text ? text : strdup("");s->length=bytes;if(!s->bytes)s->oom=true;
 }
 if(!s->error && !s->oom)pull_parse_document(s);s->parsed=true;return true;
}

static bool pull_next(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)args;(void)nargs;struct dvm_object *o=dvm__obj(vm,self);struct pull_state *s=o && o->data_destroy==pull_destroy ? o->data : NULL;
 if(!s){dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","XML input not set");return false;}
 if(!s->parsed && !pull_read_document(vm,self,s))return false;
 if(s->oom){dvm__throw(vm,"java/lang/OutOfMemoryError","XML events");return false;}
 if(s->index==s->count && s->error){dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","%s at byte %zu",s->error,s->pos);return false;}
 struct pull_event *e=s->index<s->count ? &s->events[s->index++] : NULL;
 int event=e ? e->type : 1;rt_set_int(vm,self,"eventType",event);rt_set_int(vm,self,"depth",e ? e->depth : 0);rt_set_int(vm,self,"attributeCount",event==2 ? (int)e->count : -1);rt_set_int(vm,self,"empty",e && e->empty);
 dvm_set_field(vm,self,"name","Ljava/lang/String;",(union dvm_value){.l=pull_name_part(vm,e ? e->name : NULL,0)});
 dvm_set_field(vm,self,"namespace","Ljava/lang/String;",(union dvm_value){.l=pull_name_part(vm,e ? e->name : NULL,1)});
 dvm_set_field(vm,self,"prefix","Ljava/lang/String;",(union dvm_value){.l=pull_name_part(vm,e ? e->name : NULL,2)});
 dvm_set_field(vm,self,"text","Ljava/lang/String;",(union dvm_value){.l=e && e->text ? dvm_new_string(vm,e->text) : 0});
 const char *fields[]={"attributeNameStrings","attributeValueStrings","attributeNamespaces","attributePrefixes"};
 for(int f=0;f<4;++f){dvm_ref array=event==2 ? dvm_new_array(vm,'L',"[Ljava/lang/String;",(uint32_t)e->count) : 0;dvm_set_field(vm,self,fields[f],"[Ljava/lang/String;",(union dvm_value){.l=array});
  for(size_t i=0;event==2 && i<e->count;++i){dvm_ref str=f==1 ? dvm_new_string(vm,e->attrs[i*2+1]) : pull_name_part(vm,e->attrs[i*2],f==0 ? 0 : f-1);((dvm_ref *)dvm_array_data(vm,array))[i]=str;}}
 RETI(event);
}

static bool xml_parser_next(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   struct dvm_object *object=dvm__obj(vm,self);
   if(object && object->data_destroy==pull_destroy)return pull_next(vm,self,args,nargs,out);
   (void)args; (void)nargs;
   union dvm_value resource = { 0 }, index = { 0 };
   (void)dvm_get_field(vm, self, "resourceId", "I", &resource);
   (void)dvm_get_field(vm, self, "eventIndex", "I", &index);
   int32_t iv = 0; const char *path = NULL;
   if (arm_exec_apk_resource_value((uint32_t)resource.i, &iv, &path) != 'L' ||
       !path) RETI(1);
   size_t n = 0; unsigned char *bytes = arm_exec_asset_read(path, &n);
   if (!bytes || n < 8 || ax_u16(bytes) != 3) { free(bytes); RETI(1); }
   uint32_t nstrings = 0; char **strings = ax_string_pool(bytes, n, &nstrings);
   if (!strings) { free(bytes); RETI(1); }
   int32_t seen = 0, event = 1, depth = 0, event_depth = 0, attribute_count = -1;
   const char *name = NULL, *text = NULL;
   const unsigned char *resource_map = NULL; uint32_t resource_map_count = 0;
   dvm_ref attribute_names = 0, attribute_types = 0, attribute_data = 0;
   dvm_ref attribute_name_strings = 0, attribute_value_strings = 0;
   dvm_ref attribute_namespaces = 0;
   size_t pos = ax_u16(bytes + 2);
   while (pos + 8 <= n) {
      uint16_t type = ax_u16(bytes + pos), header = ax_u16(bytes + pos + 2);
      uint32_t size = ax_u32(bytes + pos + 4);
      if (size < header || pos + size > n) break;
      if (type == 0x0180 && header <= size) {
         resource_map = bytes + pos + header;
         resource_map_count = (size - header) / 4;
      }
      /* RES_XML_CDATA_TYPE.  An element's character data is its own chunk, and
       * a parser that walks only start and end tags never reports TEXT (4) —
       * a reader after <tag>value</tag> then sees every value as empty.  The
       * string-pool index sits right after the 16-byte node header. */
      if (type == 0x0104) {
         if (seen++ == index.i) {
            event = 4;
            event_depth = depth;
            uint32_t text_index = pos + 20 <= n ? ax_u32(bytes + pos + 16)
                                                : UINT32_MAX;
            if (text_index < nstrings) text = strings[text_index];
            break;
         }
      }
      if (type == 0x0102 || type == 0x0103) {
         if (type == 0x0102) depth++;
         if (seen++ == index.i) {
            event = type == 0x0102 ? 2 : 3;
            event_depth = depth;
            if (type == 0x0102 && pos + 30 <= n)
               attribute_count = ax_u16(bytes + pos + 28);
            if (type == 0x0102 && attribute_count > 0) {
               attribute_names = dvm_new_array(vm, 'I', "I", (uint32_t)attribute_count);
               attribute_types = dvm_new_array(vm, 'I', "I", (uint32_t)attribute_count);
               attribute_data = dvm_new_array(vm, 'I', "I", (uint32_t)attribute_count);
               attribute_name_strings = dvm_new_array(vm, 'L',
                  "[Ljava/lang/String;", (uint32_t)attribute_count);
               attribute_value_strings = dvm_new_array(vm, 'L',
                  "[Ljava/lang/String;", (uint32_t)attribute_count);
               attribute_namespaces = dvm_new_array(vm, 'L',
                  "[Ljava/lang/String;", (uint32_t)attribute_count);
               int32_t *names = dvm_array_data(vm, attribute_names);
               int32_t *types = dvm_array_data(vm, attribute_types);
               int32_t *data = dvm_array_data(vm, attribute_data);
               dvm_ref *name_strings = dvm_array_data(vm, attribute_name_strings);
               dvm_ref *value_strings = dvm_array_data(vm, attribute_value_strings);
               dvm_ref *namespaces = dvm_array_data(vm, attribute_namespaces);
               uint16_t start = ax_u16(bytes + pos + 24);
               uint16_t width = ax_u16(bytes + pos + 26);
               size_t attrs = pos + 16u + start;
               for (int32_t i = 0; width >= 20 && i < attribute_count; ++i) {
                  size_t q = attrs + (size_t)i * width;
                  if (q + 20 > pos + size) break;
                  uint32_t name_index = ax_u32(bytes + q + 4);
                  uint32_t ns_index = ax_u32(bytes + q);
                  if (namespaces && ns_index < nstrings)
                     namespaces[i] = dvm_new_string(vm, strings[ns_index]);
                  names[i] = name_index < resource_map_count
                     ? (int32_t)ax_u32(resource_map + name_index * 4) : 0;
                  types[i] = bytes[q + 15]; data[i] = (int32_t)ax_u32(bytes + q + 16);
                  if (name_strings && name_index < nstrings && strings[name_index])
                     name_strings[i] = dvm_new_string(vm, strings[name_index]);
                  /* The raw-value slot holds a string-pool index whenever the
                   * attribute was written as text; TYPE_STRING (0x03) values
                   * repeat it in the typed data. */
                  uint32_t raw_index = ax_u32(bytes + q + 8);
                  uint32_t value_index = types[i] == 0x03
                     ? (uint32_t)data[i] : raw_index;
                  if (value_strings && value_index < nstrings &&
                      strings[value_index])
                     value_strings[i] = dvm_new_string(vm, strings[value_index]);
                  else if (value_strings)
                     value_strings[i] = ax_value_string(vm, types[i], data[i]);
               }
            }
            uint32_t name_index = pos + 24 <= n ? ax_u32(bytes + pos + 20) : UINT32_MAX;
            if (name_index < nstrings) name = strings[name_index];
            break;
         }
         if (type == 0x0103 && depth > 0) depth--;
      }
      pos += size;
   }
   index.i++;
   (void)dvm_set_field(vm, self, "eventIndex", "I", index);
   union dvm_value value = { .i = event };
   (void)dvm_set_field(vm, self, "eventType", "I", value);
   value.i = event_depth;
   (void)dvm_set_field(vm, self, "depth", "I", value);
   value.i = attribute_count;
   (void)dvm_set_field(vm, self, "attributeCount", "I", value);
   union dvm_value array_value = { .l = attribute_names };
   (void)dvm_set_field(vm, self, "attributeNames", "[I", array_value);
   array_value.l = attribute_types;
   (void)dvm_set_field(vm, self, "attributeTypes", "[I", array_value);
   array_value.l = attribute_data;
   (void)dvm_set_field(vm, self, "attributeData", "[I", array_value);
   array_value.l = attribute_name_strings;
   (void)dvm_set_field(vm, self, "attributeNameStrings",
                       "[Ljava/lang/String;", array_value);
   array_value.l = attribute_value_strings;
   (void)dvm_set_field(vm, self, "attributeValueStrings",
                       "[Ljava/lang/String;", array_value);
   array_value.l = attribute_namespaces;
   (void)dvm_set_field(vm, self, "attributeNamespaces",
                       "[Ljava/lang/String;", array_value);
   union dvm_value name_ref = { .l = name ? dvm_new_string(vm, name) : 0 };
   (void)dvm_set_field(vm, self, "name", "Ljava/lang/String;", name_ref);
   /* getName() is null on a TEXT event and getText() is null on a tag; both
    * are read straight after next() returns, so neither may keep the value the
    * previous event left behind. */
   union dvm_value text_ref = { .l = text ? dvm_new_string(vm, text) : 0 };
   (void)dvm_set_field(vm, self, "text", "Ljava/lang/String;", text_ref);
   for (uint32_t i = 0; i < nstrings; ++i) free(strings[i]);
   free(strings); free(bytes);
   RETI(event);
}

static bool xml_parser_attribute_resource(struct dvm *vm, dvm_ref self,
                                          const union dvm_value *args,
                                          int nargs, union dvm_value *out)
{
   union dvm_value names = { 0 };
   (void)dvm_get_field(vm, self, "attributeNames", "[I", &names);
   struct dvm_object *array = dvm__obj(vm, names.l);
   int32_t index = nargs ? ARG(0).i : -1;
   if (!array || !array->data || index < 0 || (uint32_t)index >= array->length)
      RETI(0);
   RETI(((int32_t *)array->data)[index]);
}

static bool xml_parser_attribute_boolean(struct dvm *vm, dvm_ref self,
                                         const union dvm_value *args,
                                         int nargs, union dvm_value *out)
{
   union dvm_value data_ref = { 0 };
   (void)dvm_get_field(vm, self, "attributeData", "[I", &data_ref);
   struct dvm_object *data = dvm__obj(vm, data_ref.l);
   int32_t index = nargs ? ARG(0).i : -1;
   if (!data || !data->data || index < 0 || (uint32_t)index >= data->length)
      RETI(nargs > 1 ? ARG(1).i : 0);
   RETI(((int32_t *)data->data)[index] != 0);
}

static bool xml_parser_attribute_count(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value count = { .i = -1 };
   (void)dvm_get_field(vm, self, "attributeCount", "I", &count);
   RETI(count.i);
}

static bool xml_parser_depth(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value depth = { 0 };
   (void)dvm_get_field(vm, self, "depth", "I", &depth);
   RETI(depth.i);
}

static bool xml_parser_event(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value event = { 0 };
   (void)dvm_get_field(vm, self, "eventType", "I", &event);
   RETI(event.i);
}

static bool xml_parser_name(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value name = { 0 };
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &name);
   RETL(name.l);
}

/* getAttributeName()/getAttributeValue() read the string-pool text captured
 * alongside the resource ids.  Without them a caller that walks attributes by
 * name — ProfileInstaller's manifest scan does — got null and died in
 * String.equalsIgnoreCase on the very next instruction. */
static dvm_ref xml_parser_string_at(struct dvm *vm, dvm_ref self,
                                    const char *field,
                                    const union dvm_value *args, int nargs)
{
   union dvm_value array_ref = { 0 };
   (void)dvm_get_field(vm, self, field, "[Ljava/lang/String;", &array_ref);
   struct dvm_object *array = dvm__obj(vm, array_ref.l);
   int32_t index = nargs ? ARG(0).i : -1;
   if (!array || !array->data || index < 0 || (uint32_t)index >= array->length)
      return 0;
   return ((dvm_ref *)array->data)[index];
}

static bool xml_parser_text(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value text = { 0 };
   (void)dvm_get_field(vm, self, "text", "Ljava/lang/String;", &text);
   RETL(text.l);
}

static bool xml_parser_attribute_name(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
   RETL(xml_parser_string_at(vm, self, "attributeNameStrings", args, nargs));
}

static bool xml_parser_attribute_value(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   RETL(xml_parser_string_at(vm, self, "attributeValueStrings", args, nargs));
}

/* XmlPullParser's namespace/name overload locates an attribute by name and
 * returns the value at the same index.  Android binary XML keeps namespace
 * separately; null requests the unqualified attribute, not any namespace. */
static bool xml_parser_attribute_value_named(struct dvm *vm, dvm_ref self,
                                             const union dvm_value *args,
                                             int nargs, union dvm_value *out)
{
   const char *wanted = nargs > 1 ? dvm_string_utf8(vm, ARG(1).l) : NULL;
   if (!wanted) {
      dvm__throw(vm,"java/lang/NullPointerException","attribute name");
      return false;
   }
   const char *space = nargs && ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   union dvm_value names_ref = { 0 }, values_ref = { 0 }, ns_ref = { 0 };
   (void)dvm_get_field(vm, self, "attributeNameStrings",
                       "[Ljava/lang/String;", &names_ref);
   (void)dvm_get_field(vm, self, "attributeValueStrings",
                       "[Ljava/lang/String;", &values_ref);
   (void)dvm_get_field(vm, self, "attributeNamespaces",
                       "[Ljava/lang/String;", &ns_ref);
   struct dvm_object *names = dvm__obj(vm, names_ref.l);
   struct dvm_object *values = dvm__obj(vm, values_ref.l);
   struct dvm_object *namespaces = dvm__obj(vm, ns_ref.l);
   if (!wanted || !names || !values || !names->data || !values->data)
      RETL(0);
   uint32_t count = names->length < values->length ? names->length : values->length;
   dvm_ref *name_items = names->data, *value_items = values->data;
   for (uint32_t i = 0; i < count; ++i) {
      const char *name = dvm_string_utf8(vm, name_items[i]);
      const char *uri = namespaces && namespaces->data && i < namespaces->length
         ? dvm_string_utf8(vm, ((dvm_ref *)namespaces->data)[i]) : NULL;
      bool same_namespace = space ? uri && !strcmp(uri, space) : !uri;
      if (name && !strcmp(name, wanted) && same_namespace)
         RETL(value_items[i]);
   }
   RETL(0);
}

/* XmlPullParser.nextText(): the text of the element the parser is on, leaving it on
 * the END_TAG.  GoogleAnalytics' tracker loader reads every <string> value of
 * res/xml/global_tracker.xml this way. */
static bool xml_parser_nextText(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   union dvm_value ev = { 0 }, text = { 0 }, ignored = { 0 };
   (void)xml_parser_event(vm, self, args, nargs, &ev);
   if (ev.i != 2) {   /* START_TAG */
      dvm__throw(vm, "org/xmlpull/v1/XmlPullParserException",
                 "parser must be on START_TAG to read next text");
      return false;
   }
   if(!xml_parser_next(vm, self, args, nargs, &ev))return false;
   if (ev.i == 4) {   /* TEXT */
      (void)xml_parser_text(vm, self, args, nargs, &text);
      if(!xml_parser_next(vm, self, args, nargs, &ev))return false;
      if (ev.i != 3) {
         dvm__throw(vm, "org/xmlpull/v1/XmlPullParserException",
                    "END_TAG expected");
         return false;
      }
      RETL(text.l ? text.l : dvm_new_string(vm, ""));
   }
   (void)ignored;
   if (ev.i == 3) RETL(dvm_new_string(vm, ""));
   dvm__throw(vm, "org/xmlpull/v1/XmlPullParserException",
              "parser must be on START_TAG or TEXT to read text");
   return false;
}

/* nextTag(): the next START_TAG or END_TAG, skipping comments, processing
 * instructions and whitespace-only text. */
static bool xml_parser_nextTag(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   union dvm_value ev = { 0 };
   for (;;) {
      if(!xml_parser_next(vm, self, args, nargs, &ev))return false;
      if (ev.i == 2 || ev.i == 3 || ev.i == 1) RETI(ev.i);
      if (ev.i == 4) {
         union dvm_value text = { 0 };
         (void)xml_parser_text(vm, self, args, nargs, &text);
         const char *t = text.l ? dvm_string_utf8(vm, text.l) : "";
         while (t && (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n')) ++t;
         if (t && *t) {
            dvm__throw(vm, "org/xmlpull/v1/XmlPullParserException",
                       "unexpected text in tag position");
            return false;
         }
      }
   }
}

static bool xml_parser_false(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETI(0);
}

static bool xml_parser_style(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value refs[3] = {{0}};
   const char *fields[] = {"attributeNameStrings", "attributeTypes", "attributeData"};
   for (int i = 0; i < 3; ++i)
      (void)dvm_get_field(vm, self, fields[i], i ? "[I" : "[Ljava/lang/String;", &refs[i]);
   struct dvm_object *names = dvm__obj(vm, refs[0].l);
   struct dvm_object *types = dvm__obj(vm, refs[1].l);
   struct dvm_object *data = dvm__obj(vm, refs[2].l);
   if (!names || !types || !data || !names->data || !types->data || !data->data) RETI(0);
   uint32_t count = names->length;
   if (types->length < count) count = types->length;
   if (data->length < count) count = data->length;
   for (uint32_t i = 0; i < count; ++i) {
      const char *name = dvm_string_utf8(vm, ((dvm_ref *)names->data)[i]);
      if (name && !strcmp(name, "style")) {
         int type = ((int32_t *)types->data)[i];
         RETI(type == 1 || type == 2 ? ((int32_t *)data->data)[i] : 0);
      }
   }
   RETI(0);
}

static bool xml_parser_attribute_namespace(struct dvm *vm,dvm_ref self,
   const union dvm_value *args,int nargs,union dvm_value *out)
{
   if (ARG(0).i<0 || ARG(0).i>=rt_get_int(vm,self,"attributeCount")) {
      dvm__throw(vm,"java/lang/IndexOutOfBoundsException","XML attribute");
      return false;
   }
   dvm_ref ns=xml_parser_string_at(vm,self,"attributeNamespaces",args,nargs);
   RETL(ns ? ns : dvm_new_string(vm,""));
}

static const struct rt_method rt_xml_parser[] = {
   M("getAttributeNamespace", "(I)Ljava/lang/String;", xml_parser_attribute_namespace),
   M("getStyleAttribute", "()I", xml_parser_style),
   M("nextText", "()Ljava/lang/String;", xml_parser_nextText),
   M("nextTag", "()I", xml_parser_nextTag),
   M("isEmptyElementTag", "()Z", xml_parser_false),
   M("next", "()I", xml_parser_next), M("getEventType", "()I", xml_parser_event),
   M("getName", "()Ljava/lang/String;", xml_parser_name),
   M("getText", "()Ljava/lang/String;", xml_parser_text),
   M("getDepth", "()I", xml_parser_depth),
   M("getAttributeCount", "()I", xml_parser_attribute_count),
   M("getAttributeNameResource", "(I)I", xml_parser_attribute_resource),
   M("getAttributeName", "(I)Ljava/lang/String;", xml_parser_attribute_name),
   M("getAttributeValue", "(I)Ljava/lang/String;", xml_parser_attribute_value),
   M("getAttributeValue",
     "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
     xml_parser_attribute_value_named),
   M("getAttributeBooleanValue", "(IZ)Z", xml_parser_attribute_boolean),
   M("close", "()V", nop_void), M_END,
};

static bool pull_get_namespace(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const char *name=builtin_name();
 if(nargs){const char *field=!strcmp(name,"getAttributeNamespace") ? "attributeNamespaces" : "attributePrefixes";
  dvm_ref ref=xml_parser_string_at(vm,self,field,args,nargs);if(!ref && !strcmp(field,"attributeNamespaces")){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","XML attribute");return false;}RETL(ref);}
 union dvm_value v={0};dvm_get_field(vm,self,!strcmp(name,"getNamespace") ? "namespace" : "prefix","Ljava/lang/String;",&v);RETL(v.l);
}
static bool pull_empty(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;union dvm_value e={0},v={0};dvm_get_field(vm,self,"eventType","I",&e);if(e.i!=2){dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","Not on START_TAG");return false;}dvm_get_field(vm,self,"empty","I",&v);RETI(v.i);}
static bool pull_named_attribute(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;const char *wanted=dvm_string_utf8(vm,ARG(1).l),*space=ARG(0).l ? dvm_string_utf8(vm,ARG(0).l) : "";
 union dvm_value count={0};dvm_get_field(vm,self,"attributeCount","I",&count);
 for(int i=0;wanted && i<count.i;++i){union dvm_value a={.i=i};dvm_ref n=xml_parser_string_at(vm,self,"attributeNameStrings",&a,1),ns=xml_parser_string_at(vm,self,"attributeNamespaces",&a,1);
  const char *name=dvm_string_utf8(vm,n),*uri=dvm_string_utf8(vm,ns);if(name && uri && !strcmp(name,wanted) && !strcmp(uri,space))RETL(xml_parser_string_at(vm,self,"attributeValueStrings",&a,1));}RETL(0);
}
static const struct rt_method rt_pull_factory[]={
 SM("newInstance","()Lorg/xmlpull/v1/XmlPullParserFactory;",pull_factory_new),
 M("setNamespaceAware","(Z)V",pull_factory_flag),M("isNamespaceAware","()Z",pull_factory_flag),
 M("setValidating","(Z)V",pull_factory_flag),M("isValidating","()Z",pull_factory_flag),
 M("newPullParser","()Lorg/xmlpull/v1/XmlPullParser;",pull_factory_new),M_END};
static const struct rt_method rt_pull_parser[]={
 M("<init>","()V",nop_void),M("setFeature","(Ljava/lang/String;Z)V",pull_feature),M("getFeature","(Ljava/lang/String;)Z",pull_feature),
 M("setInput","(Ljava/io/InputStream;Ljava/lang/String;)V",pull_set_input),M("setInput","(Ljava/io/Reader;)V",pull_set_input),
 M("next","()I",xml_parser_next),M("getEventType","()I",xml_parser_event),M("getDepth","()I",xml_parser_depth),
 M("getName","()Ljava/lang/String;",xml_parser_name),M("getText","()Ljava/lang/String;",xml_parser_text),
 M("getNamespace","()Ljava/lang/String;",pull_get_namespace),M("getPrefix","()Ljava/lang/String;",pull_get_namespace),
 M("getAttributeCount","()I",xml_parser_attribute_count),M("getAttributeName","(I)Ljava/lang/String;",xml_parser_attribute_name),
 M("getAttributeValue","(I)Ljava/lang/String;",xml_parser_attribute_value),
 M("getAttributeValue","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",pull_named_attribute),
 M("getAttributeNamespace","(I)Ljava/lang/String;",pull_get_namespace),M("getAttributePrefix","(I)Ljava/lang/String;",pull_get_namespace),
 M("nextText","()Ljava/lang/String;",xml_parser_nextText),M("nextTag","()I",xml_parser_nextTag),M("isEmptyElementTag","()Z",pull_empty),M_END};
static const char *const rt_iface_pull[]={"Lorg/xmlpull/v1/XmlPullParser;",NULL};

static bool color_state_default(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value color = { 0 };
   (void)dvm_get_field(vm, self, "color", "I", &color);
   RETI(color.i);
}

/* getColorForState(int[] stateSet, int defaultColor). */
static bool color_state_for_state(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   struct dvm_object *a = nargs > 0 && ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   RETI(color_state_lookup(vm, self, a ? a->data : NULL, a ? a->length : 0,
                           nargs > 1 ? ARG(1).i : 0));
}

static bool color_state_init(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   union dvm_value specs = { .l = nargs ? ARG(0).l : 0 };
   union dvm_value colors = { .l = nargs > 1 ? ARG(1).l : 0 };
   (void)dvm_set_field(vm, self, "stateSpecs", "[[I", specs);
   (void)dvm_set_field(vm, self, "colors", "[I", colors);
   struct dvm_object *states = dvm__obj(vm, specs.l);
   struct dvm_object *values = dvm__obj(vm, colors.l);
   int32_t color = 0;
   if (values && values->kind == DVM_OBJ_ARRAY && values->data && values->length) {
      int32_t *data = values->data; color = data[0];
      if (states && states->kind == DVM_OBJ_ARRAY && states->data) {
         dvm_ref *rows = states->data;
         uint32_t count = states->length < values->length ? states->length : values->length;
         for (uint32_t i = 0; i < count; ++i) {
            struct dvm_object *row = dvm__obj(vm, rows[i]);
            if (row && row->kind == DVM_OBJ_ARRAY && row->length == 0) {
               color = data[i]; break;
            }
         }
      }
   }
   union dvm_value value = { .i = color };
   (void)dvm_set_field(vm, self, "color", "I", value);
   RETV();
}

static const struct rt_field rt_color_state_fields[] = {
   { "color", "I" }, { "stateSpecs", "[[I" }, { "colors", "[I" },
   { "themeXml", "Ljava/lang/String;" }, { "themePending", "Z" }, F_END
};
/* ColorStateList.valueOf(int) — the single-colour list every widget uses when
 * it is handed a plain colour.  Unresolved it answered null, and the caller
 * then either drew with no colour at all (CardView's background) or threw on
 * the null it stored. */
static bool color_state_value_of(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)nargs;
   RETL(color_state_list_new(vm, ARG(0).i));
}

static bool color_state_theme(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                               int nargs,union dvm_value *out)
{
   union dvm_value pending={0},source={0};
   (void)dvm_get_field(vm,self,"themePending","Z",&pending);
   if(!strcmp(builtin_name(),"canApplyTheme")) RETI(pending.i);
   if(!pending.i || !nargs || !ARG(0).l) RETL(self);
   (void)dvm_get_field(vm,self,"themeXml","Ljava/lang/String;",&source);
   const char *path=dvm_string_utf8(vm,source.l);
   dvm_ref copy=color_value_list(vm,'L',0,path,ARG(0).l,0);
   if(!copy) return false;
   RETL(copy);
}

static const struct rt_method rt_color_state[] = {
   M("canApplyTheme","()Z",color_state_theme),
   M("obtainForTheme","(Landroid/content/res/Resources$Theme;)Landroid/content/res/ColorStateList;",color_state_theme),
   M("<init>", "([[I[I)V", color_state_init),
   SM("valueOf", "(I)Landroid/content/res/ColorStateList;", color_state_value_of),
   M("getDefaultColor", "()I", color_state_default),
   M("getColorForState", "([II)I", color_state_for_state),
   M_END,
};

static const struct rt_method rt_resources_theme[] = {
   M("applyStyle", "(IZ)V", theme_apply_style),
   M("obtainStyledAttributes", "([I)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M("obtainStyledAttributes", "(I[I)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M("obtainStyledAttributes",
     "(Landroid/util/AttributeSet;[III)Landroid/content/res/TypedArray;",
     context_obtainStyledAttributes),
   M("setTo", "(Landroid/content/res/Resources$Theme;)V", theme_set_to),
   M("getResources","()Landroid/content/res/Resources;",view_get_resources),
   M("resolveAttribute", "(ILandroid/util/TypedValue;Z)Z",
     theme_resolve_attribute),
   M("rebase", "()V", nop_void),
   M_END,
};

static bool activity_set_volume_stream(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "volumeControlStream", "I", ARG(0));
   RETV();
}

static bool activity_get_volume_stream(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { .i = INT32_MIN }; /* USE_DEFAULT_STREAM_TYPE */
   if (!dvm_get_field(vm, self, "volumeControlStream", "I", &v))
      v.i = INT32_MIN;
   RETI(v.i);
}

static bool cw_attachBaseContext(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .l = nargs > 0 ? ARG(0).l : 0 };
   (void)dvm_set_field(vm, self, "mBase", "Landroid/content/Context;", v);
   /* Application.attachBaseContext() is where ActivityThread publishes the
    * process Application, so every later getApplicationContext() can name it. */
   struct dvm_object *o = dvm__obj(vm, self);
   struct dvm_class *app = dvm__class_by_desc(vm, "Landroid/app/Application;");
   if (o && app && dvm__class_assignable(vm, o->cls, app) &&
       g_process_application != self) {
      if (g_process_application) dvm_unpin(vm, g_process_application);
      g_process_application = self;
      dvm_pin(vm, self);
   }
   RETV();
}

static bool application_get_process_name(struct dvm *vm, dvm_ref self,
                                          const union dvm_value *args,
                                          int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   const char *name = getenv("ANDROID_PROCESS_NAME");
   if(!name || !*name)name=getenv("ANDROID_PACKAGE_NAME");
   RETL(dvm_new_string(vm, name && *name ? name : "com.lunaria.app"));
}

/* android.app.AppGlobals.getInitialApplication() — the same process
 * Application ActivityThread published.  SDKs reach for it when they were
 * handed no Context of their own (AppsFlyer's AFb1uSDK does), and a null here
 * NPEs on the getPackageManager() that always follows. */
static bool app_globals_initial_application(struct dvm *vm, dvm_ref self,
                                            const union dvm_value *args,
                                            int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETL(g_process_application);
}

static const struct rt_method rt_app_globals[] = {
   SM("getInitialApplication", "()Landroid/app/Application;",
      app_globals_initial_application),
   SM("getInitialApplication", "()Ljava/lang/Object;",
      app_globals_initial_application),
   SM("getInitialPackage", "()Ljava/lang/String;",
      application_get_process_name),
   M_END,
};

/* Android's memory/configuration callback interfaces. Interface inheritance
 * must be represented separately from the Object superclass. */
static const char *const rt_iface_component_callbacks[] = {
   "Landroid/content/ComponentCallbacks;", NULL
};
static const struct rt_method rt_component_callbacks[] = {
   {"onConfigurationChanged", "(Landroid/content/res/Configuration;)V", NULL,
    DEX_ACC_PUBLIC | DEX_ACC_ABSTRACT},
   {"onLowMemory", "()V", NULL, DEX_ACC_PUBLIC | DEX_ACC_ABSTRACT},
   M_END,
};
static const struct rt_method rt_component_callbacks2[] = {
   {"onTrimMemory", "(I)V", NULL, DEX_ACC_PUBLIC | DEX_ACC_ABSTRACT},
   M_END,
};
static const struct rt_field rt_component_callbacks2_sfields[] = {
   {"TRIM_MEMORY_COMPLETE", "I"}, {"TRIM_MEMORY_MODERATE", "I"},
   {"TRIM_MEMORY_BACKGROUND", "I"}, {"TRIM_MEMORY_UI_HIDDEN", "I"},
   {"TRIM_MEMORY_RUNNING_CRITICAL", "I"}, {"TRIM_MEMORY_RUNNING_LOW", "I"},
   {"TRIM_MEMORY_RUNNING_MODERATE", "I"}, {NULL, NULL},
};

static const struct rt_method rt_application[] = {
   M("<init>", "()V", empty_void),
   SM("getProcessName", "()Ljava/lang/String;", application_get_process_name),
   /* Application.onCreate()'s own body is empty; the app's override is what
    * does the work, and this is the super() call reaching the base. */
   M("onCreate", "()V", empty_void),
   M("attachBaseContext", "(Landroid/content/Context;)V", cw_attachBaseContext),
   M("onLowMemory", "()V", nop_void),
   M("onTrimMemory", "(I)V", nop_void),
   M("onConfigurationChanged", "(Landroid/content/res/Configuration;)V",
     nop_void),
   M("registerActivityLifecycleCallbacks",
     "(Landroid/app/Application$ActivityLifecycleCallbacks;)V",
     app_register_lifecycle),
   M("unregisterActivityLifecycleCallbacks",
     "(Landroid/app/Application$ActivityLifecycleCallbacks;)V",
     app_unregister_lifecycle),
   /* ComponentCallbacks are onConfigurationChanged / onLowMemory.  Neither
    * ever happens here: the emulator has one configuration for the life of
    * the process and never reports memory pressure to the app. */
   M("registerComponentCallbacks", "(Landroid/content/ComponentCallbacks;)V",
     empty_void),
   M("unregisterComponentCallbacks", "(Landroid/content/ComponentCallbacks;)V",
     empty_void),
   M_END,
};

static bool cp_attach_info(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   union dvm_value context={0};
   (void)dvm_get_field(vm,self,"context","Landroid/content/Context;",&context);
   if(context.l)RETV();
   context.l=nargs ? ARG(0).l : 0;
   (void)dvm_set_field(vm,self,"context","Landroid/content/Context;",context);
   (void)dvm_set_field(vm,self,"info","Landroid/content/pm/ProviderInfo;",
                       (union dvm_value){.l=nargs>1 ? ARG(1).l : 0});
   bool registered=false;
   for(dvm_ref p=vm->content_providers;p;){
      if(p==self){registered=true;break;}
      union dvm_value next={0};
      (void)dvm_get_field(vm,p,"resolverNext","Landroid/content/ContentProvider;",&next);
      p=next.l;
   }
   if(!registered){
      (void)dvm_set_field(vm,self,"resolverNext","Landroid/content/ContentProvider;",
                          (union dvm_value){.l=vm->content_providers});
      dvm_pin(vm,self);vm->content_providers=self;
   }
   /* AOSP ContentProvider.attachInfo owns this virtual callback, including
    * an override that calls super.attachInfo from its own attachInfo. */
   struct dvm_method *method=dvm_find_method(vm,dvm_object_class(vm,self),"onCreate","()Z");
   union dvm_value ignored={0};
   if(method && !dvm_call(vm,method,self,NULL,0,&ignored))return false;
   RETV();
}

static bool cp_get_context(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value value = { 0 };
   (void)dvm_get_field(vm, self, "context", "Landroid/content/Context;", &value);
   RETL(value.l);
}

static const struct rt_field rt_content_provider_fields[] = {
   { "resolverNext", "Landroid/content/ContentProvider;" },
   { "context", "Landroid/content/Context;" },
   { "info", "Landroid/content/pm/ProviderInfo;" },
   F_END,
};

static const struct rt_method rt_content_provider[] = {
   /* ContentProvider() is `{}` on Android — the provider is set up by
    * attachInfo(), not by its constructor. */
   M("<init>", "()V", empty_void),
   M("attachInfo",
     "(Landroid/content/Context;Landroid/content/pm/ProviderInfo;)V",
     cp_attach_info),
   M("getContext", "()Landroid/content/Context;", cp_get_context),
   M("onCreate", "()Z", ret_true),
   M_END,
};

static bool act_getReferrer(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(0);
}

static bool act_get_application(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "application", "Landroid/app/Application;", &v);
   RETL(v.l);
}

static bool act_get_component_name(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value stored={0};
   dvm_get_field(vm,self,"componentName","Landroid/content/ComponentName;",&stored);
   if (stored.l) RETL(stored.l);
   struct dvm_class *c =
      dvm__class_by_desc(vm, "Landroid/content/ComponentName;");
   dvm_ref component = c ? dvm_new_object(vm, c) : 0;
   if (component) {
      union dvm_value ignored = { 0 };
      union dvm_value a[2]={{.l=self},{.l=dvm_class_object(vm,dvm_object_class(vm,self))}};
      if (!cn_init_context_class(vm,component,a,2,&ignored)) return false;
      dvm_set_field(vm,self,"componentName","Landroid/content/ComponentName;",(union dvm_value){.l=component});
   }
   RETL(component);
}

/* Activity.getLocalClassName(): the class name relative to the package. */
static bool act_get_local_class_name(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value component={0}, name={0}, package={0};
   if (!act_get_component_name(vm,self,NULL,0,&component)) return false;
   dvm_get_field(vm,component.l,"cls","Ljava/lang/String;",&name);
   struct dvm_method *get=dvm_find_method(vm,dvm_object_class(vm,self),"getPackageName","()Ljava/lang/String;");
   if (!get || !dvm_call(vm,get,self,NULL,0,&package)) return false;
   size_t cn=0,pn=0;
   const char *c=sdata(vm,name.l,&cn), *p=sdata(vm,package.l,&pn);
   if (!c || !p) {dvm__throw(vm,"java/lang/NullPointerException","activity component");return false;}
   if (cn>pn && !memcmp(c,p,pn) && c[pn]=='.') {c+=pn+1;cn-=pn+1;}
   RETL(dvm_new_string_n(vm,c,cn));
}

static bool act_get_preferences(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                int nargs,union dvm_value *out)
{
   struct dvm_class *c=dvm_object_class(vm,self);
   struct dvm_method *local=dvm_find_method(vm,c,"getLocalClassName","()Ljava/lang/String;");
   union dvm_value a[2]={{0},{.i=nargs ? ARG(0).i : 0}};
   if (!local || !dvm_call(vm,local,self,NULL,0,&a[0])) return false;
   struct dvm_method *get=dvm_find_method(vm,c,"getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
   return get && dvm_call(vm,get,self,a,2,out);
}

static bool li_from(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   (void)self;
   dvm_ref context = nargs ? ARG(0).l : 0;
   if (!context) {
      dvm__throw(vm, "java/lang/NullPointerException", "null inflater context");
      return false;
   }
   struct dvm_method *get = dvm_find_method(vm, dvm_object_class(vm, context),
      "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
   union dvm_value name = { .l = dvm_new_string(vm, "layout_inflater") };
   if (!get || !dvm_call(vm, get, context, &name, 1, out)) return false;
   if (!out->l) {
      dvm__throw(vm, "java/lang/AssertionError", "LayoutInflater not found.");
      return false;
   }
   if (!dvm__class_assignable(vm, dvm_object_class(vm, out->l),
                              dvm_find_class(vm, "android/view/LayoutInflater"))) {
      dvm__throw(vm, "java/lang/ClassCastException", "service is not LayoutInflater");
      return false;
   }
   return true;
}
static bool act_get_layout_inflater(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value context = { .l = self };
   return li_from(vm, 0, &context, 1, out);
}

static bool act_permission_result_run(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value activity = { 0 }, a[3] = { 0 }, ignored = { 0 };
   (void)dvm_get_field(vm, self, "activity", "Landroid/app/Activity;", &activity);
   (void)dvm_get_field(vm, self, "requestCode", "I", &a[0]);
   (void)dvm_get_field(vm, self, "permissions", "[Ljava/lang/String;", &a[1]);
   (void)dvm_get_field(vm, self, "results", "[I", &a[2]);
   struct dvm_class *c = dvm_object_class(vm, activity.l);
   struct dvm_method *m = c ? dvm_find_method(vm, c, "onRequestPermissionsResult",
                                              "(I[Ljava/lang/String;[I)V") : NULL;
   if (m && !dvm_call(vm, m, activity.l, a, 3, &ignored)) return false;
   RETV();
}
static bool act_request_permissions(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)nargs;
   struct dvm_object *permissions = dvm__obj(vm, ARG(0).l);
   if (!permissions) {
      dvm__throw(vm, "java/lang/NullPointerException", "permissions");
      return false;
   }
   dvm_ref results = dvm_new_array(vm, 'I', "I", permissions->length);
   int32_t *grants = results ? dvm_array_data(vm, results) : NULL;
   for (uint32_t i = 0; grants && i < permissions->length; ++i) {
      union dvm_value permission = { .l = ((dvm_ref *)permissions->data)[i] }, result = { 0 };
      (void)ctx_check_permission(vm, self, &permission, 1, &result);
      grants[i] = result.i;
   }
   struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/PermissionResult;");
   dvm_ref callback = c ? dvm_new_object(vm, c) : 0;
   union dvm_value v = { .l = self };
   (void)dvm_set_field(vm, callback, "activity", "Landroid/app/Activity;", v);
   (void)dvm_set_field(vm, callback, "permissions", "[Ljava/lang/String;", ARG(0));
   (void)dvm_set_field(vm, callback, "requestCode", "I", ARG(1));
   v.l = results;
   (void)dvm_set_field(vm, callback, "results", "[I", v);
   if (!callback || !queue_runnable_for(vm, callback, false, 0, 0, self, 0)) {
      dvm__throw(vm, "java/lang/IllegalStateException", "permission callback queue full");
      return false;
   }
   RETV();
}
static const struct rt_field rt_permission_result_fields[] = {
   { "activity", "Landroid/app/Activity;" }, { "requestCode", "I" },
   { "permissions", "[Ljava/lang/String;" }, { "results", "[I" }, F_END,
};
static const struct rt_method rt_permission_result[] = {
   M("run", "()V", act_permission_result_run), M_END,
};

static bool act_get_window(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value window = { 0 };
   (void)dvm_get_field(vm, self, "window", "Landroid/view/Window;", &window);
   if (!window.l) {
      struct dvm_class *wc = dvm__class_by_desc(vm, "Lcom/android/internal/policy/PhoneWindow;");
      window.l = wc ? dvm_new_object(vm, wc) : 0;
      if (window.l) {
         (void)dvm_set_field(vm,window.l,"context","Landroid/content/Context;",(union dvm_value){.l=self});
         union dvm_value callback = { .l = self };
         (void)dvm_set_field(vm, window.l, "callback",
                             "Landroid/view/Window$Callback;", callback);
         (void)dvm_set_field(vm, self, "window", "Landroid/view/Window;", window);
      }
   }
   RETL(window.l);
}

static bool act_getCurrentFocus(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out);

static bool act_request_window_feature(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   union dvm_value window = { 0 };
   if (!act_get_window(vm, self, NULL, 0, &window) || !window.l) RETI(0);
   struct dvm_class *wc = dvm_object_class(vm, window.l);
   struct dvm_method *request = wc ? dvm_find_method(
      vm, wc, "requestFeature", "(I)Z") : NULL;
   if (!request) RETI(0);
   union dvm_value feature = { .i = nargs > 0 ? ARG(0).i : 0 };
   return dvm_call(vm, request, window.l, &feature, 1, out);
}

static bool act_get_fragment_manager(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value manager = { 0 };
   (void)dvm_get_field(vm, self, "fragmentManager",
                       "Landroid/app/FragmentManager;", &manager);
   if (!manager.l) {
      struct dvm_class *mc =
         dvm__class_by_desc(vm, "Landroid/app/FragmentManager;");
      manager.l = mc ? dvm_new_object(vm, mc) : 0;
      (void)dvm_set_field(vm, self, "fragmentManager",
                          "Landroid/app/FragmentManager;", manager);
   }
   RETL(manager.l);
}

/* setContentView(View[, LayoutParams]): the view goes into the window's
 * content FrameLayout with the params given, or — for the one-argument form —
 * MATCH_PARENT both ways, which is what PhoneWindow.setContentView(View)
 * passes.  Leaving it WRAP_CONTENT shrank a full-screen root to its panel and
 * put the panel in the corner the SDK meant it to be centred away from. */
static void content_install_params(struct dvm *vm, dvm_ref view,
                                   const union dvm_value *args, int nargs)
{
   if (!view) return;
   union dvm_value lp = { .l = nargs > 1 ? ARG(1).l : 0 };
   if (!lp.l) {
      struct dvm_class *c =
         dvm__class_by_desc(vm, "Landroid/widget/FrameLayout$LayoutParams;");
      if (!c) c = dvm__class_by_desc(vm, "Landroid/view/ViewGroup$MarginLayoutParams;");
      lp.l = c ? dvm_new_object(vm, c) : 0;
      if (!lp.l) return;
      union dvm_value a[2] = { { .i = -1 }, { .i = -1 } }, ignored = { 0 };
      (void)layout_params_init(vm, lp.l, a, 2, &ignored);
   }
   (void)dvm_set_field(vm, view, "layoutParams",
                       "Landroid/view/ViewGroup$LayoutParams;", lp);
}

static bool act_set_content_resource(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   union dvm_value window = {0}, content = {0};
   if (!act_get_window(vm, self, NULL, 0, &window) || !window.l) return false;
   if (!am_vcall(vm, window.l, "setContentView", "(I)V", args, nargs, out)) return false;
   (void)dvm_get_field(vm, window.l, "contentView", "Landroid/view/View;", &content);
   (void)dvm_set_field(vm, self, "contentView", "Landroid/view/View;", content);
   RETV();
}

static bool act_set_content_view(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)out;
   union dvm_value v = { .l = nargs > 0 ? ARG(0).l : 0 };
   union dvm_value window = { 0 }, parent = { 0 }, id = { .i = 0x01020002 }, ignored = { 0 };
   if (!act_get_window(vm, self, NULL, 0, &window)) return false;
   if (!window_find_view(vm, window.l, &id, 1, &parent)) return false;
   if (!parent.l || !vg_remove_all(vm, parent.l, NULL, 0, &ignored)) return false;
   content_install_params(vm, v.l, args, nargs);
   if (v.l && !vg_add_view(vm, parent.l, &v, 1, &ignored)) return false;
   (void)dvm_set_field(vm, self, "contentView", "Landroid/view/View;", v);
   RETV();
}

/* Activity.addContentView — attach an overlay (ads banners, etc.) to the
 * content parent without replacing setContentView()'s root. */
static bool act_add_content_view(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)out;
   dvm_ref view = nargs > 0 ? ARG(0).l : 0;
   if (!view) RETV();
   union dvm_value window = { 0 }, content = { 0 }, id = { .i = 0x01020002 }, ignored = { 0 };
   if (!act_get_window(vm, self, NULL, 0, &window)) return false;
   if (!window_find_view(vm, window.l, &id, 1, &content)) return false;
   if (!content.l) return false;
   content_install_params(vm, view, args, nargs);
   union dvm_value child = { .l = view };
   if (!vg_add_view(vm, content.l, &child, 1, &ignored)) return false;
   RETV();
}

static bool act_find_view_by_id(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   /* Activity delegates to its Window. This also installs the decor's
    * content parent when queried before the first setContentView(). */
   union dvm_value window = { 0 };
   if (!act_get_window(vm, self, NULL, 0, &window)) return false;
   if (!window.l) RETL(0);
   return am_vcall(vm, window.l, "findViewById", "(I)Landroid/view/View;",
                   args, nargs, out);
}

static const struct rt_field rt_activity_fields[] = {
   { "componentName", "Landroid/content/ComponentName;" },
   { "title", "Ljava/lang/CharSequence;" },
   { "contentView", "Landroid/view/View;" },
   { "application", "Landroid/app/Application;" },
   { "window", "Landroid/view/Window;" },
   { "fragmentManager", "Landroid/app/FragmentManager;" },
   { "actionBar", "Landroid/app/ActionBar;" },
   { "destroyed", "Z" }, { "finishing", "Z" },
   /* getIntent() / setResult(): what the activity was started with and
      what it hands back. */
   { "intent", "Landroid/content/Intent;" },
   { "resultCode", "I" }, { "resultData", "Landroid/content/Intent;" },
   /* setVolumeControlStream()'s value, so getVolumeControlStream() answers
      what was set rather than a constant. */
   { "volumeControlStream", "I" }, F_END,
};

static bool act_is_destroyed(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value state = { 0 };
   (void)dvm_get_field(vm, self, "destroyed", "Z", &state);
   RETI(state.i != 0);
}

static bool act_on_destroy(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value state = { .i = 1 };
   (void)dvm_set_field(vm, self, "destroyed", "Z", state);
   RETV();
}

static bool act_get_title(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   const char *package = getenv("ANDROID_PACKAGE_NAME");
   RETL(dvm_new_string(vm, package && *package ? package : "Lunaria"));
}

/* Context.bindService(): the app binds to a Service of its own — Cross Worlds
 * runs its whole patch download inside com.netmarble.enn.DownloadService and
 * reaches it exactly this way.  The service class lives in the app's dex, so
 * there is a real object to build: create it, run onCreate(), take the binder
 * from onBind(), and hand it to the connection.  Answering false left the app
 * at "0.0%" with nothing to explain why.
 *
 * Real Android does this asynchronously; running it inline is the same
 * ordering a caller can observe, because it cannot touch mService before
 * onServiceConnected() sets it. */
static dvm_ref service_component_name(struct dvm *vm, struct dvm_class *svc)
{
   struct dvm_class *cc =
      dvm__class_by_desc(vm, "Landroid/content/ComponentName;");
   dvm_ref cn = cc ? dvm_new_object(vm, cc) : 0;
   if (!cn) return 0;
   const char *pkg = getenv("ANDROID_PACKAGE_NAME");
   union dvm_value v = { .l = dvm_new_string(vm, pkg ? pkg : "") };
   (void)dvm_set_field(vm, cn, "pkg", "Ljava/lang/String;", v);
   char name[512];
   snprintf(name, sizeof name, "%s", svc && svc->name ? svc->name : "");
   for (char *q = name; *q; ++q) if (*q == '/') *q = '.';
   v.l = dvm_new_string(vm, name);
   (void)dvm_set_field(vm, cn, "cls", "Ljava/lang/String;", v);
   return cn;
}

/* The process's running services, as ActivityManager's ActiveServices keeps
 * them: one instance per service class, created on the first start or
 * auto-creating bind and destroyed once it is neither started nor bound.
 * onBind() runs once and its binder is handed to every later connection.
 * The previous model built a fresh Service object — with its own onCreate()
 * — for every bindService() and startService() call, and unbindService()
 * did nothing, so onUnbind()/onDestroy() never ran. */
struct svc_rec {
   struct dvm_class *cls;
   dvm_ref obj;
   dvm_ref binder;
   dvm_ref bind_intent;   /* what onBind() was given; onUnbind() gets it too */
   bool have_binder;
   bool started;
   int bindings;
   int last_start_id;
   struct svc_rec *next;
};

/* Every connection passed to bindService(), even one whose bind failed: on a
 * device the connection's dispatcher is registered before the bind is
 * attempted, so unbindService() of it is legal either way. */
struct svc_conn {
   dvm_ref conn;
   struct svc_rec *svc;   /* NULL when nothing is bound through it */
   struct svc_conn *next;
};

static struct svc_rec *g_services;
static struct svc_conn *g_service_conns;

static void svc_call_void(struct dvm *vm, dvm_ref obj, const char *name,
                          const char *sig, union dvm_value *a, int n,
                          union dvm_value *ret)
{
   struct dvm_class *c = dvm_object_class(vm, obj);
   struct dvm_method *m = c ? dvm_find_method(vm, c, name, sig) : NULL;
   union dvm_value ignored = { 0 };
   if (m) (void)dvm_call(vm, m, obj, a, n, ret ? ret : &ignored);
}

/* The app's own service an intent names: new Intent(ctx, Foo.class) or an
 * explicit ComponentName in this package.  Anything else belongs to another
 * package this device does not have. */
static bool svc_process_is_current(struct dvm *vm, dvm_ref resolve)
{
   union dvm_value info = { 0 }, process = { 0 }, name = { 0 };
   (void)dvm_get_field(vm, resolve, "serviceInfo", "Landroid/content/pm/ServiceInfo;", &info);
   (void)dvm_get_field(vm, info.l, "processName", "Ljava/lang/String;", &process);
   const char *wanted = process.l ? dvm_string_utf8(vm, process.l) : NULL;
   const char *current = getenv("ANDROID_PROCESS_NAME");
   if (!current || !*current) current = getenv("ANDROID_PACKAGE_NAME");
   if (!wanted || !*wanted || !current || !strcmp(wanted, current)) return true;
   /* A separate VM/address space is required. Never run its native library
    * against the caller's globals or acknowledge a fake start. */
   (void)dvm_get_field(vm, info.l, "name", "Ljava/lang/String;", &name);
   fprintf(stderr, "[service] separate process required: %s (%s)\n",
           name.l ? dvm_string_utf8(vm, name.l) : "?", wanted);
   return false;
}

static struct dvm_class *svc_resolve_mode(struct dvm *vm, dvm_ref intent,bool allow_remote,char **remote_process)
{
   if(remote_process)*remote_process=NULL;
   if (!intent) {
      dvm__throw(vm, "java/lang/NullPointerException", "service intent");
      return NULL;
   }
   union dvm_value cn = { 0 }, pkg = { 0 }, cls = { 0 };
   (void)dvm_get_field(vm, intent, "component", "Landroid/content/ComponentName;", &cn);
   if (!cn.l) {
      (void)dvm_get_field(vm, intent, "package", "Ljava/lang/String;", &pkg);
      const char *sdk = getenv("ANDROID_TARGET_SDK");
      if (!pkg.l && sdk && atoi(sdk) >= 21) {
         dvm__throw(vm, "java/lang/IllegalArgumentException", "Service Intent must be explicit");
         return NULL;
      }
   }
   /* A package-scoped action shares PackageManager's manifest resolver. */
   union dvm_value a[2] = { { .l = intent }, { .i = 0 } }, result = { 0 };
   if (!pm_resolveService(vm, 0, a, 2, &result)) return NULL;
   if (result.l && !svc_process_is_current(vm, result.l)) {
      if(!allow_remote || !remote_process)return NULL;
      union dvm_value info={0},process={0};
      (void)dvm_get_field(vm,result.l,"serviceInfo","Landroid/content/pm/ServiceInfo;",&info);
      (void)dvm_get_field(vm,info.l,"processName","Ljava/lang/String;",&process);
      const char *name=process.l ? dvm_string_utf8(vm,process.l) : NULL;
      if(!name || !*name)return NULL;
      *remote_process=strdup(name);
      if(!*remote_process){dvm__throw(vm,"java/lang/OutOfMemoryError","service process");return NULL;}
   }
   if (!cn.l) {
      if (!result.l) return NULL;
      union dvm_value info = { 0 };
      (void)dvm_get_field(vm, result.l, "serviceInfo", "Landroid/content/pm/ServiceInfo;", &info);
      (void)dvm_get_field(vm, info.l, "name", "Ljava/lang/String;", &cls);
   } else {
      (void)dvm_get_field(vm, cn.l, "pkg", "Ljava/lang/String;", &pkg);
      (void)dvm_get_field(vm, cn.l, "cls", "Ljava/lang/String;", &cls);
      const char *self_pkg = getenv("ANDROID_PACKAGE_NAME");
      const char *p = pkg.l ? dvm_string_utf8(vm, pkg.l) : NULL;
      if (!p || !self_pkg || strcmp(p, self_pkg)) return NULL;
   }
   const char *self_pkg = getenv("ANDROID_PACKAGE_NAME");
   const char *c = cls.l ? dvm_string_utf8(vm, cls.l) : NULL;
   if (!c || !self_pkg) return NULL;
   char name[512];
   snprintf(name, sizeof name, "%s%s", c[0] == '.' ? self_pkg : "", c);
   for (char *q = name; *q; ++q) if (*q == '.') *q = '/';
   return dvm_find_class(vm, name);
}

static struct dvm_class *svc_resolve(struct dvm *vm,dvm_ref intent)
{ return svc_resolve_mode(vm,intent,false,NULL); }

static struct svc_rec *svc_find(struct dvm_class *cls)
{
   for (struct svc_rec *r = g_services; r; r = r->next)
      if (r->cls == cls) return r;
   return NULL;
}

static struct svc_rec *svc_create(struct dvm *vm, struct dvm_class *cls)
{
   struct svc_rec *r = svc_find(cls);
   if (r) return r;
   (void)dvm_init_class(vm, cls);
   dvm_ref obj = dvm_new_object(vm, cls);
   if (!obj) return NULL;
   r = calloc(1, sizeof *r);
   if (!r) return NULL;
   dvm_pin(vm, obj);
   r->cls = cls;
   r->obj = obj;
   r->next = g_services;
   g_services = r;
   svc_call_void(vm, obj, "<init>", "()V", NULL, 0, NULL);
   /* ActivityThread.handleCreateService attaches a service Context before
    * onCreate. A bare ContextWrapper cannot resolve services or resources:
    * background download tasks then fail before reading their first byte. */
   struct dvm_class *context = dvm__class_by_desc(vm, "Landroid/content/Context;");
   union dvm_value base = { .l = context ? dvm_new_object(vm, context) : 0 };
   if (base.l)
      svc_call_void(vm, obj, "attachBaseContext", "(Landroid/content/Context;)V",
                    &base, 1, NULL);
   svc_call_void(vm, obj, "onCreate", "()V", NULL, 0, NULL);
   return r;
}

/* ActiveServices.bringDownServiceIfNeeded(): neither started nor bound. */
static void svc_bring_down_if_needed(struct dvm *vm, struct svc_rec *r)
{
   if (!r || r->started || r->bindings > 0) return;
   for (struct svc_rec **pp = &g_services; *pp; pp = &(*pp)->next) {
      if (*pp != r) continue;
      *pp = r->next;
      break;
   }
   svc_call_void(vm, r->obj, "onDestroy", "()V", NULL, 0, NULL);
   fprintf(stderr, "[dvm] service %s destroyed\n", r->cls->name);
   dvm_unpin(vm, r->obj);
   if (r->binder) dvm_unpin(vm, r->binder);
   if (r->bind_intent) dvm_unpin(vm, r->bind_intent);
   free(r);
}

static struct svc_conn *svc_conn_find(dvm_ref conn)
{
   for (struct svc_conn *c = g_service_conns; c; c = c->next)
      if (c->conn == conn) return c;
   return NULL;
}

#define BIND_AUTO_CREATE 0x0001

/* Context.bindService(intent, conn, flags).  ActiveServices connects on the
 * main thread after bindService() has returned: the service is created, its
 * onBind() answers, and ServiceDispatcher posts onServiceConnected() to the
 * connection's thread.  A caller therefore holds its own locks and has set
 * its "connecting" flag when the callback arrives; delivering inline broke
 * exactly that (Firebase Analytics' measurement client saw its connection
 * come back before it had recorded that one was in progress, and never
 * connected).  The bind itself is posted to the main looper. */
static void svc_bind_deliver(struct dvm *vm, dvm_ref intent, dvm_ref conn, int flags)
{
   struct svc_conn *c = svc_conn_find(conn);
   if (!c) return;   /* unbound before the connection was made */
   struct dvm_class *cls = svc_resolve(vm, intent);
   if (!cls) return;
   struct svc_rec *r = svc_find(cls);
   if (!r && !(flags & BIND_AUTO_CREATE)) return;
   if (!r) r = svc_create(vm, cls);
   if (!r) return;
   if (c->svc == r) return;
   if (c->svc) {
      /* Rebinding a connection to another service drops the old binding. */
      c->svc->bindings--;
      svc_bring_down_if_needed(vm, c->svc);
   }
   c->svc = r;
   r->bindings++;
   if (!r->have_binder) {
      union dvm_value a = { .l = intent }, ret = { 0 };
      svc_call_void(vm, r->obj, "onBind",
                    "(Landroid/content/Intent;)Landroid/os/IBinder;", &a, 1, &ret);
      r->binder = ret.l;
      r->have_binder = true;
      if (r->binder) dvm_pin(vm, r->binder);
      r->bind_intent = intent;
      if (intent) dvm_pin(vm, intent);
   }
   union dvm_value a[2] = { { .l = service_component_name(vm, cls) },
                            { .l = r->binder } };
   dvm_pin(vm, a[0].l);
   if (r->binder)
      svc_call_void(vm, conn, "onServiceConnected",
                    "(Landroid/content/ComponentName;Landroid/os/IBinder;)V",
                    a, 2, NULL);
   else
      svc_call_void(vm, conn, "onNullBinding",
                    "(Landroid/content/ComponentName;)V", a, 1, NULL);
   dvm_unpin(vm, a[0].l);
   fprintf(stderr, "[dvm] bindService %s -> %s\n", cls->name,
           r->binder ? "bound" : "null binding");
}

static const struct rt_field rt_service_bind_fields[] = {
   { "intent", "Landroid/content/Intent;" }, { "conn", "Landroid/content/ServiceConnection;" },
   { "flags", "I" }, F_END
};

static bool service_bind_run(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   union dvm_value intent = { 0 }, conn = { 0 }, flags = { 0 };
   (void)dvm_get_field(vm, self, "intent", "Landroid/content/Intent;", &intent);
   (void)dvm_get_field(vm, self, "conn", "Landroid/content/ServiceConnection;", &conn);
   (void)dvm_get_field(vm, self, "flags", "I", &flags);
   svc_bind_deliver(vm, intent.l, conn.l, flags.i);
   if (intent.l) dvm_unpin(vm, intent.l);
   RETV();
}
static const struct rt_method rt_service_bind[] = { M("run", "()V", service_bind_run), M_END };

static bool ctx_bindService(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self;
   dvm_ref intent = ARG(0).l, conn = ARG(1).l;
   const int flags = nargs > 2 ? ARG(2).i : 0;
   if (!conn) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "connection is null");
      return false;
   }
   struct svc_conn *c = svc_conn_find(conn);
   if (!c) {
      c = calloc(1, sizeof *c);
      if (!c) RETI(0);
      dvm_pin(vm, conn);
      c->conn = conn;
      c->next = g_service_conns;
      g_service_conns = c;
   }
   if (!svc_resolve(vm, intent)) {
      if (vm->exception) return false;
      RETI(0);
   }
   struct dvm_class *bind = dvm__class_by_desc(vm, "Llunaria/ServiceBind;");
   dvm_ref job = bind ? dvm_new_object(vm, bind) : 0;
   if (!job) RETI(0);
   dvm_pin(vm, intent);
   union dvm_value v = { .l = intent };
   (void)dvm_set_field(vm, job, "intent", "Landroid/content/Intent;", v);
   v.l = conn;
   (void)dvm_set_field(vm, job, "conn", "Landroid/content/ServiceConnection;", v);
   v.i = flags;
   (void)dvm_set_field(vm, job, "flags", "I", v);
   if (!queue_runnable(vm, job, false)) { dvm_unpin(vm, intent); RETI(0); }
   RETI(1);
}

/* Context.unbindService(conn): the connection goes away without
 * onServiceDisconnected() (that is for a service that dies); the last
 * binding runs onUnbind(), and an unstarted service is then destroyed. */
static bool ctx_unbindService(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref conn = ARG(0).l;
   if (!conn) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "connection is null");
      return false;
   }
   struct svc_conn **pp = &g_service_conns;
   while (*pp && (*pp)->conn != conn) pp = &(*pp)->next;
   if (!*pp) {
      struct dvm_class *cc = dvm_object_class(vm, conn);
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "Service not registered: %s@%x",
                 cc && cc->name ? cc->name : "?", (unsigned)conn);
      return false;
   }
   struct svc_conn *c = *pp;
   *pp = c->next;
   struct svc_rec *r = c->svc;
   dvm_unpin(vm, c->conn);
   free(c);
   if (r && --r->bindings == 0) {
      union dvm_value a = { .l = r->bind_intent };
      svc_call_void(vm, r->obj, "onUnbind", "(Landroid/content/Intent;)Z",
                    &a, 1, NULL);
      svc_bring_down_if_needed(vm, r);
   }
   RETV();
}

/* Context.startService(intent): create the service if needed and deliver
 * onStartCommand() with the next start id. */
static bool ctx_startService(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref intent = ARG(0).l;
   char *remote_process=NULL;
   struct dvm_class *cls = svc_resolve_mode(vm,intent,vm->service_submit!=NULL,&remote_process);
   if (!cls) {
      free(remote_process);
      if (vm->exception) return false;
      RETL(0);
   }
   if(remote_process){
      void *bytes=NULL;size_t size=0;
      int rc=dvm_service_pack(vm,intent,1,&bytes,&size);
      if(!rc)rc=vm->service_submit(vm->service_submit_user,remote_process,bytes,size);
      free(bytes);free(remote_process);
      if(rc<0){
         if(!vm->exception)dvm__throw(vm,"java/lang/RuntimeException","Cannot start service process: %s",strerror(-rc));
         return false;
      }
      RETL(service_component_name(vm,cls));
   }
   struct svc_rec *r = svc_create(vm, cls);
   if (!r) RETL(0);
   r->started = true;
   union dvm_value a[3] = { { .l = intent }, { .i = 0 },
                            { .i = ++r->last_start_id } };
   svc_call_void(vm, r->obj, "onStartCommand", "(Landroid/content/Intent;II)I",
                 a, 3, NULL);
   RETL(service_component_name(vm, cls));
}

/* Context.stopService(intent): true when a started service was stopped. */
static bool ctx_stopService(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)nargs;
   struct dvm_class *cls = svc_resolve(vm, ARG(0).l);
   if (vm->exception) return false;
   struct svc_rec *r = cls ? svc_find(cls) : NULL;
   if (!r || !r->started) RETI(0);
   r->started = false;
   svc_bring_down_if_needed(vm, r);
   RETI(1);
}

/* Service.stopSelf() / stopSelf(startId) / stopSelfResult(startId): the
 * start-id forms stop only when startId is the latest start (a negative id
 * always does). */
static bool svc_stopSelf(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   struct svc_rec *r = g_services;
   while (r && r->obj != self) r = r->next;
   const bool result = !strcmp(builtin_name(), "stopSelfResult");
   bool stopped = false;
   if (r && (nargs == 0 || ARG(0).i < 0 || ARG(0).i == r->last_start_id)) {
      stopped = r->started;
      r->started = false;
      svc_bring_down_if_needed(vm, r);
   }
   if (result) RETI(stopped);
   RETV();
}

/* android.app.Service: the platform half a dex-defined service inherits.  Its
 * lifecycle callbacks are the app's; what has to exist here is the Context
 * behaviour (which ContextWrapper supplies) plus the calls a service makes
 * about itself. */
static const struct rt_method rt_service[] = {
   /* Service's own constructor and onCreate() are empty on a device too — a
      subclass that calls super.onCreate() is not asking for anything. */
   M("<init>", "()V", empty_void),
   M("onCreate", "()V", empty_void),
   M("onDestroy", "()V", nop_void),
   M("onStartCommand", "(Landroid/content/Intent;II)I", ret_true),
   M("onBind", "(Landroid/content/Intent;)Landroid/os/IBinder;", ret_zero),
   M("onUnbind", "(Landroid/content/Intent;)Z", ret_false),
   M("stopSelf", "()V", svc_stopSelf),
   M("stopSelf", "(I)V", svc_stopSelf),
   M("stopSelfResult", "(I)Z", svc_stopSelf),
   M("startForeground", "(ILandroid/app/Notification;)V", nop_void),
   M("startForeground", "(ILandroid/app/Notification;I)V", nop_void),
   M("stopForeground", "(Z)V", nop_void),
   M("stopForeground", "(I)V", nop_void),
   M_END,
};



/* Application.ActivityLifecycleCallbacks.
 *
 * The register call was accepted and the callbacks thrown away, so every
 * library that learns whether the app is in the foreground this way — the
 * Facebook SDK's ActivityLifecycleTracker, Play Services' BackgroundDetector,
 * AppsFlyer — was registered and never called.  Each of them then behaves as
 * if the app had never come to the foreground: sessions are not opened,
 * "app in background" stays true.
 *
 * On a device the dispatch happens inside Activity's own lifecycle methods —
 * Activity.onResume() calls getApplication().dispatchActivityResumed(this) —
 * so that is where it happens here too, and the ordering relative to a
 * subclass's own onResume() body is the same as the framework's.
 *
 * Registrations belong to the Application instance. Dispatch snapshots the
 * list before entering Java, so callback mutations affect the next event. */
struct lifecycle_registration { dvm_ref owner, callback; };

/* A lifecycle method, called virtually so the app's override runs (and, via
 * its super call, the framework's part). */
static void act_call(struct dvm *vm, dvm_ref activity, const char *name,
                     const char *sig, union dvm_value *a, int n)
{
   struct dvm_class *c = dvm_object_class(vm, activity);
   struct dvm_method *m = c ? dvm_find_method(vm, c, name, sig) : NULL;
   if (!m) return;
   union dvm_value ret;
   if (!dvm_call(vm, m, activity, a, n, &ret) && dvm_exception(vm)) {
      char how[256];
      dvm_describe_exception(vm, dvm_exception(vm), how, sizeof how);
      fprintf(stderr, "[activity] %s.%s threw %s\n",
              c->name ? c->name : "?", name, how);
      dvm_clear_exception(vm);
   }
}
static struct lifecycle_registration *g_lifecycle_callbacks;
static size_t g_nlifecycle_callbacks, g_lifecycle_capacity;

static bool app_register_lifecycle(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref cb = ARG(0).l;
   if (!cb) { RETV(); }
   if (g_nlifecycle_callbacks == g_lifecycle_capacity) {
      size_t cap = g_lifecycle_capacity ? g_lifecycle_capacity * 2 : 16;
      if (cap < g_lifecycle_capacity || cap > SIZE_MAX / sizeof *g_lifecycle_callbacks) {
         dvm__throw(vm, "java/lang/OutOfMemoryError", "lifecycle callbacks");
         RETV();
      }
      struct lifecycle_registration *items = realloc(g_lifecycle_callbacks, cap * sizeof *items);
      if (!items) {
         dvm__throw(vm, "java/lang/OutOfMemoryError", "lifecycle callbacks");
         RETV();
      }
      g_lifecycle_callbacks = items;
      g_lifecycle_capacity = cap;
   }
   dvm_pin(vm, cb);
   g_lifecycle_callbacks[g_nlifecycle_callbacks++] =
      (struct lifecycle_registration){self, cb};
   RETV();
}

static bool app_unregister_lifecycle(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self; (void)nargs;
   dvm_ref cb = ARG(0).l;
   for (size_t i = 0; i < g_nlifecycle_callbacks; ++i) {
      if (g_lifecycle_callbacks[i].owner != self ||
          g_lifecycle_callbacks[i].callback != cb) continue;
      dvm_unpin(vm, cb);
      for (size_t k = i; k + 1 < g_nlifecycle_callbacks; ++k)
         g_lifecycle_callbacks[k] = g_lifecycle_callbacks[k + 1];
      --g_nlifecycle_callbacks;
      break;
   }
   RETV();
}

/* `sig` is the callback's own signature: the Created form takes a Bundle, the
 * rest take only the Activity. */
static void dispatch_lifecycle_owner(struct dvm *vm, dvm_ref owner,
                                     dvm_ref activity, const char *name,
                                     const char *sig, dvm_ref bundle, bool has_bundle,
                                     bool reverse)
{
   size_t count = 0;
   for (size_t i = 0; i < g_nlifecycle_callbacks; ++i)
      if (g_lifecycle_callbacks[i].owner == owner) ++count;
   if (!count) return;
   dvm_ref *snapshot = malloc(count * sizeof *snapshot);
   if (!snapshot) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "lifecycle dispatch");
      return;
   }
   size_t at = 0;
   for (size_t i = 0; i < g_nlifecycle_callbacks; ++i)
      if (g_lifecycle_callbacks[i].owner == owner)
         snapshot[at++] = g_lifecycle_callbacks[i].callback;
   for (size_t i = 0; i < count; ++i) dvm_pin(vm, snapshot[i]);
   for (size_t i = 0; i < count; ++i) {
      dvm_ref cb = snapshot[reverse ? count - 1 - i : i];
      struct dvm_class *c = cb ? dvm_object_class(vm, cb) : NULL;
      struct dvm_method *m = c ? dvm_find_method(vm, c, name, sig) : NULL;
      if (!m) continue;
      union dvm_value a[2] = { { .l = activity }, { .l = bundle } };
      union dvm_value ret;
      (void)dvm_call(vm, m, cb, a, has_bundle ? 2 : 1, &ret);
      if (dvm_exception(vm)) break;
   }
   for (size_t i = 0; i < count; ++i) dvm_unpin(vm, snapshot[i]);
   free(snapshot);
}

static void dispatch_lifecycle(struct dvm *vm, dvm_ref activity,
                               const char *name, const char *sig,
                               bool has_bundle, dvm_ref bundle)
{
   union dvm_value app = {0};
   (void)dvm_get_field(vm, activity, "application", "Landroid/app/Application;", &app);
   if (!app.l) app.l = g_process_application;
   bool exiting = !strcmp(name, "onActivityPaused") ||
                  !strcmp(name, "onActivityStopped") ||
                  !strcmp(name, "onActivityDestroyed");
   if (exiting) {
      dispatch_lifecycle_owner(vm, activity, activity, name, sig, bundle, has_bundle, true);
      if (!dvm_exception(vm))
         dispatch_lifecycle_owner(vm, app.l, activity, name, sig, bundle, has_bundle, false);
   } else {
      dispatch_lifecycle_owner(vm, app.l, activity, name, sig, bundle, has_bundle, false);
      if (!dvm_exception(vm))
         dispatch_lifecycle_owner(vm, activity, activity, name, sig, bundle, has_bundle, false);
   }
}

/* The API 29 Pre/Post callbacks (onActivityPreStarted, onActivityPostResumed,
 * ...), which a framework Activity dispatches around its own perform*()
 * step rather than from inside onX().  androidx.lifecycle drives its
 * LifecycleOwner from the Post ones on API 29+, so without them an Activity
 * never leaves INITIALIZED and no LiveData observer is ever called.
 *
 * `step` is Created, Started, Resumed, Paused, Stopped or Destroyed.  The Pre
 * form reaches the Application's callbacks first and then the Activity's own;
 * the Post form the Activity's first, as Activity.dispatchActivity*() does. */
void dvm__activity_dispatch(struct dvm *vm, dvm_ref activity, const char *step,
                            bool post, dvm_ref bundle)
{
   char name[48];
   snprintf(name, sizeof name, "onActivity%s%s", post ? "Post" : "Pre", step);
   const bool has_bundle = !strcmp(step, "Created");
   const char *sig = has_bundle ? "(Landroid/app/Activity;Landroid/os/Bundle;)V"
                                : "(Landroid/app/Activity;)V";
   union dvm_value app = { 0 };
   (void)dvm_get_field(vm, activity, "application", "Landroid/app/Application;", &app);
   if (!app.l) app.l = g_process_application;
   if (post) {
      dispatch_lifecycle_owner(vm, activity, activity, name, sig, bundle, has_bundle, false);
      if (!dvm_exception(vm))
         dispatch_lifecycle_owner(vm, app.l, activity, name, sig, bundle, has_bundle, false);
   } else {
      dispatch_lifecycle_owner(vm, app.l, activity, name, sig, bundle, has_bundle, false);
      if (!dvm_exception(vm))
         dispatch_lifecycle_owner(vm, activity, activity, name, sig, bundle, has_bundle, false);
   }
}

/* One perform*() step: Pre callbacks, the lifecycle method, Post callbacks. */
static void act_perform(struct dvm *vm, dvm_ref activity, const char *step,
                        const char *method, const char *sig, union dvm_value *a,
                        int n)
{
   dvm_ref bundle = n > 0 ? a[0].l : 0;
   dvm__activity_dispatch(vm, activity, step, false, bundle);
   act_call(vm, activity, method, sig, a, n);
   /* Activity.performResume completes onPostResume before PostResumed.
    * FragmentActivity resumes its FragmentManager in that callback. */
   if (!strcmp(step, "Resumed"))
      act_call(vm, activity, "onPostResume", "()V", NULL, 0);
   dvm__activity_dispatch(vm, activity, step, true, bundle);
}

#define ACT_LIFECYCLE(fn, cbname, cbsig, has_bundle)                          \
   static bool fn(struct dvm *vm, dvm_ref self,                               \
                  const union dvm_value *args, int nargs,                     \
                  union dvm_value *out)                                       \
   {                                                                          \
      (void)args; (void)nargs;                                                \
      if (!strcmp(cbname,"onActivityStopped") ||                             \
          !strcmp(cbname,"onActivityStarted") ||                             \
          !strcmp(cbname,"onActivityResumed")) {                             \
         union dvm_value manager={0};                                       \
         (void)dvm_get_field(vm,self,"fragmentManager",                      \
                             "Landroid/app/FragmentManager;",&manager);     \
         if(manager.l) (void)dvm_set_field(vm,manager.l,"stateSaved","Z",    \
            (union dvm_value){.i=!strcmp(cbname,"onActivityStopped")});       \
      }                                                                     \
      dispatch_lifecycle(vm, self, cbname, cbsig, has_bundle,                 \
                         has_bundle && nargs ? ARG(0).l : 0);                \
      RETV();                                                                 \
   }

ACT_LIFECYCLE(act_on_create, "onActivityCreated",
              "(Landroid/app/Activity;Landroid/os/Bundle;)V", true)
ACT_LIFECYCLE(act_on_start, "onActivityStarted", "(Landroid/app/Activity;)V", false)
ACT_LIFECYCLE(act_on_resume, "onActivityResumed", "(Landroid/app/Activity;)V", false)
ACT_LIFECYCLE(act_on_pause, "onActivityPaused", "(Landroid/app/Activity;)V", false)
ACT_LIFECYCLE(act_on_stop, "onActivityStopped", "(Landroid/app/Activity;)V", false)
#undef ACT_LIFECYCLE


/* ---- the activity stack -------------------------------------------------
 *
 * ActivityManager and ActivityThread, for the activities of this package.
 * startActivity() only asks: the new activity is created on the main thread
 * afterwards (here, the next UI tick), the one it covers is paused, and the
 * new one runs onCreate -> onStart -> onResume and becomes a full-screen
 * window over it.  finish() tears it down the same way and resumes what is
 * underneath, delivering onActivityResult() first to a caller that asked for
 * one.  Without this, startActivity() was unresolved and did nothing, so any
 * flow that moves to a screen of its own — the HoYoverse account login is an
 * activity — stopped at the moment it began. */
#define ACT_MAX 16
struct act_rec {
   dvm_ref activity;
   dvm_ref caller;        /* startActivityForResult: who gets the result */
   int32_t request_code;
   bool for_result;
   bool stopped, visible;
};
static struct act_rec g_acts[ACT_MAX];
static int g_nacts;

struct act_launch {
   struct dvm_class *cls;
   dvm_ref component;
   dvm_ref intent;
   dvm_ref caller;
   int32_t request_code;
   bool for_result;
};
static struct act_launch g_act_launches[8];
static int g_nact_launches;
static dvm_ref g_act_finishes[8];
static int g_nact_finishes;
static bool g_task_background, g_task_move_back;

static void ui_window_shown(struct dvm *vm, dvm_ref window);
static void ui_window_hidden(struct dvm *vm, dvm_ref window);

/* The content view of a Dialog, or of a Window. */
static dvm_ref ui_window_content(struct dvm *vm, dvm_ref window)
{
   if (ui_is_a(vm, window, "android/widget/PopupWindow")) {
      dvm_ref decor = ui_obj(vm, window, "decorView", "Landroid/view/View;");
      if (decor) return decor;
   }
   dvm_ref v = ui_obj(vm, window, "contentView", "Landroid/view/View;");
   if (v) return v;
   dvm_ref w = ui_obj(vm, window, "window", "Landroid/view/Window;");
   if (w) return ui_obj(vm, w, "contentView", "Landroid/view/View;");
   return 0;
}
static dvm_ref ui_window_root(struct dvm *, dvm_ref);
static void act_reconcile_visibility(struct dvm *);

#define ACT_FLAG_NEW_TASK 0x10000000

static int act_index(dvm_ref activity)
{
   for (int i = 0; i < g_nacts; ++i)
      if (g_acts[i].activity == activity) return i;
   return -1;
}

/* The launcher is created by loader.c rather than startActivity().  It still
 * belongs to the same activity stack and its window must be presented when
 * resumed, just like subsequently launched activities. */
void dvm__activity_visible(struct dvm *vm, dvm_ref activity)
{
   if (act_index(activity) < 0 && g_nacts < ACT_MAX) {
      dvm_pin(vm, activity);
      if(g_nacts && !g_task_background)
         act_perform(vm,g_acts[g_nacts-1].activity,"Paused","onPause","()V",NULL,0);
      g_acts[g_nacts++] = (struct act_rec){ .activity = activity, .visible = true };
   }
   g_task_background = false;
   ui_window_shown(vm, activity);
   act_reconcile_visibility(vm);
}

static bool act_start_common(struct dvm *vm, dvm_ref self, dvm_ref intent,
                             bool from_activity, bool for_result,
                             int32_t request_code)
{
   if (!intent) {
      dvm__throw(vm, "java/lang/NullPointerException", "intent");
      return false;
   }
   union dvm_value fl = { 0 };
   (void)dvm_get_field(vm, intent, "flags", "I", &fl);
   if (!from_activity && !(fl.i & ACT_FLAG_NEW_TASK)) {
      dvm__throw(vm, "android/util/AndroidRuntimeException",
                 "Calling startActivity() from outside of an Activity  context "
                 "requires the FLAG_ACTIVITY_NEW_TASK flag. Is this really what "
                 "you want?");
      return false;
   }
   /* Activities and services have separate manifest resolvers. An alias is
    * the public component, while targetActivity is the class to instantiate. */
   union dvm_value query[2]={{.l=intent},{.i=0x10000}}, resolved={0};
   if (!pm_resolveActivity(vm,0,query,2,&resolved)) return false;
   struct dvm_class *cls=NULL;
   dvm_ref component=0;
   if (resolved.l) {
      union dvm_value info={0}, target={0}, name={0};
      dvm_get_field(vm,resolved.l,"activityInfo","Landroid/content/pm/ActivityInfo;",&info);
      dvm_get_field(vm,info.l,"targetActivity","Ljava/lang/String;",&target);
      dvm_get_field(vm,info.l,"name","Ljava/lang/String;",&name);
      union dvm_value package={0}, ignored={0};
      dvm_get_field(vm,info.l,"packageName","Ljava/lang/String;",&package);
      component=dvm_new_object(vm,dvm_find_class(vm,"android/content/ComponentName"));
      union dvm_value pair[2]={package,name};
      if (!component || !cn_init(vm,component,pair,2,&ignored)) return false;
      const char *dotted=dvm_string_utf8(vm,target.l ? target.l : name.l);
      char *descriptor=dotted ? strdup(dotted) : NULL;
      if (descriptor) {
         for (char *p=descriptor;*p;++p) if (*p=='.') *p='/';
         cls=dvm_find_class(vm,descriptor);
         free(descriptor);
      }
   }
   if (!cls || !cls->dex) {
      union dvm_value act = { 0 };
      (void)dvm_get_field(vm, intent, "action", "Ljava/lang/String;", &act);
      const char *a = act.l ? dvm_string_utf8(vm, act.l) : NULL;
      dvm__throw(vm, "android/content/ActivityNotFoundException",
                 "No Activity found to handle Intent { %s%s }",
                 a ? "act=" : "", a ? a : "");
      return false;
   }
   if (g_nact_launches >= (int)(sizeof g_act_launches / sizeof g_act_launches[0])) {
      fprintf(stderr, "[activity] launch queue full; %s dropped\n", cls->name);
      return true;
   }
   /* The activity that asked is the one on top; the first one to ask is the
    * launcher, which the loader started before this stack existed. */
   if (from_activity && g_nacts == 0) {
      dvm_pin(vm, self);
      g_acts[g_nacts++] = (struct act_rec){ .activity = self, .visible = true };
   }
   dvm_pin(vm, intent);
   dvm_pin(vm, component);
   if (self) dvm_pin(vm, self);
   g_act_launches[g_nact_launches++] = (struct act_launch){
      .cls = cls, .component = component, .intent = intent, .caller = self,
      .request_code = request_code, .for_result = for_result,
   };
   fprintf(stderr, "[activity] start %s%s\n", cls->name,
           for_result ? " (for result)" : "");
   return true;
}

static bool act_startActivity(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs;
   const bool from_activity = object_is_a(vm, self, "Landroid/app/Activity;");
   if (!act_start_common(vm, self, ARG(0).l, from_activity, false, -1))
      return false;
   RETV();
}

static bool act_startActivityForResult(struct dvm *vm, dvm_ref self,
                                       const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
   (void)nargs;
   /* A negative request code is startActivity(): no result is delivered. */
   const int32_t code = args[1].i;
   if (!act_start_common(vm, self, ARG(0).l, true, code >= 0, code))
      return false;
   RETV();
}

static bool act_finish(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value finishing = { 0 };
   (void)dvm_get_field(vm, self, "finishing", "Z", &finishing);
   if (finishing.i) RETV();
   finishing.i = 1;
   (void)dvm_set_field(vm, self, "finishing", "Z", finishing);
   if (g_nact_finishes < (int)(sizeof g_act_finishes / sizeof g_act_finishes[0])) {
      dvm_pin(vm, self);
      g_act_finishes[g_nact_finishes++] = self;
   }
   RETV();
}

static bool act_isFinishing(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "finishing", "Z", &v);
   RETI(v.i != 0);
}

static bool act_setResult(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   union dvm_value code = { .i = ARG(0).i }, data = { .l = nargs > 1 ? args[1].l : 0 };
   (void)dvm_set_field(vm, self, "resultCode", "I", code);
   (void)dvm_set_field(vm, self, "resultData", "Landroid/content/Intent;", data);
   RETV();
}

/* getIntent(): the Intent the activity was started with.  The launcher was
 * started by the home screen with MAIN/LAUNCHER and NEW_TASK |
 * RESET_TASK_IF_NEEDED, naming itself. */
static bool act_getIntent(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "intent", "Landroid/content/Intent;", &v);
   if (v.l) RETL(v.l);
   struct dvm_class *ic = dvm__class_by_desc(vm, "Landroid/content/Intent;");
   dvm_ref intent = ic ? dvm_new_object(vm, ic) : 0;
   if (!intent) RETL(0);
   dvm_pin(vm, intent);
   union dvm_value f = { .l = dvm_new_string(vm, "android.intent.action.MAIN") };
   (void)dvm_set_field(vm, intent, "action", "Ljava/lang/String;", f);
   f.i = 0x10200000;
   (void)dvm_set_field(vm, intent, "flags", "I", f);
   f.l = service_component_name(vm, dvm_object_class(vm, self));
   (void)dvm_set_field(vm, intent, "component", "Landroid/content/ComponentName;", f);
   f.l = intent;
   (void)dvm_set_field(vm, self, "intent", "Landroid/content/Intent;", f);
   dvm_unpin(vm, intent);
   RETL(intent);
}

static bool act_setIntent(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)nargs;
   union dvm_value v = { .l = ARG(0).l };
   (void)dvm_set_field(vm, self, "intent", "Landroid/content/Intent;", v);
   RETV();
}

/* A paused Activity remains visible behind floating/translucent windows.
 * Keep its window attached when stopped; visibility is independent of the
 * window registry and of the content retained for findViewById/returning. */
static bool act_window_opaque(struct dvm *vm,dvm_ref activity)
{
   union dvm_value theme={0};
   if(!context_get_theme(vm,activity,NULL,0,&theme)) return true;
   const uint32_t attrs[]={0x01010057u,0x01010058u}; /* windowIsFloating/Translucent */
   for(unsigned i=0;i<sizeof attrs/sizeof *attrs;++i) {
      int32_t value=0;const char *text=NULL;
      int kind=theme_style_value(vm,theme.l,attrs[i],&value,&text);
      for(unsigned depth=0;kind==2 && depth<64;++depth)
         kind=theme_style_value(vm,theme.l,(uint32_t)value,&value,&text);
      if(kind==1) kind=res_value_resolved((uint32_t)value,&value,&text);
      if(kind==0x12 && value) return false;
   }
   return true;
}
static bool act_window_present(struct dvm *vm,dvm_ref window)
{
   /* Dialogs and popups inherit their owning Activity's app visibility. */
   for(unsigned depth=0;window && depth<64;++depth) {
      int index=act_index(window);
      if(index>=0) return g_acts[index].visible;
      union dvm_value owner={0};
      (void)dvm_get_field(vm,window,"ownerActivity","Landroid/app/Activity;",&owner);
      if(!owner.l) (void)dvm_get_field(vm,window,"context","Landroid/content/Context;",&owner);
      if(!owner.l) (void)dvm_get_field(vm,window,"mBase","Landroid/content/Context;",&owner);
      if(owner.l==window) break;
      window=owner.l;
   }
   return true;
}
static void act_restart(struct dvm *vm,int index)
{
   if(!g_acts[index].stopped) return;
   g_acts[index].stopped=false;
   act_call(vm,g_acts[index].activity,"onRestart","()V",NULL,0);
   act_perform(vm,g_acts[index].activity,"Started","onStart","()V",NULL,0);
}
static void act_window_visibility(struct dvm *vm,int index,bool visible)
{
   struct act_rec *rec=&g_acts[index];
   if(rec->visible==visible) return;
   rec->visible=visible;
   dvm_ref content=ui_window_root(vm,rec->activity);
   union dvm_value attached={0},value={.i=visible ? 0 : 8},ignored={0};
   if(content) (void)dvm_get_field(vm,content,"attachedToWindow","Z",&attached);
   if(attached.i && !am_vcall(vm,content,"dispatchWindowVisibilityChanged","(I)V",&value,1,&ignored))
      ui_report_exception(vm,"dispatchWindowVisibilityChanged");
   if(visible) ui_layout_changed(); else ui_invalidate();
}
static void act_reconcile_visibility(struct dvm *vm)
{
   bool covered=g_task_background,wanted[ACT_MAX]={0};
   for(int i=g_nacts;i-->0;) {
      wanted[i]=!covered;
      if(!covered && act_window_opaque(vm,g_acts[i].activity)) covered=true;
   }
   for(int i=0;i<g_nacts;++i) {
      if(wanted[i]) act_restart(vm,i);
      act_window_visibility(vm,i,wanted[i]);
      if(!wanted[i] && !g_acts[i].stopped) {
         g_acts[i].stopped=true;
         act_perform(vm,g_acts[i].activity,"Stopped","onStop","()V",NULL,0);
      }
   }
}

static void act_do_launch(struct dvm *vm, const struct act_launch *l)
{
   dvm_ref obj = dvm_new_object(vm, l->cls);
   if (!obj) return;
   dvm_pin(vm, obj);
   (void)dvm_init_class(vm, l->cls);
   act_call(vm, obj, "<init>", "()V", NULL, 0);

   /* Activity.attach(): the base Context and the Application are the
    * process's, the Intent is the one it was started with. */
   dvm_ref caller = l->caller;
   union dvm_value base = { 0 }, app = { 0 };
   if (caller) {
      (void)dvm_get_field(vm, caller, "mBase", "Landroid/content/Context;", &base);
      (void)dvm_get_field(vm, caller, "application", "Landroid/app/Application;", &app);
   }
   if (!app.l && g_nacts)
      (void)dvm_get_field(vm, g_acts[0].activity, "application",
                          "Landroid/app/Application;", &app);
   if (!base.l && g_nacts)
      (void)dvm_get_field(vm, g_acts[0].activity, "mBase",
                          "Landroid/content/Context;", &base);
   if (!base.l) base.l = app.l;
   if (base.l) act_call(vm, obj, "attachBaseContext", "(Landroid/content/Context;)V",
                        &base, 1);
   if (app.l) (void)dvm_set_field(vm, obj, "application", "Landroid/app/Application;", app);
   /* ActivityThread applies the manifest theme before onCreate. */
   {
      const uint32_t theme = arm_exec_activity_theme(l->cls->name);
      if (theme) {
         union dvm_value tv = { .i = (int32_t)theme };
         union dvm_value ignored = {0};
         if (!am_vcall(vm, obj, "setTheme", "(I)V", &tv, 1, &ignored)) {
            dvm_unpin(vm, obj);
            return;
         }
      }
      fprintf(stderr, "[activity] %s theme 0x%08x%s\n",
              l->cls->name ? l->cls->name : "?", theme,
              theme && arm_exec_theme_is_floating(theme) ? " (floating)" : "");
   }
   dvm_set_field(vm,obj,"componentName","Landroid/content/ComponentName;",(union dvm_value){.l=l->component});
   union dvm_value iv = { .l = l->intent };
   (void)dvm_set_field(vm, obj, "intent", "Landroid/content/Intent;", iv);

   /* The activity it covers is paused first. */
   if (g_nacts && !g_task_background) act_perform(vm, g_acts[g_nacts - 1].activity, "Paused", "onPause", "()V", NULL, 0);
   g_task_background = false;
   if (g_nacts < ACT_MAX)
      g_acts[g_nacts++] = (struct act_rec){
         .activity = obj, .caller = l->caller,
         .request_code = l->request_code, .for_result = l->for_result,
      };
   if (l->caller && l->for_result) dvm_pin(vm, l->caller);
   union dvm_value bundle = { 0 };
   act_perform(vm, obj, "Created", "onCreate", "(Landroid/os/Bundle;)V", &bundle, 1);
   act_perform(vm, obj, "Started", "onStart", "()V", NULL, 0);
   act_call(vm, obj, "onPostCreate", "(Landroid/os/Bundle;)V", &bundle, 1);
   act_perform(vm, obj, "Resumed", "onResume", "()V", NULL, 0);
   ui_window_shown(vm, obj);
   act_reconcile_visibility(vm);
   fprintf(stderr, "[activity] %s resumed (%d on the stack)\n",
           l->cls->name ? l->cls->name : "?", g_nacts);
}

static void act_do_finish(struct dvm *vm, dvm_ref activity)
{
   int i = act_index(activity);
   if (i < 0) return;
   const bool top = i == g_nacts - 1;
   struct act_rec rec = g_acts[i];
   if (top && !g_task_background) act_perform(vm, activity, "Paused", "onPause", "()V", NULL, 0);
   if(!rec.stopped) act_perform(vm, activity, "Stopped", "onStop", "()V", NULL, 0);
   act_perform(vm, activity, "Destroyed", "onDestroy", "()V", NULL, 0);
   ui_window_hidden(vm, activity);
   memmove(&g_acts[i], &g_acts[i + 1], (size_t)(g_nacts - i - 1) * sizeof g_acts[0]);
   --g_nacts;
   if (!g_nacts) g_task_background=false;
   act_reconcile_visibility(vm);
   if (rec.for_result && rec.caller) {
      union dvm_value code = { 0 }, data = { 0 };
      (void)dvm_get_field(vm, activity, "resultCode", "I", &code);
      (void)dvm_get_field(vm, activity, "resultData", "Landroid/content/Intent;", &data);
      union dvm_value a[3] = { { .i = rec.request_code }, { .i = code.i }, { .l = data.l } };
      act_call(vm, rec.caller, "onActivityResult", "(IILandroid/content/Intent;)V", a, 3);
      dvm_unpin(vm, rec.caller);
   }
   if (top && g_nacts && !g_task_background)
      act_perform(vm, g_acts[g_nacts - 1].activity, "Resumed", "onResume", "()V", NULL, 0);
   fprintf(stderr, "[activity] %s finished (%d on the stack)\n",
           dvm_object_class(vm, activity) ? dvm_object_class(vm, activity)->name : "?",
           g_nacts);
   dvm_unpin(vm, activity);
}

/* On the main thread, between messages: what startActivity()/finish()
 * asked for. */
static void act_tick(struct dvm *vm)
{
   while (g_nact_finishes) {
      dvm_ref a = g_act_finishes[0];
      memmove(&g_act_finishes[0], &g_act_finishes[1],
              (size_t)(--g_nact_finishes) * sizeof g_act_finishes[0]);
      act_do_finish(vm, a);
      dvm_unpin(vm, a);
   }
   if (g_task_move_back) {
      g_task_move_back = false;
      if (g_nacts && !g_task_background) {
         dvm_ref top=g_acts[g_nacts-1].activity;
         act_perform(vm,top,"Paused","onPause","()V",NULL,0);
         g_task_background=true;
         act_reconcile_visibility(vm);
      }
   }
   while (g_nact_launches) {
      struct act_launch l = g_act_launches[0];
      memmove(&g_act_launches[0], &g_act_launches[1],
              (size_t)(--g_nact_launches) * sizeof g_act_launches[0]);
      act_do_launch(vm, &l);
      dvm_unpin(vm, l.intent);
      if (l.component) dvm_unpin(vm, l.component);
      if (l.caller) dvm_unpin(vm, l.caller);
   }
}

static bool act_runOnUiThread(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   dvm_ref runnable = nargs ? ARG(0).l : 0;
   if (!runnable) RETV();
   if (!dvm_on_main_thread()) {
      if (!queue_runnable_for(vm, runnable, false, 0, 0, self, 0)) {
         dvm__throw(vm, "java/lang/IllegalStateException", "main queue full");
         return false;
      }
      RETV();
   }
   struct dvm_class *c = dvm_object_class(vm, runnable);
   struct dvm_method *run = c ? dvm_find_method(vm, c, "run", "()V") : NULL;
   union dvm_value ignored = { 0 };
   if (run) {
      if (!dvm_call(vm, run, runnable, NULL, 0, &ignored)) return false;
   } else {
      struct dvm_class *iface = dvm__class_by_desc(vm, "Ljava/lang/Runnable;");
      if (dvm_proxy_try_invoke(vm, runnable, iface, "run", "()V",
                               NULL, 0, &ignored) && vm->exception)
         return false;
   }
   RETV();
}

/* setTitle()/getTitle(): kept on the activity (the window caption of a desktop
 * has no use for it, but an app reads back what it set). */
static bool act_setTitle(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   union dvm_value v = { .l = ARG(0).l };
   (void)dvm_set_field(vm, self, "title", "Ljava/lang/CharSequence;", v);
   RETV();
}

static bool act_getTitle(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "title", "Ljava/lang/CharSequence;", &v);
   RETL(v.l ? v.l : dvm_new_string(vm, ""));
}

/* isTaskRoot(): the first activity of the task is the one at the bottom of the
 * stack (or the one the loader started before the stack existed). */
static bool act_isTaskRoot(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)vm; (void)args; (void)nargs;
   RETI(act_index(self) == 0);
}



static bool fm_find(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   const char *tag = nargs ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   union dvm_value added = { 0 };
   (void)dvm_get_field(vm, self, "added", "Ljava/util/ArrayList;", &added);
   struct rt_list *list = list_of(vm, added.l);
   for (uint32_t i = list && tag ? list->size : 0; i > 0; --i) {
      union dvm_value stored = { 0 };
      (void)dvm_get_field(vm, list->items[i-1], "mTag", "Ljava/lang/String;", &stored);
      const char *candidate = dvm_string_utf8(vm, stored.l);
      if (candidate && !strcmp(tag, candidate)) RETL(list->items[i-1]);
   }
   RETL(0);
}

static dvm_ref fm_list(struct dvm *vm, dvm_ref self, const char *field)
{
   union dvm_value value = { 0 };
   (void)dvm_get_field(vm, self, field, "Ljava/util/ArrayList;", &value);
   if (!value.l) {
      value.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      (void)dvm_set_field(vm, self, field, "Ljava/util/ArrayList;", value);
   }
   return value.l;
}

static bool fm_execute(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value pending = { 0 }, ignored = { 0 };
   (void)dvm_get_field(vm, self, "pending", "Ljava/util/ArrayList;", &pending);
   struct rt_list *queue = list_of(vm, pending.l);
   bool changed = queue && queue->size;
   dvm_ref added = changed ? fm_list(vm, self, "added") : 0;
   for (uint32_t i = 0; queue && i < queue->size; ++i) {
      union dvm_value ops = { 0 };
      (void)dvm_get_field(vm, queue->items[i], "ops", "Ljava/util/ArrayList;", &ops);
      struct rt_list *list = list_of(vm, ops.l);
      for (uint32_t j = 0; list && j < list->size; ++j) {
         union dvm_value fragment = { .l = list->items[j] };
         union dvm_value found = { 0 };
         if (!list_contains(vm, added, &fragment, 1, &found)) return false;
         if (found.i) {
            dvm__throw(vm, "java/lang/IllegalStateException", "Fragment already added");
            return false;
         }
         if (!list_add(vm, added, &fragment, 1, &ignored)) return false;
      }
      union dvm_value back={0};
      (void)dvm_get_field(vm,queue->items[i],"addToBackStack","Z",&back);
      if(back.i) {
         dvm_ref stack=fm_list(vm,self,"backStack");
         union dvm_value record={.l=queue->items[i]};
         if(!stack || !list_add(vm,stack,&record,1,&ignored)) return false;
      }
   }
   if (pending.l && !list_clear(vm, pending.l, NULL, 0, &ignored)) return false;
   RETI(changed);
}

static bool fm_run(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   union dvm_value manager = { 0 };
   (void)dvm_get_field(vm, self, "manager", "Landroid/app/FragmentManager;", &manager);
   if (!fm_execute(vm, manager.l, args, nargs, out)) return false;
   RETV();
}

static bool fm_begin(struct dvm *vm, dvm_ref self,
                     const union dvm_value *args, int nargs,
                     union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *tc =
      dvm__class_by_desc(vm, "Landroid/app/FragmentTransaction;");
   dvm_ref transaction = tc ? dvm_new_object(vm, tc) : 0;
   if (transaction) {
      union dvm_value manager = { .l = self };
      (void)dvm_set_field(vm, transaction, "manager",
                          "Landroid/app/FragmentManager;", manager);
   }
   RETL(transaction);
}

static bool ft_add(struct dvm *vm, dvm_ref self,
                   const union dvm_value *args, int nargs,
                   union dvm_value *out)
{
   union dvm_value fragment = { .l = nargs > 0 ? ARG(0).l : 0 }, ignored = {0};
   if (!fragment.l) {
      dvm__throw(vm, "java/lang/NullPointerException", "fragment");
      return false;
   }
   if (nargs > 1 && ARG(1).l) {
      union dvm_value tag = {0};
      (void)dvm_get_field(vm, fragment.l, "mTag", "Ljava/lang/String;", &tag);
      if (tag.l && strcmp(dvm_string_utf8(vm, tag.l), dvm_string_utf8(vm, ARG(1).l))) {
         dvm__throw(vm, "java/lang/IllegalStateException", "Cannot change fragment tag");
         return false;
      }
      (void)dvm_set_field(vm, fragment.l, "mTag", "Ljava/lang/String;", ARG(1));
   }
   dvm_ref ops = fm_list(vm, self, "ops");
   if (!ops || !list_add(vm, ops, &fragment, 1, &ignored)) return false;
   RETL(self);
}

static bool ft_back_stack(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value committed={0};
   (void)dvm_get_field(vm,self,"committed","Z",&committed);
   if(committed.i) {dvm__throw(vm,"java/lang/IllegalStateException","commit already called");return false;}
   (void)dvm_set_field(vm,self,"addToBackStack","Z",(union dvm_value){.i=1});RETL(self);
}
static bool fm_state_saved(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value state={0};
   (void)dvm_get_field(vm,self,"stateSaved","Z",&state);RETI(state.i);
}
static bool fm_back_stack(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   union dvm_value stack_ref={0};(void)dvm_get_field(vm,self,"backStack","Ljava/util/ArrayList;",&stack_ref);
   struct rt_list *stack=list_of(vm,stack_ref.l);
   if(!strcmp(builtin_name(),"getBackStackEntryCount")) RETI(stack ? (int32_t)stack->size : 0);
   union dvm_value state={0},ignored={0};
   (void)dvm_get_field(vm,self,"stateSaved","Z",&state);
   if(state.i) {dvm__throw(vm,"java/lang/IllegalStateException","Can not perform this action after onSaveInstanceState");return false;}
   if(!fm_execute(vm,self,NULL,0,&ignored)) return false;
   (void)dvm_get_field(vm,self,"backStack","Ljava/util/ArrayList;",&stack_ref);stack=list_of(vm,stack_ref.l);
   if(!stack || !stack->size) RETI(0);
   dvm_ref transaction=stack->items[stack->size-1];union dvm_value ops={0},added={0};
   (void)dvm_get_field(vm,transaction,"ops","Ljava/util/ArrayList;",&ops);
   (void)dvm_get_field(vm,self,"added","Ljava/util/ArrayList;",&added);
   struct rt_list *operations=list_of(vm,ops.l);
   for(uint32_t i=operations ? operations->size : 0;i-->0;) {
      union dvm_value fragment={.l=operations->items[i]};
      if(!list_remove_obj(vm,added.l,&fragment,1,&ignored)) return false;
   }
   --stack->size;++stack->mod_count;RETI(1);
}

static bool ft_commit(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value manager = { 0 }, committed = { 0 }, ignored = {0};
   (void)dvm_get_field(vm, self, "committed", "Z", &committed);
   if (committed.i) {
      dvm__throw(vm, "java/lang/IllegalStateException", "commit already called");
      return false;
   }
   (void)dvm_get_field(vm, self, "manager", "Landroid/app/FragmentManager;",
                       &manager);
   union dvm_value saved={0};
   (void)dvm_get_field(vm,manager.l,"stateSaved","Z",&saved);
   if(saved.i && strcmp(builtin_name(),"commitAllowingStateLoss")) {
      dvm__throw(vm,"java/lang/IllegalStateException","Can not perform this action after onSaveInstanceState");return false;
   }
   dvm_ref pending = fm_list(vm, manager.l, "pending");
   union dvm_value transaction = { .l = self };
   if (!pending || !list_add(vm, pending, &transaction, 1, &ignored)) return false;
   committed.i = 1;
   (void)dvm_set_field(vm, self, "committed", "Z", committed);
   if (list_of(vm, pending)->size == 1) {
      struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/FragmentCommit;");
      dvm_ref runnable = c ? dvm_new_object(vm, c) : 0;
      (void)dvm_set_field(vm, runnable, "manager", "Landroid/app/FragmentManager;", manager);
      if (!runnable || !queue_runnable_for(vm, runnable, false, 0, 0, manager.l, 0)) return false;
   }
   union dvm_value back={0},index={0};
   (void)dvm_get_field(vm,self,"addToBackStack","Z",&back);
   if(!back.i) RETI(-1);
   (void)dvm_get_field(vm,manager.l,"nextBackStackIndex","I",&index);
   int32_t allocated=index.i;index.i=(int32_t)((uint32_t)index.i+1u);
   (void)dvm_set_field(vm,manager.l,"nextBackStackIndex","I",index);RETI(allocated);
}

static const struct rt_field rt_fragment_manager_fields[] = {
   { "added", "Ljava/util/ArrayList;" },
   { "pending", "Ljava/util/ArrayList;" }, { "backStack", "Ljava/util/ArrayList;" },
   { "stateSaved", "Z" }, { "nextBackStackIndex", "I" }, F_END,
};

static const struct rt_method rt_fragment_manager[] = {
   M("isStateSaved", "()Z", fm_state_saved),
   M("getBackStackEntryCount", "()I", fm_back_stack),
   M("popBackStackImmediate", "()Z", fm_back_stack),
   M("findFragmentByTag", "(Ljava/lang/String;)Landroid/app/Fragment;", fm_find),
   M("beginTransaction", "()Landroid/app/FragmentTransaction;", fm_begin),
   M("executePendingTransactions", "()Z", fm_execute),
   M_END,
};

static const struct rt_field rt_fragment_transaction_fields[] = {
   { "manager", "Landroid/app/FragmentManager;" },
   { "ops", "Ljava/util/ArrayList;" }, { "committed", "Z" }, { "addToBackStack", "Z" }, F_END,
};

static const struct rt_field rt_fragment_commit_fields[] = {
   { "manager", "Landroid/app/FragmentManager;" }, F_END,
};
static const struct rt_method rt_fragment_commit[] = {
   M("run", "()V", fm_run), M_END,
};

static const struct rt_method rt_fragment_transaction[] = {
   M("addToBackStack", "(Ljava/lang/String;)Landroid/app/FragmentTransaction;", ft_back_stack),
   M("add", "(Landroid/app/Fragment;Ljava/lang/String;)Landroid/app/FragmentTransaction;",
     ft_add),
   M("commit", "()I", ft_commit),
   M("commitAllowingStateLoss", "()I", ft_commit),
   M_END,
};

static bool act_move_task_back(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)vm;(void)nargs;int index=act_index(self);
   if(index<0 || (!ARG(0).i && index!=0)) RETI(0);
   g_task_move_back=true;RETI(1);
}
static bool act_get_action_bar(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value bar={0};
   (void)dvm_get_field(vm,self,"actionBar","Landroid/app/ActionBar;",&bar);RETL(bar.l);
}
static bool act_save_instance_state(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if(!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","outState");return false;}
   union dvm_value manager={0};
   if(!act_get_fragment_manager(vm,self,NULL,0,&manager)) return false;
   (void)dvm_set_field(vm,manager.l,"stateSaved","Z",(union dvm_value){.i=1});RETV();
}
static bool act_back_pressed(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value bar={0},manager={0},result={0};
   (void)dvm_get_field(vm,self,"actionBar","Landroid/app/ActionBar;",&bar);
   if(bar.l) {
      struct dvm_method *collapse=dvm_find_method(vm,dvm_object_class(vm,bar.l),"collapseActionView","()Z");
      if(collapse && !dvm_call(vm,collapse,bar.l,NULL,0,&result)) return false;
      if(result.i) RETV();
   }
   if(!act_get_fragment_manager(vm,self,NULL,0,&manager)) return false;
   struct dvm_class *cls=dvm_object_class(vm,manager.l);
   struct dvm_method *saved=cls ? dvm_find_method(vm,cls,"isStateSaved","()Z") : NULL;
   if(!saved || !dvm_call(vm,saved,manager.l,NULL,0,&result)) return false;
   if(!result.i) {
      struct dvm_method *pop=dvm_find_method(vm,cls,"popBackStackImmediate","()Z");
      if(!pop || !dvm_call(vm,pop,manager.l,NULL,0,&result)) return false;
      if(result.i) RETV();
   }
   if(act_index(self)==0 && lunaria_sdk_int()>=31) {
      const char *launcher=getenv("ANDROID_LAUNCH_ACTIVITY");
      struct dvm_class *activity=dvm_object_class(vm,self);
      const char *name=activity ? activity->name : NULL;
      if(launcher && name) {
         const char *a=launcher,*b=name;
         while(*a && *b && (*a==*b || (*a=='.' && *b=='/'))) {++a;++b;}
         if(!*a && !*b) {g_task_move_back=true;RETV();}
      }
   }
   struct dvm_class *activity=dvm_object_class(vm,self);
   struct dvm_method *finish=activity ? dvm_find_method(vm,activity,"finishAfterTransition","()V") : NULL;
   return finish && dvm_call(vm,finish,self,NULL,0,out);
}

static const struct rt_method rt_activity[] = {
   M("onCreateView", "(Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;", ret_null),
   M("onCreateView", "(Landroid/view/View;Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;", ret_null),
   M("setTitle", "(Ljava/lang/CharSequence;)V", act_setTitle),
   M("getTitle", "()Ljava/lang/CharSequence;", act_getTitle),
   M("isTaskRoot", "()Z", act_isTaskRoot),
   M("moveTaskToBack", "(Z)Z", act_move_task_back),
   M("getActionBar", "()Landroid/app/ActionBar;", act_get_action_bar),
   M("onBackPressed", "()V", act_back_pressed),
   M("onSaveInstanceState", "(Landroid/os/Bundle;)V", act_save_instance_state),
   M("finishAfterTransition", "()V", act_finish),
   M("dispatchTouchEvent","(Landroid/view/MotionEvent;)Z",activity_dispatch_touch),
   M("onTouchEvent","(Landroid/view/MotionEvent;)Z",ret_false),
   /* Framework default; app/AndroidX overrides receive the queued result. */
   M("onRequestPermissionsResult", "(I[Ljava/lang/String;[I)V", empty_void),
   M("requestPermissions", "([Ljava/lang/String;I)V", act_request_permissions),
   /* Activity() is empty on a device; subclasses start with super(). */
   M("<init>", "()V", empty_void),
   M("runOnUiThread", "(Ljava/lang/Runnable;)V", act_runOnUiThread),
   M("getTitle", "()Ljava/lang/CharSequence;", act_get_title),
   M("getReferrer", "()Landroid/net/Uri;", act_getReferrer),
   M("getApplication", "()Landroid/app/Application;", act_get_application),
   M("getComponentName", "()Landroid/content/ComponentName;",
     act_get_component_name),
   M("getLocalClassName", "()Ljava/lang/String;", act_get_local_class_name),
   M("getPreferences", "(I)Landroid/content/SharedPreferences;", act_get_preferences),
   M("getWindow", "()Landroid/view/Window;", act_get_window),
   M("getCurrentFocus", "()Landroid/view/View;", act_getCurrentFocus),
   M("getLayoutInflater", "()Landroid/view/LayoutInflater;", act_get_layout_inflater),
   M("requestWindowFeature", "(I)Z", act_request_window_feature),
   M("getFragmentManager", "()Landroid/app/FragmentManager;",
     act_get_fragment_manager),
   M("getLastNonConfigurationInstance", "()Ljava/lang/Object;", ret_null),
   M("setContentView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V",
     act_set_content_view),
   M("setContentView", "(Landroid/view/View;)V", act_set_content_view),
   M("setContentView", "(I)V", act_set_content_resource),
   M("addContentView",
     "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V",
     act_add_content_view),
   M("findViewById", "(I)Landroid/view/View;", act_find_view_by_id),
   /* setVolumeControlStream(int): which stream the hardware volume keys move
      while this activity is in front.  There are no volume keys here, but the
      value reads back — an activity that sets it and checks is entitled to
      see its own write, and Android's default is USE_DEFAULT_STREAM_TYPE. */
   M("setVolumeControlStream", "(I)V", activity_set_volume_stream),
   M("getVolumeControlStream", "()I", activity_get_volume_stream),
   M("startActivity", "(Landroid/content/Intent;)V", act_startActivity),
   M("startActivity", "(Landroid/content/Intent;Landroid/os/Bundle;)V",
     act_startActivity),
   M("startActivityForResult", "(Landroid/content/Intent;I)V",
     act_startActivityForResult),
   M("startActivityForResult",
     "(Landroid/content/Intent;ILandroid/os/Bundle;)V",
     act_startActivityForResult),
   M("finish", "()V", act_finish),
   M("isFinishing", "()Z", act_isFinishing),
   /* There are no window transitions to override, and a configuration change
    * (rotation, resize) never tears an activity down here. */
   M("overridePendingTransition", "(II)V", nop_void),
   M("isChangingConfigurations", "()Z", ret_false),
   M("setResult", "(I)V", act_setResult),
   M("setResult", "(ILandroid/content/Intent;)V", act_setResult),
   M("getIntent", "()Landroid/content/Intent;", act_getIntent),
   M("setIntent", "(Landroid/content/Intent;)V", act_setIntent),
   M("onActivityResult", "(IILandroid/content/Intent;)V", empty_void),
   M("onPostCreate", "(Landroid/os/Bundle;)V", empty_void),
   M("onPostResume", "()V", empty_void),
   M("onConfigurationChanged", "(Landroid/content/res/Configuration;)V", empty_void),
   M("onWindowFocusChanged", "(Z)V", empty_void),
   M("onCreate", "(Landroid/os/Bundle;)V", act_on_create),
   M("onRestart", "()V", nop_void),
   M("onStart", "()V", act_on_start),
   M("onResume", "()V", act_on_resume),
   M("onPause", "()V", act_on_pause),
   M("onStop", "()V", act_on_stop),
   M("onDestroy", "()V", act_on_destroy),
   M("isDestroyed", "()Z", act_is_destroyed),
   /* Lunaria exposes one full-screen Android display and never places an
    * activity beside another one.  This is the platform state queried by
    * Activity.isInMultiWindowMode(), not an application-specific fallback. */
   M("isInMultiWindowMode", "()Z", ret_false),
   M("registerActivityLifecycleCallbacks",
     "(Landroid/app/Application$ActivityLifecycleCallbacks;)V",
     app_register_lifecycle),
   M("unregisterActivityLifecycleCallbacks",
     "(Landroid/app/Application$ActivityLifecycleCallbacks;)V",
     app_unregister_lifecycle),
   M_END,
};

static dvm_ref li_new(struct dvm *vm, dvm_ref context)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/view/LayoutInflater;");
   dvm_ref inflater = c ? dvm_new_object(vm, c) : 0;
   if (inflater) {
      (void)dvm_set_field(vm, inflater, "context", "Landroid/content/Context;",
                          (union dvm_value){ .l = context });
      if (ui_is_a(vm,context,"android/app/Activity"))
         (void)dvm_set_field(vm,inflater,"mPrivateFactory","Landroid/view/LayoutInflater$Factory2;",
                             (union dvm_value){.l=context});
   }
   return inflater;
}

static bool li_get_context(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   return dvm_get_field(vm, self, "context", "Landroid/content/Context;", out);
}

static bool li_get_factory(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value factory = { 0 };
   bool two=!strcmp(builtin_name(),"getFactory2");
   (void)dvm_get_field(vm,self,two ? "mFactory2" : "mFactory",
      two ? "Landroid/view/LayoutInflater$Factory2;" : "Landroid/view/LayoutInflater$Factory;",&factory);
   RETL(factory.l);
}

static dvm_ref li_merge_factories(struct dvm *vm,dvm_ref first,dvm_ref first2,
                                   dvm_ref second,dvm_ref second2)
{
   dvm_ref merger=dvm_new_object(vm,dvm_find_class(vm,"lunaria/InflaterFactoryMerger"));
   if (!merger) return 0;
   const char *fields[]={"first","first2","second","second2"};
   dvm_ref refs[]={first,first2,second,second2};
   for(int i=0;i<4;++i)
      (void)dvm_set_field(vm,merger,fields[i],"Ljava/lang/Object;",(union dvm_value){.l=refs[i]});
   return merger;
}

static bool li_factory_call(struct dvm *vm,dvm_ref factory,bool two,
                            const union dvm_value args[4],union dvm_value *out)
{
   out->l=0;
   if (!factory) return true;
   const char *sig=two ? "(Landroid/view/View;Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;" :
      "(Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;";
   struct dvm_method *method=dvm_find_method(vm,dvm_object_class(vm,factory),"onCreateView",sig);
   if (!method) {dvm__throw(vm,"java/lang/AbstractMethodError","Inflater factory onCreateView%s",sig); return false;}
   return dvm_call(vm,method,factory,two ? args : args+1,two ? 4 : 3,out);
}

static bool li_merger_create(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   union dvm_value a[4]={{0},{0},{0},{0}};
   memcpy(a+(nargs==4 ? 0 : 1),args,(size_t)nargs*sizeof *args);
   const char *fields[]={"first","second"},*fields2[]={"first2","second2"};
   for(int i=0;i<2;++i) {
      union dvm_value f={0},f2={0};
      (void)dvm_get_field(vm,self,fields[i],"Ljava/lang/Object;",&f);
      (void)dvm_get_field(vm,self,fields2[i],"Ljava/lang/Object;",&f2);
      bool two=nargs==4 && f2.l;
      if (!li_factory_call(vm,two ? f2.l : f.l,two,a,out)) return false;
      if (out->l) return true;
   }
   RETL(0);
}

static bool li_set_factory(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   union dvm_value factory = { .l = nargs > 0 ? ARG(0).l : 0 };
   union dvm_value old={0},old2={0},set={0};
   bool private=!strcmp(builtin_name(),"setPrivateFactory");
   bool two=private || !strcmp(builtin_name(),"setFactory2");
   (void)dvm_get_field(vm,self,"mFactorySet","Z",&set);
   if (!private && set.i) {dvm__throw(vm,"java/lang/IllegalStateException","A factory has already been set on this LayoutInflater"); return false;}
   if (!factory.l) {dvm__throw(vm,"java/lang/NullPointerException","Given factory can not be null"); return false;}
   const char *field=private ? "mPrivateFactory" : "mFactory";
   const char *type=private ? "Landroid/view/LayoutInflater$Factory2;" : "Landroid/view/LayoutInflater$Factory;";
   (void)dvm_get_field(vm,self,field,type,&old);
   if (private) old2=old;
   else (void)dvm_get_field(vm,self,"mFactory2","Landroid/view/LayoutInflater$Factory2;",&old2);
   if (old.l) {
      factory.l=li_merge_factories(vm,factory.l,two ? factory.l : 0,old.l,old2.l);
      if (!factory.l) return false;
   }
   (void)dvm_set_field(vm,self,field,type,factory);
   if (!private) {
      if (two) (void)dvm_set_field(vm,self,"mFactory2","Landroid/view/LayoutInflater$Factory2;",factory);
      (void)dvm_set_field(vm,self,"mFactorySet","Z",(union dvm_value){.i=1});
   }
   RETV();
}

static bool li_clone(struct dvm *vm, dvm_ref self,
                     const union dvm_value *args, int nargs,
                     union dvm_value *out)
{
   if (!nargs || !ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","null inflater context"); return false;}
   union dvm_value made = { .l = li_new(vm, nargs ? ARG(0).l : 0) };
   if (!made.l) return false;
   const char *fields[]={"mFactory","mFactory2","mPrivateFactory"};
   const char *types[]={"Landroid/view/LayoutInflater$Factory;","Landroid/view/LayoutInflater$Factory2;","Landroid/view/LayoutInflater$Factory2;"};
   for(int i=0;i<3;++i) {
      union dvm_value factory={0};
      (void)dvm_get_field(vm,self,fields[i],types[i],&factory);
      if (i!=2 || factory.l) (void)dvm_set_field(vm,made.l,fields[i],types[i],factory);
   }
   RETL(made.l);
}

static uint16_t ax_u16(const unsigned char *p)
{
   uint16_t v; memcpy(&v, p, sizeof v); return v;
}

static uint32_t ax_u32(const unsigned char *p)
{
   uint32_t v; memcpy(&v, p, sizeof v); return v;
}

static size_t ax_len8(const unsigned char *p, size_t left, uint32_t *value)
{
   if (!left) return 0;
   if (!(p[0] & 0x80)) { *value = p[0]; return 1; }
   if (left < 2) return 0;
   *value = ((uint32_t)(p[0] & 0x7f) << 8) | p[1]; return 2;
}

static char **ax_string_pool(const unsigned char *b, size_t n,
                             uint32_t *count_out)
{
   size_t pos = n >= 8 ? ax_u16(b + 2) : n;
   while (pos + 28 <= n) {
      uint16_t type = ax_u16(b + pos), hs = ax_u16(b + pos + 2);
      uint32_t size = ax_u32(b + pos + 4);
      if (size < hs || pos + size > n) break;
      if (type != 0x0001) { pos += size; continue; }
      uint32_t count = ax_u32(b + pos + 8);
      uint32_t flags = ax_u32(b + pos + 16);
      uint32_t strings_off = ax_u32(b + pos + 20);
      if (hs + (uint64_t)count * 4 > size || strings_off > size) return NULL;
      char **strings = calloc(count ? count : 1, sizeof *strings);
      if (!strings) return NULL;
      for (uint32_t i = 0; i < count; ++i) {
         uint32_t off = ax_u32(b + pos + hs + i * 4u);
         size_t q = pos + strings_off + off;
         if (q >= pos + size) continue;
         if (flags & 0x100) {
            uint32_t chars = 0, bytes = 0;
            size_t used = ax_len8(b + q, pos + size - q, &chars);
            if (!used) continue;
            q += used;
            used = ax_len8(b + q, pos + size - q, &bytes);
            if (!used || q + used + bytes > pos + size) continue;
            q += used;
            strings[i] = luna_strndup((const char *)b + q, bytes);
         } else {
            if (q + 2 > pos + size) continue;
            uint32_t chars = ax_u16(b + q); q += 2;
            if (chars & 0x8000) {
               if (q + 2 > pos + size) continue;
               chars = ((chars & 0x7fff) << 16) | ax_u16(b + q); q += 2;
            }
            if (q + (uint64_t)chars * 2 > pos + size) continue;
            strings[i] = malloc((size_t)chars + 1);
            if (!strings[i]) continue;
            /* Resource tag and attribute names are ASCII.  Preserve other
             * BMP text losslessly enough for class/resource identifiers. */
            for (uint32_t k = 0; k < chars; ++k) {
               uint16_t c = ax_u16(b + q + k * 2u);
               strings[i][k] = c < 0x80 ? (char)c : '?';
            }
            strings[i][chars] = '\0';
         }
      }
      *count_out = count;
      return strings;
   }
   return NULL;
}

static dvm_ref li_make_view(struct dvm *vm, const char *tag, dvm_ref context, dvm_ref attributes)
{
   char desc[512];
   if (!tag || !*tag) {dvm__throw(vm,"android/view/InflateException","Missing view class name"); return 0;}
   const char *name = tag;
   const char *class_name = NULL;
   /* <view class="com.foo.Bar"> names its class in an attribute. */
   char named[400];
   if (!strcmp(name, "view") && attributes) {
      union dvm_value names = { 0 }, values = { 0 };
      (void)dvm_get_field(vm, attributes, "attributeNameStrings", "[Ljava/lang/String;", &names);
      (void)dvm_get_field(vm, attributes, "attributeValueStrings", "[Ljava/lang/String;", &values);
      const dvm_ref *keys = names.l ? dvm_array_data(vm, names.l) : NULL;
      const dvm_ref *vals = values.l ? dvm_array_data(vm, values.l) : NULL;
      const uint32_t n = names.l ? dvm_array_length(vm, names.l) : 0;
      for (uint32_t i = 0; keys && vals && i < n && i < dvm_array_length(vm, values.l); ++i) {
         const char *k = keys[i] ? dvm_string_utf8(vm, keys[i]) : NULL;
         const char *v = vals[i] ? dvm_string_utf8(vm, vals[i]) : NULL;
         if (k && v && !strcmp(k, "class") && *v) {
            /* A bare name is a platform view: android.view.<name>. */
            snprintf(named, sizeof named, "%s%s", strchr(v, '.') ? "" : "android.view.", v);
            name = named;
            break;
         }
      }
   }
   if (!strcmp(name, "view") || !strcmp(name, "View"))
      class_name = "android/view/View";
   if (class_name) snprintf(desc, sizeof desc, "L%s;", class_name);
   else if (strchr(name, '.')) {
      size_t j = 1; desc[0] = 'L';
      for (size_t i = 0; name[i] && j + 2 < sizeof desc; ++i)
         desc[j++] = name[i] == '.' ? '/' : name[i];
      desc[j++] = ';'; desc[j] = '\0';
   } else {
      snprintf(desc, sizeof desc, "Landroid/widget/%s;", name);
   }
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   if ((!c || c->synthesized) && !strchr(name, '.')) {
      snprintf(desc, sizeof desc, "Landroid/view/%s;", name);
      c = dvm__class_by_desc(vm, desc);
   }
   if (!c || c->synthesized || !dvm__class_assignable(vm,c,dvm_find_class(vm,"android/view/View"))) {
      dvm__throw(vm,"android/view/InflateException","Error inflating class %s",name);
      return 0;
   }
   dvm_ref view = c ? dvm_new_object(vm, c) : 0;
   if (!view) return 0;
   union dvm_value cv = { .l = context };
   (void)dvm_set_field(vm, view, "context", "Landroid/content/Context;", cv);

   /* XML constructors receive the AttributeSet positioned on this tag. */
   struct dvm_method *ctor = dvm_find_method(
      vm, c, "<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V");
   union dvm_value ignored = { 0 };
   if (ctor) {
      union dvm_value a[2] = { { .l = context }, { .l = attributes } };
      if (!dvm_call(vm, ctor, view, a, 2, &ignored)) return 0;
   } else {
      ctor = dvm_find_method(vm, c, "<init>", "(Landroid/content/Context;)V");
      if (ctor && !dvm_call(vm, ctor, view, &cv, 1, &ignored))
         return 0;
   }
   return view;
}

/* --- inflated layout attributes ----------------------------------------- *
 *
 * The inflater used to read exactly two attributes out of a compiled layout —
 * `id` and `layout` (for <include>) — and drop the rest on the floor.  A tree
 * built that way has the right shape and no geometry at all: no width, no
 * height, no orientation, no weight, no text, no colour.  That is enough for
 * findViewById() to answer, which is all the emulator ever asked of it, but it
 * is not enough to put the layout on screen, and it is not what the platform
 * does.  Everything below turns a compiled Res_value into the field the widget
 * would have been constructed with.
 *
 * Value types are the Res_value ones from androidfw/ResourceTypes.h.
 */
enum {
   AX_REFERENCE  = 0x01,
   AX_ATTRIBUTE  = 0x02,
   AX_STRING     = 0x03,
   AX_FLOAT      = 0x04,
   AX_DIMENSION  = 0x05,
   AX_FRACTION   = 0x06,
   AX_INT_DEC    = 0x10,
   AX_INT_HEX    = 0x11,
   AX_INT_BOOL   = 0x12,
   AX_COLOR_ARGB8 = 0x1c,
   AX_COLOR_RGB8  = 0x1d,
   AX_COLOR_ARGB4 = 0x1e,
   AX_COLOR_RGB4  = 0x1f,
};

/* A length in device pixels.  MATCH_PARENT (-1) and WRAP_CONTENT (-2) arrive
 * as plain ints and have to survive as themselves. */
static int32_t ax_dimension_px(struct dvm *vm, dvm_ref context, uint8_t type, uint32_t data)
{
   union dvm_value theme = {0}, style = {0};
   if (type == AX_ATTRIBUTE) {
      struct dvm_class *c = dvm_object_class(vm, context);
      struct dvm_method *get = c ? dvm_find_method(vm, c, "getTheme", "()Landroid/content/res/Resources$Theme;") : NULL;
      if (get && !dvm_call(vm, get, context, NULL, 0, &theme)) return 0;
      (void)dvm_get_field(vm, theme.l, "style", "I", &style);
   }
   unsigned depth = 0;
   while ((type == AX_ATTRIBUTE || type == AX_REFERENCE) && data && depth++ < 32) {
      int32_t resolved = 0;
      const char *text = NULL;
      int kind = type == AX_ATTRIBUTE
         ? arm_exec_apk_style_value((uint32_t)style.i, data, &resolved, &text)
         : arm_exec_apk_resource_value(data, &resolved, &text);
      if (!kind || text) {
         dvm__throw(vm, "java/lang/UnsupportedOperationException", "unresolved dimension #0x%08x", data);
         return 0;
      }
      type = (uint8_t)kind;
      data = (uint32_t)resolved;
   }
   if (type == AX_DIMENSION)
      return android_dimension_pixel_size((int32_t)data);
   if (type == AX_FLOAT) {
      float f; uint32_t u = data; memcpy(&f, &u, sizeof f);
      return (int32_t)lrintf(f);
   }
   /* A layout is free to write @dimen/foo anywhere a length is expected.  The
    * reference has to be followed, because the id itself is a large positive
    * number and would otherwise be used as a length in pixels. */
   if (type >= AX_INT_DEC && type <= AX_COLOR_RGB4) return (int32_t)data;
   if (!type || (type == AX_REFERENCE && !data)) return 0;
   dvm__throw(vm, "java/lang/UnsupportedOperationException", "invalid dimension type 0x%x", type);
   return 0;
}

/* Colours can be written inline (#RRGGBB) or named through a colour resource
 * or a ColorStateList; the reference is resolved the same way the platform
 * resolves it, through the resource table. */
static bool ax_color(struct dvm *vm, uint8_t type, uint32_t data,
                     int32_t *out_argb)
{
   (void)vm;
   if (type >= AX_COLOR_ARGB8 && type <= AX_COLOR_RGB4) {
      *out_argb = (int32_t)data;
      return true;
   }
   if (type == AX_REFERENCE && data) {
      int32_t value = 0; const char *sv = NULL;
      int kind = res_value_resolved(data, &value, &sv);
      /* A colour resource resolves to an int; a ColorStateList resolves to a
       * file path, and its default colour is not knowable from here. */
      if (kind>=0x10 && kind<=0x1f) { *out_argb = value; return true; }
   }
   return false;
}


/* --- XML drawables -------------------------------------------------------
 *
 * Drawable.createFromXml(): a drawable resource that is not a bitmap is a
 * small program — <shape>, <selector>, <layer-list>, <ripple>, <inset>,
 * <color> — and a layout's `android:background="@drawable/..."` is the usual
 * way an app asks for one.  Each root is built as the class the platform
 * builds, filled from its attributes.  The overlay paints one appearance per
 * view, so a list drawable (selector, layer-list, ripple, inset) stands for
 * the drawable it shows at rest, kept in `current`. */
struct dx {
   const unsigned char *b;
   size_t n, pos;
   char **s;
   uint32_t ns;
   dvm_ref theme;
   const unsigned char *resource_map;
   uint32_t resource_count;
};

struct dx_tag {
   const char *name;
   size_t attrs, end;
   uint16_t asz, count;
};

/* The next start or end tag: 0x0102, 0x0103, or 0 at the end of the document. */
static int dx_next(struct dx *x, struct dx_tag *t)
{
   while (x->pos + 8 <= x->n) {
      const unsigned char *p = x->b + x->pos;
      const uint16_t type = ax_u16(p);
      const uint32_t size = ax_u32(p + 4);
      if (size < 8 || x->pos + size > x->n) break;
      const size_t at = x->pos;
      x->pos += size;
      if (type == 0x0180) {
         uint16_t header=ax_u16(p+2);
         if (header>=8 && header<=size) {
            x->resource_map=p+header;
            x->resource_count=(size-header)/4;
         }
      }
      if (type == 0x0102 && at + 36 <= x->n) {
         const uint32_t ni = ax_u32(p + 20);
         t->name = ni < x->ns && x->s[ni] ? x->s[ni] : "";
         t->attrs = at + 16u + ax_u16(p + 24);
         t->asz = ax_u16(p + 26);
         t->count = ax_u16(p + 28);
         t->end = at + size;
         return 0x0102;
      }
      if (type == 0x0103) return 0x0103;
   }
   return 0;
}

/* Skip to the end tag that closes the element whose start tag was just read. */
static void dx_skip(struct dx *x)
{
   struct dx_tag t;
   for (int depth = 1, e; depth > 0 && (e = dx_next(x, &t));)
      depth += e == 0x0102 ? 1 : -1;
}

static bool dx_attr(const struct dx *x, const struct dx_tag *t, const char *name,
                    uint8_t *type, uint32_t *data)
{
   for (uint16_t i = 0; t->asz >= 20 && i < t->count; ++i) {
      const size_t q = t->attrs + (size_t)i * t->asz;
      if (q + 20 > t->end) break;
      const uint32_t ai = ax_u32(x->b + q + 4);
      const char *an = ai < x->ns ? x->s[ai] : NULL;
      if (an && !strcmp(an, name)) {
         *type = x->b[q + 15];
         *data = ax_u32(x->b + q + 16);
         return true;
      }
   }
   return false;
}

static bool dx_resolve_attribute(struct dvm *vm, const struct dx *x,
                                 uint8_t *type, uint32_t *data)
{
   uint32_t visited[64]; unsigned count = 0;
   while (*type == AX_ATTRIBUTE && x->theme && count < 64) {
      for (unsigned i = 0; i < count; ++i) if (visited[i] == *data) return false;
      visited[count++] = *data;
      int32_t value = 0; const char *text = NULL;
      int kind = theme_style_value(vm, x->theme, *data, &value, &text);
      if (!kind) return false;
      *type = (uint8_t)kind; *data = (uint32_t)value;
   }
   return *type != AX_ATTRIBUTE;
}

static bool dx_color(struct dvm *vm, const struct dx *x, const struct dx_tag *t,
                     const char *name, int32_t *argb)
{
   uint8_t type; uint32_t data;
   if (!dx_attr(x, t, name, &type, &data) || !dx_resolve_attribute(vm, x, &type, &data)) return false;
   return ax_color(vm, type, data, argb);
}

static int32_t dx_px(struct dvm *vm, const struct dx *x, const struct dx_tag *t,
                     const char *name, int32_t def)
{
   uint8_t type; uint32_t data;
   if (!dx_attr(x, t, name, &type, &data)) return def;
   const int32_t px = ax_dimension_px(vm, 0, type, data);
   if (dvm_exception(vm)) { dvm_clear_exception(vm); return def; }
   return px;
}

static int32_t dx_int(const struct dx *x, const struct dx_tag *t, const char *name,
                      int32_t def)
{
   uint8_t type; uint32_t data;
   return dx_attr(x, t, name, &type, &data) ? (int32_t)data : def;
}

static dvm_ref dx_new(struct dvm *vm, const char *desc)
{
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   return c ? dvm_new_object(vm, c) : 0;
}

/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* Private C runtime implementation of Android drawable layer composition. */
static const char *const layer_edges[] = {"left","top","right","bottom"};
static int32_t layer_int(struct dvm *vm,dvm_ref obj,const char *field)
{
   union dvm_value v={0};
   (void)dvm_get_field(vm,obj,field,"I",&v);
   return v.i;
}
static dvm_ref layer_ref(struct dvm *vm,dvm_ref obj,const char *field,const char *type)
{
   union dvm_value v={0};
   (void)dvm_get_field(vm,obj,field,type,&v);
   return v.l;
}
static dvm_ref layer_list(struct dvm *vm,dvm_ref self)
{
   dvm_ref list=layer_ref(vm,self,"layers","Ljava/util/List;");
   if (!list) {
      list=dx_new(vm,"Ljava/util/ArrayList;");
      union dvm_value ignored={0};
      if (list) {
         (void)dvm_set_field(vm,self,"layers","Ljava/util/List;",(union dvm_value){.l=list});
         if (!list_init(vm,list,NULL,0,&ignored)) return 0;
      }
   }
   return list;
}
static bool layer_append(struct dvm *vm,dvm_ref self,dvm_ref child,const int32_t *meta)
{
   dvm_ref list=layer_list(vm,self);
   if (!list) return false;
   dvm_ref item=dx_new(vm,"Llunaria/DrawableLayer;");
   if (!item) return false;
   dvm_pin(vm,item);
   static const char *const fields[]={"id","left","top","right","bottom",
      "start","end","width","height","gravity"};
   (void)dvm_set_field(vm,item,"drawable","Landroid/graphics/drawable/Drawable;",
                       (union dvm_value){.l=child});
   for (int i=0;i<10;++i)
      (void)dvm_set_field(vm,item,fields[i],"I",(union dvm_value){.i=meta[i]});
   union dvm_value arg={.l=item},ignored={0};
   bool good=list_add(vm,list,&arg,1,&ignored);
   if (good && child) {
      arg.l=self;
      good=am_vcall(vm,child,"setCallback",
         "(Landroid/graphics/drawable/Drawable$Callback;)V",&arg,1,&ignored);
   }
   dvm_unpin(vm,item);
   return good;
}
static bool layer_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                       int nargs,union dvm_value *out)
{
   if (!nargs || !ARG(0).l) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","layers must be non-null"); return false;
   }
   if (!drawable_init(vm,self,NULL,0,out)) return false;
   int32_t meta[10]={-1,0,0,0,0,INT32_MIN,INT32_MIN,-1,-1,0};
   uint32_t n=dvm_array_length(vm,ARG(0).l);
   for (uint32_t i=0;i<n;++i) {
      dvm_ref child=((dvm_ref *)dvm_array_data(vm,ARG(0).l))[i];
      if (!layer_append(vm,self,child,meta)) return false;
   }
   RETV();
}
static bool layer_access(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                         int nargs,union dvm_value *out)
{
   (void)nargs;
   struct rt_list *list=list_of(vm,layer_list(vm,self));
   if (!strcmp(builtin_name(),"getNumberOfLayers")) RETI(list ? (int32_t)list->size : 0);
   if (!strcmp(builtin_name(),"findDrawableByLayerId")) {
      for (uint32_t i=list ? list->size : 0;i>0;--i) {
         dvm_ref item=list->items[i-1];
         if (layer_int(vm,item,"id")==ARG(0).i)
            RETL(layer_ref(vm,item,"drawable","Landroid/graphics/drawable/Drawable;"));
      }
      RETL(0);
   }
   if (!list || ARG(0).i<0 || (uint32_t)ARG(0).i>=list->size) {
      dvm__throw(vm,"java/lang/IndexOutOfBoundsException","layer index %d",ARG(0).i); return false;
   }
   dvm_ref item=list->items[ARG(0).i];
   if (!strcmp(builtin_name(),"setId")) {
      (void)dvm_set_field(vm,item,"id","I",ARG(1)); RETV();
   }
   if (!strcmp(builtin_name(),"getId")) RETI(layer_int(vm,item,"id"));
   RETL(layer_ref(vm,item,"drawable","Landroid/graphics/drawable/Drawable;"));
}
static bool layer_bounds(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                         int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref container=dx_new(vm,"Landroid/graphics/Rect;");
   if (!container) return false;
   dvm_pin(vm,container);
   dvm_ref result=dx_new(vm,"Landroid/graphics/Rect;");
   if (!result) {dvm_unpin(vm,container); return false;}
   dvm_pin(vm,result);
   dvm_ref padding=dx_new(vm,"Landroid/graphics/Rect;");
   if (!padding) {dvm_unpin(vm,result);dvm_unpin(vm,container);return false;}
   dvm_pin(vm,padding);
   int32_t pad[4]={0},rtl=layer_int(vm,self,"layoutDirection");
   bool good=true;
   struct rt_list *list=list_of(vm,layer_list(vm,self));
   uint32_t count=list ? list->size : 0;
   for (uint32_t i=0;good && i<count;++i) {
      list=list_of(vm,layer_ref(vm,self,"layers","Ljava/util/List;"));
      dvm_ref item=list->items[i];
      dvm_ref child=layer_ref(vm,item,"drawable","Landroid/graphics/drawable/Drawable;");
      if (!child) continue;
      union dvm_value edges[4],ignored={0},iw={0},ih={0};
      for (int j=0;j<4;++j) {
         int32_t inset=layer_int(vm,item,layer_edges[j]);
         if (j==0 || j==2) {
            int32_t rel=layer_int(vm,item,((j==0) != (rtl==1)) ? "start" : "end");
            if (rel!=INT32_MIN) inset=rel;
         }
         int32_t delta=gravity_add(inset,pad[j]);
         int32_t bound=layer_int(vm,ARG(0).l,layer_edges[j]);
         edges[j].i=j<2 ? gravity_add(bound,delta) : gravity_sub(bound,delta);
      }
      good=rect_set(vm,container,edges,4,&ignored) &&
         am_vcall(vm,child,"getIntrinsicWidth","()I",NULL,0,&iw) &&
         am_vcall(vm,child,"getIntrinsicHeight","()I",NULL,0,&ih);
      int32_t w=layer_int(vm,item,"width"),h=layer_int(vm,item,"height");
      int32_t gravity=layer_int(vm,item,"gravity");
      if (!(gravity&7)) gravity |= w<0 ? 7 : 0x800003;
      if (!(gravity&0x70)) gravity |= h<0 ? 0x70 : 0x30;
      if (w<0 && iw.i<0) gravity |= 7;
      if (h<0 && ih.i<0) gravity |= 0x70;
      union dvm_value apply[]={{.i=gravity},{.i=w<0 ? iw.i : w},{.i=h<0 ? ih.i : h},
         {.l=container},{.l=result},{.i=rtl}};
      if (good) good=gravity_apply(vm,0,apply,6,&ignored) &&
         am_vcall(vm,child,"setBounds","(Landroid/graphics/Rect;)V",&apply[4],1,&ignored);
      if (good && layer_int(vm,self,"paddingMode")==0) {
         union dvm_value arg={.l=padding};
         good=am_vcall(vm,child,"getPadding","(Landroid/graphics/Rect;)Z",&arg,1,&ignored);
         if (good) for (int j=0;j<4;++j) pad[j]=gravity_add(pad[j],layer_int(vm,padding,layer_edges[j]));
      }
   }
   dvm_unpin(vm,padding); dvm_unpin(vm,result); dvm_unpin(vm,container);
   if (!good) return false;
   RETV();
}
static bool layer_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                       int nargs,union dvm_value *out)
{
   (void)nargs;
   struct rt_list *list=list_of(vm,layer_list(vm,self));
   uint32_t count=list ? list->size : 0;
   union dvm_value ignored={0};
   for (uint32_t i=0;i<count;++i) {
      list=list_of(vm,layer_ref(vm,self,"layers","Ljava/util/List;"));
      dvm_ref child=layer_ref(vm,list->items[i],"drawable","Landroid/graphics/drawable/Drawable;");
      if (child && !am_vcall(vm,child,"draw","(Landroid/graphics/Canvas;)V",args,1,&ignored)) return false;
   }
   RETV();
}
static bool layer_inset(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                        int nargs,union dvm_value *out)
{
   (void)nargs;
   struct rt_list *list=list_of(vm,layer_list(vm,self));
   if (!list || ARG(0).i<0 || (uint32_t)ARG(0).i>=list->size) {
      dvm__throw(vm,"java/lang/IndexOutOfBoundsException","layer index %d",ARG(0).i); return false;
   }
   dvm_ref item=list->items[ARG(0).i];
   for (int i=0;i<4;++i) (void)dvm_set_field(vm,item,layer_edges[i],"I",args[i+1]);
   (void)dvm_set_field(vm,item,"start","I",(union dvm_value){.i=INT32_MIN});
   (void)dvm_set_field(vm,item,"end","I",(union dvm_value){.i=INT32_MIN});
   union dvm_value bounds={0};
   if (!drawable_get_bounds(vm,self,NULL,0,&bounds)) return false;
   return layer_bounds(vm,self,&bounds,1,out);
}
static const struct rt_field rt_layer_item_fields[]={
   {"drawable","Landroid/graphics/drawable/Drawable;"},
   {"id","I"},{"left","I"},{"top","I"},{"right","I"},{"bottom","I"},
   {"start","I"},{"end","I"},{"width","I"},{"height","I"},{"gravity","I"},F_END
};
static const struct rt_field rt_layer_drawable_fields[]={
   {"layers","Ljava/util/List;"},{"paddingMode","I"},
   {"current","Landroid/graphics/drawable/Drawable;"},F_END
};
static const struct rt_method rt_layer_drawable[]={
   M("<init>","([Landroid/graphics/drawable/Drawable;)V",layer_init),
   M("getNumberOfLayers","()I",layer_access),
   M("getDrawable","(I)Landroid/graphics/drawable/Drawable;",layer_access),
   M("getId","(I)I",layer_access), M("setId","(II)V",layer_access),
   M("findDrawableByLayerId","(I)Landroid/graphics/drawable/Drawable;",layer_access),
   M("setLayerInset","(IIIII)V",layer_inset),
   M("onBoundsChange","(Landroid/graphics/Rect;)V",layer_bounds),
   M("draw","(Landroid/graphics/Canvas;)V",layer_draw),M_END
};

/* TransitionDrawable's state follows the Android transition clock: the first
 * draw starts the clock, and reversing uses the last drawn alpha. */
static int transition_int(struct dvm *vm,dvm_ref self,const char *name)
{
   union dvm_value value={0};
   (void)dvm_get_field(vm,self,name,"I",&value);return value.i;
}
static int64_t transition_started(struct dvm *vm,dvm_ref self)
{
   union dvm_value value={0};
   (void)dvm_get_field(vm,self,"transitionStarted","J",&value);return value.j;
}
static bool transition_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!layer_init(vm,self,args,nargs,out)) return false;
   rt_set_int(vm,self,"transitionState",2);RETV();
}
static bool transition_control(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   const char *name=dvm__builtin_method()->name;
   if(!strcmp(name,"isCrossFadeEnabled")) RETI(transition_int(vm,self,"crossFade"));
   if(!strcmp(name,"setCrossFadeEnabled")) {rt_set_int(vm,self,"crossFade",ARG(0).i!=0);RETV();}
   int alpha=transition_int(vm,self,"transitionAlpha");
   if(!strcmp(name,"resetTransition") || !strcmp(name,"showSecondLayer")) {
      rt_set_int(vm,self,"transitionAlpha",!strcmp(name,"showSecondLayer") ? 255 : 0);
      rt_set_int(vm,self,"transitionState",2);
   } else {
      int from=0,to=255,duration=ARG(0).i,reverse=0;
      bool original=true;
      if(!strcmp(name,"reverseTransition")) {
         int64_t elapsed=sysclock_ms(CLOCK_MONOTONIC)-transition_started(vm,self);
         int prior_duration=transition_int(vm,self,"transitionDuration");
         if(elapsed>prior_duration) {
            if(transition_int(vm,self,"transitionTo")!=0) {from=255;to=0;reverse=1;}
            alpha=from;
         } else {
            original=false;
            reverse=!transition_int(vm,self,"transitionReverse");
            from=alpha;to=reverse ? 0 : 255;
            duration=(int)(reverse ? elapsed : transition_int(vm,self,"transitionOriginalDuration")-elapsed);
         }
      } else alpha=0;
      if(original)
         rt_set_int(vm,self,"transitionOriginalDuration",ARG(0).i);
      rt_set_int(vm,self,"transitionAlpha",alpha);
      rt_set_int(vm,self,"transitionFrom",from);rt_set_int(vm,self,"transitionTo",to);
      rt_set_int(vm,self,"transitionDuration",duration);rt_set_int(vm,self,"transitionReverse",reverse);
      rt_set_int(vm,self,"transitionState",0);
   }
   ui_invalidate();RETV();
}
static bool transition_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   int state=transition_int(vm,self,"transitionState"),alpha=transition_int(vm,self,"transitionAlpha");
   bool done=true,cross=transition_int(vm,self,"crossFade")!=0;
   if(state==0) {
      (void)dvm_set_field(vm,self,"transitionStarted","J",(union dvm_value){.j=sysclock_ms(CLOCK_MONOTONIC)});
      rt_set_int(vm,self,"transitionState",1);done=false;
   } else if(state==1) {
      int duration=transition_int(vm,self,"transitionDuration");
      int64_t elapsed=sysclock_ms(CLOCK_MONOTONIC)-transition_started(vm,self);
      float ratio=duration>0 ? (float)elapsed/(float)duration : 1.f;
      done=ratio>=1.f;if(ratio>1.f) ratio=1.f;
      int from=transition_int(vm,self,"transitionFrom"),to=transition_int(vm,self,"transitionTo");
      alpha=(int)((float)from+(float)(to-from)*ratio);rt_set_int(vm,self,"transitionAlpha",alpha);
   }
   struct rt_list *layers=list_of(vm,layer_list(vm,self));
   if(!layers || layers->size<2) {dvm__throw(vm,"java/lang/IndexOutOfBoundsException","transition requires two layers");return false;}
   dvm_ref children[2];
   for(int i=0;i<2;++i) children[i]=layer_ref(vm,layers->items[i],"drawable","Landroid/graphics/drawable/Drawable;");
   for(int i=0;i<2;++i) {
      bool draw=i==0 ? (!done || !cross || alpha==0) : (done ? alpha==255 : alpha>0);
      if(!draw || !children[i]) continue;
      bool temporary=!done && (i==1 || cross);
      union dvm_value ignored={0},opacity={.i=i==0 ? 255-alpha : alpha};
      if(temporary && !am_vcall(vm,children[i],"setAlpha","(I)V",&opacity,1,&ignored)) return false;
      bool good=am_vcall(vm,children[i],"draw","(Landroid/graphics/Canvas;)V",args,1,&ignored);
      if(temporary) {opacity.i=255;(void)am_vcall(vm,children[i],"setAlpha","(I)V",&opacity,1,&ignored);}
      if(!good) return false;
   }
   if(!done) ui_invalidate();RETV();
}
static const struct rt_field rt_transition_fields[]={
   {"transitionState","I"},{"transitionAlpha","I"},{"transitionFrom","I"},{"transitionTo","I"},
   {"transitionDuration","I"},{"transitionOriginalDuration","I"},{"transitionReverse","I"},
   {"crossFade","I"},{"transitionStarted","J"},F_END
};
static const struct rt_method rt_transition_drawable[]={
   M("<init>","([Landroid/graphics/drawable/Drawable;)V",transition_init),
   M("startTransition","(I)V",transition_control),M("reverseTransition","(I)V",transition_control),
   M("resetTransition","()V",transition_control),M("showSecondLayer","()V",transition_control),
   M("setCrossFadeEnabled","(Z)V",transition_control),M("isCrossFadeEnabled","()Z",transition_control),
   M("draw","(Landroid/graphics/Canvas;)V",transition_draw),M_END
};

static dvm_ref drawable_from_resource(struct dvm *vm, uint32_t id, unsigned depth, dvm_ref theme);
static dvm_ref dx_build(struct dvm *vm, struct dx *x, const struct dx_tag *t,
                        unsigned depth);

/* The drawable an <item> shows: its `drawable` attribute, or the element
 * nested inside it. */
static dvm_ref dx_item_drawable(struct dvm *vm, struct dx *x, const struct dx_tag *item,
                                unsigned depth)
{
   uint8_t type = 0; uint32_t data = 0;
   dvm_ref d = 0;
   if (dx_attr(x, item, "drawable", &type, &data) && dx_resolve_attribute(vm, x, &type, &data)) {
      int32_t argb = 0;
      if (type == AX_REFERENCE) {
         d=drawable_from_resource(vm,data,depth+1,x->theme);
         int32_t raw=0; const char *path=NULL;
         int kind=arm_exec_apk_resource_value(data,&raw,&path);
         bool xml=kind=='L' && path && strlen(path)>4 && !strcasecmp(path+strlen(path)-4,".xml");
         if (!d && !xml && !dvm_exception(vm)) {
            dvm_ref resources=dx_new(vm,"Landroid/content/res/Resources;");
            if (resources) {
               dvm_pin(vm,resources);
               union dvm_value input[2]={{.i=(int32_t)data},{.l=x->theme}},value={0};
               if (res_get_drawable(vm,resources,input,2,&value)) d=value.l;
               dvm_unpin(vm,resources);
            }
         }
      }
      else if (ax_color(vm, type, data, &argb)) {
         d = dx_new(vm, "Landroid/graphics/drawable/ColorDrawable;");
         union dvm_value c = { .i = argb };
         if (d) {
            union dvm_value ignored={0};
            (void)drawable_init(vm,d,NULL,0,&ignored);
            (void)color_drawable_init(vm,d,&c,1,&ignored);
         }
      }
   }
   struct dx_tag c;
   for (int depth_in = 1, e; depth_in > 0 && (e = dx_next(x, &c));) {
      if (e == 0x0103) { --depth_in; continue; }
      if (depth_in == 1 && !d) d = dx_build(vm, x, &c, depth + 1);
      else dx_skip(x);
   }
   if (!d && !dvm_exception(vm)) {
      if (dvm__env("LUNARIA_TRACE_DRAWABLE"))
         fprintf(stderr,"[drawable] missing item: theme=%x type=%x value=%08x depth=%u\n",
                 x->theme,type,data,depth);
      dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException",
                  "<item> requires a drawable attribute or child drawable");
   }
   return d;
}

/* Compiled XML maps attribute-name string indices to resource IDs. Keep
 * positive requirements and negative exclusions, including application states. */
static dvm_ref dx_item_states(struct dvm *vm,const struct dx *x,const struct dx_tag *t)
{
   dvm_ref array=dvm_new_array(vm,'I',"I",t->count);
   if (!array) return 0;
   int32_t *states=dvm_array_data(vm,array); uint32_t count=0;
   for (uint16_t i=0; t->asz>=20 && i<t->count; ++i) {
      size_t q=t->attrs+(size_t)i*t->asz;
      if (q+20>t->end) break;
      uint32_t name=ax_u32(x->b+q+4);
      uint32_t id=name<x->resource_count ? ax_u32(x->resource_map+name*4) : 0;
      const char *text=name<x->ns ? x->s[name] : NULL;
      if (!id || (text && (!strcmp(text,"drawable") || !strcmp(text,"id")))) continue;
      uint32_t value=ax_u32(x->b+q+16);
      states[count++]=value ? (int32_t)id : -(int32_t)id;
   }
   /* Trim the logical array without another allocation: no references to it
    * have escaped, and the unused tail contains only zero primitive values. */
   dvm__obj(vm,array)->length=count;
   return array;
}

/* Scalar colors need no guest ColorStateList allocation. */
static bool color_value_argb(struct dvm *vm,int kind,int32_t value,const char *text,
                             dvm_ref theme,unsigned depth,union dvm_value *out)
{
   if(depth>32) {dvm__throw(vm,"android/content/res/Resources$NotFoundException","Cyclic color reference");return false;}
   if(kind==1) {
      kind=res_value_resolved((uint32_t)value,&value,&text);
      if(!kind) {dvm__throw(vm,"android/content/res/Resources$NotFoundException","Color resource could not be resolved");return false;}
   }
   if(kind==2 && theme) {
      int next=theme_style_value(vm,theme,(uint32_t)value,&value,&text);
      if(next) return color_value_argb(vm,next,value,text,theme,depth+1,out);
   }
   if(kind>=0x10 && kind<=0x1f) {out->i=value;return true;}
   dvm_ref colors=color_value_list(vm,kind,value,text,theme,depth);
   return colors && dvm_get_field(vm,colors,"color","I",out);
}

/* Shared resource/TypedArray color loading. Keep resource IDs out of ARGB. */
static dvm_ref color_value_list(struct dvm *vm,int kind,int32_t value,
                               const char *text,dvm_ref theme,unsigned depth)
{
   if (depth>32) goto missing;
   if (kind==1) {
      kind=res_value_resolved((uint32_t)value,&value,&text);
      if (!kind) goto missing;
   }
   if (kind==2) {
      if (!theme) goto invalid;
      int next=theme_style_value(vm,theme,(uint32_t)value,&value,&text);
      if (!next) goto invalid;
      return color_value_list(vm,next,value,text,theme,depth+1);
   }
   if (kind>=0x10 && kind<=0x1f) return color_state_list_new(vm,value);
   if (kind!='L' || !text) goto invalid;
   size_t length=0;
   unsigned char *bytes=arm_exec_asset_read(text,&length);
   if (!bytes || length<8 || ax_u16(bytes)!=3) {free(bytes);goto missing;}
   struct dx xml={.b=bytes,.n=length,.pos=ax_u16(bytes+2),.theme=theme};
   xml.s=ax_string_pool(bytes,length,&xml.ns);
   struct dx_tag tag;
   bool good=xml.s && dx_next(&xml,&tag)==0x0102 && !strcmp(tag.name,"selector");
   struct dx scan=xml;unsigned count=0;int nesting=1,event;
   while (good && nesting && (event=dx_next(&scan,&tag))) {
      if (event==0x0103) --nesting;
      else {if(nesting==1 && !strcmp(tag.name,"item")) ++count;++nesting;}
   }
   good=good && nesting==0;
   dvm_ref result=good ? dx_new(vm,"Landroid/content/res/ColorStateList;") : 0;
   if (result) dvm_pin(vm,result); else good=false;
   dvm_ref specs=good ? dvm_new_array(vm,'L',"[I",count) : 0;
   if (specs) (void)dvm_set_field(vm,result,"stateSpecs","[[I",(union dvm_value){.l=specs}); else good=false;
   dvm_ref colors=good ? dvm_new_array(vm,'I',"I",count) : 0;
   if (colors) (void)dvm_set_field(vm,result,"colors","[I",(union dvm_value){.l=colors}); else good=false;
   unsigned index=0;bool pending=false;
   while (good && (event=dx_next(&xml,&tag))==0x0102) {
      if (strcmp(tag.name,"item")) {dx_skip(&xml);continue;}
      uint8_t type=0;uint32_t data=0;int32_t color=(int32_t)0xffff00ffu;
      if (dx_attr(&xml,&tag,"color",&type,&data)) {
         if (!dx_resolve_attribute(vm,&xml,&type,&data)) pending=true;
         else {
            union dvm_value v={0};
            if (!color_value_argb(vm,type,(int32_t)data,NULL,theme,depth+1,&v)) {good=false;break;}
            color=v.i;
         }
      }
      float alpha=1;
      if (dx_attr(&xml,&tag,"alpha",&type,&data)) {
         if (!dx_resolve_attribute(vm,&xml,&type,&data)) pending=true;
         else {
            int32_t raw=(int32_t)data;const char *unused=NULL;int k=type;
            if(k==1) k=res_value_resolved(data,&raw,&unused);
            if(k==4) memcpy(&alpha,&raw,sizeof alpha);
            else if(k>=0x10 && k<=0x1f) alpha=(float)raw;
            else {good=false;break;}
         }
      }
      float modulated=(float)((uint32_t)color>>24)*alpha+.5f;
      int a=isnan(modulated) || modulated<=0 ? 0 : modulated>=255 ? 255 : (int)modulated;
      color=(int32_t)(((uint32_t)color&0xffffffu)|((uint32_t)a<<24));
      dvm_ref states=dvm_new_array(vm,'I',"I",tag.count);
      if(!states) {good=false;break;}
      ((dvm_ref *)dvm_array_data(vm,specs))[index]=states;
      int32_t *requirements=dvm_array_data(vm,states);unsigned nstates=0;
      for(uint16_t i=0;tag.asz>=20 && i<tag.count;++i) {
         size_t at=tag.attrs+(size_t)i*tag.asz;
         if(at+20>tag.end) {good=false;break;}
         uint32_t name=ax_u32(xml.b+at+4);
         const char *label=name<xml.ns ? xml.s[name] : NULL;
         uint32_t id=name<xml.resource_count ? ax_u32(xml.resource_map+name*4) : 0;
         if(!id || (label && (!strcmp(label,"color") || !strcmp(label,"alpha") || !strcmp(label,"lStar")))) continue;
         requirements[nstates++]=ax_u32(xml.b+at+16) ? (int32_t)id : -(int32_t)id;
      }
      dvm__obj(vm,states)->length=nstates;
      ((int32_t *)dvm_array_data(vm,colors))[index++]=color;
      dx_skip(&xml);
   }
   if(good && index==count) {
      union dvm_value args[]={{.l=specs},{.l=colors}},ignored={0};
      good=color_state_init(vm,result,args,2,&ignored);
      if(good && !count) (void)dvm_set_field(vm,result,"color","I",(union dvm_value){.u=0xffff0000u});
   } else good=false;
   if(good && pending) {
      dvm_ref source=dvm_new_string(vm,text);
      good=source && dvm_set_field(vm,result,"themeXml","Ljava/lang/String;",(union dvm_value){.l=source}) &&
         dvm_set_field(vm,result,"themePending","Z",(union dvm_value){.i=1});
   }
   for(uint32_t i=0;xml.s && i<xml.ns;++i) free(xml.s[i]);
   free(xml.s);free(bytes);
   if(result) dvm_unpin(vm,result);
   if(good) return result;
   if(!dvm_exception(vm)) dvm__throw(vm,"android/content/res/Resources$NotFoundException","Invalid color selector");
   return 0;
missing:
   dvm__throw(vm,"android/content/res/Resources$NotFoundException","Color resource could not be resolved");return 0;
invalid:
   dvm__throw(vm,"java/lang/UnsupportedOperationException","Can't convert type 0x%x to color",kind);return 0;
}

static bool gd_init(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);

static dvm_ref dx_shape(struct dvm *vm, struct dx *x, const struct dx_tag *t)
{
   dvm_ref g = dx_new(vm, "Landroid/graphics/drawable/GradientDrawable;");
   if (!g) { dx_skip(x); return 0; }
   dvm_pin(vm, g);
   union dvm_value ignored={0};
   if (!gd_init(vm,g,NULL,0,&ignored)) {dvm_unpin(vm,g); dx_skip(x); return 0;}
   union dvm_value v = { .i = dx_int(x, t, "shape", 0) };
   (void)dvm_set_field(vm, g, "shape", "I", v);
   struct dx_tag c;
   for (int e; (e = dx_next(x, &c)) != 0 && e != 0x0103;) {
      int32_t argb = 0;
      if (!strcmp(c.name, "solid")) {
         if (dx_color(vm, x, &c, "color", &argb)) {
            v.i = argb;
            (void)dvm_set_field(vm, g, "color", "I", v);
         }
      } else if (!strcmp(c.name, "gradient")) {
         int32_t cols[3], n = 0;
         if (dx_color(vm, x, &c, "startColor", &cols[n])) ++n; else cols[n++] = 0;
         if (dx_color(vm, x, &c, "centerColor", &cols[n])) ++n;
         if (dx_color(vm, x, &c, "endColor", &cols[n])) ++n; else cols[n++] = 0;
         dvm_ref colors = dvm_new_array(vm, 'I', NULL, (uint32_t)n);
         if (colors) {
            memcpy(dvm_array_data(vm, colors), cols, (size_t)n * sizeof cols[0]);
            v.l = colors;
            (void)dvm_set_field(vm, g, "colors", "[I", v);
         }
         /* GradientDrawable.Orientation is declared in 45-degree steps of the
          * XML `angle`, 0 meaning left to right. */
         static const char *const by_angle[8] = {
            "LEFT_RIGHT", "BL_TR", "BOTTOM_TOP", "BR_TL",
            "RIGHT_LEFT", "TR_BL", "TOP_BOTTOM", "TL_BR",
         };
         uint8_t at = 0; uint32_t ad = 0;
         float af = 0.f;
         if (dx_attr(x, &c, "angle", &at, &ad)) {
            if (at == 0x04) memcpy(&af, &ad, sizeof af); else af = (float)(int32_t)ad;
         }
         const int32_t angle = (int32_t)af;
         struct dvm_class *oc = dvm__class_by_desc(vm,
            "Landroid/graphics/drawable/GradientDrawable$Orientation;");
         union dvm_value o = { 0 };
         if (oc && dvm_get_static(vm, oc, by_angle[((angle / 45) % 8 + 8) % 8],
               "Landroid/graphics/drawable/GradientDrawable$Orientation;", &o) && o.l)
            (void)dvm_set_field(vm, g, "orientation",
               "Landroid/graphics/drawable/GradientDrawable$Orientation;", o);
      } else if (!strcmp(c.name, "corners")) {
         union dvm_value r = { .f = (float)dx_px(vm, x, &c, "radius", 0) };
         (void)dvm_set_field(vm, g, "cornerRadius", "F", r);
      } else if (!strcmp(c.name, "stroke")) {
         v.i = dx_px(vm, x, &c, "width", 0);
         (void)dvm_set_field(vm, g, "strokeWidth", "I", v);
         if (dx_color(vm, x, &c, "color", &argb)) {
            v.i = argb;
            (void)dvm_set_field(vm, g, "strokeColor", "I", v);
         }
      } else if (!strcmp(c.name, "size")) {
         v.i = dx_px(vm, x, &c, "width", -1);
         (void)dvm_set_field(vm, g, "width", "I", v);
         v.i = dx_px(vm, x, &c, "height", -1);
         (void)dvm_set_field(vm, g, "height", "I", v);
      }
      dx_skip(x);
   }
   dvm_unpin(vm, g);
   return g;
}

static float dx_float(const struct dx *x, const struct dx_tag *t, const char *name, float def)
{
   uint8_t type; uint32_t data;
   if (!dx_attr(x, t, name, &type, &data)) return def;
   if (type == 0x04) { float f; memcpy(&f, &data, sizeof f); return f; }
   if (type == 0x05) return (float)android_dimension_pixel_size((int32_t)data);
   return (float)(int32_t)data;
}

/* C VectorDrawable resource model. Group scopes retain transforms and clips. */
struct vector_gradient {
 int type,tile;float x0,y0,x1,y1,cx,cy,radius;
 uint32_t *colors;float *positions;size_t count,capacity;
};
static void vector_gradient_free(struct vector_gradient *g)
{if(g) {free(g->colors);free(g->positions);free(g);}}
static bool vector_gradient_stop(struct vector_gradient *g,uint32_t color,float position)
{
 if(g->count==g->capacity) {
  size_t cap=g->capacity ? g->capacity*2 : 4;
  uint32_t *colors=realloc(g->colors,cap*sizeof *colors);if(!colors) return false;g->colors=colors;
  float *positions=realloc(g->positions,cap*sizeof *positions);if(!positions) return false;g->positions=positions;g->capacity=cap;
 }
 g->colors[g->count]=color;g->positions[g->count++]=position;return true;
}
struct vector_node {
   struct vector_node *next,*children;
   luna_path path;
   struct vector_gradient *fill_gradient,*stroke_gradient;
   float matrix[9];
   uint32_t fill,stroke;
   float fill_alpha,stroke_alpha,stroke_width,miter,trim_start,trim_end,trim_offset;
   int kind,cap,join; /* group=0, path=1, clip=2 */
};
struct vector_data {
   struct vector_node root;
   float viewport_width,viewport_height,alpha;
   int width,height;
   bool mirrored;
   bool tint;
   uint32_t tint_color;
   int density;
};
static void vector_nodes_clear(struct vector_node *node)
{
   while(node) {struct vector_node *next=node->next;vector_nodes_clear(node->children);
      luna_path_clear(&node->path,true);vector_gradient_free(node->fill_gradient);vector_gradient_free(node->stroke_gradient);free(node);node=next;}
}
static void vector_destroy(void *data)
{
   struct vector_data *vector=data;if(!vector) return;
   vector_nodes_clear(vector->root.children);free(vector);
}
/* The VM reference owns native vector state and roots its tint references.
 * Drawable instances retain it through a normal scanned object field. */
static dvm_ref vector_state_ref(struct dvm *vm,dvm_ref drawable)
{ return ui_obj(vm,drawable,"vectorState","Landroid/graphics/drawable/VectorDrawable$VectorDrawableState;"); }
static struct vector_data *vector_of(struct dvm *vm,dvm_ref drawable)
{
   struct dvm_object *state=dvm__obj(vm,vector_state_ref(vm,drawable));
   return state ? state->data : NULL;
}
static bool vector_attach(struct dvm *vm,dvm_ref drawable,struct vector_data *data)
{
   dvm_pin(vm,drawable);
   dvm_ref state=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/drawable/VectorDrawable$VectorDrawableState"));
   if (!state) {dvm_unpin(vm,drawable);vector_destroy(data);return false;}
   struct dvm_object *object=dvm__obj(vm,state);
   object->data=data;object->data_destroy=vector_destroy;
   data->density=device_density_dpi();
   bool good=dvm_set_field(vm,drawable,"vectorState","Landroid/graphics/drawable/VectorDrawable$VectorDrawableState;",
                           (union dvm_value){.l=state});
   dvm_unpin(vm,drawable);return good;
}
static struct vector_gradient *vector_gradient_copy(const struct vector_gradient *source)
{
   if (!source) return NULL;
   struct vector_gradient *copy=calloc(1,sizeof *copy);
   if (!copy) return NULL;
   *copy=*source;copy->colors=NULL;copy->positions=NULL;copy->count=copy->capacity=0;
   for (size_t i=0;i<source->count;++i)
      if (!vector_gradient_stop(copy,source->colors[i],source->positions[i])) {
         vector_gradient_free(copy);return NULL;
      }
   return copy;
}
static bool vector_nodes_copy(const struct vector_node *source,struct vector_node **out)
{
   for (;source;source=source->next) {
      struct vector_node *copy=calloc(1,sizeof *copy);
      if (!copy) return false;
      *copy=*source;copy->next=copy->children=NULL;
      copy->path=(luna_path){0};copy->fill_gradient=copy->stroke_gradient=NULL;
      *out=copy;out=&copy->next;
      if (!luna_path_append(&copy->path,&source->path,NULL)) return false;
      copy->path.fill=source->path.fill;copy->path.after_close=source->path.after_close;
      if (source->fill_gradient && !(copy->fill_gradient=vector_gradient_copy(source->fill_gradient))) return false;
      if (source->stroke_gradient && !(copy->stroke_gradient=vector_gradient_copy(source->stroke_gradient))) return false;
      if (!vector_nodes_copy(source->children,&copy->children)) return false;
   }
   return true;
}
static struct vector_data *vector_copy(const struct vector_data *source)
{
   struct vector_data *copy=malloc(sizeof *copy);
   if (!copy) return NULL;
   *copy=*source;copy->root.children=NULL;
   if (!vector_nodes_copy(source->root.children,&copy->root.children)) {vector_destroy(copy);return NULL;}
   return copy;
}

static float dx_float(const struct dx *x,const struct dx_tag *t,const char *name,float def);
static struct vector_gradient *vector_gradient_resource(struct dvm *vm,uint32_t id,dvm_ref theme)
{
 int32_t value=0;const char *name=NULL;int kind=res_value_resolved(id,&value,&name);
 if(kind!='L' || !name) return NULL;
 size_t size=0;unsigned char *bytes=arm_exec_asset_read(name,&size);if(!bytes) return NULL;
 struct dx x={.b=bytes,.n=size,.pos=size>=8 ? ax_u16(bytes+2) : size,.theme=theme};
 x.s=size>=8 && ax_u16(bytes)==3 ? ax_string_pool(bytes,size,&x.ns) : NULL;
 struct dx_tag root;struct vector_gradient *g=NULL;
 if(x.s && dx_next(&x,&root)==0x0102 && !strcmp(root.name,"gradient")) {
  g=calloc(1,sizeof *g);
  if(g) {
   g->type=dx_int(&x,&root,"type",0);g->tile=dx_int(&x,&root,"tileMode",0);
   g->x0=dx_float(&x,&root,"startX",0);g->y0=dx_float(&x,&root,"startY",0);
   g->x1=dx_float(&x,&root,"endX",0);g->y1=dx_float(&x,&root,"endY",0);
   g->cx=dx_float(&x,&root,"centerX",0);g->cy=dx_float(&x,&root,"centerY",0);g->radius=dx_float(&x,&root,"gradientRadius",0);
   struct dx_tag item;bool good=g->type>=0 && g->type<=2 && g->tile>=0 && g->tile<=2 && (g->type!=1 || g->radius>0);
   for(int event;good && (event=dx_next(&x,&item)) && event!=0x0103;) {
    if(!strcmp(item.name,"item")) {
     uint8_t type;uint32_t data;int32_t color=0;
     good=dx_color(vm,&x,&item,"color",&color) && dx_attr(&x,&item,"offset",&type,&data) &&
       vector_gradient_stop(g,(uint32_t)color,dx_float(&x,&item,"offset",0));
    }
    dx_skip(&x);
   }
   if(good && !g->count) {
    int32_t start=0,end=0,center=0;
    (void)dx_color(vm,&x,&root,"startColor",&start);(void)dx_color(vm,&x,&root,"endColor",&end);
    good=vector_gradient_stop(g,(uint32_t)start,0);
    if(dx_color(vm,&x,&root,"centerColor",&center)) good=good && vector_gradient_stop(g,(uint32_t)center,.5f);
    good=good && vector_gradient_stop(g,(uint32_t)end,1);
   }
   if(!good || g->count<2) {vector_gradient_free(g);g=NULL;}
  }
 }
 for(uint32_t i=0;x.s && i<x.ns;++i) free(x.s[i]);free(x.s);free(bytes);
 return g;
}
static bool vector_parse_paint(struct dvm *vm,struct dx *x,const struct dx_tag *tag,const char *name,
                                 uint32_t *solid,struct vector_gradient **gradient)
{
 uint8_t type;uint32_t value;int32_t color=0;
 if(!dx_attr(x,tag,name,&type,&value)) return true;
 if(dx_color(vm,x,tag,name,&color)) {*solid=(uint32_t)color;return true;}
 if(type==AX_REFERENCE) *gradient=vector_gradient_resource(vm,value,x->theme);
 if(*gradient) return true;
 dvm__throw(vm,"java/lang/UnsupportedOperationException","Unresolved vector %s (type=%x value=%08x)",name,type,value);
 return false;
}
static bool vector_parse_children(struct dvm *vm,struct dx *x,struct vector_node *parent,unsigned depth)
{
   if(depth>64) {dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","Vector group nesting");return false;}
   struct vector_node **tail=&parent->children;struct dx_tag tag;
   for(int event;(event=dx_next(x,&tag)) && event!=0x0103;) {
      int kind=!strcmp(tag.name,"group") ? 0 : !strcmp(tag.name,"path") ? 1 : !strcmp(tag.name,"clip-path") ? 2 : -1;
      if(kind<0) {dx_skip(x);continue;}
      struct vector_node *node=calloc(1,sizeof *node);
      if(!node) {dvm__throw(vm,"java/lang/OutOfMemoryError","Vector nodes");return false;}
      *tail=node;tail=&node->next;node->kind=kind;
      node->matrix[0]=node->matrix[4]=node->matrix[8]=1;
      if(!kind) {
         float px=dx_float(x,&tag,"pivotX",0),py=dx_float(x,&tag,"pivotY",0);
         float sx=dx_float(x,&tag,"scaleX",1),sy=dx_float(x,&tag,"scaleY",1);
         float angle=dx_float(x,&tag,"rotation",0)*(float)(M_PI/180),c=cosf(angle),s=sinf(angle);
         node->matrix[0]=c*sx;node->matrix[1]=-s*sy;node->matrix[3]=s*sx;node->matrix[4]=c*sy;
         node->matrix[2]=dx_float(x,&tag,"translateX",0)+px-node->matrix[0]*px-node->matrix[1]*py;
         node->matrix[5]=dx_float(x,&tag,"translateY",0)+py-node->matrix[3]*px-node->matrix[4]*py;
         if(!vector_parse_children(vm,x,node,depth+1)) return false;
         continue;
      }
      uint8_t type;uint32_t value;
      if(!dx_attr(x,&tag,"pathData",&type,&value) || type!=AX_STRING || value>=x->ns || !x->s[value]) {
         dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","Vector pathData is required");return false;
      }
      if(!luna_path_parse(&node->path,x->s[value])) {
         dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","Invalid vector pathData");return false;
      }
      node->path.fill=dx_int(x,&tag,"fillType",0)&1;
      if(!vector_parse_paint(vm,x,&tag,"fillColor",&node->fill,&node->fill_gradient) ||
         !vector_parse_paint(vm,x,&tag,"strokeColor",&node->stroke,&node->stroke_gradient)) return false;
      node->fill_alpha=dx_float(x,&tag,"fillAlpha",1);node->stroke_alpha=dx_float(x,&tag,"strokeAlpha",1);
      node->stroke_width=dx_float(x,&tag,"strokeWidth",0);node->miter=dx_float(x,&tag,"strokeMiterLimit",4);
      node->cap=dx_int(x,&tag,"strokeLineCap",0);node->join=dx_int(x,&tag,"strokeLineJoin",0);
      node->trim_start=dx_float(x,&tag,"trimPathStart",0);node->trim_end=dx_float(x,&tag,"trimPathEnd",1);
      node->trim_offset=dx_float(x,&tag,"trimPathOffset",0);
      dx_skip(x);
   }
   return true;
}
static dvm_ref dx_vector(struct dvm *vm,struct dx *x,const struct dx_tag *tag)
{
   dvm_ref drawable=dx_new(vm,"Landroid/graphics/drawable/VectorDrawable;");
   if(!drawable) return 0;
   struct vector_data *vector=calloc(1,sizeof *vector);
   if(!vector) {dvm__throw(vm,"java/lang/OutOfMemoryError","Vector");return 0;}
   if (!vector_attach(vm,drawable,vector)) return 0;
   vector->width=dx_px(vm,x,tag,"width",0);vector->height=dx_px(vm,x,tag,"height",0);
   vector->viewport_width=dx_float(x,tag,"viewportWidth",0);vector->viewport_height=dx_float(x,tag,"viewportHeight",0);
   vector->alpha=dx_float(x,tag,"alpha",1);vector->mirrored=dx_int(x,tag,"autoMirrored",0)!=0;
   if(vector->width<=0 || vector->height<=0 || !(vector->viewport_width>0) || !(vector->viewport_height>0)) {
      dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","Vector dimensions and viewport must be positive");return 0;
   }
   int32_t tint=0;vector->tint=dx_color(vm,x,tag,"tint",&tint);vector->tint_color=(uint32_t)tint;
   dvm_pin(vm,drawable);union dvm_value ignored={0};
   bool good=drawable_init(vm,drawable,NULL,0,&ignored) && vector_parse_children(vm,x,&vector->root,0);
   dvm_unpin(vm,drawable);return good ? drawable : 0;
}

/* Builds the drawable of the element whose start tag `t` was just read and
 * consumes the element, children and end tag included. */
static dvm_ref dx_build(struct dvm *vm, struct dx *x, const struct dx_tag *t,
                        unsigned depth)
{
   if (depth > 8) { dx_skip(x); return 0; }
   if (!strcmp(t->name,"vector")) return dx_vector(vm,x,t);
   if (!strcmp(t->name, "bitmap")) {
      /* The XML is a BitmapDrawable specification, not image bytes. Decode
       * its src with the resource decoder so source density is preserved. */
      uint8_t type = 0;
      uint32_t id = 0;
      if (!dx_attr(x, t, "src", &type, &id) || type != AX_REFERENCE || !id) {
         dvm__throw(vm, "org/xmlpull/v1/XmlPullParserException",
                     "<bitmap> requires a valid src attribute");
         dx_skip(x);
         return 0;
      }
      dvm_ref resources = dx_new(vm, "Landroid/content/res/Resources;");
      if (!resources) { dx_skip(x); return 0; }
      dvm_pin(vm, resources);
      union dvm_value a[] = {{.l = resources}, {.i = (int32_t)id}}, bitmap = {0};
      bool good = bf_decodeResource(vm, 0, a, 2, &bitmap);
      dvm_ref drawable = 0;
      if (good && bitmap.l) {
         dvm_pin(vm, bitmap.l);
         drawable = dx_new(vm, "Landroid/graphics/drawable/BitmapDrawable;");
         if (drawable) {
            dvm_pin(vm, drawable);
            union dvm_value ctor[] = {{.l = resources}, bitmap}, ignored = {0};
            good = am_vcall(vm, drawable, "<init>",
               "(Landroid/content/res/Resources;Landroid/graphics/Bitmap;)V",
               ctor, 2, &ignored);
            if (good) {
               union dvm_value gravity={.i=dx_int(x,t,"gravity",0x77)};
               good=am_vcall(vm,drawable,"setGravity","(I)V",&gravity,1,&ignored);
               const char *names[]={"antialias","filter","dither"};
               const char *setters[]={"setAntiAlias","setFilterBitmap","setDither"};
               for (int i=0;good && i<3;++i) {
                  uint8_t attr_type; uint32_t data;
                  if (dx_attr(x,t,names[i],&attr_type,&data)) {
                     union dvm_value value={.i=data!=0};
                     good=am_vcall(vm,drawable,setters[i],"(Z)V",&value,1,&ignored);
                  }
               }
               int32_t both=dx_int(x,t,"tileMode",-1);
               (void)dvm_set_field(vm,drawable,"tileModeX","I",(union dvm_value){.i=dx_int(x,t,"tileModeX",both)});
               (void)dvm_set_field(vm,drawable,"tileModeY","I",(union dvm_value){.i=dx_int(x,t,"tileModeY",both)});
            }
            dvm_unpin(vm, drawable);
            if (!good) drawable = 0;
         }
         dvm_unpin(vm, bitmap.l);
      } else if (good) {
         dvm__throw(vm, "org/xmlpull/v1/XmlPullParserException",
                     "<bitmap> src 0x%08x cannot be decoded", id);
      }
      dvm_unpin(vm, resources);
      dx_skip(x);
      return drawable;
   }
   if (!strcmp(t->name, "shape")) return dx_shape(vm, x, t);
   if (!strcmp(t->name, "color")) {
      dvm_ref d = dx_new(vm, "Landroid/graphics/drawable/ColorDrawable;");
      int32_t argb = 0;
      if (d && dx_color(vm, x, t, "color", &argb)) {
         union dvm_value c = { .i = argb };
         union dvm_value ignored={0};
         (void)drawable_init(vm,d,NULL,0,&ignored);
         (void)color_drawable_init(vm,d,&c,1,&ignored);
      }
      dx_skip(x);
      return d;
   }
   const bool selector = !strcmp(t->name, "selector");
   const bool transition = !strcmp(t->name, "transition");
   const bool layers = transition || !strcmp(t->name, "layer-list") || !strcmp(t->name, "ripple");
   const bool animated_rotate=!strcmp(t->name,"animated-rotate");
   const bool inset = animated_rotate || !strcmp(t->name, "inset") || !strcmp(t->name, "rotate") ||
                      !strcmp(t->name, "scale") || !strcmp(t->name, "clip");
   if (selector || layers || inset) {
      const char *cls = selector ? "Landroid/graphics/drawable/StateListDrawable;"
         : transition ? "Landroid/graphics/drawable/TransitionDrawable;"
         : !strcmp(t->name, "ripple") ? "Landroid/graphics/drawable/RippleDrawable;"
         : layers ? "Landroid/graphics/drawable/LayerDrawable;"
         : animated_rotate ? "Landroid/graphics/drawable/AnimatedRotateDrawable;"
         : !strcmp(t->name,"rotate") ? "Landroid/graphics/drawable/RotateDrawable;"
         : !strcmp(t->name,"clip") ? "Landroid/graphics/drawable/ClipDrawable;"
         : "Landroid/graphics/drawable/InsetDrawable;";
      dvm_ref list = dx_new(vm, cls);
      if (!list) { dx_skip(x); return 0; }
      dvm_pin(vm, list);
      if (selector) {
         union dvm_value ignored={0};
         if (!sld_init(vm,list,NULL,0,&ignored)) {dvm_unpin(vm,list); return 0;}
      }
      if (layers) {
         if(transition) rt_set_int(vm,list,"transitionState",2);
         union dvm_value ignored={0},mode={.i=dx_int(x,t,"paddingMode",0)};
         (void)drawable_init(vm,list,NULL,0,&ignored);
         (void)dvm_set_field(vm,list,"paddingMode","I",mode);
      }
      dvm_ref shown = 0, first = 0;
      if (inset) {
         shown = dx_item_drawable(vm, x, t, depth);
         if(animated_rotate || !strcmp(t->name,"rotate")) {
            union dvm_value ignored={0};
            if(!shown || !(animated_rotate ? animated_rotate_init(vm,list,NULL,0,&ignored) : rotate_init(vm,list,NULL,0,&ignored))) {
               if(!dvm_exception(vm)) dvm__throw(vm,"org/xmlpull/v1/XmlPullParserException","<%s> requires a drawable",t->name);
               dvm_unpin(vm,list);return 0;
            }
            if(!animated_rotate) {
               rotate_set_float(vm,list,"fromDegrees",dx_float(x,t,"fromDegrees",0.0f));
               rotate_set_float(vm,list,"toDegrees",dx_float(x,t,"toDegrees",360.0f));
               rotate_set_float(vm,list,"currentDegrees",rotate_float(vm,list,"fromDegrees"));
            } else {
               rt_set_int(vm,list,"framesCount",dx_int(x,t,"framesCount",12));
               rt_set_int(vm,list,"frameDuration",dx_int(x,t,"frameDuration",150));
            }
            const char *pivots[]={"pivotX","pivotY"};
            const char *relative[]={"pivotXRelative","pivotYRelative"};
            for(int i=0;i<2;++i) {
               uint8_t type;uint32_t data;
               if(dx_attr(x,t,pivots[i],&type,&data)) {
                  union dvm_value value={.i=(int32_t)data},decoded={0};
                  if(type==AX_FRACTION) (void)typed_value_complexToFloat(vm,0,&value,1,&decoded);
                  else decoded.f=dx_float(x,t,pivots[i],0.5f);
                  rotate_set_float(vm,list,pivots[i],decoded.f);
                  (void)dvm_set_field(vm,list,relative[i],"Z",(union dvm_value){.i=type==AX_FRACTION});
               }
            }
            union dvm_value child={.l=shown};
            if(!wrapper_set_drawable(vm,list,&child,1,&ignored)) {dvm_unpin(vm,list);return 0;}
            if(animated_rotate) {
               union dvm_value yes={.i=1};
               if(!am_vcall(vm,shown,"setFilterBitmap","(Z)V",&yes,1,&ignored) ||
                  (ui_is_a(vm,shown,"android/graphics/drawable/BitmapDrawable") &&
                   !am_vcall(vm,shown,"setAntiAlias","(Z)V",&yes,1,&ignored))) {dvm_unpin(vm,list);return 0;}
            }
         }
         if(!strcmp(t->name,"clip") && shown) {
            union dvm_value input[3]={{.l=shown},{.i=dx_int(x,t,"gravity",3)},{.i=dx_int(x,t,"clipOrientation",1)}},ignored={0};
            if(!clip_drawable_init(vm,list,input,3,&ignored)) {dvm_unpin(vm,list);return 0;}
         }
      } else {
         struct dx_tag c;
         for (int e; (e = dx_next(x, &c)) != 0 && e != 0x0103;) {
            if (strcmp(c.name, "item")) { dx_skip(x); continue; }
            /* The mask of a ripple clips its effect; it is not the content. */
            const bool mask = !strcmp(t->name,"ripple") && dx_int(x, &c, "id", 0) == 0x0102002e;
            int32_t meta[10]={dx_int(x,&c,"id",-1),dx_px(vm,x,&c,"left",0),
               dx_px(vm,x,&c,"top",0),dx_px(vm,x,&c,"right",0),dx_px(vm,x,&c,"bottom",0),
               dx_px(vm,x,&c,"start",INT32_MIN),dx_px(vm,x,&c,"end",INT32_MIN),
               dx_px(vm,x,&c,"width",-1),dx_px(vm,x,&c,"height",-1),dx_int(x,&c,"gravity",0)};
            dvm_ref states=selector ? dx_item_states(vm,x,&c) : 0;
            if (selector && !states) {dvm_unpin(vm,list); return 0;}
            if (states) dvm_pin(vm,states);
            dvm_ref d = dx_item_drawable(vm, x, &c, depth);
            if (dvm_exception(vm)) {
               if (states) dvm_unpin(vm,states);
               dvm_unpin(vm,list); return 0;
            }
            if (selector) {
               union dvm_value pair[2]={{.l=states},{.l=d}},ignored={0};
               bool good=sld_addState(vm,list,pair,2,&ignored);
               dvm_unpin(vm,states);
               if (!good) {dvm_unpin(vm,list); return 0;}
               continue;
            }
            if (!d || mask) continue;
            dvm_pin(vm, d);
            if (layers && !layer_append(vm,list,d,meta)) {
               dvm_unpin(vm,d); dvm_unpin(vm,list); return 0;
            }
            if (!first) {
               first=d;
               (void)dvm_set_field(vm,list,"current","Landroid/graphics/drawable/Drawable;",(union dvm_value){.l=d});
            }
            /* The overlay's representative layer is the topmost child. */
            shown = d;
            (void)dvm_set_field(vm,list,"current","Landroid/graphics/drawable/Drawable;",(union dvm_value){.l=d});
            dvm_unpin(vm,d);
         }
      }
      if(transition) shown=first;
      if (!shown) shown = first;
      if (shown) {
         union dvm_value cur = { .l = shown };
         (void)dvm_set_field(vm, list, "current",
                             "Landroid/graphics/drawable/Drawable;", cur);
      }
      dvm_unpin(vm, list);
      return list;
   }
   dx_skip(x);
   return 0;
}

/* Resources.getDrawable() for an XML drawable: the parsed drawable, or 0 when
 * the resource is not one. */
static dvm_ref drawable_from_resource(struct dvm *vm, uint32_t id, unsigned depth, dvm_ref theme)
{
   int32_t iv = 0; const char *path = NULL;
   if (depth > 8 || !id || arm_exec_apk_resource_value(id, &iv, &path) != 'L' || !path)
      return 0;
   const size_t len = strlen(path);
   if (dvm__env("LUNARIA_TRACE_DRAWABLE"))
      fprintf(stderr,"[drawable] resource=%08x theme=%x depth=%u path=%s\n",id,theme,depth,path);
   if (len <= 4 || strcasecmp(path + len - 4, ".xml")) return 0;
   size_t n = 0;
   unsigned char *b = arm_exec_asset_read(path, &n);
   if (!b || n < 8 || ax_u16(b) != 0x0003) { free(b); return 0; }
   struct dx x = { .b = b, .n = n, .pos = ax_u16(b + 2), .theme = theme };
   x.s = ax_string_pool(b, n, &x.ns);
   dvm_ref d = 0;
   struct dx_tag root;
   if (x.s && dx_next(&x, &root) == 0x0102) d = dx_build(vm, &x, &root, depth);
   for (uint32_t i = 0; x.s && i < x.ns; ++i) free(x.s[i]);
   free(x.s);
   free(b);
   return d;
}

/* Text is either literal or an @string reference. */
static dvm_ref ax_string(struct dvm *vm, uint8_t type, uint32_t data,
                         char *const *strings, uint32_t nstrings)
{
   if (type == AX_STRING && data < nstrings && strings[data])
      return dvm_new_string(vm, strings[data]);
   if (type == AX_REFERENCE && data) {
      int32_t value = 0; const char *sv = NULL;
      if (arm_exec_apk_resource_value(data, &value, &sv) == 'L' && sv)
         return dvm_new_string(vm, sv);
   }
   return 0;
}

/* Same two widths as ui_int(): `visibility` is an int and `disabled` is a
 * boolean, and the inflater has no reason to know which is which. */
static void ax_set_int(struct dvm *vm, dvm_ref view, const char *field,
                       int32_t value)
{
   union dvm_value v = { .i = value };
   if (!dvm_set_field(vm, view, field, "I", v))
      (void)dvm_set_field(vm, view, field, "Z", v);
}

/* Resolve XML inset shorthands after ordinary attributes. Android gives
 * uniform values precedence over axes, and axes over physical edges,
 * independently of the binary XML attribute order. */
static void ax_apply_inset_shorthands(struct dvm *vm,dvm_ref view,const uint8_t *b,
 size_t attrs,size_t end,uint16_t attr_size,uint16_t attr_count,
 char *const *strings,uint32_t nstrings,bool margins_only)
{
   static const char *const names[]={"padding","paddingHorizontal","paddingVertical",
      "layout_margin","layout_marginHorizontal","layout_marginVertical","paddingStart","paddingEnd"};
   int32_t values[8]={-1,-1,-1,-1,-1,-1,-1,-1};
   union dvm_value context={0};
   dvm_get_field(vm,view,"context","Landroid/content/Context;",&context);
   for(uint16_t i=0;attr_size>=20 && i<attr_count;++i) {
      size_t q=attrs+(size_t)i*attr_size;
      if(q>end || end-q<20) break;
      uint32_t index=ax_u32(b+q+4);
      const char *name=index<nstrings ? strings[index] : NULL;
      if(!name) continue;
      for(size_t j=0;j<8;++j) if(!strcmp(name,names[j])) {
         values[j]=ax_dimension_px(vm,context.l,b[q+15],ax_u32(b+q+16));break;
      }
   }
   static const char *const fields[]={"paddingLeft","paddingTop","paddingRight","paddingBottom",
      "marginLeft","marginTop","marginRight","marginBottom"};
   for(int group=margins_only ? 1 : 0;group<2;++group) {
      for(int edge=0;edge<4;++edge) {
         int base=group*3;
         int32_t value=values[base]>=0 ? values[base] : values[base+1+(edge&1)];
         if(value>=0) ax_set_int(vm,view,fields[group*4+edge],value);
      }
   }
   /* Preserve relative-edge precedence, as with the existing inflater's
    * start/end handling. Full inherited RTL resolution belongs to View. */
   if(!margins_only) {
      if(values[6]>=0) ax_set_int(vm,view,"paddingLeft",values[6]);
      if(values[7]>=0) ax_set_int(vm,view,"paddingRight",values[7]);
   }
}

/* android:gravity and android:layout_gravity are bit masks, not enums; the
 * constants are the platform's. */
/* View.DeclaredOnClickListener resolves a public (View) handler against the
 * view's Context and its ContextWrapper chain on the first click. Inflating
 * a view does not require its handler to exist yet. */
static bool declared_click_on_click(struct dvm *vm,dvm_ref self,
                                    const union dvm_value *args,int nargs,
                                    union dvm_value *out)
{
   (void)args;(void)nargs;(void)out;
   dvm_ref view=ui_obj(vm,self,"hostView","Landroid/view/View;");
   dvm_ref named=ui_obj(vm,self,"methodName","Ljava/lang/String;");
   const char *name=dvm_string_utf8(vm,named);
   dvm_ref context=ui_obj(vm,self,"resolvedContext","Landroid/content/Context;");
   if(!context)context=ui_obj(vm,view,"context","Landroid/content/Context;");
   for(unsigned depth=0;context && depth<64;depth++) {
      union dvm_value restricted={0};
      if(!am_vcall(vm,context,"isRestricted","()Z",NULL,0,&restricted))return false;
      struct dvm_class *cls=dvm_object_class(vm,context);
      struct dvm_method *method=name && cls
         ? dvm_find_method(vm,cls,name,"(Landroid/view/View;)V") : NULL;
      if(!restricted.i && method && (method->access&DEX_ACC_PUBLIC)) {
         (void)dvm_set_field(vm,self,"resolvedContext","Landroid/content/Context;",
                            (union dvm_value){.l=context});
         union dvm_value argument={.l=view},ignored={0};
         return dvm_call(vm,method,(method->access&DEX_ACC_STATIC) ? 0 : context,
                         &argument,1,&ignored);
      }
      if(!ui_is_a(vm,context,"android/content/ContextWrapper"))break;
      union dvm_value base={0};
      if(!am_vcall(vm,context,"getBaseContext","()Landroid/content/Context;",NULL,0,&base))return false;
      if(base.l==context)break;
      context=base.l;
   }
   dvm__throw(vm,"java/lang/IllegalStateException",
              "Could not find public method %s(View) in a parent Context for android:onClick",
              name ? name : "");
   return false;
}
static const struct rt_field rt_declared_click_fields[]={
   {"hostView","Landroid/view/View;"}, {"methodName","Ljava/lang/String;"},
   {"resolvedContext","Landroid/content/Context;"}, F_END
};
static const struct rt_method rt_declared_click[]={
   M("onClick","(Landroid/view/View;)V",declared_click_on_click),M_END
};
static const char *const rt_declared_click_ifaces[]={"Landroid/view/View$OnClickListener;",NULL};

static void ax_apply_attribute(struct dvm *vm, dvm_ref view, const char *name,
                               uint8_t type, uint32_t data,
                               char *const *strings, uint32_t nstrings)
{
   if (!view || !name) return;
   union dvm_value context = {0};
   (void)dvm_get_field(vm, view, "context", "Landroid/content/Context;", &context);

   if (!strcmp(name, "minWidth"))
      { ax_set_int(vm, view, "minimumWidth", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "minHeight"))
      { ax_set_int(vm, view, "minimumHeight", ax_dimension_px(vm, context.l, type, data)); return; }

   /* Geometry the parent uses to place this child. */
   if (!strcmp(name, "layout_width"))
      { ax_set_int(vm, view, "layoutWidth", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "layout_height"))
      { ax_set_int(vm, view, "layoutHeight", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "layout_weight")) {
      float f = 0.0f;
      if (type == AX_FLOAT) { uint32_t u = data; memcpy(&f, &u, sizeof f); }
      else f = (float)(int32_t)data;
      union dvm_value v = { .f = f };
      (void)dvm_set_field(vm, view, "layoutWeight", "F", v);
      return;
   }
   if (!strcmp(name, "layout_margin")) {
      int32_t m = ax_dimension_px(vm, context.l, type, data);
      ax_set_int(vm, view, "marginLeft", m);
      ax_set_int(vm, view, "marginTop", m);
      ax_set_int(vm, view, "marginRight", m);
      ax_set_int(vm, view, "marginBottom", m);
      return;
   }
   if (!strcmp(name, "layout_marginLeft") || !strcmp(name, "layout_marginStart"))
      { ax_set_int(vm, view, "marginLeft", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "layout_marginTop"))
      { ax_set_int(vm, view, "marginTop", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "layout_marginRight") || !strcmp(name, "layout_marginEnd"))
      { ax_set_int(vm, view, "marginRight", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "layout_marginBottom"))
      { ax_set_int(vm, view, "marginBottom", ax_dimension_px(vm, context.l, type, data)); return; }

   if (!strcmp(name, "padding")) {
      int32_t m = ax_dimension_px(vm, context.l, type, data);
      ax_set_int(vm, view, "paddingLeft", m);
      ax_set_int(vm, view, "paddingTop", m);
      ax_set_int(vm, view, "paddingRight", m);
      ax_set_int(vm, view, "paddingBottom", m);
      return;
   }
   if (!strcmp(name, "paddingLeft") || !strcmp(name, "paddingStart"))
      { ax_set_int(vm, view, "paddingLeft", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "paddingTop"))
      { ax_set_int(vm, view, "paddingTop", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "paddingRight") || !strcmp(name, "paddingEnd"))
      { ax_set_int(vm, view, "paddingRight", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "paddingBottom"))
      { ax_set_int(vm, view, "paddingBottom", ax_dimension_px(vm, context.l, type, data)); return; }

   if (!strcmp(name, "fillViewport"))
      { ax_set_int(vm, view, "fillViewport", data ? 1 : 0); return; }
   if (!strcmp(name, "orientation"))
      { ax_set_int(vm, view, "orientation", (int32_t)data); return; }
   if (!strcmp(name, "gravity"))
      { ax_set_int(vm, view, "gravity", (int32_t)data); return; }
   if (!strcmp(name, "layout_gravity"))
      { ax_set_int(vm, view, "layoutGravity", (int32_t)data); return; }
   if (!strcmp(name,"visibility") && ui_is_a(vm,view,"android/view/ViewStub")) return;
   if (!strcmp(name, "visibility"))
      { if (data <= 2) ax_set_int(vm, view, "visibility", (int32_t)data * 4); return; }
   /* android:tag is View.setTag(Object): a String.  Data binding finds the
    * binding class of a layout by the "layout/<name>_<n>" tag on its root. */
   if (!strcmp(name, "tag")) {
      dvm_ref text = ax_string(vm, type, data, strings, nstrings);
      if (text)
         (void)dvm_set_field(vm, view, "tag", "Ljava/lang/Object;", (union dvm_value){ .l = text });
      return;
   }
   if (!strcmp(name,"onClick")) {
      dvm_ref method_name=ax_string(vm,type,data,strings,nstrings);
      if(!method_name)return;
      union dvm_value restricted={0};
      if(context.l && !am_vcall(vm,context.l,"isRestricted","()Z",NULL,0,&restricted))return;
      if(restricted.i) {
         dvm__throw(vm,"java/lang/IllegalStateException",
                    "android:onClick cannot be used within a restricted context");
         return;
      }
      dvm_ref listener=dvm_new_object(vm,dvm_find_class(vm,"android/view/View$DeclaredOnClickListener"));
      if(!listener)return;
      (void)dvm_set_field(vm,listener,"hostView","Landroid/view/View;",(union dvm_value){.l=view});
      (void)dvm_set_field(vm,listener,"methodName","Ljava/lang/String;",(union dvm_value){.l=method_name});
      union dvm_value argument={.l=listener},ignored={0};
      (void)am_vcall(vm,view,"setOnClickListener","(Landroid/view/View$OnClickListener;)V",&argument,1,&ignored);
      return;
   }
   if (!strcmp(name, "enabled"))
      { ax_set_int(vm, view, "disabled", data ? 0 : 1); return; }
   if (!strcmp(name, "layout_centerInParent"))
      { ax_set_int(vm, view, "centerInParent", data ? 1 : 0); return; }
   if (!strcmp(name, "layout_centerHorizontal"))
      { ax_set_int(vm, view, "centerHorizontal", data ? 1 : 0); return; }
   if (!strcmp(name, "layout_centerVertical"))
      { ax_set_int(vm, view, "centerVertical", data ? 1 : 0); return; }
   if (!strcmp(name, "layout_below"))
      { ax_set_int(vm, view, "ruleBelow", (int32_t)data); return; }
   if (!strcmp(name, "layout_above"))
      { ax_set_int(vm, view, "ruleAbove", (int32_t)data); return; }
   if (!strcmp(name, "layout_toRightOf") || !strcmp(name, "layout_toEndOf"))
      { ax_set_int(vm, view, "ruleRightOf", (int32_t)data); return; }
   if (!strcmp(name, "layout_toLeftOf") || !strcmp(name, "layout_toStartOf"))
      { ax_set_int(vm, view, "ruleLeftOf", (int32_t)data); return; }
   if (!strcmp(name, "layout_alignParentBottom"))
      { ax_set_int(vm, view, "alignParentBottom", data ? 1 : 0); return; }
   if (!strcmp(name, "layout_alignParentTop"))
      { ax_set_int(vm, view, "alignParentTop", data ? 1 : 0); return; }
   if (!strcmp(name, "layout_alignParentRight") ||
       !strcmp(name, "layout_alignParentEnd"))
      { ax_set_int(vm, view, "alignParentRight", data ? 1 : 0); return; }
   if (!strcmp(name, "layout_alignParentLeft") ||
       !strcmp(name, "layout_alignParentStart"))
      { ax_set_int(vm, view, "alignParentLeft", data ? 1 : 0); return; }
   if (!strcmp(name, "layout_alignTop"))
      { ax_set_int(vm, view, "alignTop", (int32_t)data); return; }
   if (!strcmp(name, "layout_alignBottom"))
      { ax_set_int(vm, view, "alignBottom", (int32_t)data); return; }
   if (!strcmp(name, "layout_alignLeft") || !strcmp(name, "layout_alignStart"))
      { ax_set_int(vm, view, "alignLeft", (int32_t)data); return; }
   if (!strcmp(name, "layout_alignRight") || !strcmp(name, "layout_alignEnd"))
      { ax_set_int(vm, view, "alignRight", (int32_t)data); return; }

   /* Backgrounds arrive as a colour, a drawable reference, or a state list.
    * Only a resolved colour can be painted; the reference is kept either way
    * so a widget that reads it back sees what the layout said. */
   if (!strcmp(name, "background")) {
      int32_t argb = 0;
      if (ax_color(vm, type, data, &argb))
         ax_set_int(vm, view, "backgroundColor", argb);
      if (type == AX_REFERENCE) {
         ax_set_int(vm, view, "backgroundResource", (int32_t)data);
         /* A drawable XML (<shape>, <selector>, ...) is built, as the platform
          * does when it resolves the attribute to a Drawable. */
         union dvm_value theme = {0};
         if (!view_drawable_theme(vm, view, &theme)) return;
         dvm_ref d = argb ? 0 : drawable_from_resource(vm, data, 0, theme.l);
         if (d) {
            union dvm_value bg = { .l = d };
            (void)dvm_set_field(vm, view, "background",
                                "Landroid/graphics/drawable/Drawable;", bg);
         }
      }
      return;
   }
   /* android:drawableStart & co. name a drawable resource (the terms dialog's
    * check box is one: button=@null, the picture comes from here).  Start/end
    * are kept beside left/right; text_on_draw reads whichever is set. */
   if (!strncmp(name, "drawable", 8) && strcmp(name, "drawablePadding") &&
       (!strcmp(name + 8, "Left") || !strcmp(name + 8, "Right") || !strcmp(name + 8, "Top") ||
        !strcmp(name + 8, "Bottom") || !strcmp(name + 8, "Start") || !strcmp(name + 8, "End"))) {
      if (type != AX_REFERENCE) return;
      union dvm_value theme = {0};
      if (!view_drawable_theme(vm, view, &theme)) return;
      dvm_ref d = drawable_from_resource(vm, data, 0, theme.l);
      if (dvm_exception(vm)) return;
      (void)dvm_set_field(vm, view, name, "Landroid/graphics/drawable/Drawable;", (union dvm_value){ .l = d });
      return;
   }
   if (!strcmp(name, "drawablePadding"))
      { ax_set_int(vm, view, "drawablePadding", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "src") || !strcmp(name, "srcCompat"))
      { ax_set_int(vm, view, "imageResource", (int32_t)data); return; }
   if (!strcmp(name, "cropToPadding")) {
      (void)dvm_set_field(vm,view,"cropToPadding","Z",(union dvm_value){.i=data!=0}); return;
   }

   if (!strcmp(name, "text")) {
      union dvm_value v = { .l = ax_string(vm, type, data, strings, nstrings) };
      if (v.l) (void)dvm_set_field(vm, view, "text",
                                   "Ljava/lang/CharSequence;", v);
      return;
   }
   if (!strcmp(name, "hint")) {
      union dvm_value v = { .l = ax_string(vm, type, data, strings, nstrings) };
      if (v.l) (void)dvm_set_field(vm, view, "hint",
                                   "Ljava/lang/CharSequence;", v);
      return;
   }
   if (!strcmp(name, "textSize")) {
      text_store_size(vm, view, (float)ax_dimension_px(vm, context.l, type, data));
      return;
   }
   if (!strcmp(name, "textColor")) {
      int32_t argb = 0;
      if (ax_color(vm, type, data, &argb))
         ax_set_int(vm, view, "textColor", argb);
      return;
   }
   if (!strcmp(name, "textStyle"))
      { ax_set_int(vm, view, "textStyle", (int32_t)data); return; }
   if (!strcmp(name, "maxLines") || !strcmp(name, "lines"))
      { ax_set_int(vm, view, "maxLines", (int32_t)data); return; }
   /* An EditText's editor: the keyboard it asks for, its action key, and
    * whether it is one line.  Without them every field is plain text. */
   if (!strcmp(name, "inputType"))
      { ax_set_int(vm, view, "inputType", (int32_t)data); return; }
   if (!strcmp(name, "imeOptions"))
      { ax_set_int(vm, view, "imeOptions", (int32_t)data); return; }
   if (!strcmp(name, "singleLine")) {
      (void)dvm_set_field(vm, view, "singleLine", "Z", (union dvm_value){ .i = data != 0 });
      return;
   }

   /* app: namespace attributes the SDK's own layouts use. */
   if (!strcmp(name, "cardCornerRadius") || !strcmp(name, "radius"))
      { ax_set_int(vm, view, "cornerRadius", ax_dimension_px(vm, context.l, type, data)); return; }
   if (!strcmp(name, "cardBackgroundColor")) {
      int32_t argb = 0;
      if (ax_color(vm, type, data, &argb))
         ax_set_int(vm, view, "backgroundColor", argb);
      return;
   }
}

/* LayoutInflater.inflate(resource, root, attachToRoot): the root element gets
 * its LayoutParams from root.generateLayoutParams(its own attributes) whenever
 * a root is given, and joins it only when attachToRoot.  An <include> or a
 * <merge> is inflated against the group it is included in the same way. */
static dvm_ref li_create_tag(struct dvm *vm,dvm_ref inflater,dvm_ref parent,
                              const char *tag,dvm_ref context,dvm_ref attrs)
{
   union dvm_value args[4]={{.l=parent},{.l=dvm_new_string(vm,tag ? tag : "")},
      {.l=context},{.l=attrs}},created={0},factory={0},factory2={0};
   if (tag && !strcmp(tag,"view")) {
      union dvm_value name[]={{.l=0},{.l=dvm_new_string(vm,"class")}};
      if (!am_vcall(vm,attrs,"getAttributeValue","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",name,2,&args[1])) return 0;
   }
   (void)dvm_get_field(vm,inflater,"mFactory","Landroid/view/LayoutInflater$Factory;",&factory);
   (void)dvm_get_field(vm,inflater,"mFactory2","Landroid/view/LayoutInflater$Factory2;",&factory2);
   if (!li_factory_call(vm,factory2.l ? factory2.l : factory.l,!!factory2.l,args,&created)) return 0;
   if (!created.l) {
      (void)dvm_get_field(vm,inflater,"mPrivateFactory","Landroid/view/LayoutInflater$Factory2;",&factory2);
      if (!li_factory_call(vm,factory2.l,true,args,&created)) return 0;
   }
   if (created.l) {
      if (!dvm__class_assignable(vm,dvm_object_class(vm,created.l),dvm_find_class(vm,"android/view/View"))) {
         dvm__throw(vm,"java/lang/ClassCastException","Inflater factory returned a non-View"); return 0;
      }
      return created.l;
   }
   const char *name=args[1].l ? dvm_string_utf8(vm,args[1].l) : NULL;
   return li_make_view(vm,name,context,attrs);
}

static dvm_ref li_inflate_resource(struct dvm *vm, dvm_ref inflater,dvm_ref context,
                                   uint32_t resource, unsigned depth,
                                   dvm_ref root_parent, bool attach_root)
{
   if (depth > 16 || !resource) return 0;
   int32_t iv = 0; const char *path = NULL;
   if (arm_exec_apk_resource_value(resource, &iv, &path) != 'L' || !path)
      return 0;
   if (dvm__env("LUNARIA_DVM_TRACE_LAYOUT"))
      fprintf(stderr, "[dvm] layout 0x%08x -> %s\n", resource, path);
   size_t n = 0;
   unsigned char *b = arm_exec_asset_read(path, &n);
   if (!b || n < 8 || ax_u16(b) != 0x0003) { free(b); return 0; }
   uint32_t nstrings = 0;
   char **strings = ax_string_pool(b, n, &nstrings);
   if (!strings) { free(b); return 0; }
   dvm_ref stack[64] = { 0 }, root = 0;
   bool stack_view[64] = { false };   /* a view element (not <include>/<merge>) */
   struct dvm_class *parser_class = dvm__class_by_desc(vm, "Landroid/content/res/XmlResourceParser;");
   dvm_ref parser = parser_class ? dvm_new_object(vm, parser_class) : 0;
   dvm_pin(vm,parser);
   union dvm_value resource_value = { .i = (int32_t)resource };
   (void)dvm_set_field(vm, parser, "resourceId", "I", resource_value);
   int32_t event_index = 0;
   unsigned sp = 0;
   size_t pos = ax_u16(b + 2);
   while (pos + 8 <= n && !dvm_exception(vm)) {
      uint16_t type = ax_u16(b + pos), hs = ax_u16(b + pos + 2);
      uint32_t size = ax_u32(b + pos + 4);
      if (size < hs || pos + size > n) break;
      if (type == 0x0102 && pos + 36 <= n) {
         uint32_t name_i = ax_u32(b + pos + 20);
         const char *tag = name_i < nstrings ? strings[name_i] : NULL;
         uint16_t attr_start = ax_u16(b + pos + 24);
         uint16_t attr_size = ax_u16(b + pos + 26);
         uint16_t attr_count = ax_u16(b + pos + 28);
         uint32_t id = 0, include = 0;
         size_t attrs = pos + 16u + attr_start;
         for (uint16_t i = 0; attr_size >= 20 && i < attr_count; ++i) {
            size_t q = attrs + (size_t)i * attr_size;
            if (q + 20 > pos + size) break;
            uint32_t ai = ax_u32(b + q + 4);
            const char *an = ai < nstrings ? strings[ai] : NULL;
            uint8_t value_type = b[q + 15];
            uint32_t data = ax_u32(b + q + 16);
            if (an && !strcmp(an, "id") && value_type == 0x01) id = data;
            if (an && !strcmp(an, "layout") && value_type == 0x01)
               include = data;
         }
         if (dvm__env("LUNARIA_DVM_TRACE_LAYOUT"))
            fprintf(stderr, "[dvm] layout tag depth=%u %s id=0x%08x include=0x%08x\n",
                    sp, tag ? tag : "?", id, include);
         dvm_ref parent = sp ? stack[sp - 1] : root_parent;
         const bool add_to_parent = sp ? true : attach_root;
         union dvm_value cursor = { .i = event_index }, event = { 0 };
         (void)dvm_set_field(vm, parser, "eventIndex", "I", cursor);
         (void)xml_parser_next(vm, parser, NULL, 0, &event);
         if (tag && !strcmp(tag, "include")) {
            /* The included layout is inflated into this group, so its root gets
             * its LayoutParams from the group (parseInclude()). */
            dvm_ref child = li_inflate_resource(vm,inflater,context,include,depth+1,
                                                parent, add_to_parent);
            dvm_pin(vm,child);
            bool has_width = false, has_height = false;
            for (uint16_t i = 0; attr_size >= 20 && i < attr_count; ++i) {
               size_t q = attrs + (size_t)i * attr_size;
               if (q + 20 > pos + size) break;
               uint32_t ai = ax_u32(b + q + 4);
               const char *an = ai < nstrings ? strings[ai] : NULL;
               if (an && !strcmp(an, "layout_width")) has_width = true;
               if (an && !strcmp(an, "layout_height")) has_height = true;
            }
            /* Include overrides only id, visibility and the parent's layout
             * parameters. Other View attributes belong to the included XML.
             * Framework parents require both dimensions; otherwise keep the
             * included root's parameters. */
            if (parent && child && has_width && has_height) {
               struct dvm_class *pc = dvm_object_class(vm, parent);
               struct dvm_method *generate = pc ? dvm_find_method(vm, pc,
                  "generateLayoutParams", "(Landroid/util/AttributeSet;)Landroid/view/ViewGroup$LayoutParams;") : NULL;
               if (generate) {
                  union dvm_value attr = { .l = parser }, params = { 0 };
                  if (dvm_call(vm, generate, parent, &attr, 1, &params))
                     (void)dvm_set_field(vm, child, "layoutParams", "Landroid/view/ViewGroup$LayoutParams;", params);
               }
            }
            for (uint16_t i = 0; child && attr_size >= 20 && i < attr_count; ++i) {
               size_t q = attrs + (size_t)i * attr_size;
               if (q + 20 > pos + size) break;
               uint32_t ai = ax_u32(b + q + 4);
               const char *an = ai < nstrings ? strings[ai] : NULL;
               if (an && (!strcmp(an, "visibility") ||
                   (has_width && has_height && !strncmp(an, "layout_", 7))))
                  ax_apply_attribute(vm, child, an, b[q + 15], ax_u32(b + q + 16), strings, nstrings);
            }
            if(child && has_width && has_height)
               ax_apply_inset_shorthands(vm,child,b,attrs,pos+size,attr_size,attr_count,strings,nstrings,true);
            if (child && id) {
               union dvm_value v = { .i = (int32_t)id };
               (void)dvm_set_field(vm, child, "id", "I", v);
            }
            if (sp < 64) { stack_view[sp] = false; stack[sp++] = 0; }
            if (!root) {root=child; dvm_pin(vm,root);}
            dvm_unpin(vm,child);
         } else if (tag && !strcmp(tag, "merge")) {
            if (sp < 64) { stack_view[sp] = false; stack[sp++] = parent; }
         } else {
            dvm_ref view=li_create_tag(vm,inflater,parent,tag,context,parser);
            if (!view || dvm_exception(vm)) break;
            dvm_pin(vm,view);
            if (ui_is_a(vm,view,"android/view/ViewStub")) {
               union dvm_value ctx={.l=context},clone={0};
               if (!li_clone(vm,inflater,&ctx,1,&clone)) {dvm_unpin(vm,view);break;}
               dvm_set_field(vm,view,"stubInflater","Landroid/view/LayoutInflater;",clone);
            }
            if (parent && view) {
               struct dvm_class *parent_class = dvm_object_class(vm, parent);
               struct dvm_method *generate = parent_class ? dvm_find_method(vm, parent_class,
                  "generateLayoutParams", "(Landroid/util/AttributeSet;)Landroid/view/ViewGroup$LayoutParams;") : NULL;
               if (generate) {
                  union dvm_value attr = { .l = parser }, params = { 0 };
                  if (dvm_call(vm, generate, parent, &attr, 1, &params))
                     (void)dvm_set_field(vm, view, "layoutParams", "Landroid/view/ViewGroup$LayoutParams;", params);
               }
            }
            if (view && id) {
               union dvm_value v = { .i = (int32_t)id };
               (void)dvm_set_field(vm, view, "id", "I", v);
            }
            /* Attributes are applied after construction, the same order the
             * platform uses: the constructor establishes the widget's
             * defaults, then the layout overrides them. */
            for (uint16_t i = 0; view && attr_size >= 20 && i < attr_count; ++i) {
               size_t q = attrs + (size_t)i * attr_size;
               if (q + 20 > pos + size) break;
               uint32_t ai = ax_u32(b + q + 4);
               const char *an = ai < nstrings ? strings[ai] : NULL;
               ax_apply_attribute(vm, view, an, b[q + 15], ax_u32(b + q + 16),
                                  strings, nstrings);
            }
            ax_apply_inset_shorthands(vm,view,b,attrs,pos+size,attr_size,attr_count,strings,nstrings,false);
            if (parent && view && add_to_parent) {
               union dvm_value a = { .l = view }, ignored = { 0 };
               (void)vg_add_view(vm, parent, &a, 1, &ignored);
            }
            if (!root) {root=view; dvm_pin(vm,root);}
            if (sp < 64) { stack_view[sp] = true; stack[sp++] = view; }
            dvm_unpin(vm,view);
         }
      } else if (type == 0x0103 && sp) {
         --sp;
         /* The inflater tells a view it is done once its children are in
          * (rInflate(): parent.onFinishInflate()). */
         if (stack_view[sp] && stack[sp]) {
            struct dvm_method *fin = dvm_find_method(vm, dvm_object_class(vm, stack[sp]),
                                                     "onFinishInflate", "()V");
            union dvm_value ignored = { 0 };
            if (fin && !dvm_call(vm, fin, stack[sp], NULL, 0, &ignored)) break;
         }
      }
      if (type == 0x0102 || type == 0x0103 || type == 0x0104) ++event_index;
      pos += size;
   }
   for (uint32_t i = 0; i < nstrings; ++i) free(strings[i]);
   free(strings); free(b);
   dvm_unpin(vm,root); dvm_unpin(vm,parser);
   return dvm_exception(vm) ? 0 : root;
}

static void li_wrap_error(struct dvm *vm,uint32_t resource)
{
   dvm_ref cause=dvm_exception(vm);
   if (!cause || ui_is_a(vm,cause,"android/view/InflateException")) return;
   dvm_pin(vm,cause); dvm_clear_exception(vm);
   dvm__throw(vm,"android/view/InflateException","Error inflating resource 0x%08x",resource);
   (void)dvm_set_field(vm,dvm_exception(vm),"cause","Ljava/lang/Throwable;",(union dvm_value){.l=cause});
   dvm_unpin(vm,cause);
}

static bool li_inflate(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   if (dvm__env("LUNARIA_DVM_TRACE_LAYOUT") && nargs > 0) {
      int32_t value = 0; const char *path = NULL;
      int kind = arm_exec_apk_resource_value((uint32_t)ARG(0).i,
                                              &value, &path);
      fprintf(stderr, "[dvm] inflate resource 0x%08x kind=%d path=%s\n",
              (unsigned)ARG(0).i, kind, path ? path : "(none)");
   }
   union dvm_value context = { 0 };
   (void)dvm_get_field(vm, self, "context", "Landroid/content/Context;",
                       &context);
   dvm_ref parent = nargs > 1 ? ARG(1).l : 0;
   /* inflate(resource, root) is specified as
    * inflate(resource, root, root != null). */
   const bool attach = parent && (nargs < 3 || ARG(2).i);
   dvm_ref view = nargs ? li_inflate_resource(
      vm,self,context.l,(uint32_t)ARG(0).i,0,parent,attach) : 0;
   if (dvm_exception(vm)) {li_wrap_error(vm,(uint32_t)ARG(0).i); return false;}
   if (attach) RETL(view ? parent : 0);
   RETL(view);
}

/* PhoneWindow installs content inside android.R.id.content. Passing that
 * parent to the inflater creates the root's XML LayoutParams and supports
 * <merge>; replacement removes only the content children, never the decor. */
static bool window_set_content(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   union dvm_value parent = {0}, decor = {0}, ignored = {0};
   if (!window_getDecorView(vm,self,NULL,0,&decor)) return false;
   (void)dvm_get_field(vm,self,"contentParent","Landroid/view/ViewGroup;",&parent);
   if (!parent.l) return false;
   if (!vg_remove_all(vm,parent.l,NULL,0,&ignored)) return false;
   const char *sig = builtin_sig();
   if (sig && sig[1]=='I') {
      union dvm_value context = {0}, inflater = {0};
      (void)dvm_get_field(vm,self,"context","Landroid/content/Context;",&context);
      if (!li_from(vm,0,&context,1,&inflater)) return false;
      (void)li_inflate_resource(vm,inflater.l,context.l,(uint32_t)ARG(0).i,0,parent.l,true);
      if (dvm_exception(vm)) {li_wrap_error(vm,(uint32_t)ARG(0).i); return false;}
   } else {
      if (!nargs || !ARG(0).l) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Cannot add a null child view");return false;
      }
      content_install_params(vm,ARG(0).l,args,nargs);
      if (!vg_add_view(vm,parent.l,args,1,&ignored)) return false;
   }
   (void)dvm_set_field(vm,self,"contentView","Landroid/view/View;",parent);
   union dvm_value callback = {0};
   (void)dvm_get_field(vm,self,"callback","Landroid/view/Window$Callback;",&callback);
   struct dvm_method *changed = callback.l ? dvm_find_method(vm,
      dvm_object_class(vm,callback.l),"onContentChanged","()V") : NULL;
   if (changed && !dvm_call(vm,changed,callback.l,NULL,0,&ignored)) return false;
   RETV();
}

static bool view_inflate(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self; (void)nargs;
   union dvm_value inflater = {0};
   if (!li_from(vm, 0, args, 1, &inflater)) return false;
   struct dvm_method *m = dvm_find_method(vm, dvm_object_class(vm, inflater.l),
      "inflate", "(ILandroid/view/ViewGroup;)Landroid/view/View;");
   return m && dvm_call(vm, m, inflater.l, args + 1, 2, out);
}

/* ViewStub is a zero-sized placeholder. Its layout is inflated only on
 * request; normal parent operations preserve callbacks and child ordering. */
static bool viewstub_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                           int nargs,union dvm_value *out)
{
   if (!view_init_context(vm,self,args,1,out)) return false;
   rt_set_int(vm,self,"visibility",8);rt_set_int(vm,self,"inflatedId",-1);rt_set_int(vm,self,"id",-1);
   dvm_set_field(vm,self,"willNotDraw","Z",(union dvm_value){.i=1});
   if (nargs==2 && strstr(builtin_sig(),"Context;I)")) {
      rt_set_int(vm,self,"layoutResource",ARG(1).i);RETV();
   }
   const uint32_t attrs[]={0x010100d0u,0x010100f2u,0x010100f3u}; /* id, layout, inflatedId */
   dvm_ref ids=dvm_new_array(vm,'I',"I",3);
   if (!ids) return false;
   memcpy(dvm_array_data(vm,ids),attrs,sizeof attrs);
   union dvm_value input[]={{.l=nargs>1 ? ARG(1).l : 0},{.l=ids},
      {.i=nargs>2 ? ARG(2).i : 0},{.i=nargs>3 ? ARG(3).i : 0}},array={0},ignored={0};
   if (!am_vcall(vm,ARG(0).l,"obtainStyledAttributes",
      "(Landroid/util/AttributeSet;[III)Landroid/content/res/TypedArray;",input,4,&array)) return false;
   dvm_pin(vm,array.l);
   const char *fields[]={"id","layoutResource","inflatedId"};
   bool good=true;
   for (int i=0;i<3 && good;++i) {
      union dvm_value a[]={{.i=i},{.i=i==1 ? 0 : -1}},value={0};
      good=am_vcall(vm,array.l,"getResourceId","(II)I",a,2,&value);
      if (good) rt_set_int(vm,self,fields[i],value.i);
   }
   if (good) good=am_vcall(vm,array.l,"recycle","()V",NULL,0,&ignored);
   dvm_unpin(vm,array.l);return good;
}

static bool viewstub_inflate(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   dvm_ref parent=ui_obj(vm,self,"parent","Landroid/view/ViewParent;");
   if (!parent || !ui_is_a(vm,parent,"android/view/ViewGroup")) {
      dvm__throw(vm,"java/lang/IllegalStateException","ViewStub must have a non-null ViewGroup parent");return false;
   }
   int32_t layout=rt_get_int(vm,self,"layoutResource");
   if (!layout) {dvm__throw(vm,"java/lang/IllegalArgumentException","ViewStub must have a valid layoutResource");return false;}
   dvm_ref inflater=ui_obj(vm,self,"stubInflater","Landroid/view/LayoutInflater;");
   if (!inflater) {
      union dvm_value context={.l=ui_obj(vm,self,"context","Landroid/content/Context;")},result={0};
      if (!li_from(vm,0,&context,1,&result)) return false;
      inflater=result.l;
   }
   union dvm_value input[]={{.i=layout},{.l=parent},{.i=0}},view={0};
   if (!am_vcall(vm,inflater,"inflate","(ILandroid/view/ViewGroup;Z)Landroid/view/View;",input,3,&view)) return false;
   dvm_pin(vm,view.l);
   union dvm_value ignored={0},index={0},stub={.l=self};
   int32_t id=rt_get_int(vm,self,"inflatedId");
   bool good=true;
   if (id!=-1) {
      union dvm_value value={.i=id};good=am_vcall(vm,view.l,"setId","(I)V",&value,1,&ignored);
   }
   if (good) good=am_vcall(vm,parent,"indexOfChild","(Landroid/view/View;)I",&stub,1,&index);
   if (good) good=am_vcall(vm,parent,"removeViewInLayout","(Landroid/view/View;)V",&stub,1,&ignored);
   union dvm_value a[]={{.l=view.l},{.i=index.i},{.l=ui_obj(vm,self,"layoutParams","Landroid/view/ViewGroup$LayoutParams;")}};
   if (good) good=am_vcall(vm,parent,"addView",a[2].l ?
      "(Landroid/view/View;ILandroid/view/ViewGroup$LayoutParams;)V" : "(Landroid/view/View;I)V",a,a[2].l ? 3 : 2,&ignored);
   if (good) {
      dvm_ref weak=dvm_new_object(vm,dvm_find_class(vm,"java/lang/ref/WeakReference"));
      good=weak && ref_init(vm,weak,&view,1,&ignored)
         && dvm_set_field(vm,self,"inflatedViewRef","Ljava/lang/ref/WeakReference;",(union dvm_value){.l=weak});
   }
   dvm_ref listener=ui_obj(vm,self,"inflateListener","Landroid/view/ViewStub$OnInflateListener;");
   if (good && listener) {
      union dvm_value notify[]={{.l=self},{.l=view.l}};
      good=am_vcall(vm,listener,"onInflate","(Landroid/view/ViewStub;Landroid/view/View;)V",notify,2,&ignored);
   }
   dvm_unpin(vm,view.l);
   if (!good) return false;
   RETL(view.l);
}

static bool viewstub_visibility(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                 int nargs,union dvm_value *out)
{
   dvm_ref weak=ui_obj(vm,self,"inflatedViewRef","Ljava/lang/ref/WeakReference;");
   if (weak) {
      union dvm_value view={0};
      if (!ref_get(vm,weak,NULL,0,&view)) return false;
      if (!view.l) {dvm__throw(vm,"java/lang/IllegalStateException","setVisibility called on un-referenced view");return false;}
      return am_vcall(vm,view.l,"setVisibility","(I)V",args,nargs,out);
   }
   if (!view_set_visibility(vm,self,args,nargs,out)) return false;
   if (ARG(0).i==0 || ARG(0).i==4) {
      union dvm_value ignored={0};return viewstub_inflate(vm,self,NULL,0,&ignored);
   }
   RETV();
}
static bool viewstub_measure(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                              int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   union dvm_value zero[2]={{.i=0},{.i=0}};
   return view_set_measured(vm,self,zero,2,out);
}
static bool viewstub_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                               int nargs,union dvm_value *out)
{
   (void)nargs;
   const char *name=builtin_name();
   const char *field=strstr(name,"LayoutResource") ? "layoutResource" : "inflatedId";
   if (name[0]=='g') RETI(rt_get_int(vm,self,field));
   rt_set_int(vm,self,field,ARG(0).i);RETV();
}
static bool viewstub_reference(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                int nargs,union dvm_value *out)
{
   (void)nargs;
   bool inflater=strstr(builtin_name(),"LayoutInflater")!=NULL;
   const char *field=inflater ? "stubInflater" : "inflateListener";
   const char *sig=inflater ? "Landroid/view/LayoutInflater;" : "Landroid/view/ViewStub$OnInflateListener;";
   if (builtin_name()[0]=='g') {union dvm_value value={0};dvm_get_field(vm,self,field,sig,&value);RETL(value.l);}
   dvm_set_field(vm,self,field,sig,ARG(0));RETV();
}
static const struct rt_field rt_viewstub_fields[]={
 {"layoutResource","I"},{"inflatedId","I"},
 {"stubInflater","Landroid/view/LayoutInflater;"},
 {"inflateListener","Landroid/view/ViewStub$OnInflateListener;"},
 {"inflatedViewRef","Ljava/lang/ref/WeakReference;"},F_END,
};
static const struct rt_method rt_viewstub[]={
 M("<init>","(Landroid/content/Context;)V",viewstub_init),
 M("<init>","(Landroid/content/Context;I)V",viewstub_init),
 M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;)V",viewstub_init),
 M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;I)V",viewstub_init),
 M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;II)V",viewstub_init),
 M("inflate","()Landroid/view/View;",viewstub_inflate),
 M("setVisibility","(I)V",viewstub_visibility),
 M("onMeasure","(II)V",viewstub_measure),
 M("draw","(Landroid/graphics/Canvas;)V",empty_void),
 M("dispatchDraw","(Landroid/graphics/Canvas;)V",empty_void),
 M("getLayoutResource","()I",viewstub_property),M("setLayoutResource","(I)V",viewstub_property),
 M("getInflatedId","()I",viewstub_property),M("setInflatedId","(I)V",viewstub_property),
 M("getLayoutInflater","()Landroid/view/LayoutInflater;",viewstub_reference),
 M("setLayoutInflater","(Landroid/view/LayoutInflater;)V",viewstub_reference),
 M("setOnInflateListener","(Landroid/view/ViewStub$OnInflateListener;)V",viewstub_reference),M_END,
};
static const struct rt_method rt_viewstub_listener[]={
 {"onInflate","(Landroid/view/ViewStub;Landroid/view/View;)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},M_END,
};

static const struct rt_field rt_layout_inflater_fields[] = {
   { "context", "Landroid/content/Context;" },
   { "mFactory", "Landroid/view/LayoutInflater$Factory;" },
   { "mFactory2", "Landroid/view/LayoutInflater$Factory2;" },
   { "mPrivateFactory", "Landroid/view/LayoutInflater$Factory2;" },
   { "mFactorySet", "Z" },
   F_END,
};

static const struct rt_method rt_layout_inflater[] = {
   SM("from", "(Landroid/content/Context;)Landroid/view/LayoutInflater;", li_from),
   M("getContext", "()Landroid/content/Context;", li_get_context),
   M("getFactory", "()Landroid/view/LayoutInflater$Factory;", li_get_factory),
   M("getFactory2", "()Landroid/view/LayoutInflater$Factory2;", li_get_factory),
   M("setFactory", "(Landroid/view/LayoutInflater$Factory;)V", li_set_factory),
   M("setFactory2", "(Landroid/view/LayoutInflater$Factory2;)V", li_set_factory),
   M("setPrivateFactory", "(Landroid/view/LayoutInflater$Factory2;)V", li_set_factory),
   M("cloneInContext", "(Landroid/content/Context;)Landroid/view/LayoutInflater;",
     li_clone),
   M("inflate", "(ILandroid/view/ViewGroup;)Landroid/view/View;", li_inflate),
   M("inflate", "(ILandroid/view/ViewGroup;Z)Landroid/view/View;", li_inflate),
   M_END,
};

static const struct rt_field rt_inflater_merger_fields[] = {
   {"first","Ljava/lang/Object;"}, {"first2","Ljava/lang/Object;"},
   {"second","Ljava/lang/Object;"}, {"second2","Ljava/lang/Object;"}, F_END
};
static const struct rt_method rt_inflater_merger[] = {
   M("onCreateView", "(Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;",li_merger_create),
   M("onCreateView", "(Landroid/view/View;Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;",li_merger_create),
   M_END
};
static const char *const rt_iface_factory2[]={"Landroid/view/LayoutInflater$Factory2;",NULL};
static const char *const rt_iface_factory[]={"Landroid/view/LayoutInflater$Factory;",NULL};
static const char *const rt_iface_xml_resource[]={"Lorg/xmlpull/v1/XmlPullParser;","Landroid/util/AttributeSet;","Ljava/lang/AutoCloseable;",NULL};
static const struct rt_method rt_inflater_factory[] = {
   {"onCreateView","(Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}, M_END
};
static const struct rt_method rt_inflater_factory2[] = {
   {"onCreateView","(Landroid/view/View;Ljava/lang/String;Landroid/content/Context;Landroid/util/AttributeSet;)Landroid/view/View;",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT}, M_END
};

/* Typeface retains its selected immutable host face as well as style. */
static const struct rt_field rt_typeface_fields[] = {
   { "style", "I" }, { "weight", "I" }, { "family", "Ljava/lang/String;" }, { "fontHandle", "I" },
   { "fontFamily", "Landroid/graphics/fonts/FontFamily;" }, { "weightStyles", "Ljava/util/HashMap;" }, F_END
};
static const struct rt_field rt_typeface_sfields[] = {
   { "DEFAULT", "Landroid/graphics/Typeface;" },
   { "DEFAULT_BOLD", "Landroid/graphics/Typeface;" },
   { "SANS_SERIF", "Landroid/graphics/Typeface;" },
   { "SERIF", "Landroid/graphics/Typeface;" },
   { "MONOSPACE", "Landroid/graphics/Typeface;" },
   F_END
};

static dvm_ref typeface_new(struct dvm *vm, const char *family, int32_t style)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/graphics/Typeface;");
   dvm_ref t = c ? dvm_new_object(vm, c) : 0;
   if (!t) return 0;
   rt_set_int(vm, t, "style", style & 3);
   rt_set_int(vm, t, "weight", (style&1) ? 700 : 400);
   union dvm_value v = { .l = dvm_new_string(vm, family ? family : "sans-serif") };
   (void)dvm_set_field(vm, t, "family", "Ljava/lang/String;", v);
   return t;
}

/* Font files and API 29 builders. Included by dvm_android_ui.h.
 * Data is copied into the host font cache; Java objects retain immutable handles.
 * Variable-font axes and additional custom fallback families are not implemented. */
static const struct rt_field rt_font_style_fields[] = {{"weight","I"},{"slant","I"},F_END};
static dvm_ref font_style_new(struct dvm *vm,int weight,int slant)
{
   dvm_ref ref=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/graphics/fonts/FontStyle;"));
   if(ref) {rt_set_int(vm,ref,"weight",weight);rt_set_int(vm,ref,"slant",slant);}
   return ref;
}
static bool font_style_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"<init>")) {
      int weight=nargs ? ARG(0).i : 400,slant=nargs ? ARG(1).i : 0;
      if(weight<1 || weight>1000 || slant<0 || slant>1) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid font style");return false;
      }
      rt_set_int(vm,self,"weight",weight);rt_set_int(vm,self,"slant",slant);RETV();
   }
   if(!strcmp(name,"getWeight")) RETI(rt_get_int(vm,self,"weight"));
   RETI(rt_get_int(vm,self,"slant"));
}
static const struct rt_method rt_font_style[]={
   M("<init>","()V",font_style_method),M("<init>","(II)V",font_style_method),
   M("getWeight","()I",font_style_method),M("getSlant","()I",font_style_method),M_END
};
static const struct rt_field rt_font_fields[]={
   {"handle","I"},{"fontStyle","Landroid/graphics/fonts/FontStyle;"},{"ttcIndex","I"},F_END
};
static const struct rt_field rt_font_builder_fields[]={
   {"path","Ljava/lang/String;"},{"weight","I"},{"slant","I"},{"ttcIndex","I"},F_END
};
static uint32_t font_file_load(const char *path,int index,int *weight,int *slant)
{
   FILE *file=path ? luna_file_fopen(path,"rb") : NULL;
   if(!file) return 0;
   long size=0;unsigned char *bytes=NULL;
   if(!fseek(file,0,SEEK_END) && (size=ftell(file))>0 && size<=INT_MAX && !fseek(file,0,SEEK_SET)) {
      bytes=malloc((size_t)size);
      if(bytes && fread(bytes,1,(size_t)size,file)!=(size_t)size) {free(bytes);bytes=NULL;}
   }
   fclose(file);
   uint32_t handle=bytes ? luna_overlay_font_load(bytes,(size_t)size,index) : 0;
   if(handle && (weight || slant)) {
      /* OS/2 usWeightClass and fsSelection, with the Android default on absence. */
      if(weight) *weight=400;
      if(slant) *slant=0;
      size_t offset=0;
      if(!memcmp(bytes,"ttcf",4)) offset=((size_t)bytes[12+4*index]<<24)|((size_t)bytes[13+4*index]<<16)|((size_t)bytes[14+4*index]<<8)|bytes[15+4*index];
      unsigned count=((unsigned)bytes[offset+4]<<8)|bytes[offset+5];
      for(unsigned i=0;i<count;++i) {
         const unsigned char *entry=bytes+offset+12+16*i;
         if(memcmp(entry,"OS/2",4)) continue;
         size_t pos=((size_t)entry[8]<<24)|((size_t)entry[9]<<16)|((size_t)entry[10]<<8)|entry[11];
         size_t length=((size_t)entry[12]<<24)|((size_t)entry[13]<<16)|((size_t)entry[14]<<8)|entry[15];
         if(length>=6 && weight) {int w=((unsigned)bytes[pos+4]<<8)|bytes[pos+5];*weight=w<1 ? 1 : w>1000 ? 1000 : w;}
         if(length>=64 && slant) *slant=(bytes[pos+63]&1)!=0;
         break;
      }
   }
   free(bytes);return handle;
}
static bool font_builder_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"<init>")) {
      if(!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","font source");return false;}
      const char *path=NULL;char host[DVM_PATH_MAX];
      if(!strcmp(builtin_sig(),"(Landroid/content/res/Resources;I)V")) {
         int32_t value=ARG(1).i;const char *entry=NULL;int kind=1;
         for(int depth=0;kind==1 && depth<32;++depth) kind=arm_exec_apk_resource_value((uint32_t)value,&value,&entry);
         if(kind==0 || kind==1) {dvm__throw(vm,"android/content/res/Resources$NotFoundException","Font resource #0x%x",ARG(1).u);return false;}
         if(kind=='L' && entry && !strstr(entry,".xml")) path=arm_exec_apk_entry_path(entry);
      } else {
         const char *guest=file_path(vm,ARG(0).l);
         if(guest) path=arm_exec_map_guest_path(guest,host,sizeof host);
      }
      if(path) (void)dvm_set_field(vm,self,"path","Ljava/lang/String;",(union dvm_value){.l=dvm_new_string(vm,path)});
      rt_set_int(vm,self,"weight",-1);rt_set_int(vm,self,"slant",-1);RETV();
   }
   if(!strcmp(name,"setWeight")) {
      if(ARG(0).i<1 || ARG(0).i>1000) {dvm__throw(vm,"java/lang/IllegalArgumentException","font weight");return false;}
      rt_set_int(vm,self,"weight",ARG(0).i);RETL(self);
   }
   if(!strcmp(name,"setSlant")) {rt_set_int(vm,self,"slant",ARG(0).i!=0);RETL(self);}
   if(!strcmp(name,"setTtcIndex")) {rt_set_int(vm,self,"ttcIndex",ARG(0).i);RETL(self);}
   union dvm_value path={0};(void)dvm_get_field(vm,self,"path","Ljava/lang/String;",&path);
   int weight=400,slant=0,index=rt_get_int(vm,self,"ttcIndex");
   uint32_t handle=font_file_load(path.l ? dvm_string_utf8(vm,path.l) : NULL,index,&weight,&slant);
   if(!handle) {dvm__throw(vm,"java/io/IOException","Failed to read font contents");return false;}
   if(rt_get_int(vm,self,"weight")!=-1) weight=rt_get_int(vm,self,"weight");
   if(rt_get_int(vm,self,"slant")!=-1) slant=rt_get_int(vm,self,"slant");
   dvm_ref font=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/graphics/fonts/Font;"));
   if(!font) return false;
   rt_set_int(vm,font,"handle",(int32_t)handle);rt_set_int(vm,font,"ttcIndex",index);
   (void)dvm_set_field(vm,font,"fontStyle","Landroid/graphics/fonts/FontStyle;",(union dvm_value){.l=font_style_new(vm,weight,slant)});
   RETL(font);
}
static const struct rt_method rt_font_builder[]={
   M("<init>","(Landroid/content/res/Resources;I)V",font_builder_method),
   M("<init>","(Ljava/io/File;)V",font_builder_method),
   M("setWeight","(I)Landroid/graphics/fonts/Font$Builder;",font_builder_method),
   M("setSlant","(I)Landroid/graphics/fonts/Font$Builder;",font_builder_method),
   M("setTtcIndex","(I)Landroid/graphics/fonts/Font$Builder;",font_builder_method),
   M("build","()Landroid/graphics/fonts/Font;",font_builder_method),M_END
};
static bool font_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   if(!strcmp(builtin_name(),"getTtcIndex")) RETI(rt_get_int(vm,self,"ttcIndex"));
   union dvm_value style={0};(void)dvm_get_field(vm,self,"fontStyle","Landroid/graphics/fonts/FontStyle;",&style);RETL(style.l);
}
static const struct rt_method rt_font[]={M("getStyle","()Landroid/graphics/fonts/FontStyle;",font_method),M("getTtcIndex","()I",font_method),M_END};
static const struct rt_field rt_font_family_fields[]={{"fonts","[Landroid/graphics/fonts/Font;"},F_END};
static bool font_family_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *name=builtin_name();union dvm_value list={0};
   (void)dvm_get_field(vm,self,"fonts","[Landroid/graphics/fonts/Font;",&list);
   int count=list.l ? dvm_array_length(vm,list.l) : 0;
   if(!strcmp(name,"getSize")) RETI(count);
   if(!strcmp(name,"getFont")) {
      if(ARG(0).i<0 || ARG(0).i>=count) {dvm__throw(vm,"java/lang/IndexOutOfBoundsException","font index");return false;}
      RETL(((dvm_ref *)dvm_array_data(vm,list.l))[ARG(0).i]);
   }
   if(!strcmp(name,"build")) {
      dvm_ref family=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/graphics/fonts/FontFamily;"));
      (void)dvm_set_field(vm,family,"fonts","[Landroid/graphics/fonts/Font;",list);RETL(family);
   }
   if(!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","font");return false;}
   if(!strcmp(name,"<init>")) count=0;
   union dvm_value style={0};(void)dvm_get_field(vm,ARG(0).l,"fontStyle","Landroid/graphics/fonts/FontStyle;",&style);
   for(int i=0;i<count;++i) {
      dvm_ref other=((dvm_ref *)dvm_array_data(vm,list.l))[i];union dvm_value other_style={0};
      (void)dvm_get_field(vm,other,"fontStyle","Landroid/graphics/fonts/FontStyle;",&other_style);
      if(rt_get_int(vm,style.l,"weight")==rt_get_int(vm,other_style.l,"weight") && rt_get_int(vm,style.l,"slant")==rt_get_int(vm,other_style.l,"slant")) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Font style already present");return false;
      }
   }
   dvm_ref next=dvm_new_array(vm,'L',"Landroid/graphics/fonts/Font;",count+1);if(!next) return false;
   if(count) memcpy(dvm_array_data(vm,next),dvm_array_data(vm,list.l),(size_t)count*sizeof(dvm_ref));
   ((dvm_ref *)dvm_array_data(vm,next))[count]=ARG(0).l;
   (void)dvm_set_field(vm,self,"fonts","[Landroid/graphics/fonts/Font;",(union dvm_value){.l=next});
   if(!strcmp(name,"<init>")) RETV();
   RETL(self);
}
static const struct rt_method rt_font_family_builder[]={
   M("<init>","(Landroid/graphics/fonts/Font;)V",font_family_method),
   M("addFont","(Landroid/graphics/fonts/Font;)Landroid/graphics/fonts/FontFamily$Builder;",font_family_method),
   M("build","()Landroid/graphics/fonts/FontFamily;",font_family_method),M_END
};
static const struct rt_method rt_font_family[]={M("getSize","()I",font_family_method),M("getFont","(I)Landroid/graphics/fonts/Font;",font_family_method),M_END};
static const struct rt_field rt_font_fallback_fields[]={
   {"fontFamily","Landroid/graphics/fonts/FontFamily;"},{"fontStyle","Landroid/graphics/fonts/FontStyle;"},F_END
};
static dvm_ref font_family_match(struct dvm *vm,dvm_ref family,int weight,int slant)
{
   union dvm_value list={0};
   (void)dvm_get_field(vm,family,"fonts","[Landroid/graphics/fonts/Font;",&list);
   uint32_t count=list.l ? dvm_array_length(vm,list.l) : 0;
   if(!count) {dvm__throw(vm,"java/lang/IllegalArgumentException","Empty font family");return 0;}
   dvm_ref best=0;int score=INT_MAX;
   for(uint32_t i=0;i<count;++i) {
      dvm_ref font=((dvm_ref *)dvm_array_data(vm,list.l))[i];union dvm_value fs={0};
      (void)dvm_get_field(vm,font,"fontStyle","Landroid/graphics/fonts/FontStyle;",&fs);
      int match=abs(rt_get_int(vm,fs.l,"weight")-weight)/100+(rt_get_int(vm,fs.l,"slant")!=slant ? 2 : 0);
      if(match<score) {score=match;best=font;}
   }
   return best;
}
static bool font_fallback_method(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *name=builtin_name();
   if(!strcmp(name,"<init>") || !strcmp(name,"setStyle")) {
      if(!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","font family/style");return false;}
      if(!strcmp(name,"<init>")) {
         (void)dvm_set_field(vm,self,"fontFamily","Landroid/graphics/fonts/FontFamily;",ARG(0));RETV();
      }
      (void)dvm_set_field(vm,self,"fontStyle","Landroid/graphics/fonts/FontStyle;",ARG(0));RETL(self);
   }
   union dvm_value family={0},style={0};
   (void)dvm_get_field(vm,self,"fontFamily","Landroid/graphics/fonts/FontFamily;",&family);
   (void)dvm_get_field(vm,self,"fontStyle","Landroid/graphics/fonts/FontStyle;",&style);
   int weight=style.l ? rt_get_int(vm,style.l,"weight") : 400,slant=style.l ? rt_get_int(vm,style.l,"slant") : 0;
   dvm_ref best=font_family_match(vm,family.l,weight,slant);
   if(!best) return false;
   dvm_ref face=typeface_new(vm,"custom",(weight>=600 ? 1 : 0)|(slant ? 2 : 0));
   if(!face) return false;
   rt_set_int(vm,face,"weight",weight);
   (void)dvm_set_field(vm,face,"fontFamily","Landroid/graphics/fonts/FontFamily;",family);
   rt_set_int(vm,face,"fontHandle",rt_get_int(vm,best,"handle"));RETL(face);
}
static const struct rt_method rt_font_fallback[]={
   M("<init>","(Landroid/graphics/fonts/FontFamily;)V",font_fallback_method),
   M("setStyle","(Landroid/graphics/fonts/FontStyle;)Landroid/graphics/Typeface$CustomFallbackBuilder;",font_fallback_method),
   M("build","()Landroid/graphics/Typeface;",font_fallback_method),M_END
};

/* create(String family, int style) / create(Typeface family, int style) /
 * defaultFromStyle(int style). */
static bool typeface_create(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self;
   const char *sig = builtin_sig();
   const int32_t style = nargs >= 2 ? ARG(1).i : ARG(0).i;
   char family[128] = "sans-serif";
   if (sig && !strncmp(sig, "(Ljava/lang/String;", 19) && ARG(0).l)
      snprintf(family, sizeof family, "%s", dvm_string_utf8(vm, ARG(0).l) ?: "");
   else if (sig && !strncmp(sig, "(Landroid/graphics/Typeface;", 28) && ARG(0).l) {
      union dvm_value f = { 0 };
      (void)dvm_get_field(vm, ARG(0).l, "family", "Ljava/lang/String;", &f);
      if (f.l) snprintf(family, sizeof family, "%s", dvm_string_utf8(vm, f.l) ?: "");
   }
   dvm_ref face=typeface_new(vm,family,style);
   if(face && sig && !strncmp(sig,"(Landroid/graphics/Typeface;",28) && ARG(0).l) {
      union dvm_value source_family={0};
      (void)dvm_get_field(vm,ARG(0).l,"fontFamily","Landroid/graphics/fonts/FontFamily;",&source_family);
      int handle=rt_get_int(vm,ARG(0).l,"fontHandle");
      if(source_family.l) {
         dvm_ref font=font_family_match(vm,source_family.l,(style&1) ? 700 : 400,(style&2)!=0);
         if(!font) return false;
         handle=rt_get_int(vm,font,"handle");
         (void)dvm_set_field(vm,face,"fontFamily","Landroid/graphics/fonts/FontFamily;",source_family);
      }
      rt_set_int(vm,face,"fontHandle",handle);
   }
   RETL(face);
}

static bool typeface_weight_style(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   int weight=args[1].i,italic=args[2].i!=0;
   /* Android checks 0..1000 even though the public annotation says 1..1000. */
   if(weight<0 || weight>1000) {dvm__throw(vm,"java/lang/IllegalArgumentException","weight out of range");return false;}
   dvm_ref base=args[0].l;union dvm_value value={0};
   if(!base) {
      (void)dvm_get_static(vm,dvm__class_by_desc(vm,"Landroid/graphics/Typeface;"),"DEFAULT","Landroid/graphics/Typeface;",&value);
      base=value.l;
   }
   if(!base) return false;
   union dvm_value cache={0},family={0};
   (void)dvm_get_field(vm,base,"weightStyles","Ljava/util/HashMap;",&cache);
   union dvm_value key={.l=box_make(vm,"Ljava/lang/Integer;",(union dvm_value){.i=(weight<<1)|italic})};
   if(!key.l) return false;
   if(cache.l) {
      if(!map_get(vm,cache.l,&key,1,&value)) return false;
      if(value.l) RETL(value.l);
   }
   (void)dvm_get_field(vm,base,"family","Ljava/lang/String;",&family);
   dvm_ref face=typeface_new(vm,dvm_string_utf8(vm,family.l),(weight>=600 ? 1 : 0)|(italic ? 2 : 0));
   if(!face) return false;
   rt_set_int(vm,face,"weight",weight);
   (void)dvm_get_field(vm,base,"fontFamily","Landroid/graphics/fonts/FontFamily;",&family);
   int handle=rt_get_int(vm,base,"fontHandle");
   if(family.l) {
      dvm_ref font=font_family_match(vm,family.l,weight,italic);
      if(!font) return false;
      handle=rt_get_int(vm,font,"handle");
      (void)dvm_set_field(vm,face,"fontFamily","Landroid/graphics/fonts/FontFamily;",family);
   }
   rt_set_int(vm,face,"fontHandle",handle);
   if(!cache.l) {
      cache.l=coll_new_map(vm);
      if(!cache.l) return false;
      (void)dvm_set_field(vm,base,"weightStyles","Ljava/util/HashMap;",cache);
   }
   union dvm_value entry[2]={key,{.l=face}};
   if(!map_put(vm,cache.l,entry,2,&value)) return false;
   RETL(face);
}

/* createFromAsset / createFromFile: the font has to exist — a missing one is
 * RuntimeException("Font asset not found …"), which is how SDKs learn they
 * have to download it. */
static bool typeface_createFrom(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)self; (void)nargs;
   const bool asset = !strcmp(builtin_name(), "createFromAsset");
   dvm_ref pathref = asset ? ARG(1).l : ARG(0).l;
   const char *path = NULL;
   if (pathref && object_is_a(vm, pathref, "Ljava/io/File;"))
      path = file_path(vm, pathref);
   else if (pathref)
      path = dvm_string_utf8(vm, pathref);
   if (!path) {
      dvm__throw(vm, "java/lang/NullPointerException", "path");
      return false;
   }
   uint32_t handle=0;int weight=400,slant=0;
   if (asset) {
      size_t len=0;unsigned char *data=arm_exec_asset_read(path,&len);
      if(data) handle=luna_overlay_font_load(data,len,0);
      free(data);
   } else {
      char host[DVM_PATH_MAX];
      const char *hp=arm_exec_map_guest_path(path,host,sizeof host);
      handle=font_file_load(hp,0,&weight,&slant);
   }
   if(!handle) {
      dvm__throw(vm,"java/lang/RuntimeException","Unable to load font %s",path);return false;
   }
   const char *slash=strrchr(path,'/');
   dvm_ref face=typeface_new(vm,slash ? slash+1 : path,(weight>=600 ? 1 : 0)|(slant ? 2 : 0));
   if(face) {rt_set_int(vm,face,"fontHandle",(int32_t)handle);rt_set_int(vm,face,"weight",weight);}
   RETL(face);
}

static bool typeface_getStyle(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   const int32_t st = rt_get_int(vm, self, "style");
   const char *n = builtin_name();
   if (!strcmp(n, "getWeight")) RETI(rt_get_int(vm,self,"weight"));
   if (!strcmp(n, "isBold")) RETI((st & 1) != 0);
   if (!strcmp(n, "isItalic")) RETI((st & 2) != 0);
   RETI(st);
}

static const struct rt_method rt_typeface[] = {
   SM("createFromAsset",
      "(Landroid/content/res/AssetManager;Ljava/lang/String;)Landroid/graphics/Typeface;",
      typeface_createFrom),
   SM("createFromFile", "(Ljava/lang/String;)Landroid/graphics/Typeface;",
      typeface_createFrom),
   SM("createFromFile", "(Ljava/io/File;)Landroid/graphics/Typeface;",
      typeface_createFrom),
   SM("create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;",
      typeface_create),
   SM("create", "(Landroid/graphics/Typeface;I)Landroid/graphics/Typeface;",
      typeface_create),
   SM("create", "(Landroid/graphics/Typeface;IZ)Landroid/graphics/Typeface;", typeface_weight_style),
   SM("defaultFromStyle", "(I)Landroid/graphics/Typeface;", typeface_create),
   M("getStyle", "()I", typeface_getStyle),
   M("getWeight", "()I", typeface_getStyle),
   M("isBold", "()Z", typeface_getStyle),
   M("isItalic", "()Z", typeface_getStyle),
   M_END,
};

/* android.app.NativeActivity.
 *
 * On a device these do the work of starting a native app: onCreate loads the
 * library named by android.app.lib_name and calls ANativeActivity_onCreate
 * through it, and onStart/onResume forward to the activity's native
 * callbacks.  That is guest work (the ELF image, its threads and the command
 * pipe of its glue), so the guest side does it behind the native_activity
 * hook, from the lifecycle call itself and in the order Android makes them.
 * A subclass such as UE4's GameActivity reaches here through super.onCreate().
 * Failure to load or enter native code is an UnsatisfiedLinkError. */
static bool native_activity_stage(struct dvm *vm, dvm_ref self, int stage)
{
   if (vm->hooks.native_activity &&
       vm->hooks.native_activity(vm->hooks.user, vm, stage, self)) return true;
   if (!dvm_exception(vm))
      dvm__throw(vm, "java/lang/UnsatisfiedLinkError", "NativeActivity native lifecycle stage %d failed", stage);
   return false;
}

static bool na_onCreate(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   union dvm_value context = { .l = self }, view = { 0 }, ignored = { 0 };
   struct dvm_class *cls = dvm_find_class(vm, "android/app/NativeActivity$NativeContentView");
   view.l = cls ? dvm_new_object(vm, cls) : 0;
   if (!view.l) return false;
   dvm_pin(vm, view.l);
   bool good = am_vcall(vm, view.l, "<init>", "(Landroid/content/Context;)V", &context, 1, &ignored);
   if (good) good = act_set_content_view(vm, self, &view, 1, &ignored);
   if (good) good = am_vcall(vm, view.l, "requestFocus", "()Z", NULL, 0, &ignored);
   dvm_unpin(vm, view.l);
   if (!good) return false;

   union dvm_value component = { 0 }, metadata = { 0 };
   if (!act_get_component_name(vm, self, NULL, 0, &component)) return false;
   union dvm_value query[2] = { component, { .i = PM_GET_META_DATA } };
   dvm_ref info = pm_info_for_component(vm, "Landroid/content/pm/ActivityInfo;", query, 2);
   (void)dvm_get_field(vm, info, "metaData", "Landroid/os/Bundle;", &metadata);
   union dvm_value key = { .l = dvm_new_string(vm, "android.app.lib_name") }, library = { 0 };
   if (metadata.l && !bundle_getString(vm, metadata.l, &key, 1, &library)) return false;
   if (!library.l) library.l = dvm_new_string(vm, "main");
   if (!sys_loadLibrary(vm, 0, &library, 1, &ignored)) return false;
   if (!native_activity_stage(vm, self, DVM_NATIVE_ACTIVITY_CREATE)) return false;
   if (!act_on_create(vm, self, args, nargs, out)) return false;
   RETV();
}

static bool na_onStart(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   if (!native_activity_stage(vm, self, DVM_NATIVE_ACTIVITY_START)) return false;
   RETV();
}

static bool na_onResume(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   if (!native_activity_stage(vm, self, DVM_NATIVE_ACTIVITY_RESUME)) return false;
   RETV();
}

static bool na_window_flags_run(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value activity={0},window={0},flags[2]={{0},{0}};
   (void)dvm_get_field(vm,self,"activity","Landroid/app/Activity;",&activity);
   (void)dvm_get_field(vm,self,"flags","I",&flags[0]);
   (void)dvm_get_field(vm,self,"mask","I",&flags[1]);
   if (!act_get_window(vm,activity.l,NULL,0,&window)) return false;
   return am_vcall(vm,window.l,"setFlags","(II)V",flags,2,out);
}

/* NDK window requests are posted to the main Looper from any caller thread. */
static bool na_set_window_flags(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)nargs;
   dvm_ref task=dvm_new_object(vm,dvm_find_class(vm,"lunaria/NativeWindowFlags"));
   if (!task) return false;
   dvm_pin(vm,task);
   (void)dvm_set_field(vm,task,"activity","Landroid/app/Activity;",(union dvm_value){.l=self});
   (void)dvm_set_field(vm,task,"flags","I",ARG(0));
   (void)dvm_set_field(vm,task,"mask","I",ARG(1));
   bool queued=queue_runnable_for(vm,task,false,0,0,self,0);
   dvm_unpin(vm,task);
   if (!queued) {
      dvm__throw(vm,"java/lang/IllegalStateException","native window request queue full");
      return false;
   }
   RETV();
}
static const struct rt_field rt_native_window_flags_fields[] = {
   {"activity","Landroid/app/Activity;"},{"flags","I"},{"mask","I"},F_END,
};
static const struct rt_method rt_native_window_flags[] = {
   M("run","()V",na_window_flags_run),M_END,
};

static const struct rt_method rt_native_content_view[] = {
   M("<init>", "(Landroid/content/Context;)V", view_init_context),
   M("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V", view_init_context),
   M_END,
};

static const struct rt_method rt_native_activity[] = {
   M("setWindowFlags", "(II)V", na_set_window_flags),
   M("<init>", "()V", empty_void),
   M("onCreate", "(Landroid/os/Bundle;)V", na_onCreate),
   M("onStart", "()V", na_onStart),
   M("onResume", "()V", na_onResume),
   M_END,
};

/* AlertDialog.Builder is stateful on Android: its mutators return the same
 * builder and create() materializes a distinct Dialog carrying that state.
 * Keeping that contract matters even for a headless backend because guest
 * code commonly builds dialogs through a fluent call chain. */
/* --- the emulator's widget presentation layer --------------------------- *
 *
 * Everything above keeps a faithful *book* of the Android view tree: parents,
 * children, ids, text, colours, the geometry the layout asked for.  None of it
 * ever reached a pixel, because Lunaria had no measure/layout/draw pass and no
 * window compositor.  For a game that draws its own frames with GL that was
 * invisible — right up to the point where the SDK puts a modal dialog in front
 * of the player and waits.  Netmarble's terms-of-service gate is exactly that:
 * a native layout (nm_tos_single.xml — title, a card with the terms, an Agree
 * and a Disagree row) whose Agree button the boot flow will not pass without.
 *
 * This is the missing half.  The view tree is translated into an HTML/CSS
 * document and handed to luna-ui through luna_overlay, which composites it
 * over the guest's finished frame — the position a real compositor puts a
 * dialog window in.  A click on an element is routed back to the OnClickListener
 * of the View it stands for.
 *
 * The mapping is deliberately narrow and literal: LinearLayout is a flex box in
 * its declared orientation, layout_weight is flex-grow, MATCH_PARENT/
 * WRAP_CONTENT are 100%/auto, and everything else stacks in document order.
 * That is not a general Android layout engine, and it does not pretend to be
 * one; it is enough to place, read and press the dialogs an SDK puts up.
 */

#include "luna_overlay.h"

/* Gravity bits, from android.view.Gravity. */
#define UI_GRAVITY_CENTER_H 0x01
#define UI_GRAVITY_CENTER_V 0x10
/* parent_orientation values that are not a LinearLayout axis. */
#define UI_PARENT_FRAME    (-2)   /* a FrameLayout: children overlay */
#define UI_PARENT_RELATIVE (-3)   /* RelativeLayout: align* rules */
#define UI_GRAVITY_LEFT     0x03
#define UI_GRAVITY_RIGHT    0x05
#define UI_GRAVITY_TOP      0x30
#define UI_GRAVITY_BOTTOM   0x50

/* A growing text buffer.  The document is built once per change, not per
 * frame, so a plain realloc buffer is the whole story. */
struct ui_buf { char *p; size_t len, cap; };

static void ui_put(struct ui_buf *b, const char *s, size_t n)
{
   if (!b->p && b->cap) return;                   /* a previous grow failed */
   if (b->len + n + 1 > b->cap) {
      size_t cap = b->cap ? b->cap * 2 : 4096;
      while (cap < b->len + n + 1) cap *= 2;
      char *q = realloc(b->p, cap);
      if (!q) { free(b->p); b->p = NULL; b->cap = 1; return; }
      b->p = q; b->cap = cap;
   }
   memcpy(b->p + b->len, s, n);
   b->len += n;
   b->p[b->len] = '\0';
}

static void ui_puts(struct ui_buf *b, const char *s) { ui_put(b, s, strlen(s)); }

static void ui_printf(struct ui_buf *b, const char *fmt, ...)
{
   char tmp[512];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
   va_end(ap);
   if (n > 0) ui_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* Text out of the guest goes into markup, so the five characters that would
 * otherwise be parsed as markup have to stop being those characters. */
static void ui_put_escaped(struct ui_buf *b, const char *s)
{
   for (; s && *s; ++s) {
      switch (*s) {
      case '&': ui_puts(b, "&amp;"); break;
      case '<': ui_puts(b, "&lt;"); break;
      case '>': ui_puts(b, "&gt;"); break;
      case '"': ui_puts(b, "&quot;"); break;
      default: ui_put(b, s, 1); break;
      }
   }
}

/* Field lookup matches on the descriptor, so an int read of a field declared
 * `Z` finds nothing and answers 0 — which is a valid value, so the miss is
 * invisible.  Both widths live in the same slot here, and which of the two a
 * given flag was declared with is not something the caller should have to
 * track, so both are tried. */
static int32_t ui_int(struct dvm *vm, dvm_ref o, const char *name)
{
   union dvm_value v = { 0 };
   if (dvm_get_field(vm, o, name, "I", &v)) return v.i;
   if (dvm_get_field(vm, o, name, "Z", &v)) return v.i;
   return 0;
}

static float ui_float(struct dvm *vm, dvm_ref o, const char *name)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, o, name, "F", &v);
   return v.f;
}



/* The simple name of a view's class, for the element's CSS class. */
static const char *ui_class_simple_name(struct dvm *vm, dvm_ref view)
{
   struct dvm_class *c = dvm_object_class(vm, view);
   const char *n = c ? dvm_class_name(c) : NULL;
   if (!n) return "view";
   const char *slash = strrchr(n, '/');
   return slash ? slash + 1 : n;
}

/* Whether `view` is a `want` (or derives from one), by walking the declared
 * superclass chain the same way the VM's own cast check does. */


/* A CharSequence is not always a String — SpannableString and StringBuilder
 * are the two an app reaches for constantly — and reading only Strings would
 * silently drop the label. */


/* Android colours are 0xAARRGGBB ints; CSS wants the alpha separately and as a
 * fraction.  A fully transparent colour is "no background", which is also what
 * a view that was never given one has. */
static void ui_color_css(char *out, size_t n, int32_t argb)
{
   unsigned a = ((uint32_t)argb >> 24) & 0xff;
   snprintf(out, n, "rgba(%u,%u,%u,%.3f)",
            ((uint32_t)argb >> 16) & 0xff, ((uint32_t)argb >> 8) & 0xff,
            (uint32_t)argb & 0xff, a / 255.0);
}

/* A background can be a colour attribute the inflater resolved, or a Drawable
 * the code set later; both end up as one colour here. */
static const char *bitmap_png_path(struct dvm *vm, dvm_ref b);
static bool ninepatch_border_image_css(struct dvm *vm, dvm_ref drawable,
                                       char *out, size_t n);

/* What a background drawable adds beyond a colour: a GradientDrawable's
 * corners (an OVAL is fully round), stroke and gradient colours, a
 * BitmapDrawable's image.  `radius` comes in as the view's own and leaves as
 * the one to draw. */
/* The view's background as the overlay draws it: a StateListDrawable stands
 * for the drawable of its resting state. */
/* A drawable as it looks at rest: a StateListDrawable stands for the
 * drawable of its resting state. */
static dvm_ref ui_drawable_at_rest(struct dvm *vm, dvm_ref d)
{
   struct dvm_class *sc =
      dvm__class_by_desc(vm, "Landroid/graphics/drawable/StateListDrawable;");
   struct dvm_class *lc =
      dvm__class_by_desc(vm, "Landroid/graphics/drawable/LayerDrawable;");
   struct dvm_class *ic =
      dvm__class_by_desc(vm, "Landroid/graphics/drawable/InsetDrawable;");
   for (int hop = 0; d && sc && hop < 4; ++hop) {
      struct dvm_class *oc = dvm_object_class(vm, d);
      if (!oc || !(dvm__class_assignable(vm, oc, sc) ||
                   (lc && dvm__class_assignable(vm, oc, lc)) ||
                   (ic && dvm__class_assignable(vm, oc, ic)))) break;
      d = ui_obj(vm, d, "current", "Landroid/graphics/drawable/Drawable;");
   }
   return d;
}

static dvm_ref ui_view_background(struct dvm *vm, dvm_ref view)
{
   return ui_drawable_at_rest(vm, ui_obj(vm, view, "background",
                                         "Landroid/graphics/drawable/Drawable;"));
}

static void ui_emit_background_shape(struct dvm *vm, struct ui_buf *css,
                                     dvm_ref view, int32_t *radius)
{
   dvm_ref bg = ui_view_background(vm, view);
   if (!bg) return;
   struct dvm_class *gc =
      dvm__class_by_desc(vm, "Landroid/graphics/drawable/GradientDrawable;");
   struct dvm_class *bc =
      dvm__class_by_desc(vm, "Landroid/graphics/drawable/BitmapDrawable;");
   struct dvm_class *nc =
      dvm__class_by_desc(vm, "Landroid/graphics/drawable/NinePatchDrawable;");
   struct dvm_class *oc = dvm_object_class(vm, bg);
   if (oc && nc && dvm__class_assignable(vm, oc, nc)) {
      char bi[DVM_PATH_MAX + 160];
      if (ninepatch_border_image_css(vm, bg, bi, sizeof bi)) ui_puts(css, bi);
   } else if (oc && gc && dvm__class_assignable(vm, oc, gc)) {
      const float r = ui_float(vm, bg, "cornerRadius");
      if (ui_int(vm, bg, "shape") == 1) {
         ui_puts(css, "border-radius:50%;");
         *radius = 0;
      } else if (r > 0.0f && *radius <= 0) {
         *radius = (int32_t)(r + 0.5f);
      }
      const int32_t sw = ui_int(vm, bg, "strokeWidth");
      if (sw > 0) {
         char c[64];
         ui_color_css(c, sizeof c, ui_int(vm, bg, "strokeColor"));
         ui_printf(css, "border:%dpx solid %s;", sw, c);
      }
      union dvm_value colors = { 0 };
      (void)dvm_get_field(vm, bg, "colors", "[I", &colors);
      struct dvm_object *a = colors.l ? dvm__obj(vm, colors.l) : NULL;
      if (a && a->length >= 2) {
         static const char *const dirs[] = {
            "to bottom", "to bottom left", "to left", "to top left",
            "to top", "to top right", "to right", "to bottom right",
         };
         union dvm_value o = { 0 }, ord = { 0 };
         (void)dvm_get_field(vm, bg, "orientation",
            "Landroid/graphics/drawable/GradientDrawable$Orientation;", &o);
         if (o.l) (void)dvm_get_field(vm, o.l, "ordinal", "I", &ord);
         ui_printf(css, "background:linear-gradient(%s",
                   dirs[(unsigned)ord.i < 8 ? ord.i : 0]);
         for (uint32_t i = 0; i < a->length; ++i) {
            char c[64];
            ui_color_css(c, sizeof c, ((int32_t *)a->data)[i]);
            ui_printf(css, ",%s", c);
         }
         ui_puts(css, ");");
      }
   } else if (oc && bc && dvm__class_assignable(vm, oc, bc)) {
      dvm_ref b = ui_obj(vm, bg, "bitmap", "Landroid/graphics/Bitmap;");
      const char *png = b ? bitmap_png_path(vm, b) : NULL;
      if (png)
         ui_printf(css, "background-image:url('%s');background-size:100%% 100%%;",
                   png);
   }
}

static bool ui_background_color(struct dvm *vm, dvm_ref view, int32_t *out)
{
   int32_t c = ui_int(vm, view, "backgroundColor");
   if (c) { *out = c; return true; }
   dvm_ref bg = ui_view_background(vm, view);
   if (bg) {
      c = ui_int(vm, bg, "color");
      if (c) { *out = c; return true; }
   }
   return false;
}

/* A drawable resource as a file the overlay's renderer can load.
 *
 * On a device a drawable is most of what a widget *is*: an ImageButton has no
 * text, so the icon is the whole button.  This layer drew text and nothing
 * else, which made every icon-only view an empty box — invisible, and with
 * wrap_content sizing, zero-sized and therefore unhittable.  A dialog's close
 * button is exactly that view, so a window the emulator put up could not be
 * taken down, and its scrim goes on swallowing every touch meant for the game
 * underneath.
 *
 * resources.arsc already maps the id to an entry name and the package is
 * already unpacked, so this is a lookup, not a decoder.  Only the bitmap
 * formats luna-ui reads are answered: an .xml drawable is a vector or a state
 * list, which is a program to run rather than a file to show, and claiming to
 * have drawn one would be a worse answer than drawing nothing. */
static const char *ui_drawable_path(int32_t res_id)
{
   if (res_id <= 0) return NULL;
   int32_t iv = 0;
   const char *entry = NULL;
   if (!arm_exec_apk_resource_value((uint32_t)res_id, &iv, &entry)) return NULL;
   if (!entry || !*entry) return NULL;
   const char *dot = strrchr(entry, '.');
   if (!dot) return NULL;
   if (strcmp(dot, ".png") != 0 && strcmp(dot, ".jpg") != 0 &&
       strcmp(dot, ".jpeg") != 0 && strcmp(dot, ".webp") != 0 &&
       strcmp(dot, ".svg") != 0)
      return NULL;
   return arm_exec_apk_entry_path(entry);
}

/* --- VectorDrawable -> SVG -----------------------------------------------
 *
 * A <vector> drawable is a viewport of paths, which is exactly an SVG.  The
 * overlay's renderer loads .svg files, so a vector resource is written once
 * beside its unpacked entry as "<entry>.svg" and drawn like any image. */

static bool dx_color_svg(struct dvm *vm, const struct dx *x, const struct dx_tag *t,
                         const char *name, float alpha, char *rgb, size_t cap, float *opacity)
{
   uint8_t type; uint32_t data;
   int32_t argb = 0;
   if (!dx_attr(x, t, name, &type, &data)) return false;
   if (!ax_color(vm, type, data, &argb)) argb = (int32_t)0xff000000u;   /* ?attr tint: unresolved here */
   snprintf(rgb, cap, "#%06x", (unsigned)argb & 0xffffffu);
   *opacity = (float)(((uint32_t)argb >> 24) & 0xff) / 255.0f * alpha;
   return true;
}

static void dx_svg_escape(FILE *f, const char *s)
{
   for (; s && *s; ++s) {
      if (*s == '"' || *s == '&' || *s == '<') continue;   /* path data has none */
      fputc(*s, f);
   }
}

static bool vector_write_svg(struct dvm *vm, const unsigned char *b, size_t n, const char *out_path)
{
   struct dx x = { .b = b, .n = n, .pos = n >= 8 ? ax_u16(b + 2) : n };
   x.s = ax_string_pool(b, n, &x.ns);
   struct dx_tag t;
   bool ok = false;
   if (x.s && dx_next(&x, &t) == 0x0102 && !strcmp(t.name, "vector")) {
      FILE *f = luna_file_fopen(out_path, "w");
      if (f) {
         const float vw = dx_float(&x, &t, "viewportWidth", 24.f);
         const float vh = dx_float(&x, &t, "viewportHeight", 24.f);
         const float w = dx_float(&x, &t, "width", vw), h = dx_float(&x, &t, "height", vh);
         fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%gpx\" height=\"%gpx\" "
                    "viewBox=\"0 0 %g %g\">\n", (double)w, (double)h, (double)vw, (double)vh);
         const float root_alpha = dx_float(&x, &t, "alpha", 1.0f);
         bool group[32] = { false };
         int depth = 1;
         for (int e; depth > 0 && (e = dx_next(&x, &t)) != 0;) {
            if (e == 0x0103) {
               --depth;
               if (depth >= 1 && depth < 32 && group[depth]) fputs("</g>\n", f);
               continue;
            }
            const bool is_group = !strcmp(t.name, "group");
            if (depth < 32) group[depth] = is_group;
            if (is_group) {
               const float px = dx_float(&x, &t, "pivotX", 0.f), py = dx_float(&x, &t, "pivotY", 0.f);
               fprintf(f, "<g transform=\"translate(%g %g) translate(%g %g) rotate(%g) scale(%g %g) translate(%g %g)\">\n",
                       (double)dx_float(&x, &t, "translateX", 0.f), (double)dx_float(&x, &t, "translateY", 0.f),
                       (double)px, (double)py, (double)dx_float(&x, &t, "rotation", 0.f),
                       (double)dx_float(&x, &t, "scaleX", 1.f), (double)dx_float(&x, &t, "scaleY", 1.f),
                       (double)-px, (double)-py);
            } else if (!strcmp(t.name, "path")) {
               uint8_t type; uint32_t data;
               if (dx_attr(&x, &t, "pathData", &type, &data) && data < x.ns && x.s[data]) {
                  char rgb[16]; float op;
                  fputs("<path d=\"", f);
                  dx_svg_escape(f, x.s[data]);
                  fputc('"', f);
                  if (dx_color_svg(vm, &x, &t, "fillColor", root_alpha * dx_float(&x, &t, "fillAlpha", 1.f),
                                   rgb, sizeof rgb, &op))
                     fprintf(f, " fill=\"%s\" fill-opacity=\"%g\"", rgb, (double)op);
                  else fputs(" fill=\"none\"", f);
                  if (dx_int(&x, &t, "fillType", 0) == 1) fputs(" fill-rule=\"evenodd\"", f);
                  if (dx_color_svg(vm, &x, &t, "strokeColor", root_alpha * dx_float(&x, &t, "strokeAlpha", 1.f),
                                   rgb, sizeof rgb, &op)) {
                     static const char *const cap[] = { "butt", "round", "square" };
                     static const char *const join[] = { "miter", "round", "bevel" };
                     const int c = dx_int(&x, &t, "strokeLineCap", 0), j = dx_int(&x, &t, "strokeLineJoin", 0);
                     fprintf(f, " stroke=\"%s\" stroke-opacity=\"%g\" stroke-width=\"%g\" "
                                "stroke-linecap=\"%s\" stroke-linejoin=\"%s\"", rgb, (double)op,
                             (double)dx_float(&x, &t, "strokeWidth", 0.f),
                             cap[(unsigned)c < 3 ? c : 0], join[(unsigned)j < 3 ? j : 0]);
                  }
                  fputs("/>\n", f);
               }
            }
            ++depth;
         }
         while (depth-- > 1) if (depth < 32 && group[depth]) fputs("</g>\n", f);
         fputs("</svg>\n", f);
         ok = fclose(f) == 0;
      }
   }
   for (uint32_t i = 0; x.s && i < x.ns; ++i) free(x.s[i]);
   free(x.s);
   return ok;
}

/* The image file a drawable resource shows: a bitmap as itself, a vector as
 * the SVG written for it. */
static const char *ui_drawable_src(struct dvm *vm, int32_t res_id)
{
   const char *png = ui_drawable_path(res_id);
   if (png || res_id <= 0) return png;
   int32_t iv = 0; const char *entry = NULL;
   if (arm_exec_apk_resource_value((uint32_t)res_id, &iv, &entry) != 'L' || !entry) return NULL;
   const size_t len = strlen(entry);
   if (len <= 4 || strcasecmp(entry + len - 4, ".xml")) return NULL;
   const char *host = arm_exec_apk_entry_path(entry);
   static char svg[DVM_PATH_MAX];
   if (!host || snprintf(svg, sizeof svg, "%s.svg", host) >= (int)sizeof svg) return NULL;
   struct stat sb;
   if (luna_file_stat_follow(svg, &sb) == 0 && S_ISREG(sb.st_mode)) return svg;
   size_t n = 0;
   unsigned char *b = arm_exec_asset_read(entry, &n);
   const bool ok = b && n >= 8 && ax_u16(b) == 0x0003 && vector_write_svg(vm, b, n, svg);
   free(b);
   return ok ? svg : NULL;
}

/* A PNG's pixel size, from its IHDR — the first chunk, at a fixed offset.
 *
 * This is the drawable's intrinsic size, which is what Android measures an
 * ImageView's wrap_content against.  Without it an icon-only view has no size
 * at all: nothing to paint the image into and nothing to press.  Reading two
 * big-endian words is not decoding the image, and the renderer decodes it
 * anyway when it draws. */
static bool ui_png_size(const char *path, int32_t *w, int32_t *h)
{
   static const unsigned char sig[8] =
      { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
   unsigned char hdr[24];
   FILE *f = luna_file_fopen(path, "rb");
   if (!f) return false;
   size_t n = fread(hdr, 1, sizeof hdr, f);
   fclose(f);
   if (n != sizeof hdr || memcmp(hdr, sig, sizeof sig) != 0) return false;
   if (memcmp(hdr + 12, "IHDR", 4) != 0) return false;
   *w = (int32_t)(((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) |
                  ((uint32_t)hdr[18] << 8) | hdr[19]);
   *h = (int32_t)(((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) |
                  ((uint32_t)hdr[22] << 8) | hdr[23]);
   return *w > 0 && *h > 0;
}

/* The drawable this view shows: its source image if it has one, otherwise the
 * background it was given as a reference rather than a colour.
 *
 * `lw`/`lh` are the layout's own width and height, so a view that was sized
 * by the layout keeps that size and only a wrap_content one is measured from
 * the image, as the platform does. */
static void ui_emit_drawables(struct dvm *vm, struct ui_buf *css, dvm_ref view,
                              int32_t lw, int32_t lh)
{
   const char *src = ui_drawable_src(vm, ui_int(vm, view, "imageResource"));
   /* An image set as a Bitmap (setImageBitmap / a BitmapDrawable). */
   if (!src) {
      dvm_ref d = ui_drawable_at_rest(vm, ui_obj(vm, view, "drawable",
                                      "Landroid/graphics/drawable/Drawable;"));
      dvm_ref b = d ? ui_obj(vm, d, "bitmap", "Landroid/graphics/Bitmap;") : 0;
      if (b) src = bitmap_png_path(vm, b);
   }
   if (!src) src = ui_drawable_src(vm, ui_int(vm, view, "backgroundResource"));
   if (src) {
      /* contain, because the entry resources.arsc names is whichever density
       * bucket this build shipped and the view was sized in dp. */
      /* How the image fills the box is the view's scale type; FIT_CENTER
       * (contain) is ImageView's default. */
      static const char *const fills[] = {
         "contain", "auto", "100% 100%", "contain", "contain", "contain",
         "auto", "cover", "contain",
      };
      const int32_t st = ui_int(vm, view, "scaleType");
      const char *pos = st == 3 || st == 1 ? "left top"
                      : st == 5 ? "right bottom" : "center";
      ui_printf(css, "background-image:url('%s');background-size:%s;"
                     "background-repeat:no-repeat;background-position:%s;",
                src, fills[(unsigned)st < 9 ? st : 0], pos);
      int32_t iw = 0, ih = 0;
      if (ui_png_size(src, &iw, &ih)) {
         if (lw != -1 && lw <= 0) ui_printf(css, "width:%dpx;", iw);
         if (lh != -1 && lh <= 0) ui_printf(css, "height:%dpx;", ih);
         return;
      }
   }
   /* Vector / state-list drawables are not rasterised here.  An icon-only
    * ImageView/ImageButton/Button that is wrap_content (or whose @dimen size
    * failed to resolve) would otherwise measure to nothing and become
    * unhittable — exactly WebViewDialog's close control, which left the
    * modal overlay eating every touch under the character select screen.
    * Give it a real hit target; the glyph may still be blank. */
   if (ui_is_a(vm, view, "android/widget/ImageView") ||
       ui_is_a(vm, view, "android/widget/ImageButton") ||
       ui_is_a(vm, view, "android/widget/Button")) {
      if (lw != -1 && lw <= 0) ui_puts(css, "width:48px;min-width:48px;");
      if (lh != -1 && lh <= 0) ui_puts(css, "height:48px;min-height:48px;");
   }
}

static const char *ui_capture_custom_view(struct dvm *vm,dvm_ref view);
static void ui_emit_view(struct dvm *vm, struct ui_buf *b, struct ui_buf *css,
                         dvm_ref view, int parent_orientation, unsigned depth);
static bool ui_is_clickable(struct dvm *vm, dvm_ref view);

/* An exception thrown inside the widget layer would otherwise vanish: the
 * caller is the emulator, not the app, so nothing else will ever report it. */
static void ui_report_exception(struct dvm *vm, const char *what)
{
   dvm_ref e = dvm_exception(vm);
   char buf[256] = "";
   if (e) dvm_describe_exception(vm, e, buf, sizeof buf);
   fprintf(stderr, "[ui] %s threw %s\n", what, buf[0] ? buf : "(no exception)");
   dvm_clear_exception(vm);
}

static bool ui_call(struct dvm *vm, dvm_ref self, const char *method,
                    const char *sig, const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   struct dvm_class *c = dvm_object_class(vm, self);
   struct dvm_method *m = c ? dvm_find_method_by_params(vm, c, method, sig) : NULL;
   if (!m) {
      fprintf(stderr, "[ui] %s%s not found on %s\n", method, sig,
              c ? dvm_class_name(c) : "?");
      return false;
   }
   if (!dvm_call(vm, m, self, args, nargs, out)) {
      ui_report_exception(vm, method);
      return false;
   }
   return true;
}

/* Draw the actual ViewGroup children managed by the guest. */
static void ui_emit_children(struct dvm *vm, struct ui_buf *b,
                             struct ui_buf *css, dvm_ref view,
                             int orientation, unsigned depth)
{
   dvm_ref kids = ui_obj(vm, view, "children", "Ljava/util/List;");
   struct rt_list *l = kids ? list_of(vm, kids) : NULL;
   union dvm_value enabled = { 0 };
   (void)dvm_get_field(vm, view, "childrenDrawingOrderEnabled", "Z", &enabled);
   if (!enabled.i || !l || !l->size) {
      for (uint32_t i = 0; l && i < l->size; ++i)
         ui_emit_view(vm, b, css, l->items[i], orientation, depth + 1);
      return;
   }
   /* Guest overrides can run arbitrary code, including modifying the child
    * list. Retain the children across callbacks; never sort the layout list. */
   const uint32_t count = l->size;
   dvm_ref *children = malloc((size_t)count * sizeof *children);
   if (!children) {
      dvm__throw(vm, "java/lang/OutOfMemoryError", "child drawing order");
      return;
   }
   memcpy(children, l->items, (size_t)count * sizeof *children);
   for (uint32_t i = 0; i < count; ++i) dvm_pin(vm, children[i]);
   for (uint32_t i = 0; i < count; ++i) {
      union dvm_value a[2] = { { .i = (int32_t)count }, { .i = (int32_t)i } }, index = { 0 };
      if (!view_state_call(vm, view, "getChildDrawingOrder", "(II)I", a, 2, &index)) break;
      if (index.i < 0 || (uint32_t)index.i >= count) {
         dvm__throw(vm, "java/lang/IndexOutOfBoundsException",
                    "getChildDrawingOrder() returned invalid index %d (child count is %u)", index.i, count);
         break;
      }
      ui_emit_view(vm, b, css, children[index.i], orientation, depth + 1);
   }
   for (uint32_t i = 0; i < count; ++i) dvm_unpin(vm, children[i]);
   free(children);
}

/* parent_orientation: 1 vertical, 0 horizontal, -1 not a flex parent.
 *
 * The structure goes into `b` and the per-view declarations into `css`, as a
 * rule keyed on the element's id.  They are not written inline: luna-ui's
 * inline-style path applies only part of what it parses (padding, flex,
 * align-self, opacity and visibility are all dropped there) and caps the
 * attribute at 256 bytes, while the stylesheet path supports the whole set. */
/* ---- Android's measure and layout pass ----------------------------------
 *
 * The overlay draws boxes; Android decides where they go.  This is that
 * decision, made the way the framework makes it: MeasureSpecs flow down,
 * measured sizes flow up (View.onMeasure / ViewGroup.getChildMeasureSpec),
 * then every ViewGroup places its children (onLayout).  The emitter below
 * writes each view as an absolutely positioned box of exactly that size.
 *
 * It replaced a translation of the view tree into flex CSS.  That was an
 * approximation with no way to express RelativeLayout — below/above/toRightOf
 * a sibling, centred in the parent — and it lost every rule an SDK added in
 * code: HoYoverse's login screen came out as one row across the top of the
 * display.  A box per view is also the cheapest thing the renderer can be
 * asked to draw: no flex solving at all. */

enum { UI_MS_UNSPEC = 0, UI_MS_EXACT = 1, UI_MS_ATMOST = 2 };
struct ui_ms { int mode; int32_t size; };
static void list_view_measure(struct dvm *,dvm_ref,struct ui_ms,struct ui_ms,unsigned);

/* RelativeLayout verbs, android.widget.RelativeLayout. */
enum {
   RL_LEFT_OF, RL_RIGHT_OF, RL_ABOVE, RL_BELOW, RL_ALIGN_BASELINE,
   RL_ALIGN_LEFT, RL_ALIGN_TOP, RL_ALIGN_RIGHT, RL_ALIGN_BOTTOM,
   RL_ALIGN_PARENT_LEFT, RL_ALIGN_PARENT_TOP, RL_ALIGN_PARENT_RIGHT,
   RL_ALIGN_PARENT_BOTTOM, RL_CENTER_IN_PARENT, RL_CENTER_HORIZONTAL,
   RL_CENTER_VERTICAL, RL_START_OF, RL_END_OF, RL_ALIGN_START, RL_ALIGN_END,
   RL_ALIGN_PARENT_START, RL_ALIGN_PARENT_END, RL_VERB_COUNT
};

/* A view's LayoutParams, whichever way it got them: set in code (a
 * LayoutParams object) or inflated from XML (fields on the view). */
struct ui_lp {
   int32_t w, h, ml, mt, mr, mb;
   float weight;
   int32_t gravity;                 /* layout_gravity; -1 when unset */
   int32_t rules[RL_VERB_COUNT];
};

/* Where the pass put a view: measured size, and position relative to its
 * parent's top-left. */
struct ui_node {
   dvm_ref view;
   int32_t w, h, x, y;
   /* RelativeLayout's working edges (mLeft/mRight/mTop/mBottom); -1 unset. */
   int32_t l, r, t, b;
};

/* Unscoped framework changes conservatively invalidate all measurements.
 * requestLayout marks just the affected branch instead. */
static uint64_t g_ui_measure_epoch=1;
static struct ui_node *g_ui_nodes;
/* Slots in the per-view layout table (a power of two: it is probed by hash).
 * A Material home screen is several thousand views, and nodes are handed out
 * as pointers during a pass, so the table is sized once rather than grown. */
#define UI_NODE_CAP (1u << 16)
static uint32_t g_ui_node_cap;

static void ui_nodes_reset(void)
{
   if (g_ui_nodes) memset(g_ui_nodes, 0, g_ui_node_cap * sizeof *g_ui_nodes);
}

static struct ui_node *ui_node(struct dvm *vm, dvm_ref view)
{
   if (!view) return NULL;
   if (!g_ui_nodes) {
      g_ui_node_cap = UI_NODE_CAP;
      g_ui_nodes = calloc(g_ui_node_cap, sizeof *g_ui_nodes);
      if (!g_ui_nodes) return NULL;
   }
   uint32_t mask = g_ui_node_cap - 1, i = ((uint32_t)view * 2654435761u) & mask;
   for (uint32_t probe = 0; probe < g_ui_node_cap; ++probe, i = (i + 1) & mask) {
      if (g_ui_nodes[i].view == view) return &g_ui_nodes[i];
      if (!g_ui_nodes[i].view) {
         g_ui_nodes[i].view = view;
         /* Measurement and layout live on View, not in this redraw cache.
          * A guest layout manager can reuse its earlier results on a pass. */
         struct ui_node *node = &g_ui_nodes[i];
         node->x = rt_get_int(vm, view, "left");
         node->y = rt_get_int(vm, view, "top");
         node->w = (int32_t)((uint32_t)rt_get_int(vm, view, "right") - (uint32_t)node->x);
         node->h = (int32_t)((uint32_t)rt_get_int(vm, view, "bottom") - (uint32_t)node->y);
         g_ui_nodes[i].l = g_ui_nodes[i].r = g_ui_nodes[i].t = g_ui_nodes[i].b = -1;
         return &g_ui_nodes[i];
      }
   }
   return NULL;                     /* a tree that fills the table: not a window */
}

/* Update the measured node used by the presentation layer without measuring
 * the tree again. A view not yet measured will be positioned by its first layout. */
static void ui_offset_position(dvm_ref view, int32_t offset, bool vertical)
{
   if (!g_ui_nodes || !view) return;
   uint32_t mask = g_ui_node_cap - 1, i = ((uint32_t)view * 2654435761u) & mask;
   for (uint32_t probe = 0; probe < g_ui_node_cap; ++probe, i = (i + 1) & mask) {
      struct ui_node *n = &g_ui_nodes[i];
      if (!n->view) return;
      if (n->view != view) continue;
      int32_t *position = vertical ? &n->y : &n->x;
      *position = (int32_t)((uint32_t)*position + (uint32_t)offset);
      return;
   }
}

static struct ui_ms ui_ms_make(int mode, int32_t size)
{
   struct ui_ms s = { mode, size < 0 ? 0 : size };
   return s;
}

/* ViewGroup.getChildMeasureSpec(). */
static struct ui_ms ui_child_spec(struct ui_ms spec, int32_t padding, int32_t dim)
{
   int32_t size = (int32_t)((uint32_t)spec.size - (uint32_t)padding);
   if (size < 0) size = 0;
   if (dim >= 0) return ui_ms_make(UI_MS_EXACT, dim);
   switch (spec.mode) {
   case UI_MS_EXACT:
      return ui_ms_make(dim == -1 ? UI_MS_EXACT : UI_MS_ATMOST, size);
   case UI_MS_ATMOST:
      return ui_ms_make(UI_MS_ATMOST, size);
   default:
      return ui_ms_make(UI_MS_UNSPEC, size);
   }
}

static bool vg_child_measure_spec(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   uint32_t bits = (uint32_t)ARG(0).i;
   const int mode = (int)(bits >> 30);
   const int32_t dim = ARG(2).i;
   /* Unknown modes or layout dimensions leave the AOSP result at zero. */
   if (mode == 3 || dim < -2) RETI(0);
   struct ui_ms spec = ui_child_spec(
      ui_ms_make(mode, (int32_t)(bits & 0x3fffffffu)), ARG(1).i, dim);
   RETI((int32_t)(((uint32_t)spec.size & 0x3fffffffu) | ((uint32_t)spec.mode << 30)));
}

/* View.resolveSize(). */
static int32_t ui_resolve(int32_t desired, struct ui_ms spec)
{
   if (spec.mode == UI_MS_EXACT) return spec.size;
   if (spec.mode == UI_MS_ATMOST) return desired < spec.size ? desired : spec.size;
   return desired;
}

static void ui_read_lp(struct dvm *vm, dvm_ref view, struct ui_lp *lp)
{
   memset(lp, 0, sizeof *lp);
   lp->gravity = -1;
   dvm_ref o = ui_obj(vm, view, "layoutParams",
                      "Landroid/view/ViewGroup$LayoutParams;");
   if (o) {
      lp->w = ui_int(vm, o, "width");
      lp->h = ui_int(vm, o, "height");
      lp->ml = ui_int(vm, o, "leftMargin");
      lp->mt = ui_int(vm, o, "topMargin");
      lp->mr = ui_int(vm, o, "rightMargin");
      lp->mb = ui_int(vm, o, "bottomMargin");
      lp->weight = ui_float(vm, o, "weight");
      union dvm_value g = { 0 };
      if (dvm_get_field(vm, o, "gravity", "I", &g) && g.i) lp->gravity = g.i;
      dvm_ref rules = ui_obj(vm, o, "rules", "[I");
      struct dvm_object *ra = rules ? dvm__obj(vm, rules) : NULL;
      if (ra)
         for (uint32_t i = 0; i < ra->length && i < RL_VERB_COUNT; ++i)
            lp->rules[i] = ((int32_t *)ra->data)[i];
   } else {
      /* 0 is "the layout never said": WRAP_CONTENT, as a LayoutParams the
       * inflater had nothing for starts. */
      lp->w = ui_int(vm, view, "layoutWidth");
      lp->h = ui_int(vm, view, "layoutHeight");
      if (!lp->w) lp->w = -2;
      if (!lp->h) lp->h = -2;
      lp->ml = ui_int(vm, view, "marginLeft");
      lp->mt = ui_int(vm, view, "marginTop");
      lp->mr = ui_int(vm, view, "marginRight");
      lp->mb = ui_int(vm, view, "marginBottom");
      lp->weight = ui_float(vm, view, "layoutWeight");
      const int32_t g = ui_int(vm, view, "layoutGravity");
      if (g) lp->gravity = g;
   }
   /* The XML forms of the RelativeLayout rules live on the view; a rule set
    * in code wins where both exist. */
   static const struct { const char *field; int verb; bool is_bool; } xr[] = {
      { "alignParentLeft", RL_ALIGN_PARENT_LEFT, true },
      { "alignParentTop", RL_ALIGN_PARENT_TOP, true },
      { "alignParentRight", RL_ALIGN_PARENT_RIGHT, true },
      { "alignParentBottom", RL_ALIGN_PARENT_BOTTOM, true },
      { "centerInParent", RL_CENTER_IN_PARENT, true },
      { "centerHorizontal", RL_CENTER_HORIZONTAL, true },
      { "centerVertical", RL_CENTER_VERTICAL, true },
      { "alignLeft", RL_ALIGN_LEFT, false },
      { "alignTop", RL_ALIGN_TOP, false },
      { "alignRight", RL_ALIGN_RIGHT, false },
      { "alignBottom", RL_ALIGN_BOTTOM, false },
      { "ruleLeftOf", RL_LEFT_OF, false },
      { "ruleRightOf", RL_RIGHT_OF, false },
      { "ruleAbove", RL_ABOVE, false },
      { "ruleBelow", RL_BELOW, false },
   };
   for (size_t i = 0; i < sizeof xr / sizeof xr[0]; ++i) {
      if (lp->rules[xr[i].verb]) continue;
      const int32_t v = ui_int(vm, view, xr[i].field);
      if (v) lp->rules[xr[i].verb] = xr[i].is_bool ? -1 : v;
   }
   /* Start/end are left/right on a left-to-right layout, unless the
    * left/right form was given as well (RelativeLayout.resolveRules). */
   static const int rel[][2] = {
      { RL_START_OF, RL_LEFT_OF }, { RL_END_OF, RL_RIGHT_OF },
      { RL_ALIGN_START, RL_ALIGN_LEFT }, { RL_ALIGN_END, RL_ALIGN_RIGHT },
      { RL_ALIGN_PARENT_START, RL_ALIGN_PARENT_LEFT },
      { RL_ALIGN_PARENT_END, RL_ALIGN_PARENT_RIGHT },
   };
   for (size_t i = 0; i < sizeof rel / sizeof rel[0]; ++i)
      if (lp->rules[rel[i][0]] && !lp->rules[rel[i][1]])
         lp->rules[rel[i][1]] = lp->rules[rel[i][0]];
}

/* The actual children attached to a ViewGroup. */
static uint32_t ui_children(struct dvm *vm, dvm_ref view, dvm_ref *out,
                            uint32_t max)
{
   uint32_t n = 0;
   dvm_ref kids = ui_obj(vm, view, "children", "Ljava/util/List;");
   struct rt_list *l = kids ? list_of(vm, kids) : NULL;
   for (uint32_t i = 0; l && i < l->size && n < max; ++i)
      if (l->items[i]) out[n++] = l->items[i];
   return n;
}

static bool ui_gone(struct dvm *vm, dvm_ref v)
{
   return ui_int(vm, v, "visibility") == 8;
}

static void ui_pad(struct dvm *vm, dvm_ref v, int32_t p[4])
{
   p[0] = ui_int(vm, v, "paddingLeft");
   p[1] = ui_int(vm, v, "paddingTop");
   p[2] = ui_int(vm, v, "paddingRight");
   p[3] = ui_int(vm, v, "paddingBottom");
}

#define UI_MAX_KIDS 256

/* LayoutParams subclasses resolve their own constraints; the framework must
 * dispatch this callback before an onMeasure implementation consumes them. */
static bool view_resolve_layout_params(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref params = ui_obj(vm, self, "layoutParams", "Landroid/view/ViewGroup$LayoutParams;");
   if (!params) RETV();
   struct dvm_class *c = dvm_object_class(vm, self);
   struct dvm_method *direction = c ? dvm_find_method(vm, c, "getLayoutDirection", "()I") : NULL;
   union dvm_value value = {0}, ignored = {0};
   if (direction && !dvm_call(vm, direction, self, NULL, 0, &value)) return false;
   c = dvm_object_class(vm, params);
   struct dvm_method *resolve = c ? dvm_find_method(vm, c, "resolveLayoutDirection", "(I)V") : NULL;
   if (resolve && !dvm_call(vm, resolve, params, &value, 1, &ignored)) return false;
   RETV();
}

static bool vg_resolve_layout_params(struct dvm *vm, dvm_ref self,
   const union dvm_value *args, int nargs, union dvm_value *out)
{
   if (!view_resolve_layout_params(vm, self, args, nargs, out)) return false;
   dvm_ref kids[UI_MAX_KIDS];
   uint32_t count = ui_children(vm, self, kids, UI_MAX_KIDS);
   for (uint32_t i = 0; i < count; ++i) {
      struct dvm_class *c = dvm_object_class(vm, kids[i]);
      struct dvm_method *resolve = c ? dvm_find_method(vm, c, "resolveLayoutParams", "()V") : NULL;
      union dvm_value ignored = {0};
      if (resolve && !dvm_call(vm, resolve, kids[i], NULL, 0, &ignored)) return false;
   }
   RETV();
}

static void ui_measure(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                       struct ui_ms hs, unsigned depth);

/* The picture a view shows by itself, for wrap_content: an ImageView's
 * source, or a background given as a bitmap. */
static bool ui_intrinsic_image(struct dvm *vm, dvm_ref view, int32_t *w, int32_t *h)
{
   const char *src = ui_drawable_src(vm, ui_int(vm, view, "imageResource"));
   if (!src) {
      dvm_ref d = ui_drawable_at_rest(vm, ui_obj(vm, view, "drawable",
                                      "Landroid/graphics/drawable/Drawable;"));
      dvm_ref bm = d ? ui_obj(vm, d, "bitmap", "Landroid/graphics/Bitmap;") : 0;
      if (bm) src = bitmap_png_path(vm, bm);
   }
   if (src && ui_png_size(src, w, h)) return true;
   /* A vector or state-list source: not rasterised here, but on a device it
    * has the intrinsic size of a Material icon. */
   if (ui_int(vm, view, "imageResource") > 0) { *w = *h = 24; return true; }
   return false;
}

struct rt_text_units { const uint16_t *data; uint16_t *owned; size_t length; };
struct textview_line {
   size_t start,end;
   struct luna_text_run run;
   float left,top,bottom,baseline,descent,extra;
   int direction;
};
struct textview_layout {
   struct rt_text_units text;
   struct textview_line *lines;
   size_t count;
   dvm_ref source,paint,face,direction,draw_units;
   int source_start,source_end,paragraph_direction;
   float px,scale,height,widest,top_padding,bottom_padding,spacing_mult,spacing_add;
   int width,max_lines;
   bool bold,single,padding,fallback_spacing;
};
static bool text_layout_direction(struct dvm *,struct textview_layout *,size_t,size_t);
static struct textview_layout *textview_layout_get(struct dvm *,dvm_ref,int);

/* The widest hard line of a TextView's text (its hint when empty) and how many
 * hard lines there are, in the overlay's font. */
static void ui_text_extent(struct dvm *vm, dvm_ref view, float *width, int *hard_lines)
{
   struct textview_layout *layout=textview_layout_get(vm,view,INT_MAX);
   *width=layout ? layout->widest : 0;
   *hard_lines=layout ? (int)layout->count : 1;
}

/* The drawables a TextView carries beside its text (android:drawableStart, a
 * check box's picture) and the room they take: width plus drawablePadding.
 * Layout is left-to-right here; start/end stand in for left/right. */
struct text_compound {dvm_ref left,right;int left_w,left_h,right_w,right_h,left_inset,right_inset;};
static void text_compound_get(struct dvm *vm,dvm_ref view,struct text_compound *c)
{
   memset(c,0,sizeof *c);
   const char *const names[2][3]={{"buttonDrawable","drawableLeft","drawableStart"},{"drawableRight","drawableEnd",NULL}};
   dvm_ref found[2]={0,0};
   for(int side=0;side<2;++side) for(int k=0;k<3 && names[side][k] && !found[side];++k) {
      union dvm_value v={0};
      if(dvm_get_field(vm,view,names[side][k],"Landroid/graphics/drawable/Drawable;",&v) && v.l) found[side]=v.l;
   }
   const int padding=ui_int(vm,view,"drawablePadding");
   for(int side=0;side<2;++side) {
      if(!found[side]) continue;
      union dvm_value w={0},h={0};
      if(!am_vcall(vm,found[side],"getIntrinsicWidth","()I",NULL,0,&w) ||
         !am_vcall(vm,found[side],"getIntrinsicHeight","()I",NULL,0,&h)) {dvm_clear_exception(vm);continue;}
      if(w.i<=0 || h.i<=0) continue;
      if(side==0) {c->left=found[0];c->left_w=w.i;c->left_h=h.i;c->left_inset=w.i+padding;}
      else {c->right=found[1];c->right_w=w.i;c->right_h=h.i;c->right_inset=w.i+padding;}
   }
}

static struct textview_layout *textview_current_layout(struct dvm *vm,dvm_ref view)
{
   const struct ui_node *n=ui_node(vm,view);int32_t pad[4];ui_pad(vm,view,pad);
   struct text_compound compound;text_compound_get(vm,view,&compound);
   int width=(n ? n->w : ui_int(vm,view,"right")-ui_int(vm,view,"left"))-pad[0]-pad[2]-compound.left_inset-compound.right_inset;
   return textview_layout_get(vm,view,width>0 ? width : INT_MAX);
}

/* android.text.Layout as a TextView reports it after measuring: the view's
 * text laid out in the width it was given.  Each answer is derived from that
 * measurement (ui_text_extent), so it agrees with what the overlay draws. */
static dvm_ref layout_view(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "view", "Landroid/widget/TextView;", &v);
   return v.l;
}

/* Both TextView's cached layouts and independently built layouts own the
 * same native shaped-line data. Only the former refresh from a View. */
static struct textview_layout *layout_data(struct dvm *vm,dvm_ref self)
{
   dvm_ref view=layout_view(vm,self);
   if(view) return textview_current_layout(vm,view);
   struct dvm_object *object=dvm__obj(vm,self);
   return object ? object->data : NULL;
}

static bool layout_line_count(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct textview_layout *layout=layout_data(vm,self);
   RETI(layout ? (int32_t)layout->count : 0);
}

static float layout_line_height(struct dvm *vm, dvm_ref view)
{
   return luna_overlay_line_height(text_size_px(vm, view));
}

static bool layout_get_height(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   struct textview_layout *layout=layout_data(vm,self);
   RETI(layout ? (int32_t)ceilf(layout->height) : 0);
}

static bool layout_get_width(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct textview_layout *layout=layout_data(vm,self);
   RETI(layout ? layout->width : 0);
}

/* getLineTop/getLineBottom/getLineBaseline(line): lines are stacked at the
 * font's line height. */
static bool layout_line_edge(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name=builtin_name();
   struct textview_layout *layout=layout_data(vm,self);
   if(!layout) return false;
   int64_t line=ARG(0).i;
   if(line<0 || line>(int64_t)layout->count || (strcmp(name,"getLineTop") && line==(int64_t)layout->count)) {
      dvm__throw(vm,"java/lang/IndexOutOfBoundsException","line");return false;
   }
   if(line==(int64_t)layout->count) RETI((int32_t)layout->height);
   struct textview_line *entry=&layout->lines[line];
   if(!strcmp(name,"getLineBaseline")) RETI((int32_t)entry->baseline);
   if(!strcmp(name,"getLineDescent")) RETI((int32_t)entry->descent);
   if(!strcmp(name,"getLineExtra")) RETI((int32_t)entry->extra);
   if(!strcmp(name,"getLineBottom")) RETI((int32_t)entry->bottom);
   RETI((int32_t)entry->top);
}

static bool layout_padding(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                           int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   struct textview_layout *layout=layout_data(vm,self);
   RETI(layout ? (int32_t)(!strcmp(builtin_name(),"getTopPadding") ? layout->top_padding : layout->bottom_padding) : 0);
}

static bool layout_ellipsis_count(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(0); }

static bool layout_line_width(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct textview_layout *layout=layout_data(vm,self);
   if(!layout) return false;
   int line=ARG(0).i;
   if(line<0 || (size_t)line>=layout->count) {dvm__throw(vm,"java/lang/IndexOutOfBoundsException","line");return false;}
   RETF(layout->lines[line].run.width*layout->scale);
}

static bool layout_get_text(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref view = layout_view(vm, self);
   union dvm_value text = { 0 };
   if (view) (void)dvm_get_field(vm, view, "text", "Ljava/lang/CharSequence;", &text);
   else (void)dvm_get_field(vm,self,"source","Ljava/lang/CharSequence;",&text);
   RETL(text.l);
}

static bool layout_line_position(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   struct textview_layout *layout=layout_data(vm,self);
   int line=ARG(0).i;
   if(!layout || line<0 || (size_t)line>=layout->count) {dvm__throw(vm,"java/lang/IndexOutOfBoundsException","line");return false;}
   struct textview_line *entry=&layout->lines[line];
   const char *name=builtin_name();
   if(!strcmp(name,"getLineStart")) RETI(layout->source_start+entry->start);
   if(!strcmp(name,"getLineEnd")) RETI(layout->source_start+entry->end);
   if(!strcmp(name,"getParagraphDirection")) RETI(entry->direction==2 ? -1 : 1);
   if(!strcmp(name,"getLineRight")) RETF(entry->left+entry->run.width*layout->scale);
   RETF(entry->left);
}
static bool layout_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   const char *name=builtin_name();union dvm_value value={0};
   if(!strcmp(name,"getAlignment")) {
      dvm_get_field(vm,self,"alignment","Landroid/text/Layout$Alignment;",&value);
      if(!value.l) dvm_get_static(vm,dvm_find_class(vm,"android/text/Layout$Alignment"),"ALIGN_NORMAL","Landroid/text/Layout$Alignment;",&value);
   } else {
      struct textview_layout *layout=layout_data(vm,self);
      value.l=layout ? layout->paint : 0;
   }
   RETL(value.l);
}
static bool layout_draw(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static bool layout_desired_width(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);

static const struct rt_method rt_text_layout[] = {
   SM("getDesiredWidth","(Ljava/lang/CharSequence;Landroid/text/TextPaint;)F",layout_desired_width),
   SM("getDesiredWidth","(Ljava/lang/CharSequence;IILandroid/text/TextPaint;)F",layout_desired_width),
   SM("getDesiredWidth","(Ljava/lang/CharSequence;IILandroid/text/TextPaint;Landroid/text/TextDirectionHeuristic;)F",layout_desired_width),
   SM("getDesiredWidthWithLimit","(Ljava/lang/CharSequence;IILandroid/text/TextPaint;Landroid/text/TextDirectionHeuristic;F)F",layout_desired_width),
   M("draw","(Landroid/graphics/Canvas;)V",layout_draw),
   M("getPaint","()Landroid/text/TextPaint;",layout_property),
   M("getAlignment","()Landroid/text/Layout$Alignment;",layout_property),
   M("getLineStart","(I)I",layout_line_position),M("getLineEnd","(I)I",layout_line_position),
   M("getParagraphDirection","(I)I",layout_line_position),
   M("getLineLeft","(I)F",layout_line_position),M("getLineRight","(I)F",layout_line_position),
   M("getLineCount", "()I", layout_line_count),
   M("getHeight", "()I", layout_get_height),
   M("getWidth", "()I", layout_get_width),
   M("getLineTop", "(I)I", layout_line_edge),
   M("getLineBottom", "(I)I", layout_line_edge),
   M("getLineBaseline", "(I)I", layout_line_edge),
   M("getLineDescent", "(I)I", layout_line_edge),M("getLineExtra", "(I)I", layout_line_edge),
   M("getTopPadding", "()I", layout_padding),M("getBottomPadding", "()I", layout_padding),
   M("getEllipsisCount", "(I)I", layout_ellipsis_count),
   M("getLineWidth", "(I)F", layout_line_width),
   M("getText", "()Ljava/lang/CharSequence;", layout_get_text),
   M_END,
};
static const struct rt_field rt_text_layout_fields[] = {
   { "view", "Landroid/widget/TextView;" }, {"source","Ljava/lang/CharSequence;"}, {"paint","Landroid/text/TextPaint;"},
   {"alignment","Landroid/text/Layout$Alignment;"},{"direction","Landroid/text/TextDirectionHeuristic;"}, F_END
};

/* TextView.getLayout(): null until the view has been measured, as on a device. */
static bool text_get_layout(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct ui_node *n = ui_node(vm, self);
   if (!n || (n->w <= 0 && n->h <= 0)) RETL(0);
   if(!textview_current_layout(vm,self)) return false;
   RETL(ui_obj(vm,self,"textLayout","Landroid/text/Layout;"));
}

/* TextView.getLineCount(): the hard lines, each wrapped inside the width the
 * view was given, limited by maxLines / singleLine. */
static bool text_get_line_count(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   struct textview_layout *layout=textview_current_layout(vm,self);
   if(!layout) return false;
   RETI((int)layout->count);
}

static void ui_measure_leaf(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                            struct ui_ms hs, struct ui_node *n)
{
   int32_t pad[4];
   ui_pad(vm, view, pad);
   const bool is_text = ui_is_a(vm, view, "android/widget/TextView");
   const bool is_button = ui_is_a(vm, view, "android/widget/Button");
   const bool is_edit = ui_is_a(vm, view, "android/widget/EditText");
   int32_t want_w = pad[0] + pad[2], want_h = pad[1] + pad[3];
   if (is_text) {
      float tw = 0.f;
      int hard_lines = 0;
      ui_text_extent(vm, view, &tw, &hard_lines);
      /* The picture beside the text (a check box's button, drawableStart)
       * takes its width from the line and may be taller than it. */
      struct text_compound compound;
      text_compound_get(vm, view, &compound);
      const int32_t beside = compound.left_inset + compound.right_inset;
      int32_t w;
      if (ws.mode == UI_MS_EXACT) w = ws.size;
      else {
         w = (int32_t)ceilf(tw) + pad[0] + pad[2] + beside;
         int32_t min_w = view_suggested_minimum(vm, view, false);
         if (w < min_w) w = min_w;
         if (is_button && w < 88) w = 88;         /* Widget.Button minWidth */
         if (ws.mode == UI_MS_ATMOST && w > ws.size) w = ws.size;
      }
      /* Soft wrapping inside what is left of the width. */
      const int32_t avail = w - pad[0] - pad[2] - beside;
      struct textview_layout *layout=textview_layout_get(vm,view,avail>0 ? avail : INT_MAX);
      if(!layout) return;
      want_h = (int32_t)ceilf(layout->height);
      if (want_h < compound.left_h) want_h = compound.left_h;
      if (want_h < compound.right_h) want_h = compound.right_h;
      want_h += pad[1] + pad[3];
      if (is_button && want_h < 48) want_h = 48;   /* Widget.Button minHeight */
      int32_t min_h = view_suggested_minimum(vm, view, true);
      if (want_h < min_h) want_h = min_h;
      n->w = w;
      n->h = ui_resolve(want_h, hs);
      return;
   }
   int32_t iw = 0, ih = 0;
   if (ui_intrinsic_image(vm, view, &iw, &ih)) {
      want_w += iw;
      want_h += ih;
   } else if (ui_is_a(vm, view, "android/widget/ImageButton")) {
      want_w += 24; want_h += 24;
   }
   int32_t min_w = view_suggested_minimum(vm, view, false);
   int32_t min_h = view_suggested_minimum(vm, view, true);
   if (want_w < min_w) want_w = min_w;
   if (want_h < min_h) want_h = min_h;
   /* View.getDefaultSize(): a plain view takes whatever it is offered. */
   if (!ui_is_a(vm, view, "android/widget/ImageView")) {
      n->w = ws.mode == UI_MS_UNSPEC ? want_w : ws.size;
      n->h = hs.mode == UI_MS_UNSPEC ? want_h : hs.size;
      return;
   }
   n->w = ui_resolve(want_w, ws);
   n->h = ui_resolve(want_h, hs);
}

/* LinearLayout.measureVertical / measureHorizontal. */
static void ui_measure_linear(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                              struct ui_ms hs, struct ui_node *n, unsigned depth)
{
   const bool vertical = ui_int(vm, view, "orientation") == 1;
   const struct ui_ms main_s = vertical ? hs : ws, cross_s = vertical ? ws : hs;
   int32_t pad[4];
   ui_pad(vm, view, pad);
   const int32_t main_pad = vertical ? pad[1] + pad[3] : pad[0] + pad[2];
   const int32_t cross_pad = vertical ? pad[0] + pad[2] : pad[1] + pad[3];
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, view, kids, UI_MAX_KIDS);
   struct ui_lp lps[UI_MAX_KIDS];
   bool skipped[UI_MAX_KIDS] = { false };
   int32_t total = 0, max_cross = 0;
   float total_weight = 0.f;
   bool match_cross = false;
   for (uint32_t i = 0; i < nk; ++i) {
      if (ui_gone(vm, kids[i])) continue;
      struct ui_lp *lp = &lps[i];
      ui_read_lp(vm, kids[i], lp);
      const int32_t lmain = vertical ? lp->h : lp->w;
      const int32_t lcross = vertical ? lp->w : lp->h;
      const int32_t mmain = vertical ? lp->mt + lp->mb : lp->ml + lp->mr;
      const int32_t mcross = vertical ? lp->ml + lp->mr : lp->mt + lp->mb;
      total_weight += lp->weight;
      if (main_s.mode == UI_MS_EXACT && lmain == 0 && lp->weight > 0.f) {
         skipped[i] = true;
         total += mmain;
      } else {
         const int32_t used = total_weight == 0.f ? total : 0;
         struct ui_ms cm = ui_child_spec(main_s, main_pad + mmain + used,
                                         lmain == 0 && lp->weight > 0.f ? -2 : lmain);
         struct ui_ms cc = ui_child_spec(cross_s, cross_pad + mcross, lcross);
         if (vertical) ui_measure(vm, kids[i], cc, cm, depth + 1);
         else ui_measure(vm, kids[i], cm, cc, depth + 1);
         struct ui_node *c = ui_node(vm, kids[i]);
         total += (vertical ? c->h : c->w) + mmain;
      }
      if (cross_s.mode != UI_MS_EXACT && lcross == -1) match_cross = true;
      struct ui_node *c = ui_node(vm, kids[i]);
      const int32_t cw = skipped[i] ? 0 : (vertical ? c->w : c->h);
      if (cw + mcross > max_cross) max_cross = cw + mcross;
   }
   total += main_pad;
   int32_t min_main = view_suggested_minimum(vm, view, vertical);
   const int32_t main_size = ui_resolve(total > min_main ? total : min_main, main_s);
   int32_t delta = main_size - total;
   if (total_weight > 0.f && (delta != 0 || main_s.mode == UI_MS_EXACT)) {
      float weight_sum = total_weight;
      total = 0;
      max_cross = 0;
      for (uint32_t i = 0; i < nk; ++i) {
         if (ui_gone(vm, kids[i])) continue;
         struct ui_lp *lp = &lps[i];
         const int32_t lcross = vertical ? lp->w : lp->h;
         const int32_t mmain = vertical ? lp->mt + lp->mb : lp->ml + lp->mr;
         const int32_t mcross = vertical ? lp->ml + lp->mr : lp->mt + lp->mb;
         struct ui_node *c = ui_node(vm, kids[i]);
         if (lp->weight > 0.f) {
            const int32_t share = (int32_t)((float)delta * lp->weight / weight_sum);
            weight_sum -= lp->weight;
            delta -= share;
            int32_t size = skipped[i] ? share
                         : (vertical ? c->h : c->w) + share;
            if (size < 0) size = 0;
            struct ui_ms cm = ui_ms_make(UI_MS_EXACT, size);
            struct ui_ms cc = ui_child_spec(cross_s, cross_pad + mcross, lcross);
            if (vertical) ui_measure(vm, kids[i], cc, cm, depth + 1);
            else ui_measure(vm, kids[i], cm, cc, depth + 1);
         }
         total += (vertical ? c->h : c->w) + mmain;
         const int32_t cw = vertical ? c->w : c->h;
         if (cw + mcross > max_cross) max_cross = cw + mcross;
      }
      total += main_pad;
   }
   int32_t cross_want = max_cross + cross_pad;
   int32_t min_cross = view_suggested_minimum(vm, view, !vertical);
   const int32_t cross_size = ui_resolve(cross_want > min_cross ? cross_want : min_cross, cross_s);
   /* forceUniformWidth/Height: MATCH_PARENT children of a wrapping layout
    * take the size the layout settled on. */
   if (match_cross) {
      for (uint32_t i = 0; i < nk; ++i) {
         if (ui_gone(vm, kids[i])) continue;
         struct ui_lp *lp = &lps[i];
         if ((vertical ? lp->w : lp->h) != -1) continue;
         struct ui_node *c = ui_node(vm, kids[i]);
         const int32_t mcross = vertical ? lp->ml + lp->mr : lp->mt + lp->mb;
         struct ui_ms cc = ui_ms_make(UI_MS_EXACT, cross_size - cross_pad - mcross);
         struct ui_ms cm = ui_ms_make(UI_MS_EXACT, vertical ? c->h : c->w);
         if (vertical) ui_measure(vm, kids[i], cc, cm, depth + 1);
         else ui_measure(vm, kids[i], cm, cc, depth + 1);
      }
   }
   n->w = vertical ? cross_size : main_size;
   n->h = vertical ? main_size : cross_size;
   (void)total;
}

/* LinearLayout.layoutVertical / layoutHorizontal. */
static void ui_layout_linear(struct dvm *vm, dvm_ref view, struct ui_node *n)
{
   const bool vertical = ui_int(vm, view, "orientation") == 1;
   int32_t pad[4];
   ui_pad(vm, view, pad);
   const int32_t gravity = ui_int(vm, view, "gravity");
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, view, kids, UI_MAX_KIDS);
   int32_t used = 0;
   for (uint32_t i = 0; i < nk; ++i) {
      if (ui_gone(vm, kids[i])) continue;
      struct ui_lp lp;
      ui_read_lp(vm, kids[i], &lp);
      struct ui_node *c = ui_node(vm, kids[i]);
      used += vertical ? c->h + lp.mt + lp.mb : c->w + lp.ml + lp.mr;
   }
   const int32_t main_space = vertical ? n->h - pad[1] - pad[3] : n->w - pad[0] - pad[2];
   int32_t pos = vertical ? pad[1] : pad[0];
   const int32_t main_g = vertical ? (gravity & 0x70) : (gravity & 0x07);
   if (main_g == (vertical ? UI_GRAVITY_CENTER_V : UI_GRAVITY_CENTER_H))
      pos += (main_space - used) / 2;
   else if (main_g == (vertical ? UI_GRAVITY_BOTTOM : UI_GRAVITY_RIGHT))
      pos += main_space - used;
   for (uint32_t i = 0; i < nk; ++i) {
      if (ui_gone(vm, kids[i])) continue;
      struct ui_lp lp;
      ui_read_lp(vm, kids[i], &lp);
      struct ui_node *c = ui_node(vm, kids[i]);
      int32_t g = lp.gravity >= 0 ? lp.gravity : gravity;
      if (vertical) {
         const int32_t space = n->w - pad[0] - pad[2];
         const int32_t hg = g & 0x07;
         c->x = hg == UI_GRAVITY_CENTER_H
                   ? pad[0] + (space - c->w) / 2 + lp.ml - lp.mr
              : hg == UI_GRAVITY_RIGHT ? n->w - pad[2] - c->w - lp.mr
              : pad[0] + lp.ml;
         pos += lp.mt;
         c->y = pos;
         pos += c->h + lp.mb;
      } else {
         const int32_t space = n->h - pad[1] - pad[3];
         const int32_t vg = g & 0x70;
         c->y = vg == UI_GRAVITY_CENTER_V
                   ? pad[1] + (space - c->h) / 2 + lp.mt - lp.mb
              : vg == UI_GRAVITY_BOTTOM ? n->h - pad[3] - c->h - lp.mb
              : pad[1] + lp.mt;
         pos += lp.ml;
         c->x = pos;
         pos += c->w + lp.mr;
      }
   }
}

/* FrameLayout.onMeasure, and ScrollView's (its child is unbounded along the
 * scroll axis). */
static void ui_measure_frame(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                             struct ui_ms hs, struct ui_node *n, unsigned depth)
{
   const bool vscroll = ui_is_a(vm, view, "android/widget/ScrollView") ||
                        ui_is_a(vm, view, "androidx/core/widget/NestedScrollView");
   const bool hscroll = ui_is_a(vm, view, "android/widget/HorizontalScrollView");
   int32_t pad[4];
   ui_pad(vm, view, pad);
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, view, kids, UI_MAX_KIDS);
   int32_t mw = 0, mh = 0;
   uint32_t match_count = 0;
   for (uint32_t i = 0; i < nk; ++i) {
      if (ui_gone(vm, kids[i])) continue;
      struct ui_lp lp;
      ui_read_lp(vm, kids[i], &lp);
      if (lp.w == -1 || lp.h == -1) ++match_count;
      struct ui_ms cw = ui_child_spec(ws, pad[0] + pad[2] + lp.ml + lp.mr, lp.w);
      struct ui_ms ch = ui_child_spec(hs, pad[1] + pad[3] + lp.mt + lp.mb, lp.h);
      if (vscroll) ch = ui_ms_make(UI_MS_UNSPEC, 0);
      if (hscroll) cw = ui_ms_make(UI_MS_UNSPEC, 0);
      ui_measure(vm, kids[i], cw, ch, depth + 1);
      struct ui_node *c = ui_node(vm, kids[i]);
      if (c->w + lp.ml + lp.mr > mw) mw = c->w + lp.ml + lp.mr;
      if (c->h + lp.mt + lp.mb > mh) mh = c->h + lp.mt + lp.mb;
   }
   mw += pad[0] + pad[2]; mh += pad[1] + pad[3];
   int32_t min_w = view_suggested_minimum(vm, view, false);
   int32_t min_h = view_suggested_minimum(vm, view, true);
   n->w = ui_resolve(mw > min_w ? mw : min_w, ws);
   n->h = ui_resolve(mh > min_h ? mh : min_h, hs);
   /* ScrollView.onMeasure() with fillViewport: content shorter than the
    * viewport is measured again at exactly the viewport's size, so a
    * layout that fills it (a ConstraintLayout laid out against its parent)
    * has something to fill. */
   if ((vscroll || hscroll) && ui_int(vm, view, "fillViewport") &&
       (vscroll ? hs.mode : ws.mode) != UI_MS_UNSPEC) {
      for (uint32_t i = 0; i < nk; ++i) {
         if (ui_gone(vm, kids[i])) continue;
         struct ui_lp lp;
         ui_read_lp(vm, kids[i], &lp);
         struct ui_node *c = ui_node(vm, kids[i]);
         if (vscroll) {
            const int32_t room = n->h - pad[1] - pad[3] - lp.mt - lp.mb;
            if (c->h < room)
               ui_measure(vm, kids[i], ui_ms_make(UI_MS_EXACT, c->w), ui_ms_make(UI_MS_EXACT, room), depth + 1);
         } else {
            const int32_t room = n->w - pad[0] - pad[2] - lp.ml - lp.mr;
            if (c->w < room)
               ui_measure(vm, kids[i], ui_ms_make(UI_MS_EXACT, room), ui_ms_make(UI_MS_EXACT, c->h), depth + 1);
         }
      }
   }
   /* FrameLayout remeasures only when more than one child requested a
    * match-parent axis. Other axes retain the original parent constraint;
    * forcing a wrapping axis exact can consume all space in nested layouts. */
   if ((ws.mode != UI_MS_EXACT || hs.mode != UI_MS_EXACT) && match_count > 1) {
      for (uint32_t i = 0; i < nk; ++i) {
         if (ui_gone(vm, kids[i])) continue;
         struct ui_lp lp;
         ui_read_lp(vm, kids[i], &lp);
         if (lp.w != -1 && lp.h != -1) continue;
         struct ui_ms cw = lp.w == -1
            ? ui_ms_make(UI_MS_EXACT, n->w - pad[0] - pad[2] - lp.ml - lp.mr)
            : ui_child_spec(ws, pad[0] + pad[2] + lp.ml + lp.mr, lp.w);
         struct ui_ms ch = lp.h == -1 && !vscroll
            ? ui_ms_make(UI_MS_EXACT, n->h - pad[1] - pad[3] - lp.mt - lp.mb)
            : vscroll ? ui_ms_make(UI_MS_UNSPEC, 0)
            : ui_child_spec(hs, pad[1] + pad[3] + lp.mt + lp.mb, lp.h);
         ui_measure(vm, kids[i], cw, ch, depth + 1);
      }
   }
}

static void ui_layout_frame(struct dvm *vm, dvm_ref view, struct ui_node *n)
{
   int32_t pad[4];
   ui_pad(vm, view, pad);
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, view, kids, UI_MAX_KIDS);
   for (uint32_t i = 0; i < nk; ++i) {
      if (ui_gone(vm, kids[i])) continue;
      struct ui_lp lp;
      ui_read_lp(vm, kids[i], &lp);
      struct ui_node *c = ui_node(vm, kids[i]);
      const int32_t g = lp.gravity >= 0 ? lp.gravity : 0x33;  /* TOP|START */
      const int32_t sw = n->w - pad[0] - pad[2], sh = n->h - pad[1] - pad[3];
      const int32_t hg = g & 0x07, vg = g & 0x70;
      c->x = hg == UI_GRAVITY_CENTER_H ? pad[0] + (sw - c->w) / 2 + lp.ml - lp.mr
           : hg == UI_GRAVITY_RIGHT ? n->w - pad[2] - c->w - lp.mr
           : pad[0] + lp.ml;
      c->y = vg == UI_GRAVITY_CENTER_V ? pad[1] + (sh - c->h) / 2 + lp.mt - lp.mb
           : vg == UI_GRAVITY_BOTTOM ? n->h - pad[3] - c->h - lp.mb
           : pad[1] + lp.mt;
   }
}

/* The sibling a RelativeLayout rule names, skipping over GONE anchors the
 * way getRelatedView() does. */
static int ui_rl_anchor(struct dvm *vm, dvm_ref *kids, struct ui_lp *lps,
                        uint32_t nk, int32_t id, int verb)
{
   for (int hop = 0; id && id != -1 && hop < 16; ++hop) {
      int found = -1;
      for (uint32_t i = 0; i < nk; ++i)
         if (ui_int(vm, kids[i], "id") == id) { found = (int)i; break; }
      if (found < 0) return -1;
      if (!ui_gone(vm, kids[found])) return found;
      id = lps[found].rules[verb];
   }
   return -1;
}

static int32_t ui_rl_spec_size(int32_t start, int32_t end, int32_t dim,
                               int32_t m_start, int32_t m_end, int32_t pad_start,
                               int32_t pad_end, int32_t my, struct ui_ms *out)
{
   if (my < 0) {
      if (start >= 0 && end >= 0) *out = ui_ms_make(UI_MS_EXACT, end - start);
      else if (dim >= 0) *out = ui_ms_make(UI_MS_EXACT, dim);
      else *out = ui_ms_make(UI_MS_UNSPEC, 0);
      return 0;
   }
   const int32_t ts = start >= 0 ? start : pad_start + m_start;
   const int32_t te = end >= 0 ? end : my - pad_end - m_end;
   const int32_t avail = te - ts;
   if (start >= 0 && end >= 0) *out = ui_ms_make(UI_MS_EXACT, avail);
   else if (dim >= 0) *out = ui_ms_make(UI_MS_EXACT,
                                        avail >= 0 && avail < dim ? avail : dim);
   else if (dim == -1) *out = ui_ms_make(UI_MS_EXACT, avail);
   else *out = avail >= 0 ? ui_ms_make(UI_MS_ATMOST, avail)
                          : ui_ms_make(UI_MS_UNSPEC, 0);
   return 0;
}

/* RelativeLayout.onMeasure, left to right. */
static void ui_measure_relative(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                                struct ui_ms hs, struct ui_node *n, unsigned depth)
{
   int32_t pad[4];
   ui_pad(vm, view, pad);
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, view, kids, UI_MAX_KIDS);
   if (dvm__env("LUNARIA_UI_LAYOUT_TRACE"))
      fprintf(stderr, "[ui-relative] v%u has %u children\n", (unsigned)view, nk);
   struct ui_lp lps[UI_MAX_KIDS];
   const int32_t my_w = ws.mode == UI_MS_UNSPEC ? -1 : ws.size;
   const int32_t my_h = hs.mode == UI_MS_UNSPEC ? -1 : hs.size;
   const bool wrap_w = ws.mode != UI_MS_EXACT, wrap_h = hs.mode != UI_MS_EXACT;
   int32_t width = wrap_w ? 0 : my_w, height = wrap_h ? 0 : my_h;
   for (uint32_t i = 0; i < nk; ++i) {
      ui_read_lp(vm, kids[i], &lps[i]);
      if (dvm__env("LUNARIA_UI_LAYOUT_TRACE"))
         fprintf(stderr, "[ui-relative]   child v%u %s lp=%dx%d parentBottom=%d above=%d below=%d\n",
                 (unsigned)kids[i], ui_class_simple_name(vm, kids[i]), lps[i].w, lps[i].h,
                 lps[i].rules[RL_ALIGN_PARENT_BOTTOM], lps[i].rules[RL_ABOVE], lps[i].rules[RL_BELOW]);
      struct ui_node *c = ui_node(vm, kids[i]);
      c->l = c->r = c->t = c->b = -1;
   }
   /* Dependency order: a child is placed once every sibling its rules name
    * has been.  A cycle (an app error) is broken by placing what is left. */
   bool done_h[UI_MAX_KIDS] = { false }, done_v[UI_MAX_KIDS] = { false };
   static const int hverbs[] = { RL_LEFT_OF, RL_RIGHT_OF, RL_ALIGN_LEFT, RL_ALIGN_RIGHT };
   static const int vverbs[] = { RL_ABOVE, RL_BELOW, RL_ALIGN_TOP, RL_ALIGN_BOTTOM,
                                 RL_ALIGN_BASELINE };
   for (int axis = 0; axis < 2; ++axis) {
      bool *done = axis ? done_v : done_h;
      const int *verbs = axis ? vverbs : hverbs;
      const int nverbs = axis ? 5 : 4;
      for (uint32_t pass = 0; pass <= nk; ++pass) {
         bool progressed = false, left = false;
         for (uint32_t i = 0; i < nk; ++i) {
            if (done[i]) continue;
            if (ui_gone(vm, kids[i])) { done[i] = true; continue; }
            bool ready = true;
            for (int k = 0; k < nverbs && ready; ++k) {
               const int32_t id = lps[i].rules[verbs[k]];
               if (!id || id == -1) continue;
               const int a = ui_rl_anchor(vm, kids, lps, nk, id, verbs[k]);
               if (a >= 0 && !done[a] && (uint32_t)a != i) ready = false;
            }
            if (!ready && pass < nk) { left = true; continue; }
            struct ui_lp *lp = &lps[i];
            struct ui_node *c = ui_node(vm, kids[i]);
            if (!axis) {
               /* applyHorizontalSizeRules */
               int a;
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_LEFT_OF], RL_LEFT_OF)) >= 0)
                  c->r = ui_node(vm, kids[a])->l - (lps[a].ml + lp->mr);
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_RIGHT_OF], RL_RIGHT_OF)) >= 0)
                  c->l = ui_node(vm, kids[a])->r + (lps[a].mr + lp->ml);
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_ALIGN_LEFT], RL_ALIGN_LEFT)) >= 0)
                  c->l = ui_node(vm, kids[a])->l + lp->ml;
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_ALIGN_RIGHT], RL_ALIGN_RIGHT)) >= 0)
                  c->r = ui_node(vm, kids[a])->r - lp->mr;
               if (lp->rules[RL_ALIGN_PARENT_LEFT]) c->l = pad[0] + lp->ml;
               if (lp->rules[RL_ALIGN_PARENT_RIGHT] && my_w >= 0)
                  c->r = my_w - pad[2] - lp->mr;
               /* measureChildHorizontal */
               struct ui_ms cw, ch;
               ui_rl_spec_size(c->l, c->r, lp->w, lp->ml, lp->mr, pad[0], pad[2],
                               my_w, &cw);
               if (my_h < 0)
                  ch = lp->h >= 0 ? ui_ms_make(UI_MS_EXACT, lp->h)
                                  : ui_ms_make(UI_MS_UNSPEC, 0);
               else
                  ch = ui_ms_make(lp->w == -1 ? UI_MS_EXACT : UI_MS_ATMOST,
                                  my_h - pad[1] - pad[3] - lp->mt - lp->mb);
               ui_measure(vm, kids[i], cw, ch, depth + 1);
               /* positionChildHorizontal */
               if (c->l < 0 && c->r >= 0) c->l = c->r - c->w;
               else if (c->l >= 0 && c->r < 0) c->r = c->l + c->w;
               else if (c->l < 0 && c->r < 0) {
                  if ((lp->rules[RL_CENTER_IN_PARENT] ||
                       lp->rules[RL_CENTER_HORIZONTAL]) && !wrap_w) {
                     c->l = (my_w - c->w) / 2;
                     c->r = c->l + c->w;
                  } else {
                     c->l = pad[0] + lp->ml;
                     c->r = c->l + c->w;
                  }
               }
            } else {
               /* applyVerticalSizeRules */
               int a;
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_ABOVE], RL_ABOVE)) >= 0)
                  c->b = ui_node(vm, kids[a])->t - (lps[a].mt + lp->mb);
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_BELOW], RL_BELOW)) >= 0)
                  c->t = ui_node(vm, kids[a])->b + (lps[a].mb + lp->mt);
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_ALIGN_TOP], RL_ALIGN_TOP)) >= 0)
                  c->t = ui_node(vm, kids[a])->t + lp->mt;
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_ALIGN_BOTTOM], RL_ALIGN_BOTTOM)) >= 0)
                  c->b = ui_node(vm, kids[a])->b - lp->mb;
               if ((a = ui_rl_anchor(vm, kids, lps, nk, lp->rules[RL_ALIGN_BASELINE], RL_ALIGN_BASELINE)) >= 0) {
                  /* Baselines are not tracked; same-height rows share a top. */
                  c->t = ui_node(vm, kids[a])->t;
               }
               if (lp->rules[RL_ALIGN_PARENT_TOP]) c->t = pad[1] + lp->mt;
               if (lp->rules[RL_ALIGN_PARENT_BOTTOM] && my_h >= 0)
                  c->b = my_h - pad[3] - lp->mb;
               /* measureChild: the width it settled on horizontally stays. */
               struct ui_ms cw = ui_ms_make(UI_MS_EXACT, c->w), ch;
               ui_rl_spec_size(c->t, c->b, lp->h, lp->mt, lp->mb, pad[1], pad[3],
                               my_h, &ch);
               ui_measure(vm, kids[i], cw, ch, depth + 1);
               if (c->t < 0 && c->b >= 0) c->t = c->b - c->h;
               else if (c->t >= 0 && c->b < 0) c->b = c->t + c->h;
               else if (c->t < 0 && c->b < 0) {
                  if ((lp->rules[RL_CENTER_IN_PARENT] ||
                       lp->rules[RL_CENTER_VERTICAL]) && !wrap_h) {
                     c->t = (my_h - c->h) / 2;
                     c->b = c->t + c->h;
                  } else {
                     c->t = pad[1] + lp->mt;
                     c->b = c->t + c->h;
                  }
               }
               if (wrap_w && c->r + lp->mr > width) width = c->r + lp->mr;
               if (wrap_h && c->b + lp->mb > height) height = c->b + lp->mb;
            }
            done[i] = true;
            progressed = true;
         }
         if (!left) break;
         (void)progressed;
      }
   }
   if (wrap_w) {
      width += pad[2];
      if (my_w >= 0 && width > my_w) width = my_w;
   }
   if (wrap_h) {
      height += pad[3];
      if (my_h >= 0 && height > my_h) height = my_h;
   }
   /* Centring and parent-end rules can only be resolved against the size
    * the layout took when it wrapped. */
   for (uint32_t i = 0; i < nk; ++i) {
      if (ui_gone(vm, kids[i])) continue;
      struct ui_lp *lp = &lps[i];
      struct ui_node *c = ui_node(vm, kids[i]);
      if (wrap_w) {
         if (lp->rules[RL_CENTER_IN_PARENT] || lp->rules[RL_CENTER_HORIZONTAL]) {
            c->l = (width - c->w) / 2; c->r = c->l + c->w;
         } else if (lp->rules[RL_ALIGN_PARENT_RIGHT]) {
            c->l = width - pad[2] - c->w - lp->mr; c->r = c->l + c->w;
         }
      }
      if (wrap_h) {
         if (lp->rules[RL_CENTER_IN_PARENT] || lp->rules[RL_CENTER_VERTICAL]) {
            c->t = (height - c->h) / 2; c->b = c->t + c->h;
         } else if (lp->rules[RL_ALIGN_PARENT_BOTTOM]) {
            c->t = height - pad[3] - c->h - lp->mb; c->b = c->t + c->h;
         }
      }
      c->x = c->l;
      c->y = c->t;
      if (dvm__env("LUNARIA_UI_LAYOUT_TRACE"))
         fprintf(stderr, "[ui-relative]   placed v%u at %d,%d size %dx%d\n",
                 (unsigned)kids[i], c->x, c->y, c->w, c->h);
   }
   n->w = width < 0 ? 0 : width;
   n->h = height < 0 ? 0 : height;
}

static bool ui_is_frame_like(struct dvm *vm, dvm_ref view)
{
   return ui_is_a(vm, view, "android/widget/FrameLayout") ||
          ui_is_a(vm, view, "android/widget/ScrollView") ||
          ui_is_a(vm, view, "android/widget/HorizontalScrollView") ||
          ui_is_a(vm, view, "androidx/cardview/widget/CardView");
}

static void ui_measure_dispatch(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                                struct ui_ms hs, unsigned depth);

/* LUNARIA_UI_LAYOUT_TRACE=1: every measure, with the specs it was given, the
 * LayoutParams it read and the size it took. */
static bool view_measure_reuse(struct dvm *vm,dvm_ref view,int32_t width,int32_t height)
{
   union dvm_value epoch={0};
   if(!ui_int(vm,view,"measuredDimensionSet") || ui_int(vm,view,"layoutRequired") ||
      ui_int(vm,view,"measureWidthSpec")!=width || ui_int(vm,view,"measureHeightSpec")!=height ||
      !dvm_get_field(vm,view,"measureEpoch","J",&epoch) || (uint64_t)epoch.j!=g_ui_measure_epoch) return false;
   struct ui_node *node=ui_node(vm,view);
   if(!node) return false;
   node->w=(int32_t)((uint32_t)ui_int(vm,view,"measuredWidth")&0x00ffffffu);
   node->h=(int32_t)((uint32_t)ui_int(vm,view,"measuredHeight")&0x00ffffffu);
   return true;
}
static void view_measure_remember(struct dvm *vm,dvm_ref view,int32_t width,int32_t height,uint64_t epoch)
{
   rt_set_int(vm,view,"measureWidthSpec",width);rt_set_int(vm,view,"measureHeightSpec",height);
   (void)dvm_set_field(vm,view,"measureEpoch","J",(union dvm_value){.j=(int64_t)epoch});
}

static void ui_measure(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                       struct ui_ms hs, unsigned depth)
{
   if (dvm_exception(vm) || depth > 48 || !view) return;
   int32_t width_spec=(int32_t)((uint32_t)ws.size|((uint32_t)ws.mode<<30));
   int32_t height_spec=(int32_t)((uint32_t)hs.size|((uint32_t)hs.mode<<30));
   if(!ui_gone(vm,view) && view_measure_reuse(vm,view,width_spec,height_spec)) return;
   uint64_t epoch=g_ui_measure_epoch;
   struct dvm_class *klass = dvm_object_class(vm, view);
   struct dvm_method *on_measure = klass
      ? dvm_find_method(vm, klass, "onMeasure", "(II)V") : NULL;
   /* Runtime widgets use the host implementations below. An app override
    * owns its measurement, including any child measurements it requests. */
   if (on_measure && on_measure->builtin != view_on_measure &&
       on_measure->builtin != widget_on_measure && !ui_gone(vm, view)) {
      union dvm_value specs[2] = {
         { .i = (int32_t)((uint32_t)ws.size | ((uint32_t)ws.mode << 30)) },
         { .i = (int32_t)((uint32_t)hs.size | ((uint32_t)hs.mode << 30)) }
      }, ignored;
      if (!view_measure_api(vm, view, specs, 2, &ignored)) return;
   } else {
      union dvm_value ignored = {0};
      if (!view_resolve_layout_params(vm, view, NULL, 0, &ignored)) return;
      ui_measure_dispatch(vm, view, ws, hs, depth);
      if (dvm_exception(vm)) return;
      const struct ui_node *n = ui_node(vm, view);
      if (n) {
         union dvm_value sizes[2] = {{ .i = n->w }, { .i = n->h }}, ignored;
         (void)view_set_measured(vm, view, sizes, 2, &ignored);
         view_measure_remember(vm,view,width_spec,height_spec,epoch);
      }
   }
   static int trace = -1;
   if (trace < 0) trace = dvm__env("LUNARIA_UI_LAYOUT_TRACE") != NULL;
   if (trace) {
      struct ui_lp lp;
      ui_read_lp(vm, view, &lp);
      const struct ui_node *n = ui_node(vm, view);
      static const char mode[] = "UEA";
      int32_t pad[4];
      ui_pad(vm, view, pad);
      fprintf(stderr, "[ui-layout] %*s%s v%u spec=%c%d x %c%d lp=%d x %d "
              "m=%d,%d,%d,%d p=%d,%d,%d,%d w=%g o=%d -> %d x %d\n",
              (int)(depth * 2), "",
              ui_class_simple_name(vm, view), (unsigned)view,
              mode[ws.mode], ws.size, mode[hs.mode], hs.size, lp.w, lp.h,
              lp.ml, lp.mt, lp.mr, lp.mb, pad[0], pad[1], pad[2], pad[3],
              (double)lp.weight, ui_int(vm, view, "orientation"),
              n ? n->w : -1, n ? n->h : -1);
   }
}

static void ui_measure_dispatch(struct dvm *vm, dvm_ref view, struct ui_ms ws,
                                struct ui_ms hs, unsigned depth)
{
   struct ui_node *n = ui_node(vm, view);
   if (dvm__env("LUNARIA_UI_LAYOUT_TRACE"))
      fprintf(stderr, "[ui-dispatch] v%u %s node=%d gone=%d depth=%u\n", (unsigned)view,
              ui_class_simple_name(vm, view), n != NULL, ui_gone(vm, view), depth);
   if (!n) return;
   if (depth > 48 || ui_gone(vm, view)) { n->w = n->h = 0; return; }
   dvm_ref kids[1];
   const bool group = ui_children(vm, view, kids, 1) > 0 ||
                      ui_is_a(vm, view, "android/view/ViewGroup");
   if (ui_is_a(vm, view, "android/widget/LinearLayout"))
      ui_measure_linear(vm, view, ws, hs, n, depth);
   else if (ui_is_a(vm, view, "android/widget/RelativeLayout"))
      ui_measure_relative(vm, view, ws, hs, n, depth);
   else if (ui_is_a(vm, view, "android/widget/ListView")) {
      list_view_measure(vm,view,ws,hs,depth);
   } else if (group || ui_is_frame_like(vm, view))
      ui_measure_frame(vm, view, ws, hs, n, depth);
   else
      ui_measure_leaf(vm, view, ws, hs, n);
}

/* Public View measurement is independent of layout bounds. The virtual
 * callback must store both dimensions, including their measured state bits. */
static bool view_set_measured(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs;
   rt_set_int(vm, self, "measuredWidth", ARG(0).i);
   rt_set_int(vm, self, "measuredHeight", ARG(1).i);
   union dvm_value required = { .i = 1 };
   (void)dvm_set_field(vm, self, "layoutRequired", "Z", required);
   struct ui_node *node = ui_node(vm, self);
   if (node) {
      node->w = (int32_t)((uint32_t)ARG(0).i & 0x00ffffffu);
      node->h = (int32_t)((uint32_t)ARG(1).i & 0x00ffffffu);
   }
   union dvm_value yes = { .i = 1 };
   (void)dvm_set_field(vm, self, "measuredDimensionSet", "Z", yes);
   RETV();
}

static bool view_measure_api(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   struct dvm_class *c = dvm_object_class(vm, self);
   struct dvm_method *resolve = c ? dvm_find_method(vm, c, "resolveLayoutParams", "()V") : NULL;
   union dvm_value ignored = {0};
   if (resolve && !dvm_call(vm, resolve, self, NULL, 0, &ignored)) return false;
   if(view_measure_reuse(vm,self,ARG(0).i,ARG(1).i)) RETV();
   uint64_t epoch=g_ui_measure_epoch;
   union dvm_value flag = {0};
   (void)dvm_set_field(vm, self, "measuredDimensionSet", "Z", flag);
   struct dvm_method *m = c ? dvm_find_method(vm, c, "onMeasure", "(II)V") : NULL;
   if (!m || !dvm_call(vm, m, self, args, 2, out)) return false;
   (void)dvm_get_field(vm, self, "measuredDimensionSet", "Z", &flag);
   if (!flag.i) {
      dvm__throw(vm, "java/lang/IllegalStateException", "onMeasure must call setMeasuredDimension");
      return false;
   }
   view_measure_remember(vm,self,ARG(0).i,ARG(1).i,epoch);
   static int trace = -1;
   if (trace < 0) trace = dvm__env("LUNARIA_UI_LAYOUT_TRACE") != NULL;
   if (trace) {
      static const char mode[] = "UEA";
      const uint32_t ws = (uint32_t)args[0].i, hs = (uint32_t)args[1].i;
      fprintf(stderr, "[ui-measure] %s v%u spec=%c%u x %c%u -> %d x %d\n",
              ui_class_simple_name(vm, self), (unsigned)self,
              mode[(ws >> 30) & 3], ws & 0x3fffffffu,
              mode[(hs >> 30) & 3], hs & 0x3fffffffu,
              rt_get_int(vm, self, "measuredWidth"), rt_get_int(vm, self, "measuredHeight"));
      static int params_trace = -1;
      if (params_trace < 0) params_trace = dvm__env("LUNARIA_UI_PARAMS_TRACE") != NULL;
      if (params_trace) {
         dvm_ref params = ui_obj(vm, self, "layoutParams", "Landroid/view/ViewGroup$LayoutParams;");
         fprintf(stderr, "[ui-params] v%u id=%d params=v%u %s", (unsigned)self,
                 ui_int(vm, self, "id"), (unsigned)params,
                 params ? ui_class_simple_name(vm, params) : "null");
         static const char *const fields[] = {
            "width", "height", "leftToLeft", "leftToRight", "rightToLeft", "rightToRight",
            "topToTop", "topToBottom", "bottomToTop", "bottomToBottom",
            "startToStart", "startToEnd", "endToStart", "endToEnd",
            "resolvedLeftToLeft", "resolvedLeftToRight", "resolvedRightToLeft", "resolvedRightToRight"
         };
         for (size_t i = 0; params && i < sizeof fields / sizeof fields[0]; ++i) {
            union dvm_value value = {0};
            if (dvm_get_field(vm, params, fields[i], "I", &value))
               fprintf(stderr, " %s=%d", fields[i], value.i);
         }
         fputc('\n', stderr);
      }
   }
   RETV();
}

/* Child measurement uses the actual LayoutParams, not overlay fallbacks. */
static bool vg_measure_child(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   dvm_ref child = ARG(0).l;
   if (!child) { dvm__throw(vm, "java/lang/NullPointerException", "child"); return false; }
   dvm_ref lp = ui_obj(vm, child, "layoutParams", "Landroid/view/ViewGroup$LayoutParams;");
   if (!lp) { dvm__throw(vm, "java/lang/NullPointerException", "child LayoutParams"); return false; }
   const bool margins = nargs == 5;
   if (margins && !ui_is_a(vm, lp, "android/view/ViewGroup$MarginLayoutParams")) {
      dvm__throw(vm, "java/lang/ClassCastException", "expected MarginLayoutParams");
      return false;
   }
   uint32_t px = (uint32_t)ui_int(vm, self, "paddingLeft") + (uint32_t)ui_int(vm, self, "paddingRight");
   uint32_t py = (uint32_t)ui_int(vm, self, "paddingTop") + (uint32_t)ui_int(vm, self, "paddingBottom");
   if (margins) {
      px += (uint32_t)ui_int(vm, lp, "leftMargin") + (uint32_t)ui_int(vm, lp, "rightMargin") + (uint32_t)ARG(2).i;
      py += (uint32_t)ui_int(vm, lp, "topMargin") + (uint32_t)ui_int(vm, lp, "bottomMargin") + (uint32_t)ARG(4).i;
   }
   uint32_t w = (uint32_t)ARG(1).i, h = (uint32_t)args[margins ? 3 : 2].i;
   struct ui_ms ws = ui_child_spec(ui_ms_make(w >> 30, w & 0x3fffffffu), (int32_t)px, ui_int(vm, lp, "width"));
   struct ui_ms hs = ui_child_spec(ui_ms_make(h >> 30, h & 0x3fffffffu), (int32_t)py, ui_int(vm, lp, "height"));
   union dvm_value specs[2] = {
      { .i = (int32_t)((uint32_t)ws.size | ((uint32_t)ws.mode << 30)) },
      { .i = (int32_t)((uint32_t)hs.size | ((uint32_t)hs.mode << 30)) }
   };
   struct dvm_class *c = dvm_object_class(vm, child);
   struct dvm_method *measure = c ? dvm_find_method(vm, c, "measure", "(II)V") : NULL;
   return measure && dvm_call(vm, measure, child, specs, 2, out);
}

static bool drawable_minimum(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   const struct dvm_method *caller = dvm__builtin_method();
   bool vertical = caller && strstr(caller->name, "Height");
   struct dvm_class *c = dvm_object_class(vm, self);
   struct dvm_method *m = c ? dvm_find_method(vm, c,
      vertical ? "getIntrinsicHeight" : "getIntrinsicWidth", "()I") : NULL;
   union dvm_value size = {0};
   if (m && !dvm_call(vm, m, self, NULL, 0, &size)) return false;
   RETI(size.i > 0 ? size.i : 0);
}

static int32_t view_suggested_minimum(struct dvm *vm, dvm_ref self, bool vertical)
{
   int32_t size = ui_int(vm, self, vertical ? "minimumHeight" : "minimumWidth");
   dvm_ref bg = ui_obj(vm, self, "background", "Landroid/graphics/drawable/Drawable;");
   struct dvm_class *c = bg ? dvm_object_class(vm, bg) : NULL;
   struct dvm_method *m = c ? dvm_find_method(vm, c,
      vertical ? "getMinimumHeight" : "getMinimumWidth", "()I") : NULL;
   union dvm_value result = {0};
   if (m && dvm_call(vm, m, bg, NULL, 0, &result) && result.i > size) size = result.i;
   return size;
}

static bool view_minimum(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   const struct dvm_method *caller = dvm__builtin_method();
   const char *name = caller ? caller->name : "";
   bool vertical = strstr(name, "Height") != NULL;
   const char *field = vertical ? "minimumHeight" : "minimumWidth";
   if (nargs) {
      rt_set_int(vm, self, field, ARG(0).i);
      return view_request_layout(vm, self, NULL, 0, out);
   }
   if (strstr(name, "Suggested")) {
      int32_t size = view_suggested_minimum(vm, self, vertical);
      if (dvm_exception(vm)) return false;
      RETI(size);
   }
   RETI(ui_int(vm, self, field));
}

static bool view_on_measure(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   union dvm_value sizes[2] = {{0}, {0}}, result = {0};
   for (int i = 0; i < 2; ++i) {
      union dvm_value defaults[2] = {{ .i = view_suggested_minimum(vm, self, i != 0) }, args[i]};
      if (dvm_exception(vm)) return false;
      (void)view_getDefaultSize(vm, self, defaults, 2, &result);
      sizes[i] = result;
   }
   return view_set_measured(vm, self, sizes, nargs, out);
}

static bool widget_on_measure(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   struct ui_ms ws = { (int)((uint32_t)ARG(0).i >> 30),
                      (int32_t)((uint32_t)ARG(0).i & 0x3fffffffu) };
   struct ui_ms hs = { (int)((uint32_t)ARG(1).i >> 30),
                      (int32_t)((uint32_t)ARG(1).i & 0x3fffffffu) };
   ui_measure_dispatch(vm, self, ws, hs, 0);
   if (dvm_exception(vm)) return false;
   struct ui_node *n = ui_node(vm, self);
   union dvm_value sizes[2] = {{ .i = n ? n->w : 0 }, { .i = n ? n->h : 0 }};
   return view_set_measured(vm, self, sizes, nargs, out);
}

#define VIEW_MEASURE_GETTER(fn, field, mask) \
static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
               int nargs, union dvm_value *out) \
{ (void)args; (void)nargs; RETI((int32_t)((uint32_t)ui_int(vm, self, field) & (mask))); }
VIEW_MEASURE_GETTER(view_get_measured_width, "measuredWidth", 0x00ffffffu)
VIEW_MEASURE_GETTER(view_get_measured_height, "measuredHeight", 0x00ffffffu)
VIEW_MEASURE_GETTER(view_get_measured_width_state, "measuredWidth", 0xffffffffu)
VIEW_MEASURE_GETTER(view_get_measured_height_state, "measuredHeight", 0xffffffffu)
#undef VIEW_MEASURE_GETTER

static bool view_get_measured_state(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI((ui_int(vm, self, "measuredWidth") & (int32_t)0xff000000u) |
         ((uint32_t)ui_int(vm, self, "measuredHeight") >> 16 & 0x0000ff00u));
}

static void ui_layout(struct dvm *vm, dvm_ref view, unsigned depth)
{
   struct ui_node *n = ui_node(vm, view);
   if (!n || depth > 48 || ui_gone(vm, view)) return;
   struct dvm_class *klass = dvm_object_class(vm, view);
   struct dvm_method *callback = klass
      ? dvm_find_method(vm, klass, "onLayout", "(ZIIII)V") : NULL;
   /* An app override owns its child placement. Do not overwrite it with a
    * FrameLayout pass after the callback returns. */
   if (callback && callback->builtin != empty_void) {
      union dvm_value a[4] = {{.i=n->x}, {.i=n->y},
         {.i=(int32_t)((uint32_t)n->x + (uint32_t)n->w)},
         {.i=(int32_t)((uint32_t)n->y + (uint32_t)n->h)}}, ignored = {0};
      (void)view_layout_api(vm, view, a, 4, &ignored);
      return;
   }
   union dvm_value bounds[4] = {{.i=n->x}, {.i=n->y},
      {.i=(int32_t)((uint32_t)n->x + (uint32_t)n->w)},
      {.i=(int32_t)((uint32_t)n->y + (uint32_t)n->h)}}, ignored = {0};
   if (!view_layout_api(vm, view, bounds, 4, &ignored)) return;
   if (ui_is_a(vm, view, "android/widget/LinearLayout"))
      ui_layout_linear(vm, view, n);
   else if (ui_is_a(vm, view, "android/widget/RelativeLayout") ||
            ui_is_a(vm, view, "android/widget/ListView"))
      ;
   else
      ui_layout_frame(vm, view, n);
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, view, kids, UI_MAX_KIDS);
   for (uint32_t i = 0; i < nk; ++i) {
      ui_layout(vm, kids[i], depth + 1);
      /* View.layout(): the frame the app reads back through getLeft(),
       * getWidth() and friends, relative to the parent. */
      const struct ui_node *c = ui_node(vm, kids[i]);
      if (!c) continue;
      rt_set_int(vm, kids[i], "left", c->x);
      rt_set_int(vm, kids[i], "top", c->y);
      rt_set_int(vm, kids[i], "right", c->x + c->w);
      rt_set_int(vm, kids[i], "bottom", c->y + c->h);
   }
}

static bool view_layout_api(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   static const char *edges[] = { "left", "top", "right", "bottom" };
   bool changed = false;
   union dvm_value notification[9]={{.l=self},args[0],args[1],args[2],args[3]};
   for (int i=0;i<4;++i) notification[5+i].i=ui_int(vm,self,edges[i]);
   const int32_t old_w = (int32_t)((uint32_t)ui_int(vm, self, "right") - (uint32_t)ui_int(vm, self, "left"));
   const int32_t old_h = (int32_t)((uint32_t)ui_int(vm, self, "bottom") - (uint32_t)ui_int(vm, self, "top"));
   for (int i = 0; i < 4; ++i) {
      changed |= ui_int(vm, self, edges[i]) != ARG(i).i;
      rt_set_int(vm, self, edges[i], ARG(i).i);
   }
   /* View.setFrame(): a new size is announced before onLayout, so a view that
    * forwards it (Cocos2dxGLSurfaceView hands it to its renderer, which passes
    * it to nativeInit) knows its extent when the surface is created. */
   const int32_t new_w = (int32_t)((uint32_t)ARG(2).i - (uint32_t)ARG(0).i);
   const int32_t new_h = (int32_t)((uint32_t)ARG(3).i - (uint32_t)ARG(1).i);
   if (new_w != old_w || new_h != old_h) {
      struct dvm_class *c = dvm_object_class(vm, self);
      struct dvm_method *m = c ? dvm_find_method(vm, c, "onSizeChanged", "(IIII)V") : NULL;
      union dvm_value a[4] = {{.i=new_w}, {.i=new_h}, {.i=old_w}, {.i=old_h}}, ignored = {0};
      if (m && !dvm_call(vm, m, self, a, 4, &ignored)) return false;
   }
   struct ui_node *n = ui_node(vm, self);
   if (n) {
      if (dvm__env("LUNARIA_UI_LAYOUT_TRACE") && n->y != ARG(1).i)
         fprintf(stderr, "[ui-relayout] v%u %s y %d -> %d\n", (unsigned)self,
                 ui_class_simple_name(vm, self), n->y, ARG(1).i);
      n->x = ARG(0).i; n->y = ARG(1).i;
      n->w = (int32_t)((uint32_t)ARG(2).i - (uint32_t)ARG(0).i);
      n->h = (int32_t)((uint32_t)ARG(3).i - (uint32_t)ARG(1).i);
   }
   union dvm_value required = {0};
   (void)dvm_get_field(vm, self, "layoutRequired", "Z", &required);
   if (changed || required.i) {
      if (dvm__env("LUNARIA_UI_LAYOUT_TRACE"))
         fprintf(stderr, "[ui-layout-call] v%u %s changed=%d required=%d bounds=%d,%d,%d,%d\n",
                 (unsigned)self, ui_class_simple_name(vm, self), changed, required.i,
                 ARG(0).i, ARG(1).i, ARG(2).i, ARG(3).i);
      struct dvm_class *c = dvm_object_class(vm, self);
      struct dvm_method *m = c ? dvm_find_method(vm, c, "onLayout", "(ZIIII)V") : NULL;
      union dvm_value a[5] = {{.i=changed}, args[0], args[1], args[2], args[3]}, ignored = {0};
      if (m && !dvm_call(vm, m, self, a, 5, &ignored)) return false;
      required.i = 0;
      (void)dvm_set_field(vm, self, "layoutRequired", "Z", required);
      /* Android snapshots the list after onLayout so callbacks may add or
       * remove listeners without changing this notification's recipients. */
      union dvm_value listeners={0}, snapshot={0};
      (void)dvm_get_field(vm,self,"layoutListeners","Ljava/util/List;",&listeners);
      if (listeners.l) {
         if (!list_toArray(vm,listeners.l,NULL,0,&snapshot)) return false;
         dvm_pin(vm,snapshot.l);
         uint32_t count=dvm_array_length(vm,snapshot.l);
         bool good=true;
         for (uint32_t i=0;i<count && good;++i) {
            dvm_ref *items=dvm_array_data(vm,snapshot.l);
            good=am_vcall(vm,items[i],"onLayoutChange","(Landroid/view/View;IIIIIIII)V",notification,9,&ignored);
         }
         dvm_unpin(vm,snapshot.l);
         if (!good) return false;
      }
   }
   union dvm_value yes = {.i=1};
   (void)dvm_set_field(vm, self, "laidOut", "Z", yes);
   ui_invalidate();
   RETV();
}

static bool widget_on_layout(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   struct ui_node *n = ui_node(vm, self);
   if (!n) RETV();
   if (ui_is_a(vm, self, "android/widget/LinearLayout"))
      ui_layout_linear(vm, self, n);
   else if (!ui_is_a(vm, self, "android/widget/RelativeLayout") &&
            !ui_is_a(vm, self, "android/widget/ListView"))
      ui_layout_frame(vm, self, n);   /* a RelativeLayout's rules placed its children in measure */
   dvm_ref kids[UI_MAX_KIDS];
   uint32_t count = ui_children(vm, self, kids, UI_MAX_KIDS);
   for (uint32_t i = 0; i < count; ++i) {
      ui_layout(vm, kids[i], 0);
      if (dvm_exception(vm)) return false;
   }
   RETV();
}

static void ui_emit_view(struct dvm *vm, struct ui_buf *b, struct ui_buf *css,
                         dvm_ref view, int parent_orientation, unsigned depth)
{
   (void)parent_orientation;
   if (!view || depth > 48) return;
   const int32_t visibility = ui_int(vm, view, "visibility");
   if (visibility == 8) return;             /* GONE: not laid out at all */
   const struct ui_node *n = ui_node(vm, view);
   if (!n) return;

   const char *raster=ui_capture_custom_view(vm,view);
   if (dvm_exception(vm)) return;
   const char *simple = ui_class_simple_name(vm, view);
   const bool is_compound = ui_is_a(vm, view, "android/widget/CompoundButton");
   const bool is_scroll = ui_is_a(vm, view, "android/widget/ScrollView") ||
                          ui_is_a(vm, view, "android/widget/HorizontalScrollView") ||
                          ui_is_a(vm, view, "androidx/core/widget/NestedScrollView");

   struct ui_buf *b2 = css;
   if (dvm__env("LUNARIA_UI_LAYOUT_TRACE"))
      fprintf(stderr, "[ui-emit] v%u %s at %d,%d %dx%d\n", (unsigned)view, simple,
              n->x, n->y, n->w, n->h);
   /* The box the layout pass gave it, against the parent's top-left — moved by
    * how far the parent is scrolled. */
   dvm_ref scroll_parent = ui_obj(vm, view, "parent", "Landroid/view/ViewParent;");
   const int32_t off_x = scroll_parent ? ui_int(vm, scroll_parent, "scrollX") : 0;
   const int32_t off_y = scroll_parent ? ui_int(vm, scroll_parent, "scrollY") : 0;
   ui_printf(b2, "#v%u{position:absolute;left:%dpx;top:%dpx;width:%dpx;height:%dpx;",
             (unsigned)view, n->x - off_x + (int32_t)ui_float(vm, view, "translationX"),
             n->y - off_y + (int32_t)ui_float(vm, view, "translationY"), n->w, n->h);
   if (is_scroll || ui_is_a(vm, view, "android/widget/NumberPicker"))
      ui_puts(b2, "overflow:hidden;");

   int32_t bg = 0;
   if (!raster && ui_background_color(vm, view, &bg)) {
      char c[64];
      ui_color_css(c, sizeof c, bg);
      ui_printf(b2, "background:%s;", c);
   }
   const bool is_webview = ui_is_a(vm, view, "android/webkit/WebView");
   struct webview_host_view *web_host = is_webview ? webview_host_find(view) : NULL;
   if (web_host) { web_host->width = n->w; web_host->height = n->h; }
   if (web_host && web_host->image_ready)
      ui_printf(b2, "background-image:url('%s');background-size:100%% 100%%;",
                web_host->image);
   int32_t radius = ui_int(vm, view, "cornerRadius");
   if (!raster) ui_emit_background_shape(vm, b2, view, &radius);
   if (radius > 0) ui_printf(b2, "border-radius:%dpx;", radius);

   /* The picture, drawn into the box the layout sized for it. */
   if (raster) ui_printf(b2,"background-image:url('%s');background-size:100%% 100%%;",raster);
   else ui_emit_drawables(vm, b2, view, -1, -1);

   /* Text sits in the view's padding box, placed by its gravity. */
   const char *text = ui_text_of(vm, view);
   const bool showing_hint = !text || !*text;
   if (showing_hint) {
      dvm_ref hint = ui_obj(vm, view, "hint", "Ljava/lang/CharSequence;");
      text = hint ? dvm_string_utf8(vm, hint) : NULL;
   }
   if ((text && *text) || is_compound) {
      int32_t pad[4];
      ui_pad(vm, view, pad);
      if (pad[0] || pad[1] || pad[2] || pad[3])
         ui_printf(b2, "padding:%dpx %dpx %dpx %dpx;", pad[1], pad[2], pad[3], pad[0]);
      float size = text_size_px(vm, view);
      ui_printf(b2, "font-size:%gpx;", (double)size);
      const int32_t color = ui_int(vm, view,
                                   showing_hint ? "hintTextColor" : "textColor");
      if (color) {
         char c[64];
         ui_color_css(c, sizeof c, color);
         ui_printf(b2, "color:%s;", c);
      }
      if (ui_int(vm, view, "textStyle") & 1) ui_puts(b2, "font-weight:bold;");
      /* TextView defaults to top|start, Button to center, EditText to
       * center_vertical|start (their styles' android:gravity). */
      int32_t g = ui_int(vm, view, "gravity");
      if (!g) g = ui_is_a(vm, view, "android/widget/Button") &&
                  !is_compound ? 0x11
                : ui_is_a(vm, view, "android/widget/EditText") || is_compound
                  ? 0x13 : 0x33;
      const int32_t hg = g & 0x07, vg = g & 0x70;
      ui_printf(b2, "display:flex;flex-direction:row;align-items:%s;"
                "justify-content:%s;text-align:%s;",
                vg == UI_GRAVITY_CENTER_V ? "center"
                : vg == UI_GRAVITY_BOTTOM ? "flex-end" : "flex-start",
                hg == UI_GRAVITY_CENTER_H ? "center"
                : hg == UI_GRAVITY_RIGHT ? "flex-end" : "flex-start",
                hg == UI_GRAVITY_CENTER_H ? "center"
                : hg == UI_GRAVITY_RIGHT ? "right" : "left");
   }

   if (visibility == 4) ui_puts(b2, "visibility:hidden;");
   const bool disabled = ui_int(vm, view, "disabled") != 0;
   union dvm_value alpha_set = { 0 };
   (void)dvm_get_field(vm, view, "alphaSet", "Z", &alpha_set);
   union dvm_value transition_set = {0};
   (void)dvm_get_field(vm, view, "transitionAlphaSet", "Z", &transition_set);
   const float alpha = (alpha_set.i ? ui_float(vm, view, "alpha") : 1.0f) *
                       (transition_set.i ? ui_float(vm, view, "transitionAlpha") : 1.0f);
   if (disabled || alpha_set.i || transition_set.i)
      ui_printf(b2, "opacity:%g;", (double)(alpha * (disabled ? 0.45f : 1.0f)));
   /* An EditText takes a touch to focus it, listener or not. */
   struct dvm_class *touch_class=dvm_object_class(vm,view);
   struct dvm_method *touch_method=touch_class ? dvm_find_method(vm,touch_class,"onTouchEvent","(Landroid/view/MotionEvent;)Z") : NULL;
   const bool custom_touch=raster || (touch_method && touch_method->has_code) ||
                           ui_is_a(vm, view, "android/widget/NumberPicker");
   const bool clickable = custom_touch || ui_is_clickable(vm, view) || is_compound ||
                          is_webview ||
                          ui_is_a(vm, view, "android/widget/EditText");
   if (clickable && !disabled) ui_puts(b2, "cursor:pointer;pointer-events:auto;");
   /* A view that does not handle a touch lets it through to what is under
    * it (onTouchEvent returns false); only clickable views stop one. */
   if (!clickable) ui_puts(b2, "pointer-events:none;");
   ui_puts(b2, "}");

   /* SurfaceView draws into a separate surface behind its window. Its View
    * paint point clears the window buffer before drawing its own background
    * or children (SurfaceView.draw/dispatchDraw, PorterDuff.Mode.CLEAR). */
   ui_printf(b, "<div id=\"v%u\" class=\"w%s %s%s%s\">", (unsigned)view,
             ui_is_a(vm, view, "android/view/SurfaceView") ? " lunaria-surface-hole" : "",
             simple, is_webview ? " lunaria-webview" : custom_touch ? " lunaria-touchview" : "",
             ui_is_a(vm, view, "android/widget/NumberPicker") ? " lunaria-gestureview" : "");
   if (raster) {ui_puts(b,"</div>"); return;}
   if (is_compound) {
      const bool checked = ui_int(vm, view, "checked") != 0;
      ui_printf(b, "<div id=\"v%ub\" class=\"cbox%s\"></div>", (unsigned)view,
                checked ? " on" : "");
   }
   if (text && *text) {
      ui_printf(b, "<span id=\"v%ut\" class=\"label\">", (unsigned)view);
      ui_put_escaped(b, text);
      ui_puts(b, "</span>");
   }
   ui_emit_children(vm, b, css, view, -1, depth);
   ui_puts(b, "</div>");
}

/* Windows currently up, innermost last.  A dialog is a window; so is the
 * Activity's own content view when something put one there. */
#define UI_MAX_WINDOWS 8
static dvm_ref g_ui_windows[UI_MAX_WINDOWS];
static int g_ui_nwindows;
static bool g_ui_dirty;
static bool g_ui_building;
static unsigned g_ui_draw_trace_budget;
static bool g_ui_measure_dirty = true;
static struct {uint64_t builds,build_ns,draws,draw_ns,publish_ns,layout_ns,scroll_ns,emit_ns;} g_ui_render_prof;
static uint64_t ui_render_ns(void)
{
   if(!dvm__env("LUNARIA_UI_RENDER_PROF")) return 0;
   struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
   return (uint64_t)ts.tv_sec*1000000000u+(uint64_t)ts.tv_nsec;
}
static struct dvm *g_ui_vm;
static dvm_ref g_ui_focused_window;

/* Clicks come back from the presenting thread; they are dispatched on the
 * thread that drains the pending queue, which is where every other callback
 * the app posted to the main looper runs. */
static dvm_ref g_ui_clicks[16];
static int g_ui_nclicks;
static pthread_mutex_t g_ui_click_lock = PTHREAD_MUTEX_INITIALIZER;

static bool dialog_user_cancelable(struct dvm *vm, dvm_ref dialog);
static bool dialog_cancel(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out);

static void ui_click_from_overlay(const char *id)
{
   if (!id || !*id) return;
   /* The dim behind a cancelable dialog: on a device a press outside the
    * window cancels it when setCanceledOnTouchOutside is true.  The scrim
    * has no view id of its own, so name it and answer it here — otherwise
    * the overlay eats the press and the dialog never comes down. */
   if (!strcmp(id, "scrim")) {
      if (!g_ui_vm || g_ui_nwindows <= 0) return;
      dvm_ref top = g_ui_windows[g_ui_nwindows - 1];
      if (!dialog_user_cancelable(g_ui_vm, top)) {
         fprintf(stderr, "[ui] scrim press: top window %u is not cancelable\n",
                 (unsigned)top);
         return;
      }
      union dvm_value co = { 0 };
      (void)dvm_get_field(g_ui_vm, top, "cancelOutside", "Z", &co);
      if (!co.i) {
         fprintf(stderr, "[ui] scrim press: window %u rejects outside cancel\n",
                 (unsigned)top);
         return;
      }
      fprintf(stderr, "[ui] scrim press -> cancel window %u\n", (unsigned)top);
      union dvm_value ignored = { 0 };
      (void)dialog_cancel(g_ui_vm, top, NULL, 0, &ignored);
      return;
   }
   if (id[0] != 'v') return;
   unsigned long ref = strtoul(id + 1, NULL, 10);
   if (!ref) return;
   pthread_mutex_lock(&g_ui_click_lock);
   if (g_ui_nclicks < (int)(sizeof g_ui_clicks / sizeof g_ui_clicks[0]))
      g_ui_clicks[g_ui_nclicks++] = (dvm_ref)ref;
   pthread_mutex_unlock(&g_ui_click_lock);
}

/* Drags the overlay saw, replayed on the main looper like clicks. */
struct ui_scroll_event { double x, y, dx, dy; };
static struct ui_scroll_event g_ui_scrolls[32];
static int g_ui_nscrolls;

static void ui_scroll_from_overlay(double x, double y, double dx, double dy)
{
   pthread_mutex_lock(&g_ui_click_lock);
   if (g_ui_nscrolls < (int)(sizeof g_ui_scrolls / sizeof g_ui_scrolls[0]))
      g_ui_scrolls[g_ui_nscrolls++] = (struct ui_scroll_event){ x, y, dx, dy };
   pthread_mutex_unlock(&g_ui_click_lock);
}

/* Content may be a subtree below PhoneWindow's content parent and DecorView.
 * Attachment and window visibility belong to the complete root hierarchy. */
static dvm_ref ui_window_root(struct dvm *vm,dvm_ref window)
{
   return view_hierarchy_root(vm,ui_window_content(vm,window));
}

static dvm_ref ui_presented_window_content(struct dvm *vm,dvm_ref window)
{ return act_window_present(vm,window) ? ui_window_content(vm,window) : 0; }

/* The WindowManager.LayoutParams a window was given, if it has any.  A Dialog
 * keeps its Window in a field; a Window carries them itself. */
static dvm_ref ui_window_params(struct dvm *vm, dvm_ref window)
{
   dvm_ref lp = ui_obj(vm, window, "attributes",
                       "Landroid/view/WindowManager$LayoutParams;");
   if (lp) return lp;
   dvm_ref w = ui_obj(vm, window, "window", "Landroid/view/Window;");
   return w ? ui_obj(vm, w, "attributes",
                     "Landroid/view/WindowManager$LayoutParams;") : 0;
}

/* WindowManager.LayoutParams.FLAG_DIM_BEHIND.  A Dialog gets it from the
 * floating-window theme unless it cleared it, so absent params mean a dialog
 * dims and a window added straight to the WindowManager does not. */
#define UI_FLAG_DIM_BEHIND 0x00000002
/* A floating window (windowIsFloating): a Dialog, or an Activity whose theme
 * is one of the dialog themes.  It is as big as its content, centred, and
 * dims what is behind it — the three things Theme.Dialog sets. */
static bool ui_window_floating(struct dvm *vm, dvm_ref window)
{
   if (ui_is_a(vm, window, "android/app/Dialog")) return true;
   if (ui_is_a(vm, window, "android/app/Activity")) {
      const int32_t theme = ui_int(vm, window, "themeResId");
      return theme && arm_exec_theme_is_floating((uint32_t)theme);
   }
   return false;
}

/* A colour a theme gives an attribute, following `?attr` and `@color`
 * references; false when it is a drawable or not defined. */
static bool ui_theme_color(int32_t theme, uint32_t attr, int32_t *argb)
{
   for (int hops = 0; hops < 6 && theme; ++hops) {
      int32_t value = 0; const char *string = NULL;
      int kind = arm_exec_apk_style_value((uint32_t)theme, attr, &value, &string);
      if (dvm__env("LUNARIA_TRACE_TA"))
         fprintf(stderr, "[theme] 0x%x attr 0x%x -> kind 0x%x value 0x%x\n", (unsigned)theme, (unsigned)attr, (unsigned)kind, (unsigned)value);
      if (kind >= AX_COLOR_ARGB8 && kind <= AX_COLOR_RGB4) { *argb = value; return true; }
      if (kind == AX_ATTRIBUTE) { attr = (uint32_t)value; continue; }   /* ?attr/name */
      if (kind == AX_REFERENCE) {
         int rk = arm_exec_apk_resource_value((uint32_t)value, &value, &string);
         if (rk >= AX_COLOR_ARGB8 && rk <= AX_COLOR_RGB4) { *argb = value; return true; }
      }
      return false;
   }
   return false;
}

static bool ui_window_dims(struct dvm *vm, dvm_ref window)
{
   dvm_ref lp = ui_window_params(vm, window);
   if (lp && ui_int(vm, lp, "flags"))
      return (ui_int(vm, lp, "flags") & UI_FLAG_DIM_BEHIND) != 0;
   return ui_window_floating(vm, window);
}

/* Where the window sits and how big it is, from its own LayoutParams.
 *
 * MATCH_PARENT (-1) fills the screen along that axis, WRAP_CONTENT (-2) and an
 * unset 0 are as big as the content, and anything else is a pixel size.  The
 * gravity is the same one View uses, applied against the display; with none
 * given a floating window is centred, which is what a Dialog does. */
static void ui_emit_window_box(struct dvm *vm, struct ui_buf *css, dvm_ref window)
{
   dvm_ref lp = ui_window_params(vm, window);
   /* An unset field reads back as 0, which is not a size any LayoutParams can
    * hold: `new WindowManager.LayoutParams()` starts at MATCH_PARENT. */
   int32_t w = lp ? ui_int(vm, lp, "width") : 0;
   int32_t h = lp ? ui_int(vm, lp, "height") : 0;
   const int32_t g = lp ? ui_int(vm, lp, "gravity") : 0;
   /* Unset: a floating window wraps its content (PhoneWindow sets
    * WRAP_CONTENT for windowIsFloating), any other fills the display. */
   const bool floating = ui_window_floating(vm, window);
   if (w == 0 && !ui_is_a(vm, window, "android/widget/PopupWindow")) w = floating ? -2 : -1;
   if (h == 0 && !ui_is_a(vm, window, "android/widget/PopupWindow")) h = floating ? -2 : -1;
   const int32_t x = lp ? ui_int(vm, lp, "x") : 0;
   const int32_t y = lp ? ui_int(vm, lp, "y") : 0;
   const int32_t sw = rt_surface_w(), sh = rt_surface_h();

   /* The window is measured the way ViewRootImpl measures its DecorView:
    * exactly the display for MATCH_PARENT, at most the display for
    * WRAP_CONTENT, exactly the number otherwise.  Its content is then
    * measured against that box like a FrameLayout child. */
   struct ui_ms ws = w == -1 ? ui_ms_make(UI_MS_EXACT, sw)
                   : w == -2 ? ui_ms_make(UI_MS_ATMOST, sw)
                   : ui_ms_make(UI_MS_EXACT, w);
   struct ui_ms hs = h == -1 ? ui_ms_make(UI_MS_EXACT, sh)
                   : h == -2 ? ui_ms_make(UI_MS_ATMOST, sh)
                   : ui_ms_make(UI_MS_EXACT, h);
   dvm_ref content = ui_presented_window_content(vm, window);
   int32_t win_w = ws.size, win_h = hs.size;
   if (content) {
      if (!view_dispatch_attachment(vm, ui_window_root(vm,window), true)) {
         ui_report_exception(vm, "onAttachedToWindow"); return;
      }
      struct ui_lp clp;
      ui_read_lp(vm, content, &clp);
      if (g_ui_measure_dirty)
         ui_measure(vm, content, ui_child_spec(ws, clp.ml + clp.mr, clp.w),
                    ui_child_spec(hs, clp.mt + clp.mb, clp.h), 0);
      if (dvm_exception(vm)) { ui_report_exception(vm, "onMeasure"); return; }
      struct ui_node *c = ui_node(vm, content);
      if (ws.mode != UI_MS_EXACT) win_w = c->w + clp.ml + clp.mr;
      if (hs.mode != UI_MS_EXACT) win_h = c->h + clp.mt + clp.mb;
      /* The content view is a child of the window's content FrameLayout
       * (android.R.id.content), placed by its layout_gravity. */
      const int32_t cg = clp.gravity >= 0 ? clp.gravity : 0x33;
      const int32_t chg = cg & 0x07, cvg = cg & 0x70;
      if (g_ui_measure_dirty) {
         c->x = chg == UI_GRAVITY_CENTER_H ? (win_w - c->w) / 2 + clp.ml - clp.mr
              : chg == UI_GRAVITY_RIGHT ? win_w - c->w - clp.mr : clp.ml;
         c->y = cvg == UI_GRAVITY_CENTER_V ? (win_h - c->h) / 2 + clp.mt - clp.mb
              : cvg == UI_GRAVITY_BOTTOM ? win_h - c->h - clp.mb : clp.mt;
         ui_layout(vm, content, 0);
      }
      static int said;
      if (said++ < 16)
         fprintf(stderr, "[ui] window %u: lp %d x %d gravity 0x%x%s -> %d x %d; "
                 "content %s %d x %d gravity 0x%x at %d,%d\n",
                 (unsigned)window, w, h, g,
                 ui_window_floating(vm, window) ? " floating" : "",
                 win_w, win_h, ui_class_simple_name(vm, content), c->w, c->h,
                 clp.gravity, c->x, c->y);
   }
   /* Placed on the display by its gravity and offset (Gravity.apply). */
   const int32_t hg = g & 0x07, vg = g & 0x70;
   int32_t left = hg == UI_GRAVITY_LEFT ? x
                : hg == UI_GRAVITY_RIGHT ? sw - win_w - x
                : (sw - win_w) / 2 + x;
   int32_t top = vg == UI_GRAVITY_TOP ? y
               : vg == UI_GRAVITY_BOTTOM ? sh - win_h - y
               : (sh - win_h) / 2 + y;
   /* The window's own background is the theme's android:windowBackground;
    * without it everything the views leave unpainted is the framebuffer. */
   char window_bg[64] = "";
   int32_t bg_argb = 0;
   if (!ui_window_floating(vm, window) && ui_is_a(vm, window, "android/app/Activity") &&
       ((ui_theme_color(ui_int(vm, window, "themeResId"), 0x01010054u, &bg_argb)) ||
        /* The theme chain ends in a platform theme this runtime has no style
         * table for.  Theme.Material.Light's windowBackground is
         * @color/background_material_light, #fafafa. */
        (bg_argb = (int32_t)0xfffafafau, true))) {
      char c[48];
      ui_color_css(c, sizeof c, bg_argb);
      snprintf(window_bg, sizeof window_bg, "background:%s;", c);
   }
   ui_printf(css, "#win%u{left:%dpx;top:%dpx;width:%dpx;height:%dpx;%s}",
             (unsigned)window, left, top, win_w, win_h, window_bg);
}

/* The sheet is fixed: everything that varies about a view is written inline,
 * because it comes from the guest's own attributes rather than from a theme
 * this side gets to choose. */
static const char *ui_stylesheet(void)
{
   return
      "body{margin:0;background:transparent;font-size:14px;color:#ffffff;}"
      ".scrim{position:fixed;left:0;top:0;width:100%;height:100%;"
      "background:rgba(0,0,0,0.6);}"
      /* A window is placed by its own LayoutParams; only the box it asks
       * for belongs to it, and the rest of the screen belongs to whatever is
       * underneath.  `.root` used to be the whole screen for every window,
       * which turned a notification banner into a full-screen yellow panel
       * and pushed its close button, laid out with gravity bottom|end, into
       * the corner of the display instead of the corner of the banner. */
      /* Not a flex column: a MATCH_PARENT content root with height:100% inside
       * a flex parent collapsed to 0 in luna-ui, which made WebViewDialog an
       * invisible modal.  Block layout + an absolute fill on MATCH_PARENT
       * roots (see ui_emit_view) matches how the window's own box is sized. */
      ".root{position:fixed;box-sizing:border-box;}"
      ".w{box-sizing:border-box;}"
      /* TextView already supplies the line-box width. CSS's automatic
       * min-content size must not widen its text beyond that measured box. */
      ".label{flex:0 1 auto;min-width:0;max-width:100%;}"
      /* The check box a CompoundButton would have drawn from its drawable. */
      ".cbox{width:18px;height:18px;flex:0 0 auto;margin-right:8px;"
      "border:2px solid #8a8f9a;border-radius:3px;background:#ffffff;}"
      ".cbox.on{background:#2b6fd8;border-color:#2b6fd8;}";
}

/* LUNARIA_UI_TREE: the view hierarchy of every window — class, id, visibility,
 * and the box the layout pass gave it — for finding out why a screen is empty. */
static void ui_dump_tree(struct dvm *vm, dvm_ref view, unsigned depth)
{
   if (!view || depth > 40) return;
   const struct ui_node *n = ui_node(vm, view);
   fprintf(stderr, "[tree] %*s%s id=0x%x vis=%d %dx%d @%d,%d ref=%u\n", (int)(depth * 2), "",
           ui_class_simple_name(vm, view), (unsigned)ui_int(vm, view, "id"),
           ui_int(vm, view, "visibility"), n ? n->w : -1, n ? n->h : -1,
           n ? n->x : 0, n ? n->y : 0,(unsigned)view);
   dvm_ref source=ui_obj(vm,view,"text","Ljava/lang/CharSequence;");
   if(source) {
      const char *text=ui_text_of(vm,view);
      const char *kind=dvm_class_name(dvm_object_class(vm,source));
      fprintf(stderr,"[tree-text] ref=%u source=%s color=%08x text=%.160s\n",
         (unsigned)view,kind?kind:"?",(unsigned)ui_int(vm,view,"textColor"),text?text:"");
   }
   dvm_ref kids[UI_MAX_KIDS];
   const uint32_t nk = ui_children(vm, view, kids, UI_MAX_KIDS);
   for (uint32_t i = 0; i < nk; ++i) ui_dump_tree(vm, kids[i], depth + 1);
}

/* View.updateDisplayListIfDirty / child drawing computes animated scroll
 * before applying scrollX/Y to content. Invisible subtrees do not draw. */
static bool ui_compute_scroll(struct dvm *vm,dvm_ref view,unsigned depth)
{
   if(!view || depth>48 || ui_int(vm,view,"visibility")!=0) return true;
   struct dvm_class *cls=dvm_object_class(vm,view);
   struct dvm_method *compute=cls ? dvm_find_method(vm,cls,"computeScroll","()V") : NULL;
   if(compute && compute->builtin!=empty_void) {
      union dvm_value ignored={0};
      if(!dvm_call(vm,compute,view,NULL,0,&ignored)) return false;
   }
   dvm_ref children[UI_MAX_KIDS];
   uint32_t count=ui_children(vm,view,children,UI_MAX_KIDS);
   for(uint32_t i=0;i<count;++i)
      if(!ui_compute_scroll(vm,children[i],depth+1)) return false;
   return true;
}


static void ui_rebuild(struct dvm *vm)
{
   uint64_t started=ui_render_ns();
   g_ui_draw_trace_budget=0;
   const char *trigger=getenv("LUNARIA_VIEW_DRAW_TRIGGER");
   if(trigger && *trigger) {
      FILE *request=luna_file_fopen(trigger,"rb");
      if(request) {
         fclose(request);
         if(luna_file_unlink(trigger)==0) g_ui_draw_trace_budget=4096;
      }
   }
   g_ui_dirty = false;
   if (!g_ui_nwindows) {
      luna_overlay_set_document(NULL, NULL);
      return;
   }
   /* A window this layer has no content view for is not a window it can show:
    * nothing is drawn for it and nothing in it can be pressed.  Publishing a
    * document for it anyway is worse than publishing none, because a guest
    * document makes the overlay *modal* — luna_overlay_pointer() then takes
    * every touch and the application below never sees one again.  That is how
    * Cross Worlds' account-link dialog stopped the title being got past:
    * the dialog on screen was the game's own, drawn by the game, and every
    * press on it was being eaten by an Android window the emulator could not
    * materialise ("[overlay] took pointer ... a guest dialog is up and is
    * modal"). */
   int drawable = 0;
   for (int i = 0; i < g_ui_nwindows; ++i)
      if (ui_presented_window_content(vm, g_ui_windows[i])) ++drawable;
   if (!drawable) {
      luna_overlay_set_document(NULL, NULL);
      return;
   }
   struct ui_buf b = { 0 }, css = { 0 };
   g_ui_building = true;
   /* What this rebuild repaints; what changes while it runs is the next one's. */
   g_ui_frame_damage = g_ui_damage;
   g_ui_damage = (struct ui_damage){ 0 };
   ++g_ui_build_seq;
   if (g_ui_measure_dirty) ui_nodes_reset();
   ui_puts(&css, ui_stylesheet());
   ui_puts(&b, "<body>");
   /* The dim behind belongs to the windows that asked for it, not to the act
    * of having a window up.  A window without FLAG_DIM_BEHIND does not dim on
    * a device, and painting one anyway also takes every touch meant for the
    * app underneath — which is how a banner that says "press X to play" ended
    * up making the game unplayable. */
   /* And it belongs to a window that is actually *there*.  A Dialog whose
    * content view this layer never got (it was built from something the
    * inflater does not produce a view tree for) has nothing to draw and
    * nothing to press, so the loop below emits no window for it — but it
    * still answered "I dim", and the scrim alone is a full-screen element
    * with no way past it: every touch lands on the dim, `ui_click_from_
    * overlay` answers "not cancelable", and the game underneath — which is
    * drawing its own dialog with its own renderer — never sees a press again.
    * That is Cross Worlds' account-link screen: the × is the game's, the
    * scrim was ours, and the title could not be got past. */
   bool dim = false;
   for (int i = 0; i < g_ui_nwindows; ++i)
      if (ui_window_dims(vm, g_ui_windows[i]) &&
          ui_presented_window_content(vm, g_ui_windows[i]))
         dim = true;
   if (dim) ui_puts(&b, "<div id=\"scrim\" class=\"scrim\"></div>");
   for (int i = 0; i < g_ui_nwindows; ++i) {
      dvm_ref content = ui_presented_window_content(vm, g_ui_windows[i]);
      if (!content) continue;
      ui_printf(&b, "<div id=\"win%u\" class=\"root\">", (unsigned)g_ui_windows[i]);
      uint64_t phase=started ? ui_render_ns() : 0;
      ui_emit_window_box(vm, &css, g_ui_windows[i]);
      /* Window layout has now measured and positioned this tree.  Dumping
       * before it reports stale boxes (or zeroes for newly added children). */
      if (dvm__env("LUNARIA_UI_TREE")) ui_dump_tree(vm, content, 0);
      if(started) {uint64_t now=ui_render_ns();g_ui_render_prof.layout_ns+=now-phase;phase=now;}
      if(!ui_compute_scroll(vm,content,0)) ui_report_exception(vm,"computeScroll");
      if(started) {uint64_t now=ui_render_ns();g_ui_render_prof.scroll_ns+=now-phase;phase=now;}
      ui_emit_view(vm, &b, &css, content, -1, 0);
      if(started) g_ui_render_prof.emit_ns+=ui_render_ns()-phase;
      if (dvm_exception(vm)) {
         g_ui_building=false;
         g_ui_damage.full = true;
         free(b.p); free(css.p);
         return;
      }
      ui_puts(&b, "</div>");
   }
   ui_puts(&b, "</body>");
   g_ui_building = false;
   g_ui_measure_dirty = false;
   if (b.p && css.p) {
      if (getenv("LUNARIA_UI_DUMP")) {
         FILE *df = luna_file_fopen("/tmp/lunaria-ui.html", "w");
         if (df) {
            fprintf(df, "<!DOCTYPE html><html><head><meta charset=utf-8>"
                    "<style>%s</style></head>%s</html>\n", css.p, b.p);
            fclose(df);
            fprintf(stderr, "[ui] document dumped (%zu+%zu bytes) -> "
                    "/tmp/lunaria-ui.html\n", b.len, css.len);
         }
      }
      luna_overlay_set_document(b.p, css.p);
   }
   free(b.p);
   free(css.p);
   if(started) {
      ++g_ui_render_prof.builds;g_ui_render_prof.build_ns+=ui_render_ns()-started;
      if(g_ui_render_prof.builds==1 || g_ui_render_prof.builds%10==0) {
         fprintf(stderr,"[ui-stage] layout=%.3f scroll=%.3f emit=%.3f ms/build\n",
            g_ui_render_prof.layout_ns/1e6/g_ui_render_prof.builds,
            g_ui_render_prof.scroll_ns/1e6/g_ui_render_prof.builds,
            g_ui_render_prof.emit_ns/1e6/g_ui_render_prof.builds);
         fprintf(stderr,"[ui-render-prof] %llu builds %.3f ms/build; %llu captures %.3f ms/draw %.3f ms/publish\n",
            (unsigned long long)g_ui_render_prof.builds,g_ui_render_prof.build_ns/1e6/g_ui_render_prof.builds,
            (unsigned long long)g_ui_render_prof.draws,g_ui_render_prof.draws ? g_ui_render_prof.draw_ns/1e6/g_ui_render_prof.draws : 0,
            g_ui_render_prof.draws ? g_ui_render_prof.publish_ns/1e6/g_ui_render_prof.draws : 0);
      }
   }
}

/* Android finds the deepest view under the touch that is clickable, which for
 * a listener set in code means the nearest ancestor that has one. */
/* A view acts on a touch when it has a listener, when it was made clickable,
 * or when it is a CompoundButton — which is clickable by construction, because
 * pressing it is how it changes state. */
static bool ui_is_clickable(struct dvm *vm, dvm_ref view)
{
   return ui_obj(vm, view, "clickListener",
                 "Landroid/view/View$OnClickListener;") != 0 ||
          ui_int(vm, view, "clickable") != 0 ||
          ui_is_a(vm, view, "android/widget/CompoundButton") ||
          /* Button / ImageButton are clickable by construction on a device,
           * even before a listener is attached.  WebViewDialog's close control
           * is exactly that shape. */
          ui_is_a(vm, view, "android/widget/Button") ||
          ui_is_a(vm, view, "android/widget/ImageButton");
}

static dvm_ref ui_click_target(struct dvm *vm, dvm_ref view)
{
   for (int guard = 0; view && guard < 32; ++guard) {
      /* A disabled view swallows the touch without acting on it, and so does
       * everything inside it; the press does not fall through to a parent. */
      if (ui_int(vm, view, "disabled")) return 0;
      if (ui_is_clickable(vm, view)) return view;
      view = ui_obj(vm, view, "parent", "Landroid/view/ViewParent;");
   }
   return 0;
}

/* View.performClick(), in the order the platform runs it: CompoundButton
 * toggles first and only then lets the OnClickListener see the click, so a
 * listener that reads isChecked() gets the state the press produced. */
static bool view_request_focus(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out);
static bool imm_show_soft_input(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out);

static bool dialog_button_click(struct dvm *vm, dvm_ref button);
static bool picker_button_click(struct dvm *vm, dvm_ref button);

static bool list_view_tap(struct dvm *,dvm_ref,dvm_ref);
static void ui_dispatch_click(struct dvm *vm, dvm_ref clicked)
{
   /* A tap on an enabled EditText focuses it and brings up the input method
    * for it (TextView.onTouchEvent -> requestFocus + showSoftInput), before
    * any click listener it may also have. */
   if (clicked && ui_is_a(vm, clicked, "android/widget/EditText") &&
       !ui_int(vm, clicked, "disabled")) {
      union dvm_value r = { 0 };
      (void)view_request_focus(vm, clicked, NULL, 0, &r);
      union dvm_value a[2] = { { .l = clicked }, { .i = 0 } };
      (void)imm_show_soft_input(vm, 0, a, 2, &r);
      if (!ui_obj(vm, clicked, "clickListener", "Landroid/view/View$OnClickListener;"))
         return;
   }
   dvm_ref target = ui_click_target(vm, clicked);
   if (!target) {
      fprintf(stderr, "[ui] click on v%u: nothing clickable up the tree:", (unsigned)clicked);
      dvm_ref up = clicked;
      for (int guard = 0; up && guard < 6; ++guard) {
         struct dvm_class *k = dvm_object_class(vm, up);
         fprintf(stderr, " %s%s", k ? dvm_class_name(k) : "?",
                 ui_obj(vm, up, "clickListener", "Landroid/view/View$OnClickListener;") ? "(listener)" : "");
         up = ui_obj(vm, up, "parent", "Landroid/view/ViewParent;");
      }
      fprintf(stderr, "\n");
      return;
   }
   if(ui_is_a(vm,target,"android/widget/ListView")) {
      if(!list_view_tap(vm,target,clicked))ui_report_exception(vm,"performItemClick");
      return;
   }
   if (ui_is_a(vm, target, "android/widget/CompoundButton")) {
      union dvm_value ignored = { 0 };
      if (!am_vcall(vm,target,"toggle","()V",NULL,0,&ignored))
         ui_report_exception(vm, "toggle");
   }
   dvm_ref listener = ui_obj(vm, target, "clickListener",
                             "Landroid/view/View$OnClickListener;");
   if (!listener) {
      if (picker_button_click(vm, target) || dialog_button_click(vm, target)) return;
      fprintf(stderr, "[ui] click v%u (no OnClickListener)\n", (unsigned)target);
      return;
   }
   struct dvm_class *c = dvm_object_class(vm, listener);
   struct dvm_method *m =
      c ? dvm_find_method(vm, c, "onClick", "(Landroid/view/View;)V") : NULL;
   fprintf(stderr, "[ui] click v%u -> %s.onClick\n", (unsigned)target,
           c ? dvm_class_name(c) : "?");
   if (!m) return;
   union dvm_value a = { .l = target }, ignored = { 0 };
   if (!dvm_call(vm, m, listener, &a, 1, &ignored))
      ui_report_exception(vm, "onClick");
}

/* callOnClick only delivers the listener callback: it does not toggle a
 * CompoundButton, play a sound or dispatch the normal click side effects. */
static bool view_call_on_click(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref listener=ui_obj(vm,self,"clickListener","Landroid/view/View$OnClickListener;");
   if (!listener) RETI(0);
   struct dvm_class *cls=dvm_object_class(vm,listener);
   struct dvm_method *method=cls ? dvm_find_method(vm,cls,"onClick","(Landroid/view/View;)V") : NULL;
   if (!method) {
      dvm__throw(vm,"java/lang/AbstractMethodError","OnClickListener.onClick");
      return false;
   }
   union dvm_value arg={.l=self},ignored={0};
   if (!dvm_call(vm,method,listener,&arg,1,&ignored)) return false;
   RETI(1);
}

static bool view_perform_click(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   if (ui_is_a(vm, self, "android/widget/CompoundButton")) {
      union dvm_value ignored = { 0 };
      if (!am_vcall(vm,self,"toggle","()V",NULL,0,&ignored)) return false;
   }
   dvm_ref listener = ui_obj(vm, self, "clickListener",
                             "Landroid/view/View$OnClickListener;");
   if (listener) {
      struct dvm_class *c = dvm_object_class(vm, listener);
      struct dvm_method *m = c ? dvm_find_method(vm, c, "onClick",
                                   "(Landroid/view/View;)V") : NULL;
      union dvm_value a = { .l = self }, ignored = { 0 };
      if (m && !dvm_call(vm, m, listener, &a, 1, &ignored)) return false;
      RETI(1);
   }
   RETI(picker_button_click(vm, self) || dialog_button_click(vm, self));
}

/* --- what the rest of the runtime calls -------------------------------- */

/* Rebuilding the document runs guest code (a RecyclerView's adapter creates
 * and binds its item views), and that code sets exactly the fields this
 * watches.  Without the guard the rebuild would mark itself stale for ever. */
static void ui_mark_dirty(void)
{
   if (g_ui_nwindows && !g_ui_building) g_ui_dirty = true;
}

/* A view changed in a way that changes pixels. */
static void ui_invalidate(void)
{
   if (g_ui_nwindows && !g_ui_building) {ui_damage_full();g_ui_dirty = true;}
}

static void ui_layout_changed(void)
{
   ++g_ui_measure_epoch;
   if (g_ui_building) return;
   g_ui_measure_dirty = true;
   ui_invalidate();
}

static void ui_view_layout_changed(struct dvm *vm,dvm_ref view)
{
   /* A child request must invalidate cached ancestor measurements as well;
    * unchanged sibling branches keep their measured dimensions/state. */
   for(unsigned depth=0;view && depth<64;++depth) {
      rt_set_int(vm,view,"layoutRequired",1);
      view=ui_obj(vm,view,"parent","Landroid/view/ViewParent;");
   }
   if(g_ui_building) return;
   g_ui_measure_dirty=true;ui_invalidate();
}

static void ui_window_shown(struct dvm *vm, dvm_ref window)
{
   g_ui_vm = vm;
   luna_overlay_set_scroll_handler(ui_scroll_from_overlay);
   g_ui_measure_dirty = true;
   for (int i = 0; i < g_ui_nwindows; ++i)
      if (g_ui_windows[i] == window) { g_ui_dirty = true; return; }
   if (g_ui_nwindows >= UI_MAX_WINDOWS) return;
   luna_overlay_set_click_handler(ui_click_from_overlay);
   luna_overlay_set_web_pointer_handler(webview_pointer_from_overlay);
   luna_overlay_set_web_text_handlers(webview_char_from_overlay,
                                      webview_key_from_overlay,
                                      webview_text_from_overlay);
   dvm_pin(vm, window);
   g_ui_windows[g_ui_nwindows++] = window;
   fprintf(stderr, "[ui] window %u shown (%d up)\n", (unsigned)window,
           g_ui_nwindows);
   g_ui_dirty = true;
}

static void ui_window_hidden(struct dvm *vm, dvm_ref window)
{
   for (int i = 0; i < g_ui_nwindows; ++i) {
      if (g_ui_windows[i] != window) continue;
      dvm_ref content = ui_window_root(vm, window);
      if (content && !view_dispatch_attachment(vm, content, false))
         ui_report_exception(vm, "onDetachedFromWindow");
      memmove(&g_ui_windows[i], &g_ui_windows[i + 1],
              (size_t)(g_ui_nwindows - i - 1) * sizeof g_ui_windows[0]);
      --g_ui_nwindows;
      dvm_unpin(vm, window);
      fprintf(stderr, "[ui] window %u hidden (%d up)\n", (unsigned)window,
              g_ui_nwindows);
      g_ui_dirty = true;
      return;
   }
}



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
