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
