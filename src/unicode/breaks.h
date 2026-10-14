/* Unicode 15.0 UAX #14 (numeric-expression tailoring) and UAX #29.
 * Included by luna_unicode.c to share classification data. */
#include "break_data.h"
struct uc_boundary {
   uint32_t cp;
   size_t pos, base, nonspace;
   uint8_t lb, raw, gb, flags;
   unsigned ri;
   bool numeric;
};
static const struct uc_break_range *uc_break_property(uint32_t cp)
{
   if(cp<128) return uc_break_ascii+cp;
   size_t lo=0, hi=COUNT(uc_break_ranges);
   while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(uc_break_ranges[mid].last<cp) lo=mid+1;else hi=mid;}
   return &uc_break_ranges[lo];
}
static bool uc_hard(unsigned lb)
{ return lb==LB_BK || lb==LB_CR || lb==LB_LF || lb==LB_NL; }
static bool uc_alpha(unsigned lb) {return lb==LB_AL || lb==LB_HL;}
static bool uc_hangul(unsigned lb)
{return lb==LB_JL || lb==LB_JV || lb==LB_JT || lb==LB_H2 || lb==LB_H3;}
static bool uc_control(unsigned gb)
{return gb==GB_CR || gb==GB_LF || gb==GB_Control;}
static bool uc_grapheme(const struct uc_boundary *a,const struct uc_boundary *b,unsigned ep_state,unsigned ri)
{
   unsigned l=a->gb,r=b->gb;
   if(l==GB_CR && r==GB_LF) return false; /* GB3 */
   if(uc_control(l) || uc_control(r)) return true; /* GB4/5 */
   if(l==GB_L && (r==GB_L || r==GB_V || r==GB_LV || r==GB_LVT)) return false;
   if((l==GB_LV || l==GB_V) && (r==GB_V || r==GB_T)) return false;
   if((l==GB_LVT || l==GB_T) && r==GB_T) return false;
   if(r==GB_Extend || r==GB_ZWJ || r==GB_SpacingMark || l==GB_Prepend) return false;
   if(ep_state==2 && (b->flags&1)) return false; /* GB11 */
   if(l==GB_Regional_Indicator && r==GB_Regional_Indicator && (ri&1)) return false;
   return true;
}
static unsigned uc_line(const struct uc_boundary *s,size_t i)
{
   const struct uc_boundary *a=s+i-1,*b=s+i;
   unsigned l=a->lb,r=b->lb;
   if(a->raw==LB_CR && b->raw==LB_LF) return 0;
   if(uc_hard(a->raw)) return 5; /* LB4/5 */
   if(uc_hard(b->raw) || r==LB_SP || r==LB_ZW) return 0; /* LB6/7 */
   unsigned ns=s[a->nonspace].lb;
   if(ns==LB_ZW) return 1; /* LB8 */
   if(a->raw==LB_ZWJ) return 0;
   if(b->base!=i) return 0; /* LB9 */
   if(l==LB_WJ || r==LB_WJ || l==LB_GL) return 0;
   if(r==LB_GL && l!=LB_SP && l!=LB_BA && l!=LB_HY) return 0;
   /* LB13, tailored for numeric expressions. */
   if(r==LB_EX || (l!=LB_NU && (r==LB_CL || r==LB_CP || r==LB_IS || r==LB_SY))) return 0;
   if(ns==LB_OP || (ns==LB_QU && r==LB_OP) || ((ns==LB_CL || ns==LB_CP) && r==LB_NS) ||
      (ns==LB_B2 && r==LB_B2)) return 0;
   if(l==LB_SP) return 1;
   if(l==LB_QU || r==LB_QU) return 0;
   if(l==LB_CB || r==LB_CB) return 1;
   if(r==LB_BA || r==LB_HY || r==LB_NS || l==LB_BB) return 0;
   size_t base=a->base;
   if((l==LB_HY || l==LB_BA) && base && s[base-1].lb==LB_HL) return 0;
   if(l==LB_SY && r==LB_HL) return 0;
   if(r==LB_IN) return 0;
   if((uc_alpha(l) && r==LB_NU) || (l==LB_NU && uc_alpha(r))) return 0;
   if((l==LB_PR && (r==LB_ID || r==LB_EB || r==LB_EM)) ||
      ((l==LB_ID || l==LB_EB || l==LB_EM) && r==LB_PO)) return 0;
   if(((l==LB_PR || l==LB_PO) && uc_alpha(r)) || (uc_alpha(l) && (r==LB_PR || r==LB_PO))) return 0;
   /* UAX #14 §8.2 example 7: contextual numbers, not all punctuation pairs. */
   if(l==LB_PR || l==LB_PO) {
      if(r==LB_NU) return 0;
      if(r==LB_OP || r==LB_HY) {
         size_t next=i+1;
         while(s[next].base!=next) ++next;
         if(s[next].lb==LB_NU) return 0;
      }
   }
   if((l==LB_OP || l==LB_HY) && r==LB_NU) return 0;
   if(a->numeric && (r==LB_NU || r==LB_SY || r==LB_IS || r==LB_CL || r==LB_CP || r==LB_PO || r==LB_PR)) return 0;
   if((l==LB_CL || l==LB_CP) && base && s[base-1].numeric && (r==LB_PO || r==LB_PR)) return 0;
   if(l==LB_JL && (r==LB_JL || r==LB_JV || r==LB_H2 || r==LB_H3)) return 0;
   if((l==LB_JV || l==LB_H2) && (r==LB_JV || r==LB_JT)) return 0;
   if((l==LB_JT || l==LB_H3) && r==LB_JT) return 0;
   if((uc_hangul(l) && r==LB_PO) || (l==LB_PR && uc_hangul(r))) return 0;
   if(uc_alpha(l) && uc_alpha(r)) return 0;
   if(l==LB_IS && uc_alpha(r)) return 0;
   if((uc_alpha(l) || l==LB_NU) && r==LB_OP && !(b->flags&2)) return 0;
   if(l==LB_CP && !(s[a->base].flags&2) && (uc_alpha(r) || r==LB_NU)) return 0;
   if(l==LB_RI && r==LB_RI && (a->ri&1)) return 0;
   if(r==LB_EM && (l==LB_EB || ((s[a->base].flags&1) && luna_uc_type((int32_t)s[a->base].cp)==0))) return 0;
   return 1;
}
bool luna_uc_breaks_utf16(const uint16_t *text,size_t length,unsigned char *breaks)
{
   if(!breaks || (!text && length) || length>INT32_MAX || length>(SIZE_MAX/sizeof(struct uc_boundary))-1) return false;
   struct uc_boundary *s=calloc(length+1,sizeof *s);
   if(!s) return false;
   memset(breaks,0,length+1);
   size_t count=0;
   for(size_t pos=0;pos<length;) {
      size_t start=pos;uint32_t cp=text[pos++];
      if(cp>=0xd800 && cp<=0xdbff && pos<length && text[pos]>=0xdc00 && text[pos]<=0xdfff)
         cp=0x10000+((cp-0xd800)<<10)+(text[pos++]-0xdc00);
      const struct uc_break_range *p=uc_break_property(cp);
      struct uc_boundary *b=s+count;
      b->cp=cp;b->pos=start;b->raw=p->lb;b->lb=p->lb;b->gb=p->gb;b->flags=p->flags;b->base=count;
      if(b->lb==LB_AI || b->lb==LB_SG || b->lb==LB_XX) b->lb=LB_AL;
      if(b->lb==LB_CJ) b->lb=LB_NS;
      if(b->lb==LB_SA) {int type=luna_uc_type((int32_t)cp);b->lb=type==6 || type==8 ? LB_CM : LB_AL;}
      if(b->lb==LB_CM || b->lb==LB_ZWJ) {
         if(count && !uc_hard(s[count-1].lb) && s[count-1].lb!=LB_SP && s[count-1].lb!=LB_ZW) {
            b->lb=s[count-1].lb;b->base=s[count-1].base;
         } else b->lb=LB_AL;
      }
      b->nonspace=b->lb==LB_SP && count ? s[count-1].nonspace : count;
      if(b->lb==LB_RI) b->ri=b->base!=count ? s[count-1].ri : (count && s[count-1].lb==LB_RI ? s[count-1].ri+1 : 1);
      b->numeric=b->lb==LB_NU || ((b->lb==LB_SY || b->lb==LB_IS) && count && s[count-1].numeric);
      ++count;
   }
   /* Sentinel supplies bounded numeric lookahead. */
   s[count].lb=LB_XX;s[count].base=count;
   breaks[0]=2;
   unsigned ep_state=0,ri=0;
   for(size_t i=0;i<count;++i) {
      if(i) {if(uc_grapheme(s+i-1,s+i,ep_state,ri)) breaks[s[i].pos]|=2;breaks[s[i].pos]|=(unsigned char)uc_line(s,i);}
      if(s[i].flags&1) ep_state=1;
      else if(s[i].gb==GB_ZWJ && ep_state==1) ep_state=2;
      else if(s[i].gb!=GB_Extend || ep_state!=1) ep_state=0;
      ri=s[i].gb==GB_Regional_Indicator ? ri+1 : 0;
   }
   breaks[length]=7;
   free(s);return true;
}
