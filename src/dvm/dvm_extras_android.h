/* android.content / android.os / android.net / android.webkit / android.text /
 * android.media additions.
 *
 * Merged into the builtin tables through rt_extras (see dvm_runtime_define).
 * Included from dvm_runtime.c; the platform classes these extend are defined
 * above. */

#ifndef DVM_EXTRAS_ANDROID_H
#define DVM_EXTRAS_ANDROID_H

/* -------------------------------------------------------------- AudioManager */

/* generateAudioSessionId(): a fresh, positive session id every call, as
 * audioserver hands them out. */
static bool x_audio_generateSessionId(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                      int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   static _Atomic int32_t next = 1;
   RETI(atomic_fetch_add(&next, 1));
}

static const struct rt_method rt_x_audio_manager[] = {
   M("generateAudioSessionId", "()I", x_audio_generateSessionId),
   M_END,
};

/* ---------------------------------------------------------------- Vibrator */

/* This device has no vibration motor, so no haptic primitive is supported. */
static const struct rt_method rt_x_vibrator[] = {
   M("areAllPrimitivesSupported", "([I)Z", ret_false),
   M("areAllEffectsSupported", "([I)[I", ret_null),
   M_END,
};

/* -------------------------------------------------------------- StrictMode */

/* The VM policy is process-wide (the thread policy is per thread): a mask of
 * the detectors and penalties its builder switched on. */
static const struct rt_field rt_x_vm_policy_fields[] = { { "mask", "I" }, F_END };

static dvm_ref x_vm_policy_new(struct dvm *vm, int32_t mask)
{
   dvm_ref policy = dvm_new_object(vm, dvm_find_class(vm, "android/os/StrictMode$VmPolicy"));
   if (policy) rt_set_int(vm, policy, "mask", mask);
   return policy;
}

static dvm_ref g_x_vm_policy;

static bool x_strict_vmPolicy(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!strcmp(builtin_name(), "setVmPolicy")) {
      if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "policy"); return false; }
      if (g_x_vm_policy) dvm_unpin(vm, g_x_vm_policy);
      g_x_vm_policy = ARG(0).l;
      dvm_pin(vm, g_x_vm_policy);
      RETV();
   }
   if (!g_x_vm_policy) {
      g_x_vm_policy = x_vm_policy_new(vm, 0);       /* VmPolicy.LAX */
      if (!g_x_vm_policy) return false;
      dvm_pin(vm, g_x_vm_policy);
   }
   RETL(g_x_vm_policy);
}

static const struct rt_method rt_x_strictmode[] = {
   SM("getVmPolicy", "()Landroid/os/StrictMode$VmPolicy;", x_strict_vmPolicy),
   SM("setVmPolicy", "(Landroid/os/StrictMode$VmPolicy;)V", x_strict_vmPolicy),
   M_END,
};

/* VmPolicy.Builder: each detect... or penalty... call sets one bit, in the order the
 * methods are listed; build() freezes them into a VmPolicy. */
static const char *const x_vm_builder_bits[] = {
   "detectActivityLeaks", "detectCleartextNetwork", "detectContentUriWithoutPermission",
   "detectCredentialProtectedWhileLocked", "detectFileUriExposure", "detectImplicitDirectBoot",
   "detectIncorrectContextUse", "detectLeakedClosableObjects", "detectLeakedRegistrationObjects",
   "detectLeakedSqlLiteObjects", "detectNonSdkApiUsage", "detectUnsafeIntentLaunch",
   "detectUntaggedSockets", "penaltyDeath", "penaltyDeathOnCleartextNetwork",
   "penaltyDeathOnFileUriExposure", "penaltyDropBox", "penaltyListener", "penaltyLog", NULL
};

static bool x_vm_builder(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *name = builtin_name();
   int32_t mask = rt_get_int(vm, self, "mask");
   if (!strcmp(name, "<init>")) RETV();
   if (!strcmp(name, "build")) {
      dvm_ref policy = x_vm_policy_new(vm, mask);
      if (!policy) return false;
      RETL(policy);
   }
   if (!strcmp(name, "detectAll")) mask |= 0x1fff;                       /* every detector, no penalty */
   else if (!strcmp(name, "permitNonSdkApiUsage")) mask &= ~(1 << 10);
   else if (!strcmp(name, "permitUnsafeIntentLaunch")) mask &= ~(1 << 11);
   else for (int i = 0; x_vm_builder_bits[i]; ++i)
      if (!strcmp(name, x_vm_builder_bits[i])) { mask |= 1 << i; break; }
   rt_set_int(vm, self, "mask", mask);
   RETL(self);
}

static const struct rt_method rt_x_vm_builder[] = {
   M("<init>", "()V", x_vm_builder),
   M("detectActivityLeaks", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectAll", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectCleartextNetwork", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectContentUriWithoutPermission", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectCredentialProtectedWhileLocked", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectFileUriExposure", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectImplicitDirectBoot", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectIncorrectContextUse", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectLeakedClosableObjects", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectLeakedRegistrationObjects", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectLeakedSqlLiteObjects", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectNonSdkApiUsage", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectUnsafeIntentLaunch", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("detectUntaggedSockets", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("penaltyDeath", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("penaltyDeathOnCleartextNetwork", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("penaltyDeathOnFileUriExposure", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("penaltyDropBox", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("penaltyListener", "(Ljava/util/concurrent/Executor;Landroid/os/StrictMode$OnVmViolationListener;)Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("penaltyLog", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("permitNonSdkApiUsage", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("permitUnsafeIntentLaunch", "()Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("setClassInstanceLimit", "(Ljava/lang/Class;I)Landroid/os/StrictMode$VmPolicy$Builder;", x_vm_builder),
   M("build", "()Landroid/os/StrictMode$VmPolicy;", x_vm_builder),
   M_END,
};

/* ----------------------------------------------------------- ComponentName */

/* flattenToShortString(): "pkg/.Class" when the class lives in the package. */
static bool x_component_flattenShort(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value pkg = { 0 }, cls = { 0 };
   (void)dvm_get_field(vm, self, "pkg", "Ljava/lang/String;", &pkg);
   (void)dvm_get_field(vm, self, "cls", "Ljava/lang/String;", &cls);
   if (!pkg.l || !cls.l) {
      /* The long form is the only one this component has fields for. */
      return am_vcall(vm, self, "flattenToString", "()Ljava/lang/String;", NULL, 0, out);
   }
   const char *p = dvm_string_utf8(vm, pkg.l), *c = dvm_string_utf8(vm, cls.l);
   if (!p || !c) return false;
   const size_t pl = strlen(p);
   char text[1024];
   if (!strncmp(c, p, pl) && c[pl] == '.') snprintf(text, sizeof text, "%s/%s", p, c + pl);
   else snprintf(text, sizeof text, "%s/%s", p, c);
   RETL(dvm_new_string(vm, text));
}

static const struct rt_method rt_x_component[] = {
   M("flattenToShortString", "()Ljava/lang/String;", x_component_flattenShort),
   M_END,
};

/* ------------------------------------------------------------- IntentFilter *
 *
 * Action, category and data matching exactly as the framework does it, so a
 * receiver registered for an action (or a scheme/type) hears what it should. */

static dvm_ref x_if_list(struct dvm *vm, dvm_ref self, const char *field, bool create)
{
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, field, "Ljava/util/List;", &v);
   if (!v.l && create) {
      v.l = coll_new_list(vm, "Ljava/util/ArrayList;");
      (void)dvm_set_field(vm, self, field, "Ljava/util/List;", v);
   }
   return v.l;
}

static bool x_if_str_in(struct dvm *vm, dvm_ref list, const char *s, bool fold)
{
   struct rt_list *l = list ? list_of(vm, list) : NULL;
   for (uint32_t i = 0; l && i < l->size; ++i) {
      const char *e = dvm_string_utf8(vm, l->items[i]);
      if (e && (fold ? !strcasecmp(e, s) : !strcmp(e, s))) return true;
   }
   return false;
}

static const char *x_if_field_for(const char *name)
{
   if (strstr(name, "Action")) return "actions";
   if (strstr(name, "Categor")) return "categories";
   if (strstr(name, "Scheme")) return "schemes";
   if (strstr(name, "Type")) return "types";
   if (strstr(name, "Authorit")) return "authorities";
   return "paths";
}

/* addAction / addCategory / addDataScheme: a set, in insertion order. */
static bool x_if_add(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *name = builtin_name();
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "%s", name); return false; }
   const char *s = dvm_string_utf8(vm, ARG(0).l);
   const char *field = x_if_field_for(name);
   dvm_ref list = x_if_list(vm, self, field, true);
   if (!list || !s) return false;
   if (!strcmp(name, "addDataType")) {
      /* MIME types are "type/subtype"; anything else is malformed. */
      const char *slash = strchr(s, '/');
      if (!slash || slash == s || !slash[1] || strchr(slash + 1, '/')) {
         dvm__throw(vm, "android/content/IntentFilter$MalformedMimeTypeException", "%s", s);
         return false;
      }
   }
   if (!x_if_str_in(vm, list, s, !strcmp(name, "addDataType"))) {
      union dvm_value ignored = { 0 };
      if (!list_add(vm, list, args, 1, &ignored)) return false;
   }
   if (!strcmp(name, "addAction")) {
      union dvm_value first = { 0 };
      (void)dvm_get_field(vm, self, "action", "Ljava/lang/String;", &first);
      if (!first.l) (void)dvm_set_field(vm, self, "action", "Ljava/lang/String;", ARG(0));
   } else if (!strcmp(name, "addDataScheme")) {
      (void)dvm_set_field(vm, self, "scheme", "Ljava/lang/String;", ARG(0));
   }
   RETV();
}

static bool x_if_has(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   const char *s = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   RETI(s && x_if_str_in(vm, x_if_list(vm, self, x_if_field_for(name), false), s, !strcmp(name, "hasDataType")));
}

static bool x_if_count(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_list *l = list_of(vm, x_if_list(vm, self, x_if_field_for(builtin_name()), false));
   RETI(l ? (int32_t)l->size : 0);
}

static bool x_if_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)nargs;
   struct rt_list *l = list_of(vm, x_if_list(vm, self, x_if_field_for(builtin_name()), false));
   if (!l || ARG(0).i < 0 || (uint32_t)ARG(0).i >= l->size) {
      dvm__throw(vm, "java/lang/IndexOutOfBoundsException", "Index: %d", ARG(0).i);
      return false;
   }
   RETL(l->items[ARG(0).i]);
}

static bool x_if_iterator(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref list = x_if_list(vm, self, x_if_field_for(builtin_name()), false);
   if (!list) { list = coll_new_list(vm, "Ljava/util/ArrayList;"); if (!list) return false; }
   return am_vcall(vm, list, "iterator", "()Ljava/util/Iterator;", NULL, 0, out);
}

static bool x_if_setPriority(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!strcmp(builtin_name(), "getPriority")) RETI(rt_get_int(vm, self, "priority"));
   rt_set_int(vm, self, "priority", ARG(0).i);
   RETV();
}

/* addDataAuthority(host, port): kept as "host" or "host:port".  A leading '*'
 * in the host is a wildcard prefix; a port of "*" or null accepts any. */
static bool x_if_addAuthority(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *host = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   const char *port = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL;
   if (!host) { dvm__throw(vm, "java/lang/NullPointerException", "host"); return false; }
   char text[512];
   snprintf(text, sizeof text, "%s:%s", host, port ? port : "*");
   dvm_ref list = x_if_list(vm, self, "authorities", true);
   union dvm_value item = { .l = dvm_new_string(vm, text) }, ignored = { 0 };
   if (!list || !item.l || !list_add(vm, list, &item, 1, &ignored)) return false;
   RETV();
}

/* addDataPath(path, type): LITERAL (0), PREFIX (1), SIMPLE_GLOB (2). */
static bool x_if_addPath(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   const char *path = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!path) { dvm__throw(vm, "java/lang/NullPointerException", "path"); return false; }
   char text[1024];
   snprintf(text, sizeof text, "%d:%s", ARG(1).i, path);
   dvm_ref list = x_if_list(vm, self, "paths", true);
   union dvm_value item = { .l = dvm_new_string(vm, text) }, ignored = { 0 };
   if (!list || !item.l || !list_add(vm, list, &item, 1, &ignored)) return false;
   RETV();
}

/* Simple glob: '.' matches one char, '*' any run; ".*" is the usual wildcard. */
static bool x_glob(const char *pat, const char *s)
{
   if (!*pat) return !*s;
   if (pat[0] == '\\' && pat[1]) return *s == pat[1] && x_glob(pat + 2, s + 1);
   if (pat[1] == '*') {
      for (const char *t = s;; ++t) {
         if (x_glob(pat + 2, t)) return true;
         if (!*t || (pat[0] != '.' && *t != pat[0])) return false;
      }
   }
   if (*s && (pat[0] == '.' || pat[0] == *s)) return x_glob(pat + 1, s + 1);
   return false;
}

static bool x_if_path_ok(struct dvm *vm, dvm_ref paths, const char *path)
{
   struct rt_list *l = paths ? list_of(vm, paths) : NULL;
   for (uint32_t i = 0; l && i < l->size; ++i) {
      const char *e = dvm_string_utf8(vm, l->items[i]);
      if (!e || !path) continue;
      const int type = atoi(e);
      const char *p = strchr(e, ':') + 1;
      if ((type == 0 && !strcmp(p, path)) || (type == 1 && !strncmp(p, path, strlen(p)) && false) ||
          (type == 1 && !strncmp(path, p, strlen(p))) || (type == 2 && x_glob(p, path))) return true;
   }
   return false;
}

static bool x_if_hasPath(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *path = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   RETI(x_if_path_ok(vm, x_if_list(vm, self, "paths", false), path));
}

/* The Uri's authority matched against the filter's: -2 for none, else a
 * MATCH_CATEGORY_HOST (0x300000) or _PORT (0x400000) score. */
static int32_t x_if_matchAuthority(struct dvm *vm, dvm_ref self, dvm_ref data)
{
   dvm_ref list = x_if_list(vm, self, "authorities", false);
   struct rt_list *l = list ? list_of(vm, list) : NULL;
   if (!l || !l->size) return -2;
   if (!data) return -2;
   union dvm_value host = { 0 }, port = { .i = -1 };
   if (!am_vcall(vm, data, "getHost", "()Ljava/lang/String;", NULL, 0, &host) ||
       !am_vcall(vm, data, "getPort", "()I", NULL, 0, &port)) return -2;
   const char *h = host.l ? dvm_string_utf8(vm, host.l) : NULL;
   if (!h) return -2;
   int32_t best = -2;
   for (uint32_t i = 0; i < l->size; ++i) {
      const char *e = dvm_string_utf8(vm, l->items[i]);
      if (!e) continue;
      char fh[512];
      snprintf(fh, sizeof fh, "%s", e);
      char *colon = strrchr(fh, ':');
      const char *fp = colon ? colon + 1 : "*";
      if (colon) *colon = '\0';
      const bool wild = fh[0] == '*';
      if (wild ? (strlen(h) < strlen(fh + 1) || strcasecmp(h + strlen(h) - strlen(fh + 1), fh + 1)) : strcasecmp(h, fh)) continue;
      if (!strcmp(fp, "*")) { if (best < 0x300000) best = 0x300000; continue; }
      if (port.i == atoi(fp)) best = 0x400000;
   }
   return best;
}

static int32_t x_if_match_data(struct dvm *vm, dvm_ref self, const char *type, const char *scheme, dvm_ref data)
{
   dvm_ref types = x_if_list(vm, self, "types", false), schemes = x_if_list(vm, self, "schemes", false);
   struct rt_list *tl = types ? list_of(vm, types) : NULL, *sl = schemes ? list_of(vm, schemes) : NULL;
   const bool has_types = tl && tl->size, has_schemes = sl && sl->size;
   int32_t match = 0x100000;                       /* MATCH_CATEGORY_EMPTY */
   if (!has_types && !has_schemes) return !type && !data ? 0x108000 : -2;
   if (has_schemes) {
      if (x_if_str_in(vm, schemes, scheme ? scheme : "", false)) match = 0x200000;   /* SCHEME */
      else return -2;
      if (x_if_list(vm, self, "authorities", false) && list_of(vm, x_if_list(vm, self, "authorities", false))->size) {
         const int32_t auth = x_if_matchAuthority(vm, self, data);
         if (auth < 0) return -2;
         dvm_ref paths = x_if_list(vm, self, "paths", false);
         if (!paths || !list_of(vm, paths)->size) match = auth;
         else {
            union dvm_value p = { 0 };
            if (data && !am_vcall(vm, data, "getPath", "()Ljava/lang/String;", NULL, 0, &p)) return -2;
            if (x_if_path_ok(vm, paths, p.l ? dvm_string_utf8(vm, p.l) : NULL)) match = 0x500000;   /* PATH */
            else return -2;
         }
      }
   } else if (scheme && *scheme && strcmp(scheme, "content") && strcmp(scheme, "file")) return -2;
   if (has_types) {
      bool found = false;
      if (type) {
         const char *slash = strchr(type, '/');
         for (uint32_t i = 0; !found && i < tl->size; ++i) {
            const char *want = dvm_string_utf8(vm, tl->items[i]);
            if (!want) continue;
            if (!strcasecmp(want, type)) found = true;
            else if (slash) {
               const char *ws = strchr(want, '/');
               if (ws && !strcmp(ws, "/*") && !strncasecmp(want, type, (size_t)(slash - type)) && ws - want == slash - type) found = true;
               else if (!strcmp(want, "*/*")) found = true;
            }
         }
      }
      if (found) match = 0x600000; else return -1;     /* TYPE; NO_MATCH_TYPE */
   } else if (type) return -1;
   return match + 0x8000;                            /* MATCH_ADJUSTMENT_NORMAL */
}

static bool x_if_matchAction(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *a = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   RETI(a && x_if_str_in(vm, x_if_list(vm, self, "actions", false), a, false));
}

/* matchCategories(set): null when every category is one the filter lists, else
 * the first that is not. */
static bool x_if_matchCategories(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                 int nargs, union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l) RETL(0);
   union dvm_value it = { 0 }, more = { 0 }, item = { 0 };
   if (!am_vcall(vm, ARG(0).l, "iterator", "()Ljava/util/Iterator;", NULL, 0, &it)) return false;
   dvm_ref mine = x_if_list(vm, self, "categories", false);
   for (;;) {
      if (!am_vcall(vm, it.l, "hasNext", "()Z", NULL, 0, &more) || !more.i) break;
      if (!am_vcall(vm, it.l, "next", "()Ljava/lang/Object;", NULL, 0, &item)) return false;
      const char *c = item.l ? dvm_string_utf8(vm, item.l) : NULL;
      if (c && !x_if_str_in(vm, mine, c, false)) RETL(item.l);
   }
   RETL(0);
}

static bool x_if_matchDataM(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs;
   RETI(x_if_match_data(vm, self,
        ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL, ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL, ARG(2).l));
}

/* match(action, type, scheme, data, categories, logTag), and the Intent form. */
static bool x_if_match(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *action, *type, *scheme;
   dvm_ref data, categories;
   if (strstr(builtin_sig(), "Intent;")) {              /* (ContentResolver, Intent, boolean, String) */
      dvm_ref intent = ARG(1).l;
      if (!intent) { dvm__throw(vm, "java/lang/NullPointerException", "intent"); return false; }
      union dvm_value a = { 0 }, t = { 0 }, s = { 0 }, d = { 0 }, c = { 0 };
      if (!am_vcall(vm, intent, "getAction", "()Ljava/lang/String;", NULL, 0, &a) ||
          !am_vcall(vm, intent, "getType", "()Ljava/lang/String;", NULL, 0, &t) ||
          !am_vcall(vm, intent, "getScheme", "()Ljava/lang/String;", NULL, 0, &s) ||
          !am_vcall(vm, intent, "getData", "()Landroid/net/Uri;", NULL, 0, &d) ||
          !am_vcall(vm, intent, "getCategories", "()Ljava/util/Set;", NULL, 0, &c)) return false;
      action = a.l ? dvm_string_utf8(vm, a.l) : NULL; type = t.l ? dvm_string_utf8(vm, t.l) : NULL;
      scheme = s.l ? dvm_string_utf8(vm, s.l) : NULL; data = d.l; categories = c.l;
   } else {
      action = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL; type = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL;
      scheme = ARG(2).l ? dvm_string_utf8(vm, ARG(2).l) : NULL; data = ARG(3).l; categories = ARG(4).l;
   }
   if (action && !x_if_str_in(vm, x_if_list(vm, self, "actions", false), action, false)) RETI(-3);   /* NO_MATCH_ACTION */
   const int32_t dm = x_if_match_data(vm, self, type, scheme, data);
   if (dm < 0) RETI(dm);
   if (categories) {
      union dvm_value arg = { .l = categories }, missing = { 0 };
      if (!x_if_matchCategories(vm, self, &arg, 1, &missing)) return false;
      if (missing.l) RETI(-4);                                                         /* NO_MATCH_CATEGORY */
   }
   RETI(dm);
}

static const struct rt_method rt_x_intent_filter[] = {
   M("<init>", "()V", empty_void),
   M("<init>", "(Ljava/lang/String;)V", x_if_add),
   M("<init>", "(Landroid/content/IntentFilter;)V", empty_void),
   M("addAction", "(Ljava/lang/String;)V", x_if_add), M("hasAction", "(Ljava/lang/String;)Z", x_if_has),
   M("countActions", "()I", x_if_count), M("getAction", "(I)Ljava/lang/String;", x_if_get),
   M("actionsIterator", "()Ljava/util/Iterator;", x_if_iterator),
   M("addCategory", "(Ljava/lang/String;)V", x_if_add), M("hasCategory", "(Ljava/lang/String;)Z", x_if_has),
   M("countCategories", "()I", x_if_count), M("getCategory", "(I)Ljava/lang/String;", x_if_get),
   M("categoriesIterator", "()Ljava/util/Iterator;", x_if_iterator),
   M("addDataScheme", "(Ljava/lang/String;)V", x_if_add), M("hasDataScheme", "(Ljava/lang/String;)Z", x_if_has),
   M("countDataSchemes", "()I", x_if_count), M("getDataScheme", "(I)Ljava/lang/String;", x_if_get),
   M("schemesIterator", "()Ljava/util/Iterator;", x_if_iterator),
   M("addDataType", "(Ljava/lang/String;)V", x_if_add), M("hasDataType", "(Ljava/lang/String;)Z", x_if_has),
   M("countDataTypes", "()I", x_if_count), M("getDataType", "(I)Ljava/lang/String;", x_if_get),
   M("typesIterator", "()Ljava/util/Iterator;", x_if_iterator),
   M("addDataAuthority", "(Ljava/lang/String;Ljava/lang/String;)V", x_if_addAuthority),
   M("countDataAuthorities", "()I", x_if_count),
   M("addDataPath", "(Ljava/lang/String;I)V", x_if_addPath), M("hasDataPath", "(Ljava/lang/String;)Z", x_if_hasPath),
   M("countDataPaths", "()I", x_if_count),
   M("setPriority", "(I)V", x_if_setPriority), M("getPriority", "()I", x_if_setPriority),
   M("matchAction", "(Ljava/lang/String;)Z", x_if_matchAction),
   M("matchCategories", "(Ljava/util/Set;)Ljava/lang/String;", x_if_matchCategories),
   M("matchData", "(Ljava/lang/String;Ljava/lang/String;Landroid/net/Uri;)I", x_if_matchDataM),
   M("match", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/net/Uri;Ljava/util/Set;Ljava/lang/String;)I", x_if_match),
   M("match", "(Landroid/content/ContentResolver;Landroid/content/Intent;ZLjava/lang/String;)I", x_if_match),
   M_END,
};


/* -------------------------------------------------------------- Connectivity *
 *
 * The link is one always-up wired interface (see cm_getActiveNetwork).  The
 * legacy NetworkInfo views and LinkProperties describe that same link. */

static const struct rt_method rt_x_enum_net_state[] = {
   SM("values", "()[Landroid/net/NetworkInfo$State;", builtin_enum_values),
   SM("valueOf", "(Ljava/lang/String;)Landroid/net/NetworkInfo$State;", builtin_enum_values),
   M_END,
};
static const struct rt_method rt_x_enum_net_detailed[] = {
   SM("values", "()[Landroid/net/NetworkInfo$DetailedState;", builtin_enum_values),
   SM("valueOf", "(Ljava/lang/String;)Landroid/net/NetworkInfo$DetailedState;", builtin_enum_values),
   M_END,
};
static const struct rt_field rt_x_net_state_sfields[] = {
   { "CONNECTED", "Landroid/net/NetworkInfo$State;" }, { "CONNECTING", "Landroid/net/NetworkInfo$State;" },
   { "DISCONNECTED", "Landroid/net/NetworkInfo$State;" }, { "DISCONNECTING", "Landroid/net/NetworkInfo$State;" },
   { "SUSPENDED", "Landroid/net/NetworkInfo$State;" }, { "UNKNOWN", "Landroid/net/NetworkInfo$State;" }, F_END
};
static const struct rt_field rt_x_net_detailed_sfields[] = {
   { "IDLE", "Landroid/net/NetworkInfo$DetailedState;" }, { "SCANNING", "Landroid/net/NetworkInfo$DetailedState;" },
   { "CONNECTING", "Landroid/net/NetworkInfo$DetailedState;" }, { "AUTHENTICATING", "Landroid/net/NetworkInfo$DetailedState;" },
   { "OBTAINING_IPADDR", "Landroid/net/NetworkInfo$DetailedState;" }, { "CONNECTED", "Landroid/net/NetworkInfo$DetailedState;" },
   { "SUSPENDED", "Landroid/net/NetworkInfo$DetailedState;" }, { "DISCONNECTING", "Landroid/net/NetworkInfo$DetailedState;" },
   { "DISCONNECTED", "Landroid/net/NetworkInfo$DetailedState;" }, { "FAILED", "Landroid/net/NetworkInfo$DetailedState;" },
   { "BLOCKED", "Landroid/net/NetworkInfo$DetailedState;" }, { "VERIFYING_POOR_LINK", "Landroid/net/NetworkInfo$DetailedState;" },
   { "CAPTIVE_PORTAL_CHECK", "Landroid/net/NetworkInfo$DetailedState;" }, F_END
};

static bool x_ni_state(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   const bool detailed = !strcmp(builtin_name(), "getDetailedState");
   union dvm_value v = { 0 };
   (void)dvm_get_static(vm, dvm_find_class(vm, detailed ? "android/net/NetworkInfo$DetailedState" : "android/net/NetworkInfo$State"),
                        dvm_network_enabled() ? "CONNECTED" : "DISCONNECTED",
                        detailed ? "Landroid/net/NetworkInfo$DetailedState;" : "Landroid/net/NetworkInfo$State;", &v);
   RETL(v.l);
}

static const struct rt_method rt_x_network_info[] = {
   M("getState", "()Landroid/net/NetworkInfo$State;", x_ni_state),
   M("getDetailedState", "()Landroid/net/NetworkInfo$DetailedState;", x_ni_state),
   /* Only a Wi-Fi or mobile link carries an SSID/APN; a wired one has none. */
   M("getExtraInfo", "()Ljava/lang/String;", ret_null),
   M("getReason", "()Ljava/lang/String;", ret_null),
   M("isFailover", "()Z", ret_false),
   M_END,
};

/* getNetworkInfo(Network) and getAllNetworkInfo(): the one link. */
static bool x_cm_networkInfo(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value info = { 0 };
   if (!cm_getActiveNetworkInfo(vm, self, NULL, 0, &info)) return false;
   if (strcmp(builtin_name(), "getAllNetworkInfo")) RETL(info.l);
   dvm_ref arr = dvm_new_array(vm, 'L', "Landroid/net/NetworkInfo;", info.l ? 1u : 0u);
   if (arr && info.l) ((dvm_ref *)dvm_array_data(vm, arr))[0] = info.l;
   RETL(arr);
}

/* LinkProperties: the interface name the host's default route uses, its
 * addresses, and the resolvers from resolv.conf. */
static const struct rt_field rt_x_link_properties_fields[] = {
   { "interfaceName", "Ljava/lang/String;" }, { "dnsServers", "Ljava/util/List;" }, F_END
};

static bool x_default_interface(char *name, size_t size)
{
   FILE *f = fopen("/proc/net/route", "r");
   if (!f) return false;
   char line[256];
   bool found = false;
   (void)fgets(line, sizeof line, f);                   /* header */
   while (fgets(line, sizeof line, f)) {
      char iface[32];
      unsigned long dest;
      if (sscanf(line, "%31s %lx", iface, &dest) == 2 && dest == 0) { snprintf(name, size, "%s", iface); found = true; break; }
   }
   fclose(f);
   return found;
}

static dvm_ref x_link_properties(struct dvm *vm)
{
   dvm_ref lp = dvm_new_object(vm, dvm_find_class(vm, "android/net/LinkProperties"));
   if (!lp) return 0;
   char iface[32] = "eth0";
   (void)x_default_interface(iface, sizeof iface);
   (void)dvm_set_field(vm, lp, "interfaceName", "Ljava/lang/String;", (union dvm_value){ .l = dvm_new_string(vm, iface) });
   dvm_ref dns = coll_new_list(vm, "Ljava/util/ArrayList;");
   FILE *f = fopen("/etc/resolv.conf", "r");
   char line[256];
   while (dns && f && fgets(line, sizeof line, f)) {
      char ip[128];
      if (sscanf(line, "nameserver %127s", ip) != 1) continue;
      union dvm_value name = { .l = dvm_new_string(vm, ip) }, addr = { 0 }, ignored = { 0 };
      if (name.l && x_call_static(vm, "java/net/InetAddress", "getByName", "(Ljava/lang/String;)Ljava/net/InetAddress;", &name, 1, &addr) && addr.l)
         (void)list_add(vm, dns, &addr, 1, &ignored);
      else dvm_clear_exception(vm);
   }
   if (f) fclose(f);
   (void)dvm_set_field(vm, lp, "dnsServers", "Ljava/util/List;", (union dvm_value){ .l = dns });
   return lp;
}

static bool x_cm_linkProperties(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   if (!dvm_network_enabled()) RETL(0);
   RETL(x_link_properties(vm));
}

static bool x_lp_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *name = builtin_name();
   if (!strcmp(name, "getInterfaceName")) return dvm_get_field(vm, self, "interfaceName", "Ljava/lang/String;", out);
   if (!strcmp(name, "getDnsServers")) return dvm_get_field(vm, self, "dnsServers", "Ljava/util/List;", out);
   if (!strcmp(name, "getHttpProxy") || !strcmp(name, "getPrivateDnsServerName") || !strcmp(name, "getDomains") ||
       !strcmp(name, "getTcpBufferSizes")) RETL(0);
   if (!strcmp(name, "isPrivateDnsActive")) RETI(0);
   RETL(coll_new_list(vm, "Ljava/util/ArrayList;"));          /* getLinkAddresses / getRoutes */
}

static const struct rt_method rt_x_link_properties[] = {
   M("<init>", "()V", empty_void),
   M("getInterfaceName", "()Ljava/lang/String;", x_lp_get), M("getDnsServers", "()Ljava/util/List;", x_lp_get),
   M("getLinkAddresses", "()Ljava/util/List;", x_lp_get), M("getRoutes", "()Ljava/util/List;", x_lp_get),
   M("getHttpProxy", "()Landroid/net/ProxyInfo;", x_lp_get), M("getDomains", "()Ljava/lang/String;", x_lp_get),
   M("isPrivateDnsActive", "()Z", x_lp_get), M("getPrivateDnsServerName", "()Ljava/lang/String;", x_lp_get),
   M_END,
};

static const struct rt_method rt_x_conn_manager[] = {
   M("getNetworkInfo", "(Landroid/net/Network;)Landroid/net/NetworkInfo;", x_cm_networkInfo),
   M("getAllNetworkInfo", "()[Landroid/net/NetworkInfo;", x_cm_networkInfo),
   M("getLinkProperties", "(Landroid/net/Network;)Landroid/net/LinkProperties;", x_cm_linkProperties),
   M_END,
};


/* ------------------------------------------------------------------- Looper *
 *
 * setMessageLogging(Printer): the loop prints a line before and after every
 * message it dispatches, which is how block detectors time the main thread.
 * The line names the handler, its callback and the message's `what`. */

static bool x_looper_setMessageLogging(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                       int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   (void)dvm_set_field(vm, self, "messageLogging", "Landroid/util/Printer;", ARG(0));
   RETV();
}

/* Called by the dispatchers; `looper` 0 is the main looper. */
void dvm_runtime_looper_log(struct dvm *vm, dvm_ref looper, bool begin, dvm_ref runnable)
{
   dvm_ref l = looper ? looper : looper_main(vm);
   union dvm_value printer = { 0 };
   if (!l || !dvm_get_field(vm, l, "messageLogging", "Landroid/util/Printer;", &printer) || !printer.l) return;
   struct dvm_class *c = runnable ? dvm_object_class(vm, runnable) : NULL;
   char text[256];
   snprintf(text, sizeof text, "%s Handler (android.os.Handler) {%x} %s: 0",
            begin ? ">>>>> Dispatching to" : "<<<<< Finished to", (unsigned)runnable, c && c->name ? c->name : "null");
   union dvm_value line = { .l = dvm_new_string(vm, text) }, ignored = { 0 };
   dvm_ref raised = vm->exception;
   if (raised) { dvm_pin(vm, raised); dvm_clear_exception(vm); }
   if (line.l) (void)am_vcall(vm, printer.l, "println", "(Ljava/lang/String;)V", &line, 1, &ignored);
   if (dvm_exception(vm)) dvm_clear_exception(vm);        /* a logger must not break the loop */
   if (raised) { vm->exception = raised; dvm_unpin(vm, raised); }
}

static const struct rt_method rt_x_looper[] = {
   M("setMessageLogging", "(Landroid/util/Printer;)V", x_looper_setMessageLogging),
   M_END,
};

/* HandlerThread.onLooperPrepared(): empty in the base class; run() calls it
 * between prepare() and loop(). */
static const struct rt_method rt_x_handler_thread[] = {
   M("onLooperPrepared", "()V", empty_void),
   M_END,
};

/* ------------------------------------------------------------ Settings, etc. */

/* Settings.canDrawOverlays()/canWrite(): SYSTEM_ALERT_WINDOW and WRITE_SETTINGS
 * are special permissions a user grants in Settings; this device has not. */
static const struct rt_method rt_x_settings[] = {
   SM("canDrawOverlays", "(Landroid/content/Context;)Z", ret_false),
   SM("canWrite", "(Landroid/content/Context;)Z", ret_false),
   M_END,
};

static const struct { const char *name, *value; } x_settings_actions[] = {
   { "ACTION_SETTINGS", "android.settings.SETTINGS" },
   { "ACTION_APPLICATION_DETAILS_SETTINGS", "android.settings.APPLICATION_DETAILS_SETTINGS" },
   { "ACTION_MANAGE_OVERLAY_PERMISSION", "android.settings.action.MANAGE_OVERLAY_PERMISSION" },
   { "ACTION_MANAGE_WRITE_SETTINGS", "android.settings.action.MANAGE_WRITE_SETTINGS" },
   { "ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION", "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION" },
   { "ACTION_WIRELESS_SETTINGS", "android.settings.WIRELESS_SETTINGS" },
   { "ACTION_WIFI_SETTINGS", "android.settings.WIFI_SETTINGS" },
   { "ACTION_BLUETOOTH_SETTINGS", "android.settings.BLUETOOTH_SETTINGS" },
   { "ACTION_LOCATION_SOURCE_SETTINGS", "android.settings.LOCATION_SOURCE_SETTINGS" },
   { "ACTION_SECURITY_SETTINGS", "android.settings.SECURITY_SETTINGS" },
   { "ACTION_ACCESSIBILITY_SETTINGS", "android.settings.ACCESSIBILITY_SETTINGS" },
   { "ACTION_DATE_SETTINGS", "android.settings.DATE_SETTINGS" },
   { "ACTION_NOTIFICATION_LISTENER_SETTINGS", "android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS" },
   { "ACTION_APP_NOTIFICATION_SETTINGS", "android.settings.APP_NOTIFICATION_SETTINGS" },
   { "ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS", "android.settings.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS" },
   { "ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS", "android.settings.IGNORE_BATTERY_OPTIMIZATION_SETTINGS" },
   { "EXTRA_APP_PACKAGE", "android.provider.extra.APP_PACKAGE" },
   { "EXTRA_CHANNEL_ID", "android.provider.extra.CHANNEL_ID" },
   { NULL, NULL }
};
static const struct rt_field rt_x_settings_sfields[] = {
   { "ACTION_SETTINGS", "Ljava/lang/String;" }, { "ACTION_APPLICATION_DETAILS_SETTINGS", "Ljava/lang/String;" },
   { "ACTION_MANAGE_OVERLAY_PERMISSION", "Ljava/lang/String;" }, { "ACTION_MANAGE_WRITE_SETTINGS", "Ljava/lang/String;" },
   { "ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION", "Ljava/lang/String;" }, { "ACTION_WIRELESS_SETTINGS", "Ljava/lang/String;" },
   { "ACTION_WIFI_SETTINGS", "Ljava/lang/String;" }, { "ACTION_BLUETOOTH_SETTINGS", "Ljava/lang/String;" },
   { "ACTION_LOCATION_SOURCE_SETTINGS", "Ljava/lang/String;" }, { "ACTION_SECURITY_SETTINGS", "Ljava/lang/String;" },
   { "ACTION_ACCESSIBILITY_SETTINGS", "Ljava/lang/String;" }, { "ACTION_DATE_SETTINGS", "Ljava/lang/String;" },
   { "ACTION_NOTIFICATION_LISTENER_SETTINGS", "Ljava/lang/String;" }, { "ACTION_APP_NOTIFICATION_SETTINGS", "Ljava/lang/String;" },
   { "ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS", "Ljava/lang/String;" },
   { "ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS", "Ljava/lang/String;" },
   { "EXTRA_APP_PACKAGE", "Ljava/lang/String;" }, { "EXTRA_CHANNEL_ID", "Ljava/lang/String;" }, F_END
};

/* android.os.ServiceManager is hidden API: a service this process has not been
 * given a binder for is simply absent, which is a null, not an error. */
static bool x_servicemanager_list(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   RETL(dvm_new_array(vm, 'L', "Ljava/lang/String;", 0));
}

static const struct rt_method rt_x_service_manager[] = {
   SM("getService", "(Ljava/lang/String;)Landroid/os/IBinder;", ret_null),
   SM("checkService", "(Ljava/lang/String;)Landroid/os/IBinder;", ret_null),
   SM("listServices", "()[Ljava/lang/String;", x_servicemanager_list),
   M_END,
};

/* ----------------------------------------------------------------- Context */

/* deleteSharedPreferences(name): the file goes, and so does the in-memory map,
 * so a later getSharedPreferences(name) starts empty. */
static bool x_context_deleteSharedPreferences(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                              int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   const char *name = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!name) { dvm__throw(vm, "java/lang/NullPointerException", "fileName"); return false; }
   for (int i = 0; i < g_nprefs; ++i) {
      if (strcmp(g_prefs[i].name, name)) continue;
      union dvm_value ignored = { 0 };
      if (g_prefs[i].map) (void)map_clear(vm, g_prefs[i].map, NULL, 0, &ignored);
      g_prefs[i].dirty = false;
   }
   char path[DVM_PATH_MAX];
   prefs_path(name, path, sizeof path);
   const int rc = unlink(path);
   RETI(rc == 0);
}

static const struct rt_method rt_x_context[] = {
   M("deleteSharedPreferences", "(Ljava/lang/String;)Z", x_context_deleteSharedPreferences),
   M_END,
};

/* ---------------------------------------------------------- ContentResolver */

/* The installed provider that owns an authority, or 0. */
static dvm_ref x_find_provider(struct dvm *vm, const char *wanted)
{
   const size_t wanted_len = strlen(wanted);
   for (dvm_ref p = vm->content_providers; p;) {
      union dvm_value info = { 0 }, names = { 0 }, next = { 0 };
      (void)dvm_get_field(vm, p, "info", "Landroid/content/pm/ProviderInfo;", &info);
      (void)dvm_get_field(vm, info.l, "authority", "Ljava/lang/String;", &names);
      const char *entry = dvm_string_utf8(vm, names.l);
      while (entry && *entry) {
         const char *end = strchr(entry, ';');
         const size_t len = end ? (size_t)(end - entry) : strlen(entry);
         if (len == wanted_len && !memcmp(entry, wanted, len)) return p;
         entry = end ? end + 1 : NULL;
      }
      (void)dvm_get_field(vm, p, "resolverNext", "Landroid/content/ContentProvider;", &next);
      p = next.l;
   }
   return 0;
}

/* call(uri, method, arg, extras): the provider's own call(), the way the
 * framework reaches one for an authority the uri names. */
static bool x_resolver_call(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self; (void)nargs;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "uri"); return false; }
   if (!ARG(1).l) { dvm__throw(vm, "java/lang/NullPointerException", "method"); return false; }
   union dvm_value authority = { 0 };
   if (!am_vcall(vm, ARG(0).l, "getAuthority", "()Ljava/lang/String;", NULL, 0, &authority)) return false;
   const char *a = authority.l ? dvm_string_utf8(vm, authority.l) : NULL;
   dvm_ref provider = a ? x_find_provider(vm, a) : 0;
   if (!provider) {
      /* No such provider: acquireProvider() comes back empty. */
      char text[256];
      union dvm_value s = { 0 };
      (void)am_vcall(vm, ARG(0).l, "toString", "()Ljava/lang/String;", NULL, 0, &s);
      snprintf(text, sizeof text, "Unknown URI %s", s.l ? dvm_string_utf8(vm, s.l) : "?");
      dvm__throw(vm, "java/lang/IllegalArgumentException", "%s", text);
      return false;
   }
   return am_vcall(vm, provider, "call", "(Ljava/lang/String;Ljava/lang/String;Landroid/os/Bundle;)Landroid/os/Bundle;",
                   &args[1], 3, out);
}

static const struct rt_method rt_x_content_resolver[] = {
   M("call", "(Landroid/net/Uri;Ljava/lang/String;Ljava/lang/String;Landroid/os/Bundle;)Landroid/os/Bundle;", x_resolver_call),
   M_END,
};

/* ------------------------------------------------------------------ WebKit *
 *
 * WebSettings keeps every setting the app gives it so the getters answer what
 * was set (and the framework's default when nothing was).  The engine reads
 * the few it acts on (JavaScript, user agent) from their own fields. */

static const struct {
   const char *key; char kind;                /* Z bool, I int, J long, S string */
   int64_t ival; const char *sval;
} x_ws_settings[] = {
   { "AllowContentAccess", 'Z', 1, NULL }, { "AllowFileAccess", 'Z', 1, NULL },
   { "AllowFileAccessFromFileURLs", 'Z', 0, NULL }, { "AllowUniversalAccessFromFileURLs", 'Z', 0, NULL },
   { "AppCacheEnabled", 'Z', 0, NULL }, { "AppCacheMaxSize", 'J', 0, NULL }, { "AppCachePath", 'S', 0, "" },
   { "BlockNetworkImage", 'Z', 0, NULL }, { "BlockNetworkLoads", 'Z', 0, NULL },
   { "BuiltInZoomControls", 'Z', 0, NULL }, { "CacheMode", 'I', -1, NULL },
   { "DatabaseEnabled", 'Z', 0, NULL }, { "DatabasePath", 'S', 0, "" },
   { "DefaultFixedFontSize", 'I', 13, NULL }, { "DefaultFontSize", 'I', 16, NULL },
   { "DefaultTextEncodingName", 'S', 0, "UTF-8" }, { "DisplayZoomControls", 'Z', 1, NULL },
   { "DomStorageEnabled", 'Z', 0, NULL }, { "EnableSmoothTransition", 'Z', 0, NULL },
   { "ForceDark", 'I', 1, NULL }, { "GeolocationDatabasePath", 'S', 0, "" },
   { "GeolocationEnabled", 'Z', 1, NULL }, { "JavaScriptCanOpenWindowsAutomatically", 'Z', 0, NULL },
   { "LightTouchEnabled", 'Z', 0, NULL }, { "LoadWithOverviewMode", 'Z', 0, NULL },
   { "LoadsImagesAutomatically", 'Z', 1, NULL }, { "MediaPlaybackRequiresUserGesture", 'Z', 1, NULL },
   { "MinimumFontSize", 'I', 8, NULL }, { "MinimumLogicalFontSize", 'I', 8, NULL },
   { "MixedContentMode", 'I', 1, NULL }, { "NeedInitialFocus", 'Z', 1, NULL },
   { "OffscreenPreRaster", 'Z', 0, NULL }, { "SafeBrowsingEnabled", 'Z', 1, NULL },
   { "SaveFormData", 'Z', 1, NULL }, { "SavePassword", 'Z', 1, NULL },
   { "StandardFontFamily", 'S', 0, "sans-serif" }, { "FixedFontFamily", 'S', 0, "monospace" },
   { "SansSerifFontFamily", 'S', 0, "sans-serif" }, { "SerifFontFamily", 'S', 0, "serif" },
   { "CursiveFontFamily", 'S', 0, "cursive" }, { "FantasyFontFamily", 'S', 0, "fantasy" },
   { "SupportMultipleWindows", 'Z', 0, NULL }, { "SupportZoom", 'Z', 1, NULL },
   { "TextZoom", 'I', 100, NULL }, { "UseWideViewPort", 'Z', 0, NULL },
   { "DomStorage", 'Z', 0, NULL },
};

static bool x_ws_setting(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   const bool set = !strncmp(name, "set", 3);
   const char *key = name + (set || !strncmp(name, "get", 3) ? 3 : 2);
   size_t index = SIZE_MAX;
   for (size_t i = 0; i < sizeof x_ws_settings / sizeof x_ws_settings[0]; ++i)
      if (!strcmp(x_ws_settings[i].key, key)) { index = i; break; }
   if (index == SIZE_MAX) { dvm__throw(vm, "java/lang/NoSuchMethodError", "WebSettings.%s", name); return false; }
   union dvm_value map = { 0 };
   (void)dvm_get_field(vm, self, "props", "Ljava/util/Map;", &map);
   if (!map.l) {
      map.l = coll_new_map(vm);
      if (!map.l) return false;
      (void)dvm_set_field(vm, self, "props", "Ljava/util/Map;", map);
   }
   union dvm_value k = { .l = dvm_new_string(vm, key) };
   if (!k.l) return false;
   if (set) {
      union dvm_value kv[2] = { k, { 0 } }, ignored = { 0 };
      switch (x_ws_settings[index].kind) {
      case 'Z': kv[1].l = box_make(vm, "Ljava/lang/Boolean;", (union dvm_value){ .i = ARG(0).i != 0 }); break;
      case 'I': kv[1].l = box_make(vm, "Ljava/lang/Integer;", (union dvm_value){ .i = ARG(0).i }); break;
      case 'J': kv[1].l = box_make(vm, "Ljava/lang/Long;", (union dvm_value){ .j = ARG(0).j }); break;
      default: kv[1].l = ARG(0).l; break;
      }
      if (!kv[1].l && x_ws_settings[index].kind != 'S') return false;
      if (!map_put(vm, map.l, kv, 2, &ignored)) return false;
      RETV();
   }
   union dvm_value got = { 0 };
   if (!map_get(vm, map.l, &k, 1, &got)) return false;
   switch (x_ws_settings[index].kind) {
   case 'Z': RETI(got.l ? box_get(vm, got.l).i != 0 : x_ws_settings[index].ival != 0);
   case 'I': RETI(got.l ? box_get(vm, got.l).i : (int32_t)x_ws_settings[index].ival);
   case 'J': RETJ(got.l ? box_get(vm, got.l).j : x_ws_settings[index].ival);
   default: RETL(got.l ? got.l : dvm_new_string(vm, x_ws_settings[index].sval ? x_ws_settings[index].sval : ""));
   }
}

#define WS_Z(n) M("set" n, "(Z)V", x_ws_setting), M("get" n, "()Z", x_ws_setting)
#define WS_I(n) M("set" n, "(I)V", x_ws_setting), M("get" n, "()I", x_ws_setting)
#define WS_S(n) M("set" n, "(Ljava/lang/String;)V", x_ws_setting), M("get" n, "()Ljava/lang/String;", x_ws_setting)
static const struct rt_method rt_x_web_settings[] = {
   WS_Z("AllowContentAccess"), WS_Z("AllowFileAccess"), WS_Z("AllowFileAccessFromFileURLs"),
   WS_Z("AllowUniversalAccessFromFileURLs"), WS_Z("AppCacheEnabled"),
   M("setAppCacheMaxSize", "(J)V", x_ws_setting), WS_S("AppCachePath"),
   WS_Z("BlockNetworkImage"), WS_Z("BlockNetworkLoads"), WS_Z("BuiltInZoomControls"),
   WS_I("CacheMode"), WS_Z("DatabaseEnabled"), WS_S("DatabasePath"),
   WS_I("DefaultFixedFontSize"), WS_I("DefaultFontSize"), WS_S("DefaultTextEncodingName"),
   WS_Z("DisplayZoomControls"), WS_Z("DomStorageEnabled"), M("setEnableSmoothTransition", "(Z)V", x_ws_setting),
   WS_I("ForceDark"), M("setGeolocationDatabasePath", "(Ljava/lang/String;)V", x_ws_setting),
   M("setGeolocationEnabled", "(Z)V", x_ws_setting),
   WS_Z("JavaScriptCanOpenWindowsAutomatically"), M("setLightTouchEnabled", "(Z)V", x_ws_setting),
   M("getLightTouchEnabled", "()Z", x_ws_setting), WS_Z("LoadWithOverviewMode"),
   WS_Z("LoadsImagesAutomatically"), WS_Z("MediaPlaybackRequiresUserGesture"),
   WS_I("MinimumFontSize"), WS_I("MinimumLogicalFontSize"), WS_I("MixedContentMode"),
   M("setNeedInitialFocus", "(Z)V", x_ws_setting), WS_Z("OffscreenPreRaster"), WS_Z("SafeBrowsingEnabled"),
   M("setSaveFormData", "(Z)V", x_ws_setting), M("getSaveFormData", "()Z", x_ws_setting),
   M("setSavePassword", "(Z)V", x_ws_setting), M("getSavePassword", "()Z", x_ws_setting),
   WS_S("StandardFontFamily"), WS_S("FixedFontFamily"), WS_S("SansSerifFontFamily"),
   WS_S("SerifFontFamily"), WS_S("CursiveFontFamily"), WS_S("FantasyFontFamily"),
   WS_Z("SupportMultipleWindows"), M("setSupportZoom", "(Z)V", x_ws_setting), M("supportZoom", "()Z", x_ws_setting),
   WS_I("TextZoom"), WS_Z("UseWideViewPort"),
   M_END,
};
#undef WS_Z
#undef WS_I
#undef WS_S

/* The WebView implementation package.  This emulator renders with its own
 * Chromium-based engine, reported at the version its user agent names. */
static bool x_webview_package(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   dvm_ref pi = dvm_new_object(vm, dvm_find_class(vm, "android/content/pm/PackageInfo"));
   if (!pi) return false;
   (void)dvm_set_field(vm, pi, "packageName", "Ljava/lang/String;", (union dvm_value){ .l = dvm_new_string(vm, "com.google.android.webview") });
   (void)dvm_set_field(vm, pi, "versionName", "Ljava/lang/String;", (union dvm_value){ .l = dvm_new_string(vm, "120.0.0.0") });
   rt_set_int(vm, pi, "versionCode", 612000000);
   RETL(pi);
}

static bool x_webview_classloader(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   return cl_getClassLoader(vm, 0, NULL, 0, out);
}

static char g_x_webview_suffix[64];
static bool g_x_webview_suffix_set;

/* setDataDirectorySuffix(): must come before any WebView is made, and a
 * suffix is a single path component. */
static bool x_webview_setDataDirectorySuffix(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                             int nargs, union dvm_value *out)
{
   (void)self; (void)nargs; (void)out;
   const char *suffix = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
   if (!suffix) { dvm__throw(vm, "java/lang/NullPointerException", "suffix"); return false; }
   if (strchr(suffix, '/') || strchr(suffix, '\\')) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Suffix %s contains a path separator", suffix);
      return false;
   }
   if (dvm_webview_count() > 0) {
      dvm__throw(vm, "java/lang/IllegalStateException",
                 "Can't set data directory suffix: WebView already initialized");
      return false;
   }
   snprintf(g_x_webview_suffix, sizeof g_x_webview_suffix, "%s", suffix);
   g_x_webview_suffix_set = true;
   RETV();
}

static const struct rt_method rt_x_webview[] = {
   SM("getCurrentWebViewPackage", "()Landroid/content/pm/PackageInfo;", x_webview_package),
   SM("getWebViewClassLoader", "()Ljava/lang/ClassLoader;", x_webview_classloader),
   SM("setDataDirectorySuffix", "(Ljava/lang/String;)V", x_webview_setDataDirectorySuffix),
   M("pauseTimers", "()V", empty_void),
   M("resumeTimers", "()V", empty_void),
   M_END,
};

#endif
