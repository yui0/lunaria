/* MPL-2.0. Vector's public operations share the runtime's sequence storage. */
static int32_t vector_increment(struct dvm *vm,dvm_ref self)
{
   union dvm_value v={0};dvm__get_field_from(vm,self,dvm_find_class(vm,"java/util/Vector"),"capacityIncrement","I",&v);return v.i;
}
static bool vector_capacity_set(struct dvm *vm,dvm_ref self,uint32_t capacity)
{
   struct dvm_object *o=dvm__obj(vm,self);struct rt_list *old=o?o->data:NULL;
   if (!o || capacity>INT32_MAX || capacity>(SIZE_MAX-sizeof(struct rt_list))/sizeof(dvm_ref)) {
      dvm__throw(vm,"java/lang/OutOfMemoryError","Vector capacity");return false;
   }
   uint32_t previous=old?old->cap:0;
   struct rt_list *next=realloc(old,sizeof *next+(size_t)capacity*sizeof(dvm_ref));
   if(!next){dvm__throw(vm,"java/lang/OutOfMemoryError","Vector storage");return false;}
   if(!old){next->size=0;next->mod_count=0;}
   if(capacity>previous)memset(next->items+previous,0,(size_t)(capacity-previous)*sizeof(dvm_ref));
   next->cap=capacity;o->data=next;return true;
}
static bool vector_reserve(struct dvm *vm,dvm_ref self,uint32_t needed)
{
   struct rt_list *v=list_of(vm,self);uint32_t old=v?v->cap:0;
   if(needed<=old && v)return true;
   int32_t increment=vector_increment(vm,self);
   uint64_t grown=(uint64_t)old+(increment>0?(uint32_t)increment:old);
   if(grown<needed)grown=needed;if(grown>INT32_MAX)grown=INT32_MAX;
   if(needed>INT32_MAX){dvm__throw(vm,"java/lang/OutOfMemoryError","Vector size");return false;}
   return vector_capacity_set(vm,self,(uint32_t)grown);
}
static bool vector_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *sig=builtin_sig();bool collection=nargs && strstr(sig,"Ljava/util/Collection;");
   dvm_ref snapshot=0;uint32_t size=0;int32_t capacity=nargs&&!collection?ARG(0).i:10;
   int32_t increment=nargs>1?ARG(1).i:0;
   if(collection){
      if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","collection");return false;}
      union dvm_value r={0};if(!am_vcall(vm,ARG(0).l,"toArray","()[Ljava/lang/Object;",NULL,0,&r))return false;
      struct dvm_object *a=dvm__obj(vm,r.l);
      if(!a || a->kind!=DVM_OBJ_ARRAY || a->elem_kind!='L'){dvm__throw(vm,"java/lang/NullPointerException","collection array");return false;}
      snapshot=r.l;size=a->length;capacity=(int32_t)size;dvm_pin(vm,snapshot);
   }
   if(capacity<0){dvm__throw(vm,"java/lang/IllegalArgumentException","negative initial capacity");return false;}
   struct dvm_object *o=dvm__obj(vm,self);struct rt_list *old=list_of(vm,self);
   for(uint32_t i=0;old && i<old->size;++i)dvm_unpin(vm,old->items[i]);
   if(o){free(o->data);o->data=NULL;}
   dvm__set_field_from(vm,self,dvm_find_class(vm,"java/util/Vector"),"capacityIncrement","I",(union dvm_value){.i=increment});
   if(!vector_capacity_set(vm,self,(uint32_t)capacity)){if(snapshot)dvm_unpin(vm,snapshot);return false;}
   struct rt_list *v=list_of(vm,self);v->size=size;
   if(size){memcpy(v->items,dvm_array_data(vm,snapshot),(size_t)size*sizeof(dvm_ref));for(uint32_t i=0;i<size;++i)dvm_pin(vm,v->items[i]);}
   if(snapshot)dvm_unpin(vm,snapshot);RETV();
}
static bool vector_bounds(struct dvm *vm,dvm_ref self,int32_t index,bool insertion)
{
   struct rt_list *v=list_of(vm,self);uint32_t size=v?v->size:0;
   if(index<0 || (uint32_t)index>size || (!insertion&&(uint32_t)index==size)) {
      dvm__throw(vm,"java/lang/ArrayIndexOutOfBoundsException","index %d size %u",index,size);return false;
   }return true;
}
static bool vector_insert_at(struct dvm *vm,dvm_ref self,int32_t index,dvm_ref value)
{
   if(!vector_bounds(vm,self,index,true))return false;
   struct rt_list *v=list_of(vm,self);uint32_t size=v?v->size:0;
   if(!vector_reserve(vm,self,size+1))return false;
   v=list_of(vm,self);memmove(v->items+index+1,v->items+index,(size-index)*sizeof(dvm_ref));
   v->items[index]=value;v->size=size+1;++v->mod_count;dvm_pin(vm,value);return true;
}
static bool vector_append(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;struct rt_list *v=list_of(vm,self);if(!vector_insert_at(vm,self,v?(int32_t)v->size:0,ARG(0).l))return false;RETI(1);}
static bool vector_insert(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if(!strcmp(builtin_name(),"add")){
      union dvm_value pair[2]={ARG(1),ARG(0)};
      return am_vcall(vm,self,"insertElementAt","(Ljava/lang/Object;I)V",pair,2,out);
   }
   if(!vector_insert_at(vm,self,ARG(1).i,ARG(0).l))return false;RETV();
}
static bool vector_read(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct rt_list *v=list_of(vm,self);const char *name=builtin_name();
   if(!strcmp(name,"capacity"))RETI(v?(int32_t)v->cap:0);
   if(!strcmp(name,"size"))RETI(v?(int32_t)v->size:0);
   if(!strcmp(name,"isEmpty"))RETI(!v || !v->size);
   int32_t index=nargs?ARG(0).i:0;
   if(!strcmp(name,"firstElement") || !strcmp(name,"lastElement")){
      if(!v || !v->size){dvm__throw(vm,"java/util/NoSuchElementException","empty Vector");return false;}
      index=!strcmp(name,"firstElement")?0:(int32_t)v->size-1;
   }
   if(!vector_bounds(vm,self,index,false))return false;RETL(list_of(vm,self)->items[index]);
}
static bool vector_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;bool legacy=!strcmp(builtin_name(),"setElementAt");int32_t index=ARG(legacy?1:0).i;
   dvm_ref value=ARG(legacy?0:1).l;if(!vector_bounds(vm,self,index,false))return false;
   struct rt_list *v=list_of(vm,self);dvm_ref old=v->items[index];v->items[index]=value;
   dvm_pin(vm,value);dvm_unpin(vm,old);RETL(old);
}
static bool vector_remove_at(struct dvm *vm,dvm_ref self,int32_t index,union dvm_value *out)
{
   if(!vector_bounds(vm,self,index,false))return false;
   struct rt_list *v=list_of(vm,self);dvm_ref old=v->items[index];
   memmove(v->items+index,v->items+index+1,(v->size-index-1)*sizeof(dvm_ref));
   v->items[--v->size]=0;++v->mod_count;dvm_unpin(vm,old);RETL(old);
}
static bool vector_index(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   bool reverse=!strcmp(builtin_name(),"lastIndexOf");
   if(!reverse && nargs==1){union dvm_value pair[2]={ARG(0),{.i=0}};return am_vcall(vm,self,"indexOf","(Ljava/lang/Object;I)I",pair,2,out);}
   struct rt_list *v=list_of(vm,self);int32_t size=v?(int32_t)v->size:0;
   int32_t at=nargs>1?ARG(1).i:reverse?size-1:0;
   if((!reverse&&at<0)||(reverse&&at>=size)){dvm__throw(vm,"java/lang/ArrayIndexOutOfBoundsException","index %d",at);return false;}
   for(int32_t i=at;i>=0;i+=reverse?-1:1){
      v=list_of(vm,self);if(!v || (uint32_t)i>=v->size){if(reverse)continue;break;}
      bool same=key_eq(vm,ARG(0).l,v->items[i]);if(dvm_exception(vm))return false;if(same)RETI(i);
      if(i==INT32_MAX)break;
   }RETI(-1);
}
static bool vector_contains(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{union dvm_value found={0};if(!am_vcall(vm,self,"indexOf","(Ljava/lang/Object;)I",args,nargs,&found))return false;RETI(found.i>=0);}
static bool vector_remove(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   const char *name=builtin_name();
   if(!strcmp(name,"removeElementAt") || !strcmp(builtin_sig(),"(I)Ljava/lang/Object;"))return vector_remove_at(vm,self,ARG(0).i,out);
   if(!strcmp(name,"remove"))return am_vcall(vm,self,"removeElement","(Ljava/lang/Object;)Z",args,nargs,out);
   struct rt_list *v=list_of(vm,self);if(v)++v->mod_count;
   union dvm_value found={0};if(!am_vcall(vm,self,"indexOf","(Ljava/lang/Object;)I",args,1,&found))return false;
   if(found.i<0)RETI(0);union dvm_value ignored={0};if(!vector_remove_at(vm,self,found.i,&ignored))return false;RETI(1);
}
static bool vector_clear(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   if(!strcmp(builtin_name(),"clear"))return am_vcall(vm,self,"removeAllElements","()V",NULL,0,out);
   struct rt_list *v=list_of(vm,self);
   if(v){for(uint32_t i=0;i<v->size;++i)dvm_unpin(vm,v->items[i]);memset(v->items,0,(size_t)v->size*sizeof(dvm_ref));v->size=0;++v->mod_count;}RETV();
}
static bool vector_resize(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct rt_list *v=list_of(vm,self);const char *name=builtin_name();
   if(!strcmp(name,"trimToSize")){if(v)++v->mod_count;if(!vector_capacity_set(vm,self,v?v->size:0))return false;RETV();}
   int32_t size=ARG(0).i;
   if(!strcmp(name,"ensureCapacity")){if(size>0){if(v)++v->mod_count;if(!vector_reserve(vm,self,(uint32_t)size))return false;}RETV();}
   if(v)++v->mod_count;if(size<0){dvm__throw(vm,"java/lang/ArrayIndexOutOfBoundsException","negative Vector size");return false;}
   uint32_t previous=v?v->size:0;if(!vector_reserve(vm,self,(uint32_t)size))return false;v=list_of(vm,self);
   for(uint32_t i=(uint32_t)size;i<previous;++i){dvm_unpin(vm,v->items[i]);v->items[i]=0;}
   if((uint32_t)size>previous)memset(v->items+previous,0,((uint32_t)size-previous)*sizeof(dvm_ref));v->size=(uint32_t)size;RETV();
}
static bool vector_copy_into(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct dvm_object *array=dvm__obj(vm,ARG(0).l);struct rt_list *v=list_of(vm,self);
   if(!array){dvm__throw(vm,"java/lang/NullPointerException","array");return false;}
   if(array->kind!=DVM_OBJ_ARRAY || array->elem_kind!='L'){dvm__throw(vm,"java/lang/ArrayStoreException","destination");return false;}
   uint32_t size=v?v->size:0;if(array->length<size){dvm__throw(vm,"java/lang/ArrayIndexOutOfBoundsException","destination too short");return false;}
   struct dvm_class *element=array->cls?array->cls->elem:NULL;
   for(uint32_t i=0;i<size;++i){dvm_ref item=v->items[i];if(item && element && !dvm__class_assignable(vm,dvm_object_class(vm,item),element)){dvm__throw(vm,"java/lang/ArrayStoreException","Vector element");return false;}((dvm_ref *)array->data)[i]=item;}
   RETV();
}
static bool vector_to_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{if(nargs && !ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","array");return false;}return list_toArray(vm,self,args,nargs,out);}
static bool vector_clone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value result={0};if(!o_clone(vm,self,args,nargs,&result) || !result.l)return false;
   struct rt_list *v=list_of(vm,self);uint32_t size=v?v->size:0;
   if(!vector_capacity_set(vm,result.l,size))return false;struct rt_list *copy=list_of(vm,result.l);copy->size=size;copy->mod_count=0;
   if(size)memcpy(copy->items,v->items,(size_t)size*sizeof(dvm_ref));for(uint32_t i=0;i<size;++i)dvm_pin(vm,copy->items[i]);RETL(result.l);
}
static bool vector_add_all(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   int32_t index=nargs>1?ARG(0).i:(int32_t)(list_of(vm,self)?list_of(vm,self)->size:0);
   if(!vector_bounds(vm,self,index,true))return false;
   struct dvm_object *source=collection_snapshot(vm,ARG(nargs>1?1:0).l);if(!source)return false;
   uint32_t count=source->length;dvm_ref *snapshot=malloc((size_t)(count?count:1)*sizeof *snapshot);
   if(!snapshot){dvm__throw(vm,"java/lang/OutOfMemoryError","collection snapshot");return false;}
   if(count)memcpy(snapshot,source->data,(size_t)count*sizeof *snapshot);
   struct rt_list *v=list_of(vm,self);uint32_t size=v?v->size:0;
   if((uint32_t)index>size){free(snapshot);dvm__throw(vm,"java/util/ConcurrentModificationException","Vector changed");return false;}
   if(count>INT32_MAX-size || !vector_reserve(vm,self,size+count)){free(snapshot);if(!dvm_exception(vm))dvm__throw(vm,"java/lang/OutOfMemoryError","Vector size");return false;}
   v=list_of(vm,self);++v->mod_count;
   memmove(v->items+index+count,v->items+index,(size-index)*sizeof(dvm_ref));
   if(count)memcpy(v->items+index,snapshot,(size_t)count*sizeof(dvm_ref));v->size=size+count;
   for(uint32_t i=0;i<count;++i)dvm_pin(vm,snapshot[i]);free(snapshot);RETI(count!=0);
}
static bool vector_contains_all(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct dvm_object *source=collection_snapshot(vm,ARG(0).l);if(!source)return false;
   for(uint32_t i=0;i<source->length;++i){union dvm_value item={.l=((dvm_ref *)source->data)[i]},present={0};
      if(!am_vcall(vm,self,"contains","(Ljava/lang/Object;)Z",&item,1,&present))return false;if(!present.i)RETI(0);
   }RETI(1);
}
static bool vector_bulk(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","collection");return false;}
   bool retain=!strcmp(builtin_name(),"retainAll"),changed=false;
   for(uint32_t i=0;;){
      struct rt_list *v=list_of(vm,self);if(!v || i>=v->size)break;
      uint32_t version=v->mod_count;union dvm_value item={.l=v->items[i]},present={0};
      if(!am_vcall(vm,ARG(0).l,"contains","(Ljava/lang/Object;)Z",&item,1,&present))return false;
      v=list_of(vm,self);if(!v || v->mod_count!=version){dvm__throw(vm,"java/util/ConcurrentModificationException","Vector changed during contains");return false;}
      if((present.i!=0)==retain){++i;continue;}
      union dvm_value ignored={0};if(!vector_remove_at(vm,self,(int32_t)i,&ignored))return false;changed=true;
   }RETI(changed);
}
static bool vector_value(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;bool hash=!strcmp(builtin_name(),"hashCode");
   if(!hash){if(self==ARG(0).l)RETI(1);if(!ARG(0).l || !dvm__class_assignable(vm,dvm_object_class(vm,ARG(0).l),dvm_find_class(vm,"java/util/List")))RETI(0);}
   union dvm_value iterator={0};
   if(!hash && !am_vcall(vm,ARG(0).l,"iterator","()Ljava/util/Iterator;",NULL,0,&iterator))return false;
   uint32_t h=1;
   for(uint32_t i=0;;++i){
      struct rt_list *v=list_of(vm,self);if(!v || i>=v->size)break;
      uint32_t version=v->mod_count;dvm_ref item=v->items[i];
      if(hash){uint32_t part=0;if(!map_item_hash(vm,item,&part))return false;h=31*h+part;}
      else{union dvm_value more={0},other={0};if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&more))return false;if(!more.i)RETI(0);
         if(!am_vcall(vm,iterator.l,"next","()Ljava/lang/Object;",NULL,0,&other))return false;
         bool same=key_eq(vm,item,other.l);if(dvm_exception(vm))return false;if(!same)RETI(0);
      }
      v=list_of(vm,self);if(!v || v->mod_count!=version){dvm__throw(vm,"java/util/ConcurrentModificationException","Vector changed during value callback");return false;}
   }
   if(hash)RETI((int32_t)h);union dvm_value more={0};if(!am_vcall(vm,iterator.l,"hasNext","()Z",NULL,0,&more))return false;RETI(!more.i);
}
/* Vector Enumeration is live and deliberately not fail-fast. Iterator and
 * ListIterator check structural changes and support changes through themselves. */
static bool vector_iterator(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   bool enumeration=!strcmp(builtin_name(),"elements");int32_t at=nargs?ARG(0).i:0;
   struct rt_list *v=list_of(vm,self);int32_t size=v?(int32_t)v->size:0;
   if(at<0 || at>size){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","iterator index %d",at);return false;}
   dvm_ref it=dvm_new_object(vm,dvm_find_class(vm,enumeration?"lunaria/util/VectorEnumeration":!strcmp(builtin_name(),"listIterator")?"lunaria/util/VectorListIterator":"lunaria/util/VectorIterator"));
   if(!it)return false;dvm_set_field(vm,it,"target","Ljava/lang/Object;",(union dvm_value){.l=self});
   rt_set_int(vm,it,"index",at);rt_set_int(vm,it,"last",-1);rt_set_int(vm,it,"limit",size);rt_set_int(vm,it,"expected",(int32_t)(list_of(vm,self)?list_of(vm,self)->mod_count:0));RETL(it);
}
static bool vector_iterator_step(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value target={0};dvm_get_field(vm,self,"target","Ljava/lang/Object;",&target);
   const char *name=builtin_name();bool enumeration=dvm_object_class(vm,self)==dvm_find_class(vm,"lunaria/util/VectorEnumeration");
   struct rt_list *v=list_of(vm,target.l);int32_t size=v?(int32_t)v->size:0,at=rt_get_int(vm,self,"index"),last=rt_get_int(vm,self,"last");
   if(!strcmp(name,"hasMoreElements"))RETI(at<size);
   if(!strcmp(name,"hasNext"))RETI(at<rt_get_int(vm,self,"limit"));
   if(!strcmp(name,"hasPrevious"))RETI(at>0);
   if(!strcmp(name,"nextIndex"))RETI(at);
   if(!strcmp(name,"previousIndex"))RETI(at-1);
   if((!strcmp(name,"remove")||!strcmp(name,"set")) && last<0){dvm__throw(vm,"java/lang/IllegalStateException","no iterator element");return false;}
   if(!dvm__monitor_enter(vm,target.l))return false;
   bool ok=false;
   v=list_of(vm,target.l);size=v?(int32_t)v->size:0;
   if(!enumeration && v && v->mod_count!=(uint32_t)rt_get_int(vm,self,"expected")){dvm__throw(vm,"java/util/ConcurrentModificationException","Vector iterator");goto done;}
   if(!strcmp(name,"next")||!strcmp(name,"nextElement")||!strcmp(name,"previous")){
      bool back=!strcmp(name,"previous");int32_t index=back?at-1:at;
      int32_t bound=enumeration?size:rt_get_int(vm,self,"limit");
      if(index<0 || index>=bound){dvm__throw(vm,"java/util/NoSuchElementException","Vector iterator");goto done;}
      out->l=v->items[index];rt_set_int(vm,self,"index",back?index:index+1);rt_set_int(vm,self,"last",index);ok=true;
   }else if(!strcmp(name,"add")){
      if(!vector_insert_at(vm,target.l,at,ARG(0).l))goto done;rt_set_int(vm,self,"index",at+1);rt_set_int(vm,self,"last",-1);rt_set_int(vm,self,"limit",rt_get_int(vm,self,"limit")+1);ok=true;
   }else{
      if(last<0){dvm__throw(vm,"java/lang/IllegalStateException","no iterator element");goto done;}
      if(!strcmp(name,"remove")){union dvm_value ignored={0};if(!vector_remove_at(vm,target.l,last,&ignored))goto done;if(last<at)rt_set_int(vm,self,"index",at-1);rt_set_int(vm,self,"last",-1);rt_set_int(vm,self,"limit",rt_get_int(vm,self,"limit")-1);ok=true;}
      else if(!strcmp(name,"set")){dvm_ref old=v->items[last];v->items[last]=ARG(0).l;dvm_pin(vm,ARG(0).l);dvm_unpin(vm,old);ok=true;}
   }
   if(ok)rt_set_int(vm,self,"expected",(int32_t)list_of(vm,target.l)->mod_count);
 done:
   if(!dvm__monitor_exit(vm,target.l))ok=false;return ok;
}
static const struct rt_field rt_vector_fields[]={{"capacityIncrement","I"},{"backing","Ljava/lang/Object;"},F_END};
static const struct rt_field rt_vector_iterator_fields[]={{"target","Ljava/lang/Object;"},{"index","I"},{"last","I"},{"limit","I"},{"expected","I"},F_END};
static const char *const rt_vector_ifaces[]={"Ljava/util/List;","Ljava/util/RandomAccess;","Ljava/lang/Cloneable;","Ljava/io/Serializable;",NULL};
static const char *const rt_vector_enumeration_ifaces[]={"Ljava/util/Enumeration;",NULL};
static const char *const rt_vector_iterator_ifaces[]={"Ljava/util/Iterator;",NULL};
static const char *const rt_vector_list_iterator_ifaces[]={"Ljava/util/ListIterator;",NULL};
#define VS(n,s,f) {n,s,f,DEX_ACC_PUBLIC|DEX_ACC_SYNCHRONIZED}
#define VP(n,s,f) {n,s,f,DEX_ACC_PUBLIC}
static const struct rt_method rt_vector[]={
 VP("<init>","()V",vector_init),VP("<init>","(I)V",vector_init),VP("<init>","(II)V",vector_init),VP("<init>","(Ljava/util/Collection;)V",vector_init),
 VS("addElement","(Ljava/lang/Object;)V",vector_append),VS("add","(Ljava/lang/Object;)Z",vector_append),
 VP("add","(ILjava/lang/Object;)V",vector_insert),VS("insertElementAt","(Ljava/lang/Object;I)V",vector_insert),
 VS("elementAt","(I)Ljava/lang/Object;",vector_read),VS("get","(I)Ljava/lang/Object;",vector_read),
 VS("size","()I",vector_read),VS("capacity","()I",vector_read),VS("isEmpty","()Z",vector_read),
 VS("firstElement","()Ljava/lang/Object;",vector_read),VS("lastElement","()Ljava/lang/Object;",vector_read),
 VS("setElementAt","(Ljava/lang/Object;I)V",vector_set),VS("set","(ILjava/lang/Object;)Ljava/lang/Object;",vector_set),
 VS("removeElementAt","(I)V",vector_remove),VS("removeElement","(Ljava/lang/Object;)Z",vector_remove),
 VP("remove","(Ljava/lang/Object;)Z",vector_remove),VS("remove","(I)Ljava/lang/Object;",vector_remove),
 VS("removeAllElements","()V",vector_clear),VP("clear","()V",vector_clear),
 VS("setSize","(I)V",vector_resize),VS("ensureCapacity","(I)V",vector_resize),VS("trimToSize","()V",vector_resize),
 VP("contains","(Ljava/lang/Object;)Z",vector_contains),VP("indexOf","(Ljava/lang/Object;)I",vector_index),
 VS("indexOf","(Ljava/lang/Object;I)I",vector_index),VS("lastIndexOf","(Ljava/lang/Object;)I",vector_index),VS("lastIndexOf","(Ljava/lang/Object;I)I",vector_index),
 VS("copyInto","([Ljava/lang/Object;)V",vector_copy_into),VS("toArray","()[Ljava/lang/Object;",vector_to_array),VS("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",vector_to_array),
 VS("clone","()Ljava/lang/Object;",vector_clone),VS("equals","(Ljava/lang/Object;)Z",vector_value),VS("hashCode","()I",vector_value),VS("addAll","(Ljava/util/Collection;)Z",vector_add_all),VS("addAll","(ILjava/util/Collection;)Z",vector_add_all),
 VS("containsAll","(Ljava/util/Collection;)Z",vector_contains_all),VS("removeAll","(Ljava/util/Collection;)Z",vector_bulk),VS("retainAll","(Ljava/util/Collection;)Z",vector_bulk),
 VS("toString","()Ljava/lang/String;",list_toString),VS("subList","(II)Ljava/util/List;",list_sublist),
 VP("elements","()Ljava/util/Enumeration;",vector_iterator),VS("iterator","()Ljava/util/Iterator;",vector_iterator),VS("listIterator","()Ljava/util/ListIterator;",vector_iterator),VS("listIterator","(I)Ljava/util/ListIterator;",vector_iterator),M_END
};
static const struct rt_method rt_vector_enumeration[]={VP("hasMoreElements","()Z",vector_iterator_step),VP("nextElement","()Ljava/lang/Object;",vector_iterator_step),M_END};
static const struct rt_method rt_vector_iterator[]={
 VP("hasNext","()Z",vector_iterator_step),VP("next","()Ljava/lang/Object;",vector_iterator_step),VP("remove","()V",vector_iterator_step),M_END
};
static const struct rt_method rt_vector_list_iterator[]={
 VP("hasPrevious","()Z",vector_iterator_step),VP("previous","()Ljava/lang/Object;",vector_iterator_step),VP("nextIndex","()I",vector_iterator_step),VP("previousIndex","()I",vector_iterator_step),VP("set","(Ljava/lang/Object;)V",vector_iterator_step),VP("add","(Ljava/lang/Object;)V",vector_iterator_step),M_END
};
#undef VS
#undef VP

/* MPL-2.0. Arrays.asList is a fixed-size, live view of its reference array. */
static bool am_equals(struct dvm *,dvm_ref,dvm_ref,bool *);
static const struct rt_field rt_array_list_fields[]={
   {"a","[Ljava/lang/Object;"},{"offset","I"},{"length","I"},F_END
};
static dvm_ref array_list_array(struct dvm *vm,dvm_ref self)
{union dvm_value a={0};(void)dvm_get_field(vm,self,"a","[Ljava/lang/Object;",&a);return a.l;}
static int array_list_int(struct dvm *vm,dvm_ref self,const char *name)
{union dvm_value v={0};(void)dvm_get_field(vm,self,name,"I",&v);return v.i;}
static dvm_ref array_list_create(struct dvm *vm,dvm_ref array,int offset,int length,bool sub)
{
   dvm_ref list=dvm_new_object(vm,dvm_find_class(vm,sub ? "lunaria/util/ArraySubList" : "java/util/Arrays$ArrayList"));
   if(!list)return 0;
   (void)dvm_set_field(vm,list,"a","[Ljava/lang/Object;",(union dvm_value){.l=array});
   (void)dvm_set_field(vm,list,"offset","I",(union dvm_value){.i=offset});
   (void)dvm_set_field(vm,list,"length","I",(union dvm_value){.i=length});return list;
}
static bool array_list_factory(struct dvm *vm,dvm_ref array,union dvm_value *out)
{
   if(!array){dvm__throw(vm,"java/lang/NullPointerException","array");return false;}
   struct dvm_object *a=dvm__obj(vm,array);
   if(!a || a->kind!=DVM_OBJ_ARRAY || a->elem_kind!='L') {
      dvm__throw(vm,"java/lang/ClassCastException","asList requires a reference array");return false;
   }
   RETL(array_list_create(vm,array,0,(int)a->length,false));
}
static bool array_list_size(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;RETI(array_list_int(vm,self,"length"));}
static bool array_list_element(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   int size=array_list_int(vm,self,"length"),i=ARG(0).i;
   if(i<0 || i>=size){
      struct dvm_class *own=dvm_object_class(vm,self);
      dvm__throw(vm,own && !strcmp(own->name,"java/util/Arrays$ArrayList") ?
         "java/lang/ArrayIndexOutOfBoundsException" : "java/lang/IndexOutOfBoundsException","index %d length %d",i,size);return false;
   }
   dvm_ref array=array_list_array(vm,self);int offset=array_list_int(vm,self,"offset");
   dvm_ref *data=dvm_array_data(vm,array);dvm_ref old=data[offset+i];
   if(nargs>1) {
      struct dvm_class *actual=dvm_object_class(vm,ARG(1).l),*ac=dvm_object_class(vm,array);
      struct dvm_class *component=ac ? dvm__class_by_desc(vm,ac->desc+1) : NULL;
      if(ARG(1).l && (!actual || !component || !dvm__class_assignable(vm,actual,component))) {
         dvm__throw(vm,"java/lang/ArrayStoreException","incompatible array element");return false;
      }
      data=dvm_array_data(vm,array);data[offset+i]=ARG(1).l;
   }
   RETL(old);
}
static bool array_list_find(struct dvm *vm,dvm_ref self,dvm_ref needle,bool reverse,int *found)
{
   int n=array_list_int(vm,self,"length"),offset=array_list_int(vm,self,"offset");
   dvm_ref array=array_list_array(vm,self);*found=-1;
   for(int step=0;step<n;++step) {
      int i=reverse ? n-step-1 : step;dvm_ref *data=dvm_array_data(vm,array);bool equal=false;
      if(!am_equals(vm,needle,data[offset+i],&equal))return false;
      if(equal){*found=i;break;}
   }
   return true;
}
static bool array_list_search(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;int index=-1;const char *name=builtin_name();
   if(!array_list_find(vm,self,ARG(0).l,!strcmp(name,"lastIndexOf"),&index))return false;
   RETI(!strcmp(name,"contains") ? index>=0 : index);
}
static bool array_list_unsupported(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;(void)out;
   dvm__throw(vm,"java/lang/UnsupportedOperationException","fixed-size array list");return false;
}
static bool array_list_clear(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{if(array_list_int(vm,self,"length"))return array_list_unsupported(vm,self,args,nargs,out);RETV();}
static bool array_list_remove_object(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   int found=-1;if(!array_list_find(vm,self,ARG(0).l,false,&found))return false;
   if(found>=0)return array_list_unsupported(vm,self,args,nargs,out);RETI(0);
}
static bool array_list_add_all(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(nargs>1 && (ARG(0).i<0 || ARG(0).i>array_list_int(vm,self,"length"))) {
      dvm__throw(vm,"java/lang/IndexOutOfBoundsException","index %d",ARG(0).i);return false;
   }
   union dvm_value it={0},has={0};
   if(!am_vcall(vm,ARG(nargs-1).l,"iterator","()Ljava/util/Iterator;",NULL,0,&it) ||
      !am_vcall(vm,it.l,"hasNext","()Z",NULL,0,&has))return false;
   if(has.i)return array_list_unsupported(vm,self,args,nargs,out);RETI(0);
}
static bool array_list_remove_all(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","collection");return false;}
   bool retain=!strcmp(builtin_name(),"retainAll");int n=array_list_int(vm,self,"length"),offset=array_list_int(vm,self,"offset");
   dvm_ref array=array_list_array(vm,self);
   for(int i=0;i<n;++i) {
      dvm_ref *data=dvm_array_data(vm,array);union dvm_value item={.l=data[offset+i]},has={0};
      if(!am_vcall(vm,ARG(0).l,"contains","(Ljava/lang/Object;)Z",&item,1,&has))return false;
      if(!!has.i!=retain)return array_list_unsupported(vm,self,args,nargs,out);
   }
   RETI(0);
}
static bool array_list_iterator(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;dvm_ref it=dvm_new_object(vm,dvm_find_class(vm,"java/util/Arrays$ArrayItr"));
   if(!it)return false;
   (void)dvm_set_field(vm,it,"target","Ljava/lang/Object;",(union dvm_value){.l=self});
   (void)dvm_set_field(vm,it,"last","I",(union dvm_value){.i=-1});RETL(it);
}
static bool array_list_to_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(nargs)return abstract_collection_to_array(vm,self,args,nargs,out);
   dvm_ref array=array_list_array(vm,self);int n=array_list_int(vm,self,"length"),offset=array_list_int(vm,self,"offset");
   struct dvm_class *c=dvm_object_class(vm,array),*own=dvm_object_class(vm,self);
   bool root=own && !strcmp(own->name,"java/util/Arrays$ArrayList");
   dvm_ref copy=dvm_new_array(vm,'L',root && c ? c->desc+1 : "Ljava/lang/Object;",(uint32_t)n);
   if(!copy)return false;
   if(n)memcpy(dvm_array_data(vm,copy),(dvm_ref*)dvm_array_data(vm,array)+offset,(size_t)n*sizeof(dvm_ref));RETL(copy);
}
static bool array_list_sublist(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;int from=ARG(0).i,to=ARG(1).i,n=array_list_int(vm,self,"length");
   if(from<0 || to>n){dvm__throw(vm,"java/lang/IndexOutOfBoundsException","subList range");return false;}
   if(from>to){dvm__throw(vm,"java/lang/IllegalArgumentException","fromIndex > toIndex");return false;}
   RETL(array_list_create(vm,array_list_array(vm,self),array_list_int(vm,self,"offset")+from,to-from,true));
}
static bool array_list_value(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;bool equals=!strcmp(builtin_name(),"equals");int n=array_list_int(vm,self,"length"),offset=array_list_int(vm,self,"offset");
   if(equals) {
      if(self==ARG(0).l)RETI(1);
      struct dvm_class *other=dvm_object_class(vm,ARG(0).l);
      if(!other || !dvm__class_assignable(vm,other,dvm_find_class(vm,"java/util/List")))RETI(0);
      union dvm_value length={0};if(!am_vcall(vm,ARG(0).l,"size","()I",NULL,0,&length))return false;
      if(length.i!=n)RETI(0);
   }
   uint32_t hash=1;dvm_ref array=array_list_array(vm,self);
   for(int i=0;i<n;++i) {
      dvm_ref *data=dvm_array_data(vm,array),item=data[offset+i];union dvm_value result={0};
      if(equals) {
         union dvm_value index={.i=i};bool equal=false;
         if(!am_vcall(vm,ARG(0).l,"get","(I)Ljava/lang/Object;",&index,1,&result) || !am_equals(vm,item,result.l,&equal))return false;
         if(!equal)RETI(0);
      } else {
         if(item && !am_vcall(vm,item,"hashCode","()I",NULL,0,&result))return false;
         hash=31u*hash+(uint32_t)result.i;
      }
   }
   RETI(equals ? 1 : (int32_t)hash);
}
static bool array_list_function(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;const char *name=builtin_name();bool replace=!strcmp(name,"replaceAll"),remove=!strcmp(name,"removeIf");
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","action");return false;}
   int n=array_list_int(vm,self,"length");
   for(int i=0;i<n;++i) {
      union dvm_value index={.i=i},item={0},result={0};
      if(!array_list_element(vm,self,&index,1,&item))return false;
      if(!am_vcall(vm,ARG(0).l,replace ? "apply" : remove ? "test" : "accept",
         replace ? "(Ljava/lang/Object;)Ljava/lang/Object;" : remove ? "(Ljava/lang/Object;)Z" : "(Ljava/lang/Object;)V",&item,1,&result))return false;
      if(remove && result.i)return array_list_unsupported(vm,self,args,nargs,out);
      if(replace) {
         union dvm_value set[2]={index,result};if(!array_list_element(vm,self,set,2,&item))return false;
      }
   }
   if(remove)RETI(0);RETV();
}
static bool array_list_sort(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value array={0},ignored={0};
   struct dvm_class *own=dvm_object_class(vm,self);
   bool root=own && !strcmp(own->name,"java/util/Arrays$ArrayList");
   if(root)array.l=array_list_array(vm,self);
   else if(!array_list_to_array(vm,self,NULL,0,&array))return false;
   struct dvm_method *sort=dvm_lookup(vm,"java/util/Arrays","sort","([Ljava/lang/Object;Ljava/util/Comparator;)V");
   union dvm_value call[2]={array,ARG(0)};
   if(!sort || !dvm_call(vm,sort,0,call,2,&ignored))return false;
   if(!root) {
      int n=array_list_int(vm,self,"length");
      for(int i=0;i<n;++i){dvm_ref *data=dvm_array_data(vm,array.l);union dvm_value set[2]={{.i=i},{.l=data[i]}};
         if(!array_list_element(vm,self,set,2,&ignored))return false;}
   }
   RETV();
}
static const char *const rt_array_list_ifaces[]={"Ljava/util/RandomAccess;","Ljava/io/Serializable;","Ljava/util/List;",NULL};
static const char *const rt_array_sublist_ifaces[]={"Ljava/util/RandomAccess;","Ljava/util/List;",NULL};
static const char *const rt_array_iterator_ifaces[]={"Ljava/util/Iterator;",NULL};
static const struct rt_method rt_array_list[]={
   M("size","()I",array_list_size),M("isEmpty","()Z",abstract_collection_is_empty),
   M("get","(I)Ljava/lang/Object;",array_list_element),M("set","(ILjava/lang/Object;)Ljava/lang/Object;",array_list_element),
   M("contains","(Ljava/lang/Object;)Z",array_list_search),M("indexOf","(Ljava/lang/Object;)I",array_list_search),M("lastIndexOf","(Ljava/lang/Object;)I",array_list_search),
   M("iterator","()Ljava/util/Iterator;",array_list_iterator),M("listIterator","()Ljava/util/ListIterator;",list_listIterator),M("listIterator","(I)Ljava/util/ListIterator;",list_listIterator),
   M("toArray","()[Ljava/lang/Object;",array_list_to_array),M("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",array_list_to_array),
   M("add","(Ljava/lang/Object;)Z",array_list_unsupported),M("add","(ILjava/lang/Object;)V",array_list_unsupported),
   M("remove","(I)Ljava/lang/Object;",array_list_unsupported),M("remove","(Ljava/lang/Object;)Z",array_list_remove_object),M("clear","()V",array_list_clear),
   M("addAll","(Ljava/util/Collection;)Z",array_list_add_all),M("addAll","(ILjava/util/Collection;)Z",array_list_add_all),
   M("removeAll","(Ljava/util/Collection;)Z",array_list_remove_all),M("retainAll","(Ljava/util/Collection;)Z",array_list_remove_all),
   M("forEach","(Ljava/util/function/Consumer;)V",array_list_function),
   M("replaceAll","(Ljava/util/function/UnaryOperator;)V",array_list_function),
   M("removeIf","(Ljava/util/function/Predicate;)Z",array_list_function),
   M("sort","(Ljava/util/Comparator;)V",array_list_sort),
   M("subList","(II)Ljava/util/List;",array_list_sublist),M("equals","(Ljava/lang/Object;)Z",array_list_value),M("hashCode","()I",array_list_value),M("toString","()Ljava/lang/String;",list_toString),M_END
};

enum { PM_GET_META_DATA = 0x00000080 };
static dvm_ref resolve_info_new(struct dvm *vm);
static dvm_ref pm_component_info(struct dvm *vm, const char *desc);
static dvm_ref pm_application_info(struct dvm *,const struct lunaria_android_package *);
/* MPL-2.0. Manifest-backed activity resolution for the installed application. */
static bool activity_csv_has(const char *csv, const char *word)
{
   if (!word) return false;
   size_t n = strlen(word);
   for (const char *p = csv; p && *p;) {
      const char *end = strchr(p, ',');
      size_t len = end ? (size_t)(end-p) : strlen(p);
      if (len == n && !memcmp(p, word, n)) return true;
      p = end ? end+1 : NULL;
   }
   return false;
}

static bool activity_filter_matches(struct dvm *vm, dvm_ref intent,
                                    const char *component, int flags, int *priority)
{
   union dvm_value action={0}, categories={0}, data={0}, type={0};
   dvm_get_field(vm,intent,"action","Ljava/lang/String;",&action);
   dvm_get_field(vm,intent,"categories","Ljava/util/Set;",&categories);
   dvm_get_field(vm,intent,"data","Landroid/net/Uri;",&data);
   dvm_get_field(vm,intent,"type","Ljava/lang/String;",&type);
   /* URI/MIME matching needs the full data specification, not a match-all. */
   if (data.l || type.l) return false;
   const char *text=getenv("ANDROID_ACTIVITY_FILTERS");
   char *copy=text ? strdup(text) : NULL;
   bool matched=false;
   for (char *p=copy, *row; p && (row=luna_strsep(&p,"\n"));) {
      char *name=luna_strsep(&row,"|"), *actions=luna_strsep(&row,"|"), *cats=luna_strsep(&row,"|");
      char *data = luna_strsep(&row,"|");
      int candidate_priority = row ? atoi(row) : 0;
      if (!data || !actions || !cats || strcmp(name,component) || atoi(data)) continue;
      if (action.l && !activity_csv_has(actions,dvm_string_utf8(vm,action.l))) continue;
      if ((flags & 0x10000) && !activity_csv_has(cats,"android.intent.category.DEFAULT")) continue;
      struct rt_list *requested=categories.l ? list_of(vm,categories.l) : NULL;
      bool good=true;
      for (size_t i=0; requested && i<requested->size; ++i)
         if (!activity_csv_has(cats,dvm_string_utf8(vm,requested->items[i]))) { good=false; break; }
      if (good) { if (!matched || candidate_priority > *priority) *priority=candidate_priority; matched=true; }
   }
   free(copy);
   return matched;
}

static bool pm_queryIntentActivities(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)self;
   dvm_ref list=coll_new_list(vm,"Ljava/util/ArrayList;");
   if (!list || nargs<1 || !ARG(0).l) RETL(list);
   const char *package=getenv("ANDROID_PACKAGE_NAME");
   if (!package || !*package) RETL(list);
   union dvm_value pkg={0}, component={0}, cp={0}, cc={0};
   dvm_get_field(vm,ARG(0).l,"package","Ljava/lang/String;",&pkg);
   if (pkg.l && strcmp(dvm_string_utf8(vm,pkg.l),package)) RETL(list);
   dvm_get_field(vm,ARG(0).l,"component","Landroid/content/ComponentName;",&component);
   if (component.l) {
      dvm_get_field(vm,component.l,"pkg","Ljava/lang/String;",&cp);
      dvm_get_field(vm,component.l,"cls","Ljava/lang/String;",&cc);
      if (!cp.l || !cc.l || strcmp(dvm_string_utf8(vm,cp.l),package)) RETL(list);
   }
   const char *text=getenv("ANDROID_ACTIVITIES");
   char *copy=text ? strdup(text) : NULL;
   int flags=nargs>1 ? ARG(1).i : 0;
   for (char *p=copy,*row; p && (row=luna_strsep(&p,"\n"));) {
      char *name=luna_strsep(&row,"|"), *target=luna_strsep(&row,"|");
      if (!row || !target || !*name) continue;
      bool enabled=atoi(row)!=0;
      if (!enabled && !(flags & 0x200)) continue;
      int priority=0;
      if (component.l ? strcmp(name,dvm_string_utf8(vm,cc.l))!=0
                      : !activity_filter_matches(vm,ARG(0).l,name,flags,&priority)) continue;
      dvm_ref resolve=resolve_info_new(vm);
      dvm_ref info=pm_component_info(vm,"Landroid/content/pm/ActivityInfo;");
      if (!resolve || !info) {free(copy); return false;}
      dvm_set_field(vm,info,"packageName","Ljava/lang/String;",(union dvm_value){.l=dvm_new_string(vm,package)});
      dvm_set_field(vm,info,"name","Ljava/lang/String;",(union dvm_value){.l=dvm_new_string(vm,name)});
      dvm_set_field(vm,info,"targetActivity","Ljava/lang/String;",(union dvm_value){.l=*target ? dvm_new_string(vm,target) : 0});
      dvm_set_field(vm,info,"enabled","Z",(union dvm_value){.i=enabled});
      struct lunaria_android_package installed;
      if (lunaria_android_package_find(package,&installed))
         dvm_set_field(vm,info,"applicationInfo","Landroid/content/pm/ApplicationInfo;",
                       (union dvm_value){.l=pm_application_info(vm,&installed)});
      if (flags & PM_GET_META_DATA) {
         union dvm_value bundle={0};
         dvm_get_field(vm,info,"metaData","Landroid/os/Bundle;",&bundle);
         manifest_bundle_fill(vm,bundle.l,arm_exec_apk_component_meta_keys(name));
      }
      dvm_set_field(vm,resolve,"priority","I",(union dvm_value){.i=priority});
      dvm_set_field(vm,resolve,"activityInfo","Landroid/content/pm/ActivityInfo;",(union dvm_value){.l=info});
      union dvm_value item={.l=resolve}, ignored={0};
      if (!list_add(vm,list,&item,1,&ignored)) {free(copy);return false;}
      struct rt_list *results=list_of(vm,list);
      for (size_t j=results->size-1;j>0;--j) {
         union dvm_value previous={0};
         dvm_get_field(vm,results->items[j-1],"priority","I",&previous);
         if (previous.i>=priority) break;
         results->items[j]=results->items[j-1];
         results->items[j-1]=resolve;
      }
   }
   free(copy);
   RETL(list);
}

static bool pm_launch_intent(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   if (!nargs || !ARG(0).l) RETL(0);
   dvm_ref intent=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/content/Intent;"));
   if (!intent) return false;
   union dvm_value action={.l=dvm_new_string(vm,"android.intent.action.MAIN")}, ignored={0};
   if (!intent_init(vm,intent,&action,1,&ignored)) return false;
   dvm_set_field(vm,intent,"package","Ljava/lang/String;",ARG(0));
   const char *cats[]={"android.intent.category.INFO","android.intent.category.LAUNCHER"};
   for (size_t i=0;i<2;++i) {
      dvm_set_field(vm,intent,"categories","Ljava/util/Set;",(union dvm_value){0});
      union dvm_value category={.l=dvm_new_string(vm,cats[i])};
      if (!intent_addCategory(vm,intent,&category,1,&ignored)) return false;
      union dvm_value query[2]={{.l=intent},{.i=0}}, result={0};
      if (!pm_queryIntentActivities(vm,self,query,2,&result)) return false;
      struct rt_list *l=result.l ? list_of(vm,result.l) : NULL;
      if (!l || !l->size) continue;
      union dvm_value info={0}, package={0}, name={0};
      dvm_get_field(vm,l->items[0],"activityInfo","Landroid/content/pm/ActivityInfo;",&info);
      dvm_get_field(vm,info.l,"packageName","Ljava/lang/String;",&package);
      dvm_get_field(vm,info.l,"name","Ljava/lang/String;",&name);
      dvm_ref component=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/content/ComponentName;"));
      union dvm_value pair[2]={package,name};
      if (!component || !cn_init(vm,component,pair,2,&ignored)) return false;
      dvm_set_field(vm,intent,"component","Landroid/content/ComponentName;",(union dvm_value){.l=component});
      dvm_set_field(vm,intent,"flags","I",(union dvm_value){.i=0x10000000});
      RETL(intent);
   }
   RETL(0);
}

static dvm_ref pm_activity_info_for_component(struct dvm *vm,
                                              const union dvm_value *args, int nargs)
{
   if (!nargs || !ARG(0).l) {
      dvm__throw(vm,"java/lang/NullPointerException","component"); return 0;
   }
   dvm_ref intent=dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/content/Intent;"));
   union dvm_value ignored={0};
   if (!intent || !intent_init(vm,intent,NULL,0,&ignored)) return 0;
   dvm_set_field(vm,intent,"component","Landroid/content/ComponentName;",ARG(0));
   union dvm_value query[2]={{.l=intent},{.i=nargs>1 ? ARG(1).i : 0}}, result={0};
   if (!pm_queryIntentActivities(vm,0,query,2,&result)) return 0;
   struct rt_list *l=result.l ? list_of(vm,result.l) : NULL;
   if (l && l->size) {
      union dvm_value info={0};
      dvm_get_field(vm,l->items[0],"activityInfo","Landroid/content/pm/ActivityInfo;",&info);
      return info.l;
   }
   dvm__throw(vm,"android/content/pm/PackageManager$NameNotFoundException","activity component");
   return 0;
}
