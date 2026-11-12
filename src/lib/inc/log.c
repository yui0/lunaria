/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

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

/* Buffer selection is retained in the host log, as on the guest bridge. */
int __android_log_buf_print(int buffer,int prio,const char *tag,const char *fmt,...)
{
  char message[1024];
  va_list args;
  va_start(args,fmt);
  vsnprintf(message,sizeof message,fmt,args);
  va_end(args);
  fprintf(stderr,"[android_log b=%d p=%d t=%s] %s\n",buffer,prio,tag ? tag : "?",message);
  return 1;
}
int __android_log_buf_write(int buffer,int prio,const char *tag,const char *text)
{
  fprintf(stderr,"[android_log b=%d p=%d t=%s] %s\n",buffer,prio,tag ? tag : "?",text ? text : "");
  return 1;
}
