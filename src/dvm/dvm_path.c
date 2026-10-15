/* C path geometry. NanoSVG supplies cubic tessellation and scanline coverage;
 * no window, GL context, or platform font dependency is required. */
#include "dvm_path.h"
#include "../luna-ui/nanosvgrast.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>

struct path_contour {NSVGpath node;size_t capacity;};

static void map(const float *m,float x,float y,float *ox,float *oy)
{
   if (!m) {*ox=x;*oy=y;return;}
   float d=m[6]*x+m[7]*y+m[8];
   *ox=(m[0]*x+m[1]*y+m[2])/d;*oy=(m[3]*x+m[4]*y+m[5])/d;
}
void luna_path_clear(luna_path *p,bool reset_fill)
{
   NSVGpath *chains[]={p->first,p->spare};
   for(int i=0;i<2;++i) {
      NSVGpath *n=chains[i];
      while(n) {NSVGpath *next=n->next;free(n->pts);free(n);n=next;}
   }
   p->first=p->last=p->spare=NULL;p->capacity=0;p->after_close=false;
   if(reset_fill) p->fill=0;
}
void luna_path_rewind(luna_path *p)
{
   if(p->last) {p->last->next=p->spare;p->spare=p->first;}
   p->first=p->last=NULL;p->capacity=0;p->after_close=false;p->fill=0;
}
void luna_path_free(void *data)
{if(data) {luna_path_clear(data,true);free(data);}}
static bool reserve(luna_path *p,int count)
{
   size_t needed=(size_t)p->last->npts+(size_t)count;
   if(needed>INT32_MAX || needed>SIZE_MAX/(2*sizeof(float))) return false;
   if(needed<=p->capacity) return true;
   size_t cap=p->capacity ? p->capacity*2 : 16;
   if(cap<needed) cap=needed;
   float *points=realloc(p->last->pts,cap*2*sizeof *points);
   if(!points) return false;
   p->last->pts=points;p->capacity=cap;((struct path_contour *)p->last)->capacity=cap;return true;
}
bool luna_path_move(luna_path *p,float x,float y)
{
   if(p->last && p->last->npts==1 && !p->after_close) {
      p->last->pts[0]=x;p->last->pts[1]=y;return true;
   }
   NSVGpath *n=p->spare;
   if(n) p->spare=n->next;
   else {
      struct path_contour *contour=calloc(1,sizeof *contour);
      if(!contour) return false;
      n=&contour->node;contour->capacity=16;
      n->pts=malloc(32*sizeof *n->pts);
      if(!n->pts) {free(contour);return false;}
   }
   n->next=NULL;n->closed=0;n->npts=1;n->pts[0]=x;n->pts[1]=y;
   if(p->last) p->last->next=n;else p->first=n;
   p->last=n;p->capacity=((struct path_contour *)n)->capacity;p->after_close=false;return true;
}
static bool begin_segment(luna_path *p)
{
   if(!p->last) return luna_path_move(p,0,0);
   if(p->after_close) return luna_path_move(p,p->last->pts[0],p->last->pts[1]);
   return true;
}
bool luna_path_cubic(luna_path *p,const float q[6])
{
   if(!begin_segment(p) || !reserve(p,3)) return false;
   memcpy(p->last->pts+2*p->last->npts,q,6*sizeof *q);p->last->npts+=3;return true;
}
bool luna_path_line(luna_path *p,float x,float y)
{
   if(!begin_segment(p)) return false;
   float *last=p->last->pts+2*(p->last->npts-1);
   float q[]={last[0]+(x-last[0])/3,last[1]+(y-last[1])/3,
              last[0]+2*(x-last[0])/3,last[1]+2*(y-last[1])/3,x,y};
   return luna_path_cubic(p,q);
}
bool luna_path_quad(luna_path *p,float x1,float y1,float x2,float y2)
{
   if(!begin_segment(p)) return false;
   float *last=p->last->pts+2*(p->last->npts-1);
   float q[]={last[0]+(x1-last[0])*2/3,last[1]+(y1-last[1])*2/3,
              x2+(x1-x2)*2/3,y2+(y1-y2)*2/3,x2,y2};
   return luna_path_cubic(p,q);
}
bool luna_path_close(luna_path *p)
{
   if(p->last) {p->last->closed=1;p->after_close=true;}return true;
}
bool luna_path_append(luna_path *p,const luna_path *other,const float *matrix)
{
   /* Capture the old tail so addPath(this) cannot loop through appended data. */
   const NSVGpath *end=other->last;
   for(const NSVGpath *n=other->first;n;) {
      int count=n->npts;bool closed=n->closed;const NSVGpath *next=n->next;
      float *points=malloc((size_t)count*2*sizeof *points);
      if(!points) return false;
      for(int i=0;i<count;++i) map(matrix,n->pts[2*i],n->pts[2*i+1],&points[2*i],&points[2*i+1]);
      bool good=luna_path_move(p,points[0],points[1]);
      for(int i=1;i<count && good;i+=3) good=luna_path_cubic(p,points+2*i);
      if(good && closed) good=luna_path_close(p);
      free(points);if(!good) return false;
      if(n==end) break;
      n=next;
   }
   return true;
}
bool luna_path_transform(luna_path *p,const float matrix[9])
{
   for(NSVGpath *n=p->first;n;n=n->next) for(int i=0;i<n->npts;++i)
      map(matrix,n->pts[2*i],n->pts[2*i+1],&n->pts[2*i],&n->pts[2*i+1]);
   return true;
}
bool luna_path_arc(luna_path *p,const float r[4],float start,float sweep,bool move)
{
   if(!(r[0]<r[2] && r[1]<r[3]) || !isfinite(start) || !isfinite(sweep)) return true;
   float rx=(r[2]-r[0])/2,ry=(r[3]-r[1])/2,cx=(r[0]+r[2])/2,cy=(r[1]+r[3])/2;
   double a=fmod(start,360)*3.14159265358979323846/180;
   double total=fmax(-360,fmin(360,sweep))*3.14159265358979323846/180;
   /* At most 45 degrees per cubic keeps circle error below 5e-6 of radius. */
   int steps=(int)ceil(fabs(total)/(3.14159265358979323846/4));
   float x=cx+rx*(float)cos(a),y=cy+ry*(float)sin(a);
   if(!((move || !p->last) ? luna_path_move(p,x,y) : luna_path_line(p,x,y))) return false;
   for(int i=0;i<steps;++i) {
      double b=a+total/steps,k=4.0/3*tan((b-a)/4);
      float q[]={x-rx*(float)(k*sin(a)),y+ry*(float)(k*cos(a)),
         cx+rx*(float)(cos(b)+k*sin(b)),cy+ry*(float)(sin(b)-k*cos(b)),
         cx+rx*(float)cos(b),cy+ry*(float)sin(b)};
      if(!luna_path_cubic(p,q)) return false;
      a=b;x=q[4];y=q[5];
   }
   return true;
}
bool luna_path_round_rect(luna_path *path,const float rect[4],const float input[8],bool direction)
{
   for(int i=0;i<4;++i) if(!isfinite(rect[i])) return true;
   if(!(rect[0]<rect[2] && rect[1]<rect[3])) return true;
   float radii[8];memcpy(radii,input,sizeof radii);
   bool finite=true;for(int i=0;i<8;++i) finite=finite && isfinite(radii[i]);
   if(!finite) memset(radii,0,sizeof radii);
   for(int i=0;i<8;i+=2) {
      if(radii[i]<=0 || radii[i+1]<=0) radii[i]=radii[i+1]=0;
   }
   double width=(double)rect[2]-rect[0],height=(double)rect[3]-rect[1],scale=1;
   double sums[]={(double)radii[0]+radii[2],(double)radii[6]+radii[4],
      (double)radii[1]+radii[7],(double)radii[3]+radii[5]};
   for(int i=0;i<4;++i) if(sums[i]>0) scale=fmin(scale,(i<2 ? width : height)/sums[i]);
   for(int i=0;i<8;++i) radii[i]=(float)(radii[i]*scale);
   /* Build clockwise first, then reverse all cubic contours for CCW. */
   luna_path rounded={0};
   bool good=luna_path_move(&rounded,rect[0]+radii[0],rect[1]);
   const int corners[]={1,2,3,0};
   for(int j=0;j<4 && good;++j) {
      int c=corners[j];float rx=radii[2*c],ry=radii[2*c+1];
      float cx=(c==1 || c==2) ? rect[2]-rx : rect[0]+rx;
      float cy=c>=2 ? rect[3]-ry : rect[1]+ry;
      float r[]={cx-rx,cy-ry,cx+rx,cy+ry};
      if(rx>0 && ry>0) good=luna_path_arc(&rounded,r,-90+j*90,90,false);
      else good=luna_path_line(&rounded,(c==1 || c==2) ? rect[2] : rect[0],c>=2 ? rect[3] : rect[1]);
   }
   good=good && luna_path_close(&rounded);
   if(good && direction && rounded.first) {
      NSVGpath *n=rounded.first;int count=n->npts;
      for(int i=0;i<count/2;++i) for(int k=0;k<2;++k) {float v=n->pts[2*i+k];n->pts[2*i+k]=n->pts[2*(count-1-i)+k];n->pts[2*(count-1-i)+k]=v;}
   }
   if(good) good=luna_path_append(path,&rounded,NULL);
   luna_path_clear(&rounded,true);
   return good;
}

bool luna_path_parse(luna_path *p,const char *data)
{
   if(!data) return false;
   size_t len=strlen(data);
   if(len>SIZE_MAX-128 || strpbrk(data,"\"<&")) return false;
   char *svg=malloc(len+128);if(!svg) return false;
   snprintf(svg,len+128,"<svg width=\"1\" height=\"1\"><path d=\"%s\"/></svg>",data);
   NSVGimage *image=nsvgParse(svg,"px",96);free(svg);
   if(!image) return false;
   bool good=true;
   for(NSVGshape *s=image->shapes;s && good;s=s->next) {
      /* NanoSVG prepends contours; PathMeasure trims the first written one. */
      NSVGpath *ordered=NULL,*item=s->paths;
      while(item) {NSVGpath *next=item->next;item->next=ordered;ordered=item;item=next;}
      s->paths=ordered;
      NSVGpath *last=s->paths;while(last && last->next) last=last->next;
      luna_path parsed={.first=s->paths,.last=last};
      good=luna_path_append(p,&parsed,NULL);
   }
   nsvgDelete(image);return good;
}
void luna_path_bounds(const luna_path *p,float bounds[4])
{
   memset(bounds,0,4*sizeof *bounds);bool first=true;
   for(const NSVGpath *n=p->first;n;n=n->next) for(int i=0;i<n->npts;++i) {
      float x=n->pts[2*i],y=n->pts[2*i+1];
      if(first) {bounds[0]=bounds[2]=x;bounds[1]=bounds[3]=y;first=false;}
      else {bounds[0]=fminf(bounds[0],x);bounds[1]=fminf(bounds[1],y);
            bounds[2]=fmaxf(bounds[2],x);bounds[3]=fmaxf(bounds[3],y);}
   }
}
/* De Casteljau subdivision for first-contour PathMeasure, used by vector trim. */
static bool flatten(luna_path *p,const float *v,int depth)
{
   float dx=v[6]-v[0],dy=v[7]-v[1];
   float d1=fabsf((v[2]-v[6])*dy-(v[3]-v[7])*dx),d2=fabsf((v[4]-v[6])*dy-(v[5]-v[7])*dx);
   float span=dx*dx+dy*dy;
   if(depth>=18 || (span>0 && (d1+d2)*(d1+d2)<0.0001f*span) ||
      (span==0 && hypotf(v[2]-v[0],v[3]-v[1])+hypotf(v[4]-v[0],v[5]-v[1])<0.01f))
      return luna_path_line(p,v[6],v[7]);
   float a[8],b[8],x01=(v[0]+v[2])/2,y01=(v[1]+v[3])/2,x12=(v[2]+v[4])/2,y12=(v[3]+v[5])/2;
   float x23=(v[4]+v[6])/2,y23=(v[5]+v[7])/2,x012=(x01+x12)/2,y012=(y01+y12)/2;
   float x123=(x12+x23)/2,y123=(y12+y23)/2,x=(x012+x123)/2,y=(y012+y123)/2;
   float left[]={v[0],v[1],x01,y01,x012,y012,x,y},right[]={x,y,x123,y123,x23,y23,v[6],v[7]};
   memcpy(a,left,sizeof a);memcpy(b,right,sizeof b);
   return flatten(p,a,depth+1) && flatten(p,b,depth+1);
}
static bool trim_range(luna_path *out,const NSVGpath *n,double lo,double hi)
{
   double offset=0;bool started=false;
   for(int i=1;i<n->npts;i+=3) {
      const float *a=n->pts+2*(i-1),*b=n->pts+2*(i+2);
      double len=hypot(b[0]-a[0],b[1]-a[1]),from=fmax(lo-offset,0),to=fmin(hi-offset,len);
      if(from<to && len>0) {
         float x=a[0]+(b[0]-a[0])*(float)(from/len),y=a[1]+(b[1]-a[1])*(float)(from/len);
         if(!started && !luna_path_move(out,x,y)) return false;
         started=true;
         if(!luna_path_line(out,a[0]+(b[0]-a[0])*(float)(to/len),a[1]+(b[1]-a[1])*(float)(to/len))) return false;
      }
      offset+=len;
   }
   return true;
}
bool luna_path_trim(luna_path *out,const luna_path *p,float start,float end,float offset)
{
   if(start==0 && end==1) return luna_path_append(out,p,NULL);
   if(!p->first) return true;
   luna_path lines={0};const NSVGpath *n=p->first;
   bool good=luna_path_move(&lines,n->pts[0],n->pts[1]);
   for(int i=0;i+3<n->npts && good;i+=3) good=flatten(&lines,n->pts+2*i,0);
   if(good && n->closed) good=luna_path_line(&lines,n->pts[0],n->pts[1]);
   double length=0;
   for(int i=1;good && i<lines.last->npts;i+=3) {
      float *a=lines.last->pts+2*(i-1),*b=lines.last->pts+2*(i+2);length+=hypot(b[0]-a[0],b[1]-a[1]);
   }
   double a=fmod(start+offset,1)*length,b=fmod(end+offset,1)*length;
   if(good) good=a>b ? trim_range(out,lines.last,a,length) && trim_range(out,lines.last,0,b) : trim_range(out,lines.last,a,b);
   luna_path_clear(&lines,true);return good;
}
struct path_samples { float *values;size_t count,capacity;float length; };
static bool sample_add(struct path_samples *s,float x,float y,bool move)
{
   if(s->count==s->capacity) {
      size_t cap=s->capacity ? s->capacity*2 : 32;
      if(cap<s->capacity || cap>SIZE_MAX/(3*sizeof(float))) return false;
      float *v=realloc(s->values,cap*3*sizeof *v);if(!v) return false;
      s->values=v;s->capacity=cap;
   }
   if(s->count && !move) {
      float *last=s->values+(s->count-1)*3;
      s->length+=hypotf(x-last[1],y-last[2]);
   }
   float *v=s->values+s->count++*3;v[0]=s->length;v[1]=x;v[2]=y;return true;
}
/* Bound the error against the straight segment at the same parameter.
 * Unlike distance to the chord, this also subdivides collinear reversals. */
static bool sample_cubic(struct path_samples *s,const float *v,float error,int depth)
{
   float dx=v[6]-v[0],dy=v[7]-v[1];
   float d1=hypotf(v[2]-(v[0]+dx/3),v[3]-(v[1]+dy/3));
   float d2=hypotf(v[4]-(v[0]+2*dx/3),v[5]-(v[1]+2*dy/3));
   if(depth>=24 || fmaxf(d1,d2)<=error) return sample_add(s,v[6],v[7],false);
   float x01=(v[0]+v[2])/2,y01=(v[1]+v[3])/2,x12=(v[2]+v[4])/2,y12=(v[3]+v[5])/2;
   float x23=(v[4]+v[6])/2,y23=(v[5]+v[7])/2,x012=(x01+x12)/2,y012=(y01+y12)/2;
   float x123=(x12+x23)/2,y123=(y12+y23)/2,x=(x012+x123)/2,y=(y012+y123)/2;
   float a[]={v[0],v[1],x01,y01,x012,y012,x,y};
   float b[]={x,y,x123,y123,x23,y23,v[6],v[7]};
   return sample_cubic(s,a,error,depth+1) && sample_cubic(s,b,error,depth+1);
}
float *luna_path_approximate(const luna_path *p,float error,size_t *count)
{
   *count=0;struct path_samples s={0};bool good=true;
   if(!isfinite(error) || error<0) return NULL;
   /* Float subdivision cannot distinguish parameters below this precision. */
   error=fmaxf(error,1e-7f);
   for(const NSVGpath *n=p->first;n && good;n=n->next) {
      for(int i=0;i<n->npts*2;++i) if(!isfinite(n->pts[i])) {good=false;break;}
      if(!good) break;
      good=sample_add(&s,n->pts[0],n->pts[1],true);
      for(int i=0;i+3<n->npts && good;i+=3) good=sample_cubic(&s,n->pts+2*i,error,0);
      if(n->closed && good) good=sample_add(&s,n->pts[0],n->pts[1],false);
   }
   if(good && !s.count) good=sample_add(&s,0,0,true);
   if(good && s.length==0) {
      float *last=s.values+(s.count-1)*3;float x=last[1],y=last[2];
      good=sample_add(&s,x,y,true);s.length=1;if(good) s.values[(s.count-1)*3]=1;
   }
   if(!good) {free(s.values);return NULL;}
   for(size_t i=0;i<s.count;++i) s.values[i*3]/=s.length;
   *count=s.count;return s.values;
}
unsigned char *luna_path_mask(const luna_path *p,int w,int h,const float matrix[9],
                             int style,float stroke,int cap,int join,float miter,bool aa)
{
   if(w<=0 || h<=0 || w>INT32_MAX/4 || (size_t)w>SIZE_MAX/(size_t)h/4) return NULL;
   luna_path copy={0};if(!luna_path_append(&copy,p,matrix)) {luna_path_clear(&copy,true);return NULL;}
   for(NSVGpath *n=copy.first;n;n=n->next) {
      luna_path part={.first=n,.last=n};
      /* bounds are per contour; prevent walking the following contours. */
      NSVGpath *next=n->next;n->next=NULL;luna_path_bounds(&part,n->bounds);n->next=next;
   }
   NSVGshape shape={0};shape.paths=copy.first;shape.opacity=1;shape.flags=NSVG_FLAGS_VISIBLE;
   shape.fillRule=p->fill&1;shape.strokeWidth=stroke;shape.strokeLineCap=(char)cap;
   shape.strokeLineJoin=(char)join;shape.miterLimit=miter;
   shape.paintOrder=NSVG_PAINT_FILL | (NSVG_PAINT_STROKE<<2) | (NSVG_PAINT_MARKERS<<4);
   shape.fill.type=style!=1 ? NSVG_PAINT_COLOR : NSVG_PAINT_NONE;shape.fill.color=0xffffffffu;
   shape.stroke.type=style!=0 ? NSVG_PAINT_COLOR : NSVG_PAINT_NONE;shape.stroke.color=0xffffffffu;
   unsigned char *pixels=calloc((size_t)w*h,4);NSVGrasterizer *r=nsvgCreateRasterizer();
   if(!pixels || !r) {free(pixels);pixels=NULL;}
   else {
      NSVGimage image={.width=(float)w,.height=(float)h,.shapes=&shape};
      nsvgRasterize(r,&image,0,0,1,pixels,w,h,w*4);
      for(size_t i=0;i<(size_t)w*h;++i) {
         unsigned char coverage=pixels[4*i+3];
         if(!aa) coverage=coverage>=128 ? 255 : 0;
         if((p->fill&2) && style!=1) coverage=255-coverage;
         pixels[4*i+3]=coverage;
      }
   }
   nsvgDeleteRasterizer(r);luna_path_clear(&copy,true);return pixels;
}
