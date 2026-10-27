/* android.app.Notification and Notification.Builder.
 *
 * A Notification is plain data (title, text, flags, group, extras); posting it
 * is NotificationManager's business.  The Builder collects settings and
 * build() returns a Notification carrying them, with the title/text/etc. in
 * `extras` under the same keys AOSP uses, since apps read them back from there.
 * Included from dvm_runtime.c after the Bundle and NotificationManager code. */

static const struct { const char *name, *value; } notification_extras_keys[] = {
   { "EXTRA_TITLE", "android.title" },
   { "EXTRA_TEXT", "android.text" },
   { "EXTRA_BIG_TEXT", "android.bigText" },
   { "EXTRA_SUB_TEXT", "android.subText" },
   { "EXTRA_SUMMARY_TEXT", "android.summaryText" },
   { "EXTRA_TITLE_BIG", "android.title.big" },
   { "EXTRA_SHOW_WHEN", "android.showWhen" },
   { "EXTRA_SHOW_CHRONOMETER", "android.showChronometer" },
   { "EXTRA_PROGRESS", "android.progress" },
   { "EXTRA_PROGRESS_MAX", "android.progressMax" },
   { "EXTRA_PROGRESS_INDETERMINATE", "android.progressIndeterminate" },
   { "EXTRA_SMALL_ICON", "android.icon" },
   { "EXTRA_LARGE_ICON", "android.largeIcon" },
   { "CATEGORY_MESSAGE", "msg" },
   { "CATEGORY_REMINDER", "reminder" },
   { "CATEGORY_EVENT", "event" },
   { "CATEGORY_ALARM", "alarm" },
   { "CATEGORY_PROMO", "promo" },
   { "CATEGORY_SERVICE", "service" },
   { "CATEGORY_STATUS", "status" },
   { NULL, NULL }
};
static const struct rt_field rt_notification_sfields[] = {
   { "EXTRA_TITLE", "Ljava/lang/String;" }, { "EXTRA_TEXT", "Ljava/lang/String;" },
   { "EXTRA_BIG_TEXT", "Ljava/lang/String;" }, { "EXTRA_SUB_TEXT", "Ljava/lang/String;" },
   { "EXTRA_SUMMARY_TEXT", "Ljava/lang/String;" }, { "EXTRA_TITLE_BIG", "Ljava/lang/String;" },
   { "EXTRA_SHOW_WHEN", "Ljava/lang/String;" }, { "EXTRA_SHOW_CHRONOMETER", "Ljava/lang/String;" },
   { "EXTRA_PROGRESS", "Ljava/lang/String;" }, { "EXTRA_PROGRESS_MAX", "Ljava/lang/String;" },
   { "EXTRA_PROGRESS_INDETERMINATE", "Ljava/lang/String;" }, { "EXTRA_SMALL_ICON", "Ljava/lang/String;" },
   { "EXTRA_LARGE_ICON", "Ljava/lang/String;" },
   { "CATEGORY_MESSAGE", "Ljava/lang/String;" }, { "CATEGORY_REMINDER", "Ljava/lang/String;" },
   { "CATEGORY_EVENT", "Ljava/lang/String;" }, { "CATEGORY_ALARM", "Ljava/lang/String;" },
   { "CATEGORY_PROMO", "Ljava/lang/String;" }, { "CATEGORY_SERVICE", "Ljava/lang/String;" },
   { "CATEGORY_STATUS", "Ljava/lang/String;" }, F_END
};

static const struct rt_field rt_notification_fields[] = {
   { "when", "J" }, { "number", "I" }, { "flags", "I" }, { "defaults", "I" },
   { "priority", "I" }, { "extras", "Landroid/os/Bundle;" },
   { "group", "Ljava/lang/String;" }, { "sortKey", "Ljava/lang/String;" },
   { "channelId", "Ljava/lang/String;" }, { "category", "Ljava/lang/String;" },
   { "tickerText", "Ljava/lang/CharSequence;" }, { "contentIntent", "Landroid/app/PendingIntent;" },
   { "deleteIntent", "Landroid/app/PendingIntent;" }, F_END
};

/* The Builder keeps exactly the fields a Notification has, so build() copies
 * them across by name. */
#define NOTIFICATION_COPY_FIELDS(X) \
   X("when", "J") X("number", "I") X("flags", "I") X("defaults", "I") X("priority", "I") \
   X("group", "Ljava/lang/String;") X("sortKey", "Ljava/lang/String;") \
   X("channelId", "Ljava/lang/String;") X("category", "Ljava/lang/String;") \
   X("tickerText", "Ljava/lang/CharSequence;") X("contentIntent", "Landroid/app/PendingIntent;") \
   X("deleteIntent", "Landroid/app/PendingIntent;")

static dvm_ref notification_extras(struct dvm *vm, dvm_ref obj)
{
   union dvm_value ex = { 0 }, ignored = { 0 };
   (void)dvm_get_field(vm, obj, "extras", "Landroid/os/Bundle;", &ex);
   if (ex.l) return ex.l;
   struct dvm_class *bc = dvm__class_by_desc(vm, "Landroid/os/Bundle;");
   ex.l = bc ? dvm_new_object(vm, bc) : 0;
   if (!ex.l) return 0;
   (void)map_init(vm, ex.l, NULL, 0, &ignored);
   (void)dvm_set_field(vm, obj, "extras", "Landroid/os/Bundle;", ex);
   return ex.l;
}

static bool notification_put_extra(struct dvm *vm, dvm_ref obj, const char *key,
                                   const char *name, const char *sig, union dvm_value v)
{
   dvm_ref ex = notification_extras(vm, obj);
   if (!ex) return false;
   union dvm_value a[2] = { { .l = dvm_new_string(vm, key) }, v }, ignored = { 0 };
   return am_vcall(vm, ex, name, sig, a, 2, &ignored);
}

static bool notification_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   (void)notification_extras(vm, self);
   RETV();
}

static bool notification_get_extras(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(notification_extras(vm, self));
}

#define NOTIFICATION_GETTER(fn, field, desc, ret) \
static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
               int nargs, union dvm_value *out) \
{ \
   union dvm_value v = { 0 }; \
   (void)args; (void)nargs; \
   (void)dvm_get_field(vm, self, field, desc, &v); \
   ret(v.l); \
}
NOTIFICATION_GETTER(notification_get_group, "group", "Ljava/lang/String;", RETL)
NOTIFICATION_GETTER(notification_get_sort_key, "sortKey", "Ljava/lang/String;", RETL)
NOTIFICATION_GETTER(notification_get_channel_id, "channelId", "Ljava/lang/String;", RETL)
NOTIFICATION_GETTER(notification_get_category, "category", "Ljava/lang/String;", RETL)

static bool notification_get_when(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   union dvm_value v = { 0 };
   (void)args; (void)nargs;
   (void)dvm_get_field(vm, self, "when", "J", &v);
   RETJ(v.j);
}

static const struct rt_method rt_notification[] = {
   M("<init>", "()V", notification_init),
   M("getGroup", "()Ljava/lang/String;", notification_get_group),
   M("getSortKey", "()Ljava/lang/String;", notification_get_sort_key),
   M("getChannelId", "()Ljava/lang/String;", notification_get_channel_id),
   M("getExtras", "()Landroid/os/Bundle;", notification_get_extras),
   M("getWhen", "()J", notification_get_when),
   M_END,
};

/* ---------------------------------------------------------------- Builder */

static bool nb_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)out;
   (void)notification_extras(vm, self);
   /* Builder(Context, String channelId) */
   if (nargs > 1 && args[1].l)
      (void)dvm_set_field(vm, self, "channelId", "Ljava/lang/String;", args[1]);
   union dvm_value now = { .j = (int64_t)time(NULL) * 1000 };
   (void)dvm_set_field(vm, self, "when", "J", now);
   RETV();
}

/* One setter per (method, field): store the argument and return the builder so
 * calls chain. */
#define NB_SET(fn, field, desc) \
static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
               int nargs, union dvm_value *out) \
{ \
   (void)dvm_set_field(vm, self, field, desc, ARG(0)); \
   RETL(self); \
}
#define NB_EXTRA(fn, key, call, sig) \
static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
               int nargs, union dvm_value *out) \
{ \
   if (!notification_put_extra(vm, self, key, call, sig, ARG(0))) return false; \
   RETL(self); \
}
NB_SET(nb_set_when, "when", "J")
NB_SET(nb_set_number, "number", "I")
NB_SET(nb_set_group, "group", "Ljava/lang/String;")
NB_SET(nb_set_sort_key, "sortKey", "Ljava/lang/String;")
NB_SET(nb_set_channel, "channelId", "Ljava/lang/String;")
NB_SET(nb_set_category, "category", "Ljava/lang/String;")
NB_SET(nb_set_ticker, "tickerText", "Ljava/lang/CharSequence;")
NB_SET(nb_set_content_intent, "contentIntent", "Landroid/app/PendingIntent;")
NB_SET(nb_set_delete_intent, "deleteIntent", "Landroid/app/PendingIntent;")
NB_SET(nb_set_defaults, "defaults", "I")
NB_SET(nb_set_priority, "priority", "I")
NB_EXTRA(nb_set_title, "android.title", "putCharSequence", "(Ljava/lang/String;Ljava/lang/CharSequence;)V")
NB_EXTRA(nb_set_text, "android.text", "putCharSequence", "(Ljava/lang/String;Ljava/lang/CharSequence;)V")
NB_EXTRA(nb_set_sub_text, "android.subText", "putCharSequence", "(Ljava/lang/String;Ljava/lang/CharSequence;)V")
NB_EXTRA(nb_set_show_when, "android.showWhen", "putBoolean", "(Ljava/lang/String;Z)V")

/* Flags are single bits of Notification.flags. */
static bool nb_flag(struct dvm *vm, dvm_ref self, int32_t bit, bool on)
{
   union dvm_value f = { 0 };
   (void)dvm_get_field(vm, self, "flags", "I", &f);
   f.i = on ? (f.i | bit) : (f.i & ~bit);
   return dvm_set_field(vm, self, "flags", "I", f);
}
#define NB_FLAG(fn, bit) \
static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, \
               int nargs, union dvm_value *out) \
{ \
   (void)nb_flag(vm, self, bit, ARG(0).i != 0); \
   RETL(self); \
}
NB_FLAG(nb_set_auto_cancel, 0x10)
NB_FLAG(nb_set_ongoing, 0x2)
NB_FLAG(nb_set_only_alert_once, 0x8)
NB_FLAG(nb_set_group_summary, 0x200)

/* Settings with no visible effect on this device are accepted and chained. */
static bool nb_ignore(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)vm; (void)args; (void)nargs;
   RETL(self);
}

static bool nb_set_style(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   /* BigTextStyle.bigText() supplies the expanded text. */
   union dvm_value text = { 0 };
   if (ARG(0).l) {
      (void)dvm_get_field(vm, ARG(0).l, "bigText", "Ljava/lang/CharSequence;", &text);
      if (text.l && !notification_put_extra(vm, self, "android.bigText", "putCharSequence",
                         "(Ljava/lang/String;Ljava/lang/CharSequence;)V", text))
         return false;
   }
   RETL(self);
}

static bool nb_get_extras(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETL(notification_extras(vm, self));
}

static bool nb_set_extras(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   if (ARG(0).l) {
      union dvm_value ignored = { 0 };
      dvm_ref mine = notification_extras(vm, self);
      if (mine && !am_vcall(vm, mine, "putAll", "(Landroid/os/Bundle;)V", args, 1, &ignored))
         return false;
   }
   RETL(self);
}

static bool nb_build(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *nc = dvm__class_by_desc(vm, "Landroid/app/Notification;");
   dvm_ref n = nc ? dvm_new_object(vm, nc) : 0;
   if (!n) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Notification"); return false; }
   union dvm_value v = { 0 };
#define X(name, desc) \
   (void)dvm_get_field(vm, self, name, desc, &v); (void)dvm_set_field(vm, n, name, desc, v);
   NOTIFICATION_COPY_FIELDS(X)
#undef X
   /* The notification owns a copy of the extras, not the builder's Bundle. */
   union dvm_value ignored = { 0 };
   dvm_ref extras = notification_extras(vm, n);
   dvm_ref mine = notification_extras(vm, self);
   if (extras && mine &&
       !am_vcall(vm, extras, "putAll", "(Landroid/os/Bundle;)V",
                 &(union dvm_value){ .l = mine }, 1, &ignored))
      return false;
   RETL(n);
}

static const struct rt_method rt_notification_builder[] = {
   M("<init>", "(Landroid/content/Context;)V", nb_init),
   M("<init>", "(Landroid/content/Context;Ljava/lang/String;)V", nb_init),
   M("setContentTitle", "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;", nb_set_title),
   M("setContentText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;", nb_set_text),
   M("setSubText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;", nb_set_sub_text),
   M("setTicker", "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;", nb_set_ticker),
   M("setWhen", "(J)Landroid/app/Notification$Builder;", nb_set_when),
   M("setShowWhen", "(Z)Landroid/app/Notification$Builder;", nb_set_show_when),
   M("setNumber", "(I)Landroid/app/Notification$Builder;", nb_set_number),
   M("setAutoCancel", "(Z)Landroid/app/Notification$Builder;", nb_set_auto_cancel),
   M("setOngoing", "(Z)Landroid/app/Notification$Builder;", nb_set_ongoing),
   M("setOnlyAlertOnce", "(Z)Landroid/app/Notification$Builder;", nb_set_only_alert_once),
   M("setGroup", "(Ljava/lang/String;)Landroid/app/Notification$Builder;", nb_set_group),
   M("setGroupSummary", "(Z)Landroid/app/Notification$Builder;", nb_set_group_summary),
   M("setSortKey", "(Ljava/lang/String;)Landroid/app/Notification$Builder;", nb_set_sort_key),
   M("setChannelId", "(Ljava/lang/String;)Landroid/app/Notification$Builder;", nb_set_channel),
   M("setCategory", "(Ljava/lang/String;)Landroid/app/Notification$Builder;", nb_set_category),
   M("setDefaults", "(I)Landroid/app/Notification$Builder;", nb_set_defaults),
   M("setPriority", "(I)Landroid/app/Notification$Builder;", nb_set_priority),
   M("setContentIntent", "(Landroid/app/PendingIntent;)Landroid/app/Notification$Builder;", nb_set_content_intent),
   M("setDeleteIntent", "(Landroid/app/PendingIntent;)Landroid/app/Notification$Builder;", nb_set_delete_intent),
   M("setStyle", "(Landroid/app/Notification$Style;)Landroid/app/Notification$Builder;", nb_set_style),
   M("setExtras", "(Landroid/os/Bundle;)Landroid/app/Notification$Builder;", nb_set_extras),
   M("addExtras", "(Landroid/os/Bundle;)Landroid/app/Notification$Builder;", nb_set_extras),
   M("getExtras", "()Landroid/os/Bundle;", nb_get_extras),
   /* Presentation-only settings: nothing on this device draws a status bar. */
   M("setSmallIcon", "(I)Landroid/app/Notification$Builder;", nb_ignore),
   M("setLargeIcon", "(Landroid/graphics/Bitmap;)Landroid/app/Notification$Builder;", nb_ignore),
   M("setColor", "(I)Landroid/app/Notification$Builder;", nb_ignore),
   M("setVisibility", "(I)Landroid/app/Notification$Builder;", nb_ignore),
   M("setSound", "(Landroid/net/Uri;)Landroid/app/Notification$Builder;", nb_ignore),
   M("setVibrate", "([J)Landroid/app/Notification$Builder;", nb_ignore),
   M("setLights", "(III)Landroid/app/Notification$Builder;", nb_ignore),
   M("setTimeoutAfter", "(J)Landroid/app/Notification$Builder;", nb_ignore),
   M("setProgress", "(IIZ)Landroid/app/Notification$Builder;", nb_ignore),
   M("build", "()Landroid/app/Notification;", nb_build),
   M_END,
};

/* BigTextStyle: only the expanded text is read back, by setStyle(). */
static bool bts_big_text(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)dvm_set_field(vm, self, "bigText", "Ljava/lang/CharSequence;", ARG(0));
   RETL(self);
}
static const struct rt_field rt_big_text_style_fields[] = {
   { "bigText", "Ljava/lang/CharSequence;" }, F_END
};
static const struct rt_method rt_big_text_style[] = {
   M("<init>", "()V", empty_void),
   M("bigText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;", bts_big_text),
   M("setBigContentTitle", "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;", nb_ignore),
   M("setSummaryText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;", nb_ignore),
   M_END,
};
