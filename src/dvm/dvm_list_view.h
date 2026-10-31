/* MPL-2.0. Adapter-owned ListView rows and viewport measurement. */
static const struct rt_field rt_list_view_fields[]={
   {"listAdapter","Landroid/widget/ListAdapter;"},
   {"listObserver","Landroid/database/DataSetObserver;"},
   {"itemCount","I"},{"firstPosition","I"},{"dataGeneration","I"},F_END
};
static const struct rt_field rt_list_observer_fields[]={
   {"owner","Landroid/widget/ListView;"},F_END
};
static const struct rt_field rt_abs_list_params_fields[]={
   {"viewType","I"},{"itemId","J"},{"isEnabled","Z"},F_END
};
static bool list_view_request(struct dvm *vm,dvm_ref self)
{
   union dvm_value ignored={0};
   ui_invalidate();
   return am_vcall(vm,self,"requestLayout","()V",NULL,0,&ignored);
}
static bool list_view_observed(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;(void)out;
   dvm_ref owner=ui_obj(vm,self,"owner","Landroid/widget/ListView;");
   if(!owner)return true;
   dvm_ref adapter=ui_obj(vm,owner,"listAdapter","Landroid/widget/ListAdapter;");
   union dvm_value count={0};
   if(!strcmp(builtin_name(),"onChanged") && adapter &&
      !am_vcall(vm,adapter,"getCount","()I",NULL,0,&count))return false;
   if(adapter!=ui_obj(vm,owner,"listAdapter","Landroid/widget/ListAdapter;"))return true;
   rt_set_int(vm,owner,"itemCount",count.i);
   rt_set_int(vm,owner,"dataGeneration",(int32_t)((uint32_t)ui_int(vm,owner,"dataGeneration")+1));
   if(!count.i) {
      rt_set_int(vm,owner,"firstPosition",0);rt_set_int(vm,owner,"selectedPosition",-1);
      (void)dvm_set_field(vm,owner,"selectedRowId","J",(union dvm_value){.j=INT64_MIN});
   }
   return list_view_request(vm,owner);
}
static const struct rt_method rt_list_observer[]={
   M("onChanged","()V",list_view_observed),M("onInvalidated","()V",list_view_observed),M_END
};
static bool list_view_set_adapter(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                   int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref old=ui_obj(vm,self,"listAdapter","Landroid/widget/ListAdapter;");
   dvm_ref observer=ui_obj(vm,self,"listObserver","Landroid/database/DataSetObserver;");
   union dvm_value a={.l=observer},ignored={0};
   if(old && observer && !am_vcall(vm,old,"unregisterDataSetObserver",
      "(Landroid/database/DataSetObserver;)V",&a,1,&ignored))return false;
   if(observer)(void)dvm_set_field(vm,observer,"owner","Landroid/widget/ListView;",(union dvm_value){0});
   (void)dvm_set_field(vm,self,"listObserver","Landroid/database/DataSetObserver;",(union dvm_value){0});
   (void)dvm_set_field(vm,self,"listAdapter","Landroid/widget/ListAdapter;",ARG(0));
   rt_set_int(vm,self,"dataGeneration",(int32_t)((uint32_t)ui_int(vm,self,"dataGeneration")+1));
   rt_set_int(vm,self,"firstPosition",0);rt_set_int(vm,self,"itemCount",0);
   rt_set_int(vm,self,"selectedPosition",-1);
   (void)dvm_set_field(vm,self,"selectedRowId","J",(union dvm_value){.j=INT64_MIN});
   if(!vg_remove_all(vm,self,NULL,0,&ignored))return false;
   dvm_ref adapter=ARG(0).l;
   if(adapter) {
      union dvm_value count={0};
      if(!am_vcall(vm,adapter,"getCount","()I",NULL,0,&count))return false;
      if(adapter!=ui_obj(vm,self,"listAdapter","Landroid/widget/ListAdapter;"))return true;
      rt_set_int(vm,self,"itemCount",count.i);
      observer=dvm_new_object(vm,dvm_find_class(vm,"lunaria/widget/ListDataSetObserver"));
      if(!observer)return false;
      (void)dvm_set_field(vm,observer,"owner","Landroid/widget/ListView;",(union dvm_value){.l=self});
      (void)dvm_set_field(vm,self,"listObserver","Landroid/database/DataSetObserver;",(union dvm_value){.l=observer});
      a.l=observer;
      if(!am_vcall(vm,adapter,"registerDataSetObserver","(Landroid/database/DataSetObserver;)V",&a,1,&ignored))return false;
   }
   (void)out;return list_view_request(vm,self);
}
static bool list_view_get_adapter(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                   int nargs,union dvm_value *out)
{ (void)args;(void)nargs;RETL(ui_obj(vm,self,"listAdapter","Landroid/widget/ListAdapter;")); }
static bool list_view_count(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                             int nargs,union dvm_value *out)
{ (void)args;(void)nargs;RETI(ui_int(vm,self,"itemCount")); }
static bool list_view_first(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                             int nargs,union dvm_value *out)
{ (void)args;(void)nargs;RETI(ui_int(vm,self,"firstPosition")); }
static bool list_view_last(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                            int nargs,union dvm_value *out)
{ (void)args;(void)nargs;struct rt_list *l=list_of(vm,view_children(vm,self,false));
  RETI(ui_int(vm,self,"firstPosition")+(l ? (int32_t)l->size : 0)-1); }

struct list_recycle_row {dvm_ref view;int type;bool used;};
static void list_view_layout_rows(struct dvm *vm,dvm_ref self,struct ui_ms ws,
                              struct ui_ms hs,unsigned depth)
{
   dvm_ref adapter=ui_obj(vm,self,"listAdapter","Landroid/widget/ListAdapter;");
   int generation=ui_int(vm,self,"dataGeneration"),count=ui_int(vm,self,"itemCount");
   int first=ui_int(vm,self,"firstPosition");if(first>=count)first=count>0 ? count-1 : 0;
   rt_set_int(vm,self,"firstPosition",first);
   struct rt_list *children=list_of(vm,view_children(vm,self,false));
   uint32_t old_count=children ? children->size : 0;
   struct list_recycle_row *pool=old_count ? calloc(old_count,sizeof *pool) : NULL;
   if(old_count && !pool){dvm__throw(vm,"java/lang/OutOfMemoryError","ListView recycle rows");return;}
   for(uint32_t i=0;i<old_count;++i) {
      pool[i].view=children->items[i];dvm_pin(vm,pool[i].view);
      dvm_ref lp=ui_obj(vm,pool[i].view,"layoutParams","Landroid/view/ViewGroup$LayoutParams;");
      pool[i].type=ui_int(vm,lp,"viewType");
   }
   union dvm_value ignored={0};
   bool good=vg_detach_all_views_from_parent(vm,self,NULL,0,&ignored);
   int32_t pad[4];ui_pad(vm,self,pad);int64_t total=(int64_t)pad[1]+pad[3];int32_t widest=0;
   for(int position=first;good && adapter && position<count;++position) {
      if(position>first && (hs.mode==UI_MS_UNSPEC || total>=hs.size))break;
      union dvm_value index={.i=position},type={0},id={0},enabled={0},row={0};
      good=am_vcall(vm,adapter,"getItemViewType","(I)I",&index,1,&type);
      dvm_ref convert=0;
      for(uint32_t i=0;good && type.i>=0 && i<old_count;++i)
         if(!pool[i].used && pool[i].type==type.i){convert=pool[i].view;break;}
      union dvm_value bind[3]={index,{.l=convert},{.l=self}};
      if(good)good=am_vcall(vm,adapter,"getView","(ILandroid/view/View;Landroid/view/ViewGroup;)Landroid/view/View;",bind,3,&row);
      if(!good)break;
      if(generation!=ui_int(vm,self,"dataGeneration"))break;
      if(!row.l || !ui_is_a(vm,row.l,"android/view/View")) {
         dvm__throw(vm,"java/lang/IllegalStateException","Adapter.getView must return a View");good=false;break;
      }
      good=am_vcall(vm,adapter,"getItemId","(I)J",&index,1,&id) &&
           am_vcall(vm,adapter,"isEnabled","(I)Z",&index,1,&enabled);
      if(!good || generation!=ui_int(vm,self,"dataGeneration"))break;
      dvm_ref lp=ui_obj(vm,row.l,"layoutParams","Landroid/view/ViewGroup$LayoutParams;");
      if(!lp || !ui_is_a(vm,lp,"android/widget/AbsListView$LayoutParams")) {
         int w=lp ? ui_int(vm,lp,"width") : -1,h=lp ? ui_int(vm,lp,"height") : -2;
         lp=dvm_new_object(vm,dvm_find_class(vm,"android/widget/AbsListView$LayoutParams"));
         if(!lp){good=false;break;}
         rt_set_int(vm,lp,"width",w);rt_set_int(vm,lp,"height",h);
      }
      rt_set_int(vm,lp,"viewType",type.i);
      (void)dvm_set_field(vm,lp,"itemId","J",id);(void)dvm_set_field(vm,lp,"isEnabled","Z",enabled);
      (void)dvm_set_field(vm,row.l,"layoutParams","Landroid/view/ViewGroup$LayoutParams;",(union dvm_value){.l=lp});
      bool reused=false;
      for(uint32_t i=0;i<old_count;++i)if(pool[i].view==row.l && !pool[i].used){pool[i].used=true;reused=true;break;}
      if(ui_obj(vm,row.l,"parent","Landroid/view/ViewParent;")) {
         dvm__throw(vm,"java/lang/IllegalStateException","Adapter returned an attached View");good=false;break;
      }
      union dvm_value attach[4]={row,{.i=-1},{.l=lp},{.i=1}};
      good=am_vcall(vm,self,reused ? "attachViewToParent" : "addViewInLayout",
         reused ? "(Landroid/view/View;ILandroid/view/ViewGroup$LayoutParams;)V" :
                  "(Landroid/view/View;ILandroid/view/ViewGroup$LayoutParams;Z)Z",attach,reused ? 3 : 4,&ignored);
      if(!good)break;
      struct ui_lp layout;ui_read_lp(vm,row.l,&layout);
      ui_measure(vm,row.l,ui_child_spec(ws,pad[0]+pad[2],layout.w),
         layout.h>0 ? ui_ms_make(UI_MS_EXACT,layout.h) : ui_ms_make(UI_MS_UNSPEC,0),depth+1);
      if(dvm_exception(vm)){good=false;break;}
      struct ui_node *node=ui_node(vm,row.l);
      if(node){node->x=pad[0];node->y=(int32_t)(total-pad[3]);total+=node->h;if(node->w>widest)widest=node->w;}
      if(total>INT32_MAX){total=INT32_MAX;break;}
   }
   /* Detached rows not selected by the adapter leave the window. Keep their
    * roots alive across guest callbacks and release all pins on every path. */
   dvm_ref exception=dvm_exception(vm);if(exception)dvm_pin(vm,exception);
   for(uint32_t i=0;i<old_count;++i) {
      if(!pool[i].used && !dvm_exception(vm)) {
         union dvm_value remove[2]={{.l=pool[i].view},{0}};
         (void)vg_remove_detached_view(vm,self,remove,2,&ignored);
      }
      dvm_unpin(vm,pool[i].view);
   }
   if(exception)dvm_unpin(vm,exception);
   free(pool);
   struct ui_node *node=ui_node(vm,self);
   if(node){node->w=ui_resolve(widest+pad[0]+pad[2],ws);node->h=ui_resolve((int32_t)total,hs);}
}

/* Measuring a list may obtain unattached sample rows, but only layout owns
 * attachment. Exact parent constraints need no adapter row measurement. */
static void list_view_measure(struct dvm *vm,dvm_ref self,struct ui_ms ws,
                              struct ui_ms hs,unsigned depth)
{
   int32_t pad[4];ui_pad(vm,self,pad);
   dvm_ref adapter=ui_obj(vm,self,"listAdapter","Landroid/widget/ListAdapter;");
   int generation=ui_int(vm,self,"dataGeneration");union dvm_value count={0};
   if(adapter && !am_vcall(vm,adapter,"getCount","()I",NULL,0,&count))return;
   if(generation!=ui_int(vm,self,"dataGeneration"))return;
   rt_set_int(vm,self,"itemCount",count.i);
   int32_t width=ws.size,height=hs.size,sample_width=0;
   int64_t total=(int64_t)pad[1]+pad[3];
   if(adapter && (hs.mode!=UI_MS_EXACT || ws.mode==UI_MS_UNSPEC)) {
      for(int i=0;i<count.i;++i) {
         if(i && (hs.mode!=UI_MS_ATMOST || total>=hs.size))break;
         union dvm_value args[3]={ {.i=i},{0},{.l=self} },row={0};
         if(!am_vcall(vm,adapter,"getView","(ILandroid/view/View;Landroid/view/ViewGroup;)Landroid/view/View;",args,3,&row))return;
         if(generation!=ui_int(vm,self,"dataGeneration"))return;
         if(!row.l || !ui_is_a(vm,row.l,"android/view/View")) {
            dvm__throw(vm,"java/lang/IllegalStateException","Adapter.getView must return a View");return;
         }
         struct ui_lp lp;ui_read_lp(vm,row.l,&lp);
         ui_measure(vm,row.l,ui_child_spec(ws,pad[0]+pad[2],lp.w),
            lp.h>0 ? ui_ms_make(UI_MS_EXACT,lp.h) : ui_ms_make(UI_MS_UNSPEC,0),depth+1);
         if(dvm_exception(vm))return;
         struct ui_node *child=ui_node(vm,row.l);
         if(child){if(!i)sample_width=child->w;total+=child->h;}
         if(total>=INT32_MAX){total=INT32_MAX;break;}
      }
   }
   if(ws.mode==UI_MS_UNSPEC)width=sample_width+pad[0]+pad[2];
   if(hs.mode!=UI_MS_EXACT)height=ui_resolve((int32_t)total,hs);
   struct ui_node *node=ui_node(vm,self);if(node){node->w=width;node->h=height;}
}
static bool list_view_on_layout(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                 int nargs,union dvm_value *out)
{
   struct ui_node *node=ui_node(vm,self);
   if(node)list_view_layout_rows(vm,self,ui_ms_make(UI_MS_EXACT,node->w),
      ui_ms_make(UI_MS_EXACT,node->h),0);
   if(dvm_exception(vm))return false;
   return widget_on_layout(vm,self,args,nargs,out);
}

/* AdapterView's callback does not infer a position or ID from selection.
 * Programmatic callers supply all three values, including null row views. */
static bool adapter_view_item_listener(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(nargs){(void)dvm_set_field(vm,self,"itemClickListener","Landroid/widget/AdapterView$OnItemClickListener;",ARG(0));RETV();}
   RETL(ui_obj(vm,self,"itemClickListener","Landroid/widget/AdapterView$OnItemClickListener;"));
}
static bool adapter_view_item_click(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;dvm_ref listener=ui_obj(vm,self,"itemClickListener","Landroid/widget/AdapterView$OnItemClickListener;");
   if(!listener)RETI(0);
   union dvm_value call[4]={{.l=self},ARG(0),ARG(1),ARG(2)},ignored={0};
   if(!am_vcall(vm,listener,"onItemClick","(Landroid/widget/AdapterView;Landroid/view/View;IJ)V",call,4,&ignored))return false;
   RETI(1);
}
static bool adapter_view_position(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;dvm_ref row=ARG(0).l;
   if(!row){dvm__throw(vm,"java/lang/NullPointerException","view");return false;}
   for(;;) {
      dvm_ref parent=ui_obj(vm,row,"parent","Landroid/view/ViewParent;");
      if(!parent || parent==self)break;
      if(!ui_is_a(vm,parent,"android/view/View"))RETI(-1);
      row=parent;
   }
   struct rt_list *children=list_of(vm,view_children(vm,self,false));
   uint32_t n=children ? children->size : 0;
   for(uint32_t i=0;i<n;++i) {
      children=list_of(vm,view_children(vm,self,false));
      if(!children || i>=children->size)break;
      bool equal=false;if(!am_equals(vm,children->items[i],row,&equal))return false;
      if(equal)RETI(ui_int(vm,self,"firstPosition")+(int32_t)i);
   }
   RETI(-1);
}
static bool adapter_view_item_at(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   bool id=!strcmp(builtin_name(),"getItemIdAtPosition");union dvm_value adapter={0};
   if(!am_vcall(vm,self,"getAdapter","()Landroid/widget/Adapter;",NULL,0,&adapter))return false;
   if(!adapter.l || ARG(0).i<0){if(id)RETJ(INT64_MIN);RETL(0);}
   return am_vcall(vm,adapter.l,id ? "getItemId" : "getItem",id ? "(I)J" : "(I)Ljava/lang/Object;",args,nargs,out);
}
static bool adapter_view_reject_click_listener(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;(void)out;
   dvm__throw(vm,"java/lang/RuntimeException","Use setOnItemClickListener for an AdapterView");return false;
}
static bool list_view_tap(struct dvm *vm,dvm_ref list,dvm_ref clicked)
{
   union dvm_value view={.l=clicked},position={0},enabled={0},id={0},ignored={0};
   if(!adapter_view_position(vm,list,&view,1,&position))return false;
   if(position.i<0)return true;
   dvm_ref adapter=ui_obj(vm,list,"listAdapter","Landroid/widget/ListAdapter;");
   if(!adapter)return true;
   if(!am_vcall(vm,adapter,"isEnabled","(I)Z",&position,1,&enabled))return false;
   if(!enabled.i)return true;
   if(!am_vcall(vm,adapter,"getItemId","(I)J",&position,1,&id))return false;
   struct rt_list *rows=list_of(vm,view_children(vm,list,false));
   int offset=position.i-ui_int(vm,list,"firstPosition");
   if(!rows || offset<0 || (uint32_t)offset>=rows->size)return true;
   union dvm_value call[3]={{.l=rows->items[offset]},position,id};
   return am_vcall(vm,list,"performItemClick","(Landroid/view/View;IJ)Z",call,3,&ignored);
}
static const struct rt_method rt_item_click_listener[]={
   {"onItemClick","(Landroid/widget/AdapterView;Landroid/view/View;IJ)V",NULL,DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT},M_END
};
