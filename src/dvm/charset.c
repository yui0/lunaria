/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "charset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

#define JCS_FFFD 0xfffdu

/* --- names --------------------------------------------------------------- */

bool jcs_name_legal(const char *name)
{
   if (!name || !name[0]) return false;
   for (const char *p = name; *p; ++p) {
      const char c = *p;
      const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9');
      if (p == name ? !alnum : !(alnum || strchr("-+:_.", c))) return false;
   }
   return true;
}

/* The charsets done here, with the aliases java.nio knows them by. */
static const struct {
   enum jcs_kind kind;
   const char *name;
   const char *aliases[14];
} g_builtin[] = {
   { JCS_UTF8, "UTF-8", { "utf8", "unicode-1-1-utf-8", NULL } },
   { JCS_LATIN1, "ISO-8859-1",
     { "iso8859_1", "iso_8859_1", "iso8859-1", "iso-8859-1:1987",
       "iso_8859-1:1987", "iso_8859-1", "iso-ir-100", "latin1", "l1",
       "ibm819", "cp819", "csisolatin1", "819", NULL } },
   { JCS_ASCII, "US-ASCII",
     { "ascii", "iso646-us", "us", "cp367", "ibm367", "csascii", "646",
       "iso_646.irv:1983", "ansi_x3.4-1968", "ansi_x3.4-1986",
       "iso-ir-6", "default", NULL } },
   { JCS_UTF16, "UTF-16", { "utf16", "utf_16", "unicode", "unicodebig", NULL } },
   { JCS_UTF16BE, "UTF-16BE",
     { "utf_16be", "iso-10646-ucs-2", "x-utf-16be", "unicodebigunmarked",
       NULL } },
   { JCS_UTF16LE, "UTF-16LE",
     { "utf_16le", "x-utf-16le", "unicodelittleunmarked", NULL } },
};

void jcs_default(struct jcs *out)
{
   out->kind = JCS_UTF8;
   strcpy(out->name, "UTF-8");
}

struct native_converter;
static struct native_converter *native_open(const char *, bool);
static void native_close(struct native_converter *);
#ifdef _WIN32
static unsigned native_page(const char *, const char **);
#endif

bool jcs_lookup(const char *name, struct jcs *out)
{
   if (!jcs_name_legal(name)) return false;
   for (size_t i = 0; i < sizeof g_builtin / sizeof g_builtin[0]; ++i) {
      bool hit = !strcasecmp(name, g_builtin[i].name);
      for (size_t a = 0; !hit && g_builtin[i].aliases[a]; ++a)
         hit = !strcasecmp(name, g_builtin[i].aliases[a]);
      if (hit) {
         out->kind = g_builtin[i].kind;
         snprintf(out->name, sizeof out->name, "%s", g_builtin[i].name);
         return true;
      }
   }
   struct native_converter *c = native_open(name, true);
   if (!c) return false;
   out->kind = JCS_NATIVE;
#ifdef _WIN32
   const char *canonical = name;
   (void)native_page(name, &canonical);
   snprintf(out->name, sizeof out->name, "%s", canonical);
#else
   static const char *const names[] = {"Shift_JIS", "GBK", "GB18030", "Big5", "EUC-JP", "EUC-KR", "KOI8-R", "KOI8-U"};
   const char *canonical = name;
   if (!strcasecmp(name,"sjis") || !strcasecmp(name,"shift-jis")) canonical="Shift_JIS";
   for (size_t i=0;i<sizeof names/sizeof names[0];i++)
      if (!strcasecmp(name,names[i])) canonical=names[i];
   snprintf(out->name, sizeof out->name, "%s", canonical);
#endif
   native_close(c);
   return true;
}

/* --- WTF-8 <-> UTF-16 ---------------------------------------------------- */

static size_t put_cp(char *o, uint32_t cp)
{
   if (cp < 0x80) { o[0] = (char)cp; return 1; }
   if (cp < 0x800) {
      o[0] = (char)(0xc0 | (cp >> 6));
      o[1] = (char)(0x80 | (cp & 0x3f));
      return 2;
   }
   if (cp < 0x10000) {
      o[0] = (char)(0xe0 | (cp >> 12));
      o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
      o[2] = (char)(0x80 | (cp & 0x3f));
      return 3;
   }
   o[0] = (char)(0xf0 | (cp >> 18));
   o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
   o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
   o[3] = (char)(0x80 | (cp & 0x3f));
   return 4;
}

size_t jcs_utf16_to_wtf8(const uint16_t *u, size_t n, char *out)
{
   size_t w = 0;
   for (size_t i = 0; i < n; ++i) {
      uint32_t c = u[i];
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < n &&
          u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
         c = 0x10000u + ((c - 0xd800u) << 10) + (u[i + 1] - 0xdc00u);
         ++i;
      }
      w += put_cp(out + w, c);
   }
   return w;
}

/* One code point of WTF-8 at s (at most n bytes): the code point, and the
 * bytes it took in *w.  A byte that starts nothing valid is U+FFFD. */
static uint32_t wtf8_cp(const unsigned char *s, size_t n, size_t *w)
{
   unsigned char b = s[0];
   size_t need;
   uint32_t cp;
   if (b < 0x80) { *w = 1; return b; }
   if ((b & 0xe0) == 0xc0) { need = 1; cp = b & 0x1f; }
   else if ((b & 0xf0) == 0xe0) { need = 2; cp = b & 0x0f; }
   else if ((b & 0xf8) == 0xf0) { need = 3; cp = b & 0x07; }
   else { *w = 1; return JCS_FFFD; }
   if (need >= n) { *w = 1; return JCS_FFFD; }
   for (size_t k = 1; k <= need; ++k) {
      if ((s[k] & 0xc0) != 0x80) { *w = 1; return JCS_FFFD; }
      cp = (cp << 6) | (s[k] & 0x3f);
   }
   *w = need + 1;
   return cp;
}

size_t jcs_wtf8_to_utf16(const char *s, size_t n, uint16_t *out)
{
   const unsigned char *p = (const unsigned char *)s;
   size_t i = 0, k = 0;
   while (i < n) {
      size_t w;
      uint32_t cp = wtf8_cp(p + i, n - i, &w);
      i += w;
      if (cp >= 0x10000) {
         out[k++] = (uint16_t)(0xd800u + ((cp - 0x10000u) >> 10));
         out[k++] = (uint16_t)(0xdc00u + ((cp - 0x10000u) & 0x3ffu));
      } else {
         out[k++] = (uint16_t)cp;
      }
   }
   return k;
}

/* --- UTF-8, the maximal-subpart way -------------------------------------- */

/* For a lead byte: how many continuation bytes follow, and the range the
 * first of them must be in (Unicode Table 3-7).  0 continuation bytes and a
 * zero range means the byte cannot start a sequence. */
static int utf8_lead(unsigned b, unsigned *lo, unsigned *hi)
{
   *lo = 0x80;
   *hi = 0xbf;
   if (b >= 0xc2 && b <= 0xdf) return 1;
   if (b == 0xe0) { *lo = 0xa0; return 2; }
   if ((b >= 0xe1 && b <= 0xec) || b == 0xee || b == 0xef) return 2;
   if (b == 0xed) { *hi = 0x9f; return 2; }
   if (b == 0xf0) { *lo = 0x90; return 3; }
   if (b >= 0xf1 && b <= 0xf3) return 3;
   if (b == 0xf4) { *hi = 0x8f; return 3; }
   *lo = *hi = 0;
   return 0;
}

static char *utf8_decode(const uint8_t *in, size_t n, size_t *out_len)
{
   char *out = malloc(n * 3 + 1);
   if (!out) return NULL;
   size_t i = 0, w = 0;
   while (i < n) {
      unsigned b = in[i];
      if (b < 0x80) { out[w++] = (char)b; ++i; continue; }
      unsigned lo, hi;
      int need = utf8_lead(b, &lo, &hi);
      size_t j = i + 1;
      int got = 0;
      while (got < need && j < n && in[j] >= lo && in[j] <= hi) {
         ++got;
         ++j;
         lo = 0x80;
         hi = 0xbf;
      }
      if (need && got == need) {
         memcpy(out + w, in + i, j - i);
         w += j - i;
      } else {
         /* The lead and the valid continuation bytes after it are one
          * maximal ill-formed subpart; the byte that broke it starts over. */
         w += put_cp(out + w, JCS_FFFD);
      }
      i = j;
   }
   out[w] = '\0';
   *out_len = w;
   return out;
}

/* --- UTF-16 --------------------------------------------------------------- */

static char *utf16_decode(const struct jcs *cs, const uint8_t *in, size_t n,
                          size_t *out_len)
{
   bool little = cs->kind == JCS_UTF16LE;
   size_t i = 0;
   if (cs->kind == JCS_UTF16 && n >= 2) {
      if (in[0] == 0xfe && in[1] == 0xff) i = 2;
      else if (in[0] == 0xff && in[1] == 0xfe) { i = 2; little = true; }
   }
   size_t units = (n - i) / 2 + 1;
   uint16_t *u = malloc(units * sizeof *u);
   char *out = malloc(units * 3 + 4);
   if (!u || !out) { free(u); free(out); return NULL; }
   size_t k = 0;
   for (; i + 1 < n; i += 2)
      u[k++] = little ? (uint16_t)(in[i] | in[i + 1] << 8)
                      : (uint16_t)(in[i] << 8 | in[i + 1]);
   /* A surrogate that is not half of a pair is malformed input. */
   for (size_t j = 0; j < k; ++j) {
      const uint16_t c = u[j];
      if (c >= 0xd800 && c <= 0xdbff && j + 1 < k && u[j + 1] >= 0xdc00 &&
          u[j + 1] <= 0xdfff) {
         ++j;
         continue;
      }
      if (c >= 0xd800 && c <= 0xdfff) u[j] = JCS_FFFD;
   }
   if (i < n) u[k++] = JCS_FFFD;   /* an odd trailing byte */
   size_t w = jcs_utf16_to_wtf8(u, k, out);
   free(u);
   out[w] = '\0';
   *out_len = w;
   return out;
}

/* Platform converters only handle charsets outside the built-in UTF family. */
struct native_converter {
#ifdef _WIN32
   unsigned cp;
#else
   iconv_t handle;
#endif
   unsigned char pending[32];
   size_t count;
};
#ifdef _WIN32
static const struct { const char *name; unsigned cp; const char *aliases; } native_pages[] = {
   {"Shift_JIS",932,"sjis|shift-jis|ms932|windows-31j|cp932"},
   {"GBK",936,"cp936|ms936|windows-936"},
   {"GB18030",54936,"gb18030"}, {"GB2312",936,"euc-cn|gb2312"},
   {"Big5",950,"big-5|cp950"}, {"EUC-KR",51949,"euckr|euc_kr|ks_c_5601-1987"},
   {"EUC-JP",51932,"eucjp|euc_jp"},
   {"KOI8-R",20866,"koi8r"}, {"KOI8-U",21866,"koi8u"},
   {"ISO-8859-2",28592,"iso8859-2|latin2"}, {"ISO-8859-5",28595,"iso8859-5"},
   {"ISO-8859-7",28597,"iso8859-7"}, {"ISO-8859-9",28599,"iso8859-9|latin5"},
   {"ISO-8859-15",28605,"iso8859-15|latin9"},
   {"windows-1250",1250,"cp1250"}, {"windows-1251",1251,"cp1251"},
   {"windows-1252",1252,"cp1252"}, {"windows-1253",1253,"cp1253"},
   {"windows-1254",1254,"cp1254"}, {"windows-1255",1255,"cp1255"},
   {"windows-1256",1256,"cp1256"}, {"windows-1257",1257,"cp1257"},
   {"windows-1258",1258,"cp1258"},
};
static unsigned native_page(const char *name,const char **canonical)
{
   for(size_t i=0;i<sizeof native_pages/sizeof native_pages[0];i++){
      bool match=!strcasecmp(name,native_pages[i].name);
      const char *p=native_pages[i].aliases;
      while(!match&&*p){const char *e=strchr(p,'|');size_t n=e?(size_t)(e-p):strlen(p);
         match=strlen(name)==n&&!strncasecmp(p,name,n);p=e?e+1:p+n;}
      if(match&&IsValidCodePage(native_pages[i].cp)){*canonical=native_pages[i].name;return native_pages[i].cp;}
   }return 0;
}
#endif
static struct native_converter *native_open(const char *name,bool decoding)
{
   struct native_converter *c=calloc(1,sizeof *c);if(!c)return NULL;
#ifdef _WIN32
   (void)decoding;const char *canon=NULL;c->cp=native_page(name,&canon);if(!c->cp){free(c);return NULL;}
#else
   c->handle=iconv_open(decoding?"UTF-16LE":name,decoding?name:"UTF-16LE");
   if(c->handle==(iconv_t)-1){free(c);return NULL;}
#endif
   return c;
}
static void native_close(struct native_converter *c)
{
   if(!c)return;
#ifndef _WIN32
   iconv_close(c->handle);
#endif
   free(c);
}
/* The pending bytes belong to the decoder, including across short reads. */
static size_t native_byte(struct native_converter *c,int byte,uint16_t out[16])
{
   if(byte>=0&&c->count<sizeof c->pending)c->pending[c->count++]=(unsigned char)byte;
   if(!c->count)return 0;
#ifdef _WIN32
   size_t need=1;
   if(c->cp==51932){if(c->pending[0]==0x8f)need=3;else if(c->pending[0]>=0x80)need=2;}
   else if(c->cp==51949&&c->pending[0]>=0x80)need=2;
   else if(c->cp==54936&&c->pending[0]>=0x81&&c->pending[0]<=0xfe){
      need=2;if(c->count>=2&&c->pending[1]>='0'&&c->pending[1]<='9')need=4;
   }else if(IsDBCSLeadByteEx(c->cp,c->pending[0]))need=2;
   if(c->count<need&&byte>=0)return 0;
   int n=c->count>=need?MultiByteToWideChar(c->cp,MB_ERR_INVALID_CHARS,(const char *)c->pending,(int)need,(wchar_t *)out,16):0;
   if(!n){out[0]=JCS_FFFD;n=1;need=1;}
   memmove(c->pending,c->pending+need,c->count-need);c->count-=need;
   return (size_t)n;
#else
   char *src=(char *)c->pending,*dst=(char *)out;size_t left=c->count,space=32;
   errno=0;size_t result=iconv(c->handle,&src,&left,&dst,&space);
   size_t consumed=c->count-left,w=(32-space)/2;
   memmove(c->pending,src,left);c->count=left;
   if(result==(size_t)-1&&errno!=E2BIG&&!(errno==EINVAL&&byte>=0)){
      if(c->count){memmove(c->pending,c->pending+1,--c->count);out[w++]=JCS_FFFD;}
   }
   (void)consumed;return w;
#endif
}
static uint8_t *native_encode(const char *name,const uint16_t *u,size_t n,size_t *len)
{
   if(n>(SIZE_MAX-32)/8)return NULL;
   struct native_converter *c=native_open(name,false);if(!c)return NULL;
   size_t cap=n*8+32;uint8_t *out=malloc(cap);if(!out){native_close(c);return NULL;}
#ifdef _WIN32
   int k=WideCharToMultiByte(c->cp,c->cp==54936?0:WC_NO_BEST_FIT_CHARS,(const wchar_t *)u,(int)n,(char *)out,(int)cap,c->cp==54936?NULL:"?",NULL);
   if(n&&!k){free(out);out=NULL;}else *len=(size_t)k;
#else
   char *src=(char *)u,*dst=(char *)out;size_t left=n*2,space=cap;
   while(left){errno=0;size_t r=iconv(c->handle,&src,&left,&dst,&space);
      if(r!=(size_t)-1)break;
      if(errno!=EILSEQ&&errno!=EINVAL){free(out);out=NULL;break;}
      size_t skip=left>=2?2:left;
      if(left>=4){uint16_t hi=(unsigned char)src[0]|((unsigned char)src[1]<<8),lo=(unsigned char)src[2]|((unsigned char)src[3]<<8);
         if(hi>=0xd800&&hi<=0xdbff&&lo>=0xdc00&&lo<=0xdfff)skip=4;}
      src+=skip;left-=skip;
      char repl[2]={'?',0},*rptr=repl;size_t rleft=2;
      if(iconv(c->handle,&rptr,&rleft,&dst,&space)==(size_t)-1){free(out);out=NULL;break;}
   }
   if(out){(void)iconv(c->handle,NULL,NULL,&dst,&space);*len=cap-space;}
#endif
   native_close(c);return out;
}
static char *native_decode(const struct jcs *cs,const uint8_t *in,size_t n,size_t *len)
{
   struct native_converter *c=native_open(cs->name,true);if(!c)return NULL;
   if(n>(SIZE_MAX-4)/6){native_close(c);return NULL;}
   char *s=malloc(n*6+4);size_t w=0;if(s){
      for(size_t i=0;i<n;i++){uint16_t u[16];size_t k=native_byte(c,in[i],u);w+=jcs_utf16_to_wtf8(u,k,s+w);}
      while(c->count){uint16_t u[16];size_t k=native_byte(c,-1,u);w+=jcs_utf16_to_wtf8(u,k,s+w);}
      s[w]=0;*len=w;
   }native_close(c);return s;
}

char *jcs_decode(const struct jcs *cs, const uint8_t *in, size_t n,
                 size_t *out_len)
{
   switch (cs->kind) {
   case JCS_UTF8:
      return utf8_decode(in, n, out_len);
   case JCS_LATIN1:
   case JCS_ASCII: {
      char *out = malloc(n * 3 + 1);
      if (!out) return NULL;
      size_t w = 0;
      for (size_t i = 0; i < n; ++i) {
         uint32_t c = in[i];
         if (cs->kind == JCS_ASCII && c >= 0x80) c = JCS_FFFD;
         w += put_cp(out + w, c);
      }
      out[w] = '\0';
      *out_len = w;
      return out;
   }
   case JCS_UTF16:
   case JCS_UTF16BE:
   case JCS_UTF16LE:
      return utf16_decode(cs, in, n, out_len);
   case JCS_NATIVE: {
      char *r = native_decode(cs, in, n, out_len);
      /* A converter that cannot be opened now could when looked up; the
       * bytes still become text rather than nothing. */
      return r ? r : utf8_decode(in, n, out_len);
   }
   }
   return NULL;
}

uint8_t *jcs_encode(const struct jcs *cs, const char *wtf8, size_t n,
                    size_t *out_len)
{
   const unsigned char *p = (const unsigned char *)wtf8;
   if (cs->kind == JCS_UTF8) {
      /* Our storage is UTF-8 already, except that a lone surrogate is not
       * encodable: it becomes '?'. */
      uint8_t *out = malloc(n + 1);
      if (!out) return NULL;
      size_t i = 0, w = 0;
      while (i < n) {
         size_t len;
         uint32_t cp = wtf8_cp(p + i, n - i, &len);
         if (cp >= 0xd800 && cp <= 0xdfff) out[w++] = '?';
         else { memcpy(out + w, p + i, len); w += len; }
         i += len;
      }
      *out_len = w;
      return out;
   }
   if (cs->kind == JCS_LATIN1 || cs->kind == JCS_ASCII) {
      const uint32_t max = cs->kind == JCS_ASCII ? 0x7f : 0xff;
      uint8_t *out = malloc(n + 1);
      if (!out) return NULL;
      size_t i = 0, w = 0;
      while (i < n) {
         size_t len;
         uint32_t cp = wtf8_cp(p + i, n - i, &len);
         out[w++] = cp <= max ? (uint8_t)cp : '?';
         i += len;
      }
      *out_len = w;
      return out;
   }
   uint16_t *u = malloc((n + 1) * sizeof *u);
   if (!u) return NULL;
   size_t k = jcs_wtf8_to_utf16(wtf8, n, u);
   uint8_t *out = NULL;
   if (cs->kind != JCS_NATIVE) {
      /* UTF-16 family.  "UTF-16" writes a big-endian byte-order mark; an
       * unpaired surrogate is unmappable and becomes '?'. */
      const bool little = cs->kind == JCS_UTF16LE;
      const bool bom = cs->kind == JCS_UTF16;
      out = malloc(k * 2 + 2);
      if (out) {
         size_t w = 0;
         if (bom) { out[w++] = 0xfe; out[w++] = 0xff; }
         for (size_t i = 0; i < k; ++i) {
            uint16_t c = u[i];
            const bool paired =
               (c >= 0xd800 && c <= 0xdbff && i + 1 < k && u[i + 1] >= 0xdc00 &&
                u[i + 1] <= 0xdfff) ||
               (c >= 0xdc00 && c <= 0xdfff && i > 0 && u[i - 1] >= 0xd800 &&
                u[i - 1] <= 0xdbff);
            if (c >= 0xd800 && c <= 0xdfff && !paired) c = '?';
            out[w++] = little ? (uint8_t)c : (uint8_t)(c >> 8);
            out[w++] = little ? (uint8_t)(c >> 8) : (uint8_t)c;
         }
         *out_len = w;
      }
      free(u);
      return out;
   }
   for (size_t i = 0; i < k; i++) {
      if (u[i] >= 0xd800 && u[i] <= 0xdbff) {
         if (i + 1 < k && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) i++;
         else u[i] = '?';
      } else if (u[i] >= 0xdc00 && u[i] <= 0xdfff) u[i] = '?';
   }
   out = native_encode(cs->name, u, k, out_len);
   free(u);
   return out;
}

/* --- streaming decoder ---------------------------------------------------- */

struct jcs_decoder {
   struct jcs cs;
   struct native_converter *conv; /* JCS_NATIVE */
   uint16_t q[16];            /* decoded units not yet handed out */
   int qh, qn;
   int held;                  /* a byte read ahead and not consumed, or -3 */
   int held_unit;             /* UTF-16: a unit read ahead, or -1 */
   bool bom_done;             /* JCS_UTF16: the byte-order mark was looked for */
   bool little;
   bool done;
};

struct jcs_decoder *jcs_decoder_new(const struct jcs *cs)
{
   struct jcs_decoder *d = calloc(1, sizeof *d);
   if (!d) return NULL;
   d->cs = *cs;
   d->held = -3;
   d->held_unit = -1;
   d->little = cs->kind == JCS_UTF16LE;
   if (cs->kind == JCS_NATIVE) {
      d->conv = native_open(cs->name, true);
      if (!d->conv) jcs_default(&d->cs);
   }
   return d;
}

void jcs_decoder_free(struct jcs_decoder *d)
{
   if (!d) return;
   if (d->conv) native_close(d->conv);
   free(d);
}

static void dq_push(struct jcs_decoder *d, uint16_t u)
{
   if (d->qn < (int)(sizeof d->q / sizeof d->q[0]))
      d->q[(d->qh + d->qn++) % (int)(sizeof d->q / sizeof d->q[0])] = u;
}

static int dq_pop(struct jcs_decoder *d)
{
   int u = d->q[d->qh];
   d->qh = (d->qh + 1) % (int)(sizeof d->q / sizeof d->q[0]);
   --d->qn;
   return u;
}

static int dec_byte(struct jcs_decoder *d, int (*next_byte)(void *), void *ctx)
{
   if (d->held != -3) {
      int b = d->held;
      d->held = -3;
      return b;
   }
   return next_byte(ctx);
}

static void push_cp(struct jcs_decoder *d, uint32_t cp)
{
   if (cp >= 0x10000) {
      dq_push(d, (uint16_t)(0xd800u + ((cp - 0x10000u) >> 10)));
      dq_push(d, (uint16_t)(0xdc00u + ((cp - 0x10000u) & 0x3ffu)));
   } else {
      dq_push(d, (uint16_t)cp);
   }
}

/* The next UTF-16 unit of a UTF-16 byte stream: -1 at the end, -2 when
 * reading failed.  An odd trailing byte is one malformed unit, U+FFFD. */
static int dec_unit16(struct jcs_decoder *d, int (*next_byte)(void *), void *ctx)
{
   if (d->held_unit >= 0) {
      int u = d->held_unit;
      d->held_unit = -1;
      return u;
   }
   for (;;) {
      int b0 = dec_byte(d, next_byte, ctx);
      if (b0 < 0) return b0;
      int b1 = dec_byte(d, next_byte, ctx);
      if (b1 == -2) return -2;
      if (b1 < 0) return JCS_FFFD;
      if (d->cs.kind == JCS_UTF16 && !d->bom_done) {
         d->bom_done = true;
         if (b0 == 0xfe && b1 == 0xff) continue;
         if (b0 == 0xff && b1 == 0xfe) { d->little = true; continue; }
      }
      return d->little ? (b0 | b1 << 8) : (b0 << 8 | b1);
   }
}

/* Decodes at least one more unit into the queue.  Returns 0, -1 at the end
 * of the input, -2 when reading failed. */
static int dec_fill(struct jcs_decoder *d, int (*next_byte)(void *), void *ctx)
{
   if (d->done) return -1;
   switch (d->cs.kind) {
   case JCS_UTF8: {
      int b = dec_byte(d, next_byte, ctx);
      if (b < 0) { if (b == -1) d->done = true; return b; }
      if (b < 0x80) { dq_push(d, (uint16_t)b); return 0; }
      unsigned lo, hi;
      int need = utf8_lead((unsigned)b, &lo, &hi);
      uint32_t cp = need == 1 ? (b & 0x1fu) : need == 2 ? (b & 0x0fu) : (b & 0x07u);
      for (int k = 0; k < need; ++k) {
         int c = dec_byte(d, next_byte, ctx);
         if (c == -2) return -2;
         if (c < 0 || (unsigned)c < lo || (unsigned)c > hi) {
            d->held = c;   /* not part of this sequence: it starts the next */
            need = -1;
            break;
         }
         cp = (cp << 6) | ((unsigned)c & 0x3fu);
         lo = 0x80;
         hi = 0xbf;
      }
      push_cp(d, need > 0 ? cp : JCS_FFFD);
      return 0;
   }
   case JCS_LATIN1:
   case JCS_ASCII: {
      int b = dec_byte(d, next_byte, ctx);
      if (b < 0) { if (b == -1) d->done = true; return b; }
      dq_push(d, d->cs.kind == JCS_ASCII && b >= 0x80 ? JCS_FFFD : (uint16_t)b);
      return 0;
   }
   case JCS_UTF16:
   case JCS_UTF16BE:
   case JCS_UTF16LE: {
      int u = dec_unit16(d, next_byte, ctx);
      if (u < 0) return u;
      if (u >= 0xd800 && u <= 0xdbff) {
         int l = dec_unit16(d, next_byte, ctx);
         if (l == -2) return -2;
         if (l >= 0xdc00 && l <= 0xdfff) {
            dq_push(d, (uint16_t)u);
            dq_push(d, (uint16_t)l);
         } else {
            /* A lone high surrogate is malformed; the unit after it is
             * decoded on its own next time. */
            dq_push(d, JCS_FFFD);
            if (l >= 0) d->held_unit = l;
         }
         return 0;
      }
      dq_push(d, u >= 0xdc00 && u <= 0xdfff ? JCS_FFFD : (uint16_t)u);
      return 0;
   }
   case JCS_NATIVE: {
      /* One byte at a time: the converter keeps a partial multi-byte
       * sequence in its own state until the byte that completes it. */
      for (;;) {
         int b = dec_byte(d, next_byte, ctx);
         if (b == -2) return -2;
         uint16_t units[16];
         size_t count = native_byte(d->conv, b, units);
         for (size_t i=0;i<count;i++) dq_push(d, units[i]);
         if (b < 0 && d->conv->count) continue;
         if (b < 0) {
            d->done = true;
            return d->qn ? 0 : -1;
         }
         if (d->qn) return 0;
      }
   }
   }
   return -1;
}

int jcs_decoder_next(struct jcs_decoder *d, int (*next_byte)(void *ctx),
                     void *ctx)
{
   if (!d->qn) {
      int r = dec_fill(d, next_byte, ctx);
      if (r < 0 && !d->qn) return r;
   }
   return dq_pop(d);
}
