
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "log_assert.h"

_Noreturn void
__android_log_assert(const char *condition, const char *tag, const char *fmt, ...)
{
   char message[1024];
   if (fmt) {
      va_list args;
      va_start(args, fmt);
      vsnprintf(message, sizeof message, fmt, args);
      va_end(args);
   }
   luna_log_assert_write(condition, tag, fmt ? message : NULL);
   abort();
}

int
__android_log_vprint(int prio, const char *tag,  const char *fmt, va_list ap)
{
   int ret = printf("(%d) %s :: ", prio, tag);
   ret += vprintf(fmt, ap);
   ret += (puts("") != EOF);
   return ret;
}

int
__android_log_print(int prio, const char *tag,  const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   int ret = __android_log_vprint(prio, tag, fmt, ap);
   va_end(ap);
   return ret;
}

int
__android_log_write(int prio, const char *tag, const char *text)
{
   return __android_log_print(prio, tag, "%s", text);
}
