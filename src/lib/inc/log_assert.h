#ifndef LUNARIA_LOG_ASSERT_H
#define LUNARIA_LOG_ASSERT_H
#include <stdio.h>

/* liblog assertion messages are capped at LOG_BUF_SIZE (1024 bytes).
 * The condition is text, never a printf format string. */
static inline void luna_log_assert_write(const char *condition, const char *tag,
                                         const char *formatted)
{
   char message[1024];
   if (formatted) snprintf(message, sizeof message, "%s", formatted);
   else if (condition)
      snprintf(message, sizeof message, "Assertion failed: %s", condition);
   else snprintf(message, sizeof message, "%s", "Unspecified assertion failed");
   fprintf(stderr, "%s\n", message);
   fprintf(stderr, "[android_log p=7 t=%s] %s\n", tag ? tag : "?", message);
   fflush(stderr);
}
#endif
