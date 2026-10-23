/* MPL-2.0. java.security, javax.crypto and Android keystore key generation.
 * Implementation section included by dvm_runtime.c in declaration order.
 * Shared helpers and class registration remain in that translation unit. */

/* --- java.security.MessageDigest ---------------------------------------- */

/* A digest is incremental.  This used to accumulate every byte handed to
 * update() into a java byte[] and hash the lot in digest(), which is fine for
 * a token and wrong for a file: the patcher checksums an 8.6 GB pak through
 * this, so the emulator tried to hold 8.6 GB in one array whose length is a
 * signed 32-bit count — the checksum came out wrong and every downloaded pak
 * was rejected as ErrorMD5Mismatch even though its bytes were correct.
 *
 * So carry an EVP_MD_CTX for the object's lifetime instead, in a long field.
 * digest() and reset() end it, and the next update() starts a fresh one, which
 * is exactly what java.security.MessageDigest promises. */
static const EVP_MD *md_type_for(struct dvm *vm, dvm_ref self)
{
   union dvm_value av = { 0 };
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &av);
   const char *alg = av.l ? dvm_string_utf8(vm, av.l) : NULL;
   if (!alg || !*alg) alg = "SHA-256";
   /* Java spells some of these differently from OpenSSL. */
   if (!strcasecmp(alg, "SHA")) alg = "SHA1";
   return EVP_get_digestbyname(alg);
}

static EVP_MD_CTX *md_ctx(struct dvm *vm, dvm_ref self, bool create)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "ctx", "J", &v);
   EVP_MD_CTX *ctx = (EVP_MD_CTX *)(uintptr_t)v.j;
   if (ctx || !create) return ctx;
   const EVP_MD *type = md_type_for(vm, self);
   if (!type) return NULL;
   ctx = EVP_MD_CTX_new();
   if (!ctx) return NULL;
   if (!EVP_DigestInit_ex(ctx, type, NULL)) { EVP_MD_CTX_free(ctx); return NULL; }
   union dvm_value nv = { .j = (int64_t)(uintptr_t)ctx };
   (void)dvm_set_field(vm, self, "ctx", "J", nv);
   return ctx;
}

static void md_ctx_drop(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "ctx", "J", &v);
   if (v.j) EVP_MD_CTX_free((EVP_MD_CTX *)(uintptr_t)v.j);
   union dvm_value z = { .j = 0 };
   (void)dvm_set_field(vm, self, "ctx", "J", z);
   (void)dvm_set_field(vm, self, "fed", "J", z);
}

static bool md_getInstance(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *alg = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   struct dvm_class *c =
      dvm__class_by_desc(vm, "Ljava/security/MessageDigest;");
   dvm_ref d = c ? dvm_new_object(vm, c) : 0;
   if (!d) RETL(0);
   union dvm_value v = { .l = dvm_new_string(vm, alg ? alg : "SHA-256") };
   (void)dvm_set_field(vm, d, "algorithm", "Ljava/lang/String;", v);
   /* Unknown algorithms fail here, not at digest() — that is where the caller
    * is prepared to catch it. */
   if (!md_type_for(vm, d)) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s",
                 alg ? alg : "(null)");
      return false;
   }
   RETL(d);
}

/* The Java overload determines the input type; byte values can also be
 * valid object handles and must never be inspected as references. */
static bool md_feed(struct dvm *vm, dvm_ref self, const uint8_t *src,
                    size_t len, union dvm_value *out)
{
   (void)out;
   EVP_MD_CTX *ctx = md_ctx(vm, self, true);
   if (!ctx) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "digest");
      return false;
   }
   if (!len) RETV();
   /* DigestUpdate does not touch the VM — only the EVP context and the
    * caller's byte[].  Holding the interpreter lock across a multi-megabyte
    * MD5 of a pak (Cross Worlds' patcher) serialises every Java thread and,
    * through JNI, every engine that wants to call into Java.  A real JVM
    * releases its equivalent for native digests; do the same for large
    * updates.  Tiny ones stay locked: the hand-off itself costs more. */
   if (len >= (1u << 16)) {
      unsigned gil = dvm_gil_unlock_all(vm);
      (void)EVP_DigestUpdate(ctx, src, len);
      dvm_gil_relock(vm, gil);
   } else {
      (void)EVP_DigestUpdate(ctx, src, len);
   }
   if (getenv("LUNARIA_TRACE_MD5")) {
      union dvm_value fed = { 0 };
      (void)dvm_get_field(vm, self, "fed", "J", &fed);
      fed.j += (int64_t)len;
      (void)dvm_set_field(vm, self, "fed", "J", fed);
   }
   RETV();
}

static bool md_update_byte(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)nargs;
   const uint8_t byte = (uint8_t)ARG(0).i;
   return md_feed(vm, self, &byte, 1, out);
}

static bool md_update(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   if (!ARG(0).l) {
      dvm__throw(vm, nargs >= 3 ? "java/lang/IllegalArgumentException" :
                 "java/lang/NullPointerException", "null digest input");
      return false;
   }
   struct dvm_object *array = dvm__obj(vm, ARG(0).l);
   if (!array || array->kind != DVM_OBJ_ARRAY || array->elem_kind != 'B') {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "digest input is not byte[]");
      return false;
   }
   const int32_t offset = nargs >= 3 ? args[1].i : 0;
   const int32_t count = nargs >= 3 ? args[2].i : (int32_t)array->length;
   /* MessageDigest's public check precedes Conscrypt's native range check.
    * Use unsigned subtraction for Java's wrapping int arithmetic. */
   if (nargs >= 3 && (int32_t)(array->length - (uint32_t)offset) < count) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "digest input buffer too short");
      return false;
   }
   if (offset < 0 || count < 0 || (uint32_t)offset > array->length ||
       (uint32_t)count > array->length - (uint32_t)offset) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "digest input range");
      return false;
   }
   const uint8_t *data = array->data;
   return md_feed(vm, self, count ? data + offset : data, (size_t)count, out);
}

static bool md_reset(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   md_ctx_drop(vm, self);
   RETV();
}

static bool md_digest(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
   /* digest(byte[]) is update() then digest(). */
   if (nargs >= 1) {
      union dvm_value ignored = { 0 };
      if (!md_update(vm, self, args, 1, &ignored)) return false;
   }
   EVP_MD_CTX *ctx = md_ctx(vm, self, true);   /* digest() of nothing is legal */
   if (!ctx) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "digest");
      return false;
   }
   uint8_t md[EVP_MAX_MD_SIZE];
   unsigned mdlen = 0;
   int ok = EVP_DigestFinal_ex(ctx, md, &mdlen);
   if (getenv("LUNARIA_TRACE_MD5")) {
      union dvm_value fed = { 0 };
      (void)dvm_get_field(vm, self, "fed", "J", &fed);
      char hex[2 * EVP_MAX_MD_SIZE + 1];
      for (unsigned i = 0; i < mdlen; ++i)
         snprintf(hex + 2 * i, 3, "%02x", md[i]);
      fprintf(stderr, "[md5] fed=%lld digest=%s\n", (long long)fed.j,
              ok ? hex : "(failed)");
   }
   md_ctx_drop(vm, self);                      /* next update() starts over */
   if (!ok) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "digest failed");
      return false;
   }
   RETL(make_bytes(vm, md, mdlen));
}

static bool md_getAlgorithm(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &v);
   RETL(v.l);
}

static bool md_getDigestLength(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)args; (void)nargs;
   const EVP_MD *type = md_type_for(vm, self);
   RETI(type ? EVP_MD_get_size(type) : 0);
}

/* The digest holds a host context, not java storage, so it declares only what
 * it needs: the algorithm it was asked for and the context's address. */
static const struct rt_field rt_digest_fields[] = {
   { "algorithm", "Ljava/lang/String;" }, { "ctx", "J" },
   { "fed", "J" }, F_END
};

static const struct rt_method rt_digest[] = {
   SM("getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;",
      md_getInstance),
   SM("getInstance",
      "(Ljava/lang/String;Ljava/lang/String;)Ljava/security/MessageDigest;",
      md_getInstance),
   M("update", "([B)V", md_update),
   M("update", "([BII)V", md_update),
   M("update", "(B)V", md_update_byte),
   M("digest", "()[B", md_digest),
   M("digest", "([B)[B", md_digest),
   M("reset", "()V", md_reset),
   M("getAlgorithm", "()Ljava/lang/String;", md_getAlgorithm),
   M("getDigestLength", "()I", md_getDigestLength),
   M_END,
};

/* DigestInputStream hashes only bytes actually read, never skipped bytes.
 * Reads and inherited stream operations dispatch to the immediate delegate so
 * application-defined streams and nested digest streams keep their semantics. */
static bool digest_in_init(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)nargs; (void)out;
   dvm_set_field(vm, self, "in", "Ljava/io/InputStream;", args[0]);
   dvm_set_field(vm, self, "digest", "Ljava/security/MessageDigest;", args[1]);
   dvm_set_field(vm, self, "on", "Z", (union dvm_value){.i = 1});
   RETV();
}

static bool digest_in_read(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   union dvm_value source = {0}, digest = {0}, enabled = {0}, result = {0};
   dvm_get_field(vm, self, "in", "Ljava/io/InputStream;", &source);
   if (!source.l) {
      dvm__throw(vm, "java/lang/NullPointerException", "input stream");
      return false;
   }
   const char *sig = nargs == 0 ? "()I" : nargs >= 3 ? "([BII)I" : "([B)I";
   if (!stream_forward(vm, source.l, "read", sig, args, nargs, &result)) return false;
   dvm_get_field(vm, self, "on", "Z", &enabled);
   if (enabled.i && (nargs == 0 ? result.i >= 0 : result.i > 0)) {
      dvm_get_field(vm, self, "digest", "Ljava/security/MessageDigest;", &digest);
      if (!digest.l) {
         dvm__throw(vm, "java/lang/NullPointerException", "message digest");
         return false;
      }
      union dvm_value update[3] = {{0}}, ignored = {0};
      if (!nargs) update[0].i = (int8_t)result.i;
      else {
         update[0] = args[0];
         update[1].i = nargs >= 3 ? args[1].i : 0;
         update[2].i = result.i;
      }
      if (!stream_forward(vm, digest.l, "update", nargs ? "([BII)V" : "(B)V",
                          update, nargs ? 3 : 1, &ignored)) return false;
   }
   if (out) *out = result;
   return true;
}

static bool digest_in_get(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value digest = {0};
   dvm_get_field(vm, self, "digest", "Ljava/security/MessageDigest;", &digest);
   RETL(digest.l);
}
static bool digest_in_set(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)nargs; (void)out;
   dvm_set_field(vm, self, "digest", "Ljava/security/MessageDigest;", args[0]);
   RETV();
}
static bool digest_in_on(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs; (void)out;
   dvm_set_field(vm, self, "on", "Z", args[0]);
   RETV();
}

#define DIGEST_IN_FORWARD(fn, name, sig) \
static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
               int nargs, union dvm_value *out) { \
   union dvm_value source = {0}; \
   dvm_get_field(vm, self, "in", "Ljava/io/InputStream;", &source); \
   if (!source.l) { \
      dvm__throw(vm, "java/lang/NullPointerException", "input stream"); \
      return false; \
   } \
   return stream_forward(vm, source.l, name, sig, args, nargs, out); \
}
DIGEST_IN_FORWARD(digest_in_skip, "skip", "(J)J")
DIGEST_IN_FORWARD(digest_in_available, "available", "()I")
DIGEST_IN_FORWARD(digest_in_close, "close", "()V")
DIGEST_IN_FORWARD(digest_in_mark, "mark", "(I)V")
DIGEST_IN_FORWARD(digest_in_reset, "reset", "()V")
DIGEST_IN_FORWARD(digest_in_mark_supported, "markSupported", "()Z")
#undef DIGEST_IN_FORWARD

static const struct rt_field rt_digest_in_fields[] = {
   {"digest", "Ljava/security/MessageDigest;"}, {"on", "Z"}, F_END
};
static const struct rt_method rt_digest_in[] = {
   M("<init>", "(Ljava/io/InputStream;Ljava/security/MessageDigest;)V", digest_in_init),
   M("read", "()I", digest_in_read),
   M("read", "([B)I", digest_in_read),
   M("read", "([BII)I", digest_in_read),
   M("getMessageDigest", "()Ljava/security/MessageDigest;", digest_in_get),
   M("setMessageDigest", "(Ljava/security/MessageDigest;)V", digest_in_set),
   M("on", "(Z)V", digest_in_on),
   M("skip", "(J)J", digest_in_skip),
   M("available", "()I", digest_in_available),
   M("close", "()V", digest_in_close),
   M("mark", "(I)V", digest_in_mark),
   M("reset", "()V", digest_in_reset),
   M("markSupported", "()Z", digest_in_mark_supported),
   M_END
};

/* ------------------------------------------------------------------------ *
 * java.security / javax.crypto
 *
 * The SDK's configuration arrives encrypted under a key delivered with the
 * response, so a session cannot start without these.  Keys are carried as
 * their encoded form and every operation is done by OpenSSL, which the
 * emulator already links for its own TLS.
 * ------------------------------------------------------------------------ */

/* Cipher opmode, as java.crypto.Cipher declares them. */
enum {
   JCE_ENCRYPT = 1,
   JCE_DECRYPT = 2,
   JCE_WRAP = 3,
   JCE_UNWRAP = 4,
};

/* A Key holds its encoded form: DER for the asymmetric ones (PKCS#8 private,
 * X.509 public) and the raw bytes for a SecretKeySpec, which is exactly what
 * getEncoded() returns on a device. */
static dvm_ref key_make(struct dvm *vm, const char *desc, dvm_ref encoded,
                        const char *algorithm, int32_t kind)
{
   struct dvm_class *c = dvm__class_by_desc(vm, desc);
   dvm_ref k = c ? dvm_new_object(vm, c) : 0;
   if (!k) return 0;
   union dvm_value v = { .l = encoded };
   (void)dvm_set_field(vm, k, "encoded", "[B", v);
   v.l = dvm_new_string(vm, algorithm ? algorithm : "");
   (void)dvm_set_field(vm, k, "algorithm", "Ljava/lang/String;", v);
   v.i = kind;
   (void)dvm_set_field(vm, k, "kind", "I", v);
   return k;
}

static const uint8_t *key_bytes(struct dvm *vm, dvm_ref k, uint32_t *len)
{
   union dvm_value v = { 0 };
   *len = 0;
   if (!k || !dvm_get_field(vm, k, "encoded", "[B", &v) || !v.l) return NULL;
   return arg_bytes(vm, v.l, len);
}

static bool key_getEncoded(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "encoded", "[B", &v);
   RETL(v.l);
}

static bool key_getAlgorithm(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &v);
   RETL(v.l);
}

static bool key_getFormat(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "kind", "I", &v);
   RETL(dvm_new_string(vm, v.i == 1 ? "PKCS#8" : v.i == 2 ? "X.509" : "RAW"));
}

/* KeySpec: a byte[] wrapper, so the factory can read it back. */
static bool spec_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   union dvm_value v = { .l = ARG(0).l };
   (void)dvm_set_field(vm, self, "encoded", "[B", v);
   RETV();
}

/* SecretKeySpec(byte[] key, String algorithm) and its offset/length form. */
static bool secret_init(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)out;
   dvm_ref bytes = ARG(0).l;
   const char *alg = NULL;
   if (nargs >= 4) {
      uint32_t n = 0;
      const uint8_t *p = arg_bytes(vm, bytes, &n);
      uint32_t off = (uint32_t)args[1].i, len = (uint32_t)args[2].i;
      if (off > n) off = n;
      if (len > n - off) len = n - off;
      bytes = p ? make_bytes(vm, p + off, len) : 0;
      alg = args[3].l ? dvm_string_utf8(vm, args[3].l) : NULL;
   } else if (nargs >= 2) {
      alg = args[1].l ? dvm_string_utf8(vm, args[1].l) : NULL;
   }
   union dvm_value v = { .l = bytes };
   (void)dvm_set_field(vm, self, "encoded", "[B", v);
   v.l = dvm_new_string(vm, alg ? alg : "");
   (void)dvm_set_field(vm, self, "algorithm", "Ljava/lang/String;", v);
   v.i = 0;
   (void)dvm_set_field(vm, self, "kind", "I", v);
   RETV();
}

static bool kf_getInstance(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *alg = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/security/KeyFactory;");
   dvm_ref f = c ? dvm_new_object(vm, c) : 0;
   if (f) {
      union dvm_value v = { .l = dvm_new_string(vm, alg ? alg : "RSA") };
      (void)dvm_set_field(vm, f, "algorithm", "Ljava/lang/String;", v);
   }
   RETL(f);
}

/* The key a device's RSA KeyFactory returns is Conscrypt's own class, which
 * is a java.security.interfaces.RSAPublicKey / RSAPrivateCrtKey: callers cast
 * to those and read the modulus and exponents off them. */
static const char *kf_key_class(const char *alg, int32_t kind)
{
   if (alg && !strcasecmp(alg, "RSA"))
      return kind == 2 ? "Lcom/android/org/conscrypt/OpenSSLRSAPublicKey;"
                       : "Lcom/android/org/conscrypt/OpenSSLRSAPrivateCrtKey;";
   return "Ljava/security/Key;";
}

/* Numeric RSA specifications are immutable BigInteger references, not an
 * encoded byte array. Keep the inputs until the provider imports the key. */
static bool rsa_public_spec_init(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm,self,"modulus","Ljava/math/BigInteger;",ARG(0));
   (void)dvm_set_field(vm,self,"publicExponent","Ljava/math/BigInteger;",ARG(1));
   RETV();
}

static bool rsa_public_spec_modulus(struct dvm *vm, dvm_ref self,
                                    const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
   (void)args; (void)nargs;
   return dvm_get_field(vm,self,"modulus","Ljava/math/BigInteger;",out);
}

static bool rsa_public_spec_exponent(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   (void)args; (void)nargs;
   return dvm_get_field(vm,self,"publicExponent","Ljava/math/BigInteger;",out);
}

static bool kf_rsa_public_spec(struct dvm *vm, dvm_ref spec,
                               union dvm_value *out)
{
   union dvm_value modulus={0}, exponent={0};
   (void)dvm_get_field(vm,spec,"modulus","Ljava/math/BigInteger;",&modulus);
   (void)dvm_get_field(vm,spec,"publicExponent","Ljava/math/BigInteger;",&exponent);
   if(!modulus.l || !exponent.l) {
      dvm__throw(vm,"java/security/spec/InvalidKeySpecException","null RSA component");
      return false;
   }
   BIGNUM *n=bigint_bn(vm,modulus.l);
   BIGNUM *e=n ? bigint_bn(vm,exponent.l) : NULL;
   if(!n || !e) {BN_free(n);BN_free(e);return false;}
   OSSL_PARAM_BLD *builder=OSSL_PARAM_BLD_new();
   OSSL_PARAM *params=NULL;
   EVP_PKEY_CTX *ctx=NULL;
   EVP_PKEY *key=NULL;
   bool good=!BN_is_negative(n) && !BN_is_zero(n) &&
      !BN_is_negative(e) && !BN_is_zero(e) && builder &&
      OSSL_PARAM_BLD_push_BN(builder,OSSL_PKEY_PARAM_RSA_N,n) &&
      OSSL_PARAM_BLD_push_BN(builder,OSSL_PKEY_PARAM_RSA_E,e);
   if(good) {
      params=OSSL_PARAM_BLD_to_param(builder);
      ctx=EVP_PKEY_CTX_new_from_name(NULL,"RSA",NULL);
      good=params && ctx && EVP_PKEY_fromdata_init(ctx)>0 &&
         EVP_PKEY_fromdata(ctx,&key,EVP_PKEY_PUBLIC_KEY,params)>0;
   }
   unsigned char *der=NULL;
   int length=good ? i2d_PUBKEY(key,&der) : 0;
   EVP_PKEY_free(key);EVP_PKEY_CTX_free(ctx);
   OSSL_PARAM_free(params);OSSL_PARAM_BLD_free(builder);
   BN_free(n);BN_free(e);
   if(length<=0) {
      OPENSSL_free(der);
      dvm__throw(vm,"java/security/spec/InvalidKeySpecException","invalid RSA public key");
      return false;
   }
   dvm_ref encoded=make_bytes(vm,der,(uint32_t)length);
   OPENSSL_free(der);
   RETL(key_make(vm,kf_key_class("RSA",2),encoded,"RSA",2));
}

/* generatePrivate / generatePublic: the spec's bytes become the key's encoded
 * form; OpenSSL parses them when the cipher is used, which is where a
 * malformed key should surface. */
static bool kf_generate(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out, int32_t kind)
{
   (void)nargs;
   union dvm_value enc = { 0 }, alg = { 0 };
   if (ARG(0).l) (void)dvm_get_field(vm, ARG(0).l, "encoded", "[B", &enc);
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &alg);
   const char *alg_name = alg.l ? dvm_string_utf8(vm, alg.l) : "RSA";
   struct dvm_class *spec_class=dvm_object_class(vm,ARG(0).l);
   if(kind==2 && alg_name && !strcasecmp(alg_name,"RSA") && spec_class &&
      dvm__class_assignable(vm,spec_class,
         dvm__class_by_desc(vm,"Ljava/security/spec/RSAPublicKeySpec;")))
      return kf_rsa_public_spec(vm,ARG(0).l,out);
   if (!enc.l) {
      if (getenv("LUNARIA_DVM_TRACE")) {
         struct dvm_class *spec = dvm_object_class(vm, ARG(0).l);
         fprintf(stderr, "[crypto] KeyFactory.generate%s spec=%s\n",
                 kind == 2 ? "Public" : "Private", spec ? spec->desc : "null");
      }
      dvm__throw(vm, "java/security/spec/InvalidKeySpecException",
                 "no encoded key in the spec");
      return false;
   }
   RETL(key_make(vm, kf_key_class(alg_name, kind), enc.l, alg_name, kind));
}

/* The key's modulus / public exponent / private exponent as a BigInteger,
 * parsed from the encoded form the key carries. */
static bool rsa_key_component(struct dvm *vm, dvm_ref self, const char *param,
                              union dvm_value *out)
{
   uint32_t len = 0;
   const uint8_t *kb = key_bytes(vm, self, &len);
   union dvm_value kind = { 0 };
   (void)dvm_get_field(vm, self, "kind", "I", &kind);
   if (!kb) RETL(0);
   const uint8_t *kp = kb;
   EVP_PKEY *pkey = kind.i == 2 ? d2i_PUBKEY(NULL, &kp, len)
                                : d2i_AutoPrivateKey(NULL, &kp, len);
   BIGNUM *bn = NULL;
   dvm_ref result = 0;
   if (pkey && EVP_PKEY_get_bn_param(pkey, param, &bn) > 0 && bn) {
      uint8_t mag[1024];
      const int n = BN_num_bytes(bn);
      if (n >= 0 && (size_t)n <= sizeof mag) {
         BN_bn2bin(bn, mag);
         struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/math/BigInteger;");
         result = c ? dvm_new_object(vm, c) : 0;
         if (result) bigint_store_mag(vm, result, 1, mag, (uint32_t)n);
      }
   }
   BN_free(bn);
   EVP_PKEY_free(pkey);
   RETL(result);
}

static bool rsa_getModulus(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   return rsa_key_component(vm, self, OSSL_PKEY_PARAM_RSA_N, out);
}

static bool rsa_getPublicExponent(struct dvm *vm, dvm_ref self,
                                  const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
   (void)args; (void)nargs;
   return rsa_key_component(vm, self, OSSL_PKEY_PARAM_RSA_E, out);
}

static bool rsa_getPrivateExponent(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
   (void)args; (void)nargs;
   return rsa_key_component(vm, self, OSSL_PKEY_PARAM_RSA_D, out);
}

static bool kf_generatePrivate(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   return kf_generate(vm, self, args, nargs, out, 1);
}

static bool kf_generatePublic(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   return kf_generate(vm, self, args, nargs, out, 2);
}

/* --- javax.crypto.Cipher ------------------------------------------------ */

static void cipher_parse(const char *, char *, size_t, char *, size_t, char *, size_t);

static bool cipher_validate_transform(struct dvm *vm, const char *t)
{
   char alg[128], mode[128], pad[128];
   if (!t || !*t || strlen(t) >= sizeof alg) goto algorithm_error;
   cipher_parse(t, alg, sizeof alg, mode, sizeof mode, pad, sizeof pad);
   if (!*alg || (strchr(t, '/') && (!*mode || !*pad || strchr(pad, '/'))))
      goto algorithm_error;
   bool aes = !strcasecmp(alg, "AES");
   bool rsa = !strcasecmp(alg, "RSA");
   bool des = !strcasecmp(alg, "DES") || !strcasecmp(alg, "DESede") ||
              !strcasecmp(alg, "TripleDES");
   bool rc4 = !strcasecmp(alg, "ARCFOUR") || !strcasecmp(alg, "RC4");
   if (!aes && !rsa && !des && !rc4) goto algorithm_error;
   if (*mode && strcasecmp(mode, "ECB") &&
       !(rsa && !strcasecmp(mode, "NONE")) &&
       !((aes || des) && !strcasecmp(mode, "CBC")) &&
       !(aes && (!strcasecmp(mode, "CTR") || !strcasecmp(mode, "GCM"))))
      goto algorithm_error;
   bool padding = !*pad || !strcasecmp(pad, "NoPadding") ||
      (!rsa && !rc4 && (!strcasecmp(pad, "PKCS5Padding") || !strcasecmp(pad, "PKCS7Padding"))) ||
      (rsa && (!strcasecmp(pad, "PKCS1Padding") || !strcasecmp(pad, "OAEPPadding") ||
               !strcasecmp(pad, "OAEPWithSHA-1AndMGF1Padding") ||
               !strcasecmp(pad, "OAEPWithSHA-224AndMGF1Padding") ||
               !strcasecmp(pad, "OAEPWithSHA-256AndMGF1Padding") ||
               !strcasecmp(pad, "OAEPWithSHA-384AndMGF1Padding") ||
               !strcasecmp(pad, "OAEPWithSHA-512AndMGF1Padding")));
   if (aes && (!strcasecmp(mode, "CTR") || !strcasecmp(mode, "GCM")))
      padding = !strcasecmp(pad, "NoPadding");
   if (!padding) {
      dvm__throw(vm, "javax/crypto/NoSuchPaddingException", "%s", pad);
      return false;
   }
   return true;
algorithm_error:
   dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s", t ? t : "null");
   return false;
}

static bool cipher_getInstance(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *t = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!cipher_validate_transform(vm, t)) return false;
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljavax/crypto/Cipher;");
   dvm_ref k = c ? dvm_new_object(vm, c) : 0;
   if (k) {
      union dvm_value v = { .l = dvm_new_string(vm, t ? t : "") };
      (void)dvm_set_field(vm, k, "transform", "Ljava/lang/String;", v);
      v.l = security_builtin_provider(vm);
      (void)dvm_set_field(vm, k, "provider", "Ljava/security/Provider;", v);
   }
   RETL(k);
}

static bool cipher_getProvider(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value provider = {0};
   (void)dvm_get_field(vm, self, "provider", "Ljava/security/Provider;", &provider);
   RETL(provider.l);
}

static bool cipher_getInstance_provider(struct dvm *vm, dvm_ref self,
                                        const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
   if (nargs < 2 || !args[1].l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Missing provider");
      return false;
   }
   if (args[1].l != security_builtin_provider(vm)) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException",
                 "No Cipher service in the specified provider");
      return false;
   }
   return cipher_getInstance(vm, self, args, 1, out);
}

static bool cipher_getInstance_named(struct dvm *vm, dvm_ref self,
                                     const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
   const char *name = nargs > 1 && args[1].l ? dvm_string_utf8(vm, args[1].l) : NULL;
   if (!name || !*name) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Missing provider");
      return false;
   }
   /* Android 12's providers include "BC" next to "AndroidOpenSSL"; both serve
    * the transformations this runtime implements (PKCS7 padding is PKCS5's). */
   if (strcmp(name, "AndroidOpenSSL") && strcmp(name, "BC")) {
      dvm__throw(vm, "java/security/NoSuchProviderException", "%s", name);
      return false;
   }
   return cipher_getInstance(vm, self, args, 1, out);
}

/* Native state follows the Java object's lifetime. */
static void cipher_state_destroy(void *data)
{
   EVP_CIPHER_CTX_free(data);
}
static void cipher_state_clear(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *object = dvm__obj(vm, self);
   if (object && object->data) {
      cipher_state_destroy(object->data);
      object->data = NULL;
      object->data_destroy = NULL;
   }
}

static bool cipher_init(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)out;
   union dvm_value empty = {0};
   cipher_state_clear(vm, self);
   (void)dvm_set_field(vm, self, "pending", "[B", empty);
   (void)dvm_set_field(vm, self, "opmode", "I", empty);
   (void)dvm_set_field(vm, self, "key", "Ljava/lang/Object;", empty);
   if (ARG(0).i < JCE_ENCRYPT || ARG(0).i > JCE_UNWRAP) {
      dvm__throw(vm, "java/security/InvalidParameterException", "unsupported cipher mode");
      return false;
   }
   if (nargs < 2 || !args[1].l) {
      dvm__throw(vm, "java/security/InvalidKeyException", "no key");
      return false;
   }
   const struct dvm_method *method = dvm__builtin_method();
   const bool with_params = method && strstr(method->sig, "Ljava/security/spec/AlgorithmParameterSpec;");
   dvm_ref params = with_params && nargs >= 3 ? args[2].l : 0;
   dvm_ref random = !with_params && nargs >= 3 ? args[2].l : 0;
   union dvm_value iv = {0}, transform = {0};
   if (params) (void)dvm_get_field(vm, params, "iv", "[B", &iv);
   (void)dvm_get_field(vm, self, "transform", "Ljava/lang/String;", &transform);
   char alg[128], mode[128], pad[128];
   cipher_parse(dvm_string_utf8(vm, transform.l), alg, sizeof alg,
                mode, sizeof mode, pad, sizeof pad);
   const bool rsa = !strcasecmp(alg, "RSA");
   const bool gcm = !strcasecmp(mode, "GCM");
   const bool encrypting = ARG(0).i == JCE_ENCRYPT || ARG(0).i == JCE_WRAP;
   int expected = gcm ? 12 : (!strcasecmp(mode, "CBC") || !strcasecmp(mode, "CTR"))
      ? (!strcasecmp(alg, "AES") ? 16 : 8) : 0;
   if (!rsa) {
      if (params && !iv.l) {
         dvm__throw(vm, "java/security/InvalidAlgorithmParameterException", "unsupported cipher parameters");
         return false;
      }
      if (iv.l && ((!expected) || (gcm ? !dvm_array_length(vm, iv.l)
                                  : dvm_array_length(vm, iv.l) != (uint32_t)expected))) {
         dvm__throw(vm, "java/security/InvalidAlgorithmParameterException", "invalid IV length");
         return false;
      }
      if (!iv.l && expected) {
         if (!encrypting) {
            dvm__throw(vm, with_params ? "java/security/InvalidAlgorithmParameterException"
                                      : "java/security/InvalidKeyException", "IV must be specified");
            return false;
         }
         iv.l = dvm_new_array(vm, 'B', NULL, (uint32_t)expected);
         if (!iv.l) return false;
         if (random) {
            struct dvm_method *next = dvm_find_method(vm, dvm_object_class(vm, random), "nextBytes", "([B)V");
            union dvm_value ignored = {0};
            if (!next || !dvm_call(vm, next, random, &iv, 1, &ignored)) return false;
         } else if (RAND_bytes(dvm_array_data(vm, iv.l), expected) != 1) {
            dvm__throw(vm, "java/security/ProviderException", "CSPRNG failed while generating an IV");
            return false;
         }
      } else if (iv.l) {
         uint32_t length = 0;
         const uint8_t *bytes = arg_bytes(vm, iv.l, &length);
         iv.l = make_bytes(vm, bytes, length);
      }
   }
   union dvm_value value = {.i = ARG(0).i};
   (void)dvm_set_field(vm, self, "opmode", "I", value);
   value.l = args[1].l;
   (void)dvm_set_field(vm, self, "key", "Ljava/lang/Object;", value);
   (void)dvm_set_field(vm, self, "iv", "[B", iv);
   (void)dvm_set_field(vm, self, "aad", "[B", empty);
   RETV();
}

static bool cipher_updateAAD(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)out;
   uint32_t input_len = 0;
   const uint8_t *input = arg_bytes(vm, ARG(0).l, &input_len);
   uint32_t offset = 0, count = input_len;
   if (nargs >= 3) {
      if (args[1].i < 0 || args[2].i < 0 ||
          (uint32_t)args[1].i > input_len ||
          (uint32_t)args[2].i > input_len - (uint32_t)args[1].i) {
         dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "AAD range");
         return false;
      }
      offset = (uint32_t)args[1].i;
      count = (uint32_t)args[2].i;
   }
   union dvm_value oldv = { 0 };
   (void)dvm_get_field(vm, self, "aad", "[B", &oldv);
   uint32_t old_len = 0;
   const uint8_t *old = oldv.l ? arg_bytes(vm, oldv.l, &old_len) : NULL;
   dvm_ref joined = dvm_new_array(vm, 'B', "B", old_len + count);
   struct dvm_object *array = joined ? dvm__obj(vm, joined) : NULL;
   if (!array || !array->data) RETV();
   if (old_len && old) memcpy(array->data, old, old_len);
   if (count && input)
      memcpy((uint8_t *)array->data + old_len, input + offset, count);
   union dvm_value next = { .l = joined };
   (void)dvm_set_field(vm, self, "aad", "[B", next);
   RETV();
}

/* Splits "AES/CBC/PKCS5Padding" into its three parts. */
static void cipher_parse(const char *t, char *alg, size_t na, char *mode,
                         size_t nm, char *pad, size_t np)
{
   snprintf(alg, na, "%s", t ? t : "");
   mode[0] = pad[0] = '\0';
   char *slash = strchr(alg, '/');
   if (!slash) return;
   *slash = '\0';
   snprintf(mode, nm, "%s", slash + 1);
   char *slash2 = strchr(mode, '/');
   if (!slash2) return;
   *slash2 = '\0';
   snprintf(pad, np, "%s", slash2 + 1);
}

static bool cipher_fail(struct dvm *vm, const char *what)
{
   unsigned long e = ERR_get_error();
   char ebuf[128] = "";
   if (e) ERR_error_string_n(e, ebuf, sizeof ebuf);
   dvm__throw(vm, "javax/crypto/BadPaddingException", "%s%s%s", what,
              ebuf[0] ? ": " : "", ebuf);
   return false;
}

static bool cipher_rsa(struct dvm *vm, int opmode, const char *pad,
                       const uint8_t *key, uint32_t keylen, int32_t kind,
                       const uint8_t *in, uint32_t inlen, union dvm_value *out)
{
   const uint8_t *kp = key;
   EVP_PKEY *pkey = kind == 2 ? d2i_PUBKEY(NULL, &kp, keylen)
                              : d2i_AutoPrivateKey(NULL, &kp, keylen);
   if (!pkey) return cipher_fail(vm, "cannot read the RSA key");

   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
   bool enc = opmode == JCE_ENCRYPT || opmode == JCE_WRAP;
   const EVP_MD *oaep_md = NULL;
   int ok = ctx && (enc ? EVP_PKEY_encrypt_init(ctx)
                        : EVP_PKEY_decrypt_init(ctx)) > 0;
   if (ok) {
      int p = RSA_PKCS1_PADDING;
      bool oaep = pad && luna_strcasestr(pad, "OAEP");
      if (oaep) p = RSA_PKCS1_OAEP_PADDING;
      else if (pad && (!strcasecmp(pad, "NoPadding") || !*pad))
         p = RSA_NO_PADDING;
      ok = EVP_PKEY_CTX_set_rsa_padding(ctx, p) > 0;

      /* "OAEPWithSHA-256AndMGF1Padding" names the label digest.  Which digest
       * MGF1 then uses is not settled: Sun's provider keeps SHA-1 while
       * Conscrypt and BouncyCastle follow the name, and the transformation
       * alone cannot say which one wrote the ciphertext.  The label digest is
       * set here and the MGF1 choice is decided by trying both below. */
      if (ok && oaep) {
         const char *w = luna_strcasestr(pad, "OAEPWith");
         if (w) {
            char name[32];
            const char *b = w + 8;
            const char *e = luna_strcasestr(b, "And");
            size_t n = e ? (size_t)(e - b) : strlen(b);
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, b, n);
            name[n] = '\0';
            oaep_md = EVP_get_digestbyname(name);
         }
         if (oaep_md) ok = EVP_PKEY_CTX_set_rsa_oaep_md(ctx, oaep_md) > 0;
      }
   }

   size_t outlen = 0;
   if (ok)
      ok = (enc ? EVP_PKEY_encrypt(ctx, NULL, &outlen, in, inlen)
                : EVP_PKEY_decrypt(ctx, NULL, &outlen, in, inlen)) > 0;
   uint8_t *buf = ok ? malloc(outlen ? outlen : 1) : NULL;
   if (buf)
      ok = (enc ? EVP_PKEY_encrypt(ctx, buf, &outlen, in, inlen)
                : EVP_PKEY_decrypt(ctx, buf, &outlen, in, inlen)) > 0;

   /* An OAEP decrypt that fails only tells us the padding did not check out,
    * which is exactly what the wrong MGF1 digest looks like.  Retry once with
    * the other reading of the transformation name before giving up. */
   if (!ok && !enc && oaep_md && buf) {
      ERR_clear_error();
      const EVP_MD *alt = oaep_md == EVP_sha1() ? EVP_sha256() : EVP_sha1();
      EVP_PKEY_CTX *c2 = EVP_PKEY_CTX_new(pkey, NULL);
      if (c2 && EVP_PKEY_decrypt_init(c2) > 0 &&
          EVP_PKEY_CTX_set_rsa_padding(c2, RSA_PKCS1_OAEP_PADDING) > 0 &&
          EVP_PKEY_CTX_set_rsa_oaep_md(c2, oaep_md) > 0 &&
          EVP_PKEY_CTX_set_rsa_mgf1_md(c2, alt) > 0) {
         outlen = 0;
         if (EVP_PKEY_decrypt(c2, NULL, &outlen, in, inlen) > 0) {
            uint8_t *b2 = malloc(outlen ? outlen : 1);
            if (b2 && EVP_PKEY_decrypt(c2, b2, &outlen, in, inlen) > 0) {
               free(buf);
               buf = b2;
               ok = true;
            } else {
               free(b2);
            }
         }
      }
      EVP_PKEY_CTX_free(c2);
   }
   EVP_PKEY_CTX_free(ctx);
   EVP_PKEY_free(pkey);
   if (!ok || !buf) {
      free(buf);
      char what[128];
      snprintf(what, sizeof what, "RSA/%s %s failed", pad && *pad ? pad : "-",
               enc ? "encrypt" : "decrypt");
      return cipher_fail(vm, what);
   }
   dvm_ref r = make_bytes(vm, buf, (uint32_t)outlen);
   free(buf);
   RETL(r);
}

static const EVP_CIPHER *cipher_pick(const char *alg, const char *mode,
                                     uint32_t keylen)
{
   bool cbc = !strcasecmp(mode, "CBC");
   bool ecb = !strcasecmp(mode, "ECB") || !*mode;
   bool gcm = !strcasecmp(mode, "GCM");
   bool ctr = !strcasecmp(mode, "CTR");
   if (!strcasecmp(alg, "AES")) {
      if (keylen == 16)
         return gcm ? EVP_aes_128_gcm() : ctr ? EVP_aes_128_ctr()
              : cbc ? EVP_aes_128_cbc() : ecb ? EVP_aes_128_ecb() : NULL;
      if (keylen == 24)
         return gcm ? EVP_aes_192_gcm() : ctr ? EVP_aes_192_ctr()
              : cbc ? EVP_aes_192_cbc() : ecb ? EVP_aes_192_ecb() : NULL;
      if (keylen == 32)
         return gcm ? EVP_aes_256_gcm() : ctr ? EVP_aes_256_ctr()
              : cbc ? EVP_aes_256_cbc() : ecb ? EVP_aes_256_ecb() : NULL;
      return NULL;
   }
   if (!strcasecmp(alg, "DESede") || !strcasecmp(alg, "TripleDES"))
      return cbc ? EVP_des_ede3_cbc() : EVP_des_ede3_ecb();
   if (!strcasecmp(alg, "DES"))
      return cbc ? EVP_des_cbc() : EVP_des_ecb();
   if (!strcasecmp(alg, "ARCFOUR") || !strcasecmp(alg, "RC4"))
      return EVP_rc4();
   return NULL;
}

static bool cipher_sym(struct dvm *vm, int opmode, const char *alg,
                       const char *mode, const char *pad, const uint8_t *key,
                       uint32_t keylen, const uint8_t *iv, uint32_t ivlen,
                       const uint8_t *aad, uint32_t aadlen,
                       const uint8_t *in, uint32_t inlen, union dvm_value *out)
{
   const EVP_CIPHER *type = cipher_pick(alg, mode, keylen);
   if (!type) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s/%s", alg,
                 mode);
      return false;
   }
   bool enc = opmode == JCE_ENCRYPT || opmode == JCE_WRAP;
   /* Java's PKCS5Padding is PKCS#7, which is what OpenSSL applies by default;
    * NoPadding turns it off. */
   bool padding = !pad || !*pad || strcasecmp(pad, "NoPadding") != 0;

   EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
   if (!ctx) return cipher_fail(vm, "cipher init failed");
   int ok = EVP_CipherInit_ex(ctx, type, NULL, NULL, NULL, enc) > 0;
   if (ok && ivlen && EVP_CIPHER_CTX_get_iv_length(ctx) != (int)ivlen)
      ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)ivlen,
                               NULL) > 0;
   if (ok) ok = EVP_CipherInit_ex(ctx, NULL, NULL, key, ivlen ? iv : NULL,
                                  enc) > 0;
   if (ok) EVP_CIPHER_CTX_set_padding(ctx, padding);

   /* GCM carries a 16-byte tag at the end of the ciphertext, the way the JCE
    * presents it. */
   bool gcm = !strcasecmp(mode, "GCM");
   uint32_t body = inlen;
   if (ok && gcm && !enc) {
      if (inlen < 16) ok = 0;
      else {
         body = inlen - 16;
         ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                  (void *)(uintptr_t)(in + body)) > 0;
      }
   }

   int aad_out = 0;
   if (ok && gcm && aadlen)
      ok = EVP_CipherUpdate(ctx, NULL, &aad_out, aad, (int)aadlen) > 0;

   uint8_t *buf = ok ? malloc((size_t)inlen + EVP_MAX_BLOCK_LENGTH + 16) : NULL;
   int n = 0, total = 0;
   if (buf) ok = EVP_CipherUpdate(ctx, buf, &n, in, (int)body) > 0;
   if (ok) {
      total = n;
      ok = EVP_CipherFinal_ex(ctx, buf + total, &n) > 0;
   }
   if (ok) {
      total += n;
      if (gcm && enc) {
         ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                                  buf + total) > 0;
         if (ok) total += 16;
      }
   }
   EVP_CIPHER_CTX_free(ctx);
   if (!ok || !buf) {
      free(buf);
      return cipher_fail(vm, enc ? "encrypt failed" : "decrypt failed");
   }
   dvm_ref r = make_bytes(vm, buf, (uint32_t)total);
   free(buf);
   RETL(r);
}

/* Clone before processing so an undersized output does not consume input or
 * advance cipher state. GCM retains ciphertext until authentication at final;
 * RSA similarly has no partial output. */
static bool cipher_update_bytes(struct dvm *vm, dvm_ref self,
                                const uint8_t *input, uint32_t length,
                                uint8_t **output, int *written,
                                EVP_CIPHER_CTX **next, bool *buffered)
{
   union dvm_value op={0}, transform={0}, keyref={0}, ivref={0};
   (void)dvm_get_field(vm,self,"opmode","I",&op);
   if (op.i != JCE_ENCRYPT && op.i != JCE_DECRYPT) {
      dvm__throw(vm,"java/lang/IllegalStateException","cipher is not initialized for encryption/decryption");
      return false;
   }
   (void)dvm_get_field(vm,self,"transform","Ljava/lang/String;",&transform);
   char alg[128],mode[128],pad[128];
   cipher_parse(dvm_string_utf8(vm,transform.l),alg,sizeof alg,mode,sizeof mode,pad,sizeof pad);
   *buffered = !strcasecmp(alg,"RSA") || !strcasecmp(mode,"GCM");
   *written=0; *output=NULL; *next=NULL;
   if (*buffered) return true;
   EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();
   struct dvm_object *object=dvm__obj(vm,self);
   int ok=ctx != NULL;
   if (ok && object->data) ok=EVP_CIPHER_CTX_copy(ctx,object->data)>0;
   else if (ok) {
      uint32_t keylen=0,ivlen=0;
      (void)dvm_get_field(vm,self,"key","Ljava/lang/Object;",&keyref);
      (void)dvm_get_field(vm,self,"iv","[B",&ivref);
      const uint8_t *key=key_bytes(vm,keyref.l,&keylen);
      const uint8_t *iv=ivref.l?arg_bytes(vm,ivref.l,&ivlen):NULL;
      const EVP_CIPHER *type=cipher_pick(alg,mode,keylen);
      ok=key && type && EVP_CipherInit_ex(ctx,type,NULL,key,iv,op.i==JCE_ENCRYPT)>0;
      if(ok) ok=EVP_CIPHER_CTX_set_padding(ctx,strcasecmp(pad,"NoPadding")!=0)>0;
   }
   if(ok) *output=malloc((size_t)length+EVP_MAX_BLOCK_LENGTH);
   if(ok && !*output) ok=0;
   if(ok && length) ok=EVP_CipherUpdate(ctx,*output,written,input,(int)length)>0;
   if(!ok) {
      free(*output); *output=NULL; EVP_CIPHER_CTX_free(ctx);
      return cipher_fail(vm,"cipher update failed");
   }
   *next=ctx;
   return true;
}
static bool cipher_commit_update(struct dvm *vm,dvm_ref self,
                                 EVP_CIPHER_CTX *next,bool buffered,
                                 const uint8_t *input,uint32_t length)
{
   if(buffered) {
      union dvm_value old={0}; uint32_t size=0;
      (void)dvm_get_field(vm,self,"pending","[B",&old);
      const uint8_t *bytes=old.l?arg_bytes(vm,old.l,&size):NULL;
      if(length>INT32_MAX-size) {
         dvm__throw(vm,"java/lang/OutOfMemoryError","cipher input too large"); return false;
      }
      dvm_ref joined=dvm_new_array(vm,'B',NULL,size+length);
      if(!joined) return false;
      uint8_t *dest=dvm_array_data(vm,joined);
      if(size) memcpy(dest,bytes,size);
      if(length) memcpy(dest+size,input,length);
      union dvm_value value={.l=joined};
      return dvm_set_field(vm,self,"pending","[B",value);
   }
   cipher_state_clear(vm,self);
   struct dvm_object *object=dvm__obj(vm,self);
   object->data=next; object->data_destroy=cipher_state_destroy;
   return true;
}
static bool cipher_update_buffers(struct dvm *vm,dvm_ref self,
                                  const union dvm_value *args,int nargs,
                                  union dvm_value *out)
{
   (void)nargs;
   if(!ARG(0).l || !ARG(1).l) {
      dvm__throw(vm,"java/lang/NullPointerException","cipher buffer"); return false;
   }
   if(ARG(0).l==ARG(1).l) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","input and output are the same buffer"); return false;
   }
   union dvm_value readonly={0};
   (void)dvm_get_field(vm,ARG(1).l,"readOnly","Z",&readonly);
   if(readonly.i) {
      dvm__throw(vm,"java/nio/ReadOnlyBufferException","cipher output"); return false;
   }
   struct dvm_object *src=stream_buf(vm,ARG(0).l),*dst=stream_buf(vm,ARG(1).l);
   int32_t ip=0,il=0,op=0,ol=0;
   (void)stream_field(vm,ARG(0).l,"pos",&ip);
   (void)stream_field(vm,ARG(0).l,"count",&il);
   (void)stream_field(vm,ARG(1).l,"pos",&op);
   (void)stream_field(vm,ARG(1).l,"count",&ol);
   if(!src || !dst || ip<0 || il<ip || (uint32_t)il>src->length ||
      op<0 || ol<op || (uint32_t)ol>dst->length) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","invalid cipher buffer window"); return false;
   }
   const uint8_t *input=src->data?(uint8_t *)src->data+ip:(const uint8_t *)"";
   uint8_t *bytes=NULL; int written=0; EVP_CIPHER_CTX *next=NULL; bool buffered=false;
   if(!cipher_update_bytes(vm,self,input,(uint32_t)(il-ip),&bytes,&written,&next,&buffered)) return false;
   if(written>ol-op) {
      free(bytes); EVP_CIPHER_CTX_free(next);
      dvm__throw(vm,"javax/crypto/ShortBufferException","output buffer too small"); return false;
   }
   if(!cipher_commit_update(vm,self,next,buffered,input,(uint32_t)(il-ip))) {
      free(bytes); EVP_CIPHER_CTX_free(next); return false;
   }
   if(written) memcpy((uint8_t *)dst->data+op,bytes,(size_t)written);
   free(bytes);
   stream_set_field(vm,ARG(0).l,"pos",il);
   stream_set_field(vm,ARG(1).l,"pos",op+written);
   RETI(written);
}

static bool cipher_update_array(struct dvm *vm,dvm_ref self,
                                const union dvm_value *args,int nargs,
                                union dvm_value *out)
{
   if(!ARG(0).l) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","null cipher input"); return false;
   }
   uint32_t size=0; const uint8_t *input=arg_bytes(vm,ARG(0).l,&size);
   int32_t offset=nargs>=3?ARG(1).i:0;
   int32_t length=nargs>=3?ARG(2).i:(int32_t)size;
   if(offset<0 || length<0 || (uint32_t)offset>size || (uint32_t)length>size-(uint32_t)offset) {
      dvm__throw(vm,"java/lang/IllegalArgumentException","invalid cipher input range"); return false;
   }
   if(input) input+=offset;
   uint8_t *bytes=NULL; int written=0; EVP_CIPHER_CTX *next=NULL; bool buffered=false;
   if(!cipher_update_bytes(vm,self,input,(uint32_t)length,&bytes,&written,&next,&buffered)) return false;
   dvm_ref result=written?make_bytes(vm,bytes,(uint32_t)written):0;
   free(bytes);
   if(written && !result) { EVP_CIPHER_CTX_free(next); return false; }
   if(!cipher_commit_update(vm,self,next,buffered,input,(uint32_t)length)) {
      EVP_CIPHER_CTX_free(next); return false;
   }
   RETL(result);
}

static bool cipher_doFinal(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   union dvm_value tv = { 0 }, ov = { 0 }, kv = { 0 }, ivv = { 0 }, aadv = { 0 };
   (void)dvm_get_field(vm, self, "transform", "Ljava/lang/String;", &tv);
   (void)dvm_get_field(vm, self, "opmode", "I", &ov);
   if (!ov.i) {
      dvm__throw(vm, "java/lang/IllegalStateException", "cipher is not initialized");
      return false;
   }
   (void)dvm_get_field(vm, self, "key", "Ljava/lang/Object;", &kv);
   (void)dvm_get_field(vm, self, "iv", "[B", &ivv);
   (void)dvm_get_field(vm, self, "aad", "[B", &aadv);

   /* "RSA/ECB/OAEPWithSHA-256AndMGF1Padding" is 37 characters, and the middle
    * buffer briefly holds everything after the algorithm. */
   char alg[128], mode[128], pad[128];
   cipher_parse(tv.l ? dvm_string_utf8(vm, tv.l) : "", alg, sizeof alg, mode,
                sizeof mode, pad, sizeof pad);

   uint32_t keylen = 0, ivlen = 0, aadlen = 0, inlen = 0;
   const uint8_t *key = key_bytes(vm, kv.l, &keylen);
   const uint8_t *iv = ivv.l ? arg_bytes(vm, ivv.l, &ivlen) : NULL;
   const uint8_t *aad = aadv.l ? arg_bytes(vm, aadv.l, &aadlen) : NULL;
   const uint8_t *in = nargs ? arg_bytes(vm, ARG(0).l, &inlen) : NULL;
   if (nargs >= 3) {
      uint32_t off = (uint32_t)args[1].i, len = (uint32_t)args[2].i;
      if (off > inlen || len > inlen - off) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","invalid cipher input range");
         return false;
      }
      if (in) in += off;
      inlen = len;
   }
   if (!key) {
      dvm__throw(vm, "java/security/InvalidKeyException", "no key");
      return false;
   }
   if (!in) { in = (const uint8_t *)""; inlen = 0; }

   union dvm_value kind = { 0 };
   if (kv.l) (void)dvm_get_field(vm, kv.l, "kind", "I", &kind);
   if (getenv("LUNARIA_HTTP_TRACE"))
      fprintf(stderr, "[jce] %s alg=%s mode=%s pad=%s key=%u(kind %d) iv=%u "
              "in=%u\n", ov.i == 1 ? "encrypt" : ov.i == 2 ? "decrypt" : "?",
              alg, mode, pad, keylen, kind.i, ivlen, inlen);
   struct dvm_object *object=dvm__obj(vm,self);
   if(object && object->data) {
      uint8_t *bytes=NULL; int written=0,tail=0;
      EVP_CIPHER_CTX *next=NULL; bool buffered=false;
      if(!cipher_update_bytes(vm,self,in,inlen,&bytes,&written,&next,&buffered)) return false;
      int final_ok=EVP_CipherFinal_ex(next,bytes+written,&tail)>0;
      EVP_CIPHER_CTX_free(next);
      if(!final_ok) { free(bytes); return cipher_fail(vm,"cipher final failed"); }
      dvm_ref result=make_bytes(vm,bytes,(uint32_t)(written+tail));
      free(bytes);
      if(!result) return false;
      cipher_state_clear(vm,self);
      union dvm_value empty={0};
      (void)dvm_set_field(vm,self,"aad","[B",empty);
      RETL(result);
   }
   union dvm_value pending={0}; uint32_t pending_len=0;
   (void)dvm_get_field(vm,self,"pending","[B",&pending);
   const uint8_t *prefix=pending.l?arg_bytes(vm,pending.l,&pending_len):NULL;
   uint8_t *joined=NULL;
   if(pending_len) {
      if(inlen>INT32_MAX-pending_len) {
         dvm__throw(vm,"java/lang/OutOfMemoryError","cipher input too large"); return false;
      }
      joined=malloc((size_t)pending_len+inlen);
      if(!joined) { dvm__throw(vm,"java/lang/OutOfMemoryError","cipher input"); return false; }
      memcpy(joined,prefix,pending_len);
      if(inlen) memcpy(joined+pending_len,in,inlen);
      in=joined; inlen+=pending_len;
   }
   bool ok = !strcasecmp(alg, "RSA")
      ? cipher_rsa(vm, ov.i, pad, key, keylen, kind.i, in, inlen, out)
      : cipher_sym(vm, ov.i, alg, mode, pad, key, keylen, iv, ivlen, aad,
                   aadlen, in, inlen, out);
   free(joined);
   if (ok) {
      union dvm_value empty = { .l = 0 };
      (void)dvm_set_field(vm, self, "pending", "[B", empty);
      (void)dvm_set_field(vm, self, "aad", "[B", empty);
   }
   return ok;
}

static bool cipher_doFinal_into(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   if (nargs < 5) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "doFinal args");
      return false;
   }
   struct dvm_object *object=dvm__obj(vm,self);
   EVP_CIPHER_CTX *saved=NULL;
   if(object && object->data) {
      saved=EVP_CIPHER_CTX_new();
      if(!saved || EVP_CIPHER_CTX_copy(saved,object->data)<=0) {
         EVP_CIPHER_CTX_free(saved); return cipher_fail(vm,"cannot preserve cipher state");
      }
   }
   union dvm_value aad={0},pending={0};
   (void)dvm_get_field(vm,self,"aad","[B",&aad);
   (void)dvm_get_field(vm,self,"pending","[B",&pending);
   union dvm_value bytes = { 0 };
   if (!cipher_doFinal(vm, self, args, 3, &bytes)) {
      EVP_CIPHER_CTX_free(saved); return false;
   }
   struct dvm_object *src = bytes.l ? dvm__obj(vm, bytes.l) : NULL;
   struct dvm_object *dst = args[3].l ? dvm__obj(vm, args[3].l) : NULL;
   int32_t offset = args[4].i;
   if (!src || src->kind != DVM_OBJ_ARRAY || !dst ||
       dst->kind != DVM_OBJ_ARRAY || offset < 0 ||
       (uint32_t)offset > dst->length ||
       src->length > dst->length - (uint32_t)offset) {
      cipher_state_clear(vm,self);
      object=dvm__obj(vm,self);
      object->data=saved;
      object->data_destroy=saved?cipher_state_destroy:NULL;
      (void)dvm_set_field(vm,self,"aad","[B",aad);
      (void)dvm_set_field(vm,self,"pending","[B",pending);
      dvm__throw(vm, "javax/crypto/ShortBufferException", "output buffer too small");
      return false;
   }
   EVP_CIPHER_CTX_free(saved);
   if (src->length)
      memcpy((uint8_t *)dst->data + offset, src->data, src->length);
   RETI((int32_t)src->length);
}

static bool cipher_getBlockSize(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value transform = {0};
   (void)dvm_get_field(vm, self, "transform", "Ljava/lang/String;", &transform);
   char alg[128], mode[128], pad[128];
   cipher_parse(dvm_string_utf8(vm, transform.l), alg, sizeof alg,
                mode, sizeof mode, pad, sizeof pad);
   /* Report the cipher's block, not EVP's streaming update granularity:
    * AES CTR/GCM still have 16-byte blocks before and after init(). */
   if (!strcasecmp(alg, "AES")) RETI(16);
   if (!strcasecmp(alg, "DES") || !strcasecmp(alg, "DESede") ||
       !strcasecmp(alg, "TripleDES")) RETI(8);
   if (strcasecmp(alg, "RSA")) RETI(0); /* RC4 is a stream cipher. */

   /* Conscrypt RSA reports the maximum input block for the current mode. */
   union dvm_value key = {0}, kind = {0}, op = {0};
   (void)dvm_get_field(vm, self, "key", "Ljava/lang/Object;", &key);
   if (!key.l) {
      dvm__throw(vm, "java/lang/IllegalStateException", "cipher is not initialized");
      return false;
   }
   uint32_t length = 0;
   const uint8_t *encoded = key_bytes(vm, key.l, &length);
   (void)dvm_get_field(vm, key.l, "kind", "I", &kind);
   const uint8_t *cursor = encoded;
   EVP_PKEY *pkey = !encoded ? NULL : kind.i == 2
      ? d2i_PUBKEY(NULL, &cursor, length)
      : d2i_AutoPrivateKey(NULL, &cursor, length);
   if (!pkey) return cipher_fail(vm, "cannot read the RSA key");
   int size = EVP_PKEY_get_size(pkey);
   EVP_PKEY_free(pkey);
   (void)dvm_get_field(vm, self, "opmode", "I", &op);
   if (op.i == JCE_ENCRYPT || op.i == JCE_WRAP) {
      if (luna_strcasestr(pad, "OAEP")) {
         int digest = 20;
         if (luna_strcasestr(pad, "SHA-224")) digest = 28;
         else if (luna_strcasestr(pad, "SHA-256")) digest = 32;
         else if (luna_strcasestr(pad, "SHA-384")) digest = 48;
         else if (luna_strcasestr(pad, "SHA-512")) digest = 64;
         size -= 2 * digest + 2;
      } else if (!strcasecmp(pad, "PKCS1Padding") || !*pad) size -= 11;
   }
   RETI(size);
}

static bool cipher_getIV(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "iv", "[B", &v);
   if (!v.l) RETL(0);
   uint32_t length = 0;
   const uint8_t *bytes = arg_bytes(vm, v.l, &length);
   RETL(make_bytes(vm, bytes, length));
}

/* --- javax.crypto.Mac --------------------------------------------------- */

/* Keep constant-size host digest state; data_destroy releases both contexts
 * when the Java object is reclaimed or the VM is destroyed. */
struct mac_state { EVP_MD_CTX *initial, *active; };
static void mac_destroy(void *data)
{
   struct mac_state *state = data;
   if (!state) return;
   EVP_MD_CTX_free(state->initial);
   EVP_MD_CTX_free(state->active);
   free(state);
}
static const EVP_MD *mac_type(const char *algorithm)
{
   return algorithm && !strncasecmp(algorithm, "Hmac", 4) ?
      EVP_get_digestbyname(algorithm + 4) : NULL;
}
static struct mac_state *mac_ready(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *object = dvm__obj(vm, self);
   struct mac_state *state = object ? object->data : NULL;
   if (!state) dvm__throw(vm, "java/lang/IllegalStateException", "MAC not initialized");
   return state;
}
static bool mac_getInstance(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *alg = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!alg) {
      dvm__throw(vm, "java/lang/NullPointerException", "algorithm"); return false;
   }
   if (!mac_type(alg)) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s", alg); return false;
   }
   dvm_ref m = dvm_new_object(vm, dvm__class_by_desc(vm, "Ljavax/crypto/Mac;"));
   if (m) (void)dvm_set_field(vm, m, "algorithm", "Ljava/lang/String;", ARG(0));
   RETL(m);
}
static bool mac_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   uint32_t length = 0;
   const uint8_t *bytes = key_bytes(vm, ARG(0).l, &length);
   if (!ARG(0).l || !bytes || !length) {
      dvm__throw(vm, "java/security/InvalidKeyException", "invalid MAC key"); return false;
   }
   union dvm_value algorithm = {0};
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &algorithm);
   const EVP_MD *md = mac_type(dvm_string_utf8(vm, algorithm.l));
   struct mac_state *state = calloc(1, sizeof *state);
   EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, NULL, bytes, length);
   if (state) {
      state->initial = EVP_MD_CTX_new(); state->active = EVP_MD_CTX_new();
   }
   int good = state && state->initial && state->active && key &&
      EVP_DigestSignInit(state->initial, NULL, md, NULL, key) == 1 &&
      EVP_MD_CTX_copy_ex(state->active, state->initial) == 1;
   EVP_PKEY_free(key);
   if (!good) {
      mac_destroy(state);
      dvm__throw(vm, "java/security/InvalidKeyException", "cannot initialize MAC"); return false;
   }
   struct dvm_object *object = dvm__obj(vm, self);
   mac_destroy(object->data);
   object->data = state; object->data_destroy = mac_destroy;
   RETV();
}
static bool mac_reset(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct dvm_object *object = dvm__obj(vm, self);
   struct mac_state *state = object ? object->data : NULL;
   if (state && EVP_MD_CTX_copy_ex(state->active, state->initial) != 1) {
      dvm__throw(vm, "java/security/ProviderException", "MAC reset failed"); return false;
   }
   RETV();
}
static bool mac_update(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)out;
   struct mac_state *state = mac_ready(vm, self);
   if (!state) return false;
   /* Both array overloads ignore null after checking initialization. */
   uint32_t length = 0;
   const uint8_t *bytes = arg_bytes(vm, ARG(0).l, &length);
   if (!ARG(0).l) RETV();
   if (nargs == 3) {
      int32_t offset = ARG(1).i, count = ARG(2).i;
      if (!bytes || offset < 0 || count < 0 || (uint32_t)offset > length ||
          (uint32_t)count > length - (uint32_t)offset) {
         dvm__throw(vm, "java/lang/IllegalArgumentException", "invalid MAC input range"); return false;
      }
      bytes += offset; length = (uint32_t)count;
   }
   if (length && EVP_DigestSignUpdate(state->active, bytes, length) != 1) {
      dvm__throw(vm, "java/security/ProviderException", "MAC update failed"); return false;
   }
   RETV();
}
static bool mac_update_byte(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   struct mac_state *state = mac_ready(vm, self);
   if (!state) return false;
   uint8_t byte = (uint8_t)ARG(0).i;
   if (EVP_DigestSignUpdate(state->active, &byte, 1) != 1) {
      dvm__throw(vm, "java/security/ProviderException", "MAC update failed"); return false;
   }
   RETV();
}
static bool mac_doFinal(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   struct mac_state *state = mac_ready(vm, self);
   if (!state) return false;
   if (nargs && !mac_update(vm, self, args, 1, out)) return false;
   uint8_t bytes[EVP_MAX_MD_SIZE]; size_t length = sizeof bytes;
   if (EVP_DigestSignFinal(state->active, bytes, &length) != 1) {
      dvm__throw(vm, "java/security/ProviderException", "MAC finalization failed"); return false;
   }
   if (!mac_reset(vm, self, NULL, 0, out)) return false;
   RETL(make_bytes(vm, bytes, (uint32_t)length));
}
static bool mac_length(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value algorithm = {0};
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &algorithm);
   const EVP_MD *md = mac_type(dvm_string_utf8(vm, algorithm.l));
   RETI(md ? EVP_MD_get_size(md) : 0);
}

/* --- java.security.Signature ------------------------------------------- */

/* "<digest>with<key algorithm>" (SHA256withRSA, SHA1withECDSA, NONEwithRSA,
 * SHA256withRSA/PSS).  The bytes of update() are kept until sign()/verify(),
 * which is when OpenSSL sees them; a key that does not parse fails there. */
struct sig_alg { char digest[16]; bool none, pss, ecdsa; };

static bool sig_parse(const char *name, struct sig_alg *a)
{
   memset(a, 0, sizeof *a);
   const char *with = name ? luna_strcasestr(name, "with") : NULL;
   if (!with || with == name || (size_t)(with - name) >= sizeof a->digest) return false;
   memcpy(a->digest, name, (size_t)(with - name));
   const char *key = with + 4;
   a->none = !strcasecmp(a->digest, "NONE");
   a->pss = luna_strcasestr(key, "/PSS") != NULL || !strncasecmp(key, "RSASSA-PSS", 10);
   a->ecdsa = !strncasecmp(key, "ECDSA", 5);
   if (!a->ecdsa && strncasecmp(key, "RSA", 3)) return false;
   if (!a->none && !EVP_get_digestbyname(a->digest)) return false;
   return true;
}

static bool sig_getInstance(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *alg = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   struct sig_alg parsed;
   if (!sig_parse(alg, &parsed)) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s", alg ? alg : "null");
      return false;
   }
   dvm_ref sig = dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/security/Signature;"));
   if (sig) {
      (void)dvm_set_field(vm, sig, "algorithm", "Ljava/lang/String;",
                          (union dvm_value){ .l = dvm_new_string(vm, alg) });
      (void)dvm_set_field(vm, sig, "provider", "Ljava/security/Provider;",
                          (union dvm_value){ .l = security_builtin_provider(vm) });
   }
   RETL(sig);
}

static bool sig_init(struct dvm *vm, dvm_ref self, dvm_ref key, int32_t state)
{
   if (!key) {
      dvm__throw(vm, "java/security/InvalidKeyException", "key is null");
      return false;
   }
   (void)dvm_set_field(vm, self, "key", "Ljava/lang/Object;", (union dvm_value){ .l = key });
   (void)dvm_set_field(vm, self, "state", "I", (union dvm_value){ .i = state });
   (void)dvm_set_field(vm, self, "data", "[B", (union dvm_value){ .l = 0 });
   return true;
}

static bool sig_initSign(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!sig_init(vm, self, ARG(0).l, 1)) return false;
   RETV();
}

static bool sig_initVerify(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!sig_init(vm, self, ARG(0).l, 2)) return false;
   RETV();
}

static bool sig_append(struct dvm *vm, dvm_ref self, const uint8_t *p, uint32_t n)
{
   union dvm_value state = { 0 }, old = { 0 };
   (void)dvm_get_field(vm, self, "state", "I", &state);
   if (!state.i) {
      dvm__throw(vm, "java/security/SignatureException", "Signature object not initialized");
      return false;
   }
   (void)dvm_get_field(vm, self, "data", "[B", &old);
   uint32_t old_len = 0;
   const uint8_t *op = arg_bytes(vm, old.l, &old_len);
   uint8_t *all = malloc((size_t)old_len + n + 1);
   if (!all) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature"); return false; }
   if (old_len) memcpy(all, op, old_len);
   if (n) memcpy(all + old_len, p, n);
   dvm_ref merged = make_bytes(vm, all, old_len + n);
   free(all);
   (void)dvm_set_field(vm, self, "data", "[B", (union dvm_value){ .l = merged });
   return true;
}

static bool sig_update(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)out;
   uint32_t len = 0;
   const uint8_t *p = arg_bytes(vm, ARG(0).l, &len);
   if (nargs >= 3) {
      if (!p || ARG(1).i < 0 || ARG(2).i < 0 || (uint32_t)ARG(1).i > len ||
          (uint32_t)ARG(2).i > len - (uint32_t)ARG(1).i) {
         dvm__throw(vm, "java/lang/IllegalArgumentException", "bad offset or length");
         return false;
      }
      p += ARG(1).i; len = (uint32_t)ARG(2).i;
   }
   if (!sig_append(vm, self, p, len)) return false;
   RETV();
}

static bool sig_update_byte(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const uint8_t b = (uint8_t)ARG(0).i;
   if (!sig_append(vm, self, &b, 1)) return false;
   RETV();
}

/* RSASSA-PKCS1-v1_5 built from its two parts: the DER DigestInfo of the hash,
 * then the raw RSA padding and private-key operation.  Going through the
 * digest-bound EVP signature API instead ties the answer to the host's crypto
 * policy: a distribution that refuses SHA-1 signatures would make
 * Signature("SHA1withRSA"), which every Android has, fail. */
static int sig_digest_info(const EVP_MD *md, const uint8_t *data, uint32_t len,
                           uint8_t **der, int *der_len)
{
   /* DigestInfo ::= SEQUENCE { SEQUENCE { OID, NULL }, OCTET STRING }.  Written
    * out rather than asked of X509_ALGOR_set_md, which leaves the NULL
    * parameter off for some digests; RFC 8017 has it present.  Every length
    * here is below 128, so each is a single byte. */
   uint8_t hash[EVP_MAX_MD_SIZE];
   unsigned hash_len = 0;
   const ASN1_OBJECT *oid = OBJ_nid2obj(EVP_MD_get_type(md));
   const int oid_len = oid ? OBJ_length(oid) : 0;
   *der = NULL;
   if (!oid || oid_len <= 0 || oid_len > 16 ||
       !EVP_Digest(data, len, hash, &hash_len, md, NULL) || hash_len > 64)
      return 0;
   const int algorithm = 2 + oid_len + 2;
   const int total = 2 + algorithm + 2 + (int)hash_len;
   uint8_t *out = malloc((size_t)total + 2);
   if (!out) return 0;
   int at = 0;
   out[at++] = 0x30; out[at++] = (uint8_t)total;
   out[at++] = 0x30; out[at++] = (uint8_t)algorithm;
   out[at++] = 0x06; out[at++] = (uint8_t)oid_len;
   memcpy(out + at, OBJ_get0_data(oid), (size_t)oid_len); at += oid_len;
   out[at++] = 0x05; out[at++] = 0x00;
   out[at++] = 0x04; out[at++] = (uint8_t)hash_len;
   memcpy(out + at, hash, hash_len); at += (int)hash_len;
   *der = out; *der_len = at;
   return 1;
}

static int sig_rsa_pkcs1(EVP_PKEY *pkey, const EVP_MD *md, bool verify,
                         const uint8_t *data, uint32_t dlen,
                         const uint8_t *sig_in, uint32_t sig_in_len,
                         uint8_t **sig_out, size_t *sig_out_len)
{
   uint8_t *der = NULL;
   int der_len = 0, result = -2;
   if (!sig_digest_info(md, data, dlen, &der, &der_len)) return -2;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
   if (ctx && (verify ? EVP_PKEY_verify_recover_init(ctx) : EVP_PKEY_sign_init(ctx)) > 0 &&
       EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) > 0) {
      if (verify) {
         size_t n = 0;
         uint8_t *plain = NULL;
         if (EVP_PKEY_verify_recover(ctx, NULL, &n, sig_in, sig_in_len) > 0 &&
             (plain = malloc(n)) != NULL) {
            result = EVP_PKEY_verify_recover(ctx, plain, &n, sig_in, sig_in_len) > 0 &&
                     n == (size_t)der_len && !memcmp(plain, der, n);
            free(plain);
         } else result = 0;   /* a signature that is not a valid RSA block */
      } else {
         size_t n = 0;
         if (EVP_PKEY_sign(ctx, NULL, &n, der, (size_t)der_len) > 0 &&
             (*sig_out = malloc(n)) != NULL &&
             EVP_PKEY_sign(ctx, *sig_out, &n, der, (size_t)der_len) > 0) {
            *sig_out_len = n; result = 1;
         }
      }
   }
   EVP_PKEY_CTX_free(ctx);
   free(der);
   return result;
}

/* Signs or verifies the accumulated bytes; the key and algorithm are read back
 * from the object.  Returns 1/0, or -1 with an exception pending. */
static const char *pkey_failed_exception(const uint8_t *kb)
{
   return kb ? "java/security/SignatureException" : "java/security/InvalidKeyException";
}

static int sig_finish(struct dvm *vm, dvm_ref self, bool verify,
                      const uint8_t *sig_in, uint32_t sig_in_len,
                      uint8_t **sig_out, size_t *sig_out_len)
{
   union dvm_value av = { 0 }, kv = { 0 }, state = { 0 }, data = { 0 };
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &av);
   (void)dvm_get_field(vm, self, "key", "Ljava/lang/Object;", &kv);
   (void)dvm_get_field(vm, self, "state", "I", &state);
   (void)dvm_get_field(vm, self, "data", "[B", &data);
   if (state.i != (verify ? 2 : 1)) {
      dvm__throw(vm, "java/security/SignatureException", "Signature object not initialized");
      return -1;
   }
   struct sig_alg alg;
   if (!sig_parse(av.l ? dvm_string_utf8(vm, av.l) : NULL, &alg)) return -1;
   uint32_t klen = 0, dlen = 0;
   const uint8_t *kb = key_bytes(vm, kv.l, &klen);
   const uint8_t *db = arg_bytes(vm, data.l, &dlen);
   const uint8_t *kp = kb;
   unsigned depth = dvm_gil_unlock_all(vm);
   EVP_PKEY *pkey = !kb ? NULL : verify ? d2i_PUBKEY(NULL, &kp, klen)
                                        : d2i_AutoPrivateKey(NULL, &kp, klen);
   EVP_MD_CTX *mctx = pkey ? EVP_MD_CTX_new() : NULL;
   EVP_PKEY_CTX *pctx = NULL;
   const EVP_MD *md = alg.none ? NULL : EVP_get_digestbyname(alg.digest);
   int result = -2;
   if (alg.none) {
      /* No digest: the caller's bytes are what RSA signs (PKCS#1 v1.5). */
      pctx = pkey ? EVP_PKEY_CTX_new(pkey, NULL) : NULL;
      if (pctx && (verify ? EVP_PKEY_verify_init(pctx) : EVP_PKEY_sign_init(pctx)) > 0 &&
          (alg.ecdsa || EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) > 0)) {
         if (verify) {
            result = EVP_PKEY_verify(pctx, sig_in, sig_in_len, db ? db : (const uint8_t *)"", dlen) == 1;
         } else {
            size_t n = 0;
            if (EVP_PKEY_sign(pctx, NULL, &n, db ? db : (const uint8_t *)"", dlen) > 0 &&
                (*sig_out = malloc(n)) != NULL &&
                EVP_PKEY_sign(pctx, *sig_out, &n, db ? db : (const uint8_t *)"", dlen) > 0) {
               *sig_out_len = n; result = 1;
            }
         }
      }
      EVP_PKEY_CTX_free(pctx);
   } else if (pkey && md && !alg.ecdsa && !alg.pss) {
      result = sig_rsa_pkcs1(pkey, md, verify, db ? db : (const uint8_t *)"", dlen,
                             sig_in, sig_in_len, sig_out, sig_out_len);
   } else if (mctx && md) {
      int init = verify ? EVP_DigestVerifyInit(mctx, &pctx, md, NULL, pkey)
                        : EVP_DigestSignInit(mctx, &pctx, md, NULL, pkey);
      if (init > 0 && alg.pss && !alg.ecdsa &&
          (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0 ||
           EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, -1) <= 0 ||
           EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, md) <= 0))
         init = 0;
      if (init > 0) {
         if (verify) {
            result = EVP_DigestVerify(mctx, sig_in, sig_in_len, db ? db : (const uint8_t *)"", dlen) == 1;
         } else {
            size_t n = 0;
            if (EVP_DigestSign(mctx, NULL, &n, db ? db : (const uint8_t *)"", dlen) > 0 &&
                (*sig_out = malloc(n)) != NULL &&
                EVP_DigestSign(mctx, *sig_out, &n, db ? db : (const uint8_t *)"", dlen) > 0) {
               *sig_out_len = n; result = 1;
            }
         }
      }
   }
   EVP_MD_CTX_free(mctx);
   EVP_PKEY_free(pkey);
   dvm_gil_relock(vm, depth);
   char why[160] = "";
   if (result == -2) ERR_error_string_n(ERR_peek_last_error(), why, sizeof why);
   ERR_clear_error();
   if (result == -2) {
      if (sig_out) { free(*sig_out); *sig_out = NULL; }
      dvm__throw(vm, pkey_failed_exception(kb), "cannot %s with this key (%s: %s)",
                 verify ? "verify" : "sign", av.l ? dvm_string_utf8(vm, av.l) : "?", why);
      return -1;
   }
   /* The object is ready for the next message. */
   (void)dvm_set_field(vm, self, "data", "[B", (union dvm_value){ .l = 0 });
   return result;
}

static bool sig_sign(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   uint8_t *sig = NULL;
   size_t n = 0;
   if (sig_finish(vm, self, false, NULL, 0, &sig, &n) < 0) return false;
   dvm_ref bytes = make_bytes(vm, sig, (uint32_t)n);
   free(sig);
   RETL(bytes);
}

static bool sig_verify(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   uint32_t len = 0;
   const uint8_t *p = arg_bytes(vm, ARG(0).l, &len);
   if (!p) {
      dvm__throw(vm, "java/lang/NullPointerException", "signature");
      return false;
   }
   if (nargs >= 3) {
      if (ARG(1).i < 0 || ARG(2).i < 0 || (uint32_t)ARG(1).i > len ||
          (uint32_t)ARG(2).i > len - (uint32_t)ARG(1).i) {
         dvm__throw(vm, "java/lang/IllegalArgumentException", "bad offset or length");
         return false;
      }
      p += ARG(1).i; len = (uint32_t)ARG(2).i;
   }
   const int ok = sig_finish(vm, self, true, p, len, NULL, NULL);
   if (ok < 0) return false;
   RETI(ok);
}

static bool sig_getAlgorithm(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &v);
   RETL(v.l);
}

static const struct rt_field rt_sig_fields[] = {
   { "algorithm", "Ljava/lang/String;" }, { "provider", "Ljava/security/Provider;" },
   { "key", "Ljava/lang/Object;" }, { "state", "I" }, { "data", "[B" }, F_END
};
static const struct rt_method rt_sig[] = {
   SM("getInstance", "(Ljava/lang/String;)Ljava/security/Signature;", sig_getInstance),
   SM("getInstance", "(Ljava/lang/String;Ljava/lang/String;)Ljava/security/Signature;",
      sig_getInstance),
   M("getAlgorithm", "()Ljava/lang/String;", sig_getAlgorithm),
   M("initSign", "(Ljava/security/PrivateKey;)V", sig_initSign),
   M("initSign", "(Ljava/security/PrivateKey;Ljava/security/SecureRandom;)V", sig_initSign),
   M("initVerify", "(Ljava/security/PublicKey;)V", sig_initVerify),
   M("update", "([B)V", sig_update),
   M("update", "([BII)V", sig_update),
   M("update", "(B)V", sig_update_byte),
   M("sign", "()[B", sig_sign),
   M("verify", "([B)Z", sig_verify),
   M("verify", "([BII)Z", sig_verify),
   M_END
};

/* --- java.security.SecureRandom ----------------------------------------- */

static bool sr_nextBytes(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)self; (void)nargs; (void)out;
   struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   if (a && a->kind == DVM_OBJ_ARRAY && a->data && a->length)
      RAND_bytes(a->data, (int)a->length);
   RETV();
}

static const struct rt_field rt_key_fields[] = {
   { "encoded", "[B" }, { "algorithm", "Ljava/lang/String;" },
   { "kind", "I" }, F_END
};

/* Software RSA uses the crypto backend already linked by the runtime.
 * Never substitute fixed keys, suppress generation failures, or add another
 * crypto dependency. Prime generation runs outside the interpreter lock. */
static bool kpg_instance(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "algorithm");
      return false;
   }
   const char *name = dvm_string_utf8(vm, ARG(0).l);
   if (!name || strcasecmp(name, "RSA")) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s", name ? name : "");
      return false;
   }
   dvm_ref generator = dvm_new_object(vm,
      dvm__class_by_desc(vm, "Ljava/security/KeyPairGenerator;"));
   (void)dvm_set_field(vm, generator, "algorithm", "Ljava/lang/String;",
      (union dvm_value){.l = dvm_new_string(vm, "RSA")});
   (void)dvm_set_field(vm, generator, "keySize", "I", (union dvm_value){.i = 2048});
   RETL(generator);
}

static bool kpg_initialize(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)nargs; (void)out;
   int bits = ARG(0).i;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   int valid = bits > 0 && ctx && EVP_PKEY_keygen_init(ctx) > 0 &&
      EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) > 0;
   EVP_PKEY_CTX_free(ctx);
   if (!valid) {
      dvm__throw(vm, "java/security/InvalidParameterException", "RSA key size %d", bits);
      return false;
   }
   (void)dvm_set_field(vm, self, "keySize", "I", (union dvm_value){.i = bits});
   RETV();
}

static bool kp_init(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "publicKey", "Ljava/security/PublicKey;", ARG(0));
   (void)dvm_set_field(vm, self, "privateKey", "Ljava/security/PrivateKey;", ARG(1));
   RETV();
}

static bool kp_public(struct dvm *vm, dvm_ref self,
                      const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   (void)dvm_get_field(vm, self, "publicKey", "Ljava/security/PublicKey;", out);
   return true;
}

static bool kp_private(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   (void)dvm_get_field(vm, self, "privateKey", "Ljava/security/PrivateKey;", out);
   return true;
}

static bool kpg_generate(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value bits = {0};
   (void)dvm_get_field(vm, self, "keySize", "I", &bits);
   unsigned depth = dvm_gil_unlock_all(vm);
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   EVP_PKEY *key = NULL;
   int ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 &&
      EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits.i) > 0 && EVP_PKEY_keygen(ctx, &key) > 0;
   EVP_PKEY_CTX_free(ctx);
   unsigned char *pub = NULL, *priv = NULL;
   PKCS8_PRIV_KEY_INFO *p8 = ok ? EVP_PKEY2PKCS8(key) : NULL;
   int pub_len = ok ? i2d_PUBKEY(key, &pub) : 0;
   int priv_len = p8 ? i2d_PKCS8_PRIV_KEY_INFO(p8, &priv) : 0;
   PKCS8_PRIV_KEY_INFO_free(p8);
   EVP_PKEY_free(key);
   dvm_gil_relock(vm, depth);
   if (!ok || pub_len <= 0 || priv_len <= 0) {
      OPENSSL_free(pub); OPENSSL_free(priv);
      dvm__throw(vm, "java/security/ProviderException", "RSA key generation failed");
      return false;
   }
   dvm_ref public_key = key_make(vm, kf_key_class("RSA", 2),
      make_bytes(vm, pub, (uint32_t)pub_len), "RSA", 2);
   dvm_ref private_key = key_make(vm, kf_key_class("RSA", 1),
      make_bytes(vm, priv, (uint32_t)priv_len), "RSA", 1);
   OPENSSL_free(pub); OPENSSL_clear_free(priv, (size_t)priv_len);
   dvm_ref pair = dvm_new_object(vm, dvm__class_by_desc(vm, "Ljava/security/KeyPair;"));
   union dvm_value keys[] = {{.l = public_key}, {.l = private_key}}, ignored;
   if (!kp_init(vm, pair, keys, 2, &ignored)) return false;
   RETL(pair);
}

static const struct rt_field rt_kpg_fields[] = {
   {"algorithm", "Ljava/lang/String;"}, {"keySize", "I"}, F_END
};
static const struct rt_field rt_kp_fields[] = {
   {"publicKey", "Ljava/security/PublicKey;"},
   {"privateKey", "Ljava/security/PrivateKey;"}, F_END
};
static const struct rt_method rt_kpg[] = {
   SM("getInstance", "(Ljava/lang/String;)Ljava/security/KeyPairGenerator;", kpg_instance),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M("initialize", "(I)V", kpg_initialize),
   M("initialize", "(ILjava/security/SecureRandom;)V", kpg_initialize),
   M("generateKeyPair", "()Ljava/security/KeyPair;", kpg_generate),
   M("genKeyPair", "()Ljava/security/KeyPair;", kpg_generate), M_END
};
static const struct rt_method rt_kp[] = {
   M("<init>", "(Ljava/security/PublicKey;Ljava/security/PrivateKey;)V", kp_init),
   M("getPublic", "()Ljava/security/PublicKey;", kp_public),
   M("getPrivate", "()Ljava/security/PrivateKey;", kp_private), M_END
};

/* android.security.keystore.KeyGenParameterSpec(+Builder).
 *
 * OSParamsStore migrates device params through Android Keystore: it builds a
 * KeyGenParameterSpec and feeds it to KeyGenerator.  The Builder is just a
 * bag of named parameters — hold them on the object and hand a finished Spec
 * out of build().  Software KeyGenerator already exists; what was missing was
 * this parameter object, not a second crypto engine. */
static bool kgs_builder_init(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs; (void)out;
   union dvm_value alias = { .l = ARG(0).l }, purposes = { .i = ARG(1).i };
   (void)dvm_set_field(vm, self, "keystoreAlias", "Ljava/lang/String;", alias);
   (void)dvm_set_field(vm, self, "purposes", "I", purposes);
   RETV();
}

static bool kgs_builder_build(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm,
      "Landroid/security/keystore/KeyGenParameterSpec;");
   dvm_ref spec = c ? dvm_new_object(vm, c) : 0;
   if (!spec) RETL(0);
   static const char *const copy[] = {
      "keystoreAlias", "blockModes", "encryptionPaddings", "digests",
      "signaturePaddings", NULL
   };
   for (int i = 0; copy[i]; ++i) {
      union dvm_value v = { 0 };
      const char *sig = (i == 0) ? "Ljava/lang/String;" : "[Ljava/lang/String;";
      if (dvm_get_field(vm, self, copy[i], sig, &v) && v.l)
         (void)dvm_set_field(vm, spec, copy[i], sig, v);
   }
   static const char *const copy_int[] = {
      "purposes", "keySize", "randomizedEncryptionRequired", "randomizedSet",
      "userAuthenticationRequired", NULL
   };
   for (int i = 0; copy_int[i]; ++i) {
      union dvm_value v = { 0 };
      (void)dvm_get_field(vm, self, copy_int[i], "I", &v);
      (void)dvm_set_field(vm, spec, copy_int[i], "I", v);
   }
   RETL(spec);
}

static bool kgs_set_block_modes(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
   (void)nargs;
   union dvm_value v = { .l = ARG(0).l };
   (void)dvm_set_field(vm, self, "blockModes", "[Ljava/lang/String;", v);
   RETL(self);
}

static bool kgs_set_enc_paddings(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)nargs;
   union dvm_value v = { .l = ARG(0).l };
   (void)dvm_set_field(vm, self, "encryptionPaddings", "[Ljava/lang/String;", v);
   RETL(self);
}

static bool kgs_set_digests(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)nargs;
   union dvm_value v = { .l = ARG(0).l };
   (void)dvm_set_field(vm, self, "digests", "[Ljava/lang/String;", v);
   RETL(self);
}

static bool kgs_set_key_size(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)nargs;
   /* The size has to survive into the Spec: KeyGenerator.init(spec) reads it
    * to decide how many bits to draw, and a zero there is not "the default",
    * it is an InvalidAlgorithmParameterException on a real device. */
   rt_set_int(vm, self, "keySize", ARG(0).i);
   RETL(self);
}

/* Android's defaults: randomized encryption is required unless the caller
 * turns it off, user authentication is not.  Store the two so build() can
 * carry them and the getters answer what was actually asked for. */
static bool kgs_set_randomized(struct dvm *vm, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
   (void)nargs;
   rt_set_int(vm, self, "randomizedEncryptionRequired", ARG(0).i ? 1 : 0);
   rt_set_int(vm, self, "randomizedSet", 1);
   RETL(self);
}

static bool kgs_set_user_auth(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)nargs;
   rt_set_int(vm, self, "userAuthenticationRequired", ARG(0).i ? 1 : 0);
   RETL(self);
}

static bool kgs_is_randomized(struct dvm *vm, dvm_ref self,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
   (void)args; (void)nargs;
   if (!rt_get_int(vm, self, "randomizedSet")) RETI(1);   /* Android default */
   RETI(rt_get_int(vm, self, "randomizedEncryptionRequired") ? 1 : 0);
}

static bool kgs_is_user_auth(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "userAuthenticationRequired") ? 1 : 0);
}

static bool kgs_set_sig_paddings(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
   (void)nargs;
   union dvm_value v = { .l = ARG(0).l };
   (void)dvm_set_field(vm, self, "signaturePaddings", "[Ljava/lang/String;", v);
   RETL(self);
}

static const struct rt_field rt_kgs_builder_fields[] = {
   { "keystoreAlias", "Ljava/lang/String;" },
   { "purposes", "I" },
   { "keySize", "I" },
   { "randomizedEncryptionRequired", "I" }, { "randomizedSet", "I" },
   { "userAuthenticationRequired", "I" },
   { "blockModes", "[Ljava/lang/String;" },
   { "encryptionPaddings", "[Ljava/lang/String;" },
   { "digests", "[Ljava/lang/String;" },
   { "signaturePaddings", "[Ljava/lang/String;" },
   F_END
};

static const struct rt_method rt_kgs_builder[] = {
   M("<init>", "(Ljava/lang/String;I)V", kgs_builder_init),
   M("setBlockModes",
     "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
     kgs_set_block_modes),
   M("setEncryptionPaddings",
     "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
     kgs_set_enc_paddings),
   M("setDigests",
     "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
     kgs_set_digests),
   M("setKeySize", "(I)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
     kgs_set_key_size),
   M("setSignaturePaddings",
     "([Ljava/lang/String;)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
     kgs_set_sig_paddings),
   M("setRandomizedEncryptionRequired",
     "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
     kgs_set_randomized),
   M("setUserAuthenticationRequired",
     "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
     kgs_set_user_auth),
   M("build", "()Landroid/security/keystore/KeyGenParameterSpec;",
     kgs_builder_build),
   M_END,
};

static bool kgs_get_alias(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "keystoreAlias", "Ljava/lang/String;", &v);
   RETL(v.l);
}

static bool kgs_get_key_size(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "keySize"));
}

static const struct rt_field rt_kgs_fields[] = {
   { "keystoreAlias", "Ljava/lang/String;" },
   { "purposes", "I" },
   { "keySize", "I" },
   { "randomizedEncryptionRequired", "I" }, { "randomizedSet", "I" },
   { "userAuthenticationRequired", "I" },
   { "blockModes", "[Ljava/lang/String;" },
   { "encryptionPaddings", "[Ljava/lang/String;" },
   { "digests", "[Ljava/lang/String;" },
   { "signaturePaddings", "[Ljava/lang/String;" },
   F_END
};

static bool kgs_get_purposes(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "purposes", "I", &v);
   RETI(v.i);
}

/* androidx.security's MasterKeys.validate() reads every parameter back off the
 * spec and compares it with what it asked for; a missing getter is not a
 * missing convenience, it is the key being rejected. */
#define KGS_GET_STR_ARRAY(fn, field)                                         \
   static bool fn(struct dvm *vm, dvm_ref self,                              \
                  const union dvm_value *args, int nargs,                    \
                  union dvm_value *out)                                      \
   {                                                                         \
      (void)args; (void)nargs;                                               \
      union dvm_value v = { 0 };                                             \
      if (dvm_get_field(vm, self, field, "[Ljava/lang/String;", &v) && v.l)   \
         RETL(v.l);                                                          \
      RETL(dvm_new_array(vm, 'L', "Ljava/lang/String;", 0));                 \
   }
KGS_GET_STR_ARRAY(kgs_get_block_modes, "blockModes")
KGS_GET_STR_ARRAY(kgs_get_enc_paddings, "encryptionPaddings")
KGS_GET_STR_ARRAY(kgs_get_digests, "digests")
KGS_GET_STR_ARRAY(kgs_get_sig_paddings, "signaturePaddings")
#undef KGS_GET_STR_ARRAY

static const struct rt_method rt_kgs[] = {
   M("getKeystoreAlias", "()Ljava/lang/String;", kgs_get_alias),
   M("getPurposes", "()I", kgs_get_purposes),
   M("getKeySize", "()I", kgs_get_key_size),
   M("getBlockModes", "()[Ljava/lang/String;", kgs_get_block_modes),
   M("getEncryptionPaddings", "()[Ljava/lang/String;", kgs_get_enc_paddings),
   M("getDigests", "()[Ljava/lang/String;", kgs_get_digests),
   M("getSignaturePaddings", "()[Ljava/lang/String;", kgs_get_sig_paddings),
   M("isRandomizedEncryptionRequired", "()Z", kgs_is_randomized),
   M("isUserAuthenticationRequired", "()Z", kgs_is_user_auth),
   M_END,
};

/* javax.crypto.KeyGenerator.
 *
 * This is a generator, rather than a MasterKeys special case: ordinary JCE
 * callers can initialise it with a bit count, while AndroidKeyStore callers
 * initialise it with KeyGenParameterSpec.  AndroidKeyStore generation also
 * installs the key under the spec's alias in the process-wide KeyStore view,
 * which is the provider contract MasterKeys relies on.  Random generation is
 * host CSPRNG work and all VM mutation stays under the DVM interpreter lock;
 * no additional mutex is needed. */
static const struct rt_field rt_key_generator_fields[] = {
   { "algorithm", "Ljava/lang/String;" },
   { "provider", "Ljava/lang/String;" },
   { "params", "Ljava/security/spec/AlgorithmParameterSpec;" },
   { "keySize", "I" },
   F_END
};

static bool kg_supported(const char *algorithm)
{
   return algorithm &&
      (!strcasecmp(algorithm, "AES") || !strcasecmp(algorithm, "DES") ||
       !strcasecmp(algorithm, "DESede") ||
       !strncasecmp(algorithm, "Hmac", 4));
}

static int32_t kg_default_bits(const char *algorithm)
{
   if (algorithm && !strcasecmp(algorithm, "DES")) return 56;
   if (algorithm && !strcasecmp(algorithm, "DESede")) return 168;
   if (algorithm && !strncasecmp(algorithm, "Hmac", 4)) {
      const char *digest = algorithm + 4;
      const EVP_MD *md = EVP_get_digestbyname(digest);
      int n = md ? EVP_MD_get_size(md) : 0;
      if (n > 0) return n * 8;
   }
   return 128;
}

static bool kg_getInstance(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self;
   const char *algorithm = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!kg_supported(algorithm)) {
      dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s",
                 algorithm ? algorithm : "(null)");
      return false;
   }
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljavax/crypto/KeyGenerator;");
   dvm_ref generator = c ? dvm_new_object(vm, c) : 0;
   if (!generator) RETL(0);
   union dvm_value v = { .l = dvm_new_string(vm, algorithm) };
   (void)dvm_set_field(vm, generator, "algorithm", "Ljava/lang/String;", v);
   if (nargs >= 2 && ARG(1).l) {
      v.l = ARG(1).l;
      (void)dvm_set_field(vm, generator, "provider", "Ljava/lang/String;", v);
   }
   v.i = kg_default_bits(algorithm);
   (void)dvm_set_field(vm, generator, "keySize", "I", v);
   RETL(generator);
}

static bool kg_init_bits(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs; (void)out;
   int32_t bits = ARG(0).i;
   if (bits <= 0 || (bits & 7)) {
      dvm__throw(vm, "java/security/InvalidParameterException",
                 "invalid key size %d", bits);
      return false;
   }
   union dvm_value v = { .i = bits };
   (void)dvm_set_field(vm, self, "keySize", "I", v);
   RETV();
}

static bool kg_init_spec(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   (void)nargs; (void)out;
   dvm_ref params = ARG(0).l;
   if (!params) {
      dvm__throw(vm, "java/security/InvalidAlgorithmParameterException",
                 "params == null");
      return false;
   }
   union dvm_value v = { .l = params };
   (void)dvm_set_field(vm, self, "params",
                       "Ljava/security/spec/AlgorithmParameterSpec;", v);
   union dvm_value size = { 0 };
   if (dvm_get_field(vm, params, "keySize", "I", &size) && size.i > 0)
      (void)dvm_set_field(vm, self, "keySize", "I", size);
   RETV();
}

static bool kg_generateKey(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value algorithm = { 0 }, provider = { 0 }, params = { 0 };
   union dvm_value size = { 0 }, alias = { 0 };
   (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &algorithm);
   (void)dvm_get_field(vm, self, "provider", "Ljava/lang/String;", &provider);
   (void)dvm_get_field(vm, self, "params",
                       "Ljava/security/spec/AlgorithmParameterSpec;", &params);
   (void)dvm_get_field(vm, self, "keySize", "I", &size);
   const char *alg = algorithm.l ? dvm_string_utf8(vm, algorithm.l) : NULL;
   const char *prov = provider.l ? dvm_string_utf8(vm, provider.l) : NULL;
   int32_t bits = size.i > 0 ? size.i : kg_default_bits(alg);
   /* DES and DESede carry parity bits in their encoded keys. */
   int32_t bytes = alg && !strcasecmp(alg, "DES") ? 8
                 : alg && !strcasecmp(alg, "DESede") ? 24 : bits / 8;
   if (bytes <= 0 || bytes > 1024) {
      dvm__throw(vm, "java/security/InvalidParameterException",
                 "invalid key size %d", bits);
      return false;
   }
   dvm_ref encoded = dvm_new_array(vm, 'B', "B", (uint32_t)bytes);
   struct dvm_object *array = encoded ? dvm__obj(vm, encoded) : NULL;
   if (!array || !array->data || RAND_bytes(array->data, bytes) != 1) {
      dvm__throw(vm, "java/security/ProviderException",
                 "CSPRNG failed while generating %s", alg ? alg : "key");
      return false;
   }
   dvm_ref key = key_make(vm, "Ljavax/crypto/SecretKey;", encoded,
                          alg ? alg : "", 0);
   if (!key) RETL(0);

   if (params.l)
      (void)dvm_get_field(vm, params.l, "keystoreAlias",
                          "Ljava/lang/String;", &alias);
   if (prov && !strcasecmp(prov, "AndroidKeyStore")) {
      if (!alias.l) {
         dvm__throw(vm, "java/security/InvalidAlgorithmParameterException",
                    "AndroidKeyStore generation requires an alias");
         return false;
      }
      (void)ks_set_alias(vm, 0, alias.l, key);
   }
   RETL(key);
}

/* ---- AndroidKeyStore persistence -----------------------------------------
 *
 * File layout under <data>/misc/keystore/<alias>.bin (C-friendly binary):
 *   magic "AKS1" (4)
 *   alg_len u32le, alg bytes (no trailing NUL required)
 *   key_len u32le, key bytes
 * Alias characters that cannot be a path component become '_'. */
static void ks_data_dir(char *buf, size_t n)
{
   const char *f = getenv("ANDROID_FILES_DIR");
   if (f && *f) {
      size_t len = strlen(f), tail = strlen("/files");
      if (len > tail && !strcmp(f + len - tail, "/files"))
         snprintf(buf, n, "%.*s/misc/keystore", (int)(len - tail), f);
      else
         snprintf(buf, n, "%s/misc/keystore", f);
      return;
   }
   snprintf(buf, n, "/tmp/lunaria-keystore");
}

static void ks_alias_path(const char *alias, char *buf, size_t n)
{
   char dir[DVM_PATH_MAX - 300];
   ks_data_dir(dir, sizeof dir);
   char safe[256];
   size_t i = 0;
   for (; alias && alias[i] && i < sizeof safe - 1; ++i) {
      unsigned char c = (unsigned char)alias[i];
      safe[i] = (c == '/' || c == '\\' || c < 0x20) ? '_' : (char)c;
   }
   safe[i] = '\0';
   snprintf(buf, n, "%s/%s.bin", dir, safe[0] ? safe : "_");
}

static void ks_persist(struct dvm *vm, dvm_ref alias, dvm_ref key)
{
   const char *aname = alias ? dvm_string_utf8(vm, alias) : NULL;
   if (!aname || !*aname || !key) return;
   union dvm_value enc = { 0 }, alg = { 0 };
   if (!dvm_get_field(vm, key, "encoded", "[B", &enc) || !enc.l) return;
   (void)dvm_get_field(vm, key, "algorithm", "Ljava/lang/String;", &alg);
   uint32_t key_len = 0;
   const uint8_t *key_bytes_p = arg_bytes(vm, enc.l, &key_len);
   if (!key_bytes_p || !key_len || key_len > 4096u) return;
   const char *alg_s = alg.l ? dvm_string_utf8(vm, alg.l) : "AES";
   if (!alg_s) alg_s = "AES";
   uint32_t alg_len = (uint32_t)strlen(alg_s);
   if (alg_len > 256u) alg_len = 256u;

   char dir[DVM_PATH_MAX - 300], path[DVM_PATH_MAX], tmp[DVM_PATH_MAX + 8];
   ks_data_dir(dir, sizeof dir);
   prefs_mkdir_p(dir);
   ks_alias_path(aname, path, sizeof path);
   snprintf(tmp, sizeof tmp, "%s.tmp", path);

   FILE *fp = luna_file_fopen(tmp, "wb");
   if (!fp) return;
   const char magic[4] = { 'A', 'K', 'S', '1' };
   uint8_t le[4];
   int ok = fwrite(magic, 1, 4, fp) == 4;
   le[0] = (uint8_t)alg_len;
   le[1] = (uint8_t)(alg_len >> 8);
   le[2] = (uint8_t)(alg_len >> 16);
   le[3] = (uint8_t)(alg_len >> 24);
   ok = ok && fwrite(le, 1, 4, fp) == 4;
   ok = ok && fwrite(alg_s, 1, alg_len, fp) == alg_len;
   le[0] = (uint8_t)key_len;
   le[1] = (uint8_t)(key_len >> 8);
   le[2] = (uint8_t)(key_len >> 16);
   le[3] = (uint8_t)(key_len >> 24);
   ok = ok && fwrite(le, 1, 4, fp) == 4;
   ok = ok && fwrite(key_bytes_p, 1, key_len, fp) == key_len;
   if (fclose(fp) != 0) ok = 0;
   if (!ok) { luna_file_unlink(tmp); return; }
   if (luna_file_rename(tmp, path) != 0) luna_file_unlink(tmp);
}

static dvm_ref ks_load_one(struct dvm *vm, const char *alias_utf8)
{
   if (!alias_utf8 || !*alias_utf8) return 0;
   char path[DVM_PATH_MAX];
   ks_alias_path(alias_utf8, path, sizeof path);
   FILE *fp = luna_file_fopen(path, "rb");
   if (!fp) return 0;
   char magic[4];
   uint8_t le[4];
   uint32_t alg_len = 0, key_len = 0;
   char alg[257];
   uint8_t keybuf[4096];
   int ok = fread(magic, 1, 4, fp) == 4 &&
            magic[0] == 'A' && magic[1] == 'K' &&
            magic[2] == 'S' && magic[3] == '1';
   ok = ok && fread(le, 1, 4, fp) == 4;
   if (ok)
      alg_len = (uint32_t)le[0] | ((uint32_t)le[1] << 8) |
                ((uint32_t)le[2] << 16) | ((uint32_t)le[3] << 24);
   ok = ok && alg_len > 0 && alg_len < sizeof alg;
   ok = ok && fread(alg, 1, alg_len, fp) == alg_len;
   if (ok) alg[alg_len] = '\0';
   ok = ok && fread(le, 1, 4, fp) == 4;
   if (ok)
      key_len = (uint32_t)le[0] | ((uint32_t)le[1] << 8) |
                ((uint32_t)le[2] << 16) | ((uint32_t)le[3] << 24);
   ok = ok && key_len > 0 && key_len <= sizeof keybuf;
   ok = ok && fread(keybuf, 1, key_len, fp) == key_len;
   fclose(fp);
   if (!ok) return 0;
   dvm_ref encoded = make_bytes(vm, keybuf, key_len);
   return key_make(vm, "Ljavax/crypto/SecretKey;", encoded, alg, 0);
}

static void ks_hydrate(struct dvm *vm, dvm_ref map)
{
   if (!map) return;
   char dir[DVM_PATH_MAX];
   ks_data_dir(dir, sizeof dir);
   luna_directory *d = luna_directory_open(dir);
   if (!d) return;
   luna_directory_entry *de;
   while ((de = luna_directory_read(d))) {
      const char *name = de->d_name;
      size_t n = strlen(name);
      if (n < 5 || strcmp(name + n - 4, ".bin") != 0) continue;
      char alias[256];
      if (n - 4 >= sizeof alias) continue;
      memcpy(alias, name, n - 4);
      alias[n - 4] = '\0';
      dvm_ref key = ks_load_one(vm, alias);
      if (!key) continue;
      dvm_ref a = dvm_new_string(vm, alias);
      union dvm_value kv[2] = { { .l = a }, { .l = key } };
      union dvm_value ignored = { 0 };
      (void)map_put(vm, map, kv, 2, &ignored);
   }
   luna_directory_close(d);
}

static const struct rt_method rt_key_generator[] = {
   SM("getInstance", "(Ljava/lang/String;)Ljavax/crypto/KeyGenerator;",
      kg_getInstance),
   SM("getInstance",
      "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/KeyGenerator;",
      kg_getInstance),
   M("init", "(I)V", kg_init_bits),
   M("init", "(ILjava/security/SecureRandom;)V", kg_init_bits),
   M("init", "(Ljava/security/spec/AlgorithmParameterSpec;)V", kg_init_spec),
   M("init", "(Ljava/security/spec/AlgorithmParameterSpec;Ljava/security/SecureRandom;)V",
     kg_init_spec),
   M("generateKey", "()Ljavax/crypto/SecretKey;", kg_generateKey),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M_END,
};

static const struct rt_field rt_spec_fields[] = {
   { "encoded", "[B" }, { "iv", "[B" }, { "tagLen", "I" }, F_END
};

static const struct rt_field rt_cipher_fields[] = {
   { "provider", "Ljava/security/Provider;" },
   { "transform", "Ljava/lang/String;" }, { "opmode", "I" },
   { "key", "Ljava/lang/Object;" }, { "iv", "[B" },
   { "aad", "[B" }, { "pending", "[B" },
   { "algorithm", "Ljava/lang/String;" }, F_END
};

static const struct rt_method rt_key[] = {
   M("getEncoded", "()[B", key_getEncoded),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M("getFormat", "()Ljava/lang/String;", key_getFormat),
   M_END,
};

static const struct rt_method rt_rsa_public_key[] = {
   M("getEncoded", "()[B", key_getEncoded),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M("getFormat", "()Ljava/lang/String;", key_getFormat),
   M("getModulus", "()Ljava/math/BigInteger;", rsa_getModulus),
   M("getPublicExponent", "()Ljava/math/BigInteger;", rsa_getPublicExponent),
   M_END,
};

static const struct rt_method rt_rsa_private_key[] = {
   M("getEncoded", "()[B", key_getEncoded),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M("getFormat", "()Ljava/lang/String;", key_getFormat),
   M("getModulus", "()Ljava/math/BigInteger;", rsa_getModulus),
   M("getPrivateExponent", "()Ljava/math/BigInteger;", rsa_getPrivateExponent),
   M("getPublicExponent", "()Ljava/math/BigInteger;", rsa_getPublicExponent),
   M_END,
};

static const struct rt_method rt_secret_key[] = {
   M("<init>", "([BLjava/lang/String;)V", secret_init),
   M("<init>", "([BIILjava/lang/String;)V", secret_init),
   M("getEncoded", "()[B", key_getEncoded),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M("getFormat", "()Ljava/lang/String;", key_getFormat),
   M_END,
};

static const struct rt_method rt_key_spec[] = {
   M("<init>", "([B)V", spec_init),
   M("getEncoded", "()[B", key_getEncoded),
   M_END,
};

static const char *const rt_iface_key_spec[] = {"Ljava/security/spec/KeySpec;",NULL};
static const struct rt_field rt_encoded_key_spec_fields[] = {
   {"encoded","[B"},F_END
};
static const struct rt_field rt_rsa_public_spec_fields[] = {
   {"modulus","Ljava/math/BigInteger;"},
   {"publicExponent","Ljava/math/BigInteger;"},F_END
};
static const struct rt_method rt_rsa_public_spec[] = {
   M("<init>","(Ljava/math/BigInteger;Ljava/math/BigInteger;)V",rsa_public_spec_init),
   M("getModulus","()Ljava/math/BigInteger;",rsa_public_spec_modulus),
   M("getPublicExponent","()Ljava/math/BigInteger;",rsa_public_spec_exponent),
   M_END
};

/* IvParameterSpec / GCMParameterSpec both surface the IV through `iv`. */
static bool iv_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)out;
   /* IvParameterSpec(byte[,off,len]) and
    * GCMParameterSpec(tagLen,byte[,off,len]) both make a defensive copy. */
   /* The constructor is told apart by its arity (Iv: 1 or 3, GCM: 2 or 4),
    * never by what the first argument happens to hold: GCM's tag length is an
    * int whose bits can equal a live object's reference. */
   const bool gcm = (nargs & 1) == 0;
   const int byte_arg = gcm ? 1 : 0;
   dvm_ref source = byte_arg < nargs ? args[byte_arg].l : 0;
   uint32_t source_len = 0;
   const uint8_t *source_data = arg_bytes(vm, source, &source_len);
   int offset_arg = byte_arg + 1, length_arg = byte_arg + 2;
   int32_t offset = 0, length = (int32_t)source_len;
   if (nargs > length_arg) {
      offset = args[offset_arg].i;
      length = args[length_arg].i;
   }
   if (!source_data || offset < 0 || length < 0 ||
       (uint32_t)offset > source_len ||
       (uint32_t)length > source_len - (uint32_t)offset) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "invalid IV range");
      return false;
   }
   union dvm_value v = {
      .l = make_bytes(vm, source_data + offset, (uint32_t)length)
   };
   (void)dvm_set_field(vm, self, "iv", "[B", v);
   if (gcm) {
      v.i = ARG(0).i;
      (void)dvm_set_field(vm, self, "tagLen", "I", v);
   }
   RETV();
}

static bool iv_getIV(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   return cipher_getIV(vm, self, args, nargs, out);
}

static bool gcm_getTLen(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "tagLen"));
}

static const struct rt_method rt_iv_spec[] = {
   M("<init>", "([B)V", iv_init),
   M("<init>", "([BII)V", iv_init),
   M("<init>", "(I[B)V", iv_init),
   M("<init>", "(I[BII)V", iv_init),
   M("getIV", "()[B", iv_getIV),
   M("getTLen", "()I", gcm_getTLen),
   M_END,
};

static const struct rt_method rt_key_factory[] = {
   SM("getInstance", "(Ljava/lang/String;)Ljava/security/KeyFactory;",
      kf_getInstance),
   SM("getInstance",
      "(Ljava/lang/String;Ljava/lang/String;)Ljava/security/KeyFactory;",
      kf_getInstance),
   M("generatePrivate",
     "(Ljava/security/spec/KeySpec;)Ljava/security/PrivateKey;",
     kf_generatePrivate),
   M("generatePublic",
     "(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;",
     kf_generatePublic),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M_END,
};

static const struct rt_method rt_cipher[] = {
   SM("getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;",
      cipher_getInstance),
   SM("getInstance", "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/Cipher;",
      cipher_getInstance_named),
   SM("getInstance", "(Ljava/lang/String;Ljava/security/Provider;)Ljavax/crypto/Cipher;",
      cipher_getInstance_provider),
   M("init", "(ILjava/security/Key;)V", cipher_init),
   M("init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V",
     cipher_init),
   M("init", "(ILjava/security/Key;Ljava/security/SecureRandom;)V", cipher_init),
   M("doFinal", "([B)[B", cipher_doFinal),
   M("doFinal", "()[B", cipher_doFinal),
   M("update", "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;)I", cipher_update_buffers),
   M("update", "([B)[B", cipher_update_array),
   M("update", "([BII)[B", cipher_update_array),
   M("doFinal", "([BII)[B", cipher_doFinal),
   M("doFinal", "([BII[BI)I", cipher_doFinal_into),
   M("updateAAD", "([B)V", cipher_updateAAD),
   M("updateAAD", "([BII)V", cipher_updateAAD),
   M("getIV", "()[B", cipher_getIV),
   M("getBlockSize", "()I", cipher_getBlockSize),
   M("getProvider", "()Ljava/security/Provider;", cipher_getProvider),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M_END,
};

static const struct rt_field rt_mac_fields[] = {
   { "algorithm", "Ljava/lang/String;" }, F_END
};
static const struct rt_method rt_mac[] = {
   SM("getInstance", "(Ljava/lang/String;)Ljavax/crypto/Mac;", mac_getInstance),
   M("init", "(Ljava/security/Key;)V", mac_init),
   M("update", "([B)V", mac_update),
   M("update", "([BII)V", mac_update),
   M("update", "(B)V", mac_update_byte),
   M("reset", "()V", mac_reset),
   M("getMacLength", "()I", mac_length),
   M("doFinal", "()[B", mac_doFinal),
   M("doFinal", "([B)[B", mac_doFinal),
   M("getAlgorithm", "()Ljava/lang/String;", key_getAlgorithm),
   M_END,
};

/* SecureRandom extends Random, so the whole Random surface has to work off the
 * same CSPRNG — the SDKs pick nextInt()/nextLong() as often as nextBytes(). */
static bool sr_rand64(uint64_t *value)
{
   unsigned char buf[8];
   if (RAND_bytes(buf, (int)sizeof buf) != 1) return false;
   *value = 0;
   for (size_t i = 0; i < sizeof buf; ++i) *value = (*value << 8) | buf[i];
   return true;
}

static bool sr_nextInt(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)vm; (void)self;
   uint64_t r = 0;
   if (!sr_rand64(&r)) RETI(0);
   if (nargs < 1) RETI((int32_t)(uint32_t)r);
   int32_t bound = ARG(0).i;
   if (bound <= 0) RETI(0);   /* Random.nextInt(<=0) is an error, not a value */
   RETI((int32_t)(r % (uint64_t)bound));
}

static bool sr_nextLong(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   uint64_t r = 0;
   if (!sr_rand64(&r)) RETJ(0);
   RETJ((int64_t)r);
}

static bool sr_nextBoolean(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   uint64_t r = 0;
   if (!sr_rand64(&r)) RETI(0);
   RETI((int32_t)(r & 1u));
}

static bool sr_nextDouble(struct dvm *vm, dvm_ref self,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   uint64_t r = 0;
   if (!sr_rand64(&r)) RETD(0.0);
   RETD((double)(r >> 11) / (double)(1ULL << 53));
}

static bool sr_nextFloat(struct dvm *vm, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   union dvm_value d = { 0 };
   if (!sr_nextDouble(vm, self, args, nargs, &d)) return false;
   RETF((float)d.d);
}

static bool sr_getSeed(struct dvm *vm, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
   (void)self; (void)nargs;
   int32_t n = ARG(0).i;
   if (n < 0) n = 0;
   dvm_ref a = dvm_new_array(vm, 'B', "B", n);
   struct dvm_object *o = a ? dvm__obj(vm, a) : NULL;
   if (o && o->data && n) RAND_bytes(o->data, n);
   RETL(a);
}

static bool sr_getInstance(struct dvm *vm, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/security/SecureRandom;");
   RETL(c ? dvm_new_object(vm, c) : 0);
}

static bool sr_getAlgorithm(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(dvm_new_string(vm, "SHA1PRNG"));
}

/* SecureRandom's seeding calls.  Its contract is explicit that a given seed
 * *supplements* rather than replaces the generator's existing state — the
 * opposite of java.util.Random, where the seed is the whole state — so mixing
 * it into the CSPRNG is both what the API promises and the only thing that
 * would be safe to do with it. */
static bool sr_setSeed_bytes(struct dvm *vm, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
   (void)self; (void)nargs;
   const void *b = dvm_array_data(vm, ARG(0).l);
   uint32_t n = dvm_array_length(vm, ARG(0).l);
   if (b && n) RAND_seed(b, (int)n);
   RETV();
}

static bool sr_setSeed_long(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
   (void)vm; (void)self; (void)nargs;
   int64_t v = ARG(0).j;
   RAND_seed(&v, (int)sizeof v);
   RETV();
}

static const struct rt_method rt_secure_random[] = {
   /* Constructing a SecureRandom picks a provider; here there is one CSPRNG
    * and it needs no per-instance state, so there is nothing to construct.
    * The seeding constructor is a different matter — see setSeed below. */
   M("<init>", "()V", empty_void),
   M("<init>", "([B)V", sr_setSeed_bytes),
   SM("getInstance", "(Ljava/lang/String;)Ljava/security/SecureRandom;",
      sr_getInstance),
   SM("getInstanceStrong", "()Ljava/security/SecureRandom;", sr_getInstance),
   SM("getSeed", "(I)[B", sr_getSeed),
   M("generateSeed", "(I)[B", sr_getSeed),
   M("setSeed", "(J)V", sr_setSeed_long),
   M("setSeed", "([B)V", sr_setSeed_bytes),
   M("nextBytes", "([B)V", sr_nextBytes),
   M("nextInt", "()I", sr_nextInt),
   M("nextInt", "(I)I", sr_nextInt),
   M("nextLong", "()J", sr_nextLong),
   M("nextBoolean", "()Z", sr_nextBoolean),
   M("nextFloat", "()F", sr_nextFloat),
   M("nextDouble", "()D", sr_nextDouble),
   M("getAlgorithm", "()Ljava/lang/String;", sr_getAlgorithm),
   M_END,
};


