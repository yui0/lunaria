/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * APK v2/v3 certificate metadata. No new library dependencies.
 * * Format: https://source.android.com/docs/security/features/apksigning/v2
 * *         https://source.android.com/docs/security/features/apksigning/v3
 */
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <zlib.h>
#include "dvm/dvm.h"
#include "lunaria_os.h"
#include <limits.h>
#include <strings.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct bytes {
  const unsigned char *p;
  size_t n;
};
static uint32_t le32(const unsigned char *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p)
{
  return le32(p) | (uint64_t)le32(p + 4) << 32;
}
static int number(struct bytes *b, uint32_t *out)
{
  if (b->n < 4) return 0;
  *out = le32(b->p);
  b->p += 4;
  b->n -= 4;
  return 1;
}
static int slice(struct bytes *b, struct bytes *out)
{
  uint32_t n;
  if (!number(b, &n) || n > INT32_MAX || n > b->n) return 0;
  *out = (struct bytes) {
    b->p, n
  };
  b->p += n;
  b->n -= n;
  return 1;
}
static int read_at(int fd, void *buffer, size_t n, uint64_t offset)
{
  unsigned char *p = buffer;
  while (n) {
    ptrdiff_t got = luna_file_pread(fd, p, n, (int64_t)offset);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return 0;
    p += got;
    n -= (size_t)got;
    offset += (uint64_t)got;
  }
  return 1;
}
static X509 *certificate(struct bytes b)
{
  if (!b.n || b.n > LONG_MAX) return NULL;
  const unsigned char *p = b.p;
  X509 *cert = d2i_X509(NULL, &p, (long)b.n);
  if (cert && p == b.p + b.n) return cert;
  X509_free(cert);
  return NULL;
}

static int rotation(struct bytes b, STACK_OF(X509) **out)
{
  uint32_t version;
  if (!number(&b, &version) || !b.n) return 0;
  STACK_OF(X509) *list = sk_X509_new_null();
  if (!list) return 0;
  while (b.n) {
    struct bytes level, signed_data, der, signature;
    uint32_t algorithm, flags, next_algorithm;
    if (!slice(&b, &level) || !slice(&level, &signed_data) ||
        !number(&level, &flags) || !number(&level, &next_algorithm) ||
        !slice(&level, &signature) || level.n ||
        !slice(&signed_data, &der) || !number(&signed_data, &algorithm) || signed_data.n)
      goto bad;
    X509 *cert = certificate(der);
    if (!cert) goto bad;
    for (int i = 0; i < sk_X509_num(list); ++i)
      if (X509_cmp(cert, sk_X509_value(list, i)) == 0) {
        X509_free(cert);
        goto bad;
      }
    if (!sk_X509_push(list, cert)) {
      X509_free(cert);
      goto bad;
    }
  }
  *out = list;
  return 1;
bad:
  sk_X509_pop_free(list, X509_free);
  return 0;
}

static int scheme(struct bytes b, int v3, unsigned sdk,
                  STACK_OF(X509) **current, STACK_OF(X509) **history)
{
  struct bytes signers;
  if (!slice(&b, &signers) || b.n || !signers.n) return -1;
  STACK_OF(X509) *list = sk_X509_new_null(), *past = NULL;
  if (!list) return -1;
  while (signers.n) {
    struct bytes signer, data, digests, certs, attrs, signatures, key, der;
    uint32_t min = 0, max = UINT32_MAX, signed_min, signed_max;
    if (!slice(&signers, &signer) || !slice(&signer, &data)) goto bad;
    if (v3 && (!number(&signer, &min) || !number(&signer, &max) || min > max)) goto bad;
    if (v3 && (sdk < min || sdk > max)) continue;
    if (!slice(&signer, &signatures) || !signatures.n ||
        !slice(&signer, &key) || !key.n || signer.n ||
        !slice(&data, &digests) || !digests.n || !slice(&data, &certs) || !certs.n)
      goto bad;
    if (v3 && (!number(&data, &signed_min) || !number(&data, &signed_max) ||
               signed_min != min || signed_max != max)) goto bad;
    /* Trailing bytes after the attributes are ignored, as AOSP's verifier does:
     * signers in the wild (Google Play services) append an empty field. */
    if (!slice(&data, &attrs) || !slice(&certs, &der)) goto bad;
    X509 *cert = certificate(der);
    if (!cert) goto bad;
    if (!sk_X509_push(list, cert)) {
      X509_free(cert);
      goto bad;
    }
    /* A chain is one signer, not one Signature per issuer certificate. */
    while (certs.n) {
      if (!slice(&certs, &der)) goto bad;
      X509 *issuer = certificate(der);
      if (!issuer) goto bad;
      X509_free(issuer);
    }
    /* Match the leaf's SPKI to the public key stored by this signer. */
    X509_PUBKEY *pub = X509_get_X509_PUBKEY(cert);
    int key_size = i2d_X509_PUBKEY(pub, NULL);
    unsigned char *encoded = key_size > 0 ? malloc((size_t)key_size) : NULL;
    unsigned char *write = encoded;
    int key_ok = encoded && (size_t)key_size == key.n &&
                 i2d_X509_PUBKEY(pub, &write) == key_size && !memcmp(encoded, key.p, key.n);
    free(encoded);
    if (!key_ok) goto bad;
    while (digests.n || signatures.n) {
      struct bytes digest_record, signature_record, digest, signature;
      uint32_t digest_algorithm, signature_algorithm;
      if (!slice(&digests, &digest_record) || !slice(&signatures, &signature_record) ||
          !number(&digest_record, &digest_algorithm) ||
          !number(&signature_record, &signature_algorithm) ||
          digest_algorithm != signature_algorithm ||
          !slice(&digest_record, &digest) || digest_record.n || !digest.n ||
          !slice(&signature_record, &signature) || signature_record.n || !signature.n)
        goto bad;
    }
    while (attrs.n) {
      struct bytes attr;
      uint32_t id;
      if (!slice(&attrs, &attr) || !number(&attr, &id)) goto bad;
      if (v3 && id == 0x3ba06f8cu) {
        if (past || !rotation(attr, &past) ||
            X509_cmp(sk_X509_value(past, sk_X509_num(past) - 1), cert) != 0) goto bad;
      }
    }
  }
  int count = sk_X509_num(list);
  if (v3 && count > 1) goto bad;
  if (!count) {
    sk_X509_free(list);
    return 0;
  }
  *current = list;
  *history = past;
  return 1;
bad:
  sk_X509_pop_free(list, X509_free);
  sk_X509_pop_free(past, X509_free);
  return -1;
}

int dvm_apk_signers_read(const char *path, unsigned sdk,
                         STACK_OF(X509) **current, STACK_OF(X509) **history)
{
  if (!current || !history) return -1;
  *current = NULL;
  *history = NULL;
  if (!path || !*path) return 0;
  int fd = luna_file_open(path, O_RDONLY | LUNA_FILE_CLOEXEC, 0);
  if (fd < 0) return -1;
  int result = -1;
  unsigned char *tail = NULL, *value = NULL;
  luna_file_info info;
  if (luna_file_fd_info(fd, &info) || info.size < 22) goto done;
  size_t n = info.size < 65557 ? (size_t)info.size : 65557;
  tail = malloc(n);
  if (!tail || !read_at(fd, tail, n, (uint64_t)info.size - n)) goto done;
  size_t eocd;
  for (eocd = n - 22; ; --eocd) {
    if (le32(tail + eocd) == 0x06054b50u &&
        eocd + 22u + ((uint32_t)tail[eocd + 20] | (uint32_t)tail[eocd + 21] << 8) == n)
      break;
    if (!eocd) goto done;
  }
  const unsigned char *end = tail + eocd;
  uint64_t cd = le32(end + 16), cd_size = le32(end + 12);
  uint64_t end_offset = (uint64_t)info.size - n + eocd;
  /* Android APKs use a single-disk ZIP, with central directory adjacent to EOCD. */
  if (end[4] || end[5] || end[6] || end[7] ||
      end[8] != end[10] || end[9] != end[11] || cd + cd_size != end_offset) goto done;
  result = 0;
  unsigned char footer[24], header[12];
  if (cd < 32 || !read_at(fd, footer, sizeof footer, cd - 24) ||
      memcmp(footer + 8, "APK Sig Block 42", 16)) goto done;
  result = -1;
  uint64_t size = le64(footer);
  if (size < 24 || size > INT32_MAX - 8u || size + 8 > cd) goto done;
  uint64_t start = cd - size - 8, stop = cd - 24;
  if (!read_at(fd, header, 8, start) || le64(header) != size) goto done;
  uint64_t offsets[2] = {0, 0};
  size_t lengths[2] = {0, 0};
  for (uint64_t pos = start + 8; pos < stop; ) {
    if (stop - pos < 12 || !read_at(fd, header, sizeof header, pos)) goto done;
    uint64_t length = le64(header);
    if (length < 4 || length > INT32_MAX || length > stop - pos - 8) goto done;
    uint32_t id = le32(header + 8);
    int slot = id == 0x7109871au ? 0 : id == 0xf05368c0u ? 1 : -1;
    if (slot >= 0 && !offsets[slot]) {
      offsets[slot] = pos + 12;
      lengths[slot] = (size_t)length - 4;
    }
    pos += length + 8;
  }
  result = 0;
  for (int slot = sdk >= 28 ? 1 : 0; slot >= 0; --slot) {
    if (!offsets[slot]) continue;
    value = malloc(lengths[slot] ? lengths[slot] : 1);
    if (!value || !read_at(fd, value, lengths[slot], offsets[slot])) {
      result = -1;
      break;
    }
    result = scheme((struct bytes) {
      value, lengths[slot]
    }, slot == 1, sdk, current, history);
    free(value);
    value = NULL;
    if (result) break;
  }
done:
  free(value);
  free(tail);
  luna_fd_close(fd);
  return result;
}

static unsigned le16(const unsigned char *p)
{
  return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/* A META-INF signature block: META-INF/<name>.RSA, .DSA or .EC, directly in
 * META-INF (the JAR signature file's PKCS#7 envelope). */
static int v1_block_name(const unsigned char *name, size_t n)
{
  static const char prefix[] = "META-INF/";
  size_t pl = sizeof prefix - 1;
  if (n <= pl + 3 || memcmp(name, prefix, pl)) return 0;
  for (size_t i = pl; i < n; ++i)
    if (name[i] == '/') return 0;
  const unsigned char *ext = memchr(name + pl, '.', n - pl);
  if (!ext) return 0;
  size_t el = (size_t)(name + n - ext);
  static const char *const exts[] = { ".RSA", ".DSA", ".EC" };
  for (size_t i = 0; i < sizeof exts / sizeof exts[0]; ++i)
    if (el == strlen(exts[i]) && !strncasecmp((const char *)ext, exts[i], el))
      return 1;
  return 0;
}

STACK_OF(X509) *dvm_apk_v1_signers_read(const char *path)
{
  if (!path || !*path) return NULL;
  int fd = luna_file_open(path, O_RDONLY | LUNA_FILE_CLOEXEC, 0);
  if (fd < 0) return NULL;
  unsigned char *tail = NULL, *dir = NULL, *raw = NULL, *blob = NULL;
  STACK_OF(X509) *result = NULL;
  luna_file_info info;
  if (luna_file_fd_info(fd, &info) || info.size < 22) goto done;
  size_t n = info.size < 65557 ? (size_t)info.size : 65557;
  tail = malloc(n);
  if (!tail || !read_at(fd, tail, n, (uint64_t)info.size - n)) goto done;
  size_t eocd;
  for (eocd = n - 22; ; --eocd) {
    if (le32(tail + eocd) == 0x06054b50u &&
        eocd + 22u + le16(tail + eocd + 20) == n)
      break;
    if (!eocd) goto done;
  }
  const unsigned char *end = tail + eocd;
  uint64_t cd = le32(end + 16), cd_size = le32(end + 12);
  unsigned entries = le16(end + 10);
  if (cd_size > 64u * 1024 * 1024 || cd + cd_size > (uint64_t)info.size) goto done;
  dir = malloc(cd_size ? (size_t)cd_size : 1);
  if (!dir || !read_at(fd, dir, (size_t)cd_size, cd)) goto done;

  size_t pos = 0;
  for (unsigned i = 0; i < entries && !result; ++i) {
    if (cd_size - pos < 46 || le32(dir + pos) != 0x02014b50u) break;
    const unsigned char *h = dir + pos;
    unsigned method = le16(h + 10);
    uint32_t csize = le32(h + 20), usize = le32(h + 24);
    unsigned nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32);
    uint32_t local = le32(h + 42);
    if (cd_size - pos - 46 < (uint64_t)nlen + xlen + clen) break;
    int wanted = v1_block_name(h + 46, nlen);
    pos += 46u + nlen + xlen + clen;
    /* A signature block is a few KiB; anything larger is not one. */
    if (!wanted || usize == 0 || usize > 1024 * 1024 || csize > 1024 * 1024 ||
        (method != 0 && method != 8))
      continue;
    unsigned char lh[30];
    if (!read_at(fd, lh, sizeof lh, local) || le32(lh) != 0x04034b50u) continue;
    uint64_t data = (uint64_t)local + 30u + le16(lh + 26) + le16(lh + 28);
    raw = malloc(csize ? csize : 1);
    if (!raw || !read_at(fd, raw, csize, data)) goto done;
    const unsigned char *der = raw;
    size_t der_len = csize;
    if (method == 8) {
      blob = malloc(usize);
      if (!blob) goto done;
      z_stream zs;
      memset(&zs, 0, sizeof zs);
      if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) goto done;
      zs.next_in = raw;
      zs.avail_in = csize;
      zs.next_out = blob;
      zs.avail_out = usize;
      int rc = inflate(&zs, Z_FINISH);
      size_t got = zs.total_out;
      inflateEnd(&zs);
      if (rc != Z_STREAM_END) {
        free(blob);
        blob = NULL;
        free(raw);
        raw = NULL;
        continue;
      }
      der = blob;
      der_len = got;
    }
    PKCS7 *p7 = d2i_PKCS7(NULL, &der, (long)der_len);
    if (p7) {
      STACK_OF(X509) *signers = PKCS7_get0_signers(p7, NULL, 0);
      /* get0_signers allocates the stack but not the certificates. */
      for (int k = 0; signers && k < sk_X509_num(signers); ++k)
        X509_up_ref(sk_X509_value(signers, k));
      if (signers && sk_X509_num(signers) > 0) result = signers;
      else sk_X509_pop_free(signers, X509_free);
      PKCS7_free(p7);
    }
    free(blob);
    blob = NULL;
    free(raw);
    raw = NULL;
  }
done:
  free(blob);
  free(raw);
  free(dir);
  free(tail);
  luna_fd_close(fd);
  return result;
}
