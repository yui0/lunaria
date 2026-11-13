/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* More of the platform the system apps reach for: collections, cancellation,
 * writers, trust anchors, and a handful of statics.  Included once, after
 * dvm_extras_system.h, before the class tables. */

/* ---- ArraySet: a HashSet whose elements can also be read by position ---------
 * (the set keeps insertion order, so the position is stable until a removal) */
static bool x_set_valueAt(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)nargs;
  struct rt_list *l = list_of(vm, self);
  if (!l || args[0].i < 0 || (uint32_t)args[0].i >= l->size) {
    dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "index=%d size=%u", (int)args[0].i, l ? l->size : 0u);
    return false;
  }
  RETL(l->items[args[0].i]);
}
static bool x_set_indexOf(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)nargs;
  struct rt_list *l = list_of(vm, self);
  for (uint32_t i = 0; l && i < l->size; ++i) {
    bool eq = false;
    if (!am_equals(vm, args[0].l, l->items[i], &eq)) return false;
    if (eq) RETI((int32_t)i);
  }
  RETI(-1);
}
static bool x_set_removeAt(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)nargs;
  struct rt_list *l = list_of(vm, self);
  if (!l || args[0].i < 0 || (uint32_t)args[0].i >= l->size) {
    dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "index=%d size=%u", (int)args[0].i, l ? l->size : 0u);
    return false;
  }
  dvm_ref old = l->items[args[0].i];
  memmove(&l->items[args[0].i], &l->items[args[0].i + 1], (l->size - (uint32_t)args[0].i - 1) * sizeof l->items[0]);
  --l->size;
  ++l->mod_count;
  RETL(old);
}
static bool x_set_add_void(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  union dvm_value added = { 0 };
  (void)nargs;
  (void)out;
  if (!list_add(vm, self, args, 1, &added)) return false;
  RETV();
}
static const struct rt_method rt_array_set_more[] = {
  M("<init>", "()V", list_init),
  M("<init>", "(I)V", list_init),
  M("<init>", "(Ljava/util/Collection;)V", list_init_coll),
  M("valueAt", "(I)Ljava/lang/Object;", x_set_valueAt),
  M("indexOf", "(Ljava/lang/Object;)I", x_set_indexOf),
  M("removeAt", "(I)Ljava/lang/Object;", x_set_removeAt),
  M("append", "(Ljava/lang/Object;)V", x_set_add_void),
  M_END,
};

/* ---- CancellationSignal ----------------------------------------------------- */
static const struct rt_field rt_cancellation_fields[] = {
  { "cancelled", "Z" }, { "listener", "Landroid/os/CancellationSignal$OnCancelListener;" }, F_END,
};
static bool x_cancel_listener_call(struct dvm *vm, dvm_ref listener)
{
  union dvm_value ignored = { 0 };
  return !listener || am_vcall(vm, listener, "onCancel", "()V", NULL, 0, &ignored);
}
static bool x_cancel(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value was = { 0 }, listener = { 0 }, one = { .i = 1 };
  (void)dvm_get_field(vm, self, "cancelled", "Z", &was);
  if (was.i) RETV();
  (void)dvm_set_field(vm, self, "cancelled", "Z", one);
  (void)dvm_get_field(vm, self, "listener", "Landroid/os/CancellationSignal$OnCancelListener;", &listener);
  if (!x_cancel_listener_call(vm, listener.l)) return false;
  RETV();
}
static bool x_is_cancelled(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "cancelled", "Z", &v);
  RETI(v.i != 0);
}
static bool x_throw_if_cancelled(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  union dvm_value v = { 0 };
  (void)args;
  (void)nargs;
  (void)out;
  (void)dvm_get_field(vm, self, "cancelled", "Z", &v);
  if (v.i) {
    dvm__throw(vm, "android/os/OperationCanceledException", "The operation has been canceled.");
    return false;
  }
  RETV();
}
static bool x_set_cancel_listener(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_set_field(vm, self, "listener", "Landroid/os/CancellationSignal$OnCancelListener;", args[0]);
  (void)dvm_get_field(vm, self, "cancelled", "Z", &v);
  if (v.i && !x_cancel_listener_call(vm, args[0].l)) return false;   /* already cancelled: told at once */
  RETV();
}
static const struct rt_method rt_cancellation_signal[] = {
  M("<init>", "()V", empty_void),
  M("cancel", "()V", x_cancel),
  M("isCanceled", "()Z", x_is_cancelled),
  M("throwIfCanceled", "()V", x_throw_if_cancelled),
  M("setOnCancelListener", "(Landroid/os/CancellationSignal$OnCancelListener;)V", x_set_cancel_listener),
  M_END,
};

/* ---- CharArrayWriter: a Writer into a growing char buffer ---------------------
 * StringWriter's buffer is the same thing; the array forms are added. */
static bool x_writer_write_int(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
  union dvm_value c = { .i = args[0].i & 0xffff };
  (void)nargs;
  (void)out;
  return sb_append_char(vm, self, &c, 1, out);
}
static bool x_writer_write_string_range(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                        int nargs, union dvm_value *out)
{
  /* write(String, off, len) */
  (void)nargs;
  union dvm_value r[3] = { args[0], { .i = args[1].i }, { .i = args[1].i + args[2].i } };
  return sb_append_range(vm, self, r, 3, out);
}
static bool x_cw_toCharArray(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
  union dvm_value s = { 0 };
  (void)args;
  (void)nargs;
  if (!sb_toString(vm, self, NULL, 0, &s) || !s.l) return false;
  return am_vcall(vm, s.l, "toCharArray", "()[C", NULL, 0, out);
}
static bool x_cw_reset(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
  union dvm_value zero = { .i = 0 };
  (void)args;
  (void)nargs;
  (void)out;
  return sb_setLength(vm, self, &zero, 1, out);
}
static const struct rt_method rt_char_array_writer[] = {
  M("<init>", "()V", sb_init),
  M("<init>", "(I)V", sb_init),
  M("write", "(I)V", x_writer_write_int),
  M("write", "([CII)V", sb_append_chars),
  M("write", "(Ljava/lang/String;II)V", x_writer_write_string_range),
  M("write", "(Ljava/lang/String;)V", sb_append_str),
  M("append", "(Ljava/lang/CharSequence;)Ljava/io/Writer;", sb_append_str),
  M("append", "(C)Ljava/io/Writer;", sb_append_char),
  M("toCharArray", "()[C", x_cw_toCharArray),
  M("toString", "()Ljava/lang/String;", sb_toString),
  M("size", "()I", sb_length),
  M("reset", "()V", x_cw_reset),
  M("flush", "()V", empty_void),
  M("close", "()V", empty_void),
  M_END,
};

/* ---- java.security.cert.TrustAnchor ---------------------------------------- */
static const struct rt_field rt_trust_anchor_fields[] = {
  { "cert", "Ljava/security/cert/X509Certificate;" }, { "name", "Ljava/lang/String;" },
  { "key", "Ljava/security/PublicKey;" }, { "constraints", "[B" }, F_END,
};
static bool x_trust_anchor_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *sig = dvm__builtin_method()->sig;
  if (!strncmp(sig, "(Ljava/security/cert/X509Certificate;", 37)) {
    if (!args[0].l) {
      dvm__throw(vm, "java/lang/NullPointerException", "the trustedCert parameter must be non-null");
      return false;
    }
    (void)dvm_set_field(vm, self, "cert", "Ljava/security/cert/X509Certificate;", args[0]);
    (void)dvm_set_field(vm, self, "constraints", "[B", args[1]);
  } else {
    /* (String caName, PublicKey, byte[]) or (X500Principal, PublicKey, byte[]) */
    if (!args[0].l || !args[1].l) {
      dvm__throw(vm, "java/lang/NullPointerException", "the caName and pubKey parameters must be non-null");
      return false;
    }
    (void)dvm_set_field(vm, self, "name", "Ljava/lang/String;", args[0]);
    (void)dvm_set_field(vm, self, "key", "Ljava/security/PublicKey;", args[1]);
    (void)dvm_set_field(vm, self, "constraints", "[B", args[2]);
  }
  RETV();
}
static bool x_trust_anchor_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const char *name = dvm__builtin_method()->name;
  union dvm_value v = { 0 };
  if (!strcmp(name, "getTrustedCert")) (void)dvm_get_field(vm, self, "cert", "Ljava/security/cert/X509Certificate;", &v);
  else if (!strcmp(name, "getCAName")) (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &v);
  else if (!strcmp(name, "getCAPublicKey")) (void)dvm_get_field(vm, self, "key", "Ljava/security/PublicKey;", &v);
  else (void)dvm_get_field(vm, self, "constraints", "[B", &v);
  RETL(v.l);
}
static const struct rt_method rt_trust_anchor[] = {
  M("<init>", "(Ljava/security/cert/X509Certificate;[B)V", x_trust_anchor_init),
  M("<init>", "(Ljava/lang/String;Ljava/security/PublicKey;[B)V", x_trust_anchor_init),
  M("getTrustedCert", "()Ljava/security/cert/X509Certificate;", x_trust_anchor_get),
  M("getCAName", "()Ljava/lang/String;", x_trust_anchor_get),
  M("getCAPublicKey", "()Ljava/security/PublicKey;", x_trust_anchor_get),
  M("getNameConstraints", "()[B", x_trust_anchor_get),
  M_END,
};

/* Abstract bases of the JSSE service classes: a subclass (a TLS provider) runs
 * their constructor and has nothing of theirs to initialise. */
static const struct rt_method rt_empty_constructor[] = { M("<init>", "()V", empty_void), M_END };

/* ---- Thread.holdsLock(Object) ----------------------------------------------- */
static bool x_thread_holds_lock(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if (!args[0].l) {
    dvm__throw(vm, "java/lang/NullPointerException", "object");
    return false;
  }
  RETI(dvm__monitor_held(vm, args[0].l));
}
static const struct rt_method rt_x_thread_more[] = {
  SM("holdsLock", "(Ljava/lang/Object;)Z", x_thread_holds_lock), M_END,
};

/* ---- Condition.awaitNanos / awaitUninterruptibly ---------------------------- */
static bool x_condition_await_nanos(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)nargs;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (args[0].j > 0) {
    const uint64_t ms = ((uint64_t)args[0].j + 999999u) / 1000000u;
    bool notified = false;
    if (!dvm__monitor_wait(vm, lock_target(vm, self), ms, &notified)) return false;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const int64_t spent = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
  RETJ(args[0].j - spent);
}
static const struct rt_method rt_x_condition_more[] = {
  M("awaitNanos", "(J)J", x_condition_await_nanos), M_END,
};

/* ThreadPoolExecutor's hooks are empty in the platform; a subclass overrides them. */
static const struct rt_method rt_x_thread_pool_more[] = {
  M("beforeExecute", "(Ljava/lang/Thread;Ljava/lang/Runnable;)V", empty_void),
  M("afterExecute", "(Ljava/lang/Runnable;Ljava/lang/Throwable;)V", empty_void),
  M("terminated", "()V", empty_void),
  M_END,
};

/* ---- small statics ------------------------------------------------------------ */
static bool x_null_static(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETL(0);
}
static bool x_true(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(1);
}
static bool x_heap_allocated(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  const char *name = dvm__builtin_method()->name;
#ifdef __GLIBC__ /* mallinfo2 is glibc-only; other hosts report zeros. */
  const struct mallinfo2 mi = mallinfo2();
  RETJ(!strcmp(name, "getNativeHeapFreeSize") ? (int64_t)mi.fordblks :
       !strcmp(name, "getNativeHeapSize") ? (int64_t)(mi.arena + mi.hblkhd) : (int64_t)(mi.uordblks + mi.hblkhd));
#else
  (void)name;
  RETJ(0);
#endif
}
static const struct rt_method rt_x_debug_more[] = {
  SM("getNativeHeapAllocatedSize", "()J", x_heap_allocated),
  SM("getNativeHeapSize", "()J", x_heap_allocated),
  SM("getNativeHeapFreeSize", "()J", x_heap_allocated),
  M_END,
};
/* Tracing is off for this process: a section has nowhere to go. */
static const struct rt_method rt_x_trace_more[] = {
  SM("beginAsyncSection", "(Ljava/lang/String;I)V", empty_void),
  SM("endAsyncSection", "(Ljava/lang/String;I)V", empty_void),
  SM("setCounter", "(Ljava/lang/String;J)V", empty_void),
  M_END,
};
/* UserHandle.isOwner(): the device has one user, user 0. */
static const struct rt_method rt_x_user_handle_more[] = {
  M("isOwner", "()Z", x_true), SM("isOwner", "()Z", x_true), M_END,
};
/* LruCache.evictAll() is trimToSize(-1): every entry goes. */
static const struct rt_method rt_x_lru_more[] = {
  M("evictAll", "()V", map_clear), M_END,
};
/* Security properties include the values installed by a JCA provider. */
static const struct rt_method rt_x_security_more[] = {
  SM("getProperty", "(Ljava/lang/String;)Ljava/lang/String;", security_property), M_END,
};
/* There is no navigation bar to colour. */
static const struct rt_method rt_x_window_more[] = {
  M("setNavigationBarDividerColor", "(I)V", empty_void), M_END,
};

/* MessageDigest(String algorithm): a provider's own digest keeps the name it was given. */
static const struct rt_field rt_unused_fields[] = { F_END };
static bool x_md_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
  (void)nargs;
  (void)dvm_set_field(vm, self, "algorithm", "Ljava/lang/String;", args[0]);
  RETV();
}
static const struct rt_method rt_x_message_digest_more[] = {
  M("<init>", "(Ljava/lang/String;)V", x_md_init), M_END,
};

/* ---- XmlResourceParser.getAttributeIntValue(namespace, name, default) / (index, default)
 * The attribute's text as an integer ("12", "-3", "0x10"), else the default. */
static bool x_xml_attribute_int(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)nargs;
  union dvm_value text = { 0 };
  const bool by_name = dvm__builtin_method()->sig[1] == 'L';
  const int32_t fallback = by_name ? args[2].i : args[1].i;
  if (by_name) {
    if (!am_vcall(vm, self, "getAttributeValue", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", args, 2, &text)) return false;
  } else if (!am_vcall(vm, self, "getAttributeValue", "(I)Ljava/lang/String;", args, 1, &text)) return false;
  const char *t = text.l ? dvm_string_utf8(vm, text.l) : NULL;
  char *end = NULL;
  if (t && *t) {
    const long long n = strtoll(t, &end, 0);
    if (end != t && *end == 0 && n >= INT32_MIN && n <= INT32_MAX) RETI((int32_t)n);
  }
  RETI(fallback);
}
static const struct rt_method rt_x_xml_parser_more[] = {
  M("getAttributeIntValue", "(Ljava/lang/String;Ljava/lang/String;I)I", x_xml_attribute_int),
  M("getAttributeIntValue", "(II)I", x_xml_attribute_int),
  M_END,
};

/* ---- PackageManager.getSystemSharedLibraryNames(): what a Pixel image ships ---- */
static bool x_pm_shared_library_names(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                      int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  static const char *const names[] = {
    "android.ext.services", "android.ext.shared", "android.test.base", "android.test.mock",
    "android.test.runner", "com.android.future.usb.accessory", "com.android.location.provider",
    "com.android.media.remotedisplay", "com.android.mediadrm.signer", "com.google.android.maps",
    "javax.obex", "org.apache.http.legacy",
  };
  const uint32_t n = sizeof names / sizeof names[0];
  dvm_ref array = dvm_new_array(vm, 'L', "Ljava/lang/String;", n);
  if (!array) return false;
  dvm_pin(vm, array);
  for (uint32_t i = 0; i < n; ++i) ((dvm_ref *)dvm_array_data(vm, array))[i] = dvm_new_string(vm, names[i]);
  dvm_unpin(vm, array);
  RETL(array);
}
static const struct rt_method rt_x_package_manager_libs[] = {
  M("getSystemSharedLibraryNames", "()[Ljava/lang/String;", x_pm_shared_library_names), M_END,
};

/* ---- AssetManager.getLocales(): the locales the app's resources are written for ----
 * (the app's resource table is looked at through the device locale list here) */
static bool x_asset_locales(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  dvm_ref array = dvm_new_array(vm, 'L', "Ljava/lang/String;", 2);
  if (!array) return false;
  dvm_pin(vm, array);
  ((dvm_ref *)dvm_array_data(vm, array))[0] = dvm_new_string(vm, "");        /* the default resources */
  ((dvm_ref *)dvm_array_data(vm, array))[1] = dvm_new_string(vm, "en-US");
  dvm_unpin(vm, array);
  RETL(array);
}
static const struct rt_method rt_x_asset_manager_more[] = {
  M("getLocales", "()[Ljava/lang/String;", x_asset_locales), M_END,
};

/* ---- SSLContext backed by a provider's SSLContextSpi ---------------------------
 *
 * SSLContext.getInstance(protocol, provider) asks that provider for the class
 * registered as "SSLContext.<protocol>" and the context hands every call to an
 * instance of it.  Play services and the Play Store carry their own TLS
 * (Conscrypt) and reach it only this way.  A context made without a provider
 * keeps the built-in engine. */
static const struct rt_field rt_ssl_context_spi_fields[] = {
  { "spi", "Ljavax/net/ssl/SSLContextSpi;" }, { "provider", "Ljava/security/Provider;" },
  { "protocol", "Ljava/lang/String;" }, F_END,
};
static dvm_ref ssl_spi_of(struct dvm *vm, dvm_ref context)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, context, "spi", "Ljavax/net/ssl/SSLContextSpi;", &v);
  return v.l;
}
static bool x_ssl_context_get_instance_provider(struct dvm *vm, dvm_ref self, const union dvm_value *args,
    int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *protocol = args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
  if (!protocol) {
    dvm__throw(vm, "java/lang/NullPointerException", "protocol");
    return false;
  }
  struct dvm_class *cc = dvm__class_by_desc(vm, "Ljavax/net/ssl/SSLContext;");
  dvm_ref context = cc ? dvm_new_object(vm, cc) : 0;
  if (!context) RETL(0);
  dvm_pin(vm, context);
  (void)dvm_set_field(vm, context, "protocol", "Ljava/lang/String;", args[0]);
  (void)dvm_set_field(vm, context, "provider", "Ljava/security/Provider;", args[1]);
  char key[160];
  snprintf(key, sizeof key, "SSLContext.%s", protocol);
  union dvm_value k = { .l = dvm_new_string(vm, key) }, name = { 0 };
  if (args[1].l && am_vcall(vm, args[1].l, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", &k, 1, &name) && name.l) {
    char cls[256];
    snprintf(cls, sizeof cls, "%s", dvm_string_utf8(vm, name.l));
    for (char *q = cls; *q; ++q) if (*q == '.') *q = '/';
    struct dvm_class *sc = dvm_find_class(vm, cls);
    dvm_ref spi = sc ? dvm_new_object(vm, sc) : 0;
    union dvm_value ignored = { 0 };
    if (spi) {
      dvm_pin(vm, spi);
      if (!am_vcall(vm, spi, "<init>", "()V", NULL, 0, &ignored)) {
        dvm_unpin(vm, spi);
        dvm_unpin(vm, context);
        return false;
      }
      (void)dvm_set_field(vm, context, "spi", "Ljavax/net/ssl/SSLContextSpi;", (union dvm_value) {
        .l = spi
      });
      dvm_unpin(vm, spi);
    }
  }
  /* The Default SPI initializes itself in its constructor. Java forbids
   * callers from initializing that SPI again, so its wrapper is ready now. */
  if (!strcasecmp(protocol, "Default"))
    dvm_set_field(vm, context, "initialized", "Z", (union dvm_value) {
    .i = 1
  });
  (void)self;
  dvm_unpin(vm, context);
  RETL(context);
}
static bool x_ssl_context_provider(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "provider", "Ljava/security/Provider;", &v);
  RETL(v.l ? v.l : security_builtin_provider(vm));
}
static bool x_ssl_context_protocol(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "protocol", "Ljava/lang/String;", &v);
  RETL(v.l ? v.l : dvm_new_string(vm, "TLS"));
}
/* init / createSSLEngine / socket factories: the provider's engine*() methods. */
static bool x_ssl_context_delegate(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  dvm_ref spi = ssl_spi_of(vm, self);
  const struct dvm_method *m = dvm__builtin_method();
  /* Sockets are the host's TLS (the platform's own socket layer); what the
   * built-in context cannot make -- an SSLEngine, session contexts, the
   * parameter sets -- is the provider's. */
  if (!spi) {
    dvm__throw(vm, "java/lang/UnsupportedOperationException", "SSLContext.%s", m->name);
    return false;
  }
  static const struct {
    const char *api, *spi_name;
  } map[] = {
    { "getSocketFactory", "engineGetSocketFactory" }, { "getServerSocketFactory", "engineGetServerSocketFactory" },
    { "createSSLEngine", "engineCreateSSLEngine" }, { "getClientSessionContext", "engineGetClientSessionContext" },
    { "getServerSessionContext", "engineGetServerSessionContext" }, { "getDefaultSSLParameters", "engineGetDefaultSSLParameters" },
    { "getSupportedSSLParameters", "engineGetSupportedSSLParameters" }, { "init", "engineInit" },
  };
  for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i) {
    if (strcmp(m->name, map[i].api)) continue;
    char sig[256];
    snprintf(sig, sizeof sig, "%s", m->sig);
    return am_vcall(vm, spi, map[i].spi_name, sig, args, nargs, out);
  }
  dvm__throw(vm, "java/lang/UnsupportedOperationException", "SSLContext.%s", m->name);
  return false;
}
static const struct rt_method rt_x_ssl_context_more[] = {
  M("getProvider", "()Ljava/security/Provider;", x_ssl_context_provider),
  M("getProtocol", "()Ljava/lang/String;", x_ssl_context_protocol),
  SM("getInstance", "(Ljava/lang/String;Ljava/security/Provider;)Ljavax/net/ssl/SSLContext;", x_ssl_context_get_instance_provider),
  M("createSSLEngine", "()Ljavax/net/ssl/SSLEngine;", x_ssl_context_delegate),
  M("createSSLEngine", "(Ljava/lang/String;I)Ljavax/net/ssl/SSLEngine;", x_ssl_context_delegate),
  M("getServerSocketFactory", "()Ljavax/net/ssl/SSLServerSocketFactory;", x_ssl_context_delegate),
  M("getClientSessionContext", "()Ljavax/net/ssl/SSLSessionContext;", x_ssl_context_delegate),
  M("getServerSessionContext", "()Ljavax/net/ssl/SSLSessionContext;", x_ssl_context_delegate),
  M("getDefaultSSLParameters", "()Ljavax/net/ssl/SSLParameters;", x_ssl_context_delegate),
  M("getSupportedSSLParameters", "()Ljavax/net/ssl/SSLParameters;", x_ssl_context_delegate),
  M_END,
};

/* ---- Collections.reverseOrder(Comparator) ------------------------------------- */
static const struct rt_field rt_reverse_comparator_fields[] = { { "inner", "Ljava/util/Comparator;" }, F_END };
static bool x_reverse_compare(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)nargs;
  union dvm_value inner = { 0 }, swapped[2] = { args[1], args[0] };
  (void)dvm_get_field(vm, self, "inner", "Ljava/util/Comparator;", &inner);
  if (inner.l) return am_vcall(vm, inner.l, "compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", swapped, 2, out);
  return am_vcall(vm, args[1].l, "compareTo", "(Ljava/lang/Object;)I", &args[0], 1, out);
}
static const struct rt_method rt_reverse_comparator[] = {
  M("compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", x_reverse_compare), M_END,
};
static bool x_collections_reverse_order(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                        int nargs, union dvm_value *out)
{
  (void)self;
  struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/ReverseComparator;");
  dvm_ref r = c ? dvm_new_object(vm, c) : 0;
  if (!r) RETL(0);
  (void)dvm_set_field(vm, r, "inner", "Ljava/util/Comparator;", (union dvm_value) {
    .l = nargs ? args[0].l : 0
  });
  RETL(r);
}
static const struct rt_method rt_x_collections_more[] = {
  SM("reverseOrder", "(Ljava/util/Comparator;)Ljava/util/Comparator;", x_collections_reverse_order),
  SM("reverseOrder", "()Ljava/util/Comparator;", x_collections_reverse_order), M_END,
};

/* ---- CyclicBarrier ---------------------------------------------------------------
 * `parties` threads each await(); the last to arrive runs the barrier action and
 * releases the rest into a new generation.  A timeout breaks the barrier. */
static const struct rt_field rt_cyclic_barrier_fields[] = {
  { "parties", "I" }, { "waiting", "I" }, { "generation", "I" }, { "broken", "Z" },
  { "action", "Ljava/lang/Runnable;" }, F_END,
};
static bool x_barrier_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  if (args[0].i <= 0) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "parties");
    return false;
  }
  (void)out;
  (void)dvm_set_field(vm, self, "parties", "I", (union dvm_value) {
    .i = args[0].i
  });
  if (nargs > 1) (void)dvm_set_field(vm, self, "action", "Ljava/lang/Runnable;", args[1]);
  RETV();
}
static int x_barrier_int(struct dvm *vm, dvm_ref self, const char *f)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, f, "I", &v);
  return v.i;
}
static bool x_barrier_await(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  const bool timed = nargs >= 2;
  const int64_t wait_ms = timed ? latch_timeout_ms(vm, args) : 0;
  const uint64_t deadline = timed ? dvm__now_ms() + (uint64_t)(wait_ms > 0 ? wait_ms : 1) : dvm__now_ms() + DVM_BLOCK_MAX_MS;
  if (rt_get_int(vm, self, "broken")) {
    dvm__throw(vm, "java/util/concurrent/BrokenBarrierException", "");
    return false;
  }
  const int parties = x_barrier_int(vm, self, "parties");
  const int arrived = x_barrier_int(vm, self, "waiting") + 1;
  const int generation = x_barrier_int(vm, self, "generation");
  const int index = parties - arrived;
  if (arrived == parties) {
    union dvm_value action = { 0 }, ignored = { 0 };
    (void)dvm_get_field(vm, self, "action", "Ljava/lang/Runnable;", &action);
    if (action.l && !am_vcall(vm, action.l, "run", "()V", NULL, 0, &ignored)) {
      (void)dvm_set_field(vm, self, "broken", "Z", (union dvm_value) {
        .i = 1
      });
      dvm_gil_notify_for(self);
      return false;
    }
    (void)dvm_set_field(vm, self, "waiting", "I", (union dvm_value) {
      .i = 0
    });
    (void)dvm_set_field(vm, self, "generation", "I", (union dvm_value) {
      .i = generation + 1
    });
    dvm_gil_notify_for(self);
    RETI(0);
  }
  (void)dvm_set_field(vm, self, "waiting", "I", (union dvm_value) {
    .i = arrived
  });
  for (;;) {
    if (dvm_is_stopping(vm)) return false;
    if (x_barrier_int(vm, self, "generation") != generation) RETI(index);
    if (rt_get_int(vm, self, "broken")) {
      dvm__throw(vm, "java/util/concurrent/BrokenBarrierException", "");
      return false;
    }
    const uint64_t now = dvm__now_ms();
    if (now >= deadline) {
      (void)dvm_set_field(vm, self, "broken", "Z", (union dvm_value) {
        .i = 1
      });
      dvm_gil_notify_for(self);
      if (timed) {
        dvm__throw(vm, "java/util/concurrent/TimeoutException", "");
        return false;
      }
      dvm__throw(vm, "java/util/concurrent/BrokenBarrierException", "");
      return false;
    }
    dvm_ref thread = dvm__current_thread(vm);
    dvm__thread_set_state(vm, thread, timed ? DVM_THREAD_TIMED_WAITING : DVM_THREAD_WAITING);
    dvm__drain_for_wait(vm);
    dvm_gil_wait_for(vm, (uintptr_t)self, (unsigned)(deadline - now));
    dvm__thread_set_state(vm, thread, DVM_THREAD_RUNNABLE);
  }
}
static bool x_barrier_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const char *name = dvm__builtin_method()->name;
  if (!strcmp(name, "getParties")) RETI(x_barrier_int(vm, self, "parties"));
  if (!strcmp(name, "getNumberWaiting")) RETI(x_barrier_int(vm, self, "waiting"));
  RETI(rt_get_int(vm, self, "broken") != 0);                    /* isBroken */
}
static bool x_barrier_reset(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  (void)out;
  (void)dvm_set_field(vm, self, "waiting", "I", (union dvm_value) {
    .i = 0
  });
  (void)dvm_set_field(vm, self, "broken", "Z", (union dvm_value) {
    .i = 0
  });
  (void)dvm_set_field(vm, self, "generation", "I", (union dvm_value) {
    .i = x_barrier_int(vm, self, "generation") + 1
  });
  dvm_gil_notify_for(self);
  RETV();
}
static const struct rt_method rt_cyclic_barrier[] = {
  M("<init>", "(I)V", x_barrier_init),
  M("<init>", "(ILjava/lang/Runnable;)V", x_barrier_init),
  M("await", "()I", x_barrier_await),
  M("await", "(JLjava/util/concurrent/TimeUnit;)I", x_barrier_await),
  M("getParties", "()I", x_barrier_get), M("getNumberWaiting", "()I", x_barrier_get),
  M("isBroken", "()Z", x_barrier_get), M("reset", "()V", x_barrier_reset), M_END,
};

/* ---- constructors of classes whose state is the base class's --------------------- */
static const struct rt_method rt_x_hash_set_more[] = {
  M("<init>", "(IF)V", list_init), M_END,
};

/* ConcurrentSkipListMap: a sorted map (the interpreter runs one thread at a time
 * inside the VM lock, so TreeMap's behaviour is already atomic per call). */
static const struct rt_method rt_skip_list_map[] = {
  M("<init>", "()V", tree_map_init),
  M("<init>", "(Ljava/util/Comparator;)V", tree_map_init),
  M("<init>", "(Ljava/util/Map;)V", tree_map_init_map),
  M("<init>", "(Ljava/util/SortedMap;)V", tree_map_init_map),
  M_END,
};

/* ---- Platform methods that were answered by the "not implemented" stub ------
 * Each is the real behaviour for this device, not a placeholder. */

/* ---- Matrix.setRectToRect (Skia's SkMatrix::setRectToRect) -------------- */
static bool x_rect_floats(struct dvm *vm, dvm_ref r, float v[4])
{
  static const char *const edges[4] = { "left", "top", "right", "bottom" };
  if (!r) {
    dvm__throw(vm, "java/lang/NullPointerException", "rect");
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    union dvm_value f = { 0 };
    (void)dvm_get_field(vm, r, edges[i], "F", &f);
    v[i] = f.f;
  }
  return true;
}

static bool x_matrix_set_rect_to_rect(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                      int nargs, union dvm_value *out)
{
  (void)nargs;
  float src[4], dst[4];
  if (!x_rect_floats(vm, ARG(0).l, src) || !x_rect_floats(vm, ARG(1).l, dst)) return false;
  float *m = matrix_values(vm, self);
  if (!m) return false;
  const float sw = src[2] - src[0], sh = src[3] - src[1], dw = dst[2] - dst[0], dh = dst[3] - dst[1];
  if (!(sw > 0) || !(sh > 0)) {          /* an empty source leaves the identity */
    mat_identity(m);
    RETI(0);
  }
  if (!(dw > 0) || !(dh > 0)) {          /* an empty target collapses everything */
    memset(m, 0, 9 * sizeof *m);
    m[8] = 1.0f;
    RETI(1);
  }
  union dvm_value name = { 0 };
  const char *fit = ARG(2).l && dvm_get_field(vm, ARG(2).l, "name", "Ljava/lang/String;", &name) && name.l
                    ? dvm_string_utf8(vm, name.l) : "FILL";
  float sx = dw / sw, sy = dh / sh;
  bool x_larger = false;
  if (strcmp(fit, "FILL")) {
    if (sx > sy) {
      x_larger = true;
      sx = sy;
    } else sy = sx;
  }
  float tx = dst[0] - src[0] * sx, ty = dst[1] - src[1] * sy;
  if (!strcmp(fit, "CENTER") || !strcmp(fit, "END")) {
    float diff = x_larger ? dw - sw * sx : dh - sh * sy;
    if (!strcmp(fit, "CENTER")) diff /= 2;
    if (x_larger) tx += diff;
    else ty += diff;
  }
  const float values[9] = { sx, 0, tx, 0, sy, ty, 0, 0, 1 };
  memcpy(m, values, sizeof values);
  ui_invalidate();
  RETI(1);
}
static const struct rt_method rt_x_matrix_rect[] = {
  M("setRectToRect", "(Landroid/graphics/RectF;Landroid/graphics/RectF;Landroid/graphics/Matrix$ScaleToFit;)Z",
    x_matrix_set_rect_to_rect),
  M_END,
};

/* ---- ColorStateList.isStateful ------------------------------------------ */
static bool x_csl_is_stateful(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value specs = { 0 };
  (void)dvm_get_field(vm, self, "stateSpecs", "[[I", &specs);
  RETI(specs.l && dvm_array_length(vm, specs.l) > 1);
}
static const struct rt_method rt_x_color_state_more[] = {
  M("isStateful", "()Z", x_csl_is_stateful),
  M_END,
};

/* ---- Paint: letter spacing, shadow layer, linear text ------------------- */
static bool x_paint_letter_spacing(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "getLetterSpacing")) {
    union dvm_value v = { 0 };
    (void)dvm_get_field(vm, self, "letterSpacing", "F", &v);
    RETF(v.f);
  }
  (void)out;
  (void)dvm_set_field(vm, self, "letterSpacing", "F", ARG(0));
  RETV();
}
static bool x_paint_shadow(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  (void)dvm_set_field(vm, self, "shadowRadius", "F", ARG(0));
  (void)dvm_set_field(vm, self, "shadowDx", "F", ARG(1));
  (void)dvm_set_field(vm, self, "shadowDy", "F", ARG(2));
  (void)dvm_set_field(vm, self, "shadowColor", "I", ARG(3));
  RETV();
}
static bool x_paint_linear_text(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  union dvm_value flags = { 0 };
  (void)dvm_get_field(vm, self, "flags", "I", &flags);
  flags.i = ARG(0).i ? flags.i | 0x40 : flags.i & ~0x40;
  (void)dvm_set_field(vm, self, "flags", "I", flags);
  RETV();
}
static const struct rt_method rt_x_paint_more[] = {
  M("setLetterSpacing", "(F)V", x_paint_letter_spacing),
  M("getLetterSpacing", "()F", x_paint_letter_spacing),
  M("setShadowLayer", "(FFFI)V", x_paint_shadow),
  M("setLinearText", "(Z)V", x_paint_linear_text),
  M_END,
};

/* ---- TextView: compound drawable padding, auto-size configuration ------- */
static bool x_text_drawable_padding(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "getCompoundDrawablePadding")) {
    union dvm_value v = { 0 };
    (void)dvm_get_field(vm, self, "drawablePadding", "I", &v);
    RETI(v.i);
  }
  (void)out;
  (void)dvm_set_field(vm, self, "drawablePadding", "I", ARG(0));
  ui_layout_changed();
  ui_invalidate();
  RETV();
}
static bool x_text_autosize_step(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "autoSizeStep", "I", &v);
  RETI(v.i ? v.i : -1);
}
static bool x_text_autosize_config(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                   int nargs, union dvm_value *out)
{
  (void)nargs; (void)out;
  (void)dvm_set_field(vm, self, "autoSizeMin", "I", ARG(0));
  (void)dvm_set_field(vm, self, "autoSizeMax", "I", ARG(1));
  (void)dvm_set_field(vm, self, "autoSizeStep", "I", ARG(2));
  ui_layout_changed();
  ui_invalidate();
  RETV();
}
static const struct rt_method rt_x_text_view_more[] = {
  M("setCompoundDrawablePadding", "(I)V", x_text_drawable_padding),
  M("getCompoundDrawablePadding", "()I", x_text_drawable_padding),
  M("getAutoSizeStepGranularity", "()I", x_text_autosize_step),
  M("setAutoSizeTextTypeUniformWithConfiguration", "(IIII)V", x_text_autosize_config),
  M_END,
};

/* ---- Intent, WifiManager, ActivityManager, Debug, Activity -------------- */
static bool x_intent_data_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value data = { 0 };
  if (!am_vcall(vm, self, "getData", "()Landroid/net/Uri;", NULL, 0, &data)) return false;
  if (!data.l) RETL(0);
  return am_vcall(vm, data.l, "toString", "()Ljava/lang/String;", NULL, 0, out);
}
static const struct rt_method rt_x_intent_more[] = {
  M("getDataString", "()Ljava/lang/String;", x_intent_data_string),
  M("getClipData", "()Landroid/content/ClipData;", ret_null),   /* none is ever attached */
  M("getSelector", "()Landroid/content/Intent;", ret_null),
  M_END,
};
static const struct rt_method rt_x_wifi_more[] = {
  M("isWifiEnabled", "()Z", ret_true),
  M_END,
};
static bool x_current_user(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETI(0);   /* the owner: this device has one user */
}
static const struct rt_method rt_x_activity_manager_user[] = {
  SM("getCurrentUser", "()I", x_current_user),
  M_END,
};
static bool x_thread_cpu_ns(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  RETJ((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);
}
static const struct rt_method rt_x_debug_cpu[] = {
  SM("threadCpuTimeNanos", "()J", x_thread_cpu_ns),
  M_END,
};
static const struct rt_method rt_x_activity_child[] = {
  M("isChild", "()Z", ret_false),   /* activities here are never embedded in an ActivityGroup */
  M_END,
};
static const struct rt_method rt_x_vm_policy_builder[] = {
  M("<init>", "(Landroid/os/StrictMode$VmPolicy;)V", empty_void),
  M_END,
};
