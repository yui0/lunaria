#include "log_policy.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGBUF_BYTES (256u * 1024u)
static struct {
   char *ring;
   size_t head, length;     /* oldest byte, bytes held */
   pthread_mutex_t lock;
} g_logbuf = { NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER };

void lunaria_logbuf_write(char priority, const char *tag, const char *message)
{
   if (!message) message = "";
   if (!tag) tag = "?";
   struct timespec now;
   if (timespec_get(&now, TIME_UTC) != TIME_UTC) now = (struct timespec){ 0 };
   pthread_mutex_lock(&g_logbuf.lock);
   struct tm local = { 0 };
   const time_t seconds = now.tv_sec;
   const struct tm *shared = localtime(&seconds);   /* guarded by the lock */
   if (shared) local = *shared;
   if (!g_logbuf.ring) g_logbuf.ring = malloc(LOGBUF_BYTES);
   if (!g_logbuf.ring) { pthread_mutex_unlock(&g_logbuf.lock); return; }
   /* One logcat line per line of message, as logd splits them. */
   for (const char *line = message; ; ) {
      const char *end = strchr(line, '\n');
      const size_t length = end ? (size_t)(end - line) : strlen(line);
      char head[384];
      const int n = snprintf(head, sizeof head, "%02d-%02d %02d:%02d:%02d.%03d %5d %5d %c %s: ",
                             local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                             local.tm_sec, (int)(now.tv_nsec / 1000000), (int)getpid(),
                             (int)getpid(), priority, tag);
      const size_t total = (size_t)n + length + 1;
      if (n > 0 && (size_t)n < sizeof head && total < LOGBUF_BYTES) {
         while (g_logbuf.length + total > LOGBUF_BYTES) {    /* drop whole oldest lines */
            size_t dropped = 0;
            while (dropped < g_logbuf.length) {
               const char c = g_logbuf.ring[(g_logbuf.head + dropped++) % LOGBUF_BYTES];
               if (c == '\n') break;
            }
            g_logbuf.head = (g_logbuf.head + dropped) % LOGBUF_BYTES;
            g_logbuf.length -= dropped;
         }
         size_t put = (g_logbuf.head + g_logbuf.length) % LOGBUF_BYTES;
         for (int i = 0; i < n; ++i, put = (put + 1) % LOGBUF_BYTES) g_logbuf.ring[put] = head[i];
         for (size_t i = 0; i < length; ++i, put = (put + 1) % LOGBUF_BYTES) g_logbuf.ring[put] = line[i];
         g_logbuf.ring[put] = '\n';
         g_logbuf.length += total;
      }
      if (!end) break;
      line = end + 1;
      if (!*line) break;
   }
   pthread_mutex_unlock(&g_logbuf.lock);
}

size_t lunaria_logbuf_read(char *out, size_t capacity)
{
   if (!out || !capacity) return 0;
   pthread_mutex_lock(&g_logbuf.lock);
   size_t take = g_logbuf.length < capacity - 1 ? g_logbuf.length : capacity - 1;
   size_t from = g_logbuf.head + (g_logbuf.length - take);
   /* Start on a line boundary when the tail was cut. */
   if (take < g_logbuf.length) {
      while (take && g_logbuf.ring[(from + LOGBUF_BYTES - 1) % LOGBUF_BYTES] != '\n') { ++from; --take; }
   }
   for (size_t i = 0; i < take; ++i) out[i] = g_logbuf.ring[(from + i) % LOGBUF_BYTES];
   out[take] = 0;
   pthread_mutex_unlock(&g_logbuf.lock);
   return take;
}

void lunaria_logbuf_clear(void)
{
   pthread_mutex_lock(&g_logbuf.lock);
   g_logbuf.head = g_logbuf.length = 0;
   pthread_mutex_unlock(&g_logbuf.lock);
}

