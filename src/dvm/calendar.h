/* MPL-2.0. Calendar state and arithmetic, shared by Android class adapters.
 * No VM objects, locale library, or host-specific date representation. */
#ifndef LUNARIA_DVM_CALENDAR_H
#define LUNARIA_DVM_CALENDAR_H
#include <stdbool.h>
#include <stdint.h>
enum {
 LC_ERA,LC_YEAR,LC_MONTH,LC_WEEK_OF_YEAR,LC_WEEK_OF_MONTH,LC_DATE,
 LC_DAY_OF_YEAR,LC_DAY_OF_WEEK,LC_DAY_OF_WEEK_IN_MONTH,LC_AM_PM,LC_HOUR,
 LC_HOUR_OF_DAY,LC_MINUTE,LC_SECOND,LC_MILLISECOND,LC_ZONE_OFFSET,
 LC_DST_OFFSET,LC_YEAR_WOY,LC_DOW_LOCAL,LC_EXTENDED_YEAR,LC_JULIAN_DAY,
 LC_MILLISECONDS_IN_DAY,LC_IS_LEAP_MONTH,LC_COUNT
};
enum {LC_OK, LC_FIELD_ERROR, LC_RANGE_ERROR, LC_ZONE_ERROR, LC_UNSUPPORTED};
#define LC_DAY INT64_C(86400000)
#define LC_EPOCH_JD INT64_C(2440588)
#define LC_MIN_MS INT64_C(-184303902528000000)
#define LC_MAX_MS ((INT64_C(0x7f000000)-LC_EPOCH_JD)*LC_DAY)
struct lc_zone {
 void *context;
 bool (*offset)(void *,int64_t,int32_t *,int32_t *);
 bool (*resolve)(void *,int64_t,int,int,bool,int64_t *);
};
struct lc_calendar {
 int32_t fields[LC_COUNT];
 uint32_t stamps[LC_COUNT],next_stamp;
 int64_t millis,cutover;
 unsigned first_day,min_days;
 int repeated,skipped;
 bool icu,buddhist,lenient,time_valid,fields_valid,virtual_fields;
};
void lc_init(struct lc_calendar *,bool icu,bool buddhist,unsigned first,unsigned minimal);
int lc_set_millis(struct lc_calendar *,int64_t);
int lc_get_millis(struct lc_calendar *,const struct lc_zone *,int64_t *);
int lc_get(struct lc_calendar *,const struct lc_zone *,int,int32_t *);
int lc_set(struct lc_calendar *,const struct lc_zone *,int,int32_t);
int lc_clear(struct lc_calendar *,const struct lc_zone *,int); /* -1 = all */
bool lc_is_set(const struct lc_calendar *,int);
int lc_add(struct lc_calendar *,const struct lc_zone *,int,int32_t);
int lc_roll(struct lc_calendar *,const struct lc_zone *,int,int32_t);
int lc_limit(struct lc_calendar *,const struct lc_zone *,int,int,int32_t *);
int64_t lc_floor(int64_t,int64_t);
int64_t lc_civil_day(int64_t,unsigned,unsigned,bool gregorian);
void lc_day_civil(int64_t,bool gregorian,int64_t *,unsigned *,unsigned *);
#endif
