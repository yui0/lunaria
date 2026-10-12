/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* Private C BitmapDrawable placement and software Canvas bridge. */
static bool bd_get_paint(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                         int nargs,union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref paint=ui_obj(vm,self,"paint","Landroid/graphics/Paint;");
   if (!paint) {
      paint=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Paint"));
      if (!paint) return false;
      (void)dvm_set_field(vm,self,"paint","Landroid/graphics/Paint;",(union dvm_value){.l=paint});
      union dvm_value flags={.i=6},ignored={0};
      if (!paint_init(vm,paint,&flags,1,&ignored)) return false;
   }
   RETL(paint);
}
static bool bd_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                        int nargs,union dvm_value *out)
{
   (void)nargs;
   const char *name=builtin_name();
   if (!strcmp(name,"setBitmap")) {
      if (ui_obj(vm,self,"bitmap","Landroid/graphics/Bitmap;")!=ARG(0).l) {
         (void)dvm_set_field(vm,self,"bitmap","Landroid/graphics/Bitmap;",ARG(0));
         bd_compute_size(vm,self); ui_invalidate();
      }
      RETV();
   }
   if (!strcmp(name,"setTargetDensity")) {
      int32_t density=ARG(0).i;
      if (strstr(builtin_sig(),"DisplayMetrics")) {
         if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","metrics"); return false;}
         density=ui_int(vm,ARG(0).l,"densityDpi");
      } else if (strstr(builtin_sig(),"Canvas")) {
         union dvm_value result={0};
         if (!am_vcall(vm,ARG(0).l,"getDensity","()I",NULL,0,&result)) return false;
         density=result.i;
      }
      if (!density) density=160;
      if (density!=ui_int(vm,self,"targetDensity")) {
         (void)dvm_set_field(vm,self,"targetDensity","I",(union dvm_value){.i=density});
         bd_compute_size(vm,self); ui_invalidate();
      }
      RETV();
   }
   if (strstr(name,"Gravity") || strstr(name,"Mirrored")) {
      bool gravity=strstr(name,"Gravity")!=NULL;
      const char *field=gravity ? "gravity" : "autoMirrored", *type=gravity ? "I" : "Z";
      union dvm_value old={0};
      (void)dvm_get_field(vm,self,field,type,&old);
      if (name[0]!='s') RETI(old.i);
      if (old.i!=ARG(0).i) { (void)dvm_set_field(vm,self,field,type,ARG(0)); ui_invalidate(); }
      RETV();
   }
   union dvm_value paint={0},value={0};
   if (!bd_get_paint(vm,self,NULL,0,&paint)) return false;
   if (strstr(name,"Alpha")) {
      (void)dvm_get_field(vm,paint.l,"color","I",&value);
      if (name[0]!='s') RETI(value.u>>24);
      value.u=(value.u&0x00ffffffu) | ((ARG(0).u&255u)<<24);
      (void)dvm_set_field(vm,paint.l,"color","I",value);
   } else {
      int bit=strstr(name,"AntiAlias") ? 1 : strstr(name,"FilterBitmap") ? 2 : 4;
      (void)dvm_get_field(vm,paint.l,"flags","I",&value);
      if (name[0]!='s') RETI((value.i&bit)!=0);
      value.i=ARG(0).i ? value.i|bit : value.i&~bit;
      (void)dvm_set_field(vm,paint.l,"flags","I",value);
   }
   ui_invalidate(); RETV();
}
static bool bd_draw(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                    int nargs,union dvm_value *out)
{
   (void)nargs;
   dvm_ref bitmap=ui_obj(vm,self,"bitmap","Landroid/graphics/Bitmap;");
   if (!bitmap) RETV();
   if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","canvas"); return false;}
   struct rt_bitmap *bm=bitmap_of(vm,bitmap);
   if (!bitmap_live(vm,bm)) return false;
   if (ui_obj(vm,self,"colorFilter","Landroid/graphics/ColorFilter;") ||
       ui_int(vm,self,"tileModeX")!=-1 || ui_int(vm,self,"tileModeY")!=-1) {
      dvm__throw(vm,"java/lang/UnsupportedOperationException","BitmapDrawable color filters and tiles are not implemented");
      return false;
   }
   union dvm_value bounds={0},paint={0},iw={0},ih={0},ignored={0};
   if (!drawable_get_bounds(vm,self,NULL,0,&bounds) || !bd_get_paint(vm,self,NULL,0,&paint)) return false;
   iw.i=ui_int(vm,self,"bitmapWidth"); ih.i=ui_int(vm,self,"bitmapHeight");
   dvm_ref rect=ui_obj(vm,self,"drawRect","Landroid/graphics/Rect;");
   if (!rect) {
      rect=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Rect"));
      if (!rect) return false;
      (void)dvm_set_field(vm,self,"drawRect","Landroid/graphics/Rect;",(union dvm_value){.l=rect});
   }
   union dvm_value place[]={{.i=ui_int(vm,self,"gravity")},iw,ih,bounds,{.l=rect},
      {.i=ui_int(vm,self,"layoutDirection")}};
   if (!gravity_apply(vm,0,place,6,&ignored)) return false;
   int32_t left=ui_int(vm,rect,"left"),top=ui_int(vm,rect,"top");
   int32_t right=ui_int(vm,rect,"right"),bottom=ui_int(vm,rect,"bottom");
   if (right<=left || bottom<=top || bm->w<=0 || bm->h<=0) RETV();
   dvm_ref matrix=ui_obj(vm,self,"drawMatrix","Landroid/graphics/Matrix;");
   if (!matrix) {
      matrix=dvm_new_object(vm,dvm_find_class(vm,"android/graphics/Matrix"));
      if (!matrix) return false;
      (void)dvm_set_field(vm,self,"drawMatrix","Landroid/graphics/Matrix;",(union dvm_value){.l=matrix});
   }
   float *values=matrix_values(vm,matrix);
   if (!values) return false;
   bool mirror=ui_int(vm,self,"autoMirrored") && ui_int(vm,self,"layoutDirection")==1;
   float width=(float)((int64_t)right-left),height=(float)((int64_t)bottom-top);
   memset(values,0,9*sizeof *values);
   values[0]=(mirror ? -width : width)/bm->w; values[4]=height/bm->h;
   values[2]=(float)(mirror ? right : left); values[5]=(float)top; values[8]=1;
   union dvm_value draw[]={{.l=bitmap},{.l=matrix},paint};
   return canvas_draw_bitmap(vm,ARG(0).l,draw,3,out);
}
