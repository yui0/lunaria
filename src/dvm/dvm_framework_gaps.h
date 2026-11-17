#ifndef _WIN32
#include <sys/statvfs.h>
#endif
/* Framework classes and methods apps reach that had no implementation: each of
 * these used to answer null/0 through the "method not implemented" fallback, and
 * every one of them was found by an app that then failed on the null.  Included
 * by dvm_runtime.c after the class helpers; the tables are listed in rt_extras
 * (existing classes) and rt_classes (new ones). */

/* ---- android.text.format.DateUtils --------------------------------------- */
#define DU_SHOW_TIME     0x00001
#define DU_SHOW_WEEKDAY  0x00002
#define DU_SHOW_YEAR     0x00004
#define DU_NO_YEAR       0x00008
#define DU_SHOW_DATE     0x00010
#define DU_NO_MONTH_DAY  0x00020
#define DU_12HOUR        0x00040
#define DU_24HOUR        0x00080
#define DU_ABBREV_WEEKDAY 0x08000
#define DU_ABBREV_MONTH  0x10000
#define DU_ABBREV_ALL    0x80000
#define DU_NUMERIC_DATE  0x20000

/* Removes the year field and the literal text hugging it from a date pattern
 * ("yyyy年M月d日" -> "M月d日", "MMM d, yyyy" -> "MMM d"). */
static void du_strip_year(char *p)
{
  char out[160];
  size_t n = 0;
  bool quote = false;
  for (size_t i = 0; p[i] && n + 1 < sizeof out;) {
    if (p[i] == '\'') {
      quote = !quote;
      out[n++] = p[i++];
    } else if (!quote && (p[i] == 'y' || p[i] == 'Y')) {
      while (p[i] == 'y' || p[i] == 'Y') ++i;
      /* what followed the year (年, '/', ',', ' ') belongs to it */
      while (p[i] && !isalpha((unsigned char)p[i]) && p[i] != '\'' && (unsigned char)p[i] < 0x80) ++i;
      if ((unsigned char)p[i] == 0xe5 && (unsigned char)p[i + 1] == 0xb9 && (unsigned char)p[i + 2] == 0xb4) i += 3;
      /* and what led up to it, when the year was last */
      if (!p[i]) while (n && !isalnum((unsigned char)out[n - 1]) && (unsigned char)out[n - 1] < 0x80) --n;
    } else out[n++] = p[i++];
  }
  out[n] = 0;
  strcpy(p, out);
}

static bool du_pattern_of(struct dvm *vm, dvm_ref locale, int date_style, int time_style, char *out, size_t cap)
{
  union dvm_value fmt = { 0 }, pat = { 0 };
  if (!date_time_instance_impl(vm, date_style, time_style, locale, &fmt) ||
      !am_vcall(vm, fmt.l, "toPattern", "()Ljava/lang/String;", NULL, 0, &pat)) return false;
  snprintf(out, cap, "%s", pat.l ? dvm_string_utf8(vm, pat.l) : "");
  return true;
}

static bool x_dateutils_format(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const int64_t millis = ARG(1).j;
  int flags = ARG(2).i;
  union dvm_value locale = { 0 };
  if (!android_context_locale(vm, ARG(0).l, &locale)) return false;
  char pattern[200] = "", part[160];
  const bool abbrev_month = flags & (DU_ABBREV_MONTH | DU_ABBREV_ALL);
  if ((flags & (DU_SHOW_DATE | DU_SHOW_WEEKDAY | DU_SHOW_YEAR)) || !(flags & DU_SHOW_TIME)) {
    /* The year is left out of a date in the current year, unless asked for. */
    time_t when = (time_t)(millis / 1000), now = time(NULL);
    struct tm tw, tn;
    localtime_r(&when, &tw);
    localtime_r(&now, &tn);
    const bool show_year = (flags & DU_SHOW_YEAR) || (!(flags & DU_NO_YEAR) && tw.tm_year != tn.tm_year);
    const int style = (flags & DU_NUMERIC_DATE) ? 3 : abbrev_month ? 2 : 1;
    if (!du_pattern_of(vm, locale.l, style, -1, part, sizeof part)) return false;
    if (!show_year) du_strip_year(part);
    snprintf(pattern, sizeof pattern, "%s", part);
    if (flags & DU_SHOW_WEEKDAY) {
      const size_t n = strlen(pattern);
      snprintf(pattern + n, sizeof pattern - n, " %s", (flags & (DU_ABBREV_WEEKDAY | DU_ABBREV_ALL)) ? "EEE" : "EEEE");
    }
  }
  if (flags & DU_SHOW_TIME) {
    if (!du_pattern_of(vm, locale.l, -1, 3, part, sizeof part)) return false;
    /* FORMAT_24HOUR / FORMAT_12HOUR override the locale's clock. */
    if (flags & DU_24HOUR) snprintf(part, sizeof part, "HH:mm");
    else if (flags & DU_12HOUR) snprintf(part, sizeof part, "h:mm a");
    const size_t n = strlen(pattern);
    snprintf(pattern + n, sizeof pattern - n, "%s%s", n ? " " : "", part);
  }
  return android_format_millis(vm, pattern, millis, out);
}

static bool x_dateutils_elapsed(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)self;
  (void)nargs;
  int64_t s = ARG(0).j;
  char buf[48];
  const int64_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
  if (h > 0) snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)sec);
  else snprintf(buf, sizeof buf, "%02lld:%02lld", (long long)m, (long long)sec);
  RETL(dvm_new_string(vm, buf));
}

static bool x_dateutils_is_today(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  time_t a = (time_t)(ARG(0).j / 1000), b = ts.tv_sec;
  struct tm ta, tb;
  localtime_r(&a, &ta);
  localtime_r(&b, &tb);
  RETI(ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday);
}

static const struct rt_method rt_dateutils[] = {
  SM("formatDateTime", "(Landroid/content/Context;JI)Ljava/lang/String;", x_dateutils_format),
  SM("formatElapsedTime", "(J)Ljava/lang/String;", x_dateutils_elapsed),
  SM("isToday", "(J)Z", x_dateutils_is_today),
  M_END,
};

/* ---- android.graphics.drawable.Icon ---------------------------------------- */
enum { ICON_BITMAP = 1, ICON_RESOURCE = 2, ICON_DATA = 3, ICON_URI = 4, ICON_ADAPTIVE = 5 };
static const struct rt_field rt_icon_fields[] = {
  { "type", "I" }, { "resId", "I" }, { "resPackage", "Ljava/lang/String;" }, { "bitmap", "Landroid/graphics/Bitmap;" },
  { "data", "[B" }, { "uri", "Landroid/net/Uri;" }, { "tintList", "Landroid/content/res/ColorStateList;" },
  F_END,
};
static dvm_ref icon_new(struct dvm *vm, int type)
{
  struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/graphics/drawable/Icon;");
  dvm_ref icon = c ? dvm_new_object(vm, c) : 0;
  if (icon) (void)dvm_set_field(vm, icon, "type", "I", (union dvm_value) {
    .i = type
  });
  return icon;
}
static bool x_icon_create(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *name = builtin_name(), *sig = builtin_sig();
  dvm_ref icon;
  if (!strcmp(name, "createWithResource")) {
    icon = icon_new(vm, ICON_RESOURCE);
    if (!icon) return false;
    union dvm_value pkg = { 0 };
    if (!strncmp(sig, "(Landroid/content/Context;", 26)) {
      if (!ARG(0).l) {
        dvm__throw(vm, "java/lang/IllegalArgumentException", "Context must not be null.");
        return false;
      }
      if (!am_vcall(vm, ARG(0).l, "getPackageName", "()Ljava/lang/String;", NULL, 0, &pkg)) return false;
    } else pkg = ARG(0);
    (void)dvm_set_field(vm, icon, "resPackage", "Ljava/lang/String;", pkg);
    (void)dvm_set_field(vm, icon, "resId", "I", ARG(1));
    RETL(icon);
  }
  if (!strcmp(name, "createWithBitmap") || !strcmp(name, "createWithAdaptiveBitmap")) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Bitmap must not be null.");
      return false;
    }
    icon = icon_new(vm, !strcmp(name, "createWithBitmap") ? ICON_BITMAP : ICON_ADAPTIVE);
    if (!icon) return false;
    (void)dvm_set_field(vm, icon, "bitmap", "Landroid/graphics/Bitmap;", ARG(0));
    RETL(icon);
  }
  if (!strcmp(name, "createWithData")) {
    icon = icon_new(vm, ICON_DATA);
    if (!icon) return false;
    (void)dvm_set_field(vm, icon, "data", "[B", ARG(0));
    RETL(icon);
  }
  icon = icon_new(vm, ICON_URI);   /* createWithContentUri */
  if (!icon) return false;
  if (!strstr(sig, "Landroid/net/Uri;")) {
    union dvm_value uri = { 0 };
    union dvm_value a = ARG(0);
    struct dvm_class *uc = dvm_find_class(vm, "android/net/Uri");
    struct dvm_method *parse = uc ? dvm_find_method(vm, uc, "parse", "(Ljava/lang/String;)Landroid/net/Uri;") : NULL;
    if (!parse || !dvm_call(vm, parse, 0, &a, 1, &uri)) return false;
    (void)dvm_set_field(vm, icon, "uri", "Landroid/net/Uri;", uri);
  } else (void)dvm_set_field(vm, icon, "uri", "Landroid/net/Uri;", ARG(0));
  RETL(icon);
}
static bool x_icon_get(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "getType")) return dvm_get_field(vm, self, "type", "I", out);
  if (!strcmp(n, "getResId")) return dvm_get_field(vm, self, "resId", "I", out);
  if (!strcmp(n, "getResPackage")) return dvm_get_field(vm, self, "resPackage", "Ljava/lang/String;", out);
  if (!strcmp(n, "getBitmap")) return dvm_get_field(vm, self, "bitmap", "Landroid/graphics/Bitmap;", out);
  return dvm_get_field(vm, self, "uri", "Landroid/net/Uri;", out);   /* getUri */
}
static bool x_icon_tint(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
  (void)nargs;
  union dvm_value list = ARG(0);
  if (!strcmp(builtin_name(), "setTint")) {
    list.l = color_state_list_new(vm, ARG(0).i);
    if (!list.l) return false;
  }
  (void)dvm_set_field(vm, self, "tintList", "Landroid/content/res/ColorStateList;", list);
  RETL(self);
}
/* loadDrawable(Context): a resource icon is that resource, a bitmap icon a
 * BitmapDrawable over the bitmap. */
static bool x_icon_load(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
  (void)nargs;
  union dvm_value type = { 0 }, drawable = { 0 };
  (void)dvm_get_field(vm, self, "type", "I", &type);
  dvm_ref ctx = ARG(0).l;
  if (!ctx) RETL(0);
  union dvm_value res = { 0 };
  if (!am_vcall(vm, ctx, "getResources", "()Landroid/content/res/Resources;", NULL, 0, &res)) return false;
  if (type.i == ICON_RESOURCE) {
    union dvm_value id = { 0 }, ign = { 0 }, theme = { 0 };
    (void)dvm_get_field(vm, self, "resId", "I", &id);
    union dvm_value a[2] = { id, theme };
    if (!am_vcall(vm, res.l, "getDrawable", "(ILandroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;", a, 2, &drawable)) return false;
    (void)ign;
  } else if (type.i == ICON_BITMAP || type.i == ICON_ADAPTIVE) {
    union dvm_value bmp = { 0 };
    (void)dvm_get_field(vm, self, "bitmap", "Landroid/graphics/Bitmap;", &bmp);
    dvm_ref d = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/drawable/BitmapDrawable"));
    if (!d) return false;
    dvm_pin(vm, d);
    union dvm_value a[2] = { res, bmp }, ign = { 0 };
    const bool ok = am_vcall(vm, d, "<init>", "(Landroid/content/res/Resources;Landroid/graphics/Bitmap;)V", a, 2, &ign);
    dvm_unpin(vm, d);
    if (!ok) return false;
    drawable.l = d;
  }
  if (drawable.l) {
    union dvm_value tint = { 0 }, ign = { 0 };
    (void)dvm_get_field(vm, self, "tintList", "Landroid/content/res/ColorStateList;", &tint);
    if (tint.l && !am_vcall(vm, drawable.l, "setTintList", "(Landroid/content/res/ColorStateList;)V", &tint, 1, &ign)) return false;
  }
  RETL(drawable.l);
}
static const struct rt_method rt_icon[] = {
  SM("createWithResource", "(Landroid/content/Context;I)Landroid/graphics/drawable/Icon;", x_icon_create),
  SM("createWithResource", "(Ljava/lang/String;I)Landroid/graphics/drawable/Icon;", x_icon_create),
  SM("createWithBitmap", "(Landroid/graphics/Bitmap;)Landroid/graphics/drawable/Icon;", x_icon_create),
  SM("createWithAdaptiveBitmap", "(Landroid/graphics/Bitmap;)Landroid/graphics/drawable/Icon;", x_icon_create),
  SM("createWithData", "([BII)Landroid/graphics/drawable/Icon;", x_icon_create),
  SM("createWithContentUri", "(Landroid/net/Uri;)Landroid/graphics/drawable/Icon;", x_icon_create),
  SM("createWithContentUri", "(Ljava/lang/String;)Landroid/graphics/drawable/Icon;", x_icon_create),
  M("getType", "()I", x_icon_get),
  M("getResId", "()I", x_icon_get),
  M("getResPackage", "()Ljava/lang/String;", x_icon_get),
  M("getBitmap", "()Landroid/graphics/Bitmap;", x_icon_get),
  M("getUri", "()Landroid/net/Uri;", x_icon_get),
  M("setTint", "(I)Landroid/graphics/drawable/Icon;", x_icon_tint),
  M("setTintList", "(Landroid/content/res/ColorStateList;)Landroid/graphics/drawable/Icon;", x_icon_tint),
  M("loadDrawable", "(Landroid/content/Context;)Landroid/graphics/drawable/Drawable;", x_icon_load),
  M_END,
};

/* ---- android.app.Person ---------------------------------------------------- */
static const struct rt_field rt_person_fields[] = {
  { "name", "Ljava/lang/CharSequence;" }, { "icon", "Landroid/graphics/drawable/Icon;" },
  { "uri", "Ljava/lang/String;" }, { "key", "Ljava/lang/String;" }, { "bot", "Z" }, { "important", "Z" },
  F_END,
};
static bool x_person_builder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    if (nargs && ARG(0).l) {   /* Builder(Person) */
      static const char *const f[] = { "name", "icon", "uri", "key", "bot", "important" };
      static const char *const t[] = { "Ljava/lang/CharSequence;", "Landroid/graphics/drawable/Icon;", "Ljava/lang/String;",
                                       "Ljava/lang/String;", "Z", "Z"
                                     };
      for (int i = 0; i < 6; ++i) {
        union dvm_value v = { 0 };
        (void)dvm_get_field(vm, ARG(0).l, f[i], t[i], &v);
        (void)dvm_set_field(vm, self, f[i], t[i], v);
      }
    }
    RETV();
  }
  if (!strcmp(n, "build")) {
    struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/app/Person;");
    dvm_ref p = c ? dvm_new_object(vm, c) : 0;
    if (!p) return false;
    static const char *const f[] = { "name", "icon", "uri", "key", "bot", "important" };
    static const char *const t[] = { "Ljava/lang/CharSequence;", "Landroid/graphics/drawable/Icon;", "Ljava/lang/String;",
                                     "Ljava/lang/String;", "Z", "Z"
                                   };
    for (int i = 0; i < 6; ++i) {
      union dvm_value v = { 0 };
      (void)dvm_get_field(vm, self, f[i], t[i], &v);
      (void)dvm_set_field(vm, p, f[i], t[i], v);
    }
    RETL(p);
  }
  const char *field = !strcmp(n, "setName") ? "name" : !strcmp(n, "setIcon") ? "icon" : !strcmp(n, "setUri") ? "uri"
                      : !strcmp(n, "setKey") ? "key" : !strcmp(n, "setBot") ? "bot" : "important";
  const char *type = !strcmp(field, "name") ? "Ljava/lang/CharSequence;" : !strcmp(field, "icon") ? "Landroid/graphics/drawable/Icon;"
                     : (!strcmp(field, "bot") || !strcmp(field, "important")) ? "Z" : "Ljava/lang/String;";
  (void)dvm_set_field(vm, self, field, type, ARG(0));
  RETL(self);
}
static bool x_person_get(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "getName")) return dvm_get_field(vm, self, "name", "Ljava/lang/CharSequence;", out);
  if (!strcmp(n, "getIcon")) return dvm_get_field(vm, self, "icon", "Landroid/graphics/drawable/Icon;", out);
  if (!strcmp(n, "getUri")) return dvm_get_field(vm, self, "uri", "Ljava/lang/String;", out);
  if (!strcmp(n, "getKey")) return dvm_get_field(vm, self, "key", "Ljava/lang/String;", out);
  if (!strcmp(n, "isBot")) return dvm_get_field(vm, self, "bot", "Z", out);
  if (!strcmp(n, "isImportant")) return dvm_get_field(vm, self, "important", "Z", out);
  /* toBuilder() */
  struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/app/Person$Builder;");
  dvm_ref b = c ? dvm_new_object(vm, c) : 0;
  if (!b) return false;
  dvm_pin(vm, b);
  union dvm_value a = { .l = self }, ign = { 0 };
  const bool ok = am_vcall(vm, b, "<init>", "(Landroid/app/Person;)V", &a, 1, &ign);
  dvm_unpin(vm, b);
  if (!ok) return false;
  RETL(b);
}
static const struct rt_method rt_person[] = {
  M("getName", "()Ljava/lang/CharSequence;", x_person_get), M("getIcon", "()Landroid/graphics/drawable/Icon;", x_person_get),
  M("getUri", "()Ljava/lang/String;", x_person_get), M("getKey", "()Ljava/lang/String;", x_person_get),
  M("isBot", "()Z", x_person_get), M("isImportant", "()Z", x_person_get),
  M("toBuilder", "()Landroid/app/Person$Builder;", x_person_get),
  M_END,
};
static const struct rt_method rt_person_builder[] = {
  M("<init>", "()V", x_person_builder), M("<init>", "(Landroid/app/Person;)V", x_person_builder),
  M("setName", "(Ljava/lang/CharSequence;)Landroid/app/Person$Builder;", x_person_builder),
  M("setIcon", "(Landroid/graphics/drawable/Icon;)Landroid/app/Person$Builder;", x_person_builder),
  M("setUri", "(Ljava/lang/String;)Landroid/app/Person$Builder;", x_person_builder),
  M("setKey", "(Ljava/lang/String;)Landroid/app/Person$Builder;", x_person_builder),
  M("setBot", "(Z)Landroid/app/Person$Builder;", x_person_builder),
  M("setImportant", "(Z)Landroid/app/Person$Builder;", x_person_builder),
  M("build", "()Landroid/app/Person;", x_person_builder),
  M_END,
};

/* ---- small state holders ---------------------------------------------------- */
/* ImageView.setColorFilter: kept on the view and applied to its drawable. */
static bool x_image_color_filter(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name(), *sig = builtin_sig();
  union dvm_value filter = { 0 }, ign = { 0 };
  if (!strcmp(n, "clearColorFilter")) {
    /* no filter */
  } else if (!strcmp(sig, "(Landroid/graphics/ColorFilter;)V")) {
    filter = ARG(0);
  } else {
    dvm_ref f = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/PorterDuffColorFilter"));
    if (!f) return false;
    dvm_pin(vm, f);
    union dvm_value mode = { 0 };
    if (!strcmp(sig, "(I)V")) {   /* setColorFilter(color): SRC_ATOP */
      struct dvm_class *pm = dvm_find_class(vm, "android/graphics/PorterDuff$Mode");
      if (!pm || !dvm_get_static(vm, pm, "SRC_ATOP", "Landroid/graphics/PorterDuff$Mode;", &mode)) {
        dvm_unpin(vm, f);
        return false;
      }
    } else mode = ARG(1);
    union dvm_value a[2] = { ARG(0), mode };
    const bool ok = am_vcall(vm, f, "<init>", "(ILandroid/graphics/PorterDuff$Mode;)V", a, 2, &ign);
    dvm_unpin(vm, f);
    if (!ok) return false;
    filter.l = f;
  }
  (void)dvm_set_field(vm, self, "colorFilter", "Landroid/graphics/ColorFilter;", filter);
  dvm_ref drawable = ui_obj(vm, self, "drawable", "Landroid/graphics/drawable/Drawable;");
  if (drawable && !am_vcall(vm, drawable, "setColorFilter", "(Landroid/graphics/ColorFilter;)V", &filter, 1, &ign)) return false;
  ui_invalidate_view(vm, self, NULL, 0);
  RETV();
}
static bool x_image_get_color_filter(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)args;
  (void)nargs;
  return dvm_get_field(vm, self, "colorFilter", "Landroid/graphics/ColorFilter;", out);
}
static const struct rt_method rt_gap_image_view[] = {
  M("setColorFilter", "(I)V", x_image_color_filter),
  M("setColorFilter", "(ILandroid/graphics/PorterDuff$Mode;)V", x_image_color_filter),
  M("setColorFilter", "(Landroid/graphics/ColorFilter;)V", x_image_color_filter),
  M("clearColorFilter", "()V", x_image_color_filter),
  M("getColorFilter", "()Landroid/graphics/ColorFilter;", x_image_get_color_filter),
  M_END,
};

/* Plain stored properties: a setter/getter pair over one declared field. */
#define GAP_PROP(fn, field, type, set_sig, get_sig, set_name, get_name, retself) \
  static bool fn(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out) \
  { \
    (void)nargs; \
    if (!strcmp(builtin_name(), set_name)) { \
      (void)dvm_set_field(vm, self, field, type, ARG(0)); \
      if (retself) RETL(self); \
      RETV(); \
    } \
    return dvm_get_field(vm, self, field, type, out); \
  }
GAP_PROP(x_msg_async, "asynchronous", "Z", "(Z)V", "()Z", "setAsynchronous", "isAsynchronous", 0)
GAP_PROP(x_paint_hinting, "hinting", "I", "(I)V", "()I", "setHinting", "getHinting", 0)
GAP_PROP(x_text_hscroll, "horizontallyScrolling", "Z", "(Z)V", "()Z", "setHorizontallyScrolling", "getHorizontallyScrolling", 0)
GAP_PROP(x_view_fade_len, "fadingEdgeLength", "I", "(I)V", "()I", "setFadingEdgeLength", "getVerticalFadingEdgeLength", 0)
GAP_PROP(x_list_cache_hint, "listCacheColorHint", "I", "(I)V", "()I", "setCacheColorHint", "getCacheColorHint", 0)
GAP_PROP(x_helper_idle, "idleConnectionTimeout", "J", "(J)V", "()J", "setIdleConnectionTimeout", "getIdleConnectionTimeout", 0)
#undef GAP_PROP

static bool x_text_shadow_color(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)args;
  (void)nargs;
  /* The shadow's colour: whatever setShadowLayer() recorded, else none. */
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "shadowColor", "I", &v);
  RETI(v.i);
}
static bool x_list_selector(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  /* setSelector(int resId): a resource, resolved the way setBackgroundResource() is. */
  union dvm_value d = { 0 }, ign = { 0 }, ctx = { 0 };
  if (!am_vcall(vm, self, "getContext", "()Landroid/content/Context;", NULL, 0, &ctx)) return false;
  union dvm_value res = { 0 }, a[2] = { ARG(0), { 0 } };
  if (!am_vcall(vm, ctx.l, "getResources", "()Landroid/content/res/Resources;", NULL, 0, &res) ||
      !am_vcall(vm, res.l, "getDrawable", "(ILandroid/content/res/Resources$Theme;)Landroid/graphics/drawable/Drawable;", a, 2, &d)) return false;
  (void)dvm_set_field(vm, self, "listSelector", "Landroid/graphics/drawable/Drawable;", d);
  (void)ign;
  (void)out;
  RETV();
}
static const struct rt_method rt_gap_message[] = {
  M("setAsynchronous", "(Z)V", x_msg_async), M("isAsynchronous", "()Z", x_msg_async), M_END,
};
static const struct rt_method rt_gap_paint[] = {
  M("setHinting", "(I)V", x_paint_hinting), M("getHinting", "()I", x_paint_hinting), M_END,
};
/* TextView.setShowSoftInputOnFocus(): whether focusing the field raises the keyboard by itself.
 * Stored inverted so a view that never set it reads true, as the platform's default. */
static bool x_text_soft_input_focus(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "setShowSoftInputOnFocus")) {
    rt_set_int(vm, self, "noSoftInputOnFocus", ARG(0).i ? 0 : 1);
    RETV();
  }
  RETI(!rt_get_int(vm, self, "noSoftInputOnFocus"));
}
static const struct rt_method rt_gap_text_view[] = {
  M("setShowSoftInputOnFocus", "(Z)V", x_text_soft_input_focus),
  M("getShowSoftInputOnFocus", "()Z", x_text_soft_input_focus),
  /* TextView.onTextChanged(): the protected hook subclasses override; the base does nothing. */
  M("onTextChanged", "(Ljava/lang/CharSequence;III)V", nop_void),
  /* TextView.onSelectionChanged(): likewise, a hook for subclasses. */
  M("onSelectionChanged", "(II)V", nop_void),
  M("getShadowColor", "()I", x_text_shadow_color),
  M("setHorizontallyScrolling", "(Z)V", x_text_hscroll), M("getHorizontallyScrolling", "()Z", x_text_hscroll), M_END,
};
static const struct rt_method rt_gap_view[] = {
  M("setFadingEdgeLength", "(I)V", x_view_fade_len), M("getVerticalFadingEdgeLength", "()I", x_view_fade_len), M_END,
};
static const struct rt_method rt_gap_abs_list[] = {
  M("setCacheColorHint", "(I)V", x_list_cache_hint), M("getCacheColorHint", "()I", x_list_cache_hint),
  M("setSelector", "(I)V", x_list_selector), M_END,
};
static const struct rt_method rt_gap_sqlite_helper[] = {
  M("setIdleConnectionTimeout", "(J)V", x_helper_idle), M_END,
};

/* ---- java.lang.Math.scalb, android.system.Os.sysconf, SdkExtensions ---------- */
static bool x_math_scalb(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)nargs;
  if (builtin_sig()[1] == 'F') {
    out->f = scalbnf(ARG(0).f, ARG(1).i);
    return true;
  }
  out->d = scalbn(ARG(0).d, ARG(1).i);
  return true;
}
static bool x_os_sysconf(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)self;
  (void)nargs;
  /* The bionic _SC_ values apps ask for. */
  const int name = ARG(0).i;
  long v = sysconf(name == 0x27 ? _SC_PAGESIZE : name == 0x60 ? _SC_NPROCESSORS_CONF : name == 0x61 ? _SC_NPROCESSORS_ONLN
                   : name == 0x02 ? _SC_CLK_TCK : name == 0x28 ? _SC_PHYS_PAGES : name == 0x29 ? _SC_AVPHYS_PAGES : -1);
  if (v < 0) {
    dvm__throw(vm, "android/system/ErrnoException", "sysconf failed: EINVAL (Invalid argument)");
    return false;
  }
  out->j = v;
  return true;
}
static bool x_sdk_all_versions(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  dvm_ref map = coll_new_map(vm);
  if (!map) return false;
  dvm_pin(vm, map);
  union dvm_value r = { 0 };
  /* The extension ids this release defines (R=30, S=31) each at version 2. */
  for (int id = 30; id <= 31; ++id) {
    union dvm_value k = { 0 }, v = { 0 }, a[2];
    struct dvm_class *ic = dvm_find_class(vm, "java/lang/Integer");
    struct dvm_method *vo = ic ? dvm_find_method(vm, ic, "valueOf", "(I)Ljava/lang/Integer;") : NULL;
    union dvm_value i1 = { .i = id }, i2 = { .i = 2 };
    if (!vo || !dvm_call(vm, vo, 0, &i1, 1, &k) || !dvm_call(vm, vo, 0, &i2, 1, &v)) {
      dvm_unpin(vm, map);
      return false;
    }
    a[0] = k;
    a[1] = v;
    if (!am_vcall(vm, map, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", a, 2, &r)) {
      dvm_unpin(vm, map);
      return false;
    }
  }
  dvm_unpin(vm, map);
  RETL(map);
}
static const struct rt_method rt_gap_math[] = {
  SM("scalb", "(FI)F", x_math_scalb), SM("scalb", "(DI)D", x_math_scalb), M_END,
};
static const struct rt_method rt_gap_os[] = {
  SM("sysconf", "(I)J", x_os_sysconf), M_END,
};
static const struct rt_method rt_gap_sdk_ext[] = {
  SM("getAllExtensionVersions", "()Ljava/util/Map;", x_sdk_all_versions), M_END,
};

/* ---- android.util.TimingLogger ----------------------------------------------- */
static const struct rt_field rt_timing_fields[] = {
  { "tag", "Ljava/lang/String;" }, { "label", "Ljava/lang/String;" }, { "splits", "Ljava/util/List;" }, F_END,
};
static bool x_timing(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                     union dvm_value *out)
{
  (void)out;
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    (void)dvm_set_field(vm, self, "tag", "Ljava/lang/String;", ARG(0));
    (void)dvm_set_field(vm, self, "label", "Ljava/lang/String;", ARG(1));
    RETV();
  }
  if (!strcmp(n, "reset")) {
    union dvm_value l = { .l = 0 };
    if (nargs >= 2) {
      (void)dvm_set_field(vm, self, "tag", "Ljava/lang/String;", ARG(0));
      (void)dvm_set_field(vm, self, "label", "Ljava/lang/String;", ARG(1));
    }
    (void)dvm_set_field(vm, self, "splits", "Ljava/util/List;", l);
    RETV();
  }
  if (!strcmp(n, "addSplit")) {
    union dvm_value l = { 0 }, ign = { 0 };
    (void)dvm_get_field(vm, self, "splits", "Ljava/util/List;", &l);
    if (!l.l) {
      l.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      (void)dvm_set_field(vm, self, "splits", "Ljava/util/List;", l);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char buf[256];
    snprintf(buf, sizeof buf, "%lld %s", (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : "");
    union dvm_value s = { .l = dvm_new_string(vm, buf) };
    return am_vcall(vm, l.l, "add", "(Ljava/lang/Object;)Z", &s, 1, &ign);
  }
  /* dumpToLog(): the splits are timing data for the log; a device logs them at
   * VERBOSE only when the tag is loggable, which is off by default. */
  RETV();
}
static const struct rt_method rt_timing_logger[] = {
  M("<init>", "(Ljava/lang/String;Ljava/lang/String;)V", x_timing),
  M("reset", "()V", x_timing), M("reset", "(Ljava/lang/String;Ljava/lang/String;)V", x_timing),
  M("addSplit", "(Ljava/lang/String;)V", x_timing), M("dumpToLog", "()V", x_timing), M_END,
};

/* ---- android.util.Rational, android.os.ParcelUuid ------------------------------ */
static const struct rt_field rt_rational_fields[] = { { "num", "I" }, { "den", "I" }, F_END };
static int rat_gcd(int a, int b)
{
  a = a < 0 ? -a : a;
  b = b < 0 ? -b : b;
  while (b) {
    int t = a % b;
    a = b;
    b = t;
  }
  return a;
}
static bool x_rational(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  union dvm_value a = { 0 }, b = { 0 };
  (void)dvm_get_field(vm, self, "num", "I", &a);
  (void)dvm_get_field(vm, self, "den", "I", &b);
  if (!strcmp(n, "<init>")) {
    int num = ARG(0).i, den = ARG(1).i;
    if (den == 0 && num == 0) { /* NaN */ }
    else if (den == 0) num = num > 0 ? 1 : -1;
    else {
      if (den < 0) {
        num = -num;
        den = -den;
      }
      const int g = rat_gcd(num, den);
      if (g > 1) {
        num /= g;
        den /= g;
      }
    }
    (void)dvm_set_field(vm, self, "num", "I", (union dvm_value) {
      .i = num
    });
    (void)dvm_set_field(vm, self, "den", "I", (union dvm_value) {
      .i = den
    });
    RETV();
  }
  if (!strcmp(n, "getNumerator")) RETI(a.i);
  if (!strcmp(n, "getDenominator")) RETI(b.i);
  if (!strcmp(n, "doubleValue")) out->d = b.i == 0 ? (a.i > 0 ? INFINITY : a.i < 0 ? -INFINITY : NAN) : (double)a.i / (double)b.i, (void)0;
  else if (!strcmp(n, "floatValue")) out->f = (float)(b.i == 0 ? (a.i > 0 ? INFINITY : a.i < 0 ? -INFINITY : NAN) : (double)a.i / (double)b.i);
  else if (!strcmp(n, "intValue")) out->i = b.i == 0 ? 0 : a.i / b.i;
  else if (!strcmp(n, "longValue")) out->j = b.i == 0 ? 0 : a.i / b.i;
  else if (!strcmp(n, "toString")) {
    char buf[40];
    if (b.i == 0 && a.i == 0) snprintf(buf, sizeof buf, "NaN");
    else if (b.i == 0) snprintf(buf, sizeof buf, a.i > 0 ? "Infinity" : "-Infinity");
    else if (a.i == 0) snprintf(buf, sizeof buf, "0");
    else if (b.i == 1) snprintf(buf, sizeof buf, "%d", a.i);
    else snprintf(buf, sizeof buf, "%d/%d", a.i, b.i);
    RETL(dvm_new_string(vm, buf));
  } else if (!strcmp(n, "equals")) {
    struct dvm_object *o = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL, *me = dvm__obj(vm, self);
    union dvm_value x = { 0 }, y = { 0 };
    if (!o || !me || o->cls != me->cls) RETI(0);
    (void)dvm_get_field(vm, ARG(0).l, "num", "I", &x);
    (void)dvm_get_field(vm, ARG(0).l, "den", "I", &y);
    RETI(x.i == a.i && y.i == b.i);
  } else if (!strcmp(n, "hashCode")) RETI(a.i ^ ((b.i << 16) | ((unsigned)b.i >> 16)));
  return true;
}
static const struct rt_method rt_rational[] = {
  M("<init>", "(II)V", x_rational), M("getNumerator", "()I", x_rational), M("getDenominator", "()I", x_rational),
  M("doubleValue", "()D", x_rational), M("floatValue", "()F", x_rational), M("intValue", "()I", x_rational),
  M("longValue", "()J", x_rational), M("toString", "()Ljava/lang/String;", x_rational),
  M("equals", "(Ljava/lang/Object;)Z", x_rational), M("hashCode", "()I", x_rational), M_END,
};
static const struct rt_field rt_parcel_uuid_fields[] = { { "uuid", "Ljava/util/UUID;" }, F_END };
static bool x_parcel_uuid(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    (void)dvm_set_field(vm, self, "uuid", "Ljava/util/UUID;", ARG(0));
    RETV();
  }
  union dvm_value u = { 0 };
  (void)dvm_get_field(vm, self, "uuid", "Ljava/util/UUID;", &u);
  if (!strcmp(n, "getUuid")) RETL(u.l);
  if (!strcmp(n, "toString")) return am_vcall(vm, u.l, "toString", "()Ljava/lang/String;", NULL, 0, out);
  if (!strcmp(n, "hashCode")) return am_vcall(vm, u.l, "hashCode", "()I", NULL, 0, out);
  if (!strcmp(n, "equals")) {
    struct dvm_object *o = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL, *me = dvm__obj(vm, self);
    if (!o || !me || o->cls != me->cls) RETI(0);
    union dvm_value v = { 0 }, a = { 0 };
    (void)dvm_get_field(vm, ARG(0).l, "uuid", "Ljava/util/UUID;", &v);
    a.l = v.l;
    return am_vcall(vm, u.l, "equals", "(Ljava/lang/Object;)Z", &a, 1, out);
  }
  /* static fromString(String) */
  struct dvm_class *uc = dvm_find_class(vm, "java/util/UUID");
  struct dvm_method *fs = uc ? dvm_find_method(vm, uc, "fromString", "(Ljava/lang/String;)Ljava/util/UUID;") : NULL;
  union dvm_value parsed = { 0 };
  union dvm_value arg0 = ARG(0);
  if (!fs || !dvm_call(vm, fs, 0, &arg0, 1, &parsed)) return false;
  dvm_ref p = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/os/ParcelUuid;"));
  if (!p) return false;
  (void)dvm_set_field(vm, p, "uuid", "Ljava/util/UUID;", parsed);
  RETL(p);
}
static const struct rt_method rt_parcel_uuid[] = {
  M("<init>", "(Ljava/util/UUID;)V", x_parcel_uuid), M("getUuid", "()Ljava/util/UUID;", x_parcel_uuid),
  M("toString", "()Ljava/lang/String;", x_parcel_uuid), M("hashCode", "()I", x_parcel_uuid),
  M("equals", "(Ljava/lang/Object;)Z", x_parcel_uuid),
  SM("fromString", "(Ljava/lang/String;)Landroid/os/ParcelUuid;", x_parcel_uuid), M_END,
};

/* ---- android.webkit.CookieSyncManager / WebView.disableWebView ------------------ */
static bool x_cookie_sync(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)args;
  (void)nargs;
  static dvm_ref instance;
  const char *n = builtin_name();
  if (!strcmp(n, "createInstance") || !strcmp(n, "getInstance")) {
    if (!instance) {
      if (!strcmp(n, "getInstance")) {
        dvm__throw(vm, "java/lang/IllegalStateException", "CookieSyncManager::createInstance() needs to be called before CookieSyncManager::getInstance()");
        return false;
      }
      instance = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/webkit/CookieSyncManager;"));
      if (!instance) return false;
      dvm_pin(vm, instance);
    }
    RETL(instance);
  }
  /* sync / resetSync / startSync / stopSync: cookies live in CookieManager's own
   * store, which is already the persistent one: there is nothing to flush. */
  (void)self;
  RETV();
}
static const struct rt_method rt_cookie_sync[] = {
  SM("createInstance", "(Landroid/content/Context;)Landroid/webkit/CookieSyncManager;", x_cookie_sync),
  SM("getInstance", "()Landroid/webkit/CookieSyncManager;", x_cookie_sync),
  M("sync", "()V", x_cookie_sync), M("resetSync", "()V", x_cookie_sync),
  M("startSync", "()V", x_cookie_sync), M("stopSync", "()V", x_cookie_sync), M_END,
};
static bool x_webview_disable(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  (void)out;
  /* Only legal before any WebView exists: the app's own opt-out, which a later
   * WebView construction would not honour on a device that already has one. */
  RETV();
}
static const struct rt_method rt_gap_webview[] = { SM("disableWebView", "()V", x_webview_disable), M_END };

/* ---- AccessibilityManager, NotificationManager ----------------------------------- */
static bool x_a11y_services(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  /* No accessibility service is installed or enabled on this device. */
  RETL(coll_new_list(vm, "Ljava/util/ArrayList;"));
}
static const struct rt_method rt_gap_a11y[] = {
  M("getEnabledAccessibilityServiceList", "(I)Ljava/util/List;", x_a11y_services),
  M("getInstalledAccessibilityServiceList", "()Ljava/util/List;", x_a11y_services), M_END,
};
static bool x_notif_channels(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  union dvm_value list = { 0 }, ign = { 0 };
  if (!strcmp(n, "getNotificationChannelGroups")) RETL(coll_new_list(vm, "Ljava/util/ArrayList;"));
  if (!strcmp(n, "createNotificationChannels")) {
    union dvm_value it = { 0 };
    if (!ARG(0).l || !am_vcall(vm, ARG(0).l, "iterator", "()Ljava/util/Iterator;", NULL, 0, &it)) return false;
    for (;;) {
      union dvm_value more = { 0 }, ch = { 0 };
      if (!am_vcall(vm, it.l, "hasNext", "()Z", NULL, 0, &more)) return false;
      if (!more.i) break;
      if (!am_vcall(vm, it.l, "next", "()Ljava/lang/Object;", NULL, 0, &ch)) return false;
      union dvm_value a = { .l = ch.l };
      if (!am_vcall(vm, self, "createNotificationChannel", "(Landroid/app/NotificationChannel;)V", &a, 1, &ign)) return false;
    }
    RETV();
  }
  /* deleteNotificationChannel(id): drop the channel with that id. */
  (void)dvm_get_field(vm, self, "channels", "Ljava/util/List;", &list);
  if (!list.l || !ARG(0).l) RETV();
  union dvm_value it = { 0 };
  if (!am_vcall(vm, list.l, "iterator", "()Ljava/util/Iterator;", NULL, 0, &it)) return false;
  for (;;) {
    union dvm_value more = { 0 }, ch = { 0 }, id = { 0 }, eq = { 0 };
    if (!am_vcall(vm, it.l, "hasNext", "()Z", NULL, 0, &more)) return false;
    if (!more.i) break;
    if (!am_vcall(vm, it.l, "next", "()Ljava/lang/Object;", NULL, 0, &ch) ||
        !am_vcall(vm, ch.l, "getId", "()Ljava/lang/String;", NULL, 0, &id)) return false;
    union dvm_value a = { .l = ARG(0).l };
    if (id.l && am_vcall(vm, id.l, "equals", "(Ljava/lang/Object;)Z", &a, 1, &eq) && eq.i) {
      if (!am_vcall(vm, it.l, "remove", "()V", NULL, 0, &ign)) return false;
      break;
    }
  }
  RETV();
}
static const struct rt_method rt_gap_notification[] = {
  M("deleteNotificationChannel", "(Ljava/lang/String;)V", x_notif_channels),
  M("getNotificationChannelGroups", "()Ljava/util/List;", x_notif_channels),
  M("createNotificationChannels", "(Ljava/util/List;)V", x_notif_channels), M_END,
};

/* ---- radios the device does not have ---------------------------------------------- */
static bool x_wifi_band(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI(0);   /* the emulated Wi-Fi is a 2.4 GHz-only link (is5GHz / is6GHz both false) */
}
static const struct rt_method rt_gap_wifi[] = {
  M("is5GHzBandSupported", "()Z", x_wifi_band), M("is6GHzBandSupported", "()Z", x_wifi_band), M_END,
};
static bool x_net_signal(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)vm;
  (void)self;
  (void)args;
  (void)nargs;
  RETI((int32_t)0x80000000);   /* SIGNAL_STRENGTH_UNSPECIFIED: a wired link has none */
}
static const struct rt_method rt_gap_net_caps[] = { M("getSignalStrength", "()I", x_net_signal), M_END };

/* ---- java.util.OptionalInt / OptionalLong / OptionalDouble ------------------------ */
static const struct rt_field rt_optional_prim_fields[] = {
  { "present", "Z" }, { "ivalue", "J" }, { "dvalue", "D" }, F_END,
};
static dvm_ref optp_make(struct dvm *vm, const char *desc, bool present, int64_t i, double d)
{
  struct dvm_class *c = dvm__class_by_desc(vm, desc);
  dvm_ref o = c ? dvm_new_object(vm, c) : 0;
  if (!o) return 0;
  (void)dvm_set_field(vm, o, "present", "Z", (union dvm_value) {
    .i = present
  });
  (void)dvm_set_field(vm, o, "ivalue", "J", (union dvm_value) {
    .j = i
  });
  (void)dvm_set_field(vm, o, "dvalue", "D", (union dvm_value) {
    .d = d
  });
  return o;
}
/* Which of the three the object (or a static call's signature) is. */
static char optp_kind(struct dvm *vm, dvm_ref self, const char *sig)
{
  if (self) {
    const char *n = dvm_class_name(dvm_object_class(vm, self));
    if (n) {
      const size_t l = strlen(n);
      return n[l - 1] == 't' ? 'I' : n[l - 1] == 'g' ? 'J' : 'D';   /* Int, Long, Double -> t, g, e */
    }
  }
  const char *r = strrchr(sig, ')');
  return r && r[1] == 'L' ? (strstr(r, "OptionalInt") ? 'I' : strstr(r, "OptionalLong") ? 'J' : 'D') : 'I';
}
static const char *optp_desc(char kind)
{
  return kind == 'I' ? "Ljava/util/OptionalInt;" : kind == 'J' ? "Ljava/util/OptionalLong;" : "Ljava/util/OptionalDouble;";
}
static bool x_optp(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                   union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name(), *sig = builtin_sig();
  const char kind = optp_kind(vm, self, sig);
  if (!strcmp(n, "empty")) RETL(optp_make(vm, optp_desc(kind), false, 0, 0));
  if (!strcmp(n, "of")) {
    RETL(kind == 'D' ? optp_make(vm, optp_desc(kind), true, 0, ARG(0).d)
         : optp_make(vm, optp_desc(kind), true, kind == 'I' ? (int64_t)ARG(0).i : ARG(0).j, 0));
  }
  union dvm_value present = { 0 }, iv = { 0 }, dv = { 0 };
  (void)dvm_get_field(vm, self, "present", "Z", &present);
  (void)dvm_get_field(vm, self, "ivalue", "J", &iv);
  (void)dvm_get_field(vm, self, "dvalue", "D", &dv);
  union dvm_value value = kind == 'D' ? (union dvm_value) {
    .d = dv.d
  } : kind == 'I' ? (union dvm_value) {
    .i = (int32_t)iv.j
  } : (union dvm_value) {
    .j = iv.j
  };
  if (!strcmp(n, "isPresent")) RETI(present.i);
  if (!strcmp(n, "isEmpty")) RETI(!present.i);
  if (!strncmp(n, "getAs", 5) || (!strcmp(n, "orElseThrow") && !strstr(sig, "Supplier"))) {
    if (!present.i) {
      dvm__throw(vm, "java/util/NoSuchElementException", "No value present");
      return false;
    }
    *out = value;
    return true;
  }
  if (!strcmp(n, "orElse")) {
    *out = present.i ? value : ARG(0);
    return true;
  }
  if (!strcmp(n, "orElseGet")) {
    if (present.i) {
      *out = value;
      return true;
    }
    const char *g = kind == 'I' ? "getAsInt" : kind == 'J' ? "getAsLong" : "getAsDouble";
    const char *gs = kind == 'I' ? "()I" : kind == 'J' ? "()J" : "()D";
    return am_vcall(vm, ARG(0).l, g, gs, NULL, 0, out);
  }
  if (!strcmp(n, "orElseThrow")) {   /* (Supplier): throw what it makes */
    if (present.i) {
      *out = value;
      return true;
    }
    union dvm_value ex = { 0 };
    if (!am_vcall(vm, ARG(0).l, "get", "()Ljava/lang/Object;", NULL, 0, &ex)) return false;
    vm->exception = ex.l;
    return false;
  }
  if (!strcmp(n, "ifPresent") || !strcmp(n, "ifPresentOrElse")) {
    union dvm_value ign = { 0 }, a = value;
    if (present.i) {
      const char *sg = kind == 'I' ? "(I)V" : kind == 'J' ? "(J)V" : "(D)V";
      return am_vcall(vm, ARG(0).l, "accept", sg, &a, 1, &ign);
    }
    if (!strcmp(n, "ifPresentOrElse") && ARG(1).l) return am_vcall(vm, ARG(1).l, "run", "()V", NULL, 0, &ign);
    RETV();
  }
  if (!strcmp(n, "equals")) {
    struct dvm_object *o = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL, *me = dvm__obj(vm, self);
    if (!o || !me || o->cls != me->cls) RETI(0);
    union dvm_value p2 = { 0 }, i2 = { 0 }, d2 = { 0 };
    (void)dvm_get_field(vm, ARG(0).l, "present", "Z", &p2);
    (void)dvm_get_field(vm, ARG(0).l, "ivalue", "J", &i2);
    (void)dvm_get_field(vm, ARG(0).l, "dvalue", "D", &d2);
    if (!present.i || !p2.i) RETI(present.i == p2.i);
    RETI(kind == 'D' ? (dv.d == d2.d || (dv.d != dv.d && d2.d != d2.d)) : iv.j == i2.j);
  }
  if (!strcmp(n, "hashCode")) {
    if (!present.i) RETI(0);
    if (kind == 'D') {
      int64_t bits;
      memcpy(&bits, &dv.d, sizeof bits);
      RETI((int32_t)(bits ^ ((uint64_t)bits >> 32)));
    }
    RETI(kind == 'I' ? (int32_t)iv.j : (int32_t)(iv.j ^ ((uint64_t)iv.j >> 32)));
  }
  /* toString */
  const char *label = kind == 'I' ? "OptionalInt" : kind == 'J' ? "OptionalLong" : "OptionalDouble";
  char buf[96];
  if (!present.i) snprintf(buf, sizeof buf, "%s.empty", label);
  else if (kind == 'D') snprintf(buf, sizeof buf, "%s[%s]", label, "");
  else snprintf(buf, sizeof buf, "%s[%lld]", label, (long long)iv.j);
  if (present.i && kind == 'D') {
    union dvm_value s = { 0 }, a = { .d = dv.d };
    struct dvm_class *dc = dvm_find_class(vm, "java/lang/Double");
    struct dvm_method *ts = dc ? dvm_find_method(vm, dc, "toString", "(D)Ljava/lang/String;") : NULL;
    if (!ts || !dvm_call(vm, ts, 0, &a, 1, &s)) return false;
    snprintf(buf, sizeof buf, "%s[%s]", label, dvm_string_utf8(vm, s.l));
  }
  RETL(dvm_new_string(vm, buf));
}
#define OPTP_TABLE(name, T, SIG, CONS, SUP, GET) \
  static const struct rt_method name[] = { \
    SM("empty", "()Ljava/util/Optional" T ";", x_optp), SM("of", "(" SIG ")Ljava/util/Optional" T ";", x_optp), \
    M("isPresent", "()Z", x_optp), M("isEmpty", "()Z", x_optp), M(GET, "()" SIG, x_optp), \
    M("orElse", "(" SIG ")" SIG, x_optp), M("orElseGet", "(L" SUP ";)" SIG, x_optp), \
    M("orElseThrow", "()" SIG, x_optp), M("orElseThrow", "(Ljava/util/function/Supplier;)" SIG, x_optp), \
    M("ifPresent", "(L" CONS ";)V", x_optp), M("ifPresentOrElse", "(L" CONS ";Ljava/lang/Runnable;)V", x_optp), \
    M("equals", "(Ljava/lang/Object;)Z", x_optp), M("hashCode", "()I", x_optp), \
    M("toString", "()Ljava/lang/String;", x_optp), M_END, \
  }
OPTP_TABLE(rt_optional_int, "Int", "I", "java/util/function/IntConsumer", "java/util/function/IntSupplier", "getAsInt");
OPTP_TABLE(rt_optional_long, "Long", "J", "java/util/function/LongConsumer", "java/util/function/LongSupplier", "getAsLong");
OPTP_TABLE(rt_optional_double, "Double", "D", "java/util/function/DoubleConsumer", "java/util/function/DoubleSupplier", "getAsDouble");
#undef OPTP_TABLE

/* Optional itself: equality, hashing, or(), ifPresentOrElse(). */
static bool x_optional_more(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  union dvm_value v = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "value", "Ljava/lang/Object;", &v);
  if (!strcmp(n, "equals")) {
    struct dvm_object *o = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL, *me = dvm__obj(vm, self);
    if (!o || !me || o->cls != me->cls) RETI(0);
    union dvm_value w = { 0 };
    (void)dvm_get_field(vm, ARG(0).l, "value", "Ljava/lang/Object;", &w);
    if (!v.l || !w.l) RETI(v.l == w.l);
    union dvm_value a = { .l = w.l };
    return am_vcall(vm, v.l, "equals", "(Ljava/lang/Object;)Z", &a, 1, out);
  }
  if (!strcmp(n, "hashCode")) {
    if (!v.l) RETI(0);
    return am_vcall(vm, v.l, "hashCode", "()I", NULL, 0, out);
  }
  if (!strcmp(n, "or")) {
    if (v.l) RETL(self);
    return am_vcall(vm, ARG(0).l, "get", "()Ljava/lang/Object;", NULL, 0, out);
  }
  /* ifPresentOrElse(consumer, runnable) */
  if (v.l) {
    union dvm_value a = { .l = v.l };
    return am_vcall(vm, ARG(0).l, "accept", "(Ljava/lang/Object;)V", &a, 1, &ign);
  }
  return am_vcall(vm, ARG(1).l, "run", "()V", NULL, 0, &ign);
}
static const struct rt_method rt_gap_optional[] = {
  M("equals", "(Ljava/lang/Object;)Z", x_optional_more), M("hashCode", "()I", x_optional_more),
  M("or", "(Ljava/util/function/Supplier;)Ljava/util/Optional;", x_optional_more),
  M("ifPresentOrElse", "(Ljava/util/function/Consumer;Ljava/lang/Runnable;)V", x_optional_more), M_END,
};

/* ---- android.view.ViewOverlay / ViewGroupOverlay ----------------------------------- */
/* The overlay keeps what was added (Drawables, and for a group Views) in order, as
 * the real one does; transitions put their transient views there during an
 * animation.  What it holds is not drawn over the host window. */
static const struct rt_field rt_overlay_fields[] = { { "items", "Ljava/util/List;" }, F_END };
static bool x_overlay_of(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value o = { 0 };
  (void)dvm_get_field(vm, self, "overlay", "Landroid/view/ViewOverlay;", &o);
  if (!o.l) {
    const bool group = ui_is_a(vm, self, "android/view/ViewGroup");
    struct dvm_class *c = dvm__class_by_desc(vm, group ? "Landroid/view/ViewGroupOverlay;" : "Landroid/view/ViewOverlay;");
    o.l = c ? dvm_new_object(vm, c) : 0;
    if (!o.l) return false;
    (void)dvm_set_field(vm, self, "overlay", "Landroid/view/ViewOverlay;", o);
  }
  RETL(o.l);
}
static bool x_overlay_items(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  union dvm_value items = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "items", "Ljava/util/List;", &items);
  if (!items.l) {
    items.l = coll_new_list(vm, "Ljava/util/ArrayList;");
    (void)dvm_set_field(vm, self, "items", "Ljava/util/List;", items);
  }
  if (!strcmp(n, "clear")) return am_vcall(vm, items.l, "clear", "()V", NULL, 0, &ign);
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "view must be non-null");
    return false;
  }
  union dvm_value a = { .l = ARG(0).l };
  if (!strcmp(n, "add")) return am_vcall(vm, items.l, "add", "(Ljava/lang/Object;)Z", &a, 1, &ign);
  return am_vcall(vm, items.l, "remove", "(Ljava/lang/Object;)Z", &a, 1, &ign);
}
static const struct rt_method rt_overlay[] = {
  M("add", "(Landroid/graphics/drawable/Drawable;)V", x_overlay_items),
  M("remove", "(Landroid/graphics/drawable/Drawable;)V", x_overlay_items),
  M("clear", "()V", x_overlay_items), M_END,
};
static const struct rt_method rt_group_overlay[] = {
  M("add", "(Landroid/view/View;)V", x_overlay_items), M("remove", "(Landroid/view/View;)V", x_overlay_items), M_END,
};
static const struct rt_method rt_gap_overlay_owner[] = {
  M("getOverlay", "()Landroid/view/ViewOverlay;", x_overlay_of), M_END,
};
static const struct rt_method rt_gap_group_overlay_owner[] = {
  M("getOverlay", "()Landroid/view/ViewGroupOverlay;", x_overlay_of), M_END,
};

/* ---- Animator.setTarget / getTarget, AnimatorInflater.loadAnimator ------------------- */
static bool is_object_animator(struct dvm *vm, dvm_ref a)
{
  struct dvm_class *c = dvm_object_class(vm, a);
  for (; c; c = c->super)
    if (c->name && !strcmp(c->name, "android/animation/ObjectAnimator")) return true;
  return false;
}
/* ObjectAnimator.setTarget(Object); on an AnimatorSet it goes to every child that has a target. */
static bool animator_set_target(struct dvm *vm, dvm_ref a, dvm_ref target, unsigned depth)
{
  if (depth > 8) return true;
  if (is_object_animator(vm, a)) {
    (void)dvm_set_field(vm, a, "target", "Ljava/lang/Object;", (union dvm_value) {
      .l = target
    });
    return true;
  }
  if (is_animator_set(vm, a)) {
    union dvm_value children = { 0 };
    (void)dvm_get_field(vm, a, "children", "Ljava/util/List;", &children);
    struct rt_list *cl = children.l ? list_of(vm, children.l) : NULL;
    for (uint32_t i = 0; cl && i < cl->size; ++i)
      if (!animator_set_target(vm, cl->items[i], target, depth + 1)) return false;
  }
  return true;
}
static bool x_animator_target(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "getTarget")) return dvm_get_field(vm, self, "target", "Ljava/lang/Object;", out);
  if (!animator_set_target(vm, self, ARG(0).l, 0)) return false;
  RETV();
}
static const struct rt_method rt_gap_animator_target[] = {
  M("setTarget", "(Ljava/lang/Object;)V", x_animator_target), M("getTarget", "()Ljava/lang/Object;", x_animator_target), M_END,
};

/* ObjectAnimator over an ARGB property (valueType=colorType): a 0..1 animator whose update
 * listener blends the two colours as ArgbEvaluator does (in linear light, gamma 2.2) and
 * hands the result to the target's setXxx(int). */
static const struct rt_field rt_color_listener_fields[] = {
  { "property", "Ljava/lang/String;" }, { "from", "I" }, { "to", "I" }, F_END,
};
static int32_t argb_blend(float f, int32_t a, int32_t b)
{
  const float sa = ((a >> 24) & 0xff) / 255.0f, sr = powf(((a >> 16) & 0xff) / 255.0f, 2.2f),
              sg = powf(((a >> 8) & 0xff) / 255.0f, 2.2f), sb = powf((a & 0xff) / 255.0f, 2.2f);
  const float ea = ((b >> 24) & 0xff) / 255.0f, er = powf(((b >> 16) & 0xff) / 255.0f, 2.2f),
              eg = powf(((b >> 8) & 0xff) / 255.0f, 2.2f), eb = powf((b & 0xff) / 255.0f, 2.2f);
  float oa = sa + f * (ea - sa), orr = sr + f * (er - sr), og = sg + f * (eg - sg), ob = sb + f * (eb - sb);
  oa *= 255.0f;
  orr = powf(orr, 1.0f / 2.2f) * 255.0f;
  og = powf(og, 1.0f / 2.2f) * 255.0f;
  ob = powf(ob, 1.0f / 2.2f) * 255.0f;
  return (int32_t)(((uint32_t)lroundf(oa) << 24) | ((uint32_t)lroundf(orr) << 16) | ((uint32_t)lroundf(og) << 8) | (uint32_t)lroundf(ob));
}
static bool x_color_listener(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)out;
  (void)nargs;
  dvm_ref animator = ARG(0).l;
  union dvm_value target = { 0 }, frac = { 0 }, prop = { 0 }, from = { 0 }, to = { 0 };
  (void)dvm_get_field(vm, animator, "target", "Ljava/lang/Object;", &target);
  (void)dvm_get_field(vm, animator, "fraction", "F", &frac);
  (void)dvm_get_field(vm, self, "property", "Ljava/lang/String;", &prop);
  (void)dvm_get_field(vm, self, "from", "I", &from);
  (void)dvm_get_field(vm, self, "to", "I", &to);
  const char *name = prop.l ? dvm_string_utf8(vm, prop.l) : NULL;
  if (!target.l || !name || !*name) RETV();   /* no target yet: nothing to drive */
  char setter[96];
  snprintf(setter, sizeof setter, "set%c%s", toupper((unsigned char)name[0]), name + 1);
  struct dvm_class *tc = dvm_object_class(vm, target.l);
  struct dvm_method *m = tc ? dvm_find_method(vm, tc, setter, "(I)V") : NULL;
  if (!m) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "Method %s(int) with type int not found on target class", setter);
    return false;
  }
  union dvm_value v = { .i = argb_blend(frac.f, from.i, to.i) }, ign = { 0 };
  return dvm_call(vm, m, target.l, &v, 1, &ign);
}
static const struct rt_method rt_color_listener[] = {
  M("onAnimationUpdate", "(Landroid/animation/ValueAnimator;)V", x_color_listener), M_END,
};
static const char *const rt_iface_update_listener[] = { "Landroid/animation/ValueAnimator$AnimatorUpdateListener;", NULL };

/* One <objectAnimator> / <animator>: valueFrom/valueTo as float or int, duration,
 * startOffset, repeatCount, repeatMode, interpolator.  Color and path animators
 * are not modelled and are reported, not skipped. */
static bool inflater_value_animator(struct dvm *vm, struct dx *x, const struct dx_tag *t, dvm_ref context,
                                    bool object, dvm_ref *result)
{
  uint8_t type = 0;
  uint32_t data = 0;
  /* valueType: floatType=0, intType=1, pathType=2, colorType=3 */
  const int32_t value_type = dx_int(x, t, "valueType", object ? -1 : 0);
  if (value_type == 2 || (value_type == 3 && !object)) {
    dvm__throw(vm, "android/view/InflateException", "<%s valueType=%s> is not supported",
               object ? "objectAnimator" : "animator", value_type == 2 ? "pathType" : "colorType");
    return false;
  }
  const char *prop = NULL;
  if (object) {
    if (!dx_attr(x, t, "propertyName", &type, &data) || type != AX_STRING || data >= x->ns || !x->s[data]) {
      dvm__throw(vm, "android/view/InflateException", "<objectAnimator> requires propertyName");
      return false;
    }
    prop = x->s[data];
  }
  const bool as_int = value_type == 1;
  const bool as_color = value_type == 3;
  const bool has_from = dx_attr(x, t, "valueFrom", &type, &data);
  const uint32_t n = has_from ? 2 : 1;
  union dvm_value values = { 0 }, animator = { 0 };
  int32_t color_from = 0, color_to = 0;
  if (as_color) {
    /* a colour has no float keyframes: a 0 -> 1 animator carries the progress */
    if (has_from && !dx_color(vm, x, t, "valueFrom", &color_from)) {
      dvm__throw(vm, "android/view/InflateException", "<objectAnimator> valueFrom is not a colour");
      return false;
    }
    if (!dx_color(vm, x, t, "valueTo", &color_to)) {
      dvm__throw(vm, "android/view/InflateException", "<objectAnimator> valueTo is not a colour");
      return false;
    }
    values.l = dvm_new_array(vm, 'F', "F", 2);
    float *v = values.l ? dvm_array_data(vm, values.l) : NULL;
    if (!v) return false;
    v[0] = 0.0f;
    v[1] = 1.0f;
  } else if (as_int) {
    values.l = dvm_new_array(vm, 'I', "I", n);
    int32_t *v = values.l ? dvm_array_data(vm, values.l) : NULL;
    if (!v) return false;
    if (has_from) v[0] = dx_int(x, t, "valueFrom", 0);
    v[n - 1] = dx_int(x, t, "valueTo", 0);
  } else {
    values.l = dvm_new_array(vm, 'F', "F", n);
    float *v = values.l ? dvm_array_data(vm, values.l) : NULL;
    if (!v) return false;
    if (has_from) v[0] = dx_float(x, t, "valueFrom", 0.0f);
    v[n - 1] = dx_float(x, t, "valueTo", 0.0f);
  }
  dvm_pin(vm, values.l);
  if (object && as_color) {
    union dvm_value a[3] = { { .l = 0 }, { .l = dvm_new_string(vm, prop) }, values };
    if (!oa_of_float_name(vm, 0, a, 3, &animator) || !animator.l) {
      dvm_unpin(vm, values.l);
      return false;
    }
    dvm_pin(vm, animator.l);
    /* no float Property: the colour listener drives the setter instead */
    (void)dvm_set_field(vm, animator.l, "property", "Landroid/util/Property;", (union dvm_value) {
      .l = 0
    });
    dvm_ref listener = dvm_new_object(vm, dvm__class_by_desc(vm, "Llunaria/ColorAnimatorListener;"));
    union dvm_value ign2 = { 0 };
    if (!listener) {
      dvm_unpin(vm, animator.l);
      dvm_unpin(vm, values.l);
      return false;
    }
    dvm_pin(vm, listener);
    (void)dvm_set_field(vm, listener, "property", "Ljava/lang/String;", a[1]);
    (void)dvm_set_field(vm, listener, "from", "I", (union dvm_value) {
      .i = color_from
    });
    (void)dvm_set_field(vm, listener, "to", "I", (union dvm_value) {
      .i = color_to
    });
    union dvm_value l = { .l = listener };
    const bool added = am_vcall(vm, animator.l, "addUpdateListener", "(Landroid/animation/ValueAnimator$AnimatorUpdateListener;)V", &l, 1, &ign2);
    dvm_unpin(vm, listener);
    dvm_unpin(vm, animator.l);
    if (!added) {
      dvm_unpin(vm, values.l);
      return false;
    }
  } else if (object) {
    union dvm_value a[3] = { { .l = 0 }, { .l = dvm_new_string(vm, prop) }, values };
    if (!as_int) {
      if (!oa_of_float_name(vm, 0, a, 3, &animator)) {
        dvm_unpin(vm, values.l);
        return false;
      }
    } else {
      dvm__throw(vm, "android/view/InflateException", "<objectAnimator valueType=intType> is not supported");
      dvm_unpin(vm, values.l);
      return false;
    }
  } else {
    struct dvm_class *vc = dvm_find_class(vm, "android/animation/ValueAnimator");
    struct dvm_method *m = vc ? dvm_find_method(vm, vc, as_int ? "ofInt" : "ofFloat",
                           as_int ? "([I)Landroid/animation/ValueAnimator;" : "([F)Landroid/animation/ValueAnimator;") : NULL;
    if (!m || !dvm_call(vm, m, 0, &values, 1, &animator)) {
      dvm_unpin(vm, values.l);
      return false;
    }
  }
  dvm_unpin(vm, values.l);
  if (!animator.l) return false;
  dvm_pin(vm, animator.l);
  union dvm_value ign = { 0 };
  int64_t duration = 300;
  if (dx_attr(x, t, "duration", &type, &data) && dx_resolve_attribute(vm, x, &type, &data)) duration = (int32_t)data;
  union dvm_value j = { .j = duration };
  bool ok = am_vcall(vm, animator.l, "setDuration", "(J)Landroid/animation/ValueAnimator;", &j, 1, &ign) ||
            am_vcall(vm, animator.l, "setDuration", "(J)Landroid/animation/ObjectAnimator;", &j, 1, &ign);
  if (!ok) dvm_clear_exception(vm);
  if (dx_attr(x, t, "startOffset", &type, &data)) {
    union dvm_value d = { .j = (int32_t)data };
    (void)am_vcall(vm, animator.l, "setStartDelay", "(J)V", &d, 1, &ign);
  }
  union dvm_value rc = { .i = dx_int(x, t, "repeatCount", 0) }, rm = { .i = dx_int(x, t, "repeatMode", 1) };
  (void)am_vcall(vm, animator.l, "setRepeatCount", "(I)V", &rc, 1, &ign);
  (void)am_vcall(vm, animator.l, "setRepeatMode", "(I)V", &rm, 1, &ign);
  if (dx_attr(x, t, "interpolator", &type, &data) && type == AX_REFERENCE && context) {
    struct dvm_class *au = dvm_find_class(vm, "android/view/animation/AnimationUtils");
    struct dvm_method *li = au ? dvm_find_method(vm, au, "loadInterpolator", "(Landroid/content/Context;I)Landroid/view/animation/Interpolator;") : NULL;
    union dvm_value a[2] = { { .l = context }, { .i = (int32_t)data } }, interp = { 0 };
    if (!li || !dvm_call(vm, li, 0, a, 2, &interp)) {
      dvm_unpin(vm, animator.l);
      return false;
    }
    union dvm_value ia = interp;
    (void)am_vcall(vm, animator.l, "setInterpolator", "(Landroid/animation/TimeInterpolator;)V", &ia, 1, &ign);
  }
  dvm_unpin(vm, animator.l);
  *result = animator.l;
  return true;
}

static bool inflater_node(struct dvm *vm, struct dx *x, const struct dx_tag *t, dvm_ref context,
                          unsigned depth, dvm_ref *result)
{
  if (depth > 8) {
    dvm__throw(vm, "android/view/InflateException", "animator resource nests too deeply");
    return false;
  }
  if (!strcmp(t->name, "objectAnimator") || !strcmp(t->name, "animator")) {
    const bool ok = inflater_value_animator(vm, x, t, context, t->name[0] == 'o', result);
    dx_skip(x);
    return ok;
  }
  if (strcmp(t->name, "set")) {
    dvm__throw(vm, "android/view/InflateException", "Unknown animator name: %s", t->name);
    return false;
  }
  const bool sequential = dx_int(x, t, "ordering", 0) == 1;
  struct dvm_class *sc = dvm_find_class(vm, "android/animation/AnimatorSet");
  dvm_ref set = sc ? dvm_new_object(vm, sc) : 0;
  if (!set) return false;
  dvm_pin(vm, set);
  dvm_ref kids[64];
  uint32_t nk = 0;
  bool ok = true;
  struct dx_tag child;
  for (int e; ok && (e = dx_next(x, &child)) != 0 && e != 0x0103;) {
    dvm_ref node = 0;
    if (nk == 64) {
      dvm__throw(vm, "android/view/InflateException", "too many animators in one <set>");
      ok = false;
      break;
    }
    ok = inflater_node(vm, x, &child, context, depth + 1, &node);
    if (ok && node) {
      kids[nk++] = node;
      dvm_pin(vm, node);
    }
  }
  if (ok) {
    dvm_ref arr = dvm_new_array(vm, 'L', "Landroid/animation/Animator;", nk);
    if (arr) {
      for (uint32_t i = 0; i < nk; ++i) ((dvm_ref *)dvm_array_data(vm, arr))[i] = kids[i];
      union dvm_value a = { .l = arr }, ign = { 0 };
      ok = am_vcall(vm, set, sequential ? "playSequentially" : "playTogether", "([Landroid/animation/Animator;)V", &a, 1, &ign);
    } else ok = false;
  }
  for (uint32_t i = 0; i < nk; ++i) dvm_unpin(vm, kids[i]);
  dvm_unpin(vm, set);
  if (ok) *result = set;
  return ok;
}

static bool x_inflater_load_animator(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const uint32_t resource = (uint32_t)ARG(1).i;
  int32_t iv = 0;
  const char *path = NULL;
  size_t n = 0;
  unsigned char *b = NULL;
  if (arm_exec_apk_resource_value(resource, &iv, &path) == 'L' && path) b = arm_exec_asset_read(path, &n);
  if (!b || n < 8 || ax_u16(b) != 3) {
    free(b);
    dvm__throw(vm, "android/content/res/Resources$NotFoundException", "Animator resource 0x%08x", resource);
    return false;
  }
  struct dx x = { .b = b, .n = n, .pos = ax_u16(b + 2) };
  x.s = ax_string_pool(b, n, &x.ns);
  struct dx_tag root;
  dvm_ref animator = 0;
  bool ok = x.s != NULL;
  if (ok && dx_next(&x, &root) != 0x0102) {
    dvm__throw(vm, "android/view/InflateException", "Empty animator resource 0x%08x", resource);
    ok = false;
  }
  if (ok) ok = inflater_node(vm, &x, &root, ARG(0).l, 0, &animator);
  for (uint32_t i = 0; x.s && i < x.ns; ++i) free(x.s[i]);
  free(x.s);
  free(b);
  if (!ok) return false;
  RETL(animator);
}
static const struct rt_method rt_gap_animator_inflater[] = {
  SM("loadAnimator", "(Landroid/content/Context;I)Landroid/animation/Animator;", x_inflater_load_animator), M_END,
};

/* AnimationUtils.loadInterpolator(Context, id): the <...Interpolator> element made into the
 * stock interpolator it names, with the attributes the constructor takes. */
static bool x_load_interpolator(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const uint32_t resource = (uint32_t)ARG(1).i;
  int32_t iv = 0;
  const char *path = NULL;
  size_t n = 0;
  unsigned char *b = NULL;
  if (arm_exec_apk_resource_value(resource, &iv, &path) == 'L' && path) b = arm_exec_asset_read(path, &n);
  if (!b || n < 8 || ax_u16(b) != 3) {
    free(b);
    dvm__throw(vm, "android/content/res/Resources$NotFoundException", "Interpolator resource 0x%08x", resource);
    return false;
  }
  struct dx x = { .b = b, .n = n, .pos = ax_u16(b + 2) };
  x.s = ax_string_pool(b, n, &x.ns);
  struct dx_tag t;
  bool ok = x.s && dx_next(&x, &t) == 0x0102;
  static const struct {
    const char *tag, *cls;
  } known[] = {
    { "accelerateDecelerateInterpolator", "android/view/animation/AccelerateDecelerateInterpolator" },
    { "accelerateInterpolator", "android/view/animation/AccelerateInterpolator" },
    { "decelerateInterpolator", "android/view/animation/DecelerateInterpolator" },
    { "linearInterpolator", "android/view/animation/LinearInterpolator" },
    { "anticipateInterpolator", "android/view/animation/AnticipateInterpolator" },
    { "overshootInterpolator", "android/view/animation/OvershootInterpolator" },
    { "anticipateOvershootInterpolator", "android/view/animation/AnticipateOvershootInterpolator" },
    { "bounceInterpolator", "android/view/animation/BounceInterpolator" },
    { "cycleInterpolator", "android/view/animation/CycleInterpolator" },
    { "pathInterpolator", "android/view/animation/PathInterpolator" },
  };
  const char *cls = NULL;
  for (size_t i = 0; ok && i < sizeof known / sizeof known[0]; ++i)
    if (!strcmp(t.name, known[i].tag)) cls = known[i].cls;
  dvm_ref obj = 0;
  if (ok && !cls) {
    dvm__throw(vm, "android/content/res/Resources$NotFoundException", "Unknown interpolator name: %s", ok ? t.name : "?");
    ok = false;
  }
  if (ok) {
    obj = dvm_new_object(vm, dvm_find_class(vm, cls));
    ok = obj != 0;
  }
  if (ok) {
    dvm_pin(vm, obj);
    union dvm_value ign = { 0 };
    uint8_t type;
    uint32_t data;
    if (!strcmp(t.name, "pathInterpolator")) {
      if (dx_attr(&x, &t, "controlX2", &type, &data)) {
        union dvm_value a[4] = { { .f = dx_float(&x, &t, "controlX1", 0) }, { .f = dx_float(&x, &t, "controlY1", 0) },
          { .f = dx_float(&x, &t, "controlX2", 0) }, { .f = dx_float(&x, &t, "controlY2", 0) }
        };
        ok = am_vcall(vm, obj, "<init>", "(FFFF)V", a, 4, &ign);
      } else {
        union dvm_value a[2] = { { .f = dx_float(&x, &t, "controlX1", 0) }, { .f = dx_float(&x, &t, "controlY1", 0) } };
        ok = am_vcall(vm, obj, "<init>", "(FF)V", a, 2, &ign);
      }
    } else if (dx_attr(&x, &t, "factor", &type, &data) || dx_attr(&x, &t, "tension", &type, &data) ||
               dx_attr(&x, &t, "cycles", &type, &data)) {
      union dvm_value a = { .f = dx_float(&x, &t, "factor", dx_float(&x, &t, "tension", dx_float(&x, &t, "cycles", 1.0f))) };
      ok = am_vcall(vm, obj, "<init>", "(F)V", &a, 1, &ign);
    } else {
      ok = am_vcall(vm, obj, "<init>", "()V", NULL, 0, &ign);
    }
    dvm_unpin(vm, obj);
  }
  for (uint32_t i = 0; x.s && i < x.ns; ++i) free(x.s[i]);
  free(x.s);
  free(b);
  if (!ok) return false;
  RETL(obj);
}
static const struct rt_method rt_gap_animation_utils[] = {
  SM("loadInterpolator", "(Landroid/content/Context;I)Landroid/view/animation/Interpolator;", x_load_interpolator), M_END,
};

/* TextView.getEditableText(): the text when it is Editable (an EditText's always is), else null. */
static bool x_text_editable(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value text = { 0 };
  if (!am_vcall(vm, self, "getText", "()Ljava/lang/CharSequence;", NULL, 0, &text)) return false;
  struct dvm_class *editable = dvm_find_class(vm, "android/text/Editable");
  if (text.l && editable && dvm__class_assignable(vm, dvm_object_class(vm, text.l), editable)) RETL(text.l);
  RETL(0);
}
static const struct rt_method rt_gap_text_editable[] = {
  M("getEditableText", "()Landroid/text/Editable;", x_text_editable), M_END,
};

/* ---- java.lang.Iterable.forEach, java.util.Collection.removeIf ------------------------- */
static bool x_iterable_default(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)nargs;
  union dvm_value it = { 0 }, ign = { 0 };
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "action");
    return false;
  }
  if (!am_vcall(vm, self, "iterator", "()Ljava/util/Iterator;", NULL, 0, &it)) return false;
  const bool remove_if = !strcmp(builtin_name(), "removeIf");
  bool removed = false;
  for (;;) {
    union dvm_value more = { 0 }, item = { 0 }, a = { 0 }, hit = { 0 };
    if (!am_vcall(vm, it.l, "hasNext", "()Z", NULL, 0, &more)) return false;
    if (!more.i) break;
    if (!am_vcall(vm, it.l, "next", "()Ljava/lang/Object;", NULL, 0, &item)) return false;
    a.l = item.l;
    if (remove_if) {
      if (!am_vcall(vm, ARG(0).l, "test", "(Ljava/lang/Object;)Z", &a, 1, &hit)) return false;
      if (hit.i) {
        if (!am_vcall(vm, it.l, "remove", "()V", NULL, 0, &ign)) return false;
        removed = true;
      }
    } else if (!am_vcall(vm, ARG(0).l, "accept", "(Ljava/lang/Object;)V", &a, 1, &ign)) return false;
  }
  if (remove_if) RETI(removed);
  RETV();
}
static const struct rt_method rt_gap_iterable[] = {
  M("forEach", "(Ljava/util/function/Consumer;)V", x_iterable_default), M_END,
};
static const struct rt_method rt_gap_collection[] = {
  M("removeIf", "(Ljava/util/function/Predicate;)Z", x_iterable_default), M_END,
};

/* ---- AtomicLong / AtomicInteger functional updates ------------------------------------- */
static bool x_atomic_update(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name(), *sig = builtin_sig();
  const bool wide = strstr(builtin_sig(), "Long") != NULL;
  const bool acc = strstr(n, "ccumulate") != NULL;
  const bool ret_new = n[0] == 'u' || n[0] == 'a';   /* updateAndGet / accumulateAndGet */
  union dvm_value prev = { 0 }, next = { 0 }, ign = { 0 };
  if (!am_vcall(vm, self, "get", wide ? "()J" : "()I", NULL, 0, &prev)) return false;
  if (acc) {
    union dvm_value a[2] = { prev, ARG(0) };
    if (wide) a[1].j = ARG(0).j;
    if (!am_vcall(vm, ARG(1).l, wide ? "applyAsLong" : "applyAsInt", wide ? "(JJ)J" : "(II)I", a, 2, &next)) return false;
  } else {
    if (!am_vcall(vm, ARG(0).l, wide ? "applyAsLong" : "applyAsInt", wide ? "(J)J" : "(I)I", &prev, 1, &next)) return false;
  }
  if (!am_vcall(vm, self, "set", wide ? "(J)V" : "(I)V", &next, 1, &ign)) return false;
  (void)sig;
  *out = ret_new ? next : prev;
  return true;
}
#define ATOMIC_ROWS(T, S, U, B) \
  M("updateAndGet", "(Ljava/util/function/" U "UnaryOperator;)" S, x_atomic_update), \
  M("getAndUpdate", "(Ljava/util/function/" U "UnaryOperator;)" S, x_atomic_update), \
  M("accumulateAndGet", "(" S "Ljava/util/function/" U "BinaryOperator;)" S, x_atomic_update), \
  M("getAndAccumulate", "(" S "Ljava/util/function/" U "BinaryOperator;)" S, x_atomic_update)
static const struct rt_method rt_gap_atomic_long[] = { ATOMIC_ROWS(Long, "J", "Long", 0), M_END };
static const struct rt_method rt_gap_atomic_int[] = { ATOMIC_ROWS(Int, "I", "Int", 0), M_END };
#undef ATOMIC_ROWS

/* ---- ConcurrentHashMap.keys() --------------------------------------------------------- */
static bool x_chm_keys(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value set = { 0 }, a = { 0 };
  if (!am_vcall(vm, self, "keySet", "()Ljava/util/Set;", NULL, 0, &set)) return false;
  struct dvm_class *cc = dvm_find_class(vm, "java/util/Collections");
  struct dvm_method *m = cc ? dvm_find_method(vm, cc, "enumeration", "(Ljava/util/Collection;)Ljava/util/Enumeration;") : NULL;
  a.l = set.l;
  if (!m) {
    dvm__throw(vm, "java/lang/NoSuchMethodError", "Collections.enumeration");
    return false;
  }
  return dvm_call(vm, m, 0, &a, 1, out);
}
static const struct rt_method rt_gap_chm[] = {
  M("keys", "()Ljava/util/Enumeration;", x_chm_keys), M_END,
};

/* ---- java.sql.Date(long) -------------------------------------------------------------- */
static bool x_sql_date(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  union dvm_value ign = { 0 };
  (void)nargs;
  (void)out;
  struct dvm_class *dc = dvm_find_class(vm, "java/util/Date");
  struct dvm_method *init = dc ? dvm_find_method(vm, dc, "<init>", "(J)V") : NULL;
  if (!init) return false;
  return dvm_call(vm, init, self, (union dvm_value *)args, 1, &ign);
}
static const struct rt_method rt_sql_date[] = { M("<init>", "(J)V", x_sql_date), M_END };

/* ---- android.view.inputmethod.InputConnectionWrapper -------------------------------------- */
/* Every InputConnection call is forwarded to the wrapped connection; with none, the call
 * answers false / null / 0 as the platform's wrapper does when its target is null. */
static const struct rt_field rt_icw_fields[] = {
  { "target", "Landroid/view/inputmethod/InputConnection;" }, { "mutable", "Z" }, F_END,
};
static const char *const rt_iface_icw[] = { "Landroid/view/inputmethod/InputConnection;", NULL };
static bool x_icw(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                  union dvm_value *out)
{
  const char *name = builtin_name(), *sig = builtin_sig();
  if (!strcmp(name, "<init>")) {
    (void)dvm_set_field(vm, self, "target", "Landroid/view/inputmethod/InputConnection;", ARG(0));
    (void)dvm_set_field(vm, self, "mutable", "Z", ARG(1));
    RETV();
  }
  union dvm_value target = { 0 };
  (void)dvm_get_field(vm, self, "target", "Landroid/view/inputmethod/InputConnection;", &target);
  if (!strcmp(name, "setTarget")) {
    union dvm_value m = { 0 };
    (void)dvm_get_field(vm, self, "mutable", "Z", &m);
    if (target.l && !m.i) {
      dvm__throw(vm, "java/lang/SecurityException", "not mutable");
      return false;
    }
    (void)dvm_set_field(vm, self, "target", "Landroid/view/inputmethod/InputConnection;", ARG(0));
    RETV();
  }
  memset(out, 0, sizeof *out);
  if (!target.l) return true;
  return am_vcall(vm, target.l, name, sig, args, nargs, out);
}
#define ICW(n, s) M(n, s, x_icw)
static const struct rt_method rt_input_connection_wrapper[] = {
  M("<init>", "(Landroid/view/inputmethod/InputConnection;Z)V", x_icw),
  ICW("setTarget", "(Landroid/view/inputmethod/InputConnection;)V"),
  ICW("getTextBeforeCursor", "(II)Ljava/lang/CharSequence;"),
  ICW("getTextAfterCursor", "(II)Ljava/lang/CharSequence;"),
  ICW("getSelectedText", "(I)Ljava/lang/CharSequence;"),
  ICW("getCursorCapsMode", "(I)I"),
  ICW("getExtractedText", "(Landroid/view/inputmethod/ExtractedTextRequest;I)Landroid/view/inputmethod/ExtractedText;"),
  ICW("deleteSurroundingText", "(II)Z"),
  ICW("deleteSurroundingTextInCodePoints", "(II)Z"),
  ICW("setComposingText", "(Ljava/lang/CharSequence;I)Z"),
  ICW("setComposingRegion", "(II)Z"),
  ICW("finishComposingText", "()Z"),
  ICW("commitText", "(Ljava/lang/CharSequence;I)Z"),
  ICW("commitCompletion", "(Landroid/view/inputmethod/CompletionInfo;)Z"),
  ICW("commitCorrection", "(Landroid/view/inputmethod/CorrectionInfo;)Z"),
  ICW("setSelection", "(II)Z"),
  ICW("performEditorAction", "(I)Z"),
  ICW("performContextMenuAction", "(I)Z"),
  ICW("beginBatchEdit", "()Z"),
  ICW("endBatchEdit", "()Z"),
  ICW("sendKeyEvent", "(Landroid/view/KeyEvent;)Z"),
  ICW("clearMetaKeyStates", "(I)Z"),
  ICW("reportFullscreenMode", "(Z)Z"),
  ICW("performPrivateCommand", "(Ljava/lang/String;Landroid/os/Bundle;)Z"),
  ICW("requestCursorUpdates", "(I)Z"),
  ICW("getHandler", "()Landroid/os/Handler;"),
  ICW("closeConnection", "()V"),
  M_END,
};
#undef ICW

/* ---- TextView.onCreateInputConnection and the connection it makes ---------------------------- */
/* Only a field that can be edited has an input connection; EditorInfo says what it is. */
static bool x_text_create_ic(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)nargs;
  if (!ui_is_a(vm, self, "android/widget/EditText")) RETL(0);
  dvm_ref info = ARG(0).l;
  if (info) {
    union dvm_value sel = { 0 };
    int32_t type = ui_int(vm, self, "inputType");
    if (!type) type = 1;   /* TYPE_CLASS_TEXT */
    rt_set_int(vm, info, "inputType", type);
    rt_set_int(vm, info, "imeOptions", ui_int(vm, self, "imeOptions"));
    (void)sel;
    rt_set_int(vm, info, "initialSelStart", ui_int(vm, self, "selectionStart"));
    rt_set_int(vm, info, "initialSelEnd", ui_int(vm, self, "selectionEnd"));
  }
  dvm_ref ic = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/view/inputmethod/BaseInputConnection;"));
  if (!ic) return false;
  dvm_pin(vm, ic);
  union dvm_value a[2] = { { .l = self }, { .i = 1 } }, ign = { 0 };
  const bool ok = am_vcall(vm, ic, "<init>", "(Landroid/view/View;Z)V", a, 2, &ign);
  dvm_unpin(vm, ic);
  if (!ok) return false;
  RETL(ic);
}
static const struct rt_method rt_gap_text_ic[] = {
  M("onCreateInputConnection", "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;", x_text_create_ic),
  M_END,
};

/* ---- android.view.inputmethod.CompletionInfo / ExtractedTextRequest / ExtractedText ------------- */
static const struct rt_field rt_completion_fields[] = {
  { "id", "J" }, { "position", "I" }, { "text", "Ljava/lang/CharSequence;" }, { "label", "Ljava/lang/CharSequence;" }, F_END,
};
static bool x_completion(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    (void)dvm_set_field(vm, self, "id", "J", ARG(0));
    (void)dvm_set_field(vm, self, "position", "I", ARG(1));
    (void)dvm_set_field(vm, self, "text", "Ljava/lang/CharSequence;", ARG(2));
    if (nargs > 3) (void)dvm_set_field(vm, self, "label", "Ljava/lang/CharSequence;", ARG(3));
    RETV();
  }
  if (!strcmp(n, "getId")) return dvm_get_field(vm, self, "id", "J", out);
  if (!strcmp(n, "getPosition")) return dvm_get_field(vm, self, "position", "I", out);
  if (!strcmp(n, "getText")) return dvm_get_field(vm, self, "text", "Ljava/lang/CharSequence;", out);
  return dvm_get_field(vm, self, "label", "Ljava/lang/CharSequence;", out);   /* getLabel */
}
static const struct rt_method rt_completion_info[] = {
  M("<init>", "(JILjava/lang/CharSequence;)V", x_completion),
  M("<init>", "(JILjava/lang/CharSequence;Ljava/lang/CharSequence;)V", x_completion),
  M("getId", "()J", x_completion), M("getPosition", "()I", x_completion),
  M("getText", "()Ljava/lang/CharSequence;", x_completion), M("getLabel", "()Ljava/lang/CharSequence;", x_completion), M_END,
};
static const struct rt_field rt_extracted_request_fields[] = {
  { "token", "I" }, { "flags", "I" }, { "hintMaxLines", "I" }, { "hintMaxChars", "I" }, F_END,
};
static bool x_extracted_request_init(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs; (void)out;
  RETV();
}
static const struct rt_method rt_extracted_text_request[] = {
  M("<init>", "()V", x_extracted_request_init), M_END,
};

/* ======================= stub batch 3 ======================================================= */

/* ---- ContentValues(int), DatabaseUtils.cursor*ToContentValuesIfPresent ------------------------- */
static bool x_cv_sized_init(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  if (ARG(0).i < 0) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "capacity must be >= 0");
    return false;
  }
  return map_init(vm, self, args, 0, out);
}
static const struct rt_method rt_gap_content_values[] = { M("<init>", "(I)V", x_cv_sized_init), M_END };

/* copies one column into the values when the column exists and is not NULL */
static bool x_db_copy_column(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)self;
  (void)nargs;
  (void)out;
  const char *n = builtin_name();
  dvm_ref cursor = ARG(0).l, values = ARG(1).l;
  union dvm_value col = { .l = ARG(2).l }, idx = { 0 }, isnull = { 0 }, v = { 0 }, ign = { 0 };
  if (!cursor || !values || !col.l) {
    dvm__throw(vm, "java/lang/NullPointerException", "cursor, values or column");
    return false;
  }
  if (!am_vcall(vm, cursor, "getColumnIndex", "(Ljava/lang/String;)I", &col, 1, &idx)) return false;
  if (idx.i < 0) RETV();
  if (!am_vcall(vm, cursor, "isNull", "(I)Z", &idx, 1, &isnull)) return false;
  if (isnull.i) RETV();
  union dvm_value put[2] = { col, { 0 } };
  if (!strcmp(n, "cursorIntToContentValuesIfPresent")) {
    if (!am_vcall(vm, cursor, "getInt", "(I)I", &idx, 1, &v)) return false;
    put[1].l = box_make(vm, "Ljava/lang/Integer;", v);
    return am_vcall(vm, values, "put", "(Ljava/lang/String;Ljava/lang/Integer;)V", put, 2, &ign);
  }
  if (!strcmp(n, "cursorLongToContentValuesIfPresent")) {
    if (!am_vcall(vm, cursor, "getLong", "(I)J", &idx, 1, &v)) return false;
    put[1].l = box_make(vm, "Ljava/lang/Long;", v);
    return am_vcall(vm, values, "put", "(Ljava/lang/String;Ljava/lang/Long;)V", put, 2, &ign);
  }
  if (!am_vcall(vm, cursor, "getString", "(I)Ljava/lang/String;", &idx, 1, &v)) return false;
  put[1] = v;
  return am_vcall(vm, values, "put", "(Ljava/lang/String;Ljava/lang/String;)V", put, 2, &ign);
}
static const struct rt_method rt_gap_database_utils[] = {
  SM("cursorIntToContentValuesIfPresent", "(Landroid/database/Cursor;Landroid/content/ContentValues;Ljava/lang/String;)V", x_db_copy_column),
  SM("cursorLongToContentValuesIfPresent", "(Landroid/database/Cursor;Landroid/content/ContentValues;Ljava/lang/String;)V", x_db_copy_column),
  SM("cursorStringToContentValuesIfPresent", "(Landroid/database/Cursor;Landroid/content/ContentValues;Ljava/lang/String;)V", x_db_copy_column),
  M_END,
};

/* ---- AlarmManager.set(type, triggerAtMillis, windowMillis, intervalMillis, operation, workSource) --- */
static bool x_alarm_hidden_set(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)nargs;
  (void)out;
  union dvm_value ign = { 0 };
  /* A repeating alarm when an interval is given, else a window around the trigger time. */
  if (ARG(3).j > 0) {
    union dvm_value a[4] = { ARG(0), ARG(1), ARG(3), ARG(4) };
    a[1].j = ARG(1).j;
    a[2].j = ARG(3).j;
    return am_vcall(vm, self, "setRepeating", "(IJJLandroid/app/PendingIntent;)V", a, 4, &ign);
  }
  union dvm_value a[4] = { ARG(0), ARG(1), ARG(2), ARG(4) };
  a[1].j = ARG(1).j;
  a[2].j = ARG(2).j;
  return am_vcall(vm, self, "setWindow", "(IJJLandroid/app/PendingIntent;)V", a, 4, &ign);
}
static const struct rt_method rt_gap_alarm[] = {
  M("set", "(IJJJLandroid/app/PendingIntent;Landroid/os/WorkSource;)V", x_alarm_hidden_set), M_END,
};

/* ---- ShortcutManager.pushDynamicShortcut ------------------------------------------------------- */
static bool x_shortcut_push(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  (void)out;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "shortcut");
    return false;
  }
  dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
  if (!list) return false;
  dvm_pin(vm, list);
  union dvm_value item = { .l = ARG(0).l }, ign = { 0 }, l = { .l = list };
  bool ok = list_add(vm, list, &item, 1, &ign) && am_vcall(vm, self, "addDynamicShortcuts", "(Ljava/util/List;)Z", &l, 1, &ign);
  dvm_unpin(vm, list);
  return ok;
}
static const struct rt_method rt_gap_shortcut[] = {
  M("pushDynamicShortcut", "(Landroid/content/pm/ShortcutInfo;)V", x_shortcut_push), M_END,
};

/* ---- IntentFilter data scheme-specific parts --------------------------------------------------- */
static bool x_if_ssp(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                     union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  union dvm_value parts = { 0 }, kinds = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "dataSchemeSpecificParts", "Ljava/util/List;", &parts);
  (void)dvm_get_field(vm, self, "dataSchemeSpecificPartTypes", "Ljava/util/List;", &kinds);
  if (!strcmp(n, "addDataSchemeSpecificPart")) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "ssp");
      return false;
    }
    if (!parts.l) {
      parts.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      kinds.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      (void)dvm_set_field(vm, self, "dataSchemeSpecificParts", "Ljava/util/List;", parts);
      (void)dvm_set_field(vm, self, "dataSchemeSpecificPartTypes", "Ljava/util/List;", kinds);
    }
    union dvm_value p = { .l = ARG(0).l }, k = { .l = box_make(vm, "Ljava/lang/Integer;", (union dvm_value) {
      .i = ARG(1).i
    }) };
    return list_add(vm, parts.l, &p, 1, &ign) && list_add(vm, kinds.l, &k, 1, &ign);
  }
  struct rt_list *pl = parts.l ? list_of(vm, parts.l) : NULL, *kl = kinds.l ? list_of(vm, kinds.l) : NULL;
  if (!strcmp(n, "countDataSchemeSpecificParts")) RETI(pl ? pl->size : 0);
  /* hasDataSchemeSpecificPart(ssp): a literal, a prefix or a simple glob (type 0, 1, 2) matches */
  const char *want = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  for (uint32_t i = 0; want && pl && kl && i < pl->size; ++i) {
    const char *pat = dvm_string_utf8(vm, pl->items[i]);
    const int type = box_get(vm, kl->items[i]).i;
    if (!pat) continue;
    if (type == 0 && !strcmp(pat, want)) RETI(1);
    if (type == 1 && !strncmp(pat, want, strlen(pat))) RETI(1);
    if (type >= 2 && luna_fnmatch(pat, want, 0) == 0) RETI(1);
  }
  RETI(0);
}
static const struct rt_method rt_gap_intent_filter[] = {
  M("addDataSchemeSpecificPart", "(Ljava/lang/String;I)V", x_if_ssp),
  M("countDataSchemeSpecificParts", "()I", x_if_ssp),
  M("hasDataSchemeSpecificPart", "(Ljava/lang/String;)Z", x_if_ssp), M_END,
};

/* ---- android.text.TextUtils.SimpleStringSplitter ------------------------------------------------- */
static const struct rt_field rt_splitter_fields[] = {
  { "string", "Ljava/lang/String;" }, { "delim", "Ljava/lang/String;" }, { "pos", "I" }, F_END,
};
static const char *const rt_iface_splitter[] = {
  "Landroid/text/TextUtils$StringSplitter;", "Ljava/lang/Iterable;", "Ljava/util/Iterator;", NULL,
};
static bool x_splitter(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    char d[5] = { 0 };
    const uint32_t c = (uint32_t)ARG(0).i & 0xffff;
    if (c < 0x80) d[0] = (char)c;
    else if (c < 0x800) {
      d[0] = (char)(0xc0 | (c >> 6));
      d[1] = (char)(0x80 | (c & 0x3f));
    } else {
      d[0] = (char)(0xe0 | (c >> 12));
      d[1] = (char)(0x80 | ((c >> 6) & 0x3f));
      d[2] = (char)(0x80 | (c & 0x3f));
    }
    (void)dvm_set_field(vm, self, "delim", "Ljava/lang/String;", (union dvm_value) {
      .l = dvm_new_string(vm, d)
    });
    RETV();
  }
  if (!strcmp(n, "setString")) {
    (void)dvm_set_field(vm, self, "string", "Ljava/lang/String;", ARG(0));
    rt_set_int(vm, self, "pos", 0);
    RETV();
  }
  if (!strcmp(n, "iterator")) RETL(self);
  if (!strcmp(n, "remove")) {
    dvm__throw(vm, "java/lang/UnsupportedOperationException", "remove");
    return false;
  }
  union dvm_value str = { 0 }, delim = { 0 };
  (void)dvm_get_field(vm, self, "string", "Ljava/lang/String;", &str);
  (void)dvm_get_field(vm, self, "delim", "Ljava/lang/String;", &delim);
  const char *s = str.l ? dvm_string_utf8(vm, str.l) : "";
  const char *d = delim.l ? dvm_string_utf8(vm, delim.l) : ",";
  const size_t len = strlen(s), pos = (size_t)rt_get_int(vm, self, "pos");
  if (!strcmp(n, "hasNext")) RETI(pos < len);
  if (pos >= len) {
    dvm__throw(vm, "java/util/NoSuchElementException", "next");
    return false;
  }
  const char *hit = strstr(s + pos, d);
  const size_t end = hit ? (size_t)(hit - s) : len;
  rt_set_int(vm, self, "pos", (int32_t)(hit ? end + strlen(d) : len));
  RETL(dvm_new_string_n(vm, s + pos, end - pos));
}
static const struct rt_method rt_simple_string_splitter[] = {
  M("<init>", "(C)V", x_splitter), M("setString", "(Ljava/lang/String;)V", x_splitter),
  M("iterator", "()Ljava/util/Iterator;", x_splitter), M("hasNext", "()Z", x_splitter),
  M("next", "()Ljava/lang/Object;", x_splitter), M("next", "()Ljava/lang/String;", x_splitter),
  M("remove", "()V", x_splitter), M_END,
};

/* ---- constructors of stateless framework objects ------------------------------------------------- */
static bool x_empty_ctor(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs; (void)out;
  RETV();
}
static const struct rt_method rt_gap_empty_ctor[] = { M("<init>", "()V", x_empty_ctor), M_END };

/* ---- View: getLocalVisibleRect, content-receiving listener, translation callback ------------------ */
static bool x_view_local_visible(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "r");
    return false;
  }
  dvm_ref pt = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Point"));
  union dvm_value ign = { 0 }, a[2] = { { .l = ARG(0).l }, { .l = pt } }, visible = { 0 };
  if (!pt) return false;
  dvm_pin(vm, pt);
  bool ok = am_vcall(vm, pt, "<init>", "()V", NULL, 0, &ign) &&
            am_vcall(vm, self, "getGlobalVisibleRect", "(Landroid/graphics/Rect;Landroid/graphics/Point;)Z", a, 2, &visible);
  union dvm_value px = { 0 }, py = { 0 };
  if (ok) {
    (void)dvm_get_field(vm, pt, "x", "I", &px);
    (void)dvm_get_field(vm, pt, "y", "I", &py);
    union dvm_value o[2] = { { .i = -px.i }, { .i = -py.i } };
    ok = am_vcall(vm, ARG(0).l, "offset", "(II)V", o, 2, &ign);
  }
  dvm_unpin(vm, pt);
  if (!ok) return false;
  RETI(visible.i);
}
static bool x_view_receive_content(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  (void)nargs;
  (void)out;
  const char *n = builtin_name();
  if (!strcmp(n, "clearViewTranslationCallback")) RETV();   /* none was ever set */
  (void)dvm_set_field(vm, self, "receiveContentMimeTypes", "[Ljava/lang/String;", ARG(0));
  (void)dvm_set_field(vm, self, "receiveContentListener", "Ljava/lang/Object;", ARG(1));
  RETV();
}
static const struct rt_method rt_gap_view2[] = {
  M("getLocalVisibleRect", "(Landroid/graphics/Rect;)Z", x_view_local_visible),
  M("setOnReceiveContentListener", "([Ljava/lang/String;Landroid/view/OnReceiveContentListener;)V", x_view_receive_content),
  M("clearViewTranslationCallback", "()V", x_view_receive_content), M_END,
};

/* ---- TextView: setLineSpacing, setImeActionLabel ---------------------------------------------------- */
static bool x_text_spacing(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "setLineSpacing")) {
    (void)dvm_set_field(vm, self, "lineSpacingExtra", "F", ARG(0));
    (void)dvm_set_field(vm, self, "lineSpacingMult", "F", ARG(1));
    rt_set_int(vm, self, "lineSpacingMultSet", 1);
    ui_view_layout_changed(vm, self);
    ui_invalidate_view(vm, self, NULL, 0);
    RETV();
  }
  if (!strcmp(n, "getLineSpacingExtra")) return dvm_get_field(vm, self, "lineSpacingExtra", "F", out);
  if (!rt_get_int(vm, self, "lineSpacingMultSet")) RETF(1.0f);
  return dvm_get_field(vm, self, "lineSpacingMult", "F", out);   /* getLineSpacingMultiplier */
}
static bool x_text_ime_label(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "setImeActionLabel")) {
    (void)dvm_set_field(vm, self, "imeActionLabel", "Ljava/lang/CharSequence;", ARG(0));
    rt_set_int(vm, self, "imeActionId", ARG(1).i);
    RETV();
  }
  if (!strcmp(n, "getImeActionLabel")) return dvm_get_field(vm, self, "imeActionLabel", "Ljava/lang/CharSequence;", out);
  RETI(rt_get_int(vm, self, "imeActionId"));
}
static const struct rt_method rt_gap_text_view2[] = {
  M("setLineSpacing", "(FF)V", x_text_spacing), M("getLineSpacingExtra", "()F", x_text_spacing),
  M("getLineSpacingMultiplier", "()F", x_text_spacing),
  M("setImeActionLabel", "(Ljava/lang/CharSequence;I)V", x_text_ime_label),
  M("getImeActionLabel", "()Ljava/lang/CharSequence;", x_text_ime_label),
  M("getImeActionId", "()I", x_text_ime_label), M_END,
};

/* ---- XmlResourceParser: getClassAttribute, getAttributeBooleanValue(ns, name, default) ---------------- */
static bool x_xml_more(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
  (void)nargs;
  union dvm_value value = { 0 }, a[2] = { { .l = 0 }, { 0 } };
  if (!strcmp(builtin_name(), "getClassAttribute")) {
    a[1].l = dvm_new_string(vm, "class");
    return am_vcall(vm, self, "getAttributeValue", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", a, 2, out);
  }
  a[0] = ARG(0);
  a[1] = ARG(1);
  if (!am_vcall(vm, self, "getAttributeValue", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", a, 2, &value)) return false;
  const char *v = value.l ? dvm_string_utf8(vm, value.l) : NULL;
  if (!v) RETI(ARG(2).i);
  RETI(!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "-1") || !strcasecmp(v, "0xffffffff"));
}
static const struct rt_method rt_gap_xml[] = {
  M("getClassAttribute", "()Ljava/lang/String;", x_xml_more),
  M("getAttributeBooleanValue", "(Ljava/lang/String;Ljava/lang/String;Z)Z", x_xml_more), M_END,
};

/* ---- android.system.Os.statvfs / StructStatVfs ---------------------------------------------------------- */
static const struct rt_field rt_statvfs_fields[] = {
  { "f_bsize", "J" }, { "f_frsize", "J" }, { "f_blocks", "J" }, { "f_bfree", "J" }, { "f_bavail", "J" },
  { "f_files", "J" }, { "f_ffree", "J" }, { "f_favail", "J" }, { "f_fsid", "J" }, { "f_flag", "J" },
  { "f_namemax", "J" }, F_END,
};
static bool x_os_statvfs(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const char *path = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  if (!path) {
    dvm__throw(vm, "java/lang/NullPointerException", "path");
    return false;
  }
  char mapped[PATH_MAX];
  const char *host = arm_exec_map_guest_path(path, mapped, sizeof mapped);
#ifdef _WIN32
  /* The volume's free space as the Windows API reports it, in 4 KiB blocks. */
  ULARGE_INTEGER avail, total, freeb;
  struct {
    uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
  } sv = { 4096, 4096, 0, 0, 0, 0, 0, 0, 0, 0, 255 };
  if (!host || !GetDiskFreeSpaceExA(host, &avail, &total, &freeb)) {
    dvm__throw(vm, "android/system/ErrnoException", "statvfs failed: %s", strerror(ENOENT));
    return false;
  }
  sv.f_blocks = total.QuadPart / 4096;
  sv.f_bfree = freeb.QuadPart / 4096;
  sv.f_bavail = avail.QuadPart / 4096;
#else
  struct statvfs sv;
  if (!host || statvfs(host, &sv) != 0) {
    dvm__throw(vm, "android/system/ErrnoException", "statvfs failed: %s", strerror(host ? errno : ENOENT));
    return false;
  }
#endif
  dvm_ref o = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/system/StructStatVfs;"));
  if (!o) return false;
  const struct {
    const char *f;
    uint64_t v;
  } rows[] = {
    { "f_bsize", sv.f_bsize }, { "f_frsize", sv.f_frsize }, { "f_blocks", sv.f_blocks }, { "f_bfree", sv.f_bfree },
    { "f_bavail", sv.f_bavail }, { "f_files", sv.f_files }, { "f_ffree", sv.f_ffree }, { "f_favail", sv.f_favail },
    { "f_fsid", sv.f_fsid }, { "f_flag", sv.f_flag }, { "f_namemax", sv.f_namemax },
  };
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i)
    (void)dvm_set_field(vm, o, rows[i].f, "J", (union dvm_value) {
    .j = (int64_t)rows[i].v
  });
  RETL(o);
}
static const struct rt_method rt_gap_os2[] = {
  SM("statvfs", "(Ljava/lang/String;)Landroid/system/StructStatVfs;", x_os_statvfs), M_END,
};

/* ---- NotificationChannelGroup and the manager's group calls ---------------------------------------------- */
static const struct rt_field rt_channel_group_fields[] = {
  { "id", "Ljava/lang/String;" }, { "name", "Ljava/lang/CharSequence;" }, { "description", "Ljava/lang/String;" },
  { "blocked", "Z" }, { "channels", "Ljava/util/List;" }, F_END,
};
static bool x_channel_group(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "id must not be null");
      return false;
    }
    (void)dvm_set_field(vm, self, "id", "Ljava/lang/String;", ARG(0));
    (void)dvm_set_field(vm, self, "name", "Ljava/lang/CharSequence;", ARG(1));
    RETV();
  }
  if (!strcmp(n, "getId")) return dvm_get_field(vm, self, "id", "Ljava/lang/String;", out);
  if (!strcmp(n, "getName")) return dvm_get_field(vm, self, "name", "Ljava/lang/CharSequence;", out);
  if (!strcmp(n, "getDescription")) return dvm_get_field(vm, self, "description", "Ljava/lang/String;", out);
  if (!strcmp(n, "setDescription")) {
    (void)dvm_set_field(vm, self, "description", "Ljava/lang/String;", ARG(0));
    RETV();
  }
  if (!strcmp(n, "isBlocked")) return dvm_get_field(vm, self, "blocked", "Z", out);
  union dvm_value l = { 0 };
  (void)dvm_get_field(vm, self, "channels", "Ljava/util/List;", &l);
  if (!l.l) {
    l.l = coll_new_list(vm, "Ljava/util/ArrayList;");
    (void)dvm_set_field(vm, self, "channels", "Ljava/util/List;", l);
  }
  RETL(l.l);   /* getChannels */
}
static const struct rt_method rt_notification_channel_group[] = {
  M("<init>", "(Ljava/lang/String;Ljava/lang/CharSequence;)V", x_channel_group),
  M("getId", "()Ljava/lang/String;", x_channel_group), M("getName", "()Ljava/lang/CharSequence;", x_channel_group),
  M("getDescription", "()Ljava/lang/String;", x_channel_group), M("setDescription", "(Ljava/lang/String;)V", x_channel_group),
  M("isBlocked", "()Z", x_channel_group), M("getChannels", "()Ljava/util/List;", x_channel_group), M_END,
};
static bool x_notif_groups(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  union dvm_value list = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "channelGroups", "Ljava/util/List;", &list);
  if (!list.l) {
    list.l = coll_new_list(vm, "Ljava/util/ArrayList;");
    (void)dvm_set_field(vm, self, "channelGroups", "Ljava/util/List;", list);
  }
  struct rt_list *l = list_of(vm, list.l);
  if (!strcmp(n, "getNotificationChannelGroups")) {
    dvm_ref copy = coll_new_list(vm, "Ljava/util/ArrayList;");
    for (uint32_t i = 0; copy && l && i < l->size; ++i) {
      union dvm_value g = { .l = l->items[i] };
      (void)list_add(vm, copy, &g, 1, &ign);
    }
    RETL(copy);
  }
  /* find by id */
  const char *want = (!strcmp(n, "getNotificationChannelGroup") || !strcmp(n, "deleteNotificationChannelGroup")) && ARG(0).l
                     ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  if (want) {
    for (uint32_t i = 0; l && i < l->size; ++i) {
      union dvm_value id = { 0 };
      (void)dvm_get_field(vm, l->items[i], "id", "Ljava/lang/String;", &id);
      if (id.l && !strcmp(dvm_string_utf8(vm, id.l), want)) {
        dvm_ref g = l->items[i];
        if (!strcmp(n, "deleteNotificationChannelGroup")) {
          union dvm_value a = { .l = g };
          return am_vcall(vm, list.l, "remove", "(Ljava/lang/Object;)Z", &a, 1, &ign);
        }
        RETL(g);
      }
    }
    if (!strcmp(n, "getNotificationChannelGroup")) RETL(0);
    RETV();
  }
  /* create one or a list: a group with an id already known replaces nothing and is kept once */
  union dvm_value items[64];
  uint32_t count = 0;
  if (!strcmp(n, "createNotificationChannelGroup")) items[count++].l = ARG(0).l;
  else if (ARG(0).l) {
    union dvm_value arr = { 0 };
    if (!am_vcall(vm, ARG(0).l, "toArray", "()[Ljava/lang/Object;", NULL, 0, &arr)) return false;
    struct dvm_object *ao = arr.l ? dvm__obj(vm, arr.l) : NULL;
    for (uint32_t i = 0; ao && i < ao->length && count < 64; ++i) items[count++].l = ((dvm_ref *)ao->data)[i];
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!items[i].l) continue;
    union dvm_value id = { 0 };
    (void)dvm_get_field(vm, items[i].l, "id", "Ljava/lang/String;", &id);
    bool known = false;
    for (uint32_t k = 0; l && k < l->size && !known; ++k) {
      union dvm_value other = { 0 };
      (void)dvm_get_field(vm, l->items[k], "id", "Ljava/lang/String;", &other);
      known = id.l && other.l && !strcmp(dvm_string_utf8(vm, id.l), dvm_string_utf8(vm, other.l));
    }
    if (!known && !list_add(vm, list.l, &items[i], 1, &ign)) return false;
  }
  RETV();
}
static const struct rt_method rt_gap_notification2[] = {
  M("createNotificationChannelGroup", "(Landroid/app/NotificationChannelGroup;)V", x_notif_groups),
  M("createNotificationChannelGroups", "(Ljava/util/List;)V", x_notif_groups),
  M("getNotificationChannelGroup", "(Ljava/lang/String;)Landroid/app/NotificationChannelGroup;", x_notif_groups),
  M("deleteNotificationChannelGroup", "(Ljava/lang/String;)V", x_notif_groups),
  M("getNotificationChannelGroups", "()Ljava/util/List;", x_notif_groups), M_END,
};

/* ---- android.view.inputmethod.CorrectionInfo ---------------------------------------------------------------- */
static const struct rt_field rt_correction_fields[] = {
  { "offset", "I" }, { "oldText", "Ljava/lang/CharSequence;" }, { "newText", "Ljava/lang/CharSequence;" }, F_END,
};
static bool x_correction(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    (void)dvm_set_field(vm, self, "offset", "I", ARG(0));
    (void)dvm_set_field(vm, self, "oldText", "Ljava/lang/CharSequence;", ARG(1));
    (void)dvm_set_field(vm, self, "newText", "Ljava/lang/CharSequence;", ARG(2));
    RETV();
  }
  if (!strcmp(n, "getOffset")) return dvm_get_field(vm, self, "offset", "I", out);
  if (!strcmp(n, "getOldText")) return dvm_get_field(vm, self, "oldText", "Ljava/lang/CharSequence;", out);
  return dvm_get_field(vm, self, "newText", "Ljava/lang/CharSequence;", out);
}
static const struct rt_method rt_correction_info[] = {
  M("<init>", "(ILjava/lang/CharSequence;Ljava/lang/CharSequence;)V", x_correction),
  M("getOffset", "()I", x_correction), M("getOldText", "()Ljava/lang/CharSequence;", x_correction),
  M("getNewText", "()Ljava/lang/CharSequence;", x_correction), M_END,
};

/* ---- android.bluetooth.le.ScanFilter and its Builder -------------------------------------------------------- */
static const struct rt_field rt_scan_filter_fields[] = {
  { "serviceUuid", "Landroid/os/ParcelUuid;" }, { "serviceUuidMask", "Landroid/os/ParcelUuid;" },
  { "deviceName", "Ljava/lang/String;" }, { "deviceAddress", "Ljava/lang/String;" },
  { "manufacturerId", "I" }, { "manufacturerData", "[B" }, F_END,
};
static const char *const scan_filter_fields[] = { "serviceUuid", "serviceUuidMask", "deviceName", "deviceAddress", "manufacturerId", "manufacturerData" };
static const char *const scan_filter_types[] = { "Landroid/os/ParcelUuid;", "Landroid/os/ParcelUuid;", "Ljava/lang/String;", "Ljava/lang/String;", "I", "[B" };
static bool x_scan_builder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    rt_set_int(vm, self, "manufacturerId", -1);
    RETV();
  }
  if (!strcmp(n, "build")) {
    dvm_ref f = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/bluetooth/le/ScanFilter;"));
    if (!f) return false;
    for (int i = 0; i < 6; ++i) {
      union dvm_value v = { 0 };
      (void)dvm_get_field(vm, self, scan_filter_fields[i], scan_filter_types[i], &v);
      (void)dvm_set_field(vm, f, scan_filter_fields[i], scan_filter_types[i], v);
    }
    RETL(f);
  }
  if (!strcmp(n, "setServiceUuid")) {
    (void)dvm_set_field(vm, self, "serviceUuid", "Landroid/os/ParcelUuid;", ARG(0));
    if (nargs > 1) (void)dvm_set_field(vm, self, "serviceUuidMask", "Landroid/os/ParcelUuid;", ARG(1));
  } else if (!strcmp(n, "setDeviceName")) (void)dvm_set_field(vm, self, "deviceName", "Ljava/lang/String;", ARG(0));
  else if (!strcmp(n, "setDeviceAddress")) (void)dvm_set_field(vm, self, "deviceAddress", "Ljava/lang/String;", ARG(0));
  else if (!strcmp(n, "setManufacturerData")) {
    if (ARG(0).i < 0) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "invalid manufacture id");
      return false;
    }
    rt_set_int(vm, self, "manufacturerId", ARG(0).i);
    (void)dvm_set_field(vm, self, "manufacturerData", "[B", ARG(1));
  }
  RETL(self);
}
static bool x_scan_filter(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)args;
  (void)nargs;
  const char *n = builtin_name();
  for (int i = 0; i < 6; ++i) {
    char getter[40];
    snprintf(getter, sizeof getter, "get%c%s", toupper((unsigned char)scan_filter_fields[i][0]), scan_filter_fields[i] + 1);
    if (!strcmp(n, getter)) return dvm_get_field(vm, self, scan_filter_fields[i], scan_filter_types[i], out);
  }
  RETL(0);
}
static const struct rt_method rt_scan_filter_builder[] = {
  M("<init>", "()V", x_scan_builder),
  M("setServiceUuid", "(Landroid/os/ParcelUuid;)Landroid/bluetooth/le/ScanFilter$Builder;", x_scan_builder),
  M("setServiceUuid", "(Landroid/os/ParcelUuid;Landroid/os/ParcelUuid;)Landroid/bluetooth/le/ScanFilter$Builder;", x_scan_builder),
  M("setDeviceName", "(Ljava/lang/String;)Landroid/bluetooth/le/ScanFilter$Builder;", x_scan_builder),
  M("setDeviceAddress", "(Ljava/lang/String;)Landroid/bluetooth/le/ScanFilter$Builder;", x_scan_builder),
  M("setManufacturerData", "(I[B)Landroid/bluetooth/le/ScanFilter$Builder;", x_scan_builder),
  M("build", "()Landroid/bluetooth/le/ScanFilter;", x_scan_builder), M_END,
};
static const struct rt_method rt_scan_filter[] = {
  M("getServiceUuid", "()Landroid/os/ParcelUuid;", x_scan_filter), M("getServiceUuidMask", "()Landroid/os/ParcelUuid;", x_scan_filter),
  M("getDeviceName", "()Ljava/lang/String;", x_scan_filter), M("getDeviceAddress", "()Ljava/lang/String;", x_scan_filter),
  M("getManufacturerId", "()I", x_scan_filter), M("getManufacturerData", "()[B", x_scan_filter), M_END,
};

/* ---- android.view.animation.LayoutAnimationController ---------------------------------------------------------- */
static const struct rt_field rt_layout_anim_fields[] = {
  { "animation", "Landroid/view/animation/Animation;" }, { "delay", "F" }, { "order", "I" }, F_END,
};
static bool x_layout_anim(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    (void)dvm_set_field(vm, self, "animation", "Landroid/view/animation/Animation;", ARG(0));
    (void)dvm_set_field(vm, self, "delay", "F", nargs > 1 ? ARG(1) : (union dvm_value) {
      .f = 0.5f
    });
    RETV();
  }
  if (!strcmp(n, "getAnimation")) return dvm_get_field(vm, self, "animation", "Landroid/view/animation/Animation;", out);
  if (!strcmp(n, "setAnimation")) {
    (void)dvm_set_field(vm, self, "animation", "Landroid/view/animation/Animation;", ARG(0));
    RETV();
  }
  if (!strcmp(n, "getDelay")) return dvm_get_field(vm, self, "delay", "F", out);
  if (!strcmp(n, "setDelay")) {
    (void)dvm_set_field(vm, self, "delay", "F", ARG(0));
    RETV();
  }
  if (!strcmp(n, "getOrder")) RETI(rt_get_int(vm, self, "order"));
  rt_set_int(vm, self, "order", ARG(0).i);   /* setOrder */
  RETV();
}
static const struct rt_method rt_layout_animation_controller[] = {
  M("<init>", "(Landroid/view/animation/Animation;)V", x_layout_anim),
  M("<init>", "(Landroid/view/animation/Animation;F)V", x_layout_anim),
  M("getAnimation", "()Landroid/view/animation/Animation;", x_layout_anim),
  M("setAnimation", "(Landroid/view/animation/Animation;)V", x_layout_anim),
  M("getDelay", "()F", x_layout_anim), M("setDelay", "(F)V", x_layout_anim),
  M("getOrder", "()I", x_layout_anim), M("setOrder", "(I)V", x_layout_anim), M_END,
};
static bool x_group_layout_anim(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "getLayoutAnimation")) return dvm_get_field(vm, self, "layoutAnimation", "Landroid/view/animation/LayoutAnimationController;", out);
  (void)dvm_set_field(vm, self, "layoutAnimation", "Landroid/view/animation/LayoutAnimationController;", ARG(0));
  RETV();
}
static const struct rt_method rt_gap_group_layout_anim[] = {
  M("setLayoutAnimation", "(Landroid/view/animation/LayoutAnimationController;)V", x_group_layout_anim),
  M("getLayoutAnimation", "()Landroid/view/animation/LayoutAnimationController;", x_group_layout_anim), M_END,
};
/* AnimationUtils.loadLayoutAnimation(Context, id): <layoutAnimation animation delay animationOrder> */
static bool x_load_layout_animation(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const uint32_t resource = (uint32_t)ARG(1).i;
  int32_t iv = 0;
  const char *path = NULL;
  size_t n = 0;
  unsigned char *b = NULL;
  if (arm_exec_apk_resource_value(resource, &iv, &path) == 'L' && path) b = arm_exec_asset_read(path, &n);
  if (!b || n < 8 || ax_u16(b) != 3) {
    free(b);
    dvm__throw(vm, "android/content/res/Resources$NotFoundException", "Layout animation resource 0x%08x", resource);
    return false;
  }
  struct dx x = { .b = b, .n = n, .pos = ax_u16(b + 2) };
  x.s = ax_string_pool(b, n, &x.ns);
  struct dx_tag t;
  bool ok = x.s && dx_next(&x, &t) == 0x0102 && !strcmp(t.name, "layoutAnimation");
  dvm_ref controller = 0;
  if (!ok) dvm__throw(vm, "java/lang/RuntimeException", "Unknown layout animation name");
  if (ok) {
    uint8_t type = 0;
    uint32_t data = 0;
    union dvm_value anim = { 0 };
    if (dx_attr(&x, &t, "animation", &type, &data) && type == AX_REFERENCE) {
      struct dvm_class *au = dvm_find_class(vm, "android/view/animation/AnimationUtils");
      struct dvm_method *la = au ? dvm_find_method(vm, au, "loadAnimation", "(Landroid/content/Context;I)Landroid/view/animation/Animation;") : NULL;
      union dvm_value a[2] = { ARG(0), { .i = (int32_t)data } };
      ok = la && dvm_call(vm, la, 0, a, 2, &anim);
    }
    if (ok) {
      controller = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/view/animation/LayoutAnimationController;"));
      ok = controller != 0;
    }
    if (ok) {
      dvm_pin(vm, controller);
      (void)dvm_set_field(vm, controller, "animation", "Landroid/view/animation/Animation;", anim);
      (void)dvm_set_field(vm, controller, "delay", "F", (union dvm_value) {
        .f = dx_float(&x, &t, "delay", 0.5f)
      });
      rt_set_int(vm, controller, "order", dx_int(&x, &t, "animationOrder", 0));
      dvm_unpin(vm, controller);
    }
  }
  for (uint32_t i = 0; x.s && i < x.ns; ++i) free(x.s[i]);
  free(x.s);
  free(b);
  if (!ok) return false;
  RETL(controller);
}
static const struct rt_method rt_gap_animation_utils2[] = {
  SM("loadLayoutAnimation", "(Landroid/content/Context;I)Landroid/view/animation/LayoutAnimationController;", x_load_layout_animation), M_END,
};

/* ======================= stub batch 4 ======================================================= */

/* ---- java.util.concurrent.DelayQueue ------------------------------------------------------------ */
static const struct rt_field rt_delay_queue_fields[] = { { "items", "Ljava/util/List;" }, F_END };
static const char *const rt_iface_delay_queue[] = {
  "Ljava/util/concurrent/BlockingQueue;", "Ljava/util/Queue;", "Ljava/util/Collection;", "Ljava/lang/Iterable;", NULL,
};
static struct rt_list *dq_items(struct dvm *vm, dvm_ref self)
{
  union dvm_value l = { 0 };
  (void)dvm_get_field(vm, self, "items", "Ljava/util/List;", &l);
  if (!l.l) {
    l.l = coll_new_list(vm, "Ljava/util/ArrayList;");
    (void)dvm_set_field(vm, self, "items", "Ljava/util/List;", l);
  }
  return l.l ? list_of(vm, l.l) : NULL;
}
/* getDelay(NANOSECONDS) of a Delayed */
static bool dq_delay(struct dvm *vm, dvm_ref item, int64_t *ns)
{
  struct dvm_class *tu = dvm_find_class(vm, "java/util/concurrent/TimeUnit");
  union dvm_value unit = { 0 }, d = { 0 };
  if (!tu || !dvm_get_static(vm, tu, "NANOSECONDS", "Ljava/util/concurrent/TimeUnit;", &unit)) return false;
  if (!am_vcall(vm, item, "getDelay", "(Ljava/util/concurrent/TimeUnit;)J", &unit, 1, &d)) return false;
  *ns = d.j;
  return true;
}
static bool x_delay_queue(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    (void)dq_items(vm, self);
    RETV();
  }
  struct rt_list *l = dq_items(vm, self);
  if (!l) return false;
  if (!strcmp(n, "offer") || !strcmp(n, "add") || !strcmp(n, "put")) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "e");
      return false;
    }
    /* keep the queue ordered by compareTo(), the head being the earliest to expire */
    uint32_t at = l->size;
    for (uint32_t i = 0; i < l->size; ++i) {
      union dvm_value a = { .l = l->items[i] }, c = { 0 };
      if (!am_vcall(vm, ARG(0).l, "compareTo", "(Ljava/lang/Object;)I", &a, 1, &c)) return false;
      if (c.i < 0) {
        at = i;
        break;
      }
    }
    union dvm_value idx = { .i = (int32_t)at }, item = { .l = ARG(0).l }, ign = { 0 };
    union dvm_value ia[2] = { idx, item };
    struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/util/ArrayList;");
    (void)c;
    union dvm_value list = { 0 };
    (void)dvm_get_field(vm, self, "items", "Ljava/util/List;", &list);
    if (!am_vcall(vm, list.l, "add", "(ILjava/lang/Object;)V", ia, 2, &ign)) return false;
    if (!strcmp(n, "put")) RETV();
    RETI(1);
  }
  if (!strcmp(n, "size")) RETI(l->size);
  if (!strcmp(n, "isEmpty")) RETI(l->size == 0);
  union dvm_value list = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "items", "Ljava/util/List;", &list);
  if (!strcmp(n, "clear")) return am_vcall(vm, list.l, "clear", "()V", NULL, 0, &ign);
  if (!strcmp(n, "iterator")) return am_vcall(vm, list.l, "iterator", "()Ljava/util/Iterator;", NULL, 0, out);
  if (!strcmp(n, "remove")) {
    union dvm_value a = { .l = ARG(0).l };
    return am_vcall(vm, list.l, "remove", "(Ljava/lang/Object;)Z", &a, 1, out);
  }
  if (!strcmp(n, "peek")) RETL(l->size ? l->items[0] : 0);
  /* poll() / take() / poll(timeout, unit): the head only once its delay has run out */
  int64_t budget_ns = -1;   /* take(): wait for ever */
  const bool timed = !strcmp(n, "poll") && nargs == 2;
  if (timed) {
    union dvm_value ns = { 0 }, a = { .j = ARG(0).j };
    if (!am_vcall(vm, ARG(1).l, "toNanos", "(J)J", &a, 1, &ns)) return false;
    budget_ns = ns.j;
  }
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    if (timed) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if ((int64_t)(now.tv_sec - t0.tv_sec) * 1000000000LL + (now.tv_nsec - t0.tv_nsec) >= budget_ns) RETL(0);
    }
    l = dq_items(vm, self);
    if (!l || !l->size) {
      if (!strcmp(n, "poll") && !timed) RETL(0);
      /* take() / a timed poll() on an empty queue waits for an element */
      struct dvm_class *th = dvm_find_class(vm, "java/lang/Thread");
      struct dvm_method *sl = th ? dvm_find_method(vm, th, "sleep", "(J)V") : NULL;
      union dvm_value ms = { .j = 5 };
      if (!sl || !dvm_call(vm, sl, 0, &ms, 1, &ign)) return false;
      continue;
    }
    dvm_ref head = l->items[0];
    int64_t delay = 0;
    if (!dq_delay(vm, head, &delay)) return false;
    if (delay <= 0) {
      union dvm_value idx = { .i = 0 };
      return am_vcall(vm, list.l, "remove", "(I)Ljava/lang/Object;", &idx, 1, out);
    }
    if (!strcmp(n, "poll") && !timed) RETL(0);
    struct dvm_class *th = dvm_find_class(vm, "java/lang/Thread");
    struct dvm_method *sl = th ? dvm_find_method(vm, th, "sleep", "(J)V") : NULL;
    union dvm_value ms = { .j = delay / 1000000 > 50 ? 50 : delay / 1000000 + 1 };
    if (!sl || !dvm_call(vm, sl, 0, &ms, 1, &ign)) return false;
  }
}
static const struct rt_method rt_delay_queue[] = {
  M("<init>", "()V", x_delay_queue), M("offer", "(Ljava/util/concurrent/Delayed;)Z", x_delay_queue),
  M("add", "(Ljava/util/concurrent/Delayed;)Z", x_delay_queue), M("put", "(Ljava/util/concurrent/Delayed;)V", x_delay_queue),
  M("poll", "()Ljava/util/concurrent/Delayed;", x_delay_queue),
  M("poll", "(JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/Delayed;", x_delay_queue), M("take", "()Ljava/util/concurrent/Delayed;", x_delay_queue),
  M("peek", "()Ljava/util/concurrent/Delayed;", x_delay_queue), M("size", "()I", x_delay_queue),
  M("isEmpty", "()Z", x_delay_queue), M("clear", "()V", x_delay_queue),
  M("iterator", "()Ljava/util/Iterator;", x_delay_queue), M("remove", "(Ljava/lang/Object;)Z", x_delay_queue), M_END,
};

/* ---- LocalDateTime.parse ---------------------------------------------------------------------------- */
static bool x_ldt_parse(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
  (void)self;
  const char *text = ARG(0).l ? rt_charseq_utf8(vm, ARG(0).l) : NULL;
  if (!text) {
    dvm__throw(vm, "java/lang/NullPointerException", "text");
    return false;
  }
  union dvm_value pat = { 0 };
  if (nargs > 1 && ARG(1).l) (void)dvm_get_field(vm, ARG(1).l, "pattern", "Ljava/lang/String;", &pat);
  int64_t seconds = 0;
  int32_t nano = 0;
  if (!pat.l) {   /* ISO_LOCAL_DATE_TIME: 2011-12-03T10:15:30[.nnnnnnnnn] */
    int y, mo, d, h, mi, s = 0, used = 0;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d%n", &y, &mo, &d, &h, &mi, &used) < 5 || used == 0) goto bad;
    const char *p = text + used;
    if (*p == ':') {
      int u2 = 0;
      if (sscanf(p, ":%2d%n", &s, &u2) < 1) goto bad;
      p += u2;
      if (*p == '.') {
        ++p;
        int digits = 0;
        int32_t frac = 0;
        while (isdigit((unsigned char)*p) && digits < 9) {
          frac = frac * 10 + (*p++ - '0');
          ++digits;
        }
        while (digits++ < 9) frac *= 10;
        nano = frac;
      }
    }
    if (*p || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) goto bad;
    struct tm tmv = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s };
    seconds = (int64_t)timegm(&tmv);
  } else {
    /* a formatter pattern: parse it as a SimpleDateFormat pattern in UTC (java.time's u == y) */
    char fixed[160];
    size_t k = 0;
    for (const char *q = dvm_string_utf8(vm, pat.l); *q && k + 1 < sizeof fixed; ++q) fixed[k++] = *q == 'u' ? 'y' : *q;
    fixed[k] = 0;
    dvm_ref sdf = dvm_new_object(vm, dvm_find_class(vm, "java/text/SimpleDateFormat"));
    if (!sdf) return false;
    dvm_pin(vm, sdf);
    union dvm_value ign = { 0 }, p1 = { .l = dvm_new_string(vm, fixed) }, tzname = { .l = dvm_new_string(vm, "UTC") }, tz = { 0 };
    struct dvm_class *tzc = dvm_find_class(vm, "java/util/TimeZone");
    struct dvm_method *gtz = tzc ? dvm_find_method(vm, tzc, "getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;") : NULL;
    union dvm_value date = { 0 }, txt = { .l = ARG(0).l }, ms = { 0 };
    bool ok = am_vcall(vm, sdf, "<init>", "(Ljava/lang/String;)V", &p1, 1, &ign) && gtz && dvm_call(vm, gtz, 0, &tzname, 1, &tz) &&
              am_vcall(vm, sdf, "setTimeZone", "(Ljava/util/TimeZone;)V", &tz, 1, &ign) &&
              am_vcall(vm, sdf, "setLenient", "(Z)V", &(union dvm_value) { .i = 0 }, 1, &ign);
    if (ok) {
      if (!am_vcall(vm, sdf, "parse", "(Ljava/lang/String;)Ljava/util/Date;", &txt, 1, &date)) {
        dvm_clear_exception(vm);
        dvm_unpin(vm, sdf);
        goto bad;
      }
      ok = am_vcall(vm, date.l, "getTime", "()J", NULL, 0, &ms);
    }
    dvm_unpin(vm, sdf);
    if (!ok) return false;
    seconds = ms.j >= 0 ? ms.j / 1000 : -((-ms.j + 999) / 1000);
    nano = (int32_t)((ms.j - seconds * 1000) * 1000000);
  }
  RETL(jt_new_parts(vm, "Ljava/time/LocalDateTime;", seconds, nano, 0));
bad:
  dvm__throw(vm, "java/time/format/DateTimeParseException", "Text '%s' could not be parsed at index 0", text);
  return false;
}
static const struct rt_method rt_gap_ldt[] = {
  SM("parse", "(Ljava/lang/CharSequence;Ljava/time/format/DateTimeFormatter;)Ljava/time/LocalDateTime;", x_ldt_parse),
  SM("parse", "(Ljava/lang/CharSequence;)Ljava/time/LocalDateTime;", x_ldt_parse), M_END,
};

/* ---- Files.createDirectory / newDirectoryStream ------------------------------------------------------- */
static const struct rt_field rt_dir_stream_fields[] = { { "entries", "Ljava/util/List;" }, F_END };
static const char *const rt_iface_dir_stream[] = { "Ljava/nio/file/DirectoryStream;", "Ljava/lang/Iterable;", "Ljava/io/Closeable;", NULL };
static bool x_files_create_directory(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)self;
  (void)nargs;
  union dvm_value pathv = { 0 };
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "dir");
    return false;
  }
  (void)dvm_get_field(vm, ARG(0).l, "path", "Ljava/lang/String;", &pathv);
  const char *p = pathv.l ? dvm_string_utf8(vm, pathv.l) : NULL;
  const char *hp = pathv.l ? file_host_path(vm, pathv.l) : NULL;
  if (!p || !*p) {
    dvm__throw(vm, "java/nio/file/NoSuchFileException", "");
    return false;
  }
  if (luna_file_mkdir(hp ? hp : p, 0777) != 0) {
    const char *cls = errno == EEXIST ? "java/nio/file/FileAlreadyExistsException"
                      : errno == ENOENT ? "java/nio/file/NoSuchFileException"
                      : errno == EACCES ? "java/nio/file/AccessDeniedException" : "java/nio/file/FileSystemException";
    dvm__throw(vm, cls, "%s", p);
    return false;
  }
  RETL(ARG(0).l);
}
/* Lists the directory through java.io.File, the same mapped view the app's other file calls use. */
static bool x_new_directory_stream(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  (void)self;
  union dvm_value pathv = { 0 }, names = { 0 }, ign = { 0 };
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "dir");
    return false;
  }
  (void)dvm_get_field(vm, ARG(0).l, "path", "Ljava/lang/String;", &pathv);
  dvm_ref file = dvm_new_object(vm, dvm_find_class(vm, "java/io/File"));
  if (!file || !pathv.l) return false;
  dvm_pin(vm, file);
  union dvm_value a = { .l = pathv.l }, isdir = { 0 };
  bool ok = am_vcall(vm, file, "<init>", "(Ljava/lang/String;)V", &a, 1, &ign) &&
            am_vcall(vm, file, "isDirectory", "()Z", NULL, 0, &isdir);
  if (ok && !isdir.i) {
    dvm_unpin(vm, file);
    dvm__throw(vm, "java/nio/file/NotDirectoryException", "%s", dvm_string_utf8(vm, pathv.l));
    return false;
  }
  ok = ok && am_vcall(vm, file, "list", "()[Ljava/lang/String;", NULL, 0, &names);
  dvm_unpin(vm, file);
  if (!ok) return false;
  dvm_ref entries = coll_new_list(vm, "Ljava/util/ArrayList;");
  dvm_ref stream = dvm_new_object(vm, dvm__class_by_desc(vm, "Llunaria/DirectoryStream;"));
  if (!entries || !stream) return false;
  dvm_pin(vm, entries);
  dvm_pin(vm, stream);
  const char *dir = dvm_string_utf8(vm, pathv.l);
  struct dvm_object *no = names.l ? dvm__obj(vm, names.l) : NULL;
  const char *glob = nargs > 1 && ARG(1).l && dvm_string_utf8(vm, ARG(1).l) ? dvm_string_utf8(vm, ARG(1).l) : NULL;
  for (uint32_t i = 0; ok && no && i < no->length; ++i) {
    const char *nm = dvm_string_utf8(vm, ((dvm_ref *)no->data)[i]);
    if (!nm) continue;
    if (glob && luna_fnmatch(glob, nm, 0) != 0) continue;
    char full[DVM_PATH_MAX];
    snprintf(full, sizeof full, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", nm);
    dvm_ref p = nio_path_from_string(vm, dvm_new_string(vm, full));
    if (!p) {
      ok = false;
      break;
    }
    union dvm_value item = { .l = p };
    /* a DirectoryStream.Filter, when one was given, decides per entry */
    if (nargs > 1 && ARG(1).l && !dvm_string_utf8(vm, ARG(1).l)) {
      union dvm_value keep = { 0 };
      if (!am_vcall(vm, ARG(1).l, "accept", "(Ljava/lang/Object;)Z", &item, 1, &keep)) {
        ok = false;
        break;
      }
      if (!keep.i) continue;
    }
    ok = list_add(vm, entries, &item, 1, &ign);
  }
  if (ok) (void)dvm_set_field(vm, stream, "entries", "Ljava/util/List;", (union dvm_value) {
    .l = entries
  });
  dvm_unpin(vm, entries);
  dvm_unpin(vm, stream);
  if (!ok) return false;
  RETL(stream);
}
static bool x_dir_stream(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)args;
  (void)nargs;
  union dvm_value l = { 0 };
  if (!strcmp(builtin_name(), "close")) {
    (void)dvm_set_field(vm, self, "entries", "Ljava/util/List;", (union dvm_value) {
      .l = 0
    });
    RETV();
  }
  (void)dvm_get_field(vm, self, "entries", "Ljava/util/List;", &l);
  if (!l.l) {
    dvm__throw(vm, "java/lang/IllegalStateException", "Directory stream is closed");
    return false;
  }
  return am_vcall(vm, l.l, "iterator", "()Ljava/util/Iterator;", NULL, 0, out);
}
static const struct rt_method rt_directory_stream[] = {
  M("iterator", "()Ljava/util/Iterator;", x_dir_stream), M("close", "()V", x_dir_stream), M_END,
};
static const struct rt_method rt_gap_files[] = {
  SM("createDirectory", "(Ljava/nio/file/Path;[Ljava/nio/file/attribute/FileAttribute;)Ljava/nio/file/Path;", x_files_create_directory),
  SM("newDirectoryStream", "(Ljava/nio/file/Path;)Ljava/nio/file/DirectoryStream;", x_new_directory_stream),
  SM("newDirectoryStream", "(Ljava/nio/file/Path;Ljava/lang/String;)Ljava/nio/file/DirectoryStream;", x_new_directory_stream),
  SM("newDirectoryStream", "(Ljava/nio/file/Path;Ljava/nio/file/DirectoryStream$Filter;)Ljava/nio/file/DirectoryStream;", x_new_directory_stream),
  M_END,
};
static const struct rt_method rt_gap_fs_provider[] = {
  M("newDirectoryStream", "(Ljava/nio/file/Path;Ljava/nio/file/DirectoryStream$Filter;)Ljava/nio/file/DirectoryStream;", x_new_directory_stream),
  M("createDirectory", "(Ljava/nio/file/Path;[Ljava/nio/file/attribute/FileAttribute;)V", x_files_create_directory), M_END,
};

/* ---- android.util.StatsEvent -------------------------------------------------------------------------- */
static const struct rt_field rt_stats_builder_fields[] = {
  { "atomId", "I" }, { "values", "Ljava/util/List;" }, { "pooled", "Z" }, F_END,
};
static const struct rt_field rt_stats_event_fields[] = { { "atomId", "I" }, { "values", "Ljava/util/List;" }, F_END };
static bool x_stats_builder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "build")) {
    dvm_ref e = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/util/StatsEvent;"));
    if (!e) return false;
    union dvm_value v = { 0 };
    (void)dvm_get_field(vm, self, "atomId", "I", &v);
    (void)dvm_set_field(vm, e, "atomId", "I", v);
    (void)dvm_get_field(vm, self, "values", "Ljava/util/List;", &v);
    (void)dvm_set_field(vm, e, "values", "Ljava/util/List;", v);
    RETL(e);
  }
  if (!strcmp(n, "setAtomId")) {
    rt_set_int(vm, self, "atomId", ARG(0).i);
    RETL(self);
  }
  if (!strcmp(n, "usePooledBuffer")) {
    rt_set_int(vm, self, "pooled", 1);
    RETL(self);
  }
  union dvm_value list = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "values", "Ljava/util/List;", &list);
  if (!list.l) {
    list.l = coll_new_list(vm, "Ljava/util/ArrayList;");
    (void)dvm_set_field(vm, self, "values", "Ljava/util/List;", list);
  }
  const char *sig = builtin_sig();
  union dvm_value item = { 0 };
  if (!strcmp(n, "writeInt")) item.l = box_make(vm, "Ljava/lang/Integer;", ARG(0));
  else if (!strcmp(n, "writeLong")) item.l = box_make(vm, "Ljava/lang/Long;", ARG(0));
  else if (!strcmp(n, "writeFloat")) item.l = box_make(vm, "Ljava/lang/Float;", ARG(0));
  else if (!strcmp(n, "writeBoolean")) item.l = box_make(vm, "Ljava/lang/Boolean;", ARG(0));
  else item.l = ARG(0).l;   /* writeString / writeByteArray / writeAttributionChain */
  (void)sig;
  if (!list_add(vm, list.l, &item, 1, &ign)) return false;
  RETL(self);
}
static bool x_stats_new_builder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  dvm_ref b = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/util/StatsEvent$Builder;"));
  if (!b) return false;
  RETL(b);
}
static bool x_stats_event(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
  (void)args; (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "getAtomId")) return dvm_get_field(vm, self, "atomId", "I", out);
  if (!strcmp(n, "getNumBytes")) RETI(0);
  if (!strcmp(n, "getBytes")) RETL(dvm_new_array(vm, 'B', "B", 0));
  RETV();   /* release */
}
static const struct rt_method rt_stats_builder[] = {
  M("setAtomId", "(I)Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("writeInt", "(I)Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("writeLong", "(J)Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("writeFloat", "(F)Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("writeBoolean", "(Z)Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("writeString", "(Ljava/lang/String;)Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("writeByteArray", "([B)Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("usePooledBuffer", "()Landroid/util/StatsEvent$Builder;", x_stats_builder),
  M("build", "()Landroid/util/StatsEvent;", x_stats_builder), M_END,
};
static const struct rt_method rt_stats_event[] = {
  SM("newBuilder", "()Landroid/util/StatsEvent$Builder;", x_stats_new_builder),
  M("getAtomId", "()I", x_stats_event), M("getNumBytes", "()I", x_stats_event),
  M("getBytes", "()[B", x_stats_event), M("release", "()V", x_stats_event), M_END,
};

/* ---- BoringLayout.isBoring ---------------------------------------------------------------------------- */
static const struct rt_field rt_boring_metrics_fields[] = { { "width", "I" }, F_END };
static bool x_boring_is_boring(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)self;
  const char *text = ARG(0).l ? rt_charseq_utf8(vm, ARG(0).l) : NULL;
  if (!text || !ARG(1).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "text or paint");
    return false;
  }
  struct rt_text_units units={0};
  if (!text_read_units(vm,ARG(0).l,false,"java/lang/NullPointerException",&units)) return false;
  const size_t length=units.length;
  bool boring=true;
  for (size_t i=0;i<length;++i) {
    uint16_t cp=units.data[i];
    int direction=luna_uc_directionality(cp);
    if (cp=='\n' || cp=='\t' || (cp>=0xd800 && cp<=0xdfff) || direction==1 || direction==2 || direction>=14) {
      boring=false;break;
    }
  }
  free(units.owned);
  if (!boring) RETL(0);
  if (nargs>=4 && ARG(2).l) {
    union dvm_value rtl={0},a[]={ARG(0),{0},{.i=(int)length}};
    if (!am_vcall(vm,ARG(2).l,"isRtl","(Ljava/lang/CharSequence;II)Z",a,3,&rtl)) return false;
    if (rtl.i) RETL(0);
  }
  if (ui_is_a(vm,ARG(0).l,"android/text/Spanned")) {
    union dvm_value spans={0},a[]={{0},{.i=(int)length},{.l=dvm_class_object(vm,dvm_find_class(vm,"android/text/style/ParagraphStyle"))}};
    if (!am_vcall(vm,ARG(0).l,"getSpans","(IILjava/lang/Class;)[Ljava/lang/Object;",a,3,&spans)) return false;
    if (dvm_array_length(vm,spans.l)) RETL(0);
  }
  dvm_ref metrics = nargs>=4 ? ARG(nargs-1).l : nargs>2 ? ARG(2).l : 0;
  if (!metrics) {
    metrics = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/text/BoringLayout$Metrics;"));
    if (!metrics) return false;
  }
  dvm_pin(vm, metrics);
  union dvm_value fm = { .l = metrics }, ign = { 0 }, w = { 0 }, s = { .l = dvm_new_string(vm, text) };
  bool ok = am_vcall(vm, ARG(1).l, "getFontMetricsInt", "(Landroid/graphics/Paint$FontMetricsInt;)I", &fm, 1, &ign) &&
            am_vcall(vm, ARG(1).l, "measureText", "(Ljava/lang/String;)F", &s, 1, &w);
  if (ok) rt_set_int(vm, metrics, "width", (int32_t)ceilf(w.f));
  dvm_unpin(vm, metrics);
  if (!ok) return false;
  RETL(metrics);
}
static const struct rt_method rt_gap_boring[] = {
  M("<init>","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;FFLandroid/text/BoringLayout$Metrics;Z)V",boring_layout_init),
  M("<init>","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;FFLandroid/text/BoringLayout$Metrics;ZLandroid/text/TextUtils$TruncateAt;I)V",boring_layout_init),
  M("<init>","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;FFLandroid/text/BoringLayout$Metrics;ZLandroid/text/TextUtils$TruncateAt;IZ)V",boring_layout_init),
  SM("make","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;FFLandroid/text/BoringLayout$Metrics;Z)Landroid/text/BoringLayout;",boring_layout_init),
  SM("make","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;FFLandroid/text/BoringLayout$Metrics;ZLandroid/text/TextUtils$TruncateAt;I)Landroid/text/BoringLayout;",boring_layout_init),
  SM("make","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;Landroid/text/BoringLayout$Metrics;ZLandroid/text/TextUtils$TruncateAt;IZ)Landroid/text/BoringLayout;",boring_layout_init),
  M("replaceOrMake","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;FFLandroid/text/BoringLayout$Metrics;Z)Landroid/text/BoringLayout;",boring_layout_init),
  M("replaceOrMake","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;FFLandroid/text/BoringLayout$Metrics;ZLandroid/text/TextUtils$TruncateAt;I)Landroid/text/BoringLayout;",boring_layout_init),
  M("replaceOrMake","(Ljava/lang/CharSequence;Landroid/text/TextPaint;ILandroid/text/Layout$Alignment;Landroid/text/BoringLayout$Metrics;ZLandroid/text/TextUtils$TruncateAt;IZ)Landroid/text/BoringLayout;",boring_layout_init),
  M("getLineStart","(I)I",boring_layout_property),M("getLineTop","(I)I",boring_layout_property),
  M("getLineDescent","(I)I",boring_layout_property),M("getParagraphDirection","(I)I",boring_layout_property),
  M("getLineWidth","(I)F",boring_layout_property),M("getLineMax","(I)F",boring_layout_property),
  M("getLineContainsTab","(I)Z",boring_layout_property),
  M("getEllipsizedWidth","()I",boring_layout_property),M("getEllipsisCount","(I)I",boring_layout_property),M("getEllipsisStart","(I)I",boring_layout_property),
  M("isFallbackLineSpacingEnabled","()Z",boring_layout_property),
  M("getSpacingMultiplier","()F",boring_layout_property),M("getSpacingAdd","()F",boring_layout_property),
  SM("isBoring", "(Ljava/lang/CharSequence;Landroid/text/TextPaint;Landroid/text/BoringLayout$Metrics;)Landroid/text/BoringLayout$Metrics;", x_boring_is_boring),
  SM("isBoring", "(Ljava/lang/CharSequence;Landroid/text/TextPaint;Landroid/text/TextDirectionHeuristic;Landroid/text/BoringLayout$Metrics;)Landroid/text/BoringLayout$Metrics;", x_boring_is_boring),
  SM("isBoring", "(Ljava/lang/CharSequence;Landroid/text/TextPaint;Landroid/text/TextDirectionHeuristic;ZLandroid/text/BoringLayout$Metrics;)Landroid/text/BoringLayout$Metrics;", x_boring_is_boring),
  SM("isBoring", "(Ljava/lang/CharSequence;Landroid/text/TextPaint;)Landroid/text/BoringLayout$Metrics;", x_boring_is_boring), M_END,
};

/* ---- Resources.obtainAttributes, TypedArray.getType, AttributeSet resource values -------------------------- */
static bool x_resources_obtain_attributes(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                          union dvm_value *out)
{
  (void)nargs;
  /* AOSP: obtainAttributes() is obtainStyledAttributes() with no defaults and no style. */
  union dvm_value theme = { 0 };
  if (!am_vcall(vm, self, "newTheme", "()Landroid/content/res/Resources$Theme;", NULL, 0, &theme)) return false;
  union dvm_value a[4] = { ARG(0), ARG(1), { .i = 0 }, { .i = 0 } };
  return am_vcall(vm, theme.l, "obtainStyledAttributes", "(Landroid/util/AttributeSet;[III)Landroid/content/res/TypedArray;", a, 4, out);
}
static bool x_typed_array_type(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)nargs;
  union dvm_value attrs = { 0 }, names = { 0 }, types = { 0 };
  (void)dvm_get_field(vm, self, "attrs", "[I", &attrs);
  (void)dvm_get_field(vm, self, "xmlNames", "[I", &names);
  (void)dvm_get_field(vm, self, "xmlTypes", "[I", &types);
  struct dvm_object *ao = attrs.l ? dvm__obj(vm, attrs.l) : NULL, *no = names.l ? dvm__obj(vm, names.l) : NULL,
                    *to = types.l ? dvm__obj(vm, types.l) : NULL;
  if (!ao || ARG(0).i < 0 || (uint32_t)ARG(0).i >= ao->length) {
    dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "Index %d", ARG(0).i);
    return false;
  }
  const int32_t id = ((int32_t *)ao->data)[ARG(0).i];
  for (uint32_t i = 0; no && to && i < no->length && i < to->length; ++i)
    if (((int32_t *)no->data)[i] == id) RETI(((int32_t *)to->data)[i]);
  RETI(0);   /* TYPE_NULL: the attribute has no value */
}
static bool x_attr_resource_value(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
  (void)nargs;
  int index = -1, def;
  if (!strcmp(builtin_sig(), "(II)I")) {
    index = ARG(0).i;
    def = ARG(1).i;
  } else {   /* (String namespace, String attribute, int default) */
    def = ARG(2).i;
    const char *want = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL;
    union dvm_value count = { 0 };
    if (!want || !am_vcall(vm, self, "getAttributeCount", "()I", NULL, 0, &count)) RETI(def);
    for (int i = 0; i < count.i; ++i) {
      union dvm_value iv = { .i = i }, name = { 0 };
      if (am_vcall(vm, self, "getAttributeName", "(I)Ljava/lang/String;", &iv, 1, &name) && name.l &&
          !strcmp(dvm_string_utf8(vm, name.l), want)) {
        index = i;
        break;
      }
    }
  }
  union dvm_value types = { 0 }, data = { 0 };
  (void)dvm_get_field(vm, self, "attributeTypes", "[I", &types);
  (void)dvm_get_field(vm, self, "attributeData", "[I", &data);
  struct dvm_object *to = types.l ? dvm__obj(vm, types.l) : NULL, *dobj = data.l ? dvm__obj(vm, data.l) : NULL;
  if (index < 0 || !to || !dobj || (uint32_t)index >= to->length || (uint32_t)index >= dobj->length) RETI(def);
  /* TYPE_REFERENCE (0x01): the value is a resource id; anything else is not one */
  RETI(((int32_t *)to->data)[index] == 1 ? ((int32_t *)dobj->data)[index] : def);
}
static const struct rt_method rt_gap_resources2[] = {
  M("obtainAttributes", "(Landroid/util/AttributeSet;[I)Landroid/content/res/TypedArray;", x_resources_obtain_attributes), M_END,
};
static const struct rt_method rt_gap_typed_array[] = { M("getType", "(I)I", x_typed_array_type), M_END };
static const struct rt_method rt_gap_xml2[] = {
  M("getAttributeResourceValue", "(II)I", x_attr_resource_value),
  M("getAttributeResourceValue", "(Ljava/lang/String;Ljava/lang/String;I)I", x_attr_resource_value), M_END,
};

/* ---- android.content.res.loader.ResourcesLoader / ResourcesProvider -------------------------------------- */
static const struct rt_field rt_res_loader_fields[] = { { "providers", "Ljava/util/List;" }, F_END };
static const struct rt_field rt_res_provider_fields[] = { { "source", "Landroid/os/ParcelFileDescriptor;" }, F_END };
static bool x_res_loader(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  const char *n = builtin_name();
  union dvm_value list = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "providers", "Ljava/util/List;", &list);
  if (!list.l) {
    list.l = coll_new_list(vm, "Ljava/util/ArrayList;");
    (void)dvm_set_field(vm, self, "providers", "Ljava/util/List;", list);
  }
  if (!strcmp(n, "<init>")) RETV();
  if (!strcmp(n, "getProviders")) {
    union dvm_value a = { .l = list.l };
    struct dvm_class *c = dvm_find_class(vm, "java/util/Collections");
    struct dvm_method *m = c ? dvm_find_method(vm, c, "unmodifiableList", "(Ljava/util/List;)Ljava/util/List;") : NULL;
    return m && dvm_call(vm, m, 0, &a, 1, out);
  }
  if (!strcmp(n, "clearProviders")) return am_vcall(vm, list.l, "clear", "()V", NULL, 0, &ign);
  union dvm_value a = { .l = ARG(0).l };
  if (!strcmp(n, "addProvider")) return am_vcall(vm, list.l, "add", "(Ljava/lang/Object;)Z", &a, 1, &ign);
  if (!strcmp(n, "removeProvider")) return am_vcall(vm, list.l, "remove", "(Ljava/lang/Object;)Z", &a, 1, &ign);
  /* setProviders(List) */
  if (!am_vcall(vm, list.l, "clear", "()V", NULL, 0, &ign)) return false;
  return am_vcall(vm, list.l, "addAll", "(Ljava/util/Collection;)Z", &a, 1, &ign);
}
static bool x_res_provider(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
  (void)self; (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "fileDescriptor");
    return false;
  }
  dvm_ref p = dvm_new_object(vm, dvm__class_by_desc(vm, "Landroid/content/res/loader/ResourcesProvider;"));
  if (!p) return false;
  (void)dvm_set_field(vm, p, "source", "Landroid/os/ParcelFileDescriptor;", ARG(0));
  RETL(p);
}
static bool x_resources_loaders(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)nargs; (void)out;
  union dvm_value list = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "loaders", "Ljava/util/List;", &list);
  if (!list.l) {
    list.l = coll_new_list(vm, "Ljava/util/ArrayList;");
    (void)dvm_set_field(vm, self, "loaders", "Ljava/util/List;", list);
  }
  struct dvm_object *arr = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
  const bool add = !strcmp(builtin_name(), "addLoaders");
  for (uint32_t i = 0; arr && i < arr->length; ++i) {
    union dvm_value a = { .l = ((dvm_ref *)arr->data)[i] };
    if (add) (void)am_vcall(vm, list.l, "remove", "(Ljava/lang/Object;)Z", &a, 1, &ign);   /* re-adding moves it last */
    if (!am_vcall(vm, list.l, add ? "add" : "remove", "(Ljava/lang/Object;)Z", &a, 1, &ign)) return false;
  }
  RETV();
}
static const struct rt_method rt_resources_loader[] = {
  M("<init>", "()V", x_res_loader), M("addProvider", "(Landroid/content/res/loader/ResourcesProvider;)V", x_res_loader),
  M("removeProvider", "(Landroid/content/res/loader/ResourcesProvider;)V", x_res_loader),
  M("getProviders", "()Ljava/util/List;", x_res_loader), M("clearProviders", "()V", x_res_loader),
  M("setProviders", "(Ljava/util/List;)V", x_res_loader), M_END,
};
static const struct rt_method rt_resources_provider[] = {
  SM("loadFromApk", "(Landroid/os/ParcelFileDescriptor;)Landroid/content/res/loader/ResourcesProvider;", x_res_provider), M_END,
};
static const struct rt_method rt_gap_resources3[] = {
  M("addLoaders", "([Landroid/content/res/loader/ResourcesLoader;)V", x_resources_loaders),
  M("removeLoaders", "([Landroid/content/res/loader/ResourcesLoader;)V", x_resources_loaders), M_END,
};

/* ---- RenderScript: this host has no RenderScript runtime, which apps are told as the platform tells them --- */
static bool x_renderscript_create(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
  (void)self; (void)args; (void)nargs; (void)out;
  dvm__throw(vm, "android/renderscript/RSRuntimeException", "Renderscript is not available on this device");
  return false;
}
static const struct rt_method rt_renderscript[] = {
  SM("create", "(Landroid/content/Context;)Landroid/renderscript/RenderScript;", x_renderscript_create), M_END,
};

/* ---- SSLServerSocketFactory.getDefault ----------------------------------------------------------------------- */
static bool x_ssl_server_factory(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  const char *n = builtin_name();
  if (!strcmp(n, "getDefault")) {
    dvm_ref f = dvm_new_object(vm, dvm__class_by_desc(vm, "Llunaria/DefaultSSLServerSocketFactory;"));
    if (!f) return false;
    RETL(f);
  }
  if (!strcmp(n, "getDefaultCipherSuites") || !strcmp(n, "getSupportedCipherSuites")) {
    const char *const *all = dvm_tls_supported_ciphers();
    RETL(string_array_of(vm, all, cipher_count(all)));
  }
  /* The host library serves TLS clients only: a listening TLS socket is reported as not possible. */
  dvm__throw(vm, "java/net/SocketException", "TLS server sockets are not supported on this host");
  return false;
}
static const struct rt_method rt_default_ssl_server_factory[] = {
  M("<init>", "()V", x_empty_ctor),
  M("getDefaultCipherSuites", "()[Ljava/lang/String;", x_ssl_server_factory),
  M("getSupportedCipherSuites", "()[Ljava/lang/String;", x_ssl_server_factory),
  M("createServerSocket", "()Ljava/net/ServerSocket;", x_ssl_server_factory),
  M("createServerSocket", "(I)Ljava/net/ServerSocket;", x_ssl_server_factory),
  M("createServerSocket", "(II)Ljava/net/ServerSocket;", x_ssl_server_factory),
  M("createServerSocket", "(IILjava/net/InetAddress;)Ljava/net/ServerSocket;", x_ssl_server_factory), M_END,
};
static const struct rt_method rt_gap_ssl_server_static[] = {
  SM("getDefault", "()Ljavax/net/ServerSocketFactory;", x_ssl_server_factory), M_END,
};

/* ---- ScaleGestureDetector (AOSP's span / focus tracking) -------------------------------------------------------- */
static const struct rt_field rt_scale_fields[] = {
  { "listener", "Landroid/view/ScaleGestureDetector$OnScaleGestureListener;" }, { "inProgress", "Z" },
  { "focusX", "F" }, { "focusY", "F" }, { "currSpan", "F" }, { "prevSpan", "F" }, { "initialSpan", "F" },
  { "currTime", "J" }, { "prevTime", "J" }, { "minSpan", "I" }, { "spanSlop", "I" }, F_END,
};
static float sc_f(struct dvm *vm, dvm_ref s, const char *f)
{
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, s, f, "F", &v);
  return v.f;
}
static void sc_set_f(struct dvm *vm, dvm_ref s, const char *f, float v)
{
  (void)dvm_set_field(vm, s, f, "F", (union dvm_value) {
    .f = v
  });
}
static bool sc_notify(struct dvm *vm, dvm_ref self, const char *name, const char *ret, bool *answer)
{
  union dvm_value l = { 0 }, a = { .l = self }, r = { 0 };
  (void)dvm_get_field(vm, self, "listener", "Landroid/view/ScaleGestureDetector$OnScaleGestureListener;", &l);
  char sig[96];
  snprintf(sig, sizeof sig, "(Landroid/view/ScaleGestureDetector;)%s", ret);
  if (!l.l) return true;
  if (!am_vcall(vm, l.l, name, sig, &a, 1, &r)) return false;
  if (answer) *answer = r.i != 0;
  return true;
}
static bool x_scale_detector(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  const char *n = builtin_name();
  if (!strcmp(n, "<init>")) {
    if (!ARG(0).l || !ARG(1).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "context or listener");
      return false;
    }
    (void)dvm_set_field(vm, self, "listener", "Landroid/view/ScaleGestureDetector$OnScaleGestureListener;", ARG(1));
    const float d = rt_density();
    rt_set_int(vm, self, "minSpan", (int32_t)(27 * d + 0.5f));   /* 27dp: ViewConfiguration.getScaledMinimumScalingSpan */
    rt_set_int(vm, self, "spanSlop", (int32_t)(2 * 8 * d + 0.5f));   /* twice the 8dp touch slop */
    RETV();
  }
  if (!strcmp(n, "isInProgress")) RETI(rt_get_int(vm, self, "inProgress"));
  if (!strcmp(n, "getFocusX")) RETF(sc_f(vm, self, "focusX"));
  if (!strcmp(n, "getFocusY")) RETF(sc_f(vm, self, "focusY"));
  if (!strcmp(n, "getCurrentSpan")) RETF(sc_f(vm, self, "currSpan"));
  if (!strcmp(n, "getPreviousSpan")) RETF(sc_f(vm, self, "prevSpan"));
  if (!strcmp(n, "getScaleFactor")) {
    const float prev = sc_f(vm, self, "prevSpan");
    RETF(rt_get_int(vm, self, "inProgress") && prev > 0 ? sc_f(vm, self, "currSpan") / prev : 1.0f);
  }
  union dvm_value t = { 0 };
  if (!strcmp(n, "getEventTime") || !strcmp(n, "getTimeDelta")) {
    union dvm_value c = { 0 }, p = { 0 };
    (void)dvm_get_field(vm, self, "currTime", "J", &c);
    (void)dvm_get_field(vm, self, "prevTime", "J", &p);
    RETJ(!strcmp(n, "getEventTime") ? c.j : c.j - p.j);
  }
  if (strcmp(n, "onTouchEvent")) RETV();   /* quick-scale and stylus switches have no effect on a two-finger scale */
  /* onTouchEvent(MotionEvent) */
  dvm_ref ev = ARG(0).l;
  union dvm_value am = { 0 }, pc = { 0 }, ai = { 0 }, et = { 0 };
  if (!ev || !am_vcall(vm, ev, "getActionMasked", "()I", NULL, 0, &am) || !am_vcall(vm, ev, "getPointerCount", "()I", NULL, 0, &pc) ||
      !am_vcall(vm, ev, "getActionIndex", "()I", NULL, 0, &ai) || !am_vcall(vm, ev, "getEventTime", "()J", NULL, 0, &et)) return false;
  (void)dvm_set_field(vm, self, "currTime", "J", et);
  const int action = am.i, count_all = pc.i;
  const bool stream_complete = action == 1 || action == 3;
  if (action == 0 || stream_complete) {
    if (rt_get_int(vm, self, "inProgress")) {
      if (!sc_notify(vm, self, "onScaleEnd", "V", NULL)) return false;
      rt_set_int(vm, self, "inProgress", 0);
      sc_set_f(vm, self, "initialSpan", 0);
    }
    if (stream_complete) RETI(1);
  }
  const bool config_changed = action == 0 || action == 6 || action == 5;
  const bool pointer_up = action == 6;
  const int skip = pointer_up ? ai.i : -1;
  float sx = 0, sy = 0;
  const int count = pointer_up ? count_all - 1 : count_all;
  for (int i = 0; i < count_all; ++i) {
    if (i == skip) continue;
    union dvm_value iv = { .i = i }, x = { 0 }, y = { 0 };
    if (!am_vcall(vm, ev, "getX", "(I)F", &iv, 1, &x) || !am_vcall(vm, ev, "getY", "(I)F", &iv, 1, &y)) return false;
    sx += x.f;
    sy += y.f;
  }
  const float fx = count ? sx / (float)count : 0, fy = count ? sy / (float)count : 0;
  float dev = 0;
  for (int i = 0; i < count_all; ++i) {
    if (i == skip) continue;
    union dvm_value iv = { .i = i }, x = { 0 }, y = { 0 };
    if (!am_vcall(vm, ev, "getX", "(I)F", &iv, 1, &x) || !am_vcall(vm, ev, "getY", "(I)F", &iv, 1, &y)) return false;
    dev += hypotf(x.f - fx, y.f - fy);
  }
  const float span = count ? 2.0f * (dev / (float)count) : 0;
  sc_set_f(vm, self, "focusX", fx);
  sc_set_f(vm, self, "focusY", fy);
  const bool was = rt_get_int(vm, self, "inProgress");
  const int min_span = rt_get_int(vm, self, "minSpan");
  if (was && (span < (float)min_span || count < 2)) {
    if (!sc_notify(vm, self, "onScaleEnd", "V", NULL)) return false;
    rt_set_int(vm, self, "inProgress", 0);
    sc_set_f(vm, self, "initialSpan", span);
  }
  if (config_changed) {
    sc_set_f(vm, self, "currSpan", span);
    sc_set_f(vm, self, "prevSpan", span);
    sc_set_f(vm, self, "initialSpan", span);
  }
  if (!rt_get_int(vm, self, "inProgress") && count >= 2 && span >= (float)min_span &&
      (was || fabsf(span - sc_f(vm, self, "initialSpan")) > (float)rt_get_int(vm, self, "spanSlop"))) {
    sc_set_f(vm, self, "prevSpan", sc_f(vm, self, "currSpan"));
    (void)dvm_get_field(vm, self, "currTime", "J", &t);
    (void)dvm_set_field(vm, self, "prevTime", "J", t);
    bool begin = false;
    if (!sc_notify(vm, self, "onScaleBegin", "Z", &begin)) return false;
    rt_set_int(vm, self, "inProgress", begin);
  }
  if (action == 2) {
    sc_set_f(vm, self, "currSpan", span);
    if (rt_get_int(vm, self, "inProgress")) {
      bool update = true;
      if (!sc_notify(vm, self, "onScale", "Z", &update)) return false;
      if (update) {
        sc_set_f(vm, self, "prevSpan", span);
        (void)dvm_get_field(vm, self, "currTime", "J", &t);
        (void)dvm_set_field(vm, self, "prevTime", "J", t);
      }
    }
  }
  RETI(1);
}
static const struct rt_method rt_scale_gesture_detector[] = {
  M("<init>", "(Landroid/content/Context;Landroid/view/ScaleGestureDetector$OnScaleGestureListener;)V", x_scale_detector),
  M("<init>", "(Landroid/content/Context;Landroid/view/ScaleGestureDetector$OnScaleGestureListener;Landroid/os/Handler;)V", x_scale_detector),
  M("onTouchEvent", "(Landroid/view/MotionEvent;)Z", x_scale_detector), M("isInProgress", "()Z", x_scale_detector),
  M("getFocusX", "()F", x_scale_detector), M("getFocusY", "()F", x_scale_detector), M("getCurrentSpan", "()F", x_scale_detector),
  M("getPreviousSpan", "()F", x_scale_detector), M("getScaleFactor", "()F", x_scale_detector),
  M("getEventTime", "()J", x_scale_detector), M("getTimeDelta", "()J", x_scale_detector),
  M("setQuickScaleEnabled", "(Z)V", x_scale_detector), M("setStylusScaleEnabled", "(Z)V", x_scale_detector), M_END,
};

/* ---- ObjectAnimator.ofObject(target, Property, TypeConverter, Path) ----------------------------------------------- */
static const struct rt_field rt_path_listener_fields[] = {
  { "property", "Landroid/util/Property;" }, { "converter", "Landroid/animation/TypeConverter;" },
  { "measure", "Landroid/graphics/PathMeasure;" }, F_END,
};
static bool x_path_listener(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
  (void)out; (void)nargs;
  dvm_ref animator = ARG(0).l;
  union dvm_value target = { 0 }, frac = { 0 }, prop = { 0 }, conv = { 0 }, measure = { 0 }, len = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, animator, "target", "Ljava/lang/Object;", &target);
  (void)dvm_get_field(vm, animator, "fraction", "F", &frac);
  (void)dvm_get_field(vm, self, "property", "Landroid/util/Property;", &prop);
  (void)dvm_get_field(vm, self, "converter", "Landroid/animation/TypeConverter;", &conv);
  (void)dvm_get_field(vm, self, "measure", "Landroid/graphics/PathMeasure;", &measure);
  if (!target.l || !prop.l || !measure.l) RETV();
  if (!am_vcall(vm, measure.l, "getLength", "()F", NULL, 0, &len)) return false;
  dvm_ref pos = dvm_new_array(vm, 'F', "F", 2);
  if (!pos) return false;
  dvm_pin(vm, pos);
  union dvm_value pa[3] = { { .f = len.f * frac.f }, { .l = pos }, { .l = 0 } };
  bool ok = am_vcall(vm, measure.l, "getPosTan", "(F[F[F)Z", pa, 3, &ign);
  dvm_ref point = ok ? dvm_new_object(vm, dvm_find_class(vm, "android/graphics/PointF")) : 0;
  union dvm_value set[2] = { { .l = target.l }, { 0 } };
  if (point) {
    dvm_pin(vm, point);
    float *xy = dvm_array_data(vm, pos);
    union dvm_value fxy[2] = { { .f = xy[0] }, { .f = xy[1] } };
    ok = am_vcall(vm, point, "<init>", "(FF)V", fxy, 2, &ign);
    union dvm_value p = { .l = point }, converted = { 0 };
    if (ok) ok = !conv.l ? (converted.l = point, true) : am_vcall(vm, conv.l, "convert", "(Ljava/lang/Object;)Ljava/lang/Object;", &p, 1, &converted);
    set[1].l = converted.l;
    if (ok) ok = am_vcall(vm, prop.l, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V", set, 2, &ign);
    dvm_unpin(vm, point);
  } else ok = false;
  dvm_unpin(vm, pos);
  return ok;
}
static const struct rt_method rt_path_listener[] = {
  M("onAnimationUpdate", "(Landroid/animation/ValueAnimator;)V", x_path_listener), M_END,
};
static bool x_animator_of_object(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)self; (void)nargs;
  if (!ARG(1).l || !ARG(3).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "property or path");
    return false;
  }
  dvm_ref values = dvm_new_array(vm, 'F', "F", 2);
  float *v = values ? dvm_array_data(vm, values) : NULL;
  if (!v) return false;
  v[0] = 0.0f;
  v[1] = 1.0f;
  dvm_pin(vm, values);
  union dvm_value a[3] = { ARG(0), ARG(1), { .l = values } }, animator = { 0 }, ign = { 0 };
  bool ok = oa_of_float(vm, 0, a, 3, &animator) && animator.l;
  dvm_unpin(vm, values);
  if (!ok) return false;
  dvm_pin(vm, animator.l);
  /* the animator only carries progress: the listener walks the path and sets the property */
  (void)dvm_set_field(vm, animator.l, "property", "Landroid/util/Property;", (union dvm_value) {
    .l = 0
  });
  dvm_ref measure = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/PathMeasure"));
  dvm_ref listener = dvm_new_object(vm, dvm__class_by_desc(vm, "Llunaria/PathAnimatorListener;"));
  union dvm_value pm[2] = { ARG(3), { .i = 0 } };
  ok = measure && listener && am_vcall(vm, measure, "<init>", "(Landroid/graphics/Path;Z)V", pm, 2, &ign);
  if (ok) {
    dvm_pin(vm, listener);
    (void)dvm_set_field(vm, listener, "property", "Landroid/util/Property;", ARG(1));
    (void)dvm_set_field(vm, listener, "converter", "Landroid/animation/TypeConverter;", ARG(2));
    (void)dvm_set_field(vm, listener, "measure", "Landroid/graphics/PathMeasure;", (union dvm_value) {
      .l = measure
    });
    union dvm_value l = { .l = listener };
    ok = am_vcall(vm, animator.l, "addUpdateListener", "(Landroid/animation/ValueAnimator$AnimatorUpdateListener;)V", &l, 1, &ign);
    dvm_unpin(vm, listener);
  }
  dvm_unpin(vm, animator.l);
  if (!ok) return false;
  RETL(animator.l);
}
static const struct rt_method rt_gap_object_animator2[] = {
  SM("ofObject", "(Ljava/lang/Object;Landroid/util/Property;Landroid/animation/TypeConverter;Landroid/graphics/Path;)Landroid/animation/ObjectAnimator;", x_animator_of_object),
  M_END,
};


/* ---- Framework APIs reached while WhatsApp starts ---------------------------------------------------------------- */

/* Application.ActivityLifecycleCallbacks (API 29) declares the Pre/Post callbacks as default methods with empty
 * bodies; an app interface extending it inherits them and calls them through itself. */
static const struct rt_method rt_gap_lifecycle_callbacks[] = {
  M("onActivityPreCreated", "(Landroid/app/Activity;Landroid/os/Bundle;)V", empty_void),
  M("onActivityPostCreated", "(Landroid/app/Activity;Landroid/os/Bundle;)V", empty_void),
  M("onActivityPreStarted", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPostStarted", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPreResumed", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPostResumed", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPrePaused", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPostPaused", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPreStopped", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPostStopped", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPreSaveInstanceState", "(Landroid/app/Activity;Landroid/os/Bundle;)V", empty_void),
  M("onActivityPostSaveInstanceState", "(Landroid/app/Activity;Landroid/os/Bundle;)V", empty_void),
  M("onActivityPreDestroyed", "(Landroid/app/Activity;)V", empty_void),
  M("onActivityPostDestroyed", "(Landroid/app/Activity;)V", empty_void),
  M_END,
};

/* Activity/View.requireViewById(): findViewById() that refuses to answer null. */
static bool x_require_view(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
  (void)nargs;
  union dvm_value v = { 0 };
  if (!am_vcall(vm, self, "findViewById", "(I)Landroid/view/View;", args, 1, &v)) return false;
  if (!v.l) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "ID does not reference a View inside this %s",
               ui_is_a(vm, self, "android/app/Activity") ? "Activity" : "View");
    return false;
  }
  RETL(v.l);
}
static const struct rt_method rt_gap_require_view[] = {
  M("requireViewById", "(I)Landroid/view/View;", x_require_view), M_END,
};

/* TextView.isAllCaps / minimum and maximum width / compound paddings. */
static bool x_text_is_all_caps(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)args; (void)nargs;
  RETI(ui_int(vm, self, "allCaps") != 0);
}
static bool x_text_width_limit(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  if (!strcmp(name, "getMinWidth")) RETI(ui_int(vm, self, "textMinWidth"));
  if (!strcmp(name, "getMaxWidth")) RETI(ui_int(vm, self, "textMaxWidthSet") ? ui_int(vm, self, "textMaxWidth") : INT32_MAX);
  if (!strcmp(name, "setMinWidth")) rt_set_int(vm, self, "textMinWidth", ARG(0).i);
  else {
    rt_set_int(vm, self, "textMaxWidth", ARG(0).i);
    rt_set_int(vm, self, "textMaxWidthSet", 1);
  }
  ui_layout_changed();
  ui_invalidate();
  RETV();
}
static bool x_text_compound_padding(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
  (void)args; (void)nargs;
  int32_t pad[4];
  ui_pad(vm, self, pad);
  struct text_compound c;
  text_compound_get(vm, self, &c);
  const char *name = builtin_name();
  if (strstr(name, "Left")) RETI(pad[0] + c.left_inset);
  if (strstr(name, "Right")) RETI(pad[2] + c.right_inset);
  if (strstr(name, "Top")) RETI(pad[1]);
  RETI(pad[3]);
}
/* Tint applies to the compound drawables now present and is remembered for ones set later. */
static bool x_text_compound_tint(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)nargs; (void)out;
  const bool mode = !strcmp(builtin_name(), "setCompoundDrawableTintMode");
  (void)dvm_set_field(vm, self, mode ? "compoundTintMode" : "compoundTint",
                      mode ? "Landroid/graphics/PorterDuff$Mode;" : "Landroid/content/res/ColorStateList;", ARG(0));
  static const char *const names[] = { "drawableLeft", "drawableTop", "drawableRight", "drawableBottom",
                                       "drawableStart", "drawableEnd" };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
    dvm_ref d = ui_obj(vm, self, names[i], "Landroid/graphics/drawable/Drawable;");
    union dvm_value ign = { 0 };
    if (d && !am_vcall(vm, d, mode ? "setTintMode" : "setTintList",
                       mode ? "(Landroid/graphics/PorterDuff$Mode;)V" : "(Landroid/content/res/ColorStateList;)V",
                       args, 1, &ign)) return false;
  }
  ui_invalidate();
  RETV();
}
/* setAutoSizeTextTypeUniformWithPresetSizes(int[] sizes, unit): the sizes are kept; the range is their extent. */
static bool x_text_autosize_presets(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
  (void)nargs; (void)out;
  struct dvm_object *a = args[0].l ? dvm__obj(vm, args[0].l) : NULL;
  if (!a) {
    dvm__throw(vm, "java/lang/NullPointerException", "presetSizes");
    return false;
  }
  const int32_t *v = a->data;
  int32_t lo = INT32_MAX, hi = 0;
  for (uint32_t i = 0; i < a->length; ++i) {
    if (v[i] <= 0) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "All preset sizes must be positive");
      return false;
    }
    if (v[i] < lo) lo = v[i];
    if (v[i] > hi) hi = v[i];
  }
  (void)dvm_set_field(vm, self, "autoSizePresets", "[I", args[0]);
  rt_set_int(vm, self, "autoSizeMin", a->length ? lo : 0);
  rt_set_int(vm, self, "autoSizeMax", a->length ? hi : 0);
  ui_layout_changed();
  ui_invalidate();
  RETV();
}
static const struct rt_method rt_gap_text_view3[] = {
  M("isAllCaps", "()Z", x_text_is_all_caps),
  M("getMinWidth", "()I", x_text_width_limit), M("setMinWidth", "(I)V", x_text_width_limit),
  M("getMaxWidth", "()I", x_text_width_limit), M("setMaxWidth", "(I)V", x_text_width_limit),
  M("getCompoundPaddingLeft", "()I", x_text_compound_padding),
  M("getCompoundPaddingRight", "()I", x_text_compound_padding),
  M("getCompoundPaddingTop", "()I", x_text_compound_padding),
  M("getCompoundPaddingBottom", "()I", x_text_compound_padding),
  M("setCompoundDrawableTintList", "(Landroid/content/res/ColorStateList;)V", x_text_compound_tint),
  M("setCompoundDrawableTintMode", "(Landroid/graphics/PorterDuff$Mode;)V", x_text_compound_tint),
  M("setAutoSizeTextTypeUniformWithPresetSizes", "([II)V", x_text_autosize_presets),
  M_END,
};

/* Paint.clearShadowLayer(): setShadowLayer(0, 0, 0, 0). */
static bool x_paint_clear_shadow(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)args; (void)nargs; (void)out;
  union dvm_value z = { 0 };
  (void)dvm_set_field(vm, self, "shadowRadius", "F", z);
  (void)dvm_set_field(vm, self, "shadowDx", "F", z);
  (void)dvm_set_field(vm, self, "shadowDy", "F", z);
  (void)dvm_set_field(vm, self, "shadowColor", "I", z);
  RETV();
}
static const struct rt_method rt_gap_paint2[] = {
  M("clearShadowLayer", "()V", x_paint_clear_shadow), M_END,
};

/* View.invalidateOutline(): the outline is queried from its provider when drawn, so a redraw is the whole update. */
static bool x_view_invalidate_outline(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
  (void)args; (void)nargs; (void)out;
  ui_invalidate_view(vm, self, NULL, 0);
  RETV();
}
static const struct rt_method rt_gap_view_outline[] = {
  M("invalidateOutline", "()V", x_view_invalidate_outline), M_END,
};

/* RoundRectShape(outerRadii, inset, innerRadii): an outer rounded rectangle, minus an inner one when `inset` is given. */
static const struct rt_field rt_round_rect_fields[] = {
  { "outer", "[F" }, { "inset", "Landroid/graphics/RectF;" }, { "inner", "[F" }, F_END,
};
static bool x_round_rect_init(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  (void)nargs;
  struct dvm_object *o = args[0].l ? dvm__obj(vm, args[0].l) : NULL;
  if (!o || o->length < 8) {
    dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "outer radii array needs 8 values");
    return false;
  }
  struct dvm_object *i = args[2].l ? dvm__obj(vm, args[2].l) : NULL;
  if (i && i->length < 8) {
    dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "inner radii array needs 8 values");
    return false;
  }
  (void)dvm_set_field(vm, self, "outer", "[F", args[0]);
  (void)dvm_set_field(vm, self, "inset", "Landroid/graphics/RectF;", args[1]);
  (void)dvm_set_field(vm, self, "inner", "[F", args[2]);
  RETV();
}
static bool x_rectf_new(struct dvm *vm, float l, float t, float r, float b, dvm_ref *rect)
{
  *rect = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/RectF"));
  union dvm_value a[4] = { { .f = l }, { .f = t }, { .f = r }, { .f = b } }, ign = { 0 };
  return *rect && am_vcall(vm, *rect, "<init>", "(FFFF)V", a, 4, &ign);
}
static bool x_round_rect_draw(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  (void)nargs;
  union dvm_value w = { 0 }, h = { 0 }, outer = { 0 }, inset = { 0 }, inner = { 0 }, cw = { 0 }, ccw = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "width", "F", &w);
  (void)dvm_get_field(vm, self, "height", "F", &h);
  (void)dvm_get_field(vm, self, "outer", "[F", &outer);
  (void)dvm_get_field(vm, self, "inset", "Landroid/graphics/RectF;", &inset);
  (void)dvm_get_field(vm, self, "inner", "[F", &inner);
  struct dvm_class *dir = dvm_find_class(vm, "android/graphics/Path$Direction");
  if (!dir || !dvm_get_static(vm, dir, "CW", "Landroid/graphics/Path$Direction;", &cw) ||
      !dvm_get_static(vm, dir, "CCW", "Landroid/graphics/Path$Direction;", &ccw)) return false;
  dvm_ref path = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Path"));
  if (!path || !am_vcall(vm, path, "<init>", "()V", NULL, 0, &ign)) return false;
  dvm_pin(vm, path);
  dvm_ref rect = 0;
  bool ok = x_rectf_new(vm, 0, 0, w.f, h.f, &rect);
  if (ok) {
    dvm_pin(vm, rect);
    union dvm_value a[3] = { { .l = rect }, outer, cw };
    ok = outer.l ? am_vcall(vm, path, "addRoundRect", "(Landroid/graphics/RectF;[FLandroid/graphics/Path$Direction;)V", a, 3, &ign)
                 : am_vcall(vm, path, "addRect", "(Landroid/graphics/RectF;Landroid/graphics/Path$Direction;)V", (union dvm_value[]){ { .l = rect }, cw }, 2, &ign);
    dvm_unpin(vm, rect);
  }
  if (ok && inset.l) {
    float l = ui_float(vm, inset.l, "left"), t = ui_float(vm, inset.l, "top");
    float r = ui_float(vm, inset.l, "right"), b = ui_float(vm, inset.l, "bottom");
    dvm_ref hole = 0;
    ok = x_rectf_new(vm, l, t, w.f - r, h.f - b, &hole);
    if (ok) {
      dvm_pin(vm, hole);
      union dvm_value a[3] = { { .l = hole }, inner, ccw };
      ok = inner.l ? am_vcall(vm, path, "addRoundRect", "(Landroid/graphics/RectF;[FLandroid/graphics/Path$Direction;)V", a, 3, &ign)
                   : am_vcall(vm, path, "addRect", "(Landroid/graphics/RectF;Landroid/graphics/Path$Direction;)V", (union dvm_value[]){ { .l = hole }, ccw }, 2, &ign);
      dvm_unpin(vm, hole);
    }
  }
  if (ok) {
    union dvm_value a[2] = { { .l = path }, args[1] };
    ok = am_vcall(vm, args[0].l, "drawPath", "(Landroid/graphics/Path;Landroid/graphics/Paint;)V", a, 2, out);
  }
  dvm_unpin(vm, path);
  return ok;
}
static const struct rt_method rt_round_rect_shape[] = {
  M("<init>", "([FLandroid/graphics/RectF;[F)V", x_round_rect_init),
  M("draw", "(Landroid/graphics/Canvas;Landroid/graphics/Paint;)V", x_round_rect_draw),
  M_END,
};

/* Intent.sourceBounds / selector / clipData. */
static bool x_intent_aux(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  const char *field = strstr(name, "SourceBounds") ? "sourceBounds" : strstr(name, "Selector") ? "selector" : "clipData";
  const char *type = !strcmp(field, "sourceBounds") ? "Landroid/graphics/Rect;"
                     : !strcmp(field, "selector") ? "Landroid/content/Intent;" : "Landroid/content/ClipData;";
  if (name[0] == 'g') return dvm_get_field(vm, self, field, type, out);
  union dvm_value v = ARG(0);
  if (!strcmp(field, "selector") && v.l) {
    if (v.l == self) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Intent being set as a selector of itself");
      return false;
    }
    union dvm_value pkg = { 0 };
    (void)dvm_get_field(vm, self, "package", "Ljava/lang/String;", &pkg);
    if (pkg.l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Can't set selector when package already set to %s",
                 dvm_string_utf8(vm, pkg.l));
      return false;
    }
  }
  if (!strcmp(field, "sourceBounds") && v.l) {          /* the Intent keeps its own copy */
    dvm_ref copy = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Rect"));
    union dvm_value src = v, ign = { 0 };
    if (!copy || !am_vcall(vm, copy, "<init>", "(Landroid/graphics/Rect;)V", &src, 1, &ign)) return false;
    v.l = copy;
  }
  (void)dvm_set_field(vm, self, field, type, v);
  RETV();
}
static const struct rt_method rt_gap_intent_aux[] = {
  M("setSourceBounds", "(Landroid/graphics/Rect;)V", x_intent_aux),
  M("getSourceBounds", "()Landroid/graphics/Rect;", x_intent_aux),
  M("setSelector", "(Landroid/content/Intent;)V", x_intent_aux),
  M("getSelector", "()Landroid/content/Intent;", x_intent_aux),
  M("setClipData", "(Landroid/content/ClipData;)V", x_intent_aux),
  M("getClipData", "()Landroid/content/ClipData;", x_intent_aux),
  M_END,
};

/* Activity.finishAffinity(): finish this activity; the task this emulator keeps is single-affinity, so the
 * activities beneath it are finished too. */
static bool x_activity_finish_affinity(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
  (void)args; (void)nargs;
  const int at = act_index(self);
  for (int i = 0; i < at; ++i)
    if (!act_finish(vm, g_acts[i].activity, NULL, 0, out)) return false;
  return act_finish(vm, self, NULL, 0, out);
}
static bool x_activity_set_title_res(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)nargs;
  union dvm_value res = { 0 }, text = { 0 };
  if (!am_vcall(vm, self, "getResources", "()Landroid/content/res/Resources;", NULL, 0, &res) ||
      !am_vcall(vm, res.l, "getText", "(I)Ljava/lang/CharSequence;", args, 1, &text)) return false;
  return am_vcall(vm, self, "setTitle", "(Ljava/lang/CharSequence;)V", &text, 1, out);
}
static const struct rt_method rt_gap_activity2[] = {
  M("finishAffinity", "()V", x_activity_finish_affinity),
  M("setTitle", "(I)V", x_activity_set_title_res),
  M_END,
};

/* A phone without a SIM: no roaming, and the default data subscription is INVALID_SUBSCRIPTION_ID. */
static bool x_sub_roaming(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETI(0);
}
static bool x_sub_default_data(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETI(-1);
}
static const struct rt_method rt_gap_subscription[] = {
  M("isNetworkRoaming", "(I)Z", x_sub_roaming),
  SM("getDefaultDataSubscriptionId", "()I", x_sub_default_data),
  M_END,
};

/* NotificationChannel.setConversationId(parent, conversation) / getConversationId() / getParentChannelId(). */
static bool x_channel_conversation(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  if (!strcmp(name, "getConversationId"))
    return dvm_get_field(vm, self, "conversationId", "Ljava/lang/String;", out);
  if (!strcmp(name, "getParentChannelId"))
    return dvm_get_field(vm, self, "parentChannelId", "Ljava/lang/String;", out);
  (void)dvm_set_field(vm, self, "parentChannelId", "Ljava/lang/String;", ARG(0));
  (void)dvm_set_field(vm, self, "conversationId", "Ljava/lang/String;", ARG(1));
  RETV();
}
static const struct rt_method rt_gap_channel2[] = {
  M("setConversationId", "(Ljava/lang/String;Ljava/lang/String;)V", x_channel_conversation),
  M("getConversationId", "()Ljava/lang/String;", x_channel_conversation),
  M("getParentChannelId", "()Ljava/lang/String;", x_channel_conversation),
  M_END,
};

/* Resources(AssetManager, DisplayMetrics, Configuration). */
static bool x_resources_init3(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  (void)nargs;
  (void)dvm_set_field(vm, self, "displayMetrics", "Landroid/util/DisplayMetrics;", args[1]);
  (void)dvm_set_field(vm, self, "configuration", "Landroid/content/res/Configuration;", args[2]);
  RETV();
}
static const struct rt_method rt_gap_resources4[] = {
  M("<init>", "(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V",
    x_resources_init3),
  M_END,
};


/* ---- java.util.concurrent.atomic.AtomicIntegerArray / AtomicLongArray / LongAdder ------------------------------------ */
/* Guest threads run under one interpreter lock, so a plain read-modify-write is atomic. */
static const struct rt_field rt_atomic_long_array_fields[] = { { "values", "[J" }, F_END };
static const struct rt_field rt_atomic_int_array_fields[] = { { "values", "[I" }, F_END };
static bool x_atomic_array(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
  const struct dvm_method *m = dvm__builtin_method();
  const char *name = m->name, *sig = m->sig;
  const bool wide = !strcmp(dvm_class_name(dvm_object_class(vm, self)), "java/util/concurrent/atomic/AtomicLongArray");
  const char *type = wide ? "[J" : "[I";
  if (!strcmp(name, "<init>")) {
    dvm_ref a;
    if (sig[1] == 'I' || sig[1] == 'J') {
      if (ARG(0).i < 0) {
        dvm__throw(vm, "java/lang/NegativeArraySizeException", "%d", ARG(0).i);
        return false;
      }
      a = dvm_new_array(vm, wide ? 'J' : 'I', type, (uint32_t)ARG(0).i);
    } else {
      struct dvm_object *src = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
      if (!src) {
        dvm__throw(vm, "java/lang/NullPointerException", "array");
        return false;
      }
      a = dvm_new_array(vm, wide ? 'J' : 'I', type, src->length);
      struct dvm_object *dst = a ? dvm__obj(vm, a) : NULL;
      src = dvm__obj(vm, ARG(0).l);
      if (dst) memcpy(dst->data, src->data, (size_t)src->length * (wide ? 8u : 4u));
    }
    if (!a) return false;
    (void)dvm_set_field(vm, self, "values", type, (union dvm_value) { .l = a });
    RETV();
  }
  union dvm_value arr = { 0 };
  (void)dvm_get_field(vm, self, "values", type, &arr);
  struct dvm_object *o = arr.l ? dvm__obj(vm, arr.l) : NULL;
  if (!o) {
    dvm__throw(vm, "java/lang/NullPointerException", "values");
    return false;
  }
  if (!strcmp(name, "length")) RETI((int32_t)o->length);
  if (ARG(0).i < 0 || (uint32_t)ARG(0).i >= o->length) {
    dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "index %d", ARG(0).i);
    return false;
  }
  int64_t *w = (int64_t *)o->data;
  int32_t *n = (int32_t *)o->data;
  const unsigned i = (unsigned)ARG(0).i;
  const int64_t cur = wide ? w[i] : n[i];
  /* arguments after the index: one value (set/add/getAndSet), or expect+update (compareAndSet) */
  const int64_t v1 = nargs > 1 ? (wide ? ARG(1).j : (int64_t)ARG(1).i) : 0;
  const int64_t v2 = nargs > 2 ? (wide ? ARG(2).j : (int64_t)ARG(2).i) : 0;
  int64_t next = cur, result = cur;
  if (!strcmp(name, "get") || !strcmp(name, "getPlain") || !strcmp(name, "getAcquire") || !strcmp(name, "getOpaque")) {
  } else if (!strncmp(name, "set", 3) || !strcmp(name, "lazySet")) next = v1, result = 0;
  else if (!strcmp(name, "getAndSet")) next = v1;
  else if (!strncmp(name, "compareAndSet", 13) || !strncmp(name, "weakCompareAndSet", 17)) {
    const bool ok = cur == v1;
    if (ok) next = v2;
    result = ok;
  } else if (!strcmp(name, "getAndIncrement")) next = cur + 1;
  else if (!strcmp(name, "getAndDecrement")) next = cur - 1;
  else if (!strcmp(name, "getAndAdd")) next = cur + v1;
  else if (!strcmp(name, "incrementAndGet")) next = result = cur + 1;
  else if (!strcmp(name, "decrementAndGet")) next = result = cur - 1;
  else if (!strcmp(name, "addAndGet")) next = result = cur + v1;
  else {
    dvm__throw(vm, "java/lang/UnsupportedOperationException", "%s", name);
    return false;
  }
  if (wide) w[i] = next; else n[i] = (int32_t)next;
  const char r = sig[strlen(sig) - 1];
  if (r == 'V') RETV();
  if (r == 'Z') RETI(result != 0);
  if (r == 'J') { out->j = result; return true; }
  RETI((int32_t)result);
}
#define AA(T, N, SIG) M(N, SIG, x_atomic_array)
static const struct rt_method rt_atomic_int_array[] = {
  M("<init>", "(I)V", x_atomic_array), M("<init>", "([I)V", x_atomic_array), M("length", "()I", x_atomic_array),
  M("get", "(I)I", x_atomic_array), M("set", "(II)V", x_atomic_array), M("lazySet", "(II)V", x_atomic_array),
  M("getAndSet", "(II)I", x_atomic_array), M("compareAndSet", "(III)Z", x_atomic_array),
  M("weakCompareAndSet", "(III)Z", x_atomic_array), M("getAndIncrement", "(I)I", x_atomic_array),
  M("getAndDecrement", "(I)I", x_atomic_array), M("getAndAdd", "(II)I", x_atomic_array),
  M("incrementAndGet", "(I)I", x_atomic_array), M("decrementAndGet", "(I)I", x_atomic_array),
  M("addAndGet", "(II)I", x_atomic_array), M_END,
};
static const struct rt_method rt_atomic_long_array[] = {
  M("<init>", "(I)V", x_atomic_array), M("<init>", "([J)V", x_atomic_array), M("length", "()I", x_atomic_array),
  M("get", "(I)J", x_atomic_array), M("set", "(IJ)V", x_atomic_array), M("lazySet", "(IJ)V", x_atomic_array),
  M("getAndSet", "(IJ)J", x_atomic_array), M("compareAndSet", "(IJJ)Z", x_atomic_array),
  M("weakCompareAndSet", "(IJJ)Z", x_atomic_array), M("getAndIncrement", "(I)J", x_atomic_array),
  M("getAndDecrement", "(I)J", x_atomic_array), M("getAndAdd", "(IJ)J", x_atomic_array),
  M("incrementAndGet", "(I)J", x_atomic_array), M("decrementAndGet", "(I)J", x_atomic_array),
  M("addAndGet", "(IJ)J", x_atomic_array), M_END,
};
#undef AA

static const struct rt_field rt_long_adder_fields[] = { { "sum", "J" }, F_END };
static bool x_long_adder(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  union dvm_value sum = { 0 };
  (void)dvm_get_field(vm, self, "sum", "J", &sum);
  if (!strcmp(name, "<init>")) RETV();
  if (!strcmp(name, "add")) sum.j += ARG(0).j;
  else if (!strcmp(name, "increment")) sum.j += 1;
  else if (!strcmp(name, "decrement")) sum.j -= 1;
  else if (!strcmp(name, "reset") || !strcmp(name, "sumThenReset")) {
    (void)dvm_set_field(vm, self, "sum", "J", (union dvm_value) { .j = 0 });
    if (name[0] == 'r') RETV();
    out->j = sum.j;
    return true;
  } else if (!strcmp(name, "sum") || !strcmp(name, "longValue")) { out->j = sum.j; return true; }
  else if (!strcmp(name, "intValue")) RETI((int32_t)sum.j);
  else if (!strcmp(name, "doubleValue")) RETD((double)sum.j);
  else if (!strcmp(name, "floatValue")) RETF((float)sum.j);
  else if (!strcmp(name, "toString")) {
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)sum.j);
    RETL(dvm_new_string(vm, b));
  }
  (void)dvm_set_field(vm, self, "sum", "J", sum);
  RETV();
}
static const struct rt_method rt_long_adder[] = {
  M("<init>", "()V", x_long_adder), M("add", "(J)V", x_long_adder), M("increment", "()V", x_long_adder),
  M("decrement", "()V", x_long_adder), M("reset", "()V", x_long_adder), M("sum", "()J", x_long_adder),
  M("sumThenReset", "()J", x_long_adder), M("longValue", "()J", x_long_adder), M("intValue", "()I", x_long_adder),
  M("floatValue", "()F", x_long_adder), M("doubleValue", "()D", x_long_adder),
  M("toString", "()Ljava/lang/String;", x_long_adder), M_END,
};


/* ShortcutManager.getShortcuts(matchFlags): the dynamic shortcuts when FLAG_MATCH_DYNAMIC (2) is asked for; pinned,
 * manifest and cached shortcuts are not kept by this device, so those flags match nothing. */
static bool x_shortcut_get_shortcuts(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)nargs;
  if (!(ARG(0).i & 2)) RETL(coll_new_list(vm, "Ljava/util/ArrayList;"));
  return am_vcall(vm, self, "getDynamicShortcuts", "()Ljava/util/List;", NULL, 0, out);
}
static const struct rt_method rt_gap_shortcut2[] = {
  M("getShortcuts", "(I)Ljava/util/List;", x_shortcut_get_shortcuts), M_END,
};


/* CertificateFactory.generateCertificates(InputStream): every certificate in the stream — a run of PEM blocks, or
 * concatenated DER — as an immutable-by-convention list. */
static bool x_cf_generate_certificates(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
  (void)self; (void)nargs;
  int n = 0;
  uint8_t *p = cf_slurp(vm, ARG(0).l, &n);
  dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
  if (!list) {
    free(p);
    return false;
  }
  dvm_pin(vm, list);
  bool ok = true;
  size_t at = 0;
  while (ok && p && at < (size_t)n) {
    /* skip whitespace between PEM blocks */
    while (at < (size_t)n && (p[at] == '\n' || p[at] == '\r' || p[at] == ' ' || p[at] == '\t')) ++at;
    if (at >= (size_t)n) break;
    X509 *x = NULL;
    if ((size_t)n - at > 10 && !memcmp(p + at, "-----BEGIN", 10)) {
      BIO *bio = BIO_new_mem_buf(p + at, (int)((size_t)n - at));
      if (bio) {
        x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        if (x) {
          const char *end = strstr((const char *)p + at, "-----END");
          const char *nl = end ? strchr(end, '\n') : NULL;
          at = nl ? (size_t)((const uint8_t *)nl - p) + 1 : (size_t)n;
        }
        BIO_free(bio);
      }
    } else {
      const unsigned char *q = p + at;
      x = d2i_X509(NULL, &q, (long)((size_t)n - at));
      if (x) at = (size_t)(q - p);
    }
    if (!x) {
      dvm__throw(vm, "java/security/cert/CertificateException", "failed to parse certificate");
      ok = false;
      break;
    }
    int len = i2d_X509(x, NULL);
    uint8_t *der = len > 0 ? malloc((size_t)len) : NULL;
    uint8_t *w = der;
    dvm_ref cert = der && i2d_X509(x, &w) == len ? x509_from_der(vm, der, len) : 0;
    free(der);
    X509_free(x);
    union dvm_value item = { .l = cert }, ign = { 0 };
    ok = cert && list_add(vm, list, &item, 1, &ign);
  }
  free(p);
  dvm_unpin(vm, list);
  if (!ok) return false;
  RETL(list);
}
static bool x_gap_instance_of(struct dvm *vm, dvm_ref obj, const char *desc)
{
  struct dvm_class *want = dvm__class_by_desc(vm, desc);
  return want && dvm__class_assignable(vm, dvm_object_class(vm, obj), want);
}
/* ---- PKIX certification path validation: CertPath, PKIXParameters, CertPathValidator ---------------------------- */
/* A CertPath is the ordered chain, target first, root excluded.  Validation is the host's X509_verify_cert against the
 * parameters' trust anchors at the parameters' date (now by default), the way PKIXCertPathValidator checks a path. */
static const struct rt_field rt_cert_path_fields[] = {
  { "certs", "Ljava/util/List;" }, { "type", "Ljava/lang/String;" }, F_END,
};
static bool x_cf_generate_cert_path(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
  (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "certificates");
    return false;
  }
  union dvm_value items = { 0 }, type = { 0 }, copy = { 0 };
  if (!am_vcall(vm, ARG(0).l, "toArray", "()[Ljava/lang/Object;", NULL, 0, &items)) return false;
  uint32_t n = dvm_array_length(vm, items.l);
  dvm_ref list = coll_new_list(vm, "Ljava/util/ArrayList;");
  if (!list) return false;
  for (uint32_t i = 0; i < n; ++i) {
    union dvm_value item = { .l = ((dvm_ref *)dvm_array_data(vm, items.l))[i] }, ign = { 0 };
    if (!item.l || !x_gap_instance_of(vm, item.l, "Ljava/security/cert/X509Certificate;")) {
      dvm__throw(vm, "java/security/cert/CertificateException", "list is empty or contains a non-certificate");
      return false;
    }
    if (!list_add(vm, list, &item, 1, &ign)) return false;
  }
  struct dvm_class *c = dvm_find_class(vm, "java/security/cert/CertPath");
  dvm_ref path = c ? dvm_new_object(vm, c) : 0;
  if (!path) return false;
  copy.l = list;
  (void)dvm_set_field(vm, path, "certs", "Ljava/util/List;", copy);
  (void)am_vcall(vm, self, "getType", "()Ljava/lang/String;", NULL, 0, &type);
  (void)dvm_set_field(vm, path, "type", "Ljava/lang/String;", type);
  RETL(path);
}
static bool x_cert_path_get(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value v = { 0 };
  if (!strcmp(dvm__builtin_method()->name, "getType")) (void)dvm_get_field(vm, self, "type", "Ljava/lang/String;", &v);
  else (void)dvm_get_field(vm, self, "certs", "Ljava/util/List;", &v);
  RETL(v.l);
}
static const struct rt_method rt_cert_path[] = {
  M("getType", "()Ljava/lang/String;", x_cert_path_get),
  M("getCertificates", "()Ljava/util/List;", x_cert_path_get), M_END,
};
/* The built-in factory only ever parses X.509 (getInstance does not hand out any other kind). */
static bool x_cf_get_type(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  RETL(dvm_new_string(vm, "X.509"));
}
static const struct rt_method rt_gap_cert_factory[] = {
  M("getType", "()Ljava/lang/String;", x_cf_get_type),
  M("generateCertificates", "(Ljava/io/InputStream;)Ljava/util/Collection;", x_cf_generate_certificates),
  M("generateCertPath", "(Ljava/util/List;)Ljava/security/cert/CertPath;", x_cf_generate_cert_path), M_END,
};

static const struct rt_field rt_pkix_parameters_fields[] = {
  { "anchors", "Ljava/util/Set;" }, { "date", "Ljava/util/Date;" }, { "revocation", "Z" }, F_END,
};
static bool x_pkix_parameters_init(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  (void)nargs;
  union dvm_value size = { 0 };
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "the trustAnchors parameter must be non-null");
    return false;
  }
  if (!am_vcall(vm, ARG(0).l, "size", "()I", NULL, 0, &size)) return false;
  if (!size.i) {
    dvm__throw(vm, "java/security/InvalidAlgorithmParameterException", "the trustAnchors parameter must be non-empty");
    return false;
  }
  (void)dvm_set_field(vm, self, "anchors", "Ljava/util/Set;", ARG(0));
  rt_set_int(vm, self, "revocation", 1);           /* revocation checking is on unless the caller turns it off */
  RETV();
}
static bool x_pkix_parameters_method(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)nargs;
  const char *name = dvm__builtin_method()->name;
  union dvm_value v = { 0 };
  if (!strcmp(name, "setDate")) { (void)dvm_set_field(vm, self, "date", "Ljava/util/Date;", ARG(0)); RETV(); }
  if (!strcmp(name, "getDate")) { (void)dvm_get_field(vm, self, "date", "Ljava/util/Date;", &v); RETL(v.l); }
  if (!strcmp(name, "setRevocationEnabled")) { rt_set_int(vm, self, "revocation", ARG(0).i); RETV(); }
  if (!strcmp(name, "isRevocationEnabled")) RETI(rt_get_int(vm, self, "revocation"));
  if (!strcmp(name, "getTrustAnchors")) { (void)dvm_get_field(vm, self, "anchors", "Ljava/util/Set;", &v); RETL(v.l); }
  if (!strcmp(name, "setTrustAnchors")) { (void)dvm_set_field(vm, self, "anchors", "Ljava/util/Set;", ARG(0)); RETV(); }
  RETV();                          /* policy and checker settings do not change what the host chain check decides */
}
static const struct rt_method rt_pkix_parameters[] = {
  M("<init>", "(Ljava/util/Set;)V", x_pkix_parameters_init),
  M("setDate", "(Ljava/util/Date;)V", x_pkix_parameters_method), M("getDate", "()Ljava/util/Date;", x_pkix_parameters_method),
  M("setRevocationEnabled", "(Z)V", x_pkix_parameters_method), M("isRevocationEnabled", "()Z", x_pkix_parameters_method),
  M("getTrustAnchors", "()Ljava/util/Set;", x_pkix_parameters_method),
  M("setTrustAnchors", "(Ljava/util/Set;)V", x_pkix_parameters_method),
  M("addCertPathChecker", "(Ljava/security/cert/PKIXCertPathChecker;)V", x_pkix_parameters_method),
  M("setPolicyQualifiersRejected", "(Z)V", x_pkix_parameters_method),
  M("setExplicitPolicyRequired", "(Z)V", x_pkix_parameters_method),
  M("setAnyPolicyInhibited", "(Z)V", x_pkix_parameters_method),
  M("setPolicyMappingInhibited", "(Z)V", x_pkix_parameters_method), M_END,
};

static const struct rt_field rt_pkix_result_fields[] = { { "anchor", "Ljava/security/cert/TrustAnchor;" }, F_END };
static bool x_pkix_result_anchor(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "anchor", "Ljava/security/cert/TrustAnchor;", &v);
  RETL(v.l);
}
static const struct rt_method rt_pkix_result[] = {
  M("getTrustAnchor", "()Ljava/security/cert/TrustAnchor;", x_pkix_result_anchor), M_END,
};

static const struct rt_field rt_cert_path_validator_fields[] = { { "algorithm", "Ljava/lang/String;" }, F_END };
static bool x_cpv_get_instance(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)self; (void)nargs;
  const char *alg = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  if (!alg) {
    dvm__throw(vm, "java/lang/NullPointerException", "null algorithm name");
    return false;
  }
  if (strcasecmp(alg, "PKIX")) {
    dvm__throw(vm, "java/security/NoSuchAlgorithmException", "%s CertPathValidator not available", alg);
    return false;
  }
  struct dvm_class *c = dvm_find_class(vm, "java/security/cert/CertPathValidator");
  dvm_ref v = c ? dvm_new_object(vm, c) : 0;
  if (!v) return false;
  (void)dvm_set_field(vm, v, "algorithm", "Ljava/lang/String;", ARG(0));
  RETL(v);
}
static bool x_cpv_default_type(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  RETL(dvm_new_string(vm, "PKIX"));
}
static bool x_cpv_algorithm(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value v = { 0 };
  (void)dvm_get_field(vm, self, "algorithm", "Ljava/lang/String;", &v);
  RETL(v.l);
}
static bool x_cpv_validate(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)nargs;
  dvm_ref path = ARG(0).l, params = ARG(1).l;
  if (!path || !params) {
    dvm__throw(vm, "java/lang/NullPointerException", "certification path and parameters");
    return false;
  }
  if (!x_gap_instance_of(vm, params, "Ljava/security/cert/PKIXParameters;")) {
    dvm__throw(vm, "java/security/InvalidAlgorithmParameterException", "inappropriate parameters, must be an instance of PKIXParameters");
    return false;
  }
  union dvm_value certs = { 0 }, items = { 0 }, anchors = { 0 }, anchor_items = { 0 }, date = { 0 }, ms = { 0 };
  (void)dvm_get_field(vm, path, "certs", "Ljava/util/List;", &certs);
  (void)dvm_get_field(vm, params, "anchors", "Ljava/util/Set;", &anchors);
  (void)dvm_get_field(vm, params, "date", "Ljava/util/Date;", &date);
  if (!am_vcall(vm, certs.l, "toArray", "()[Ljava/lang/Object;", NULL, 0, &items) ||
      !am_vcall(vm, anchors.l, "toArray", "()[Ljava/lang/Object;", NULL, 0, &anchor_items)) return false;
  const uint32_t n = dvm_array_length(vm, items.l), na = dvm_array_length(vm, anchor_items.l);
  if (!n) {
    dvm__throw(vm, "java/security/cert/CertPathValidatorException", "the certification path is empty");
    return false;
  }
  bool good = false;
  X509_STORE *store = X509_STORE_new();
  STACK_OF(X509) *untrusted = sk_X509_new_null(), *roots = sk_X509_new_null(), *verified = NULL;
  X509_STORE_CTX *ctx = X509_STORE_CTX_new();
  X509 *leaf = NULL;
  if (!store || !untrusted || !roots || !ctx) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "certification path");
    goto done;
  }
  for (uint32_t i = 0; i < na; ++i) {
    union dvm_value cert = { 0 };
    dvm_ref anchor = ((dvm_ref *)dvm_array_data(vm, anchor_items.l))[i];
    (void)dvm_get_field(vm, anchor, "cert", "Ljava/security/cert/X509Certificate;", &cert);
    if (!cert.l) continue;                           /* key-only anchors carry no certificate to chain to */
    X509 *x = trust_parse_certificate(vm, cert.l);
    if (!x) goto done;
    X509_STORE_add_cert(store, x);
    sk_X509_push(roots, x);
  }
  for (uint32_t i = 0; i < n; ++i) {
    X509 *x = trust_parse_certificate(vm, ((dvm_ref *)dvm_array_data(vm, items.l))[i]);
    if (!x) goto done;
    if (!i) leaf = x;
    else if (!sk_X509_push(untrusted, x)) {
      X509_free(x);
      dvm__throw(vm, "java/lang/OutOfMemoryError", "certification path");
      goto done;
    }
  }
  if (!X509_STORE_CTX_init(ctx, store, leaf, untrusted)) {
    dvm__throw(vm, "java/security/cert/CertPathValidatorException", "cannot initialize certification path validation");
    goto done;
  }
  X509_STORE_CTX_set_flags(ctx, X509_V_FLAG_PARTIAL_CHAIN | X509_V_FLAG_TRUSTED_FIRST);
  X509_STORE_CTX_set_verify_cb(ctx, trust_verify_callback);
  X509_VERIFY_PARAM_set_auth_level(X509_STORE_CTX_get0_param(ctx), 1);
  if (date.l) {
    if (!am_vcall(vm, date.l, "getTime", "()J", NULL, 0, &ms)) goto done;
    X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(ctx), (time_t)(ms.j / 1000));
  }
  if (X509_verify_cert(ctx) != 1) {
    dvm__throw(vm, "java/security/cert/CertPathValidatorException", "%s",
               X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)));
    goto done;
  }
  verified = X509_STORE_CTX_get1_chain(ctx);
  dvm_ref matched = 0;
  X509 *top = verified && sk_X509_num(verified) ? sk_X509_value(verified, sk_X509_num(verified) - 1) : NULL;
  for (uint32_t i = 0, k = 0; i < na && top; ++i) {
    union dvm_value cert = { 0 };
    dvm_ref anchor = ((dvm_ref *)dvm_array_data(vm, anchor_items.l))[i];
    (void)dvm_get_field(vm, anchor, "cert", "Ljava/security/cert/X509Certificate;", &cert);
    if (!cert.l) continue;
    if (!X509_cmp(top, sk_X509_value(roots, (int)k++))) { matched = anchor; break; }
  }
  struct dvm_class *rc = dvm_find_class(vm, "java/security/cert/PKIXCertPathValidatorResult");
  dvm_ref result = rc ? dvm_new_object(vm, rc) : 0;
  if (!result) goto done;
  union dvm_value a = { .l = matched };
  (void)dvm_set_field(vm, result, "anchor", "Ljava/security/cert/TrustAnchor;", a);
  out->l = result;
  good = true;
done:
  sk_X509_pop_free(verified, X509_free);
  X509_STORE_CTX_free(ctx);
  sk_X509_pop_free(untrusted, X509_free);
  sk_X509_pop_free(roots, X509_free);
  if (leaf) X509_free(leaf);
  X509_STORE_free(store);
  return good;
}
static const struct rt_method rt_cert_path_validator[] = {
  SM("getInstance", "(Ljava/lang/String;)Ljava/security/cert/CertPathValidator;", x_cpv_get_instance),
  SM("getDefaultType", "()Ljava/lang/String;", x_cpv_default_type),
  M("getAlgorithm", "()Ljava/lang/String;", x_cpv_algorithm),
  M("validate", "(Ljava/security/cert/CertPath;Ljava/security/cert/CertPathParameters;)Ljava/security/cert/CertPathValidatorResult;",
    x_cpv_validate), M_END,
};


/* Context.createDisplayContext(display) / getDisplay(): a context for the same app tied to another display. */
static bool x_context_create_display(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "display must not be null");
    return false;
  }
  dvm_ref wrapper = dvm_new_object(vm, dvm_find_class(vm, "android/content/ContextWrapper"));
  union dvm_value base = { .l = self }, ign = { 0 };
  if (!wrapper) return false;
  dvm_pin(vm, wrapper);
  bool ok = am_vcall(vm, wrapper, "<init>", "(Landroid/content/Context;)V", &base, 1, &ign);
  if (ok) (void)dvm_set_field(vm, wrapper, "display", "Landroid/view/Display;", ARG(0));
  dvm_unpin(vm, wrapper);
  if (!ok) return false;
  RETL(wrapper);
}
static bool x_context_get_display(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value d = { 0 };
  (void)dvm_get_field(vm, self, "display", "Landroid/view/Display;", &d);
  if (d.l) RETL(d.l);
  union dvm_value base = { 0 };
  (void)dvm_get_field(vm, self, "mBase", "Landroid/content/Context;", &base);
  if (base.l) return am_vcall(vm, base.l, "getDisplay", "()Landroid/view/Display;", NULL, 0, out);
  /* an application context sees the default display */
  union dvm_value name = { .l = dvm_new_string(vm, "display") }, dm = { 0 }, zero = { .i = 0 };
  if (!am_vcall(vm, self, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", &name, 1, &dm) || !dm.l)
    RETL(0);
  return am_vcall(vm, dm.l, "getDisplay", "(I)Landroid/view/Display;", &zero, 1, out);
}
static const struct rt_method rt_gap_context_display[] = {
  M("createDisplayContext", "(Landroid/view/Display;)Landroid/content/Context;", x_context_create_display),
  M("getDisplay", "()Landroid/view/Display;", x_context_get_display),
  M_END,
};

/* ActivityManager.setProcessStateSummary(byte[]): kept for the process-exit report. */
static dvm_ref g_process_state_summary;
static bool x_set_process_state_summary(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
  (void)self; (void)nargs; (void)out;
  if (g_process_state_summary) dvm_unpin(vm, g_process_state_summary);
  g_process_state_summary = ARG(0).l;
  if (g_process_state_summary) dvm_pin(vm, g_process_state_summary);
  RETV();
}
static const struct rt_method rt_gap_activity_manager3[] = {
  M("setProcessStateSummary", "([B)V", x_set_process_state_summary), M_END,
};

/* TelephonyManager on a device with no SIM: not roaming; listen() delivers the current call state (idle) at once to
 * a listener that asked for call-state changes (LISTEN_CALL_STATE = 0x20). */
static bool x_tm_roaming(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETI(0);
}
static bool x_tm_listen(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)nargs; (void)out;
  if (ARG(0).l && (ARG(1).i & 0x20)) {
    union dvm_value a[2] = { { .i = 0 }, { .l = dvm_new_string(vm, "") } }, ign = { 0 };
    if (!am_vcall(vm, ARG(0).l, "onCallStateChanged", "(ILjava/lang/String;)V", a, 2, &ign)) return false;
  }
  RETV();
}
static const struct rt_method rt_gap_telephony2[] = {
  M("isNetworkRoaming", "()Z", x_tm_roaming),
  M("listen", "(Landroid/telephony/PhoneStateListener;I)V", x_tm_listen),
  M_END,
};

/* Activity panel callbacks: the options menu is the only panel an Activity populates itself. */
static bool x_activity_create_panel_menu(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                         union dvm_value *out)
{
  (void)nargs;
  if (ARG(0).i != 0) RETI(0);
  return am_vcall(vm, self, "onCreateOptionsMenu", "(Landroid/view/Menu;)Z", &args[1], 1, out);
}
static bool x_activity_prepare_panel(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  (void)nargs;
  if (ARG(0).i != 0 || !ARG(2).l) RETI(1);
  union dvm_value ok = { 0 }, visible = { 0 };
  if (!am_vcall(vm, self, "onPrepareOptionsMenu", "(Landroid/view/Menu;)Z", &args[2], 1, &ok)) return false;
  if (!ok.i) RETI(0);
  if (!am_vcall(vm, ARG(2).l, "hasVisibleItems", "()Z", NULL, 0, &visible)) return false;
  RETI(visible.i);
}
static const struct rt_method rt_gap_activity_panels[] = {
  M("onCreatePanelMenu", "(ILandroid/view/Menu;)Z", x_activity_create_panel_menu),
  M("onPreparePanel", "(ILandroid/view/View;Landroid/view/Menu;)Z", x_activity_prepare_panel),
  /* Window.Callback.onContentChanged(): Activity's body is empty. */
  M("onContentChanged", "()V", empty_void),
  M_END,
};

/* View.setTextDirection / setTextAlignment: stored; the text layout reads them when it measures. */
static bool x_view_text_dir(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  const char *field = strstr(name, "Direction") ? "textDirection" : "textAlignment";
  if (name[0] == 'g') {
    int v = rt_get_int(vm, self, field);
    RETI(v ? v : (field[4] == 'D' ? 0 : 1));       /* TEXT_DIRECTION_INHERIT / TEXT_ALIGNMENT_GRAVITY */
  }
  rt_set_int(vm, self, field, ARG(0).i);
  ui_layout_changed();
  ui_invalidate();
  RETV();
}
static const struct rt_method rt_gap_view_text_dir[] = {
  M("setTextDirection", "(I)V", x_view_text_dir), M("getTextDirection", "()I", x_view_text_dir),
  M("setTextAlignment", "(I)V", x_view_text_dir), M("getTextAlignment", "()I", x_view_text_dir),
  M_END,
};

/* ViewConfiguration.getScaledWindowTouchSlop(): WINDOW_TOUCH_SLOP (16) dp. */
static bool x_vc_window_slop(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  dvm_ref dm = dvm_new_object(vm, dvm_find_class(vm, "android/util/DisplayMetrics"));
  if (!dm) return false;
  dvm_pin(vm, dm);
  rt_fill_display_metrics(vm, dm);
  union dvm_value density = { 0 };
  (void)dvm_get_field(vm, dm, "density", "F", &density);
  dvm_unpin(vm, dm);
  RETI((int32_t)(16.0f * density.f + 0.5f));
}
static const struct rt_method rt_gap_view_config2[] = {
  M("getScaledWindowTouchSlop", "()I", x_vc_window_slop), M_END,
};

/* OverScroller.springBack(startX, startY, minX, maxX, minY, maxY): when the position lies outside the range, scroll
 * back to the nearest edge over the time a 2000 px/s^2 spring takes; false when it was already inside. */
static bool x_overscroller_spring_back(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                       union dvm_value *out)
{
  (void)nargs;
  const int32_t sx = ARG(0).i, sy = ARG(1).i;
  const int32_t fx = sx < ARG(2).i ? ARG(2).i : sx > ARG(3).i ? ARG(3).i : sx;
  const int32_t fy = sy < ARG(4).i ? ARG(4).i : sy > ARG(5).i ? ARG(5).i : sy;
  if (fx == sx && fy == sy) {
    rt_set_int(vm, self, "finished", 1);
    RETI(0);
  }
  const float far_px = (float)(abs(fx - sx) > abs(fy - sy) ? abs(fx - sx) : abs(fy - sy));
  int32_t ms = (int32_t)(1000.0 * sqrt(2.0 * far_px / 2000.0));
  if (ms < 1) ms = 1;
  union dvm_value a[5] = { { .i = sx }, { .i = sy }, { .i = fx - sx }, { .i = fy - sy }, { .i = ms } }, ign = { 0 };
  if (!am_vcall(vm, self, "startScroll", "(IIIII)V", a, 5, &ign)) return false;
  RETI(1);
}
static const struct rt_method rt_gap_overscroller[] = {
  M("springBack", "(IIIIII)Z", x_overscroller_spring_back), M_END,
};


/* ProcessBuilder: the configuration calls a caller chains before start().  The setters answer the builder itself. */
static bool x_pb_config(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  const char *sig = dvm__builtin_method()->sig;
  const bool getter = sig[1] == ')';
  if (!strcmp(name, "redirectErrorStream")) {
    if (getter) return dvm_get_field(vm, self, "redirectErrorStream", "Z", out);
    (void)dvm_set_field(vm, self, "redirectErrorStream", "Z", (union dvm_value) { .i = ARG(0).i != 0 });
    RETL(self);
  }
  if (!strcmp(name, "directory")) {
    if (getter) return dvm_get_field(vm, self, "directory", "Ljava/io/File;", out);
    (void)dvm_set_field(vm, self, "directory", "Ljava/io/File;", ARG(0));
    RETL(self);
  }
  if (!strcmp(name, "environment")) {
    union dvm_value env = { 0 };
    (void)dvm_get_field(vm, self, "environment", "Ljava/util/Map;", &env);
    if (!env.l) {
      env.l = dvm_new_object(vm, dvm_find_class(vm, "java/util/HashMap"));
      union dvm_value ign = { 0 };
      if (!env.l || !am_vcall(vm, env.l, "<init>", "()V", NULL, 0, &ign)) return false;
      (void)dvm_set_field(vm, self, "environment", "Ljava/util/Map;", env);
    }
    RETL(env.l);
  }
  /* command(List) / command(String...) replace the command and answer the builder; command() reads it. */
  union dvm_value current = ARG(0);
  union dvm_value fresh = { 0 };
  const union dvm_value one = { .l = current.l };
  if (!x_pb_init(vm, self, &one, 1, &fresh)) return false;
  RETL(self);
}
static const struct rt_method rt_gap_process_builder[] = {
  M("redirectErrorStream", "(Z)Ljava/lang/ProcessBuilder;", x_pb_config),
  M("redirectErrorStream", "()Z", x_pb_config),
  M("directory", "(Ljava/io/File;)Ljava/lang/ProcessBuilder;", x_pb_config),
  M("directory", "()Ljava/io/File;", x_pb_config),
  M("environment", "()Ljava/util/Map;", x_pb_config),
  M("command", "(Ljava/util/List;)Ljava/lang/ProcessBuilder;", x_pb_config),
  M("command", "([Ljava/lang/String;)Ljava/lang/ProcessBuilder;", x_pb_config),
  M_END,
};


/* TypedValue.getDimension(metrics): this value's packed dimension in pixels. */
static bool x_typed_value_get_dimension(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
  (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "metrics");
    return false;
  }
  RETF(x_complex_to_dimension(vm, rt_get_int(vm, self, "data"), ARG(0).l));
}
static const struct rt_method rt_gap_typed_value[] = {
  M("getDimension", "(Landroid/util/DisplayMetrics;)F", x_typed_value_get_dimension), M_END,
};


/* TextView.setHint(int resid): the string resource, looked up the way setText(int) does. */
static bool x_text_set_hint_res(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)nargs;
  union dvm_value ctx = { 0 }, res = { 0 }, text = { 0 };
  if (!am_vcall(vm, self, "getContext", "()Landroid/content/Context;", NULL, 0, &ctx) ||
      !am_vcall(vm, ctx.l, "getResources", "()Landroid/content/res/Resources;", NULL, 0, &res) ||
      !am_vcall(vm, res.l, "getText", "(I)Ljava/lang/CharSequence;", args, 1, &text)) return false;
  return am_vcall(vm, self, "setHint", "(Ljava/lang/CharSequence;)V", &text, 1, out);
}
static const struct rt_method rt_gap_text_hint_res[] = {
  M("setHint", "(I)V", x_text_set_hint_res), M_END,
};

/* SubscriptionManager.from(context): the context's telephony subscription service. */
static bool x_subscription_from(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)self; (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "context");
    return false;
  }
  union dvm_value name = { .l = dvm_new_string(vm, "telephony_subscription_service") };
  return am_vcall(vm, ARG(0).l, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", &name, 1, out);
}
static const struct rt_method rt_gap_subscription2[] = {
  SM("from", "(Landroid/content/Context;)Landroid/telephony/SubscriptionManager;", x_subscription_from), M_END,
};


/* ---- StringBuilder/StringBuffer.appendCodePoint ----------------------------------------------------------------- */
static bool x_sb_append_code_point(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  (void)nargs;
  const int32_t cp = ARG(0).i;
  if (cp < 0 || cp > 0x10ffff) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "Not a valid Unicode code point: 0x%X", (unsigned)cp);
    return false;
  }
  uint16_t units[2];
  size_t n = 0;
  if (cp >= 0x10000) {
    units[n++] = (uint16_t)(0xd800 + ((cp - 0x10000) >> 10));
    units[n++] = (uint16_t)(0xdc00 + ((cp - 0x10000) & 0x3ff));
  } else units[n++] = (uint16_t)cp;
  char text[8];
  const size_t bytes = jcs_utf16_to_wtf8(units, n, text);
  union dvm_value str = { .l = dvm_new_string_n(vm, text, bytes) };
  if (!str.l) return false;
  return sb_append_str(vm, self, &str, 1, out);
}
static const struct rt_method rt_gap_append_code_point[] = {
  M("appendCodePoint", "(I)Ljava/lang/StringBuilder;", x_sb_append_code_point), M_END,
};
static const struct rt_method rt_gap_append_code_point_buffer[] = {
  M("appendCodePoint", "(I)Ljava/lang/StringBuffer;", x_sb_append_code_point), M_END,
};

/* ---- java.text.Collator ------------------------------------------------------------------------------------------- */
/* A comparison in three layers, as ICU orders: base letters (accents and case removed), then accents, then case with
 * lower case first.  Strength says how many layers count.  Text is first decomposed (NFD), so "é" is "e" + a mark. */
#define COLLATOR_PRIMARY 0
#define COLLATOR_SECONDARY 1
#define COLLATOR_TERTIARY 2
#define COLLATOR_IDENTICAL 3
static const struct rt_field rt_collator_fields[] = { { "strength", "I" }, { "decomposition", "I" }, F_END };
static bool x_collator_decompose(struct dvm *vm, dvm_ref text, char **out_utf8)
{
  /* Normalizer.Form ordinals: NFD 0, NFC 1, NFKD 2, NFKC 3. */
  const char *u = text ? dvm_string_utf8(vm, text) : NULL;
  *out_utf8 = u ? luna_unicode_normalize(u, 0) : NULL;
  if (!*out_utf8) dvm__throw(vm, "java/lang/IllegalArgumentException", "Invalid Unicode input");
  return *out_utf8 != NULL;
}
static uint32_t x_utf8_next(const unsigned char **p)
{
  const unsigned char *s = *p;
  uint32_t c = *s++;
  int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  if (more) c &= (0x3fu >> more);
  while (more-- > 0 && (*s & 0xc0) == 0x80) c = (c << 6) | (*s++ & 0x3f);
  *p = s;
  return c;
}
static uint32_t x_fold_lower(uint32_t c)
{
  if (c >= 'A' && c <= 'Z') return c + 32;
  if ((c >= 0xc0 && c <= 0xde && c != 0xd7)) return c + 32;        /* Latin-1 */
  if (c >= 0x391 && c <= 0x3a9) return c + 32;                      /* Greek */
  if (c >= 0x410 && c <= 0x42f) return c + 32;                      /* Cyrillic */
  if (c >= 0x100 && c <= 0x17f && !(c & 1) && c != 0x138) return c + 1;   /* Latin Extended-A pairs (approx.) */
  return c;
}
static bool x_is_mark(uint32_t c) { return (c >= 0x300 && c <= 0x36f) || (c >= 0x3099 && c <= 0x309a); }
static int x_collator_cmp(const char *a, const char *b, int strength)
{
  /* layer 1: base letters */
  const unsigned char *pa = (const unsigned char *)a, *pb = (const unsigned char *)b;
  for (;;) {
    uint32_t ca = 0, cb = 0;
    while (*pa && x_is_mark(ca = x_utf8_next(&pa))) ca = 0;
    if (!*pa && (ca == 0 || x_is_mark(ca))) ca = 0;
    while (*pb && x_is_mark(cb = x_utf8_next(&pb))) cb = 0;
    if (!*pb && (cb == 0 || x_is_mark(cb))) cb = 0;
    ca = x_fold_lower(ca);
    cb = x_fold_lower(cb);
    if (ca != cb) return ca < cb ? -1 : 1;
    if (!ca) break;
  }
  if (strength == COLLATOR_PRIMARY) return 0;
  /* layer 2: the accents, in order of appearance */
  pa = (const unsigned char *)a;
  pb = (const unsigned char *)b;
  while (*pa || *pb) {
    uint32_t ma = 0, mb = 0;
    while (*pa) { uint32_t c = x_utf8_next(&pa); if (x_is_mark(c)) { ma = c; break; } }
    while (*pb) { uint32_t c = x_utf8_next(&pb); if (x_is_mark(c)) { mb = c; break; } }
    if (ma != mb) return ma < mb ? -1 : 1;
  }
  if (strength == COLLATOR_SECONDARY) return 0;
  /* layer 3: case, lower before upper; IDENTICAL then falls back to the code points themselves */
  pa = (const unsigned char *)a;
  pb = (const unsigned char *)b;
  while (*pa && *pb) {
    uint32_t ca = x_utf8_next(&pa), cb = x_utf8_next(&pb);
    const bool ua = x_fold_lower(ca) != ca, ub = x_fold_lower(cb) != cb;
    if (ua != ub) return ua ? 1 : -1;
  }
  if (strength == COLLATOR_TERTIARY) return 0;
  const int r = strcmp(a, b);
  return r < 0 ? -1 : r > 0;
}
static bool x_collator_get_instance(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  dvm_ref c = dvm_new_object(vm, dvm_find_class(vm, "java/text/Collator"));
  if (!c) return false;
  rt_set_int(vm, c, "strength", COLLATOR_TERTIARY);
  RETL(c);
}
static bool x_collator_compare(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
  (void)nargs;
  if (!ARG(0).l || !ARG(1).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "source or target");
    return false;
  }
  union dvm_value sa = ARG(0), sb = ARG(1);
  if (builtin_sig()[1] == 'L' && builtin_sig()[2] == 'j' && strstr(builtin_sig(), "Object")) {
    union dvm_value in = ARG(0);
    if (!am_vcall(vm, in.l, "toString", "()Ljava/lang/String;", NULL, 0, &sa)) return false;
    in = ARG(1);
    if (!am_vcall(vm, in.l, "toString", "()Ljava/lang/String;", NULL, 0, &sb)) return false;
  }
  char *a = NULL, *b = NULL;
  const bool ok = x_collator_decompose(vm, sa.l, &a) && x_collator_decompose(vm, sb.l, &b);
  const int r = ok ? x_collator_cmp(a, b, rt_get_int(vm, self, "strength")) : 0;
  free(a);
  free(b);
  if (!ok) return false;
  RETI(r);
}
static bool x_collator_props(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  const char *field = strstr(name, "Strength") ? "strength" : "decomposition";
  if (name[0] == 'g') RETI(rt_get_int(vm, self, field));
  if (ARG(0).i < 0 || ARG(0).i > 3) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "Incorrect %s value", strstr(name, "Strength") ? "strength" : "decomposition");
    return false;
  }
  rt_set_int(vm, self, field, ARG(0).i);
  RETV();
}
static bool x_collator_equals(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
  union dvm_value r = { 0 };
  if (!x_collator_compare(vm, self, args, nargs, &r)) return false;
  RETI(r.i == 0);
}
static const struct rt_method rt_collator[] = {
  SM("getInstance", "()Ljava/text/Collator;", x_collator_get_instance),
  SM("getInstance", "(Ljava/util/Locale;)Ljava/text/Collator;", x_collator_get_instance),
  M("compare", "(Ljava/lang/String;Ljava/lang/String;)I", x_collator_compare),
  M("compare", "(Ljava/lang/Object;Ljava/lang/Object;)I", x_collator_compare),
  M("equals", "(Ljava/lang/String;Ljava/lang/String;)Z", x_collator_equals),
  M("setStrength", "(I)V", x_collator_props), M("getStrength", "()I", x_collator_props),
  M("setDecomposition", "(I)V", x_collator_props), M("getDecomposition", "()I", x_collator_props),
  M_END,
};

/* ---- View pressed/hovered/activated state, ViewGroup.focusableViewAvailable, TextView cursor and link colours ---- */
static bool x_view_state_get(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  const char *name = builtin_name();
  if (!strcmp(name, "isPressed")) RETI(ui_int(vm, self, "pressed") != 0);
  if (!strcmp(name, "isHovered")) RETI(ui_int(vm, self, "hovered") != 0);
  RETI(ui_int(vm, self, "activated") != 0);
}
static bool x_group_focusable_available(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
  (void)nargs;
  /* ViewGroup.focusableViewAvailable(v) tells the parent, unless descendants are blocked from focus. */
  union dvm_value parent = { 0 };
  (void)dvm_get_field(vm, self, "parent", "Landroid/view/ViewParent;", &parent);
  if (!parent.l) RETV();
  return am_vcall(vm, parent.l, "focusableViewAvailable", "(Landroid/view/View;)V", args, 1, out);
}
static bool x_text_cursor_props(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  if (!strcmp(name, "isCursorVisible")) RETI(!rt_get_int(vm, self, "cursorHidden"));
  if (!strcmp(name, "setCursorVisible")) {
    rt_set_int(vm, self, "cursorHidden", ARG(0).i ? 0 : 1);
    ui_invalidate();
    RETV();
  }
  if (!strcmp(name, "getLinkTextColors")) return dvm_get_field(vm, self, "linkTextColors", "Landroid/content/res/ColorStateList;", out);
  if (!strcmp(name, "getCurrentLinkTextColor")) RETI(rt_get_int(vm, self, "linkTextColor"));
  /* setLinkTextColor(int) / (ColorStateList) */
  if (builtin_sig()[1] == 'I') {
    rt_set_int(vm, self, "linkTextColor", ARG(0).i);
    (void)dvm_set_field(vm, self, "linkTextColors", "Landroid/content/res/ColorStateList;", (union dvm_value) { 0 });
  } else (void)dvm_set_field(vm, self, "linkTextColors", "Landroid/content/res/ColorStateList;", ARG(0));
  ui_invalidate();
  RETV();
}
static const struct rt_method rt_gap_view_state[] = {
  M("isPressed", "()Z", x_view_state_get), M("isHovered", "()Z", x_view_state_get),
  M("isActivated", "()Z", x_view_state_get), M_END,
};
static const struct rt_method rt_gap_group_focus[] = {
  M("focusableViewAvailable", "(Landroid/view/View;)V", x_group_focusable_available), M_END,
};
static const struct rt_method rt_gap_text_cursor[] = {
  M("setCursorVisible", "(Z)V", x_text_cursor_props), M("isCursorVisible", "()Z", x_text_cursor_props),
  M("setLinkTextColor", "(I)V", x_text_cursor_props),
  M("setLinkTextColor", "(Landroid/content/res/ColorStateList;)V", x_text_cursor_props),
  M("getLinkTextColors", "()Landroid/content/res/ColorStateList;", x_text_cursor_props),
  M("getCurrentLinkTextColor", "()I", x_text_cursor_props),
  M_END,
};

/* ---- Activity options-menu defaults ----------------------------------------------------------------------------- */
static bool x_activity_menu_true(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETI(1);
}
static bool x_activity_menu_false(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETI(0);
}
/* Activity.onMenuItemSelected(featureId, item): options-panel items go to onOptionsItemSelected, context-menu items
 * (feature 6) to onContextItemSelected; anything else is not handled. */
static bool x_activity_menu_item_selected(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                          union dvm_value *out)
{
  (void)nargs;
  if (ARG(0).i == 0)
    return am_vcall(vm, self, "onOptionsItemSelected", "(Landroid/view/MenuItem;)Z", &args[1], 1, out);
  if (ARG(0).i == 6)
    return am_vcall(vm, self, "onContextItemSelected", "(Landroid/view/MenuItem;)Z", &args[1], 1, out);
  RETI(0);
}
static const struct rt_method rt_gap_activity_menus[] = {
  M("onCreateOptionsMenu", "(Landroid/view/Menu;)Z", x_activity_menu_true),
  M("onPrepareOptionsMenu", "(Landroid/view/Menu;)Z", x_activity_menu_true),
  M("onOptionsItemSelected", "(Landroid/view/MenuItem;)Z", x_activity_menu_false),
  M("onContextItemSelected", "(Landroid/view/MenuItem;)Z", x_activity_menu_false),
  M("onMenuItemSelected", "(ILandroid/view/MenuItem;)Z", x_activity_menu_item_selected),
  M_END,
};


/* ---- java.security.Permission / BasicPermission ------------------------------------------------------------------ */
static const struct rt_field rt_permission_name_fields[] = { { "name", "Ljava/lang/String;" }, F_END };
static bool x_permission(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  const char *name = builtin_name();
  if (!strcmp(name, "<init>")) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "name can't be null");
      return false;
    }
    const char *n = dvm_string_utf8(vm, ARG(0).l);
    if (!n || !*n) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "name can't be empty");
      return false;
    }
    (void)dvm_set_field(vm, self, "name", "Ljava/lang/String;", ARG(0));
    RETV();
  }
  union dvm_value mine = { 0 };
  (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &mine);
  const char *n = mine.l ? dvm_string_utf8(vm, mine.l) : "";
  if (!strcmp(name, "getName")) RETL(mine.l);
  if (!strcmp(name, "getActions")) RETL(dvm_new_string(vm, ""));
  if (!strcmp(name, "toString")) {
    char b[512];
    snprintf(b, sizeof b, "(\"%s\" \"%s\")", dvm_class_name(dvm_object_class(vm, self)), n);
    for (char *c = b; *c; ++c) if (*c == '/') *c = '.';
    RETL(dvm_new_string(vm, b));
  }
  if (!strcmp(name, "hashCode")) {
    int32_t h = 0;
    for (const char *c = n; *c; ++c) h = (int32_t)((uint32_t)h * 31u + (unsigned char)*c);
    RETI(h);
  }
  /* equals / implies: the same class and name; implies also takes a trailing ".*" or a lone "*" as a wildcard. */
  if (!ARG(0).l || dvm_object_class(vm, ARG(0).l) != dvm_object_class(vm, self)) RETI(0);
  union dvm_value other = { 0 };
  (void)dvm_get_field(vm, ARG(0).l, "name", "Ljava/lang/String;", &other);
  const char *o = other.l ? dvm_string_utf8(vm, other.l) : "";
  if (!strcmp(name, "equals")) RETI(!strcmp(n, o));
  const size_t len = strlen(n);
  if (!strcmp(n, "*")) RETI(1);
  if (len >= 2 && !strcmp(n + len - 2, ".*")) RETI(!strncmp(n, o, len - 1));
  RETI(!strcmp(n, o));
}
static const struct rt_method rt_permission[] = {
  M("getName", "()Ljava/lang/String;", x_permission), M("getActions", "()Ljava/lang/String;", x_permission),
  M("toString", "()Ljava/lang/String;", x_permission), M("hashCode", "()I", x_permission),
  M("equals", "(Ljava/lang/Object;)Z", x_permission),
  M("implies", "(Ljava/security/Permission;)Z", x_permission), M_END,
};
static const struct rt_method rt_basic_permission[] = {
  M("<init>", "(Ljava/lang/String;)V", x_permission), M("<init>", "(Ljava/lang/String;Ljava/lang/String;)V", x_permission),
  M_END,
};


/* ---- JobWorkItem ------------------------------------------------------------------------------------------------ */
static const struct rt_field rt_job_work_item_fields[] = {
  { "intent", "Landroid/content/Intent;" }, { "deliveryCount", "I" }, { "workId", "I" }, F_END,
};
static bool x_job_work_item(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  if (!strcmp(name, "<init>")) {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "intent");
      return false;
    }
    (void)dvm_set_field(vm, self, "intent", "Landroid/content/Intent;", ARG(0));
    RETV();
  }
  if (!strcmp(name, "getIntent")) return dvm_get_field(vm, self, "intent", "Landroid/content/Intent;", out);
  if (!strcmp(name, "getDeliveryCount")) return dvm_get_field(vm, self, "deliveryCount", "I", out);
  RETI(rt_get_int(vm, self, "workId"));              /* getJobId-style accessors */
}
static const struct rt_method rt_job_work_item[] = {
  M("<init>", "(Landroid/content/Intent;)V", x_job_work_item),
  M("getIntent", "()Landroid/content/Intent;", x_job_work_item),
  M("getDeliveryCount", "()I", x_job_work_item),
  M_END,
};

/* ---- Context.getExternalMediaDirs ------------------------------------------------------------------------------- */
/* <storage>/Android/media/<package>, beside the Android/data/<package> tree the external files dir lives in. */
static bool x_context_external_media_dirs(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                          union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value null_type = { 0 }, files = { 0 }, path = { 0 };
  if (!am_vcall(vm, self, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", &null_type, 1, &files)) return false;
  dvm_ref array = dvm_new_array(vm, 'L', "Ljava/io/File;", files.l ? 1 : 0);
  if (!array) return false;
  if (files.l) {
    if (!am_vcall(vm, files.l, "getAbsolutePath", "()Ljava/lang/String;", NULL, 0, &path)) return false;
    char built[1100];
    const char *orig = dvm_string_utf8(vm, path.l);
    const char *at = strstr(orig, "/Android/data/");
    if (at) {
      const char *pkg = at + 14;
      const char *slash = strchr(pkg, '/');
      snprintf(built, sizeof built, "%.*s/Android/media/%.*s", (int)(at - orig), orig,
               (int)(slash ? slash - pkg : (int)strlen(pkg)), pkg);
    } else snprintf(built, sizeof built, "%s-media", orig);    /* a host-backed external dir has no Android/data tree */
    union dvm_value arg = { .l = dvm_new_string(vm, built) }, file = { 0 }, ign = { 0 };
    dvm_ref f = dvm_new_object(vm, dvm_find_class(vm, "java/io/File"));
    if (!f || !am_vcall(vm, f, "<init>", "(Ljava/lang/String;)V", &arg, 1, &ign)) return false;
    file.l = f;
    (void)am_vcall(vm, f, "mkdirs", "()Z", NULL, 0, &ign);
    ((dvm_ref *)dvm_array_data(vm, array))[0] = f;
  }
  RETL(array);
}
static const struct rt_method rt_gap_context_media[] = {
  M("getExternalMediaDirs", "()[Ljava/io/File;", x_context_external_media_dirs), M_END,
};

/* ---- ShortcutManager.removeLongLivedShortcuts / ConnectivityManager / Network / Message / Runtime ----------------- */
static bool x_shortcut_remove_long_lived(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                         union dvm_value *out)
{
  (void)nargs;
  /* long-lived shortcuts are dynamic shortcuts that also stay for the sharing surfaces; this device keeps them in
   * the one list, so removing them is removing those ids. */
  return am_vcall(vm, self, "removeDynamicShortcuts", "(Ljava/util/List;)V", args, 1, out);
}
static const struct rt_method rt_gap_shortcut3[] = {
  M("removeLongLivedShortcuts", "(Ljava/util/List;)V", x_shortcut_remove_long_lived), M_END,
};
static bool x_cm_register_default_handler(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                          union dvm_value *out)
{
  (void)nargs;
  /* The handler only says which looper the callbacks run on; delivery here is on the registering thread. */
  return am_vcall(vm, self, "registerDefaultNetworkCallback",
                  "(Landroid/net/ConnectivityManager$NetworkCallback;)V", args, 1, out);
}
static const struct rt_method rt_gap_conn3[] = {
  M("registerDefaultNetworkCallback", "(Landroid/net/ConnectivityManager$NetworkCallback;Landroid/os/Handler;)V",
    x_cm_register_default_handler),
  M_END,
};
/* Network.getNetworkHandle(): ((long)netId << 32) | HANDLE_MAGIC.  The one network this device has is netId 100. */
static bool x_network_handle(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETJ(((int64_t)100 << 32) | 0xcafed00dLL);
}
static const struct rt_method rt_gap_network[] = {
  M("getNetworkHandle", "()J", x_network_handle), M_END,
};
/* Message.obtain(orig): a new message with orig's what, arg1, arg2, obj, replyTo, sendingUid, workSourceUid, target,
 * callback and a copy of its data. */
static bool x_message_obtain_copy(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                  union dvm_value *out)
{
  (void)self; (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "orig");
    return false;
  }
  dvm_ref m = msg_new(vm);
  if (!m) RETL(0);
  static const struct { const char *n, *t; } kCopy[] = {
    { "what", "I" }, { "arg1", "I" }, { "arg2", "I" }, { "obj", "Ljava/lang/Object;" },
    { "replyTo", "Landroid/os/Messenger;" }, { "sendingUid", "I" }, { "workSourceUid", "I" },
    { "target", "Landroid/os/Handler;" }, { "callback", "Ljava/lang/Runnable;" },
  };
  for (size_t i = 0; i < sizeof kCopy / sizeof kCopy[0]; ++i) {
    union dvm_value v = { 0 };
    (void)dvm_get_field(vm, ARG(0).l, kCopy[i].n, kCopy[i].t, &v);
    (void)dvm_set_field(vm, m, kCopy[i].n, kCopy[i].t, v);
  }
  union dvm_value data = { 0 };
  (void)dvm_get_field(vm, ARG(0).l, "data", "Landroid/os/Bundle;", &data);
  if (data.l) {
    dvm_pin(vm, m);
    dvm_ref copy = dvm_new_object(vm, dvm_find_class(vm, "android/os/Bundle"));
    union dvm_value src = data, ign = { 0 };
    bool ok = copy && am_vcall(vm, copy, "<init>", "(Landroid/os/Bundle;)V", &src, 1, &ign);
    dvm_unpin(vm, m);
    if (!ok) return false;
    (void)dvm_set_field(vm, m, "data", "Landroid/os/Bundle;", (union dvm_value) { .l = copy });
  }
  RETL(m);
}
static const struct rt_method rt_gap_message2[] = {
  SM("obtain", "(Landroid/os/Message;)Landroid/os/Message;", x_message_obtain_copy), M_END,
};
/* Runtime.runFinalization(): this runtime reclaims objects by its own collector and runs no Java finalizers on
 * demand, so there is nothing queued to run. */
static const struct rt_method rt_gap_runtime2[] = {
  M("runFinalization", "()V", empty_void), M_END,
};

/* ---- MenuInflater ----------------------------------------------------------------------------------------------- */
static const struct rt_field rt_menu_inflater_fields[] = { { "context", "Landroid/content/Context;" }, F_END };
static bool x_menu_inflater_init(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)nargs;
  (void)dvm_set_field(vm, self, "context", "Landroid/content/Context;", ARG(0));
  RETV();
}
/* The attributes of one <item>/<group>, read through the framework's own styleable attribute ids. */
static bool x_menu_attrs(struct dvm *vm, dvm_ref context, dvm_ref parser, const char *const *names, int n,
                         dvm_ref *typed)
{
  dvm_ref ids = dvm_new_array(vm, 'I', "I", (uint32_t)n);
  if (!ids) return false;
  int32_t *v = dvm_array_data(vm, ids);
  for (int i = 0; i < n; ++i) v[i] = (int32_t)arm_exec_framework_resource_id("attr", names[i]);
  dvm_pin(vm, ids);
  union dvm_value a[2] = { { .l = parser }, { .l = ids } }, ta = { 0 };
  const bool ok = am_vcall(vm, context, "obtainStyledAttributes", "(Landroid/util/AttributeSet;[I)Landroid/content/res/TypedArray;", a, 2, &ta);
  dvm_unpin(vm, ids);
  *typed = ta.l;
  return ok;
}
static bool x_menu_ta_int(struct dvm *vm, dvm_ref ta, int idx, int def, int32_t *out)
{
  union dvm_value a[2] = { { .i = idx }, { .i = def } }, r = { 0 };
  if (!am_vcall(vm, ta, "getInt", "(II)I", a, 2, &r)) return false;
  *out = r.i;
  return true;
}
static bool x_menu_ta_bool(struct dvm *vm, dvm_ref ta, int idx, bool def, bool *out)
{
  union dvm_value a[2] = { { .i = idx }, { .i = def } }, r = { 0 };
  if (!am_vcall(vm, ta, "getBoolean", "(IZ)Z", a, 2, &r)) return false;
  *out = r.i != 0;
  return true;
}
enum { MI_ID, MI_TITLE, MI_CONDENSED, MI_ICON, MI_ORDER, MI_CAT, MI_VISIBLE, MI_ENABLED, MI_CHECKABLE, MI_CHECKED, MI_SHOW, MI_COUNT };
static bool x_menu_parse(struct dvm *vm, dvm_ref context, dvm_ref parser, dvm_ref menu)
{
  static const char *const item_attrs[MI_COUNT] = { "id", "title", "titleCondensed", "icon", "orderInCategory",
                                                      "menuCategory", "visible", "enabled", "checkable", "checked",
                                                      "showAsAction" };
  int32_t group = 0, group_order = 0;
  bool group_visible = true, group_enabled = true;
  union dvm_value ev = { 0 }, name = { 0 };
  struct { int32_t id, order, show; dvm_ref title, condensed, icon; bool visible, enabled, checkable, checked, pending; } item = { 0 };
  dvm_ref item_ta = 0;
  for (;;) {
    if (!am_vcall(vm, parser, "next", "()I", NULL, 0, &ev)) return false;
    if (ev.i == 1) return true;                                       /* END_DOCUMENT */
    if (!am_vcall(vm, parser, "getName", "()Ljava/lang/String;", NULL, 0, &name)) return false;
    const char *tag = name.l ? dvm_string_utf8(vm, name.l) : "";
    if (ev.i == 2) {                                                  /* START_TAG */
      if (!strcmp(tag, "menu") && !item.pending) continue;            /* the root */
      if (!strcmp(tag, "group")) {
        static const char *const ga[] = { "id", "orderInCategory", "visible", "enabled" };
        dvm_ref ta = 0;
        if (!x_menu_attrs(vm, context, parser, ga, 4, &ta)) return false;
        bool ok = x_menu_ta_int(vm, ta, 0, 0, &group) && x_menu_ta_int(vm, ta, 1, 0, &group_order) &&
                  x_menu_ta_bool(vm, ta, 2, true, &group_visible) && x_menu_ta_bool(vm, ta, 3, true, &group_enabled);
        (void)am_vcall(vm, ta, "recycle", "()V", NULL, 0, &ev);
        if (!ok) return false;
      } else if (!strcmp(tag, "item")) {
        dvm_ref ta = 0;
        if (!x_menu_attrs(vm, context, parser, item_attrs, MI_COUNT, &ta)) return false;
        union dvm_value r = { 0 }, ix = { 0 };
        item.pending = true;
        bool ok = x_menu_ta_int(vm, ta, MI_ID, 0, &item.id) && x_menu_ta_int(vm, ta, MI_ORDER, group_order, &item.order) &&
                  x_menu_ta_bool(vm, ta, MI_VISIBLE, group_visible, &item.visible) &&
                  x_menu_ta_bool(vm, ta, MI_ENABLED, group_enabled, &item.enabled) &&
                  x_menu_ta_bool(vm, ta, MI_CHECKABLE, false, &item.checkable) &&
                  x_menu_ta_bool(vm, ta, MI_CHECKED, false, &item.checked) && x_menu_ta_int(vm, ta, MI_SHOW, 0, &item.show);
        ix.i = MI_TITLE;
        ok = ok && am_vcall(vm, ta, "getText", "(I)Ljava/lang/CharSequence;", &ix, 1, &r);
        item.title = r.l;
        ix.i = MI_CONDENSED;
        ok = ok && am_vcall(vm, ta, "getText", "(I)Ljava/lang/CharSequence;", &ix, 1, &r);
        item.condensed = r.l;
        ix.i = MI_ICON;
        r.l = 0;
        ok = ok && am_vcall(vm, ta, "getDrawable", "(I)Landroid/graphics/drawable/Drawable;", &ix, 1, &r);
        item.icon = r.l;
        if (item.title) dvm_pin(vm, item.title);
        if (item.condensed) dvm_pin(vm, item.condensed);
        if (item.icon) dvm_pin(vm, item.icon);
        item_ta = ta;
        if (!ok) return false;
      } else if (!strcmp(tag, "menu") && item.pending) {              /* <item><menu>: a submenu */
        union dvm_value a[4] = { { .i = group }, { .i = item.id }, { .i = item.order }, { .l = item.title } }, sub = { 0 };
        if (!am_vcall(vm, menu, "addSubMenu", "(IIILjava/lang/CharSequence;)Landroid/view/SubMenu;", a, 4, &sub) || !sub.l) return false;
        union dvm_value icon = { .l = item.icon }, ign = { 0 };
        if (item.icon) (void)am_vcall(vm, sub.l, "setHeaderIcon", "(Landroid/graphics/drawable/Drawable;)Landroid/view/SubMenu;", &icon, 1, &ign);
        item.pending = false;
        dvm_pin(vm, sub.l);
        const bool ok = x_menu_parse(vm, context, parser, sub.l);
        dvm_unpin(vm, sub.l);
        if (!ok) return false;
      }
    } else if (ev.i == 3) {                                           /* END_TAG */
      if (!strcmp(tag, "menu")) return true;
      if (!strcmp(tag, "group")) {
        group = 0; group_order = 0; group_visible = true; group_enabled = true;
      } else if (!strcmp(tag, "item") && item.pending) {
        union dvm_value a[4] = { { .i = group }, { .i = item.id }, { .i = item.order }, { .l = item.title } }, mi = { 0 }, v = { 0 }, ign = { 0 };
        if (!am_vcall(vm, menu, "add", "(IIILjava/lang/CharSequence;)Landroid/view/MenuItem;", a, 4, &mi) || !mi.l) return false;
        v.l = item.condensed;
        if (item.condensed) (void)am_vcall(vm, mi.l, "setTitleCondensed", "(Ljava/lang/CharSequence;)Landroid/view/MenuItem;", &v, 1, &ign);
        v.l = item.icon;
        if (item.icon) (void)am_vcall(vm, mi.l, "setIcon", "(Landroid/graphics/drawable/Drawable;)Landroid/view/MenuItem;", &v, 1, &ign);
        v.i = item.visible;
        (void)am_vcall(vm, mi.l, "setVisible", "(Z)Landroid/view/MenuItem;", &v, 1, &ign);
        v.i = item.enabled;
        (void)am_vcall(vm, mi.l, "setEnabled", "(Z)Landroid/view/MenuItem;", &v, 1, &ign);
        v.i = item.checkable;
        (void)am_vcall(vm, mi.l, "setCheckable", "(Z)Landroid/view/MenuItem;", &v, 1, &ign);
        v.i = item.checked;
        if (item.checkable) (void)am_vcall(vm, mi.l, "setChecked", "(Z)Landroid/view/MenuItem;", &v, 1, &ign);
        v.i = item.show;
        (void)am_vcall(vm, mi.l, "setShowAsAction", "(I)V", &v, 1, &ign);
        item.pending = false;
        if (item.title) dvm_unpin(vm, item.title);
        if (item.condensed) dvm_unpin(vm, item.condensed);
        if (item.icon) dvm_unpin(vm, item.icon);
        if (item_ta) (void)am_vcall(vm, item_ta, "recycle", "()V", NULL, 0, &ign);
        item_ta = 0;
      }
    }
    if (dvm_exception(vm)) return false;
  }
}
static bool x_menu_inflater_inflate(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                    union dvm_value *out)
{
  (void)nargs;
  if (!ARG(1).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "menu");
    return false;
  }
  union dvm_value context = { 0 }, res = { 0 }, parser = { 0 };
  (void)dvm_get_field(vm, self, "context", "Landroid/content/Context;", &context);
  if (!context.l || !am_vcall(vm, context.l, "getResources", "()Landroid/content/res/Resources;", NULL, 0, &res) ||
      !am_vcall(vm, res.l, "getXml", "(I)Landroid/content/res/XmlResourceParser;", args, 1, &parser) || !parser.l) {
    if (!dvm_exception(vm))
      dvm__throw(vm, "android/view/InflateException", "Error inflating menu XML");
    return false;
  }
  dvm_pin(vm, parser.l);
  const bool ok = x_menu_parse(vm, context.l, parser.l, ARG(1).l);
  union dvm_value ign = { 0 };
  (void)am_vcall(vm, parser.l, "close", "()V", NULL, 0, &ign);
  dvm_unpin(vm, parser.l);
  if (!ok) return false;
  RETV();
}
static const struct rt_method rt_menu_inflater[] = {
  M("<init>", "(Landroid/content/Context;)V", x_menu_inflater_init),
  M("<init>", "(Landroid/content/Context;Ljava/lang/Object;)V", x_menu_inflater_init),
  M("inflate", "(ILandroid/view/Menu;)V", x_menu_inflater_inflate),
  M_END,
};


/* ---- org.xmlpull.v1.XmlSerializer (android.util.Xml.newSerializer) --------------------------------------------- */
/* Output accumulates in a StringBuilder and is written to the OutputStream (as UTF-8 or the named encoding) or the
 * Writer at flush(), as KXmlSerializer buffers. */
static const struct rt_field rt_xml_serializer_fields[] = {
  { "sb", "Ljava/lang/StringBuilder;" }, { "out", "Ljava/lang/Object;" }, { "isWriter", "Z" },
  { "encoding", "Ljava/lang/String;" }, { "tagOpen", "Z" }, { "names", "Ljava/util/List;" },
  { "indent", "Z" }, { "lastEnd", "Z" }, F_END,
};
static const char *const rt_iface_xml_serializer[] = { "Lorg/xmlpull/v1/XmlSerializer;", NULL };
static bool xs_put(struct dvm *vm, dvm_ref self, const char *text)
{
  union dvm_value sb = { 0 }, str = { .l = dvm_new_string(vm, text) }, r = { 0 };
  (void)dvm_get_field(vm, self, "sb", "Ljava/lang/StringBuilder;", &sb);
  if (!sb.l) {
    sb.l = dvm_new_object(vm, dvm_find_class(vm, "java/lang/StringBuilder"));
    union dvm_value ign = { 0 };
    if (!sb.l || !am_vcall(vm, sb.l, "<init>", "()V", NULL, 0, &ign)) return false;
    (void)dvm_set_field(vm, self, "sb", "Ljava/lang/StringBuilder;", sb);
  }
  return str.l && am_vcall(vm, sb.l, "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", &str, 1, &r);
}
/* text with the five XML-special characters escaped; quote is '"' inside attribute values, 0 in text. */
static bool xs_put_escaped(struct dvm *vm, dvm_ref self, const char *s, char quote)
{
  for (const char *p = s; *p;) {
    const char *run = p;
    while (*p && *p != '&' && *p != '<' && *p != '>' && !(quote && *p == quote)) ++p;
    if (p > run) {
      char *chunk = strndup(run, (size_t)(p - run));
      const bool ok = chunk && xs_put(vm, self, chunk);
      free(chunk);
      if (!ok) return false;
    }
    if (*p) {
      const char *rep = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : "&quot;";
      if (!xs_put(vm, self, rep)) return false;
      ++p;
    }
  }
  return true;
}
static bool xs_close_tag(struct dvm *vm, dvm_ref self)
{
  if (!rt_get_int(vm, self, "tagOpen")) return true;
  rt_set_int(vm, self, "tagOpen", 0);
  return xs_put(vm, self, ">");
}
static int xs_depth(struct dvm *vm, dvm_ref self)
{
  union dvm_value names = { 0 };
  (void)dvm_get_field(vm, self, "names", "Ljava/util/List;", &names);
  struct rt_list *l = names.l ? list_of(vm, names.l) : NULL;
  return l ? (int)l->size : 0;
}
static bool xs_newline(struct dvm *vm, dvm_ref self, int depth)
{
  if (!rt_get_int(vm, self, "indent")) return true;
  char b[160] = "\n";
  size_t n = 1;
  for (int i = 0; i < depth && n + 2 < sizeof b; ++i) { b[n++] = ' '; b[n++] = ' '; }
  b[n] = 0;
  return xs_put(vm, self, b);
}
static bool xs_flush(struct dvm *vm, dvm_ref self)
{
  union dvm_value sb = { 0 }, out = { 0 }, text = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "sb", "Ljava/lang/StringBuilder;", &sb);
  (void)dvm_get_field(vm, self, "out", "Ljava/lang/Object;", &out);
  if (!sb.l || !out.l) return true;
  if (!am_vcall(vm, sb.l, "toString", "()Ljava/lang/String;", NULL, 0, &text)) return false;
  struct dvm_object *so = dvm__obj(vm, text.l);
  if (so && !strlen(dvm_string_utf8(vm, text.l))) return true;
  if (rt_get_int(vm, self, "isWriter")) {
    if (!am_vcall(vm, out.l, "write", "(Ljava/lang/String;)V", &text, 1, &ign)) return false;
    (void)am_vcall(vm, out.l, "flush", "()V", NULL, 0, &ign);
  } else {
    union dvm_value enc = { 0 }, bytes = { 0 };
    (void)dvm_get_field(vm, self, "encoding", "Ljava/lang/String;", &enc);
    if (!enc.l) enc.l = dvm_new_string(vm, "UTF-8");
    union dvm_value a[1] = { enc };
    if (!am_vcall(vm, text.l, "getBytes", "(Ljava/lang/String;)[B", a, 1, &bytes)) return false;
    if (!am_vcall(vm, out.l, "write", "([B)V", &bytes, 1, &ign)) return false;
    (void)am_vcall(vm, out.l, "flush", "()V", NULL, 0, &ign);
  }
  union dvm_value zero = { .i = 0 };
  return am_vcall(vm, sb.l, "setLength", "(I)V", &zero, 1, &ign);
}
static bool x_xml_serializer(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)nargs;
  const char *name = builtin_name();
  const char *sig = dvm__builtin_method()->sig;
  const char *a0 = ARG(0).l && sig[1] == 'L' && !strncmp(sig + 1, "Ljava/lang/String;", 18) ? dvm_string_utf8(vm, ARG(0).l) : NULL;
  if (!strcmp(name, "setOutput")) {
    (void)dvm_set_field(vm, self, "out", "Ljava/lang/Object;", ARG(0));
    rt_set_int(vm, self, "isWriter", strstr(sig, "Writer") != NULL);
    if (nargs > 1) (void)dvm_set_field(vm, self, "encoding", "Ljava/lang/String;", ARG(1));
    RETV();
  }
  if (!strcmp(name, "setFeature")) {
    const char *f = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : "";
    if (!strcmp(f, "http://xmlpull.org/v1/doc/features.html#indent-output")) {
      rt_set_int(vm, self, "indent", ARG(1).i != 0);
      RETV();
    }
    dvm__throw(vm, "java/lang/IllegalStateException", "Unsupported feature: %s", f);
    return false;
  }
  if (!strcmp(name, "getFeature")) {
    const char *f = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : "";
    RETI(!strcmp(f, "http://xmlpull.org/v1/doc/features.html#indent-output") && rt_get_int(vm, self, "indent"));
  }
  if (!strcmp(name, "setProperty")) {
    dvm__throw(vm, "java/lang/IllegalStateException", "Unsupported property");
    return false;
  }
  if (!strcmp(name, "getProperty")) RETL(0);
  if (!strcmp(name, "getDepth")) RETI(xs_depth(vm, self));
  if (!strcmp(name, "getName")) {
    union dvm_value names = { 0 };
    (void)dvm_get_field(vm, self, "names", "Ljava/util/List;", &names);
    struct rt_list *l = names.l ? list_of(vm, names.l) : NULL;
    RETL(l && l->size ? l->items[l->size - 1] : 0);
  }
  if (!strcmp(name, "getNamespace") || !strcmp(name, "getPrefix")) RETL(!strcmp(name, "getPrefix") ? dvm_new_string(vm, "") : 0);
  if (!strcmp(name, "setPrefix")) RETV();
  if (!strcmp(name, "startDocument")) {
    const char *enc = a0 ? a0 : "UTF-8";
    if (a0) (void)dvm_set_field(vm, self, "encoding", "Ljava/lang/String;", ARG(0));
    char b[200];
    snprintf(b, sizeof b, "<?xml version='1.0' encoding='%s'%s ?>", enc,
             nargs > 1 && ARG(1).l ? (rt_get_int(vm, ARG(1).l, "value") ? " standalone='yes'" : " standalone='no'") : "");
    if (!xs_put(vm, self, b) || !xs_newline(vm, self, 0)) return false;
    RETV();
  }
  if (!strcmp(name, "flush")) {
    if (!xs_close_tag(vm, self) || !xs_flush(vm, self)) return false;
    RETV();
  }
  if (!strcmp(name, "endDocument")) {
    while (xs_depth(vm, self)) {
      union dvm_value none[2] = { { 0 }, { 0 } };
      if (!am_vcall(vm, self, "endTag", "(Ljava/lang/String;Ljava/lang/String;)Lorg/xmlpull/v1/XmlSerializer;", none, 2, out)) return false;
    }
    if (!xs_close_tag(vm, self) || !xs_flush(vm, self)) return false;
    RETV();
  }
  if (!strcmp(name, "startTag")) {
    const char *tag = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL;
    if (!tag) {
      dvm__throw(vm, "java/lang/NullPointerException", "name");
      return false;
    }
    union dvm_value names = { 0 }, item = { .l = ARG(1).l }, ign = { 0 };
    (void)dvm_get_field(vm, self, "names", "Ljava/util/List;", &names);
    if (!names.l) {
      names.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      if (!names.l) return false;
      (void)dvm_set_field(vm, self, "names", "Ljava/util/List;", names);
    }
    const int depth = xs_depth(vm, self);
    if (!xs_close_tag(vm, self) || (depth || rt_get_int(vm, self, "lastEnd") ? !xs_newline(vm, self, depth) : false) ||
        !xs_put(vm, self, "<") || !xs_put(vm, self, tag) || !list_add(vm, names.l, &item, 1, &ign)) return false;
    rt_set_int(vm, self, "tagOpen", 1);
    rt_set_int(vm, self, "lastEnd", 0);
    RETL(self);
  }
  if (!strcmp(name, "attribute")) {
    if (!rt_get_int(vm, self, "tagOpen")) {
      dvm__throw(vm, "java/lang/IllegalStateException", "illegal position for attribute");
      return false;
    }
    const char *key = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL, *val = ARG(2).l ? dvm_string_utf8(vm, ARG(2).l) : "";
    if (!key || !xs_put(vm, self, " ") || !xs_put(vm, self, key) || !xs_put(vm, self, "=\"") ||
        !xs_put_escaped(vm, self, val, '"') || !xs_put(vm, self, "\"")) return false;
    RETL(self);
  }
  if (!strcmp(name, "endTag")) {
    union dvm_value names = { 0 };
    (void)dvm_get_field(vm, self, "names", "Ljava/util/List;", &names);
    struct rt_list *l = names.l ? list_of(vm, names.l) : NULL;
    if (!l || !l->size) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "no start tag to end");
      return false;
    }
    const char *tag = dvm_string_utf8(vm, l->items[l->size - 1]);
    if (!tag) return false;
    const int depth = (int)l->size - 1;
    if (rt_get_int(vm, self, "tagOpen")) {
      rt_set_int(vm, self, "tagOpen", 0);
      if (!xs_put(vm, self, " />")) return false;
    } else {
      if (rt_get_int(vm, self, "lastEnd") && !xs_newline(vm, self, depth)) return false;
      if (!xs_put(vm, self, "</") || !xs_put(vm, self, tag) || !xs_put(vm, self, ">")) return false;
    }
    l->size--;
    rt_set_int(vm, self, "lastEnd", 1);
    RETL(self);
  }
  if (!strcmp(name, "text") || !strcmp(name, "ignorableWhitespace")) {
    const char *t;
    char *copy = NULL;
    if (sig[1] == '[') {                                       /* text(char[], start, len) */
      struct dvm_object *arr = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
      if (!arr || ARG(1).i < 0 || ARG(2).i < 0 || (uint64_t)ARG(1).i + (uint64_t)ARG(2).i > arr->length) {
        dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "text range");
        return false;
      }
      uint16_t *u = (uint16_t *)arr->data + ARG(1).i;
      copy = malloc((size_t)ARG(2).i * 3 + 1);
      if (!copy) return false;
      copy[jcs_utf16_to_wtf8(u, (size_t)ARG(2).i, copy)] = 0;
      t = copy;
    } else t = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : "";
    const bool ok = xs_close_tag(vm, self) && (!strcmp(name, "text") ? xs_put_escaped(vm, self, t, 0) : xs_put(vm, self, t));
    free(copy);
    if (!ok) return false;
    rt_set_int(vm, self, "lastEnd", 0);
    RETL(self);
  }
  /* cdsect / comment / docdecl / entityRef / processingInstruction: the delimiters around the text. */
  const char *open = !strcmp(name, "cdsect") ? "<![CDATA[" : !strcmp(name, "comment") ? "<!--"
                     : !strcmp(name, "docdecl") ? "<!DOCTYPE" : !strcmp(name, "entityRef") ? "&" : "<?";
  const char *close = !strcmp(name, "cdsect") ? "]]>" : !strcmp(name, "comment") ? "-->"
                      : !strcmp(name, "docdecl") ? ">" : !strcmp(name, "entityRef") ? ";" : "?>";
  const bool pad = !strcmp(name, "docdecl");
  if (!xs_close_tag(vm, self) || !xs_put(vm, self, open) || (pad && !xs_put(vm, self, " ")) ||
      !xs_put(vm, self, a0 ? a0 : "") || !xs_put(vm, self, close)) return false;
  RETV();
}
#define XS_M(n, s) M(n, s, x_xml_serializer)
static const struct rt_method rt_xml_serializer[] = {
  M("<init>", "()V", empty_void),
  XS_M("setOutput", "(Ljava/io/OutputStream;Ljava/lang/String;)V"), XS_M("setOutput", "(Ljava/io/Writer;)V"),
  XS_M("setFeature", "(Ljava/lang/String;Z)V"), XS_M("getFeature", "(Ljava/lang/String;)Z"),
  XS_M("setProperty", "(Ljava/lang/String;Ljava/lang/Object;)V"), XS_M("getProperty", "(Ljava/lang/String;)Ljava/lang/Object;"),
  XS_M("getDepth", "()I"), XS_M("getName", "()Ljava/lang/String;"), XS_M("getNamespace", "()Ljava/lang/String;"),
  XS_M("getPrefix", "(Ljava/lang/String;Z)Ljava/lang/String;"), XS_M("setPrefix", "(Ljava/lang/String;Ljava/lang/String;)V"),
  XS_M("startDocument", "(Ljava/lang/String;Ljava/lang/Boolean;)V"), XS_M("endDocument", "()V"), XS_M("flush", "()V"),
  XS_M("startTag", "(Ljava/lang/String;Ljava/lang/String;)Lorg/xmlpull/v1/XmlSerializer;"),
  XS_M("attribute", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Lorg/xmlpull/v1/XmlSerializer;"),
  XS_M("endTag", "(Ljava/lang/String;Ljava/lang/String;)Lorg/xmlpull/v1/XmlSerializer;"),
  XS_M("text", "(Ljava/lang/String;)Lorg/xmlpull/v1/XmlSerializer;"), XS_M("text", "([CII)Lorg/xmlpull/v1/XmlSerializer;"),
  XS_M("cdsect", "(Ljava/lang/String;)V"), XS_M("comment", "(Ljava/lang/String;)V"), XS_M("docdecl", "(Ljava/lang/String;)V"),
  XS_M("entityRef", "(Ljava/lang/String;)V"), XS_M("processingInstruction", "(Ljava/lang/String;)V"),
  XS_M("ignorableWhitespace", "(Ljava/lang/String;)V"),
  M_END,
};
static bool x_xml_new_serializer(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  RETL(dvm_new_object(vm, dvm_find_class(vm, "org/kxml2/io/KXmlSerializer")));
}
static const struct rt_method rt_gap_xml_util[] = {
  SM("newSerializer", "()Lorg/xmlpull/v1/XmlSerializer;", x_xml_new_serializer), M_END,
};


/* ---- Small platform answers for a device with no device owner, no statsd and one network -------------------------- */
static bool x_ret_null(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs;
  RETL(0);
}
static const struct rt_method rt_gap_device_policy2[] = {
  /* a personal device: no device owner and no managed profile */
  M("getDeviceOwnerComponentOnAnyUser", "()Landroid/content/ComponentName;", x_ret_null),
  M("isOrganizationOwnedDeviceWithManagedProfile", "()Z", ret_false),
  M_END,
};
static bool x_component_short(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value pkg = { 0 }, cls = { 0 };
  (void)dvm_get_field(vm, self, "mPackage", "Ljava/lang/String;", &pkg);
  (void)dvm_get_field(vm, self, "mClass", "Ljava/lang/String;", &cls);
  char b[512];
  snprintf(b, sizeof b, "{%s/%s}", pkg.l ? dvm_string_utf8(vm, pkg.l) : "", cls.l ? dvm_string_utf8(vm, cls.l) : "");
  RETL(dvm_new_string(vm, b));
}
static const struct rt_method rt_gap_component2[] = {
  M("toShortString", "()Ljava/lang/String;", x_component_short), M_END,
};
/* Context.checkUriPermission(uri, pid, uid, flags): an app may touch its own URIs; any other uid holds no grant here. */
static bool x_check_uri_permission(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)nargs;
  union dvm_value uid = { 0 };
  if (!x_call_static(vm, "android/os/Process", "myUid", "()I", NULL, 0, &uid)) return false;
  RETI(ARG(2).i == uid.i || ARG(2).i == 0 ? 0 : -1);           /* PERMISSION_GRANTED / PERMISSION_DENIED */
}
static const struct rt_method rt_gap_context_uri[] = {
  M("checkUriPermission", "(Landroid/net/Uri;III)I", x_check_uri_permission), M_END,
};
static const struct rt_method rt_gap_appinfo2[] = {
  M("isInstantApp", "()Z", ret_false), M_END,                  /* an installed app, not an instant app */
};
static const struct rt_method rt_gap_environment2[] = {
  SM("isExternalStorageLegacy", "()Z", ret_false), M_END,      /* scoped storage is enforced */
};
static const struct rt_method rt_gap_telephony3[] = {
  M("getCarrierIdFromSimMccMnc", "()I", x_tm_unknown_carrier), M_END,
};
static bool x_conn_register_request_handler(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                            union dvm_value *out)
{
  (void)nargs;
  return am_vcall(vm, self, "registerNetworkCallback",
                  "(Landroid/net/NetworkRequest;Landroid/net/ConnectivityManager$NetworkCallback;)V", args, 2, out);
}
static const struct rt_method rt_gap_conn4[] = {
  M("registerNetworkCallback",
    "(Landroid/net/NetworkRequest;Landroid/net/ConnectivityManager$NetworkCallback;Landroid/os/Handler;)V",
    x_conn_register_request_handler),
  M_END,
};
/* statsd is a system daemon this device does not run; StatsLog.write() hands the event to it and returns. */
static const struct rt_method rt_gap_stats_log[] = {
  SM("write", "(Landroid/util/StatsEvent;)V", empty_void), SM("write", "(Landroid/util/StatsEvent;)Ljava/lang/Object;", x_ret_null), M_END,
};
static bool x_stats_annotation(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)args; (void)nargs;
  RETL(self);
}
static const struct rt_method rt_gap_stats_builder2[] = {
  M("addBooleanAnnotation", "(BZ)Landroid/util/StatsEvent$Builder;", x_stats_annotation),
  M("addIntAnnotation", "(BI)Landroid/util/StatsEvent$Builder;", x_stats_annotation),
  M_END,
};
/* SQLiteQueryBuilder strictness only widens what setStrict* rejects; the flags are kept for getters. */
static bool x_qb_strict(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)vm; (void)self; (void)args; (void)nargs; (void)out;
  RETV();
}
static const struct rt_method rt_gap_query_builder2[] = {
  M("setStrict", "(Z)V", x_qb_strict), M("setStrictColumns", "(Z)V", x_qb_strict), M("setStrictGrammar", "(Z)V", x_qb_strict),
  M_END,
};
/* SQLiteDatabase.acquireReference(): a reference on an open database; a closed one cannot be acquired. */
static bool x_db_acquire(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value open = { 0 };
  if (!am_vcall(vm, self, "isOpen", "()Z", NULL, 0, &open)) return false;
  if (!open.i) {
    dvm__throw(vm, "java/lang/IllegalStateException", "attempt to re-open an already-closed object: SQLiteDatabase");
    return false;
  }
  RETV();
}
static const struct rt_method rt_gap_sqlite_db2[] = {
  M("acquireReference", "()V", x_db_acquire), M_END,
};
static bool x_key_factory_provider(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)nargs;
  /* the provider only says who implements the algorithm; the runtime has the one */
  return x_call_static(vm, "java/security/KeyFactory", "getInstance", "(Ljava/lang/String;)Ljava/security/KeyFactory;", args, 1, out);
}
static const struct rt_method rt_gap_key_factory2[] = {
  SM("getInstance", "(Ljava/lang/String;Ljava/security/Provider;)Ljava/security/KeyFactory;", x_key_factory_provider), M_END,
};
static const struct rt_method rt_gap_shared_secrets[] = {
  SM("getJavaLangAccess", "()Ljava/lang/Object;", x_ret_null), M_END,    /* Android never installs one */
};
static const struct rt_method rt_gap_socket_address[] = {
  M("<init>", "()V", empty_void), M_END,                       /* SocketAddress() has an empty body */
};

/* ---- PackageManager.PackageInfoFlags / PackageInstaller --------------------------------------------------------- */
static const struct rt_field rt_pm_flags_fields[] = { { "value", "J" }, F_END };
static bool x_pm_flags(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "getValue")) return dvm_get_field(vm, self, "value", "J", out);
  dvm_ref f = dvm_new_object(vm, dvm_find_class(vm, "android/content/pm/PackageManager$PackageInfoFlags"));
  if (!f) return false;
  (void)dvm_set_field(vm, f, "value", "J", ARG(0));
  RETL(f);
}
static const struct rt_method rt_pm_flags[] = {
  SM("of", "(J)Landroid/content/pm/PackageManager$PackageInfoFlags;", x_pm_flags), M("getValue", "()J", x_pm_flags), M_END,
};
static bool x_installer_empty_list(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  RETL(coll_new_list(vm, "Ljava/util/ArrayList;"));
}
static bool x_pm_get_installer(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  RETL(dvm_new_object(vm, dvm_find_class(vm, "android/content/pm/PackageInstaller")));
}
static const struct rt_method rt_gap_pm_installer[] = {
  M("getPackageInstaller", "()Landroid/content/pm/PackageInstaller;", x_pm_get_installer), M_END,
};
/* No installation session exists on this device, so the session lists are empty and no session info resolves. */
static const struct rt_method rt_package_installer[] = {
  M("getAllSessions", "()Ljava/util/List;", x_installer_empty_list),
  M("getMySessions", "()Ljava/util/List;", x_installer_empty_list),
  M("getStagedSessions", "()Ljava/util/List;", x_installer_empty_list),
  M("getSessionInfo", "(I)Landroid/content/pm/PackageInstaller$SessionInfo;", x_ret_null),
  M("registerSessionCallback", "(Landroid/content/pm/PackageInstaller$SessionCallback;)V", empty_void),
  M("registerSessionCallback", "(Landroid/content/pm/PackageInstaller$SessionCallback;Landroid/os/Handler;)V", empty_void),
  M("unregisterSessionCallback", "(Landroid/content/pm/PackageInstaller$SessionCallback;)V", empty_void),
  M_END,
};

/* ---- java.io.PipedInputStream / PipedOutputStream --------------------------------------------------------------- */
/* A bounded ring buffer between a reader thread and a writer thread; either side blocks, as Java's does. */
static const struct rt_field rt_piped_in_fields[] = {
  { "buf", "[B" }, { "head", "I" }, { "count", "I" }, { "writerClosed", "Z" }, { "readerClosed", "Z" }, { "connected", "Z" }, F_END,
};
static const struct rt_field rt_piped_out_fields[] = { { "sink", "Ljava/io/PipedInputStream;" }, F_END };
static bool pipe_wait(struct dvm *vm, dvm_ref pipe)
{
  if (dvm_is_stopping(vm)) return false;
  if (dvm__thread_take_interrupt(vm)) {
    dvm__throw(vm, "java/io/InterruptedIOException", "pipe wait interrupted");
    return false;
  }
  dvm_ref thread = dvm__current_thread(vm);
  dvm__thread_set_state(vm, thread, DVM_THREAD_WAITING);
  dvm__thread_set_wait(vm, (uintptr_t)pipe);
  dvm_gil_wait_for_ns(vm, pipe, 1000000000ull);                 /* wake to re-check interrupts at least once a second */
  dvm__thread_set_wait(vm, 0);
  dvm__thread_set_state(vm, thread, DVM_THREAD_RUNNABLE);
  return true;
}
static bool pipe_ensure(struct dvm *vm, dvm_ref self, int32_t size)
{
  union dvm_value b = { 0 };
  (void)dvm_get_field(vm, self, "buf", "[B", &b);
  if (b.l) return true;
  if (size <= 0) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "Pipe Size <= 0");
    return false;
  }
  b.l = dvm_new_array(vm, 'B', "B", (uint32_t)size);
  if (!b.l) return false;
  (void)dvm_set_field(vm, self, "buf", "[B", b);
  return true;
}
static bool x_piped_in(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  const char *name = builtin_name();
  const char *sig = dvm__builtin_method()->sig;
  if (!strcmp(name, "<init>")) {
    int32_t size = 1024, source_index = -1;
    if (sig[1] == 'I') size = ARG(0).i;
    else if (sig[1] == 'L') source_index = 0;                       /* (PipedOutputStream[, int]) connects at once */
    if (nargs > 1) size = ARG(1).i;
    if (!pipe_ensure(vm, self, size)) return false;
    if (source_index == 0 && ARG(0).l) {
      union dvm_value sink = { .l = self }, ign = { 0 };
      if (!am_vcall(vm, ARG(0).l, "connect", "(Ljava/io/PipedInputStream;)V", &sink, 1, &ign)) return false;
    }
    RETV();
  }
  if (!strcmp(name, "connect")) {
    if (rt_get_int(vm, self, "connected")) {
      dvm__throw(vm, "java/io/IOException", "Already connected");
      return false;
    }
    if (!pipe_ensure(vm, self, 1024)) return false;
    rt_set_int(vm, self, "connected", 1);
    RETV();
  }
  union dvm_value bufv = { 0 };
  (void)dvm_get_field(vm, self, "buf", "[B", &bufv);
  struct dvm_object *b = bufv.l ? dvm__obj(vm, bufv.l) : NULL;
  if (!strcmp(name, "close")) {
    rt_set_int(vm, self, "readerClosed", 1);
    dvm_gil_notify_for((uintptr_t)self);
    RETV();
  }
  if (!b || !rt_get_int(vm, self, "connected")) {
    dvm__throw(vm, "java/io/IOException", "Pipe not connected");
    return false;
  }
  const int32_t cap = (int32_t)b->length;
  if (!strcmp(name, "available")) RETI(rt_get_int(vm, self, "count"));
  if (!strcmp(name, "receive")) {                                  /* receive(int b) / receive(byte[],off,len): the writer's side */
    const bool one = sig[1] == 'I';
    const uint8_t *src = NULL;
    int32_t len = 1;
    uint8_t single = (uint8_t)ARG(0).i;
    if (!one) {
      struct dvm_object *a = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
      if (!a || ARG(1).i < 0 || ARG(2).i < 0 || (uint64_t)ARG(1).i + (uint64_t)ARG(2).i > a->length) {
        dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "pipe write range");
        return false;
      }
      src = (const uint8_t *)a->data + ARG(1).i;
      len = ARG(2).i;
    } else src = &single;
    while (len > 0) {
      if (rt_get_int(vm, self, "readerClosed") || rt_get_int(vm, self, "writerClosed")) {
        dvm__throw(vm, "java/io/IOException", "Pipe closed");
        return false;
      }
      int32_t count = rt_get_int(vm, self, "count");
      if (count == cap) {
        if (!pipe_wait(vm, self)) return false;
        b = dvm__obj(vm, bufv.l);
        continue;
      }
      const int32_t head = rt_get_int(vm, self, "head");
      int32_t take = cap - count < len ? cap - count : len;
      for (int32_t i = 0; i < take; ++i) ((uint8_t *)b->data)[(head + count + i) % cap] = src[i];
      rt_set_int(vm, self, "count", count + take);
      src += take;
      len -= take;
      dvm_gil_notify_for((uintptr_t)self);
    }
    RETV();
  }
  if (!strcmp(name, "receivedLast")) {
    rt_set_int(vm, self, "writerClosed", 1);
    dvm_gil_notify_for((uintptr_t)self);
    RETV();
  }
  /* read() / read(byte[]) / read(byte[],off,len): block for the first byte, then take what is there. */
  const bool one = sig[1] == ')';
  struct dvm_object *dst = NULL;
  int32_t off = 0, want = 1;
  if (!one) {
    dst = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
    if (!dst) {
      dvm__throw(vm, "java/lang/NullPointerException", "b");
      return false;
    }
    off = nargs > 1 ? ARG(1).i : 0;
    want = nargs > 2 ? ARG(2).i : (int32_t)dst->length;
    if (off < 0 || want < 0 || (uint64_t)off + (uint64_t)want > dst->length) {
      dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "pipe read range");
      return false;
    }
    if (!want) RETI(0);
  }
  for (;;) {
    if (rt_get_int(vm, self, "readerClosed")) {
      dvm__throw(vm, "java/io/IOException", "Pipe closed");
      return false;
    }
    const int32_t count = rt_get_int(vm, self, "count");
    if (count > 0) {
      const int32_t head = rt_get_int(vm, self, "head");
      const int32_t n = count < want ? count : want;
      for (int32_t i = 0; i < n; ++i) {
        const uint8_t byte = ((uint8_t *)b->data)[(head + i) % cap];
        if (one) {
          rt_set_int(vm, self, "head", (head + 1) % cap);
          rt_set_int(vm, self, "count", count - 1);
          dvm_gil_notify_for((uintptr_t)self);
          RETI(byte);
        }
        ((uint8_t *)dst->data)[off + i] = byte;
      }
      rt_set_int(vm, self, "head", (head + n) % cap);
      rt_set_int(vm, self, "count", count - n);
      dvm_gil_notify_for((uintptr_t)self);
      RETI(n);
    }
    if (rt_get_int(vm, self, "writerClosed")) RETI(-1);
    if (!pipe_wait(vm, self)) return false;
    b = dvm__obj(vm, bufv.l);
  }
}
static const struct rt_method rt_piped_in[] = {
  M("<init>", "()V", x_piped_in), M("<init>", "(I)V", x_piped_in),
  M("<init>", "(Ljava/io/PipedOutputStream;)V", x_piped_in), M("<init>", "(Ljava/io/PipedOutputStream;I)V", x_piped_in),
  M("connect", "(Ljava/io/PipedOutputStream;)V", x_piped_in), M("read", "()I", x_piped_in), M("read", "([BII)I", x_piped_in),
  M("read", "([B)I", x_piped_in), M("available", "()I", x_piped_in), M("close", "()V", x_piped_in),
  M("receive", "(I)V", x_piped_in), M("receive", "([BII)V", x_piped_in), M("receivedLast", "()V", x_piped_in),
  M_END,
};
static bool x_piped_out(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  const char *name = builtin_name();
  const char *sig = dvm__builtin_method()->sig;
  union dvm_value sink = { 0 }, ign = { 0 };
  if (!strcmp(name, "<init>") || !strcmp(name, "connect")) {
    if (!nargs || !ARG(0).l) {
      if (nargs) {
        dvm__throw(vm, "java/lang/NullPointerException", "snk");
        return false;
      }
      RETV();
    }
    (void)dvm_get_field(vm, self, "sink", "Ljava/io/PipedInputStream;", &sink);
    if (sink.l) {
      dvm__throw(vm, "java/io/IOException", "Already connected");
      return false;
    }
    if (!am_vcall(vm, ARG(0).l, "connect", "(Ljava/io/PipedOutputStream;)V", &(union dvm_value) { .l = self }, 1, &ign)) return false;
    (void)dvm_set_field(vm, self, "sink", "Ljava/io/PipedInputStream;", ARG(0));
    RETV();
  }
  (void)dvm_get_field(vm, self, "sink", "Ljava/io/PipedInputStream;", &sink);
  if (!strcmp(name, "close")) {
    if (sink.l) return am_vcall(vm, sink.l, "receivedLast", "()V", NULL, 0, out);
    RETV();
  }
  if (!sink.l) {
    dvm__throw(vm, "java/io/IOException", "Pipe not connected");
    return false;
  }
  if (!strcmp(name, "flush")) {
    dvm_gil_notify_for((uintptr_t)sink.l);
    RETV();
  }
  if (sig[1] == 'I') return am_vcall(vm, sink.l, "receive", "(I)V", args, 1, out);
  union dvm_value a[3] = { ARG(0), { .i = nargs > 1 ? ARG(1).i : 0 }, { .i = 0 } };
  struct dvm_object *arr = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
  a[2].i = nargs > 2 ? ARG(2).i : arr ? (int32_t)arr->length : 0;
  return am_vcall(vm, sink.l, "receive", "([BII)V", a, 3, out);
}
static const struct rt_method rt_piped_out[] = {
  M("<init>", "()V", x_piped_out), M("<init>", "(Ljava/io/PipedInputStream;)V", x_piped_out),
  M("connect", "(Ljava/io/PipedInputStream;)V", x_piped_out), M("write", "(I)V", x_piped_out),
  M("write", "([BII)V", x_piped_out), M("write", "([B)V", x_piped_out), M("flush", "()V", x_piped_out),
  M("close", "()V", x_piped_out), M_END,
};


/* TextView.getSelectionStart()/End(): the caret of the text, as android.text.Selection holds it (-1 when none). */
static bool x_text_selection(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value text = { 0 };
  if (!am_vcall(vm, self, "getText", "()Ljava/lang/CharSequence;", NULL, 0, &text)) return false;
  if (!text.l) RETI(-1);
  const bool end = !strcmp(builtin_name(), "getSelectionEnd");
  return x_call_static(vm, "android/text/Selection", end ? "getSelectionEnd" : "getSelectionStart",
                       "(Ljava/lang/CharSequence;)I", &text, 1, out);
}
static const struct rt_method rt_gap_text_selection[] = {
  M("getSelectionStart", "()I", x_text_selection), M("getSelectionEnd", "()I", x_text_selection), M_END,
};
/* ActivityManager.getLauncherLargeIconSize(): the 48 dp launcher icon in pixels. */
static bool x_launcher_icon_size(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)args; (void)nargs;
  dvm_ref dm = dvm_new_object(vm, dvm_find_class(vm, "android/util/DisplayMetrics"));
  if (!dm) return false;
  dvm_pin(vm, dm);
  rt_fill_display_metrics(vm, dm);
  union dvm_value density = { 0 };
  (void)dvm_get_field(vm, dm, "density", "F", &density);
  dvm_unpin(vm, dm);
  RETI((int32_t)(48.0f * density.f + 0.5f));
}
static const struct rt_method rt_gap_activity_manager4[] = {
  M("getLauncherLargeIconSize", "()I", x_launcher_icon_size), M_END,
};
static bool x_signature_provider(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)self; (void)nargs;
  return x_call_static(vm, "java/security/Signature", "getInstance", "(Ljava/lang/String;)Ljava/security/Signature;", args, 1, out);
}
static const struct rt_method rt_gap_signature2[] = {
  SM("getInstance", "(Ljava/lang/String;Ljava/security/Provider;)Ljava/security/Signature;", x_signature_provider), M_END,
};


/* ---- ScrollView.smoothScrollTo/By ------------------------------------------------------------------------------- */
/* The scroll is clamped to the content, run by an OverScroller for 250 ms (ScrollView.ANIMATED_SCROLL_GAP) and
 * advanced a step each time the view computes its scroll, which is how the framework animates it. */
static bool x_scrollview_smooth_by(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  (void)nargs; (void)out;
  union dvm_value range = { 0 }, extent = { 0 };
  if (!am_vcall(vm, self, "computeVerticalScrollRange", "()I", NULL, 0, &range) ||
      !am_vcall(vm, self, "computeVerticalScrollExtent", "()I", NULL, 0, &extent)) return false;
  const int32_t max = range.i - extent.i > 0 ? range.i - extent.i : 0;
  const int32_t sx = ui_int(vm, self, "scrollX"), sy = ui_int(vm, self, "scrollY");
  int32_t target = sy + ARG(1).i;
  target = target < 0 ? 0 : target > max ? max : target;
  union dvm_value scroller = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "smoothScroller", "Landroid/widget/OverScroller;", &scroller);
  if (!scroller.l) {
    union dvm_value ctx = { 0 };
    scroller.l = dvm_new_object(vm, dvm_find_class(vm, "android/widget/OverScroller"));
    if (!scroller.l || !am_vcall(vm, self, "getContext", "()Landroid/content/Context;", NULL, 0, &ctx) ||
        !am_vcall(vm, scroller.l, "<init>", "(Landroid/content/Context;)V", &ctx, 1, &ign)) return false;
    (void)dvm_set_field(vm, self, "smoothScroller", "Landroid/widget/OverScroller;", scroller);
  }
  union dvm_value a[5] = { { .i = sx }, { .i = sy }, { .i = 0 }, { .i = target - sy }, { .i = 250 } };
  if (!am_vcall(vm, scroller.l, "startScroll", "(IIIII)V", a, 5, &ign)) return false;
  ui_invalidate_view(vm, self, NULL, 0);
  RETV();
}
static bool x_scrollview_smooth_to(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  (void)nargs;
  union dvm_value by[2] = { { .i = ARG(0).i - ui_int(vm, self, "scrollX") }, { .i = ARG(1).i - ui_int(vm, self, "scrollY") } };
  return x_scrollview_smooth_by(vm, self, by, 2, out);
}
static bool x_scrollview_compute_scroll(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
  (void)args; (void)nargs; (void)out;
  union dvm_value scroller = { 0 }, more = { 0 }, x = { 0 }, y = { 0 }, ign = { 0 };
  (void)dvm_get_field(vm, self, "smoothScroller", "Landroid/widget/OverScroller;", &scroller);
  if (!scroller.l || !am_vcall(vm, scroller.l, "computeScrollOffset", "()Z", NULL, 0, &more) || !more.i) RETV();
  if (!am_vcall(vm, scroller.l, "getCurrX", "()I", NULL, 0, &x) || !am_vcall(vm, scroller.l, "getCurrY", "()I", NULL, 0, &y)) return false;
  union dvm_value to[2] = { x, y };
  if (!am_vcall(vm, self, "scrollTo", "(II)V", to, 2, &ign)) return false;
  ui_invalidate_view(vm, self, NULL, 0);                      /* the next step is owed */
  RETV();
}
static const struct rt_method rt_gap_scroll_view_smooth[] = {
  M("smoothScrollBy", "(II)V", x_scrollview_smooth_by), M("smoothScrollTo", "(II)V", x_scrollview_smooth_to),
  M("computeScroll", "()V", x_scrollview_compute_scroll), M_END,
};

/* ---- Drawable.setHotspotBounds / getHotspotBounds --------------------------------------------------------------- */
/* The rectangle a ripple radiates within, kept as the drawable's own Rect (null until set). */
static bool x_drawable_hotspot(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs, union dvm_value *out)
{
  (void)nargs;
  if (!strcmp(builtin_name(), "setHotspotBounds")) {
    dvm_ref r = dvm_new_object(vm, dvm_find_class(vm, "android/graphics/Rect"));
    union dvm_value ign = { 0 };
    if (!r || !am_vcall(vm, r, "<init>", "(IIII)V", args, 4, &ign)) return false;
    (void)dvm_set_field(vm, self, "hotspotBounds", "Landroid/graphics/Rect;", (union dvm_value) { .l = r });
    ui_invalidate_view(vm, self, NULL, 0);
    RETV();
  }
  union dvm_value hs = { 0 }, ign = { 0 };           /* getHotspotBounds(Rect out): the hotspot, else the bounds */
  (void)dvm_get_field(vm, self, "hotspotBounds", "Landroid/graphics/Rect;", &hs);
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "outRect");
    return false;
  }
  if (!hs.l && !am_vcall(vm, self, "getBounds", "()Landroid/graphics/Rect;", NULL, 0, &hs)) return false;
  union dvm_value src = hs;
  return am_vcall(vm, ARG(0).l, "set", "(Landroid/graphics/Rect;)V", &src, 1, &ign);
}
static const struct rt_method rt_gap_drawable_hotspot[] = {
  M("setHotspotBounds", "(IIII)V", x_drawable_hotspot), M("getHotspotBounds", "(Landroid/graphics/Rect;)V", x_drawable_hotspot),
  M_END,
};


/* ---- Activity managed dialogs: showDialog / dismissDialog / removeDialog ---------------------------------------- */
/* Activity keeps the dialogs it was asked to show by id.  showDialog(id) builds one with onCreateDialog() the first
 * time, lets onPrepareDialog() adjust it each time, and shows it; removeDialog(id) dismisses and forgets it. */
struct x_managed_dialog {
  dvm_ref activity, dialog;
  int32_t id;
};
static struct x_managed_dialog g_managed_dialogs[64];
static struct x_managed_dialog *x_managed_find(dvm_ref activity, int32_t id)
{
  for (size_t i = 0; i < sizeof g_managed_dialogs / sizeof g_managed_dialogs[0]; ++i)
    if (g_managed_dialogs[i].dialog && g_managed_dialogs[i].activity == activity && g_managed_dialogs[i].id == id)
      return &g_managed_dialogs[i];
  return NULL;
}
static bool x_activity_show_dialog(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
  const int32_t id = ARG(0).i;
  const bool with_args = nargs > 1;
  union dvm_value ign = { 0 };
  struct x_managed_dialog *slot = x_managed_find(self, id);
  if (!slot) {
    union dvm_value create[2] = { { .i = id }, with_args ? ARG(1) : (union dvm_value){ 0 } }, dialog = { 0 };
    if (!am_vcall(vm, self, "onCreateDialog", "(ILandroid/os/Bundle;)Landroid/app/Dialog;", create, 2, &dialog)) return false;
    if (!dialog.l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "no dialog with id %d was ever shown via Activity#showDialog", id);
      return false;
    }
    for (size_t i = 0; i < sizeof g_managed_dialogs / sizeof g_managed_dialogs[0] && !slot; ++i)
      if (!g_managed_dialogs[i].dialog) slot = &g_managed_dialogs[i];
    if (!slot) {
      dvm__throw(vm, "java/lang/IllegalStateException", "too many managed dialogs");
      return false;
    }
    dvm_pin(vm, dialog.l);
    dvm_pin(vm, self);
    slot->activity = self;
    slot->dialog = dialog.l;
    slot->id = id;
    union dvm_value owner = { .l = self };
    (void)am_vcall(vm, dialog.l, "setOwnerActivity", "(Landroid/app/Activity;)V", &owner, 1, &ign);
    if (dvm_exception(vm)) return false;
  }
  union dvm_value prep[3] = { { .i = id }, { .l = slot->dialog }, with_args ? ARG(1) : (union dvm_value){ 0 } };
  if (!am_vcall(vm, self, "onPrepareDialog", "(ILandroid/app/Dialog;Landroid/os/Bundle;)V", prep, 3, &ign)) return false;
  if (!am_vcall(vm, slot->dialog, "show", "()V", NULL, 0, &ign)) return false;
  if (with_args) RETI(1);                 /* showDialog(id, Bundle) answers true */
  RETV();
}
static bool x_activity_dismiss_dialog(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
  (void)nargs;
  struct x_managed_dialog *slot = x_managed_find(self, ARG(0).i);
  if (!slot) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "no dialog with id %d was ever shown via Activity#showDialog", ARG(0).i);
    return false;
  }
  const bool remove = !strcmp(builtin_name(), "removeDialog");
  union dvm_value ign = { 0 };
  const bool ok = am_vcall(vm, slot->dialog, "dismiss", "()V", NULL, 0, &ign);
  if (remove) {                          /* removeDialog also forgets the dialog (and never throws for an unknown id) */
    dvm_unpin(vm, slot->dialog);
    dvm_unpin(vm, slot->activity);
    slot->dialog = 0;
    slot->activity = 0;
  }
  if (!ok) return false;
  RETV();
}
static bool x_activity_remove_dialog(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                     union dvm_value *out)
{
  if (!x_managed_find(self, ARG(0).i)) {
    (void)out; (void)nargs;
    RETV();                               /* removeDialog() of a dialog that was never shown is a no-op */
  }
  return x_activity_dismiss_dialog(vm, self, args, nargs, out);
}
static bool x_activity_on_create_dialog(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                        union dvm_value *out)
{
  if (nargs > 1)                          /* onCreateDialog(id, Bundle) delegates to onCreateDialog(id) */
    return am_vcall(vm, self, "onCreateDialog", "(I)Landroid/app/Dialog;", args, 1, out);
  RETL(0);
}
static bool x_activity_on_prepare_dialog(struct dvm *vm, dvm_ref self, const union dvm_value *args, int nargs,
                                         union dvm_value *out)
{
  union dvm_value ign = { 0 };
  if (nargs > 2)                          /* (id, Dialog, Bundle) delegates to (id, Dialog) */
    return am_vcall(vm, self, "onPrepareDialog", "(ILandroid/app/Dialog;)V", args, 2, out);
  (void)ign;
  RETV();                                 /* onPrepareDialog(id, Dialog) is empty */
}
static const struct rt_method rt_gap_activity_dialogs[] = {
  M("showDialog", "(I)V", x_activity_show_dialog), M("showDialog", "(ILandroid/os/Bundle;)Z", x_activity_show_dialog),
  M("dismissDialog", "(I)V", x_activity_dismiss_dialog), M("removeDialog", "(I)V", x_activity_remove_dialog),
  M("onCreateDialog", "(I)Landroid/app/Dialog;", x_activity_on_create_dialog),
  M("onCreateDialog", "(ILandroid/os/Bundle;)Landroid/app/Dialog;", x_activity_on_create_dialog),
  M("onPrepareDialog", "(ILandroid/app/Dialog;)V", x_activity_on_prepare_dialog),
  M("onPrepareDialog", "(ILandroid/app/Dialog;Landroid/os/Bundle;)V", x_activity_on_prepare_dialog),
  M_END,
};
