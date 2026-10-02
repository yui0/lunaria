/* MPL-2.0. Portable Unicode operations; data license: LICENSE.unicode. */
#ifndef LUNARIA_UNICODE_H
#define LUNARIA_UNICODE_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Forms match java.text.Normalizer.Form ordinals: NFD, NFC, NFKD, NFKC. */
char *luna_unicode_normalize(const char *utf8, int form);
char *luna_idn_convert(const char *utf8, unsigned flags, bool to_ascii);
int32_t luna_uc_compose(int32_t first, int32_t second);
int luna_uc_type(int32_t cp);
int32_t luna_uc_upper(int32_t cp);
int32_t luna_uc_lower(int32_t cp);
enum { LUNA_UC_ALPHABETIC, LUNA_UC_IDEOGRAPHIC, LUNA_UC_LOWERCASE, LUNA_UC_UPPERCASE, LUNA_UC_ALNUM };
bool luna_uc_property(int32_t cp, int property);
bool luna_uc_space(int32_t cp);
bool luna_uc_whitespace(int32_t cp);
bool luna_uc_spacechar(int32_t cp);
bool luna_uc_blank(int32_t cp);
bool luna_uc_digit(int32_t cp);
bool luna_uc_alpha(int32_t cp);
bool luna_uc_alnum(int32_t cp);
bool luna_uc_defined(int32_t cp);
bool luna_uc_uppercase(int32_t cp);
bool luna_uc_lowercase(int32_t cp);
bool luna_uc_control(int32_t cp);
bool luna_uc_graph(int32_t cp);
bool luna_uc_print(int32_t cp);
bool luna_uc_punct(int32_t cp);
bool luna_uc_xdigit(int32_t cp);
#ifdef __cplusplus
}
#endif
#endif
