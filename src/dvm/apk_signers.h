/* MPL-2.0. APK signing certificate metadata, using the existing OpenSSL. */
#pragma once
#include <openssl/x509.h>

/* 1: certificates read; 0: no supported scheme; -1: malformed or unreadable.
 * Returned stacks own their X509 objects. History is oldest-first, including
 * the current certificate, and is NULL unless v3 records key rotation.
 * This reads installed-package metadata; it is not an APK installation verifier. */
int dvm_apk_signers_read(const char *path, unsigned sdk,
                         STACK_OF(X509) **current, STACK_OF(X509) **history);
