/* MPL-2.0. Gregorian/Julian calendar arithmetic and deferred field resolution.
 * Gregorian default cutover: 1582-10-15. Buddhist years share this chronology.
 * Host adapters supply zone offsets and local wall-time resolution only. */
#include "dvm_lib.h"
#include <limits.h>
#include <string.h>
int64_t lc_floor(int64_t n,int64_t d) {int64_t q=n/d;return q-(n%d<0);}
static int64_t mod(int64_t n,int64_t d) {return n-lc_floor(n,d)*d;}
int64_t lc_civil_day(int64_t year,unsigned month,unsigned day,bool gregorian)
{
 int64_t a=(14-(int64_t)month)/12,y=year+4800-a,m=(int64_t)month+12*a-3;
 int64_t jd=(int64_t)day+(153*m+2)/5+365*y+lc_floor(y,4);
 return gregorian ? jd-lc_floor(y,100)+lc_floor(y,400)-32045 : jd-32083;
}
void lc_day_civil(int64_t jd,bool gregorian,int64_t *year,unsigned *month,unsigned *day)
{
 int64_t b=0,c=jd+32082;
 if(gregorian) {int64_t a=jd+32044;b=lc_floor(4*a+3,146097);c=a-lc_floor(146097*b,4);}
 int64_t d=lc_floor(4*c+3,1461),e=c-lc_floor(1461*d,4),m=(5*e+2)/153;
 *day=(unsigned)(e-(153*m+2)/5+1);*month=(unsigned)(m+3-12*(m/10));
 *year=100*b+d-4800+m/10;
}
static bool field_ok(const struct lc_calendar *c,int field)
{return field>=0 && field<(c->icu ? LC_COUNT : 17);}
static int64_t civil(const struct lc_calendar *c,int64_t year,int64_t month,int64_t day)
{
 year+=lc_floor(month,12);month=mod(month,12);
 int64_t g=lc_civil_day(year,(unsigned)month+1,1,true)+day-1;
 return g>=c->cutover ? g : lc_civil_day(year,(unsigned)month+1,1,false)+day-1;
}
static unsigned month_length(const struct lc_calendar *c,int64_t year,int64_t month)
{
 static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
 year+=lc_floor(month,12);month=mod(month,12);
 bool gregorian=lc_civil_day(year,(unsigned)month+1,1,true)>=c->cutover;
 bool leap=mod(year,4)==0 && (!gregorian || mod(year,100)!=0 || mod(year,400)==0);
 return days[month]+(month==1 && leap);
}
static unsigned dow(int64_t day) {return (unsigned)mod(day+1,7)+1;}
static int64_t week_start(const struct lc_calendar *c,int64_t start)
{
 unsigned before=(unsigned)mod((int)dow(start)-(int)c->first_day,7);
 return start-before+(7-before<c->min_days ? 7 : 0);
}
void lc_init(struct lc_calendar *c,bool icu,bool buddhist,unsigned first,unsigned minimal)
{
 memset(c,0,sizeof *c);c->icu=icu;c->buddhist=buddhist;c->lenient=true;c->next_stamp=2;
 c->first_day=first;c->min_days=minimal;c->cutover=2299161;
}
int lc_set_millis(struct lc_calendar *c,int64_t millis)
{
 if(c->icu && (millis<LC_MIN_MS || millis>LC_MAX_MS)) {
  if(!c->lenient) return LC_RANGE_ERROR;
  millis=millis<LC_MIN_MS ? LC_MIN_MS : LC_MAX_MS;
 }
 c->millis=millis;c->time_valid=true;c->fields_valid=false;c->virtual_fields=true;c->next_stamp=2;
 memset(c->fields,0,sizeof c->fields);memset(c->stamps,0,sizeof c->stamps);return LC_OK;
}
static int compute_fields(struct lc_calendar *c,const struct lc_zone *zone)
{
 int32_t raw=0,dst=0;
 if(zone && zone->offset && !zone->offset(zone->context,c->millis,&raw,&dst)) return LC_ZONE_ERROR;
 int64_t day=lc_floor(c->millis,LC_DAY),wall=mod(c->millis,LC_DAY)+(int64_t)raw+dst;
 day+=lc_floor(wall,LC_DAY);wall=mod(wall,LC_DAY);day+=LC_EPOCH_JD;
 int64_t year;unsigned month,date;lc_day_civil(day,day>=c->cutover,&year,&month,&date);
 int64_t jan=civil(c,year,0,1),next=civil(c,year+1,0,1),start=week_start(c,jan),week_year=year;
 if(day<start) {week_year=year-1;start=week_start(c,civil(c,week_year,0,1));}
 else if(day>=week_start(c,next)) {week_year=year+1;start=week_start(c,next);}
 int32_t *f=c->fields;
 f[LC_ERA]=c->buddhist ? 0 : year>=1;f[LC_YEAR]=(int32_t)(c->buddhist ? year+543 : year>=1 ? year : 1-year);
 f[LC_MONTH]=(int32_t)month-1;f[LC_DATE]=(int32_t)date;f[LC_EXTENDED_YEAR]=(int32_t)year;
 f[LC_DAY_OF_YEAR]=(int32_t)(day-jan+1);f[LC_DAY_OF_WEEK]=(int32_t)dow(day);
 f[LC_WEEK_OF_YEAR]=(int32_t)((day-start)/7+1);f[LC_YEAR_WOY]=(int32_t)week_year;
 f[LC_WEEK_OF_MONTH]=(int32_t)(lc_floor(day-week_start(c,civil(c,year,month-1,1)),7)+1);
 f[LC_DAY_OF_WEEK_IN_MONTH]=((int32_t)date-1)/7+1;
 f[LC_DOW_LOCAL]=(int32_t)mod((int)dow(day)-(int)c->first_day,7)+1;
 f[LC_MILLISECONDS_IN_DAY]=(int32_t)wall;f[LC_HOUR_OF_DAY]=(int32_t)(wall/3600000);
 f[LC_HOUR]=f[LC_HOUR_OF_DAY]%12;f[LC_AM_PM]=f[LC_HOUR_OF_DAY]/12;
 f[LC_MINUTE]=(int32_t)(wall/60000%60);f[LC_SECOND]=(int32_t)(wall/1000%60);f[LC_MILLISECOND]=(int32_t)(wall%1000);
 f[LC_ZONE_OFFSET]=raw;f[LC_DST_OFFSET]=dst;f[LC_JULIAN_DAY]=(int32_t)day;f[LC_IS_LEAP_MONTH]=0;
 for(int i=0;i<LC_COUNT;++i) c->stamps[i]=1;
 c->fields_valid=true;c->virtual_fields=false;return LC_OK;
}
static int32_t value(const struct lc_calendar *c,int field,int32_t fallback)
{return c->stamps[field] ? c->fields[field] : fallback;}
static uint32_t newer(uint32_t a,uint32_t b) {return a>b ? a : b;}
static int resolve_date(const struct lc_calendar *c)
{
 /* Day-of-month wins equal stamps. Rows with absent fields are ineligible. */
 static const int rows[][2]={{LC_DATE,-1},{LC_WEEK_OF_YEAR,LC_DAY_OF_WEEK},{LC_WEEK_OF_MONTH,LC_DAY_OF_WEEK},
  {LC_DAY_OF_WEEK_IN_MONTH,LC_DAY_OF_WEEK},{LC_WEEK_OF_YEAR,LC_DOW_LOCAL},{LC_WEEK_OF_MONTH,LC_DOW_LOCAL},
  {LC_DAY_OF_WEEK_IN_MONTH,LC_DOW_LOCAL},{LC_DAY_OF_YEAR,-1},{LC_DATE,LC_YEAR},{LC_WEEK_OF_YEAR,LC_YEAR_WOY}};
 int result=LC_DATE;uint32_t best=0;
 for(unsigned i=0;i<sizeof rows/sizeof rows[0];++i) {
  int a=rows[i][0],b=rows[i][1];uint32_t rank=c->stamps[a];
  if(i==8) rank=c->stamps[LC_YEAR];
  if(i==9) rank=c->stamps[LC_YEAR_WOY];
  if(!rank || (b>=0 && !c->stamps[b])) continue;
  if(b>=0) rank=newer(rank,c->stamps[b]);
  if(rank>best) {best=rank;result=a;}
 }
 if(best) return result;
 static const int fallback[]={LC_WEEK_OF_YEAR,LC_WEEK_OF_MONTH,LC_DAY_OF_WEEK_IN_MONTH,LC_DAY_OF_WEEK,LC_DOW_LOCAL};
 for(unsigned i=0;i<sizeof fallback/sizeof fallback[0];++i) if(c->stamps[fallback[i]]>best) {
  best=c->stamps[fallback[i]];result=i>=3 ? LC_DAY_OF_WEEK_IN_MONTH : fallback[i];
 }
 return result;
}
static int limit_value(struct lc_calendar *c,const struct lc_zone *zone,int field,int kind,int32_t *out);
static int compute_time(struct lc_calendar *c,const struct lc_zone *zone)
{
 if(!c->lenient) for(int i=0;i<(c->icu ? LC_COUNT : 17);++i) if(c->stamps[i]>=2) {
  int32_t low,high;limit_value(c,zone,i,0,&low);limit_value(c,zone,i,1,&high);
  if(c->fields[i]<low || c->fields[i]>high) return LC_RANGE_ERROR;
 }
 int best=resolve_date(c);int64_t year;
 if(c->stamps[LC_EXTENDED_YEAR]>c->stamps[LC_YEAR]) year=value(c,LC_EXTENDED_YEAR,1970);
 else {year=value(c,LC_YEAR,c->buddhist ? 2513 : 1970);if(c->buddhist) year-=543;else if(value(c,LC_ERA,1)==0) year=1-year;}
 if(best==LC_WEEK_OF_YEAR && c->stamps[LC_YEAR_WOY]>c->stamps[LC_YEAR]) year=value(c,LC_YEAR_WOY,1970);
 int64_t month=value(c,LC_MONTH,0),day;
 uint32_t date_stamp=0;
 for(int i=LC_ERA;i<=LC_DAY_OF_WEEK_IN_MONTH;++i) date_stamp=newer(date_stamp,c->stamps[i]);
 for(int i=LC_YEAR_WOY;i<=LC_EXTENDED_YEAR;++i) date_stamp=newer(date_stamp,c->stamps[i]);
 if(c->stamps[LC_JULIAN_DAY]>=2 && c->stamps[LC_JULIAN_DAY]>=date_stamp) day=c->fields[LC_JULIAN_DAY];
 else if(best==LC_DAY_OF_YEAR) day=civil(c,year,0,1)+value(c,LC_DAY_OF_YEAR,1)-1;
 else if(best==LC_DATE) {
  int32_t date=value(c,LC_DATE,1);day=civil(c,year,month,date);
  if(!c->lenient) {
   if(date>(int32_t)month_length(c,year,month)) return LC_RANGE_ERROR;
   int64_t y;unsigned m,d;lc_day_civil(day,day>=c->cutover,&y,&m,&d);
   if(y!=year || (int64_t)m!=month+1 || d!=(unsigned)date) return LC_RANGE_ERROR;
  }
 } else {
  int64_t local_dow=c->stamps[LC_DOW_LOCAL]>c->stamps[LC_DAY_OF_WEEK] ? value(c,LC_DOW_LOCAL,1)-1 :
    mod((int64_t)value(c,LC_DAY_OF_WEEK,(int)c->first_day)-(int)c->first_day,7);
  int64_t start=civil(c,year,best==LC_WEEK_OF_YEAR ? 0 : month,1);
  if(best==LC_DAY_OF_WEEK_IN_MONTH) {
   int64_t ordinal=value(c,LC_DAY_OF_WEEK_IN_MONTH,1);
   if(ordinal>=0) day=start+mod(local_dow-mod((int64_t)dow(start)-(int)c->first_day,7),7)+7*(ordinal-1);
   else {int64_t end=start+month_length(c,year,month)-1;day=end-mod(mod((int64_t)dow(end)-(int)c->first_day,7)-local_dow,7)+7*(ordinal+1);}
  } else day=week_start(c,start)+7*((int64_t)value(c,best,1)-1)+local_dow;
 }
 uint32_t hour_stamp=newer(c->stamps[LC_HOUR],c->stamps[LC_AM_PM]);
 int64_t hour=c->stamps[LC_HOUR_OF_DAY]>=hour_stamp ? value(c,LC_HOUR_OF_DAY,0) : (int64_t)value(c,LC_HOUR,0)+12*(int64_t)value(c,LC_AM_PM,0);
 uint32_t time_stamp=newer(newer(hour_stamp,c->stamps[LC_HOUR_OF_DAY]),newer(c->stamps[LC_MINUTE],newer(c->stamps[LC_SECOND],c->stamps[LC_MILLISECOND])));
 int64_t tod=c->stamps[LC_MILLISECONDS_IN_DAY]>=2 && c->stamps[LC_MILLISECONDS_IN_DAY]>=time_stamp ? c->fields[LC_MILLISECONDS_IN_DAY] :
  ((hour*60+value(c,LC_MINUTE,0))*60+value(c,LC_SECOND,0))*1000+value(c,LC_MILLISECOND,0);
 day+=lc_floor(tod,LC_DAY);tod=mod(tod,LC_DAY);int64_t days=day-LC_EPOCH_JD;
 if(days<INT64_MIN/LC_DAY || days>INT64_MAX/LC_DAY || (days==INT64_MAX/LC_DAY && tod>INT64_MAX%LC_DAY)) return LC_RANGE_ERROR;
 int64_t wall=days*LC_DAY+tod,millis=wall;
 if(c->stamps[LC_ZONE_OFFSET]>=2 || c->stamps[LC_DST_OFFSET]>=2) {
  int64_t off=(int64_t)value(c,LC_ZONE_OFFSET,0)+value(c,LC_DST_OFFSET,0);
  if((off>0 && wall<INT64_MIN+off) || (off<0 && wall>INT64_MAX+off)) return LC_RANGE_ERROR;
  millis=wall-off;
 } else if(zone && zone->resolve && !zone->resolve(zone->context,wall,c->repeated,c->skipped,c->lenient,&millis)) return LC_ZONE_ERROR;
 if(c->icu && (millis<LC_MIN_MS || millis>LC_MAX_MS)) {if(!c->lenient) return LC_RANGE_ERROR;millis=millis<LC_MIN_MS ? LC_MIN_MS : LC_MAX_MS;}
 c->millis=millis;c->time_valid=true;c->virtual_fields=false;return LC_OK;
}
int lc_get_millis(struct lc_calendar *c,const struct lc_zone *zone,int64_t *out)
{int err=c->time_valid ? LC_OK : compute_time(c,zone);if(!err) *out=c->millis;return err;}
static int complete(struct lc_calendar *c,const struct lc_zone *zone)
{int64_t ignored;int err=lc_get_millis(c,zone,&ignored);return err ? err : c->fields_valid ? LC_OK : compute_fields(c,zone);}
int lc_get(struct lc_calendar *c,const struct lc_zone *zone,int field,int32_t *out)
{if(!field_ok(c,field)) return LC_FIELD_ERROR;int err=complete(c,zone);if(!err) *out=c->fields[field];return err;}
int lc_set(struct lc_calendar *c,const struct lc_zone *zone,int field,int32_t val)
{
 if(!field_ok(c,field)) return LC_FIELD_ERROR;
 if(c->virtual_fields) {int err=compute_fields(c,zone);if(err) return err;}
 if(c->next_stamp==UINT32_MAX) {
  /* Preserve relative age, without relying on counter wraparound. */
  uint32_t old[LC_COUNT];memcpy(old,c->stamps,sizeof old);
  for(int i=0;i<LC_COUNT;++i) if(old[i]>=2) {unsigned rank=2;for(int j=0;j<LC_COUNT;++j) if(old[j]>=2 && old[j]<old[i]) ++rank;c->stamps[i]=rank;}
  c->next_stamp=LC_COUNT+2;
 }
 c->fields[field]=val;c->stamps[field]=c->next_stamp++;
 c->time_valid=c->fields_valid=c->virtual_fields=false;return LC_OK;
}
int lc_clear(struct lc_calendar *c,const struct lc_zone *zone,int field)
{
 if(field!=-1 && !field_ok(c,field)) return LC_FIELD_ERROR;
 if(field==-1) {memset(c->fields,0,sizeof c->fields);memset(c->stamps,0,sizeof c->stamps);c->next_stamp=2;}
 else {if(c->virtual_fields) {int err=compute_fields(c,zone);if(err) return err;}c->fields[field]=0;c->stamps[field]=0;}
 c->time_valid=c->fields_valid=c->virtual_fields=false;return LC_OK;
}
bool lc_is_set(const struct lc_calendar *c,int field)
{return field_ok(c,field) && (c->virtual_fields || c->stamps[field]!=0);}
static int set_wall_day(struct lc_calendar *c,const struct lc_zone *zone,int64_t day,int64_t tod)
{
 if(day-LC_EPOCH_JD<INT64_MIN/LC_DAY || day-LC_EPOCH_JD>INT64_MAX/LC_DAY) return LC_RANGE_ERROR;
 int64_t wall=(day-LC_EPOCH_JD)*LC_DAY+tod,millis=wall;
 if(zone && zone->resolve && !zone->resolve(zone->context,wall,c->repeated,c->skipped,c->lenient,&millis)) return LC_ZONE_ERROR;
 return lc_set_millis(c,millis);
}
int lc_add(struct lc_calendar *c,const struct lc_zone *zone,int field,int32_t amount)
{
 if(!field_ok(c,field) || field>=LC_ZONE_OFFSET) return LC_FIELD_ERROR;
 if(!amount) return LC_OK;
 int err=complete(c,zone);if(err) return err;
 int64_t year=c->fields[LC_EXTENDED_YEAR],month=c->fields[LC_MONTH],date=c->fields[LC_DATE],tod=c->fields[LC_MILLISECONDS_IN_DAY];
 if(field==LC_YEAR || field==LC_MONTH) {
  if(field==LC_YEAR) year+=!c->buddhist && c->fields[LC_ERA]==0 ? -(int64_t)amount : amount;
  else month+=amount;
  unsigned max=month_length(c,year,month);if(date>max) date=max;
  return set_wall_day(c,zone,civil(c,year,month,date),tod);
 }
 if(field==LC_DATE || field==LC_DAY_OF_YEAR || field==LC_DAY_OF_WEEK || field==LC_WEEK_OF_YEAR || field==LC_WEEK_OF_MONTH || field==LC_DAY_OF_WEEK_IN_MONTH) {
  int64_t delta=(int64_t)amount*((field==LC_WEEK_OF_YEAR || field==LC_WEEK_OF_MONTH || field==LC_DAY_OF_WEEK_IN_MONTH) ? 7 : 1);
  return set_wall_day(c,zone,civil(c,year,month,date)+delta,tod);
 }
 if(field==LC_ERA) {int64_t era=c->fields[LC_ERA]+(int64_t)amount;era=era<0 ? 0 : era>1 ? 1 : era;
  err=lc_set(c,zone,LC_ERA,(int32_t)era);int64_t ignored;return err ? err : lc_get_millis(c,zone,&ignored);}
 int64_t scale=field==LC_AM_PM ? 43200000 : field==LC_HOUR || field==LC_HOUR_OF_DAY ? 3600000 : field==LC_MINUTE ? 60000 : field==LC_SECOND ? 1000 : 1;
 int64_t delta=(int64_t)amount*scale;
 if((delta>0 && c->millis>INT64_MAX-delta) || (delta<0 && c->millis<INT64_MIN-delta)) return LC_RANGE_ERROR;
 return lc_set_millis(c,c->millis+delta);
}
int lc_roll(struct lc_calendar *c,const struct lc_zone *zone,int field,int32_t amount)
{
 if(!field_ok(c,field)) return LC_FIELD_ERROR;
 if(!amount) return LC_OK;
 int err=complete(c,zone);if(err) return err;
 int64_t year=c->fields[LC_EXTENDED_YEAR],month=c->fields[LC_MONTH],date=c->fields[LC_DATE],tod=c->fields[LC_MILLISECONDS_IN_DAY];
 if(field==LC_DATE || field==LC_DAY_OF_YEAR) {
  int64_t first=civil(c,year,field==LC_DATE ? month : 0,1),last=civil(c,year+(field==LC_DAY_OF_YEAR),field==LC_DATE ? month+1 : 0,1);
  int64_t day=civil(c,year,month,date);return set_wall_day(c,zone,first+mod(day-first+amount,last-first),tod);
 }
 if(field==LC_MONTH) {month=mod(month+amount,12);unsigned max=month_length(c,year,month);if(date>max) date=max;return set_wall_day(c,zone,civil(c,year,month,date),tod);}
 if(field==LC_HOUR || field==LC_HOUR_OF_DAY || field==LC_AM_PM || field==LC_MINUTE || field==LC_SECOND || field==LC_MILLISECOND) {
  int32_t max=field==LC_HOUR ? 12 : field==LC_HOUR_OF_DAY ? 24 : field==LC_AM_PM ? 2 : field==LC_MILLISECOND ? 1000 : 60;
  err=lc_set(c,zone,field,(int32_t)mod((int64_t)c->fields[field]+amount,max));int64_t ignored;return err ? err : lc_get_millis(c,zone,&ignored);
 }
 return LC_UNSUPPORTED;
}
static int limit_value(struct lc_calendar *c,const struct lc_zone *zone,int field,int kind,int32_t *out)
{
 static const int32_t low[LC_COUNT]={0,1,0,1,0,1,1,1,-1,0,0,0,0,0,0,-57600000,-3600000,-5838269,1,-5838269,-2130706432,0,0};
 static const int32_t high[LC_COUNT]={1,5838270,11,53,6,31,366,7,5,1,11,23,59,59,999,108000000,7200000,5828963,7,5828963,2130706432,86399999,0};
 if(kind==0 || kind==2) {*out=low[field];if(c->buddhist && field==LC_ERA) *out=0;return LC_OK;}
 *out=high[field];if(c->buddhist && field==LC_ERA) *out=0;
 if(kind==3) {if(field==LC_WEEK_OF_YEAR) *out=52;if(field==LC_WEEK_OF_MONTH) *out=4;if(field==LC_DATE) *out=28;if(field==LC_DAY_OF_YEAR) *out=365;}
 if(kind>=4) {
  int err=complete(c,zone);if(err) return err;
  if(kind==4) {if(field==LC_WEEK_OF_MONTH) *out=0;else *out=low[field];}
  else {
   int64_t y=c->fields[LC_EXTENDED_YEAR],m=c->fields[LC_MONTH];
   if(field==LC_DATE) *out=(int32_t)month_length(c,y,m);
   if(field==LC_DAY_OF_YEAR) *out=(int32_t)(civil(c,y+1,0,1)-civil(c,y,0,1));
   if(field==LC_WEEK_OF_YEAR) *out=(int32_t)((week_start(c,civil(c,c->fields[LC_YEAR_WOY]+1,0,1))-week_start(c,civil(c,c->fields[LC_YEAR_WOY],0,1)))/7);
  }
 }
 return LC_OK;
}
int lc_limit(struct lc_calendar *c,const struct lc_zone *zone,int field,int kind,int32_t *out)
{if(!field_ok(c,field)) return LC_FIELD_ERROR;return limit_value(c,zone,field,kind,out);}
