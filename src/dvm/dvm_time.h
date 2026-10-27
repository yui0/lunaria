/* MPL-2.0. java.util date/calendar/time-zone, java.text date formatting and java.time.
 * Implementation section included by dvm_runtime.c in declaration order.
 * Shared helpers and class registration remain in that translation unit. */

/* ------------------------------------------------------------------------ *
 * java.util.Date / Calendar / TimeZone, java.text.SimpleDateFormat
 *
 * Calendar.getInstance() answered null through the stub layer, so the very
 * first getTime() on it threw inside a <clinit> — and an exception out of a
 * static initialiser has nowhere to be caught, which took Session.initialize()
 * down with it.  Time is kept as epoch milliseconds and rendered through the
 * host's own calendar routines.
 * ------------------------------------------------------------------------ */

static int64_t now_millis(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t time_millis(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "millis", "J", &v);
   return v.j;
}

static void time_set_millis(struct dvm *vm, dvm_ref self, int64_t ms)
{
   union dvm_value v = { .j = ms };
   (void)dvm_set_field(vm, self, "millis", "J", v);
}

/* The zone a Calendar/DateFormat renders in.  An empty id means the host's
 * local zone, which is what getDefault() reports. */
static bool time_zone_of(struct dvm *vm, dvm_ref self, char *buf, size_t sz)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "zone", "Ljava/lang/String;", &v);
   const char *s = v.l ? dvm_string_utf8(vm, v.l) : NULL;
   snprintf(buf, sz, "%s", s ? s : "");
   return buf[0] != '\0';
}

/* Breaks `ms` down in the named zone.  TZ is swapped around the call because
 * the host's only zone-aware breakdown is localtime_r. */
#ifdef _WIN32
static _Thread_local int32_t time_zone_offset;
static _Thread_local char time_zone_display[128];
#endif
static const char *time_zone_label(const struct tm *tm)
{
#ifdef _WIN32
   (void)tm;
   return time_zone_display[0] ? time_zone_display : "UTC";
#else
   return tm->tm_zone ? tm->tm_zone : "UTC";
#endif
}
static time_t time_utc(struct tm *tm)
{
#ifdef _WIN32
   int64_t ms;
   return luna_calendar_assemble(tm, "UTC", 0, &ms) == 0 ? (time_t)(ms / 1000) : (time_t)-1;
#else
   return timegm(tm);
#endif
}

static void time_break(int64_t ms, const char *zone, struct tm *tm)
{
#ifdef _WIN32
   if (!zone || !*zone) zone = arm_exec_device_timezone();
   if (luna_calendar_break(ms, zone, tm, &time_zone_offset,
                           time_zone_display, sizeof time_zone_display) != 0) {
      if (luna_calendar_break(ms, "UTC", tm, &time_zone_offset,
                              time_zone_display, sizeof time_zone_display) != 0) {
         memset(tm, 0, sizeof *tm);
         time_zone_offset = 0;
         strcpy(time_zone_display, "UTC");
      }
   }
#else

   time_t secs = (time_t)(ms / 1000);
   if (ms < 0 && ms % 1000) --secs;
   if (!zone || !*zone) zone = arm_exec_device_timezone();
   if (!strcmp(zone, "UTC") || !strcmp(zone, "GMT")) {
      gmtime_r(&secs, tm);
      return;
   }
   /* TZ and tzset() are process-global POSIX state.  This emulates a
    * zone-specific conversion API; serialize it with native localtime/mktime
    * so a Java formatter can never expose its temporary zone to bionic. */
   arm_exec_timezone_lock();
   char *old = getenv("TZ");
   char saved[128] = "";
   if (old) snprintf(saved, sizeof saved, "%s", old);
   setenv("TZ", zone, 1);
   tzset();
   localtime_r(&secs, tm);
   if (old) setenv("TZ", saved, 1);
   else unsetenv("TZ");
   tzset();
   arm_exec_timezone_unlock();
#endif
}

/* Seconds east of UTC at `ms`, in the given zone. */
static int32_t time_offset(int64_t ms, const char *zone)
{
   struct tm tm;
   time_break(ms, zone, &tm);
   #ifdef _WIN32
   return time_zone_offset;
#else
   return (int32_t)tm.tm_gmtoff;
#endif
}

/* --- java.util.Date ----------------------------------------------------- */

static bool date_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)out;
   time_set_millis(vm, self, nargs >= 1 ? ARG(0).j : now_millis());
   RETV();
}

static bool date_getTime(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   RETJ(time_millis(vm, self));
}

static bool date_setTime(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs; (void)out;
   time_set_millis(vm, self, ARG(0).j);
   RETV();
}

static bool date_before(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs;
   RETI(ARG(0).l && time_millis(vm, self) < time_millis(vm, ARG(0).l));
}

static bool date_after(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)nargs;
   RETI(ARG(0).l && time_millis(vm, self) > time_millis(vm, ARG(0).l));
}

static bool date_compareTo(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) RETI(0);
   int64_t a = time_millis(vm, self), b = time_millis(vm, ARG(0).l);
   RETI(a < b ? -1 : a > b ? 1 : 0);
}

static bool date_equals(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs;
   RETI(ARG(0).l && time_millis(vm, self) == time_millis(vm, ARG(0).l));
}

static bool date_toString(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   struct tm tm;
   time_break(time_millis(vm, self), "", &tm);
   char buf[64];
   strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Z %Y", &tm);
   RETL(dvm_new_string(vm, buf));
}

/* --- java.util.TimeZone ------------------------------------------------- */

static dvm_ref zone_make(struct dvm *vm, const char *id)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/util/TimeZone;");
   dvm_ref z = c ? dvm_new_object(vm, c) : 0;
   if (z) {
      union dvm_value v = { .l = dvm_new_string(vm, id ? id : "") };
      (void)dvm_set_field(vm, z, "zone", "Ljava/lang/String;", v);
   }
   return z;
}

static bool tz_getDefault(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(zone_make(vm, arm_exec_device_timezone()));
}

static bool tz_getTimeZone(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *id = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   RETL(zone_make(vm, id));
}

/* getID() has to name a real zone even when this one tracks the host's local
 * zone, since callers put it in log payloads. */
static bool tz_getID(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   char zone[128];
   if (time_zone_of(vm, self, zone, sizeof zone)) RETL(dvm_new_string(vm, zone));
   const char *env = getenv("TZ");
   if (env && *env) RETL(dvm_new_string(vm, env));
   struct tm tm;
   time_break(now_millis(), "", &tm);
   RETL(dvm_new_string(vm, time_zone_label(&tm)));
}

static bool tz_getOffset(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   char zone[128];
   (void)time_zone_of(vm, self, zone, sizeof zone);
   int64_t at = nargs >= 1 ? ARG(0).j : now_millis();
   RETI(time_offset(at, zone) * 1000);
}

static bool tz_getRawOffset(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   char zone[128];
   (void)time_zone_of(vm, self, zone, sizeof zone);
   /* January is outside DST in every zone that observes it in the northern
    * hemisphere, and inside it for none in the southern; midwinter UTC is the
    * closest this can get to a raw offset without a zone database. */
   struct tm tm;
   time_break(now_millis(), zone, &tm);
   struct tm jan = tm;
   jan.tm_mon = 0;
   jan.tm_mday = 15;
   jan.tm_isdst = -1;
   RETI(time_offset((int64_t)time_utc(&jan) * 1000, zone) * 1000);
}

static bool tz_inDaylightTime(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs;
   char zone[128];
   (void)time_zone_of(vm, self, zone, sizeof zone);
   struct tm tm;
   time_break(ARG(0).l ? time_millis(vm, ARG(0).l) : now_millis(), zone, &tm);
   RETI(tm.tm_isdst > 0);
}

static bool tz_getDisplayName(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   char zone[128];
   (void)time_zone_of(vm, self, zone, sizeof zone);
   struct tm tm;
   time_break(now_millis(), zone, &tm);
   RETL(dvm_new_string(vm, time_zone_label(&tm)));
}

/* --- java.util.Calendar ------------------------------------------------- */

/* Java Calendar aliases share the C core's field numbers. */
#include "calendar.h"
enum {
   CAL_ERA = LC_ERA, CAL_YEAR = LC_YEAR, CAL_MONTH = LC_MONTH,
   CAL_WEEK_OF_YEAR = LC_WEEK_OF_YEAR, CAL_WEEK_OF_MONTH = LC_WEEK_OF_MONTH,
   CAL_DATE = LC_DATE, CAL_DAY_OF_YEAR = LC_DAY_OF_YEAR,
   CAL_DAY_OF_WEEK = LC_DAY_OF_WEEK, CAL_DAY_OF_WEEK_IN_MONTH = LC_DAY_OF_WEEK_IN_MONTH,
   CAL_AM_PM = LC_AM_PM, CAL_HOUR = LC_HOUR, CAL_HOUR_OF_DAY = LC_HOUR_OF_DAY,
   CAL_MINUTE = LC_MINUTE, CAL_SECOND = LC_SECOND, CAL_MILLISECOND = LC_MILLISECOND,
   CAL_ZONE_OFFSET = LC_ZONE_OFFSET, CAL_DST_OFFSET = LC_DST_OFFSET,
};

/* Android Calendar adapters over the shared C calendar state. */
#include "calendar_data.h"
struct cal_zone_context {char id[128];bool fixed;int32_t offset;};
static bool cal_fixed_id(const char *id,int32_t *offset)
{
 if(!strcmp(id,"UTC") || !strcmp(id,"GMT") || !strcmp(id,"Etc/UTC") || !strcmp(id,"Etc/GMT") || !strcmp(id,"Etc/Unknown")) {*offset=0;return true;}
 if(strncmp(id,"GMT",3) || (id[3]!='+' && id[3]!='-')) return false;
 const char *p=id+4;int h=0,m=0,n=0;
 while(*p>='0' && *p<='9' && n<4) {h=h*10+*p++-'0';++n;}
 if(!n) return false;
 if(*p==':') {++p;if(p[0]<'0' || p[0]>'9' || p[1]<'0' || p[1]>'9') return false;m=(p[0]-'0')*10+p[1]-'0';p+=2;}
 else if(n>2) {m=h%100;h/=100;}
 if(*p || h>23 || m>59) return false;
 *offset=(h*60+m)*60000*(id[3]=='-' ? -1 : 1);return true;
}
static bool cal_host_offset(void *arg,int64_t millis,int32_t *raw,int32_t *dst)
{
 const struct cal_zone_context *z=arg;
 if(z->fixed) {*raw=z->offset;*dst=0;return true;}
 struct tm tm;time_break(millis,z->id,&tm);
#ifdef _WIN32
 int total=time_zone_offset;int32_t west;int daylight;char names[2][32];
 if(luna_calendar_timezone(z->id,millis,&west,&daylight,names)) return false;
 *raw=-west*1000;*dst=total*1000-*raw;
#else
 int64_t total=tm.tm_gmtoff,standard=total;
 if(tm.tm_isdst>0) {
  /* mktime with tm_isdst=0 supplies the standard offset of this rule set;
   * unlike subtracting an hour this also handles half-hour DST. */
  tm.tm_isdst=0;arm_exec_timezone_lock();
  const char *old=getenv("TZ");char *saved=old ? strdup(old) : NULL;
  if(old && !saved) {arm_exec_timezone_unlock();return false;}
  setenv("TZ",z->id,1);tzset();errno=0;time_t standard_time=mktime(&tm);int error=errno;
  if(saved) {setenv("TZ",saved,1);free(saved);} else unsetenv("TZ");tzset();arm_exec_timezone_unlock();
  if(error==EOVERFLOW) return false;
  int64_t delta=(int64_t)standard_time-lc_floor(millis,1000);standard=total-delta;
 }
 if(standard<INT32_MIN/1000 || standard>INT32_MAX/1000 || total-standard<INT32_MIN/1000 || total-standard>INT32_MAX/1000) return false;
 *raw=(int32_t)(standard*1000);*dst=(int32_t)((total-standard)*1000);
#endif
 return true;
}
static bool cal_host_resolve(void *arg,int64_t wall,int repeated,int skipped,bool lenient,int64_t *out)
{
 struct cal_zone_context *z=arg;
 if(z->fixed) {
  if((z->offset>0 && wall<INT64_MIN+z->offset) || (z->offset<0 && wall>INT64_MAX+z->offset)) return false;
  *out=wall-z->offset;return true;
 }
 int64_t candidates[3];unsigned count=0;int32_t offsets[3];
 for(int i=0;i<3;++i) {
  int64_t sample=wall+(i-1)*INT64_C(172800000);int32_t raw,dst;
  if(!cal_host_offset(z,sample,&raw,&dst)) return false;
  offsets[i]=raw+dst;int64_t candidate=wall-offsets[i];
  if(!cal_host_offset(z,candidate,&raw,&dst)) return false;
  if((int64_t)raw+dst==offsets[i]) {
   bool duplicate=false;for(unsigned j=0;j<count;++j) duplicate |= candidates[j]==candidate;
   if(!duplicate) candidates[count++]=candidate;
  }
 }
 if(count) {
  int64_t chosen=candidates[0];for(unsigned i=1;i<count;++i)
   if(repeated ? candidates[i]<chosen : candidates[i]>chosen) chosen=candidates[i];
  *out=chosen;return true;
 }
 if(!lenient) return false;
 int32_t low=offsets[0],high=offsets[0];for(int i=1;i<3;++i) {if(offsets[i]<low) low=offsets[i];if(offsets[i]>high) high=offsets[i];}
 if(skipped!=2) {*out=wall-(skipped ? high : low);return true;}
 /* Locate the transition, rather than preserving minutes inside a gap. */
 int64_t start=wall-high,end=wall-low;
 while(start+1<end) {int64_t mid=start+(end-start)/2;int32_t raw,dst;
  if(!cal_host_offset(z,mid,&raw,&dst)) return false;
  if(mid+(int64_t)raw+dst<wall) start=mid;else end=mid;
 }
 *out=end;return true;
}
static bool cal_zone_context(struct dvm *vm,dvm_ref zone,struct cal_zone_context *z)
{
 memset(z,0,sizeof *z);
 union dvm_value id={0},fixed={0},offset={0};
 if(zone) {
  dvm_get_field(vm,zone,"rules","Ljava/lang/String;",&id);
  if(!id.l) dvm_get_field(vm,zone,"zone","Ljava/lang/String;",&id);
  dvm_get_field(vm,zone,"fixed","Z",&fixed);dvm_get_field(vm,zone,"rawOffset","I",&offset);
 }
 const char *text=id.l ? dvm_string_utf8(vm,id.l) : arm_exec_device_timezone();
 if(!text || strlen(text)>=sizeof z->id) {dvm__throw(vm,"java/lang/IllegalArgumentException","Time zone ID");return false;}
 strcpy(z->id,text);z->fixed=fixed.i!=0;z->offset=offset.i;
 if(!z->fixed) z->fixed=cal_fixed_id(z->id,&z->offset);
 return true;
}
static dvm_ref cal_zone_make(struct dvm *vm,const char *id,bool icu)
{
 int32_t offset=0;bool fixed=cal_fixed_id(id,&offset),known=fixed;
 if(!known && *id && id[0]!='/' && !strstr(id,"..") && strlen(id)<100) {
#ifdef _WIN32
  int32_t west;int daylight;char names[2][32];known=luna_calendar_timezone(id,now_millis(),&west,&daylight,names)==0;
#else
  bool safe=true;for(const unsigned char *p=(const unsigned char *)id;*p;++p)
   safe &= (*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || strchr("_/-+",*p)!=NULL;
  char path[256];snprintf(path,sizeof path,"/usr/share/zoneinfo/%s",id);
  struct stat st;known=safe && stat(path,&st)==0 && S_ISREG(st.st_mode);
#endif
 }
 if(!known) {id=icu ? "Etc/Unknown" : "GMT";offset=0;fixed=true;}
 dvm_ref result=dvm_new_object(vm,dvm_find_class(vm,icu ? fixed ? "android/icu/util/SimpleTimeZone" : "android/icu/impl/OlsonTimeZone" : "java/util/TimeZone"));
 if(!result) return 0;
 dvm_set_field(vm,result,"zone","Ljava/lang/String;",(union dvm_value){.l=dvm_new_string(vm,id)});
 union dvm_value rules={0};dvm_get_field(vm,result,"zone","Ljava/lang/String;",&rules);
 dvm_set_field(vm,result,"rules","Ljava/lang/String;",rules);
 dvm_set_field(vm,result,"fixed","Z",(union dvm_value){.i=fixed});
 dvm_set_field(vm,result,"rawOffset","I",(union dvm_value){.i=offset});return result;
}
static const struct cal_region *cal_locale_region(struct dvm *vm,dvm_ref locale)
{
 const char *country=loc_field_utf8(vm,locale,"country"),*language=loc_field_utf8(vm,locale,"language");
 if(!*country) {
  if(!*language) language="und";
  size_t lo=0,hi=sizeof cal_language_regions/sizeof cal_language_regions[0];
  while(lo<hi) {size_t m=lo+(hi-lo)/2;int cmp=strcmp(language,cal_language_regions[m].language);if(!cmp) {country=cal_language_regions[m].region;break;}if(cmp>0) lo=m+1;else hi=m;}
 }
 for(size_t i=0;i<sizeof cal_regions/sizeof cal_regions[0];++i) if(!strcmp(country,cal_regions[i].region)) return cal_regions+i;
 return cal_regions; /* sorted first entry is the world default, 001 */
}
static bool cal_icu(struct dvm *vm,dvm_ref self)
{const struct dvm_class *c=dvm_object_class(vm,self);return c && c->name && !strncmp(c->name,"android/icu/",12);}
static bool cal_error(struct dvm *vm,int error)
{
 if(!error) return true;
 dvm__throw(vm,error==LC_FIELD_ERROR ? "java/lang/ArrayIndexOutOfBoundsException" : error==LC_UNSUPPORTED ? "java/lang/UnsupportedOperationException" : "java/lang/IllegalArgumentException",
            error==LC_UNSUPPORTED ? "Calendar field operation not implemented" : "Invalid calendar field or time");return false;
}
static struct lc_calendar *cal_state(struct dvm *vm,dvm_ref self,struct cal_zone_context *context,struct lc_zone *zone)
{
 struct dvm_object *o=dvm__obj(vm,self);
 if(!o || !o->data) {dvm__throw(vm,"java/lang/IllegalStateException","Calendar not initialized");return NULL;}
 union dvm_value tz={0};dvm_get_field(vm,self,"timeZone","Ljava/lang/Object;",&tz);
 if(!cal_zone_context(vm,tz.l,context)) return NULL;
 *zone=(struct lc_zone){context,cal_host_offset,cal_host_resolve};return o->data;
}
static bool cal_initialize(struct dvm *vm,dvm_ref self,dvm_ref tz,dvm_ref locale,bool buddhist)
{
 if(!locale) {dvm__throw(vm,"java/lang/NullPointerException","locale");return false;}
 bool icu=cal_icu(vm,self);if(!tz) tz=cal_zone_make(vm,arm_exec_device_timezone(),icu);
 if(!tz) return false;
 const struct cal_region *region=cal_locale_region(vm,locale);
 struct lc_calendar *state=calloc(1,sizeof *state);
 if(!state) {dvm__throw(vm,"java/lang/OutOfMemoryError","Calendar state");return false;}
 lc_init(state,icu,buddhist,region->first,region->minimal);lc_set_millis(state,now_millis());
 struct dvm_object *o=dvm__obj(vm,self);if(o->data_destroy) o->data_destroy(o->data);else free(o->data);
 o->data=state;o->data_destroy=free;
 dvm_set_field(vm,self,"timeZone","Ljava/lang/Object;",(union dvm_value){.l=tz});return true;
}
static bool cal_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 const struct dvm_method *method=dvm__builtin_method();dvm_ref tz=0,locale=0;
 union dvm_value def={0};if(!loc_getDefault(vm,0,NULL,0,&def)) return false;locale=def.l;
 bool date_ctor=method && nargs>=3 && method->sig[1]=='I';
 if(nargs && !date_ctor) {
  bool zone_arg=method && strstr(method->sig,"TimeZone;");
  if(!args[0].l || (nargs>1 && !args[1].l)) {dvm__throw(vm,"java/lang/NullPointerException",zone_arg ? "zone" : "locale");return false;}
  if(zone_arg) {tz=args[0].l;if(nargs>1) locale=args[1].l;} else locale=args[0].l;
 }
 const struct dvm_class *cls=dvm_object_class(vm,self);bool buddhist=cls && cls->name && strstr(cls->name,"BuddhistCalendar");
 if(!cal_initialize(vm,self,tz,locale,buddhist)) return false;
 if(date_ctor) {
  struct cal_zone_context context;struct lc_zone zone;struct lc_calendar *c=cal_state(vm,self,&context,&zone);if(!c) return false;
  lc_clear(c,&zone,-1);const int fields[]={LC_YEAR,LC_MONTH,LC_DATE,LC_HOUR_OF_DAY,LC_MINUTE,LC_SECOND};
  for(int i=0;i<nargs;++i) if(!cal_error(vm,lc_set(c,&zone,fields[i],args[i].i))) return false;
 }
 RETV();
}
static bool cal_getInstance(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)self;const struct dvm_method *method=dvm__builtin_method();
 bool icu=method && method->cls && !strncmp(method->cls->name,"android/icu/",12);
 dvm_ref tz=0,locale=0;union dvm_value def={0};if(!loc_getDefault(vm,0,NULL,0,&def)) return false;locale=def.l;
 if(nargs) {if(!args[0].l || (nargs>1 && !args[1].l)) {dvm__throw(vm,"java/lang/NullPointerException","zone or locale");return false;}
  if(method && strstr(method->sig,"TimeZone;")) {tz=args[0].l;if(nargs>1) locale=args[1].l;}else locale=args[0].l;}
 const struct cal_region *region=cal_locale_region(vm,locale);bool buddhist=!strcmp(region->type,"buddhist");
 if(icu && strcmp(region->type,"gregorian") && !buddhist) {dvm__throw(vm,"java/lang/UnsupportedOperationException",region->type);return false;}
 const char *name=icu ? buddhist ? "android/icu/util/BuddhistCalendar" : "android/icu/util/GregorianCalendar" : "java/util/GregorianCalendar";
 dvm_ref result=dvm_new_object(vm,dvm_find_class(vm,name));if(!result || !cal_initialize(vm,result,tz,locale,icu && buddhist)) return false;
 RETL(result);
}
#define CAL_STATE() struct cal_zone_context context;struct lc_zone zone;struct lc_calendar *c=cal_state(vm,self,&context,&zone);if(!c) return false
static bool cal_getTimeInMillis(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;CAL_STATE();int64_t millis;if(!cal_error(vm,lc_get_millis(c,&zone,&millis))) return false;RETJ(millis);}
static bool cal_setTimeInMillis(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;CAL_STATE();if(!cal_error(vm,lc_set_millis(c,ARG(0).j))) return false;RETV();}
static bool cal_getTime(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{union dvm_value value={0};if(!cal_getTimeInMillis(vm,self,args,nargs,&value)) return false;dvm_ref date=dvm_new_object(vm,dvm_find_class(vm,"java/util/Date"));if(date) time_set_millis(vm,date,value.j);RETL(date);}
static bool cal_setTime(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;if(!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","date");return false;}union dvm_value value={.j=time_millis(vm,ARG(0).l)};return cal_setTimeInMillis(vm,self,&value,1,out);}
static bool cal_get(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 (void)nargs;struct dvm_object *object=dvm__obj(vm,self);struct lc_calendar *cached=object ? object->data : NULL;
 if(cached && cached->fields_valid) {
  int field=ARG(0).i;
  if(field<0 || field>=(cached->icu ? LC_COUNT : 17)) return cal_error(vm,LC_FIELD_ERROR);
  RETI(cached->fields[field]);
 }
 CAL_STATE();int32_t value;if(!cal_error(vm,lc_get(c,&zone,ARG(0).i,&value))) return false;RETI(value);
}
static bool cal_set(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{CAL_STATE();if(nargs==2) {if(!cal_error(vm,lc_set(c,&zone,ARG(0).i,ARG(1).i))) return false;}
 else {const int fields[]={LC_YEAR,LC_MONTH,LC_DATE,LC_HOUR_OF_DAY,LC_MINUTE,LC_SECOND};for(int i=0;i<nargs && i<6;++i) if(!cal_error(vm,lc_set(c,&zone,fields[i],args[i].i))) return false;}RETV();}
static bool cal_add(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;CAL_STATE();if(!cal_error(vm,lc_add(c,&zone,ARG(0).i,ARG(1).i))) return false;RETV();}
static bool cal_roll(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;CAL_STATE();const struct dvm_method *m=dvm__builtin_method();int32_t amount=m && !strcmp(m->sig,"(IZ)V") ? ARG(1).i ? 1 : -1 : ARG(1).i;if(!cal_error(vm,lc_roll(c,&zone,ARG(0).i,amount))) return false;RETV();}
static bool cal_clear(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{CAL_STATE();if(!cal_error(vm,lc_clear(c,&zone,nargs ? ARG(0).i : -1))) return false;RETV();}
static bool cal_isSet(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;CAL_STATE();if(ARG(0).i<0 || ARG(0).i>=(c->icu ? LC_COUNT : 17)) return cal_error(vm,LC_FIELD_ERROR);RETI(lc_is_set(c,ARG(0).i));}
static bool cal_attribute(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 CAL_STATE();const struct dvm_method *m=dvm__builtin_method();bool set=m->name[0]=='s';int32_t value=nargs ? args[0].i : 0;
 if(strstr(m->name,"Lenient")) {if(set) c->lenient=value!=0;else RETI(c->lenient);}
 else if(strstr(m->name,"WallTime")) {bool repeated=strstr(m->name,"Repeated")!=NULL;
  if(set) {if(value<0 || value>(repeated ? 1 : 2)) return cal_error(vm,LC_RANGE_ERROR);if(repeated) c->repeated=value;else c->skipped=value;}else RETI(repeated ? c->repeated : c->skipped);}
 else {bool first=strstr(m->name,"FirstDay")!=NULL;
  if(set) {if(value<1 || value>7) return cal_error(vm,LC_RANGE_ERROR);if(first) c->first_day=(unsigned)value;else c->min_days=(unsigned)value;c->fields_valid=false;}
  else RETI(first ? c->first_day : c->min_days);}
 RETV();
}
static bool cal_limit(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;CAL_STATE();const struct dvm_method *m=dvm__builtin_method();const char *names[]={"getMinimum","getMaximum","getGreatestMinimum","getLeastMaximum","getActualMinimum","getActualMaximum"};int kind=0;while(kind<6 && strcmp(m->name,names[kind])) ++kind;int32_t value;if(!cal_error(vm,lc_limit(c,&zone,ARG(0).i,kind,&value))) return false;RETI(value);}
static bool cal_getTimeZone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;union dvm_value tz={0};dvm_get_field(vm,self,"timeZone","Ljava/lang/Object;",&tz);if(cal_icu(vm,self)) RETL(tz.l);return o_clone(vm,tz.l,NULL,0,out);}
static bool cal_setTimeZone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;if(!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","zone");return false;}CAL_STATE();c->fields_valid=false;dvm_set_field(vm,self,"timeZone","Ljava/lang/Object;",ARG(0));RETV();}
static bool cal_clone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
 CAL_STATE();union dvm_value clone={0};if(!o_clone(vm,self,args,nargs,&clone) || !clone.l) return false;
 struct lc_calendar *copy=malloc(sizeof *copy);if(!copy) {dvm__throw(vm,"java/lang/OutOfMemoryError","Calendar clone");return false;}*copy=*c;
 struct dvm_object *o=dvm__obj(vm,clone.l);o->data=copy;o->data_destroy=free;
 union dvm_value tz={0},newtz={0};dvm_get_field(vm,self,"timeZone","Ljava/lang/Object;",&tz);
 if(!o_clone(vm,tz.l,NULL,0,&newtz)) return false;
 dvm_set_field(vm,clone.l,"timeZone","Ljava/lang/Object;",newtz);RETL(clone.l);
}
static bool cal_type(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;CAL_STATE();RETL(dvm_new_string(vm,c->buddhist ? "buddhist" : "gregorian"));}
static bool cal_tz_factory(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)self;const char *id=nargs ? dvm_string_utf8(vm,ARG(0).l) : arm_exec_device_timezone();if(!id) {dvm__throw(vm,"java/lang/NullPointerException","time zone ID");return false;}RETL(cal_zone_make(vm,id,true));}
static bool cal_tz_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;if(!ARG(1).l) {dvm__throw(vm,"java/lang/NullPointerException","time zone ID");return false;}dvm_set_field(vm,self,"zone","Ljava/lang/String;",ARG(1));dvm_set_field(vm,self,"fixed","Z",(union dvm_value){.i=1});dvm_set_field(vm,self,"rawOffset","I",ARG(0));RETV();}
static bool cal_tz_offset(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{struct cal_zone_context z;if(!cal_zone_context(vm,self,&z)) return false;int32_t raw,dst;int64_t at=nargs ? ARG(0).j : now_millis();if(!cal_host_offset(&z,at,&raw,&dst)) return cal_error(vm,LC_ZONE_ERROR);RETI(nargs ? raw+dst : raw);}
static bool cal_tz_setID(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)nargs;if(!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","time zone ID");return false;}dvm_set_field(vm,self,"zone","Ljava/lang/String;",ARG(0));RETV();}
#undef CAL_STATE
static const struct rt_field rt_cal_fields[]={{"timeZone","Ljava/lang/Object;"},F_END};
static const struct rt_field rt_cal_tz_fields[]={{"zone","Ljava/lang/String;"},{"rules","Ljava/lang/String;"},{"fixed","Z"},{"rawOffset","I"},F_END};
static const struct rt_method rt_icu_timezone[]={
 SM("getDefault","()Landroid/icu/util/TimeZone;",cal_tz_factory),SM("getTimeZone","(Ljava/lang/String;)Landroid/icu/util/TimeZone;",cal_tz_factory),
 M("getID","()Ljava/lang/String;",tz_getID),M("setID","(Ljava/lang/String;)V",cal_tz_setID),M("getOffset","(J)I",cal_tz_offset),
 M("getRawOffset","()I",cal_tz_offset),M("clone","()Ljava/lang/Object;",o_clone),M_END};
static const struct rt_method rt_icu_simple_timezone[]={M("<init>","(ILjava/lang/String;)V",cal_tz_init),M_END};
#define CAL_COMMON_METHODS \
 M("getTime","()Ljava/util/Date;",cal_getTime),M("setTime","(Ljava/util/Date;)V",cal_setTime), \
 M("getTimeInMillis","()J",cal_getTimeInMillis),M("setTimeInMillis","(J)V",cal_setTimeInMillis), \
 M("get","(I)I",cal_get),M("set","(II)V",cal_set),M("set","(III)V",cal_set), \
 M("set","(IIIII)V",cal_set),M("set","(IIIIII)V",cal_set),M("add","(II)V",cal_add), \
 M("roll","(II)V",cal_roll),M("roll","(IZ)V",cal_roll),M("clear","()V",cal_clear),M("clear","(I)V",cal_clear), \
 M("isSet","(I)Z",cal_isSet),M("isLenient","()Z",cal_attribute),M("setLenient","(Z)V",cal_attribute), \
 M("getFirstDayOfWeek","()I",cal_attribute),M("setFirstDayOfWeek","(I)V",cal_attribute), \
 M("getMinimalDaysInFirstWeek","()I",cal_attribute),M("setMinimalDaysInFirstWeek","(I)V",cal_attribute), \
 M("getMinimum","(I)I",cal_limit),M("getMaximum","(I)I",cal_limit),M("getGreatestMinimum","(I)I",cal_limit), \
 M("getLeastMaximum","(I)I",cal_limit),M("getActualMinimum","(I)I",cal_limit),M("getActualMaximum","(I)I",cal_limit), \
 M("clone","()Ljava/lang/Object;",cal_clone)
static const struct rt_method rt_calendar[]={
 SM("getInstance","()Ljava/util/Calendar;",cal_getInstance),SM("getInstance","(Ljava/util/Locale;)Ljava/util/Calendar;",cal_getInstance),
 SM("getInstance","(Ljava/util/TimeZone;)Ljava/util/Calendar;",cal_getInstance),SM("getInstance","(Ljava/util/TimeZone;Ljava/util/Locale;)Ljava/util/Calendar;",cal_getInstance),
 M("getTimeZone","()Ljava/util/TimeZone;",cal_getTimeZone),M("setTimeZone","(Ljava/util/TimeZone;)V",cal_setTimeZone),CAL_COMMON_METHODS,M_END};
static const struct rt_method rt_icu_calendar[]={
 SM("getInstance","()Landroid/icu/util/Calendar;",cal_getInstance),SM("getInstance","(Ljava/util/Locale;)Landroid/icu/util/Calendar;",cal_getInstance),
 SM("getInstance","(Landroid/icu/util/TimeZone;)Landroid/icu/util/Calendar;",cal_getInstance),SM("getInstance","(Landroid/icu/util/TimeZone;Ljava/util/Locale;)Landroid/icu/util/Calendar;",cal_getInstance),
 M("getTimeZone","()Landroid/icu/util/TimeZone;",cal_getTimeZone),M("setTimeZone","(Landroid/icu/util/TimeZone;)V",cal_setTimeZone),
 M("getType","()Ljava/lang/String;",cal_type),M("setRepeatedWallTimeOption","(I)V",cal_attribute),M("getRepeatedWallTimeOption","()I",cal_attribute),
 M("setSkippedWallTimeOption","(I)V",cal_attribute),M("getSkippedWallTimeOption","()I",cal_attribute),CAL_COMMON_METHODS,M_END};
#undef CAL_COMMON_METHODS
static const struct rt_method rt_gregorian_calendar[]={
 M("<init>","()V",cal_init),M("<init>","(Ljava/util/Locale;)V",cal_init),M("<init>","(Ljava/util/TimeZone;)V",cal_init),
 M("<init>","(Ljava/util/TimeZone;Ljava/util/Locale;)V",cal_init),M("<init>","(III)V",cal_init),M("<init>","(IIIII)V",cal_init),M("<init>","(IIIIII)V",cal_init),M_END};
static const struct rt_method rt_icu_gregorian_calendar[]={
 M("<init>","()V",cal_init),M("<init>","(Ljava/util/Locale;)V",cal_init),M("<init>","(Landroid/icu/util/TimeZone;)V",cal_init),
 M("<init>","(Landroid/icu/util/TimeZone;Ljava/util/Locale;)V",cal_init),M("<init>","(III)V",cal_init),M("<init>","(IIIII)V",cal_init),M("<init>","(IIIIII)V",cal_init),M_END};

/* java.text.DateFormatSymbols: immutable CLDR data, mutable per-object copies. */
#include "dvm/date_symbols_data.h"
static const struct { const char *field, *get, *set; unsigned start, count; } dfs_arrays[] = {
   {"eras","getEras","setEras",0,2},
   {"months","getMonths","setMonths",2,13},
   {"shortMonths","getShortMonths","setShortMonths",15,13},
   {"weekdays","getWeekdays","setWeekdays",28,8},
   {"shortWeekdays","getShortWeekdays","setShortWeekdays",36,8},
   {"ampms","getAmPmStrings","setAmPmStrings",44,2}
};
static unsigned dfs_locale_row(char *locale)
{
   for (;;) {
      size_t lo = 0, hi = sizeof(date_symbol_locales)/sizeof(date_symbol_locales[0]);
      while (lo < hi) {
         size_t mid = lo + (hi-lo)/2;
         int cmp = strcmp(locale, date_symbol_locales[mid].locale);
         if (!cmp) return date_symbol_locales[mid].row;
         if (cmp < 0) hi = mid; else lo = mid+1;
      }
      char *end = strrchr(locale, '_');
      if (end) *end = 0;
      else if (strcmp(locale, "root")) strcpy(locale, "root");
      else return 0;
   }
}
static dvm_ref dfs_copy_array(struct dvm *vm, dvm_ref source)
{
   if (!source) {
      dvm__throw(vm, "java/lang/NullPointerException", "symbols");
      return 0;
   }
   struct dvm_object *a = dvm__obj(vm, source);
   if (!a || a->kind != DVM_OBJ_ARRAY) return 0;
   uint32_t length = a->length;
   dvm_ref copy = dvm_new_array(vm, 'L', "Ljava/lang/String;", length);
   a = dvm__obj(vm, source);
   struct dvm_object *b = copy ? dvm__obj(vm, copy) : NULL;
   if (!b) return 0;
   if (a->length) memcpy(b->data, a->data, a->length * sizeof(dvm_ref));
   return copy;
}
static bool dfs_init(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)out;
   union dvm_value locale = {0}, name = {0};
   if (nargs) locale = args[0];
   else if (!loc_getDefault(vm, 0, NULL, 0, &locale)) return false;
   if (!locale.l) {
      dvm__throw(vm, "java/lang/NullPointerException", "locale");
      return false;
   }
   if (!loc_toString(vm, locale.l, NULL, 0, &name)) return false;
   const char *text = dvm_string_utf8(vm, name.l);
   char id[256]; snprintf(id, sizeof id, "%s", text && *text ? text : "root");
   unsigned row = dfs_locale_row(id);
   for (unsigned k = 0; k < sizeof dfs_arrays/sizeof dfs_arrays[0]; ++k) {
      dvm_ref array = dvm_new_array(vm, 'L', "Ljava/lang/String;", dfs_arrays[k].count);
      if (!array) return false;
      dvm_set_field(vm, self, dfs_arrays[k].field, "[Ljava/lang/String;", (union dvm_value){.l=array});
      for (unsigned i = 0; i < dfs_arrays[k].count; ++i) {
         dvm_ref value = dvm_new_string(vm, date_symbol_rows[row][dfs_arrays[k].start+i]);
         ((dvm_ref *)dvm__obj(vm, array)->data)[i] = value;
      }
   }
   dvm_set_field(vm, self, "localPatternChars", "Ljava/lang/String;",
      (union dvm_value){.l=dvm_new_string(vm,"GyMdkHmsSEDFwWahKzZ")});
   RETV();
}
static bool dfs_array(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)nargs;
   const struct dvm_method *m = dvm__builtin_method();
   for (unsigned k = 0; m && k < sizeof dfs_arrays/sizeof dfs_arrays[0]; ++k) {
      bool set = !strcmp(m->name, dfs_arrays[k].set);
      if (!set && strcmp(m->name, dfs_arrays[k].get)) continue;
      union dvm_value v = {0};
      if (set) v = args[0];
      else dvm_get_field(vm, self, dfs_arrays[k].field, "[Ljava/lang/String;", &v);
      dvm_ref copy = dfs_copy_array(vm, v.l);
      if (!copy) return false;
      if (!set) RETL(copy);
      dvm_set_field(vm, self, dfs_arrays[k].field, "[Ljava/lang/String;", (union dvm_value){.l=copy});
      RETV();
   }
   return false;
}
static bool dfs_pattern(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs, union dvm_value *out)
{
   if (nargs) {
      if (!args[0].l) {
         dvm__throw(vm,"java/lang/NullPointerException","pattern characters");
         return false;
      }
      dvm_set_field(vm,self,"localPatternChars","Ljava/lang/String;",args[0]);
      RETV();
   }
   union dvm_value v = {0};
   dvm_get_field(vm,self,"localPatternChars","Ljava/lang/String;",&v);
   RETL(v.l);
}
static bool dfs_instance(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)self;
   dvm_ref object = dvm_new_object(vm,dvm_find_class(vm,"java/text/DateFormatSymbols"));
   if (!object) return false;
   dvm_pin(vm,object);
   union dvm_value ignored = {0};
   bool ok = dfs_init(vm,object,args,nargs,&ignored);
   dvm_unpin(vm,object);
   if (!ok) return false;
   RETL(object);
}
static bool dfs_clone(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value cloned = {0};
   if (!o_clone(vm,self,NULL,0,&cloned) || !cloned.l) return false;
   dvm_ref copy = cloned.l;
   dvm_pin(vm,copy);
   bool ok = true;
   for (unsigned k = 0; ok && k < sizeof dfs_arrays/sizeof dfs_arrays[0]; ++k) {
      union dvm_value v = {0};
      dvm_get_field(vm,self,dfs_arrays[k].field,"[Ljava/lang/String;",&v);
      v.l = dfs_copy_array(vm,v.l);
      ok = v.l != 0;
      if (ok) dvm_set_field(vm,copy,dfs_arrays[k].field,"[Ljava/lang/String;",v);
   }
   union dvm_value v = {0};
   dvm_get_field(vm,self,"localPatternChars","Ljava/lang/String;",&v);
   dvm_set_field(vm,copy,"localPatternChars","Ljava/lang/String;",v);
   dvm_unpin(vm,copy);
   if (!ok) return false;
   RETL(copy);
}
static const struct rt_field rt_dfs_fields[] = {
   {"eras","[Ljava/lang/String;"},{"months","[Ljava/lang/String;"},
   {"shortMonths","[Ljava/lang/String;"},{"weekdays","[Ljava/lang/String;"},
   {"shortWeekdays","[Ljava/lang/String;"},{"ampms","[Ljava/lang/String;"},
   {"localPatternChars","Ljava/lang/String;"},F_END
};
#define DFS_PAIR(name) M("get" name,"()[Ljava/lang/String;",dfs_array), M("set" name,"([Ljava/lang/String;)V",dfs_array)
static const struct rt_method rt_dfs[] = {
   M("<init>","()V",dfs_init), M("<init>","(Ljava/util/Locale;)V",dfs_init),
   SM("getInstance","()Ljava/text/DateFormatSymbols;",dfs_instance),
   SM("getInstance","(Ljava/util/Locale;)Ljava/text/DateFormatSymbols;",dfs_instance),
   DFS_PAIR("Eras"),DFS_PAIR("Months"),DFS_PAIR("ShortMonths"),
   DFS_PAIR("Weekdays"),DFS_PAIR("ShortWeekdays"),DFS_PAIR("AmPmStrings"),
   M("getLocalPatternChars","()Ljava/lang/String;",dfs_pattern),
   M("setLocalPatternChars","(Ljava/lang/String;)V",dfs_pattern),
   M("clone","()Ljava/lang/Object;",dfs_clone), M_END
};
#undef DFS_PAIR
static bool format_object_string(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static const struct rt_method rt_format[] = {
   M("<init>","()V",o_init),M("format","(Ljava/lang/Object;)Ljava/lang/String;",format_object_string),
   M("clone","()Ljava/lang/Object;",o_clone),M_END
};

/* --- java.text.SimpleDateFormat ----------------------------------------- */

static const char *const kMonths[] = { "January", "February", "March", "April",
   "May", "June", "July", "August", "September", "October", "November",
   "December" };
static const char *const kDays[] = { "Sunday", "Monday", "Tuesday",
   "Wednesday", "Thursday", "Friday", "Saturday" };

static unsigned date_pattern_row(char *locale)
{
   for (;;) {
      size_t lo=0,hi=sizeof date_pattern_locales/sizeof *date_pattern_locales;
      while (lo<hi) {
         size_t mid=lo+(hi-lo)/2;int cmp=strcmp(locale,date_pattern_locales[mid].locale);
         if (!cmp) return date_pattern_locales[mid].row;
         if (cmp<0) hi=mid;else lo=mid+1;
      }
      char *end=strrchr(locale,'_');
      if (end) *end=0;
      else if (strcmp(locale,"root")) strcpy(locale,"root");
      else return 0;
   }
}
static bool date_locale_name(struct dvm *vm,dvm_ref locale,char *name,size_t size)
{
   union dvm_value text={0};
   if (!locale) {dvm__throw(vm,"java/lang/NullPointerException","locale");return false;}
   if (!loc_toString(vm,locale,NULL,0,&text)) return false;
   const char *value=dvm_string_utf8(vm,text.l);
   snprintf(name,size,"%s",value && *value ? value : "root");return true;
}

static bool sdf_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)out;
   if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","pattern");return false;}
   union dvm_value locale={0};
   if (nargs>1) locale=args[1];
   else if (!loc_getDefault(vm,0,NULL,0,&locale)) return false;
   dvm_ref calendar=dvm_new_object(vm,dvm_find_class(vm,"java/util/GregorianCalendar"));
   if (!calendar || !cal_initialize(vm,calendar,0,locale.l,false)) return false;
   dvm_set_field(vm,self,"calendar","Ljava/util/Calendar;",(union dvm_value){.l=calendar});
   dvm_set_field(vm,self,"locale","Ljava/util/Locale;",locale);
   union dvm_value v = ARG(0);
   (void)dvm_set_field(vm, self, "pattern", "Ljava/lang/String;", v);
   RETV();
}

static bool sdf_applyPattern(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","pattern");return false;}
   dvm_set_field(vm,self,"pattern","Ljava/lang/String;",ARG(0));
   RETV();
}

/* Formats one run of a repeated pattern letter. */
static void sdf_field(struct rt_bytes *b, char c, int n, const struct tm *tm,
                      int32_t sub_ms, int32_t off_secs)
{
   char buf[64];
   /* A run longer than the widest field is a malformed pattern; clamping keeps
    * the zero padding from running off the end of buf. */
   if (n > 16) n = 16;
   switch (c) {
   case 'y': {
      int y = tm->tm_year + 1900;
      if (n == 2) snprintf(buf, sizeof buf, "%02d", y % 100);
      else snprintf(buf, sizeof buf, "%0*d", n, y);
      break;
   }
   case 'M':
      if (n >= 4) snprintf(buf, sizeof buf, "%s", kMonths[tm->tm_mon % 12]);
      else if (n == 3) snprintf(buf, sizeof buf, "%.3s", kMonths[tm->tm_mon % 12]);
      else snprintf(buf, sizeof buf, "%0*d", n, tm->tm_mon + 1);
      break;
   case 'd': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_mday); break;
   case 'D': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_yday + 1); break;
   case 'H': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_hour); break;
   case 'k': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_hour ? tm->tm_hour : 24); break;
   case 'h': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
   case 'K': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_hour % 12); break;
   case 'm': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_min); break;
   case 's': snprintf(buf, sizeof buf, "%0*d", n, tm->tm_sec); break;
   case 'S': snprintf(buf, sizeof buf, "%0*d", n, sub_ms); break;
   case 'a': snprintf(buf, sizeof buf, "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
   case 'E':
      if (n >= 4) snprintf(buf, sizeof buf, "%s", kDays[tm->tm_wday % 7]);
      else snprintf(buf, sizeof buf, "%.3s", kDays[tm->tm_wday % 7]);
      break;
   case 'z':
      snprintf(buf, sizeof buf, "%s", time_zone_label(tm));
      break;
   case 'Z': {
      int m = off_secs / 60;
      snprintf(buf, sizeof buf, "%c%02d%02d", m < 0 ? '-' : '+',
               abs(m) / 60, abs(m) % 60);
      break;
   }
   case 'X': {
      int m = off_secs / 60;
      if (!m && n < 3) snprintf(buf, sizeof buf, "Z");
      else if (n >= 3) snprintf(buf, sizeof buf, "%c%02d:%02d",
                                m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60);
      else snprintf(buf, sizeof buf, "%c%02d%02d", m < 0 ? '-' : '+',
                    abs(m) / 60, abs(m) % 60);
      break;
   }
   case 'G': snprintf(buf, sizeof buf, "AD"); break;
   default:
      /* An unmodelled letter renders as itself, repeated, so the output stays
       * the length the caller laid out. */
      for (int i = 0; i < n && i < (int)sizeof buf - 1; ++i) buf[i] = c;
      buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = '\0';
      break;
   }
   bytes_append(b, buf, strlen(buf));
}

/* Renders a broken-down time through a date/time pattern.  java.time formats
 * the same letters as SimpleDateFormat does, so both go through here. */
static void time_render(struct rt_bytes *b, const char *pat,
                        const struct tm *tm, int32_t sub_ms, int32_t off_secs,
                        const char *const *symbols,unsigned zero,int era)
{
   for (const char *p = pat; p && *p;) {
      if (*p == '\'') {           /* quoted literal; '' is one quote */
         ++p;
         if (*p == '\'') { bytes_append(b, "'", 1); ++p; continue; }
         while (*p && *p != '\'') { bytes_append(b, p, 1); ++p; }
         if (*p) ++p;
         continue;
      }
      if (!isalpha((unsigned char)*p)) { bytes_append(b, p, 1); ++p; continue; }
      char c = *p;
      int n = 0;
      while (p[n] == c) ++n;
      const char *word=NULL;
      if ((c=='M' || c=='L') && n>=3) word=symbols[(n>=4 ? 2 : 15)+tm->tm_mon];
      else if (c=='E') word=symbols[(n>=4 ? 28 : 36)+tm->tm_wday+1];
      else if (c=='a') word=symbols[44+(tm->tm_hour>=12)];
      else if (c=='G') word=symbols[era!=0];
      if (word) bytes_append(b,word,strlen(word));
      else if (zero=='0' || c=='z' || c=='Z' || c=='X')
         sdf_field(b,c=='L' ? 'M' : c,n,tm,sub_ms,off_secs);
      else {
         unsigned char storage[64];struct rt_bytes field={.p=storage,.cap=sizeof storage};
         sdf_field(&field,c=='L' ? 'M' : c,n,tm,sub_ms,off_secs);
         for (size_t k=0;k<field.len;++k) {
            unsigned ch=field.p[k];
            if (zero!='0' && ch>='0' && ch<='9' && c!='z' && c!='Z' && c!='X') {
               unsigned cp=zero+ch-'0';uint16_t units[2];size_t count=1;
               if (cp>0xffff) {cp-=0x10000;units[0]=(uint16_t)(0xd800+(cp>>10));units[1]=(uint16_t)(0xdc00+(cp&1023));count=2;}
               else units[0]=(uint16_t)cp;
               char text[6];size_t length=jcs_utf16_to_wtf8(units,count,text);bytes_append(b,text,length);
            } else bytes_append(b,field.p+k,1);
         }
         if (field.p!=storage) free(field.p);
      }
      p += n;
   }
}

static bool sdf_format(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)nargs;
   union dvm_value pv = { 0 };
   (void)dvm_get_field(vm, self, "pattern", "Ljava/lang/String;", &pv);
   const char *pat = pv.l ? dvm_string_utf8(vm, pv.l) : "";
   if (!pat) pat = "";

   int64_t ms = now_millis();
   if (ARG(0).l) {
      struct dvm_class *c = dvm_object_class(vm, ARG(0).l);
      /* format(Date) and format(Long) both appear in app code. */
      if (c && (!strcmp(c->name, "java/lang/Long") ||
                !strcmp(c->name, "java/lang/Integer")))
         ms = box_get(vm, ARG(0).l).j;
      else
         ms = time_millis(vm, ARG(0).l);
   }
   union dvm_value calendar={0};dvm_get_field(vm,self,"calendar","Ljava/util/Calendar;",&calendar);
   struct cal_zone_context context;struct lc_zone zone;
   struct lc_calendar *state=cal_state(vm,calendar.l,&context,&zone);
   if (!state || !cal_error(vm,lc_set_millis(state,ms))) return false;
   int32_t ignored;if (!cal_error(vm,lc_get(state,&zone,LC_YEAR,&ignored))) return false;
   const int32_t *fields=state->fields;struct tm tm={0};
   tm.tm_year=fields[LC_YEAR]-1900;tm.tm_mon=fields[LC_MONTH];tm.tm_mday=fields[LC_DATE];
   tm.tm_yday=fields[LC_DAY_OF_YEAR]-1;tm.tm_wday=fields[LC_DAY_OF_WEEK]-1;
   tm.tm_hour=fields[LC_HOUR_OF_DAY];tm.tm_min=fields[LC_MINUTE];tm.tm_sec=fields[LC_SECOND];
   tm.tm_isdst=fields[LC_DST_OFFSET]!=0;
   int32_t sub=fields[LC_MILLISECOND],off=(fields[LC_ZONE_OFFSET]+fields[LC_DST_OFFSET])/1000;
#ifdef _WIN32
   time_zone_offset=off;snprintf(time_zone_display,sizeof time_zone_display,"%s",context.id);
#else
   tm.tm_gmtoff=off;tm.tm_zone=context.id;
#endif

   struct rt_bytes b = { 0 };
   union dvm_value locale={0};dvm_get_field(vm,self,"locale","Ljava/util/Locale;",&locale);
   if (!locale.l && !loc_getDefault(vm,0,NULL,0,&locale)) return false;
   char symbol_name[256],pattern_name[256];
   if (!date_locale_name(vm,locale.l,symbol_name,sizeof symbol_name)) return false;
   snprintf(pattern_name,sizeof pattern_name,"%s",symbol_name);
   unsigned row=dfs_locale_row(symbol_name),pattern_row=date_pattern_row(pattern_name);
   time_render(&b,pat,&tm,sub,off,date_symbol_rows[row],date_pattern_rows[pattern_row].zero,fields[LC_ERA]);
   dvm_ref s = dvm_new_string_n(vm, b.p ? (const char *)b.p : "", b.len);
   free(b.p);
   RETL(s);
}

static bool sdf_setTimeZone(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   union dvm_value calendar={0};dvm_get_field(vm,self,"calendar","Ljava/util/Calendar;",&calendar);
   return cal_setTimeZone(vm,calendar.l,args,nargs,out);
}

static bool sdf_getTimeZone(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value calendar={0};dvm_get_field(vm,self,"calendar","Ljava/util/Calendar;",&calendar);
   return cal_getTimeZone(vm,calendar.l,args,nargs,out);
}

static bool sdf_calendar(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if (nargs) {
      if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","calendar");return false;}
      dvm_set_field(vm,self,"calendar","Ljava/util/Calendar;",ARG(0));RETV();
   }
   union dvm_value calendar={0};dvm_get_field(vm,self,"calendar","Ljava/util/Calendar;",&calendar);RETL(calendar.l);
}
static bool sdf_lenient(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   union dvm_value calendar={0};dvm_get_field(vm,self,"calendar","Ljava/util/Calendar;",&calendar);
   return cal_attribute(vm,calendar.l,args,nargs,out);
}

/* parse() reads back what format() writes, for the numeric fields.  Anything
 * it cannot place is skipped, so a pattern this does not model yields the
 * fields it did recognise rather than nothing. */
static bool sdf_parse(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   (void)nargs;
   union dvm_value pv = { 0 };
   (void)dvm_get_field(vm, self, "pattern", "Ljava/lang/String;", &pv);
   const char *pat = pv.l ? dvm_string_utf8(vm, pv.l) : "";
   const char *s = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   const size_t slen = ARG(0).l ? dvm_string_utf8_length(vm, ARG(0).l) : 0;
   if (!pat || !s) {
      dvm__throw(vm, "java/text/ParseException", "unparseable date");
      return false;
   }

   struct tm tm;
   memset(&tm, 0, sizeof tm);
   tm.tm_year = 70;
   tm.tm_mday = 1;
   int32_t sub = 0;
   const char *in = s;
   const char *in_end = s + slen;
   for (const char *p = pat; *p && in < in_end;) {
      if (*p == '\'') {
         ++p;
         if (*p == '\'') { ++p; if (*in == '\'') ++in; continue; }
         while (*p && *p != '\'') { if (in < in_end) ++in; ++p; }
         if (*p) ++p;
         continue;
      }
      if (!isalpha((unsigned char)*p)) { ++p; if (*in) ++in; continue; }
      char c = *p;
      int n = 0;
      while (p[n] == c) ++n;
      p += n;

      while (in < in_end && !isdigit((unsigned char)*in) &&
             (c == 'y' || c == 'M' || c == 'd' || c == 'H' || c == 'h' ||
              c == 'm' || c == 's' || c == 'S'))
         ++in;
      const char *digits = in;
      long v = 0;
      while (in < in_end && isdigit((unsigned char)*in)) {
         const int digit = *in++ - '0';
         if (v <= (LONG_MAX - digit) / 10) v = v * 10 + digit;
         else v = LONG_MAX;
      }
      if (in == digits) {
         if (in < in_end) ++in;
         else break;
         continue;
      }
      switch (c) {
      case 'y': tm.tm_year = (int)(v < 100 ? v + 2000 : v) - 1900; break;
      case 'M': tm.tm_mon = (int)v - 1; break;
      case 'd': tm.tm_mday = (int)v; break;
      case 'H': case 'h': case 'k': case 'K': tm.tm_hour = (int)v; break;
      case 'm': tm.tm_min = (int)v; break;
      case 's': tm.tm_sec = (int)v; break;
      case 'S': sub = (int32_t)v; break;
      default: break;
      }
   }

   union dvm_value calendar={0};dvm_get_field(vm,self,"calendar","Ljava/util/Calendar;",&calendar);
   struct cal_zone_context context;struct lc_zone zone;
   struct lc_calendar *state=cal_state(vm,calendar.l,&context,&zone);if(!state) return false;
   lc_clear(state,&zone,-1);
   const int fields[]={LC_YEAR,LC_MONTH,LC_DATE,LC_HOUR_OF_DAY,LC_MINUTE,LC_SECOND,LC_MILLISECOND};
   const int32_t values[]={tm.tm_year+1900,tm.tm_mon,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,sub};
   for(unsigned i=0;i<sizeof fields/sizeof fields[0];++i) lc_set(state,&zone,fields[i],values[i]);
   int64_t millis;
   if(lc_get_millis(state,&zone,&millis)!=LC_OK) {dvm__throw(vm,"java/text/ParseException","Invalid calendar date");return false;}
   struct dvm_class *dc = dvm__class_by_desc(vm, "Ljava/util/Date;");
   dvm_ref d = dc ? dvm_new_object(vm, dc) : 0;
   if (d) time_set_millis(vm, d, millis);
   RETL(d);
}

/* Defined with the rest of java.time below; Date is the doorway into it. */
static bool date_toInstant(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out);

/* DateFormat style factories use generated CLDR data and create independent
 * mutable SimpleDateFormat/Calendar objects on every call. */
static bool date_instance(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                           int nargs,union dvm_value *out)
{
   (void)self;int style=nargs ? ARG(0).i : 2;
   if (style<0 || style>3) {dvm__throw(vm,"java/lang/IllegalArgumentException","Invalid date style %d",style);return false;}
   union dvm_value locale={0};
   if (nargs>1) locale=ARG(1);
   else if (!loc_getDefault(vm,0,NULL,0,&locale)) return false;
   char name[256];if (!date_locale_name(vm,locale.l,name,sizeof name)) return false;
   unsigned row=date_pattern_row(name);
   dvm_ref format=dvm_new_object(vm,dvm_find_class(vm,"java/text/SimpleDateFormat"));
   if (!format) return false;
   dvm_pin(vm,format);
   union dvm_value init[]={{.l=dvm_new_string(vm,date_pattern_rows[row].styles[style])},locale};
   bool good=init[0].l && sdf_init(vm,format,init,2,out);
   dvm_unpin(vm,format);
   if (!good) return false;
   RETL(format);
}
static bool am_vcall(struct dvm *, dvm_ref, const char *, const char *,
                     const union dvm_value *, int, union dvm_value *);
static bool android_date_instance(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                   int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   if (!ARG(0).l) {dvm__throw(vm,"java/lang/NullPointerException","context");return false;}
   union dvm_value resources={0},configuration={0},locale={0};
   if (!am_vcall(vm,ARG(0).l,"getResources","()Landroid/content/res/Resources;",NULL,0,&resources) ||
       !am_vcall(vm,resources.l,"getConfiguration","()Landroid/content/res/Configuration;",NULL,0,&configuration)) return false;
   dvm_get_field(vm,configuration.l,"locale","Ljava/util/Locale;",&locale);
   if (!locale.l && !loc_getDefault(vm,0,NULL,0,&locale)) return false;
   const char *name=builtin_name();
   union dvm_value factory[]={{.i=strstr(name,"Medium") ? 2 : strstr(name,"Long") ? 1 : 3},locale};
   return date_instance(vm,0,factory,2,out);
}
static bool sdf_toPattern(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                           int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;return dvm_get_field(vm,self,"pattern","Ljava/lang/String;",out);
}
static const struct rt_method rt_android_date_format[]={
 SM("getDateFormat","(Landroid/content/Context;)Ljava/text/DateFormat;",android_date_instance),
 SM("getMediumDateFormat","(Landroid/content/Context;)Ljava/text/DateFormat;",android_date_instance),
 SM("getLongDateFormat","(Landroid/content/Context;)Ljava/text/DateFormat;",android_date_instance),M_END,
};

static const struct rt_field rt_time_fields[] = {
   { "millis", "J" }, { "zone", "Ljava/lang/String;" }, F_END
};

static const struct rt_field rt_sdf_fields[] = {
   { "pattern", "Ljava/lang/String;" },
   { "calendar", "Ljava/util/Calendar;" }, { "locale", "Ljava/util/Locale;" }, F_END
};

static const struct rt_method rt_date[] = {
   M("<init>", "()V", date_init),
   M("<init>", "(J)V", date_init),
   M("getTime", "()J", date_getTime),
   M("setTime", "(J)V", date_setTime),
   M("before", "(Ljava/util/Date;)Z", date_before),
   M("after", "(Ljava/util/Date;)Z", date_after),
   M("compareTo", "(Ljava/util/Date;)I", date_compareTo),
   M("compareTo", "(Ljava/lang/Object;)I", date_compareTo),
   M("equals", "(Ljava/lang/Object;)Z", date_equals),
   M("toString", "()Ljava/lang/String;", date_toString),
   M("toInstant", "()Ljava/time/Instant;", date_toInstant),
   M_END,
};

static const struct rt_method rt_timezone[] = {
   SM("getDefault", "()Ljava/util/TimeZone;", tz_getDefault),
   SM("getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;", tz_getTimeZone),
   M("getID", "()Ljava/lang/String;", tz_getID),
   M("getOffset", "(J)I", cal_tz_offset),
   M("getRawOffset", "()I", cal_tz_offset),
   M("inDaylightTime", "(Ljava/util/Date;)Z", tz_inDaylightTime),
   M("getDisplayName", "()Ljava/lang/String;", tz_getDisplayName),
   M("getDisplayName", "(ZI)Ljava/lang/String;", tz_getDisplayName),
   M_END,
};

static const struct rt_method rt_sdf[] = {
   SM("getDateInstance","()Ljava/text/DateFormat;",date_instance),
   SM("getDateInstance","(I)Ljava/text/DateFormat;",date_instance),
   SM("getDateInstance","(ILjava/util/Locale;)Ljava/text/DateFormat;",date_instance),
   M("toPattern","()Ljava/lang/String;",sdf_toPattern),
   M("<init>", "(Ljava/lang/String;)V", sdf_init),
   M("<init>", "(Ljava/lang/String;Ljava/util/Locale;)V", sdf_init),
   M("applyPattern", "(Ljava/lang/String;)V", sdf_applyPattern),
   M("format", "(Ljava/util/Date;)Ljava/lang/String;", sdf_format),
   M("format", "(Ljava/lang/Object;)Ljava/lang/String;", sdf_format),
   M("parse", "(Ljava/lang/String;)Ljava/util/Date;", sdf_parse),
   M("setTimeZone", "(Ljava/util/TimeZone;)V", sdf_setTimeZone),
   M("getTimeZone", "()Ljava/util/TimeZone;", sdf_getTimeZone),
   M("getCalendar", "()Ljava/util/Calendar;", sdf_calendar),
   M("setCalendar", "(Ljava/util/Calendar;)V", sdf_calendar),
   M("isLenient", "()Z", sdf_lenient),
   M("setLenient", "(Z)V", sdf_lenient),
   M_END,
};

/* ------------------------------------------------------------------------ *
 * java.time
 *
 * Firebase's HeartBeatInfoStorage.getFormattedDate() takes the API-24 branch on
 * this device, and that branch is one chain:
 *
 *   new Date(ms).toInstant().atOffset(ZoneOffset.UTC).toLocalDateTime()
 *   → DateTimeFormatter.ISO_LOCAL_DATE.format(…)
 *
 * With Date.toInstant() unresolved the chain answered null and the heartbeat
 * store wrote a null date into its own state.  Netmarble's Utils takes the same
 * LocalDateTime.format() step.
 *
 * Temporal values retain seconds, nanoseconds and an offset. LocalDateTime
 * keeps its wall-clock fields through that offset; arithmetic never scales
 * the entire supported date range to milliseconds.
 * ------------------------------------------------------------------------ */

static const struct rt_field rt_temporal_fields[] = {
   { "seconds", "J" }, { "nano", "I" }, { "offset", "I" }, F_END
};

static const struct rt_field rt_zone_offset_fields[] = {
   { "seconds", "I" }, F_END
};

static const struct rt_field rt_dtf_fields[] = {
   { "pattern", "Ljava/lang/String;" }, F_END
};

static dvm_ref jt_new_parts(struct dvm *vm, const char *desc, int64_t seconds,
                             int32_t nano, int32_t off)
{
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   dvm_ref r = c ? dvm_new_object(vm, c) : 0;
   if (!r) return 0;
   (void)dvm_set_field(vm, r, "seconds", "J", (union dvm_value){.j=seconds});
   (void)dvm_set_field(vm, r, "nano", "I", (union dvm_value){.i=nano});
   (void)dvm_set_field(vm, r, "offset", "I", (union dvm_value){.i=off});
   return r;
}

/* Millisecond sources (Date, the host clock) enter the same precise model. */
static dvm_ref jt_new(struct dvm *vm, const char *desc, int64_t ms, int32_t off)
{
   int64_t seconds = lc_floor(ms, 1000);
   int32_t nano = (int32_t)(ms % 1000);
   if (nano < 0) nano += 1000;
   return jt_new_parts(vm, desc, seconds, nano * 1000000, off);
}

static int64_t jt_seconds(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = {0};
   (void)dvm_get_field(vm, self, "seconds", "J", &v);
   return v.j;
}

static int32_t jt_nano(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = {0};
   (void)dvm_get_field(vm, self, "nano", "I", &v);
   return v.i;
}

static int32_t jt_offset(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = {0};
   (void)dvm_get_field(vm, self, "offset", "I", &v);
   return v.i;
}

/* java.time uses the proleptic Gregorian calendar at every year, including
 * year zero. No host time_t or millisecond range limits enter this path. */
static void jt_break(struct dvm *vm, dvm_ref self, struct tm *tm,
                     int32_t *sub_ms, int32_t *off_secs)
{
   int32_t off = jt_offset(vm, self);
   int64_t wall = jt_seconds(vm, self) + off;
   int64_t day = lc_floor(wall, 86400);
   int64_t year; unsigned month, date;
   lc_day_civil(day + LC_EPOCH_JD, true, &year, &month, &date);
   int32_t tod = (int32_t)(wall - day * 86400);
   memset(tm, 0, sizeof *tm);
   tm->tm_year = (int)(year - 1900);
   tm->tm_mon = (int)month - 1; tm->tm_mday = (int)date;
   tm->tm_hour = tod / 3600; tm->tm_min = tod / 60 % 60; tm->tm_sec = tod % 60;
   tm->tm_yday = (int)(day + LC_EPOCH_JD - lc_civil_day(year, 1, 1, true));
   tm->tm_wday = (int)((day + 4) % 7);
   if (tm->tm_wday < 0) tm->tm_wday += 7;
   *sub_ms = jt_nano(vm, self) / 1000000;
   *off_secs = off;
}

static dvm_ref jt_render(struct dvm *vm, dvm_ref temporal, const char *pattern)
{
   struct tm tm;
   int32_t sub = 0, off = 0;
   jt_break(vm, temporal, &tm, &sub, &off);
   struct rt_bytes b = { 0 };
   const char *p = pattern ? pattern : "";
   while (*p) {
      if (*p == '\'') {
         ++p;
         if (*p == '\'') { bytes_append(&b, "'", 1); ++p; continue; }
         while (*p && *p != '\'') bytes_append(&b, p++, 1);
         if (*p) ++p;
         continue;
      }
      if (!isalpha((unsigned char)*p)) { bytes_append(&b, p++, 1); continue; }
      char c = *p; int n = 0;
      while (p[n] == c) ++n;
      if (c == 'S') {
         if (n > 9) { free(b.p); dvm__throw(vm, "java/lang/IllegalArgumentException", "fraction width"); return 0; }
         char fraction[16]; snprintf(fraction, sizeof fraction, "%09d", jt_nano(vm, temporal));
         bytes_append(&b, fraction, (size_t)n);
      } else if (c == 'u' || c == 'y') {
         int year = tm.tm_year + 1900;
         if (c == 'y' && year <= 0) year = 1 - year;
         char text[32];
         if (n == 2) snprintf(text, sizeof text, "%02d", abs(year) % 100);
         else snprintf(text, sizeof text, "%s%0*d", year < 0 ? "-" : (n >= 4 && year > 9999 ? "+" : ""), n > 16 ? 16 : n, abs(year));
         bytes_append(&b, text, strlen(text));
      } else sdf_field(&b, c, n, &tm, sub, off);
      p += n;
   }
   dvm_ref s = dvm_new_string_n(vm, b.p ? (const char *)b.p : "", b.len);
   free(b.p);
   return s;
}

/* format() reads the pattern from whichever of the two objects is the
 * formatter: LocalDateTime.format(fmt) and fmt.format(temporal) are the same
 * operation with the receiver and the argument swapped. */
static dvm_ref jt_pattern_of(struct dvm *vm, dvm_ref formatter)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, formatter, "pattern", "Ljava/lang/String;", &v);
   return v.l;
}

static bool jt_format(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   dvm_ref fmt = nargs > 0 ? ARG(0).l : 0;
   dvm_ref pattern = fmt ? jt_pattern_of(vm, fmt) : 0;
   RETL(jt_render(vm, self, pattern ? dvm_string_utf8(vm, pattern) : ""));
}

static bool dtf_format(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   dvm_ref pattern = jt_pattern_of(vm, self);
   RETL(jt_render(vm, nargs > 0 ? ARG(0).l : 0,
                  pattern ? dvm_string_utf8(vm, pattern) : ""));
}

static bool dtf_ofPattern(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)self;
   struct dvm_class *c =
      dvm__class_by_desc(vm, "Ljava/time/format/DateTimeFormatter;");
   dvm_ref f = c ? dvm_new_object(vm, c) : 0;
   if (f) {
      union dvm_value v = { .l = nargs > 0 ? ARG(0).l : 0 };
      (void)dvm_set_field(vm, f, "pattern", "Ljava/lang/String;", v);
   }
   RETL(f);
}

static bool date_toInstant(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(jt_new(vm, "Ljava/time/Instant;", time_millis(vm, self), 0));
}

static bool instant_now(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(jt_new(vm, "Ljava/time/Instant;", now_millis(), 0));
}

static bool instant_ofEpochMilli(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)nargs;
   RETL(jt_new(vm, "Ljava/time/Instant;", ARG(0).j, 0));
}

static bool instant_ofEpochSecond(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)self;
   int64_t adjustment = nargs > 1 ? ARG(1).j : 0;
   int64_t extra = lc_floor(adjustment, 1000000000);
   if ((extra > 0 && ARG(0).j > INT64_MAX - extra) ||
       (extra < 0 && ARG(0).j < INT64_MIN - extra)) {
      dvm__throw(vm, "java/lang/ArithmeticException", "instant seconds overflow"); return false;
   }
   int64_t seconds = ARG(0).j + extra;
   if (seconds < INT64_C(-31557014167219200) || seconds > INT64_C(31556889864403199)) {
      dvm__throw(vm, "java/time/DateTimeException", "instant out of range"); return false;
   }
   int32_t nano = (int32_t)(adjustment % 1000000000);
   if (nano < 0) nano += 1000000000;
   RETL(jt_new_parts(vm, "Ljava/time/Instant;", seconds, nano, 0));
}

static bool jt_toEpochMilli(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   int64_t seconds = jt_seconds(vm, self), fraction = jt_nano(vm, self) / 1000000;
   /* Move a negative fraction into the preceding multiplication's range. */
   int64_t scaled = seconds < 0 ? seconds + 1 : seconds;
   if (seconds < 0) fraction -= 1000;
   if (scaled > INT64_MAX / 1000 || scaled < INT64_MIN / 1000) goto overflow;
   int64_t value = scaled * 1000;
   if ((fraction > 0 && value > INT64_MAX - fraction) ||
       (fraction < 0 && value < INT64_MIN - fraction)) goto overflow;
   RETJ(value + fraction);
overflow:
   dvm__throw(vm, "java/lang/ArithmeticException", "epoch milliseconds overflow");
   return false;
}

static bool jt_getEpochSecond(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   RETJ(jt_seconds(vm, self));
}

static bool jt_getNano(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(jt_nano(vm, self));
}

static bool jt_order(struct dvm *vm, dvm_ref self, dvm_ref other, int *order)
{
   if (!other) { dvm__throw(vm, "java/lang/NullPointerException", "other"); return false; }
   struct dvm_class *c = dvm_object_class(vm, self);
   bool local = c && !strcmp(c->name, "java/time/LocalDateTime");
   int64_t a = jt_seconds(vm, self), b = jt_seconds(vm, other);
   if (local) { a += jt_offset(vm, self); b += jt_offset(vm, other); }
   int32_t na = jt_nano(vm, self), nb = jt_nano(vm, other);
   *order = a < b ? -1 : a > b ? 1 : na < nb ? -1 : na > nb ? 1 : 0;
   return true;
}

static bool jt_compareTo(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs; int order;
   if (!jt_order(vm, self, ARG(0).l, &order)) return false;
   RETI(order);
}

static bool jt_isBefore(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)nargs; int order;
   if (!jt_order(vm, self, ARG(0).l, &order)) return false;
   RETI(order < 0);
}

static bool jt_isAfter(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)nargs; int order;
   if (!jt_order(vm, self, ARG(0).l, &order)) return false;
   RETI(order > 0);
}

static dvm_ref zone_offset_new(struct dvm *vm, int32_t seconds)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/time/ZoneOffset;");
   dvm_ref z = c ? dvm_new_object(vm, c) : 0;
   if (z) {
      union dvm_value v = { .i = seconds };
      (void)dvm_set_field(vm, z, "seconds", "I", v);
   }
   return z;
}

static bool zoff_ofHours(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)self; (void)nargs;
   if (ARG(0).i < -18 || ARG(0).i > 18) {
      dvm__throw(vm, "java/time/DateTimeException", "offset hours out of range"); return false;
   }
   RETL(zone_offset_new(vm, ARG(0).i * 3600));
}

static bool zoff_ofTotalSeconds(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)self; (void)nargs;
   if (ARG(0).i < -64800 || ARG(0).i > 64800) {
      dvm__throw(vm, "java/time/DateTimeException", "offset seconds out of range"); return false;
   }
   RETL(zone_offset_new(vm, ARG(0).i));
}

static int32_t zone_offset_seconds(struct dvm *vm, dvm_ref zone)
{
   union dvm_value v = { 0 };
   if (zone) (void)dvm_get_field(vm, zone, "seconds", "I", &v);
   return v.i;
}

static bool zoff_getTotalSeconds(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(zone_offset_seconds(vm, self));
}

static void jt_offset_text(char *buf, size_t cap, int32_t off)
{
   if (!off) { snprintf(buf, cap, "Z"); return; }
   int seconds = abs(off);
   int n = snprintf(buf, cap, "%c%02d:%02d", off < 0 ? '-' : '+', seconds / 3600, seconds / 60 % 60);
   if (seconds % 60 && n >= 0 && (size_t)n < cap)
      snprintf(buf + n, cap - (size_t)n, ":%02d", seconds % 60);
}

static bool zoff_toString(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   char buf[20];
   jt_offset_text(buf, sizeof buf, zone_offset_seconds(vm, self));
   RETL(dvm_new_string(vm, buf));
}

/* Bounds are wall-clock seconds for -999999999-01-01 through
 * +999999999-12-31. Check before adding the offset to avoid C overflow. */
static bool jt_local_range(struct dvm *vm, int64_t seconds, int32_t off)
{
   int64_t min = (lc_civil_day(-999999999, 1, 1, true) - LC_EPOCH_JD) * 86400;
   int64_t max = (lc_civil_day(999999999, 12, 31, true) - LC_EPOCH_JD) * 86400 + 86399;
   if (seconds < min - off || seconds > max - off) {
      dvm__throw(vm, "java/time/DateTimeException", "local date-time out of range"); return false;
   }
   return true;
}

static bool instant_atOffset(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "offset"); return false; }
   if (!jt_local_range(vm, jt_seconds(vm, self), zone_offset_seconds(vm, ARG(0).l))) return false;
   RETL(jt_new_parts(vm, "Ljava/time/OffsetDateTime;", jt_seconds(vm, self), jt_nano(vm, self),
               zone_offset_seconds(vm, ARG(0).l)));
}

static bool odt_toInstant(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(jt_new_parts(vm, "Ljava/time/Instant;", jt_seconds(vm, self), jt_nano(vm, self), 0));
}

/* toLocalDateTime() drops the offset from the value's identity but keeps the
 * wall-clock fields it produced, so the offset has to travel with it. */
static bool odt_toLocalDateTime(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(jt_new_parts(vm, "Ljava/time/LocalDateTime;", jt_seconds(vm, self), jt_nano(vm, self),
               jt_offset(vm, self)));
}

static bool odt_getOffset(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(zone_offset_new(vm, jt_offset(vm, self)));
}

static bool ldt_ofEpochSecond(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(2).l) { dvm__throw(vm, "java/lang/NullPointerException", "offset"); return false; }
   if (ARG(1).i < 0 || ARG(1).i > 999999999) {
      dvm__throw(vm, "java/time/DateTimeException", "nano-of-second out of range"); return false;
   }
   int32_t off = zone_offset_seconds(vm, ARG(2).l);
   if (!jt_local_range(vm, ARG(0).j, off)) return false;
   RETL(jt_new_parts(vm, "Ljava/time/LocalDateTime;", ARG(0).j, ARG(1).i, off));
}

static bool ldt_toEpochSecond(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "offset"); return false; }
   RETJ(jt_seconds(vm, self) + jt_offset(vm, self) - zone_offset_seconds(vm, ARG(0).l));
}

static bool ldt_toInstant(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   union dvm_value seconds = {0};
   if (!ldt_toEpochSecond(vm, self, args, nargs, &seconds)) return false;
   RETL(jt_new_parts(vm, "Ljava/time/Instant;", seconds.j, jt_nano(vm, self), 0));
}

/* Month constants use the shared Enum allocation/identity machinery. */
#define JT_MONTHS(X) \
   X(JANUARY) X(FEBRUARY) X(MARCH) X(APRIL) X(MAY) X(JUNE) \
   X(JULY) X(AUGUST) X(SEPTEMBER) X(OCTOBER) X(NOVEMBER) X(DECEMBER)
#define JT_MONTH_FIELD(name) { #name, "Ljava/time/Month;" },
static const struct rt_field rt_month_sfields[] = { JT_MONTHS(JT_MONTH_FIELD) F_END };
#undef JT_MONTH_FIELD
#undef JT_MONTHS

static bool month_of(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   int month = ARG(0).i;
   if (month < 1 || month > 12) {
      dvm__throw(vm, "java/time/DateTimeException", "month out of range"); return false;
   }
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/time/Month;");
   RETL(c && c->sslots ? c->sslots[month - 1].l : 0);
}

static int month_number(struct dvm *vm, dvm_ref self)
{
   union dvm_value ordinal = {0};
   (void)dvm_get_field(vm, self, "ordinal", "I", &ordinal);
   return ordinal.i + 1;
}

static bool month_getValue(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(month_number(vm, self));
}

static bool month_shift(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs;
   int shift = (int)(ARG(0).j % 12);
   if (!strcmp(builtin_name(), "minus")) shift = -shift;
   int index = (month_number(vm, self) - 1 + shift + 12) % 12;
   union dvm_value value = {.i = index + 1};
   return month_of(vm, 0, &value, 1, out);
}

static bool month_length(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
   int month = month_number(vm, self);
   const char *method = builtin_name();
   bool leap = !strcmp(method, "maxLength") || (nargs && ARG(0).i);
   if (!strcmp(method, "minLength")) leap = false;
   if (!strcmp(method, "firstDayOfYear")) {
      int day = 1;
      for (int i = 1; i < month; ++i) day += days[i - 1] + (i == 2 && leap);
      RETI(day);
   }
   RETI(days[month - 1] + (month == 2 && leap));
}

static bool month_firstQuarter(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value value = {.i = (month_number(vm, self) - 1) / 3 * 3 + 1};
   return month_of(vm, 0, &value, 1, out);
}

static const struct rt_method rt_month[] = {
   SM("of", "(I)Ljava/time/Month;", month_of),
   SM("values", "()[Ljava/time/Month;", builtin_enum_values),
   SM("valueOf", "(Ljava/lang/String;)Ljava/time/Month;", builtin_enum_values),
   M("getValue", "()I", month_getValue),
   M("plus", "(J)Ljava/time/Month;", month_shift),
   M("minus", "(J)Ljava/time/Month;", month_shift),
   M("length", "(Z)I", month_length),
   M("minLength", "()I", month_length),
   M("maxLength", "()I", month_length),
   M("firstDayOfYear", "(Z)I", month_length),
   M("firstMonthOfQuarter", "()Ljava/time/Month;", month_firstQuarter),
   M_END,
};

static bool ldt_getMonth(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   struct tm tm; int32_t sub, off;
   jt_break(vm, self, &tm, &sub, &off);
   union dvm_value value = {.i = tm.tm_mon + 1};
   return month_of(vm, 0, &value, 1, out);
}

static bool ldt_now(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   int64_t ms = now_millis();
   RETL(jt_new(vm, "Ljava/time/LocalDateTime;", ms, time_offset(ms, "")));
}

static bool odt_now(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   int64_t ms = now_millis();
   RETL(jt_new(vm, "Ljava/time/OffsetDateTime;", ms, time_offset(ms, "")));
}

#define JT_FIELD(fn, expr)                                                    \
   static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args,  \
                  int nargs, union dvm_value *out)                            \
   {                                                                          \
      (void)args; (void)nargs;                                                \
      struct tm tm; int32_t sub = 0, off = 0;                                 \
      jt_break(vm, self, &tm, &sub, &off);                                    \
      RETI(expr);                                                             \
   }

JT_FIELD(ldt_getYear,       tm.tm_year + 1900)
JT_FIELD(ldt_getMonthValue, tm.tm_mon + 1)
JT_FIELD(ldt_getDayOfMonth, tm.tm_mday)
JT_FIELD(ldt_getHour,       tm.tm_hour)
JT_FIELD(ldt_getMinute,     tm.tm_min)
JT_FIELD(ldt_getSecond,     tm.tm_sec)
#undef JT_FIELD

/* ISO toString retains subsecond precision; LocalTime omits zero seconds,
 * while Instant always writes seconds and a UTC suffix. */
static dvm_ref jt_iso_string(struct dvm *vm, dvm_ref self, int kind)
{
   struct tm tm; int32_t sub, off;
   jt_break(vm, self, &tm, &sub, &off);
   int year = tm.tm_year + 1900, nano = jt_nano(vm, self);
   char text[128], fraction[16], zone[20] = "";
   int length = snprintf(text, sizeof text, "%s%04d-%02d-%02dT%02d:%02d",
      year < 0 ? "-" : year > 9999 ? "+" : "", abs(year),
      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
   if (kind == 0 || tm.tm_sec || nano)
      length += snprintf(text + length, sizeof text - (size_t)length, ":%02d", tm.tm_sec);
   if (nano) {
      int width = nano % 1000000 == 0 ? 3 : nano % 1000 == 0 ? 6 : 9;
      snprintf(fraction, sizeof fraction, "%09d", nano);
      text[length++] = '.';
      memcpy(text + length, fraction, (size_t)width); length += width;
      text[length] = 0;
   }
   if (kind == 0 || kind == 2) {
      jt_offset_text(zone, sizeof zone, kind == 0 ? 0 : off);
      length += snprintf(text + length, sizeof text - (size_t)length, "%s", zone);
   }
   return dvm_new_string_n(vm, text, (size_t)length);
}

static bool instant_toString(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(jt_iso_string(vm, self, 0));
}

static bool ldt_toString(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(jt_iso_string(vm, self, 1));
}

static bool odt_toString(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(jt_iso_string(vm, self, 2));
}

/* LocalDate stores normalized local midnight in the shared temporal fields.
 * No host timezone or millisecond conversion participates in extraction. */
static dvm_ref ld_new_day(struct dvm *vm,int64_t day)
{
   int64_t min=lc_civil_day(-999999999,1,1,true)-LC_EPOCH_JD;
   int64_t max=lc_civil_day(999999999,12,31,true)-LC_EPOCH_JD;
   if (day<min || day>max) {
      dvm__throw(vm,"java/time/DateTimeException","epoch day out of range");return 0;
   }
   return jt_new_parts(vm,"Ljava/time/LocalDate;",day*86400,0,0);
}

static bool ldt_toLocalDate(struct dvm *vm,dvm_ref self,
                            const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   dvm_ref date=ld_new_day(vm,lc_floor(jt_seconds(vm,self)+jt_offset(vm,self),86400));
   if (!date) return false;
   RETL(date);
}

static bool ld_ofEpochDay(struct dvm *vm,dvm_ref self,
                          const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   dvm_ref date=ld_new_day(vm,ARG(0).j);
   if (!date) return false;
   RETL(date);
}

static bool ld_of(struct dvm *vm,dvm_ref self,
                  const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)nargs;
   int year=ARG(0).i,month=ARG(1).i,day=ARG(2).i;
   if (year < -999999999 || year > 999999999 || month<1 || month>12 || day<1 || day>31) {
      dvm__throw(vm,"java/time/DateTimeException","invalid local date");return false;
   }
   int64_t jd=lc_civil_day(year,(unsigned)month,(unsigned)day,true),y;
   unsigned m,d;
   lc_day_civil(jd,true,&y,&m,&d);
   if (y!=year || m!=(unsigned)month || d!=(unsigned)day) {
      dvm__throw(vm,"java/time/DateTimeException","invalid day of month");return false;
   }
   dvm_ref date=ld_new_day(vm,jd-LC_EPOCH_JD);
   if (!date) return false;
   RETL(date);
}

static bool ld_now(struct dvm *vm,dvm_ref self,
                   const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;
   int64_t ms=now_millis();
   dvm_ref date=ld_new_day(vm,lc_floor(lc_floor(ms,1000)+time_offset(ms,""),86400));
   if (!date) return false;
   RETL(date);
}

static bool ld_toEpochDay(struct dvm *vm,dvm_ref self,
                          const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;RETJ(jt_seconds(vm,self)/86400);
}

static bool ld_toString(struct dvm *vm,dvm_ref self,
                        const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   struct tm tm;int32_t sub,off;
   jt_break(vm,self,&tm,&sub,&off);
   int year=tm.tm_year+1900;
   char text[32];
   int n=snprintf(text,sizeof text,"%s%04d-%02d-%02d",year<0 ? "-" : year>9999 ? "+" : "",
                  abs(year),tm.tm_mon+1,tm.tm_mday);
   RETL(dvm_new_string_n(vm,text,(size_t)n));
}

static bool ld_equals(struct dvm *vm,dvm_ref self,
                      const union dvm_value *args,int nargs,union dvm_value *out)
{
   struct dvm_class *c=nargs ? dvm_object_class(vm,ARG(0).l) : NULL;
   RETI(c && !strcmp(c->name,"java/time/LocalDate") && jt_seconds(vm,self)==jt_seconds(vm,ARG(0).l));
}

static bool ld_hashCode(struct dvm *vm,dvm_ref self,
                        const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   struct tm tm;int32_t sub,off;
   jt_break(vm,self,&tm,&sub,&off);
   uint32_t y=(uint32_t)(tm.tm_year+1900);
   RETI((int32_t)((y&0xfffff800u)^(y<<11)^((uint32_t)(tm.tm_mon+1)<<6)^(uint32_t)tm.tm_mday));
}

static const struct rt_method rt_local_date[]={
   SM("now","()Ljava/time/LocalDate;",ld_now),
   SM("of","(III)Ljava/time/LocalDate;",ld_of),
   SM("ofEpochDay","(J)Ljava/time/LocalDate;",ld_ofEpochDay),
   M("toEpochDay","()J",ld_toEpochDay),
   M("getYear","()I",ldt_getYear),
   M("getMonthValue","()I",ldt_getMonthValue),
   M("getMonth","()Ljava/time/Month;",ldt_getMonth),
   M("getDayOfMonth","()I",ldt_getDayOfMonth),
   M("format","(Ljava/time/format/DateTimeFormatter;)Ljava/lang/String;",jt_format),
   M("compareTo","(Ljava/time/chrono/ChronoLocalDate;)I",jt_compareTo),
   M("compareTo","(Ljava/lang/Object;)I",jt_compareTo),
   M("isBefore","(Ljava/time/chrono/ChronoLocalDate;)Z",jt_isBefore),
   M("isAfter","(Ljava/time/chrono/ChronoLocalDate;)Z",jt_isAfter),
   M("isEqual","(Ljava/time/chrono/ChronoLocalDate;)Z",ld_equals),
   M("equals","(Ljava/lang/Object;)Z",ld_equals),
   M("hashCode","()I",ld_hashCode),
   M("toString","()Ljava/lang/String;",ld_toString),
   M_END,
};

static const struct rt_method rt_instant[] = {
   SM("now", "()Ljava/time/Instant;", instant_now),
   SM("ofEpochMilli", "(J)Ljava/time/Instant;", instant_ofEpochMilli),
   SM("ofEpochSecond", "(J)Ljava/time/Instant;", instant_ofEpochSecond),
   SM("ofEpochSecond", "(JJ)Ljava/time/Instant;", instant_ofEpochSecond),
   M("toEpochMilli", "()J", jt_toEpochMilli),
   M("getEpochSecond", "()J", jt_getEpochSecond),
   M("getNano", "()I", jt_getNano),
   M("atOffset", "(Ljava/time/ZoneOffset;)Ljava/time/OffsetDateTime;",
     instant_atOffset),
   M("compareTo", "(Ljava/time/Instant;)I", jt_compareTo),
   M("compareTo", "(Ljava/lang/Object;)I", jt_compareTo),
   M("isBefore", "(Ljava/time/Instant;)Z", jt_isBefore),
   M("isAfter", "(Ljava/time/Instant;)Z", jt_isAfter),
   M("toString", "()Ljava/lang/String;", instant_toString),
   M_END,
};

static bool duration_ofMinutes(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)self; (void)nargs;
   int64_t minutes = ARG(0).j;
   if (minutes > INT64_MAX / 60 || minutes < INT64_MIN / 60) {
      dvm__throw(vm, "java/lang/ArithmeticException", "long overflow");
      return false;
   }
   RETL(jt_new_parts(vm, "Ljava/time/Duration;", minutes * 60, 0, 0));
}

static const struct rt_method rt_duration[] = {
   SM("ofMinutes", "(J)Ljava/time/Duration;", duration_ofMinutes),
   M("getSeconds", "()J", jt_getEpochSecond),
   M("getNano", "()I", jt_getNano),
   M_END,
};

/* ISO-8601 date-time with an offset: yyyy-MM-dd'T'HH:mm[:ss[.fraction]](Z|+hh:mm[:ss])
 * and, for ZonedDateTime, an optional trailing [Region/City].  Stored like every
 * other temporal here: epoch seconds, nanos and the offset in effect. */
static bool iso_digits(const char **p, int count, int *value)
{
   int v = 0;
   for (int i = 0; i < count; ++i, ++*p) {
      if (**p < '0' || **p > '9') return false;
      v = v * 10 + (**p - '0');
   }
   *value = v;
   return true;
}

static bool iso_parse(struct dvm *vm, dvm_ref text, int64_t *seconds, int32_t *nano,
                      int32_t *offset, char zone[64])
{
   const char *s = text ? dvm_string_utf8(vm, text) : NULL, *p = s;
   int y, mo, d, h, mi, sec = 0, sign = 1, oh = 0, om = 0, os = 0;
   int32_t frac = 0;
   zone[0] = 0;
   if (!s) { dvm__throw(vm, "java/lang/NullPointerException", "text"); return false; }
   bool ok = iso_digits(&p, 4, &y) && *p++ == '-' && iso_digits(&p, 2, &mo) && *p++ == '-' &&
             iso_digits(&p, 2, &d) && (*p == 'T' || *p == 't') && p++ &&
             iso_digits(&p, 2, &h) && *p++ == ':' && iso_digits(&p, 2, &mi);
   if (ok && *p == ':') { ++p; ok = iso_digits(&p, 2, &sec); }
   if (ok && *p == '.') {
      ++p;
      int digits = 0;
      for (; *p >= '0' && *p <= '9'; ++p, ++digits)
         if (digits < 9) frac = frac * 10 + (*p - '0');
      ok = digits > 0;
      for (; ok && digits < 9; ++digits) frac *= 10;
   }
   if (ok && (*p == 'Z' || *p == 'z')) ++p;
   else if (ok && (*p == '+' || *p == '-')) {
      sign = *p++ == '-' ? -1 : 1;
      ok = iso_digits(&p, 2, &oh);
      if (ok && *p == ':') ++p;
      if (ok && *p >= '0' && *p <= '9') ok = iso_digits(&p, 2, &om);
      if (ok && *p == ':') { ++p; ok = iso_digits(&p, 2, &os); }
   } else ok = false;
   if (ok && *p == '[') {
      const char *end = strchr(p + 1, ']');
      if (end && end - p - 1 < 63) { memcpy(zone, p + 1, (size_t)(end - p - 1)); zone[end - p - 1] = 0; p = end + 1; }
   }
   if (!ok || *p || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59 || oh > 18 || om > 59 || os > 59) {
      dvm__throw(vm, "java/time/format/DateTimeParseException", "Text '%s' could not be parsed", s);
      return false;
   }
   int64_t day = lc_civil_day(y, (unsigned)mo, (unsigned)d, true) - LC_EPOCH_JD;
   *offset = sign * (oh * 3600 + om * 60 + os);
   *seconds = day * 86400 + h * 3600 + mi * 60 + sec - *offset;
   *nano = frac;
   return true;
}

static bool odt_parse_as(struct dvm *vm, const union dvm_value *args, union dvm_value *out,
                         const char *desc)
{
   int64_t seconds; int32_t nano, offset; char zone[64];
   if (!iso_parse(vm, args[0].l, &seconds, &nano, &offset, zone)) return false;
   dvm_ref r = jt_new_parts(vm, desc, seconds, nano, offset);
   if (!r) return false;
   if (!strcmp(desc, "Ljava/time/ZonedDateTime;") && zone[0])
      (void)dvm_set_field(vm, r, "zone", "Ljava/lang/String;", (union dvm_value){.l = dvm_new_string(vm, zone)});
   RETL(r);
}

static bool odt_parse(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                      union dvm_value *out)
{ (void)self; (void)nargs; return odt_parse_as(vm, args, out, "Ljava/time/OffsetDateTime;"); }
static bool zdt_parse(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                      union dvm_value *out)
{ (void)self; (void)nargs; return odt_parse_as(vm, args, out, "Ljava/time/ZonedDateTime;"); }

/* ZoneId: an offset ("Z", "+09:00") or a region name the host zone database knows.
 * A region keeps its id; its offset at an instant comes from that database. */
static const char *zone_id_text(struct dvm *vm, dvm_ref zone, char buf[64])
{
   struct dvm_class *c = dvm_object_class(vm, zone), *off = dvm__class_by_desc(vm, "Ljava/time/ZoneOffset;");
   if (c && off && dvm__class_assignable(vm, c, off)) {
      int32_t t = zone_offset_seconds(vm, zone);
      if (!t) return strcpy(buf, "Z");
      int a = t < 0 ? -t : t;
      snprintf(buf, 64, "%c%02d:%02d", t < 0 ? '-' : '+', a / 3600, a / 60 % 60);
      return buf;
   }
   union dvm_value id = {0};
   (void)dvm_get_field(vm, zone, "id", "Ljava/lang/String;", &id);
   const char *t = id.l ? dvm_string_utf8(vm, id.l) : NULL;
   snprintf(buf, 64, "%s", t ? t : "");
   return buf;
}

static bool zoneid_of(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *id = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!id) { dvm__throw(vm, "java/lang/NullPointerException", "zoneId"); return false; }
   if (!strcmp(id, "Z") || id[0] == '+' || id[0] == '-') {
      int sign = id[0] == '-' ? -1 : 1, h = 0, m = 0;
      const char *p = id[0] == 'Z' ? id + 1 : id + 1;
      if (id[0] != 'Z' && !(iso_digits(&p, 2, &h) && (*p != ':' || (++p, 1)) &&
                             (!*p || iso_digits(&p, 2, &m)) && !*p)) {
         dvm__throw(vm, "java/time/DateTimeException", "Invalid ID for ZoneOffset: %s", id); return false;
      }
      RETL(zone_offset_new(vm, sign * (h * 3600 + m * 60)));
   }
   /* A region id is Area/Location (or a short name such as UTC/GMT): letters, digits, / _ - + only. */
   bool valid = *id != 0;
   for (const char *q = id; *q && valid; ++q)
      valid = isalnum((unsigned char)*q) || *q == '/' || *q == '_' || *q == '-' || *q == '+';
   if (!valid) { dvm__throw(vm, "java/time/DateTimeException", "Invalid ID for region-based ZoneId, invalid format: %s", id); return false; }
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/time/ZoneId;");
   dvm_ref z = c ? dvm_new_object(vm, c) : 0;
   if (!z) return false;
   (void)dvm_set_field(vm, z, "id", "Ljava/lang/String;", (union dvm_value){.l = ARG(0).l});
   RETL(z);
}

static bool zoneid_systemDefault(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   const char *tz = getenv("TZ");
   union dvm_value id = { .l = dvm_new_string(vm, tz && *tz ? tz : "UTC") }, a[1] = { id };
   return zoneid_of(vm, 0, a, 1, out);
}

static bool zoneid_getId(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   char buf[64];
   RETL(dvm_new_string(vm, zone_id_text(vm, self, buf)));
}

/* withZoneSameInstant(zone): same instant, the offset that zone has then. */
static bool zdt_withZoneSameInstant(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "zone"); return false; }
   char buf[64];
   const char *id = zone_id_text(vm, ARG(0).l, buf);
   int64_t seconds = jt_seconds(vm, self);
   struct dvm_class *c = dvm_object_class(vm, ARG(0).l), *off = dvm__class_by_desc(vm, "Ljava/time/ZoneOffset;");
   int32_t offset = c && off && dvm__class_assignable(vm, c, off) ? zone_offset_seconds(vm, ARG(0).l)
                                                                  : time_offset(seconds * 1000, id);
   dvm_ref r = jt_new_parts(vm, "Ljava/time/ZonedDateTime;", seconds, jt_nano(vm, self), offset);
   if (!r) return false;
   (void)dvm_set_field(vm, r, "zone", "Ljava/lang/String;", (union dvm_value){.l = dvm_new_string(vm, id)});
   RETL(r);
}

static bool zdt_getZone(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value id = {0}, a[1];
   (void)dvm_get_field(vm, self, "zone", "Ljava/lang/String;", &id);
   if (!id.l) RETL(zone_offset_new(vm, jt_offset(vm, self)));
   a[0] = id;
   return zoneid_of(vm, 0, a, 1, out);
}

static bool zdt_toOffsetDateTime(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(jt_new_parts(vm, "Ljava/time/OffsetDateTime;", jt_seconds(vm, self), jt_nano(vm, self), jt_offset(vm, self)));
}

static bool zdt_now(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   int64_t ms = now_millis();
   RETL(jt_new(vm, "Ljava/time/ZonedDateTime;", ms, time_offset(ms, "")));
}

static const struct rt_method rt_zone_id[] = {
   SM("of", "(Ljava/lang/String;)Ljava/time/ZoneId;", zoneid_of),
   SM("systemDefault", "()Ljava/time/ZoneId;", zoneid_systemDefault),
   M("getId", "()Ljava/lang/String;", zoneid_getId),
   M("toString", "()Ljava/lang/String;", zoneid_getId),
   M_END,
};
static const struct rt_field rt_zone_id_fields[] = { { "id", "Ljava/lang/String;" }, F_END };

static const struct rt_method rt_zone_offset[] = {
   SM("ofHours", "(I)Ljava/time/ZoneOffset;", zoff_ofHours),
   SM("ofTotalSeconds", "(I)Ljava/time/ZoneOffset;", zoff_ofTotalSeconds),
   M("getTotalSeconds", "()I", zoff_getTotalSeconds),
   M("getId", "()Ljava/lang/String;", zoff_toString),
   M("toString", "()Ljava/lang/String;", zoff_toString),
   M_END,
};

static const struct rt_method rt_offset_date_time[] = {
   SM("now", "()Ljava/time/OffsetDateTime;", odt_now),
   SM("parse", "(Ljava/lang/CharSequence;)Ljava/time/OffsetDateTime;", odt_parse),
   M("toInstant", "()Ljava/time/Instant;", odt_toInstant),
   M("toLocalDateTime", "()Ljava/time/LocalDateTime;", odt_toLocalDateTime),
   M("toLocalDate", "()Ljava/time/LocalDate;", ldt_toLocalDate),
   M("toEpochSecond", "()J", jt_getEpochSecond),
   M("getOffset", "()Ljava/time/ZoneOffset;", odt_getOffset),
   M("getYear", "()I", ldt_getYear),
   M("getMonthValue", "()I", ldt_getMonthValue),
   M("getMonth", "()Ljava/time/Month;", ldt_getMonth),
   M("getDayOfMonth", "()I", ldt_getDayOfMonth),
   M("getHour", "()I", ldt_getHour),
   M("getMinute", "()I", ldt_getMinute),
   M("getSecond", "()I", ldt_getSecond),
   M("getNano", "()I", jt_getNano),
   M("format", "(Ljava/time/format/DateTimeFormatter;)Ljava/lang/String;",
     jt_format),
   M("toString", "()Ljava/lang/String;", odt_toString),
   M_END,
};

static const struct rt_field rt_zoned_fields[] = {
   { "seconds", "J" }, { "nano", "I" }, { "offset", "I" }, { "zone", "Ljava/lang/String;" }, F_END
};
static const struct rt_method rt_zoned_date_time[] = {
   SM("now", "()Ljava/time/ZonedDateTime;", zdt_now),
   SM("parse", "(Ljava/lang/CharSequence;)Ljava/time/ZonedDateTime;", zdt_parse),
   M("withZoneSameInstant", "(Ljava/time/ZoneId;)Ljava/time/ZonedDateTime;", zdt_withZoneSameInstant),
   M("getZone", "()Ljava/time/ZoneId;", zdt_getZone),
   M("toOffsetDateTime", "()Ljava/time/OffsetDateTime;", zdt_toOffsetDateTime),
   M("toInstant", "()Ljava/time/Instant;", odt_toInstant),
   M("toLocalDateTime", "()Ljava/time/LocalDateTime;", odt_toLocalDateTime),
   M("toLocalDate", "()Ljava/time/LocalDate;", ldt_toLocalDate),
   M("toEpochSecond", "()J", jt_getEpochSecond),
   M("getOffset", "()Ljava/time/ZoneOffset;", odt_getOffset),
   M("getYear", "()I", ldt_getYear),
   M("getMonthValue", "()I", ldt_getMonthValue),
   M("getMonth", "()Ljava/time/Month;", ldt_getMonth),
   M("getDayOfMonth", "()I", ldt_getDayOfMonth),
   M("getHour", "()I", ldt_getHour),
   M("getMinute", "()I", ldt_getMinute),
   M("getSecond", "()I", ldt_getSecond),
   M("getNano", "()I", jt_getNano),
   M("format", "(Ljava/time/format/DateTimeFormatter;)Ljava/lang/String;", jt_format),
   M("toString", "()Ljava/lang/String;", odt_toString),
   M_END,
};

static const struct rt_method rt_local_date_time[] = {
   M("toLocalDate", "()Ljava/time/LocalDate;", ldt_toLocalDate),
   SM("now", "()Ljava/time/LocalDateTime;", ldt_now),
   SM("ofEpochSecond", "(JILjava/time/ZoneOffset;)Ljava/time/LocalDateTime;", ldt_ofEpochSecond),
   M("toEpochSecond", "(Ljava/time/ZoneOffset;)J", ldt_toEpochSecond),
   M("toInstant", "(Ljava/time/ZoneOffset;)Ljava/time/Instant;", ldt_toInstant),
   M("getYear", "()I", ldt_getYear),
   M("getMonthValue", "()I", ldt_getMonthValue),
   M("getMonth", "()Ljava/time/Month;", ldt_getMonth),
   M("getDayOfMonth", "()I", ldt_getDayOfMonth),
   M("getHour", "()I", ldt_getHour),
   M("getMinute", "()I", ldt_getMinute),
   M("getSecond", "()I", ldt_getSecond),
   M("getNano", "()I", jt_getNano),
   M("format", "(Ljava/time/format/DateTimeFormatter;)Ljava/lang/String;",
     jt_format),
   M("compareTo", "(Ljava/time/LocalDateTime;)I", jt_compareTo),
   M("compareTo", "(Ljava/lang/Object;)I", jt_compareTo),
   M("isBefore", "(Ljava/time/chrono/ChronoLocalDateTime;)Z", jt_isBefore),
   M("isAfter", "(Ljava/time/chrono/ChronoLocalDateTime;)Z", jt_isAfter),
   M("toString", "()Ljava/lang/String;", ldt_toString),
   M_END,
};

static const struct rt_method rt_date_time_formatter[] = {
   SM("ofPattern", "(Ljava/lang/String;)Ljava/time/format/DateTimeFormatter;",
      dtf_ofPattern),
   SM("ofPattern",
      "(Ljava/lang/String;Ljava/util/Locale;)Ljava/time/format/DateTimeFormatter;",
      dtf_ofPattern),
   M("format", "(Ljava/time/temporal/TemporalAccessor;)Ljava/lang/String;",
     dtf_format),
   M_END,
};

