/* Host-independent parser for the IEEE-754 binary128 value used by AArch64
 * long double.  Some hosts (notably arm64 macOS) have only a 64-bit host long
 * double, so converting through a host floating type loses both range and
 * precision. */
#ifndef LUNARIA_BINARY128_H
#define LUNARIA_BINARY128_H

#include <stdint.h>
#include <wchar.h>

typedef struct luna_binary128 {
   uint64_t lo;
   uint64_t hi;
} luna_binary128;

#ifdef __cplusplus
extern "C" {
#endif

luna_binary128 luna_binary128_from_string(const char *s, char **end);
luna_binary128 luna_binary128_from_wstring(const wchar_t *s, wchar_t **end);

#ifdef __cplusplus
}
#endif

#endif
