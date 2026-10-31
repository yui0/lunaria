/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef LUNARIA_DVM_LIB_H
#define LUNARIA_DVM_LIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include "../luna-ui/nanosvg.h"

/* ---- java.nio.charset ---- */
/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * java.nio.charset for the VM: turning bytes in a named charset into the
 * VM's string storage and back.
 *
 * Strings here are stored as UTF-8, except that a lone surrogate — which a
 * Java String can hold and UTF-8 cannot — is kept as its three-byte form
 * (WTF-8).  Everything below converts between that and the charset's bytes
 * with the rules a device applies:
 *
 *  - decoding never fails: malformed input becomes U+FFFD, one per maximal
 *    ill-formed subsequence (Unicode 3.9, the ICU/Android behaviour);
 *  - encoding never fails: a character the charset cannot represent becomes
 *    the charset's replacement, '?' for every charset this handles;
 *  - "UTF-16" reads a byte-order mark (big-endian without one) and writes a
 *    big-endian one.
 *
 * UTF-8, ISO-8859-1, US-ASCII and the UTF-16 family are done here; every
 * other charset (GBK, Shift_JIS, EUC-KR, Big5, windows-125x …) goes through
 * the platform code-page API on Windows and iconv on Linux/macOS.
 */


enum jcs_kind {
   JCS_UTF8,
   JCS_LATIN1,
   JCS_ASCII,
   JCS_UTF16,      /* BOM-detecting on decode, BOM + big-endian on encode */
   JCS_UTF16BE,
   JCS_UTF16LE,
   JCS_NATIVE,
};

struct jcs {
   enum jcs_kind kind;
   char name[64];   /* Java's canonical name, what Charset.name() returns */
};

/* Charset.isSupported's name syntax: letters, digits and "-+:_.", starting
 * with a letter or digit.  A name that fails this is an
 * IllegalCharsetNameException rather than an unsupported charset. */
bool jcs_name_legal(const char *name);

/* Resolves a charset name or alias (case-insensitive).  False when no
 * charset of that name exists. */
bool jcs_lookup(const char *name, struct jcs *out);

/* The platform default charset, UTF-8 on Android. */
void jcs_default(struct jcs *out);

/* Bytes in `cs` to WTF-8.  The result is NUL-terminated; *out_len excludes
 * the terminator.  NULL only when memory runs out. */
char *jcs_decode(const struct jcs *cs, const uint8_t *in, size_t n,
                 size_t *out_len);

/* WTF-8 to bytes in `cs`.  NULL only when memory runs out. */
uint8_t *jcs_encode(const struct jcs *cs, const char *wtf8, size_t n,
                    size_t *out_len);

/* UTF-16 code units to WTF-8: surrogate pairs become one four-byte sequence,
 * a lone surrogate its three-byte form.  `out` needs 3 * n bytes; returns the
 * bytes written (no terminator). */
size_t jcs_utf16_to_wtf8(const uint16_t *u, size_t n, char *out);

/* WTF-8 to UTF-16 code units. `out` needs n units, or may be NULL to count. */
size_t jcs_wtf8_to_utf16(const char *s, size_t n, uint16_t *out);

/* A decoder for a byte stream (InputStreamReader): bytes are pulled from
 * the caller one at a time and UTF-16 units come out in order. */
struct jcs_decoder;

struct jcs_decoder *jcs_decoder_new(const struct jcs *cs);
void jcs_decoder_free(struct jcs_decoder *d);

/* The next UTF-16 unit.  `next_byte` returns the next byte (0..255), -1 at
 * the end of the input, or -2 when reading failed; a -2 is passed straight
 * back.  Returns -1 once the input is exhausted and every unit delivered. */
int jcs_decoder_next(struct jcs_decoder *d, int (*next_byte)(void *ctx),
                     void *ctx);

/* ---- java.util.regex engine ---- */
/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* A java.util.regex engine.
 *
 * The VM used to translate Java patterns into POSIX ERE and hand them to the
 * host's regcomp()/regexec().  That is not the same language: POSIX ERE has no
 * reluctant quantifier, no lookaround, no backreference, no \b, no \A/\z, and
 * leftmost-longest instead of Java's leftmost-first semantics.  Every one of
 * those differences is silent — the translation either drops the construct or
 * refuses the pattern, and app code sees a PatternSyntaxException or, worse, a
 * match of the wrong extent.  `(.*?)` — the single most common idiom in
 * hand-written parsers — came out as a greedy `(.*)`.
 *
 * So this is a backtracking matcher written against Java's grammar directly.
 * It works over UTF-8 and reports byte offsets, which is what the VM's string
 * objects are; the callers convert when they have to.
 *
 * The engine is self-contained: it depends on nothing from the VM, so it can
 * be tested on its own (test/regex_test.c). */


/* Pattern flag bits, spelled exactly as java.util.regex.Pattern does, so a
 * guest's Pattern.compile(s, flags) passes its int through untouched. */
enum {
   RX_UNIX_LINES               = 0x01,
   RX_CASE_INSENSITIVE         = 0x02,
   RX_COMMENTS                 = 0x04,
   RX_MULTILINE                = 0x08,
   RX_LITERAL                  = 0x10,
   RX_DOTALL                   = 0x20,
   RX_UNICODE_CASE             = 0x40,
   RX_CANON_EQ                 = 0x80,
   RX_UNICODE_CHARACTER_CLASS  = 0x100,
};

struct rx;

/* Compile `pattern`.  On failure returns NULL and, when `err` is non-NULL,
 * points it at a static-lifetime-free heap string describing the problem in
 * the shape java.util.regex.PatternSyntaxException wants; the caller frees. */
struct rx *rx_compile(const char *pattern, int flags, char **err);
void rx_free(struct rx *re);

/* The pattern text as it was handed to rx_compile(). */
const char *rx_source(const struct rx *re);
int rx_flags(const struct rx *re);
/* Number of capturing groups, not counting group 0. */
int rx_group_count(const struct rx *re);
/* Index of a named group, or -1. */
int rx_group_index(const struct rx *re, const char *name);

/* Search `text[0..len)` for a match starting at or after `from`.
 *
 * `caps` receives 2*(rx_group_count()+1) byte offsets — start/end per group,
 * -1/-1 for a group that did not participate.  `ncaps` is the number of ints
 * `caps` can hold; groups beyond it are simply not reported.
 *
 * `anchor_start` requires the match to begin exactly at `from` (lookingAt);
 * `anchor_end` requires it to end at `len` (matches()).  `from` also fixes
 * where \G matches. */
bool rx_search(const struct rx *re, const char *text, size_t len, size_t from,
               int *caps, int ncaps, bool anchor_start, bool anchor_end);

/* A bounded compiled-pattern cache, so String.matches()/replaceAll() in a loop
 * do not recompile (and, since builtin objects have no finalizer, leak) once
 * per call.  rx_cached() hands out a reference; give it back with
 * rx_cached_release(), which frees the pattern if the table had no room for
 * it.  Never call rx_free() on a pattern from rx_cached(). */
const struct rx *rx_cached(const char *pattern, int flags, char **err);
void rx_cached_release(const struct rx *re);

/* ---- calendar arithmetic ---- */
/* MPL-2.0. Calendar state and arithmetic, shared by Android class adapters.
 * No VM objects, locale library, or host-specific date representation. */
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

/* ---- Porter-Duff / separable blend modes ---- */
/* MPL-2.0. Android/Skia blend modes on straight ARGB pixels. */
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

/* ---- Path geometry ---- */
/* Shared C geometry for Android Path and VectorDrawable. */
typedef struct luna_path {
   NSVGpath *first, *last, *spare;
   size_t capacity;
   int fill;
   bool after_close;
} luna_path;
void luna_path_free(void *path);
void luna_path_clear(luna_path *path,bool reset_fill);
void luna_path_rewind(luna_path *path);
bool luna_path_move(luna_path *path,float x,float y);
bool luna_path_line(luna_path *path,float x,float y);
bool luna_path_cubic(luna_path *path,const float points[6]);
bool luna_path_quad(luna_path *path,float x1,float y1,float x2,float y2);
bool luna_path_close(luna_path *path);
bool luna_path_append(luna_path *path,const luna_path *other,const float *matrix);
bool luna_path_transform(luna_path *path,const float matrix[9]);
bool luna_path_arc(luna_path *path,const float rect[4],float start,float sweep,bool move);
bool luna_path_round_rect(luna_path *path,const float rect[4],const float radii[8],bool counterclockwise);
bool luna_path_parse(luna_path *path,const char *data);
bool luna_path_trim(luna_path *out,const luna_path *path,float start,float end,float offset);
void luna_path_bounds(const luna_path *path,float bounds[4]);
typedef struct luna_path_measure luna_path_measure;
luna_path_measure *luna_path_measure_new(void);
void luna_path_measure_free(void *measure);
bool luna_path_measure_set(luna_path_measure *measure,const luna_path *path,bool force_closed);
float luna_path_measure_length(const luna_path_measure *measure);
bool luna_path_measure_closed(const luna_path_measure *measure);
/* next/segment return -1 for allocation failure, 0 for no result, 1 for success. */
int luna_path_measure_next(luna_path_measure *measure);
bool luna_path_measure_sample(const luna_path_measure *measure,float distance,float pos[2],float tangent[2]);
int luna_path_measure_segment(const luna_path_measure *measure,float start,float stop,luna_path *dst,bool move);
/* Fraction/x/y triples, owned by caller. Moves retain the same fraction. */
float *luna_path_approximate(const luna_path *path,float error,size_t *count);
/* RGBA mask, with top-left origin; free() by caller. Matrix is row-major. */
unsigned char *luna_path_mask(const luna_path *path,int w,int h,const float matrix[9],
                             int style,float stroke,int cap,int join,float miter,bool aa);

#endif /* LUNARIA_DVM_LIB_H */
