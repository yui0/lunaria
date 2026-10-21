/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNARIA_LOG_POLICY_H
#define LUNARIA_LOG_POLICY_H
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* liblog property values use their first character; literal false means
 * SILENT, whereas true is not a priority. */
static inline int lunaria_log_level(const char *value)
{
   if(!value || !*value) return -1;
   if(!strcasecmp(value,"false")) return 8;
   if(!strcasecmp(value,"true")) return -1;
   switch(toupper((unsigned char)*value)) {
      case 'V': return 2; case 'D': return 3; case 'I': return 4;
      case 'W': return 5; case 'E': return 6; case 'A': case 'F': return 7;
      case 'S': return 8; default: return -1;
   }
}

/* Match Android 12 liblog's actual property lookup (persist before volatile
 * at each scope). A nonempty invalid tag value falls back to global values.
 * Use the shared device property provider so Java and JNI see the same state. */
static inline bool lunaria_log_is_loggable(const char *tag,int priority,
                                          const char *(*property)(const char *))
{
   int level=-1;
   if(tag && *tag) {
      const char prefix[]="persist.log.tag.";
      size_t len=strlen(tag);
      char local[128],*key=NULL;
      if(len<=SIZE_MAX-sizeof prefix) {
         size_t size=sizeof prefix+len;
         key=size<=sizeof local ? local : malloc(size);
         if(key) {
            memcpy(key,prefix,sizeof prefix-1);
            memcpy(key+sizeof prefix-1,tag,len+1);
            const char *value=property(key);
            if(!value || !*value) value=property(key+8);
            level=lunaria_log_level(value);
            if(key!=local) free(key);
         }
      }
   }
   if(level<0) {
      const char *value=property("persist.log.tag");
      if(!value || !*value) value=property("log.tag");
      level=lunaria_log_level(value);
   }
   return priority>=(level<0 ? 4 : level);
}

/* logd's main buffer.  Every android.util.Log call lands here as well as on
 * stderr, so `logcat -d` -- which crash reporters run and attach -- has the
 * app's own log to dump.  Lines are kept in logcat's threadtime format. */
void lunaria_logbuf_write(char priority,const char *tag,const char *message);
/* The newest lines that fit in `capacity`-1 bytes, oldest first, whole lines
 * only; NUL-terminated.  Returns the length. */
size_t lunaria_logbuf_read(char *out,size_t capacity);
void lunaria_logbuf_clear(void);
#endif
