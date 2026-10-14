/* MPL-2.0. Android/Skia blend modes on straight ARGB pixels. */
#ifndef LUNA_COLOR_BLEND_H
#define LUNA_COLOR_BLEND_H
#include <stdint.h>
#include <math.h>
static inline float luna_blend_lum(const float c[3])
{return .3f*c[0]+.59f*c[1]+.11f*c[2];}
static inline float luna_blend_sat(const float c[3])
{return fmaxf(c[0],fmaxf(c[1],c[2]))-fminf(c[0],fminf(c[1],c[2]));}
static inline void luna_blend_set_lum(float c[3],float lum)
{
 float delta=lum-luna_blend_lum(c);for(int i=0;i<3;++i)c[i]+=delta;
 float n=fminf(c[0],fminf(c[1],c[2])),x=fmaxf(c[0],fmaxf(c[1],c[2]));
 if(n<0)for(int i=0;i<3;++i)c[i]=lum+(c[i]-lum)*lum/(lum-n);
 if(x>1)for(int i=0;i<3;++i)c[i]=lum+(c[i]-lum)*(1-lum)/(x-lum);
}
static inline void luna_blend_set_sat(float c[3],float sat)
{
 int order[3]={0,1,2};
 for(int i=0;i<2;++i)for(int j=i+1;j<3;++j)if(c[order[i]]>c[order[j]]){int v=order[i];order[i]=order[j];order[j]=v;}
 int lo=order[0],mid=order[1],hi=order[2];
 if(c[hi]>c[lo]){c[mid]=(c[mid]-c[lo])*sat/(c[hi]-c[lo]);c[hi]=sat;}else{c[mid]=c[hi]=0;}
 c[lo]=0;
}
static inline uint32_t luna_blend_argb(uint32_t source,uint32_t destination,int mode)
{
 if(mode==5){unsigned alpha=((source>>24)*(destination>>24)+127)/255;return alpha ? (source&0xffffffu)|(alpha<<24) : 0;}
 float sa=(source>>24)/255.f,da=(destination>>24)/255.f,s[3],d[3],p[3]={0},a=0,fs=0,fd=0;
 for(int i=0;i<3;++i){s[i]=((source>>(16-8*i))&255)/255.f;d[i]=((destination>>(16-8*i))&255)/255.f;}
 switch(mode){
 case 0:break;case 1:fs=1;break;case 2:fd=1;break;
 case 3:fs=1;fd=1-sa;break;case 4:fs=1-da;fd=1;break;
 case 5:fs=da;break;case 6:fd=sa;break;case 7:fs=1-da;break;case 8:fd=1-sa;break;
 case 9:fs=da;fd=1-sa;break;case 10:fs=1-da;fd=sa;break;case 11:fs=1-da;fd=1-sa;break;
 }
 if(mode<=11){a=sa*fs+da*fd;for(int i=0;i<3;++i)p[i]=s[i]*sa*fs+d[i]*da*fd;}
 else if(mode==12){a=fminf(1,sa+da);for(int i=0;i<3;++i)p[i]=fminf(1,s[i]*sa+d[i]*da);}
 else if(mode==13){a=sa*da;for(int i=0;i<3;++i)p[i]=s[i]*d[i]*a;}
 else {
  float b[3];a=sa+da-sa*da;
  if(mode>=25){
   for(int i=0;i<3;++i)b[i]=(mode==25 || mode==27) ? s[i] : d[i];
   if(mode==25)luna_blend_set_sat(b,luna_blend_sat(d));
   if(mode==26)luna_blend_set_sat(b,luna_blend_sat(s));
   luna_blend_set_lum(b,mode==28 ? luna_blend_lum(s) : luna_blend_lum(d));
  } else for(int i=0;i<3;++i){float x=s[i],y=d[i];
   switch(mode){
   case 14:b[i]=x+y-x*y;break;
   case 15:b[i]=y<=.5f ? 2*x*y : 1-2*(1-x)*(1-y);break;
   case 16:b[i]=fminf(x,y);break;case 17:b[i]=fmaxf(x,y);break;
   case 18:b[i]=y==0 ? 0 : x==1 ? 1 : fminf(1,y/(1-x));break;
   case 19:b[i]=y==1 ? 1 : x==0 ? 0 : 1-fminf(1,(1-y)/x);break;
   case 20:b[i]=x<=.5f ? 2*x*y : 1-2*(1-x)*(1-y);break;
   case 21:{float z=y<=.25f ? ((16*y-12)*y+4)*y : sqrtf(y);b[i]=x<=.5f ? y-(1-2*x)*y*(1-y) : y+(2*x-1)*(z-y);break;}
   case 22:b[i]=fabsf(y-x);break;case 23:b[i]=x+y-2*x*y;break;
   default:b[i]=x*y;break;
   }
  }
  for(int i=0;i<3;++i)p[i]=s[i]*sa*(1-da)+d[i]*da*(1-sa)+b[i]*sa*da;
 }
 if(!(a>0))return 0;
 uint32_t result=(uint32_t)lroundf(fminf(1,a)*255)<<24;
 if(!(result>>24))return 0;
 for(int i=0;i<3;++i)result|=(uint32_t)lroundf(fmaxf(0,fminf(1,p[i]/a))*255)<<(16-8*i);
 return result;
}
#endif
