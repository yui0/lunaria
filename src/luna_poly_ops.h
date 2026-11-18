/* Copyright © 2026 Project Lunaria. SPDX-License-Identifier: MPL-2.0 */
/* Polygon region operations, C11, standard library only.
 * Define LUNA_POLY_OPS_IMPLEMENTATION in one translation unit.
 * Contours close implicitly. Fill bits: 1 even/odd, 2 inverse; otherwise
 * nonzero winding. Operations: A-B, intersection, union, xor, B-A (0..4).
 * Input coordinates and emitted coordinates are float, as in Android Path.
 * Intersections and predicates use long double. There is no pixel grid or
 * fixed geometric epsilon. A BVH prunes intersection and winding queries.
 * Coincident edges are split and classified once. Boundary membership uses
 * an infinitesimal normal displacement, not a finite distance sample.
 * The emit callback receives move=0, line=1, close=2. Failure can leave partial
 * callback output: callers needing atomic output should stage the result.
 */
#ifndef LUNA_POLY_OPS_H
#define LUNA_POLY_OPS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lpo_vertex;
struct lpo_segment;
typedef struct luna_poly_ops {
  struct lpo_vertex *vertices;
  struct lpo_segment *segments;
  size_t nv,cv,ns,cs;
  size_t *vertex_table;
  size_t table_capacity;
  unsigned result_fill; /* 1 even/odd, 3 inverse even/odd after compute */
} luna_poly_ops;
typedef bool (*luna_poly_emit)(void *user,int action,float x,float y);
bool luna_poly_add(luna_poly_ops *,unsigned operand,const float *xy,size_t count,size_t stride);
bool luna_poly_compute(luna_poly_ops *,unsigned fill_a,unsigned fill_b,int operation,luna_poly_emit,void *);
void luna_poly_free(luna_poly_ops *);
#ifdef __cplusplus
}
#endif
#endif

#ifdef LUNA_POLY_OPS_IMPLEMENTATION
#ifndef LUNA_POLY_OPS_IMPLEMENTED
#define LUNA_POLY_OPS_IMPLEMENTED
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
struct lpo_vertex { float x,y; size_t head; };
struct lpo_segment { size_t a,b; unsigned operand; };
typedef struct lpo_cut { size_t segment,vertex; long double t; } lpo_cut;
typedef struct lpo_edge { size_t a,b,next; bool used; } lpo_edge;
typedef struct lpo_order { size_t segment; long double key; } lpo_order;
typedef struct lpo_box { long double x0,y0,x1,y1; size_t left,right,first,count; } lpo_box;
typedef struct lpo_work {
  luna_poly_ops *p;
  lpo_order *order;
  lpo_box *boxes;
  size_t nb;
  lpo_box operand_box[2];
  size_t operand_count[2];
  lpo_cut *cuts;
  size_t nc,cc;
  lpo_edge *edges;
  size_t ne,ce;
  size_t *edge_table,edge_capacity;
} lpo_work;

static bool lpo_grow(void **p,size_t *cap,size_t need,size_t item)
{
  if(need<=*cap)return true;
  if(need>SIZE_MAX/item)return false;
  size_t n=*cap ? *cap : 16;
  while(n<need) { if(n>SIZE_MAX/2) { n=need;break; } n*=2; }
  if(n>SIZE_MAX/item)n=need;
  void *v=realloc(*p,n*item);
  if(!v)return false;
  *p=v;*cap=n;return true;
}
static uint64_t lpo_hash(uint64_t x)
{
  x^=x>>30;x*=UINT64_C(0xbf58476d1ce4e5b9);
  x^=x>>27;x*=UINT64_C(0x94d049bb133111eb);
  return x^(x>>31);
}
static uint64_t lpo_point_hash(float x,float y)
{
  uint32_t a,b;memcpy(&a,&x,4);memcpy(&b,&y,4);
  return lpo_hash(((uint64_t)a<<32)|b);
}
static bool lpo_vertex_get(luna_poly_ops *p,float x,float y,size_t *index)
{
  if(!isfinite(x)||!isfinite(y))return false;
  if(x==0)x=0;
  if(y==0)y=0; /* canonicalize negative zero */
  if(!p->table_capacity || p->nv>=p->table_capacity/2) {
    if(p->table_capacity>SIZE_MAX/2/sizeof(size_t))return false;
    size_t cap=p->table_capacity ? p->table_capacity*2 : 32;
    size_t *table=calloc(cap,sizeof *table);
    if(!table)return false;
    for(size_t i=0;i<p->nv;++i) {
      size_t j=(size_t)lpo_point_hash(p->vertices[i].x,p->vertices[i].y)&(cap-1);
      while(table[j])j=(j+1)&(cap-1);
      table[j]=i+1;
    }
    free(p->vertex_table);p->vertex_table=table;p->table_capacity=cap;
  }
  size_t at=(size_t)lpo_point_hash(x,y)&(p->table_capacity-1);
  while(p->vertex_table[at]) {
    size_t i=p->vertex_table[at]-1;
    if(p->vertices[i].x==x && p->vertices[i].y==y) { *index=i;return true; }
    at=(at+1)&(p->table_capacity-1);
  }
  if(!lpo_grow((void **)&p->vertices,&p->cv,p->nv+1,sizeof *p->vertices))return false;
  *index=p->nv;p->vertices[p->nv++]=(struct lpo_vertex){x,y,SIZE_MAX};
  p->vertex_table[at]=*index+1;
  return true;
}
bool luna_poly_add(luna_poly_ops *p,unsigned operand,const float *xy,size_t count,size_t stride)
{
  if(!p || operand>1 || (!xy && count) || stride<2*sizeof(float) ||
     (count && count-1>(SIZE_MAX-2*sizeof(float))/stride))return false;
  size_t first=0,previous=0,before=p->ns;
  for(size_t i=0;i<count;++i) {
    float v[2];memcpy(v,(const unsigned char *)xy+i*stride,sizeof v);
    size_t vertex;
    if(!lpo_vertex_get(p,v[0],v[1],&vertex))goto failure;
    if(!i)first=vertex;
    else if(previous!=vertex) {
      if(!lpo_grow((void **)&p->segments,&p->cs,p->ns+1,sizeof *p->segments))goto failure;
      p->segments[p->ns++]=(struct lpo_segment){previous,vertex,operand};
    }
    previous=vertex;
  }
  if(count>1 && previous!=first) {
    if(!lpo_grow((void **)&p->segments,&p->cs,p->ns+1,sizeof *p->segments))goto failure;
    p->segments[p->ns++]=(struct lpo_segment){previous,first,operand};
  }
  return true;
failure:
  p->ns=before;return false;
}
void luna_poly_free(luna_poly_ops *p)
{
  if(!p)return;
  free(p->vertices);free(p->segments);free(p->vertex_table);memset(p,0,sizeof *p);
}
static long double lpo_cross(long double ax,long double ay,long double bx,long double by)
{ return ax*by-ay*bx; }
/* Error bounds scale with the operands, preserving arbitrarily small input
 * geometry. Zero means numerically indistinguishable from collinearity. */
static int lpo_cross_sign(long double ax,long double ay,long double bx,long double by)
{
  long double v=lpo_cross(ax,ay,bx,by);
  long double bound=16*LDBL_EPSILON*(fabsl(ax*by)+fabsl(ay*bx));
  return v>bound ? 1 : v< -bound ? -1 : 0;
}
static int lpo_order_compare(const void *a,const void *b)
{
  const lpo_order *x=a,*y=b;
  return x->key<y->key ? -1 : x->key>y->key ? 1 : x->segment<y->segment ? -1 : x->segment>y->segment;
}
static size_t lpo_build_box(lpo_work *w,size_t first,size_t count)
{
  size_t id=w->nb++;
  lpo_box *box=&w->boxes[id];
  *box=(lpo_box){LDBL_MAX,LDBL_MAX,-LDBL_MAX,-LDBL_MAX,0,0,first,count};
  for(size_t i=first;i<first+count;++i) {
    struct lpo_segment s=w->p->segments[w->order[i].segment];
    struct lpo_vertex a=w->p->vertices[s.a],b=w->p->vertices[s.b];
    box->x0=fminl(box->x0,fminl(a.x,b.x));box->x1=fmaxl(box->x1,fmaxl(a.x,b.x));
    box->y0=fminl(box->y0,fminl(a.y,b.y));box->y1=fmaxl(box->y1,fmaxl(a.y,b.y));
  }
  if(count<=8)return id;
  bool x=box->x1-box->x0>=box->y1-box->y0;
  for(size_t i=first;i<first+count;++i) {
    struct lpo_segment s=w->p->segments[w->order[i].segment];
    struct lpo_vertex a=w->p->vertices[s.a],b=w->p->vertices[s.b];
    w->order[i].key=x ? (long double)a.x+b.x : (long double)a.y+b.y;
  }
  qsort(w->order+first,count,sizeof *w->order,lpo_order_compare);
  box->left=lpo_build_box(w,first,count/2);
  box->right=lpo_build_box(w,first+count/2,count-count/2);
  box->count=0;
  return id;
}
static bool lpo_add_cut(lpo_work *w,size_t s,long double t,size_t v)
{
  if(!lpo_grow((void **)&w->cuts,&w->cc,w->nc+1,sizeof *w->cuts))return false;
  w->cuts[w->nc++]=(lpo_cut){s,v,t};return true;
}
static bool lpo_unit(long double *t)
{
  if(*t< -32*LDBL_EPSILON || *t>1+32*LDBL_EPSILON)return false;
  if(*t<0)*t=0;
  if(*t>1)*t=1;
  return true;
}
static bool lpo_on_segment(lpo_work *w,size_t segment,size_t vertex)
{
  struct lpo_segment s=w->p->segments[segment];
  struct lpo_vertex a=w->p->vertices[s.a],b=w->p->vertices[s.b],v=w->p->vertices[vertex];
  long double dx=(long double)b.x-a.x,dy=(long double)b.y-a.y;
  if(lpo_cross_sign(dx,dy,(long double)v.x-a.x,(long double)v.y-a.y))return true;
  long double t=fabsl(dx)>=fabsl(dy) ? ((long double)v.x-a.x)/dx : ((long double)v.y-a.y)/dy;
  return !lpo_unit(&t) || lpo_add_cut(w,segment,t,vertex);
}
static bool lpo_intersect(lpo_work *w,size_t i,size_t j)
{
  struct lpo_segment a=w->p->segments[i],b=w->p->segments[j];
  struct lpo_vertex p=w->p->vertices[a.a],q=w->p->vertices[a.b];
  struct lpo_vertex r=w->p->vertices[b.a],s=w->p->vertices[b.b];
  long double ax=(long double)q.x-p.x,ay=(long double)q.y-p.y;
  long double bx=(long double)s.x-r.x,by=(long double)s.y-r.y;
  long double rx=(long double)r.x-p.x,ry=(long double)r.y-p.y;
  if(!lpo_cross_sign(ax,ay,bx,by)) {
    if(lpo_cross_sign(ax,ay,rx,ry))return true;
    return lpo_on_segment(w,i,b.a) && lpo_on_segment(w,i,b.b) &&
           lpo_on_segment(w,j,a.a) && lpo_on_segment(w,j,a.b);
  }
  long double den=lpo_cross(ax,ay,bx,by);
  long double t=lpo_cross(rx,ry,bx,by)/den,u=lpo_cross(rx,ry,ax,ay)/den;
  if(!lpo_unit(&t)||!lpo_unit(&u))return true;
  size_t v;
  if(t==0)v=a.a;else if(t==1)v=a.b;else if(u==0)v=b.a;else if(u==1)v=b.b;
  else if(!lpo_vertex_get(w->p,(float)(p.x+t*ax),(float)(p.y+t*ay),&v))return false;
  return lpo_add_cut(w,i,t,v) && lpo_add_cut(w,j,u,v);
}
static bool lpo_query_intersections(lpo_work *w,size_t id,size_t segment,long double x0,long double y0,long double x1,long double y1)
{
  const lpo_box *b=&w->boxes[id];
  if(b->x1<x0||b->x0>x1||b->y1<y0||b->y0>y1)return true;
  if(b->count) {
    for(size_t k=b->first;k<b->first+b->count;++k) {
      size_t j=w->order[k].segment;
      if(j>segment && !lpo_intersect(w,segment,j))return false;
    }
    return true;
  }
  return lpo_query_intersections(w,b->left,segment,x0,y0,x1,y1) &&
         lpo_query_intersections(w,b->right,segment,x0,y0,x1,y1);
}
static int lpo_side_sign(long double v,long double scale,long double derivative)
{
  long double bound=16*LDBL_EPSILON*scale;
  return v>bound ? 1 : v< -bound ? -1 : derivative>0 ? 1 : derivative<0 ? -1 : 0;
}
static void lpo_winding(const lpo_work *w,size_t id,long double x,long double y,long double nx,long double ny,int64_t winding[2][2])
{
  const lpo_box *box=&w->boxes[id];
  long double e=32*LDBL_EPSILON*(fabsl(x)+fabsl(y)+fabsl(box->x1)+fabsl(box->y0)+fabsl(box->y1));
  if(box->x1<x-e || box->y0>y+e || box->y1<y-e)return;
  if(!box->count) {
    lpo_winding(w,box->left,x,y,nx,ny,winding);
    lpo_winding(w,box->right,x,y,nx,ny,winding);return;
  }
  for(size_t k=box->first;k<box->first+box->count;++k) {
    struct lpo_segment s=w->p->segments[w->order[k].segment];
    struct lpo_vertex a=w->p->vertices[s.a],b=w->p->vertices[s.b];
    long double dx=(long double)b.x-a.x,dy=(long double)b.y-a.y;
    long double px=x-a.x,py=y-a.y;
    long double cross=lpo_cross(dx,dy,px,py);
    long double scale=fabsl(dx)*(fabsl(y)+fabsl(a.y))+
                      fabsl(dy)*(fabsl(x)+fabsl(a.x))+
                      fabsl(px)*(fabsl(b.y)+fabsl(a.y))+
                      fabsl(py)*(fabsl(b.x)+fabsl(a.x));
    for(int side=0;side<2;++side) {
      long double sign=side ? 1 : -1;
      int ay=lpo_side_sign((long double)a.y-y,fabsl(a.y)+fabsl(y),-sign*ny);
      int by=lpo_side_sign((long double)b.y-y,fabsl(b.y)+fabsl(y),-sign*ny);
      int orient=lpo_side_sign(cross,scale,sign*lpo_cross(dx,dy,nx,ny));
      if(ay<=0 && by>0 && orient>0)++winding[side][s.operand];
      else if(ay>0 && by<=0 && orient<0)--winding[side][s.operand];
    }
  }
}
static bool lpo_value(int op,bool a,bool b)
{
  switch(op) { case 0:return a&&!b;case 1:return a&&b;case 2:return a||b;case 3:return a!=b;default:return b&&!a; }
}
static bool lpo_filled(int64_t winding,unsigned fill)
{ return ((fill&1) ? winding%2!=0 : winding!=0) != ((fill&2)!=0); }
static int lpo_cut_compare(const void *a,const void *b)
{
  const lpo_cut *x=a,*y=b;
  if(x->segment!=y->segment)return x->segment<y->segment ? -1 : 1;
  return x->t<y->t ? -1 : x->t>y->t ? 1 : x->vertex<y->vertex ? -1 : x->vertex>y->vertex;
}
static bool lpo_boundary(lpo_work *w,size_t a,size_t b,long double x,long double y,long double dx,long double dy,unsigned operand,unsigned fa,unsigned fb,int op)
{
  if(a==b)return true;
  /* Outside the other operand's bounds its membership is constant on both
   * sides. Discard this edge without ray queries when the truth table cannot
   * depend on its own operand (notably rounded-outline hit intersections). */
  unsigned other=1-operand;
  const lpo_box *box=&w->operand_box[other];
  bool outside=!w->operand_count[other];
  if(!outside) {
    long double e=32*LDBL_EPSILON*(fabsl(x)+fabsl(y)+fabsl(box->x0)+fabsl(box->x1)+fabsl(box->y0)+fabsl(box->y1));
    outside=x<box->x0-e || x>box->x1+e || y<box->y0-e || y>box->y1+e;
  }
  if(outside) {
    bool value=(((other ? fb : fa)&2)!=0);
    if(operand ? lpo_value(op,value,false)==lpo_value(op,value,true)
               : lpo_value(op,false,value)==lpo_value(op,true,value))return true;
  }
  size_t lo=a<b?a:b,hi=a<b?b:a;
  size_t at=(size_t)lpo_hash((uint64_t)lo ^ lpo_hash(hi))&(w->edge_capacity-1);
  while(w->edge_table[at]) {
    const lpo_edge *e=&w->edges[w->edge_table[at]-1];
    if((e->a==a && e->b==b)||(e->a==b && e->b==a))return true;
    at=(at+1)&(w->edge_capacity-1);
  }
  /* Record even a discarded edge so coincident copies are classified once. */
  if(!lpo_grow((void **)&w->edges,&w->ce,w->ne+1,sizeof *w->edges))return false;
  size_t edge=w->ne++;
  w->edges[edge]=(lpo_edge){a,b,SIZE_MAX,true};w->edge_table[at]=edge+1;
  int64_t winding[2][2]={{0,0},{0,0}};
  lpo_winding(w,0,x,y,-dy,dx,winding);
  bool right=lpo_value(op,lpo_filled(winding[0][0],fa),lpo_filled(winding[0][1],fb));
  bool left=lpo_value(op,lpo_filled(winding[1][0],fa),lpo_filled(winding[1][1],fb));
  if(left==right)return true;
  if(!left) { size_t t=a;a=b;b=t; }
  w->edges[edge]=(lpo_edge){a,b,w->p->vertices[a].head,false};
  w->p->vertices[a].head=edge;
  return true;
}
static bool lpo_emit_contours(lpo_work *w,luna_poly_emit emit,void *user)
{
  const long double tau=2*acosl(-1);
  for(size_t i=0;i<w->ne;++i)if(!w->edges[i].used) {
    size_t current=i,start=w->edges[i].a,steps=0;
    struct lpo_vertex first=w->p->vertices[start];
    if(!emit(user,0,first.x,first.y))return false;
    for(;;) {
      lpo_edge *edge=&w->edges[current];
      if(edge->used || ++steps>w->ne)return false;
      edge->used=true;
      struct lpo_vertex a=w->p->vertices[edge->a],b=w->p->vertices[edge->b];
      if(edge->b==start) { if(!emit(user,2,first.x,first.y))return false;break; }
      if(!emit(user,1,b.x,b.y))return false;
      size_t next=SIZE_MAX;long double best=LDBL_MAX;
      for(size_t k=b.head;k!=SIZE_MAX;k=w->edges[k].next)if(!w->edges[k].used) {
        struct lpo_vertex c=w->p->vertices[w->edges[k].b];
        long double rx=(long double)a.x-b.x,ry=(long double)a.y-b.y;
        long double ox=(long double)c.x-b.x,oy=(long double)c.y-b.y;
        long double angle=atan2l(lpo_cross(ox,oy,rx,ry),ox*rx+oy*ry);
        if(angle<0)angle+=tau;
        if(angle<best) { best=angle;next=k; }
      }
      if(next==SIZE_MAX)return false;
      current=next;
    }
  }
  return true;
}
bool luna_poly_compute(luna_poly_ops *p,unsigned fa,unsigned fb,int op,luna_poly_emit emit,void *user)
{
  if(!p||!emit||op<0||op>4||fa>3||fb>3)return false;
  p->result_fill=lpo_value(op,(fa&2)!=0,(fb&2)!=0) ? 3 : 1;
  if(!p->ns)return true;
  lpo_work w={0};w.p=p;bool good=false;
  if(p->ns>SIZE_MAX/2/sizeof *w.boxes || p->ns>SIZE_MAX/sizeof *w.order)goto done;
  w.order=malloc(p->ns*sizeof *w.order);w.boxes=malloc(p->ns*2*sizeof *w.boxes);
  if(!w.order||!w.boxes)goto done;
  for(unsigned i=0;i<2;++i)w.operand_box[i]=(lpo_box){LDBL_MAX,LDBL_MAX,-LDBL_MAX,-LDBL_MAX,0,0,0,0};
  for(size_t i=0;i<p->ns;++i) {
    w.order[i]=(lpo_order){i,0};
    struct lpo_segment s=p->segments[i];
    struct lpo_vertex a=p->vertices[s.a],b=p->vertices[s.b];
    lpo_box *box=&w.operand_box[s.operand];++w.operand_count[s.operand];
    box->x0=fminl(box->x0,fminl(a.x,b.x));box->x1=fmaxl(box->x1,fmaxl(a.x,b.x));
    box->y0=fminl(box->y0,fminl(a.y,b.y));box->y1=fmaxl(box->y1,fmaxl(a.y,b.y));
  }
  lpo_build_box(&w,0,p->ns);
  for(size_t i=0;i<p->nv;++i)p->vertices[i].head=SIZE_MAX;
  for(size_t i=0;i<p->ns;++i) {
    struct lpo_segment s=p->segments[i];
    struct lpo_vertex a=p->vertices[s.a],b=p->vertices[s.b];
    if(!lpo_add_cut(&w,i,0,s.a)||!lpo_add_cut(&w,i,1,s.b) ||
       !lpo_query_intersections(&w,0,i,fminl(a.x,b.x),fminl(a.y,b.y),fmaxl(a.x,b.x),fmaxl(a.y,b.y)))goto done;
  }
  qsort(w.cuts,w.nc,sizeof *w.cuts,lpo_cut_compare);
  w.edge_capacity=32;
  if(w.nc>SIZE_MAX/2/sizeof(size_t))goto done;
  while(w.edge_capacity<w.nc*2) { if(w.edge_capacity>SIZE_MAX/2)goto done;w.edge_capacity*=2; }
  w.edge_table=calloc(w.edge_capacity,sizeof *w.edge_table);
  if(!w.edge_table)goto done;
  for(size_t i=1;i<w.nc;++i) {
    lpo_cut a=w.cuts[i-1],b=w.cuts[i];
    if(a.segment!=b.segment || a.vertex==b.vertex || a.t==b.t)continue;
    struct lpo_segment s=p->segments[a.segment];
    struct lpo_vertex x=p->vertices[s.a],y=p->vertices[s.b];
    long double dx=(long double)y.x-x.x,dy=(long double)y.y-x.y,t=(a.t+b.t)/2;
    if(!lpo_boundary(&w,a.vertex,b.vertex,x.x+t*dx,x.y+t*dy,dx,dy,s.operand,fa,fb,op))goto done;
  }
  good=lpo_emit_contours(&w,emit,user);
done:
  free(w.order);free(w.boxes);free(w.cuts);free(w.edges);free(w.edge_table);
  return good;
}
#endif
#endif
