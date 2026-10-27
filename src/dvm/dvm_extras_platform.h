/* Small platform APIs apps and SDKs call that had no implementation: the
 * dalvik.system.CloseGuard leak detector, java.net.URI.toURL(),
 * Configuration.setToDefaults(), FileObserver(String), and the Activity
 * entry points with nothing to do on this device. */

/* CloseGuard: get() makes a guard, open(what) arms it with the caller's
 * description, close() disarms it, warnIfOpen() reports a guard still armed. */
static const struct rt_field rt_close_guard_fields[] = {
   { "opener", "Ljava/lang/String;" }, F_END
};

static bool close_guard_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Ldalvik/system/CloseGuard;");
   RETL(c ? dvm_new_object(vm, c) : 0);
}

static bool close_guard_open(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)out;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "closer == null");
      return false;
   }
   (void)dvm_set_field(vm, self, "opener", "Ljava/lang/String;", ARG(0));
   RETV();
}

static bool close_guard_close(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "opener", "Ljava/lang/String;", (union dvm_value){ .l = 0 });
   RETV();
}

static bool close_guard_warn(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   union dvm_value opener = { 0 };
   (void)args; (void)nargs; (void)out;
   (void)dvm_get_field(vm, self, "opener", "Ljava/lang/String;", &opener);
   if (opener.l)
      fprintf(stderr, "[dvm] CloseGuard: A resource was acquired at attached stack trace "
                      "but never released (%s)\n", dvm_string_utf8(vm, opener.l));
   RETV();
}

static const struct rt_method rt_close_guard[] = {
   SM("get", "()Ldalvik/system/CloseGuard;", close_guard_get),
   M("open", "(Ljava/lang/String;)V", close_guard_open),
   M("close", "()V", close_guard_close),
   M("warnIfOpen", "()V", close_guard_warn),
   M_END,
};

/* URI.toURL(): only an absolute URI names a location a URL can open. */
static bool juri_to_url(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *text = uri_text(vm, self);
   const char *colon = text ? strchr(text, ':') : NULL;
   if (!colon || colon == text) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "URI is not absolute");
      return false;
   }
   struct dvm_class *uc = dvm__class_by_desc(vm, "Ljava/net/URL;");
   dvm_ref url = uc ? dvm_new_object(vm, uc) : 0;
   union dvm_value spec = { .l = dvm_new_string(vm, text) }, ignored = { 0 };
   if (!url || !am_vcall(vm, url, "<init>", "(Ljava/lang/String;)V", &spec, 1, &ignored))
      return false;
   RETL(url);
}

static const struct rt_method rt_x_java_uri[] = {
   M("toURL", "()Ljava/net/URL;", juri_to_url),
   M_END,
};

/* Configuration.setToDefaults(): font scale 1, no locale override, nothing
 * else set. */
static bool cfg_set_to_defaults(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "fontScale", "F", (union dvm_value){ .f = 1.0f });
   (void)dvm_set_field(vm, self, "screenLayout", "I", (union dvm_value){ .i = 0 });
   configuration_ensure_locale(vm, self);
   RETV();
}

static const struct rt_method rt_x_configuration[] = {
   M("setToDefaults", "()V", cfg_set_to_defaults),
   M_END,
};

/* FileObserver(String path) watches for every event. */
static const struct rt_method rt_x_file_observer[] = {
   M("<init>", "(Ljava/lang/String;)V", file_observer_init),
   M_END,
};

/* Activity: no touch-outside dismissal on a full-screen window; Unity reports
 * first-frame completion for a system that has no startup trace; a plain launch
 * has no calling package; onContentChanged() is empty in the base class. */
static const struct rt_method rt_x_activity[] = {
   M("setFinishOnTouchOutside", "(Z)V", empty_void),
   M("reportFullyDrawn", "()V", empty_void),
   M("getCallingPackage", "()Ljava/lang/String;", ret_null),
   M("onContentChanged", "()V", empty_void),
   M_END,
};
