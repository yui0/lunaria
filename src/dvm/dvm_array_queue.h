/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0
 * ArrayBlockingQueue: fixed ring, reentrant exclusion and two FIFO conditions.
 * The interpreter lock protects native bookkeeping. Every wait releases it;
 * no guest stack is unwound and no polling deadline is invented.
 */
struct abq_waiter { struct abq_waiter *next; bool signalled; };
struct abq_waiters { struct abq_waiter *first, *last; };
struct abq_state {
  uint32_t capacity, count, head, depth;
  dvm_ref owner;
  uint64_t serial;
  bool fair;
  dvm_ref items;
  struct abq_waiters locks, readers, writers;
  uint64_t ids[];
};
static void abq_append(struct abq_waiters *list, struct abq_waiter *w)
{
  w->next=NULL;
  if(list->last) list->last->next=w;
  else list->first=w;
  list->last=w;
}
static void abq_unlink(struct abq_waiters *list, struct abq_waiter *w)
{
  struct abq_waiter *prev=NULL;
  for(struct abq_waiter *p=list->first; p; prev=p,p=p->next) if(p==w) {
    if(prev) prev->next=p->next;
    else list->first=p->next;
    if(list->last==p) list->last=prev;
    p->next=NULL;
    return;
  }
}
static bool abq_interrupt(struct dvm *vm)
{
  if(!dvm__thread_take_interrupt(vm)) return false;
  dvm__throw(vm,"java/lang/InterruptedException","queue wait interrupted");
  return true;
}
static void abq_park(struct dvm *vm,dvm_ref self,uint64_t ns,bool timed)
{
  dvm_ref thread=dvm__current_thread(vm);
  dvm__thread_set_state(vm,thread,timed ? DVM_THREAD_TIMED_WAITING : DVM_THREAD_WAITING);
  dvm__thread_set_wait(vm,self);
  dvm_gil_wait_for_ns(vm,self,ns);
  dvm__thread_set_wait(vm,0);
  dvm__thread_set_state(vm,thread,DVM_THREAD_RUNNABLE);
}
static bool abq_lock(struct dvm *vm,dvm_ref self,struct abq_state *q,bool interruptible,
                     struct abq_waiter *transferred)
{
  struct abq_waiter local={0},*w=transferred ? transferred : &local;
  bool queued=transferred!=NULL;
  dvm_ref me=dvm__current_thread(vm);
  for(;;) {
    if(dvm_is_stopping(vm) || (interruptible && abq_interrupt(vm))) {
      if(queued) abq_unlink(&q->locks,w);
      dvm_gil_notify_for(self);
      return false;
    }
    if(q->owner==me || (!q->owner &&
       (!q->fair || !q->locks.first || q->locks.first==w))) {
      if(queued) abq_unlink(&q->locks,w);
      q->owner=me;
      ++q->depth;
      return true;
    }
    if(!queued) { abq_append(&q->locks,w); queued=true; }
    abq_park(vm,self,UINT64_MAX,false);
  }
}
static void abq_unlock(dvm_ref self,struct abq_state *q)
{
  if(--q->depth==0) { q->owner=0; dvm_gil_notify_for(self); }
}
static void abq_signal(dvm_ref self,struct abq_state *q,struct abq_waiters *condition)
{
  struct abq_waiter *w=condition->first;
  if(!w) return;
  abq_unlink(condition,w);
  w->signalled=true;
  abq_append(&q->locks,w);
  dvm_gil_notify_for(self);
}
static bool abq_await(struct dvm *vm,dvm_ref self,struct abq_state *q,
                      struct abq_waiters *condition,uint64_t ns,bool timed)
{
  struct abq_waiter w={0};
  if(abq_interrupt(vm)) return false;
  uint64_t began=luna_os_monotonic_ns();
  uint32_t depth=q->depth;
  abq_append(condition,&w);
  q->depth=1;
  abq_unlock(self,q);
  bool interrupted=false;
  while(!w.signalled && !dvm_is_stopping(vm)) {
    if(dvm__thread_take_interrupt(vm)) { interrupted=true; break; }
    uint64_t elapsed=luna_os_monotonic_ns()-began;
    if(timed && elapsed>=ns) break;
    abq_park(vm,self,timed ? ns-elapsed : UINT64_MAX,timed);
  }
  if(!w.signalled) {
    abq_unlink(condition,&w);
    abq_append(&q->locks,&w);
  }
  if(!abq_lock(vm,self,q,false,&w)) return false;
  q->depth=depth;
  if(interrupted) {
    dvm__throw(vm,"java/lang/InterruptedException","queue wait interrupted");
    return false;
  }
  return true;
}
static struct abq_state *abq_of(struct dvm *vm,dvm_ref self)
{
  struct dvm_object *o=dvm__obj(vm,self);
  return o ? o->data : NULL;
}
static dvm_ref *abq_items(struct dvm *vm,struct abq_state *q)
{ return dvm_array_data(vm,q->items); }
static uint32_t abq_slot(struct abq_state *q,uint32_t offset)
{
  uint32_t slot=q->head+offset; /* both operands are < capacity <= INT32_MAX */
  return slot>=q->capacity ? slot-q->capacity : slot;
}
static void abq_enqueue(struct dvm *vm,dvm_ref self,struct abq_state *q,dvm_ref value)
{
  uint32_t i=abq_slot(q,q->count++);
  dvm_keep_managed(vm,value);
  abq_items(vm,q)[i]=value;
  q->ids[i]=++q->serial;
  abq_signal(self,q,&q->readers);
}
static dvm_ref abq_remove_at(struct dvm *vm,dvm_ref self,struct abq_state *q,uint32_t offset)
{
  dvm_ref *items=abq_items(vm,q),value=items[abq_slot(q,offset)];
  if(!offset) {
    items[q->head]=0;
    q->ids[q->head]=0;
    q->head=abq_slot(q,1);
  } else {
    for(uint32_t j=offset; j+1<q->count; ++j) {
      uint32_t a=abq_slot(q,j),b=abq_slot(q,j+1);
      items[a]=items[b]; q->ids[a]=q->ids[b];
    }
    uint32_t last=abq_slot(q,q->count-1);
    items[last]=0; q->ids[last]=0;
  }
  --q->count;
  abq_signal(self,q,&q->writers);
  return value;
}
static bool abq_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  int32_t capacity=ARG(0).i;
  if(capacity<=0) { dvm__throw(vm,"java/lang/IllegalArgumentException","capacity <= 0"); return false; }
  if((size_t)capacity>(SIZE_MAX-sizeof(struct abq_state))/sizeof(uint64_t)) {
    dvm__throw(vm,"java/lang/OutOfMemoryError","queue capacity"); return false;
  }
  struct abq_state *q=calloc(1,sizeof *q+(size_t)capacity*sizeof *q->ids);
  if(!q) { dvm__throw(vm,"java/lang/OutOfMemoryError","queue capacity"); return false; }
  q->capacity=(uint32_t)capacity;
  q->fair=nargs>1 && ARG(1).i;
  q->items=dvm_new_array(vm,'L',"Ljava/lang/Object;",q->capacity);
  if(!q->items) { free(q); dvm__throw(vm,"java/lang/OutOfMemoryError","queue items"); return false; }
  struct dvm_object *o=dvm__obj(vm,self);
  if(o->data_destroy) o->data_destroy(o->data); else free(o->data);
  o->data=q; o->data_destroy=free;
  (void)dvm_set_field(vm,self,"items","[Ljava/lang/Object;",(union dvm_value){.l=q->items});
  if(nargs==3) {
    if(!ARG(2).l) { dvm__throw(vm,"java/lang/NullPointerException","collection"); return false; }
    union dvm_value it={0},has={0},item={0};
    if(!am_vcall(vm,ARG(2).l,"iterator","()Ljava/util/Iterator;",NULL,0,&it)) return false;
    dvm_pin(vm,it.l);
    bool good=true;
    while(good) {
      good=am_vcall(vm,it.l,"hasNext","()Z",NULL,0,&has);
      if(!good || !has.i) break;
      good=am_vcall(vm,it.l,"next","()Ljava/lang/Object;",NULL,0,&item);
      if(!good) break;
      if(!item.l) { dvm__throw(vm,"java/lang/NullPointerException","collection element"); good=false; }
      else if(q->count==q->capacity) { dvm__throw(vm,"java/lang/IllegalArgumentException","collection exceeds capacity"); good=false; }
      else abq_enqueue(vm,self,q,item.l);
    }
    dvm_unpin(vm,it.l);
    if(!good) return false;
  }
  RETV();
}
static bool abq_transfer(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  const char *name=builtin_name();
  bool insert=!strcmp(name,"add") || !strcmp(name,"offer") || !strcmp(name,"put");
  bool timed=insert ? nargs==3 : nargs==2;
  bool blocking=timed || !strcmp(name,"put") || !strcmp(name,"take");
  if(insert && !ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","element"); return false; }
  int64_t timeout=0;
  if(timed) {
    int offset=insert ? 1 : 0;
    if(!ARG(offset+1).l) { dvm__throw(vm,"java/lang/NullPointerException","unit"); return false; }
    timeout=tu_rescale(ARG(offset).j,tu_scale(vm,ARG(offset+1).l),1);
  }
  struct abq_state *q=abq_of(vm,self);
  if(!q || !abq_lock(vm,self,q,blocking,NULL)) return false;
  uint64_t began=luna_os_monotonic_ns();
  bool good=true;
  while(insert ? q->count==q->capacity : !q->count) {
    uint64_t elapsed=luna_os_monotonic_ns()-began;
    if(!blocking || (timed && (timeout<=0 || elapsed>=(uint64_t)timeout))) break;
    good=abq_await(vm,self,q,insert ? &q->writers : &q->readers,
                   timed ? (uint64_t)timeout-elapsed : UINT64_MAX,timed);
    if(!good) break;
  }
  if(good && (insert ? q->count<q->capacity : q->count>0)) {
    if(insert) { abq_enqueue(vm,self,q,ARG(0).l); out->i=1; }
    else out->l=abq_remove_at(vm,self,q,0);
  } else if(good) {
    out->u=0;
    if(!strcmp(name,"add") || !strcmp(name,"remove")) {
      dvm__throw(vm,insert ? "java/lang/IllegalStateException" : "java/util/NoSuchElementException","queue has no available element/space");
      good=false;
    }
  }
  if(q->owner==dvm__current_thread(vm)) abq_unlock(self,q);
  return good;
}
static bool abq_query(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  struct abq_state *q=abq_of(vm,self);
  if(!q || !abq_lock(vm,self,q,false,NULL)) return false;
  const char *name=builtin_name();
  bool good=true;
  if(!strcmp(name,"size")) out->i=(int32_t)q->count;
  else if(!strcmp(name,"isEmpty")) out->i=!q->count;
  else if(!strcmp(name,"remainingCapacity")) out->i=(int32_t)(q->capacity-q->count);
  else if(!strcmp(name,"clear")) {
    memset(abq_items(vm,q),0,(size_t)q->capacity*sizeof(dvm_ref));
    memset(q->ids,0,(size_t)q->capacity*sizeof(uint64_t));
    q->head=q->count=0;
    while(q->writers.first) abq_signal(self,q,&q->writers);
  } else {
    out->l=q->count ? abq_items(vm,q)[q->head] : 0;
    if(!out->l && !strcmp(name,"element")) { dvm__throw(vm,"java/util/NoSuchElementException","empty queue"); good=false; }
  }
  abq_unlock(self,q);
  return good;
}
static bool abq_find(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  if(!ARG(0).l) RETI(0);
  struct abq_state *q=abq_of(vm,self);
  if(!q || !abq_lock(vm,self,q,false,NULL)) return false;
  bool found=false,good=true;
  for(uint32_t i=0; i<q->count; ++i) {
    uint32_t slot=abq_slot(q,i);
    uint64_t id=q->ids[slot];
    bool equal=key_eq(vm,ARG(0).l,abq_items(vm,q)[slot]);
    if(dvm_exception(vm)) { good=false; break; }
    /* equals may call back into this reentrant queue. */
    if(equal && i<q->count && q->ids[abq_slot(q,i)]==id) {
      found=true;
      if(!strcmp(builtin_name(),"remove")) abq_remove_at(vm,self,q,i);
      break;
    }
  }
  abq_unlock(self,q);
  if(!good) return false;
  RETI(found);
}
static bool abq_drain(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(!ARG(0).l || ARG(0).l==self) {
    dvm__throw(vm,ARG(0).l ? "java/lang/IllegalArgumentException" : "java/lang/NullPointerException","destination"); return false;
  }
  int32_t max=nargs>1 ? ARG(1).i : INT32_MAX;
  if(max<=0) RETI(0);
  struct abq_state *q=abq_of(vm,self);
  if(!q || !abq_lock(vm,self,q,false,NULL)) return false;
  uint32_t limit=q->count<(uint32_t)max ? q->count : (uint32_t)max,moved=0;
  bool good=true;
  while(moved<limit && q->count) {
    union dvm_value value={.l=abq_items(vm,q)[q->head]},ignored={0};
    uint64_t id=q->ids[q->head];
    if(!am_vcall(vm,ARG(0).l,"add","(Ljava/lang/Object;)Z",&value,1,&ignored)) { good=false; break; }
    if(q->count && q->ids[q->head]==id) abq_remove_at(vm,self,q,0);
    ++moved;
  }
  abq_unlock(self,q);
  if(!good) return false;
  RETI((int32_t)moved);
}
/* Iterators cache one next element, as Android does: a successful hasNext
 * guarantees next even when another thread removes that element. Subsequent
 * steps skip removed insertion IDs and never fail on concurrent mutation. */
static void abq_iter_cache(struct dvm *vm,dvm_ref it,struct abq_state *q,uint64_t after)
{
  dvm_ref value=0;
  uint64_t id=0;
  for(uint32_t i=0; i<q->count; ++i) {
    uint32_t slot=abq_slot(q,i);
    if(q->ids[slot]>after) { value=abq_items(vm,q)[slot]; id=q->ids[slot]; break; }
  }
  (void)dvm_set_field(vm,it,"nextItem","Ljava/lang/Object;",(union dvm_value){.l=value});
  (void)dvm_set_field(vm,it,"nextId","J",(union dvm_value){.j=(int64_t)id});
}
static bool abq_iterator(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  struct abq_state *q=abq_of(vm,self);
  if(!q || !abq_lock(vm,self,q,false,NULL)) return false;
  dvm_ref it=dvm_new_object(vm,dvm_find_class(vm,"java/util/concurrent/ArrayBlockingQueue$Itr"));
  if(it) {
    (void)dvm_set_field(vm,it,"owner","Ljava/util/concurrent/ArrayBlockingQueue;",(union dvm_value){.l=self});
    abq_iter_cache(vm,it,q,0);
  }
  abq_unlock(self,q);
  if(!it) { dvm__throw(vm,"java/lang/OutOfMemoryError","queue iterator"); return false; }
  RETL(it);
}
static bool abq_iter_op(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value owner={0},next={0},id={0},last={0};
  (void)dvm_get_field(vm,self,"owner","Ljava/util/concurrent/ArrayBlockingQueue;",&owner);
  (void)dvm_get_field(vm,self,"nextItem","Ljava/lang/Object;",&next);
  (void)dvm_get_field(vm,self,"nextId","J",&id);
  (void)dvm_get_field(vm,self,"lastId","J",&last);
  const char *name=builtin_name();
  if(!strcmp(name,"hasNext")) RETI(next.l!=0);
  bool advance=!strcmp(name,"next");
  if(advance && !next.l) { dvm__throw(vm,"java/util/NoSuchElementException","iterator exhausted"); return false; }
  if(!advance && !last.j) { dvm__throw(vm,"java/lang/IllegalStateException","iterator remove without next"); return false; }
  struct abq_state *q=abq_of(vm,owner.l);
  if(!q || !abq_lock(vm,owner.l,q,false,NULL)) return false;
  if(advance) {
    (void)dvm_set_field(vm,self,"lastId","J",id);
    abq_iter_cache(vm,self,q,(uint64_t)id.j);
  } else {
    for(uint32_t i=0; i<q->count; ++i) if(q->ids[abq_slot(q,i)]==(uint64_t)last.j) { abq_remove_at(vm,owner.l,q,i); break; }
    (void)dvm_set_field(vm,self,"lastId","J",(union dvm_value){0});
  }
  abq_unlock(owner.l,q);
  if(advance) RETL(next.l);
  RETV();
}
static bool abq_to_array(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  if(nargs && !ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","array"); return false; }
  struct abq_state *q=abq_of(vm,self);
  if(!q || !abq_lock(vm,self,q,false,NULL)) return false;
  struct dvm_object *proto=nargs ? dvm__obj(vm,ARG(0).l) : NULL;
  const char *component=proto ? proto->cls->desc+1 : "Ljava/lang/Object;";
  dvm_ref result=proto && proto->length>=q->count ? ARG(0).l : dvm_new_array(vm,'L',component,q->count);
  bool good=result!=0;
  if(!good) dvm__throw(vm,"java/lang/OutOfMemoryError","queue array");
  struct dvm_class *element=dvm__class_by_desc(vm,component);
  dvm_ref *dest=dvm_array_data(vm,result);
  for(uint32_t i=0; good && i<q->count; ++i) {
    dvm_ref value=abq_items(vm,q)[abq_slot(q,i)];
    if(!dvm__class_assignable(vm,dvm_object_class(vm,value),element)) {
      dvm__throw(vm,"java/lang/ArrayStoreException","queue toArray"); good=false;
    } else dest[i]=value;
  }
  if(good && dvm_array_length(vm,result)>q->count) dest[q->count]=0;
  abq_unlock(self,q);
  if(!good) return false;
  RETL(result);
}
static bool abq_bulk_remove(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  if(!ARG(0).l) { dvm__throw(vm,"java/lang/NullPointerException","collection"); return false; }
  bool retain=!strcmp(builtin_name(),"retainAll"),changed=false,good=true;
  union dvm_value it={0},has={0},item={0},contains={0},ignored={0};
  if(!abq_iterator(vm,self,NULL,0,&it)) return false;
  dvm_pin(vm,it.l);
  while(good) {
    good=am_vcall(vm,it.l,"hasNext","()Z",NULL,0,&has);
    if(!good || !has.i) break;
    good=am_vcall(vm,it.l,"next","()Ljava/lang/Object;",NULL,0,&item) &&
         am_vcall(vm,ARG(0).l,"contains","(Ljava/lang/Object;)Z",&item,1,&contains);
    if(good && (retain ? !contains.i : contains.i)) {
      good=am_vcall(vm,it.l,"remove","()V",NULL,0,&ignored);
      changed |= good;
    }
  }
  dvm_unpin(vm,it.l);
  if(!good) return false;
  RETI(changed);
}
static bool abq_add_all(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
  (void)nargs;
  if(!ARG(0).l || ARG(0).l==self) { dvm__throw(vm,ARG(0).l ? "java/lang/IllegalArgumentException" : "java/lang/NullPointerException","collection"); return false; }
  union dvm_value it={0},has={0},item={0},ignored={0};
  if(!am_vcall(vm,ARG(0).l,"iterator","()Ljava/util/Iterator;",NULL,0,&it)) return false;
  dvm_pin(vm,it.l);
  bool good=true,changed=false;
  while(good) {
    good=am_vcall(vm,it.l,"hasNext","()Z",NULL,0,&has);
    if(!good || !has.i) break;
    good=am_vcall(vm,it.l,"next","()Ljava/lang/Object;",NULL,0,&item) &&
         am_vcall(vm,self,"add","(Ljava/lang/Object;)Z",&item,1,&ignored);
    if(good) changed=true;
  }
  dvm_unpin(vm,it.l);
  if(!good) return false;
  RETI(changed);
}
static const struct rt_field rt_abq_fields[]={ {"items","[Ljava/lang/Object;"},F_END };
static const struct rt_field rt_abq_iter_fields[]={
  {"owner","Ljava/util/concurrent/ArrayBlockingQueue;"},{"nextItem","Ljava/lang/Object;"},
  {"nextId","J"},{"lastId","J"},F_END
};
static const struct rt_method rt_abq_iter[]={
  M("hasNext","()Z",abq_iter_op),M("next","()Ljava/lang/Object;",abq_iter_op),M("remove","()V",abq_iter_op),M_END
};
static const struct rt_method rt_abq[]={
  M("<init>","(I)V",abq_init),M("<init>","(IZ)V",abq_init),M("<init>","(IZLjava/util/Collection;)V",abq_init),
  M("add","(Ljava/lang/Object;)Z",abq_transfer),M("offer","(Ljava/lang/Object;)Z",abq_transfer),
  M("offer","(Ljava/lang/Object;JLjava/util/concurrent/TimeUnit;)Z",abq_transfer),M("put","(Ljava/lang/Object;)V",abq_transfer),
  M("take","()Ljava/lang/Object;",abq_transfer),M("poll","()Ljava/lang/Object;",abq_transfer),
  M("poll","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",abq_transfer),M("remove","()Ljava/lang/Object;",abq_transfer),
  M("peek","()Ljava/lang/Object;",abq_query),M("element","()Ljava/lang/Object;",abq_query),
  M("size","()I",abq_query),M("isEmpty","()Z",abq_query),M("remainingCapacity","()I",abq_query),M("clear","()V",abq_query),
  M("contains","(Ljava/lang/Object;)Z",abq_find),M("remove","(Ljava/lang/Object;)Z",abq_find),
  M("addAll","(Ljava/util/Collection;)Z",abq_add_all),
  M("drainTo","(Ljava/util/Collection;)I",abq_drain),M("drainTo","(Ljava/util/Collection;I)I",abq_drain),
  M("iterator","()Ljava/util/Iterator;",abq_iterator),M("toString","()Ljava/lang/String;",list_toString),
  M("toArray","()[Ljava/lang/Object;",abq_to_array),M("toArray","([Ljava/lang/Object;)[Ljava/lang/Object;",abq_to_array),
  M("removeAll","(Ljava/util/Collection;)Z",abq_bulk_remove),M("retainAll","(Ljava/util/Collection;)Z",abq_bulk_remove),M_END
};
