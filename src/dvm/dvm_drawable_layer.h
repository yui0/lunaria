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
