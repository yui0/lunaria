/* MPL-2.0. APKPure discovery and streaming downloads, using the host HTTP client. */
#ifndef LUNARIA_APKPURE_H
#define LUNARIA_APKPURE_H
#include <stddef.h>
#include <stdint.h>
#define APKPURE_RESULTS 8
struct apkpure_result { char package[256], title[256]; };
/* Return nonzero from progress to cancel. Called on the worker, never the UI. */
typedef int (*apkpure_progress)(void *, uint64_t, uint64_t);
int apkpure_package_valid(const char *name);
int apkpure_search_url(const char *query, char *url, size_t capacity);
int apkpure_parse_results(const char *html, struct apkpure_result *out, size_t capacity);
int apkpure_search(const char *query, struct apkpure_result *out, size_t capacity,
                   char *error, size_t error_size, apkpure_progress progress, void *context);
int apkpure_download(const char *package, const char *directory, char *path, size_t capacity,
                     char *error, size_t error_size, apkpure_progress progress, void *context);
#endif
