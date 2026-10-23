/* MPL-2.0. Android settings shared by native JNI and dex callers. */
#include "jvm.h"
#include "../lunaria_file.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t identity_once = PTHREAD_ONCE_INIT;
static char android_id[17];

static void init_identity(void)
{
   /* Preserve Lunaria's existing per-host, per-package identity. This is an
    * emulator identity, not Android's signing-key based SSAID algorithm. */
   uint64_t h = 1469598103934665603ull;
   char seed[512] = "";
   FILE *f = luna_file_fopen("/etc/machine-id", "r");
   if (f) {
      if (!fgets(seed, sizeof seed, f)) seed[0] = 0;
      fclose(f);
   }
   const char *package = getenv("ANDROID_PACKAGE_NAME");
   snprintf(seed + strlen(seed), sizeof seed - strlen(seed), "|%s",
            package ? package : "");
   for (const char *p = seed; *p; ++p) {
      h ^= (unsigned char)*p;
      h *= 1099511628211ull;
   }
   snprintf(android_id, sizeof android_id, "%016llx", (unsigned long long)h);
}

const char *lunaria_android_setting(const char *table, const char *name)
{
   if (!table || !name || strcmp(table, "secure") || strcmp(name, "android_id"))
      return NULL;
   pthread_once(&identity_once, init_identity);
   return android_id;
}
